#include "shader_cpu.h"
#include "render_workers.h"
#include <QStringList>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#endif

// How it works, front to back:
//  1. A preprocessor (comments, #define, #if...) turns the text into tokens.
//  2. A parser turns those into a tree: functions, statements, expressions.
//  3. The compiler walks the tree from the entry function, inlining every call
//     (GLSL has no recursion), and writes instructions that each work on one
//     number of a value for a row of B pixels at once -- a vec3 is three such
//     numbers. Anything that depends only on the inputs (the sliders, the time)
//     is worked out once per run instead of once per pixel.
//  4. Ifs and loops become nodes. Each runs with a mask of the lanes (pixels)
//     taking it; lanes that return, break, continue or discard drop out of the
//     mask for what follows, exactly as the card runs a group of pixels.
//  5. run() hands rows of the picture to the host's workers, each with its own
//     registers, B pixels at a time.
namespace cpu_shader {
namespace {

constexpr int B = 64; // lanes: pixels worked on together

struct CompileError {
  int line;
  std::string what;
};
[[noreturn]] void fail(int line, const std::string &what) { throw CompileError{line, what}; }

// =============================================================================
// Tokens
// =============================================================================
enum class Tok { Ident, Int, UInt, Float, Punct };
struct Token {
  Tok kind = Tok::Punct;
  std::string text;
  double f = 0;
  uint64_t i = 0;
  int line = 0;
  bool is(const char *p) const { return kind == Tok::Punct && text == p; }
};

bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

void tokenizeLine(const std::string &s, int line, std::vector<Token> &out) {
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const char c = s[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (identStart(c)) {
      size_t j = i + 1;
      while (j < n && identChar(s[j])) ++j;
      Token t;
      t.kind = Tok::Ident;
      t.text = s.substr(i, j - i);
      t.line = line;
      out.push_back(t);
      i = j;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
      size_t j = i;
      Token t;
      t.line = line;
      if (c == '0' && j + 1 < n && (s[j + 1] == 'x' || s[j + 1] == 'X')) {
        j += 2;
        const size_t start = j;
        while (j < n && std::isxdigit(static_cast<unsigned char>(s[j]))) ++j;
        if (j == start) fail(line, "a hexadecimal number without digits");
        if (j - start > 8) fail(line, "a hexadecimal number too large for 32 bits");
        t.i = std::stoull(s.substr(start, j - start), nullptr, 16);
        t.kind = Tok::Int;
        if (j < n && (s[j] == 'u' || s[j] == 'U')) {
          t.kind = Tok::UInt;
          ++j;
        }
        t.text = s.substr(i, j - i);
        out.push_back(t);
        i = j;
        continue;
      }
      bool isFloat = false;
      while (j < n && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
      if (j < n && s[j] == '.') {
        isFloat = true;
        ++j;
        while (j < n && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
      }
      if (j < n && (s[j] == 'e' || s[j] == 'E')) {
        size_t k = j + 1;
        if (k < n && (s[k] == '+' || s[k] == '-')) ++k;
        if (k < n && std::isdigit(static_cast<unsigned char>(s[k]))) {
          isFloat = true;
          j = k;
          while (j < n && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
        }
      }
      const std::string digits = s.substr(i, j - i);
      if (isFloat || (j < n && (s[j] == 'f' || s[j] == 'F'))) {
        t.kind = Tok::Float;
        t.f = std::strtod(digits.c_str(), nullptr);
        if (j < n && (s[j] == 'f' || s[j] == 'F')) ++j;
        else if (j + 1 < n && (s[j] == 'l' || s[j] == 'L') && (s[j + 1] == 'f' || s[j + 1] == 'F')) j += 2;
      } else {
        t.kind = Tok::Int;
        try {
          t.i = digits.size() > 1 && digits[0] == '0' ? std::stoull(digits, nullptr, 8) : std::stoull(digits, nullptr, 10);
        } catch (...) {
          fail(line, "the number " + digits + " is too large");
        }
        if (t.i > 0xffffffffull) fail(line, "the number " + digits + " is too large for 32 bits");
        if (j < n && (s[j] == 'u' || s[j] == 'U')) {
          t.kind = Tok::UInt;
          ++j;
        }
      }
      if (j < n && identChar(s[j])) fail(line, "a number running into a name: " + s.substr(i, j - i + 1));
      t.text = s.substr(i, j - i);
      out.push_back(t);
      i = j;
      continue;
    }
    static const char *const three[] = {"<<=", ">>="};
    static const char *const two[] = {"++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
                                      "^^", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=", "##"};
    Token t;
    t.kind = Tok::Punct;
    t.line = line;
    bool matched = false;
    for (const char *p : three)
      if (s.compare(i, 3, p) == 0) {
        t.text = p;
        i += 3;
        matched = true;
        break;
      }
    if (!matched)
      for (const char *p : two)
        if (s.compare(i, 2, p) == 0) {
          t.text = p;
          i += 2;
          matched = true;
          break;
        }
    if (!matched) {
      if (!std::strchr("+-*/%<>=!~&|^?:;,.()[]{}#", c)) fail(line, std::string("an unexpected character '") + c + "'");
      t.text = std::string(1, c);
      ++i;
    }
    out.push_back(t);
  }
}

// =============================================================================
// Preprocessor
// =============================================================================
struct Macro {
  bool function = false;
  std::vector<std::string> params;
  std::vector<Token> body;
};

class Preprocessor {
public:
  std::vector<Token> run(const std::string &source) {
    const std::vector<std::pair<int, std::string>> lines = logicalLines(stripComments(source));
    std::vector<Token> out, chunk;
    struct Cond {
      bool parent, active, taken, sawElse;
    };
    std::vector<Cond> conds;
    auto active = [&] { return conds.empty() || conds.back().active; };
    auto flush = [&] {
      if (chunk.empty()) return;
      std::set<std::string> busy;
      expand(chunk, out, busy);
      chunk.clear();
    };
    int shift = 0; // what #line moved the numbering by
    for (const auto &[physical, text] : lines) {
      const int line = physical + shift;
      size_t k = 0;
      while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
      if (k < text.size() && text[k] == '#') {
        flush();
        std::vector<Token> tokens;
        tokenizeLine(text.substr(k + 1), line, tokens);
        if (tokens.empty()) continue; // a lone '#'
        const std::string directive = tokens[0].text;
        const std::vector<Token> rest(tokens.begin() + 1, tokens.end());
        if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
          const bool parent = active();
          bool value = false;
          if (parent) {
            if (directive == "if") value = evaluateIf(rest, line) != 0;
            else {
              if (rest.empty() || rest[0].kind != Tok::Ident) fail(line, "#" + directive + " without a name");
              value = (macros.count(rest[0].text) != 0) == (directive == "ifdef");
            }
          }
          conds.push_back({parent, parent && value, parent && value, false});
        } else if (directive == "elif") {
          if (conds.empty() || conds.back().sawElse) fail(line, "#elif without #if");
          Cond &c = conds.back();
          const bool value = c.parent && !c.taken && evaluateIf(rest, line) != 0;
          c.active = value;
          c.taken = c.taken || value;
        } else if (directive == "else") {
          if (conds.empty() || conds.back().sawElse) fail(line, "#else without #if");
          Cond &c = conds.back();
          c.active = c.parent && !c.taken;
          c.taken = true;
          c.sawElse = true;
        } else if (directive == "endif") {
          if (conds.empty()) fail(line, "#endif without #if");
          conds.pop_back();
        } else if (!active()) {
          // Inside a part that is left out: nothing else counts.
        } else if (directive == "define") {
          define(text.substr(k + 1), rest, line);
        } else if (directive == "undef") {
          if (rest.empty() || rest[0].kind != Tok::Ident) fail(line, "#undef without a name");
          macros.erase(rest[0].text);
        } else if (directive == "error") {
          fail(line, "#error " + text.substr(text.find("error") + 5));
        } else if (directive == "line") {
          // The next line is numbered as given: a host's wrapper can make an
          // effect's own lines read from 1.
          if (rest.empty() || rest[0].kind != Tok::Int) fail(line, "#line without a number");
          shift = int(rest[0].i) - (physical + 1);
        } else if (directive == "version" || directive == "extension" || directive == "pragma") {
          // The card's matter.
        } else {
          fail(line, "an unknown directive #" + directive);
        }
        continue;
      }
      if (!active()) continue;
      tokenizeLine(text, line, chunk);
    }
    flush();
    if (!conds.empty()) fail(lines.empty() ? 1 : lines.back().first, "an #if without its #endif");
    return out;
  }

private:
  std::map<std::string, Macro> macros;

  static std::string stripComments(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
      if (s.compare(i, 2, "//") == 0) {
        while (i < s.size() && s[i] != '\n') ++i;
      } else if (s.compare(i, 2, "/*") == 0) {
        i += 2;
        out += ' ';
        while (i < s.size() && s.compare(i, 2, "*/") != 0) {
          if (s[i] == '\n') out += '\n';
          ++i;
        }
        i = std::min(s.size(), i + 2);
      } else {
        out += s[i++];
      }
    }
    return out;
  }
  // Lines with their numbers, a backslash at the end joining the next one on.
  static std::vector<std::pair<int, std::string>> logicalLines(const std::string &s) {
    std::vector<std::pair<int, std::string>> lines;
    int number = 1;
    std::string current;
    int start = 1;
    size_t i = 0;
    while (i <= s.size()) {
      const size_t end = s.find('\n', i);
      std::string physical = s.substr(i, end == std::string::npos ? std::string::npos : end - i);
      if (!physical.empty() && physical.back() == '\r') physical.pop_back();
      if (current.empty()) start = number;
      if (!physical.empty() && physical.back() == '\\') {
        physical.pop_back();
        current += physical + ' ';
      } else {
        current += physical;
        lines.emplace_back(start, current);
        current.clear();
      }
      ++number;
      if (end == std::string::npos) break;
      i = end + 1;
    }
    if (!current.empty()) lines.emplace_back(start, current);
    return lines;
  }

  void define(const std::string &text, const std::vector<Token> &rest, int line) {
    if (rest.empty() || rest[0].kind != Tok::Ident) fail(line, "#define without a name");
    Macro m;
    const std::string name = rest[0].text;
    if (name.rfind("GL_", 0) == 0 || name == "defined") fail(line, "#define of a reserved name " + name);
    size_t bodyStart = 1;
    // A function-like macro has its '(' right against its name.
    size_t at = text.find("define") + 6;
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
    if (rest.size() > 1 && rest[1].is("(") && text.compare(at, name.size(), name) == 0 && text.size() > at + name.size() &&
        text[at + name.size()] == '(') {
      m.function = true;
      size_t k = 2;
      if (k < rest.size() && rest[k].is(")")) {
        ++k;
      } else {
        for (;;) {
          if (k >= rest.size() || rest[k].kind != Tok::Ident) fail(line, "a bad parameter list in #define " + name);
          m.params.push_back(rest[k].text);
          ++k;
          if (k < rest.size() && rest[k].is(",")) {
            ++k;
            continue;
          }
          if (k < rest.size() && rest[k].is(")")) {
            ++k;
            break;
          }
          fail(line, "a bad parameter list in #define " + name);
        }
      }
      bodyStart = k;
    }
    m.body.assign(rest.begin() + long(bodyStart), rest.end());
    for (const Token &t : m.body)
      if (t.is("##") || t.is("#")) fail(line, "# and ## are not supported in #define");
    macros[name] = m;
  }

  void expand(const std::vector<Token> &in, std::vector<Token> &out, std::set<std::string> &busy) {
    for (size_t i = 0; i < in.size(); ++i) {
      const Token &t = in[i];
      auto found = t.kind == Tok::Ident ? macros.find(t.text) : macros.end();
      if (found == macros.end() || busy.count(t.text)) {
        out.push_back(t);
        continue;
      }
      const Macro m = found->second; // copied: expanding may not change it, but keep it safe
      if (!m.function) {
        std::vector<Token> body = m.body;
        for (Token &b : body) b.line = t.line;
        busy.insert(t.text);
        expand(body, out, busy);
        busy.erase(t.text);
        continue;
      }
      if (i + 1 >= in.size() || !in[i + 1].is("(")) {
        out.push_back(t); // the name alone, not a call: left as it is
        continue;
      }
      std::vector<std::vector<Token>> args(1);
      int depth = 0;
      size_t j = i + 2;
      for (; j < in.size(); ++j) {
        const Token &a = in[j];
        if (a.is("(")) ++depth;
        if (a.is(")")) {
          if (depth == 0) break;
          --depth;
        }
        if (a.is(",") && depth == 0) {
          args.emplace_back();
          continue;
        }
        args.back().push_back(a);
      }
      if (j >= in.size()) fail(t.line, "the arguments of " + t.text + " are never closed");
      if (args.size() == 1 && args[0].empty() && m.params.empty()) args.clear();
      if (args.size() != m.params.size())
        fail(t.line, t.text + " takes " + std::to_string(m.params.size()) + " argument(s), given " + std::to_string(args.size()));
      std::vector<std::vector<Token>> expanded(args.size());
      for (size_t k = 0; k < args.size(); ++k) expand(args[k], expanded[k], busy);
      std::vector<Token> body;
      for (const Token &b : m.body) {
        if (b.kind == Tok::Ident) {
          const auto p = std::find(m.params.begin(), m.params.end(), b.text);
          if (p != m.params.end()) {
            for (Token a : expanded[size_t(p - m.params.begin())]) {
              a.line = t.line;
              body.push_back(a);
            }
            continue;
          }
        }
        Token c = b;
        c.line = t.line;
        body.push_back(c);
      }
      busy.insert(t.text);
      expand(body, out, busy);
      busy.erase(t.text);
      i = j;
    }
  }

  long long evaluateIf(const std::vector<Token> &tokens, int line) {
    std::vector<Token> resolved;
    for (size_t i = 0; i < tokens.size(); ++i) {
      if (tokens[i].kind == Tok::Ident && tokens[i].text == "defined") {
        std::string name;
        if (i + 1 < tokens.size() && tokens[i + 1].kind == Tok::Ident) {
          name = tokens[++i].text;
        } else if (i + 3 < tokens.size() && tokens[i + 1].is("(") && tokens[i + 2].kind == Tok::Ident && tokens[i + 3].is(")")) {
          name = tokens[i + 2].text;
          i += 3;
        } else {
          fail(line, "a bad defined() in #if");
        }
        Token v;
        v.kind = Tok::Int;
        v.i = macros.count(name) ? 1 : 0;
        v.line = line;
        resolved.push_back(v);
        continue;
      }
      resolved.push_back(tokens[i]);
    }
    std::vector<Token> expanded;
    std::set<std::string> busy;
    expand(resolved, expanded, busy);
    size_t at = 0;
    const long long value = ifExpression(expanded, at, 0, line);
    if (at != expanded.size()) fail(line, "something left over in #if");
    return value;
  }
  long long ifPrimary(const std::vector<Token> &t, size_t &at, int line) {
    if (at >= t.size()) fail(line, "an #if that ends too soon");
    const Token &k = t[at++];
    if (k.is("(")) {
      const long long v = ifExpression(t, at, 0, line);
      if (at >= t.size() || !t[at].is(")")) fail(line, "a ( without its ) in #if");
      ++at;
      return v;
    }
    if (k.is("!")) return !ifPrimary(t, at, line);
    if (k.is("~")) return ~ifPrimary(t, at, line);
    if (k.is("-")) return -ifPrimary(t, at, line);
    if (k.is("+")) return ifPrimary(t, at, line);
    if (k.kind == Tok::Int || k.kind == Tok::UInt) return static_cast<long long>(k.i);
    if (k.kind == Tok::Ident) return 0; // not defined: 0, as in C
    fail(line, "an #if that is not a whole-number expression");
  }
  static int ifPrecedence(const std::string &op) {
    static const std::map<std::string, int> table{{"||", 1}, {"&&", 2}, {"|", 3}, {"^", 4}, {"&", 5}, {"==", 6}, {"!=", 6},
                                                  {"<", 7},  {">", 7},  {"<=", 7}, {">=", 7}, {"<<", 8}, {">>", 8}, {"+", 9},
                                                  {"-", 9},  {"*", 10}, {"/", 10}, {"%", 10}};
    const auto f = table.find(op);
    return f == table.end() ? -1 : f->second;
  }
  long long ifExpression(const std::vector<Token> &t, size_t &at, int minimum, int line) {
    long long left = ifPrimary(t, at, line);
    for (;;) {
      if (at >= t.size() || t[at].kind != Tok::Punct) break;
      if (t[at].is("?")) {
        if (minimum > 0) break;
        ++at;
        const long long a = ifExpression(t, at, 0, line);
        if (at >= t.size() || !t[at].is(":")) fail(line, "a ? without its : in #if");
        ++at;
        const long long b = ifExpression(t, at, 0, line);
        left = left ? a : b;
        continue;
      }
      const int p = ifPrecedence(t[at].text);
      if (p < 0 || p <= minimum - 1 || p < minimum) break;
      const std::string op = t[at++].text;
      const long long right = ifExpression(t, at, p + 1, line);
      if ((op == "/" || op == "%") && right == 0) fail(line, "a division by zero in #if");
      if (op == "||") left = left || right;
      else if (op == "&&") left = left && right;
      else if (op == "|") left |= right;
      else if (op == "^") left ^= right;
      else if (op == "&") left &= right;
      else if (op == "==") left = left == right;
      else if (op == "!=") left = left != right;
      else if (op == "<") left = left < right;
      else if (op == ">") left = left > right;
      else if (op == "<=") left = left <= right;
      else if (op == ">=") left = left >= right;
      else if (op == "<<") left <<= (right & 63);
      else if (op == ">>") left >>= (right & 63);
      else if (op == "+") left += right;
      else if (op == "-") left -= right;
      else if (op == "*") left *= right;
      else if (op == "/") left /= right;
      else if (op == "%") left %= right;
    }
    return left;
  }
};

// =============================================================================
// Syntax tree
// =============================================================================
struct Expr;
struct Stmt;
using ExprP = std::shared_ptr<Expr>;
using StmtP = std::shared_ptr<Stmt>;

struct TypeSpec {
  std::string name;  // "float", "vec3", ...
  int arraySize = 0; // 0: none; -1: [] (sized by its initializer)
  ExprP sizeExpr;    // when the size is an expression
};

struct Expr {
  enum Kind { Int, UInt, Float, Bool, Name, Binary, Unary, Assign, Ternary, Call, Construct, Index, Field, PostInc, PostDec, Comma, Length };
  Kind kind = Int;
  int line = 0;
  std::string op;   // operator, or the name called / the field
  TypeSpec type;    // Construct
  double f = 0;
  uint64_t i = 0;
  std::vector<ExprP> args;
};

struct Declarator {
  std::string name;
  int arraySize = 0; // 0 none, -1 sized by initializer, >0 fixed
  ExprP sizeExpr;
  ExprP init;
  int line = 0;
  int id = -1; // a number for this declaration, for what is known about it
};

struct Stmt {
  enum Kind { Block, Decl, ExprS, If, For, While, Do, Break, Continue, Return, Discard, Empty };
  Kind kind = Empty;
  int line = 0;
  std::vector<StmtP> children; // Block
  TypeSpec type;               // Decl
  bool isConst = false;
  std::vector<Declarator> decls;
  ExprP expr;           // ExprS, Return, If/While/Do condition
  StmtP thenS, elseS;   // If
  StmtP init, body;     // For / While / Do
  ExprP cond, step;     // For
};

struct Param {
  TypeSpec type;
  std::string name;
  enum Qualifier { In, Out, InOut } qualifier = In;
  int id = -1; // as Declarator::id
};

struct Function {
  TypeSpec result;
  std::string name;
  std::vector<Param> params;
  StmtP body; // null for a prototype
  int line = 0;
};

struct GlobalDecl {
  enum Storage { Plain, Const, Uniform, Out, In } storage = Plain;
  StmtP decl; // a Decl statement
};

struct Unit {
  std::vector<std::shared_ptr<Function>> functions;
  std::vector<GlobalDecl> globals;
  int declarators = 0;
};

const std::set<std::string> &typeNames() {
  static const std::set<std::string> names{"void",  "bool",  "int",   "uint",  "float", "vec2",   "vec3",   "vec4",
                                           "ivec2", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4",  "bvec2",  "bvec3",
                                           "bvec4", "mat2",  "mat3",  "mat4",  "mat2x2", "mat3x3", "mat4x4", "sampler2D"};
  return names;
}
bool isPrecision(const std::string &s) { return s == "highp" || s == "mediump" || s == "lowp"; }
bool isInterpolation(const std::string &s) {
  return s == "flat" || s == "smooth" || s == "noperspective" || s == "invariant" || s == "centroid";
}

// =============================================================================
// Parser
// =============================================================================
class Parser {
public:
  explicit Parser(std::vector<Token> tokens) : t(std::move(tokens)) {
    endToken.text = "<end>";
    endToken.line = t.empty() ? 1 : t.back().line;
  }

  Unit unit() {
    Unit u;
    while (!done()) {
      if (peek().is(";")) {
        ++at;
        continue;
      }
      if (peekIdent("precision")) {
        while (!done() && !peek().is(";")) ++at;
        expect(";");
        continue;
      }
      if (peekIdent("struct")) fail(peek().line, "structs are not supported");
      GlobalDecl::Storage storage = GlobalDecl::Plain;
      for (;;) {
        if (peekIdent("layout")) {
          ++at;
          expect("(");
          int depth = 1;
          while (!done() && depth > 0) {
            if (peek().is("(")) ++depth;
            if (peek().is(")")) --depth;
            ++at;
          }
          continue;
        }
        if (peekIdent("const")) storage = GlobalDecl::Const;
        else if (peekIdent("uniform")) storage = GlobalDecl::Uniform;
        else if (peekIdent("out")) storage = GlobalDecl::Out;
        else if (peekIdent("in")) storage = GlobalDecl::In;
        else if (peek().kind == Tok::Ident && (isPrecision(peek().text) || isInterpolation(peek().text))) {
        } else break;
        ++at;
      }
      const int line = peek().line;
      TypeSpec type = typeSpec();
      if (peek().kind != Tok::Ident) fail(line, "a name expected after " + type.name);
      const std::string name = peek().text;
      if (at + 1 < t.size() && t[at + 1].is("(") && storage != GlobalDecl::Uniform) {
        if (storage != GlobalDecl::Plain && storage != GlobalDecl::Const) fail(line, "a function cannot be " + qualifierName(storage));
        u.functions.push_back(function(type, name, line));
        continue;
      }
      GlobalDecl g;
      g.storage = storage;
      g.decl = declarationAfterType(type, storage == GlobalDecl::Const, line);
      u.globals.push_back(g);
    }
    u.declarators = declarators;
    return u;
  }

private:
  std::vector<Token> t;
  size_t at = 0;
  int declarators = 0; // every declaration and parameter gets a number of its own
  Token endToken;

  static std::string qualifierName(GlobalDecl::Storage s) {
    switch (s) {
    case GlobalDecl::Uniform: return "uniform";
    case GlobalDecl::Out: return "out";
    case GlobalDecl::In: return "in";
    default: return "const";
    }
  }
  bool done() const { return at >= t.size(); }
  const Token &peek(size_t ahead = 0) const { return at + ahead < t.size() ? t[at + ahead] : endToken; }
  bool peekIdent(const char *word, size_t ahead = 0) const { return peek(ahead).kind == Tok::Ident && peek(ahead).text == word; }
  void expect(const char *p) {
    if (!peek().is(p)) fail(peek().line, std::string("'") + p + "' expected, found '" + peek().text + "'");
    ++at;
  }
  bool accept(const char *p) {
    if (!peek().is(p)) return false;
    ++at;
    return true;
  }
  bool isTypeAt(size_t ahead) const {
    size_t k = ahead;
    while (peek(k).kind == Tok::Ident && isPrecision(peek(k).text)) ++k;
    return peek(k).kind == Tok::Ident && typeNames().count(peek(k).text);
  }

  TypeSpec typeSpec() {
    while (peek().kind == Tok::Ident && isPrecision(peek().text)) ++at;
    const Token &k = peek();
    if (k.kind != Tok::Ident || !typeNames().count(k.text)) {
      if (k.kind == Tok::Ident) fail(k.line, "unknown type '" + k.text + "'");
      fail(k.line, "a type expected, found '" + k.text + "'");
    }
    TypeSpec s;
    s.name = k.text;
    if (s.name == "mat2x2") s.name = "mat2";
    if (s.name == "mat3x3") s.name = "mat3";
    if (s.name == "mat4x4") s.name = "mat4";
    ++at;
    if (peek().is("[")) {
      ++at;
      if (accept("]")) s.arraySize = -1;
      else {
        s.sizeExpr = expression();
        s.arraySize = 1;
        expect("]");
      }
    }
    return s;
  }

  std::shared_ptr<Function> function(const TypeSpec &result, const std::string &name, int line) {
    auto f = std::make_shared<Function>();
    f->result = result;
    f->name = name;
    f->line = line;
    ++at; // name
    expect("(");
    if (peekIdent("void") && peek(1).is(")")) at += 1;
    if (!peek().is(")")) {
      for (;;) {
        Param p;
        for (;;) {
          if (peekIdent("const")) ++at;
          else if (peekIdent("in")) { p.qualifier = Param::In; ++at; }
          else if (peekIdent("out")) { p.qualifier = Param::Out; ++at; }
          else if (peekIdent("inout")) { p.qualifier = Param::InOut; ++at; }
          else if (peek().kind == Tok::Ident && isPrecision(peek().text)) ++at;
          else break;
        }
        p.type = typeSpec();
        if (peek().kind == Tok::Ident) {
          p.name = peek().text;
          ++at;
          if (accept("[")) {
            if (p.type.arraySize) fail(peek().line, "an array of arrays");
            p.type.sizeExpr = expression();
            p.type.arraySize = 1;
            expect("]");
          }
        }
        p.id = declarators++;
        f->params.push_back(p);
        if (accept(",")) continue;
        break;
      }
    }
    expect(")");
    if (accept(";")) return f; // a prototype
    f->body = block();
    return f;
  }

  StmtP declarationAfterType(const TypeSpec &type, bool isConst, int line) {
    auto s = std::make_shared<Stmt>();
    s->kind = Stmt::Decl;
    s->line = line;
    s->type = type;
    s->isConst = isConst;
    for (;;) {
      Declarator d;
      d.line = peek().line;
      if (peek().kind != Tok::Ident) fail(peek().line, "a name expected in a declaration");
      d.name = peek().text;
      ++at;
      if (accept("[")) {
        if (type.arraySize) fail(d.line, "an array of arrays");
        if (accept("]")) d.arraySize = -1;
        else {
          d.sizeExpr = expression();
          d.arraySize = 1;
          expect("]");
        }
      }
      if (accept("=")) d.init = assignment();
      d.id = declarators++;
      s->decls.push_back(d);
      if (accept(",")) continue;
      break;
    }
    expect(";");
    return s;
  }
  StmtP block() {
    auto s = std::make_shared<Stmt>();
    s->kind = Stmt::Block;
    s->line = peek().line;
    expect("{");
    while (!peek().is("}")) {
      if (done()) fail(s->line, "a { without its }");
      s->children.push_back(statement());
    }
    ++at;
    return s;
  }

  StmtP statement() {
    const Token &k = peek();
    if (k.is("{")) return block();
    if (k.is(";")) {
      ++at;
      auto s = std::make_shared<Stmt>();
      s->kind = Stmt::Empty;
      s->line = k.line;
      return s;
    }
    auto s = std::make_shared<Stmt>();
    s->line = k.line;
    if (k.kind == Tok::Ident) {
      if (k.text == "if") {
        ++at;
        s->kind = Stmt::If;
        expect("(");
        s->expr = expression();
        expect(")");
        s->thenS = statement();
        if (peekIdent("else")) {
          ++at;
          s->elseS = statement();
        }
        return s;
      }
      if (k.text == "for") {
        ++at;
        s->kind = Stmt::For;
        expect("(");
        if (accept(";")) {
          s->init = nullptr;
        } else {
          s->init = simpleStatement();
        }
        if (!peek().is(";")) s->cond = expression();
        expect(";");
        if (!peek().is(")")) s->step = expression();
        expect(")");
        s->body = statement();
        return s;
      }
      if (k.text == "while") {
        ++at;
        s->kind = Stmt::While;
        expect("(");
        s->expr = expression();
        expect(")");
        s->body = statement();
        return s;
      }
      if (k.text == "do") {
        ++at;
        s->kind = Stmt::Do;
        s->body = statement();
        if (!peekIdent("while")) fail(peek().line, "'while' expected after do");
        ++at;
        expect("(");
        s->expr = expression();
        expect(")");
        expect(";");
        return s;
      }
      if (k.text == "return") {
        ++at;
        s->kind = Stmt::Return;
        if (!peek().is(";")) s->expr = expression();
        expect(";");
        return s;
      }
      if (k.text == "break" || k.text == "continue" || k.text == "discard") {
        ++at;
        s->kind = k.text == "break" ? Stmt::Break : k.text == "continue" ? Stmt::Continue : Stmt::Discard;
        expect(";");
        return s;
      }
      if (k.text == "switch" || k.text == "case" || k.text == "default") fail(k.line, "switch is not supported: use if and else");
      if (k.text == "struct") fail(k.line, "structs are not supported");
    }
    return simpleStatement();
  }

  // A declaration or an expression, up to and including its ';'.
  StmtP simpleStatement() {
    const int line = peek().line;
    bool isConst = false;
    size_t k = 0;
    if (peekIdent("const")) {
      isConst = true;
      k = 1;
    }
    if (isTypeAt(k)) {
      // "vec3(" and "float[3](" begin expressions; "vec3 x" and "float[3] x" declarations.
      size_t m = k;
      while (peek(m).kind == Tok::Ident && isPrecision(peek(m).text)) ++m;
      ++m; // the type
      bool declaration = peek(m).kind == Tok::Ident;
      if (peek(m).is("[")) {
        size_t depth = 0, n = m;
        for (;; ++n) {
          if (peek(n).is("[")) ++depth;
          else if (peek(n).is("]") && --depth == 0) break;
          if (at + n >= t.size()) break;
        }
        declaration = peek(n + 1).kind == Tok::Ident;
      }
      if (declaration || isConst) {
        if (isConst) ++at;
        TypeSpec type = typeSpec();
        return declarationAfterType(type, isConst, line);
      }
    }
    auto s = std::make_shared<Stmt>();
    s->kind = Stmt::ExprS;
    s->line = line;
    s->expr = expression();
    expect(";");
    return s;
  }

  // ---- expressions ----------------------------------------------------------
  ExprP make(Expr::Kind kind, int line, const std::string &op = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = kind;
    e->line = line;
    e->op = op;
    return e;
  }

public:
  ExprP expression() {
    ExprP left = assignment();
    while (peek().is(",")) {
      const int line = peek().line;
      ++at;
      ExprP c = make(Expr::Comma, line, ",");
      c->args = {left, assignment()};
      left = c;
    }
    return left;
  }

private:
  ExprP assignment() {
    ExprP left = conditional();
    static const std::set<std::string> ops{"=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "^=", "|="};
    if (peek().kind == Tok::Punct && ops.count(peek().text)) {
      const Token op = peek();
      ++at;
      ExprP a = make(Expr::Assign, op.line, op.text);
      a->args = {left, assignment()};
      return a;
    }
    return left;
  }
  ExprP conditional() {
    ExprP c = binary(1);
    if (peek().is("?")) {
      const int line = peek().line;
      ++at;
      ExprP a = expression();
      expect(":");
      ExprP b = assignment();
      ExprP e = make(Expr::Ternary, line, "?");
      e->args = {c, a, b};
      return e;
    }
    return c;
  }
  static int precedence(const std::string &op) {
    static const std::map<std::string, int> table{{"||", 1}, {"^^", 2}, {"&&", 3}, {"|", 4},  {"^", 5},  {"&", 6},
                                                  {"==", 7}, {"!=", 7}, {"<", 8},  {">", 8},  {"<=", 8}, {">=", 8},
                                                  {"<<", 9}, {">>", 9}, {"+", 10}, {"-", 10}, {"*", 11}, {"/", 11}, {"%", 11}};
    const auto f = table.find(op);
    return f == table.end() ? -1 : f->second;
  }
  ExprP binary(int minimum) {
    ExprP left = unary();
    for (;;) {
      if (peek().kind != Tok::Punct) break;
      const int p = precedence(peek().text);
      if (p < minimum) break;
      const Token op = peek();
      ++at;
      ExprP right = binary(p + 1);
      ExprP e = make(Expr::Binary, op.line, op.text);
      e->args = {left, right};
      left = e;
    }
    return left;
  }
  ExprP unary() {
    const Token &k = peek();
    if (k.kind == Tok::Punct && (k.text == "++" || k.text == "--" || k.text == "+" || k.text == "-" || k.text == "!" || k.text == "~")) {
      ++at;
      ExprP e = make(Expr::Unary, k.line, k.text);
      e->args = {unary()};
      return e;
    }
    return postfix(primary());
  }
  ExprP postfix(ExprP e) {
    for (;;) {
      const Token &k = peek();
      if (k.is("[")) {
        ++at;
        ExprP i = make(Expr::Index, k.line, "[]");
        i->args = {e, expression()};
        expect("]");
        e = i;
      } else if (k.is(".")) {
        ++at;
        if (peek().kind != Tok::Ident) fail(peek().line, "a name expected after '.'");
        const std::string field = peek().text;
        ++at;
        if (field == "length" && peek().is("(")) {
          ++at;
          expect(")");
          ExprP l = make(Expr::Length, k.line, "length");
          l->args = {e};
          e = l;
        } else {
          ExprP f = make(Expr::Field, k.line, field);
          f->args = {e};
          e = f;
        }
      } else if (k.is("++") || k.is("--")) {
        ++at;
        ExprP p = make(k.is("++") ? Expr::PostInc : Expr::PostDec, k.line, k.text);
        p->args = {e};
        e = p;
      } else {
        break;
      }
    }
    return e;
  }
  std::vector<ExprP> arguments() {
    std::vector<ExprP> args;
    expect("(");
    if (peekIdent("void") && peek(1).is(")")) ++at;
    if (!peek().is(")")) {
      for (;;) {
        args.push_back(assignment());
        if (accept(",")) continue;
        break;
      }
    }
    expect(")");
    return args;
  }
  ExprP primary() {
    const Token k = peek();
    if (k.kind == Tok::Int || k.kind == Tok::UInt) {
      ++at;
      ExprP e = make(k.kind == Tok::Int ? Expr::Int : Expr::UInt, k.line);
      e->i = k.i;
      return e;
    }
    if (k.kind == Tok::Float) {
      ++at;
      ExprP e = make(Expr::Float, k.line);
      e->f = k.f;
      return e;
    }
    if (k.is("(")) {
      ++at;
      ExprP e = expression();
      expect(")");
      return e;
    }
    if (k.kind == Tok::Ident) {
      if (k.text == "true" || k.text == "false") {
        ++at;
        ExprP e = make(Expr::Bool, k.line);
        e->i = k.text == "true";
        return e;
      }
      if (isTypeAt(0)) {
        TypeSpec type = typeSpec();
        if (!peek().is("(")) fail(k.line, "'(' expected after the type " + type.name);
        ExprP e = make(Expr::Construct, k.line, type.name);
        e->type = type;
        e->args = arguments();
        return e;
      }
      ++at;
      if (peek().is("(")) {
        ExprP e = make(Expr::Call, k.line, k.text);
        e->args = arguments();
        return e;
      }
      return make(Expr::Name, k.line, k.text);
    }
    fail(k.line, "an expression expected, found '" + k.text + "'");
  }
};

// =============================================================================
// Types and values
// =============================================================================
enum class K : uint8_t { Void, Bool, Int, UInt, Float, Sampler };

struct Type {
  K k = K::Void;
  int rows = 1, cols = 1; // a vector has rows 2..4; a matrix cols 2..4 of `rows` each
  int array = 0;          // 0: not an array
  int element() const { return rows * cols; }
  int cells() const { return k == K::Void ? 0 : element() * std::max(1, array); }
  bool scalar() const { return rows == 1 && cols == 1 && array == 0 && k != K::Void && k != K::Sampler; }
  bool vector() const { return rows > 1 && cols == 1 && array == 0; }
  bool matrix() const { return cols > 1 && array == 0; }
  bool numeric() const { return k == K::Int || k == K::UInt || k == K::Float; }
  bool integer() const { return k == K::Int || k == K::UInt; }
  bool operator==(const Type &o) const { return k == o.k && rows == o.rows && cols == o.cols && array == o.array; }
  bool operator!=(const Type &o) const { return !(*this == o); }
  Type elementType() const {
    Type e = *this;
    e.array = 0;
    return e;
  }
  Type withKind(K kind) const {
    Type e = *this;
    e.k = kind;
    return e;
  }
  Type column() const {
    Type c = *this;
    c.cols = 1;
    c.array = 0;
    return c;
  }
  Type scalarType() const { return Type{k, 1, 1, 0}; }
  std::string name() const {
    std::string base;
    if (k == K::Void) return "void";
    if (k == K::Sampler) return "sampler2D";
    if (cols > 1) base = "mat" + std::to_string(cols);
    else if (rows == 1) base = k == K::Bool ? "bool" : k == K::Int ? "int" : k == K::UInt ? "uint" : "float";
    else base = std::string(k == K::Bool ? "bvec" : k == K::Int ? "ivec" : k == K::UInt ? "uvec" : "vec") + std::to_string(rows);
    if (array) base += "[" + std::to_string(array) + "]";
    return base;
  }
};
Type scalarOf(K k) { return Type{k, 1, 1, 0}; }
Type vectorOf(K k, int n) { return Type{k, n, 1, 0}; }
const Type kFloat = scalarOf(K::Float), kInt = scalarOf(K::Int), kUInt = scalarOf(K::UInt), kBool = scalarOf(K::Bool);
const Type kVoid{};

Type typeFromName(const std::string &n, int line) {
  if (n == "void") return kVoid;
  if (n == "bool") return kBool;
  if (n == "int") return kInt;
  if (n == "uint") return kUInt;
  if (n == "float") return kFloat;
  if (n == "sampler2D") return Type{K::Sampler, 1, 1, 0};
  auto tail = [&](size_t at) { return n[at] - '0'; };
  if (n.rfind("vec", 0) == 0) return vectorOf(K::Float, tail(3));
  if (n.rfind("ivec", 0) == 0) return vectorOf(K::Int, tail(4));
  if (n.rfind("uvec", 0) == 0) return vectorOf(K::UInt, tail(4));
  if (n.rfind("bvec", 0) == 0) return vectorOf(K::Bool, tail(4));
  if (n.rfind("mat", 0) == 0) return Type{K::Float, tail(3), tail(3), 0};
  fail(line, "unknown type " + n);
}

// The numbers a constant is made of, as their bits.
inline uint32_t fbits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
inline float bitsf(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// =============================================================================
// Instructions
// =============================================================================
enum Op : uint16_t {
  MOV, STORE, SEL,
  FADD, FSUB, FMUL, FDIV, FNEG, FMIN, FMAX, FABS, FSIGN, FFLOOR, FCEIL, FFRACT, FTRUNC, FROUND, FROUNDEVEN,
  FSQRT, FINVSQRT, FSIN, FCOS, FTAN, FASIN, FACOS, FATAN, FATAN2, FSINH, FCOSH, FTANH, FASINH, FACOSH, FATANH,
  FEXP, FLOG, FEXP2, FLOG2, FPOW, FMOD, FSTEP, FMIX, FCLAMP, FSMOOTH, FISNAN, FISINF,
  FLT, FLE, FGT, FGE, FEQ, FNE,
  IADD, ISUB, IMUL, IDIV, IMOD, INEG, IABS, ISIGN, IMIN, IMAX, ICLAMP,
  UDIV, UMOD, UMIN, UMAX, UCLAMP,
  IAND, IOR, IXOR, INOT, ISHL, ISHR, USHR,
  ILT, ILE, IGT, IGE, IEQ, INE, ULT, ULE, UGT, UGE,
  BNOT,
  I2F, U2F, F2I, F2U, F2B, I2B,
  GATHER, SCATTER, UVALUE, UGATHER, HOST, TEXEL, TEXTURE,
  OP_COUNT
};
// What HOST reads.
enum Host : int32_t {
  H_SECONDS, H_FRAME, H_SCALE, H_BOX0, H_BOX1, H_BOX2, H_BOX3, H_DEPTH, H_PASS, H_PASSES,
  H_SOURCE_W, H_SOURCE_H, H_ORIGINAL_W, H_ORIGINAL_H, H_COUNT
};

struct Ins {
  Op op = MOV;
  int32_t d = 0, a = 0, b = 0, c = 0, e = 0;
};

// ---- the arithmetic, one lane at a time; shared by the VM and by constant folding ----
inline float glslMod(float x, float y) { return x - y * std::floor(x / y); }
inline float glslSign(float x) { return x > 0 ? 1.f : x < 0 ? -1.f : 0.f; }
inline float glslRoundEven(float x) { return std::nearbyint(x); }
inline float glslRound(float x) { return std::round(x); }
inline float glslFract(float x) { return x - std::floor(x); }
inline float glslInvSqrt(float x) { return 1.f / std::sqrt(x); }
inline float glslMix(float a, float b, float t) { return a * (1.f - t) + b * t; }
inline float glslClamp(float x, float lo, float hi) { return std::min(std::max(x, lo), hi); }
inline float glslSmooth(float e0, float e1, float x) {
  const float t = glslClamp((x - e0) / (e1 - e0), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}
inline float glslStep(float edge, float x) { return x < edge ? 0.f : 1.f; }
// The card turns a float into a whole number by cutting towards zero, holding
// it to the range and making NaN 0 -- never the processor's "integer
// indefinite", and never undefined behaviour.
inline int32_t floatToInt(float f) {
  if (!(f == f)) return 0;
  if (f >= 2147483647.f) return std::numeric_limits<int32_t>::max();
  if (f <= -2147483648.f) return std::numeric_limits<int32_t>::min();
  return static_cast<int32_t>(f);
}
inline uint32_t floatToUInt(float f) {
  if (!(f == f) || f <= 0.f) return 0;
  if (f >= 4294967295.f) return 0xffffffffu;
  return static_cast<uint32_t>(f);
}
inline int32_t idiv(int32_t a, int32_t b) {
  if (b == 0) return a < 0 ? std::numeric_limits<int32_t>::min() : std::numeric_limits<int32_t>::max();
  if (a == std::numeric_limits<int32_t>::min() && b == -1) return a;
  return a / b;
}
inline int32_t imod(int32_t a, int32_t b) {
  if (b == 0) return a;
  if (a == std::numeric_limits<int32_t>::min() && b == -1) return 0;
  return a % b;
}
inline uint32_t udiv(uint32_t a, uint32_t b) { return b == 0 ? 0xffffffffu : a / b; }
inline uint32_t umod(uint32_t a, uint32_t b) { return b == 0 ? a : a % b; }
inline int32_t ishl(int32_t a, int32_t b) {
  const uint32_t s = static_cast<uint32_t>(b);
  return s >= 32 ? 0 : static_cast<int32_t>(static_cast<uint32_t>(a) << s);
}
inline int32_t ishr(int32_t a, int32_t b) {
  const uint32_t s = static_cast<uint32_t>(b);
  return s >= 32 ? (a < 0 ? -1 : 0) : (a >> s);
}
inline uint32_t ushr(uint32_t a, uint32_t b) { return b >= 32 ? 0 : a >> b; }

// One lane of an instruction on 32-bit cells: what constant folding uses.
uint32_t foldLane(Op op, uint32_t a, uint32_t b, uint32_t c) {
  const float fa = bitsf(a), fb = bitsf(b), fc = bitsf(c);
  const int32_t ia = static_cast<int32_t>(a), ib = static_cast<int32_t>(b), ic = static_cast<int32_t>(c);
  auto F = [](float f) { return fbits(f); };
  auto I = [](int32_t i) { return static_cast<uint32_t>(i); };
  switch (op) {
  case MOV: return a;
  case SEL: return a ? b : c;
  case FADD: return F(fa + fb);
  case FSUB: return F(fa - fb);
  case FMUL: return F(fa * fb);
  case FDIV: return F(fa / fb);
  case FNEG: return F(-fa);
  case FMIN: return F(std::min(fa, fb));
  case FMAX: return F(std::max(fa, fb));
  case FABS: return F(std::fabs(fa));
  case FSIGN: return F(glslSign(fa));
  case FFLOOR: return F(std::floor(fa));
  case FCEIL: return F(std::ceil(fa));
  case FFRACT: return F(glslFract(fa));
  case FTRUNC: return F(std::trunc(fa));
  case FROUND: return F(glslRound(fa));
  case FROUNDEVEN: return F(glslRoundEven(fa));
  case FSQRT: return F(std::sqrt(fa));
  case FINVSQRT: return F(glslInvSqrt(fa));
  case FSIN: return F(std::sin(fa));
  case FCOS: return F(std::cos(fa));
  case FTAN: return F(std::tan(fa));
  case FASIN: return F(std::asin(fa));
  case FACOS: return F(std::acos(fa));
  case FATAN: return F(std::atan(fa));
  case FATAN2: return F(std::atan2(fa, fb));
  case FSINH: return F(std::sinh(fa));
  case FCOSH: return F(std::cosh(fa));
  case FTANH: return F(std::tanh(fa));
  case FASINH: return F(std::asinh(fa));
  case FACOSH: return F(std::acosh(fa));
  case FATANH: return F(std::atanh(fa));
  case FEXP: return F(std::exp(fa));
  case FLOG: return F(std::log(fa));
  case FEXP2: return F(std::exp2(fa));
  case FLOG2: return F(std::log2(fa));
  case FPOW: return F(std::pow(fa, fb));
  case FMOD: return F(glslMod(fa, fb));
  case FSTEP: return F(glslStep(fa, fb));
  case FMIX: return F(glslMix(fa, fb, fc));
  case FCLAMP: return F(glslClamp(fa, fb, fc));
  case FSMOOTH: return F(glslSmooth(fa, fb, fc));
  case FISNAN: return std::isnan(fa) ? 1u : 0u;
  case FISINF: return std::isinf(fa) ? 1u : 0u;
  case FLT: return fa < fb;
  case FLE: return fa <= fb;
  case FGT: return fa > fb;
  case FGE: return fa >= fb;
  case FEQ: return fa == fb;
  case FNE: return fa != fb;
  case IADD: return a + b;
  case ISUB: return a - b;
  case IMUL: return a * b;
  case IDIV: return I(idiv(ia, ib));
  case IMOD: return I(imod(ia, ib));
  case INEG: return 0u - a;
  case IABS: return ia < 0 ? 0u - a : a;
  case ISIGN: return I(ia > 0 ? 1 : ia < 0 ? -1 : 0);
  case IMIN: return I(std::min(ia, ib));
  case IMAX: return I(std::max(ia, ib));
  case ICLAMP: return I(std::min(std::max(ia, ib), ic));
  case UDIV: return udiv(a, b);
  case UMOD: return umod(a, b);
  case UMIN: return std::min(a, b);
  case UMAX: return std::max(a, b);
  case UCLAMP: return std::min(std::max(a, b), c);
  case IAND: return a & b;
  case IOR: return a | b;
  case IXOR: return a ^ b;
  case INOT: return ~a;
  case ISHL: return I(ishl(ia, ib));
  case ISHR: return I(ishr(ia, ib));
  case USHR: return ushr(a, b);
  case ILT: return ia < ib;
  case ILE: return ia <= ib;
  case IGT: return ia > ib;
  case IGE: return ia >= ib;
  case IEQ: return a == b;
  case INE: return a != b;
  case ULT: return a < b;
  case ULE: return a <= b;
  case UGT: return a > b;
  case UGE: return a >= b;
  case BNOT: return a ? 0u : 1u;
  case I2F: return F(float(ia));
  case U2F: return F(float(a));
  case F2I: return I(floatToInt(fa));
  case F2U: return floatToUInt(fa);
  case F2B: return fa != 0.f;
  case I2B: return a != 0;
  default: break;
  }
  (void)ic;
  (void)fc;
  return 0;
}

// =============================================================================
// The program
// =============================================================================
struct Block;
struct Node {
  enum T : uint8_t { Code, If, Loop, Call, Break, Continue, Return, Discard } t = Code;
  std::vector<Ins> code;
  int cond = -1;                       // If: a register holding 0 or 1
  std::unique_ptr<Block> a, b, c;      // If: then, else. Loop: cond, body, step. Call: body
  int condReg = -1;                    // Loop
  bool condFirst = true;               // Loop: for and while; do tests last
  int loop = -1;                       // Loop: its number; Break/Continue: the loop they leave
};
struct Block {
  std::vector<Node> nodes;
};

} // namespace

struct Program {
  uint64_t serial = 0; // never reused, unlike an address: what a thread's machines are kept by
  int regs = 0;
  std::vector<std::pair<int, uint32_t>> constants; // filled when a worker takes the program
  std::vector<Ins> prologue;                        // once per run: what only the inputs decide
  Block body;                                       // per B pixels
  std::vector<int> lists;                           // register lists for GATHER, SCATTER, TEXEL
  int loops = 0;
  bool isMain = false;
  int result[4] = {-1, -1, -1, -1};
  int value = -1;
  K valueKind = K::Float;
  int instructions = 0;
};

namespace {

// =============================================================================
// Compiler
// =============================================================================
struct Val {
  Type type;
  std::vector<int> regs;
  bool isConst = false;
  std::vector<uint32_t> konst;
  bool uniform = false;   // the same in every lane for the whole run
  bool owns = false;      // fresh registers nobody else reads
  bool immutable = false; // registers never written again
  bool global = false;    // a global variable's own registers
};

// An assignable place: registers, or an array element picked at run time.
struct Symbol;
struct Place {
  Type type;
  std::vector<int> regs;         // static: one per slot
  bool dynamic = false;          // an array element chosen by `index`
  std::vector<int> arrayRegs;    // dynamic: every slot of the array
  int elementSlots = 0;
  int index = -1;                // dynamic: the register holding the element number
  std::vector<int> components;   // dynamic: which slots of the element (after a swizzle)
  bool global = false;
  int declaration = -1;          // the variable's declaration, when it is one
  Symbol *symbol = nullptr;      // static: the variable, and which of its slots
  std::vector<int> positions;
};

struct Symbol {
  enum Kind { Variable, Values, HostValue, Sampler, Output, Input } kind = Variable;
  Type type;
  std::vector<int> regs;
  bool immutable = false;
  bool global = false;
  bool isConst = false;
  std::vector<uint32_t> konst;
  bool uniform = false;
  std::vector<int> host; // HostValue: what each slot reads
  int sampler = -1;
  int declaration = -1;
  std::vector<int> owned; // a statement's registers it took over, given back when its scope ends
  int depth = 0;          // the branches and loops around its declaration
};

struct LoopFrame {
  int number;
};
struct FunctionFrame {
  const Function *f;
  std::vector<int> result; // registers for the returned value
  Type type;
  // A function whose only return is its last statement hands that value
  // straight to the caller, its registers and all, instead of copying it.
  bool tail = false;
  bool forwarded = false;
  Val value;
};

class Compiler {
public:
  explicit Compiler(const Unit &unit) : u(unit) {}

  std::shared_ptr<Program> compile(const std::string &entry) {
    static std::atomic<uint64_t> serials{0};
    p = std::make_shared<Program>();
    p->serial = ++serials;
    current = &p->body;
    for (const auto &f : u.functions) functions[f->name].push_back(f.get());
    markWrites();
    scopes.emplace_back();
    // gl_FragCoord: registers 0..3, set for every B pixels.
    {
      Symbol s;
      s.kind = Symbol::Input;
      s.type = vectorOf(K::Float, 4);
      s.regs = {next++, next++, next++, next++};
      s.immutable = true;
      scopes.back()["gl_FragCoord"] = s;
    }
    declareGlobals();
    const auto found = functions.find(entry);
    if (found == functions.end()) fail(1, "no function called " + entry);
    const Function *f = nullptr;
    for (const Function *candidate : found->second)
      if (candidate->body && candidate->params.empty()) f = candidate;
    if (!f) fail(found->second.front()->line, entry + " must take nothing");
    const Type resultType = typeOf(f->result, f->line);
    if (entry == "main") {
      if (resultType != kVoid) fail(f->line, "main must return void");
      const auto out = scopes.front().find("result");
      if (out == scopes.front().end() || out->second.kind != Symbol::Output) fail(f->line, "main writes no 'out vec4 result'");
      for (int k = 0; k < 4; ++k) p->result[k] = out->second.regs[size_t(k)];
      p->isMain = true;
    } else if (!resultType.scalar()) {
      fail(f->line, entry + " must return a bool, int, uint or float");
    }
    // Global variables are set up for every pixel, before the entry runs.
    for (const auto &init : globalInits) init();
    Val returned = call(*f, {}, f->line);
    if (!p->isMain) {
      Val v = materialize(returned);
      p->value = v.regs[0];
      p->valueKind = v.type.k;
      // Kept: the value must outlive the call it came from.
      pinned.insert(p->value);
    }
    p->regs = next;
    p->instructions = int(p->prologue.size()) + countInstructions(p->body);
    return p;
  }

private:
  const Unit &u;
  std::shared_ptr<Program> p;
  Block *current = nullptr;
  int next = 0;
  std::vector<int> freeRegs;
  std::set<int> pinned;
  std::deque<std::map<std::string, Symbol>> scopes; // a deque: a symbol stays where it is while its scope lives
  std::map<std::string, std::vector<const Function *>> functions;
  std::vector<std::function<void()>> globalInits;
  std::vector<LoopFrame> loops;
  std::vector<FunctionFrame> frames;
  std::vector<const Function *> callStack;
  std::map<uint32_t, int> constantRegs;
  std::set<int> written;          // declarations written after they are declared
  std::map<std::string, std::vector<bool>> outParams; // function name -> positions taking out/inout
  std::map<std::pair<int, std::vector<uint32_t>>, std::vector<int>> hostCache;
  std::map<int, int> valueCache;  // values[] constant index -> register
  std::vector<std::vector<int>> statementTemps; // temporaries made by the statements under way
  // How many branches and loop blocks the code being written is inside: a
  // value assigned at its variable's own depth reaches every lane that will
  // read the variable again, so the variable can simply take its registers.
  int depth = 0;
  struct Deeper {
    int &depth;
    explicit Deeper(int &d) : depth(d) { ++depth; }
    ~Deeper() { --depth; }
  };

  static int countInstructions(const Block &b) {
    int n = 0;
    for (const Node &node : b.nodes) {
      n += int(node.code.size());
      for (const auto *child : {node.a.get(), node.b.get(), node.c.get()})
        if (child) n += countInstructions(*child);
    }
    return n;
  }

  // ---- what is written where -------------------------------------------------
  // A variable written only by its declaration can share its initializer's
  // registers, and is as uniform as that is; a parameter never written can share
  // its argument's. So: which declarations are written anywhere else?
  void markWrites() {
    for (const auto &f : u.functions)
      for (size_t i = 0; i < f->params.size(); ++i) {
        auto &positions = outParams[f->name];
        if (positions.size() < f->params.size()) positions.resize(f->params.size(), false);
        if (f->params[i].qualifier != Param::In) positions[i] = true;
      }
    std::vector<std::map<std::string, int>> names(1);
    for (const auto &g : u.globals)
      for (const auto &d : g.decl->decls) names.back()[d.name] = d.id;
    for (const auto &f : u.functions) {
      if (!f->body) continue;
      names.emplace_back();
      for (const auto &prm : f->params)
        if (!prm.name.empty()) names.back()[prm.name] = prm.id;
      walkStatement(f->body.get(), names);
      names.pop_back();
    }
  }
  static int lookupName(const std::vector<std::map<std::string, int>> &names, const std::string &n) {
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
      const auto f = it->find(n);
      if (f != it->end()) return f->second;
    }
    return -1;
  }
  void markTarget(const Expr *e, const std::vector<std::map<std::string, int>> &names) {
    while (e && (e->kind == Expr::Index || e->kind == Expr::Field)) {
      if (e->kind == Expr::Index) walkExpr(e->args[1].get(), names);
      e = e->args[0].get();
    }
    if (e && e->kind == Expr::Name) {
      const int id = lookupName(names, e->op);
      if (id >= 0) written.insert(id);
    }
  }
  void walkExpr(const Expr *e, const std::vector<std::map<std::string, int>> &names) {
    if (!e) return;
    if (e->kind == Expr::Assign) markTarget(e->args[0].get(), names);
    if (e->kind == Expr::PostInc || e->kind == Expr::PostDec || (e->kind == Expr::Unary && (e->op == "++" || e->op == "--")))
      markTarget(e->args[0].get(), names);
    if (e->kind == Expr::Call) {
      const auto f = outParams.find(e->op);
      if (f != outParams.end())
        for (size_t i = 0; i < e->args.size() && i < f->second.size(); ++i)
          if (f->second[i]) markTarget(e->args[i].get(), names);
      if (e->op == "modf" && e->args.size() == 2) markTarget(e->args[1].get(), names);
    }
    for (const auto &a : e->args) walkExpr(a.get(), names);
  }
  void walkStatement(const Stmt *s, std::vector<std::map<std::string, int>> &names) {
    if (!s) return;
    switch (s->kind) {
    case Stmt::Block:
      names.emplace_back();
      for (const auto &c : s->children) walkStatement(c.get(), names);
      names.pop_back();
      break;
    case Stmt::Decl:
      for (const auto &d : s->decls) {
        walkExpr(d.init.get(), names);
        walkExpr(d.sizeExpr.get(), names);
        names.back()[d.name] = d.id;
      }
      break;
    case Stmt::For:
      names.emplace_back();
      walkStatement(s->init.get(), names);
      walkExpr(s->cond.get(), names);
      walkExpr(s->step.get(), names);
      walkStatement(s->body.get(), names);
      names.pop_back();
      break;
    default:
      walkExpr(s->expr.get(), names);
      walkExpr(s->cond.get(), names);
      walkExpr(s->step.get(), names);
      walkStatement(s->init.get(), names);
      walkStatement(s->thenS.get(), names);
      walkStatement(s->elseS.get(), names);
      walkStatement(s->body.get(), names);
      break;
    }
  }

  // ---- registers ----------------------------------------------------------------
  int fresh() {
    int r;
    if (!freeRegs.empty()) {
      r = freeRegs.back();
      freeRegs.pop_back();
    } else {
      r = next++;
    }
    if (!statementTemps.empty()) statementTemps.back().push_back(r);
    return r;
  }
  int keep() { return next++; } // never given back: uniform values, variables that live on
  std::vector<int> contiguous(int n) {
    std::vector<int> r;
    for (int i = 0; i < n; ++i) r.push_back(next++);
    return r;
  }
  struct StatementTemps {
    Compiler &c;
    explicit StatementTemps(Compiler &compiler) : c(compiler) { c.statementTemps.emplace_back(); }
    ~StatementTemps() {
      if (c.statementTemps.empty()) return;
      std::vector<int> made = std::move(c.statementTemps.back());
      c.statementTemps.pop_back();
      for (int r : made)
        if (!c.pinned.count(r)) c.freeRegs.push_back(r);
    }
  };
  void pin(const std::vector<int> &regs) {
    for (int r : regs) pinned.insert(r);
  }
  // A value's fresh registers, taken over by a variable that is only ever that
  // value: no longer the statement's to free when it ends, but the variable's,
  // given back when its scope does. Each register once, however many slots
  // share it (vec3(x) is x three times).
  std::vector<int> adopt(const Val &v) {
    std::vector<int> taken;
    if (!v.owns) return taken;
    for (int r : v.regs) {
      if (std::find(taken.begin(), taken.end(), r) != taken.end()) continue;
      for (auto &list : statementTemps) {
        const auto at = std::find(list.begin(), list.end(), r);
        if (at == list.end()) continue;
        list.erase(at);
        taken.push_back(r);
        break;
      }
    }
    return taken;
  }

  int constantReg(uint32_t bits) {
    const auto f = constantRegs.find(bits);
    if (f != constantRegs.end()) return f->second;
    const int r = keep();
    constantRegs[bits] = r;
    p->constants.emplace_back(r, bits);
    return r;
  }
  Val constant(const Type &t, const std::vector<uint32_t> &bits) {
    Val v;
    v.type = t;
    v.isConst = true;
    v.konst = bits;
    v.uniform = true;
    v.immutable = true;
    for (uint32_t b : bits) v.regs.push_back(constantReg(b));
    return v;
  }
  Val constFloat(float f) { return constant(kFloat, {fbits(f)}); }
  Val constInt(int32_t i) { return constant(kInt, {uint32_t(i)}); }

  // ---- emitting -------------------------------------------------------------------
  void emitTo(std::vector<Ins> &list, Op op, int d, int a = 0, int b = 0, int c = 0, int e = 0) {
    Ins in;
    in.op = op;
    in.d = d;
    in.a = a;
    in.b = b;
    in.c = c;
    in.e = e;
    list.push_back(in);
  }
  std::vector<Ins> &code() {
    if (current->nodes.empty() || current->nodes.back().t != Node::Code) {
      current->nodes.emplace_back();
      current->nodes.back().t = Node::Code;
    }
    return current->nodes.back().code;
  }
  // One instruction producing one number: folded when its inputs are
  // constants, worked out once per run when they are uniform.
  struct Arg {
    int reg;
    bool isConst;
    uint32_t bits;
    bool uniform;
  };
  static Arg arg(const Val &v, int slot) {
    return Arg{v.regs[size_t(slot)], v.isConst, v.isConst ? v.konst[size_t(slot)] : 0u, v.uniform};
  }
  Val one(Op op, K kind, const std::vector<Arg> &args) {
    bool allConst = true, allUniform = true;
    for (const Arg &a : args) {
      allConst = allConst && a.isConst;
      allUniform = allUniform && a.uniform;
    }
    if (allConst) {
      const uint32_t r = foldLane(op, args.size() > 0 ? args[0].bits : 0, args.size() > 1 ? args[1].bits : 0,
                                  args.size() > 2 ? args[2].bits : 0);
      return constant(scalarOf(kind), {r});
    }
    Val v;
    v.type = scalarOf(kind);
    v.uniform = allUniform;
    v.owns = !allUniform;
    v.immutable = allUniform;
    const int d = allUniform ? keep() : fresh();
    v.regs = {d};
    emitTo(allUniform ? p->prologue : code(), op, d, args.size() > 0 ? args[0].reg : 0, args.size() > 1 ? args[1].reg : 0,
           args.size() > 2 ? args[2].reg : 0);
    return v;
  }
  // Joins scalars into one value of type t.
  static Val join(const Type &t, const std::vector<Val> &parts) {
    Val v;
    v.type = t;
    v.isConst = true;
    v.uniform = true;
    v.owns = true;
    v.immutable = true;
    for (const Val &s : parts) {
      v.regs.push_back(s.regs[0]);
      v.isConst = v.isConst && s.isConst;
      if (s.isConst) v.konst.push_back(s.konst[0]);
      v.uniform = v.uniform && s.uniform;
      v.owns = v.owns && s.owns;
      v.immutable = v.immutable && s.immutable;
      v.global = v.global || s.global; // a global can change under a parameter that shares it
    }
    if (!v.isConst) v.konst.clear();
    return v;
  }
  static Val slot(const Val &v, int i) {
    Val s;
    s.type = v.type.scalarType();
    s.regs = {v.regs[size_t(i)]};
    s.isConst = v.isConst;
    if (v.isConst) s.konst = {v.konst[size_t(i)]};
    s.uniform = v.uniform;
    s.owns = false;
    s.immutable = v.immutable;
    s.global = v.global;
    return s;
  }
  // A value whose registers can be read after the statement that made it.
  Val materialize(const Val &v) { return v; }

  // ---- conversions --------------------------------------------------------------
  Val convertScalar(const Val &s, K to) {
    const K from = s.type.k;
    if (from == to) return s;
    Op op;
    if (to == K::Float) op = from == K::Int ? I2F : from == K::UInt ? U2F : I2F; // bool 0/1 -> float
    else if (to == K::Int) op = from == K::Float ? F2I : MOV;                   // uint/bool -> int: the bits
    else if (to == K::UInt) op = from == K::Float ? F2U : MOV;
    else op = from == K::Float ? F2B : I2B;
    if (op == MOV) {
      Val r = s;
      r.type.k = to;
      if (from == K::Bool) return r; // 0 and 1 are the same bits
      return r;
    }
    return one(op, to, {arg(s, 0)});
  }
  Val convert(const Val &v, K to) {
    if (v.type.k == to) return v;
    std::vector<Val> parts;
    for (int i = 0; i < v.type.cells(); ++i) parts.push_back(convertScalar(slot(v, i), to));
    Type t = v.type;
    t.k = to;
    Val r = join(t, parts);
    return r;
  }
  // Whether `from` may be used where `to` is wanted without saying so: GLSL's
  // int and uint to float, and (as the card's compilers allow) int to uint.
  static bool implicitly(const Type &from, const Type &to) {
    if (from == to) return true;
    if (from.rows != to.rows || from.cols != to.cols || from.array != to.array) return false;
    if (to.k == K::Float) return from.k == K::Int || from.k == K::UInt;
    if (to.k == K::UInt) return from.k == K::Int;
    return false;
  }
  Val coerce(const Val &v, const Type &to, int line, const std::string &what) {
    if (v.type == to) return v;
    if (!implicitly(v.type, to)) fail(line, "cannot use a " + v.type.name() + " as a " + to.name() + " " + what);
    return convert(v, to.k);
  }

  // ---- variables ----------------------------------------------------------------
  Symbol *lookup(const std::string &name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      const auto f = it->find(name);
      if (f != it->end()) return &f->second;
    }
    return nullptr;
  }
  Type typeOf(const TypeSpec &spec, int line) {
    Type t = typeFromName(spec.name, line);
    if (spec.arraySize > 0) t.array = arraySizeOf(spec.sizeExpr, line);
    else if (spec.arraySize < 0) t.array = -1;
    return t;
  }
  int arraySizeOf(const ExprP &e, int line) {
    if (!e) fail(line, "an array without a size");
    StatementTemps temps(*this);
    const Val v = expression(e.get());
    if (!v.isConst || !v.type.scalar() || !v.type.integer()) fail(line, "an array size must be a constant whole number");
    const int32_t n = int32_t(v.konst[0]);
    if (n <= 0 || n > 65536) fail(line, "an array size must be between 1 and 65536");
    return n;
  }

  void declareGlobals() {
    for (const auto &g : u.globals) {
      const Stmt &s = *g.decl;
      for (const auto &d : s.decls) {
        Type t = typeOf(s.type, d.line);
        if (d.arraySize > 0) t.array = arraySizeOf(d.sizeExpr, d.line);
        if (d.arraySize < 0) t.array = -1;
        if (g.storage == GlobalDecl::Uniform) {
          declareUniform(d.name, t, d.line);
          continue;
        }
        if (g.storage == GlobalDecl::In) fail(d.line, "an input other than gl_FragCoord: " + d.name);
        if (g.storage == GlobalDecl::Out) {
          if (d.name != "result" || t != vectorOf(K::Float, 4)) fail(d.line, "the only output is 'out vec4 result'");
          Symbol sym;
          sym.kind = Symbol::Output;
          sym.type = t;
          sym.regs = contiguous(4);
          sym.global = true;
          scopes.front()[d.name] = sym;
          // Clear to start with, every pixel: a main that writes nothing leaves nothing.
          const std::vector<int> regs = sym.regs;
          globalInits.push_back([this, regs] {
            for (int r : regs) emitTo(code(), MOV, r, constantReg(0));
          });
          continue;
        }
        if (t.k == K::Sampler) fail(d.line, "a sampler must be a uniform");
        declareGlobalVariable(d, t, g.storage == GlobalDecl::Const);
      }
    }
  }
  void declareUniform(const std::string &name, const Type &t, int line) {
    Symbol s;
    s.uniform = true;
    s.immutable = true;
    s.global = true;
    if (name == "values") {
      if (t.k != K::Float || t.rows != 1 || t.cols != 1 || t.array <= 0) fail(line, "values must be 'uniform float values[256]'");
      s.kind = Symbol::Values;
      s.type = t;
    } else if (name == "src" || name == "wavyOriginalTexture") {
      if (t.k != K::Sampler) fail(line, name + " must be a sampler2D");
      s.kind = Symbol::Sampler;
      s.type = t;
      s.sampler = name == "src" ? 0 : 1;
    } else {
      static const std::map<std::string, std::pair<Type, std::vector<int>>> host{
          {"sourceSize", {vectorOf(K::Float, 2), {H_SOURCE_W, H_SOURCE_H}}},
          {"processingDepth", {kInt, {H_DEPTH}}},
          {"wavySeconds", {kFloat, {H_SECONDS}}},
          {"wavyFrame", {kFloat, {H_FRAME}}},
          {"wavyScale", {kFloat, {H_SCALE}}},
          {"wavyBox", {vectorOf(K::Float, 4), {H_BOX0, H_BOX1, H_BOX2, H_BOX3}}},
          {"wavyPass", {kInt, {H_PASS}}},
          {"wavyPasses", {kInt, {H_PASSES}}}};
      const auto f = host.find(name);
      if (f == host.end()) fail(line, "the uniform " + name + " is not one Wavy-Flo provides");
      if (f->second.first != t) fail(line, "the uniform " + name + " must be a " + f->second.first.name());
      s.kind = Symbol::HostValue;
      s.type = t;
      s.host = f->second.second;
    }
    scopes.front()[name] = s;
  }
  void declareGlobalVariable(const Declarator &d, Type t, bool isConst) {
    Symbol s;
    s.kind = Symbol::Variable;
    s.global = true;
    s.declaration = d.id;
    s.isConst = isConst;
    if (d.init) {
      // Constant initializers are worked out here; the others for every pixel.
      StatementTemps temps(*this);
      Val v = expression(d.init.get());
      if (t.array < 0) t.array = v.type.array;
      v = coerce(v, t, d.line, "to start " + d.name);
      if (v.isConst && (isConst || !written.count(d.id))) {
        s.type = t;
        s.regs = v.regs;
        s.isConst = true;
        s.konst = v.konst;
        s.immutable = true;
        s.uniform = true;
        scopes.front()[d.name] = s;
        return;
      }
      if (v.uniform && !written.count(d.id)) {
        pin(v.regs);
        s.type = t;
        s.regs = v.regs;
        s.immutable = true;
        s.uniform = true;
        scopes.front()[d.name] = s;
        return;
      }
    } else if (t.array < 0) {
      fail(d.line, d.name + " has no size");
    }
    s.type = t;
    s.regs = contiguous(t.cells());
    scopes.front()[d.name] = s;
    const ExprP init = d.init;
    const std::vector<int> regs = s.regs;
    const Type type = t;
    const int line = d.line;
    const std::string name = d.name;
    globalInits.push_back([this, init, regs, type, line, name] {
      StatementTemps temps(*this);
      if (init) {
        Val v = coerce(expression(init.get()), type, line, "to start " + name);
        for (size_t k = 0; k < regs.size(); ++k) emitTo(code(), MOV, regs[k], v.regs[k]);
      } else {
        for (int r : regs) emitTo(code(), MOV, r, constantReg(0));
      }
    });
  }

  // ---- statements -----------------------------------------------------------------
  void statement(const Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case Stmt::Empty: return;
    case Stmt::Block: {
      scopes.emplace_back();
      for (size_t k = 0; k < s->children.size(); ++k) {
        // "if (c) return a; return b;" with nothing to change on either side
        // is one return of a choice: no branch for the lanes to split over.
        // wavyPixel() is written so, and is called in every blur's loop.
        if (k + 1 < s->children.size() && returnChoice(s->children[k].get(), s->children[k + 1].get())) break;
        statement(s->children[k].get());
      }
      releaseScope();
      return;
    }
    case Stmt::Decl: declaration(*s); return;
    case Stmt::ExprS: {
      StatementTemps temps(*this);
      expression(s->expr.get(), true);
      return;
    }
    case Stmt::If: ifStatement(*s); return;
    case Stmt::For:
    case Stmt::While:
    case Stmt::Do: loopStatement(*s); return;
    case Stmt::Break:
    case Stmt::Continue: {
      if (loops.empty()) fail(s->line, std::string(s->kind == Stmt::Break ? "break" : "continue") + " outside a loop");
      Node n;
      n.t = s->kind == Stmt::Break ? Node::Break : Node::Continue;
      n.loop = loops.back().number;
      current->nodes.push_back(std::move(n));
      return;
    }
    case Stmt::Return: {
      if (frames.empty()) fail(s->line, "return outside a function");
      // Copied: working out the value may call functions, and their frames
      // may move this one.
      const Type type = frames.back().type;
      const std::vector<int> result = frames.back().result;
      (void)result;
      if (s->expr) {
        if (type == kVoid) fail(s->line, "a void function returning a value");
        StatementTemps temps(*this);
        returnValue(coerce(expression(s->expr.get()), type, s->line, "as the value returned"));
      } else if (type != kVoid) {
        fail(s->line, "return without a value in a function returning " + type.name());
      }
      // A function's last statement needs no node to leave it by.
      if (!frames.back().tail) {
        Node n;
        n.t = Node::Return;
        current->nodes.push_back(std::move(n));
      }
      return;
    }
    case Stmt::Discard: {
      Node n;
      n.t = Node::Discard;
      current->nodes.push_back(std::move(n));
      return;
    }
    }
  }
  // `first` an if without else returning a value, `second` a return: the
  // pair returnChoice() compiles as one return. `then` is first's return.
  bool returnPair(const Stmt *first, const Stmt *second, const Stmt **then) {
    if (first->kind != Stmt::If || first->elseS || second->kind != Stmt::Return || !second->expr) return false;
    const Stmt *t = first->thenS.get();
    while (t && t->kind == Stmt::Block && t->children.size() == 1) t = t->children[0].get();
    if (!t || t->kind != Stmt::Return || !t->expr) return false;
    if (!pure(first->expr.get()) || !pure(t->expr.get()) || !pure(second->expr.get())) return false;
    // Both sides are worked out for every lane: only when both are cheap. A
    // branch lets the lanes that leave early skip a costly other side
    // (Mirror's sampling on the half it keeps as it is).
    if (cost(t->expr.get()) > 16 || cost(second->expr.get()) > 16) return false;
    *then = t;
    return true;
  }
  // Roughly how much working out an expression takes: a node a step, a call
  // of the shader's own functions too much to do for lanes that do not need it.
  int cost(const Expr *e) {
    if (!e) return 0;
    if (e->kind == Expr::Call && functions.count(e->op)) return 1000;
    int total = 1;
    for (const auto &a : e->args) total += cost(a.get());
    return total;
  }
  // "if (c) return a; return b;" compiled as one return of c ? a : b, when
  // working out all three changes nothing. False, compiling nothing, otherwise.
  bool returnChoice(const Stmt *first, const Stmt *second) {
    const Stmt *then = nullptr;
    if (frames.empty() || !returnPair(first, second, &then)) return false;
    const Type type = frames.back().type;
    if (type == kVoid || type.array) return false;
    StatementTemps temps(*this);
    const Val c = condition(first->expr.get());
    if (c.isConst) {
      const Stmt *taken = c.konst[0] ? then : second;
      returnValue(coerce(expression(taken->expr.get()), type, taken->line, "as the value returned"));
    } else {
      const Val a = coerce(expression(then->expr.get()), type, then->line, "as the value returned");
      const Val b = coerce(expression(second->expr.get()), type, second->line, "as the value returned");
      std::vector<Val> parts;
      for (int k = 0; k < type.cells(); ++k) parts.push_back(one(SEL, type.k, {arg(c, 0), arg(a, k), arg(b, k)}));
      returnValue(join(type, parts));
    }
    if (!frames.back().tail) {
      Node n;
      n.t = Node::Return;
      current->nodes.push_back(std::move(n));
    }
    return true;
  }
  // Whether a function's only way out is its last statement (or the pair
  // returnChoice() makes one return of): every lane that enters it reaches
  // that return, so its value can be handed over as it is.
  static bool containsReturn(const Stmt *s) {
    if (!s) return false;
    if (s->kind == Stmt::Return) return true;
    for (const auto &c : s->children)
      if (containsReturn(c.get())) return true;
    return containsReturn(s->thenS.get()) || containsReturn(s->elseS.get()) || containsReturn(s->body.get()) ||
           containsReturn(s->init.get());
  }
  bool tailOnly(const Function &f) {
    const Stmt *body = f.body.get();
    if (!body || body->kind != Stmt::Block || body->children.empty()) return false;
    const auto &c = body->children;
    const Stmt *last = c.back().get();
    if (last->kind != Stmt::Return || !last->expr) return false;
    size_t before = c.size() - 1;
    const Stmt *then = nullptr;
    if (before >= 1 && returnPair(c[before - 1].get(), last, &then)) --before;
    for (size_t k = 0; k < before; ++k)
      if (containsReturn(c[k].get())) return false;
    return true;
  }
  static bool distinct(const std::vector<int> &regs) {
    std::set<int> seen(regs.begin(), regs.end());
    return seen.size() == regs.size();
  }
  // The value a function returns: handed over as it is from a tail return
  // when nothing will write or free its registers before the caller reads
  // them, otherwise copied into the function's result registers.
  void returnValue(const Val &v) {
    FunctionFrame &f = frames.back();
    if (f.tail && (v.isConst || v.uniform || (v.owns && distinct(v.regs)))) {
      adopt(v); // the caller's from here: not freed with the return statement
      f.forwarded = true;
      f.value = v;
      return;
    }
    if (f.result.empty()) {
      if (f.type.array) f.result = contiguous(f.type.cells());
      else
        for (int k = 0; k < f.type.cells(); ++k) f.result.push_back(keep());
    }
    for (size_t k = 0; k < f.result.size(); ++k) emitTo(code(), STORE, f.result[k], v.regs[k]);
  }

  void releaseScope() {
    for (auto &[name, sym] : scopes.back()) {
      for (int r : sym.owned) freeRegs.push_back(r);
      if (sym.kind != Symbol::Variable || sym.global || sym.immutable) continue;
      if (sym.type.array) continue; // arrays are contiguous blocks, kept
      for (int r : sym.regs)
        if (!pinned.count(r)) freeRegs.push_back(r);
    }
    scopes.pop_back();
  }

  void declaration(const Stmt &s) {
    for (const auto &d : s.decls) {
      Type t = typeOf(s.type, d.line);
      if (d.arraySize > 0) {
        if (t.array) fail(d.line, "an array of arrays");
        t.array = arraySizeOf(d.sizeExpr, d.line);
      } else if (d.arraySize < 0) {
        t.array = -1;
      }
      if (t.k == K::Sampler) fail(d.line, "a sampler can only be one of the host's uniforms");
      if (t.k == K::Void) fail(d.line, "a variable cannot be void");
      if (scopes.back().count(d.name)) fail(d.line, d.name + " is declared twice");
      Symbol sym;
      sym.kind = Symbol::Variable;
      sym.declaration = d.id;
      sym.depth = depth;
      sym.isConst = s.isConst;
      const bool neverWritten = !written.count(d.id);
      if (d.init) {
        StatementTemps temps(*this);
        Val v = expression(d.init.get());
        if (t.array < 0) t.array = v.type.array;
        v = coerce(v, t, d.line, "to start " + d.name);
        if (s.isConst && !neverWritten) fail(d.line, "the constant " + d.name + " is written");
        if (neverWritten && (v.isConst || v.uniform || v.owns || v.immutable)) {
          // Only ever this value: the variable is its registers.
          sym.type = t;
          sym.regs = v.regs;
          sym.isConst = v.isConst;
          sym.konst = v.konst;
          sym.uniform = v.uniform;
          sym.immutable = true;
          sym.owned = adopt(v);
          scopes.back()[d.name] = sym;
          continue;
        }
        sym.type = t;
        if (!t.array && v.owns && distinct(v.regs)) {
          // A fresh value, written again later: its registers become the variable.
          adopt(v);
          sym.regs = v.regs;
          scopes.back()[d.name] = sym;
          continue;
        }
        sym.regs = t.array ? contiguous(t.cells()) : std::vector<int>();
        if (!t.array) {
          for (int k = 0; k < t.cells(); ++k) sym.regs.push_back(freshVariable());
        }
        // A new variable: whatever lanes are not running cannot read it, so it
        // is filled whole, not under the mask.
        for (int k = 0; k < t.cells(); ++k) emitTo(code(), MOV, sym.regs[size_t(k)], v.regs[size_t(k)]);
        scopes.back()[d.name] = sym;
        continue;
      }
      if (t.array < 0) fail(d.line, d.name + " has no size");
      sym.type = t;
      if (t.array) sym.regs = contiguous(t.cells());
      else
        for (int k = 0; k < t.cells(); ++k) sym.regs.push_back(freshVariable());
      // Declared without a value: zero, so a lane never reads what another left.
      for (int r : sym.regs) emitTo(code(), MOV, r, constantReg(0));
      scopes.back()[d.name] = sym;
    }
  }
  int freshVariable() {
    int r;
    if (!freeRegs.empty()) {
      r = freeRegs.back();
      freeRegs.pop_back();
    } else {
      r = next++;
    }
    return r; // not a temporary: lives to the end of its scope
  }

  Val condition(const Expr *e) {
    Val c = expression(e);
    if (c.type != kBool) fail(e->line, "a condition must be a bool, not a " + c.type.name());
    return c;
  }

  void ifStatement(const Stmt &s) {
    // The condition's registers are the statement's until both branches are
    // done: the node reads them when it runs, after the branches are written.
    StatementTemps temps(*this);
    Val c = condition(s.expr.get());
    if (c.isConst) {
      // Known now: only that branch exists.
      if (c.konst[0]) branch(s.thenS.get());
      else if (s.elseS) branch(s.elseS.get());
      return;
    }
    Node n;
    n.t = Node::If;
    n.cond = c.regs[0];
    n.a = std::make_unique<Block>();
    n.b = std::make_unique<Block>();
    Block *a = n.a.get(), *b = n.b.get();
    current->nodes.push_back(std::move(n));
    Block *outer = current;
    Deeper deeper(depth);
    current = a;
    branch(s.thenS.get());
    current = b;
    if (s.elseS) branch(s.elseS.get());
    current = outer;
  }
  void branch(const Stmt *s) {
    scopes.emplace_back();
    statement(s);
    releaseScope();
  }

  void loopStatement(const Stmt &s) {
    scopes.emplace_back();
    if (s.kind == Stmt::For && s.init) statement(s.init.get());
    Node n;
    n.t = Node::Loop;
    n.loop = p->loops++;
    n.condFirst = s.kind != Stmt::Do;
    n.a = std::make_unique<Block>();
    n.b = std::make_unique<Block>();
    n.c = std::make_unique<Block>();
    Block *condBlock = n.a.get(), *body = n.b.get(), *step = n.c.get();
    const int number = n.loop;
    current->nodes.push_back(std::move(n));
    const size_t placed = current->nodes.size() - 1;
    Block *outer = current;
    const ExprP condExpr = s.kind == Stmt::For ? s.cond : s.expr;
    // The loop's blocks run turn after turn, some lanes stopping early: deeper.
    Deeper deeper(depth);
    current = condBlock;
    if (condExpr) {
      // Read by the loop as soon as its block has run, before anything else
      // can reuse the register: no copy of it is needed.
      StatementTemps temps(*this);
      outer->nodes[placed].condReg = condition(condExpr.get()).regs[0];
    }
    loops.push_back({number});
    current = body;
    branch(s.body.get());
    loops.pop_back();
    current = step;
    if (s.kind == Stmt::For && s.step) {
      StatementTemps temps(*this);
      expression(s.step.get(), true);
    }
    current = outer;
    releaseScope();
  }

  // ---- calls ----------------------------------------------------------------------
  const Function *resolve(const std::string &name, const std::vector<Val> &args, int line) {
    const auto f = functions.find(name);
    if (f == functions.end()) return nullptr;
    const Function *exact = nullptr, *loose = nullptr;
    int looseCount = 0;
    for (const Function *candidate : f->second) {
      if (candidate->params.size() != args.size()) continue;
      bool same = true, possible = true;
      for (size_t i = 0; i < args.size(); ++i) {
        const Type want = typeOf(candidate->params[i].type, line);
        if (want != args[i].type) same = false;
        const bool out = candidate->params[i].qualifier != Param::In;
        if (!(want == args[i].type || (!out && implicitly(args[i].type, want)))) possible = false;
      }
      if (same) {
        if (!candidate->body && exact) continue;
        if (!exact || candidate->body) exact = candidate;
      } else if (possible) {
        if (!loose || loose->name != candidate->name || candidate->body) {
          if (!loose) ++looseCount;
          loose = candidate;
        }
      }
    }
    if (exact) {
      if (!exact->body) {
        // A prototype: the definition with the same parameter types.
        for (const Function *candidate : f->second)
          if (candidate->body && candidate->params.size() == exact->params.size()) {
            bool same = true;
            for (size_t i = 0; i < args.size(); ++i)
              same = same && typeOf(candidate->params[i].type, line) == typeOf(exact->params[i].type, line);
            if (same) return candidate;
          }
        fail(line, name + " is declared but never defined");
      }
      return exact;
    }
    if (loose) return loose;
    std::string given;
    for (const Val &a : args) given += (given.empty() ? "" : ", ") + a.type.name();
    fail(line, "no " + name + " takes (" + given + ")");
  }

  // An argument as the call site worked it out: a value, or for a parameter
  // that is written (out, inout) the place it goes back to.
  struct Argument {
    Val value;
    Place place;
    bool isPlace = false;
  };
  Val call(const Function &f, std::vector<Argument> args, int line) {
    for (const Function *active : callStack)
      if (active == &f) fail(line, f.name + " calls itself: GLSL has no recursion");
    if (callStack.size() > 64) fail(line, "calls nested too deep");
    if (args.size() != f.params.size()) fail(line, f.name + " takes " + std::to_string(f.params.size()) + " argument(s)");
    const Type resultType = typeOf(f.result, f.line);
    std::vector<Val> values(f.params.size());
    for (size_t i = 0; i < f.params.size(); ++i) {
      const Param &prm = f.params[i];
      const Type want = typeOf(prm.type, line);
      Argument &a = args[i];
      const std::string which = "argument " + std::to_string(i + 1) + " of " + f.name;
      if (prm.qualifier == Param::In) {
        if (a.isPlace) a.value = read(a.place); // another overload writes it; this one only reads
        values[i] = coerce(a.value, want, line, "as " + which);
      } else {
        if (!a.isPlace) fail(line, which + " must be a variable: it is written");
        if (a.place.type != want) fail(line, which + " must be a " + want.name());
        if (prm.qualifier == Param::InOut) values[i] = read(a.place);
      }
    }
    bool anyOut = false;
    for (const auto &prm : f.params) anyOut = anyOut || prm.qualifier != Param::In;
    scopes.emplace_back();
    for (size_t i = 0; i < f.params.size(); ++i) {
      const Param &prm = f.params[i];
      if (prm.name.empty()) continue;
      const Type want = typeOf(prm.type, line);
      Symbol s;
      s.kind = Symbol::Variable;
      s.type = want;
      s.declaration = prm.id;
      s.depth = depth;
      const bool neverWritten = !written.count(prm.id);
      const Val &v = values[i];
      // A parameter the body never writes can be the argument's own registers:
      // nothing can change them while the body runs, short of a global (the
      // body may write it) or another parameter going out to the same place.
      if (prm.qualifier == Param::In && neverWritten &&
          (v.isConst || v.uniform || v.owns || v.immutable || (!v.global && !anyOut))) {
        s.regs = v.regs;
        s.isConst = v.isConst;
        s.konst = v.konst;
        s.uniform = v.uniform;
        s.immutable = true;
        s.owned = adopt(v);
      } else if (prm.qualifier == Param::In && !want.array && v.owns && distinct(v.regs)) {
        // A fresh argument the body writes: its registers become the parameter.
        adopt(v);
        s.regs = v.regs;
      } else {
        s.regs = want.array ? contiguous(want.cells()) : std::vector<int>();
        if (!want.array)
          for (int k = 0; k < want.cells(); ++k) s.regs.push_back(freshVariable());
        if (prm.qualifier != Param::Out)
          for (int k = 0; k < want.cells(); ++k) emitTo(code(), MOV, s.regs[size_t(k)], v.regs[size_t(k)]);
        else
          for (int r : s.regs) emitTo(code(), MOV, r, constantReg(0));
      }
      scopes.back()[prm.name] = s;
    }
    FunctionFrame frame;
    frame.f = &f;
    frame.type = resultType;
    frame.tail = resultType != kVoid && tailOnly(f);
    if (resultType != kVoid && !frame.tail) {
      // Returned from more than one place: one set of registers they all
      // store to, clear to start with for a lane that falls off the end.
      if (resultType.array) frame.result = contiguous(resultType.cells());
      else
        for (int k = 0; k < resultType.cells(); ++k) frame.result.push_back(keep());
      for (int r : frame.result) emitTo(code(), MOV, r, constantReg(0));
    }
    // The body in a node of its own: lanes that return leave it, and all
    // come back together after it.
    Node n;
    n.t = Node::Call;
    n.a = std::make_unique<Block>();
    Block *body = n.a.get();
    current->nodes.push_back(std::move(n));
    Block *outer = current;
    current = body;
    frames.push_back(frame);
    callStack.push_back(&f);
    const auto savedLoops = loops;
    loops.clear();
    {
      // A new set of temporaries for the body: its statements make and free
      // their own. Put back however the body ends, a compile error included.
      struct Aside {
        std::vector<std::vector<int>> &live, saved;
        explicit Aside(std::vector<std::vector<int>> &temps) : live(temps) { saved.swap(live); }
        ~Aside() { saved.swap(live); }
      } aside(statementTemps);
      statement(f.body.get());
    }
    loops = savedLoops;
    callStack.pop_back();
    const FunctionFrame done = frames.back();
    frames.pop_back();
    current = outer;
    // Out and inout arguments go back to where they came from.
    for (size_t i = 0; i < f.params.size(); ++i) {
      if (f.params[i].qualifier == Param::In || f.params[i].name.empty()) continue;
      const Symbol &s = scopes.back()[f.params[i].name];
      Val v;
      v.type = s.type;
      v.regs = s.regs;
      write(args[i].place, v, false);
    }
    releaseScope();
    if (done.forwarded) {
      // The tail return's own value. What it owns is freed with the
      // statement that called it, as a copy would have been.
      const Val r = done.value;
      if (r.owns && !statementTemps.empty())
        for (int reg : std::set<int>(r.regs.begin(), r.regs.end())) statementTemps.back().push_back(reg);
      return r;
    }
    Val r;
    r.type = resultType;
    r.regs = done.result;
    r.owns = true;
    if (!r.regs.empty() && !statementTemps.empty()) {
      // Free with the statement that called it.
      for (int reg : r.regs) statementTemps.back().push_back(reg);
    }
    return r;
  }

  // ---- places ---------------------------------------------------------------------
  Place place(const Expr *e) {
    switch (e->kind) {
    case Expr::Name: {
      Symbol *s = lookup(e->op);
      if (!s) fail(e->line, "unknown name " + e->op);
      if (s->kind == Symbol::Output) {
        Place pl;
        pl.type = s->type;
        pl.regs = s->regs;
        pl.global = true;
        return pl;
      }
      if (s->kind != Symbol::Variable) fail(e->line, e->op + " cannot be assigned");
      if (s->isConst || s->immutable) {
        if (s->isConst) fail(e->line, "the constant " + e->op + " cannot be assigned");
        fail(e->line, e->op + " cannot be assigned here");
      }
      Place pl;
      pl.type = s->type;
      pl.regs = s->regs;
      pl.global = s->global;
      pl.declaration = s->declaration;
      pl.symbol = s;
      for (int k = 0; k < s->type.cells(); ++k) pl.positions.push_back(k);
      return pl;
    }
    case Expr::Index: {
      Place base = place(e->args[0].get());
      if (base.dynamic) fail(e->line, "an element of an element picked at run time");
      Val index = expression(e->args[1].get());
      if (!index.type.scalar() || !index.type.integer()) fail(e->line, "an index must be an int or a uint");
      const Type t = base.type;
      Type elementType;
      int count, span;
      if (t.array) {
        elementType = t.elementType();
        count = t.array;
        span = t.element();
      } else if (t.matrix()) {
        elementType = t.column();
        count = t.cols;
        span = t.rows;
      } else if (t.vector()) {
        elementType = t.scalarType();
        count = t.rows;
        span = 1;
      } else {
        fail(e->line, "only arrays, vectors and matrices can be indexed");
      }
      if (index.isConst) {
        const int32_t i = int32_t(index.konst[0]);
        if (i < 0 || i >= count) fail(e->line, "index " + std::to_string(i) + " is outside 0.." + std::to_string(count - 1));
        Place pl;
        pl.type = elementType;
        pl.global = base.global;
        pl.declaration = base.declaration;
        pl.symbol = base.symbol;
        for (int k = 0; k < span; ++k) {
          pl.regs.push_back(base.regs[size_t(i * span + k)]);
          if (base.symbol) pl.positions.push_back(base.positions[size_t(i * span + k)]);
        }
        return pl;
      }
      Place pl;
      pl.type = elementType;
      pl.dynamic = true;
      pl.arrayRegs = base.regs;
      pl.elementSlots = span;
      pl.index = convert(index, K::Int).regs[0];
      for (int k = 0; k < span; ++k) pl.components.push_back(k);
      pl.global = base.global;
      pl.declaration = base.declaration;
      return pl;
    }
    case Expr::Field: {
      Place base = place(e->args[0].get());
      const std::vector<int> picked = swizzle(e->op, base.type, e->line);
      std::set<int> unique(picked.begin(), picked.end());
      if (unique.size() != picked.size()) fail(e->line, "a swizzle written to may not repeat a component: " + e->op);
      Place pl = base;
      pl.type = picked.size() == 1 ? base.type.scalarType() : vectorOf(base.type.k, int(picked.size()));
      if (base.dynamic) {
        std::vector<int> components;
        for (int k : picked) components.push_back(base.components[size_t(k)]);
        pl.components = components;
      } else {
        pl.regs.clear();
        pl.positions.clear();
        for (int k : picked) {
          pl.regs.push_back(base.regs[size_t(k)]);
          if (base.symbol) pl.positions.push_back(base.positions[size_t(k)]);
        }
      }
      return pl;
    }
    default:
      fail(e->line, "this cannot be assigned to");
    }
  }
  Val read(const Place &pl) {
    if (!pl.dynamic) {
      Val v;
      v.type = pl.type;
      v.regs = pl.regs;
      v.global = pl.global;
      return v;
    }
    std::vector<Val> parts;
    for (int comp : pl.components) {
      const int list = int(p->lists.size());
      const int count = int(pl.arrayRegs.size()) / pl.elementSlots;
      for (int i = 0; i < count; ++i) p->lists.push_back(pl.arrayRegs[size_t(i * pl.elementSlots + comp)]);
      Val v;
      v.type = pl.type.scalarType();
      v.owns = true;
      const int d = fresh();
      v.regs = {d};
      emitTo(code(), GATHER, d, pl.index, 0, count, list);
      parts.push_back(v);
    }
    Val r = join(pl.type, parts);
    r.isConst = false;
    r.uniform = false;
    r.immutable = false;
    return r;
  }
  // `whole`: a new variable's first value, not under the mask.
  void write(Place &pl, const Val &v, bool whole) {
    Symbol *s = pl.symbol;
    if (!pl.dynamic && !whole && s && s->kind == Symbol::Variable && !s->immutable && !s->type.array && s->depth == depth &&
        v.owns && distinct(v.regs) && pl.positions.size() == v.regs.size()) {
      // At its own depth: the variable takes the value's fresh registers, and
      // the ones it had are free -- what read them has been written already.
      adopt(v);
      for (size_t k = 0; k < pl.positions.size(); ++k) {
        int &slot = s->regs[size_t(pl.positions[k])];
        if (!pinned.count(slot)) freeRegs.push_back(slot);
        slot = v.regs[k];
        pl.regs[k] = v.regs[k]; // read back from where it now is
      }
      return;
    }
    if (!pl.dynamic) {
      for (size_t k = 0; k < pl.regs.size(); ++k) emitTo(code(), whole ? MOV : STORE, pl.regs[k], v.regs[k]);
      return;
    }
    const int count = int(pl.arrayRegs.size()) / pl.elementSlots;
    for (size_t k = 0; k < pl.components.size(); ++k) {
      const int list = int(p->lists.size());
      for (int i = 0; i < count; ++i) p->lists.push_back(pl.arrayRegs[size_t(i * pl.elementSlots + pl.components[k])]);
      emitTo(code(), SCATTER, 0, pl.index, v.regs[k], count, list);
    }
  }

  // ---- swizzles -------------------------------------------------------------------
  static std::vector<int> swizzle(const std::string &field, const Type &t, int line) {
    if (t.array || t.matrix()) fail(line, "." + field + " on a " + t.name());
    if (field.empty() || field.size() > 4) fail(line, "a bad swizzle ." + field);
    static const char *const sets[] = {"xyzw", "rgba", "stpq"};
    for (const char *set : sets) {
      std::vector<int> picked;
      bool ok = true;
      for (char c : field) {
        const char *at = std::strchr(set, c);
        if (!at) {
          ok = false;
          break;
        }
        const int k = int(at - set);
        if (k >= t.rows) fail(line, "." + field + " reaches past a " + t.name());
        picked.push_back(k);
      }
      if (ok) return picked;
    }
    fail(line, "a bad swizzle ." + field);
  }

  // ---- expressions --------------------------------------------------------------------
  Val expression(const Expr *e, bool discarded = false) {
    switch (e->kind) {
    case Expr::Int: return constant(kInt, {uint32_t(e->i)});
    case Expr::UInt: return constant(kUInt, {uint32_t(e->i)});
    case Expr::Float: return constFloat(float(e->f));
    case Expr::Bool: return constant(kBool, {uint32_t(e->i ? 1 : 0)});
    case Expr::Name: return name(e);
    case Expr::Binary: return binary(e);
    case Expr::Unary: return unary(e);
    case Expr::Assign: return assign(e, discarded);
    case Expr::Ternary: return ternary(e);
    case Expr::Call: return callExpr(e);
    case Expr::Construct: return construct(e);
    case Expr::Index: return indexed(e);
    case Expr::Field: return field(e);
    case Expr::PostInc:
    case Expr::PostDec: return increment(e, e->kind == Expr::PostInc ? 1 : -1, true);
    case Expr::Comma: {
      expression(e->args[0].get(), true);
      return expression(e->args[1].get());
    }
    case Expr::Length: {
      const Val v = expression(e->args[0].get());
      if (v.type.array) return constInt(v.type.array);
      if (v.type.matrix()) return constInt(v.type.cols);
      if (v.type.vector()) return constInt(v.type.rows);
      fail(e->line, ".length() of something that is not an array");
    }
    }
    fail(e->line, "an expression not understood");
  }

  Val name(const Expr *e) {
    Symbol *s = lookup(e->op);
    if (!s) fail(e->line, "unknown name " + e->op);
    switch (s->kind) {
    case Symbol::Variable:
    case Symbol::Input:
    case Symbol::Output: {
      Val v;
      v.type = s->type;
      v.regs = s->regs;
      v.isConst = s->isConst && !s->konst.empty();
      v.konst = s->konst;
      v.uniform = s->uniform;
      v.immutable = s->immutable;
      v.global = s->global;
      return v;
    }
    case Symbol::HostValue: {
      std::vector<Val> parts;
      for (int h : s->host) parts.push_back(hostValue(h));
      return join(s->type, parts);
    }
    case Symbol::Values: fail(e->line, "values must be indexed: values[0]");
    case Symbol::Sampler: fail(e->line, e->op + " can only be passed to texelFetch, texture or textureSize");
    }
    fail(e->line, "unknown name " + e->op);
  }
  Val hostValue(int which) {
    const auto key = std::make_pair(which, std::vector<uint32_t>{});
    const auto f = hostCache.find(key);
    Val v;
    v.type = which == H_DEPTH || which == H_PASS || which == H_PASSES ? kInt : kFloat;
    v.uniform = true;
    v.immutable = true;
    if (f != hostCache.end()) {
      v.regs = f->second;
      return v;
    }
    const int r = keep();
    emitTo(p->prologue, HOST, r, which);
    hostCache[key] = {r};
    v.regs = {r};
    return v;
  }

  Val indexed(const Expr *e) {
    // values[i]: the host's sliders.
    if (e->args[0]->kind == Expr::Name) {
      const Symbol *found = lookup(e->args[0]->op);
      if (found && found->kind == Symbol::Values) {
        // Copied: the index may call a function, whose scope may move this one.
        const Symbol values = *found;
        const Symbol *s = &values;
        Val i = expression(e->args[1].get());
        if (!i.type.scalar() || !i.type.integer()) fail(e->line, "an index must be an int or a uint");
        Val v;
        v.type = kFloat;
        if (i.isConst) {
          const int32_t k = int32_t(i.konst[0]);
          if (k < 0 || k >= s->type.array) fail(e->line, "values[" + std::to_string(k) + "] is outside the 256 values");
          const auto f = valueCache.find(k);
          int r;
          if (f != valueCache.end()) r = f->second;
          else {
            r = keep();
            emitTo(p->prologue, UVALUE, r, k);
            valueCache[k] = r;
          }
          v.regs = {r};
          v.uniform = true;
          v.immutable = true;
          return v;
        }
        i = convert(i, K::Int);
        v.uniform = i.uniform;
        v.immutable = i.uniform;
        v.owns = !i.uniform;
        const int d = i.uniform ? keep() : fresh();
        emitTo(i.uniform ? p->prologue : code(), UGATHER, d, i.regs[0], s->type.array);
        v.regs = {d};
        return v;
      }
    }
    Val base = expression(e->args[0].get());
    Val i = expression(e->args[1].get());
    if (!i.type.scalar() || !i.type.integer()) fail(e->line, "an index must be an int or a uint");
    const Type t = base.type;
    Type elementType;
    int count, span;
    if (t.array) {
      elementType = t.elementType();
      count = t.array;
      span = t.element();
    } else if (t.matrix()) {
      elementType = t.column();
      count = t.cols;
      span = t.rows;
    } else if (t.vector()) {
      elementType = t.scalarType();
      count = t.rows;
      span = 1;
    } else {
      fail(e->line, "only arrays, vectors and matrices can be indexed");
    }
    if (i.isConst) {
      const int32_t k = int32_t(i.konst[0]);
      if (k < 0 || k >= count) fail(e->line, "index " + std::to_string(k) + " is outside 0.." + std::to_string(count - 1));
      std::vector<Val> parts;
      for (int s = 0; s < span; ++s) parts.push_back(slot(base, k * span + s));
      Val r = join(elementType, parts);
      r.owns = false;
      r.global = base.global;
      return r;
    }
    i = convert(i, K::Int);
    std::vector<Val> parts;
    for (int s = 0; s < span; ++s) {
      const int list = int(p->lists.size());
      for (int k = 0; k < count; ++k) p->lists.push_back(base.regs[size_t(k * span + s)]);
      Val v;
      v.type = elementType.scalarType();
      const bool uniform = base.uniform && i.uniform;
      v.uniform = uniform;
      v.immutable = uniform;
      v.owns = !uniform;
      const int d = uniform ? keep() : fresh();
      emitTo(uniform ? p->prologue : code(), GATHER, d, i.regs[0], 0, count, list);
      v.regs = {d};
      parts.push_back(v);
    }
    return join(elementType, parts);
  }

  Val field(const Expr *e) {
    Val base = expression(e->args[0].get());
    if (base.type.scalar()) {
      // A scalar's .x (or .xx): the card's compilers take it.
      std::vector<Val> parts;
      for (char c : e->op) {
        if (c != 'x' && c != 'r' && c != 's') fail(e->line, "a bad swizzle ." + e->op);
        parts.push_back(base);
      }
      if (parts.size() == 1) return base;
      return join(vectorOf(base.type.k, int(parts.size())), parts);
    }
    const std::vector<int> picked = swizzle(e->op, base.type, e->line);
    std::vector<Val> parts;
    for (int k : picked) parts.push_back(slot(base, k));
    if (parts.size() == 1) {
      Val r = parts[0];
      r.global = base.global;
      return r;
    }
    Val r = join(vectorOf(base.type.k, int(parts.size())), parts);
    r.owns = false;
    r.global = base.global;
    return r;
  }

  // ---- operators ------------------------------------------------------------------
  // The kind two operands meet at: float if either is, uint over int.
  static K meet(K a, K b) {
    if (a == K::Float || b == K::Float) return K::Float;
    if (a == K::UInt || b == K::UInt) return K::UInt;
    return a;
  }
  Val arithmetic(const std::string &op, Val a, Val b, int line) {
    if (!a.type.numeric() || !b.type.numeric()) fail(line, "'" + op + "' needs numbers, not " + a.type.name() + " and " + b.type.name());
    if (a.type.array || b.type.array) fail(line, "'" + op + "' on an array");
    const K k = meet(a.type.k, b.type.k);
    if (op == "%" && k == K::Float) fail(line, "'%' takes whole numbers: use mod() for floats");
    a = convert(a, k);
    b = convert(b, k);
    // Linear algebra.
    if (op == "*" && (a.type.matrix() || b.type.matrix()) && !(a.type.scalar() || b.type.scalar())) return multiplyMatrix(a, b, line);
    Type result;
    if (a.type.scalar()) result = b.type;
    else if (b.type.scalar()) result = a.type;
    else if (a.type == b.type) result = a.type;
    else fail(line, "'" + op + "' between a " + a.type.name() + " and a " + b.type.name());
    Op o;
    if (op == "+") o = k == K::Float ? FADD : IADD;
    else if (op == "-") o = k == K::Float ? FSUB : ISUB;
    else if (op == "*") o = k == K::Float ? FMUL : IMUL;
    else if (op == "/") o = k == K::Float ? FDIV : k == K::UInt ? UDIV : IDIV;
    else o = k == K::UInt ? UMOD : IMOD;
    std::vector<Val> parts;
    for (int i = 0; i < result.cells(); ++i) {
      const Val x = slot(a, a.type.scalar() ? 0 : i), y = slot(b, b.type.scalar() ? 0 : i);
      parts.push_back(one(o, k, {arg(x, 0), arg(y, 0)}));
    }
    return join(result, parts);
  }
  Val multiplyMatrix(const Val &a, const Val &b, int line) {
    // m * v: a vector; v * m: a vector; m * m: a matrix. Column-major.
    auto at = [](const Val &m, int col, int row) { return slot(m, col * m.type.rows + row); };
    auto dotOf = [&](std::vector<Val> xs, std::vector<Val> ys) {
      Val sum = one(FMUL, K::Float, {arg(xs[0], 0), arg(ys[0], 0)});
      for (size_t i = 1; i < xs.size(); ++i) {
        const Val prod = one(FMUL, K::Float, {arg(xs[i], 0), arg(ys[i], 0)});
        sum = one(FADD, K::Float, {arg(sum, 0), arg(prod, 0)});
      }
      return sum;
    };
    if (a.type.matrix() && b.type.vector()) {
      if (a.type.cols != b.type.rows) fail(line, "a " + a.type.name() + " times a " + b.type.name());
      std::vector<Val> parts;
      for (int r = 0; r < a.type.rows; ++r) {
        std::vector<Val> xs, ys;
        for (int c = 0; c < a.type.cols; ++c) {
          xs.push_back(at(a, c, r));
          ys.push_back(slot(b, c));
        }
        parts.push_back(dotOf(xs, ys));
      }
      return join(vectorOf(K::Float, a.type.rows), parts);
    }
    if (a.type.vector() && b.type.matrix()) {
      if (a.type.rows != b.type.rows) fail(line, "a " + a.type.name() + " times a " + b.type.name());
      std::vector<Val> parts;
      for (int c = 0; c < b.type.cols; ++c) {
        std::vector<Val> xs, ys;
        for (int r = 0; r < b.type.rows; ++r) {
          xs.push_back(slot(a, r));
          ys.push_back(at(b, c, r));
        }
        parts.push_back(dotOf(xs, ys));
      }
      return join(vectorOf(K::Float, b.type.cols), parts);
    }
    if (a.type.matrix() && b.type.matrix()) {
      if (a.type.cols != b.type.rows) fail(line, "a " + a.type.name() + " times a " + b.type.name());
      std::vector<Val> parts;
      for (int c = 0; c < b.type.cols; ++c)
        for (int r = 0; r < a.type.rows; ++r) {
          std::vector<Val> xs, ys;
          for (int k = 0; k < a.type.cols; ++k) {
            xs.push_back(at(a, k, r));
            ys.push_back(at(b, c, k));
          }
          parts.push_back(dotOf(xs, ys));
        }
      return join(Type{K::Float, a.type.rows, b.type.cols, 0}, parts);
    }
    fail(line, "a " + a.type.name() + " times a " + b.type.name());
  }
  Val compare(const std::string &op, Val a, Val b, int line) {
    if (op == "==" || op == "!=") {
      if (a.type != b.type) {
        if (!(a.type.numeric() && b.type.numeric() && a.type.rows == b.type.rows && a.type.cols == b.type.cols && !a.type.array &&
              !b.type.array))
          fail(line, "'" + op + "' between a " + a.type.name() + " and a " + b.type.name());
        const K k = meet(a.type.k, b.type.k);
        a = convert(a, k);
        b = convert(b, k);
      }
      const K k = a.type.k;
      const Op o = k == K::Float ? (op == "==" ? FEQ : FNE) : (op == "==" ? IEQ : INE);
      Val all;
      for (int i = 0; i < a.type.cells(); ++i) {
        const Val c = one(o, K::Bool, {arg(a, i), arg(b, i)});
        if (i == 0) all = c;
        else all = one(op == "==" ? IAND : IOR, K::Bool, {arg(all, 0), arg(c, 0)});
      }
      return all;
    }
    if (!a.type.scalar() || !b.type.scalar() || !a.type.numeric() || !b.type.numeric())
      fail(line, "'" + op + "' compares two numbers; for vectors use lessThan() and the like");
    const K k = meet(a.type.k, b.type.k);
    a = convert(a, k);
    b = convert(b, k);
    Op o;
    if (k == K::Float) o = op == "<" ? FLT : op == "<=" ? FLE : op == ">" ? FGT : FGE;
    else if (k == K::UInt) o = op == "<" ? ULT : op == "<=" ? ULE : op == ">" ? UGT : UGE;
    else o = op == "<" ? ILT : op == "<=" ? ILE : op == ">" ? IGT : IGE;
    return one(o, K::Bool, {arg(a, 0), arg(b, 0)});
  }
  Val bitwise(const std::string &op, Val a, Val b, int line) {
    if (!a.type.integer() || !b.type.integer() || a.type.array || b.type.array)
      fail(line, "'" + op + "' takes ints or uints, not " + a.type.name() + " and " + b.type.name());
    if (op == "<<" || op == ">>") {
      // The result is the left one's type; the right may be int or uint.
      Type result = a.type;
      if (a.type.scalar() && !b.type.scalar()) fail(line, "a scalar shifted by a vector");
      std::vector<Val> parts;
      const Op o = op == "<<" ? ISHL : a.type.k == K::UInt ? USHR : ISHR;
      for (int i = 0; i < result.cells(); ++i) {
        Val y = slot(b, b.type.scalar() ? 0 : i);
        y.type.k = K::Int;
        parts.push_back(one(o, a.type.k, {arg(a, i), arg(y, 0)}));
      }
      return join(result, parts);
    }
    const K k = meet(a.type.k, b.type.k);
    a = convert(a, k);
    b = convert(b, k);
    Type result;
    if (a.type.scalar()) result = b.type;
    else if (b.type.scalar()) result = a.type;
    else if (a.type == b.type) result = a.type;
    else fail(line, "'" + op + "' between a " + a.type.name() + " and a " + b.type.name());
    const Op o = op == "&" ? IAND : op == "|" ? IOR : IXOR;
    std::vector<Val> parts;
    for (int i = 0; i < result.cells(); ++i)
      parts.push_back(one(o, k, {arg(a, a.type.scalar() ? 0 : i), arg(b, b.type.scalar() ? 0 : i)}));
    return join(result, parts);
  }
  // Whether working out `e` changes nothing: it may then be worked out for
  // lanes that do not need it, and && || ?: need no branch.
  bool pure(const Expr *e) {
    if (!e) return true;
    switch (e->kind) {
    case Expr::Assign:
    case Expr::PostInc:
    case Expr::PostDec: return false;
    case Expr::Unary:
      if (e->op == "++" || e->op == "--") return false;
      break;
    case Expr::Call: {
      if (e->op == "modf") return false;
      if (functions.count(e->op)) {
        const auto f = outParams.find(e->op);
        if (f != outParams.end())
          for (bool out : f->second)
            if (out) return false;
        for (const Function *fn : functions[e->op])
          if (fn->body && !functionPure(fn)) return false;
      }
      break;
    }
    default: break;
    }
    for (const auto &a : e->args)
      if (!pure(a.get())) return false;
    return true;
  }
  // A function is pure when running it for a lane that did not need it changes
  // nothing that lane can see: it writes only its own variables, discards
  // nothing, and calls only functions like itself.
  std::map<const Function *, bool> pureFunctions;
  bool functionPure(const Function *f) {
    const auto known = pureFunctions.find(f);
    if (known != pureFunctions.end()) return known->second;
    pureFunctions[f] = false; // while it is being looked at (recursion is refused anyway)
    std::set<std::string> locals;
    for (const auto &prm : f->params) locals.insert(prm.name);
    bool ok = true;
    auto local = [&](const Expr *target) {
      while (target && (target->kind == Expr::Index || target->kind == Expr::Field)) target = target->args[0].get();
      return target && target->kind == Expr::Name && locals.count(target->op);
    };
    std::function<void(const Expr *)> expr = [&](const Expr *e) {
      if (!e || !ok) return;
      if (e->kind == Expr::Assign || e->kind == Expr::PostInc || e->kind == Expr::PostDec ||
          (e->kind == Expr::Unary && (e->op == "++" || e->op == "--")))
        ok = ok && local(e->args[0].get());
      if (e->kind == Expr::Call) {
        if (e->op == "modf" && e->args.size() == 2) ok = ok && local(e->args[1].get());
        if (functions.count(e->op)) {
          for (const Function *fn : functions[e->op])
            if (fn->body && !functionPure(fn)) ok = false;
          const auto outs = outParams.find(e->op);
          if (outs != outParams.end())
            for (size_t i = 0; i < e->args.size() && i < outs->second.size(); ++i)
              if (outs->second[i]) ok = ok && local(e->args[i].get());
        }
      }
      for (const auto &a : e->args) expr(a.get());
    };
    std::function<void(const Stmt *)> stmt = [&](const Stmt *s) {
      if (!s || !ok) return;
      if (s->kind == Stmt::Discard) {
        ok = false;
        return;
      }
      for (const auto &d : s->decls) {
        expr(d.init.get());
        locals.insert(d.name);
      }
      for (const auto &c : s->children) stmt(c.get());
      stmt(s->init.get());
      expr(s->expr.get());
      expr(s->cond.get());
      expr(s->step.get());
      stmt(s->thenS.get());
      stmt(s->elseS.get());
      stmt(s->body.get());
    };
    stmt(f->body.get());
    pureFunctions[f] = ok;
    return ok;
  }
  Val logical(const Expr *e) {
    const std::string &op = e->op;
    Val a = expression(e->args[0].get());
    if (a.type != kBool) fail(e->line, "'" + op + "' takes bools");
    if (op == "^^") {
      Val b = expression(e->args[1].get());
      if (b.type != kBool) fail(e->line, "'" + op + "' takes bools");
      return one(IXOR, K::Bool, {arg(a, 0), arg(b, 0)});
    }
    if (a.isConst) {
      // Decided by the left: the right is not even looked at.
      if (op == "&&" && !a.konst[0]) return a;
      if (op == "||" && a.konst[0]) return a;
      Val b = expression(e->args[1].get());
      if (b.type != kBool) fail(e->line, "'" + op + "' takes bools");
      return b;
    }
    if (pure(e->args[1].get())) {
      Val b = expression(e->args[1].get());
      if (b.type != kBool) fail(e->line, "'" + op + "' takes bools");
      return one(op == "&&" ? IAND : IOR, K::Bool, {arg(a, 0), arg(b, 0)});
    }
    // The right side changes something: only the lanes that need it run it.
    const int result = keep();
    emitTo(code(), MOV, result, a.regs[0]);
    int test = a.regs[0];
    if (op == "||") test = one(BNOT, K::Bool, {arg(a, 0)}).regs[0];
    Node n;
    n.t = Node::If;
    n.cond = test;
    n.a = std::make_unique<Block>();
    n.b = std::make_unique<Block>();
    Block *then = n.a.get();
    current->nodes.push_back(std::move(n));
    Block *outer = current;
    current = then;
    {
      Deeper deeper(depth);
      Val b = expression(e->args[1].get());
      if (b.type != kBool) fail(e->line, "'" + op + "' takes bools");
      emitTo(code(), STORE, result, b.regs[0]);
    }
    current = outer;
    Val r;
    r.type = kBool;
    r.regs = {result};
    return r;
  }
  Val binary(const Expr *e) {
    const std::string &op = e->op;
    if (op == "&&" || op == "||" || op == "^^") return logical(e);
    Val a = expression(e->args[0].get());
    Val b = expression(e->args[1].get());
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") return arithmetic(op, a, b, e->line);
    if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=") return compare(op, a, b, e->line);
    if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") return bitwise(op, a, b, e->line);
    fail(e->line, "an unknown operator " + op);
  }
  Val unary(const Expr *e) {
    const std::string &op = e->op;
    if (op == "++" || op == "--") return increment(e, op == "++" ? 1 : -1, false);
    Val a = expression(e->args[0].get());
    if (op == "+") {
      if (!a.type.numeric()) fail(e->line, "'+' on a " + a.type.name());
      return a;
    }
    if (op == "-") {
      if (!a.type.numeric() || a.type.array) fail(e->line, "'-' on a " + a.type.name());
      std::vector<Val> parts;
      for (int i = 0; i < a.type.cells(); ++i) parts.push_back(one(a.type.k == K::Float ? FNEG : INEG, a.type.k, {arg(a, i)}));
      return join(a.type, parts);
    }
    if (op == "!") {
      if (a.type != kBool) fail(e->line, "'!' takes a bool");
      return one(BNOT, K::Bool, {arg(a, 0)});
    }
    if (op == "~") {
      if (!a.type.integer() || a.type.array) fail(e->line, "'~' takes ints or uints");
      std::vector<Val> parts;
      for (int i = 0; i < a.type.cells(); ++i) parts.push_back(one(INOT, a.type.k, {arg(a, i)}));
      return join(a.type, parts);
    }
    fail(e->line, "an unknown operator " + op);
  }
  Val increment(const Expr *e, int by, bool post) {
    Place pl = place(e->args[0].get());
    if (!pl.type.numeric() || pl.type.array || pl.type.matrix()) fail(e->line, "++ and -- take numbers");
    const Val before = read(pl);
    // The old value, kept: the place is about to change.
    std::vector<Val> keptParts, parts;
    for (int i = 0; i < pl.type.cells(); ++i) {
      const Val s = slot(before, i);
      if (post) keptParts.push_back(one(MOV, pl.type.k, {arg(s, 0)}));
      const Val step = pl.type.k == K::Float ? constFloat(float(by)) : constant(scalarOf(pl.type.k), {uint32_t(int32_t(by))});
      parts.push_back(one(pl.type.k == K::Float ? FADD : IADD, pl.type.k, {arg(s, 0), arg(step, 0)}));
    }
    const Val after = join(pl.type, parts);
    write(pl, after, false);
    return post ? join(pl.type, keptParts) : after;
  }
  Val assign(const Expr *e, bool discarded) {
    const std::string &op = e->op;
    Place pl = place(e->args[0].get());
    Val v = expression(e->args[1].get());
    if (op != "=") {
      const std::string base = op.substr(0, op.size() - 1);
      const Val now = read(pl);
      if (base == "+" || base == "-" || base == "*" || base == "/" || base == "%") v = arithmetic(base, now, v, e->line);
      else v = bitwise(base, now, v, e->line);
    }
    v = coerce(v, pl.type, e->line, "in an assignment");
    write(pl, v, false);
    if (discarded) return Val{}; // a statement of its own: nobody reads it back
    return read(pl);
  }
  Val ternary(const Expr *e) {
    const Val c = condition(e->args[0].get());
    if (c.isConst) return expression(e->args[c.konst[0] ? 1 : 2].get());
    if (pure(e->args[1].get()) && pure(e->args[2].get())) {
      Val a = expression(e->args[1].get());
      Val b = expression(e->args[2].get());
      unify(a, b, e->line);
      std::vector<Val> parts;
      for (int i = 0; i < a.type.cells(); ++i) parts.push_back(one(SEL, a.type.k, {arg(c, 0), arg(a, i), arg(b, i)}));
      return join(a.type, parts);
    }
    // A side changes something: each lane runs only its own.
    Type t;
    {
      // Find the type without emitting: compile both into scratch blocks,
      // deeper, so that no variable takes registers this code never fills.
      Deeper deeper(depth);
      Block scratch;
      Block *outer = current;
      current = &scratch;
      Val a = expression(e->args[1].get());
      Val b = expression(e->args[2].get());
      unify(a, b, e->line);
      t = a.type;
      current = outer;
    }
    std::vector<int> result = t.array ? contiguous(t.cells()) : std::vector<int>();
    if (!t.array)
      for (int k = 0; k < t.cells(); ++k) result.push_back(keep());
    Node n;
    n.t = Node::If;
    n.cond = c.regs[0];
    n.a = std::make_unique<Block>();
    n.b = std::make_unique<Block>();
    Block *a = n.a.get(), *b = n.b.get();
    current->nodes.push_back(std::move(n));
    Block *outer = current;
    Deeper deeper(depth);
    current = a;
    {
      Val v = coerce(expression(e->args[1].get()), t, e->line, "in ?:");
      for (int k = 0; k < t.cells(); ++k) emitTo(code(), STORE, result[size_t(k)], v.regs[size_t(k)]);
    }
    current = b;
    {
      Val v = coerce(expression(e->args[2].get()), t, e->line, "in ?:");
      for (int k = 0; k < t.cells(); ++k) emitTo(code(), STORE, result[size_t(k)], v.regs[size_t(k)]);
    }
    current = outer;
    Val r;
    r.type = t;
    r.regs = result;
    return r;
  }
  void unify(Val &a, Val &b, int line) {
    if (a.type == b.type) return;
    if (implicitly(a.type, b.type)) a = convert(a, b.type.k);
    else if (implicitly(b.type, a.type)) b = convert(b, a.type.k);
    else fail(line, "the two sides of ?: are a " + a.type.name() + " and a " + b.type.name());
  }

  // ---- constructors -----------------------------------------------------------------
  Val construct(const Expr *e) {
    Type t = typeFromName(e->type.name, e->line);
    std::vector<Val> args;
    for (const auto &a : e->args) args.push_back(expression(a.get()));
    if (e->type.arraySize != 0) {
      // float[3](...) and float[](...)
      int n = e->type.arraySize < 0 ? int(args.size()) : arraySizeOf(e->type.sizeExpr, e->line);
      if (int(args.size()) != n) fail(e->line, "an array of " + std::to_string(n) + " given " + std::to_string(args.size()) + " values");
      Type element = t;
      std::vector<Val> parts;
      for (const Val &a : args) {
        const Val v = coerce(a, element, e->line, "in an array");
        for (int i = 0; i < v.type.cells(); ++i) parts.push_back(slot(v, i));
      }
      t.array = n;
      return join(t, parts);
    }
    if (t.k == K::Void || t.k == K::Sampler) fail(e->line, "cannot make a " + t.name());
    if (args.empty()) fail(e->line, t.name() + "() needs something to make it from");
    for (const Val &a : args)
      if (a.type.array) fail(e->line, "an array cannot make a " + t.name());
    if (t.matrix()) return constructMatrix(t, args, e->line);
    // Scalars and vectors: a single scalar fills every slot; otherwise the
    // arguments' slots in order.
    std::vector<Val> scalars;
    if (args.size() == 1 && args[0].type.scalar()) {
      const Val s = convertScalar(args[0], t.k);
      for (int i = 0; i < t.cells(); ++i) scalars.push_back(s);
      return join(t, scalars);
    }
    for (const Val &a : args) {
      if (a.type.matrix() && t.vector()) {
        for (int i = 0; i < a.type.cells(); ++i) scalars.push_back(slot(a, i));
        continue;
      }
      for (int i = 0; i < a.type.cells(); ++i) scalars.push_back(slot(a, i));
    }
    if (int(scalars.size()) < t.cells()) fail(e->line, "not enough values to make a " + t.name());
    if (int(scalars.size()) > t.cells() && args.size() > 1) {
      // GLSL: every argument must be used; only a single larger one is cut down.
      int used = 0;
      for (size_t i = 0; i + 1 < args.size(); ++i) used += args[i].type.cells();
      if (used >= t.cells()) fail(e->line, "too many values to make a " + t.name());
    }
    std::vector<Val> parts;
    for (int i = 0; i < t.cells(); ++i) parts.push_back(convertScalar(scalars[size_t(i)], t.k));
    return join(t, parts);
  }
  Val constructMatrix(const Type &t, const std::vector<Val> &args, int line) {
    const int n = t.cols;
    std::vector<Val> parts;
    if (args.size() == 1 && args[0].type.scalar()) {
      const Val s = convertScalar(args[0], K::Float);
      const Val zero = constFloat(0);
      for (int c = 0; c < n; ++c)
        for (int r = 0; r < n; ++r) parts.push_back(c == r ? s : zero);
      return join(t, parts);
    }
    if (args.size() == 1 && args[0].type.matrix()) {
      const Val &m = args[0];
      for (int c = 0; c < n; ++c)
        for (int r = 0; r < n; ++r) {
          if (c < m.type.cols && r < m.type.rows) parts.push_back(slot(m, c * m.type.rows + r));
          else parts.push_back(constFloat(c == r ? 1.f : 0.f));
        }
      return join(t, parts);
    }
    std::vector<Val> scalars;
    for (const Val &a : args)
      for (int i = 0; i < a.type.cells(); ++i) scalars.push_back(convertScalar(slot(a, i), K::Float));
    if (int(scalars.size()) != t.cells()) fail(line, t.name() + " needs " + std::to_string(t.cells()) + " values");
    return join(t, scalars);
  }

  // ---- calls --------------------------------------------------------------------
  Val callExpr(const Expr *e) {
    if (!functions.count(e->op)) return builtin(e);
    // Arguments are worked out left to right, each as the call site sees it
    // then: one written by a later argument (f(x, x += 1.)) is copied first.
    bool allPure = true;
    for (const auto &a : e->args) allPure = allPure && pure(a.get());
    const auto outs = outParams.find(e->op);
    std::vector<Argument> args;
    std::vector<Val> types;
    for (size_t i = 0; i < e->args.size(); ++i) {
      Argument a;
      if (outs != outParams.end() && i < outs->second.size() && outs->second[i]) {
        a.place = place(e->args[i].get());
        a.isPlace = true;
        a.value.type = a.place.type;
      } else {
        a.value = expression(e->args[i].get());
        if (!allPure) a.value = snapshot(a.value);
      }
      types.push_back(a.value);
      args.push_back(std::move(a));
    }
    const Function *f = resolve(e->op, types, e->line);
    if (!f) fail(e->line, "unknown function " + e->op);
    return call(*f, std::move(args), e->line);
  }
  // A copy of a value whose registers something may write before it is used.
  Val snapshot(const Val &v) {
    if (v.isConst || v.uniform || v.owns || v.immutable) return v;
    std::vector<Val> parts;
    for (int i = 0; i < v.type.cells(); ++i) parts.push_back(one(MOV, v.type.k, {arg(v, i)}));
    return join(v.type, parts);
  }

  // ---- built-in functions ---------------------------------------------------------
  Val floats(Val v, int line, const std::string &fn) {
    if (!v.type.numeric() || v.type.array || v.type.matrix()) fail(line, fn + " takes floats, not a " + v.type.name());
    return convert(v, K::Float);
  }
  // A function of one value, slot by slot.
  Val map1(Op op, Val a, K resultKind) {
    std::vector<Val> parts;
    for (int i = 0; i < a.type.cells(); ++i) parts.push_back(one(op, resultKind, {arg(a, i)}));
    return join(a.type.withKind(resultKind), parts);
  }
  Val map1(Op op, Val a) { return map1(op, a, a.type.k); }
  // Of two: a scalar on either side goes with every slot of the other.
  Val map2(Op op, Val a, Val b, K resultKind, int line, const std::string &fn) {
    Type t;
    if (a.type == b.type) t = a.type;
    else if (b.type.scalar()) t = a.type;
    else if (a.type.scalar()) t = b.type;
    else fail(line, fn + " between a " + a.type.name() + " and a " + b.type.name());
    std::vector<Val> parts;
    for (int i = 0; i < t.cells(); ++i)
      parts.push_back(one(op, resultKind, {arg(a, a.type.scalar() ? 0 : i), arg(b, b.type.scalar() ? 0 : i)}));
    return join(t.withKind(resultKind), parts);
  }
  Val map3(Op op, Val a, Val b, Val c, K resultKind, int line, const std::string &fn) {
    Type t = a.type;
    for (const Val *v : {&b, &c})
      if (!v->type.scalar()) {
        if (!t.scalar() && t != v->type) fail(line, fn + " with mismatched sizes");
        t = v->type;
      }
    for (const Val *v : {&a, &b, &c})
      if (!v->type.scalar() && v->type != t) fail(line, fn + " with mismatched sizes");
    std::vector<Val> parts;
    for (int i = 0; i < t.cells(); ++i)
      parts.push_back(one(op, resultKind,
                          {arg(a, a.type.scalar() ? 0 : i), arg(b, b.type.scalar() ? 0 : i), arg(c, c.type.scalar() ? 0 : i)}));
    return join(t.withKind(resultKind), parts);
  }
  Val dot(const Val &a, const Val &b) {
    Val sum = one(FMUL, K::Float, {arg(a, 0), arg(b, 0)});
    for (int i = 1; i < a.type.cells(); ++i) {
      const Val prod = one(FMUL, K::Float, {arg(a, i), arg(b, i)});
      sum = one(FADD, K::Float, {arg(sum, 0), arg(prod, 0)});
    }
    return sum;
  }
  Val builtin(const Expr *e) {
    const std::string &fn = e->op;
    const int line = e->line;
    const size_t n = e->args.size();
    auto need = [&](size_t count) {
      if (n != count) fail(line, fn + " takes " + std::to_string(count) + " argument(s)");
    };
    // The pictures: the sampler is a name, not a value.
    if (fn == "texelFetch" || fn == "texture" || fn == "textureSize") {
      if (n < 1 || e->args[0]->kind != Expr::Name) fail(line, fn + " needs src or wavyOriginalTexture first");
      const Symbol *s = lookup(e->args[0]->op);
      if (!s || s->kind != Symbol::Sampler) fail(line, fn + " needs src or wavyOriginalTexture first");
      const int sampler = s->sampler;
      if (fn == "textureSize") {
        need(2);
        expression(e->args[1].get());
        std::vector<Val> parts{hostValue(sampler == 0 ? H_SOURCE_W : H_ORIGINAL_W), hostValue(sampler == 0 ? H_SOURCE_H : H_ORIGINAL_H)};
        return convert(join(vectorOf(K::Float, 2), parts), K::Int);
      }
      if (fn == "texelFetch") need(3);
      else if (n != 2 && n != 3) fail(line, "texture takes a sampler, a vec2 and maybe a bias");
      Val where = expression(e->args[1].get());
      if (n == 3) expression(e->args[2].get());
      if (fn == "texelFetch") {
        if (where.type != vectorOf(K::Int, 2) && where.type != vectorOf(K::UInt, 2)) fail(line, "texelFetch takes an ivec2 position");
        where = convert(where, K::Int);
      } else {
        if (where.type != vectorOf(K::Float, 2)) fail(line, "texture takes a vec2 position");
      }
      const int list = int(p->lists.size());
      const bool uniform = where.uniform;
      std::vector<Val> parts;
      for (int k = 0; k < 4; ++k) {
        Val v;
        v.type = kFloat;
        v.uniform = uniform;
        v.immutable = uniform;
        v.owns = !uniform;
        const int d = uniform ? keep() : fresh();
        v.regs = {d};
        p->lists.push_back(d);
        parts.push_back(v);
      }
      emitTo(uniform ? p->prologue : code(), fn == "texelFetch" ? TEXEL : TEXTURE, 0, where.regs[0], where.regs[1], sampler, list);
      return join(vectorOf(K::Float, 4), parts);
    }
    std::vector<Val> a;
    for (const auto &x : e->args) a.push_back(expression(x.get()));
    static const std::map<std::string, Op> unaryFloat{
        {"sin", FSIN},    {"cos", FCOS},     {"tan", FTAN},     {"asin", FASIN},   {"acos", FACOS},   {"sinh", FSINH},
        {"cosh", FCOSH},  {"tanh", FTANH},   {"asinh", FASINH}, {"acosh", FACOSH}, {"atanh", FATANH}, {"exp", FEXP},
        {"log", FLOG},    {"exp2", FEXP2},   {"log2", FLOG2},   {"sqrt", FSQRT},   {"inversesqrt", FINVSQRT},
        {"floor", FFLOOR}, {"ceil", FCEIL}, {"fract", FFRACT}, {"trunc", FTRUNC}, {"round", FROUND}, {"roundEven", FROUNDEVEN}};
    if (const auto f = unaryFloat.find(fn); f != unaryFloat.end()) {
      need(1);
      return map1(f->second, floats(a[0], line, fn));
    }
    if (fn == "radians" || fn == "degrees") {
      need(1);
      const Val x = floats(a[0], line, fn);
      const Val k = constFloat(fn == "radians" ? float(3.14159265358979323846 / 180.0) : float(180.0 / 3.14159265358979323846));
      return map2(FMUL, x, k, K::Float, line, fn);
    }
    if (fn == "atan") {
      if (n == 1) return map1(FATAN, floats(a[0], line, fn));
      need(2);
      return map2(FATAN2, floats(a[0], line, fn), floats(a[1], line, fn), K::Float, line, fn);
    }
    if (fn == "pow") {
      need(2);
      return map2(FPOW, floats(a[0], line, fn), floats(a[1], line, fn), K::Float, line, fn);
    }
    if (fn == "abs" || fn == "sign") {
      need(1);
      if (a[0].type.k == K::Float) return map1(fn == "abs" ? FABS : FSIGN, a[0]);
      if (a[0].type.k == K::Int) return map1(fn == "abs" ? IABS : ISIGN, a[0]);
      fail(line, fn + " takes floats or ints");
    }
    if (fn == "min" || fn == "max") {
      need(2);
      const K k = meet(a[0].type.k, a[1].type.k);
      if (!a[0].type.numeric() || !a[1].type.numeric()) fail(line, fn + " takes numbers");
      const Val x = convert(a[0], k), y = convert(a[1], k);
      const Op op = k == K::Float ? (fn == "min" ? FMIN : FMAX) : k == K::UInt ? (fn == "min" ? UMIN : UMAX) : (fn == "min" ? IMIN : IMAX);
      return map2(op, x, y, k, line, fn);
    }
    if (fn == "clamp") {
      need(3);
      K k = meet(meet(a[0].type.k, a[1].type.k), a[2].type.k);
      if (!a[0].type.numeric()) fail(line, "clamp takes numbers");
      const Op op = k == K::Float ? FCLAMP : k == K::UInt ? UCLAMP : ICLAMP;
      return map3(op, convert(a[0], k), convert(a[1], k), convert(a[2], k), k, line, fn);
    }
    if (fn == "mix") {
      need(3);
      if (a[2].type.k == K::Bool) {
        // mix(x, y, bvec): y where true, x where false.
        std::vector<Val> parts;
        unify(a[0], a[1], line);
        for (int i = 0; i < a[0].type.cells(); ++i)
          parts.push_back(one(SEL, a[0].type.k, {arg(a[2], a[2].type.scalar() ? 0 : i), arg(a[1], i), arg(a[0], i)}));
        return join(a[0].type, parts);
      }
      return map3(FMIX, floats(a[0], line, fn), floats(a[1], line, fn), floats(a[2], line, fn), K::Float, line, fn);
    }
    if (fn == "step") {
      need(2);
      return map2(FSTEP, floats(a[0], line, fn), floats(a[1], line, fn), K::Float, line, fn);
    }
    if (fn == "smoothstep") {
      need(3);
      return map3(FSMOOTH, floats(a[0], line, fn), floats(a[1], line, fn), floats(a[2], line, fn), K::Float, line, fn);
    }
    if (fn == "mod") {
      need(2);
      if (a[0].type.integer() && a[1].type.integer()) fail(line, "mod takes floats: use % for whole numbers");
      return map2(FMOD, floats(a[0], line, fn), floats(a[1], line, fn), K::Float, line, fn);
    }
    if (fn == "modf") {
      need(2);
      const Val x = floats(a[0], line, fn);
      const Val whole = map1(FTRUNC, x);
      Place pl = place(e->args[1].get());
      if (pl.type != whole.type) fail(line, "modf's second argument must be a " + whole.type.name());
      write(pl, whole, false);
      return map2(FSUB, x, whole, K::Float, line, fn);
    }
    if (fn == "isnan" || fn == "isinf") {
      need(1);
      return map1(fn == "isnan" ? FISNAN : FISINF, floats(a[0], line, fn), K::Bool);
    }
    if (fn == "floatBitsToInt" || fn == "floatBitsToUint") {
      // The same bits, read as a whole number: a register holds either.
      need(1);
      Val r = floats(a[0], line, fn);
      r.type.k = fn == "floatBitsToInt" ? K::Int : K::UInt;
      return r;
    }
    if (fn == "intBitsToFloat" || fn == "uintBitsToFloat") {
      need(1);
      Val r = a[0];
      if (!r.type.integer()) fail(line, fn + " takes whole numbers");
      r.type.k = K::Float;
      return r;
    }
    if (fn == "length") {
      need(1);
      const Val x = floats(a[0], line, fn);
      return one(FSQRT, K::Float, {arg(dot(x, x), 0)});
    }
    if (fn == "distance") {
      need(2);
      const Val d = map2(FSUB, floats(a[0], line, fn), floats(a[1], line, fn), K::Float, line, fn);
      return one(FSQRT, K::Float, {arg(dot(d, d), 0)});
    }
    if (fn == "dot") {
      need(2);
      const Val x = floats(a[0], line, fn), y = floats(a[1], line, fn);
      if (x.type != y.type) fail(line, "dot of a " + x.type.name() + " and a " + y.type.name());
      return dot(x, y);
    }
    if (fn == "normalize") {
      need(1);
      const Val x = floats(a[0], line, fn);
      const Val inv = one(FINVSQRT, K::Float, {arg(dot(x, x), 0)});
      return map2(FMUL, x, inv, K::Float, line, fn);
    }
    if (fn == "cross") {
      need(2);
      const Val x = floats(a[0], line, fn), y = floats(a[1], line, fn);
      if (x.type != vectorOf(K::Float, 3) || y.type != x.type) fail(line, "cross takes two vec3");
      auto c = [&](int i, int j) {
        const Val p1 = one(FMUL, K::Float, {arg(x, i), arg(y, j)});
        const Val p2 = one(FMUL, K::Float, {arg(x, j), arg(y, i)});
        return one(FSUB, K::Float, {arg(p1, 0), arg(p2, 0)});
      };
      return join(x.type, {c(1, 2), c(2, 0), c(0, 1)});
    }
    if (fn == "reflect") {
      need(2);
      const Val i = floats(a[0], line, fn), nn = floats(a[1], line, fn);
      const Val d = dot(nn, i);
      const Val twice = one(FMUL, K::Float, {arg(constFloat(2), 0), arg(d, 0)});
      return map2(FSUB, i, map2(FMUL, nn, twice, K::Float, line, fn), K::Float, line, fn);
    }
    if (fn == "refract") {
      need(3);
      const Val i = floats(a[0], line, fn), nn = floats(a[1], line, fn), eta = floats(a[2], line, fn);
      const Val d = dot(nn, i);
      const Val oneV = constFloat(1);
      const Val d2 = one(FMUL, K::Float, {arg(d, 0), arg(d, 0)});
      const Val e2 = one(FMUL, K::Float, {arg(eta, 0), arg(eta, 0)});
      const Val k = one(FSUB, K::Float, {arg(oneV, 0), arg(one(FMUL, K::Float, {arg(e2, 0), arg(one(FSUB, K::Float, {arg(oneV, 0), arg(d2, 0)}), 0)}), 0)});
      const Val zero = constFloat(0);
      const Val negative = one(FLT, K::Bool, {arg(k, 0), arg(zero, 0)});
      const Val root = one(FSQRT, K::Float, {arg(k, 0)});
      const Val factor = one(FADD, K::Float, {arg(one(FMUL, K::Float, {arg(eta, 0), arg(d, 0)}), 0), arg(root, 0)});
      const Val r = map2(FSUB, map2(FMUL, i, eta, K::Float, line, fn), map2(FMUL, nn, factor, K::Float, line, fn), K::Float, line, fn);
      std::vector<Val> parts;
      for (int s = 0; s < r.type.cells(); ++s) parts.push_back(one(SEL, K::Float, {arg(negative, 0), arg(zero, 0), arg(r, s)}));
      return join(r.type, parts);
    }
    if (fn == "faceforward") {
      need(3);
      const Val nn = floats(a[0], line, fn), i = floats(a[1], line, fn), nref = floats(a[2], line, fn);
      const Val d = dot(nref, i);
      const Val negative = one(FLT, K::Bool, {arg(d, 0), arg(constFloat(0), 0)});
      std::vector<Val> parts;
      for (int s = 0; s < nn.type.cells(); ++s) {
        const Val neg = one(FNEG, K::Float, {arg(nn, s)});
        parts.push_back(one(SEL, K::Float, {arg(negative, 0), arg(nn, s), arg(neg, 0)}));
      }
      return join(nn.type, parts);
    }
    if (fn == "lessThan" || fn == "lessThanEqual" || fn == "greaterThan" || fn == "greaterThanEqual" || fn == "equal" || fn == "notEqual") {
      need(2);
      if (!a[0].type.vector() || a[0].type.rows != a[1].type.rows) fail(line, fn + " takes two vectors of the same size");
      const K k = meet(a[0].type.k, a[1].type.k);
      const Val x = convert(a[0], k), y = convert(a[1], k);
      Op op;
      const bool f = k == K::Float, uu = k == K::UInt;
      if (fn == "lessThan") op = f ? FLT : uu ? ULT : ILT;
      else if (fn == "lessThanEqual") op = f ? FLE : uu ? ULE : ILE;
      else if (fn == "greaterThan") op = f ? FGT : uu ? UGT : IGT;
      else if (fn == "greaterThanEqual") op = f ? FGE : uu ? UGE : IGE;
      else if (fn == "equal") op = f ? FEQ : IEQ;
      else op = f ? FNE : INE;
      return map2(op, x, y, K::Bool, line, fn);
    }
    if (fn == "any" || fn == "all") {
      need(1);
      if (a[0].type.k != K::Bool || !a[0].type.vector()) fail(line, fn + " takes a bvec");
      Val r = slot(a[0], 0);
      for (int i = 1; i < a[0].type.cells(); ++i) r = one(fn == "any" ? IOR : IAND, K::Bool, {arg(r, 0), arg(a[0], i)});
      return r;
    }
    if (fn == "not") {
      need(1);
      if (a[0].type.k != K::Bool) fail(line, "not takes a bvec");
      return map1(BNOT, a[0]);
    }
    if (fn == "matrixCompMult") {
      need(2);
      if (!a[0].type.matrix() || a[0].type != a[1].type) fail(line, "matrixCompMult takes two matrices of the same size");
      return map2(FMUL, a[0], a[1], K::Float, line, fn);
    }
    if (fn == "transpose") {
      need(1);
      if (!a[0].type.matrix()) fail(line, "transpose takes a matrix");
      const int m = a[0].type.cols;
      std::vector<Val> parts;
      for (int c = 0; c < m; ++c)
        for (int r = 0; r < m; ++r) parts.push_back(slot(a[0], r * m + c));
      return join(a[0].type, parts);
    }
    if (fn == "determinant") {
      need(1);
      if (!a[0].type.matrix()) fail(line, "determinant takes a matrix");
      const Val &m = a[0];
      auto at = [&](int c, int r) { return slot(m, c * m.type.rows + r); };
      auto mul = [&](Val x, Val y) { return one(FMUL, K::Float, {arg(x, 0), arg(y, 0)}); };
      auto sub = [&](Val x, Val y) { return one(FSUB, K::Float, {arg(x, 0), arg(y, 0)}); };
      auto add = [&](Val x, Val y) { return one(FADD, K::Float, {arg(x, 0), arg(y, 0)}); };
      if (m.type.cols == 2) return sub(mul(at(0, 0), at(1, 1)), mul(at(1, 0), at(0, 1)));
      if (m.type.cols == 3) {
        const Val c0 = sub(mul(at(1, 1), at(2, 2)), mul(at(2, 1), at(1, 2)));
        const Val c1 = sub(mul(at(0, 1), at(2, 2)), mul(at(2, 1), at(0, 2)));
        const Val c2 = sub(mul(at(0, 1), at(1, 2)), mul(at(1, 1), at(0, 2)));
        return add(sub(mul(at(0, 0), c0), mul(at(1, 0), c1)), mul(at(2, 0), c2));
      }
      fail(line, "determinant of a mat4 is not supported");
    }
    if (fn == "dFdx" || fn == "dFdy" || fn == "fwidth") fail(line, fn + " is not supported: a pixel here does not know its neighbours' values");
    fail(line, "unknown function " + fn);
  }
};

// =============================================================================
// The machine
// =============================================================================
// The lowest lane in a mask that is not empty.
inline int lowestLane(uint64_t mask) {
#ifdef _MSC_VER
  unsigned long at;
  _BitScanForward64(&at, mask);
  return int(at);
#else
  return __builtin_ctzll(mask);
#endif
}
// A lane mask: bit l is lane l, running.
using Mask = uint64_t;
static_assert(B == 64, "a Mask holds one bit per lane");

// The lanes whose number is not zero, as a mask.
inline Mask bitsOf(const int32_t *c) {
  Mask m = 0;
#if defined(_M_X64) || defined(__SSE2__)
  const __m128i zero = _mm_setzero_si128();
  for (int g = 0; g < B / 4; ++g) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(c + g * 4));
    const int zeros = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(v, zero)));
    m |= Mask(~zeros & 15) << (g * 4);
  }
#else
  for (int l = 0; l < B; ++l) m |= Mask(c[l] != 0) << l;
#endif
  return m;
}

#if defined(_M_X64) || defined(__SSE2__)
#define WAVY_SSE2 1
// Four lanes at a time where the compiler leaves a loop going lane by lane --
// min, max, clamp, select, float to int, shifts, floor -- with the same
// results, bit for bit, as the one-lane functions above: NaN, signed zeros and
// the ends of the integer range included. Measured over Wavy-Flo's own 52
// shaders, these took 150 to 300 cycles for 64 lanes where an add takes 50.
namespace sse {
inline __m128 ld(const float *p) { return _mm_loadu_ps(p); }
inline void st(float *p, __m128 v) { _mm_storeu_ps(p, v); }
inline __m128i ldi(const void *p) { return _mm_loadu_si128(static_cast<const __m128i *>(p)); }
inline void sti(void *p, __m128i v) { _mm_storeu_si128(static_cast<__m128i *>(p), v); }
// m ? a : b, bit by bit.
inline __m128i pick(__m128i m, __m128i a, __m128i b) { return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b)); }
// std::min and std::max of int32 (SSE2 has no pminsd): (b < a) ? b : a, and (a < b) ? b : a.
inline __m128i imin(__m128i a, __m128i b) { return pick(_mm_cmpgt_epi32(a, b), b, a); }
inline __m128i imax(__m128i a, __m128i b) { return pick(_mm_cmpgt_epi32(b, a), b, a); }
// floatToInt: cut towards zero, held to the range, NaN 0.
inline __m128i toInt(__m128 x) {
  __m128i t = _mm_cvttps_epi32(x); // INT_MIN for NaN and anything out of range
  t = _mm_add_epi32(t, _mm_castps_si128(_mm_cmpge_ps(x, _mm_set1_ps(2147483648.f)))); // INT_MIN - 1: INT_MAX
  return _mm_andnot_si128(_mm_castps_si128(_mm_cmpunord_ps(x, x)), t);
}
// std::floor: whole numbers from 2^23 up, the infinities and NaN as they are;
// below that cut towards zero and one down where that went up; -0 stays -0.
inline __m128 floor(__m128 x) {
  const __m128 sign = _mm_set1_ps(-0.f);
  const __m128 keep = _mm_cmpnlt_ps(_mm_andnot_ps(sign, x), _mm_set1_ps(8388608.f));
  __m128 t = _mm_cvtepi32_ps(_mm_cvttps_epi32(x));
  t = _mm_sub_ps(t, _mm_and_ps(_mm_cmpgt_ps(t, x), _mm_set1_ps(1.f)));
  t = _mm_or_ps(t, _mm_and_ps(x, sign));
  return _mm_or_ps(_mm_and_ps(keep, x), _mm_andnot_ps(keep, t));
}
// ---- the functions a shader spends its time in, four lanes at a time ----
// The C library takes them a lane at a time: pow was three quarters of
// Exposure and Levels, sin, cos and exp most of Light Leak. These are the
// classic Cephes single-precision forms (range reduction, a short polynomial),
// within 2 ulp of the library over the ranges below (cpu_shader::check holds
// them to it). Outside those ranges -- and for NaN, the infinities, zero
// where it matters -- a lane is worked out by the library as before, so the
// edges behave exactly as they did.
inline __m128 polynomial(__m128 x, std::initializer_list<float> c) {
  auto it = c.begin();
  __m128 y = _mm_set1_ps(*it++);
  for (; it != c.end(); ++it) y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(*it));
  return y;
}
// The lanes of `fast` worked out here; the rest by the library.
template <class Scalar> inline __m128 mend(__m128 fast, __m128 bad, __m128 x, Scalar scalar) {
  const int lanes = _mm_movemask_ps(bad);
  if (!lanes) return fast;
  alignas(16) float out[4], in[4];
  _mm_store_ps(out, fast);
  _mm_store_ps(in, x);
  for (int k = 0; k < 4; ++k)
    if (lanes >> k & 1) out[k] = scalar(in[k]);
  return _mm_load_ps(out);
}
// e^x for x where the result is a normal float.
inline __m128 expFast(__m128 x) {
  const __m128 n = floor(_mm_add_ps(_mm_mul_ps(x, _mm_set1_ps(1.44269504088896341f)), _mm_set1_ps(.5f)));
  __m128 r = _mm_sub_ps(x, _mm_mul_ps(n, _mm_set1_ps(0.693359375f)));
  r = _mm_sub_ps(r, _mm_mul_ps(n, _mm_set1_ps(-2.12194440e-4f)));
  const __m128 r2 = _mm_mul_ps(r, r);
  __m128 y = polynomial(r, {1.9875691500e-4f, 1.3981999507e-3f, 8.3334519073e-3f, 4.1665795894e-2f, 1.6666665459e-1f, 5.0000001201e-1f});
  y = _mm_add_ps(_mm_add_ps(_mm_mul_ps(y, r2), r), _mm_set1_ps(1.f));
  const __m128i scale = _mm_slli_epi32(_mm_add_epi32(_mm_cvttps_epi32(n), _mm_set1_epi32(127)), 23);
  return _mm_mul_ps(y, _mm_castsi128_ps(scale));
}
// e^-87.33 is the least normal float, e^88.37 near the largest; below e^-105
// nothing is left (0), above e^89 the infinity. Only the bands between, and
// NaN, are the library's: a gaussian's far tail -- Light Leak's -- is all 0.
inline __m128 expEnds(__m128 x, __m128 fast) {
  const __m128 none = _mm_cmplt_ps(x, _mm_set1_ps(-105.f)), all = _mm_cmpgt_ps(x, _mm_set1_ps(89.f));
  return _mm_or_ps(_mm_andnot_ps(_mm_or_ps(none, all), fast), _mm_and_ps(all, _mm_set1_ps(INFINITY)));
}
inline __m128 expBad(__m128 x, __m128 ok) {
  const __m128 ends = _mm_or_ps(_mm_cmplt_ps(x, _mm_set1_ps(-105.f)), _mm_cmpgt_ps(x, _mm_set1_ps(89.f)));
  return _mm_andnot_ps(_mm_or_ps(ok, ends), _mm_castsi128_ps(_mm_set1_epi32(-1)));
}
inline __m128 exp(__m128 x) {
  const __m128 ok = _mm_and_ps(_mm_cmpge_ps(x, _mm_set1_ps(-87.33f)), _mm_cmple_ps(x, _mm_set1_ps(88.37f)));
  const __m128 fast = expEnds(x, expFast(_mm_and_ps(ok, x)));
  return mend(fast, expBad(x, ok), x, [](float v) { return std::exp(v); });
}
// ln x for x a normal positive float.
inline __m128 logFast(__m128 x) {
  const __m128i bits = _mm_castps_si128(x);
  __m128 e = _mm_cvtepi32_ps(_mm_sub_epi32(_mm_srli_epi32(bits, 23), _mm_set1_epi32(126)));
  // The mantissa in [0.5, 1), then in [sqrt(1/2), sqrt(2)) less one.
  __m128 m = _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(bits, _mm_set1_epi32(0x007FFFFF)), _mm_set1_epi32(0x3F000000)));
  const __m128 small = _mm_cmplt_ps(m, _mm_set1_ps(0.707106781186547524f));
  e = _mm_sub_ps(e, _mm_and_ps(small, _mm_set1_ps(1.f)));
  m = _mm_add_ps(_mm_sub_ps(m, _mm_set1_ps(1.f)), _mm_and_ps(small, m));
  const __m128 z = _mm_mul_ps(m, m);
  __m128 y = polynomial(m, {7.0376836292e-2f, -1.1514610310e-1f, 1.1676998740e-1f, -1.2420140846e-1f, 1.4249322787e-1f,
                            -1.6668057665e-1f, 2.0000714765e-1f, -2.4999993993e-1f, 3.3333331174e-1f});
  y = _mm_mul_ps(_mm_mul_ps(y, m), z);
  y = _mm_add_ps(y, _mm_mul_ps(e, _mm_set1_ps(-2.12194440e-4f)));
  y = _mm_sub_ps(y, _mm_mul_ps(z, _mm_set1_ps(.5f)));
  return _mm_add_ps(_mm_add_ps(m, y), _mm_mul_ps(e, _mm_set1_ps(0.693359375f)));
}
inline __m128 normalPositive(__m128 x) {
  return _mm_and_ps(_mm_cmpge_ps(x, _mm_set1_ps(1.17549435e-38f)), _mm_cmplt_ps(x, _mm_set1_ps(3.40282347e+38f)));
}
inline __m128 log(__m128 x) {
  const __m128 ok = normalPositive(x);
  return mend(logFast(_mm_or_ps(_mm_and_ps(ok, x), _mm_andnot_ps(ok, _mm_set1_ps(1.f)))), _mm_cmpeq_ps(ok, _mm_setzero_ps()), x,
              [](float v) { return std::log(v); });
}
// x^y as e^(y ln x), for x a normal positive float and a result in range;
// anything else -- x 0 or below, the infinities, NaN -- the library's pow.
inline __m128 pow(__m128 x, __m128 y) {
  const __m128 ok0 = normalPositive(x);
  const __m128 power = _mm_mul_ps(y, logFast(_mm_or_ps(_mm_and_ps(ok0, x), _mm_andnot_ps(ok0, _mm_set1_ps(1.f)))));
  const __m128 ok = _mm_and_ps(ok0, _mm_and_ps(_mm_cmpge_ps(power, _mm_set1_ps(-87.33f)), _mm_cmple_ps(power, _mm_set1_ps(88.37f))));
  __m128 fast = expFast(_mm_and_ps(ok, power));
  // x^1 and x^2 as exactly as the library has them: a gamma of 1 changes nothing.
  const __m128 once = _mm_cmpeq_ps(y, _mm_set1_ps(1.f)), twice = _mm_cmpeq_ps(y, _mm_set1_ps(2.f));
  fast = _mm_or_ps(_mm_and_ps(once, x), _mm_andnot_ps(once, fast));
  fast = _mm_or_ps(_mm_and_ps(twice, _mm_mul_ps(x, x)), _mm_andnot_ps(twice, fast));
  // Past either end, 0 or the infinity, as for e^x.
  const __m128 none = _mm_and_ps(ok0, _mm_cmplt_ps(power, _mm_set1_ps(-105.f)));
  const __m128 all = _mm_and_ps(ok0, _mm_cmpgt_ps(power, _mm_set1_ps(89.f)));
  fast = _mm_or_ps(_mm_andnot_ps(_mm_or_ps(none, all), fast), _mm_and_ps(all, _mm_set1_ps(INFINITY)));
  const int lanes = _mm_movemask_ps(_mm_cmpeq_ps(_mm_or_ps(ok, _mm_or_ps(none, all)), _mm_setzero_ps()));
  if (!lanes) return fast;
  alignas(16) float out[4], a[4], b[4];
  _mm_store_ps(out, fast);
  _mm_store_ps(a, x);
  _mm_store_ps(b, y);
  for (int k = 0; k < 4; ++k)
    if (lanes >> k & 1) out[k] = std::pow(a[k], b[k]);
  return _mm_load_ps(out);
}
// sin x and cos x for |x| up to 8192: x reduced by quarter turns, in three
// parts so the reduction itself loses nothing there.
inline __m128 sinCos(__m128 x, bool cosine) {
  const __m128 sign = _mm_set1_ps(-0.f);
  __m128 a = _mm_andnot_ps(sign, x);
  __m128i j = _mm_cvttps_epi32(_mm_mul_ps(a, _mm_set1_ps(1.27323954473516f))); // 4 / pi
  j = _mm_and_si128(_mm_add_epi32(j, _mm_set1_epi32(1)), _mm_set1_epi32(~1));
  const __m128 n = _mm_cvtepi32_ps(j);
  __m128 flip;
  if (cosine) {
    j = _mm_sub_epi32(j, _mm_set1_epi32(2));
    flip = _mm_castsi128_ps(_mm_slli_epi32(_mm_andnot_si128(j, _mm_set1_epi32(4)), 29));
  } else {
    flip = _mm_xor_ps(_mm_and_ps(x, sign), _mm_castsi128_ps(_mm_slli_epi32(_mm_and_si128(j, _mm_set1_epi32(4)), 29)));
  }
  const __m128 sinPart = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(j, _mm_set1_epi32(2)), _mm_setzero_si128()));
  a = _mm_sub_ps(a, _mm_mul_ps(n, _mm_set1_ps(0.78515625f)));
  a = _mm_sub_ps(a, _mm_mul_ps(n, _mm_set1_ps(2.4187564849853515625e-4f)));
  a = _mm_sub_ps(a, _mm_mul_ps(n, _mm_set1_ps(3.77489497744594108e-8f)));
  const __m128 z = _mm_mul_ps(a, a);
  __m128 c = polynomial(z, {2.443315711809948e-5f, -1.388731625493765e-3f, 4.166664568298827e-2f});
  c = _mm_add_ps(_mm_sub_ps(_mm_mul_ps(_mm_mul_ps(c, z), z), _mm_mul_ps(z, _mm_set1_ps(.5f))), _mm_set1_ps(1.f));
  __m128 s = polynomial(z, {-1.9515295891e-4f, 8.3321608736e-3f, -1.6666654611e-1f});
  s = _mm_add_ps(_mm_mul_ps(_mm_mul_ps(s, z), a), a);
  const __m128 r = _mm_or_ps(_mm_and_ps(sinPart, s), _mm_andnot_ps(sinPart, c));
  return _mm_xor_ps(r, flip);
}
inline __m128 sin(__m128 x) {
  const __m128 bad = _mm_cmpnle_ps(_mm_andnot_ps(_mm_set1_ps(-0.f), x), _mm_set1_ps(8192.f)); // NaN too
  return mend(sinCos(_mm_andnot_ps(bad, x), false), bad, x, [](float v) { return std::sin(v); });
}
inline __m128 cos(__m128 x) {
  const __m128 bad = _mm_cmpnle_ps(_mm_andnot_ps(_mm_set1_ps(-0.f), x), _mm_set1_ps(8192.f));
  return mend(sinCos(_mm_andnot_ps(bad, x), true), bad, x, [](float v) { return std::cos(v); });
}
// atan x for finite x: by eighths of a turn, then a short polynomial.
inline __m128 atanFast(__m128 x) {
  const __m128 sign = _mm_set1_ps(-0.f);
  __m128 a = _mm_andnot_ps(sign, x);
  const __m128 far = _mm_cmpgt_ps(a, _mm_set1_ps(2.414213562373095f)), mid = _mm_andnot_ps(far, _mm_cmpgt_ps(a, _mm_set1_ps(0.4142135623730950f)));
  const __m128 base = _mm_or_ps(_mm_and_ps(far, _mm_set1_ps(1.5707963267948966f)), _mm_and_ps(mid, _mm_set1_ps(0.7853981633974483f)));
  const __m128 farX = _mm_div_ps(_mm_set1_ps(-1.f), a), midX = _mm_div_ps(_mm_sub_ps(a, _mm_set1_ps(1.f)), _mm_add_ps(a, _mm_set1_ps(1.f)));
  a = _mm_or_ps(_mm_and_ps(far, farX), _mm_or_ps(_mm_and_ps(mid, midX), _mm_andnot_ps(_mm_or_ps(far, mid), a)));
  const __m128 z = _mm_mul_ps(a, a);
  const __m128 p = polynomial(z, {8.05374449538e-2f, -1.38776856032e-1f, 1.99777106478e-1f, -3.33329491539e-1f});
  const __m128 r = _mm_add_ps(base, _mm_add_ps(_mm_mul_ps(_mm_mul_ps(p, z), a), a));
  return _mm_or_ps(r, _mm_and_ps(x, sign));
}
inline __m128 atan(__m128 x) {
  const __m128 bad = _mm_cmpnlt_ps(_mm_andnot_ps(_mm_set1_ps(-0.f), x), _mm_set1_ps(3.40282347e+38f)); // inf, NaN
  return mend(atanFast(_mm_andnot_ps(bad, x)), bad, x, [](float v) { return std::atan(v); });
}
// atan2(y, x) for both finite and neither 0; the rest -- the axes, the
// infinities, NaN -- by the library, signed zeros and all.
inline __m128 atan2(__m128 y, __m128 x) {
  const __m128 sign = _mm_set1_ps(-0.f), huge = _mm_set1_ps(3.40282347e+38f), zero = _mm_setzero_ps();
  const __m128 ok = _mm_and_ps(_mm_and_ps(_mm_cmple_ps(_mm_andnot_ps(sign, x), huge), _mm_cmple_ps(_mm_andnot_ps(sign, y), huge)),
                               _mm_and_ps(_mm_cmpneq_ps(x, zero), _mm_cmpneq_ps(y, zero)));
  const __m128 sx = _mm_or_ps(_mm_and_ps(ok, x), _mm_andnot_ps(ok, _mm_set1_ps(1.f)));
  __m128 r = atanFast(_mm_div_ps(_mm_and_ps(ok, y), sx));
  // Left of the y axis: half a turn on, towards y's side.
  const __m128 turn = _mm_or_ps(_mm_set1_ps(3.14159265358979f), _mm_and_ps(y, sign));
  r = _mm_add_ps(r, _mm_and_ps(_mm_cmplt_ps(x, zero), turn));
  const int lanes = _mm_movemask_ps(_mm_cmpeq_ps(ok, zero));
  if (!lanes) return r;
  alignas(16) float out[4], a[4], b[4];
  _mm_store_ps(out, r);
  _mm_store_ps(a, y);
  _mm_store_ps(b, x);
  for (int k = 0; k < 4; ++k)
    if (lanes >> k & 1) out[k] = std::atan2(a[k], b[k]);
  return _mm_load_ps(out);
}
// asin x for |x| up to 1.
inline __m128 asin(__m128 x) {
  const __m128 sign = _mm_set1_ps(-0.f);
  const __m128 a = _mm_andnot_ps(sign, x);
  const __m128 bad = _mm_cmpnle_ps(a, _mm_set1_ps(1.f)); // NaN too
  const __m128 high = _mm_cmpgt_ps(a, _mm_set1_ps(.5f));
  const __m128 z = _mm_or_ps(_mm_and_ps(high, _mm_mul_ps(_mm_set1_ps(.5f), _mm_sub_ps(_mm_set1_ps(1.f), a))), _mm_andnot_ps(high, _mm_mul_ps(a, a)));
  const __m128 v = _mm_or_ps(_mm_and_ps(high, _mm_sqrt_ps(_mm_max_ps(z, _mm_setzero_ps()))), _mm_andnot_ps(high, a));
  const __m128 p = polynomial(z, {4.2163199048e-2f, 2.4181311049e-2f, 4.5470025998e-2f, 7.4953002686e-2f, 1.6666752422e-1f});
  __m128 r = _mm_add_ps(_mm_mul_ps(_mm_mul_ps(p, z), v), v);
  r = _mm_or_ps(_mm_and_ps(high, _mm_sub_ps(_mm_set1_ps(1.5707963267948966f), _mm_add_ps(r, r))), _mm_andnot_ps(high, r));
  r = _mm_or_ps(r, _mm_and_ps(x, sign));
  return mend(r, bad, x, [](float v2) { return std::asin(v2); });
}
// Whether every lane holds the same number: a shift by a constant, mostly.
inline bool uniform(const int32_t *v) {
  const __m128i first = _mm_set1_epi32(v[0]);
  __m128i same = _mm_set1_epi32(-1);
  for (int l = 0; l < B; l += 4) same = _mm_and_si128(same, _mm_cmpeq_epi32(ldi(v + l), first));
  return _mm_movemask_epi8(same) == 0xFFFF;
}
} // namespace sse
#endif

class Machine {
public:
  const Program *program = nullptr;
  std::vector<float> file; // registers, B floats each
  std::vector<Mask> loopBroken, loopContinued;
  Mask discarded = 0;
  Mask full = ~Mask(0); // the lanes holding pixels; the rest are worked on and thrown away
  uint64_t run = ~uint64_t(0);
  // What the instructions read.
  const Inputs *inputs = nullptr;
  float host[H_COUNT] = {};
  struct Picture {
    const float *pixels = nullptr; // straight RGBA32F
    qsizetype stride = 0;          // in floats
    int width = 0, height = 0;     // what is there
    int originX = 0, originY = 0;  // where that lies in the raster
    int rasterWidth = 0, rasterHeight = 0;
  } pictures[2];

  void take(const Program &p) {
    program = &p;
    file.assign(size_t(p.regs) * B + 16, 0.f);
    for (const auto &[reg, bits] : p.constants) {
      float *r = f(reg);
      const float value = bitsf(bits);
      for (int l = 0; l < B; ++l) r[l] = value;
    }
    loopBroken.assign(size_t(std::max(1, p.loops)), Mask(0));
    loopContinued.assign(size_t(std::max(1, p.loops)), Mask(0));
    run = ~uint64_t(0);
  }
  float *f(int r) { return file.data() + size_t(r) * B; }
  int32_t *i(int r) { return reinterpret_cast<int32_t *>(file.data() + size_t(r) * B); }
  uint32_t *u(int r) { return reinterpret_cast<uint32_t *>(file.data() + size_t(r) * B); }

  void prologue() {
    const Mask was = full;
    full = ~Mask(0);
    execute(program->prologue, ~Mask(0));
    full = was;
  }

  // Runs one block for the lanes in `mask`; returns the lanes still going
  // after it (not returned, broken, continued or discarded).
  Mask block(const Block &b, Mask cur) {
    for (const Node &node : b.nodes) {
      if (!cur) break;
      switch (node.t) {
      case Node::Code: execute(node.code, cur); break;
      case Node::If: {
        const Mask c = bitsOf(i(node.cond));
        const Mask then = cur & c, otherwise = cur & ~c;
        Mask out = 0;
        if (then) out |= block(*node.a, then);
        if (otherwise) out |= block(*node.b, otherwise);
        cur = out;
        break;
      }
      case Node::Loop: cur = loop(node, cur); break;
      case Node::Call:
        // Lanes that returned come back after the body; discarded ones do not.
        block(*node.a, cur);
        cur &= ~discarded;
        break;
      case Node::Break: loopBroken[size_t(node.loop)] |= cur; cur = 0; break;
      case Node::Continue: loopContinued[size_t(node.loop)] |= cur; cur = 0; break;
      case Node::Return: cur = 0; break;
      case Node::Discard: discarded |= cur; cur = 0; break;
      }
    }
    return cur;
  }

  Mask loop(const Node &node, Mask cur) {
    Mask active = cur, exited = 0;
    Mask &broken = loopBroken[size_t(node.loop)];
    Mask &continued = loopContinued[size_t(node.loop)];
    broken = 0;
    // A loop that never ends would hang the card; here it is cut off.
    constexpr int kMostTurns = 1 << 18;
    int turns = 0;
    const auto test = [&] {
      block(*node.a, active);
      if (node.condReg < 0) return;
      const Mask go = active & bitsOf(i(node.condReg));
      exited |= active & ~go;
      active = go;
    };
    for (;;) {
      if (node.condFirst) test();
      if (!active) break;
      continued = 0;
      active = block(*node.b, active) | continued;
      if (!active) break;
      block(*node.c, active);
      if (!node.condFirst) test();
      if (!active) break;
      if (++turns >= kMostTurns) {
        exited |= active;
        break;
      }
    }
    return cur & (exited | broken) & ~discarded;
  }

  // Every instruction works on all the lanes; only stores into variables
  // heed the mask, and a store to every lane holding a pixel is a copy.
  void execute(const std::vector<Ins> &list, Mask mask) {
    const bool whole = (mask & full) == full;
    for (const Ins &in : list) step(in, mask, whole);
  }

#define LANES1(expr)                                                                                                  \
  {                                                                                                                   \
    float *d = f(in.d);                                                                                               \
    const float *a = f(in.a);                                                                                         \
    for (int l = 0; l < B; ++l) d[l] = (expr);                                                                        \
  }
#define LANES2(expr)                                                                                                  \
  {                                                                                                                   \
    float *d = f(in.d);                                                                                               \
    const float *a = f(in.a), *b = f(in.b);                                                                           \
    for (int l = 0; l < B; ++l) d[l] = (expr);                                                                        \
  }
#define LANES3(expr)                                                                                                  \
  {                                                                                                                   \
    float *d = f(in.d);                                                                                               \
    const float *a = f(in.a), *b = f(in.b), *c = f(in.c);                                                             \
    for (int l = 0; l < B; ++l) d[l] = (expr);                                                                        \
  }
#define CMP2(expr)                                                                                                    \
  {                                                                                                                   \
    int32_t *d = i(in.d);                                                                                             \
    const float *a = f(in.a), *b = f(in.b);                                                                           \
    for (int l = 0; l < B; ++l) d[l] = (expr) ? 1 : 0;                                                                \
  }
#define INT1(expr)                                                                                                    \
  {                                                                                                                   \
    int32_t *d = i(in.d);                                                                                             \
    const int32_t *a = i(in.a);                                                                                       \
    for (int l = 0; l < B; ++l) d[l] = (expr);                                                                        \
  }
#define INT2(expr)                                                                                                    \
  {                                                                                                                   \
    int32_t *d = i(in.d);                                                                                             \
    const int32_t *a = i(in.a), *b = i(in.b);                                                                         \
    for (int l = 0; l < B; ++l) d[l] = (expr);                                                                        \
  }
#define INT3(expr)                                                                                                    \
  {                                                                                                                   \
    int32_t *d = i(in.d);                                                                                             \
    const int32_t *a = i(in.a), *b = i(in.b), *c = i(in.c);                                                           \
    for (int l = 0; l < B; ++l) d[l] = (expr);                                                                        \
  }
#define UINT2(expr)                                                                                                   \
  {                                                                                                                   \
    uint32_t *d = u(in.d);                                                                                            \
    const uint32_t *a = u(in.a), *b = u(in.b);                                                                        \
    for (int l = 0; l < B; ++l) d[l] = (expr);                                                                        \
  }
#define UINT3(expr)                                                                                                   \
  {                                                                                                                   \
    uint32_t *d = u(in.d);                                                                                            \
    const uint32_t *a = u(in.a), *b = u(in.b), *c = u(in.c);                                                          \
    for (int l = 0; l < B; ++l) d[l] = (expr);                                                                        \
  }

  void step(const Ins &in, Mask mask, bool whole) {
    switch (in.op) {
    case MOV: {
      if (in.d == in.a) break;
      std::memcpy(f(in.d), f(in.a), sizeof(float) * B);
      break;
    }
    case STORE: {
      if (in.d == in.a) break;
      if (whole) {
        std::memcpy(f(in.d), f(in.a), sizeof(float) * B);
        break;
      }
      float *d = f(in.d);
      const float *a = f(in.a);
#if defined(_M_X64) || defined(__SSE2__)
      // Four lanes at a time, each group copied whole, left, or blended.
      static const __m128i lanes[16] = {
          _mm_setr_epi32(0, 0, 0, 0),    _mm_setr_epi32(-1, 0, 0, 0),    _mm_setr_epi32(0, -1, 0, 0),
          _mm_setr_epi32(-1, -1, 0, 0),  _mm_setr_epi32(0, 0, -1, 0),    _mm_setr_epi32(-1, 0, -1, 0),
          _mm_setr_epi32(0, -1, -1, 0),  _mm_setr_epi32(-1, -1, -1, 0),  _mm_setr_epi32(0, 0, 0, -1),
          _mm_setr_epi32(-1, 0, 0, -1),  _mm_setr_epi32(0, -1, 0, -1),   _mm_setr_epi32(-1, -1, 0, -1),
          _mm_setr_epi32(0, 0, -1, -1),  _mm_setr_epi32(-1, 0, -1, -1),  _mm_setr_epi32(0, -1, -1, -1),
          _mm_setr_epi32(-1, -1, -1, -1)};
      for (int g = 0; g < B / 4; ++g) {
        const unsigned bits = unsigned(mask >> (g * 4)) & 15u;
        if (!bits) continue;
        const __m128 value = _mm_loadu_ps(a + g * 4);
        if (bits == 15u) {
          _mm_storeu_ps(d + g * 4, value);
          continue;
        }
        const __m128 keep = _mm_castsi128_ps(lanes[bits]);
        _mm_storeu_ps(d + g * 4, _mm_or_ps(_mm_and_ps(keep, value), _mm_andnot_ps(keep, _mm_loadu_ps(d + g * 4))));
      }
#else
      for (int l = 0; l < B; ++l)
        if ((mask >> l) & 1) d[l] = a[l];
#endif
      break;
    }
    case SEL: {
      uint32_t *d = u(in.d);
      const int32_t *c = i(in.a);
      const uint32_t *x = u(in.b), *y = u(in.c);
#ifdef WAVY_SSE2
      const __m128i zero = _mm_setzero_si128();
      for (int l = 0; l < B; l += 4)
        sse::sti(d + l, sse::pick(_mm_cmpeq_epi32(sse::ldi(c + l), zero), sse::ldi(y + l), sse::ldi(x + l)));
#else
      for (int l = 0; l < B; ++l) d[l] = c[l] ? x[l] : y[l];
#endif
      break;
    }
    case FADD: LANES2(a[l] + b[l]) break;
    case FSUB: LANES2(a[l] - b[l]) break;
    case FMUL: LANES2(a[l] * b[l]) break;
    case FDIV: LANES2(a[l] / b[l]) break;
    case FNEG: LANES1(-a[l]) break;
#ifdef WAVY_SSE2
    // std::min(a, b) is (b < a) ? b : a, which is minps with b first -- NaN
    // and signed zero alike; std::max(a, b) is (a < b) ? b : a, maxps so.
    case FMIN: {
      float *d = f(in.d);
      const float *a = f(in.a), *b = f(in.b);
      for (int l = 0; l < B; l += 4) sse::st(d + l, _mm_min_ps(sse::ld(b + l), sse::ld(a + l)));
      break;
    }
    case FMAX: {
      float *d = f(in.d);
      const float *a = f(in.a), *b = f(in.b);
      for (int l = 0; l < B; l += 4) sse::st(d + l, _mm_max_ps(sse::ld(b + l), sse::ld(a + l)));
      break;
    }
#else
    case FMIN: LANES2(std::min(a[l], b[l])) break;
    case FMAX: LANES2(std::max(a[l], b[l])) break;
#endif
    case FABS: LANES1(std::fabs(a[l])) break;
    case FSIGN: LANES1(glslSign(a[l])) break;
#ifdef WAVY_SSE2
    case FFLOOR: {
      float *d = f(in.d);
      const float *a = f(in.a);
      for (int l = 0; l < B; l += 4) sse::st(d + l, sse::floor(sse::ld(a + l)));
      break;
    }
#else
    case FFLOOR: LANES1(std::floor(a[l])) break;
#endif
    case FCEIL: LANES1(std::ceil(a[l])) break;
#ifdef WAVY_SSE2
    case FFRACT: {
      float *d = f(in.d);
      const float *a = f(in.a);
      for (int l = 0; l < B; l += 4) {
        const __m128 x = sse::ld(a + l);
        sse::st(d + l, _mm_sub_ps(x, sse::floor(x)));
      }
      break;
    }
#else
    case FFRACT: LANES1(glslFract(a[l])) break;
#endif
    case FTRUNC: LANES1(std::trunc(a[l])) break;
    case FROUND: LANES1(glslRound(a[l])) break;
    case FROUNDEVEN: LANES1(glslRoundEven(a[l])) break;
    case FSQRT: LANES1(std::sqrt(a[l])) break;
    case FINVSQRT: LANES1(glslInvSqrt(a[l])) break;
#if defined(WAVY_SSE2) && !defined(WAVY_LIBRARY_MATH)
    case FSIN: {
      float *d = f(in.d);
      const float *a = f(in.a);
      for (int l = 0; l < B; l += 4) sse::st(d + l, sse::sin(sse::ld(a + l)));
      break;
    }
#else
    case FSIN: LANES1(std::sin(a[l])) break;
#endif
#if defined(WAVY_SSE2) && !defined(WAVY_LIBRARY_MATH)
    case FCOS: {
      float *d = f(in.d);
      const float *a = f(in.a);
      for (int l = 0; l < B; l += 4) sse::st(d + l, sse::cos(sse::ld(a + l)));
      break;
    }
#else
    case FCOS: LANES1(std::cos(a[l])) break;
#endif
    case FTAN: LANES1(std::tan(a[l])) break;
#if defined(WAVY_SSE2) && !defined(WAVY_LIBRARY_MATH)
    case FASIN: {
      float *d = f(in.d);
      const float *a = f(in.a);
      for (int l = 0; l < B; l += 4) sse::st(d + l, sse::asin(sse::ld(a + l)));
      break;
    }
#else
    case FASIN: LANES1(std::asin(a[l])) break;
#endif
    case FACOS: LANES1(std::acos(a[l])) break;
#if defined(WAVY_SSE2) && !defined(WAVY_LIBRARY_MATH)
    case FATAN: {
      float *d = f(in.d);
      const float *a = f(in.a);
      for (int l = 0; l < B; l += 4) sse::st(d + l, sse::atan(sse::ld(a + l)));
      break;
    }
#else
    case FATAN: LANES1(std::atan(a[l])) break;
#endif
#if defined(WAVY_SSE2) && !defined(WAVY_LIBRARY_MATH)
    case FATAN2: {
      float *d = f(in.d);
      const float *a = f(in.a), *b = f(in.b);
      for (int l = 0; l < B; l += 4) sse::st(d + l, sse::atan2(sse::ld(a + l), sse::ld(b + l)));
      break;
    }
#else
    case FATAN2: LANES2(std::atan2(a[l], b[l])) break;
#endif
    case FSINH: LANES1(std::sinh(a[l])) break;
    case FCOSH: LANES1(std::cosh(a[l])) break;
    case FTANH: LANES1(std::tanh(a[l])) break;
    case FASINH: LANES1(std::asinh(a[l])) break;
    case FACOSH: LANES1(std::acosh(a[l])) break;
    case FATANH: LANES1(std::atanh(a[l])) break;
#if defined(WAVY_SSE2) && !defined(WAVY_LIBRARY_MATH)
    case FEXP: {
      float *d = f(in.d);
      const float *a = f(in.a);
      for (int l = 0; l < B; l += 4) sse::st(d + l, sse::exp(sse::ld(a + l)));
      break;
    }
#else
    case FEXP: LANES1(std::exp(a[l])) break;
#endif
#if defined(WAVY_SSE2) && !defined(WAVY_LIBRARY_MATH)
    case FLOG: {
      float *d = f(in.d);
      const float *a = f(in.a);
      for (int l = 0; l < B; l += 4) sse::st(d + l, sse::log(sse::ld(a + l)));
      break;
    }
#else
    case FLOG: LANES1(std::log(a[l])) break;
#endif
    case FEXP2: LANES1(std::exp2(a[l])) break;
    case FLOG2: LANES1(std::log2(a[l])) break;
#if defined(WAVY_SSE2) && !defined(WAVY_LIBRARY_MATH)
    case FPOW: {
      float *d = f(in.d);
      const float *a = f(in.a), *b = f(in.b);
      for (int l = 0; l < B; l += 4) sse::st(d + l, sse::pow(sse::ld(a + l), sse::ld(b + l)));
      break;
    }
#else
    case FPOW: LANES2(std::pow(a[l], b[l])) break;
#endif
    case FMOD: LANES2(glslMod(a[l], b[l])) break;
    case FSTEP: LANES2(glslStep(a[l], b[l])) break;
    case FMIX: LANES3(glslMix(a[l], b[l], c[l])) break;
#ifdef WAVY_SSE2
    case FCLAMP: {
      // std::min(std::max(x, lo), hi), as above.
      float *d = f(in.d);
      const float *a = f(in.a), *b = f(in.b), *c = f(in.c);
      for (int l = 0; l < B; l += 4)
        sse::st(d + l, _mm_min_ps(sse::ld(c + l), _mm_max_ps(sse::ld(b + l), sse::ld(a + l))));
      break;
    }
#else
    case FCLAMP: LANES3(glslClamp(a[l], b[l], c[l])) break;
#endif
    case FSMOOTH: LANES3(glslSmooth(a[l], b[l], c[l])) break;
    case FISNAN: {
      int32_t *d = i(in.d);
      const float *a = f(in.a);
      for (int l = 0; l < B; ++l) d[l] = std::isnan(a[l]) ? 1 : 0;
      break;
    }
    case FISINF: {
      int32_t *d = i(in.d);
      const float *a = f(in.a);
      for (int l = 0; l < B; ++l) d[l] = std::isinf(a[l]) ? 1 : 0;
      break;
    }
    case FLT: CMP2(a[l] < b[l]) break;
    case FLE: CMP2(a[l] <= b[l]) break;
    case FGT: CMP2(a[l] > b[l]) break;
    case FGE: CMP2(a[l] >= b[l]) break;
    case FEQ: CMP2(a[l] == b[l]) break;
    case FNE: CMP2(a[l] != b[l]) break;
    case IADD: UINT2(a[l] + b[l]) break;
    case ISUB: UINT2(a[l] - b[l]) break;
    case IMUL: UINT2(a[l] * b[l]) break;
    case IDIV: INT2(idiv(a[l], b[l])) break;
    case IMOD: INT2(imod(a[l], b[l])) break;
    case INEG: {
      uint32_t *d = u(in.d);
      const uint32_t *a = u(in.a);
      for (int l = 0; l < B; ++l) d[l] = 0u - a[l];
      break;
    }
    case IABS: {
      uint32_t *d = u(in.d);
      const int32_t *a = i(in.a);
      for (int l = 0; l < B; ++l) d[l] = a[l] < 0 ? 0u - uint32_t(a[l]) : uint32_t(a[l]);
      break;
    }
    case ISIGN: INT1(a[l] > 0 ? 1 : a[l] < 0 ? -1 : 0) break;
#ifdef WAVY_SSE2
    case IMIN: {
      int32_t *d = i(in.d);
      const int32_t *a = i(in.a), *b = i(in.b);
      for (int l = 0; l < B; l += 4) sse::sti(d + l, sse::imin(sse::ldi(a + l), sse::ldi(b + l)));
      break;
    }
    case IMAX: {
      int32_t *d = i(in.d);
      const int32_t *a = i(in.a), *b = i(in.b);
      for (int l = 0; l < B; l += 4) sse::sti(d + l, sse::imax(sse::ldi(a + l), sse::ldi(b + l)));
      break;
    }
    case ICLAMP: {
      int32_t *d = i(in.d);
      const int32_t *a = i(in.a), *b = i(in.b), *c = i(in.c);
      for (int l = 0; l < B; l += 4) sse::sti(d + l, sse::imin(sse::imax(sse::ldi(a + l), sse::ldi(b + l)), sse::ldi(c + l)));
      break;
    }
#else
    case IMIN: INT2(std::min(a[l], b[l])) break;
    case IMAX: INT2(std::max(a[l], b[l])) break;
    case ICLAMP: INT3(std::min(std::max(a[l], b[l]), c[l])) break;
#endif
    case UDIV: UINT2(udiv(a[l], b[l])) break;
    case UMOD: UINT2(umod(a[l], b[l])) break;
    case UMIN: UINT2(std::min(a[l], b[l])) break;
    case UMAX: UINT2(std::max(a[l], b[l])) break;
    case UCLAMP: UINT3(std::min(std::max(a[l], b[l]), c[l])) break;
    case IAND: UINT2(a[l] & b[l]) break;
    case IOR: UINT2(a[l] | b[l]) break;
    case IXOR: UINT2(a[l] ^ b[l]) break;
    case INOT: {
      uint32_t *d = u(in.d);
      const uint32_t *a = u(in.a);
      for (int l = 0; l < B; ++l) d[l] = ~a[l];
      break;
    }
    // By the same count in every lane -- a constant, mostly -- the vector
    // shifts, which give 0 (and the sign, shifted right) from 32 up, as these do.
    case ISHL: {
#ifdef WAVY_SSE2
      if (sse::uniform(i(in.b))) {
        int32_t *d = i(in.d);
        const int32_t *a = i(in.a);
        const __m128i count = _mm_cvtsi32_si128(i(in.b)[0]);
        for (int l = 0; l < B; l += 4) sse::sti(d + l, _mm_sll_epi32(sse::ldi(a + l), count));
        break;
      }
#endif
      INT2(ishl(a[l], b[l]))
      break;
    }
    case ISHR: {
#ifdef WAVY_SSE2
      if (sse::uniform(i(in.b))) {
        int32_t *d = i(in.d);
        const int32_t *a = i(in.a);
        const __m128i count = _mm_cvtsi32_si128(i(in.b)[0]);
        for (int l = 0; l < B; l += 4) sse::sti(d + l, _mm_sra_epi32(sse::ldi(a + l), count));
        break;
      }
#endif
      INT2(ishr(a[l], b[l]))
      break;
    }
    case USHR: {
#ifdef WAVY_SSE2
      if (sse::uniform(i(in.b))) {
        int32_t *d = i(in.d);
        const int32_t *a = i(in.a);
        const __m128i count = _mm_cvtsi32_si128(i(in.b)[0]);
        for (int l = 0; l < B; l += 4) sse::sti(d + l, _mm_srl_epi32(sse::ldi(a + l), count));
        break;
      }
#endif
      UINT2(ushr(a[l], b[l]))
      break;
    }
    case ILT: INT2(a[l] < b[l] ? 1 : 0) break;
    case ILE: INT2(a[l] <= b[l] ? 1 : 0) break;
    case IGT: INT2(a[l] > b[l] ? 1 : 0) break;
    case IGE: INT2(a[l] >= b[l] ? 1 : 0) break;
    case IEQ: INT2(a[l] == b[l] ? 1 : 0) break;
    case INE: INT2(a[l] != b[l] ? 1 : 0) break;
    case ULT: UINT2(a[l] < b[l] ? 1u : 0u) break;
    case ULE: UINT2(a[l] <= b[l] ? 1u : 0u) break;
    case UGT: UINT2(a[l] > b[l] ? 1u : 0u) break;
    case UGE: UINT2(a[l] >= b[l] ? 1u : 0u) break;
    case BNOT: INT1(a[l] ? 0 : 1) break;
    case I2F: {
      float *d = f(in.d);
      const int32_t *a = i(in.a);
      for (int l = 0; l < B; ++l) d[l] = float(a[l]);
      break;
    }
    case U2F: {
      float *d = f(in.d);
      const uint32_t *a = u(in.a);
      for (int l = 0; l < B; ++l) d[l] = float(a[l]);
      break;
    }
    case F2I: {
      int32_t *d = i(in.d);
      const float *a = f(in.a);
#ifdef WAVY_SSE2
      for (int l = 0; l < B; l += 4) sse::sti(d + l, sse::toInt(sse::ld(a + l)));
#else
      for (int l = 0; l < B; ++l) d[l] = floatToInt(a[l]);
#endif
      break;
    }
    case F2U: {
      uint32_t *d = u(in.d);
      const float *a = f(in.a);
      for (int l = 0; l < B; ++l) d[l] = floatToUInt(a[l]);
      break;
    }
    case F2B: {
      int32_t *d = i(in.d);
      const float *a = f(in.a);
      for (int l = 0; l < B; ++l) d[l] = a[l] != 0.f ? 1 : 0;
      break;
    }
    case I2B: INT1(a[l] != 0 ? 1 : 0) break;
    case GATHER: {
      uint32_t *d = u(in.d);
      const int32_t *idx = i(in.a);
      const int *list = program->lists.data() + in.e;
      const int count = in.c;
      for (int l = 0; l < B; ++l) {
        const int k = std::clamp(idx[l], 0, count - 1);
        d[l] = u(list[k])[l];
      }
      break;
    }
    case SCATTER: {
      const int32_t *idx = i(in.a);
      const uint32_t *v = u(in.b);
      const int *list = program->lists.data() + in.e;
      const int count = in.c;
      for (Mask left = mask; left; left &= left - 1) {
        const int l = lowestLane(left);
        const int k = std::clamp(idx[l], 0, count - 1);
        u(list[k])[l] = v[l];
      }
      break;
    }
    case UVALUE: {
      float *d = f(in.d);
      const float value = in.a < inputs->valueCount && inputs->values ? inputs->values[in.a] : 0.f;
      for (int l = 0; l < B; ++l) d[l] = value;
      break;
    }
    case UGATHER: {
      float *d = f(in.d);
      const int32_t *idx = i(in.a);
      const int count = std::min(in.b, inputs->values ? inputs->valueCount : 0);
      for (int l = 0; l < B; ++l) d[l] = idx[l] >= 0 && idx[l] < count ? inputs->values[idx[l]] : 0.f;
      break;
    }
    case HOST: {
      float *d = f(in.d);
      const float value = host[in.a];
      if (in.a == H_DEPTH || in.a == H_PASS || in.a == H_PASSES) {
        int32_t *di = i(in.d);
        for (int l = 0; l < B; ++l) di[l] = int32_t(value);
      } else {
        for (int l = 0; l < B; ++l) d[l] = value;
      }
      break;
    }
    case TEXEL: {
      const Picture &pic = pictures[in.c];
      const int32_t *x = i(in.a), *y = i(in.b);
      const int *list = program->lists.data() + in.e;
      float *r = f(list[0]), *g = f(list[1]), *bl = f(list[2]), *al = f(list[3]);
#ifdef WAVY_SSE2
      // Four pixels at a time, each read whole and turned into the four
      // channels' registers.
      if (pic.pixels) {
        for (int l = 0; l < B; l += 4) {
          __m128 p[4];
          for (int k = 0; k < 4; ++k) {
            const int px = x[l + k] - pic.originX, py = y[l + k] - pic.originY;
            p[k] = unsigned(px) < unsigned(pic.width) && unsigned(py) < unsigned(pic.height)
                       ? _mm_loadu_ps(pic.pixels + pic.stride * py + px * 4)
                       : _mm_setzero_ps();
          }
          _MM_TRANSPOSE4_PS(p[0], p[1], p[2], p[3]);
          sse::st(r + l, p[0]);
          sse::st(g + l, p[1]);
          sse::st(bl + l, p[2]);
          sse::st(al + l, p[3]);
        }
        break;
      }
#endif
      for (int l = 0; l < B; ++l) {
        const int px = x[l] - pic.originX, py = y[l] - pic.originY;
        if (px < 0 || py < 0 || px >= pic.width || py >= pic.height || !pic.pixels) {
          r[l] = g[l] = bl[l] = al[l] = 0.f;
          continue;
        }
        const float *s = pic.pixels + pic.stride * py + px * 4;
        r[l] = s[0];
        g[l] = s[1];
        bl[l] = s[2];
        al[l] = s[3];
      }
      break;
    }
    case TEXTURE: {
      // Normalized coordinates over the whole raster, filtered between the four
      // nearest texels and held to the edge -- GL_LINEAR, GL_CLAMP_TO_EDGE.
      const Picture &pic = pictures[in.c];
      const float *x = f(in.a), *y = f(in.b);
      const int *list = program->lists.data() + in.e;
      float *out[4] = {f(list[0]), f(list[1]), f(list[2]), f(list[3])};
      const int w = std::max(1, pic.rasterWidth), h = std::max(1, pic.rasterHeight);
      auto fetch = [&](int px, int py, int k) -> float {
        px = std::clamp(px, 0, w - 1) - pic.originX;
        py = std::clamp(py, 0, h - 1) - pic.originY;
        if (px < 0 || py < 0 || px >= pic.width || py >= pic.height || !pic.pixels) return 0.f;
        return pic.pixels[pic.stride * py + px * 4 + k];
      };
      for (int l = 0; l < B; ++l) {
        const float fx = x[l] * float(w) - .5f, fy = y[l] * float(h) - .5f;
        if (!(fx == fx) || !(fy == fy)) {
          for (int k = 0; k < 4; ++k) out[k][l] = 0.f;
          continue;
        }
        const float x0f = std::floor(std::clamp(fx, -1.f, float(w))), y0f = std::floor(std::clamp(fy, -1.f, float(h)));
        const int x0 = int(x0f), y0 = int(y0f);
        const float tx = std::clamp(fx, -1.f, float(w)) - x0f, ty = std::clamp(fy, -1.f, float(h)) - y0f;
        for (int k = 0; k < 4; ++k) {
          const float top = fetch(x0, y0, k) * (1.f - tx) + fetch(x0 + 1, y0, k) * tx;
          const float bottom = fetch(x0, y0 + 1, k) * (1.f - tx) + fetch(x0 + 1, y0 + 1, k) * tx;
          out[k][l] = top * (1.f - ty) + bottom * ty;
        }
      }
      break;
    }
    default: break;
    }
  }
#undef LANES1
#undef LANES2
#undef LANES3
#undef CMP2
#undef INT1
#undef INT2
#undef INT3
#undef UINT2
#undef UINT3
};

std::atomic<uint64_t> runs{0};

// A few programs' machines per thread: a chain of effects alternates programs
// strip by strip, and setting a machine up for a program costs its constants.
Machine &machineFor(const Program &p) {
  struct Held {
    uint64_t serial = 0;
    std::unique_ptr<Machine> machine;
    uint64_t used = 0;
  };
  thread_local std::array<Held, 4> held;
  thread_local uint64_t clock = 0;
  ++clock;
  for (Held &h : held)
    if (h.machine && h.serial == p.serial) {
      h.used = clock;
      return *h.machine;
    }
  Held *oldest = &held[0];
  for (Held &h : held)
    if (h.used < oldest->used) oldest = &h;
  if (!oldest->machine) oldest->machine = std::make_unique<Machine>();
  oldest->serial = p.serial;
  oldest->used = clock;
  oldest->machine->take(p);
  return *oldest->machine;
}

void setHost(Machine &m, const Inputs &in) {
  m.inputs = &in;
  m.host[H_SECONDS] = in.seconds;
  m.host[H_FRAME] = in.frame;
  m.host[H_SCALE] = in.scale;
  for (int k = 0; k < 4; ++k) m.host[H_BOX0 + k] = in.box[k];
  m.host[H_DEPTH] = float(in.depth);
  m.host[H_PASS] = float(in.pass);
  m.host[H_PASSES] = float(in.passes);
  m.host[H_SOURCE_W] = m.host[H_ORIGINAL_W] = float(in.rasterWidth);
  m.host[H_SOURCE_H] = m.host[H_ORIGINAL_H] = float(in.rasterHeight);
}

} // namespace

std::shared_ptr<const Program> compile(const QString &source, const QString &entry, QString *problem) {
  try {
    Preprocessor pre;
    std::vector<Token> tokens = pre.run(source.toStdString());
    Parser parser(std::move(tokens));
    Unit unit = parser.unit();
    Compiler compiler(unit);
    return compiler.compile(entry.toStdString());
  } catch (const CompileError &error) {
    if (problem) *problem = QString("line %1: %2").arg(error.line).arg(QString::fromStdString(error.what));
  } catch (const std::exception &error) {
    if (problem) *problem = QString("cannot compile: %1").arg(QString::fromUtf8(error.what()));
  }
  return nullptr;
}

bool run(const Program &p, const QImage &input, const QImage &original, QImage &output, const Inputs &in, QPoint origin,
         QSize raster) {
  // Four floats a pixel, read as they are, as the card reads a float texture.
  auto floats = [](const QImage &image) {
    return image.format() == QImage::Format_RGBA32FPx4 || image.format() == QImage::Format_RGBA32FPx4_Premultiplied;
  };
  if (!p.isMain || input.isNull() || !floats(input)) return false;
  if (output.size() != input.size() || !floats(output)) return false;
  const QImage &orig = original.isNull() ? input : original;
  if (!floats(orig)) return false;
  if (raster.isEmpty()) raster = input.size();
  const uint64_t thisRun = ++runs;
  const int width = input.width(), height = input.height();
  const float *source = reinterpret_cast<const float *>(input.constBits());
  const float *first = reinterpret_cast<const float *>(orig.constBits());
  float *target = reinterpret_cast<float *>(output.bits());
  const qsizetype sourceStride = input.bytesPerLine() / qsizetype(sizeof(float));
  const qsizetype firstStride = orig.bytesPerLine() / qsizetype(sizeof(float));
  const qsizetype targetStride = output.bytesPerLine() / qsizetype(sizeof(float));
  // The original covers the raster the same way the input does when it is the
  // same size; one of another size (a whole raster beside a strip) starts at 0.
  const QPoint originalOrigin = orig.size() == input.size() ? origin : QPoint();
  renderParallel(height, [&](int begin, int end) {
    Machine &m = machineFor(p);
    setHost(m, in);
    m.host[H_SOURCE_W] = float(raster.width());
    m.host[H_SOURCE_H] = float(raster.height());
    m.host[H_ORIGINAL_W] = float(orig.size() == input.size() ? raster.width() : orig.width());
    m.host[H_ORIGINAL_H] = float(orig.size() == input.size() ? raster.height() : orig.height());
    m.pictures[0] = Machine::Picture{source, sourceStride, width, height, origin.x(), origin.y(), raster.width(), raster.height()};
    m.pictures[1] = Machine::Picture{first, firstStride, orig.width(), orig.height(), originalOrigin.x(), originalOrigin.y(),
                                     int(m.host[H_ORIGINAL_W]), int(m.host[H_ORIGINAL_H])};
    if (m.run != thisRun) {
      m.prologue();
      m.run = thisRun;
    }
    float *fx = m.f(0), *fy = m.f(1), *fz = m.f(2), *fw = m.f(3);
    for (int l = 0; l < B; ++l) {
      fz[l] = .5f;
      fw[l] = 1.f;
    }
    const float *result[4] = {m.f(p.result[0]), m.f(p.result[1]), m.f(p.result[2]), m.f(p.result[3])};
    for (int y = begin; y < end; ++y) {
      float *row = target + targetStride * y;
      for (int x0 = 0; x0 < width; x0 += B) {
        const int valid = std::min(B, width - x0);
        for (int l = 0; l < B; ++l) {
          fx[l] = float(origin.x() + x0 + l) + .5f;
          fy[l] = float(origin.y() + y) + .5f;
        }
        m.full = valid == B ? ~Mask(0) : (Mask(1) << valid) - 1;
        m.discarded = 0;
        m.block(p.body, m.full);
        for (int l = 0; l < valid; ++l) {
          float *px = row + (x0 + l) * 4;
          if ((m.discarded >> l) & 1) {
            px[0] = px[1] = px[2] = px[3] = 0.f;
            continue;
          }
          px[0] = result[0][l];
          px[1] = result[1][l];
          px[2] = result[2][l];
          px[3] = result[3][l];
        }
      }
    }
  });
  return true;
}

double evaluate(const Program &p, const Inputs &in) {
  if (p.isMain || p.value < 0) return std::numeric_limits<double>::quiet_NaN();
  Machine &m = machineFor(p);
  setHost(m, in);
  m.run = 0; // this call's prologue, not any run's
  m.prologue();
  m.full = 1;
  m.discarded = 0;
  m.block(p.body, m.full);
  switch (p.valueKind) {
  case K::Float: return double(m.f(p.value)[0]);
  case K::Int: return double(m.i(p.value)[0]);
  case K::UInt: return double(m.u(p.value)[0]);
  case K::Bool: return m.i(p.value)[0] ? 1. : 0.;
  default: return std::numeric_limits<double>::quiet_NaN();
  }
}

int registerCount(const Program &p) { return p.regs; }
int instructionCount(const Program &p) { return p.instructions; }

// =============================================================================
// Checks
// =============================================================================
int check(QStringList &failures) {
  int checks = 0;
  const float sliders[8] = {1, 2, 3, 8, 0, 0, .25f, -1};
  Inputs base;
  base.values = sliders;
  base.valueCount = 8;
  const QString uniforms = "uniform float values[256];uniform float wavySeconds;uniform float wavyFrame;uniform float wavyScale;"
                           "uniform vec4 wavyBox;uniform int wavyPass;uniform int wavyPasses;uniform int processingDepth;\n";
  // A value: f() worked out once, from the sliders above.
  auto value = [&](const QString &what, const QString &source, double expected, double tolerance = 1e-6,
                   const Inputs &in = Inputs()) {
    ++checks;
    QString problem;
    const auto program = compile(uniforms + source, "f", &problem);
    if (!program) {
      failures << "shader on the processor, " + what + ": " + problem;
      return;
    }
    const double got = evaluate(*program, in.values ? in : base);
    if (!(std::abs(got - expected) <= tolerance * std::max(1., std::abs(expected))))
      failures << QString("shader on the processor, %1: %2, expected %3").arg(what).arg(got, 0, 'g', 10).arg(expected, 0, 'g', 10);
  };
  auto refused = [&](const QString &what, const QString &source, const QString &mention, const QString &entry = "f") {
    ++checks;
    QString problem;
    if (compile(uniforms + source, entry, &problem)) {
      failures << "shader on the processor, " + what + ": compiled, but should not have";
      return;
    }
    if (!problem.contains(mention)) failures << "shader on the processor, " + what + ": said \"" + problem + "\", not " + mention;
  };
  // A picture: main() over a raster whose pixel (x, y) is (x, y, x + y, 1),
  // seventy wide so a row is one full batch of lanes and part of another.
  using Expected = std::function<std::array<float, 4>(int, int)>;
  auto picture = [&](const QString &what, const QString &globals, const QString &body, const Expected &expected,
                     const Inputs &in = Inputs(), QPoint origin = {}, QSize raster = {}, const QImage &original = {}) {
    ++checks;
    QString problem;
    const QString source = "#version 330 core\n"
                           "uniform sampler2D src;uniform float values[256];uniform vec2 sourceSize;\n"
                           "uniform int wavyPass;uniform sampler2D wavyOriginalTexture;\n"
                           "out vec4 result;\n" +
                           globals + "\nvoid main(){\n" + body + "\n}\n";
    const auto program = compile(source, "main", &problem);
    if (!program) {
      failures << "shader on the processor, " + what + ": " + problem;
      return;
    }
    QImage input(70, 3, QImage::Format_RGBA32FPx4);
    for (int y = 0; y < input.height(); ++y) {
      float *row = reinterpret_cast<float *>(input.scanLine(y));
      for (int x = 0; x < input.width(); ++x) {
        row[x * 4] = float(x);
        row[x * 4 + 1] = float(y);
        row[x * 4 + 2] = float(x + y);
        row[x * 4 + 3] = 1;
      }
    }
    QImage output(input.size(), input.format());
    if (!run(*program, input, original, output, in.values ? in : base, origin, raster)) {
      failures << "shader on the processor, " + what + ": did not run";
      return;
    }
    for (int y = 0; y < output.height(); ++y) {
      const float *row = reinterpret_cast<const float *>(output.constScanLine(y));
      for (int x = 0; x < output.width(); ++x) {
        const std::array<float, 4> want = expected(x, y);
        for (int k = 0; k < 4; ++k)
          if (!(std::abs(row[x * 4 + k] - want[size_t(k)]) <= 1e-4f * std::max(1.f, std::abs(want[size_t(k)])))) {
            failures << QString("shader on the processor, %1: pixel %2,%3 channel %4 is %5, expected %6")
                            .arg(what).arg(x).arg(y).arg(k).arg(double(row[x * 4 + k])).arg(double(want[size_t(k)]));
            return;
          }
      }
    }
  };

  // ---- arithmetic, worked out per run (the sliders are not constants) ----
  value("precedence", "float f(){ return values[0] + values[1] * values[2] - values[3] / 4.0; }", 5);
  value("whole numbers", "float f(){ int a = int(values[2]) * 2 + 1; int b = int(values[1]);"
                         " return float(a / b * 100 + a % b * 10 + (a << 2) + (a >> 1)); }", 341);
  value("no trap on a division by zero",
        "float f(){ int a = int(values[2]); int z = int(values[4]); uint u = uint(values[2]); uint uz = uint(values[4]);"
        " return float(a / z != 0) + float(a % z == a) * 10.0 + float(u / uz == 4294967295u) * 100.0; }", 111);
  value("floats held to the whole numbers",
        "float f(){ float big = values[3] * 1e30; float notNumber = values[4] / values[4];"
        " return float(int(big) == 2147483647) + float(int(-big) == -2147483647 - 1) * 10.0"
        " + float(int(notNumber) == 0) * 100.0 + float(uint(-values[3]) == 0u) * 1000.0; }", 1111);
  value("uint wraps", "float f(){ uint u = uint(values[2]) * 2u + 1u; u -= 8u; return float(u == 4294967295u); }", 1);
  value("bits", "float f(){ int a = int(values[2]) * 2 + 1; return float((a & 3) | (a ^ 5) << 4) + float(~a) * 1000.0; }", -7965);
  value("swizzles", "float f(){ vec4 v = vec4(values[0], values[1], values[2], values[3]); v.xz = v.zx * 2.0;"
                    " return v.x + v.y * 10.0 + v.z * 100.0 + dot(v.xy, vec2(1.0)) * 1000.0 + v.w; }", 8234);
  value("matrices", "float f(){ mat2 m = mat2(values[0], values[1], values[2], values[3]); vec2 r = m * vec2(1.0, 10.0);"
                    " vec2 s = vec2(1.0, 10.0) * m; return r.x + r.y * 100.0 + s.x * 1e4 + determinant(m) * 1e6; }", 2218231);
  value("a matrix's columns", "float f(){ mat3 m = mat3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0) * values[0];"
                              " mat3 t = transpose(m); vec3 c = t[int(values[1])]; return t[0][1] + c.z * 10.0; }", 94);
  value("for, break and continue", "float f(){ float s = 0.0; for (int i = 0; i < 10; ++i) {"
                                   " if (i == int(values[1])) continue; if (i > 6) break; s += float(i); } return s; }", 19);
  value("while and do", "float f(){ int n = 0; float x = values[2]; while (x > 0.5) { x *= 0.5; ++n; }"
                        " do { n += 100; } while (n < 300); return float(n); }", 303);
  value("functions, out and inout, overloads",
        "void split(float v, out float whole, inout float count) { whole = floor(v); count += 1.0; }\n"
        "float pick(int k) { if (k < 0) return -1.0; if (k == 0) return 0.0; return 1.0; }\n"
        "float pick(float k) { return 2.0 * k; }\n"
        "float f(){ float w; float c = 10.0; split(values[2] + 0.5, w, c);"
        " return w * 100.0 + c + pick(int(values[0])) * 1000.0 + pick(values[0]) * 10000.0; }", 21311);
  value("arrays", "float f(){ float w[5]; for (int i = 0; i < 5; ++i) w[i] = float(i * i); int k = int(values[2]);"
                  " w[k] += 0.5; return w[k] + w[4] * 10.0 + float(w.length()) * 1000.0; }", 5169.5);
  value("constant arrays", "const float table[3] = float[3](1.0, 2.0, 4.0);\n"
                           "float f(){ return table[int(values[0])] + table[2] * 10.0; }", 42);
  value("the preprocessor", "#define SQUARE(x) ((x) * (x))\n#define TWICE(x) (2.0 * (x))\n#define LIMIT 3\n"
                            "#ifndef GUARD\n#define GUARD\n#endif\n"
                            "#if LIMIT > 2 && defined(SQUARE) && defined GUARD\n"
                            "float f(){ return SQUARE(TWICE(values[0] + 1.0)); }\n"
                            "#elif 1\nfloat f(){ return -1.0; }\n#else\nfloat f(){ return -2.0; }\n#endif\n", 16);
  value("#undef", "#define GONE 1\n#undef GONE\n#ifdef GONE\nfloat f(){ return -1.0; }\n#else\n"
                  "float f(){ return values[0] * 5.0; }\n#endif\n", 5);
  value("globals written by a function", "float total = 1.0;\nvoid add(float v) { total += v; }\n"
                                         "float f(){ add(values[0]); add(values[1]); return total; }", 4);
  value("?:, && and || run only the side they take",
        "float f(){ float a = 0.0, b = 0.0; bool c = values[0] > 0.5; float r = c ? (a += 1.0) : (b += 1.0);"
        " bool t = c || (b += 5.0) > 0.0; bool u = !c && (a += 7.0) > 0.0;"
        " return a * 100.0 + b * 10.0 + r + float(t) * 1000.0 + float(u) * 10000.0; }", 1101);
  value("arguments are worked out left to right",
        "float g(float x, float y) { return x * 10.0 + y; }\nfloat f(){ float x = values[0]; return g(x, x += 1.0); }", 12);
  value("built-ins", "float f(){ float x = values[0]; float s = 0.0; s += mod(-x, 3.0); s += fract(x * 2.75) * 10.0;"
                     " s += clamp(x * 5.0, 0.0, 3.0) * 100.0; s += mix(0.0, 10.0, x * 0.25) * 1000.0;"
                     " s += step(0.5, x) * 1e4; return s; }", 12809.5);
  value("more built-ins", "float f(){ float x = values[0]; float s = smoothstep(0.0, 4.0, x); s += pow(x * 2.0, 3.0);"
                          " s += exp2(x * 3.0) * 10.0; s += log2(x * 16.0) * 100.0;"
                          " s += length(cross(vec3(x, 0.0, 0.0), vec3(0.0, x * 2.0, 0.0))) * 1000.0; return s; }",
        2488.15625, 1e-6);
  value("geometry", "float f(){ float x = values[0]; vec2 r = reflect(vec2(x, -x), vec2(0.0, 1.0));"
                    " float a = atan(x, -values[1]); vec3 n = normalize(vec3(values[1], 0.0, 0.0));"
                    " return r.x + r.y * 10.0 + a * 100.0 + n.x * 1000.0 + float(isnan(values[4] / values[4])) * 10000.0; }",
        11278.79451, 1e-6);
  value("whole-number built-ins and rounding",
        "float f(){ int a = -int(values[2]); return float(abs(a) + sign(a) * 10 + min(a, 2) * 100 + max(a, 2) * 1000)"
        " + round(values[6] * 10.0 + 0.2) * 1e4 + floor(-values[6]) * 1e5 + ceil(values[6]) * 1e6; }", 931693);
  value("a loop that never ends is cut off", "float f(){ float s = 0.0; while (values[0] > 0.0) { s += 1.0; } return s; }", 262144);
  {
    Inputs when = base;
    when.seconds = 1.5;
    when.frame = 3;
    when.scale = 2;
    when.box[2] = 4;
    when.pass = 1;
    when.passes = 2;
    when.depth = 8;
    value("where and when", "float f(){ return wavySeconds * 10.0 + wavyFrame * 100.0 + wavyScale * 1000.0 + wavyBox.z * 1e4"
                            " + float(wavyPass + wavyPasses * 10 + processingDepth * 100); }", 43136, 1e-6, when);
  }
  value("a bool answer", "bool f(){ return values[0] == 1.0 && values[1] > 1.0; }", 1);

  // ---- what is refused, and where it says the trouble is ----
  refused("recursion", "float f(){ return f(); }", "itself");
  refused("switch", "float f(){ switch (1) { default: return 0.0; } }", "switch");
  refused("structs", "struct S { float a; };\nfloat f(){ return 0.0; }", "struct");
  refused("derivatives", "float f(){ return dFdx(values[0]); }", "dFdx");
  refused("an unknown function", "float f(){ return frobnicate(1.0); }", "unknown function");
  refused("a wrong type", "float f(){ vec3 a = vec2(1.0); return a.x; }", "cannot use");
  refused("a missing ';'", "float f(){ return 1.0 }", "expected");
  refused("a uniform the host does not give", "uniform float mySlider;\nfloat f(){ return mySlider; }", "not one Wavy-Flo provides");
  refused("an index past the end", "float f(){ float a[2]; return a[2]; }", "outside");
  refused("#error", "#error stop here\nfloat f(){ return 0.0; }", "#error");
  refused("a constant written", "const float k = 1.0;\nfloat f(){ k = 2.0; return k; }", "constant");
  refused("no entry", "float g(){ return 0.0; }", "no function called f");
  refused("an out argument that is no variable", "void g(out float a){ a = 1.0; }\nfloat f(){ g(1.0); return 0.0; }", "assigned");
  {
    ++checks;
    QString problem;
    compile("#line 1\nfloat f(){\n  return nothing;\n}\n", "f", &problem);
    if (!problem.startsWith("line 2:")) failures << "shader on the processor, #line numbers the lines: said \"" + problem + "\"";
  }

  // ---- pixels going different ways ----
  picture("an if per pixel", "",
          "ivec2 p = ivec2(gl_FragCoord.xy); float r;\n"
          "if (p.x < 10) r = 1.0; else if (p.x < 40) r = 2.0; else r = 3.0;\n"
          "float v = gl_FragCoord.x; if (p.x % 2 == 0) v = -v;\n"
          "result = vec4(r, float(p.y), v, 1.0);",
          [](int x, int y) {
            return std::array<float, 4>{x < 10 ? 1.f : x < 40 ? 2.f : 3.f, float(y), x % 2 == 0 ? -(x + .5f) : x + .5f, 1};
          });
  picture("loops running a different number of times per pixel", "",
          "int x = int(gl_FragCoord.x); float s = 0.0;\n"
          "for (int i = 0; i < 100; ++i) { if (i >= x) break; if (i % 3 == 0) continue; s += float(i); }\n"
          "int count = 0;\n"
          "for (int i = 0; i < 8; ++i) { if (i == x % 5) continue;\n"
          "  for (int j = 0; j < 8; ++j) { if (j > i) break; if ((i + j + x) % 2 == 0) continue; ++count; } }\n"
          "result = vec4(s, float(count), 0.0, 1.0);",
          [](int x, int) {
            float s = 0;
            for (int i = 0; i < 100 && i < x; ++i)
              if (i % 3) s += float(i);
            int count = 0;
            for (int i = 0; i < 8; ++i) {
              if (i == x % 5) continue;
              for (int j = 0; j <= i; ++j)
                if ((i + j + x) % 2) ++count;
            }
            return std::array<float, 4>{s, float(count), 0, 1};
          });
  picture("a return from inside a loop", "float root(int n) { int k = 0; while (true) { if (k * k >= n) return float(k); ++k; } return -1.0; }",
          "result = vec4(root(int(gl_FragCoord.x)), 0.0, 0.0, 1.0);", [](int x, int) {
            int k = 0;
            while (k * k < x) ++k;
            return std::array<float, 4>{float(k), 0, 0, 1};
          });
  picture("discard", "", "if (int(gl_FragCoord.x) % 4 == 1) discard;\nresult = vec4(1.0);", [](int x, int) {
    const float v = x % 4 == 1 ? 0.f : 1.f;
    return std::array<float, 4>{v, v, v, v};
  });
  picture("reading the picture", "",
          "ivec2 p = ivec2(gl_FragCoord.xy); vec4 here = texelFetch(src, p, 0);\n"
          "vec4 left = p.x > 0 ? texelFetch(src, p - ivec2(1, 0), 0) : vec4(-1.0);\n"
          "vec4 soft = texture(src, (gl_FragCoord.xy + vec2(0.5, 0.0)) / sourceSize);\n"
          "result = vec4(here.x + here.z * 1000.0, left.x, soft.x, float(textureSize(src, 0).x));",
          [](int x, int y) {
            return std::array<float, 4>{float(x + (x + y) * 1000), x > 0 ? float(x - 1) : -1.f, x < 69 ? x + .5f : 69.f, 70};
          });
  {
    Inputs doubled = base;
    const float two[1] = {2};
    doubled.values = two;
    doubled.valueCount = 1;
    picture("a branch every pixel takes the same way", "",
            "float k = values[0];\nif (k > 0.5) result = vec4(gl_FragCoord.x * k); else result = vec4(-1.0);",
            [](int x, int) {
              const float v = 2 * (x + .5f);
              return std::array<float, 4>{v, v, v, v};
            },
            doubled);
    Inputs second = base;
    second.pass = 1;
    picture("a strip of a larger raster", "",
            "vec4 above = texelFetch(src, ivec2(gl_FragCoord.x, 4.0), 0);\n"
            "result = vec4(gl_FragCoord.y, sourceSize.y, above.a, float(wavyPass));",
            [](int, int y) { return std::array<float, 4>{y + 5.5f, 20, 0, 1}; }, second, QPoint(0, 5), QSize(70, 20));
    QImage original(70, 3, QImage::Format_RGBA32FPx4);
    original.fill(QColor::fromRgbF(.25f, .25f, .25f, .25f));
    picture("the picture the effect began with", "",
            "result = texelFetch(wavyOriginalTexture, ivec2(gl_FragCoord.xy), 0) * 2.0;",
            [](int, int) { return std::array<float, 4>{.5f, .5f, .5f, .5f}; }, base, QPoint(), QSize(), original);
  }
#ifdef WAVY_SSE2
  // The functions worked out four lanes at a time, against the library's, over
  // the ranges a shader uses them in and past their edges: within `ulps` units
  // in the last place of the library's answer, give or take `near` -- for a
  // result close to 0, where a few units of so small a number are nothing
  // (sin near a whole number of half turns) -- or `relative`, for pow, whose
  // e^(y ln x) carries the rounding of ln x times y.
  {
    struct Sweep {
      const char *name;
      std::function<float(uint32_t)> input; // the k-th of `count` inputs
      int count;
      std::function<__m128(__m128)> vec;
      std::function<float(float)> lib;
      double ulps, near, relative;
    };
    auto spread = [](double lo, double hi, int count) {
      return [=](uint32_t k) { return float(lo + (hi - lo) * (k + .5) / count); };
    };
    // Every magnitude from 1e-30 to 1e30, both signs.
    auto magnitudes = [](uint32_t k) {
      const double e = -30 + 60 * ((k / 2) % 50000) / 50000.;
      return float((k & 1 ? -1 : 1) * std::pow(10., e));
    };
    auto unit = [](float v) { return std::nextafter(std::abs(v), INFINITY) - std::abs(v); };
    const std::vector<Sweep> sweeps{
        {"exp", spread(-110, 100, 200000), 200000, [](__m128 x) { return sse::exp(x); }, [](float v) { return std::exp(v); }, 4, 0, 0},
        {"log", [](uint32_t k) { return std::abs(float(std::pow(10., -40 + 79 * (k + .5) / 200000.))); }, 200000,
         [](__m128 x) { return sse::log(x); }, [](float v) { return std::log(v); }, 4, 2e-8, 0},
        {"log near 1", spread(.5, 2, 100000), 100000, [](__m128 x) { return sse::log(x); }, [](float v) { return std::log(v); }, 4, 2e-8, 0},
        {"log at the edges", [](uint32_t k) { const float v[] = {0.f, -0.f, -1.f, INFINITY, -INFINITY, NAN, 1e-45f, 1.f}; return v[k % 8]; }, 8,
         [](__m128 x) { return sse::log(x); }, [](float v) { return std::log(v); }, 0, 0, 0},
        {"sin", spread(-8192, 8192, 300000), 300000, [](__m128 x) { return sse::sin(x); }, [](float v) { return std::sin(v); }, 4, 2e-7, 0},
        {"sin near 0", spread(-10, 10, 200000), 200000, [](__m128 x) { return sse::sin(x); }, [](float v) { return std::sin(v); }, 4, 2e-7, 0},
        {"cos", spread(-8192, 8192, 300000), 300000, [](__m128 x) { return sse::cos(x); }, [](float v) { return std::cos(v); }, 4, 2e-7, 0},
        {"cos near 0", spread(-10, 10, 200000), 200000, [](__m128 x) { return sse::cos(x); }, [](float v) { return std::cos(v); }, 4, 2e-7, 0},
        {"sin and cos past the edges", [](uint32_t k) { const float v[] = {1e5f, -3e7f, INFINITY, -INFINITY, NAN, 0.f, -0.f, 8192.5f}; return v[k % 8]; }, 8,
         [](__m128 x) { return _mm_add_ps(sse::sin(x), sse::cos(x)); }, [](float v) { return std::sin(v) + std::cos(v); }, 0, 0, 0},
        {"atan", magnitudes, 100000, [](__m128 x) { return sse::atan(x); }, [](float v) { return std::atan(v); }, 4, 1e-7, 0},
        {"asin", spread(-1.02, 1.02, 200000), 200000, [](__m128 x) { return sse::asin(x); }, [](float v) { return std::asin(v); }, 4, 1e-7, 0},
    };
    for (const Sweep &s : sweeps) {
      ++checks;
      double worst = 0;
      float worstAt = 0;
      for (int k = 0; k < s.count; k += 4) {
        alignas(16) float in[4], got[4];
        for (int j = 0; j < 4; ++j) in[j] = s.input(uint32_t(std::min(k + j, s.count - 1)));
        _mm_store_ps(got, s.vec(_mm_load_ps(in)));
        for (int j = 0; j < 4; ++j) {
          const float want = s.lib(in[j]);
          double off;
          if (std::isnan(want) || std::isinf(want)) off = (std::isnan(want) ? std::isnan(got[j]) : got[j] == want) ? 0 : 1e30;
          else off = std::max(0., std::abs(double(got[j]) - want) - s.near) / double(unit(want));
          if (off > worst || off != off) {
            worst = off != off ? 1e30 : off;
            worstAt = in[j];
          }
        }
      }
      if (worst > s.ulps)
        failures << QString("the processor's %1, four lanes at a time: %2 units off the library's at %3").arg(s.name).arg(worst, 0, 'g', 4).arg(worstAt, 0, 'g', 9);
    }
    // pow and atan2 take two.
    ++checks;
    double worstPow = 0;
    QString worstPowAt;
    for (int k = 0; k < 200000; k += 4) {
      alignas(16) float x[4], y[4], got[4];
      for (int j = 0; j < 4; ++j) {
        const int n = k + j;
        x[j] = float(std::pow(10., -3 + 4.5 * ((int64_t(n) * 7919) % 200000) / 200000.));
        y[j] = float(-6 + 12 * ((int64_t(n) * 104729) % 200000) / 200000.);
        if (n % 97 == 0) y[j] = 1;
        if (n % 89 == 0) y[j] = 2;
        if (n % 83 == 0) x[j] = -2;
        if (n % 79 == 0) x[j] = 0;
        if (n % 73 == 0) x[j] = 1e-20f, y[j] = 6;
        if (n % 71 == 0) x[j] = 1e20f, y[j] = 5;
        if (n % 67 == 0) x[j] = 1e-6f, y[j] = 6.25f; // 1e-37.5: the band left to the library
      }
      _mm_store_ps(got, sse::pow(_mm_load_ps(x), _mm_load_ps(y)));
      for (int j = 0; j < 4; ++j) {
        const float want = std::pow(x[j], y[j]);
        double off;
        if (std::isnan(want) || std::isinf(want)) off = (std::isnan(want) ? std::isnan(got[j]) : got[j] == want) ? 0 : 1e30;
        else if (y[j] == 1 || x[j] <= 0) off = got[j] == want ? 0 : 1e30;
        else off = std::abs(double(got[j]) - want) / std::max(double(std::abs(want)), 1e-37);
        if (off > worstPow || off != off) {
          worstPow = off != off ? 1e30 : off;
          worstPowAt = QString("%1^%2").arg(x[j], 0, 'g', 9).arg(y[j], 0, 'g', 9);
        }
      }
    }
    if (worstPow > 2e-5) failures << QString("the processor's pow, four lanes at a time: %1 relative off the library's at %2").arg(worstPow, 0, 'g', 4).arg(worstPowAt);
    ++checks;
    double worstAtan2 = 0;
    QString worstAtan2At;
    const float edges[] = {0.f, -0.f, 1.f, -1.f, INFINITY, -INFINITY, NAN, 3e-39f, 1e30f};
    for (int k = 0; k < 160000 + 81 * 4; k += 4) {
      alignas(16) float y[4], x[4], got[4];
      for (int j = 0; j < 4; ++j) {
        const int n = k + j;
        if (n >= 160000) {
          y[j] = edges[(n - 160000) % 9];
          x[j] = edges[((n - 160000) / 9) % 9];
        } else {
          y[j] = float(-100 + 200 * ((int64_t(n) * 7919) % 160000) / 160000.);
          x[j] = float(-100 + 200 * ((int64_t(n) * 104729) % 160000) / 160000.);
        }
      }
      _mm_store_ps(got, sse::atan2(_mm_load_ps(y), _mm_load_ps(x)));
      for (int j = 0; j < 4; ++j) {
        const float want = std::atan2(y[j], x[j]);
        double off;
        if (std::isnan(want)) off = std::isnan(got[j]) ? 0 : 1e30;
        else if (x[j] == 0 || y[j] == 0 || std::isinf(x[j]) || std::isinf(y[j])) off = std::memcmp(&got[j], &want, 4) == 0 ? 0 : 1e30;
        else off = std::max(0., std::abs(double(got[j]) - want) - 2e-7) / double(unit(want));
        if (off > worstAtan2 || off != off) {
          worstAtan2 = off != off ? 1e30 : off;
          worstAtan2At = QString("(%1, %2)").arg(y[j], 0, 'g', 9).arg(x[j], 0, 'g', 9);
        }
      }
    }
    if (worstAtan2 > 4) failures << QString("the processor's atan2, four lanes at a time: %1 units off the library's at %2").arg(worstAtan2, 0, 'g', 4).arg(worstAtan2At);
  }
#endif
  return checks;
}

} // namespace cpu_shader
