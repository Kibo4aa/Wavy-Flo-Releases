#include "engine.h"
#include <condition_variable>
#include <set>
#include <optional>
#include "shapes.h"
#include "expressions.h"
#include "effect_packs.h"
#include "particles.h"
#include "lights.h"
#include <atomic>
#include "gpu_pixels.h"
#include "character_animation.h"
#include "route_choice.h"
#include "render_workers.h"
#include "gpu_pixels.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <map>
#include <new>
#include <deque>
#include <future>
#include <QImageReader>
#include "image_files.h"
#include "native_media.h"
#include "video_frames.h"
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <QtGui/private/qvectorpath_p.h>
const char *propNames[PropertyCount] = {"Position X",  "Position Y",  "Scale X",
                            "Scale Y",     "Rotation",    "Opacity",
                            "Strength In", "Strength Out", "Anchor X", "Anchor Y", "Anchor Z",
                            "Position Z", "Rotation X", "Rotation Y", "Scale Z", "Zoom",
                            "Width", "Height", "Roundness", "Points", "Outer Radius", "Inner Radius",
                            "Path Rotation", "Trim Start", "Trim Offset", "Trim End",
                            "Round Corners", "Pucker & Bloat", "Zig Zag Size", "Zig Zag Ridges", "Twist", "Offset Path",
                            "Linear Wipe Completion", "Linear Wipe Angle", "Linear Wipe Feather",
                            "Radial Wipe Completion", "Radial Wipe Start", "Radial Wipe Feather",
                            "Extrude Depth", "Extrude Shade", "Extrude Bevel", "Extrude Bevel Roundness",
                            "Emission Rate", "Particle Life", "Particle Speed", "Direction", "Spread",
                            "Particle Size", "Gravity", "Wind", "Turbulence", "Spin", "Time Remap",
                            "Intensity", "Cone Angle", "Cone Feather", "Radius", "Falloff Distance",
                            "Aim X", "Aim Y", "Aim Z", "Shine", "Gloss", "Shadow Darkness", "Shadow Softness"};
const char *propKeys[PropertyCount] = {"positionX",  "positionY",  "scaleX",
                           "scaleY",     "rotation",   "opacity",
                           "strengthIn", "strengthOut", "anchorX", "anchorY", "anchorZ",
                           "positionZ", "rotationX", "rotationY", "scaleZ", "cameraZoom",
                           "shapeWidth", "shapeHeight", "shapeRoundness", "shapePoints", "outerRadius", "innerRadius",
                           "pathRotation", "shapeTrimStart", "shapeTrimOffset", "shapeTrimEnd",
                           "shapeRoundCorners", "shapePuckerBloat", "shapeZigZagSize", "shapeZigZagRidges", "shapeTwist", "shapeOffsetPath",
                           "wipeLinearCompletion", "wipeLinearAngle", "wipeLinearFeather",
                           "wipeRadialCompletion", "wipeRadialStart", "wipeRadialFeather",
                           "extrudeDepth", "extrudeShade", "extrudeBevel", "extrudeBevelRound",
                           "particleRate", "particleLife", "particleSpeed", "particleDirection", "particleSpread",
                           "particleSize", "particleGravity", "particleWind", "particleTurbulence", "particleSpin", "timeRemap",
                           "lightIntensity", "lightConeAngle", "lightConeFeather", "lightRadius", "lightFalloffDistance",
                           "lightAimX", "lightAimY", "lightAimZ", "surfaceShine", "surfaceGloss",
                           "lightShadowDarkness", "lightShadowSoftness"};
double eased(double t, const Key &k) {
  if (t <= 0)
    return 0;
  if (t >= 1)
    return 1;
  if (k.ease == "hold")
    return 0;
  if (k.ease == "linear" && !k.custom)
    return t;
  auto p = k.curve;
  if (!k.custom) {
    if (k.ease == "easeIn")
      p = {.42, 0, 1, 1};
    else if (k.ease == "easeOut")
      p = {0, 0, .58, 1};
    else
      p = {.42, 0, .58, 1};
  }
  auto cubic = [](double a, double b, double u) {
    return 3 * (1 - u) * (1 - u) * u * a + 3 * (1 - u) * u * u * b + u * u * u;
  };
  double lo = 0, hi = 1, u = t;
  for (int i = 0; i < 24; i++) {
    u = (lo + hi) / 2;
    if (cubic(p[0], p[2], u) < t)
      lo = u;
    else
      hi = u;
  }
  return cubic(p[1], p[3], u);
}
std::array<QPointF, 4> segmentPoints(const Key &a, const Key &b) {
  auto c = a.curve;
  if (!a.custom)
    c = a.ease == "linear"    ? std::array<double, 4>{0, 0, 1, 1}
        : a.ease == "easeIn"  ? std::array<double, 4>{.42, 0, 1, 1}
        : a.ease == "easeOut" ? std::array<double, 4>{0, 0, .58, 1}
                              : std::array<double, 4>{.42, 0, .58, 1};
  double dt = b.frame - a.frame, dv = b.value - a.value;
  return {
      QPointF(a.frame, a.value),
      QPointF(a.frame + dt * c[0],
              a.custom && a.absolute ? a.value + c[1] : a.value + dv * c[1]),
      QPointF(a.frame + dt * c[2],
              a.custom && a.absolute ? b.value + c[3] : a.value + dv * c[3]),
      QPointF(b.frame, b.value)};
}
void setSegment(Key &a, const Key &b, QPointF out, QPointF in) {
  double dt = b.frame - a.frame;
  if (dt <= 0)
    return;
  a.curve = {std::clamp((out.x() - a.frame) / dt, 0., 1.), out.y() - a.value,
             std::clamp((in.x() - a.frame) / dt, 0., 1.), in.y() - b.value};
  // The motion path's timing, normalised (Key::normCurve): Position's alone,
  // started by its paired-curve sync in the editor and kept in step here.
  // Any other property's handles stay as they are drawn.
  if (a.hasNormCurve) {
    const double dv = b.value - a.value;
    if (std::abs(dv) > 1e-3) {
      a.normCurve = {std::clamp(a.curve[1] / dv, -3.0, 3.0),
                     std::clamp(a.curve[3] / dv, -3.0, 3.0)};
    } else {
      a.hasNormCurve = false;
    }
  }
  a.custom = true;
  a.absolute = true;
  a.ease = "custom";
}
static QPointF lerp(QPointF a, QPointF b, double t) { return a + (b - a) * t; }
int splitSegment(Property &p, int index, double frame) {
  if (index < 0 || index + 1 >= int(p.keys.size()))
    return -1;
  auto a = p.keys[index], b = p.keys[index + 1];
  if (frame <= a.frame + 1e-6 || frame >= b.frame - 1e-6 || a.ease == "hold")
    return -1;
  auto c = segmentPoints(a, b);
  double lo = 0, hi = 1, t = .5;
  for (int i = 0; i < 48; i++) {
    t = (lo + hi) * .5;
    auto q = lerp(lerp(lerp(c[0], c[1], t), lerp(c[1], c[2], t), t),
                  lerp(lerp(c[1], c[2], t), lerp(c[2], c[3], t), t), t);
    if (q.x() < frame)
      lo = t;
    else
      hi = t;
  }
  auto l0 = lerp(c[0], c[1], t), l1 = lerp(c[1], c[2], t),
       l2 = lerp(c[2], c[3], t), h0 = lerp(l0, l1, t), h1 = lerp(l1, l2, t),
       point = lerp(h0, h1, t);
  Key key;
  key.frame = frame;
  key.value = point.y();
  key.hasNormCurve = a.hasNormCurve;
  setSegment(a, key, l0, h0);
  setSegment(key, b, h1, l2);
  p.keys[index] = a;
  p.keys.insert(p.keys.begin() + index + 1, key);
  return index + 1;
}
double Property::at(double frame) const {
  if (hasExpression()) return evaluateExpression(*program, *this, frame);
  return keyedAt(frame);
}
bool Property::setExpression(const QString &text, QString *error) {
  expression = text;
  program.reset();
  if (text.trimmed().isEmpty()) {
    expression.clear();
    if (error) error->clear();
    return true;
  }
  QString why;
  program = compileExpression(text, &why);
  if (error) *error = why;
  return program != nullptr;
}
double Property::keyedAt(double frame) const {
  if (!std::is_sorted(keys.begin(), keys.end(), [](const Key &a, const Key &b) {
        return a.frame < b.frame;
      })) {
    Property ordered;
    ordered.base = base;
    ordered.keys = keys;
    std::sort(ordered.keys.begin(), ordered.keys.end(),
              [](const Key &a, const Key &b) { return a.frame < b.frame; });
    return ordered.keyedAt(frame);
  }
  if (keys.empty())
    return base;
  if (frame <= keys.front().frame)
    return keys.front().value;
  for (size_t i = 0; i + 1 < keys.size(); i++) {
    if (frame <= keys[i + 1].frame) {
      const auto &a = keys[i];
      const auto &b = keys[i + 1];
      if (a.custom && a.absolute && a.ease != "hold") {
        auto c = segmentPoints(a, b);
        double lo = 0, hi = 1;
        for (int n = 0; n < 40; n++) {
          double t = (lo + hi) * .5;
          auto q = lerp(lerp(lerp(c[0], c[1], t), lerp(c[1], c[2], t), t),
                        lerp(lerp(c[1], c[2], t), lerp(c[2], c[3], t), t), t);
          if (q.x() < frame)
            lo = t;
          else
            hi = t;
        }
        double t = (lo + hi) * .5;
        return lerp(lerp(lerp(c[0], c[1], t), lerp(c[1], c[2], t), t),
                    lerp(lerp(c[1], c[2], t), lerp(c[2], c[3], t), t), t)
            .y();
      }
      return a.value + (b.value - a.value) *
                           eased((frame - a.frame) / (b.frame - a.frame), a);
    }
  }
  return keys.back().value;
}
void Property::set(double frame, double value) {
  auto applyValueAt = [&](size_t i) {
    if (i > 0) {
      auto &prev = keys[i - 1];
      if (prev.custom && prev.absolute && prev.hasNormCurve) {
        prev.normCurve[0] = std::clamp(prev.normCurve[0], -3.0, 3.0);
        prev.normCurve[1] = std::clamp(prev.normCurve[1], -3.0, 3.0);
        const double dvNew = value - prev.value;
        if (std::abs(dvNew) > 1e-6) {
          prev.curve[1] = prev.normCurve[0] * dvNew;
          prev.curve[3] = prev.normCurve[1] * dvNew;
        }
      }
    }
    if (i + 1 < keys.size()) {
      auto &cur = keys[i];
      if (cur.custom && cur.absolute && cur.hasNormCurve) {
        cur.normCurve[0] = std::clamp(cur.normCurve[0], -3.0, 3.0);
        cur.normCurve[1] = std::clamp(cur.normCurve[1], -3.0, 3.0);
        const double dvNew = keys[i + 1].value - value;
        if (std::abs(dvNew) > 1e-6) {
          cur.curve[1] = cur.normCurve[0] * dvNew;
          cur.curve[3] = cur.normCurve[1] * dvNew;
        }
      }
    }
    keys[i].value = value;
  };
  for (size_t i = 0; i < keys.size(); ++i)
    if (std::abs(keys[i].frame - frame) < .001) {
      applyValueAt(i);
      return;
    }
  if (!std::is_sorted(keys.begin(), keys.end(),
                      [](const Key &a, const Key &b) { return a.frame < b.frame; })) {
    std::sort(keys.begin(), keys.end(),
              [](const Key &a, const Key &b) { return a.frame < b.frame; });
  }
  for (size_t i = 0; i + 1 < keys.size(); ++i) {
    if (frame > keys[i].frame + .001 && frame < keys[i + 1].frame - .001) {
      if (keys[i].custom && keys[i].absolute && keys[i].ease != "hold") {
        const int splitIdx = splitSegment(*this, int(i), frame);
        if (splitIdx >= 0 && size_t(splitIdx) < keys.size()) {
          applyValueAt(size_t(splitIdx));
          return;
        }
      }
      Key k;
      k.frame = frame;
      k.value = value;
      k.ease = keys[i].ease;
      keys.insert(keys.begin() + int(i) + 1, k);
      return;
    }
  }
  Key k;
  k.frame = frame;
  k.value = value;
  keys.push_back(k);
  std::sort(keys.begin(), keys.end(),
            [](const Key &a, const Key &b) { return a.frame < b.frame; });
}
Layer::Layer() {
  p[0].base = 640;
  p[1].base = 360;
  p[2].base = p[3].base = p[5].base = 1;
  p[6].base = 100;
  p[ScaleZ].base = 1;
  p[CameraZoom].base = 1000;
  p[ShapeWidth].base = 200; p[ShapeHeight].base = 120;
  p[ShapePoints].base = 5; p[OuterRadius].base = 100; p[InnerRadius].base = 50;
  p[ShapeTrimStart].base = 0; p[ShapeTrimOffset].base = 0; p[ShapeTrimEnd].base = 100;
  p[ShapeZigZagRidges].base = 4; // per hundred pixels of outline; the other modifiers start at nothing
  p[WipeLinearAngle].base = 90; // AE's default: the edge moves downward
  p[WipeRadialStart].base = 0;
  p[ExtrudeShade].base = 55; // how much darker the sides get than the front, in percent
  // A particle layer starts as a gentle upward fountain.
  p[ParticleRate].base = 40; p[ParticleLife].base = 3; p[ParticleSpeed].base = 150;
  p[ParticleSpread].base = 30; p[ParticleSize].base = 12;
  // A light starts at full strength with a soft-edged ninety degree cone.
  p[LightIntensity].base = 100; p[LightConeAngle].base = 90; p[LightConeFeather].base = 50;
  p[LightRadius].base = 500; p[LightFalloffDistance].base = 500;
  p[LightAimX].base = 960; p[LightAimY].base = 540;
  // A soft sheen: enough to see where a lamp is, not enough to wash a layer out.
  p[SurfaceShine].base = 20; p[SurfaceGloss].base = 30;
  // Shadows as dark as the light is bright, with a slightly soft edge.
  p[LightShadowDarkness].base = 100; p[LightShadowSoftness].base = 6;
}
namespace {
// The same shape a fixed property is written in, so an extra curve reads the
// same way in a saved file and nothing has to learn a second format.
void writeExpression(const Property &p, QJsonObject &j) {
  if (p.expression.isEmpty()) return;
  j["expression"] = p.expression;
  if (!p.expressionOn) j["expressionOn"] = false;
}
bool readExpression(const QJsonObject &j, Property &into) {
  const QString text = j["expression"].toString();
  if (text.size() > 20000) return false;
  into.expressionOn = j["expressionOn"].toBool(true);
  if (!text.isEmpty()) into.setExpression(text);
  return true;
}
QJsonObject curveJson(const Property &p) {
  QJsonArray keys;
  for (const auto &k : p.keys) {
    QJsonObject j{{"time", k.frame}, {"value", k.value}, {"easing", k.ease}};
    if (k.custom) j["bezier"] = QJsonArray{k.curve[0], k.curve[1], k.curve[2], k.curve[3]};
    if (k.hasNormCurve) j["normCurve"] = QJsonArray{k.normCurve[0], k.normCurve[1]};
    if (k.hasSpatial) {
      j["hasSpatial"] = true;
      j["spatialOut"] = QJsonArray{k.spatialOut.x(), k.spatialOut.y()};
      j["spatialIn"] = QJsonArray{k.spatialIn.x(), k.spatialIn.y()};
    }
    if (k.timing) j["timing"] = true;
    j["absoluteHandles"] = k.absolute;
    j["continuous"] = k.continuous;
    keys.append(j);
  }
  QJsonObject j{{"value", p.base}, {"keyframes", keys}};
  writeExpression(p, j);
  return j;
}
bool curveFromJson(const QJsonObject &j, Property &into) {
  into.base = j["value"].toDouble(into.base);
  if (!readExpression(j, into)) return false;
  const auto keys = j["keyframes"].toArray();
  if (keys.size() > 5000) return false;
  for (auto value : keys) {
    const auto k = value.toObject();
    Key key;
    key.frame = k["time"].toDouble(-1);
    key.value = k["value"].toDouble();
    key.ease = k["easing"].toString("linear");
    key.absolute = k["absoluteHandles"].toBool();
    key.continuous = k["continuous"].toBool(true);
    key.hasSpatial = k["hasSpatial"].toBool(false);
    if (key.hasSpatial) {
      const auto so = k["spatialOut"].toArray();
      const auto si = k["spatialIn"].toArray();
      if (so.size() == 2) key.spatialOut = QPointF(so[0].toDouble(), so[1].toDouble());
      if (si.size() == 2) key.spatialIn = QPointF(si[0].toDouble(), si[1].toDouble());
    }
    key.timing = k["timing"].toBool(false);
    if (!std::isfinite(key.frame) || key.frame < 0) return false;
    const auto curve = k["bezier"].toArray();
    if (curve.size() == 4) {
      key.custom = true;
      for (int n = 0; n < 4; n++) key.curve[n] = curve[n].toDouble();
    }
    const auto nc = k["normCurve"].toArray();
    if (nc.size() == 2) {
      key.normCurve = {nc[0].toDouble(), nc[1].toDouble()};
      key.hasNormCurve = true;
    }
    into.keys.push_back(key);
  }
  std::sort(into.keys.begin(), into.keys.end(),
            [](const Key &a, const Key &b) { return a.frame < b.frame; });
  for (size_t n = 1; n < into.keys.size(); n++)
    if (into.keys[n].frame == into.keys[n - 1].frame) return false;
  return true;
}
}  // namespace

namespace {
QJsonArray markersJson(const std::vector<Marker> &markers) {
  QJsonArray array;
  for (const auto &m : markers) {
    QJsonObject j{{"frame", m.frame}};
    if (!m.label.isEmpty()) j["label"] = m.label;
    if (m.beat) j["beat"] = true;
    array.append(j);
  }
  return array;
}
bool readMarkers(const QJsonArray &array, int frames, std::vector<Marker> &into) {
  if (array.size() > 20000) return false;
  for (const auto &value : array) {
    const auto j = value.toObject();
    Marker m;
    m.frame = j["frame"].toDouble(-1);
    if (!std::isfinite(m.frame) || m.frame < 0 || m.frame >= std::max(1, frames)) continue;
    m.label = j["label"].toString().left(200);
    m.beat = j["beat"].toBool();
    into.push_back(m);
  }
  sortMarkers(into);
  return true;
}
}  // namespace
void sortMarkers(std::vector<Marker> &markers) {
  std::stable_sort(markers.begin(), markers.end(), [](const Marker &a, const Marker &b) { return a.frame < b.frame; });
}
int lineUpInTime(std::vector<Layer> &layers, const std::vector<int> &order, const std::vector<Marker> &markers,
                 const LineUpSettings &settings) {
  std::vector<double> stops;
  if (settings.onMarkers)
    for (const Marker &m : markers)
      if (m.frame >= settings.from - 1e-6) stops.push_back(m.frame);
  std::sort(stops.begin(), stops.end());
  int moved = 0;
  double at = settings.from;
  for (size_t i = 0; i < order.size(); ++i) {
    const int index = order[i];
    if (index < 0 || index >= int(layers.size())) continue;
    if (settings.onMarkers && i < stops.size()) at = stops[i];
    else if (i > 0) at += settings.step;
    Layer &l = layers[size_t(index)];
    if (std::abs(l.start - at) > 1e-9) { l.start = at; ++moved; }
  }
  return moved;
}
bool reverseKeys(Property &p, int first, int last) {
  auto &keys = p.keys;
  if (first < 0 || last >= int(keys.size()) || last - first < 1) return false;
  const std::vector<Key> was(keys.begin() + first, keys.begin() + last + 1);
  const int m = last - first;
  const double span = was.front().frame + was.back().frame;
  std::vector<Key> turned(size_t(m) + 1);
  for (int j = 0; j <= m; ++j) {
    const Key &from = was[size_t(m - j)];
    Key &key = turned[size_t(j)];
    key.frame = span - from.frame;
    key.value = from.value;
    key.continuous = from.continuous;
    key.hasSpatial = from.hasSpatial;
    key.spatialOut = -from.spatialIn;
    key.spatialIn = -from.spatialOut;
    key.timing = from.timing;
  }
  for (int j = 0; j < m; ++j) {
    // The move from turned[j] to turned[j+1] is the old move from was[m-j-1]
    // to was[m-j], run backwards.
    const Key &a = was[size_t(m - j - 1)], &b = was[size_t(m - j)];
    Key &key = turned[size_t(j)];
    if (a.ease == "hold" || !a.custom) {
      key.ease = a.ease == "easeIn" ? "easeOut" : a.ease == "easeOut" ? "easeIn" : a.ease;
      key.custom = false;
      key.absolute = a.absolute;
      key.curve = a.curve;
    } else {
      // A shaped move: its handles mirrored in time, in and out swapped.
      const auto c = segmentPoints(a, b);
      setSegment(key, turned[size_t(j) + 1], QPointF(span - c[2].x(), c[2].y()), QPointF(span - c[1].x(), c[1].y()));
    }
  }
  // The last key's own move, onward past the range, stays the move that was there.
  const Key &onward = was.back();
  turned.back().ease = onward.ease;
  turned.back().custom = onward.custom;
  turned.back().absolute = onward.absolute;
  turned.back().curve = onward.curve;
  std::copy(turned.begin(), turned.end(), keys.begin() + first);
  return true;
}
int pulseOnMarkers(Layer &target, const std::vector<Marker> &markers, const PulseSettings &settings) {
  // The hits, in the layer's own frames, whole frames so a key lands on one.
  std::vector<double> hits;
  int counted = 0;
  for (const Marker &m : markers) {
    if (!m.beat && !settings.handMarkers) continue;
    if (counted++ % std::max(1, settings.every) != 0) continue;
    const double at = std::round(m.frame - target.start);
    if (at < 0 || at >= target.duration) continue;
    if (hits.empty() || at > hits.back()) hits.push_back(at);
  }
  if (hits.empty()) return 0;
  std::vector<int> moved;
  switch (settings.move) {
    case 0: moved = {2, 3}; break;
    case 1: moved = {5}; break;
    case 2: moved = {1}; break;
    default: moved = {4}; break;
  }
  const double strength = settings.strength;
  const int settle = std::max(1, settings.settle);
  int written = 0;
  for (int slot : moved) {
    Property &prop = target.p[size_t(slot)];
    const Property original = prop; // read while the new keys are made
    // The value on a hit and at rest, over what the property already does.
    auto hit = [&](double t, size_t index) {
      const double v = original.keyedAt(t);
      switch (settings.move) {
        case 0: return v * (1 + strength);
        case 1: return v;
        case 2: return v - strength;
        default: return v + (index % 2 ? -strength : strength);
      }
    };
    auto rest = [&](double t) {
      const double v = original.keyedAt(t);
      return settings.move == 1 ? std::clamp(v * (1 - strength), 0., 1.) : v;
    };
    auto key = [&](double t, double v, const char *ease) {
      prop.set(t, v);
      for (Key &k : prop.keys)
        if (std::abs(k.frame - t) < 1e-9) { k.ease = ease; k.custom = false; }
      ++written;
    };
    prop.keys.clear();
    // At rest until the first hit, held so it does not creep toward it.
    if (hits.front() >= 1) key(0, rest(0), "hold");
    for (size_t i = 0; i < hits.size(); ++i) {
      const double at = hits[i];
      const double next = i + 1 < hits.size() ? hits[i + 1] : target.duration;
      // Out fast, then easing to rest; held at rest until the next hit.
      key(at, hit(at, i), "easeOut");
      const double settled = std::min(at + settle, next - 1);
      if (settled > at) key(settled, rest(settled), "hold");
    }
    prop.base = original.base;
  }
  return written;
}

QJsonObject Scene::json() const {
  QJsonArray array;
  for (const auto &l : layers) {
    QJsonObject properties;
    for (int i = 0; i < PropertyCount; i++) {
      QJsonArray keys;
      for (const auto &k : l.p[i].keys) {
        QJsonObject j{
            {"time", k.frame}, {"value", k.value}, {"easing", k.ease}};
        if (k.custom)
          j["bezier"] =
              QJsonArray{k.curve[0], k.curve[1], k.curve[2], k.curve[3]};
        if (k.hasNormCurve) j["normCurve"] = QJsonArray{k.normCurve[0], k.normCurve[1]};
        if (k.hasSpatial) {
          j["hasSpatial"] = true;
          j["spatialOut"] = QJsonArray{k.spatialOut.x(), k.spatialOut.y()};
          j["spatialIn"] = QJsonArray{k.spatialIn.x(), k.spatialIn.y()};
        }
        if (k.timing) j["timing"] = true;
        j["absoluteHandles"] = k.absolute;
        j["continuous"] = k.continuous;
        keys.append(j);
      }
      QJsonObject property{{"value", l.p[i].base}, {"keyframes", keys}};
      writeExpression(l.p[i], property);
      properties[propKeys[i]] = property;
    }
    QJsonArray binding3D;
    for (int row = 0; row < 4; ++row)
      for (int col = 0; col < 4; ++col) binding3D.append(l.parentBind3D(row, col));
    QJsonArray textStrokes;
    for (const auto &stroke : l.textStrokes)
      textStrokes.append(QJsonObject{{"width", stroke.width},
                                     {"color", stroke.color.name(QColor::HexArgb)},
                                     {"mode", stroke.mode},
                                     {"join", stroke.join == 1 ? "round" : "miter"}});
    QJsonObject j{{"id", l.id}, {"parentId", l.parentId}, {"groupId", l.groupId},
                  {"groupExpanded", l.groupExpanded},
                  {"threeD", l.threeD}, {"parentBind3D", binding3D},
                  {"parentBind", QJsonArray{l.parentBind.m11(), l.parentBind.m12(), l.parentBind.m21(), l.parentBind.m22(), l.parentBind.dx(), l.parentBind.dy()}},
                  {"name", l.name},
                  {"type", l.type},
                  {"blendMode", l.blendMode},
                  {"labelColor", l.labelColor.isValid() ? l.labelColor.name() : ""},
                  {"trackMatteId", l.trackMatteId},
          {"trackMatteInverted", l.trackMatteInverted},
                  {"shapeKind", l.shapeKind},
                  {"text", l.text},
                  {"fontFamily", l.font},
                  {"fontSize", l.fontSize},
                  {"richHtml", l.richHtml},
                  {"characterAnimation", l.characterAnimation},
                  {"bold", l.bold},
                  {"fontStyle", l.fontStyle},
                  {"fauxBold", l.fauxBold},
                  {"fauxItalic", l.fauxItalic},
                  {"capitals", l.capitals},
                  {"leading", l.leading},
                  {"strokeWidth", l.strokeWidth},
                  {"strokeMode", l.strokeMode},
                  {"shapeStrokeCap", l.shapeStrokeCap},
                  {"textStrokes", textStrokes},
                  {"surfaceEffects", l.surfaceEffects},
                  {"strokeColor", l.strokeColor.name()},
                  {"blur", l.blur},
                  {"appliedEffects", l.appliedEffects},
                  {"hiddenEffects", l.hiddenEffects},
                  {"effectOrder", l.effectOrder},
                  {"mainTransform", QJsonObject{{"anchorX", l.mainAnchorX},
                                                {"anchorY", l.mainAnchorY},
                                                {"x", l.mainX}, {"y", l.mainY},
                                                {"z", l.mainZ},
                                                {"scaleX", l.mainScaleX},
                                                {"scaleY", l.mainScaleY},
                                                {"rotation", l.mainRotation},
                                                {"scaleLocked", l.mainScaleLocked}}},
                   {"shapePoints", l.shapePoints}, {"masks", l.masks},
                   {"shapeClosed", l.shapeClosed}, {"shapeFill", l.shapeFill},
                   {"repeater", QJsonObject{{"enabled", l.repeaterEnabled},
                                                  {"copies", l.repeaterCopies},
                                                  {"offset", l.repeaterOffset},
                                                  {"positionX", l.repeaterPositionX},
                                                  {"positionY", l.repeaterPositionY},
                                                  {"positionZ", l.repeaterPositionZ},
                                                  {"scale", l.repeaterScale},
                                                  {"rotation", l.repeaterRotation},
                                                  {"spiral", l.repeaterSpiral}}},
                   {"contentsExpanded", l.contentsExpanded}, {"pathExpanded", l.pathExpanded},
                   {"extrudeExpanded", l.extrudeExpanded},
                   {"particles", l.particles}, {"particlesExpanded", l.particlesExpanded}, {"particlesOn", l.particlesOn},
                   {"tracks", l.tracks}, {"timeRemapOn", l.timeRemap}, {"frameBlend", l.frameBlend},
                   {"lightKind", l.lightKind}, {"lightFalloff", l.lightFalloff}, {"lightExpanded", l.lightExpanded},
                   {"acceptsLights", l.acceptsLights}, {"surfaceExpanded", l.surfaceExpanded},
                   {"lightShadows", l.lightShadows}, {"castsShadows", l.castsShadows},
                   {"sharesMainView", l.sharesMainView},
                  {"glow", l.glow},
                  {"shadow", l.shadow},
                  {"expanded", l.expanded},
                  {"transformExpanded", l.transformExpanded},
                  {"effectsExpanded", l.effectsExpanded},
                  {"masksExpanded", l.masksExpanded},
                  {"textAnimationExpanded", l.textAnimationExpanded},
                  {"foldedTextAnimations", QJsonArray::fromStringList(l.foldedTextAnimations)},
                  {"separatePosition", l.separatePosition},
                  {"separateScale", l.separateScale},
                  {"separateAnchor", l.separateAnchor},
                  {"separateRotation", l.separateRotation},
                  {"lockScale", l.lockScale},
                  {"scaleRatio", l.scaleRatio},
                  {"motionBlur", l.motionBlur},
                  {"tracking", l.tracking},
                  {"align", l.align},
                  {"paragraphWidth", l.paragraphWidth},
                  {"fillColor", l.color.name(QColor::HexArgb)},
                  {"textColor", l.color.name(QColor::HexArgb)},
                  {"path", l.path},
                  {"startFrame", l.start},
                  {"duration", l.duration},
                  {"sourceDurationMs", l.sourceDurationMs},
                  {"sourceOffsetMs", l.sourceOffsetMs},
                  {"pendingVideoDuration", l.pendingVideoDuration},
                  {"visible", l.visible},
                  {"audioEnabled", l.audioEnabled},
                  {"namedByHand", l.namedByHand},
                  {"lens", QJsonObject{{"focalLength", l.lensFocalLength},
                                       {"dofEnabled", l.dofEnabled},
                                       {"focusDistance", l.dofFocusDistance},
                                       {"fStop", l.dofFStop},
                                       {"blurLevel", l.dofBlurLevel}}},
                  {"properties", properties}};
    // Written only when on, so a project that never used them is unchanged.
    if (l.textStrokeOnly) j["textStrokeOnly"] = true;
    if (l.isolated) j["isolated"] = true;
    if (l.locked) j["locked"] = true;
    if (!l.structureId.isEmpty()) j["structureId"] = l.structureId;
    if (!l.extraCurves.empty()) {
      QJsonArray extras;
      for (size_t n = 0; n < l.extraCurves.size(); ++n)
        extras.append(QJsonObject{{"id", n < size_t(l.extraCurveIds.size())
                                             ? l.extraCurveIds[int(n)]
                                             : QString()},
                                  {"curve", curveJson(l.extraCurves[n])}});
      j["extraCurves"] = extras;
    }
    if (!l.additionalTransforms.empty()) {
      QJsonArray arr;
      for (const auto &at : l.additionalTransforms) {
        arr.append(QJsonObject{{"id", at.id},
                                {"name", at.name},
                                {"expanded", at.expanded},
                                {"separatePosition", at.separatePosition},
                                {"separateScale", at.separateScale},
                                {"separateRotation", at.separateRotation},
                                {"separateAnchor", at.separateAnchor},
                                {"lockScale", at.lockScale}});
      }
      j["additionalTransforms"] = arr;
    }
    if (l.type == "image" && !l.image.isNull()) {
      if (l.image.format()==QImage::Format_RGBX32FPx4 || l.image.format()==QImage::Format_RGBX16FPx4 ||
          l.image.format()==QImage::Format_RGBA32FPx4 || l.image.format()==QImage::Format_RGBA32FPx4_Premultiplied ||
          l.image.format()==QImage::Format_RGBA16FPx4 || l.image.format()==QImage::Format_RGBA16FPx4_Premultiplied) {
        j["floatImage"] = encodeFloatImage(l.image);
      } else {
      QByteArray bytes;
      QBuffer buffer(&bytes);
      buffer.open(QIODevice::WriteOnly);
      l.image.save(&buffer, "PNG");
      j["imageData"] =
          "data:image/png;base64," + QString::fromLatin1(bytes.toBase64());
      }
    }
    array.append(j);
  }
  QJsonObject project{{"format", "wavy-flo"},
          {"version", 1},
          {"composition", QJsonObject{{"width", width},
                                      {"height", height},
                                      {"processingDepth", processingDepth},
                                      {"fps", fps},
                                      {"duration", frames}}},
          {"layers", array},
          {"assets", assets},
          {"pluginSettings", pluginSettings},
          {"background", background.name()},
          {"frame", frame},
          {"selected", selected},
          {"motionBlur", motionBlur},
          {"exposure", exposure},
          {"markers", markersJson(markers)}};
  if (!renderQueue.isEmpty()) project["renderQueue"] = renderQueue;
  if (linearLight) project["linearLight"] = true;
  if (hasPlayRange(*this)) project["playRange"] = QJsonArray{playStart, playEnd};
  return project;
}
bool Scene::load(const QJsonObject &j, QString &error) {
  auto fail = [&](QString s) {
    error = s;
    return false;
  };
  QString format = j["format"].toString();
  // Before Wavy-Flo took its name the same formats were "anjim-native" and "anjim-effect".
  if (format == "anjim-native") format = "wavy-flo";
  if (format == "anjim-effect") format = "wavy-flo-effect";
  if (format != "wavy-flo" && format != "wavy-flo-effect")
    return fail("Unknown project format");
  Scene next;
  next.assets = j["assets"].toArray();
  next.pluginSettings = j["pluginSettings"].toObject();
  next.background = QColor(j["background"].toString("#101014"));
  auto c = j["composition"].toObject();
  next.width = c["width"].toInt();
  next.processingDepth = c["processingDepth"].toInt(8);
  if (next.processingDepth != 8 && next.processingDepth != 16 && next.processingDepth != 32)
    return fail("Invalid processing depth");
  next.height = c["height"].toInt();
  next.fps = c["fps"].toInt();
  next.frames = c["duration"].toInt();
  if (next.width < 16 || next.height < 16 || next.width > 7680 ||
      next.height > 7680 || next.width * next.height > 33000000 ||
      next.fps < 1 || next.fps > 120 || next.frames < 1 || next.frames > 36000)
    return fail("Invalid Main size");
  if (j["layers"].toArray().size() > maxLayersPerComposition)
    return fail(QString("Too many layers: the limit is %1").arg(maxLayersPerComposition));
  for (auto item : j["layers"].toArray()) {
    auto a = item.toObject();
    Layer l;
    l.name = a["name"].toString("Layer");
    l.type = a["type"].toString();
    const QString legacyShapeType = l.type;
    l.shapeKind = a["shapeKind"].toString();
    // Every type the menus can make. "fill" was missing, so a project holding
    // a Fill layer was refused on the way back in -- saved without complaint
    // and unopenable afterwards.
    if (!QStringList{"text", "rect", "circle", "image", "video", "audio", "empty", "camera",
                     "shape", "star", "polygon", "fill", "group", "effect", "particles", "light", "structure"}
             .contains(
            l.type))
      return fail("Unsupported layer type");
    if (a.contains("videoData"))
      return fail("Embedded web video must be re-imported from its original "
                  "media file.");
    l.id = a["id"].toString(l.id);
    l.parentId = a["parentId"].toString();
    l.groupId = a["groupId"].toString();
    l.structureId = a["structureId"].toString();
    l.groupExpanded = a["groupExpanded"].toBool(true);
    l.sharesMainView = a["sharesMainView"].toBool(false);
    l.threeD = a["threeD"].toBool() || l.type == "camera" || l.type == "light";
    auto binding3D = a["parentBind3D"].toArray();
    if (binding3D.size() == 16)
      for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
          l.parentBind3D(row, col) = float(binding3D[row * 4 + col].toDouble());
    auto binding = a["parentBind"].toArray();
    if (binding.size() == 6) l.parentBind = QTransform(binding[0].toDouble(), binding[1].toDouble(), binding[2].toDouble(), binding[3].toDouble(), binding[4].toDouble(), binding[5].toDouble());
    // A project saved before blend modes, or naming one this build does not
    // know, is drawn normally rather than refused.
    l.blendMode = a["blendMode"].toString("normal");
    {
      bool known = false;
      for (const auto &mode : layerBlendModes()) known |= mode.first == l.blendMode;
      if (!known) l.blendMode = "normal";
    }
    l.trackMatteId = a["trackMatteId"].toString("");
    if (a.contains("labelColor") && !a["labelColor"].toString().isEmpty())
      l.labelColor = QColor(a["labelColor"].toString());
    l.trackMatteInverted = a["trackMatteInverted"].toBool(false);
    l.text = a["text"].toString();
    l.font = a["fontFamily"].toString("Segoe UI");
    l.fontSize = a["fontSize"].toDouble(80);
    l.richHtml = a["richHtml"].toString();
    l.characterAnimation = a["characterAnimation"].toObject();
    l.bold = a["bold"].toBool(a["fontWeight"].toString("700") == "700");
    l.fontStyle = a["fontStyle"].toString();
    l.fauxBold = a["fauxBold"].toBool();
    l.fauxItalic = a["fauxItalic"].toBool();
    l.capitals = std::clamp(a["capitals"].toInt(), 0, 3);
    l.leading = a["leading"].toDouble(1.2);
    l.strokeWidth = a["strokeWidth"].toDouble();
    // A project without the key predates alignment choices, when every stroke
    // was centred; the new Outside default is for strokes made from now on.
    l.strokeMode = std::clamp(a["strokeMode"].toInt(2),0,2);
    l.surfaceEffects = a["surfaceEffects"].toObject();
    l.strokeColor = QColor(a["strokeColor"].toString("#000000"));
    if (l.type == "text") {
      const auto savedStrokes = a["textStrokes"].toArray();
      for (int strokeIndex = 0; strokeIndex < std::min(32, int(savedStrokes.size())); ++strokeIndex) {
        const auto saved = savedStrokes[strokeIndex].toObject();
        TextStrokeStyle stroke;
        stroke.width = std::clamp(saved["width"].toDouble(4), 0., 1000.);
        stroke.color = QColor(saved["color"].toString("#ff000000"));
        if (!stroke.color.isValid()) stroke.color = Qt::black;
        stroke.mode = std::clamp(saved["mode"].toInt(2), 0, 2);
        // Projects from before the choice existed were drawn with miter joins.
        stroke.join = saved["join"].toString("miter") == "round" ? 1 : 0;
        l.textStrokes.push_back(stroke);
      }
      // Projects written before the stack existed retain their Character
      // stroke as the first entry.
      if (l.textStrokes.empty() && l.strokeWidth > 0)
        l.textStrokes.push_back({l.strokeWidth, l.strokeColor, l.strokeMode});
      syncLegacyTextStroke(l);
      l.textStrokeOnly = a["textStrokeOnly"].toBool(false);
    }
    l.isolated = a["isolated"].toBool(false);
    l.locked = a["locked"].toBool(false);
    l.blur = a["blur"].toDouble(a["effects"].toObject()["blur"].toDouble());
    l.glow = a["glow"].toDouble(a["effects"].toObject()["glow"].toDouble());
    l.shadow =
        a["shadow"].toDouble(a["effects"].toObject()["shadow"].toDouble());
    l.expanded = a["expanded"].toBool(false);
    l.transformExpanded = a["transformExpanded"].toBool(false);
    l.effectsExpanded = a["effectsExpanded"].toBool(false);
    l.masksExpanded = a["masksExpanded"].toBool(true);
    l.textAnimationExpanded = a["textAnimationExpanded"].toBool(true);
    l.foldedTextAnimations.clear();
    for (const auto &v : a["foldedTextAnimations"].toArray()) l.foldedTextAnimations << v.toString();
    l.separatePosition = a["separatePosition"].toBool();
    l.separateScale = a["separateScale"].toBool();
    l.separateAnchor = a["separateAnchor"].toBool();
    l.separateRotation = a["separateRotation"].toBool();
    l.appliedEffects = a["appliedEffects"].toObject();
    l.hiddenEffects = a["hiddenEffects"].toObject();
    l.effectOrder = a["effectOrder"].toArray();
    {
      const auto main = a["mainTransform"].toObject();
      l.mainAnchorX = main["anchorX"].toDouble();
      l.mainAnchorY = main["anchorY"].toDouble();
      l.mainX = main["x"].toDouble();
      l.mainY = main["y"].toDouble();
      l.mainZ = main["z"].toDouble();
      l.mainScaleX = main["scaleX"].toDouble(1);
      l.mainScaleY = main["scaleY"].toDouble(1);
      l.mainRotation = main["rotation"].toDouble();
      // The main transform follows the timeline's Scale default: dimensions
      // stay linked unless a saved project explicitly chose otherwise.
      l.mainScaleLocked = main["scaleLocked"].toBool(true);
    }
    l.shapePoints = a["shapePoints"].toArray();
    l.masks = a["masks"].toArray();
    l.shapeStrokeCap = std::clamp(a["shapeStrokeCap"].toInt(0), 0, 1);
    l.shapeClosed = a["shapeClosed"].toBool(true);
    l.shapeFill = a["shapeFill"].toBool(true);
    {
      const auto repeater = a["repeater"].toObject();
      l.repeaterEnabled = repeater["enabled"].toBool(false);
      l.repeaterCopies = std::clamp(repeater["copies"].toInt(3), 1, 1000);
      l.repeaterOffset = std::clamp(repeater["offset"].toDouble(), -1000., 1000.);
      l.repeaterPositionX = std::clamp(repeater["positionX"].toDouble(100), -100000., 100000.);
      l.repeaterPositionY = std::clamp(repeater["positionY"].toDouble(), -100000., 100000.);
      l.repeaterPositionZ = std::clamp(repeater["positionZ"].toDouble(), -100000., 100000.);
      l.repeaterScale = std::clamp(repeater["scale"].toDouble(100), .01, 10000.);
      l.repeaterRotation = std::clamp(repeater["rotation"].toDouble(), -36000., 36000.);
      l.repeaterSpiral = repeater["spiral"].toBool(false);
    }
    l.contentsExpanded = a["contentsExpanded"].toBool(true);
    l.extrudeExpanded = a["extrudeExpanded"].toBool(true);
    l.particles = a["particles"].toObject();
    l.particlesExpanded = a["particlesExpanded"].toBool(true);
    l.particlesOn = a["particlesOn"].toBool(false);
    l.tracks = a["tracks"].toArray();
    l.timeRemap = a["timeRemapOn"].toBool(false);
    l.frameBlend = std::clamp(a["frameBlend"].toInt(0), 0, 1);
    l.lightKind = std::clamp(a["lightKind"].toInt(1), 0, 3);
    l.lightFalloff = std::clamp(a["lightFalloff"].toInt(0), 0, 2);
    l.lightExpanded = a["lightExpanded"].toBool(true);
    l.acceptsLights = a["acceptsLights"].toBool(true);
    l.surfaceExpanded = a["surfaceExpanded"].toBool(true);
    l.lightShadows = a["lightShadows"].toBool(false);
    l.castsShadows = a["castsShadows"].toBool(false);
    l.pathExpanded = a["pathExpanded"].toBool(true);
    if (l.type == "circle") l.p[ShapeWidth].base = l.p[ShapeHeight].base = 160;
    if (l.shapePoints.size() > 10000 || l.masks.size() > 100) return fail("Too many vector points or masks");
    l.lockScale = a["lockScale"].toBool(true);
    l.scaleRatio = a["scaleRatio"].toDouble(1);
    l.motionBlur = a["motionBlur"].toBool(false);
    l.tracking = a["tracking"].toDouble();
    l.align = a["align"].toInt(1);
    l.paragraphWidth = std::clamp(a["paragraphWidth"].toDouble(), 0., 30000.);
    l.color = QColor(
        a[l.type == "text" ? "textColor" : "fillColor"].toString("#ffffff"));
    if (format == "wavy-flo-effect") {
      QString alignment = a["textAlign"].toString("center");
      l.align = alignment == "left" ? 0 : alignment == "right" ? 2 : 1;
      l.leading = a["lineHeight"].toDouble(l.leading);
      auto styles = a["characterStyles"].toArray();
      if (!styles.isEmpty() && l.type == "text") {
        std::unique_ptr<QTextDocument> doc(textDocument(l));
        for (int i = 0; i < styles.size() && i < l.text.size(); i++) {
          auto style = styles[i].toObject();
          if (style.isEmpty())
            continue;
          QTextCursor cursor(doc.get());
          cursor.setPosition(i);
          cursor.setPosition(i + 1, QTextCursor::KeepAnchor);
          QTextCharFormat f;
          if (style.contains("fontFamily"))
            f.setFontFamilies({style["fontFamily"].toString()});
          if (style.contains("fontSize"))
            f.setFontPointSize(style["fontSize"].toDouble() * .75);
          if (style.contains("fontWeight"))
            f.setFontWeight(style["fontWeight"].toVariant().toInt() >= 600
                                ? QFont::Bold
                                : QFont::Normal);
          if (style.contains("textColor"))
            f.setForeground(QColor(style["textColor"].toString()));
          if (style.contains("tracking")) {
            f.setFontLetterSpacingType(QFont::AbsoluteSpacing);
            f.setFontLetterSpacing(style["tracking"].toDouble());
          }
          if (style.contains("strokeWidth") || style.contains("strokeColor"))
            f.setTextOutline(QPen(
                QColor(style["strokeColor"].toString(l.strokeColor.name())),
                style["strokeWidth"].toDouble(l.strokeWidth)));
          cursor.mergeCharFormat(f);
        }
        l.richHtml = doc->toHtml();
      }
    }
    l.path = a["path"].toString();
    l.start = a["startFrame"].toDouble();
    l.duration = a["duration"].toDouble(next.frames);
    l.sourceDurationMs = std::max(0., a["sourceDurationMs"].toDouble());
    l.sourceOffsetMs = std::max(0., a["sourceOffsetMs"].toDouble());
    l.pendingVideoDuration = a["pendingVideoDuration"].toBool();
    l.visible = a["visible"].toBool(true);
    l.audioEnabled = a["audioEnabled"].toBool(true);
    // Older projects have no such flag. A name that differs from what the
    // layer says was chosen by somebody, so it is theirs to keep.
    l.namedByHand = a["namedByHand"].toBool(l.type == "text" &&
                                            !l.name.isEmpty() &&
                                            l.name != nameFromText(l.text));
    if (!std::isfinite(l.start) || !std::isfinite(l.duration) ||
        l.duration < 1 || l.fontSize < 1 || l.fontSize > 2000)
      return fail("Invalid layer values");
    if (l.type == "image") {
      if (a.contains("floatImage")) l.image=decodeFloatImage(a["floatImage"].toObject());
      else if (!a["imageData"].toString().isEmpty()) {
        QString data = a["imageData"].toString();
        l.image.loadFromData(
            QByteArray::fromBase64(data.mid(data.indexOf(',') + 1).toLatin1()));
        l.image = imageInWorkingSpace(l.image);
      } else if (!a["path"].toString().isEmpty()) {
        // Qt's readers, then Windows' (TIFF, WebP, HEIC...), then Targa.
        l.image = imageInWorkingSpace(readImageFile(a["path"].toString()));
      }
      // Not a reason to lose the whole project: the layer stays, marked,
      // with its path, and reads its picture again the next time.
      l.pictureMissing = l.image.isNull();
    }
    auto props = a["properties"].toObject();
    for (int i = 0; i < PropertyCount; i++) {
      auto p = props[propKeys[i]].toObject();
      l.p[i].base = p["value"].toDouble(l.p[i].base);
      if (!readExpression(p, l.p[i])) return fail("An expression is too long");
      if (p["keyframes"].toArray().size() > 5000)
        return fail("Too many keyframes");
      for (auto kv : p["keyframes"].toArray()) {
        auto k = kv.toObject();
        Key key;
        key.frame = k["time"].toDouble(-1);
        key.value = k["value"].toDouble();
        key.ease = k["easing"].toString("linear");
        key.absolute = k["absoluteHandles"].toBool();
        key.continuous = k["continuous"].toBool(true);
        key.hasSpatial = k["hasSpatial"].toBool(false);
        if (key.hasSpatial) {
          const auto so = k["spatialOut"].toArray();
          const auto si = k["spatialIn"].toArray();
          if (so.size() == 2) key.spatialOut = QPointF(so[0].toDouble(), so[1].toDouble());
          if (si.size() == 2) key.spatialIn = QPointF(si[0].toDouble(), si[1].toDouble());
        }
        if (const auto nc = k["normCurve"].toArray(); nc.size() == 2) {
          key.normCurve = {nc[0].toDouble(), nc[1].toDouble()};
          key.hasNormCurve = true;
        }
        key.timing = k["timing"].toBool(false);
        // Trimming retains keys beyond the out point so extending restores them.
        if (!std::isfinite(key.frame) || key.frame < 0)
          return fail("Invalid keyframe time");
        auto curve = k["bezier"].toArray();
        if (curve.size() == 4) {
          key.custom = true;
          for (int n = 0; n < 4; n++)
            key.curve[n] = curve[n].toDouble();
        }
        l.p[i].keys.push_back(key);
      }
      std::sort(l.p[i].keys.begin(), l.p[i].keys.end(),
                [](const Key &a, const Key &b) { return a.frame < b.frame; });
      for (size_t n = 1; n < l.p[i].keys.size(); n++)
        if (l.p[i].keys[n].frame == l.p[i].keys[n - 1].frame)
          return fail("Duplicate keyframe");
    }
    for (auto value : a["extraCurves"].toArray()) {
      const auto entry = value.toObject();
      const QString id = entry["id"].toString();
      if (id.isEmpty() || l.extraCurveIds.contains(id)) continue;
      if (l.extraCurves.size() >= 256) return fail("Too many animated effect parameters");
      Property curve;
      if (!curveFromJson(entry["curve"].toObject(), curve))
        return fail("Invalid animated effect parameter");
      l.extraCurves.push_back(curve);
      l.extraCurveIds << id;
    }
    // The main transform used to carry an opacity of its own. A layer has one
    // opacity now, the Transform's, so a saved one is folded into it (after
    // the properties are read, or they would overwrite it): it looks as it did.
    if (const double faded = std::clamp(a["mainTransform"].toObject()["opacity"].toDouble(1), 0., 1.); faded < 1) {
      l.p[5].base *= faded;
      for (auto &key : l.p[5].keys) key.value *= faded;
    }
    for (auto value : a["additionalTransforms"].toArray()) {
      const auto entry = value.toObject();
      AdditionalTransform at;
      at.id = entry["id"].toString();
      if (at.id.isEmpty()) continue;
      at.name = entry["name"].toString();
      at.expanded = entry["expanded"].toBool(false);
      at.separatePosition = entry["separatePosition"].toBool(false);
      at.separateScale = entry["separateScale"].toBool(false);
      at.separateRotation = entry["separateRotation"].toBool(false);
      at.separateAnchor = entry["separateAnchor"].toBool(false);
      at.lockScale = entry["lockScale"].toBool(true);
      layerRemoveExtra(l, "Xform/" + at.id + "/Opacity");
      at.resolveProps(l);
      l.additionalTransforms.push_back(at);
    }
    // Projects made before Shape Layer stored every generator as a different
    // layer type. Convert them at the boundary; everything after loading sees
    // one stable shape type and the pixels remain driven by the same fields.
    if (QStringList{"rect", "circle", "polygon", "star"}.contains(legacyShapeType)) {
      l.type = "shape";
      l.shapeKind = legacyShapeType;
    } else if (l.type == "shape") {
      if (!QStringList{"path", "rect", "circle", "polygon", "star"}.contains(l.shapeKind))
        l.shapeKind = "path";
    }
    {
      // After the properties, because a project written before the Lens tab
      // existed has no focus distance to restore, and the honest stand-in for
      // one is the distance Zoom already frames -- which leaves an old camera
      // focused on its own composition rather than opening wide of it the
      // first time somebody ticks Depth of Field.
      const auto lens = a["lens"].toObject();
      l.lensFocalLength = std::clamp(lens["focalLength"].toDouble(50), 1., 2000.);
      l.dofEnabled = lens["dofEnabled"].toBool(false);
      l.dofFocusDistance = lens["focusDistance"].toDouble(
          l.type == "camera" ? std::max(1., l.p[CameraZoom].base) : 1000.);
      l.dofFStop = std::clamp(lens["fStop"].toDouble(2.8), .5, 64.);
      l.dofBlurLevel = std::clamp(lens["blurLevel"].toDouble(100), 0., 200.);
    }
    // Smooth Strength, from before it went: drawn into In's and Out's keys.
    settleSmoothStrength(l);
    next.layers.push_back(l);
  }
  next.selected = next.layers.empty() ? -1 : 0;
  next.frame = std::clamp(j["frame"].toDouble(), 0., double(next.frames - 1));
  next.motionBlur = j["motionBlur"].toBool();
  next.exposure = j["exposure"].toDouble();
  if (!readMarkers(j["markers"].toArray(), next.frames, next.markers)) return fail("Too many markers");
  next.renderQueue = j["renderQueue"].toArray();
  next.linearLight = j["linearLight"].toBool(false);
  if (const QJsonArray range = j["playRange"].toArray(); range.size() == 2) {
    next.playStart = std::clamp(range[0].toInt(-1), -1, next.frames - 1);
    next.playEnd = std::clamp(range[1].toInt(-1), -1, next.frames - 1);
    if (!hasPlayRange(next)) next.playStart = next.playEnd = -1;
  }
  tidyStructures(next);
  *this = std::move(next);
  return true;
}

bool tidyStructures(Scene &s, std::vector<int> *newIndexOf) {
  if (newIndexOf) newIndexOf->clear();
  const int count = int(s.layers.size());
  bool any = false;
  for (const Layer &l : s.layers)
    if (layerIsStructure(l) || !l.structureId.isEmpty()) {
      any = true;
      break;
    }
  if (!any) return false;
  bool changed = false;
  QHash<QString, int> structureAt;
  for (int i = 0; i < count; ++i)
    if (layerIsStructure(s.layers[size_t(i)]) && !s.layers[size_t(i)].id.isEmpty())
      structureAt.insert(s.layers[size_t(i)].id, i);
  // Memberships that cannot stand.
  for (int i = 0; i < count; ++i) {
    Layer &l = s.layers[size_t(i)];
    if (l.structureId.isEmpty()) continue;
    const auto owner = structureAt.constFind(l.structureId);
    if (owner == structureAt.cend() || *owner == i || s.layers[size_t(*owner)].groupId != l.groupId) {
      l.structureId.clear();
      changed = true;
    }
  }
  // A structure inside itself, however far down, comes out at the top.
  for (int i = 0; i < count; ++i) {
    if (!layerIsStructure(s.layers[size_t(i)])) continue;
    QSet<QString> seen{s.layers[size_t(i)].id};
    QString up = s.layers[size_t(i)].structureId;
    while (!up.isEmpty()) {
      if (seen.contains(up)) {
        s.layers[size_t(i)].structureId.clear();
        changed = true;
        break;
      }
      seen.insert(up);
      up = s.layers[size_t(structureAt.value(up))].structureId;
    }
  }
  // Each structure's members straight after it, depth first; everything else
  // keeps its order.
  std::vector<std::vector<int>> members(static_cast<size_t>(count));
  std::vector<int> roots;
  for (int i = 0; i < count; ++i) {
    const QString &owner = s.layers[size_t(i)].structureId;
    if (owner.isEmpty()) roots.push_back(i);
    else members[size_t(structureAt.value(owner))].push_back(i);
  }
  std::vector<int> order;
  order.reserve(size_t(count));
  std::function<void(int)> visit = [&](int i) {
    order.push_back(i);
    for (int member : members[size_t(i)]) visit(member);
  };
  for (int root : roots) visit(root);
  bool moved = false;
  for (int i = 0; i < count; ++i) moved |= order[size_t(i)] != i;
  if (moved) {
    std::vector<Layer> placed;
    placed.reserve(size_t(count));
    for (int i : order) placed.push_back(std::move(s.layers[size_t(i)]));
    s.layers = std::move(placed);
    if (newIndexOf) {
      newIndexOf->assign(size_t(count), -1);
      for (int at = 0; at < count; ++at) (*newIndexOf)[size_t(order[size_t(at)])] = at;
    }
    if (s.selected >= 0 && s.selected < count) {
      for (int at = 0; at < count; ++at)
        if (order[size_t(at)] == s.selected) {
          s.selected = at;
          break;
        }
    }
    changed = true;
  }
  // Spans, innermost first: walking the list backwards meets a structure
  // after everything inside it.
  for (int i = count - 1; i >= 0; --i) {
    Layer &structure = s.layers[size_t(i)];
    if (!layerIsStructure(structure)) continue;
    double first = std::numeric_limits<double>::max(), last = std::numeric_limits<double>::lowest();
    for (int j = i + 1; j < count && !s.layers[size_t(j)].structureId.isEmpty(); ++j) {
      const Layer &member = s.layers[size_t(j)];
      if (member.structureId != structure.id) {
        // Deeper down, or past the end of this one.
        bool inside = false;
        for (QString up = member.structureId; !up.isEmpty(); up = s.layers[size_t(structureAt.value(up))].structureId)
          if (up == structure.id) {
            inside = true;
            break;
          }
        if (!inside) break;
      }
      first = std::min(first, member.start);
      last = std::max(last, member.start + member.duration);
    }
    if (first > last) continue; // nothing inside: it keeps the span it has
    if (structure.start != first || structure.duration != last - first) {
      structure.start = first;
      structure.duration = last - first;
      changed = true;
    }
  }
  return changed;
}

std::vector<int> structureContents(const Scene &s, int index) {
  std::vector<int> inside;
  if (index < 0 || index >= int(s.layers.size()) || !layerIsStructure(s.layers[size_t(index)])) return inside;
  QSet<QString> owners{s.layers[size_t(index)].id};
  for (bool more = true; more;) {
    more = false;
    for (const Layer &l : s.layers)
      if (layerIsStructure(l) && !owners.contains(l.id) && owners.contains(l.structureId)) {
        owners.insert(l.id);
        more = true;
      }
  }
  for (int i = 0; i < int(s.layers.size()); ++i)
    if (i != index && owners.contains(s.layers[size_t(i)].structureId)) inside.push_back(i);
  return inside;
}

int structureDepth(const Scene &s, int index) {
  if (index < 0 || index >= int(s.layers.size())) return 0;
  int depth = 0;
  QString up = s.layers[size_t(index)].structureId;
  while (!up.isEmpty() && depth <= int(s.layers.size())) {
    const auto found = std::find_if(s.layers.begin(), s.layers.end(), [&](const Layer &l) { return l.id == up; });
    if (found == s.layers.end()) break;
    ++depth;
    up = found->structureId;
  }
  return depth;
}
void clearTextBackgrounds(QTextDocument *doc) {
  for (auto block = doc->begin(); block.isValid(); block = block.next()) {
    QTextCursor cursor(block);
    auto bf = block.blockFormat();
    if (bf.hasProperty(QTextFormat::BackgroundBrush)) {
      bf.clearBackground();
      cursor.setBlockFormat(bf);
    }
    auto cf = block.charFormat();
    if (cf.hasProperty(QTextFormat::BackgroundBrush)) {
      cf.clearBackground();
      cursor.setBlockCharFormat(cf);
    }
    std::vector<std::pair<QPair<int, int>, QTextCharFormat>> changes;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
      auto fragment = it.fragment();
      auto format = fragment.charFormat();
      if (fragment.isValid() && format.hasProperty(QTextFormat::BackgroundBrush)) {
        format.clearBackground();
        changes.push_back({{fragment.position(), fragment.length()}, format});
      }
    }
    for (const auto &change : changes) {
      cursor.setPosition(change.first.first);
      cursor.setPosition(change.first.first + change.first.second, QTextCursor::KeepAnchor);
      cursor.setCharFormat(change.second);
    }
  }
  std::function<void(QTextFrame *)> clearFrame = [&](QTextFrame *frame) {
    auto format = frame->frameFormat();
    if (format.hasProperty(QTextFormat::BackgroundBrush)) {
      format.clearBackground();
      frame->setFrameFormat(format);
    }
    for (auto *child : frame->childFrames()) clearFrame(child);
  };
  clearFrame(doc->rootFrame());
}
double textLeadingOffset(const Layer &l, const QTextDocument &doc) {
  if (l.type != "text") return 0;
  // Typography -- and CSS -- split the extra evenly above and below the line.
  // That centres a single line and leaves the distance between several of them
  // untouched, because every line moves by the same amount.
  return std::max(0., QFontMetricsF(doc.defaultFont()).lineSpacing() * (l.leading - 1) / 2.);
}
std::vector<TextStrokeStyle> layerTextStrokes(const Layer &l) {
  if (l.type != "text") return {};
  if (!l.textStrokes.empty()) return l.textStrokes;
  if (l.strokeWidth > 0) return {{l.strokeWidth, l.strokeColor, l.strokeMode}};
  return {};
}
void syncLegacyTextStroke(Layer &l) {
  if (l.type != "text") return;
  if (l.textStrokes.empty()) {
    l.strokeWidth = 0;
    return;
  }
  l.strokeWidth = l.textStrokes.front().width;
  l.strokeColor = l.textStrokes.front().color;
  l.strokeMode = std::clamp(l.textStrokes.front().mode, 0, 2);
}
double textStrokeOutwardReach(const Layer &l) {
  double reach = 0;
  for (const auto &stroke : layerTextStrokes(l)) {
    const double width = std::max(0., stroke.width);
    if (stroke.mode == 1) reach += width;
    else if (stroke.mode == 2) reach += width / 2;
  }
  return reach;
}
QFont::Capitalization textCapitalization(int capitals) {
  return capitals == 1 ? QFont::AllUppercase : capitals == 2 ? QFont::SmallCaps
                       : capitals == 3 ? QFont::AllLowercase : QFont::MixedCase;
}
QTextDocument *textDocument(const Layer &l) {
  auto *doc = new QTextDocument;
  doc->setDocumentMargin(0);
  QFont f(l.font);
  f.setPixelSize(qRound(l.fontSize));
  // The face named, by what sets it apart -- its weight and its slant -- so a
  // text formatted letter by letter keeps it too; else Regular or Bold.
  if (!l.fontStyle.isEmpty() && QFontDatabase::styles(l.font).contains(l.fontStyle)) {
    f.setWeight(QFont::Weight(QFontDatabase::weight(l.font, l.fontStyle)));
    f.setItalic(QFontDatabase::italic(l.font, l.fontStyle));
  } else {
    f.setBold(l.bold);
  }
  f.setCapitalization(textCapitalization(l.capitals));
  f.setLetterSpacing(QFont::AbsoluteSpacing, l.tracking);
  doc->setDefaultFont(f);
  QTextOption opt;
  opt.setWrapMode(l.paragraphWidth > 0 ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
  opt.setAlignment(l.align == 0   ? Qt::AlignLeft
                   : l.align == 2 ? Qt::AlignRight
                   : l.align == 3 ? Qt::AlignJustify
                                  : Qt::AlignHCenter);
  doc->setDefaultTextOption(opt);
  if (!l.richHtml.isEmpty())
    doc->setHtml(l.richHtml);
  else {
    doc->setPlainText(l.text);
    QTextCursor c(doc);
    c.select(QTextCursor::Document);
    QTextCharFormat fmt;
    fmt.setFont(f);
    fmt.setForeground(l.color);
    c.mergeCharFormat(fmt);
    QTextBlockFormat block;
    block.setAlignment(opt.alignment());
    block.setLineHeight(l.leading * 100, QTextBlockFormat::ProportionalHeight);
    c.mergeBlockFormat(block);
  }
  QTextCursor paragraphCursor(doc);
  paragraphCursor.select(QTextCursor::Document);
  QTextBlockFormat paragraphFormat;
  paragraphFormat.setAlignment(opt.alignment());
  paragraphFormat.setLineHeight(l.leading * 100, QTextBlockFormat::ProportionalHeight);
  paragraphCursor.mergeBlockFormat(paragraphFormat);
  clearTextBackgrounds(doc);
  if (l.paragraphWidth > 0) doc->setTextWidth(l.paragraphWidth);
  else {
    doc->adjustSize();
    doc->setTextWidth(doc->idealWidth());
  }
  return doc;
}
namespace {
QMutex &textMeasurementGuard() { static QMutex guard; return guard; }
QCache<QByteArray, QSizeF> &textMeasurements() {
  static QCache<QByteArray, QSizeF> cache{512 * 1024};
  return cache;
}
QCache<QByteArray, QRectF> &textInkBoundsCache() {
  static QCache<QByteArray, QRectF> cache{512 * 1024};
  return cache;
}
QByteArray textDocumentKey(const Layer &l) {
  QByteArray key;
  QDataStream stream(&key, QIODevice::WriteOnly);
  stream << l.text << l.richHtml << l.font << qRound(l.fontSize) << l.bold << l.fontStyle << l.fauxBold << l.fauxItalic
         << l.capitals << l.tracking << l.leading << l.align << l.paragraphWidth << l.color;
  return key;
}
// The handwriting mask, kept: what the pen has uncovered at one moment is a
// sweep cut against every letter it is part way through, boolean work on paths,
// and a frame asked for it once per band and per motion-blur sample -- a 3D
// subtitle being written cost 30 ms a frame for one mask made twenty times.
// Kept by the letters (the document) and by where the pen is.
QPainterPath cachedHandwritingMask(QTextDocument &doc, const Layer &l, double time, double reach) {
  const double progress = handwritingProgress(l, time);
  const double erased = std::clamp(characterProgress(l, 7, time, 0, 0), 0., 1.);
  if (progress <= 0 || erased >= progress) return {};
  const double nib = std::clamp(l.characterAnimation.value("writeNib").toDouble(115), 20., 400.) / 100.;
  QByteArray key = textDocumentKey(l);
  key.append(reinterpret_cast<const char *>(&progress), sizeof progress);
  key.append(reinterpret_cast<const char *>(&erased), sizeof erased);
  key.append(reinterpret_cast<const char *>(&nib), sizeof nib);
  key.append(reinterpret_cast<const char *>(&reach), sizeof reach);
  static QMutex guard;
  static QCache<QByteArray, QPainterPath> kept(48);
  {
    QMutexLocker lock(&guard);
    if (const QPainterPath *hit = kept.object(key)) return *hit;
  }
  // Grown, it starts from the plain mask, kept too: the face asks for one and
  // the extruded sides for the other in the same frame, and the pen's sweep
  // is most of the work.
  QPainterPath mask = reach > 0 ? cachedHandwritingMask(doc, l, time, 0) : handwritingReveal(characterGlyphs(doc, l), progress, erased, nib);
  if (reach > 0 && !mask.isEmpty()) {
    // Grown all round by what the strokes reach outside the letters: what the
    // pen has written carries its Outside stroke with it. A union of the two
    // as paths cost seventy milliseconds a frame while the pen moved. Every
    // outline turned the same way round instead, and filled by winding, the
    // two simply add up to their union: no place can wind to nothing. The
    // letters' counters come out filled, which shows nothing -- there is
    // nothing drawn in them but the strokes that belong there.
    QPainterPathStroker grow;
    grow.setWidth(reach * 2);
    grow.setJoinStyle(Qt::RoundJoin);
    grow.setCapStyle(Qt::RoundCap);
    const QPainterPath band = grow.createStroke(mask);
    QPainterPath grown;
    grown.setFillRule(Qt::WindingFill);
    auto addTurnedOneWay = [&grown](const QPainterPath &path) {
      for (QPolygonF outline : path.toSubpathPolygons()) {
        if (outline.size() < 3) continue;
        double area = 0;
        for (int i = 0, j = int(outline.size()) - 1; i < outline.size(); j = i++)
          area += outline[j].x() * outline[i].y() - outline[i].x() * outline[j].y();
        if (area < 0) std::reverse(outline.begin(), outline.end());
        grown.addPolygon(outline);
        grown.closeSubpath();
      }
    };
    addTurnedOneWay(mask);
    addTurnedOneWay(band);
    mask = grown;
  }
  // Kept, it is clipped to by every strip and band at once.
  readyPathForThreads(mask);
  QMutexLocker lock(&guard);
  kept.insert(key, new QPainterPath(mask));
  return mask;
}
// Held per rendering thread, so the limit is what one thread may keep rather
// than what the whole program may. Enough for every text of a long music
// video: at 64, a scene of ninety titles emptied the cache several times a
// frame, and every title was laid out again -- its letters with it -- each
// time it was drawn.
int textDocumentCacheLimit() { return 256; }
std::atomic<quint64> textMeasurementsBuilt{0};
// Raised whenever the caches are dropped. Each thread's own cache compares it
// against the generation it built at, which is how a clear on one thread
// reaches the caches held by all the others.
std::atomic<unsigned> textDocumentGeneration{0};
}
std::shared_ptr<QTextDocument> cachedTextDocument(const Layer &l) {
  // A QTextDocument lays itself out the first time anything asks it anything,
  // so reading one mutates it. Rendering reads them from a pool of threads,
  // and one document shared between them meant several threads shaping the
  // same layout at once -- a data race inside Qt's text engine, which faults
  // reading a half-built layout. It showed up on a font that had just changed,
  // because a new font means a new document with no layout yet and every
  // worker racing to be the one that builds it. So each thread keeps its own.
  struct Kept { std::shared_ptr<QTextDocument> doc; quint64 used = 0; };
  static thread_local QHash<QByteArray, Kept> cache;
  static thread_local unsigned generation = 0;
  static thread_local quint64 clock = 0;
  const unsigned current = textDocumentGeneration.load(std::memory_order_relaxed);
  if (generation != current) {
    cache.clear();
    generation = current;
  }
  const QByteArray key = textDocumentKey(l);
  auto it = cache.find(key);
  if (it != cache.end()) {
    it->used = ++clock;
    return it->doc;
  }
  std::shared_ptr<QTextDocument> doc(textDocument(l));
  // Full, the one drawn least lately goes -- not all of them, which on a
  // scene with more texts than the limit rebuilt every one of them in turn.
  if (cache.size() >= textDocumentCacheLimit()) {
    auto oldest = cache.begin();
    for (auto entry = cache.begin(); entry != cache.end(); ++entry)
      if (entry->used < oldest->used) oldest = entry;
    cache.erase(oldest);
  }
  cache.insert(key, {doc, ++clock});
  return doc;
}
void clearTextDocumentCache() {
  textDocumentGeneration.fetch_add(1, std::memory_order_relaxed);
  QMutexLocker lock(&textMeasurementGuard());
  textMeasurements().clear();
  textInkBoundsCache().clear();
}
namespace {
QSizeF measuredTextSize(const Layer &l) {
  const QByteArray key = textDocumentKey(l);
  {
    QMutexLocker lock(&textMeasurementGuard());
    if (const auto *size = textMeasurements().object(key)) return *size;
  }
  auto doc = cachedTextDocument(l);
  const QSizeF size = doc->size();
  ++textMeasurementsBuilt;
  const qsizetype cost = key.size() + sizeof(QSizeF);
  if (cost <= 512 * 1024) {
    QMutexLocker lock(&textMeasurementGuard());
    textMeasurements().insert(key, new QSizeF(size), int(cost));
  }
  return size;
}
// Where the letters actually sit, in layer coordinates: every glyph outline
// united, then placed the way drawLayer places the document. Cached beside the
// size measurement, because the selection box asks for it on every frame it is
// drawn and building it walks the whole document.
QRectF measuredTextInk(const Layer &l) {
  if (l.type != "text") return {};
  const QByteArray key = textDocumentKey(l);
  {
    QMutexLocker lock(&textMeasurementGuard());
    if (const auto *hit = textInkBoundsCache().object(key)) return *hit;
  }
  auto doc = cachedTextDocument(l);
  QRectF ink;
  for (const auto &glyph : characterGlyphs(*doc, l)) {
    const QRectF outline = glyph.path.boundingRect();
    ink = ink.isNull() ? outline : ink.united(outline);
  }
  if (!ink.isNull()) {
    const QSizeF size = doc->size();
    ink.translate(-size.width() / 2, -size.height() / 2 + textLeadingOffset(l, *doc));
  }
  const qsizetype cost = key.size() + sizeof(QRectF);
  if (cost <= 512 * 1024) {
    QMutexLocker lock(&textMeasurementGuard());
    textInkBoundsCache().insert(key, new QRectF(ink), int(std::max<qsizetype>(1, cost)));
  }
  return ink;
}
}
quint64 textLayoutMeasurements() { return textMeasurementsBuilt.load(); }
// The box a still text's Character strokes fill, Miter corners out to their
// tips, in layer coordinates; null when no stroke has Miter corners outside the
// letters (the layer's own box holds every other stroke).
QRectF mitredTextStrokeBox(const Layer &l) {
  if (l.type != "text") return {};
  double outward = 0;
  bool mitred = false;
  for (const auto &stroke : textStrokeStack(l)) {
    const double width = std::max(0., stroke.width);
    outward += stroke.mode == 1 ? width : stroke.mode == 2 ? width / 2 : 0;
    mitred |= stroke.join == 0 && stroke.mode != 0 && width > 0;
  }
  if (!mitred || outward <= 0) return {};
  // Rounded up, as the raster's own margin is, so a width dragged a step at a
  // time keeps the same raster -- and the distance field already worked out
  // for it -- instead of a new one, a few pixels larger, every step.
  outward = std::ceil(outward / 16.) * 16.;
  QByteArray key = textDocumentKey(l);
  key += QByteArray::number(outward, 'g', 17);
  static QMutex guard;
  static QCache<QByteArray, QRectF> kept(256);
  {
    QMutexLocker lock(&guard);
    if (const auto *hit = kept.object(key)) return *hit;
  }
  auto doc = cachedTextDocument(l);
  QRectF box;
  for (const auto &glyph : characterGlyphs(*doc, l)) {
    const QRectF one = mitredStrokeBox(glyph.path, outward);
    box = box.isNull() ? one : box.united(one);
  }
  if (!box.isNull()) {
    const QSizeF size = doc->size();
    box.translate(-size.width() / 2, -size.height() / 2 + textLeadingOffset(l, *doc));
  }
  QMutexLocker lock(&guard);
  kept.insert(key, new QRectF(box));
  return box;
}
QPainterPath vectorPath(const QJsonArray &points, bool closed) {
  QPainterPath path;
  auto point = [](const QJsonObject &n, QString key) {
    const auto a = n[key].toArray();
    return QPointF(a.size()>0?a.at(0).toDouble():0., a.size()>1?a.at(1).toDouble():0.);
  };
  QJsonArray contour;
  const auto appendContour = [&] {
    if (contour.isEmpty()) return;
    path.moveTo(point(contour[0].toObject(), "p"));
    const int count = int(contour.size());
    for (int i = 1; i < count + (closed ? 1 : 0); ++i) {
      auto previous = contour[(i-1)%count].toObject(), next = contour[i%count].toObject();
      const auto a = point(previous,"p"), b = point(next,"p");
      const QPointF out = point(previous,"out"), in = point(next,"in");
      if (out.isNull() && in.isNull()) path.lineTo(b);
      else path.cubicTo(a + out, b + in, b);
    }
    if (closed) path.closeSubpath();
  };
  // One Shape Layer can contain several Pen paths. A `break` flag starts a
  // fresh subpath while leaving old project files, which contain no flag,
  // completely unchanged.
  for (const auto &value : points) {
    const auto node = value.toObject();
    if (node["break"].toBool(false)) {
      appendContour();
      contour = {};
    }
    contour.append(node);
  }
  appendContour();
  return path;
}
QString shapeGeneratorKind(const Layer &l) {
  if (l.type == "rect" || l.type == "circle" || l.type == "polygon" || l.type == "star")
    return l.type;
  return l.type == "shape" ? l.shapeKind : QString();
}
double posterizedTime(const Layer &l, const Scene &s, double t) {
  static const QString id = "Posterize Time";
  if (!l.appliedEffects.value(id).toBool() || l.hiddenEffects.value(id).toBool()) return t;
  double rate = 12;
  if (const PackEffect *effect = packEffect(id)) {
    const QVector<float> values = packEffectValues(l, *effect, t);
    if (!values.isEmpty()) rate = values[0];
  }
  rate = std::clamp(rate, .1, 1000.);
  const double step = std::max(1, s.fps) / rate; // frames each picture is held
  if (step <= 1 + 1e-9) return t;
  return std::floor(t / step + 1e-6) * step;
}
bool characterPoseAtRest(const Layer &l, double t) {
  if (!l.characterAnimation.value("enabled").toBool() || l.hiddenEffects.value("Text-Animation").toBool()) return false;
  if (!l.richHtml.isEmpty()) return false;
  const int count = int(l.text.size());
  if (count < 1 || count > 400) return false;
  // At rest when every animator of the stack is: in all the way, out not yet.
  // Exactly: an In above 100 (a curve overshooting) throws the letters past
  // their places and an Out below 0 the other way, and both still move.
  for (const auto &animator : textAnimators(l)) {
    const QJsonObject &a = animator.settings;
    if (a.value("wiggleSpeed").toDouble(0) > 0) return false;
    const double inDelay = a.value("delay").toDouble(2);
    const double outDelay = a.value("outDelay").toDouble(2);
    for (int rank = 0; rank < count; ++rank)
      if (std::abs(animatorProgress(*animator.in, a, t, rank, inDelay) - 1) > 1e-9 ||
          std::abs(animatorProgress(*animator.out, a, t, rank, outDelay)) > 1e-9)
        return false;
  }
  return true;
}
bool parametricShape(const Layer &l) {
  const QString kind = shapeGeneratorKind(l);
  return kind == "rect" || kind == "circle" || kind == "polygon" || kind == "star";
}
QPainterPath shapePath(const Layer &l, double t) {
  QPainterPath path;
  const double w = std::clamp(l.p[ShapeWidth].at(t),.01,100000.);
  const double h = std::clamp(l.p[ShapeHeight].at(t),.01,100000.);
  const double round = std::max(0.,l.p[ShapeRoundness].at(t));
  const QString kind = shapeGeneratorKind(l);
  if (kind == "rect") {
    const double r = std::min(round,std::min(w,h)/2);
    path.addRoundedRect(QRectF(-w/2,-h/2,w,h),r,r);
  } else if (kind == "circle") path.addEllipse(QRectF(-w/2,-h/2,w,h));
  else if (kind == "polygon" || kind == "star") {
    const int count = std::clamp(qRound(l.p[ShapePoints].at(t)),kind == "star" ? 1 : 3,1000);
    const int vertices = kind == "star" ? count*2 : count;
    const double outer = std::clamp(l.p[OuterRadius].at(t),.01,100000.);
    const double inner = std::clamp(l.p[InnerRadius].at(t),0.,outer);
    std::vector<QPointF> pts;
    for (int i=0;i<vertices;++i) {
      const double angle = -1.5707963267948966 + i*6.283185307179586/vertices;
      const double radius = kind == "star" && i%2 ? inner : outer;
      pts.emplace_back(std::cos(angle)*radius,std::sin(angle)*radius);
    }
    if (round == 0 || vertices < 3) {
      path.moveTo(pts[0]); for(int i=1;i<vertices;++i) path.lineTo(pts[i]);
    } else {
      for(int i=0;i<vertices;++i) {
        const auto p=pts[i], previous=pts[(i+vertices-1)%vertices], next=pts[(i+1)%vertices];
        const double a=QLineF(p,previous).length(), b=QLineF(p,next).length();
        const auto in=p+(previous-p)*(a>0 ? std::min(round,a/2)/a : 0);
        const auto out=p+(next-p)*(b>0 ? std::min(round,b/2)/b : 0);
        if(i==0) path.moveTo(in); else path.lineTo(in);
        path.quadTo(p,out);
      }
    }
    path.closeSubpath();
  }
  QTransform rotation; rotation.rotate(l.p[PathRotation].at(t));
  return rotation.map(path);
}
QTransform layerMainTransform(const Layer &l) {
  // Rotation and scale only. The main position is not here: it is added to the
  // movement position instead, because the movement transform subtracts its own
  // anchor, and a translation on the anchor's side of that subtraction cancels
  // itself out exactly. Putting it beside Position also carries the anchor
  // point along with the layer, which is where it belongs. A translation before
  // the effects and a translation after them draw the same picture anyway.
  QTransform t;
  t.rotate(l.mainRotation);
  t.scale(l.mainScaleX, l.mainScaleY);
  t.translate(-l.mainAnchorX, -l.mainAnchorY);
  return t;
}
bool layerHasMainTransform(const Layer &l) {
  return l.mainScaleX != 1 || l.mainScaleY != 1 || l.mainRotation != 0 ||
         l.mainAnchorX != 0 || l.mainAnchorY != 0;
}
// The outline should scale with the layer, but it should not become wider on
// one axis than the other. This normalized linear transform contains only the
// anisotropy/shear that has to be cancelled in the source outline. Its overall
// scale is removed, so a uniformly scaled layer retains the familiar behaviour
// where its stroke grows with it.
static QTransform normalizedShapeStrokeMetric(const Layer &l,const Scene &s,double frame) {
  QTransform metric=layerMainTransform(l)*layerWorldTransform(l,s,frame);
  if(!metric.isAffine()) return {};
  const double scale=std::sqrt(std::abs(metric.determinant()));
  if(!std::isfinite(scale) || scale<1e-8) return {};
  metric=QTransform(metric.m11()/scale,metric.m12()/scale,
                    metric.m21()/scale,metric.m22()/scale,0,0);
  return metric.isInvertible() ? metric : QTransform();
}
RepeaterCopyTransform repeaterCopyTransform(const Layer &l, double step) {
  const double angle = step * l.repeaterRotation;
  const double scale = std::pow(std::max(.0001, l.repeaterScale / 100.), step);
  if (!l.repeaterSpiral)
    return {QPointF(step * l.repeaterPositionX, step * l.repeaterPositionY),
            step * l.repeaterPositionZ, angle, scale};
  // Compose the same rotate-scale-translate step "step" times instead of just
  // multiplying it: p -> A*p + t with A = scale*rotate. Applied n times that
  // is A^n*p + (1 + A + ... + A^{n-1})*t, a geometric series with closed form
  // (A^n - 1)/(A - 1)*t that stays exact for a fractional step too, so an
  // animated Offset still moves smoothly along the spiral rather than jumping
  // between the straight-line and curled arrangements.
  const double theta = l.repeaterRotation * 3.141592653589793 / 180.;
  const double s = std::max(.0001, l.repeaterScale / 100.);
  const std::complex<double> planar = s * std::polar(1.0, theta);
  const std::complex<double> seriesXY =
      std::abs(planar - 1.0) < 1e-9
          ? std::complex<double>(step)
          : (std::pow(planar, step) - 1.0) / (planar - 1.0);
  const std::complex<double> offsetXY =
      seriesXY * std::complex<double>(l.repeaterPositionX, l.repeaterPositionY);
  const double seriesZ =
      std::abs(s - 1.0) < 1e-9 ? step : (std::pow(s, step) - 1.0) / (s - 1.0);
  return {QPointF(offsetXY.real(), offsetXY.imag()), seriesZ * l.repeaterPositionZ,
          angle, scale};
}
static QTransform repeaterTransform(const Layer &l, int copy) {
  if (!l.repeaterEnabled) return {};
  const auto t = repeaterCopyTransform(l, copy + l.repeaterOffset);
  QTransform transform = QTransform::fromTranslate(t.offset.x(), t.offset.y());
  transform.rotate(t.angle);
  transform.scale(t.scale, t.scale);
  return transform;
}
static int repeaterCopyCount(const Layer &l) {
  return l.repeaterEnabled ? std::clamp(l.repeaterCopies, 1, 1000) : 1;
}
static QPainterPath shapeStrokeOutlineForPath(const Layer &l,const Scene &s,
                                               double localFrame,
                                               const QPainterPath &path) {
  if(l.strokeWidth<=0) return {};
  const bool closed=l.shapeClosed && !shapeTrimActive(l,localFrame);
  const auto metric=normalizedShapeStrokeMetric(l,s,localFrame+l.start);
  bool invertible=false;
  const auto inverse=metric.inverted(&invertible);
  QPainterPathStroker stroker;
  stroker.setWidth(l.strokeWidth*((!closed || l.strokeMode==2)?1:2));
  stroker.setJoinStyle(Qt::SvgMiterJoin);
  stroker.setMiterLimit(2);
  stroker.setCapStyle(l.shapeStrokeCap == 1 ? Qt::FlatCap : Qt::RoundCap);
  return invertible ? inverse.map(stroker.createStroke(metric.map(path)))
                    : stroker.createStroke(path);
}
static QPainterPath shapeStrokeOutline(const Layer &l,const Scene &s,double localFrame) {
  if (l.strokeWidth <= 0) return {}; // no stroke: no need to build the shape to find that out
  return shapeStrokeOutlineForPath(l, s, localFrame, shapeGeometry(l, localFrame));
}
QRectF layerBounds(const Layer &l, const Scene &s) {
  // Where the particles are now, and the emitter.
  if (l.type == "particles") return particleBounds(l, s, s.frame - l.start);
  const QRectF own = layerOwnBounds(l, s);
  // What it draws and the particles it gives off, which fly past its edges.
  return l.particlesOn ? own.united(particleBounds(l, s, s.frame - l.start)) : own;
}
QRectF layerOwnBounds(const Layer &l, const Scene &s) {
  if (layerIsStructure(l)) return {}; // nothing of its own to cover
  // One question for every shape now: what does its geometry come out as.
  if (l.type == "shape" || parametricShape(l)) {
    const double t=s.frame-l.start;
    // A plain generated shape can say how big it is without being built, which
    // is most of what asking this question used to cost.
    if(l.strokeWidth<=0 || (l.shapeClosed && l.strokeMode==0)) {
      QRectF quick;
      if(shapeGeneratorBounds(l,t,quick)) return quick;
    }
    const auto geometry=shapeGeometry(l,t).boundingRect();
    if(l.strokeWidth<=0 || (l.shapeClosed && l.strokeMode==0)) return geometry;
    return geometry.united(shapeStrokeOutline(l,s,t).boundingRect());
  }
  // A fill layer carries no size of its own: it takes the composition's, so
  // a centred one still covers the frame after the composition is resized
  // rather than leaving the new corners empty. Its own transform still
  // applies, which is what lets it be moved, scaled, rotated or masked.
  if (l.type == "fill" || l.type == "effect")
    return QRectF(-s.width / 2., -s.height / 2., s.width, s.height);
  QSizeF size;
  if (l.type == "text") {
    size = measuredTextSize(l);
  } else if (l.type == "empty" || l.type == "camera" || l.type == "light" || l.type == "audio")
    size = {80, 80};
  else if (l.type == "rect")
    size = {200, 120};
  else if (l.type == "circle")
    size = {160, 160};
  else {
    size = l.type == "video" && !l.footageSize.isEmpty() ? QSizeF(l.footageSize) : QSizeF(l.image.size());
    if (size.isEmpty())
      size = {200, 120};
    // Imported footage uses its native pixel dimensions at 100% layer scale.
    // Only the composition viewer controls how the canvas fits on screen.
  }
  QRectF bounds(-size.width() / 2, -size.height() / 2, size.width(), size.height());
  if (l.type == "text") {
    // The letters themselves, wherever they stand. Under a leading of 1 the
    // lines are shorter than the letters on them, and a box of the lines
    // alone cut off their tops and feet; faux italic leans them past it too.
    // The box grows by as much on every side, so its middle -- what Align
    // and Center go by -- stays where it was.
    const QRectF ink = measuredTextInk(l);
    const double overflow = ink.isNull() ? 0.
        : std::max({0., bounds.left() - ink.left(), ink.right() - bounds.right(),
                    bounds.top() - ink.top(), ink.bottom() - bounds.bottom()});
    const double pad = textStrokeOutwardReach(l) + letterBackgroundReach(l, size) + textAnimationReach(l) + overflow;
    bounds.adjust(-pad, -pad, pad, pad);
  }
  return bounds;
}
namespace {
// How far a text's strokes reach outside its letters, as far as they show (a
// stroke with no colour leaves no edge), and the corners of the one that reaches
// farthest -- the edge the face is seen to have.
struct StrokeEdge {
  double outside = 0;
  int join = 0;
  QColor colour;
};
StrokeEdge textStrokeEdge(const Layer &l) {
  StrokeEdge edge;
  double outside = 0;
  for (const auto &stroke : layerTextStrokes(l)) {
    const double width = std::max(0., stroke.width);
    outside += stroke.mode == 1 ? width : stroke.mode == 2 ? width / 2 : 0;
    if (width > 0 && stroke.color.alpha() > 0 && outside > edge.outside) edge = {outside, stroke.join, stroke.color};
  }
  return edge;
}
// A letter grown out to the edge its strokes give it, with the stroke's own
// corners, so the sides of an extruded
// title meet its face at the outer edge of the stroke and not at the letter
// inside it, with the stroke standing out past the solid. A union of paths is
// slow work, so each letter's is kept: a title's letters are the same ones
// frame after frame, wherever they fly.
QPainterPath grownToStrokeEdge(const QPainterPath &letter, const StrokeEdge &edge) {
  if (edge.outside <= 0 || letter.isEmpty()) return letter;
  QByteArray key;
  key.reserve(letter.elementCount() * 17 + 16);
  key.append(reinterpret_cast<const char *>(&edge.outside), sizeof edge.outside);
  key.append(char(edge.join));
  key.append(char(letter.fillRule()));
  for (int i = 0; i < letter.elementCount(); ++i) {
    const auto element = letter.elementAt(i);
    key.append(char(element.type));
    key.append(reinterpret_cast<const char *>(&element.x), sizeof element.x);
    key.append(reinterpret_cast<const char *>(&element.y), sizeof element.y);
  }
  static QMutex guard;
  static QCache<QByteArray, QPainterPath> kept(2048);
  {
    QMutexLocker lock(&guard);
    if (const QPainterPath *hit = kept.object(key)) return *hit;
  }
  QPainterPathStroker stroker;
  stroker.setWidth(edge.outside * 2);
  stroker.setJoinStyle(edge.join == 1 ? Qt::RoundJoin : Qt::SvgMiterJoin);
  // The letters' stroke lets a sharp corner reach out four times its width
  // before cutting it off; a limit of 1.5 here (a pen's) left the stroke's
  // corners standing out past the solid's.
  stroker.setMiterLimit(4);
  stroker.setCapStyle(Qt::FlatCap);
  const QPainterPath grown = letter.united(stroker.createStroke(letter));
  // Kept, it is drawn by every band at once.
  readyPathForThreads(grown);
  QMutexLocker lock(&guard);
  kept.insert(key, new QPainterPath(grown));
  return grown;
}
// The letters 3D Extrude stands on, one per letter in the order the letters
// come, in the document's own coordinates: each grown to its stroke's edge and,
// while the pen is writing, cut to what it has written so far (empty where it
// has not reached yet). A frame drawn in bands asked for these once per band,
// and a word being written with an Outside stroke cost 50 ms each time --
// 300 to 500 ms a frame -- cutting every grown letter against a grown mask.
//
// Now a letter the pen has finished is the whole letter, grown once and kept;
// one it has not reached is nothing; only the letter it is part way through is
// cut, plain, against the plain mask, and grown after (the part written so
// far, grown, is the solid the face shows). Kept by the letters and where the
// pen is, and built once: a band that asks while another is building it waits
// for that one rather than building it again.
using ExtrudeLetters = std::vector<QPainterPath>;
std::shared_ptr<const ExtrudeLetters> extrudeLetters(QTextDocument &doc, const Layer &l, double t, const StrokeEdge &edge) {
  const bool writing = handwritingEnabled(l);
  double progress = 1, erased = 0, nib = 0;
  if (writing) {
    progress = handwritingProgress(l, t);
    erased = std::clamp(characterProgress(l, 7, t, 0, 0), 0., 1.);
    nib = std::clamp(l.characterAnimation.value("writeNib").toDouble(115), 20., 400.) / 100.;
  }
  QByteArray key = textDocumentKey(l);
  for (const double v : {edge.outside, double(edge.join), writing ? 1. : 0., progress, erased, nib})
    key.append(reinterpret_cast<const char *>(&v), sizeof v);
  using Built = std::shared_future<std::shared_ptr<const ExtrudeLetters>>;
  static std::mutex guard;
  static std::map<QByteArray, Built> kept;
  static std::deque<QByteArray> order;
  std::promise<std::shared_ptr<const ExtrudeLetters>> promise;
  Built built;
  bool mine = false;
  {
    std::lock_guard<std::mutex> lock(guard);
    auto found = kept.find(key);
    if (found != kept.end()) built = found->second;
    else {
      built = promise.get_future().share();
      kept.emplace(key, built);
      order.push_back(key);
      mine = true;
      while (order.size() > 24) {
        kept.erase(order.front());
        order.pop_front();
      }
    }
  }
  if (!mine) return built.get();

  const auto glyphs = characterGlyphs(doc, l);
  // How much of each letter the pen has written, as handwritingReveal
  // decides it: 0 none, 1 part, 2 all.
  std::vector<int> reached(size_t(glyphs.size()), 2);
  if (writing) {
    if (progress <= 0 || erased >= progress) std::fill(reached.begin(), reached.end(), 0);
    else if (progress < 1 || erased > 0) {
      std::vector<double> lengths;
      double total = 0;
      for (const auto &glyph : glyphs) {
        lengths.push_back(cachedLetterSpine(glyph.path).length);
        total += lengths.back();
      }
      if (total <= 0) {
        const int first = int(std::ceil(erased * glyphs.size())), last = int(progress * glyphs.size());
        for (int i = 0; i < glyphs.size(); ++i) reached[size_t(i)] = i >= first && i < last ? 2 : 0;
      } else {
        const double from = erased * total, to = progress * total;
        double start = 0;
        for (size_t i = 0; i < lengths.size(); ++i) {
          const double end = start + lengths[i];
          reached[i] = end <= from || start >= to ? 0 : start >= from && end <= to ? 2 : 1;
          start = end;
        }
      }
    }
  }
  auto letters = std::make_shared<ExtrudeLetters>();
  letters->reserve(size_t(glyphs.size()));
  QPainterPath written;
  for (int i = 0; i < glyphs.size(); ++i) {
    const QPainterPath &letter = glyphs[i].path;
    if (reached[size_t(i)] == 0) { letters->push_back({}); continue; }
    if (reached[size_t(i)] == 2) { letters->push_back(grownToStrokeEdge(letter, edge)); continue; }
    if (written.isEmpty()) written = cachedHandwritingMask(doc, l, t, 0);
    QPainterPath part = written.controlPointRect().intersects(letter.controlPointRect()) ? letter.intersected(written) : QPainterPath();
    letters->push_back(part.isEmpty() ? part : grownToStrokeEdge(part, edge));
  }
  std::shared_ptr<const ExtrudeLetters> done = letters;
  promise.set_value(done);
  return done;
}
}
// The outline 3D Extrude gives depth to, in layer coordinates and after the
// layer's own main transform: the shape as it is drawn (modifiers and trim
// included) out to the edge of its stroke, the letters of a text layer out to
// the edge of theirs, or the box of anything else.
QPainterPath layerExtrudeOutline(const Layer &l, const Scene &s, double t) {
  QPainterPath outline;
  if (l.type == "shape" || parametricShape(l)) {
    if (!l.shapeFill) return {};
    outline = shapeGeometry(l, t);
    // An Inside stroke stays within the shape; any other carries its edge out.
    if (l.strokeWidth > 0 && l.strokeColor.alpha() > 0 && !(l.shapeClosed && l.strokeMode == 0)) {
      const QPainterPath stroke = shapeStrokeOutlineForPath(l, s, t, outline);
      if (!stroke.isEmpty()) outline = outline.united(stroke);
    }
  } else if (l.type == "text") {
    auto doc = cachedTextDocument(l);
    const QSizeF size = doc->size();
    // The pen gives the letters their depth as it reaches them: without this
    // the sides of a word stood there in full while its face was still being
    // written, which reads as the writing filling in a shape already built.
    outline.setFillRule(Qt::WindingFill);
    for (const QPainterPath &letter : *extrudeLetters(*doc, l, t, textStrokeEdge(l))) outline.addPath(letter);
    if (outline.isEmpty()) return {};
    outline.translate(-size.width() / 2, -size.height() / 2 + textLeadingOffset(l, *doc));
  } else if (l.type == "fill" || l.type == "effect" || l.type == "camera" || l.type == "light" || l.type == "empty" || l.type == "audio" || l.type == "group" ||
             l.type == "particles" || layerIsStructure(l)) {
    return {};
  } else {
    outline.addRect(layerBounds(l, s));
  }
  if (layerHasMainTransform(l)) outline = layerMainTransform(l).map(outline);
  return outline;
}
std::vector<ExtrudePiece> layerExtrudePieces(const Layer &l, const Scene &s, double t) {
  // A text whose letters animate gives each letter its walls where the letter
  // is drawn: moved, turned, scaled and faded with it. From the letters at
  // rest instead, the sides of a word stood where it would end up, solid,
  // while its faces were still flying and fading in.
  if (l.type == "text" && l.characterAnimation.value("enabled").toBool() &&
      !l.hiddenEffects.value("Text-Animation").toBool()) {
    auto doc = cachedTextDocument(l);
    const QSizeF size = doc->size();
    const auto glyphs = characterGlyphs(*doc, l);
    const auto poses = animatedCharacterPoses(glyphs, l, t, s.fps);
    const auto letters = extrudeLetters(*doc, l, t, textStrokeEdge(l));
    const QTransform place = QTransform::fromTranslate(-size.width() / 2, -size.height() / 2 + textLeadingOffset(l, *doc)) *
                             (layerHasMainTransform(l) ? layerMainTransform(l) : QTransform());
    std::vector<ExtrudePiece> pieces;
    for (int i = 0; i < glyphs.size() && i < poses.size() && i < int(letters->size()); ++i) {
      const auto &pose = poses[i];
      // The pen gives a letter its depth as it reaches it.
      const QPainterPath &letter = (*letters)[size_t(i)];
      if (pose.opacity <= 0 || letter.isEmpty()) continue;
      const QPointF centre = glyphs[i].center;
      const QTransform posed = QTransform::fromTranslate(-centre.x(), -centre.y()) * pose.matrix *
                               QTransform::fromTranslate(centre.x(), centre.y());
      pieces.push_back({(posed * place).map(letter), std::clamp(pose.opacity, 0., 1.)});
    }
    return pieces;
  }
  QPainterPath outline = layerExtrudeOutline(l, s, t);
  if (outline.isEmpty()) return {};
  return {{outline, 1.}};
}
QColor layerExtrudeSideColour(const Layer &l) {
  if (l.type == "text") {
    const StrokeEdge edge = textStrokeEdge(l);
    if (edge.outside > 0 && edge.colour.isValid()) return edge.colour;
  } else if ((l.type == "shape" || parametricShape(l)) && l.strokeWidth > 0 && l.strokeColor.alpha() > 0 &&
             !(l.shapeClosed && l.strokeMode == 0)) {
    return l.strokeColor;
  }
  return l.color.isValid() ? l.color : QColor(Qt::white);
}
static QRectF repeatedLayerBounds(const Layer &l, const QRectF &source) {
  QRectF bounds;
  for (int copy = 0; copy < repeaterCopyCount(l); ++copy) {
    const QRectF copyBounds = repeaterTransform(l, copy).mapRect(source);
    bounds = copy == 0 ? copyBounds : bounds.united(copyBounds);
  }
  return bounds;
}
QRectF layerContentBounds(const Layer &l, const Scene &s) {
  // A particle layer is placed, aligned and snapped by its emitter; the
  // particles themselves go where they go.
  auto bounds = l.type == "particles" ? particleEmitterBounds(l) : layerOwnBounds(l, s);
  if (layerHasMainTransform(l)) bounds = layerMainTransform(l).mapRect(bounds);
  // Repeater copies are rendered instances, not new editable geometry. Keeping
  // the source bounds here makes selection, transform handles, align and snap
  // stay anchored to the original layer while repeater values animate.
  return bounds;
}
QRectF layerSelectionBounds(const Layer &l, const Scene &s) {
  // The box around a particle layer is its emitter, which holds still, not the
  // cloud, which grows and swings every frame as the particles fly out.
  if (l.type == "particles") return particleEmitterBounds(l);
  // The layer's own drawing: particles it gives off are not what is grabbed.
  QRectF bounds = layerOwnBounds(l, s);
  if (l.type != "text") return bounds;
  // Ascent and descent put the line box where the ink is not: at 200 px Arial
  // leaves 38 px of it above the letters and 1 px below, and every typeface is
  // out by a different amount. The box keeps the size the metrics give it --
  // that part reads correctly -- and moves onto the letters, so the slack
  // falls evenly around them.
  //
  // Only what the viewer draws and drags. Position, alignment and snapping
  // stay on the line box, where a layer centred in the composition still
  // reports a round 960.
  const QRectF ink = measuredTextInk(l);
  if (!ink.isNull()) bounds.moveCenter(ink.center());
  return bounds;
}
QRectF layerSelectionContentBounds(const Layer &l, const Scene &s) {
  auto bounds = layerSelectionBounds(l, s);
  if (layerHasMainTransform(l)) bounds = layerMainTransform(l).mapRect(bounds);
  return bounds;
}
// Where X and Y are both keyed (to a thousandth of a frame) and neither key
// only times the move (Key::timing), in order.
static std::vector<double> pairedPositionFrames(const Layer &l) {
  std::vector<double> paired;
  for (const Key &x : l.p[0].keys)
    for (const Key &y : l.p[1].keys)
      if (std::abs(x.frame - y.frame) < 1e-3) {
        if (!x.timing && !y.timing) paired.push_back(x.frame);
        break;
      }
  std::sort(paired.begin(), paired.end());
  return paired;
}
std::vector<double> positionPathFrames(const Layer &l) {
  std::vector<double> frames = pairedPositionFrames(l);
  if (frames.size() >= 2) return frames;
  // Not two such points: every key is one, as it always was.
  frames.clear();
  for (int axis : {0, 1})
    for (const Key &k : l.p[axis].keys) frames.push_back(k.frame);
  std::sort(frames.begin(), frames.end());
  std::vector<double> unique;
  for (double f : frames)
    if (unique.empty() || std::abs(f - unique.back()) > 1e-3) unique.push_back(f);
  return unique;
}
// The position at `t` on the stretch of the path from the keys at `ax` to `bx`
// (X) and `ayKey`/`byKey` (Y; `ayVal`/`byVal` their values, or Y's own where it
// has no key there): how far along from the reference key's easing, or
// `given` when the timing comes from elsewhere -- keys on one dimension alone,
// between (positionPathFrames). Past either end -- an anticipation, an
// overshoot -- it goes on straight, the way the path was heading.
static QPointF positionOnSegment(double t, const Key &ax, const Key &bx, const Key *ayKey, const Key *byKey,
                                 double ayVal, double byVal, std::optional<double> given) {
  const double dt = bx.frame - ax.frame;
  const double dvX = bx.value - ax.value;
  const double dvY = byVal - ayVal;

  const bool hasNormX = ax.custom && ax.absolute &&
                        (ax.hasNormCurve || std::abs(ax.curve[1]) > 1e-5 || std::abs(ax.curve[3]) > 1e-5);
  const bool hasNormY = ayKey && ayKey->custom && ayKey->absolute &&
                        (ayKey->hasNormCurve || std::abs(ayKey->curve[1]) > 1e-5 || std::abs(ayKey->curve[3]) > 1e-5);
  bool useY = false;
  if (hasNormY && !hasNormX) {
    useY = true;
  } else if (hasNormX && !hasNormY) {
    useY = false;
  } else if (ayKey && (ayKey->custom || ayKey->ease != "linear")) {
    if (!ax.custom && ax.ease == "linear") {
      useY = true;
    } else if (std::abs(dvY) > std::abs(dvX) && std::abs(dvY) > 1e-4) {
      useY = true;
    }
  } else if (std::abs(dvX) <= 1e-4 && std::abs(dvY) > 1e-4 && ayKey) {
    useY = true;
  }

  const Key &kRef = useY ? *ayKey : ax;
  const Key dummyB = useY ? (byKey ? *byKey : bx) : bx;
  const double refValA = useY ? ayVal : ax.value;
  const double refValB = useY ? byVal : bx.value;
  const double dv = refValB - refValA;

  double progress = 0;
  if (given) {
    progress = *given;
  } else if (kRef.ease == "hold") {
    progress = 0.0;
  } else if (kRef.custom && kRef.absolute) {
    auto c = segmentPoints(kRef, dummyB);
    double lo = 0, hi = 1;
    for (int n = 0; n < 40; ++n) {
      double mid = (lo + hi) * .5;
      auto q = lerp(lerp(lerp(c[0], c[1], mid), lerp(c[1], c[2], mid), mid),
                    lerp(lerp(c[1], c[2], mid), lerp(c[2], c[3], mid), mid), mid);
      if (q.x() < t) lo = mid; else hi = mid;
    }
    double mid = (lo + hi) * .5;
    if (std::abs(dv) > 1e-6 && (std::abs(kRef.curve[1]) > 1e-6 || std::abs(kRef.curve[3]) > 1e-6)) {
      double val = lerp(lerp(lerp(c[0], c[1], mid), lerp(c[1], c[2], mid), mid),
                        lerp(lerp(c[1], c[2], mid), lerp(c[2], c[3], mid), mid), mid).y();
      progress = (val - refValA) / dv;
    } else if (kRef.hasNormCurve) {
      const double u = mid, om = 1.0 - u;
      progress = 3.0 * om * om * u * kRef.normCurve[0] +
                 3.0 * om * u * u * (1.0 + kRef.normCurve[1]) +
                 u * u * u;
    } else if (std::abs(dv) > 1e-6) {
      double val = lerp(lerp(lerp(c[0], c[1], mid), lerp(c[1], c[2], mid), mid),
                        lerp(lerp(c[1], c[2], mid), lerp(c[2], c[3], mid), mid), mid).y();
      progress = (val - refValA) / dv;
    } else {
      const double chord = std::hypot(dvX, dvY);
      if (chord > 1e-6 && (std::abs(kRef.curve[1]) > 1e-6 || std::abs(kRef.curve[3]) > 1e-6)) {
        double val = lerp(lerp(lerp(c[0], c[1], mid), lerp(c[1], c[2], mid), mid),
                          lerp(lerp(c[1], c[2], mid), lerp(c[2], c[3], mid), mid), mid).y();
        progress = (val - refValA) / chord;
      } else {
        progress = eased((t - ax.frame) / dt, kRef);
      }
    }
  } else {
    progress = eased((t - ax.frame) / dt, kRef);
  }

  const bool hasSpatial = ax.hasSpatial || bx.hasSpatial ||
                          (ayKey && ayKey->hasSpatial) || (byKey && byKey->hasSpatial);
  const QPointF p0(ax.value, ayVal);
  const QPointF p3(bx.value, byVal);

  if (hasSpatial) {
    QPointF sOut = ax.hasSpatial ? ax.spatialOut : (ayKey && ayKey->hasSpatial ? ayKey->spatialOut : (p3 - p0) / 3.0);
    QPointF sIn = bx.hasSpatial ? bx.spatialIn : (byKey && byKey->hasSpatial ? byKey->spatialIn : -(p3 - p0) / 3.0);
    const QPointF p1 = p0 + sOut;
    const QPointF p2 = p3 + sIn;

    if (progress < 0.0) {
      QPointF v0 = 3.0 * (p1 - p0);
      if (std::hypot(v0.x(), v0.y()) < 1e-4) v0 = p3 - p0;
      return p0 + v0 * progress;
    } else if (progress > 1.0) {
      QPointF v1 = 3.0 * (p3 - p2);
      if (std::hypot(v1.x(), v1.y()) < 1e-4) v1 = p3 - p0;
      return p3 + v1 * (progress - 1.0);
    } else {
      return lerp(lerp(lerp(p0, p1, progress), lerp(p1, p2, progress), progress),
                  lerp(lerp(p1, p2, progress), lerp(p2, p3, progress), progress), progress);
    }
  } else {
    return p0 + (p3 - p0) * progress;
  }
}
QPointF layerLocalPosition(const Layer &l, double t) {
  if (l.separatePosition || l.p[0].hasExpression() || l.p[1].hasExpression()) {
    return QPointF(l.p[0].at(t), l.p[1].at(t));
  }
  if (l.p[0].keys.empty() || l.p[1].keys.empty()) {
    return QPointF(l.p[0].at(t), l.p[1].at(t));
  }

  const auto &kx = l.p[0].keys;
  const auto &ky = l.p[1].keys;

  // The path's points are where X and Y are both keyed. A key set in the
  // graph editor between two of them -- an anticipation, an overshoot
  // (Key::timing) -- does not bend the path: it times the move along it. How
  // far along is that dimension's own curve read between the two points'
  // values, so a curve that goes past either value carries the layer on past
  // that end, straight the way the path heads there. X and Y keyed apart
  // between two points, as before there was a path, keep each their own
  // timing.
  const std::vector<double> points = pairedPositionFrames(l);
  if (points.size() >= 2) {
    if (t <= points.front() || t >= points.back()) return QPointF(l.p[0].at(t), l.p[1].at(t));
    size_t i = 0;
    while (i + 2 < points.size() && t > points[i + 1]) ++i;
    const double fa = points[i], fb = points[i + 1];
    auto keyAt = [](const std::vector<Key> &keys, double f) -> const Key * {
      for (const Key &k : keys)
        if (std::abs(k.frame - f) < 1e-3) return &k;
      return nullptr;
    };
    const Key *ax = keyAt(kx, fa), *bx = keyAt(kx, fb), *ay = keyAt(ky, fa), *by = keyAt(ky, fb);
    if (ax && bx && ay && by && bx->frame - ax->frame > 1e-6) {
      auto inside = [&](const Key &k) { return k.frame > fa + 1e-3 && k.frame < fb - 1e-3; };
      // A timing key's partner on the other dimension (made to keep the two
      // keyed together) times nothing of its own.
      auto timingAt = [&](double f) {
        for (const auto *keys : {&kx, &ky})
          for (const Key &k : *keys)
            if (k.timing && std::abs(k.frame - f) < 1e-3) return true;
        return false;
      };
      bool apart = false, timedX = false, timedY = false;
      for (int axis : {0, 1})
        for (const Key &k : axis ? ky : kx) {
          if (!inside(k)) continue;
          if (k.timing) (axis ? timedY : timedX) = true;
          else if (!timingAt(k.frame)) apart = true;
        }
      if (apart) return QPointF(l.p[0].at(t), l.p[1].at(t));
      std::optional<double> given;
      if (timedX || timedY) {
        const double spanX = bx->value - ax->value, spanY = by->value - ay->value;
        const int axis = timedX && (!timedY || std::abs(spanX) >= std::abs(spanY)) ? 0 : 1;
        const double from = axis ? ay->value : ax->value, span = axis ? spanY : spanX;
        if (std::abs(span) < 1e-6) return QPointF(l.p[0].at(t), l.p[1].at(t));
        given = (l.p[axis].at(t) - from) / span;
      }
      return positionOnSegment(t, *ax, *bx, ay, by, ay->value, by->value, given);
    }
  }
  // Not two points keyed on both, and no path drawn with handles: X and Y
  // keyed apart, each with its own timing, as before there was a path.
  const auto spatial = [](const Key &k) { return k.hasSpatial; };
  if (points.size() < 2 && std::none_of(kx.begin(), kx.end(), spatial) && std::none_of(ky.begin(), ky.end(), spatial))
    return QPointF(l.p[0].at(t), l.p[1].at(t));

  if (t <= kx.front().frame) {
    const double yVal = !ky.empty() ? ky.front().value : l.p[1].keyedAt(t);
    return QPointF(kx.front().value, yVal);
  }
  if (t >= kx.back().frame) {
    const double yVal = !ky.empty() ? ky.back().value : l.p[1].keyedAt(t);
    return QPointF(kx.back().value, yVal);
  }

  for (size_t i = 0; i + 1 < kx.size(); ++i) {
    if (t <= kx[i + 1].frame) {
      const auto &ax = kx[i];
      const auto &bx = kx[i + 1];
      const double dt = bx.frame - ax.frame;
      if (dt <= 1e-6) return QPointF(bx.value, l.p[1].keyedAt(t));

      const Key *ayKey = nullptr;
      const Key *byKey = nullptr;
      for (size_t j = 0; j < ky.size(); ++j) {
        if (std::abs(ky[j].frame - ax.frame) < 1e-3) ayKey = &ky[j];
        if (std::abs(ky[j].frame - bx.frame) < 1e-3) byKey = &ky[j];
      }
      const double ayVal = ayKey ? ayKey->value : l.p[1].keyedAt(ax.frame);
      const double byVal = byKey ? byKey->value : l.p[1].keyedAt(bx.frame);

      return positionOnSegment(t, ax, bx, ayKey, byKey, ayVal, byVal, std::nullopt);
    }
  }

  return QPointF(l.p[0].at(t), l.p[1].at(t));
}
static QTransform worldTransform(const Layer &l, const Scene &s, double frame, QSet<QString> &visited) {
  if (visited.contains(l.id)) return {};
  visited.insert(l.id);
  double t = frame - l.start;
  QTransform local;
  const QPointF pos = layerLocalPosition(l, t);
  local.translate(pos.x() + l.mainX, pos.y() + l.mainY);
  local.rotate(l.p[4].at(t));
  local.scale(l.p[2].at(t), l.p[3].at(t));
  for (int i = int(l.additionalTransforms.size()) - 1; i >= 0; --i) {
    const auto &at = l.additionalTransforms[i];
    at.resolveProps(l);
    const double ax = at.propAnchorX >= 0 ? l.at(at.propAnchorX).at(t) : 0.;
    const double ay = at.propAnchorY >= 0 ? l.at(at.propAnchorY).at(t) : 0.;
    const double px = at.propPosX >= 0 ? l.at(at.propPosX).at(t) : 0.;
    const double py = at.propPosY >= 0 ? l.at(at.propPosY).at(t) : 0.;
    const double sx = at.propScaleX >= 0 ? l.at(at.propScaleX).at(t) : 1.;
    const double sy = at.propScaleY >= 0 ? l.at(at.propScaleY).at(t) : 1.;
    const double rot = at.propRotation >= 0 ? l.at(at.propRotation).at(t) : 0.;

    local.translate(ax + px, ay + py);
    local.rotate(rot);
    local.scale(sx, sy);
    local.translate(-ax, -ay);
  }
  local.translate(-l.p[AnchorX].at(t), -l.p[AnchorY].at(t));
  if (!l.parentId.isEmpty())
    for (const auto &parent : s.layers)
      if (parent.id == l.parentId)
        return local * l.parentBind * worldTransform(parent, s, frame, visited);
  return local * l.parentBind;
}
QTransform layerWorldTransform(const Layer &l, const Scene &s, double frame) {
  if (l.threeD && l.type != "camera") return projectLayer(l, s, frame);
  QSet<QString> visited;
  return worldTransform(l, s, frame, visited);
}
QTransform layerParentTransform(const Layer &l, const Scene &s, double frame) {
  if (l.parentId.isEmpty()) return l.parentBind;
  for (const auto &parent : s.layers)
    if (!l.parentId.isEmpty() && parent.id == l.parentId) {
      QSet<QString> visited;
      return l.parentBind * worldTransform(parent, s, frame, visited);
    }
  return l.parentBind;
}
bool setLayerParent(Scene &s, int child, int parent) {
  if (child < 0 || child >= int(s.layers.size()) || parent >= int(s.layers.size()) || parent == child) return false;
  auto &l = s.layers[child];
  // A Layer Struktur has no transform to give or to take.
  if (parent >= 0 && (layerIsStructure(l) || layerIsStructure(s.layers[size_t(parent)]))) return false;
  QTransform target;
  QMatrix4x4 target3D;
  QMatrix4x4 previous3D = l.parentBind3D;
  for (const auto &item : s.layers)
    if (!l.parentId.isEmpty() && item.id == l.parentId)
      previous3D = layerWorldMatrix(item, s, s.frame) * l.parentBind3D;
  QString id;
  if (parent >= 0) {
    id = s.layers[parent].id;
    QString cursor = id;
    QSet<QString> visited;
    while (!cursor.isEmpty()) {
      if (cursor == l.id || visited.contains(cursor)) return false;
      visited.insert(cursor);
      QString next;
      for (const auto &item : s.layers) if (item.id == cursor) { next = item.parentId; break; }
      cursor = next;
    }
    QSet<QString> visited2D;
    target = worldTransform(s.layers[parent], s, s.frame, visited2D);
    if (!target.isInvertible()) return false;
    target3D = layerWorldMatrix(s.layers[parent], s, s.frame);
    bool invertible;
    target3D.inverted(&invertible);
    if (!invertible) return false;
  }
  QTransform previous2D = l.parentBind;
  for (const auto &item : s.layers)
    if (!l.parentId.isEmpty() && item.id == l.parentId) {
      QSet<QString> visited2D;
      previous2D *= worldTransform(item, s, s.frame, visited2D);
    }
  l.parentBind = previous2D * target.inverted();
  l.parentBind3D = target3D.inverted() * previous3D;
  l.parentId = id;
  return true;
}
bool vectorEffectStroke(const Layer &l) {
  // Baking Stroke into the source is only equivalent when no earlier pixel
  // effect changes that source. Otherwise Stroke must consume the stack result.
  for(const auto &name:layerEffectOrder(l)) {
    if(name=="Stroke")break;
    if(l.hiddenEffects[name].toBool())continue;
    if(l.appliedEffects[name].toBool())return false;
    if((name=="Gaussian Blur" && l.blur>0) || (name=="Glow" && l.glow>0) ||
       (name=="Drop Shadow" && l.shadow>0))return false;
  }
  // Animated characters are stroked per glyph by drawAnimatedCharacters, which
  // keeps every glyph's own outline correct at its own opacity. The raster
  // path cannot: it strokes the composite alpha, so a glyph fading in vanishes
  // from the distance field below half alpha and the stroke bleeds through it
  // above.
  return l.type=="text" && layerTextStrokes(l).empty() &&
      (!(l.appliedEffects["Gradient"].toBool() && !l.hiddenEffects["Gradient"].toBool()) ||
       vectorEffectGradient(l)) &&
      (!(l.appliedEffects["Fill"].toBool() && !l.hiddenEffects["Fill"].toBool()) ||
       vectorEffectFill(l)) &&
      l.appliedEffects["Stroke"].toBool() && !l.hiddenEffects["Stroke"].toBool();
}
// A layer whose fill is one flat colour, which is what lets a colour effect be
// expressed as the brush it is drawn with rather than a pass over its pixels.
static bool solidFillLayer(const Layer &l) {
  // A colour pass must recolour the existing outline as well as the fill.
  // Replacing only the fill brush would leave pre-existing strokes unchanged.
  if (l.type == "text" ? !layerTextStrokes(l).empty() : l.strokeWidth > 0) return false;
  if (l.type == "text") {
    // Per-character colour, coloured fonts and animated glyphs are not one fill.
    if (!l.richHtml.isEmpty()) return false;
    if (l.characterAnimation.value("enabled").toBool() &&
        !l.hiddenEffects.value("Text-Animation").toBool())
      return false;
    return true;
  }
  return l.type == "fill" || l.type == "shape" || parametricShape(l);
}
// The effects at the head of the chain that fold into the drawing. Fill and
// Gradient both describe what colour the shape is, so they can be handed to the
// painter as a brush; the run ends at the first effect that needs pixels,
// because from there on the gradient is meant to tint that result instead.
static QStringList foldedEffects(const Layer &l) {
  QStringList folded;
  // Only a Fill or a Gradient is ever folded into the brush, so a layer
  // carrying neither has nothing to fold. Asked of every shape twice over
  // while a frame is drawn, and the whole effect order was being built to
  // answer it each time.
  if (!l.appliedEffects.value("Fill").toBool() &&
      !l.appliedEffects.value("Gradient").toBool())
    return folded;
  if (!solidFillLayer(l)) return folded;
  for (const auto &name : layerEffectOrder(l)) {
    if(l.hiddenEffects[name].toBool())continue;
    const bool applied = name == "Gaussian Blur" ? l.blur != 0
                       : name == "Glow"          ? l.glow != 0
                       : name == "Drop Shadow"   ? l.shadow != 0
                       : l.appliedEffects[name].toBool() && !l.hiddenEffects[name].toBool();
    if (!applied) continue;
    if (name != "Fill" && name != "Gradient") break;
    folded << name;
  }
  return folded;
}
bool vectorEffectFill(const Layer &l) { return foldedEffects(l).contains("Fill"); }
bool vectorEffectGradient(const Layer &l) { return foldedEffects(l).contains("Gradient"); }
static QColor blendedTowards(const QColor &base, const QColor &towards, double amount) {
  return QColor::fromRgbF(float(base.redF() * (1 - amount) + towards.redF() * amount),
                          float(base.greenF() * (1 - amount) + towards.greenF() * amount),
                          float(base.blueF() * (1 - amount) + towards.blueF() * amount),
                          float(base.alphaF()));
}
QBrush layerDrawingBrush(const Layer &l, QRectF box) {
  QColor base = l.color;
  QBrush brush(base);
  for (const auto &name : foldedEffects(l)) {
    if (name == "Fill") {
      auto s = l.surfaceEffects["Fill"].toObject();
      QColor colour(s["color"].toString("#ffffff"));
      const double opacity = std::clamp(s["opacity"].toDouble(100) / 100., 0., 1.);
      base = QColor::fromRgbF(float(colour.redF()), float(colour.greenF()), float(colour.blueF()),
                              float(base.alphaF() * colour.alphaF() * opacity));
      brush = QBrush(base);
      continue;
    }
    auto s = l.surfaceEffects["Gradient"].toObject();
    const QColor from(s["start"].toString("#ff6a00")), to(s["end"].toString("#6633ff"));
    const double amount = std::clamp(s["amount"].toDouble(100) / 100., 0., 1.);
    // The per-pixel form mixes the layer's own colour with the gradient by the
    // amount; because that mix is linear in the gradient position, folding it
    // into the two stops gives the same colour everywhere.
    if (s["type"].toInt() == 1) {
      QRadialGradient radial(box.center(),
                             std::max(1., std::max(box.width(), box.height()) / 2), box.center());
      radial.setColorAt(0, blendedTowards(base, from, amount));
      radial.setColorAt(1, blendedTowards(base, to, amount));
      brush = radial;
      continue;
    }
    const double angle = s["angle"].toDouble() * 3.141592653589793 / 180.;
    const QPointF axis(std::cos(angle), std::sin(angle));
    const double span = std::max(1., std::abs(axis.x()) * box.width() +
                                         std::abs(axis.y()) * box.height());
    QLinearGradient linear(box.center() - axis * span / 2, box.center() + axis * span / 2);
    linear.setColorAt(0, blendedTowards(base, from, amount));
    linear.setColorAt(1, blendedTowards(base, to, amount));
    brush = linear;
  }
  return brush;
}
QPen effectStrokePen(const Layer &l) {
  auto s = l.surfaceEffects["Stroke"].toObject();
  return QPen(QColor(s["color"].toString("#ffffff")),
              std::clamp(s["width"].toDouble(4), 0., 200.));
}
int effectStrokeMode(const Layer &l) {
  return std::clamp(l.surfaceEffects["Stroke"].toObject()["mode"].toInt(1), 0, 2);
}
bool effectStrokeOnly(const Layer &l) {
  return l.appliedEffects["Stroke"].toBool() && !l.hiddenEffects["Stroke"].toBool() &&
         l.surfaceEffects["Stroke"].toObject()["strokeOnly"].toBool(false);
}
bool textStrokeOnly(const Layer &l) { return l.type == "text" && l.textStrokeOnly; }
// What a text's letters are stroked with in the raster stage, from their
// distance: its Character strokes, and the Stroke effect when it goes into the
// letters' own picture (vectorEffectStroke) -- drawn the same way, with Miter
// corners unless it asks for round ones. Stroked with a pen and the letters'
// outlines united, a wide Stroke effect came apart: holes where the union
// failed, a "W" grown into a hexagon.
std::vector<TextStrokeStyle> textStrokeStack(const Layer &l) {
  auto strokes = layerTextStrokes(l);
  if (vectorEffectStroke(l)) {
    const auto settings = l.surfaceEffects["Stroke"].toObject();
    TextStrokeStyle stroke;
    stroke.width = std::clamp(settings["width"].toDouble(4), 0., 200.);
    stroke.color = QColor(settings["color"].toString("#ffffff"));
    stroke.mode = effectStrokeMode(l);
    stroke.join = settings["join"].toString("miter") == "round" ? 1 : 0;
    if (stroke.width > 0) strokes.push_back(stroke);
  }
  return strokes;
}
// Only the strokes, the letters left out: the Character option, or the Stroke
// effect's when it is in the stack.
bool textStackStrokeOnly(const Layer &l) {
  return textStrokeOnly(l) || (vectorEffectStroke(l) && effectStrokeOnly(l));
}
static double strokeStackOutward(const std::vector<TextStrokeStyle> &strokes) {
  double reach = 0;
  for (const auto &stroke : strokes) {
    const double width = std::max(0., stroke.width);
    reach += stroke.mode == 1 ? width : stroke.mode == 2 ? width / 2 : 0;
  }
  return reach;
}
// How far the Stroke surface effect reaches beyond the layer's own shape. An
// inside stroke stays within it and a centred stroke crosses only half its
// width, so padding the offscreen raster by the full width -- and paying for a
// distance field over all those extra pixels -- is work nothing ever reads.
static double strokeOutwardReach(const Layer &l) {
  auto s = l.surfaceEffects["Stroke"].toObject();
  const int mode = std::clamp(s["mode"].toInt(1), 0, 2);
  const double width = std::clamp(s["width"].toDouble(4), 0., 200.);
  const double reach = mode == 0 ? 0 : mode == 2 ? width / 2 : width;
  // Quantized, so nudging the width does not resize the offscreen raster on
  // every step of the pointer. An identical raster keeps its distance field.
  return reach > 0 ? std::ceil(reach / 24.) * 24. : 0;
}
// How far the layer's pack effects can carry its pixels (a colour fringe, a
// displacement), in layer pixels, so the raster has room for them.
static double rgbSplitReach(const Layer &l) { return layerPackReach(l); }
// Whether the layer carries a built-in colour or look effect that has to be run
// on a raster of it. A picture or a shape has no raster of its own to hand them
// unless something asks for one, and without this a lone HSL or Levels on such a
// layer was drawn as if it were not there.
static bool layerHasPixelEffect(const Layer &l) { return layerCarriesPackEffect(l); }
std::atomic<quint64> &blurredLetterPictures() {
  static std::atomic<quint64> made{0};
  return made;
}
void readyPathForThreads(const QPainterPath &path) {
  if (path.isEmpty()) return;
  path.boundingRect();
  path.controlPointRect();
  qtVectorPathForPath(path).controlPointRect();
}
namespace {
// On while the raster stage paints a text layer whose Character strokes it will
// build itself from the letters' distance field. Per thread, because source
// strips paint on workers of their own.
thread_local bool characterStrokesDeferred = false;
// On while the raster stage paints an animated text with no such strokes: its
// blurred letters are set apart (characterStrokeAnimated) and laid after.
thread_local bool characterLettersApart = false;
// Where the letters' outlines go while they are painted, in the painter's
// device pixels: a Miter stroke takes its corners from them.
thread_local std::vector<QPainterPath> *characterStrokeLetters = nullptr;
// The same for an animated text: its letters' outlines, opacity and size, and
// its blurred letters finished apart.
thread_local AnimatedLetterStrokes *characterStrokeAnimated = nullptr;
// Where the letters' backgrounds go meanwhile (LetterBackdrop), laid under the
// letters and their strokes once those are built. Only the strip that collects
// has one; the others leave them out.
thread_local LetterBackdrop *characterStrokeBackdrop = nullptr;
// The letters' raster stage as this thread had it, put back however the
// drawing ends: left set by a drawing that threw, the next text this worker
// drew went into a collection long gone.
struct LetterStage {
  bool deferred = characterStrokesDeferred, apart = characterLettersApart;
  std::vector<QPainterPath> *outlines = characterStrokeLetters;
  AnimatedLetterStrokes *animated = characterStrokeAnimated;
  LetterBackdrop *backdrop = characterStrokeBackdrop;
  ~LetterStage() {
    characterStrokesDeferred = deferred;
    characterLettersApart = apart;
    characterStrokeLetters = outlines;
    characterStrokeAnimated = animated;
    characterStrokeBackdrop = backdrop;
  }
};
// An animated text's blurred letters, finished apart, laid over its raster. A
// blurred letter's picture is drawn small and enlarged, so this is most of the
// raster resampled several times over; one painter did it on one core, about
// as long as the letters took to build. Each band of rows is painted through
// a picture of its own over the same pixels, on the cores at once: every
// pixel is worked out from the overlays alone, so the bands meet exactly.
// Only `within` (all of it when null) can show, and only that is laid.
void layLetterOverlays(QImage &raster, const std::vector<AnimatedLetterStrokes::Overlay> &overlays,
                       QRect within = QRect()) {
  if (overlays.empty() || raster.isNull()) return;
  const QRect area = within.isValid() ? within & raster.rect() : raster.rect();
  if (area.isEmpty()) return;
  // Each clip is set on every band at once (readyPathForThreads).
  for (const auto &overlay : overlays)
    if (overlay.clipped) readyPathForThreads(overlay.clip);
  uchar *const bits = raster.bits();
  const qsizetype stride = raster.bytesPerLine();
  constexpr int band = 64;
  const int first = area.top() / band;
  renderParallelEach(area.bottom() / band - first + 1, [&](int n) {
    const int top = (first + n) * band, rows = std::min(band, raster.height() - top);
    QImage part(bits + top * stride, raster.width(), rows, stride, raster.format());
    QPainter over(&part);
    over.setRenderHint(QPainter::SmoothPixmapTransform);
    over.translate(0, -top);
    for (const auto &overlay : overlays) {
      if (overlay.at.bottom() < top || overlay.at.top() > top + rows || !overlay.at.intersects(QRectF(area))) continue;
      if (overlay.clipped) {
        over.save();
        // Smoothed, as the painter it was clipped on was.
        over.setRenderHint(QPainter::Antialiasing);
        over.setClipPath(overlay.clip);
      }
      over.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing, false);
      over.setRenderHints(overlay.hints | QPainter::SmoothPixmapTransform);
      over.setOpacity(std::clamp(overlay.opacity, 0., 1.));
      over.drawImage(overlay.at, overlay.picture);
      if (overlay.clipped) over.restore();
    }
  });
}
// What a raster's blurred letters, and their backgrounds, were left to do while
// it was painted (AnimatedLetterStrokes::jobs). Done by the strip that collected
// them, they went one after another on one core, the other strips long done:
// 870 ms of a frame of a title scaled to six times with a wide stroke. Here
// each uses the cores itself, its strokes and its blur, or, with at least as
// many of them as cores, they are shared out among the cores instead.
void finishLetterJobs(AnimatedLetterStrokes &letters) {
  auto &jobs = letters.jobs;
  if (int(jobs.size()) >= renderWorkerLimit())
    renderParallelEach(int(jobs.size()), [&](int n) { jobs[size_t(n)](); });
  else
    for (auto &job : jobs) job();
  jobs.clear();
}
// Whether a background fades with a letter drawn sharp, whose pixels the
// stroke pass then has to say (layLetterBackdrop).
bool backdropFadesWithLetters(const LetterBackdrop &backdrop) {
  return std::any_of(backdrop.pieces.begin(), backdrop.pieces.end(), [](const LetterBackdrop::Piece &piece) {
    return piece.shown < 1 - 1e-9 && piece.overlay < 0;
  });
}
// A premultiplied pixel's coverage, and the whole pixel made `k` times as
// strong, in the formats a layer's raster comes in.
float coverageAt(const uchar *line, int x, QImage::Format format) {
  switch (format) {
  case QImage::Format_RGBA64_Premultiplied: return reinterpret_cast<const quint16 *>(line)[x * 4 + 3] / 65535.f;
  case QImage::Format_RGBA32FPx4_Premultiplied: return reinterpret_cast<const float *>(line)[x * 4 + 3];
  default: return line[x * 4 + 3] / 255.f; // 8 bits, RGBA or ARGB
  }
}
void strengthen(uchar *line, int x, float k, QImage::Format format) {
  switch (format) {
  case QImage::Format_RGBA64_Premultiplied: {
    auto *p = reinterpret_cast<quint16 *>(line) + x * 4;
    for (int c = 0; c < 4; ++c) p[c] = quint16(p[c] * k + .5f);
    break;
  }
  case QImage::Format_RGBA32FPx4_Premultiplied: {
    auto *p = reinterpret_cast<float *>(line) + x * 4;
    for (int c = 0; c < 4; ++c) p[c] *= k;
    break;
  }
  default:
    for (int c = 0; c < 4; ++c) line[x * 4 + c] = uchar(line[x * 4 + c] * k + .5f);
  }
}
// The letters' backgrounds laid under a raster once its letters and their
// strokes are on it -- letters over strokes over backgrounds -- and before the
// blurred letters go over all of it. Each band of rows paints its backgrounds,
// in their order, on a picture of its own and lays it under itself, on the
// cores at once; a picture of the raster's size painted on one core, as they
// once were, cost as much as the strokes. Every pixel is worked out from the
// backgrounds alone, so the bands meet exactly.
//
// A letter fading in or out fades as one picture with its background, as it
// does with its strokes -- and letters fading together, as one: where a letter
// showing as much as a background's own lies over it, with its strokes, the
// background shows only as much as it would through them whole. Laid under
// the faded letters as it was, a background the strokes hide once the letters
// are in showed through them while they came in, then went. A letter fading
// on its own time covers the others' backgrounds as it comes. Which letter is
// where comes from `owners` (the stroke pass's: which of `letters` each pixel
// went with) or, for a blurred letter, from its own picture.
void layLetterBackdrop(QImage &raster, const LetterBackdrop &backdrop, const std::vector<uint16_t> &owners = {},
                       const AnimatedLetterStrokes *letters = nullptr, QRect within = QRect()) {
  if (backdrop.pieces.empty() || raster.isNull()) return;
  // Only `within` (all of it when null) can show, and only that is laid.
  const QRect area = within.isValid() ? within & raster.rect() : raster.rect();
  if (area.isEmpty()) return;
  const auto format = raster.format();
  const int w = raster.width();
  const bool cuts = letters && (format == QImage::Format_RGBA8888_Premultiplied ||
                                format == QImage::Format_ARGB32_Premultiplied ||
                                format == QImage::Format_RGBA64_Premultiplied ||
                                format == QImage::Format_RGBA32FPx4_Premultiplied);
  const bool ownersKnown = owners.size() == size_t(w) * size_t(raster.height());
  // Where each lands, to leave the bands it misses alone.
  std::vector<QRectF> reach;
  reach.reserve(backdrop.pieces.size());
  for (const auto &piece : backdrop.pieces) {
    QRectF r = piece.picture.isNull() ? piece.at.mapRect(piece.paint->backgroundBox(piece.index)) : piece.onto;
    if (piece.clipped) r &= piece.clip.boundingRect();
    reach.push_back(r.adjusted(-2, -2, 2, 2));
    // A QPainterPath makes the form it is drawn in the first time it is
    // drawn, in the data every copy of it shares, without a lock: filled by
    // two bands at once, both made it and one freed what the other was still
    // drawing -- and the heap with it, in half the runs of a title scaled up
    // as it came in. Made here, before the bands begin, each band only reads it.
    if (piece.picture.isNull() && piece.index >= 0 && piece.index < piece.paint->backgrounds.size())
      readyPathForThreads(piece.paint->backgrounds[piece.index]);
    if (piece.clipped) readyPathForThreads(piece.clip);
  }
  uchar *const bits = raster.bits();
  const qsizetype stride = raster.bytesPerLine();
  constexpr int band = 64;
  const int first = area.top() / band;
  renderParallelEach(area.bottom() / band - first + 1, [&](int n) {
    const int top = (first + n) * band, rows = std::min(band, raster.height() - top);
    const QTransform shift = QTransform::fromTranslate(0, -top);
    QImage under, apart, cover;
    std::optional<QPainter> q;
    for (size_t k = 0; k < backdrop.pieces.size(); ++k) {
      const auto &piece = backdrop.pieces[k];
      if (reach[k].isEmpty() || reach[k].bottom() < top || reach[k].top() > top + rows ||
          !reach[k].intersects(QRectF(area)))
        continue;
      if (!q) {
        under = QImage(w, rows, format);
        if (under.isNull()) return;
        under.fill(Qt::transparent);
        q.emplace(&under);
        q->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
      }
      // A fading letter's own background is painted apart, to be cut where
      // the letter lies over it. Only its own part of the band is cleared
      // and laid, not the whole band again for every one -- all of its part:
      // a picture enlarged into a clip a pixel narrower lands a step off in
      // other columns.
      const QRect span = reach[k].toAlignedRect().translated(0, -top) & QRect(0, 0, w, rows);
      bool fades = cuts && !span.isEmpty() && piece.shown < 1 - 1e-9 &&
                   (piece.overlay >= 0 ? size_t(piece.overlay) < letters->overlays.size() : ownersKnown);
      if (fades && apart.isNull()) apart = QImage(w, rows, format);
      fades = fades && !apart.isNull();
      auto clear = [&](QImage &image) {
        const int bytes = image.depth() / 8;
        for (int y = span.top(); y <= span.bottom(); ++y)
          std::memset(image.scanLine(y) + span.left() * bytes, 0, size_t(span.width()) * bytes);
      };
      std::optional<QPainter> alone;
      if (fades) {
        clear(apart);
        alone.emplace(&apart);
        alone->setRenderHints(q->renderHints());
        alone->setClipRect(span);
      }
      QPainter &onto = fades ? *alone : *q;
      onto.save();
      onto.setTransform(shift);
      if (piece.clipped) onto.setClipPath(piece.clip, fades ? Qt::IntersectClip : Qt::ReplaceClip);
      onto.setOpacity(piece.opacity);
      if (piece.picture.isNull()) {
        onto.setTransform(piece.at * shift);
        piece.paint->drawBackground(onto, piece.index);
      } else {
        onto.setRenderHint(QPainter::SmoothPixmapTransform);
        onto.drawImage(piece.onto, piece.picture);
      }
      onto.restore();
      if (!fades) continue;
      alone->end();
      // How much of the letter, at full strength, lies over each pixel: its
      // own share of what the stroke pass left there, or its blurred picture
      // as layLetterOverlays lays it.
      const AnimatedLetterStrokes::Overlay *mine =
          piece.overlay >= 0 ? &letters->overlays[size_t(piece.overlay)] : nullptr;
      if (mine) {
        if (cover.isNull()) cover = QImage(w, rows, QImage::Format_ARGB32_Premultiplied);
        if (cover.isNull()) return;
        clear(cover);
        QPainter c(&cover);
        c.setClipRect(span);
        c.setRenderHint(QPainter::SmoothPixmapTransform);
        c.translate(0, -top);
        c.drawImage(mine->at, mine->picture);
      }
      const float shown = float(piece.shown);
      for (int y = span.top(); y <= span.bottom(); ++y) {
        uchar *line = apart.scanLine(y);
        const uchar *beneath = bits + (top + y) * stride;
        const uchar *covered = mine ? cover.constScanLine(y) : nullptr;
        const uint16_t *owner = mine ? nullptr : owners.data() + size_t(top + y) * size_t(w);
        for (int x = span.left(); x <= span.right(); ++x) {
          if (coverageAt(line, x, format) <= 0) continue;
          float whole;
          if (covered) whole = covered[x * 4 + 3] / 255.f;
          else {
            const int o = int(owner[x]) - 1;
            if (o < 0 || size_t(o) >= letters->opacity.size() || std::abs(letters->opacity[size_t(o)] - piece.shown) > 1e-9)
              continue;
            whole = std::min(1.f, coverageAt(beneath, x, format) / shown);
          }
          // Under the faded letter it is laid at (1 - shown * whole): left
          // at (1 - whole), as under the letter whole.
          if (whole > 0) strengthen(line, x, whole >= 1 ? 0.f : (1 - whole) / (1 - shown * whole), format);
        }
      }
      q->save();
      q->resetTransform();
      q->drawImage(QPointF(span.topLeft()), apart, QRectF(span));
      q->restore();
    }
    if (!q) return;
    q->end();
    QImage part(bits + top * stride, w, rows, stride, format);
    QPainter put(&part);
    put.setCompositionMode(QPainter::CompositionMode_DestinationOver);
    put.drawImage(0, 0, under);
  });
}
// The particles a stroked text gives off, over its finished raster: over the
// letters, their strokes and their backgrounds, as over a text with no
// strokes. Its strips paint the letters alone and drew none at all; painted
// with the letters, they would go under the strokes built after, and the
// strokes would be measured round them as round letters. Placed as the strip
// at the top places the letters.
void drawParticlesOverRaster(QImage &raster, const Layer &l, const Scene &s, double t, QRectF logical,
                             double rasterScale) {
  if (!l.particlesOn || l.type == "particles" || raster.isNull()) return;
  QPainter q(&raster);
  q.setRenderHint(QPainter::Antialiasing);
  q.setRenderHint(QPainter::TextAntialiasing);
  q.scale(rasterScale, rasterScale);
  q.translate(-logical.left(), -logical.top());
  if (layerHasMainTransform(l)) q.setWorldTransform(layerMainTransform(l), true);
  drawParticles(q, l, s, t);
}
}
// ---------------------------------------------------------------------------
// How far the letters' animations can carry a letter beyond where it stands:
// every animation of the stack at the most it moves, scales and turns, the
// lower ones counted in the scale of those above. The picture of an animated
// text is drawn this much larger, or a letter pushed a long way -- a Position
// of a hundred and eighty, under another one -- was cut off at the edge.
double textAnimationReach(const Layer &l) {
  if (l.type != "text" || !l.characterAnimation.value("enabled").toBool() ||
      l.hiddenEffects.value("Text-Animation").toBool())
    return 0;
  double travel = 0, scale = 1, soft = 0, lean = 0, unit = 1;
  bool turns = false;
  // Tracking spreads a line from its middle: the end letters go furthest.
  const double halfLine = std::max(1., double(l.text.size())) / 2;
  for (const auto &animator : textAnimators(l)) {
    const QJsonObject &a = animator.settings;
    auto number = [&](const char *key, double fallback) { return std::abs(a.value(key).toDouble(fallback)); };
    const bool separate = a.value("separateOut").toBool();
    double moves = std::max(std::hypot(number("x", 0), number("y", 80)),
                            separate ? std::hypot(number("outX", 0), number("outY", 80)) : 0.);
    moves += number("wigglePosition", 0) * 1.5;
    moves += std::max(number("tracking", 0), separate ? number("outTracking", 0) : 0.) * halfLine;
    soft += std::max(number("blur", 0), separate ? number("outBlur", 0) : 0.) * 3 * scale;
    lean = std::max(lean, std::max(number("skew", 0), separate ? number("outSkew", 0) : 0.));
    // A whole word or line turns and grows about its own middle: further out
    // than one letter does.
    const int based = a.value("basedOn").toInt();
    unit = std::max(unit, based == 2 ? halfLine : based == 1 ? 3. : 1.);
    travel += moves * scale;
    double grows = std::max({1., number("scale", 100) / 100., separate ? number("outScale", 100) / 100. : 1.});
    grows *= 1 + number("wiggleScale", 0) / 100.;
    scale *= grows;
    turns = turns || number("rotation", 0) > 0 || (separate && number("outRotation", 0) > 0) || number("wiggleRotation", 0) > 0;
  }
  // A letter grown or turned about its own middle reaches past its own box.
  const double letter = std::max(1., l.fontSize) * unit;
  return travel + letter * (scale - 1) * .75 + (turns ? letter * .5 : 0) + soft +
         letter * std::tan(std::min(lean, 85.) * M_PI / 180.) * .6 + 4;
}
// Where an animated text's letters are at one moment, in the layer's own
// coordinates: each letter's box, strokes included, carried by its pose, its
// blur round it. textAnimationReach is the furthest they can ever go, and a
// picture that large -- a wiggling word turned seventy degrees reached six
// hundred pixels past its letters on every side -- was drawn, shadowed and
// converted whole every frame for letters covering a tenth of it: 85 ms of a
// title card. Null when the letters cannot be measured this way (backgrounds
// travel with them, or the animation is off).
namespace {
// By the layer and its id: a layer copied whole and changed keeps its id.
using TextBoundsMemo = std::map<std::tuple<const Layer *, QString, double, int>, QRectF>;
QRectF animatedTextBoundsNow(const Layer &l, double t, int fps);
}
struct RenderFrameContext {
  std::mutex guard;
  TextBoundsMemo textBounds;
  // How many times each layer is sampled for its own motion blur, and whether
  // its letters are what moves: asked by the count of moving layers, by the
  // warming, and by every band that meets the layer.
  std::map<std::tuple<const Layer *, QString, double, double, int, int, int>, std::pair<int, bool>> blurSamples;
};
TextBoundsMemoScope::TextBoundsMemoScope() : owned(!renderFrame) {
  if (owned) renderFrame = std::make_shared<RenderFrameContext>();
}
TextBoundsMemoScope::~TextBoundsMemoScope() {
  if (owned) renderFrame.reset();
}
QRectF animatedTextBounds(const Layer &l, double t, int fps) {
  RenderFrameContext *const memo = renderFrame.get();
  if (!memo || l.type != "text") return animatedTextBoundsNow(l, t, fps);
  const auto key = std::make_tuple(&l, l.id, t, fps);
  {
    std::lock_guard<std::mutex> lock(memo->guard);
    const auto found = memo->textBounds.find(key);
    if (found != memo->textBounds.end()) return found->second;
  }
  const QRectF bounds = animatedTextBoundsNow(l, t, fps);
  std::lock_guard<std::mutex> lock(memo->guard);
  memo->textBounds.emplace(key, bounds);
  return bounds;
}
namespace {
// Each letter's box with its strokes round it, in the letter's own place before
// its animation moves it -- Miter tips included. Worked out once for a text and
// its strokes, and kept: the letters' shapes do not change while they move,
// and a moving title asks for its box thousands of times a frame (every band,
// every motion-blur sample, every check of where it lands). Finding the
// corners again each time was most of what a page of animated lyrics cost.
std::shared_ptr<const std::vector<QRectF>> letterStrokeBoxes(const Layer &l, const QVector<AnimatedGlyph> &glyphs,
                                                             double strokeReach, bool mitred) {
  QByteArray key = textDocumentKey(l);
  key += QByteArray::number(strokeReach, 'g', 12) + (mitred ? "m" : "r") + QByteArray::number(glyphs.size());
  static QMutex guard;
  static QCache<QByteArray, std::shared_ptr<const std::vector<QRectF>>> kept(512);
  {
    QMutexLocker lock(&guard);
    if (const auto *hit = kept.object(key)) return *hit;
  }
  auto boxes = std::make_shared<std::vector<QRectF>>();
  boxes->reserve(size_t(glyphs.size()));
  for (const auto &g : glyphs) {
    const double reach = strokeReach + (g.outline.style() != Qt::NoPen ? g.outline.widthF() : 0.);
    boxes->push_back(mitred ? mitredStrokeBox(g.path, reach) : g.path.boundingRect().adjusted(-reach, -reach, reach, reach));
  }
  std::shared_ptr<const std::vector<QRectF>> done = boxes;
  QMutexLocker lock(&guard);
  kept.insert(key, new std::shared_ptr<const std::vector<QRectF>>(done));
  return done;
}
QRectF animatedTextBoundsNow(const Layer &l, double t, int fps) {
  if (l.type != "text" || !l.characterAnimation.value("enabled").toBool() ||
      l.hiddenEffects.value("Text-Animation").toBool() || layerHasLetterBackground(l))
    return {};
  auto doc = cachedTextDocument(l);
  const auto glyphs = characterGlyphs(*doc, l);
  const QSizeF size = doc->size();
  const QPointF origin(-size.width() / 2, -size.height() / 2 + textLeadingOffset(l, *doc));
  if (glyphs.isEmpty()) return QRectF(origin + QPointF(size.width() / 2, size.height() / 2), QSizeF(1, 1));
  const auto poses = animatedCharacterPoses(glyphs, l, t, fps);
  // Every stroke drawn round a letter, at its full width: an outside stroke
  // reaches all of it, a centred one half, but this is only a margin.
  double strokeReach = 2;
  for (const auto &stroke : layerTextStrokes(l)) strokeReach += std::max(0., stroke.width);
  if (vectorEffectStroke(l)) strokeReach += std::max(0., effectStrokePen(l).widthF());
  // Miter corners run out past that, as far as their tips.
  const auto stack = textStrokeStack(l);
  const bool mitred = std::any_of(stack.begin(), stack.end(), [](const TextStrokeStyle &stroke) {
    return stroke.join == 0 && stroke.mode != 0 && stroke.width > 0;
  });
  const auto boxes = letterStrokeBoxes(l, glyphs, strokeReach, mitred);
  QRectF ink;
  for (int i = 0; i < glyphs.size(); ++i) {
    const auto &g = glyphs[i];
    const auto &pose = poses[i];
    if (pose.opacity <= 0) continue;
    QTransform local;
    local.translate(g.center.x(), g.center.y());
    local = pose.matrix * local;
    local.translate(-g.center.x(), -g.center.y());
    const double soft = pose.blur * 3;
    const QRectF placed = local.mapRect((*boxes)[size_t(i)]).adjusted(-soft, -soft, soft, soft);
    ink = ink.isNull() ? placed : ink.united(placed);
  }
  // Every letter faded out: nothing to draw, a box of nothing.
  if (ink.isNull()) return QRectF(origin + QPointF(size.width() / 2, size.height() / 2), QSizeF(1, 1));
  return ink.translated(origin);
}
}  // namespace
// The same over a motion-blur shutter about t -- the letters wherever any
// sample puts them -- for deciding where a moving text lands in a frame. Null
// when that cannot be said (the letters still, or carrying something they do
// not measure), and the layer's full reach is used instead.
QRectF movingLettersBounds(const Layer &l, const Scene &s, double t) {
  if (l.type != "text" || l.particlesOn || layerHasPixelEffect(l) || characterPoseAtRest(l, t)) return {};
  QRectF all;
  for (double at : {t - .25, t, t + .25}) {
    const QRectF box = animatedTextBounds(l, at, s.fps);
    if (box.isNull()) return {};
    all = all.isNull() ? box : all.united(box);
  }
  return all;
}
// Letters dressed: a background behind each letter, and another layer's
// picture as the texture of the letters or of their backgrounds.
// ---------------------------------------------------------------------------
static QJsonObject lettersOf(const Layer &l) {
  return l.type == "text" ? l.surfaceEffects.value("Letters").toObject() : QJsonObject();
}
bool layerHasLetterBackground(const Layer &l) { return lettersOf(l).value("background").toBool(); }
// Every layer a Letters texture setting names: the one chosen, or, when the
// choice is animated, every one its keys move between ("<key>List").
static QStringList letterTextureIds(const QJsonObject &o, const QString &key) {
  QStringList ids;
  if (!o.value(key).toString().isEmpty()) ids << o.value(key).toString();
  for (const auto &entry : o.value(key + "List").toArray())
    if (!entry.toString().isEmpty()) ids << entry.toString();
  ids.removeDuplicates();
  return ids;
}
bool layerUsesLetterTexture(const Layer &l) {
  const QJsonObject o = lettersOf(l);
  return !letterTextureIds(o, "letterTexture").isEmpty() ||
         (o.value("background").toBool() && !letterTextureIds(o, "backgroundTexture").isEmpty());
}
// A Letters number at layer frame t: its curve when it is animated
// ("Letters/<key>" among the layer's extra curves), its setting when not.
static double letterNumber(const Layer &l, const QString &key, double fallback, double t) {
  const int curve = layerFindExtra(l, "Letters/" + key);
  if (curve >= 0) return l.at(curve).at(t);
  return lettersOf(l).value(key).toDouble(fallback);
}
// The largest a Letters number is anywhere on its curve, for a box that must
// hold every frame of it.
static double letterNumberMost(const Layer &l, const QString &key, double fallback) {
  const int curve = layerFindExtra(l, "Letters/" + key);
  if (curve < 0) return lettersOf(l).value(key).toDouble(fallback);
  const Property &p = l.at(curve);
  double most = p.base;
  for (const auto &k : p.keys) most = std::max(most, k.value);
  return p.hasExpression() ? std::max(most, 500.) : most;
}
double letterBackgroundReach(const Layer &l, const QSizeF &textSize) {
  const QJsonObject o = lettersOf(l);
  if (!o.value("background").toBool()) return 0;
  const double padding = std::clamp(letterNumberMost(l, "padding", 10), 0., 500.);
  // Letters made the same size, or round, reach past a narrow letter's own box.
  const double wider = letterNumberMost(l, "shape", 0) > 0 ? l.fontSize * .7 : 0;
  // A texture's picture in every box, laid larger than the box: half the
  // growth of the largest box it can be in -- a letter's, or a word's or a
  // line's, which can be as long as the text.
  double larger = 0;
  const double most = std::clamp(letterNumberMost(l, "textureScale", 100), 1., 2000.) / 100.;
  if (o.value("textureEach").toBool() && most > 1) {
    const double letter = l.fontSize * 1.6 + 2 * padding;
    const double box = o.value("per").toInt(0) == 0
                           ? letter
                           : std::max({textSize.width(), textSize.height(), letter}) + 2 * padding;
    larger = (most - 1) / 2 * box;
  }
  return padding + wider + larger + 2;
}
namespace {
const Layer *layerById(const Scene &s, const QString &id) {
  for (const auto &candidate : s.layers)
    if (candidate.id == id) return &candidate;
  return nullptr;
}
void writeCurve(QDataStream &stream, const Property &p) {
  stream << p.base << qint64(p.keys.size()) << p.expression;
  for (const auto &k : p.keys) stream << k.frame << k.value << k.ease;
}
// Everything that decides a layer's own picture, and the frame when that
// picture moves.
QByteArray texturePictureKey(const Layer &l, const Scene &s, double frame) {
  QByteArray content;
  QDataStream stream(&content, QIODevice::WriteOnly);
  stream << l.id << l.type << l.path << l.image.cacheKey() << l.text << l.richHtml << l.font << l.fontSize << l.color
         << l.bold << l.fontStyle << l.fauxBold << l.fauxItalic << l.capitals << l.leading << l.tracking << l.align
         << l.paragraphWidth << l.shapeKind
         << l.shapeFill << l.shapeClosed
         << l.strokeWidth << l.strokeColor << l.strokeMode << l.blur << l.glow << l.shadow << l.mainAnchorX
         << l.mainAnchorY << l.mainScaleX << l.mainScaleY << l.mainRotation << l.start << l.duration << s.width
         << s.height << s.processingDepth;
  for (const auto &object : {l.shapePoints, l.masks})
    stream << QJsonDocument(object).toJson(QJsonDocument::Compact);
  for (const auto &object : {l.surfaceEffects, l.appliedEffects, l.hiddenEffects, l.particles, l.characterAnimation})
    stream << QJsonDocument(object).toJson(QJsonDocument::Compact);
  bool moves = l.type == "video" || layerEmitsParticles(l) || layerHasAnimatedEffect(l) ||
               (l.characterAnimation.value("enabled").toBool() && !l.hiddenEffects.value("Text-Animation").toBool());
  for (const auto &p : l.p) { writeCurve(stream, p); moves = moves || p.varies(); }
  for (const auto &p : l.extraCurves) { writeCurve(stream, p); moves = moves || p.varies(); }
  if (moves) stream << frame;
  return QCryptographicHash::hash(content, QCryptographicHash::Sha1);
}
QMutex &textureGuard() { static QMutex guard; return guard; }
QCache<QByteArray, QImage> &textureCache() {
  static QCache<QByteArray, QImage> cache(262144); // KiB, 256 MiB
  return cache;
}
thread_local int textureDepth = 0;
}  // namespace
QByteArray letterTextureKey(const Layer &l, const Scene &s, double t) {
  QByteArray key;
  const QJsonObject o = lettersOf(l);
  for (const char *field : {"letterTexture", "backgroundTexture"})
    for (const QString &id : letterTextureIds(o, field)) {
      const Layer *source = id == l.id ? nullptr : layerById(s, id);
      key += source ? texturePictureKey(*source, s, l.start + t) : QByteArray("-");
    }
  return key;
}
QByteArray layerPictureKey(const Layer &l, const Scene &s, double frame) { return texturePictureKey(l, s, frame); }
QImage layerTexturePicture(const Layer &source, const Scene &s, double frame) {
  // A texture whose layer is textured in turn is followed once, no further: two
  // text layers wearing each other would otherwise draw each other for ever.
  if (textureDepth >= 2) return {};
  const QByteArray key = texturePictureKey(source, s, frame);
  {
    QMutexLocker lock(&textureGuard());
    if (const QImage *kept = textureCache().object(key)) return *kept;
  }
  const QRectF box = layerContentBounds(source, s);
  if (box.isEmpty() || !std::isfinite(box.width()) || !std::isfinite(box.height())) return {};
  const QSize full(std::max(1, int(std::ceil(box.width()))), std::max(1, int(std::ceil(box.height()))));
  const double shrink = std::min(1., 2048. / std::max(full.width(), full.height()));
  const QSize size(std::max(1, int(std::round(full.width() * shrink))), std::max(1, int(std::round(full.height() * shrink))));
  // The layer alone, unmoved, one layer pixel to a pixel, on nothing.
  Scene only;
  only.width = full.width();
  only.height = full.height();
  only.previewDraft = s.previewDraft;
  only.finalRender = s.finalRender;
  only.fps = s.fps;
  only.frames = s.frames;
  only.background = Qt::transparent;
  only.processingDepth = 8;
  Layer target = source;
  target.visible = true;
  target.groupId.clear();
  target.parentId.clear();
  target.parentBind = QTransform();
  target.trackMatteId.clear();
  target.threeD = false;
  target.motionBlur = false;
  target.repeaterEnabled = false;
  target.blendMode = "normal";
  target.isolated = false;
  const auto fixed = [&](int slot, double value) { target.p[size_t(slot)] = Property{}; target.p[size_t(slot)].base = value; };
  fixed(0, -source.mainX - box.left());
  fixed(1, -source.mainY - box.top());
  fixed(2, 1);
  fixed(3, 1);
  fixed(4, 0);
  fixed(5, 1);
  fixed(AnchorX, 0);
  fixed(AnchorY, 0);
  only.layers = {target};
  QImage picture;
  {
    // Drawn on this thread alone, whatever thread asks: a frame being drawn in
    // bands must not have its plan replaced by this smaller one's. Nor may it
    // take the text that asked for it for its own: the letters' raster stage
    // (characterStrokesDeferred...) is still under way on this thread, and a
    // texture's own letters went into that text's collection.
    struct Scope {
      bool previous, deferred, apart;
      std::vector<QPainterPath> *outlines;
      AnimatedLetterStrokes *animated;
      LetterBackdrop *backdrop;
      Scope()
          : previous(inRenderWorker), deferred(characterStrokesDeferred), apart(characterLettersApart),
            outlines(characterStrokeLetters), animated(characterStrokeAnimated), backdrop(characterStrokeBackdrop) {
        inRenderWorker = true;
        ++textureDepth;
        characterStrokesDeferred = characterLettersApart = false;
        characterStrokeLetters = nullptr;
        characterStrokeAnimated = nullptr;
        characterStrokeBackdrop = nullptr;
      }
      ~Scope() {
        inRenderWorker = previous;
        --textureDepth;
        characterStrokesDeferred = deferred;
        characterLettersApart = apart;
        characterStrokeLetters = outlines;
        characterStrokeAnimated = animated;
        characterStrokeBackdrop = backdrop;
      }
    } scope;
    picture = renderCPU(only, frame, false, size).convertToFormat(QImage::Format_ARGB32_Premultiplied);
  }
  if (picture.isNull()) return {};
  QMutexLocker lock(&textureGuard());
  textureCache().insert(key, new QImage(picture), std::max(1, int(picture.sizeInBytes() / 1024)));
  return picture;
}
namespace {
// A picture laid over `area` so that it covers it, centred, at `scale`.
QBrush textureBrush(const QImage &picture, const QRectF &area, double scale) {
  if (picture.isNull() || area.isEmpty()) return {};
  const double k = std::max(area.width() / picture.width(), area.height() / picture.height()) * scale;
  QBrush brush(picture);
  QTransform place;
  place.translate(area.center().x(), area.center().y());
  place.scale(k, k);
  place.translate(-picture.width() / 2., -picture.height() / 2.);
  brush.setTransform(place);
  return brush;
}
}  // namespace
// The letters' dress for this frame (`frame` the composition's), in the text
// document's coordinates, one background per letter in the order the letters
// are drawn. Nothing when the layer asks for neither.
static std::optional<LetterPaint> letterPaintFor(const Layer &l, const Scene &s, double frame, QTextDocument &doc) {
  const QJsonObject o = lettersOf(l);
  const bool background = o.value("background").toBool();
  const bool letterTextured = !letterTextureIds(o, "letterTexture").isEmpty();
  if (!background && !letterTextured) return {};
  const auto glyphs = characterGlyphs(doc, l);
  if (glyphs.isEmpty()) return {};
  const double t = frame - l.start;
  const double scale = std::clamp(letterNumber(l, "textureScale", 100, t), 1., 2000.) / 100.;
  auto textureOf = [&](const QString &id, const QRectF &area) -> QBrush {
    const Layer *source = id.isEmpty() || id == l.id ? nullptr : layerById(s, id);
    return source ? textureBrush(layerTexturePicture(*source, s, frame), area, scale) : QBrush();
  };
  // The texture a setting gives at this frame: the layer chosen, or -- when
  // the choice is animated -- the one its curve is at, crossfading into the
  // next between keys. `plain` stands for "None". NoBrush when neither side
  // has a texture, so the letters keep their own colours.
  auto textureAt = [&](const QString &key, const QRectF &area, const QBrush &plain) -> QBrush {
    const int curve = layerFindExtra(l, "Letters/" + key);
    if (curve < 0) {
      const QString id = o.value(key).toString();
      return id.isEmpty() ? QBrush() : textureOf(id, area);
    }
    const QJsonArray list = o.value(key + "List").toArray();
    if (list.isEmpty()) return {};
    const double v = std::clamp(l.at(curve).at(t), 0., double(list.size() - 1));
    const int i = std::clamp(int(std::floor(v)), 0, int(list.size()) - 1);
    const double f = v - i;
    auto one = [&](int k) {
      const QString id = list[k].toString();
      return id.isEmpty() ? QBrush() : textureOf(id, area);
    };
    const QBrush a = one(i);
    if (f < 1e-4 || i + 1 >= list.size()) return a;
    const QBrush b = one(i + 1);
    if (a.style() == Qt::NoBrush && b.style() == Qt::NoBrush) return {};
    // The two laid over each other at the weights between the keys, once, as
    // a picture of the area: the letters are then filled once, strokes and all.
    const double longest = std::max(area.width(), area.height());
    const double k = longest > 1024 ? 1024 / longest : 1;
    QImage mixed(std::max(1, int(std::ceil(area.width() * k))), std::max(1, int(std::ceil(area.height() * k))),
                 QImage::Format_ARGB32_Premultiplied);
    mixed.fill(Qt::transparent);
    {
      QPainter q(&mixed);
      q.setRenderHint(QPainter::SmoothPixmapTransform);
      q.scale(k, k);
      q.translate(-area.topLeft());
      q.fillRect(area, a.style() == Qt::NoBrush ? plain : a);
      q.setOpacity(f);
      q.fillRect(area, b.style() == Qt::NoBrush ? plain : b);
    }
    QBrush brush(mixed);
    QTransform place;
    place.translate(area.left(), area.top());
    place.scale(1 / k, 1 / k);
    brush.setTransform(place);
    return brush;
  };
  LetterPaint paint;
  QRectF letters;
  for (const auto &g : glyphs) letters = letters.isNull() ? g.path.boundingRect() : letters.united(g.path.boundingRect());
  if (background) {
    // The lines, so every letter's background on a line stands on the same
    // top and bottom, whatever the letter's own height.
    std::vector<QRectF> lines;
    for (auto block = doc.begin(); block.isValid(); block = block.next()) {
      const QTextLayout *layout = block.layout();
      if (!layout) continue;
      for (int i = 0; i < layout->lineCount(); ++i) {
        const QTextLine line = layout->lineAt(i);
        lines.push_back(QRectF(layout->position().x() + line.x(), layout->position().y() + line.y(), line.width(),
                               line.ascent() + line.descent()));
      }
    }
    // A background for each letter, each word or each line: the letters of a
    // word or a line share one box, the width of them all. Each box is
    // carried by its first letter, whose animation moves it.
    const int per = std::clamp(o.value("per").toInt(0), 0, 2);
    std::vector<QRectF> cells;
    std::map<int, size_t> cellOf; // a word or line, to its box
    double widest = 0, tallest = 0;
    for (int i = 0; i < glyphs.size(); ++i) {
      const auto &g = glyphs[i];
      const QRectF ink = g.path.boundingRect();
      if (ink.isEmpty()) continue; // nothing to stand behind
      QRectF line(ink.left(), ink.top(), ink.width(), ink.height());
      double nearest = 1e300;
      for (const auto &candidate : lines) {
        const double off = g.center.y() < candidate.top() ? candidate.top() - g.center.y()
                           : g.center.y() > candidate.bottom() ? g.center.y() - candidate.bottom() : 0;
        if (off < nearest) { nearest = off; line = candidate; }
      }
      const QRectF cell(ink.left(), line.top(), ink.width(), line.height());
      const int group = per == 1 ? g.word : per == 2 ? g.line : -1;
      if (group >= 0) {
        if (auto found = cellOf.find(group); found != cellOf.end()) {
          cells[found->second] = cells[found->second].united(cell);
          continue;
        }
        cellOf[group] = cells.size();
      }
      cells.push_back(cell);
      paint.backgroundCarrier.push_back(i);
    }
    for (const auto &cell : cells) {
      widest = std::max(widest, cell.width());
      tallest = std::max(tallest, cell.height());
    }
    const double padding = std::clamp(letterNumber(l, "padding", 10, t), 0., 500.);
    const double padX = padding, padY = padding * .6;
    const double round = std::clamp(letterNumber(l, "round", 30, t), 0., 100.) / 100.;
    // The shape as a number, so a key can move between shapes: 0 fits each
    // letter, 1 gives every letter the widest one's size, 2 is a circle, and
    // between them it changes over -- fitted boxes widening into tiles, tiles
    // rounding into circles.
    const double shape = std::clamp(letterNumber(l, "shape", 0, t), 0., 2.);
    const double toTile = std::min(1., shape), toCircle = std::max(0., shape - 1);
    for (const auto &cell : cells) {
      const double w = cell.width() + (widest - cell.width()) * toTile + 2 * padX;
      const double h = cell.height() + 2 * padY;
      const double d = std::max(widest + 2 * padX, tallest + 2 * padY);
      const double bw = w + (d - w) * toCircle, bh = h + (d - h) * toCircle;
      const double r0 = round * std::min(w, h) / 2;
      const double r = r0 + (d / 2 - r0) * toCircle;
      const QPointF middle(cell.center().x(), cell.top() + cell.height() / 2);
      QPainterPath path;
      path.addRoundedRect(QRectF(middle.x() - bw / 2, middle.y() - bh / 2, bw, bh), std::min(r, bw / 2), std::min(r, bh / 2));
      paint.backgrounds.push_back(path);
    }
    QRectF area;
    for (const auto &path : paint.backgrounds) area = area.isNull() ? path.boundingRect() : area.united(path.boundingRect());
    // The colour first: "None" among animated textures crossfades to it.
    QBrush colourBrush;
    {
      // The colour, from its three curves when it is animated.
      QColor colour(o.value("color").toString("#1d1d27"));
      if (layerFindExtra(l, "Letters/colorR") >= 0)
        colour = QColor::fromRgbF(float(std::clamp(letterNumber(l, "colorR", 0, t) / 255., 0., 1.)),
                                  float(std::clamp(letterNumber(l, "colorG", 0, t) / 255., 0., 1.)),
                                  float(std::clamp(letterNumber(l, "colorB", 0, t) / 255., 0., 1.)));
      colourBrush = QBrush(colour);
    }
    if (o.value("textureEach").toBool()) {
      // Every background its own copy of the texture's picture, in the
      // picture's own shape, laid into its box at the texture's scale.
      auto pictureOf = [&](const QString &id) {
        const Layer *source = id.isEmpty() || id == l.id ? nullptr : layerById(s, id);
        return source ? layerTexturePicture(*source, s, frame) : QImage();
      };
      const int curve = layerFindExtra(l, "Letters/backgroundTexture");
      if (curve < 0) {
        const QImage one = pictureOf(o.value("backgroundTexture").toString());
        if (!one.isNull()) paint.backgroundPictures.push_back({one, 1.});
      } else if (const QJsonArray list = o.value("backgroundTextureList").toArray(); !list.isEmpty()) {
        // Between keys one crossfades into the next; "None" is the box in its colour.
        const double v = std::clamp(l.at(curve).at(t), 0., double(list.size() - 1));
        const int i = std::clamp(int(std::floor(v)), 0, int(list.size()) - 1);
        const double f = v - i;
        paint.backgroundPictures.push_back({pictureOf(list[i].toString()), 1 - f});
        if (f > 1e-4 && i + 1 < list.size()) paint.backgroundPictures.push_back({pictureOf(list[i + 1].toString()), f});
        if (std::all_of(paint.backgroundPictures.begin(), paint.backgroundPictures.end(),
                        [](const auto &entry) { return entry.first.isNull(); }))
          paint.backgroundPictures.clear();
      }
      paint.backgroundPictureScale = scale;
      paint.backgroundBrush = colourBrush;
    } else {
      paint.backgroundBrush = textureAt("backgroundTexture", area, colourBrush);
      if (paint.backgroundBrush.style() == Qt::NoBrush) paint.backgroundBrush = colourBrush;
    }
    paint.backgroundOpacity = std::clamp(letterNumber(l, "opacity", 100, t), 0., 100.) / 100.;
  }
  if (letterTextured) paint.letterBrush = textureAt("letterTexture", letters, QBrush(l.color));
  return paint;
}
namespace {
thread_local const ImageCardHook *g_imageCard = nullptr;
thread_local const ShapeCardHook *g_shapeCard = nullptr;
// A painter a card draws for: neither an image nor a picture being recorded.
bool paintsOnCard(const QPainter &p) {
  const QPaintDevice *device = p.device();
  return device && device->devType() != QInternal::Image && device->devType() != QInternal::Picture;
}
}
ImageCardScope::ImageCardScope(const ImageCardHook *hook) : previous(g_imageCard) { g_imageCard = hook; }
ImageCardScope::~ImageCardScope() { g_imageCard = previous; }
ShapeCardScope::ShapeCardScope(const ShapeCardHook *hook) : previous(g_shapeCard) { g_shapeCard = hook; }
ShapeCardScope::~ShapeCardScope() { g_shapeCard = previous; }
thread_local const LitFaceHook *g_litFaceCard = nullptr;
LitFaceScope::LitFaceScope(const LitFaceHook *hook) : previous(g_litFaceCard) { g_litFaceCard = hook; }
LitFaceScope::~LitFaceScope() { g_litFaceCard = previous; }
thread_local const BlendCardHook *g_blendCard = nullptr;
BlendCardScope::BlendCardScope(const BlendCardHook *hook) : previous(g_blendCard) { g_blendCard = hook; }
BlendCardScope::~BlendCardScope() { g_blendCard = previous; }
thread_local const MotionBlurCardHook *g_blurCard = nullptr;
MotionBlurCardScope::MotionBlurCardScope(const MotionBlurCardHook *hook) : previous(g_blurCard) { g_blurCard = hook; }
MotionBlurCardScope::~MotionBlurCardScope() { g_blurCard = previous; }
thread_local const MatteCardHook *g_matteCard = nullptr;
MatteCardScope::MatteCardScope(const MatteCardHook *hook) : previous(g_matteCard) { g_matteCard = hook; }
MatteCardScope::~MatteCardScope() { g_matteCard = previous; }
bool painterClipIsDeviceRect(const QPainter &p, QRect *rect) {
  const QPaintDevice *device = p.device();
  const QRect whole = device ? QRect(0, 0, device->width(), device->height()) : QRect();
  if (!p.hasClipping()) {
    if (rect) *rect = whole;
    return true;
  }
  // A rectangle has the area of its bounds; anything else cut out of them less.
  const QPainterPath clip = p.worldTransform().map(p.clipPath());
  const QRectF bounds = clip.boundingRect();
  // The clip every pass sets -- four corners round its bounds, each side
  // upright or level -- known for that without cutting it into polygons: on a
  // card drawing 600 small shapes, that took 2.2 us of the 7.7 each shape took.
  if (clip.elementCount() == 5) {
    const double tolerance = 1e-6 * std::max({1., std::abs(bounds.left()), std::abs(bounds.right()),
                                              std::abs(bounds.top()), std::abs(bounds.bottom())});
    auto near = [&](double a, double b) { return std::abs(a - b) <= tolerance; };
    bool around = near(clip.elementAt(4).x, clip.elementAt(0).x) && near(clip.elementAt(4).y, clip.elementAt(0).y);
    for (int i = 0; i < 4 && around; ++i) {
      const QPainterPath::Element a = clip.elementAt(i), b = clip.elementAt(i + 1);
      around = !b.isCurveTo() && (near(a.x, bounds.left()) || near(a.x, bounds.right())) &&
               (near(a.y, bounds.top()) || near(a.y, bounds.bottom())) && (near(a.x, b.x) || near(a.y, b.y));
    }
    if (around) {
      if (rect) *rect = bounds.adjusted(.001, .001, -.001, -.001).toAlignedRect() & whole;
      return true;
    }
  }
  double covered = 0;
  for (const QPolygonF &polygon : clip.toFillPolygons()) {
    double twice = 0;
    for (int i = 0; i + 1 < polygon.size(); ++i)
      twice += polygon[i].x() * polygon[i + 1].y() - polygon[i + 1].x() * polygon[i].y();
    covered += twice / 2;
  }
  if (std::abs(std::abs(covered) - bounds.width() * bounds.height()) > 1) return false;
  // Mapped into the layer's turn and back, a whole-pixel edge comes out a hair
  // to either side of where it was, and would round out a pixel too far.
  if (rect) *rect = bounds.adjusted(.001, .001, -.001, -.001).toAlignedRect() & whole;
  return true;
}
namespace {
// The painter a pass on a card is drawing a layer with, and its clip as the
// pass found it on the way in (passClipIsDeviceRect): set for each layer no
// mask cuts, and cleared when one does. Asked of the painter instead, each of
// 600 small shapes spent 0.9 us of its 4.6 working out a clip that never
// changed between them.
struct PassClip { const QPainter *painter = nullptr; bool rect = false; QRect device; };
thread_local PassClip g_passClip;
struct PassClipScope {
  PassClip previous;
  PassClipScope(const QPainter *painter, bool rect, QRect device) : previous(g_passClip) {
    g_passClip = {painter, rect, device};
  }
  ~PassClipScope() { g_passClip = previous; }
  void forget() { g_passClip.painter = nullptr; }
};
}
bool passClipIsDeviceRect(const QPainter &p, QRect *rect) {
  if (g_passClip.painter != &p) return painterClipIsDeviceRect(p, rect);
  if (g_passClip.rect && rect) *rect = g_passClip.device;
  return g_passClip.rect;
}
// The clip a pass drawing one layer alone on a card starts from. It is nearly
// always in whole pixels, and is set as such: Qt's QPainter::clipPath() works a
// clip out quickly only when it was given as a QRect, and one given as a QRectF
// through a region -- under a turned layer a staircase, about 0.2 ms each time
// -- which the card's own drawing then took for a clip it cannot cut to, and
// left to Qt's. Sixteen motion-blur samples of the promo's two turning discs
// spent 3.8 ms of every frame there. The pixels cut away are the same.
static void clipAlonePass(QPainter &q, bool clipped, const QRectF &clip) {
  if (!clipped) return;
  const QRect whole = clip.toRect();
  if (QRectF(whole) == clip) q.setClipRect(whole);
  else q.setClipRect(clip);
}
// A shape layer's fills, in the order they go down: the shape, then its
// stroke cut to the shape (Inside) or to outside it (Outside). For a filled
// Outside stroke the complete outline goes down first and the fill covers its
// inner half: clipping two separately antialiased edges against each other
// left a dark one-pixel seam at large stroke widths.
static ShapeFills shapeFillsOf(const Layer &l, const QPainterPath &path, const QBrush &fillBrush, bool filled,
                               bool closed, const QPainterPath &outline) {
  ShapeFills fills;
  if (!outline.isEmpty() && closed && l.strokeMode == 1 && filled) {
    fills.push_back({outline, QBrush(l.strokeColor), {}, false});
    fills.push_back({path, fillBrush, {}, false});
    return fills;
  }
  if (filled) fills.push_back({path, fillBrush, {}, false});
  if (!outline.isEmpty()) {
    ShapeFill stroke{outline, QBrush(l.strokeColor), {}, closed && (l.strokeMode == 0 || l.strokeMode == 1)};
    if (closed && l.strokeMode == 0) stroke.clip = path;
    else if (closed && l.strokeMode == 1) {
      stroke.clip.setFillRule(Qt::OddEvenFill);
      const auto bounds = outline.controlPointRect().adjusted(-2, -2, 2, 2);
      stroke.clip.addRect(bounds);
      stroke.clip.addPath(path);
    }
    fills.push_back(stroke);
  }
  return fills;
}
ShapeFills shapeLayerFills(const Layer &l, const Scene &s, double frame) {
  if (l.type != "shape" && !parametricShape(l)) return {};
  return shapeFillsOf(l, shapeGeometry(l, frame), layerDrawingBrush(l, layerBounds(l, s)), l.shapeClosed && l.shapeFill,
                      l.shapeClosed && !shapeTrimActive(l, frame), shapeStrokeOutline(l, s, frame));
}
static void drawLayer(QPainter &p, const Layer &l, const Scene &s,
                      double frame) {
  if (l.type == "empty" || l.type == "camera" || l.type == "light" || l.type == "audio" || l.type == "effect") return;
  if (l.type == "particles") { drawParticles(p, l, s, frame); return; }
  auto box = layerBounds(l, s);
  const QBrush drawingBrush = layerDrawingBrush(l, box);
  p.setPen(Qt::NoPen);
  p.setBrush(drawingBrush);
  if (l.type == "shape" || parametricShape(l)) {
    const auto path=shapeGeometry(l,frame);
    const bool filled=l.shapeClosed && l.shapeFill;
    const bool closed=l.shapeClosed && !shapeTrimActive(l,frame);
    const ShapeFills fills = shapeFillsOf(l, path, drawingBrush, filled, closed, shapeStrokeOutline(l, s, frame));
    // On a card, covered there by the exact area of each pixel inside, as
    // the rasterizer covers them here: through Qt's GL engine their edges took
    // eight levels from multisampling, up to 75/255 off this picture.
    if (g_shapeCard && *g_shapeCard && paintsOnCard(p) && (*g_shapeCard)(p, l, &fills)) return;
    for (const ShapeFill &fill : fills) {
      if (!fill.clipped) {
        p.fillPath(fill.path, fill.brush);
        continue;
      }
      p.save();
      p.setClipPath(fill.clip, Qt::IntersectClip);
      p.fillPath(fill.path, fill.brush);
      p.restore();
    }
    return;
  }
  if (l.type == "text") {
    auto doc = cachedTextDocument(l);
    p.save();
    // layerBounds includes outward Character strokes; the document itself is
    // still centred on its unpadded typographic size.
    const QSizeF textSize = doc->size();
    p.translate(-textSize.width() / 2,
                -textSize.height() / 2 + textLeadingOffset(l, *doc));
    // Handwriting masks whatever is drawn below, so the letters keep their
    // colour, stroke and per-character animation while the pen uncovers them.
    // Once every letter is written (and none taken away) there is nothing to
    // mask: drawn as it stands, its strokes are exactly the still text's.
    const bool writing = handwritingEnabled(l) &&
                         !(handwritingProgress(l, frame) >= 1 && characterProgress(l, 7, frame, 0, 0) <= 0);
    if (writing) {
      // The letters as written so far, and round them as far as their strokes
      // reach outside: clipped to the letters alone, an Outside stroke was cut
      // away whole, even once every letter was written.
      double reach = 0;
      for (const auto &stroke : layerTextStrokes(l))
        reach += stroke.mode == 1 ? std::max(0., stroke.width) : stroke.mode == 2 ? std::max(0., stroke.width) / 2 : 0;
      if (vectorEffectStroke(l)) {
        const double width = std::max(0., effectStrokePen(l).widthF());
        const int mode = effectStrokeMode(l);
        reach += mode == 1 ? width : mode == 2 ? width / 2 : 0;
      }
      // A mitred corner reaches past the stroke's width: half as far again at
      // the strokes' miter limit.
      const double grow = reach > 0 ? reach * 1.6 + 1.5 : 0;
      const QPainterPath written = cachedHandwritingMask(*doc, l, frame, grow);
      if (written.isEmpty()) { p.restore(); return; } // the pen has not started
      p.setClipPath(written, Qt::IntersectClip);
    }
    // Each letter's background, and the textures in letters and backgrounds.
    std::optional<LetterPaint> letters;
    if (l.surfaceEffects.contains("Letters")) letters = letterPaintFor(l, s, l.start + frame, *doc);
    auto letterFill = [&](const AnimatedGlyph &g) {
      return letters && letters->letterBrush.style() != Qt::NoBrush ? letters->letterBrush : QBrush(g.fill);
    };
    // Per-character animation owns the drawing when it is on, stroke included.
    if (l.characterAnimation["enabled"].toBool() && !l.hiddenEffects["Text-Animation"].toBool()) {
      drawAnimatedCharacters(p, *doc, l, frame, s.fps, letters ? &*letters : nullptr,
                             characterStrokesDeferred || characterLettersApart ? characterStrokeAnimated : nullptr);
      p.restore(); return;
    }
    // Standing still, the backgrounds go down first, under every letter and
    // stroke -- or, while the raster stage builds the strokes from the letters'
    // distance, into the backdrop laid under all of it after.
    if (letters && !letters->backgrounds.isEmpty()) {
      if (!characterStrokesDeferred) {
        p.save();
        p.setOpacity(p.opacity() * letters->backgroundOpacity);
        for (int b = 0; b < letters->backgrounds.size(); ++b) letters->drawBackground(p, b);
        p.restore();
      } else if (characterStrokeBackdrop) {
        const auto paint = std::make_shared<const LetterPaint>(*letters);
        for (int b = 0; b < letters->backgrounds.size(); ++b)
          characterStrokeBackdrop->add(p, paint, b, p.worldTransform(), p.opacity() * letters->backgroundOpacity);
      }
    }
    const bool vectorStroke=vectorEffectStroke(l);
    const bool vectorFill=vectorEffectGradient(l) || vectorEffectFill(l);
    if(vectorStroke && characterStrokesDeferred) {
      // The raster stage strokes them from their distance (textStrokeStack):
      // only the letters, in their fill, and their outlines for its corners.
      const auto glyphs=characterGlyphs(*doc,l);
      QPainterPath silhouette;
      silhouette.setFillRule(Qt::WindingFill);
      for(const auto &g:glyphs) silhouette.addPath(g.path);
      if(vectorFill) p.fillPath(silhouette,layerDrawingBrush(l,QRectF(QPointF(),box.size())));
      else for(const auto &g:glyphs) p.fillPath(g.path,letterFill(g));
      if(characterStrokeLetters)
        for(const auto &g:glyphs) characterStrokeLetters->push_back(p.combinedTransform().map(g.path));
      p.restore();return;
    }
    if(vectorStroke || vectorFill) {
      QPainterPath silhouette;
      const auto glyphs=characterGlyphs(*doc,l);
      if(vectorStroke) for(const auto &g:glyphs) {
        auto shape=g.path;
        if(g.outline.style()!=Qt::NoPen && g.outline.widthF()>0) {
          QPainterPathStroker stroke;stroke.setWidth(g.outline.widthF()*(l.strokeMode==2?1:2));
          stroke.setJoinStyle(Qt::SvgMiterJoin);
          stroke.setMiterLimit(2);
          if(l.strokeMode!=0) shape=shape.united(stroke.createStroke(g.path));
        }
        silhouette=silhouette.united(shape);
      }
      else {
        // A fill needs no union: winding covers overlapping glyphs on its own.
        silhouette.setFillRule(Qt::WindingFill);
        for(const auto &g:glyphs) silhouette.addPath(g.path);
      }
      // Color/bitmap glyphs lack vector contours: preserve their normal content.
      auto settings=l.surfaceEffects["Stroke"].toObject();
      const int mode=std::clamp(settings["mode"].toInt(1),0,2);
      QPen outline(QColor(settings["color"].toString("#ffffff")),std::clamp(settings["width"].toDouble(4),0.,200.));
      if(vectorStroke && mode==1) drawTextStrokePass(p,silhouette,Qt::NoBrush,outline,mode,false);
      // Stroke Only: the letters are left out, the stroke drawn on its own.
      if(vectorStroke && effectStrokeOnly(l)) {}
      else if(vectorFill) p.fillPath(silhouette,layerDrawingBrush(l,QRectF(QPointF(),box.size())));
      else doc->drawContents(&p);
      if(vectorStroke && mode!=1) drawTextStrokePass(p,silhouette,Qt::NoBrush,outline,mode,false);
      p.restore();return;
    }
    {
      auto glyphs=characterGlyphs(*doc,l);
      const auto textStrokes = layerTextStrokes(l);
      bool stroked=!textStrokes.empty() ||
          std::any_of(glyphs.begin(),glyphs.end(),[](const auto &g){return g.outline.style()!=Qt::NoPen && g.outline.widthF()>0;});
      if(stroked && characterStrokesDeferred && !textStrokes.empty()) {
        // The raster stage builds these strokes from the letters' distance
        // field once the whole raster exists, so only the letters are painted
        // here. Painting the strokes as well would put the old wide-pen shapes
        // underneath the correct ones.
        for (const auto &g : glyphs) {
          p.fillPath(g.path, letterFill(g));
          if (characterStrokeLetters)
            characterStrokeLetters->push_back(p.combinedTransform().map(g.path));
        }
      } else if(stroked) {
        // Every Character stroke is a whole-text pass.  This keeps a broad
        // rear stroke behind narrower strokes and, crucially, keeps every
        // outline behind every glyph fill where letters overlap.
        QPainterPath silhouette;
        silhouette.setFillRule(Qt::WindingFill);
        for (const auto &g : glyphs) silhouette.addPath(g.path);
        // Broad cumulative strokes go down first, then narrower ones restore
        // the visible inner bands. This is the same cheap pen path for every
        // entry; no QPainterPath boolean is performed while a value is dragged.
        // Stroke Only leaves the letters out, and keeps each outside stroke to
        // the outside, where the letters no longer cover its inner half.
        const bool hollow = textStrokeOnly(l) && !textStrokes.empty();
        for (int strokeIndex = int(textStrokes.size()) - 1; strokeIndex >= 0; --strokeIndex)
          drawTextStrokeStackPass(p, silhouette, textStrokes, strokeIndex, hollow);
        if (textStrokes.empty())
          for (const auto &g : glyphs)
            drawTextStrokePass(p, g.path, Qt::NoBrush, g.outline, l.strokeMode, false);
        if (!hollow)
          for (const auto &g : glyphs)
            drawTextFillAfterStrokes(p, g.path, letterFill(g), textStrokes,
                                     textStrokes.empty() ? g.outline : QPen(), l.strokeMode);
        // Inside and Center strokes belong above the fill. Paint them only
        // after every glyph fill so overlapping letters cannot cover them.
        for (int strokeIndex = int(textStrokes.size()) - 1; strokeIndex >= 0; --strokeIndex)
          drawTextStrokeStackInsidePass(p, silhouette, textStrokes, strokeIndex);
      } else if ((l.richHtml.isEmpty() || l.fauxBold || l.fauxItalic) && !glyphs.isEmpty()) {
        // Record scalable outlines, not font-atlas text commands. QPicture is
        // prepared on workers and replayed at different scales on the GL
        // thread; ordinary text must not depend on that atlas across threads.
        for (const auto &g : glyphs) p.fillPath(g.path, letterFill(g));
      } else doc->drawContents(&p);
    }
    p.restore();
  } else if (l.type == "fill" || l.type == "rect")
    p.drawRect(box);
  else if (l.type == "circle")
    p.drawEllipse(box);
  else if (!l.image.isNull()) {
    // On a card, from a texture it keeps: through the card's painter a picture
    // is uploaded again whenever Qt's own small texture cache has let it go,
    // which a few full-size pictures and a frame's changing rasters made every
    // frame -- most of what a picture layer cost there.
    if (g_imageCard && *g_imageCard && p.device() && p.device()->devType() != QInternal::Image &&
        p.device()->devType() != QInternal::Picture && (*g_imageCard)(p, box, l.image))
      return;
    p.drawImage(box, l.image);
  }
}
QPicture prepareLayerDrawing(const Layer &layer, const Scene &scene, double localFrame) {
  QPicture picture;
  QPainter painter(&picture);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::TextAntialiasing);
  drawLayer(painter, layer, scene, localFrame);
  painter.end();
  return picture;
}
Property &Layer::at(int index) {
  if (index >= 0 && index < PropertyCount) return p[size_t(index)];
  const int extra = index - PropertyCount;
  if (extra >= 0 && extra < int(extraCurves.size())) return extraCurves[size_t(extra)];
  // Total on purpose. A timeline row can outlive the rebuild that made it by
  // one repaint, and an index that names nothing should do nothing rather than
  // reach into whatever is next in memory. The scratch curve is never drawn and
  // never saved.
  static thread_local Property scratch;
  scratch = Property();
  return scratch;
}
const Property &Layer::at(int index) const { return const_cast<Layer *>(this)->at(index); }
int layerPropertyCount(const Layer &l) { return PropertyCount + int(l.extraCurves.size()); }
Property *layerProperty(Layer &l, int index) {
  if (index >= 0 && index < PropertyCount) return &l.p[index];
  const int extra = index - PropertyCount;
  if (extra >= 0 && extra < int(l.extraCurves.size())) return &l.extraCurves[size_t(extra)];
  return nullptr;
}
const Property *layerProperty(const Layer &l, int index) {
  return layerProperty(const_cast<Layer &>(l), index);
}
QString layerExtraId(const Layer &l, int index) {
  const int extra = index - PropertyCount;
  return extra >= 0 && extra < l.extraCurveIds.size() ? l.extraCurveIds[extra] : QString();
}
int layerFindExtra(const Layer &l, const QString &id) {
  const int at = l.extraCurveIds.indexOf(id);
  return at < 0 ? -1 : PropertyCount + at;
}
int layerAddExtra(Layer &l, const QString &id, double initial) {
  const int existing = layerFindExtra(l, id);
  if (existing >= 0) return existing;
  Property curve;
  curve.base = initial;
  l.extraCurves.push_back(curve);
  l.extraCurveIds << id;
  return PropertyCount + int(l.extraCurves.size()) - 1;
}
void layerRemoveExtra(Layer &l, const QString &id) {
  const int at = l.extraCurveIds.indexOf(id);
  if (at < 0 || at >= int(l.extraCurves.size())) return;
  l.extraCurves.erase(l.extraCurves.begin() + at);
  l.extraCurveIds.removeAt(at);
}
void AdditionalTransform::resolveProps(const Layer &l) const {
  propAnchorX = layerFindExtra(l, "Xform/" + id + "/AnchorX");
  propAnchorY = layerFindExtra(l, "Xform/" + id + "/AnchorY");
  propAnchorZ = layerFindExtra(l, "Xform/" + id + "/AnchorZ");
  propPosX = layerFindExtra(l, "Xform/" + id + "/PositionX");
  propPosY = layerFindExtra(l, "Xform/" + id + "/PositionY");
  propPosZ = layerFindExtra(l, "Xform/" + id + "/PositionZ");
  propScaleX = layerFindExtra(l, "Xform/" + id + "/ScaleX");
  propScaleY = layerFindExtra(l, "Xform/" + id + "/ScaleY");
  propScaleZ = layerFindExtra(l, "Xform/" + id + "/ScaleZ");
  propRotation = layerFindExtra(l, "Xform/" + id + "/Rotation");
  propRotationX = layerFindExtra(l, "Xform/" + id + "/RotationX");
  propRotationY = layerFindExtra(l, "Xform/" + id + "/RotationY");
}
AdditionalTransform *Layer::findAdditionalTransform(const QString &targetId) {
  for (auto &at : additionalTransforms)
    if (at.id == targetId) return &at;
  return nullptr;
}
const AdditionalTransform *Layer::findAdditionalTransform(const QString &targetId) const {
  for (const auto &at : additionalTransforms)
    if (at.id == targetId) return &at;
  return nullptr;
}
bool Layer::isAdditionalTransformPercent(int prop) const {
  const QString id = layerExtraId(*this, prop);
  if (id.endsWith("/opacity")) return true;
  for (const auto &at : additionalTransforms) {
    at.resolveProps(*this);
    if (prop == at.propScaleX || prop == at.propScaleY || prop == at.propScaleZ)
      return true;
  }
  return false;
}
void addAdditionalTransform(Layer &l, const QString &name) {
  AdditionalTransform at;
  at.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const int count = int(l.additionalTransforms.size()) + 2;
  at.name = name.isEmpty() ? QString("Transform %1").arg(count) : name;
  at.expanded = true;

  layerAddExtra(l, "Xform/" + at.id + "/AnchorX", 0.0);
  layerAddExtra(l, "Xform/" + at.id + "/AnchorY", 0.0);
  layerAddExtra(l, "Xform/" + at.id + "/AnchorZ", 0.0);
  layerAddExtra(l, "Xform/" + at.id + "/PositionX", 0.0);
  layerAddExtra(l, "Xform/" + at.id + "/PositionY", 0.0);
  layerAddExtra(l, "Xform/" + at.id + "/PositionZ", 0.0);
  layerAddExtra(l, "Xform/" + at.id + "/ScaleX", 1.0);
  layerAddExtra(l, "Xform/" + at.id + "/ScaleY", 1.0);
  layerAddExtra(l, "Xform/" + at.id + "/ScaleZ", 1.0);
  layerAddExtra(l, "Xform/" + at.id + "/Rotation", 0.0);
  layerAddExtra(l, "Xform/" + at.id + "/RotationX", 0.0);
  layerAddExtra(l, "Xform/" + at.id + "/RotationY", 0.0);

  at.resolveProps(l);
  l.additionalTransforms.push_back(at);
}
void removeAdditionalTransform(Layer &l, const QString &id) {
  for (auto it = l.additionalTransforms.begin(); it != l.additionalTransforms.end(); ++it) {
    if (it->id == id) {
      layerRemoveExtra(l, "Xform/" + id + "/AnchorX");
      layerRemoveExtra(l, "Xform/" + id + "/AnchorY");
      layerRemoveExtra(l, "Xform/" + id + "/AnchorZ");
      layerRemoveExtra(l, "Xform/" + id + "/PositionX");
      layerRemoveExtra(l, "Xform/" + id + "/PositionY");
      layerRemoveExtra(l, "Xform/" + id + "/PositionZ");
      layerRemoveExtra(l, "Xform/" + id + "/ScaleX");
      layerRemoveExtra(l, "Xform/" + id + "/ScaleY");
      layerRemoveExtra(l, "Xform/" + id + "/ScaleZ");
      layerRemoveExtra(l, "Xform/" + id + "/Rotation");
      layerRemoveExtra(l, "Xform/" + id + "/RotationX");
      layerRemoveExtra(l, "Xform/" + id + "/RotationY");
      l.additionalTransforms.erase(it);
      break;
    }
  }
  for (auto &at : l.additionalTransforms) at.resolveProps(l);
}
QString nameFromText(const QString &text) {
  const QString line = text.split('\n').value(0).simplified();
  if (line.isEmpty()) return {};
  // Long enough to tell two layers apart, short enough not to swamp the column.
  constexpr int most = 40;
  return line.size() <= most ? line : line.left(most - 1) + QChar(0x2026);
}
void followTextName(Layer &l) {
  if (l.type != "text" || l.namedByHand) return;
  const QString taken = nameFromText(l.text);
  if (!taken.isEmpty()) l.name = taken;
}
QStringList layerEffectOrder(const Layer &l) {
  static const QStringList defaults{
      "Fill","Gradient","Tint","Exposure","Levels","Curves",
      "Brightness & Contrast","HSL","Vibrance","Color Balance",
      "Three-Way Color Corrector","Temperature & Tint","Black & White","RGB Split","Film Grain","Vignette","Noise","Turbulent Noise","Displacement Map","Linear Wipe","Radial Wipe",
      "Stroke","Drop Shadow",
      "Glow","Gaussian Blur"};
  QStringList order;
  for (auto value : l.effectOrder) {
    const auto name = value.toString();
    if (name.isEmpty() || order.contains(name)) continue;
    // A recorded name is kept when it is one of ours, or when the layer is
    // actually carrying it. That second case is how an effect whose name this
    // engine cannot know -- one supplied by a plugin -- takes its place in the
    // stack and can be reordered among the built-in ones.
    if (defaults.contains(name) || (name == "Text-Animation" && l.type == "text") || l.appliedEffects[name].toBool()) order << name;
  }
  // What the layer did record wins, in the order it recorded it. Whatever it
  // did not -- an older project, or an effect added to the application since
  // it was saved -- follows in the default order rather than displacing it.
  for (const auto &name : defaults)
    if (!order.contains(name)) order << name;
  if (l.type == "text" && !order.contains("Text-Animation"))
    order << "Text-Animation";
  // Anything applied that nobody recorded an order for -- an effect just added
  // from outside -- goes after the built-in chain. Text Animation is part of
  // drawing the layer rather than of the pixel chain, so it stays out of it.
  for (auto it = l.appliedEffects.constBegin(); it != l.appliedEffects.constEnd(); ++it)
    if (it.value().toBool() && it.key() != "Text-Animation" && !order.contains(it.key()))
      order << it.key();
  return order;
}
// The same chain the CPU would run, described as passes for the card. Empty
// means some effect in it has no form the card can take, and the whole chain
// belongs on the CPU: a chain half here and half there would pay a round trip
// across the bus between every pair of effects, which is what made adding an
// effect feel arbitrary.
QVector<GpuPass> layerEffectPasses(const Layer &l, QRectF box, double scale,
                                   QSize size, bool premultiplied) {
  // Glow, blur and shadow in linear light are the processor's: the card's
  // passes work on the encoded values.
  if (effectsInLinearLight() && (l.glow || l.shadow || l.blur)) return {};
  QVector<GpuPass> chain;
  const bool started = premultiplied;
  bool alphaTouched = false;
  auto want = [&](bool wantPremultiplied) {
    if (premultiplied == wantPremultiplied) return;
    chain += gpuAlphaPass(wantPremultiplied);
    premultiplied = wantPremultiplied;
  };
  const bool vectorStroke = vectorEffectStroke(l);
  for (const auto &name : layerEffectOrder(l)) {
    const bool applied = l.appliedEffects[name].toBool() && !l.hiddenEffects[name].toBool();
    if (name == "Fill") {
      if (!applied || vectorEffectFill(l)) continue;
      auto s = l.surfaceEffects["Fill"].toObject();
      QColor colour(s["color"].toString("#ffffff"));
      const double opacity = std::clamp(s["opacity"].toDouble(100) / 100., 0., 1.);
      want(false); // a fill is about coverage, so it reasons in straight alpha
      GpuPass pass;
      pass.op = 14;
      pass.a = {float(colour.redF()), float(colour.greenF()), float(colour.blueF()),
                float(colour.alphaF() * opacity)};
      pass.input = GpuPass::Current;
      pass.endsEffect = true;
      chain.push_back(pass);
      alphaTouched = true;
    } else if (name == "Gradient") {
      if (!applied || vectorEffectGradient(l)) continue;
      auto s = l.surfaceEffects["Gradient"].toObject();
      QColor from(s["start"].toString("#ff6a00")), to(s["end"].toString("#6633ff"));
      const double amount = std::clamp(s["amount"].toDouble(100)/100., 0., 1.);
      want(false); // a gradient tints coverage, so it reasons in straight alpha
      GpuPass pass;
      pass.op = 1;
      pass.params = {float(box.center().x()), float(box.center().y()),
                     float(s["angle"].toDouble()), float(to.blueF())};
      pass.a = {float(from.redF()), float(from.greenF()), float(from.blueF()),
                float(s["type"].toInt()==1 ? -amount : amount)};
      pass.b = {float(to.redF()), float(to.greenF()), float(box.width()), float(box.height())};
      pass.input = GpuPass::Current;
      pass.endsEffect = true;
      chain.push_back(pass);
    } else if (const PackEffect *pack = packEffect(name)) {
      // An effect from a pack runs on the card through its own shader, among
      // the host's passes; one with no shader sends the whole chain to the CPU.
      if (!applied) continue;
      const auto values = packEffectValues(l, *pack, currentEffectFrame());
      if (packEffectInert(*pack, values)) continue;
      const auto passes = packEffectPasses(*pack, values, 8, size, box, scale, currentEffectSeconds(), currentEffectFrame());
      if (passes.isEmpty()) return {};
      want(false); // pack effects work in straight alpha
      chain += passes;
    } else if (name == "Stroke") {
      if (!applied || vectorStroke) continue;
      auto s = l.surfaceEffects["Stroke"].toObject();
      const double width = std::clamp(s["width"].toDouble(4), 0., 200.) * scale;
      const int mode = std::clamp(s["mode"].toInt(1), 0, 2);
      QColor colour(s["color"].toString("#ffffff"));
      if (width <= 0 || colour.alpha() == 0) continue;
      want(false); // a stroke reasons about coverage, so it needs straight alpha
      const auto stroke = gpuStrokePasses(size, width, mode, colour);
      if (stroke.isEmpty()) return {};
      chain += stroke;
      alphaTouched = true;
    } else if (name == "Drop Shadow" || name == "Glow") {
      if(l.hiddenEffects[name].toBool())continue;
      const double radius = name == "Glow" ? l.glow : l.shadow;
      if (!radius) continue;
      want(true); // filtering only behaves on premultiplied colour
      const auto halo = gpuHaloPasses(size, radius*scale,
                             name == "Glow" ? QColor() : QColor(0,0,0,200),
                             name == "Glow" ? QPointF() : QPointF(5,7)*scale);
      if (halo.isEmpty()) return {};
      chain += halo;
      alphaTouched = true;
    } else if (name == "Gaussian Blur") {
      if(l.hiddenEffects[name].toBool())continue;
      if (!l.blur) continue;
      want(true);
      auto blur = gpuSoftenPasses(size, l.blur*scale);
      if (blur.isEmpty()) return {};
      // The soften passes leave the picture they were given as the current
      // one (a halo reads both); a blur replaces it. Unmarked, a pass after
      // the blur -- the alpha conversion back, when the chain started in
      // straight alpha -- read the picture from before it, and the blur was
      // thrown away.
      blur.back().endsEffect = true;
      chain += blur;
      alphaTouched = true;
    }
  }
  if (chain.isEmpty()) return {};
  want(started); // hand it back the way it arrived, so the readback needs no work
  return chain;
}
// Whether splitting the effects into every colour pass first, then shadow,
// glow and blur (which lets the colour passes be kept apart from the blurs and
// reused) still runs them in the layer's own order. It does not when the layer
// puts a colour pass after one of those three: a Fill under a Glow would run
// before it, and the glow would then grow from the faded fill.
bool preSpatialSplitKeepsOrder(const Layer &l) {
  bool spatialSeen = false;
  for (const auto &name : layerEffectOrder(l)) {
    if (name == "Drop Shadow" || name == "Glow" || name == "Gaussian Blur") {
      const bool on = name == "Drop Shadow" ? l.shadow != 0 : name == "Glow" ? l.glow != 0 : l.blur != 0;
      if (on && !l.hiddenEffects[name].toBool()) spatialSeen = true;
      continue;
    }
    if (spatialSeen && l.appliedEffects[name].toBool() && !l.hiddenEffects[name].toBool()) return false;
  }
  return true;
}
QImage applyPreSpatialEffects(QImage image, const Layer &l, QRectF box, double scale) {
  const auto outputFormat = image.format();
  const bool vectorStroke = vectorEffectStroke(l);
  auto straight = [&] { image = image.convertToFormat(QImage::Format_RGBA32FPx4); };
  const QSet<QString> graded = gradeLeadingEightBit(image, l, currentEffectFrame(), currentEffectSeconds(), box, scale,
                                                    [&](const QString &name) {
    return (name == "Fill" && vectorEffectFill(l)) || (name == "Gradient" && vectorEffectGradient(l)) ||
           (name == "Stroke" && vectorStroke) || name == "Drop Shadow" || name == "Glow" || name == "Gaussian Blur";
  });
  for (const auto &name : layerEffectOrder(l)) {
    if (graded.contains(name)) continue;
    const bool applied = l.appliedEffects[name].toBool() && !l.hiddenEffects[name].toBool();
    if (name == "Fill") { if (applied && !vectorEffectFill(l)) { straight(); fillPass(image, l); } }
    else if (name == "Gradient") { if (applied && !vectorEffectGradient(l)) { straight(); gradientPass(image, l, box); } }
    else if (const PackEffect *pack = packEffect(name)) {
      if (applied) {
        const auto values = packEffectValues(l, *pack, currentEffectFrame());
        if (!packEffectInert(*pack, values))
          runPackEffectInPlace(image, *pack, values, box, scale, currentEffectSeconds(), currentEffectFrame(), &l);
      }
    }
    else if (name == "Stroke") { if (applied && !vectorStroke) { straight(); strokePass(image, l, scale); } }
  }
  return image.convertToFormat(outputFormat);
}
QImage applySpatialEffects(QImage image, const Layer &l, QRectF box, double scale) {
  const auto outputFormat = image.format();
  auto premultiplied = [&] { image = image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied); };
  for (const auto &name : layerEffectOrder(l)) {
    if (name == "Drop Shadow") { if (l.shadow && !l.hiddenEffects[name].toBool()) { premultiplied(); shadowPass(image, l, scale); } }
    else if (name == "Glow") { if (l.glow && !l.hiddenEffects[name].toBool()) { premultiplied(); glowPass(image, l, scale); } }
    else if (name == "Gaussian Blur") { if (l.blur && !l.hiddenEffects[name].toBool()) { premultiplied(); blurPass(image, l, scale); } }
  }
  return image.convertToFormat(outputFormat);
}
// The layer whose raster the stage below is making, and its source drawn at
// another time: set around that layer's effect chain, for a host effect that
// reads the frames around the one it draws (an echo, a frame blend).
struct LayerOtherTimes {
  const Layer *layer = nullptr;
  int fps = 30;
  bool draft = false; // the scene's previewDraft
  std::function<QImage(double t)> source;
  std::function<QImage(const QString &id, bool stretched)> otherLayer; // HostEffectRender::otherLayer
};
thread_local const LayerOtherTimes *g_otherTimes = nullptr;
struct LayerOtherTimesScope {
  const LayerOtherTimes *was;
  explicit LayerOtherTimesScope(const LayerOtherTimes *now) : was(g_otherTimes) { g_otherTimes = now; }
  ~LayerOtherTimesScope() { g_otherTimes = was; }
};
// How far the host's effects on this layer draw beyond its picture of
// `content` size (HostLayerEffects::reach), held to the size of the frame: what
// its raster grows by, and how far past its bounds a band of the frame has to
// look for it. 0 where it has none.
static double hostEffectReach(const Layer &l, const Scene &s, double t, QSizeF content) {
  const auto *host = hostLayerEffects();
  if (!host || !host->reach || !host->active || !host->active(l)) return 0;
  const double reach = host->reach(l, t, content);
  return reach > 0 ? std::min(reach, double(std::max(s.width, s.height))) : 0.;
}
// A video layer's footage at a time of its own, for the same: decoded by a
// reader this thread keeps for the layer, which reads on from one frame to the
// next rather than seeking for each.
static QImage footageAt(const Layer &l, double t, int fps) {
  thread_local std::map<QString, std::unique_ptr<NativeMedia>> readers;
  if (!readers.count(l.id) && readers.size() >= 8) readers.clear();
  auto &reader = readers[l.id];
  try {
    if (!reader) reader = std::make_unique<NativeMedia>();
    return footageFrame(*reader, l.path, layerFootageTime(l, t, fps), l.frameBlend == 1);
  } catch (...) {
    readers.erase(l.id);
    return {};
  }
}
QImage applyLayerEffects(QImage image, const Layer &l, QRectF box, double scale, double time, const QString &stopBefore) {
  const auto outputFormat = image.format();
  const int depth = image.depth() <= 32 ? 8 : image.depth() == 64 ? 16 : 32;
  const auto *host = hostLayerEffects();
  const bool hostEffect = host && host->active && host->active(l);
  const bool vectorStroke = vectorEffectStroke(l);
  // One submission for the whole chain when the card can take all of it: the
  // raster goes up once and comes back once, and the conversions between
  // straight and premultiplied alpha happen there as passes rather than here as
  // walks over the whole image.
  //
  // Only when that is the quicker here, for this chain at about this size: a
  // grade or two on a small raster is done by the processor before the picture
  // has been to the card and back, a wide blur the other way round, and which
  // way round depends on the machine. Each chain learns it (route_choice.h).
  RouteChoice *processorRoute = nullptr;
  const qint64 pixels = qint64(image.width()) * image.height();
  if (!hostEffect && canUseGpuPixels()) {
    const bool premultiplied = image.format() == QImage::Format_RGBA32FPx4_Premultiplied ||
                               image.format() == QImage::Format_RGBA8888_Premultiplied ||
                               image.format() == QImage::Format_RGBA64_Premultiplied;
    const auto passes = layerEffectPasses(l, box, scale, image.size(), premultiplied);
    if (!passes.isEmpty()) {
      QByteArray kind = "Layer effects:";
      for (const auto &name : layerEffectOrder(l))
        if (l.appliedEffects[name].toBool() && !l.hiddenEffects[name].toBool()) kind += ' ' + name.toUtf8();
      RouteChoice &route = routeFor(kind);
      if (route.card(pixels)) {
        QElapsedTimer clock;
        clock.start();
        // Handed over in the depth it already has: widening it here would undo
        // the point of uploading eight bit rasters as eight bits.
        auto rendered = gpuRunPasses(image, passes);
        if (!rendered.isNull() && rendered.size() == image.size()) {
          route.took(pixels, true, clock.nsecsElapsed() / 1e6);
          return rendered.convertToFormat(outputFormat);
        }
        route.refused(pixels);
      } else processorRoute = &route;
    }
  }
  const ProcessorTiming timing(processorRoute, pixels);
  auto straight = [&] { image = image.convertToFormat(QImage::Format_RGBA32FPx4); };
  auto premultiplied = [&] { image = image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied); };
  // Colour grades at the head of the stack run on the 8-bit pixels directly.
  const QSet<QString> graded = gradeLeadingEightBit(image, l, currentEffectFrame(), currentEffectSeconds(), box, scale,
                                                    [&](const QString &name) {
    return (name == "Fill" && vectorEffectFill(l)) || (name == "Gradient" && vectorEffectGradient(l)) ||
           (name == "Stroke" && vectorStroke);
  });
  QStringList order = layerEffectOrder(l);
  if (!stopBefore.isEmpty()) order = order.mid(0, std::max(0, int(order.indexOf(stopBefore))));
  // What one step of the chain does: nothing, a pixel-by-pixel pack effect, or
  // anything else. Decided by the same tests the loop below applies.
  enum Step { Nothing, PerPixel, Other };
  auto stepOf = [&](const QString &name, PackStep *pack = nullptr) {
    if (graded.contains(name)) return Nothing;
    const bool applied = l.appliedEffects[name].toBool() && !l.hiddenEffects[name].toBool();
    if (name == "Fill") return applied && !vectorEffectFill(l) ? Other : Nothing;
    if (name == "Gradient") return applied && !vectorEffectGradient(l) ? Other : Nothing;
    if (const PackEffect *effect = packEffect(name)) {
      if (!applied) return Nothing;
      auto values = packEffectValues(l, *effect, currentEffectFrame());
      if (packEffectInert(*effect, values)) return Nothing;
      if (!packEffectPerPixel(*effect)) return Other;
      if (pack) *pack = PackStep{effect, std::move(values)};
      return PerPixel;
    }
    if (name == "Stroke") return applied && !vectorStroke ? Other : Nothing;
    if (name == "Drop Shadow") return l.shadow && !l.hiddenEffects[name].toBool() ? Other : Nothing;
    if (name == "Glow") return l.glow && !l.hiddenEffects[name].toBool() ? Other : Nothing;
    if (name == "Gaussian Blur") return l.blur && !l.hiddenEffects[name].toBool() ? Other : Nothing;
    return applied && hostEffect && host->apply ? Other : Nothing;
  };
  for (int at = 0; at < order.size(); ++at) {
    const QString &name = order[at];
    if (graded.contains(name)) continue;
    // From here to the end, nothing but pixel-by-pixel effects: they run as one,
    // a strip at a time, straight into the format the chain hands back.
    if (stepOf(name) == PerPixel) {
      std::vector<PackStep> steps;
      bool tail = true;
      for (int next = at; next < order.size() && tail; ++next) {
        PackStep step;
        const Step kind = stepOf(order[next], &step);
        if (kind == Other) tail = false;
        else if (kind == PerPixel) steps.push_back(std::move(step));
      }
      if (tail && runPerPixelPacks(image, outputFormat, steps, box, scale, currentEffectSeconds(), currentEffectFrame(), &l))
        return image;
    }
    const bool applied = l.appliedEffects[name].toBool() && !l.hiddenEffects[name].toBool();
    if (name == "Fill") { if (applied && !vectorEffectFill(l)) { straight(); fillPass(image, l); } }
    else if (name == "Gradient") { if (applied && !vectorEffectGradient(l)) { straight(); gradientPass(image, l, box); } }
    else if (const PackEffect *pack = packEffect(name)) {
      if (applied) {
        const auto values = packEffectValues(l, *pack, currentEffectFrame());
        if (!packEffectInert(*pack, values))
          runPackEffectInPlace(image, *pack, values, box, scale, currentEffectSeconds(), currentEffectFrame(), &l);
      }
    }
    // A vector stroke is already part of the drawing, not of these pixels.
    else if (name == "Stroke") { if (applied && !vectorStroke) { straight(); strokePass(image, l, scale); } }
    else if (name == "Drop Shadow") { if (l.shadow && !l.hiddenEffects[name].toBool()) { premultiplied(); shadowPass(image, l, scale); } }
    else if (name == "Glow") { if (l.glow && !l.hiddenEffects[name].toBool()) { premultiplied(); glowPass(image, l, scale); } }
    else if (name == "Gaussian Blur") { if (l.blur && !l.hiddenEffects[name].toBool()) { premultiplied(); blurPass(image, l, scale); } }
    else if (applied && hostEffect && host->apply) {
      // Keep the requested SDK depth, even if preceding built-ins used float
      // intermediates. The next effect receives this plugin's actual output.
      image=image.convertToFormat(processingFormat(depth,true));
      // The picture as it reaches this effect at another time: the layer drawn
      // then, through the effects before this one.
      HostEffectRender render;
      const bool ours = g_otherTimes && g_otherTimes->layer == &l;
      render.draft = ours && g_otherTimes->draft;
      render.content = box;
      if (ours) render.otherLayer = g_otherTimes->otherLayer;
      if (ours && g_otherTimes->source) {
        const LayerOtherTimes *times = g_otherTimes;
        const QImage::Format here = image.format();
        render.otherFrame = [times, &l, box, scale, name, here](double when) -> QImage {
          QImage source = times->source(when);
          if (source.isNull()) return {};
          EffectTimeScope effectTime(when / std::max(1, times->fps), when);
          return applyLayerEffects(std::move(source), l, box, scale, when, name).convertToFormat(here);
        };
      }
      host->apply(l,time,name,image,scale,render);
    }
  }
  return image.convertToFormat(outputFormat);
}
// Everything that decides what an effected raster contains. A layer whose
// content, effects and framing are unchanged produces the same pixels, and in a
// composition of many layers most of them are unchanged from one frame to the
// next -- which is what a scrub or a playback spends its time redoing.
//
// Masks are absent from raster-cache identities on purpose. Hard masks remain
// painter clips, while feathered masks are applied to a disposable copy after
// the cached effect raster has been found. Editing a mask therefore never
// rebuilds the layer or its effects.
namespace {
// Set once, from the UI thread, before any render. Read from the workers, which
// is safe because nothing writes it again.
const HostLayerEffects *hostEffectsRegistered = nullptr;

struct MaskPathCacheEntry {
  quint64 pointsHash = 0;
  int pointsCount = 0;
  double expansion = 0;
  bool inverted = false;
  QRectF bounds;
  QPainterPath path;
};

static quint64 hashPointsArray(const QJsonArray &arr) {
  quint64 h = 14695981039346656037ULL;
  for (const auto &v : arr) {
    const auto o = v.toObject();
    const auto p = o["p"].toArray();
    const auto in = o["in"].toArray();
    const auto out = o["out"].toArray();
    auto mix = [&](double d) {
      quint64 u = 0;
      std::memcpy(&u, &d, sizeof(u));
      h ^= u;
      h *= 1099511628211ULL;
    };
    if (p.size() >= 2) { mix(p[0].toDouble()); mix(p[1].toDouble()); }
    if (in.size() >= 2) { mix(in[0].toDouble()); mix(in[1].toDouble()); }
    if (out.size() >= 2) { mix(out[0].toDouble()); mix(out[1].toDouble()); }
    if (o["break"].toBool()) { h ^= 0x55; h *= 1099511628211ULL; }
  }
  return h;
}

static std::mutex g_maskPathMutex;
static std::vector<MaskPathCacheEntry> g_maskPathCache;

static QPainterPath expandContourPath(const QJsonArray &points, double expansion, bool closed) {
  if (points.isEmpty()) return QPainterPath();
  if (std::abs(expansion) < 1e-5) return vectorPath(points, closed);

  auto getPt = [](const QJsonObject &n, const QString &key) -> QPointF {
    const auto a = n[key].toArray();
    return QPointF(a.size() > 0 ? a.at(0).toDouble() : 0.,
                   a.size() > 1 ? a.at(1).toDouble() : 0.);
  };

  std::vector<std::vector<QJsonObject>> subpaths;
  std::vector<QJsonObject> currentSubpath;
  for (const auto &val : points) {
    const auto node = val.toObject();
    if (node["break"].toBool(false) && !currentSubpath.empty()) {
      subpaths.push_back(currentSubpath);
      currentSubpath.clear();
    }
    currentSubpath.push_back(node);
  }
  if (!currentSubpath.empty()) subpaths.push_back(currentSubpath);

  QPainterPath resultPath;

  for (const auto &nodes : subpaths) {
    const int n = int(nodes.size());
    if (n < 2) {
      QJsonArray subArr;
      for (const auto &nd : nodes) subArr.append(nd);
      resultPath.addPath(vectorPath(subArr, closed));
      continue;
    }

    std::vector<QPointF> pts(n);
    for (int i = 0; i < n; ++i) pts[i] = getPt(nodes[i], "p");

    double signedArea = 0;
    for (int i = 0; i < n; ++i) {
      const auto &p0 = pts[i];
      const auto &p1 = pts[(i + 1) % n];
      signedArea += (p0.x() * p1.y() - p1.x() * p0.y());
    }

    std::vector<QPointF> edgeNormals(n);
    for (int i = 0; i < n; ++i) {
      const auto &p0 = pts[i];
      const auto &p1 = pts[(i + 1) % n];
      const double dx = p1.x() - p0.x();
      const double dy = p1.y() - p0.y();
      const double len = std::hypot(dx, dy);
      if (len > 1e-6) {
        if (signedArea >= 0) {
          edgeNormals[i] = QPointF(dy / len, -dx / len);
        } else {
          edgeNormals[i] = QPointF(-dy / len, dx / len);
        }
      } else {
        edgeNormals[i] = QPointF(0, 0);
      }
    }

    std::vector<QPointF> newPts(n);
    for (int i = 0; i < n; ++i) {
      const int prevIdx = (i - 1 + n) % n;
      const auto &nPrev = edgeNormals[prevIdx];
      const auto &nCurr = edgeNormals[i];
      const double dot = nPrev.x() * nCurr.x() + nPrev.y() * nCurr.y();
      const double miterScale = std::min(4.0, 1.0 / std::max(0.1, 1.0 + dot));
      const QPointF dir = nPrev + nCurr;
      const QPointF delta = expansion * miterScale * dir;
      newPts[i] = pts[i] + delta;
    }

    resultPath.moveTo(newPts[0]);
    for (int i = 1; i < n + (closed ? 1 : 0); ++i) {
      const int prevIdx = (i - 1) % n;
      const int currIdx = i % n;
      const auto &prevNode = nodes[prevIdx];
      const auto &currNode = nodes[currIdx];
      const QPointF a = newPts[prevIdx];
      const QPointF b = newPts[currIdx];
      const QPointF outHandle = getPt(prevNode, "out");
      const QPointF inHandle = getPt(currNode, "in");

      if (outHandle.isNull() && inHandle.isNull()) {
        resultPath.lineTo(b);
      } else {
        resultPath.cubicTo(a + outHandle, b + inHandle, b);
      }
    }
    if (closed) resultPath.closeSubpath();
  }

  return resultPath;
}

QPainterPath expandedMaskPath(const QJsonObject &mask, const Layer &layer,
                              const Scene &scene) {
  const auto points = mask["points"].toArray();
  double expansionVal = mask.value("expansion").toDouble(0.0);
  if (!mask.contains("expansion")) {
    for (int m = 0; m < layer.masks.size(); ++m) {
      if (layer.masks[m].toObject()["name"] == mask["name"]) {
        int extraProp = layerFindExtra(layer, QString("Mask/%1/expansion").arg(m));
        if (extraProp >= 0) expansionVal = layer.at(extraProp).at(scene.frame - layer.start);
        break;
      }
    }
  }
  const double expansion = std::clamp(expansionVal, -10000., 10000.);
  const bool inverted = mask["inverted"].toBool();
  if (expansion == 0 && !inverted) {
    return vectorPath(points, true);
  }
  const QRectF bounds = inverted ? layerBounds(layer, scene) : QRectF();

  const quint64 h = hashPointsArray(points);
  const int count = points.size();

  {
    std::lock_guard<std::mutex> lock(g_maskPathMutex);
    for (auto it = g_maskPathCache.begin(); it != g_maskPathCache.end(); ++it) {
      if (it->pointsHash == h && it->pointsCount == count &&
          std::abs(it->expansion - expansion) < 1e-5 &&
          it->inverted == inverted && it->bounds == bounds) {
        auto found = it->path;
        if (it != g_maskPathCache.begin()) {
          auto entry = *it;
          g_maskPathCache.erase(it);
          g_maskPathCache.insert(g_maskPathCache.begin(), entry);
        }
        return found;
      }
    }
  }

  QPainterPath path = expandContourPath(points, expansion, true);
  if (inverted) {
    QPainterPath b; b.addRect(bounds);
    path = b.subtracted(path);
  }

  {
    std::lock_guard<std::mutex> lock(g_maskPathMutex);
    MaskPathCacheEntry entry;
    entry.pointsHash = h;
    entry.pointsCount = count;
    entry.expansion = expansion;
    entry.inverted = inverted;
    entry.bounds = bounds;
    entry.path = path;
    g_maskPathCache.insert(g_maskPathCache.begin(), entry);
    if (g_maskPathCache.size() > 64) g_maskPathCache.pop_back();
  }

  return path;
}

bool hasFeatheredMask(const Layer &layer) {
  for(const auto &value:layer.masks) {
    const auto mask=value.toObject();
    if(mask["enabled"].toBool(true) && mask["feather"].toDouble()>0)return true;
  }
  return false;
}

} // namespace

namespace {
struct Silhouette { QImage map; QRectF bounds; double resolution = 1; };
QMutex &silhouetteGuard() { static QMutex guard; return guard; }
QCache<QByteArray, Silhouette> &silhouettes() {
  static QCache<QByteArray, Silhouette> cache(64 * 1024); // KiB
  return cache;
}
}
QImage layerSilhouette(const Layer &l, const Scene &s, double t, double softness, QRectF &bounds, double &resolution) {
  QRectF box = layerBounds(l, s);
  if (layerHasMainTransform(l)) box = layerMainTransform(l).mapRect(box);
  softness = std::clamp(softness, 0., 2000.);
  box.adjust(-softness * 1.5 - 2, -softness * 1.5 - 2, softness * 1.5 + 2, softness * 1.5 + 2);
  if (!box.isValid() || box.isEmpty()) return {};
  // A shadow carries no detail finer than a few hundred pixels across; its
  // softness takes away more.
  const double longest = std::max(box.width(), box.height());
  const double scale = std::clamp(512. / longest, .02, 1.);
  QByteArray key = effectedRasterKey(l, s, t, box, scale, box, QByteArrayLiteral("silhouette"));
  {
    QDataStream stream(&key, QIODevice::Append);
    stream << softness << QJsonDocument(l.masks).toJson(QJsonDocument::Compact);
  }
  {
    QMutexLocker lock(&silhouetteGuard());
    if (const Silhouette *kept = silhouettes().object(key)) {
      bounds = kept->bounds;
      resolution = kept->resolution;
      return kept->map;
    }
  }
  // Made without holding the lock: a band waiting here while another band
  // waits for a raster this one would build is how frames used to hang.
  QImage drawn(qMax(1, qCeil(box.width() * scale)), qMax(1, qCeil(box.height() * scale)),
               QImage::Format_ARGB32_Premultiplied);
  if (drawn.isNull()) return {};
  drawn.fill(Qt::transparent);
  {
    QPainter q(&drawn);
    q.setRenderHint(QPainter::Antialiasing);
    q.setRenderHint(QPainter::TextAntialiasing);
    q.scale(scale, scale);
    q.translate(-box.left(), -box.top());
    for (const auto &value : l.masks) {
      const auto mask = value.toObject();
      if (mask["enabled"].toBool(true)) q.setClipPath(expandedMaskPath(mask, l, s), Qt::IntersectClip);
    }
    if (layerHasMainTransform(l)) q.setWorldTransform(layerMainTransform(l), true);
    drawLayer(q, l, s, t);
    if (l.particlesOn && l.type != "particles") drawParticles(q, l, s, t);
  }
  if (softness > 0) drawn = fullResolutionBlur(drawn, softness * scale).convertToFormat(QImage::Format_ARGB32_Premultiplied);
  QImage map = drawn.convertToFormat(QImage::Format_Alpha8);
  bool any = false;
  for (int y = 0; y < map.height() && !any; ++y) {
    const uchar *row = map.constScanLine(y);
    for (int x = 0; x < map.width() && !any; ++x) any = row[x] != 0;
  }
  if (!any) map = QImage();
  {
    QMutexLocker lock(&silhouetteGuard());
    silhouettes().insert(key, new Silhouette{map, box, scale}, std::max(1, int(map.sizeInBytes() / 1024)));
  }
  bounds = box;
  resolution = scale;
  return map;
}

static std::atomic<bool> g_fastMaskScrubbing{false};
void setFastMaskScrubbing(bool active) { g_fastMaskScrubbing.store(active, std::memory_order_relaxed); }
bool fastMaskScrubbing() { return g_fastMaskScrubbing.load(std::memory_order_relaxed); }

namespace {

// Ultra-fast After Effects style mask feathering:
// High performance 3-pass separable box blur on 1-channel 8-bit alpha buffer.
// Constant O(1) arithmetic per pixel via running accumulator (independent of feather radius).
// Cache-friendly sequential memory traversal without float heap thrashing.
static QImage fastMaskBlurFeather(const QImage &src, double feather, bool inverted) {
  if (src.isNull()) return src;
  const int w = src.width();
  const int h = src.height();
  if (w <= 0 || h <= 0) return src;

  QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
  const int srcStride = src.bytesPerLine();
  const quint8 *srcBits = src.constBits();

  if (feather <= 0.25) {
    const int outStride = out.bytesPerLine();
    quint8 *outBits = out.bits();
    renderParallel(h, [&](int y0, int y1) {
      for (int y = y0; y < y1; ++y) {
        const auto *inRow = reinterpret_cast<const quint32 *>(srcBits + y * srcStride);
        auto *outRow = reinterpret_cast<quint32 *>(outBits + y * outStride);
        for (int x = 0; x < w; ++x) {
          int a = int(inRow[x] >> 24);
          if (inverted) a = 255 - a;
          outRow[x] = (quint32(a) << 24) | (quint32(a) << 16) | (quint32(a) << 8) | quint32(a);
        }
      }
    });
    return out;
  }

  std::vector<quint8> bufA(size_t(w) * h);
  std::vector<quint8> bufB(size_t(w) * h);

  std::atomic<bool> hasInside{false};
  std::atomic<bool> hasOutside{false};

  renderParallel(h, [&](int y0, int y1) {
    bool localIn = false, localOut = false;
    for (int y = y0; y < y1; ++y) {
      const auto *inRow = reinterpret_cast<const quint32 *>(srcBits + y * srcStride);
      quint8 *aRow = bufA.data() + y * w;
      for (int x = 0; x < w; ++x) {
        const quint8 a = quint8(inRow[x] >> 24);
        aRow[x] = a;
        if (a >= 128) localIn = true;
        else localOut = true;
      }
    }
    if (localIn) hasInside = true;
    if (localOut) hasOutside = true;
  });

  if (!hasInside) {
    out.fill(inverted ? Qt::white : Qt::transparent);
    return out;
  }
  if (!hasOutside) {
    out.fill(inverted ? Qt::transparent : Qt::white);
    return out;
  }

  const bool isScrubbing = fastMaskScrubbing();
  const int passes = isScrubbing ? 1 : 3;
  const int radius = isScrubbing ? std::max(1, int(feather * 0.75 + 0.5))
                                 : std::max(1, int(feather * 0.45 + 0.5));

  auto boxBlurH = [&](const quint8 *in, quint8 *out, int r) {
    const int window = 2 * r + 1;
    const quint32 invWindow = quint32((1ULL << 24) / window);
    renderParallel(h, [&](int y0, int y1) {
      for (int y = y0; y < y1; ++y) {
        const quint8 *srcRow = in + y * w;
        quint8 *dstRow = out + y * w;
        int sum = int(srcRow[0]) * (r + 1);
        for (int x = 1; x <= r; ++x) {
          sum += int(srcRow[std::min(x, w - 1)]);
        }
        for (int x = 0; x < w; ++x) {
          dstRow[x] = quint8(((quint64(sum) * invWindow) + (1 << 23)) >> 24);
          int enterPixel = std::min(w - 1, x + r + 1);
          int leavePixel = std::max(0, x - r);
          sum += int(srcRow[enterPixel]) - int(srcRow[leavePixel]);
        }
      }
    });
  };

  auto boxBlurV = [&](const quint8 *in, quint8 *out, int r) {
    const int window = 2 * r + 1;
    const quint32 invWindow = quint32((1ULL << 24) / window);
    const int sliceSize = 64;
    const int numSlices = (w + sliceSize - 1) / sliceSize;
    renderParallel(numSlices, [&](int s0, int s1) {
      std::vector<int> colSum(sliceSize);
      for (int s = s0; s < s1; ++s) {
        const int x0 = s * sliceSize;
        const int x1 = std::min(w, x0 + sliceSize);
        const int curW = x1 - x0;
        for (int x = 0; x < curW; ++x) {
          colSum[x] = int(in[x0 + x]) * (r + 1);
        }
        for (int y = 1; y <= r; ++y) {
          const quint8 *row = in + std::min(y, h - 1) * w + x0;
          for (int x = 0; x < curW; ++x) colSum[x] += int(row[x]);
        }
        for (int y = 0; y < h; ++y) {
          quint8 *dstRow = out + y * w + x0;
          for (int x = 0; x < curW; ++x) {
            dstRow[x] = quint8(((quint64(colSum[x]) * invWindow) + (1 << 23)) >> 24);
          }
          const quint8 *enterRow = in + std::min(h - 1, y + r + 1) * w + x0;
          const quint8 *leaveRow = in + std::max(0, y - r) * w + x0;
          for (int x = 0; x < curW; ++x) {
            colSum[x] += int(enterRow[x]) - int(leaveRow[x]);
          }
        }
      }
    });
  };

  for (int p = 0; p < passes; ++p) {
    boxBlurH(bufA.data(), bufB.data(), radius);
    boxBlurV(bufB.data(), bufA.data(), radius);
  }

  const int outStride = out.bytesPerLine();
  quint8 *outBits = out.bits();
  renderParallel(h, [&](int y0, int y1) {
    for (int y = y0; y < y1; ++y) {
      auto *outRow = reinterpret_cast<quint32 *>(outBits + y * outStride);
      const quint8 *aRow = bufA.data() + y * w;
      for (int x = 0; x < w; ++x) {
        int a = int(aRow[x]);
        if (inverted) a = 255 - a;
        outRow[x] = (quint32(a) << 24) | (quint32(a) << 16) | (quint32(a) << 8) | quint32(a);
      }
    }
  });

  return out;
}

static QImage fastMaskDistanceFeather(const QImage &src, double feather, bool inverted) {
  return fastMaskBlurFeather(src, feather, inverted);
}

struct MaskMatteCacheEntry {
  QJsonArray masks;
  QRectF logical;
  double rasterScale = 1.0;
  double frame = 0.0;
  QSize size;
  QImage matte;
  QRect affectedPixels;
  bool outsideOpaque = true;
};
static std::mutex g_maskMatteMutex;
static std::vector<MaskMatteCacheEntry> g_maskMatteCache;
static qint64 g_maskMatteCacheBytes = 0;

QImage applyFeatheredMasks(const QImage &source,const Layer &layer,const Scene &scene,
                           QRectF logical,double rasterScale) {
  if(source.isNull())return source;
  QImage combined;
  QRect affectedPixels;
  bool outsideOpaque = true;
  bool foundInCache = false;

  auto lookUpMatte = [&] {
    std::lock_guard<std::mutex> lock(g_maskMatteMutex);
    for (auto it = g_maskMatteCache.begin(); it != g_maskMatteCache.end(); ++it) {
      if (it->size == source.size() &&
          std::abs(it->rasterScale - rasterScale) < 1e-4 &&
          std::abs(it->frame - scene.frame) < 1e-4 &&
          it->logical == logical &&
          it->masks == layer.masks) {
        combined = it->matte;
        affectedPixels = it->affectedPixels;
        outsideOpaque = it->outsideOpaque;
        foundInCache = true;
        if (it != g_maskMatteCache.begin()) {
          auto entry = *it;
          g_maskMatteCache.erase(it);
          g_maskMatteCache.insert(g_maskMatteCache.begin(), entry);
        }
        break;
      }
    }
  };
  lookUpMatte();
  // A frame drawn in bands has every band ask for this same matte at the same
  // moment, all of them missing, and each one built it for itself: on twenty
  // cores a feathered mask cost twenty times the work of one. Only one builds
  // it; the rest wait here and find it in the cache.
  static std::mutex building;
  std::unique_lock<std::mutex> buildLock(building, std::defer_lock);
  if (!foundInCache) {
    buildLock.lock();
    lookUpMatte();
  }

  if (!foundInCache) {
    bool any = false;
    for (const auto &value : layer.masks) {
      if (value.toObject()["enabled"].toBool(true)) { any = true; break; }
    }
    if (!any) return source;

    combined = QImage(source.size(), QImage::Format_ARGB32_Premultiplied);
    combined.fill(Qt::white);
    bool firstMask = true;

    for (int maskIdx = 0; maskIdx < layer.masks.size(); ++maskIdx) {
      const auto value = layer.masks[maskIdx];
      auto mask = value.toObject();
      if (!mask["enabled"].toBool(true)) continue;
      const QString mode = mask.value("mode").toString(mask["inverted"].toBool() ? "Subtract" : "Add");
      if (mode == "None") continue;
      const auto points = mask["points"].toArray();
      if (points.isEmpty()) continue;

      const double t = scene.frame - layer.start;
      int featherProp = layerFindExtra(layer, QString("Mask/%1/feather").arg(maskIdx));
      double featherVal = (featherProp >= 0) ? layer.at(featherProp).at(t) : mask["feather"].toDouble();

      int opacityProp = layerFindExtra(layer, QString("Mask/%1/opacity").arg(maskIdx));
      double opacityVal = (opacityProp >= 0) ? layer.at(opacityProp).at(t) * 100.0 : mask.value("opacity").toDouble(100.0);

      int expansionProp = layerFindExtra(layer, QString("Mask/%1/expansion").arg(maskIdx));
      double expansionVal = (expansionProp >= 0) ? layer.at(expansionProp).at(t) : mask["expansion"].toDouble(0.0);

      mask["feather"] = featherVal;
      mask["opacity"] = opacityVal;
      mask["expansion"] = expansionVal;

      // Expand the boundary once as geometry, then rasterize its interior.
      // Stroking the original path into the alpha image rounded sharp points
      // and made the feathered and hard-mask paths disagree.
      QJsonObject upright = mask;
      upright["inverted"] = false;
      const QPainterPath boundary = expandedMaskPath(upright, layer, scene);
      const double feather = std::clamp(featherVal, 0., 10000.) * rasterScale;
      const bool inverted = (mode == "Subtract") || mask["inverted"].toBool();

      // The blur is limited to the boundary's neighbourhood, even when the
      // source image spans the full composition.
      const double pad = (2.0 * feather + 4.0) / std::max(1e-4, rasterScale);
      const QRectF localRoi = boundary.boundingRect().adjusted(-pad, -pad, pad, pad);
      const QRect pixelRoi = QRectF((localRoi.left() - logical.left()) * rasterScale,
                                    (localRoi.top() - logical.top()) * rasterScale,
                                    localRoi.width() * rasterScale,
                                    localRoi.height() * rasterScale)
                                 .toAlignedRect()
                                 .intersected(source.rect());
      affectedPixels = affectedPixels.united(pixelRoi);
      outsideOpaque &= inverted;

      QImage roiImg;
      if (!pixelRoi.isEmpty()) {
        roiImg = QImage(pixelRoi.size(), QImage::Format_ARGB32_Premultiplied);
        roiImg.fill(Qt::transparent);
        {
          QPainter painter(&roiImg);
          painter.setRenderHint(QPainter::Antialiasing);
          painter.translate(-pixelRoi.left(), -pixelRoi.top());
          painter.scale(rasterScale, rasterScale);
          painter.translate(-logical.left(), -logical.top());
          painter.fillPath(boundary, Qt::white);
        }
        if (feather > 0) {
          roiImg = fastMaskDistanceFeather(roiImg, feather, inverted);
        } else if (inverted) {
          const int outStride = roiImg.bytesPerLine();
          quint8 *outBits = roiImg.bits();
          for (int y = 0; y < roiImg.height(); ++y) {
            auto *row = reinterpret_cast<quint32 *>(outBits + y * outStride);
            for (int x = 0; x < roiImg.width(); ++x) {
              int a = 255 - int(row[x] >> 24);
              row[x] = (quint32(a) << 24) | (quint32(a) << 16) | (quint32(a) << 8) | quint32(a);
            }
          }
        }
        if (opacityVal < 99.99) {
          const float op = std::clamp(float(opacityVal / 100.0), 0.0f, 1.0f);
          const int outStride = roiImg.bytesPerLine();
          quint8 *outBits = roiImg.bits();
          for (int y = 0; y < roiImg.height(); ++y) {
            auto *row = reinterpret_cast<quint32 *>(outBits + y * outStride);
            for (int x = 0; x < roiImg.width(); ++x) {
              int a = int(float(row[x] >> 24) * op + 0.5f);
              row[x] = (quint32(a) << 24) | (quint32(a) << 16) | (quint32(a) << 8) | quint32(a);
            }
          }
        }
      }

      if (firstMask) {
        // The common one-mask case needs no second full-size image or
        // full-size DestinationIn pass.
        combined.fill(inverted ? Qt::white : Qt::transparent);
        if (!roiImg.isNull()) {
          QPainter painter(&combined);
          painter.setCompositionMode(QPainter::CompositionMode_Source);
          painter.drawImage(pixelRoi.topLeft(), roiImg);
        }
        firstMask = false;
      } else {
        QImage one(source.size(), QImage::Format_ARGB32_Premultiplied);
        one.fill(inverted ? Qt::white : Qt::transparent);
        if (!roiImg.isNull()) {
          QPainter painter(&one);
          painter.setCompositionMode(QPainter::CompositionMode_Source);
          painter.drawImage(pixelRoi.topLeft(), roiImg);
        }
        QPainter intersection(&combined);
        intersection.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        intersection.drawImage(0, 0, one);
      }
    }

    {
      std::lock_guard<std::mutex> lock(g_maskMatteMutex);
      MaskMatteCacheEntry entry;
      entry.masks = layer.masks;
      entry.logical = logical;
      entry.rasterScale = rasterScale;
      entry.frame = scene.frame;
      entry.size = source.size();
      entry.matte = combined;
      entry.affectedPixels = affectedPixels;
      entry.outsideOpaque = outsideOpaque;
      constexpr qint64 matteCacheBudget = 64 * 1024 * 1024;
      const qint64 cost = combined.sizeInBytes();
      if (cost <= matteCacheBudget) {
        g_maskMatteCache.insert(g_maskMatteCache.begin(), entry);
        g_maskMatteCacheBytes += cost;
        while (g_maskMatteCacheBytes > matteCacheBudget || g_maskMatteCache.size() > 32) {
          g_maskMatteCacheBytes -= g_maskMatteCache.back().matte.sizeInBytes();
          g_maskMatteCache.pop_back();
        }
      }
    }
  }
  // Built and stored: whoever waited for it can look it up now.
  if (buildLock.owns_lock()) buildLock.unlock();

  if (source.format() == QImage::Format_ARGB32_Premultiplied) {
    QImage result = outsideOpaque
        ? source.copy() : QImage(source.size(), QImage::Format_ARGB32_Premultiplied);
    if (!outsideOpaque) result.fill(Qt::transparent);
    if (affectedPixels.isEmpty()) return result;
    const auto *srcBits = source.constBits();
    const int srcStride = source.bytesPerLine();
    const auto *maskBits = combined.constBits();
    const int maskStride = combined.bytesPerLine();
    auto *dstBits = result.bits();
    const int dstStride = result.bytesPerLine();
    const int left = affectedPixels.left(), right = affectedPixels.right() + 1;
    const int top = affectedPixels.top(), bottom = affectedPixels.bottom() + 1;

    renderParallel(bottom - top, [&](int y0, int y1) {
      for (int y = top + y0; y < top + y1; ++y) {
        const auto *src = reinterpret_cast<const quint32 *>(srcBits + y * srcStride);
        auto *dst = reinterpret_cast<quint32 *>(dstBits + y * dstStride);
        const auto *m = reinterpret_cast<const quint32 *>(maskBits + y * maskStride);
        for (int x = left; x < right; ++x) {
          const quint32 ma = (m[x] >> 24);
          if (ma == 255) { if (!outsideOpaque) dst[x] = src[x]; continue; }
          if (ma == 0) { dst[x] = 0; continue; }
          const quint32 d = src[x];
          quint32 da = (d >> 24);
          quint32 dr = (d >> 16) & 0xff;
          quint32 dg = (d >> 8) & 0xff;
          quint32 db = d & 0xff;
          da = (da * ma + 128 + ((da * ma + 128) >> 8)) >> 8;
          dr = (dr * ma + 128 + ((dr * ma + 128) >> 8)) >> 8;
          dg = (dg * ma + 128 + ((dg * ma + 128) >> 8)) >> 8;
          db = (db * ma + 128 + ((db * ma + 128) >> 8)) >> 8;
          dst[x] = (da << 24) | (dr << 16) | (dg << 8) | db;
        }
      }
    });
    return result;
  }

  if (source.format() == QImage::Format_RGBA32FPx4_Premultiplied) {
    QImage result = outsideOpaque
        ? source.copy() : QImage(source.size(), QImage::Format_RGBA32FPx4_Premultiplied);
    if (!outsideOpaque) result.fill(Qt::transparent);
    if (affectedPixels.isEmpty()) return result;
    const auto *srcBits = source.constBits();
    const int srcStride = source.bytesPerLine();
    const auto *maskBits = combined.constBits();
    const int maskStride = combined.bytesPerLine();
    auto *dstBits = result.bits();
    const int dstStride = result.bytesPerLine();
    const int left = affectedPixels.left(), right = affectedPixels.right() + 1;
    const int top = affectedPixels.top(), bottom = affectedPixels.bottom() + 1;

    renderParallel(bottom - top, [&](int y0, int y1) {
      for (int y = top + y0; y < top + y1; ++y) {
        const auto *src = reinterpret_cast<const float *>(srcBits + y * srcStride);
        auto *dst = reinterpret_cast<float *>(dstBits + y * dstStride);
        const auto *m = reinterpret_cast<const quint32 *>(maskBits + y * maskStride);
        for (int x = left; x < right; ++x) {
          const quint32 ma = (m[x] >> 24);
          if (ma == 255) {
            if (!outsideOpaque)
              for (int channel = 0; channel < 4; ++channel)
                dst[x * 4 + channel] = src[x * 4 + channel];
            continue;
          }
          if (ma == 0) {
            dst[x * 4 + 0] = 0.f; dst[x * 4 + 1] = 0.f;
            dst[x * 4 + 2] = 0.f; dst[x * 4 + 3] = 0.f;
            continue;
          }
          const float a = float(ma) * (1.0f / 255.0f);
          dst[x * 4 + 0] = src[x * 4 + 0] * a;
          dst[x * 4 + 1] = src[x * 4 + 1] * a;
          dst[x * 4 + 2] = src[x * 4 + 2] * a;
          dst[x * 4 + 3] = src[x * 4 + 3] * a;
        }
      }
    });
    return result;
  }

  QImage result = source.copy();
  QPainter cut(&result);
  cut.setCompositionMode(QPainter::CompositionMode_DestinationIn);
  cut.drawImage(0, 0, combined);
  cut.end();
  return result;
}
}
void setHostLayerEffects(const HostLayerEffects *effects) { hostEffectsRegistered = effects; }
// As the raster stage works it out for a layer carrying a host effect (which
// keeps the layer's whole box, its letters moving or not): keep the two alike.
QRectF hostEffectContentBox(const Layer &l, const Scene &s) {
  QRectF box = layerBounds(l, s);
  if (l.type == "text" && !textStrokeStack(l).empty()) {
    const double exact = textStrokeOutwardReach(l);
    const double stable = exact > 0 ? std::ceil(exact / 16.) * 16. : 0;
    box = box.adjusted(exact - stable, exact - stable, stable - exact, stable - exact);
    if (const QRectF corners = mitredTextStrokeBox(l); !corners.isNull()) box = box.united(corners);
  }
  if (layerHasMainTransform(l)) box = layerMainTransform(l).mapRect(box);
  return box;
}
QPointF hostEffectPointToComposition(const Layer &l, const Scene &s, double frame, QPointF point) {
  const QRectF box = hostEffectContentBox(l, s);
  return layerWorldTransform(l, s, frame).map(QPointF(box.left() + point.x(), box.bottom() - point.y()));
}
QPointF compositionToHostEffectPoint(const Layer &l, const Scene &s, double frame, QPointF at) {
  const QRectF box = hostEffectContentBox(l, s);
  bool invertible = false;
  const QPointF local = layerWorldTransform(l, s, frame).inverted(&invertible).map(at);
  if (!invertible) return {};
  return {local.x() - box.left(), box.bottom() - local.y()};
}
const HostLayerEffects *hostLayerEffects() { return hostEffectsRegistered; }

QByteArray effectedRasterKey(const Layer &l, const Scene &s, double t, QRectF logical,
                             double rasterScale, QRectF box, const QByteArray &host) {
  QByteArray content;
  QDataStream stream(&content, QIODevice::WriteOnly);
  stream << l.type << l.shapeKind << l.text << l.richHtml << l.font << l.fontSize << l.color << l.bold
         << l.fontStyle << l.fauxBold << l.fauxItalic << l.capitals << l.leading << l.tracking << l.align
         << l.paragraphWidth << l.strokeWidth
         << l.strokeColor << l.strokeMode << l.shapeStrokeCap
         << l.shapeFill << l.shapeClosed << l.shapePoints;
  stream << qint32(l.textStrokes.size());
  for (const auto &stroke : l.textStrokes)
    stream << stroke.width << stroke.color << stroke.mode << stroke.join;
  if (l.textStrokeOnly) stream << QByteArray("stroke only");
  stream << l.image.cacheKey();
  stream << l.blur << l.glow << l.shadow;
  // Worked out in linear light, those three are other pictures.
  stream << (s.linearLight && (l.blur || l.glow || l.shadow));
  stream << QJsonDocument(l.appliedEffects).toJson(QJsonDocument::Compact)
         << QJsonDocument(l.hiddenEffects).toJson(QJsonDocument::Compact)
         << QJsonDocument(l.surfaceEffects).toJson(QJsonDocument::Compact)
         << QJsonDocument(QJsonObject{{"order", l.effectOrder}}).toJson(QJsonDocument::Compact);
  // The framing the raster was built for, and the composition a fill layer takes
  // its size from.
  stream << logical << rasterScale << box << s.processingDepth << s.width << s.height;
  // The main transform's rotation, scale, anchor and opacity are inside the
  // raster, so the effects see them and a change to any of them is a different
  // picture. Its position is not: that was moved out beside the movement
  // position, so sliding a layer by it costs nothing, exactly as sliding it by
  // Position costs nothing. Naming it here would rebuild the raster for a move
  // that cannot change a single pixel of it.
  stream << l.mainAnchorX << l.mainAnchorY << l.mainScaleX
         << l.mainScaleY << l.mainRotation;
  if (parametricShape(l))
    for (int property = ShapeWidth; property < PropertyCount; ++property)
      stream << l.p[property].at(t);
  else if (l.type == "shape")
    for (int property = ShapeTrimStart; property < PropertyCount; ++property)
      stream << l.p[property].at(t);
  // The wipes' sliders are properties that any kind of layer can carry.
  for (int property = WipeLinearCompletion; property < PropertyCount; ++property)
    stream << l.p[property].at(t);
  stream << QJsonDocument(l.surfaceEffects["Radial Wipe"].toObject()).toJson(QJsonDocument::Compact);
  // A particle layer's picture hangs on its whole history -- a rate keyed a
  // second ago, where the layer was when each particle was born -- not only on
  // the values at this frame.
  if (layerEmitsParticles(l)) stream << particleIdentity(l, s, t);
  // Letters carrying another layer's picture change when that layer does.
  if (layerUsesLetterTexture(l)) stream << letterTextureKey(l, s, t);
  // Animated Letters numbers, as they are at this frame.
  for (int i = 0; i < l.extraCurveIds.size() && i < int(l.extraCurves.size()); ++i)
    if (l.extraCurveIds[i].startsWith("Letters/")) stream << l.extraCurveIds[i] << l.extraCurves[size_t(i)].at(t);
  if (l.characterAnimation.value("enabled").toBool() &&
      !l.hiddenEffects.value("Text-Animation").toBool()) {
    // Once every character has settled the frame no longer changes the picture,
    // so it is not part of its identity: a title held on screen is one raster,
    // not one per frame.
    if (characterPoseAtRest(l, t)) stream << QByteArray("at rest");
    else stream << t;
    stream << QJsonDocument(l.characterAnimation).toJson(QJsonDocument::Compact);
    for (int index : {6, 7}) {
      stream << l.p[index].base;
      for (const auto &key : l.p[index].keys) {
        stream << key.frame << key.value << key.ease << key.custom << key.absolute
               << key.continuous;
        for(double control:key.curve)stream<<control;
      }
      if (l.p[index].hasExpression()) stream << l.p[index].expression << l.p[index].at(t);
    }
    // The In and Out of the stacked animators waiting outside those two.
    for (int i = 0; i < l.extraCurveIds.size() && i < int(l.extraCurves.size()); ++i)
      if (l.extraCurveIds[i].startsWith("TextAnim/")) {
        const Property &curve = l.extraCurves[size_t(i)];
        stream << l.extraCurveIds[i] << curve.base << curve.at(t);
        // The whole curve, handles too: each letter reads it at its own delay,
        // so past the last key a handle dragged earlier still moves letters.
        for (const auto &key : curve.keys) {
          stream << key.frame << key.value << key.ease << key.custom << key.absolute << key.continuous;
          for (double control : key.curve) stream << control;
        }
        if (curve.hasExpression()) stream << curve.expression;
      }
  }
  // What the host's own effects for this layer amount to at this frame. Load
  // bearing for an animated parameter and nothing else: a plugin's constants
  // live in surfaceEffects, which is hashed a few lines above, but a curve
  // evaluates differently at every frame while the layer's JSON never changes.
  // Without this, a text layer with an animated effect parameter renders frame
  // zero and then reuses that raster for the whole animation -- verified by
  // removing this line and watching two frames come back identical.
  // Moving noise is a different picture at every frame.
  if (layerHasAnimatedEffect(l)) stream << t;
  stream << host;
  // The same for effects from packs, whose sliders can be animated.
  stream << layerPackIdentity(l, t);
  return QCryptographicHash::hash(content, QCryptographicHash::Sha256);
}
// Defined beside the frame cache, which asks the same question of the machine.
qint64 physicalMemoryBytes();

namespace {
// Bounded, and shared: export can drive this from a worker.
QMutex &rasterCacheGuard() { static QMutex guard; return guard; }
// An eighth of the machine, never less than the old fixed figure and never
// more than a gigabyte and a half. A hundred and twenty large glowing layers
// need about two hundred megabytes of finished rasters between them; at the
// fixed hundred and twenty-eight this cache held none of them from one frame
// to the next.
int rasterBudgetKiB() {
  static const int budget = [] {
    const qint64 physical = physicalMemoryBytes();
    if (physical <= 0) return 131072;
    const qint64 eighth = physical / 8 / 1024;
    return int(std::clamp<qint64>(eighth, 131072, 1536 * 1024));
  }();
  return budget;
}
QCache<QByteArray, QImage> &rasterCache() {
  static QCache<QByteArray, QImage> cache{rasterBudgetKiB()};
  return cache;
}
}
void setRasterCacheBudget(int kibibytes) {
  QMutexLocker lock(&rasterCacheGuard());
  rasterCache().setMaxCost(std::max(1024, kibibytes));
}
int rasterCacheBudget() {
  QMutexLocker lock(&rasterCacheGuard());
  return rasterCache().maxCost();
}
namespace {
// How much of the budget this frame has already claimed, by hitting an entry
// or by adding one. Guarded by rasterCacheGuard.
//
// This is what stops the cliff. A least-recently-used cache walked in the same
// order every frame is the worst case there is: by the time the last layer is
// drawn the first one has been evicted to make room for it, so the next frame
// misses on every single layer and the hit rate is not reduced, it is zero.
// Measured, a hundred and twenty large glowing layers went from eight
// milliseconds at thirty layers to eleven hundred at a hundred and twenty --
// a cliff, not a slope. Counting what the frame has already claimed and
// refusing to add beyond it means the layers that did fit stay fit, and a
// scene too big for any budget keeps whatever fraction of itself fits instead
// of losing all of it.
int rasterFrameClaimedKiB = 0;
// Each entry claims its room once per frame, however many bands, samples or
// layers ask for it again. Counted per hit, a motion-blurred frame of a thousand
// layers claimed the same rasters dozens of times over, passed the budget, and
// from then on stored nothing: every frame rebuilt every layer.
QSet<QByteArray> rasterFrameClaimed;
}
void beginRasterFrame() {
  QMutexLocker lock(&rasterCacheGuard());
  rasterFrameClaimedKiB = 0;
  rasterFrameClaimed.clear();
}
namespace {
std::atomic<quint64> rastersBuilt{0}, sourceNs{0}, effectNs{0}, sourceHits{0}, effectHits{0};
thread_local quint64 sourcesBuiltHere = 0;
QCache<QByteArray,QImage> &sourceCache() {
  // Separate from the effected rasters. A sixteenth of the machine, 64 MiB to
  // 768 MiB: a scene of a thousand layers has to hold every layer's source at
  // once, or sources rasterized ahead on all cores are evicted before the
  // serial pass reaches them.
  static QCache<QByteArray,QImage> cache{[] {
    const qint64 physical = physicalMemoryBytes();
    return physical <= 0 ? 65536
                         : int(std::clamp<qint64>(physical / 16 / 1024, 65536, 786432));
  }()};
  return cache;
}
QByteArray sourceRasterKey(const Layer &layer,const Scene &s,double t,QRectF logical,double scale,QRectF box) {
  Layer source=layer;
  source.blur=source.glow=source.shadow=0;
  source.appliedEffects={};source.surfaceEffects={};source.effectOrder={};
  // The letters' backgrounds and textures are drawn with the letters: part of the source.
  if(layer.surfaceEffects.contains("Letters")) source.surfaceEffects["Letters"]=layer.surfaceEffects["Letters"];
  // Only effects folded into the source drawing affect its cache identity.
  for(const auto &name:layerEffectOrder(layer)) {
    const bool folded=(name=="Fill" && vectorEffectFill(layer)) ||
                      (name=="Gradient" && vectorEffectGradient(layer)) ||
                      (name=="Stroke" && vectorEffectStroke(layer));
    if(folded) {
      source.appliedEffects[name]=true;
      source.surfaceEffects[name]=layer.surfaceEffects[name];
      source.effectOrder.append(name);
    }
  }
  return effectedRasterKey(source,s,t,logical,scale,box);
}
QCache<QByteArray,QImage> &preBlurCache() {
  static QCache<QByteArray,QImage> cache{65536};
  return cache;
}
QByteArray preBlurKey(const Layer &layer,const Scene &s,double t,QRectF logical,double scale,QRectF box) {
  Layer stripped=layer;
  stripped.blur=stripped.glow=stripped.shadow=0;
  return effectedRasterKey(stripped,s,t,logical,scale,box);
}
}
RenderWorkCounters renderWorkCounters() {
  return {sourceNs.load(),effectNs.load(),sourceHits.load(),effectHits.load()};
}
QString renderWorkReport(const RenderWorkCounters &before) {
  const auto now=renderWorkCounters();
  return QString("Source %1 ms; effects %2 ms; cache hits source %3 / effects %4 (aggregate work)")
      .arg((now.sourceNs-before.sourceNs)/1e6,0,'f',2)
      .arg((now.effectNs-before.effectNs)/1e6,0,'f',2)
      .arg(now.sourceHits-before.sourceHits).arg(now.effectHits-before.effectHits);
}
quint64 effectedRasterBuilds() { return rastersBuilt.load(); }
quint64 layerSourcesBuiltHere() { return sourcesBuiltHere; }
QImage cachedEffectedRaster(const QByteArray &key) {
  QMutexLocker lock(&rasterCacheGuard());
  if (auto *hit = rasterCache().object(key)) {
    // A frame that is using this entry has a claim on the room it occupies.
    // Without that claim the layers still to be drawn would evict it to make
    // room for themselves, and the next frame would find nothing at all.
    if (!rasterFrameClaimed.contains(key)) {
      rasterFrameClaimed.insert(key);
      rasterFrameClaimedKiB += std::max(1, int((hit->sizeInBytes() + 1023) / 1024));
    }
    return *hit;
  }
  return {};
}
// A raster in the painters' own format. An 8 bpc layer's (RGBA8888,
// premultiplied) differs from it only in where red and blue sit, which Qt
// swaps a pixel at a time on one core: 15 ms a frame for a title at 900%,
// twelve megapixels. Here in rows on the cores at once, the same bytes.
static QImage inPainterFormat(const QImage &source) {
  constexpr auto painter = QImage::Format_ARGB32_Premultiplied;
  if (source.format() == painter) return source;
  if (source.format() != QImage::Format_RGBA8888_Premultiplied || qint64(source.width()) * source.height() < 262144)
    return source.convertToFormat(painter);
  QImage out(source.size(), painter);
  if (out.isNull()) return source.convertToFormat(painter);
  out.setDotsPerMeterX(source.dotsPerMeterX());
  out.setDotsPerMeterY(source.dotsPerMeterY());
  out.setOffset(source.offset());
  out.setDevicePixelRatio(source.devicePixelRatio());
  out.setColorSpace(source.colorSpace());
  for (const QString &key : source.textKeys()) out.setText(key, source.text(key));
  const uchar *const from = source.constBits();
  uchar *const to = out.bits();
  const qsizetype fromStride = source.bytesPerLine(), toStride = out.bytesPerLine();
  const int width = source.width();
  renderParallel(source.height(), [&](int begin, int end) {
    for (int y = begin; y < end; ++y) {
      const auto *in = reinterpret_cast<const quint32 *>(from + y * fromStride);
      auto *outLine = reinterpret_cast<quint32 *>(to + y * toStride);
      // Bytes R G B A as A R G B: red and blue change places.
      for (int x = 0; x < width; ++x) {
        const quint32 v = in[x];
        outLine[x] = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);
      }
    }
  });
  return out;
}
void storeEffectedRaster(const QByteArray &key, const QImage &raster) {
  if (raster.isNull()) return;
  ++rastersBuilt;
  const int cost = std::max(1, int((raster.sizeInBytes() + 1023) / 1024));
  QMutexLocker lock(&rasterCacheGuard());
  // Beyond what this frame can hold, keeping it would cost something the same
  // frame is still going to ask for. The raster is returned to the caller
  // either way; only its place in the cache is refused.
  const bool claimedAlready = rasterFrameClaimed.contains(key);
  if (!claimedAlready) {
    if (rasterFrameClaimedKiB + cost > rasterCache().maxCost()) return;
    rasterFrameClaimedKiB += cost;
    rasterFrameClaimed.insert(key);
  }
  rasterCache().insert(key, new QImage(raster), cost);
}
void clearRenderCaches() {
  shapeGeometryForget();
  {
    QMutexLocker lock(&rasterCacheGuard());
    rasterCache().clear();
    rasterFrameClaimedKiB = 0;
    rasterFrameClaimed.clear();
    // Under the same guard as every read and insert: a render running on a
    // worker (the pre-render lanes) may be looking in them right now.
    sourceCache().clear();
    preBlurCache().clear();
  }
}
const QVector<QPair<QString, QString>> &layerBlendModes() {
  static const QVector<QPair<QString, QString>> modes{
      {"normal", "Normal"},
      {"darken", "Darken"},       {"multiply", "Multiply"},     {"colorBurn", "Color Burn"},
      {"lighten", "Lighten"},     {"screen", "Screen"},         {"colorDodge", "Color Dodge"},
      {"add", "Add"},
      {"overlay", "Overlay"},     {"softLight", "Soft Light"},  {"hardLight", "Hard Light"},
      {"difference", "Difference"}, {"exclusion", "Exclusion"}};
  return modes;
}
QPainter::CompositionMode layerCompositionMode(const Layer &l) {
  static const QHash<QString, QPainter::CompositionMode> modes{
      {"darken", QPainter::CompositionMode_Darken},
      {"multiply", QPainter::CompositionMode_Multiply},
      {"colorBurn", QPainter::CompositionMode_ColorBurn},
      {"lighten", QPainter::CompositionMode_Lighten},
      {"screen", QPainter::CompositionMode_Screen},
      {"colorDodge", QPainter::CompositionMode_ColorDodge},
      {"add", QPainter::CompositionMode_Plus},
      {"overlay", QPainter::CompositionMode_Overlay},
      {"softLight", QPainter::CompositionMode_SoftLight},
      {"hardLight", QPainter::CompositionMode_HardLight},
      {"difference", QPainter::CompositionMode_Difference},
      {"exclusion", QPainter::CompositionMode_Exclusion}};
  return modes.value(l.blendMode, QPainter::CompositionMode_SourceOver);
}
bool sceneUsesBlendModes(const Scene &s, double frame) {
  QHash<QString, const Layer *> groups;
  for (const auto &layer : s.layers)
    if (layer.type == "group") groups.insert(layer.id, &layer);
  auto active = [frame](const Layer &layer) {
    return layer.visible && frame >= layer.start && frame < layer.start + layer.duration;
  };
  for (const auto &layer : s.layers) {
    if (!active(layer) || layer.type == "camera" || layer.type == "light" || layer.type == "empty" || layer.type == "audio" ||
        layerCompositionMode(layer) == QPainter::CompositionMode_SourceOver)
      continue;
    // A child of a hidden/inactive group contributes no pixels. An orphaned
    // child is also absent from layerPaintOrder, so neither should force an
    // otherwise GPU-compatible frame through the CPU renderer.
    QString owner = layer.groupId;
    QSet<QString> visited;
    bool contributes = true;
    while (!owner.isEmpty()) {
      if (visited.contains(owner)) { contributes = false; break; }
      visited.insert(owner);
      const auto found = groups.constFind(owner);
      if (found == groups.cend() || !active(**found)) { contributes = false; break; }
      owner = (*found)->groupId;
    }
    if (contributes) return true;
  }
  return false;
}

int layerMatteSource(const Scene &s, int owner) {
  if (owner < 0 || owner >= int(s.layers.size())) return -1;
  const Layer &l = s.layers[owner];
  if (l.trackMatteId.isEmpty()) return -1;
  for (int i = 0; i < int(s.layers.size()); ++i) {
    if (s.layers[i].id == l.trackMatteId) {
      if (s.layers[i].type == "camera" || s.layers[i].type == "light" || s.layers[i].type == "empty" || s.layers[i].type == "audio" ||
          layerIsStructure(s.layers[i]))
        return -1;
      if (!s.layers[i].groupId.isEmpty()) return -1;
      return i;
    }
  }
  return -1;
}
bool sceneUsesTrackMattes(const Scene &s, double frame) {
  for (int i = 0; i < int(s.layers.size()); ++i) {
    const Layer &l = s.layers[i];
    if (!l.visible || frame < l.start || frame >= l.start + l.duration) continue;
    if (layerMatteSource(s, i) >= 0) return true;
  }
  return false;
}
// The three identities a layer's raster is looked up by -- the finished raster,
// the source it is made from, the picture before its blur -- cost about a
// hundred microseconds together: two of them copy the whole Layer and all three
// write its effects out as JSON. A layer drawn once per temporal sample for
// motion blur asks for them again for every sample, and they are the same each
// time. Inside one such layer they are kept. Only where nothing but the frame
// and the layer's own settings can change them: a text or fill layer, without
// animated characters and without a host effect, at the same size and place.
namespace {
struct KeyMemo {
  const Scene *scene = nullptr;
  int layer = -1;
  QRectF logical, box;
  double scale = 0;
  bool hasRaster = false, hasSource = false, hasPre = false;
  QByteArray raster, source, pre;
};
thread_local KeyMemo g_keyMemo;
struct KeyMemoScope {
  KeyMemo previous;
  KeyMemoScope(const Scene &s, int index) : previous(g_keyMemo) {
    g_keyMemo = KeyMemo{};
    g_keyMemo.scene = &s;
    g_keyMemo.layer = index;
  }
  ~KeyMemoScope() { g_keyMemo = previous; }
};
bool keyMemoApplies(const Scene &s, int index, const Layer &l, const QRectF &logical, double scale,
                    const QRectF &box, const QByteArray &hostKey) {
  if (g_keyMemo.scene != &s || g_keyMemo.layer != index || !hostKey.isEmpty()) return false;
  if ((l.type != "text" && l.type != "fill") || l.characterAnimation.value("enabled").toBool()) return false;
  if (g_keyMemo.logical != logical || g_keyMemo.box != box || g_keyMemo.scale != scale) {
    g_keyMemo.logical = logical; g_keyMemo.box = box; g_keyMemo.scale = scale;
    g_keyMemo.hasRaster = g_keyMemo.hasSource = g_keyMemo.hasPre = false;
  }
  return true;
}
}
// One raster is built by one thread. A frame drawn in bands has every band ask
// for the same layer's raster at the same moment, and when it is not in the cache
// yet they all build it -- the same picture, eight or twenty times over, which is
// what made the first frame after an edit several times dearer than the edit
// itself. The first to ask builds; the others wait here, and find it in the cache.
// Held only while building, and never while asking for another raster, so waiting
// threads cannot wait on one another.
namespace {
std::mutex g_buildGuard;
std::condition_variable g_buildDone;
std::set<QByteArray> g_building;
// Prewarm workers are called from inside a build that already holds a ticket,
// and the thread that holds it is waiting for them: if one of them waited for the
// same ticket -- two layers with the same picture -- neither could go on. They
// take no ticket and never wait; at worst a picture is made twice.
thread_local bool g_neverWaitForBuild = false;
struct BuildTicket {
  QByteArray key;
  bool registered = false;
  explicit BuildTicket(const QByteArray &wanted) : key(wanted) {
    if (g_neverWaitForBuild) return;
    std::unique_lock<std::mutex> lock(g_buildGuard);
    g_buildDone.wait(lock, [&] { return g_building.count(key) == 0; });
    g_building.insert(key);
    registered = true;
  }
  ~BuildTicket() {
    if (!registered) return;
    { std::lock_guard<std::mutex> lock(g_buildGuard); g_building.erase(key); }
    g_buildDone.notify_all();
  }
  BuildTicket(const BuildTicket &) = delete;
  BuildTicket &operator=(const BuildTicket &) = delete;
};
}
void setPrewarmWorker(bool on) { g_neverWaitForBuild = on; }
// Motion blur, one layer at a time. A layer that moves during the shutter is
// drawn once per temporal sample on its own, over a box big enough for all of
// them, the samples are averaged, and the average is laid onto the frame once.
// For ordinary (Normal) layers over a fixed backdrop that is the same picture as
// averaging whole frames -- the composite is linear in what is beneath it -- but
// it never redraws the layers that stand still, which is nearly all of a busy
// scene. Only where two moving layers overlap does it differ, and then only as
// After Effects' own per-layer blur does; an Effect Layer above them works once,
// on the blurred picture, as it does there. Off for groups and track mattes,
// which keep whole-frame averaging. A frame of 16 or 32 bits a channel blurs
// the same way, in pictures of its own depth.
namespace { thread_local bool g_layerMotionBlur = false; }
// Averaging the temporal samples of a motion-blurred layer is, per layer, a few
// hundred thousand bytes added into sixteen-bit sums once per sample and divided
// once at the end. Written plainly that was most of what motion blur cost: the
// division alone is one per byte. SSE2 is on every x86-64 CPU, and a multiply and
// a shift give exactly n / d for these sizes (n below 12 500, d up to 48), so the
// picture is the same to the last bit.
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define WAVY_ACCUM_SSE2 1
#endif
static void addBytes(quint16 *sums, const uchar *bytes, qsizetype count) {
  qsizetype i = 0;
#ifdef WAVY_ACCUM_SSE2
  const __m128i zero = _mm_setzero_si128();
  for (; i + 16 <= count; i += 16) {
    const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes + i));
    __m128i *low = reinterpret_cast<__m128i *>(sums + i);
    _mm_storeu_si128(low, _mm_add_epi16(_mm_loadu_si128(low), _mm_unpacklo_epi8(b, zero)));
    _mm_storeu_si128(low + 1, _mm_add_epi16(_mm_loadu_si128(low + 1), _mm_unpackhi_epi8(b, zero)));
  }
#endif
  for (; i < count; ++i) sums[i] = quint16(sums[i] + bytes[i]);
}
static void averageBytes(uchar *dest, const quint16 *sums, qsizetype count, unsigned divisor) {
  const unsigned half = divisor / 2;
  const quint64 reciprocal = ((quint64(1) << 32) + divisor - 1) / divisor;
  for (qsizetype i = 0; i < count; ++i)
    dest[i] = uchar((quint64(sums[i] + half) * reciprocal) >> 32);
}
// The same for a deep frame. At 16 bits a channel the values go into 32-bit
// sums -- forty-eight samples of 65 535 are three million -- the first sample
// stored rather than added, so the sums are never cleared.
static void addWords(quint32 *sums, const quint16 *words, qsizetype count, bool first) {
  qsizetype i = 0;
#ifdef WAVY_ACCUM_SSE2
  const __m128i zero = _mm_setzero_si128();
  for (; i + 8 <= count; i += 8) {
    const __m128i w = _mm_loadu_si128(reinterpret_cast<const __m128i *>(words + i));
    const __m128i low = _mm_unpacklo_epi16(w, zero), high = _mm_unpackhi_epi16(w, zero);
    __m128i *out = reinterpret_cast<__m128i *>(sums + i);
    _mm_storeu_si128(out, first ? low : _mm_add_epi32(_mm_loadu_si128(out), low));
    _mm_storeu_si128(out + 1, first ? high : _mm_add_epi32(_mm_loadu_si128(out + 1), high));
  }
#endif
  for (; i < count; ++i) sums[i] = first ? quint32(words[i]) : sums[i] + words[i];
}
// And back, each sum over `divisor` rounded to nearest -- (sum + divisor / 2) /
// divisor -- four at a time in float. The sums are below 2^24, so they convert
// exactly and the division is correctly rounded; a quotient that is not whole
// is at least 1/divisor short of the next whole number, 1/64 at the least,
// where a float below 65 536 is never more than 1/512 out; so dropping the
// fraction is the integer division, to the last bit (checked for every sum of
// every count of samples up to 64). A divisor above 64 goes the plain way.
static void averageWords(quint16 *dest, const quint32 *sums, qsizetype count, unsigned divisor) {
  const quint32 half = divisor / 2;
  qsizetype i = 0;
#ifdef WAVY_ACCUM_SSE2
  if (divisor <= 64) {
    const __m128 by = _mm_set1_ps(float(divisor));
    const __m128i rounding = _mm_set1_epi32(int(half)), offset = _mm_set1_epi32(32768);
    const __m128i sign = _mm_set1_epi16(short(-32768));
    const auto quotient = [&](qsizetype at) {
      const __m128i sum = _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(sums + at)), rounding);
      return _mm_sub_epi32(_mm_cvttps_epi32(_mm_div_ps(_mm_cvtepi32_ps(sum), by)), offset);
    };
    // SSE2 packs with signed saturation: moved down by 32 768 for it, and back.
    for (; i + 8 <= count; i += 8)
      _mm_storeu_si128(reinterpret_cast<__m128i *>(dest + i),
                       _mm_xor_si128(_mm_packs_epi32(quotient(i), quotient(i + 4)), sign));
  }
#endif
  for (; i < count; ++i) dest[i] = quint16((sums[i] + half) / divisor);
}
// At 32 bits a channel the samples are floats, added as they come and divided
// once at the end.
static void addFloats(float *sums, const float *values, qsizetype count, bool first) {
  if (first) {
    std::memcpy(sums, values, size_t(count) * sizeof(float));
    return;
  }
  qsizetype i = 0;
#ifdef WAVY_ACCUM_SSE2
  for (; i + 4 <= count; i += 4) _mm_storeu_ps(sums + i, _mm_add_ps(_mm_loadu_ps(sums + i), _mm_loadu_ps(values + i)));
#endif
  for (; i < count; ++i) sums[i] += values[i];
}
static void averageFloats(float *values, qsizetype count, float divisor) {
  qsizetype i = 0;
#ifdef WAVY_ACCUM_SSE2
  const __m128 by = _mm_set1_ps(divisor);
  for (; i + 4 <= count; i += 4) _mm_storeu_ps(values + i, _mm_div_ps(_mm_loadu_ps(values + i), by));
#endif
  for (; i < count; ++i) values[i] /= divisor;
}
// Kept from layer to layer instead of allocated and zeroed for each: a layer's
// buffers are a megabyte or more, and fresh memory of that size is paid for in
// page faults every time.
namespace {
struct BlurScratch {
  std::vector<uchar> sample, mean;
  std::vector<quint16> sums;
  std::vector<quint32> wordSums; // 16 bpc
  std::vector<float> floatSums;  // 32 bpc
};
thread_local BlurScratch g_blurScratch;
}
// Painting one layer on its own is done once per layer per temporal sample per
// flatten, so whatever such a call does for the whole scene is done that many
// times over: putting every layer in paint order (in 3D, a depth sort that builds
// a projection matrix per comparison) and scanning every layer for keyed motion
// made a sixty-layer 3D scene with motion blur cost 1.1 seconds a frame, and
// quadratically more for more layers. The caller already knows which layer it
// wants and whether the scene moves, and says so.
namespace {
thread_local int g_isolateIndexHint = -1;
thread_local int g_animatedSceneHint = -1;
// Whether a light shows in the frame, known to a pass that draws one layer
// from the frame that asked for it. -1 when nobody has said.
thread_local int g_litSceneHint = -1;
// The layer being drawn for its lights, and which of its copies: that pass
// draws the one copy's face alone, its walls lit apart.
thread_local const Layer *g_litLayer = nullptr;
thread_local int g_litCopy = -1;
struct IsolateHint {
  int previous;
  explicit IsolateHint(int index) : previous(g_isolateIndexHint) { g_isolateIndexHint = index; }
  ~IsolateHint() { g_isolateIndexHint = previous; }
};
// Painting over a kept picture of everything beneath one layer of this scene
// (renderOverPlate): the layers beneath it are in the picture already.
thread_local const Scene *g_plateScene = nullptr;
thread_local int g_plateBelow = -1;
thread_local const std::vector<int> *g_plateSkip = nullptr; // sorted: layers left out as well
}
static bool motionBlurPerLayerEligible(const Scene &s, double frame) {
  for (const auto &layer : s.layers) {
    if (!layer.visible || frame < layer.start || frame >= layer.start + layer.duration) continue;
    if (layer.type == "group" || !layer.groupId.isEmpty() ||
        !layer.trackMatteId.isEmpty())
      return false;
  }
  return true;
}
bool motionBlurPerLayer(const Scene &s, double frame) { return motionBlurPerLayerEligible(s, frame); }
static int layerBlurSampleCount(const Layer &l, const Scene &s, double t, double center,
                                QSize outputSize, bool *animated = nullptr);
MovingLayers movingLayers(const Scene &s, double frame, QSize outputSize) {
  MovingLayers count;
  for (const auto &l : s.layers) {
    if (!l.visible || frame < l.start || frame >= l.start + l.duration) continue;
    if (l.type == "camera" || l.type == "light" || l.type == "audio" || l.type == "effect") continue;
    ++count.drawn;
    if (!l.motionBlur || l.type == "group") continue;
    bool letters = false;
    const double local = std::clamp(frame - l.start, 0., std::max(0., l.duration - 1e-6));
    if (layerBlurSampleCount(l, s, local, frame, outputSize, &letters) > 0) {
      ++count.moving;
      count.letters += letters;
    }
  }
  return count;
}
LayerMotionBlurScope::LayerMotionBlurScope(bool on) : previous(g_layerMotionBlur) { if (on) g_layerMotionBlur = true; }
LayerMotionBlurScope::~LayerMotionBlurScope() { g_layerMotionBlur = previous; }
// Pixels a layer travels across the shutter, at the size being drawn.
static double layerBlurTravel(const Layer &layer, const Scene &s, double frame, QSize outputSize) {
  const double scaleX = double(outputSize.width()) / std::max(1, s.width);
  const double scaleY = double(outputSize.height()) / std::max(1, s.height);
  const double firstTime = std::clamp(frame - .25, layer.start, layer.start + layer.duration - 1e-6);
  const double lastTime = std::clamp(frame + .25, layer.start, layer.start + layer.duration - 1e-6);
  QRectF bounds = repeatedLayerBounds(layer, layerBounds(layer, s));
  QTransform first = layerWorldTransform(layer, s, firstTime);
  QTransform last = layerWorldTransform(layer, s, lastTime);
  if (layerHasMainTransform(layer)) { first *= layerMainTransform(layer); last *= layerMainTransform(layer); }
  double travel = 0;
  for (const QPointF &probe : {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(),
                               bounds.bottomRight(), bounds.center()}) {
    const QPointF a = first.map(probe), b = last.map(probe);
    travel = std::max(travel, std::hypot((b.x() - a.x()) * scaleX, (b.y() - a.y()) * scaleY));
  }
  return travel;
}
// How many times a layer is sampled across the shutter when it blurs itself, or
// 0 when it does not move enough to need it. `t` is the layer's own time and
// `center` the composition time; `animated` says whether its letters are what
// moves (their raster then differs from sample to sample).
static int layerBlurSampleCountNow(const Layer &l, const Scene &s, double t, double center,
                                   QSize outputSize, bool *animated);
static int layerBlurSampleCount(const Layer &l, const Scene &s, double t, double center,
                                QSize outputSize, bool *animated) {
  RenderFrameContext *const memo = renderFrame.get();
  if (!memo) return layerBlurSampleCountNow(l, s, t, center, outputSize, animated);
  const auto key = std::make_tuple(&l, l.id, t, center, outputSize.width(), outputSize.height(), previewMotionBlurCap(s));
  {
    std::lock_guard<std::mutex> lock(memo->guard);
    const auto found = memo->blurSamples.find(key);
    if (found != memo->blurSamples.end()) {
      if (animated) *animated = found->second.second;
      return found->second.first;
    }
  }
  bool letters = false;
  const int samples = layerBlurSampleCountNow(l, s, t, center, outputSize, &letters);
  if (animated) *animated = letters;
  std::lock_guard<std::mutex> lock(memo->guard);
  memo->blurSamples.emplace(key, std::make_pair(samples, letters));
  return samples;
}
static int layerBlurSampleCountNow(const Layer &l, const Scene &s, double t, double center,
                                   QSize outputSize, bool *animated) {
  const bool animatedContent = l.characterAnimation.value("enabled").toBool() &&
                               !l.hiddenEffects.value("Text-Animation").toBool() &&
                               !(characterPoseAtRest(l, t - .6) && characterPoseAtRest(l, t) && characterPoseAtRest(l, t + .6));
  if (animated) *animated = animatedContent;
  const double layerTravel = layerBlurTravel(l, s, center, outputSize);
  double travel = layerTravel, lettersTravel = 0;
  if (animatedContent) {
    // What the letters themselves do across the shutter, from their poses,
    // at the size being drawn. Text arriving or leaving moves a pixel or so a
    // frame; sampling it eight times over rebuilt eight rasters of effects
    // for a blur nobody can see.
    const int count = std::clamp(int(l.text.size()), 1, 400);
    const double pixel = double(outputSize.width()) / std::max(1, s.width) * std::max(std::abs(l.p[2].at(t)), std::abs(l.p[3].at(t)));
    double letters = 0;
    for (int rank = 0; rank < count; ++rank) {
      const CharacterPose a = characterPose(l, t - .25, rank, count, s.fps), b = characterPose(l, t + .25, rank, count, s.fps);
      letters = std::max({letters, std::hypot(b.offset.x() - a.offset.x(), b.offset.y() - a.offset.y()),
                          std::abs(b.scale - a.scale) * l.fontSize,
                          std::abs(b.rotation - a.rotation) * M_PI / 180. * l.fontSize * .5,
                          std::abs(b.opacity - a.opacity) * 4.});
    }
    lettersTravel = letters * pixel;
    travel = std::max(travel, lettersTravel);
  }
  // Letters arriving or leaving move a pixel or two a frame: a blur that short
  // is invisible, and each of its samples is a raster of effects. Only the layer
  // itself moving is blurred from half a pixel.
  if (layerTravel < .5 && lettersTravel < 2.5) return 0;
  // The same count whole-frame averaging would have used for this motion,
  // never fewer than eight, so a short move is as smooth as it was. Text
  // whose letters are what moves is sampled by how far they go, and may be
  // as few as three: each sample is a raster of its own.
  int samples = std::clamp(int(std::ceil(travel / 4.)) + 1, animatedContent ? 3 : 8, 48);
  const int cap = previewMotionBlurCap(s);
  if (cap > 0) {
    // Playing, a layer is sampled by how far it actually goes, a sample for
    // every four pixels and two at least, not the eight a render takes for
    // the smoothest short move: a big disc turning a few pixels a frame drew
    // its whole raster four times over, every frame, for a blur of three pixels.
    samples = std::min(std::max(2, int(std::ceil(travel / 4.)) + 1), cap);
    // Playing, letters on the move take half the samples again: each of them
    // is a raster of the text and all its effects built from nothing (four of
    // them made a shadowed title 110 ms a frame), and at playing speed two
    // read as the same smear. A render keeps every sample.
    if (animatedContent) samples = std::min(samples, std::max(2, cap / 2));
  }
  return samples;
}
// Whether so many of the frame's layers are blended or matted that the CPU
// draws it faster. Each of those is flattened on its own and combined with the
// frame beneath it, which the card's painter can only do by reading the frame
// back, blending it in software and putting it up again -- correct, and about
// as costly as doing it here. Everything else on the card is much cheaper than
// on the CPU, so a few blended layers among many ordinary ones still go to it.
// The part of a premultiplied raster that is not transparent, with three pixels
// of transparent margin so drawing only that part samples exactly the neighbours
// the whole picture would. One pixel was not enough: drawn turned and enlarged,
// the antialiased edge of the sub-rectangle itself reaches a pixel or two inside
// it, and took a corner of the picture with it. Nearly every raster is mostly
// margin -- a glow's room to spread, or a frame-sized layer inside a feathered
// mask -- and drawing all of it for every position and every motion-blur sample
// is drawing millions of pixels that add nothing. Found once per raster and
// remembered.
static QRect inkBounds(const QByteArray &identity, const QImage &image) {
  static QMutex guard;
  static QCache<QByteArray, QRect> known{8192};
  {
    QMutexLocker lock(&guard);
    if (const QRect *hit = known.object(identity)) return *hit;
  }
  const int w = image.width(), h = image.height();
  int left = w, right = -1, top = h, bottom = -1;
  for (int y = 0; y < h; ++y) {
    const auto *row = reinterpret_cast<const quint32 *>(image.constScanLine(y));
    int first = 0;
    while (first < w && (row[first] >> 24) == 0) ++first;
    if (first == w) continue;
    int last = w - 1;
    while (last > first && (row[last] >> 24) == 0) --last;
    left = std::min(left, first); right = std::max(right, last);
    top = std::min(top, y); bottom = y;
  }
  QRect box = right < left ? QRect(0, 0, 1, 1)
                           : QRect(QPoint(left - 3, top - 3), QPoint(right + 3, bottom + 3))
                                 .intersected(QRect(0, 0, w, h));
  if (box.isEmpty()) box = QRect(0, 0, 1, 1);
  QMutexLocker lock(&guard);
  known.insert(identity, new QRect(box));
  return box;
}
// Where, in device pixels, a layer can put anything at all -- or false when that
// cannot be told from the layer's own description. A layer that is blended or
// matted is flattened on its own first, and doing that over the whole frame for
// a layer that covers a corner of it is what made each one cost a full frame
// of clearing, drawing and blending. The box is generous on purpose: every
// effect reaches at most three times its radius, and what is drawn inside a box
// that is too big is the same as what is drawn inside the frame.
static bool layerDeviceRect(const Layer &l, const Scene &s, double t, const QTransform &toDevice,
                            QSize device, QRect &result, bool allowProjective = false) {
  if ((l.threeD && !allowProjective) || l.repeaterEnabled || l.type == "group" || l.type == "camera" || l.type == "light" || l.type == "empty" || l.type == "audio" ||
      layerIsStructure(l))
    return false;
  if (l.threeD && layerExtrudeDepth(l, t) > 0) return false; // the walls reach beyond the layer's own box
  if (particleVolume(l)) return false; // each particle is somewhere of its own in depth
  // A plugin can draw beyond the raster it was given, and animated characters
  // leave the box their document reports.
  const auto *host = hostLayerEffects();
  if (host && host->active && host->active(l)) return false;
  // Letters on the move are wherever their animation has them now
  // (animatedTextBounds) -- unless a pixel effect, which can carry pixels
  // anywhere, works on them. Unknown, a moving title was blurred in buffers the
  // size of the whole frame, twenty milliseconds of a thirty millisecond layer.
  QRectF letters;
  if (l.characterAnimation.value("enabled").toBool() && !l.hiddenEffects.value("Text-Animation").toBool() &&
      !(characterPoseAtRest(l, t - .6) && characterPoseAtRest(l, t) && characterPoseAtRest(l, t + .6))) {
    if (layerHasPixelEffect(l) || l.particlesOn) return false;
    letters = animatedTextBounds(l, t, s.fps);
    if (letters.isNull()) return false;
  }
  const QTransform world = layerWorldTransform(l, s, t + l.start);
  if (!world.isAffine() && !allowProjective) return false;
  QRectF bounds = letters.isNull() ? layerBounds(l, s) : letters;
  if (layerHasMainTransform(l)) bounds = layerMainTransform(l).mapRect(bounds);
  // A layer inside a mask shows nothing outside it, but for the feather's own
  // reach: half the feather to either side of the edge. The mask lives in the
  // layer's own coordinates, before the main transform, which is where the
  // bounds are by now. A feathered mask is applied after the effects, so those
  // reach no further than it does; a hard one clips first, and a glow still
  // spills over its edge. An inverted mask shows everything outside, and a mask
  // with no points masks nothing.
  // Where the layer ends, not a guess above it: its raster reaches 1.5 times the
  // radius of each spatial effect beyond the layer and a dozen pixels more, and
  // nothing is drawn outside the raster. Twenty pixels leaves margin for the
  // shadow offset and the antialiased edge, and a box twice as wide as it needs
  // to be is cleared, drawn into and summed twice as many pixels for every sample.
  double pad = (l.blur + l.glow + l.shadow) * 1.5 + l.strokeWidth + textStrokeOutwardReach(l) +
               strokeOutwardReach(l) + rgbSplitReach(l) + 20;
  {
    QRectF maskBox;
    double widestFeather = 0;
    bool masked = false, inverted = false;
    for (const auto &value : l.masks) {
      const auto mask = value.toObject();
      if (!mask["enabled"].toBool(true) || mask["points"].toArray().isEmpty()) continue;
      masked = true;
      if (mask["inverted"].toBool()) inverted = true;
      widestFeather = std::max(widestFeather, mask["feather"].toDouble());
      QJsonObject upright = mask;
      upright["inverted"] = false;
      maskBox = maskBox.united(expandedMaskPath(upright, l, s).boundingRect());
    }
    if (masked && !inverted) {
      const double margin = widestFeather > 0 ? widestFeather * .5 + 4 : 0;
      bounds = bounds.intersected(maskBox.adjusted(-margin, -margin, margin, margin));
      if (widestFeather > 0) pad = 8;
      if (bounds.isNull() || bounds.isEmpty()) bounds = QRectF(maskBox.center(), QSizeF(0, 0));
    }
  }
  bounds.adjust(-pad, -pad, pad, pad);
  // A projected plane is bounded by where its four corners land, provided every
  // one of them is in front of the camera; a corner behind it lands somewhere
  // meaningless, and then the box cannot be trusted.
  if (!world.isAffine())
    for (const QPointF &corner : {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight()})
      if (world.m13() * corner.x() + world.m23() * corner.y() + world.m33() <= 1e-6) return false;
  const QRectF mapped = toDevice.mapRect(world.mapRect(bounds));
  if (!std::isfinite(mapped.left()) || !std::isfinite(mapped.top()) ||
      !std::isfinite(mapped.width()) || !std::isfinite(mapped.height()))
    return false;
  result = mapped.toAlignedRect().adjusted(-2, -2, 2, 2).intersected(QRect(QPoint(), device));
  return true;
}
// The device pixels one copy of a lit layer can reach, given the transform
// that takes its plane there. The whole device when that cannot be told.
static QRect litCopyDeviceRect(const Layer &l, const Scene &s, double t, const QTransform &localToDevice,
                               const QRect &device) {
  const auto *host = hostLayerEffects();
  if (host && host->active && host->active(l)) return device;
  if (l.characterAnimation.value("enabled").toBool() && !l.hiddenEffects.value("Text-Animation").toBool() &&
      !(characterPoseAtRest(l, t - .6) && characterPoseAtRest(l, t) && characterPoseAtRest(l, t + .6)))
    return device;
  QRectF bounds = layerBounds(l, s);
  if (layerHasMainTransform(l)) bounds = layerMainTransform(l).mapRect(bounds);
  const double pad = (l.blur + l.glow + l.shadow) * 1.5 + l.strokeWidth + textStrokeOutwardReach(l) +
                     strokeOutwardReach(l) + rgbSplitReach(l) + 20;
  bounds.adjust(-pad, -pad, pad, pad);
  for (const QPointF &corner : {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight()})
    if (localToDevice.m13() * corner.x() + localToDevice.m23() * corner.y() + localToDevice.m33() <= 1e-6) return device;
  const QRectF mapped = localToDevice.mapRect(bounds);
  if (!std::isfinite(mapped.left()) || !std::isfinite(mapped.top()) ||
      !std::isfinite(mapped.width()) || !std::isfinite(mapped.height()))
    return device;
  return mapped.toAlignedRect().adjusted(-2, -2, 2, 2).intersected(device);
}
// Whether this layer's on-screen size changes over time: its own Scale is keyed,
// or a layer it is linked to has a keyed Scale.
static bool layerScaleAnimated(const Layer &layer, const Scene &s) {
  const Layer *current = &layer;
  for (int depth = 0; current && depth < 16; ++depth) {
    if (current->p[2].varies() || current->p[3].varies()) return true;
    if (current->parentId.isEmpty()) return false;
    const Layer *parent = nullptr;
    for (const auto &candidate : s.layers) if (candidate.id == current->parentId) { parent = &candidate; break; }
    current = parent;
  }
  return false;
}
// The scale a zooming layer's one halo (a drop shadow or a glow, nothing
// else) is resolved at once the screen is finer than its blur needs: set by
// the blur alone, in half-octave steps of eight to twelve pixels of blur, and
// whole, so every frame and every motion-blur sample of the zoom draws the
// same picture enlarged. 0 where the halo is too large to keep whole. (At
// sixteen pixels the first halo of a disc the width of the frame took 97 ms
// to build, in the middle of a frame.)
static double steadyHaloScale(double radius, const QRectF &logical) {
  if (!(radius > 0)) return 0;
  const double steady = std::pow(2., std::ceil(std::log2(8. / radius) * 2. - 1e-6) / 2.);
  return logical.width() * steady * logical.height() * steady <= 4194304. ? steady : 0;
}
// Whether anything in the scene moves over time: any transform, camera or
// plugin curve with more than one key. Decided from the scene's content alone,
// so a still frame and a frame reached by playing show the same picture.
static bool sceneHasKeyedMotion(const Scene &s) {
  for (const auto &layer : s.layers) {
    for (const auto &property : layer.p) if (property.varies()) return true;
    for (const auto &property : layer.extraCurves) if (property.varies()) return true;
  }
  return false;
}
// ---------------------------------------------------------------------------
// Effect Layers.
//
// A layer with no picture of its own. Where it sits in the stack, it takes the
// picture made so far from every layer beneath it, runs its own effects over
// that, and puts the result back -- only within its own box (which it can move,
// turn and scale like any layer), only inside its masks, and at its opacity. A
// grade, a blur, a noise or a wipe put on it reaches everything below it at
// once, and nothing above it.
// ---------------------------------------------------------------------------
static bool effectLayerActive(const Layer &l, double frame) {
  if (l.type != "effect" || !l.visible) return false;
  const double at = frame - l.start;
  if (!(at >= 0 && at < l.duration)) return false;
  // One that changes nothing at this frame is not there at all: faded out, or
  // every effect on it at rest (a zoom that only hits on the beat, a tear only
  // where a part begins). Counted as there, it took the frame out of bands --
  // off every core but one -- for all of its length, not just where it acts.
  if (l.p[5].at(at) <= 0) return false;
  if ((l.blur && !l.hiddenEffects["Gaussian Blur"].toBool()) || (l.glow && !l.hiddenEffects["Glow"].toBool()) ||
      (l.shadow && !l.hiddenEffects["Drop Shadow"].toBool()))
    return true;
  for (auto it = l.appliedEffects.constBegin(); it != l.appliedEffects.constEnd(); ++it) {
    if (!it.value().toBool() || l.hiddenEffects[it.key()].toBool()) continue;
    const PackEffect *pack = packEffect(it.key());
    if (!pack || !packEffectInert(*pack, packEffectValues(l, *pack, at))) return true;
  }
  return false;
}
bool sceneUsesEffectLayers(const Scene &s, double frame) {
  return std::any_of(s.layers.begin(), s.layers.end(),
                     [frame](const Layer &l) { return effectLayerActive(l, frame); });
}
bool effectLayersRunOnCard(const Scene &s, double frame) {
  for (const Layer &l : s.layers) {
    if (!effectLayerActive(l, frame)) continue;
    const double t = frame - l.start;
    EffectTimeScope time(t / std::max(1, s.fps), t);
    if (layerEffectPasses(l, QRectF(0, 0, s.width, s.height), 1, QSize(s.width, s.height), true).isEmpty())
      return false;
  }
  return true;
}
// Whether an acting Effect Layer only recolours: every effect on it one that
// treats each pixel on its own (a pack effect with a span -- by the SDK's word,
// nothing about it depends on where the pixel is or what lies around it). Such
// a layer gives the same picture worked on a strip at a time as worked on the
// whole, so each band grades its own rows where the layer lies in the stack,
// on its own core. Cutting the frame there instead meant two runs of bands,
// their set-up twice, and the grade itself as one more step after them -- a
// saturation grade over a song's second verse cost more than everything above it.
static bool effectLayerPerPixel(const Layer &l, double frame) {
  if ((l.blur && !l.hiddenEffects["Gaussian Blur"].toBool()) || (l.glow && !l.hiddenEffects["Glow"].toBool()) ||
      (l.shadow && !l.hiddenEffects["Drop Shadow"].toBool()))
    return false;
  const double at = frame - l.start;
  for (auto it = l.appliedEffects.constBegin(); it != l.appliedEffects.constEnd(); ++it) {
    if (!it.value().toBool() || l.hiddenEffects[it.key()].toBool()) continue;
    const PackEffect *pack = packEffect(it.key());
    if (!pack) return false;
    if (pack->api->span && pack->api->prepare && pack->api->release) continue;
    if (!packEffectInert(*pack, packEffectValues(l, *pack, at))) return false;
  }
  return true;
}
// Whether an Effect Layer can be worked on part of the picture beneath it --
// its effects told where that part lies -- and give there what the whole
// would: every effect on it at rest, a colour grade worked a span at a time,
// or an effect worked pixel by pixel by its own process() (Film Grain keyed to
// the raster, Vignette to the box). Nothing built in -- blur, glow, shadow,
// fill, gradient, stroke -- and no plug-in.
static bool effectLayerPartable(const Layer &l, double frame) {
  if ((l.blur && !l.hiddenEffects["Gaussian Blur"].toBool()) || (l.glow && !l.hiddenEffects["Glow"].toBool()) ||
      (l.shadow && !l.hiddenEffects["Drop Shadow"].toBool()))
    return false;
  if (const auto *host = hostLayerEffects(); host && host->active && host->active(l)) return false;
  const double at = frame - l.start;
  for (auto it = l.appliedEffects.constBegin(); it != l.appliedEffects.constEnd(); ++it) {
    if (!it.value().toBool() || l.hiddenEffects[it.key()].toBool()) continue;
    const PackEffect *pack = packEffect(it.key());
    if (!pack) return false;
    if (packEffectInert(*pack, packEffectValues(l, *pack, at))) continue;
    if (!packEffectPartable(*pack)) return false;
  }
  return true;
}
// An Effect Layer acting here that needs the whole picture beneath it at once.
static bool effectLayerNeedsWhole(const Layer &l, double frame) {
  return effectLayerActive(l, frame) && !effectLayerPerPixel(l, frame);
}
static bool sceneNeedsWholeForEffects(const Scene &s, double frame) {
  return std::any_of(s.layers.begin(), s.layers.end(),
                     [frame](const Layer &l) { return effectLayerNeedsWhole(l, frame); });
}
// Whether an Effect Layer has anything to do. One carrying no effect (or only
// hidden ones) is left out entirely: copying the picture beneath it and putting
// it back unchanged costs a few milliseconds a frame for nothing.
static bool effectLayerDoesSomething(const Layer &l) {
  if ((l.blur && !l.hiddenEffects["Gaussian Blur"].toBool()) ||
      (l.glow && !l.hiddenEffects["Glow"].toBool()) ||
      (l.shadow && !l.hiddenEffects["Drop Shadow"].toBool()))
    return true;
  for (auto it = l.appliedEffects.constBegin(); it != l.appliedEffects.constEnd(); ++it)
    if (it.value().toBool() && !l.hiddenEffects[it.key()].toBool()) return true;
  return false;
}
namespace {
thread_local const EffectLayerHook *g_effectLayerHook = nullptr;
}
EffectLayerHookScope::EffectLayerHookScope(const EffectLayerHook *hook) : previous(g_effectLayerHook) {
  g_effectLayerHook = hook;
}
EffectLayerHookScope::~EffectLayerHookScope() { g_effectLayerHook = previous; }
static void applyEffectLayer(QPainter &p, const Layer &l, const Scene &s, double t) {
  if (!effectLayerDoesSomething(l)) return;
  auto *target = dynamic_cast<QImage *>(p.device());
  // Drawn on the card, the picture beneath is there: the GPU compositor works
  // the layer where it is, from the same area, reach and scale as below.
  const EffectLayerHook *onCard = target ? nullptr : g_effectLayerHook;
  if ((!target || target->isNull()) && !onCard) return;
  const QTransform layerToDevice = layerWorldTransform(l, s, t + l.start) * p.worldTransform();
  // Where it acts: its own box as it has been placed, cut to its masks.
  QPainterPath box;
  box.addRect(layerBounds(l, s));
  if (layerHasMainTransform(l)) box = layerMainTransform(l).map(box);
  QPainterPath area = layerToDevice.map(box);
  for (const auto &value : l.masks) {
    const auto mask = value.toObject();
    if (!mask["enabled"].toBool(true) || mask["points"].toArray().isEmpty()) continue;
    area = area.intersected(layerToDevice.map(expandedMaskPath(mask, l, s)));
  }
  const QRect image(QPoint(), target ? target->size() : QSize(p.device()->width(), p.device()->height()));
  const QRect acting = area.boundingRect().toAlignedRect().intersected(image);
  if (acting.isEmpty()) return;
  // Effects are sized in layer pixels; the picture here is in output pixels.
  const double scale = std::max(.0001, std::max(std::hypot(layerToDevice.m11(), layerToDevice.m12()),
                                                 std::hypot(layerToDevice.m21(), layerToDevice.m22())));
  // A blur or a glow reads the picture around what it changes, so it is given
  // that much more of it to read. Beyond the frame there is nothing to give.
  const int reach = qCeil(((l.blur + l.glow + l.shadow) * 3 + rgbSplitReach(l)) * scale) + 4;
  const QRect source = acting.adjusted(-reach, -reach, reach, reach).intersected(image);
  const double opacity = std::clamp(l.p[5].at(t), 0., 1.);
  if (onCard) {
    (*onCard)(p, l, s, t, area, acting, source, scale, opacity);
    return;
  }
  // A pass drawing only part of the picture -- where a dragged layer is
  // (renderOverPlate) -- has an Effect Layer that can be so worked there alone:
  // its effects told the part lies where it does in the raster the whole would
  // have handed them, and given that raster's box, so a grain keyed to the
  // raster or a vignette to the box comes out the same. On the processor: the
  // card's passes are told no such thing.
  QRect read = source;
  std::optional<RasterPartScope> part;
  std::optional<CpuPixelScope> processor;
  if (p.hasClipping() && !target->colorSpace().isValid() && effectLayerPartable(l, t + l.start)) {
    const QRect working = acting.intersected(p.worldTransform().mapRect(p.clipBoundingRect()).toAlignedRect());
    if (working.isEmpty()) return;
    if (working != acting) {
      read = working.adjusted(-reach, -reach, reach, reach).intersected(image);
      part.emplace(read.topLeft() - source.topLeft(), source.size());
      processor.emplace();
    }
  }
  QImage beneath = target->copy(read);
  const QRectF content(QPointF(acting.topLeft() - source.topLeft()), QSizeF(acting.size()));
  EffectTimeScope effectTime(t / std::max(1, s.fps), t); // what moving noise reads
  const QImage effected = applyLayerEffects(beneath, l, content, scale, t);
  if (effected.isNull() || effected.size() != beneath.size()) return;
  // Put back, not laid over: where the effects thinned or moved the picture (a
  // blur softening an edge, a wipe taking it away) what was there must go, so
  // the result replaces the picture beneath -- in proportion to the layer's
  // opacity, which is what Source composition with an opacity does.
  p.save();
  p.resetTransform();
  p.setClipPath(area, Qt::IntersectClip);
  p.setCompositionMode(QPainter::CompositionMode_Source);
  p.setOpacity(opacity);
  p.drawImage(read.topLeft(), effected);
  p.restore();
}
// The usual case -- one layer grading the whole frame, fully on, no masks --
// runs its effects on the picture where it lies: no copy of it, nothing put
// back, and none of the conversions a painter needs. False where it is not that
// case, and the picture is left as it was.
static bool applyEffectLayerWhole(QImage &picture, const Layer &effect, const Scene &s, double frame, QSize outputSize) {
  // Its clock as the frame's own pass reads it, Posterize Time included.
  const double t = posterizedTime(effect, s, std::clamp(frame - effect.start, 0., std::max(0., effect.duration - 1e-6)));
  const QTransform layerToDevice =
      layerWorldTransform(effect, s, t + effect.start) *
      QTransform::fromScale(double(outputSize.width()) / s.width, double(outputSize.height()) / s.height);
  QPainterPath box;
  box.addRect(layerBounds(effect, s));
  if (layerHasMainTransform(effect)) box = layerMainTransform(effect).map(box);
  for (const auto &value : effect.masks) {
    const auto mask = value.toObject();
    if (mask["enabled"].toBool(true) && !mask["points"].toArray().isEmpty()) return false;
  }
  if (effect.threeD || effect.p[5].at(t) < 1 || picture.size() != outputSize) return false;
  // Every pixel centre, which is what "covers the frame" means; the frame's own
  // edges lie on the box's and count as outside.
  if (!layerToDevice.map(box).contains(QRectF(picture.rect()).adjusted(.5, .5, -.5, -.5))) return false;
  const double scale = std::max(.0001, std::max(std::hypot(layerToDevice.m11(), layerToDevice.m12()),
                                                 std::hypot(layerToDevice.m21(), layerToDevice.m22())));
  EffectTimeScope effectTime(t / std::max(1, s.fps), t);
  // Handed over rather than shared: a picture still held here would be copied
  // whole -- eight megabytes at 1080p -- the moment the effect wrote to it.
  const QRectF whole(picture.rect());
  picture = applyLayerEffects(std::move(picture), effect, whole, scale, t);
  return true;
}
// ---------------------------------------------------------------------------
// Where every layer lands, worked out once for the whole frame.
//
// A frame is drawn in horizontal bands, one per core, and each band walks the
// whole scene to find out which layers reach its own rows. Everything that
// decides that -- the layer's transform, its box, the room its blur and glow
// and stroke need -- is the same in every band, and was being worked out again
// in each of them: a thousand layers over sixteen bands meant sixteen thousand
// answers to a thousand questions.
//
// So it is answered once, before the bands start, and they look the answer up.
// A band can then pass over a layer it does not touch without working out
// anything about it at all. The plan is built and installed before any worker
// starts and taken down after they have all joined, so nothing reads it while
// it is being written.
// ---------------------------------------------------------------------------
namespace {
struct CompositionReach {
  const void *scene = nullptr;
  double frame = 0, center = 0;
  // Per layer: the box it fills in composition coordinates, and what the box
  // means. Unknown layers (text, groups) are left to the old test below.
  enum State : quint8 { Unknown = 0, Bounded = 1, Invisible = 2 };
  std::vector<QRectF> bounds;
  std::vector<quint8> state;
};
std::shared_ptr<const CompositionReach> g_reachPlan;
QMutex &reachPlanGuard() { static QMutex guard; return guard; }
std::shared_ptr<const CompositionReach> currentReachPlan() {
  QMutexLocker lock(&reachPlanGuard());
  return g_reachPlan;
}
void installReachPlan(std::shared_ptr<const CompositionReach> plan) {
  QMutexLocker lock(&reachPlanGuard());
  g_reachPlan = std::move(plan);
}
bool g_reachPlansAllowed = true;
}  // namespace
// Whether frames may be planned this way at all. Only the self-test turns it
// off, to draw the same frame both ways and compare them pixel for pixel.
void setCompositionPlanning(bool on) { g_reachPlansAllowed = on; }
bool compositionPlanning() { return g_reachPlansAllowed; }
// The box a layer fills in the composition, with room for everything its
// effects reach beyond it. False means the layer cannot be seen this frame.
//
// This is the one description of where a layer lands: the bands use it through
// the plan, and a frame drawn without a plan calls it directly, so the two
// cannot drift apart.
// Where a layer lands, grown by what its extruded sides reach beyond its face
// (`face` is its box in its own space, `landed` where the face lands).
static QRectF withExtrudedWalls(const Layer &l, const Scene &s, double frame, int copy, const QRectF &face,
                                const QRectF &landed) {
  if (!l.threeD) return landed;
  const QRectF walls = extrudedWallReach(l, s, frame, copy, face);
  return walls.isEmpty() ? landed : landed.united(walls);
}
static bool layerCompositionReach(const Layer &original, const Scene &s, double frame,
                                  double center, QRectF &bounds) {
  // Not on at this frame, nor within the shutter round it: nowhere. (A layer
  // in a group keeps the group's time.) Asked of every layer of a long music
  // video for every frame, the letters of hundreds of lines not on yet were
  // posed three times each to find where they would have been.
  if (original.groupId.isEmpty() &&
      (frame < original.start - .5 || frame >= original.start + original.duration + .5))
    return false;
  std::optional<Layer> adjusted;
  if ((original.blur && original.hiddenEffects["Gaussian Blur"].toBool()) ||
      (original.glow && original.hiddenEffects["Glow"].toBool()) ||
      (original.shadow && original.hiddenEffects["Drop Shadow"].toBool())) {
    adjusted.emplace(original);
    if (original.hiddenEffects["Gaussian Blur"].toBool()) adjusted->blur = 0;
    if (original.hiddenEffects["Glow"].toBool()) adjusted->glow = 0;
    if (original.hiddenEffects["Drop Shadow"].toBool()) adjusted->shadow = 0;
  }
  const double dof =
      layerDepthOfFieldBlur(original, s, original.motionBlur ? frame : center);
  if (dof > 0) {
    if (!adjusted) adjusted.emplace(original);
    adjusted->blur += dof;
    adjusted->hiddenEffects["Gaussian Blur"] = false;
  }
  const Layer &l = adjusted ? *adjusted : original;
  const bool strokeEffect = l.appliedEffects["Stroke"].toBool() &&
                            !l.hiddenEffects["Stroke"].toBool() && !vectorEffectStroke(l);
  const double effectPad =
      ((strokeEffect || vectorEffectStroke(l)) ? strokeOutwardReach(l) : 0) + rgbSplitReach(l);
  const double t = (l.motionBlur ? frame : center) - l.start;
  // A title's letters on the move, where they are rather than everywhere they
  // could go: the bands their full reach crossed all walked them for nothing.
  bounds = movingLettersBounds(l, s, t);
  if (bounds.isNull()) bounds = layerBounds(l, s);
  // Where the layer ends up, not where it started: the composition is
  // rendered in horizontal bands, so a cull that ignored the main
  // transform dropped the layer out of the bands it had moved into.
  if (layerHasMainTransform(l)) bounds = layerMainTransform(l).mapRect(bounds);
  const double pad = (l.blur + l.glow + l.shadow) * 3 + l.strokeWidth + effectPad + 20 +
                     hostEffectReach(l, s, t, bounds.size());
  bounds.adjust(-pad, -pad, pad, pad);
  if (l.threeD && l.repeaterEnabled) {
    QRectF projected;
    bool hasProjectedCopy = false;
    for (int copy = 0; copy < repeaterCopyCount(l); ++copy) {
      if (!repeaterCopyInFrontOfCamera(l, s, t + l.start, copy)) continue;
      const QRectF copyBounds = withExtrudedWalls(l, s, t + l.start, copy, bounds,
                                                  projectRepeaterCopy(l, s, t + l.start, copy).mapRect(bounds));
      projected = hasProjectedCopy ? projected.united(copyBounds) : copyBounds;
      hasProjectedCopy = true;
    }
    if (!hasProjectedCopy) return false;
    bounds = projected;
    return true;
  }
  const QRectF face = bounds;
  bounds = withExtrudedWalls(l, s, t + l.start, 0, face,
                             layerWorldTransform(l, s, t + l.start).mapRect(repeatedLayerBounds(l, face)));
  return true;
}
// A 3D particle layer whose particles stand in depth is not one plane to be
// sorted among the others: a character can stand in the middle of it. So it
// is cut into slices at the depths of the other 3D layers that draw now, and
// each slice goes where its depth puts it -- after every layer it is in front
// of, before every layer it is behind. That holds for 3D layers anywhere in
// the stack, not only the ones next to it: a 2D layer between the particles
// and the character (a title, an Effect Layer, the music) cut them apart, and
// the character fell out of the particles. A slice keeps the particle layer's
// own place in the stack whenever its depth allows, so 2D layers keep their
// order to it. Slices that hold no particles are not drawn.
namespace {
thread_local std::pair<double, double> g_volumeWindow{-1e300, 1e300};
struct VolumeWindowScope {
  std::pair<double, double> previous;
  explicit VolumeWindowScope(std::pair<double, double> window) : previous(g_volumeWindow) { g_volumeWindow = window; }
  ~VolumeWindowScope() { g_volumeWindow = previous; }
};
}  // namespace
static void sliceParticleVolumes(const Scene &s, double frame, std::vector<int> &order,
                                 std::vector<std::pair<double, double>> &windows) {
  const std::pair<double, double> whole{-1e300, 1e300};
  windows.assign(order.size(), whole);
  if (std::none_of(order.begin(), order.end(), [&](int i) { return particleVolume(s.layers[size_t(i)]); })) return;
  // The order without the volumes, and the place each volume had in it.
  std::vector<int> base;
  std::vector<std::pair<int, size_t>> volumes;
  for (int i : order) {
    if (particleVolume(s.layers[size_t(i)])) volumes.push_back({i, base.size()});
    else base.push_back(i);
  }
  // Every 3D layer that draws now, far to near. Only those are somewhere to
  // cut: one that is hidden or not on yet would split the particles for nothing.
  struct Wall { double depth; size_t at; };
  std::vector<Wall> walls;
  for (size_t k = 0; k < base.size(); ++k) {
    const Layer &o = s.layers[size_t(base[k])];
    if (o.threeD && o.visible && frame >= o.start && frame < o.start + o.duration)
      walls.push_back({layerCameraDepth(o, s, frame), k});
  }
  std::stable_sort(walls.begin(), walls.end(), [](const Wall &a, const Wall &b) { return a.depth > b.depth; });
  // Before every nearer wall: the lowest place among the walls from w on.
  std::vector<size_t> beforeNearer(walls.size() + 1, base.size());
  for (size_t w = walls.size(); w-- > 0;) beforeNearer[w] = std::min(beforeNearer[w + 1], walls[w].at);
  struct Piece { size_t before; double far; int layer; std::pair<double, double> window; };
  std::vector<Piece> pieces;
  for (const auto &[layer, own] : volumes) {
    const Layer &l = s.layers[size_t(layer)];
    double nearest = 0, farthest = 0;
    if (!particleVolumeDepths(l, s, frame - l.start, nearest, farthest)) {
      pieces.push_back({own, 1e300, layer, whole});
      continue;
    }
    double farther = 1e300;
    size_t afterFarther = 0;
    for (size_t w = 0; w <= walls.size(); ++w) {
      const double nearer = w < walls.size() ? walls[w].depth : -1e300;
      // [nearer, farther): the particles between this wall and the one before.
      if (farthest >= nearer && nearest < farther) {
        const size_t lowest = afterFarther, highest = std::max(lowest, beforeNearer[w]);
        pieces.push_back({std::clamp(own, lowest, highest), farther, layer, {nearer, farther}});
      }
      if (w == walls.size()) break;
      farther = walls[w].depth;
      afterFarther = std::max(afterFarther, walls[w].at + 1);
    }
  }
  std::stable_sort(pieces.begin(), pieces.end(), [](const Piece &a, const Piece &b) {
    return a.before != b.before ? a.before < b.before : a.far > b.far;
  });
  std::vector<int> cut;
  std::vector<std::pair<double, double>> cutWindows;
  size_t next = 0;
  for (size_t k = 0; k <= base.size(); ++k) {
    for (; next < pieces.size() && pieces[next].before == k; ++next) {
      cut.push_back(pieces[next].layer);
      cutWindows.push_back(pieces[next].window);
    }
    if (k == base.size()) break;
    cut.push_back(base[k]);
    cutWindows.push_back(whole);
  }
  order.swap(cut);
  windows.swap(cutWindows);
}
// Answers the question for every layer of a frame, for the bands to share.
// Text is left unanswered: it is drawn wherever it falls, because animated
// characters travel beyond the box the document reports.
//
// With `shutter`, the frame is one whose moving layers blur themselves (each
// drawn several times across its own shutter, inside the one pass): such a
// layer's reach is everywhere it lands across that shutter, not only where it
// is at the frame, or a band would cull a layer whose trail it holds.
static std::shared_ptr<const CompositionReach> planCompositionReach(const Scene &s, double frame,
                                                                    double center, bool shutter = false) {
  auto plan = std::make_shared<CompositionReach>();
  plan->scene = static_cast<const void *>(&s);
  plan->frame = frame;
  plan->center = center;
  plan->bounds.resize(s.layers.size());
  plan->state.assign(s.layers.size(), CompositionReach::Unknown);
  for (size_t index = 0; index < s.layers.size(); ++index) {
    const Layer &l = s.layers[index];
    if (l.type == "group" || l.type == "camera" || l.type == "light" || l.type == "empty" || l.type == "audio" ||
        layerIsStructure(l) || !l.groupId.isEmpty())
      continue;
    // Text is drawn wherever it falls while its characters are animating: they
    // travel out of the box the document reports, and a wiggle has no bound at
    // all. Standing still it fills its own box like anything else, and a page
    // of titles is then culled band by band like anything else.
    if (l.type == "text" && l.characterAnimation.value("enabled").toBool() &&
        !l.hiddenEffects.value("Text-Animation").toBool())
      continue;
    // Particles standing in depth land wherever the camera puts each of them,
    // not on the layer's plane: drawn wherever they fall, like moving letters.
    if (particleVolume(l)) continue;
    QRectF bounds;
    bool reaches = layerCompositionReach(l, s, frame, center, bounds);
    if (shutter && l.motionBlur) {
      // The shutter's two ends, its middle and the quarters between: a trail
      // bends little in half a frame, and the margin takes the rest.
      for (double offset : {-.25, -.125, .125, .25}) {
        QRectF at;
        if (!layerCompositionReach(l, s, frame + offset, center, at)) continue;
        bounds = reaches ? bounds.united(at) : at;
        reaches = true;
      }
      if (reaches) bounds.adjust(-4, -4, 4, 4);
    }
    plan->state[index] = reaches ? CompositionReach::Bounded : CompositionReach::Invisible;
    plan->bounds[index] = bounds;
  }
  return plan;
}
// Holds a plan up while a frame's bands are drawn, and takes it down after.
struct CompositionPlanScope {
  std::shared_ptr<const CompositionReach> previous, mine;
  CompositionPlanScope(const Scene &s, double frame, double center, bool shutter = false) {
    if (!g_reachPlansAllowed) return;
    previous = currentReachPlan();
    mine = planCompositionReach(s, frame, center, shutter);
    installReachPlan(mine);
  }
  ~CompositionPlanScope() {
    if (!mine) return;
    // Put back the plan this one interrupted -- a frame drawn inside another
    // frame, which is how a group is composed -- but only if this plan is
    // still the one standing. Two frames drawn at once on different threads
    // leave whichever finished last in charge, and neither may hand the other
    // a plan describing a scene that is on its way out.
    QMutexLocker lock(&reachPlanGuard());
    if (g_reachPlan == mine) g_reachPlan = previous;
    else if (g_reachPlan == previous) g_reachPlan = nullptr;
  }
};
void paintComposition(QPainter &p, const Scene &s, double frame, double center,
                       QSize outputSize, const QHash<QString, QPicture> *prepared,
                       const LayerEffectHook *effects, const QString &isolate) {
  ExpressionSceneScope expressions(&s);
  LinearLightScope linearLight(s.linearLight);
  // The painter as it was handed over, before this function scales it. A layer
  // flattened on its own has to start from exactly the same place, clip
  // included, or it lands somewhere else in a band.
  const QTransform entryTransform = p.transform();
  // Nothing a shape looked like in another pass can be relied on here. A pass
  // inside this one on the same thread -- a layer drawn alone for its blend,
  // matte, lit face or a sample of its motion blur, or another layer's picture
  // for a texture -- draws this scene or a copy whose shapes differ at most in
  // where they stand and in the numbers shapeGeometry checks, so what this pass
  // found out holds there too, and is kept. Forgotten by each of those as well,
  // every motion-blur sample of the Twitter promo's two turning rings built
  // their outlines again: 21 builds a frame instead of 4, 1.4 ms of the card's
  // frame and 4 to 7 ms of the cores'.
  static thread_local int passDepth = 0;
  if (passDepth == 0) shapeGeometryForget();
  struct InPass { int &depth; explicit InPass(int &d) : depth(d) { ++depth; } ~InPass() { --depth; } } inPass{passDepth};
  const bool animatedScene = (!isolate.isEmpty() && g_animatedSceneHint >= 0)
                                 ? g_animatedSceneHint != 0 : sceneHasKeyedMotion(s);
  struct AnimatedHint {
    int previous; bool set;
    AnimatedHint(bool on, bool active) : previous(g_animatedSceneHint), set(active) { if (set) g_animatedSceneHint = on ? 1 : 0; }
    ~AnimatedHint() { if (set) g_animatedSceneHint = previous; }
  } animatedHint(animatedScene, isolate.isEmpty());
  // Whether any light shows: found out once for the whole frame, and only
  // when a layer that could take light is met while drawing one alone.
  int litState = isolate.isEmpty() ? (sceneHasLights(s, frame) ? 1 : 0) : g_litSceneHint;
  struct LitHint {
    int previous; bool set;
    LitHint(int state, bool active) : previous(g_litSceneHint), set(active) { if (set) g_litSceneHint = state; }
    ~LitHint() { if (set) g_litSceneHint = previous; }
  } litHint(litState, isolate.isEmpty());
  auto lightsShowing = [&] {
    if (litState < 0) litState = sceneHasLights(s, frame) ? 1 : 0;
    return litState == 1;
  };
  // The lights as they stand at a time, worked out once for every layer this
  // pass lights: with shadows that means every caster's silhouette.
  std::vector<std::pair<double, std::unique_ptr<SceneLighting>>> lightings;
  auto lightingAt = [&](double at) -> const SceneLighting & {
    for (const auto &known : lightings)
      if (known.first == at) return *known.second;
    lightings.emplace_back(at, std::make_unique<SceneLighting>(sceneLighting(s, at)));
    return *lightings.back().second;
  };
  const bool entryClipped = p.hasClipping();
  const QRectF entryClip = entryClipped ? p.clipBoundingRect() : QRectF();
  // On a card, whether that clip is one rectangle on the device, asked once
  // for every layer the pass draws (passClipIsDeviceRect).
  const bool onCard = paintsOnCard(p);
  QRect entryDevice;
  const bool entryDeviceRect = onCard && painterClipIsDeviceRect(p, &entryDevice);
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::TextAntialiasing);
  p.scale(double(outputSize.width()) / s.width, double(outputSize.height()) / s.height);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  // A matte layer shapes the one below it and is not drawn on its own, so it is
  // held out of the normal pass. Skipped while isolating, where the caller has
  // asked for exactly one layer and its matte role does not apply.
  QSet<int> consumedAsMatte;
  if (isolate.isEmpty())
    for (int i = 0; i < int(s.layers.size()); ++i) {
      const int src = layerMatteSource(s, i);
      if (src >= 0) consumedAsMatte.insert(src);
    }
  std::vector<int> paintOrder;
  // Per entry of the order: the distances from the camera, [near, far), of the
  // particles a volume layer draws there. Everything else draws whole.
  std::vector<std::pair<double, double>> depthWindows;
  if (isolate.isEmpty()) {
    paintOrder = layerPaintOrder(s, frame);
    sliceParticleVolumes(s, frame, paintOrder, depthWindows);
  }
  else {
    // One layer on its own: no need to order the rest.
    auto paintable = [&](int i) {
      const Layer &candidate = s.layers[size_t(i)];
      return candidate.type != "camera" && candidate.type != "light" && candidate.type != "empty" && candidate.type != "audio" && candidate.groupId.isEmpty();
    };
    if (g_isolateIndexHint >= 0 && g_isolateIndexHint < int(s.layers.size()) &&
        s.layers[size_t(g_isolateIndexHint)].id == isolate) {
      if (paintable(g_isolateIndexHint)) paintOrder.push_back(g_isolateIndexHint);
    } else {
      for (int i = int(s.layers.size()) - 1; i >= 0; --i)
        if (s.layers[size_t(i)].id == isolate && paintable(i)) paintOrder.push_back(i);
    }
  }
  // Where the layers land, if it was worked out for this frame before the
  // bands started. A band can then pass over a layer that does not reach its
  // rows without working out anything about that layer at all, which on a
  // scene of many layers is most of what a band used to do.
  const auto reachPlan = currentReachPlan();
  const bool planApplies = reachPlan && reachPlan->scene == static_cast<const void *>(&s) &&
                           reachPlan->frame == frame && reachPlan->center == center &&
                           reachPlan->state.size() == s.layers.size();
  QRectF planVisible(0, 0, s.width, s.height);
  if (planApplies && p.hasClipping()) planVisible = planVisible.intersected(p.clipBoundingRect());
  for (size_t slot = 0; slot < paintOrder.size(); ++slot) {
    const int index = paintOrder[slot];
    // The slice of depth this entry draws of a volume layer. A pass that draws
    // one layer alone (its blend, its motion blur) keeps the slice it was
    // asked for from the pass around it.
    VolumeWindowScope volumeWindow(isolate.isEmpty() && slot < depthWindows.size() ? depthWindows[slot]
                                                                                   : g_volumeWindow);
    const Layer &original = s.layers[index];
    if (!isolate.isEmpty() && original.id != isolate) continue;
    if (consumedAsMatte.contains(index)) continue;
    if (&s == g_plateScene && isolate.isEmpty() &&
        (index > g_plateBelow || (g_plateSkip && std::binary_search(g_plateSkip->begin(), g_plateSkip->end(), index))))
      continue;
    // Hidden, or not on at this frame: nothing about it needs working out.
    // Checked first, because what follows -- depth of field, the effect
    // settings, the expression scope -- cost every layer of a long scene
    // about ten microseconds a pass whether it showed or not: on a music
    // video of 140 layers, several milliseconds of every frame, on every
    // motion-blur sample, spent on layers that were not there. (The same test
    // as below, which reads the adjusted copy; that copy differs only in its
    // effects, never in its eye or its span.)
    if ((!original.visible && isolate != original.id) || center - original.start < 0 ||
        center - original.start >= original.duration)
      continue;
    bool reachKnown = false;
    if (planApplies) {
      const quint8 state = reachPlan->state[size_t(index)];
      if (state == CompositionReach::Invisible) continue;
      if (state == CompositionReach::Bounded) {
        if (!reachPlan->bounds[size_t(index)].intersects(planVisible)) continue;
        reachKnown = true;
      }
    }
    std::optional<Layer> adjusted;
    if ((original.blur && original.hiddenEffects["Gaussian Blur"].toBool()) ||
        (original.glow && original.hiddenEffects["Glow"].toBool()) ||
        (original.shadow && original.hiddenEffects["Drop Shadow"].toBool())) {
      adjusted.emplace(original);
      if (original.hiddenEffects["Gaussian Blur"].toBool()) adjusted->blur = 0;
      if (original.hiddenEffects["Glow"].toBool()) adjusted->glow = 0;
      if (original.hiddenEffects["Drop Shadow"].toBool()) adjusted->shadow = 0;
    }
    // Depth of Field rides the same blur pass as Gaussian Blur, added on top
    // of whatever the layer already carries rather than replacing it, using
    // the same frame a layer's own transform reads a moment later below.
    const double dof =
        layerDepthOfFieldBlur(original, s, original.motionBlur ? frame : center);
    if (dof > 0) {
      if (!adjusted) adjusted.emplace(original);
      adjusted->blur += dof;
      // The camera's blur, not the layer's own Gaussian Blur effect, so a
      // layer whose own blur is hidden still receives it: the value above
      // already excludes whatever the hidden manual blur would have added.
      adjusted->hiddenEffects["Gaussian Blur"] = false;
    }
    // Most layers need no modification. Borrow their curves/settings rather
    // than copying every property and detaching JSON through mutable reads.
    const Layer &l = adjusted ? *adjusted : original;
    ExpressionLayerScope expressionLayer(&l); // for curves that are not one of the fixed properties
    bool gradient = (l.appliedEffects["Gradient"].toBool() &&
                     !l.hiddenEffects["Gradient"].toBool() && !vectorEffectGradient(l)) ||
                    (l.appliedEffects["Fill"].toBool() &&
                     !l.hiddenEffects["Fill"].toBool() && !vectorEffectFill(l)) ||
                    (l.appliedEffects["Tint"].toBool() && !l.hiddenEffects["Tint"].toBool());
    bool strokeEffect = l.appliedEffects["Stroke"].toBool() && !l.hiddenEffects["Stroke"].toBool() && !vectorEffectStroke(l);
    // Effects the host owns -- plugins in this layer's own stack. A layer that
    // has one needs the raster stage even when nothing built in would have
    // asked for it, and none of the shortcuts below may skip past it.
    const auto *host = hostLayerEffects();
    const bool hostEffect = host && host->active && host->active(l);
    double effectPad = ((strokeEffect || vectorEffectStroke(l)) ? strokeOutwardReach(l) : 0) + rgbSplitReach(l);
    double t = (l.motionBlur ? frame : center) - l.start;
    // The shutter is centred on the frame, so on a layer's first and last
    // frame half of its samples land outside the layer's own span. Dropping
    // those samples left the layer half transparent for exactly one frame.
    // Whether a layer appears is decided by the frame being rendered; a
    // sample that reaches past the in- or out-point is held at the boundary.
    const double visibleAt = center - l.start;
    // Setting a track matte switches the matte layer's eye off by convention,
    // yet it must still be rendered to shape the layer below. When this pass is
    // isolating one layer -- which is how a matte source is drawn -- honour the
    // time span but not the eye.
    const bool ignoreEye = isolate == original.id;
    if ((!l.visible && !ignoreEye) || visibleAt < 0 || visibleAt >= l.duration)
      continue;
    if (l.type == "audio") continue; // sound only: nothing to draw
    t = std::clamp(t, 0., std::max(0., l.duration - 1e-6));
    // Posterize Time: the layer's clock held in steps. Everything below reads t.
    t = posterizedTime(l, s, t);
    // A 3D repeater is a set of independently projected planes. The source
    // copy may already be behind the camera while copies shifted by Position Z
    // are still visible. Culling the source here used to discard the complete
    // repeater before its per-copy checks below ever ran.
    // Particles in depth are each in front of the camera or not on their own.
    if (!particleVolume(l) && !layerOrRepeaterInFrontOfCamera(l, s, t + l.start)) continue;
    if (l.p[5].at(t) <= 0) continue;
    // An Effect Layer draws nothing of its own: it works on the picture already
    // made beneath it. Drawn on its own -- the passes that isolate one layer --
    // it has nothing beneath it to work on. Every effect on it at rest (a beat
    // effect between its hits) and it changes nothing: it used to be worked
    // anyway, the picture beneath copied out and put back as it was -- on the
    // card read back, worked on the processor and sent up again, for every
    // motion-blur sample: 13 ms of each frame of a chorus doing nothing.
    if (l.type == "effect") {
      if (isolate.isEmpty() && effectLayerActive(l, t + l.start)) applyEffectLayer(p, l, s, t);
      continue;
    }
    // Shaped by the layer above before anything else touches it, so its blend
    // mode combines the cut-out result with what lies beneath, not the whole
    // rectangle. Only at the top level; the flatten passes below draw plainly.
    const int matteSource = isolate.isEmpty() ? layerMatteSource(s, index) : -1;
    if (matteSource >= 0) {
      const QPaintDevice *device = p.device();
      if (!device || device->width() <= 0 || device->height() <= 0) continue;
      const QSize size(device->width(), device->height());
      // A small owner is flattened and matted in buffers of its own size,
      // shifted by whole pixels so nothing is clipped and every pixel is drawn
      // exactly as it would be in the frame; a large one keeps the frame-sized
      // buffers as before.
      QRect dirty(QPoint(), size);
      const bool bounded = layerDeviceRect(original, s, t, p.worldTransform(), size, dirty);
      if (bounded && dirty.isEmpty()) continue;
      // On a card both are flattened and combined there, as a blended layer is.
      if (s.processingDepth <= 8 && g_matteCard && *g_matteCard && device->devType() != QInternal::Image &&
          device->devType() != QInternal::Picture) {
        auto alone = [&](const QString &id, int layerIndex) {
          return [&, id, layerIndex](QPainter &q, const LayerEffectHook *onCard) {
            q.setTransform(entryTransform);
            clipAlonePass(q, entryClipped, entryClip);
            IsolateHint hint(layerIndex);
            paintComposition(q, s, frame, center, outputSize, nullptr, onCard, id);
          };
        };
        if ((*g_matteCard)(p, bounded ? dirty : QRect(QPoint(), size), layerCompositionMode(original),
                           original.trackMatteInverted, alone(original.id, index),
                           alone(s.layers[size_t(matteSource)].id, matteSource)))
          continue;
      }
      const bool small = bounded && qint64(dirty.width()) * dirty.height() * 4 <
                                        qint64(size.width()) * size.height();
      if (!small) dirty = QRect(QPoint(), size);
      const QSize bufferSize = small ? dirty.size() : size;
      const QTransform shift = small ? QTransform::fromTranslate(-dirty.x(), -dirty.y()) : QTransform();
      static thread_local QImage ownerBuf, matteBuf;
      QImage ownerSmall, matteSmall;
      QImage &owner = small ? ownerSmall : ownerBuf;
      QImage &matte = small ? matteSmall : matteBuf;
      auto flattenInto = [&](QImage &buffer, const QString &id, int layerIndex) {
        if (buffer.size() != bufferSize || buffer.format() != QImage::Format_ARGB32_Premultiplied)
          buffer = QImage(bufferSize, QImage::Format_ARGB32_Premultiplied);
        if (buffer.isNull()) return false;
        buffer.fill(Qt::transparent);
        QPainter q(&buffer);
        q.setTransform(entryTransform * shift);
        if (entryClipped) q.setClipRect(entryClip);
        IsolateHint hint(layerIndex);
        paintComposition(q, s, frame, center, outputSize, nullptr, nullptr, id);
        return true;
      };
      if (!flattenInto(owner, original.id, index) ||
          !flattenInto(matte, s.layers[matteSource].id, matteSource))
        continue;
      const bool useLuma = false;
      const bool invert = original.trackMatteInverted;
      const int rows = owner.height(), cols = owner.width();
      for (int y = 0; y < rows; ++y) {
        auto *o = reinterpret_cast<QRgb *>(owner.scanLine(y));
        const auto *m = reinterpret_cast<const QRgb *>(matte.constScanLine(y));
        for (int x = 0; x < cols; ++x) {
          // matteBuf is premultiplied: a transparent matte pixel is 0,0,0,0, so
          // its luma is already 0 where it is see-through, no separate alpha
          // multiply needed.
          int weight = useLuma ? (qRed(m[x]) * 299 + qGreen(m[x]) * 587 +
                                  qBlue(m[x]) * 114) / 1000
                               : qAlpha(m[x]);
          if (invert) weight = 255 - weight;
          if (weight == 255) continue;
          if (weight == 0) { o[x] = 0; continue; }
          o[x] = qRgba((qRed(o[x]) * weight) / 255, (qGreen(o[x]) * weight) / 255,
                       (qBlue(o[x]) * weight) / 255, (qAlpha(o[x]) * weight) / 255);
        }
      }
      p.save();
      p.resetTransform();
      p.setOpacity(1);
      p.setCompositionMode(layerCompositionMode(original));
      p.drawImage(dirty.topLeft(), owner);
      p.restore();
      continue;
    }
    if (g_layerMotionBlur && isolate.isEmpty() && s.motionBlur && l.motionBlur && l.type != "group") {
      const int samples = layerBlurSampleCount(l, s, t, center, outputSize);
      if (samples > 0) {
        const QPaintDevice *device = p.device();
        if (!device || device->width() <= 0 || device->height() <= 0) continue;
        const QSize size(device->width(), device->height());
        std::vector<double> times(size_t(std::max(samples, 1)));
        for (int k = 0; k < samples; ++k) times[size_t(k)] = center + ((k + .5) / samples - .5) * .5;
        // One box that holds the layer at every sample.
        QRect dirty(QPoint(), size);
        bool bounded = true;
        QRect all;
        for (int k = 0; k < samples && bounded; ++k) {
          const double local = std::clamp(times[size_t(k)] - l.start, 0., std::max(0., l.duration - 1e-6));
          QRect box;
          if (!layerDeviceRect(l, s, local, p.worldTransform(), size, box, true)) bounded = false;
          else all = k ? all.united(box) : box;
        }
        if (bounded) dirty = all.intersected(QRect(QPoint(), size));
        if (bounded && dirty.isEmpty()) continue;
        const bool small = bounded && qint64(dirty.width()) * dirty.height() * 4 <
                                          qint64(size.width()) * size.height();
        if (!small) dirty = QRect(QPoint(), size);
        const QSize bufferSize = small ? dirty.size() : size;
        const QTransform shift = small ? QTransform::fromTranslate(-dirty.x(), -dirty.y()) : QTransform();
        KeyMemoScope keyMemoScope(s, index);
        // On a card the samples are drawn, laid over what lies beneath and
        // averaged there, never read back. At 16 bpc as at 8: the card draws a
        // frame of either depth at eight bits a channel and sums samples in
        // half floats, whole frames or a layer at a time.
        if (g_blurCard && *g_blurCard && device->devType() != QInternal::Image &&
            device->devType() != QInternal::Picture) {
          auto paintSample = [&](QPainter &q, int k, const LayerEffectHook *onCard) {
            q.setTransform(entryTransform);
            clipAlonePass(q, entryClipped, entryClip);
            IsolateHint hint(index);
            paintComposition(q, s, times[size_t(std::clamp(k, 0, samples - 1))], center, outputSize, nullptr, onCard, l.id);
          };
          if ((*g_blurCard)(p, bounded ? all.intersected(QRect(QPoint(), size)) : QRect(QPoint(), size),
                            layerCompositionMode(l), samples, paintSample))
            continue;
        }
        // A blended layer cannot be averaged first and blended once: the blend
        // is not linear. Each sample is blended into a copy of what lies beneath
        // -- which does not change from sample to sample -- and those are
        // averaged. Needs to read the frame being drawn, which the CPU
        // composition always is: an image.
        const QPainter::CompositionMode blendMode = layerCompositionMode(l);
        const QImage *beneath = (blendMode != QPainter::CompositionMode_SourceOver &&
                                 device->devType() == QInternal::Image)
                                    ? static_cast<const QImage *>(device) : nullptr;
        if (blendMode != QPainter::CompositionMode_SourceOver && !beneath) continue;
        // A deep frame the same way, in pictures of its own depth: each sample
        // drawn in the frame's own format, its values added into 32-bit sums at
        // 16 bits a channel and into float ones at 32, and the mean laid on as
        // below. Averaged from whole frames instead, as deep frames used to be,
        // every sample drew every layer and every Effect Layer again, and the
        // Effect Layer kept the frame out of bands besides: at 960x540 the
        // Twitter promo took 72 ms a frame at 16 bpc and 96 at 32, against 30
        // and 41 this way and 25 at 8; a Kotonoha chorus 166 and 194, against
        // 29 and 34 (28 at 8).
        if (s.processingDepth > 8) {
          const QImage::Format format = processingFormat(s.processingDepth, true);
          const bool floats = s.processingDepth > 16;
          const int width = bufferSize.width(), height = bufferSize.height();
          const qsizetype rowValues = qsizetype(width) * 4;
          const qsizetype rowBytes = rowValues * qsizetype(floats ? sizeof(float) : sizeof(quint16));
          const size_t values = size_t(rowValues) * size_t(height);
          BlurScratch &scratch = g_blurScratch;
          if (scratch.sample.size() < size_t(rowBytes) * size_t(height)) scratch.sample.resize(size_t(rowBytes) * size_t(height));
          if (floats && scratch.floatSums.size() < values) scratch.floatSums.resize(values);
          if (!floats && scratch.wordSums.size() < values) scratch.wordSums.resize(values);
          QImage sample(scratch.sample.data(), width, height, rowBytes, format);
          // What lies beneath a blended layer, copied out once: nothing is
          // drawn on the frame while its samples are.
          QImage under, blended;
          if (beneath) {
            under = beneath->copy(dirty).convertToFormat(format);
            blended = QImage(width, height, format);
            if (under.isNull() || blended.isNull()) continue;
          }
          for (int k = 0; k < samples; ++k) {
            sample.fill(Qt::transparent);
            {
              QPainter q(&sample);
              q.setTransform(entryTransform * shift);
              if (entryClipped) q.setClipRect(entryClip);
              IsolateHint hint(index);
              paintComposition(q, s, times[size_t(k)], center, outputSize, nullptr, nullptr, l.id);
            }
            if (beneath) {
              for (int y = 0; y < height; ++y) std::memcpy(blended.scanLine(y), under.constScanLine(y), size_t(rowBytes));
              QPainter over(&blended);
              over.setCompositionMode(blendMode);
              over.drawImage(QPoint(), sample);
            }
            const QImage &added = beneath ? blended : sample;
            for (int y = 0; y < height; ++y) {
              const uchar *row = added.constScanLine(y);
              if (floats)
                addFloats(scratch.floatSums.data() + size_t(y) * size_t(rowValues), reinterpret_cast<const float *>(row),
                          rowValues, k == 0);
              else
                addWords(scratch.wordSums.data() + size_t(y) * size_t(rowValues), reinterpret_cast<const quint16 *>(row),
                         rowValues, k == 0);
            }
          }
          // The mean: at 32 bits the sums divided where they lie, at 16 written
          // over the last sample.
          QImage mean;
          if (floats) {
            averageFloats(scratch.floatSums.data(), qsizetype(values), float(samples));
            mean = QImage(reinterpret_cast<uchar *>(scratch.floatSums.data()), width, height, rowBytes, format);
          } else {
            averageWords(reinterpret_cast<quint16 *>(scratch.sample.data()), scratch.wordSums.data(), qsizetype(values),
                         unsigned(samples));
            mean = QImage(scratch.sample.data(), width, height, rowBytes, format);
          }
          p.save();
          p.resetTransform();
          p.setOpacity(1);
          if (beneath) p.setCompositionMode(QPainter::CompositionMode_Source);
          p.drawImage(dirty.topLeft(), mean);
          p.restore();
          // Kept for the next layer, as the 8-bit buffers are, up to about a
          // 1440p frame's worth at 32 bpc: a film written at 4K, a frame whole on
          // each of several workers, would otherwise hold a fifth of a gigabyte
          // on every one of them for good.
          if (scratch.sample.size() + (scratch.wordSums.size() + scratch.floatSums.size()) * 4 > (size_t(128) << 20)) {
            std::vector<uchar>().swap(scratch.sample);
            std::vector<quint32>().swap(scratch.wordSums);
            std::vector<float>().swap(scratch.floatSums);
          }
          continue;
        }
        const qsizetype bytesPerRow = qsizetype(bufferSize.width()) * 4;
        BlurScratch &scratch = g_blurScratch;
        const size_t cells = size_t(bytesPerRow) * size_t(bufferSize.height());
        if (scratch.sample.size() < cells) scratch.sample.resize(cells);
        if (scratch.mean.size() < cells) scratch.mean.resize(cells);
        scratch.sums.assign(cells, 0);
        QImage sample(scratch.sample.data(), bufferSize.width(), bufferSize.height(), bytesPerRow,
                      QImage::Format_ARGB32_Premultiplied);
        std::vector<quint16> &sum = scratch.sums;
        for (int k = 0; k < samples; ++k) {
          sample.fill(Qt::transparent);
          {
            QPainter q(&sample);
            q.setTransform(entryTransform * shift);
            if (entryClipped) q.setClipRect(entryClip);
            IsolateHint hint(index);
            paintComposition(q, s, times[size_t(k)], center, outputSize, nullptr, nullptr, l.id);
          }
          QImage blended;
          if (beneath) {
            // Whatever byte order the frame is in, the sum is kept in ARGB32.
            blended = beneath->copy(dirty).convertToFormat(QImage::Format_ARGB32_Premultiplied);
            QPainter over(&blended);
            over.setCompositionMode(blendMode);
            over.drawImage(QPoint(), sample);
          }
          const QImage &added = beneath ? blended : sample;
          for (int y = 0; y < bufferSize.height(); ++y)
            addBytes(sum.data() + size_t(y) * size_t(bytesPerRow), added.constScanLine(y), bytesPerRow);
        }
        QImage mean(scratch.mean.data(), bufferSize.width(), bufferSize.height(), bytesPerRow,
                    QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < bufferSize.height(); ++y)
          averageBytes(mean.scanLine(y), sum.data() + size_t(y) * size_t(bytesPerRow), bytesPerRow, unsigned(samples));
        p.save();
        p.resetTransform();
        p.setOpacity(1);
        if (beneath) p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(dirty.topLeft(), mean);
        p.restore();
        continue;
      }
    }
    const QPainter::CompositionMode blend = layerCompositionMode(l);
    if (blend != QPainter::CompositionMode_SourceOver && l.type != "group" && isolate != l.id) {
      // Flattened on its own first, then combined with what lies beneath as one
      // picture. Blending every stroke, fill and glyph of it separately would
      // let the layer blend with itself: a stroke multiplied again by the fill
      // laid over it, overlapping letters screened brighter where they cross.
      const QPaintDevice *device = p.device();
      if (!device || device->width() <= 0 || device->height() <= 0) continue;
      const QSize size(device->width(), device->height());
      const auto painting = processingFormat(s.processingDepth, true);
      // Kept between frames for the reason sampleRegion keeps its buffer: first
      // touching a frame's worth of fresh memory costs page faults every frame.
      // One per nesting depth, because a blended layer inside a blended group
      // needs its own while the outer one is still being drawn; a deque, so
      // growing it never moves the buffer an outer level is holding.
      static thread_local std::deque<QImage> pool;
      static thread_local int depth = 0;
      // A layer that covers a corner of the frame is flattened in a buffer of
      // its own size, shifted by whole pixels so nothing is clipped and every
      // pixel comes out as it would in the frame. Only a large one keeps the
      // frame-sized buffer above.
      QRect dirty(QPoint(), size);
      const bool bounded = layerDeviceRect(l, s, t, p.worldTransform(), size, dirty, true);
      if (bounded && dirty.isEmpty()) continue;
      // On a card it is flattened there, its effects too, and blended with what
      // lies beneath by the same arithmetic: flattened on the CPU, a blended
      // title with a moving effect cost more than the rest of the frame.
      if (s.processingDepth <= 8 && g_blendCard && *g_blendCard && device->devType() != QInternal::Image &&
          device->devType() != QInternal::Picture) {
        const QRect area = bounded ? dirty : QRect(QPoint(), size);
        auto paintAlone = [&](QPainter &isolated, const LayerEffectHook *onCard) {
          isolated.setTransform(entryTransform);
          clipAlonePass(isolated, entryClipped, entryClip);
          IsolateHint hint(index);
          paintComposition(isolated, s, frame, center, outputSize, nullptr, onCard, l.id);
        };
        if ((*g_blendCard)(p, area, blend, paintAlone)) continue;
      }
      const bool small = bounded && qint64(dirty.width()) * dirty.height() * 4 <
                                        qint64(size.width()) * size.height();
      if (!small) dirty = QRect(QPoint(), size);
      QImage smallBuffer;
      if (int(pool.size()) <= depth) pool.resize(size_t(depth) + 1);
      QImage &flattened = small ? smallBuffer : pool[size_t(depth)];
      const QSize bufferSize = small ? dirty.size() : size;
      if (flattened.size() != bufferSize || flattened.format() != painting)
        flattened = QImage(bufferSize, painting);
      if (flattened.isNull()) continue;
      flattened.fill(Qt::transparent);
      {
        struct Deeper { int &d; Deeper(int &v) : d(v) { ++d; } ~Deeper() { --d; } } deeper{depth};
        QPainter isolated(&flattened);
        isolated.setTransform(small ? entryTransform * QTransform::fromTranslate(-dirty.x(), -dirty.y())
                                    : entryTransform);
        if (entryClipped) isolated.setClipRect(entryClip);
        IsolateHint hint(index);
        paintComposition(isolated, s, frame, center, outputSize, nullptr, nullptr, l.id);
      }
      p.save();
      p.resetTransform();
      p.setOpacity(1);
      p.setCompositionMode(blend);
      p.drawImage(dirty.topLeft(), flattened);
      p.restore();
      // A frame far larger than a frame is not worth holding on to.
      if (!small && qint64(size.width()) * size.height() > 64ll * 1024 * 1024) flattened = QImage();
      continue;
    }
    // Particles in depth: each seen through the camera where it stands, the
    // ones in this entry's slice of depth, far to near. Not lit, not culled by
    // the layer's plane, and without pixel effects: they are points, not a
    // picture on a plane.
    if (particleVolume(l)) {
      p.save();
      p.setOpacity(p.opacity() * std::clamp(l.p[5].at(t), 0., 1.));
      drawParticleVolume(p, l, s, t, g_volumeWindow.first, g_volumeWindow.second);
      p.restore();
      continue;
    }
    // Lit by the scene's lights (lights.h). Each copy's face is drawn alone,
    // effects, masks and all, and every pixel of it is lit at the point of
    // its plane it shows; the extruded walls are lit face by face as they are
    // drawn. A layer that blends, is matted or blurs with motion comes here
    // from the pass that draws it alone for that.
    if (g_litLayer != &original && layerCanTakeLight(l) && lightsShowing()) {
      const QPaintDevice *device = p.device();
      if (!device || device->width() <= 0 || device->height() <= 0) continue;
      const QRect whole(0, 0, device->width(), device->height());
      const double at = t + l.start;
      const SceneLighting &lighting = lightingAt(at);
      const float shine = float(std::clamp(l.p[SurfaceShine].at(t), 0., 100.) / 100.);
      const float gloss = float(std::clamp(l.p[SurfaceGloss].at(t), 0., 100.) / 100.);
      const auto painting = processingFormat(s.processingDepth, true);
      const int pixelBytes = s.processingDepth == 32 ? 16 : s.processingDepth == 16 ? 8 : 4;
      // One store per nesting depth, kept between frames: fresh memory the size
      // of a layer costs page faults every frame.
      static thread_local std::deque<std::vector<uchar>> stores;
      static thread_local int depth = 0;
      if (int(stores.size()) <= depth) stores.resize(size_t(depth) + 1);
      struct Deeper { int &d; Deeper(int &v) : d(v) { ++d; } ~Deeper() { --d; } } deeper{depth};
      std::vector<uchar> &store = stores[size_t(depth - 1)];
      for (int copy = 0; copy < repeaterCopyCount(l); ++copy) {
        if (!repeaterCopyInFrontOfCamera(l, s, at, copy)) continue;
        // Seen from behind: the face first, then the solid over it, and on its
        // back the same picture, lit where it faces (as below).
        const bool extruded = layerExtrudeDepth(l, t) > 0;
        const bool fromBehind = extruded && !extrudeFrontFacesCamera(l, s, at, copy);
        if (extruded && !fromBehind) drawExtrudeWalls(p, l, s, at, copy);
        auto backFace = [&, index, depth = layerExtrudeDepth(l, t)] {
          if (repeaterCopyCount(l) != 1) return;
          Scene back = s;
          Layer &behind = back.layers[size_t(index)];
          Property &anchor = behind.p[AnchorZ];
          anchor.base -= depth;
          for (Key &key : anchor.keys) key.value -= depth;
          behind.p[ExtrudeDepth] = Property{};
          p.save();
          p.setTransform(entryTransform);
          IsolateHint hint(index);
          paintComposition(p, back, frame, center, outputSize, prepared, effects, behind.id);
          p.restore();
        };
        struct SolidAfterFace {
          QPainter &p; const Layer &l; const Scene &s; double frame; int copy; bool on; std::function<void()> back;
          ~SolidAfterFace() {
            if (!on) return;
            drawExtrudeWalls(p, l, s, frame, copy, true);
            back();
          }
        } solidAfterFace{p, l, s, at, copy, fromBehind, backFace};
        const QTransform localToDevice = projectRepeaterCopy(l, s, at, copy) * p.worldTransform();
        const QRect dirty = litCopyDeviceRect(l, s, t, localToDevice, whole);
        if (dirty.isEmpty()) continue;
        // The face alone, effects, masks and all, its device's top left at the
        // top left of `dirty`.
        auto paintFace = [&](QPainter &q) {
          q.setTransform(entryTransform * QTransform::fromTranslate(-dirty.x(), -dirty.y()));
          if (entryClipped) q.setClipRect(entryClip);
          struct LitPass {
            const Layer *layer; int copy;
            LitPass(const Layer *l, int c) : layer(g_litLayer), copy(g_litCopy) { g_litLayer = l; g_litCopy = c; }
            ~LitPass() { g_litLayer = layer; g_litCopy = copy; }
          } pass(&original, copy);
          IsolateHint hint(index);
          paintComposition(q, s, frame, center, outputSize, nullptr, nullptr, l.id);
        };
        // On a card the face is painted, lit and laid in there: a full-frame
        // wall painted and lit on the CPU and sent up again was most of a lit
        // scene's frame (11.6 ms of the lights demo's 20 at 1080p).
        if (s.processingDepth <= 8 && g_litFaceCard && *g_litFaceCard && p.device()->devType() != QInternal::Image &&
            p.device()->devType() != QInternal::Picture &&
            (*g_litFaceCard)(p, dirty, paintFace, localToDevice, repeaterCopyWorldMatrix(l, s, at, copy), lighting,
                             shine, gloss, index))
          continue;
        const qsizetype bytesPerLine = qsizetype(dirty.width()) * pixelBytes;
        const size_t needed = size_t(bytesPerLine) * size_t(dirty.height());
        if (store.size() < needed) store.resize(needed);
        QImage face(store.data(), dirty.width(), dirty.height(), bytesPerLine, painting);
        face.fill(Qt::transparent);
        {
          QPainter q(&face);
          paintFace(q);
        }
        // The device is a band's rows when the frame is drawn in bands: the
        // painter came in shifted by where the band starts.
        const QPoint deviceToFrame = entryTransform.type() <= QTransform::TxTranslate
                                         ? QPoint(qRound(-entryTransform.dx()), qRound(-entryTransform.dy()))
                                         : QPoint();
        lightLayerImage(face, dirty.topLeft(), localToDevice, repeaterCopyWorldMatrix(l, s, at, copy), lighting, shine,
                        gloss, index, deviceToFrame);
        p.save();
        p.resetTransform();
        p.setOpacity(1);
        p.drawImage(dirty.topLeft(), face);
        p.restore();
      }
      if (store.size() > size_t(64) * 1024 * 1024) std::vector<uchar>().swap(store);
      continue;
    }
    if (l.type == "group") {
      Scene sub;
      sub.width = s.width; sub.height = s.height; sub.fps = s.fps;
      sub.frames = s.frames; sub.processingDepth = s.processingDepth; sub.linearLight = s.linearLight;
      sub.frame = s.frame; sub.motionBlur = s.motionBlur;
      sub.previewDraft = s.previewDraft; sub.finalRender = s.finalRender;
      sub.background = QColor(Qt::transparent);
      sub.pluginSettings = s.pluginSettings;
      // A precomposition may itself contain another precomposition. Discover
      // the complete subtree first, then promote only this group's direct
      // children to the sub-scene root; nested ownership remains intact.
      QSet<QString> containedGroups{l.id};
      bool foundAnother = true;
      while (foundAnother) {
        foundAnother = false;
        for (const auto &child : s.layers)
          if (child.type == "group" && containedGroups.contains(child.groupId) &&
              !containedGroups.contains(child.id)) {
            containedGroups.insert(child.id);
            foundAnother = true;
          }
      }
      for (const auto &child : s.layers)
        if (containedGroups.contains(child.groupId)) {
          sub.layers.push_back(child);
          if (sub.layers.back().groupId == l.id)
            sub.layers.back().groupId.clear();
        }
      if (l.sharesMainView && !sub.layers.empty()) addSharedView(sub, s, l.groupId);
      if (!sub.layers.empty()) {
        const QPaintDevice *device = p.device();
        if (!device || device->width() <= 0 || device->height() <= 0) continue;
        auto painting = processingFormat(s.processingDepth, true);
        // The destination may be one horizontal worker band or an export ROI.
        // Render the group in that same device-sized coordinate space. A full
        // output-sized image here made every worker render the entire group,
        // then draw its top rows into every band; lower bands therefore showed
        // the wrong part of a grouped layer and CPU work multiplied by the
        // number of cores.
        QImage groupImage(QSize(device->width(), device->height()), painting);
        if (groupImage.isNull()) continue;
        groupImage.fill(Qt::transparent);
        QPainter gp(&groupImage);
        gp.setTransform(entryTransform);
        if (entryClipped) gp.setClipRect(entryClip);
        paintComposition(gp, sub, frame, center, outputSize, prepared, effects);
        gp.end();
        bool hasEffects = (l.blur > 0 && !l.hiddenEffects["Gaussian Blur"].toBool()) ||
                          (l.glow > 0 && !l.hiddenEffects["Glow"].toBool()) ||
                          (l.shadow > 0 && !l.hiddenEffects["Drop Shadow"].toBool());
        if (!hasEffects) {
          for (auto it = l.appliedEffects.constBegin(); it != l.appliedEffects.constEnd(); ++it) {
            if (it.value().toBool() && !l.hiddenEffects.value(it.key()).toBool()) {
              hasEffects = true;
              break;
            }
          }
        }
        if (hasEffects) {
          const QRectF box(0, 0, groupImage.width(), groupImage.height());
          EffectTimeScope effectTime(t / std::max(1, s.fps), t);
          groupImage = applyLayerEffects(std::move(groupImage), l, box, 1.0, t);
        }
        const QTransform compositionToDevice = p.worldTransform();
        bool invertible = false;
        const QTransform deviceToComposition = compositionToDevice.inverted(&invertible);
        if (!invertible) continue;
        bool base3DInvertible = false;
        const QTransform base3D = l.threeD ? projectLayer(l, s, frame) : QTransform();
        const QTransform compositionToGroupLocal =
            l.threeD ? base3D.inverted(&base3DInvertible) : QTransform();
        if (l.threeD && !base3DInvertible) continue;
        const QPolygonF sourceQuad{{0, 0}, {double(groupImage.width()), 0},
                                   {double(groupImage.width()), double(groupImage.height())},
                                   {0, double(groupImage.height())}};
        for (int copy = 0; copy < repeaterCopyCount(l); ++copy) {
          if (l.threeD && !repeaterCopyInFrontOfCamera(l, s, frame, copy))
            continue;
          QPolygonF destinationQuad;
          const QTransform repeat = repeaterTransform(l, copy);
          const QTransform repeat3D =
              l.threeD ? projectRepeaterCopy(l, s, frame, copy) : QTransform();
          for (const QPointF &point : sourceQuad) {
            const QPointF compositionPoint = deviceToComposition.map(point);
            const QPointF repeatedPoint = l.threeD
                ? repeat3D.map(compositionToGroupLocal.map(compositionPoint))
                : repeat.map(compositionPoint);
            destinationQuad << compositionToDevice.map(repeatedPoint);
          }
          QTransform deviceRepeat;
          if (!QTransform::quadToQuad(sourceQuad, destinationQuad, deviceRepeat)) continue;
          p.save();
          p.resetTransform();
          p.setWorldTransform(deviceRepeat);
          p.setOpacity(std::clamp(l.p[5].at(t), 0., 1.));
          // Already one flattened picture, so it blends as a whole without the
          // isolation a single layer needs.
          p.setCompositionMode(blend);
          p.drawImage(0, 0, groupImage);
          p.restore();
        }
      }
      continue;
    }
    const auto transform = layerWorldTransform(l,s,t+l.start);
    // Animated characters can extend beyond their document bounds. Other
    // layers can be rejected before texture upload or expensive CPU effects.
    if (!reachKnown && (l.type != "text" || (!l.characterAnimation.value("enabled").toBool() || l.hiddenEffects.value("Text-Animation").toBool()))) {
      auto bounds = layerBounds(l, s);
      // Where the layer ends up, not where it started: the composition is
      // rendered in horizontal bands, so a cull that ignored the main
      // transform dropped the layer out of the bands it had moved into.
      if (layerHasMainTransform(l)) bounds = layerMainTransform(l).mapRect(bounds);
      const double pad = (l.blur+l.glow+l.shadow)*3 + l.strokeWidth + effectPad + 20 +
                         hostEffectReach(l, s, t, bounds.size());
      bounds.adjust(-pad,-pad,pad,pad);
      if (l.threeD && l.repeaterEnabled) {
        QRectF projected;
        bool hasProjectedCopy = false;
        for (int copy = 0; copy < repeaterCopyCount(l); ++copy) {
          if (!repeaterCopyInFrontOfCamera(l, s, t + l.start, copy)) continue;
          const QRectF copyBounds = withExtrudedWalls(l, s, t + l.start, copy, bounds,
                                                      projectRepeaterCopy(l, s, t + l.start, copy).mapRect(bounds));
          projected = hasProjectedCopy ? projected.united(copyBounds) : copyBounds;
          hasProjectedCopy = true;
        }
        if (!hasProjectedCopy) continue;
        bounds = projected;
      } else {
        bounds = withExtrudedWalls(l, s, t + l.start, 0, bounds, transform.mapRect(repeatedLayerBounds(l, bounds)));
      }
      QRectF visible(0,0,s.width,s.height);
      if (p.hasClipping()) visible = visible.intersected(p.clipBoundingRect());
      if (!bounds.intersects(visible)) continue;
    }
    // Past the end of its footage a video layer shows nothing -- unless its
    // time is remapped, which holds the last picture instead.
    if (l.type == "video" && !l.timeRemap && l.sourceDurationMs > 0 &&
        l.sourceOffsetMs + t * 1000. / s.fps >= l.sourceDurationMs)
      continue;
    // Animated letters too: each is painted whole and its strokes built here
    // with the rest, at its own opacity and size. Stroked with a pen per letter
    // instead, a wide stack turned a "W" into a pentagon, ran past the box the
    // picture was cut to, and faded each band over the last, so a letter
    // fading out kept its strokes. Letters with backgrounds too: those are
    // painted apart meanwhile and laid under the strokes after.
    const bool rasterCharacterStrokes=l.type=="text" && !textStrokeStack(l).empty();
    const bool lettersAnimated=l.characterAnimation.value("enabled").toBool() &&
                               !l.hiddenEffects.value("Text-Animation").toBool();
    auto draw = [&](QPainter &painter) {
      if (prepared && prepared->contains(l.id)) {
        // On a card, the fills it made ready beside the picture are covered
        // there (ShapeCardHook); the picture is played where it cannot.
        const bool coveredOnCard =
            g_shapeCard && *g_shapeCard && paintsOnCard(painter) && (*g_shapeCard)(painter, l, nullptr);
        if (!coveredOnCard) {
          // A QPicture plays back through a buffer with a read position of its
          // own, and every copy of the picture shares that buffer. Source strips
          // replay the same layer on several threads at once, so each plays a
          // private copy of the bytes. Playing the shared one raced on that
          // position: intermittently a crash reading address 0x20 inside
          // QPicture::exec, and otherwise a comparison drawn wrong.
          const QPicture shared = prepared->value(l.id);
          QPicture local;
          local.setData(shared.data(), shared.size());
          painter.drawPicture(QPointF(), local);
        }
      } else drawLayer(painter, l, s, t);
      // Particles a layer gives off, over what it draws.
      if (l.particlesOn && l.type != "particles") drawParticles(painter, l, s, t);
    };
    for (int repeaterCopy = 0; repeaterCopy < repeaterCopyCount(l); ++repeaterCopy) {
    // Drawn for its lights: one copy's face alone, the walls lit apart.
    const bool litFace = g_litLayer == &original;
    if (litFace && repeaterCopy != g_litCopy) continue;
    if (l.threeD &&
        !repeaterCopyInFrontOfCamera(l, s, t + l.start, repeaterCopy))
      continue;
    // Seen from behind, the face is the far side of the solid: it goes down
    // first, and the walls and the back over it once it is drawn. Drawn over
    // its walls whatever the angle, a title turned past ninety degrees showed
    // its face through the solid, mirrored, with no back to it.
    const bool extruded = l.threeD && !litFace && layerExtrudeDepth(l, t) > 0;
    const bool fromBehind = extruded && !extrudeFrontFacesCamera(l, s, t + l.start, repeaterCopy);
    if (extruded && !fromBehind) drawExtrudeWalls(p, l, s, t + l.start, repeaterCopy);
    // The back of the solid carries the same picture as its face -- fill,
    // stroke, effects -- the way a title cut from a block shows its letters
    // from both sides. Drawn as the same layer set back by the depth, alone.
    // (One copy of a repeater cannot be drawn alone, so a repeater's back
    // stays the plain solid colour.)
    auto backFace = [&, index, depth = layerExtrudeDepth(l, t)] {
      if (repeaterCopyCount(l) != 1) return;
      Scene back = s;
      Layer &behind = back.layers[size_t(index)];
      Property &anchor = behind.p[AnchorZ];
      anchor.base -= depth;
      for (Key &key : anchor.keys) key.value -= depth;
      behind.p[ExtrudeDepth] = Property{};
      p.save();
      p.setTransform(entryTransform);
      IsolateHint hint(index);
      paintComposition(p, back, frame, center, outputSize, prepared, effects, behind.id);
      p.restore();
    };
    struct SolidAfterFace {
      QPainter &p; const Layer &l; const Scene &s; double frame; int copy; bool on; std::function<void()> back;
      ~SolidAfterFace() {
        if (!on) return;
        drawExtrudeWalls(p, l, s, frame, copy, true);
        back();
      }
    } solidAfterFace{p, l, s, t + l.start, repeaterCopy, fromBehind, backFace};
    p.save();
    if (l.threeD) {
      p.setWorldTransform(
          projectRepeaterCopy(l, s, t + l.start, repeaterCopy), true);
    } else {
      p.setWorldTransform(transform, true);
      // A layer with no repeater has nothing to offset, and multiplying the
      // painter's transform by an identity is work every layer used to do.
      if (l.repeaterEnabled) p.setWorldTransform(repeaterTransform(l, repeaterCopy), true);
    }
    bool masked = false;
    // A hard expanded mask is still a vector clip. Only a genuinely soft
    // feather needs an offscreen alpha raster.
    const bool featheredMask=hasFeatheredMask(l);
    for (auto value : l.masks) {
      const auto mask = value.toObject();
      if (!mask["enabled"].toBool(true)) continue;
      masked = true;
      // Once one mask feathers, all enabled masks are multiplied together in
      // an alpha raster below. Mixing that with painter clips would cut away
      // the soft pixels before they had a chance to reach the frame.
      if(!featheredMask)
        p.setClipPath(expandedMaskPath(mask,l,s),Qt::IntersectClip);
    }
    // While this layer draws, a card's hooks know the painter's clip without
    // asking it: the pass's own, unless a mask has just cut it.
    PassClipScope layerClip(onCard ? &p : nullptr, entryDeviceRect && !(masked && !featheredMask), entryDevice);
    p.setOpacity(std::clamp(l.p[5].at(t), 0., 1.));
    auto drawSharpFallback = [&] {
      layerClip.forget(); // its masks cut it below
      if(featheredMask)
        for(const auto &value:l.masks) {
          const auto mask=value.toObject();
          if(mask["enabled"].toBool(true))
            p.setClipPath(expandedMaskPath(mask,l,s),Qt::IntersectClip);
        }
      if (layerHasMainTransform(l)) {
        p.save();
        p.setWorldTransform(layerMainTransform(l), true);
        draw(p);
        p.restore();
      } else {
        draw(p);
      }
    };
    // A fully opaque solid covering the clip hides every halo behind it.
    // Prove coverage geometrically instead of allocating an enormous blur.
    if(!featheredMask && l.type=="fill" && l.color.alpha()==255 && !hostEffect &&
       !l.blur && !gradient && !strokeEffect && !layerHasPixelEffect(l) && !vectorEffectFill(l) &&
       !vectorEffectGradient(l) && p.hasClipping()) {
      bool invertible=false;
      const auto inverse=layerMainTransform(l).inverted(&invertible);
      if(invertible && layerBounds(l,s).contains(inverse.mapRect(p.clipBoundingRect()))) {
        p.save();p.setWorldTransform(layerMainTransform(l),true);draw(p);p.restore();
        p.restore();continue;
      }
    }
    // Text retains a shared raster because font hinting can otherwise differ
    // between the CPU and GL paths. A normal Shape, however, is already a
    // QPainterPath: sending it through an intermediate image turns a continuous
    // scale or rotation into repeated texture resampling. That was visible as
    // one-pixel jitter while zooming or adjusting Transform. Keep un-effected
    // Shape paths and plain Fill rectangles on the vector path; a Fill with
    // effect settings still uses its existing raster/effect pipeline.
    const bool plainFill = l.type == "fill" && l.appliedEffects.isEmpty() &&
                           l.surfaceEffects.isEmpty() && !hostEffect;
    const bool needsRaster = rasterCharacterStrokes ||

        (p.worldTransform().isAffine() && (l.type=="text" ||
                                          (l.type=="fill" && !plainFill))) ||
        l.blur || l.glow || l.shadow || gradient || strokeEffect || hostEffect || featheredMask ||
        layerHasPixelEffect(l);
    if (needsRaster) {
      // Letters on the move: the picture covers where they are now, not every
      // place their animation could take them. Only where nothing measures
      // itself on the picture's extent (a gradient, a host or pixel effect),
      // and not for a recorded drawing, which may be of another moment.
      QRectF box;
      if (!l.particlesOn && !gradient && !hostEffect && !layerHasPixelEffect(l) &&
          !(prepared && prepared->contains(l.id)) && !characterPoseAtRest(l, t))
        box = animatedTextBounds(l, t, s.fps);
      const QRectF reach = layerBounds(l, s);
      const bool lettersMove = !box.isNull();
      // Grown out to whole units from the corner of the box it replaces: the
      // picture keeps that box's sub-pixel phase, and the letters the same
      // pixels -- placed anywhere, they were resampled half a pixel over and
      // came out softer at every edge.
      // (The picture is laid out from the box's middle less half its width
      // rounded up; a box of whole units starts where that puts the old one.)
      if (!box.isNull()) {
        const QPointF phase(reach.center().x() - std::ceil(reach.width()) / 2,
                            reach.center().y() - std::ceil(reach.height()) / 2);
        box = QRectF(QPointF(phase.x() + std::floor(box.left() - phase.x()),
                             phase.y() + std::floor(box.top() - phase.y())),
                     QPointF(phase.x() + std::ceil(box.right() - phase.x()),
                             phase.y() + std::ceil(box.bottom() - phase.y())));
      } else box = reach;
      if(rasterCharacterStrokes && !lettersMove) {
        // The selection bounds report the exact stroke width, while the
        // offscreen margin stays in a small number of reusable buckets.  This
        // avoids reallocating and remeasuring a full raster for every decimal
        // value crossed while Width is scrubbed. Only the stroke's share of
        // the layer's box is swapped: the box of the lines alone, which this
        // once started from, cut off the feet of letters set tighter than
        // their own height (Leading under 1), a faux italic's lean and letter
        // backgrounds. Letters on the move keep their own box, which holds
        // every stroke already.
        const double exact=textStrokeOutwardReach(l);
        const double stable=exact>0?std::ceil(exact/16.)*16.:0;
        box=box.adjusted(exact-stable,exact-stable,stable-exact,stable-exact);
        // And out to the tips of Miter corners, which reach past the width.
        if(const QRectF corners=mitredTextStrokeBox(l); !corners.isNull()) box=box.united(corners);
      }
      // The effects see the layer as the main transform leaves it, so the
      // raster has to cover that and the gradient has to be measured on it.
      // (For a layer with a host effect, hostEffectContentBox says the same.)
      if (layerHasMainTransform(l)) box = layerMainTransform(l).mapRect(box);
      // A blur of radius R is three box passes of half-width ceil(R/2), so it
      // reaches 1.5R, not the 3R this padded for. Summed across stacked
      // blurs that was twice the border they need, and on a wide glow the
      // border is most of the raster.
      const double sourceStrokePad = l.type == "text" ? 0. : l.strokeWidth;
      int pad = qCeil((l.blur + l.glow + l.shadow) * 1.5 + effectPad + sourceStrokePad + 12);
      // And as far as the host's effects say they draw beyond the layer (an
      // OpenFX plug-in's region of definition), no further than the frame.
      if (hostEffect) pad += qCeil(hostEffectReach(l, s, t, box.size()));
      const double padded = qCeil(box.width()) + pad * 2., paddedHeight = qCeil(box.height()) + pad * 2.;
      QRectF logical(box.center().x()-padded/2., box.center().y()-paddedHeight/2., padded, paddedHeight);
      auto transform = p.worldTransform();
      double rasterScale = 1;
      bool scaleQuantized = false;
      if (transform.isAffine()) {
        rasterScale = std::max({.0001,
                                std::hypot(transform.m11(), transform.m12()),
                                std::hypot(transform.m21(), transform.m22())});
        // A layer that zooms has a slightly different scale every frame, and the
        // scale is part of what identifies its raster: a slow push-in on a
        // picture rebuilt every effect on it, every frame, for a difference of
        // a fraction of a pixel. Held to steps of a quarter of an octave, never
        // down -- the steps a 3D layer takes -- it is rebuilt only when the zoom
        // has grown by a fifth. Steps of a sixteenth (four per cent) were too
        // fine for a quick pop or a dive: every frame and every motion-blur
        // sample of a disc growing with a drop shadow passed one, and rebuilt
        // its shadow, 250 ms a frame through the dive of a title sequence. Never
        // coarser than the screen, a raster up to a fifth finer looks the same.
        // Only for layers whose scale is keyed; a still layer keeps its exact
        // scale, which is what makes CPU and GPU draw the same pixels.
        if (animatedScene && layerScaleAnimated(l, s)) {
          scaleQuantized = true;
          rasterScale = std::pow(2., std::ceil(std::log2(rasterScale) * 4. - 1e-6) / 4.);
        }
      } else {
        // A projected 3D plane is a homography. Its raw matrix contains the
        // camera zoom in m11/m22 and the camera distance in m33; reading only
        // m11/m22 made a normal 100% layer look thousands of times larger.
        // As soon as DOF requested a raster, that bogus scale asked for an
        // enormous image, allocation failed, and the layer vanished. Measure
        // the actual on-screen derivative at this layer instead.
        const QPointF at = box.center();
        const QPointF mapped = transform.map(at);
        const double scaleX = QLineF(mapped, transform.map(at + QPointF(1, 0))).length();
        const double scaleY = QLineF(mapped, transform.map(at + QPointF(0, 1))).length();
        rasterScale = std::max({.0001, scaleX, scaleY});
        // Stepped up to the next quarter octave, never down. The scale is part
        // of what identifies a raster, and this one is measured afresh from the
        // projection every frame: with a camera or a layer moving through 3D it
        // changed a little each time, so every frame rebuilt every effect
        // raster -- sixty glowing planes cost 370 ms a frame. Held to steps a
        // fifth apart it is rebuilt only when the plane has genuinely grown, and
        // being at most a fifth finer than the screen needs, it looks no worse.
        rasterScale = std::pow(2., std::ceil(std::log2(rasterScale) * 4.) / 4.);
      }
      // Output sampling and content bounds have different margins. A constant
      // layer-space margin grows enormously at high layer/viewer scales. A
      // pointwise gradient only needs the visible pixels plus AA guard pixels.
      // Spatial effects need their true neighbourhood, summed for effect chains.
      double sampleMargin = 2. / rasterScale;
      for(double radius : {l.blur,l.glow,l.shadow})
        if(radius>0) sampleMargin += 3.*std::max(1.,std::ceil(radius*rasterScale/2.))/rasterScale;
      if(l.shadow>0) sampleMargin += 7.;
      if(strokeEffect) {
        auto stroke=l.surfaceEffects["Stroke"].toObject();
        double width=std::clamp(stroke["width"].toDouble(4),0.,200.);
        sampleMargin += stroke["mode"].toInt(1)==2 ? width/2. : width;
      }
      // A letter just outside the visible region still throws its stroke into
      // it. Without this the raster was cut at the viewport's edge, the field
      // never saw those letters, and zoomed in the stroke ended in a straight
      // vertical line.
      double strokeReach=0;
      if(rasterCharacterStrokes) {
        strokeReach=strokeStackOutward(textStrokeStack(l));
        // A Miter corner's tip, past that.
        const QRectF corners=mitredTextStrokeBox(l), ink=measuredTextInk(l);
        if(!corners.isNull() && !ink.isNull())
          strokeReach=std::max({strokeReach,ink.left()-corners.left(),corners.right()-ink.right(),
                                ink.top()-corners.top(),corners.bottom()-ink.bottom()});
        sampleMargin += strokeReach;
      }
      // Only the part of the layer the target can show has to be rasterized,
      // grown by the same pad the effects reach across so their edges still
      // see the neighbours they need. Zoomed in, this is the difference
      // between a viewport and a whole magnified layer.
      // Keep small rasters independent of viewport/position, so translating a
      // layer reuses its completed effects. Large rasters retain ROI clipping.
      const bool cacheWhole = logical.width()*rasterScale*logical.height()*rasterScale <= 4194304.;
      // An effect that can bring any pixel of the layer anywhere (a tiling)
      // needs the layer's own box whole whatever part of the frame is drawn --
      // one band of it alone would tile holes -- but the room it fills around
      // the layer only where that shows.
      const bool wholeLayer = layerPackNeedsWholeLayer(l);
      const QRectF unclipped = logical;
      // The sides the part that can show cut the raster back on: past what
      // shows, it reaches there only as far as the effects need and the
      // strokes of letters beyond it reach in (strokeReach).
      bool cutLeft = false, cutTop = false, cutRight = false, cutBottom = false;
      if (p.hasClipping() && !cacheWhole) {
        const QRectF whole = logical;
        logical = logical.intersected(p.clipBoundingRect().adjusted(-sampleMargin, -sampleMargin, sampleMargin, sampleMargin));
        if (wholeLayer) logical = logical.united(box).intersected(whole);
        // The visible part of a layer larger than the frame slides as the layer
        // moves, and with it the raster's key: every frame, and every motion blur
        // sample within it, built the whole chain again for a strip a few
        // pixels different from the last. In an animated scene the strip is
        // widened outward to a 128 pixel grid, so neighbouring samples and
        // frames share one raster. Pixels already inside the visible strip are
        // unchanged: they only gain more of the layer beyond them.
        if (animatedScene && !logical.isEmpty() && rasterScale > 0) {
          const double step = 128. / rasterScale;
          const double left = std::floor(logical.left() / step) * step, top = std::floor(logical.top() / step) * step;
          const double right = std::ceil(logical.right() / step) * step, bottom = std::ceil(logical.bottom() / step) * step;
          logical = QRectF(QPointF(left, top), QPointF(right, bottom)).intersected(whole);
        }
        if (!wholeLayer) {
          cutLeft = logical.left() > whole.left() + 1e-9;
          cutTop = logical.top() > whole.top() + 1e-9;
          cutRight = logical.right() < whole.right() - 1e-9;
          cutBottom = logical.bottom() < whole.bottom() - 1e-9;
        }
      }
      if (logical.isEmpty()) { p.restore(); continue; }
      if(!scaleQuantized && transform.isAffine() && transform.m12()==0 && transform.m21()==0 &&
         transform.m11()>0 && transform.m11()==transform.m22()) {
        // Align offscreen samples to the final pixel grid. Otherwise CPU and
        // GL resample half-pixel text edges differently at Half/Quarter.
        //
        // In a scene with animation the position is rounded to a whole pixel
        // first. The raster is part of the cache key, so aligning to the exact
        // sub-pixel phase gave every frame of any animation a new key: nothing
        // was reused while a layer moved, and a scene of a few hundred glowing
        // layers rebuilt all of them thirty times a second. Rounded, a layer
        // has one raster however it moves, and the composition's smooth
        // sampling resamples it across the remaining sub-pixel offset the way
        // any moving layer's bitmap is. A scene with nothing keyed keeps the
        // exact phase, which is what makes CPU and GPU draw the same pixels.
        //
        // Except under a host's effect (an OpenFX plug-in): drawn in another
        // process, often a tenth of a second or more, and a layer dragged in
        // the preview lands on a new fraction of a pixel at every step -- each
        // a new raster, so the plug-in ran again on every mouse move, 300 ms a
        // step. Rounded, the drag slides the picture it already made.
        auto snapped=transform;
        if(animatedScene || hostEffect)
          snapped.setMatrix(transform.m11(),transform.m12(),transform.m13(),
                            transform.m21(),transform.m22(),transform.m23(),
                            std::round(transform.dx()),std::round(transform.dy()),
                            transform.m33());
        const QRectF device=snapped.mapRect(logical);
        logical=snapped.inverted().mapRect(QRectF(device.toAlignedRect()));
      }
      // A layer whose whole pixel chain is one soft copy laid behind it never
      // has its own pixels blurred, so it does not need a raster at its own
      // resolution -- or a raster at all. The halo is resolved at the scale its
      // blur can express, and the layer goes into the frame as the vectors it
      // already is, exactly as a layer with no effects would.
      //
      // Laying the halo down and then drawing the layer over it gives the same
      // pixels as compositing the two inside a raster and drawing that, but
      // only at full opacity: below it the two forms weight the halo showing
      // through the layer differently, so a faded layer keeps the raster.
      // Exactly one halo, because a second one blurs the first one's result.
      // Only worth it when the finished pixels would be too big to keep. Below
      // a megapixel the raster is cached whole and blitting it back is cheaper
      // than drawing the layer again -- measured on twenty glowing layers at
      // twice scale, 10.3 ms the cached way against 30.2 ms this way. At six
      // times scale, where nothing caches, it is 729.7 ms against 81.0 ms.
      // Not when a pack effect is in the chain: drawn this way, the layer goes
      // into the frame as its vectors and every pack effect on it -- a grade,
      // a tiling that fills the frame -- is simply never run.
      //
      // A layer zooming in is the exception to that: its raster is a new one at
      // every quarter octave anyway, so nothing is kept by caching it whole,
      // while its halo can be kept (steadyHaloScale below).
      const bool oneHaloChain = !featheredMask && !rasterCharacterStrokes && !l.blur && !gradient && !strokeEffect && !hostEffect &&
                                !layerHasPixelEffect(l) && (l.glow != 0) != (l.shadow != 0);
      const double haloRadius = l.glow ? l.glow : l.shadow;
      const QSize full(qCeil(logical.width() * rasterScale),
                       qCeil(logical.height() * rasterScale));
      const double screenHaloScale = rasterScale / blurReduction(haloRadius * rasterScale, full);
      const double steadyHalo = oneHaloChain && scaleQuantized ? steadyHaloScale(haloRadius, unclipped) : 0.;
      const bool keepsHalo = steadyHalo > 0 && steadyHalo < screenHaloScale;
      const bool oneHalo = oneHaloChain && (!cacheWhole || keepsHalo);
      if (oneHalo && (p.opacity() >= 1 || keepsHalo)) {
        const double radius = haloRadius;
        const QColor tint = l.glow ? QColor() : QColor(0, 0, 0, 200);
        const QPointF offset = l.glow ? QPointF() : QPointF(5, 7);
        double haloScale = screenHaloScale;
        // A layer zooming in -- a dive through a stack of shadowed discs --
        // asked for a new halo at every quarter octave and for every strip of
        // it the frame showed, which slid with the zoom: each motion-blur
        // sample of every disc built its own, 330 ms a frame for nine of them.
        // A halo holds no detail finer than its blur, so once the screen is
        // finer than the blur needs it is resolved at a scale set by the blur
        // alone (half-octave steps, eight to twelve pixels of blur) and whole:
        // one picture, built once, that every sample and frame of the zoom
        // draws enlarged.
        QRectF haloLogical = logical;
        if (keepsHalo) {
          haloScale = steadyHalo;
          haloLogical = unclipped;
        }
        // The halo is a completed effect like any other, so it lives in the
        // same cache under a key that carries the scale it was resolved at.
        // A small layer's bounds do not move when the layer does, so dragging
        // one still costs nothing.
        // Marked as the halo alone: at a scale and framing the layer's own
        // raster can also have, the two would otherwise share one key, and the
        // layer drawn from its raster the next frame came out as its shadow.
        const auto haloKey = effectedRasterKey(l, s, t, haloLogical, haloScale, box, QByteArray("halo only"));
        QImage halo = cachedEffectedRaster(haloKey);
        if (!halo.isNull()) ++effectHits;
        QImage small;
        if (halo.isNull())
          small = QImage(qCeil(haloLogical.width() * haloScale), qCeil(haloLogical.height() * haloScale),
                         processingFormat(s.processingDepth, true));
        if (!halo.isNull() || !small.isNull()) {
          if (halo.isNull()) {
            QElapsedTimer stage; stage.start();
            small.fill(Qt::transparent);
            { QPainter q(&small);
              q.setRenderHint(QPainter::Antialiasing);
              q.setRenderHint(QPainter::TextAntialiasing);
              q.scale(haloScale, haloScale);
              q.translate(-haloLogical.left(), -haloLogical.top());
              if (layerHasMainTransform(l)) q.setWorldTransform(layerMainTransform(l), true);
              draw(q);
            }
            sourceNs += quint64(stage.nsecsElapsed());
            stage.restart();
            halo = renderHaloOnly(small, radius * haloScale, tint);
            // Kept in the depth the frame is painted in, as at 8 bpc. Handed a
            // float halo, a 16 bpc frame widened it back to 16 bits as it drew
            // it: four neighbours of every pixel the halo covered, in every
            // motion-blur sample of every band. The Twitter promo at 1080p drew
            // 692 halos a frame that way, 464 ms a frame against 162 converted
            // once here (101 at 8 bpc), and 2022 against 744 where ten layers
            // move. Qt widens a float picture with this same conversion as it
            // reads it, so the frame is the same to the byte. On the card the
            // halo goes up as a 16-bit texture, half the bytes; a few hundred
            // pixels of a frame land an 8-bit step or two apart. 32 bpc keeps
            // floats, light above white and all.
            if (!halo.isNull() && s.processingDepth <= 16)
              halo = halo.convertToFormat(s.processingDepth <= 8 ? QImage::Format_ARGB32_Premultiplied
                                                                 : QImage::Format_RGBA64_Premultiplied);
            effectNs += quint64(stage.nsecsElapsed());
            if (!halo.isNull()) storeEffectedRaster(haloKey, halo);
          }
          if (!halo.isNull()) {
            // Halo, then the layer over it.
            auto lay = [&](QPainter &onto) {
              const bool smoothed = onto.testRenderHint(QPainter::SmoothPixmapTransform);
              onto.setRenderHint(QPainter::SmoothPixmapTransform, true);
              onto.drawImage(QRectF(haloLogical.left() + offset.x(), haloLogical.top() + offset.y(),
                                    halo.width() / haloScale, halo.height() / haloScale), halo);
              onto.setRenderHint(QPainter::SmoothPixmapTransform, smoothed);
              if (layerHasMainTransform(l)) {
                onto.save();
                onto.setWorldTransform(layerMainTransform(l), true);
                draw(onto);
                onto.restore();
              } else draw(onto);
            };
            if (p.opacity() >= 1) {
              lay(p);
              p.restore();
              continue;
            }
            // Faded -- a title zooming at the camera as it goes -- the two are
            // one picture faded together, as its raster would be: laid
            // together on a sheet the size of what shows, and the sheet faded.
            // Kept to its raster, the zooming title built one the size of the
            // frame at every quarter octave of the zoom, shadow and all.
            const QRect device = p.paintEngine() && p.device()
                                     ? QRect(0, 0, p.device()->width(), p.device()->height()) : QRect();
            QRect area = p.deviceTransform().mapRect(haloLogical).toAlignedRect().adjusted(-2, -2, 2, 2);
            if (p.hasClipping()) area &= p.deviceTransform().mapRect(p.clipBoundingRect()).toAlignedRect().adjusted(-1, -1, 1, 1);
            if (!device.isEmpty()) area &= device;
            if (!area.isEmpty()) {
              QImage sheet(area.size(), processingFormat(s.processingDepth, true));
              if (!sheet.isNull()) {
                sheet.fill(Qt::transparent);
                {
                  QPainter q(&sheet);
                  q.setRenderHints(p.renderHints());
                  q.setTransform(p.deviceTransform() * QTransform::fromTranslate(-area.left(), -area.top()));
                  lay(q);
                }
                p.resetTransform();
                p.drawImage(area.topLeft(), sheet);
              }
            }
            p.restore();
            continue;
          }
        }
      }
      const QByteArray hostKey =
          hostEffect && host->identity ? host->identity(l, t, s) : QByteArray();
      // Not when the layer was altered for this frame (depth of field adds blur that
      // differs from sample to sample): then its identity is not the stored one.
      const bool memoKeys = !adjusted.has_value() && keyMemoApplies(s, index, l, logical, rasterScale, box, hostKey);
      QByteArray rasterKey;
      if (memoKeys && g_keyMemo.hasRaster) rasterKey = g_keyMemo.raster;
      else {
        rasterKey = effectedRasterKey(l, s, t, logical, rasterScale, box, hostKey);
        if (memoKeys) { g_keyMemo.raster = rasterKey; g_keyMemo.hasRaster = true; }
      }
      bool compositedOnCard = false;
      QImage source = cachedEffectedRaster(rasterKey);
      if (!source.isNull()) ++effectHits;
      std::optional<BuildTicket> ticket;
      if (source.isNull()) {
        ticket.emplace(rasterKey);
        source = cachedEffectedRaster(rasterKey);
        if (!source.isNull()) ++effectHits;
      }
      if (source.isNull()) {
        QByteArray inputKey;
        if (memoKeys && g_keyMemo.hasSource) inputKey = g_keyMemo.source;
        else {
          inputKey = sourceRasterKey(l,s,t,logical,rasterScale,box);
          if (memoKeys) { g_keyMemo.source = inputKey; g_keyMemo.hasSource = true; }
        }
        QImage input;
        // The pixels whose strokes can show. On a side cut back to what shows
        // (cutLeft...), the raster's last strokeReach is there only for the
        // letters in it, whose strokes reach in: those pixels need no strokes,
        // nor blurred letters or backgrounds of their own. A layer with pack
        // or host effects keeps every one.
        QRect strokesWithin(0,0,qCeil(logical.width()*rasterScale),qCeil(logical.height()*rasterScale));
        if(rasterCharacterStrokes && !hostEffect && !layerHasPixelEffect(l)) {
          const int spare=std::max(0,int(std::floor(strokeReach*rasterScale))-2);
          const QRect kept=strokesWithin.adjusted(cutLeft?spare:0,cutTop?spare:0,cutRight?-spare:0,cutBottom?-spare:0);
          if(!kept.isEmpty()) strokesWithin=kept;
        }
        { QMutexLocker lock(&rasterCacheGuard());
          if(auto *hit=sourceCache().object(inputKey)) { input=*hit; ++sourceHits; } }
        if(input.isNull()) {
          QElapsedTimer stage;stage.start();
          const int sourceWidth=qCeil(logical.width()*rasterScale);
          const int sourceHeight=qCeil(logical.height()*rasterScale);
          const auto sourceFormat=processingFormat(s.processingDepth,true);
          input=QImage(sourceWidth,sourceHeight,sourceFormat);
          if(input.isNull()) {
            // A spatial effect may run out of raster memory at an extreme
            // camera scale. Keep the layer visible and sharp for this frame;
            // silently dropping it is never an acceptable fallback for DOF.
            drawSharpFallback();
            p.restore();
            continue;
          }
          std::vector<QPainterPath> letterOutlines;
          AnimatedLetterStrokes animatedLetters;
          animatedLetters.within=QRectF(strokesWithin);
          animatedLetters.raster=QRectF(0,0,sourceWidth,sourceHeight);
          // The letters' backgrounds, kept apart while the strokes are built
          // from the letters' distance (characterStrokeBackdrop).
          LetterBackdrop backdrop;
          const bool keepsBackdrop=rasterCharacterStrokes && layerHasLetterBackground(l);
          // An animated text with no strokes: its blurred letters set apart
          // and laid after (characterLettersApart). Not with particles, which
          // go over the letters in each strip: laid after, the letters went
          // over them.
          const bool lettersApart=!rasterCharacterStrokes && lettersAnimated && l.type=="text" && !l.particlesOn;
          auto paintSource=[&](QImage &target,int top) {
            target.fill(Qt::transparent);
            QPainter q(&target);
            q.setRenderHint(QPainter::Antialiasing);
            q.setRenderHint(QPainter::TextAntialiasing);
            // Each worker owns its image. The first translation places this
            // strip at its rows in the complete raster; the remaining two are
            // exactly the old single-image transform.
            q.translate(0,-top);
            q.scale(rasterScale,rasterScale);q.translate(-logical.left(),-logical.top());
            if(layerHasMainTransform(l))
              q.setWorldTransform(layerMainTransform(l),true);
            if(rasterCharacterStrokes) {
              const LetterStage restore;
              AnimatedLetterStrokes painting;
              painting.collect=false;
              characterStrokesDeferred=true;
              // The strip at the top maps straight onto the whole raster's
              // pixels, and every strip paints the same letters, so one
              // collection is all of them.
              characterStrokeLetters=top==0?&letterOutlines:nullptr;
              characterStrokeAnimated=top==0?&animatedLetters:&painting;
              // The backgrounds by that strip too, all of them: its transform
              // is the whole raster's.
              characterStrokeBackdrop=top==0 && keepsBackdrop?&backdrop:nullptr;
              // Handed to that strip's own collection alone. The strips paint
              // at once, and each one set the shared collection's and cleared
              // it after: the strip that keeps the backgrounds found nowhere to
              // keep them whenever another got in between, and a large animated
              // title -- a raster painted in strips -- lost them.
              characterStrokeAnimated->backdrop=characterStrokeBackdrop;
              // Straight to the drawing: a prepared picture was recorded with
              // the strokes already in it.
              drawLayer(q,l,s,t);
              characterStrokeAnimated->backdrop=nullptr;
            } else if(lettersApart) {
              // No strokes to build, but the blurred letters set apart all the
              // same: the strip at the top collects them, to be blurred once
              // and laid over the raster after, not again by every strip for
              // its own rows.
              const LetterStage restore;
              AnimatedLetterStrokes painting;
              painting.collect=false;
              characterStrokeAnimated=top==0?&animatedLetters:&painting;
              characterLettersApart=true;
              draw(q);
            } else draw(q);
            q.end();
          };
          // A magnified vector cannot be cached as one small texture and used
          // to force the whole composition onto one core. Rasterise disjoint
          // row strips with the same fast vector path instead. No worker
          // repeats another worker's pixels, and the assembled image remains
          // byte-identical to the serial input consumed by every effect.
          const bool vectorSource=l.type=="text" || l.type=="fill" || l.type=="shape" ||
                                  parametricShape(l);
          int sourceWorkers=(!inRenderWorker && vectorSource &&
                             qint64(sourceWidth)*sourceHeight>=1048576)
                                ? std::min(renderWorkerLimit(),std::max(1,sourceHeight/128)) : 1;
          if(sourceWorkers==1) {
            paintSource(input,0);
          } else {
            // Each strip painted where it lies in the raster, through a view of
            // its rows: no picture of its own to be made and copied across, a
            // frame's worth of memory a frame. Every strip is waited for, a
            // failed one too: left running, the others drew through what this
            // frame had already let go.
            uchar *const rows=input.bits();
            const qsizetype stride=input.bytesPerLine();
            std::vector<std::future<void>> strips;
            strips.reserve(sourceWorkers);
            for(int worker=0;worker<sourceWorkers;++worker) {
              const int top=sourceHeight*worker/sourceWorkers;
              const int bottom=sourceHeight*(worker+1)/sourceWorkers;
              strips.push_back(renderJob([&,top,bottom] {
                QImage strip(rows+top*stride,sourceWidth,bottom-top,stride,sourceFormat);
                paintSource(strip,top);
              }));
            }
            std::exception_ptr failed;
            for(auto &strip:strips) {
              try { strip.get(); } catch(...) { if(!failed) failed=std::current_exception(); }
            }
            if(failed) std::rethrow_exception(failed);
          }
          if(input.isNull()) {
            drawSharpFallback();
            p.restore();
            continue;
          }
          if(rasterCharacterStrokes) {
            // Once, over the assembled raster. Per strip, each strip would
            // measure distance only to the letters inside it, and a seam would
            // run along every strip boundary a stroke crosses.
            const double mainScale=layerHasMainTransform(l)
                ? std::sqrt(std::abs(layerMainTransform(l).determinant())) : 1.;
            // What the blurred letters and backgrounds were left to need.
            finishLetterJobs(animatedLetters);
            if(lettersAnimated) {
              // Which letter each pixel went with, where a background fades
              // with its letter (layLetterBackdrop).
              std::vector<uint16_t> owners;
              textStrokeStackPass(input,textStrokeStack(l),rasterScale*mainScale,1.,
                                  animatedLetters.outlines,textStackStrokeOnly(l),
                                  animatedLetters.opacity,animatedLetters.scale,
                                  backdropFadesWithLetters(backdrop)?&owners:nullptr,strokesWithin);
              // The backgrounds under the letters and their strokes, each
              // fading with its letter.
              layLetterBackdrop(input,backdrop,owners,&animatedLetters,strokesWithin);
              // The blurred letters, finished apart, over the rest.
              layLetterOverlays(input,animatedLetters.overlays,strokesWithin);
            } else {
              textStrokeStackPass(input,textStrokeStack(l),rasterScale*mainScale,
                                  1.,letterOutlines,textStackStrokeOnly(l),{},{},nullptr,strokesWithin);
              // The backgrounds under all of it: letters over strokes over
              // backgrounds, as when the backgrounds were painted first.
              layLetterBackdrop(input,backdrop,{},nullptr,strokesWithin);
            }
            // The particles over all of it, as `draw` lays them.
            drawParticlesOverRaster(input,l,s,t,logical,rasterScale);
          } else if(lettersApart) {
            // The blurred letters set apart, blurred once and laid over the
            // rest, as each strip used to lay them over its own rows.
            finishLetterJobs(animatedLetters);
            layLetterOverlays(input,animatedLetters.overlays,strokesWithin);
          }
          sourceNs+=quint64(stage.nsecsElapsed());
          ++sourcesBuiltHere;
          const qint64 cost=(input.sizeInBytes()+1023)/1024;
          if(cost<=65536) {
            QMutexLocker lock(&rasterCacheGuard());
            sourceCache().insert(inputKey,new QImage(input),int(std::max<qint64>(1,cost)));
          }
        }
        // Work from the glyph alpha after all strips have been assembled.  The
        // same morphology is therefore consumed by CPU effects and by the GPU
        // pass chain, so backend selection cannot change the text shape.
        QElapsedTimer stage;stage.start();
        // Both backends receive exactly the same source and transform.
        LayerEffectResult produced;
        // A layer carrying a host effect stays on the CPU. The card composites
        // straight into the frame and would leave no raster for the plugin to
        // be applied to, so it would simply be skipped.
        if(effects && !hostEffect && !l.repeaterEnabled)
          produced=(*effects)(l,t,logical,rasterScale,box,input,masked);
        compositedOnCard=produced.composited;
        source=produced.image;
        if(!compositedOnCard && source.isNull()) {
          const bool hasSpatial=l.blur||l.glow||l.shadow;
          const bool hasPreSpatial=
              (l.appliedEffects["Fill"].toBool()&&!l.hiddenEffects["Fill"].toBool()&&!vectorEffectFill(l))||
              (l.appliedEffects["Gradient"].toBool()&&!l.hiddenEffects["Gradient"].toBool()&&!vectorEffectGradient(l))||
              (l.appliedEffects["Tint"].toBool()&&!l.hiddenEffects["Tint"].toBool())||
              (l.appliedEffects["Stroke"].toBool()&&!l.hiddenEffects["Stroke"].toBool()&&!vectorEffectStroke(l));
          const auto effectBox=QRectF((box.left()-logical.left())*rasterScale,
              (box.top()-logical.top())*rasterScale,box.width()*rasterScale,box.height()*rasterScale);
          EffectTimeScope effectTime(t / std::max(1, s.fps), t); // what moving noise reads
          if(hasSpatial && hasPreSpatial && !hostEffect && !canUseGpuPixels() && preSpatialSplitKeepsOrder(l)) {
            QByteArray pbKey;
            if (memoKeys && g_keyMemo.hasPre) pbKey = g_keyMemo.pre;
            else {
              pbKey = preBlurKey(l,s,t,logical,rasterScale,box);
              if (memoKeys) { g_keyMemo.pre = pbKey; g_keyMemo.hasPre = true; }
            }
            QImage preBlurInput;
            { QMutexLocker lock(&rasterCacheGuard());
              if(auto *hit=preBlurCache().object(pbKey)) preBlurInput=*hit; }
            if(preBlurInput.isNull()) {
              preBlurInput=applyPreSpatialEffects(input,l,effectBox,rasterScale);
              const qint64 cost=(preBlurInput.sizeInBytes()+1023)/1024;
              if(cost<=65536) {
                QMutexLocker lock(&rasterCacheGuard());
                preBlurCache().insert(pbKey,new QImage(preBlurInput),int(std::max<qint64>(1,cost)));
              }
            }
            source=applySpatialEffects(preBlurInput,l,effectBox,rasterScale);
          } else {
            // A plug-in may read the layer at other times: drawn as `input`
            // is, at that time (kept in the same cache, so playing on finds the
            // frames before this one already made).
            LayerOtherTimes otherTimes;
            if(hostEffect) {
              otherTimes.layer=&l;
              otherTimes.fps=std::max(1,s.fps);
              otherTimes.draft=s.previewDraft;
              // Another layer, as an input: drawn into this raster where it is
              // in the frame -- the painter's own way there undone back to
              // this layer's pixels -- or its own picture over this layer's.
              // An input whose effects take this layer as theirs is followed
              // once, no further.
              otherTimes.otherLayer=[&](const QString &id, bool stretched) -> QImage {
                thread_local int depth=0;
                if(depth>=2 || id.isEmpty() || id==l.id) return {};
                const Layer *other=nullptr;
                for(const auto &candidate:s.layers) if(candidate.id==id) { other=&candidate; break; }
                if(!other) return {};
                struct Deeper {
                  bool previous;
                  Deeper() : previous(inRenderWorker) { inRenderWorker=true; ++depth; }
                  ~Deeper() { inRenderWorker=previous; --depth; }
                } deeper;
                QImage out(input.size(),QImage::Format_ARGB32_Premultiplied);
                if(out.isNull()) return {};
                out.fill(Qt::transparent);
                QPainter q(&out);
                q.setRenderHint(QPainter::Antialiasing);
                q.setRenderHint(QPainter::SmoothPixmapTransform);
                if(stretched) {
                  const QImage own=layerTexturePicture(*other,s,frame);
                  if(own.isNull()) return {};
                  q.drawImage(effectBox,own);
                } else {
                  // That layer alone, as the frame shows it, in a render of
                  // its own (this one's buffers are in use): the rest hidden,
                  // and it shown even when its eye is off, as a matte is.
                  Scene alone=s;
                  for(auto &candidate:alone.layers) {
                    if(candidate.id==id) candidate.visible=true;
                    else if(candidate.type!="camera" && candidate.type!="light") candidate.visible=false;
                    candidate.isolated=false;
                  }
                  const QSize size=outputSize.isEmpty()?QSize(s.width,s.height):outputSize;
                  const QImage seen=renderCPU(alone,frame,false,size);
                  if(seen.isNull()) return {};
                  // From the frame's pixels to this raster's: back through the
                  // painter's way to this layer, then onto the raster.
                  q.setTransform(entryTransform*transform.inverted()*
                                 QTransform::fromTranslate(-logical.left(),-logical.top())*
                                 QTransform::fromScale(rasterScale,rasterScale));
                  q.drawImage(QPointF(0,0),seen);
                }
                q.end();
                return out;
              };
              otherTimes.source=[&](double when) -> QImage {
                when=std::clamp(when,0.,std::max(0.,l.duration-1e-6));
                Layer then=l;
                if(l.type=="video" && !l.path.isEmpty()) {
                  const QImage footage=footageAt(l,when,s.fps);
                  if(!footage.isNull()) then.image=footage;
                }
                const QByteArray key=sourceRasterKey(then,s,when,logical,rasterScale,box);
                { QMutexLocker lock(&rasterCacheGuard());
                  if(auto *hit=sourceCache().object(key)) return *hit; }
                QImage frame(input.size(),input.format());
                if(frame.isNull()) return {};
                frame.fill(Qt::transparent);
                std::vector<QPainterPath> outlines;
                AnimatedLetterStrokes animated;
                animated.within=QRectF(strokesWithin);
                animated.raster=QRectF(QPointF(),QSizeF(frame.size()));
                LetterBackdrop backdrop;
                const bool apart=!rasterCharacterStrokes && lettersAnimated && then.type=="text" && !then.particlesOn;
                {
                  QPainter q(&frame);
                  q.setRenderHint(QPainter::Antialiasing);
                  q.setRenderHint(QPainter::TextAntialiasing);
                  q.scale(rasterScale,rasterScale);q.translate(-logical.left(),-logical.top());
                  if(layerHasMainTransform(then))
                    q.setWorldTransform(layerMainTransform(then),true);
                  if(rasterCharacterStrokes) {
                    const LetterStage restore;
                    characterStrokesDeferred=true;
                    characterStrokeLetters=&outlines;
                    characterStrokeAnimated=&animated;
                    characterStrokeBackdrop=layerHasLetterBackground(then)?&backdrop:nullptr;
                    animated.backdrop=characterStrokeBackdrop;
                    drawLayer(q,then,s,when);
                    animated.backdrop=nullptr;
                  } else if(apart) {
                    // As the main raster sets them apart: the same pixels
                    // under the same key, whichever made them first.
                    const LetterStage restore;
                    characterStrokeAnimated=&animated;
                    characterLettersApart=true;
                    drawLayer(q,then,s,when);
                  } else {
                    drawLayer(q,then,s,when);
                    if(then.particlesOn && then.type!="particles") drawParticles(q,then,s,when);
                  }
                }
                if(apart) {
                  finishLetterJobs(animated);
                  layLetterOverlays(frame,animated.overlays,strokesWithin);
                }
                if(rasterCharacterStrokes) {
                  const double mainScale=layerHasMainTransform(then)
                      ? std::sqrt(std::abs(layerMainTransform(then).determinant())) : 1.;
                  finishLetterJobs(animated);
                  if(lettersAnimated) {
                    std::vector<uint16_t> owners;
                    textStrokeStackPass(frame,textStrokeStack(then),rasterScale*mainScale,1.,
                                        animated.outlines,textStackStrokeOnly(then),
                                        animated.opacity,animated.scale,
                                        backdropFadesWithLetters(backdrop)?&owners:nullptr,strokesWithin);
                    layLetterBackdrop(frame,backdrop,owners,&animated,strokesWithin);
                    layLetterOverlays(frame,animated.overlays,strokesWithin);
                  } else {
                    textStrokeStackPass(frame,textStrokeStack(then),rasterScale*mainScale,
                                        1.,outlines,textStackStrokeOnly(then),{},{},nullptr,strokesWithin);
                    layLetterBackdrop(frame,backdrop,{},nullptr,strokesWithin);
                  }
                  drawParticlesOverRaster(frame,then,s,when,logical,rasterScale);
                }
                const qint64 cost=(frame.sizeInBytes()+1023)/1024;
                if(cost<=65536) {
                  QMutexLocker lock(&rasterCacheGuard());
                  sourceCache().insert(key,new QImage(frame),int(std::max<qint64>(1,cost)));
                }
                return frame;
              };
            }
            LayerOtherTimesScope otherScope(hostEffect ? &otherTimes : g_otherTimes);
            source=applyLayerEffects(input,l,effectBox,rasterScale,t);
          }
        }
        if (!compositedOnCard && source.isNull()) source = input;
        effectNs+=quint64(stage.nsecsElapsed());
        // Nothing to keep when the result never came back as pixels. Normalize
        // the CPU painter format before inserting it, so one edit performs one
        // cache write rather than storing and immediately replacing the same
        // raster after conversion below.
        if(!compositedOnCard) {
          if (s.processingDepth <= 8 &&
              source.format() != QImage::Format_ARGB32_Premultiplied)
            source = inPainterFormat(source);
          storeEffectedRaster(rasterKey,source);
        }
      }
      ticket.reset();
      // Qt's painters are built around ARGB32 premultiplied; handing them
      // anything else converts the whole image on every single draw.
      if (!compositedOnCard) {
        if (s.processingDepth <= 8 && source.format() != QImage::Format_ARGB32_Premultiplied) {
          source = inPainterFormat(source);
        }
        QRect inkSource(QPoint(), source.size());
        // Through a perspective the raster costs several times what it does turned
        // or scaled, so there it is worth finding the part that is not margin.
        if (!featheredMask && !p.worldTransform().isAffine() &&
            source.format() == QImage::Format_ARGB32_Premultiplied &&
            qint64(source.width()) * source.height() >= 16384)
          inkSource = inkBounds(rasterKey, source);
        if(featheredMask) {
          // The masked picture does not depend on where the layer is, only on
          // its raster and its masks, so it is kept. Applying the matte copies
          // the whole raster and multiplies the area the feather reaches; done
          // afresh for every temporal sample of a moving layer, a full-frame
          // layer with a wide feather cost several milliseconds per sample.
          QByteArray maskedIdentity = rasterKey;
          QDataStream maskedStream(&maskedIdentity, QIODevice::Append);
          maskedStream << QJsonDocument(l.masks).toJson(QJsonDocument::Compact)
                       << logical << rasterScale << source.size() << qint32(source.format());
          const QByteArray maskedKey =
              QCryptographicHash::hash(maskedIdentity, QCryptographicHash::Sha256);
          QImage masked = cachedEffectedRaster(maskedKey);
          if (masked.isNull()) {
            BuildTicket building(maskedKey);
            masked = cachedEffectedRaster(maskedKey);
            if (masked.isNull()) {
              masked = applyFeatheredMasks(source,l,s,logical,rasterScale);
              storeEffectedRaster(maskedKey, masked);
            }
          }
          source = masked;
          if (source.format() == QImage::Format_ARGB32_Premultiplied)
            inkSource = inkBounds(maskedKey, source);
        }
        const QRectF target(logical.left() + inkSource.x() / rasterScale, logical.top() + inkSource.y() / rasterScale,
                            inkSource.width() / rasterScale, inkSource.height() / rasterScale);
        // On a card, from a texture it keeps, as a picture layer is drawn: Qt's
        // own small texture cache let a frame's worth of titles go (88 in the
        // Kotonoha MV), and every one was sent up again each frame.
        const bool onCard = s.processingDepth <= 8 && inkSource == QRect(QPoint(), source.size()) &&
                            g_imageCard && *g_imageCard && p.device() &&
                            p.device()->devType() != QInternal::Image && p.device()->devType() != QInternal::Picture &&
                            (*g_imageCard)(p, target, source);
        // A worker warming the caches paints onto a picture no one looks at:
        // what it came for is kept by now.
        if (!onCard && !g_neverWaitForBuild) p.drawImage(target, source, inkSource);
      }
    } else if (layerHasMainTransform(l)) {
      // No effects to sit between them, so the two transforms are one.
      p.save();
      p.setWorldTransform(layerMainTransform(l), true);
      draw(p);
      p.restore();
    } else
      draw(p);
    p.restore();
    }
  }
  p.restore();
}
// How many layers may build their effects at once. Each build holds several
// float copies of its raster (sixteen bytes a pixel), so on big layers every
// extra worker is hundreds of megabytes -- twenty at once on a large glowing
// layer is gigabytes -- and the speed-up stops at about six anyway, when the
// memory system rather than the arithmetic is the limit.
static int warmWorkerCap(const Scene &s, const Layer &sample, QSize outputSize) {
  const double scale = double(outputSize.width()) / std::max(1, s.width);
  const QRectF box = layerBounds(sample, s);
  const double pad = (sample.blur + sample.glow + sample.shadow) * 1.5 + sample.strokeWidth + 20;
  const double pixels = (box.width() + 2 * pad) * (box.height() + 2 * pad) * scale * scale;
  const double bytesPerBuild = std::isfinite(pixels) ? pixels * 16 * 5 : 4e9;
  return std::clamp(int(1.5e9 / std::max(1., bytesPerBuild)), 2, 1000);
}
// Frames are made by the thousand, eight megabytes each in HD. Memory that size
// is new to the process every time it is asked for: touching it costs page
// faults, and giving it back costs again -- over a millisecond a frame, more
// than a light frame takes to draw. So the memory of a frame nobody holds any
// more is kept, and the next frame of that size is drawn into it.
namespace {
struct FramePool {
  QMutex guard;
  std::map<size_t, std::vector<uchar *>> spare;
  size_t spareBytes = 0;
};
// Never destroyed: a frame can be let go of after every static is gone.
FramePool &framePool() {
  static FramePool *pool = new FramePool;
  return *pool;
}
constexpr size_t kFrameHeader = 64;          // the block's size is kept here, ahead of the pixels
constexpr size_t kFramePoolBytes = 384ull << 20;
constexpr size_t kFramesPerSize = 16;
void giveBackFrame(void *block) {
  auto *start = static_cast<uchar *>(block);
  size_t bytes = 0;
  std::memcpy(&bytes, start, sizeof bytes);
  FramePool &pool = framePool();
  {
    QMutexLocker lock(&pool.guard);
    auto &spare = pool.spare[bytes];
    if (pool.spareBytes + bytes <= kFramePoolBytes && spare.size() < kFramesPerSize) {
      spare.push_back(start);
      pool.spareBytes += bytes;
      return;
    }
  }
  ::operator delete(start, std::align_val_t(64));
}
// A picture of this size and format, in memory a frame before it used if there
// is any. Its pixels are whatever they were: the caller fills it.
QImage pooledImage(QSize size, QImage::Format format) {
  const int bits = QImage::toPixelFormat(format).bitsPerPixel();
  const qsizetype stride = (qsizetype(size.width()) * bits / 8 + 3) & ~qsizetype(3);
  const size_t bytes = size_t(stride) * size_t(std::max(0, size.height()));
  if (size.isEmpty() || bytes == 0) return QImage(size, format);
  uchar *start = nullptr;
  FramePool &pool = framePool();
  {
    QMutexLocker lock(&pool.guard);
    auto found = pool.spare.find(bytes);
    if (found != pool.spare.end() && !found->second.empty()) {
      start = found->second.back();
      found->second.pop_back();
      pool.spareBytes -= bytes;
    }
  }
  if (!start) {
    start = static_cast<uchar *>(::operator new(bytes + kFrameHeader, std::align_val_t(64), std::nothrow));
    if (!start) return QImage(size, format);
    std::memcpy(start, &bytes, sizeof bytes);
  }
  return QImage(start + kFrameHeader, size.width(), size.height(), stride, format, giveBackFrame, start);
}
// Qt's own premultiplied-to-straight conversion, value by value. The table is
// made by Qt converting every premultiplied value at every alpha, once, so a
// frame converted with it is exactly the frame Qt's conversion would make, on
// whatever processor Qt picked its conversion for. (A Source blit, which looks
// like the same thing, rounds 8038 of those values the other way.)
struct UnpremultiplyTable { uchar value[256][256]; }; // [alpha][premultiplied value]
const UnpremultiplyTable &unpremultiplyTable() {
  static const UnpremultiplyTable *table = [] {
    QImage every(256, 256, QImage::Format_RGBA8888_Premultiplied);
    for (int a = 0; a < 256; ++a) {
      uchar *row = every.scanLine(a);
      for (int v = 0; v < 256; ++v) {
        const uchar c = uchar(std::min(v, a));
        row[v * 4] = row[v * 4 + 1] = row[v * 4 + 2] = c;
        row[v * 4 + 3] = uchar(a);
      }
    }
    const QImage straight = every.convertToFormat(QImage::Format_RGBA8888);
    auto *made = new UnpremultiplyTable;
    for (int a = 0; a < 256; ++a)
      for (int v = 0; v < 256; ++v) made->value[a][v] = straight.constScanLine(a)[std::min(v, a) * 4];
    return made;
  }();
  return *table;
}
// A premultiplied 8-bit frame as the straight one everything downstream takes,
// converted where it lies. Opaque pixels -- most of any frame with something
// behind it -- and clear ones -- most of any frame without -- are the same
// either way and are passed over; the rest are looked up.
QImage straightFrame(QImage frame) {
  if (frame.format() != QImage::Format_RGBA8888_Premultiplied) return frame.convertToFormat(QImage::Format_RGBA8888);
  const auto &table = unpremultiplyTable();
  const int width = frame.width();
  auto convert = [&table](quint32 &pixel) {
    const quint32 a = pixel >> 24; // RGBA in memory: alpha is the top byte of the word
    if (a == 255 || pixel == 0) return;
    const uchar *to = table.value[a];
    pixel = (a << 24) | (quint32(to[(pixel >> 16) & 0xff]) << 16) | (quint32(to[(pixel >> 8) & 0xff]) << 8) |
            quint32(to[pixel & 0xff]);
  };
#if defined(_M_X64) || defined(__SSE2__)
  const __m128i alpha = _mm_set1_epi32(int(0xff000000u)), clear = _mm_setzero_si128();
  // Looked up one at a time, a frame that is mostly half-clear -- soft tiles,
  // a glow over nothing -- converts slower than Qt's own conversion does all of
  // it, new memory and all. One row in eight says which kind this is.
  qint64 groups = 0, busy = 0;
  for (int y = 0; y < frame.height(); y += 8) {
    const quint32 *row = reinterpret_cast<const quint32 *>(frame.constScanLine(y));
    for (int x = 0; x + 4 <= width; x += 4, ++groups) {
      const __m128i four = _mm_loadu_si128(reinterpret_cast<const __m128i *>(row + x));
      const __m128i unchanged = _mm_or_si128(_mm_cmpeq_epi32(_mm_and_si128(four, alpha), alpha), _mm_cmpeq_epi32(four, clear));
      if (_mm_movemask_epi8(unchanged) != 0xffff) ++busy;
    }
  }
  if (busy * 5 > groups) return frame.convertToFormat(QImage::Format_RGBA8888);
#endif
  for (int y = 0; y < frame.height(); ++y) {
    quint32 *row = reinterpret_cast<quint32 *>(frame.scanLine(y));
    int x = 0;
#if defined(_M_X64) || defined(__SSE2__)
    // Four at a time past the ones with nothing to change: one at a time, the
    // passing over alone took a millisecond of an HD frame.
    for (; x + 4 <= width; x += 4) {
      const __m128i four = _mm_loadu_si128(reinterpret_cast<const __m128i *>(row + x));
      const __m128i unchanged = _mm_or_si128(_mm_cmpeq_epi32(_mm_and_si128(four, alpha), alpha), _mm_cmpeq_epi32(four, clear));
      if (_mm_movemask_epi8(unchanged) == 0xffff) continue;
      for (int k = 0; k < 4; ++k) convert(row[x + k]);
    }
#endif
    for (; x < width; ++x) convert(row[x]);
  }
  if (!frame.reinterpretAsFormat(QImage::Format_RGBA8888)) return frame.convertToFormat(QImage::Format_RGBA8888);
  return frame;
}
}  // namespace

// Set on a worker while it draws one sample of a frame's motion blur, averaged
// from whole samples, beside the others (renderRegion).
namespace { thread_local bool g_sampleLane = false; }

static QImage sampleRegion(const Scene &s, double frame, double center,
                           QSize outputSize, QRect region) {
  // Painted premultiplied, which is the form the raster engine composites in.
  // Handing it a straight-alpha target makes it convert on every operation, and
  // the composition is nothing but operations. Converted back once at the end,
  // so what comes out is the format everything downstream expects.
  const auto painting = processingFormat(s.processingDepth, true);
  // A region far larger than a frame is drawn once and not held on to.
  if (qint64(region.width()) * region.height() > 64ll * 1024 * 1024) {
    QImage once(region.size(), painting);
    if (once.isNull()) return {};
    once.fill(Qt::transparent);
    QPainter loner(&once);
    loner.translate(-region.left(), -region.top());
    loner.setClipRect(region);
    paintComposition(loner, s, frame, center, outputSize);
    loner.end();
    return once.convertToFormat(processingFormat(s.processingDepth));
  }
  // Touching eight megabytes of new memory costs 1.4 ms in page faults, every
  // frame, because Windows hands large blocks back to the system when they are
  // freed and faults them in again on the next allocation. A whole 8-bit frame
  // is drawn into memory a finished frame gave back (pooledImage) and handed out
  // as the result itself, converted where it lies (straightFrame): a light frame
  // took twice as long before. A band, or a deeper frame, is drawn into a buffer
  // kept per thread and converted out of it -- twenty bands taking and giving
  // back from one pool at once, and converting a pixel at a time, cost a band-
  // drawn frame of six hundred shapes a millisecond more than that.
  //
  // A sample of a frame's motion blur drawn beside the others, on a worker
  // (renderRegion), is painted into the pool's memory as well: any of the
  // workers may draw one, and each of them keeping a frame of its own for good
  // would be twenty frames, a third of a gigabyte at 1080p and 16 bpc. At 8 bpc
  // it is handed out converted where it lies, as a whole frame is.
  const bool pooled = s.processingDepth <= 8 && (!inRenderWorker || g_sampleLane);
  const bool fromPool = pooled || g_sampleLane;
  // A frame drawn while this thread is still drawing another -- a layer an
  // effect takes as an input, rendered on its own (HostEffectRender::
  // otherLayer) -- gets a buffer of its own: the one kept per thread is being
  // painted by the frame it is drawn for.
  static thread_local QImage kept;
  static thread_local int sampling = 0;
  struct Sampling {
    Sampling() { ++sampling; }
    ~Sampling() { --sampling; }
  } inside;
  QImage nested;
  QImage &scratch = sampling > 1 ? nested : kept;
  QImage drawn;
  if (fromPool) drawn = pooledImage(region.size(), painting);
  else if (scratch.size() != region.size() || scratch.format() != painting) scratch = QImage(region.size(), painting);
  QImage &img = fromPool ? drawn : scratch;
  if (img.isNull()) return {};
  img.fill(Qt::transparent);
  QPainter painter(&img);
  painter.translate(-region.left(), -region.top());
  // Stated rather than left implicit: layers read this back to skip work that
  // falls outside it, and rasterizing offscreen effects to it is the whole
  // reason a zoomed-in preview costs a viewport instead of a magnified frame.
  painter.setClipRect(region);
  // A cold frame with many layers builds each layer's effects one after
  // another here, and every one of those is a small raster: too little work for
  // the passes' own row splitting to pay for its scheduling, so most of the
  // cores idle. On the fourth miss, build all of them at once instead, one
  // whole layer per worker, each painted alone with the same painter setup so
  // its raster lands in the cache under the key this pass is about to look for.
  // A worker runs its own passes inline, so nothing here nests.
  LayerEffectHook warmLayers;
  int misses = 0;
  bool warmed = false;
  const bool canWarm = !inRenderWorker && s.layers.size() >= 6 && renderWorkerLimit() > 1;
  if (canWarm)
    warmLayers = [&](const Layer &current, double, QRectF, double, QRectF, const QImage &, bool) {
      if (!warmed && ++misses >= 4) {
        warmed = true;
        renderParallelEach(int(s.layers.size()), [&](int index) {
          struct NoWait { NoWait() { setPrewarmWorker(true); } ~NoWait() { setPrewarmWorker(false); } } noWait;
          static thread_local QImage target;
          if (target.size() != region.size() || target.format() != painting) target = QImage(region.size(), painting);
          if (target.isNull()) return;
          const Layer &layer = s.layers[size_t(index)];
          // The layer that triggered this is already in hand and will be
          // finished by the pass that called us.
          if (!layer.visible || layer.id == current.id) return;
          QPainter alone(&target);
          alone.translate(-region.left(), -region.top());
          alone.setClipRect(region);
          paintComposition(alone, s, frame, center, outputSize, nullptr, nullptr, layer.id);
        }, warmWorkerCap(s, current, outputSize));
      }
      return LayerEffectResult{};
    };
  paintComposition(painter, s, frame, center, outputSize, nullptr, canWarm ? &warmLayers : nullptr);
  painter.end();
  return pooled ? straightFrame(std::move(drawn)) : img.convertToFormat(processingFormat(s.processingDepth));
}

// A cold frame of many layers, split into bands, used to build its rasters one
// at a time: every band reaches the first layer together, one builds it while
// the rest wait on its ticket, then they all move on to the next. Whole layers
// are the unit that parallelizes, so when a few sampled layers turn out not to
// be cached yet, build every layer's raster at once, one whole layer per
// worker, each painted alone under the painter setup the bands will use. A
// frame that is already warm pays for the three sampled layers and no more.
//
// A layer whose letters are moving blurs itself by drawing several samples, and
// every sample is a raster of its own, built one after another as the frame is
// composed. They do not depend on each other, so they are built here at the same
// moment, each at the time it will be asked for. That is known without sampling
// anything, so such a layer starts the warming by itself, even in a scene of
// two layers, and in a frame that is not split into bands.
static void warmColdLayers(const Scene &s, double frame, QSize outputSize, QRect region,
                           bool perLayerBlur, bool probeStaticLayers, const LayerEffectHook *effects = nullptr) {
  if (renderWorkerLimit() <= 1 || inRenderWorker) return;
  const auto painting = processingFormat(s.processingDepth, true);
  const bool animated = sceneHasKeyedMotion(s);
  struct Item { int index; double time; };
  // Only what this frame draws: a layer before its in-point or past its out-point
  // is passed over by the frame, and by the pass that draws it alone. Asked
  // about anyway, a music video of 1,600 layers worked out how far the letters
  // of 880 titles not yet on screen would move -- 34 ms of every frame -- and
  // then drew each of them alone, to find nothing to draw.
  std::vector<int> drawn;
  for (size_t i = 0; i < s.layers.size(); ++i) {
    const Layer &layer = s.layers[i];
    if (!layer.visible || frame - layer.start < 0 || frame - layer.start >= layer.duration) continue;
    if (layer.type != "camera" && layer.type != "light" && layer.type != "audio" &&
        layer.type != "effect") // nothing of its own to warm
      drawn.push_back(int(i));
  }
  std::vector<Item> movingLetters, others, letterSamples;
  for (int index : drawn) {
    const Layer &layer = s.layers[size_t(index)];
    bool lettersMove = false;
    int samples = 0;
    if (layer.characterAnimation.value("enabled").toBool()) {
      const double local = std::clamp(frame - layer.start, 0., std::max(0., layer.duration - 1e-6));
      if (perLayerBlur && layer.motionBlur && layer.type != "group")
        samples = layerBlurSampleCount(layer, s, local, frame, outputSize, &lettersMove);
      else lettersMove = !characterPoseAtRest(layer, local);
    }
    // A layer whose letters are moving builds a raster for each sample of its own
    // blur, at the sample's own time. Left to the bands, the first band to
    // reach it built it on one core -- work inside a band runs inline -- while
    // every other band waited for it: a shadowed title cost 85 ms of a 140 ms
    // frame that way. Built here first, at exactly the times its blur will ask
    // for, and the bands find them ready: a big one alone, spreading its own
    // passes over the cores (built beside each other they were no faster),
    // small ones side by side.
    if (lettersMove) {
      if (samples > 0)
        for (int k = 0; k < samples; ++k) letterSamples.push_back({index, frame + ((k + .5) / samples - .5) * .5});
      else
        letterSamples.push_back({index, frame});
      continue;
    }
    others.push_back({index, frame});
  }
  auto paintAlone = [&](QImage &target, const Item &item) {
    QPainter alone(&target);
    alone.translate(-region.left(), -region.top());
    alone.setClipRect(region);
    paintComposition(alone, s, item.time, frame, outputSize, nullptr, effects, s.layers[size_t(item.index)].id);
  };
  struct Hint {
    int previous;
    explicit Hint(bool on) : previous(g_animatedSceneHint) { g_animatedSceneHint = on ? 1 : 0; }
    ~Hint() { g_animatedSceneHint = previous; }
  };
  // A layer built alone spreads its build over every core -- its effects'
  // passes, its strips -- and is done about two and a half times sooner than
  // on one; layers built side by side, one to a core, each take one core's
  // time but all at once. A page of lyrics is many small ones, and one after
  // another they left the rest idle: seventeen moving lines, 34 ms of a frame
  // on the CPU, 10 ms of one on the card; and the zooming discs of a chorus,
  // each with its glow, built in turn to find out whether the frame was cold,
  // 25 ms more. But a background of moving noise built on one core beside
  // them held the whole frame up for 17 ms, against 6 alone. So the dearest
  // are taken out to be built alone, one at a time, for as long as that
  // brings the end sooner; or all of them alone, where that is sooner still
  // (two or three alike). What each costs is what it took the last time it
  // was built (frames follow one another closely), or, the first time, its
  // area on the frame: area alone misjudged a disc with Light Rays, three
  // times the work of one the same size with a glow. A layer whose size
  // cannot be told beforehand is built alone.
  const QTransform toOutput = QTransform::fromScale(double(outputSize.width()) / std::max(1, s.width),
                                                    double(outputSize.height()) / std::max(1, s.height));
  auto areaOf = [&](const Layer &layer, double local) -> double {
    QRect area;
    if (layerDeviceRect(layer, s, local, toOutput, outputSize, area)) return double(area.width()) * area.height();
    // An effect on moving letters -- a glow -- leaves where its pixels land to
    // the effect, which is all the frame's planning wants to know; how big the
    // letters are is still known. Their effects' room is not, so they count
    // four times their size.
    const QTransform world = layerWorldTransform(layer, s, local + layer.start);
    if (!world.isAffine()) return -1;
    QRectF bounds = animatedTextBounds(layer, local, s.fps);
    if (bounds.isNull()) return -1;
    if (layerHasMainTransform(layer)) bounds = layerMainTransform(layer).mapRect(bounds);
    const QRectF device = (world * toOutput).mapRect(bounds);
    return device.width() * device.height() * 4;
  };
  // One core's milliseconds for each layer, the last time it was built.
  static QMutex costGuard;
  static QHash<QString, double> lastCost;
  constexpr double alone = 2.5;
  auto remember = [&](const Layer &layer, double milliseconds) {
    QMutexLocker lock(&costGuard);
    if (lastCost.size() > 4096) lastCost.clear();
    auto found = lastCost.find(layer.id);
    if (found == lastCost.end()) lastCost.insert(layer.id, milliseconds);
    else *found = *found * .5 + milliseconds * .5;
  };
  auto build = [&](const std::vector<Item> &items) {
    const double cores = std::max(1, renderWorkerLimit());
    std::vector<double> costs(items.size(), -1), areas(items.size(), -1);
    double perPixel = 0, measuredPixels = 0;
    {
      QMutexLocker lock(&costGuard);
      for (size_t i = 0; i < items.size(); ++i) {
        const Layer &layer = s.layers[size_t(items[i].index)];
        const double local = std::clamp(items[i].time - layer.start, 0., std::max(0., layer.duration - 1e-6));
        areas[i] = areaOf(layer, local);
        const auto found = lastCost.constFind(layer.id);
        if (found == lastCost.constEnd()) continue;
        costs[i] = *found;
        if (areas[i] > 0) perPixel += *found, measuredPixels += areas[i];
      }
    }
    // Not built before: by its area, at what the others cost a pixel (25 ns
    // on one core when none is known).
    perPixel = measuredPixels > 0 ? perPixel / measuredPixels : 25e-6;
    std::vector<std::pair<double, size_t>> sized;
    std::vector<Item> big, small;
    double rest = 0;
    for (size_t i = 0; i < items.size(); ++i) {
      const double cost = costs[i] >= 0 ? costs[i] : areas[i] >= 0 ? areas[i] * perPixel : -1;
      if (cost < 0) big.push_back(items[i]);
      else sized.push_back({cost, i}), rest += cost;
    }
    std::sort(sized.begin(), sized.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    const double allAlone = rest / alone;
    double before = 0;
    size_t taken = 0;
    for (; taken < sized.size(); ++taken) {
      const double cost = sized[taken].first;
      const double next = taken + 1 < sized.size() ? sized[taken + 1].first : 0;
      const double keep = before + std::max(cost, rest / cores);
      const double take = before + cost / alone + std::max(next, (rest - cost) / cores);
      if (take >= keep) break;
      before += cost / alone;
      rest -= cost;
    }
    const double mixed = before + (taken < sized.size() ? std::max(sized[taken].first, rest / cores) : 0);
    if (allAlone < mixed) taken = sized.size();
    for (size_t k = 0; k < sized.size(); ++k) (k < taken ? big : small).push_back(items[sized[k].second]);
    if (!big.empty()) {
      QImage target(region.width(), 1, painting);
      if (!target.isNull()) {
        Hint hint(animated);
        for (const Item &item : big) {
          QElapsedTimer clock;
          clock.start();
          paintAlone(target, item);
          remember(s.layers[size_t(item.index)], clock.nsecsElapsed() / 1e6 * alone);
        }
      }
    }
    int cap = 1000;
    for (size_t i = 0; i < small.size() && i < 3; ++i)
      cap = std::min(cap, warmWorkerCap(s, s.layers[size_t(small[i].index)], outputSize));
    renderParallelEach(int(small.size()), [&](int n) {
      struct NoWait { NoWait() { setPrewarmWorker(true); } ~NoWait() { setPrewarmWorker(false); } } noWait;
      static thread_local QImage target;
      if (target.width() != region.width() || target.height() != 1 || target.format() != painting)
        target = QImage(region.width(), 1, painting);
      if (target.isNull()) return;
      Hint hint(animated);
      QElapsedTimer clock;
      clock.start();
      paintAlone(target, small[size_t(n)]);
      remember(s.layers[size_t(small[size_t(n)].index)], clock.nsecsElapsed() / 1e6);
    }, cap);
  };
  build(letterSamples);
  std::vector<Item> work = movingLetters;
  int cap = 1000;
  for (size_t i = 0; i < movingLetters.size() && i < 3; ++i)
    cap = std::min(cap, warmWorkerCap(s, s.layers[size_t(movingLetters[i].index)], outputSize));
  std::vector<int> probes;
  if (movingLetters.empty()) {
    // Nothing moving: only a frame with a few layers not yet cached is worth it.
    if (!probeStaticLayers || drawn.size() < 6) return;
    probes = {drawn.front(), drawn[drawn.size() / 2], drawn.back()};
    // And every layer whose size is keyed and that carries a halo: as a zoom
    // passes from one way of drawing it to another, all of them want a new
    // picture on the same frame -- nine discs in a dive, each built in a band on
    // one core while the other bands waited, 120 ms of one frame -- and the
    // three layers sampled above, from the top, middle and bottom of the stack,
    // knew nothing of it.
    for (int index : drawn) {
      if (probes.size() >= 16) break;
      const Layer &l = s.layers[size_t(index)];
      if ((l.shadow || l.glow || l.blur) && layerScaleAnimated(l, s) &&
          std::find(probes.begin(), probes.end(), index) == probes.end())
        probes.push_back(index);
    }
    const auto before = effectedRasterBuilds();
    std::vector<Item> probed;
    for (int index : probes) probed.push_back({index, frame});
    build(probed);
    if (effectedRasterBuilds() == before) return;
    for (int index : probes) cap = std::min(cap, warmWorkerCap(s, s.layers[size_t(index)], outputSize));
  }
  for (const Item &item : others)
    if (std::find(probes.begin(), probes.end(), item.index) == probes.end()) work.push_back(item);
  if (work.size() < 2) return;
  renderParallelEach(int(work.size()), [&](int n) {
    struct NoWait { NoWait() { setPrewarmWorker(true); } ~NoWait() { setPrewarmWorker(false); } } noWait;
    static thread_local QImage target;
    if (target.width() != region.width() || target.height() != 1 || target.format() != painting)
      target = QImage(region.width(), 1, painting);
    if (target.isNull()) return;
    Hint hint(animated);
    paintAlone(target, work[size_t(n)]);
  }, cap);
}
void warmFrameLayers(const Scene &s, double frame, QSize outputSize, QRect region, bool perLayerBlur,
                     const LayerEffectHook *effects) {
  warmColdLayers(s, frame, outputSize, region, perLayerBlur, false, effects);
}
// How many motion-blur samples of a frame averaged from whole samples are drawn
// at once, one to a worker (renderRegion). A sample being drawn holds about four
// pictures of its own -- the one it paints, an Effect Layer's copy of what lies
// beneath and what its effects make of that, the straight copy it hands over (at
// 8 bpc a group's or a matte's picture instead, the straight one being the one
// it painted) -- so 16 bytes a pixel at 8 bpc, 32 at 16 and 64 at 32. Frames are
// drawn several at a time (two while the preview plays, half the cores' worth
// while a film is written), so the lanes of all of them together are held to
// 384 MB: up to twenty-four samples at once at 960x540 and 16 bpc, six at
// 1080p, and at 4K one after another as before; at 8 bpc twelve at 1080p and
// three at 4K. One after another also inside a worker, where nothing nests; for
// a sliver of a frame; and on the interface thread, which reaches the card its
// own way, so a sample there could come out of the card where on a worker it
// would come out of the processor.
namespace {
std::atomic<qint64> g_sampleLaneBytes{0};
struct SampleLanes {
  int count = 1;
  qint64 held = 0;
  SampleLanes(const Scene &s, QRect region, int samples) {
    if (inRenderWorker || samples < 2 || renderWorkerLimit() <= 1) return;
    const qint64 pixels = qint64(region.width()) * region.height();
    if (pixels < 65536) return;
    if (const QCoreApplication *app = QCoreApplication::instance(); !app || QThread::currentThread() == app->thread()) return;
    const qint64 each = pixels * (s.processingDepth > 16 ? 64 : s.processingDepth > 8 ? 32 : 16);
    constexpr qint64 budget = 384ll << 20;
    const int wanted = std::min(renderWorkerLimit(), samples);
    qint64 used = g_sampleLaneBytes.load();
    int lanes = 1;
    do {
      lanes = int(std::clamp<qint64>((budget - used) / each, 0, wanted));
      if (lanes < 2) return;
    } while (!g_sampleLaneBytes.compare_exchange_weak(used, used + lanes * each));
    count = lanes;
    held = lanes * each;
  }
  ~SampleLanes() {
    if (held) g_sampleLaneBytes -= held;
  }
  SampleLanes(const SampleLanes &) = delete;
  SampleLanes &operator=(const SampleLanes &) = delete;
};
}  // namespace
// One motion-blur sample of a deep frame, straight as sampleRegion hands it
// over, added into `sum` at its share of the shutter. Widened to premultiplied
// float a strip at a time -- Qt converts pixel by pixel, so a strip comes out as
// those rows of the whole picture would -- and every value divided by the number
// of samples and added, four at a time: the same division and the same
// addition, rounded the same way, as one value at a time. The strips are spread
// over the cores. Done over the whole picture on one core, through a fresh float
// copy of all of it, adding up the eight samples of a 16 bpc frame of the
// Twitter promo took 33 ms of its 158.
static void addSampleShare(QImage &sum, const QImage &sample, int samples) {
  if (sample.isNull() || sample.size() != sum.size()) return;
  // Row addresses taken once, here: bits() and scanLine() detach, and two
  // workers detaching one picture race on its reference count.
  uchar *const total = sum.bits();
  const uchar *const source = sample.constBits();
  const qsizetype totalStride = sum.bytesPerLine(), sourceStride = sample.bytesPerLine();
  const int width = sum.width(), height = sum.height();
  const QImage::Format format = sample.format();
  const float divisor = float(samples);
  constexpr int strip = 16;
  renderParallelEach((height + strip - 1) / strip, [&](int n) {
    const int top = n * strip, rows = std::min(strip, height - top);
    const QImage floats = QImage(source + sourceStride * top, width, rows, sourceStride, format)
                              .convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
    if (floats.isNull()) return;
    const int values = width * 4;
    for (int y = 0; y < rows; ++y) {
      float *out = reinterpret_cast<float *>(total + totalStride * (top + y));
      const float *in = reinterpret_cast<const float *>(floats.constScanLine(y));
      int x = 0;
#if defined(_M_X64) || defined(__SSE2__)
      const __m128 by = _mm_set1_ps(divisor);
      for (; x + 4 <= values; x += 4)
        _mm_storeu_ps(out + x, _mm_add_ps(_mm_loadu_ps(out + x), _mm_div_ps(_mm_loadu_ps(in + x), by)));
#endif
      for (; x < values; ++x) out[x] += in[x] / divisor;
    }
  });
}
// One motion-blur sample of an 8-bit frame, straight as sampleRegion hands it
// over, premultiplied again a strip at a time -- Qt converts pixel by pixel, so
// a strip comes out as those rows of the whole picture would -- and added into
// the sixteen-bit sums sixteen bytes at a time (addBytes); the first sample
// added sets them. Sums of whole numbers come out the same in whatever order
// they are made, and the strips are spread over the cores.
static bool addSampleBytes(QImage &sums, const QImage &sample, bool first) {
  if (sample.isNull() || sample.size() != sums.size()) return false;
  // Row addresses taken once, here: bits() and scanLine() detach, and two
  // workers detaching one picture race on its reference count.
  uchar *const total = sums.bits();
  const uchar *const source = sample.constBits();
  const qsizetype totalStride = sums.bytesPerLine(), sourceStride = sample.bytesPerLine();
  const int width = sums.width(), height = sums.height();
  const QImage::Format format = sample.format();
  const qsizetype values = qsizetype(width) * 4;
  constexpr int strip = 16;
  renderParallelEach((height + strip - 1) / strip, [&](int n) {
    const int top = n * strip, rows = std::min(strip, height - top);
    const QImage premultiplied = QImage(source + sourceStride * top, width, rows, sourceStride, format)
                                     .convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < rows; ++y) {
      auto *row = reinterpret_cast<quint16 *>(total + totalStride * (top + y));
      if (first) std::memset(row, 0, size_t(values) * sizeof(quint16));
      if (!premultiplied.isNull()) addBytes(row, premultiplied.constScanLine(y), values);
    }
  });
  return true;
}
// The samples of a frame whose motion blur is averaged from whole frames, each
// handed to `add` in its order. Each is taken at the centre of its slice of the
// shutter: end-point sampling gave the first and last ghost a full sample of
// weight and made fast motion look like a row of separate copies.
//
// Every sample is a whole frame of its own, drawn just as it is drawn by
// itself, so several are drawn at once, each whole on a worker, and handed over
// in their order: the same pictures, the same additions in the same order, the
// same frame to the bit. One after another, the eight samples of a 16 bpc frame
// of the Twitter promo took 117 of its 158 ms: only the Effect Layer's grain
// and vignette were spread over the cores, and everything else in a sample was
// drawn on one. Side by side, with the sum in strips, it takes 65 ms against
// 182 (cold 93 against 223), and a frame of the first Kotonoha video 57
// against 183.
template <class Add>
static void drawMotionSamples(const Scene &s, double frame, QSize outputSize, QRect region, int motionSamples,
                              const Add &add) {
  const auto at = [&](int sample) { return frame + ((sample + .5) / motionSamples - .5) * .5; };
  const SampleLanes lanes(s, region, motionSamples);
  if (lanes.count <= 1) {
    for (int sample = 0; sample < motionSamples; ++sample) add(sampleRegion(s, at(sample), frame, outputSize, region));
    return;
  }
  // The workers reach the card as this thread does (it is not the
  // interface thread), and are kept off it where this thread is.
  const bool card = canUseGpuPixels();
  std::vector<std::future<QImage>> drawn(static_cast<size_t>(motionSamples));
  int started = 0;
  const auto start = [&] {
    const double when = at(started);
    drawn[size_t(started)] = renderJob([&s, when, frame, outputSize, region, card] {
      std::optional<CpuPixelScope> processor;
      if (!card) processor.emplace();
      struct Lane {
        bool previous;
        Lane() : previous(g_sampleLane) { g_sampleLane = true; }
        ~Lane() { g_sampleLane = previous; }
      } lane;
      return sampleRegion(s, when, frame, outputSize, region);
    });
    ++started;
  };
  while (started < lanes.count) start();
  // Each one waited for in its turn, the next started as it comes in, and
  // every one started waited for even after a failure: they read the scene.
  std::exception_ptr failure;
  for (int sample = 0; sample < started; ++sample) {
    try {
      const QImage img = drawn[size_t(sample)].get();
      if (failure) continue;
      if (started < motionSamples) start();
      add(img);
    } catch (...) {
      if (!failure) failure = std::current_exception();
    }
  }
  if (failure) std::rethrow_exception(failure);
}
static QImage renderRegion(const Scene &s, double frame, bool motionBlur,
                           QSize outputSize, QRect region, int motionSamples = 8) {
  if (!motionBlur)
    return sampleRegion(s, frame, frame, outputSize, region);
  if (motionBlurPerLayerEligible(s, frame)) {
    // The frame is drawn once; each moving layer blurs itself inside it.
    struct Scope {
      bool previous;
      Scope() : previous(g_layerMotionBlur) { g_layerMotionBlur = true; }
      ~Scope() { g_layerMotionBlur = previous; }
    } scope;
    return sampleRegion(s, frame, frame, outputSize, region);
  }
  motionSamples = std::clamp(motionSamples, 8, 48);

  if (s.processingDepth > 8) {
    QImage sum(region.size(), QImage::Format_RGBA32FPx4_Premultiplied);
    sum.fill(Qt::transparent);
    drawMotionSamples(s, frame, outputSize, region, motionSamples,
                      [&](const QImage &sample) { addSampleShare(sum, sample, motionSamples); });
    return sum.convertToFormat(processingFormat(s.processingDepth));
  }

  // At 8 bpc -- a group, a grouped layer or a track matte in the frame -- the
  // samples are premultiplied and added up byte by byte in sixteen bits (48
  // samples of 255 come to 12 240), and divided once at the end, rounded to
  // the nearest: averageBytes, whose multiply and shift give (sum + n / 2) / n.
  // The sums and the frame are kept in memory a frame before gave back, as a
  // whole frame is; a band takes its own. Drawn one after another and added up
  // a byte at a time, a frame of the Twitter promo with its title in a matte
  // took 140 ms at 960x540; the sum in strips alone takes 26 off that, and with
  // the samples side by side it takes 52 -- one of the first Kotonoha video
  // with its bokeh in a group 42 against 131.
  const auto picture = [&](QImage::Format format) {
    return inRenderWorker ? QImage(region.size(), format) : pooledImage(region.size(), format);
  };
  QImage sums = picture(QImage::Format_RGBA64);
  QImage out = picture(QImage::Format_RGBA8888_Premultiplied);
  if (sums.isNull() || out.isNull()) return {};
  int added = 0;
  drawMotionSamples(s, frame, outputSize, region, motionSamples, [&](const QImage &sample) {
    if (addSampleBytes(sums, sample, added == 0)) ++added;
  });
  if (!added) sums.fill(Qt::transparent);
  const uchar *const total = sums.constBits();
  uchar *const mean = out.bits();
  const qsizetype totalStride = sums.bytesPerLine(), meanStride = out.bytesPerLine();
  const qsizetype values = qsizetype(region.width()) * 4;
  const int height = region.height();
  constexpr int strip = 16;
  renderParallelEach((height + strip - 1) / strip, [&](int n) {
    for (int y = n * strip; y < std::min(height, (n + 1) * strip); ++y)
      averageBytes(mean + meanStride * y, reinterpret_cast<const quint16 *>(total + totalStride * y), values,
                   unsigned(motionSamples));
  });
  return straightFrame(std::move(out));
}

bool compositionHasMotion(const Scene &s, double frame) {
  ExpressionSceneScope expressions(&s);
  bool needsSamples=false;
  for(const auto &layer:s.layers) {
    if(!layer.visible || !layer.motionBlur || frame<layer.start-.25 || frame>layer.start+layer.duration+.25)continue;
    needsSamples=true;
    if(std::abs(frame-layer.start)<=.25 || std::abs(frame-layer.start-layer.duration)<=.25)return true;
    if(layer.characterAnimation["enabled"].toBool() && !layer.hiddenEffects["Text-Animation"].toBool()) {
      const double local=frame-layer.start;
      if(!(characterPoseAtRest(layer,local-.6) && characterPoseAtRest(layer,local) && characterPoseAtRest(layer,local+.6)))return true;
    }
  }
  if(!needsSamples)return false;
  for (const auto &layer : s.layers) {
    // Parent animation can affect a visible child, so include keyed parents too.
    auto variesDuringShutter = [&](const Property &property) {
      if (!property.varies()) return false;
      const double first=property.at(frame-layer.start-.25);
      for(int sample=1;sample<8;++sample)
        if(property.at(frame-layer.start+(sample/7.-.5)*.5)!=first)return true;
      return false;
    };
    for (const auto &property : layer.p) if (variesDuringShutter(property)) return true;
    for (const auto &property : layer.extraCurves) if (variesDuringShutter(property)) return true;
  }
  return false;
}
bool sameCompositionPixels(const Scene &s,double from,double to) {
  ExpressionSceneScope expressions(&s);
  if(from<0)return false;
  if(from==to)return true;
  for(const auto &l:s.layers) {
    // Include keyed parents/cameras, even if they do not draw themselves.
    auto unchanged = [&](const Property &p) {
      if(!p.varies())return true;
      if(p.at(from-l.start)!=p.at(to-l.start))return false;
      // Motion blur samples the shutter interval, not only the frame centre.
      if(s.motionBlur)for(int i=0;i<8;++i) {
        double offset=(i/7.-.5)*.5;
        if(p.at(from-l.start+offset)!=p.at(to-l.start+offset))return false;
      }
      return true;
    };
    for(const auto &p:l.p)if(!unchanged(p))return false;
    // Plugin parameters live beyond the fixed transform property array. Their
    // animation changes pixels too, including the samples inside a shutter.
    for(const auto &p:l.extraCurves)if(!unchanged(p))return false;
    if(!l.visible)continue;
    if(l.type=="video")return false;
    if(layerHasAnimatedEffect(l))return false;
    if(l.characterAnimation["enabled"].toBool() && !l.hiddenEffects["Text-Animation"].toBool()) {
      // Settled at both ends, and through the shutter around each: nothing moves.
      for(double f:{from,to})for(double o:{-.6,0.,.6})
        if(!characterPoseAtRest(l,f-l.start+o))return false;
    }
    auto active=[&](double f){return f>=l.start && f<l.start+l.duration;};
    if(active(from)!=active(to))return false;
    if(s.motionBlur && l.motionBlur)
      for(double f:{from,to})
        if(std::abs(f-l.start)<=.5 || std::abs(f-l.start-l.duration)<=.5)return false;
  }
  return true;
}
// Whether one layer, drawn in bands, would repeat work or come out differently
// -- the reasons follow. compositionBands asks it of every layer a frame draws;
// renderCPU asks it to find the few layers to draw whole between banded runs.
//
// Spatial effects (blur, glow, shadow, stroke) read a padded neighbourhood.
// When the layer's raster is cached whole every band shares it and no work
// repeats; when it is too large each band recomputes the padding, which
// measured forty seconds against 130 ms on three hundred glowing layers.
//
// Pointwise effects (gradient, tint, fill) touch each pixel once with no
// spatial dependency, so banding never repeats their work.
//
// A small vector layer's raster is cached whole, every band looks up the
// same one and blits its slice, and the work divides properly: a hundred
// plain text layers went from 6.9 ms to 4.5.
static bool layerBlocksBands(const Layer &layer, const Scene &s, QSize outputSize, double frame) {
  // Particles in depth are points drawn straight onto whatever band holds
  // them. Cut out and drawn whole on their own, they would lose the 3D layers
  // they are sorted among.
  if (particleVolume(layer)) return false;
  // An Effect Layer draws nothing of its own. Where it acts, the frame is drawn
  // in parts around it (sceneUsesEffectLayers); where it rests it lets the
  // picture through untouched. Taken for a raster as wide as the frame, one
  // carrying a zoom that only hits on the beat kept whole choruses on one core.
  if (layer.type == "effect") return false;
  const double compositionScale = double(outputSize.width()) / std::max(1, s.width);
  // Eighty per cent of the real cacheWhole threshold, so rounding cannot push
  // an estimate below it while the real raster lands above it.
  const double bandableArea = 4194304. * .8;
  // Depth of Field adds to a layer's blur without touching layer.blur
  // itself, so a layer this heuristic would otherwise call band-safe still
  // needs to account for it here or a band seam can cut through the blur.
  const double dofBlur = layerDepthOfFieldBlur(layer, s, frame);
  const bool hasSpatialEffect =
      layer.blur != 0 || layer.glow != 0 || layer.shadow != 0 || dofBlur > 0 || rgbSplitReach(layer) > 0 ||
      (layer.appliedEffects["Stroke"].toBool() && !layer.hiddenEffects["Stroke"].toBool() &&
       !vectorEffectStroke(layer));
  const bool vector = layer.type == "text" || layer.type == "fill" ||
                      layer.type == "shape" || parametricShape(layer);
  // A wide feather is one matte for the whole layer: its reach is a couple of
  // feather widths whichever band asks, so banding cannot divide that work,
  // only make every band wait for the one that builds it. Drawn as a whole,
  // the pixel passes inside it use every core instead.
  if (hasFeatheredMask(layer)) {
    double widest = 0;
    for (const auto &value : layer.masks) {
      const auto mask = value.toObject();
      if (mask["enabled"].toBool(true)) widest = std::max(widest, mask["feather"].toDouble());
    }
    if (widest * compositionScale >= 32.) return true;
  }
  // Shapes stay as vector paths unless a spatial effect forces a raster.
  if ((layer.type == "shape" || parametricShape(layer)) && !hasSpatialEffect)
    return false;
  // So does 3D text, or a 3D fill, with nothing that needs a raster: through
  // the camera it is drawn from its vectors, each band drawing where it falls
  // (its extruded sides too), like a shape. Refused before, a single 3D title
  // kept the whole frame on one core.
  if (layer.threeD && !hasSpatialEffect && layer.appliedEffects.isEmpty() && layer.surfaceEffects.isEmpty() &&
      !layerHasPixelEffect(layer) && layerTextStrokes(layer).empty() && !hasFeatheredMask(layer))
    return false;
  // Raster images without spatial effects are just blitted -- safe to band.
  if (!vector && !hasSpatialEffect) return false;
  // A title flying in measured by everywhere its letters could ever go -- six
  // hundred pixels round a word turned seventy degrees -- was taken for a
  // frame-sized shadowed layer, cut the frame there and drew on one core: 50 ms
  // of an end card. Where its letters are now is what lands.
  QRectF bounds = movingLettersBounds(layer, s, frame - layer.start);
  if (bounds.isNull()) {
    bounds = layerBounds(layer, s);
    // Out to the tips of the strokes' Miter corners.
    if (const QRectF corners = mitredTextStrokeBox(layer); !corners.isNull()) bounds = bounds.united(corners);
  }
  if (layerHasMainTransform(layer)) bounds = layerMainTransform(layer).mapRect(bounds);
  bounds = repeatedLayerBounds(layer, bounds);
  const QTransform world = layerWorldTransform(layer, s, frame);
  // Include the spatial-effect padding so the estimate matches the
  // cacheWhole decision in paintComposition.
  // The strokes built round a text's letters reach out with them: its
  // Character strokes and a Stroke effect drawn into them. Left out, a text
  // with a wide Stroke effect was taken for a small layer and the frame cut
  // into bands -- each one building the same distance field again over most of
  // the letters: 600 ms a frame at the effect's widest.
  double pad = layer.strokeWidth + strokeStackOutward(textStrokeStack(layer)) + 20;
  if (hasSpatialEffect)
    pad += (layer.blur + layer.glow + layer.shadow + dofBlur) * 1.5 + strokeOutwardReach(layer) + rgbSplitReach(layer);
  double area = 0;
  if (world.isAffine()) {
    const double areaScale = std::abs(world.m11() * world.m22() - world.m12() * world.m21()) *
                             compositionScale * compositionScale;
    area = (bounds.width() + 2 * pad) * (bounds.height() + 2 * pad) * areaScale;
  } else {
    // A layer seen through the camera: its raster is still built flat, once,
    // and each band draws its own slice of it through the projection, which
    // measured identical to drawing it whole. What decides is how large it
    // lands, from its four corners -- and a corner behind the camera has no
    // place on screen to measure.
    //
    // Not while its letters are moving: each one is then projected and drawn on
    // its own, and every band did all of them again -- a title being written
    // out in 3D took twice as long in twenty bands as on one core.
    if (layer.type == "text" && layer.characterAnimation.value("enabled").toBool() &&
        !layer.hiddenEffects.value("Text-Animation").toBool() && !characterPoseAtRest(layer, frame - layer.start))
      return true;
    const QRectF padded = bounds.adjusted(-pad, -pad, pad, pad);
    QPolygonF landed;
    for (const QPointF corner : {padded.topLeft(), padded.topRight(), padded.bottomRight(), padded.bottomLeft()}) {
      const double w = world.m13() * corner.x() + world.m23() * corner.y() + world.m33();
      if (!(w > 1e-6)) return true;
      landed << world.map(corner);
    }
    const QRectF box = landed.boundingRect();
    area = box.width() * box.height() * compositionScale * compositionScale;
  }
  if (!std::isfinite(area) || area > bandableArea) {
    if (hasSpatialEffect) {
      // The padding a spatial effect adds to each band. When it is small
      // the redundant overlap between bands is cheap and multicore still
      // wins; only heavy blurs make banding worse than serial.
      const double effectPad = (layer.blur + layer.glow + layer.shadow + dofBlur) * 1.5
                               + strokeOutwardReach(layer);
      const double bandHeight = double(outputSize.height())
                                / std::max(1, QThread::idealThreadCount());
      // A zooming layer whose one halo is kept whole (steadyHaloScale) is built
      // once and every band draws its own rows of it, like any cached raster:
      // nine shadowed discs in a dive kept the frame on one core, 290 ms.
      const bool oneHalo = !layer.blur && !dofBlur && !rgbSplitReach(layer) && (layer.glow != 0) != (layer.shadow != 0) &&
                           !hasFeatheredMask(layer) && !layerHasPixelEffect(layer) &&
                           !(layer.appliedEffects["Stroke"].toBool() && !layer.hiddenEffects["Stroke"].toBool()) &&
                           !(layer.appliedEffects["Gradient"].toBool() && !layer.hiddenEffects["Gradient"].toBool()) &&
                           !(layer.appliedEffects["Fill"].toBool() && !layer.hiddenEffects["Fill"].toBool()) &&
                           !(layer.appliedEffects["Tint"].toBool() && !layer.hiddenEffects["Tint"].toBool());
      if (oneHalo && world.isAffine() && layerScaleAnimated(layer, s)) {
        const double radius = layer.glow ? layer.glow : layer.shadow;
        const double haloPad = (layer.blur + layer.glow + layer.shadow) * 1.5 + 12;
        const QRectF whole = bounds.adjusted(-haloPad, -haloPad, haloPad, haloPad);
        const double steady = steadyHaloScale(radius, whole);
        // The scale paintComposition would otherwise resolve the halo at.
        const double onScreen = std::sqrt(std::abs(world.m11() * world.m22() - world.m12() * world.m21())) * compositionScale;
        const QSize full(std::max(1, qCeil(whole.width() * onScreen)), std::max(1, qCeil(whole.height() * onScreen)));
        if (steady > 0 && steady < onScreen / blurReduction(radius * onScreen, full)) return false;
      }
      if (effectPad > bandHeight * .25) return true;
    }
    // A large raster-backed vector layer is clipped per band and redrawn.
    if (!hasSpatialEffect && vector && layer.type != "shape" && !parametricShape(layer))
      return true;
  }
  return false;
}
// The layers used as someone's track matte: drawn even when hidden.
static QSet<QString> matteSources(const Scene &s) {
  QSet<QString> ids;
  for (const auto &layer : s.layers)
    if (!layer.trackMatteId.isEmpty()) ids.insert(layer.trackMatteId);
  return ids;
}
// Whether a layer puts anything into this frame: hidden layers and layers
// whose time is not now draw nothing, so they cannot stop the frame being
// split. Inside a group a layer keeps the group's time, so it always counts; a
// hidden layer still draws as another's matte; and the shutter of a motion
// blur reaches a quarter of a frame either side.
static bool layerDrawsAt(const Layer &layer, double frame, const QSet<QString> &mattes) {
  if (layerIsStructure(layer)) return false;
  if (!layer.groupId.isEmpty()) return true;
  if (!layer.visible && !mattes.contains(layer.id)) return false;
  return frame >= layer.start - .5 && frame < layer.start + layer.duration + .5;
}
bool compositionBands(const Scene &s, QSize outputSize, double frame) {
  if (outputSize.isEmpty()) outputSize = QSize(s.width, s.height);
  // An Effect Layer works on the whole picture beneath it; a band holds only
  // its own rows of that, and a blur on it would stop at every band's edge.
  if (sceneNeedsWholeForEffects(s, frame)) return false;
  const QSet<QString> mattes = matteSources(s);
  for (const auto &layer : s.layers)
    if (layerDrawsAt(layer, frame, mattes) && layerBlocksBands(layer, s, outputSize, frame)) return false;
  return true;
}

int motionBlurSampleCount(const Scene &s, double frame, QSize outputSize) {
  // Keep slow motion inexpensive, then add temporal samples only when the
  // visible travel would leave gaps. Corners catch rotation and scale as well
  // as translation; measuring in output pixels keeps Full/Half/Quarter useful.
  const double outputScaleX = double(outputSize.width()) / std::max(1, s.width);
  const double outputScaleY = double(outputSize.height()) / std::max(1, s.height);
  double travel = 0;
  for (const auto &layer : s.layers) {
    if (!layer.visible || !layer.motionBlur || frame < layer.start ||
        frame >= layer.start + layer.duration)
      continue;
    const double firstTime = std::clamp(frame - .25, layer.start,
                                        layer.start + layer.duration - 1e-6);
    const double lastTime = std::clamp(frame + .25, layer.start,
                                       layer.start + layer.duration - 1e-6);
    const double local = std::clamp(frame - layer.start, 0.,
                                    std::max(0., layer.duration - 1e-6));
    QRectF bounds = repeatedLayerBounds(layer, layerBounds(layer, s));
    QTransform first = layerWorldTransform(layer, s, firstTime);
    QTransform last = layerWorldTransform(layer, s, lastTime);
    if (layerHasMainTransform(layer)) {
      first *= layerMainTransform(layer);
      last *= layerMainTransform(layer);
    }
    const std::array<QPointF, 5> probes{
        bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(),
        bounds.bottomRight(), bounds.center()};
    for (const QPointF &probe : probes) {
      const QPointF a = first.map(probe), b = last.map(probe);
      travel = std::max(travel,
                        std::hypot((b.x() - a.x()) * outputScaleX,
                                   (b.y() - a.y()) * outputScaleY));
    }
  }
  // Around four pixels between samples is smooth once the antialiased sample
  // footprints overlap. Forty-eight caps pathological motion and memory/work.
  return std::clamp(int(std::ceil(travel / 4.)) + 1, 8, 48);
}

namespace { int g_previewMotionBlurCap = 0; }
void setPreviewMotionBlurCap(int maxSamples) { g_previewMotionBlurCap = maxSamples; }
int previewMotionBlurCap() { return g_previewMotionBlurCap; }
int previewMotionBlurCap(const Scene &s) { return s.finalRender ? 0 : g_previewMotionBlurCap; }

// How many bands a frame is worth. Most layers divide: a band draws its own
// rows of a cached raster, or fills its own rows of a path. Two kinds are drawn
// again, whole, by every band they reach, and a frame made mostly of them pays
// for that once per band:
//  - text whose letters are moving, which is drawn wherever its letters fall,
//    so by every band -- sixty such titles took 4.9 ms in twenty bands against
//    4.0 on one core;
//  - a 3D layer drawn from its vectors, projected (and walled, if extruded)
//    band by band -- a hundred and fifty 3D shapes took 29 ms in twenty bands,
//    23 on one core, and 19 in four, where the filling still divides but little
//    is done four times over.
static int bandsWorthUsing(const Scene &s, double frame, int workers) {
  if (workers <= 1) return workers;
  const QSet<QString> mattes = matteSources(s);
  int drawing = 0, letters = 0, vectors3D = 0;
  for (const auto &l : s.layers) {
    if (l.type == "camera" || l.type == "light" || l.type == "empty" || l.type == "audio" || l.type == "effect" ||
        !l.groupId.isEmpty() || !layerDrawsAt(l, frame, mattes))
      continue;
    ++drawing;
    if (l.type == "text" && l.characterAnimation.value("enabled").toBool() &&
        !l.hiddenEffects.value("Text-Animation").toBool() && !characterPoseAtRest(l, frame - l.start)) {
      ++letters;
      continue;
    }
    if (!l.threeD) continue;
    const bool spatial = l.blur != 0 || l.glow != 0 || l.shadow != 0 || rgbSplitReach(l) > 0 ||
                         (l.appliedEffects.value("Stroke").toBool() && !l.hiddenEffects.value("Stroke").toBool() &&
                          !vectorEffectStroke(l));
    if (spatial) continue;
    if (l.type == "shape" || parametricShape(l)) ++vectors3D;
    else if ((l.type == "text" || l.type == "fill") && l.appliedEffects.isEmpty() && l.surfaceEffects.isEmpty() &&
             !layerHasPixelEffect(l) && layerTextStrokes(l).empty() && !hasFeatheredMask(l))
      ++vectors3D;
  }
  if (letters >= 8 && 2 * letters >= drawing) return 1;
  if (vectors3D >= 8 && 2 * vectors3D >= drawing) return std::min(workers, 4);
  return workers;
}
// Where to cut a frame into bands so that they finish together. Cut into equal
// heights, a frame waits for whichever band holds the expensive part -- the lit
// face in a scene of lights took 15 ms in one band while the other nineteen
// averaged 6 and then waited -- and cutting it finer instead costs every band
// its share of setting up every layer again: six hundred shapes in forty bands
// took longer than in twenty. So the cuts move instead: each band's time is
// measured, spread over its rows as a cost per row, and the next frame of the
// same kind is cut where that cost divides evenly. Consecutive frames look
// alike, so the last one is a good guide; the cost is smoothed over frames, so
// a band that happened to land on a slower core does not throw the cuts about.
// Only where the rows are cut changes, never how any row is drawn.
namespace {
struct BandProfile {
  int height = 0;
  std::vector<float> cost; // per row, smoothed
  // How long the slowest band took, smoothed, with the rows cut evenly [0] and
  // cut by cost [1]: the cost of a row is only a guess -- where a band's time
  // goes on the layers it touches rather than on its rows, cutting by it made a
  // frame slower -- so whichever has actually done better is used.
  double slowest[2] = {0, 0};
  int tried[2] = {0, 0};
  quint64 frames = 0, used = 0;
};
QMutex &bandProfileGuard() { static QMutex guard; return guard; }
std::map<quint64, BandProfile> &bandProfiles() { static std::map<quint64, BandProfile> profiles; return profiles; }
quint64 bandProfileClock = 0;
quint64 bandKind(QRect region, int bands, int from, int to, size_t layers) {
  quint64 key = 1469598103934665603ull;
  for (qint64 v : {qint64(region.left()), qint64(region.top()), qint64(region.width()), qint64(region.height()),
                   qint64(bands), qint64(from), qint64(to), qint64(layers)})
    key = (key ^ quint64(v)) * 1099511628211ull;
  return key;
}
// The first row of each band and, last, the row after the final one. `byCost`
// says which way they were cut, for learnBandCosts.
std::vector<int> bandCuts(quint64 kind, QRect region, int bands, bool *byCost) {
  *byCost = false;
  std::vector<int> cuts(size_t(bands) + 1);
  for (int b = 0; b <= bands; ++b) cuts[size_t(b)] = region.top() + region.height() * b / bands;
  if (bands < 2 || region.height() < bands * 4) return cuts;
  QMutexLocker lock(&bandProfileGuard());
  const auto found = bandProfiles().find(kind);
  if (found == bandProfiles().end() || found->second.height != region.height()) return cuts;
  const BandProfile &profile = found->second;
  const std::vector<float> &cost = profile.cost;
  double total = 0;
  for (float c : cost) total += c;
  // A frame too light for its times to mean much keeps even cuts.
  if (!(total >= 20e6)) return cuts;
  // Whichever way has done better, the other tried again now and then in case
  // the frames have changed; and cost cuts tried first where even cuts are
  // clearly uneven.
  bool useCost;
  if (!profile.tried[1] || !profile.tried[0]) {
    double heaviest = 0;
    for (int b = 0; b < bands; ++b) {
      double sum = 0;
      for (int r = cuts[size_t(b)] - region.top(); r < cuts[size_t(b) + 1] - region.top(); ++r) sum += cost[size_t(r)];
      heaviest = std::max(heaviest, sum);
    }
    useCost = !profile.tried[1] && heaviest >= 1.3 * total / bands;
  } else {
    const bool costBetter = profile.slowest[1] < profile.slowest[0] * .97;
    useCost = profile.frames % 24 == 12 ? !costBetter : costBetter;
  }
  if (!useCost) return cuts;
  *byCost = true;
  // Every band at least four rows, so none is left with nothing but its setup.
  constexpr int least = 4;
  double sum = 0;
  int row = 0;
  for (int b = 1; b < bands; ++b) {
    const double wanted = total * b / bands;
    while (row < region.height() && sum + cost[size_t(row)] <= wanted) sum += cost[size_t(row++)];
    const int earliest = (cuts[size_t(b - 1)] - region.top()) + least;
    const int latest = region.height() - least * (bands - b);
    cuts[size_t(b)] = region.top() + std::clamp(row, earliest, std::max(earliest, latest));
  }
  return cuts;
}
void learnBandCosts(quint64 kind, QRect region, const std::vector<int> &cuts, const std::vector<qint64> &nanoseconds,
                    bool byCost) {
  if (cuts.size() < 3 || nanoseconds.size() + 1 != cuts.size()) return;
  QMutexLocker lock(&bandProfileGuard());
  auto &profiles = bandProfiles();
  BandProfile &profile = profiles[kind];
  const bool fresh = profile.height != region.height();
  if (fresh) { profile = BandProfile{}; profile.height = region.height(); profile.cost.assign(size_t(region.height()), 0.f); }
  qint64 slowest = 0;
  for (size_t b = 0; b + 1 < cuts.size(); ++b) {
    slowest = std::max(slowest, nanoseconds[b]);
    const int rows = cuts[b + 1] - cuts[b];
    if (rows <= 0 || nanoseconds[b] <= 0) continue;
    const float perRow = float(nanoseconds[b]) / float(rows);
    for (int r = cuts[b] - region.top(); r < cuts[b + 1] - region.top(); ++r) {
      float &c = profile.cost[size_t(r)];
      c = fresh ? perRow : c * .6f + perRow * .4f;
    }
  }
  const int way = byCost ? 1 : 0;
  profile.slowest[way] = profile.tried[way] ? profile.slowest[way] * .7 + double(slowest) * .3 : double(slowest);
  ++profile.tried[way];
  ++profile.frames;
  profile.used = ++bandProfileClock;
  // A handful of kinds of frame at a time is plenty: the least recently used go.
  while (profiles.size() > 32) {
    auto oldest = profiles.begin();
    for (auto it = profiles.begin(); it != profiles.end(); ++it)
      if (it->second.used < oldest->second.used) oldest = it;
    profiles.erase(oldest);
  }
}
}  // namespace

// Whether a banded frame should first build one row of itself, so every band
// then finds the large rasters already made. A layer carrying an effect (a
// blur, a glow, a stroke, noise, colour) builds its raster inside one band's
// worker, and a worker runs the effect's own passes on the single thread it is
// already on: a blur that takes thirteen milliseconds over the cores took fifty
// there. Built first it has them, and the bands then only read the finished
// raster from the cache. Only a large one: a small layer's effect is cheap
// wherever it is built, and looking at every such layer here would cost a
// scene of hundreds of them time on every frame.
static bool layerWarmsFirst(const Layer &layer, const Scene &s) {
  if (!layer.visible || !layer.groupId.isEmpty()) return false;
  if (layer.type == "text") return true;
  const bool heavy = layerHasPixelEffect(layer) || layer.blur || layer.glow || layer.shadow ||
                     (layer.appliedEffects["Stroke"].toBool() && !layer.hiddenEffects["Stroke"].toBool());
  if (!heavy) return false;
  const QRectF bounds = layerBounds(layer, s);
  return bounds.width() * bounds.height() >= 262144.;
}
// The rows to draw first, one row each, so that every such layer is built on
// this thread before the bands start. A row has to cross the layer to build
// it: the frame's top row alone missed every title that does not reach the top
// of the frame, which then went on to be built inside one band, its effects on
// a single core -- a zoom blur on a title took four times as long. As few rows
// as cross them all, a handful at most; the top row when nothing says where a
// layer lands.
static std::vector<int> bandsWarmRows(const Scene &s, double frame, QSize outputSize, QRect region) {
  std::vector<std::pair<int, int>> spans; // first and last output row each layer reaches
  bool unplaced = false;
  const double down = s.height > 0 ? double(outputSize.height()) / s.height : 1.;
  for (const Layer &layer : s.layers) {
    if (!layerWarmsFirst(layer, s)) continue;
    QRectF bounds;
    if (!layerCompositionReach(layer, s, frame, frame, bounds)) continue;
    const int top = std::max(region.top(), int(std::floor(bounds.top() * down)));
    const int bottom = std::min(region.bottom(), int(std::ceil(bounds.bottom() * down)) - 1);
    if (!bounds.isValid() || bottom < top) { unplaced = unplaced || !bounds.isValid(); continue; }
    spans.push_back({top, bottom});
  }
  std::vector<int> rows;
  if (unplaced) rows.push_back(region.top());
  // Fewest rows crossing every span: the one ending first takes a row at its
  // end, which also serves every span that has started by then.
  std::sort(spans.begin(), spans.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
  int last = std::numeric_limits<int>::min();
  for (const auto &[top, bottom] : spans) {
    if (last >= top && last <= bottom) continue;
    if (std::find(rows.begin(), rows.end(), bottom) == rows.end()) rows.push_back(bottom);
    last = bottom;
    if (rows.size() >= 6) break;
  }
  return rows;
}
// The rows above, drawn at once rather than one after another: each is a pass
// over the whole stack for one row, and in a music video whose lyrics change
// every frame six of them one by one took seven milliseconds on one core while
// the rest waited to start the bands. Two rows reaching the same text share its
// raster -- the second waits for the first to finish building it, as bands do.
static void warmBandRows(const Scene &s, double frame, bool motionBlur, QSize outputSize, QRect region,
                         int motionSamples = 8) {
  const std::vector<int> rows = bandsWarmRows(s, frame, outputSize, region);
  auto warm = [&](int n) {
    (void)renderRegion(s, frame, motionBlur, outputSize, QRect(region.left(), rows[size_t(n)], region.width(), 1),
                       motionSamples);
  };
  if (rows.size() < 2 || inRenderWorker || renderWorkerLimit() <= 1) {
    for (int n = 0; n < int(rows.size()); ++n) warm(n);
    return;
  }
  renderParallelEach(int(rows.size()), warm);
}
// Paints a scene's layers over `region` of the output onto `target`, which
// already holds what lies beneath them: sampleRegion, without starting from a
// fresh transparent picture.
static void paintRegionOnto(QImage &target, const Scene &s, double frame, QSize outputSize, QRect region,
                            bool perLayerBlur) {
  struct Scope {
    bool previous;
    explicit Scope(bool on) : previous(g_layerMotionBlur) { if (on) g_layerMotionBlur = true; }
    ~Scope() { g_layerMotionBlur = previous; }
  } scope(perLayerBlur);
  QPainter painter(&target);
  painter.translate(-region.left(), -region.top());
  painter.setClipRect(region);
  paintComposition(painter, s, frame, frame, outputSize);
}
// One layer that cannot be drawn in bands kept the whole frame on one core: in
// a music video with a large glowing picture near the bottom of the stack, the
// ninety layers above it were drawn one after another while nineteen cores
// waited. Instead the stack is cut at such layers. The runs between them are
// drawn in bands over every core, each such layer whole where it lies, all
// into one picture from the bottom up -- the same painting in the same order
// as drawing it straight through, so a blend mode still meets what is beneath
// it. Returns a null image where the stack cannot be cut that way.
static QImage renderSplitAtBlockers(const Scene &s, double frame, bool blur, QSize outputSize, QRect region,
                                    int workers) {
  const bool motion = s.motionBlur && blur;
  // A blur that averages whole frames would average across the cut.
  if (motion && !motionBlurPerLayerEligible(s, frame)) return {};
  // A part of the frame could not give a blur on the picture beneath what lies
  // just outside it.
  if (region != QRect(QPoint(), outputSize) && sceneNeedsWholeForEffects(s, frame)) return {};
  const QSet<QString> mattes = matteSources(s);
  std::vector<int> blockers;
  for (int i = 0; i < int(s.layers.size()); ++i) {
    const Layer &l = s.layers[size_t(i)];
    // An Effect Layer acting here is a cut too: what is beneath it is finished
    // first, it works on that whole, and the layers above are painted onto the
    // result -- in bands, and in place, so an Add or a matte above meets the
    // graded picture as it would drawn straight through. Laid on as a separate
    // frame instead, one glint set to Add above a grade kept the whole of a
    // music video's second verse on one core.
    const bool effect = effectLayerNeedsWhole(l, frame);
    if (!effect && (!layerDrawsAt(l, frame, mattes) || !layerBlocksBands(l, s, outputSize, frame))) continue;
    // Only a layer drawn in its own place in the stack: one in a group is drawn
    // by its group, a 3D one is sorted by depth among its neighbours, and a
    // matte is drawn together with the layer it cuts.
    if (!l.groupId.isEmpty() || l.type == "group" || l.threeD || !l.trackMatteId.isEmpty() || mattes.contains(l.id))
      return {};
    blockers.push_back(i);
  }
  if (blockers.empty() || blockers.size() > 8) return {};
  // Worth it only for a stack with something to share out: cutting a frame of
  // three layers into parts cost more than drawing it straight through.
  int shared = 0;
  for (int i = 0; i < int(s.layers.size()); ++i) {
    const Layer &l = s.layers[size_t(i)];
    if (l.type == "camera" || l.type == "light" || l.type == "empty" || l.type == "audio") continue;
    if (layerDrawsAt(l, frame, mattes) && std::find(blockers.begin(), blockers.end(), i) == blockers.end()) ++shared;
  }
  if (shared < 8) return {};
  const auto painting = processingFormat(s.processingDepth, true);
  QImage picture = s.processingDepth <= 8 ? pooledImage(region.size(), painting) : QImage(region.size(), painting);
  if (picture.isNull()) return {};
  picture.fill(Qt::transparent);
  uchar *rows = picture.bits();
  qsizetype stride = picture.bytesPerLine();
  // The layers from `from` to `to` (index 0 is the top of the stack). Hidden
  // rather than removed: whatever is parented to a layer still needs it, a
  // camera still has to look, and the 3D layers between two 2D ones are still
  // the same run to be sorted by depth. One copy of the scene serves every
  // part, its layers shown and hidden again for each: the parts are drawn one
  // after another, and a copy of a music video's 1,600 layers is 6 ms.
  std::optional<Scene> only;
  auto part = [&](int from, int to) -> const Scene & {
    if (!only) only.emplace(s);
    for (size_t i = 0; i < s.layers.size(); ++i) {
      const Layer &l = s.layers[i];
      if (!l.groupId.isEmpty() || l.type == "camera" || l.type == "light" || l.type == "empty" || l.type == "audio") continue;
      only->layers[i].visible = l.visible && int(i) >= from && int(i) <= to;
    }
    return *only;
  };
  auto drawsSomething = [&](int from, int to) {
    for (int i = std::max(0, from); i <= to; ++i)
      if (layerDrawsAt(s.layers[size_t(i)], frame, mattes)) return true;
    return false;
  };
  auto banded = [&](int from, int to) {
    if (from > to || !drawsSomething(from, to)) return;
    // Nothing in it but Effect Layers -- a grade over the whole frame, on top of
    // it all -- they are worked on the picture where it lies, each over the
    // cores, from the bottom up. Drawn in bands, a saturation grade on a music
    // video took a copy of its scene, a plan of where its 1,600 layers land,
    // and twenty passes down the whole stack to find it: 31 ms of a frame.
    // Where one cannot be worked that way, it and the rest above it go to
    // the bands as before.
    {
      bool onlyEffects = true;
      for (int i = std::max(0, from); i <= to && onlyEffects; ++i) {
        const Layer &l = s.layers[size_t(i)];
        if (layerDrawsAt(l, frame, mattes) && (l.type != "effect" || !l.groupId.isEmpty())) onlyEffects = false;
      }
      if (onlyEffects) {
        while (to >= from) {
          const Layer &l = s.layers[size_t(to)];
          if (l.type == "effect" && effectLayerActive(l, frame) && effectLayerDoesSomething(l)) {
            if (!applyEffectLayerWhole(picture, l, s, frame, outputSize)) break;
            if (picture.format() != painting) picture = picture.convertToFormat(painting);
            rows = picture.bits();
            stride = picture.bytesPerLine();
          }
          --to;
        }
        if (from > to || !drawsSomething(from, to)) return;
      }
    }
    const Scene &run = part(from, to);
    // Moving layers here blur themselves inside the one pass, so the plan holds
    // with their trails in it.
    CompositionPlanScope plan(run, frame, frame, motion);
    warmColdLayers(run, frame, outputSize, region, motion, true);
    warmBandRows(run, frame, motion, outputSize, region);
    const int lanes = bandsWorthUsing(run, frame, workers);
    const quint64 kind = bandKind(region, lanes, from, to, s.layers.size());
    bool byCost = false;
    const std::vector<int> cuts = bandCuts(kind, region, lanes, &byCost);
    std::vector<qint64> took(size_t(lanes), 0);
    std::vector<std::future<int>> jobs;
    for (int worker = 0; worker < lanes; ++worker) {
      const int top = cuts[size_t(worker)], bottom = cuts[size_t(worker) + 1];
      const QRect band(region.left(), top, region.width(), bottom - top);
      if (band.isEmpty()) continue;
      uchar *const first = rows + stride * (top - region.top());
      qint64 *const time = &took[size_t(worker)];
      jobs.push_back(renderJob([&run, first, stride, band, frame, outputSize, motion, painting, time] {
        QElapsedTimer clock;
        clock.start();
        // The band's own rows of the one picture, painted where they lie.
        QImage view(first, band.width(), band.height(), stride, painting);
        paintRegionOnto(view, run, frame, outputSize, band, motion);
        *time = clock.nsecsElapsed();
        return 0;
      }));
    }
    std::exception_ptr failure;
    for (auto &job : jobs) {
      try { job.get(); } catch (...) { failure = std::current_exception(); }
    }
    if (failure) std::rethrow_exception(failure);
    learnBandCosts(kind, region, cuts, took, byCost);
  };
  // From the bottom of the stack up.
  int below = int(s.layers.size()) - 1;
  for (auto it = blockers.rbegin(); it != blockers.rend(); ++it) {
    banded(*it + 1, below);
    // Whole, on this thread: its own passes spread over the cores.
    const Layer &cut = s.layers[size_t(*it)];
    const bool effect = cut.type == "effect";
    if (effect && !effectLayerDoesSomething(cut)) {
      // Nothing on it to do.
    } else if (effect && applyEffectLayerWhole(picture, cut, s, frame, outputSize)) {
      // Worked on where it lay; it may have come back as a new picture.
      if (picture.format() != painting) picture = picture.convertToFormat(painting);
      rows = picture.bits();
      stride = picture.bytesPerLine();
    } else {
      paintRegionOnto(picture, part(*it, *it), frame, outputSize, region, motion);
    }
    below = *it - 1;
  }
  banded(0, below);
  return s.processingDepth <= 8 ? straightFrame(std::move(picture)) : picture.convertToFormat(processingFormat(s.processingDepth));
}
bool sceneIsolates(const Scene &s) {
  for (const Layer &l : s.layers)
    if (l.isolated && l.groupId.isEmpty()) return true;
  return false;
}
Scene isolatedScene(const Scene &s) {
  Scene only = s;
  for (Layer &l : only.layers) {
    if (l.groupId.isEmpty() && !l.isolated && l.type != "camera" && l.type != "light" && l.type != "audio")
      l.visible = false;
    l.isolated = false;
  }
  return only;
}
QImage renderCPU(const Scene &s, double frame, bool blur, QSize outputSize, QRect region) {
  if (sceneIsolates(s)) return renderCPU(isolatedScene(s), frame, blur, outputSize, region);
  ExpressionSceneScope expressions(&s);
  // Where moving letters land, worked out once for the frame and every band.
  TextBoundsMemoScope textBounds;
  // One frame, one claim on the cache. Not inside a worker: a banded render is
  // still the same frame, and resetting per band would let the last band evict
  // what the first one just put there.
  if (!inRenderWorker) beginRasterFrame();
  if (outputSize.isEmpty())
    outputSize = QSize(s.width, s.height);
  region = region.isEmpty() ? QRect(QPoint(), outputSize)
                            : region.intersected(QRect(QPoint(), outputSize));
  if (region.isEmpty()) return QImage();
  // Part of a frame -- the preview zoomed in -- is drawn whole and cut down
  // when an Effect Layer is in it: a blur on the picture beneath needs what
  // lies just outside the part being shown, and drawn alone the part would
  // blur into nothing at its own edges.
  if (region != QRect(QPoint(), outputSize) && sceneNeedsWholeForEffects(s, frame))
    return renderCPU(s, frame, blur, outputSize, QRect()).copy(region);
  blur = blur && compositionHasMotion(s, frame);
  // A frame with an Effect Layer in it is drawn in the order of its stack: the
  // layers beneath the topmost Effect Layer first, as a frame of their own --
  // so they are split over the cores like any other frame -- then that layer's
  // effects once over the whole of it, then the layers above it laid on top.
  // Drawn in one pass instead, the picture beneath could not be split into
  // bands, and five hundred shapes under a grade took three times as long.
  // Only where laying the upper layers on afterwards is the same as painting
  // them in place: plain layers above (no blend mode, no matte) and no motion
  // blur, whose samples would otherwise be combined across the split -- unless
  // every moving layer blurs itself, inside its own pass, which is the same on
  // either side of the split. A music video with one flying title used to lose
  // the split for the whole of its length, and every frame under its grain and
  // grades was drawn on one pass instead of across the cores.
  if (!inRenderWorker && renderWorkerLimit() > 1 && (!(s.motionBlur && blur) || motionBlurPerLayerEligible(s, frame)) &&
      sceneNeedsWholeForEffects(s, frame)) {
    const QImage cut = renderSplitAtBlockers(s, frame, blur, outputSize, region,
                                             std::min(renderWorkerLimit(), region.height()));
    if (!cut.isNull()) return cut;
    int top = -1;
    for (int i = 0; i < int(s.layers.size()); ++i) {
      const Layer &l = s.layers[size_t(i)];
      if (l.groupId.isEmpty() && effectLayerNeedsWhole(l, frame)) { top = i; break; }
    }
    bool plainAbove = top >= 0;
    for (int i = 0; i < top && plainAbove; ++i) {
      const Layer &l = s.layers[size_t(i)];
      if (!l.visible || !l.groupId.isEmpty()) continue;
      if (l.blendMode != "normal" || !l.trackMatteId.isEmpty()) plainAbove = false;
      // A grade above, laid on with the rest, would grade nothing but them.
      if (effectLayerActive(l, frame)) plainAbove = false;
    }
    if (plainAbove) {
      // Hidden rather than removed: a layer left out of a part still has to be
      // there for whatever is parented to it, and a camera still has to look.
      Scene below = s, only = s, above = s;
      for (size_t i = 0; i < s.layers.size(); ++i) {
        const Layer &l = s.layers[i];
        if (!l.groupId.isEmpty() || l.type == "camera" || l.type == "light" || l.type == "empty" || l.type == "audio") continue;
        if (int(i) <= top) below.layers[i].visible = false;
        if (int(i) != top) only.layers[i].visible = false;
        if (int(i) >= top) above.layers[i].visible = false;
      }
      QImage picture = renderCPU(below, frame, blur, outputSize, region);
      if (picture.isNull()) return picture;
      const Layer &effect = s.layers[size_t(top)];
      if (effectLayerDoesSomething(effect) && !applyEffectLayerWhole(picture, effect, s, frame, outputSize)) {
        picture = picture.convertToFormat(processingFormat(s.processingDepth, true));
        QPainter painter(&picture);
        painter.setClipRect(QRect(QPoint(), outputSize));
        paintComposition(painter, only, frame, frame, outputSize);
      }
      // Nothing above that draws at this frame -- a fade at nought, a camera, the
      // song -- is no reason to draw a whole frame of nothing and lay it on.
      bool anythingAbove = false;
      if (top > 0) {
        const QSet<QString> mattes = matteSources(s);
        for (int i = 0; i < top && !anythingAbove; ++i) {
          const Layer &l = s.layers[size_t(i)];
          if (!l.groupId.isEmpty() || l.type == "camera" || l.type == "light" || l.type == "empty" || l.type == "audio") continue;
          if (!layerDrawsAt(l, frame, mattes)) continue;
          anythingAbove = l.p[5].at(std::clamp(frame - l.start, 0., std::max(0., l.duration - 1e-6))) > 0;
        }
      }
      if (anythingAbove) {
        const QImage over = renderCPU(above, frame, blur, outputSize, region);
        QPainter painter(&picture);
        painter.drawImage(0, 0, over);
      }
      return picture.convertToFormat(processingFormat(s.processingDepth));
    }
  }
  int motionSamples = blur ? motionBlurSampleCount(s, frame, outputSize) : 8;
  if (blur && previewMotionBlurCap(s) > 0)
    motionSamples = std::min(motionSamples, previewMotionBlurCap(s));

  // Effect frames parallelize inside each pixel pass. Splitting them into
  // strips would repeat padded blur/stroke work per strip and suppress those
  // inner workers. Plain composition instead parallelizes spatial regions.
  //
  int workers = (!compositionBands(s, outputSize, frame) || inRenderWorker)
                    ? 1
                    : renderWorkerLimit();
  workers = std::min(workers, std::max(1, region.height()));
  workers = bandsWorthUsing(s, frame, workers);
  if (workers == 1 && !inRenderWorker && renderWorkerLimit() > 1 && region.height() > 1) {
    const QImage split = renderSplitAtBlockers(s, frame, blur, outputSize, region,
                                               std::min(renderWorkerLimit(), region.height()));
    if (!split.isNull()) return split;
  }

  const bool perLayerBlur = s.motionBlur && blur && motionBlurPerLayerEligible(s, frame);
  if (workers == 1) {
    warmColdLayers(s, frame, outputSize, region, perLayerBlur, false);
    return renderRegion(s, frame, s.motionBlur && blur, outputSize, region, motionSamples);
  }

  // Where every layer lands, worked out once here rather than again in each
  // band. A frame whose moving layers blur themselves is still drawn once, so
  // the plan holds there too, with their trails in it; a frame averaged from
  // whole samples is drawn at several times, each landing somewhere of its own.
  std::optional<CompositionPlanScope> plan;
  if (!(s.motionBlur && blur)) plan.emplace(s, frame, frame);
  else if (perLayerBlur) plan.emplace(s, frame, frame, true);

  warmColdLayers(s, frame, outputSize, region, perLayerBlur, true);
  // On a cold frame every band used to miss the same whole-text raster at the
  // same time and all cores rebuilt it. Warm Character/text sources once on a
  // one-row target; the real bands then share the completed raster and spend
  // their cores compositing different output rows. This also makes width and
  // colour scrubs build one stroke raster per edit instead of one per core.
  warmBandRows(s, frame, s.motionBlur && blur, outputSize, region, motionSamples);

  // The picture the bands end up in. Not cleared first -- every row is written
  // by its band -- and each band puts its own rows in, on its own worker: the
  // clearing and the copying used to happen here, on one core, after the bands
  // were done, and on a light frame they cost more than drawing it (a frame of
  // four layers drew in 1.8 ms straight through and took 2.6 in bands, 2.2 of
  // them spent on eight megabytes of clearing, first touches and copies).
  QImage out = s.processingDepth <= 8 ? pooledImage(region.size(), processingFormat(s.processingDepth))
                                       : QImage(region.size(), processingFormat(s.processingDepth));
  if (out.isNull()) return out;
  uchar *const rows = out.bits();
  const qsizetype stride = out.bytesPerLine();
  const size_t rowBytes = size_t(region.width()) * out.depth() / 8;
  const quint64 kind = bandKind(region, workers, -1, -1, s.layers.size());
  bool byCost = false;
  const std::vector<int> cuts = bandCuts(kind, region, workers, &byCost);
  std::vector<qint64> took(size_t(workers), 0);
  std::vector<std::future<int>> jobs;
  jobs.reserve(workers);
  for (int worker = 0; worker < workers; ++worker) {
    const int top = cuts[size_t(worker)], bottom = cuts[size_t(worker) + 1];
    const QRect band(region.left(), top, region.width(), bottom - top);
    uchar *const first = rows + stride * (top - region.top());
    qint64 *const time = &took[size_t(worker)];
    jobs.push_back(renderJob([&s, frame, blur, outputSize, band, motionSamples, first, stride, rowBytes, time] {
      QElapsedTimer clock;
      clock.start();
      const QImage strip = renderRegion(s, frame, s.motionBlur && blur, outputSize, band, motionSamples);
      for (int row = 0; row < band.height(); ++row) {
        // A band that could not be drawn is left transparent, as it always was.
        if (row < strip.height()) std::memcpy(first + stride * row, strip.constScanLine(row), rowBytes);
        else std::memset(first + stride * row, 0, rowBytes);
      }
      *time = clock.nsecsElapsed();
      return 0;
    }));
  }
  std::exception_ptr failure;
  for (auto &job : jobs) {
    try { job.get(); } catch (...) { failure = std::current_exception(); }
  }
  if (failure) std::rethrow_exception(failure);
  learnBandCosts(kind, region, cuts, took, byCost);
  return out;
}

// ---------------------------------------------------------------------------
// A layer dragged in the viewer.
//
// From one step of a drag to the next only the dragged layer changes, yet every
// step drew the whole frame again: a music video's 27 layers beneath a title
// were drawn anew for each pixel it moved. Everything beneath it is drawn once
// and kept; each step paints the layer, and everything above it, onto a copy of
// that picture in one ordinary pass. That is the pass the whole frame makes,
// with the layers beneath already painted, so the pixels are the same: Effect
// Layers above it still work on the picture beneath them, blend modes still
// blend with it, its own matte still shapes it.
// ---------------------------------------------------------------------------
namespace {
// Whether an expression may read another layer: thisComp.layer(), a Layer
// control, a parent. Taken as yes whenever it might.
bool expressionReadsLayers(const QString &text) {
  return text.contains("layer", Qt::CaseInsensitive) || text.contains("effect(") || text.contains("parent");
}
// Whether layer `j` may be shaped by layer `index`, hang from it, or read it:
// its matte, its parents, an expression, a setting naming it, an effect that
// picks a layer by its place in the stack.
bool layerMayRead(const Scene &s, int j, int index) {
  const Layer &moved = s.layers[size_t(index)];
  const Layer &l = s.layers[size_t(j)];
  if (l.trackMatteId == moved.id || layerMatteSource(s, j) == index) return true;
  QString up = l.parentId;
  for (int depth = 0; !up.isEmpty() && depth < 64; ++depth) {
    if (up == moved.id) return true;
    const auto parent = std::find_if(s.layers.begin(), s.layers.end(), [&](const Layer &other) { return other.id == up; });
    up = parent == s.layers.end() ? QString() : parent->parentId;
  }
  for (const Property &p : l.p)
    if (p.hasExpression() && expressionReadsLayers(p.expression)) return true;
  for (const Property &p : l.extraCurves)
    if (p.hasExpression() && expressionReadsLayers(p.expression)) return true;
  // A texture, a source: any setting naming it.
  if (!l.surfaceEffects.isEmpty() && QJsonDocument(l.surfaceEffects).toJson(QJsonDocument::Compact).contains(moved.id.toUtf8()))
    return true;
  if (const auto *host = hostLayerEffects(); host && host->active && host->active(l)) return true;
  for (auto it = l.appliedEffects.constBegin(); it != l.appliedEffects.constEnd(); ++it) {
    if (!it.value().toBool()) continue;
    if (const PackEffect *pack = packEffect(it.key()))
      for (int k = 0; k < pack->api->parameterCount; ++k)
        if (pack->api->parameters[k].kind == WFX_LAYER) return true;
  }
  return false;
}
// Whether what lies beneath the lowest of the layers `moved` (sorted) stays as
// it is while they move.
bool plateHolds(const Scene &s, const std::vector<int> &moved, double frame, bool blur) {
  for (int index : moved) {
    const Layer &l = s.layers[size_t(index)];
    if (l.threeD || !l.groupId.isEmpty() || l.type == "camera" || l.type == "light" || l.type == "empty" || l.type == "audio")
      return false;
  }
  // Each whole sample of a motion blur would carry the layer through the rest.
  if (s.motionBlur && blur && compositionHasMotion(s, frame) && !motionBlurPerLayerEligible(s, frame)) return false;
  // Nothing beneath them may be shaped by one, hang from one, or read one.
  for (int j = moved.back() + 1; j < int(s.layers.size()); ++j)
    for (int index : moved)
      if (layerMayRead(s, j, index)) return false;
  return true;
}
// Where the layer can paint at this frame, in output pixels: what the frame's
// own passes trust for it (layerDeviceRect -- its box or its letters' as they
// are animated now, padded for its effects, cut to its masks, placed), and
// under its own motion blur the same at each sample of the shutter. Nothing
// when it is not there at all. Unknown (nullopt) where that cannot be told --
// particles, repeaters, groups, 3D, a plug-in drawing past its raster -- or
// where something above reads past the layer's place: an Effect Layer that
// cannot be worked in part (effectLayerPartable), or one hanging from it.
std::optional<QRect> layerFootprint(const Scene &s, int index, double frame, bool blur, QSize outputSize, QRect region) {
  const Layer &l = s.layers[size_t(index)];
  if (l.type == "particles" || l.particlesOn) return std::nullopt;
  if (!l.visible || frame - l.start < 0 || frame - l.start >= l.duration) return QRect();
  const double t = posterizedTime(l, s, std::clamp(frame - l.start, 0., std::max(0., l.duration - 1e-6)));
  if (l.p[5].at(t) <= 0) return QRect();
  const QTransform toOutput =
      QTransform::fromScale(double(outputSize.width()) / std::max(1, s.width), double(outputSize.height()) / std::max(1, s.height));
  QRect box;
  if (l.motionBlur && s.motionBlur && blur) {
    // The samples paintComposition averages, each where the layer is then.
    const int samples = layerBlurSampleCount(l, s, t, frame, outputSize);
    for (int k = 0; k < samples; ++k) {
      const double local = std::clamp(frame + ((k + .5) / samples - .5) * .5 - l.start, 0., std::max(0., l.duration - 1e-6));
      QRect one;
      if (!layerDeviceRect(l, s, local, toOutput, outputSize, one, true)) return std::nullopt;
      box = k ? box.united(one) : one;
    }
    if (samples > 0) return box.adjusted(-2, -2, 2, 2).intersected(region);
  }
  if (!layerDeviceRect(l, s, t, toOutput, outputSize, box)) return std::nullopt;
  return box.adjusted(-2, -2, 2, 2).intersected(region);
}
// The same for the layers `moved` (sorted) together: where any of them can
// paint. The frame without them is kept for the whole drag, so nothing else
// above the lowest of them may move with one, be shaped by one or read one.
std::optional<QRect> plateFootprint(const Scene &s, const std::vector<int> &moved, double frame, bool blur, QSize outputSize,
                                    QRect region) {
  for (int j = 0; j < moved.back(); ++j) {
    if (std::binary_search(moved.begin(), moved.end(), j)) continue;
    const Layer &above = s.layers[size_t(j)];
    if (effectLayerActive(above, frame) && effectLayerDoesSomething(above) && !effectLayerPartable(above, frame)) return std::nullopt;
    for (int index : moved)
      if (layerMayRead(s, j, index)) return std::nullopt;
  }
  QRect all;
  for (int index : moved) {
    const std::optional<QRect> one = layerFootprint(s, index, frame, blur, outputSize, region);
    if (!one) return std::nullopt;
    all = all.united(*one);
  }
  return all;
}
struct PlateScope {
  const Scene *scene;
  int below;
  const std::vector<int> *skip;
  PlateScope(const Scene &s, int index, const std::vector<int> *leftOut = nullptr)
      : scene(g_plateScene), below(g_plateBelow), skip(g_plateSkip) {
    g_plateScene = &s;
    g_plateBelow = index;
    g_plateSkip = leftOut;
  }
  ~PlateScope() {
    g_plateScene = scene;
    g_plateBelow = below;
    g_plateSkip = skip;
  }
};
// Where the layers named are in the stack, sorted; empty when one is not there.
std::vector<int> plateIndices(const Scene &s, const QStringList &ids) {
  std::vector<int> found;
  for (const QString &id : ids) {
    int index = -1;
    for (int i = 0; i < int(s.layers.size()) && index < 0; ++i)
      if (s.layers[size_t(i)].id == id) index = i;
    if (index < 0) return {};
    found.push_back(index);
  }
  std::sort(found.begin(), found.end());
  found.erase(std::unique(found.begin(), found.end()), found.end());
  return found;
}
}  // namespace

QImage renderOverPlate(const Scene &s, double frame, bool blur, QSize outputSize, QRect region, const QString &layerId,
                       quint64 key, DragPlate &plate) {
  return renderOverPlate(s, frame, blur, outputSize, region, QStringList{layerId}, key, plate);
}
QImage renderOverPlate(const Scene &s, double frame, bool blur, QSize outputSize, QRect region, const QStringList &layerIds,
                       quint64 key, DragPlate &plate) {
  if (inRenderWorker || sceneIsolates(s)) return {};
  if (outputSize.isEmpty()) outputSize = QSize(s.width, s.height);
  const QRect whole(QPoint(), outputSize);
  region = region.isEmpty() ? whole : region.intersected(whole);
  if (region.isEmpty()) return {};
  // Part of a frame with an Effect Layer in it is drawn whole and cut down
  // (renderCPU); not this way.
  if (region != whole && sceneNeedsWholeForEffects(s, frame)) return {};
  // The layers moved; the plate is everything beneath the lowest of them.
  const std::vector<int> moved = plateIndices(s, layerIds);
  if (moved.empty()) return {};
  const int index = moved.back();
  // Everything the picture beneath stands for, besides what the caller's key does.
  const quint64 full = renderOverPlateKey(s, frame, blur, outputSize, region, layerIds, key);
  QImage below;
  {
    QMutexLocker hold(&plate.lock);
    if (plate.key != full) {
      plate.key = full;
      plate.below = QImage();
      plate.holds = plateHolds(s, moved, frame, blur);
      if (plate.holds) {
        // Hidden rather than removed, as the Effect Layer split does: a layer
        // left out still has to be there for whatever is parented to it.
        Scene beneath = s;
        for (size_t i = 0; i <= size_t(index) && i < s.layers.size(); ++i) {
          const Layer &l = s.layers[i];
          if (!l.groupId.isEmpty() || l.type == "camera" || l.type == "light" || l.type == "empty" || l.type == "audio") continue;
          beneath.layers[i].visible = false;
        }
        plate.below = renderCPU(beneath, frame, blur, outputSize, region)
                          .convertToFormat(processingFormat(s.processingDepth, true));
      }
    }
    plate.readyKey = full;
    plate.readyHolds = plate.holds && !plate.below.isNull();
    if (!plate.holds || plate.below.isNull()) return {};
    below = plate.below;
  }
  QElapsedTimer step;
  step.start();
  ExpressionSceneScope expressions(&s);
  TextBoundsMemoScope textBounds;
  beginRasterFrame();
  // The pass the whole frame makes, over `picture` and inside `clip`, drawing
  // the layers from `lowest` up -- the dragged ones and those above them --
  // leaving out `skip` (the dragged ones, for the frame without them).
  const auto pass = [&](QImage &picture, int lowest, QRect clip, const std::vector<int> *skip = nullptr) {
    QPainter painter(&picture);
    painter.translate(-region.left(), -region.top());
    painter.setClipRect(clip);
    // The motion blur the whole frame would give it: each moving layer
    // blurring itself inside the one pass.
    LayerMotionBlurScope motion(s.motionBlur && blur && compositionHasMotion(s, frame) && motionBlurPerLayerEligible(s, frame));
    PlateScope over(s, lowest, skip);
    paintComposition(painter, s, frame, frame, outputSize);
  };
  // Only where the layer is, when that is known and is not most of the frame:
  // everywhere else the step is the frame with the layer left out, kept as the
  // plate is. An Effect Layer grading the whole frame above a title is then
  // worked over the title's place alone, not over all of it at every step.
  const std::optional<QRect> footprint = plateFootprint(s, moved, frame, blur, outputSize, region);
  QImage without;
  if (footprint && qint64(footprint->width()) * footprint->height() < qint64(region.width()) * region.height() * 6 / 10) {
    QMutexLocker hold(&plate.lock);
    if (plate.withoutKey != full || plate.without.isNull()) {
      QImage made = below.copy();
      pass(made, index, region, &moved);
      plate.without = made;
      plate.withoutKey = full;
    }
    without = plate.without;
    plate.lastDrawn = *footprint;
  } else {
    QMutexLocker hold(&plate.lock);
    plate.lastDrawn = region;
  }
  const QImage &start = without.isNull() ? below : without;
  QImage picture = s.processingDepth <= 8 ? pooledImage(region.size(), below.format()) : QImage(region.size(), below.format());
  if (picture.isNull()) return {};
  const size_t rowBytes = std::min(size_t(start.bytesPerLine()), size_t(picture.bytesPerLine()));
  for (int y = 0; y < start.height(); ++y) std::memcpy(picture.scanLine(y), start.constScanLine(y), rowBytes);
  QRect clip = region;
  if (!without.isNull()) {
    // Where the layer is: the picture beneath it, for the pass to draw over.
    clip = *footprint;
    const int bytes = below.depth() / 8;
    const QRect local = clip.translated(-region.topLeft());
    for (int y = local.top(); y <= local.bottom() && !local.isEmpty(); ++y)
      std::memcpy(picture.scanLine(y) + size_t(local.left()) * bytes, below.constScanLine(y) + size_t(local.left()) * bytes,
                  size_t(local.width()) * bytes);
  }
  if (!clip.isEmpty()) pass(picture, index, clip);
  QImage done = s.processingDepth <= 8 ? straightFrame(std::move(picture)) : picture.convertToFormat(processingFormat(s.processingDepth));
  plate.stepMs = step.nsecsElapsed() / 1e6;
  return done;
}

quint64 renderOverPlateKey(const Scene &s, double frame, bool blur, QSize outputSize, QRect region, const QString &layerId,
                           quint64 key) {
  return renderOverPlateKey(s, frame, blur, outputSize, region, QStringList{layerId}, key);
}
quint64 renderOverPlateKey(const Scene &s, double frame, bool blur, QSize outputSize, QRect region, const QStringList &layerIds,
                           quint64 key) {
  if (outputSize.isEmpty()) outputSize = QSize(s.width, s.height);
  const QRect whole(QPoint(), outputSize);
  region = region.isEmpty() ? whole : region.intersected(whole);
  quint64 full = key;
  const auto mix = [&full](quint64 v) { full = (full ^ v) * 0x100000001b3ull; };
  const std::vector<int> moved = plateIndices(s, layerIds);
  for (int index : moved) mix(quint64(index));
  if (moved.empty()) mix(quint64(-1));
  mix(quint64(s.layers.size()));
  mix(qHash(frame));
  mix(quint64(outputSize.width()) << 32 | quint64(outputSize.height()));
  mix(quint64(region.x()) << 48 ^ quint64(region.y()) << 32 ^ quint64(region.width()) << 16 ^ quint64(region.height()));
  mix(blur ? 3 : 5);
  return full;
}

void gradeCPU(QImage &image, float exposure) {
  // On the card or the processor, whichever is the quicker here.
  static RouteChoice route("Exposure");
  const qint64 gradePixels = qint64(image.width()) * image.height();
  const bool cardCan = canUseGpuPixels();
  if (cardCan && route.card(gradePixels)) {
    QElapsedTimer clock;
    clock.start();
    auto accelerated=gpuPixelOperation(image,0,{exposure,0,0,0});
    if(!accelerated.isNull()){image=accelerated;route.took(gradePixels,true,clock.nsecsElapsed()/1e6);return;}
    route.refused(gradePixels);
  }
  const ProcessorTiming timing(cardCan ? &route : nullptr, gradePixels);
  float gain = std::exp2(exposure);
  if (image.depth() > 32) {
    auto format = image.format();
    auto linear = image.convertToFormat(QImage::Format_RGBA32FPx4);
    auto *bits=linear.bits();const auto stride=linear.bytesPerLine();
    renderParallel(linear.height(),[&](int begin,int end){for (int y = begin; y < end; ++y) {
      auto *p = reinterpret_cast<float *>(bits+y*stride);
      for (int x = 0; x < linear.width(); ++x)
        for (int c = 0; c < 3; ++c) p[x*4+c] *= gain;
    }});
    image = linear.convertToFormat(format);
    return;
  }
  int workers = std::min(int(std::max(1u, std::thread::hardware_concurrency())),
                         std::max(1, image.height()));
  uchar *pixels = image.bits();
  const int stride = image.bytesPerLine();
  const int width = image.width();
  std::vector<std::future<void>> jobs;
  jobs.reserve(workers);
  for (int worker = 0; worker < workers; ++worker) {
    int top = image.height() * worker / workers;
    int bottom = image.height() * (worker + 1) / workers;
    jobs.push_back(renderJob([pixels, stride, width, gain, top, bottom] {
      for (int y = top; y < bottom; ++y) {
        auto *p = pixels + size_t(y) * stride;
        for (int x = 0; x < width; ++x)
          for (int c = 0; c < 3; ++c)
            p[x * 4 + c] = uint8_t(
                std::clamp(p[x * 4 + c] * gain, 0.f, 255.f) + .5f);
      }
    }));
  }
  std::exception_ptr failure;
  for (auto &job : jobs) {
    try { job.get(); } catch (...) { failure = std::current_exception(); }
  }
  if (failure) std::rethrow_exception(failure);
}
