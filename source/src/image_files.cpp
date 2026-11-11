#include "image_files.h"
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QTemporaryDir>
#include <QTransform>
#include <algorithm>
#include <cstring>
#include <tuple>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <propvarutil.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace {
// A picture larger than this many pixels is refused rather than allocated.
constexpr qint64 kMostPixels = 400LL * 1000 * 1000;

// Upright, as an orientation tag (EXIF 1..8) says the picture is meant to be
// seen. Qt's reader does this itself; Windows' does not.
QImage upright(const QImage &image, int orientation) {
  switch (orientation) {
  case 2: return image.mirrored(true, false);
  case 3: return image.transformed(QTransform().rotate(180));
  case 4: return image.mirrored(false, true);
  case 5: return image.mirrored(true, false).transformed(QTransform().rotate(270));
  case 6: return image.transformed(QTransform().rotate(90));
  case 7: return image.mirrored(true, false).transformed(QTransform().rotate(90));
  case 8: return image.transformed(QTransform().rotate(270));
  default: return image;
  }
}

// Targa, which neither Qt nor Windows reads: uncompressed or run-length
// encoded, true colour (24 or 32 bit) or grey (8 bit), either row order.
QImage readTga(const QString &path, QString *error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return {}; }
  const QByteArray data = file.readAll();
  auto fail = [&](const char *why) { if (error) *error = why; return QImage(); };
  if (data.size() < 18) return fail("not a Targa file");
  const auto *h = reinterpret_cast<const uchar *>(data.constData());
  const int idLength = h[0], mapType = h[1], kind = h[2];
  const int width = h[12] | (h[13] << 8), height = h[14] | (h[15] << 8), bits = h[16], descriptor = h[17];
  const bool rle = kind == 10 || kind == 11, grey = kind == 3 || kind == 11;
  if (mapType != 0 || !(kind == 2 || kind == 3 || kind == 10 || kind == 11)) return fail("a kind of Targa file that is not read");
  if (width <= 0 || height <= 0 || qint64(width) * height > kMostPixels) return fail("a Targa picture of no or too great a size");
  const int bytes = bits / 8;
  if (!(grey ? bits == 8 : (bits == 24 || bits == 32))) return fail("a Targa colour depth that is not read");
  const qint64 pixels = qint64(width) * height;
  QByteArray raw;
  raw.resize(int(pixels * bytes));
  qint64 at = 18 + idLength, written = 0;
  auto *out = reinterpret_cast<uchar *>(raw.data());
  if (!rle) {
    if (at + pixels * bytes > data.size()) return fail("a Targa file cut short");
    std::memcpy(out, h + at, size_t(pixels * bytes));
  } else {
    while (written < pixels) {
      if (at >= data.size()) return fail("a Targa file cut short");
      const int packet = h[at++], count = (packet & 0x7f) + 1;
      if (written + count > pixels) return fail("a damaged Targa file");
      if (packet & 0x80) {
        if (at + bytes > data.size()) return fail("a Targa file cut short");
        for (int i = 0; i < count; ++i) std::memcpy(out + (written + i) * bytes, h + at, size_t(bytes));
        at += bytes;
      } else {
        if (at + qint64(count) * bytes > data.size()) return fail("a Targa file cut short");
        std::memcpy(out + written * bytes, h + at, size_t(count) * size_t(bytes));
        at += qint64(count) * bytes;
      }
      written += count;
    }
  }
  const bool alpha = bits == 32 && (descriptor & 0x0f) != 0;
  QImage image(width, height, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
  if (image.isNull()) return fail("not enough memory for the picture");
  const bool topFirst = (descriptor & 0x20) != 0, rightFirst = (descriptor & 0x10) != 0;
  for (int y = 0; y < height; ++y) {
    const int row = topFirst ? y : height - 1 - y;
    auto *line = reinterpret_cast<QRgb *>(image.scanLine(row));
    for (int x = 0; x < width; ++x) {
      const uchar *p = out + (qint64(y) * width + x) * bytes;
      const int column = rightFirst ? width - 1 - x : x;
      line[column] = grey ? qRgb(p[0], p[0], p[0])
                          : qRgba(p[2], p[1], p[0], bits == 32 && alpha ? p[3] : 255);
    }
  }
  return image;
}

#ifdef _WIN32
// COM on this thread for as long as it is needed, whatever the thread had.
struct ComScope {
  bool owned = false;
  ComScope() { owned = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
  ~ComScope() { if (owned) CoUninitialize(); }
};
ComPtr<IWICBitmapDecoder> wicDecoder(const QString &path, ComPtr<IWICImagingFactory> &factory) {
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    return nullptr;
  ComPtr<IWICBitmapDecoder> decoder;
  const std::wstring name = QFileInfo(path).absoluteFilePath().toStdWString();
  if (FAILED(factory->CreateDecoderFromFilename(name.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &decoder)))
    return nullptr;
  return decoder;
}
ComPtr<IWICBitmapFrameDecode> wicFrame(const QString &path, ComPtr<IWICImagingFactory> &factory, UINT index = 0) {
  ComPtr<IWICBitmapDecoder> decoder = wicDecoder(path, factory);
  ComPtr<IWICBitmapFrameDecode> frame;
  if (!decoder || FAILED(decoder->GetFrame(index, &frame))) return nullptr;
  return frame;
}
QImage wicPixels(IWICImagingFactory *factory, IWICBitmapFrameDecode *frame, QString *error) {
  UINT width = 0, height = 0;
  if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
      qint64(width) * height > kMostPixels) {
    if (error) *error = "a picture of no or too great a size";
    return {};
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0, WICBitmapPaletteTypeMedianCut))) {
    if (error) *error = "Windows cannot turn it into colours";
    return {};
  }
  // Premultiplied BGRA in memory is what Qt calls ARGB32 premultiplied.
  QImage image(int(width), int(height), QImage::Format_ARGB32_Premultiplied);
  if (image.isNull()) { if (error) *error = "not enough memory for the picture"; return {}; }
  if (FAILED(converter->CopyPixels(nullptr, UINT(image.bytesPerLine()), UINT(image.sizeInBytes()), image.bits()))) {
    if (error) *error = "Windows could not decode it";
    return {};
  }
  return image;
}
QImage readWithWindows(const QString &path, QString *error) {
  ComScope com;
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmapFrameDecode> frame = wicFrame(path, factory);
  if (!frame) { if (error) *error = "Windows has no decoder for it"; return {}; }
  const QImage image = wicPixels(factory.Get(), frame.Get(), error);
  if (image.isNull()) return {};
  int orientation = 1;
  ComPtr<IWICMetadataQueryReader> metadata;
  if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata))) {
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(metadata->GetMetadataByName(L"System.Photo.Orientation", &value)) && value.vt == VT_UI2)
      orientation = value.uiVal;
    PropVariantClear(&value);
  }
  return upright(image, orientation);
}
bool windowsReads(const QString &path) {
  ComScope com;
  ComPtr<IWICImagingFactory> factory;
  return wicFrame(path, factory) != nullptr;
}
// An animated picture through Windows' reader (WebP), which lays each frame on
// those before it as the file says: whole pictures, each with its time.
int wicDelayMs(IWICBitmapFrameDecode *frame) {
  ComPtr<IWICMetadataQueryReader> metadata;
  if (FAILED(frame->GetMetadataQueryReader(&metadata))) return 0;
  PROPVARIANT value;
  PropVariantInit(&value);
  int delay = 0;
  if (SUCCEEDED(metadata->GetMetadataByName(L"/ANMF/FrameDuration", &value)))
    delay = value.vt == VT_UI4 ? int(std::min<ULONG>(value.ulVal, 1000000)) : value.vt == VT_UI2 ? int(value.uiVal) : 0;
  PropVariantClear(&value);
  return delay;
}
// Every frame in order, its pictures only when `pixels`; false from `each`
// stops. The number of frames, 0 when Windows cannot read it.
int wicAnimation(const QString &path, bool pixels, const std::function<bool(const QImage &, int)> &each) {
  ComScope com;
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmapDecoder> decoder = wicDecoder(path, factory);
  UINT count = 0;
  if (!decoder || FAILED(decoder->GetFrameCount(&count))) return 0;
  for (UINT index = 0; index < count && index < 100000; ++index) {
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(index, &frame))) return int(index);
    const QImage image = pixels ? wicPixels(factory.Get(), frame.Get(), nullptr) : QImage();
    if (pixels && image.isNull()) return int(index);
    if (!each(image, wicDelayMs(frame.Get()))) break;
  }
  return int(count);
}
QImage wicAnimationFrame(const QString &path, int index) {
  ComScope com;
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmapFrameDecode> frame = wicFrame(path, factory, UINT(index));
  return frame ? wicPixels(factory.Get(), frame.Get(), nullptr) : QImage();
}
#endif
// Qt reads GIF frames itself; Windows reads WebP ones.
bool qtAnimates(const QString &path) {
  QImageReader reader(path);
  return reader.canRead() && reader.supportsAnimation();
}
// Every frame of an animated picture in order, with its time (ms, as the file
// says): Qt's reader, or Windows'.
void animatedFrames(const QString &path, bool pixels, const std::function<bool(const QImage &, int)> &each) {
  if (qtAnimates(path)) {
    QImageReader reader(path);
    while (reader.canRead()) {
      const QImage image = reader.read();
      if (image.isNull() || !each(image, reader.nextImageDelay())) break;
    }
    return;
  }
#ifdef _WIN32
  wicAnimation(path, pixels, each);
#endif
}
// A frame's time: one given none, or under 20 ms, shows for 100 ms, as
// browsers do.
int frameDelayMs(int delay) { return delay < 20 ? 100 : delay; }
}  // namespace

QImage readImageFile(const QString &path, QString *error) {
  if (QFileInfo(path).suffix().compare("tga", Qt::CaseInsensitive) == 0 ||
      QFileInfo(path).suffix().compare("targa", Qt::CaseInsensitive) == 0)
    return readTga(path, error);
  QImageReader reader(path);
  reader.setAutoTransform(true);
  QImage image = reader.read();
  if (!image.isNull()) return image;
  const QString qtSaid = reader.errorString();
#ifdef _WIN32
  QString windowsSaid;
  image = readWithWindows(path, &windowsSaid);
  if (!image.isNull()) return image;
  // Windows is asked last, so what it said is the nearer reason.
  if (error) *error = windowsSaid.isEmpty() ? qtSaid : windowsSaid;
  return {};
#endif
  if (error) *error = qtSaid;
  return {};
}

bool canReadImageFile(const QString &path) {
  const QString suffix = QFileInfo(path).suffix().toLower();
  if (suffix == "tga" || suffix == "targa") {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = file.read(18);
    return head.size() == 18 && head[1] == 0 && QList<int>{2, 3, 10, 11}.contains(uchar(head[2]));
  }
  if (QImageReader(path).canRead()) return true;
#ifdef _WIN32
  return windowsReads(path);
#else
  return false;
#endif
}

QStringList importImageSuffixes() {
  QStringList suffixes{"png", "jpg", "jpeg", "jpe", "jfif", "bmp", "dib", "gif", "svg", "ico",
                       "tif", "tiff", "webp", "heic", "heif", "hif", "avif", "jxl", "jxr", "wdp", "hdp",
                       "dds", "tga", "targa",
                       // camera RAW, read through Windows' Raw Image Extension
                       "dng", "cr2", "cr3", "nef", "arw", "orf", "rw2", "raf", "srw", "pef"};
  for (const QByteArray &format : QImageReader::supportedImageFormats()) {
    const QString name = QString::fromLatin1(format).toLower();
    if (!suffixes.contains(name)) suffixes << name;
  }
  return suffixes;
}

QStringList importVideoSuffixes() {
  return {"mp4", "m4v", "mov", "qt", "mkv", "webm", "avi", "wmv", "asf", "mpg", "mpeg", "m2v", "mpe",
          "ts", "mts", "m2ts", "3gp", "3g2", "flv", "f4v", "ogv", "mxf", "vob", "dv"};
}

QStringList importAudioSuffixes() {
  return {"mp3", "wav", "wave", "ogg", "oga", "flac", "m4a", "m4b", "aac", "opus", "wma",
          "aif", "aiff", "aifc", "caf", "ac3", "amr"};
}

QString unreadablePictureReason(const QString &suffix) {
  const QString kind = suffix.toLower();
  if (kind == "heic" || kind == "heif" || kind == "hif")
    return "Windows reads HEIC pictures only with Microsoft's \"HEIF Image Extensions\" "
           "(free in the Microsoft Store). Install it and import again.";
  if (kind == "avif")
    return "Windows reads AVIF pictures only with Microsoft's \"AV1 Video Extension\" "
           "(free in the Microsoft Store). Install it and import again.";
  if (kind == "jxl")
    return "Windows reads JPEG XL pictures only with Microsoft's \"JPEG XL Image Extension\" "
           "(free in the Microsoft Store). Install it and import again.";
  if (kind == "webp")
    return "Windows reads WebP pictures only with Microsoft's \"Webp Image Extensions\" "
           "(free in the Microsoft Store). Install it and import again.";
  if (QStringList{"dng", "cr2", "cr3", "nef", "arw", "orf", "rw2", "raf", "srw", "pef"}.contains(kind))
    return "Windows reads camera RAW pictures only with Microsoft's \"Raw Image Extension\" "
           "(free in the Microsoft Store). Install it and import again.";
  return "the picture cannot be read (the file may be damaged, or of a kind Windows has no reader for).";
}

AnimatedImageInfo animatedImageInfo(const QString &path) {
  AnimatedImageInfo info;
  if (!isAnimatedImageName(path)) return info;
  qint64 total = 0;
  int frames = 0;
  animatedFrames(path, false, [&](const QImage &image, int delay) {
    if (frames == 0) info.size = image.size();
    total += frameDelayMs(delay);
    return ++frames <= 100000;
  });
  info.frames = frames;
  info.durationMs = frames > 1 ? total : 0;
  if (frames > 1 && info.size.isEmpty()) info.size = readImageFile(path).size();
  return info;
}

bool isAnimatedImageName(const QString &path) {
  const QString suffix = QFileInfo(path).suffix().toLower();
  return suffix == "gif" || suffix == "webp";
}

// ---------------------------------------------------------------------------
struct AnimatedImageFootage::State {
  QString path;
  QSize size;
  int shrink = 1;
  std::vector<qint64> starts, ends; // 100 ns
  std::vector<QImage> kept;
  qint64 keptBytes = 0;
  // Decoding on from the start, for a frame no longer kept; or, through
  // Windows, the frame itself.
  bool throughWindows = false;
  std::unique_ptr<QImageReader> reader;
  int readerAt = 0;
  // The last frame made smaller, so a held frame is not shrunk again.
  int shrunkIndex = -1, shrunkBy = 0;
  QImage shrunk;
  static constexpr qint64 kBudget = 512LL * 1024 * 1024;

  void keep(int index, const QImage &image) {
    if (index < 0 || index >= int(kept.size()) || !kept[size_t(index)].isNull()) return;
    if (keptBytes + image.sizeInBytes() > kBudget) return;
    kept[size_t(index)] = image;
    keptBytes += image.sizeInBytes();
  }
  QImage whole(int index) {
    if (!kept[size_t(index)].isNull()) return kept[size_t(index)];
#ifdef _WIN32
    if (throughWindows) {
      const QImage image = wicAnimationFrame(path, index);
      keep(index, image);
      return image;
    }
#endif
    if (!reader || readerAt > index) {
      reader = std::make_unique<QImageReader>(path);
      readerAt = 0;
    }
    QImage image;
    while (readerAt <= index && reader->canRead()) {
      image = reader->read().convertToFormat(QImage::Format_ARGB32_Premultiplied);
      keep(readerAt, image);
      ++readerAt;
    }
    return readerAt > index ? image : QImage();
  }
  QImage at(int index) {
    const QImage image = whole(index);
    if (shrink <= 1 || image.isNull()) return image;
    if (shrunkIndex == index && shrunkBy == shrink) return shrunk;
    shrunk = image.scaled(std::max(1, image.width() / shrink), std::max(1, image.height() / shrink),
                          Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    shrunkIndex = index; shrunkBy = shrink;
    return shrunk;
  }
  int indexAt(qint64 time) const {
    const auto after = std::upper_bound(starts.begin(), starts.end(), time);
    return std::clamp(int(after - starts.begin()) - 1, 0, int(starts.size()) - 1);
  }
};

AnimatedImageFootage::AnimatedImageFootage() : state(std::make_unique<State>()) {}
AnimatedImageFootage::~AnimatedImageFootage() = default;

bool AnimatedImageFootage::open(const QString &path) {
  if (state->path == path && !state->starts.empty()) return true;
  *state = State();
  state->path = path;
  if (!isAnimatedImageName(path)) return false;
  state->throughWindows = !qtAnimates(path);
  qint64 time = 0;
  animatedFrames(path, true, [&](const QImage &image, int delay) {
    if (state->starts.empty()) state->size = image.size();
    const qint64 length = qint64(frameDelayMs(delay)) * 10000;
    state->starts.push_back(time);
    state->ends.push_back(time + length);
    state->kept.emplace_back();
    state->keep(int(state->kept.size()) - 1, image.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    time += length;
    return state->starts.size() <= 100000;
  });
  if (state->starts.size() < 2) { *state = State(); return false; }
  return true;
}

QSize AnimatedImageFootage::size() const { return state->size; }

void AnimatedImageFootage::setShrink(int divisor) { state->shrink = std::clamp(divisor, 1, 4); }

QImage AnimatedImageFootage::frame(qint64 time, qint64 *start, qint64 *length) {
  if (state->starts.empty()) return {};
  // Played over and over, as everything that shows a GIF plays it.
  const qint64 pass = state->ends.back();
  const qint64 at = std::max<qint64>(0, time), lap = at / pass * pass;
  const int index = state->indexAt(at - lap);
  if (start) *start = lap + state->starts[size_t(index)];
  if (length) *length = state->ends[size_t(index)] - state->starts[size_t(index)];
  return state->at(index);
}

bool AnimatedImageFootage::readOn(qint64 from, const std::function<bool(qint64, const QImage &)> &picture) {
  if (state->starts.empty()) return false;
  const qint64 pass = state->ends.back();
  const qint64 at = std::max<qint64>(0, from);
  // Round and round until `picture` has seen enough -- or a day of it.
  constexpr qint64 kDay = 24LL * 3600 * 10000000;
  for (qint64 lap = at / pass * pass; lap < kDay; lap += pass)
    for (int index = lap <= at ? state->indexAt(at - lap) : 0; index < int(state->starts.size()); ++index)
      if (!picture(lap + state->starts[size_t(index)], state->at(index))) return true;
  return true;
}

QByteArray solidAnimatedGif(QSize size, const std::vector<std::pair<QRgb, int>> &frames) {
  QByteArray out("GIF89a");
  auto u16 = [&](int v) { out.append(char(v & 255)); out.append(char((v >> 8) & 255)); };
  u16(size.width()); u16(size.height());
  out.append(char(0xF1)); // a colour table of four
  out.append(char(0)); out.append(char(0));
  for (size_t i = 0; i < 4; ++i) {
    const QRgb c = i < frames.size() ? frames[i].first : qRgb(0, 0, 0);
    out.append(char(qRed(c))); out.append(char(qGreen(c))); out.append(char(qBlue(c)));
  }
  out.append("\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00", 19); // played over and over
  for (size_t i = 0; i < frames.size() && i < 4; ++i) {
    out.append("\x21\xF9\x04\x00", 4); u16(frames[i].second); out.append(char(0)); out.append(char(0));
    out.append(char(0x2C)); u16(0); u16(0); u16(size.width()); u16(size.height()); out.append(char(0));
    out.append(char(2));
    // Every pixel after a clear code, so no code grows past three bits.
    QByteArray data;
    quint32 bits = 0;
    int count = 0;
    auto put = [&](int code) {
      bits |= quint32(code) << count;
      for (count += 3; count >= 8; count -= 8) { data.append(char(bits & 255)); bits >>= 8; }
    };
    for (int p = 0; p < size.width() * size.height(); ++p) { put(4); put(int(i)); }
    put(5);
    if (count > 0) data.append(char(bits & 255));
    for (qsizetype at = 0; at < data.size(); at += 255) {
      const int n = int(std::min<qsizetype>(255, data.size() - at));
      out.append(char(n)); out.append(data.mid(at, n));
    }
    out.append(char(0));
  }
  out.append(char(0x3B));
  return out;
}

namespace {
bool alike(QRgb a, QRgb b) {
  return std::abs(qRed(a) - qRed(b)) < 8 && std::abs(qGreen(a) - qGreen(b)) < 8 && std::abs(qBlue(a) - qBlue(b)) < 8;
}
QString colour(QRgb c) { return QString("%1,%2,%3").arg(qRed(c)).arg(qGreen(c)).arg(qBlue(c)); }
bool writeFile(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
} // namespace

int checkImageFiles(QStringList &report) {
  int failed = 0;
  auto check = [&](bool ok, const QString &what) {
    report << (ok ? "PASS: " : "FAIL: ") + what;
    if (!ok) ++failed;
  };
  QTemporaryDir dir;
  if (!dir.isValid()) { report << "FAIL: cannot make a folder for the picture checks"; return 1; }
  const QRgb red = qRgb(255, 0, 0), green = qRgb(0, 255, 0), blue = qRgb(0, 0, 255), white = qRgb(255, 255, 255);

  // TGA, as it is stored: rows from the bottom, blue first.
  {
    QByteArray tga(18, 0);
    tga[2] = 2; tga[12] = 2; tga[14] = 2; tga[16] = 24;
    tga.append("\x00\xFF\x00\xFF\xFF\xFF", 6); // bottom: green, white
    tga.append("\x00\x00\xFF\xFF\x00\x00", 6); // top: red, blue
    const QString path = dir.path() + "/plain.tga";
    writeFile(path, tga);
    const QImage image = readImageFile(path);
    check(canReadImageFile(path) && image.size() == QSize(2, 2) && alike(image.pixel(0, 0), red) &&
              alike(image.pixel(1, 0), blue) && alike(image.pixel(0, 1), green) && alike(image.pixel(1, 1), white),
          "a Targa (TGA) picture reads the right way up, in its colours");
    // Packed (RLE), rows from the top, with alpha.
    QByteArray rle(18, 0);
    rle[2] = 10; rle[12] = 2; rle[14] = 1; rle[16] = 32; rle[17] = char(0x28);
    rle.append("\x81\xFF\x00\x00\xFF", 5); // two of the same: blue, opaque
    const QString packed = dir.path() + "/packed.tga";
    writeFile(packed, rle);
    const QImage two = readImageFile(packed);
    check(two.size() == QSize(2, 1) && alike(two.pixel(1, 0), blue) && qAlpha(two.pixel(1, 0)) == 255,
          "a packed (RLE) Targa picture with alpha reads");
  }
  // TIFF: Qt here has no reader for it; Windows has.
  {
    const int w = 8, h = 4;
    QByteArray tiff("II\x2A\x00\x08\x00\x00\x00", 8);
    auto u16 = [&](int v) { tiff.append(char(v & 255)); tiff.append(char((v >> 8) & 255)); };
    auto u32 = [&](quint32 v) { for (int k = 0; k < 4; ++k) tiff.append(char((v >> (8 * k)) & 255)); };
    auto entry = [&](int tag, int type, quint32 count, quint32 value) { u16(tag); u16(type); u32(count); u32(value); };
    const quint32 bitsAt = 8 + 2 + 9 * 12 + 4, pixelsAt = bitsAt + 6;
    u16(9);
    entry(256, 4, 1, w);
    entry(257, 4, 1, h);
    entry(258, 3, 3, bitsAt);
    entry(259, 3, 1, 1);
    entry(262, 3, 1, 2);
    entry(273, 4, 1, pixelsAt);
    entry(277, 3, 1, 3);
    entry(278, 4, 1, h);
    entry(279, 4, 1, w * h * 3);
    u32(0);
    u16(8); u16(8); u16(8);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const QRgb c = y >= 2 ? green : x < 4 ? red : blue;
        tiff.append(char(qRed(c))); tiff.append(char(qGreen(c))); tiff.append(char(qBlue(c)));
      }
    const QString path = dir.path() + "/picture.tif";
    writeFile(path, tiff);
    QString error;
    const QImage image = readImageFile(path, &error);
    check(canReadImageFile(path) && image.size() == QSize(w, h) && alike(image.pixel(1, 0), red) &&
              alike(image.pixel(6, 0), blue) && alike(image.pixel(3, 3), green),
          "a TIFF picture reads (" + (image.isNull() ? error : QString("%1x%2").arg(image.width()).arg(image.height())) + ")");
  }
  // Not a picture at all, named as one Windows reads only with an extension:
  // no picture, and a reason the person can act on.
  {
    const QString path = dir.path() + "/broken.heic";
    writeFile(path, "not a picture");
    QString error;
    const QImage image = readImageFile(path, &error);
    check(image.isNull() && !error.isEmpty() && !canReadImageFile(path) &&
              unreadablePictureReason("heic").contains("HEIF"),
          "a file that is no picture gives none, and says why");
  }
  // An animated GIF: its frames and their own times.
  {
    const QString path = dir.path() + "/moving.gif";
    writeFile(path, solidAnimatedGif(QSize(16, 8), {{red, 10}, {green, 20}, {blue, 30}}));
    const AnimatedImageInfo info = animatedImageInfo(path);
    check(info.frames == 3 && info.durationMs == 600 && info.size == QSize(16, 8),
          QString("an animated GIF has 3 frames over 600 ms (read %1 over %2 ms)").arg(info.frames).arg(info.durationMs));
    const QString quick = dir.path() + "/quick.gif";
    writeFile(quick, solidAnimatedGif(QSize(4, 4), {{red, 0}, {blue, 1}}));
    check(animatedImageInfo(quick).durationMs == 200, "a GIF frame given no time, or too short a one, shows for 100 ms");
    const QString still = dir.path() + "/still.gif";
    writeFile(still, solidAnimatedGif(QSize(4, 4), {{red, 10}}));
    check(animatedImageInfo(still).frames <= 1 && !readImageFile(still).isNull(), "a one-frame GIF stays a picture");
    AnimatedImageFootage footage;
    QString wrong;
    if (!footage.open(path)) wrong = " (does not open)";
    // Forward, round again past the end, and back.
    const std::vector<std::tuple<qint64, QRgb, qint64>> times{
        {50, red, 0}, {150, green, 100}, {299, green, 100}, {400, blue, 300}, {650, red, 600}, {5000, green, 4900},
        {20, red, 0}, {250, green, 100}};
    for (const auto &[ms, want, startMs] : times) {
      qint64 start = -1, length = -1;
      const QImage frame = footage.frame(ms * 10000, &start, &length);
      if (frame.isNull() || !alike(frame.pixel(8, 4), want) || start != startMs * 10000 || start + length <= ms * 10000)
        wrong += QString(" %1 ms: %2 from %3 ms").arg(ms).arg(frame.isNull() ? "nothing" : colour(frame.pixel(8, 4))).arg(start / 1e4);
    }
    std::vector<qint64> starts;
    footage.readOn(1000000, [&](qint64 time, const QImage &) {
      if (time >= 10000000) return false;
      starts.push_back(time);
      return true;
    });
    if (starts != std::vector<qint64>{1000000, 3000000, 6000000, 7000000, 9000000})
      wrong += " (read on from 100 ms is not its frames, round and round)";
    footage.setShrink(2);
    if (footage.frame(0, nullptr, nullptr).size() != QSize(8, 4) || footage.size() != QSize(16, 8)) wrong += " (made smaller, wrong size)";
    check(wrong.isEmpty(), "an animated GIF gives each frame for its own time, over and over" + wrong);
  }
  return failed ? 1 : 0;
}
