#pragma once
#include <QtGui>
#include <memory>
#include <functional>
class NativeMedia {
  struct State;
  std::unique_ptr<State> state;
public:
  NativeMedia();
  ~NativeMedia();
  QImage videoFrame(const QString &path, qint64 time100ns);
  // The same, and when the picture returned starts and how long it stays on
  // screen (100 ns): what blending it with the next one needs. A request for a
  // time a little after the last reads on instead of seeking.
  QImage videoFrame(const QString &path, qint64 time100ns, qint64 *start100ns, qint64 *length100ns);
  // Every picture from `from100ns` on, in order, seeking once: what following
  // something through footage needs, where videoFrame would seek every time.
  // `picture` gets each one with its start time (100 ns); returning false
  // stops. False if the file has no video the reader can turn into RGB.
  // Whether the last video opened decodes on the graphics card.
  bool decodesOnCard() const;
  // Pictures made 1/2 or 1/4 the footage's size, for a viewer that shows it no
  // larger: made smaller on the card, they are read back a quarter or a
  // sixteenth the size. 1 is whole. Where only the processor decodes, whole.
  void setShrink(int divisor);
  // The footage's own size as shown (its display aperture), whatever size the
  // pictures are made at; empty before any is open.
  QSize footageSize() const;
  // Whether this system's decoders can turn `path` into pictures at all
  // (Windows has none for ProRes, nor for HEVC without its extension).
  bool opensVideo(const QString &path);
  // Whether the last video opened is read through FFmpeg, Windows having no
  // decoder for it.
  bool decodesThroughFFmpeg() const;
  bool readVideo(const QString &path, qint64 from100ns,
                 const std::function<bool(qint64 time100ns, const QImage &)> &picture);
  // Decode interleaved stereo PCM at 48 kHz into a caller-owned temporary file.
  bool decodeAudio(const QString &path, QIODevice &pcm, const std::function<bool()> &cancel);
  void beginMovie(const QString &path, int width, int height, int fps, int bitrate, bool audio);
  void writeFrame(const QImage &image, int frame, int fps);
  void writeAudio(const QByteArray &pcm, qint64 firstSample);
  void finishMovie();
private:
  QImage videoFrameAt(const QString &path, qint64 time100ns, qint64 *start100ns, qint64 *length100ns);
};
int checkNativeMedia(QStringList &report);
