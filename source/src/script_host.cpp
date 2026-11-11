#include "script_host.h"
#include "character_animation.h"
#include "expression_controls.h"
#include "expressions.h"
#include "tracker.h"
#include "color_picker.h"
#include "jsxbin.h"
#include "render_queue.h"
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QJSEngine>
#include <QJSValue>
#include <QJSValueIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QStatusBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QStringEncoder>
#include <QSysInfo>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <cmath>
#include <map>
#include <thread>

// ---------------------------------------------------------------------------
// Reading a script: ExtendScript's directives, worked out before it runs.
//   #include "file"  //@include "file"   the file's text, there
//   #includepath "a;b"                   where else #include looks
//   #target, #targetengine, #script, #strict, #engine   read and passed over
// Every line of the text that runs remembers the file and line it came from,
// so an error is reported where it was written.
// A compiled script (.jsxbin, or eval("@JSXBIN@...") in a plain one) is read
// back into the script it was made from (jsxbin.h) and runs as that.
// ---------------------------------------------------------------------------
namespace {
struct Origin {
  QString file;
  int line = 0;
};

QString unquote(QString s) {
  s = s.trimmed();
  if (s.endsWith(';')) s.chop(1);
  s = s.trimmed();
  if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) s = s.mid(1, s.size() - 2);
  return s;
}

// A path as ExtendScript writes one -- "/c/Users/...", "~/Documents", or
// Windows' own -- as this machine names it.
QString nativePath(QString path) {
  path = path.trimmed();
  if (path.startsWith("~/") || path == "~") path = QDir::homePath() + path.mid(1);
  static const QRegularExpression drive("^/([a-zA-Z])(/|$)");
  if (const auto m = drive.match(path); m.hasMatch()) path = m.captured(1).toUpper() + ":/" + path.mid(3);
  if (path.startsWith("file:///")) path = QUrl(path).toLocalFile();
  path = QUrl::fromPercentEncoding(path.toUtf8());
  return QDir::cleanPath(path);
}

bool preprocess(const QString &file, const QString &text, QStringList includePath, QSet<QString> &seen,
                QString &out, std::vector<Origin> &origins, QString *error, int depth = 0) {
  if (depth > 32) {
    if (error) *error = "#include nested too deeply at " + file;
    return false;
  }
  if (isJsxbin(text)) {
    const JsxbinScript compiled = decodeJsxbin(text);
    if (!compiled.ok()) {
      if (error) *error = QFileInfo(file).fileName() + " could not be read as a compiled script: " + compiled.error;
      return false;
    }
    const QStringList lines = compiled.code.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
      out += lines[i] + '\n';
      origins.push_back({file, compiled.lines[size_t(i)]});
    }
    return true;
  }
  const QString folder = QFileInfo(file).absolutePath();
  const QStringList lines = inlineJsxbinEvals(text).split('\n');
  for (int i = 0; i < lines.size(); ++i) {
    QString line = lines[i];
    if (line.endsWith('\r')) line.chop(1);
    const QString trimmed = line.trimmed();
    QString directive, argument;
    for (const QString prefix : {QStringLiteral("#"), QStringLiteral("//@")}) {
      if (!trimmed.startsWith(prefix)) continue;
      const QString rest = trimmed.mid(prefix.size());
      const int space = rest.indexOf(QRegularExpression("\\s"));
      directive = (space < 0 ? rest : rest.left(space)).toLower();
      argument = space < 0 ? QString() : rest.mid(space + 1);
      break;
    }
    static const QSet<QString> known{"include", "includepath", "target", "targetengine", "script", "strict", "engine"};
    if (!known.contains(directive)) {
      out += line + '\n';
      origins.push_back({file, i + 1});
      continue;
    }
    if (directive == "includepath") {
      for (const QString &part : unquote(argument).split(';', Qt::SkipEmptyParts)) {
        const QString p = nativePath(part);
        includePath.prepend(QFileInfo(p).isAbsolute() ? p : QDir(folder).filePath(p));
      }
    } else if (directive == "include") {
      const QString wanted = nativePath(unquote(argument));
      QString found;
      QStringList where{folder};
      where += includePath;
      if (QFileInfo(wanted).isAbsolute()) found = QFileInfo::exists(wanted) ? wanted : QString();
      else
        for (const QString &base : where)
          if (QFileInfo::exists(QDir(base).filePath(wanted))) {
            found = QDir(base).filePath(wanted);
            break;
          }
      if (found.isEmpty()) {
        if (error) *error = QString("%1, line %2: #include \"%3\" was not found").arg(QFileInfo(file).fileName()).arg(i + 1).arg(wanted);
        return false;
      }
      found = QFileInfo(found).canonicalFilePath();
      if (!seen.contains(found)) {
        seen.insert(found);
        QFile f(found);
        if (!f.open(QIODevice::ReadOnly)) {
          if (error) *error = "cannot read " + found;
          return false;
        }
        if (!preprocess(found, QString::fromUtf8(f.readAll()), includePath, seen, out, origins, error, depth + 1)) return false;
      }
    }
    // The directive's own line stays, empty, so the lines after it keep their numbers.
    out += '\n';
    origins.push_back({file, i + 1});
  }
  return true;
}

// ---------------------------------------------------------------------------
// The scene, reached by layer id.
// ---------------------------------------------------------------------------
QVariantList colorList(const QColor &c) { return {c.redF(), c.greenF(), c.blueF(), c.alphaF()}; }
QColor colorFrom(const QVariant &v) {
  if (v.typeId() == QMetaType::QString) return QColor(v.toString());
  const QVariantList l = v.toList();
  if (l.size() < 3) return {};
  QColor c;
  c.setRgbF(float(std::clamp(l[0].toDouble(), 0., 1.)), float(std::clamp(l[1].toDouble(), 0., 1.)),
            float(std::clamp(l[2].toDouble(), 0., 1.)), float(l.size() > 3 ? std::clamp(l[3].toDouble(), 0., 1.) : 1.));
  return c;
}

// A value deep-merged into another: objects field by field, anything else replaced.
QJsonValue merged(const QJsonValue &into, const QJsonValue &patch) {
  if (!into.isObject() || !patch.isObject()) return patch;
  QJsonObject out = into.toObject();
  const QJsonObject p = patch.toObject();
  for (auto it = p.begin(); it != p.end(); ++it) out[it.key()] = merged(out.value(it.key()), it.value());
  return out;
}
} // namespace

// ---------------------------------------------------------------------------
// One script's world: its engine, the native bridge it calls (exposed as
// __wf), and the windows it made.
// ---------------------------------------------------------------------------
class ScriptBridge;
class ScriptSession {
public:
  ScriptHost host;
  QJSEngine engine;
  ScriptBridge *bridge = nullptr;
  bool unattended = false;
  QStringList output;
  std::vector<Origin> origins;
  int origin = 0; // the line in `origins` the text being run starts at
  // Changed the scene during this entry from outside (a run, a click, a timer).
  int depth = 0;
  bool touched = false;
  // The windows: by id, with what each is.
  struct Ui {
    QPointer<QWidget> widget;
    QString type;
    int parent = 0;
    QBoxLayout *layout = nullptr; // a container's
    QGridLayout *stack = nullptr; // a "stack" container's
    QString orientation;
    QString alignH = "center", alignV = "top"; // alignChildren
    QString ownH, ownV;                         // its own alignment, if set
    bool placed = false;                        // laid at bounds of its own, outside the layout
    double minimum = 0, maximum = 100;          // a slider's or a progress bar's range
    int result = 0;                             // a dialog's, set by close()
    bool external = false;                      // a docked panel the editor gave
    bool dock = false;                          // ... made for the script (wavy.ui.panel): goes with it
  };
  std::map<int, Ui> ui;
  int nextUi = 1;
  QList<QPointer<QWidget>> windows;
  std::map<int, QTimer *> timers;
  int nextTimer = 1;
  // wavy.on: the events listened for, and what was last seen of the editor.
  QSet<QString> listening;
  double seenFrame = 0;
  QStringList seenSelection;
  bool seenPlaying = false;
  quint64 seenRevision = 0;
  // wavy.app.addCommand: the menu items the script put in, and the menus it
  // made to hold them.
  std::map<int, QPointer<QAction>> commands;
  QList<QPointer<QMenu>> madeMenus;
  int nextCommand = 1;
  // This entry's Undo step is taken (wavy.undoGroup starts another).
  bool stepped = false;
  // wavy.image: pictures the script holds, by id.
  std::map<int, QImage> images;
  int nextImage = 1;

  explicit ScriptSession(ScriptHost h);
  ~ScriptSession();

  // Entered from outside: one step for Undo, and the editor brought up to
  // date when it is done -- if anything changed.
  struct Entry {
    ScriptSession &s;
    explicit Entry(ScriptSession &session) : s(session) {
      if (s.depth++ == 0) s.touched = s.stepped = false;
    }
    ~Entry() {
      if (--s.depth > 0) return;
      if (s.touched && s.host.refresh) s.host.refresh();
      // What it did itself is not news to it.
      if (s.host.revision) s.seenRevision = s.host.revision();
    }
  };
  void change() {
    touched = true;
    if (stepped) return;
    stepped = true;
    if (host.checkpoint) host.checkpoint();
  }
  void emitEvent(const QString &event, const QVariant &data);
  void poll();
  void endAll();
  Scene &scene() { return host.scene(); }
  QString located(const QJSValue &error) const;
  bool evaluate(const QString &code, const QString &fileName, QString *error, const QJSValue &thisObject = {});
  void fail(const QString &message) { engine.throwError(QJSValue::GenericError, message); }
  void reportCallbackError(const QJSValue &result);
  void dispatch(int id, const QString &event, const QVariant &argument = {});
};

// Where an error happened, in the file and line it was written on.
QString ScriptSession::located(const QJSValue &error) const {
  int line = error.property("lineNumber").toInt();
  // Thrown inside the After Effects or Wavy-Flo layer (":/scripting/..."):
  // placed where the script called it, as After Effects places its own.
  auto inside = [](const QString &file) { return file.startsWith("qrc:/scripting/") || file.startsWith(":/scripting/"); };
  if (inside(error.property("fileName").toString())) {
    static const QRegularExpression frame("@(.*):(\\d+)$");
    for (const QString &entry : error.property("stack").toString().split('\n')) {
      const auto m = frame.match(entry.trimmed());
      if (m.hasMatch() && !inside(m.captured(1))) {
        line = m.captured(2).toInt();
        break;
      }
    }
  }
  QString where;
  const int at = origin + line - 1;
  if (line > 0 && at >= 0 && at < int(origins.size()))
    where = QString("%1, line %2: ").arg(QFileInfo(origins[size_t(at)].file).fileName()).arg(origins[size_t(at)].line);
  else if (line > 0)
    where = QString("line %1: ").arg(line);
  QString message = error.property("message").toString();
  if (message.isEmpty()) message = error.toString();
  return where + message;
}

bool ScriptSession::evaluate(const QString &code, const QString &fileName, QString *error, const QJSValue &thisObject) {
  QJSValue result;
  if (thisObject.isUndefined()) {
    result = engine.evaluate(code, fileName, 1);
  } else {
    // A panel's script runs with `this` its Panel: as a function called on it.
    // The opening line goes first, so the script's lines keep their numbers.
    QJSValue function = engine.evaluate("(function(){" + code + "\n})", fileName, 1);
    result = function.isError() ? function : function.callWithInstance(thisObject);
  }
  if (result.isError()) {
    if (error) *error = located(result);
    return false;
  }
  return true;
}

void ScriptSession::reportCallbackError(const QJSValue &result) {
  if (!result.isError()) return;
  const QString text = located(result);
  output << "Error: " + text;
  if (host.log) host.log("Script error: " + text);
  if (!unattended) QMessageBox::warning(host.window, "Script", text);
}

// ---------------------------------------------------------------------------
// The editor's menus, as wavy.app reaches them: "File/Save Project".
// ---------------------------------------------------------------------------
namespace {
QString menuText(QString t) {
  t.remove('&');
  t = t.trimmed();
  if (t.endsWith("...")) t.chop(3);
  if (t.endsWith(QChar(0x2026))) t.chop(1);
  return t.trimmed();
}
void menuWalk(QMenu *menu, const QString &path, std::vector<std::pair<QString, QAction *>> &out) {
  for (QAction *a : menu->actions()) {
    if (a->isSeparator() || menuText(a->text()).isEmpty()) continue;
    const QString here = path + "/" + menuText(a->text());
    if (a->menu()) menuWalk(a->menu(), here, out);
    else out.push_back({here, a});
  }
}
// The editor's menu bar as it is. Never QMainWindow::menuBar(): the editor
// keeps its bar inside a widget of its own (setMenuWidget), and menuBar()
// would put a new, empty one in that widget's place.
QMenuBar *menuBarOf(QWidget *window) {
  auto *main = qobject_cast<QMainWindow *>(window);
  if (!main) return nullptr;
  if (auto *bar = qobject_cast<QMenuBar *>(main->menuWidget())) return bar;
  return main->menuWidget() ? main->menuWidget()->findChild<QMenuBar *>() : nullptr;
}
std::vector<std::pair<QString, QAction *>> menuActions(QWidget *window) {
  std::vector<std::pair<QString, QAction *>> out;
  QMenuBar *bar = menuBarOf(window);
  if (!bar) return out;
  for (QAction *top : bar->actions())
    if (top->menu()) menuWalk(top->menu(), menuText(top->text()), out);
  return out;
}
QMenu *childMenu(QMenu *in, const QString &title) {
  for (QAction *a : in->actions())
    if (a->menu() && menuText(a->text()).compare(title, Qt::CaseInsensitive) == 0) return a->menu();
  return nullptr;
}
QTimer *eventPoll(); // the few-times-a-second look at the editor (pollScriptEvents)
// A rendered frame over the composition's background, as the viewer shows it
// (the render itself keeps the background out, in its alpha).
QImage overBackground(const QImage &frame, const QColor &background) {
  QImage out(frame.size(), QImage::Format_ARGB32_Premultiplied);
  out.fill(background);
  QPainter p(&out);
  p.drawImage(0, 0, frame);
  p.end();
  return out;
}
// wavy.ui's colour: a swatch of it.
void setSwatch(QToolButton *swatch, const QColor &c) {
  swatch->setProperty("wfColor", c);
  swatch->setStyleSheet(QString("QToolButton{background:rgba(%1,%2,%3,%4);border:1px solid #3A3A3A;border-radius:2px;}")
                            .arg(c.red())
                            .arg(c.green())
                            .arg(c.blue())
                            .arg(c.alpha()));
  swatch->setToolTip(hexTextFromColor(c, c.alpha() < 255));
}
} // namespace

// ---------------------------------------------------------------------------
// The native bridge: what wavy.js, ae.js and scriptui.js are built on.
// Frames are the Main's (a layer's keys are told and taken in them, not in
// the layer's own); colours are [r, g, b, a] from 0 to 1.
// ---------------------------------------------------------------------------
class ScriptBridge : public QObject {
  Q_OBJECT
public:
  explicit ScriptBridge(ScriptSession &s) : s(s) {}

  // --- The project -----------------------------------------------------
  Q_INVOKABLE QVariantMap project() {
    const Scene &sc = s.scene();
    return QVariantMap{{"width", sc.width},
                       {"height", sc.height},
                       {"fps", sc.fps},
                       {"frames", sc.frames},
                       {"frame", sc.frame},
                       {"depth", sc.processingDepth},
                       {"background", colorList(sc.background)},
                       {"linearLight", sc.linearLight},
                       {"motionBlur", sc.motionBlur},
                       {"playStart", sc.playStart},
                       {"playEnd", sc.playEnd},
                       {"mainName", s.host.mainName ? s.host.mainName() : QString("Main 01")},
                       {"file", s.host.projectFile ? s.host.projectFile() : QString()},
                       {"active", s.host.activeComposition ? s.host.activeComposition() : QString()},
                       {"version", s.host.version}};
  }
  Q_INVOKABLE void setProject(const QString &key, const QVariant &value) {
    Scene &sc = s.scene();
    if (key == "frame") {
      // The playhead is not part of the work: no Undo step for it.
      sc.frame = std::clamp(std::round(value.toDouble()), 0., double(std::max(0, sc.frames - 1)));
      s.touched = true;
      return;
    }
    s.change();
    if (key == "width") sc.width = std::clamp(value.toInt(), 16, 7680);
    else if (key == "height") sc.height = std::clamp(value.toInt(), 16, 7680);
    else if (key == "frames") sc.frames = std::clamp(value.toInt(), 1, 30 * 60 * 120);
    else if (key == "depth") {
      const int bits = value.toInt();
      sc.processingDepth = bits == 32 ? 32 : bits == 16 ? 16 : 8;
    } else if (key == "background") sc.background = colorFrom(value);
    else if (key == "linearLight") sc.linearLight = value.toBool();
    else if (key == "motionBlur") sc.motionBlur = value.toBool();
    else if (key == "playStart") sc.playStart = value.toInt();
    else if (key == "playEnd") sc.playEnd = value.toInt();
    else s.fail("The project has no setting \"" + key + "\" a script can set");
  }
  // The Main ("") and every group, each a composition of its own.
  Q_INVOKABLE QVariantList compositions() {
    QVariantList out{QVariantMap{{"id", QString()}, {"name", project().value("mainName")}}};
    for (const Layer &l : s.scene().layers)
      if (l.type == "group") out << QVariantMap{{"id", l.id}, {"name", l.name}};
    return out;
  }
  Q_INVOKABLE void openComposition(const QString &id) {
    if (s.host.openComposition) s.host.openComposition(id);
  }
  // The layers directly in a composition, top first ("*": every layer).
  Q_INVOKABLE QStringList layerIds(const QString &composition) {
    QStringList ids;
    for (const Layer &l : s.scene().layers)
      if (composition == "*" || l.groupId == composition) ids << l.id;
    return ids;
  }
  Q_INVOKABLE QStringList selectedLayers() { return s.host.selectedLayers ? s.host.selectedLayers() : QStringList(); }
  Q_INVOKABLE void selectLayers(const QStringList &ids) {
    if (s.host.selectLayers) s.host.selectLayers(ids);
    s.touched = true;
  }

  // --- A layer ----------------------------------------------------------
  // The fields read and set by name; anything else through data/patch.
  Q_INVOKABLE QVariant layerGet(const QString &id, const QString &field) {
    const Layer *l = find(id);
    if (!l) return {};
    if (field == "index") {
      int at = 0;
      for (const Layer &other : s.scene().layers) {
        if (other.groupId != l->groupId) continue;
        ++at;
        if (other.id == l->id) return at;
      }
      return 0;
    }
    if (field == "name") return l->name;
    if (field == "type") return l->type;
    if (field == "shapeKind") return l->shapeKind;
    if (field == "composition") return l->groupId;
    if (field == "parent") return l->parentId;
    if (field == "start") return l->start;
    if (field == "duration") return l->duration;
    if (field == "visible") return l->visible;
    if (field == "isolated") return l->isolated;
    if (field == "locked") return l->locked;
    if (field == "threeD") return l->threeD;
    if (field == "blendMode") return l->blendMode;
    if (field == "motionBlur") return l->motionBlur;
    if (field == "color") return colorList(l->color);
    if (field == "labelColor") return l->labelColor.isValid() ? QVariant(colorList(l->labelColor)) : QVariant();
    if (field == "trackMatte") return l->trackMatteId;
    if (field == "trackMatteInverted") return l->trackMatteInverted;
    if (field == "audioEnabled") return l->audioEnabled;
    if (field == "path") return l->path;
    if (field == "text") return l->text;
    if (field == "font") return l->font;
    if (field == "fontStyle") return l->fontStyle;
    if (field == "fontSize") return l->fontSize;
    if (field == "tracking") return l->tracking;
    if (field == "leading") return l->leading;
    if (field == "align") return l->align;
    if (field == "fauxBold") return l->fauxBold;
    if (field == "fauxItalic") return l->fauxItalic;
    if (field == "bold") return l->bold;
    if (field == "capitals") return l->capitals;
    if (field == "paragraphWidth") return l->paragraphWidth;
    if (field == "strokeOnly") return l->textStrokeOnly;
    if (field == "strokeWidth") return l->strokeWidth;
    if (field == "strokeColor") return colorList(l->strokeColor);
    if (field == "strokes") {
      QVariantList out;
      for (const auto &stroke : l->textStrokes)
        out << QVariantMap{{"width", stroke.width}, {"color", colorList(stroke.color)}, {"mode", stroke.mode}, {"join", stroke.join}};
      return out;
    }
    if (field == "blur") return l->blur;
    if (field == "glow") return l->glow;
    if (field == "shadow") return l->shadow;
    if (field == "separatePosition") return l->separatePosition;
    if (field == "separateScale") return l->separateScale;
    if (field == "lockScale") return l->lockScale;
    if (field == "timeRemap") return l->timeRemap;
    if (field == "lightKind") return l->lightKind;
    if (field == "namedByHand") return l->namedByHand;
    const QJsonObject data = layerJson(*l);
    if (data.contains(field)) return data.value(field).toVariant();
    s.fail("A layer has no field \"" + field + "\"");
    return {};
  }
  Q_INVOKABLE void layerSet(const QString &id, const QString &field, const QVariant &value) {
    Layer *l = find(id);
    if (!l) return;
    s.change();
    if (field == "name") {
      l->name = value.toString();
      l->namedByHand = true;
    } else if (field == "start") {
      l->start = value.toDouble();
    } else if (field == "duration") {
      l->duration = std::max(1., value.toDouble());
    } else if (field == "visible") l->visible = value.toBool();
    else if (field == "isolated") l->isolated = value.toBool();
    else if (field == "locked") l->locked = value.toBool();
    else if (field == "threeD") {
      l->threeD = value.toBool();
      if (l->threeD && l->parentBind3D.isIdentity()) l->parentBind3D = QMatrix4x4(l->parentBind);
    } else if (field == "blendMode") {
      bool known = false;
      for (const auto &mode : layerBlendModes()) known = known || mode.first == value.toString();
      if (!known) return s.fail("There is no blend mode \"" + value.toString() + "\"");
      l->blendMode = value.toString();
    } else if (field == "motionBlur") l->motionBlur = value.toBool();
    else if (field == "color") l->color = colorFrom(value);
    else if (field == "labelColor") l->labelColor = value.isNull() ? QColor() : colorFrom(value);
    else if (field == "trackMatte") l->trackMatteId = value.toString();
    else if (field == "trackMatteInverted") l->trackMatteInverted = value.toBool();
    else if (field == "audioEnabled") l->audioEnabled = value.toBool();
    else if (field == "text") {
      l->text = value.toString();
      // Written plainly, the words take the layer's own styling throughout.
      l->richHtml.clear();
      followTextName(*l);
    } else if (field == "font") l->font = value.toString();
    else if (field == "fontStyle") l->fontStyle = value.toString();
    else if (field == "fontSize") l->fontSize = std::max(1., value.toDouble());
    else if (field == "tracking") l->tracking = value.toDouble();
    else if (field == "leading") l->leading = std::max(.1, value.toDouble());
    else if (field == "align") l->align = std::clamp(value.toInt(), 0, 3);
    else if (field == "fauxBold") l->fauxBold = value.toBool();
    else if (field == "fauxItalic") l->fauxItalic = value.toBool();
    else if (field == "bold") l->bold = value.toBool();
    else if (field == "capitals") l->capitals = std::clamp(value.toInt(), 0, 3);
    else if (field == "paragraphWidth") l->paragraphWidth = std::max(0., value.toDouble());
    else if (field == "strokeOnly") l->textStrokeOnly = value.toBool();
    else if (field == "strokeWidth") l->strokeWidth = std::max(0., value.toDouble());
    else if (field == "strokeColor") l->strokeColor = colorFrom(value);
    else if (field == "strokes") {
      l->textStrokes.clear();
      for (const QVariant &entry : value.toList()) {
        const QVariantMap m = entry.toMap();
        TextStrokeStyle stroke;
        stroke.width = m.value("width", 4).toDouble();
        if (m.contains("color")) stroke.color = colorFrom(m.value("color"));
        stroke.mode = std::clamp(m.value("mode", 1).toInt(), 0, 2);
        stroke.join = m.value("join", 0).toInt() == 1 ? 1 : 0;
        l->textStrokes.push_back(stroke);
      }
    } else if (field == "blur") l->blur = std::max(0., value.toDouble());
    else if (field == "glow") l->glow = std::max(0., value.toDouble());
    else if (field == "shadow") l->shadow = std::max(0., value.toDouble());
    else if (field == "separatePosition") l->separatePosition = value.toBool();
    else if (field == "separateScale") l->separateScale = value.toBool();
    else if (field == "lockScale") l->lockScale = value.toBool();
    else if (field == "timeRemap") l->timeRemap = value.toBool();
    else if (field == "parent") {
      const int child = indexOf(id), parent = value.toString().isEmpty() ? -1 : indexOf(value.toString());
      if (parent < -1 || (!value.toString().isEmpty() && parent < 0)) return s.fail("No such parent layer");
      if (!setLayerParent(s.scene(), child, parent)) s.fail("That layer cannot be the parent (it would parent itself)");
    } else {
      QVariantMap patch{{field, value}};
      layerPatch(id, patch);
    }
  }
  // The layer whole, as the project file keeps it -- and changed, any part of
  // it, by a patch merged in (objects field by field, anything else replaced).
  Q_INVOKABLE QVariantMap layerData(const QString &id) {
    const Layer *l = find(id);
    return l ? layerJson(*l).toVariantMap() : QVariantMap();
  }
  Q_INVOKABLE void layerPatch(const QString &id, const QVariantMap &patch) {
    Layer *l = find(id);
    if (!l) return;
    QJsonObject data = merged(layerJson(*l), QJsonObject::fromVariantMap(patch)).toObject();
    data["id"] = l->id; // a layer keeps who it is
    Scene holder = sceneFor(*l);
    QJsonObject j = holder.json();
    j["layers"] = QJsonArray{data};
    Scene back;
    QString why;
    if (!back.load(j, why) || back.layers.size() != 1) return s.fail("The layer did not take that: " + why);
    s.change();
    Layer made = back.layers.front();
    // What the file does not carry stays as it was.
    if (made.path == l->path) {
      made.image = l->image;
      made.footageSize = l->footageSize;
      made.pictureMissing = l->pictureMissing;
    }
    *l = made;
  }
  // A new layer at the top of a composition. `type`: text, shape (with
  // shapeKind rect / circle / polygon / star / path), fill, empty, camera,
  // light, effect, group, particles, audio. Returns its id.
  Q_INVOKABLE QString addLayer(const QString &composition, const QString &type, const QVariantMap &options) {
    Scene &sc = s.scene();
    if (int(sc.layers.size()) >= maxLayersPerComposition) {
      s.fail(QString("The Main already holds %1 layers, the most it can carry").arg(maxLayersPerComposition));
      return {};
    }
    static const QStringList types{"text", "shape", "fill", "empty", "camera", "light", "effect", "group", "particles", "audio", "image", "video"};
    if (!types.contains(type)) {
      s.fail("There is no layer type \"" + type + "\"");
      return {};
    }
    if (!composition.isEmpty() && indexOf(composition) < 0) {
      s.fail("No such composition");
      return {};
    }
    s.change();
    Layer l;
    l.groupId = composition;
    l.type = type;
    const int count = int(sc.layers.size()) + 1;
    l.p[0].base = sc.width / 2.;
    l.p[1].base = sc.height / 2.;
    l.duration = sc.frames;
    if (type != "text") l.color = QColor("#8175ee");
    if (type == "text") {
      l.text = options.value("text", "Text").toString();
    } else if (type == "shape") {
      l.shapeKind = options.value("shapeKind", "rect").toString();
      l.name = "Shape " + QString::number(count);
      if (l.shapeKind == "circle") l.p[ShapeWidth].base = l.p[ShapeHeight].base = 160;
    } else if (type == "fill") {
      l.name = "Fill " + QString::number(count);
      l.color = QColor("#808080");
    } else if (type == "empty") {
      l.name = "Empty " + QString::number(count);
    } else if (type == "camera") {
      l.name = "Camera " + QString::number(count);
      l.threeD = true;
      l.lensFocalLength = 50;
      l.p[CameraZoom].base = l.lensFocalLength * sc.width / 36.;
      l.p[PositionZ].base = -l.p[CameraZoom].base;
      l.dofFocusDistance = l.p[CameraZoom].base;
    } else if (type == "light") {
      l.name = "Light " + QString::number(count);
      l.threeD = true;
      l.color = Qt::white;
      l.p[0].base = sc.width * .35;
      l.p[1].base = sc.height * .25;
      l.p[PositionZ].base = -sc.width * .45;
      l.p[LightAimX].base = sc.width / 2.;
      l.p[LightAimY].base = sc.height / 2.;
      l.p[LightRadius].base = sc.width * .3;
      l.p[LightFalloffDistance].base = sc.width * .5;
    } else if (type == "effect") {
      l.name = "Effect Layer " + QString::number(count);
    } else if (type == "group") {
      l.name = "Group " + QString::number(count);
    } else {
      l.name = type + " " + QString::number(count);
    }
    if (options.contains("name")) {
      l.name = options.value("name").toString();
      l.namedByHand = true;
    }
    if (options.contains("color")) l.color = colorFrom(options.value("color"));
    if (options.contains("width")) l.p[ShapeWidth].base = options.value("width").toDouble();
    if (options.contains("height")) l.p[ShapeHeight].base = options.value("height").toDouble();
    if (options.contains("start")) l.start = options.value("start").toDouble();
    if (options.contains("duration")) l.duration = std::max(1., options.value("duration").toDouble());
    if (options.contains("path")) l.path = options.value("path").toString();
    followTextName(l);
    // At the top of its composition, as a layer added in the editor is.
    int at = 0;
    if (!composition.isEmpty()) {
      int group = -1, firstChild = -1;
      for (int i = 0; i < int(sc.layers.size()); ++i) {
        if (sc.layers[size_t(i)].id == composition) group = i;
        if (sc.layers[size_t(i)].groupId == composition && firstChild < 0) firstChild = i;
      }
      at = firstChild >= 0 ? firstChild : group + 1;
    }
    sc.layers.insert(sc.layers.begin() + at, l);
    if (sc.selected >= at) ++sc.selected;
    return l.id;
  }
  Q_INVOKABLE void removeLayer(const QString &id) {
    Scene &sc = s.scene();
    const int at = indexOf(id);
    if (at < 0) return;
    s.change();
    // A group goes with everything in it.
    QSet<QString> going{id};
    for (bool more = true; more;) {
      more = false;
      for (const Layer &l : sc.layers)
        if (!going.contains(l.id) && going.contains(l.groupId)) {
          going.insert(l.id);
          more = true;
        }
    }
    for (int i = 0; i < int(sc.layers.size()); ++i)
      if (!going.contains(sc.layers[size_t(i)].id) && going.contains(sc.layers[size_t(i)].parentId))
        setLayerParent(sc, i, -1); // a child stays where it is on screen
    for (Layer &l : sc.layers)
      if (going.contains(l.trackMatteId)) l.trackMatteId.clear();
    sc.layers.erase(std::remove_if(sc.layers.begin(), sc.layers.end(), [&](const Layer &l) { return going.contains(l.id); }),
                    sc.layers.end());
    sc.selected = std::min(sc.selected, int(sc.layers.size()) - 1);
  }
  // To `index` (1 = the top) among its composition's layers.
  Q_INVOKABLE void moveLayer(const QString &id, int index) {
    Scene &sc = s.scene();
    const int from = indexOf(id);
    if (from < 0) return;
    const QString comp = sc.layers[size_t(from)].groupId;
    std::vector<int> places;
    for (int i = 0; i < int(sc.layers.size()); ++i)
      if (sc.layers[size_t(i)].groupId == comp) places.push_back(i);
    index = std::clamp(index, 1, int(places.size()));
    const int to = places[size_t(index - 1)];
    if (to == from) return;
    s.change();
    const QString selected = sc.selected >= 0 && sc.selected < int(sc.layers.size()) ? sc.layers[size_t(sc.selected)].id : QString();
    Layer moving = sc.layers[size_t(from)];
    sc.layers.erase(sc.layers.begin() + from);
    sc.layers.insert(sc.layers.begin() + to, moving);
    if (!selected.isEmpty()) sc.selected = indexOf(selected);
  }
  Q_INVOKABLE QString duplicateLayer(const QString &id) {
    Scene &sc = s.scene();
    const int at = indexOf(id);
    if (at < 0) return {};
    if (sc.layers[size_t(at)].type == "group") {
      s.fail("A group cannot be duplicated from a script yet");
      return {};
    }
    s.change();
    Layer copy = sc.layers[size_t(at)];
    copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    sc.layers.insert(sc.layers.begin() + at, copy);
    return copy.id;
  }
  // Every key of every property of the layer moved by `frames` (its start
  // moved the other way keeps them where they were in the Main).
  Q_INVOKABLE void shiftKeys(const QString &id, double frames) {
    Layer *l = find(id);
    if (!l || frames == 0) return;
    s.change();
    for (int i = 0; i < layerPropertyCount(*l); ++i)
      for (Key &k : layerProperty(*l, i)->keys) k.frame += frames;
  }
  // Where its picture lies, in its own space: [left, top, width, height].
  Q_INVOKABLE QVariantList layerBounds(const QString &id, double frame) {
    const Layer *l = find(id);
    if (!l) return {};
    Scene copy = s.scene();
    copy.frame = std::isfinite(frame) ? frame : copy.frame;
    const QRectF r = ::layerBounds(*l, copy);
    return {r.left(), r.top(), r.width(), r.height()};
  }
  // Where a point of the layer (its own space) is in the Main, at a frame.
  Q_INVOKABLE QVariantList toComposition(const QString &id, double x, double y, double frame) {
    const Layer *l = find(id);
    if (!l) return {};
    const QPointF p = layerWorldTransform(*l, s.scene(), std::isfinite(frame) ? frame : s.scene().frame).map(QPointF(x, y));
    return {p.x(), p.y()};
  }

  // --- Properties ------------------------------------------------------
  // A layer's properties are numbered: the fixed ones below PropertyCount
  // (propKeys / propNames), then its extra curves -- an effect's settings
  // ("<effect>/<setting>"), text animation -- by their ids.
  Q_INVOKABLE QVariantList propertyNames() {
    QVariantList out;
    for (int i = 0; i < PropertyCount; ++i) out << QVariantMap{{"index", i}, {"key", propKeys[i]}, {"name", propNames[i]}};
    return out;
  }
  Q_INVOKABLE QStringList curveIds(const QString &id) {
    const Layer *l = find(id);
    return l ? l->extraCurveIds : QStringList();
  }
  // The index of an extra curve; made, flat at `initial`, if `create`.
  Q_INVOKABLE int curve(const QString &id, const QString &curveId, bool create, double initial) {
    Layer *l = find(id);
    if (!l) return -1;
    int at = layerFindExtra(*l, curveId);
    if (at < 0 && create) {
      s.change();
      at = layerAddExtra(*l, curveId, initial);
    }
    return at;
  }
  // The value at a frame of the Main (NaN: the playhead), expression and all.
  Q_INVOKABLE double value(const QString &id, int index, double frame) {
    const Property *p = property(id, index);
    if (!p) return 0;
    const Layer *l = find(id);
    // An expression on it reads the project, and a curve of the layer's own
    // knows its layer by this.
    ExpressionSceneScope sceneScope(&s.scene());
    ExpressionLayerScope layerScope(l);
    return p->at((std::isfinite(frame) ? frame : s.scene().frame) - l->start);
  }
  Q_INVOKABLE double keyedValue(const QString &id, int index, double frame) {
    const Property *p = property(id, index);
    if (!p) return 0;
    return p->keyedAt((std::isfinite(frame) ? frame : s.scene().frame) - find(id)->start);
  }
  Q_INVOKABLE double base(const QString &id, int index) {
    const Property *p = property(id, index);
    return p ? p->base : 0;
  }
  Q_INVOKABLE void setBase(const QString &id, int index, double value) {
    Property *p = property(id, index);
    if (!p) return;
    if (!p->keys.empty()) return s.fail("It has keyframes: set a value at a time instead (setValueAtTime / setAt)");
    s.change();
    p->base = value;
  }
  Q_INVOKABLE QVariantList keys(const QString &id, int index) {
    const Property *p = property(id, index);
    QVariantList out;
    if (!p) return out;
    const double start = find(id)->start;
    for (const Key &k : p->keys)
      out << QVariantMap{{"frame", k.frame + start},
                         {"value", k.value},
                         {"ease", k.ease},
                         {"custom", k.custom},
                         {"absolute", k.absolute},
                         {"curve", QVariantList{k.curve[0], k.curve[1], k.curve[2], k.curve[3]}},
                         {"continuous", k.continuous},
                         {"spatial", k.hasSpatial},
                         {"spatialIn", QVariantList{k.spatialIn.x(), k.spatialIn.y()}},
                         {"spatialOut", QVariantList{k.spatialOut.x(), k.spatialOut.y()}},
                         {"timing", k.timing}};
    return out;
  }
  // A key at a frame of the Main, made or moved there.
  Q_INVOKABLE void setKey(const QString &id, int index, double frame, double value) {
    Property *p = property(id, index);
    if (!p) return;
    s.change();
    p->set(frame - find(id)->start, value);
  }
  Q_INVOKABLE void removeKey(const QString &id, int index, double frame) {
    Property *p = property(id, index);
    if (!p) return;
    const double at = frame - find(id)->start;
    const auto before = p->keys.size();
    s.change();
    p->keys.erase(std::remove_if(p->keys.begin(), p->keys.end(), [&](const Key &k) { return std::abs(k.frame - at) < 1e-3; }),
                  p->keys.end());
    // The value stays what it was there once the last key is gone.
    if (p->keys.empty() && before) p->base = p->keyedAt(at);
  }
  Q_INVOKABLE void clearKeys(const QString &id, int index) {
    Property *p = property(id, index);
    if (!p || p->keys.empty()) return;
    s.change();
    const double held = p->keyedAt(s.scene().frame - find(id)->start);
    p->keys.clear();
    p->base = held;
  }
  // The key at a frame changed in any of these: ease (linear, hold, easeIn,
  // easeOut, easeInOut, custom), curve [4], absolute, continuous, spatial,
  // spatialIn / spatialOut [x, y] (the motion path's handles), timing.
  Q_INVOKABLE void setKeyData(const QString &id, int index, double frame, const QVariantMap &data) {
    Key *k = key(id, index, frame);
    if (!k) return s.fail(QString("There is no key at frame %1").arg(frame));
    s.change();
    if (data.contains("ease")) {
      k->ease = data.value("ease").toString();
      k->custom = k->ease == "custom";
      if (!k->custom) k->hasNormCurve = false;
    }
    if (data.contains("curve")) {
      const QVariantList c = data.value("curve").toList();
      for (int n = 0; n < 4 && n < c.size(); ++n) k->curve[size_t(n)] = c[n].toDouble();
      k->custom = true;
      k->ease = "custom";
    }
    if (data.contains("absolute")) k->absolute = data.value("absolute").toBool();
    if (data.contains("continuous")) k->continuous = data.value("continuous").toBool();
    if (data.contains("spatial")) k->hasSpatial = data.value("spatial").toBool();
    auto point = [](const QVariant &v) {
      const QVariantList l = v.toList();
      return l.size() >= 2 ? QPointF(l[0].toDouble(), l[1].toDouble()) : QPointF();
    };
    if (data.contains("spatialIn")) {
      k->spatialIn = point(data.value("spatialIn"));
      k->hasSpatial = true;
    }
    if (data.contains("spatialOut")) {
      k->spatialOut = point(data.value("spatialOut"));
      k->hasSpatial = true;
    }
    if (data.contains("timing")) k->timing = data.value("timing").toBool();
  }
  // The handles of the move from the key at `frame` to the next one, as
  // points (frame of the Main, value): out of this key, into the next.
  Q_INVOKABLE void setHandles(const QString &id, int index, double frame, double outFrame, double outValue, double inFrame,
                              double inValue) {
    Property *p = property(id, index);
    if (!p) return;
    const double start = find(id)->start, at = frame - start;
    for (size_t i = 0; i + 1 < p->keys.size(); ++i)
      if (std::abs(p->keys[i].frame - at) < 1e-3) {
        s.change();
        setSegment(p->keys[i], p->keys[i + 1], QPointF(outFrame - start, outValue), QPointF(inFrame - start, inValue));
        return;
      }
    s.fail(QString("There is no move from a key at frame %1").arg(frame));
  }
  Q_INVOKABLE QVariantList handles(const QString &id, int index, double frame) {
    const Property *p = property(id, index);
    if (!p) return {};
    const double start = find(id)->start, at = frame - start;
    for (size_t i = 0; i + 1 < p->keys.size(); ++i)
      if (std::abs(p->keys[i].frame - at) < 1e-3) {
        const auto c = segmentPoints(p->keys[i], p->keys[i + 1]);
        return {c[1].x() + start, c[1].y(), c[2].x() + start, c[2].y()};
      }
    return {};
  }
  Q_INVOKABLE QString expression(const QString &id, int index) {
    const Property *p = property(id, index);
    return p ? p->expression : QString();
  }
  // Set; empty removes it. Returns why it does not compile, or nothing.
  Q_INVOKABLE QString setExpression(const QString &id, int index, const QString &text) {
    Property *p = property(id, index);
    if (!p) return {};
    s.change();
    QString error;
    p->setExpression(text, &error);
    return error;
  }
  Q_INVOKABLE bool expressionOn(const QString &id, int index) {
    const Property *p = property(id, index);
    return p && p->expressionOn;
  }
  Q_INVOKABLE void setExpressionOn(const QString &id, int index, bool on) {
    Property *p = property(id, index);
    if (!p) return;
    s.change();
    p->expressionOn = on;
  }

  // --- Effects ---------------------------------------------------------
  Q_INVOKABLE QStringList effectNames() { return s.host.effectNames ? s.host.effectNames() : QStringList(); }
  Q_INVOKABLE QVariantList effects(const QString &id) {
    const Layer *l = find(id);
    QVariantList out;
    if (!l) return out;
    for (const QString &name : layerEffectOrder(*l))
      if (l->appliedEffects.value(name).toBool())
        out << QVariantMap{{"name", name}, {"enabled", !l->hiddenEffects.value(name).toBool()}};
    return out;
  }
  Q_INVOKABLE bool applyEffect(const QString &id, const QString &name) {
    if (!find(id)) return false;
    if (!s.host.applyEffect) return false;
    s.change();
    return s.host.applyEffect(id, name);
  }
  Q_INVOKABLE void removeEffect(const QString &id, const QString &name) {
    Layer *l = find(id);
    if (!l || !l->appliedEffects.contains(name)) return;
    s.change();
    l->appliedEffects.remove(name);
    l->hiddenEffects.remove(name);
    l->surfaceEffects.remove(name);
    QJsonArray order;
    for (const auto &entry : l->effectOrder)
      if (entry.toString() != name) order.append(entry);
    l->effectOrder = order;
    if (name == "Gaussian Blur") l->blur = 0;
    if (name == "Glow") l->glow = 0;
    if (name == "Drop Shadow") l->shadow = 0;
    const QString prefix = name + "/";
    for (const QString &curveId : QStringList(l->extraCurveIds))
      if (curveId.startsWith(prefix)) layerRemoveExtra(*l, curveId);
  }
  Q_INVOKABLE void setEffectEnabled(const QString &id, const QString &name, bool on) {
    Layer *l = find(id);
    if (!l) return;
    s.change();
    if (on) l->hiddenEffects.remove(name);
    else l->hiddenEffects[name] = true;
  }
  // An effect's settings as the layer keeps them (surfaceEffects[effect]).
  Q_INVOKABLE QVariantMap effectSettings(const QString &id, const QString &name) {
    const Layer *l = find(id);
    return l ? l->surfaceEffects.value(name).toObject().toVariantMap() : QVariantMap();
  }
  Q_INVOKABLE void setEffectSetting(const QString &id, const QString &name, const QString &setting, const QVariant &value) {
    Layer *l = find(id);
    if (!l) return;
    s.change();
    QJsonObject settings = l->surfaceEffects.value(name).toObject();
    settings[setting] = QJsonValue::fromVariant(value);
    l->surfaceEffects[name] = settings;
  }

  // --- Markers ---------------------------------------------------------
  Q_INVOKABLE QVariantList markers() {
    QVariantList out;
    for (const Marker &m : s.scene().markers) out << QVariantMap{{"frame", m.frame}, {"label", m.label}, {"beat", m.beat}};
    return out;
  }
  Q_INVOKABLE void addMarker(double frame, const QString &label, bool beat) {
    s.change();
    s.scene().markers.push_back(Marker{frame, label, beat});
    sortMarkers(s.scene().markers);
  }
  Q_INVOKABLE void removeMarker(int index) {
    auto &all = s.scene().markers;
    if (index < 0 || index >= int(all.size())) return;
    s.change();
    all.erase(all.begin() + index);
  }
  Q_INVOKABLE void setMarker(int index, double frame, const QString &label) {
    auto &all = s.scene().markers;
    if (index < 0 || index >= int(all.size())) return;
    s.change();
    all[size_t(index)].frame = frame;
    all[size_t(index)].label = label;
    sortMarkers(all);
  }

  // --- Rendering, the project file, history ----------------------------
  // A frame of a composition ("" the Main) written as a picture; the size
  // the composition's, or `width` wide. Returns where it went, or "".
  Q_INVOKABLE QString renderFrame(const QString &composition, double frame, const QString &path, int width, bool transparent) {
    if (s.touched && s.host.refresh) s.host.refresh();
    Scene shown = compositionScene(s.scene(), composition);
    shown.frame = std::isfinite(frame) ? frame : shown.frame;
    QSize size(shown.width, shown.height);
    if (width > 0) size = QSize(width, std::max(1, int(std::round(double(width) * shown.height / std::max(1, shown.width)))));
    shown.finalRender = true;
    QImage image = renderCPU(shown, shown.frame, true, size);
    if (!transparent && !image.isNull()) image = overBackground(image, shown.background);
    const QString target = nativePath(path);
    QDir().mkpath(QFileInfo(target).absolutePath());
    return !image.isNull() && image.save(target) ? target : QString();
  }
  Q_INVOKABLE bool saveProject(const QString &path) { return s.host.saveProject && s.host.saveProject(nativePath(path)); }
  Q_INVOKABLE void importFiles(const QStringList &paths) {
    QStringList natives;
    for (const QString &p : paths) natives << nativePath(p);
    if (s.host.importFiles) s.host.importFiles(natives);
  }
  Q_INVOKABLE void undo() {
    if (s.host.undo) s.host.undo();
  }
  Q_INVOKABLE void redo() {
    if (s.host.redo) s.host.redo();
  }

  // --- Files -----------------------------------------------------------
  Q_INVOKABLE QString fsNative(const QString &path) { return nativePath(path); }
  Q_INVOKABLE bool fsExists(const QString &path) { return QFileInfo::exists(nativePath(path)); }
  Q_INVOKABLE bool fsIsFolder(const QString &path) { return QFileInfo(nativePath(path)).isDir(); }
  // Text in `encoding` ("UTF-8" unless said; "BINARY" byte for byte). Null when it cannot be read.
  Q_INVOKABLE QVariant fsRead(const QString &path, const QString &encoding) {
    QFile f(nativePath(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = f.readAll();
    if (encoding.compare("BINARY", Qt::CaseInsensitive) == 0) return QString::fromLatin1(bytes);
    if (!encoding.isEmpty()) {
      QStringDecoder decoder(encoding.toUtf8().constData());
      if (decoder.isValid()) return QString(decoder(bytes));
    }
    QString text = QString::fromUtf8(bytes);
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);
    return text;
  }
  Q_INVOKABLE bool fsWrite(const QString &path, const QString &text, bool append, const QString &encoding) {
    const QString target = nativePath(path);
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile f(target);
    if (!f.open(append ? QIODevice::Append : (QIODevice::WriteOnly | QIODevice::Truncate))) return false;
    QByteArray bytes;
    if (encoding.compare("BINARY", Qt::CaseInsensitive) == 0) bytes = text.toLatin1();
    else if (!encoding.isEmpty() && encoding.compare("UTF-8", Qt::CaseInsensitive) && encoding.compare("UTF8", Qt::CaseInsensitive)) {
      QStringEncoder encoder(encoding.toUtf8().constData());
      bytes = encoder.isValid() ? QByteArray(encoder(text)) : text.toUtf8();
    } else bytes = text.toUtf8();
    return f.write(bytes) == bytes.size();
  }
  // What is in a folder, matching `mask` ("*.jsx;*.js", empty for all).
  Q_INVOKABLE QStringList fsList(const QString &folder, const QString &mask) {
    QDir dir(nativePath(folder));
    QStringList out;
    const QStringList filters = mask.isEmpty() ? QStringList() : mask.split(QRegularExpression("[;,]"), Qt::SkipEmptyParts);
    for (const QFileInfo &info : dir.entryInfoList(filters, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name))
      out << info.absoluteFilePath();
    return out;
  }
  Q_INVOKABLE bool fsMakeFolder(const QString &path) { return QDir().mkpath(nativePath(path)); }
  Q_INVOKABLE bool fsRemove(const QString &path) {
    const QString p = nativePath(path);
    QFileInfo info(p);
    return info.isDir() ? QDir(p).rmdir(p) : QFile::remove(p);
  }
  Q_INVOKABLE bool fsCopy(const QString &from, const QString &to) {
    const QString target = nativePath(to);
    if (QFileInfo::exists(target)) QFile::remove(target);
    return QFile::copy(nativePath(from), target);
  }
  Q_INVOKABLE bool fsRename(const QString &from, const QString &to) { return QFile::rename(nativePath(from), nativePath(to)); }
  Q_INVOKABLE QVariantMap fsStat(const QString &path) {
    QFileInfo info(nativePath(path));
    if (!info.exists()) return {};
    return QVariantMap{{"size", double(info.size())},
                       {"modified", double(info.lastModified().toMSecsSinceEpoch())},
                       {"created", double(info.birthTime().toMSecsSinceEpoch())},
                       {"folder", info.isDir()},
                       {"hidden", info.isHidden()},
                       {"readonly", !info.isWritable()}};
  }
  // documents, desktop, temp, userData, appData, home, program, current, scripts
  Q_INVOKABLE QString fsSpecial(const QString &which) {
    if (which == "documents") return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (which == "desktop") return QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (which == "temp") return QDir::tempPath();
    if (which == "userData") return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (which == "appData") return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (which == "home") return QDir::homePath();
    if (which == "program") return QCoreApplication::applicationDirPath();
    if (which == "current") return QDir::currentPath();
    if (which == "scripts") return scriptFolders().value(1);
    return {};
  }
  Q_INVOKABLE QStringList fsOpenDialog(const QString &title, const QString &filter, bool multiple, const QString &start) {
    if (s.unattended) return {};
    if (multiple) return QFileDialog::getOpenFileNames(s.host.window, title, nativePath(start), dialogFilter(filter));
    const QString one = QFileDialog::getOpenFileName(s.host.window, title, nativePath(start), dialogFilter(filter));
    return one.isEmpty() ? QStringList() : QStringList{one};
  }
  Q_INVOKABLE QString fsSaveDialog(const QString &title, const QString &filter, const QString &start) {
    if (s.unattended) return {};
    return QFileDialog::getSaveFileName(s.host.window, title, nativePath(start), dialogFilter(filter));
  }
  Q_INVOKABLE QString fsFolderDialog(const QString &title, const QString &start) {
    if (s.unattended) return {};
    return QFileDialog::getExistingDirectory(s.host.window, title, nativePath(start));
  }
  Q_INVOKABLE bool fsOpen(const QString &path) {
    const QString p = path.contains("://") ? path : nativePath(path);
    return QDesktopServices::openUrl(path.contains("://") ? QUrl(p) : QUrl::fromLocalFile(p));
  }

  // --- Dialogs, settings, programs, time --------------------------------
  Q_INVOKABLE void alert(const QString &message, const QString &title, bool error) {
    if (s.touched && s.host.refresh) s.host.refresh();
    s.output << "alert: " + message;
    if (s.unattended) return;
    if (error) QMessageBox::warning(s.host.window, title.isEmpty() ? "Script" : title, message);
    else QMessageBox::information(s.host.window, title.isEmpty() ? "Script" : title, message);
  }
  Q_INVOKABLE bool confirm(const QString &message, const QString &title, bool noAsDefault) {
    if (s.touched && s.host.refresh) s.host.refresh();
    s.output << "confirm: " + message;
    if (s.unattended) return true;
    return QMessageBox::question(s.host.window, title.isEmpty() ? "Script" : title, message, QMessageBox::Yes | QMessageBox::No,
                                 noAsDefault ? QMessageBox::No : QMessageBox::Yes) == QMessageBox::Yes;
  }
  Q_INVOKABLE QVariant prompt(const QString &message, const QString &preset, const QString &title) {
    if (s.touched && s.host.refresh) s.host.refresh();
    s.output << "prompt: " + message;
    if (s.unattended) return preset;
    bool ok = false;
    const QString text = QInputDialog::getText(s.host.window, title.isEmpty() ? "Script" : title, message, QLineEdit::Normal, preset, &ok);
    return ok ? QVariant(text) : QVariant();
  }
  Q_INVOKABLE QVariant settingGet(const QString &section, const QString &key) {
    QSettings settings("Wavy-Flo", "Wavy-Flo");
    return settings.value("Scripts/" + section + "/" + key);
  }
  Q_INVOKABLE void settingSet(const QString &section, const QString &key, const QVariant &value) {
    QSettings settings("Wavy-Flo", "Wavy-Flo");
    settings.setValue("Scripts/" + section + "/" + key, value);
  }
  Q_INVOKABLE bool settingHave(const QString &section, const QString &key) {
    QSettings settings("Wavy-Flo", "Wavy-Flo");
    return settings.contains("Scripts/" + section + "/" + key);
  }
  // A program run and waited for (a command line through the shell):
  // {code, output, error}.
  Q_INVOKABLE QVariantMap run(const QString &command, int timeoutMs) {
    QProcess process;
#ifdef Q_OS_WIN
    process.startCommand("cmd /c " + command);
#else
    process.start("/bin/sh", {"-c", command});
#endif
    const bool finished = process.waitForFinished(timeoutMs > 0 ? timeoutMs : 60000);
    if (!finished) process.kill();
    return QVariantMap{{"code", finished ? process.exitCode() : -1},
                       {"output", QString::fromLocal8Bit(process.readAllStandardOutput())},
                       {"error", QString::fromLocal8Bit(process.readAllStandardError())}};
  }
  Q_INVOKABLE QString env(const QString &name) { return qEnvironmentVariable(name.toUtf8().constData()); }
  Q_INVOKABLE void log(const QString &text) {
    s.output << text;
    if (s.host.log) s.host.log(text);
  }
  Q_INVOKABLE void sleep(int ms) { QThread::msleep(ulong(std::clamp(ms, 0, 60000))); }
  Q_INVOKABLE QString os() { return QSysInfo::prettyProductName() + " (" + QSysInfo::currentCpuArchitecture() + ")"; }
  Q_INVOKABLE bool windows() {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
  }
  // A timer: the script's __wfTimer(id) called after `ms`, again and again if `repeat`.
  Q_INVOKABLE int timer(int ms, bool repeat) {
    auto *t = new QTimer;
    t->setSingleShot(!repeat);
    const int id = s.nextTimer++;
    s.timers[id] = t;
    QObject::connect(t, &QTimer::timeout, t, [this, id, repeat] {
      ScriptSession::Entry entry(s);
      QJSValue call = s.engine.globalObject().property("__wfTimer");
      if (call.isCallable()) s.reportCallbackError(call.call({id}));
      if (!repeat) cancelTimer(id);
    });
    t->start(std::max(0, ms));
    return id;
  }
  Q_INVOKABLE void cancelTimer(int id) {
    auto found = s.timers.find(id);
    if (found == s.timers.end()) return;
    found->second->stop();
    found->second->deleteLater();
    s.timers.erase(found);
  }

  // --- The editor (wavy.app, wavy.on) --------------------------------------
  Q_INVOKABLE void listen(const QString &event, bool on) {
    if (!on) {
      s.listening.remove(event);
      return;
    }
    if (s.listening.contains(event)) return;
    s.listening.insert(event);
    // From now on: what is there already is not news.
    s.seenFrame = s.scene().frame;
    if (s.host.selectedLayers) s.seenSelection = s.host.selectedLayers();
    if (s.host.playing) s.seenPlaying = s.host.playing();
    if (s.host.revision) s.seenRevision = s.host.revision();
    eventPoll()->start();
  }
  Q_INVOKABLE QStringList commands() {
    QStringList out;
    for (const auto &[path, action] : menuActions(s.host.window)) out << path;
    return out;
  }
  // A menu item run as if clicked: by its whole path ("Layer/Precompose"),
  // or by its own name when only one item has it.
  Q_INVOKABLE bool runCommand(const QString &path) {
    const auto all = menuActions(s.host.window);
    QStringList parts = path.split('/', Qt::SkipEmptyParts);
    for (QString &part : parts) part = menuText(part);
    const QString wanted = parts.join('/');
    QAction *found = nullptr;
    for (const auto &[p, a] : all)
      if (p.compare(menuText(wanted), Qt::CaseInsensitive) == 0) found = a;
    if (!found) {
      int same = 0;
      for (const auto &[p, a] : all)
        if (p.section('/', -1).compare(menuText(wanted), Qt::CaseInsensitive) == 0) {
          found = a;
          ++same;
        }
      if (same != 1) found = nullptr;
    }
    if (!found) return false;
    // Its menu brought up to date first, as opening it would.
    for (QObject *o : found->associatedObjects())
      if (auto *menu = qobject_cast<QMenu *>(o)) emit menu->aboutToShow();
    if (!found->isEnabled()) {
      s.fail(QString("\"%1\" cannot be done now (it is greyed out)").arg(path));
      return true;
    }
    s.change();
    if (s.host.withinStep) s.host.withinStep([found] { found->trigger(); });
    else found->trigger();
    return true;
  }
  // A menu item of the script's own: in the menu its path names (made if need
  // be; "Scripts/..." when only a name is given), with a shortcut if asked.
  Q_INVOKABLE int addCommand(const QString &path, const QString &shortcut) {
    auto *main = qobject_cast<QMainWindow *>(s.host.window);
    if (!main) return 0;
    QStringList parts = path.split('/', Qt::SkipEmptyParts);
    for (QString &part : parts) part = part.trimmed();
    if (parts.isEmpty()) {
      s.fail("A command needs a name");
      return 0;
    }
    if (parts.size() == 1) parts.prepend("Scripts");
    const QKeySequence keys(shortcut);
    if (!shortcut.isEmpty()) {
      if (keys.isEmpty()) {
        s.fail(QString("\"%1\" is not a shortcut Wavy-Flo can read").arg(shortcut));
        return 0;
      }
      for (QAction *a : main->findChildren<QAction *>())
        if (a->shortcuts().contains(keys)) {
          s.fail(shortcut + " is already the shortcut of " + menuText(a->text()));
          return 0;
        }
    }
    QMenuBar *bar = menuBarOf(main);
    if (!bar) return 0;
    QMenu *menu = nullptr;
    QAction *help = nullptr;
    for (QAction *top : bar->actions()) {
      if (top->menu() && menuText(top->text()).compare(parts[0], Qt::CaseInsensitive) == 0) menu = top->menu();
      if (menuText(top->text()) == "Help") help = top;
    }
    if (!menu) {
      menu = new QMenu(parts[0], bar);
      if (help) bar->insertMenu(help, menu);
      else bar->addMenu(menu);
      s.madeMenus << menu;
    }
    for (int i = 1; i + 1 < parts.size(); ++i) {
      QMenu *inner = childMenu(menu, parts[i]);
      if (!inner) {
        inner = menu->addMenu(parts[i]);
        s.madeMenus.prepend(inner);
      }
      menu = inner;
    }
    QAction *action = menu->addAction(parts.back());
    if (!shortcut.isEmpty()) action->setShortcut(keys);
    const int id = s.nextCommand++;
    s.commands[id] = action;
    QObject::connect(action, &QAction::triggered, action, [this, id] {
      ScriptSession::Entry entry(s);
      QJSValue call = s.engine.globalObject().property("__wfCommand");
      if (call.isCallable()) s.reportCallbackError(call.call({id}));
    });
    return id;
  }
  Q_INVOKABLE void removeCommand(int id) {
    auto found = s.commands.find(id);
    if (found == s.commands.end()) return;
    delete found->second.data();
    s.commands.erase(found);
    for (const QPointer<QMenu> &menu : s.madeMenus)
      if (menu && menu->actions().isEmpty()) delete menu.data();
  }
  Q_INVOKABLE bool playing() { return s.host.playing && s.host.playing(); }
  Q_INVOKABLE void setPlaying(bool on) {
    if (s.host.setPlaying) s.host.setPlaying(on);
  }
  // wavy.undoGroup: what comes next is a step of its own.
  Q_INVOKABLE void newStep() { s.stepped = false; }
  Q_INVOKABLE void status(const QString &text, int ms) {
    if (auto *main = qobject_cast<QMainWindow *>(s.host.window)) main->statusBar()->showMessage(text, ms);
  }
  // --- What Wavy-Flo does to layers (wavy.js: pulseOnBeats, lineUp ...) ----
  Q_INVOKABLE QVariantList effectInfo() { return s.host.effectInfo ? s.host.effectInfo() : QVariantList(); }
  // Pulse on Beats: keys that jump on the markers and settle before the next.
  Q_INVOKABLE int pulse(const QString &id, const QVariantMap &o) {
    Layer *l = find(id);
    if (!l) return 0;
    s.change();
    PulseSettings p;
    p.move = std::clamp(o.value("move", p.move).toInt(), 0, 3);
    p.strength = o.value("strength", p.strength).toDouble();
    p.settle = std::max(1, o.value("settle", p.settle).toInt());
    p.every = std::max(1, o.value("every", p.every).toInt());
    p.handMarkers = o.value("handMarkers", p.handMarkers).toBool();
    return pulseOnMarkers(*l, s.scene().markers, p);
  }
  // Line Up in Time: one after another, in the order given.
  Q_INVOKABLE int lineUp(const QStringList &ids, const QVariantMap &o) {
    std::vector<int> order;
    for (const QString &id : ids)
      if (const int at = indexOf(id); at >= 0) order.push_back(at);
    if (order.empty()) return 0;
    s.change();
    LineUpSettings settings;
    settings.from = o.value("from", settings.from).toDouble();
    settings.step = o.value("step", settings.step).toDouble();
    settings.onMarkers = o.value("onMarkers", settings.onMarkers).toBool();
    return lineUpInTime(s.scene().layers, order, s.scene().markers, settings);
  }
  // Keys `first` to `last` (by number, from 0) played backwards in their span.
  Q_INVOKABLE bool reverseKeys(const QString &id, int prop, int first, int last) {
    Layer *l = find(id);
    if (!l || prop < 0 || prop >= layerPropertyCount(*l)) return false;
    Property &p = l->at(prop);
    if (last < 0) last = int(p.keys.size()) - 1;
    if (first < 0 || last <= first || last >= int(p.keys.size())) return false;
    s.change();
    return ::reverseKeys(p, first, last);
  }
  // Transforms after the first (Transform 2, 3 ...): each its own Position,
  // Scale, Rotation and Anchor Point, curves of the layer's ("Xform/<id>/...").
  Q_INVOKABLE QVariantList transforms(const QString &id) {
    QVariantList out;
    if (const Layer *l = find(id))
      for (const AdditionalTransform &t : l->additionalTransforms) out << QVariantMap{{"id", t.id}, {"name", t.name}};
    return out;
  }
  Q_INVOKABLE QString addTransform(const QString &id, const QString &name) {
    Layer *l = find(id);
    if (!l) return {};
    s.change();
    addAdditionalTransform(*l, name);
    return l->additionalTransforms.back().id;
  }
  Q_INVOKABLE void removeTransform(const QString &id, const QString &transform) {
    Layer *l = find(id);
    if (!l || !l->findAdditionalTransform(transform)) return;
    s.change();
    removeAdditionalTransform(*l, transform);
  }

  // --- Pictures (wavy.image) ------------------------------------------------
  // Straight RGBA, 8 bits a channel; pixels read and written as numbers 0..255.
  int keepImage(QImage image) {
    if (image.isNull()) return 0;
    const int id = s.nextImage++;
    s.images[id] = image.convertToFormat(QImage::Format_RGBA8888);
    return id;
  }
  QImage *image(int id) {
    auto found = s.images.find(id);
    if (found == s.images.end()) {
      s.fail("That picture is gone (it was freed)");
      return nullptr;
    }
    return &found->second;
  }
  Q_INVOKABLE int imageRender(const QString &composition, double frame, int width, bool transparent) {
    if (s.touched && s.host.refresh) s.host.refresh();
    Scene shown = compositionScene(s.scene(), composition);
    shown.frame = std::isfinite(frame) ? frame : shown.frame;
    QSize size(shown.width, shown.height);
    if (width > 0) size = QSize(width, std::max(1, int(std::round(double(width) * shown.height / std::max(1, shown.width)))));
    shown.finalRender = true;
    const QImage frameImage = renderCPU(shown, shown.frame, true, size);
    return keepImage(transparent || frameImage.isNull() ? frameImage : overBackground(frameImage, shown.background));
  }
  Q_INVOKABLE int imageLoad(const QString &path) {
    QImage loaded(nativePath(path));
    if (loaded.isNull()) s.fail("Cannot read the picture " + path);
    return keepImage(loaded);
  }
  Q_INVOKABLE int imageNew(int width, int height, const QVariant &color) {
    if (width < 1 || height < 1 || qint64(width) * height > 400000000LL) {
      s.fail("A picture is from 1x1 to 20000x20000");
      return 0;
    }
    QImage made(width, height, QImage::Format_RGBA8888);
    const QColor c = color.isValid() ? colorFrom(color) : QColor(0, 0, 0, 0);
    made.fill(c.isValid() ? c : QColor(0, 0, 0, 0));
    return keepImage(made);
  }
  Q_INVOKABLE QVariantMap imageInfo(int id) {
    const QImage *im = image(id);
    return im ? QVariantMap{{"width", im->width()}, {"height", im->height()}} : QVariantMap();
  }
  Q_INVOKABLE QVariantList imagePixels(int id, int x, int y, int w, int h) {
    QVariantList out;
    const QImage *im = image(id);
    if (!im) return out;
    const QRect r = QRect(x, y, w, h).intersected(im->rect());
    out.reserve(r.width() * r.height() * 4);
    for (int row = r.top(); row <= r.bottom(); ++row) {
      const uchar *line = im->constScanLine(row) + r.left() * 4;
      for (int i = 0; i < r.width() * 4; ++i) out << int(line[i]);
    }
    return out;
  }
  Q_INVOKABLE void imageSetPixels(int id, int x, int y, int w, int h, const QVariantList &values) {
    QImage *im = image(id);
    if (!im) return;
    const QRect r = QRect(x, y, w, h).intersected(im->rect());
    int i = 0;
    for (int row = y; row < y + h; ++row)
      for (int col = x; col < x + w; ++col)
        for (int c = 0; c < 4; ++c, ++i)
          if (r.contains(col, row) && i < values.size())
            im->scanLine(row)[col * 4 + c] = uchar(std::clamp(values[i].toInt(), 0, 255));
  }
  // The average colour of a box: [r, g, b, a], 0..255.
  Q_INVOKABLE QVariantList imageAverage(int id, int x, int y, int w, int h) {
    const QImage *im = image(id);
    if (!im) return {};
    const QRect r = QRect(x, y, w, h).intersected(im->rect());
    if (r.isEmpty()) return {0, 0, 0, 0};
    double sum[4] = {0, 0, 0, 0};
    for (int row = r.top(); row <= r.bottom(); ++row) {
      const uchar *line = im->constScanLine(row) + r.left() * 4;
      for (int col = 0; col < r.width(); ++col)
        for (int c = 0; c < 4; ++c) sum[c] += line[col * 4 + c];
    }
    const double n = double(r.width()) * r.height();
    return {sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n};
  }
  Q_INVOKABLE int imageScaled(int id, int width, int height, bool smooth) {
    const QImage *im = image(id);
    if (!im || width < 1 || height < 1) return 0;
    return keepImage(im->scaled(width, height, Qt::IgnoreAspectRatio, smooth ? Qt::SmoothTransformation : Qt::FastTransformation));
  }
  Q_INVOKABLE int imageCopy(int id, int x, int y, int w, int h) {
    const QImage *im = image(id);
    return im ? keepImage(im->copy(x, y, w, h)) : 0;
  }
  Q_INVOKABLE QString imageSave(int id, const QString &path, int quality) {
    const QImage *im = image(id);
    if (!im) return {};
    const QString target = nativePath(path);
    QDir().mkpath(QFileInfo(target).absolutePath());
    return im->save(target, nullptr, quality) ? target : QString();
  }
  Q_INVOKABLE void imageFree(int id) { s.images.erase(id); }

  // --- The network (wavy.net) ------------------------------------------------
  // A request, waited for (the editor keeps drawing meanwhile):
  // {status, headers, body, error}.
  Q_INVOKABLE QVariantMap http(const QString &method, const QString &url, const QVariantMap &headers,
                               const QVariant &body, int timeoutMs, const QString &saveTo) {
    if (QUrl(url).isLocalFile()) {
      // A file:// address: the file itself.
      QFile file(QUrl(url).toLocalFile());
      if (!file.open(QIODevice::ReadOnly)) return {{"status", 0}, {"error", "Cannot read " + url}, {"body", QString()}};
      const QByteArray bytes = file.readAll();
      if (saveTo.isEmpty()) return {{"status", 0}, {"body", QString::fromUtf8(bytes)}, {"headers", QVariantMap()}};
      QFile out(nativePath(saveTo));
      return {{"status", 0}, {"saved", out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size()}, {"size", bytes.size()}};
    }
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Wavy-Flo/" + s.host.version);
    for (auto it = headers.begin(); it != headers.end(); ++it) request.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
    QByteArray payload;
    if (body.typeId() == QMetaType::QVariantMap || body.typeId() == QMetaType::QVariantList) {
      payload = QJsonDocument::fromVariant(body).toJson(QJsonDocument::Compact);
      if (!headers.contains("Content-Type")) request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    } else if (body.isValid() && !body.isNull()) {
      payload = body.toString().toUtf8();
    }
    QNetworkReply *reply = manager.sendCustomRequest(request, method.toUtf8().toUpper(), payload);
    QEventLoop wait;
    QTimer limit;
    limit.setSingleShot(true);
    QObject::connect(&limit, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, &wait, &QEventLoop::quit);
    limit.start(timeoutMs > 0 ? timeoutMs : 30000);
    wait.exec(QEventLoop::ExcludeUserInputEvents);
    QVariantMap out;
    out["status"] = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QVariantMap got;
    for (const auto &pair : reply->rawHeaderPairs()) got[QString::fromUtf8(pair.first)] = QString::fromUtf8(pair.second);
    out["headers"] = got;
    const QByteArray bytes = reply->readAll();
    if (!saveTo.isEmpty()) {
      QFile file(nativePath(saveTo));
      QDir().mkpath(QFileInfo(file).absolutePath());
      out["saved"] = reply->error() == QNetworkReply::NoError && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
      out["size"] = bytes.size();
    } else {
      out["body"] = QString::fromUtf8(bytes);
    }
    if (reply->error() != QNetworkReply::NoError) out["error"] = reply->errorString();
    reply->deleteLater();
    return out;
  }

  // --- Movies (composition.renderVideo, wavy.renderQueue) ---------------------
  // RenderSettings from a script's options: format ("mp4", "png" -- a
  // sequence --, "mov", "movpng", "prores", or 0..4), depth, bitrate (Mb/s),
  // audio, alpha, from, to.
  bool renderSettingsFrom(const QVariantMap &o, RenderSettings &r) {
    const QVariant format = o.value("format", "mp4");
    static const QStringList names{"mp4", "png", "mov", "movpng", "prores"};
    if (format.typeId() == QMetaType::QString) {
      const int at = names.indexOf(format.toString().toLower());
      if (at < 0) {
        s.fail("format is one of mp4, png, mov, movpng, prores");
        return false;
      }
      r.format = at;
    } else {
      r.format = std::clamp(format.toInt(), 0, 4);
    }
    const int depth = o.value("depth", 0).toInt();
    r.processingDepth = depth == 8 || depth == 16 || depth == 32 ? depth : 0;
    r.bitrateMbps = std::clamp(o.value("bitrate", r.bitrateMbps).toInt(), 1, 2000);
    r.includeAudio = o.value("audio", r.includeAudio).toBool();
    r.preserveTransparency = o.value("alpha", r.preserveTransparency).toBool();
    r.firstFrame = std::max(0, o.value("from", 0).toInt());
    r.lastFrame = o.value("to", -1).toInt();
    return true;
  }
  QString compositionName(const QString &composition) {
    if (composition.isEmpty()) return s.host.mainName ? s.host.mainName() : QString("Main 01");
    const Layer *group = find(composition);
    return group ? group->name : QString("Main");
  }
  // A movie of a composition ("" the Main), waited for. The script's
  // __wfProgress(done, total) hears how far it is, and stops it by returning
  // false. Gives {ok, file, error}.
  Q_INVOKABLE QVariantMap renderVideo(const QString &composition, const QString &output, const QVariantMap &o) {
    if (s.touched && s.host.refresh) s.host.refresh();
    RenderQueueItem item;
    item.composition = composition;
    item.name = compositionName(composition);
    item.output = nativePath(output);
    if (!renderSettingsFrom(o, item.settings)) return {};
    const QString target = renderQueueTarget(item);
    Scene scene = compositionScene(s.scene(), composition);
    std::function<QImage(Scene &)> render = s.host.renderer ? s.host.renderer() : std::function<QImage(Scene &)>();
    if (!render)
      render = [](Scene &sc) { return renderCPU(sc, sc.frame, true, QSize(sc.width, sc.height)); };
    QDir().mkpath(QFileInfo(target).absolutePath()); // a sequence's own folder is made by the render
    QJSValue progress = s.engine.globalObject().property("__wfProgress");
    auto *main = qobject_cast<QMainWindow *>(s.host.window);
    const QString why = renderToFile(
        scene, item.settings, target, render,
        [&](int done, int total, const QString &) {
          if (main) main->statusBar()->showMessage(QString("Rendering %1: %2 of %3").arg(item.name).arg(done).arg(total));
          QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
          if (!progress.isCallable()) return true;
          const QJSValue answer = progress.call({done, total});
          if (answer.isError()) {
            s.reportCallbackError(answer);
            return false;
          }
          return !(answer.isBool() && !answer.toBool());
        },
        true);
    if (main) main->statusBar()->showMessage(why.isEmpty() ? "Rendered " + target : "Render: " + why, 8000);
    return {{"ok", why.isEmpty()}, {"file", target}, {"error", why}};
  }
  Q_INVOKABLE QVariantList renderQueueItems() { return s.host.renderQueueItems ? s.host.renderQueueItems() : QVariantList(); }
  Q_INVOKABLE QString renderQueueAdd(const QString &composition, const QString &output, const QVariantMap &o) {
    RenderQueueItem item;
    item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.composition = composition;
    item.name = o.value("name", compositionName(composition)).toString();
    item.output = nativePath(output);
    if (!renderSettingsFrom(o, item.settings)) return {};
    item.enabled = o.value("enabled", true).toBool();
    if (!s.host.renderQueueAdd || !s.host.renderQueueAdd(renderQueueItemJson(item))) {
      s.fail("The Render Queue cannot take a render now (it is rendering, or there is none)");
      return {};
    }
    return item.id;
  }
  Q_INVOKABLE bool renderQueueRemove(const QString &id) { return s.host.renderQueueRemove && s.host.renderQueueRemove(id); }
  Q_INVOKABLE void renderQueueRun(bool start) {
    if (s.host.renderQueueRun) s.host.renderQueueRun(start);
  }
  Q_INVOKABLE bool renderQueueBusy() { return s.host.renderQueueBusy && s.host.renderQueueBusy(); }

  // --- Text Animations (layer.textAnimators) ----------------------------------
  // Each by its uid, top first: {uid, name, off, edited, settings, summary}.
  Q_INVOKABLE QVariantList textAnimators(const QString &id) {
    QVariantList out;
    const Layer *l = find(id);
    if (!l || !l->appliedEffects.value("Text-Animation").toBool()) return out;
    const QString edited = textAnimatorUid(l->characterAnimation);
    for (const TextAnimator &a : ::textAnimators(*l, true)) {
      QJsonObject own = textAnimatorOwnSettings(a.settings);
      out << QVariantMap{{"uid", a.uid},
                         {"name", own.value("name").toString()},
                         {"off", own.value("off").toBool()},
                         {"edited", a.uid == edited},
                         {"settings", own.toVariantMap()},
                         {"summary", textAnimatorSummary(own)}};
    }
    return out;
  }
  // A new one at the bottom of the stack (the layer's first switches Text
  // Animation on, in the place it has), with these settings. Its uid.
  Q_INVOKABLE QString addTextAnimator(const QString &id, const QVariantMap &settings) {
    Layer *l = find(id);
    if (!l) return {};
    if (l->type != "text") {
      s.fail("Text Animation is for text layers");
      return {};
    }
    s.change();
    QString uid;
    if (!l->appliedEffects.value("Text-Animation").toBool()) {
      l->appliedEffects["Text-Animation"] = true;
      l->hiddenEffects.remove("Text-Animation");
      QJsonObject fresh{{"enabled", true}};
      if (l->characterAnimation.contains("uid")) fresh["uid"] = l->characterAnimation.value("uid");
      l->characterAnimation = fresh;
      l->p[6] = Property{};
      l->p[6].base = 100;
      l->p[7] = Property{};
      uid = textAnimatorUid(l->characterAnimation);
    } else {
      l->characterAnimation["enabled"] = true;
      uid = ::addTextAnimator(*l);
    }
    for (auto it = settings.begin(); it != settings.end(); ++it)
      ::setTextAnimatorSetting(*l, uid, it.key(), QJsonValue::fromVariant(it.value()));
    return uid;
  }
  Q_INVOKABLE void setTextAnimatorSetting(const QString &id, const QString &uid, const QString &key, const QVariant &value) {
    Layer *l = find(id);
    if (!l || !hasAnimator(*l, uid)) return;
    s.change();
    if (key == "off") ::setTextAnimatorOff(*l, uid, value.toBool());
    else ::setTextAnimatorSetting(*l, uid, key, QJsonValue::fromVariant(value));
  }
  // The curve an animator's In, Out or Range number is keyed on: In and Out
  // of the one in the panel's places are properties 6 and 7; every other is
  // a curve of the layer's own, made when first asked for.
  Q_INVOKABLE int textAnimatorCurve(const QString &id, const QString &uid, const QString &which) {
    Layer *l = find(id);
    if (!l || !hasAnimator(*l, uid)) return -1;
    if (uid == textAnimatorUid(l->characterAnimation)) {
      if (which == "In") return 6;
      if (which == "Out") return 7;
    }
    const QString curveId = "TextAnim/" + uid + "/" + which;
    if (const int at = layerFindExtra(*l, curveId); at >= 0) return at;
    if (!textRangeCurves().contains(which)) return -1;
    s.change();
    double initial = which == "Range End" ? 100 : 0;
    for (const TextAnimator &a : ::textAnimators(*l, true))
      if (a.uid == uid) {
        const QString key = which == "Range Start" ? "rangeStart" : which == "Range End" ? "rangeEnd" : "rangeOffset";
        initial = a.settings.value(key).toDouble(initial);
      }
    return layerAddExtra(*l, curveId, initial);
  }
  Q_INVOKABLE bool editTextAnimator(const QString &id, const QString &uid) {
    Layer *l = find(id);
    if (!l || !hasAnimator(*l, uid)) return false;
    if (uid == textAnimatorUid(l->characterAnimation)) return true;
    s.change();
    ::selectTextAnimator(*l, uid);
    return true;
  }
  Q_INVOKABLE bool moveTextAnimator(const QString &id, const QString &uid, int to) {
    Layer *l = find(id);
    if (!l || !hasAnimator(*l, uid)) return false;
    QStringList order = textAnimatorOrder(l->characterAnimation);
    order.removeAll(uid);
    order.insert(std::clamp(to, 0, int(order.size())), uid);
    s.change();
    return setTextAnimatorOrder(*l, order) || true;
  }
  Q_INVOKABLE QString duplicateTextAnimator(const QString &id, const QString &uid) {
    Layer *l = find(id);
    if (!l || !hasAnimator(*l, uid)) return {};
    s.change();
    ::selectTextAnimator(*l, uid);
    return ::duplicateTextAnimator(*l);
  }
  Q_INVOKABLE void removeTextAnimator(const QString &id, const QString &uid) {
    Layer *l = find(id);
    if (!l || !hasAnimator(*l, uid)) return;
    s.change();
    const bool last = textAnimatorOrder(l->characterAnimation).size() <= 1;
    ::selectTextAnimator(*l, uid);
    ::removeTextAnimator(*l);
    if (last) {
      // The only one: Text Animation off, as removing the effect leaves it.
      l->appliedEffects.remove("Text-Animation");
      l->characterAnimation = QJsonObject{};
    }
  }
  Q_INVOKABLE QVariantList textPresets() { return s.host.textPresets ? s.host.textPresets() : QVariantList(); }
  bool hasAnimator(const Layer &l, const QString &uid) {
    if (textAnimatorOrder(l.characterAnimation).contains(uid)) return true;
    s.fail("This layer has no Text Animation " + uid);
    return false;
  }

  // --- Expression Controls (layer.addControl) ---------------------------------
  // Each {uid, name, kind, label, valueName, layer, curves: [property numbers]}.
  Q_INVOKABLE QVariantList controls(const QString &id) {
    QVariantList out;
    const Layer *l = find(id);
    if (!l) return out;
    for (const ExpressionControl &c : expressionControls(*l)) {
      QVariantList curves;
      for (const QString &part : expressionControlParts(c.kind)) curves << layerFindExtra(*l, expressionControlCurve(c.uid, part));
      out << QVariantMap{{"uid", c.uid}, {"name", c.name}, {"kind", c.kind}, {"label", expressionControlLabel(c.kind)},
                         {"valueName", expressionControlValueName(c.kind)}, {"layer", c.layer}, {"curves", curves}};
    }
    return out;
  }
  Q_INVOKABLE QString addControl(const QString &id, const QString &kind, const QString &name) {
    Layer *l = find(id);
    if (!l) return {};
    if (!expressionControlKinds().contains(kind)) {
      s.fail("A control is one of " + expressionControlKinds().join(", "));
      return {};
    }
    s.change();
    return addExpressionControl(*l, kind, name);
  }
  Q_INVOKABLE bool renameControl(const QString &id, const QString &uid, const QString &name) {
    Layer *l = find(id);
    if (!l) return false;
    s.change();
    return renameExpressionControl(*l, uid, name);
  }
  Q_INVOKABLE bool removeControl(const QString &id, const QString &uid) {
    Layer *l = find(id);
    if (!l) return false;
    s.change();
    return removeExpressionControl(*l, uid);
  }
  Q_INVOKABLE bool setControlLayer(const QString &id, const QString &uid, const QString &layerId) {
    Layer *l = find(id);
    if (!l) return false;
    s.change();
    return setExpressionControlLayer(*l, uid, layerId);
  }

  // --- Motion tracking (layer.addTrackPoint) -------------------------------------
  // A video layer's points, each {name, feature, search, samples: [[frame,
  // x, y, confidence] ...]}: frames of the Main, places in the layer's own
  // pixels (the footage's, from its middle).
  Q_INVOKABLE QVariantList tracks(const QString &id) {
    QVariantList out;
    const Layer *l = find(id);
    if (!l) return out;
    for (const TrackPoint &p : layerTracks(*l)) {
      QVariantList samples;
      for (const TrackSample &sample : p.samples)
        samples << QVariant(QVariantList{sample.frame + l->start, sample.at.x(), sample.at.y(), sample.confidence});
      out << QVariantMap{{"name", p.name}, {"feature", p.feature}, {"search", p.search}, {"samples", samples}};
    }
    return out;
  }
  int trackIndex(const Layer &l, const QString &name) {
    const auto points = layerTracks(l);
    for (int i = 0; i < int(points.size()); ++i)
      if (points[size_t(i)].name == name) return i;
    s.fail("The layer has no track point \"" + name + "\"");
    return -1;
  }
  Q_INVOKABLE QString addTrackPoint(const QString &id, const QString &name, double x, double y, double frame, int feature, int search) {
    Layer *l = find(id);
    if (!l) return {};
    s.change();
    auto points = layerTracks(*l);
    QString unique = name.trimmed().isEmpty() ? QString("Track Point %1").arg(points.size() + 1) : name.trimmed();
    for (int n = 2;; ++n) {
      bool taken = false;
      for (const TrackPoint &p : points) taken |= p.name == unique;
      if (!taken) break;
      unique = QString("%1 %2").arg(name.trimmed().isEmpty() ? QString("Track Point") : name.trimmed()).arg(n);
    }
    TrackPoint point;
    point.name = unique;
    point.feature = std::clamp(feature, 8, 400);
    point.search = std::clamp(search, point.feature + 4, 1200);
    point.set({std::round(frame - l->start), QPointF(x, y), 1});
    points.push_back(point);
    setLayerTracks(*l, points);
    return unique;
  }
  Q_INVOKABLE void setTrackPoint(const QString &id, const QString &name, const QVariantMap &change) {
    Layer *l = find(id);
    if (!l) return;
    const int at = trackIndex(*l, name);
    if (at < 0) return;
    s.change();
    auto points = layerTracks(*l);
    TrackPoint &p = points[size_t(at)];
    if (change.contains("feature")) p.feature = std::clamp(change.value("feature").toInt(), 8, 400);
    if (change.contains("search")) p.search = std::clamp(change.value("search").toInt(), p.feature + 4, 1200);
    if (change.contains("name") && !change.value("name").toString().trimmed().isEmpty()) p.name = change.value("name").toString().trimmed();
    if (change.contains("clear")) p.samples.clear();
    setLayerTracks(*l, points);
  }
  Q_INVOKABLE void setTrackSample(const QString &id, const QString &name, double frame, double x, double y) {
    Layer *l = find(id);
    if (!l) return;
    const int at = trackIndex(*l, name);
    if (at < 0) return;
    s.change();
    auto points = layerTracks(*l);
    points[size_t(at)].set({std::round(frame - l->start), QPointF(x, y), 1});
    setLayerTracks(*l, points);
  }
  Q_INVOKABLE void removeTrackPoint(const QString &id, const QString &name) {
    Layer *l = find(id);
    if (!l) return;
    const int at = trackIndex(*l, name);
    if (at < 0) return;
    s.change();
    auto points = layerTracks(*l);
    points.erase(points.begin() + at);
    setLayerTracks(*l, points);
  }
  // Follows the point through the footage from `from` to `to` (frames of the
  // Main; `to` before `from` goes backwards), waited for. __wfProgress(done,
  // total) hears how far, and stops it by returning false. Gives {frames,
  // lostAt (a frame of the Main, or -1), lowest (the worst match), error}.
  Q_INVOKABLE QVariantMap runTracker(const QString &id, const QString &name, double from, double to, double lostBelow) {
    Layer *l = find(id);
    if (!l) return {};
    const int at = trackIndex(*l, name);
    if (at < 0) return {};
    if (l->path.isEmpty() || !QFileInfo::exists(l->path)) {
      s.fail("The layer's footage cannot be found, so it cannot be tracked");
      return {};
    }
    s.change();
    auto points = layerTracks(*l);
    TrackPoint &point = points[size_t(at)];
    const double last = std::max(0., std::ceil(l->duration) - 1);
    const double fromFrame = std::clamp(std::round(from - l->start), 0., last);
    const double toFrame = std::clamp(std::round(to - l->start), 0., last);
    const bool forward = toFrame >= fromFrame;
    const TrackSample *here = point.sampleAt(fromFrame);
    const TrackSample start{fromFrame, point.at(fromFrame), here ? here->confidence : 1.};
    // What was tracked beyond the start, this way, is tracked again.
    point.samples.erase(std::remove_if(point.samples.begin(), point.samples.end(),
                                       [&](const TrackSample &sample) {
                                         return forward ? sample.frame > fromFrame && sample.frame <= toFrame
                                                        : sample.frame < fromFrame && sample.frame >= toFrame;
                                       }),
                        point.samples.end());
    point.set(start);
    TrackJob job;
    job.path = l->path;
    job.sourceOffsetMs = l->sourceOffsetMs;
    job.sourceDurationMs = l->sourceDurationMs > 0 ? l->sourceDurationMs : l->duration * 1000. / std::max(1, s.scene().fps);
    job.fps = s.scene().fps;
    job.fromFrame = fromFrame;
    job.toFrame = toFrame;
    job.start = start.at;
    job.feature = point.feature;
    job.search = point.search;
    if (lostBelow > 0) job.lostBelow = lostBelow;
    QJSValue progress = s.engine.globalObject().property("__wfProgress");
    const int total = int(std::abs(toFrame - fromFrame));
    std::thread worker([&job] { runTrack(job); });
    while (!job.done) {
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 30);
      size_t done = 0;
      {
        std::lock_guard<std::mutex> lock(job.guard);
        done = job.samples.size();
      }
      if (progress.isCallable()) {
        const QJSValue answer = progress.call({int(done), total});
        if (answer.isError()) {
          s.reportCallbackError(answer);
          job.cancel = true;
        } else if (answer.isBool() && !answer.toBool()) {
          job.cancel = true;
        }
      }
      QThread::msleep(5);
    }
    worker.join();
    double lowest = 1;
    for (const TrackSample &sample : job.samples) {
      point.set(sample);
      lowest = std::min(lowest, sample.confidence);
    }
    setLayerTracks(*l, points);
    return {{"frames", int(job.samples.size())},
            {"lostAt", job.lostAt >= 0 ? job.lostAt + l->start : -1.},
            {"lowest", lowest},
            {"stopped", bool(job.cancel)},
            {"error", job.error}};
  }
  // Position keys for `target` that carry it with the point, as it is now
  // relative to the point at `reference` (a frame of the Main).
  Q_INVOKABLE int applyTrack(const QString &id, const QString &name, const QString &target, double reference) {
    const int tracked = indexOf(id), onto = indexOf(target);
    if (tracked < 0 || onto < 0) return 0;
    const int at = trackIndex(s.scene().layers[size_t(tracked)], name);
    if (at < 0) return 0;
    s.change();
    const TrackPoint point = layerTracks(s.scene().layers[size_t(tracked)])[size_t(at)];
    return applyTrackToLayer(s.scene(), tracked, point, onto, reference);
  }
  // Anchor keys on the tracked layer that hold the point where it is at `reference`.
  Q_INVOKABLE int stabilizeTrack(const QString &id, const QString &name, double reference) {
    const int tracked = indexOf(id);
    if (tracked < 0) return 0;
    const int at = trackIndex(s.scene().layers[size_t(tracked)], name);
    if (at < 0) return 0;
    s.change();
    const TrackPoint point = layerTracks(s.scene().layers[size_t(tracked)])[size_t(at)];
    return stabilizeWithTrack(s.scene(), tracked, point, reference);
  }

  Q_INVOKABLE void endScript() {
    s.endAll();
    for (auto &[id, u] : s.ui)
      if (u.widget && !u.external && !u.parent) u.widget->close();
  }
  // $.evalFile: the file's text as it runs -- its #includes in, a compiled
  // one read back.
  Q_INVOKABLE QString evalFileText(const QString &path, QString included) {
    Q_UNUSED(included);
    const QString file = nativePath(path);
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
      s.fail("Cannot read " + path);
      return {};
    }
    QString text = QString::fromUtf8(f.readAll());
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);
    QSet<QString> seen;
    QString out, why;
    std::vector<Origin> origins;
    if (!preprocess(QFileInfo(file).absoluteFilePath(), text, {}, seen, out, origins, &why)) {
      s.fail(why);
      return {};
    }
    return out;
  }

  // --- Windows (ScriptUI, wavy.ui) ---------------------------------------
  Q_INVOKABLE int uiCreate(int parent, const QString &type, const QVariantMap &props);
  Q_INVOKABLE void uiSet(int id, const QString &key, const QVariant &value);
  Q_INVOKABLE QVariant uiGet(int id, const QString &key);
  Q_INVOKABLE QVariant uiShow(int id);
  Q_INVOKABLE void uiHide(int id);
  Q_INVOKABLE void uiClose(int id, int result);
  Q_INVOKABLE void uiItems(int id, const QStringList &items);
  Q_INVOKABLE void uiRemove(int id);
  Q_INVOKABLE void uiLayout(int id);
  Q_INVOKABLE bool uiAlive(int id) { return s.ui.count(id) && s.ui[id].widget; }
  // wavy.ui.panel: a panel docked in the editor (a window of its own where
  // there is no editor), filled as a ScriptUI Panel.
  Q_INVOKABLE int uiDock(const QString &title) {
    QWidget *content = s.host.dockPanel ? s.host.dockPanel(title) : nullptr;
    if (!content) {
      content = new QWidget(s.host.window, Qt::Tool);
      content->setWindowTitle(title);
      content->setAttribute(Qt::WA_DeleteOnClose);
      content->show();
    }
    const int id = adoptPanel(content);
    s.ui[id].dock = s.host.dockPanel != nullptr;
    return id;
  }

  int adoptPanel(QWidget *panel);

private:
  ScriptSession &s;

  int indexOf(const QString &id) const {
    const Scene &sc = s.host.scene();
    for (int i = 0; i < int(sc.layers.size()); ++i)
      if (sc.layers[size_t(i)].id == id) return i;
    return -1;
  }
  Layer *find(const QString &id) {
    const int at = indexOf(id);
    if (at < 0) {
      s.fail("The layer is gone (it was deleted)");
      return nullptr;
    }
    return &s.scene().layers[size_t(at)];
  }
  Property *property(const QString &id, int index) {
    Layer *l = find(id);
    if (!l) return nullptr;
    if (index < 0 || index >= layerPropertyCount(*l)) {
      s.fail(QString("The layer has no property %1").arg(index));
      return nullptr;
    }
    return layerProperty(*l, index);
  }
  Key *key(const QString &id, int index, double frame) {
    Property *p = property(id, index);
    if (!p) return nullptr;
    const double at = frame - find(id)->start;
    for (Key &k : p->keys)
      if (std::abs(k.frame - at) < 1e-3) return &k;
    return nullptr;
  }
  Scene sceneFor(const Layer &) const {
    Scene holder;
    const Scene &sc = s.host.scene();
    holder.width = sc.width;
    holder.height = sc.height;
    holder.fps = sc.fps;
    holder.frames = sc.frames;
    return holder;
  }
  QJsonObject layerJson(const Layer &l) const {
    Scene holder = sceneFor(l);
    holder.layers = {l};
    holder.layers.front().image = {};
    return holder.json().value("layers").toArray().at(0).toObject();
  }
  static QString dialogFilter(const QString &filter) {
    // ExtendScript's "Scripts:*.jsx;*.js" or Qt's own "Scripts (*.jsx *.js)".
    if (filter.isEmpty() || filter.contains('(')) return filter;
    QStringList parts;
    for (const QString &entry : filter.split(',', Qt::SkipEmptyParts)) {
      const int colon = entry.indexOf(':');
      if (colon < 0) parts << QString("%1 (%1)").arg(entry.trimmed().replace(';', ' '));
      else parts << QString("%1 (%2)").arg(entry.left(colon).trimmed(), entry.mid(colon + 1).trimmed().replace(';', ' '));
    }
    return parts.join(";;");
  }
};

// ---------------------------------------------------------------------------
// ScriptUI's controls as Qt widgets.
// ---------------------------------------------------------------------------
namespace {
Qt::Alignment horizontalFlag(const QString &a) {
  if (a == "left") return Qt::AlignLeft;
  if (a == "right") return Qt::AlignRight;
  if (a == "center") return Qt::AlignHCenter;
  return {};
}
Qt::Alignment verticalFlag(const QString &a) {
  if (a == "top") return Qt::AlignTop;
  if (a == "bottom") return Qt::AlignBottom;
  if (a == "center") return Qt::AlignVCenter;
  return {};
}
// A ScriptUI window: closing asks the script first (onClose may say no).
class ScriptWindow : public QDialog {
public:
  std::function<bool()> closing;
  using QDialog::QDialog;

protected:
  void closeEvent(QCloseEvent *event) override {
    if (closing && !closing()) {
      event->ignore();
      return;
    }
    QDialog::closeEvent(event);
  }
};
} // namespace

int ScriptBridge::adoptPanel(QWidget *panel) {
  const int id = s.nextUi++;
  ScriptSession::Ui u;
  u.widget = panel;
  u.type = "panel";
  u.orientation = "column";
  u.alignH = "fill";
  u.external = true;
  auto *layout = new QVBoxLayout(panel);
  layout->setContentsMargins(4, 4, 4, 4);
  layout->setSpacing(4);
  layout->setAlignment(Qt::AlignTop);
  u.layout = layout;
  s.ui[id] = u;
  return id;
}

int ScriptBridge::uiCreate(int parent, const QString &type, const QVariantMap &props) {
  QWidget *parentWidget = nullptr;
  if (parent) {
    auto found = s.ui.find(parent);
    if (found == s.ui.end() || !found->second.widget) {
      s.fail("The window it goes in is gone");
      return 0;
    }
    parentWidget = found->second.widget;
    if (found->second.type == "tabbedpanel") parentWidget = nullptr; // a tab is a page of it, added below
  }
  const QString text = props.value("text").toString();
  ScriptSession::Ui u;
  u.type = type;
  u.parent = parent;
  QWidget *w = nullptr;
  const int id = s.nextUi++;
  auto event = [this, id](const QString &name, const QVariant &argument = {}) { s.dispatch(id, name, argument); };
  if (type == "window" || type == "dialog" || type == "palette") {
    auto *window = new ScriptWindow(s.host.window);
    window->setWindowTitle(text);
    if (type != "dialog") window->setWindowFlag(Qt::Tool, true);
    window->setAttribute(Qt::WA_DeleteOnClose, type != "dialog");
    window->closing = [this, id] {
      QJSValue call = s.engine.globalObject().property("__wfUiClosing");
      if (!call.isCallable()) return true;
      const QJSValue answer = call.call({id});
      s.reportCallbackError(answer);
      return answer.isError() || !answer.isBool() || answer.toBool();
    };
    w = window;
    s.windows << window;
    u.orientation = "column";
  } else if (type == "panel") {
    auto *box = new QGroupBox(text, parentWidget);
    w = box;
    u.orientation = "column";
  } else if (type == "group" || type == "tab") {
    w = new QWidget(parentWidget);
    u.orientation = type == "tab" ? "column" : "row";
  } else if (type == "tabbedpanel") {
    auto *tabs = new QTabWidget(parentWidget);
    QObject::connect(tabs, &QTabWidget::currentChanged, tabs, [event](int) { event("change"); });
    w = tabs;
  } else if (type == "button" || type == "iconbutton") {
    auto *button = new QPushButton(text, parentWidget);
    QObject::connect(button, &QPushButton::clicked, button, [event] { event("click"); });
    w = button;
  } else if (type == "statictext") {
    auto *label = new QLabel(text, parentWidget);
    label->setWordWrap(props.value("multiline").toBool());
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    w = label;
  } else if (type == "edittext") {
    if (props.value("multiline").toBool()) {
      auto *edit = new QPlainTextEdit(text, parentWidget);
      edit->setReadOnly(props.value("readonly").toBool());
      QObject::connect(edit, &QPlainTextEdit::textChanged, edit, [event] { event("changing"); });
      w = edit;
    } else {
      auto *edit = new QLineEdit(text, parentWidget);
      edit->setReadOnly(props.value("readonly").toBool());
      if (props.value("password").toBool() || props.value("noecho").toBool()) edit->setEchoMode(QLineEdit::Password);
      QObject::connect(edit, &QLineEdit::textEdited, edit, [event] { event("changing"); });
      QObject::connect(edit, &QLineEdit::editingFinished, edit, [event] { event("change"); });
      w = edit;
    }
  } else if (type == "checkbox") {
    auto *box = new QCheckBox(text, parentWidget);
    QObject::connect(box, &QCheckBox::clicked, box, [event] { event("click"); });
    w = box;
  } else if (type == "radiobutton") {
    auto *radio = new QRadioButton(text, parentWidget);
    QObject::connect(radio, &QRadioButton::clicked, radio, [event] { event("click"); });
    w = radio;
  } else if (type == "slider" || type == "scrollbar") {
    auto *slider = new QSlider(Qt::Horizontal, parentWidget);
    slider->setRange(0, 1000);
    QObject::connect(slider, &QSlider::valueChanged, slider, [event, slider] {
      event("changing");
      if (!slider->isSliderDown()) event("change");
    });
    QObject::connect(slider, &QSlider::sliderReleased, slider, [event] { event("change"); });
    w = slider;
  } else if (type == "progressbar") {
    auto *bar = new QProgressBar(parentWidget);
    bar->setRange(0, 1000);
    bar->setTextVisible(false);
    w = bar;
  } else if (type == "dropdownlist") {
    auto *combo = new QComboBox(parentWidget);
    QObject::connect(combo, qOverload<int>(&QComboBox::activated), combo, [event](int) { event("change"); });
    w = combo;
  } else if (type == "listbox") {
    auto *list = new QListWidget(parentWidget);
    if (props.value("multiselect").toBool()) list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    QObject::connect(list, &QListWidget::itemSelectionChanged, list, [event] { event("change"); });
    QObject::connect(list, &QListWidget::itemDoubleClicked, list, [event](QListWidgetItem *) { event("doubleclick"); });
    w = list;
  } else if (type == "image" || type == "wfpicture") {
    w = new QLabel(parentWidget);
  } else if (type == "wfcolor") {
    // wavy.ui: a swatch; a click opens Wavy-Flo's own colour picker, the
    // colour followed live while it is open.
    auto *swatch = new QToolButton(parentWidget);
    swatch->setFixedSize(44, 18);
    setSwatch(swatch, Qt::white);
    QObject::connect(swatch, &QToolButton::clicked, swatch, [this, swatch, event] {
      const QColor was = swatch->property("wfColor").value<QColor>();
      const QColor chosen = pickColor(s.host.window, was, "Colour", true, [swatch, event](const QColor &c) {
        setSwatch(swatch, c);
        event("changing");
      });
      setSwatch(swatch, chosen.isValid() ? chosen : was);
      event("change");
    });
    w = swatch;
  } else if (type == "wfnumber") {
    auto *spin = new QDoubleSpinBox(parentWidget);
    spin->setRange(-1e9, 1e9);
    spin->setDecimals(2);
    spin->setKeyboardTracking(false);
    spin->setMinimumWidth(70);
    spin->setLocale(QLocale::c()); // 7.5, as scripts write numbers, whatever the system's own
    QObject::connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), spin, [event] { event("change"); });
    w = spin;
  } else if (type == "wfline") {
    auto *line = new QFrame(parentWidget);
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet("color:#2E2E2E;");
    w = line;
  } else {
    s.fail("ScriptUI has no control \"" + type + "\" here");
    return 0;
  }
  u.widget = w;
  w->setObjectName(props.value("name").toString());
  // A container lays out what goes in it.
  if (type == "window" || type == "dialog" || type == "palette" || type == "panel" || type == "group" || type == "tab") {
    if (props.value("orientation").toString() == "stack") {
      u.stack = new QGridLayout(w);
      u.stack->setContentsMargins(0, 0, 0, 0);
      u.orientation = "stack";
    } else {
      if (props.contains("orientation")) u.orientation = props.value("orientation").toString();
      u.layout = new QBoxLayout(u.orientation == "row" ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom, w);
      const int margin = type == "group" || type == "tab" ? 0 : 10;
      u.layout->setContentsMargins(margin, margin, margin, margin);
      u.layout->setSpacing(type == "group" ? 10 : 10);
    }
    u.alignH = "center";
    u.alignV = u.orientation == "row" ? "center" : "top";
    if (u.layout) u.layout->setAlignment(u.orientation == "row" ? Qt::AlignLeft : Qt::AlignTop);
  }
  s.ui[id] = u;
  // Into its parent: laid by the parent's layout, a page of tabs, or placed.
  if (parent) {
    ScriptSession::Ui &p = s.ui[parent];
    const QVariantList bounds = props.value("bounds").toList();
    if (p.type == "tabbedpanel") {
      static_cast<QTabWidget *>(p.widget.data())->addTab(w, text);
    } else if (bounds.size() == 4) {
      s.ui[id].placed = true;
      w->setParent(p.widget);
      w->setGeometry(QRect(QPoint(bounds[0].toInt(), bounds[1].toInt()), QPoint(bounds[2].toInt() - 1, bounds[3].toInt() - 1)));
      QSize need = p.widget->minimumSize().expandedTo(QSize(bounds[2].toInt() + 4, bounds[3].toInt() + 4));
      p.widget->setMinimumSize(need);
      w->show();
    } else if (p.stack) {
      p.stack->addWidget(w, 0, 0);
    } else if (p.layout) {
      p.layout->addWidget(w);
    }
    // The parent's alignChildren, until it has its own.
    uiSet(id, "@align", {});
  } else {
    const QVariantList bounds = props.value("bounds").toList();
    if (bounds.size() == 4) w->setGeometry(QRect(QPoint(bounds[0].toInt(), bounds[1].toInt()), QPoint(bounds[2].toInt() - 1, bounds[3].toInt() - 1)));
  }
  for (auto it = props.begin(); it != props.end(); ++it)
    if (it.key() != "bounds" && it.key() != "name" && it.key() != "multiline" && it.key() != "readonly" && it.key() != "orientation")
      uiSet(id, it.key(), it.value());
  return id;
}

void ScriptBridge::uiSet(int id, const QString &key, const QVariant &value) {
  auto found = s.ui.find(id);
  if (found == s.ui.end() || !found->second.widget) return;
  ScriptSession::Ui &u = found->second;
  QWidget *w = u.widget;
  // How a control sits in its parent: its own alignment, or the parent's alignChildren.
  auto placeInParent = [this](int child) {
    ScriptSession::Ui &c = s.ui[child];
    if (!c.parent || c.placed || !c.widget) return;
    ScriptSession::Ui &p = s.ui[c.parent];
    if (!p.layout) return;
    const bool row = p.orientation == "row";
    // Across the parent's direction: left/right/center/fill in a column,
    // top/bottom/center/fill in a row.
    const QString across = row ? (c.ownV.isEmpty() ? p.alignV : c.ownV) : (c.ownH.isEmpty() ? p.alignH : c.ownH);
    const Qt::Alignment flag = row ? verticalFlag(across) : horizontalFlag(across);
    QSizePolicy policy = c.widget->sizePolicy();
    if (row) policy.setVerticalPolicy(across == "fill" ? QSizePolicy::Expanding : QSizePolicy::Preferred);
    else policy.setHorizontalPolicy(across == "fill" ? QSizePolicy::Expanding : QSizePolicy::Preferred);
    // Along it: fill shares the room out.
    const QString along = row ? (c.ownH.isEmpty() ? p.alignH : c.ownH) : (c.ownV.isEmpty() ? p.alignV : c.ownV);
    if (along == "fill") {
      if (row) policy.setHorizontalPolicy(QSizePolicy::Expanding);
      else policy.setVerticalPolicy(QSizePolicy::Expanding);
      p.layout->setStretchFactor(c.widget, 1);
    }
    c.widget->setSizePolicy(policy);
    p.layout->setAlignment(c.widget, flag);
  };
  auto pair = [](const QVariant &v, QString &h, QString &vertical) {
    if (v.typeId() == QMetaType::QString) {
      const QString one = v.toString();
      if (one == "left" || one == "right") h = one;
      else if (one == "top" || one == "bottom") vertical = one;
      else h = vertical = one; // center, fill
    } else {
      const QVariantList l = v.toList();
      if (l.size() >= 1) h = l[0].toString();
      if (l.size() >= 2) vertical = l[1].toString();
    }
  };
  if (key == "@align") {
    placeInParent(id);
  } else if (key == "text") {
    const QString text = value.toString();
    if (auto *window = qobject_cast<QDialog *>(w)) window->setWindowTitle(text);
    else if (auto *box = qobject_cast<QGroupBox *>(w)) box->setTitle(text);
    else if (auto *button = qobject_cast<QAbstractButton *>(w)) button->setText(text);
    else if (auto *label = qobject_cast<QLabel *>(w)) label->setText(text);
    else if (auto *line = qobject_cast<QLineEdit *>(w)) line->setText(text);
    else if (auto *edit = qobject_cast<QPlainTextEdit *>(w)) edit->setPlainText(text);
    else if (u.type == "tab" && u.parent) {
      auto *tabs = qobject_cast<QTabWidget *>(s.ui[u.parent].widget.data());
      if (tabs) tabs->setTabText(tabs->indexOf(w), text);
    }
  } else if (key == "value" && u.type == "wfcolor") {
    if (auto *swatch = qobject_cast<QToolButton *>(w)) setSwatch(swatch, colorFrom(value));
  } else if (key == "value" && u.type == "wfnumber") {
    if (auto *spin = qobject_cast<QDoubleSpinBox *>(w)) {
      QSignalBlocker block(spin);
      spin->setValue(value.toDouble());
    }
  } else if ((key == "minvalue" || key == "maxvalue" || key == "step" || key == "decimals") && u.type == "wfnumber") {
    if (auto *spin = qobject_cast<QDoubleSpinBox *>(w)) {
      QSignalBlocker block(spin);
      if (key == "minvalue") spin->setMinimum(value.toDouble());
      else if (key == "maxvalue") spin->setMaximum(value.toDouble());
      else if (key == "step") spin->setSingleStep(value.toDouble());
      else spin->setDecimals(std::clamp(value.toInt(), 0, 8));
    }
  } else if (key == "picture") {
    // wavy.ui: a picture the script holds (wavy.image), shown at most 600 wide.
    auto found = s.images.find(value.toInt());
    if (auto *label = qobject_cast<QLabel *>(w); label && found != s.images.end()) {
      QImage shown = found->second;
      if (shown.width() > 600) shown = shown.scaledToWidth(600, Qt::SmoothTransformation);
      label->setPixmap(QPixmap::fromImage(shown));
    }
  } else if (key == "value") {
    if (auto *box = qobject_cast<QAbstractButton *>(w); box && box->isCheckable()) {
      box->setChecked(value.toBool());
    } else if (auto *slider = qobject_cast<QSlider *>(w)) {
      const double span = std::max(1e-9, u.maximum - u.minimum);
      QSignalBlocker block(slider);
      slider->setValue(int(std::round((std::clamp(value.toDouble(), u.minimum, u.maximum) - u.minimum) / span * 1000)));
    } else if (auto *bar = qobject_cast<QProgressBar *>(w)) {
      const double span = std::max(1e-9, u.maximum - u.minimum);
      bar->setValue(int(std::round((std::clamp(value.toDouble(), u.minimum, u.maximum) - u.minimum) / span * 1000)));
    }
  } else if (key == "minvalue" || key == "maxvalue") {
    const double keep = uiGet(id, "value").toDouble();
    (key == "minvalue" ? u.minimum : u.maximum) = value.toDouble();
    uiSet(id, "value", keep);
  } else if (key == "enabled") {
    w->setEnabled(value.toBool());
  } else if (key == "visible") {
    if (!qobject_cast<QDialog *>(w)) w->setVisible(value.toBool());
  } else if (key == "helpTip") {
    w->setToolTip(value.toString());
  } else if (key == "active") {
    if (value.toBool()) w->setFocus();
  } else if (key == "selection") {
    if (auto *combo = qobject_cast<QComboBox *>(w)) {
      QSignalBlocker block(combo);
      combo->setCurrentIndex(value.isNull() ? -1 : value.toInt());
    } else if (auto *list = qobject_cast<QListWidget *>(w)) {
      QSignalBlocker block(list);
      list->clearSelection();
      const QVariantList many = value.typeId() == QMetaType::QVariantList ? value.toList() : QVariantList{value};
      for (const QVariant &one : many)
        if (!one.isNull() && one.toInt() >= 0 && one.toInt() < list->count()) {
          list->item(one.toInt())->setSelected(true);
          list->setCurrentRow(one.toInt(), QItemSelectionModel::Select);
        }
    } else if (auto *tabs = qobject_cast<QTabWidget *>(w)) {
      tabs->setCurrentIndex(value.toInt());
    }
  } else if (key == "orientation") {
    if (u.layout) {
      u.orientation = value.toString() == "row" ? "row" : "column";
      u.layout->setDirection(u.orientation == "row" ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
      for (auto &[child, c] : s.ui)
        if (c.parent == id) placeInParent(child);
    }
  } else if (key == "alignChildren") {
    pair(value, u.alignH, u.alignV);
    if (u.layout) {
      const bool row = u.orientation == "row";
      const QString along = row ? u.alignH : u.alignV;
      u.layout->setAlignment(row ? horizontalFlag(along == "fill" ? "left" : along) : verticalFlag(along == "fill" ? "top" : along));
    }
    for (auto &[child, c] : s.ui)
      if (c.parent == id) placeInParent(child);
  } else if (key == "alignment") {
    pair(value, u.ownH, u.ownV);
    placeInParent(id);
  } else if (key == "spacing") {
    if (u.layout) u.layout->setSpacing(value.toInt());
  } else if (key == "margins") {
    const QVariantList m = value.typeId() == QMetaType::QVariantList ? value.toList() : QVariantList{value, value, value, value};
    if (u.layout && m.size() == 4) u.layout->setContentsMargins(m[0].toInt(), m[1].toInt(), m[2].toInt(), m[3].toInt());
  } else if (key == "preferredSize" || key == "minimumSize" || key == "size") {
    const QVariantList size = value.toList();
    if (size.size() >= 2) {
      if (size[0].toInt() > 0) w->setMinimumWidth(size[0].toInt());
      if (size[1].toInt() > 0) w->setMinimumHeight(size[1].toInt());
      if (key == "size" && size[0].toInt() > 0 && size[1].toInt() > 0) w->resize(size[0].toInt(), size[1].toInt());
    }
  } else if (key == "maximumSize") {
    const QVariantList size = value.toList();
    if (size.size() >= 2) w->setMaximumSize(size[0].toInt() > 0 ? size[0].toInt() : QWIDGETSIZE_MAX,
                                            size[1].toInt() > 0 ? size[1].toInt() : QWIDGETSIZE_MAX);
  } else if (key == "characters") {
    w->setMinimumWidth(w->fontMetrics().averageCharWidth() * std::max(1, value.toInt()) + 12);
  } else if (key == "justify") {
    if (auto *label = qobject_cast<QLabel *>(w))
      label->setAlignment((value.toString() == "right" ? Qt::AlignRight : value.toString() == "center" ? Qt::AlignHCenter : Qt::AlignLeft) |
                          Qt::AlignVCenter);
  } else if (key == "image") {
    QPixmap picture(nativePath(value.toString()));
    if (auto *label = qobject_cast<QLabel *>(w)) {
      if (!picture.isNull()) label->setPixmap(picture);
    } else if (auto *button = qobject_cast<QAbstractButton *>(w)) {
      if (!picture.isNull()) {
        button->setIcon(QIcon(picture));
        button->setIconSize(picture.size());
      }
    }
  } else if (key == "bounds") {
    const QVariantList b = value.toList();
    if (b.size() == 4) w->setGeometry(QRect(QPoint(b[0].toInt(), b[1].toInt()), QPoint(b[2].toInt() - 1, b[3].toInt() - 1)));
  } else if (key == "location") {
    const QVariantList p = value.toList();
    if (p.size() == 2) w->move(p[0].toInt(), p[1].toInt());
  } else if (key == "items") {
    QStringList texts;
    for (const QVariant &item : value.toList()) texts << item.toString();
    uiItems(id, texts);
  } else if (key == "foregroundColor" || key == "backgroundColor") {
    const QColor c = colorFrom(value);
    QPalette palette = w->palette();
    palette.setColor(key == "foregroundColor" ? QPalette::WindowText : QPalette::Window, c);
    if (key == "foregroundColor") {
      palette.setColor(QPalette::ButtonText, c);
      palette.setColor(QPalette::Text, c);
    } else w->setAutoFillBackground(true);
    w->setPalette(palette);
  } else if (key == "font") {
    const QVariantMap f = value.toMap();
    QFont font = w->font();
    if (f.contains("family") && !f.value("family").toString().isEmpty()) font.setFamily(f.value("family").toString());
    if (f.value("size").toDouble() > 0) font.setPointSizeF(f.value("size").toDouble());
    const QString style = f.value("style").toString().toLower();
    font.setBold(style.contains("bold"));
    font.setItalic(style.contains("italic"));
    w->setFont(font);
  }
}

QVariant ScriptBridge::uiGet(int id, const QString &key) {
  auto found = s.ui.find(id);
  if (found == s.ui.end() || !found->second.widget) return {};
  ScriptSession::Ui &u = found->second;
  QWidget *w = u.widget;
  if (key == "text") {
    if (auto *window = qobject_cast<QDialog *>(w)) return window->windowTitle();
    if (auto *box = qobject_cast<QGroupBox *>(w)) return box->title();
    if (auto *button = qobject_cast<QAbstractButton *>(w)) return button->text();
    if (auto *label = qobject_cast<QLabel *>(w)) return label->text();
    if (auto *line = qobject_cast<QLineEdit *>(w)) return line->text();
    if (auto *edit = qobject_cast<QPlainTextEdit *>(w)) return edit->toPlainText();
    return {};
  }
  if (key == "value" && u.type == "wfcolor") return colorList(w->property("wfColor").value<QColor>());
  if (key == "value" && u.type == "wfnumber") return qobject_cast<QDoubleSpinBox *>(w)->value();
  if (key == "value") {
    if (auto *box = qobject_cast<QAbstractButton *>(w); box && box->isCheckable()) return box->isChecked();
    if (auto *slider = qobject_cast<QSlider *>(w)) return u.minimum + slider->value() / 1000. * (u.maximum - u.minimum);
    if (auto *bar = qobject_cast<QProgressBar *>(w)) return u.minimum + bar->value() / 1000. * (u.maximum - u.minimum);
    return {};
  }
  if (key == "minvalue") return u.minimum;
  if (key == "maxvalue") return u.maximum;
  if (key == "enabled") return w->isEnabled();
  if (key == "visible") return w->isVisible();
  if (key == "selection") {
    if (auto *combo = qobject_cast<QComboBox *>(w)) return combo->currentIndex() < 0 ? QVariant() : QVariant(combo->currentIndex());
    if (auto *list = qobject_cast<QListWidget *>(w)) {
      QVariantList rows;
      for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->isSelected()) rows << i;
      return rows;
    }
    if (auto *tabs = qobject_cast<QTabWidget *>(w)) return tabs->currentIndex();
    return {};
  }
  if (key == "size") return QVariantList{w->width(), w->height()};
  if (key == "bounds") return QVariantList{w->x(), w->y(), w->x() + w->width(), w->y() + w->height()};
  if (key == "location") return QVariantList{w->x(), w->y()};
  if (key == "result") return u.result;
  return {};
}

QVariant ScriptBridge::uiShow(int id) {
  auto found = s.ui.find(id);
  if (found == s.ui.end() || !found->second.widget) return 0;
  ScriptSession::Ui &u = found->second;
  if (s.touched && s.host.refresh) s.host.refresh();
  auto *window = qobject_cast<QDialog *>(u.widget.data());
  if (!window) {
    u.widget->show();
    return 0;
  }
  window->adjustSize();
  s.dispatch(id, "show");
  if (u.type == "dialog") {
    if (s.unattended) {
      // Shown, not waited for: the self-tests press its buttons themselves.
      window->show();
      return 1;
    }
    u.result = 0;
    const int code = window->exec();
    const int result = s.ui.count(id) ? s.ui[id].result : code;
    return result ? result : (code == QDialog::Accepted ? 1 : 2);
  }
  window->show();
  window->raise();
  return 0;
}

void ScriptBridge::uiHide(int id) {
  if (s.ui.count(id) && s.ui[id].widget) s.ui[id].widget->hide();
}

void ScriptBridge::uiClose(int id, int result) {
  auto found = s.ui.find(id);
  if (found == s.ui.end() || !found->second.widget) return;
  found->second.result = result;
  if (auto *window = qobject_cast<QDialog *>(found->second.widget.data())) {
    if (window->isModal() || found->second.type == "dialog") window->done(result == 2 ? QDialog::Rejected : QDialog::Accepted);
    window->close();
  } else {
    found->second.widget->hide();
  }
}

void ScriptBridge::uiItems(int id, const QStringList &items) {
  auto found = s.ui.find(id);
  if (found == s.ui.end() || !found->second.widget) return;
  QWidget *w = found->second.widget;
  if (auto *combo = qobject_cast<QComboBox *>(w)) {
    QSignalBlocker block(combo);
    const int keep = combo->currentIndex();
    combo->clear();
    for (const QString &item : items) {
      if (item == "-") combo->insertSeparator(combo->count());
      else combo->addItem(item);
    }
    combo->setCurrentIndex(keep < combo->count() ? keep : -1);
  } else if (auto *list = qobject_cast<QListWidget *>(w)) {
    QSignalBlocker block(list);
    list->clear();
    list->addItems(items);
  }
}

void ScriptBridge::uiRemove(int id) {
  auto found = s.ui.find(id);
  if (found == s.ui.end()) return;
  if (found->second.widget && !found->second.external) found->second.widget->deleteLater();
  s.ui.erase(found);
}

void ScriptBridge::uiLayout(int id) {
  auto found = s.ui.find(id);
  if (found == s.ui.end() || !found->second.widget) return;
  if (found->second.widget->layout()) found->second.widget->layout()->activate();
  if (qobject_cast<QDialog *>(found->second.widget.data())) found->second.widget->adjustSize();
}

// An event from a control, handed to the script (scriptui.js's __wfUi).
void ScriptSession::dispatch(int id, const QString &event, const QVariant &argument) {
  Entry entry(*this);
  QJSValue call = engine.globalObject().property("__wfUi");
  if (!call.isCallable()) return;
  reportCallbackError(call.call({id, event, engine.toScriptValue(argument)}));
}

namespace {
// Every script alive, for the events they listen for.
QList<ScriptSession *> &liveSessions() {
  static QList<ScriptSession *> sessions;
  return sessions;
}
QTimer *eventPoll() {
  static QPointer<QTimer> timer;
  if (!timer) {
    timer = new QTimer(qApp);
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, [] { pollScriptEvents(); });
  }
  return timer;
}
} // namespace

ScriptSession::ScriptSession(ScriptHost h) : host(std::move(h)) { liveSessions().append(this); }

void ScriptSession::emitEvent(const QString &event, const QVariant &data) {
  Entry entry(*this);
  QJSValue call = engine.globalObject().property("__wfEvent");
  if (call.isCallable()) reportCallbackError(call.call({event, engine.toScriptValue(data)}));
}

// What changed in the editor since it last looked, told to the script.
void ScriptSession::poll() {
  if (listening.isEmpty() || depth > 0) return;
  const Scene &sc = scene();
  if (listening.contains("frame") && sc.frame != seenFrame) {
    seenFrame = sc.frame;
    emitEvent("frame", seenFrame);
  }
  if (listening.contains("selection") && host.selectedLayers) {
    const QStringList now = host.selectedLayers();
    if (now != seenSelection) {
      seenSelection = now;
      emitEvent("selection", now);
    }
  }
  if ((listening.contains("play") || listening.contains("stop")) && host.playing) {
    const bool now = host.playing();
    if (now != seenPlaying) {
      seenPlaying = now;
      emitEvent(now ? "play" : "stop", scene().frame);
    }
  }
  if (listening.contains("change") && host.revision) {
    const quint64 now = host.revision();
    if (now != seenRevision) {
      seenRevision = now;
      emitEvent("change", {});
    }
  }
}

// wavy.script.end, and the end of the runner: its timers, its menu items,
// what it listened for.
void ScriptSession::endAll() {
  for (auto &[id, timer] : timers) delete timer;
  timers.clear();
  listening.clear();
  for (auto &[id, action] : commands) delete action.data();
  commands.clear();
  for (const QPointer<QMenu> &menu : madeMenus)
    if (menu && menu->actions().isEmpty()) delete menu.data();
  madeMenus.clear();
  for (auto &[id, u] : ui)
    if (u.dock && u.widget)
      if (auto *d = qobject_cast<QDockWidget *>(u.widget->parentWidget())) d->deleteLater();
}

void notifyScripts(const QString &event, const QVariant &data) {
  const QList<ScriptSession *> sessions = liveSessions();
  for (ScriptSession *s : sessions)
    if (liveSessions().contains(s) && s->listening.contains(event)) s->emitEvent(event, data);
}

void pollScriptEvents() {
  const QList<ScriptSession *> sessions = liveSessions();
  bool any = false;
  for (ScriptSession *s : sessions)
    if (liveSessions().contains(s)) {
      s->poll();
      any |= liveSessions().contains(s) && !s->listening.isEmpty();
    }
  if (!any) eventPoll()->stop();
}

ScriptSession::~ScriptSession() {
  liveSessions().removeAll(this);
  endAll();
  for (auto &[id, u] : ui)
    if (u.widget && !u.external && !u.parent) delete u.widget.data();
  for (auto &[id, u] : ui)
    if (u.widget && u.external) {
      // A docked panel the editor gave: emptied, left to the editor.
      if (QLayout *layout = u.widget->layout()) {
        while (QLayoutItem *item = layout->takeAt(0)) {
          delete item->widget();
          delete item;
        }
        delete layout;
      }
    }
}

// ---------------------------------------------------------------------------
// The runner.
// ---------------------------------------------------------------------------
namespace {
QString resourceText(const QString &path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}
} // namespace

ScriptRunner::ScriptRunner(ScriptHost host) : session(std::make_unique<ScriptSession>(std::move(host))) {
  ScriptSession &s = *session;
  s.engine.installExtensions(QJSEngine::ConsoleExtension | QJSEngine::GarbageCollectionExtension);
  s.bridge = new ScriptBridge(s);
  QJSEngine::setObjectOwnership(s.bridge, QJSEngine::CppOwnership);
  s.engine.globalObject().setProperty("__wf", s.engine.newQObject(s.bridge));
  for (const QString &part : {QStringLiteral(":/scripting/wavy.js"), QStringLiteral(":/scripting/scriptui.js"),
                              QStringLiteral(":/scripting/wavyui.js"), QStringLiteral(":/scripting/ae.js")}) {
    const QJSValue result = s.engine.evaluate(resourceText(part), part, 1);
    if (result.isError())
      qWarning("Scripting: %s line %d: %s", qPrintable(part), result.property("lineNumber").toInt(),
               qPrintable(result.property("message").toString()));
  }
}

ScriptRunner::~ScriptRunner() {
  if (session) delete session->bridge;
}

bool ScriptRunner::runText(const QString &code, const QString &fileName, QString *error) {
  ScriptSession &s = *session;
  ScriptSession::Entry entry(s);
  QSet<QString> seen;
  QString text;
  s.origins.clear();
  if (!preprocess(fileName, code, {}, seen, text, s.origins, error)) return false;
  s.origin = 0;
  // Who is running: $.fileName, and the folder #include looked in.
  s.engine.globalObject().property("__wfBegin").call({QFileInfo(fileName).absoluteFilePath()});
  return s.evaluate(text, fileName, error);
}

bool ScriptRunner::runFile(const QString &path, QString *error) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    if (error) *error = "Cannot read " + path;
    return false;
  }
  QString code = QString::fromUtf8(f.readAll());
  if (code.startsWith(QChar(0xFEFF))) code.remove(0, 1);
  return runText(code, QFileInfo(path).absoluteFilePath(), error);
}

bool ScriptRunner::runPanel(const QString &path, QWidget *panel, QString *error) {
  ScriptSession &s = *session;
  ScriptSession::Entry entry(s);
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    if (error) *error = "Cannot read " + path;
    return false;
  }
  QString code = QString::fromUtf8(f.readAll());
  if (code.startsWith(QChar(0xFEFF))) code.remove(0, 1);
  QSet<QString> seen;
  QString text;
  s.origins.clear();
  const QString file = QFileInfo(path).absoluteFilePath();
  if (!preprocess(file, code, {}, seen, text, s.origins, error)) return false;
  s.engine.globalObject().property("__wfBegin").call({file});
  const int id = s.bridge->adoptPanel(panel);
  const QJSValue panelObject = s.engine.globalObject().property("__wfPanel").call({id, QFileInfo(path).completeBaseName()});
  return s.evaluate(text, file, error, panelObject);
}

bool ScriptRunner::alive() const {
  const ScriptSession &s = *session;
  if (!s.timers.empty() || !s.listening.isEmpty() || !s.commands.empty()) return true;
  for (const auto &[id, u] : s.ui)
    if (u.widget && !u.parent && (u.external || u.widget->isVisible())) return true;
  return false;
}

QStringList ScriptRunner::output() const { return session->output; }

QString ScriptRunner::evaluate(const QString &expression) {
  ScriptSession::Entry entry(*session);
  const QJSValue result = session->engine.evaluate(expression, "evaluate", 1);
  if (result.isError()) return "Error: " + result.property("message").toString();
  if (result.isObject() && !result.isCallable()) {
    QJSValue json = session->engine.globalObject().property("JSON").property("stringify");
    return json.call({result}).toString();
  }
  return result.toString();
}

void ScriptRunner::setUnattended(bool on) { session->unattended = on; }

namespace {
// The newest window's control by its name, or by the text it shows.
QWidget *controlIn(ScriptSession &s, const QString &control, int *id) {
  for (auto it = s.ui.rbegin(); it != s.ui.rend(); ++it) {
    QWidget *w = it->second.widget;
    if (!w) continue;
    QString text;
    if (auto *button = qobject_cast<QAbstractButton *>(w)) text = button->text();
    if (w->objectName() == control || (!text.isEmpty() && text == control)) {
      *id = it->first;
      return w;
    }
  }
  return nullptr;
}
} // namespace

bool ScriptRunner::press(const QString &control) {
  int id = 0;
  QWidget *w = controlIn(*session, control, &id);
  auto *button = qobject_cast<QAbstractButton *>(w);
  if (!button) return false;
  button->click();
  return true;
}

bool ScriptRunner::setControlValue(const QString &control, const QVariant &value) {
  int id = 0;
  QWidget *w = controlIn(*session, control, &id);
  if (!w) return false;
  if (auto *slider = qobject_cast<QSlider *>(w)) {
    const auto &u = session->ui[id];
    slider->setValue(int(std::round((value.toDouble() - u.minimum) / std::max(1e-9, u.maximum - u.minimum) * 1000)));
    session->dispatch(id, "change");
    return true;
  }
  if (auto *line = qobject_cast<QLineEdit *>(w)) {
    line->setText(value.toString());
    session->dispatch(id, "change");
    return true;
  }
  if (auto *combo = qobject_cast<QComboBox *>(w)) {
    combo->setCurrentIndex(value.toInt());
    session->dispatch(id, "change");
    return true;
  }
  if (auto *box = qobject_cast<QAbstractButton *>(w); box && box->isCheckable()) {
    if (box->isChecked() != value.toBool()) box->click();
    return true;
  }
  return false;
}

QStringList scriptFolders() {
  return {QDir(QCoreApplication::applicationDirPath()).filePath("Scripts"),
          QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath("Wavy-Flo/Scripts")};
}

#include "script_host.moc"
