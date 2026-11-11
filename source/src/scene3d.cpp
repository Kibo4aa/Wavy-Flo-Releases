#include "engine.h"
#include "lights.h"
#include <algorithm>
#include <cmath>

namespace {
QMatrix4x4 world(const Layer &l, const Scene &s, double frame, QSet<QString> &seen) {
  if (seen.contains(l.id)) return {};
  seen.insert(l.id);
  const double t = frame - l.start;
  QMatrix4x4 local;
  const QPointF pos = layerLocalPosition(l, t);
  local.translate(float(pos.x() + l.mainX), float(pos.y() + l.mainY),
                  l.threeD ? float(l.p[PositionZ].at(t) + l.mainZ) : 0.f);
  local.rotate(l.p[4].at(t), 0, 0, 1);
  if (l.threeD) {
    local.rotate(l.p[RotationY].at(t), 0, 1, 0);
    local.rotate(l.p[RotationX].at(t), 1, 0, 0);
  }
  local.scale(l.p[2].at(t), l.p[3].at(t), l.threeD ? l.p[ScaleZ].at(t) : 1);
  for (int i = int(l.additionalTransforms.size()) - 1; i >= 0; --i) {
    const auto &at = l.additionalTransforms[i];
    at.resolveProps(l);
    const double px = at.propPosX >= 0 ? l.at(at.propPosX).at(t) : 0.;
    const double py = at.propPosY >= 0 ? l.at(at.propPosY).at(t) : 0.;
    const double pz = (l.threeD && at.propPosZ >= 0) ? l.at(at.propPosZ).at(t) : 0.;
    const double rot = at.propRotation >= 0 ? l.at(at.propRotation).at(t) : 0.;
    const double rx = (l.threeD && at.propRotationX >= 0) ? l.at(at.propRotationX).at(t) : 0.;
    const double ry = (l.threeD && at.propRotationY >= 0) ? l.at(at.propRotationY).at(t) : 0.;
    const double sx = at.propScaleX >= 0 ? l.at(at.propScaleX).at(t) : 1.;
    const double sy = at.propScaleY >= 0 ? l.at(at.propScaleY).at(t) : 1.;
    const double sz = (l.threeD && at.propScaleZ >= 0) ? l.at(at.propScaleZ).at(t) : 1.;
    const double ax = at.propAnchorX >= 0 ? l.at(at.propAnchorX).at(t) : 0.;
    const double ay = at.propAnchorY >= 0 ? l.at(at.propAnchorY).at(t) : 0.;
    const double az = (l.threeD && at.propAnchorZ >= 0) ? l.at(at.propAnchorZ).at(t) : 0.;

    local.translate(float(ax + px), float(ay + py), float(az + pz));
    local.rotate(float(rot), 0, 0, 1);
    if (l.threeD) {
      local.rotate(float(ry), 0, 1, 0);
      local.rotate(float(rx), 1, 0, 0);
    }
    local.scale(float(sx), float(sy), float(sz));
    local.translate(float(-ax), float(-ay), float(-az));
  }
  local.translate(-l.p[AnchorX].at(t), -l.p[AnchorY].at(t),
                  l.threeD ? -l.p[AnchorZ].at(t) : 0);
  QMatrix4x4 binding = l.threeD ? l.parentBind3D : QMatrix4x4(l.parentBind);
  for (const auto &parent : s.layers)
    if (!l.parentId.isEmpty() && parent.id == l.parentId)
      return world(parent, s, frame, seen) * binding * local;
  return binding * local;
}
const Layer *activeCameraLayer(const Scene &s, double frame) {
  for (const auto &l : s.layers)
    if (l.type == "camera" && l.visible && frame >= l.start && frame < l.start + l.duration)
      return &l;
  return nullptr;
}
struct CameraView { QMatrix4x4 view; double zoom; };
CameraView cameraView(const Scene &s, double frame) {
  // Camera local +Z points into the scene; composition Y points down.
  for (const auto &l : s.layers) {
    if (l.type != "camera" || !l.visible || frame < l.start || frame >= l.start + l.duration) continue;
    bool invertible = false;
    auto view = layerWorldMatrix(l, s, frame).inverted(&invertible);
    if (invertible) return {view, std::max(1., l.p[CameraZoom].at(frame - l.start))};
  }
  const double zoom = s.width * 1.2;
  QMatrix4x4 view;
  view.translate(-s.width / 2., -s.height / 2., zoom);
  return {view, zoom};
}
}

QMatrix4x4 layerWorldMatrix(const Layer &l, const Scene &s, double frame) {
  QSet<QString> seen;
  return world(l, s, frame, seen);
}

QMatrix4x4 layerCameraMatrix(const Layer &l, const Scene &s, double frame, double &zoom) {
  const auto camera = cameraView(s, frame);
  zoom = camera.zoom;
  return camera.view * layerWorldMatrix(l, s, frame);
}

QVector3D cameraEyePosition(const Scene &s, double frame) {
  bool invertible = false;
  const QMatrix4x4 place = cameraView(s, frame).view.inverted(&invertible);
  return invertible ? place.map(QVector3D()) : QVector3D(float(s.width / 2.), float(s.height / 2.), float(-s.width * 1.2));
}

void addSharedView(Scene &inner, const Scene &outer, const QString &level) {
  QSet<QString> present;
  for (const auto &l : inner.layers) present.insert(l.id);
  auto find = [&](const QString &id) -> const Layer * {
    for (const auto &l : outer.layers)
      if (l.id == id) return &l;
    return nullptr;
  };
  std::vector<Layer> extra;
  for (const auto &l : outer.layers) {
    if ((l.type != "camera" && l.type != "light") || l.groupId != level || present.contains(l.id)) continue;
    Layer copy = l;
    copy.groupId.clear();
    extra.push_back(copy);
    present.insert(l.id);
    // What it hangs from, so it stands where it stands; hidden, so it draws nothing.
    QSet<QString> seen;
    for (const Layer *parent = find(l.parentId); parent && !seen.contains(parent->id); parent = find(parent->parentId)) {
      seen.insert(parent->id);
      if (present.contains(parent->id)) continue;
      Layer hidden = *parent;
      hidden.visible = false;
      hidden.groupId.clear();
      extra.push_back(hidden);
      present.insert(parent->id);
    }
  }
  inner.layers.insert(inner.layers.end(), extra.begin(), extra.end());
}

bool projectScenePoint(const Scene &s, double frame, const QVector3D &point, QPointF &at) {
  const auto camera = cameraView(s, frame);
  const QVector3D v = camera.view.map(point);
  if (v.z() <= 1.f) return false;
  at = QPointF(s.width / 2. + camera.zoom * v.x() / v.z(), s.height / 2. + camera.zoom * v.y() / v.z());
  return true;
}

QMatrix4x4 repeaterCopyWorldMatrix(const Layer &l, const Scene &s, double frame, int copy) {
  QMatrix4x4 model = layerWorldMatrix(l, s, frame);
  if (l.repeaterEnabled) {
    const auto t = repeaterCopyTransform(l, copy + l.repeaterOffset);
    model.translate(float(t.offset.x()), float(t.offset.y()), float(t.offsetZ));
    model.rotate(float(t.angle), 0, 0, 1);
    model.scale(float(t.scale), float(t.scale), float(t.scale));
  }
  return model;
}

QTransform projectLayer(const Layer &l, const Scene &s, double frame) {
  const auto camera = cameraView(s, frame);
  const auto m = camera.view * layerWorldMatrix(l, s, frame);
  const double cx = s.width / 2., cy = s.height / 2., z = camera.zoom;
  // Homography of the layer's z=0 plane; also used for vector painting and picking.
  return QTransform(z*m(0,0)+cx*m(2,0), z*m(1,0)+cy*m(2,0), m(2,0),
                    z*m(0,1)+cx*m(2,1), z*m(1,1)+cy*m(2,1), m(2,1),
                    z*m(0,3)+cx*m(2,3), z*m(1,3)+cy*m(2,3), m(2,3));
}

QTransform projectRepeaterCopy(const Layer &l, const Scene &s, double frame,
                               int copy) {
  if (!l.threeD || !l.repeaterEnabled)
    return projectLayer(l, s, frame);
  const auto t = repeaterCopyTransform(l, copy + l.repeaterOffset);
  QMatrix4x4 repeated = layerWorldMatrix(l, s, frame);
  repeated.translate(float(t.offset.x()), float(t.offset.y()), float(t.offsetZ));
  repeated.rotate(float(t.angle), 0, 0, 1);
  repeated.scale(float(t.scale), float(t.scale), float(t.scale));
  const auto camera = cameraView(s, frame);
  const auto m = camera.view * repeated;
  const double cx = s.width / 2., cy = s.height / 2., z = camera.zoom;
  return QTransform(z*m(0,0)+cx*m(2,0), z*m(1,0)+cy*m(2,0), m(2,0),
                    z*m(0,1)+cx*m(2,1), z*m(1,1)+cy*m(2,1), m(2,1),
                    z*m(0,3)+cx*m(2,3), z*m(1,3)+cy*m(2,3), m(2,3));
}

bool repeaterCopyInFrontOfCamera(const Layer &l, const Scene &s, double frame,
                                 int copy) {
  if (!l.threeD) return true;
  const double step = l.repeaterEnabled ? copy + l.repeaterOffset : 0;
  const auto t = repeaterCopyTransform(l, step);
  QMatrix4x4 repeated = layerWorldMatrix(l, s, frame);
  repeated.translate(float(t.offset.x()), float(t.offset.y()), float(t.offsetZ));
  repeated.rotate(float(t.angle), 0, 0, 1);
  repeated.scale(float(t.scale), float(t.scale), float(t.scale));
  const auto m = cameraView(s, frame).view * repeated;
  const auto box = layerBounds(l, s);
  for (const auto &point : {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()})
    if (m.map(QVector3D(point.x(), point.y(), 0)).z() <= 1.) return false;
  return true;
}

bool layerOrRepeaterInFrontOfCamera(const Layer &l, const Scene &s,
                                    double frame) {
  if (!l.threeD || l.type == "camera" || l.type == "light") return true;
  if (!l.repeaterEnabled) return layerInFrontOfCamera(l, s, frame);
  const int copies = std::clamp(l.repeaterCopies, 1, 1000);
  for (int copy = 0; copy < copies; ++copy)
    if (repeaterCopyInFrontOfCamera(l, s, frame, copy)) return true;
  return false;
}

double layerCameraDepth(const Layer &l, const Scene &s, double frame) {
  return (cameraView(s, frame).view * layerWorldMatrix(l, s, frame)).map(QVector3D()).z();
}

double cameraPlaneDistance(const Layer &camera, const Scene &s, double frame) {
  bool invertible = false;
  const auto view = layerWorldMatrix(camera, s, frame).inverted(&invertible);
  if (!invertible) return 0;
  return view.map(QVector3D()).z();
}

double layerDepthOfFieldBlur(const Layer &l, const Scene &s, double frame) {
  // Camera and Empty never render, and an ordinary 2D layer already ignores
  // the camera for everything else, so it ignores its Depth of Field too.
  if (!l.threeD || l.type == "camera" || l.type == "light" || l.type == "empty") return 0;
  const Layer *camera = activeCameraLayer(s, frame);
  if (!camera || !camera->dofEnabled) return 0;
  const double level = std::max(0., camera->dofBlurLevel) / 100.;
  if (level <= 0) return 0;
  const double fStop = std::max(.5, camera->dofFStop);
  const double sensor = 36.;
  const double width = std::max(1, s.width);
  // Read the focal length back out of the projection rather than off the
  // stored millimetres, so a keyframed Zoom moves the blur the way zooming a
  // real lens does instead of leaving the optics behind at the chosen prime.
  const double zoom = std::max(1., camera->p[CameraZoom].at(frame - camera->start));
  const double focal = zoom * sensor / width;    // millimetres
  const double focus = camera->dofFocusDistance; // scene units from the camera
  const double subject = layerCameraDepth(l, s, frame);
  // Behind the camera is not drawn anyway, and no lens focuses at a distance
  // inside its own focal length.
  if (subject <= 0 || focus <= focal) return 0;
  // The thin-lens circle of confusion: the diameter, on the sensor, of the
  // disc a point at `subject` lands as while the lens is focused at `focus`.
  // One scene unit reads as one millimetre of real distance, which puts the
  // camera about 2.7m from its subject at the default framing and makes the
  // f-number behave the way it does on a lens -- f/1.4 opens the background
  // right up, f/16 brings very nearly all of it back.
  const double coc = (focal * focal / fStop) * std::abs(subject - focus) /
                     (subject * (focus - focal));
  // Sensor millimetres back into composition pixels, then diameter to radius,
  // because l.blur is the radius the Gaussian pass works in. Held to the same
  // 100px ceiling the manual Gaussian Blur radius already uses, so a layer
  // right against the lens cannot ask for a blur past what is tuned for.
  return std::clamp(coc * width / sensor * .5 * level, 0., 100.);
}

bool layerInFrontOfCamera(const Layer &l, const Scene &s, double frame) {
  if (!l.threeD || l.type == "camera") return true;
  // A light has no plane; it is seen wherever it is not behind the lens.
  if (l.type == "light")
    return (cameraView(s, frame).view * layerWorldMatrix(l, s, frame)).map(QVector3D()).z() > 1.f;
  auto m = cameraView(s, frame).view * layerWorldMatrix(l, s, frame);
  // Do not let a plane crossing the near plane produce an unbounded projection.
  const auto box = layerBounds(l, s);
  for (const auto &pt : {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()})
    if (m.map(QVector3D(pt.x(), pt.y(), 0)).z() <= 1.) return false;
  return true;
}

QPointF layerPositionDelta(const Layer &l, const Scene &s, double frame, QPointF delta) {
  if (!l.threeD || l.type == "camera") {
    const auto inverse = layerParentTransform(l, s, frame).inverted();
    return inverse.map(delta) - inverse.map(QPointF());
  }
  // Invert the plane swept by Position X/Y at the current Z, including parents.
  QPolygonF local, projected;
  for (const auto &pt : {QPointF(0,0), QPointF(100,0), QPointF(100,100), QPointF(0,100)}) {
    Layer moved = l;
    for (int axis : {0, 1}) {
      moved.p[axis].keys.clear();
      moved.p[axis].setExpression({}); // the value below already includes it
      moved.p[axis].base = l.p[axis].at(frame - l.start) + (axis == 0 ? pt.x() : pt.y());
    }
    local << pt;
    projected << projectLayer(moved, s, frame).map(QPointF());
  }
  QTransform mapping;
  if (!QTransform::quadToQuad(projected, local, mapping)) return {};
  return mapping.map(projected[0] + delta) - mapping.map(projected[0]);
}

QPointF layerAnchorPosition(const Layer &l, const Scene &s, double frame) {
  const double t = frame - l.start;
  const QPointF anchor(l.p[AnchorX].at(t), l.p[AnchorY].at(t));
  if (!l.threeD || l.type == "camera")
    return layerWorldTransform(l, s, frame).map(anchor);
  const auto camera = cameraView(s, frame);
  const auto pt = (camera.view * layerWorldMatrix(l, s, frame)).map(
      QVector3D(anchor.x(), anchor.y(), l.p[AnchorZ].at(t)));
  return QPointF(s.width / 2. + camera.zoom * pt.x() / std::max(1.f, pt.z()),
                 s.height / 2. + camera.zoom * pt.y() / std::max(1.f, pt.z()));
}

std::vector<int> layerPaintOrder(const Scene &s, double frame) {
  std::vector<int> order;
  for (int i = int(s.layers.size()) - 1; i >= 0; --i)
    if (s.layers[i].type != "camera" && s.layers[i].type != "light" && s.layers[i].type != "empty"
        && !layerIsStructure(s.layers[i]) && s.layers[i].groupId.isEmpty()) order.push_back(i);
  // Preserve 2D stack boundaries; order each contiguous 3D group back to front.
  for (size_t begin = 0; begin < order.size();) {
    if (!s.layers[order[begin]].threeD) { ++begin; continue; }
    size_t end = begin + 1;
    while (end < order.size() && s.layers[order[end]].threeD) ++end;
    std::stable_sort(order.begin() + begin, order.begin() + end, [&](int a, int b) {
      return layerCameraDepth(s.layers[a], s, frame) > layerCameraDepth(s.layers[b], s, frame);
    });
    begin = end;
  }
  return order;
}

double layerExtrudeDepth(const Layer &l, double t) {
  if (!l.threeD) return 0;
  return std::clamp(l.p[ExtrudeDepth].at(t), 0., 100000.);
}

// The side walls of an extruded layer, drawn with the painter's own transform
// (the composition's) so they land where the layer's face will be drawn over
// them. Every edge of the outline is a quad from the face back to the given
// depth; the ones that face away from the camera are dropped and the rest are
// painted far to near, each lit by one light from above and to the left --
// or, in a scene with lights of its own, by those, face by face.
// The walls stand between the face's outline (pushed out by the bevel) and the
// same outline at the full depth, so they lie inside that box; seen from in
// front of the camera, a box lands inside the outline of its eight corners.
// A frame drawn in bands asks this before drawing a layer in a band: the face
// alone left out the sides that stand beyond it, and a turning, extruded shape
// lost its sides from every band below the one its face ended in.
QRectF extrudedWallReach(const Layer &l, const Scene &s, double frame, int repeaterCopy, const QRectF &face) {
  const double t = frame - l.start;
  const double depth = layerExtrudeDepth(l, t);
  if (depth <= 0) return {};
  const double bevel = std::clamp(l.p[ExtrudeBevel].at(t), 0., depth / 2);
  const QRectF grown = face.adjusted(-bevel, -bevel, bevel, bevel);
  const auto camera = cameraView(s, frame);
  const QMatrix4x4 m = camera.view * repeaterCopyWorldMatrix(l, s, frame, repeaterCopy);
  const double cx = s.width / 2., cy = s.height / 2., zoom = camera.zoom;
  double left = 0, top = 0, right = 0, bottom = 0;
  bool first = true;
  for (const double z : {0., depth})
    for (const QPointF corner : {grown.topLeft(), grown.topRight(), grown.bottomLeft(), grown.bottomRight()}) {
      const QVector3D v = m.map(QVector3D(float(corner.x()), float(corner.y()), float(z)));
      // Part of it behind the camera: no outline of the corners holds it.
      if (v.z() <= 1) return QRectF(-1e7, -1e7, 2e7, 2e7);
      const double x = cx + zoom * v.x() / v.z(), y = cy + zoom * v.y() / v.z();
      if (first) { left = right = x; top = bottom = y; first = false; }
      left = std::min(left, x); right = std::max(right, x);
      top = std::min(top, y); bottom = std::max(bottom, y);
    }
  return QRectF(QPointF(left, top), QPointF(right, bottom));
}

bool extrudeFrontFacesCamera(const Layer &l, const Scene &s, double frame, int repeaterCopy) {
  const auto camera = cameraView(s, frame);
  const QMatrix4x4 m = camera.view * repeaterCopyWorldMatrix(l, s, frame, repeaterCopy);
  const QPointF c = layerBounds(l, s).center();
  const QVector3D centre = m.map(QVector3D(float(c.x()), float(c.y()), 0.f));
  const QVector3D deeper = m.map(QVector3D(float(c.x()), float(c.y()), 1.f));
  // The camera looks out from the origin: the face is toward it when the
  // solid's depth runs on away from the camera behind it.
  return QVector3D::dotProduct(deeper - centre, centre) > 0;
}

void drawExtrudeWalls(QPainter &p, const Layer &l, const Scene &s, double frame, int repeaterCopy, bool backCap) {
  const double t = frame - l.start;
  const double depth = layerExtrudeDepth(l, t);
  if (depth <= 0) return;
  const auto pieces = layerExtrudePieces(l, s, t);
  if (pieces.empty()) return;

  const QMatrix4x4 model = repeaterCopyWorldMatrix(l, s, frame, repeaterCopy);
  const bool lit = layerTakesLight(l, s, frame);
  const SceneLighting lighting = lit ? sceneLighting(s, frame) : SceneLighting();
  // Its own face does not shadow its sides.
  int self = -1;
  for (int i = 0; i < int(s.layers.size()) && self < 0; ++i)
    if (s.layers[size_t(i)].id == l.id) self = i;
  const QMatrix3x3 worldNormals = model.normalMatrix();
  const float shine = float(std::clamp(l.p[SurfaceShine].at(t), 0., 100.) / 100.);
  const float gloss = float(std::clamp(l.p[SurfaceGloss].at(t), 0., 100.) / 100.);
  const auto camera = cameraView(s, frame);
  const QMatrix4x4 m = camera.view * model;
  const QMatrix3x3 normalMatrix = m.normalMatrix();
  const double cx = s.width / 2., cy = s.height / 2., zoom = camera.zoom;

  // Curves are flattened at the size they are seen, not the size they are made.
  const QVector3D origin = m.map(QVector3D(0, 0, 0));
  const double seen = origin.z() > 1 ? std::clamp(zoom / origin.z() * std::max(std::abs(l.p[2].at(t)), std::abs(l.p[3].at(t))), .25, 16.) : 1.;
  // Every outline of every piece, with how solid its piece is.
  std::vector<std::pair<QPolygonF, double>> polygons;
  for (const auto &piece : pieces)
    for (const QPolygonF &polygon : piece.outline.toSubpathPolygons(QTransform::fromScale(seen, seen)))
      polygons.push_back({polygon, piece.opacity});

  const QColor base = layerExtrudeSideColour(l);
  const double darkest = std::clamp(l.p[ExtrudeShade].at(t), 0., 100.) / 100.;
  const QVector3D toLight = QVector3D(-.5f, -.7f, -.6f).normalized(); // view space: x right, y down, z into the scene

  // The bevel: from the face's edge the wall first leans outward by `bevel`
  // while it goes back by as much, runs straight, and leans back in the same
  // way to the back -- which is the face's own size, so the back, carrying the
  // face's picture, meets its walls the way the face does (bevelled at the
  // front only, the back stood out past its picture in a plain ring). A
  // roundness above zero bends each lean into a quarter circle, one ring of
  // points per step. Every ring keeps the vertices of the outline, so the walls
  // are simply the quads between one ring and the next.
  const double bevel = std::clamp(l.p[ExtrudeBevel].at(t), 0., depth / 2);
  const double round = std::clamp(l.p[ExtrudeBevelRound].at(t), 0., 100.) / 100.;
  struct Step { double out, back; };
  std::vector<Step> profile{{0, 0}};
  if (bevel > 0) {
    // A curve drawn in steps shows its steps once it is wide enough on screen,
    // so a rounded bevel is given a step for roughly every three pixels of its
    // own size, and a chamfer needs only the one.
    const int steps = round > 0 ? std::clamp(int(bevel * seen / 3.), 4, 24) : 1;
    for (int k = 1; k <= steps; ++k) {
      const double u = double(k) / steps, angle = u * 3.14159265358979323846 / 2;
      profile.push_back({(1 - round) * u * bevel + round * bevel * std::sin(angle),
                         (1 - round) * u * bevel + round * bevel * (1 - std::cos(angle))});
    }
  }
  const size_t leaning = profile.size();
  if (profile.back().back < depth - bevel) profile.push_back({profile.back().out, depth - bevel});
  for (size_t k = leaning - 1; k-- > 0;) profile.push_back({profile[k].out, depth - profile[k].back});

  struct Wall { QPolygonF quad; double depth; QColor colour; };
  std::vector<Wall> walls;
  // The back, at the full depth, one outline per piece: its letters' rings
  // where the walls end.
  std::vector<std::pair<QPainterPath, double>> caps;
  auto project = [&](const QVector3D &v) { return QPointF(cx + zoom * v.x() / v.z(), cy + zoom * v.y() / v.z()); };
  auto toView = [&](const QVector3D &v) { return m.map(v); };
  for (const auto &[raw, pieceOpacity] : polygons) {
    QVector<QPointF> ring;
    for (const QPointF &point : raw) ring.push_back(point / seen);
    if (ring.size() > 1 && QLineF(ring.first(), ring.last()).length() < 1e-9) ring.removeLast();
    const int count = ring.size();
    if (count < 3) continue;
    double area = 0;
    for (int i = 0; i < count; ++i) area += ring[i].x() * ring[(i + 1) % count].y() - ring[(i + 1) % count].x() * ring[i].y();
    if (std::abs(area) < 1e-9) continue;
    const double orientation = area > 0 ? 1. : -1.;
    // Where each vertex moves for one unit of outward offset (a mitred corner,
    // held to twice the offset so a sharp point does not shoot away).
    QVector<QPointF> push(count);
    for (int i = 0; i < count; ++i) {
      const QPointF before = ring[i] - ring[(i + count - 1) % count], after = ring[(i + 1) % count] - ring[i];
      auto outward = [&](QPointF e) {
        const double length = std::hypot(e.x(), e.y());
        return length < 1e-9 ? QPointF() : QPointF(e.y() * orientation / length, -e.x() * orientation / length);
      };
      const QPointF n0 = outward(before), n1 = outward(after);
      QPointF sum = n0 + n1;
      const double sumLength = std::hypot(sum.x(), sum.y());
      if (sumLength < 1e-6) { push[i] = n1; continue; }
      sum /= sumLength;
      const double cosine = std::max(.5, sum.x() * n1.x() + sum.y() * n1.y());
      push[i] = sum / cosine;
    }
    auto point3 = [&](int i, const Step &step) {
      const QPointF at = ring[i % count] + push[i % count] * step.out;
      return QVector3D(float(at.x()), float(at.y()), float(step.back));
    };
    if (backCap) {
      QPolygonF projected;
      bool seenWhole = true;
      for (int i = 0; i < count && seenWhole; ++i) {
        const QVector3D v = toView(point3(i, profile.back()));
        if (v.z() <= 1) seenWhole = false;
        else projected << project(v);
      }
      if (seenWhole) {
        if (caps.empty() || caps.back().second != pieceOpacity) {
          caps.push_back({QPainterPath(), pieceOpacity});
          caps.back().first.setFillRule(Qt::WindingFill);
        }
        caps.back().first.addPolygon(projected);
        caps.back().first.closeSubpath();
      }
    }
    for (size_t k = 0; k + 1 < profile.size(); ++k)
      for (int i = 0; i < count; ++i) {
        if (QLineF(ring[i], ring[(i + 1) % count]).length() < 1e-6) continue;
        const QVector3D a0 = point3(i, profile[k]), b0 = point3(i + 1, profile[k]);
        const QVector3D a1 = point3(i, profile[k + 1]), b1 = point3(i + 1, profile[k + 1]);
        const QVector3D v[4] = {toView(a0), toView(b0), toView(b1), toView(a1)};
        if (v[0].z() <= 1 || v[1].z() <= 1 || v[2].z() <= 1 || v[3].z() <= 1) continue;
        // Outward normal of this stretch of wall: along the edge, then down its slope.
        const QVector3D local = QVector3D::crossProduct(b0 - a0, a1 - a0) * float(orientation);
        QVector3D n(normalMatrix(0, 0) * local.x() + normalMatrix(0, 1) * local.y() + normalMatrix(0, 2) * local.z(),
                    normalMatrix(1, 0) * local.x() + normalMatrix(1, 1) * local.y() + normalMatrix(1, 2) * local.z(),
                    normalMatrix(2, 0) * local.x() + normalMatrix(2, 1) * local.y() + normalMatrix(2, 2) * local.z());
        if (n.lengthSquared() < 1e-12f) continue;
        n.normalize();
        if (QVector3D::dotProduct(n, -v[0]) <= 0) continue; // faces away: hidden behind the wall in front
        Wall wall;
        wall.quad << project(v[0]) << project(v[1]) << project(v[2]) << project(v[3]);
        wall.depth = (v[0].z() + v[1].z() + v[2].z() + v[3].z()) / 4.;
        if (lit) {
          const QVector3D worldNormal(
              worldNormals(0, 0) * local.x() + worldNormals(0, 1) * local.y() + worldNormals(0, 2) * local.z(),
              worldNormals(1, 0) * local.x() + worldNormals(1, 1) * local.y() + worldNormals(1, 2) * local.z(),
              worldNormals(2, 0) * local.x() + worldNormals(2, 1) * local.y() + worldNormals(2, 2) * local.z());
          const LightAmount amount = lightAt(lighting, model.map((a0 + b0 + a1 + b1) / 4.f), worldNormal.normalized(), shine, gloss, self);
          auto channel = [](double c, float multiply, float add) { return float(std::clamp(c * multiply + add, 0., 1.)); };
          wall.colour = QColor::fromRgbF(channel(base.redF(), amount.multiply.x(), amount.add.x()),
                                         channel(base.greenF(), amount.multiply.y(), amount.add.y()),
                                         channel(base.blueF(), amount.multiply.z(), amount.add.z()), base.alphaF());
        } else {
          const double lightness = std::max(0., double(QVector3D::dotProduct(n, toLight)));
          const double keep = 1. - darkest * (1. - lightness);
          wall.colour = QColor::fromRgbF(float(base.redF() * keep), float(base.greenF() * keep), float(base.blueF() * keep), base.alphaF());
        }
        if (pieceOpacity < 1) wall.colour.setAlphaF(wall.colour.alphaF() * pieceOpacity);
        walls.push_back(std::move(wall));
      }
  }
  if (walls.empty() && caps.empty()) return;
  std::sort(walls.begin(), walls.end(), [](const Wall &a, const Wall &b) { return a.depth > b.depth; });
  // The back's colour, flat: lit as a wall facing straight back would be.
  QColor capColour = base;
  if (!caps.empty()) {
    const QVector3D local(0, 0, 1);
    QVector3D n(normalMatrix(0, 2), normalMatrix(1, 2), normalMatrix(2, 2));
    n.normalize();
    if (lit) {
      const QVector3D worldNormal = QVector3D(worldNormals(0, 2), worldNormals(1, 2), worldNormals(2, 2)).normalized();
      const QPointF c = layerBounds(l, s).center();
      const LightAmount amount = lightAt(lighting, model.map(QVector3D(float(c.x()), float(c.y()), float(depth))), worldNormal, shine, gloss, self);
      auto channel = [](double c, float multiply, float add) { return float(std::clamp(c * multiply + add, 0., 1.)); };
      capColour = QColor::fromRgbF(channel(base.redF(), amount.multiply.x(), amount.add.x()),
                                   channel(base.greenF(), amount.multiply.y(), amount.add.y()),
                                   channel(base.blueF(), amount.multiply.z(), amount.add.z()), base.alphaF());
    } else {
      const double lightness = std::max(0., double(QVector3D::dotProduct(n, toLight)));
      const double keep = 1. - darkest * (1. - lightness);
      capColour = QColor::fromRgbF(float(base.redF() * keep), float(base.greenF() * keep), float(base.blueF() * keep), base.alphaF());
    }
    (void)local;
  }
  p.save();
  p.setOpacity(p.opacity() * std::clamp(l.p[5].at(t), 0., 1.));
  p.setRenderHint(QPainter::Antialiasing);
  {
    // Each wall pushed out by three quarters of a device pixel on every side,
    // so the antialiased edges of neighbours overlap instead of leaving a
    // hairline. Half a pixel left one where a sliver of bevel seen edge on is
    // dropped between two walls.
    // (A pen did that before, and stroking every wall was most of their cost:
    // 750 walls of a bevelled title, 9.4 ms against 5.4.)
    const QTransform device = p.worldTransform();
    const double grow = .75 / std::max(1e-6, std::sqrt(std::abs(device.determinant())));
    // What this painter can show: a band of the frame, or all of it. A wall
    // outside it is not handed to the painter at all -- a band that sent
    // every wall paid for each one, where it lands or not.
    QRectF visible;
    if (const QPaintDevice *target = p.device()) {
      bool invertible = false;
      const QTransform back = device.inverted(&invertible);
      if (invertible) visible = back.mapRect(QRectF(0, 0, target->width(), target->height()));
    }
    if (p.hasClipping()) visible = visible.isNull() ? p.clipBoundingRect() : visible.intersected(p.clipBoundingRect());
    if (!visible.isNull()) visible.adjust(-2 * grow, -2 * grow, 2 * grow, 2 * grow);
    p.setPen(Qt::NoPen);
    for (const Wall &wall : walls) {
      const QPolygonF &q = wall.quad;
      if (!visible.isNull() && !q.boundingRect().intersects(visible)) continue;
      double area = 0;
      for (int i = 0; i < 4; ++i) area += q[i].x() * q[(i + 1) % 4].y() - q[(i + 1) % 4].x() * q[i].y();
      if (std::abs(area) < 1e-12) continue;
      const double side = area > 0 ? 1. : -1.;
      QPointF grown[4];
      for (int i = 0; i < 4; ++i) {
        auto outward = [&](QPointF e) {
          const double length = std::hypot(e.x(), e.y());
          return length < 1e-12 ? QPointF() : QPointF(e.y() * side / length, -e.x() * side / length);
        };
        const QPointF n0 = outward(q[i] - q[(i + 3) % 4]), n1 = outward(q[(i + 1) % 4] - q[i]);
        const double dot = n0.x() * n1.x() + n0.y() * n1.y();
        grown[i] = q[i] + (n0 + n1) * (grow / std::max(.25, 1. + dot));
      }
      p.setBrush(wall.colour);
      p.drawConvexPolygon(grown, 4);
    }
    for (const auto &[cap, opacity] : caps) {
      QColor colour = capColour;
      colour.setAlphaF(colour.alphaF() * opacity);
      p.setBrush(colour);
      p.drawPath(cap);
    }
  }
  p.restore();
}
