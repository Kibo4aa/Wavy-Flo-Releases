#include "lights.h"
#include <QMatrix3x3>
#include "render_workers.h"
#include <type_traits>
#include <algorithm>
#include <cmath>

QStringList lightKindNames() { return {"Point", "Spot", "Sun", "Ambient"}; }
QStringList lightFalloffNames() { return {"None", "Smooth", "Inverse Square"}; }

namespace {
bool lightShowing(const Layer &l, double frame) {
  return l.type == "light" && l.visible && frame >= l.start && frame < l.start + l.duration;
}
// How much of a lamp's light is left this far from it.
float falloffAt(const SceneLight &light, float distance) {
  if (light.falloff == 1) {
    if (distance <= light.radius) return 1;
    const float x = (distance - light.radius) / std::max(1.f, light.reach);
    return x >= 1 ? 0.f : 1.f - x * x * (3.f - 2.f * x);
  }
  if (light.falloff == 2) {
    const float r = std::max(1.f, light.radius);
    return distance <= r ? 1.f : (r / distance) * (r / distance);
  }
  return 1;
}
// How far inside a spot's cone a direction is: 1 in its middle, 0 outside.
float coneAt(const SceneLight &light, float cosine) {
  if (cosine <= light.coneOuter) return 0;
  if (cosine >= light.coneInner) return 1;
  const float x = (cosine - light.coneOuter) / (light.coneInner - light.coneOuter);
  return x * x * (3.f - 2.f * x);
}
}  // namespace

bool sceneHasLights(const Scene &s, double frame) {
  for (const auto &l : s.layers)
    if (lightShowing(l, frame)) return true;
  return false;
}

SceneLighting sceneLighting(const Scene &s, double frame) {
  SceneLighting lighting;
  lighting.eye = cameraEyePosition(s, frame);
  for (const auto &l : s.layers) {
    if (!lightShowing(l, frame)) continue;
    const double t = frame - l.start;
    SceneLight light;
    light.kind = std::clamp(l.lightKind, 0, 3);
    const double intensity = std::max(0., l.p[LightIntensity].at(t)) / 100.;
    const QColor c = l.color.isValid() ? l.color : QColor(Qt::white);
    light.colour = QVector3D(float(c.redF()), float(c.greenF()), float(c.blueF())) * float(intensity);
    const QMatrix4x4 m = layerWorldMatrix(l, s, frame);
    light.position = m.map(QVector3D(float(l.p[AnchorX].at(t)), float(l.p[AnchorY].at(t)), float(l.p[AnchorZ].at(t))));
    // Pointed at its aim, a place in the scene; one standing on it shines ahead.
    const QVector3D aim(float(l.p[LightAimX].at(t)), float(l.p[LightAimY].at(t)), float(l.p[LightAimZ].at(t)));
    QVector3D direction = aim - light.position;
    if (direction.lengthSquared() < 1e-6f) direction = m.mapVector(QVector3D(0, 0, 1));
    light.direction = direction.lengthSquared() < 1e-12f ? QVector3D(0, 0, 1) : direction.normalized();
    const double half = std::clamp(l.p[LightConeAngle].at(t), 0., 180.) / 2. * 3.14159265358979323846 / 180.;
    const double feather = std::clamp(l.p[LightConeFeather].at(t), 0., 100.) / 100.;
    light.coneOuter = float(std::cos(half));
    // A hard edge is still given a sliver of softness, or it stair-steps.
    light.coneInner = std::min(1.f, std::max(float(std::cos(half * (1. - feather))), light.coneOuter + .0015f));
    light.falloff = std::clamp(l.lightFalloff, 0, 2);
    light.radius = float(std::max(0., l.p[LightRadius].at(t)));
    light.reach = float(std::max(1., l.p[LightFalloffDistance].at(t)));
    light.shadowDarkness = float(std::clamp(l.p[LightShadowDarkness].at(t), 0., 100.) / 100.);
    if (l.lightShadows && light.kind != LightAmbient && light.shadowDarkness > 0) {
      const double softness = std::clamp(l.p[LightShadowSoftness].at(t), 0., 2000.);
      for (int index = 0; index < int(s.layers.size()); ++index) {
        const Layer &c = s.layers[size_t(index)];
        if (!c.castsShadows || !c.threeD || !c.visible || frame < c.start || frame >= c.start + c.duration) continue;
        if (c.type == "camera" || c.type == "light" || c.type == "empty" || c.type == "audio" || c.type == "effect" ||
            c.type == "group" || !c.groupId.isEmpty())
          continue;
        const double ct = frame - c.start;
        const float opacity = float(std::clamp(c.p[5].at(ct), 0., 1.));
        if (opacity <= 0) continue;
        ShadowCaster caster;
        double resolution = 1;
        caster.map = layerSilhouette(c, s, ct, softness, caster.bounds, resolution);
        if (caster.map.isNull()) continue;
        caster.resolution = float(resolution);
        caster.layer = index;
        caster.opacity = opacity;
        // Each copy of a repeater is a plane of its own.
        const int copies = c.repeaterEnabled ? std::clamp(c.repeaterCopies, 1, 64) : 1;
        for (int copy = 0; copy < copies; ++copy) {
          const QMatrix4x4 world = repeaterCopyWorldMatrix(c, s, frame, copy);
          bool invertible = false;
          caster.toLocal = world.inverted(&invertible);
          const QMatrix3x3 normals = world.normalMatrix();
          caster.normal = QVector3D(normals(0, 2), normals(1, 2), normals(2, 2));
          if (!invertible || caster.normal.lengthSquared() < 1e-20f) continue;
          caster.normal.normalize();
          caster.point = world.column(3).toVector3D();
          light.casters.push_back(caster);
        }
      }
    }
    lighting.lights.push_back(light);
  }
  return lighting;
}

bool layerCanTakeLight(const Layer &l) {
  return l.threeD && l.acceptsLights && l.type != "camera" && l.type != "light" && l.type != "empty" &&
         l.type != "audio" && l.type != "effect" && l.type != "group";
}

bool layerTakesLight(const Layer &l, const Scene &s, double frame) {
  return layerCanTakeLight(l) && sceneHasLights(s, frame);
}

namespace {
// A caster's coverage where a line through it meets its plane, read between
// its four nearest samples.
float coverageAt(const ShadowCaster &c, float u, float v) {
  const float x = (u - float(c.bounds.left())) * c.resolution - .5f, y = (v - float(c.bounds.top())) * c.resolution - .5f;
  const int w = c.map.width(), h = c.map.height();
  if (x <= -1.f || y <= -1.f || x >= float(w) || y >= float(h)) return 0;
  const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  const float fx = x - float(x0), fy = y - float(y0);
  auto at = [&](int px, int py) -> float {
    if (px < 0 || py < 0 || px >= w || py >= h) return 0.f;
    return c.map.constScanLine(py)[px] / 255.f;
  };
  return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}
}  // namespace

float lightGetsThrough(const SceneLight &light, const QVector3D &point, int self) {
  if (light.casters.empty()) return 1;
  // From the point to the lamp; a sun is as good as infinitely far away.
  const QVector3D reach = light.kind == LightSun ? -light.direction * 1e7f : light.position - point;
  float through = 1;
  for (const auto &c : light.casters) {
    if (c.layer == self) continue;
    const float along = QVector3D::dotProduct(reach, c.normal);
    if (std::abs(along) < 1e-9f) continue; // edge on to the light
    const float at = QVector3D::dotProduct(c.point - point, c.normal) / along;
    if (at <= 1e-5f || at >= 1.f - 1e-5f) continue; // not between the point and the lamp
    const QVector3D local = c.toLocal.map(point + reach * at);
    const float covered = coverageAt(c, local.x(), local.y());
    if (covered <= 0) continue;
    through *= 1.f - covered * c.opacity * light.shadowDarkness;
    if (through <= 1e-4f) return 0;
  }
  return through;
}

LightAmount lightAt(const SceneLighting &lighting, const QVector3D &point, const QVector3D &normal,
                    float shine, float gloss, int self) {
  LightAmount amount;
  const QVector3D toEye = lighting.eye - point;
  const float eyeDistance = toEye.length();
  const QVector3D v = eyeDistance > 1e-6f ? toEye / eyeDistance : QVector3D(0, 0, -1);
  QVector3D n = normal;
  if (QVector3D::dotProduct(n, v) < 0) n = -n; // the side the camera sees
  const float exponent = 2.f + gloss * gloss * 254.f;
  for (const auto &light : lighting.lights) {
    if (light.kind == LightAmbient) {
      amount.multiply += light.colour;
      continue;
    }
    QVector3D l;
    float strength = 1;
    if (light.kind == LightSun) l = -light.direction;
    else {
      const QVector3D toLight = light.position - point;
      const float distance = toLight.length();
      if (distance < 1e-6f) continue;
      l = toLight / distance;
      strength = falloffAt(light, distance);
      if (light.kind == LightSpot) strength *= coneAt(light, -QVector3D::dotProduct(l, light.direction));
      if (strength <= 0) continue;
    }
    const float diffuse = QVector3D::dotProduct(n, l);
    if (diffuse <= 0) continue;
    strength *= lightGetsThrough(light, point, self);
    if (strength <= 0) continue;
    amount.multiply += light.colour * (diffuse * strength);
    if (shine > 0) {
      const QVector3D h = l + v;
      const float length = h.length();
      if (length > 1e-6f) {
        const float nh = QVector3D::dotProduct(n, h) / length;
        if (nh > 0) amount.add += light.colour * (shine * std::pow(nh, exponent) * strength);
      }
    }
  }
  return amount;
}

namespace {
// A run of premultiplied pixels lit by an amount that changes by `step` from
// one pixel to the next. T is the channel, R G B A where each sits in a pixel.
template <typename T, int R, int G, int B, int A>
void shadeRun(uchar *row, int from, int to, QVector3D multiply, QVector3D add, const QVector3D &multiplyStep,
              const QVector3D &addStep) {
  constexpr float round = std::is_integral<T>::value ? .5f : 0.f;
  T *p = reinterpret_cast<T *>(row) + size_t(from) * 4;
  for (int x = from; x < to; ++x, p += 4, multiply += multiplyStep, add += addStep) {
    const float a = float(p[A]);
    if (a <= 0) continue;
    // Premultiplied: no channel may come out brighter than the pixel is solid.
    p[R] = T(std::clamp(float(p[R]) * multiply.x() + a * add.x(), 0.f, a) + round);
    p[G] = T(std::clamp(float(p[G]) * multiply.y() + a * add.y(), 0.f, a) + round);
    p[B] = T(std::clamp(float(p[B]) * multiply.z() + a * add.z(), 0.f, a) + round);
  }
}
using ShadeRun = void (*)(uchar *, int, int, QVector3D, QVector3D, const QVector3D &, const QVector3D &);
ShadeRun shadeRunFor(QImage::Format format) {
  switch (format) {
  case QImage::Format_RGBA8888_Premultiplied: return shadeRun<uchar, 0, 1, 2, 3>;
  case QImage::Format_RGBA64_Premultiplied: return shadeRun<quint16, 0, 1, 2, 3>;
  case QImage::Format_RGBA32FPx4_Premultiplied: return shadeRun<float, 0, 1, 2, 3>;
  case QImage::Format_ARGB32_Premultiplied:
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    return shadeRun<uchar, 2, 1, 0, 3>;
#else
    return shadeRun<uchar, 1, 2, 3, 0>;
#endif
  default: return nullptr;
  }
}
bool transparentAt(const uchar *row, int x, QImage::Format format) {
  switch (format) {
  case QImage::Format_RGBA64_Premultiplied: return reinterpret_cast<const quint16 *>(row)[x * 4 + 3] == 0;
  case QImage::Format_RGBA32FPx4_Premultiplied: return reinterpret_cast<const float *>(row)[x * 4 + 3] <= 0;
  case QImage::Format_ARGB32_Premultiplied: return qAlpha(reinterpret_cast<const QRgb *>(row)[x]) == 0;
  default: return row[x * 4 + 3] == 0;
  }
}
}  // namespace

void lightLayerImage(QImage &image, QPoint origin, const QTransform &localToDevice,
                     const QMatrix4x4 &world, const SceneLighting &lighting, float shine, float gloss, int self,
                     QPoint deviceToFrame) {
  if (image.isNull()) return;
  const QImage::Format format = image.format();
  const ShadeRun shade = shadeRunFor(format);
  if (!shade) {
    QImage converted = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    lightLayerImage(converted, origin, localToDevice, world, lighting, shine, gloss, -1, deviceToFrame);
    image = converted.convertToFormat(format);
    return;
  }
  bool invertible = false;
  const QTransform toLocal = localToDevice.inverted(&invertible);
  if (!invertible) return;
  const QVector3D corner = world.column(3).toVector3D();
  const QVector3D across = world.column(0).toVector3D(), down = world.column(1).toVector3D();
  const QMatrix3x3 normals = world.normalMatrix();
  QVector3D normal(normals(0, 2), normals(1, 2), normals(2, 2));
  if (normal.lengthSquared() < 1e-20f) return; // flattened to nothing: nothing to light
  normal.normalize();
  const int width = image.width(), height = image.height();
  // Rows are lit on several threads at once. Asked for a row, a QImage first
  // makes sure it is not shared and notes that it was asked -- from every
  // thread, on the same memory -- so the pixels are taken once, here.
  uchar *const pixels = image.bits();
  const qsizetype stride = image.bytesPerLine();
  auto rowAt = [pixels, stride](int y) { return pixels + stride * y; };
  // Where a device pixel's centre falls on the layer's plane, in the scene.
  auto pointAt = [&](double x, double y) {
    x += origin.x() + .5; y += origin.y() + .5;
    const double w = toLocal.m13() * x + toLocal.m23() * y + toLocal.m33();
    const double safe = std::abs(w) < 1e-12 ? 1e-12 : w;
    const double u = (toLocal.m11() * x + toLocal.m21() * y + toLocal.dx()) / safe;
    const double v = (toLocal.m12() * x + toLocal.m22() * y + toLocal.dy()) / safe;
    return corner + across * float(u) + down * float(v);
  };
  const QVector3D none;

  // Suns and ambient light alone, and no highlight: the same everywhere.
  bool uniform = true;
  for (const auto &light : lighting.lights)
    uniform &= light.kind == LightAmbient || (light.kind == LightSun && shine <= 0 && light.casters.empty());
  if (uniform) {
    const LightAmount amount = lightAt(lighting, pointAt(width / 2., height / 2.), normal, shine, gloss, self);
    renderParallel(height, [&](int from, int to) {
      for (int y = from; y < to; ++y) shade(rowAt(y), 0, width, amount.multiply, amount.add, none, none);
    });
    return;
  }

  // Worked out every fourth pixel and blended between. Blending is off by
  // about an eighth of how sharply the light bends there -- the difference
  // between a point and the mean of its neighbours -- so a cell where it bends
  // more than that allows (a cone's edge, the peak of a highlight) is worked
  // out pixel by pixel instead.
  //
  // The grid is laid on the frame, not on this picture or the device it is
  // for: a frame drawn in bands lights each band's part of a layer on its own,
  // and a grid starting at each part's corner -- or at each band's top row,
  // 54 rows apart, which a grid of 4 does not divide -- put its points somewhere
  // else in every band, and the frame came out a shade (1 in 255) off the same
  // frame drawn whole. There is also a
  // point to spare all round, so every point the pixels use reads the bend from
  // real neighbours rather than, at a band's edge, from the points inside it.
  constexpr int cell = 4;
  const QPoint onFrame = origin + deviceToFrame;
  const int ox = ((onFrame.x() % cell) + cell) % cell, oy = ((onFrame.y() % cell) + cell) % cell;
  const int cellColumns = (width + ox + cell - 1) / cell, cellRows = (height + oy + cell - 1) / cell;
  // Points -1 to cellColumns + 1 across, and the same down.
  const int columns = cellColumns + 3, rows = cellRows + 3;
  std::vector<LightAmount> grid(size_t(columns) * size_t(rows));
  renderParallel(columns * rows, [&](int from, int to) {
    for (int n = from; n < to; ++n)
      grid[size_t(n)] = lightAt(lighting, pointAt((n % columns - 1) * cell - ox, (n / columns - 1) * cell - oy),
                                normal, shine, gloss, self);
  });
  auto at = [&](int gx, int gy) -> const LightAmount & {
    return grid[size_t(std::clamp(gy, 0, rows - 1)) * columns + std::clamp(gx, 0, columns - 1)];
  };
  auto second = [](const LightAmount &a, const LightAmount &b, const LightAmount &c) {
    const QVector3D m = a.multiply + c.multiply - b.multiply * 2, d = a.add + c.add - b.add * 2;
    return std::max({std::abs(m.x()), std::abs(m.y()), std::abs(m.z()), std::abs(d.x()), std::abs(d.y()), std::abs(d.z())});
  };
  // Only the outermost points, which no pixel reads, borrow from the point inside.
  std::vector<float> bend(grid.size());
  renderParallel(columns * rows, [&](int from, int to) {
    for (int n = from; n < to; ++n) {
      const int gx = n % columns, gy = n / columns;
      const int x = std::clamp(gx, 1, std::max(1, columns - 2)), y = std::clamp(gy, 1, std::max(1, rows - 2));
      bend[size_t(n)] = std::max(second(at(x - 1, gy), at(x, gy), at(x + 1, gy)), second(at(gx, y - 1), at(gx, y), at(gx, y + 1)));
    }
  });
  constexpr float sharpest = .016f; // an eighth of it is half of one 8-bit step
  renderParallel(height, [&](int from, int to) {
    for (int y = from; y < to; ++y) {
      uchar *row = rowAt(y);
      const int cy = (y + oy) / cell;
      const float fy = float(y + oy - cy * cell) / cell;
      for (int cx = 0; cx < cellColumns; ++cx) {
        const int start = cx * cell - ox;
        const int x0 = std::max(0, start), x1 = std::min(width, start + cell);
        if (x0 >= x1) continue;
        const size_t here = size_t(cy + 1) * columns + size_t(cx + 1);
        if (std::max({bend[here], bend[here + 1], bend[here + columns], bend[here + columns + 1]}) < sharpest) {
          const LightAmount &a00 = grid[here], &a10 = grid[here + 1], &a01 = grid[here + columns], &a11 = grid[here + columns + 1];
          QVector3D leftMultiply = a00.multiply + (a01.multiply - a00.multiply) * fy;
          const QVector3D rightMultiply = a10.multiply + (a11.multiply - a10.multiply) * fy;
          QVector3D leftAdd = a00.add + (a01.add - a00.add) * fy;
          const QVector3D rightAdd = a10.add + (a11.add - a10.add) * fy;
          const QVector3D multiplyStep = (rightMultiply - leftMultiply) * (1.f / cell);
          const QVector3D addStep = (rightAdd - leftAdd) * (1.f / cell);
          // A cell cut by the picture's left edge starts part way along, stepped
          // there the way the run itself steps, so it adds up the same.
          for (int skip = start; skip < x0; ++skip) { leftMultiply += multiplyStep; leftAdd += addStep; }
          shade(row, x0, x1, leftMultiply, leftAdd, multiplyStep, addStep);
          continue;
        }
        for (int x = x0; x < x1; ++x) {
          if (transparentAt(row, x, format)) continue;
          const LightAmount amount = lightAt(lighting, pointAt(x, y), normal, shine, gloss, self);
          shade(row, x, x + 1, amount.multiply, amount.add, none, none);
        }
      }
    }
  });
}
