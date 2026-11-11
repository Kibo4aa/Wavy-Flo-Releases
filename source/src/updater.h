#pragma once
// Updates: when Wavy-Flo opens it asks one address whether a newer version is
// out, and if so shows the Update panel -- what is new, and a button that
// downloads it, checks it arrived whole, and installs it once Wavy-Flo closes.
//
// The address answers with either
//   * a GitHub release ("https://api.github.com/repos/<owner>/<repo>/releases/latest"):
//     its tag is the version, its notes what is new, its .exe (or .zip) the download; or
//   * a small JSON file of our own, anywhere on the web:
//     {"version": "0.25", "notes": "What is new...", "url": "https://.../Wavy-Flo-0.25-Setup.exe",
//      "sha256": "<64 hex digits>", "size": 123456}; or
//   * a list of every version, {"versions": [ ...entries of that form... ]}, of
//     which the newest that is not a preview is the one offered.
// A url may be relative to the address it was read from. The address may be a
// file or folder on this computer too (a folder holds versions.json).
// Nothing else -- no account, no licence, no server of our own to run.
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QWidget>
#include <functional>

// The version this build is. Raised by hand for each release.
inline const char *kWavyFloVersion = "0.43.1";

// -1, 0 or 1 as `a` is older than, the same as or newer than `b` ("0.9" < "0.10", "v1.2" == "1.2.0").
int compareVersions(const QString &a, const QString &b);

struct UpdateInfo {
  bool valid = false;
  QString version, notes, url, fileName, sha256; // sha256 empty when the source gives none
  qint64 size = 0;
};
// What a source said, in any of its forms; `valid` false when it says nothing
// usable. A relative url is made whole against `from`, the address read.
UpdateInfo parseUpdateManifest(const QByteArray &json, const QUrl &from = QUrl());

// Whether this copy of Wavy-Flo is the one Wavy-Flo Setup installed (it is then
// updated by a Setup); a copy unpacked from a zip is updated from a zip.
bool installedWithSetup();

// Where updates are asked for: the one set in Help > Update Address, the
// environment's WAVY_FLO_UPDATE_SOURCE (for testing), or the built-in one.
QString updateSource();
void setUpdateSource(const QString &address);
QString defaultUpdateSource();

// Asks the source. Quietly (when Wavy-Flo opens) nothing is said unless there
// is something newer -- not that there is none, nor that the network is down
// -- and a version skipped stays skipped; asked from the menu it always says.
// `closeEditor` closes Wavy-Flo (asking about unsaved work) and says whether it did.
void checkForUpdates(QWidget *parent, bool quiet, std::function<bool()> closeEditor);
// A downloaded zip put in place: once process `waitPid` has ended, the zip is
// unpacked over `target` and `exe` (when given) opened again. Runs on its own,
// after Wavy-Flo has closed.
bool startZipInstall(const QString &zip, const QString &target, const QString &exe, qint64 waitPid);
QString zipInstallScript();
// Whether Wavy-Flo looks for updates as it opens.
bool updateCheckOnStart();
void setUpdateCheckOnStart(bool on);
