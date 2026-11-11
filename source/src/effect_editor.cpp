#include "effect_editor.h"
#include "effect_packs.h"
#include "gpu_pixels.h"
#include "ui_theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QThreadPool>
#include <QElapsedTimer>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

namespace effect_editor {
namespace {
Host &host() {
  static Host h;
  return h;
}
void say(const QString &message, int ms = 5000) {
  if (host().status) host().status(message, ms);
}
void tell(QStringList ids) {
  ids.removeDuplicates();
  ids.removeAll(QString());
  if (!ids.isEmpty() && host().effectsChanged) host().effectsChanged(ids);
}
bool selfTesting() { return qApp && qApp->arguments().join(' ').contains("self-test"); }

// What the registry holds for each file, as this side last put it there: a
// file saved here and then seen changed on disk is not read in twice.
QHash<QString, QByteArray> &installed() {
  static QHash<QString, QByteArray> bytes;
  return bytes;
}
QString canonical(const QString &file) {
  const QString c = QFileInfo(file).canonicalFilePath();
  return c.isEmpty() ? QDir::cleanPath(QFileInfo(file).absoluteFilePath()) : c;
}
QByteArray fileBytes(const QString &file) {
  QFile f(file);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
QString jsonString(const QString &text) {
  const QByteArray array = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
  return QString::fromUtf8(array.mid(1, array.size() - 2));
}
// A name a file can carry.
QString fileNameFor(const QString &id) {
  QString name = id.trimmed();
  for (QChar &c : name)
    if (QStringLiteral("\\/:*?\"<>|").contains(c) || c.unicode() < 32) c = '-';
  return name.isEmpty() ? QString("Effect") : name;
}
// An id no effect has yet, and no file in the folder is named after.
QString freeId(const QString &wanted) {
  const QString base = wanted.trimmed().isEmpty() ? QString("My Effect") : wanted.trimmed();
  const QDir folder(userEffectsFolder());
  QString id = base;
  for (int n = 2; packEffect(id) || folder.exists(fileNameFor(id) + ".wfxs"); ++n) id = base + " " + QString::number(n);
  return id;
}
// The colours, as Dark has them; every look turns them (ui_theme).
QColor tone(const char *dark) { return ui_theme::ink(dark); }

// Where the leading comment -- the description -- ends ("*/"), or -1.
int descriptionEnd(const QString &text, int *start = nullptr) {
  int at = 0;
  while (at < text.size() && text[at].isSpace()) ++at;
  if (!QStringView(text).mid(at).startsWith(u"/*")) return -1;
  if (start) *start = at;
  return int(text.indexOf("*/", at + 2));
}
// The span of a string value of the description's own (not a parameter's),
// quotes included; {-1, 0} when it has none.
QPair<int, int> topLevelString(const QString &text, const QString &key) {
  int start = 0;
  const int end = descriptionEnd(text, &start);
  if (end < 0) return {-1, 0};
  int depth = 0;
  for (int i = int(text.indexOf('{', start)); i >= 0 && i < end; ++i) {
    const QChar c = text[i];
    if (c == '"') {
      // A string: a key of the description itself when it is at depth 1 and
      // a colon follows it.
      int close = i + 1;
      while (close < end && text[close] != '"') close += text[close] == '\\' ? 2 : 1;
      if (depth == 1 && QStringView(text).mid(i + 1, close - i - 1) == key) {
        int colon = close + 1;
        while (colon < end && text[colon].isSpace()) ++colon;
        if (colon < end && text[colon] == ':') {
          int value = colon + 1;
          while (value < end && text[value].isSpace()) ++value;
          if (value < end && text[value] == '"') {
            int valueEnd = value + 1;
            while (valueEnd < end && text[valueEnd] != '"') valueEnd += text[valueEnd] == '\\' ? 2 : 1;
            return {value, valueEnd - value + 1};
          }
        }
      }
      i = close;
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
    }
  }
  return {-1, 0};
}

// ---- the starting points -------------------------------------------------------------------
struct Starter {
  const char *name;
  const char *text;
};
const std::vector<Starter> &starters() {
  static const std::vector<Starter> list{
      {"Colour: change each pixel",
       R"(/* Wavy-Flo effect
{
  "id": {{ID}},
  "name": {{NAME}},
  "category": {{CATEGORY}},
  "perPixel": true,
  "parameters": [
    {"id": "tint", "kind": "color", "label": "Tint", "default": "#ff9d5c",
     "help": "The colour laid over the light parts of the picture."},
    {"id": "contrast", "label": "Contrast", "min": 0, "max": 200, "default": 120, "unit": " %"},
    {"id": "amount", "label": "Amount", "min": 0, "max": 100, "default": 100, "unit": " %"}
  ],
  "inert": "param_amount <= 0.0"
}
*/
// Runs for every pixel of the layer: `colour` is the pixel (straight alpha,
// 0 to 1), `coord` where it is in the raster, `size` the raster's size.
// Return its new colour. Wavy-Flo runs this on the graphics card, and on
// every core of the processor where there is none.
vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size) {
  vec3 c = (colour.rgb - 0.5) * (param_contrast / 100.0) + 0.5;
  c = mix(c, c * param_tint.rgb * 1.5, 0.5 * wavyLuma(c));
  return vec4(mix(colour.rgb, clamp(c, 0.0, 1.0), param_amount / 100.0), colour.a);
}
)"},
      {"Distort: move pixels with a wave",
       R"(/* Wavy-Flo effect
{
  "id": {{ID}},
  "name": {{NAME}},
  "category": {{CATEGORY}},
  "movesAlone": true,
  "parameters": [
    {"id": "height", "label": "Wave Height", "min": 0, "max": 400, "default": 24, "unit": " px",
     "help": "How far the pixels are pushed."},
    {"id": "length", "label": "Wave Length", "min": 4, "max": 2000, "default": 160, "unit": " px"},
    {"id": "direction", "kind": "angle", "label": "Direction", "default": 0},
    {"id": "speed", "label": "Speed", "min": -10, "max": 10, "default": 1, "step": 0.1,
     "help": "Waves a second; 0 holds it still."}
  ],
  "inert": "param_height <= 0.0",
  "varies": "param_speed != 0.0",
  "reach": "param_height"
}
*/
// Moves pixels: each takes its colour from a little way off, across the
// direction the wave travels. Sizes are in layer pixels; wavyScale turns them
// into the raster's (a half-size preview draws at half). "reach" tells
// Wavy-Flo how much room to leave around the layer.
vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size) {
  float a = radians(param_direction);
  vec2 along = vec2(cos(a), sin(a));
  vec2 across = vec2(-along.y, along.x);
  float phase = dot(coord / wavyScale, along) / param_length - wavySeconds * param_speed;
  vec2 offset = across * sin(phase * 6.2831853) * param_height * wavyScale;
  return wavySample(coord + offset);
}
)"},
      {"Blur: two passes, across then down",
       R"(/* Wavy-Flo effect
{
  "id": {{ID}},
  "name": {{NAME}},
  "category": {{CATEGORY}},
  "parameters": [
    {"id": "radius", "label": "Radius", "min": 0, "max": 200, "default": 10, "unit": " px",
     "help": "How far each pixel spreads."},
    {"id": "direction", "kind": "choice", "label": "Direction",
     "choices": ["Both", "Across", "Down"], "default": 0}
  ],
  "passes": "2",
  "reach": "param_radius",
  "inert": "param_radius <= 0.0"
}
*/
// A gaussian blur in two passes, across and then down: 2 x 25 samples a
// pixel instead of 25 x 25. What the first pass returns is what the second
// reads; in between, the colour is kept premultiplied so that clear pixels
// lend no colour. The last pass returns straight alpha again.
vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size) {
  bool first = wavyPass == 0;
  if ((first && param_direction == 2) || (!first && param_direction == 1))
    return first ? wavyPremultiply(colour) : wavyUnpremultiply(colour);
  float sigma = max(param_radius * wavyScale / 3.0, 0.3);
  int taps = int(ceil(sigma * 3.0));
  ivec2 here = ivec2(coord);
  ivec2 way = first ? ivec2(1, 0) : ivec2(0, 1);
  vec4 sum = vec4(0.0);
  float total = 0.0;
  for (int i = -taps; i <= taps; ++i) {
    float w = exp(-float(i * i) / (2.0 * sigma * sigma));
    vec4 c = wavyPixel(here + way * i);
    sum += (first ? wavyPremultiply(c) : c) * w;
    total += w;
  }
  vec4 blurred = sum / total;
  return first ? blurred : wavyUnpremultiply(blurred);
}
)"},
      {"Generate: draw a moving pattern",
       R"(/* Wavy-Flo effect
{
  "id": {{ID}},
  "name": {{NAME}},
  "category": {{CATEGORY}},
  "perPixel": true,
  "movesAlone": true,
  "parameters": [
    {"id": "first", "kind": "color", "label": "Colour A", "default": "#1d2bff"},
    {"id": "second", "kind": "color", "label": "Colour B", "default": "#ff3d9a"},
    {"id": "size", "label": "Size", "min": 2, "max": 2000, "default": 120, "unit": " px"},
    {"id": "speed", "label": "Speed", "min": 0, "max": 10, "default": 0.5, "step": 0.1},
    {"id": "opacity", "label": "Opacity", "min": 0, "max": 100, "default": 100, "unit": " %"},
    {"id": "inside", "kind": "bool", "label": "Inside the Layer Only", "default": true,
     "help": "Off, the pattern fills the layer's whole box, its clear parts too."}
  ],
  "inert": "param_opacity <= 0.0",
  "varies": "param_speed != 0.0"
}
*/
// Draws over the layer: smooth noise between two colours, drifting with time.
// wavyNoise gives the same numbers on the card and on the processor, so the
// pattern is the same wherever it is drawn.
vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size) {
  vec2 p = coord / wavyScale / param_size;
  float n = smoothstep(0.2, 0.8, wavyNoise(p + vec2(wavySeconds * param_speed, 0.0), 7.0));
  vec4 pattern = mix(param_first, param_second, n);
  float k = param_opacity / 100.0 * pattern.a;
  float alpha = param_inside ? colour.a : 1.0;
  return vec4(mix(colour.rgb, pattern.rgb, k), mix(colour.a, alpha, k));
}
)"},
      {"Blank: the least an effect needs",
       R"(/* Wavy-Flo effect
{
  "id": {{ID}},
  "name": {{NAME}},
  "category": {{CATEGORY}},
  "perPixel": true,
  "parameters": [
    {"id": "amount", "label": "Amount", "min": 0, "max": 100, "default": 100, "unit": " %"}
  ],
  "inert": "param_amount <= 0.0"
}
*/
vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size) {
  vec4 result = colour;
  // Change result here.
  return mix(colour, result, param_amount / 100.0);
}
)"}};
  return list;
}

// ---- the text --------------------------------------------------------------------------------
const QSet<QString> &keywords() {
  static const QSet<QString> words{"if",    "else",    "for",     "while",  "do",       "return", "break",
                                   "continue", "discard", "const", "in",     "out",      "inout",  "uniform",
                                   "true",  "false",   "struct",  "switch", "case",     "default", "precision",
                                   "highp", "mediump", "lowp"};
  return words;
}
const QSet<QString> &typeNames() {
  static const QSet<QString> words{"void",  "bool",  "int",   "uint",  "float", "vec2",  "vec3",  "vec4",
                                   "ivec2", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4", "bvec2", "bvec3",
                                   "bvec4", "mat2",  "mat3",  "mat4",  "mat2x2", "mat3x3", "mat4x4", "sampler2D"};
  return words;
}
const QSet<QString> &builtinNames() {
  static const QSet<QString> words{
      "abs",   "acos",        "asin",    "atan",        "ceil",      "clamp",       "cos",      "cosh",      "cross",
      "degrees", "distance",  "dot",     "exp",         "exp2",      "floor",       "fract",    "inversesqrt", "length",
      "log",   "log2",        "max",     "min",         "mix",       "mod",         "normalize", "pow",     "radians",
      "reflect", "refract",   "round",   "roundEven",   "sign",      "sin",         "sinh",     "smoothstep", "sqrt",
      "step",  "tan",         "tanh",    "trunc",       "texelFetch", "texture",    "textureSize", "isnan", "isinf",
      "floatBitsToInt", "floatBitsToUint", "intBitsToFloat", "uintBitsToFloat", "any", "all", "not", "lessThan",
      "greaterThan", "lessThanEqual", "greaterThanEqual", "equal", "notEqual", "transpose", "determinant", "inverse",
      "outerProduct", "matrixCompMult", "modf"};
  return words;
}
const QSet<QString> &hostNames() {
  static const QSet<QString> words{"wavyEffect",  "wavyPixel",       "wavySample",        "wavySampleEdge", "wavyOriginal",
                                   "wavyRandom",  "wavyNoise",       "wavyHash",          "wavyLuma",       "wavyCurve",
                                   "wavyPremultiply", "wavyUnpremultiply", "wavySeconds", "wavyFrame",      "wavyScale",
                                   "wavyBox",     "wavyPass",        "wavyPasses",        "sourceSize",     "processingDepth",
                                   "WAVY_SHOWN",  "WAVY_DIMMED",     "WAVY_HIDDEN"};
  return words;
}

// GLSL, and the description before it as JSON.
class Highlighter : public QSyntaxHighlighter {
public:
  explicit Highlighter(QTextDocument *document) : QSyntaxHighlighter(document) {}

protected:
  enum State { Code = 0, Description = 1, Comment = 2 };
  static QTextCharFormat format(const char *dark, bool bold = false, bool italic = false) {
    QTextCharFormat f;
    f.setForeground(tone(dark));
    if (bold) f.setFontWeight(QFont::DemiBold);
    if (italic) f.setFontItalic(true);
    return f;
  }
  // Whether nothing but blank lines comes before this one: a comment opened
  // here is the description.
  bool atTop() const {
    for (QTextBlock b = currentBlock().previous(); b.isValid(); b = b.previous())
      if (!b.text().trimmed().isEmpty()) return false;
    return true;
  }
  void json(const QString &text, int from, int to) {
    static const QRegularExpression number("-?\\d+(\\.\\d+)?([eE][-+]?\\d+)?");
    for (int i = from; i < to;) {
      const QChar c = text[i];
      if (c == '"') {
        int close = i + 1;
        while (close < to && text[close] != '"') close += text[close] == '\\' ? 2 : 1;
        close = std::min(close, to - 1);
        int after = close + 1;
        while (after < to && text[after].isSpace()) ++after;
        const bool key = after < to && text[after] == ':';
        setFormat(i, close - i + 1, key ? format("#9CDCFE") : format("#CE9178"));
        i = close + 1;
      } else if (c.isDigit() || (c == '-' && i + 1 < to && text[i + 1].isDigit())) {
        const auto m = number.match(text, i, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        const int length = m.hasMatch() ? int(m.capturedLength()) : 1;
        setFormat(i, length, format("#B5CEA8"));
        i += length;
      } else if (c.isLetter()) {
        int end = i;
        while (end < to && text[end].isLetter()) ++end;
        const QStringView word = QStringView(text).mid(i, end - i);
        if (word == u"true" || word == u"false" || word == u"null") setFormat(i, end - i, format("#569CD6"));
        else if (word == u"Wavy") setFormat(i, std::min(to, i + 15) - i, format("#E2B86B", true));
        i = end;
      } else {
        ++i;
      }
    }
  }
  void highlightBlock(const QString &text) override {
    int state = std::max(0, previousBlockState());
    const int n = int(text.size());
    int i = 0;
    while (i < n) {
      if (state == Description || state == Comment) {
        const int close = text.indexOf("*/", i);
        const int to = close < 0 ? n : close;
        if (state == Description) json(text, i, to);
        else setFormat(i, to - i, format("#6A9955", false, true));
        if (close < 0) { i = n; break; }
        setFormat(close, 2, format("#6A9955"));
        i = close + 2;
        state = Code;
        continue;
      }
      const QChar c = text[i];
      if (c == '/' && i + 1 < n && text[i + 1] == '/') {
        setFormat(i, n - i, format("#6A9955", false, true));
        break;
      }
      if (c == '/' && i + 1 < n && text[i + 1] == '*') {
        state = atTop() && text.left(i).trimmed().isEmpty() ? Description : Comment;
        setFormat(i, 2, format("#6A9955"));
        i += 2;
        continue;
      }
      if (c == '#' && text.left(i).trimmed().isEmpty()) {
        setFormat(i, n - i, format("#C586C0"));
        break;
      }
      if (c.isLetter() || c == '_') {
        int end = i;
        while (end < n && (text[end].isLetterOrNumber() || text[end] == '_')) ++end;
        const QString word = text.mid(i, end - i);
        if (keywords().contains(word)) setFormat(i, end - i, format("#C586C0"));
        else if (typeNames().contains(word)) setFormat(i, end - i, format("#4EC9B0"));
        else if (word.startsWith("param_")) setFormat(i, end - i, format("#9CDCFE", true));
        else if (hostNames().contains(word)) setFormat(i, end - i, format("#6CB6FF", true));
        else if (builtinNames().contains(word)) setFormat(i, end - i, format("#DCDCAA"));
        i = end;
        continue;
      }
      if (c.isDigit() || (c == '.' && i + 1 < n && text[i + 1].isDigit())) {
        int end = i;
        while (end < n && (text[end].isLetterOrNumber() || text[end] == '.' ||
                           ((text[end] == '-' || text[end] == '+') && end > i && (text[end - 1] == 'e' || text[end - 1] == 'E'))))
          ++end;
        setFormat(i, end - i, format("#B5CEA8"));
        i = end;
        continue;
      }
      ++i;
    }
    setCurrentBlockState(state);
  }
};

class CodeEdit;
class Gutter : public QWidget {
public:
  explicit Gutter(CodeEdit *edit);
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  CodeEdit *edit;
};

// The text, with the line numbers beside it, the line a problem is on marked,
// and the keys a person writing code expects: Tab and Shift+Tab indent,
// Enter keeps the indent, Ctrl+/ turns lines into comments and back.
class CodeEdit : public QPlainTextEdit {
public:
  explicit CodeEdit(QWidget *parent = nullptr) : QPlainTextEdit(parent), gutter(new Gutter(this)) {
    setObjectName("effectSource");
    setLineWrapMode(NoWrap);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const QStringList families = QFontDatabase::families();
    for (const char *family : {"Cascadia Mono", "Consolas"})
      if (families.contains(family)) {
        mono.setFamily(family);
        break;
      }
    mono.setPointSizeF(10.5);
    setFont(mono);
    setTabStopDistance(QFontMetricsF(mono).horizontalAdvance(' ') * 2);
    setStyleSheet("QPlainTextEdit#effectSource { background: #17171A; color: #D4D4D8; border: none;"
                  " selection-background-color: #264F78; selection-color: #FFFFFF; }");
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { setViewportMargins(gutterWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
      if (dy) gutter->scroll(0, dy);
      else gutter->update(0, rect.y(), gutter->width(), rect.height());
      if (rect.contains(viewport()->rect())) setViewportMargins(gutterWidth(), 0, 0, 0);
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] { marks(); });
    setViewportMargins(gutterWidth(), 0, 0, 0);
    marks();
  }
  int gutterWidth() const {
    int digits = 1;
    for (int n = std::max(1, blockCount()); n >= 10; n /= 10) ++digits;
    return 18 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * std::max(3, digits);
  }
  void setErrorLine(int line) {
    if (errorLine == line) return;
    errorLine = line;
    marks();
    gutter->update();
  }
  void goToLine(int line) {
    const QTextBlock block = document()->findBlockByNumber(std::max(0, line - 1));
    if (!block.isValid()) return;
    QTextCursor c(block);
    c.movePosition(QTextCursor::EndOfLine);
    setTextCursor(c);
    centerCursor();
    setFocus();
  }
  void paintGutter(QPaintEvent *event) {
    QPainter p(gutter);
    p.fillRect(event->rect(), tone("#17171A"));
    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int current = textCursor().blockNumber();
    const int height = fontMetrics().height();
    p.setFont(font());
    while (block.isValid() && top <= event->rect().bottom()) {
      if (block.isVisible() && bottom >= event->rect().top()) {
        const bool wrong = number + 1 == errorLine;
        p.setPen(wrong ? tone("#FF6B81") : number == current ? tone("#C8CCD4") : tone("#5A5E6B"));
        p.drawText(0, top, gutter->width() - 10, height, Qt::AlignRight | Qt::AlignVCenter, QString::number(number + 1));
        if (wrong) {
          p.save();
          p.setRenderHint(QPainter::Antialiasing);
          p.setPen(Qt::NoPen);
          p.setBrush(tone("#FF6B81"));
          p.drawEllipse(QPointF(6, top + height / 2.), 3, 3);
          p.restore();
        }
      }
      block = block.next();
      top = bottom;
      bottom = top + qRound(blockBoundingRect(block).height());
      ++number;
    }
  }
  // Repaints what depends on the look (the line numbers, the marks).
  void lookChanged() {
    marks();
    gutter->update();
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QPlainTextEdit::resizeEvent(event);
    const QRect r = contentsRect();
    gutter->setGeometry(QRect(r.left(), r.top(), gutterWidth(), r.height()));
  }
  void keyPressEvent(QKeyEvent *event) override {
    QTextCursor c = textCursor();
    const bool lines = c.hasSelection() && document()->findBlock(c.selectionStart()) != document()->findBlock(c.selectionEnd());
    if (event->key() == Qt::Key_Backtab || (event->key() == Qt::Key_Tab && lines)) {
      indentLines(event->key() == Qt::Key_Tab ? 1 : -1);
      return;
    }
    if (event->key() == Qt::Key_Tab && !(event->modifiers() & Qt::ControlModifier)) {
      c.insertText("  ");
      return;
    }
    if ((event->key() == Qt::Key_Slash) && (event->modifiers() & Qt::ControlModifier)) {
      toggleComment();
      return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
      // The indent of the line it breaks, one step more after an opening brace.
      const QString line = c.block().text();
      const QString before = line.left(c.positionInBlock());
      const QString after = line.mid(c.positionInBlock());
      QString indent;
      for (QChar ch : line) {
        if (ch != ' ' && ch != '\t') break;
        indent += ch;
      }
      const QString base = indent;
      const QString trimmed = before.trimmed();
      if (trimmed.endsWith('{') || trimmed.endsWith('(') || trimmed.endsWith('[')) indent += "  ";
      c.beginEditBlock();
      c.insertText("\n" + indent);
      // Between a pair of braces: the closing one on a line of its own.
      if (trimmed.endsWith('{') && after.trimmed().startsWith('}')) {
        const int at = c.position();
        c.insertText("\n" + base);
        c.setPosition(at);
      }
      c.endEditBlock();
      setTextCursor(c);
      return;
    }
    if (event->text() == "}" && c.block().text().left(c.positionInBlock()).trimmed().isEmpty()) {
      // A closing brace goes back a step.
      c.beginEditBlock();
      const int column = c.positionInBlock();
      const int back = std::min(2, column);
      c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, back);
      if (c.selectedText().trimmed().isEmpty()) c.removeSelectedText();
      else c.clearSelection();
      c.insertText("}");
      c.endEditBlock();
      setTextCursor(c);
      return;
    }
    QPlainTextEdit::keyPressEvent(event);
  }

private:
  Gutter *gutter;
  int errorLine = 0;
  void marks() {
    QList<QTextEdit::ExtraSelection> list;
    if (errorLine > 0) {
      const QTextBlock block = document()->findBlockByNumber(errorLine - 1);
      if (block.isValid()) {
        QTextEdit::ExtraSelection wrong;
        wrong.format.setBackground(tone("#3A1D24"));
        wrong.format.setProperty(QTextFormat::FullWidthSelection, true);
        wrong.cursor = QTextCursor(block);
        list << wrong;
      }
    }
    if (!isReadOnly()) {
      QTextEdit::ExtraSelection here;
      here.format.setBackground(tone("#202025"));
      here.format.setProperty(QTextFormat::FullWidthSelection, true);
      here.cursor = textCursor();
      here.cursor.clearSelection();
      if (here.cursor.blockNumber() + 1 != errorLine) list << here;
    }
    setExtraSelections(list);
  }
  // The selected lines, or the cursor's: each moved a step in or out.
  void indentLines(int way) {
    QTextCursor c = textCursor();
    QTextBlock first = document()->findBlock(c.selectionStart());
    QTextBlock last = document()->findBlock(std::max(c.selectionStart(), c.selectionEnd() - (c.hasSelection() ? 1 : 0)));
    c.beginEditBlock();
    for (QTextBlock b = first; b.isValid(); b = b.next()) {
      QTextCursor line(b);
      if (way > 0) {
        line.insertText("  ");
      } else {
        const QString text = b.text();
        const int spaces = text.startsWith("  ") ? 2 : text.startsWith(' ') || text.startsWith('\t') ? 1 : 0;
        line.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, spaces);
        line.removeSelectedText();
      }
      if (b == last) break;
    }
    c.endEditBlock();
  }
  void toggleComment() {
    QTextCursor c = textCursor();
    QTextBlock first = document()->findBlock(c.selectionStart());
    QTextBlock last = document()->findBlock(std::max(c.selectionStart(), c.selectionEnd() - (c.hasSelection() ? 1 : 0)));
    bool all = true;
    for (QTextBlock b = first; b.isValid(); b = b.next()) {
      if (!b.text().trimmed().isEmpty() && !b.text().trimmed().startsWith("//")) all = false;
      if (b == last) break;
    }
    c.beginEditBlock();
    for (QTextBlock b = first; b.isValid(); b = b.next()) {
      const QString text = b.text();
      int indent = 0;
      while (indent < text.size() && text[indent].isSpace()) ++indent;
      QTextCursor line(b);
      line.setPosition(b.position() + indent);
      if (all) {
        if (QStringView(text).mid(indent).startsWith(u"//")) {
          line.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, QStringView(text).mid(indent).startsWith(u"// ") ? 3 : 2);
          line.removeSelectedText();
        }
      } else if (!text.trimmed().isEmpty()) {
        line.insertText("// ");
      }
      if (b == last) break;
    }
    c.endEditBlock();
  }
  friend class Gutter;
};

Gutter::Gutter(CodeEdit *e) : QWidget(e), edit(e) {}
QSize Gutter::sizeHint() const { return {edit->gutterWidth(), 0}; }
void Gutter::paintEvent(QPaintEvent *event) { edit->paintGutter(event); }

// The line a card's compiler names in its log: "0(12) : error" (NVIDIA),
// "ERROR: 0:12:" (AMD, Intel), or 0.
int cardLogLine(const QString &log) {
  static const QRegularExpression nvidia("\\b\\d+\\((\\d+)\\)"), others("\\b\\d+:(\\d+):");
  if (const auto m = nvidia.match(log); m.hasMatch()) return m.captured(1).toInt();
  if (const auto m = others.match(log); m.hasMatch()) return m.captured(1).toInt();
  return 0;
}
QString firstLine(const QString &text) {
  const QStringList lines = text.trimmed().split('\n');
  return lines.isEmpty() ? QString() : lines.front().trimmed();
}

// ---- the window ------------------------------------------------------------------------------
class EffectWindow;
QHash<QString, QPointer<EffectWindow>> &windows() {
  static QHash<QString, QPointer<EffectWindow>> open;
  return open;
}

class EffectWindow : public QWidget {
public:
  EffectWindow(QWidget *parent, const QString &file) : QWidget(parent, Qt::Window), path(canonical(file)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("effectEditor");
    resize(1180, 760);
    mine = isUsersOwn(path);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Along the top: what this is, then what can be done with it.
    auto *bar = new QWidget;
    bar->setObjectName("effectEditorBar");
    bar->setStyleSheet("QWidget#effectEditorBar { background: #222226; border-bottom: 1px solid #141416; }");
    auto *top = new QHBoxLayout(bar);
    top->setContentsMargins(12, 7, 10, 7);
    top->setSpacing(8);
    title = new QLabel;
    title->setStyleSheet("font-weight: 600; color: #DEDEDE;");
    top->addWidget(title);
    fileLabel = new QLabel;
    fileLabel->setStyleSheet("color: #7E8496;");
    fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(fileLabel, 1);
    addButton = new QToolButton;
    addButton->setText("+ Parameter");
    addButton->setToolTip("Adds a control to the effect's description, named so no other is: use it in the shader as param_<id>.");
    addButton->setPopupMode(QToolButton::InstantPopup);
    auto *kinds = new QMenu(addButton);
    for (const auto &[label, kind] : std::vector<std::pair<QString, QString>>{{"Slider", "float"},
                                                                           {"Angle", "angle"},
                                                                           {"Checkbox", "bool"},
                                                                           {"Choice", "choice"},
                                                                           {"Colour", "color"},
                                                                           {"Curve", "curve"},
                                                                           {"Point", "point"},
                                                                           {"Group (a twirl around controls)", "group"}})
      kinds->addAction(label, this, [this, kind = kind] { addParameter(kind); });
    addButton->setMenu(kinds);
    top->addWidget(addButton);
    live = new QCheckBox("Live");
    live->setToolTip("Draw the effect in the preview as it is typed, before it is saved. "
                     "Closing without saving puts back what the file holds.");
    live->setChecked(QSettings().value("effectEditor/live", true).toBool());
    connect(live, &QCheckBox::toggled, this, [this](bool on) {
      QSettings().setValue("effectEditor/live", on);
      if (on) check();
      else putBackSaved();
    });
    top->addWidget(live);
    applyButton = new QPushButton("Apply to Layer");
    applyButton->setToolTip("Put this effect on the selected layer.");
    connect(applyButton, &QPushButton::clicked, this, [this] { applyToLayer(); });
    top->addWidget(applyButton);
    checkButton = new QPushButton("Check");
    checkButton->setToolTip("Saves, then checks the effect as its author would before handing it out: runs it on the "
                            "graphics card and on the processor at its defaults and with every slider at either end, "
                            "compares the two pictures, and times both at 1920x1080.");
    connect(checkButton, &QPushButton::clicked, this, [this] { runCheck(); });
    top->addWidget(checkButton);
    saveButton = new QPushButton("Save");
    saveButton->setToolTip("Save (Ctrl+S). Wavy-Flo reads the effect again at once.");
    connect(saveButton, &QPushButton::clicked, this, [this] { save(); });
    top->addWidget(saveButton);
    guideButton = new QToolButton;
    guideButton->setText("Guide");
    guideButton->setCheckable(true);
    guideButton->setToolTip("What a shader can use, the effect's own parameters, and the description's keys. "
                            "Click a name to put it where the cursor is.");
    top->addWidget(guideButton);
    layout->addWidget(bar);

    // A banner for an effect that came with Wavy-Flo: it is read here, and
    // changed in a copy of the person's own.
    if (!mine) {
      auto *banner = new QWidget;
      banner->setObjectName("effectEditorBanner");
      banner->setStyleSheet("QWidget#effectEditorBanner { background: #2E2A1E; }");
      auto *row = new QHBoxLayout(banner);
      row->setContentsMargins(12, 6, 10, 6);
      auto *note = new QLabel("This effect came with Wavy-Flo, so it is shown here to read. To change it, make a copy of your own.");
      note->setStyleSheet("color: #E6C77A;");
      row->addWidget(note, 1);
      auto *copy = new QPushButton("Duplicate to My Effects");
      connect(copy, &QPushButton::clicked, this, [this] {
        const QString made = duplicate(parentWidget(), path);
        if (!made.isEmpty()) close();
      });
      row->addWidget(copy);
      layout->addWidget(banner);
    }

    auto *split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    edit = new CodeEdit;
    highlighter = new Highlighter(edit->document());
    split->addWidget(edit);
    guide = new QTextBrowser;
    guide->setObjectName("effectGuide");
    guide->setOpenLinks(false);
    guide->setStyleSheet("QTextBrowser#effectGuide { background: #1D1D21; color: #C8CCD4; border: none; border-left: 1px solid #141416; }");
    guide->setMinimumWidth(280);
    connect(guide, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
      const int index = url.fragment().toInt();
      if (index >= 0 && index < snippets.size()) {
        edit->insertPlainText(snippets[index]);
        edit->setFocus();
      }
    });
    split->addWidget(guide);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setSizes({860, 320});
    layout->addWidget(split, 1);
    const bool showGuide = QSettings().value("effectEditor/guide", true).toBool();
    guide->setVisible(showGuide);
    guideButton->setChecked(showGuide);
    connect(guideButton, &QToolButton::toggled, this, [this](bool on) {
      guide->setVisible(on);
      QSettings().setValue("effectEditor/guide", on);
    });

    // Along the bottom: whether it compiles, and where the cursor is.
    auto *foot = new QWidget;
    foot->setObjectName("effectEditorFoot");
    foot->setStyleSheet("QWidget#effectEditorFoot { background: #222226; border-top: 1px solid #141416; }");
    auto *bottom = new QHBoxLayout(foot);
    bottom->setContentsMargins(12, 5, 12, 5);
    bottom->setSpacing(8);
    dot = new QLabel(QString::fromUtf8("●"));
    bottom->addWidget(dot);
    message = new QLabel;
    message->setObjectName("effectEditorMessage");
    message->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    message->setWordWrap(true);
    connect(message, &QLabel::linkActivated, this, [this](const QString &link) { edit->goToLine(link.toInt()); });
    bottom->addWidget(message, 1);
    cost = new QLabel;
    cost->setObjectName("effectEditorCost");
    cost->setStyleSheet("color: #7E8496;");
    cost->setToolTip("What it takes on the processor, all cores, for a 1920x1080 frame at its defaults -- measured "
                     "on a test picture as you type. On the graphics card it is usually quicker still.");
    bottom->addWidget(cost);
    where = new QLabel;
    where->setStyleSheet("color: #7E8496;");
    bottom->addWidget(where);
    layout->addWidget(foot);

    pause.setSingleShot(true);
    pause.setInterval(350);
    connect(&pause, &QTimer::timeout, this, [this] { check(); });
    connect(edit, &QPlainTextEdit::textChanged, this, [this] {
      updateTitle();
      pause.start();
    });
    connect(edit, &QPlainTextEdit::cursorPositionChanged, this, [this] {
      const QTextCursor c = edit->textCursor();
      where->setText(QString("Line %1, Column %2").arg(c.blockNumber() + 1).arg(c.positionInBlock() + 1));
    });
    auto *saving = new QShortcut(QKeySequence::Save, this);
    connect(saving, &QShortcut::activated, this, [this] { save(); });

    // The look turning: the colours painted here follow it.
    QPointer<EffectWindow> self(this);
    ui_theme::listeners().push_back([self] {
      if (!self) return;
      self->highlighter->rehighlight();
      self->edit->lookChanged();
      self->check();
    });

    saved = fileBytes(path);
    edit->setPlainText(QString::fromUtf8(saved));
    edit->setReadOnly(!mine);
    live->setEnabled(mine);
    saveButton->setEnabled(mine);
    addButton->setEnabled(mine);
    checkButton->setEnabled(!selfTesting());
    fileLabel->setText(QDir::toNativeSeparators(path));
    updateTitle();
    check();
    cardCheck();
  }

  const QString path;

  bool dirty() const { return mine && edit->toPlainText().toUtf8() != saved; }
  // Asks what becomes of changes not saved; false when the person cancelled.
  bool settle() {
    if (!dirty()) return true;
    raise();
    activateWindow();
    const auto answer = QMessageBox::question(this, "Effect not saved",
                                              QString("Save the changes to %1?").arg(QFileInfo(path).fileName()),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Save) return save();
    edit->document()->setModified(false);
    discarding = true;
    putBackSaved();
    return true;
  }
  bool save() {
    if (!mine) return false;
    const QByteArray bytes = edit->toPlainText().toUtf8();
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
      showState(2, "Could not save: " + out.errorString());
      return false;
    }
    saved = bytes;
    updateTitle();
    if (installed().value(path) != bytes) {
      const QStringList ids = reloadTextEffect(path);
      if (!ids.isEmpty()) {
        installed()[path] = bytes;
        tell(ids);
      }
    }
    say("Saved " + QFileInfo(path).fileName(), 3000);
    check();
    cardCheck();
    return true;
  }
  // The file was changed by another program.
  void changedOnDisk(const QByteArray &bytes) {
    if (bytes == saved) return;
    if (dirty()) {
      showState(1, "The file was changed by another program. Saving here keeps what is in this window.");
      return;
    }
    saved = bytes;
    const int at = edit->textCursor().position();
    edit->setPlainText(QString::fromUtf8(bytes));
    QTextCursor c = edit->textCursor();
    c.setPosition(std::min(at, int(edit->document()->characterCount()) - 1));
    edit->setTextCursor(c);
    updateTitle();
  }

protected:
  void closeEvent(QCloseEvent *event) override {
    if (!discarding && !settle()) {
      event->ignore();
      return;
    }
    windows().remove(path);
    event->accept();
  }

private:
  bool mine = false, discarding = false;
  CodeEdit *edit = nullptr;
  Highlighter *highlighter = nullptr;
  QTextBrowser *guide = nullptr;
  QLabel *title = nullptr, *fileLabel = nullptr, *dot = nullptr, *message = nullptr, *where = nullptr;
  QCheckBox *live = nullptr;
  QPushButton *applyButton = nullptr, *checkButton = nullptr, *saveButton = nullptr;
  QToolButton *addButton = nullptr, *guideButton = nullptr;
  QTimer pause;
  QByteArray saved;
  QStringList snippets; // what each link in the guide puts in
  QString guideFor;     // the parameters the guide was last written for
  QString cardProblem;  // what the card said of the text last saved
  QLabel *cost = nullptr;
  QByteArray costFor;   // the text the cost shown was measured for

  void updateTitle() {
    QString name = textEffectId(path);
    if (name.isEmpty()) name = QFileInfo(path).completeBaseName();
    const QString shown = (dirty() ? "* " : "") + name;
    setWindowTitle(shown + " - Effect Editor");
    title->setText(shown);
  }
  // 0: all is well; 1: something to know; 2: it does not compile.
  void showState(int level, const QString &text, int line = 0) {
    static const char *colours[] = {"#40FFD0", "#FFB454", "#FF6B81"};
    dot->setStyleSheet(QString("color: %1;").arg(colours[std::clamp(level, 0, 2)]));
    QString shown = text.toHtmlEscaped();
    if (line > 0) {
      static const QRegularExpression named("\\bline \\d+");
      const QString link = QString("<a href=\"%1\" style=\"color: #6CB6FF;\">line %1</a>").arg(line);
      if (named.match(shown).hasMatch()) shown.replace(named, link);
      else shown += " (" + link + ")";
    }
    message->setText(shown);
    message->setToolTip(text);
    edit->setErrorLine(level == 2 ? line : 0);
  }
  // After a pause in the typing: does it compile, and -- Live -- on with it.
  void check() {
    const QString text = edit->toPlainText();
    const QByteArray bytes = text.toUtf8();
    QString problem;
    auto parsed = readTextEffect(bytes, path, &problem);
    if (!parsed) {
      showState(2, problem, problemLine(text, problem));
      return;
    }
    writeGuide(*parsed);
    const QString now = parsed->id;
    measure(std::move(parsed), bytes);
    if (mine && live->isChecked() && installed().value(path) != bytes && QFileInfo::exists(path)) {
      const QStringList ids = reloadTextEffect(path, &bytes);
      if (ids.isEmpty()) {
        const QString why = textEffectProblem(path);
        if (!why.isEmpty()) {
          showState(2, why, problemLine(text, why));
          return;
        }
      } else {
        installed()[path] = bytes;
        tell(ids);
      }
    }
    const QString before = topLevelValue(QString::fromUtf8(saved), "id");
    if (!cardProblem.isEmpty() && bytes == saved)
      showState(1, "Drawn on the processor: the graphics card refuses this shader -- " + cardProblem, cardLogLine(cardProblem));
    else if (!before.isEmpty() && before != now)
      showState(1, QString("Its id is now \"%1\": layers that use \"%2\" no longer find it. Projects keep the id.").arg(now, before));
    else if (!mine)
      showState(0, "Compiles.");
    else if (live->isChecked())
      showState(0, bytes == saved ? "Compiles. Saved." : "Compiles. In the preview as you type; Save keeps it.");
    else
      showState(0, bytes == saved ? "Compiles. Saved." : "Compiles. Save to see it in the preview.");
  }
  // What it costs on the processor, worked out beside the interface: the effect
  // at its defaults over a test picture a quarter of 1080p, timed twice (the
  // first run warms), scaled to the whole frame. (A sixteenth said twice too
  // much: on so few pixels, handing the work out is a large part of it.)
  void measure(std::unique_ptr<PackEffect> parsed, const QByteArray &bytes) {
    if (bytes == costFor) return;
    costFor = bytes;
    cost->setText("measuring...");
    QPointer<EffectWindow> self(this);
    std::shared_ptr<const PackEffect> effect(std::move(parsed));
    QThreadPool::globalInstance()->start([effect, self, bytes] {
      QImage picture(960, 540, QImage::Format_RGBA32FPx4);
      for (int y = 0; y < picture.height(); ++y) {
        float *row = reinterpret_cast<float *>(picture.scanLine(y));
        for (int x = 0; x < picture.width(); ++x) {
          row[x * 4] = x / 959.f;
          row[x * 4 + 1] = y / 539.f;
          row[x * 4 + 2] = float((x / 32 + y / 32) % 2);
          row[x * 4 + 3] = x > 40 && x < 920 ? 1.f : .5f;
        }
      }
      Layer none;
      const auto values = packEffectValues(none, *effect, 0);
      double best = 1e9;
      for (int run = 0; run < 2; ++run) {
        QImage working = picture;
        QElapsedTimer clock;
        clock.start();
        runPackEffect(working, *effect, values, QRectF(40, 20, 880, 500), 1, .5, 15);
        best = std::min(best, clock.nsecsElapsed() / 1e6);
      }
      const double frameMs = best * 4;
      if (!QCoreApplication::instance()) return;
      QMetaObject::invokeMethod(qApp, [self, bytes, frameMs] {
        if (!self || self->costFor != bytes) return;
        self->cost->setText(frameMs < 1 ? QString("< 1 ms a 1080p frame") : QString("about %1 ms a 1080p frame").arg(frameMs, 0, 'f', frameMs < 10 ? 1 : 0));
      }, Qt::QueuedConnection);
    });
  }
  // The card compiles what was saved too, where there is one: a shader it
  // refuses is still drawn, on the processor, but the author should know.
  void cardCheck() {
    cardProblem.clear();
    if (selfTesting()) return;
    QString problem;
    const auto parsed = readTextEffect(saved, path, &problem);
    if (!parsed) return;
    const QString log = gpuShaderProblem(parsed->shader);
    if (!log.isEmpty() && !log.startsWith("no usable") && !log.startsWith("graphics card unavailable") && log != "no shader supplied")
      cardProblem = firstLine(log);
    check();
  }
  static QString topLevelValue(const QString &text, const QString &key) {
    const auto span = topLevelString(text, key);
    if (span.first < 0) return {};
    const QJsonDocument d = QJsonDocument::fromJson(("[" + text.mid(span.first, span.second) + "]").toUtf8());
    return d.isArray() ? d.array().at(0).toString() : QString();
  }
  // The registry back to what the file holds (Live off, or changes discarded).
  void putBackSaved() {
    if (!mine || installed().value(path) == saved || !QFileInfo::exists(path)) return;
    const QStringList ids = reloadTextEffect(path);
    if (!ids.isEmpty()) {
      installed()[path] = saved;
      tell(ids);
    }
  }
  void addParameter(const QString &kind) {
    QString id;
    const QString text = withParameter(edit->toPlainText(), kind, &id);
    if (text.isNull()) {
      showState(2, "The description could not be found: the file must start with /* Wavy-Flo effect { ... } */");
      return;
    }
    // One step of Undo.
    QTextCursor c(edit->document());
    c.beginEditBlock();
    c.select(QTextCursor::Document);
    c.insertText(text);
    c.endEditBlock();
    const int line = id.isEmpty() ? 0 : problemLine(text, QString("\"%1\"").arg(id));
    if (line > 0) edit->goToLine(line);
    if (!id.isEmpty()) say(QString("Added %1: param_%1 in the shader").arg(id), 5000);
  }
  void applyToLayer() {
    const QString id = textEffectId(path);
    if (id.isEmpty()) {
      showState(2, "It has not loaded yet: " + textEffectProblem(path));
      return;
    }
    const QString why = host().applyToSelected ? host().applyToSelected(id) : QString("not available");
    if (!why.isEmpty()) showState(1, why);
  }
  // The --check-effect report, run on a copy so nothing is left beside the file.
  void runCheck() {
    if (mine && dirty() && !save()) return;
    auto *folder = new QTemporaryDir;
    if (!folder->isValid()) {
      delete folder;
      showState(2, "No room for a check: the temporary folder cannot be used.");
      return;
    }
    const QString copy = folder->filePath(QFileInfo(path).fileName());
    QFile::copy(path, copy);
    auto *process = new QProcess(this);
    checkButton->setEnabled(false);
    checkButton->setText("Checking...");
    showState(0, "Checking: the card and the processor draw it at its defaults and with every slider at either end...");
    connect(process, &QProcess::finished, this, [this, process, folder, copy](int code, QProcess::ExitStatus) {
      checkButton->setEnabled(true);
      checkButton->setText("Check");
      QString report = QString::fromUtf8(fileBytes(copy + ".check.txt"));
      if (report.isEmpty()) report = QString::fromUtf8(process->readAllStandardOutput());
      report.replace(QDir::toNativeSeparators(copy), QDir::toNativeSeparators(path));
      QImage picture;
      for (const QString &png : QDir(folder->path()).entryList({"*.check.png"}, QDir::Files)) picture.load(folder->filePath(png));
      showReport(report, picture, code == 0);
      showState(code == 0 ? 0 : 1, code == 0 ? "Check: everything holds." : "Check: something needs looking at -- see the report.");
      process->deleteLater();
      delete folder;
    });
    process->start(QCoreApplication::applicationFilePath(), {"--check-effect", copy});
  }
  void showReport(const QString &report, const QImage &picture, bool ok) {
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QFileInfo(path).fileName() + (ok ? " - holds" : " - needs looking at"));
    dialog->resize(900, 620);
    auto *layout = new QVBoxLayout(dialog);
    auto *row = new QHBoxLayout;
    auto *text = new QPlainTextEdit(report);
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    text->setFont(edit->font());
    row->addWidget(text, 3);
    if (!picture.isNull()) {
      auto *shown = new QLabel;
      shown->setPixmap(QPixmap::fromImage(picture.scaled(320, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
      shown->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
      shown->setToolTip("The effect at its defaults, on the check's test picture.");
      row->addWidget(shown, 1);
    }
    layout->addLayout(row, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
  }
  // The guide: what a shader can use, with the effect's own parameters first.
  void writeGuide(const PackEffect &effect) {
    QStringList own;
    const WfxEffect *api = effect.api;
    for (int i = 0; api && i < api->parameterCount; ++i) {
      const WfxParameter &p = api->parameters[i];
      if (!p.id) continue;
      const QString id = QString::fromUtf8(p.id);
      const QString kind = p.kind == WFX_BOOL ? "bool" : p.kind == WFX_CHOICE ? "int" : p.kind == WFX_COLOR ? "vec4"
                           : p.kind == WFX_CURVE ? "float(float)" : p.kind == WFX_POINT ? "vec2"
                           : p.kind == WFX_POINT_3D ? "vec3" : "float";
      own << id + "\t" + kind + "\t" + QString::fromUtf8(p.label ? p.label : p.id);
    }
    const QString signature = own.join('\n');
    if (signature == guideFor) return;
    guideFor = signature;
    snippets.clear();
    auto link = [&](const QString &shown, const QString &inserted) {
      snippets << inserted;
      return QString("<a href=\"#%1\" style=\"color: #6CB6FF; text-decoration: none;\"><code>%2</code></a>")
          .arg(snippets.size() - 1)
          .arg(shown.toHtmlEscaped());
    };
    QString html = "<style>td { padding: 2px 6px 2px 0; vertical-align: top; } h3 { color: #DEDEDE; margin: 12px 0 4px 0; }"
                   " .note { color: #8E93A3; }</style>";
    html += "<h3>This effect's parameters</h3><table>";
    if (own.isEmpty()) html += "<tr><td class=note>None yet: + Parameter adds one.</td></tr>";
    for (const QString &line : own) {
      const QStringList parts = line.split('\t');
      const QString name = "param_" + parts[0];
      html += "<tr><td>" + link(name, parts[1] == "float(float)" ? name + "(" : name) + "</td><td class=note>" +
              parts[1].toHtmlEscaped() + "</td><td class=note>" + parts.value(2).toHtmlEscaped() + "</td></tr>";
    }
    html += "</table>";
    html += "<h3>The shader</h3><p class=note>Define exactly " +
            link("vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size)", "vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size) {\n  return colour;\n}\n") +
            ". <code>colour</code> is the pixel, straight alpha; <code>coord</code> its centre in the raster (the top row "
            "first); <code>size</code> the raster's size. Return the pixel's new colour, straight alpha.</p>";
    html += "<h3>What it can use</h3><table>";
    struct Row { const char *shown, *inserted, *note; };
    static const Row helpers[] = {
        {"wavyPixel(ivec2 p)", "wavyPixel(ivec2(coord))", "any pixel of the picture coming in; clear beyond it"},
        {"wavySample(vec2 p)", "wavySample(coord)", "the picture between pixels (p in raster pixels); clear beyond it"},
        {"wavySampleEdge(vec2 p)", "wavySampleEdge(coord)", "the same, the edge held instead"},
        {"wavyOriginal(ivec2 p)", "wavyOriginal(ivec2(coord))", "in a later pass, the picture as the first pass had it"},
        {"wavyRandom(vec2 p, float seed)", "wavyRandom(coord, 1.0)", "0..1, the same for the same pixel and seed, on the card and here"},
        {"wavyNoise(vec2 p, float seed)", "wavyNoise(coord / 64.0, 1.0)", "smooth noise, 0..1"},
        {"wavyLuma(vec3 c)", "wavyLuma(colour.rgb)", "brightness"},
        {"wavyHash(uint x)", "wavyHash(uint(coord.x))", "a well-mixed number"},
        {"wavyPremultiply(vec4 c)", "wavyPremultiply(colour)", ""},
        {"wavyUnpremultiply(vec4 c)", "wavyUnpremultiply(colour)", ""},
        {"wavySeconds", "wavySeconds", "the layer's own time"},
        {"wavyFrame", "wavyFrame", ""},
        {"wavyScale", "wavyScale", "raster pixels per layer pixel: multiply sizes given in layer pixels by it"},
        {"wavyBox", "wavyBox", "the layer's content in the raster: x, y, width, height"},
        {"wavyPass, wavyPasses", "wavyPass", "which run this is, of how many (\"passes\")"}};
    for (const Row &h : helpers)
      html += "<tr><td>" + link(h.shown, h.inserted) + "</td><td class=note>" + QString(h.note).toHtmlEscaped() + "</td></tr>";
    html += "</table>";
    html += "<h3>The description</h3><table>";
    static const Row keys[] = {
        {"\"perPixel\": true", "\"perPixel\": true,", "each pixel depends only on itself, where it is and the values"},
        {"\"movesAlone\": true", "\"movesAlone\": true,", "it changes with time while no value does"},
        {"\"wholeLayer\": true", "\"wholeLayer\": true,", "any pixel may come from anywhere in the layer"},
        {"\"inert\"", "\"inert\": \"param_amount <= 0.0\",", "when it changes nothing (skipped)"},
        {"\"varies\"", "\"varies\": \"param_speed != 0.0\",", "with movesAlone: when it moves"},
        {"\"reach\"", "\"reach\": \"param_radius\",", "how far, in layer pixels, it carries a pixel"},
        {"\"passes\"", "\"passes\": \"2\",", "how many times the shader runs in a row"},
        {"\"controls\"", "\"controls\": {\"amount\": \"param_on ? WAVY_SHOWN : WAVY_DIMMED\"},", "which controls are shown, dimmed or hidden"}};
    for (const Row &k : keys)
      html += "<tr><td>" + link(k.shown, k.inserted) + "</td><td class=note>" + QString(k.note).toHtmlEscaped() + "</td></tr>";
    html += "</table><p class=note>Every parameter can have <code>help</code>, <code>section</code> and <code>still</code> "
            "(no stopwatch). Kinds: float, angle, bool, choice, color, curve, point, point3d, group / end.</p>";
    html += "<h3>The same picture everywhere</h3><p class=note>Use wavyRandom and wavyNoise rather than "
            "<code>fract(sin(...) * 43758.5)</code>: the card's sin is not the processor's. The processor does not take "
            "structs, switch, dFdx / dFdy / fwidth, or texture functions besides texelFetch, texture and textureSize.</p>";
    const int scrolled = guide->verticalScrollBar()->value();
    guide->setHtml(html);
    guide->verticalScrollBar()->setValue(scrolled);
  }
};

// ---- the watched folder ----------------------------------------------------------------------
struct Watch {
  QPointer<QObject> owner; // set by watchUserFolder; the watching starts once the folder is there
  QPointer<QFileSystemWatcher> watcher;
  QPointer<QTimer> settle;
  QSet<QString> known;   // the effects in the folder, as last seen
  QSet<QString> pending; // files said to have changed
};
Watch &watch() {
  static Watch w;
  return w;
}
QStringList folderEffects(const QString &folder) {
  QStringList files;
  const QDir dir(folder);
  for (const QString &name : dir.entryList({"*.wfxs"}, QDir::Files, QDir::Name)) files << canonical(dir.filePath(name));
  return files;
}
void settleWatch() {
  Watch &w = watch();
  if (!w.watcher) return;
  QStringList ids;
  QStringList problems;
  const QStringList now = folderEffects(userEffectsFolder());
  const QSet<QString> present(now.begin(), now.end());
  auto readIn = [&](const QString &file) {
    const QByteArray bytes = fileBytes(file);
    if (installed().contains(file) && installed().value(file) == bytes) return;
    if (EffectWindow *window = windows().value(file)) {
      window->changedOnDisk(bytes);
      if (window->dirty()) return;
    }
    const QStringList changed = reloadTextEffect(file);
    if (!changed.isEmpty()) {
      installed()[file] = bytes;
      ids << changed;
    } else if (const QString why = textEffectProblem(file); !why.isEmpty()) {
      problems << QFileInfo(file).fileName() + ": " + why;
    }
  };
  for (const QString &file : now)
    if (!w.known.contains(file)) {
      w.known.insert(file);
      w.watcher->addPath(file);
      readIn(file);
    }
  for (const QString &file : QSet<QString>(w.known))
    if (!present.contains(file)) {
      w.known.remove(file);
      installed().remove(file);
      ids << reloadTextEffect(file);
    }
  for (const QString &changed : QSet<QString>(w.pending)) {
    const QString file = canonical(changed);
    if (changed.isEmpty() || !present.contains(file)) continue;
    // Programs that save by replacing the file take the watch on it with them.
    if (!w.watcher->files().contains(changed)) w.watcher->addPath(file);
    readIn(file);
  }
  w.pending.clear();
  tell(ids);
  if (!problems.isEmpty()) say("Effect not loaded: " + problems.join("; "), 8000);
  else if (!ids.isEmpty()) say("Effects read again: " + QStringList(ids).join(", "), 4000);
}
}  // namespace

void setHost(Host h) { host() = std::move(h); }

bool isUsersOwn(const QString &file) {
  const QString folder = canonical(userEffectsFolder());
  return canonical(file).startsWith(folder + "/", Qt::CaseInsensitive);
}

void open(QWidget *parent, const QString &file) {
  const QString path = canonical(file);
  if (EffectWindow *window = windows().value(path)) {
    window->show();
    window->raise();
    window->activateWindow();
    return;
  }
  auto *window = new EffectWindow(parent, path);
  windows().insert(path, window);
  window->show();
  window->raise();
  window->activateWindow();
}

QStringList starterNames() {
  QStringList names;
  for (const auto &s : starters()) names << s.name;
  return names;
}

QString starterText(int index, const QString &id, const QString &name, const QString &category) {
  const auto &list = starters();
  QString text = QString::fromUtf8(list[size_t(std::clamp(index, 0, int(list.size()) - 1))].text);
  text.replace("{{ID}}", jsonString(id));
  text.replace("{{NAME}}", jsonString(name));
  text.replace("{{CATEGORY}}", jsonString(category));
  return text;
}

QString withParameter(const QString &text, const QString &kind, QString *made) {
  int start = 0;
  const int close = descriptionEnd(text, &start);
  if (close < 0) return {};
  const int open = text.indexOf('{', start);
  if (open < 0 || open > close) return {};
  QSet<QString> used;
  static const QRegularExpression ids("\"id\"\\s*:\\s*\"([^\"]*)\"");
  for (auto it = ids.globalMatch(text.left(close)); it.hasNext();) used.insert(it.next().captured(1));
  struct Kind { const char *kind, *id, *label, *entry; };
  static const Kind table[] = {
      {"float", "amount", "Amount", R"({"id": "%1", "label": "%2", "min": 0, "max": 100, "default": 50})"},
      {"angle", "angle", "Angle", R"({"id": "%1", "kind": "angle", "label": "%2", "default": 0})"},
      {"bool", "enabled", "Enabled", R"({"id": "%1", "kind": "bool", "label": "%2", "default": true})"},
      {"choice", "mode", "Mode", R"({"id": "%1", "kind": "choice", "label": "%2", "choices": ["Soft", "Hard"], "default": 0})"},
      {"color", "colour", "Colour", R"({"id": "%1", "kind": "color", "label": "%2", "default": "#ffffff"})"},
      {"curve", "response", "Response", R"({"id": "%1", "kind": "curve", "label": "%2"})"},
      {"point", "centre", "Centre", R"({"id": "%1", "kind": "point", "label": "%2", "default": [0, 0]})"},
      {"group", "", "Group", R"({"kind": "group", "label": "%2"}, {"kind": "end"})"}};
  const Kind *chosen = &table[0];
  for (const Kind &k : table)
    if (kind == QLatin1String(k.kind)) chosen = &k;
  QString id = QString::fromUtf8(chosen->id), label = QString::fromUtf8(chosen->label);
  if (!id.isEmpty())
    for (int n = 2; used.contains(id); ++n) {
      id = QString::fromUtf8(chosen->id) + QString::number(n);
      label = QString::fromUtf8(chosen->label) + " " + QString::number(n);
    }
  // Not arg(): a group has no %1, and arg() fills the lowest marker there is.
  const QString entry = QString::fromUtf8(chosen->entry).replace("%1", id).replace("%2", label);
  if (made) *made = id;
  static const QRegularExpression key("\"parameters\"\\s*:\\s*\\[");
  const auto match = key.match(text, open);
  if (!match.hasMatch() || match.capturedStart() > close)
    return text.left(open + 1) + "\n  \"parameters\": [\n    " + entry + "\n  ]," + text.mid(open + 1);
  // The ] that closes the list: brackets inside strings do not count.
  int depth = 0, end = -1;
  for (int i = int(match.capturedEnd()) - 1; i < close && end < 0; ++i) {
    const QChar c = text[i];
    if (c == '"') {
      ++i;
      while (i < close && text[i] != '"') i += text[i] == '\\' ? 2 : 1;
    } else if (c == '[' || c == '{') {
      ++depth;
    } else if ((c == ']' || c == '}') && --depth == 0) {
      end = i;
    }
  }
  if (end < 0) return {};
  int last = end - 1;
  while (last >= match.capturedEnd() && text[last].isSpace()) --last;
  const bool empty = last < match.capturedEnd();
  const int lineStart = int(text.lastIndexOf('\n', match.capturedStart())) + 1;
  QString indent;
  for (int i = lineStart; i < text.size() && (text[i] == ' ' || text[i] == '\t'); ++i) indent += text[i];
  if (empty) return text.left(match.capturedEnd()) + "\n" + indent + "  " + entry + "\n" + indent + text.mid(end);
  return text.left(last + 1) + ",\n" + indent + "  " + entry + text.mid(last + 1);
}

int problemLine(const QString &text, const QString &problem) {
  static const QRegularExpression line("\\bline (\\d+)");
  if (const auto m = line.match(problem); m.hasMatch()) return m.captured(1).toInt();
  // A parameter named in it: the line its id is on.
  static const QRegularExpression word("\"?([A-Za-z_][A-Za-z0-9_]*)\"?");
  for (auto it = word.globalMatch(problem); it.hasNext();) {
    const QString name = it.next().captured(1);
    const QRegularExpression at("\"id\"\\s*:\\s*\"" + QRegularExpression::escape(name) + "\"");
    if (const auto m = at.match(text); m.hasMatch()) return int(text.left(m.capturedStart()).count('\n')) + 1;
  }
  return 0;
}

QString createNew(QWidget *parent, const QString &category) {
  QDialog dialog(parent);
  dialog.setWindowTitle("New Effect");
  auto *layout = new QVBoxLayout(&dialog);
  auto *intro = new QLabel("An effect is one text file: a description of its controls, then a shader. Wavy-Flo runs the "
                           "shader on the graphics card, and the same text on every core of the processor where there is "
                           "none -- nothing to build, nothing to make fast.");
  intro->setWordWrap(true);
  intro->setStyleSheet("color: #A9AEBC;");
  layout->addWidget(intro);
  auto *form = new QFormLayout;
  auto *name = new QLineEdit(freeId("My Effect"));
  name->selectAll();
  form->addRow("Name", name);
  auto *group = new QComboBox;
  group->setEditable(true);
  QStringList categories{"Color Correction", "Stylize", "Blur & Sharpen", "Noise & Distort", "Generate", "Transition"};
  for (const PackEffect *e : packEffects())
    if (!categories.contains(e->category)) categories << e->category;
  group->addItems(categories);
  group->setCurrentText(!category.isEmpty() ? category : QSettings().value("effectEditor/category", "Stylize").toString());
  form->addRow("Category", group);
  auto *start = new QComboBox;
  start->addItems(starterNames());
  start->setCurrentIndex(std::clamp(QSettings().value("effectEditor/starter", 0).toInt(), 0, int(starters().size()) - 1));
  form->addRow("Start from", start);
  auto *apply = new QCheckBox("Put it on the selected layer");
  apply->setChecked(true);
  form->addRow("", apply);
  layout->addLayout(form);
  auto *where = new QLabel("Saved in " + QDir::toNativeSeparators(userEffectsFolder()));
  where->setStyleSheet("color: #7E8496;");
  layout->addWidget(where);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  buttons->button(QDialogButtonBox::Ok)->setText("Create");
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  dialog.resize(520, dialog.sizeHint().height());
  if (dialog.exec() != QDialog::Accepted) return {};
  QSettings().setValue("effectEditor/category", group->currentText());
  QSettings().setValue("effectEditor/starter", start->currentIndex());
  const QString id = freeId(name->text());
  const QString text = starterText(start->currentIndex(), id, id, group->currentText().trimmed().isEmpty() ? QString("Plugins") : group->currentText().trimmed());
  QDir().mkpath(userEffectsFolder());
  const QString file = QDir(userEffectsFolder()).filePath(fileNameFor(id) + ".wfxs");
  QSaveFile out(file);
  const QByteArray bytes = text.toUtf8();
  if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
    QMessageBox::warning(parent, "New Effect", "Could not write " + QDir::toNativeSeparators(file) + ": " + out.errorString());
    return {};
  }
  const QString path = canonical(file);
  const QStringList ids = reloadTextEffect(path);
  installed()[path] = bytes;
  watchUserFolder(nullptr);
  if (watch().watcher) {
    watch().known.insert(path);
    watch().watcher->addPath(path);
  }
  tell(ids);
  open(parent, path);
  if (apply->isChecked() && !ids.isEmpty() && host().applyToSelected) {
    const QString why = host().applyToSelected(ids.front());
    if (!why.isEmpty()) say(why, 4000);
  }
  return path;
}

QString duplicate(QWidget *parent, const QString &file) {
  QString text = QString::fromUtf8(fileBytes(file));
  const auto idSpan = topLevelString(text, "id");
  if (idSpan.first < 0) {
    QMessageBox::warning(parent, "Duplicate Effect", "Its description has no id to give the copy a new one.");
    return {};
  }
  const QString oldId = QJsonDocument::fromJson(("[" + text.mid(idSpan.first, idSpan.second) + "]").toUtf8()).array().at(0).toString();
  const QString id = freeId(oldId + " Copy");
  // From the end, so the first span stays where it was found.
  const auto nameSpan = topLevelString(text, "name");
  if (nameSpan.first > idSpan.first) text.replace(nameSpan.first, nameSpan.second, jsonString(id));
  text.replace(idSpan.first, idSpan.second, jsonString(id));
  if (nameSpan.first >= 0 && nameSpan.first < idSpan.first) text.replace(nameSpan.first, nameSpan.second, jsonString(id));
  QDir().mkpath(userEffectsFolder());
  const QString target = QDir(userEffectsFolder()).filePath(fileNameFor(id) + ".wfxs");
  QSaveFile out(target);
  const QByteArray bytes = text.toUtf8();
  if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
    QMessageBox::warning(parent, "Duplicate Effect", "Could not write " + QDir::toNativeSeparators(target) + ": " + out.errorString());
    return {};
  }
  const QString path = canonical(target);
  const QStringList ids = reloadTextEffect(path);
  installed()[path] = bytes;
  watchUserFolder(nullptr);
  if (watch().watcher) {
    watch().known.insert(path);
    watch().watcher->addPath(path);
  }
  tell(ids);
  open(parent, path);
  say(QString("Copied as \"%1\" into your effects").arg(id), 4000);
  return path;
}

bool closeAll() {
  for (const auto &window : windows().values())
    if (window && !window->settle()) return false;
  return true;
}

void watchUserFolder(QObject *owner) {
  Watch &w = watch();
  if (owner) w.owner = owner;
  // Not made for this: a folder appears in someone's Documents when they make
  // an effect, or ask to see where they would go.
  // A self-test watches only a folder it was given, never someone's Documents.
  const QString folder = userEffectsFolder();
  const bool scratch = !qEnvironmentVariable("WAVY_EFFECTS_FOLDER").isEmpty();
  if (w.watcher || !w.owner || (selfTesting() && !scratch) || !QFileInfo(folder).isDir()) return;
  auto *watcher = new QFileSystemWatcher(w.owner);
  w.watcher = watcher;
  watcher->addPath(folder);
  for (const QString &file : folderEffects(folder)) {
    w.known.insert(file);
    watcher->addPath(file);
    installed()[file] = fileBytes(file);
  }
  auto *settle = new QTimer(watcher);
  w.settle = settle;
  settle->setSingleShot(true);
  settle->setInterval(300);
  QObject::connect(settle, &QTimer::timeout, watcher, [] { settleWatch(); });
  QObject::connect(watcher, &QFileSystemWatcher::fileChanged, watcher, [](const QString &file) {
    watch().pending.insert(file);
    if (watch().settle) watch().settle->start();
  });
  QObject::connect(watcher, &QFileSystemWatcher::directoryChanged, watcher, [](const QString &) {
    if (watch().settle) watch().settle->start();
  });
}

void showProblems(QWidget *parent) {
  auto *dialog = new QDialog(parent);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle("Effects That Did Not Load");
  dialog->resize(760, 380);
  auto *layout = new QVBoxLayout(dialog);
  const QStringList packs = packProblems();
  const QMap<QString, QString> texts = textEffectProblems();
  auto *intro = new QLabel(packs.isEmpty() ? QString("Every effect found loaded.")
                                           : QString("These were found but not loaded. An effect written as text opens "
                                                     "with a double-click, at the line that is wrong."));
  intro->setWordWrap(true);
  layout->addWidget(intro);
  auto *list = new QListWidget;
  for (const QString &problem : packs) {
    auto *item = new QListWidgetItem(problem, list);
    // Which of them is a file that opens.
    for (auto it = texts.constBegin(); it != texts.constEnd(); ++it)
      if (problem == QFileInfo(it.key()).fileName() + ": " + it.value()) item->setData(Qt::UserRole, it.key());
    item->setToolTip(problem);
  }
  QObject::connect(list, &QListWidget::itemDoubleClicked, dialog, [parent, dialog](QListWidgetItem *item) {
    const QString file = item->data(Qt::UserRole).toString();
    if (file.isEmpty()) return;
    open(parent, file);
    dialog->close();
  });
  layout->addWidget(list, 1);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  auto *again = buttons->addButton("Read Effects Again", QDialogButtonBox::ActionRole);
  QObject::connect(again, &QPushButton::clicked, dialog, [dialog, parent] {
    readAgain();
    dialog->close();
    if (!packProblems().isEmpty()) showProblems(parent);
  });
  QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
  layout->addWidget(buttons);
  dialog->show();
}

void openFolder() {
  QDir().mkpath(userEffectsFolder());
  watchUserFolder(nullptr);
  QDesktopServices::openUrl(QUrl::fromLocalFile(userEffectsFolder()));
}

void reveal(const QString &file) {
#ifdef Q_OS_WIN
  QProcess::startDetached("explorer.exe", {"/select,", QDir::toNativeSeparators(file)});
#else
  QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(file).absolutePath()));
#endif
}

int checkWindow(QStringList &report) {
  int failures = 0;
  auto check = [&](bool passed, const QString &label) {
    report << label + (passed ? ": PASS" : ": FAIL");
    failures += !passed;
  };
  QTemporaryDir folder;
  if (!folder.isValid()) {
    report << "SKIP: no temporary folder for the effect editor";
    return 0;
  }
  const QByteArray wasFolder = qgetenv("WAVY_EFFECTS_FOLDER");
  qputenv("WAVY_EFFECTS_FOLDER", folder.path().toUtf8());
  const Host wasHost = host();
  QStringList told;
  host() = {[&](const QStringList &ids) { told << ids; }, [](const QString &) { return QString(); }, [](const QString &, int) {}};
  auto wait = [](int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
  };
  // The colour an effect turns a grey pixel.
  auto drawn = [](const PackEffect *e) {
    QImage pixel(4, 4, QImage::Format_RGBA32FPx4);
    pixel.fill(QColor(128, 128, 128));
    Layer none;
    if (e) runPackEffect(pixel, *e, packEffectValues(none, *e, 0), {0, 0, 4, 4}, 1, 0, 0);
    return pixel.pixelColor(1, 1);
  };
  const QString file = QDir(folder.path()).filePath("Window Test.wfxs");
  {
    QFile f(file);
    if (f.open(QIODevice::WriteOnly)) f.write(starterText(4, "Window Test", "Window Test", "Stylize").toUtf8());
  }
  reloadTextEffect(file); // as New Effect reads it in
  open(nullptr, file);
  EffectWindow *window = windows().value(canonical(file));
  auto *source = window ? window->findChild<QPlainTextEdit *>("effectSource") : nullptr;
  auto *message = window ? window->findChild<QLabel *>("effectEditorMessage") : nullptr;
  check(window && source && message && isUsersOwn(file) && !source->isReadOnly() && packEffect("Window Test"),
        "an effect of one's own opens in the effect editor to be changed");
  if (window && source && message) {
    // Typed: checked after a pause and, with Live on, in use at once.
    QString text = source->toPlainText();
    text.replace("vec4 result = colour;", "vec4 result = vec4(1.0, 0.0, 0.0, colour.a);");
    source->setPlainText(text);
    wait(800);
    const PackEffect *live = packEffect("Window Test");
    check(told.contains("Window Test") && drawn(live).redF() > .9 && drawn(live).greenF() < .1 && window->dirty() &&
              message->text().contains("Compiles"),
          "typed text is checked and, with Live, drawn before it is saved: " + message->toolTip());
    // A mistake: said at its line; the version before stays in use.
    told.clear();
    QString wrong = text;
    wrong.replace("param_amount", "param_amont");
    source->setPlainText(wrong);
    wait(800);
    check(message->text().contains("line") && packEffect("Window Test") == live && told.isEmpty(),
          "a mistake is shown at its line, and the last version that worked stays in the preview: " + message->toolTip());
    source->setPlainText(text);
    wait(800);
    check(window->save() && fileBytes(file) == text.toUtf8() && !window->dirty(), "Save writes it");
    // Changed by another program while nothing here is unsaved: the window shows it.
    QString elsewhere = text;
    elsewhere.replace("vec4(1.0, 0.0, 0.0, colour.a)", "vec4(0.0, 0.0, 1.0, colour.a)");
    window->changedOnDisk(elsewhere.toUtf8());
    check(source->toPlainText() == elsewhere && !window->dirty(), "a change made by another program shows in the window");
    window->close();
    wait(50);
    check(!windows().value(canonical(file)), "and the window closes without a question, nothing being unsaved");
  }
  host() = wasHost;
  if (wasFolder.isEmpty()) qunsetenv("WAVY_EFFECTS_FOLDER");
  else qputenv("WAVY_EFFECTS_FOLDER", wasFolder);
  QFile::remove(file);
  reloadTextEffect(file);
  return failures;
}

QStringList readAgain() {
  QStringList ids, problems;
  QStringList files = folderEffects(QCoreApplication::applicationDirPath() + "/effects");
  if (!selfTesting()) files << folderEffects(userEffectsFolder());
  for (const QString &file : files) {
    if (EffectWindow *window = windows().value(file); window && window->dirty()) continue;
    const QByteArray bytes = fileBytes(file);
    const QStringList changed = reloadTextEffect(file);
    if (!changed.isEmpty()) {
      installed()[file] = bytes;
      ids << changed;
    } else if (const QString why = textEffectProblem(file); !why.isEmpty()) {
      problems << QFileInfo(file).fileName() + ": " + why;
    }
  }
  tell(ids);
  if (!problems.isEmpty()) say("Effect not loaded: " + problems.join("; "), 8000);
  else say(QString("%1 effect(s) read again").arg(ids.size()), 4000);
  return ids;
}
}  // namespace effect_editor
