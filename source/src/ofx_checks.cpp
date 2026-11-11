#include "ofx_host.h"
#include "ofx_internal.h"
#include "engine.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// --ofx-self-test: the example plug-ins from the OpenFX project, built beside
// the program in ofx-test, are found, describe themselves, and change a picture
// the way their own code says they do -- right way up, at their own depth,
// from several layers at once.
namespace {
const OfxEffectEntry *named(const std::vector<OfxEffectEntry> &all, const QString &name) {
  for (const auto &e : all)
    if (e.name == name) return &e;
  return nullptr;
}
QImage bytes(int w, int h) {
  QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
  for (int y = 0; y < h; ++y) {
    uchar *row = image.scanLine(y);
    for (int x = 0; x < w * 4; ++x) row[x] = uchar((y * 61 + x * 7 + 3) % 256);
  }
  return image;
}
QImage floats(int w, int h) {
  QImage image(w, h, QImage::Format_RGBA32FPx4_Premultiplied);
  for (int y = 0; y < h; ++y) {
    float *row = reinterpret_cast<float *>(image.scanLine(y));
    for (int x = 0; x < w; ++x) {
      row[x * 4 + 0] = .1f + .05f * x;
      row[x * 4 + 1] = .2f + .1f * y;
      row[x * 4 + 2] = .3f;
      row[x * 4 + 3] = y == 0 ? 1.f : .5f;
    }
  }
  return image;
}
bool near(float a, float b) { return std::abs(a - b) < 1e-5f; }

// Retimer, from the OpenFX project, reads its source at other times: at Speed
// 0.5 frame 10 is the source's frame 5, at 0.25 half of frame 2 and half of 3.
// Each frame it is given is a grey of its own, ten levels a frame; without
// frames to read it is given the one being drawn.
void checkOtherFrames(const std::vector<OfxEffectEntry> &plugs, const QString &where,
                      const std::function<void(const QString &)> &fail) {
  const OfxEffectEntry *retimer = named(plugs, "Retimer");
  if (!retimer) {
    fail(where + ": Retimer was not taken");
    return;
  }
  if (!retimer->otherFrames) fail(where + ": Retimer did not say it reads other frames");
  auto grey = [](int level) {
    QImage image(16, 8, QImage::Format_RGBA8888_Premultiplied);
    image.fill(QColor(level, level, level));
    return image;
  };
  auto isGrey = [](const QImage &image, int level) {
    const QImage bytes = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < bytes.height(); ++y) {
      const uchar *row = bytes.constScanLine(y);
      for (int x = 0; x < bytes.width(); ++x)
        for (int k = 0; k < 3; ++k)
          if (std::abs(int(row[x * 4 + k]) - level) > 1) return false;
    }
    return true;
  };
  std::mutex askedGuard;
  QStringList asked;
  OfxOtherTimes frames;
  frames.first = 0;
  frames.last = 20;
  frames.at = [&](double t) {
    std::lock_guard<std::mutex> lock(askedGuard);
    asked << QString::number(t);
    return grey(int(10 * t + 5));
  };
  int speed = -1;
  QVector<float> values;
  for (const auto &c : retimer->controls) {
    if (!ofxControlHasValues(c)) continue;
    if (c.id == "Speed") speed = values.size();
    if (c.kind == OfxControl::Colour) values << 1 << 1 << 1 << 1;
    else values << float(c.initial);
  }
  if (speed < 0) {
    fail(where + ": Retimer has no Speed control");
    return;
  }
  const struct { float speed; int level; } cases[] = {{.5f, 55}, {.25f, 30}};
  for (const auto &c : cases) {
    values[speed] = c.speed;
    QImage image = grey(105);
    QString error;
    asked.clear();
    if (!ofxRender(*retimer, where + "|retimer", image, 10, 30, values, 1, &error, {&frames}))
      fail(QString("%1: Retimer at speed %2 did not draw: %3").arg(where).arg(c.speed).arg(error));
    else if (!isGrey(image, c.level))
      fail(QString("%1: Retimer at speed %2 drew %3, not frame %4 (asked for %5)")
               .arg(where).arg(c.speed).arg(image.convertToFormat(QImage::Format_RGBA8888).pixelColor(0, 0).red())
               .arg((c.level - 5) / 10.).arg(asked.join(", ")));
  }
  // Speed animated, rising from 0 at frame 0 by .1 a frame: summed to frame 10
  // it is 5, so frame 10 is the source's frame 5.
  {
    OfxOtherTimes rising = frames;
    const int at = speed;
    const QVector<float> others = values;
    rising.values = [at, others](const QVector<double> &when) {
      QVector<QVector<float>> all;
      for (double t : when) {
        QVector<float> v = others;
        v[at] = float(.1 * t);
        all << v;
      }
      return all;
    };
    rising.keys.fill({}, int(std::count_if(retimer->controls.begin(), retimer->controls.end(), ofxControlHasValues)));
    rising.keys[speed] = {0, 10};
    values[speed] = 1.f; // .1 * 10, as it is at frame 10
    QImage image = grey(105);
    QString error;
    if (!ofxRender(*retimer, where + "|retimer", image, 10, 30, values, 1, &error, {&rising}))
      fail(where + ": Retimer with Speed animated did not draw: " + error);
    else if (!isGrey(image, 55))
      fail(QString("%1: Retimer with Speed animated drew %2, not frame 5")
               .arg(where).arg(image.convertToFormat(QImage::Format_RGBA8888).pixelColor(0, 0).red()));
  }
  values[speed] = .5f;
  QImage alone = grey(105);
  QString error;
  if (!ofxRender(*retimer, where + "|retimer", alone, 10, 30, values, 1, &error) || !isGrey(alone, 105))
    fail(where + ": Retimer without other frames did not keep to the one it was given: " + error);
  ofxDropInstances([&where](const QString &instance) { return instance != where + "|retimer"; });
}

// Spill on a layer, as the engine draws it: the raster is grown by what the
// plug-in says it reaches, so the band shows around the box, not cut off at
// its edge. Thirty wide, well past the dozen pixels every raster has spare.
void checkReachOnLayer(const std::vector<OfxEffectEntry> &tests, const QString &where,
                       const std::function<void(const QString &)> &fail) {
  const OfxEffectEntry *spill = named(tests, "Wavy-Flo Test Spill");
  if (!spill) return; // said already
  Scene s;
  s.width = 160;
  s.height = 100;
  s.fps = 30;
  s.frames = 30;
  s.motionBlur = false;
  s.background = Qt::black;
  Layer box;
  box.id = "spilling";
  box.type = "shape";
  box.shapeKind = "rect";
  box.duration = 30;
  box.color = Qt::white;
  box.p[0].base = 80;
  box.p[1].base = 50;
  box.p[ShapeWidth].base = 40;
  box.p[ShapeHeight].base = 20;
  box.appliedEffects["Spill"] = true;
  s.layers = {box};
  const QByteArray run = QByteArray::number(QDateTime::currentMSecsSinceEpoch()) + where.toUtf8();
  QString problem;
  HostLayerEffects host;
  host.active = [](const Layer &l) { return l.appliedEffects.value("Spill").toBool(); };
  host.identity = [run](const Layer &, double t, const Scene &) { return QByteArray::number(t) + run + "spill"; };
  host.reach = [&](const Layer &l, double t, QSizeF content) {
    return ofxRegionReach(*spill, where + "|layer-spill", content, t, s.fps, {30});
  };
  host.apply = [&](const Layer &, double t, const QString &, QImage &image, double scale, const HostEffectRender &render) {
    OfxRenderOptions options;
    options.content = render.content.toAlignedRect().intersected(image.rect());
    QString error;
    if (!ofxRender(*spill, where + "|layer-spill", image, t, s.fps, {30}, scale, &error, options)) problem = error;
  };
  const HostLayerEffects *was = hostLayerEffects();
  setHostLayerEffects(&host);
  s.frame = 0;
  const QImage drawn = renderCPU(s, 0, false).convertToFormat(QImage::Format_ARGB32);
  setHostLayerEffects(was);
  ofxDropInstances([&where](const QString &instance) { return instance != where + "|layer-spill"; });
  if (!problem.isEmpty()) fail(where + ": Spill on a layer: " + problem);
  // The box runs 60 to 100 across and 40 to 60 down; the band 30 beyond that.
  const struct { int x, y, red; const char *what; } expected[] = {
      {80, 50, 255, "the box"}, {35, 50, 128, "the band left of it"}, {125, 50, 128, "the band right of it"},
      {80, 15, 128, "the band above it"}, {80, 85, 128, "the band below it"}, {80, 95, 0, "below the band"},
      {25, 50, 0, "left of the band"}};
  // Where anything was drawn at all, for the message.
  int top = -1, bottom = -1, left = -1, right = -1;
  for (int y = 0; y < drawn.height(); ++y)
    if (qRed(drawn.pixel(80, y)) > 3) { if (top < 0) top = y; bottom = y; }
  for (int x = 0; x < drawn.width(); ++x)
    if (qRed(drawn.pixel(x, 50)) > 3) { if (left < 0) left = x; right = x; }
  for (const auto &e : expected) {
    const int red = qRed(drawn.pixel(e.x, e.y));
    if (std::abs(red - e.red) > 3)
      fail(QString("%1: Spill on a layer drew %2 at %3,%4, %5, where %6 was due (drawn %7 to %8 across, %9 to %10 down)")
               .arg(where).arg(red).arg(e.x).arg(e.y).arg(e.what).arg(e.red).arg(left).arg(right).arg(top).arg(bottom));
  }
}

// The contexts other than a filter. A generator's picture takes the layer's
// place: the test generator fills it with an opaque grey of Level. Cross Fade,
// from the OpenFX project, is a transition: leaving, the layer fades to
// nothing as Transition goes from 0 to 1; arriving, it fades in from nothing.
void checkContexts(const std::vector<OfxEffectEntry> &tests, const std::vector<OfxEffectEntry> &conformance,
                   const QString &where, const std::function<void(const QString &)> &fail) {
  auto level = [](const QImage &image, int k) {
    return int(image.convertToFormat(QImage::Format_RGBA8888_Premultiplied).constScanLine(image.height() / 2)[image.width() / 2 * 4 + k]);
  };
  auto even = [](const QImage &image) {
    const QImage bytes = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const uchar *corner = bytes.constScanLine(0);
    for (int y = 0; y < bytes.height(); ++y)
      for (int x = 0; x < bytes.width(); ++x)
        for (int k = 0; k < 4; ++k)
          if (std::abs(int(bytes.constScanLine(y)[x * 4 + k]) - int(corner[k])) > 1) return false;
    return true;
  };
  if (const OfxEffectEntry *generator = named(tests, "Wavy-Flo Test Generator")) {
    if (generator->kind != "generator") fail(where + ": the test generator was taken as a " + generator->kind);
    if (generator->controls.size() != 1 || generator->controls[0].id != "Level")
      fail(where + ": the test generator's Level is not its one control");
    QImage image = bytes(21, 9);
    QString error;
    if (!ofxRender(*generator, where + "|generator", image, 0, 30, {.25f}, 1, &error))
      fail(where + ": the test generator did not draw: " + error);
    else if (image.size() != QSize(21, 9) || !even(image) || std::abs(level(image, 0) - 64) > 1 ||
             std::abs(level(image, 1) - 64) > 1 || level(image, 2) != 0 || level(image, 3) != 255)
      fail(QString("%1: the test generator drew %2,%3,%4/%5 where an even 64,64,0/255 was due")
               .arg(where).arg(level(image, 0)).arg(level(image, 1)).arg(level(image, 2)).arg(level(image, 3)));
    // Level animated, from 0 at frame 0 to 1 at frame 100 with a key between:
    // at frame 10 the plug-in reads .1 there, .09 a frame earlier, and three
    // keys.
    OfxOtherTimes times;
    times.values = [](const QVector<double> &when) {
      QVector<QVector<float>> values;
      for (double t : when) values << QVector<float>{float(std::clamp(t / 100, 0., 1.))};
      return values;
    };
    times.keys = {{0, 50, 100}};
    times.first = 0;
    times.last = 100;
    QImage animated = bytes(21, 9);
    if (!ofxRender(*generator, where + "|generator", animated, 10, 30, {.1f}, 1, &error, {&times}))
      fail(where + ": the test generator did not draw when animated: " + error);
    else if (std::abs(level(animated, 0) - 26) > 1 || std::abs(level(animated, 1) - 23) > 1 || level(animated, 2) != 30)
      fail(QString("%1: animated, the test generator drew %2,%3,%4 where 26,23,30 was due (its value now, a frame "
                   "before, and ten for each key)").arg(where).arg(level(animated, 0)).arg(level(animated, 1)).arg(level(animated, 2)));
    // Asked for a draft -- a preview that is playing -- it says it was.
    QImage draft = bytes(21, 9);
    if (!ofxRender(*generator, where + "|generator", draft, 10, 30, {.1f}, 1, &error, {nullptr, true}))
      fail(where + ": the test generator did not draw a draft: " + error);
    else if (level(draft, 2) != 200)
      fail(QString("%1: the test generator was not asked for a draft (blue %2, 200 due)").arg(where).arg(level(draft, 2)));
  } else {
    fail(where + ": the test generator was not taken");
  }
  const OfxEffectEntry *fade = named(conformance, "Cross Fade");
  if (!fade) {
    fail(where + ": Cross Fade was not taken");
    return;
  }
  if (fade->kind != "transition") fail(where + ": Cross Fade was taken as a " + fade->kind);
  if (fade->controls.size() < 2 || fade->controls[0].id != "Transition" || fade->controls[1].kind != OfxControl::Choice) {
    fail(where + ": Cross Fade's controls are not Transition and the layer's direction");
    return;
  }
  QVector<float> values;
  for (const auto &c : fade->controls)
    if (ofxControlHasValues(c)) values << float(c.initial);
  const struct { float transition, direction; int expected; const char *what; } cases[] = {
      {0, 0, 200, "leaving, at the start"}, {.25f, 0, 150, "leaving, a quarter of the way"},
      {1, 0, 0, "leaving, at the end"},     {.25f, 1, 50, "arriving, a quarter of the way"},
      {1, 1, 200, "arriving, at the end"},  {0, 1, 0, "arriving, at the start"}};
  for (const auto &c : cases) {
    values[0] = c.transition;
    values[1] = c.direction;
    QImage image(12, 6, QImage::Format_RGBA8888_Premultiplied);
    image.fill(QColor(255, 255, 255, 200)); // 200 in every channel, premultiplied
    QString error;
    if (!ofxRender(*fade, where + "|fade", image, 0, 30, values, 1, &error))
      fail(QString("%1: Cross Fade %2 did not draw: %3").arg(where, c.what, error));
    else if (!even(image) || std::abs(level(image, 0) - c.expected) > 2 || std::abs(level(image, 3) - c.expected) > 2)
      fail(QString("%1: Cross Fade %2 gave %3/%4, not %5").arg(where, c.what).arg(level(image, 0)).arg(level(image, 3)).arg(c.expected));
  }
  ofxDropInstances([&where](const QString &instance) { return !instance.startsWith(where + "|"); });
}

// A retimer, and what plug-ins say: the test Retime plug-in is taken as a
// retimer with Time Offset as its control -- at frame 10, an offset of -5
// shows frame 5 -- and says so in passing; while its source time is before
// the layer starts it keeps an error up, taken back when it is not. Its
// Analyse button is heard going a quarter at a time, and stops when told.
void checkRetimeAndMessages(const std::vector<OfxEffectEntry> &tests, const QString &where,
                            const std::function<void(const QString &)> &fail) {
  const OfxEffectEntry *retime = named(tests, "Wavy-Flo Test Retime");
  if (!retime) {
    fail(where + ": the test Retime plug-in, a retimer only, was not taken");
    return;
  }
  if (retime->kind != "retimer" || retime->controls.isEmpty() || retime->controls[0].id != "WavyFlo.TimeOffset") {
    fail(where + ": the Retime plug-in was taken as a " + retime->kind + " without its Time Offset");
    return;
  }
  auto grey = [](int level) {
    QImage image(12, 6, QImage::Format_RGBA8888_Premultiplied);
    image.fill(QColor(level, level, level));
    return image;
  };
  OfxOtherTimes frames;
  frames.first = 0;
  frames.last = 20;
  frames.at = [&](double t) { return grey(int(10 * t + 5)); };
  OfxRenderOptions options;
  options.times = &frames;
  const QString instance = where + "|retime";
  ofxTakeNotices();
  QImage image = grey(105);
  QString error;
  if (!ofxRender(*retime, instance, image, 10, 30, {-5}, 1, &error, options) || std::abs(qRed(image.pixel(3, 3)) - 55) > 1)
    fail(QString("%1: Retime with an offset of -5 at frame 10 drew %2, not frame 5 (%3)")
             .arg(where).arg(qRed(image.pixel(3, 3))).arg(error));
  const QStringList heard = ofxTakeNotices();
  if (!heard.contains("Wavy-Flo Test Retime: showing frame 5"))
    fail(where + ": what Retime said in passing was not heard: " + heard.join(" | "));
  if (!ofxEffectMessage(*retime, instance).isEmpty())
    fail(where + ": Retime keeps saying something with nothing wrong: " + ofxEffectMessage(*retime, instance));
  image = grey(105);
  ofxRender(*retime, instance, image, 10, 30, {-15}, 1, &error, options);
  const QString kept = ofxEffectMessage(*retime, instance);
  if (kept != "error: Source time -5 is before the layer starts")
    fail(where + ": Retime before the layer starts kept up \"" + kept + "\", not its error");
  image = grey(105);
  ofxRender(*retime, instance, image, 10, 30, {0}, 1, &error, options);
  if (!ofxEffectMessage(*retime, instance).isEmpty())
    fail(where + ": Retime's error stayed up after it took it back: " + ofxEffectMessage(*retime, instance));
  ofxTakeNotices();
  // Its button: heard going, and stopped half way.
  QVector<double> steps;
  std::mutex stepsGuard;
  QVector<float> values{0};
  QMap<QString, QString> texts;
  const bool pressed = ofxPressButton(*retime, instance, "Analyse", 10, 30, values, texts, grey(10), &error,
                                      [&](const QString &label, double fraction) {
                                        std::lock_guard<std::mutex> lock(stepsGuard);
                                        if (label == "Analysing the layer") steps << fraction;
                                        return true;
                                      });
  if (!pressed || steps != QVector<double>{0, .25, .5, .75, 1, 1})
    fail(QString("%1: Analyse was heard going %2, not 0, .25, .5, .75, 1, 1 (%3)").arg(where)
             .arg([&] { QStringList s; for (double v : steps) s << QString::number(v); return s.join(", "); }()).arg(error));
  error.clear();
  if (ofxPressButton(*retime, instance, "Analyse", 10, 30, values, texts, grey(10), &error,
                     [](const QString &, double fraction) { return fraction < .5; }) ||
      !error.contains("Analysis stopped at step 2"))
    fail(where + ": Analyse told to stop half way did not stop there: " + error);
  ofxTakeNotices();
  ofxDropInstances([&instance](const QString &name) { return name != instance; });
}

// A second picture: the test Matte plug-in needs one, Matte, and keeps the
// layer only where its alpha is. Taken all the same, with Matte as an input;
// given a matte, half of it opaque, half of the layer goes; given none, the
// layer comes through as it is.
void checkInputs(const std::vector<OfxEffectEntry> &tests, const QString &where,
                 const std::function<void(const QString &)> &fail) {
  const OfxEffectEntry *matte = named(tests, "Wavy-Flo Test Matte");
  if (!matte) {
    fail(where + ": the test Matte plug-in, which needs a second picture, was not taken");
    return;
  }
  const OfxControl *input = nullptr;
  for (const auto &c : matte->controls)
    if (c.kind == OfxControl::Input) input = &c;
  if (!input || input->id != "Matte" || input->optional || !input->mask) {
    fail(where + ": the Matte plug-in's Matte is not a needed mask input");
    return;
  }
  QImage layer(8, 4, QImage::Format_RGBA8888_Premultiplied);
  layer.fill(Qt::white);
  QImage half(8, 4, QImage::Format_RGBA8888_Premultiplied);
  half.fill(Qt::transparent);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) half.setPixelColor(x, y, QColor(0, 0, 0, 255));
  OfxRenderOptions options;
  options.inputs.insert("Matte", half);
  QImage image = layer;
  QString error;
  if (!ofxRender(*matte, where + "|matte", image, 0, 30, {}, 1, &error, options)) {
    fail(where + ": the Matte plug-in did not draw with a matte: " + error);
  } else {
    const QColor kept = image.pixelColor(1, 1), gone = image.pixelColor(6, 2);
    if (kept.alpha() != 255 || kept.red() != 255 || gone.alpha() != 0)
      fail(QString("%1: with half a matte the layer is %2/%3 on the left and %4 alpha on the right, not white and gone")
               .arg(where).arg(kept.red()).arg(kept.alpha()).arg(gone.alpha()));
  }
  image = layer;
  if (!ofxRender(*matte, where + "|matte", image, 0, 30, {}, 1, &error) || image.pixelColor(6, 2).alpha() != 255)
    fail(where + ": without a matte the Matte plug-in did not leave the layer as it was: " + error);
  ofxDropInstances([&where](const QString &instance) { return instance != where + "|matte"; });
}

// The same on a layer: a white box with Matte on it, the matte a small square
// layer -- hidden, as a matte usually is -- to the box's left. As in the Main,
// the box shows only where the square is; stretched over the box, the square's
// own picture covers all of it.
void checkInputsOnLayer(const std::vector<OfxEffectEntry> &tests, const QString &where,
                        const std::function<void(const QString &)> &fail) {
  const OfxEffectEntry *matte = named(tests, "Wavy-Flo Test Matte");
  if (!matte) return; // said already
  Scene s;
  s.width = 160;
  s.height = 100;
  s.fps = 30;
  s.frames = 30;
  s.motionBlur = false;
  s.background = Qt::black;
  Layer box;
  box.id = "box";
  box.type = "shape";
  box.shapeKind = "rect";
  box.duration = 30;
  box.color = Qt::white;
  box.p[0].base = 80;
  box.p[1].base = 50;
  box.p[ShapeWidth].base = 100;
  box.p[ShapeHeight].base = 60;
  box.appliedEffects["Matte"] = true;
  Layer dot = box;
  dot.id = "dot";
  dot.appliedEffects = {};
  dot.visible = false;
  dot.color = Qt::red;
  dot.p[0].base = 50;
  dot.p[ShapeWidth].base = 30;
  dot.p[ShapeHeight].base = 30;
  s.layers = {dot, box};
  const QByteArray run = QByteArray::number(QDateTime::currentMSecsSinceEpoch()) + where.toUtf8();
  bool stretched = false;
  QString problem;
  HostLayerEffects host;
  host.active = [](const Layer &l) { return l.appliedEffects.value("Matte").toBool(); };
  host.identity = [&](const Layer &, double t, const Scene &) {
    return QByteArray::number(t) + run + (stretched ? "stretched" : "placed");
  };
  host.apply = [&](const Layer &, double t, const QString &, QImage &image, double scale, const HostEffectRender &render) {
    OfxRenderOptions options;
    options.content = render.content.toAlignedRect().intersected(image.rect());
    const QImage picture = render.otherLayer ? render.otherLayer("dot", stretched) : QImage();
    if (picture.isNull()) problem = "no picture of the matte layer";
    else options.inputs.insert("Matte", picture);
    QString error;
    if (!ofxRender(*matte, where + "|layer-matte", image, t, s.fps, {}, scale, &error, options)) problem = error;
  };
  const HostLayerEffects *was = hostLayerEffects();
  setHostLayerEffects(&host);
  s.frame = 0;
  const QImage placed = renderCPU(s, 0, false).convertToFormat(QImage::Format_ARGB32);
  stretched = true;
  const QImage over = renderCPU(s, 0, false).convertToFormat(QImage::Format_ARGB32);
  setHostLayerEffects(was);
  ofxDropInstances([&where](const QString &instance) { return instance != where + "|layer-matte"; });
  if (!problem.isEmpty()) fail(where + ": Matte on a layer: " + problem);
  // The box runs 30 to 130 across, 20 to 80 down; the square 35 to 65 across, 35 to 65 down.
  const struct { const QImage *image; int x, y, red; const char *what; } expected[] = {
      {&placed, 50, 50, 255, "as in the Main, where the square is"},
      {&placed, 100, 50, 0, "as in the Main, in the box away from the square"},
      {&placed, 40, 25, 0, "as in the Main, in the box above the square"},
      {&over, 100, 50, 255, "stretched, in the box away from where the square is"},
      {&over, 125, 75, 255, "stretched, near the box's corner"},
      {&over, 140, 50, 0, "stretched, outside the box"}};
  for (const auto &e : expected) {
    const int red = qRed(e.image->pixel(e.x, e.y));
    if (std::abs(red - e.red) > 3)
      fail(QString("%1: Matte on a layer drew %2 at %3,%4, %5, where %6 was due").arg(where).arg(red).arg(e.x).arg(e.y)
               .arg(e.what).arg(e.red));
  }
}

// The controls that hold no number: the test Controls plug-in's button and
// words to read come first, as made, then its group folding its number --
// made before the group -- and its text; text typed reaches the plug-in; and its Measure button, pressed
// with a picture, comes back with what it measured written into its controls.
void checkControls(const std::vector<OfxEffectEntry> &tests, const QString &where,
                   const std::function<void(const QString &)> &fail) {
  const OfxEffectEntry *controls = named(tests, "Wavy-Flo Test Controls");
  if (!controls) {
    fail(where + ": the test Controls plug-in was not taken");
    return;
  }
  QStringList seen;
  static const char *kinds[] = {"number", "toggle", "choice", "colour", "text", "button", "group", "end"};
  for (const auto &c : controls->controls) seen << QString("%1 %2").arg(kinds[c.kind], c.id);
  const QStringList expected = {"button Measure", "text Note", "group Look", "number Amount", "text Caption", "end Look",
                                 "number Centre/x", "number Centre/y", "button EditLook"};
  if (seen != expected) fail(where + ": the Controls plug-in's controls came out as " + seen.join(", "));
  for (const auto &c : controls->controls) {
    if (c.id == "Note" && (c.textMode != OfxControl::Label || !c.text.startsWith("Press Measure")))
      fail(where + ": the Controls plug-in's note is not words to read: " + c.text);
    if (c.id.startsWith("Centre/") && (c.point != "Centre" || c.axis != (c.id.endsWith("x") ? 0 : 1) || !c.normalised))
      fail(where + ": the Controls plug-in's Centre is not a point with a handle: " + c.id);
  }
  auto greenOf = [](const QImage &image) { return qGreen(image.convertToFormat(QImage::Format_ARGB32).pixel(0, 0)); };
  auto redOf = [](const QImage &image) { return qRed(image.convertToFormat(QImage::Format_ARGB32).pixel(0, 0)); };
  QImage image(8, 4, QImage::Format_RGBA8888_Premultiplied);
  image.fill(Qt::black);
  QString error;
  // Its own caption, "hi": twenty levels of green. Typed, "abcd": forty.
  if (!ofxRender(*controls, where + "|controls", image, 0, 30, {.25f}, 1, &error) || greenOf(image) != 20 ||
      std::abs(redOf(image) - 64) > 1)
    fail(QString("%1: the Controls plug-in drew %2,%3 at its defaults, not 64,20 (%4)")
             .arg(where).arg(redOf(image)).arg(greenOf(image)).arg(error));
  OfxRenderOptions typed;
  typed.texts.insert("Caption", "abcd");
  image.fill(Qt::black);
  if (!ofxRender(*controls, where + "|controls", image, 0, 30, {.25f}, 1, &error, typed) || greenOf(image) != 40)
    fail(QString("%1: the Controls plug-in did not read the text typed (green %2, 40 due) %3")
             .arg(where).arg(greenOf(image)).arg(error));
  // Measure on a picture whose red is half way up.
  QImage picture(16, 8, QImage::Format_RGBA8888_Premultiplied);
  picture.fill(QColor(128, 30, 200));
  QVector<float> values{.25f};
  QMap<QString, QString> texts{{"Caption", "abcd"}};
  if (!ofxPressButton(*controls, where + "|controls", "Measure", 0, 30, values, texts, picture, &error))
    fail(where + ": the Controls plug-in's Measure could not be pressed: " + error);
  else if (values.size() != 1 || std::abs(values[0] - 128 / 255.f) > .01f || texts.value("Caption") != "measured")
    fail(QString("%1: Measure came back with Amount %2 and Caption \"%3\", not %4 and \"measured\"")
             .arg(where).arg(values.value(0)).arg(texts.value("Caption")).arg(128 / 255.));
  // A picture as large as an Edit Look is given (2560 wide) reaches the button
  // at once. Streamed to the plug-in host as a PNG of floats, a photo took about
  // a second (40 ms now), and a look editor's window came up that much later.
  QImage large(2560, 1440, QImage::Format_RGBA8888_Premultiplied);
  large.fill(QColor(128, 30, 200));
  QVector<float> largeValues{.25f};
  QMap<QString, QString> largeTexts{{"Caption", "abcd"}};
  QElapsedTimer pressing;
  pressing.start();
  const bool largePressed =
      ofxPressButton(*controls, where + "|controls", "Measure", 0, 30, largeValues, largeTexts, large, &error);
  const qint64 pressMs = pressing.elapsed();
  if (!largePressed || std::abs(largeValues.value(0) - 128 / 255.f) > .01f || pressMs > 1000)
    fail(QString("%1: Measure on a 2560 x 1440 picture took %2 ms (1000 at most) and read red %3 (%4)")
             .arg(where).arg(pressMs).arg(largeValues.value(0) * 255).arg(error));
  // Edit Look, as a look suite's own button: what it made has no control,
  // so it is kept (OfxEffectEntry::hidden) and comes back from the press; handed to
  // a copy of the effect made afresh (the project opened again) it draws the
  // look; handed back at its default (the look undone), it is gone again.
  const QMap<QString, QString> keptDue{{"LookData", ""}, {"LookVersion", "0"}};
  if (controls->hidden != keptDue)
    fail(QString("%1: what the Controls plug-in keeps without a control came out as %2")
             .arg(where, QStringList(controls->hidden.keys()).join(", ")));
  auto blueOf = [](const QImage &image) { return qBlue(image.convertToFormat(QImage::Format_ARGB32).pixel(0, 0)); };
  QVector<float> lookValues{.25f};
  QMap<QString, QString> look = controls->hidden;
  if (!ofxPressButton(*controls, where + "|controls", "EditLook", 0, 30, lookValues, look, picture, &error))
    fail(where + ": the Controls plug-in's Edit Look could not be pressed: " + error);
  else if (look.value("LookData") != "warm" || look.value("LookVersion") != "1")
    fail(QString("%1: Edit Look came back with the look \"%2\", version %3, not \"warm\", 1")
             .arg(where, look.value("LookData"), look.value("LookVersion")));
  OfxRenderOptions reopened;
  reopened.texts = look;
  image.fill(Qt::black);
  if (!ofxRender(*controls, where + "|look reopened", image, 0, 30, {.25f}, 1, &error, reopened) || blueOf(image) != 100)
    fail(QString("%1: the look kept with the layer was not drawn by a new copy of the effect (blue %2, 100 due) %3")
             .arg(where).arg(blueOf(image)).arg(error));
  OfxRenderOptions undone;
  undone.texts = controls->hidden;
  image.fill(Qt::black);
  if (!ofxRender(*controls, where + "|look reopened", image, 0, 30, {.25f}, 1, &error, undone) || blueOf(image) != 0)
    fail(QString("%1: the look handed back at its default was still drawn (blue %2, 0 due) %3")
             .arg(where).arg(blueOf(image)).arg(error));
  ofxDropInstances([&where](const QString &instance) { return instance != where + "|look reopened"; });
  QString nothing;
  QVector<float> unchanged{.25f};
  QMap<QString, QString> none;
  if (ofxPressButton(*controls, where + "|controls", "No Such Button", 0, 30, unchanged, none, picture, &nothing) ||
      nothing.isEmpty())
    fail(where + ": a button the plug-in does not have was pressed without a word");
  ofxDropInstances([&where](const QString &instance) { return instance != where + "|controls"; });
}

// Drawing on the card: the test Card plug-in draws only through OpenGL --
// its source as a texture, times Gain, into the framebuffer it is given -- and
// only once told of the context. A picture red above and blue below, at Gain
// one half, comes back half as bright, the right way up. On a machine with no
// OpenGL it says why; that is passed over only when WAVY_OFX_NO_CARD is set.
void checkCard(const std::vector<OfxEffectEntry> &tests, const QString &where,
               const std::function<void(const QString &)> &fail) {
  const OfxEffectEntry *card = named(tests, "Wavy-Flo Test Card");
  if (!card) {
    fail(where + ": the test Card plug-in, which draws only on the card, was not taken");
    return;
  }
  QImage image(16, 8, QImage::Format_RGBA8888_Premultiplied);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 16; ++x) image.setPixelColor(x, y, y < 4 ? QColor(200, 40, 0) : QColor(0, 40, 200));
  QString error;
  if (!ofxRender(*card, where + "|card", image, 0, 30, {.5f}, 1, &error)) {
    if (error.contains("gave it no OpenGL") && qEnvironmentVariableIsSet("WAVY_OFX_NO_CARD")) return;
    fail(where + ": the Card plug-in did not draw: " + error);
    return;
  }
  // Premultiplied, every channel halved: alpha 128 over colours that were
  // 200 and 40, now 100 and 20.
  const QRgb topRaw = image.convertToFormat(QImage::Format_ARGB32_Premultiplied).pixel(8, 1);
  const QRgb bottomRaw = image.convertToFormat(QImage::Format_ARGB32_Premultiplied).pixel(8, 6);
  if (std::abs(qRed(topRaw) - 100) > 3 || std::abs(qGreen(topRaw) - 20) > 3 || qBlue(topRaw) > 3 ||
      std::abs(qAlpha(topRaw) - 128) > 3 || std::abs(qBlue(bottomRaw) - 100) > 3 || qRed(bottomRaw) > 3)
    fail(QString("%1: on the card, red above and blue below at half came back %2,%3,%4,%5 above and %6,%7,%8,%9 below")
             .arg(where).arg(qRed(topRaw)).arg(qGreen(topRaw)).arg(qBlue(topRaw)).arg(qAlpha(topRaw)).arg(qRed(bottomRaw))
             .arg(qGreen(bottomRaw)).arg(qBlue(bottomRaw)).arg(qAlpha(bottomRaw)));
  ofxDropInstances([&where](const QString &instance) { return instance != where + "|card"; });
}

// A layer under a host's effect moved by fractions of a pixel -- dragged in
// the preview -- keeps the picture the effect made: the plug-in is not run
// again at each step (it was: 300 ms a mouse move with a commercial effect).
// A box, letters, and a 1080p picture in a preview drawn at 0.4375.
void checkMovingKeepsEffect(const QString &where, const std::function<void(const QString &)> &fail) {
  struct Case { const char *name; QString type; double x0, x1; int w = 200, h = 150; double scale = 1; };
  const std::vector<Case> cases = {{"a box", "shape", 100, 107.25}, {"a box half out of the frame", "shape", 21, 28.25},
                                   {"letters", "text", 102, 109.25},
                                   {"a 1080p picture in a preview at 0.4375", "image", 960, 973.7, 1920, 1080, .4375}};
  for (const Case &c : cases) {
    Scene s;
    s.width = c.w;
    s.height = c.h;
    s.fps = 30;
    s.frames = 30;
    s.motionBlur = false;
    s.background = Qt::black;
    Layer moved;
    moved.id = "moved";
    moved.type = c.type;
    moved.shapeKind = "rect";
    moved.text = "Move me";
    moved.fontSize = 40;
    moved.duration = 30;
    moved.color = Qt::white;
    moved.p[0].base = c.x0;
    moved.p[1].base = c.h / 2.;
    moved.p[ShapeWidth].base = 120;
    moved.p[ShapeHeight].base = 80;
    if (c.type == "image") {
      moved.image = QImage(c.w, c.h, QImage::Format_ARGB32_Premultiplied);
      moved.image.fill(QColor(40, 90, 160));
    }
    moved.appliedEffects["Counted"] = true;
    s.layers = {moved};
    const QSize out(qCeil(c.w * c.scale), qCeil(c.h * c.scale));
    int runs = 0;
    HostLayerEffects host;
    host.active = [](const Layer &l) { return l.appliedEffects.value("Counted").toBool(); };
    const QByteArray run = QByteArray::number(QDateTime::currentMSecsSinceEpoch()) + where.toUtf8() + c.name;
    host.identity = [run](const Layer &, double, const Scene &) { return run; };
    host.apply = [&](const Layer &, double, const QString &, QImage &image, double, const HostEffectRender &) {
      ++runs;
      image.invertPixels(QImage::InvertRgb);
    };
    const HostLayerEffects *was = hostLayerEffects();
    setHostLayerEffects(&host);
    s.frame = 0;
    const QImage before = renderCPU(s, 0, false, out);
    const int first = runs;
    QStringList again;
    QImage after;
    for (int step = 1; step <= 4; ++step) {
      s.layers[0].p[0].base = c.x0 + (c.x1 - c.x0) * step / 4.;
      s.layers[0].p[1].base = c.h / 2. + step * .7;
      const int ran = runs;
      after = renderCPU(s, 0, false, out);
      again << QString::number(runs - ran);
    }
    setHostLayerEffects(was);
    if (first != 1 || runs != first)
      fail(QString("%1: %2 under a host's effect, moved by fractions of a pixel four times, ran it %3 time(s) and then again %4")
               .arg(where, c.name).arg(first).arg(again.join(",")));
    if (before == after) fail(QString("%1: %2 did not move").arg(where, c.name));
  }
}

// A point on the preview where the plug-in has it: the Controls plug-in on a
// box turned 30 degrees draws its blue dot at Centre (a quarter across, three
// quarters up), and hostEffectPointToComposition -- where the handle for that
// point is drawn -- says the same place; and back again.
void checkPointsOnLayer(const std::vector<OfxEffectEntry> &tests, const QString &where,
                        const std::function<void(const QString &)> &fail) {
  const OfxEffectEntry *controls = named(tests, "Wavy-Flo Test Controls");
  if (!controls) return; // said already
  Scene s;
  s.width = 200;
  s.height = 150;
  s.fps = 30;
  s.frames = 30;
  s.motionBlur = false;
  s.background = Qt::black;
  Layer box;
  box.id = "pointed";
  box.type = "shape";
  box.shapeKind = "rect";
  box.duration = 30;
  box.color = Qt::white;
  box.p[0].base = 100;
  box.p[1].base = 75;
  box.p[4].base = 30;
  box.p[ShapeWidth].base = 120;
  box.p[ShapeHeight].base = 80;
  box.appliedEffects["Points"] = true;
  s.layers = {box};
  const QVector<float> values{.25f, .25f, .75f};
  const QByteArray run = QByteArray::number(QDateTime::currentMSecsSinceEpoch()) + where.toUtf8();
  QString problem;
  HostLayerEffects host;
  host.active = [](const Layer &l) { return l.appliedEffects.value("Points").toBool(); };
  host.identity = [run](const Layer &, double t, const Scene &) { return QByteArray::number(t) + run + "points"; };
  host.apply = [&](const Layer &, double t, const QString &, QImage &image, double scale, const HostEffectRender &render) {
    OfxRenderOptions options;
    options.content = render.content.toAlignedRect().intersected(image.rect());
    QString error;
    if (!ofxRender(*controls, where + "|points", image, t, s.fps, values, scale, &error, options)) problem = error;
  };
  const HostLayerEffects *was = hostLayerEffects();
  setHostLayerEffects(&host);
  s.frame = 0;
  const QImage drawn = renderCPU(s, 0, false).convertToFormat(QImage::Format_ARGB32);
  setHostLayerEffects(was);
  ofxDropInstances([&where](const QString &instance) { return instance != where + "|points"; });
  if (!problem.isEmpty()) fail(where + ": Controls on a turned layer: " + problem);
  const QRectF content = hostEffectContentBox(s.layers[0], s);
  const QPointF point(.25 * content.width(), .75 * content.height());
  const QPointF at = hostEffectPointToComposition(s.layers[0], s, 0, point);
  const QPoint pixel = at.toPoint();
  // The bluest pixel near where the handle goes, and how far off it is.
  int best = 0;
  QPoint bluest;
  for (int y = 0; y < drawn.height(); ++y)
    for (int x = 0; x < drawn.width(); ++x)
      if (qBlue(drawn.pixel(x, y)) > best) { best = qBlue(drawn.pixel(x, y)); bluest = QPoint(x, y); }
  if (best < 128 || QLineF(QPointF(bluest), at).length() > 2.5)
    fail(QString("%1: the handle is at %2,%3, the plug-in drew its dot at %4,%5 (blue %6)")
             .arg(where).arg(at.x(), 0, 'f', 1).arg(at.y(), 0, 'f', 1).arg(bluest.x()).arg(bluest.y()).arg(best));
  const QPointF back = compositionToHostEffectPoint(s.layers[0], s, 0, at);
  if (QLineF(back, point).length() > 1e-6)
    fail(QString("%1: the point back from the composition is %2,%3, not %4,%5")
             .arg(where).arg(back.x()).arg(back.y()).arg(point.x()).arg(point.y()));
  (void)pixel;
}

// A plug-in that draws beyond the layer: Spill says, as its region of
// definition, that it reaches Spill units past the picture on every side, and
// draws a grey band there. Given room around the layer's picture -- below it
// more than above, so which way up the room is counts -- the band lands in
// it, in the right place.
void checkReach(const std::vector<OfxEffectEntry> &tests, const QString &where,
                const std::function<void(const QString &)> &fail) {
  const OfxEffectEntry *spill = named(tests, "Wavy-Flo Test Spill");
  if (!spill) {
    fail(where + ": the test Spill was not taken");
    return;
  }
  const double ten = ofxRegionReach(*spill, where + "|spill", QSizeF(20, 10), 0, 30, {10});
  const double more = ofxRegionReach(*spill, where + "|spill", QSizeF(20, 10), 0, 30, {25});
  if (std::abs(ten - 10) > 1e-6 || std::abs(more - 25) > 1e-6)
    fail(QString("%1: Spill said it reaches %2 and %3, not 10 and 25").arg(where).arg(ten).arg(more));
  // A 50 x 40 image with the layer's picture, 20 x 10 and white, 15 from the
  // left and 12 from the top: the band is 5 to 45 across, 2 to 32 down.
  QImage image(50, 40, QImage::Format_RGBA8888_Premultiplied);
  image.fill(Qt::transparent);
  const QRect content(15, 12, 20, 10);
  for (int y = content.top(); y <= content.bottom(); ++y)
    for (int x = content.left(); x <= content.right(); ++x) image.setPixelColor(x, y, Qt::white);
  OfxRenderOptions options;
  options.content = content;
  QString error;
  if (!ofxRender(*spill, where + "|spill", image, 0, 30, {10}, 1, &error, options)) {
    fail(where + ": Spill did not draw: " + error);
    return;
  }
  auto at = [&](int x, int y) { return image.pixelColor(x, y); };
  const struct { int x, y; int red, alpha; const char *what; } expected[] = {
      {20, 15, 255, 255, "the picture itself"}, {10, 15, 128, 255, "the band to its left"},
      {6, 3, 128, 255, "the band's top left corner"}, {44, 31, 128, 255, "the band's bottom right corner"},
      {20, 1, 0, 0, "above the band"}, {20, 33, 0, 0, "below the band"}, {2, 20, 0, 0, "left of the band"}};
  for (const auto &e : expected) {
    const QColor c = at(e.x, e.y);
    if (std::abs(c.red() - e.red) > 2 || std::abs(c.alpha() - e.alpha) > 2)
      fail(QString("%1: Spill drew %2/%3 at %4,%5, %6, where %7/%8 was due")
               .arg(where).arg(c.red()).arg(c.alpha()).arg(e.x).arg(e.y).arg(e.what).arg(e.red).arg(e.alpha));
  }
  ofxDropInstances([&where](const QString &instance) { return instance != where + "|spill"; });
}

// The same in a layer's own stack, as the engine draws it: a box growing from
// 10 to 90 wide over 20 frames, Retimer at Speed 0.5. Frame 10 then shows the
// box as it is at frame 5 -- the layer drawn again at that time -- and not as
// it is at frame 10.
void checkOtherFramesOnLayer(const std::vector<OfxEffectEntry> &plugs, const QString &where,
                             const std::function<void(const QString &)> &fail) {
  const OfxEffectEntry *retimer = named(plugs, "Retimer");
  if (!retimer) return; // said already
  QVector<float> values;
  for (const auto &c : retimer->controls) {
    if (!ofxControlHasValues(c)) continue;
    if (c.kind == OfxControl::Colour) values << 1 << 1 << 1 << 1;
    else values << float(c.id == "Speed" ? .5 : c.initial);
  }
  Scene s;
  s.width = 128;
  s.height = 48;
  s.fps = 30;
  s.frames = 30;
  s.motionBlur = false;
  s.background = Qt::black;
  Layer box;
  box.id = "growing";
  box.type = "shape";
  box.shapeKind = "rect";
  box.duration = 30;
  box.color = Qt::white;
  box.p[0].base = 64;
  box.p[1].base = 24;
  box.p[ShapeWidth].set(0, 10);
  box.p[ShapeWidth].set(20, 90);
  box.p[ShapeHeight].base = 20;
  s.layers = {box};
  const QImage at5 = renderCPU(s, 5, false).convertToFormat(QImage::Format_ARGB32);
  const QImage at10 = renderCPU(s, 10, false).convertToFormat(QImage::Format_ARGB32);
  QString problem;
  std::atomic<int> asked{0};
  HostLayerEffects host;
  host.active = [](const Layer &l) { return l.appliedEffects.value("Retimer").toBool(); };
  // Each run its own picture: the second, in the plug-in host, must not be
  // the first one's taken from the cache.
  const QByteArray run = QByteArray::number(QDateTime::currentMSecsSinceEpoch()) + where.toUtf8();
  bool drewDraft = false;
  host.identity = [run](const Layer &, double t, const Scene &drawn) {
    return QByteArray::number(t) + run + (drawn.previewDraft ? "draft" : "");
  };
  host.apply = [&](const Layer &l, double t, const QString &, QImage &image, double scale, const HostEffectRender &render) {
    const auto &other = render.otherFrame;
    drewDraft = drewDraft || render.draft;
    OfxOtherTimes frames;
    frames.at = [&](double when) {
      ++asked;
      return other ? other(when) : QImage();
    };
    frames.first = 0;
    frames.last = l.duration - 1;
    QString error;
    if (!ofxRender(*retimer, where + "|layer", image, t, s.fps, values, scale, &error, {other ? &frames : nullptr}))
      problem = error;
    if (!other) problem = "no other frames were offered";
  };
  const HostLayerEffects *was = hostLayerEffects();
  setHostLayerEffects(&host);
  s.layers[0].appliedEffects["Retimer"] = true;
  const QImage retimed = renderCPU(s, 10, false).convertToFormat(QImage::Format_ARGB32);
  if (drewDraft) fail(where + ": a render that is not a playing preview was drawn as a draft");
  // The same frame for a preview that is playing: the effect is told so.
  Scene playing = s;
  playing.previewDraft = true;
  renderCPU(playing, 10, false);
  if (!drewDraft) fail(where + ": a playing preview's render was not drawn as a draft");
  setHostLayerEffects(was);
  ofxDropInstances([&where](const QString &instance) { return instance != where + "|layer"; });
  if (!problem.isEmpty()) fail(where + ": Retimer on a layer: " + problem);
  if (!asked) fail(where + ": Retimer on a layer never asked for another frame");
  // Lit columns, the box's width as drawn.
  auto width = [](const QImage &image) {
    int lit = 0;
    for (int x = 0; x < image.width(); ++x) lit += qRed(image.pixel(x, image.height() / 2)) > 128;
    return lit;
  };
  if (std::abs(width(retimed) - width(at5)) > 1)
    fail(QString("%1: Retimer on a layer drew a box %2 wide at frame 10; at frame 5 it is %3, at 10 %4")
             .arg(where).arg(width(retimed)).arg(width(at5)).arg(width(at10)));
}
// Every pixel of `after` is `scale` (r, g, b, a) times the one in `before`.
bool scaled(const QImage &before, const QImage &after, const float scale[4]) {
  if (after.format() != before.format() || after.size() != before.size()) return false;
  for (int y = 0; y < before.height(); ++y) {
    const float *a = reinterpret_cast<const float *>(before.constScanLine(y));
    const float *b = reinterpret_cast<const float *>(after.constScanLine(y));
    for (int k = 0; k < before.width() * 4; ++k)
      if (!near(b[k], a[k] * scale[k % 4])) return false;
  }
  return true;
}
} // namespace

int checkOfxHost(QStringList &failures, const QString &testPlugins) {
  auto fail = [&](const QString &what) { failures << what; };
  // Frames into a render and back the quick way give what Qt gives.
  for (const QString &problem : ofx_local::pictureConversionProblems()) fail("pictures in and out: " + problem);
  // Where the Support library's plug-ins (ofx-conformance, below) write what
  // they find wrong: named before anything is loaded, so that a plug-in host
  // process started by the first load has it too.
  QTemporaryDir logPlace;
  const QString log = logPlace.path() + "/support-log.txt";
  qputenv("OFX_PLUGIN_LOGFILE", QDir::toNativeSeparators(log).toLocal8Bit());
  QStringList problems;
  const auto all = ofxLoadEffects({testPlugins}, &problems);
  if (!problems.isEmpty()) fail("problems loading: " + problems.join("; "));
  const OfxEffectEntry *invert = named(all, "OFX Invert Example");
  const OfxEffectEntry *gain = named(all, "OFX Gain Example");
  if (!invert || !gain) {
    fail(QString("expected the Invert and Gain examples in %1, found %2").arg(QDir::toNativeSeparators(testPlugins)).arg(all.size()));
    return 1;
  }
  if (invert->group != "OFX Example" || invert->identifier != "uk.co.thefoundry.OfxInvertExample" || !invert->controls.isEmpty())
    fail("Invert describes itself wrongly: " + invert->group + " / " + invert->identifier);

  // Loaded once: the same folder again gives the same plug-ins back.
  const auto again = ofxLoadEffects({testPlugins});
  if (again.size() != all.size() || named(again, "OFX Gain Example")->plugin != gain->plugin)
    fail("the same folder was loaded twice");

  // Wavy-Flo's effects folder is read first, like After Effects' Plug-ins
  // folder: a bundle in a maker's folder inside it is found, and the same
  // plug-in in a second folder (the installed copy) is taken once.
  {
    const QStringList folders = ofxPluginFolders();
    if (folders.isEmpty() || folders.first() != QCoreApplication::applicationDirPath() + "/effects")
      fail("Wavy-Flo's effects folder is not the first read for plug-ins: " + folders.join("; "));
    QTemporaryDir place;
    const QString binary = testPlugins + "/invert.ofx.bundle/Contents/Win64/invert.ofx";
    auto copyTo = [&](const QString &folder) {
      const QString inside = place.path() + "/" + folder + "/invert.ofx.bundle/Contents/Win64";
      return QDir().mkpath(inside) && QFile::copy(binary, inside + "/invert.ofx");
    };
    if (!copyTo("effects/Maker/Deeper") || !copyTo("installed")) {
      fail("could not copy the Invert example for the folders check");
    } else {
      QStringList said;
      const auto found = ofxLoadEffects({place.path() + "/effects", place.path() + "/installed"}, &said);
      int inverts = 0;
      for (const OfxEffectEntry &entry : found) inverts += entry.name == "OFX Invert Example";
      if (inverts != 1 || said.join(";").indexOf("a copy of a plug-in found before it") < 0)
        fail(QString("a bundle in a maker's folder inside effects, and the same one in a second folder: %1 found (%2)")
                 .arg(inverts)
                 .arg(said.join("; ")));
    }
  }

  // Gain's settings, in the order it made them, its four channels folded in
  // the group it put them in; its page is not a control.
  const QStringList ids = {"scale", "scaleComponents", "componentScales", "scaleR", "scaleG", "scaleB", "scaleA",
                           "componentScales"};
  QStringList got;
  for (const auto &c : gain->controls) got << c.id;
  if (gain->controls.size() == ids.size() && (gain->controls[2].kind != OfxControl::GroupBegin ||
                                              gain->controls[7].kind != OfxControl::GroupEnd))
    fail("Gain's channels are not folded in their group");
  if (got != ids) fail("Gain's controls: " + got.join(", "));
  else {
    const OfxControl &scale = gain->controls[0];
    if (scale.kind != OfxControl::Number || scale.initial != 1 || scale.minimum != 0 || scale.label != "scale")
      fail(QString("Gain's scale: kind %1 initial %2 minimum %3 label %4").arg(scale.kind).arg(scale.initial).arg(scale.minimum).arg(scale.label));
    if (gain->controls[1].kind != OfxControl::Toggle || gain->controls[1].initial != 0 ||
        gain->controls[1].label != "Scale Individual Components")
      fail("Gain's per-component switch: " + gain->controls[1].label);
  }

  // Invert, a byte-only plug-in, on bytes: every byte turned over, the top row
  // still on top.
  {
    QImage image = bytes(5, 3);
    const QImage before = image.copy();
    QString error;
    if (!ofxRender(*invert, "check|invert", image, 0, 30, {}, 1, &error)) fail("Invert did not run: " + error);
    else if (image.format() != before.format() || image.size() != before.size()) fail("Invert changed the picture's format");
    else {
      bool same = true;
      for (int y = 0; y < 3 && same; ++y)
        for (int x = 0; x < 20 && same; ++x) same = image.constScanLine(y)[x] == 255 - before.constScanLine(y)[x];
      if (!same) fail("Invert's picture is not the source turned over (upside down?)");
    }
  }

  // Gain on floats, its own depth: all four channels, then red held back by
  // its own scale, then -- every scale 1 -- left alone without drawing.
  const QImage before = floats(4, 3);
  {
    QImage image = before.copy();
    QString error;
    const float times2[4] = {2, 2, 2, 2};
    if (!ofxRender(*gain, "check|gain", image, 0, 30, {2, 0, 1, 1, 1, 1}, 1, &error)) fail("Gain did not run: " + error);
    else if (!scaled(before, image, times2)) fail("Gain 2 did not double every channel");
    image = before.copy();
    const float red[4] = {1, 2, 2, 2};
    if (!ofxRender(*gain, "check|gain", image, 1, 30, {2, 1, .5f, 1, 1, 1}, 1, &error)) fail("Gain did not run per channel: " + error);
    else if (!scaled(before, image, red)) fail("Gain's own red scale was not used");
    image = before.copy();
    const uchar *drawnInto = image.constBits();
    const float same[4] = {1, 1, 1, 1};
    if (!ofxRender(*gain, "check|gain", image, 2, 30, {1, 0, 1, 1, 1, 1}, 1, &error)) fail("Gain at 1 failed: " + error);
    else if (image.constBits() != drawnInto || !scaled(before, image, same)) fail("Gain at 1 was drawn instead of passed through");
    // At half size: still the same change.
    image = before.copy();
    const float half[4] = {.5f, .5f, .5f, .5f};
    if (!ofxRender(*gain, "check|gain", image, 3, 30, {.5f, 0, 1, 1, 1, 1}, .5, &error)) fail("Gain at half size failed: " + error);
    else if (!scaled(before, image, half)) fail("Gain at half size changed something else");
  }

  // Gain, a float plug-in, given bytes: converted there and back.
  {
    QImage image(4, 2, QImage::Format_ARGB32_Premultiplied);
    image.fill(qRgba(200, 100, 40, 255));
    QString error;
    if (!ofxRender(*gain, "check|gain-bytes", image, 0, 30, {.5f, 0, 1, 1, 1, 1}, 1, &error)) fail("Gain on bytes failed: " + error);
    else {
      const QRgb p = image.pixel(1, 1);
      // Every channel halved, alpha with them (the values as stored, premultiplied).
      if (image.format() != QImage::Format_ARGB32_Premultiplied || std::abs(qAlpha(p) - 128) > 1 ||
          std::abs(qRed(p) - 100) > 2 || std::abs(qGreen(p) - 50) > 2 || std::abs(qBlue(p) - 20) > 2)
        fail(QString("Gain on bytes gave %1,%2,%3,%4").arg(qRed(p)).arg(qGreen(p)).arg(qBlue(p)).arg(qAlpha(p)));
    }
  }

  // Many layers at once, each its own copy of the effect, as the render
  // workers draw them.
  {
    std::atomic<int> wrong{0};
    std::vector<std::thread> layers;
    for (int n = 0; n < 6; ++n)
      layers.emplace_back([&, n] {
        for (int frame = 0; frame < 8; ++frame) {
          QImage image = before.copy();
          const float s = 1.5f + .25f * float(n);
          const float times[4] = {s, s, s, s};
          if (!ofxRender(*gain, QString("check|layer%1").arg(n), image, frame, 30, {s, 0, 1, 1, 1, 1}) ||
              !scaled(before, image, times))
            ++wrong;
        }
      });
    for (auto &t : layers) t.join();
    if (wrong) fail(QString("%1 of 48 renders from 6 layers at once were wrong").arg(wrong.load()));
  }

  // A layer's copy let go of while other layers render: each render is either
  // right or turned down, never half done; and a copy asked for again is made
  // again.
  {
    const int before = ofxInstanceCount();
    std::atomic<int> wrong{0}, drawn{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> layers;
    for (int n = 0; n < 4; ++n)
      layers.emplace_back([&, n] {
        for (int frame = 0; frame < 12 && !stop; ++frame) {
          QImage image = floats(4, 3).copy();
          const float times[4] = {3, 3, 3, 3};
          if (ofxRender(*gain, QString("going%1|gain").arg(n), image, frame, 30, {3, 0, 1, 1, 1, 1})) {
            if (!scaled(floats(4, 3), image, times)) ++wrong;
            else ++drawn;
          }
        }
      });
    for (int round = 0; round < 6; ++round)
      ofxDropInstances([](const QString &instance) { return !instance.startsWith("going"); });
    for (auto &t : layers) t.join();
    ofxDropInstances([](const QString &instance) { return !instance.startsWith("going"); });
    if (wrong) fail(QString("%1 renders came out wrong while their copies were let go of").arg(wrong.load()));
    if (ofxInstanceCount() > before) fail("copies of deleted layers were kept");
    QImage image = floats(4, 3);
    const float times[4] = {3, 3, 3, 3};
    if (!ofxRender(*gain, "going0|gain", image, 0, 30, {3, 0, 1, 1, 1, 1}) || !scaled(floats(4, 3), image, times))
      fail("a copy let go of was not made again");
    ofxDropInstances([](const QString &instance) { return !instance.startsWith("going"); });
  }

  // Remembered refusals: something that is not an OpenFX plug-in is written
  // down, and the same file met again is not loaded; a plug-in remembered as
  // turned away is skipped without being asked.
  {
    QTemporaryDir place;
    const QString memory = place.path() + "/refused.json";
    const QString notOfx = QCoreApplication::applicationDirPath() + "/effects/WavyInvert.wfx";
    auto bundleOf = [&](const QString &folder, const QString &from, const QString &name) {
      const QString inside = place.path() + "/" + folder + "/" + name + ".ofx.bundle/Contents/Win64";
      QDir().mkpath(inside);
      return QFile::copy(from, inside + "/" + name + ".ofx");
    };
    if (!bundleOf("first", notOfx, "fake") || !bundleOf("second", notOfx, "fake")) {
      fail("could not set up the remembered-refusal check (" + notOfx + ")");
    } else {
      ofxRememberRefusals(memory);
      QStringList said;
      ofxLoadEffects({place.path() + "/first"}, &said);
      QFile written(memory);
      const QJsonObject saved = written.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(written.readAll()).object() : QJsonObject();
      if (saved.value("bundles").toObject().isEmpty()) fail("a refusal was not written down: " + said.join("; "));
      ofxRememberRefusals(memory);
      said.clear();
      ofxLoadEffects({place.path() + "/second"}, &said);
      if (said.join(";").indexOf("(remembered)") < 0) fail("the same file was loaded again: " + said.join("; "));
      // Invert, remembered as turned away.
      const QString invertBinary = testPlugins + "/invert.ofx.bundle/Contents/Win64/invert.ofx";
      if (!bundleOf("third", invertBinary, "invert")) {
        fail("could not copy the Invert example");
      } else {
        const QFileInfo copied(place.path() + "/third/invert.ofx.bundle/Contents/Win64/invert.ofx");
        const QString stamp = QString("%1|%2|%3").arg(copied.fileName()).arg(copied.size()).arg(copied.lastModified().toMSecsSinceEpoch());
        QJsonObject plugins{{"uk.co.thefoundry.OfxInvertExample", "is not an effect on one picture (a check)"}};
        QJsonObject entry{{"checked", double(QDateTime::currentMSecsSinceEpoch())}, {"plugins", plugins}};
        QFile out(memory);
        out.open(QIODevice::WriteOnly | QIODevice::Truncate);
        // Written by this same host: whatever it numbers itself, as above.
        out.write(QJsonDocument(QJsonObject{{"host", saved.value("host")}, {"bundles", QJsonObject{{stamp, entry}}}}).toJson());
        out.close();
        ofxRememberRefusals(memory);
        said.clear();
        const auto third = ofxLoadEffects({place.path() + "/third"}, &said);
        if (named(third, "OFX Invert Example") || said.join(";").indexOf("(a check) (remembered)") < 0)
          fail("a plug-in remembered as turned away was asked again: " + said.join("; "));
      }
    }
    ofxRememberRefusals({});
  }

  // The OpenFX project's own test and example plug-ins (ofx-conformance): each
  // is taken and draws -- at its defaults and with every control moved -- or
  // is turned away cleanly; and the Support library's check of every property
  // set this host hands it finds nothing wrong but what the check itself has
  // wrong (listed below, each against the standard's headers).
  {
    const QString conformance = QFileInfo(testPlugins).absolutePath() + "/ofx-conformance";
    QStringList said;
    const auto plugs = ofxLoadEffects({conformance}, &said);
    const QStringList taken = {"Prop Tester", "Gain", "Choice Params (Support)", "Invert", "Field", "Noise", "Retimer", "Cross Fade",
                               "Dot Generator", "Gamma", "GenericTest", "OFX Choice Param Example",
                               "OFX Depth Converter Example", "OFX Rectangle Example"};
    const QStringList turnedAway = {"uk.co.thefoundry.CustomParamPlugin",
                                    "uk.co.thefoundry.BasicDrawSuitePlugin", "uk.co.thefoundry.BasicOverlayPlugin",
                                    "net.sf.openfx:PropertyTestPlugin"};
    for (const QString &name : taken)
      if (!named(plugs, name)) fail("conformance: " + name + " was not taken");
    for (const QString &id : turnedAway)
      if (said.join("\n").indexOf(id + " (") < 0) fail("conformance: " + id + " was not turned away with a reason");
    for (const auto &e : plugs) {
      QVector<float> defaults, moved;
      for (const auto &c : e.controls) {
        if (!ofxControlHasValues(c)) continue;
        if (c.kind == OfxControl::Colour) {
          for (double v : c.colour) defaults << float(v);
          moved << .25f << .5f << .75f << 1.f;
        } else {
          defaults << float(c.initial);
          if (c.kind == OfxControl::Toggle) moved << (c.initial ? 0.f : 1.f);
          else if (c.kind == OfxControl::Choice) moved << float(c.maximum);
          else moved << float(std::clamp(c.initial + std::max(c.step, (c.maximum - c.minimum) / 7), c.minimum, c.maximum));
        }
      }
      for (int pass = 0; pass < 2; ++pass) {
        QImage eight = bytes(33, 17), wide = floats(9, 5);
        QString error;
        const QVector<float> &values = pass ? moved : defaults;
        if (!ofxRender(e, "conformance|" + e.identifier, eight, pass, 30, values, 1, &error))
          fail(QString("conformance: %1 did not draw on bytes (%2): %3").arg(e.name, pass ? "controls moved" : "defaults", error));
        if (!ofxRender(e, "conformance|" + e.identifier, wide, pass, 30, values, .5, &error))
          fail(QString("conformance: %1 did not draw on floats (%2): %3").arg(e.name, pass ? "controls moved" : "defaults", error));
      }
    }
    ofxDropInstances([](const QString &instance) { return !instance.startsWith("conformance|"); });
    checkOtherFrames(plugs, "other frames", fail);
    checkOtherFramesOnLayer(plugs, "other frames", fail);
    checkContexts(all, plugs, "contexts", fail);
    checkReach(all, "reach", fail);
    checkControls(all, "controls", fail);
    checkInputs(all, "inputs", fail);
    checkInputsOnLayer(all, "inputs", fail);
    checkRetimeAndMessages(all, "retime", fail);
    checkPointsOnLayer(all, "points", fail);
    checkMovingKeepsEffect("moving", fail);
    checkCard(all, "card", fail);
    checkReachOnLayer(all, "reach", fail);
    // Where the check is wrong, not the host: the thread safety default is
    // InstanceSafe in ofxImageEffect.h; a minimum interact size is two doubles
    // (it reads its own default as integers); instance data and the field
    // flag are checked after the plug-in itself has set them, and so is
    // Retimer's reading of other frames, which it sets as it describes itself.
    const QStringList checkWrong = {"OfxImageEffectPluginRenderThreadSafety", "OfxParamPropInteractMinimumSize",
                                    "OfxPropInstanceData", "OfxImageEffectPluginPropFieldRenderTwiceAlways",
                                    "OfxImageEffectPropTemporalClipAccess"};
    QFile written(log);
    if (!written.open(QIODevice::ReadOnly)) fail("conformance: the Support library wrote no log");
    QStringList wrong;
    while (!written.atEnd()) {
      const QString line = QString::fromLocal8Bit(written.readLine()).trimmed();
      if (!line.startsWith("ERROR") && !line.startsWith("WARNING")) continue;
      bool known = false;
      for (const QString &k : checkWrong) known = known || line.contains(k);
      if (!known && !wrong.contains(line)) wrong << line;
    }
    for (const QString &line : wrong) fail("conformance: Support library says " + line);
    qunsetenv("OFX_PLUGIN_LOGFILE");
  }

  // Not a plug-in at all.
  {
    OfxEffectEntry nothing;
    nothing.name = "Nothing";
    QImage image = bytes(2, 2);
    const QImage kept = image.copy();
    QString error;
    if (ofxRender(nothing, "check|nothing", image, 0, 30, {}, 1, &error) || error.isEmpty() || image != kept)
      fail("an effect that was never loaded ran");
  }
  return failures.isEmpty() ? 0 : 1;
}

// The plug-in host process against plug-ins that misbehave on purpose
// (ofx-hazards): one that ends the process while it is described is left out
// and the rest load; one that crashes, ends the process or never returns as it
// renders is switched off -- quickly, and for good -- and the others carry on
// in a worker started again.
int checkOfxWorkerHazards(QStringList &failures, const QString &hazardPlugins, const QString &testPlugins) {
  auto fail = [&](const QString &what) { failures << what; };
  if (!ofxWorkerInUse()) {
    fail("hazards: the plug-in host is not in use");
    return 1;
  }
  ofxSetWorkerTimeouts(4000, 30000);
  QStringList said;
  const auto hazards = ofxLoadEffects({hazardPlugins}, &said);
  if (named(hazards, "Hazard Load")) fail("hazards: the one that ends the process while loading was taken");
  if (said.join("\n").indexOf("hazard-load") < 0) fail("hazards: no word of the one that stopped the host while loading: " + said.join("; "));
  const auto plugs = ofxLoadEffects({testPlugins});
  const OfxEffectEntry *gain = named(plugs, "OFX Gain Example");
  if (!gain) {
    fail("hazards: no Gain example to draw with");
    return 1;
  }
  auto gainDraws = [&] {
    QImage image = floats(4, 3);
    const float twice[4] = {2, 2, 2, 2};
    QString error;
    const bool drawn = ofxRender(*gain, "hazard|gain", image, 0, 30, {2, 0, 1, 1, 1, 1}, 1, &error);
    if (!drawn) fail("hazards: Gain: " + error);
    return drawn && scaled(floats(4, 3), image, twice);
  };
  if (!gainDraws()) fail("hazards: Gain did not draw before any hazard");
  // Other frames asked for from the plug-in host, in the middle of a render.
  checkOtherFrames(ofxLoadEffects({QFileInfo(testPlugins).absolutePath() + "/ofx-conformance"}),
                   "hazards: other frames", fail);
  for (const char *which : {"Hazard Crash", "Hazard Terminate", "Hazard Hang"}) {
    const QString name = which;
    const OfxEffectEntry *e = named(hazards, name);
    if (!e) {
      fail("hazards: " + name + " was not loaded");
      continue;
    }
    QElapsedTimer clock;
    clock.start();
    QImage image = bytes(8, 8);
    const QImage before = image.copy();
    QString error;
    if (ofxRender(*e, "hazard|" + name, image, 0, 30, {}, 1, &error)) fail("hazards: " + name + " drew");
    if (error.isEmpty()) fail("hazards: " + name + " failed without a word");
    if (image != before) fail("hazards: " + name + " left the picture changed");
    if (clock.elapsed() > 20000) fail(QString("hazards: %1 took %2 ms to be given up on").arg(name).arg(clock.elapsed()));
    clock.restart();
    error.clear();
    if (ofxRender(*e, "hazard|" + name, image, 1, 30, {}, 1, &error) || clock.elapsed() > 1000)
      fail("hazards: " + name + " was not switched off (" + error + ")");
    if (!gainDraws()) fail("hazards: Gain did not draw after " + name);
  }
  ofxSetWorkerTimeouts(30000, 60000);
  return failures.isEmpty() ? 0 : 1;
}
