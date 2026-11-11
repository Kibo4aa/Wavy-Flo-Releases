#include "gpu_renderer.h"
#include "route_choice.h"
#include "gpu_pixels.h"
#include "render_workers.h"
#include "expressions.h"
#include "particles.h"
#include "effect_packs.h"
#include "lights.h"
#include <QCache>
#include <QElapsedTimer>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <utility>
#include <QCoreApplication>
#include <QThread>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLPaintDevice>
#include <QOpenGLShaderProgram>
#include <QPainterPath>
#include <QVector2D>
#include <unordered_map>

namespace {
// QPainter may reactivate its target after a native effect pass. Keep that
// target explicit; the context's default framebuffer is not our preview FBO.
//
// One that paints a layer alone -- a blend, a matte, a motion-blur sample, a lit
// face -- is kept with its target, not made for each picture. A device begun
// while the frame's own painter is open gets a paint engine of its own, and a
// device made afresh each time built that engine, its vertex array and its
// buffers again and threw them away: 50 to 90 us a picture, about 1 ms of a
// frame of the Twitter promo, whose two turning rings take sixteen samples. On
// a kept device a painter begins in 3 us. It is still begun anew for each
// picture, so each one starts from the same state as before.
class CompositionPaintDevice final : public QOpenGLPaintDevice {
public:
  explicit CompositionPaintDevice(QOpenGLFramebufferObject &target)
      : QOpenGLPaintDevice(target.size()), target(target) {}
  void ensureActiveTarget() override { target.bind(); }
  GLuint framebuffer() const { return target.handle(); }
private:
  QOpenGLFramebufferObject &target;
};

// What a renderer asks of the card; its surfaces are made to match.
QSurfaceFormat rendererFormat() {
  QSurfaceFormat format;
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setDepthBufferSize(24);
  format.setStencilBufferSize(8);
  return format;
}
bool onInterfaceThread() {
  return QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread();
}

// What Qt's raster engine does to a shape before it covers a pixel (Qt 6.8),
// for the card to cover shapes the same way (State::shapes).
//
// The scale QOutlineMapper flattens curves for (qt_scaleForTransform): one for
// a shift, the larger axis of a plain scale, else the root of the larger
// squared column or row, whichever pair differs more.
qreal outlineScale(const QTransform &t) {
  const QTransform::TransformationType type = t.type();
  if (type <= QTransform::TxTranslate) return 1;
  if (type == QTransform::TxScale) return qMax(qAbs(t.m11()), qAbs(t.m22()));
  const qreal xScale1 = t.m11() * t.m11() + t.m21() * t.m21();
  const qreal yScale1 = t.m12() * t.m12() + t.m22() * t.m22();
  const qreal xScale2 = t.m11() * t.m11() + t.m12() * t.m12();
  const qreal yScale2 = t.m21() * t.m21() + t.m22() * t.m22();
  if (qAbs(xScale1 - yScale1) > qAbs(xScale2 - yScale2)) return qSqrt(qMax(xScale1, yScale1));
  return qSqrt(qMax(xScale2, yScale2));
}
// A curve cut into lines as QOutlineMapper::curveTo cuts it (QBezier::
// addToPolygon, line for line): halved until its control points lie within
// `threshold` of its chord, at most nine times, the end of each piece added.
void flattenCurve(std::vector<QPointF> &points, QPointF a, QPointF b, QPointF c, QPointF d, qreal threshold) {
  struct Bezier { qreal x1, y1, x2, y2, x3, y3, x4, y4; };
  // QBezier::split; `second` may be the curve split.
  auto split = [](const Bezier w, Bezier *first, Bezier *second) {
    qreal m = (w.x2 + w.x3) * .5;
    first->x2 = (w.x1 + w.x2) * .5;
    second->x3 = (w.x3 + w.x4) * .5;
    first->x1 = w.x1;
    second->x4 = w.x4;
    first->x3 = (first->x2 + m) * .5;
    second->x2 = (second->x3 + m) * .5;
    first->x4 = second->x1 = (first->x3 + second->x2) * .5;
    m = (w.y2 + w.y3) / 2;
    first->y2 = (w.y1 + w.y2) * .5;
    second->y3 = (w.y3 + w.y4) * .5;
    first->y1 = w.y1;
    second->y4 = w.y4;
    first->y3 = (first->y2 + m) * .5;
    second->y2 = (second->y3 + m) * .5;
    first->y4 = second->y1 = (first->y3 + second->y2) * .5;
  };
  Bezier beziers[10];
  int levels[10];
  beziers[0] = {a.x(), a.y(), b.x(), b.y(), c.x(), c.y(), d.x(), d.y()};
  levels[0] = 9;
  int top = 0;
  while (top >= 0) {
    Bezier *z = &beziers[top];
    const qreal y4y1 = z->y4 - z->y1, x4x1 = z->x4 - z->x1;
    qreal l = qAbs(x4x1) + qAbs(y4y1), distance;
    if (l > 1.) {
      distance = qAbs((x4x1) * (z->y1 - z->y2) - (y4y1) * (z->x1 - z->x2)) +
                 qAbs((x4x1) * (z->y1 - z->y3) - (y4y1) * (z->x1 - z->x3));
    } else {
      distance = qAbs(z->x1 - z->x2) + qAbs(z->y1 - z->y2) + qAbs(z->x1 - z->x3) + qAbs(z->y1 - z->y3);
      l = 1.;
    }
    if (distance < threshold * l || levels[top] == 0) {
      points.push_back(QPointF(z->x4, z->y4));
      --top;
    } else {
      split(*z, z + 1, z);
      levels[top + 1] = --levels[top];
      ++top;
    }
  }
}
// The colour the raster engine fills a span with (QSpanData::setup): the
// brush colour's alpha times the painter's opacity in 256ths, premultiplied
// at sixteen bits a channel, then narrowed to eight. 0..255 a channel.
QVector4D rasterColour(const QColor &colour, qreal opacity) {
  const QRgba64 rgba = colour.rgba64();
  const quint16 alpha = quint16((uint(rgba.alpha()) * uint(qRound(opacity * 256))) >> 8);
  const QRgb argb = qPremultiply(QRgba64::fromRgba64(rgba.red(), rgba.green(), rgba.blue(), alpha)).toArgb32();
  return QVector4D(qRed(argb), qGreen(argb), qBlue(argb), qAlpha(argb));
}
// A gradient as the raster engine looks its colours up (QGradientCache):
// 1024 steps across its stops, mixed premultiplied, the painter's opacity in
// them, each step's colour narrowed as rasterColour narrows one.
QImage gradientTable(const QGradient &gradient, qreal opacity) {
  QImage table(1024, 1, QImage::Format_RGBA8888_Premultiplied);
  const QGradientStops stops = gradient.stops();
  auto at = [&](double position) {
    if (stops.isEmpty()) return QColor(Qt::transparent);
    if (position <= stops.first().first) return stops.first().second;
    if (position >= stops.last().first) return stops.last().second;
    for (int i = 1; i < stops.size(); ++i) {
      if (position > stops[i].first) continue;
      const double span = stops[i].first - stops[i - 1].first;
      const double f = span > 0 ? (position - stops[i - 1].first) / span : 1;
      const QColor a = stops[i - 1].second, b = stops[i].second;
      const double alphaA = a.alphaF(), alphaB = b.alphaF(), alpha = alphaA + (alphaB - alphaA) * f;
      auto mix = [&](double x, double y) {
        const double premultiplied = x * alphaA + (y * alphaB - x * alphaA) * f;
        return alpha > 0 ? premultiplied / alpha : 0.;
      };
      return QColor::fromRgbF(float(mix(a.redF(), b.redF())), float(mix(a.greenF(), b.greenF())),
                              float(mix(a.blueF(), b.blueF())), float(alpha));
    }
    return stops.last().second;
  };
  uchar *row = table.scanLine(0);
  for (int i = 0; i < 1024; ++i) {
    const QVector4D c = rasterColour(at(i / 1023.), opacity);
    row[i * 4] = uchar(c.x()); row[i * 4 + 1] = uchar(c.y()); row[i * 4 + 2] = uchar(c.z()); row[i * 4 + 3] = uchar(c.w());
  }
  return table;
}

// On Windows an offscreen surface is a hidden window, and Qt keeps its windows
// in tables the interface thread reads with every message. A renderer that made
// its own on the thread it draws on raced the interface's windows -- a dialog
// opening, the "closed unexpectedly" notice -- and crashed inside qwindows.dll,
// with Qt's "Expect failures" warning logged just before (six crash reports,
// Oct 13-14). So surfaces are made and destroyed on the interface thread only,
// and lent to the threads that draw. A context may be made where it draws, as
// Qt Quick's render thread makes its own.
class SurfacePool {
public:
  // One to draw with. On another thread than the interface's, null when none
  // is waiting: more are asked for, and that frame goes to the CPU.
  QOffscreenSurface *take() {
    {
      std::lock_guard<std::mutex> lock(guard);
      if (!waiting.empty()) {
        QOffscreenSurface *surface = waiting.back();
        waiting.pop_back();
        return surface;
      }
    }
    if (onInterfaceThread()) return make();
    askForMore();
    return nullptr;
  }
  // Back for the next renderer, from any thread. Destroyed only at exit.
  void give(QOffscreenSurface *surface) {
    if (!surface) return;
    std::lock_guard<std::mutex> lock(guard);
    waiting.push_back(surface);
  }
  // `count` waiting, for renderers about to draw on other threads.
  void reserve(int count) {
    if (!onInterfaceThread()) {
      for (int i = 0; i < count; ++i) askForMore();
      return;
    }
    while (waitingCount() < count) {
      QOffscreenSurface *surface = make();
      if (!surface) return;
      give(surface);
    }
  }
  int waitingCount() {
    std::lock_guard<std::mutex> lock(guard);
    return int(waiting.size());
  }
  std::vector<QOffscreenSurface *> takeAllWaiting() {
    std::lock_guard<std::mutex> lock(guard);
    return std::exchange(waiting, {});
  }
private:
  // Interface thread only.
  QOffscreenSurface *make() {
    static const bool cleanUpAtExit = [] { qAddPostRoutine(cleanUp); return true; }();
    Q_UNUSED(cleanUpAtExit);
    auto *surface = new QOffscreenSurface;
    surface->setFormat(rendererFormat());
    surface->create();
    if (surface->isValid()) return surface;
    delete surface;
    return nullptr;
  }
  // Every thread that found none adds one to the order, and one request
  // carries them all to the interface thread.
  void askForMore() {
    if (!QCoreApplication::instance() || wanted.fetch_add(1) > 0) return;
    QMetaObject::invokeMethod(QCoreApplication::instance(), [this] {
      for (int count = wanted.exchange(0); count > 0; --count) give(make());
    }, Qt::QueuedConnection);
  }
  // Run by the application's destructor, on the interface thread, while
  // windows can still be destroyed. Surfaces still lent out are left to Windows.
  static void cleanUp();
  std::mutex guard;
  std::vector<QOffscreenSurface *> waiting;
  std::atomic<int> wanted{0};
};
SurfacePool &surfacePool() {
  static SurfacePool pool;
  return pool;
}
void SurfacePool::cleanUp() {
  for (QOffscreenSurface *surface : surfacePool().takeAllWaiting()) delete surface;
}
}

struct GpuRenderer::State {
  QOffscreenSurface *surface = nullptr; // lent by surfacePool()
  QOpenGLContext context;
  // Where each program's uniforms are (the ...At beside it) is asked of the
  // driver once, when the program links, and every uniform is set by where it
  // is. Set by name, QOpenGLShaderProgram asks the driver again every time
  // (glGetUniformLocation): the blend of each motion-blur sample set eight that
  // way, 0.8 ms of a Twitter promo frame spent asking; set by place, 0.02.
  std::unique_ptr<QOpenGLShaderProgram> shader;
  struct { GLint source = -1, weight = -1; } shaderAt;
  std::unique_ptr<QOpenGLFramebufferObject> sample, sampleResolved, accumulation, resolved;
  GLuint vao = 0;
  bool attempted = false, ready = false;
  QString detail = "GPU has not been initialized";
  // A shape's drawing kept two ways, both made on the workers: the recorded
  // picture, which Qt's GL engine plays wherever the card cannot cover the
  // shape itself (a mask cuts it, a 3D projection carries it), and what it
  // fills, which the card covers by exact area (shapes()).
  struct Drawing { QPicture picture; std::shared_ptr<const ShapeFills> fills; };
  QCache<QByteArray, Drawing> drawings{65536}; // KiB, bounded to 64 MiB.
  // The fills of the layers the sample being drawn has made ready, by id.
  QHash<QString, std::shared_ptr<const ShapeFills>> preparedFills;
  QHash<QString, QPicture> prepare(const Scene &scene, double sampleTime, double frame) {
    struct Pending { QStringList ids; QByteArray key; std::future<Drawing> job; };
    preparedFills.clear();
    std::vector<Pending> jobs;
    QHash<QByteArray, size_t> pendingKeys;
    QHash<QString, QPicture> result;
    // Which layers are drawn ahead, and at what time.
    struct Candidate { const Layer *layer; double time; };
    std::vector<Candidate> candidates;
    for (const auto &layer : scene.layers) {
      double time = (layer.motionBlur ? sampleTime : frame) - layer.start;
      time = posterizedTime(layer, scene, time);
      // Text layout/font engines are thread-affine. Do not replay worker
      // QPicture text commands on the GL thread, including rich/bitmap text.
      if (!layer.visible || time < 0 || time >= layer.duration ||
          layer.type != "shape" && !parametricShape(layer)) continue;
      candidates.push_back({&layer, time});
    }
    // What identifies each drawing, worked out on every core: a scene of six
    // hundred shapes spent nine milliseconds of every frame here, one after
    // another on this thread, even when every drawing was already kept.
    std::vector<QByteArray> keys(candidates.size());
    auto keyFor = [](const Layer &layer, double time) {
      QByteArray content;
      QDataStream stream(&content, QIODevice::WriteOnly);
      stream << layer.type << layer.shapeKind << layer.text << layer.richHtml << layer.font << layer.fontSize
             << layer.color << layer.bold << layer.fontStyle << layer.fauxBold << layer.fauxItalic << layer.capitals << layer.leading << layer.tracking << layer.align
             << layer.paragraphWidth << layer.strokeWidth << layer.strokeColor << layer.strokeMode
             << layer.shapeStrokeCap;
      stream << qint32(layer.textStrokes.size());
      for (const auto &stroke : layer.textStrokes)
        stream << stroke.width << stroke.color << stroke.mode << stroke.join;
      stream << layer.hiddenEffects.value("Text-Animation").toBool();
      stream << vectorEffectStroke(layer);
      if(vectorEffectStroke(layer)) stream << QJsonDocument(layer.surfaceEffects["Stroke"].toObject()).toJson(QJsonDocument::Compact);
      stream << vectorEffectGradient(layer);
      if(vectorEffectGradient(layer)) stream << QJsonDocument(layer.surfaceEffects["Gradient"].toObject()).toJson(QJsonDocument::Compact);
      // Fill folds into the drawing too, and which effects fold at all depends
      // on the order, so both belong in what identifies a cached drawing.
      stream << vectorEffectFill(layer);
      if(vectorEffectFill(layer)) stream << QJsonDocument(layer.surfaceEffects["Fill"].toObject()).toJson(QJsonDocument::Compact);
      stream << QJsonDocument(QJsonObject{{"order", layer.effectOrder}}).toJson(QJsonDocument::Compact);
      stream << layer.shapeFill << layer.shapeClosed;
      // A shape drawn from its own points rather than from a generator.
      stream << QJsonDocument(layer.shapePoints).toJson(QJsonDocument::Compact);
      if (parametricShape(layer))
        for (int p = ShapeWidth; p < PropertyCount; ++p) stream << layer.p[p].at(time);
      else if (layer.type == "shape")
        for (int p = ShapeTrimStart; p < PropertyCount; ++p) stream << layer.p[p].at(time);
      for (int p = WipeLinearCompletion; p < PropertyCount; ++p) stream << layer.p[p].at(time);
      stream << QJsonDocument(layer.surfaceEffects["Radial Wipe"].toObject()).toJson(QJsonDocument::Compact);
      if (layer.characterAnimation.value("enabled").toBool()) {
        stream << time << QJsonDocument(layer.characterAnimation).toJson(QJsonDocument::Compact);
        for (int i = 0; i < layer.extraCurveIds.size() && i < int(layer.extraCurves.size()); ++i)
          if (layer.extraCurveIds[i].startsWith("TextAnim/")) {
            // The whole curve, not its value now: each letter reads it at its
            // own delay, so past the last key the letters still move.
            const Property &curve = layer.extraCurves[size_t(i)];
            stream << layer.extraCurveIds[i] << curve.base;
            for (const auto &key : curve.keys) {
              stream << key.frame << key.value << key.ease << key.custom << key.absolute << key.continuous;
              for (double value : key.curve) stream << value;
            }
            if (curve.hasExpression()) stream << curve.expression;
          }
        for (int index : {6, 7}) {
          stream << layer.p[index].base;
          for (const auto &key : layer.p[index].keys) {
            stream << key.frame << key.value << key.ease << key.custom << key.absolute << key.continuous;
            for (double value : key.curve) stream << value;
          }
          if (layer.p[index].hasExpression()) stream << layer.p[index].expression;
        }
      }
      return QCryptographicHash::hash(content, QCryptographicHash::Sha256);
    };
    renderParallel(int(candidates.size()), [&](int from, int to) {
      ExpressionSceneScope expressions(&scene);
      for (int i = from; i < to; ++i) keys[size_t(i)] = keyFor(*candidates[size_t(i)].layer, candidates[size_t(i)].time);
    });
    for (size_t i = 0; i < candidates.size(); ++i) {
      const Layer &layer = *candidates[i].layer;
      const double time = candidates[i].time;
      const QByteArray &key = keys[i];
      if (auto *cached = drawings.object(key)) {
        result.insert(layer.id, cached->picture);
        preparedFills.insert(layer.id, cached->fills);
      } else if (auto pending = pendingKeys.constFind(key); pending != pendingKeys.cend())
        jobs[*pending].ids.append(layer.id);
      else {
        pendingKeys.insert(key, jobs.size());
        jobs.push_back({{layer.id}, key, renderJob([layer, &scene, time] {
          return Drawing{prepareLayerDrawing(layer, scene, time),
                         std::make_shared<const ShapeFills>(shapeLayerFills(layer, scene, time))};
        })});
      }
    }
    // Join every job before propagating a failure: tasks refer to this scene.
    std::exception_ptr failure;
    for (auto &pending : jobs) {
      try {
        Drawing drawing = pending.job.get();
        for (const auto &id : pending.ids) {
          result.insert(id, drawing.picture);
          preparedFills.insert(id, drawing.fills);
        }
        int cost = std::max(1, int((drawing.picture.size() + 1023) / 1024));
        drawings.insert(pending.key, new Drawing(std::move(drawing)), cost);
      } catch (...) { failure = std::current_exception(); }
    }
    if (failure) std::rethrow_exception(failure);
    return result;
  }
  bool initialize() {
    if (attempted) return ready;
    // The window it draws with first, made on the interface thread. None ready
    // yet: this frame goes to the CPU and the next one asks again, rather than
    // giving the card up for good.
    if (!surface && !(surface = surfacePool().take())) {
      detail = "GPU surface not ready yet; using CPU";
      return false;
    }
    attempted = true;
    context.setFormat(rendererFormat());
    if (!context.create()) { detail = "OpenGL context unavailable"; return false; }
    if (!surface->isValid() || !context.makeCurrent(surface)) {
      detail = "GPU offscreen surface unavailable"; return false;
    }
    auto *gl = context.extraFunctions();
    gl->initializeOpenGLFunctions();
    detail = QString::fromLatin1(reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER)));
    if (detail.contains("llvmpipe", Qt::CaseInsensitive) ||
        detail.contains("software", Qt::CaseInsensitive) || detail.contains("GDI Generic")) {
      detail = "Software OpenGL detected; using CPU"; context.doneCurrent(); return false;
    }
    shader = std::make_unique<QOpenGLShaderProgram>();
    const char *vertex = R"(
      #version 330 core
      out vec2 uv;
      void main() {
        vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
        uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
      })";
    const char *fragment = R"(
      #version 330 core
      uniform sampler2D source;
      uniform float weight;
      in vec2 uv;
      out vec4 color;
      void main() { color = texture(source, uv) * weight; })";
    if (!shader->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) ||
        !shader->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) || !shader->link()) {
      detail = "GPU shader unavailable: " + shader->log();
      context.doneCurrent(); return false;
    }
    shaderAt = {shader->uniformLocation("source"), shader->uniformLocation("weight")};
    gl->glGenVertexArrays(1, &vao);
    gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    gl->glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    ready = true;
    context.doneCurrent();
    return true;
  }
  // The context's limits, asked for once: every question put to the driver
  // waits for it, as the errors below do.
  GLint maxTextureSize = 0, maxSamples = 0;
  // GL errors, and what asking for them costs. Under NVIDIA's threaded driver
  // every glGetError waits until the driver's own thread has worked through
  // all that was queued before it. Each step below -- a picture, a blend, an
  // effected raster, particles, a lit face, an Effect Layer -- once cleared
  // the errors left before it and asked again after it: the 27 steps of a
  // Twitter promo frame waited 1.5 of its 5.2 ms there, and its 21 pictures
  // took 1.22 ms where they now take 0.06. Now a card that has made no error
  // is asked once a frame, at its end (render()), and a frame with an error is
  // handed back for the CPU to draw. With the bindings read off the device too
  // (drawTarget), the promo frame takes 4.1 ms and a Kotonoha MV frame 3.9
  // where it took 5.3, every byte of both pictures as before. Only a step
  // whose result outlives the frame -- a texture kept, a picture read back, a
  // shadow atlas -- still asks after its own work, having first set aside
  // what came before it. After a frame with an error each step checks its own
  // again, as before, for the rest of this renderer's life: one that fails
  // falls back alone, and an error none of them owns gives the card up.
  bool checkEachStep = false;
  GLenum frameError = GL_NO_ERROR; // found by a step that checks its own, made by another
  // What came before a step, cleared, while each step checks its own.
  void clearBeforeStep(QOpenGLExtraFunctions *gl) {
    if (!checkEachStep) return;
    for (int attempt = 0; attempt < 16 && gl->glGetError() != GL_NO_ERROR; ++attempt) {}
  }
  // Whether a step made an error, while each step checks its own; until then
  // the frame's check answers for it.
  bool stepWithoutError(QOpenGLExtraFunctions *gl) {
    if (!checkEachStep) return true;
    const GLenum error = gl->glGetError();
    return error == GL_NO_ERROR;
  }
  // For a step whose result is kept: whether the card has made an error since
  // it was last asked. One found while steps do not check their own may be an
  // earlier step's, so it is the frame's as well.
  bool noErrorSince(QOpenGLExtraFunctions *gl) {
    const GLenum error = gl->glGetError();
    if (error == GL_NO_ERROR) return true;
    if (!checkEachStep && frameError == GL_NO_ERROR) frameError = error;
    return false;
  }
  // Before a step that checks its own work: what came before set aside -- the
  // frame's, while steps do not check theirs -- so the step does not take it
  // for its own.
  void setErrorsAside(QOpenGLExtraFunctions *gl) {
    for (int attempt = 0; attempt < 16 && !noErrorSince(gl); ++attempt) {}
  }
  // Where a painter on the card draws once it has handed the card over
  // (beginNativePainting): Qt binds its device's framebuffer then, all of it
  // the viewport -- what picture() draws into without asking. Read off the
  // device, not asked of the driver: a glGetIntegerv waits for it as a
  // glGetError does, and with the error checks gone the 8 a Twitter promo
  // frame made took 1.35 ms. A device made elsewhere is asked about, as before.
  struct DrawTarget { GLint fbo = 0; GLint viewport[4] = {}; };
  DrawTarget drawTarget(QPaintDevice *device) {
    DrawTarget target;
    if (auto *ours = dynamic_cast<CompositionPaintDevice *>(device)) {
      target.fbo = GLint(ours->framebuffer());
      target.viewport[2] = ours->width();
      target.viewport[3] = ours->height();
      return target;
    }
    auto *gl = context.extraFunctions();
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target.fbo);
    gl->glGetIntegerv(GL_VIEWPORT, target.viewport);
    return target;
  }
  bool buffers(QSize size) {
    if (sample && sample->size() == size) return true;
    if (size.width() > maxTextureSize || size.height() > maxTextureSize ||
        qint64(size.width()) * size.height() > 33000000)
      return false;
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    format.setInternalTextureFormat(GL_RGBA8);
    // The edges of what the GL painter draws itself -- shapes, fills, masks --
    // are only ever smoothed by multisampling: Qt's GL paint engine has no
    // other antialiasing for a filled path. With a single sample every edge
    // came out stepped, every pixel wholly in or wholly out. The blit after
    // painting resolves the samples at the same size. Pictures that arrive as
    // finished rasters (text, anything with an effect) are not touched by it.
    // Eight samples: measured against the CPU over 176 frames, four halved the
    // difference the stepped edges made and eight took it lower still, at no
    // cost the card could show. A driver offering fewer gets what it has.
    const int wanted = std::clamp(8, 0, std::max(0, int(maxSamples)));
    format.setSamples(wanted);
    sample = std::make_unique<QOpenGLFramebufferObject>(size, format);
    if (!sample->isValid() && wanted > 0) {
      // A driver that will not multisample still gets a picture, stepped.
      format.setSamples(0);
      sample = std::make_unique<QOpenGLFramebufferObject>(size, format);
    }
    format.setSamples(0);
    format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    sampleResolved = std::make_unique<QOpenGLFramebufferObject>(size, format);
    resolved = std::make_unique<QOpenGLFramebufferObject>(size, format);
    format.setInternalTextureFormat(GL_RGBA16F);
    accumulation = std::make_unique<QOpenGLFramebufferObject>(size, format);
    if (sample->isValid() &&
        sampleResolved->isValid() && accumulation->isValid() && resolved->isValid()) return true;
    sample.reset(); sampleResolved.reset(); accumulation.reset(); resolved.reset();
    return false;
  }
  // `area`, in the target's pixels from the top, clears only that much of it.
  void clear(QOpenGLFramebufferObject &target, const QRect *area = nullptr) {
    target.bind();
    auto *gl = context.extraFunctions();
    gl->glViewport(0, 0, target.width(), target.height());
    gl->glDisable(GL_SCISSOR_TEST);
    if (area) {
      const QRect cut = area->intersected(QRect(QPoint(), target.size()));
      gl->glEnable(GL_SCISSOR_TEST);
      gl->glScissor(cut.left(), target.height() - cut.bottom() - 1, cut.width(), cut.height());
    }
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    // QPainter leaves stencil writes masked after path filling/clipping.
    // glClear honours that mask: without resetting it, old glyph triangles
    // survive into subsequent frames (including after a resolution change).
    gl->glStencilMask(0xff);
    gl->glDepthMask(GL_TRUE);
    gl->glClearStencil(0);
    gl->glClearColor(0, 0, 0, 0);
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    gl->glDisable(GL_SCISSOR_TEST);
  }
  void transfer(GLuint texture, QOpenGLFramebufferObject &target, float weight, bool add) {
    target.bind();
    auto *gl = context.extraFunctions();
    gl->glViewport(0, 0, target.width(), target.height());
    gl->glDisable(GL_DEPTH_TEST); gl->glDisable(GL_STENCIL_TEST);
    gl->glDisable(GL_SCISSOR_TEST); gl->glDisable(GL_CULL_FACE);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (add) { gl->glEnable(GL_BLEND); gl->glBlendEquation(GL_FUNC_ADD); gl->glBlendFunc(GL_ONE, GL_ONE); }
    else gl->glDisable(GL_BLEND);
    shader->bind();
    shader->setUniformValue(shaderAt.source, 0);
    shader->setUniformValue(shaderAt.weight, weight);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glBindVertexArray(vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    gl->glBindVertexArray(0);
    shader->release();
    gl->glDisable(GL_BLEND);
  }
  // The effect pipeline, in this context. The chain the CPU would run is run
  // here between textures that never leave the card, and the result is
  // composited straight into the frame -- no readback, and no second upload
  // through drawImage.
  std::unique_ptr<QOpenGLShaderProgram> effects, composite;
  // What a pass sets: the built-in chain's program reads the first six, a
  // pack's the rest (and src).
  struct PassUniforms {
    GLint aux = -1, src = -1, op = -1, params = -1, ca = -1, cb = -1, wavyOriginalTexture = -1, sourceSize = -1,
          values = -1, processingDepth = -1, wavySeconds = -1, wavyFrame = -1, wavyScale = -1, wavyPass = -1,
          wavyPasses = -1, wavyBox = -1;
    PassUniforms() = default;
    explicit PassUniforms(QOpenGLShaderProgram &p)
        : aux(p.uniformLocation("aux")), src(p.uniformLocation("src")), op(p.uniformLocation("op")),
          params(p.uniformLocation("params")), ca(p.uniformLocation("ca")), cb(p.uniformLocation("cb")),
          wavyOriginalTexture(p.uniformLocation("wavyOriginalTexture")), sourceSize(p.uniformLocation("sourceSize")),
          values(p.uniformLocation("values")), processingDepth(p.uniformLocation("processingDepth")),
          wavySeconds(p.uniformLocation("wavySeconds")), wavyFrame(p.uniformLocation("wavyFrame")),
          wavyScale(p.uniformLocation("wavyScale")), wavyPass(p.uniformLocation("wavyPass")),
          wavyPasses(p.uniformLocation("wavyPasses")), wavyBox(p.uniformLocation("wavyBox")) {}
  } effectsAt;
  struct { GLint quad = -1, source = -1, opacity = -1; } compositeAt;
  GLuint effectFbo = 0, effectTexture[4] = {};
  QSize effectSize[4];
  // Each slot a pass writes to has a framebuffer of its own, its texture
  // attached and checked once, when the texture was made; and a texture of a
  // size no slot wants now is kept, still attached, for the next chain that
  // wants that size. Before, one framebuffer had each pass's target attached
  // in turn and checked, and a slot that changed size had its storage made
  // anew: 85 microseconds a pass, the most of a glow's whole chain -- 13 ms of
  // every frame of a music video with a dozen glowing layers on screen. Only
  // where the work happens changes; every pass reads and writes the same
  // pixels as before.
  GLuint effectSlotFbo[3] = {};
  struct SpareWork { GLuint texture = 0, fbo = 0; QSize size; quint64 used = 0; };
  std::vector<SpareWork> spareWork;
  qint64 spareWorkBytes = 0;
  quint64 workClock = 0;
  bool effectsAttempted = false, effectsReady = false;

  struct ResidentRaster {
    GLuint texture = 0;
    QSize size;
    quint64 used = 0;
  };
  // Large rasters must not repeat their whole shader chain for every copy of
  // a layer. Keep completed float textures in this context, including across
  // frames. Content/ROI identify them; opacity and the final layer transform
  // still belong to each draw. This budget is separate from scratch storage.
  QHash<QByteArray, ResidentRaster> residentRasters;
  qint64 residentBytes = 0;
  quint64 residentClock = 0;
  quint64 residentHits = 0, residentBuilds = 0;
  // Layer sources this thread had rasterized when the frame being drawn
  // began, and whether the frame has already had its sources rasterized on
  // all cores.
  quint64 frameSourceBuilds = 0;
  bool sourcesPrewarmed = false;
  // 0 not yet checked, 1 blend modes match the CPU, 2 they do not.
  int blendCheck = 0;
  bool checkingBlend = false;
  // The card's memory, not a fixed figure: float textures are sixteen bytes a
  // pixel, so a single full-frame layer with a glow is tens of megabytes and a
  // fixed 128 MiB held one or two of them. Twenty such layers then evicted each
  // other in turn and every frame rebuilt every chain. Sized at a third of what
  // the driver reports free (NVIDIA / AMD extensions), else a modest default.
  qint64 residentBudget = 128ll * 1024 * 1024;
  // Entries stamped after this were claimed by the frame being drawn. They are
  // never evicted for a later layer of the same frame: the cache walked in one
  // order every frame is the worst case for LRU, so what fits stays fit.
  quint64 frameStartClock = 0;
  void sizeResidentBudget() {
    auto *gl = context.extraFunctions();
    const QByteArray extensions(reinterpret_cast<const char *>(gl->glGetString(GL_EXTENSIONS)));
    while (gl->glGetError() != GL_NO_ERROR) {}
    qint64 freeKiB = 0;
    GLint info[4] = {0, 0, 0, 0};
    if (extensions.contains("GL_NVX_gpu_memory_info")) {
      gl->glGetIntegerv(0x9049, info); // current available dedicated video memory, KiB
      freeKiB = info[0];
    } else if (extensions.contains("GL_ATI_meminfo")) {
      gl->glGetIntegerv(0x87FB, info); // free texture memory: total, largest block, aux, aux
      freeKiB = info[0];
    }
    if (gl->glGetError() != GL_NO_ERROR) freeKiB = 0;
    residentBudget = freeKiB > 0 ? std::clamp<qint64>(freeKiB * 1024 / 3, 128ll << 20, 3ll << 30)
                                 : 256ll << 20;
  }

  void retainRaster(const QByteArray &key, GLuint texture, QSize size) {
    const qint64 bytes = qint64(size.width()) * size.height() * 4 * sizeof(float);
    if (bytes <= 0 || bytes > residentBudget || residentRasters.contains(key)) return;
    int slot = -1;
    for (int index = 0; index < 3; ++index)
      if (effectTexture[index] == texture) { slot = index; break; }
    if (slot < 0) return;
    // Room may only come from entries this frame has not used. If that is not
    // enough, this raster is simply not kept: it was drawn already.
    qint64 evictable = 0;
    for (const auto &entry : residentRasters)
      if (entry.used <= frameStartClock)
        evictable += qint64(entry.size.width()) * entry.size.height() * 4 * sizeof(float);
    if (residentBytes - evictable + bytes > residentBudget) return;
    auto *gl = context.extraFunctions();
    GLuint replacement = 0;
    gl->glGenTextures(1, &replacement);
    if (!replacement) return;
    while (residentBytes + bytes > residentBudget && !residentRasters.isEmpty()) {
      auto oldest = residentRasters.end();
      for (auto it = residentRasters.begin(); it != residentRasters.end(); ++it)
        if (it->used <= frameStartClock && (oldest == residentRasters.end() || it->used < oldest->used))
          oldest = it;
      if (oldest == residentRasters.end()) break;
      gl->glDeleteTextures(1, &oldest->texture);
      residentBytes -= qint64(oldest->size.width()) * oldest->size.height() * 4 * sizeof(float);
      residentRasters.erase(oldest);
    }
    // Transfer ownership of the finished scratch texture instead of copying
    // it or downloading it. The next chain allocates only the vacated slot.
    if (effectSlotFbo[slot]) gl->glDeleteFramebuffers(1, &effectSlotFbo[slot]);
    effectSlotFbo[slot] = 0;
    effectTexture[slot] = replacement;
    effectSize[slot] = QSize();
    residentRasters.insert(key, ResidentRaster{texture, size, ++residentClock});
    residentBytes += bytes;
  }

  bool prepareEffects() {
    if (effectsAttempted) return effectsReady;
    effectsAttempted = true;
    if (!ready) return false;
    auto *gl = context.extraFunctions();
    effects = std::make_unique<QOpenGLShaderProgram>();
    if (!effects->addShaderFromSourceCode(QOpenGLShader::Vertex, effectVertexShader()) ||
        !effects->addShaderFromSourceCode(QOpenGLShader::Fragment, effectFragmentShader()) ||
        !effects->link()) { effects.reset(); return false; }
    const char *quadVertex = R"(#version 330 core
      uniform mat4 quad;
      out vec2 uv;
      void main() {
        const vec2 corner[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),
                                       vec2(1,0),vec2(1,1),vec2(0,1));
        vec2 p = corner[gl_VertexID];
        uv = p;
        gl_Position = quad * vec4(p, 0.0, 1.0);
      })";
    const char *quadFragment = R"(#version 330 core
      uniform sampler2D source;
      uniform float opacity;
      in vec2 uv;
      out vec4 color;
      void main() { color = texture(source, uv) * opacity; })";
    composite = std::make_unique<QOpenGLShaderProgram>();
    if (!composite->addShaderFromSourceCode(QOpenGLShader::Vertex, quadVertex) ||
        !composite->addShaderFromSourceCode(QOpenGLShader::Fragment, quadFragment) ||
        !composite->link()) { effects.reset(); composite.reset(); return false; }
    effectsAt = PassUniforms(*effects);
    compositeAt = {composite->uniformLocation("quad"), composite->uniformLocation("source"),
                   composite->uniformLocation("opacity")};
    gl->glGenFramebuffers(1, &effectFbo);
    gl->glGenTextures(4, effectTexture);
    sizeResidentBudget(); effectsReady = true;
    return true;
  }

  // One numbering throughout: slot i is texture unit i. Slot 0 is the raster
  // that came up; slots 1 to 3 are the ones passes write to. Mixing the two
  // numberings is what let a sampler end up pointed at the texture the pass was
  // in the middle of writing.
  static qint64 workBytes(QSize size) { return qint64(size.width()) * size.height() * 16; }
  // Slot `slot` made `size`: from a kept texture of that size if there is one,
  // else a new one. Its framebuffer is bound afterwards.
  bool defineWork(int slot, QSize size) {
    auto *gl = context.extraFunctions();
    const int index = slot - 1;
    if (effectSlotFbo[index] && !effectSize[index].isEmpty()) {
      spareWork.push_back({effectTexture[index], effectSlotFbo[index], effectSize[index], ++workClock});
      spareWorkBytes += workBytes(effectSize[index]);
    } else {
      if (effectTexture[index]) gl->glDeleteTextures(1, &effectTexture[index]);
      if (effectSlotFbo[index]) gl->glDeleteFramebuffers(1, &effectSlotFbo[index]);
    }
    effectTexture[index] = 0;
    effectSlotFbo[index] = 0;
    effectSize[index] = QSize();
    bool found = false;
    for (size_t i = spareWork.size(); i-- > 0;) {
      if (spareWork[i].size != size) continue;
      effectTexture[index] = spareWork[i].texture;
      effectSlotFbo[index] = spareWork[i].fbo;
      spareWorkBytes -= workBytes(size);
      spareWork.erase(spareWork.begin() + std::ptrdiff_t(i));
      found = true;
      break;
    }
    if (!found) {
      GLuint texture = 0, fbo = 0;
      gl->glGenTextures(1, &texture);
      gl->glGenFramebuffers(1, &fbo);
      if (!texture || !fbo) {
        if (texture) gl->glDeleteTextures(1, &texture);
        if (fbo) gl->glDeleteFramebuffers(1, &fbo);
        return false;
      }
      gl->glActiveTexture(GL_TEXTURE0 + slot);
      gl->glBindTexture(GL_TEXTURE_2D, texture);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, size.width(), size.height(), 0,
                       GL_RGBA, GL_FLOAT, nullptr);
      gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
      if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, effectFbo);
        gl->glDeleteFramebuffers(1, &fbo);
        gl->glDeleteTextures(1, &texture);
        return false;
      }
      effectTexture[index] = texture;
      effectSlotFbo[index] = fbo;
    }
    effectSize[index] = size;
    // Kept within a budget, the one used longest ago going first.
    constexpr qint64 budget = 256ll << 20;
    while ((spareWorkBytes > budget || spareWork.size() > 96) && !spareWork.empty()) {
      auto oldest = spareWork.begin();
      for (auto it = spareWork.begin(); it != spareWork.end(); ++it)
        if (it->used < oldest->used) oldest = it;
      gl->glDeleteFramebuffers(1, &oldest->fbo);
      gl->glDeleteTextures(1, &oldest->texture);
      spareWorkBytes -= workBytes(oldest->size);
      spareWork.erase(oldest);
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, effectSlotFbo[index]);
    return true;
  }

  // A layer's source picture, kept on the card by QImage::cacheKey. A title
  // under an effect that moves by itself (Glitch, a scrolling Pattern) is the
  // same picture every frame, and sending it up again was most of its chain's
  // time. 8- and 16-bit pictures stay 8 and 16 bits on the card -- a sampler
  // reads the same values from them as from float -- so a quarter or half
  // the bytes cross the bus when one does go up.
  struct KeptSource { GLuint texture = 0; qint64 bytes = 0; quint64 used = 0; };
  std::unordered_map<qint64, KeptSource> keptSources;
  qint64 keptSourceBytes = 0;
  quint64 sourceClock = 0, sourceFrameStart = 0;
  void releaseSources() {
    auto *gl = context.extraFunctions();
    for (auto &entry : keptSources) gl->glDeleteTextures(1, &entry.second.texture);
    keptSources.clear();
    keptSourceBytes = 0;
  }
  GLuint uploadRaster(const QImage &raster) {
    const bool eightBit = raster.format() == QImage::Format_RGBA8888 ||
                          raster.format() == QImage::Format_RGBA8888_Premultiplied;
    const bool sixteenBit = raster.format() == QImage::Format_RGBA64 ||
                            raster.format() == QImage::Format_RGBA64_Premultiplied;
    const bool floatFormat = raster.format() == QImage::Format_RGBA32FPx4 ||
                             raster.format() == QImage::Format_RGBA32FPx4_Premultiplied;
    if (!eightBit && !sixteenBit && !floatFormat) return 0;
    const GLenum uploadType = eightBit ? GL_UNSIGNED_BYTE
                                       : sixteenBit ? GL_UNSIGNED_SHORT : GL_FLOAT;
    const GLenum internal = eightBit ? GL_RGBA8 : sixteenBit ? GL_RGBA16 : GL_RGBA32F;
    const int pixelBytes = eightBit ? 4 : sixteenBit ? 8 : 16;
    auto *gl = context.extraFunctions();
    auto define = [&](GLuint texture) {
      gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(raster.bytesPerLine() / pixelBytes));
      gl->glActiveTexture(GL_TEXTURE0);
      gl->glBindTexture(GL_TEXTURE_2D, texture);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      gl->glTexImage2D(GL_TEXTURE_2D, 0, GLint(internal), raster.width(), raster.height(), 0,
                       GL_RGBA, uploadType, raster.constBits());
      gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    };
    if (auto found = keptSources.find(raster.cacheKey()); found != keptSources.end()) {
      found->second.used = ++sourceClock;
      gl->glActiveTexture(GL_TEXTURE0);
      gl->glBindTexture(GL_TEXTURE_2D, found->second.texture);
      return found->second.texture;
    }
    const qint64 bytes = qint64(raster.width()) * raster.height() * pixelBytes;
    constexpr qint64 budget = 192ll * 1024 * 1024;
    // Room comes only from pictures this frame has not drawn; without it the
    // picture goes up into the one scratch slot, as it always did.
    while (keptSourceBytes + bytes > budget) {
      auto oldest = keptSources.end();
      for (auto it = keptSources.begin(); it != keptSources.end(); ++it)
        if (it->second.used <= sourceFrameStart && (oldest == keptSources.end() || it->second.used < oldest->second.used))
          oldest = it;
      if (oldest == keptSources.end()) break;
      gl->glDeleteTextures(1, &oldest->second.texture);
      keptSourceBytes -= oldest->second.bytes;
      keptSources.erase(oldest);
    }
    if (keptSourceBytes + bytes > budget) {
      define(effectTexture[3]);
      return effectTexture[3];
    }
    setErrorsAside(gl);
    GLuint texture = 0;
    gl->glGenTextures(1, &texture);
    if (!texture) return 0;
    define(texture);
    if (gl->glGetError() != GL_NO_ERROR) { gl->glDeleteTextures(1, &texture); return 0; }
    keptSources[raster.cacheKey()] = {texture, bytes, ++sourceClock};
    keptSourceBytes += bytes;
    return texture;
  }

  // A pack effect's own GLSL body, wrapped as the pixel service wraps it
  // (gpu_pixels.cpp): it is handed one texel and its coordinate, never a
  // sampler or a version line. Kept for the life of the context; a body that
  // will not compile is remembered, and its chain goes to the CPU.
  struct PackProgram { std::unique_ptr<QOpenGLShaderProgram> program; PassUniforms at; };
  std::map<QString, PackProgram> packPrograms;
  QSet<QString> refusedPrograms;
  PackProgram *packProgram(const QString &body) {
    if (auto found = packPrograms.find(body); found != packPrograms.end()) return &found->second;
    if (refusedPrograms.contains(body)) return nullptr;
    const QString source = packShaderSource(body);
    auto program = std::make_unique<QOpenGLShaderProgram>();
    if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, effectVertexShader()) ||
        !program->addShaderFromSourceCode(QOpenGLShader::Fragment, source.toUtf8().constData()) ||
        !program->link()) {
      refusedPrograms.insert(body);
      return nullptr;
    }
    const PassUniforms at(*program);
    return &packPrograms.emplace(body, PackProgram{std::move(program), at}).first->second;
  }

  GLuint runPasses(GLuint source, QSize sourceSize, const QVector<GpuPass> &passes,
                   QSize &resultSize) {
    if (!effectsReady || passes.isEmpty()) return 0;
    auto *gl = context.extraFunctions();
    GLuint id[4] = {source, effectTexture[0], effectTexture[1], effectTexture[2]};
    QSize size[4] = {sourceSize, effectSize[0], effectSize[1], effectSize[2]};
    int current = 0, lastWritten = -1;
    QSize lastSize = sourceSize;
    gl->glBindFramebuffer(GL_FRAMEBUFFER, effectFbo);
    gl->glDisable(GL_BLEND); gl->glDisable(GL_DEPTH_TEST); gl->glDisable(GL_SCISSOR_TEST);
    gl->glBindVertexArray(vao);
    QOpenGLShaderProgram *bound = nullptr;
    struct Unbind {
      QOpenGLShaderProgram *&program;
      ~Unbind() { if (program) program->release(); }
    } unbind{bound};
    for (const auto &pass : passes) {
      if (!pass.auxImage.isNull()) return 0; // nothing in a layer's chain needs one
      // A pass of a pack effect runs its own program. Before this, such a pass
      // ran the shared program with op 0 -- a plain copy -- so a colour grade
      // on the card left the picture as it was.
      QOpenGLShaderProgram *program = effects.get();
      const PassUniforms *at = &effectsAt;
      if (!pass.source.isEmpty()) {
        PackProgram *pack = packProgram(pass.source);
        if (!pack) return 0;
        program = pack->program.get();
        at = &pack->at;
      }
      if (program != bound) { program->bind(); bound = program; }
      const bool fromPrevious = pass.input == GpuPass::Previous && lastWritten >= 0;
      const int input = fromPrevious ? lastWritten : current;
      const QSize target = pass.size.isEmpty() ? size[input] : pass.size;
      if (target.isEmpty()) return 0;
      int aux = -1;
      if (pass.useAux) {
        const bool auxPrevious = pass.auxFrom == GpuPass::Previous && lastWritten >= 0;
        aux = auxPrevious ? lastWritten : current;
      }
      int write = -1;
      for (int candidate = 1; candidate < 4; ++candidate)
        if (candidate != current && candidate != input && candidate != aux) { write = candidate; break; }
      if (write < 0) return 0;
      // Allocated first, so nothing bound afterwards can be knocked off its unit.
      if (size[write] != target) {
        if (!defineWork(write, target)) return 0;
        size[write] = target;
        id[write] = effectTexture[write - 1];
      }
      if (aux >= 0) {
        gl->glActiveTexture(GL_TEXTURE0 + aux);
        gl->glBindTexture(GL_TEXTURE_2D, id[aux]);
        if (pass.source.isEmpty()) {
          gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                              pass.auxSmooth ? GL_LINEAR : GL_NEAREST);
          gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                              pass.auxSmooth ? GL_LINEAR : GL_NEAREST);
          program->setUniformValue(at->aux, aux);
        } else {
          // What a pack's wavyOriginal() reads, by texelFetch: no filter to set.
          program->setUniformValue(at->wavyOriginalTexture, aux);
        }
      }
      gl->glActiveTexture(GL_TEXTURE0 + input);
      gl->glBindTexture(GL_TEXTURE_2D, id[input]);
      program->setUniformValue(at->src, input);
      gl->glBindFramebuffer(GL_FRAMEBUFFER, effectSlotFbo[write - 1]);
      gl->glViewport(0, 0, target.width(), target.height());
      if (pass.source.isEmpty()) {
        program->setUniformValue(at->op, pass.op);
        program->setUniformValue(at->params, pass.params);
        program->setUniformValue(at->ca, pass.a);
        program->setUniformValue(at->cb, pass.b);
      } else {
        program->setUniformValue(at->sourceSize, QVector2D(float(size[input].width()), float(size[input].height())));
        QVector<float> padded = pass.values;
        padded.resize(256);
        program->setUniformValueArray(at->values, padded.constData(), 256, 1);
        program->setUniformValue(at->processingDepth, pass.processingDepth);
        program->setUniformValue(at->wavySeconds, float(pass.seconds));
        program->setUniformValue(at->wavyFrame, float(pass.frame));
        program->setUniformValue(at->wavyScale, float(pass.scale));
        program->setUniformValue(at->wavyPass, pass.shaderPass);
        program->setUniformValue(at->wavyPasses, pass.shaderPasses);
        program->setUniformValue(at->wavyBox, QVector4D(float(pass.box.x()), float(pass.box.y()),
                                                        float(pass.box.width()), float(pass.box.height())));
      }
      gl->glDrawArrays(GL_TRIANGLES, 0, 3);
      lastWritten = write; lastSize = target;
      if (pass.endsEffect) current = write;
    }
    gl->glBindVertexArray(0);
    // A result that is kept or read back is checked by the step that keeps it.
    if (lastWritten < 0 || !stepWithoutError(gl)) return 0;
    resultSize = lastSize;
    return id[lastWritten];
  }

  // Particles, written by the card all at once: one quad per particle, each
  // pixel worked out by the same arithmetic writeMarks does on the CPU
  // (particles.cpp), into a picture of their own that then goes over the
  // frame -- or is added to it -- at the layer's opacity, as the CPU's
  // picture of them does. Over a picture the order they are drawn in is kept.
  // Squares, stars and confetti, which the CPU fills with QPainter, are
  // covered here as Qt's rasterizer covers them, from their outlines.
  static constexpr int kParticleFloats = 48; // a mark's 20, its outline's 20, fill and how
  // The line rasterizer's levels for one draw's squares (rule 1), one byte a
  // pixel, kLevelRow to a row of a picture made again only when it must grow.
  static constexpr int kLevelRow = 2048;
  GLuint particleLevels = 0;
  int particleLevelRows = 0;
  bool uploadParticleLevels(const std::vector<uchar> &levels) {
    auto *gl = context.extraFunctions();
    const int rows = int((levels.size() + kLevelRow - 1) / kLevelRow);
    gl->glActiveTexture(GL_TEXTURE0);
    if (!particleLevels || particleLevelRows < rows) {
      if (!particleLevels) gl->glGenTextures(1, &particleLevels);
      if (!particleLevels) return false;
      particleLevelRows = std::max(rows, std::max(16, particleLevelRows * 2));
      gl->glBindTexture(GL_TEXTURE_2D, particleLevels);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kLevelRow, particleLevelRows, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    }
    gl->glBindTexture(GL_TEXTURE_2D, particleLevels);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const int whole = int(levels.size() / kLevelRow), rest = int(levels.size() % kLevelRow);
    if (whole) gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kLevelRow, whole, GL_RED, GL_UNSIGNED_BYTE, levels.data());
    if (rest)
      gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, whole, rest, 1, GL_RED, GL_UNSIGNED_BYTE,
                          levels.data() + size_t(whole) * kLevelRow);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    return true;
  }
  std::unique_ptr<QOpenGLShaderProgram> particleProgram, particleMix;
  struct { GLint size = -1, height = -1, shape = -1, levels = -1; } particleAt;
  struct { GLint beneath = -1, particles = -1, opacity = -1; } particleMixAt;
  std::unique_ptr<QOpenGLFramebufferObject> particleTarget, particleBeneath;
  GLuint particleVao = 0, particleVbo = 0;
  bool particlesAttempted = false, particlesReady = false;
  quint64 particleLayers = 0;
  bool prepareParticles() {
    if (particlesAttempted) return particlesReady;
    particlesAttempted = true;
    auto program = std::make_unique<QOpenGLShaderProgram>();
    const char *vertex = R"(
      #version 330 core
      layout(location = 0) in vec4 bounds;  // left, top, right, bottom (pixels, exclusive)
      layout(location = 1) in vec4 centre;  // cx, cy, radius, pixelScale
      layout(location = 2) in vec4 inverse; // i11, i12, i21, i22
      layout(location = 3) in vec4 streak;  // sx, sy, lsx, lsy
      layout(location = 4) in vec4 colour;  // straight r, g, b (0..255), alpha
      // Squares, stars and confetti (ParticleOutline): the outline's corners,
      // two to a vector, or rule 1's box of levels; the colour Qt fills it
      // with, premultiplied, 0..255; how many corners, which of Qt's
      // rasterizers fills it, and where rule 1's levels start.
      layout(location = 5) in vec4 outline[5];
      layout(location = 10) in vec4 fill;
      layout(location = 11) in vec4 how;
      uniform vec2 size;
      flat out vec4 vCentre; flat out vec4 vInverse; flat out vec4 vStreak; flat out vec4 vColour;
      flat out vec4 vOutline[5]; flat out vec4 vFill; flat out vec4 vHow;
      void main() {
        int id = gl_VertexID;
        vec2 corner = vec2(id == 1 || id == 2 || id == 4 ? 1.0 : 0.0, id == 2 || id == 4 || id == 5 ? 1.0 : 0.0);
        vec2 at = mix(bounds.xy, bounds.zw, corner);
        gl_Position = vec4(at.x / size.x * 2.0 - 1.0, 1.0 - at.y / size.y * 2.0, 0.0, 1.0);
        vCentre = centre; vInverse = inverse; vStreak = streak; vColour = colour;
        for (int i = 0; i < 5; ++i) vOutline[i] = outline[i];
        vFill = fill; vHow = how;
      })";
    const char *fragment = R"(
      #version 330 core
      uniform int shape;
      uniform float height;
      flat in vec4 vCentre; flat in vec4 vInverse; flat in vec4 vStreak; flat in vec4 vColour;
      flat in vec4 vOutline[5]; flat in vec4 vFill; flat in vec4 vHow;
      uniform sampler2D levels; // rule 1's, 2048 (kLevelRow) to a row
      out vec4 color;
      float roundAlpha(float u) {
        if (shape == 1) return 1.0;
        if (shape == 0) return u < 0.45 ? 1.0 - u / 0.45 * 0.45 : 0.55 * (1.0 - (u - 0.45) / 0.55);
        if (u < 0.78) return 0.3;
        if (u < 0.93) return 0.3 + (u - 0.78) / 0.15 * 0.55;
        return 0.85 * (1.0 - (u - 0.93) / 0.07);
      }
      // Twice the area the side from p to q adds to how much of the pixel
      // whose top left corner is the origin the outline covers. The side is
      // pressed into the pixel -- each coordinate held to 0..1, which leaves
      // the outline winding the same way about every point of the pixel -- and
      // what is left measured by the shoelace, the side broken where it crosses
      // the pixel's edges, between which the pressing keeps it straight. No
      // arrays of corners cut down side by side: done that way, the particles
      // of Promo's end card at 1080p took 0.12 ms of the card's own time a
      // frame; this way 0.0093 (the old guess from the slope took 0.0025).
      float sideInPixel(vec2 p, vec2 q) {
        vec2 d = q - p;
        float a = d.x != 0.0 ? clamp(-p.x / d.x, 0.0, 1.0) : 0.0;
        float b = d.x != 0.0 ? clamp((1.0 - p.x) / d.x, 0.0, 1.0) : 0.0;
        float c = d.y != 0.0 ? clamp(-p.y / d.y, 0.0, 1.0) : 0.0;
        float e = d.y != 0.0 ? clamp((1.0 - p.y) / d.y, 0.0, 1.0) : 0.0;
        float low1 = min(a, b), high1 = max(a, b), low2 = min(c, e), high2 = max(c, e);
        float middle1 = max(low1, low2), middle2 = min(high1, high2);
        float t1 = min(low1, low2), t2 = min(middle1, middle2), t3 = max(middle1, middle2), t4 = max(high1, high2);
        vec2 s0 = clamp(p, 0.0, 1.0);
        vec2 s1 = clamp(p + d * t1, 0.0, 1.0), s2 = clamp(p + d * t2, 0.0, 1.0);
        vec2 s3 = clamp(p + d * t3, 0.0, 1.0), s4 = clamp(p + d * t4, 0.0, 1.0);
        vec2 s5 = clamp(q, 0.0, 1.0);
        return (s0.x * s1.y - s1.x * s0.y) + (s1.x * s2.y - s2.x * s1.y) + (s2.x * s3.y - s3.x * s2.y) +
               (s3.x * s4.y - s4.x * s3.y) + (s4.x * s5.y - s5.x * s4.y);
      }
      // A square, star or piece of confetti as Qt's raster engine fills it on
      // the CPU (see ParticleOutline in particles.h), in the levels Qt's
      // rasterizer gives this pixel, and the colour scaled by that as Qt's
      // blend scales it.
      vec4 outlined() {
        vec2 origin = vec2(floor(gl_FragCoord.x), height - gl_FragCoord.y - 0.5);
        float level;
        if (vHow.y > 0.5) {
          // The line rasterizer's, for a rectangle that only turns and scales
          // evenly: worked out on the CPU, its own arithmetic being nothing
          // like the share of the pixel covered.
          ivec2 at = ivec2(origin - vOutline[0].xy);
          int columns = int(vOutline[0].z);
          if (at.x < 0 || at.y < 0 || at.x >= columns || at.y >= int(vOutline[0].w)) return vec4(0.0);
          int index = int(vHow.z) + at.y * columns + at.x;
          level = floor(texelFetch(levels, ivec2(index % 2048, index / 2048), 0).r * 255.0 + 0.5);
        } else {
          // The gray rasterizer's: how much of the pixel the outline covers --
          // its sides pressed into the pixel, their shares added -- in 1/256 of
          // a pixel, the sum shifted down: towards nought for an outline turning
          // one way, away from it for the other.
          vec2 corners[10];
          corners[0] = vOutline[0].xy; corners[1] = vOutline[0].zw; corners[2] = vOutline[1].xy; corners[3] = vOutline[1].zw;
          corners[4] = vOutline[2].xy; corners[5] = vOutline[2].zw; corners[6] = vOutline[3].xy; corners[7] = vOutline[3].zw;
          corners[8] = vOutline[4].xy; corners[9] = vOutline[4].zw;
          int n = int(vHow.x);
          float twice = 0.0;
          for (int i = 0; i < 10; ++i)
            if (i < n) twice += sideInPixel(corners[i] - origin, (i + 1 < n ? corners[i + 1 < 10 ? i + 1 : 0] : corners[0]) - origin);
          float area = 0.5 * twice;
          level = area >= 0.0 ? ceil(area * 256.0 - 1e-3) : floor(-area * 256.0 + 1e-3);
        }
        level = clamp(level, 0.0, 255.0);
        // Qt's BYTE_MUL, channel by channel.
        vec4 scaled = vFill * level;
        return floor((scaled + floor(scaled / 256.0) + 128.0) / 256.0) / 255.0;
      }
      void main() {
        if (shape == 2 || shape == 5 || shape == 7) {
          color = outlined();
          if (color.a <= 0.0) discard;
          return;
        }
        vec2 d = vec2(gl_FragCoord.x, height - gl_FragCoord.y) - vCentre.xy;
        vec2 l = vec2(vInverse.x * d.x + vInverse.y * d.y, vInverse.z * d.x + vInverse.w * d.y);
        float radius = vCentre.z, pixelScale = vCentre.w;
        float strength = 1.0, edge;
        if (shape == 3) {
          vec2 own = vStreak.zw;
          float length2 = dot(own, own);
          float along = length2 > 1e-12 ? dot(l, own) / length2 : 1.0;
          float t = clamp(along, 0.0, 1.0);
          float distance = length(l - own * t);
          float scale = distance > 1e-6 ? length(d - vStreak.xy * t) / distance : pixelScale;
          edge = min(1.0, (radius - distance) * scale + 0.5);
          strength = t;
        } else if (shape == 4) {
          float distance = length(l);
          float gradient = pixelScale > 0.0 ? 1.0 / pixelScale : 1.0;
          if (distance > 1e-6) {
            vec2 q = l / distance;
            gradient = length(vec2(vInverse.x * q.x + vInverse.z * q.y, vInverse.y * q.x + vInverse.w * q.y));
          }
          edge = min(1.0, (radius - distance) / max(gradient, 1e-6) + 0.5);
        } else {
          float distance = length(l);
          float scale = distance > 1e-6 ? length(d) / distance : pixelScale;
          edge = min(1.0, (radius - distance) * scale + 0.5);
          strength = roundAlpha(min(1.0, distance / radius));
        }
        float a = vColour.a * strength;
        if (edge <= 0.0 || a <= 0.0) discard;
        color = vec4(vColour.rgb / 255.0 * a, a) * edge;
      })";
    if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) ||
        !program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) || !program->link())
      return false;
    // Added at part opacity, the picture goes that share of the way from what
    // is beneath to the two added together -- which stop at white -- as
    // QPainter's Plus does. Blending can only add a share of the picture, which
    // runs past where the CPU's stops wherever the sum is brighter than white.
    auto mix = std::make_unique<QOpenGLShaderProgram>();
    const char *mixVertex = R"(
      #version 330 core
      void main() {
        vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
        gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
      })";
    const char *mixFragment = R"(
      #version 330 core
      uniform sampler2D beneath, particles;
      uniform float opacity;
      out vec4 color;
      void main() {
        ivec2 at = ivec2(gl_FragCoord.xy);
        vec4 d = texelFetch(beneath, at, 0), s = texelFetch(particles, at, 0);
        color = mix(d, min(d + s, vec4(1.0)), opacity);
      })";
    if (!mix->addShaderFromSourceCode(QOpenGLShader::Vertex, mixVertex) ||
        !mix->addShaderFromSourceCode(QOpenGLShader::Fragment, mixFragment) || !mix->link())
      return false;
    particleAt = {program->uniformLocation("size"), program->uniformLocation("height"), program->uniformLocation("shape"),
                  program->uniformLocation("levels")};
    particleMixAt = {mix->uniformLocation("beneath"), mix->uniformLocation("particles"), mix->uniformLocation("opacity")};
    particleMix = std::move(mix);
    auto *gl = context.extraFunctions();
    gl->glGenVertexArrays(1, &particleVao);
    gl->glGenBuffers(1, &particleVbo);
    gl->glBindVertexArray(particleVao);
    gl->glBindBuffer(GL_ARRAY_BUFFER, particleVbo);
    for (GLuint index = 0; index < 12; ++index) {
      gl->glEnableVertexAttribArray(index);
      gl->glVertexAttribPointer(index, 4, GL_FLOAT, GL_FALSE, kParticleFloats * sizeof(float),
                                reinterpret_cast<const void *>(size_t(index) * 4 * sizeof(float)));
      gl->glVertexAttribDivisor(index, 1);
    }
    gl->glBindVertexArray(0);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    particleProgram = std::move(program);
    particlesReady = true;
    return true;
  }
  bool particles(QPainter &p, const std::vector<ParticleMark> &marks, const ParticleOutlines &outlines,
                 QRect area, int shape, bool add) {
    QPaintDevice *device = p.device();
    const QSize size(device->width(), device->height());
    // Squares, stars and confetti are covered from their outlines, one a mark.
    const bool outlined = shape == 2 || shape == 5 || shape == 7;
    if (outlined && outlines.shapes.size() != marks.size()) return false;
    area &= QRect(QPoint(), size);
    if (area.isEmpty()) return true;
    const double opacity = p.opacity();
    p.beginNativePainting();
    struct EndNative {
      QPainter &p;
      ~EndNative() { p.endNativePainting(); }
    } endNative{p};
    if (!prepareParticles()) return false;
    auto *gl = context.extraFunctions();
    clearBeforeStep(gl);
    // Where the frame is being drawn, known before a new framebuffer is made:
    // making one leaves another bound, and the particles went there instead.
    const DrawTarget frame = drawTarget(device);
    const GLint previousFbo = frame.fbo;
    const GLint *viewport = frame.viewport;
    const bool addedInPart = add && opacity < 1;
    auto target = [&](std::unique_ptr<QOpenGLFramebufferObject> &fbo) {
      if (fbo && fbo->size() == size) return true;
      QOpenGLFramebufferObjectFormat format;
      format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
      format.setInternalTextureFormat(GL_RGBA8);
      fbo = std::make_unique<QOpenGLFramebufferObject>(size, format);
      if (fbo->isValid()) return true;
      fbo.reset();
      return false;
    };
    if (!target(particleTarget) || (addedInPart && !target(particleBeneath))) {
      gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFbo));
      return false;
    }
    std::vector<float> instances;
    instances.reserve(marks.size() * kParticleFloats);
    for (size_t i = 0; i < marks.size(); ++i) {
      const ParticleMark &m = marks[i];
      instances.insert(instances.end(), {float(m.left), float(m.top), float(m.right + 1), float(m.bottom + 1),
                                         m.cx, m.cy, m.radius, m.pixelScale,
                                         m.i11, m.i12, m.i21, m.i22,
                                         m.sx, m.sy, m.lsx, m.lsy,
                                         m.r, m.g, m.b, m.a});
      if (!outlined) {
        instances.insert(instances.end(), kParticleFloats - 20, 0.f);
        continue;
      }
      const ParticleOutline &o = outlines.shapes[i];
      if (o.rule == 1) {
        instances.insert(instances.end(), {float(o.levelsBox.left()), float(o.levelsBox.top()), float(o.levelsBox.width()),
                                           float(o.levelsBox.height())});
        instances.insert(instances.end(), 16, 0.f);
      } else {
        for (int k = 0; k < 10; ++k) instances.insert(instances.end(), {o.x[k], o.y[k]});
      }
      instances.insert(instances.end(), {o.r, o.g, o.b, o.a, float(o.corners), float(o.rule), float(o.levelsAt), 0.f});
    }
    // The line rasterizer's levels (rule 1), in rows of a picture the shader
    // reads them from by where each mark's start.
    const bool levelled = outlined && !outlines.levels.empty();
    if (levelled && !uploadParticleLevels(outlines.levels)) {
      gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFbo));
      return false;
    }
    // Their own picture, cleared where they fall.
    particleTarget->bind();
    gl->glViewport(0, 0, size.width(), size.height());
    gl->glDisable(GL_DEPTH_TEST); gl->glDisable(GL_STENCIL_TEST); gl->glDisable(GL_CULL_FACE);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl->glEnable(GL_SCISSOR_TEST);
    gl->glScissor(area.left(), size.height() - area.bottom() - 1, area.width(), area.height());
    gl->glClearColor(0, 0, 0, 0);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glEnable(GL_BLEND);
    gl->glBlendEquation(GL_FUNC_ADD);
    if (add) gl->glBlendFunc(GL_ONE, GL_ONE);
    else gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    particleProgram->bind();
    particleProgram->setUniformValue(particleAt.size, QVector2D(size.width(), size.height()));
    particleProgram->setUniformValue(particleAt.height, float(size.height()));
    particleProgram->setUniformValue(particleAt.shape, shape);
    particleProgram->setUniformValue(particleAt.levels, 1);
    gl->glActiveTexture(GL_TEXTURE1);
    gl->glBindTexture(GL_TEXTURE_2D, levelled ? particleLevels : 0);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindVertexArray(particleVao);
    gl->glBindBuffer(GL_ARRAY_BUFFER, particleVbo);
    gl->glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(instances.size() * sizeof(float)), instances.data(), GL_STREAM_DRAW);
    gl->glDrawArraysInstanced(GL_TRIANGLES, 0, 6, GLsizei(marks.size()));
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glBindVertexArray(0);
    particleProgram->release();
    gl->glActiveTexture(GL_TEXTURE1);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    gl->glActiveTexture(GL_TEXTURE0);
    // Then that picture onto the frame, over it or added, at the layer's opacity.
    gl->glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (addedInPart) {
      // What is beneath, its samples resolved, for the mix to read.
      gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(previousFbo));
      gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, particleBeneath->handle());
      const int bottom = size.height() - area.bottom() - 1;
      gl->glBlitFramebuffer(area.left(), bottom, area.right() + 1, bottom + area.height(),
                            area.left(), bottom, area.right() + 1, bottom + area.height(),
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
      gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFbo));
      gl->glDisable(GL_BLEND);
      particleMix->bind();
      particleMix->setUniformValue(particleMixAt.beneath, 0);
      particleMix->setUniformValue(particleMixAt.particles, 1);
      particleMix->setUniformValue(particleMixAt.opacity, float(opacity));
      gl->glActiveTexture(GL_TEXTURE1);
      gl->glBindTexture(GL_TEXTURE_2D, particleTarget->texture());
      gl->glActiveTexture(GL_TEXTURE0);
      gl->glBindTexture(GL_TEXTURE_2D, particleBeneath->texture());
      gl->glBindVertexArray(vao);
      gl->glDrawArrays(GL_TRIANGLES, 0, 3);
      gl->glBindVertexArray(0);
      particleMix->release();
      gl->glActiveTexture(GL_TEXTURE1);
      gl->glBindTexture(GL_TEXTURE_2D, 0);
      gl->glActiveTexture(GL_TEXTURE0);
      gl->glDisable(GL_SCISSOR_TEST);
      if (!stepWithoutError(gl)) return false;
      ++particleLayers;
      return true;
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFbo));
    shader->bind();
    shader->setUniformValue(shaderAt.source, 0);
    shader->setUniformValue(shaderAt.weight, float(opacity));
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, particleTarget->texture());
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glBindVertexArray(vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    gl->glBindVertexArray(0);
    shader->release();
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_SCISSOR_TEST);
    if (!stepWithoutError(gl)) return false;
    ++particleLayers;
    return true;
  }
  // Picture layers from textures the card keeps, by the picture's identity,
  // the least lately drawn let go past a budget. Drawn as a quad carried by
  // the painter's transform -- a projected 3D plane included -- at its
  // opacity, cut to the painter's rectangle.
  struct KeptImage { GLuint texture = 0; QSize size; qint64 bytes = 0; quint64 used = 0; };
  std::unordered_map<qint64, KeptImage> keptImages;
  qint64 keptImageBytes = 0;
  quint64 imageClock = 0, imageUploads = 0, imageDraws = 0, litFaces = 0;
  void releaseImages() {
    auto *gl = context.extraFunctions();
    for (auto &entry : keptImages) gl->glDeleteTextures(1, &entry.second.texture);
    keptImages.clear();
    keptImageBytes = 0;
  }
  GLuint keptImage(const QImage &image) {
    auto found = keptImages.find(image.cacheKey());
    if (found != keptImages.end()) {
      found->second.used = ++imageClock;
      return found->second.texture;
    }
    // ARGB32 premultiplied -- every raster the compositor draws -- goes up as
    // it is, BGRA in memory; anything else is turned into RGBA first.
    const bool bgra = image.format() == QImage::Format_ARGB32_Premultiplied;
    const QImage upload = bgra ? image : image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (upload.isNull()) return 0;
    auto *gl = context.extraFunctions();
    if (upload.width() > maxTextureSize || upload.height() > maxTextureSize) return 0;
    const qint64 bytes = qint64(upload.width()) * upload.height() * 4;
    constexpr qint64 budget = 384ll * 1024 * 1024;
    while (!keptImages.empty() && keptImageBytes + bytes > budget) {
      auto oldest = keptImages.begin();
      for (auto it = keptImages.begin(); it != keptImages.end(); ++it)
        if (it->second.used < oldest->second.used) oldest = it;
      gl->glDeleteTextures(1, &oldest->second.texture);
      keptImageBytes -= oldest->second.bytes;
      keptImages.erase(oldest);
    }
    // Kept for later frames, so its upload is checked on its own.
    setErrorsAside(gl);
    GLuint texture = 0;
    gl->glGenTextures(1, &texture);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(upload.bytesPerLine() / 4));
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, upload.width(), upload.height(), 0,
                     bgra ? GL_BGRA : GL_RGBA, GL_UNSIGNED_BYTE, upload.constBits());
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (gl->glGetError() != GL_NO_ERROR) { gl->glDeleteTextures(1, &texture); return 0; }
    keptImages[image.cacheKey()] = {texture, upload.size(), bytes, ++imageClock};
    keptImageBytes += bytes;
    ++imageUploads;
    return texture;
  }
  bool picture(QPainter &p, const QRectF &box, const QImage &image) {
    if (!effectsReady || box.isEmpty()) return false;
    if (p.compositionMode() != QPainter::CompositionMode_SourceOver) return false;
    QRect clip;
    if (!painterClipIsDeviceRect(p, &clip)) return false;
    if (clip.isEmpty()) return true;
    const QTransform toDevice = QTransform::fromScale(box.width(), box.height()) *
                                QTransform::fromTranslate(box.left(), box.top()) * p.combinedTransform();
    const double opacity = p.opacity();
    const bool smooth = p.testRenderHint(QPainter::SmoothPixmapTransform);
    const QSize device(p.device()->width(), p.device()->height());
    p.beginNativePainting();
    struct EndNative {
      QPainter &p;
      ~EndNative() { p.endNativePainting(); }
    } endNative{p};
    auto *gl = context.extraFunctions();
    clearBeforeStep(gl);
    const GLuint texture = keptImage(image);
    if (!texture) return false;
    const QTransform toClip(2. / device.width(), 0, 0, -2. / device.height(), -1, 1);
    const QTransform t = toDevice * toClip;
    const QMatrix4x4 quad(t.m11(), t.m21(), 0, t.m31(),
                          t.m12(), t.m22(), 0, t.m32(),
                          0, 0, 1, 0,
                          t.m13(), t.m23(), 0, t.m33());
    composite->bind();
    composite->setUniformValue(compositeAt.quad, quad);
    composite->setUniformValue(compositeAt.source, 0);
    composite->setUniformValue(compositeAt.opacity, float(opacity));
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    gl->glEnable(GL_BLEND);
    gl->glBlendEquation(GL_FUNC_ADD);
    gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDisable(GL_DEPTH_TEST); gl->glDisable(GL_STENCIL_TEST); gl->glDisable(GL_CULL_FACE);
    gl->glEnable(GL_SCISSOR_TEST);
    gl->glScissor(clip.left(), device.height() - clip.bottom() - 1, clip.width(), clip.height());
    gl->glBindVertexArray(vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 6);
    gl->glBindVertexArray(0);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glDisable(GL_BLEND);
    composite->release();
    if (!stepWithoutError(gl)) return false;
    ++imageDraws;
    return true;
  }
  // Shape layers (ShapeCardHook), every pixel covered the way the processor's
  // rasterizer covers it. Through Qt's GL engine a shape's edge pixels took
  // the share of the eight samples they covered: seven levels on an edge where
  // the CPU has every one, up to 75/255 off the exported picture on the
  // Twitter promo's ovals and rings and 80 on Kotonoha's hairline threads.
  // Sixteen and thirty-two samples cost twice and four times as much and were
  // still 37 to 80 off.
  //
  // Here the outline is laid out as Qt's raster engine lays it out
  // (QOutlineMapper): curves cut into lines in the shape's own units, 0.25 /
  // scale from true, every point carried to the device and rounded to 1/64 of
  // a pixel. Each edge then adds to every pixel of the rows it crosses its
  // share of the pixel to its right, signed by the way it runs -- the gray
  // rasterizer's area and cover in one -- so a float target ends up holding
  // in each pixel the area of it inside the outline, counted as often as the
  // outline winds round it. That becomes the rasterizer's own level (floor of
  // 256 times it, the sign dropped; at most 255 winding, folded even-odd), a
  // stroke's cut is worked out the same way and multiplied in as the raster
  // engine multiplies its clip, and the colour is Qt's premultiplied 8-bit
  // one times the level by BYTE_MUL. Edges then land within 2/255 of the CPU's
  // (4 where a stroke lies over a translucent fill: two blends, each rounded
  // its own way).
  // The fill in two forms (see prepareShapes): [0] reads a shape's area
  // from the float targets, [1] sums it itself.
  std::unique_ptr<QOpenGLShaderProgram> shapeEdgeProgram, shapeFillProgram[2];
  struct { GLint box = -1; } shapeEdgeAt;
  struct FillAt {
    GLint box = -1, device = -1, tables = -1, bases = -1, evenOdd = -1, cutRule = -1, kind = -1, colour = -1,
          toBrush = -1, geometry = -1;
  } shapeFillAt[2];
  GLuint shapeVao = 0, shapeEdgeBuffer = 0, shapeEdgeList = 0, shapeRowBuffer = 0, shapeRowList = 0, shapeTable = 0;
  // How many edges a shape's pixels may sum between them -- each pixel its
  // row's -- for it still to be summed in one pass (see shapes).
  static constexpr qint64 kShapeSummedAtMost = qint64(1) << 23;
  GLuint shapeArea[2] = {}, shapeAreaFbo[2] = {}; // a shape's, its cut's
  QSize shapeAreaSize;
  bool shapesAttempted = false, shapesReady = false;
  quint64 shapeFills = 0, shapeFillsAddedUp = 0;
  std::vector<float> shapeEdges; // x0, y0, x1, y1 in device pixels, rows from the top, an edge to four
  std::vector<qint32> shapeEdgeRows; // each edge's first row and the one past its last, two to an edge
  // Row tables (rowTable): per table, its box's rows' starts, then edge indices.
  std::vector<qint32> shapeRows;
  std::vector<qint32> rowCursors, shapeSpans, cutSpans;
  // Both go up into rings, each call's after the last's, a buffer made
  // anew only when the next will not fit. Made anew for every call, a
  // buffer the fill reads as a texture kept the card waiting on it: 600
  // small shapes a frame came to 6 ms more than Qt's engine took.
  struct Ring { GLuint buffer = 0; qint64 size = 0, used = 0; };
  Ring edgeRing, rowRing;
  // `bytes` put into the ring at a fresh offset; that offset in `unit`s.
  // A full ring starts again in new storage of the same size (the card may
  // still be reading the old), larger only for an upload that would not fit
  // in it at all: doubled each time round, the promo's took 8 MB in ten
  // seconds and would have gone on growing as long as it played.
  qint64 stream(Ring &ring, GLenum target, const void *data, qint64 bytes, qint64 unit) {
    auto *gl = context.extraFunctions();
    gl->glBindBuffer(target, ring.buffer);
    if (ring.used + bytes > ring.size) {
      if (bytes > ring.size) ring.size = std::max<qint64>({ring.size * 2, bytes, qint64(1) << 20});
      gl->glBufferData(target, GLsizeiptr(ring.size), nullptr, GL_STREAM_DRAW);
      ring.used = 0;
    }
    const qint64 at = ring.used;
    gl->glBufferSubData(target, GLintptr(at), GLsizeiptr(bytes), data);
    ring.used = (at + bytes + 63) / 64 * 64;
    return at / unit;
  }
  bool prepareShapes() {
    if (shapesAttempted) return shapesReady;
    shapesAttempted = true;
    // An edge's share of a pixel to its right, within the pixel's row: its
    // height there times the part of the row it leaves to its right, taken in
    // three runs (left of the pixel, across it, right of it), so an upright
    // edge loses no precision. Running down the screen it adds, up it takes;
    // summed over a closed outline, the area of the pixel inside it, counted
    // as often as the outline winds round it.
    const QByteArray rightOf = R"(
      float rightOf(vec4 edge, vec2 pixel) {
        float x0 = edge.x, y0 = edge.y, x1 = edge.z, y1 = edge.w, sign = 1.0;
        if (y0 > y1) { float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; sign = -1.0; }
        float top = max(y0, pixel.y), bottom = min(y1, pixel.y + 1.0);
        float height = bottom - top;
        if (height <= 0.0) return 0.0;
        // Wholly left of the pixel it covers all of the pixel's height in
        // its row; wholly right, none: what the sums below come to there,
        // without them.
        if (max(x0, x1) <= pixel.x) return sign * height;
        if (min(x0, x1) >= pixel.x + 1.0) return 0.0;
        float slope = (x1 - x0) / (y1 - y0);
        float xt = x0 + (top - y0) * slope - pixel.x, xb = x0 + (bottom - y0) * slope - pixel.x;
        float lo = min(xt, xb), hi = max(xt, xb), across = hi - lo, left;
        if (across < 1e-6) left = clamp(0.5 * (lo + hi), 0.0, 1.0);
        else {
          float before = clamp(-lo / across, 0.0, 1.0), after = clamp((hi - 1.0) / across, 0.0, 1.0);
          left = max(0.0, 1.0 - before - after) * 0.5 * (max(lo, 0.0) + min(hi, 1.0)) + after;
        }
        return sign * height * (1.0 - left);
      })";
    // A large shape's edges added up in a float target, each edge one
    // instance: a quad over the rows it crosses, from its left to the right
    // side of the area being worked out.
    const char *edgeVertex = R"(#version 330 core
      layout(location = 0) in vec4 edge; // x0, y0, x1, y1
      uniform vec4 box;                  // left, top, width, height of the area worked out
      flat out vec4 vEdge;
      void main() {
        int id = gl_VertexID;
        vec2 corner = vec2(id == 1 || id == 2 || id == 4 ? 1.0 : 0.0, id == 2 || id == 4 || id == 5 ? 1.0 : 0.0);
        float right = box.x + box.z;
        float left = min(floor(min(edge.x, edge.z)), right);
        vec2 at = vec2(mix(left, right, corner.x), mix(floor(min(edge.y, edge.w)), ceil(max(edge.y, edge.w)), corner.y));
        gl_Position = vec4((at - box.xy) / box.zw * 2.0 - 1.0, 0.0, 1.0);
        vEdge = edge;
      })";
    const QByteArray edgeFragment = QByteArray(R"(#version 330 core
      flat in vec4 vEdge;
      uniform vec4 box;
      out vec4 area;)") + rightOf + R"(
      void main() {
        float share = rightOf(vEdge, box.xy + floor(gl_FragCoord.xy));
        if (share == 0.0) discard;
        area = vec4(share, 0.0, 0.0, 0.0);
      })";
    const char *fillVertex = R"(#version 330 core
      uniform vec4 box;    // device pixels: left, top, width, height
      uniform vec2 device; // its width and height
      void main() {
        const vec2 corner[6] = vec2[6](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,0), vec2(1,1), vec2(0,1));
        vec2 at = box.xy + corner[gl_VertexID] * box.zw;
        gl_Position = vec4(at.x / device.x * 2.0 - 1.0, 1.0 - at.y / device.y * 2.0, 0.0, 1.0);
      })";
    // A shape's area summed here, pixel by pixel, over the edges crossing its
    // row (and its cut's); a huge one's read from what the edges added up
    // in the float target. Then the
    // level qgrayraster.c's gray_hline makes of it, a stroke's cut multiplied
    // in as qt_intersect_spans multiplies a clip (qt_div_255), and the span
    // laid in as comp_func_solid_SourceOver lays it: the colour times the level
    // by BYTE_MUL, the card adding what lies beneath times one less its alpha.
    //
    // Two programs, one for each way of coming by the area, rather than one
    // asking a uniform which: with one, a frame that had both kinds -- the
    // promo's rings, large ones added up and small ones summed -- held the
    // card past Windows' two seconds (a TDR; the program was closed), the
    // sums run over whatever row table a summed shape had left behind. A
    // sum is also never longer than the most edges any row of the call's
    // tables holds, whatever it reads.
    auto fillFragment = [&](bool summed) {
      return QByteArray("#version 330 core\n#define SUMMED ") + (summed ? "1" : "0") + R"(
      uniform sampler2D area, cut, table;
      uniform samplerBuffer edgeList;
      uniform isamplerBuffer rowList;
      uniform vec4 box;
      uniform vec2 device;
      uniform ivec4 tables;               // summed: the shape's row table, its cut's, the most edges a row holds
      uniform ivec2 bases;                // where this call's edges and row tables start in the rings
      uniform int evenOdd, cutRule, kind; // cutRule: 0 none, 1 winding, 2 even-odd; kind: 0 colour, 1 linear, 2 radial
      uniform vec4 colour;                // premultiplied, 0..255, the painter's opacity in it
      uniform mat3 toBrush;               // a device pixel's centre to the gradient's own place
      uniform vec4 geometry;              // linear: start, end; radial: centre, radius
      out vec4 fragColour;)" + rightOf + R"(
      // Over the edges crossing the pixel's row: its row's entry in the table
      // gives where their indices start, the next row's where they end.
      float areaOf(vec2 pixel, int table, int row) {
        int from = bases.y + texelFetch(rowList, bases.y + table + row).r;
        int to = min(bases.y + texelFetch(rowList, bases.y + table + row + 1).r, from + tables.z);
        float total = 0.0;
        for (int i = from; i < to; ++i) total += rightOf(texelFetch(edgeList, bases.x + texelFetch(rowList, i).r), pixel);
        return total;
      }
      int level(float covered, bool oddEven) {
        float s = covered * 256.0, r = floor(s + 0.5);
        if (abs(s - r) < 1e-3) s = r; // a sum of 64ths the floats left a hair off
        int v = abs(int(floor(s)));
        if (oddEven) { v &= 511; if (v > 256) v = 512 - v; else if (v == 256) v = 255; }
        return min(v, 255);
      }
      int div255(int x) { return (x + (x >> 8) + 128) >> 8; }
      int byteMul(int x, int a) { int t = x * a + 128; return (t + (t >> 8)) >> 8; }
      void main() {
        ivec2 pixel = ivec2(int(gl_FragCoord.x), int(device.y) - 1 - int(gl_FragCoord.y));
        ivec2 at = pixel - ivec2(box.xy);
      #if SUMMED
        float covered = areaOf(vec2(pixel), tables.x, at.y);
      #else
        float covered = texelFetch(area, at, 0).r;
      #endif
        int coverage = level(covered, evenOdd != 0);
        if (cutRule != 0) {
      #if SUMMED
          float cutCovered = areaOf(vec2(pixel), tables.y, at.y);
      #else
          float cutCovered = texelFetch(cut, at, 0).r;
      #endif
          coverage = div255(coverage * level(cutCovered, cutRule == 2));
        }
        if (coverage <= 0) discard;
        ivec4 c = ivec4(colour + 0.5);
        if (kind != 0) {
          vec3 q = toBrush * vec3(vec2(pixel) + 0.5, 1.0);
          vec2 p = q.xy / q.z;
          float t;
          if (kind == 1) {
            vec2 d = geometry.zw - geometry.xy;
            float l = dot(d, d);
            t = l > 0.0 ? dot(p - geometry.xy, d) / l : 0.0;
          } else t = geometry.z > 0.0 ? length(p - geometry.xy) / geometry.z : 0.0;
          c = ivec4(texelFetch(table, ivec2(clamp(int(t * 1023.0 + 0.5), 0, 1023), 0), 0) * 255.0 + 0.5);
        }
        if (coverage < 255)
          c = ivec4(byteMul(c.r, coverage), byteMul(c.g, coverage), byteMul(c.b, coverage), byteMul(c.a, coverage));
        fragColour = vec4(c) / 255.0;
      })";
    };
    auto edges = std::make_unique<QOpenGLShaderProgram>();
    if (!edges->addShaderFromSourceCode(QOpenGLShader::Vertex, edgeVertex) ||
        !edges->addShaderFromSourceCode(QOpenGLShader::Fragment, edgeFragment) || !edges->link())
      return false;
    shapeEdgeAt.box = edges->uniformLocation("box");
    std::unique_ptr<QOpenGLShaderProgram> fills[2];
    for (int summed = 0; summed < 2; ++summed) {
      auto &fill = fills[summed];
      fill = std::make_unique<QOpenGLShaderProgram>();
      if (!fill->addShaderFromSourceCode(QOpenGLShader::Vertex, fillVertex) ||
          !fill->addShaderFromSourceCode(QOpenGLShader::Fragment, fillFragment(summed == 1)) || !fill->link())
        return false;
      shapeFillAt[summed] = {fill->uniformLocation("box"), fill->uniformLocation("device"),
                             fill->uniformLocation("tables"), fill->uniformLocation("bases"),
                             fill->uniformLocation("evenOdd"), fill->uniformLocation("cutRule"),
                             fill->uniformLocation("kind"), fill->uniformLocation("colour"),
                             fill->uniformLocation("toBrush"), fill->uniformLocation("geometry")};
      fill->bind();
      fill->setUniformValue(fill->uniformLocation("area"), 0);
      fill->setUniformValue(fill->uniformLocation("cut"), 1);
      fill->setUniformValue(fill->uniformLocation("table"), 2);
      fill->setUniformValue(fill->uniformLocation("edgeList"), 3);
      fill->setUniformValue(fill->uniformLocation("rowList"), 4);
      fill->release();
    }
    auto *gl = context.extraFunctions();
    gl->glGenVertexArrays(1, &shapeVao);
    gl->glGenBuffers(1, &shapeEdgeBuffer);
    edgeRing.buffer = shapeEdgeBuffer;
    gl->glBindVertexArray(shapeVao);
    gl->glBindBuffer(GL_ARRAY_BUFFER, shapeEdgeBuffer);
    gl->glBufferData(GL_ARRAY_BUFFER, 16, nullptr, GL_STREAM_DRAW);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, nullptr);
    gl->glVertexAttribDivisor(0, 1);
    gl->glBindVertexArray(0);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    // The same edges read by the fill, four floats a texel.
    gl->glGenTextures(1, &shapeEdgeList);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_BUFFER, shapeEdgeList);
    gl->glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, shapeEdgeBuffer);
    // The row tables, one integer a texel.
    gl->glGenBuffers(1, &shapeRowBuffer);
    rowRing.buffer = shapeRowBuffer;
    gl->glBindBuffer(GL_TEXTURE_BUFFER, shapeRowBuffer);
    gl->glBufferData(GL_TEXTURE_BUFFER, 16, nullptr, GL_STREAM_DRAW);
    gl->glBindBuffer(GL_TEXTURE_BUFFER, 0);
    gl->glGenTextures(1, &shapeRowList);
    gl->glBindTexture(GL_TEXTURE_BUFFER, shapeRowList);
    gl->glTexBuffer(GL_TEXTURE_BUFFER, GL_R32I, shapeRowBuffer);
    gl->glBindTexture(GL_TEXTURE_BUFFER, 0);
    gl->glGenTextures(1, &shapeTable);
    gl->glBindTexture(GL_TEXTURE_2D, shapeTable);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    shapeEdgeProgram = std::move(edges);
    shapeFillProgram[0] = std::move(fills[0]);
    shapeFillProgram[1] = std::move(fills[1]);
    // The float targets made and checked now: a driver that will not draw
    // into one leaves shapes to Qt's engine, as before.
    if (!shapeAreasFor(QSize(256, 256))) return false;
    shapesReady = true;
    return true;
  }
  // The float targets areas are added into, grown when a shape needs more.
  // Grown rarely, so their framebuffers are checked as they are made.
  bool shapeAreasFor(QSize size) {
    if (shapeArea[0] && shapeAreaSize.width() >= size.width() && shapeAreaSize.height() >= size.height()) return true;
    const QSize grown = size.expandedTo(shapeAreaSize);
    auto *gl = context.extraFunctions();
    gl->glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < 2; ++i) {
      if (!shapeArea[i]) gl->glGenTextures(1, &shapeArea[i]);
      if (!shapeAreaFbo[i]) gl->glGenFramebuffers(1, &shapeAreaFbo[i]);
      gl->glBindTexture(GL_TEXTURE_2D, shapeArea[i]);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, grown.width(), grown.height(), 0, GL_RED, GL_FLOAT, nullptr);
      gl->glBindFramebuffer(GL_FRAMEBUFFER, shapeAreaFbo[i]);
      gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shapeArea[i], 0);
      if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        return false;
      }
    }
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    shapeAreaSize = grown;
    return true;
  }
  // A shape's outline as QOutlineMapper hands it on, still in the shape's own
  // units: subpaths closed, curves cut into lines. Kept by what the path holds
  // and how finely it is cut, so the motion-blur samples of a moving shape --
  // each its path built anew -- and the frames of a still one cut it once.
  struct FlatOutline { std::vector<QPointF> points; std::vector<int> starts; int elements = 0; QPointF first, last; quint64 used = 0; };
  std::unordered_map<size_t, FlatOutline> flatOutlines;
  quint64 flatClock = 0;
  qint64 flatPoints = 0;
  // `content` is what the path holds, hashed: worked out here when it is 0,
  // and kept by the caller (ShapeFill::pathKey) for the next time.
  const FlatOutline &flatOutline(const QPainterPath &path, qreal threshold, size_t &content) {
    const int count = path.elementCount();
    if (!content) {
      size_t hash = qHashMulti(0, count);
      for (int i = 0; i < count; ++i) {
        const QPainterPath::Element e = path.elementAt(i);
        hash = qHashMulti(hash, e.x, e.y, int(e.type));
      }
      content = hash ? hash : 1;
    }
    const size_t key = qHashMulti(content, threshold);
    const QPointF first = count ? QPointF(path.elementAt(0)) : QPointF();
    const QPointF last = count ? QPointF(path.elementAt(count - 1)) : QPointF();
    if (auto found = flatOutlines.find(key);
        found != flatOutlines.end() && found->second.elements == count && found->second.first == first &&
        found->second.last == last) {
      found->second.used = ++flatClock;
      return found->second;
    }
    // Kept within about four million points, those used longest ago going first.
    if (flatPoints > (4ll << 20) || flatOutlines.size() > 4096) {
      std::vector<std::pair<quint64, size_t>> ages;
      for (const auto &entry : flatOutlines) ages.push_back({entry.second.used, entry.first});
      std::sort(ages.begin(), ages.end());
      for (size_t i = 0; i < ages.size() / 2; ++i) {
        auto old = flatOutlines.find(ages[i].second);
        flatPoints -= qint64(old->second.points.size());
        flatOutlines.erase(old);
      }
    }
    FlatOutline flat;
    flat.elements = count;
    flat.first = first;
    flat.last = last;
    std::vector<QPointF> &points = flat.points;
    auto close = [&] {
      if (!points.empty() && !flat.starts.empty() && points.back() != points[size_t(flat.starts.back())])
        points.push_back(points[size_t(flat.starts.back())]);
    };
    for (int i = 0; i < count; ++i) {
      const QPainterPath::Element e = path.elementAt(i);
      if (e.type == QPainterPath::MoveToElement) {
        if (i == count - 1) continue;
        close();
        flat.starts.push_back(int(points.size()));
        points.push_back(QPointF(e.x, e.y));
      } else if (e.type == QPainterPath::LineToElement) {
        points.push_back(QPointF(e.x, e.y));
      } else if (e.type == QPainterPath::CurveToElement) {
        if (!points.empty() && i + 2 < count)
          flattenCurve(points, points.back(), QPointF(e.x, e.y), QPointF(path.elementAt(i + 1)),
                       QPointF(path.elementAt(i + 2)), threshold);
        i += 2;
      }
    }
    close();
    flat.used = ++flatClock;
    flatPoints += qint64(points.size());
    return flatOutlines[key] = std::move(flat);
  }
  // An outline's edges on the device as the rasterizer has them: each point
  // carried by QTransform::map and rounded to 1/64 of a pixel, each contour
  // closed, level edges left out (they cover nothing). Added to shapeEdges;
  // where they start there, how many, and the pixels they reach. False when
  // it reaches so far off that a float would lose its 64ths.
  struct EdgeRun { int first = 0, count = 0; QRect box; bool winding = false; };
  bool edgesOf(const QPainterPath &path, size_t &content, const QTransform &toDevice, EdgeRun &run) {
    run = EdgeRun();
    run.first = int(shapeEdges.size() / 4);
    run.winding = path.fillRule() == Qt::WindingFill;
    const qreal scale = outlineScale(toDevice);
    const FlatOutline &flat = flatOutline(path, scale == 0 ? qreal(0.25) : qreal(0.25) / scale, content);
    if (flat.points.empty()) return true;
    // Each point carried by QTransform::map's own sums, type by type, so it
    // lands on the very same double. Called into Qt's library point by point,
    // that took 9 of the 26 us a ring of the promo cost (775 edges).
    const QTransform::TransformationType type = toDevice.type();
    const qreal m11 = toDevice.m11(), m12 = toDevice.m12(), m21 = toDevice.m21(), m22 = toDevice.m22(),
                dx = toDevice.dx(), dy = toDevice.dy();
    constexpr qreal reach = 1 << 17;
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    bool held = true;
    auto fixed = [&](const QPointF &local, int &fx, int &fy) {
      const qreal lx = local.x(), ly = local.y();
      qreal x, y;
      switch (type) {
      case QTransform::TxNone: x = lx; y = ly; break;
      case QTransform::TxTranslate: x = lx + dx; y = ly + dy; break;
      case QTransform::TxScale: x = m11 * lx + dx; y = m22 * ly + dy; break;
      default: x = m11 * lx + m21 * ly + dx; y = m12 * lx + m22 * ly + dy; break;
      }
      if (!(qAbs(x) < reach && qAbs(y) < reach)) { held = false; fx = fy = 0; return; }
      fx = qRound(x * 64);
      fy = qRound(y * 64);
      x0 = std::min(x0, fx); y0 = std::min(y0, fy); x1 = std::max(x1, fx); y1 = std::max(y1, fy);
    };
    // Room for as many edges as there are points (the closing ones among
    // them), written in place and cut back after; each edge's rows beside it.
    const size_t before = shapeEdges.size();
    shapeEdges.resize(before + flat.points.size() * 4);
    shapeEdgeRows.resize((size_t(run.first) + flat.points.size()) * 2);
    float *edge = shapeEdges.data() + before;
    qint32 *rows = shapeEdgeRows.data() + size_t(run.first) * 2;
    const size_t contours = std::max<size_t>(flat.starts.size(), 1);
    for (size_t c = 0; c < contours && held; ++c) {
      const size_t from = flat.starts.empty() ? 0 : size_t(flat.starts[c]);
      const size_t to = c + 1 < flat.starts.size() ? size_t(flat.starts[c + 1]) : flat.points.size();
      if (to <= from) continue;
      int startX, startY;
      fixed(flat.points[from], startX, startY);
      int fromX = startX, fromY = startY;
      for (size_t i = from + 1; i <= to; ++i) {
        int toX = startX, toY = startY; // past the last point, the rasterizer closes the contour
        if (i < to) fixed(flat.points[i], toX, toY);
        if (toY != fromY) {
          edge[0] = fromX / 64.f; edge[1] = fromY / 64.f; edge[2] = toX / 64.f; edge[3] = toY / 64.f;
          edge += 4;
          // The row its top lies in and the one past its bottom's, from the
          // 64ths (an arithmetic shift floors): std::floor and std::ceil on
          // the floats took 5.6 us more a ring.
          rows[0] = std::min(fromY, toY) >> 6;
          rows[1] = (std::max(fromY, toY) + 63) >> 6;
          rows += 2;
          ++run.count;
        }
        fromX = toX; fromY = toY;
      }
    }
    shapeEdges.resize(before + size_t(run.count) * 4);
    shapeEdgeRows.resize((size_t(run.first) + size_t(run.count)) * 2);
    if (!held) return false;
    auto floorDiv = [](int v) { return v >= 0 ? v / 64 : -((-v + 63) / 64); };
    auto ceilDiv = [](int v) { return v >= 0 ? (v + 63) / 64 : -((-v) / 64); };
    if (x1 >= x0 && y1 >= y0) {
      const int left = floorDiv(x0), top = floorDiv(y0), right = ceilDiv(x1), bottom = ceilDiv(y1);
      if (right > left && bottom > top) run.box = QRect(left, top, right - left, bottom - top);
    }
    return true;
  }
  // The rows of `box` each edge of a run crosses, from and past the last, two
  // to an edge; how many edges its pixels sum between them, row by row.
  qint64 rowSpans(const EdgeRun &run, const QRect &box, std::vector<qint32> &spans) const {
    spans.resize(size_t(run.count) * 2);
    qint64 entries = 0;
    const qint32 *rows = shapeEdgeRows.data() + size_t(run.first) * 2;
    const int top = box.top(), height = box.height();
    for (size_t i = 0; i < size_t(run.count) * 2; i += 2) {
      const int from = std::max(rows[i] - top, 0), to = std::min(rows[i + 1] - top, height);
      spans[i] = from;
      spans[i + 1] = to;
      entries += std::max(to - from, 0);
    }
    return entries;
  }
  // A run's row table in shapeRows: for each row of the box where the indices
  // of the edges crossing it start (and, one more, where the last row's end),
  // then those indices row by row. Where it starts; `most` raised to the
  // most edges a row of it holds.
  int rowTable(const EdgeRun &run, const QRect &box, const std::vector<qint32> &spans, int &most) {
    const int rows = box.height(), table = int(shapeRows.size());
    rowCursors.assign(size_t(rows) + 1, 0);
    for (int i = 0; i < run.count; ++i) {
      const qint32 from = spans[size_t(i) * 2], to = spans[size_t(i) * 2 + 1];
      if (to <= from) continue;
      ++rowCursors[size_t(from)];
      --rowCursors[size_t(to)];
    }
    shapeRows.resize(size_t(table) + size_t(rows) + 1);
    qint32 at = qint32(table + rows + 1), crossing = 0;
    for (int r = 0; r <= rows; ++r) {
      crossing += rowCursors[size_t(r)];
      shapeRows[size_t(table + r)] = at;
      rowCursors[size_t(r)] = at;
      if (r < rows) {
        at += crossing;
        most = std::max(most, int(crossing));
      }
    }
    shapeRows.resize(size_t(at));
    for (int i = 0; i < run.count; ++i) {
      const qint32 from = spans[size_t(i) * 2], to = spans[size_t(i) * 2 + 1];
      for (qint32 r = from; r < to; ++r) shapeRows[size_t(rowCursors[size_t(r)]++)] = run.first + i;
    }
    return table;
  }
  bool shapes(QPainter &p, const ShapeFills &fills) {
    // What Qt's engine keeps drawing: another blend than Normal, a projection,
    // a brush other than a colour or a plain gradient, and a clip that is not
    // one rectangle on the device (a layer's mask, which Qt keeps in the
    // stencil).
    if (p.compositionMode() != QPainter::CompositionMode_SourceOver) return false;
    const QTransform toDevice = p.combinedTransform();
    if (toDevice.type() >= QTransform::TxProject) return false;
    for (const ShapeFill &fill : fills) {
      const Qt::BrushStyle style = fill.brush.style();
      if (style == Qt::NoBrush || style == Qt::SolidPattern) continue;
      if (style != Qt::LinearGradientPattern && style != Qt::RadialGradientPattern) return false;
      const QGradient *g = fill.brush.gradient();
      if (g->coordinateMode() != QGradient::LogicalMode || g->spread() != QGradient::PadSpread) return false;
      if (style == Qt::RadialGradientPattern) {
        const auto *radial = static_cast<const QRadialGradient *>(g);
        if (radial->focalPoint() != radial->center() || radial->focalRadius() != 0) return false;
      }
    }
    QRect clip;
    if (!passClipIsDeviceRect(p, &clip)) return false;
    QPaintDevice *device = p.device();
    const QSize size(device->width(), device->height());
    clip &= QRect(QPoint(), size);
    if (clip.isEmpty()) return true;
    // Every fill's edges and its cut's, gathered for one upload.
    struct Work { EdgeRun shape, cut; QRect box; const ShapeFill *fill; bool direct; int shapeTable, cutTable, most; };
    std::vector<Work> work;
    work.reserve(fills.size());
    shapeEdges.clear();
    shapeEdgeRows.clear();
    shapeRows.clear();
    for (const ShapeFill &fill : fills) {
      if (fill.brush.style() == Qt::NoBrush) continue;
      Work w;
      w.fill = &fill;
      if (!edgesOf(fill.path, fill.pathKey, toDevice, w.shape)) return false;
      w.box = w.shape.box & clip;
      if (fill.clipped) {
        if (!edgesOf(fill.clip, fill.clipKey, toDevice, w.cut)) return false;
        w.box &= w.cut.box;
      }
      if (w.box.isEmpty() || !w.shape.count) continue;
      // A shape's area is summed in the fill itself, each pixel over the
      // edges crossing its row, and the shape laid straight into the frame in
      // one pass. Added up in the float target, each shape turned the card to
      // it and back (and to its cut's): 600 small shapes took 2.2 ms a frame
      // more than Qt's engine; summed over all its edges at every pixel, 4.6.
      // Only a huge one, whose rows hold so many edges that every pixel would
      // sum dozens, goes to the target: each edge there covers its rows once.
      const qint64 entries = rowSpans(w.shape, w.box, shapeSpans) + (fill.clipped ? rowSpans(w.cut, w.box, cutSpans) : 0);
      w.direct = qint64(w.box.width()) * entries <= kShapeSummedAtMost;
      w.most = 0;
      w.shapeTable = w.direct ? rowTable(w.shape, w.box, shapeSpans, w.most) : -1;
      w.cutTable = w.direct && fill.clipped ? rowTable(w.cut, w.box, cutSpans, w.most) : -1;
      work.push_back(w);
    }
    if (work.empty()) return true;
    const double opacity = p.opacity();
    p.beginNativePainting();
    struct EndNative {
      QPainter &p;
      ~EndNative() { p.endNativePainting(); }
    } endNative{p};
    auto *gl = context.extraFunctions();
    // Where the frame is drawn, known before any target of these is bound.
    const DrawTarget frame = drawTarget(device);
    const bool first = !shapesAttempted;
    if (!prepareShapes()) {
      gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(frame.fbo));
      return false;
    }
    clearBeforeStep(gl);
    QSize most;
    for (const Work &w : work)
      if (!w.direct) most = most.expandedTo(w.box.size());
    const QSize areasBefore = shapeAreaSize;
    if (!most.isEmpty() && !shapeAreasFor(most)) {
      gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(frame.fbo));
      return false;
    }
    // Only what this call uses is set: a shape is drawn with some forty
    // calls to the driver, and 600 of them a frame were worth trimming.
    const bool added = !most.isEmpty();
    const bool graded = std::any_of(work.begin(), work.end(),
                                    [](const Work &w) { return w.fill->brush.style() != Qt::SolidPattern; });
    gl->glDisable(GL_CULL_FACE);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl->glEnable(GL_SCISSOR_TEST);
    gl->glEnable(GL_BLEND);
    gl->glBlendEquation(GL_FUNC_ADD);
    if (added) gl->glClearColor(0, 0, 0, 0);
    const GLint edgeBase = GLint(stream(edgeRing, GL_ARRAY_BUFFER, shapeEdges.data(),
                                        qint64(shapeEdges.size() * sizeof(float)), 16));
    const GLint rowBase = shapeRows.empty() ? 0
                          : GLint(stream(rowRing, GL_TEXTURE_BUFFER, shapeRows.data(),
                                         qint64(shapeRows.size() * sizeof(qint32)), 4));
    gl->glBindBuffer(GL_TEXTURE_BUFFER, 0);
    if (added) {
      gl->glBindBuffer(GL_ARRAY_BUFFER, shapeEdgeBuffer);
      for (int i = 0; i < 2; ++i) {
        gl->glActiveTexture(GL_TEXTURE0 + i);
        gl->glBindTexture(GL_TEXTURE_2D, shapeArea[i]);
      }
    }
    gl->glActiveTexture(GL_TEXTURE3);
    gl->glBindTexture(GL_TEXTURE_BUFFER, shapeEdgeList);
    gl->glActiveTexture(GL_TEXTURE4);
    gl->glBindTexture(GL_TEXTURE_BUFFER, shapeRowList);
    if (graded) {
      gl->glActiveTexture(GL_TEXTURE2);
      gl->glBindTexture(GL_TEXTURE_2D, shapeTable);
    }
    QOpenGLShaderProgram &edges = *shapeEdgeProgram;
    // The frame's target, bound as beginNativePainting left it unless this
    // call has just made or grown a target of its own; and which form of the
    // fill is bound (-1 none), with its blend and its vertex array.
    bool onFrame = !first && shapeAreaSize == areasBefore;
    int bound = -1;
    for (const Work &w : work) {
      const QRect box = w.box;
      const QVector4D area(box.left(), box.top(), box.width(), box.height());
      if (!w.direct) {
        auto addUp = [&](const EdgeRun &run, int target) {
          gl->glBindFramebuffer(GL_FRAMEBUFFER, shapeAreaFbo[target]);
          gl->glViewport(0, 0, box.width(), box.height());
          gl->glScissor(0, 0, box.width(), box.height());
          gl->glClear(GL_COLOR_BUFFER_BIT);
          if (!run.count) return;
          gl->glBlendFunc(GL_ONE, GL_ONE);
          edges.bind();
          edges.setUniformValue(shapeEdgeAt.box, area);
          gl->glBindVertexArray(shapeVao);
          gl->glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16,
                                    reinterpret_cast<const void *>((qintptr(edgeBase) + run.first) * 16));
          gl->glDrawArraysInstanced(GL_TRIANGLES, 0, 6, run.count);
        };
        addUp(w.shape, 0);
        if (w.fill->clipped) addUp(w.cut, 1);
        onFrame = false;
        bound = -1;
      }
      if (!onFrame) {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(frame.fbo));
        gl->glViewport(frame.viewport[0], frame.viewport[1], frame.viewport[2], frame.viewport[3]);
        onFrame = true;
      }
      const int form = w.direct ? 1 : 0;
      QOpenGLShaderProgram &fill = *shapeFillProgram[form];
      const FillAt &fillAt = shapeFillAt[form];
      if (bound != form) {
        gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        fill.bind();
        fill.setUniformValue(fillAt.device, QVector2D(size.width(), size.height()));
        const GLint bases[2] = {edgeBase, rowBase};
        gl->glUniform2iv(fillAt.bases, 1, bases);
        gl->glBindVertexArray(vao);
        bound = form;
      }
      gl->glScissor(box.left(), size.height() - box.bottom() - 1, box.width(), box.height());
      fill.setUniformValue(fillAt.box, area);
      if (w.direct) {
        const GLint tables[4] = {w.shapeTable, w.cutTable, w.most, 0};
        gl->glUniform4iv(fillAt.tables, 1, tables);
      }
      fill.setUniformValue(fillAt.evenOdd, w.shape.winding ? 0 : 1);
      fill.setUniformValue(fillAt.cutRule, w.fill->clipped ? (w.cut.winding ? 1 : 2) : 0);
      const QBrush &brush = w.fill->brush;
      int kind = 0;
      if (brush.style() == Qt::SolidPattern) {
        fill.setUniformValue(fillAt.colour, rasterColour(brush.color(), opacity));
      } else {
        const QGradient *g = brush.gradient();
        bool invertible = false;
        const QTransform toBrush = (brush.transform() * toDevice).inverted(&invertible);
        kind = !invertible ? 0 : g->type() == QGradient::LinearGradient ? 1 : 2;
        const float m[9] = {float(toBrush.m11()), float(toBrush.m21()), float(toBrush.m31()),
                            float(toBrush.m12()), float(toBrush.m22()), float(toBrush.m32()),
                            float(toBrush.m13()), float(toBrush.m23()), float(toBrush.m33())};
        fill.setUniformValue(fillAt.toBrush, QMatrix3x3(m));
        if (kind == 1) {
          const auto *linear = static_cast<const QLinearGradient *>(g);
          fill.setUniformValue(fillAt.geometry, QVector4D(linear->start().x(), linear->start().y(),
                                                          linear->finalStop().x(), linear->finalStop().y()));
        } else if (kind == 2) {
          const auto *radial = static_cast<const QRadialGradient *>(g);
          fill.setUniformValue(fillAt.geometry, QVector4D(radial->center().x(), radial->center().y(), radial->radius(), 0));
        }
        const QImage table = gradientTable(*g, opacity);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 1, GL_RGBA, GL_UNSIGNED_BYTE, table.constBits());
        if (kind == 0) fill.setUniformValue(fillAt.colour, rasterColour(brush.color(), opacity));
      }
      fill.setUniformValue(fillAt.kind, kind);
      gl->glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    gl->glUseProgram(0);
    // Units 0 to 2 as Qt's engine found them; it never reads a buffer texture.
    if (graded) {
      gl->glActiveTexture(GL_TEXTURE2);
      gl->glBindTexture(GL_TEXTURE_2D, 0);
    }
    if (added)
      for (int i = 1; i >= 0; --i) {
        gl->glActiveTexture(GL_TEXTURE0 + i);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
      }
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glBindVertexArray(0);
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_SCISSOR_TEST);
    if (!stepWithoutError(gl)) return false;
    shapeFills += work.size();
    shapeFillsAddedUp += quint64(std::count_if(work.begin(), work.end(), [](const Work &w) { return !w.direct; }));
    return true;
  }
  // A lit 3D layer's face (lights.h), all on the card. The face is painted by
  // the compositor's own code into a multisampled target the size of the
  // frame, its top left where the face's rectangle starts; every pixel of it
  // is then lit at the point of the layer's plane it shows -- lightAt(), line
  // for line, shadows included -- and laid into the frame. The CPU works the
  // light out every fourth pixel and blends between where it bends gently,
  // within half an 8-bit step; here every pixel is worked out.
  std::unique_ptr<QOpenGLFramebufferObject> faceSample, faceResolved;
  std::unique_ptr<CompositionPaintDevice> facePainted; // paints into faceSample, kept with it
  std::unique_ptr<QOpenGLShaderProgram> litProgram;
  bool litAttempted = false;
  GLuint shadowAtlas = 0;
  QByteArray shadowAtlasKey;
  static constexpr int kLitLights = 8, kLitCasters = 16;
  // Each light's and each caster's own, element by element, as they were
  // asked for by name ("kind[2]").
  struct LitUniforms {
    GLint area = -1, face = -1, atlas = -1, deviceHeight = -1, dirtyCorner = -1, toLocal = -1, corner = -1, across = -1,
          down = -1, normal = -1, eye = -1, shine = -1, gloss = -1, self = -1, lightCount = -1;
    GLint kind[kLitLights], falloff[kLitLights], position[kLitLights], direction[kLitLights], colour[kLitLights],
        coneOuter[kLitLights], coneInner[kLitLights], radius[kLitLights], reach[kLitLights], darkness[kLitLights],
        firstCaster[kLitLights], casterCount[kLitLights];
    GLint casterLayer[kLitCasters], casterPoint[kLitCasters], casterNormal[kLitCasters], casterToLocal[kLitCasters],
        casterBounds[kLitCasters], casterMap[kLitCasters], casterResolution[kLitCasters], casterOpacity[kLitCasters];
    LitUniforms() = default;
    explicit LitUniforms(QOpenGLShaderProgram &p) {
      auto at = [&](const char *name) { return p.uniformLocation(name); };
      area = at("area"); face = at("face"); atlas = at("atlas"); deviceHeight = at("deviceHeight");
      dirtyCorner = at("dirtyCorner"); toLocal = at("toLocal"); corner = at("corner"); across = at("across");
      down = at("down"); normal = at("normal"); eye = at("eye"); shine = at("shine"); gloss = at("gloss");
      self = at("self"); lightCount = at("lightCount");
      for (int i = 0; i < kLitLights; ++i) {
        auto element = [&](const char *name) {
          return at((QByteArray(name) + '[' + QByteArray::number(i) + ']').constData());
        };
        kind[i] = element("kind"); falloff[i] = element("falloff"); position[i] = element("position");
        direction[i] = element("direction"); colour[i] = element("colour"); coneOuter[i] = element("coneOuter");
        coneInner[i] = element("coneInner"); radius[i] = element("radius"); reach[i] = element("reach");
        darkness[i] = element("darkness"); firstCaster[i] = element("firstCaster"); casterCount[i] = element("casterCount");
      }
      for (int i = 0; i < kLitCasters; ++i) {
        auto element = [&](const char *name) {
          return at((QByteArray(name) + '[' + QByteArray::number(i) + ']').constData());
        };
        casterLayer[i] = element("casterLayer"); casterPoint[i] = element("casterPoint");
        casterNormal[i] = element("casterNormal"); casterToLocal[i] = element("casterToLocal");
        casterBounds[i] = element("casterBounds"); casterMap[i] = element("casterMap");
        casterResolution[i] = element("casterResolution"); casterOpacity[i] = element("casterOpacity");
      }
    }
  } litAt;
  bool prepareLit() {
    if (litAttempted) return bool(litProgram);
    litAttempted = true;
    const char *vertex = R"(#version 330 core
      uniform vec4 area; // x, y, width, height in clip space
      void main() {
        const vec2 corner[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(1,0),vec2(1,1),vec2(0,1));
        gl_Position = vec4(area.xy + corner[gl_VertexID] * area.zw, 0.0, 1.0);
      })";
    const char *fragment = R"(#version 330 core
      uniform sampler2D face;
      uniform sampler2D atlas;
      uniform int deviceHeight;
      uniform ivec2 dirtyCorner;
      uniform mat3 toLocal;          // device pixels to the layer's own plane
      uniform vec3 corner, across, down, normal, eye;
      uniform float shine, gloss;
      uniform int self, lightCount;
      uniform int kind[8], falloff[8], firstCaster[8], casterCount[8];
      uniform vec3 position[8], direction[8], colour[8];
      uniform float coneOuter[8], coneInner[8], radius[8], reach[8], darkness[8];
      uniform int casterLayer[16];
      uniform vec3 casterPoint[16], casterNormal[16];
      uniform mat4 casterToLocal[16];
      uniform vec4 casterBounds[16];
      uniform ivec4 casterMap[16]; // where its map lies in the atlas: x, y, width, height
      uniform float casterResolution[16], casterOpacity[16];
      out vec4 color;
      float falloffAt(int i, float distance) {
        if (falloff[i] == 1) {
          if (distance <= radius[i]) return 1.0;
          float x = (distance - radius[i]) / max(1.0, reach[i]);
          return x >= 1.0 ? 0.0 : 1.0 - x * x * (3.0 - 2.0 * x);
        }
        if (falloff[i] == 2) {
          float r = max(1.0, radius[i]);
          return distance <= r ? 1.0 : (r / distance) * (r / distance);
        }
        return 1.0;
      }
      float coneAt(int i, float cosine) {
        if (cosine <= coneOuter[i]) return 0.0;
        if (cosine >= coneInner[i]) return 1.0;
        float x = (cosine - coneOuter[i]) / (coneInner[i] - coneOuter[i]);
        return x * x * (3.0 - 2.0 * x);
      }
      float mapAt(int c, int px, int py) {
        if (px < 0 || py < 0 || px >= casterMap[c].z || py >= casterMap[c].w) return 0.0;
        return texelFetch(atlas, casterMap[c].xy + ivec2(px, py), 0).r;
      }
      float coverageAt(int c, float u, float v) {
        float x = (u - casterBounds[c].x) * casterResolution[c] - 0.5, y = (v - casterBounds[c].y) * casterResolution[c] - 0.5;
        if (x <= -1.0 || y <= -1.0 || x >= float(casterMap[c].z) || y >= float(casterMap[c].w)) return 0.0;
        int x0 = int(floor(x)), y0 = int(floor(y));
        float fx = x - float(x0), fy = y - float(y0);
        return (mapAt(c, x0, y0) * (1.0 - fx) + mapAt(c, x0 + 1, y0) * fx) * (1.0 - fy) +
               (mapAt(c, x0, y0 + 1) * (1.0 - fx) + mapAt(c, x0 + 1, y0 + 1) * fx) * fy;
      }
      float getsThrough(int i, vec3 point) {
        if (casterCount[i] == 0) return 1.0;
        vec3 toward = kind[i] == 2 ? -direction[i] * 1e7 : position[i] - point;
        float through = 1.0;
        for (int k = 0; k < casterCount[i]; ++k) {
          int c = firstCaster[i] + k;
          if (casterLayer[c] == self) continue;
          float along = dot(toward, casterNormal[c]);
          if (abs(along) < 1e-9) continue;
          float at = dot(casterPoint[c] - point, casterNormal[c]) / along;
          if (at <= 1e-5 || at >= 1.0 - 1e-5) continue;
          vec4 local = casterToLocal[c] * vec4(point + toward * at, 1.0);
          float covered = coverageAt(c, local.x, local.y);
          if (covered <= 0.0) continue;
          through *= 1.0 - covered * casterOpacity[c] * darkness[i];
          if (through <= 1e-4) return 0.0;
        }
        return through;
      }
      void main() {
        // The frame's top row first, as the device counts them; the face was
        // painted by Qt bottom row first, as every target it paints is.
        int x = int(gl_FragCoord.x), y = deviceHeight - 1 - int(gl_FragCoord.y);
        vec4 p = texelFetch(face, ivec2(x - dirtyCorner.x, deviceHeight - 1 - (y - dirtyCorner.y)), 0);
        if (p.a <= 0.0) discard;
        vec3 d = toLocal * vec3(float(x) + 0.5, float(y) + 0.5, 1.0);
        float w = abs(d.z) < 1e-12 ? 1e-12 : d.z;
        vec3 point = corner + across * (d.x / w) + down * (d.y / w);
        vec3 multiply = vec3(0.0), add = vec3(0.0);
        vec3 toEye = eye - point;
        float eyeDistance = length(toEye);
        vec3 v = eyeDistance > 1e-6 ? toEye / eyeDistance : vec3(0.0, 0.0, -1.0);
        vec3 n = normal;
        if (dot(n, v) < 0.0) n = -n;
        float exponent = 2.0 + gloss * gloss * 254.0;
        for (int i = 0; i < lightCount; ++i) {
          if (kind[i] == 3) { multiply += colour[i]; continue; }
          vec3 l;
          float strength = 1.0;
          if (kind[i] == 2) l = -direction[i];
          else {
            vec3 toLight = position[i] - point;
            float distance = length(toLight);
            if (distance < 1e-6) continue;
            l = toLight / distance;
            strength = falloffAt(i, distance);
            if (kind[i] == 1) strength *= coneAt(i, -dot(l, direction[i]));
            if (strength <= 0.0) continue;
          }
          float diffuse = dot(n, l);
          if (diffuse <= 0.0) continue;
          strength *= getsThrough(i, point);
          if (strength <= 0.0) continue;
          multiply += colour[i] * (diffuse * strength);
          if (shine > 0.0) {
            vec3 h = l + v;
            float hl = length(h);
            if (hl > 1e-6) {
              float nh = dot(n, h) / hl;
              if (nh > 0.0) add += colour[i] * (shine * pow(nh, exponent) * strength);
            }
          }
        }
        color = vec4(clamp(p.rgb * multiply + p.a * add, vec3(0.0), vec3(p.a)), p.a);
      })";
    auto program = std::make_unique<QOpenGLShaderProgram>();
    if (program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) &&
        program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) && program->link()) {
      litAt = LitUniforms(*program);
      litProgram = std::move(program);
    }
    return bool(litProgram);
  }
  bool litFace(QPainter &p, QRect dirty, const std::function<void(QPainter &)> &paintFace,
               const QTransform &localToDevice, const QMatrix4x4 &world, const SceneLighting &lighting,
               float shine, float gloss, int self) {
    if (!effectsReady || !vao || !p.device() || !prepareLit()) return false;
    if (p.compositionMode() != QPainter::CompositionMode_SourceOver) return false;
    QRect clip;
    if (!painterClipIsDeviceRect(p, &clip)) return false;
    if (int(lighting.lights.size()) > kLitLights) return false;
    std::vector<const ShadowCaster *> casters;
    for (const auto &light : lighting.lights)
      for (const auto &caster : light.casters) casters.push_back(&caster);
    if (int(casters.size()) > kLitCasters) return false;
    bool invertible = false;
    const QTransform toLocal = localToDevice.inverted(&invertible);
    if (!invertible) return false;
    const QMatrix3x3 normals = world.normalMatrix();
    QVector3D normal(normals(0, 2), normals(1, 2), normals(2, 2));
    if (normal.lengthSquared() < 1e-20f) return false;
    normal.normalize();
    const QSize device(p.device()->width(), p.device()->height());
    dirty &= QRect(QPoint(), device);
    if (dirty.isEmpty()) return true;
    auto *gl = context.extraFunctions();
    // The shadow maps, stacked down one texture; built again only when they change.
    QByteArray atlasKey;
    int atlasWidth = 1, atlasHeight = 0;
    for (const ShadowCaster *c : casters) {
      atlasKey += QByteArray::number(c->map.cacheKey()) + ',';
      atlasWidth = std::max(atlasWidth, c->map.width());
      atlasHeight += c->map.height();
    }
    if (atlasWidth > maxTextureSize || atlasHeight > maxTextureSize) return false;
    p.beginNativePainting();
    struct EndNative {
      QPainter &p;
      ~EndNative() { p.endNativePainting(); }
    } endNative{p};
    clearBeforeStep(gl);
    const GLint frameFbo = drawTarget(p.device()).fbo;
    if (!casters.empty() && atlasKey != shadowAtlasKey) {
      // Kept for later frames, so its upload is checked on its own; one that
      // failed is made again next time.
      setErrorsAside(gl);
      shadowAtlasKey.clear();
      if (!shadowAtlas) gl->glGenTextures(1, &shadowAtlas);
      gl->glActiveTexture(GL_TEXTURE1);
      gl->glBindTexture(GL_TEXTURE_2D, shadowAtlas);
      nearest(gl);
      gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlasWidth, atlasHeight, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
      gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      int row = 0;
      for (const ShadowCaster *c : casters) {
        const QImage map = c->map.format() == QImage::Format_Alpha8 ? c->map : c->map.convertToFormat(QImage::Format_Alpha8);
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(map.bytesPerLine()));
        gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, map.width(), map.height(), GL_RED, GL_UNSIGNED_BYTE, map.constBits());
        row += map.height();
      }
      gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      gl->glActiveTexture(GL_TEXTURE0);
      if (gl->glGetError() != GL_NO_ERROR) return false;
      shadowAtlasKey = atlasKey;
    }
    // 1. The face, painted by the compositor on the card.
    if (!faceSample || faceSample->size() != device) {
      facePainted.reset(); // it paints into the target about to go
      QOpenGLFramebufferObjectFormat format;
      format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
      format.setInternalTextureFormat(GL_RGBA8);
      format.setSamples(std::clamp(8, 0, std::max(0, int(maxSamples))));
      faceSample = std::make_unique<QOpenGLFramebufferObject>(device, format);
      if (!faceSample->isValid()) { format.setSamples(0); faceSample = std::make_unique<QOpenGLFramebufferObject>(device, format); }
      format.setSamples(0);
      format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
      faceResolved = std::make_unique<QOpenGLFramebufferObject>(device, format);
      if (!faceSample->isValid() || !faceResolved->isValid()) { faceSample.reset(); faceResolved.reset(); return false; }
      facePainted = std::make_unique<CompositionPaintDevice>(*faceSample);
    }
    clear(*faceSample);
    {
      QPainter q(facePainted.get());
      if (!q.isActive()) return false;
      paintFace(q);
    }
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, faceSample->handle());
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, faceResolved->handle());
    gl->glBlitFramebuffer(0, 0, device.width(), device.height(), 0, 0, device.width(), device.height(),
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // 2. Lit, pixel by pixel, into the frame.
    gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(frameFbo));
    gl->glViewport(0, 0, device.width(), device.height());
    litProgram->bind();
    // The face's rectangle in clip space, the frame's top row at the top.
    const float cx = 2.f * float(dirty.x()) / float(device.width()) - 1.f;
    const float cw = 2.f * float(dirty.width()) / float(device.width());
    const float cy = 1.f - 2.f * float(dirty.y() + dirty.height()) / float(device.height());
    const float ch = 2.f * float(dirty.height()) / float(device.height());
    const LitUniforms &at = litAt;
    litProgram->setUniformValue(at.area, QVector4D(cx, cy, cw, ch));
    litProgram->setUniformValue(at.face, 0);
    litProgram->setUniformValue(at.atlas, 1);
    litProgram->setUniformValue(at.deviceHeight, device.height());
    if (at.dirtyCorner >= 0) gl->glUniform2i(at.dirtyCorner, dirty.x(), dirty.y());
    const float local[9] = {float(toLocal.m11()), float(toLocal.m21()), float(toLocal.m31()),
                            float(toLocal.m12()), float(toLocal.m22()), float(toLocal.m32()),
                            float(toLocal.m13()), float(toLocal.m23()), float(toLocal.m33())};
    litProgram->setUniformValue(at.toLocal, QMatrix3x3(local));
    litProgram->setUniformValue(at.corner, world.column(3).toVector3D());
    litProgram->setUniformValue(at.across, world.column(0).toVector3D());
    litProgram->setUniformValue(at.down, world.column(1).toVector3D());
    litProgram->setUniformValue(at.normal, normal);
    litProgram->setUniformValue(at.eye, lighting.eye);
    litProgram->setUniformValue(at.shine, shine);
    litProgram->setUniformValue(at.gloss, gloss);
    litProgram->setUniformValue(at.self, self);
    litProgram->setUniformValue(at.lightCount, int(lighting.lights.size()));
    int casterIndex = 0, atlasRow = 0;
    for (int i = 0; i < int(lighting.lights.size()); ++i) {
      const SceneLight &light = lighting.lights[size_t(i)];
      litProgram->setUniformValue(at.kind[i], light.kind);
      litProgram->setUniformValue(at.falloff[i], light.falloff);
      litProgram->setUniformValue(at.position[i], light.position);
      litProgram->setUniformValue(at.direction[i], light.direction);
      litProgram->setUniformValue(at.colour[i], light.colour);
      litProgram->setUniformValue(at.coneOuter[i], light.coneOuter);
      litProgram->setUniformValue(at.coneInner[i], light.coneInner);
      litProgram->setUniformValue(at.radius[i], light.radius);
      litProgram->setUniformValue(at.reach[i], light.reach);
      litProgram->setUniformValue(at.darkness[i], light.shadowDarkness);
      litProgram->setUniformValue(at.firstCaster[i], casterIndex);
      litProgram->setUniformValue(at.casterCount[i], int(light.casters.size()));
      for (const ShadowCaster &c : light.casters) {
        const int k = casterIndex;
        litProgram->setUniformValue(at.casterLayer[k], c.layer);
        litProgram->setUniformValue(at.casterPoint[k], c.point);
        litProgram->setUniformValue(at.casterNormal[k], c.normal);
        litProgram->setUniformValue(at.casterToLocal[k], c.toLocal);
        litProgram->setUniformValue(at.casterBounds[k],
                                    QVector4D(float(c.bounds.left()), float(c.bounds.top()),
                                              float(c.bounds.width()), float(c.bounds.height())));
        if (at.casterMap[k] >= 0) gl->glUniform4i(at.casterMap[k], 0, atlasRow, c.map.width(), c.map.height());
        litProgram->setUniformValue(at.casterResolution[k], c.resolution);
        litProgram->setUniformValue(at.casterOpacity[k], c.opacity);
        atlasRow += c.map.height();
        ++casterIndex;
      }
    }
    gl->glActiveTexture(GL_TEXTURE1);
    gl->glBindTexture(GL_TEXTURE_2D, shadowAtlas);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, faceResolved->texture());
    nearest(gl);
    gl->glEnable(GL_BLEND);
    gl->glBlendEquation(GL_FUNC_ADD);
    gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDisable(GL_DEPTH_TEST); gl->glDisable(GL_STENCIL_TEST); gl->glDisable(GL_CULL_FACE);
    gl->glEnable(GL_SCISSOR_TEST);
    gl->glScissor(clip.left(), device.height() - clip.bottom() - 1, clip.width(), clip.height());
    gl->glBindVertexArray(vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 6);
    gl->glBindVertexArray(0);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glDisable(GL_BLEND);
    litProgram->release();
    if (!stepWithoutError(gl)) return false;
    ++litFaces;
    return true;
  }
  // A layer with a blend mode, flattened and blended on the card. The layer is
  // painted alone by the compositor's own code into a multisampled target the
  // size of the frame, where it lies in the frame -- its effects through the
  // card too -- and what lies beneath is copied out of the frame; one pass then
  // writes the blend of the two back over the layer's rectangle, by the same
  // arithmetic as Qt's raster blends (qcompositionfunctions.cpp), in float
  // where Qt works in 8-bit steps. One target per level of nesting: a blended
  // layer inside a blended group needs its own while the outer one is open.
  // `blur` sums the samples of a layer blurring itself by its own motion.
  // `painted`, the device that paints into `sample`, is kept with it (see
  // CompositionPaintDevice).
  struct BlendTargets {
    std::unique_ptr<QOpenGLFramebufferObject> sample, flat, beneath, matte, blur;
    std::unique_ptr<CompositionPaintDevice> painted;
  };
  std::vector<BlendTargets> blendTargets;
  int blendDepth = 0;
  std::unique_ptr<QOpenGLShaderProgram> blendProgram;
  struct BlendUniforms {
    GLint area = -1, layer = -1, beneath = -1, mode = -1, matte = -1, useMatte = -1, invertMatte = -1, weight = -1;
    BlendUniforms() = default;
    explicit BlendUniforms(QOpenGLShaderProgram &p)
        : area(p.uniformLocation("area")), layer(p.uniformLocation("layer")), beneath(p.uniformLocation("beneath")),
          mode(p.uniformLocation("mode")), matte(p.uniformLocation("matte")), useMatte(p.uniformLocation("useMatte")),
          invertMatte(p.uniformLocation("invertMatte")), weight(p.uniformLocation("weight")) {}
  } blendAt;
  bool blendAttempted = false;
  quint64 blendedLayers = 0, mattedLayers = 0, blurredLayers = 0;
  static int blendCode(QPainter::CompositionMode mode) {
    switch (mode) {
      case QPainter::CompositionMode_Multiply: return 1;
      case QPainter::CompositionMode_Screen: return 2;
      case QPainter::CompositionMode_Overlay: return 3;
      case QPainter::CompositionMode_Darken: return 4;
      case QPainter::CompositionMode_Lighten: return 5;
      case QPainter::CompositionMode_ColorDodge: return 6;
      case QPainter::CompositionMode_ColorBurn: return 7;
      case QPainter::CompositionMode_HardLight: return 8;
      case QPainter::CompositionMode_SoftLight: return 9;
      case QPainter::CompositionMode_Difference: return 10;
      case QPainter::CompositionMode_Exclusion: return 11;
      case QPainter::CompositionMode_Plus: return 12;
      default: return 0; // laid over, as Source Over is
    }
  }
  bool prepareBlend() {
    if (blendAttempted) return bool(blendProgram);
    blendAttempted = true;
    const char *vertex = R"(#version 330 core
      uniform vec4 area; // x, y, width, height in clip space
      void main() {
        const vec2 corner[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(1,0),vec2(1,1),vec2(0,1));
        gl_Position = vec4(area.xy + corner[gl_VertexID] * area.zw, 0.0, 1.0);
      })";
    const char *fragment = R"(#version 330 core
      uniform sampler2D layer, beneath, matte;
      uniform int mode;
      uniform bool useMatte, invertMatte;
      uniform float weight; // 1 laid into the frame; 1/n added into a sum of n
      out vec4 color;
      // One channel: s and d premultiplied, sa and da the alphas.
      float op(float s, float d, float sa, float da) {
        float temp = s * (1.0 - da) + d * (1.0 - sa);
        if (mode == 1) return s * d + temp;
        if (mode == 2) return s + d - s * d;
        if (mode == 3) return 2.0 * d < da ? 2.0 * s * d + temp : sa * da - 2.0 * (da - d) * (sa - s) + temp;
        if (mode == 4) return min(s * da, d * sa) + temp;
        if (mode == 5) return max(s * da, d * sa) + temp;
        if (mode == 6) {
          if (s * da + d * sa >= sa * da) return sa * da + temp;
          return d * sa / (1.0 - s / sa) + temp;
        }
        if (mode == 7) {
          if (s == 0.0 || s * da + d * sa <= sa * da) return temp;
          return sa * (s * da + d * sa - sa * da) / s + temp;
        }
        if (mode == 8) return 2.0 * s < sa ? 2.0 * s * d + temp : sa * da - 2.0 * (da - d) * (sa - s) + temp;
        if (mode == 9) {
          float m = da != 0.0 ? d / da : 0.0;
          if (2.0 * s < sa) return d * (sa + (2.0 * s - sa) * (1.0 - m)) + temp;
          if (4.0 * d <= da) return d * sa + da * (2.0 * s - sa) * (((16.0 * m - 12.0) * m + 3.0) * m) + temp;
          return d * sa + da * (2.0 * s - sa) * (sqrt(m) - m) + temp;
        }
        if (mode == 10) return s + d - 2.0 * min(s * da, d * sa);
        if (mode == 11) return s + d - 2.0 * s * d;
        return min(s + d, 1.0);
      }
      void main() {
        ivec2 at = ivec2(gl_FragCoord.xy);
        vec4 s = texelFetch(layer, at, 0), d = texelFetch(beneath, at, 0);
        if (useMatte) {
          float w = texelFetch(matte, at, 0).a;
          s *= invertMatte ? 1.0 - w : w;
        }
        if (mode == 0) { color = (s + d * (1.0 - s.a)) * weight; return; }
        float a = mode == 12 ? min(s.a + d.a, 1.0) : s.a + d.a - s.a * d.a;
        color = clamp(vec4(op(s.r, d.r, s.a, d.a), op(s.g, d.g, s.a, d.a), op(s.b, d.b, s.a, d.a), a), 0.0, 1.0) * weight;
      })";
    auto program = std::make_unique<QOpenGLShaderProgram>();
    if (program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) &&
        program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) && program->link()) {
      blendAt = BlendUniforms(*program);
      blendProgram = std::move(program);
    }
    return bool(blendProgram);
  }
  bool blendLayer(const Scene &scene, QPainter &p, QRect dirty, QPainter::CompositionMode mode,
                  const std::function<void(QPainter &, const LayerEffectHook *)> &paintAlone) {
    return combineLayer(scene, p, dirty, mode, false, paintAlone, nullptr);
  }
  // A layer blurring itself by its own motion: each of its samples flattened
  // here, laid over what lies beneath with the layer's blend, and the results
  // averaged -- what the CPU does with its sample buffers, without leaving the
  // card. The rest of the frame is drawn once. Before, a frame with motion
  // blur was drawn whole once per sample here, every layer of it, still or not,
  // and every moving text's picture built again in turn on this one thread: a
  // stroked title cost three times what it costs on the CPU.
  using PaintSample = std::function<void(QPainter &, int, const LayerEffectHook *)>;
  bool motionBlurLayer(const Scene &scene, QPainter &p, QRect dirty, QPainter::CompositionMode mode, int samples,
                       const PaintSample &paintSample) {
    if (samples < 1) return false;
    const PaintAlone first = [&](QPainter &q, const LayerEffectHook *onCard) { paintSample(q, 0, onCard); };
    return combineLayer(scene, p, dirty, mode, false, first, nullptr, samples, &paintSample);
  }
  // A layer, flattened, blended with what lies beneath; and with a matte,
  // first kept only where the matte covers it. With `samples` above one, the
  // layer drawn that many times by `paintSample` and the blends averaged.
  using PaintAlone = std::function<void(QPainter &, const LayerEffectHook *)>;
  bool combineLayer(const Scene &scene, QPainter &p, QRect dirty, QPainter::CompositionMode mode, bool inverted,
                    const PaintAlone &paintAlone, const PaintAlone *paintMatte, int samples = 1,
                    const PaintSample *paintSample = nullptr) {
    const int code = blendCode(mode);
    const bool sampled = samples > 1 && paintSample;
    if ((!code && mode != QPainter::CompositionMode_SourceOver) || (!code && !paintMatte && !sampled)) return false;
    if (!effectsReady || !vao || !p.device() || !prepareBlend()) return false;
    if (p.compositionMode() != QPainter::CompositionMode_SourceOver) return false;
    QRect clip;
    if (!painterClipIsDeviceRect(p, &clip)) return false;
    const QSize device(p.device()->width(), p.device()->height());
    dirty &= QRect(QPoint(), device);
    if (!clip.isNull()) dirty &= clip;
    if (dirty.isEmpty()) return true;
    if (blendDepth >= 8) return false;
    auto *gl = context.extraFunctions();
    p.beginNativePainting();
    struct EndNative {
      QPainter &p;
      ~EndNative() { p.endNativePainting(); }
    } endNative{p};
    clearBeforeStep(gl);
    // Known before anything is made: constructing a framebuffer object binds it.
    const GLint frameFbo = drawTarget(p.device()).fbo;
    // Room for every level made at once, so a level opened inside this one never
    // moves the targets this one is holding.
    if (blendTargets.capacity() < 8) blendTargets.reserve(8);
    if (int(blendTargets.size()) <= blendDepth) blendTargets.resize(size_t(blendDepth) + 1);
    BlendTargets &targets = blendTargets[size_t(blendDepth)];
    if (!targets.sample || targets.sample->size() != device) {
      targets.painted.reset(); // it paints into the target about to go
      QOpenGLFramebufferObjectFormat format;
      format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
      format.setInternalTextureFormat(GL_RGBA8);
      format.setSamples(std::clamp(8, 0, std::max(0, int(maxSamples))));
      targets.sample = std::make_unique<QOpenGLFramebufferObject>(device, format);
      if (!targets.sample->isValid()) { format.setSamples(0); targets.sample = std::make_unique<QOpenGLFramebufferObject>(device, format); }
      format.setSamples(0);
      format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
      targets.flat = std::make_unique<QOpenGLFramebufferObject>(device, format);
      targets.beneath = std::make_unique<QOpenGLFramebufferObject>(device, format);
      targets.matte = std::make_unique<QOpenGLFramebufferObject>(device, format);
      if (!targets.sample->isValid() || !targets.flat->isValid() || !targets.beneath->isValid() ||
          !targets.matte->isValid()) {
        targets = BlendTargets{};
        gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(frameFbo));
        return false;
      }
      targets.painted = std::make_unique<CompositionPaintDevice>(*targets.sample);
    }
    if (sampled && (!targets.blur || targets.blur->size() != device)) {
      // Half floats, so a sum of samples keeps what eight bits a sample would lose.
      QOpenGLFramebufferObjectFormat format;
      format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
      format.setInternalTextureFormat(GL_RGBA16F);
      targets.blur = std::make_unique<QOpenGLFramebufferObject>(device, format);
      if (!targets.blur->isValid()) {
        targets.blur.reset();
        gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(frameFbo));
        return false;
      }
    }
    // 1. What lies beneath, out of the frame (resolved, if the frame is
    // multisampled), before anything else is drawn.
    const int glTop = device.height() - dirty.bottom() - 1, glBottom = device.height() - dirty.top();
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(frameFbo));
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targets.beneath->handle());
    gl->glBlitFramebuffer(dirty.left(), glTop, dirty.right() + 1, glBottom, dirty.left(), glTop, dirty.right() + 1, glBottom,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // 2. The layer alone (and its matte), flattened on the card, their own
    // effects there too.
    auto flatten = [&](const PaintAlone &paint, QOpenGLFramebufferObject &into) {
      // Only the layer's rectangle is read back out of it, so only that is
      // cleared: the whole of a multisampled frame-sized target, colour, depth
      // and stencil, cleared for every layer and every sample of its blur, cost a
      // title sequence with many moving layers more than its blur saved.
      clear(*targets.sample, &dirty);
      {
        struct Deeper { int &d; explicit Deeper(int &v) : d(v) { ++d; } ~Deeper() { --d; } } deeper{blendDepth};
        QPainter q(targets.painted.get());
        if (!q.isActive()) return false;
        const LayerEffectHook onCard = [&](const Layer &l, double t, QRectF logical, double rasterScale, QRectF box,
                                           const QImage &raster, bool masked) {
          const QTransform toDevice = q.combinedTransform();
          const double opacity = q.opacity();
          q.beginNativePainting();
          struct EndInner {
            QPainter &p;
            ~EndInner() { p.endNativePainting(); }
          } endInner{q};
          return effectedRaster(scene, l, t, logical, rasterScale, box, raster, masked, toDevice, opacity, q.device());
        };
        paint(q, &onCard);
      }
      gl->glDisable(GL_SCISSOR_TEST);
      gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, targets.sample->handle());
      gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, into.handle());
      gl->glBlitFramebuffer(dirty.left(), glTop, dirty.right() + 1, glBottom, dirty.left(), glTop, dirty.right() + 1, glBottom,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
      return true;
    };
    auto useBlend = [&](float weight) {
      blendProgram->bind();
      const float cx = 2.f * float(dirty.x()) / float(device.width()) - 1.f;
      const float cw = 2.f * float(dirty.width()) / float(device.width());
      const float cy = 1.f - 2.f * float(dirty.y() + dirty.height()) / float(device.height());
      const float ch = 2.f * float(dirty.height()) / float(device.height());
      blendProgram->setUniformValue(blendAt.area, QVector4D(cx, cy, cw, ch));
      blendProgram->setUniformValue(blendAt.layer, 0);
      blendProgram->setUniformValue(blendAt.beneath, 1);
      blendProgram->setUniformValue(blendAt.mode, code);
      blendProgram->setUniformValue(blendAt.matte, 2);
      blendProgram->setUniformValue(blendAt.useMatte, false);
      blendProgram->setUniformValue(blendAt.invertMatte, false);
      blendProgram->setUniformValue(blendAt.weight, weight);
      gl->glActiveTexture(GL_TEXTURE1);
      gl->glBindTexture(GL_TEXTURE_2D, targets.beneath->texture());
      nearest(gl);
      gl->glActiveTexture(GL_TEXTURE0);
      gl->glBindTexture(GL_TEXTURE_2D, targets.flat->texture());
      nearest(gl);
      gl->glDisable(GL_DEPTH_TEST); gl->glDisable(GL_STENCIL_TEST); gl->glDisable(GL_CULL_FACE);
      gl->glEnable(GL_SCISSOR_TEST);
      gl->glScissor(dirty.left(), glTop, dirty.width(), dirty.height());
      gl->glBindVertexArray(vao);
      gl->glDrawArrays(GL_TRIANGLES, 0, 6);
      gl->glBindVertexArray(0);
      gl->glDisable(GL_SCISSOR_TEST);
      blendProgram->release();
    };
    if (sampled) {
      // 2b. Each sample flattened, laid over what lies beneath, and added into
      // the sum at its share; then the sum is the layer's rectangle of the frame.
      gl->glBindFramebuffer(GL_FRAMEBUFFER, targets.blur->handle());
      gl->glViewport(0, 0, device.width(), device.height());
      gl->glEnable(GL_SCISSOR_TEST);
      gl->glScissor(dirty.left(), glTop, dirty.width(), dirty.height());
      gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      gl->glClearColor(0, 0, 0, 0);
      gl->glClear(GL_COLOR_BUFFER_BIT);
      gl->glDisable(GL_SCISSOR_TEST);
      for (int k = 0; k < samples; ++k) {
        const PaintAlone one = [&, k](QPainter &q, const LayerEffectHook *onCard) { (*paintSample)(q, k, onCard); };
        if (!flatten(one, *targets.flat)) return false;
        gl->glBindFramebuffer(GL_FRAMEBUFFER, targets.blur->handle());
        gl->glViewport(0, 0, device.width(), device.height());
        gl->glEnable(GL_BLEND);
        gl->glBlendEquation(GL_FUNC_ADD);
        gl->glBlendFunc(GL_ONE, GL_ONE);
        useBlend(1.f / float(samples));
        gl->glDisable(GL_BLEND);
      }
      gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(frameFbo));
      gl->glViewport(0, 0, device.width(), device.height());
      gl->glDisable(GL_BLEND); gl->glDisable(GL_DEPTH_TEST); gl->glDisable(GL_STENCIL_TEST); gl->glDisable(GL_CULL_FACE);
      gl->glEnable(GL_SCISSOR_TEST);
      gl->glScissor(dirty.left(), glTop, dirty.width(), dirty.height());
      shader->bind();
      shader->setUniformValue(shaderAt.source, 0);
      shader->setUniformValue(shaderAt.weight, 1.f);
      gl->glActiveTexture(GL_TEXTURE0);
      gl->glBindTexture(GL_TEXTURE_2D, targets.blur->texture());
      nearest(gl);
      gl->glBindVertexArray(vao);
      gl->glDrawArrays(GL_TRIANGLES, 0, 3);
      gl->glBindVertexArray(0);
      shader->release();
      gl->glDisable(GL_SCISSOR_TEST);
      if (!stepWithoutError(gl)) return false;
      ++blurredLayers;
      return true;
    }
    if (!flatten(paintAlone, *targets.flat)) return false;
    if (paintMatte && !flatten(*paintMatte, *targets.matte)) return false;
    // 3. The blend, written over the layer's rectangle of the frame.
    gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(frameFbo));
    gl->glViewport(0, 0, device.width(), device.height());
    blendProgram->bind();
    blendProgram->setUniformValue(blendAt.weight, 1.f);
    const float cx = 2.f * float(dirty.x()) / float(device.width()) - 1.f;
    const float cw = 2.f * float(dirty.width()) / float(device.width());
    const float cy = 1.f - 2.f * float(dirty.y() + dirty.height()) / float(device.height());
    const float ch = 2.f * float(dirty.height()) / float(device.height());
    blendProgram->setUniformValue(blendAt.area, QVector4D(cx, cy, cw, ch));
    blendProgram->setUniformValue(blendAt.layer, 0);
    blendProgram->setUniformValue(blendAt.beneath, 1);
    blendProgram->setUniformValue(blendAt.mode, code);
    blendProgram->setUniformValue(blendAt.matte, 2);
    blendProgram->setUniformValue(blendAt.useMatte, paintMatte != nullptr);
    blendProgram->setUniformValue(blendAt.invertMatte, inverted);
    gl->glActiveTexture(GL_TEXTURE2);
    gl->glBindTexture(GL_TEXTURE_2D, targets.matte->texture());
    nearest(gl);
    gl->glActiveTexture(GL_TEXTURE1);
    gl->glBindTexture(GL_TEXTURE_2D, targets.beneath->texture());
    nearest(gl);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, targets.flat->texture());
    nearest(gl);
    gl->glDisable(GL_BLEND); gl->glDisable(GL_DEPTH_TEST); gl->glDisable(GL_STENCIL_TEST); gl->glDisable(GL_CULL_FACE);
    gl->glEnable(GL_SCISSOR_TEST);
    gl->glScissor(dirty.left(), glTop, dirty.width(), dirty.height());
    gl->glBindVertexArray(vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 6);
    gl->glBindVertexArray(0);
    gl->glDisable(GL_SCISSOR_TEST);
    blendProgram->release();
    if (!stepWithoutError(gl)) return false;
    ++(paintMatte ? mattedLayers : blendedLayers);
    return true;
  }
  // The unit square, carried by the layer's own transform into the frame.
  // The finished frame as the 8-bit straight-alpha picture the rest of the
  // program works in. QOpenGLFramebufferObject::toImage reads it bottom row
  // first and premultiplied; Qt then turns it over and divides the alpha out
  // on the CPU, a walk over the whole frame for each -- about 3.5 ms of every
  // 1080p frame. Here one pass on the card does both, and the read lands in
  // the image as it is. The division rounds as the card rounds, which can be
  // a level away from Qt's in an odd pixel at part alpha.
  std::unique_ptr<QOpenGLShaderProgram> readout;
  GLint readoutSourceAt = -1;
  std::unique_ptr<QOpenGLFramebufferObject> readoutTarget;
  bool readoutAttempted = false;
  QImage readStraight8(QOpenGLFramebufferObject &from) {
    auto *gl = context.extraFunctions();
    if (!readoutAttempted) {
      readoutAttempted = true;
      const char *vertex = R"(#version 330 core
        void main() {
          const vec2 corner[6] = vec2[6](vec2(-1,-1),vec2(1,-1),vec2(-1,1),
                                         vec2(1,-1),vec2(1,1),vec2(-1,1));
          gl_Position = vec4(corner[gl_VertexID], 0.0, 1.0);
        })";
      const char *fragment = R"(#version 330 core
        uniform sampler2D source;
        out vec4 color;
        void main() {
          ivec2 size = textureSize(source, 0);
          vec4 c = texelFetch(source, ivec2(int(gl_FragCoord.x), size.y - 1 - int(gl_FragCoord.y)), 0);
          color = c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
        })";
      auto program = std::make_unique<QOpenGLShaderProgram>();
      if (program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) &&
          program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) && program->link()) {
        readoutSourceAt = program->uniformLocation("source");
        readout = std::move(program);
      }
    }
    if (!readout || !vao) return {};
    const QSize size = from.size();
    if (!readoutTarget || readoutTarget->size() != size) {
      QOpenGLFramebufferObjectFormat format;
      format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
      format.setInternalTextureFormat(GL_RGBA8);
      readoutTarget = std::make_unique<QOpenGLFramebufferObject>(size, format);
      if (!readoutTarget->isValid()) { readoutTarget.reset(); return {}; }
    }
    QImage image(size, QImage::Format_RGBA8888);
    if (image.isNull()) return {};
    // The frame was asked about just before this (render()): nothing is left.
    clearBeforeStep(gl);
    readoutTarget->bind();
    gl->glViewport(0, 0, size.width(), size.height());
    gl->glDisable(GL_BLEND); gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_SCISSOR_TEST); gl->glDisable(GL_STENCIL_TEST);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    readout->bind();
    readout->setUniformValue(readoutSourceAt, 0);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, from.texture());
    gl->glBindVertexArray(vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 6);
    gl->glBindVertexArray(0);
    readout->release();
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, GLint(image.bytesPerLine() / 4));
    gl->glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    if (gl->glGetError() != GL_NO_ERROR) return {};
    return image;
  }

  void drawTexture(GLuint texture, const QTransform &quadToDevice, QSize device, double opacity) {
    auto *gl = context.extraFunctions();
    const QTransform toClip(2. / device.width(), 0, 0, -2. / device.height(), -1, 1);
    const QTransform t = quadToDevice * toClip;
    const QMatrix4x4 quad(t.m11(), t.m21(), 0, t.m31(),
                          t.m12(), t.m22(), 0, t.m32(),
                          0, 0, 1, 0,
                          t.m13(), t.m23(), 0, t.m33());
    composite->bind();
    composite->setUniformValue(compositeAt.quad, quad);
    composite->setUniformValue(compositeAt.source, 0);
    composite->setUniformValue(compositeAt.opacity, float(opacity));
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glEnable(GL_BLEND);
    gl->glBlendEquation(GL_FUNC_ADD);
    gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // the chain leaves it premultiplied
    gl->glDisable(GL_DEPTH_TEST); gl->glDisable(GL_SCISSOR_TEST);
    gl->glBindVertexArray(vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 6);
    gl->glBindVertexArray(0);
    gl->glDisable(GL_BLEND);
    composite->release();
  }

  // Rasterize the layer and run its chain on the card. A null image hands the
  // layer to the ordinary CPU route.
  //
  // A big result stays in a bounded texture cache and is drawn directly into
  // the frame, as does one that moves by itself; a small one is read back to
  // the shared image cache. The painter is already inside beginNativePainting
  // here, so drawing costs no extra transition.
  //
  // A mask is a clip path, which native drawing cannot see, so a masked layer
  // always comes back as pixels for the painter to clip.
  // Per layer: what the last result made for it was, and how many times in a
  // row that changed.
  struct EffectedHistory { QByteArray key; int streak = 0; };
  QHash<QString, EffectedHistory> lastEffectedKey;
  // `device` is what the painter draws on.
  LayerEffectResult effectedRaster(const Scene &scene, const Layer &l, double t, QRectF logical,
                        double rasterScale, QRectF box,
                        const QImage &raster, bool masked,
                        const QTransform &toDevice, double opacity, QPaintDevice *device) {
    if (!effectsReady) return {};
    const QSize rasterSize(qCeil(logical.width() * rasterScale),
                           qCeil(logical.height() * rasterScale));
    if (rasterSize.isEmpty()) return {};
    // The same identity as the CPU cache, including active effects/order,
    // source image revision, shape/text animation, main transform, pixel
    // scale and bounds.
    const QByteArray residentKey = effectedRasterKey(l, scene, t, logical, rasterScale, box);
    // A result that will be asked for again is kept where it is cheapest to
    // find: a big one in the card's resident cache, a small one read back once
    // to the shared image cache, from which later frames draw it (through the
    // card's kept textures) without coming here at all. A result that is new
    // every time it is asked for -- an effect that moves by itself, letters
    // that animate, anything whose picture changed the last two times one was
    // made for this layer (one change is an edit, kept as ever) -- is drawn
    // straight from the card, never read back nor kept: read back and sent up
    // again by the painter, Glitch on six titles cost 28 ms a frame against
    // the CPU's 11.
    EffectedHistory &history = lastEffectedKey[l.id];
    history.streak = !history.key.isEmpty() && history.key != residentKey ? history.streak + 1 : 0;
    history.key = residentKey;
    const bool changing = layerPackEffectMoves(l, t) || history.streak >= 2;
    if (lastEffectedKey.size() > 4096) lastEffectedKey.clear();
    const bool big = qint64(rasterSize.width()) * rasterSize.height() > 1048576;
    const bool residentCandidate = !masked && !changing && big;
    auto *gl = context.extraFunctions();
    clearBeforeStep(gl);
    const DrawTarget frame = drawTarget(device);
    const GLint previousFbo = frame.fbo;
    const GLint *viewport = frame.viewport;
    auto drawResult = [&](GLuint texture, QSize size) {
      QTransform quadToLogical;
      quadToLogical.translate(logical.left(), logical.top());
      quadToLogical.scale(size.width() / rasterScale, size.height() / rasterScale);
      drawTexture(texture, quadToLogical * toDevice, QSize(viewport[2], viewport[3]), opacity);
      return stepWithoutError(gl);
    };
    if (residentCandidate) {
      auto cached = residentRasters.find(residentKey);
      if (cached != residentRasters.end()) {
        cached->used = ++residentClock;
        if (drawResult(cached->texture, cached->size)) {
          ++residentHits;
          return {QImage(), true};
        }
        return {};
      }
    }
    // The gradient reads this, and it reads it in raster pixels: the same
    // rectangle the CPU chain is handed, not the layer's own coordinates.
    const QRectF content((box.left() - logical.left()) * rasterScale,
                         (box.top() - logical.top()) * rasterScale,
                         box.width() * rasterScale, box.height() * rasterScale);
    // What animated sliders and moving effects read: this layer's own time.
    EffectTimeScope effectTime(t / std::max(1, scene.fps), t);
    const auto passes = layerEffectPasses(l, content, rasterScale, rasterSize, true);
    if (passes.isEmpty()) return {};
    QSize resultSize;
    const GLuint uploaded = uploadRaster(raster);
    const GLuint result = uploaded ? runPasses(uploaded, rasterSize, passes, resultSize) : 0;
    // A masked result comes back as pixels for the painter to clip.
    const bool keepOnCard = result && !masked && (changing || big);
    QImage effected;
    if (result && !keepOnCard) {
      const bool highDepth = scene.processingDepth > 8;
      effected = QImage(resultSize, highDepth ? QImage::Format_RGBA64_Premultiplied
                                              : QImage::Format_RGBA8888_Premultiplied);
      if (!effected.isNull()) {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, effectFbo);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, result, 0);
        gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
        gl->glReadPixels(0, 0, resultSize.width(), resultSize.height(), GL_RGBA,
                         highDepth ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, effected.bits());
        // Kept in the image cache, for this backend and the CPU's: checked.
        if (!noErrorSince(gl)) effected = QImage();
      }
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFbo));
    gl->glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (keepOnCard) {
      ++residentBuilds;
      if (drawResult(result, resultSize)) {
        // Kept for later frames only when nothing has gone wrong: drawn wrong,
        // this frame goes to the CPU, but kept it would be drawn again and again.
        if (residentCandidate && noErrorSince(gl)) retainRaster(residentKey, result, resultSize);
        return {QImage(), true};
      }
      return {};
    }
    if (effected.isNull()) return {};
    auto converted = effected.convertToFormat(processingFormat(scene.processingDepth, true));
    return {converted, false};
  }

  // An Effect Layer, worked on the card. The picture drawn so far is resolved,
  // the part the layer's effects read is copied out (top row first, the way a
  // raster arrives), its chain runs on it -- on the card when every effect has
  // a form there, else on the CPU with the very same code the CPU compositor
  // uses -- and the result goes back where the layer acts, in proportion to
  // its opacity: the picture is replaced there, not laid over, as the CPU's
  // Source composition does. The rest of the frame never leaves the card.
  std::unique_ptr<QOpenGLShaderProgram> layerMix;
  struct { GLint quad = -1, beneath = -1, effected = -1, coverage = -1, opacity = -1, masked = -1; } layerMixAt;
  GLuint copyFbo = 0, beneathTexture = 0, coverageTexture = 0;
  QSize beneathSize;
  bool layerMixAttempted = false;
  bool effectLayerFailed = false;
  bool prepareLayerMix() {
    if (layerMixAttempted) return bool(layerMix);
    layerMixAttempted = true;
    if (!effectsReady) return false;
    const char *vertex = R"(#version 330 core
      uniform mat4 quad;
      out vec2 uv;
      void main() {
        const vec2 corner[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),
                                       vec2(1,0),vec2(1,1),vec2(0,1));
        vec2 p = corner[gl_VertexID];
        uv = p;
        gl_Position = quad * vec4(p, 0.0, 1.0);
      })";
    const char *fragment = R"(#version 330 core
      uniform sampler2D beneath;
      uniform sampler2D effected;
      uniform sampler2D coverage;
      uniform float opacity;
      uniform bool masked;
      in vec2 uv;
      out vec4 color;
      void main() {
        float k = opacity * (masked ? texture(coverage, uv).r : 1.0);
        color = mix(texture(beneath, uv), texture(effected, uv), k);
      })";
    auto program = std::make_unique<QOpenGLShaderProgram>();
    if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) ||
        !program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) || !program->link())
      return false;
    auto *gl = context.extraFunctions();
    gl->glGenFramebuffers(1, &copyFbo);
    gl->glGenTextures(1, &beneathTexture);
    gl->glGenTextures(1, &coverageTexture);
    layerMixAt = {program->uniformLocation("quad"), program->uniformLocation("beneath"),
                  program->uniformLocation("effected"), program->uniformLocation("coverage"),
                  program->uniformLocation("opacity"), program->uniformLocation("masked")};
    layerMix = std::move(program);
    return copyFbo && beneathTexture && coverageTexture;
  }
  static void nearest(QOpenGLExtraFunctions *gl) {
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  quint64 effectLayers = 0, effectLayerReadbacks = 0;
  // `device` is what the painter draws on.
  bool effectLayer(const Scene &scene, const Layer &l, double t, const QPainterPath &area,
                   QRect acting, QRect source, double scale, double opacity, QPaintDevice *device) {
    if (!effectsReady || !prepareLayerMix() || !sample || source.isEmpty()) return false;
    ++effectLayers;
    auto *gl = context.extraFunctions();
    clearBeforeStep(gl);
    const DrawTarget before = drawTarget(device);
    struct Restore {
      QOpenGLExtraFunctions *gl; const DrawTarget &to;
      ~Restore() {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(to.fbo));
        gl->glViewport(to.viewport[0], to.viewport[1], to.viewport[2], to.viewport[3]);
      }
    } restore{gl, before};
    const int width = sample->width(), height = sample->height();
    gl->glDisable(GL_SCISSOR_TEST);
    // The picture so far, its samples resolved.
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, sample->handle());
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sampleResolved->handle());
    gl->glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // What the effects read, turned the right way up: the frame's rows run
    // bottom to top on the card, a raster's top to bottom.
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, beneathTexture);
    nearest(gl);
    const bool remade = beneathSize != source.size();
    if (remade) {
      gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, source.width(), source.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      beneathSize = source.size();
    }
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copyFbo);
    // Attached and checked when it is made, not on every use: the check too
    // waits for the driver, and nothing else is ever attached here.
    if (remade) {
      gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, beneathTexture, 0);
      if (gl->glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        beneathSize = QSize();
        return false;
      }
    }
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, sampleResolved->handle());
    gl->glBlitFramebuffer(source.left(), height - source.top() - source.height(), source.left() + source.width(), height - source.top(),
                          0, source.height(), source.width(), 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // Its effects.
    const QRectF content(QPointF(acting.topLeft() - source.topLeft()), QSizeF(acting.size()));
    EffectTimeScope effectTime(t / std::max(1, scene.fps), t);
    QSize resultSize;
    GLuint effected = 0;
    const auto passes = layerEffectPasses(l, content, scale, source.size(), true);
    if (!passes.isEmpty()) effected = runPasses(beneathTexture, source.size(), passes, resultSize);
    if (!effected || resultSize != source.size()) {
      ++effectLayerReadbacks;
      QImage beneath(source.size(), QImage::Format_RGBA8888_Premultiplied);
      if (beneath.isNull()) return false;
      gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, copyFbo);
      gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
      gl->glPixelStorei(GL_PACK_ROW_LENGTH, GLint(beneath.bytesPerLine() / 4));
      gl->glReadPixels(0, 0, source.width(), source.height(), GL_RGBA, GL_UNSIGNED_BYTE, beneath.bits());
      gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
      if (!noErrorSince(gl)) return false;
      const QImage picture = beneath.convertToFormat(processingFormat(scene.processingDepth, true));
      const QImage out = applyLayerEffects(picture, l, content, scale, t);
      // As on the CPU: effects that came to nothing leave the picture alone.
      if (out.isNull() || out.size() != picture.size()) return true;
      effected = uploadRaster(out.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
      if (!effected) return false;
    }
    // Where it acts: its box cut to its masks, smoothed at the edge as the
    // CPU's clip is. A layer over the whole of what it changes needs no mask.
    const bool masked = !area.contains(QRectF(source).adjusted(.5, .5, -.5, -.5));
    if (masked) {
      QImage coverage(source.size(), QImage::Format_Grayscale8);
      if (coverage.isNull()) return false;
      coverage.fill(0);
      {
        QPainter cut(&coverage);
        cut.setRenderHint(QPainter::Antialiasing);
        cut.translate(-source.left(), -source.top());
        cut.fillPath(area, Qt::white);
      }
      gl->glActiveTexture(GL_TEXTURE2);
      gl->glBindTexture(GL_TEXTURE_2D, coverageTexture);
      nearest(gl);
      gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(coverage.bytesPerLine()));
      gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, coverage.width(), coverage.height(), 0, GL_RED, GL_UNSIGNED_BYTE, coverage.constBits());
      gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    // Back into the frame, replacing the picture there.
    gl->glBindFramebuffer(GL_FRAMEBUFFER, sample->handle());
    gl->glViewport(0, 0, width, height);
    gl->glDisable(GL_BLEND); gl->glDisable(GL_DEPTH_TEST); gl->glDisable(GL_STENCIL_TEST);
    gl->glDisable(GL_SCISSOR_TEST); gl->glDisable(GL_CULL_FACE);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const QTransform quadToDevice(source.width(), 0, 0, source.height(), source.left(), source.top());
    const QTransform toClip(2. / width, 0, 0, -2. / height, -1, 1);
    const QTransform q = quadToDevice * toClip;
    const QMatrix4x4 quad(q.m11(), q.m21(), 0, q.m31(), q.m12(), q.m22(), 0, q.m32(), 0, 0, 1, 0, q.m13(), q.m23(), 0, q.m33());
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, beneathTexture);
    nearest(gl);
    gl->glActiveTexture(GL_TEXTURE1);
    gl->glBindTexture(GL_TEXTURE_2D, effected);
    nearest(gl);
    layerMix->bind();
    layerMix->setUniformValue(layerMixAt.quad, quad);
    layerMix->setUniformValue(layerMixAt.beneath, 0);
    layerMix->setUniformValue(layerMixAt.effected, 1);
    layerMix->setUniformValue(layerMixAt.coverage, 2);
    layerMix->setUniformValue(layerMixAt.opacity, float(opacity));
    layerMix->setUniformValue(layerMixAt.masked, masked);
    gl->glBindVertexArray(vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 6);
    gl->glBindVertexArray(0);
    layerMix->release();
    gl->glActiveTexture(GL_TEXTURE0);
    return stepWithoutError(gl);
  }

  ~State() {
    if (surface && context.makeCurrent(surface)) {
      sample.reset(); sampleResolved.reset(); accumulation.reset(); resolved.reset(); shader.reset();
      effects.reset(); composite.reset(); readout.reset(); readoutTarget.reset();
      auto *gl = context.extraFunctions();
      for (const auto &raster : residentRasters) gl->glDeleteTextures(1, &raster.texture);
      residentRasters.clear();
      for (GLuint &texture : effectTexture) if (texture) gl->glDeleteTextures(1, &texture);
      for (GLuint &fbo : effectSlotFbo) if (fbo) gl->glDeleteFramebuffers(1, &fbo);
      for (const SpareWork &spare : spareWork) {
        gl->glDeleteFramebuffers(1, &spare.fbo);
        gl->glDeleteTextures(1, &spare.texture);
      }
      spareWork.clear();
      layerMix.reset();
      packPrograms.clear();
      if (beneathTexture) gl->glDeleteTextures(1, &beneathTexture);
      if (coverageTexture) gl->glDeleteTextures(1, &coverageTexture);
      if (copyFbo) gl->glDeleteFramebuffers(1, &copyFbo);
      if (effectFbo) gl->glDeleteFramebuffers(1, &effectFbo);
      if (vao) gl->glDeleteVertexArrays(1, &vao);
      releaseImages();
      releaseSources();
      litProgram.reset(); facePainted.reset(); faceSample.reset(); faceResolved.reset();
      blendProgram.reset(); blendTargets.clear();
      if (shadowAtlas) gl->glDeleteTextures(1, &shadowAtlas);
      shapeEdgeProgram.reset(); shapeFillProgram[0].reset(); shapeFillProgram[1].reset();
      if (shapeVao) gl->glDeleteVertexArrays(1, &shapeVao);
      if (shapeEdgeBuffer) gl->glDeleteBuffers(1, &shapeEdgeBuffer);
      if (shapeEdgeList) gl->glDeleteTextures(1, &shapeEdgeList);
      if (shapeRowBuffer) gl->glDeleteBuffers(1, &shapeRowBuffer);
      if (shapeRowList) gl->glDeleteTextures(1, &shapeRowList);
      if (shapeTable) gl->glDeleteTextures(1, &shapeTable);
      for (GLuint &texture : shapeArea) if (texture) gl->glDeleteTextures(1, &texture);
      for (GLuint &fbo : shapeAreaFbo) if (fbo) gl->glDeleteFramebuffers(1, &fbo);
      particleProgram.reset(); particleMix.reset(); particleTarget.reset(); particleBeneath.reset();
      if (particleVbo) gl->glDeleteBuffers(1, &particleVbo);
      if (particleVao) gl->glDeleteVertexArrays(1, &particleVao);
      context.doneCurrent();
    }
  }
};
GpuRenderer::GpuRenderer() : state(std::make_unique<State>()) {}
GpuRenderer::~GpuRenderer() {
  // The context goes first, on this thread; its window outlives it and goes
  // back to the pool, never destroyed off the interface thread.
  QOffscreenSurface *lent = state->surface;
  state.reset();
  surfacePool().give(lent);
}
QString GpuRenderer::description() const { return state->detail; }
GpuResidentCacheStats GpuRenderer::residentCacheStats() const {
  return {state->residentHits, state->residentBuilds, state->residentBytes, state->particleLayers,
          state->imageUploads, state->imageDraws, state->litFaces, state->blendedLayers, state->mattedLayers,
          state->effectLayers, state->effectLayerReadbacks, state->blurredLayers, state->shapeFills,
          state->shapeFillsAddedUp};
}
QImage GpuRenderer::render(const Scene &scene, double frame, QSize size, QRect region) {
  if (sceneIsolates(scene)) return render(isolatedScene(scene), frame, size, region);
  ExpressionSceneScope expressions(&scene);
  if (const QString why = declineReason(scene, frame); !why.isEmpty()) {
    state->detail = why;
    return {};
  }
  if (QThread::currentThread() != state->context.thread() || !state->initialize()) return {};
  if (size.isEmpty()) size = QSize(scene.width, scene.height);
  region = region.isEmpty() ? QRect(QPoint(), size) : region.intersected(QRect(QPoint(), size));
  if (region.isEmpty()) return {};
  if (!state->context.makeCurrent(state->surface)) return {};
  struct ReleaseContext {
    QOpenGLContext &context;
    ~ReleaseContext() { context.doneCurrent(); }
  } release{state->context};
  // Ignore stale errors left by another Qt GL operation; validate this frame.
  auto *functions = state->context.extraFunctions();
  for (int attempt = 0; attempt < 16 && functions->glGetError() != GL_NO_ERROR; ++attempt) {}
  // However the frame ends, an error found in it means each step checks its
  // own from now on (see checkEachStep).
  state->frameError = GL_NO_ERROR;
  struct CheckAfterError {
    State &s;
    ~CheckAfterError() { if (s.frameError != GL_NO_ERROR) s.checkEachStep = true; }
  } checkAfterError{*state};
  if (!state->buffers(region.size())) { state->detail = "GPU target allocation failed; using CPU"; return {}; }
  state->clear(*state->accumulation);
  // paintComposition is driven from here rather than renderCPU, so the frame's
  // claim on the raster cache has to be started here too. Never started, the
  // claim only grows, and once it passes the budget nothing more is stored.
  beginRasterFrame();
  state->frameSourceBuilds = layerSourcesBuiltHere();
  state->frameStartClock = state->residentClock;
  state->sourceFrameStart = state->sourceClock;
  state->sourcesPrewarmed = false;
  // Motion blur, the way the CPU decides it where it can: a layer blurs itself
  // only when it moves far enough across the shutter for a blur to show. When
  // none does, the frame is drawn once; when a few do, the frame is drawn once
  // and each of them sampled on its own, its samples averaged on the card
  // (motionBlurLayer); when many do, the whole frame is drawn once per sample
  // and those averaged -- with many moving layers, flattening each of them per
  // sample costs more than drawing the frame again. Before, every frame with
  // anything keyed was drawn whole four times over, a title whose letters drift
  // a pixel a frame too, and every moving text's picture built again each time
  // on this one thread: a stroked title cost three times what it costs on the
  // CPU.
  // One scene, one frame: the letters' places, and how many times each moving
  // layer is sampled, are worked out once each.
  TextBoundsMemoScope textBounds;
  bool perLayerBlur = false;
  int samples = 1;
  RouteChoice *blurRoute = nullptr;
  qint64 blurRouteSize = 0;
  QElapsedTimer drawing;
  drawing.start();
  if (scene.motionBlur && compositionHasMotion(scene, frame)) {
    const bool eligible = motionBlurPerLayer(scene, frame);
    const MovingLayers moving = eligible ? movingLayers(scene, frame, size) : MovingLayers{};
    if (eligible && moving.moving <= 3) perLayerBlur = moving.moving > 0;
    else if (eligible) {
      // Many layers moving: each sampled on its own -- as many times as its
      // own move needs, the rest drawn once -- or the frame drawn whole once a
      // sample. Which is quicker depends on what moves (a page of flying
      // lyrics: each on its own, 199 -> 137 ms; a few big glowing rings: the
      // whole frame), so it is learned, for each number of moving layers, by
      // what each took (route_choice.h). Each on its own is also the picture
      // the CPU draws.
      static RouteChoice route("Motion blur, each moving layer on its own", 240);
      blurRoute = &route;
      blurRouteSize = qint64(std::min(moving.moving, 1000)) << 20;
      perLayerBlur = route.card(blurRouteSize);
    }
    if (!perLayerBlur && eligible && moving.moving > 3) samples = motionBlurSampleCount(scene, frame, size);
    else if (!eligible) samples = motionBlurSampleCount(scene, frame, size);
  }
  if (samples > 1 && previewMotionBlurCap(scene) > 0)
    samples = std::min(samples, previewMotionBlurCap(scene));
  const bool residentReady = state->prepareEffects();
  // Without the effect pipeline there is no way to work an Effect Layer here.
  if (!residentReady && sceneUsesEffectLayers(scene, frame)) {
    state->detail = "Effect Layers need the GPU effect pipeline; using CPU";
    return {};
  }
  state->effectLayerFailed = false;
  LayerMotionBlurScope layerBlur(perLayerBlur);
  if (samples == 1) {
    // The pictures this frame will ask for, built first on every core: a moving
    // text's at each time its blur samples, the rest at once. Left to the pass
    // below, each was built in turn on this one thread. Only the sources: the
    // card works their effects itself.
    const LayerEffectHook sourcesOnly = [](const Layer &, double, QRectF, double, QRectF, const QImage &, bool) {
      return LayerEffectResult{QImage(), true};
    };
    warmFrameLayers(scene, frame, size, region, perLayerBlur, residentReady ? &sourcesOnly : nullptr);
  }
  auto sampleTime = [&](int index) {
    return samples == 1 ? frame : frame + ((index + .5) / samples - .5) * .5;
  };
  for (int index = 0; index < samples; ++index) {
    // Sample the middle of each shutter slice. This matches the CPU renderer,
    // avoids overweighting both shutter endpoints, and stays smooth when the
    // adaptive sample count rises for fast or large on-screen movement.
    double time = sampleTime(index);
    // Every motion-blur sample of a moving layer is a picture of its own, so
    // each sample may find its layers cold and warm them on all the cores.
    // Warmed once a frame, the samples after the first built every moving
    // layer's picture one after another on this thread.
    state->frameSourceBuilds = layerSourcesBuiltHere();
    state->sourcesPrewarmed = false;
    QHash<QString, QPicture> prepared;
    try { prepared = state->prepare(scene, time, frame); }
    catch (const std::exception &error) {
      state->detail = "GPU drawing preparation failed: " + QString::fromUtf8(error.what());
      return {};
    }
    state->clear(*state->sample);
    functions->glEnable(GL_MULTISAMPLE);
    CompositionPaintDevice device(*state->sample);
    {
      QPainter painter(&device);
      if (!painter.isActive()) { state->detail = "GPU painter unavailable; using CPU"; return {}; }
      painter.translate(-region.left(), -region.top());
      painter.setClipRect(region); // layers rasterize their effects to this
      LayerEffectHook resident = [&](const Layer &l, double t, QRectF logical,
                                     double rasterScale, QRectF box,
                                     const QImage &raster, bool masked) {
        // A frame that turns out to be cold -- an edit, a first frame, a scene
        // just opened -- would otherwise rasterize every layer's source one
        // after another on this thread while the other cores sit idle. On the
        // fourth source built here, do all of them at once: each worker paints
        // one layer with the raster step skipped, which leaves the finished
        // source in the source cache for the serial pass below to pick up.
        //
        // Counted by the sources actually built, not by the layers that came
        // here: a layer whose effects the card works afresh every frame comes
        // here every frame with its source long kept, and four of them set this
        // off on every frame of a warm scene. Only the layers this frame draws,
        // one to a worker as each comes free: handed out in runs of the whole
        // stack, a music video's sixty layers on screen fell in one worker's run
        // of 1,600 and were built one after another -- 12 ms of every frame.
        if (!state->sourcesPrewarmed && layerSourcesBuiltHere() - state->frameSourceBuilds >= 4 &&
            scene.layers.size() >= 6) {
          state->sourcesPrewarmed = true;
          const LayerEffectHook sourcesOnly = [](const Layer &, double, QRectF, double, QRectF,
                                                 const QImage &, bool) {
            return LayerEffectResult{QImage(), true};
          };
          std::vector<int> drawn;
          for (size_t index = 0; index < scene.layers.size(); ++index) {
            const Layer &warm = scene.layers[index];
            if (!warm.visible || !warm.groupId.isEmpty() || frame - warm.start < 0 ||
                frame - warm.start >= warm.duration)
              continue;
            if (warm.type == "camera" || warm.type == "light" || warm.type == "audio" || warm.type == "effect" ||
                warm.type == "empty" || layerIsStructure(warm))
              continue;
            drawn.push_back(int(index));
          }
          renderParallelEach(int(drawn.size()), [&](int n) {
            struct NoWait { NoWait() { setPrewarmWorker(true); } ~NoWait() { setPrewarmWorker(false); } } noWait;
            // One row: the clip and the transform are the frame's, so every
            // source comes out as the frame will ask for it, and what the layer
            // draws on the scratch itself, which no one looks at, costs nothing.
            static thread_local QImage scratch;
            if (scratch.width() != region.width() || scratch.height() != 1)
              scratch = QImage(region.width(), 1, QImage::Format_ARGB32_Premultiplied);
            if (scratch.isNull()) return;
            QPainter target(&scratch);
            target.translate(-region.left(), -region.top());
            target.setClipRect(region);
            paintComposition(target, scene, time, frame, size, nullptr, &sourcesOnly,
                             scene.layers[size_t(drawn[size_t(n)])].id);
          });
        }
        // Read the painter's state before leaving it: this is what the layer
        // would have been drawn with, and it is what the texture is drawn with.
        const QTransform toDevice = painter.combinedTransform();
        const double opacity = painter.opacity();
        // Flush Qt's queued geometry before changing its GL program, VAO,
        // textures and framebuffer, then invalidate Qt's cached GL state.
        painter.beginNativePainting();
        struct EndNative {
          QPainter &p;
          ~EndNative() { p.endNativePainting(); }
        } endNative{painter};
        return state->effectedRaster(scene, l, t, logical, rasterScale, box, raster,
                                     masked, toDevice, opacity, painter.device());
      };
      const EffectLayerHook effectLayer = [&](QPainter &p, const Layer &l, const Scene &s, double t,
                                              const QPainterPath &area, QRect acting, QRect source,
                                              double scale, double opacity) {
        p.beginNativePainting();
        struct EndNative {
          QPainter &p;
          ~EndNative() { p.endNativePainting(); }
        } endNative{p};
        const bool done = state->effectLayer(s, l, t, area, acting, source, scale, opacity, p.device());
        if (!done) state->effectLayerFailed = true;
        return done;
      };
      EffectLayerHookScope onCard(&effectLayer);
      const ParticleCardHook particles = [&](QPainter &p, const std::vector<ParticleMark> &marks,
                                             const ParticleOutlines &outlines, QRect area, int shape, bool add) {
        return state->particles(p, marks, outlines, area, shape, add);
      };
      ParticleCardScope particlesOnCard(&particles);
      const ImageCardHook pictures = [&](QPainter &p, const QRectF &box, const QImage &image) {
        return residentReady && state->picture(p, box, image);
      };
      ImageCardScope picturesOnCard(&pictures);
      const LitFaceHook litFaces = [&](QPainter &p, QRect dirty, const std::function<void(QPainter &)> &paintFace,
                                       const QTransform &localToDevice, const QMatrix4x4 &world,
                                       const SceneLighting &lighting, float shine, float gloss, int self) {
        return residentReady && state->litFace(p, dirty, paintFace, localToDevice, world, lighting, shine, gloss, self);
      };
      LitFaceScope litOnCard(&litFaces);
      const BlendCardHook blends = [&](QPainter &p, QRect dirty, QPainter::CompositionMode mode,
                                       const std::function<void(QPainter &, const LayerEffectHook *)> &paintAlone) {
        return residentReady && state->blendLayer(scene, p, dirty, mode, paintAlone);
      };
      BlendCardScope blendsOnCard(&blends);
      const MatteCardHook mattes = [&](QPainter &p, QRect dirty, QPainter::CompositionMode mode, bool inverted,
                                       const std::function<void(QPainter &, const LayerEffectHook *)> &paintOwner,
                                       const std::function<void(QPainter &, const LayerEffectHook *)> &paintMatte) {
        return residentReady && state->combineLayer(scene, p, dirty, mode, inverted, paintOwner, &paintMatte);
      };
      MatteCardScope mattesOnCard(&mattes);
      const MotionBlurCardHook blurs = [&](QPainter &p, QRect dirty, QPainter::CompositionMode mode, int count,
                                           const std::function<void(QPainter &, int, const LayerEffectHook *)> &paintSample) {
        return residentReady && state->motionBlurLayer(scene, p, dirty, mode, count, paintSample);
      };
      MotionBlurCardScope blursOnCard(&blurs);
      const ShapeCardHook shapes = [&](QPainter &p, const Layer &l, const ShapeFills *fills) {
        if (fills) return state->shapes(p, *fills);
        const auto ready = state->preparedFills.constFind(l.id);
        return ready != state->preparedFills.cend() && *ready && state->shapes(p, **ready);
      };
      ShapeCardScope shapesOnCard(&shapes);
      paintComposition(painter, scene, time, frame, size, &prepared,
                       residentReady ? &resident : nullptr);
    }
    // An Effect Layer the card could not work leaves a picture that is wrong,
    // not merely slow: the whole frame goes to the CPU instead.
    if (state->effectLayerFailed) {
      state->detail = "An Effect Layer could not be worked on the GPU; using CPU";
      return {};
    }
    // Resolve coverage at the same pixel dimensions, before temporal samples
    // are accumulated. Multisample FBOs have no ordinary sampler2D texture.
    functions->glDisable(GL_SCISSOR_TEST);
    functions->glBindFramebuffer(GL_READ_FRAMEBUFFER, state->sample->handle());
    functions->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state->sampleResolved->handle());
    functions->glBlitFramebuffer(0, 0, region.width(), region.height(),
                                0, 0, region.width(), region.height(),
                                GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // Restore BOTH bindings. Leaving READ on the multisample target makes
    // subsequent image readback read the wrong FBO (or fail on MSAA storage).
    functions->glBindFramebuffer(GL_FRAMEBUFFER, state->sampleResolved->handle());
    if (samples > 1)
      state->transfer(state->sampleResolved->texture(), *state->accumulation, 1.f / samples, true);
  }
  if (samples > 1) {
    state->clear(*state->resolved);
    state->transfer(state->accumulation->texture(), *state->resolved, 1.f, false);
  }
  // The frame's one question (see checkEachStep), with whatever a step found.
  GLenum error = functions->glGetError();
  if (error == GL_NO_ERROR) error = state->frameError;
  QImage image;
  if (error == GL_NO_ERROR) {
    auto &finished = samples == 1 ? *state->sampleResolved : *state->resolved;
    // At 8 bpc the card hands the frame over as it is wanted (readStraight8);
    // a deeper project, or a driver that will not, takes Qt's way.
    if (scene.processingDepth <= 8) image = state->readStraight8(finished);
    if (image.isNull()) image = finished.toImage().convertToFormat(processingFormat(scene.processingDepth));
  } else if (!state->checkEachStep) {
    // The first error this card has made. Which step made it is not known, so
    // the whole frame goes to the CPU; from now on each step checks its own.
    state->detail = QString("GPU error %1 in a frame; that frame drawn on the CPU").arg(error);
    state->frameError = error;
  } else {
    state->detail = QString("GPU rendering error %1; using CPU").arg(error);
    state->ready = false;
  }
  if (blurRoute && !image.isNull()) blurRoute->took(blurRouteSize, perLayerBlur, drawing.nsecsElapsed() / 1e6);
  return image;
}
QString GpuRenderer::declineReason(const Scene &input, double frame) {
  // While the blend check itself is drawing, its probe is not asked about.
  return declineReason(input, frame, [this] { return blendModesExact(); }, !state->checkingBlend);
}
QString GpuRenderer::declineReason(const Scene &input, double frame, const std::function<bool()> &blendModesExact,
                                   bool askAboutBlends) {
  if (sceneIsolates(input)) return declineReason(isolatedScene(input), frame, blendModesExact, askAboutBlends);
  const Scene &scene = input;
  ExpressionSceneScope expressions(&scene);
  if (scene.processingDepth > 16) return "32 bpc uses the precision CPU renderer";
  // An Effect Layer works on the picture beneath it, which here is on the
  // card: effectLayer() below does that there. One whose effects have no form
  // on the card would be worked on the CPU in the middle of the frame -- the
  // picture read back and sent up again for every motion-blur sample, on one
  // thread -- and that measured slower than the CPU drawing the whole frame
  // (the Twitter promo's grain and vignette: 119 ms a frame against 55). Such
  // a frame stays the CPU's until those effects have a form here.
  if (!effectLayersRunOnCard(scene, frame)) return "An Effect Layer uses effects not yet on the GPU; using CPU";
  // Blended and matted layers are flattened and combined on the card itself
  // (combineLayer). One the card cannot take there -- under a clip that is not
  // a rectangle -- goes through Qt's GL painter, which draws a blend mode
  // correctly only where the driver supports it and elsewhere quietly draws
  // Normal; so a frame that uses one goes to the card only where blending has
  // been shown to match. (Once they were flattened on the CPU, and a frame
  // mostly of them was handed back whole: 98 ms against the CPU's 40 for six
  // blended titles with a moving effect. On the card they take 13.)
  if (askAboutBlends && sceneUsesBlendModes(scene, frame) && !blendModesExact())
    return "This driver does not blend layers like the CPU; blend modes render on the CPU";
  return {};
}
bool GpuRenderer::blendModesExact() {
  if (state->blendCheck) return state->blendCheck == 1;
  state->checkingBlend = true;
  bool decided = true, exact = true;
  for (const char *mode : {"multiply", "screen", "overlay", "softLight", "difference", "add"}) {
    Scene probe;
    probe.width = probe.height = 32;
    probe.frames = 1;
    Layer top;
    top.id = "top"; top.type = "fill"; top.color = QColor(90, 160, 220, 200);
    top.blendMode = mode; top.duration = 1;
    Layer bottom;
    bottom.id = "bottom"; bottom.type = "fill"; bottom.color = QColor(200, 120, 60); bottom.duration = 1;
    probe.layers = {top, bottom};
    const QImage card = render(probe, 0, QSize(32, 32));
    if (card.isNull()) { decided = false; break; }
    QImage cpu;
    { CpuPixelScope only; cpu = renderCPU(probe, 0, false, QSize(32, 32)); }
    const QImage a = card.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const QImage b = cpu.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (a.size() != b.size()) { exact = false; break; }
    int worst = 0;
    for (int y = 0; y < a.height() && exact; ++y) {
      const uchar *p = a.constScanLine(y), *q = b.constScanLine(y);
      for (int x = 0; x < a.width() * 4; ++x) worst = std::max(worst, std::abs(int(p[x]) - int(q[x])));
    }
    if (worst > 3) { exact = false; break; }
  }
  state->checkingBlend = false;
  // Not decided means the card could not be used from here at all, not that it
  // blends wrongly: ask again next time.
  if (!decided) return false;
  state->blendCheck = exact ? 1 : 2;
  return exact;
}

// ---------------------------------------------------------------------------
struct GpuRenderService::Worker {
  std::thread thread;
  std::mutex guard;
  std::condition_variable wake;
  std::deque<std::function<void()>> jobs;
  bool stopping = false;
  std::unique_ptr<GpuRenderer> renderer; // made, used and destroyed on `thread`
  mutable std::mutex detailGuard;
  QString detail = "GPU has not been initialized";
  GpuResidentCacheStats stats;
  std::atomic<int> blendCheck{0}; // 0 not yet known, 1 exact, 2 not
  std::atomic<int> works{0};      // 0 not yet known, 1 draws, 2 does not
  Worker() {
    // Its window is made here, on the interface thread that starts the service;
    // the thread below makes only its context.
    QOffscreenSurface *surface = surfacePool().take();
    thread = std::thread([this, surface] {
      renderer = std::make_unique<GpuRenderer>();
      renderer->state->surface = surface;
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock<std::mutex> lock(guard);
          wake.wait(lock, [this] { return stopping || !jobs.empty(); });
          if (jobs.empty()) break;
          job = std::move(jobs.front());
          jobs.pop_front();
        }
        job();
        std::lock_guard<std::mutex> lock(detailGuard);
        detail = renderer->description();
        stats = renderer->residentCacheStats();
      }
      renderer.reset();
    });
    // First of all: whether the card draws at all, and whether it blends
    // layers as the CPU does -- each answered once, before any frame needs it.
    post([this] {
      try {
        works = renderer->render(Scene(), 0, QSize(16, 16)).isNull() ? 2 : 1;
        if (works == 1) blendCheck = renderer->blendModesExact() ? 1 : 2;
      } catch (...) { works = 2; }
    });
  }
  ~Worker() {
    {
      std::lock_guard<std::mutex> lock(guard);
      stopping = true;
    }
    wake.notify_all();
    if (thread.joinable()) thread.join();
  }
  void post(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(guard);
      jobs.push_back(std::move(job));
    }
    wake.notify_one();
  }
  template <typename F> auto run(F &&work) -> decltype(work()) {
    using Result = decltype(work());
    auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<F>(work));
    auto done = task->get_future();
    post([task] { (*task)(); });
    return done.get();
  }
};
GpuRenderService::GpuRenderService() : worker(std::make_unique<Worker>()) {}
GpuRenderService::~GpuRenderService() = default;
QImage GpuRenderService::render(const Scene &scene, double frame, QSize size, QRect region, double *drawnMs) {
  if (worker->works == 2) return {};
  return worker->run([&] {
    QElapsedTimer clock;
    clock.start();
    QImage image = worker->renderer->render(scene, frame, size, region);
    if (drawnMs) *drawnMs = clock.nsecsElapsed() / 1e6;
    return image;
  });
}
QString GpuRenderService::declineReason(const Scene &scene, double frame) {
  return GpuRenderer::declineReason(scene, frame, [this] { return worker->blendCheck == 1; });
}
QString GpuRenderService::description() const {
  std::lock_guard<std::mutex> lock(worker->detailGuard);
  return worker->detail;
}
GpuResidentCacheStats GpuRenderService::residentCacheStats() const {
  std::lock_guard<std::mutex> lock(worker->detailGuard);
  return worker->stats;
}
bool GpuRenderService::usable() {
  if (worker->works == 0) worker->run([] { return 0; }); // the first job has run when this has
  return worker->works == 1;
}
void reserveGpuSurfaces(int count) { surfacePool().reserve(count); }

namespace {
std::atomic<int> offThreadWindows{0};
QtMessageHandler handlerBefore = nullptr;
// Qt's warning for a window made on another thread than the interface's, which
// preceded each of the crashes.
void countOffThreadWindows(QtMsgType type, const QMessageLogContext &context, const QString &text) {
  if (text.contains("outside the gui thread")) ++offThreadWindows;
  if (handlerBefore) handlerBefore(type, context, text);
}
}
// On the interface thread, with a card: the windows renderers draw with are made
// on it, wherever they draw.
int checkGpuSurfaces(QStringList &report) {
  int failures = 0;
  auto verdict = [&](bool ok, const QString &what) {
    if (!ok) ++failures;
    report << QString("Surfaces: %1: %2").arg(what, ok ? "PASS" : "FAIL");
  };
  offThreadWindows = 0;
  handlerBefore = qInstallMessageHandler(countOffThreadWindows);
  Scene scene;
  scene.width = 160;
  scene.height = 90;
  scene.motionBlur = false;
  Layer back;
  back.type = "fill";
  back.color = QColor(30, 60, 120);
  back.duration = 100;
  back.p[0].base = 80;
  back.p[1].base = 45;
  scene.layers = {back};
  const QImage cpu = renderCPU(scene, 0, true).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
  auto matches = [&](const QImage &drawn) {
    const QImage card = drawn.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (card.isNull() || card.size() != cpu.size()) return false;
    qint64 error = 0;
    for (int y = 0; y < cpu.height(); ++y)
      for (int x = 0; x < cpu.width() * 4; ++x) error += qAbs(int(card.constScanLine(y)[x]) - int(cpu.constScanLine(y)[x]));
    return error / (double(cpu.width()) * cpu.height() * 4) < 2;
  };
  {
    GpuRenderService service;
    verdict(matches(service.render(scene, 0, {})) && offThreadWindows == 0,
            "the card's own thread draws with a window made on the interface thread");
  }
  {
    // An export frame: a renderer of its own, on a thread of its own.
    reserveGpuSurfaces(1);
    QImage drawn;
    std::thread([&] { GpuRenderer elsewhere; drawn = elsewhere.render(scene, 0, {}); }).join();
    verdict(matches(drawn) && offThreadWindows == 0, "a renderer on another thread draws with a window reserved here");
  }
  {
    // None waiting: that frame goes to the CPU rather than a window being made
    // there, and the next is on the card, with a window made here meanwhile.
    std::vector<QOffscreenSurface *> borrowed = surfacePool().takeAllWaiting();
    std::promise<void> firstTried, tryAgain;
    std::future<void> tried = firstTried.get_future(), again = tryAgain.get_future();
    QImage before, after;
    QString why;
    std::thread drawer([&] {
      GpuRenderer elsewhere;
      before = elsewhere.render(scene, 0, {});
      why = elsewhere.description();
      firstTried.set_value();
      again.wait();
      after = elsewhere.render(scene, 0, {});
    });
    tried.wait();
    QCoreApplication::sendPostedEvents(); // the order for a window
    tryAgain.set_value();
    drawer.join();
    for (QOffscreenSurface *surface : borrowed) surfacePool().give(surface);
    verdict(before.isNull() && why.contains("not ready") && offThreadWindows == 0,
            "with none waiting, another thread's frame goes to the CPU and makes no window there");
    verdict(matches(after), "its next frame is on the card, with a window made here meanwhile");
  }
  qInstallMessageHandler(handlerBefore);
  return failures;
}
