#include "expressions.h"
#include "engine.h"
#include "expression_controls.h"
#include "render_workers.h"
#include <QHash>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

// ---------------------------------------------------------------------------
// Expressions.
//
// Text is read into tokens, the tokens into a tree of nodes held in one flat
// list, and the tree is walked to work the value out. Nothing in a compiled
// program changes after it is built; everything that changes while one runs
// (its variables, how many random numbers it has drawn) lives in the context
// of that one run, so any number of render threads can run it at once.
// ---------------------------------------------------------------------------

namespace {


thread_local int g_depth = 0;
constexpr double kPi = 3.14159265358979323846;
constexpr int kLocals = 32;

// ---- values ---------------------------------------------------------------
struct XValue {
  // EffectRef: an effect (an Expression Control) of a layer, by its name in `text`.
  enum Kind : quint8 { Undefined, Number, Array, Text, LayerRef, Comp, MathObj, TransformRef, PropRef, SelfProp, EffectRef };
  Kind kind = Undefined;
  double n = 0;
  std::array<double, 4> a{0, 0, 0, 0}; // up to four parts: a colour's
  int size = 0;
  QString text;
  int layer = -1;                           // an index in the scene; -2 for the layer the expression is on
  std::array<int, 4> parts{-1, -1, -1, -1}; // for PropRef
  int slotCount = 0;
  static XValue number(double v) { XValue x; x.kind = Number; x.n = v; return x; }
  static XValue array(std::initializer_list<double> v) {
    XValue x; x.kind = Array; x.size = 0;
    for (double e : v) if (x.size < 4) x.a[size_t(x.size++)] = e;
    return x;
  }
};

// ---- tokens ---------------------------------------------------------------
struct Token {
  enum Kind { End, Num, Str, Id, Punct } kind = End;
  double num = 0;
  QString text;
  int pos = 0;
};
bool lex(const QString &src, std::vector<Token> &out, QString *error) {
  static const QStringList puncts{"===", "!==", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=",
                                  "(", ")", "[", "]", "{", "}", ",", ";", ".", "?", ":", "+", "-", "*",
                                  "/", "%", "!", "<", ">", "="};
  int i = 0;
  const int n = src.size();
  while (i < n) {
    const QChar c = src[i];
    if (c.isSpace()) { ++i; continue; }
    if (c == '/' && i + 1 < n && src[i + 1] == '/') { while (i < n && src[i] != '\n') ++i; continue; }
    if (c == '/' && i + 1 < n && src[i + 1] == '*') {
      const int close = src.indexOf("*/", i + 2);
      if (close < 0) { if (error) *error = QString("a comment opened at %1 is never closed").arg(i + 1); return false; }
      i = close + 2; continue;
    }
    Token t; t.pos = i;
    if (c.isDigit() || (c == '.' && i + 1 < n && src[i + 1].isDigit())) {
      int j = i;
      while (j < n && (src[j].isDigit() || src[j] == '.')) ++j;
      if (j < n && (src[j] == 'e' || src[j] == 'E')) {
        int k = j + 1;
        if (k < n && (src[k] == '+' || src[k] == '-')) ++k;
        if (k < n && src[k].isDigit()) { j = k; while (j < n && src[j].isDigit()) ++j; }
      }
      bool ok = false;
      t.kind = Token::Num; t.num = src.mid(i, j - i).toDouble(&ok);
      if (!ok) { if (error) *error = QString("\"%1\" is not a number").arg(src.mid(i, j - i)); return false; }
      out.push_back(t); i = j; continue;
    }
    if (c.isLetter() || c == '_' || c == '$') {
      int j = i;
      while (j < n && (src[j].isLetterOrNumber() || src[j] == '_' || src[j] == '$')) ++j;
      t.kind = Token::Id; t.text = src.mid(i, j - i);
      out.push_back(t); i = j; continue;
    }
    if (c == '"' || c == '\'') {
      int j = i + 1;
      QString text;
      while (j < n && src[j] != c) {
        if (src[j] == '\\' && j + 1 < n) { text += src[j + 1]; j += 2; continue; }
        text += src[j++];
      }
      if (j >= n) { if (error) *error = QString("the text starting at %1 is never closed").arg(i + 1); return false; }
      t.kind = Token::Str; t.text = text;
      out.push_back(t); i = j + 1; continue;
    }
    bool matched = false;
    for (const QString &p : puncts)
      if (src.mid(i, p.size()) == p) {
        t.kind = Token::Punct; t.text = p;
        out.push_back(t); i += int(p.size()); matched = true; break;
      }
    if (!matched) { if (error) *error = QString("\"%1\" at %2 is not understood").arg(c).arg(i + 1); return false; }
  }
  Token end; end.kind = Token::End; end.pos = n;
  out.push_back(end);
  return true;
}

// ---- the tree -------------------------------------------------------------
enum class Op : quint8 {
  Num, Str, Local, Global, Member, Index, Call, Unary, Binary, Logical, Ternary, ArrayLit,
  Assign, Declare, If, Block
};
// The names a program may use without defining them.
enum class G : quint8 {
  Time, Value, Index, InPoint, OutPoint, ThisLayer, ThisComp, ThisProperty, MathObj, Transform,
  Position, Scale, Rotation, Opacity, Anchor, XRotation, YRotation, True, False, Undefined,
  // functions
  Wiggle, Random, GaussRandom, Noise, Linear, Ease, EaseIn, EaseOut, Clamp, LoopOut, LoopIn,
  ValueAtTime, DegToRad, RadToDeg, TimeToFrames, FramesToTime, Length, Add, Sub, Mul, Div, SeedRandom, Effect,
  // Math, also allowed bare
  Abs, Floor, Ceil, Round, Sqrt, Pow, Min, Max, Sin, Cos, Tan, Asin, Acos, Atan, Atan2, Exp, Log, Sign, Trunc, Hypot
};
const QHash<QString, G> &globals() {
  static const QHash<QString, G> names{
      {"time", G::Time}, {"value", G::Value}, {"index", G::Index}, {"inPoint", G::InPoint},
      {"outPoint", G::OutPoint}, {"thisLayer", G::ThisLayer}, {"thisComp", G::ThisComp},
      {"thisProperty", G::ThisProperty}, {"Math", G::MathObj}, {"transform", G::Transform},
      {"position", G::Position}, {"scale", G::Scale}, {"rotation", G::Rotation}, {"opacity", G::Opacity},
      {"anchorPoint", G::Anchor}, {"xRotation", G::XRotation}, {"yRotation", G::YRotation},
      {"zRotation", G::Rotation}, {"true", G::True}, {"false", G::False}, {"undefined", G::Undefined},
      {"wiggle", G::Wiggle}, {"random", G::Random}, {"gaussRandom", G::GaussRandom}, {"noise", G::Noise},
      {"linear", G::Linear}, {"ease", G::Ease}, {"easeIn", G::EaseIn}, {"easeOut", G::EaseOut},
      {"clamp", G::Clamp}, {"loopOut", G::LoopOut}, {"loopIn", G::LoopIn}, {"valueAtTime", G::ValueAtTime},
      {"degreesToRadians", G::DegToRad}, {"radiansToDegrees", G::RadToDeg},
      {"timeToFrames", G::TimeToFrames}, {"framesToTime", G::FramesToTime}, {"length", G::Length},
      {"add", G::Add}, {"sub", G::Sub}, {"mul", G::Mul}, {"div", G::Div}, {"seedRandom", G::SeedRandom},
      {"effect", G::Effect},
      {"abs", G::Abs}, {"floor", G::Floor}, {"ceil", G::Ceil}, {"round", G::Round}, {"sqrt", G::Sqrt},
      {"pow", G::Pow}, {"min", G::Min}, {"max", G::Max}, {"sin", G::Sin}, {"cos", G::Cos}, {"tan", G::Tan},
      {"asin", G::Asin}, {"acos", G::Acos}, {"atan", G::Atan}, {"atan2", G::Atan2}, {"exp", G::Exp},
      {"log", G::Log}, {"sign", G::Sign}, {"trunc", G::Trunc}, {"hypot", G::Hypot}};
  return names;
}
bool isFunction(G g) { return g >= G::Wiggle; }
bool isMathFunction(G g) { return g >= G::Abs; }

struct Node {
  Op op = Op::Num;
  double num = 0;
  QString text;   // a name, an operator, a string
  int slot = -1;  // a local's place, or the global's number
  int a = -1, b = -1, c = -1;
  std::vector<int> kids;
  int pos = 0;
};
}  // namespace

struct ExpressionProgram {
  QString source;
  std::vector<Node> nodes;
  std::vector<int> statements;
  int locals = 0;
  bool usesRandom = false;
};

namespace {
// ---- parsing --------------------------------------------------------------
struct Parser {
  std::vector<Token> tokens;
  size_t at = 0;
  ExpressionProgram *program = nullptr;
  QHash<QString, int> localSlots;
  QString error;

  const Token &peek(size_t ahead = 0) const { return tokens[std::min(at + ahead, tokens.size() - 1)]; }
  bool isPunct(const char *p, size_t ahead = 0) const {
    return peek(ahead).kind == Token::Punct && peek(ahead).text == QLatin1String(p);
  }
  bool isWord(const char *w) const { return peek().kind == Token::Id && peek().text == QLatin1String(w); }
  bool accept(const char *p) { if (isPunct(p)) { ++at; return true; } return false; }
  bool fail(const QString &why) {
    if (error.isEmpty()) error = QString("%1 (at %2)").arg(why).arg(peek().pos + 1);
    return false;
  }
  bool expect(const char *p) {
    if (accept(p)) return true;
    return fail(QString("\"%1\" was expected%2").arg(QLatin1String(p),
        peek().kind == Token::End ? QString(" before the end") : QString(" instead of \"%1\"").arg(peek().text)));
  }
  int add(Node node) { program->nodes.push_back(std::move(node)); return int(program->nodes.size()) - 1; }
  int local(const QString &name, bool create) {
    auto it = localSlots.find(name);
    if (it != localSlots.end()) return it.value();
    if (!create) return -1;
    if (program->locals >= kLocals) { fail("more variables than an expression may have"); return -1; }
    localSlots.insert(name, program->locals);
    return program->locals++;
  }

  int statement() {
    if (isWord("var") || isWord("let") || isWord("const")) {
      ++at;
      if (peek().kind != Token::Id) { fail("a name was expected after the declaration"); return -1; }
      const QString name = peek().text; ++at;
      Node n; n.op = Op::Declare; n.pos = peek().pos;
      n.slot = local(name, true);
      if (n.slot < 0) return -1;
      if (accept("=")) { n.a = expression(); if (n.a < 0) return -1; }
      accept(";");
      return add(n);
    }
    if (isWord("if")) {
      ++at;
      Node n; n.op = Op::If; n.pos = peek().pos;
      if (!expect("(")) return -1;
      n.a = expression(); if (n.a < 0) return -1;
      if (!expect(")")) return -1;
      n.b = statement(); if (n.b < 0) return -1;
      if (isWord("else")) { ++at; n.c = statement(); if (n.c < 0) return -1; }
      return add(n);
    }
    if (isPunct("{")) {
      ++at;
      Node n; n.op = Op::Block;
      while (!isPunct("}")) {
        if (peek().kind == Token::End) { fail("a block is never closed with \"}\""); return -1; }
        const int s = statement(); if (s < 0) return -1;
        n.kids.push_back(s);
      }
      ++at;
      return add(n);
    }
    const int e = expression();
    if (e < 0) return -1;
    accept(";");
    return e;
  }
  int expression() { return assignment(); }
  int assignment() {
    if (peek().kind == Token::Id && (isPunct("=", 1) || isPunct("+=", 1) || isPunct("-=", 1) ||
                                    isPunct("*=", 1) || isPunct("/=", 1))) {
      const QString name = peek().text;
      if (globals().contains(name)) { fail(QString("\"%1\" is a name of the program's own and cannot be set").arg(name)); return -1; }
      const QString op = peek(1).text;
      Node n; n.op = Op::Assign; n.pos = peek().pos; n.text = op;
      const bool plain = op == "=";
      n.slot = local(name, plain);
      if (n.slot < 0) { if (error.isEmpty()) fail(QString("\"%1\" is used before it is given a value").arg(name)); return -1; }
      at += 2;
      n.a = assignment(); if (n.a < 0) return -1;
      return add(n);
    }
    return ternary();
  }
  int ternary() {
    const int cond = logicalOr(); if (cond < 0) return -1;
    if (!accept("?")) return cond;
    Node n; n.op = Op::Ternary; n.a = cond;
    n.b = assignment(); if (n.b < 0) return -1;
    if (!expect(":")) return -1;
    n.c = assignment(); if (n.c < 0) return -1;
    return add(n);
  }
  int logicalOr() {
    int left = logicalAnd(); if (left < 0) return -1;
    while (isPunct("||")) {
      ++at; Node n; n.op = Op::Logical; n.text = "||"; n.a = left;
      n.b = logicalAnd(); if (n.b < 0) return -1;
      left = add(n);
    }
    return left;
  }
  int logicalAnd() {
    int left = equality(); if (left < 0) return -1;
    while (isPunct("&&")) {
      ++at; Node n; n.op = Op::Logical; n.text = "&&"; n.a = left;
      n.b = equality(); if (n.b < 0) return -1;
      left = add(n);
    }
    return left;
  }
  int binaryLevel(int (Parser::*next)(), std::initializer_list<const char *> ops) {
    int left = (this->*next)(); if (left < 0) return -1;
    for (;;) {
      const char *found = nullptr;
      for (const char *op : ops) if (isPunct(op)) { found = op; break; }
      if (!found) return left;
      Node n; n.op = Op::Binary; n.text = QLatin1String(found); n.pos = peek().pos; n.a = left;
      ++at;
      n.b = (this->*next)(); if (n.b < 0) return -1;
      left = add(n);
    }
  }
  int equality() { return binaryLevel(&Parser::relational, {"===", "!==", "==", "!="}); }
  int relational() { return binaryLevel(&Parser::additive, {"<=", ">=", "<", ">"}); }
  int additive() { return binaryLevel(&Parser::multiplicative, {"+", "-"}); }
  int multiplicative() { return binaryLevel(&Parser::unary, {"*", "/", "%"}); }
  int unary() {
    if (isPunct("-") || isPunct("+") || isPunct("!")) {
      Node n; n.op = Op::Unary; n.text = peek().text; n.pos = peek().pos; ++at;
      n.a = unary(); if (n.a < 0) return -1;
      return add(n);
    }
    return postfix();
  }
  int postfix() {
    int node = primary(); if (node < 0) return -1;
    for (;;) {
      if (accept(".")) {
        if (peek().kind != Token::Id) { fail("a name was expected after \".\""); return -1; }
        Node n; n.op = Op::Member; n.a = node; n.text = peek().text; n.pos = peek().pos; ++at;
        node = add(n);
      } else if (accept("[")) {
        Node n; n.op = Op::Index; n.a = node;
        n.b = expression(); if (n.b < 0) return -1;
        if (!expect("]")) return -1;
        node = add(n);
      } else if (accept("(")) {
        Node n; n.op = Op::Call; n.a = node; n.pos = peek().pos;
        if (!isPunct(")")) {
          for (;;) {
            const int arg = assignment(); if (arg < 0) return -1;
            n.kids.push_back(arg);
            if (!accept(",")) break;
          }
        }
        if (!expect(")")) return -1;
        if (n.kids.size() > 8) { fail("too many things handed to one function"); return -1; }
        node = add(n);
      } else {
        return node;
      }
    }
  }
  int primary() {
    const Token t = peek();
    if (t.kind == Token::Num) { ++at; Node n; n.op = Op::Num; n.num = t.num; n.pos = t.pos; return add(n); }
    if (t.kind == Token::Str) { ++at; Node n; n.op = Op::Str; n.text = t.text; n.pos = t.pos; return add(n); }
    if (t.kind == Token::Id) {
      ++at;
      Node n; n.pos = t.pos;
      const int slot = local(t.text, false);
      if (slot >= 0) { n.op = Op::Local; n.slot = slot; return add(n); }
      auto g = globals().find(t.text);
      if (g == globals().end()) { at--; fail(QString("\"%1\" is not a name the program knows").arg(t.text)); return -1; }
      n.op = Op::Global; n.slot = int(g.value()); n.text = t.text;
      if (g.value() == G::Random || g.value() == G::GaussRandom) program->usesRandom = true;
      return add(n);
    }
    if (accept("(")) {
      const int inner = expression(); if (inner < 0) return -1;
      if (!expect(")")) return -1;
      return inner;
    }
    if (accept("[")) {
      Node n; n.op = Op::ArrayLit; n.pos = t.pos;
      if (!isPunct("]")) {
        for (;;) {
          const int e = assignment(); if (e < 0) return -1;
          n.kids.push_back(e);
          if (!accept(",")) break;
        }
      }
      if (!expect("]")) return -1;
      if (n.kids.size() > 3) { fail("an array here holds at most three numbers"); return -1; }
      return add(n);
    }
    fail(t.kind == Token::End ? QString("the expression ends too soon") : QString("\"%1\" was not expected here").arg(t.text));
    return -1;
  }
};

// ---- evaluation -----------------------------------------------------------
// An expression works in the units the timeline shows: Scale and Opacity in
// percent, as After Effects writes them, though they are kept as fractions.
double shown(int slot, double v) { return slot >= 0 && percentProperty(slot) ? v * 100 : v; }
double stored(int slot, double v) { return slot >= 0 && percentProperty(slot) ? v / 100 : v; }
quint32 mix(quint32 h, quint32 v) {
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h;
}
quint32 finalise(quint32 h) {
  h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
  return h;
}
double unitOf(quint32 h) { return double(finalise(h) & 0xffffffu) / 16777216.0; }
// Smooth noise in -1..1: a random height at every whole step, eased between.
double smoothNoise(quint32 stream, double x) {
  const double floorX = std::floor(x);
  const qint64 i = qint64(floorX);
  const double f = x - floorX;
  const double v0 = unitOf(mix(stream, quint32(i))) * 2 - 1;
  const double v1 = unitOf(mix(stream, quint32(i + 1))) * 2 - 1;
  const double s = f * f * (3 - 2 * f);
  return v0 + (v1 - v0) * s;
}

struct Context {
  const ExpressionProgram *program = nullptr;
  const Property *self = nullptr;
  const Layer *owner = nullptr;
  int ownerIndex = -1;
  const Scene *scene = nullptr;
  double frame = 0, compFrame = 0, fps = 30;
  int dimension = -1, groupSize = 0;
  std::array<int, 3> group{-1, -1, -1};
  std::array<XValue, kLocals> locals;
  quint32 seed = 0;
  bool timeless = false;
  int randomCalls = 0;
  QString error;
  bool failed = false;
};

XValue failWith(Context &c, const QString &why) {
  if (!c.failed) { c.failed = true; c.error = why; }
  return {};
}

// The three properties that make one value in After Effects' terms: a
// position, a scale, an anchor point. Each is a separate curve here.
std::array<int, 3> groupOf(int slot, int *dimension) {
  const std::array<std::array<int, 3>, 3> groups{{{0, 1, PositionZ}, {2, 3, ScaleZ}, {AnchorX, AnchorY, AnchorZ}}};
  for (const auto &g : groups)
    for (int d = 0; d < 3; ++d)
      if (g[size_t(d)] == slot) { *dimension = d; return g; }
  *dimension = -1;
  return {slot, -1, -1};
}
int groupSizeFor(const Layer *owner, int dimension) {
  if (dimension < 0) return 1;
  return owner && owner->threeD ? 3 : 2;
}

const Layer *layerFor(const Context &c, int ref) {
  if (ref == -2) return c.owner;
  if (!c.scene || ref < 0 || ref >= int(c.scene->layers.size())) return nullptr;
  return &c.scene->layers[size_t(ref)];
}
// The property named on a layer, as the parts it reads.
bool propertySlots(const QString &name, std::array<int, 4> &parts, int &count) {
  count = 1;
  if (name == "position") { parts = {0, 1, PositionZ, -1}; count = 2; return true; }
  if (name == "scale") { parts = {2, 3, ScaleZ, -1}; count = 2; return true; }
  if (name == "anchorPoint") { parts = {AnchorX, AnchorY, AnchorZ, -1}; count = 2; return true; }
  if (name == "rotation" || name == "zRotation") { parts = {4, -1, -1, -1}; return true; }
  if (name == "xRotation") { parts = {RotationX, -1, -1, -1}; return true; }
  if (name == "yRotation") { parts = {RotationY, -1, -1, -1}; return true; }
  if (name == "opacity") { parts = {5, -1, -1, -1}; return true; }
  for (int i = 0; i < PropertyCount; ++i)
    if (name == QLatin1String(propKeys[i])) { parts = {i, -1, -1, -1}; return true; }
  return false;
}
XValue propRef(int layer, const std::array<int, 4> &parts, int count, const Layer *l) {
  XValue v; v.kind = XValue::PropRef; v.layer = layer; v.parts = parts;
  v.slotCount = count == 2 && l && l->threeD ? 3 : count;
  return v;
}
// A property on some layer, read at a composition frame, expression and all.
XValue readProperty(const Context &c, const XValue &ref, double compFrame) {
  const Layer *l = layerFor(c, ref.layer);
  if (!l) return {};
  const double local = compFrame - l->start;
  // A fixed property or one of the layer's own curves (an Expression Control's).
  if (ref.slotCount <= 1) return XValue::number(shown(ref.parts[0], l->at(ref.parts[0]).at(local)));
  XValue v; v.kind = XValue::Array; v.size = std::min(ref.slotCount, 4);
  for (int d = 0; d < v.size; ++d) {
    const int slot = ref.parts[size_t(d)];
    v.a[size_t(d)] = shown(slot, l->at(slot).at(local));
  }
  return v;
}
// This property's own value at a layer frame, before the expression: a number,
// or the whole position (scale, anchor) it is one part of.
XValue ownValue(const Context &c, double frame) {
  if (c.dimension < 0 || !c.owner) return XValue::number(shown(c.self->slot, c.self->keyedAt(frame)));
  XValue v; v.kind = XValue::Array; v.size = c.groupSize;
  for (int d = 0; d < c.groupSize; ++d) {
    const int slot = c.group[size_t(d)];
    v.a[size_t(d)] = shown(slot, slot == c.self->slot ? c.self->keyedAt(frame) : c.owner->p[size_t(slot)].keyedAt(frame));
  }
  return v;
}
XValue realise(const Context &c, const XValue &v) {
  if (v.kind == XValue::PropRef) return readProperty(c, v, c.compFrame);
  if (v.kind == XValue::SelfProp) return ownValue(c, c.frame);
  return v;
}
bool asNumber(const XValue &v, double &out) {
  if (v.kind == XValue::Number) { out = v.n; return true; }
  if (v.kind == XValue::Array && v.size > 0) { out = v.a[0]; return true; }
  return false;
}
bool truthy(const XValue &v) {
  switch (v.kind) {
    case XValue::Number: return v.n != 0 && !std::isnan(v.n);
    case XValue::Text: return !v.text.isEmpty();
    case XValue::Undefined: return false;
    default: return true;
  }
}
// Arithmetic that works on numbers and arrays alike: an array with a number
// takes the number to every part, two arrays go part by part.
template <class F> XValue combine(Context &c, const XValue &x, const XValue &y, F f, const QString &op) {
  if (x.kind == XValue::Number && y.kind == XValue::Number) return XValue::number(f(x.n, y.n));
  if ((x.kind == XValue::Array || x.kind == XValue::Number) && (y.kind == XValue::Array || y.kind == XValue::Number)) {
    XValue r; r.kind = XValue::Array;
    r.size = std::max(x.kind == XValue::Array ? x.size : 0, y.kind == XValue::Array ? y.size : 0);
    for (int d = 0; d < r.size; ++d) {
      const double a = x.kind == XValue::Number ? x.n : (d < x.size ? x.a[size_t(d)] : 0);
      const double b = y.kind == XValue::Number ? y.n : (d < y.size ? y.a[size_t(d)] : 0);
      r.a[size_t(d)] = f(a, b);
    }
    return r;
  }
  return failWith(c, QString("\"%1\" needs numbers or arrays on both sides").arg(op));
}
template <class F> XValue each(Context &c, const XValue &x, F f, const QString &what) {
  if (x.kind == XValue::Number) return XValue::number(f(x.n));
  if (x.kind == XValue::Array) { XValue r = x; for (int d = 0; d < r.size; ++d) r.a[size_t(d)] = f(r.a[size_t(d)]); return r; }
  return failWith(c, QString("%1 needs a number").arg(what));
}

double loopValue(const Property &p, double frame, const QString &type, int count, bool out) {
  std::vector<double> times;
  for (const auto &k : p.keys) times.push_back(k.frame);
  std::sort(times.begin(), times.end());
  if (times.size() < 2) return p.keyedAt(frame);
  double first = times.front(), last = times.back();
  if (count > 0 && size_t(count) < times.size()) {
    if (out) first = times[times.size() - 1 - size_t(count)];
    else last = times[size_t(count)];
  }
  const double span = last - first;
  if (span <= 1e-9) return p.keyedAt(frame);
  if ((out && frame <= last) || (!out && frame >= first)) return p.keyedAt(frame);
  if (type == "continue") {
    if (out) return p.keyedAt(last) + (frame - last) * (p.keyedAt(last) - p.keyedAt(last - 1));
    return p.keyedAt(first) - (first - frame) * (p.keyedAt(first + 1) - p.keyedAt(first));
  }
  const double cycles = std::floor((frame - first) / span);
  double within = std::fmod(frame - first, span);
  if (within < 0) within += span;
  if (type == "pingpong") {
    const bool forward = std::fmod(std::abs(cycles), 2.) < .5;
    return forward ? p.keyedAt(first + within) : p.keyedAt(last - within);
  }
  double value = p.keyedAt(first + within);
  if (type == "offset") value += cycles * (p.keyedAt(last) - p.keyedAt(first));
  return value;
}

XValue evaluate(Context &c, int index);

// effect("Name")("Parameter"), effect("Name")(1): an Expression Control's
// value (its curves, read at the frame, expression and all), or an effect's
// setting as the layer keeps it -- its curve when it is keyed.
XValue effectParameter(Context &c, const XValue &effect, const std::vector<XValue> &args) {
  const Layer *l = layerFor(c, effect.layer);
  if (!l) return failWith(c, "that layer is no longer there");
  const XValue which = args.empty() ? XValue::number(1) : args[0];
  const auto controls = expressionControls(*l);
  if (const ExpressionControl *control = findExpressionControl(controls, effect.text)) {
    const QString valueName = expressionControlValueName(control->kind);
    double number = 0;
    const bool byNumber = asNumber(which, number);
    if ((byNumber && std::lround(number) != 1) ||
        (which.kind == XValue::Text && which.text.compare(valueName, Qt::CaseInsensitive) != 0))
      return failWith(c, QString("\"%1\" (%2) has only \"%3\"").arg(control->name, expressionControlLabel(control->kind), valueName));
    if (control->kind == "layer") {
      if (!c.scene) return failWith(c, "there is no composition to ask");
      for (size_t i = 0; i < c.scene->layers.size(); ++i)
        if (c.scene->layers[i].id == control->layer) { XValue r; r.kind = XValue::LayerRef; r.layer = int(i); return r; }
      return failWith(c, QString("the Layer control \"%1\" has no layer").arg(control->name));
    }
    const QStringList parts = expressionControlParts(control->kind);
    XValue ref; ref.kind = XValue::PropRef; ref.layer = effect.layer;
    ref.slotCount = int(parts.size());
    for (int i = 0; i < parts.size() && i < 4; ++i) {
      const int at = layerFindExtra(*l, expressionControlCurve(control->uid, parts[i]));
      if (at < 0) return failWith(c, QString("the control \"%1\" has lost its values").arg(control->name));
      ref.parts[size_t(i)] = at;
    }
    return ref;
  }
  // Any other effect's setting, by its name.
  if (which.kind != XValue::Text) return failWith(c, QString("effect(\"%1\") is not an Expression Control: ask for a setting by its name").arg(effect.text));
  const int curve = layerFindExtra(*l, effect.text + "/" + which.text);
  if (curve >= 0) {
    XValue ref; ref.kind = XValue::PropRef; ref.layer = effect.layer; ref.slotCount = 1; ref.parts[0] = curve;
    return ref;
  }
  const QJsonValue stored = l->surfaceEffects.value(effect.text).toObject().value(which.text);
  if (stored.isDouble() || stored.isBool()) return XValue::number(stored.toDouble(stored.toBool()));
  if (stored.isArray()) {
    XValue r; r.kind = XValue::Array; r.size = 0;
    for (const auto &v : stored.toArray()) if (r.size < 4) r.a[size_t(r.size++)] = v.toDouble();
    return r;
  }
  if (effect.text == "Gaussian Blur") return XValue::number(l->blur);
  if (effect.text == "Glow") return XValue::number(l->glow);
  if (effect.text == "Drop Shadow") return XValue::number(l->shadow);
  return failWith(c, QString("the layer has no effect or control \"%1\" with \"%2\"").arg(effect.text, which.text));
}

XValue callFunction(Context &c, G g, const std::vector<XValue> &args, const QString &name) {
  auto num = [&](size_t i, double fallback) {
    double v = fallback;
    if (i < args.size()) asNumber(args[i], v);
    return v;
  };
  auto arg = [&](size_t i) -> XValue { return i < args.size() ? args[i] : XValue(); };
  const double seconds = c.compFrame / c.fps;
  switch (g) {
    case G::Wiggle: {
      const double freq = num(0, 1), amp = num(1, 0), ampMult = num(3, .5), t = num(4, seconds);
      const int octaves = std::clamp(int(num(2, 1)), 1, 10);
      const XValue base = ownValue(c, c.frame);
      const quint32 ownerSeed = c.owner ? quint32(qHash(c.owner->id)) : 0u;
      auto wig = [&](int dimension) {
        double sum = 0, a = amp, f = freq;
        for (int o = 0; o < octaves; ++o) {
          const quint32 stream = mix(mix(mix(ownerSeed, quint32(c.group[0] + 7)), quint32(dimension)), quint32(o));
          sum += smoothNoise(stream, t * f) * a;
          a *= ampMult; f *= 2;
        }
        return sum;
      };
      if (base.kind == XValue::Array) {
        XValue r = base;
        for (int d = 0; d < r.size; ++d) r.a[size_t(d)] += wig(d);
        return r;
      }
      return XValue::number(base.n + wig(0));
    }
    case G::SeedRandom: {
      c.seed = quint32(qint64(num(0, 0)));
      c.timeless = args.size() > 1 && truthy(args[1]);
      c.randomCalls = 0;
      return XValue::number(0);
    }
    case G::Random:
    case G::GaussRandom: {
      auto draw = [&]() {
        const quint32 ownerSeed = c.owner ? quint32(qHash(c.owner->id)) : 0u;
        quint32 h = mix(mix(ownerSeed, quint32(c.self->slot + 101)), c.seed);
        if (!c.timeless) h = mix(h, quint32(qint64(std::floor(c.compFrame))));
        h = mix(h, quint32(c.randomCalls++));
        if (g == G::Random) return unitOf(h);
        // A Gaussian with most of its draws inside 0..1, as After Effects has it.
        const double u1 = std::max(1e-9, unitOf(h)), u2 = unitOf(mix(h, 0x5bd1e995u));
        return .5 + std::sqrt(-2 * std::log(u1)) * std::cos(2 * kPi * u2) / 6.;
      };
      if (args.empty()) return XValue::number(draw());
      if (args.size() == 1) {
        const XValue m = args[0];
        if (m.kind == XValue::Array) { XValue r = m; for (int d = 0; d < r.size; ++d) r.a[size_t(d)] *= draw(); return r; }
        return XValue::number(num(0, 1) * draw());
      }
      const XValue lo = args[0], hi = args[1];
      if (lo.kind == XValue::Array || hi.kind == XValue::Array) {
        return combine(c, lo, hi, [&](double a, double b) { return a + (b - a) * draw(); }, "random");
      }
      const double a = num(0, 0), b = num(1, 1);
      return XValue::number(a + (b - a) * draw());
    }
    case G::Noise: return XValue::number(smoothNoise(0x2545f491u, num(0, 0)));
    case G::Effect: {
      if (args.empty() || args[0].kind != XValue::Text) return failWith(c, "effect() needs the name of an effect or a control");
      XValue r; r.kind = XValue::EffectRef; r.layer = -2; r.text = args[0].text;
      return r;
    }
    case G::Linear: case G::Ease: case G::EaseIn: case G::EaseOut: {
      double t = num(0, 0), tMin = 0, tMax = 1;
      XValue v1, v2;
      if (args.size() >= 5) { tMin = num(1, 0); tMax = num(2, 1); v1 = args[3]; v2 = args[4]; }
      else if (args.size() >= 3) { v1 = args[1]; v2 = args[2]; }
      else return failWith(c, QString("%1 needs a time and two values").arg(name));
      double u = tMax == tMin ? (t >= tMax ? 1 : 0) : (t - tMin) / (tMax - tMin);
      u = std::clamp(u, 0., 1.);
      if (g == G::Ease) u = u * u * (3 - 2 * u);
      else if (g == G::EaseIn) u = u * u;
      else if (g == G::EaseOut) u = 1 - (1 - u) * (1 - u);
      return combine(c, v1, v2, [u](double a, double b) { return a + (b - a) * u; }, name);
    }
    case G::Clamp: {
      const double lo = num(1, -1e300), hi = num(2, 1e300);
      return each(c, arg(0), [&](double v) { return std::clamp(v, std::min(lo, hi), std::max(lo, hi)); }, "clamp");
    }
    case G::LoopOut: case G::LoopIn: {
      QString type = "cycle";
      if (!args.empty() && args[0].kind == XValue::Text) type = args[0].text;
      if (type != "cycle" && type != "pingpong" && type != "offset" && type != "continue")
        return failWith(c, QString("%1 knows cycle, pingpong, offset and continue, not \"%2\"").arg(name, type));
      return XValue::number(shown(c.self->slot, loopValue(*c.self, c.frame, type, int(num(1, 0)), g == G::LoopOut)));
    }
    case G::ValueAtTime: {
      const double at = num(0, seconds) * c.fps - (c.owner ? c.owner->start : 0);
      return ownValue(c, at);
    }
    case G::DegToRad: return each(c, arg(0), [](double v) { return v * kPi / 180; }, name);
    case G::RadToDeg: return each(c, arg(0), [](double v) { return v * 180 / kPi; }, name);
    case G::TimeToFrames: return XValue::number(num(0, seconds) * num(1, c.fps));
    case G::FramesToTime: return XValue::number(num(0, c.compFrame) / num(1, c.fps));
    case G::Length: {
      XValue a = arg(0);
      if (args.size() > 1) a = combine(c, args[0], args[1], [](double x, double y) { return x - y; }, "length");
      if (a.kind == XValue::Number) return XValue::number(std::abs(a.n));
      if (a.kind != XValue::Array) return failWith(c, "length needs an array");
      double s = 0;
      for (int d = 0; d < a.size; ++d) s += a.a[size_t(d)] * a.a[size_t(d)];
      return XValue::number(std::sqrt(s));
    }
    case G::Add: return combine(c, arg(0), arg(1), [](double a, double b) { return a + b; }, name);
    case G::Sub: return combine(c, arg(0), arg(1), [](double a, double b) { return a - b; }, name);
    case G::Mul: return combine(c, arg(0), arg(1), [](double a, double b) { return a * b; }, name);
    case G::Div: return combine(c, arg(0), arg(1), [](double a, double b) { return a / b; }, name);
    case G::Abs: return each(c, arg(0), [](double v) { return std::abs(v); }, name);
    case G::Floor: return each(c, arg(0), [](double v) { return std::floor(v); }, name);
    case G::Ceil: return each(c, arg(0), [](double v) { return std::ceil(v); }, name);
    case G::Round: return each(c, arg(0), [](double v) { return std::floor(v + .5); }, name);
    case G::Trunc: return each(c, arg(0), [](double v) { return std::trunc(v); }, name);
    case G::Sqrt: return each(c, arg(0), [](double v) { return std::sqrt(v); }, name);
    case G::Sin: return each(c, arg(0), [](double v) { return std::sin(v); }, name);
    case G::Cos: return each(c, arg(0), [](double v) { return std::cos(v); }, name);
    case G::Tan: return each(c, arg(0), [](double v) { return std::tan(v); }, name);
    case G::Asin: return each(c, arg(0), [](double v) { return std::asin(v); }, name);
    case G::Acos: return each(c, arg(0), [](double v) { return std::acos(v); }, name);
    case G::Atan: return each(c, arg(0), [](double v) { return std::atan(v); }, name);
    case G::Exp: return each(c, arg(0), [](double v) { return std::exp(v); }, name);
    case G::Log: return each(c, arg(0), [](double v) { return std::log(v); }, name);
    case G::Sign: return each(c, arg(0), [](double v) { return double((v > 0) - (v < 0)); }, name);
    case G::Pow: return XValue::number(std::pow(num(0, 0), num(1, 1)));
    case G::Atan2: return XValue::number(std::atan2(num(0, 0), num(1, 1)));
    case G::Hypot: {
      double s = 0;
      for (size_t i = 0; i < args.size(); ++i) { const double v = num(i, 0); s += v * v; }
      return XValue::number(std::sqrt(s));
    }
    case G::Min: case G::Max: {
      if (args.empty()) return failWith(c, QString("%1 needs at least one number").arg(name));
      double r = num(0, 0);
      for (size_t i = 1; i < args.size(); ++i) r = g == G::Min ? std::min(r, num(i, r)) : std::max(r, num(i, r));
      return XValue::number(r);
    }
    default: break;
  }
  return failWith(c, QString("\"%1\" cannot be called").arg(name));
}

XValue member(Context &c, const XValue &object, const QString &name) {
  switch (object.kind) {
    case XValue::MathObj: {
      if (name == "PI") return XValue::number(kPi);
      if (name == "E") return XValue::number(std::exp(1.));
      if (name == "SQRT2") return XValue::number(std::sqrt(2.));
      if (name == "LN2") return XValue::number(std::log(2.));
      if (name == "LN10") return XValue::number(std::log(10.));
      break;
    }
    case XValue::Comp: {
      if (!c.scene) return failWith(c, "there is no composition to ask");
      if (name == "width") return XValue::number(c.scene->width);
      if (name == "height") return XValue::number(c.scene->height);
      if (name == "duration") return XValue::number(c.scene->frames / c.fps);
      if (name == "frameDuration") return XValue::number(1. / c.fps);
      if (name == "numLayers") return XValue::number(double(c.scene->layers.size()));
      break;
    }
    case XValue::LayerRef:
    case XValue::TransformRef: {
      const Layer *l = layerFor(c, object.layer);
      if (!l) return failWith(c, "that layer is no longer there");
      std::array<int, 4> parts; int count = 0;
      if (propertySlots(name, parts, count)) return propRef(object.layer, parts, count, l);
      if (object.kind == XValue::LayerRef) {
        if (name == "transform") { XValue t = object; t.kind = XValue::TransformRef; return t; }
        if (name == "index") {
          const int i = object.layer == -2 ? c.ownerIndex : object.layer;
          return XValue::number(i + 1);
        }
        if (name == "inPoint" || name == "startTime") return XValue::number(l->start / c.fps);
        if (name == "outPoint") return XValue::number((l->start + l->duration) / c.fps);
        if (name == "name") { XValue t; t.kind = XValue::Text; t.text = l->name; return t; }
      }
      return failWith(c, QString("a layer has no \"%1\"").arg(name));
    }
    case XValue::Array: {
      if (name == "length") return XValue::number(object.size);
      break;
    }
    default: break;
  }
  return failWith(c, QString("\"%1\" is not something that can be asked for here").arg(name));
}

XValue callMethod(Context &c, const XValue &object, const QString &name, const std::vector<XValue> &args) {
  if (object.kind == XValue::MathObj) {
    auto g = globals().find(name);
    if (g != globals().end() && isMathFunction(g.value())) return callFunction(c, g.value(), args, "Math." + name);
    return failWith(c, QString("Math has no \"%1\"").arg(name));
  }
  if (object.kind == XValue::Comp && name == "layer") {
    if (!c.scene) return failWith(c, "there is no composition to ask");
    if (args.empty()) return failWith(c, "layer() needs a name or a number");
    if (args[0].kind == XValue::Text) {
      for (size_t i = 0; i < c.scene->layers.size(); ++i)
        if (c.scene->layers[i].name == args[0].text) { XValue r; r.kind = XValue::LayerRef; r.layer = int(i); return r; }
      return failWith(c, QString("there is no layer named \"%1\"").arg(args[0].text));
    }
    double number = 0;
    if (!asNumber(args[0], number)) return failWith(c, "layer() needs a name or a number");
    const int i = int(std::lround(number)) - 1;
    if (i < 0 || i >= int(c.scene->layers.size())) return failWith(c, QString("there is no layer %1").arg(i + 1));
    XValue r; r.kind = XValue::LayerRef; r.layer = i; return r;
  }
  if (object.kind == XValue::LayerRef && name == "effect") {
    if (args.empty() || args[0].kind != XValue::Text) return failWith(c, "effect() needs the name of an effect or a control");
    XValue r; r.kind = XValue::EffectRef; r.layer = object.layer; r.text = args[0].text;
    return r;
  }
  if (object.kind == XValue::EffectRef && name == "param") return effectParameter(c, object, args);
  if (object.kind == XValue::PropRef && name == "valueAtTime") {
    double seconds = c.compFrame / c.fps;
    if (!args.empty()) asNumber(args[0], seconds);
    return readProperty(c, object, seconds * c.fps);
  }
  if (object.kind == XValue::SelfProp && name == "valueAtTime")
    return callFunction(c, G::ValueAtTime, args, name);
  if (object.kind == XValue::SelfProp && (name == "loopOut" || name == "loopIn" || name == "wiggle"))
    return callFunction(c, globals().value(name), args, name);
  return failWith(c, QString("\"%1\" cannot be called here").arg(name));
}

XValue evaluate(Context &c, int index) {
  if (c.failed || index < 0) return {};
  const Node &n = c.program->nodes[size_t(index)];
  switch (n.op) {
    case Op::Num: return XValue::number(n.num);
    case Op::Str: { XValue v; v.kind = XValue::Text; v.text = n.text; return v; }
    case Op::Local: return c.locals[size_t(n.slot)];
    case Op::Global: {
      const G g = G(n.slot);
      switch (g) {
        case G::Time: return XValue::number(c.compFrame / c.fps);
        case G::Value: return ownValue(c, c.frame);
        case G::Index: return XValue::number(c.ownerIndex + 1);
        case G::InPoint: return XValue::number((c.owner ? c.owner->start : 0) / c.fps);
        case G::OutPoint: return XValue::number((c.owner ? c.owner->start + c.owner->duration : 0) / c.fps);
        case G::ThisLayer: { XValue v; v.kind = XValue::LayerRef; v.layer = -2; return v; }
        case G::Transform: { XValue v; v.kind = XValue::TransformRef; v.layer = -2; return v; }
        case G::ThisComp: { XValue v; v.kind = XValue::Comp; return v; }
        case G::ThisProperty: { XValue v; v.kind = XValue::SelfProp; return v; }
        case G::MathObj: { XValue v; v.kind = XValue::MathObj; return v; }
        case G::True: return XValue::number(1);
        case G::False: return XValue::number(0);
        case G::Undefined: return {};
        case G::Position: case G::Scale: case G::Rotation: case G::Opacity: case G::Anchor:
        case G::XRotation: case G::YRotation: {
          XValue layer; layer.kind = XValue::LayerRef; layer.layer = -2;
          return member(c, layer, n.text == "zRotation" ? QString("rotation") : n.text);
        }
        default:
          return failWith(c, QString("\"%1\" is a function: it needs ( ) after it").arg(n.text));
      }
    }
    case Op::Member: {
      const XValue object = evaluate(c, n.a);
      if (c.failed) return {};
      return member(c, object, n.text);
    }
    case Op::Index: {
      const XValue object = realise(c, evaluate(c, n.a));
      double at = 0;
      if (!asNumber(realise(c, evaluate(c, n.b)), at)) return failWith(c, "an index has to be a number");
      if (c.failed) return {};
      const int i = int(std::lround(at));
      if (object.kind == XValue::Array) {
        if (i < 0 || i >= object.size) return XValue::number(0);
        return XValue::number(object.a[size_t(i)]);
      }
      if (object.kind == XValue::Number) return object; // a lone number stands for every part
      return failWith(c, "only an array can be indexed");
    }
    case Op::Call: {
      std::vector<XValue> args;
      args.reserve(n.kids.size());
      for (int k : n.kids) {
        XValue v = evaluate(c, k);
        // Text and layers stay as they are; a property is read now.
        if (v.kind == XValue::PropRef || v.kind == XValue::SelfProp) v = realise(c, v);
        args.push_back(v);
      }
      if (c.failed) return {};
      const Node &callee = c.program->nodes[size_t(n.a)];
      if (callee.op == Op::Global) {
        const G g = G(callee.slot);
        if (!isFunction(g)) return failWith(c, QString("\"%1\" is not a function").arg(callee.text));
        return callFunction(c, g, args, callee.text);
      }
      if (callee.op == Op::Member) {
        const XValue object = evaluate(c, callee.a);
        if (c.failed) return {};
        return callMethod(c, object, callee.text, args);
      }
      // effect("Name")("Slider"): what effect() gave, asked for one of its values.
      const XValue called = evaluate(c, n.a);
      if (c.failed) return {};
      if (called.kind == XValue::EffectRef) return effectParameter(c, called, args);
      return failWith(c, "only a function can be called");
    }
    case Op::Unary: {
      const XValue v = realise(c, evaluate(c, n.a));
      if (c.failed) return {};
      if (n.text == "!") return XValue::number(truthy(v) ? 0 : 1);
      if (n.text == "+") return each(c, v, [](double x) { return x; }, "+");
      return each(c, v, [](double x) { return -x; }, "-");
    }
    case Op::Binary: {
      const XValue x = realise(c, evaluate(c, n.a));
      const XValue y = realise(c, evaluate(c, n.b));
      if (c.failed) return {};
      const QString &op = n.text;
      if (op == "+") return combine(c, x, y, [](double a, double b) { return a + b; }, op);
      if (op == "-") return combine(c, x, y, [](double a, double b) { return a - b; }, op);
      if (op == "*") return combine(c, x, y, [](double a, double b) { return a * b; }, op);
      if (op == "/") return combine(c, x, y, [](double a, double b) { return a / b; }, op);
      if (op == "%") return combine(c, x, y, [](double a, double b) { return std::fmod(a, b); }, op);
      double a = 0, b = 0;
      if (x.kind == XValue::Text && y.kind == XValue::Text) {
        const bool same = x.text == y.text;
        if (op == "==" || op == "===") return XValue::number(same);
        if (op == "!=" || op == "!==") return XValue::number(!same);
      }
      if (!asNumber(x, a) || !asNumber(y, b)) return failWith(c, QString("\"%1\" compares numbers").arg(op));
      if (op == "<") return XValue::number(a < b);
      if (op == ">") return XValue::number(a > b);
      if (op == "<=") return XValue::number(a <= b);
      if (op == ">=") return XValue::number(a >= b);
      if (op == "==" || op == "===") return XValue::number(a == b);
      if (op == "!=" || op == "!==") return XValue::number(a != b);
      return failWith(c, QString("\"%1\" is not understood").arg(op));
    }
    case Op::Logical: {
      const XValue x = realise(c, evaluate(c, n.a));
      if (c.failed) return {};
      if (n.text == "&&") return truthy(x) ? realise(c, evaluate(c, n.b)) : x;
      return truthy(x) ? x : realise(c, evaluate(c, n.b));
    }
    case Op::Ternary:
      return truthy(realise(c, evaluate(c, n.a))) ? evaluate(c, n.b) : evaluate(c, n.c);
    case Op::ArrayLit: {
      if (n.kids.size() > 4) return failWith(c, "an array holds at most four numbers");
      XValue r; r.kind = XValue::Array; r.size = int(n.kids.size());
      for (size_t k = 0; k < n.kids.size(); ++k) {
        double v = 0;
        if (!asNumber(realise(c, evaluate(c, n.kids[k])), v)) return failWith(c, "an array holds numbers");
        r.a[k] = v;
      }
      return r;
    }
    case Op::Assign: {
      XValue v = realise(c, evaluate(c, n.a));
      if (c.failed) return {};
      XValue &slot = c.locals[size_t(n.slot)];
      if (n.text == "+=") v = combine(c, slot, v, [](double a, double b) { return a + b; }, "+=");
      else if (n.text == "-=") v = combine(c, slot, v, [](double a, double b) { return a - b; }, "-=");
      else if (n.text == "*=") v = combine(c, slot, v, [](double a, double b) { return a * b; }, "*=");
      else if (n.text == "/=") v = combine(c, slot, v, [](double a, double b) { return a / b; }, "/=");
      slot = v;
      return v;
    }
    case Op::Declare: {
      c.locals[size_t(n.slot)] = n.a >= 0 ? realise(c, evaluate(c, n.a)) : XValue();
      XValue none; none.kind = XValue::Undefined; none.layer = -99; // no completion value
      return none;
    }
    case Op::If: {
      const bool yes = truthy(realise(c, evaluate(c, n.a)));
      if (yes) return evaluate(c, n.b);
      if (n.c >= 0) return evaluate(c, n.c);
      XValue none; none.layer = -99; return none;
    }
    case Op::Block: {
      XValue last; last.layer = -99;
      for (int k : n.kids) {
        const XValue v = evaluate(c, k);
        if (c.failed) return {};
        if (!(v.kind == XValue::Undefined && v.layer == -99)) last = v;
      }
      return last;
    }
  }
  return failWith(c, "that could not be worked out");
}

// Which layer a property belongs to. Every one of a layer's fixed properties
// knows its place in the layer (the slot), and only those have one, so the
// layer is found from where the property sits -- exact, and free. That works
// the same for a copy made while drawing (a layer adjusted for depth of
// field, one stripped for a cache key): the copy carries the id of the layer
// it came from, and that is how it is found in the scene. Any other curve is
// taken to be on the layer being drawn.
std::ptrdiff_t propertiesOffset() {
  static const std::ptrdiff_t offset = [] {
    const Layer probe;
    return reinterpret_cast<const char *>(&probe.p) - reinterpret_cast<const char *>(&probe);
  }();
  return offset;
}
const Layer *ownerOf(const Property &self, const Scene *scene, int *index) {
  *index = -1;
  const Layer *owner = expressionLayer;
  if (self.slot >= 0 && self.slot < PropertyCount)
    owner = reinterpret_cast<const Layer *>(reinterpret_cast<const char *>(&self - self.slot) - propertiesOffset());
  if (!owner || !scene || scene->layers.empty()) return owner;
  const Layer *first = scene->layers.data();
  if (owner >= first && owner < first + scene->layers.size()) {
    *index = int(owner - first);
    return owner;
  }
  thread_local int remembered = -1;
  if (remembered >= 0 && remembered < int(scene->layers.size()) && scene->layers[size_t(remembered)].id == owner->id) {
    *index = remembered;
    return owner;
  }
  for (size_t i = 0; i < scene->layers.size(); ++i)
    if (scene->layers[i].id == owner->id) { *index = remembered = int(i); break; }
  return owner;
}
}  // namespace

std::shared_ptr<const ExpressionProgram> compileExpression(const QString &text, QString *error) {
  auto program = std::make_shared<ExpressionProgram>();
  program->source = text;
  Parser parser;
  parser.program = program.get();
  if (!lex(text, parser.tokens, &parser.error)) {
    if (error) *error = parser.error;
    return nullptr;
  }
  while (parser.peek().kind != Token::End) {
    const int s = parser.statement();
    if (s < 0) { if (error) *error = parser.error; return nullptr; }
    program->statements.push_back(s);
  }
  if (program->statements.empty()) { if (error) *error = "the expression is empty"; return nullptr; }
  if (error) error->clear();
  return program;
}

double evaluateExpression(const ExpressionProgram &program, const Property &self, double frame, QString *error) {
  // A property that reads itself, or two that read each other, would never
  // finish: past a few levels the property's own value is the answer.
  if (g_depth >= 12) {
    if (error) *error = "this expression reads itself, through other properties or directly";
    return self.keyedAt(frame);
  }
  struct Depth { Depth() { ++g_depth; } ~Depth() { --g_depth; } } depth;
  Context c;
  c.program = &program;
  c.self = &self;
  c.scene = expressionScene;
  c.owner = ownerOf(self, c.scene, &c.ownerIndex);
  c.fps = c.scene ? std::max(1, c.scene->fps) : 30;
  c.frame = frame;
  c.compFrame = frame + (c.owner ? c.owner->start : 0);
  c.group = groupOf(self.slot, &c.dimension);
  c.groupSize = groupSizeFor(c.owner, c.dimension);
  if (!c.owner) c.dimension = -1;
  XValue last; last.layer = -99;
  for (int s : program.statements) {
    const XValue v = evaluate(c, s);
    if (c.failed) break;
    if (!(v.kind == XValue::Undefined && v.layer == -99)) last = v;
  }
  if (!c.failed) {
    last = realise(c, last);
    double result = 0;
    bool got = false;
    if (last.kind == XValue::Number) { result = last.n; got = true; }
    else if (last.kind == XValue::Array && last.size > 0) {
      const int d = c.dimension >= 0 && c.dimension < last.size ? c.dimension : 0;
      result = last.a[size_t(d)]; got = true;
    }
    if (got && std::isfinite(result)) { if (error) error->clear(); return stored(self.slot, result); }
    c.error = got ? QString("the result is not a finite number") : QString("the expression does not end in a number");
  }
  if (error) *error = c.error;
  return self.keyedAt(frame);
}

ExpressionSceneScope::ExpressionSceneScope(const Scene *scene) : previous(expressionScene) { expressionScene = scene; }
ExpressionSceneScope::~ExpressionSceneScope() { expressionScene = previous; }
ExpressionLayerScope::ExpressionLayerScope(const Layer *layer) : previous(expressionLayer) { expressionLayer = layer; }
ExpressionLayerScope::~ExpressionLayerScope() { expressionLayer = previous; }

bool expressionReachesOtherLayers(const QString &text) {
  // Generous on purpose: a false yes only costs the cache some reuse.
  return text.contains("thisComp") || text.contains("layer(") || text.contains("index") || text.contains("effect(");
}
