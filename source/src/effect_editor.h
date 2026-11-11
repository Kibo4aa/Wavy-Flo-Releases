#pragma once
// Writing an effect inside Wavy-Flo. An effect written as text (.wfxs,
// sdk/EFFECT-TEXT.md) opens in a window of its own: checked as it is typed,
// drawn in the preview as it is typed (Live), saved where Wavy-Flo reads it,
// and checked against the card and timed (Check). New Effect writes one from a
// starting point into the person's own folder, Documents/Wavy-Flo/Effects,
// which is watched: an effect changed there by another program is read again.
#include <QString>
#include <QStringList>
#include <functional>

class QObject;
class QWidget;

namespace effect_editor {
// What the editor needs of the main window.
struct Host {
  // These effects changed in the registry -- added, read again, taken away:
  // the effect list, Effect Setting, the timeline and the preview follow.
  std::function<void(const QStringList &ids)> effectsChanged;
  // Puts the effect on the selected layer; why not, when it cannot.
  std::function<QString(const QString &id)> applyToSelected;
  // A line in the status bar.
  std::function<void(const QString &message, int ms)> status;
};
void setHost(Host host);

// Opens the window for the effect written as text in `file`, or brings the
// one already open to the front.
void open(QWidget *parent, const QString &file);
// Asks for a name, a category and a starting point, writes the effect into
// the person's folder, reads it in and opens it. Its file, or nothing when
// cancelled.
QString createNew(QWidget *parent, const QString &category = {});
// An effect that is not the person's own (one that came with Wavy-Flo) is
// copied into their folder under a name and id of its own, and opened. Its
// file, or nothing.
QString duplicate(QWidget *parent, const QString &file);
// Whether `file` is in the person's own folder.
bool isUsersOwn(const QString &file);
// Asks every window with changes not saved what becomes of them. False when
// the person cancelled.
bool closeAll();
// Starts watching the person's folder. `owner` keeps the watcher.
void watchUserFolder(QObject *owner);
// Reads every effect written as text again -- those that came with Wavy-Flo
// and the person's own -- and any new in their folder. Returns the ids that
// changed.
QStringList readAgain();
// The effects that were found but did not load, with why; one written as text
// opens from there.
void showProblems(QWidget *parent);
// The person's folder in Explorer (made if missing); a file shown in its folder.
void openFolder();
void reveal(const QString &file);

// The starting points New Effect offers, and the text of each.
QStringList starterNames();
QString starterText(int index, const QString &id, const QString &name, const QString &category);
// `text` with one more parameter of `kind` (float, angle, bool, choice,
// color, curve, point, group) in its description, named so no other is; the
// id given in `id`. Null when the description cannot be found.
QString withParameter(const QString &text, const QString &kind, QString *id = nullptr);
// The line of the file a problem names ("line 12: ...", or where in the
// description JSON stopped making sense), or 0.
int problemLine(const QString &text, const QString &problem);
// For --effect-packs-self-test: the window on an effect in a folder of its
// own -- typed into, wrong, saved, changed elsewhere, closed. Failures.
int checkWindow(QStringList &report);
}  // namespace effect_editor
