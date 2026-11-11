#include "tracker.h"
#include "native_media.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <cmath>

namespace {
// Footage whose every pixel is known exactly: a textured disc on a textured
// ground, the disc's texture carried with it, so a move of a fraction of a
// pixel is a move of a fraction of a pixel and nothing else.
double discTexture(double x, double y) {
  return 128 + 45 * std::sin(.31 * x + .17 * y + 1) + 35 * std::sin(-.23 * x + .41 * y + 2) + 25 * std::sin(.52 * x - .11 * y + .5);
}
double groundTexture(double x, double y) {
  return 110 + 30 * std::sin(.13 * x + .07 * y) + 20 * std::sin(.05 * x - .19 * y + 1.3);
}
QImage footage(QSize size, QPointF disc, bool withDisc = true, double radius = 42) {
  QImage image(size, QImage::Format_RGB32);
  for (int y = 0; y < size.height(); ++y) {
    QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
    for (int x = 0; x < size.width(); ++x) {
      const double px = x + .5, py = y + .5;
      const double d = std::hypot(px - disc.x(), py - disc.y());
      const double w = withDisc ? std::clamp((radius - d) / 2 + .5, 0., 1.) : 0.;
      const double v = w * discTexture(px - disc.x(), py - disc.y()) + (1 - w) * groundTexture(px, py);
      const int g = std::clamp(int(std::lround(v)), 0, 255);
      row[x] = qRgb(g, g, g);
    }
  }
  return image;
}
// Nothing but smooth ground, slid by `shift`: a wall or a sky, where the
// correlation peak is broad and a parabola through it misses. Three waves,
// not two: two alone repeat as an exact grid (every 32.9 px here), and a spot
// that looks exactly like another cannot be told from it by anyone.
QImage smoothGround(QSize size, QPointF shift) {
  QImage image(size, QImage::Format_RGB32);
  for (int y = 0; y < size.height(); ++y) {
    QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
    for (int x = 0; x < size.width(); ++x) {
      const double gx = x + .5 - shift.x(), gy = y + .5 - shift.y();
      const int g = std::clamp(int(std::lround(groundTexture(gx, gy) + 18 * std::sin(.083 * gx + .117 * gy + .4))), 0, 255);
      row[x] = qRgb(g, g, g);
    }
  }
  return image;
}
// Where the disc is at a frame: a drift right with a bob, never on whole pixels.
QPointF path(double frame) { return {180.3 + 4.37 * frame, 175.6 + 38 * std::sin(frame / 9.)}; }
}  // namespace

// --tracker-self-test
int checkTracker(QStringList &report) {
  int failures = 0;
  auto check = [&](bool passed, const QString &label) {
    report << label + (passed ? ": PASS" : ": FAIL");
    failures += !passed;
  };
  const QSize size(640, 360);
  {
    // A patch found where it moved, to a small fraction of a pixel.
    const QPointF from(300.4, 170.7);
    const Luma before = lumaOf(footage(size, from));
    double worst = 0;
    QRandomGenerator random(7);
    for (int i = 0; i < 16; ++i) {
      const QPointF moved(random.bounded(24.) - 12, random.bounded(24.) - 12);
      const Luma after = lumaOf(footage(size, from + moved));
      const PatchMatch m = matchPatch(before, from, 41, after, from, 81);
      worst = std::max(worst, m.found ? QLineF(m.at, from + moved).length() : 99.);
    }
    check(worst < .15, QString("a patch is found where it moved, to within %1 px").arg(worst, 0, 'f', 3));
    const Luma ground = lumaOf(smoothGround(size, {}));
    double smoothWorst = 0;
    for (int i = 0; i < 16; ++i) {
      const QPointF moved(random.bounded(24.) - 12, random.bounded(24.) - 12);
      const Luma after = lumaOf(smoothGround(size, moved));
      const PatchMatch m = matchPatch(ground, from, 51, after, from, 101);
      smoothWorst = std::max(smoothWorst, m.found ? QLineF(m.at, from + moved).length() : 99.);
    }
    check(smoothWorst < .1, QString("on smooth ground too, to within %1 px").arg(smoothWorst, 0, 'f', 3));
    Luma flat;
    flat.width = 200; flat.height = 200; flat.pixels.assign(200 * 200, 90);
    check(!matchPatch(flat, {100, 100}, 31, flat, {100, 100}, 81).found, "a flat patch is not something that can be followed");
  }
  {
    // Where a point is between samples, and outside what was tracked.
    TrackPoint p;
    p.set({10, {0, 0}, 1});
    p.set({20, {10, -20}, 1});
    p.set({15, {4, -8}, 1});
    p.set({20, {10, -30}, 1});
    check(p.samples.size() == 3 && p.at(5) == QPointF(0, 0) && p.at(17.5) == QPointF(7, -19) && p.at(40) == QPointF(10, -30) &&
              p.sampleAt(15) && !p.sampleAt(16),
          "a point between samples is on the line between them, and held outside them");
  }
  // Real footage: the disc encoded to H.264 by the same writer export uses,
  // decoded back and tracked.
  QTemporaryDir dir;
  const QString movie = dir.path() + "/moving-disc.mp4";
  const int frames = 60;
  bool written = false;
  try {
    NativeMedia media;
    media.beginMovie(movie, size.width(), size.height(), 30, 12000000, false);
    for (int f = 0; f < frames; ++f) media.writeFrame(footage(size, path(f)), f, 30);
    media.finishMovie();
    written = QFileInfo(movie).size() > 1000;
  } catch (const std::exception &e) {
    report << QString("could not write the test footage: %1").arg(e.what());
  }
  check(written, "test footage written");
  const QPointF centre(size.width() / 2., size.height() / 2.);
  std::vector<TrackSample> forward;
  if (written) {
    TrackJob job;
    job.path = movie; job.fps = 30; job.sourceDurationMs = frames * 1000. / 30;
    job.fromFrame = 0; job.toFrame = frames - 1; job.start = path(0) - centre; job.feature = 41; job.search = 101;
    QElapsedTimer clock; clock.start();
    runTrack(job);
    const double ms = clock.nsecsElapsed() / 1e6;
    double sum = 0, worst = 0;
    for (const auto &s : job.samples) {
      const double e = QLineF(s.at, path(s.frame) - centre).length();
      sum += e; worst = std::max(worst, e);
    }
    const double mean = job.samples.empty() ? 99 : sum / job.samples.size();
    check(job.error.isEmpty() && int(job.samples.size()) == frames - 1 && mean < .3 && worst < .8,
          QString("tracked forward through %1 frames of H.264: mean error %2 px, worst %3 px, %4 ms a frame%5")
              .arg(job.samples.size()).arg(mean, 0, 'f', 3).arg(worst, 0, 'f', 3).arg(ms / std::max<size_t>(1, job.samples.size()), 0, 'f', 1)
              .arg(job.error.isEmpty() ? QString() : " -- " + job.error));
    forward = job.samples;
    TrackJob back;
    back.path = movie; back.fps = 30; back.sourceDurationMs = job.sourceDurationMs;
    back.fromFrame = frames - 1; back.toFrame = 0; back.start = path(frames - 1) - centre; back.feature = 41; back.search = 101;
    runTrack(back);
    sum = 0; worst = 0;
    bool descending = true;
    for (size_t i = 0; i < back.samples.size(); ++i) {
      const double e = QLineF(back.samples[i].at, path(back.samples[i].frame) - centre).length();
      sum += e; worst = std::max(worst, e);
      if (i > 0) descending &= back.samples[i].frame == back.samples[i - 1].frame - 1;
    }
    const double backMean = back.samples.empty() ? 99 : sum / back.samples.size();
    check(back.error.isEmpty() && int(back.samples.size()) == frames - 1 && descending && backMean < .3 && worst < .8,
          QString("tracked backward, a frame at a time: mean error %1 px, worst %2 px").arg(backMean, 0, 'f', 3).arg(worst, 0, 'f', 3));
    // A job told to stop, stops.
    TrackJob stopped;
    stopped.path = movie; stopped.fps = 30; stopped.fromFrame = 0; stopped.toFrame = frames - 1; stopped.start = path(0) - centre;
    stopped.cancel = true;
    runTrack(stopped);
    check(stopped.done && stopped.samples.empty(), "a job told to stop does no more");
  }
  {
    // The disc leaves the shot at frame 20: the tracker says so and stops.
    const QString gone = dir.path() + "/disc-leaves.mp4";
    bool made = false;
    try {
      NativeMedia media;
      media.beginMovie(gone, size.width(), size.height(), 30, 12000000, false);
      for (int f = 0; f < 30; ++f) media.writeFrame(footage(size, path(f), f < 20), f, 30);
      media.finishMovie();
      made = true;
    } catch (...) {}
    TrackJob job;
    job.path = gone; job.fps = 30; job.fromFrame = 0; job.toFrame = 29; job.start = path(0) - centre; job.feature = 41; job.search = 101;
    if (made) runTrack(job);
    check(made && job.lostAt == 20 && !job.samples.empty() && job.samples.back().frame == 19,
          QString("a point that leaves the shot is reported lost where it went (frame %1)").arg(job.lostAt));
  }
  {
    // Carrying a title on the point, and holding the point still.
    Scene scene;
    scene.width = 1920; scene.height = 1080; scene.fps = 30; scene.frames = 90;
    Layer video; video.type = "video"; video.id = "v"; video.name = "Footage"; video.duration = 90; video.start = 5;
    video.image = QImage(size, QImage::Format_RGB32);
    video.p[0].base = 900; video.p[1].base = 500; video.p[2].base = video.p[3].base = 1.5; video.p[4].base = 10;
    Layer title; title.type = "text"; title.id = "t"; title.name = "Title"; title.text = "Hi"; title.duration = 90;
    title.p[0].base = 1000; title.p[1].base = 400;
    scene.layers = {title, video};
    TrackPoint point;
    point.name = "Track 1";
    point.samples.push_back({0, path(0) - centre, 1});
    for (const auto &s : forward) point.samples.push_back(s);
    if (point.samples.size() < 2)
      for (int f = 1; f < frames; ++f) point.samples.push_back({double(f), path(f) - centre, 1});
    auto onScreen = [&](const Scene &s, double layerFrame, QPointF at) {
      const Layer &v = s.layers[1];
      return layerWorldTransform(v, s, v.start + layerFrame).map(at);
    };
    Scene carried = scene;
    const int keys = applyTrackToLayer(carried, 1, point, 0, 5);
    const Layer &t = carried.layers[0];
    bool follows = keys == int(point.samples.size());
    const QPointF offset = QPointF(1000, 400) - onScreen(carried, 0, point.at(0));
    for (const auto &s : point.samples) {
      const double comp = 5 + s.frame;
      const QPointF at(t.p[0].at(comp), t.p[1].at(comp));
      follows &= QLineF(at, onScreen(carried, s.frame, s.at) + offset).length() < 1e-6;
    }
    check(follows && std::abs(t.p[0].at(5) - 1000) < 1e-6 && std::abs(t.p[1].at(5) - 400) < 1e-6,
          QString("Apply Motion: %1 keys, the title keeps where it was at the frame chosen and moves with the point").arg(keys));
    Scene held = scene;
    const QPointF pinned = onScreen(held, 30, point.at(30));
    stabilizeWithTrack(held, 1, point, 35);
    double drift = 0;
    for (const auto &s : point.samples) drift = std::max(drift, QLineF(onScreen(held, s.frame, s.at), pinned).length());
    check(drift < 1e-6, QString("Stabilize: the point stays where it was on screen, moving at most %1 px").arg(drift, 0, 'g', 3));
    // Kept with the project.
    setLayerTracks(held.layers[1], {point});
    Scene reopened;
    QString error;
    reopened.load(held.json(), error);
    const auto back = reopened.layers.size() == 2 ? layerTracks(reopened.layers[1]) : std::vector<TrackPoint>{};
    bool same = back.size() == 1 && back[0].samples.size() == point.samples.size() && back[0].name == "Track 1";
    for (size_t i = 0; same && i < point.samples.size(); ++i)
      same = QLineF(back[0].samples[i].at, point.samples[i].at).length() < .001 && back[0].samples[i].frame == point.samples[i].frame;
    check(same, "track points are saved with the project and come back the same");
  }
  {
    // What a match costs on full HD footage.
    const QSize hd(1920, 1080);
    const Luma a = lumaOf(footage(hd, {900.2, 500.7})), b = lumaOf(footage(hd, {907.9, 496.1}));
    std::vector<double> times;
    for (int i = 0; i < 7; ++i) {
      QElapsedTimer clock; clock.start();
      matchPatch(a, {900.2, 500.7}, 41, b, {900.2, 500.7}, 121);
      times.push_back(clock.nsecsElapsed() / 1e6);
    }
    std::sort(times.begin(), times.end());
    check(times[3] < 8, QString("one match, a 41 px patch over a 121 px area of 1080p: %1 ms").arg(times[3], 0, 'f', 2));
  }
  return failures;
}
