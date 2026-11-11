#include "video_frames.h"
#include "native_media.h"
#include "remap_audio.h"
#include "render_export.h"
#include <QBuffer>
#include "video_timing.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>

namespace {
// Footage that says which picture it is: its number in eight black-or-white
// blocks along the top, and a bar lower down that steps right each picture, so
// a mix of two pictures shows as two bars at half strength.
const QSize kSize(320, 180);
QImage numbered(int k) {
  QImage image(kSize, QImage::Format_RGB32);
  image.fill(Qt::black);
  for (int bit = 0; bit < 8; ++bit)
    if (k & (1 << bit))
      for (int y = 0; y < 30; ++y)
        for (int x = bit * 40; x < bit * 40 + 40; ++x) image.setPixel(x, y, qRgb(255, 255, 255));
  for (int y = 80; y < 170; ++y)
    for (int x = 20 + 4 * k; x < 24 + 4 * k && x < kSize.width(); ++x) image.setPixel(x, y, qRgb(255, 255, 255));
  return image;
}
int numberIn(const QImage &picture) {
  if (picture.isNull()) return -1;
  const QImage image = picture.convertToFormat(QImage::Format_RGB32);
  int k = 0;
  for (int bit = 0; bit < 8; ++bit) {
    double sum = 0;
    for (int y = 8; y < 22; ++y)
      for (int x = bit * 40 + 10; x < bit * 40 + 30; ++x) sum += qGray(image.pixel(x, y));
    if (sum / (14 * 20) > 128) k |= 1 << bit;
  }
  return k;
}
// How bright picture k's bar is in this picture, 0..255.
double barOf(const QImage &picture, int k) {
  const QImage image = picture.convertToFormat(QImage::Format_RGB32);
  double sum = 0;
  int n = 0;
  for (int y = 100; y < 150; ++y) { sum += qGray(image.pixel(21 + 4 * k, y)); ++n; }
  return sum / n;
}
}  // namespace

// --time-remap-self-test
int checkVideoFrames(QStringList &report) {
  int failures = 0;
  auto check = [&](bool passed, const QString &label) {
    report << label + (passed ? ": PASS" : ": FAIL");
    failures += !passed;
  };
  QTemporaryDir dir;
  const QString path = dir.path() + "/numbered.mp4";
  const int count = 60, fps = 30;
  try {
    NativeMedia media;
    media.beginMovie(path, kSize.width(), kSize.height(), fps, 8000000, false);
    for (int k = 0; k < count; ++k) media.writeFrame(numbered(k), k, fps);
    media.finishMovie();
  } catch (const std::exception &e) {
    report << QString("could not write test footage: %1").arg(e.what());
  }
  check(QFileInfo(path).size() > 1000, "numbered test footage written");
  auto at = [](int k) { return qint64(std::llround(k * 1e7 / 30)); };
  {
    // In order -- reading on -- and at random -- seeking -- every picture is the one asked for.
    NativeMedia media;
    bool inOrder = true, timed = true;
    QElapsedTimer clock; clock.start();
    for (int k = 0; k < count; ++k) {
      qint64 start = -1, length = -1;
      const QImage picture = media.videoFrame(path, at(k), &start, &length);
      inOrder &= numberIn(picture) == k;
      timed &= std::llabs(start - at(k)) < 20000 && std::llabs(length - 333333) < 20000;
    }
    const double sequential = clock.nsecsElapsed() / 1e6 / count;
    bool random = true;
    QRandomGenerator pick(3);
    clock.restart();
    for (int i = 0; i < 30; ++i) {
      const int k = int(pick.bounded(count));
      NativeMedia fresh; // a reader that has read nothing: a seek every time, as before
      random &= numberIn(fresh.videoFrame(path, at(k) + 1000)) == k;
    }
    const double seeking = clock.nsecsElapsed() / 1e6 / 30;
    bool mixed = true;
    for (int k : {40, 12, 13, 13, 50, 49, 49, 0, 59, 58})
      mixed &= numberIn(media.videoFrame(path, at(k) + 50000)) == k;
    check(inOrder && timed && random && mixed,
          QString("every picture is the one asked for, read on in order (%1 ms each), sought at random (%2 ms each), and back and forth")
              .arg(sequential, 0, 'f', 2).arg(seeking, 0, 'f', 2));
  }
  {
    // Where a layer's clock points in its footage.
    Layer plain;
    plain.type = "video"; plain.path = path; plain.duration = 90; plain.sourceOffsetMs = 500; plain.sourceDurationMs = 2000;
    check(std::abs(videoSourceSeconds(plain, 15, fps) - 1.0) < 1e-9 && videoTiming(plain, 44, fps).positionMs == 1966 &&
              !videoTiming(plain, 45, fps).active,
          "without Time Remap the footage runs on from the in-point and ends where it ends");
    Layer remapped = plain;
    remapped.timeRemap = true;
    remapped.p[TimeRemap].set(0, 1.5); remapped.p[TimeRemap].set(60, 0.5);
    check(std::abs(videoSourceSeconds(remapped, 30, fps) - 1.0) < 1e-9 && videoTiming(remapped, 80, fps).active &&
              videoTiming(remapped, 80, fps).positionMs == 500,
          "with Time Remap the keys say where, and past them the last one holds");
    remapped.p[TimeRemap].keys.clear(); remapped.p[TimeRemap].base = 7;
    check(videoTiming(remapped, 10, fps).positionMs == 1999 && layerFootageTime(remapped, 10, fps) == 19990000,
          "a remapped time past the footage's end holds its last picture");
  }
  {
    // Slowed, reversed, frozen: what export draws.
    NativeMedia media;
    Layer l;
    l.type = "video"; l.path = path; l.duration = 120; l.sourceDurationMs = 2000; l.timeRemap = true;
    l.p[TimeRemap].set(0, 0); l.p[TimeRemap].set(118, 118 / 60.); // half speed
    bool slow = true;
    for (int f = 0; f < 118; ++f) slow &= numberIn(footageFrame(media, path, layerFootageTime(l, f, fps), false)) == f / 2;
    l.p[TimeRemap].keys.clear();
    l.p[TimeRemap].set(0, 59 / 30.); l.p[TimeRemap].set(59, 0); // backwards
    bool back = true;
    for (int f = 0; f < 60; ++f) back &= numberIn(footageFrame(media, path, layerFootageTime(l, f, fps), false)) == 59 - f;
    l.p[TimeRemap].keys.clear(); l.p[TimeRemap].base = 20 / 30. + .001; // held
    bool held = true;
    for (int f = 0; f < 30; f += 7) held &= numberIn(footageFrame(media, path, layerFootageTime(l, f, fps), false)) == 20;
    check(slow && back && held, "half speed shows each picture twice, reversed runs 59 down to 0, a freeze holds one picture");
    // Frame Mix: halfway between two pictures, half of each.
    l.p[TimeRemap].keys.clear();
    l.p[TimeRemap].set(0, 0); l.p[TimeRemap].set(118, 118 / 60.);
    const QImage between = footageFrame(media, path, layerFootageTime(l, 21, fps), true); // 10.5 pictures in
    const QImage on = footageFrame(media, path, layerFootageTime(l, 20, fps), true);      // exactly picture 10
    const double b10 = barOf(between, 10), b11 = barOf(between, 11), o10 = barOf(on, 10), o11 = barOf(on, 11);
    check(std::abs(b10 - 128) < 35 && std::abs(b11 - 128) < 35 && o10 > 200 && o11 < 40,
          QString("Frame Mix halfway between two pictures shows half of each (%1, %2); on a picture, just that one (%3, %4)")
              .arg(b10, 0, 'f', 0).arg(b11, 0, 'f', 0).arg(o10, 0, 'f', 0).arg(o11, 0, 'f', 0));
    // Every mix between two pictures, including the one in three whose next
    // picture starts a step late (33 = 11000000, not 10666666 + 333333).
    int unmixed = 0;
    QStringList misses;
    for (int k = 0; k + 1 < count; ++k) {
      const qint64 t = qint64(std::llround((k + .7) * 1e7 / 30));
      const QImage mix = footageFrame(media, path, t, true);
      const double a = barOf(mix, k), b = barOf(mix, k + 1);
      if (!(std::abs(a - .3 * 255) < 35 && std::abs(b - .7 * 255) < 35)) { ++unmixed; if (misses.size() < 5) misses << QString::number(k); }
    }
    check(unmixed == 0, QString("30% of each picture and 70% of the next, between every pair (%1 not mixed%2)")
                            .arg(unmixed).arg(misses.isEmpty() ? QString() : ": after " + misses.join(", ")));
  }
  {
    // The sound follows the picture: a ramp of samples, so where each output
    // sample came from can be read straight off it.
    std::vector<int16_t> pcm(size_t(96000) * 2);
    for (qint64 i = 0; i < 96000; ++i) { pcm[size_t(i) * 2] = int16_t(i % 30000); pcm[size_t(i) * 2 + 1] = int16_t(-(i % 30000)); }
    Layer l;
    l.type = "video"; l.duration = 120; l.sourceDurationMs = 2000; l.timeRemap = true;
    auto sound = [&](qint64 first, qint64 count) {
      std::vector<qint64> sum(size_t(count) * 2, 0);
      addRemappedSound(pcm.data(), 96000, l, 30, first, count, sum.data());
      return sum;
    };
    l.p[TimeRemap].set(0, 0); l.p[TimeRemap].set(60, 1.); // two seconds of Main for one of footage
    const auto slow = sound(24000, 480); // half a second in: a quarter second of footage
    bool half = std::abs(slow[0] - 12000) <= 1 && std::abs(slow[1] + 12000) <= 1;
    for (size_t k = 1; k < 480; ++k) half &= std::abs(double(slow[k * 2]) - (12000 + k * .5)) <= 1;
    l.p[TimeRemap].keys.clear();
    l.p[TimeRemap].set(0, 2.); l.p[TimeRemap].set(60, 0.); // backwards, at speed
    const auto back = sound(24000, 480); // footage at 1.5 s, going down
    bool reversed = std::abs(back[0] - 72000 % 30000) <= 1;
    for (size_t k = 1; k < 480; ++k) reversed &= back[k * 2] == back[(k - 1) * 2] - 1;
    l.p[TimeRemap].keys.clear();
    l.p[TimeRemap].base = .5; // held
    const auto held = sound(24000, 480);
    const bool silent = std::all_of(held.begin(), held.end(), [](qint64 v) { return v == 0; });
    check(half && reversed && silent, "the sound follows the remap: half speed plays half as fast, backwards backwards, held is silent");
    // The preview's stream makes the same samples, from where playback starts.
    l.p[TimeRemap].base = 0; l.p[TimeRemap].set(0, 0); l.p[TimeRemap].set(60, 1.);
    RemapSound stream;
    stream.open(QIODevice::ReadOnly);
    stream.setPcm(pcm);
    stream.begin(l, 30, 120, 15);
    const QByteArray bytes = stream.read(480 * 4);
    const auto *made = reinterpret_cast<const int16_t *>(bytes.constData());
    bool same = bytes.size() == 480 * 4;
    for (int k = 0; same && k < 480 * 2; ++k) same = made[k] == int16_t(std::clamp<qint64>(slow[size_t(k)], -32768, 32767));
    check(same, "the preview plays exactly the samples export writes");
  }
  {
    // End to end, through export: a clip whose tone grows louder over its two
    // seconds, slowed to half speed over four. The exported sound must keep
    // growing for all four -- half as fast -- at half the pitch; before, it
    // played at full speed and stopped at two.
    QTemporaryDir out;
    const QString clip = out.path() + "/louder.mp4";
    bool made = false;
    try {
      NativeMedia media;
      media.beginMovie(clip, kSize.width(), kSize.height(), 30, 4000000, true);
      for (int f = 0; f < 60; ++f) {
        media.writeFrame(numbered(f), f, 30);
        const qint64 first = qint64(f) * 48000 / 30, end = qint64(f + 1) * 48000 / 30;
        QByteArray pcm(int((end - first) * 4), 0);
        auto *s16 = reinterpret_cast<int16_t *>(pcm.data());
        for (qint64 i = first; i < end; ++i) {
          const double t = i / 48000.;
          s16[(i - first) * 2] = s16[(i - first) * 2 + 1] = int16_t(20000 * (t / 2) * std::sin(2 * 3.14159265358979 * 440 * t));
        }
        media.writeAudio(pcm, first);
      }
      media.finishMovie();
      made = true;
    } catch (const std::exception &e) {
      report << QString("could not write the sounding clip: %1").arg(e.what());
    }
    Scene scene;
    scene.width = kSize.width(); scene.height = kSize.height(); scene.fps = 30; scene.frames = 120;
    Layer v;
    v.type = "video"; v.path = clip; v.duration = 120; v.sourceDurationMs = 2000; v.timeRemap = true;
    v.p[0].base = kSize.width() / 2.; v.p[1].base = kSize.height() / 2.;
    v.p[TimeRemap].set(0, 0); v.p[TimeRemap].set(119, 119 / 60.);
    scene.layers = {v};
    RenderSettings settings;
    settings.format = 0; settings.includeAudio = true; settings.bitrateMbps = 4;
    const QString movie = out.path() + "/slowed.mp4";
    const QString error = made ? renderToFile(scene, settings, movie, [](Scene &s) { return renderCPU(s, s.frame, false); }, {}) : QString("no clip");
    QByteArray sound;
    try {
      NativeMedia media;
      QBuffer buffer;
      buffer.open(QIODevice::ReadWrite);
      if (error.isEmpty() && media.decodeAudio(movie, buffer, [] { return false; })) sound = buffer.data();
    } catch (...) {}
    const auto *pcm = reinterpret_cast<const int16_t *>(sound.constData());
    const qint64 frames = sound.size() / 4;
    // Loudness and pitch over a quarter second at a time.
    auto at = [&](double seconds, double *hertz) {
      const qint64 from = qint64(seconds * 48000), to = std::min(frames, from + 12000);
      double energy = 0; int crossings = 0;
      for (qint64 i = from; i < to; ++i) {
        energy += double(pcm[i * 2]) * pcm[i * 2];
        if (i > from && (pcm[i * 2] >= 0) != (pcm[(i - 1) * 2] >= 0)) ++crossings;
      }
      if (hertz) *hertz = to > from ? crossings / 2. / ((to - from) / 48000.) : 0;
      return to > from ? std::sqrt(energy / (to - from)) : 0.;
    };
    double hz = 0;
    const double one = at(1.0, nullptr), three = at(3.0, &hz), late = at(3.7, nullptr);
    // At T seconds of the Main the footage is at T/2: the tone's loudness then is 20000 * T/4.
    const double expectOne = 20000 * .25 / std::sqrt(2.), expectThree = 20000 * .75 / std::sqrt(2.);
    check(error.isEmpty() && frames > 4 * 48000 - 4800 && std::abs(one / expectOne - 1) < .2 && std::abs(three / expectThree - 1) < .2 &&
              late > three && std::abs(hz - 220) < 15,
          QString("exported, a clip slowed to half speed sounds for all four seconds, growing as its picture does, an octave down "
                  "(loudness %1 at 1 s, %2 at 3 s, expected about %3 and %4; %5 Hz)%6")
              .arg(one, 0, 'f', 0).arg(three, 0, 'f', 0).arg(expectOne, 0, 'f', 0).arg(expectThree, 0, 'f', 0).arg(hz, 0, 'f', 0)
              .arg(error.isEmpty() ? QString() : " -- " + error));
  }
  {
    // The preview's server: asked, it answers later; asked again, from memory.
    int calls = 0;
    VideoFrameServer server([&] { ++calls; });
    auto waitFor = [&](const VideoFrameServer::Want &want) {
      QElapsedTimer t; t.start();
      bool exact = false;
      QImage image;
      while (t.elapsed() < 5000) {
        image = server.frame(want, &exact);
        if (exact) break;
        QCoreApplication::processEvents();
        QThread::msleep(2);
      }
      return exact ? image : QImage();
    };
    bool right = true;
    QElapsedTimer clock; clock.start();
    for (int k = 10; k < 40; ++k) right &= numberIn(waitFor({path, at(k), false})) == k;
    const double forward = clock.nsecsElapsed() / 1e6 / 30;
    for (int k = 55; k > 45; --k) right &= numberIn(waitFor({path, at(k), false})) == k;
    const quint64 before = server.remembered();
    bool exact = false;
    const int again = numberIn(server.frame({path, at(20), false}, &exact));
    // Read-ahead: asked for later, there when wanted.
    std::vector<VideoFrameServer::Want> next;
    for (int k = 0; k < 8; ++k) next.push_back({path, at(k), false});
    server.ahead(next);
    QElapsedTimer t; t.start();
    while (t.elapsed() < 5000) {
      bool all = true;
      for (const auto &w : next) { bool e = false; server.frame(w, &e); all &= e; if (!e) break; }
      if (all) break;
      QCoreApplication::processEvents();
      QThread::msleep(5);
    }
    bool readAhead = true;
    for (const auto &w : next) { bool e = false; readAhead &= numberIn(server.frame(w, &e)) == int(std::lround(w.time * 30. / 1e7)) && e; }
    QCoreApplication::processEvents();
    check(right && again == 20 && exact && server.remembered() > before && readAhead && calls > 0,
          QString("the preview's frame server answers every picture (%1 ms each going forward), remembers them, and reads ahead"
                  " [right %2, again %3 exact %4, remembered %5>%6, ahead %7, ready calls %8; decoding %9 ms a picture]")
              .arg(forward, 0, 'f', 2).arg(right).arg(again).arg(exact).arg(server.remembered()).arg(before).arg(readAhead).arg(calls)
              .arg(server.decodingMs() / std::max<quint64>(1, server.made()), 0, 'f', 2));
  }
  {
    // A video layer drawn from a smaller picture keeps the footage's size: in
    // a small viewer it looks as it does drawn from the whole picture.
    QImage whole(640, 360, QImage::Format_RGB32);
    for (int y = 0; y < 360; ++y)
      for (int x = 0; x < 640; ++x) whole.setPixel(x, y, qRgb(x * 255 / 639, y * 255 / 359, (x / 40 + y / 40) % 2 ? 230 : 30));
    Scene viewer;
    viewer.width = 1280; viewer.height = 720; viewer.fps = 30; viewer.frames = 10;
    Layer clip;
    clip.id = "clip"; clip.type = "video"; clip.duration = 10; clip.image = whole;
    clip.p[0].base = 500; clip.p[1].base = 300; clip.p[4].base = 10;
    viewer.layers = {clip};
    const QImage fromWhole = renderCPU(viewer, 0, false, QSize(320, 180)).convertToFormat(QImage::Format_ARGB32);
    viewer.layers[0].image = whole.scaled(160, 90, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    viewer.layers[0].footageSize = whole.size();
    const QImage fromSmall = renderCPU(viewer, 0, false, QSize(320, 180)).convertToFormat(QImage::Format_ARGB32);
    double off = 0;
    int covered = 0;
    for (int y = 0; y < 180; ++y)
      for (int x = 0; x < 320; ++x) {
        const QRgb a = fromWhole.pixel(x, y), b = fromSmall.pixel(x, y);
        off += std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b)) + std::abs(qAlpha(a) - qAlpha(b));
        covered += qAlpha(a) > 128;
      }
    off /= 320. * 180 * 4;
    check(off < 3 && covered > 10000 && clip.footageSize.isEmpty(),
          QString("a video layer drawn from a picture a quarter its size keeps its place and size in a small viewer (mean difference %1)")
              .arg(off, 0, 'f', 2));
  }
  {
    // A thread stuck making a picture -- inside Windows' decoder, which once
    // waited for good -- does not hold the server up as it goes: closing the
    // editor or opening another project waited on it for ever.
    VideoFrameServer::holdDecodingForTest(8000);
    QElapsedTimer stopping;
    {
      auto stuck = std::make_unique<VideoFrameServer>();
      stuck->frame({path, 0, false, "stuck"});
      QThread::msleep(300); // its thread is now in the middle of the picture
      stopping.start();
    }
    const qint64 waited = stopping.elapsed();
    VideoFrameServer::holdDecodingForTest(0);
    check(waited >= VideoFrameServer::stopWaitMs - 200 && waited < VideoFrameServer::stopWaitMs + 1500,
          QString("a frame server whose thread is stuck in a decoder lets it go after %1 ms rather than waiting for it").arg(waited));
  }
  {
    // Footage the decoders cannot open (Windows has no ProRes): the server
    // says so, so the editor can show it through the player, and it is not
    // asked for again and again. Its other footage still decodes.
    const QString broken = dir.path() + "/not-a-movie.mov";
    QFile junk(broken);
    junk.open(QIODevice::WriteOnly);
    junk.write(QByteArray(4096, 'x'));
    junk.close();
    int calls = 0;
    VideoFrameServer server([&] { ++calls; });
    QElapsedTimer t; t.start();
    while (t.elapsed() < 5000 && !server.cannotDecode(broken)) {
      server.frame({broken, 0, false, "broken"});
      QCoreApplication::processEvents();
      QThread::msleep(5);
    }
    const bool told = server.cannotDecode(broken) && server.frame({broken, 0, false, "broken"}).isNull();
    bool exact = false;
    t.restart();
    while (t.elapsed() < 5000 && !exact) {
      server.frame({path, 3333333, false, "good"}, &exact);
      QCoreApplication::processEvents();
      QThread::msleep(5);
    }
    check(told && !server.cannotDecode(path) && exact,
          QString("footage the decoders cannot open is reported, and other footage still decodes [reported %1, other %2]")
              .arg(told).arg(exact));
  }
  return failures;
}
