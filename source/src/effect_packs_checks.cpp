#include "effect_packs.h"
#include "effect_editor.h"
#include "shader_cpu.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// --effect-packs-self-test: the .wfx packs are found, read their settings the
// way the built-in effects did, tell the engine what it needs to know about
// them, and draw the same on the card as on the CPU.
namespace {
// The context-reading effects compared on the card, each where and when it
// would really run: an off-centre box, a preview's scale, a moment in time.
struct SceneEffectCase { QString id; QJsonObject settings; QRectF box; double scale, seconds, frame; };
std::vector<SceneEffectCase> sceneEffectCases() {
  const QRectF box(7, 6, 81, 52);
  return {
      {"Film Grain", {}, box, 1, 0, 0},
      {"Film Grain", {{"amount", 80.}, {"size", 3.}, {"colour", 70.}, {"seed", 41.}, {"animated", true}, {"rate", 24.}}, box, .75, 1.375, 33},
      {"Vignette", {}, box, 1, 0, 0},
      {"Vignette", {{"amount", 90.}, {"size", 20.}, {"softness", 80.}, {"color", "#ff8040"}, {"roundness", 60.}}, box, .5, 0, 0},
      {"Sparkles", {}, box, 1, 0, 0},
      {"Sparkles", {{"count", 12.}, {"size", 40.}, {"angle", 30.}, {"glow", 120.}, {"twinkle", 2.5}, {"seed", 9.}}, box, .75, 2.7, 81},
      {"Light Leak", {}, box, 1, 0, 0},
      {"Light Leak", {{"intensity", 150.}, {"size", 80.}, {"speed", 1.3}, {"seed", 5.}}, box, .5, 3.25, 97},
      {"TV Screen", {}, box, 1, 0, 0},
      {"TV Screen", {{"border", 15.}, {"roundness", 80.}, {"curve", 70.}, {"shade", 90.}, {"shadeWidth", 40.}, {"frame", "#20304080"}}, box, 1, 0, 0},
      {"RGB Split", {}, box, 1, 0, 0},
      {"RGB Split", {{"amount", 6.5}, {"angle", 125.}}, box, .75, 0, 0},
      {"Glitch", {}, box, 1, .5, 15},
      {"Glitch", {{"amount", 30.}, {"slices", 9.}, {"density", 70.}, {"split", 5.}, {"speed", 7.}, {"seed", 3.}}, box, .5, 1.9, 57},
      {"Kaleidoscope", {}, box, 1, 0, 0},
      {"Kaleidoscope", {{"segments", 5.}, {"angle", 400.}, {"x", 40.}, {"y", 60.}, {"zoom", 150.}}, box, 1, 0, 0},
      {"Zoom Blur", {}, box, 1, 0, 0},
      {"Zoom Blur", {{"amount", 60.}, {"x", 30.}, {"y", 70.}}, box, 1, 0, 0},
      {"Find Edges", {}, box, 1, 0, 0},
      {"Find Edges", {{"invert", true}, {"blend", 30.}, {"strength", 250.}}, box, 1, 0, 0},
      {"Emboss", {}, box, 1, 0, 0},
      {"Emboss", {{"direction", 40.}, {"relief", 3.5}, {"contrast", 180.}, {"blend", 25.}}, box, .5, 0, 0},
      {"Polar Coordinates", {}, box, 1, 0, 0},
      {"Polar Coordinates", {{"amount", 65.}, {"type", 1}, {"edge", 1}}, box, 1, 0, 0},
      {"Polar Coordinates", {{"type", 0}, {"edge", 0}}, box, 1, 0, 0},
      {"Linear Wipe", {{"completion", 40.}}, box, 1, 0, 0},
      {"Linear Wipe", {{"completion", 65.}, {"angle", 200.}, {"feather", 25.}}, box, 1, 0, 0},
      {"Radial Wipe", {{"completion", 30.}}, box, 1, 0, 0},
      {"Radial Wipe", {{"completion", 55.}, {"start", 45.}, {"feather", 30.}, {"direction", 2}, {"centerX", 30.}, {"centerY", 60.}}, box, 1, 0, 0},
      {"Iris Wipe", {{"completion", 40.}}, box, 1, 0, 0},
      {"Iris Wipe", {{"completion", 45.}, {"shape", 3}, {"feather", 6.}, {"rotation", 20.}}, box, .75, 0, 0},
      {"Iris Wipe", {{"completion", 35.}, {"shape", 1}, {"closing", true}}, box, 1, 0, 0},
      {"Venetian Blinds", {{"completion", 50.}}, box, 1, 0, 0},
      {"Venetian Blinds", {{"completion", 30.}, {"angle", 35.}, {"width", 14.}, {"feather", 5.}}, box, .5, 0, 0},
      {"Block Dissolve", {{"completion", 50.}}, box, 1, 0, 0},
      {"Block Dissolve", {{"completion", 70.}, {"blockWidth", 7.}, {"blockHeight", 11.}, {"softness", 40.}, {"seed", 77.}}, box, .75, 0, 0},
      {"Wave Warp", {}, box, 1, .4, 12},
      {"Wave Warp", {{"type", 1}, {"height", 6.}, {"width", 23.}, {"direction", 30.}, {"speed", 2.}, {"phase", 40.}, {"edge", 2}}, box, .75, 1.3, 39},
      {"Wave Warp", {{"type", 2}, {"height", -4.}, {"edge", 1}}, box, 1, 0, 0},
      {"Wave Warp", {{"type", 3}, {"height", 5.}}, box, 1, 0, 0},
      {"Ripple", {}, box, 1, .7, 21},
      {"Ripple", {{"amplitude", 5.}, {"wavelength", 17.}, {"speed", -1.5}, {"centerX", 30.}, {"radius", 40.}, {"edge", 2}}, box, 1, 2.2, 66},
      {"Twirl", {}, box, 1, 0, 0},
      {"Twirl", {{"angle", -720.}, {"radius", 30.}, {"centerX", 40.}, {"centerY", 60.}}, box, .75, 0, 0},
      {"Bulge", {}, box, 1, 0, 0},
      {"Bulge", {{"strength", -70.}, {"radius", 25.}, {"centerX", 60.}}, box, 1, 0, 0},
      {"Chromatic Aberration", {}, box, 1, 0, 0},
      {"Chromatic Aberration", {{"amount", 14.}, {"centerX", 30.}, {"centerY", 70.}, {"swap", true}}, box, .5, 0, 0},
      {"Halftone", {}, box, 1, 0, 0},
      {"Halftone", {{"size", 7.}, {"angle", -30.}, {"mode", 1}, {"softness", 60.}, {"amount", 70.}}, box, .75, 0, 0},
      {"Halftone", {{"size", 12.}, {"angle", 400.}, {"mode", 2}}, box, 1, 0, 0},
      {"Scanlines", {}, box, 1, 0, 0},
      {"Scanlines", {{"spacing", 7.}, {"thickness", 30.}, {"intensity", 80.}, {"angle", 20.}, {"speed", -35.}, {"mask", true}, {"softness", 10.}}, box, .75, 1.7, 51},
      {"Scanlines", {{"spacing", 6.}, {"mask", true}, {"speed", 12.}}, box, 1, 2.5, 75},
      {"Light Sweep", {}, box, 1, 0, 0},
      {"Light Sweep", {{"position", 30.}, {"angle", -40.}, {"width", 30.}, {"softness", 20.}, {"intensity", 150.}, {"color", "#ffd080"}, {"speed", .7}}, box, .5, 2.3, 69},
      {"Pattern", {}, box, 1, 0, 0},
      {"Pattern", {{"type", 1}, {"size", 13.}, {"angle", 35.}, {"thickness", 30.}, {"scrollX", 40.}, {"colorB", "#00000000"}}, box, .75, 1.2, 36},
      {"Pattern", {{"type", 2}, {"size", 9.}, {"angle", -10.}, {"softness", 3.}, {"opacity", 60.}}, box, 1, 0, 0},
      {"Pattern", {{"type", 3}, {"size", 15.}, {"thickness", 70.}, {"scrollY", -25.}}, box, 1, .8, 24},
      {"Pattern", {{"type", 4}, {"size", 11.}, {"angle", 3645.}, {"colorA", "#2040ff80"}}, box, .5, 0, 0},
      {"Pattern", {{"type", 5}, {"size", 20.}, {"thickness", 80.}}, box, 1, 0, 0},
      {"Sunburst", {}, box, 1, 0, 0},
      {"Sunburst", {{"rays", 9.}, {"centerX", 30.}, {"centerY", 70.}, {"angle", 100.}, {"spin", -45.}, {"twist", 90.}, {"fade", 60.}, {"opacity", 80.}, {"colorB", "#4080ff80"}}, box, .75, 1.6, 48},
      {"Shake", {}, box, 1, .6, 18},
      {"Shake", {{"amount", 35.}, {"rotation", 8.}, {"speed", 3.}, {"zoom", 120.}, {"seed", 7.}, {"mirror", true}}, box, .75, 2.3, 69},
      {"Mirror", {}, box, 1, 0, 0},
      {"Mirror", {{"mode", 4}, {"x", 40.}, {"y", 65.}}, box, 1, 0, 0},
      {"Mirror", {{"mode", 3}, {"y", 30.}}, box, .5, 0, 0},
      {"Mirror", {{"mode", 1}, {"x", 62.5}}, box, 1, 0, 0},
      {"Pixelate", {}, box, 1, 0, 0},
      {"Pixelate", {{"size", 7.}}, box, .75, 0, 0},
      {"Pixelate", {{"size", 5.}}, box, 1, 0, 0},
      {"Directional Blur", {}, box, 1, 0, 0},
      {"Directional Blur", {{"angle", 35.}, {"length", 12.}}, box, .75, 0, 0},
      {"Directional Blur", {{"angle", 100.}, {"length", 30.}}, box, .5, 0, 0},
      {"Sharpen", {}, box, 1, 0, 0},
      {"Sharpen", {{"amount", 250.}, {"radius", 3.}, {"threshold", 20.}}, box, .75, 0, 0},
      {"Light Rays", {}, box, 1, 0, 0},
      {"Light Rays", {{"intensity", 180.}, {"length", 70.}, {"centerX", 20.}, {"centerY", 80.}, {"threshold", 30.}, {"color", "#80c0ff"}, {"raysOnly", true}}, box, .5, 0, 0},
      {"Light Rays", {{"length", 0.}}, box, 1, 0, 0},
      {"Speed Lines", {}, box, 1, .4, 12},
      {"Speed Lines", {{"mode", 1}, {"count", 90.}, {"thickness", 7.}, {"length", 45.}, {"variation", 80.}, {"colour", "#ff408080"}, {"opacity", 70.}, {"speed", 0.}, {"x", 35.}, {"y", 60.}, {"linesOnly", true}, {"seed", 4.}}, box, .75, 0, 0},
      {"Speed Lines", {{"count", 40.}, {"thickness", 5.}, {"travel", 300.}, {"angle", 30.}, {"speed", 6.}}, box, 1, 1.35, 40},
      {"Noise", {}, box, 1, 0, 0},
      {"Noise", {{"amount", 80.}, {"size", 3.}, {"colour", 60.}, {"mode", 3}, {"speed", 12.}, {"seed", 5.}}, box, .75, 1.25, 37},
      {"Noise", {{"mode", 0}, {"size", 2.5}}, box, 1, 0, 0},
      {"Turbulent Noise", {}, box, 1, 0, 0},
      {"Turbulent Noise", {{"type", 1}, {"scale", 40.}, {"complexity", 6.}, {"contrast", 180.}, {"brightness", -10.}, {"evolution", 2.5}, {"speed", .5}, {"seed", 9.}, {"dark", "#102040"}, {"light", "#ffd080"}, {"mode", 4}, {"amount", 70.}}, box, .75, 1.4, 42},
      {"Turbulent Noise", {{"scale", 6.}, {"complexity", 3.}}, box, 1, .3, 9},
      {"Displacement Map", {}, box, 1, .5, 15},
      {"Displacement Map", {{"horizontal", -12.}, {"vertical", 30.}, {"scale", 30.}, {"complexity", 4.}, {"evolution", 1.5}, {"speed", 0.}, {"seed", 3.}, {"edge", 2}}, box, .75, 0, 0},
      {"Displacement Map", {{"edge", 1}, {"horizontal", 8.}, {"vertical", 0.}}, box, 1, 0, 0},
      {"Tile Repeat", {{"tileWidth", 40.}, {"tileHeight", 50.}}, box, 1, 0, 0},
      {"Tile Repeat", {{"tileWidth", 30.}, {"tileHeight", 45.}, {"gapX", 20.}, {"gapY", 35.}, {"shiftX", 37.}, {"shiftY", -20.}, {"mirror", true}}, box, .75, 0, 0},
      {"Tile Repeat", {{"tileWidth", 50.}, {"tileHeight", 50.}, {"stagger", 50.}, {"staggerAlong", 1}, {"alternate", 30.}, {"alternateUpDown", true}}, box, 1, 0, 0},
      {"Tile Repeat", {{"tileWidth", 60.}, {"tileHeight", 70.}, {"shiftX", 12345.}, {"alternate", -250.}, {"stagger", 25.}}, box, 1, 0, 0},
      {"Tile Repeat", {{"tileWidth", 50.}, {"extend", 10.}, {"mirror", true}}, box, .5, 0, 0},
  };
}
// The colour effects compared on the card and on the processor, each at
// settings that make it do something.
std::vector<std::pair<QString, QJsonObject>> colourEffectCases() {
  return {
      {"Curves", {{"master", QJsonArray{0, .2, .5, .85, 1}}, {"blue", QJsonArray{0, .3, .6, .8, 1}}}},
      {"HSL", {{"hue", 40.}, {"saturation", 20.}, {"lightness", -10.}}},
      {"Levels", {{"inputBlack", 12.}, {"inputWhite", 240.}, {"gamma", 1.4}, {"outputBlack", 5.}, {"outputWhite", 250.}}},
      {"Exposure", {{"exposure", .4}, {"offset", .01}, {"gamma", .8}}},
      {"Brightness & Contrast", {{"brightness", 10.}, {"contrast", 25.}}},
      {"Vibrance", {{"vibrance", 40.}, {"saturation", 10.}}},
      {"Color Balance", {{"shadowsRed", 20.}, {"midtonesGreen", -15.}, {"highlightsBlue", -15.}}},
      {"Three-Way Color Corrector", {{"shadows", "#6070a0"}, {"highlights", "#90a090"}, {"lift", .03}, {"gamma", 1.2}, {"gain", 1.1}, {"saturation", 15.}, {"balance", 20.}}},
      {"Temperature & Tint", {{"temperature", 30.}, {"tint", -10.}}},
      {"Black & White", {{"reds", 150.}, {"blues", 50.}, {"tintAmount", 30.}, {"tintColor", "#ffcc88"}}},
      {"Tint", {{"color", "#3a8fff"}, {"amount", 60.}}},
      {"Gradient Map", {{"shadows", "#202060"}, {"midtones", "#e04070"}, {"highlights", "#fff0c0"}, {"midpoint", 40.}, {"amount", 80.}}},
      {"Posterize", {{"levels", 5.}}},
      {"Threshold", {{"level", 100.}, {"softness", 25.}}}};
}
// The largest and the mean difference between two pictures, compared as they
// will be seen: premultiplied, so colour under a clear pixel does not count.
std::pair<double, double> pictureDifference(const QImage &first, const QImage &second) {
  const QImage a = first.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied),
               b = second.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
  if (a.size() != b.size() || a.isNull()) return {1e9, 1e9};
  double most = 0, total = 0;
  for (int y = 0; y < a.height(); ++y) {
    const float *pa = reinterpret_cast<const float *>(a.constScanLine(y)), *pb = reinterpret_cast<const float *>(b.constScanLine(y));
    for (int x = 0; x < a.width() * 4; ++x) {
      const double d = std::abs(double(pa[x]) - double(pb[x]));
      most = std::max(most, d);
      total += d;
    }
  }
  return {most, total / (double(a.width()) * a.height() * 4)};
}
}  // namespace
int checkEffectPacks(QStringList &report) {
  int failures = 0;
  auto check = [&](bool passed, const QString &label) {
    report << label + (passed ? ": PASS" : ": FAIL");
    failures += !passed;
  };
  {
    // The effects folder is read like After Effects' Plug-ins folder: a pack
    // in a maker's folder of its own, at any depth, is found; the folder's
    // own come first; nothing inside an OpenFX bundle is taken for one.
    QTemporaryDir place;
    const QStringList made{"top.wfx", "Maker/inside.wfx", "Maker/Deeper/deep.wfx", "Other/look.wfxs",
                           "Plug.ofx.bundle/Contents/Win64/hidden.wfx", "notes.txt"};
    bool written = true;
    for (const QString &name : made) {
      QDir().mkpath(QFileInfo(place.path() + "/" + name).absolutePath());
      QFile file(place.path() + "/" + name);
      written = written && file.open(QIODevice::WriteOnly) && file.write("x") == 1;
    }
    const QStringList packs = effectFilesIn(place.path(), {"*.wfx"}, true);
    const QStringList texts = effectFilesIn(place.path(), {"*.wfxs"}, true);
    const QStringList topOnly = effectFilesIn(place.path(), {"*.wfx"}, false);
    check(written && packs == QStringList{"top.wfx", "Maker/Deeper/deep.wfx", "Maker/inside.wfx"} &&
              texts == QStringList{"Other/look.wfxs"} && topOnly == QStringList{"top.wfx"},
          "the effects folder finds packs in makers' folders inside it, not inside a plug-in bundle (" +
              packs.join(", ") + ")");
  }
  const std::vector<std::pair<QString, QString>> expected{
      {"Curves", "Wavy-Color"}, {"HSL", "Wavy-Color"}, {"Levels", "Wavy-Color"}, {"Exposure", "Wavy-Color"},
      {"Brightness & Contrast", "Wavy-Color"}, {"Vibrance", "Wavy-Color"}, {"Color Balance", "Wavy-Color"},
      {"Three-Way Color Corrector", "Wavy-Color"}, {"Temperature & Tint", "Wavy-Color"}, {"Black & White", "Wavy-Color"},
      {"Tint", "Wavy-Stylize"}, {"RGB Split", "Wavy-Stylize"}, {"Film Grain", "Wavy-Stylize"}, {"Vignette", "Wavy-Stylize"},
      {"Tile Repeat", "Wavy-Stylize"},
      {"Noise", "Wavy-Noise"}, {"Turbulent Noise", "Wavy-Noise"}, {"Displacement Map", "Wavy-Noise"},
      {"Linear Wipe", "Wavy-Transition"}, {"Radial Wipe", "Wavy-Transition"}};
  QStringList missing;
  for (const auto &[id, file] : expected) {
    const PackEffect *e = packEffect(id);
    if (!e || QFileInfo(e->file).completeBaseName() != file) missing << id;
  }
  check(missing.isEmpty(), QString("the 20 first-party effects come from their four packs%1")
                               .arg(missing.isEmpty() ? QString() : " (missing: " + missing.join(", ") + ")"));
  check(packProblems().isEmpty(), "no pack was refused" + (packProblems().isEmpty() ? QString() : ": " + packProblems().join("; ")));
  if (!missing.isEmpty()) return failures;

  // ---- settings, read as the built-in effects read them -------------------------
  {
    Layer l;
    l.surfaceEffects["Color Balance"] = QJsonObject{{"preserveLuminosity", false}, {"shadowsRed", 20.}};
    const auto off = packEffectValues(l, *packEffect("Color Balance"), 0);
    l.surfaceEffects["Color Balance"] = QJsonObject{{"preserveLuminosity", true}};
    const auto on = packEffectValues(l, *packEffect("Color Balance"), 0);
    check(off.value(9) == 0 && on.value(9) == 1 && off.value(0) == 20, "a switch saved as true/false or as a number reads the same");
    l.surfaceEffects["Three-Way Color Corrector"] = QJsonObject{{"shadows", "#6070a0"}};
    const auto wheels = packEffectValues(l, *packEffect("Three-Way Color Corrector"), 0);
    check(std::abs(wheels.value(0) - 0x60 / 255.f) < 1e-6 && std::abs(wheels.value(2) - 0xa0 / 255.f) < 1e-6 &&
              std::abs(wheels.value(4) - 128 / 255.f) < 1e-6,
          "colours read from the project, and a colour never set rests at its default");
    l.surfaceEffects["Curves"] = QJsonObject{{"red", QJsonArray{0, .3, .55, .8, 1}}};
    const auto curves = packEffectValues(l, *packEffect("Curves"), 0);
    check(curves.size() == 20 && std::abs(curves.value(6) - .3f) < 1e-6 && std::abs(curves.value(1) - .25f) < 1e-6,
          "curves read as five points each, a curve never set is the straight line");
    l.surfaceEffects["HSL"] = QJsonObject{{"hue", 999.}};
    check(packEffectValues(l, *packEffect("HSL"), 0).value(0) == 180, "a setting beyond its range is held to it");
    // An animated slider: a curve among the layer's extra curves.
    const int curve = layerAddExtra(l, packCurveId("HSL", "saturation"), 0);
    l.at(curve).set(0, 0); l.at(curve).set(10, 40);
    check(std::abs(packEffectValues(l, *packEffect("HSL"), 5).value(1) - 20) < 1e-4, "an animated slider is read at the frame");
    // The wipes keep their sliders where they always were, as layer properties.
    l.p[WipeLinearCompletion].set(0, 0); l.p[WipeLinearCompletion].set(20, 80);
    l.p[WipeLinearAngle].base = 45;
    const auto wipe = packEffectValues(l, *packEffect("Linear Wipe"), 10);
    check(std::abs(wipe.value(0) - 40) < 1e-4 && wipe.value(1) == 45, "the wipes' sliders are read from the layer's own keyframes");
  }
  // ---- what the engine asks of them ------------------------------------------------
  {
    Layer still; still.appliedEffects["Noise"] = true; still.surfaceEffects["Noise"] = QJsonObject{{"speed", 0.}};
    Layer moving = still; moving.surfaceEffects["Noise"] = QJsonObject{{"speed", 5.}};
    Layer turbulent; turbulent.appliedEffects["Turbulent Noise"] = true; // its speed starts at 1
    Layer keyed; keyed.appliedEffects["HSL"] = true;
    const int c = layerAddExtra(keyed, packCurveId("HSL", "hue"), 0); keyed.at(c).set(0, 0); keyed.at(c).set(10, 30);
    check(!layerHasAnimatedEffect(still) && layerHasAnimatedEffect(moving) && layerHasAnimatedEffect(turbulent) && layerHasAnimatedEffect(keyed),
          "a moving noise and an animated slider change the picture with time; a still noise does not");
    Layer split; split.appliedEffects["RGB Split"] = true; split.surfaceEffects["RGB Split"] = QJsonObject{{"amount", 12.}};
    Layer shaken; shaken.appliedEffects["Displacement Map"] = true;
    check(layerPackReach(split) == 16 && layerPackReach(shaken) == 24 && layerPackReach(still) == 0,
          QString("RGB Split and Displacement Map make room around the layer (%1, %2)").arg(layerPackReach(split)).arg(layerPackReach(shaken)));
    Layer idle; idle.appliedEffects["HSL"] = true;
    Layer busy = idle; busy.surfaceEffects["HSL"] = QJsonObject{{"hue", 10.}};
    check(!layerHasPackEffect(idle, 0) && layerHasPackEffect(busy, 0) && layerCarriesPackEffect(idle),
          "an effect set to change nothing is skipped, but still counts as carried");
  }
  // ---- an effect whose pack is not installed ----------------------------------------
  {
    QImage source(24, 24, QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(200, 100, 50));
    Layer l; l.appliedEffects["Some Effect Nobody Installed"] = true;
    const QImage result = applyLayerEffects(source, l, {0, 0, 24, 24}, 1, 0);
    check(result == source, "an effect whose pack is missing is passed over, and the picture is left as it was");
  }
  // ---- pixel-by-pixel effects run a strip of rows at a time ----------------------------
  {
    // Film grain and a vignette at the end of a chain run as one, sixteen rows
    // at a time across the cores, told where each strip lies: the same bytes as
    // running each over the whole raster and converting once at the end, at a
    // raster whose height is no multiple of sixteen, a box away from its
    // corner and a grain cell bigger than a pixel.
    QImage source(301, 177, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < source.height(); ++y)
      for (int x = 0; x < source.width(); ++x) {
        const int a = (x * 7 + y * 3) % 256;
        source.setPixelColor(x, y, QColor::fromRgb(QRgb(qRgba((x * 5) % 256, (y * 9) % 256, (x + y) % 256, a))));
      }
    Layer l;
    l.appliedEffects = QJsonObject{{"Film Grain", true}, {"Vignette", true}};
    l.surfaceEffects["Film Grain"] = QJsonObject{{"amount", 60}, {"size", 2.5}, {"colour", 50}, {"seed", 7}};
    l.surfaceEffects["Vignette"] = QJsonObject{{"amount", 70}, {"size", 30}};
    const QRectF box(13, 9, 260, 150);
    const bool perPixel = packEffect("Film Grain") && packEffectPerPixel(*packEffect("Film Grain")) &&
                          packEffect("Vignette") && packEffectPerPixel(*packEffect("Vignette"));
    QImage whole = source.convertToFormat(QImage::Format_RGBA32FPx4);
    for (const QString &name : layerEffectOrder(l))
      if (const PackEffect *pack = packEffect(name); pack && l.appliedEffects.value(name).toBool())
        runPackEffectInPlace(whole, *pack, packEffectValues(l, *pack, 3), box, 1.5, .1, 3, &l);
    whole = whole.convertToFormat(source.format());
    QImage strips;
    {
      // The CPU's strips are what is being checked: with a card the two
      // effects would run there now, as one pass each.
      CpuPixelScope only;
      EffectTimeScope time(.1, 3);
      strips = applyLayerEffects(source, l, box, 1.5, 3);
    }
    check(perPixel && strips.format() == whole.format() && strips == whole && strips != source,
          "film grain and a vignette run a strip at a time give the same bytes as run over the whole raster");
  }
  // ---- Film Grain and Vignette, four pixels at a time ----------------------------------
  {
    // Since Oct 20 2026 both work on four pixels at once (SSE2). What they make
    // is pinned to what they made a pixel at a time: each hash is of the
    // picture the one-at-a-time pack gave for the same input -- widths four
    // does not divide, parts of a raster, a tinted and a round vignette,
    // animated grain, the Twitter promo's settings, and colours holding NaN,
    // infinities, -0, a denormal and values past 0..1 beside clear pixels. A
    // build that clamps with minps/maxps or multiplies by 1 / 32767.5 instead
    // of dividing changes two and three of them. A deliberate change to either
    // look renews them.
    struct Pinned {
      const char *id;
      int width, height;
      float values[8];
      int count;
      double scale, seconds, boxX, boxY, boxW, boxH;
      int originX, originY, rasterW, rasterH;
      bool odd;
      uint64_t hash;
    };
    const Pinned cases[] = {
        {"Film Grain", 37, 9, {20, 1.5f, 30, 1, 0, 24}, 6, 1, 0, 0, 0, 37, 9, 0, 0, 0, 0, false, 0x1bedcaeb1d4a83d6ull},
        {"Film Grain", 23, 11, {60, 2.5f, 50, 7, 1, 24}, 6, .75, 1.375, 3, -2, 40, 30, 5, 40, 100, 100, true, 0x4d52d8d94f2a9fd7ull},
        {"Film Grain", 41, 7, {6, 1.3f, 10, 1, 0, 24}, 6, .5, 0, 0, 0, 960, 540, 446, 267, 960, 540, false, 0x7810f64f650f46bcull},
        {"Film Grain", 61, 5, {100, .5f, 100, 9999, 1, 120}, 6, 1.7, 7.9, 0, 0, 0, 0, 0, 0, 0, 0, true, 0x1e86d619090d3f2cull},
        {"Vignette", 37, 9, {40, 60, 60, 0, 0, 0, 1, 0}, 8, 1, 0, 3, -2, 30, 12, 0, 0, 0, 0, false, 0x78e820e7dcf0a0a8ull},
        {"Vignette", 29, 13, {90, 20, 80, 1, .5f, .25f, 1, 60}, 8, .5, 0, 7, 6, 81, 52, 11, 30, 120, 90, true, 0xa0ea2e12a2325a04ull},
        {"Vignette", 64, 16, {22, 60, 80, 0, 0, 0, 1, 0}, 8, .5, 0, 0, 0, 960, 540, 100, 262, 960, 540, false, 0x5fbcab25e4c745a8ull},
        {"Vignette", 66, 11, {100, 0, 0, 0, 1, 0, 1, 100}, 8, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, true, 0x11a7d3593ed8473aull},
        {"Vignette", 50, 9, {75, 50, 40, .2f, .1f, 0, 1, 30}, 8, 1, 0, 0, 0, 200, 120, 0, 0, 200, 120, false, 0x39c34aca7fcbb717ull}};
    // A case's picture: a fixed run of colours and coverages, some of them odd.
    auto picture = [](std::vector<float> &pixels, uint32_t seed, bool odd) {
      static const uint32_t oddBits[] = {0x7fc00000u, 0xffc00000u, 0x7f800000u, 0xff800000u, 0x80000000u,
                                         0x00000001u, 0x3f800001u, 0xbf000000u, 0x40400000u};
      uint32_t s = seed * 2654435761u + 1u;
      auto next = [&] { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
      for (size_t i = 0; i < pixels.size(); i += 4) {
        for (int c = 0; c < 3; ++c) pixels[i + c] = float(next() >> 8) / 16777216.f;
        const uint32_t kind = next() % 16;
        pixels[i + 3] = kind == 0 ? 0.f : kind == 1 ? -0.f : kind < 9 ? 1.f : float(next() >> 8) / 16777216.f;
        if (odd) {
          for (int c = 0; c < 3; ++c)
            if (next() % 6 == 0) std::memcpy(&pixels[i + c], &oddBits[next() % 9], 4);
          if (next() % 13 == 0) std::memcpy(&pixels[i + 3], &oddBits[next() % 9], 4);
        }
      }
    };
    auto hashOf = [](const std::vector<float> &pixels) {
      uint64_t h = 1469598103934665603ull;
      const unsigned char *b = reinterpret_cast<const unsigned char *>(pixels.data());
      for (size_t i = 0; i < pixels.size() * 4; ++i) { h ^= b[i]; h *= 1099511628211ull; }
      return h;
    };
    QStringList wrong;
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
      const Pinned &c = cases[k];
      const PackEffect *pack = packEffect(c.id);
      std::vector<float> pixels(size_t(c.width) * size_t(c.height) * 4);
      picture(pixels, uint32_t(k), c.odd);
      WfxContext context{};
      context.structSize = sizeof(WfxContext);
      context.depth = 32;
      context.scale = c.scale;
      context.seconds = c.seconds;
      context.frame = c.seconds * 30;
      context.boxX = c.boxX;
      context.boxY = c.boxY;
      context.boxWidth = c.boxW;
      context.boxHeight = c.boxH;
      context.originX = c.originX;
      context.originY = c.originY;
      context.rasterWidth = c.rasterW;
      context.rasterHeight = c.rasterH;
      const int failed = pack && pack->api->process
                             ? pack->api->process(pixels.data(), c.width, c.height, c.width * 16, c.values, c.count, &context)
                             : -1;
      if (failed || hashOf(pixels) != c.hash) wrong << QString("%1 #%2").arg(c.id).arg(k);
    }
    check(wrong.isEmpty(), "Film Grain and Vignette, four pixels at a time, give to the bit the pictures they gave one at a time" +
                               (wrong.isEmpty() ? QString() : " (not " + wrong.join(", ") + ")"));
  }
  // ---- the effects added in October 2026 ----------------------------------------------
  {
    // One effect, set as a layer would carry it, over a straight float picture.
    auto run = [&](const QString &id, const QJsonObject &settings, QImage picture, double seconds = 0,
                   QRectF box = QRectF()) {
      const PackEffect *pack = packEffect(id);
      if (!pack) return QImage();
      Layer l;
      l.appliedEffects[id] = true;
      l.surfaceEffects[id] = settings;
      picture = picture.convertToFormat(QImage::Format_RGBA32FPx4);
      if (box.isNull()) box = QRectF(picture.rect());
      runPackEffect(picture, *pack, packEffectValues(l, *pack, seconds * 30), box, 1, seconds, seconds * 30, &l);
      return picture;
    };
    auto at = [](const QImage &image, int x, int y) {
      const auto *p = reinterpret_cast<const float *>(image.constScanLine(y)) + x * 4;
      return std::array<float, 4>{p[0], p[1], p[2], p[3]};
    };
    QImage grey(80, 60, QImage::Format_RGBA32FPx4);
    grey.fill(QColor::fromRgbF(.5f, .5f, .5f, 1.f));
    QImage ramp(256, 4, QImage::Format_RGBA32FPx4);
    for (int x = 0; x < 256; ++x)
      for (int y = 0; y < 4; ++y) ramp.setPixelColor(x, y, QColor::fromRgbF(x / 255.f, x / 255.f, x / 255.f, 1.f));
    const QStringList added{"Gradient Map", "Posterize", "Halftone", "Scanlines", "Iris Wipe", "Venetian Blinds",
                            "Block Dissolve", "Wave Warp", "Ripple", "Twirl", "Bulge", "Light Sweep", "Chromatic Aberration"};
    QStringList absent;
    for (const QString &id : added)
      if (!packEffect(id)) absent << id;
    check(absent.isEmpty(), QString("the new effects are there%1").arg(absent.isEmpty() ? QString() : ": missing " + absent.join(", ")));
    if (absent.isEmpty()) {
      // Gradient Map: black to the shadows' colour, white to the highlights'.
      const QImage mapped = run("Gradient Map", QJsonObject{{"shadows", "#ff0000"}, {"highlights", "#0000ff"}, {"midtones", "#00ff00"}}, ramp);
      const auto dark = at(mapped, 0, 1), light = at(mapped, 255, 1), middle = at(mapped, 127, 1);
      check(dark[0] > .99f && dark[2] < .01f && light[2] > .99f && light[0] < .01f && middle[1] > .9f,
            "Gradient Map turns black into the shadows' colour, white into the highlights', the middle into the midtones'");
      // Posterize: two levels leave nothing but 0 and 1.
      const QImage poster = run("Posterize", QJsonObject{{"levels", 2}}, ramp);
      bool twoLevels = true;
      for (int x = 0; x < 256; ++x) {
        const float v = at(poster, x, 0)[0];
        twoLevels = twoLevels && (v == 0.f || v == 1.f);
      }
      check(twoLevels && at(poster, 10, 0)[0] == 0.f && at(poster, 245, 0)[0] == 1.f, "Posterize at two levels leaves only black and white");
      // Halftone: a mid grey becomes dots -- some places inked, some not.
      const QImage dots = run("Halftone", QJsonObject{{"mode", 1}, {"size", 12}, {"softness", 0}}, grey);
      int dark2 = 0, light2 = 0;
      for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 80; ++x) { const float v = at(dots, x, y)[0]; dark2 += v < .2f; light2 += v > .8f; }
      check(dark2 > 600 && light2 > 600, QString("Halftone turns a mid grey into dots (%1 inked, %2 bare)").arg(dark2).arg(light2));
      // Scanlines: rows alternate dark and light, and only rows.
      const QImage lines = run("Scanlines", QJsonObject{{"spacing", 4}, {"intensity", 80}}, grey);
      float lowest = 1, highest = 0;
      bool rowsEven = true;
      for (int y = 0; y < 8; ++y) {
        const float v = at(lines, 0, y)[0];
        lowest = std::min(lowest, v); highest = std::max(highest, v);
        for (int x = 1; x < 80; ++x) rowsEven = rowsEven && at(lines, x, y)[0] == v;
      }
      check(lowest < .2f && highest > .45f && rowsEven, "Scanlines darken lines across the picture, the same all along each line");
      Layer scrolling; scrolling.appliedEffects["Scanlines"] = true; scrolling.surfaceEffects["Scanlines"] = QJsonObject{{"speed", 30}};
      Layer still; still.appliedEffects["Scanlines"] = true; still.surfaceEffects["Scanlines"] = QJsonObject{{"speed", 0}};
      check(layerPackEffectMoves(scrolling, 0) && !layerPackEffectMoves(still, 0), "scrolling Scanlines move by themselves; still ones do not");
      // Iris Wipe: half open, the middle gone and the corners kept; closing, the other way round.
      QImage solid(101, 101, QImage::Format_RGBA32FPx4);
      solid.fill(Qt::white);
      const QImage open = run("Iris Wipe", QJsonObject{{"completion", 50}}, solid);
      const QImage closing = run("Iris Wipe", QJsonObject{{"completion", 50}, {"closing", true}}, solid);
      check(at(open, 50, 50)[3] == 0.f && at(open, 0, 0)[3] == 1.f && at(closing, 50, 50)[3] == 1.f && at(closing, 0, 0)[3] == 0.f,
            "Iris Wipe opens a hole in the middle, or closing keeps only the middle");
      const QImage gone = run("Iris Wipe", QJsonObject{{"completion", 100}, {"shape", 3}}, solid);
      bool allGone = true;
      for (int y = 0; y < 101; y += 5)
        for (int x = 0; x < 101; x += 5) allGone = allGone && at(gone, x, y)[3] == 0.f;
      check(allGone, "a star Iris Wipe at 100% has taken the whole layer, its hollows included");
      // Venetian Blinds and Block Dissolve: about half at 50%, everything at 100%.
      auto coverage = [&](const QImage &image) {
        double sum = 0;
        for (int y = 0; y < image.height(); ++y)
          for (int x = 0; x < image.width(); ++x) sum += at(image, x, y)[3];
        return sum / (image.width() * image.height());
      };
      const double blinds = coverage(run("Venetian Blinds", QJsonObject{{"completion", 50}, {"width", 20}}, solid));
      const double dissolved = coverage(run("Block Dissolve", QJsonObject{{"completion", 50}, {"blockWidth", 5}, {"blockHeight", 5}}, solid));
      const double dissolvedAll = coverage(run("Block Dissolve", QJsonObject{{"completion", 100}}, solid));
      check(std::abs(blinds - .5) < .08 && std::abs(dissolved - .5) < .12 && dissolvedAll == 0.,
            QString("Venetian Blinds and Block Dissolve take half the layer at 50% (%1, %2) and all of it at 100%")
                .arg(blinds, 0, 'f', 2).arg(dissolved, 0, 'f', 2));
      // Wave Warp: a straight vertical edge comes out wavy; it reaches as far as the wave is high.
      QImage edge(120, 120, QImage::Format_RGBA32FPx4);
      edge.fill(Qt::transparent);
      for (int y = 0; y < 120; ++y)
        for (int x = 0; x < 60; ++x) edge.setPixelColor(x, y, Qt::white);
      const QImage waved = run("Wave Warp", QJsonObject{{"height", 10}, {"width", 40}, {"direction", 0}, {"speed", 0}}, edge);
      int leftmost = 120, rightmost = 0;
      for (int y = 0; y < 120; ++y) {
        int last = -1;
        for (int x = 0; x < 120; ++x) if (at(waved, x, y)[3] > .5f) last = x;
        leftmost = std::min(leftmost, last); rightmost = std::max(rightmost, last);
      }
      Layer warp; warp.appliedEffects["Wave Warp"] = true; warp.surfaceEffects["Wave Warp"] = QJsonObject{{"height", 33}};
      check(rightmost - leftmost >= 16 && rightmost - leftmost <= 22 && layerPackReach(warp) == 40,
            QString("Wave Warp bends a straight edge into a wave as high as asked (%1 px swing) and reaches that far").arg(rightmost - leftmost));
      // Film Grain, Animated: another frame, another grain; not animated, the same.
      const QJsonObject boiling{{"amount", 50}, {"animated", true}, {"rate", 24}};
      const QJsonObject fixed{{"amount", 50}};
      check(run("Film Grain", boiling, grey, 0) != run("Film Grain", boiling, grey, .5) &&
                run("Film Grain", fixed, grey, 0) == run("Film Grain", fixed, grey, .5),
            "Animated Film Grain changes from frame to frame; without it the grain holds still");
      // Ripple, Twirl and Bulge move the picture about inside their reach and
      // leave the corners -- beyond it -- as they were.
      QImage checker(120, 120, QImage::Format_RGBA32FPx4);
      for (int y = 0; y < 120; ++y)
        for (int x = 0; x < 120; ++x) checker.setPixelColor(x, y, ((x / 10 + y / 10) % 2) ? Qt::white : Qt::black);
      const QImage rippled = run("Ripple", QJsonObject{{"amplitude", 6}, {"wavelength", 30}, {"speed", 0}, {"radius", 50}}, checker);
      const QImage twirled = run("Twirl", QJsonObject{{"angle", 120}, {"radius", 50}}, checker);
      const QImage bulged = run("Bulge", QJsonObject{{"strength", 60}, {"radius", 50}}, checker);
      const QImage original = checker.convertToFormat(QImage::Format_RGBA32FPx4);
      auto corner = [&](const QImage &image) { return at(image, 2, 2) == at(original, 2, 2) && at(image, 117, 117) == at(original, 117, 117); };
      check(rippled != original && twirled != original && bulged != original && corner(rippled) && corner(twirled) && corner(bulged),
            "Ripple, Twirl and Bulge move the picture within their radius and leave what lies beyond it alone");
      Layer rings; rings.appliedEffects["Ripple"] = true; rings.surfaceEffects["Ripple"] = QJsonObject{{"amplitude", 20}, {"speed", 2}};
      check(layerPackReach(rings) == 24 && layerPackEffectMoves(rings, 0), "moving Ripple reaches as far as its amplitude and moves by itself");
      // Light Sweep: a band of light where Position puts it, on the layer only.
      QImage half(200, 40, QImage::Format_RGBA32FPx4);
      half.fill(Qt::transparent);
      for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 200; ++x) if (y < 20) half.setPixelColor(x, y, QColor::fromRgbF(.2f, .2f, .2f, 1.f));
      const QImage swept = run("Light Sweep", QJsonObject{{"position", 50}, {"angle", 0}, {"width", 40}, {"intensity", 100}}, half);
      check(at(swept, 100, 5)[0] > .9f && std::abs(at(swept, 10, 5)[0] - .2f) < 1e-6f && at(swept, 100, 30)[3] == 0.f,
            "Light Sweep lights the band where Position puts it, and only where the layer is");
      // Chromatic Aberration: a white square on black grows a red fringe on its
      // outer side and a blue one on its inner side, away from the middle; the
      // middle itself stays white.
      QImage square(200, 200, QImage::Format_RGBA32FPx4);
      square.fill(Qt::black);
      for (int y = 40; y < 160; ++y)
        for (int x = 40; x < 160; ++x) square.setPixelColor(x, y, Qt::white);
      const QImage fringed = run("Chromatic Aberration", QJsonObject{{"amount", 16}}, square);
      const auto outside = at(fringed, 162, 100), inside = at(fringed, 157, 100), centre = at(fringed, 100, 100);
      check(outside[0] > .5f && outside[2] < .1f && inside[2] < .5f && inside[0] > .9f && centre[0] == 1.f && centre[2] == 1.f,
            "Chromatic Aberration puts red outside an edge and takes blue inside it, more away from the middle");
      // Vignette toward white: the corners come out lighter, the middle as it was.
      const QImage whiteEdge = run("Vignette", QJsonObject{{"amount", 80}, {"color", "#ffffff"}}, grey);
      check(at(whiteEdge, 0, 0)[0] > .7f && std::abs(at(whiteEdge, 40, 30)[0] - .5f) < .01f,
            "a white Vignette lightens the corners and leaves the middle alone");
    }
    // The second lot: Threshold, Find Edges, Emboss, Sharpen, Polar Coordinates, Light Rays.
    const QStringList more{"Threshold", "Find Edges", "Emboss", "Sharpen", "Polar Coordinates", "Light Rays"};
    QStringList missing;
    for (const QString &id : more)
      if (!packEffect(id)) missing << id;
    check(missing.isEmpty(), QString("the second lot of new effects is there%1").arg(missing.isEmpty() ? QString() : ": missing " + missing.join(", ")));
    if (missing.isEmpty()) {
      // Threshold: black below Level, white from it on.
      const QImage stark = run("Threshold", QJsonObject{{"level", 128}}, ramp);
      bool split = true;
      for (int x = 0; x < 256; ++x) split = split && at(stark, x, 1)[0] == (x >= 129 ? 1.f : x <= 127 ? 0.f : at(stark, x, 1)[0]);
      check(split && at(stark, 0, 0)[3] == 1.f, "Threshold turns what is darker than Level black and the rest white");
      // Find Edges: a flat white square on black is white inside and out, dark on its edge.
      QImage square(200, 200, QImage::Format_RGBA32FPx4);
      square.fill(Qt::black);
      for (int y = 40; y < 160; ++y)
        for (int x = 40; x < 160; ++x) square.setPixelColor(x, y, Qt::white);
      const QImage traced = run("Find Edges", QJsonObject{}, square), glowing = run("Find Edges", QJsonObject{{"invert", true}}, square);
      check(at(traced, 100, 100)[0] > .99f && at(traced, 10, 10)[0] > .99f && at(traced, 40, 100)[0] < .2f &&
                at(glowing, 100, 100)[0] < .01f && at(glowing, 40, 100)[0] > .8f,
            "Find Edges draws dark lines on white where colour changes, or glowing ones on black inverted");
      // Emboss: flat parts middle grey, an edge facing the light lighter, the other side darker.
      const QImage pressed = run("Emboss", QJsonObject{{"direction", 0}, {"relief", 2}}, square);
      const float flat = at(pressed, 100, 100)[0], left = at(pressed, 39, 100)[0], right = at(pressed, 160, 100)[0];
      check(std::abs(flat - .5f) < 1e-5f && left > .7f && right < .3f, "Emboss leaves flat parts grey and lights edges facing the light");
      // Sharpen: a grey square on darker grey gets a brighter rim inside and a darker one outside; flat parts stay.
      QImage soft(120, 120, QImage::Format_RGBA32FPx4);
      soft.fill(QColor::fromRgbF(.2f, .2f, .2f, 1.f));
      for (int y = 30; y < 90; ++y)
        for (int x = 30; x < 90; ++x) soft.setPixelColor(x, y, QColor::fromRgbF(.5f, .5f, .5f, 1.f));
      const QImage crisp = run("Sharpen", QJsonObject{{"amount", 100}, {"radius", 1.5}}, soft);
      const QImage calm = run("Sharpen", QJsonObject{{"amount", 100}, {"radius", 1.5}, {"threshold", 200}}, soft);
      check(at(crisp, 30, 60)[0] > .55f && at(crisp, 29, 60)[0] < .15f && std::abs(at(crisp, 60, 60)[0] - .5f) < 1e-5f &&
                std::abs(at(calm, 30, 60)[0] - .5f) < 1e-5f && at(crisp, 30, 60)[3] == 1.f,
            "Sharpen brightens the inside of an edge and darkens its outside, leaves flat parts and, past Threshold, the edge alone");
      // Polar Coordinates: the strip's top edge goes to the middle, its bottom round the outside.
      QImage halves(200, 200, QImage::Format_RGBA32FPx4);
      for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x) halves.setPixelColor(x, y, y < 100 ? Qt::red : Qt::blue);
      const QImage ring = run("Polar Coordinates", QJsonObject{}, halves);
      const QImage none = run("Polar Coordinates", QJsonObject{{"amount", 0}}, halves);
      check(at(ring, 100, 101)[0] > .9f && at(ring, 100, 15)[2] > .9f && at(ring, 185, 100)[2] > .9f &&
                none == halves.convertToFormat(QImage::Format_RGBA32FPx4),
            "Polar Coordinates rolls the layer into a ring, its top in the middle; at 0 it leaves it alone");
      // Light Rays: light streams outward from a bright square, not out to where it cannot reach.
      QImage lamp(200, 200, QImage::Format_RGBA32FPx4);
      lamp.fill(Qt::transparent);
      for (int y = 80; y < 120; ++y)
        for (int x = 80; x < 120; ++x) lamp.setPixelColor(x, y, Qt::white);
      const QImage rays = run("Light Rays", QJsonObject{{"centerY", 50}, {"length", 40}}, lamp);
      check(at(rays, 130, 100)[3] > .05f && at(rays, 130, 100)[0] > .05f && at(rays, 195, 100)[3] == 0.f && at(rays, 100, 100)[0] == 1.f,
            QString("Light Rays stream out from what is bright, as far as Length (%1 just outside)").arg(at(rays, 130, 100)[3], 0, 'f', 2));
      Layer beams; beams.appliedEffects["Light Rays"] = true; beams.surfaceEffects["Light Rays"] = QJsonObject{{"length", 40}};
      check(!layerPackEffectMoves(beams, 0), "Light Rays hold still unless keyed");
    }
    // Wavy-Pop: the music-video pack.
    const QStringList pop{"Pattern", "Sunburst", "Sparkles", "Light Leak", "TV Screen"};
    QStringList popMissing;
    for (const QString &id : pop)
      if (!packEffect(id)) popMissing << id;
    check(popMissing.isEmpty(), QString("Wavy-Pop brings its effects%1").arg(popMissing.isEmpty() ? QString() : ": missing " + popMissing.join(", ")));
    if (popMissing.isEmpty()) {
      // Pattern: dots in colour A at each cell's middle, colour B between; clear B leaves the layer clear there.
      QImage plate(200, 200, QImage::Format_RGBA32FPx4);
      plate.fill(Qt::white);
      const QImage dots = run("Pattern", QJsonObject{{"size", 40}, {"colorA", "#ff0000"}, {"colorB", "#0000ff"}}, plate);
      const QImage holes = run("Pattern", QJsonObject{{"size", 40}, {"colorA", "#ff0000"}, {"colorB", "#00000000"}}, plate);
      check(at(dots, 100, 100)[0] > .99f && at(dots, 100, 100)[2] < .01f && at(dots, 80, 80)[2] > .99f &&
                at(holes, 80, 80)[3] == 0.f && at(holes, 100, 100)[3] == 1.f,
            "Pattern lays dots of colour A on colour B, and a clear B leaves the layer clear between them");
      QImage clear(60, 60, QImage::Format_RGBA32FPx4);
      clear.fill(Qt::transparent);
      check(at(run("Pattern", QJsonObject{}, clear), 30, 30)[3] == 0.f, "Pattern fills only where the layer is");
      Layer sliding; sliding.appliedEffects["Pattern"] = true; sliding.surfaceEffects["Pattern"] = QJsonObject{{"scrollX", 40}};
      Layer still; still.appliedEffects["Pattern"] = true;
      check(layerPackEffectMoves(sliding, 0) && !layerPackEffectMoves(still, 0) &&
                run("Pattern", QJsonObject{{"scrollX", 20}}, plate, 0) != run("Pattern", QJsonObject{{"scrollX", 20}}, plate, 1),
            "a scrolling Pattern moves by itself; a still one does not");
      // Sunburst: neighbouring rays alternate A and B round the middle.
      const QImage burst = run("Sunburst", QJsonObject{{"rays", 4}, {"spin", 0}, {"colorA", "#ff0000"}, {"colorB", "#0000ff"}}, plate);
      // With 4 rays each colour spans 45 degrees: A from 0 to 45, B from 45 to 90 (y down).
      check(at(burst, 190, 120)[0] > .99f && at(burst, 120, 190)[2] > .99f, "Sunburst turns rays of A and B round its middle");
      // Sparkles: light over clear parts, the same at the same time, twinkling over time.
      const QImage lit = run("Sparkles", QJsonObject{{"count", 20}, {"size", 30}}, clear.scaled(200, 200), .3);
      double light = 0;
      for (int y = 0; y < 200; ++y) for (int x = 0; x < 200; ++x) light += at(lit, x, y)[3];
      check(light > 20 && lit == run("Sparkles", QJsonObject{{"count", 20}, {"size", 30}}, clear.scaled(200, 200), .3) &&
                lit != run("Sparkles", QJsonObject{{"count", 20}, {"size", 30}}, clear.scaled(200, 200), .55),
            QString("Sparkles light up clear parts (%1), the same each time at one moment, twinkling over time").arg(light, 0, 'f', 0));
      // Light Leak: warm light over a clear layer.
      const QImage leaked = run("Light Leak", QJsonObject{{"intensity", 100}}, clear.scaled(200, 200), .5);
      double red = 0, blue = 0;
      for (int y = 0; y < 200; ++y) for (int x = 0; x < 200; ++x) { red += at(leaked, x, y)[0]; blue += at(leaked, x, y)[2]; }
      check(red > blue && red > 100, "Light Leak lays warm light over the layer");
      // TV Screen: the frame's colour in the corner, the picture in the middle.
      const QImage set = run("TV Screen", QJsonObject{{"frame", "#00ff00"}}, plate);
      check(at(set, 1, 1)[1] > .99f && at(set, 1, 1)[0] < .01f && at(set, 100, 100)[0] > .99f && at(set, 100, 100)[3] == 1.f,
            "TV Screen sets the picture in a rounded screen inside its frame");
    }
  }
  // ---- the 8-bit run of grades --------------------------------------------------------
  {
    QImage source(32, 8, QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(180, 90, 40));
    Layer l;
    l.appliedEffects = QJsonObject{{"HSL", true}, {"Levels", true}, {"Tint", true}};
    l.effectOrder = QJsonArray{"HSL", "Levels", "Tint"};
    l.surfaceEffects["HSL"] = QJsonObject{{"hue", 30.}};
    l.surfaceEffects["Levels"] = QJsonObject{{"gamma", 1.2}};
    QImage fast = source;
    const auto used = gradeLeadingEightBit(fast, l, 0, 0, {0, 0, 32, 8}, 1);
    check(used == QSet<QString>{"HSL", "Levels", "Tint"} && fast != source,
          "per-pixel pack effects in a row run together over the 8-bit pixels");
  }
  // ---- Tile Repeat -----------------------------------------------------------------------
  {
    const PackEffect &tile = *packEffect("Tile Repeat");
    // A layer 40 x 20 in a raster with 30 px of room around it; every pixel of
    // it a different colour, so where each tile pixel came from can be read.
    const int pad = 30, bw = 40, bh = 20;
    QImage raster(bw + 2 * pad, bh + 2 * pad, QImage::Format_RGBA32FPx4);
    raster.fill(Qt::transparent);
    auto source = [&](int x, int y) { return QColor::fromRgbF(float(x) / (bw - 1), float(y) / (bh - 1), float((x * 3 + y * 5) % 7) / 6.f, 1); };
    for (int y = 0; y < bh; ++y) for (int x = 0; x < bw; ++x) raster.setPixelColor(pad + x, pad + y, source(x, y));
    auto run = [&](const QJsonObject &settings) {
      Layer l; l.appliedEffects["Tile Repeat"] = true; l.surfaceEffects["Tile Repeat"] = settings;
      QImage out = raster;
      runPackEffect(out, tile, packEffectValues(l, tile, 0), QRectF(pad, pad, bw, bh), 1, 0, 0);
      return out;
    };
    auto same = [](const QColor &a, const QColor &b, double tolerance = 1e-3) {
      return std::abs(a.redF() - b.redF()) < tolerance && std::abs(a.greenF() - b.greenF()) < tolerance &&
             std::abs(a.blueF() - b.blueF()) < tolerance && std::abs(a.alphaF() - b.alphaF()) < tolerance;
    };
    Layer plain; plain.appliedEffects["Tile Repeat"] = true;
    check(packEffectInert(tile, packEffectValues(plain, tile, 0)), "Tile Repeat at its defaults leaves the layer as it was");
    // Carried past its edges: the next tile along starts again at the layer's left edge.
    const QImage onward = run({{"extend", 30.}});
    bool inside = true, right = true, below = true;
    for (int y = 0; y < bh; ++y) for (int x = 0; x < bw; ++x) inside &= same(onward.pixelColor(pad + x, pad + y), source(x, y));
    for (int y = 0; y < bh; ++y) for (int k = 0; k < 25; ++k) right &= same(onward.pixelColor(pad + bw + k, pad + y), source(k, y));
    for (int x = 0; x < bw; ++x) for (int k = 0; k < 20; ++k) below &= same(onward.pixelColor(pad + x, pad + bh + k), source(x, k));
    check(inside && right && below, "extended, the layer stays itself and the next tiles carry on from its edges");
    // Mirrored: the tile beside is the layer reflected, so the seam matches.
    const QImage mirrored = run({{"extend", 30.}, {"mirror", true}});
    bool reflected = true;
    for (int y = 0; y < bh; ++y) for (int k = 0; k < 25; ++k) reflected &= same(mirrored.pixelColor(pad + bw + k, pad + y), source(bw - 1 - k, y));
    check(reflected, "Mirror Edges reflects each neighbouring tile");
    // Shifted a quarter of a tile (10 px of 40): the pattern moves, wrapping round.
    const QImage slid = run({{"shiftX", 25.}});
    bool wraps = true;
    for (int y = 0; y < bh; ++y) for (int x = 0; x < bw; ++x) wraps &= same(slid.pixelColor(pad + x, pad + y), source(((x - 10) % bw + bw) % bw, y));
    check(wraps, "Shift slides the tiles and wraps them round: keyframed, a pattern scrolls for ever");
    // A whole tile of shift is the same picture: a loop that closes exactly,
    // even when the layer's size is not a whole number of pixels.
    QImage odd(bw + 2 * pad, bh + 2 * pad, QImage::Format_RGBA32FPx4);
    odd.fill(Qt::transparent);
    for (int y = 0; y < bh; ++y) for (int x = 0; x < bw; ++x) odd.setPixelColor(pad + x, pad + y, source(x, y));
    auto shifted = [&](double tiles) {
      Layer l; l.appliedEffects["Tile Repeat"] = true;
      l.surfaceEffects["Tile Repeat"] = QJsonObject{{"extend", 30.}, {"tileWidth", 70.}, {"shiftX", tiles * 100}};
      QImage out = odd;
      runPackEffect(out, tile, packEffectValues(l, tile, 0), QRectF(pad + .3, pad + .4, bw - .7, bh - .5), 1, 0, 0);
      return out;
    };
    const QImage none = shifted(0), one = shifted(1), half = shifted(.5);
    double loop = 0, halfway = 0;
    for (int y = 0; y < none.height(); ++y) for (int x = 0; x < none.width(); ++x) {
      loop = std::max(loop, double(std::abs(none.pixelColor(x, y).redF() - one.pixelColor(x, y).redF())));
      halfway = std::max(halfway, double(std::abs(none.pixelColor(x, y).redF() - half.pixelColor(x, y).redF())));
    }
    check(loop < 1e-4 && halfway > .1, QString("Shift 100% lands exactly back where it started (%1), half way does not (%2)")
                                          .arg(loop, 0, 'g', 2).arg(halfway, 0, 'f', 2));
    // Half-size tiles: the whole layer shrunk into each, laid from the layer's
    // centre -- one whole tile in the middle, halves at the sides.
    const QImage quarter = run({{"tileWidth", 50.}, {"tileHeight", 50.}});
    auto between = [&](double sx, double sy) { // the source, bilinear, held inside it
      sx = std::clamp(sx - .5, 0., bw - 1.); sy = std::clamp(sy - .5, 0., bh - 1.);
      const int x0 = int(sx), y0 = int(sy), x1 = std::min(x0 + 1, bw - 1), y1 = std::min(y0 + 1, bh - 1);
      const double tx = sx - x0, ty = sy - y0;
      auto mix = [&](auto channel) {
        return (channel(source(x0, y0)) * (1 - tx) + channel(source(x1, y0)) * tx) * (1 - ty) +
               (channel(source(x0, y1)) * (1 - tx) + channel(source(x1, y1)) * tx) * ty;
      };
      return QColor::fromRgbF(float(mix([](const QColor &c) { return c.redF(); })), float(mix([](const QColor &c) { return c.greenF(); })),
                              float(mix([](const QColor &c) { return c.blueF(); })), 1);
    };
    bool four = true;
    int whole = 0;
    for (int y = 0; y < bh; ++y) for (int x = 0; x < bw; ++x) {
      const double u = (x + .5 - bw / 2.) / (bw * .5) + .5, v = (y + .5 - bh / 2.) / (bh * .5) + .5;
      four &= same(quarter.pixelColor(pad + x, pad + y), between((u - std::floor(u)) * bw, (v - std::floor(v)) * bh), 2e-3);
      whole += std::floor(u) == 0 && std::floor(v) == 0;
    }
    check(four && whole == bw * bh / 4, "half-size tiles put the whole layer, shrunk, in each tile, laid from the layer's centre");
    // Stagger half a tile along rows: the second row of tiles starts half way.
    const QImage bricks = run({{"extend", 30.}, {"stagger", 50.}});
    bool staggered = true;
    for (int x = 0; x < bw; ++x) staggered &= same(bricks.pixelColor(pad + x, pad + bh + 5), source((x + bw / 2) % bw, 5));
    check(staggered, "Stagger moves every other row of tiles along like bricks");
    // Gap: 50% of a 40 px tile is 20 px of nothing between it and the next.
    const QImage spaced = run({{"extend", 30.}, {"gapX", 50.}});
    bool keeps = true, empty = true, next = true;
    for (int y = 0; y < bh; ++y) {
      for (int x = 0; x < bw; ++x) keeps &= same(spaced.pixelColor(pad + x, pad + y), source(x, y));
      for (int k = 0; k < 20; ++k) empty &= spaced.pixelColor(pad + bw + k, pad + y).alphaF() == 0;
      for (int k = 0; k < 10; ++k) next &= same(spaced.pixelColor(pad + bw + 20 + k, pad + y), source(k, y));
    }
    check(keeps && empty && next, "Gap X leaves that much empty room between tiles, and the next tile starts after it");
    const QImage stacked = run({{"extend", 30.}, {"gapY", 100.}});
    bool emptyRows = true, nextRow = true;
    for (int x = 0; x < bw; ++x) {
      for (int k = 0; k < 20; ++k) emptyRows &= stacked.pixelColor(pad + x, pad + bh + k).alphaF() == 0;
      for (int k = 0; k < 10; ++k) nextRow &= same(stacked.pixelColor(pad + x, pad + 2 * bh + k), source(x, k));
    }
    check(emptyRows && nextRow, "Gap Y does the same between rows");
    // Between pixels, a tile's edge is part covered rather than cut; where tiles
    // meet edge to edge (no gap that way) there is no seam at all.
    Layer spacedLayer; spacedLayer.appliedEffects["Tile Repeat"] = true;
    spacedLayer.surfaceEffects["Tile Repeat"] = QJsonObject{{"extend", 30.}, {"tileWidth", 70.}, {"gapX", 30.}};
    QImage soft = odd;
    runPackEffect(soft, tile, packEffectValues(spacedLayer, tile, 0), QRectF(pad + .3, pad + .4, bw - .7, bh - .5), 1, 0, 0);
    int partial = 0;
    bool seamless = true;
    const double tileW = (bw - .7) * .7, stepW = tileW * 1.3, centre = pad + .3 + (bw - .7) / 2.;
    for (int x = 0; x < soft.width(); ++x) {
      const double at = std::fmod(std::abs(x + .5 - centre) + tileW / 2., stepW); // from a tile's near edge
      const double alpha = soft.pixelColor(x, pad + bh / 2).alphaF();
      partial += alpha > .05 && alpha < .95;
      if (at > 1 && at < tileW - 1)
        for (int y = pad - 29; y < pad + bh + 29; ++y) seamless &= soft.pixelColor(x, y).alphaF() > .999;
    }
    check(partial >= 2 && seamless, QString("a gap's edges are smooth (%1 part-covered pixels), and tiles still meet without a seam the other way").arg(partial));
    auto spacedShift = [&](double steps) {
      Layer l; l.appliedEffects["Tile Repeat"] = true;
      l.surfaceEffects["Tile Repeat"] = QJsonObject{{"extend", 30.}, {"tileWidth", 70.}, {"gapX", 30.}, {"gapY", 20.}, {"shiftX", steps * 100}};
      QImage out = odd;
      runPackEffect(out, tile, packEffectValues(l, tile, 0), QRectF(pad + .3, pad + .4, bw - .7, bh - .5), 1, 0, 0);
      return out;
    };
    const QImage gap0 = spacedShift(0), gap1 = spacedShift(1);
    double gapLoop = 0;
    for (int y = 0; y < gap0.height(); ++y) for (int x = 0; x < gap0.width(); ++x) {
      gapLoop = std::max(gapLoop, double(std::abs(gap0.pixelColor(x, y).alphaF() - gap1.pixelColor(x, y).alphaF())));
      gapLoop = std::max(gapLoop, double(std::abs(gap0.pixelColor(x, y).redF() - gap1.pixelColor(x, y).redF())));
    }
    check(gapLoop < 1e-4, QString("with a gap, Shift 100% is one tile and its gap: the loop still closes (%1)").arg(gapLoop, 0, 'g', 2));
    // Alternate: 25% of a 40 px step is 10 px -- row A (the layer's) goes left,
    // row B (below it) goes right, and so on.
    const QImage alternating = run({{"extend", 30.}, {"alternate", 25.}});
    bool rowA = true, rowB = true;
    for (int x = 0; x < bw; ++x) {
      for (int y = 0; y < bh; ++y) rowA &= same(alternating.pixelColor(pad + x, pad + y), source((x + 10) % bw, y));
      for (int k = 0; k < 10; ++k) rowB &= same(alternating.pixelColor(pad + x, pad + bh + k), source((x - 10 + bw) % bw, k));
    }
    check(rowA && rowB, "Alternate sends row A left and row B right");
    // Up / Down: 25% of a 20 px step is 5 px -- column A goes up, column B down.
    const QImage upDown = run({{"extend", 30.}, {"alternate", 25.}, {"alternateUpDown", true}});
    bool columnA = true, columnB = true;
    for (int y = 0; y < bh; ++y) {
      for (int x = 0; x < bw; ++x) columnA &= same(upDown.pixelColor(pad + x, pad + y), source(x, (y + 5) % bh));
      for (int k = 0; k < 25; ++k) columnB &= same(upDown.pixelColor(pad + bw + k, pad + y), source(k, (y - 5 + bh) % bh));
    }
    check(columnA && columnB, "with Columns Up / Down, column A goes up and column B down");
    auto alternated = [&](double steps, bool columns) {
      Layer l; l.appliedEffects["Tile Repeat"] = true;
      l.surfaceEffects["Tile Repeat"] = QJsonObject{{"extend", 30.}, {"tileWidth", 70.}, {"gapX", 30.}, {"gapY", 20.},
                                                    {"alternate", steps * 100}, {"alternateUpDown", columns}};
      QImage out = odd;
      runPackEffect(out, tile, packEffectValues(l, tile, 0), QRectF(pad + .3, pad + .4, bw - .7, bh - .5), 1, 0, 0);
      return out;
    };
    double alternateLoop = 0, alternateHalf = 0;
    for (bool columns : {false, true}) {
      const QImage a0 = alternated(0, columns), a1 = alternated(1, columns), aHalf = alternated(.5, columns);
      for (int y = 0; y < a0.height(); ++y) for (int x = 0; x < a0.width(); ++x) {
        alternateLoop = std::max(alternateLoop, double(std::abs(a0.pixelColor(x, y).redF() - a1.pixelColor(x, y).redF())));
        alternateLoop = std::max(alternateLoop, double(std::abs(a0.pixelColor(x, y).alphaF() - a1.pixelColor(x, y).alphaF())));
        alternateHalf = std::max(alternateHalf, double(std::abs(a0.pixelColor(x, y).alphaF() - aHalf.pixelColor(x, y).alphaF())));
      }
    }
    check(alternateLoop < 1e-4 && alternateHalf > .5,
          QString("Alternate 100% lands back where it started, both ways (%1), half way does not (%2)")
              .arg(alternateLoop, 0, 'g', 2).arg(alternateHalf, 0, 'f', 2));
    // SDK 7: the layout -- twirls, help, dimming -- and none of it in the values.
    {
      check(tile.valueCount == 12 && packEffectValues(Layer{}, tile, 0).size() == 12,
            "Tile Repeat's twirls take no values: the twelve it always had, in the same order");
      int groups = 0, ends = 0;
      for (int i = 0; i < tile.api->parameterCount; ++i) {
        groups += tile.api->parameters[i].kind == WFX_GROUP;
        ends += tile.api->parameters[i].kind == WFX_GROUP_END;
      }
      check(groups == 5 && ends == 5 && tile.help && tile.help[3] && QByteArray(tile.help[3]).contains("side by side"),
            "Tile Repeat folds into Gap, Shift, Edges, Stagger and Alternate, with a line of help for each control");
      QVector<float> values = packEffectValues(Layer{}, tile, 0);
      const bool dimmedWithout = tile.controlState && tile.controlState(values.constData(), int32_t(values.size()), 16) == WFX_CONTROL_DIMMED;
      values[8] = 50; // stagger
      const bool usableWith = tile.controlState && tile.controlState(values.constData(), int32_t(values.size()), 16) == 0;
      check(dimmedWithout && usableWith, "Stagger Along is dimmed while there is no stagger, and usable once there is");
      // What a description may and may not do.
      std::vector<WfxParameter> list(tile.api->parameters, tile.api->parameters + tile.api->parameterCount);
      WfxEffect shape = *tile.api;
      auto problem = [&](std::vector<WfxParameter> parameters, uint32_t abi, uint32_t size) {
        shape.parameters = parameters.data();
        shape.parameterCount = int32_t(parameters.size());
        shape.abiVersion = abi;
        shape.structSize = size;
        return packEffectProblem(&shape);
      };
      check(problem(list, 9, sizeof(WfxEffect)).isEmpty(), "a pack made for SDK 9 loads");
      check(problem(list, 8, sizeof(WfxEffect)).isEmpty(), "a laid-out description is accepted");
      check(problem(list, 7, uint32_t(offsetof(WfxEffect, shaderPasses))).isEmpty(), "a pack made for SDK 7 still loads");
      check(!problem(list, 7, sizeof(WfxEffect)).isEmpty(), "a pack that says 7 with 8's description is refused");
      std::vector<WfxParameter> abi9Params = list;
      abi9Params.push_back(WfxParameter{sizeof(WfxParameter), WFX_LAYER, "testLayer", "Layer", -1.f, 10000.f, -1.f, 1.f, nullptr, nullptr, nullptr, 0u, {0,0,0,0}});
      abi9Params.push_back(WfxParameter{sizeof(WfxParameter), WFX_MASK, "testMask", "Mask", -2.f, 10000.f, -1.f, 1.f, nullptr, nullptr, nullptr, WFX_PARAM_ALL_MASKS, {0,0,0,0}});
      abi9Params.push_back(WfxParameter{sizeof(WfxParameter), WFX_POINT, "testPoint", "Point", -10000.f, 10000.f, 0.f, 1.f, nullptr, " px", nullptr, 0u, {100.f, 200.f, 0, 0}});
      abi9Params.push_back(WfxParameter{sizeof(WfxParameter), WFX_POINT_3D, "testPoint3D", "Point 3D", -10000.f, 10000.f, 0.f, 1.f, nullptr, " px", nullptr, 0u, {10.f, 20.f, 30.f, 0}});
      check(problem(abi9Params, 9, sizeof(WfxEffect)).isEmpty(), "ABI 9 parameters (LAYER, MASK, POINT, POINT_3D) are accepted");
      check(wfxParameterFloats(&abi9Params[abi9Params.size() - 4]) == 1, "WFX_LAYER takes 1 float");
      check(wfxParameterFloats(&abi9Params[abi9Params.size() - 3]) == 1, "WFX_MASK takes 1 float");
      check(wfxParameterFloats(&abi9Params[abi9Params.size() - 2]) == 2, "WFX_POINT takes 2 floats");
      check(wfxParameterFloats(&abi9Params[abi9Params.size() - 1]) == 3, "WFX_POINT_3D takes 3 floats");
      auto open = list;
      open.pop_back(); // the last group's end
      check(problem(open, 8, sizeof(WfxEffect)).contains("left open"), "a group never closed is refused");
      auto stray = list;
      stray.erase(stray.begin() + 2); // the first group's start
      check(problem(stray, 8, sizeof(WfxEffect)).contains("never opened"), "a group closed that was never opened is refused");
      std::vector<WfxParameter> plain;
      for (const auto &p : list)
        if (p.kind != WFX_GROUP && p.kind != WFX_GROUP_END) plain.push_back(p);
      check(problem(plain, 6, uint32_t(offsetof(WfxEffect, help))).isEmpty(), "a pack made for SDK 6 still loads");
      check(!problem(list, 6, uint32_t(offsetof(WfxEffect, help))).isEmpty(), "groups in a pack that says it was made for 6 are refused");
    }
    Layer wide; wide.appliedEffects["Tile Repeat"] = true; wide.surfaceEffects["Tile Repeat"] = QJsonObject{{"extend", 250.}};
    check(layerPackReach(wide) == 256 && layerPackNeedsWholeLayer(wide) && !layerPackNeedsWholeLayer(Layer{}),
          QString("Extend makes that much room around the layer (%1) and the whole layer is always drawn").arg(layerPackReach(wide)));
  }
  {
    // End to end: a big picture tiled at half size across a whole frame. Its
    // raster is too big to be built whole on its own terms, and the frame is
    // drawn in bands; each band still needs pixels from all over the picture.
    const int w = 2400, h = 1400;
    QImage picture(w, h, QImage::Format_ARGB32_Premultiplied);
    const QColor quadrant[4] = {QColor("#e04040"), QColor("#40c040"), QColor("#4060e0"), QColor("#e0c040")};
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) picture.setPixelColor(x, y, quadrant[(x >= w / 2 ? 1 : 0) + (y >= h / 2 ? 2 : 0)]);
    Scene scene; scene.width = 1920; scene.height = 1080; scene.frames = 1; scene.frame = 0;
    Layer l; l.type = "image"; l.image = picture; l.duration = 1; l.p[0].base = 960; l.p[1].base = 540;
    l.appliedEffects["Tile Repeat"] = true;
    l.surfaceEffects["Tile Repeat"] = QJsonObject{{"tileWidth", 50.}, {"tileHeight", 50.}, {"extend", 400.}};
    scene.layers = {l};
    const QImage frame = renderCPU(scene, 0, false).convertToFormat(QImage::Format_ARGB32);
    int right = 0, checked = 0;
    QStringList wrong;
    for (const QPoint at : {QPoint(40, 40), QPoint(1880, 40), QPoint(40, 1040), QPoint(1880, 1040), QPoint(960, 540), QPoint(500, 800), QPoint(1400, 300)}) {
      // Which quadrant of the picture this frame pixel shows, worked out the
      // way the tiles are laid.
      const double bx = at.x() + .5 - 960 + w / 2., by = at.y() + .5 - 540 + h / 2.;
      const double u = (bx - w / 2.) / (w * .5) + .5, v = (by - h / 2.) / (h * .5) + .5;
      const double fu = u - std::floor(u), fv = v - std::floor(v);
      const double sx = fu * w, sy = fv * h;
      if (std::abs(sx - w / 2.) < 8 || std::abs(sy - h / 2.) < 8 || sx < 8 || sy < 8 || sx > w - 8 || sy > h - 8) continue;
      ++checked;
      const QColor want = quadrant[(sx >= w / 2 ? 1 : 0) + (sy >= h / 2 ? 2 : 0)];
      const QColor got = frame.pixelColor(at);
      if (std::abs(got.red() - want.red()) <= 2 && std::abs(got.green() - want.green()) <= 2 && std::abs(got.blue() - want.blue()) <= 2) ++right;
      else wrong << QString("(%1,%2) %3 not %4").arg(at.x()).arg(at.y()).arg(got.name(QColor::HexArgb)).arg(want.name());
    }
    check(checked >= 5 && right == checked,
          QString("a large layer tiled across the whole frame is right in every corner, drawn in bands (%1 of %2)%3")
              .arg(right).arg(checked).arg(wrong.isEmpty() ? QString() : ": " + wrong.join(", ")));
  }
  {
    // A small layer tiled far past its edges, with a glow as well. The raster is
    // then too big to keep whole, which is where a layer with one halo used to
    // go into the frame as its vectors -- and the tiling was never run.
    QImage square(100, 100, QImage::Format_ARGB32_Premultiplied);
    square.fill(QColor("#40c0e0"));
    Scene scene; scene.width = 1920; scene.height = 1080; scene.frames = 1; scene.frame = 0;
    Layer l; l.type = "image"; l.image = square; l.duration = 1; l.p[0].base = 960; l.p[1].base = 540;
    l.appliedEffects["Tile Repeat"] = true;
    l.surfaceEffects["Tile Repeat"] = QJsonObject{{"extend", 1100.}};
    for (const char *halo : {"glow", "shadow"}) {
      Layer lit = l;
      if (QByteArray(halo) == "glow") lit.glow = 6; else lit.shadow = 6;
      lit.appliedEffects[QByteArray(halo) == "glow" ? "Glow" : "Drop Shadow"] = true;
      scene.layers = {lit};
      const QImage frame = renderCPU(scene, 0, false).convertToFormat(QImage::Format_ARGB32);
      const QColor corner = frame.pixelColor(60, 60), far = frame.pixelColor(1850, 1000);
      check(corner.alpha() > 240 && far.alpha() > 240,
            QString("tiles fill the frame with a %1 on the layer too (corner alpha %2, far corner %3)")
                .arg(halo).arg(corner.alpha()).arg(far.alpha()));
    }
  }
  // ---- Tile Repeat on a box with no width or height --------------------------------------
  {
    // A box 0 wide that starts part way into a pixel still covers that pixel,
    // and tiles 0 wide put every pixel's place along the pattern at infinity.
    // Until Oct 20 2026 the NaN that came of it was used as an index, and the
    // program crashed -- on a layer whose Main Transform Scale X is 0, say.
    // Such a box (0 wide or high, -0, or so thin that a place runs past every
    // number) now leaves the layer as it was, as a box covering no pixel
    // always did. Every other box tiles to the bit as before: each hash is of
    // the picture the pack gave before the change, for boxes of whole and part
    // pixels with every control, boxes turned inside out by less than a pixel,
    // tiles a ten-millionth of a pixel wide, and a box from far off.
    {
      // First through the renderer: a layer squashed flat by its Main
      // Transform, placed part way into a pixel so its box covers one, full
      // size and in a half-size preview. The pack before crashed here; with
      // the tiling on, the layer now draws what it draws without: nothing.
      CpuPixelScope only;
      QImage art(40, 20, QImage::Format_ARGB32_Premultiplied);
      art.fill(QColor("#40c0e0"));
      Scene scene; scene.width = 200; scene.height = 120; scene.frames = 1; scene.frame = 0;
      Layer flat; flat.type = "image"; flat.image = art; flat.duration = 1; flat.p[0].base = 100.3; flat.p[1].base = 60.6;
      QStringList drawn;
      for (const bool high : {false, true})
        for (const QSize size : {QSize(200, 120), QSize(100, 60)}) {
          Layer l = flat;
          (high ? l.mainScaleY : l.mainScaleX) = 0;
          scene.layers = {l};
          const QImage without = renderCPU(scene, 0, false, size);
          l.appliedEffects["Tile Repeat"] = true;
          l.surfaceEffects["Tile Repeat"] = QJsonObject{{"extend", 30.}, {"mirror", true}, {"gapX", 20.}};
          scene.layers = {l};
          const QImage with = renderCPU(scene, 0, false, size);
          if (with.isNull() || with != without) drawn << QString("Scale %1 0 at %2 px").arg(high ? "Y" : "X").arg(size.width());
        }
      check(drawn.isEmpty(), "a layer squashed to nothing by its Main Transform, tiled, draws nothing and does not crash" +
                                 (drawn.isEmpty() ? QString() : " (not " + drawn.join(", ") + ")"));
    }
    struct Case {
      int width, height;
      float values[12];
      double scale, boxX, boxY, boxW, boxH;
      bool odd;
      uint64_t hash;
    };
    const Case tiled[] = {
        {37, 23, {70, 100, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0}, 1, 5, 4, 20, 12, false, 0xbfd06d8f69a197ddull},
        {41, 29, {64, 81, 25, 40, 37, -12, 10, 1, 50, 0, 0, 0}, 1, 3.3, 2.6, 17.45, 9.8, false, 0xcdf78d7774563e0aull},
        {33, 31, {55, 120, 0, 15, 0, 0, 30, 0, 30, 1, 33, 1}, .5, 6.7, 1.2, 11.1, 14.9, false, 0x73132f0009ac31e7ull},
        {29, 17, {100, 100, 0, 0, 125, 250, 20, 1, 0, 0, -75, 0}, 1.5, 4.25, 3.5, 9.5, 6.75, true, 0xe388384ceb0c379aull},
        {35, 19, {80, 90, 10, 0, 0, 0, 12, 0, 0, 0, 0, 0}, 1, 7.6, 3.3, -.25, 9.1, false, 0x30bc92ac028f1e43ull},
        {27, 25, {100, 60, 0, 30, 0, 40, 16, 1, 0, 0, 0, 0}, 1, 2.2, 15.8, 20.5, -.3, false, 0x7dbc69873d0005c3ull},
        {23, 13, {100, 100, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0}, 1, 7.31, 2.2, 1e-7, 10.4, false, 0xe07ec3cd0d8fec77ull},
        {31, 21, {50, 50, 0, 0, 10, 0, 0, 1, 0, 0, 0, 0}, 1, -1e9 + .3, 3, 1e9 + 20.4, 11.2, true, 0x9982002138bf86f9ull}};
    // The box this was found with (a 73 x 41 picture from a host before
    // strips), and its relatives: 0 high, 0 by 0, -0, a denormal (the step
    // comes out 0) and 2.2e-310 (it does not, but a place overflows). The
    // pack before crashed on each.
    const Case empty[] = {
        {73, 41, {371.8f, 268.5f, 2.4f, 2.2f, 3.9f, 2.5f, 16.2f, 1, 0, 0, 0, 0}, 1, 5.32, -8.40, 0, 14.37, false, 0},
        {40, 30, {50, 50, 20, 0, 0, 0, 10, 1, 0, 0, 0, 0}, 1, 4.5, 6.25, 30.2, 0, true, 0},
        {40, 30, {100, 100, 0, 0, 0, 0, 10, 0, 50, 0, 0, 0}, .5, 9.6, 7.3, 0, 0, false, 0},
        {40, 30, {80, 80, 0, 25, 0, 0, 0, 0, 0, 0, 20, 1}, 1, 3.7, 1.9, -0.0, 12, false, 0},
        {40, 30, {100, 100, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0}, 1, 6.45, 2.5, 4.9406564584124654e-324, 20, false, 0},
        {40, 30, {100, 100, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0}, 1, 6.45, 2.5, 2.2e-310, 20, false, 0}};
    // A case's picture: premultiplied colours and coverages, some of them odd.
    auto picture = [](std::vector<float> &pixels, uint32_t seed, bool odd) {
      static const uint32_t oddBits[] = {0x7fc00000u, 0xffc00000u, 0x7f800000u, 0xff800000u, 0x80000000u,
                                         0x00000001u, 0x3f800001u, 0xbf000000u, 0x40400000u};
      uint32_t s = seed * 2654435761u + 7u;
      auto next = [&] { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
      for (size_t i = 0; i < pixels.size(); i += 4) {
        const uint32_t kind = next() % 16;
        const float a = kind == 0 ? 0.f : kind < 9 ? 1.f : float(next() >> 8) / 16777216.f;
        for (int c = 0; c < 3; ++c) pixels[i + c] = float(next() >> 8) / 16777216.f * a;
        pixels[i + 3] = a;
        if (odd) {
          for (int c = 0; c < 3; ++c)
            if (next() % 6 == 0) std::memcpy(&pixels[i + c], &oddBits[next() % 9], 4);
          if (next() % 13 == 0) std::memcpy(&pixels[i + 3], &oddBits[next() % 9], 4);
        }
      }
    };
    auto hashOf = [](const std::vector<float> &pixels) {
      uint64_t h = 1469598103934665603ull;
      const unsigned char *b = reinterpret_cast<const unsigned char *>(pixels.data());
      for (size_t i = 0; i < pixels.size() * 4; ++i) { h ^= b[i]; h *= 1099511628211ull; }
      return h;
    };
    const PackEffect *tile = packEffect("Tile Repeat");
    auto run = [&](const Case &c, uint32_t seed, uint32_t structSize, std::vector<float> &pixels) {
      pixels.assign(size_t(c.width) * size_t(c.height) * 4, 0.f);
      picture(pixels, seed, c.odd);
      WfxContext context{};
      context.structSize = structSize;
      context.depth = 32;
      context.scale = c.scale;
      context.boxX = c.boxX;
      context.boxY = c.boxY;
      context.boxWidth = c.boxW;
      context.boxHeight = c.boxH;
      return tile && tile->api->process
                 ? tile->api->process(pixels.data(), c.width, c.height, c.width * 16, c.values, 12, &context)
                 : -1;
    };
    QStringList wrong;
    std::vector<float> pixels, before;
    for (size_t k = 0; k < sizeof(tiled) / sizeof(tiled[0]); ++k)
      if (run(tiled[k], uint32_t(k), sizeof(WfxContext), pixels) != 0 || hashOf(pixels) != tiled[k].hash)
        wrong << QString("tiled #%1").arg(k);
    for (size_t k = 0; k < sizeof(empty) / sizeof(empty[0]); ++k) {
      before.assign(size_t(empty[k].width) * size_t(empty[k].height) * 4, 0.f);
      picture(before, uint32_t(100 + k), empty[k].odd);
      // Bytes, not floats: a NaN is never equal to itself.
      if (run(empty[k], uint32_t(100 + k), uint32_t(offsetof(WfxContext, originX)), pixels) != 0 ||
          std::memcmp(pixels.data(), before.data(), pixels.size() * 4) != 0)
        wrong << QString("empty #%1").arg(k);
    }
    check(wrong.isEmpty(), "Tile Repeat leaves a layer with no width or height as it was, and tiles every other box "
                           "to the bit as before" + (wrong.isEmpty() ? QString() : " (not " + wrong.join(", ") + ")"));
  }
  // ---- Wavy-Motion: music video and motion graphics --------------------------------------
  {
    QStringList absent;
    for (const char *id : {"Glitch", "Shake", "Mirror", "Kaleidoscope", "Pixelate", "Directional Blur", "Zoom Blur"}) {
      const PackEffect *e = packEffect(id);
      if (!e || QFileInfo(e->file).completeBaseName() != "Wavy-Motion") absent << id;
    }
    check(absent.isEmpty(), "Wavy-Motion brings Glitch, Shake, Mirror, Kaleidoscope, Pixelate, Directional and Zoom Blur" +
                                (absent.isEmpty() ? QString() : " (missing: " + absent.join(", ") + ")"));
    if (absent.isEmpty()) {
      QImage art(640, 360, QImage::Format_ARGB32_Premultiplied);
      art.fill(QColor(20, 24, 40));
      {
        QPainter p(&art);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 8; ++i) {
          p.setBrush(QColor::fromHsv(i * 45, 200, 240));
          p.drawEllipse(QPointF(50 + i * 75, 120 + 70 * std::sin(i)), 34, 34);
        }
        p.setBrush(Qt::white);
        p.drawRect(200, 250, 240, 60);
      }
      Scene scene; scene.width = 640; scene.height = 360; scene.frames = 60; scene.frame = 0;
      Layer base; base.type = "image"; base.image = art; base.duration = 60; base.p[0].base = 320; base.p[1].base = 180;
      scene.layers = {base};
      auto render = [](const Scene &s, double frame, QRect part = {}) {
        return renderCPU(s, frame, false, {}, part).convertToFormat(QImage::Format_ARGB32);
      };
      auto worst = [](const QImage &a, const QImage &b) {
        int most = 0;
        for (int y = 0; y < a.height(); ++y)
          for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            most = std::max({most, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                             std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
          }
        return most;
      };
      const QImage plain = render(scene, 10);
      const std::vector<std::pair<QString, QJsonObject>> settings{
          {"Glitch", {{"amount", 40.}, {"density", 60.}, {"split", 6.}}},
          {"Shake", {{"amount", 20.}, {"rotation", 3.}, {"zoom", 110.}}},
          {"Mirror", {{"mode", 4}}},
          {"Kaleidoscope", {{"segments", 6}, {"angle", 15.}}},
          {"Pixelate", {{"size", 20.}}},
          {"Directional Blur", {{"angle", 30.}, {"length", 40.}}},
          {"Zoom Blur", {{"amount", 25.}}}};
      for (const auto &[id, s] : settings) {
        Scene with = scene;
        with.layers[0].appliedEffects[id] = true;
        with.layers[0].surfaceEffects[id] = s;
        const QImage whole = render(with, 10);
        QImage parts(whole.size(), QImage::Format_ARGB32);
        parts.fill(Qt::transparent);
        {
          QPainter painter(&parts);
          painter.setCompositionMode(QPainter::CompositionMode_Source);
          for (int i = 0; i < 4; ++i) {
            const QRect part((i % 2) * 320, (i / 2) * 180, 320, 180);
            painter.drawImage(part.topLeft(), render(with, 10, part));
          }
        }
        check(worst(whole, plain) > 40 && worst(whole, parts) <= 1,
              QString("%1 changes the picture, and drawn in quarters is the same as drawn whole (%2)")
                  .arg(id).arg(worst(whole, parts)));
      }
      Scene still = scene, moving = scene;
      still.layers[0].appliedEffects["Glitch"] = true;
      still.layers[0].surfaceEffects["Glitch"] = QJsonObject{{"amount", 40.}, {"density", 60.}, {"speed", 0.}};
      moving.layers[0].appliedEffects["Glitch"] = true;
      moving.layers[0].surfaceEffects["Glitch"] = QJsonObject{{"amount", 40.}, {"density", 60.}, {"speed", 12.}};
      check(worst(render(still, 3), render(still, 40)) == 0 && worst(render(moving, 3), render(moving, 40)) > 40,
            "Glitch at speed 0 holds one tear; with a speed it deals new ones");
      Scene shaken = scene;
      shaken.layers[0].appliedEffects["Shake"] = true;
      shaken.layers[0].surfaceEffects["Shake"] = QJsonObject{{"amount", 20.}, {"mirror", true}};
      const QImage early = render(shaken, 3), late = render(shaken, 40);
      check(worst(early, late) > 40 && early.pixelColor(2, 2).alpha() == 255 && late.pixelColor(637, 357).alpha() == 255,
            "Shake moves with time by itself, and Mirror Edges leaves no empty corner");
      Scene blocks = scene;
      blocks.layers[0].appliedEffects["Pixelate"] = true;
      blocks.layers[0].surfaceEffects["Pixelate"] = QJsonObject{{"size", 20.}};
      const QImage pixelated = render(blocks, 0);
      // Blocks are centred on the layer: 320 is the middle of one, 310 its edge.
      bool flat = true;
      for (int y = 171; y < 189; ++y)
        for (int x = 311; x < 329; ++x) flat = flat && pixelated.pixel(x, y) == pixelated.pixel(311, 171);
      check(flat, "Pixelate fills each block with one colour, the blocks centred on the layer");
    }
  }
  // ---- the card draws what the CPU draws -----------------------------------------------
  {
    QImage patch(48, 32, QImage::Format_RGBA32FPx4);
    for (int y = 0; y < patch.height(); ++y)
      for (int x = 0; x < patch.width(); ++x)
        patch.setPixelColor(x, y, QColor::fromRgbF(x / 47.f, y / 31.f, float((x * 7 + y * 3) % 17) / 16.f, y < 24 ? 1.f : .5f));
    const std::vector<std::pair<QString, QJsonObject>> settings = colourEffectCases();
    bool card = true;
    for (const auto &[id, s] : settings) {
      const PackEffect &e = *packEffect(id);
      Layer l; l.surfaceEffects[id] = s;
      const auto values = packEffectValues(l, e, 0);
      QImage cpu = patch;
      runPackEffect(cpu, e, values, {0, 0, 48, 32}, 1, 0, 0);
      GpuPass pass;
      if (!packEffectPass(e, values, 32, pass)) { check(false, id + " has a shader for the card"); continue; }
      const QImage gpu = gpuRunPasses(patch, {pass});
      if (gpu.isNull()) { card = false; break; }
      const QImage a = cpu.convertToFormat(QImage::Format_RGBA32FPx4), b = gpu.convertToFormat(QImage::Format_RGBA32FPx4);
      double most = 0;
      for (int y = 0; y < a.height(); ++y) {
        const float *pa = reinterpret_cast<const float *>(a.constScanLine(y)), *pb = reinterpret_cast<const float *>(b.constScanLine(y));
        for (int x = 0; x < a.width() * 4; ++x) most = std::max(most, double(std::abs(pa[x] - pb[x])));
      }
      check(most < 2e-3, QString("%1 on the card matches the CPU (largest difference %2)").arg(id).arg(most, 0, 'g', 3));
    }
    if (!card) report << "SKIP: no graphics card here to compare the shaders on";
  }
  // ---- the card draws what the CPU draws, where and when it is ------------------------
  // Effects that read the layer's box, the raster's scale, the time, or other
  // pixels than their own: run on the card with the same context as the CPU's.
  {
    QImage patch(96, 64, QImage::Format_RGBA32FPx4);
    for (int y = 0; y < patch.height(); ++y)
      for (int x = 0; x < patch.width(); ++x) {
        const bool inside = x > 6 && x < 88 && y > 5 && y < 58;
        patch.setPixelColor(x, y, QColor::fromRgbF(x / 95.f, y / 63.f, float((x * 7 + y * 3) % 17) / 16.f,
                                                   !inside ? 0.f : y < 40 ? 1.f : .6f));
      }
    const std::vector<SceneEffectCase> cases = sceneEffectCases();
    bool card = true;
    for (const auto &c : cases) {
      const PackEffect *e = packEffect(c.id);
      if (!e) { check(false, c.id + " is loaded"); continue; }
      Layer l; l.surfaceEffects[c.id] = c.settings;
      const auto values = packEffectValues(l, *e, c.frame);
      QImage cpu = patch;
      runPackEffect(cpu, *e, values, c.box, c.scale, c.seconds, c.frame);
      const auto passes = packEffectPasses(*e, values, 32, patch.size(), c.box, c.scale, c.seconds, c.frame);
      if (passes.isEmpty()) { check(false, c.id + " has a shader for the card"); continue; }
      const QImage gpu = gpuRunPasses(patch, passes);
      if (gpu.isNull()) { card = false; break; }
      // Compared as they will be seen, premultiplied: in straight colour a
      // pixel at the very edge, all but clear on one side and clear on the
      // other (alpha 0.0009 against 0), differed by 0.9 in colour no one sees.
      const QImage a = cpu.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied),
                   b = gpu.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
      double most = 0, total = 0;
      for (int y = 0; y < a.height(); ++y) {
        const float *pa = reinterpret_cast<const float *>(a.constScanLine(y)), *pb = reinterpret_cast<const float *>(b.constScanLine(y));
        for (int x = 0; x < a.width() * 4; ++x) {
          const double d = std::abs(double(pa[x]) - double(pb[x]));
          most = std::max(most, d); total += d;
        }
      }
      const double mean = total / (double(a.width()) * a.height() * 4);
      check(most < 2e-3 || (mean < 2e-4 && most < .05),
            QString("%1 on the card matches the CPU (%2; largest difference %3, mean %4)")
                .arg(c.id, c.settings.isEmpty() ? QString("defaults") : QString::fromUtf8(QJsonDocument(c.settings).toJson(QJsonDocument::Compact)))
                .arg(most, 0, 'g', 3).arg(mean, 0, 'g', 3));
    }
    if (!card) report << "SKIP: no graphics card here to compare the shaders on";
    // Values a shader has no form for stay with process(): a long smear is the
    // running sum on the shear, and a crowd of streaks wants the CPU's search.
    auto leavesCard = [&](const QString &id, const QJsonObject &settings) {
      const PackEffect *e = packEffect(id);
      if (!e) return false;
      Layer l; l.surfaceEffects[id] = settings;
      return packEffectPasses(*e, packEffectValues(l, *e, 0), 32, patch.size(), QRectF(7, 6, 81, 52), 1, 0, 0).isEmpty();
    };
    check(leavesCard("Directional Blur", {{"length", 60.}}) && !leavesCard("Directional Blur", {{"length", 20.}}),
          "a long Directional Blur is left to the CPU, a short one is not");
    check(leavesCard("Speed Lines", {{"count", 900.}}) && !leavesCard("Speed Lines", {{"count", 400.}}),
          "Speed Lines past 500 streaks are left to the CPU");
  }
  // ---- the processor runs the shaders too (SDK 10) --------------------------------------
  // Each effect's shader, compiled for the processor from the very text the card
  // compiles, draws what the effect's own process() draws, to the card's
  // tolerances: an effect that brings only a shader is then the same effect
  // wherever it runs, and nobody writes it twice.
  {
    QStringList failed;
    const int ran = cpu_shader::check(failed);
    check(failed.isEmpty(), QString("the shader compiler for the processor gets its %1 small shaders right%2")
                                .arg(ran).arg(failed.isEmpty() ? QString() : ": " + failed.join("; ")));
    QStringList refused;
    int shaders = 0;
    for (const PackEffect *e : packEffects()) {
      if (e->shader.isEmpty()) continue;
      ++shaders;
      QString why;
      if (!cpuShaderProgram(e->shader, &why)) refused << e->id + " (" + why + ")";
    }
    check(refused.isEmpty() && shaders >= 50, QString("every effect's shader (%1) compiles for the processor%2")
                                                  .arg(shaders).arg(refused.isEmpty() ? QString() : ": " + refused.join("; ")));
    QImage patch(48, 32, QImage::Format_RGBA32FPx4);
    for (int y = 0; y < patch.height(); ++y)
      for (int x = 0; x < patch.width(); ++x)
        patch.setPixelColor(x, y, QColor::fromRgbF(x / 47.f, y / 31.f, float((x * 7 + y * 3) % 17) / 16.f, y < 24 ? 1.f : .5f));
    for (const auto &[id, s] : colourEffectCases()) {
      const PackEffect &e = *packEffect(id);
      Layer l; l.surfaceEffects[id] = s;
      const auto values = packEffectValues(l, e, 0);
      QImage native = patch;
      runPackEffect(native, e, values, {0, 0, 48, 32}, 1, 0, 0);
      GpuPass pass;
      if (!packEffectPass(e, values, 32, pass)) { check(false, id + " has a shader"); continue; }
      const QImage shader = cpuRunPasses(patch, {pass});
      const double most = shader.isNull() ? 1e9 : pictureDifference(native, shader).first;
      check(most < 2e-3, QString("%1's shader on the processor matches its process() (largest difference %2)").arg(id).arg(most, 0, 'g', 3));
    }
    QImage framed(96, 64, QImage::Format_RGBA32FPx4);
    for (int y = 0; y < framed.height(); ++y)
      for (int x = 0; x < framed.width(); ++x) {
        const bool inside = x > 6 && x < 88 && y > 5 && y < 58;
        framed.setPixelColor(x, y, QColor::fromRgbF(x / 95.f, y / 63.f, float((x * 7 + y * 3) % 17) / 16.f,
                                                    !inside ? 0.f : y < 40 ? 1.f : .6f));
      }
    for (const auto &c : sceneEffectCases()) {
      const PackEffect *e = packEffect(c.id);
      if (!e) { check(false, c.id + " is loaded"); continue; }
      Layer l; l.surfaceEffects[c.id] = c.settings;
      const auto values = packEffectValues(l, *e, c.frame);
      QImage native = framed;
      runPackEffect(native, *e, values, c.box, c.scale, c.seconds, c.frame);
      const auto passes = packEffectPasses(*e, values, 32, framed.size(), c.box, c.scale, c.seconds, c.frame);
      if (passes.isEmpty()) { check(false, c.id + " has a shader"); continue; }
      const QImage shader = cpuRunPasses(framed, passes);
      const auto [most, mean] = shader.isNull() ? std::pair<double, double>{1e9, 1e9} : pictureDifference(native, shader);
      check(most < 2e-3 || (mean < 2e-4 && most < .05),
            QString("%1's shader on the processor matches its process() (%2; largest difference %3, mean %4)")
                .arg(c.id, c.settings.isEmpty() ? QString("defaults") : QString::fromUtf8(QJsonDocument(c.settings).toJson(QJsonDocument::Compact)))
                .arg(most, 0, 'g', 3).arg(mean, 0, 'g', 3));
    }
  }
  // ---- effects with nothing but a shader: SDK 10's examples ------------------------------
  {
    const PackEffect *duotone = packEffect("Duotone"), *bloom = packEffect("Soft Bloom"), *wobble = packEffect("Glass Wobble");
    const auto shaderOnly = [](const PackEffect *e) { return e && !e->api->process && e->cpuProgram; };
    check(shaderOnly(duotone) && shaderOnly(bloom) && shaderOnly(wobble) && duotone->file.endsWith(".wfxs") &&
              wobble->file.endsWith(".wfx"),
          "the SDK's examples load: Duotone and Soft Bloom written as text, Glass Wobble a pack whose effect is only a shader");
    if (shaderOnly(duotone) && shaderOnly(bloom) && shaderOnly(wobble)) {
      // Duotone: a grey ramp goes from its Shadows colour to its Highlights.
      QImage ramp(64, 8, QImage::Format_RGBA32FPx4);
      for (int y = 0; y < ramp.height(); ++y)
        for (int x = 0; x < ramp.width(); ++x) ramp.setPixelColor(x, y, QColor::fromRgbF(x / 63.f, x / 63.f, x / 63.f, 1));
      Layer l;
      auto values = packEffectValues(l, *duotone, 0);
      QImage toned = ramp;
      runPackEffect(toned, *duotone, values, {0, 0, 64, 8}, 1, 0, 0);
      const QColor dark = toned.pixelColor(0, 3), light = toned.pixelColor(63, 3);
      check(std::abs(dark.redF() - 0x1b / 255.) < 2e-3 && std::abs(dark.blueF() - 0x46 / 255.) < 2e-3 &&
                std::abs(light.redF() - 1) < 2e-3 && std::abs(light.greenF() - 0xc4 / 255.) < 2e-3,
            QString("Duotone turns black to its Shadows and white to its Highlights (%1, %2)").arg(dark.name(), light.name()));
      l.surfaceEffects["Duotone"] = QJsonObject{{"amount", 0.}};
      const bool idle = packEffectInert(*duotone, packEffectValues(l, *duotone, 0));
      l.surfaceEffects["Duotone"] = QJsonObject{{"amount", 40.}};
      check(idle && !packEffectInert(*duotone, packEffectValues(l, *duotone, 0)), "a text effect's \"inert\" is worked out from its values");
      // A strip at a time, told where it lies, is the same as the whole raster.
      {
        QImage source(301, 177, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < source.height(); ++y)
          for (int x = 0; x < source.width(); ++x)
            source.setPixelColor(x, y, QColor::fromRgb(QRgb(qRgba((x * 5) % 256, (y * 9) % 256, (x + y) % 256, (x * 7 + y * 3) % 256))));
        Layer layer;
        layer.appliedEffects = QJsonObject{{"Duotone", true}};
        layer.surfaceEffects["Duotone"] = QJsonObject{{"amount", 70.}, {"response", QJsonArray{0, .1, .6, .9, 1}}};
        QImage whole = source.convertToFormat(QImage::Format_RGBA32FPx4);
        runPackEffectInPlace(whole, *duotone, packEffectValues(layer, *duotone, 0), {0, 0, 301, 177}, 1, 0, 0, &layer);
        whole = whole.convertToFormat(source.format());
        QImage strips;
        {
          CpuPixelScope only;
          EffectTimeScope time(0, 0);
          strips = applyLayerEffects(source, layer, {0, 0, 301, 177}, 1, 0);
        }
        check(strips.format() == whole.format() && strips == whole && strips != source,
              "a text effect marked perPixel runs a strip at a time with the same bytes as over the whole raster");
      }
      // Soft Bloom: two passes; a bright dot glows past its edge, by about the radius.
      QImage dot(80, 80, QImage::Format_RGBA32FPx4);
      dot.fill(QColor::fromRgbF(0, 0, 0, 1));
      for (int y = 36; y < 44; ++y)
        for (int x = 36; x < 44; ++x) dot.setPixelColor(x, y, QColor::fromRgbF(1, 1, 1, 1));
      Layer b;
      const auto bloomValues = packEffectValues(b, *bloom, 0);
      const auto passes = packEffectPasses(*bloom, bloomValues, 32, dot.size(), {0, 0, 80, 80});
      QImage glowing = dot;
      runPackEffect(glowing, *bloom, bloomValues, {0, 0, 80, 80}, 1, 0, 0);
      check(passes.size() == 2 && glowing.pixelColor(48, 40).redF() > .05 && glowing.pixelColor(70, 40).redF() < .01 &&
                std::abs(layerPackReach([&] { Layer r; r.appliedEffects["Soft Bloom"] = true; return r; }()) - 18) < 1e-6,
            QString("Soft Bloom runs in two passes, glows past a bright dot (%1 four pixels out) and makes room for it")
                .arg(glowing.pixelColor(48, 40).redF(), 0, 'f', 3));
      b.surfaceEffects["Soft Bloom"] = QJsonObject{{"intensity", 0.}};
      const auto off = packEffectValues(b, *bloom, 0);
      int tint = -1;
      for (int i = 0; i < bloom->api->parameterCount; ++i)
        if (QLatin1String(bloom->api->parameters[i].id ? bloom->api->parameters[i].id : "") == QLatin1String("tint")) tint = i;
      check(bloom->controlState && tint >= 0 && bloom->controlState(off.constData(), int32_t(off.size()), tint) == WFX_CONTROL_DIMMED &&
                bloom->controlState(bloomValues.constData(), int32_t(bloomValues.size()), tint) == 0,
            "a text effect's control rules dim Tint while Intensity is 0");
      // Glass Wobble: an SDK 10 pack in C, with nothing but a shader to draw with.
      Layer w;
      w.appliedEffects["Glass Wobble"] = true;
      const auto wobbleValues = packEffectValues(w, *wobble, 0);
      QImage early = dot, late = dot;
      runPackEffect(early, *wobble, wobbleValues, {0, 0, 80, 80}, 1, 0, 0);
      runPackEffect(late, *wobble, wobbleValues, {0, 0, 80, 80}, 1, 1.5, 45);
      w.surfaceEffects["Glass Wobble"] = QJsonObject{{"amount", 0.}};
      check(early != dot.convertToFormat(early.format()) && early != late && packEffectInert(*wobble, packEffectValues(w, *wobble, 0)) &&
                layerPackEffectMoves([&] { Layer m; m.appliedEffects["Glass Wobble"] = true; return m; }(), 0),
            "Glass Wobble bends the picture, moves with time, and is skipped at Amount 0");
      // The same shaders on the card: one effect, wherever it runs.
      QImage scene(96, 64, QImage::Format_RGBA32FPx4);
      for (int y = 0; y < scene.height(); ++y)
        for (int x = 0; x < scene.width(); ++x)
          scene.setPixelColor(x, y, QColor::fromRgbF(x / 95.f, y / 63.f, (x / 8 + y / 8) % 2 ? .9f : .2f, x > 4 && x < 90 ? 1.f : .3f));
      bool card = true;
      for (const PackEffect *e : {duotone, bloom, wobble}) {
        Layer l;
        const auto values = packEffectValues(l, *e, 0);
        const auto passes = packEffectPasses(*e, values, 32, scene.size(), {4, 4, 88, 56}, 1, .75, 22);
        const QImage gpu = gpuRunPasses(scene, passes);
        if (gpu.isNull()) { card = false; break; }
        const auto [most, mean] = pictureDifference(cpuRunPasses(scene, passes), gpu);
        check(most < 2e-3 || (mean < 2e-4 && most < .05),
              QString("%1 on the card matches the same shader on the processor (largest difference %2, mean %3)")
                  .arg(e->id).arg(most, 0, 'g', 3).arg(mean, 0, 'g', 3));
      }
      if (!card) report << "SKIP: no graphics card here to run the SDK's examples on";
    }
    // What is wrong with a text effect is said, with the file's own line.
    QString why;
    const bool refusedShader = !readTextEffect("/* Wavy-Flo effect {\"id\": \"Broken\"} */\nvec4 wavyEffect(vec4 c, vec2 p, vec2 s) {\n"
                                               "  return c * nothing;\n}\n", "broken.wfxs", &why);
    check(refusedShader && why.contains("line 3") && why.contains("nothing"), "a text effect with a mistake in its shader is refused, at its line: " + why);
    const bool refusedJson = !readTextEffect("/* Wavy-Flo effect {\"id\": \"Broken\", \"inert\": \"param_size >\"} */\n"
                                             "vec4 wavyEffect(vec4 c, vec2 p, vec2 s) { return c; }\n", "broken.wfxs", &why);
    check(refusedJson && why.contains("inert"), "and one whose \"inert\" does not compile, naming it: " + why);
  }
  // ---- Speed Lines and Posterize Time ---------------------------------------------------
  {
    const PackEffect *lines = packEffect("Speed Lines"), *steps = packEffect("Posterize Time");
    check(lines && steps && QFileInfo(lines->file).completeBaseName() == "Wavy-Motion",
          "Wavy-Motion brings Speed Lines and Posterize Time");
    if (lines && steps) {
      QImage dark(640, 360, QImage::Format_ARGB32_Premultiplied);
      dark.fill(QColor(10, 10, 16));
      Scene scene; scene.width = 640; scene.height = 360; scene.fps = 30; scene.frames = 60;
      Layer base; base.id = "lines"; base.type = "image"; base.image = dark; base.duration = 60;
      base.p[0].base = 320; base.p[1].base = 180;
      base.appliedEffects["Speed Lines"] = true;
      auto render = [&](const Layer &l, double frame) {
        scene.layers = {l};
        clearRenderCaches();
        return renderCPU(scene, frame, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
      };
      auto bright = [](QRgb c) { return qRed(c) > 128; };
      // How long the bright runs are, across and down: streaks are long one way.
      auto runs = [&](const QImage &img, bool across) {
        double total = 0; int count = 0;
        const int outer = across ? img.height() : img.width(), inner = across ? img.width() : img.height();
        for (int a = 0; a < outer; ++a) {
          int run = 0;
          for (int b = 0; b <= inner; ++b) {
            const bool on = b < inner && bright(across ? img.pixel(b, a) : img.pixel(a, b));
            if (on) ++run;
            else if (run) { total += run; ++count; run = 0; }
          }
        }
        return count ? total / count : 0.;
      };
      Layer flat = base;
      flat.surfaceEffects["Speed Lines"] = QJsonObject{{"mode", 0}, {"count", 40}, {"thickness", 6}, {"speed", 0}};
      const QImage h = render(flat, 0);
      const double along = runs(h, true), down = runs(h, false);
      check(along > down * 6, QString("Horizontal speed lines run side to side (%1 px long, %2 px thick)")
                                  .arg(along, 0, 'f', 1).arg(down, 0, 'f', 1));
      Layer tilted = flat;
      tilted.surfaceEffects["Speed Lines"] = QJsonObject{{"mode", 0}, {"count", 40}, {"thickness", 6}, {"speed", 0}, {"angle", 90}};
      const QImage v = render(tilted, 0);
      check(runs(v, false) > runs(v, true) * 6, "at 90 degrees they run up and down");
      // Radial: streaks pointing in, the middle left clear.
      Layer radial = base;
      radial.surfaceEffects["Speed Lines"] = QJsonObject{{"mode", 1}, {"count", 120}, {"thickness", 8}, {"length", 50},
                                                          {"variation", 0}, {"speed", 0}};
      const QImage r = render(radial, 0);
      int middle = 0, edge = 0;
      for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x) {
          const double d = std::hypot(x - 320., y - 180.);
          if (!bright(r.pixel(x, y))) continue;
          if (d < 120) ++middle;
          else if (d > 280) ++edge;
        }
      check(middle == 0 && edge > 2000, QString("Radial speed lines leave the middle clear (%1 lit there) and crowd the edge (%2)")
                                            .arg(middle).arg(edge));
      // Dealt again with time; still at Speed 0.
      Layer flicker = flat;
      flicker.surfaceEffects["Speed Lines"] = QJsonObject{{"mode", 0}, {"count", 40}, {"thickness", 6}, {"speed", 12}};
      check(render(flicker, 0) != render(flicker, 15) && render(flat, 0) == render(flat, 15),
            "Speed Lines are dealt again with Speed, and stay put at Speed 0");
      Layer only = flat;
      only.surfaceEffects["Speed Lines"] = QJsonObject{{"mode", 0}, {"count", 40}, {"thickness", 6}, {"speed", 0}, {"linesOnly", true}};
      const QImage o = render(only, 0);
      int clear = 0;
      for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x) clear += qAlpha(o.pixel(x, y)) == 0;
      check(clear > 640 * 360 / 2, "Lines Only keeps the streaks and nothing of the layer");

      // Posterize Time: a move at 30 fps held in steps of three frames at 10 fps.
      Layer mover; mover.id = "mover"; mover.type = "image"; mover.image = QImage(40, 40, QImage::Format_ARGB32_Premultiplied);
      mover.image.fill(Qt::white); mover.duration = 60; mover.p[1].base = 180;
      mover.p[0].set(0, 40); mover.p[0].set(30, 600);
      mover.appliedEffects["Posterize Time"] = true;
      mover.surfaceEffects["Posterize Time"] = QJsonObject{{"rate", 10}};
      check(posterizedTime(mover, scene, 4) == 3 && posterizedTime(mover, scene, 5.9) == 3 && posterizedTime(mover, scene, 6) == 6,
            "Posterize Time 10 at 30 fps holds each picture three frames");
      const QImage f3 = render(mover, 3), f4 = render(mover, 4), f5 = render(mover, 5), f6 = render(mover, 6);
      check(f3 == f4 && f4 == f5 && f5 != f6, "a posterized layer stands still for three frames, then jumps on");
      Layer smooth = mover;
      smooth.hiddenEffects["Posterize Time"] = true;
      check(render(smooth, 3) != render(smooth, 4), "with Posterize Time off it moves every frame");
      Layer fast = mover;
      fast.surfaceEffects["Posterize Time"] = QJsonObject{{"rate", 60}};
      check(posterizedTime(fast, scene, 4.5) == 4.5, "at the composition's rate or faster nothing is held");
      // On an Effect Layer the streaks go over everything beneath it.
      Layer under = base; under.appliedEffects = QJsonObject{};
      Layer over; over.id = "over"; over.type = "effect"; over.duration = 60;
      over.p[0].base = 320; over.p[1].base = 180;
      over.appliedEffects["Speed Lines"] = true;
      over.surfaceEffects["Speed Lines"] = QJsonObject{{"mode", 1}, {"count", 120}, {"thickness", 8}, {"speed", 0}};
      scene.layers = {over, under};
      clearRenderCaches();
      const QImage covered = renderCPU(scene, 0, false, QSize(640, 360)).convertToFormat(QImage::Format_ARGB32);
      int lit = 0;
      for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x) lit += bright(covered.pixel(x, y));
      check(lit > 2000, QString("Speed Lines on an Effect Layer draw over the layers beneath (%1 px)").arg(lit));
    }
  }
  // ---- effects written as text, changed while running -----------------------------------
  {
    QTemporaryDir folder;
    if (!folder.isValid()) {
      report << "SKIP: no temporary folder for effects written while running";
    } else {
      const QString file = QDir(folder.path()).filePath("live-test.wfxs");
      auto write = [&](const QString &where, const QString &text) {
        QFile f(where);
        if (f.open(QIODevice::WriteOnly)) f.write(text.toUtf8());
      };
      auto effectText = [](const QString &id, const QString &colour) {
        return "/* Wavy-Flo effect {\"id\": \"" + id + "\", \"perPixel\": true,\n"
               "  \"parameters\": [{\"id\": \"amount\", \"min\": 0, \"max\": 100, \"default\": 100}]} */\n"
               "vec4 wavyEffect(vec4 c, vec2 p, vec2 s) { return mix(c, vec4(" + colour + ", c.a), param_amount / 100.0); }\n";
      };
      // The colour a version turns a grey pixel.
      auto drawn = [](const PackEffect *e) {
        QImage pixel(4, 4, QImage::Format_RGBA32FPx4);
        pixel.fill(QColor(128, 128, 128));
        Layer none;
        if (e) runPackEffect(pixel, *e, packEffectValues(none, *e, 0), {0, 0, 4, 4}, 1, 0, 0);
        return pixel.pixelColor(1, 1);
      };
      write(file, effectText("Live Test", "1.0, 0.0, 0.0"));
      const int added = loadEffectPacks(folder.path(), true);
      const PackEffect *first = packEffect("Live Test");
      check(added == 1 && first && first->revision == 0 && textEffectId(file) == "Live Test" && drawn(first).redF() > .9,
            "an effect written as text loads from a folder of one's own, as at start-up");
      Layer user;
      user.appliedEffects["Live Test"] = true;
      const QByteArray identityBefore = layerPackIdentity(user, 0);
      // Read again: a new version under the same id. The one before is kept
      // for a frame that may still be drawing with it.
      write(file, effectText("Live Test", "0.0, 0.0, 1.0"));
      const QStringList changed = reloadTextEffect(file);
      const PackEffect *second = packEffect("Live Test");
      check(changed == QStringList{"Live Test"} && second && second != first && second->revision > 0 && drawn(second).blueF() > .9 &&
                first->id == "Live Test" && drawn(first).redF() > .9 && layerPackIdentity(user, 0) != identityBefore &&
                !layerPackRevisions(user).isEmpty(),
            "read again it is a new version: the new shader draws, every cache sees another layer, the old version still works");
      // Text not saved yet, as the effect editor's Live puts it in.
      const QByteArray typed = effectText("Live Test", "0.0, 1.0, 0.0").toUtf8();
      check(reloadTextEffect(file, &typed) == QStringList{"Live Test"} && drawn(packEffect("Live Test")).greenF() > .9,
            "text not saved yet takes its place");
      // A mistake leaves the last good version in use, and says where.
      const PackEffect *good = packEffect("Live Test");
      QByteArray broken = typed;
      broken.replace("param_amount", "param_amont");
      const bool refused = reloadTextEffect(file, &broken).isEmpty();
      const QString problem = textEffectProblem(file);
      check(refused && packEffect("Live Test") == good && problem.contains("line 3") &&
                packProblems().join('\n').contains("live-test.wfxs") && textEffectProblems().size() == 1,
            "one with a mistake is refused, the last good version stays in use, and the problem names its line: " + problem);
      check(!reloadTextEffect(file).isEmpty() && textEffectProblem(file).isEmpty() && !packProblems().join('\n').contains("live-test.wfxs"),
            "mended, it loads again and the problem goes");
      // Another id: the old one goes, the new one comes.
      write(file, effectText("Live Test 2", "1.0, 1.0, 0.0"));
      const QStringList renamed = reloadTextEffect(file);
      check(renamed.contains("Live Test 2") && renamed.contains("Live Test") && !packEffect("Live Test") && packEffect("Live Test 2"),
            "given another id, the old one goes and the new one comes");
      // An id another effect has is refused; the version before stays.
      write(file, effectText("Duotone", "1.0, 1.0, 1.0"));
      const bool taken = reloadTextEffect(file).isEmpty();
      const QString takenWhy = textEffectProblem(file);
      check(taken && takenWhy.contains("already provided") && packEffect("Live Test 2") && packEffect("Duotone") &&
                !packEffect("Duotone")->file.contains("live-test"),
            "an id another effect has is refused, and the version before stays: " + takenWhy);
      // Taken away, its effect goes; a new file brings one.
      QFile::remove(file);
      check(reloadTextEffect(file) == QStringList{"Live Test 2"} && !packEffect("Live Test 2") && textEffectProblem(file).isEmpty(),
            "a file taken away takes its effect with it");
      const QString arrived = QDir(folder.path()).filePath("arrived.wfxs");
      write(arrived, effectText("Arrived", "1.0, 0.0, 1.0"));
      check(reloadTextEffect(arrived) == QStringList{"Arrived"} && packEffect("Arrived") && packEffect("Arrived")->revision > 0,
            "a new file read in while running brings its effect, as a version of its own");
      QFile::remove(arrived);
      reloadTextEffect(arrived);
      // A description that is not JSON says the line.
      QString why;
      readTextEffect("/* Wavy-Flo effect\n{\n  \"id\": \"X\",\n  \"name\" \"Y\"\n}\n*/\nvec4 wavyEffect(vec4 c, vec2 p, vec2 s) { return c; }\n",
                     "x.wfxs", &why);
      check(why.contains("line 4"), "a description that is not JSON names its line: " + why);
    }
  }
  // ---- writing effects inside Wavy-Flo (effect_editor.h) --------------------------------
  {
    // Every starting point New Effect offers compiles and draws.
    const QStringList starters = effect_editor::starterNames();
    for (int i = 0; i < starters.size(); ++i) {
      QString why;
      const auto e = readTextEffect(effect_editor::starterText(i, "Starter Test", "Starter Test", "Stylize").toUtf8(), "starter.wfxs", &why);
      bool finite = e != nullptr;
      if (e) {
        QImage picture(24, 24, QImage::Format_RGBA32FPx4);
        for (int y = 0; y < 24; ++y)
          for (int x = 0; x < 24; ++x) picture.setPixelColor(x, y, QColor::fromRgbF(x / 23.f, y / 23.f, .5f, x > 3 && x < 20 ? 1.f : .4f));
        Layer none;
        runPackEffect(picture, *e, packEffectValues(none, *e, 0), {2, 2, 20, 20}, 1, .5, 15);
        for (int y = 0; y < 24; ++y) {
          const float *row = reinterpret_cast<const float *>(picture.constScanLine(y));
          for (int k = 0; k < 24 * 4; ++k) finite = finite && std::isfinite(row[k]);
        }
      }
      check(finite, QString("New Effect's starting point \"%1\" compiles and draws%2").arg(starters[i], why.isEmpty() ? QString() : ": " + why));
    }
    // + Parameter: every kind, after the parameters there are.
    QString text = effect_editor::starterText(0, "Param Test", "Param Test", "Stylize");
    int count = 3;
    QStringList refused;
    for (const QString &kind : {"float", "angle", "bool", "choice", "color", "curve", "point", "group"}) {
      text = effect_editor::withParameter(text, kind);
      count += kind == "group" ? 2 : 1;
      QString why;
      const auto e = readTextEffect(text.toUtf8(), "p.wfxs", &why);
      if (!e || e->api->parameterCount != count) refused << kind + (why.isEmpty() ? QString() : " (" + why + ")");
    }
    check(refused.isEmpty(), "+ Parameter adds every kind and the effect still loads" + (refused.isEmpty() ? QString() : ": " + refused.join("; ")));
    QString first, second;
    const QString twice = effect_editor::withParameter(effect_editor::withParameter(text, "float", &first), "float", &second);
    check(!first.isEmpty() && first != second && twice.contains("\"" + second + "\""), "each new one gets an id of its own: " + first + ", " + second);
    const QString emptyList = "/* Wavy-Flo effect {\"id\": \"E\", \"parameters\": []} */\nvec4 wavyEffect(vec4 c, vec2 p, vec2 s) { return c; }\n";
    const QString noList = "/* Wavy-Flo effect {\"id\": \"N\"} */\nvec4 wavyEffect(vec4 c, vec2 p, vec2 s) { return c; }\n";
    QString why;
    const auto fromEmpty = readTextEffect(effect_editor::withParameter(emptyList, "color").toUtf8(), "e.wfxs", &why);
    const auto fromNone = readTextEffect(effect_editor::withParameter(noList, "bool").toUtf8(), "n.wfxs", &why);
    check(fromEmpty && fromEmpty->api->parameterCount == 1 && fromNone && fromNone->api->parameterCount == 1,
          "and into an empty list, or a description without one");
    check(effect_editor::withParameter("vec4 wavyEffect(vec4 c, vec2 p, vec2 s) { return c; }", "float").isNull(),
          "a file without a description is left alone");
    // The line a problem is on, for the editor to go to.
    const QString badChoice = "/* Wavy-Flo effect {\"id\": \"C\",\n  \"parameters\": [\n    {\"id\": \"mode\", \"kind\": \"choice\"}\n  ]} */\n"
                              "vec4 wavyEffect(vec4 c, vec2 p, vec2 s) { return c; }\n";
    readTextEffect(badChoice.toUtf8(), "c.wfxs", &why);
    check(effect_editor::problemLine(badChoice, why) == 3, "a problem with a parameter is put on that parameter's line: " + why);
    check(effect_editor::problemLine(QString(), "its shader does not compile: line 12: unknown name") == 12,
          "and one that names its line, on that line");
    // The window itself.
    failures += effect_editor::checkWindow(report);
  }
  return failures;
}
