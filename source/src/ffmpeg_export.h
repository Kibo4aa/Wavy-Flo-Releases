#pragma once
#include <QtWidgets>
#include <functional>

enum class FFmpegMovie { Animation, Png, ProRes4444 };
QString findExportFFmpeg();
void showFFmpegLicenses(QWidget *parent);
void checkExportFFmpeg(const QString &program, FFmpegMovie codec, bool audio,
                       const std::function<bool()> &canceled);
void encodeFFmpegMovie(const QString &program, FFmpegMovie codec,
                       const QString &sequenceDir, const QString &output,
                       int fps, int frames, bool alpha, bool audio,
                       const std::function<bool()> &canceled, int depth = 8);
// FFmpeg fed the frames down a pipe as they are rendered, so nothing but the
// movie itself is written to the drive. A PNG of every frame used to go to the
// disk first: gigabytes written, read once and thrown away.
class FFmpegPipe {
public:
  // `audio` is a finished WAV file, or empty for none.
  FFmpegPipe(const QString &program, FFmpegMovie codec, const QString &audio, const QString &output,
             int width, int height, int fps, int frames, bool alpha, int depth = 8);
  ~FFmpegPipe(); // stops FFmpeg if it was not finished
  void write(const QImage &frame, const std::function<bool()> &canceled);
  // Ends the input and waits for the movie. Throws when FFmpeg failed.
  void finish(const std::function<bool()> &canceled);
private:
  void drain();
  QProcess process;
  QByteArray log;
  QString output;
  int width, height, depth;
};
int checkFFmpegExport(QStringList &failures);
