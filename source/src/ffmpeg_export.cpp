#include "ffmpeg_export.h"
#include <stdexcept>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
void fail(const QString &message) { throw std::runtime_error(message.toUtf8().constData()); }

// Execute a program with separate arguments, never through cmd.exe or a shell.
// Retain only a bounded diagnostic tail; poll so Cancel remains responsive.
QString run(const QString &program, const QStringList &arguments,
            const std::function<bool()> &canceled, int timeoutMs = 0) {
  QProcess process;
  process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
  process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
    args->flags |= CREATE_NO_WINDOW;
  });
#endif
  process.start(program, arguments);
  QElapsedTimer elapsed; elapsed.start(); QByteArray output;
  auto drain = [&] {
    output += process.readAll();
    if (output.size() > 256 * 1024) output = output.right(256 * 1024);
  };
  while (process.state() != QProcess::NotRunning) {
    process.waitForFinished(40); drain();
    if (canceled()) {
      process.kill(); process.waitForFinished(3000);
      fail("Canceled");
    }
    if (timeoutMs && elapsed.elapsed() > timeoutMs) {
      process.kill(); process.waitForFinished(3000);
      fail("FFmpeg did not respond in time.");
    }
  }
  drain();
  if (process.error() == QProcess::FailedToStart)
    fail("Cannot start FFmpeg: " + process.errorString());
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    fail("FFmpeg failed:\n" + QString::fromUtf8(output).right(12000));
  return QString::fromUtf8(output);
}

bool reportsLgpl(const QString &license, const QString &version) {
  auto all = license + '\n' + version;
  return license.contains("GNU Lesser General Public License", Qt::CaseInsensitive) &&
         !all.contains("nonfree", Qt::CaseInsensitive) &&
         !all.contains("not legally redistributable", Qt::CaseInsensitive) &&
         !all.contains("GNU General Public License", Qt::CaseInsensitive) &&
         !QRegularExpression("--enable-gpl(?:[=\\s'\"]|$)").match(all).hasMatch();
}

QString encoder(FFmpegMovie codec) {
  switch (codec) {
  case FFmpegMovie::Animation: return "qtrle";
  case FFmpegMovie::Png: return "png";
  case FFmpegMovie::ProRes4444: return "prores_ks";
  }
  return {};
}

bool hasEncoder(const QString &listing, const QString &name) {
  return QRegularExpression("^\\s*[VAS][A-Z.]{5}\\s+" +
       QRegularExpression::escape(name) + "\\s", QRegularExpression::MultilineOption)
      .match(listing).hasMatch();
}

QStringList outputArguments(QStringList args, FFmpegMovie codec, const QString &output, int fps, int frames,
                            bool alpha, bool audio, int depth);
QStringList movieArguments(FFmpegMovie codec, const QString &directory,
                           const QString &output, int fps, int frames,
                           bool alpha, bool audio, int depth = 8) {
  QStringList args{"-hide_banner", "-nostdin", "-loglevel", "error", "-y",
                   "-framerate", QString::number(fps), "-start_number", "0",
                   "-i", directory + "/frame-%06d.png"};
  if (audio) args << "-i" << directory + "/audio.wav";
  return outputArguments(args, codec, output, fps, frames, alpha, audio, depth);
}
// The same movie from raw frames on standard input: straight (not
// premultiplied) RGBA, 8 or 16 bits a channel -- the very pixels the PNG files
// held, so the movie comes out the same.
QStringList pipeArguments(FFmpegMovie codec, const QString &audio, const QString &output, int width, int height,
                          int fps, int frames, bool alpha, int depth = 8) {
  QStringList args{"-hide_banner", "-loglevel", "error", "-y",
                   "-f", "rawvideo", "-pix_fmt", depth > 8 ? "rgba64le" : "rgba",
                   "-s", QString("%1x%2").arg(width).arg(height), "-framerate", QString::number(fps),
                   "-i", "-"};
  if (!audio.isEmpty()) args << "-i" << audio;
  return outputArguments(args, codec, output, fps, frames, alpha, !audio.isEmpty(), depth);
}
QStringList outputArguments(QStringList args, FFmpegMovie codec, const QString &output, int fps, int frames,
                            bool alpha, bool audio, int depth) {
  args << "-map" << "0:v:0";
  if (audio) args << "-map" << "1:a:0" << "-c:a" << "pcm_s16le";
  else args << "-an";
  args << "-c:v" << encoder(codec);
  // Explicit conversion is necessary: '+' disables automatic conversions.
  QString pixels = codec == FFmpegMovie::Animation ? (alpha ? "argb" : "rgb24") :
                   codec == FFmpegMovie::Png ? (depth>8 ? (alpha?"rgba64be":"rgb48be") : (alpha ? "rgba" : "rgb24")) :
                   (alpha ? "yuva444p10le" : "yuv444p10le");
  // ProRes is video: made with the Rec. 709 matrix and tagged so, as the MP4
  // is. Left to itself the scaler used Rec. 601 and said nothing, and players
  // read an HD movie as Rec. 709: reds and greens shifted. Animation and PNG
  // keep the RGB pixels as they are.
  const bool video = codec == FFmpegMovie::ProRes4444;
  args << "-vf" << (video ? "scale=iw:ih:out_color_matrix=bt709:out_range=tv,setsar=1,format=" : "scale=iw:ih,setsar=1,format=") + pixels
       << "-pix_fmt" << "+" + pixels;
  if (video)
    args << "-color_primaries" << "bt709" << "-color_trc" << "bt709" << "-colorspace" << "bt709" << "-color_range" << "tv";
  if (codec == FFmpegMovie::ProRes4444)
    args << "-profile:v" << "4" << "-alpha_bits" << (alpha ? "16" : "0");
  args << "-frames:v" << QString::number(frames)
       << "-t" << QString::number(double(frames) / fps, 'f', 9)
       << "-f" << "mov" << output;
  return args;
}
}

QString findExportFFmpeg() {
  QString saved = QSettings().value("render/ffmpegPath").toString();
  if (QFileInfo(saved).isFile()) return QFileInfo(saved).absoluteFilePath();
  auto base = QCoreApplication::applicationDirPath();
  for (const auto &relative : {"/tools/ffmpeg/bin/ffmpeg.exe", "/ffmpeg.exe"}) {
    if (QFileInfo(base + relative).isFile()) return base + relative;
  }
  return QStandardPaths::findExecutable("ffmpeg");
}

void showFFmpegLicenses(QWidget *parent) {
  QDialog dialog(parent); dialog.setWindowTitle("FFmpeg — licenses and distribution");
  QVBoxLayout layout(&dialog); QTextBrowser text; text.setOpenExternalLinks(true);
  QFile document(":/third_party/FFMPEG.md");
  if (document.open(QIODevice::ReadOnly)) text.setMarkdown(QString::fromUtf8(document.readAll()));
  else text.setPlainText("Optional FFmpeg CLI export. FFmpeg is third-party software. "
                         "See https://ffmpeg.org/legal.html for licensing requirements.");
  layout.addWidget(&text); QDialogButtonBox close(QDialogButtonBox::Close); layout.addWidget(&close);
  QObject::connect(&close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  dialog.resize(640, 480); dialog.exec();
}

void checkExportFFmpeg(const QString &program, FFmpegMovie codec, bool audio,
                       const std::function<bool()> &canceled) {
  if (!QFileInfo(program).isFile()) fail("Select an LGPL ffmpeg.exe in the render dialog first.");
  auto license = run(program, {"-L"}, canceled, 15000);
  auto version = run(program, {"-version"}, canceled, 15000);
  if (!reportsLgpl(license, version))
    fail("This exporter accepts only FFmpeg builds that report LGPL licensing. "
         "GPL, nonfree, and unrecognized builds are not accepted.\n"
         "Choose an LGPL build without --enable-gpl or --enable-nonfree.");
  auto encoders = run(program, {"-hide_banner", "-encoders"}, canceled, 15000);
  if (!hasEncoder(encoders, encoder(codec)) || (audio && !hasEncoder(encoders, "pcm_s16le")))
    fail("This FFmpeg build is missing the required " + encoder(codec) + " or PCM audio encoder.");
  auto muxers = run(program, {"-hide_banner", "-muxers"}, canceled, 15000);
  if (!QRegularExpression("^\\s*E\\s+mov\\s", QRegularExpression::MultilineOption).match(muxers).hasMatch())
    fail("This FFmpeg build does not contain the MOV muxer.");
}

void encodeFFmpegMovie(const QString &program, FFmpegMovie codec,
                       const QString &directory, const QString &output,
                       int fps, int frames, bool alpha, bool audio,
                       const std::function<bool()> &canceled, int depth) {
  if (fps <= 0 || frames <= 0) fail("Invalid movie duration or frame rate.");
  run(program, movieArguments(codec, directory, output, fps, frames, alpha, audio, depth), canceled);
  if (!QFileInfo(output).isFile() || QFileInfo(output).size() == 0) fail("FFmpeg produced no movie.");
}

FFmpegPipe::FFmpegPipe(const QString &program, FFmpegMovie codec, const QString &audio, const QString &output,
                       int width, int height, int fps, int frames, bool alpha, int depth)
    : output(output), width(width), height(height), depth(depth) {
  if (fps <= 0 || frames <= 0 || width <= 0 || height <= 0) fail("Invalid movie size, duration or frame rate.");
  process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
  process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
    args->flags |= CREATE_NO_WINDOW;
  });
#endif
  process.start(program, pipeArguments(codec, audio, output, width, height, fps, frames, alpha, depth));
  if (!process.waitForStarted(15000)) fail("Cannot start FFmpeg: " + process.errorString());
}
FFmpegPipe::~FFmpegPipe() {
  if (process.state() == QProcess::NotRunning) return;
  process.kill();
  process.waitForFinished(3000);
}
void FFmpegPipe::drain() {
  log += process.readAll();
  if (log.size() > 256 * 1024) log = log.right(256 * 1024);
}
void FFmpegPipe::write(const QImage &frame, const std::function<bool()> &canceled) {
  const QImage pixels = frame.convertToFormat(depth > 8 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
  if (pixels.width() != width || pixels.height() != height) fail("A frame came out the wrong size.");
  const qsizetype row = qsizetype(width) * (depth > 8 ? 8 : 4);
  for (int y = 0; y < height; ++y) {
    if (process.write(reinterpret_cast<const char *>(pixels.constScanLine(y)), row) != row)
      fail("FFmpeg stopped taking frames:\n" + QString::fromUtf8(log).right(12000));
  }
  // Waits while FFmpeg catches up, so frames never pile up in memory.
  while (process.bytesToWrite() > 0) {
    process.waitForBytesWritten(40);
    drain();
    if (canceled()) fail("Canceled");
    if (process.state() == QProcess::NotRunning) {
      drain();
      fail("FFmpeg failed:\n" + QString::fromUtf8(log).right(12000));
    }
  }
}
void FFmpegPipe::finish(const std::function<bool()> &canceled) {
  process.closeWriteChannel();
  while (process.state() != QProcess::NotRunning) {
    process.waitForFinished(40);
    drain();
    if (canceled()) fail("Canceled");
  }
  drain();
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    fail("FFmpeg failed:\n" + QString::fromUtf8(log).right(12000));
  if (!QFileInfo(output).isFile() || QFileInfo(output).size() == 0) fail("FFmpeg produced no movie.");
}

int checkFFmpegExport(QStringList &failures) {
  auto check = [&](bool ok, const QString &name) { if (!ok) failures << name; };
  {
    const auto piped = pipeArguments(FFmpegMovie::ProRes4444, "D:/a & b/audio.wav", "D:/out.mov", 1920, 1080, 30, 90, true, 16);
    check(piped.value(piped.indexOf("-pix_fmt") + 1) == "rgba64le" && piped.contains("1920x1080") &&
              piped.value(piped.indexOf("-i") + 1) == "-" && piped.contains("D:/a & b/audio.wav") &&
              piped.contains("1:a:0") && piped.last() == "D:/out.mov" && !piped.contains("-nostdin"),
          "frames go to FFmpeg down a pipe, with the soundtrack beside them");
    const auto plain = pipeArguments(FFmpegMovie::Animation, QString(), "o.mov", 640, 360, 24, 1, false);
    check(plain.value(plain.indexOf("-pix_fmt") + 1) == "rgba" && plain.contains("-an"), "a silent 8-bit pipe");
  }
  {
    // The movie made down the pipe is the movie made from PNG files, frame for
    // frame, decoded and hashed by FFmpeg itself. Only where an FFmpeg is found.
    QString program = findExportFFmpeg();
    if (!QFileInfo(program).isFile()) program = QCoreApplication::applicationDirPath() + "/../tools/ffmpeg/bin/ffmpeg.exe";
    if (QFileInfo(program).isFile()) {
      auto never = [] { return false; };
      for (int depth : {8, 16})
        for (auto codec : {FFmpegMovie::Png, FFmpegMovie::ProRes4444, FFmpegMovie::Animation}) {
          if (codec == FFmpegMovie::Animation && depth > 8) continue;
          QTemporaryDir folder;
          QDir().mkdir(folder.path() + "/sequence");
          const int width = 96, height = 54, frames = 3;
          std::vector<QImage> pictures;
          for (int f = 0; f < frames; ++f) {
            QImage picture(width, height, depth > 8 ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA8888_Premultiplied);
            picture.fill(Qt::transparent);
            QPainter painter(&picture);
            painter.setRenderHint(QPainter::Antialiasing);
            QLinearGradient gradient(0, 0, width, height);
            gradient.setColorAt(0, QColor(255, 40, f * 60, 200));
            gradient.setColorAt(1, QColor(20, 200, 255, 60));
            painter.setBrush(gradient);
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(QRectF(4 + f * 5, 3, width - 20, height - 8));
            painter.end();
            pictures.push_back(picture);
            QImage png = picture; // as the PNG sequence export writes them
            png.setColorSpace(QColorSpace(QColorSpace::SRgb));
            png.save(folder.path() + QString("/sequence/frame-%1.png").arg(f, 6, 10, QChar('0')));
          }
          const QString name = QString("%1 %2-bit").arg(encoder(codec)).arg(depth);
          try {
            const QString fromFiles = folder.path() + "/files.mov", piped = folder.path() + "/piped.mov";
            encodeFFmpegMovie(program, codec, folder.path() + "/sequence", fromFiles, 30, frames, true, false, never, depth);
            FFmpegPipe pipe(program, codec, QString(), piped, width, height, 30, frames, true, depth);
            for (const auto &picture : pictures) pipe.write(picture, never);
            pipe.finish(never);
            auto hashes = [&](const QString &movie) {
              return run(program, {"-hide_banner", "-loglevel", "error", "-i", movie, "-map", "0:v", "-f", "framemd5", "-"}, never, 60000);
            };
            const QString a = hashes(fromFiles), b = hashes(piped);
            if (!(a.count(QChar(10)) >= frames && a == b)) {
              const QStringList left = a.split(QChar(10)), right = b.split(QChar(10));
              int at = 0;
              while (at < left.size() && at < right.size() && left[at] == right[at]) ++at;
              failures << "a piped " + name + " MOV is the one made from PNG files: line " + QString::number(at) + " | files: " +
                              left.value(at) + " | piped: " + right.value(at);
            }
          } catch (const std::exception &e) {
            failures << "piped " + name + " MOV: " + QString::fromUtf8(e.what());
          }
        }
    }
  }
  const QString lgpl = "GNU Lesser General Public License version 2.1 or later";
  check(reportsLgpl(lgpl, "configuration: --enable-shared"), "accept LGPL");
  check(!reportsLgpl(lgpl, "configuration: --enable-gpl"), "reject GPL configuration");
  check(!reportsLgpl(lgpl, "configuration: --enable-nonfree"), "reject nonfree configuration");
  check(!reportsLgpl("GNU General Public License", ""), "reject GPL license");
  check(!reportsLgpl("unknown", ""), "reject unknown license");
  check(hasEncoder(" V..... qtrle QuickTime Animation\n A....D pcm_s16le PCM\n", "qtrle"), "find encoder");
  check(!hasEncoder(" V..... qtrle_other other\n", "qtrle"), "exact encoder match");
  for (auto codec : {FFmpegMovie::Animation, FFmpegMovie::Png, FFmpegMovie::ProRes4444}) {
    auto args = movieArguments(codec, "D:/space & test/日本語", "D:/out & test.mov", 30, 180, true, true);
    check(args.contains("D:/space & test/日本語/frame-%06d.png") && args.last() == "D:/out & test.mov", "literal paths");
    check(args.contains("1:a:0") && args.contains("pcm_s16le"), "map audio");
    check(args.value(args.indexOf("-frames:v") + 1) == "180", "frame count");
    check(args.value(args.indexOf("-t") + 1) == "6.000000000", "composition duration");
    check(args.value(args.indexOf("-pix_fmt") + 1).startsWith('+'), "require pixel format");
  }
  {
    const auto prores = movieArguments(FFmpegMovie::ProRes4444, "x", "y", 24, 1, false, false);
    const auto animation = movieArguments(FFmpegMovie::Animation, "x", "y", 24, 1, false, false);
    check(prores.value(prores.indexOf("-vf") + 1).contains("out_color_matrix=bt709") &&
              prores.value(prores.indexOf("-colorspace") + 1) == "bt709" &&
              prores.value(prores.indexOf("-color_range") + 1) == "tv",
          "ProRes is made and tagged Rec. 709");
    check(!animation.contains("-colorspace") && !animation.value(animation.indexOf("-vf") + 1).contains("color_matrix"),
          "RGB codecs keep their pixels");
  }
  auto silent = movieArguments(FFmpegMovie::Animation, "x", "y", 24, 1, false, false);
  check(silent.contains("-an") && !silent.contains("1:a:0") && silent.contains("+rgb24"), "opaque silent movie");
  return failures.isEmpty() ? 0 : 1;
}
