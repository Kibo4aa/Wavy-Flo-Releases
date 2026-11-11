#pragma once
// Scripts in Wavy-Flo. One JavaScript engine (Qt's QJSEngine), two ways in:
//
//  * wavy -- Wavy-Flo's own scripting API (resources/scripting/wavy.js),
//    with nothing held back: every layer, every property and keyframe (its
//    handles, its motion path, its timing keys), every effect and its
//    settings, text animation, particles, masks, shapes, markers, rendering,
//    files, programs, timers and windows. A layer can be read and changed
//    whole, as the project file has it (layer.data / layer.patch), so what
//    Wavy-Flo learns to do a script can do too.
//
//  * After Effects' own (ExtendScript, .jsx): app.project, CompItem, layers,
//    properties, TextDocument, ScriptUI, File, Folder, $ ...
//    (resources/scripting/ae.js, scriptui.js), laid over the same calls, so
//    scripts written for After Effects run here as far as Wavy-Flo has what
//    they ask for. Wavy-Flo answers as itself (app.name "Wavy-Flo").
//
// Both stand on a small native bridge (script_host.cpp) that reads and
// changes the scene by layer id, reaches files, dialogs, settings and
// programs, and builds the widgets. A run is one step Undo takes back.
#include "engine.h"
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <functional>
#include <memory>

class QWidget;

// What a script reaches of the editor.
struct ScriptHost {
  std::function<Scene &()> scene;                        // the project as it is
  std::function<QString()> mainName;                     // the Main's name
  std::function<QString()> activeComposition;            // "" for the Main, or the group shown
  std::function<void(const QString &)> openComposition;  // show the Main ("") or a group
  std::function<QStringList()> selectedLayers;           // layer ids, top first
  std::function<void(const QStringList &)> selectLayers; // and set
  std::function<void()> checkpoint;                      // before the first change of a run
  std::function<void()> refresh;                         // after a run that changed something
  std::function<QStringList()> effectNames;              // what can be added as an effect
  // Every effect, as wavy.effects tells of it: name (as the project keeps it),
  // label, category, kind ("pack", "plugin", "openfx", "built-in") and its
  // parameters (id, label, kind, min, max, default, step, choices, unit ...).
  std::function<QVariantList()> effectInfo;
  // An effect put on a layer the way the Effects panel does (its defaults
  // filled in); false when there is no such effect.
  std::function<bool(const QString &layerId, const QString &effect)> applyEffect;
  std::function<QString()> projectFile;                  // the project's path, empty if unsaved
  std::function<bool(const QString &)> saveProject;      // to this path ("" = where it is)
  std::function<void(const QStringList &)> importFiles;  // pictures, video, sound into the project
  std::function<void()> undo, redo;
  std::function<void(const QString &)> log;              // $.writeln, wavy.log
  std::function<bool()> playing;                         // whether the preview plays
  std::function<void(bool)> setPlaying;                  // and play or stop it
  // Goes up with every change to the project (and every Undo and Redo): how
  // wavy.on("change") sees one.
  std::function<quint64()> revision;
  // Runs what the editor does for a menu item as part of the script's own Undo
  // step, rather than one of the editor's.
  std::function<void(const std::function<void()> &)> withinStep;
  // wavy.ui.panel: a panel docked in the editor, titled; its content widget,
  // which the script fills. Gone when the script ends.
  std::function<QWidget *(const QString &)> dockPanel;
  // Rendering a movie: how an export draws a frame (the editor's own renderer).
  std::function<std::function<QImage(Scene &)>()> renderer;
  // The Render Queue: its items (render_queue.h's JSON, with state, done,
  // total, message and target), one added, one taken out, all run or stopped.
  std::function<QVariantList()> renderQueueItems;
  std::function<bool(const QJsonObject &)> renderQueueAdd;
  std::function<bool(const QString &)> renderQueueRemove;
  std::function<void(bool)> renderQueueRun;
  std::function<bool()> renderQueueBusy;
  // The ready-made Text Animation presets and the person's own: each
  // {name, settings, in?, out?, inBase?, outBase?}.
  std::function<QVariantList()> textPresets;
  QString version;                                       // Wavy-Flo's own
  QWidget *window = nullptr;                             // the parent of its dialogs; the editor's own
                                                         // window, whose menus wavy.app reaches
};

class ScriptSession;

// One script. A run that leaves a palette or a docked panel open keeps its
// engine alive with it (alive()); the editor keeps the runner until then.
class ScriptRunner {
public:
  explicit ScriptRunner(ScriptHost host);
  ~ScriptRunner();
  ScriptRunner(const ScriptRunner &) = delete;
  ScriptRunner &operator=(const ScriptRunner &) = delete;
  // The script in the file at `path`, or `code` as if read from `fileName`.
  // False, with `error` ("Line 12: ..."), when it throws or cannot be read.
  bool runFile(const QString &path, QString *error = nullptr);
  bool runText(const QString &code, const QString &fileName, QString *error = nullptr);
  // A docked panel: the script run with `this` a Panel laid into `panel`, as
  // After Effects runs those in its ScriptUI Panels folder.
  bool runPanel(const QString &path, QWidget *panel, QString *error = nullptr);
  // Whether a window it opened (a palette, a docked panel) or a timer is still there.
  bool alive() const;
  // What it wrote with $.writeln / wavy.log.
  QStringList output() const;
  // For the self-tests: an expression evaluated in the script's world, as text.
  QString evaluate(const QString &expression);
  // For the self-tests: pretend the person pressed a control (by its ScriptUI
  // name or its text) in the newest window, or set its value.
  bool press(const QString &control);
  bool setControlValue(const QString &control, const QVariant &value);
  // For the self-tests: dialogs answered at once -- alert passed, confirm
  // yes, prompt its preset, a modal window shown without waiting.
  void setUnattended(bool on);

private:
  std::unique_ptr<ScriptSession> session;
};

// Where scripts live: "Scripts" beside Wavy-Flo.exe and the person's own
// (Documents/Wavy-Flo/Scripts), each with "ScriptUI Panels" inside for the
// docked panels.
QStringList scriptFolders();

// What the scripts listening (wavy.on) are told of: "save" and "open" with
// the project's path, "new". The playhead, the selection, playing and
// changes they see for themselves (pollScriptEvents, a few times a second).
void notifyScripts(const QString &event, const QVariant &data = {});
void pollScriptEvents();

// --scripts-self-test.
int checkScripts(QStringList &failures);
// --jsxbin-self-test <folder>: the compiled scripts under it read back and compiled.
int checkJsxbinFolder(const QString &folder, QStringList &report, bool context);
// --script-self-test-run <file>: a script on an empty project of its own, no editor.
int runScriptAlone(const QString &file, QStringList &report, std::function<QVariantList()> textPresets = {});
