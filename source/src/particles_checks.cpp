#include "particles.h"
#include "expressions.h"
#include "render_workers.h"
#include <QElapsedTimer>
#include <cmath>
#include <future>

// --particles-self-test
int checkParticles(QStringList &report) {
  int failures = 0;
  auto check = [&](bool passed, const QString &label) {
    report << label + (passed ? ": PASS" : ": FAIL");
    failures += !passed;
  };
  auto near = [](double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; };
  Scene scene;
  scene.width = 640; scene.height = 360; scene.fps = 30; scene.frames = 300;
  auto emitter = [&] {
    Layer l; l.type = "particles"; l.name = "Particles"; l.duration = 300;
    l.p[0].base = 320; l.p[1].base = 180;
    l.particles = QJsonObject{{"sizeVariance", 0}, {"speedVariance", 0}, {"lifeVariance", 0}, {"fadeIn", 0}, {"fadeOut", 0},
                              {"randomRotation", false}, {"space", 1}};
    return l;
  };
  auto count = [&](const Layer &l, double frame) { return int(particleStates(l, scene, frame)->size()); };

  {
    // The same frame is the same particles, whatever was drawn before it.
    Layer l = emitter();
    l.p[ParticleTurbulence].base = 30;
    const auto a = particleStates(l, scene, 45);
    particleStates(l, scene, 90);
    const auto b = particleStates(l, scene, 45);
    bool same = a->size() == b->size();
    for (size_t i = 0; same && i < a->size(); ++i) same = (*a)[i].position == (*b)[i].position && (*a)[i].size == (*b)[i].size;
    check(same && !a->empty(), "a frame is worked out from time alone: the same particles whatever came before");
  }
  {
    Layer l = emitter();
    l.p[ParticleRate].base = 30; l.p[ParticleLife].base = 2;
    check(near(count(l, 30), 30, 1) && near(count(l, 150), 60, 1),
          QString("30 a second for 2 seconds: 30 after a second, 60 once it is steady (%1, %2)").arg(count(l, 30)).arg(count(l, 150)));
    l.particles["burst"] = 50; l.p[ParticleRate].base = 0;
    check(count(l, 0) == 50 && count(l, 30) == 50 && count(l, 61) == 0, "a burst is born at the start and lives its life");
    Layer pre = emitter();
    pre.p[ParticleRate].base = 20; pre.p[ParticleLife].base = 4; pre.particles["preroll"] = 3;
    check(near(count(pre, 0), 60, 1), QString("three seconds of pre-roll fill the frame from the start (%1)").arg(count(pre, 0)));
  }
  {
    // Gravity, and drag, in closed form.
    Layer l = emitter();
    l.p[ParticleRate].base = 0; l.particles["burst"] = 1; l.p[ParticleLife].base = 10;
    l.p[ParticleSpeed].base = 0; l.p[ParticleGravity].base = 100;
    const auto fallen = particleStates(l, scene, 30);
    check(!fallen->empty() && near(fallen->front().position.y(), 50, .01), "gravity: half of g t squared after one second");
    Layer slowed = emitter();
    slowed.p[ParticleRate].base = 0; slowed.particles["burst"] = 1; slowed.p[ParticleLife].base = 20;
    slowed.p[ParticleSpeed].base = 100; slowed.p[ParticleSpread].base = 0; slowed.particles["drag"] = 2;
    const auto drifted = particleStates(slowed, scene, 300);
    check(!drifted->empty() && near(drifted->front().position.y(), -50, .05), "drag: a particle thrown upward comes to rest at speed over drag");
  }
  {
    // A rate that is keyframed: nothing until it starts, then what the curve adds up to.
    Layer l = emitter();
    l.p[ParticleLife].base = 20;
    l.p[ParticleRate].set(0, 0); l.p[ParticleRate].set(30, 0); l.p[ParticleRate].set(31, 60);
    // Half a frame ramping up, then a second at 60 a second: about 61.
    check(count(l, 29) == 0 && near(count(l, 61), 61, 2), QString("a keyframed rate: none before it starts, then its sum (%1)").arg(count(l, 61)));
  }
  {
    // In world space a moving emitter leaves its particles behind; in layer space they go with it.
    Layer l = emitter();
    l.p[ParticleRate].base = 30; l.p[ParticleLife].base = 5; l.p[ParticleSpeed].base = 0;
    l.p[0].set(0, 100); l.p[0].set(60, 500);
    l.particles["space"] = 0;
    double low = 1e9, high = -1e9;
    // Held by name: a range-for over *particleStates(...) would let the list go before the loop reads it.
    const auto worldStates = particleStates(l, scene, 60);
    for (const auto &p : *worldStates) { low = std::min(low, p.position.x()); high = std::max(high, p.position.x()); }
    const auto trail = particleStates(l, scene, 60);
    check(high - low > 350, QString("world space: a moving emitter leaves a trail %1 px long (%2 particles, first at %3)")
                                .arg(high - low, 0, 'f', 0).arg(trail->size())
                                .arg(trail->empty() ? QString("-") : QString("%1,%2").arg(trail->front().position.x()).arg(trail->front().position.y())));
    l.particles["space"] = 1;
    low = 1e9; high = -1e9;
    const auto layerStates = particleStates(l, scene, 60);
    for (const auto &p : *layerStates) { low = std::min(low, p.position.x()); high = std::max(high, p.position.x()); }
    check(!layerStates->empty() && high - low < 1, "layer space: the particles move with the layer");
  }
  {
    // Everything drawn lies inside the layer's bounds, and a frame is a frame.
    Scene one = scene;
    Layer l = emitter();
    l.p[ParticleRate].base = 80; l.p[ParticleLife].base = 3; l.p[ParticleSpeed].base = 120; l.p[ParticleSpread].base = 360;
    l.p[ParticleGravity].base = 40; l.p[ParticleSize].base = 14; l.p[ParticleTurbulence].base = 20;
    l.particles = QJsonObject{{"shape", 6}, {"sizeVariance", 50}, {"colorVariance", 30}};
    one.layers = {l};
    one.frame = 60;
    const QImage frame = renderCPU(one, 60, false).convertToFormat(QImage::Format_ARGB32);
    const QRectF bounds = layerWorldTransform(one.layers[0], one, 60).mapRect(layerBounds(one.layers[0], one));
    int outside = 0, inside = 0;
    for (int y = 0; y < frame.height(); ++y)
      for (int x = 0; x < frame.width(); ++x) {
        if (qAlpha(frame.pixel(x, y)) == 0) continue;
        (bounds.adjusted(-1, -1, 1, 1).contains(QPointF(x + .5, y + .5)) ? inside : outside)++;
      }
    check(inside > 500 && outside == 0, QString("every particle drawn lies inside the layer's bounds (%1 in, %2 out)").arg(inside).arg(outside));
    const QImage after = renderCPU(one, 60, false).convertToFormat(QImage::Format_ARGB32);
    renderCPU(one, 10, false);
    const QImage again = renderCPU(one, 60, false).convertToFormat(QImage::Format_ARGB32);
    check(frame == after && frame == again, "a frame drawn again, or after another, is the same picture");
    check(!sameCompositionPixels(one, 60, 61) && layerHasAnimatedEffect(one.layers[0]),
          "a particle frame is never taken for its neighbour");
    std::vector<std::future<QImage>> jobs;
    for (int i = 0; i < 4; ++i) jobs.push_back(std::async(std::launch::async, [&one] { return renderCPU(one, 60, false); }));
    bool allSame = true;
    for (auto &job : jobs) allSame &= job.get().convertToFormat(QImage::Format_ARGB32) == frame;
    check(allSame, "the same frame drawn on several threads at once is the same picture");
  }
  {
    // What is remembered is keyed on the whole history: a key moved a second
    // back, or the emitter's path changed before now, is a different picture
    // now -- on the plain path and through a glow, which keeps a raster.
    Scene one = scene;
    Layer l = emitter();
    l.p[ParticleRate].set(0, 20); l.p[ParticleRate].set(20, 20);
    l.p[ParticleLife].base = 4; l.p[ParticleSpeed].base = 60; l.p[ParticleSize].base = 10;
    l.p[0].set(0, 200); l.p[0].set(60, 440);
    l.particles["space"] = 0;
    one.layers = {l};
    one.frame = 60;
    const QByteArray before = particleIdentity(one.layers[0], one, 60);
    const QImage plain = renderCPU(one, 60, false);
    one.layers[0].glow = 6;
    const QImage glowing = renderCPU(one, 60, false);
    one.layers[0].p[ParticleRate].keys[1].value = 60; // up to a second ago only
    const QByteArray keyed = particleIdentity(one.layers[0], one, 60);
    const QImage glowingAfter = renderCPU(one, 60, false);
    one.layers[0].glow = 0;
    const QImage plainAfter = renderCPU(one, 60, false);
    one.layers[0].p[0].keys[0].value = 100; // where it started from
    const QByteArray moved = particleIdentity(one.layers[0], one, 60);
    const QImage plainMoved = renderCPU(one, 60, false);
    check(before != keyed && keyed != moved && plain != plainAfter && glowing != glowingAfter && plainAfter != plainMoved,
          "a key changed before this frame changes this frame, with a glow on it or not");
    ExpressionSceneScope expressions(&one); // as a render has it
    one.layers[0].p[ParticleRate].setExpression("value * 2");
    const QByteArray a = particleIdentity(one.layers[0], one, 60);
    const double twice = one.layers[0].p[ParticleRate].at(30);
    one.layers[0].p[ParticleRate].setExpression("value * 3");
    QString why;
    const double thrice = evaluateExpression(*one.layers[0].p[ParticleRate].program, one.layers[0].p[ParticleRate], 30, &why);
    check(a != particleIdentity(one.layers[0], one, 60) && !a.isEmpty(),
          QString("an expression on a slider is part of what the frame is (%1, then %2%3)").arg(twice).arg(thrice).arg(why));
    check(renderCPU(one, 10, false) != renderCPU(one, 60, false), "two frames of a particle layer are two pictures");
  }
  {
    // The dots, discs, bokeh and streaks are written straight into the pixels;
    // in deep colour QPainter still draws them. Both are the same picture.
    for (int shape : {0, 1, 3, 4, 6}) {
      for (int blend : {0, 1}) {
        Scene one = scene;
        one.background = QColor("#202030");
        Layer l = emitter();
        l.p[ParticleRate].base = 60; l.p[ParticleLife].base = 3; l.p[ParticleSpeed].base = 90; l.p[ParticleSpread].base = 360;
        l.p[ParticleSize].base = shape == 6 ? 60 : 16; l.p[2].base = 1.3; l.p[4].base = 20; l.p[5].base = .8;
        l.particles = QJsonObject{{"shape", shape}, {"blend", blend}, {"sizeVariance", 60}, {"colorVariance", 40},
                                  {"colorStart", "#ffb060"}, {"colorEnd", "#60a0ff"}, {"space", 1}};
        one.layers = {l};
        one.frame = 60;
        // Premultiplied: what is laid over the picture beneath. Straight colour
        // makes a pixel one step from clear look as wrong as a wrong one.
        const QImage eight = renderCPU(one, 60, false).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        one.processingDepth = 16;
        const QImage deep = renderCPU(one, 60, false).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        double sum = 0; int off = 0, lit = 0;
        for (int y = 0; y < eight.height(); ++y)
          for (int x = 0; x < eight.width(); ++x) {
            const QRgb a = eight.pixel(x, y), b = deep.pixel(x, y);
            const int d = std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b)),
                                    std::abs(qAlpha(a) - qAlpha(b))});
            sum += d; off += d > 24; lit += qAlpha(a) > 0;
          }
        const double mean = sum / (eight.width() * eight.height());
        // Edges are anti-aliased two ways, so a pixel here and there on a rim
        // differs; what matters is that almost none can be seen to.
        check(lit > 2000 && mean < 1 && off * 200 < lit,
              QString("shape %1%2 drawn directly matches QPainter: mean %3 levels, %4 of %5 lit pixels off by more than 24")
                  .arg(shape).arg(blend ? " (add)" : "").arg(mean, 0, 'f', 3).arg(off).arg(lit));
      }
    }
  }
  {
    // The box a person grabs is the emitter: the same at every frame, however
    // far the particles have flown, while what is drawn is still all covered.
    Scene one = scene;
    Layer l = emitter();
    applyParticlePreset(l, one, "Sparks");
    one.layers = {l};
    QRectF first, widest;
    bool steady = true;
    for (double f : {0., 20., 45., 90., 150.}) {
      one.frame = f;
      const QRectF box = layerSelectionContentBounds(one.layers[0], one);
      if (f == 0) first = box; else steady &= box == first && layerContentBounds(one.layers[0], one) == first;
      widest = widest.isNull() ? layerBounds(one.layers[0], one) : widest.united(layerBounds(one.layers[0], one));
    }
    check(steady && first.width() >= 80 && widest.height() > first.height() * 3,
          QString("the viewer's box is the emitter and holds still (%1x%2) while the particles reach %3 px")
              .arg(first.width()).arg(first.height()).arg(widest.height(), 0, 'f', 0));
    Layer line = emitter();
    line.particles["emitter"] = 1; line.particles["emitterWidth"] = 600;
    Layer box = emitter();
    box.particles["emitter"] = 2; box.particles["emitterWidth"] = 300; box.particles["emitterHeight"] = 200;
    check(particleEmitterBounds(line).width() == 600 && particleEmitterBounds(box) == QRectF(-150, -100, 300, 200),
          "a line emitter's box is as wide as the line, a box emitter's is the box");
  }
  {
    // Any layer can give off particles: a title that sparkles.
    Scene one = scene;
    Layer title; title.type = "text"; title.text = "SPARK"; title.fontSize = 80; title.duration = 300;
    title.p[0].base = 320; title.p[1].base = 180;
    one.layers = {title};
    one.frame = 30;
    check(particleStates(one.layers[0], one, 30)->empty() && !layerHasAnimatedEffect(one.layers[0]),
          "a layer gives off no particles until they are switched on for it");
    one.layers[0].particlesOn = true;
    applyParticlePreset(one.layers[0], one, "Sparkle");
    const Layer &t = one.layers[0];
    check(t.p[0].base == 320 && t.p[1].base == 180 && particleSetting(t, "emitter") == 5,
          "Sparkle given to a title leaves the title where it is and is born over its own area");
    Layer still = t;
    still.p[ParticleSpeed].base = 0; still.p[ParticleTurbulence].base = 0; still.particles["space"] = 1;
    const QRectF own = layerOwnBounds(still, one);
    const auto states = particleStates(still, one, 30);
    bool inside = !states->empty();
    for (const auto &p : *states) inside &= own.adjusted(-.5, -.5, .5, .5).contains(p.position);
    check(inside, QString("its particles are born over the words (%1 of them, all inside the %2 x %3 box)")
                      .arg(states->size()).arg(own.width(), 0, 'f', 0).arg(own.height(), 0, 'f', 0));
    Layer plain = t;
    plain.particlesOn = false;
    check(layerBounds(t, one).contains(layerOwnBounds(t, one)) && layerSelectionBounds(t, one) == layerSelectionBounds(plain, one) &&
              layerContentBounds(t, one) == layerContentBounds(plain, one),
          "what is drawn takes the particles in; the box grabbed is still just the title");
    check(layerHasAnimatedEffect(t), "a title with particles is a new picture every frame");
    Scene without = one;
    without.layers[0].particlesOn = false;
    check(renderCPU(one, 30, false) != renderCPU(without, 30, false), "the particles are drawn over the title");
    Scene reopened;
    QString error;
    reopened.load(one.json(), error);
    check(reopened.layers.size() == 1 && reopened.layers[0].particlesOn && reopened.layers[0].particles == t.particles,
          "switched on, they are saved with the project");
    Layer longer = t;
    longer.text = "SPARKLING WORDS";
    check(particleIdentity(longer, one, 30) != particleIdentity(t, one, 30),
          "a longer title is a larger area for its particles, and a different picture");
  }
  {
    // Saved with the project, sliders and settings alike.
    Scene saved = scene;
    Layer l = emitter();
    applyParticlePreset(l, scene, "Petals");
    l.p[ParticleRate].set(0, 5); l.p[ParticleRate].set(60, 25);
    saved.layers = {l};
    Scene reopened; QString error;
    const bool loaded = reopened.load(saved.json(), error);
    const bool one = loaded && reopened.layers.size() == 1;
    check(one && reopened.layers[0].type == "particles" && reopened.layers[0].particles == l.particles &&
              reopened.layers[0].p[ParticleRate].keys.size() == 2 && count(reopened.layers[0], 90) == count(l, 90),
          QString("a particle layer is saved with the project and comes back the same (loaded %1 %2, type %3, settings %4, keys %5, %6 vs %7)")
              .arg(loaded).arg(error).arg(one ? reopened.layers[0].type : QString())
              .arg(one && reopened.layers[0].particles == l.particles).arg(one ? reopened.layers[0].p[ParticleRate].keys.size() : -1)
              .arg(one ? count(reopened.layers[0], 90) : -1).arg(count(l, 90)));
  }
  {
    // Every preset makes something, and makes it on screen.
    QStringList empty;
    for (const QString &name : particlePresetNames()) {
      Layer l = emitter();
      applyParticlePreset(l, scene, name);
      if (count(l, 60) == 0) empty << name;
    }
    check(empty.isEmpty(), "every preset has particles on screen two seconds in" + (empty.isEmpty() ? QString() : ": " + empty.join(", ")));
  }
  {
    // What it costs: a busy system on a full HD frame.
    Scene big; big.width = 1920; big.height = 1080; big.fps = 30; big.frames = 300;
    Layer l; l.type = "particles"; l.duration = 300;
    applyParticlePreset(l, big, "Snow");
    l.p[ParticleRate].base = 400;
    big.layers = {l};
    renderCPU(big, 100, false);
    std::vector<double> times;
    for (int i = 0; i < 5; ++i) {
      QElapsedTimer clock; clock.start();
      renderCPU(big, 100 + i, false);
      times.push_back(clock.nsecsElapsed() / 1e6);
    }
    std::sort(times.begin(), times.end());
    const int alive = count(big.layers[0], 100);
    check(alive > 2000 && times[2] < 15, QString("%1 soft particles on a 1080p frame: %2 ms").arg(alive).arg(times[2], 0, 'f', 1));
  }
  {
    // Particles in depth: a 3D card -- the character -- standing in the middle
    // of a 3D particle layer that spreads its particles front to back.
    Scene deep;
    deep.width = 640; deep.height = 360; deep.fps = 30; deep.frames = 60;
    Layer card;
    card.id = "card"; card.name = "Character"; card.type = "rect"; card.duration = 60; card.threeD = true;
    card.color = QColor(220, 30, 30);
    card.p[0].base = 320; card.p[1].base = 180;
    card.p[ShapeWidth].base = 240; card.p[ShapeHeight].base = 200;
    Layer cloud = emitter();
    cloud.id = "cloud"; cloud.name = "Cloud"; cloud.duration = 60; cloud.threeD = true;
    cloud.p[ParticleRate].base = 0; cloud.p[ParticleSpeed].base = 0; cloud.p[ParticleLife].base = 10;
    cloud.p[ParticleSize].base = 12; cloud.p[ParticleGravity].base = 0; cloud.p[ParticleWind].base = 0;
    cloud.particles["emitter"] = 2; cloud.particles["emitterWidth"] = 420; cloud.particles["emitterHeight"] = 300;
    cloud.particles["burst"] = 500; cloud.particles["shape"] = 1; cloud.particles["depth"] = 1400;
    cloud.particles["colorStart"] = "#ffffff"; cloud.particles["colorEnd"] = "#ffffff";
    Layer flat = cloud, twoD = cloud;
    flat.particles["depth"] = 0;
    twoD.threeD = false;
    check(particleVolume(cloud) && !particleVolume(flat) && !particleVolume(twoD),
          "particles stand in depth only on a 3D particle layer with Depth or Z Speed");
    // The cloud above the card in the stack: in 3D the depths decide, not the stack.
    deep.layers = {cloud, card};
    Scene cardOnly = deep, cloudOnly = deep;
    cardOnly.layers[0].visible = false;
    cloudOnly.layers[1].visible = false;
    const QImage both = renderCPU(deep, 5, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    const QImage justCard = renderCPU(cardOnly, 5, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    const QImage justCloud = renderCPU(cloudOnly, 5, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    int inFront = 0, behind = 0;
    for (int y = 110; y < 250; ++y)
      for (int x = 230; x < 410; ++x) {
        const QRgb a = both.pixel(x, y), c = justCard.pixel(x, y), p = justCloud.pixel(x, y);
        if (qAlpha(p) < 250 || qAlpha(c) < 250) continue; // a whole particle over the whole card only
        if (qGreen(a) > 200) ++inFront;      // white over the red card
        else if (qGreen(a) < 60) ++behind;   // the card's red where a particle was
      }
    check(inFront > 40 && behind > 40,
          QString("a character stands among the particles: %1 px of them in front of it, %2 px hidden behind it").arg(inFront).arg(behind));
    // A 2D layer between them in the stack -- a title, the music, one hidden
    // or not on yet -- leaves the character where its depth puts it.
    {
      Layer title; title.id = "title"; title.type = "text"; title.text = "Hi"; title.fontSize = 30; title.duration = 60;
      title.color = Qt::yellow; title.p[0].base = 40; title.p[1].base = 30;
      Layer music; music.id = "music"; music.type = "audio"; music.duration = 60;
      Layer hidden = title; hidden.id = "hidden"; hidden.visible = false;
      Layer late = title; late.id = "late"; late.start = 30;
      for (const auto &[label, between] : {std::pair<QString, Layer>{"a 2D title", title}, {"an audio layer", music},
                                           {"a hidden 2D layer", hidden}, {"a 2D layer not on yet", late}}) {
        for (bool cardOnTop : {false, true}) {
          Scene apart = deep;
          apart.layers = cardOnTop ? std::vector<Layer>{card, between, cloud} : std::vector<Layer>{cloud, between, card};
          const QImage drawn = renderCPU(apart, 5, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
          int front = 0, back = 0;
          for (int y = 110; y < 250; ++y)
            for (int x = 230; x < 410; ++x) {
              const QRgb a = drawn.pixel(x, y), c = justCard.pixel(x, y), p = justCloud.pixel(x, y);
              if (qAlpha(p) < 250 || qAlpha(c) < 250) continue;
              if (qGreen(a) > 200) ++front;
              else if (qGreen(a) < 60) ++back;
            }
          check(front == inFront && back == behind,
                QString("with %1 between them, the character %2 still stands among the particles (%3 in front, %4 behind)")
                    .arg(label, cardOnTop ? "above" : "below").arg(front).arg(back));
        }
      }
    }
    // Near ones larger than far ones: the same particles, seen through the camera.
    const auto states = particleStates(cloud, deep, 5);
    double zoom = 1;
    const QMatrix4x4 m = layerCameraMatrix(cloud, deep, 5, zoom);
    double nearest = 1e300, farthest = -1e300;
    for (const auto &particle : *states) {
      const double z = m.map(QVector3D(float(particle.position.x()), float(particle.position.y()), float(particle.z))).z();
      nearest = std::min(nearest, z); farthest = std::max(farthest, z);
    }
    check(farthest - nearest > 1000 && nearest > 1,
          QString("Depth spreads them front to back (%1 to %2 from the camera)").arg(nearest, 0, 'f', 0).arg(farthest, 0, 'f', 0));
    // Bands give the picture one pass does.
    QImage single;
    {
      RenderSchedulingScope one(0, 1);
      single = renderCPU(deep, 5, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    }
    int off = 0;
    for (int y = 0; y < single.height(); ++y)
      for (int x = 0; x < single.width(); ++x)
        if (single.pixel(x, y) != both.pixel(x, y)) ++off;
    check(off < 40, QString("in depth, drawn in bands as in one pass (%1 pixels differ)").arg(off));
  }
  {
    // Every preset starts in depth, and still fills the frame without a camera.
    Scene frame;
    frame.width = 640; frame.height = 360; frame.fps = 30; frame.frames = 300;
    QStringList flat, empty;
    for (const QString &name : particlePresetNames()) {
      Layer l;
      l.type = "particles"; l.duration = 300;
      applyParticlePreset(l, frame, name);
      if (!particleVolume(l)) flat << name;
      frame.layers = {l};
      const QImage img = renderCPU(frame, 150, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
      int drawn = 0;
      for (int y = 0; y < img.height(); y += 2)
        for (int x = 0; x < img.width(); x += 2) drawn += qAlpha(img.pixel(x, y)) > 8;
      if (drawn < 20) empty << name;
    }
    check(flat.isEmpty() && empty.isEmpty(),
          QString("every preset starts in depth and is seen (flat: %1; empty: %2)").arg(flat.join(", "), empty.join(", ")));
    Layer twice;
    twice.type = "particles"; twice.duration = 300;
    applyParticlePreset(twice, frame, "Snow");
    const double depth = particleSetting(twice, "depth");
    check(!makeParticleLayerDeep(twice, frame) && particleSetting(twice, "depth") == depth,
          "making a layer already in depth 3D again changes nothing");
  }
  return failures;
}
