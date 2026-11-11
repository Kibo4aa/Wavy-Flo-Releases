#include "jsxbin.h"
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

// A compiled script is a stream of letters: a node's kind, then its parts in
// an order fixed for that kind. Numbers are letters too ("A".."Z" 0..25, two
// letters up to 255, "2"/"4"/"8" and that many bytes, "y" before a negative
// one, "0" and a letter for a run of zeros); strings are a length and their
// UTF-16 units; a name is "z", the name and its number the first time, the
// number alone after.
namespace {
constexpr int kMaxDepth = 3000; // nested deeper than any script is written

enum class Version { V10, V20, V21 };

struct Node;
using P = std::unique_ptr<Node>;

struct Node {
  char kind = 0;
  // A statement's line in the original script, and its labels.
  int line = 0;
  QStringList labels;
  QString name, op;       // a name, an operator, a member; a regular expression's pattern and flags
  QString lit1, lit2;     // literal values, as JavaScript ("" when there is none)
  QString raw;            // a string constant's own text
  bool isString = false;  // ... when it is one
  bool f1 = false, f2 = false;
  double n1 = 0, n2 = 0;
  P a, b, c, d;           // parts, by kind
  std::vector<P> list, list2;
  QStringList names;
  // A function: its arguments, its own variables, whether it is the script itself.
  QStringList params, locals;
  bool closure = false;
};

QString numberText(double v) {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v > 0 ? "Infinity" : "(-Infinity)";
  if (v == 0) return std::signbit(v) ? "(-0)" : "0";
  QString s = v == std::floor(v) && std::abs(v) < 9007199254740992.0 ? QString::number(qint64(v))
                                                                    : QString::number(v, 'g', 17);
  return v < 0 ? "(" + s + ")" : s;
}

QString stringLiteral(const QString &text) {
  QString out = "\"";
  out.reserve(text.size() + 2);
  for (const QChar ch : text) {
    const ushort u = ch.unicode();
    switch (u) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\v': out += "\\v"; break;
    default:
      if (u < 0x20 || u >= 0x7f) out += QString("\\u%1").arg(u, 4, 16, QChar('0'));
      else out += ch;
    }
  }
  return out + "\"";
}

bool identifierName(const QString &name) {
  if (name.isEmpty()) return false;
  for (int i = 0; i < name.size(); ++i) {
    const QChar c = name[i];
    const bool ok = c == '_' || c == '$' || (c.unicode() < 0x80 ? (c.isLetter() || (i > 0 && c.isDigit())) : c.isLetterOrNumber());
    if (!ok) return false;
  }
  return true;
}

bool reserved(const QString &name) {
  static const QSet<QString> words{"break", "case", "catch", "class", "const", "continue", "debugger", "default",
                                   "delete", "do", "else", "enum", "export", "extends", "false", "finally", "for",
                                   "function", "if", "import", "in", "instanceof", "new", "null", "return", "super",
                                   "switch", "this", "throw", "true", "try", "typeof", "var", "void", "while", "with"};
  return words.contains(name);
}

// A name as a variable: itself, or one made from it when JavaScript would not
// read it as one (names a compiler or an obfuscator left behind).
QString ident(const QString &name) {
  if (identifierName(name) && !reserved(name)) return name;
  QString made = "__wf$";
  for (const QChar c : name) made += QString::number(c.unicode(), 16) + "_";
  return made;
}

// --- Reading -------------------------------------------------------------------
class Reader {
public:
  Version version = Version::V20;
  QString error;

  explicit Reader(const QString &text) {
    data.reserve(size_t(text.size()));
    for (const QChar c : text) {
      const ushort u = c.unicode();
      if (u == ' ' || u == '\t' || u == '\r' || u == '\n' || u == '\\' || u == 0xFEFF) continue;
      data.push_back(u);
    }
  }

  bool signature() {
    static const char *const known[] = {"@JSXBIN@ES@1.0@", "@JSXBIN@ES@2.0@", "@JSXBIN@ES@2.1@"};
    for (int v = 0; v < 3; ++v) {
      bool same = data.size() >= 15;
      for (int i = 0; same && i < 15; ++i) same = data[size_t(i)] == ushort(known[v][i]);
      if (same) {
        version = Version(v);
        pos = 15;
        return true;
      }
    }
    fail("not a compiled script of a version Wavy-Flo knows (1.0, 2.0, 2.1)");
    return false;
  }

  bool failed() const { return !error.isEmpty(); }
  void fail(const QString &why) {
    if (error.isEmpty()) error = QString("%1 (at %2 of %3)").arg(why).arg(pos).arg(data.size());
  }

  P program() {
    P root = node();
    if (!failed() && !root) fail("the script is empty");
    return root;
  }

private:
  std::vector<ushort> data;
  size_t pos = 0;
  int zeros = 0; // left of a run of zeros
  int depth = 0;
  std::map<qint64, QString> symbols;

  ushort next() {
    if (pos < data.size()) return data[pos++];
    ++pos;
    fail("the script ends early");
    return 0;
  }
  ushort peek() const { return pos < data.size() ? data[pos] : 0; }
  void back() { --pos; }

  // How many zeros a run that began with "0" counts.
  int run() {
    const int levels = int(next()) - 0x3f;
    if (levels <= 0) {
      fail("a broken run of zeros");
      return 1;
    }
    if (levels > 0x1b) {
      if (peek() == 'A') {
        ++pos;
        return levels + 1;
      }
      if (peek() == '0') {
        ++pos;
        return levels + run();
      }
    }
    return levels;
  }

  int byte() {
    if (zeros > 0) {
      --zeros;
      return 0;
    }
    const ushort m = next();
    if (m == '0') {
      zeros = run() - 1;
      return 0;
    }
    if (m >= 'A' && m <= 'Z') return m - 'A';
    if (m > 'Z' && m <= 'n') {
      const ushort z = next();
      const int low = z > 'Z' ? (z <= 'f' ? z - 0x47 : -1) : z - 0x41;
      if (low < 0) {
        fail("a broken number");
        return 0;
      }
      return (low | ((m + 1) * 32)) & 0xff;
    }
    fail("a broken number");
    return 0;
  }

  double number() {
    if (zeros > 0) {
      --zeros;
      return 0;
    }
    ushort t = next();
    double sign = 1;
    if (t == 'y') {
      sign = -1;
      t = next();
    }
    if (t == '2' || t == '4' || t == '8') {
      const int n = t - '0';
      quint64 bits = 0;
      for (int i = 0; i < n; ++i) bits |= quint64(byte()) << (8 * i);
      if (n == 8) {
        double d;
        std::memcpy(&d, &bits, sizeof d);
        return sign * d;
      }
      return sign * double(bits);
    }
    back();
    return sign * byte();
  }
  // A count, a length, a kind: written without its sign.
  int count() {
    const double n = std::abs(number());
    return n > 1e8 ? (fail("a broken count"), 0) : int(n);
  }
  bool boolean() {
    const ushort t = next();
    if (t == 't') return true;
    if (t != 'f') fail("a broken yes/no");
    return false;
  }
  QString string() {
    const int length = count();
    QString s;
    s.reserve(length);
    for (int i = 0; i < length && !failed(); ++i) s += QChar(ushort(number()));
    return s;
  }
  QString symbol() {
    if (next() == 'z') {
      const QString name = string();
      symbols[qint64(number())] = name;
      return name;
    }
    back();
    const auto found = symbols.find(qint64(number()));
    return found == symbols.end() ? QString() : found->second;
  }
  // A name, and (from 2.0) a yes/no that goes with it.
  QString reference() {
    const QString name = symbol();
    if (version != Version::V10) boolean();
    return name;
  }
  // A literal value as JavaScript; "" when there is none.
  QString value(Node *into = nullptr) {
    const ushort t = next();
    switch (t) {
    case 'n': return {};
    case 'a':
    case 'b': return "null";
    case 'c': return boolean() ? "true" : "false";
    case 'd': return numberText(number());
    case 'e': {
      const QString s = string();
      if (into) {
        into->raw = s;
        into->isString = true;
      }
      return stringLiteral(s);
    }
    default: fail("a broken value"); return {};
    }
  }
  // Where a statement is: its line, its body (for those that have one), its labels.
  void place(Node &n) {
    n.line = std::max(0, int(number())); // -1: a line of no line of its own
    if (n.line > 10000000) n.line = 0;
    n.a = node();
    const int labels = count();
    for (int i = 0; i < labels && !failed(); ++i) n.labels << symbol();
  }
  std::vector<P> nodes() {
    std::vector<P> out;
    const int n = count();
    for (int i = 0; i < n && !failed(); ++i) out.push_back(node());
    return out;
  }
  void signatureOf(Node &n) {
    std::map<qint64, QString> named;
    const int count_ = count();
    for (int i = 0; i < count_ && !failed(); ++i) {
      const QString name = symbol();
      named[qint64(number())] = name;
    }
    const int arguments = count();
    count(); // its variables
    count(); // its constants
    n.name = symbol();
    n.closure = (qint64(number()) & 1) != 0;
    for (int i = 0; i < arguments; ++i) {
      const auto found = named.find(0x20000000LL + i);
      n.params << (found == named.end() || found->second.isEmpty() ? QString("__wf_arg%1").arg(i) : ident(found->second));
    }
    for (const auto &[key, name] : named) {
      const qint64 range = key >> 29;
      if ((range == 2 || range == 3) && !name.isEmpty() && name != "arguments" && !n.params.contains(ident(name)) &&
          !n.locals.contains(ident(name)))
        n.locals << ident(name);
    }
  }

  P node() {
    if (failed()) return nullptr;
    const ushort marker = next();
    if (marker == 'n' || failed()) return nullptr;
    if (++depth > kMaxDepth) {
      fail("nested too deeply");
      return nullptr;
    }
    auto n = std::make_unique<Node>();
    n->kind = char(marker);
    const bool v2 = version != Version::V10;
    switch (marker) {
    case 'A': n->a = node(); break;                                   // [ ... ]
    case 'B':                                                         // x.y = ...
      n->a = node(); n->b = node(); n->lit1 = value(); n->f1 = boolean(); break;
    case 'C':                                                         // a op b
    case 'U':                                                         // a && b
      n->op = symbol(); n->a = node(); n->b = node(); n->lit1 = value(); n->lit2 = value(); break;
    case 'D': place(*n); n->name = symbol(); n->f1 = boolean(); break; // break / continue
    case 'E': n->a = node(); n->b = node(); n->f1 = boolean(); break;  // call, new
    case 'F': n->lit1 = value(n.get()); break;                         // constant
    case 'G':                                                         // const x = ...
      n->name = symbol(); count(); n->b = node(); n->lit1 = value(); boolean(); boolean(); break;
    case 'H': place(*n); break;                                       // debugger
    case 'I': place(*n); n->b = node(); break;                        // do ... while
    case 'J': place(*n); n->b = node(); break;                        // expression;
    case 'K': place(*n); n->b = node(); n->c = node(); n->d = node(); break; // for (;;)
    case 'L':                                                         // for (x in o), for each
      place(*n); n->b = node(); n->c = node(); count(); symbol();
      if (v2) n->f1 = boolean();
      break;
    case 'M': place(*n); signatureOf(*n); count(); break;             // function
    case 'N': place(*n); n->b = node(); break;                        // function expression
    case 'O': place(*n); n->b = node(); n->c = node(); break;         // if
    case 'P': n->a = node(); n->n1 = number(); n->f1 = boolean(); break; // x.y++
    case 'Q': reference(); n->a = node(); n->b = node(); break;        // a[b]
    case 'R': n->list = nodes(); n->f1 = boolean(); break;            // a, b
    case 'S':                                                         // local = ...
      n->name = ident(symbol()); count(); n->b = node(); n->lit1 = value(); n->f1 = boolean(); n->f2 = boolean(); break;
    case 'T': n->name = ident(symbol()); count(); n->n1 = number(); n->f1 = boolean(); break; // local++
    case 'V': n->name = ident(reference()); count(); break;           // a local
    case 'W': {                                                       // { ... }
      symbol();
      const int fields = count();
      for (int i = 0; i < fields && !failed(); ++i) {
        n->names << symbol();
        n->list.push_back(node());
      }
      break;
    }
    case 'X': n->name = reference(); n->a = node(); break;            // a.b
    case 'Y': n->name = string(); n->op = string(); break;            // /re/flags
    case 'Z': place(*n); n->b = node(); break;                        // return
    case 'a':                                                         // for (i = a; i < b; i += c)
      place(*n); n->b = node(); n->n1 = number(); n->c = node(); n->n2 = number(); count(); n->op = symbol(); break;
    case 'b': {                                                       // statements
      place(*n);
      n->list = nodes();
      std::vector<P> more = nodes();
      for (P &m : more) n->list.push_back(std::move(m));
      break;
    }
    case 'c': place(*n); n->b = node(); n->list = nodes(); n->list2 = nodes(); break; // switch
    case 'd': n->a = node(); n->b = node(); n->c = node(); break;    // a ? b : c
    case 'e': reference(); break;                                     // this
    case 'f': place(*n); n->b = node(); break;                        // throw
    case 'g': {                                                       // try
      place(*n);
      const int catches = count();
      n->b = node();
      for (int i = 0; i < catches && !failed(); ++i) {
        n->names << ident(symbol());
        n->list.push_back(node());
        n->list2.push_back(node());
      }
      break;
    }
    case 'h': n->op = symbol(); n->a = node(); break;                 // -a, !a
    case 'i': n->op = symbol(); n->a = node(); break;                 // typeof a, delete a
    case 'j': n->name = ident(symbol()); if (v2) boolean(); break;    // a name
    case 'k': n->a = node(); break;                                   // default xml namespace
    case 'l': place(*n); n->b = node(); break;                        // while
    case 'm': place(*n); n->b = node(); break;                        // with
    // E4X (XML in the language itself), which no JavaScript engine of today
    // reads: kept apart, and turned into an error where it would run.
    case 'o': {
      const int parts = count();
      for (int i = 0; i < parts && !failed(); ++i) {
        n->list.push_back(node());
        count();
      }
      break;
    }
    case 'p': reference(); n->a = node(); node(); node(); symbol(); break;
    case 'q': reference(); n->a = node(); node(); node(); break;
    case 'r': reference(); n->a = node(); n->b = node(); break;
    case 's': symbol(); n->a = node(); break;
    default: fail(QString("an unknown part '%1'").arg(QChar(marker))); break;
    }
    --depth;
    return failed() ? nullptr : std::move(n);
  }
};

// --- Writing JavaScript ----------------------------------------------------------
// Text and, for each line break in it, the original line the next line belongs to.
struct Code {
  QString s;
  std::vector<int> breaks;
  Code() = default;
  Code(const QString &t) : s(t) {}
  Code(const char *t) : s(QString::fromLatin1(t)) {}
  Code &operator+=(const Code &o) {
    s += o.s;
    breaks.insert(breaks.end(), o.breaks.begin(), o.breaks.end());
    return *this;
  }
  void line(int origin) {
    s += '\n';
    breaks.push_back(origin);
  }
};
Code operator+(Code a, const Code &b) { return a += b; }

class Writer {
public:
  QString error;

  Code script(const Node *root) {
    if (root->kind == 'M' && root->closure) {
      Code c = root->locals.isEmpty() ? Code() : Code("var " + root->locals.join(", ") + ";");
      return c + body(root->a.get());
    }
    return statement(root);
  }

private:
  int temporaries = 0;

  static bool primaryKind(const Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case 'j': case 'V': case 'X': case 'Q': case 'e': case 'A': return true;
    case 'E': return !n->f1;
    default: return false;
    }
  }
  Code primary(const Node *n) {
    Code e = expression(n);
    return primaryKind(n) ? e : "(" + e + ")";
  }
  Code target(const Node *n) {
    if (n && (n->kind == 'j' || n->kind == 'V' || n->kind == 'X' || n->kind == 'Q')) return expression(n);
    return "(" + expression(n) + ")";
  }
  Code side(const Node *n, const QString &literal) {
    if (n) return expression(n);
    return literal.isEmpty() ? Code() : Code(literal);
  }
  Code arguments(const Node *list) {
    Code c;
    if (!list) return c;
    if (list->kind != 'R') return expression(list);
    for (size_t i = 0; i < list->list.size(); ++i) {
      if (i) c += ", ";
      c += list->list[i] ? expression(list->list[i].get()) : Code("undefined");
    }
    return c;
  }
  // x op= y: the operand of the operation the assignment carries.
  Code compound(const Code &into, const Node *n) {
    const Node *operation = n->b.get();
    if (!operation || (operation->kind != 'C' && operation->kind != 'U')) return "(" + into + " = " + side(operation, n->lit1) + ")";
    Code value = n->lit1.isEmpty() ? side(operation->b.get(), operation->lit2) : Code(n->lit1);
    if (value.s.isEmpty()) value = side(operation->a.get(), operation->lit1);
    return "(" + into + " " + operation->op + "= " + value + ")";
  }

  Code function(const Node *m) {
    Code c = "function " + (m->name.isEmpty() ? QString() : ident(m->name)) + "(" + m->params.join(", ") + ") {";
    if (!m->locals.isEmpty()) c += " var " + m->locals.join(", ") + ";";
    c += body(m->a.get());
    c.line(m->line);
    return c + "}";
  }

  Code expression(const Node *n) {
    if (!n) return "undefined";
    switch (n->kind) {
    case 'j': case 'V': return n->name;
    case 'e': return "this";
    case 'F': return n->lit1.isEmpty() ? QString("undefined") : n->lit1;
    case 'A': {
      Code c = "[";
      if (n->a && n->a->kind == 'R') {
        const auto &items = n->a->list;
        for (size_t i = 0; i < items.size(); ++i) {
          if (i) c += ", ";
          if (items[i]) c += expression(items[i].get());
        }
        if (!items.empty() && !items.back()) c += ",";
      } else if (n->a) {
        c += expression(n->a.get());
      }
      return c + "]";
    }
    case 'R': {
      if (n->list.size() == 1) return expression(n->list[0].get());
      if (n->list.empty()) return "undefined";
      Code c = "(";
      for (size_t i = 0; i < n->list.size(); ++i) {
        if (i) c += ", ";
        c += expression(n->list[i].get());
      }
      return c + ")";
    }
    case 'W': {
      Code c = "({";
      for (int i = 0; i < n->names.size(); ++i) {
        if (i) c += ", ";
        c += stringLiteral(n->names[i]) + ": " + expression(n->list[size_t(i)].get());
      }
      return c + "})";
    }
    case 'X': {
      Code object = n->a ? primary(n->a.get()) : Code("this");
      if (identifierName(n->name)) return object + "." + n->name;
      return object + "[" + stringLiteral(n->name) + "]";
    }
    case 'Q': return primary(n->a.get()) + "[" + expression(n->b.get()) + "]";
    case 'E': {
      const Node *callee = n->a.get();
      if (!n->f1 && callee && callee->kind == 'j' && callee->name == "eval" && n->b && n->b->kind == 'R' &&
          n->b->list.size() == 1 && n->b->list[0] && n->b->list[0]->kind == 'F' && n->b->list[0]->isString &&
          isJsxbin(n->b->list[0]->raw)) {
        // A compiled script inside: run as what it says, in the same place.
        const JsxbinScript inner = decodeJsxbin(n->b->list[0]->raw);
        if (inner.ok()) return "eval(" + stringLiteral(inner.code) + ")";
      }
      if (n->f1) return "(new " + primary(callee) + "(" + arguments(n->b.get()) + "))";
      return primary(callee) + "(" + arguments(n->b.get()) + ")";
    }
    case 'N': return n->b && n->b->kind == 'M' ? "(" + function(n->b.get()) + ")" : expression(n->b.get());
    case 'M': return "(" + function(n) + ")";
    case 'B': {
      const Code into = target(n->a.get());
      if (n->f1) return compound(into, n);
      return "(" + into + " = " + side(n->b.get(), n->lit1) + ")";
    }
    case 'S':
      if (n->f1) return compound(Code(n->name), n);
      return "(" + n->name + " = " + side(n->b.get(), n->lit1) + ")";
    case 'G': return "(" + ident(n->name) + " = " + side(n->b.get(), n->lit1) + ")";
    case 'C':
    case 'U': {
      const Code left = side(n->a.get(), n->lit1), right = side(n->b.get(), n->lit2);
      if (left.s.isEmpty()) return right;
      if (right.s.isEmpty()) return left;
      return "(" + left + " " + n->op + " " + right + ")";
    }
    case 'd': return "(" + expression(n->a.get()) + " ? " + expression(n->b.get()) + " : " + expression(n->c.get()) + ")";
    case 'h':
    case 'i': {
      const bool word = !n->op.isEmpty() && n->op[0].isLetter();
      return "(" + n->op + (word ? " " : "") + "(" + expression(n->a.get()) + "))";
    }
    case 'k': return expression(n->a.get());
    case 'P': {
      const QString op = n->n1 == 1 ? "++" : "--";
      const Code into = target(n->a.get());
      return n->f1 ? "(" + into + op + ")" : "(" + op + into + ")";
    }
    case 'T': {
      const QString op = n->n1 == 1 ? "++" : "--";
      return n->f1 ? "(" + n->name + op + ")" : "(" + op + n->name + ")";
    }
    case 'Y': return "(new RegExp(" + stringLiteral(n->name) + ", " + stringLiteral(n->op) + "))";
    case 'o': case 'p': case 'q': case 'r': case 's': return "__wfNoE4X()";
    default: {
      // A statement where a value was asked for: run it, as a function.
      Code wrapped = "(function () {" + statement(n);
      wrapped.line(n->line);
      return wrapped + "})()";
    }
    }
  }

  // The statements of a body, each on a line of its own.
  Code body(const Node *n) {
    if (!n) return {};
    if (n->kind == 'b' && n->labels.isEmpty()) {
      Code c;
      if (n->a) c += statement(n->a.get());
      for (const P &s : n->list) c += statement(s.get());
      return c;
    }
    return statement(n);
  }

  Code statement(const Node *n) {
    if (!n) return {};
    Code c;
    c.line(n->line);
    QString labels;
    for (const QString &label : n->labels) labels += ident(label) + ": ";
    auto block = [&](const Node *b) {
      Code inner = "{" + body(b);
      inner.line(n->line);
      return inner + "}";
    };
    switch (n->kind) {
    case 'b': {
      c += labels;
      Code inner = "{";
      if (n->a) inner += statement(n->a.get());
      for (const P &s : n->list) inner += statement(s.get());
      inner.line(n->line);
      return c + inner + "}";
    }
    case 'J': {
      Code e = expression(n->b.get());
      if (e.s.startsWith('{') || e.s.startsWith("function")) e = "(" + e + ")";
      return c + labels + e + ";";
    }
    case 'O': {
      c += labels + "if (" + expression(n->b.get()) + ") " + block(n->a.get());
      if (n->c) c += " else " + block(n->c.get());
      return c;
    }
    case 'K':
      return c + labels + "for (" + (n->b ? expression(n->b.get()) : Code()) + "; " +
             (n->c ? expression(n->c.get()) : Code()) + "; " + (n->d ? expression(n->d.get()) : Code()) + ") " +
             block(n->a.get());
    case 'a': {
      const Code v = target(n->b.get());
      return c + labels + "for (" + v + " = " + numberText(n->n1) + "; " + v + " " + n->op + " " +
             expression(n->c.get()) + "; " + v + " += " + numberText(n->n2) + ") " + block(n->a.get());
    }
    case 'L': {
      if (!n->f1)
        return c + labels + "for (" + target(n->b.get()) + " in " + expression(n->c.get()) + ") " + block(n->a.get());
      // for each (x in o): the values, not the names.
      const QString object = QString("__wfo%1").arg(++temporaries), key = QString("__wfk%1").arg(temporaries);
      Code inner = "{" + target(n->b.get()) + " = " + object + "[" + key + "];" + body(n->a.get());
      inner.line(n->line);
      return c + "var " + object + " = " + expression(n->c.get()) + "; " + labels + "for (var " + key + " in " +
             object + ") " + inner + "}";
    }
    case 'I': return c + labels + "do " + block(n->a.get()) + " while (" + expression(n->b.get()) + ");";
    case 'l':
      return c + labels + "while (" + (n->b ? expression(n->b.get()) : Code("true")) + ") " + block(n->a.get());
    case 'm': return c + labels + "with (" + expression(n->b.get()) + ") " + block(n->a.get());
    case 'c': {
      c += labels + "switch (" + expression(n->b.get()) + ") {";
      for (size_t i = 0; i < n->list.size(); ++i) {
        const Node *cases = n->list[i].get();
        Code heads;
        if (cases && cases->kind == 'R' && !cases->list.empty()) {
          for (const P &value : cases->list) {
            heads.line(n->line);
            heads += "case " + expression(value.get()) + ":";
          }
        } else if (cases && cases->kind != 'R') {
          heads.line(n->line);
          heads += "case " + expression(cases) + ":";
        } else {
          heads.line(n->line);
          heads += "default:";
        }
        c += heads;
        if (i < n->list2.size()) c += body(n->list2[i].get());
      }
      c.line(n->line);
      return c + "}";
    }
    case 'D': return c + labels + (n->f1 ? "break" : "continue") + (n->name.isEmpty() ? QString() : " " + ident(n->name)) + ";";
    case 'Z': return c + labels + "return" + (n->b ? " " + expression(n->b.get()) : Code()) + ";";
    case 'f': return c + labels + "throw " + expression(n->b.get()) + ";";
    case 'g': {
      c += labels + "try " + block(n->a.get());
      const int catches = n->names.size();
      bool filtered = false;
      for (const P &filter : n->list) filtered |= bool(filter);
      if (catches == 1 && !filtered) {
        c += " catch (" + n->names[0] + ") " + block(n->list2[0].get());
      } else if (catches > 0) {
        // catch (e if ...): one catch, the first whose test passes.
        const QString thrown = QString("__wfe%1").arg(++temporaries);
        Code inner = "{";
        for (int i = 0; i < catches; ++i) {
          inner += " var " + n->names[i] + " = " + thrown + ";";
          inner += QString(i ? " else " : " ") + "if (" + (n->list[size_t(i)] ? expression(n->list[size_t(i)].get()) : Code("true")) +
                   ") " + block(n->list2[size_t(i)].get());
        }
        inner += " else throw " + thrown + ";";
        inner.line(n->line);
        c += " catch (" + thrown + ") " + inner + "}";
      }
      if (n->b || catches == 0) c += " finally " + block(n->b.get());
      return c;
    }
    case 'H': return c + labels + "debugger;";
    case 'M': {
      if (n->closure) return c + labels + body(n->a.get());
      if (n->name.isEmpty()) return c + labels + "(" + function(n) + ");";
      return c + labels + function(n);
    }
    case 'N': return c + labels + "(" + expression(n) + ");";
    default: {
      Code e = expression(n);
      return c + labels + e + ";";
    }
    }
  }
};
} // namespace

bool isJsxbin(const QString &text) {
  int i = 0;
  while (i < text.size() && (text[i].isSpace() || text[i] == QChar(0xFEFF))) ++i;
  return QStringView(text).mid(i).startsWith(QLatin1String("@JSXBIN@"));
}

JsxbinScript decodeJsxbin(const QString &text) {
  JsxbinScript out;
  Reader reader(text);
  if (!reader.signature()) {
    out.error = reader.error;
    return out;
  }
  const P root = reader.program();
  if (reader.failed()) {
    out.error = reader.error;
    return out;
  }
  Writer writer;
  Code code = writer.script(root.get());
  // The first line: before any statement's own.
  int last = 0;
  for (int b : code.breaks)
    if (b > 0) {
      last = b;
      break;
    }
  out.lines.push_back(last);
  for (int b : code.breaks) {
    if (b > 0) last = b;
    out.lines.push_back(last);
  }
  out.code = std::move(code.s);
  return out;
}

QString inlineJsxbinEvals(const QString &text) {
  if (!text.contains(QLatin1String("@JSXBIN@"))) return text;
  static const QRegularExpression call("\\beval\\s*\\(\\s*([\"'])@JSXBIN@");
  QString out;
  int from = 0;
  QRegularExpressionMatchIterator it = call.globalMatch(text);
  while (it.hasNext()) {
    const QRegularExpressionMatch m = it.next();
    if (m.capturedStart() < from) continue;
    const QChar quote = m.captured(1)[0];
    // The string: to its closing quote, escapes and line continuations read.
    QString value;
    int i = m.capturedStart(1) + 1, newlines = 0;
    bool closed = false;
    while (i < text.size()) {
      const QChar c = text[i];
      if (c == quote) {
        closed = true;
        ++i;
        break;
      }
      if (c == '\n') ++newlines;
      if (c == '\\' && i + 1 < text.size()) {
        const QChar e = text[i + 1];
        if (e == '\n') ++newlines;
        if (e == 'n') value += '\n';
        else if (e == 't') value += '\t';
        else if (e != '\r' && e != '\n') value += e;
        i += 2;
        continue;
      }
      value += c;
      ++i;
    }
    if (!closed) break;
    // Then the closing parenthesis.
    int j = i;
    while (j < text.size() && text[j].isSpace()) {
      if (text[j] == '\n') ++newlines;
      ++j;
    }
    if (j >= text.size() || text[j] != ')') continue;
    const JsxbinScript inner = decodeJsxbin(value);
    if (!inner.ok()) continue;
    out += QStringView(text).mid(from, m.capturedStart() - from);
    out += "eval(" + stringLiteral(inner.code) + QString(newlines, '\n') + ")";
    from = j + 1;
  }
  out += QStringView(text).mid(from);
  return out;
}
