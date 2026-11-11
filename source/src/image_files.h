#pragma once
// Pictures from files: whatever Qt reads, and on Windows whatever Windows
// itself reads through WIC (TIFF, ICO, DDS, JPEG XR out of the box; WebP,
// HEIC, AVIF and camera RAW where their Windows extensions are installed).
// And animated pictures (GIF, and WebP through Windows): their frames as
// footage.
#include <QImage>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <utility>
#include <functional>
#include <memory>
#include <vector>

// The picture in `path`, turned upright by its orientation tag. Null when
// neither Qt nor Windows can read it; `error` then says why.
QImage readImageFile(const QString &path, QString *error = nullptr);
// Whether readImageFile can read `path`, without decoding all of it.
bool canReadImageFile(const QString &path);
// The extensions offered as pictures in Import (lower case, no dot).
QStringList importImageSuffixes();
// And as footage, and as sound: what Windows' readers or FFmpeg open.
QStringList importVideoSuffixes();
QStringList importAudioSuffixes();
// Why a picture of this kind cannot be read here, for the person: for the
// kinds Windows reads only with an extension, which one to install.
QString unreadablePictureReason(const QString &suffix);

// An animated picture: more than one frame. Its frames' times come from the
// file; a frame given no time (or under 20 ms), as browsers do, shows for
// 100 ms.
struct AnimatedImageInfo {
  int frames = 0;
  qint64 durationMs = 0; // one pass through every frame
  QSize size;
};
// Its frames and length when `path` is an animated picture; frames <= 1 when
// it is a still one (or cannot be read).
AnimatedImageInfo animatedImageInfo(const QString &path);
// Whether a video layer's file is an animated picture, by its name alone:
// what the editor asks every time it picks a layer for the sound player.
bool isAnimatedImageName(const QString &path);

// An animated picture's frames, served by time like footage (100 ns units, as
// NativeMedia's). Frames are kept once decoded, up to a budget; past it they
// are decoded again from the start when the time goes back.
class AnimatedImageFootage {
public:
  AnimatedImageFootage();
  ~AnimatedImageFootage();
  bool open(const QString &path);
  QSize size() const;
  void setShrink(int divisor);
  // The frame on screen at `time`, and when it starts and how long it lasts.
  // It plays over and over: a time past the end is a later pass through it.
  QImage frame(qint64 time, qint64 *start, qint64 *length);
  // Every frame from `from` on, in order, pass after pass; `picture`
  // returning false stops.
  bool readOn(qint64 from, const std::function<bool(qint64, const QImage &)> &picture);
private:
  struct State;
  std::unique_ptr<State> state;
};

// For the checks: a GIF of solid frames, each colour shown for its own time
// (hundredths of a second, as GIF counts them).
QByteArray solidAnimatedGif(QSize size, const std::vector<std::pair<QRgb, int>> &frames);
// Pictures of the kinds Qt here cannot read, and GIF frames by time.
int checkImageFiles(QStringList &report);
