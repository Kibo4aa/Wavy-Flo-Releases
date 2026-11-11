#pragma once
// Expressions: a line of script on a property that computes its value instead
// of (or on top of) its keyframes. The language is the part of JavaScript that
// After Effects expressions are written in -- numbers, arrays, variables,
// if/else, Math -- with the After Effects vocabulary on top: time, value,
// wiggle, loopOut, linear, ease, valueAtTime, thisComp.layer("Name")...
//
// A compiled program is never changed after it is built, so any number of
// render threads can evaluate it at once.
#include <QString>
#include <QStringList>
#include <memory>

struct Layer;
struct Scene;
struct Property;
struct ExpressionProgram;

// Builds a program from text. On failure returns null and says why, and where.
std::shared_ptr<const ExpressionProgram> compileExpression(const QString &text, QString *error = nullptr);
// What the program computes for this property at this layer-local frame. When
// it cannot be worked out (a layer it names is gone, a result that is not a
// number) the property's own value is the answer and `error` says what went wrong.
double evaluateExpression(const ExpressionProgram &program, const Property &self, double frame,
                          QString *error = nullptr);
// The scene expressions look layers and the frame rate up in, on this thread.
struct ExpressionSceneScope {
  const Scene *previous;
  explicit ExpressionSceneScope(const Scene *scene);
  ~ExpressionSceneScope();
};
// The layer being drawn, for the copies of it that drawing makes: a copy lives
// outside the scene, so it cannot be found there by where it is.
struct ExpressionLayerScope {
  const Layer *previous;
  explicit ExpressionLayerScope(const Layer *layer);
  ~ExpressionLayerScope();
};
// Whether the text names another layer, which makes a frame depend on more
// than the layer the expression sits on.
bool expressionReachesOtherLayers(const QString &text);
int checkExpressions(QStringList &failures);
