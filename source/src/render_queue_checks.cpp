#include "render_queue.h"
#include "render_queue_panel.h"
#include "native_media.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <mutex>

// --render-queue-self-test
int checkRenderQueue(QStringList &report) {
  int failures = 0;
  auto check = [&](bool passed, const QString &label) {
    report << label + (passed ? ": PASS" : ": FAIL");
    failures += !passed;
  };
  QTemporaryDir out;
  auto frameRender = [](Scene &s) { return renderCPU(s, s.frame, false); };
  // A box that moves one pixel a frame, so every frame is its own.
  auto moving = [](int frames) {
    Scene s;
    s.width = 96; s.height = 54; s.fps = 30; s.frames = frames;
    s.background = QColor(10, 10, 20);
    s.motionBlur = false;
    Layer box;
    box.type = "shape"; box.shapeKind = "rect"; box.name = "Box"; box.duration = frames; box.color = QColor(250, 200, 40);
    box.p[ShapeWidth].base = 8; box.p[ShapeHeight].base = 8;
    box.p[1].base = 27;
    box.p[0].set(0, 10); box.p[0].set(frames - 1, 10 + frames - 1);
    s.layers = {box};
    return s;
  };

  {
    RenderQueueItem item;
    item.id = "x1"; item.enabled = false; item.composition = "group-7"; item.name = "Chorus";
    item.output = "D:/renders/chorus.mov";
    item.settings.format = 4; item.settings.processingDepth = 16; item.settings.bitrateMbps = 80;
    item.settings.includeAudio = false; item.settings.preserveTransparency = true;
    item.settings.firstFrame = 30; item.settings.lastFrame = 89;
    item.state = RenderQueueItem::Done;
    const auto back = renderQueueFromJson(renderQueueJson({item}));
    const RenderQueueItem &b = back.front();
    check(back.size() == 1 && b.id == "x1" && !b.enabled && b.composition == "group-7" && b.name == "Chorus" &&
              b.output == item.output && b.settings.format == 4 && b.settings.processingDepth == 16 &&
              b.settings.bitrateMbps == 80 && !b.settings.includeAudio && b.settings.preserveTransparency &&
              b.settings.firstFrame == 30 && b.settings.lastFrame == 89 && b.state == RenderQueueItem::Done,
          "a queued render is saved with the project and opened again as it was, finished or not");
    Scene project;
    QString error;
    Scene with = moving(10);
    with.renderQueue = renderQueueJson({item});
    check(project.load(with.json(), error) && project.renderQueue.size() == 1 &&
              renderQueueFromJson(project.renderQueue).front().name == "Chorus",
          "the queue travels in the project file");
  }
  {
    // A group rendered as a Main of its own.
    Scene s = moving(10);
    Layer group; group.type = "group"; group.name = "Verse"; group.id = "g";
    Layer inside = s.layers[0]; inside.id = "a"; inside.groupId = "g";
    Layer nested; nested.type = "group"; nested.id = "h"; nested.groupId = "g";
    Layer deeper = s.layers[0]; deeper.id = "b"; deeper.groupId = "h";
    Layer outside = s.layers[0]; outside.id = "c";
    s.layers = {group, inside, nested, deeper, outside};
    const Scene alone = compositionScene(s, "g");
    bool right = alone.layers.size() == 3;
    for (const auto &l : alone.layers) {
      if (l.id == "a" || l.id == "h") right &= l.groupId.isEmpty();
      else if (l.id == "b") right &= l.groupId == "h";
      else right = false;
    }
    check(right && compositionScene(s, QString()).layers.size() == 5,
          "a group renders as a Main of its own: its layers at the top, a group inside it still holding its own");
  }
  {
    RenderQueueItem movie;
    movie.name = "Main 01"; movie.settings.format = 0; movie.output = out.path() + "/final";
    RenderQueueItem mov = movie;
    mov.settings.format = 3; mov.output = out.path() + "/final.mp4";
    RenderQueueItem frames = movie;
    frames.settings.format = 1; frames.output = out.path();
    const QString first = renderQueueTarget(frames);
    QDir().mkpath(first);
    const QString second = renderQueueTarget(frames);
    check(renderQueueTarget(movie) == out.path() + "/final.mp4" && renderQueueTarget(mov) == out.path() + "/final.mov" &&
              first == out.path() + "/Main 01 frames" && second == out.path() + "/Main 01 frames 2",
          "each render lands where its format wants it, and a sequence never over one made before");
    QDir(first).removeRecursively();
  }
  {
    // Frames 10 to 14 of thirty: five frames, the first of them the Main's tenth.
    const Scene s = moving(30);
    RenderSettings settings;
    settings.format = 1; settings.includeAudio = false;
    settings.firstFrame = 10; settings.lastFrame = 14;
    const QString folder = out.path() + "/range";
    const QString error = renderToFile(s, settings, folder, frameRender, {});
    const QStringList written = QDir(folder).entryList({"frame-*.png"}, QDir::Files, QDir::Name);
    const QImage firstFrame = QImage(folder + "/frame-000000.png").convertToFormat(QImage::Format_ARGB32);
    auto boxLeft = [](const QImage &image) {
      for (int x = 0; x < image.width(); ++x)
        if (qRed(image.pixel(x, 27)) > 200) return x;
      return -1;
    };
    Scene tenth = s;
    tenth.frame = 10;
    const int expected = boxLeft(renderCPU(tenth, 10, false).convertToFormat(QImage::Format_ARGB32));
    check(error.isEmpty() && written.size() == 5 && written.front() == "frame-000000.png" && boxLeft(firstFrame) == expected &&
              expected > 0,
          QString("a range renders only its frames, numbered from its start (%1 frames; box at %2, the Main's frame 10 has it at %3)%4")
              .arg(written.size()).arg(boxLeft(firstFrame)).arg(expected).arg(error.isEmpty() ? QString() : " -- " + error));
  }
  {
    // The colour depth: a render works at the project's (Project Settings)
    // unless it asks for another -- a PNG of a 16 bpc project has 16 bits,
    // of an 8 bpc one 8, of an 8 bpc one asked for 16 bpc 16. A queued render
    // saved as the project's stays the project's.
    auto bitsOf = [&](int project, int asked, const QString &name) {
      Scene s = moving(3);
      s.processingDepth = project;
      RenderSettings settings;
      settings.format = 1; settings.includeAudio = false;
      settings.firstFrame = settings.lastFrame = 1;
      settings.processingDepth = asked;
      const QString folder = out.path() + "/" + name;
      const QString error = renderToFile(s, settings, folder, frameRender, {});
      const QImage written(folder + "/frame-000000.png");
      return error.isEmpty() && !written.isNull() ? (written.depth() > 32 ? 16 : 8) : -1;
    };
    const int sixteen = bitsOf(16, 0, "depth16"), eight = bitsOf(8, 0, "depth8"), asked = bitsOf(8, 16, "depthAsked");
    check(sixteen == 16 && eight == 8 && asked == 16,
          QString("a render works at the project's color depth unless it asks for another (16 bpc project: %1-bit PNG, "
                  "8 bpc: %2-bit, 8 bpc asked 16: %3-bit)").arg(sixteen).arg(eight).arg(asked));
    RenderQueueItem item;
    item.name = "Main 01";
    const auto back = renderQueueFromJson(renderQueueJson({item}));
    QJsonObject old = renderQueueItemJson(item);
    old.remove("processingDepth");
    check(RenderSettings{}.processingDepth == 0 && back.front().settings.processingDepth == 0 &&
              renderQueueItemFromJson(old).settings.processingDepth == 0,
          "a queued render follows the project's color depth unless set to its own, saved and opened again");
  }
  {
    // Sound in step with the picture: a tone that starts at frame 10. Rendered
    // from frame 10 it is there from the start; rendered whole, not before 10.
    const QString clip = out.path() + "/tone.mp4";
    bool made = false;
    try {
      NativeMedia media;
      media.beginMovie(clip, 96, 54, 30, 2000000, true);
      QImage black(96, 54, QImage::Format_RGB32);
      black.fill(Qt::black);
      for (int f = 0; f < 30; ++f) {
        media.writeFrame(black, f, 30);
        const qint64 first = qint64(f) * 1600, end = first + 1600;
        QByteArray pcm(int((end - first) * 4), 0);
        auto *s16 = reinterpret_cast<int16_t *>(pcm.data());
        for (qint64 i = first; i < end; ++i)
          s16[(i - first) * 2] = s16[(i - first) * 2 + 1] = int16_t(12000 * std::sin(2 * 3.14159265358979 * 440 * i / 48000.));
        media.writeAudio(pcm, first);
      }
      media.finishMovie();
      made = true;
    } catch (const std::exception &e) {
      report << QString("could not write the tone: %1").arg(e.what());
    }
    Scene s = moving(40);
    Layer sound;
    sound.type = "audio"; sound.name = "Tone"; sound.path = clip; sound.start = 10; sound.duration = 30; sound.sourceDurationMs = 1000;
    s.layers.push_back(sound);
    auto loudness = [&](const QString &movie, qint64 from, qint64 to) {
      QByteArray pcm;
      try {
        NativeMedia media;
        QBuffer buffer;
        buffer.open(QIODevice::ReadWrite);
        if (media.decodeAudio(movie, buffer, [] { return false; })) pcm = buffer.data();
      } catch (...) {}
      const auto *samples = reinterpret_cast<const int16_t *>(pcm.constData());
      to = std::min(to, qint64(pcm.size() / 4));
      double energy = 0;
      for (qint64 i = from; i < to; ++i) energy += double(samples[i * 2]) * samples[i * 2];
      return to > from ? std::sqrt(energy / (to - from)) : -1.;
    };
    RenderSettings settings;
    settings.format = 0; settings.bitrateMbps = 2; settings.includeAudio = true;
    const QString whole = out.path() + "/whole.mp4", part = out.path() + "/part.mp4";
    QString error = made ? renderToFile(s, settings, whole, frameRender, {}) : QString("no tone");
    settings.firstFrame = 10; settings.lastFrame = 29;
    if (error.isEmpty()) error = renderToFile(s, settings, part, frameRender, {});
    const double wholeEarly = loudness(whole, 4000, 12000), wholeLate = loudness(whole, 20000, 28000);
    const double partEarly = loudness(part, 4000, 12000);
    check(error.isEmpty() && wholeEarly >= 0 && wholeEarly < 300 && wholeLate > 5000 && partEarly > 5000,
          QString("a range's sound starts where its picture does (whole: %1 before frame 10, %2 after; from frame 10: %3 at once)%4")
              .arg(wholeEarly, 0, 'f', 0).arg(wholeLate, 0, 'f', 0).arg(partEarly, 0, 'f', 0)
              .arg(error.isEmpty() ? QString() : " -- " + error));
  }
  {
    // The runner: returns at once, renders one after another, tells how it goes.
    std::mutex guard;
    std::vector<std::pair<QString, int>> heard;
    bool over = false;
    auto listen = [&](const QString &id, int state, int, int, const QString &) {
      std::lock_guard<std::mutex> lock(guard);
      if (id.isEmpty()) over = true;
      else if (heard.empty() || heard.back() != std::make_pair(id, state)) heard.push_back({id, state});
    };
    auto waitOver = [&](int ms) {
      QElapsedTimer clock;
      clock.start();
      while (clock.elapsed() < ms) {
        {
          std::lock_guard<std::mutex> lock(guard);
          if (over) return true;
        }
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
      }
      return false;
    };
    RenderSettings settings;
    settings.format = 1; settings.includeAudio = false;
    std::vector<RenderQueueRunner::Job> jobs{{"one", moving(12), settings, out.path() + "/one"},
                                             {"two", moving(8), settings, out.path() + "/two"}};
    RenderQueueRunner runner;
    QElapsedTimer started;
    started.start();
    runner.start(jobs, frameRender, listen);
    const double returnedAfter = started.nsecsElapsed() / 1e6;
    const bool busyAtOnce = runner.running();
    const bool finished = waitOver(60000);
    bool order = false;
    {
      std::lock_guard<std::mutex> lock(guard);
      std::vector<std::pair<QString, int>> expected{{"one", RenderQueueItem::Rendering}, {"one", RenderQueueItem::Done},
                                                    {"two", RenderQueueItem::Rendering}, {"two", RenderQueueItem::Done}};
      order = heard == expected;
    }
    check(returnedAfter < 50 && busyAtOnce && finished && order && !runner.running() &&
              QDir(out.path() + "/one").entryList({"*.png"}).size() == 12 && QDir(out.path() + "/two").entryList({"*.png"}).size() == 8,
          QString("the queue renders in the background, one after another (start returned after %1 ms)").arg(returnedAfter, 0, 'f', 1));

    // Stopped: the render under way stops at its next frame, leaves nothing
    // behind, and what is after it does not start.
    {
      std::lock_guard<std::mutex> lock(guard);
      heard.clear();
      over = false;
    }
    Scene longer = moving(600);
    longer.width = 640; longer.height = 360;
    std::vector<RenderQueueRunner::Job> more{{"long", longer, settings, out.path() + "/long"},
                                             {"after", moving(8), settings, out.path() + "/after"}};
    runner.start(more, frameRender, listen);
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 20000) {
      bool going = false;
      {
        std::lock_guard<std::mutex> lock(guard);
        going = !heard.empty();
      }
      if (going) break;
      QEventLoop loop;
      QTimer::singleShot(5, &loop, &QEventLoop::quit);
      loop.exec();
    }
    runner.stop();
    const bool stopped = waitOver(20000);
    bool right = false;
    {
      std::lock_guard<std::mutex> lock(guard);
      right = heard.size() == 3 && heard[0] == std::make_pair(QString("long"), int(RenderQueueItem::Rendering)) &&
              heard[1] == std::make_pair(QString("long"), int(RenderQueueItem::Stopped)) &&
              heard[2] == std::make_pair(QString("after"), int(RenderQueueItem::Stopped));
    }
    check(stopped && right && !QFileInfo::exists(out.path() + "/long") && !QFileInfo::exists(out.path() + "/after"),
          "Stop ends the render under way, leaves no half-made output, and the rest do not start");
  }
  {
    // The Main with a Play Range set goes into the queue as that range; with
    // none, whole.
    Scene ranged = moving(60);
    ranged.playStart = 10; ranged.playEnd = 19;
    Scene current = ranged;
    RenderQueueHost host;
    host.snapshot = [&] { return current; };
    host.compositions = [] { return std::vector<std::pair<QString, QString>>{{QString(), QString("Main")}}; };
    host.currentComposition = [] { return QString(); };
    host.frames = [&] { return current.frames; };
    host.projectFolder = [&] { return out.path(); };
    RenderQueuePanel panel(host);
    panel.addCurrent();
    const bool fromRange = !panel.items().empty() && panel.items().back().settings.firstFrame == 10 &&
                           panel.items().back().settings.lastFrame == 19;
    current.playStart = current.playEnd = -1;
    panel.addCurrent();
    const bool whole = panel.items().size() == 2 && panel.items().back().settings.firstFrame == 0 &&
                       panel.items().back().settings.lastFrame == -1;
    check(fromRange && whole, "a Main with a Play Range is queued as that range, one without it whole");
  }
  // Rendering while the preview plays: the preview's cap on motion blur
  // samples (one sample, no blur at all, at its lowest) is the preview's; a
  // frame drawn for the queue keeps all of its blur.
  {
    Scene s;
    s.width = 160; s.height = 60; s.fps = 30; s.frames = 30;
    s.motionBlur = true;
    s.background = Qt::black;
    Layer box;
    box.type = "rect"; box.color = Qt::white; box.duration = 30; box.motionBlur = true;
    box.p[0].set(0, 20); box.p[0].set(20, 140); box.p[1].base = 30;
    box.p[ShapeWidth].base = 20; box.p[ShapeHeight].base = 20;
    s.layers = {box};
    s.frame = 10;
    const QImage whole = renderCPU(s, 10, true);
    setPreviewMotionBlurCap(1);
    const QImage capped = renderCPU(s, 10, true);
    Scene queued = s;
    queued.finalRender = true;
    const QImage kept = renderCPU(queued, 10, true);
    setPreviewMotionBlurCap(0);
    check(!whole.isNull() && capped != whole && kept == whole,
          "a frame rendered for the queue while the preview plays keeps its motion blur");
  }
  return failures ? 1 : 0;
}
