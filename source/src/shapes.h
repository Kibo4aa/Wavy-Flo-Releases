#pragma once
#include "engine.h"

// Wavy-Flo's shape model.
//
// A shape is either a generator -- rectangle, ellipse, polygon, star -- whose
// parameters keyframe like any other property, or explicit Bezier points that
// can be edited node by node. Nothing is terminal: a generator can be broken
// open into the points it was drawing at any time, and from that moment the
// points are what the shape is.

// One Bezier node: a point with two handles, both relative to the point.
struct ShapeNode {
  QPointF point, in, out;
};
using ShapeContour = QVector<ShapeNode>;

// The path this layer's shape describes at this time.
QPainterPath shapeGeometry(const Layer &, double t);
// The box a generated shape fills, without building it; false when the shape
// has to be built to be measured.
bool shapeGeneratorBounds(const Layer &, double t, QRectF &out);
// Forget what shapes looked like: called when a composition starts on a thread
// (not by the passes drawn inside one, paintComposition) and when the editor
// refreshes, so nothing remembered outlives a change to the scene.
void shapeGeometryForget();
// A partial trim is an open segment even when its source shape is closed.
bool shapeTrimActive(const Layer &, double t);
// Whether the shape is drawn from points rather than from a generator.
bool shapeFromPoints(const Layer &);

// Breaking a generator open. Returns the geometry as points in the layer's own
// coordinates, curves and all, ready to be stored in Layer::shapePoints.
QJsonArray shapePointsFromPath(const QPainterPath &);
QVector<ShapeContour> shapeContours(const QPainterPath &);
QPainterPath shapePathFromContours(const QVector<ShapeContour> &, bool closed);

int checkShapes(QStringList &failures);
