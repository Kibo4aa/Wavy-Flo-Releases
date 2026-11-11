#pragma once
// Expression Controls: named values a layer carries for its expressions (and
// anyone else's) to read -- a Slider, an Angle, a Checkbox, a Point, a Colour,
// a Layer -- as After Effects' Expression Controls effects are, but any number
// of each, named by the person.
//
//   effect("Speed")("Slider")          a control on this layer
//   effect("Speed")(1)                 its first value, by number
//   thisComp.layer("Rig").effect("Target")("Point")
//
// Each keyable part is a curve of the layer's own ("Control/<uid>/<part>"),
// so it keys, shows in the timeline and takes an expression like any other.
// What the controls are -- their names, kinds, order, and a Layer control's
// layer -- is kept in surfaceEffects["Expression Controls"].controls.
#include "engine.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QUuid>
#include <vector>

struct ExpressionControl {
  QString uid, name, kind; // kind: slider, angle, checkbox, point, color, layer
  QString layer;           // a Layer control's layer, by id
};

inline const QStringList &expressionControlKinds() {
  static const QStringList kinds{"slider", "angle", "checkbox", "point", "color", "layer"};
  return kinds;
}
// What the Effects panel and After Effects call each kind.
inline QString expressionControlLabel(const QString &kind) {
  if (kind == "slider") return "Slider Control";
  if (kind == "angle") return "Angle Control";
  if (kind == "checkbox") return "Checkbox Control";
  if (kind == "point") return "Point Control";
  if (kind == "color") return "Color Control";
  if (kind == "layer") return "Layer Control";
  return QString();
}
// The name its one value goes by in effect("...")("<this>").
inline QString expressionControlValueName(const QString &kind) {
  if (kind == "slider") return "Slider";
  if (kind == "angle") return "Angle";
  if (kind == "checkbox") return "Checkbox";
  if (kind == "point") return "Point";
  if (kind == "color") return "Color";
  if (kind == "layer") return "Layer";
  return QString();
}
// Its keyable parts, each a curve.
inline QStringList expressionControlParts(const QString &kind) {
  if (kind == "point") return {"X", "Y"};
  if (kind == "color") return {"R", "G", "B", "A"};
  if (kind == "layer") return {};
  return {expressionControlValueName(kind)};
}
inline QString expressionControlCurve(const QString &uid, const QString &part) { return "Control/" + uid + "/" + part; }

inline std::vector<ExpressionControl> expressionControls(const Layer &l) {
  std::vector<ExpressionControl> out;
  for (const auto &v : l.surfaceEffects.value("Expression Controls").toObject().value("controls").toArray()) {
    const QJsonObject o = v.toObject();
    out.push_back({o.value("uid").toString(), o.value("name").toString(), o.value("kind").toString(), o.value("layer").toString()});
  }
  return out;
}
inline void setExpressionControls(Layer &l, const std::vector<ExpressionControl> &controls) {
  QJsonArray list;
  for (const ExpressionControl &c : controls) {
    QJsonObject o{{"uid", c.uid}, {"name", c.name}, {"kind", c.kind}};
    if (!c.layer.isEmpty()) o["layer"] = c.layer;
    list.append(o);
  }
  if (list.isEmpty()) {
    l.surfaceEffects.remove("Expression Controls");
    return;
  }
  QJsonObject held = l.surfaceEffects.value("Expression Controls").toObject();
  held["controls"] = list;
  l.surfaceEffects["Expression Controls"] = held;
}
// By name, as an expression asks for it (the case not minded).
inline const ExpressionControl *findExpressionControl(const std::vector<ExpressionControl> &controls, const QString &name) {
  for (const ExpressionControl &c : controls)
    if (c.name.compare(name, Qt::CaseInsensitive) == 0) return &c;
  return nullptr;
}
// A name no other control of the layer has: `wanted`, or it with a number.
inline QString freeExpressionControlName(const Layer &l, const QString &wanted) {
  const auto controls = expressionControls(l);
  if (!findExpressionControl(controls, wanted)) return wanted;
  for (int n = 2;; ++n)
    if (!findExpressionControl(controls, QString("%1 %2").arg(wanted).arg(n))) return QString("%1 %2").arg(wanted).arg(n);
}
// A new control at the end of the layer's: its curves made at their first
// values (a Color red, as After Effects starts one). Its uid; empty for a kind
// there is not.
inline QString addExpressionControl(Layer &l, const QString &kind, const QString &name) {
  if (!expressionControlKinds().contains(kind)) return QString();
  ExpressionControl c;
  c.uid = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
  c.kind = kind;
  c.name = freeExpressionControlName(l, name.trimmed().isEmpty() ? expressionControlLabel(kind) : name.trimmed());
  const QStringList parts = expressionControlParts(kind);
  for (int i = 0; i < parts.size(); ++i) {
    double initial = 0;
    if (kind == "color") initial = i == 0 || i == 3 ? 1 : 0;
    layerAddExtra(l, expressionControlCurve(c.uid, parts[i]), initial);
  }
  auto controls = expressionControls(l);
  controls.push_back(c);
  setExpressionControls(l, controls);
  return c.uid;
}
inline bool renameExpressionControl(Layer &l, const QString &uid, const QString &name) {
  auto controls = expressionControls(l);
  for (ExpressionControl &c : controls)
    if (c.uid == uid) {
      if (name.trimmed().isEmpty() || c.name == name.trimmed()) return false;
      c.name = freeExpressionControlName(l, name.trimmed());
      setExpressionControls(l, controls);
      return true;
    }
  return false;
}
inline bool removeExpressionControl(Layer &l, const QString &uid) {
  auto controls = expressionControls(l);
  for (size_t i = 0; i < controls.size(); ++i)
    if (controls[i].uid == uid) {
      for (const QString &part : expressionControlParts(controls[i].kind)) layerRemoveExtra(l, expressionControlCurve(uid, part));
      controls.erase(controls.begin() + std::ptrdiff_t(i));
      setExpressionControls(l, controls);
      return true;
    }
  return false;
}
inline bool setExpressionControlLayer(Layer &l, const QString &uid, const QString &layerId) {
  auto controls = expressionControls(l);
  for (ExpressionControl &c : controls)
    if (c.uid == uid && c.kind == "layer") {
      c.layer = layerId;
      setExpressionControls(l, controls);
      return true;
    }
  return false;
}
// What the timeline calls one of its curves: the control's name, and the
// part's when it has several ("Target X").
inline QString expressionControlCurveName(const Layer &l, const QString &curveId) {
  const QString uid = curveId.section('/', 1, 1), part = curveId.section('/', 2, 2);
  for (const ExpressionControl &c : expressionControls(l))
    if (c.uid == uid) return expressionControlParts(c.kind).size() > 1 ? c.name + " " + part : c.name;
  return part;
}
