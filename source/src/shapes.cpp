#include "shapes.h"
#include <algorithm>
#include <cmath>

#include <array>
#include <cstring>
namespace {
constexpr double kPi = 3.14159265358979323846;

QPointF nodePoint(const QJsonObject &n, const char *key) {
  const auto a = n[QString::fromLatin1(key)].toArray();
  return QPointF(a.size() > 0 ? a.at(0).toDouble() : 0., a.size() > 1 ? a.at(1).toDouble() : 0.);
}
QJsonArray pair(QPointF p) { return QJsonArray{p.x(), p.y()}; }

using Cubic = std::array<QPointF, 4>;
QPointF cubicPoint(const Cubic &c, double t) {
  const auto a = c[0] + (c[1] - c[0]) * t;
  const auto b = c[1] + (c[2] - c[1]) * t;
  const auto d = c[2] + (c[3] - c[2]) * t;
  const auto e = a + (b - a) * t;
  const auto f = b + (d - b) * t;
  return e + (f - e) * t;
}
std::pair<Cubic, Cubic> splitCubic(const Cubic &c, double t) {
  const auto a = c[0] + (c[1] - c[0]) * t;
  const auto b = c[1] + (c[2] - c[1]) * t;
  const auto d = c[2] + (c[3] - c[2]) * t;
  const auto e = a + (b - a) * t;
  const auto f = b + (d - b) * t;
  const auto point = e + (f - e) * t;
  return {{{c[0], a, e, point}}, {{point, f, d, c[3]}}};
}
struct MeasuredCubic {
  Cubic curve;
  std::array<double, 33> distance{};
  double start = 0, length = 0;
  bool beginsContour = false;
  double timeAtDistance(double wanted) const {
    wanted = std::clamp(wanted, 0., length);
    auto upper = std::upper_bound(distance.begin(), distance.end(), wanted);
    if (upper == distance.begin()) return 0;
    if (upper == distance.end()) return 1;
    const int index = int(upper - distance.begin());
    const double a = distance[size_t(index - 1)], b = distance[size_t(index)];
    const double within = b > a ? (wanted - a) / (b - a) : 0;
    return (index - 1 + within) / 32.;
  }
};
QPainterPath trimPath(const QPainterPath &source, bool closed, double start,
                      double end, double offset) {
  std::vector<MeasuredCubic> segments;
  double total = 0;
  for (const auto &contour : shapeContours(source)) {
    const int pointCount = int(contour.size());
    const int edgeCount = closed ? pointCount : std::max(0, pointCount - 1);
    for (int edge = 0; edge < edgeCount; ++edge) {
      const auto &from = contour[edge], &to = contour[(edge + 1) % contour.size()];
      MeasuredCubic segment;
      segment.curve = {from.point, from.point + from.out, to.point + to.in, to.point};
      segment.start = total;
      segment.beginsContour = edge == 0;
      QPointF previous = segment.curve[0];
      for (int sample = 1; sample <= 32; ++sample) {
        const auto point = cubicPoint(segment.curve, sample / 32.);
        segment.distance[size_t(sample)] =
            segment.distance[size_t(sample - 1)] + QLineF(previous, point).length();
        previous = point;
      }
      segment.length = segment.distance.back();
      total += segment.length;
      segments.push_back(segment);
    }
  }
  if (total <= 1e-9 || end <= start + 1e-9) return {};
  const double span = end - start;
  double shiftedStart = std::fmod(start + offset, 1.);
  if (shiftedStart < 0) shiftedStart += 1.;
  QPainterPath result;
  auto appendRange = [&](double fromFraction, double toFraction) {
    const double fromDistance = fromFraction * total;
    const double toDistance = toFraction * total;
    bool drawing = false;
    for (const auto &segment : segments) {
      const double segmentEnd = segment.start + segment.length;
      const double from = std::max(fromDistance, segment.start);
      const double to = std::min(toDistance, segmentEnd);
      if (to <= from + 1e-9 || segment.length <= 1e-9) continue;
      const double t0 = segment.timeAtDistance(from - segment.start);
      const double t1 = segment.timeAtDistance(to - segment.start);
      Cubic piece = splitCubic(segment.curve, t1).first;
      if (t0 > 0 && t1 > 1e-9)
        piece = splitCubic(piece, std::clamp(t0 / t1, 0., 1.)).second;
      if (!drawing || (segment.beginsContour && from <= segment.start + 1e-9))
        result.moveTo(piece[0]);
      result.cubicTo(piece[1], piece[2], piece[3]);
      drawing = true;
    }
  };
  const double shiftedEnd = shiftedStart + span;
  if (shiftedEnd <= 1.)
    appendRange(shiftedStart, shiftedEnd);
  else {
    appendRange(shiftedStart, 1.);
    appendRange(0., shiftedEnd - 1.);
  }
  return result;
}

// The generators. These are the same formulas the shapes were always drawn
// from, kept to the letter so that moving the system underneath changes no
// pixel of any shape anyone has already made.
QPainterPath generatorPath(const Layer &l, double t) {
  QPainterPath path;
  const QString kind = shapeGeneratorKind(l);
  const double w = std::clamp(l.p[ShapeWidth].at(t), .01, 100000.);
  const double h = std::clamp(l.p[ShapeHeight].at(t), .01, 100000.);
  const double round = std::max(0., l.p[ShapeRoundness].at(t));
  if (kind == "rect") {
    const double r = std::min(round, std::min(w, h) / 2);
    path.addRoundedRect(QRectF(-w / 2, -h / 2, w, h), r, r);
  } else if (kind == "circle") {
    path.addEllipse(QRectF(-w / 2, -h / 2, w, h));
  } else if (kind == "polygon" || kind == "star") {
    const int count = std::clamp(qRound(l.p[ShapePoints].at(t)), kind == "star" ? 1 : 3, 1000);
    const int vertices = kind == "star" ? count * 2 : count;
    const double outer = std::clamp(l.p[OuterRadius].at(t), .01, 100000.);
    const double inner = std::clamp(l.p[InnerRadius].at(t), 0., outer);
    std::vector<QPointF> pts;
    for (int i = 0; i < vertices; ++i) {
      const double angle = -kPi / 2 + i * 2 * kPi / vertices;
      const double radius = kind == "star" && i % 2 ? inner : outer;
      pts.emplace_back(std::cos(angle) * radius, std::sin(angle) * radius);
    }
    if (round == 0 || vertices < 3) {
      path.moveTo(pts[0]);
      for (int i = 1; i < vertices; ++i) path.lineTo(pts[i]);
    } else {
      for (int i = 0; i < vertices; ++i) {
        const auto p = pts[i], previous = pts[(i + vertices - 1) % vertices],
                   next = pts[(i + 1) % vertices];
        const double a = QLineF(p, previous).length(), b = QLineF(p, next).length();
        const auto in = p + (previous - p) * (a > 0 ? std::min(round, a / 2) / a : 0);
        const auto out = p + (next - p) * (b > 0 ? std::min(round, b / 2) / b : 0);
        if (i == 0) path.moveTo(in); else path.lineTo(in);
        path.quadTo(p, out);
      }
    }
    path.closeSubpath();
  }
  QTransform rotation;
  rotation.rotate(l.p[PathRotation].at(t));
  return rotation.map(path);
}
}  // namespace

// The box around a shape, worked out from the numbers rather than by building
// the shape and measuring it. Drawing one frame asks for this box far more
// often than it draws anything -- every band of the frame asks it of every
// layer, to find out whether the layer reaches that band at all -- and building
// a path to answer it was a large part of a busy composition.
//
// Only for the plain generators with nothing bending them: anything with a
// modifier, a trim, or its own points is measured the long way, as is a rounded
// rectangle that has been turned, whose corners sit inside the box its sharp
// corners would make.
bool shapeGeneratorBounds(const Layer &l, double t, QRectF &out) {
  if (shapeFromPoints(l)) return false;
  const QString kind = shapeGeneratorKind(l);
  if (kind != "rect" && kind != "circle" && kind != "polygon" && kind != "star") return false;
  // The same test the modifiers themselves make: ridges on their own, with no
  // size to them, change nothing, and they start at four.
  if (l.p[ShapeRoundCorners].at(t) > 0 || l.p[ShapePuckerBloat].at(t) != 0 ||
      l.p[ShapeTwist].at(t) != 0 || l.p[ShapeOffsetPath].at(t) != 0 ||
      (l.p[ShapeZigZagSize].at(t) != 0 && l.p[ShapeZigZagRidges].at(t) > 0))
    return false;
  if (shapeTrimActive(l, t)) return false;
  const double turn = l.p[PathRotation].at(t);
  const double angle = turn * kPi / 180.;
  const double cs = std::cos(angle), sn = std::sin(angle);
  const double round = std::max(0., l.p[ShapeRoundness].at(t));
  if (kind == "rect") {
    if (round > 0 && turn != 0) return false;
    const double w = std::clamp(l.p[ShapeWidth].at(t), .01, 100000.) / 2;
    const double h = std::clamp(l.p[ShapeHeight].at(t), .01, 100000.) / 2;
    const double halfWidth = std::abs(w * cs) + std::abs(h * sn);
    const double halfHeight = std::abs(w * sn) + std::abs(h * cs);
    out = QRectF(-halfWidth, -halfHeight, halfWidth * 2, halfHeight * 2);
    return true;
  }
  if (kind == "circle") {
    // Only square on. An ellipse is drawn as four Bezier arcs, which stand a
    // couple of hundredths of a pixel outside the true curve, and turned they
    // carry that outside the box the exact formula gives: near enough to see
    // in no picture, but this box is what decides whether a layer is drawn at
    // all, so a turned ellipse is measured the long way as it always was.
    if (std::fmod(std::abs(turn), 90.) > 1e-9) return false;
    const double a = std::clamp(l.p[ShapeWidth].at(t), .01, 100000.) / 2;
    const double b = std::clamp(l.p[ShapeHeight].at(t), .01, 100000.) / 2;
    const double halfWidth = std::hypot(a * cs, b * sn);
    const double halfHeight = std::hypot(a * sn, b * cs);
    out = QRectF(-halfWidth, -halfHeight, halfWidth * 2, halfHeight * 2);
    return true;
  }
  if (round > 0) return false; // rounded corners pull the points inside the box
  const int count = std::clamp(qRound(l.p[ShapePoints].at(t)), kind == "star" ? 1 : 3, 1000);
  const int vertices = kind == "star" ? count * 2 : count;
  const double outer = std::clamp(l.p[OuterRadius].at(t), .01, 100000.);
  const double inner = std::clamp(l.p[InnerRadius].at(t), 0., outer);
  double left = 1e300, top = 1e300, right = -1e300, bottom = -1e300;
  for (int i = 0; i < vertices; ++i) {
    const double at = -kPi / 2 + i * 2 * kPi / vertices;
    const double radius = kind == "star" && i % 2 ? inner : outer;
    const double x = std::cos(at) * radius, y = std::sin(at) * radius;
    const double rx = x * cs - y * sn, ry = x * sn + y * cs;
    left = std::min(left, rx); right = std::max(right, rx);
    top = std::min(top, ry); bottom = std::max(bottom, ry);
  }
  if (left > right) return false;
  out = QRectF(QPointF(left, top), QPointF(right, bottom));
  return true;
}
bool shapeFromPoints(const Layer &l) {
  return (l.type == "shape" && l.shapeKind == "path") || !l.shapePoints.isEmpty();
}

bool shapeTrimActive(const Layer &l, double t) {
  const double start = std::clamp(l.p[ShapeTrimStart].at(t), 0., 100.);
  const double end = std::clamp(l.p[ShapeTrimEnd].at(t), 0., 100.);
  return start > .000001 || end < 99.999999;
}

// ---------------------------------------------------------------------------
// Modifiers: what turns a plain box into a flower, a gear, a badge.
//
// They reshape whatever the shape draws, one after another, before Trim Path,
// and every one is a property like any other, so each can be keyframed. All of
// them are off at their starting value and cost nothing until used.
// ---------------------------------------------------------------------------
namespace {

bool nodeIsCorner(const ShapeNode &n) { return n.in.isNull() && n.out.isNull(); }

// A contour as a run of points, for the modifiers that bend a line rather than
// move its nodes. Curves are sampled about every `step` pixels; straight
// segments too when `splitStraight` is set (a twist has to have something to
// bend).
QVector<QPointF> flattenContour(const ShapeContour &c, bool closed, double step, bool splitStraight) {
  QVector<QPointF> out;
  const int n = c.size();
  if (n == 0) return out;
  const int segments = closed ? n : n - 1;
  for (int i = 0; i < segments; ++i) {
    const auto &a = c[i], &b = c[(i + 1) % n];
    out.push_back(a.point);
    const bool straight = a.out.isNull() && b.in.isNull();
    const QPointF p0 = a.point, p1 = a.point + a.out, p2 = b.point + b.in, p3 = b.point;
    const double length = straight ? QLineF(p0, p3).length()
                                   : (QLineF(p0, p1).length() + QLineF(p1, p2).length() + QLineF(p2, p3).length());
    if (straight && !splitStraight) continue;
    const int pieces = std::clamp(int(length / step), straight ? 1 : 2, 200);
    for (int j = 1; j < pieces; ++j) {
      const double t = double(j) / pieces, u = 1 - t;
      if (straight) out.push_back(p0 + (p3 - p0) * t);
      else out.push_back(u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3);
    }
  }
  if (!closed) out.push_back(c.back().point);
  return out;
}

ShapeContour cornersFromPoints(const QVector<QPointF> &points) {
  ShapeContour c;
  for (const auto &p : points) c.push_back({p, {}, {}});
  return c;
}

// Ridges along the outline: points spaced evenly by length, thrown alternately
// to either side of it.
QVector<QPointF> zigZag(const QVector<QPointF> &line, bool closed, double size, double ridgesPer100) {
  if (line.size() < 2 || size == 0 || ridgesPer100 <= 0) return line;
  QVector<double> along{0};
  for (int i = 1; i < line.size(); ++i) along.push_back(along.back() + QLineF(line[i - 1], line[i]).length());
  const double total = along.back() + (closed ? QLineF(line.back(), line.front()).length() : 0.);
  if (total < 1e-6) return line;
  int count = std::max(2, int(total / 100. * ridgesPer100 + .5) * 2); // even, so a closed loop alternates cleanly
  auto pointAt = [&](double s, QPointF *tangent) {
    if (closed) s = std::fmod(std::fmod(s, total) + total, total);
    s = std::clamp(s, 0., total);
    int i = 1;
    while (i < line.size() && along[i] < s) ++i;
    QPointF a, b;
    if (i >= line.size()) { a = line.back(); b = closed ? line.front() : line.back(); if (!closed) a = line[line.size() - 2]; }
    else { a = line[i - 1]; b = line[i]; }
    const double length = QLineF(a, b).length();
    const double base = i >= line.size() ? along.back() : along[i - 1];
    const double t = length > 1e-9 ? std::clamp((s - base) / length, 0., 1.) : 0.;
    if (tangent) *tangent = length > 1e-9 ? (b - a) / length : QPointF(1, 0);
    return a + (b - a) * t;
  };
  QVector<QPointF> out;
  for (int k = 0; k < count + (closed ? 0 : 1); ++k) {
    QPointF tangent;
    const double s = total * k / count;
    const QPointF p = pointAt(s, &tangent);
    const bool end = !closed && (k == 0 || k == count);
    const QPointF normal(-tangent.y(), tangent.x());
    out.push_back(end ? p : p + normal * (k % 2 ? size : -size));
  }
  return out;
}

// The box around a set of nodes (QRectF::united would skip a zero-size one).
QRectF nodeBounds(const QVector<ShapeContour> &contours) {
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
  for (const auto &c : contours) for (const auto &n : c) {
    x0 = std::min(x0, n.point.x()); x1 = std::max(x1, n.point.x());
    y0 = std::min(y0, n.point.y()); y1 = std::max(y1, n.point.y());
  }
  return x0 > x1 ? QRectF() : QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

// Each node moves toward (or, negative, away from) the middle of the shape, and
// its handles lean back the other way: the segments stay full while the corners
// retreat, which is what pucker does, and the reverse is a bloat.
void puckerBloat(QVector<ShapeContour> &contours, double amount) {
  if (amount == 0) return;
  const QPointF center = nodeBounds(contours).center();
  const double a = amount / 100.;
  for (auto &c : contours)
    for (auto &n : c) {
      const QPointF moved = center + (n.point - center) * (1. - a * .5);
      const QPointF lean = (n.point - moved) * 1.1;
      n.in += lean; n.out += lean;
      n.point = moved;
    }
}

// Every sharp corner given a curve of up to `radius`, never more than half of
// either side it sits between.
QVector<ShapeContour> roundCorners(const QVector<ShapeContour> &contours, double radius, bool closed) {
  if (radius <= 0) return contours;
  constexpr double k = 0.5522847498;
  QVector<ShapeContour> result;
  for (const auto &c : contours) {
    const int n = c.size();
    ShapeContour out;
    for (int i = 0; i < n; ++i) {
      const auto &node = c[i];
      const bool ends = !closed && (i == 0 || i == n - 1);
      if (!nodeIsCorner(node) || ends || n < 3) { out.push_back(node); continue; }
      const QPointF prev = c[(i + n - 1) % n].point, next = c[(i + 1) % n].point, p = node.point;
      const double a = QLineF(p, prev).length(), b = QLineF(p, next).length();
      if (a < 1e-6 || b < 1e-6) { out.push_back(node); continue; }
      const double r = std::min({radius, a / 2., b / 2.});
      const QPointF start = p + (prev - p) * (r / a), end = p + (next - p) * (r / b);
      out.push_back({start, {}, (p - start) * k});
      out.push_back({end, (p - end) * k, {}});
    }
    result.push_back(out);
  }
  return result;
}

// A layer's shape after its modifiers. `closed` says whether the outline is a
// loop; open lines are reshaped too, but keep their ends.
QPainterPath modifiedShape(const QPainterPath &source, const Layer &l, double t, bool closed) {
  const double round = std::max(0., l.p[ShapeRoundCorners].at(t));
  const double pucker = l.p[ShapePuckerBloat].at(t);
  const double zigSize = l.p[ShapeZigZagSize].at(t), ridges = l.p[ShapeZigZagRidges].at(t);
  const double twist = l.p[ShapeTwist].at(t);
  const double offset = l.p[ShapeOffsetPath].at(t);
  const bool zig = zigSize != 0 && ridges > 0;
  if (round == 0 && pucker == 0 && !zig && twist == 0 && offset == 0) return source;
  if (source.isEmpty()) return source;

  auto contours = shapeContours(source);
  if (contours.isEmpty()) return source;
  puckerBloat(contours, pucker);

  if (zig || twist != 0) {
    const QRectF box = nodeBounds(contours);
    const QPointF center = box.center();
    const double reach = std::max(1., std::hypot(box.width(), box.height()) / 2.);
    QVector<ShapeContour> bent;
    for (const auto &c : contours) {
      auto line = flattenContour(c, closed, 6., twist != 0);
      if (zig) line = zigZag(line, closed, zigSize, ridges);
      if (twist != 0)
        for (auto &p : line) {
          const QPointF v = p - center;
          const double angle = twist * kPi / 180. * std::hypot(v.x(), v.y()) / reach;
          const double cs = std::cos(angle), sn = std::sin(angle);
          p = center + QPointF(v.x() * cs - v.y() * sn, v.x() * sn + v.y() * cs);
        }
      bent.push_back(cornersFromPoints(line));
    }
    contours = bent;
  }
  contours = roundCorners(contours, round, closed);
  QPainterPath path = shapePathFromContours(contours, closed);
  if (offset != 0) {
    QPainterPathStroker stroker;
    stroker.setWidth(std::abs(offset) * 2.);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setCapStyle(Qt::RoundCap);
    const QPainterPath ring = stroker.createStroke(path);
    path = closed ? (offset > 0 ? path.united(ring) : path.subtracted(ring)) : ring;
  }
  return path;
}

}  // namespace
static QPainterPath buildShapeGeometry(const Layer &l, double t) {
  // Points win over the generator: breaking a shape open is what stores them,
  // and from that moment the shape is what its points say it is.
  const bool closed = shapeFromPoints(l) ? l.shapeClosed : true;
  const QPainterPath source = modifiedShape(shapeFromPoints(l) ? vectorPath(l.shapePoints, l.shapeClosed)
                                                               : generatorPath(l, t),
                                            l, t, closed);
  if (!shapeTrimActive(l, t)) return source;
  const double start = std::clamp(l.p[ShapeTrimStart].at(t), 0., 100.) / 100.;
  const double end = std::clamp(l.p[ShapeTrimEnd].at(t), 0., 100.) / 100.;
  if (end <= start + 1e-9) return {};
  // Offset follows the familiar Trim Paths convention: one full turn is 360°.
  const double offset = l.p[ShapeTrimOffset].at(t) / 360.;
  return trimPath(source, l.shapeClosed, start, end, offset);
}
// Drawing one frame asks a shape what it looks like about thirty times over:
// once to know where it is, once to decide whether it is on screen, once for
// its stroke, once to draw it, and again for every cached raster it takes part
// in. Building the path each time was half the cost of a busy composition, so
// the last few are remembered.
//
// What makes this safe is that the memory is thrown away whenever a
// composition starts (and whenever the editor refreshes): within one pass
// through a scene, the passes drawn inside it included, nothing about a shape
// can change, so a shape asked for twice must answer the same twice. The key is the layer's identity, the time, and
// the numbers the geometry is made from, so a copy of a layer that differs in
// any of them is built afresh.
namespace {
struct RememberedShape {
  const void *identity = nullptr;
  double time = 0;
  quint64 stamp = 0;
  QPainterPath path;
  bool held = false;
};
thread_local std::array<RememberedShape, 4> g_remembered;
thread_local int g_rememberedNext = 0;
quint64 shapeStamp(const Layer &l, double t) {
  quint64 hash = 0xcbf29ce484222325ULL;
  auto mix = [&hash](quint64 value) { hash = (hash ^ value) * 0x100000001b3ULL; };
  for (int property = ShapeWidth; property < PropertyCount; ++property) {
    const double value = l.p[property].at(t);
    quint64 bits; std::memcpy(&bits, &value, 8);
    mix(bits);
  }
  mix(quint64(l.shapePoints.size()));
  mix(quint64(l.shapeClosed) | (quint64(l.shapeFill) << 1));
  mix(qHash(l.shapeKind));
  return hash;
}
}  // namespace
void shapeGeometryForget() {
  for (auto &remembered : g_remembered) { remembered.held = false; remembered.path = QPainterPath(); }
  g_rememberedNext = 0;
}
QPainterPath shapeGeometry(const Layer &l, double t) {
  const void *identity = static_cast<const void *>(l.id.constData());
  const quint64 stamp = shapeStamp(l, t);
  for (const auto &remembered : g_remembered)
    if (remembered.held && remembered.time == t && remembered.identity == identity &&
        remembered.stamp == stamp)
      return remembered.path;
  QPainterPath path = buildShapeGeometry(l, t);
  auto &slot = g_remembered[size_t(g_rememberedNext)];
  g_rememberedNext = (g_rememberedNext + 1) % int(g_remembered.size());
  slot.identity = identity; slot.time = t; slot.stamp = stamp; slot.path = path; slot.held = true;
  return path;
}

QVector<ShapeContour> shapeContours(const QPainterPath &path) {
  QVector<ShapeContour> contours;
  ShapeContour current;
  auto flush = [&] {
    if (current.size() > 1) contours.push_back(current);
    current.clear();
  };
  for (int i = 0; i < path.elementCount(); ++i) {
    const auto e = path.elementAt(i);
    if (e.type == QPainterPath::MoveToElement) {
      flush();
      current.push_back({QPointF(e.x, e.y), {}, {}});
    } else if (e.type == QPainterPath::LineToElement) {
      if (current.isEmpty()) continue;
      current.push_back({QPointF(e.x, e.y), {}, {}});
    } else if (e.type == QPainterPath::CurveToElement) {
      if (current.isEmpty() || i + 2 >= path.elementCount()) continue;
      const auto control1 = path.elementAt(i);
      const auto control2 = path.elementAt(i + 1);
      const auto end = path.elementAt(i + 2);
      // A cubic's control points are this node's outgoing handle and the next
      // node's incoming one, which is exactly how a Bezier node stores them.
      current.back().out = QPointF(control1.x, control1.y) - current.back().point;
      const QPointF endPoint(end.x, end.y);
      current.push_back({endPoint, QPointF(control2.x, control2.y) - endPoint, {}});
      i += 2;
    }
  }
  flush();
  // A closed subpath repeats its first point at the end; fold that away and
  // carry its incoming handle back to where it belongs.
  for (auto &contour : contours)
    if (contour.size() > 1 &&
        QLineF(contour.front().point, contour.back().point).length() < 1e-9) {
      contour.front().in = contour.back().in;
      contour.removeLast();
    }
  return contours;
}

QPainterPath shapePathFromContours(const QVector<ShapeContour> &contours, bool closed) {
  QPainterPath path;
  path.setFillRule(Qt::WindingFill);
  for (const auto &contour : contours) {
    if (contour.isEmpty()) continue;
    path.moveTo(contour.front().point);
    const int count = contour.size();
    for (int i = 1; i < count + (closed ? 1 : 0); ++i) {
      const auto &previous = contour[(i - 1) % count];
      const auto &next = contour[i % count];
      path.cubicTo(previous.point + previous.out, next.point + next.in, next.point);
    }
    if (closed) path.closeSubpath();
  }
  return path;
}

QJsonArray shapePointsFromPath(const QPainterPath &path) {
  QJsonArray points;
  const auto contours = shapeContours(path);
  for (int contourIndex = 0; contourIndex < contours.size(); ++contourIndex)
    for (int nodeIndex = 0; nodeIndex < contours[contourIndex].size(); ++nodeIndex) {
      const auto &node = contours[contourIndex][nodeIndex];
      QJsonObject value{{"p", pair(node.point)},
                        {"in", pair(node.in)},
                        {"out", pair(node.out)}};
      if (contourIndex > 0 && nodeIndex == 0) value["break"] = true;
      points.push_back(value);
    }
  return points;
}

int checkShapes(QStringList &failures) {
  {
    // The box worked out from the numbers has to be the box the built shape
    // fills, or a layer would be culled, framed or cached at the wrong size.
    // Anything the quick way declines is measured the long way as before.
    auto agrees = [&](Layer l, double t, const char *what) {
      QRectF quick;
      if (!shapeGeneratorBounds(l, t, quick)) return; // measured the long way
      const QRectF built = shapeGeometry(l, t).boundingRect();
      const double slack = 0.01;
      if (std::abs(quick.left() - built.left()) > slack || std::abs(quick.top() - built.top()) > slack ||
          std::abs(quick.right() - built.right()) > slack || std::abs(quick.bottom() - built.bottom()) > slack)
        failures << QString("quick bounds match the built shape: %1 gave %2,%3 %4x%5 against %6,%7 %8x%9")
                        .arg(what).arg(quick.x()).arg(quick.y()).arg(quick.width()).arg(quick.height())
                        .arg(built.x()).arg(built.y()).arg(built.width()).arg(built.height());
    };
    for (double turn : {0., 17., 90., -143., 360.}) {
      Layer rect; rect.type = "shape"; rect.shapeKind = "rect";
      rect.p[ShapeWidth].base = 200; rect.p[ShapeHeight].base = 120; rect.p[PathRotation].base = turn;
      agrees(rect, 0, "rectangle");
      Layer rounded = rect; rounded.p[ShapeRoundness].base = 30;
      agrees(rounded, 0, "rounded rectangle");
      Layer circle = rect; circle.shapeKind = "circle";
      agrees(circle, 0, "ellipse");
      Layer polygon; polygon.type = "shape"; polygon.shapeKind = "polygon";
      polygon.p[ShapePoints].base = 6; polygon.p[OuterRadius].base = 90; polygon.p[PathRotation].base = turn;
      agrees(polygon, 0, "polygon");
      Layer star = polygon; star.shapeKind = "star";
      star.p[ShapePoints].base = 5; star.p[InnerRadius].base = 40;
      agrees(star, 0, "star");
    }
    // And the shapes it must decline: trimmed, modified, or drawn from points.
    QRectF ignored;
    Layer trimmed; trimmed.type = "shape"; trimmed.shapeKind = "rect";
    trimmed.p[ShapeWidth].base = 100; trimmed.p[ShapeHeight].base = 100; trimmed.p[ShapeTrimEnd].base = 50;
    if (shapeGeneratorBounds(trimmed, 0, ignored)) failures << "quick bounds decline a trimmed shape";
    Layer twisted = trimmed; twisted.p[ShapeTrimEnd].base = 100; twisted.p[ShapeTwist].base = 45;
    if (shapeGeneratorBounds(twisted, 0, ignored)) failures << "quick bounds decline a twisted shape";
    Layer zigzag = trimmed; zigzag.p[ShapeTrimEnd].base = 100; zigzag.p[ShapeZigZagSize].base = 8;
    if (shapeGeneratorBounds(zigzag, 0, ignored)) failures << "quick bounds decline a zig-zagged shape";
    Layer plain = trimmed; plain.p[ShapeTrimEnd].base = 100;
    if (!shapeGeneratorBounds(plain, 0, ignored))
      failures << "quick bounds still answer for a plain shape (ridges rest above zero)";
  }
  auto near = [](QPointF a, QPointF b) { return QLineF(a, b).length() < .001; };
  {
    Layer trim;
    trim.type = "shape";
    trim.shapeKind = "circle";
    trim.p[ShapeWidth].base = trim.p[ShapeHeight].base = 200;
    const auto full = shapeGeometry(trim, 0);
    trim.p[ShapeTrimEnd].base = 50;
    const auto half = shapeGeometry(trim, 0);
    if (!shapeTrimActive(trim, 0) || half.isEmpty() ||
        std::abs(half.length() / full.length() - .5) > .01)
      failures << "Shape Trim Path End keeps the requested fraction of the outline";
    const auto beforeOffset = half.pointAtPercent(0);
    trim.p[ShapeTrimOffset].base = 90;
    const auto offset = shapeGeometry(trim, 0);
    if (offset.isEmpty() || near(beforeOffset, offset.pointAtPercent(0)))
      failures << "Shape Trim Path Offset moves the visible segment around the path";
    Scene saved;
    saved.layers = {trim};
    Scene restored;
    QString error;
    if (!restored.load(saved.json(), error) || restored.layers.empty() ||
        std::abs(restored.layers[0].p[ShapeTrimEnd].base - 50) > .001 ||
        std::abs(restored.layers[0].p[ShapeTrimOffset].base - 90) > .001)
      failures << "Shape Trim Path settings survive save and load";
  }
  {
    Layer line;
    line.type = "shape";
    line.shapeKind = "path";
    line.shapeClosed = false;
    line.shapeFill = false;
    line.strokeWidth = 20;
    line.shapePoints = {
        QJsonObject{{"p", pair({0, 0})}, {"in", pair({0, 0})}, {"out", pair({0, 0})}},
        QJsonObject{{"p", pair({100, 0})}, {"in", pair({0, 0})}, {"out", pair({0, 0})}}};
    Scene scene;
    scene.layers = {line};
    const auto roundBounds = layerBounds(scene.layers[0], scene);
    scene.layers[0].shapeStrokeCap = 1;
    const auto flatBounds = layerBounds(scene.layers[0], scene);
    if (roundBounds.width() <= flatBounds.width() + 19.9)
      failures << "Shape Line Cap switches from rounded to flat endpoints";
    Scene restored;
    QString error;
    if (!restored.load(scene.json(), error) || restored.layers.empty() ||
        restored.layers[0].shapeStrokeCap != 1)
      failures << "Shape Line Cap survives save and load";
  }
  {
    // The project model exposes one Shape layer type. Its generator and colour
    // survive a round trip, while old projects using generator-as-type migrate
    // at load time without changing their appearance.
    Scene scene;
    Layer shape;
    shape.type = "shape";
    shape.shapeKind = "star";
    shape.color = QColor("#35a7ff");
    shape.p[ShapePoints].base = 7;
    shape.p[OuterRadius].base = 90;
    shape.p[InnerRadius].base = 36;
    scene.layers = {shape};
    Scene restored; QString error;
    if (!restored.load(scene.json(), error) || restored.layers.size() != 1 ||
        restored.layers[0].type != "shape" || restored.layers[0].shapeKind != "star" ||
        restored.layers[0].color != shape.color)
      failures << "Shape layer keeps its generator and own colour";

    auto legacy = scene.json();
    auto layers = legacy["layers"].toArray();
    auto old = layers[0].toObject();
    old["type"] = "star";
    old.remove("shapeKind");
    layers[0] = old;
    legacy["layers"] = layers;
    Scene migrated;
    if (!migrated.load(legacy, error) || migrated.layers[0].type != "shape" ||
        migrated.layers[0].shapeKind != "star" || migrated.layers[0].color != shape.color)
      failures << "legacy generator layers migrate to Shape without losing colour";
  }
  {
    for (QString type : {"rect", "circle", "polygon", "star"}) {
      Layer l;
      l.type = type;
      l.p[ShapeWidth].base = 180;
      l.p[ShapeHeight].base = 120;
      l.p[ShapeRoundness].base = type == "rect" ? 20 : 6;
      l.p[ShapePoints].base = 5;
      l.p[OuterRadius].base = 90;
      l.p[InnerRadius].base = 40;
      l.p[PathRotation].base = 17;
      if (shapeGeometry(l, 0).isEmpty()) failures << "the generator draws something: " + type;
    }
  }
  {
    // Breaking a generator open has to hand back the same outline, curves and
    // all, or the first thing anyone does to a circle would dent it.
    Layer round;
    round.type = "circle";
    round.p[ShapeWidth].base = 200;
    round.p[ShapeHeight].base = 140;
    const auto before = shapeGeometry(round, 0);
    Layer opened = round;
    opened.type = "shape";
    opened.shapePoints = shapePointsFromPath(before);
    opened.shapeClosed = true;
    const auto after = shapeGeometry(opened, 0);
    if (opened.shapePoints.size() != 4)
      failures << QString("an ellipse opens into four nodes, not %1").arg(opened.shapePoints.size());
    const auto a = before.boundingRect(), b = after.boundingRect();
    if (std::abs(a.width() - b.width()) > .01 || std::abs(a.height() - b.height()) > .01 ||
        !near(a.center(), b.center()))
      failures << "breaking a shape open keeps its outline";
    // And the handles have to survive, not be flattened into corners.
    bool curved = false;
    for (const auto &value : opened.shapePoints) {
      const auto n = value.toObject();
      curved |= QLineF(QPointF(), nodePoint(n, "out")).length() > 1;
    }
    if (!curved) failures << "breaking a shape open keeps its curves";
  }
  {
    // A Shape Layer can hold multiple Pen contours. The break marker begins a
    // new subpath without changing existing one-path projects.
    QJsonArray paths{
        QJsonObject{{"p", pair({0, 0})}, {"in", pair({0, 0})}, {"out", pair({0, 0})}},
        QJsonObject{{"p", pair({40, 0})}, {"in", pair({0, 0})}, {"out", pair({0, 0})}},
        QJsonObject{{"p", pair({20, 35})}, {"in", pair({0, 0})}, {"out", pair({0, 0})}},
        QJsonObject{{"p", pair({80, 0})}, {"in", pair({0, 0})}, {"out", pair({0, 0})}, {"break", true}},
        QJsonObject{{"p", pair({120, 0})}, {"in", pair({0, 0})}, {"out", pair({0, 0})}},
        QJsonObject{{"p", pair({100, 35})}, {"in", pair({0, 0})}, {"out", pair({0, 0})}}};
    const auto joined = vectorPath(paths, true);
    const auto contours = shapeContours(joined);
    const auto restored = shapePointsFromPath(joined);
    if (contours.size() != 2 || restored.size() != 6 ||
        !restored[3].toObject()["break"].toBool())
      failures << "multiple Pen contours remain separate in one Shape layer";
  }
  {
    // Extreme parameters must still produce geometry a renderer can size a
    // raster from. An infinite or absent bounding box downstream is an
    // allocation no one can satisfy, which is a crash rather than a wrong
    // picture.
    for (QString type : {"rect", "circle", "polygon", "star"})
      for (double v : {0., 1., 1e5, 1e9, -1., -1e9}) {
        Layer l;
        l.type = type;
        for (int property : {ShapeWidth, ShapeHeight, ShapeRoundness, ShapePoints, OuterRadius,
                             InnerRadius, PathRotation})
          l.p[property].base = v;
        const auto box = shapeGeometry(l, 0).boundingRect();
        const bool finite = std::isfinite(box.left()) && std::isfinite(box.top()) &&
                            std::isfinite(box.width()) && std::isfinite(box.height());
        if (!finite || box.width() > 1e7 || box.height() > 1e7) {
          failures << QString("%1 stays a size a raster can be made from at %2 (%3 by %4)")
                          .arg(type).arg(v).arg(box.width()).arg(box.height());
          break;
        }
      }
  }
  {
    // A rectangle drawn square through unequal layer scales still needs one
    // visual stroke width on every edge. This also covers projects saved by
    // the older shape tool, which encoded Width/Height as Scale X/Y.
    Scene scene;scene.width=320;scene.height=240;scene.frames=30;
    Layer shape;shape.type="shape";shape.shapeKind="rect";shape.duration=30;
    shape.p[0].base=160;shape.p[1].base=120;
    shape.p[ShapeWidth].base=80;shape.p[ShapeHeight].base=40;
    shape.p[2].base=1;shape.p[3].base=2;
    shape.color=QColor("#7568e8");shape.strokeColor=Qt::black;
    shape.strokeWidth=10;shape.strokeMode=1;
    scene.layers={shape};
    const auto image=renderCPU(scene,0,false);
    QRect opaque,fill;
    for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x) {
      const auto c=image.pixelColor(x,y);
      if(c.alpha()>32) opaque=opaque.united(QRect(x,y,1,1));
      if(c.blue()>150 && c.red()>60) fill=fill.united(QRect(x,y,1,1));
    }
    const int horizontal=(opaque.width()-fill.width())/2;
    const int vertical=(opaque.height()-fill.height())/2;
    if(opaque.isEmpty() || fill.isEmpty() || std::abs(horizontal-vertical)>2)
      failures << QString("non-uniform Shape scale keeps an even stroke: %1 px versus %2 px")
                      .arg(horizontal).arg(vertical);
  }
  {
    // Shapes without pixel effects remain paths all the way to the composition.
    // An intermediate texture can change sampling phase when scale changes and
    // makes the visible edge hop between pixels during a transform drag.
    Scene scene; scene.width = 320; scene.height = 180; scene.frames = 30;
    Layer shape; shape.type = "shape"; shape.shapeKind = "rect"; shape.duration = 30;
    shape.p[0].base = 160.25; shape.p[1].base = 90.5;
    shape.p[ShapeWidth].base = 93; shape.p[ShapeHeight].base = 57;
    shape.p[4].base = 11.5; shape.strokeWidth = 7; shape.strokeMode = 1;
    shape.strokeColor = Qt::black; shape.color = QColor("#7568e8");
    scene.layers = {shape};
    const auto before = effectedRasterBuilds();
    const auto first = renderCPU(scene, 0, false, QSize(960, 540));
    scene.layers[0].mainScaleX = 1.37;
    scene.layers[0].mainScaleY = 0.82;
    scene.layers[0].mainRotation = 7.25;
    const auto transformed = renderCPU(scene, 0, false, QSize(960, 540));
    if (first.isNull() || transformed.isNull() || first == transformed ||
        effectedRasterBuilds() != before)
      failures << "plain Shape transform stays vector instead of rebuilding a sampled raster";
  }
  {
    // A large plain Shape is still a vector path. It can be divided into
    // output bands instead of forcing all motion samples through one core.
    Scene scene; scene.width = 1920; scene.height = 1080; scene.frames = 30;
    Layer shape; shape.type = "shape"; shape.shapeKind = "rect"; shape.duration = 30;
    shape.p[0].base = 960; shape.p[1].base = 540;
    shape.p[ShapeWidth].base = 1800; shape.p[ShapeHeight].base = 900;
    scene.layers = {shape};
    if (!compositionBands(scene, QSize(1920, 1080), 0))
      failures << "a large plain Shape remains eligible for multicore composition";
  }
  {
    // Fast motion should read as one continuous exposure, not eight detached
    // copies of the shape. Adaptive midpoint samples overlap across the trail.
    Scene scene; scene.width = 640; scene.height = 120; scene.frames = 30;
    scene.motionBlur = true;
    Layer shape; shape.type = "shape"; shape.shapeKind = "circle";
    shape.duration = 30; shape.motionBlur = true; shape.color = Qt::white;
    shape.p[1].base = 60; shape.p[ShapeWidth].base = 24; shape.p[ShapeHeight].base = 24;
    shape.p[0].set(0, 100); shape.p[0].set(1, 540);
    scene.layers = {shape};
    const auto blurred = renderCPU(scene, .5, true, QSize(640, 120));
    int first = -1, last = -1, longestGap = 0, gap = 0;
    for (int x = 0; x < blurred.width(); ++x) {
      if (blurred.pixelColor(x, 60).alpha() > 0) {
        if (first < 0) first = x;
        last = x; longestGap = std::max(longestGap, gap); gap = 0;
      } else if (first >= 0) {
        ++gap;
      }
    }
    if (first < 0 || last - first < 150 || longestGap > 1)
      failures << QString("fast Shape motion blur forms a continuous trail; longest gap %1")
                      .arg(longestGap);
  }
  return failures.isEmpty() ? 0 : 1;
}
