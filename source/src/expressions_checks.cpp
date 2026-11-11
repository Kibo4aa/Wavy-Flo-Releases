#include "expressions.h"
#include "expression_controls.h"
#include "engine.h"
#include "prerender.h"
#include "render_workers.h"
#include <QElapsedTimer>
#include <cmath>
#include <future>

// --expressions-self-test: the language, the After Effects vocabulary, and an
// expression reaching every place a property's value is used.
int checkExpressions(QStringList &report) {
  int failures = 0;
  auto check = [&](bool passed, const QString &label) {
    report << label + (passed ? ": PASS" : ": FAIL");
    failures += !passed;
  };
  auto near = [](double a, double b, double tolerance = 1e-6) { return std::abs(a - b) <= tolerance; };

  Scene scene;
  scene.width = 640; scene.height = 360; scene.fps = 30; scene.frames = 120;
  Layer leader; leader.name = "Leader"; leader.type = "shape"; leader.shapeKind = "rect"; leader.duration = 120;
  leader.p[0].base = 100; leader.p[1].base = 200;
  Layer follower = leader; follower.id = QUuid::createUuid().toString(QUuid::WithoutBraces); follower.name = "Follower";
  Layer late = leader; late.id = QUuid::createUuid().toString(QUuid::WithoutBraces); late.name = "Late"; late.start = 10;
  scene.layers = {follower, leader, late};
  ExpressionSceneScope scope(&scene);
  Layer &f = scene.layers[0];
  Layer &l = scene.layers[1];
  Layer &lateLayer = scene.layers[2];

  // What a line of script works out to on the follower's rotation at a frame.
  auto value = [&](const QString &text, double frame = 0, int slot = 4, Layer *on = nullptr, QString *error = nullptr) {
    Layer &target = on ? *on : f;
    QString why;
    if (!target.p[size_t(slot)].setExpression(text, &why)) {
      if (error) *error = why;
      return std::nan("");
    }
    const double result = evaluateExpression(*target.p[size_t(slot)].program, target.p[size_t(slot)], frame, error);
    return result;
  };

  // --- the language ---------------------------------------------------------
  check(near(value("1 + 2 * 3"), 7) && near(value("(1 + 2) * 3"), 9) && near(value("10 % 4"), 2) &&
            near(value("-2 * -3"), 6) && near(value("2 - 3 - 4"), -5) && near(value("12 / 4 / 3"), 1),
        "arithmetic keeps its precedence and reads left to right");
  check(near(value("var a = 3; var b = a * 2; a + b"), 9) && near(value("let x = 1\nx += 4\nx * 2"), 10),
        "variables, and statements on separate lines");
  check(near(value("if (2 > 1) { 5 } else { 6 }"), 5) && near(value("if (0) 5; else 6"), 6) &&
            near(value("3 > 2 ? 10 : 20"), 10) && near(value("0 || 7"), 7) && near(value("1 && 0"), 0),
        "if/else, the ternary, and the logical operators");
  check(near(value("Math.sin(Math.PI / 2)"), 1) && near(value("Math.max(3, 9, 4)"), 9) &&
            near(value("Math.round(2.5) + Math.floor(-1.5)"), 1) && near(value("Math.abs(-4) + Math.pow(2, 3)"), 12),
        "Math works as it does in JavaScript");
  check(near(value("[1, 2, 3][1]"), 2) && near(value("var v = [4, 5] + [1, 1]; v[0] * v[1]"), 30) &&
            near(value("([2, 4] * 3)[1]"), 12) && near(value("length([3, 4])"), 5),
        "arrays add, scale and index part by part");
  check(near(value("// a comment\n/* another */ 42"), 42) && near(value("\"a\" == 'a' ? 1 : 0"), 1),
        "comments and text");
  {
    QString error;
    const bool broken = !compileExpression("1 +", &error) && !error.isEmpty();
    QString unknown;
    const bool named = !compileExpression("wobble(2)", &unknown) && unknown.contains("wobble");
    check(broken && named, QString("an expression that cannot be read says why (%1 / %2)").arg(error, unknown));
  }
  check(!compileExpression("time = 3") && !compileExpression("") && !compileExpression("[1,2,3,4]"),
        "the program's own names cannot be set, and empty text is not an expression");

  // --- time, value, index ------------------------------------------------------
  f.p[4].base = 12;
  check(near(value("time * 10", 15), 5) && near(value("value * 2", 0), 24),
        "time is in seconds, value is the property's own value");
  check(near(value("time", 5, 4, &lateLayer), 0.5),
        "time is the composition's, on a layer that starts later");
  check(near(value("index"), 1) && near(value("index", 0, 4, &l), 2) && near(value("thisLayer.index", 0, 4, &lateLayer), 3),
        "index counts layers from the top, from 1");
  check(near(value("inPoint", 0, 4, &lateLayer), 10. / 30) && near(value("thisComp.width + thisComp.height"), 1000) &&
            near(value("thisComp.frameDuration * 30"), 1) && near(value("thisComp.numLayers"), 3),
        "inPoint and the composition's size and frame duration");
  f.p[4].keys.clear();
  f.p[4].set(0, 0); f.p[4].set(30, 90);
  check(near(value("value", 15), 45) && near(value("valueAtTime(0.5) + valueAtTime(1)", 0), 135),
        "value follows the keyframes, valueAtTime reads them at another time");

  // --- a value in several parts ---------------------------------------------
  f.p[0].base = 100; f.p[1].base = 200;
  check(near(value("value + [10, 20]", 0, 0), 110) && near(value("value + [10, 20]", 0, 1), 220),
        "on Position, value is [x, y] and each part takes its own share of the result");
  check(near(value("[value[1], value[0]]", 0, 0), 200),
        "a Position part can read the other part");
  f.p[0].setExpression({}); f.p[1].setExpression({});
  check(near(value("position[0] + 1", 0, 4), 101) && near(value("transform.position[1]", 0, 4), 200) &&
            near(value("scale[0]", 0, 4), 100),
        "position, scale and transform name this layer's own values");
  {
    // Percent, as the timeline shows them and After Effects writes them.
    f.p[5].base = 1;
    const double half = value("value / 2", 0, 5), fifty = value("50", 0, 5);
    const double bigger = value("[value[0] * 1.5, value[1]]", 0, 2);
    f.p[5].setExpression({}); f.p[2].setExpression({});
    check(near(half, .5) && near(fifty, .5) && near(bigger, 1.5) && near(value("opacity", 0, 4), 100),
          "Opacity and Scale are written in percent");
  }

  // --- other layers ---------------------------------------------------------
  l.p[0].base = 300;
  check(near(value("thisComp.layer(\"Leader\").transform.position[0] + 50", 0, 0), 350) &&
            near(value("thisComp.layer(2).position[1]", 0, 1), 200),
        "thisComp.layer() finds a layer by name or by number");
  l.p[0].keys.clear(); l.p[0].set(0, 0); l.p[0].set(30, 300);
  check(near(value("thisComp.layer(\"Leader\").position.valueAtTime(time - 0.5)[0]", 30, 0), 150),
        "another layer's property read at another time: a delayed follow");
  {
    QString error;
    const double result = value("thisComp.layer(\"Nobody\").position[0]", 0, 0, nullptr, &error);
    check(near(result, 100) && error.contains("Nobody"),
          "a layer that is not there leaves the value as it was and says why");
  }
  {
    // Two properties that read each other would never finish.
    f.p[4].keys.clear(); l.p[4].keys.clear();
    f.p[4].setExpression("thisComp.layer(\"Leader\").rotation + 1");
    l.p[4].setExpression("thisComp.layer(\"Follower\").rotation + 1");
    QElapsedTimer clock; clock.start();
    const double a = f.p[4].at(0);
    check(std::isfinite(a) && clock.elapsed() < 2000, QString("two expressions that read each other still finish (%1)").arg(a));
    l.p[4].setExpression({});
  }

  // --- the AE vocabulary ----------------------------------------------------
  f.p[4].keys.clear(); f.p[4].base = 0;
  check(near(value("linear(time, 0, 1, 0, 100)", 7.5), 25) && near(value("linear(time, 0, 1, 0, 100)", 60), 100) &&
            near(value("ease(0.5, 0, 1, 0, 100)"), 50) && near(value("linear(0.5, 10, 20)"), 15) &&
            near(value("easeIn(0.5, 0, 1, 0, 100)"), 25) && near(value("easeOut(0.5, 0, 1, 0, 100)"), 75),
        "linear, ease, easeIn and easeOut map one range onto another");
  check(near(value("clamp(15, 0, 10)"), 10) && near(value("degreesToRadians(180)"), 3.14159265358979, 1e-9) &&
            near(value("timeToFrames(2)"), 60) && near(value("framesToTime(15)"), .5),
        "clamp and the unit conversions");
  {
    f.p[4].set(0, 0); f.p[4].set(10, 100);
    check(near(value("loopOut()", 12), 20) && near(value("loopOut('cycle')", 25), 50) &&
              near(value("loopOut('pingpong')", 12), 80) && near(value("loopOut('offset')", 12), 120) &&
              near(value("loopOut('continue')", 12), 120) && near(value("loopOut()", 5), 50),
          "loopOut cycles, ping-pongs, offsets and continues past the last key");
    check(near(value("thisProperty.loopOut('cycle')", 12), 20),
          "loopOut can be asked of thisProperty");
    f.p[4].keys.clear(); f.p[4].set(10, 0); f.p[4].set(20, 100);
    check(near(value("loopIn('cycle')", 2), 20), "loopIn cycles before the first key");
    f.p[4].keys.clear();
  }
  {
    f.p[0].keys.clear(); f.p[0].base = 100; f.p[1].base = 200;
    const double a = value("wiggle(2, 30)", 10, 0), again = value("wiggle(2, 30)", 10, 0);
    const double y = value("wiggle(2, 30)", 10, 1);
    double spread = 0, furthest = 0;
    std::vector<double> seen;
    for (int frame = 0; frame < 120; ++frame) {
      const double v = value("wiggle(2, 30)", frame, 0);
      seen.push_back(v);
      furthest = std::max(furthest, std::abs(v - 100));
    }
    for (size_t i = 1; i < seen.size(); ++i) spread = std::max(spread, std::abs(seen[i] - seen[i - 1]));
    const double otherLayer = value("wiggle(2, 30)", 10, 0, &l);
    const double lx = l.p[0].keyedAt(10);
    check(near(a, again) && !near(a - 100, y - 200, 1e-3),
          "wiggle gives the same answer for the same frame, and x and y wiggle apart");
    check(furthest > 5 && furthest <= 30.0001 && spread < 30,
          QString("wiggle stays within its amplitude and moves smoothly (%1 at most, %2 a frame)").arg(furthest).arg(spread));
    check(!near(a - 100, otherLayer - lx, 1e-3), "two layers with the same wiggle do not move together");
    f.p[0].setExpression({}); f.p[1].setExpression({}); l.p[0].setExpression({});
  }
  {
    const double r1 = value("random()", 3), r2 = value("random()", 3), r3 = value("random()", 4);
    const double fixed1 = value("seedRandom(5, true); random(10, 20)", 3), fixed2 = value("seedRandom(5, true); random(10, 20)", 40);
    const double gauss = value("gaussRandom()", 7);
    check(near(r1, r2) && !near(r1, r3) && r1 >= 0 && r1 < 1,
          "random is the same for a frame on every render, and new each frame");
    check(near(fixed1, fixed2) && fixed1 >= 10 && fixed1 < 20, "seedRandom(seed, true) holds one value for the whole layer");
    check(std::isfinite(gauss), "gaussRandom gives a number");
  }
  {
    QString error;
    const double result = value("[1, 2] > 1 ? 'a' : 'b'", 0, 4, nullptr, &error);
    check(near(result, f.p[4].keyedAt(0)) && !error.isEmpty(),
          "an expression that ends in text leaves the value as it was and says why");
  }

  // --- properties carry it --------------------------------------------------
  {
    f.p[4].keys.clear(); f.p[4].base = 5;
    f.p[4].setExpression("value + time * 30");
    check(near(f.p[4].at(15), 20) && near(f.p[4].keyedAt(15), 5) && f.p[4].varies(),
          "a property with an expression answers with it, and varies with no keys");
    f.p[4].expressionOn = false;
    check(near(f.p[4].at(15), 5) && !f.p[4].varies(), "an expression switched off is left out");
    f.p[4].expressionOn = true;
    Layer copy = f;
    check(near(copy.p[4].at(15), 20), "a copy of a layer carries its expressions");
    const Property alone = f.p[4];
    check(alone.slot == -1 && std::isfinite(alone.at(15)), "a property copied on its own still works out a value");
    copy.p[4] = Property();
    check(copy.p[4].slot == 4, "a property set into a layer keeps its place in the layer");
    QString error;
    check(!f.p[5].setExpression("1 +", &error) && !f.p[5].hasExpression() && f.p[5].expression == "1 +",
          "text that does not compile is kept, and changes nothing");
    f.p[5].setExpression({});
  }
  {
    // Saved with the project and working again when it is opened.
    f.p[2].setExpression("value * 2");
    f.p[4].expressionOn = false;
    Scene reopened; QString error;
    const bool loaded = reopened.load(scene.json(), error);
    const bool same = loaded && reopened.layers.size() == 3 && reopened.layers[0].p[4].expression == "value + time * 30" &&
                      !reopened.layers[0].p[4].expressionOn && reopened.layers[0].p[2].hasExpression();
    ExpressionSceneScope other(&reopened);
    check(same && near(reopened.layers[0].p[2].at(0), 2), "expressions are saved with the project and work again when opened");
    f.p[4].expressionOn = true;
    f.p[2].setExpression({});
  }

  // --- pixels ---------------------------------------------------------------
  {
    Scene spin; spin.width = 200; spin.height = 200; spin.fps = 30; spin.frames = 60;
    Layer box; box.type = "shape"; box.shapeKind = "rect"; box.duration = 60; box.color = Qt::white;
    box.p[0].base = 100; box.p[1].base = 100; box.p[ShapeWidth].base = 120; box.p[ShapeHeight].base = 30;
    box.p[4].setExpression("time * 90");
    spin.layers = {box};
    const QImage a = renderCPU(spin, 0, false), b = renderCPU(spin, 15, false);
    check(a != b, "a rotation expression with no keys turns the layer from one frame to the next");
    check(!sameCompositionPixels(spin, 0, 15), "a frame with an expression moving is not taken for its neighbour");
    spin.layers[0].p[4].expressionOn = false;
    check(renderCPU(spin, 0, false) == renderCPU(spin, 15, false) && sameCompositionPixels(spin, 0, 15),
          "switched off, the same layer holds still");
    spin.layers[0].p[4].expressionOn = true;
    // The same frame on many threads at once is the same picture.
    std::vector<std::future<QImage>> jobs;
    for (int i = 0; i < 6; ++i) jobs.push_back(std::async(std::launch::async, [&spin] { return renderCPU(spin, 15, false); }));
    bool allSame = true;
    for (auto &job : jobs) allSame &= job.get() == b;
    check(allSame, "a frame with an expression renders the same on every thread");
    // A cached frame is not reused once the expression changes.
    const auto before = preRenderSignature(spin, {});
    spin.layers[0].p[4].setExpression("time * 45");
    const auto after = preRenderSignature(spin, {});
    check(before.layers.value(spin.layers[0].id) != after.layers.value(spin.layers[0].id),
          "changing an expression changes what the cache knows the layer by");
    // Reading another layer ties the two together in the cache.
    Layer other = box; other.id = QUuid::createUuid().toString(QUuid::WithoutBraces); other.name = "Other";
    other.p[4].setExpression({});
    Scene linked = spin; linked.layers = {box, other};
    linked.layers[0].p[4].setExpression("thisComp.layer(\"Other\").rotation");
    const auto first = preRenderSignature(linked, {});
    linked.layers[1].p[4].base = 30;
    const auto second = preRenderSignature(linked, {});
    check(first.global != second.global || first.layers.value(linked.layers[0].id) != second.layers.value(linked.layers[0].id),
          "a layer whose expression reads another is redrawn when that one changes");
  }

  // --- cost -----------------------------------------------------------------
  {
    Scene many; many.width = 1280; many.height = 720; many.fps = 30; many.frames = 120;
    for (int i = 0; i < 1000; ++i) {
      Layer dot; dot.type = "shape"; dot.shapeKind = "circle"; dot.duration = 120;
      dot.p[0].base = 40 + (i % 40) * 30; dot.p[1].base = 40 + (i / 40) * 26;
      dot.p[ShapeWidth].base = dot.p[ShapeHeight].base = 12;
      dot.p[0].setExpression("wiggle(3, 12)");
      dot.p[1].setExpression("wiggle(3, 12)");
      dot.p[4].setExpression("time * 120 + index * 7");
      many.layers.push_back(dot);
    }
    {
      // Expression Controls: values of a layer's own, read by name.
      const QString speed = addExpressionControl(l, "slider", "Speed");
      Property &slider = l.at(layerFindExtra(l, expressionControlCurve(speed, "Slider")));
      slider.set(0, 10);
      slider.set(30, 40);
      const QString target = addExpressionControl(l, "point", "Target");
      l.at(layerFindExtra(l, expressionControlCurve(target, "X"))).base = 320;
      l.at(layerFindExtra(l, expressionControlCurve(target, "Y"))).base = 90;
      addExpressionControl(l, "color", "Tint");
      addExpressionControl(l, "checkbox", "On");
      const QString lead = addExpressionControl(f, "layer", "Leader");
      setExpressionControlLayer(f, lead, l.id);
      check(near(value("effect(\"Speed\")(\"Slider\") * 2", 15, 4, &l), 50), "a Slider read between its keys");
      check(near(value("effect(\"speed\")(1)", 30, 4, &l), 40), "a control by number, its name's case not minded");
      check(near(value("effect(\"Target\")(\"Point\")[1]", 0, 4, &l), 90) &&
                near(value("effect(\"Tint\")(\"Color\")[3]", 0, 4, &l), 1) &&
                near(value("effect(\"Tint\")(\"Color\")[0] + effect(\"On\")(\"Checkbox\")", 0, 4, &l), 1),
            "a Point's parts, a Colour's four and a Checkbox");
      check(near(value("thisComp.layer(\"Leader\").effect(\"Speed\")(\"Slider\")", 0), 10), "another layer's control");
      {
        QString why;
        const double got = value("effect(\"Leader\")(\"Layer\").index * 1000 + effect(\"Leader\")(\"Layer\").position[0]", 0, 4, nullptr, &why);
        check(near(got, 2000 + l.p[0].at(0)), QString("a Layer control gives its layer (%1 %2)").arg(got).arg(why));
      }
      check(near(value("effect(\"Target\").param(\"Point\")[0]", 0, 4, &l), 320), "param() reads a control as calling it does");
      QString why;
      value("effect(\"Speed\")(\"Angle\")", 0, 4, &l, &why);
      check(why.contains("Slider"), "asking a control for what it does not have says what it has: " + why);
      renameExpressionControl(l, speed, "Pace");
      check(near(value("effect(\"Pace\")(\"Slider\")", 30, 4, &l), 40), "a renamed control is read by its new name");
      removeExpressionControl(l, speed);
      check(layerFindExtra(l, expressionControlCurve(speed, "Slider")) < 0 && expressionControls(l).size() == 3,
            "a removed control takes its curve with it");
      l.surfaceEffects["Wavy Blur"] = QJsonObject{{"amount", 7.5}};
      check(near(value("effect(\"Wavy Blur\")(\"amount\")", 0, 4, &l), 7.5), "another effect's setting, by its name");
    }
    ExpressionSceneScope manyScope(&many);
    QElapsedTimer clock; clock.start();
    double sum = 0;
    const int frames = 20;
    for (int frame = 0; frame < frames; ++frame)
      for (const auto &dot : many.layers) sum += dot.p[0].at(frame) + dot.p[1].at(frame) + dot.p[4].at(frame);
    const double perFrame = double(clock.nsecsElapsed()) / 1e6 / frames;
    check(std::isfinite(sum) && perFrame < 8,
          QString("3000 expressions (wiggle and index) cost %1 ms a frame").arg(perFrame, 0, 'f', 3));
  }
  return failures;
}
