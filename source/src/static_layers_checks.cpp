#include "character_animation.h"
#include "engine.h"
#include "gpu_pixels.h"
#include "gpu_renderer.h"
#include "render_workers.h"
#include <algorithm>
#include <QRandomGenerator>
#include <QSet>
#include <memory>

// Counts actual document measurements, not requests served by the cache.
quint64 textLayoutMeasurements();

int checkStaticLayerRendering(QStringList &report) {
  int failures = 0;
  auto check = [&](bool passed, const QString &label) {
    report << label + (passed ? ": PASS" : ": FAIL");
    failures += !passed;
  };
  {
    // Effect Layers. A Black & White on one takes the colour out of the layers
    // beneath it and leaves the ones above alone; it acts only within its own
    // box and its masks, at its opacity, and only while it is showing.
    Scene fx; fx.width = 400; fx.height = 240; fx.fps = 30; fx.frames = 30;
    auto rect = [](double x, double y, QColor colour) {
      Layer l; l.type = "shape"; l.shapeKind = "rect"; l.color = colour; l.duration = 30;
      l.p[0].base = x; l.p[1].base = y; l.p[ShapeWidth].base = 80; l.p[ShapeHeight].base = 80;
      return l;
    };
    Layer above = rect(200, 60, QColor(230, 30, 30));
    Layer belowLeft = rect(100, 170, QColor(230, 30, 30));
    Layer belowRight = rect(300, 170, QColor(230, 30, 30));
    Layer back; back.type = "fill"; back.color = QColor(30, 60, 200); back.duration = 30;
    back.p[0].base = 200; back.p[1].base = 120; // centred, so it really is the background
    Layer effect; effect.type = "effect"; effect.duration = 30;
    effect.p[0].base = 200; effect.p[1].base = 120;
    effect.appliedEffects["Black & White"] = true;
    auto frameWith = [&](const Layer &e, bool include = true) {
      Scene one = fx;
      one.layers = include ? std::vector<Layer>{above, e, belowLeft, belowRight, back}
                           : std::vector<Layer>{above, belowLeft, belowRight, back};
      return renderCPU(one, 5, false).convertToFormat(QImage::Format_ARGB32);
    };
    auto grey = [](QColor c) { return std::abs(c.red() - c.green()) < 12 && std::abs(c.green() - c.blue()) < 12; };
    const QImage plain = frameWith(effect, false), graded = frameWith(effect);
    check(!grey(plain.pixelColor(100, 170)), "Effect Layer fixture: the layer beneath starts in colour");
    check(grey(graded.pixelColor(100, 170)) && grey(graded.pixelColor(300, 170)),
          "Effect Layer takes the colour out of the layers beneath it");
    check(grey(graded.pixelColor(20, 20)), "Effect Layer reaches the background beneath it too");
    check(graded.pixelColor(200, 60) == plain.pixelColor(200, 60),
          "Effect Layer leaves the layer above it exactly as it was");
    Layer half = effect; half.p[5].base = .5;
    const QColor halfway = frameWith(half).pixelColor(100, 170);
    const int before = plain.pixelColor(100, 170).red() - plain.pixelColor(100, 170).green();
    const int now = halfway.red() - halfway.green();
    check(now > before / 4 && now < before * 3 / 4,
          QString("Effect Layer at half opacity gives half the change (%1 of %2)").arg(now).arg(before));
    Layer leftOnly = effect; leftOnly.p[0].base = 100; leftOnly.p[2].base = leftOnly.p[3].base = .5;
    const QImage boxed = frameWith(leftOnly);
    check(grey(boxed.pixelColor(100, 170)) && !grey(boxed.pixelColor(300, 170)),
          "Effect Layer acts only within its own box as it has been moved and scaled");
    Layer masked = effect;
    QJsonArray points;
    for (auto corner : {QPointF(-200, -120), QPointF(0, -120), QPointF(0, 120), QPointF(-200, 120)})
      points.push_back(QJsonObject{{"p", QJsonArray{corner.x(), corner.y()}},
                                   {"in", QJsonArray{0, 0}}, {"out", QJsonArray{0, 0}}});
    masked.masks = QJsonArray{QJsonObject{{"name", "Mask 1"}, {"points", points}, {"enabled", true},
                                          {"inverted", false}, {"expansion", 0}, {"feather", 0}}};
    const QImage cut = frameWith(masked);
    check(grey(cut.pixelColor(100, 170)) && !grey(cut.pixelColor(300, 170)),
          "Effect Layer acts only inside its masks");
    Layer later = effect; later.start = 20;
    Layer hidden = effect; hidden.visible = false;
    check(frameWith(later) == plain && frameWith(hidden) == plain,
          "Effect Layer changes nothing while it is not showing");
    // A blur on it reaches across the whole picture, and part of the frame (the
    // preview zoomed in) is the same as that part of the whole frame.
    Layer blurred = effect; blurred.appliedEffects = QJsonObject(); blurred.blur = 14;
    Scene soft = fx; soft.layers = {blurred, belowLeft, belowRight, back};
    const QImage whole = renderCPU(soft, 5, false).convertToFormat(QImage::Format_ARGB32);
    const QRect part(60, 110, 180, 110);
    const QImage zoomed = renderCPU(soft, 5, false, QSize(400, 240), part).convertToFormat(QImage::Format_ARGB32);
    check(zoomed == whole.copy(part), "Effect Layer: part of a frame matches that part of the whole frame");
    const QColor edge = whole.pixelColor(100, 128); // just above the shape's top edge
    check(edge != plain.pixelColor(100, 128), "Effect Layer blur softens the edges of the layers beneath");
    // It is kept when the project is saved and opened again.
    Scene saved = fx; saved.layers = {above, effect, belowLeft, back};
    Scene reopened; QString error;
    const bool loaded = reopened.load(saved.json(), error);
    check(loaded && reopened.layers.size() == 4 && reopened.layers[1].type == "effect" &&
              reopened.layers[1].appliedEffects["Black & White"].toBool(),
          "Effect Layer survives saving and opening the project");
    check(sceneUsesEffectLayers(saved, 5) && !sceneUsesEffectLayers(fx, 5),
          "a frame knows when an Effect Layer is in it");
    // Drawn in parts -- the layers beneath first, over all the cores, then the
    // effect once, then the layers above -- the frame has to be the one a
    // single pass draws. One worker forces the single pass.
    auto samePicture = [&](const Scene &scene, const QString &what) {
      clearRenderCaches();
      // Compared as it is seen, premultiplied: the colour of a pixel a blur has
      // barely reached means nothing on its own, and straight it can read as
      // anything at all.
      const QImage split = renderCPU(scene, 5, false).convertToFormat(QImage::Format_ARGB32_Premultiplied);
      QImage single;
      {
        RenderSchedulingScope one(0, 1);
        clearRenderCaches();
        single = renderCPU(scene, 5, false).convertToFormat(QImage::Format_ARGB32_Premultiplied);
      }
      int worst = 0; qint64 differing = 0;
      for (int y = 0; y < split.height(); ++y)
        for (int x = 0; x < split.width(); ++x) {
          const QRgb a = split.pixel(x, y), b = single.pixel(x, y);
          if (a == b) continue;
          ++differing;
          worst = std::max({worst, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                            std::abs(qBlue(a) - qBlue(b)), std::abs(qAlpha(a) - qAlpha(b))});
        }
      check(split.size() == single.size() && worst <= 2,
            QString("Effect Layer drawn in parts matches one pass: %1 (%2 pixels differ, worst by %3)")
                .arg(what).arg(differing).arg(worst));
    };
    auto sceneWith = [&](const Layer &e) {
      Scene one = fx; one.layers = {above, e, belowLeft, belowRight, back}; return one;
    };
    samePicture(sceneWith(effect), "grading the whole frame");
    samePicture(sceneWith(half), "at half opacity");
    samePicture(sceneWith(masked), "inside a mask");
    samePicture(sceneWith(leftOnly), "moved and scaled");
    samePicture(sceneWith(blurred), "blurring");
    // And over transparency, where a blur spreads colour into nothing.
    {
      Scene clear = fx; clear.layers = {above, blurred, belowLeft, belowRight};
      samePicture(clear, "blurring over transparency");
    }
  }
  {
    // A frame drawn from a plan has to be the frame drawn without one, pixel
    // for pixel. The scene carries what the plan has to get right: layers that
    // travel, a parent chain, a main transform, a repeater, a blur and a glow
    // that reach beyond their layer, a matte, a hidden effect, text, a group,
    // and a layer standing entirely outside the frame.
    Scene planScene; planScene.width = 640; planScene.height = 360; planScene.fps = 30; planScene.frames = 20;
    auto shape = [&](const QString &kind, double x, double y, QColor colour) {
      Layer l; l.type = "shape"; l.shapeKind = kind; l.color = colour; l.duration = 20;
      l.p[0].base = x; l.p[1].base = y;
      l.p[ShapeWidth].base = 90; l.p[ShapeHeight].base = 70;
      l.p[OuterRadius].base = 45; l.p[InnerRadius].base = 20;
      return l;
    };
    Layer moving = shape("star", 90, 90, QColor(230, 120, 60));
    moving.p[0].set(0, 40); moving.p[0].set(20, 560);
    planScene.layers.push_back(moving);
    Layer glowing = shape("circle", 320, 180, QColor(90, 200, 255));
    glowing.glow = 26; glowing.shadow = 12;
    planScene.layers.push_back(glowing);
    Layer hidden = shape("rect", 500, 90, QColor(120, 230, 140));
    hidden.blur = 30; hidden.hiddenEffects["Gaussian Blur"] = true;
    planScene.layers.push_back(hidden);
    Layer repeated = shape("rect", 120, 280, QColor(240, 210, 90));
    repeated.repeaterEnabled = true; repeated.repeaterCopies = 5;
    repeated.repeaterPositionX = 70; repeated.repeaterScale = 90;
    planScene.layers.push_back(repeated);
    Layer offstage = shape("circle", -900, 180, QColor(255, 0, 0));
    planScene.layers.push_back(offstage);
    Layer stroked = shape("rect", 430, 290, QColor(200, 120, 240));
    stroked.strokeWidth = 14; stroked.strokeColor = QColor(255, 255, 255); stroked.strokeMode = 1;
    planScene.layers.push_back(stroked);
    Layer text; text.type = "text"; text.text = "Wavy"; text.fontSize = 44; text.color = Qt::white;
    text.duration = 20; text.p[0].base = 320; text.p[1].base = 60;
    planScene.layers.push_back(text);
    // Text whose characters are animating is left out of the plan, because they
    // travel beyond the box the document reports; it has to be drawn either way.
    Layer animated = text; animated.text = "Flo"; animated.p[1].base = 330;
    animated.characterAnimation = QJsonObject{{"enabled", true}, {"delay", 2.}, {"y", 70.}, {"opacity", 0.}};
    animated.p[6].set(0, 0); animated.p[6].set(20, 100);
    planScene.layers.push_back(animated);
    Layer child = shape("rect", 0, 0, QColor(150, 150, 255));
    child.parentId = planScene.layers[0].id; child.p[0].base = 60; child.p[1].base = 60;
    child.mainScaleX = 1.4; child.mainScaleY = 1.4; child.mainRotation = 20;
    planScene.layers.push_back(child);
    Layer matte = shape("circle", 320, 300, QColor(255, 255, 255));
    planScene.layers.push_back(matte);
    Layer matted = shape("rect", 320, 300, QColor(255, 180, 0));
    matted.trackMatteId = matte.id;
    planScene.layers.push_back(matted);
    for (double frame : {0., 7., 13., 19.}) {
      clearRenderCaches();
      setCompositionPlanning(false);
      const QImage plain = renderCPU(planScene, frame, false).convertToFormat(QImage::Format_ARGB32);
      clearRenderCaches();
      setCompositionPlanning(true);
      const QImage planned = renderCPU(planScene, frame, false).convertToFormat(QImage::Format_ARGB32);
      if (plain.size() != planned.size()) { check(false, "planned frame comes back the same size"); continue; }
      qint64 differing = 0; int worst = 0;
      for (int y = 0; y < plain.height(); ++y)
        for (int x = 0; x < plain.width(); ++x) {
          const QRgb a = plain.pixel(x, y), b = planned.pixel(x, y);
          if (a == b) continue;
          ++differing;
          worst = std::max({worst, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                            std::abs(qBlue(a) - qBlue(b)), std::abs(qAlpha(a) - qAlpha(b))});
        }
      check(differing == 0,
            QString("a planned frame matches an unplanned one at frame %1 (%2 pixels differ, worst by %3)")
                        .arg(frame).arg(differing).arg(worst));
    }
    setCompositionPlanning(true);
  }
  Scene scene;
  scene.width = 1920;
  scene.height = 1080;
  scene.frames = 300;
  scene.motionBlur = false;
  Layer text;
  text.text = "Static layer regression";
  text.duration = scene.frames;
  text.p[0].base = 960;
  text.p[1].base = 540;

  auto exactBounds = [](const Layer &layer) {
    std::unique_ptr<QTextDocument> doc(textDocument(layer));
    const auto size = doc->size();
    QRectF bounds(-size.width() / 2, -size.height() / 2, size.width(), size.height());
    const double stroke = textStrokeOutwardReach(layer);
    return bounds.adjusted(-stroke, -stroke, stroke, stroke);
  };
  const QRectF expected = exactBounds(text);
  const quint64 before = textLayoutMeasurements();
  QElapsedTimer timer;
  timer.start();
  bool matching = true;
  for (int i = 0; i < 120; ++i) {
    Layer copy = text;
    copy.id = QString::number(i);
    copy.name = QString("Text %1").arg(i);
    copy.p[2].base = i + 1;
    copy.glow = i;
    matching &= layerBounds(copy, scene) == expected;
  }
  const double cachedMs = timer.nsecsElapsed() / 1e6;
  check(matching && textLayoutMeasurements() - before == 1,
        "120 duplicated text layers share one immutable document measurement");
  timer.restart();
  for (int i = 0; i < 120; ++i) matching &= exactBounds(text) == expected;
  const double uncachedMs = timer.nsecsElapsed() / 1e6;
  report << QString("120 bounds requests: cache %1 ms; rebuilding document %2 ms")
      .arg(cachedMs, 0, 'f', 3).arg(uncachedMs, 0, 'f', 3);

  for (int variation = 0; variation < 10; ++variation) {
    Layer changed = text;
    switch (variation) {
    case 0: changed.text += " wide"; break;
    case 1: changed.fontSize = 117; break;
    case 2: changed.font = "Times New Roman"; break;
    case 3: changed.bold = !changed.bold; break;
    case 4: changed.tracking = 7; break;
    case 5: changed.leading = 2; changed.text += "\nSecond line"; break;
    case 6: changed.paragraphWidth = 170; break;
    case 7: changed.align = 2; changed.paragraphWidth = 900; break;
    case 8: changed.richHtml = "<p><span style='font-size:42px'>Small</span><b> Big</b></p>"; break;
    case 9: changed.strokeWidth = 12; changed.strokeColor = Qt::red; break;
    }
    check(layerBounds(changed, scene) == exactBounds(changed),
          QString("Text measurement invalidates correctly for edit %1").arg(variation));
  }

  scene.layers = {text};
  auto &animated = scene.layers.front();
  animated.extraCurves.resize(1);
  animated.extraCurves[0].set(0, 0);
  animated.extraCurves[0].set(60, 1);
  check(!sameCompositionPixels(scene, 10, 20),
        "Animated plugin parameter invalidates shared static frames");
  animated.extraCurves[0].keys.clear();
  animated.extraCurves[0].set(9.5, 1);
  animated.extraCurves[0].set(10, 0);
  animated.extraCurves[0].set(10.5, 1);
  animated.extraCurves[0].set(19, 0);
  animated.extraCurves[0].set(21, 0);
  check(sameCompositionPixels(scene, 10, 20),
        "Equal plugin parameter samples can share frames without motion blur");
  scene.motionBlur = animated.motionBlur = true;
  check(!sameCompositionPixels(scene, 10, 20) && compositionHasMotion(scene, 10),
        "Plugin parameter shutter samples invalidate equal frame centres");
  scene.motionBlur = false;

  GpuRenderer gpu;
  const bool gpuAvailable = !gpu.render(Scene(), 0, QSize(16, 16)).isNull();
  report << gpu.description();
  for (bool onGpu : {false, true}) {
    if (onGpu && !gpuAvailable) { report << "GPU static layer benchmark unavailable"; continue; }
    for (double glow : {0., 12.}) {
      text.text = QString("Wavy-Flo %1 %2").arg(onGpu ? "GPU" : "CPU").arg(glow);
      text.glow = glow;
      text.effectOrder = glow > 0 ? QJsonArray{"Glow"} : QJsonArray();
      scene.layers.clear();
      for (int i = 0; i < 120; ++i) {
        auto copy = text;
        copy.id = QString("duplicate-%1").arg(i);
        scene.layers.push_back(copy);
      }
      auto render = [&](double frame) {
        if (onGpu) return gpu.render(scene, frame, QSize(scene.width, scene.height));
        CpuPixelScope cpuOnly;
        return renderCPU(scene, frame, false);
      };
      const auto counters = renderWorkCounters();
      timer.restart();
      const QImage first = render(0);
      const double coldMs = timer.nsecsElapsed() / 1e6;
      const auto built = effectedRasterBuilds();
      std::vector<double> times;
      QImage last;
      for (int i = 1; i <= 5; ++i) {
        timer.restart();
        last = render(i);
        times.push_back(timer.nsecsElapsed() / 1e6);
      }
      std::sort(times.begin(), times.end());
      const QString label = QString("120 static %1 text layers, Glow %2")
          .arg(onGpu ? "GPU" : "CPU").arg(glow);
      check(!first.isNull() && first == last, label + ": unchanged frames preserve pixels");
      check(effectedRasterBuilds() == built, label + ": unchanged frames reuse effects");
      report << QString("%1: cold %2 ms; warm median %3 ms; %4")
          .arg(label).arg(coldMs, 0, 'f', 3).arg(times[2], 0, 'f', 3)
          .arg(renderWorkReport(counters));
      // A real content change must not reuse the preceding completed raster.
      scene.layers.front().text += " changed";
      check(render(6) != first, label + ": changing one duplicate invalidates its pixels");
    }
  }
  {
    // A stack too deep for the cache must slow down, not stop.
    //
    // This is the shape of a real fault, found by stacking layers until the
    // preview died. A least-recently-used cache walked in the same order every
    // frame is its own worst case: by the time the last layer is drawn the
    // first has been evicted to make room, so the next frame misses on every
    // single one. The hit rate does not fall, it vanishes. Measured before the
    // fix, on large glowing layers: thirty of them 8 ms with everything cached,
    // a hundred and twenty 1060 ms with nothing cached at all. A cliff.
    Scene deep;
    deep.width = 960;
    deep.height = 540;
    deep.frames = 60;
    deep.motionBlur = false;
    for (int n = 0; n < 60; ++n) {
      Layer l;
      l.id = QString("cliff-%1").arg(n);
      l.name = l.id;
      l.text = QString("Layer %1").arg(n);
      l.fontSize = 90;
      l.glow = 10;
      l.appliedEffects["Glow"] = true;
      l.duration = 60;
      l.p[0].base = 200 + (n % 6) * 110;
      l.p[1].base = 120 + (n % 5) * 80;
      deep.layers.push_back(l);
    }
    const int restore = rasterCacheBudget();
    // Deliberately far too small for sixty glowing layers.
    setRasterCacheBudget(4096);
    renderCPU(deep, 0, false, QSize(960, 540));
    renderCPU(deep, 0, false, QSize(960, 540));
    const auto before = renderWorkCounters();
    renderCPU(deep, 0, false, QSize(960, 540));
    const auto after = renderWorkCounters();
    const quint64 hits = after.effectHits - before.effectHits;
    check(hits > 0,
          QString("a stack too big for the cache still keeps what fits: %1 of %2 layers hit")
              .arg(hits)
              .arg(deep.layers.size()));

    // And with room, everything stays.
    setRasterCacheBudget(restore);
    renderCPU(deep, 0, false, QSize(960, 540));
    const auto roomy = renderWorkCounters();
    renderCPU(deep, 0, false, QSize(960, 540));
    const auto settled = renderWorkCounters();
    const quint64 all = settled.effectHits - roomy.effectHits;
    check(all >= quint64(deep.layers.size()),
          QString("and with room for them, every layer is served from it: %1 of %2")
              .arg(all)
              .arg(deep.layers.size()));
    check(rasterCacheBudget() >= 131072,
          QString("the budget is at least the old fixed figure: %1 KiB").arg(rasterCacheBudget()));
  }
  {
    // Which frames may be cut into bands across cores, and which must not.
    //
    // Getting this wrong in one direction leaves nineteen cores idle on a
    // hundred plain text layers; getting it wrong in the other turned a 130 ms
    // frame of glowing layers into forty seconds, because every band recomputes
    // the padded neighbourhood of every effect for its own slice. So the rule
    // is stated here rather than left to be inferred from a stopwatch.
    const QSize full(1920, 1080);
    auto sceneOf = [&](int count, double fontSize, double glow) {
      Scene made;
      made.width = full.width();
      made.height = full.height();
      made.frames = 60;
      made.motionBlur = false;
      for (int n = 0; n < count; ++n) {
        Layer l;
        l.id = QString("band-%1").arg(n);
        l.text = QString("Layer %1").arg(n);
        l.fontSize = fontSize;
        l.duration = 60;
        l.glow = glow;
        if (glow > 0) l.appliedEffects["Glow"] = true;
        l.p[0].base = 300 + (n % 6) * 240;
        l.p[1].base = 120 + (n % 5) * 160;
        made.layers.push_back(l);
      }
      return made;
    };
    check(compositionBands(sceneOf(100, 72, 0), full, 0),
          "a hundred small text layers divide across cores");
    Scene characterStroke = sceneOf(1, 96, 0);
    characterStroke.layers[0].text = "Character stroke multicore probe";
    characterStroke.layers[0].textStrokes = {{5, QColor("#e43b35"), 1}};
    syncLegacyTextStroke(characterStroke.layers[0]);
    check(compositionBands(characterStroke, full, 0),
          "a Character stroke keeps the fast composition path divisible across cores");
    const auto coldBuilds = effectedRasterBuilds();
    renderCPU(characterStroke, 0, false, full);
    const auto builtOnce = effectedRasterBuilds() - coldBuilds;
    check(builtOnce == 1,
          QString("all CPU bands share one freshly-built Character stroke raster: %1 build")
              .arg(builtOnce));
    characterStroke.layers[0].p[0].base += 40;
    const auto beforeMove = effectedRasterBuilds();
    renderCPU(characterStroke, 0, false, full);
    check(effectedRasterBuilds() == beforeMove,
          "moving Character-stroked text reuses its completed raster");
    {
      // A glow on them once stopped the split, a band being thought to work the
      // glow out again for its own rows. The glowed raster is built once and
      // shared by every band now, so the split stays -- and has to draw exactly
      // what one pass draws.
      const Scene glowing = sceneOf(100, 72, 12);
      clearRenderCaches();
      const QImage banded = renderCPU(glowing, 0, false, full).convertToFormat(QImage::Format_ARGB32);
      inRenderWorker = true; // one pass, no bands
      clearRenderCaches();
      const QImage serial = renderCPU(glowing, 0, false, full).convertToFormat(QImage::Format_ARGB32);
      inRenderWorker = false;
      int differing = 0, worst = 0;
      for (int y = 0; y < std::min(banded.height(), serial.height()); ++y)
        for (int x = 0; x < std::min(banded.width(), serial.width()); ++x) {
          const QRgb a = banded.pixel(x, y), b = serial.pixel(x, y);
          const int d = std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                                  std::abs(qBlue(a) - qBlue(b)), std::abs(qAlpha(a) - qAlpha(b))});
          differing += d > 0;
          worst = std::max(worst, d);
        }
      check(compositionBands(glowing, full, 0) && banded.size() == serial.size() && worst <= 1,
            QString("glowing layers split across cores draw what one pass draws (%1 pixels differ, worst by %2)")
                .arg(differing).arg(worst));
    }
    check(compositionBands(Scene(), full, 0), "an empty composition divides trivially");

    // A layer too large to be cached whole is clipped to whoever asks for it,
    // so each band would rasterise its own piece of the same text.
    Scene huge = sceneOf(1, 72, 0);
    huge.layers[0].p[2].base = huge.layers[0].p[3].base = 24;
    check(!compositionBands(huge, full, 0),
          "one layer too big to cache whole stops it too");
    // The same layer at its own size is small enough again.
    huge.layers[0].p[2].base = huge.layers[0].p[3].base = 1;
    check(compositionBands(huge, full, 0), "and at its own size it does not");

    // An image layer carries no rasterising of its own, so it never stops it.
    Scene footage;
    footage.width = full.width();
    footage.height = full.height();
    Layer picture;
    picture.type = "image";
    picture.image = QImage(64, 64, QImage::Format_RGBA8888);
    picture.image.fill(Qt::white);
    picture.duration = 60;
    footage.layers.push_back(picture);
    check(compositionBands(footage, full, 0), "footage divides across cores");

    // A highly magnified vector is deliberately not divided at composition
    // level: every output band would otherwise rebuild the same source. Its
    // fast source raster has a separate row-strip path, which must preserve the
    // exact serial pixels while using the worker pool once.
    Scene magnified = sceneOf(1, 80, 0);
    magnified.layers[0].text = "Wavy-Flo";
    magnified.layers[0].textStrokes = {{12, QColor("#df3340"), 1}};
    syncLegacyTextStroke(magnified.layers[0]);
    magnified.layers[0].p[2].base = magnified.layers[0].p[3].base = 24;
    magnified.layers[0].p[0].base = full.width()/2.;
    magnified.layers[0].p[1].base = full.height()/2.;
    magnified.layers[0].hiddenEffects["_worker_test_salt"] = "serial";
    QElapsedTimer serialClock; serialClock.start();
    QImage serial;
    {
      RenderSchedulingScope oneWorker(0,1);
      serial = renderCPU(magnified,0,false,full);
    }
    const double serialMs=serialClock.nsecsElapsed()/1e6;
    magnified.layers[0].hiddenEffects["_worker_test_salt"] = "parallel";
    QElapsedTimer parallelClock; parallelClock.start();
    const auto parallel=renderCPU(magnified,0,false,full);
    const double parallelMs=parallelClock.nsecsElapsed()/1e6;
    const auto serialPixels=serial.convertToFormat(QImage::Format_RGBA8888);
    const auto parallelPixels=parallel.convertToFormat(QImage::Format_RGBA8888);
    qint64 changedPixels=0;
    for(int y=0;y<serialPixels.height();++y) {
      const auto *a=serialPixels.constScanLine(y);
      const auto *b=parallelPixels.constScanLine(y);
      for(int x=0;x<serialPixels.width();++x)
        if(std::memcmp(a+x*4,b+x*4,4)!=0) ++changedPixels;
    }
    // Qt's scan converter may choose the neighbouring coverage pixel on a
    // long diagonal when its paint device has strip rather than frame height.
    // It is acceptable only at a sub-promille of output pixels; a horizontal
    // tile seam or coordinate error is orders of magnitude larger.
    check(!serial.isNull() && changedPixels<=qint64(full.width())*full.height()/1000,
          QString("magnified Character stroke row workers preserve the serial image: %1 edge pixels differ")
              .arg(changedPixels));
    report << QString("Magnified Character stroke source: serial %1 ms; multicore %2 ms")
                  .arg(serialMs,0,'f',3).arg(parallelMs,0,'f',3);
  }
  {
    // Letters: a background behind each letter, and another layer's picture in them.
    Scene scene;
    scene.width = 640; scene.height = 360; scene.fps = 30; scene.frames = 60;
    scene.background = Qt::transparent;
    Layer words;
    words.id = "words"; words.type = "text"; words.text = "WAVY"; words.fontSize = 110; words.bold = true;
    words.color = Qt::white; words.duration = 60;
    words.p[0].base = 320; words.p[1].base = 180;
    Layer source;
    source.id = "texture"; source.name = "Texture"; source.type = "rect"; source.duration = 60;
    source.color = QColor(20, 200, 60); source.visible = false; // hidden: a texture all the same
    source.p[0].base = 320; source.p[1].base = 180;
    source.p[ShapeWidth].base = 300; source.p[ShapeHeight].base = 200;
    auto render = [&](const std::vector<Layer> &layers) {
      scene.layers = layers;
      clearRenderCaches();
      return renderCPU(scene, 10, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    };
    auto count = [](const QImage &img, auto test) {
      int n = 0;
      for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) n += test(img.pixel(x, y)) ? 1 : 0;
      return n;
    };
    auto red = [](QRgb c) { return qAlpha(c) > 200 && qRed(c) > 180 && qGreen(c) < 80 && qBlue(c) < 80; };
    auto green = [](QRgb c) { return qAlpha(c) > 200 && qGreen(c) > 150 && qRed(c) < 90; };
    auto white = [](QRgb c) { return qAlpha(c) > 200 && qRed(c) > 220 && qGreen(c) > 220 && qBlue(c) > 220; };
    const QImage plain = render({words, source});
    Layer backed = words;
    backed.surfaceEffects["Letters"] = QJsonObject{{"background", true}, {"color", "#e02020"}, {"padding", 12}};
    const QImage withBackground = render({backed, source});
    const int reds = count(withBackground, red), letters = count(withBackground, white);
    check(count(plain, red) == 0 && reds > 2000 && letters > 1000,
          QString("each letter stands on a background of its own (%1 px of background, %2 px of letters over it)").arg(reds).arg(letters));
    check(layerBounds(backed, scene).width() > layerBounds(words, scene).width(),
          "the layer's box grows to hold the backgrounds");
    Layer textured = words;
    textured.surfaceEffects["Letters"] = QJsonObject{{"letterTexture", "texture"}};
    const QImage filled = render({textured, source});
    check(count(filled, green) > 1000 && count(filled, white) < 50,
          QString("a hidden layer's picture fills the letters (%1 px of it)").arg(count(filled, green)));
    // The texture's layer changes: the letters change with it, not a cached picture of before.
    source.color = QColor(30, 60, 230);
    scene.layers = {textured, source};
    const QImage recoloured = renderCPU(scene, 10, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    check(count(recoloured, [](QRgb c) { return qAlpha(c) > 200 && qBlue(c) > 180 && qRed(c) < 90; }) > 1000,
          "changing the texture's layer changes the letters wearing it");
    // Animated characters carry their backgrounds with them.
    Layer moving = backed;
    moving.characterAnimation = QJsonObject{{"enabled", true}};
    check(count(render({moving, source}), red) > 2000, "animated letters keep their backgrounds");
    // Stroked letters with backgrounds: letters over their stroke over the
    // backgrounds, and the stroke the one they have without backgrounds --
    // built from the letters' distance. Drawn with a wide pen instead, it
    // turned a "W" into a pentagon and let the backgrounds through holes in
    // it. Wherever the letters and stroke cover the picture without
    // backgrounds, it is the same with them; no stroke goes round a
    // background; and past the stroke, the backgrounds show.
    {
      auto underStroke = [&](Layer without, int width, int padding, const QString &what) {
        without.textStrokes = {{double(width), QColor(40, 60, 230), 1}};
        Layer with = without;
        with.surfaceEffects["Letters"] = QJsonObject{{"background", true}, {"color", "#e02020"}, {"padding", padding}};
        const QImage a = render({without, source}), b = render({with, source});
        int covered = 0, differ = 0, strokedRound = 0;
        for (int y = 0; y < a.height(); ++y)
          for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            if (qAlpha(p) == 255) {
              ++covered;
              differ += std::abs(qRed(p) - qRed(q)) > 3 || std::abs(qGreen(p) - qGreen(q)) > 3 ||
                        std::abs(qBlue(p) - qBlue(q)) > 3 || qAlpha(q) != 255;
            } else if (qAlpha(p) == 0 && qAlpha(q) > 0 && qBlue(q) > qRed(q)) ++strokedRound;
          }
        const int shown = count(b, red);
        check(covered > 10000 && differ == 0 && strokedRound == 0 && shown > 0,
              QString("%1: the letters over their stroke over the backgrounds, the stroke as without them "
                      "(%2 of %3 px differ, %4 px of stroke round the backgrounds, %5 px of background past it)")
                  .arg(what).arg(differ).arg(covered).arg(strokedRound).arg(shown));
        return shown;
      };
      underStroke(words, 36, 40, "a wide stroke");
      Layer animated = words;
      animated.characterAnimation = QJsonObject{{"enabled", true}};
      underStroke(animated, 36, 40, "a wide stroke on animated letters");
      check(underStroke(words, 4, 24, "a thin stroke") > 3000, "and the backgrounds show past a thin one");
      // Large enough to be painted in strips (a raster of a megapixel or
      // more), animated letters keep their backgrounds: every strip once set
      // and cleared the collection's place for them as it painted, and the
      // backgrounds of a large title came and went, mostly went. Part way in,
      // with letters and backgrounds blurred and finished apart, and settled;
      // as many of them show past a thin stroke as with no stroke at all.
      Layer big = words;
      big.surfaceEffects["Letters"] = QJsonObject{{"background", true}, {"color", "#e02020"}, {"padding", 30}};
      big.characterAnimation = QJsonObject{{"enabled", true}, {"y", 60}, {"opacity", 100}, {"blur", 12}, {"delay", 3}};
      big.p[6].set(0, 0);
      big.p[6].set(20, 100);
      Layer stroked = big;
      stroked.textStrokes = {{6., QColor(40, 60, 230), 1}};
      auto reddish = [](QRgb c) { return qAlpha(c) > 40 && qRed(c) > 2 * qGreen(c) && qRed(c) > 2 * qBlue(c); };
      for (int frame : {14, 50}) {
        int shown[2];
        for (int k = 0; k < 2; ++k) {
          scene.layers = {k ? stroked : big, source};
          clearRenderCaches();
          shown[k] = count(renderCPU(scene, frame, false, QSize(2560, 1440)).convertToFormat(QImage::Format_ARGB32), reddish);
        }
        check(shown[0] > 40000 && shown[1] > shown[0] / 2,
              QString("a stroked title painted in strips keeps its letters' backgrounds at frame %1 "
                      "(%2 px of them, %3 with no stroke)").arg(frame).arg(shown[1]).arg(shown[0]));
      }
      // Letters fading in together fade as one picture with their strokes and
      // backgrounds: under strokes that hide the backgrounds once the letters
      // are in, the backgrounds never show through while they come in. Laid
      // under the faded letters as they were, they did, and went again as the
      // letters settled. Sharp and blurred, part way in and settled, the frame
      // is the one with no backgrounds at all.
      Layer fading = words;
      fading.textStrokes = {{60., QColor(40, 60, 230), 1}};
      fading.p[6].set(0, 0);
      fading.p[6].set(20, 100);
      for (const double blur : {0., 6.}) {
        fading.characterAnimation = QJsonObject{{"enabled", true}, {"y", 0}, {"opacity", 0}, {"delay", 0}, {"blur", blur}};
        Layer hiding = fading;
        hiding.surfaceEffects["Letters"] = QJsonObject{{"background", true}, {"color", "#e02020"}, {"padding", 0}};
        for (int frame : {10, 40}) {
          QImage seen[2];
          for (int k = 0; k < 2; ++k) {
            scene.layers = {k ? hiding : fading, source};
            clearRenderCaches();
            seen[k] = renderCPU(scene, frame, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
          }
          int differ = 0;
          for (int y = 0; y < seen[0].height(); ++y)
            for (int x = 0; x < seen[0].width(); ++x) {
              const QRgb p = seen[0].pixel(x, y), q = seen[1].pixel(x, y);
              differ += std::abs(qRed(p) - qRed(q)) > 2 || std::abs(qGreen(p) - qGreen(q)) > 2 ||
                        std::abs(qBlue(p) - qBlue(q)) > 2 || std::abs(qAlpha(p) - qAlpha(q)) > 2;
            }
          check(differ == 0, QString("%1 letters fading in together hide the backgrounds under their strokes "
                                     "(frame %2: %3 px differ from no backgrounds)")
                                 .arg(blur > 0 ? "blurred" : "sharp").arg(frame).arg(differ));
        }
      }
      // An animated title with no strokes, painted in strips: each blurred
      // letter is drawn and blurred once a frame, not once a strip -- ten
      // strips of a title at 900% did all of them ten times, 245 ms a frame.
      Layer soft = words;
      soft.surfaceEffects["Letters"] = QJsonObject{{"background", true}, {"color", "#e02020"}, {"padding", 10}};
      soft.characterAnimation = QJsonObject{{"enabled", true}, {"y", 0}, {"opacity", 100}, {"blur", 20}, {"delay", 0}};
      soft.p[6].set(0, 0);
      soft.p[6].set(20, 100);
      scene.layers = {soft, source};
      clearRenderCaches();
      const quint64 before = blurredLetterPictures();
      const QImage strips = renderCPU(scene, 10, false, QSize(2560, 1440)).convertToFormat(QImage::Format_ARGB32);
      const quint64 made = blurredLetterPictures() - before;
      const int shownSoft = count(strips, reddish);
      check(made == 4 && shownSoft > 40000,
            QString("a blurred title with no strokes painted in strips blurs each letter once (%1 pictures for 4 "
                    "letters, %2 px of backgrounds)").arg(made).arg(shownSoft));
      // The particles a stroked title gives off show, over its letters and
      // strokes, as over a title with no strokes: its raster paints the
      // letters alone, for the strokes to be built round them after, and
      // drew no particles at all. No stroke goes round a particle -- it is
      // not a letter. Small and painted in strips, still and animated.
      Layer sparkling = words;
      sparkling.particlesOn = true;
      sparkling.particles = QJsonObject{{"emitter", 5}, {"shape", 1}, {"colorStart", "#20e040"}, {"colorEnd", "#20e040"},
                                        {"sizeVariance", 0}, {"fadeIn", 0}, {"fadeOut", 0}, {"preroll", 2}, {"seed", 5}};
      sparkling.p[ParticleRate].base = 60; sparkling.p[ParticleLife].base = 3; sparkling.p[ParticleSpeed].base = 0;
      sparkling.p[ParticleSize].base = 14; sparkling.p[ParticleGravity].base = 0; sparkling.p[ParticleTurbulence].base = 0;
      auto particle = [](QRgb c) { return qAlpha(c) > 200 && qGreen(c) > 180 && qRed(c) < 90 && qBlue(c) < 120; };
      auto bluish = [](QRgb c) { return qAlpha(c) > 0 && qBlue(c) > qGreen(c) + 40 && qBlue(c) > qRed(c) + 40; };
      for (const bool moving : {false, true})
        for (const QSize size : {QSize(640, 360), QSize(2560, 1440)}) {
          if (moving && size.width() > 640) continue;
          Layer plain = sparkling;
          if (moving) plain.characterAnimation = QJsonObject{{"enabled", true}};
          Layer stroked = plain;
          stroked.textStrokes = {{6., QColor(40, 60, 230), 1}};
          Layer bare = stroked;
          bare.particlesOn = false;
          QImage seen[3];
          int k = 0;
          for (const Layer &layer : {plain, stroked, bare}) {
            scene.layers = {layer};
            clearRenderCaches();
            seen[k++] = renderCPU(scene, 10, false, size).convertToFormat(QImage::Format_ARGB32);
          }
          const int expected = count(seen[0], particle), shown = count(seen[1], particle);
          int over = 0, strokedRound = 0;
          for (int y = 0; y < seen[2].height(); ++y)
            for (int x = 0; x < seen[2].width(); ++x) {
              const QRgb p = seen[2].pixel(x, y), q = seen[1].pixel(x, y);
              if (qAlpha(p) == 255 && particle(q)) ++over;
              else if (qAlpha(p) == 0 && bluish(q)) ++strokedRound;
            }
          check(expected > 2000 && shown > expected * 9 / 10 && over > 200 && strokedRound == 0,
                QString("%1 stroked title at %2 x %3 shows its particles over its letters and strokes (%4 px of them, "
                        "%5 with no stroke, %6 over the letters and strokes, %7 px of stroke round them)")
                    .arg(moving ? "an animated" : "a still").arg(size.width()).arg(size.height())
                    .arg(shown).arg(expected).arg(over).arg(strokedRound));
        }
    }
    // Keyed Letters numbers: padding from 0 to 40 grows the backgrounds between the keys.
    Layer keyed = backed;
    const int curve = layerAddExtra(keyed, "Letters/padding", 0);
    keyed.at(curve).set(0, 0);
    keyed.at(curve).set(30, 40);
    scene.layers = {keyed, source};
    clearRenderCaches();
    const int early = count(renderCPU(scene, 0, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32), red);
    const int late = count(renderCPU(scene, 30, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32), red);
    check(late > early + 3000, QString("keyed padding grows the backgrounds (%1 px at the first key, %2 at the last)").arg(early).arg(late));
    // Keyed colour: red at the first key, blue at the last, three curves together.
    Layer tinted = backed;
    const int channels[2][3]{{224, 32, 32}, {30, 60, 230}};
    const char *names[3]{"Letters/colorR", "Letters/colorG", "Letters/colorB"};
    for (int k = 0; k < 3; ++k) {
      const int c = layerAddExtra(tinted, names[k], channels[0][k]);
      tinted.at(c).set(0, channels[0][k]);
      tinted.at(c).set(30, channels[1][k]);
    }
    scene.layers = {tinted, source};
    clearRenderCaches();
    auto blue = [](QRgb c) { return qAlpha(c) > 200 && qBlue(c) > 180 && qRed(c) < 90; };
    const QImage first = renderCPU(scene, 0, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    const QImage last = renderCPU(scene, 30, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    check(count(first, red) > 2000 && count(last, blue) > 2000 && count(last, red) == 0,
          "a keyed background color goes from one key's color to the next's");
    // Keyed shape: fitted boxes at the first key, circles at the last, a shape between.
    Layer shaped = backed;
    const int shapeCurve = layerAddExtra(shaped, "Letters/shape", 0);
    shaped.at(shapeCurve).set(0, 0);
    shaped.at(shapeCurve).set(30, 2);
    scene.layers = {shaped, source};
    clearRenderCaches();
    const int fitted = count(renderCPU(scene, 0, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32), red);
    const int between = count(renderCPU(scene, 22, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32), red);
    const int circles = count(renderCPU(scene, 30, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32), red);
    check(fitted != circles && between != fitted && between != circles,
          QString("a keyed shape changes from one shape into the next (%1, %2, %3 px)").arg(fitted).arg(between).arg(circles));
    // Keyed letter texture: None at the first key, the texture's layer at the
    // last, the two crossfading between.
    Layer switching = words;
    switching.surfaceEffects["Letters"] = QJsonObject{{"letterTextureList", QJsonArray{QString(), QString("texture")}}};
    const int textureCurve = layerAddExtra(switching, "Letters/letterTexture", 0);
    switching.at(textureCurve).set(0, 0);
    switching.at(textureCurve).set(30, 1);
    scene.layers = {switching, source};
    clearRenderCaches();
    auto bluish = [](QRgb c) { return qAlpha(c) > 200 && qBlue(c) > 180 && qRed(c) < 90; };
    const QImage own = renderCPU(scene, 0, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    const QImage mid = renderCPU(scene, 15, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    const QImage worn = renderCPU(scene, 30, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    check(count(own, white) > 1000 && count(worn, bluish) > 1000 && count(mid, white) < 50 && count(mid, bluish) < 50,
          QString("a keyed letter texture crossfades from the letters' own colour to the texture (%1 white, then %2 textured)")
              .arg(count(own, white)).arg(count(worn, bluish)));
    // A background for each word, or each line: a word's box covers the gaps
    // between its letters, a line's the gaps between its words as well.
    Layer phrase = words;
    phrase.text = "HI HI";
    auto backgroundPixels = [&](int per) {
      Layer l = phrase;
      l.surfaceEffects["Letters"] =
          QJsonObject{{"background", true}, {"color", "#e02020"}, {"padding", 0}, {"round", 0}, {"per", per}};
      return count(render({l, source}), red);
    };
    const int perLetter = backgroundPixels(0), perWord = backgroundPixels(1), perLine = backgroundPixels(2);
    check(perLetter > 1000 && perWord > perLetter && perLine > perWord,
          QString("backgrounds per letter, per word and per line cover more each (%1, %2, %3 px)")
              .arg(perLetter).arg(perWord).arg(perLine));
    // The texture's picture in every background, in its own shape: a disc puts
    // a disc behind each letter, its corners left open; across the boxes it is
    // one picture, cut square by each box.
    Layer disc;
    disc.id = "disc"; disc.type = "circle"; disc.duration = 60; disc.color = QColor(20, 200, 60); disc.visible = false;
    disc.p[0].base = 320; disc.p[1].base = 180;
    Layer across = words;
    const QJsonObject acrossSettings{{"background", true}, {"color", "#e02020"}, {"padding", 6}, {"backgroundTexture", "disc"}};
    across.surfaceEffects["Letters"] = acrossSettings;
    Layer each = words;
    QJsonObject eachSettings = acrossSettings;
    eachSettings["textureEach"] = true;
    each.surfaceEffects["Letters"] = eachSettings;
    const QImage acrossPicture = render({across, disc}), eachPicture = render({each, disc});
    const int acrossGreen = count(acrossPicture, green), eachGreen = count(eachPicture, green);
    check(eachGreen > 1000 && eachGreen < acrossGreen * 95 / 100 && count(eachPicture, red) == 0,
          QString("Each puts the texture's own shape in every background (%1 px across the boxes, %2 px in discs)")
              .arg(acrossGreen).arg(eachGreen));
    // Animated characters carry their copies with them.
    Layer eachMoving = each;
    eachMoving.characterAnimation = QJsonObject{{"enabled", true}};
    check(count(render({eachMoving, disc}), green) > 1000, "animated letters keep their own copies of the texture");
    // A copy laid larger than a whole line's box: the layer's box makes room for it.
    Layer larger = each;
    QJsonObject largerSettings = eachSettings;
    largerSettings["per"] = 2;
    largerSettings["textureScale"] = 200;
    larger.surfaceEffects["Letters"] = largerSettings;
    check(layerBounds(larger, scene).width() > layerBounds(each, scene).width() + 100,
          "a texture laid larger than a line's box widens the layer's box for it");
  }
  {
    // Italic and the letters' case, measured on what is drawn.
    Scene scene;
    scene.width = 640; scene.height = 360; scene.fps = 30; scene.frames = 60;
    scene.background = Qt::transparent;
    auto ink = [&](const Layer &l) {
      scene.layers = {l};
      clearRenderCaches();
      const QImage img = renderCPU(scene, 0, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
      QRect box;
      for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
          if (qAlpha(img.pixel(x, y)) > 128) box = box.united(QRect(x, y, 1, 1));
      return std::make_pair(img, box);
    };
    // How far the top of the letters stands to the right of their foot: about
    // nothing upright, plainly more slanted.
    auto lean = [](const QImage &img, const QRect &box) {
      auto leftmost = [&](int y) {
        for (int x = box.left(); x <= box.right(); ++x)
          if (qAlpha(img.pixel(x, y)) > 128) return x;
        return box.right();
      };
      return leftmost(box.top() + box.height() / 6) - leftmost(box.bottom() - box.height() / 6);
    };
    Layer word;
    word.id = "word"; word.type = "text"; word.text = "IHI"; word.fontSize = 120; word.bold = false;
    word.color = Qt::white; word.duration = 60; word.p[0].base = 320; word.p[1].base = 180;
    for (const QString family : {QString("Arial"), QString("Impact")}) {
      if (!QFontDatabase::families().contains(family)) continue;
      word.font = family;
      word.fauxItalic = false;
      const auto [uprightImage, upright] = ink(word);
      word.fauxItalic = true;
      const auto [slantedImage, slanted] = ink(word);
      const int before = lean(uprightImage, upright), after = lean(slantedImage, slanted);
      check(std::abs(before) <= 3 && after >= 10,
            QString("%1 in faux italic leans (%2 px upright, %3 px slanted)").arg(family).arg(before).arg(after));
      // Faux bold: more ink in every letter, the face the same.
      word.fauxItalic = false;
      auto inked = [](const QImage &img) {
        int n = 0;
        for (int y = 0; y < img.height(); ++y)
          for (int x = 0; x < img.width(); ++x) n += qAlpha(img.pixel(x, y)) > 128 ? 1 : 0;
        return n;
      };
      const int thin = inked(uprightImage);
      word.fauxBold = true;
      const int thick = inked(ink(word).first);
      word.fauxBold = false;
      check(thick > thin * 11 / 10, QString("%1 in faux bold is thicker (%2 px of ink, %3 px bold)").arg(family).arg(thin).arg(thick));
    }
    // A face of the family by its name: Segoe UI's Light and Black, far apart in weight.
    if (QFontDatabase::families().contains("Segoe UI") && QFontDatabase::styles("Segoe UI").contains("Light") &&
        QFontDatabase::styles("Segoe UI").contains("Black")) {
      auto inkOf = [&](const QString &style) {
        Layer l = word;
        l.font = "Segoe UI";
        l.fontStyle = style;
        const QImage img = ink(l).first;
        int n = 0;
        for (int y = 0; y < img.height(); ++y)
          for (int x = 0; x < img.width(); ++x) n += qAlpha(img.pixel(x, y)) > 128 ? 1 : 0;
        return n;
      };
      const int light = inkOf("Light"), black = inkOf("Black");
      check(black > light * 13 / 10, QString("a font style chosen by name sets that face (Light %1 px of ink, Black %2)").arg(light).arg(black));
    }
    word.font = "Arial";
    if (QFontDatabase::families().contains("Arial")) {
      word.text = "ace";
      const int asTyped = ink(word).second.height();
      word.capitals = 1;
      const int allCaps = ink(word).second.height();
      word.capitals = 2;
      const int smallCaps = ink(word).second.height();
      word.text = "ACE";
      word.capitals = 3;
      const int lowered = ink(word).second.height();
      check(allCaps > asTyped + 15 && smallCaps < allCaps && lowered <= asTyped + 2,
            QString("the letters' case: all caps taller than typed lowercase, small caps lower, \"ACE\" in lowercase as low "
                    "(%1, %2, %3, %4 px)").arg(asTyped).arg(allCaps).arg(smallCaps).arg(lowered));
    }
    // A leading far under 1: the line is shorter than the letters standing on
    // it, and none of them is cut -- a single line shows all its ink either way.
    {
      auto inkOf = [&](double leading) {
        Layer l = word;
        l.text = "Wavy-Flo";
        l.capitals = 0;
        l.leading = leading;
        const QImage img = ink(l).first;
        int n = 0;
        for (int y = 0; y < img.height(); ++y)
          for (int x = 0; x < img.width(); ++x) n += qAlpha(img.pixel(x, y)) > 128 ? 1 : 0;
        return n;
      };
      const int normal = inkOf(1.2), tight = inkOf(.2);
      check(normal > 1000 && std::abs(tight - normal) <= normal / 50,
            QString("a tight leading cuts no letter (%1 px of ink at 1.2, %2 at 0.2)").arg(normal).arg(tight));
    }
    // The face, the faux styles and the case are kept in the project.
    Scene saved;
    Layer kept = word;
    kept.fontStyle = "Light";
    kept.fauxBold = true;
    kept.fauxItalic = true;
    kept.capitals = 2;
    saved.layers = {kept};
    Scene loaded;
    QString error;
    loaded.load(saved.json(), error);
    check(!loaded.layers.empty() && loaded.layers[0].fontStyle == "Light" && loaded.layers[0].fauxBold &&
              loaded.layers[0].fauxItalic && loaded.layers[0].capitals == 2,
          "the font style, faux bold and italic and the letters' case are saved and read back with the project");
  }
  {
    // Random and ZigZag give each letter one move -- Position or Scale, never
    // both. Random deals them in shuffled hands: every four letters take up,
    // down, to the side and Scale once each (A up, B down, C scaled, D to the
    // side), each at least half as far as asked; Rotation is a gacha that
    // lands on letters of any move.
    Layer text; text.type = "text"; text.text = "ABCDEFGHIJKLMNOPQRST"; text.duration = 60;
    text.characterAnimation = QJsonObject{{"enabled", true}, {"mode", 1}, {"seed", 1}, {"x", 200}, {"y", 120},
                                          {"scale", 250}, {"rotation", 90}};
    text.p[6].set(0, 0); text.p[6].set(30, 100);
    enum { Up, Down, Side, Grow, Both, None };
    auto moveOf = [](const CharacterPose &pose) {
      const bool moved = std::abs(pose.offset.x()) > 1e-6 || std::abs(pose.offset.y()) > 1e-6;
      const bool grown = std::abs(pose.scale - 1) > 1e-6;
      if (moved && grown) return int(Both);
      if (grown) return int(Grow);
      if (!moved) return int(None);
      if (std::abs(pose.offset.y()) < 1e-6) return int(Side);
      return std::abs(pose.offset.x()) < 1e-6 ? (pose.offset.y() < 0 ? int(Up) : int(Down)) : int(Both);
    };
    bool oneEach = true, everyHand = true, farEnough = true;
    int turned = 0;
    QSet<int> turnedMoves;
    for (int hand = 0; hand < 5; ++hand) {
      QSet<int> dealt;
      for (int rank = hand * 4; rank < hand * 4 + 4; ++rank) {
        const CharacterPose pose = characterPose(text, 0, rank, 20);
        const int move = moveOf(pose);
        if (move == Both || move == None) oneEach = false;
        dealt.insert(move);
        const double x = std::abs(pose.offset.x()), y = std::abs(pose.offset.y());
        if ((move == Side && (x < 100 - 1e-6 || x > 200 + 1e-6)) ||
            ((move == Up || move == Down) && (y < 60 - 1e-6 || y > 120 + 1e-6)) ||
            (move == Grow && (pose.scale < 1.75 - 1e-6 || pose.scale > 2.5 + 1e-6)))
          farEnough = false;
        if (std::abs(pose.rotation) > 1e-6) {
          ++turned;
          turnedMoves.insert(move);
        }
      }
      if (dealt != QSet<int>{Up, Down, Side, Grow}) everyHand = false;
    }
    check(oneEach && everyHand && farEnough,
          "Random gives each letter one move -- up, down, to the side or Scale -- every four letters each once, "
          "at least half as far as asked");
    check(turned > 0 && turned < 20 && turnedMoves.size() >= 3,
          QString("Random turns some letters, of every move, and not others (%1 of 20 turn)").arg(turned));
    // What is typed for Rotation is how much; the way is each letter's. A Scale
    // keeps its way: at 250% the scaled letters only grow.
    int grew = 0, shrank = 0, clockwise = 0, anticlockwise = 0;
    for (int rank = 0; rank < 40; ++rank) {
      const CharacterPose pose = characterPose(text, 0, rank, 40);
      if (pose.offset.manhattanLength() < 1e-6) (pose.scale > 1 ? grew : shrank)++;
      if (pose.rotation > 1e-6) ++clockwise;
      if (pose.rotation < -1e-6) ++anticlockwise;
    }
    check(grew > 0 && shrank == 0 && clockwise > 0 && anticlockwise > 0,
          QString("Random only grows letters at a Scale of 250%, and turns them both ways "
                  "(%1 grown, %2 shrunk; %3 and %4 turned)").arg(grew).arg(shrank).arg(clockwise).arg(anticlockwise));
    // Position Y alone: the side still comes up, as far as Y.
    text.characterAnimation = QJsonObject{{"enabled", true}, {"mode", 1}, {"seed", 1}, {"y", 80}};
    int sideways = 0;
    for (int rank = 0; rank < 12; ++rank) {
      const QPointF at = characterPose(text, 0, rank, 12).offset;
      if (std::abs(at.y()) < 1e-6 && std::abs(at.x()) >= 40 - 1e-6 && std::abs(at.x()) <= 80 + 1e-6) ++sideways;
    }
    check(sideways == 4,
          QString("Random with Position Y alone sends one letter in three to the side, as far as Y (%1 of 12)").arg(sideways));
    // ZigZag with both: Position and Scale by turns, each changing its way by turns.
    text.characterAnimation = QJsonObject{{"enabled", true}, {"mode", 2}, {"y", 80}, {"scale", 150}};
    const CharacterPose z0 = characterPose(text, 0, 0, 8), z1 = characterPose(text, 0, 1, 8),
                        z2 = characterPose(text, 0, 2, 8), z3 = characterPose(text, 0, 3, 8);
    check(std::abs(z0.offset.y() - 80) < 1e-6 && std::abs(z0.scale - 1) < 1e-6 &&
              z1.offset.manhattanLength() < 1e-6 && std::abs(z1.scale - 1.5) < 1e-6 &&
              std::abs(z2.offset.y() + 80) < 1e-6 && std::abs(z2.scale - 1) < 1e-6 &&
              z3.offset.manhattanLength() < 1e-6 && std::abs(z3.scale - 1 / 1.5) < 1e-6,
          "ZigZag with Position and Scale takes them by turns: up, grown, down, shrunk");
    // Shrunk the other way of a big Scale is still a letter: 300% one way, a
    // third the other -- mirrored about 100% it was -100%, turned inside out.
    text.characterAnimation = QJsonObject{{"enabled", true}, {"mode", 2}, {"scale", 300}, {"y", 0}};
    check(std::abs(characterPose(text, 0, 0, 8).scale - 3) < 1e-6 &&
              std::abs(characterPose(text, 0, 1, 8).scale - 1 / 3.) < 1e-6,
          "ZigZag shrinks the other letters to a third of a 300% Scale, never below nothing");
    // ZigZag with Position alone zigzags every letter, as before.
    text.characterAnimation = QJsonObject{{"enabled", true}, {"mode", 2}, {"y", 80}};
    check(std::abs(characterPose(text, 0, 0, 8).offset.y() - 80) < 1e-6 &&
              std::abs(characterPose(text, 0, 1, 8).offset.y() + 80) < 1e-6,
          "ZigZag with Position alone sends every other letter the other way");
  }
  {
    // The letters move as In's graph is drawn, and nothing eases them again: a
    // Strength keyed with a strong ease-out moves them as a layer's Position
    // keyed with the same graph moves. Smooth Strength eased them a second
    // time -- one frame in twenty they had come 19% of the way, not 28%, and
    // the long settle was cut short.
    Layer text; text.type = "text"; text.text = "AB"; text.duration = 60;
    text.characterAnimation = QJsonObject{{"enabled", true}, {"y", 100}, {"opacity", 100}, {"delay", 0}};
    auto strongOut = [](Property &p, double from, double to) {
      p = Property{};
      p.set(0, from); p.set(20, to);
      Key &k = p.keys[0];
      k.custom = true; k.absolute = false; k.ease = "custom"; k.curve = {.16, 1, .3, 1};
    };
    strongOut(text.p[6], 0, 100);
    Property y;
    strongOut(y, 100, 0);
    double worst = 0;
    for (double t = 0; t <= 24; t += .25)
      worst = std::max(worst, std::abs(characterPose(text, t, 0, 2).offset.y() - y.at(t)));
    const double early = 100 - characterPose(text, 1, 0, 2).offset.y();
    check(worst < 1e-6 && early > 27,
          QString("the letters follow In's graph as a layer keyed with it moves (off by %1 px, %2% of the way "
                  "after one frame in twenty)").arg(worst).arg(early, 0, 'f', 1));
  }
  {
    // A text from before Smooth Strength went, opened: the ease it put on the
    // letters is drawn into In's and Out's keys where they ran straight -- they
    // move as they did, and the graph shows how -- and the setting is gone. A
    // stretch with an ease of its own keeps it. Animators waiting in the stack
    // are settled too.
    Scene old; old.width = 320; old.height = 180; old.fps = 30; old.frames = 60;
    Layer text; text.id = "smoothed"; text.type = "text"; text.text = "AB"; text.duration = 60;
    text.appliedEffects["Text-Animation"] = true;
    text.characterAnimation = QJsonObject{{"enabled", true}, {"y", 100}, {"opacity", 100}, {"delay", 0}, {"smooth", true}};
    text.p[6] = Property{}; text.p[6].set(0, 0); text.p[6].set(20, 100);
    text.p[7] = Property{}; text.p[7].set(40, 0); text.p[7].set(50, 100);
    text.p[7].keys[0].ease = "easeIn";
    const Property easedOut = text.p[7];
    const QString waiting = textAnimatorUid(text.characterAnimation);
    addTextAnimator(text);
    // The second, in the editing places: straight from 0 to 100 over thirty frames.
    text.characterAnimation["x"] = 100; text.characterAnimation["y"] = 0; text.characterAnimation["delay"] = 0;
    text.characterAnimation["smooth"] = true;
    text.p[6] = Property{}; text.p[6].set(0, 0); text.p[6].set(30, 100);
    old.layers = {text};
    Scene opened;
    QString error;
    const bool loaded = opened.load(old.json(), error) && opened.layers.size() == 1;
    bool settled = loaded, followed = loaded, kept = false;
    int animators = 0;
    if (loaded) {
      const Layer &l = opened.layers[0];
      settled = !l.characterAnimation.contains("smooth");
      for (const auto &v : l.characterAnimation.value("stack").toArray())
        settled = settled && !v.toObject().contains("smooth");
      auto smoothstep = [](double x) { x = std::clamp(x, 0., 1.); return x * x * (3 - 2 * x); };
      for (const TextAnimator &animator : textAnimators(l)) {
        ++animators;
        const double length = animator.uid == waiting ? 20 : 30;
        for (double t = -2; t <= length + 2; t += .25)
          followed = followed &&
                     std::abs(animatorProgress(*animator.in, animator.settings, t, 0, 0) - smoothstep(t / length)) < 1e-6;
        const Key &first = animator.in->keys.front();
        settled = settled && first.custom && !first.absolute && std::abs(first.curve[0] - 1. / 3) < 1e-12 &&
                  first.curve[1] == 0 && std::abs(first.curve[2] - 2. / 3) < 1e-12 && first.curve[3] == 1;
        if (animator.uid != waiting) continue;
        const Key &own = animator.out->keys.front();
        kept = !own.custom && own.ease == "easeIn";
        for (double t = 38; t <= 52; t += .25) kept = kept && std::abs(animator.out->at(t) - easedOut.at(t)) < 1e-9;
      }
    }
    check(loaded && animators == 2 && settled && followed && kept,
          QString("a text saved with Smooth Strength opens with its ease drawn into the keys, moving as it did, "
                  "the setting gone (%1 animators; settled %2, as before %3, own ease kept %4) %5")
              .arg(animators).arg(settled).arg(followed).arg(kept).arg(error));
  }
  {
    // Text Animations stacked: one after another, each in the frame the one above left.
    Scene scene;
    scene.width = 640; scene.height = 360; scene.fps = 30; scene.frames = 60;
    Layer t;
    t.id = "stacked"; t.type = "text"; t.text = "AB"; t.fontSize = 80; t.duration = 60;
    t.p[0].base = 320; t.p[1].base = 180;
    // The first: turned a quarter, held there (In 0 is "all the way out of place").
    t.characterAnimation = QJsonObject{{"enabled", true}, {"x", 0}, {"y", 0}, {"rotation", 90}, {"opacity", 100}, {"delay", 0}};
    t.p[6] = Property{}; t.p[6].set(0, 0); t.p[6].set(50, 0);
    t.appliedEffects["Text-Animation"] = true;
    const QString second = addTextAnimator(t);
    // The second, now in the editing places: a push to the right, held too.
    t.characterAnimation["x"] = 100; t.characterAnimation["y"] = 0; t.characterAnimation["opacity"] = 100;
    t.characterAnimation["delay"] = 0;
    t.p[6] = Property{}; t.p[6].set(0, 0); t.p[6].set(50, 0);
    const auto stack = textAnimators(t);
    check(stack.size() == 2 && second == "2" && textAnimatorUid(t.characterAnimation) == "2",
          "a second Text Animation joins the stack and is the one being edited");
    const CharacterPose firstThenSecond = characterPose(t, 10, 0, 2);
    check(std::abs(firstThenSecond.offset.x()) < 1 && std::abs(firstThenSecond.offset.y() - 100) < 1 &&
              std::abs(firstThenSecond.rotation - 90) < 1,
          QString("the one below moves in the frame the one above turned: a push right goes down (%1, %2)")
              .arg(firstThenSecond.offset.x(), 0, 'f', 1).arg(firstThenSecond.offset.y(), 0, 'f', 1));
    moveTextAnimator(t, -1);
    const CharacterPose secondThenFirst = characterPose(t, 10, 0, 2);
    check(std::abs(secondThenFirst.offset.x() - 100) < 1 && std::abs(secondThenFirst.offset.y()) < 1,
          "moved up the stack, the push comes first and stays a push to the right");
    // Back to the first: its settings and its In keys come back whole.
    selectTextAnimator(t, "1");
    check(textAnimatorUid(t.characterAnimation) == "1" && t.characterAnimation.value("rotation").toDouble() == 90 &&
              t.p[6].keys.size() == 2 && textAnimators(t).size() == 2,
          "switching which one is edited keeps every animation's settings and keys");
    // One switched off leaves the other alone.
    t.characterAnimation["off"] = true;
    const CharacterPose onlyPush = characterPose(t, 10, 0, 2);
    check(textAnimators(t).size() == 1 && std::abs(onlyPush.rotation) < 1 && std::abs(onlyPush.offset.x() - 100) < 1,
          "a Text Animation switched off drops out; the rest go on");
    t.characterAnimation.remove("off");
    // Drawn: the letters land where the stack puts them, not where the text sits.
    scene.layers = {t};
    clearRenderCaches();
    const QImage moved = renderCPU(scene, 10, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    Layer still = t;
    still.characterAnimation["enabled"] = false;
    scene.layers = {still};
    clearRenderCaches();
    const QImage plain = renderCPU(scene, 10, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    int differ = 0;
    for (int y = 0; y < 360; ++y)
      for (int x = 0; x < 640; ++x) differ += moved.pixel(x, y) != plain.pixel(x, y) ? 1 : 0;
    check(differ > 500, QString("two stacked animations move the drawn letters (%1 px differ)").arg(differ));
    removeTextAnimator(t);
    check(textAnimators(t).size() == 1 && t.extraCurveIds.filter("TextAnim/").size() == 0,
          "removing one leaves the other, and no curves of the removed one");
    // A copy: the same settings and In keys, right below the one it came from.
    t.characterAnimation["rotation"] = 45;
    t.characterAnimation["name"] = "Turn";
    const int keysBefore = int(t.p[6].keys.size());
    const QString source = textAnimatorUid(t.characterAnimation);
    const QString copy = duplicateTextAnimator(t);
    const QStringList afterCopy = textAnimatorOrder(t.characterAnimation);
    check(textAnimatorUid(t.characterAnimation) == copy && afterCopy.indexOf(copy) == afterCopy.indexOf(source) + 1 &&
              t.characterAnimation.value("rotation").toDouble() == 45 && int(t.p[6].keys.size()) == keysBefore &&
              t.characterAnimation.value("name").toString() == "Turn copy",
          "a duplicate keeps the settings and keys, right below the one it came from");
    check(textAnimatorSummary(t.characterAnimation).contains("Turn 45"),
          QString("a heading says what its animation does (%1)").arg(textAnimatorSummary(t.characterAnimation)));
  }
  {
    // A letter pushed a long way by its animation is drawn whole, not cut at the
    // edge of the text's own box.
    Scene scene;
    scene.width = 640; scene.height = 480; scene.fps = 30; scene.frames = 60;
    Layer pushed;
    pushed.id = "pushed"; pushed.type = "text"; pushed.text = "AB"; pushed.fontSize = 70; pushed.bold = true;
    pushed.color = Qt::white; pushed.duration = 60;
    pushed.p[0].base = 320; pushed.p[1].base = 120;
    pushed.characterAnimation = QJsonObject{{"enabled", true}, {"x", 0}, {"y", 220}, {"opacity", 100}, {"delay", 0}};
    pushed.p[6] = Property{}; pushed.p[6].base = 0; // all the way out of place: 220 px down
    pushed.appliedEffects["Text-Animation"] = true;
    Layer placed = pushed;
    placed.characterAnimation = QJsonObject{};
    placed.p[1].base = 340; // where the pushed letters should land
    auto opaque = [](const QImage &img) {
      int n = 0;
      for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) n += qAlpha(img.pixel(x, y)) > 128;
      return n;
    };
    scene.layers = {pushed};
    clearRenderCaches();
    const int moved = opaque(renderCPU(scene, 5, false, QSize(640, 480)).convertToFormat(QImage::Format_ARGB32));
    scene.layers = {placed};
    clearRenderCaches();
    const int still = opaque(renderCPU(scene, 5, false, QSize(640, 480)).convertToFormat(QImage::Format_ARGB32));
    check(still > 500 && std::abs(moved - still) < still / 20,
          QString("letters pushed 220 px by their animation are drawn whole (%1 px, %2 when placed there)").arg(moved).arg(still));
  }
  {
    // Text Animation by words and lines, a Range, and the new transforms.
    Scene scene;
    scene.width = 640; scene.height = 360; scene.fps = 30; scene.frames = 60;
    Layer base;
    base.id = "units"; base.type = "text"; base.text = "AB CD"; base.fontSize = 70; base.bold = true;
    base.color = Qt::white; base.duration = 60;
    base.p[0].base = 320; base.p[1].base = 150;
    base.appliedEffects["Text-Animation"] = true;
    struct Run { double x, y; int alpha; };
    // The letters as they are drawn, left to right: each one's middle and ink.
    auto runsOf = [&](const Layer &l, double frame) {
      scene.layers = {l};
      clearRenderCaches();
      const QImage img = renderCPU(scene, frame, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
      std::vector<Run> runs;
      double sx = 0, sy = 0, sa = 0;
      bool in = false;
      for (int x = 0; x <= img.width(); ++x) {
        double column = 0, cy = 0;
        if (x < img.width())
          for (int y = 0; y < img.height(); ++y) {
            const int a = qAlpha(img.pixel(x, y));
            column += a;
            cy += a * double(y);
          }
        if (column > 40) {
          in = true;
          sx += column * x; sy += cy; sa += column;
        } else if (in) {
          runs.push_back({sx / sa, sy / sa, int(sa / 255)});
          sx = sy = sa = 0;
          in = false;
        }
      }
      return runs;
    };
    auto withIn = [](Layer l, double value) { l.p[6] = Property{}; l.p[6].base = value; return l; };
    Layer rest = withIn(base, 100);
    rest.characterAnimation = QJsonObject{{"enabled", true}, {"x", 0}, {"y", 0}, {"opacity", 100}};
    const auto still = runsOf(rest, 5);
    check(still.size() == 4, QString("four letters drawn apart (%1)").arg(still.size()));
    if (still.size() == 4) {
      // Words: each word moves as one; the second starts a word's delay later.
      Layer words = base;
      words.characterAnimation = QJsonObject{{"enabled", true}, {"x", 0}, {"y", 100}, {"opacity", 100}, {"delay", 10},
                                             {"basedOn", 1}};
      words.p[6] = Property{}; words.p[6].set(0, 0); words.p[6].set(10, 100);
      auto moved = runsOf(words, 5);
      // How far each letter has moved from where it rests: letters of a real
      // typeface carry their ink at heights of their own (an A low, a B even).
      std::vector<Run> w = moved;
      if (w.size() == 4)
        for (size_t i = 0; i < 4; ++i) w[i].y -= still[i].y;
      check(w.size() == 4 && std::abs(w[0].y - w[1].y) < 2 && std::abs(w[2].y - w[3].y) < 2 &&
                std::abs((w[2].y - w[0].y) - 50) < 4,
            w.size() == 4 ? QString("by words, a word moves as one and the next follows (%1 %2 | %3 %4)")
                                .arg(w[0].y, 0, 'f', 1).arg(w[1].y, 0, 'f', 1).arg(w[2].y, 0, 'f', 1).arg(w[3].y, 0, 'f', 1)
                          : QString("by words: %1 runs").arg(w.size()));
      Layer letters = words;
      letters.characterAnimation["basedOn"] = 0;
      const auto c = runsOf(letters, 5);
      check(c.size() == 4 && std::abs(c[0].y - c[1].y) > 20, "by characters, the letters of a word move one after another");
      // A Range: only the first half of the text is reached.
      Layer ranged = withIn(base, 0);
      ranged.characterAnimation = QJsonObject{{"enabled", true}, {"x", 0}, {"y", 100}, {"opacity", 100}, {"delay", 0},
                                              {"rangeStart", 0}, {"rangeEnd", 50}};
      const auto r = runsOf(ranged, 5);
      check(r.size() == 4 && std::abs(r[0].y - still[0].y - 100) < 3 && std::abs(r[1].y - still[1].y - 100) < 3 &&
                std::abs(r[2].y - still[2].y) < 2 && std::abs(r[3].y - still[3].y) < 2,
            "a Range of the first half moves A and B and leaves C and D where they are");
      // Keyed, its Offset sweeps it along: at frame 30 it covers the second half.
      Layer swept = ranged;
      const int offset = layerAddExtra(swept, "TextAnim/1/Range Offset", 0);
      swept.at(offset).set(0, 0);
      swept.at(offset).set(30, 50);
      const auto sw = runsOf(swept, 30);
      check(sw.size() == 4 && std::abs(sw[0].y - still[0].y) < 2 && std::abs(sw[3].y - still[3].y - 100) < 3,
            "a keyed Range Offset sweeps the animation across the text");
      // Tracking: the line spreads from its middle while out of place.
      Layer spread = withIn(base, 0);
      spread.characterAnimation = QJsonObject{{"enabled", true}, {"x", 0}, {"y", 0}, {"opacity", 100}, {"tracking", 40}};
      const auto sp = runsOf(spread, 5);
      const double wide = sp.size() == 4 ? (sp[3].x - sp[0].x) - (still[3].x - still[0].x) : 0;
      check(std::abs(wide - 120) < 6, QString("Tracking 40 spreads four letters 120 px wider (%1)").arg(wide, 0, 'f', 1));
      // Color: the letters carry the colour while out of place, and lose it in place.
      Layer tinted = withIn(base, 0);
      tinted.characterAnimation = QJsonObject{{"enabled", true}, {"x", 0}, {"y", 0}, {"opacity", 100},
                                              {"colorAmount", 100}, {"fillColor", "#FF0000"}};
      auto reds = [&](const Layer &l) {
        scene.layers = {l};
        clearRenderCaches();
        const QImage img = renderCPU(scene, 5, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
        int n = 0;
        for (int y = 0; y < img.height(); ++y)
          for (int x = 0; x < img.width(); ++x) {
            const QRgb p = img.pixel(x, y);
            n += qAlpha(p) > 200 && qRed(p) > 200 && qGreen(p) < 60 && qBlue(p) < 60;
          }
        return n;
      };
      Layer settled = tinted;
      settled.p[6].base = 100;
      check(reds(tinted) > 1000 && reds(settled) == 0, "Color tints the letters while out of place, not once in place");
      // Blur: soft edges while out of place.
      Layer soft = withIn(base, 0);
      soft.characterAnimation = QJsonObject{{"enabled", true}, {"x", 0}, {"y", 0}, {"opacity", 100}, {"blur", 8}};
      auto halfTones = [&](const Layer &l) {
        scene.layers = {l};
        clearRenderCaches();
        const QImage img = renderCPU(scene, 5, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
        int n = 0;
        for (int y = 0; y < img.height(); ++y)
          for (int x = 0; x < img.width(); ++x) n += qAlpha(img.pixel(x, y)) > 20 && qAlpha(img.pixel(x, y)) < 200;
        return n;
      };
      const int blurred = halfTones(soft), sharp = halfTones(rest);
      check(blurred > sharp * 4, QString("Blur softens the letters while out of place (%1 soft pixels, %2 sharp)").arg(blurred).arg(sharp));
      check(textAnimatorSummary(QJsonObject{{"basedOn", 1}, {"blur", 4}, {"rangeEnd", 50}}).startsWith("Words") &&
                textAnimatorSummary(QJsonObject{{"basedOn", 1}, {"blur", 4}, {"rangeEnd", 50}}).contains("Range"),
            "a heading says it moves words and has a Range");
      // A Range curve goes with its animation: a copy has its own, a removal takes it.
      Layer stack = swept;
      const QString copy = duplicateTextAnimator(stack);
      check(layerFindExtra(stack, "TextAnim/" + copy + "/Range Offset") >= 0, "a duplicate keeps its keyed Range");
      removeTextAnimator(stack);
      check(layerFindExtra(stack, "TextAnim/" + copy + "/Range Offset") < 0, "removing an animation removes its Range curves");
    }
  }
  {
    // A stacked Text Animation's curve, its handle dragged: past its last key the
    // later letters still read the curve at their delay, so the frame there
    // must change with it -- not come back from the cache as it was.
    Scene scene;
    scene.width = 640; scene.height = 360; scene.fps = 30; scene.frames = 60;
    Layer text;
    text.id = "stacked"; text.type = "text"; text.text = "ABCD"; text.fontSize = 70; text.bold = true;
    text.color = Qt::white; text.duration = 60;
    text.p[0].base = 320; text.p[1].base = 180;
    text.appliedEffects["Text-Animation"] = true;
    text.characterAnimation = QJsonObject{{"enabled", true}, {"x", 0}, {"y", 120}, {"opacity", 100}, {"delay", 10}};
    text.p[6] = Property{}; text.p[6].set(0, 0); text.p[6].set(10, 100);
    addTextAnimator(text); // the first one now waits in the stack, its In a TextAnim curve
    const int curve = layerFindExtra(text, "TextAnim/1/In");
    check(curve >= 0 && text.at(curve).keys.size() == 2, "the first Text Animation moved into the stack with its keys");
    if (curve >= 0 && text.at(curve).keys.size() == 2) {
      scene.layers = {text};
      clearRenderCaches();
      const QImage before = renderCPU(scene, 14, false, QSize(640, 360));
      scene.layers[0].at(curve).keys[0].custom = true;
      scene.layers[0].at(curve).keys[0].curve = {.95, 0., .98, .15};
      const QImage dragged = renderCPU(scene, 14, false, QSize(640, 360));
      clearRenderCaches();
      const QImage fresh = renderCPU(scene, 14, false, QSize(640, 360));
      check(fresh != before && dragged == fresh,
            "a stacked Text Animation's handle drag reaches the frames past its last key");
    }
  }
  {
    // An overshooting In: past 100 the letters are thrown beyond their places
    // and still moving, so the frames there are not "at rest" and not one picture.
    Scene scene;
    scene.width = 640; scene.height = 360; scene.fps = 30; scene.frames = 60;
    Layer text;
    text.id = "overshoot"; text.type = "text"; text.text = "ABCD"; text.fontSize = 70; text.bold = true;
    text.color = Qt::white; text.duration = 60;
    text.p[0].base = 320; text.p[1].base = 180;
    text.appliedEffects["Text-Animation"] = true;
    text.characterAnimation = QJsonObject{{"enabled", true}, {"x", 0}, {"y", 120}, {"opacity", 100}, {"delay", 4}};
    text.p[6] = Property{}; text.p[6].set(0, 0); text.p[6].set(6, 170); text.p[6].set(15, 100);
    // Frame 16: the letters read the curve at 16, 12, 8 and 4 -- every one at 100 or past it.
    check(!characterPoseAtRest(text, 16) && characterPoseAtRest(text, 30),
          "letters overshooting past their places are not at rest; settled at 100 they are");
    scene.layers = {text};
    clearRenderCaches();
    const QImage a = renderCPU(scene, 16, false, QSize(640, 360));
    const QImage b = renderCPU(scene, 18, false, QSize(640, 360));
    check(a != b && !sameCompositionPixels(scene, 16, 18),
          "an overshooting Text Animation keeps moving after every letter has passed 100");
  }
  {
    // The random numbers the letters' poses draw on are remembered instead of
    // seeded afresh every time: they have to be QRandomGenerator's own.
    bool same = true;
    for (quint32 seed : {0u, 1u, 7194u, 123456789u, 4294967295u}) {
      QRandomGenerator fresh(seed);
      const auto kept = seededDoubles(seed);
      for (double value : kept) same = same && value == fresh.generateDouble();
      same = same && seededDoubles(seed) == kept && seededWord(seed) == QRandomGenerator(seed).generate();
    }
    check(same, "the letters' remembered random numbers are QRandomGenerator's, number for number");
  }
  {
    // A moving text is drawn on a picture of where its letters are now, not of
    // everywhere their animation could take them: every pixel it draws lies
    // inside animatedTextBounds, which is far smaller than that reach.
    Scene scene;
    scene.width = 640; scene.height = 360; scene.fps = 30; scene.frames = 40;
    Layer text;
    text.id = "wander"; text.type = "text"; text.text = "WAVE"; text.fontSize = 80; text.bold = true;
    text.color = Qt::white; text.duration = 40; text.motionBlur = false;
    text.p[0].base = 320; text.p[1].base = 180;
    text.appliedEffects["Text-Animation"] = true;
    text.characterAnimation = QJsonObject{{"enabled", true}, {"mode", 1}, {"seed", 3}, {"y", -300}, {"rotation", 70},
                                          {"delay", 2}, {"wiggleSpeed", 1.4}, {"wigglePosition", 5}};
    text.p[6] = Property{}; text.p[6].set(0, 0); text.p[6].set(20, 100);
    Scene empty = scene;
    scene.layers = {text};
    clearRenderCaches();
    const QImage drawn = renderCPU(scene, 8, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    const QImage nothing = renderCPU(empty, 8, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
    const QRectF tight = animatedTextBounds(text, 8, 30).translated(320, 180).adjusted(-1, -1, 1, 1);
    int outside = 0, inside = 0;
    for (int y = 0; y < drawn.height(); ++y)
      for (int x = 0; x < drawn.width(); ++x)
        if (drawn.pixel(x, y) != nothing.pixel(x, y)) (tight.contains(QPointF(x + .5, y + .5)) ? inside : outside)++;
    const QRectF reach = layerBounds(text, scene);
    check(inside > 0 && outside == 0 && tight.width() * tight.height() < reach.width() * reach.height() / 2,
          QString("moving letters draw inside the box of where they are now (%1 pixels outside it)").arg(outside));
  }
  {
    // A zooming layer's one halo is kept whole at a scale its blur sets, apart
    // from the layer's own raster: at a scale and framing both can have they
    // once shared a key, and the layer, drawn from its raster, came out as
    // nothing but its shadow.
    Scene scene;
    scene.width = 640; scene.height = 360; scene.fps = 30; scene.frames = 30;
    Layer disc;
    disc.id = "disc"; disc.type = "shape"; disc.shapeKind = "circle"; disc.color = QColor(220, 60, 60);
    disc.duration = 30; disc.motionBlur = false;
    disc.p[0].base = 320; disc.p[1].base = 180;
    disc.p[ShapeWidth].base = disc.p[ShapeHeight].base = 300;
    disc.shadow = 18; disc.appliedEffects["Drop Shadow"] = true;
    for (int index : {2, 3}) { disc.p[index] = Property{}; disc.p[index].set(0, .5); disc.p[index].set(10, .75); disc.p[index].set(20, .5); }
    scene.layers = {disc};
    clearRenderCaches();
    renderCPU(scene, 10, false, QSize(640, 360));
    const QImage afterZoom = renderCPU(scene, 20, false, QSize(640, 360));
    clearRenderCaches();
    const QImage fresh = renderCPU(scene, 20, false, QSize(640, 360));
    check(afterZoom == fresh && QColor(fresh.pixel(320, 180)).red() > 150,
          "a zooming shadowed layer is drawn the same after its halo was kept as drawn fresh");
  }
  {
    // Strokes round a tightly led text: the outer stroke reaches below the
    // line box Leading under 1 squeezes, and it is drawn whole, not cut there.
    Scene scene;
    scene.width = 960; scene.height = 360; scene.fps = 30; scene.frames = 60;
    Layer t;
    t.id = "led"; t.type = "text"; t.text = "Wavy-Flo"; t.font = "Arial"; t.fontSize = 73; t.bold = true;
    t.tracking = 29; t.leading = .6; t.duration = 60; t.color = Qt::white;
    t.p[0].base = 480; t.p[1].base = 180;
    auto lowest = [&](const Layer &l) {
      scene.layers = {l};
      clearRenderCaches();
      const QImage frame = renderCPU(scene, 10, false, QSize(960, 360)).convertToFormat(QImage::Format_ARGB32);
      int bottom = -1;
      for (int y = 0; y < frame.height(); ++y)
        for (int x = 0; x < frame.width(); ++x)
          if (qAlpha(frame.pixel(x, y)) > 8) { bottom = y; break; }
      return bottom;
    };
    const int bare = lowest(t);
    t.textStrokes.push_back(TextStrokeStyle{6, QColor(230, 40, 40), 1, 1});
    t.textStrokes.push_back(TextStrokeStyle{9, QColor(40, 120, 255), 1, 1});
    syncLegacyTextStroke(t);
    const int stroked = lowest(t);
    check(stroked >= bare + 12,
          QString("two outside strokes on text with Leading 60% reach below the letters, not cut at the line (%1 -> %2)")
              .arg(bare).arg(stroked));
  }
  return failures ? 1 : 0;
}
