#include "prerender.h"
#include "expressions.h"
#include "effect_packs.h"
#include <thread>
#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QImageWriter>
#include <QMutex>
#include <QThreadPool>
#include <atomic>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QPainter>
#include <QDataStream>
#include <QJsonDocument>
#include <QSet>
#include <QByteArrayList>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <QDirIterator>
#include <QTemporaryDir>
#include <limits>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
QByteArray digest(const QByteArray &content) {
  return QCryptographicHash::hash(content, QCryptographicHash::Sha1);
}
QByteArray jsonBytes(const QJsonObject &o) {
  return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
QByteArray jsonBytes(const QJsonArray &a) {
  return QJsonDocument(QJsonObject{{"v", a}}).toJson(QJsonDocument::Compact);
}
void writeProperty(QDataStream &stream, const Property &p) {
  stream << p.base << qint64(p.keys.size());
  for (const auto &k : p.keys) {
    stream << k.frame << k.value << k.ease << k.custom << k.absolute << k.continuous;
    for (double control : k.curve) stream << control;
    // The motion path's handles and timing keys move the layer too.
    stream << k.hasSpatial << k.spatialOut << k.spatialIn << k.hasNormCurve << k.normCurve[0] << k.normCurve[1] << k.timing;
  }
  // An expression is part of what the property is: the same keys with a
  // different expression are a different picture.
  stream << p.expression << p.expressionOn;
}
// The frames this layer can reach. A layer is shown when the frame being
// rendered sits inside [start, start + duration); one frame of slack on each
// side covers the motion-blur shutter and fractional in-points.
QPair<int, int> layerSpan(const Layer &l) {
  return {int(std::floor(l.start)) - 1, int(std::ceil(l.start + l.duration)) + 1};
}
// Everything about a layer that reaches a pixel. Not its name, and not which of
// its panels are open: those change several times a minute and change nothing
// on screen.
QByteArray layerOwnKey(const Layer &l) {
  QByteArray content;
  QDataStream stream(&content, QIODevice::WriteOnly);
  stream << l.parentId << l.parentBind << l.parentBind3D << l.threeD;
  stream << l.type << l.shapeKind << l.text << l.richHtml << l.font << l.fontSize << l.color << l.bold
         << l.fontStyle << l.fauxBold << l.fauxItalic << l.capitals << l.leading << l.tracking << l.align << l.paragraphWidth;
  stream << l.strokeWidth << l.strokeColor << l.strokeMode;
  stream << qint32(l.textStrokes.size());
  for (const auto &stroke : l.textStrokes)
    stream << stroke.width << stroke.color << stroke.mode << stroke.join;
  if (l.textStrokeOnly) stream << QByteArray("stroke only");
  stream << l.blur << l.glow << l.shadow;
  stream << jsonBytes(l.surfaceEffects) << jsonBytes(l.appliedEffects)
         << jsonBytes(l.hiddenEffects) << jsonBytes(l.effectOrder)
         << jsonBytes(l.characterAnimation);
  // An effect written as text and changed while running draws another picture
  // with the same settings. Nothing for every other layer, whose key is as it was.
  if (const QByteArray revisions = layerPackRevisions(l); !revisions.isEmpty()) stream << revisions;
  stream << jsonBytes(l.shapePoints) << jsonBytes(l.masks) << l.shapeClosed << l.shapeFill;
  stream << jsonBytes(l.particles) << l.particlesOn;
  stream << l.repeaterEnabled << l.repeaterCopies << l.repeaterOffset
         << l.repeaterPositionX << l.repeaterPositionY << l.repeaterPositionZ
         << l.repeaterScale << l.repeaterRotation << l.repeaterSpiral;
  // Camera only, but every layer carries it through the same digest. These
  // are plain fields rather than keyframed curves, so nothing else folds
  // them in automatically the way l.p does below -- and on a camera, any of
  // them changing has to reach every 3D layer's blur, which already happens
  // because a spatial scene folds the camera's own key into the whole
  // scene's fingerprint rather than just its own frames.
  stream << l.lensFocalLength << l.dofEnabled << l.dofFocusDistance << l.dofFStop
         << l.dofBlurLevel;
  stream << l.lightKind << l.lightFalloff << l.acceptsLights << l.lightShadows << l.castsShadows << l.sharesMainView;
  stream << l.mainAnchorX << l.mainAnchorY << l.mainX << l.mainY << l.mainScaleX
         << l.mainScaleY << l.mainRotation << l.mainScaleLocked;
  stream << l.separatePosition << l.separateScale << l.separateAnchor << l.separateRotation
         << l.lockScale << l.scaleRatio;
  stream << l.motionBlur << l.visible << l.start << l.duration;
  // Changes nothing about the layer's own pixels but everything about the
  // frame, so a stored frame drawn under another mode is a picture of
  // something else.
  stream << l.blendMode;
  stream << l.trackMatteId << l.trackMatteInverted;
  // The source, named by what it is rather than by its pixels: a path, the
  // trim, and the identity Qt gives a loaded image, which changes whenever the
  // image does.
  // A song's length is measured by the player the first time it plays, and
  // written into its layer. A song draws nothing, so that is no reason to throw
  // away every frame of the timeline it spans -- which it was, on the first
  // press of Play in every session.
  stream << l.path << (l.type == "audio" ? 0. : l.sourceDurationMs) << l.sourceOffsetMs << l.image.cacheKey();
  for (const auto &property : l.p) writeProperty(stream, property);
  stream << l.extraCurveIds << qint64(l.extraCurves.size());
  for (const auto &property : l.extraCurves) writeProperty(stream, property);
  return digest(content);
}
}  // namespace

namespace {
// Rows without the padding QImage may carry, which is a small saving of its own.
QByteArray rawImage(const QImage &image) {
  const qsizetype stride = qsizetype(image.width()) * (image.depth() / 8);
  QByteArray out(stride * image.height(), Qt::Uninitialized);
  for (int y = 0; y < image.height(); ++y)
    memcpy(out.data() + stride * y, image.constScanLine(y), size_t(stride));
  return out;
}
QImage unrawImage(const QByteArray &data, QSize size, QImage::Format format) {
  QImage image(size, format);
  if (image.isNull()) return {};
  const qsizetype stride = qsizetype(size.width()) * (image.depth() / 8);
  if (data.size() != stride * size.height()) return {};
  for (int y = 0; y < size.height(); ++y)
    memcpy(image.scanLine(y), data.constData() + stride * y, size_t(stride));
  return image;
}
}  // namespace

// A row is a sequence of tokens, each a little-endian quint16 followed by
// pixels. The top bit says which kind: set means one pixel repeated
// (header & 0x7fff) + 1 times, clear means (header) + 1 pixels written out as
// they are. Runs of two already pay for themselves, and the worst case -- every
// pixel different from its neighbour -- costs two bytes per thirty-two thousand
// pixels, which is why there is no need to guess in advance whether it is worth
// trying.
QByteArray packImage(const QImage &image) {
  const int bpp = image.depth() / 8;
  const int width = image.width();
  if (bpp <= 0 || width <= 0) return {};
  QByteArray out;
  out.reserve(int(qsizetype(width) * image.height() * bpp / 8) + 1024);
  // Not named emit: Qt spells that keyword as an empty macro.
  auto token = [&](quint16 header) {
    out.append(char(header & 0xff));
    out.append(char(header >> 8));
  };
  for (int y = 0; y < image.height(); ++y) {
    const uchar *row = image.constScanLine(y);
    int x = 0, literalFrom = 0;
    auto flushLiteral = [&](int upto) {
      while (literalFrom < upto) {
        const int count = std::min(upto - literalFrom, 32768);
        token(quint16(count - 1));
        out.append(reinterpret_cast<const char *>(row + qsizetype(literalFrom) * bpp),
                   qsizetype(count) * bpp);
        literalFrom += count;
      }
    };
    while (x < width) {
      const uchar *pixel = row + qsizetype(x) * bpp;
      int run = 1;
      while (x + run < width && run < 32768 &&
             memcmp(pixel, row + qsizetype(x + run) * bpp, size_t(bpp)) == 0)
        ++run;
      if (run >= 2) {
        flushLiteral(x);
        token(quint16(0x8000 | (run - 1)));
        out.append(reinterpret_cast<const char *>(pixel), bpp);
        literalFrom = x + run;
      }
      x += run;
    }
    flushLiteral(width);
  }
  return out;
}

QImage unpackImage(const QByteArray &data, QSize size, QImage::Format format) {
  QImage image(size, format);
  if (image.isNull()) return {};
  const int bpp = image.depth() / 8;
  const char *in = data.constData(), *end = in + data.size();
  for (int y = 0; y < size.height(); ++y) {
    uchar *row = image.scanLine(y);
    int x = 0;
    while (x < size.width()) {
      if (end - in < 2) return {};
      const quint16 header = quint16(uchar(in[0])) | quint16(quint16(uchar(in[1])) << 8);
      in += 2;
      const int count = (header & 0x7fff) + 1;
      if (count > size.width() - x) return {};
      if (header & 0x8000) {
        if (end - in < bpp) return {};
        for (int n = 0; n < count; ++n)
          memcpy(row + qsizetype(x + n) * bpp, in, size_t(bpp));
        in += bpp;
      } else {
        if (end - in < qsizetype(count) * bpp) return {};
        memcpy(row + qsizetype(x) * bpp, in, size_t(count) * bpp);
        in += qsizetype(count) * bpp;
      }
      x += count;
    }
  }
  return image;
}

QByteArray packPlanes(const QImage &image) {
  const int bpp = image.depth() / 8, width = image.width();
  if (bpp <= 0 || width <= 0) return {};
  QByteArray planes(qsizetype(width) * image.height() * bpp, Qt::Uninitialized);
  uchar *write = reinterpret_cast<uchar *>(planes.data());
  for (int y = 0; y < image.height(); ++y) {
    const uchar *row = image.constScanLine(y);
    for (int channel = 0; channel < bpp; ++channel) {
      uchar previous = 0;
      for (int x = 0; x < width; ++x) {
        const uchar value = row[qsizetype(x) * bpp + channel];
        *write++ = uchar(value - previous);
        previous = value;
      }
    }
  }
  return qCompress(planes, 1);
}

QImage unpackPlanes(const QByteArray &data, QSize size, QImage::Format format) {
  QImage image(size, format);
  if (image.isNull()) return {};
  const int bpp = image.depth() / 8, width = size.width();
  const QByteArray planes = qUncompress(data);
  if (planes.size() != qsizetype(width) * size.height() * bpp) return {};
  const uchar *read = reinterpret_cast<const uchar *>(planes.constData());
  for (int y = 0; y < size.height(); ++y) {
    uchar *row = image.scanLine(y);
    for (int channel = 0; channel < bpp; ++channel) {
      uchar previous = 0;
      for (int x = 0; x < width; ++x) {
        previous = uchar(previous + *read++);
        row[qsizetype(x) * bpp + channel] = previous;
      }
    }
  }
  return image;
}

QByteArray packLight(const QImage &image, int quality) {
  if (image.isNull() || image.depth() > 32) return {};
  // Straight colour: the JPEG carries colour, and the coverage goes on its own.
  const QImage straight = image.convertToFormat(QImage::Format_ARGB32);
  const int width = straight.width(), height = straight.height();
  bool opaque = true;
  for (int y = 0; y < height && opaque; ++y) {
    const QRgb *row = reinterpret_cast<const QRgb *>(straight.constScanLine(y));
    for (int x = 0; x < width; ++x)
      if (qAlpha(row[x]) != 255) { opaque = false; break; }
  }
  QByteArray jpeg;
  {
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "jpg");
    writer.setQuality(quality);
    if (!writer.write(straight)) return {};
  }
  QByteArray alpha;
  if (!opaque) {
    QByteArray plane(qsizetype(width) * height, Qt::Uninitialized);
    for (int y = 0; y < height; ++y) {
      const QRgb *row = reinterpret_cast<const QRgb *>(straight.constScanLine(y));
      uchar *to = reinterpret_cast<uchar *>(plane.data()) + qsizetype(width) * y;
      // Each value as its difference from the one on its left, which the
      // deflate below makes almost nothing of wherever coverage is smooth.
      uchar left = 0;
      for (int x = 0; x < width; ++x) {
        const uchar a = uchar(qAlpha(row[x]));
        to[x] = uchar(a - left);
        left = a;
      }
    }
    alpha = qCompress(plane, 1);
  }
  QByteArray out;
  QDataStream stream(&out, QIODevice::WriteOnly);
  stream << quint32(jpeg.size()) << quint32(alpha.size());
  out.append(jpeg);
  out.append(alpha);
  return out;
}
QImage unpackLight(const QByteArray &data, QSize size, QImage::Format format) {
  if (data.size() < 8) return {};
  quint32 jpegSize = 0, alphaSize = 0;
  {
    QDataStream stream(data);
    stream >> jpegSize >> alphaSize;
  }
  if (qsizetype(8) + jpegSize + alphaSize != data.size()) return {};
  QImage colour = QImage::fromData(QByteArrayView(data.constData() + 8, qsizetype(jpegSize)), "JPG");
  if (colour.isNull() || colour.size() != size) return {};
  QImage straight = colour.convertToFormat(QImage::Format_ARGB32);
  if (alphaSize > 0) {
    const QByteArray plane = qUncompress(QByteArray::fromRawData(data.constData() + 8 + jpegSize, qsizetype(alphaSize)));
    if (plane.size() != qsizetype(size.width()) * size.height()) return {};
    for (int y = 0; y < size.height(); ++y) {
      QRgb *row = reinterpret_cast<QRgb *>(straight.scanLine(y));
      const uchar *from = reinterpret_cast<const uchar *>(plane.constData()) + qsizetype(size.width()) * y;
      uchar a = 0;
      for (int x = 0; x < size.width(); ++x) {
        a = uchar(a + from[x]);
        row[x] = (QRgb(a) << 24) | (row[x] & 0x00ffffffu);
      }
    }
  }
  return straight.convertToFormat(format);
}

qint64 physicalMemoryBytes() {
#ifdef Q_OS_WIN
  MEMORYSTATUSEX status;
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status)) return qint64(status.ullTotalPhys);
#endif
  return 0;
}

qint64 maximumCacheBudget() {
  const qint64 physical = physicalMemoryBytes();
  // Unknown machine: offer the old default and nothing above it rather than
  // guess upwards.
  if (physical <= 0) return 512LL * 1024 * 1024;
  // Three fifths rather than a half. Windows reports a little less than the
  // memory on the label -- a 32 GB machine reports 31.7 -- and half of that
  // falls just short of 16 GB, which would leave the largest size greyed out on
  // a machine that plainly has room for it. At three fifths each of the offered
  // sizes becomes available at about half the memory actually fitted.
  return std::max<qint64>(256LL * 1024 * 1024, physical / 5 * 3);
}

bool PreRenderSignature::stableAt(int frame) const {
  for (const auto &span : unstable)
    if (frame >= span.first && frame <= span.second) return false;
  return true;
}

QByteArray PreRenderSignature::frameKey(int frame) const {
  QByteArrayList reaching;
  for (auto it = layers.constBegin(); it != layers.constEnd(); ++it) {
    const auto span = spans.constFind(it.key());
    if (span == spans.constEnd() || frame < span->first || frame > span->second) continue;
    reaching << it.value();
  }
  // Sorted because a hash hands its contents back in whatever order it likes,
  // and this value has to mean the same thing twice.
  std::sort(reaching.begin(), reaching.end());
  QByteArray content = global;
  for (const auto &key : reaching) content += key;
  return digest(content);
}

PreRenderSignature preRenderSignature(const Scene &s, const QByteArray &context) {
  PreRenderSignature signature;
  const int count = int(s.layers.size());

  QHash<QString, QByteArray> own;
  QHash<QString, QString> parentOf;
  bool spatial = false, duplicated = false, matted = false;
  for (int i = 0; i < count; ++i) {
    const auto &l = s.layers[i];
    if (own.contains(l.id)) duplicated = true;
    own[l.id] = layerOwnKey(l);
    parentOf[l.id] = l.parentId;
    spatial |= l.threeD && l.type != "camera" && l.type != "light";
    // A matte owner's pixels depend on the layer above it, which the per-layer
    // "what is behind me" accumulation below never sees. Rather than thread that
    // one dependency through, fold the whole scene into the fingerprint whenever
    // a matte is in play: any edit then clears the cache, which is always
    // correct and costs nothing until someone actually uses a track matte.
    if (!l.trackMatteId.isEmpty()) matted = true;
    // Letters wearing another layer's picture hang on that layer as a matte does.
    if (layerUsesLetterTexture(l)) matted = true;
    // An expression that reads another layer (or where its own layer sits in
    // the stack) ties this layer's pixels to that one, the same way a matte
    // does, and is handled the same way.
    for (const auto &property : l.p)
      if (!property.expression.isEmpty() && expressionReachesOtherLayers(property.expression)) matted = true;
    for (const auto &property : l.extraCurves)
      if (!property.expression.isEmpty() && expressionReachesOtherLayers(property.expression)) matted = true;
  }

  // A layer carries its parents in its key, so moving a parent invalidates
  // everything hanging off it without anyone having to walk the tree twice.
  auto withAncestors = [&](QString id) {
    QByteArray content;
    QSet<QString> seen;
    while (!id.isEmpty() && own.contains(id) && !seen.contains(id)) {
      seen.insert(id);
      content += own.value(id);
      id = parentOf.value(id);
    }
    return content;
  };

  // What is stacked behind a layer, accumulated from the back forward. That way
  // round on purpose: putting a new layer in front of everything changes
  // nothing behind it, so the frames already rendered for those layers stay.
  QHash<QString, QByteArray> behind;
  QByteArray running;
  for (int i = count - 1; i >= 0; --i) {
    behind[s.layers[i].id] = digest(running);
    // A Layer Struktur stands in the list but draws nothing: what is in front
    // of it is not changed by it.
    if (!layerIsStructure(s.layers[i])) running += s.layers[i].id.toUtf8();
  }

  QByteArray global;
  {
    QDataStream stream(&global, QIODevice::WriteOnly);
    stream << s.width << s.height << s.fps << s.frames << s.processingDepth << s.background
           << s.motionBlur << s.exposure << context << s.linearLight;
    // A layer turning 3D can reorder two others that were in separate depth
    // groups before, at frames that layer is nowhere near, so which layers are
    // spatial cannot live per layer. Only once something is 3D, though: in a
    // flat composition the stack is settled per layer below, and folding it in
    // here would mean every added layer emptied the cache.
    stream << spatial;
    if (spatial)
      for (const auto &l : s.layers) stream << l.type << l.threeD;
    // Repeated ids mean the per-layer bookkeeping below cannot tell two layers
    // apart. Rather than guess, fold the whole scene in and let any edit empty
    // the cache.
    stream << duplicated;
    if (duplicated)
      for (const auto &l : s.layers) stream << own.value(l.id);
    stream << matted;
    if (matted)
      for (const auto &l : s.layers) stream << own.value(l.id);
    // Isolating a layer hides every other one, at frames it is nowhere near, so
    // which layers are isolated is part of every frame. Folded in only when
    // there are some, so a scene without leaves the cache as it was.
    QStringList isolated;
    for (const auto &l : s.layers)
      if (l.isolated) isolated << l.id;
    if (!isolated.isEmpty()) stream << isolated;
  }

  for (const auto &l : s.layers) {
    // A Layer Struktur is in no frame. Its span follows what it holds, so
    // counting it would throw away every frame it spans whenever one layer
    // inside it moved.
    if (layerIsStructure(l)) continue;
    const auto span = layerSpan(l);
    const QByteArray content = withAncestors(l.id) + behind.value(l.id);
    // One camera decides where every 3D layer in the scene lands, so as soon as
    // anything is 3D the camera stops being a layer among others and becomes
    // part of what every frame is.
    // Lights the same: every 3D layer is lit by all of them.
    if ((l.type == "camera" || l.type == "light") && spatial) {
      global += content;
      continue;
    }
    signature.layers[l.id] = digest(content);
    signature.spans[l.id] = span;
    // Video comes from the player, not from the scene, and the player has moved
    // on by the time anyone looks again.
    if (l.type == "video" && l.visible) signature.unstable.push_back(span);
  }
  signature.global = digest(global);
  return signature;
}

void PreRenderCache::setBudget(qint64 bytes) {
  limit = std::max<qint64>(0, bytes);
  decoded->setMaxCost(int(std::min<qint64>(65536, limit / 8192)));
  evict();
}

bool PreRenderCache::expect(const QByteArray &fingerprint) {
  if (fingerprint == sig.global) return false;
  clear();
  sig.global = fingerprint;
  return true;
}

int PreRenderCache::reconcile(const PreRenderSignature &next) {
  const int before = frames.size();
  if (next.global != sig.global) {
    dropAll();
    sig = next;
    return before;
  }
  // Every layer that is new, gone, or different, and the frames it reaches --
  // both before the edit and after it, because a layer dragged along the
  // timeline takes two stretches of it with it.
  for (auto it = sig.layers.constBegin(); it != sig.layers.constEnd(); ++it) {
    const auto now = next.layers.constFind(it.key());
    if (now != next.layers.constEnd() && now.value() == it.value()) continue;
    const auto was = sig.spans.constFind(it.key());
    if (was != sig.spans.constEnd()) invalidate(was->first, was->second);
    const auto moved = next.spans.constFind(it.key());
    if (moved != next.spans.constEnd()) invalidate(moved->first, moved->second);
  }
  for (auto it = next.layers.constBegin(); it != next.layers.constEnd(); ++it) {
    if (sig.layers.contains(it.key())) continue;
    const auto span = next.spans.constFind(it.key());
    if (span != next.spans.constEnd()) invalidate(span->first, span->second);
  }
  sig = next;
  for (const auto &span : sig.unstable) invalidate(span.first, span.second);
  return before - frames.size();
}

namespace {
QMutex &diskGuard() { static QMutex guard; return guard; }
QString &diskFolder() { static QString folder; return folder; }
std::atomic<quint64> diskCounter{0};
// Whether frames being packed now go to a file: set by the cache once what it
// keeps packed in memory has reached memoryFirstBytes(). Read by prepare() on
// the lanes, which have no cache to ask.
std::atomic<bool> spillToDisk{false};
std::atomic<bool> spillAllowed{true};
std::atomic<qint64> diskWritten{0};
std::atomic<qint64> diskRoom{std::numeric_limits<qint64>::max()};
}
void PreRenderCache::setSpillAllowed(bool allowed) { spillAllowed.store(allowed); }
qint64 PreRenderCache::diskBytesWritten() { return diskWritten.load(); }
void PreRenderCache::setDiskWriteRoom(qint64 bytes) { diskRoom.store(std::max<qint64>(0, bytes)); }
qint64 PreRenderCache::dropBehind(int from, int total, int window, qint64 bytes) {
  if (bytes <= 0 || total <= 0) return 0;
  // How long ago the playhead passed each frame, counting back and wrapping.
  std::vector<std::pair<int, int>> behind;
  for (auto it = frames.constBegin(); it != frames.constEnd(); ++it) {
    if (it->disk) continue;
    const int ahead = ((it.key() - from) % total + total) % total;
    if (ahead < window) continue;
    behind.push_back({total - ahead, it.key()});
  }
  std::sort(behind.begin(), behind.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
  const qint64 before = held;
  for (const auto &entry : behind) {
    if (before - held >= bytes) break;
    invalidate(entry.second, entry.second);
  }
  return before - held;
}
qint64 PreRenderCache::memoryFirstBytes() {
  // A sixteenth of the machine, between a quarter and two gigabytes: about half
  // a minute of a busy 1080p music video, packed, on a 32 GB machine.
  static const qint64 bytes = [] {
    const qint64 physical = physicalMemoryBytes();
    return physical <= 0 ? 256LL * 1024 * 1024
                         : std::clamp<qint64>(physical / 16, 256LL * 1024 * 1024, 2048LL * 1024 * 1024);
  }();
  return bytes;
}
void PreRenderCache::updateSpill() const {
  spillToDisk.store(held - diskHeld - hotHeld >= memoryFirstBytes());
}
namespace { std::atomic<bool> lightFrames{false}; }
void PreRenderCache::setLight(bool on) { lightFrames.store(on); }
bool PreRenderCache::light() { return lightFrames.load(); }
void PreRenderCache::setDiskDirectory(const QString &folder) {
  QMutexLocker lock(&diskGuard());
  diskFolder() = folder;
  if (!folder.isEmpty()) QDir().mkpath(folder);
}
QString PreRenderCache::diskDirectory() {
  QMutexLocker lock(&diskGuard());
  return diskFolder();
}
// Deleting a folder's worth of files on the thread that let go of them (the
// interface, when the cache is emptied) is a stall, so it is left to the pool.
DiskBlob::~DiskBlob() {
  if (path.isEmpty()) return;
  const QString file = path;
  QThreadPool::globalInstance()->start([file] { QFile::remove(file); });
}
QByteArray DiskBlob::read() const {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return file.readAll();
}
const void *PreRenderCache::keyOf(const Held &entry) {
  if (entry.disk) return entry.disk.get();
  if (!entry.image.isNull()) return entry.image.constBits();
  return entry.data.constData();
}
qint64 PreRenderCache::sizeOf(const Held &entry) {
  if (entry.disk) return entry.disk->size;
  if (!entry.image.isNull()) return entry.image.sizeInBytes();
  return entry.data.size();
}
QImage PreRenderCache::decode(const Held &entry) {
  if(!entry.image.isNull())return entry.image;
  const auto format=QImage::Format(entry.format);
  const QByteArray bytes=entry.disk ? entry.disk->read() : entry.data;
  if(bytes.isEmpty())return {};
  return entry.coding==Runs ? unpackImage(bytes,entry.size,format)
       : entry.coding==Planes ? unpackPlanes(bytes,entry.size,format)
       : entry.coding==Light ? unpackLight(bytes,entry.size,format)
                              : unrawImage(bytes,entry.size,format);
}
void PreRenderCache::keepDecoded(const Held &source,const QImage &image) const {
  if(image.isNull())return;
  const auto cost=(image.sizeInBytes()+1023)/1024;
  // Room for a window of frames whatever their size: 64 MiB held eight of 1080p
  // but only two of 4K, too few to decode ahead of playback.
  const int wanted=int(std::min<qint64>(std::max<qint64>(65536,cost*10),524288));
  if(decoded->maxCost()<wanted)decoded->setMaxCost(wanted);
  if(cost<=decoded->maxCost())
    decoded->insert(keyOf(source),new Decoded{source,image},int(cost));
}
void PreRenderCache::collectDecoded() const {
  for(size_t index=0;index<decodeJobs.size();) {
    auto &job=decodeJobs[index];
    if(job.wait_for(std::chrono::seconds(0))!=std::future_status::ready) {++index;continue;}
    try {
      Decoded ready=job.get();
      ++decodedCount;
      decoding.remove(keyOf(ready.source));
      // An edit/eviction may have removed this picture while decoding.
      if(shares.contains(keyOf(ready.source)))keepDecoded(ready.source,ready.image);
    } catch(...) { }
    decodeJobs.erase(decodeJobs.begin()+long(index));
  }
}
// Decodes the frames about to be shown, several at once. A frame is decoded on
// a thread of its own, so a run of them finishes together: one job for the whole
// window (as this used to be) took eight frames' time to hand back the first,
// by which time the playhead had passed it and the interface decoded the frame
// itself, in the time it had to show it.
void PreRenderCache::prefetch(int from,int total,int count) {
  collectDecoded();
  if(total<=0 || decoded->maxCost()<=0)return;
  const size_t parallel=size_t(std::clamp(int(std::thread::hardware_concurrency())/3,2,6));
  qint64 bytes=0;
  for(int step=0;step<std::min(total,count);++step) {
    const int frame=((from+step)%total+total)%total;
    const auto found=frames.constFind(frame);
    if(found==frames.constEnd() || !found->image.isNull())continue; // kept frames need no unpacking
    const void *key=keyOf(*found);
    if(decoding.contains(key) || decoded->contains(key) || keyOf(recentHeld)==key)continue;
    const qint64 cost=qint64(found->size.width())*found->size.height()*
        QImage::toPixelFormat(QImage::Format(found->format)).bitsPerPixel()/8;
    if(cost<=0 || bytes+cost>qint64(decoded->maxCost())*1024)break;
    if(decodeJobs.size()>=std::max<size_t>(parallel,size_t(count)))break; // a whole window may start at once
    bytes+=cost;
    decoding.insert(key);
    Held entry=*found;
    decodeJobs.push_back(std::async(std::launch::async,[entry]{return Decoded{entry,decode(entry)};}));
  }
}
QImage PreRenderCache::at(int frame) const {
  collectDecoded();
  const auto found = frames.constFind(frame);
  if (found == frames.constEnd()) return {};
  if(!recentImage.isNull() && keyOf(*found)==keyOf(recentHeld) &&
      found->size==recentImage.size() && found->format==recentImage.format()) {
    recentFrame=frame;
    return recentImage;
  }
  QImage image;
  if(!found->image.isNull()) {
    recentFrame = frame;
    recentImage = found->image;
    recentHeld = *found;
    return recentImage;
  }
  if(auto *ready=decoded->object(keyOf(*found))) {
    image=ready->image;
    // Used, and kept below in recentImage for repaints of this frame. Left in the
    // cache it counted as recently used and outranked the frames still to come,
    // so a full cache threw away the next frames and kept the ones already
    // shown: with a window of five or more, nearly every frame missed.
    decoded->remove(keyOf(*found));
  }
  if(image.isNull()) {
    image=decode(*found);++decodedCount;++foregroundCount;
    keepDecoded(*found,image);
  }
  recentFrame = frame;
  recentImage = image;
  recentHeld = *found;
  return image;
}

void PreRenderCache::store(int frame, const QImage &image) {
  if (image.isNull() || sig.global.isEmpty()) return;
  if (!sig.stableAt(frame)) return;
  storePrepared(frame,prepare(image));
}
PreRenderCache::Prepared PreRenderCache::keep(const QImage &image) {
  Held entry;
  if (image.isNull()) return entry;
  entry.size = image.size();
  entry.format = image.format();
  entry.coding = Kept;
  entry.image = image;
  return entry;
}
qint64 PreRenderCache::hotBudget() const {
  // A thirty-second of memory, between a quarter and one gigabyte: a few
  // seconds of HD waiting to be packed. It was a sixth of the machine -- nearly
  // five gigabytes of unpacked frames on 32 GB, beside the render's own caches
  // -- which is memory taken from everything else for frames that are packed
  // to an eighth of it moments later anyway.
  static const qint64 memory = [] {
    const qint64 physical = physicalMemoryBytes();
    return physical <= 0 ? 256LL * 1024 * 1024
                         : std::clamp<qint64>(physical / 32, 256LL * 1024 * 1024, 1024LL * 1024 * 1024);
  }();
  return std::min(limit, memory);
}
std::vector<PreRenderCache::Prepared> PreRenderCache::hotPictures(int from, int total, int count) const {
  std::vector<std::pair<int, Held>> kept;
  QSet<const void *> seen;
  for (auto it = frames.constBegin(); it != frames.constEnd(); ++it) {
    if (it->image.isNull()) continue;
    const void *key = keyOf(*it);
    if (seen.contains(key)) continue;
    seen.insert(key);
    const int ahead = total > 0 ? ((it.key() - from) % total + total) % total : std::abs(it.key() - from);
    kept.push_back({ahead, *it});
  }
  std::sort(kept.begin(), kept.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
  std::vector<Held> out;
  for (int n = 0; n < int(kept.size()) && n < count; ++n) out.push_back(kept[size_t(n)].second);
  return out;
}
void PreRenderCache::repack(const Prepared &hot, const Prepared &packed) {
  if (hot.image.isNull() || (packed.data.isEmpty() && !packed.disk)) return;
  // The caller still holds `hot`, so its pixels cannot have been freed and
  // their address given to a newer frame: a frame whose key matches is showing
  // this very picture.
  const void *was = keyOf(hot);
  const auto found = shares.find(was);
  if (found == shares.end()) return;
  const int showing = found.value();
  for (auto it = frames.begin(); it != frames.end(); ++it)
    if (keyOf(*it) == was) *it = packed;
  shares.erase(found);
  held -= sizeOf(hot);
  hotHeld -= sizeOf(hot);
  decoded->remove(was);
  shares[keyOf(packed)] += showing;
  held += sizeOf(packed);
  if (packed.disk) diskHeld += sizeOf(packed);
  updateSpill();
  if (keyOf(recentHeld) == was) recentHeld = packed;
}
PreRenderCache::Prepared PreRenderCache::prepare(const QImage &image) {
  if(image.isNull())return {};
  Held entry;
  entry.size = image.size();
  entry.format = image.format();
  const qsizetype plain = qsizetype(image.width()) * image.height() * (image.depth() / 8);
  QByteArray best = packImage(image);
  entry.coding = Runs;
  // Runs are cheap and win outright on a flat frame, so the second codec is
  // only paid for when the first one did badly -- which is exactly when it has
  // something to offer. Sixteen to one is well clear of both outcomes: a flat
  // frame runs to two hundred to one, a busy one to under two.
  // Light frames: a frame the runs already pack to almost nothing stays exact
  // (it is smaller that way too); anything busier goes as a JPEG.
  if (light() && image.depth() <= 32 && (best.isEmpty() || best.size() * 16 > plain)) {
    QByteArray jpeg = packLight(image);
    if (!jpeg.isEmpty() && (best.isEmpty() || jpeg.size() < best.size())) {
      best = std::move(jpeg);
      entry.coding = Light;
    }
  }
  if (entry.coding != Light && (best.isEmpty() || best.size() * 16 > plain)) {
    QByteArray planes = packPlanes(image);
    if (!planes.isEmpty() && (best.isEmpty() || planes.size() < best.size())) {
      best = std::move(planes);
      entry.coding = Planes;
    }
  }
  // And the plain rows for anything that defeats both, so a frame is never
  // stored larger than its own pixels.
  if (best.isEmpty() || best.size() >= plain) {
    best = rawImage(image);
    entry.coding = Rows;
  }
  // Down to what it holds. The runs are written into room for the whole raw
  // frame, and qCompress hands back room for its worst case, and neither gives
  // the rest back by itself: a frame packed to a few hundred kilobytes went on
  // occupying eight megabytes, and the cache used about twice the memory it
  // counted -- seven gigabytes for a music video it reckoned at four.
  best.squeeze();
  entry.data = std::move(best);
  // On disk instead, when asked to: written here, on whichever thread is doing
  // the packing, so the interface never waits on a file. If the file cannot be
  // written the frame simply stays in memory.
  if (const QString folder = diskDirectory(); !folder.isEmpty() && spillAllowed.load() && spillToDisk.load() &&
                                               diskRoom.load() >= qint64(entry.data.size())) {
    const QString path = folder + "/f" + QString::number(diskCounter.fetch_add(1)) + ".bin";
    QFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(entry.data) == entry.data.size()) {
      file.close();
      auto blob = std::make_shared<DiskBlob>();
      blob->path = path;
      blob->size = entry.data.size();
      entry.disk = blob;
      diskWritten.fetch_add(blob->size);
      diskRoom.fetch_sub(blob->size);
      entry.data.clear();
    } else {
      file.close();
      QFile::remove(path);
    }
  }
  return entry;
}
void PreRenderCache::storePrepared(int frame,const Prepared &entry) {
  if((entry.data.isEmpty() && !entry.disk && entry.image.isNull()) || sig.global.isEmpty() || !sig.stableAt(frame))return;
  // A frame larger than the whole budget would evict everything and still not
  // fit, so it is not kept at all rather than emptying the cache to fail.
  if (sizeOf(entry) > limit) return;
  place(frame, entry);
}

bool PreRenderCache::shareFrom(int like, int frame) {
  if (sig.global.isEmpty() || !sig.stableAt(frame)) return false;
  const auto found = frames.constFind(like);
  if (found == frames.constEnd()) return false;
  // The byte array is implicitly shared, so this is a pointer, not a picture.
  place(frame, *found);
  return true;
}

void PreRenderCache::place(int frame, Held entry) {
  const auto existing = frames.constFind(frame);
  if (existing != frames.constEnd()) {
    refund(*existing);
    order.removeOne(frame);
  }
  frames[frame] = std::move(entry);
  order.push_back(frame);
  charge(frames[frame]);
  if (recentFrame == frame) {
    recentFrame = -1;
    recentImage = QImage();
  }
  evict();
}

void PreRenderCache::charge(const Held &entry) {
  if (++shares[keyOf(entry)] == 1) {
    held += sizeOf(entry);
    if (!entry.image.isNull()) hotHeld += sizeOf(entry);
    if (entry.disk) diskHeld += sizeOf(entry);
    updateSpill();
  }
}

void PreRenderCache::refund(const Held &entry) {
  const auto found = shares.find(keyOf(entry));
  if (found == shares.end()) return;
  if (--found.value() > 0) return;
  held -= sizeOf(entry);
  if (!entry.image.isNull()) hotHeld -= sizeOf(entry);
  if (entry.disk) diskHeld -= sizeOf(entry);
  decoded->remove(keyOf(entry));
  shares.erase(found);
  updateSpill();
}
void PreRenderCache::evict() {
  while (held > limit && !order.isEmpty()) {
    const int oldest = order.takeFirst();
    const auto found = frames.constFind(oldest);
    if (found == frames.constEnd()) continue;
    refund(*found);
    frames.erase(found);
    if (recentFrame == oldest) {
      recentFrame = -1;
      recentImage = QImage();
    }
  }
}

void PreRenderCache::invalidate(int from, int to) {
  for (int frame = from; frame <= to && !frames.isEmpty(); ++frame) {
    const auto found = frames.constFind(frame);
    if (found == frames.constEnd()) continue;
    refund(*found);
    frames.erase(found);
    order.removeOne(frame);
    if (recentFrame == frame) {
      recentFrame = -1;
      recentImage = QImage();
    }
  }
}

void PreRenderCache::dropAll() {
  frames.clear();
  order.clear();
  shares.clear();
  held = 0;
  hotHeld = 0;
  diskHeld = 0;
  updateSpill();
  recentFrame = -1;
  recentImage = QImage();
  recentHeld = Held();
  decoded->clear();
}

qint64 PreRenderCache::clearStaleSessionFolders(const QString &base, const QString &keep) {
  qint64 freed = 0;
  const QDir root(base);
  if (base.isEmpty() || !root.exists()) return 0;
  const QString kept = keep.isEmpty() ? QString() : QFileInfo(keep).absoluteFilePath();
  for (const QFileInfo &entry : root.entryInfoList({"session-*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
    if (entry.absoluteFilePath() == kept) continue;
    bool ok = false;
    const qulonglong pid = entry.fileName().mid(8).toULongLong(&ok);
    if (!ok) continue;
#ifdef Q_OS_WIN
    // Another Wavy-Flo still running keeps its frames there: left alone.
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid))) {
      DWORD code = 0;
      const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
      CloseHandle(process);
      if (running) continue;
    }
#endif
    QDirIterator files(entry.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) freed += QFileInfo(files.next()).size();
    QDir(entry.absoluteFilePath()).removeRecursively();
  }
  return freed;
}
void PreRenderCache::clear() {
  dropAll();
  sig = PreRenderSignature();
}

QVector<QPair<int, int>> PreRenderCache::ranges(int from, int to) const {
  QVector<QPair<int, int>> spans;
  for (int frame = from; frame <= to; ++frame) {
    if (!frames.contains(frame)) continue;
    if (!spans.isEmpty() && spans.back().second == frame - 1) spans.back().second = frame;
    else spans.push_back({frame, frame});
  }
  return spans;
}

namespace {
// Pixels with no runs in them, so packing cannot help and a test about the
// budget is about the budget rather than about the codec.
QImage noisyFrame(int width, int height) {
  QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
  quint32 state = 0x12345678;
  for (int y = 0; y < height; ++y) {
    auto *row = reinterpret_cast<quint32 *>(image.scanLine(y));
    for (int x = 0; x < width; ++x) {
      state = state * 1664525u + 1013904223u;
      row[x] = state;
    }
  }
  return image;
}
// A smooth ramp in every channel: what a glow actually looks like, and the
// thing run coding is blind to because no two neighbouring pixels are alike.
QImage gradientFrame(int width, int height) {
  QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
  for (int y = 0; y < height; ++y) {
    auto *row = reinterpret_cast<quint32 *>(image.scanLine(y));
    for (int x = 0; x < width; ++x) {
      const int reach = width / 2 + height / 2;
      const int fall = std::abs(x - width / 2) + std::abs(y - height / 2);
      const int v = std::clamp(255 - fall * 255 / std::max(1, reach), 0, 255);
      row[x] = 0xff000000u | quint32((v / 2) << 16) | quint32((v * 2 / 3) << 8) | quint32(v);
    }
  }
  return image;
}
QImage testFrame(int size, QColor colour = Qt::white) {
  QImage image(size, size, QImage::Format_RGBA8888_Premultiplied);
  image.fill(colour);
  return image;
}
// Two layers that do not overlap in time, so it is visible at a glance which
// frames an edit to one of them reached.
Scene twoLayerScene() {
  Scene s;
  s.frames = 200;
  Layer early, late;
  early.id = "early";
  early.start = 0;
  early.duration = 60;
  late.id = "late";
  late.start = 100;
  late.duration = 60;
  s.layers = {early, late};
  return s;
}
Layer &layerNamed(Scene &s, const QString &id) {
  for (auto &l : s.layers)
    if (l.id == id) return l;
  return s.layers.front();
}
}  // namespace

int PreRenderCache::furthestAhead(int from, int total) const {
  int furthest = -1, reach = -1;
  for (auto it = frames.constBegin(); it != frames.constEnd(); ++it) {
    const int ahead = total > 0 ? ((it.key() - from) % total + total) % total
                                : std::abs(it.key() - from);
    if (ahead <= reach) continue;
    reach = ahead;
    furthest = it.key();
  }
  return furthest;
}

int checkPreRender(QStringList &failures) {
  auto frame = [](int size, QColor colour) { return testFrame(size, colour); };
  {
    // What goes to files is counted, and stops at the room it is given: past
    // the day's limit frames stay in memory.
    QTemporaryDir folder;
    const QString was = PreRenderCache::diskDirectory();
    PreRenderCache::setDiskDirectory(folder.path());
    spillToDisk.store(true);
    const qint64 before = PreRenderCache::diskBytesWritten();
    PreRenderCache::setDiskWriteRoom(std::numeric_limits<qint64>::max());
    const auto written = PreRenderCache::prepare(gradientFrame(320, 180));
    const qint64 counted = PreRenderCache::diskBytesWritten() - before;
    if (!written.disk || counted != written.disk->size || counted <= 0)
      failures << "frames written to the cache folder are counted";
    PreRenderCache::setDiskWriteRoom(0);
    const auto kept = PreRenderCache::prepare(gradientFrame(320, 180));
    if (kept.disk || kept.data.isEmpty() || PreRenderCache::diskBytesWritten() - before != counted)
      failures << "with no room left for the day, frames stay in memory";
    PreRenderCache::setDiskWriteRoom(std::numeric_limits<qint64>::max());
    spillToDisk.store(false);
    PreRenderCache::setDiskDirectory(was);
  }
  {
    PreRenderCache cache;cache.expect("shared-playback");
    const auto image=gradientFrame(320,180);
    cache.storePrepared(0,PreRenderCache::prepare(image));
    for(int i=1;i<180;++i)cache.shareFrom(0,i);
    const auto first=cache.at(0);const auto decoded=cache.decodeCount();
    for(int i=1;i<180;++i)
      if(cache.at(i).cacheKey()!=first.cacheKey()) {failures<<"static cached frames must share the decoded image";break;}
    if(cache.decodeCount()!=decoded)failures<<"playing identical green frames must not decode them again";
    cache.invalidate(3,3);
    if(!cache.at(3).isNull())failures<<"decoded picture cannot revive an invalidated frame";
    if(cache.at(4)!=first)failures<<"another valid shared frame survives partial invalidation";
    cache.store(4,frame(40,Qt::red));
    if(cache.at(4).size()!=QSize(40,40))failures<<"replacing a shared frame does not return its old decoded pixels";
    // Decode-ahead carries snapshots, so edits must reject an in-flight result.
    cache.prefetch(0,180);cache.clear();
    if(!cache.at(0).isNull())failures<<"cleared decode-ahead never exposes stale frames";
  }
  {
    PreRenderCache cache;cache.expect("moving-playback");
    for(int i=0;i<8;++i)cache.store(i,frame(128,QColor(i*25,30,80)));
    cache.prefetch(0,8);
    QElapsedTimer deadline;deadline.start();
    while(cache.decodeCount()<8 && deadline.elapsed()<3000) {
      cache.at(99); // harvest ready work without asking to decode a frame
      QThread::msleep(1);
    }
    if(cache.decodeCount()!=8)failures<<"decode-ahead prepares moving frames on its worker";
    const auto before=cache.decodeCount();
    for(int i=0;i<8;++i)
      if(cache.at(i).pixelColor(30,30)!=QColor(i*25,30,80))failures<<"decode-ahead changes cached pixels";
    if(cache.decodeCount()!=before)failures<<"ready moving frames require no GUI decoding";
  }
  {
    PreRenderCache cache;
    cache.expect("first");
    cache.store(3, frame(16, Qt::red));
    if (!cache.has(3) || cache.at(3).isNull()) failures << "a stored frame comes back";
    if (cache.has(4)) failures << "a frame that was never stored is not claimed";
    // The promise the whole thing rests on.
    if (!cache.expect("second")) failures << "a new fingerprint is noticed";
    if (cache.has(3) || !cache.at(3).isNull())
      failures << "nothing from the old fingerprint survives into the new one";
    if (cache.expect("second")) failures << "the same fingerprint is not a change";
  }
  {
    // Storing without saying what the frames are pictures of keeps nothing,
    // because there would be no fingerprint to invalidate them against.
    PreRenderCache cache;
    cache.store(0, frame(16, Qt::blue));
    if (cache.has(0)) failures << "a frame stored before any fingerprint is refused";
  }
  {
    PreRenderCache cache;
    cache.expect("budget");
    const auto one = noisyFrame(64, 64);
    cache.setBudget(one.sizeInBytes() * 3);
    for (int n = 0; n < 10; ++n) cache.store(n, noisyFrame(64, 64));
    if (cache.bytes() > cache.budget()) failures << "the cache stays inside its budget";
    if (cache.count() != 3) failures << QString("it keeps what fits, not %1").arg(cache.count());
    if (!cache.has(9) || cache.has(0)) failures << "and it keeps the newest, not the oldest";
    // One frame too large to ever fit must not empty the cache trying.
    const int before = cache.count();
    cache.store(99, noisyFrame(512, 512));
    if (cache.count() != before || cache.has(99))
      failures << "a frame bigger than the whole budget is refused, not accommodated";
  }
  {
    PreRenderCache cache;
    cache.expect("ranges");
    for (int n : {2, 3, 4, 7, 8, 12}) cache.store(n, frame(8, Qt::white));
    const auto spans = cache.ranges(0, 20);
    const QVector<QPair<int, int>> wanted{{2, 4}, {7, 8}, {12, 12}};
    if (spans != wanted) {
      QStringList shown;
      for (const auto &span : spans) shown << QString("%1-%2").arg(span.first).arg(span.second);
      failures << "cached frames report as contiguous runs, got " + shown.join(", ");
    }
    if (!cache.ranges(30, 40).isEmpty()) failures << "an empty stretch reports no runs";
  }

  // What the per-layer signature is for: an edit to one layer costs the frames
  // that layer is in, and not one frame more.
  auto filled = [&](const Scene &s) {
    PreRenderCache cache;
    cache.reconcile(preRenderSignature(s, "ctx"));
    for (int n = 0; n < 200; ++n) cache.store(n, frame(4, Qt::white));
    return cache;
  };
  auto kept = [](const PreRenderCache &cache, std::initializer_list<int> frames) {
    QStringList missing;
    for (int n : frames)
      if (!cache.has(n)) missing << QString::number(n);
    return missing;
  };
  auto gone = [](const PreRenderCache &cache, std::initializer_list<int> frames) {
    QStringList surviving;
    for (int n : frames)
      if (cache.has(n)) surviving << QString::number(n);
    return surviving;
  };
  {
    const Scene scene = twoLayerScene();
    auto cache = filled(scene);
    if (cache.count() != 200) failures << "every frame of a settled scene can be held";
    if (cache.reconcile(preRenderSignature(scene, "ctx")) != 0)
      failures << "re-stating an unchanged scene drops nothing";
  }
  {
    // One layer recoloured. The other one is nowhere near it in time.
    Scene edited = twoLayerScene();
    auto cache = filled(twoLayerScene());
    layerNamed(edited, "early").color = Qt::red;
    cache.reconcile(preRenderSignature(edited, "ctx"));
    const auto survivors = gone(cache, {0, 30, 59});
    if (!survivors.isEmpty())
      failures << "editing a layer drops the frames it is in, kept " + survivors.join(", ");
    const auto lost = kept(cache, {70, 100, 130, 199});
    if (!lost.isEmpty())
      failures << "and leaves the frames it is not in, dropped " + lost.join(", ");
  }
  {
    // The repeater's Z position moves a 3D layer just like X and Y do, so
    // dragging it must not reuse a frame rendered before the drag.
    Scene start = twoLayerScene();
    layerNamed(start, "early").threeD = true;
    layerNamed(start, "early").repeaterEnabled = true;
    Scene edited = start;
    auto cache = filled(start);
    layerNamed(edited, "early").repeaterPositionZ = 50;
    cache.reconcile(preRenderSignature(edited, "ctx"));
    const auto survivors = gone(cache, {0, 30, 59});
    if (!survivors.isEmpty())
      failures << "dragging the repeater's Z position drops the frames it is in, kept " +
                      survivors.join(", ");
  }
  {
    // Dragged along the timeline: the stretch it came from and the stretch it
    // went to both stop being true.
    Scene edited = twoLayerScene();
    auto cache = filled(twoLayerScene());
    layerNamed(edited, "late").start = 20;
    cache.reconcile(preRenderSignature(edited, "ctx"));
    const auto survivors = gone(cache, {25, 60, 110, 150});
    if (!survivors.isEmpty())
      failures << "moving a layer in time drops where it was and where it is, kept " +
                      survivors.join(", ");
    const auto lost = kept(cache, {0, 90, 175, 199});
    if (!lost.isEmpty()) failures << "and nothing else, dropped " + lost.join(", ");
  }
  {
    // A new layer in front of everything changes nothing behind it.
    Scene edited = twoLayerScene();
    auto cache = filled(twoLayerScene());
    Layer added;
    added.id = "added";
    added.start = 150;
    added.duration = 50;
    edited.layers.insert(edited.layers.begin(), added);
    cache.reconcile(preRenderSignature(edited, "ctx"));
    const auto lost = kept(cache, {0, 30, 100, 140});
    if (!lost.isEmpty())
      failures << "adding a layer keeps the frames it does not reach, dropped " + lost.join(", ");
    const auto survivors = gone(cache, {155, 180});
    if (!survivors.isEmpty())
      failures << "and drops the ones it does, kept " + survivors.join(", ");
  }
  {
    // Reordering two layers changes both of them, because either could now be
    // the one on top.
    Scene edited = twoLayerScene();
    auto cache = filled(twoLayerScene());
    std::swap(edited.layers[0], edited.layers[1]);
    cache.reconcile(preRenderSignature(edited, "ctx"));
    const auto survivors = gone(cache, {30, 130});
    if (!survivors.isEmpty())
      failures << "restacking layers drops both their stretches, kept " + survivors.join(", ");
  }
  {
    // A child moves when its parent does, so a parent edit has to reach the
    // child's frames as well as its own.
    Scene start = twoLayerScene();
    layerNamed(start, "late").parentId = "early";
    Scene edited = start;
    auto cache = filled(start);
    layerNamed(edited, "early").p[0].base = 40;
    cache.reconcile(preRenderSignature(edited, "ctx"));
    const auto survivors = gone(cache, {10, 120});
    if (!survivors.isEmpty())
      failures << "moving a parent drops its children's frames too, kept " + survivors.join(", ");
  }
  {
    // Turning one layer 3D can reorder two others that were in separate groups
    // before, at frames it is nowhere near. Nothing held survives that.
    Scene edited = twoLayerScene();
    auto cache = filled(twoLayerScene());
    layerNamed(edited, "early").threeD = true;
    cache.reconcile(preRenderSignature(edited, "ctx"));
    if (cache.count() != 0) failures << "a layer turning 3D empties the cache";
  }
  {
    // And once anything is 3D, the camera decides where all of it lands.
    Scene start = twoLayerScene();
    layerNamed(start, "early").threeD = true;
    Layer camera;
    camera.id = "camera";
    camera.type = "camera";
    camera.threeD = true;
    camera.start = 0;
    camera.duration = 200;
    start.layers.push_back(camera);
    Scene edited = start;
    auto cache = filled(start);
    layerNamed(edited, "camera").p[0].base = 15;
    cache.reconcile(preRenderSignature(edited, "ctx"));
    if (cache.count() != 0) failures << "moving the camera empties the cache";
  }
  {
    // The composition itself.
    Scene edited = twoLayerScene();
    auto cache = filled(twoLayerScene());
    edited.width = 1280;
    cache.reconcile(preRenderSignature(edited, "ctx"));
    if (cache.count() != 0) failures << "resizing the composition empties the cache";
  }
  {
    // The same scene rendered for a different preview size is a different set
    // of pictures, and the context is how the caller says so.
    Scene scene = twoLayerScene();
    auto cache = filled(scene);
    cache.reconcile(preRenderSignature(scene, "other"));
    if (cache.count() != 0) failures << "a different render context empties the cache";
  }
  {
    PreRenderCache cache;
    cache.expect("ahead");
    for (int n : {10, 40, 90}) cache.store(n, frame(8, Qt::white));
    if (cache.furthestAhead(80, 100) != 40)
      failures << "the frame furthest ahead of the playhead counts forward and wraps";
    if (cache.furthestAhead(0, 100) != 90) failures << "and without wrapping is simply the last";
    PreRenderCache empty;
    if (empty.furthestAhead(0, 100) != -1) failures << "an empty cache has nothing to give up";
  }
  {
    // Asked per frame, so a render that finishes after an edit somewhere else
    // on the timeline is still worth keeping.
    Scene edited = twoLayerScene();
    const auto before = preRenderSignature(twoLayerScene(), "ctx");
    layerNamed(edited, "early").color = Qt::red;
    const auto after = preRenderSignature(edited, "ctx");
    if (before.frameKey(30) == after.frameKey(30))
      failures << "a frame notices an edit to the layer that is on screen in it";
    if (before.frameKey(130) != after.frameKey(130))
      failures << "and ignores an edit to a layer that is not";
  }
  {
    // Lossless is the whole point: the preview must not change because the
    // frame came back from the cache rather than from the renderer.
    auto roundTrip = [&](const QImage &original, const QString &what) {
      const auto packed = packImage(original);
      const auto back = unpackImage(packed, original.size(), original.format());
      if (back.isNull() || back != original) failures << "packing " + what + " is lossless";
      return qint64(packed.size());
    };
    QImage empty(320, 180, QImage::Format_RGBA8888_Premultiplied);
    empty.fill(Qt::transparent);
    const qint64 plain = qint64(empty.width()) * empty.height() * 4;
    const qint64 emptySize = roundTrip(empty, "an empty frame");
    if (emptySize > plain / 50)
      failures << QString("an empty frame packs to almost nothing, got %1 of %2 bytes")
                      .arg(emptySize)
                      .arg(plain);
    // The shape of the thing actually being cached: one title over nothing.
    QImage title = empty;
    {
      QPainter paint(&title);
      paint.fillRect(60, 60, 200, 60, Qt::white);
    }
    const qint64 titleSize = roundTrip(title, "a title over nothing");
    if (titleSize > plain / 10)
      failures << QString("a title over nothing packs small, got %1 of %2 bytes")
                      .arg(titleSize)
                      .arg(plain);
    roundTrip(noisyFrame(320, 180), "noise");
    // An odd width, and more than one byte a channel.
    QImage deep(37, 11, QImage::Format_RGBA64_Premultiplied);
    deep.fill(QColor(10, 20, 30, 40));
    roundTrip(deep, "sixteen bits a channel");
    QImage single(1, 1, QImage::Format_RGBA8888_Premultiplied);
    single.fill(Qt::red);
    roundTrip(single, "a single pixel");
  }
  {
    // The second codec, over the thing the first one is blind to.
    auto planeTrip = [&](const QImage &original, const QString &what) {
      const auto packed = packPlanes(original);
      const auto back = unpackPlanes(packed, original.size(), original.format());
      if (back.isNull() || back != original)
        failures << "taking " + what + " apart into planes is lossless";
      return qint64(packed.size());
    };
    const QImage glow = gradientFrame(480, 270);
    const qint64 plain = qint64(glow.width()) * glow.height() * 4;
    const qint64 runs = packImage(glow).size();
    const qint64 planes = planeTrip(glow, "a gradient");
    if (runs * 4 < plain)
      failures << QString("runs really are blind to a gradient, got %1x").arg(plain / double(runs), 0, 'f', 1);
    if (planes * 8 > plain)
      failures << QString("planes see it, got only %1x").arg(plain / double(planes), 0, 'f', 1);
    planeTrip(gradientFrame(37, 11), "an odd width");
    planeTrip(noisyFrame(64, 64), "noise");
    QImage deep(40, 12, QImage::Format_RGBA64_Premultiplied);
    deep.fill(QColor(10, 20, 30, 40));
    planeTrip(deep, "sixteen bits a channel");
  }
  {
    // Whichever came out smallest, without the caller having to know which.
    PreRenderCache cache;
    cache.expect("pick");
    QImage flat(480, 270, QImage::Format_RGBA8888_Premultiplied);
    flat.fill(Qt::transparent);
    cache.store(0, flat);
    if (cache.bytes() != packImage(flat).size())
      failures << "a flat frame is kept as runs, which win there and cost least to find";
    if (cache.at(0) != flat) failures << "and comes back exactly";

    PreRenderCache second;
    second.expect("pick");
    const QImage glow = gradientFrame(480, 270);
    second.store(0, glow);
    if (second.bytes() != packPlanes(glow).size())
      failures << QString("a gradient is kept as planes, %1 vs %2")
                      .arg(second.bytes())
                      .arg(packPlanes(glow).size());
    if (second.at(0) != glow) failures << "and it too comes back exactly";
    const qint64 plain = qint64(glow.width()) * glow.height() * 4;
    if (second.bytes() * 8 > plain)
      failures << QString("which is what makes a busy frame affordable, got %1x")
                      .arg(plain / double(second.bytes()), 0, 'f', 1);
  }
  {
    // Noise is the one case packing cannot win, and losing by a hair on every
    // frame would be worse than not trying.
    PreRenderCache cache;
    cache.expect("noise");
    const auto one = noisyFrame(128, 128);
    cache.store(0, one);
    if (cache.bytes() > one.sizeInBytes())
      failures << QString("packing never costs more than not packing, %1 vs %2")
                      .arg(cache.bytes())
                      .arg(one.sizeInBytes());
    if (cache.at(0) != one) failures << "and the frame comes back exactly as it went in";
  }
  {
    // A title standing still for three seconds is ninety frames of timeline and
    // one picture.
    PreRenderCache cache;
    cache.expect("alike");
    cache.store(0, noisyFrame(64, 64));
    const qint64 one = cache.bytes();
    if (one <= 0) failures << "a stored frame costs something";
    for (int n = 1; n < 90; ++n)
      if (!cache.shareFrom(0, n)) failures << "a frame can be recorded as looking like another";
    if (cache.count() != 90) failures << "all ninety are held";
    if (cache.bytes() != one)
      failures << QString("and cost one picture between them, not %1 of them")
                      .arg(cache.bytes() / double(one), 0, 'f', 1);
    if (cache.at(89) != cache.at(0)) failures << "and are the same picture";
    cache.invalidate(1, 89);
    if (cache.bytes() != one) failures << "dropping the copies leaves the original charged";
    cache.invalidate(0, 0);
    if (cache.bytes() != 0) failures << "and dropping the last of them frees it";
    if (cache.shareFrom(400, 401)) failures << "nothing can look like a frame that is not held";
  }
  {
    // Taken on every edit, on the typing path, so it has to cost nothing next
    // to the render it saves.
    Scene heavy;
    heavy.frames = 3600;
    for (int n = 0; n < 80; ++n) {
      Layer l;
      l.id = QString("layer%1").arg(n);
      l.start = n * 20;
      l.duration = 200;
      l.text = QString("line %1").arg(n);
      for (int key = 0; key < 20; ++key) l.p[0].set(key * 10, key * 3.);
      heavy.layers.push_back(l);
    }
    QElapsedTimer clock;
    clock.start();
    for (int n = 0; n < 10; ++n) preRenderSignature(heavy, "ctx");
    const double each = clock.nsecsElapsed() / 1e6 / 10;
    if (each > 8)
      failures << QString("signing 80 layers stays off the typing path, took %1 ms")
                      .arg(each, 0, 'f', 2);
  }
  {
    // Video is decoded by the player as the playhead moves, so a stored video
    // frame would be the same picture forever.
    Scene scene = twoLayerScene();
    layerNamed(scene, "early").type = "video";
    auto cache = filled(scene);
    const auto survivors = gone(cache, {0, 30, 59});
    if (!survivors.isEmpty())
      failures << "frames holding video are never stored, kept " + survivors.join(", ");
    const auto lost = kept(cache, {100, 150});
    if (!lost.isEmpty())
      failures << "while the rest of the timeline still caches, dropped " + lost.join(", ");
  }
  {
    // Kept as rendered, packed later: the pixels never change, the memory comes
    // back, and every frame that showed the picture shows the packed one.
    PreRenderCache cache;
    cache.expect("kept");
    const QImage glow = gradientFrame(480, 270);
    cache.storePrepared(0, PreRenderCache::keep(glow));
    for (int n = 1; n < 10; ++n) cache.shareFrom(0, n);
    const qint64 raw = glow.sizeInBytes();
    if (cache.bytes() != raw || cache.hotBytes() != raw) failures << "a kept frame costs its own pixels, once";
    const auto decodes = cache.decodeCount();
    if (cache.at(5) != glow || cache.decodeCount() != decodes) failures << "a kept frame is shown without unpacking";
    const auto hot = cache.hotPictures(0, 10, 4);
    if (hot.size() != 1) failures << "ten frames of one kept picture are one picture to pack";
    else {
      cache.repack(hot[0], PreRenderCache::prepare(hot[0].image));
      if (cache.hotBytes() != 0 || cache.bytes() * 8 > raw) failures << "packing a kept picture gives its memory back";
      if (cache.count() != 10) failures << "and keeps every frame that showed it";
      for (int n : {0, 9})
        if (cache.at(n) != glow) failures << "and the frames come back exactly";
    }
    // Dropped while it was being packed: the packed copy must bring nothing back.
    cache.storePrepared(20, PreRenderCache::keep(noisyFrame(64, 64)));
    const auto late = cache.hotPictures(0, 30, 4);
    cache.invalidate(20, 20);
    if (!late.empty()) cache.repack(late[0], PreRenderCache::prepare(late[0].image));
    if (cache.has(20) || cache.hotBytes() != 0) failures << "a frame dropped while it was being packed stays dropped";
  }
  {
    // The picture needed last is packed first.
    PreRenderCache cache;
    cache.expect("order");
    for (int n : {10, 40, 90}) cache.storePrepared(n, PreRenderCache::keep(noisyFrame(8 + n % 7, 8)));
    const auto next = cache.hotPictures(50, 100, 1);
    if (next.size() != 1 || next[0].size != QSize(8 + 40 % 7, 8))
      failures << "packing starts with the frame furthest ahead of the playhead, wrapping";
  }
  {
    // Light frames. A picture like a music video's: smooth colour under a fine
    // grain, the kind that packs worst exactly, with a soft edge of coverage.
    QImage busy(640, 360, QImage::Format_RGBA8888);
    quint32 grain = 12345;
    for (int y = 0; y < busy.height(); ++y)
      for (int x = 0; x < busy.width(); ++x) {
        grain = grain * 1664525u + 1013904223u;
        const int noise = int((grain >> 24) % 13) - 6;
        const int alpha = std::clamp(255 - std::max(0, x - 560) * 3, 0, 255);
        busy.setPixelColor(x, y, QColor(std::clamp(40 + x * 180 / 640 + noise, 0, 255),
                                         std::clamp(60 + y * 150 / 360 + noise, 0, 255),
                                         std::clamp(90 + (x + y) * 100 / 1000 + noise, 0, 255), alpha));
      }
    const QByteArray exact = packPlanes(busy);
    const QByteArray light = packLight(busy);
    const QImage back = unpackLight(light, busy.size(), busy.format());
    if (light.isEmpty() || back.size() != busy.size() || back.format() != busy.format())
      failures << "a light frame comes back the size and kind it went in";
    else {
      if (light.size() * 3 > exact.size())
        failures << QString("a light frame is a fraction of an exact one (%1 against %2 bytes)").arg(light.size()).arg(exact.size());
      int worst = 0;
      double total = 0;
      bool coverage = true;
      for (int y = 0; y < busy.height(); ++y)
        for (int x = 0; x < busy.width(); ++x) {
          const QColor a = busy.pixelColor(x, y), b = back.pixelColor(x, y);
          if (a.alpha() != b.alpha()) coverage = false;
          if (a.alpha() < 64) continue; // colour under little coverage barely shows
          const int d = std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()), std::abs(a.blue() - b.blue())});
          worst = std::max(worst, d);
          total += d;
        }
      if (!coverage) failures << "a light frame keeps its coverage exactly";
      const double mean = total / (busy.width() * busy.height());
      if (mean > 5) failures << QString("a light frame stays close to the picture (mean %1 levels off)").arg(mean, 0, 'f', 2);
    }
    // An opaque frame carries no coverage at all.
    QImage opaque = busy.copy();
    opaque.fill(QColor(20, 40, 60));
    const QByteArray solid = packLight(opaque.convertToFormat(QImage::Format_RGB32));
    quint32 jpegSize = 0, alphaSize = 1;
    { QDataStream stream(solid); stream >> jpegSize >> alphaSize; }
    if (alphaSize != 0) failures << "an opaque light frame stores no coverage";
    // Through the cache: busy frames go light, a flat one stays exact.
    PreRenderCache::setLight(true);
    PreRenderCache cache;
    cache.expect("light");
    cache.storePrepared(0, PreRenderCache::prepare(busy));
    QImage flat(640, 360, QImage::Format_RGBA8888);
    flat.fill(QColor(12, 34, 56));
    cache.storePrepared(1, PreRenderCache::prepare(flat));
    PreRenderCache::setLight(false);
    if (cache.at(0).isNull() || cache.at(0).size() != busy.size()) failures << "a light cache shows its busy frames";
    if (cache.at(1) != flat) failures << "a flat frame stays exact in a light cache";
    if (cache.bytes() * 3 > exact.size() + 4096) failures << "a light cache holds a fraction of the bytes";
  }
  return failures.isEmpty() ? 0 : 1;
}
