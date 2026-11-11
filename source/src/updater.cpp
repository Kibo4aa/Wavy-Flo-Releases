#include "updater.h"
#include <QApplication>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

int compareVersions(const QString &a, const QString &b) {
  auto parts = [](QString v) {
    v = v.trimmed();
    if (v.startsWith('v', Qt::CaseInsensitive)) v.remove(0, 1);
    QList<int> numbers;
    for (const QString &piece : v.split('.')) {
      // "0.25-beta" counts as 0.25: the digits a piece starts with.
      int value = 0, i = 0;
      while (i < piece.size() && piece[i].isDigit()) value = value * 10 + piece[i++].digitValue();
      numbers << value;
    }
    return numbers;
  };
  const QList<int> x = parts(a), y = parts(b);
  for (int i = 0; i < std::max(x.size(), y.size()); ++i) {
    const int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
    if (p != q) return p < q ? -1 : 1;
  }
  return 0;
}

bool installedWithSetup() {
  // Setup records where it installed in Settings > Apps.
  const QString dir = QSettings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WavyFlo",
                                QSettings::NativeFormat).value("InstallLocation").toString();
  return !dir.isEmpty() && QDir::cleanPath(dir).compare(QDir::cleanPath(QCoreApplication::applicationDirPath()),
                                                        Qt::CaseInsensitive) == 0;
}

UpdateInfo parseUpdateManifest(const QByteArray &json, const QUrl &from) {
  UpdateInfo info;
  QJsonObject o = QJsonDocument::fromJson(json).object();
  if (o.isEmpty()) return info;
  // A list of every version: the newest of them that is not a preview, read
  // as one version of our own form.
  if (o.value("versions").isArray()) {
    QJsonObject newest;
    for (const auto &v : o.value("versions").toArray()) {
      const QJsonObject entry = v.toObject();
      if (entry.value("preview").toBool() || entry.value("url").toString().isEmpty()) continue;
      if (newest.isEmpty() || compareVersions(entry.value("version").toString(), newest.value("version").toString()) > 0)
        newest = entry;
    }
    if (newest.isEmpty()) return info;
    o = newest;
  }
  static const QRegularExpression hex64("([0-9a-fA-F]{64})");
  if (o.contains("tag_name")) {
    // A GitHub release: the installer if there is one, else a zip, else
    // whatever it carries first.
    info.version = o.value("tag_name").toString();
    if (info.version.startsWith('v', Qt::CaseInsensitive)) info.version.remove(0, 1);
    info.notes = o.value("body").toString();
    QJsonObject chosen;
    const QJsonArray assets = o.value("assets").toArray();
    // A copy Setup installed is updated by Setup; one unpacked from a zip,
    // from the zip -- so it stays a folder of files and nothing more.
    const char *order[2] = {".exe", ".zip"};
    if (!installedWithSetup()) std::swap(order[0], order[1]);
    for (const char *ending : order) {
      for (const auto &v : assets)
        if (v.toObject().value("name").toString().endsWith(ending, Qt::CaseInsensitive)) { chosen = v.toObject(); break; }
      if (!chosen.isEmpty()) break;
    }
    if (chosen.isEmpty() && !assets.isEmpty()) chosen = assets.first().toObject();
    info.url = chosen.value("browser_download_url").toString();
    info.fileName = chosen.value("name").toString();
    info.size = qint64(chosen.value("size").toDouble());
    // Its checksum: GitHub's own ("digest": "sha256:..."), or one written in
    // the notes next to "SHA-256".
    const QString digest = chosen.value("digest").toString();
    if (digest.startsWith("sha256:")) info.sha256 = digest.mid(7);
    else {
      const QRegularExpression written("sha-?256[^0-9a-fA-F]{0,40}([0-9a-fA-F]{64})",
                                       QRegularExpression::CaseInsensitiveOption);
      const auto found = written.match(info.notes);
      if (found.hasMatch()) info.sha256 = found.captured(1);
    }
  } else {
    info.version = o.value("version").toString();
    if (info.version.startsWith('v', Qt::CaseInsensitive)) info.version.remove(0, 1);
    info.notes = o.value("notes").toString();
    info.url = o.value("url").toString();
    info.sha256 = o.value("sha256").toString();
    info.size = qint64(o.value("size").toDouble());
    info.fileName = QFileInfo(QUrl(info.url).path()).fileName();
  }
  if (from.isValid() && !info.url.isEmpty() && QUrl(info.url).isRelative()) info.url = from.resolved(QUrl(info.url)).toString();
  if (!hex64.match(info.sha256).hasMatch() || info.sha256.size() != 64) info.sha256.clear();
  info.sha256 = info.sha256.toLower();
  if (info.fileName.isEmpty()) info.fileName = "Wavy-Flo-" + info.version + ".exe";
  // Nothing in it that could reach outside the downloads folder.
  info.fileName = QFileInfo(info.fileName).fileName();
  info.valid = !info.version.isEmpty() && QUrl(info.url).isValid() && !info.url.isEmpty();
  return info;
}

// The account was renamed from Keblackan: the old name only redirects here, and
// whoever claimed it would answer every copy's update check.
QString defaultUpdateSource() { return "https://api.github.com/repos/Kibo4aa/Wavy-Flo-Releases/releases/latest"; }
QString updateSource() {
  const QString env = qEnvironmentVariable("WAVY_FLO_UPDATE_SOURCE");
  if (!env.isEmpty()) return env;
  const QString set = QSettings("Wavy-Flo", "Wavy-Flo").value("update/source").toString().trimmed();
  return set.isEmpty() ? defaultUpdateSource() : set;
}
namespace {
// An address as written: on the web, or a file or folder here (a folder holds versions.json).
QUrl sourceUrl(const QString &source) {
  if (source.contains("://")) return QUrl(source);
  if (QFileInfo(source).isDir()) return QUrl::fromLocalFile(QDir(source).filePath("versions.json"));
  return QUrl::fromLocalFile(source);
}
}  // namespace
void setUpdateSource(const QString &address) {
  QSettings("Wavy-Flo", "Wavy-Flo").setValue("update/source", address.trimmed());
}
bool updateCheckOnStart() { return QSettings("Wavy-Flo", "Wavy-Flo").value("update/onStart", true).toBool(); }
void setUpdateCheckOnStart(bool on) { QSettings("Wavy-Flo", "Wavy-Flo").setValue("update/onStart", on); }

QString zipInstallScript() {
  // Waits for Wavy-Flo to have closed, unpacks the zip beside it, copies it
  // over the program's folder (robocopy: folders merged, files replaced,
  // retried while one is still let go of), and opens Wavy-Flo again. What
  // happened is written to %TEMP%\wavy-flo-update.log.
  return QStringLiteral(R"PS(param([string]$Zip, [string]$Target, [string]$Exe = "", [int]$WaitPid = 0)
$log = Join-Path $env:TEMP 'wavy-flo-update.log'
"$(Get-Date) updating $Target from $Zip" | Out-File $log
try {
  if ($WaitPid -gt 0) { try { Wait-Process -Id $WaitPid -Timeout 120 -ErrorAction SilentlyContinue } catch {} }
  Start-Sleep -Milliseconds 700
  $stage = Join-Path $env:TEMP ('wavy-flo-update-' + [guid]::NewGuid().ToString('N'))
  Expand-Archive -LiteralPath $Zip -DestinationPath $stage -Force
  # The program's files at the zip's root, or inside one folder.
  $root = $stage
  $items = @(Get-ChildItem -LiteralPath $stage)
  if ($items.Count -eq 1 -and $items[0].PSIsContainer) { $root = $items[0].FullName }
  robocopy $root $Target /E /R:10 /W:1 /NFL /NDL /NJH /NJS /NP | Out-File $log -Append
  "robocopy exit $LASTEXITCODE" | Out-File $log -Append
  Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
} catch {
  "failed: $_" | Out-File $log -Append
}
if ($Exe -ne "") { Start-Process -FilePath $Exe }
)PS");
}
bool startZipInstall(const QString &zip, const QString &target, const QString &exe, qint64 waitPid) {
  const QString script = QDir::temp().filePath("wavy-flo-update.ps1");
  QFile file(script);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  file.write(zipInstallScript().toUtf8());
  file.close();
  QStringList arguments{"-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden", "-File",
                        QDir::toNativeSeparators(script), "-Zip", QDir::toNativeSeparators(zip), "-Target",
                        QDir::toNativeSeparators(target), "-WaitPid", QString::number(waitPid)};
  // An empty argument is refused by PowerShell: no -Exe at all when there is none.
  if (!exe.isEmpty()) arguments << "-Exe" << QDir::toNativeSeparators(exe);
  return QProcess::startDetached("powershell.exe", arguments);
}

namespace {
QNetworkAccessManager &network() {
  static QNetworkAccessManager *manager = new QNetworkAccessManager(qApp);
  return *manager;
}
QNetworkRequest requestFor(const QUrl &url) {
  QNetworkRequest request(url);
  // GitHub refuses a request that does not say what is asking.
  request.setHeader(QNetworkRequest::UserAgentHeader, QString("Wavy-Flo/") + kWavyFloVersion);
  request.setRawHeader("Accept", "application/vnd.github+json, application/json, */*");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setTransferTimeout(30000);
  return request;
}
QString readableSize(double bytes) {
  if (bytes < 1024 * 1024) return QString::number(bytes / 1024, 'f', 0) + " KB";
  return QString::number(bytes / 1024 / 1024, 'f', 1) + " MB";
}

// The Update panel: what is new, and the download.
class UpdatePanel : public QDialog {
public:
  UpdatePanel(QWidget *parent, const UpdateInfo &info, std::function<bool()> closeEditor)
      : QDialog(parent), info(info), closeEditor(std::move(closeEditor)) {
    setObjectName("updatePanel");
    setWindowTitle("Wavy-Flo Update");
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumWidth(460);
    setStyleSheet(
        "QDialog#updatePanel { background: #222222; }"
        "QLabel { color: #DEDEDE; }"
        "QLabel#updateTitle { color: #2D8CFF; font-size: 16px; font-weight: 700; }"
        "QLabel#updateStatus { color: #8A8A8A; }"
        "QTextBrowser { background: #181818; border: 1px solid #333333; border-radius: 4px; color: #DEDEDE; padding: 6px; }"
        "QProgressBar { background: #181818; border: 1px solid #333333; border-radius: 3px; height: 8px; color: transparent; }"
        "QProgressBar::chunk { background: #2D8CFF; border-radius: 3px; }"
        "QPushButton { background: #2A2A2A; border: 1px solid #383838; border-radius: 4px; color: #DEDEDE; padding: 5px 14px; }"
        "QPushButton:hover { border-color: #4A4A4A; color: #FFFFFF; }"
        "QPushButton#updatePrimary { background: #2D8CFF; border-color: #2D8CFF; color: #FFFFFF; font-weight: 700; }"
        "QPushButton#updatePrimary:hover { background: #4D9EFF; }"
        "QPushButton:disabled { color: #555555; }"
        "QCheckBox { color: #8A8A8A; }");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);
    auto *title = new QLabel(QString("Wavy-Flo %1 is available").arg(info.version));
    title->setObjectName("updateTitle");
    auto *versions = new QLabel(QString("You have %1.%2").arg(kWavyFloVersion)
                                    .arg(info.size > 0 ? "  Download: " + readableSize(double(info.size)) + "." : QString()));
    versions->setObjectName("updateStatus");
    auto *notes = new QTextBrowser;
    notes->setObjectName("updateNotes");
    notes->setOpenExternalLinks(true);
    notes->setMarkdown(info.notes.trimmed().isEmpty() ? QString("*No notes for this version.*") : info.notes);
    notes->setMinimumHeight(170);
    progress = new QProgressBar;
    progress->setObjectName("updateProgress");
    progress->setRange(0, 1000);
    progress->setTextVisible(false);
    progress->hide();
    status = new QLabel;
    status->setObjectName("updateStatus");
    status->setWordWrap(true);
    status->hide();
    auto *onStart = new QCheckBox("Check for updates when Wavy-Flo starts");
    onStart->setChecked(updateCheckOnStart());
    connect(onStart, &QCheckBox::toggled, this, [](bool on) { setUpdateCheckOnStart(on); });
    auto *buttons = new QHBoxLayout;
    skip = new QPushButton("Skip This Version");
    skip->setObjectName("updateSkip");
    later = new QPushButton("Later");
    later->setObjectName("updateLater");
    primary = new QPushButton("Update");
    primary->setObjectName("updatePrimary");
    primary->setDefault(true);
    buttons->addWidget(skip);
    buttons->addStretch(1);
    buttons->addWidget(later);
    buttons->addWidget(primary);
    layout->addWidget(title);
    layout->addWidget(versions);
    layout->addWidget(notes, 1);
    layout->addWidget(progress);
    layout->addWidget(status);
    layout->addWidget(onStart);
    layout->addLayout(buttons);
    connect(skip, &QPushButton::clicked, this, [this] {
      QSettings("Wavy-Flo", "Wavy-Flo").setValue("update/skip", this->info.version);
      close();
    });
    connect(later, &QPushButton::clicked, this, [this] {
      if (reply) reply->abort();
      close();
    });
    connect(primary, &QPushButton::clicked, this, [this] { primaryPressed(); });
  }
  ~UpdatePanel() override {
    if (reply) reply->abort();
  }

private:
  UpdateInfo info;
  std::function<bool()> closeEditor;
  QProgressBar *progress = nullptr;
  QLabel *status = nullptr;
  QPushButton *primary = nullptr, *later = nullptr, *skip = nullptr;
  QPointer<QNetworkReply> reply;
  QFile part;
  QString done; // the downloaded file, once it has arrived whole
  bool finishing = false;
  QElapsedTimer clock;

  QString folder() const {
    QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty()) base = QDir::tempPath();
    return QDir(base).filePath("Wavy-Flo Updates");
  }
  void say(const QString &text) {
    status->setText(text);
    status->show();
  }
  void primaryPressed() {
    if (!done.isEmpty()) { finish(); return; }
    if (reply) return;
    // Already here from an earlier time, whole: nothing to download again.
    const QString target = QDir(folder()).filePath(info.fileName);
    if (QFileInfo::exists(target) && !info.sha256.isEmpty() && checksum(target) == info.sha256) {
      arrived(target);
      return;
    }
    QDir().mkpath(folder());
    part.setFileName(target + ".part");
    if (!part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      say("Cannot write to " + QDir::toNativeSeparators(folder()) + ".");
      return;
    }
    primary->setEnabled(false);
    skip->setEnabled(false);
    later->setText("Cancel");
    progress->setValue(0);
    progress->show();
    say("Downloading...");
    clock.start();
    reply = network().get(requestFor(QUrl(info.url)));
    connect(reply, &QNetworkReply::readyRead, this, [this] {
      if (reply) part.write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
      if (total <= 0) total = info.size;
      if (total > 0) progress->setValue(int(1000.0 * double(got) / double(total)));
      const double seconds = std::max(.001, clock.elapsed() / 1000.);
      say(QString("Downloading... %1 of %2  (%3/s)")
              .arg(readableSize(double(got)), total > 0 ? readableSize(double(total)) : QString("?"),
                   readableSize(double(got) / seconds)));
    });
    connect(reply, &QNetworkReply::finished, this, [this, target] {
      QNetworkReply *r = reply;
      reply = nullptr;
      if (!r) return;
      r->deleteLater();
      if (part.isOpen()) {
        part.write(r->readAll());
        part.close();
      }
      later->setText("Later");
      skip->setEnabled(true);
      if (r->error() != QNetworkReply::NoError) {
        QFile::remove(part.fileName());
        primary->setEnabled(true);
        primary->setText("Try Again");
        progress->hide();
        say(r->error() == QNetworkReply::OperationCanceledError ? QString("Download cancelled.")
                                                                : "The download failed: " + r->errorString());
        return;
      }
      // Arrived whole and as published, or not used at all.
      if (!info.sha256.isEmpty() && checksum(part.fileName()) != info.sha256) {
        QFile::remove(part.fileName());
        primary->setEnabled(true);
        primary->setText("Try Again");
        say("The download did not arrive whole (its checksum does not match), so it was thrown away.");
        return;
      }
      QFile::remove(target);
      if (!QFile::rename(part.fileName(), target)) {
        primary->setEnabled(true);
        say("Could not keep the download in " + QDir::toNativeSeparators(folder()) + ".");
        return;
      }
      arrived(target);
    });
  }
  static QString checksum(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return QString::fromLatin1(hash.result().toHex());
  }
  bool installer() const { return done.endsWith(".exe", Qt::CaseInsensitive) || done.endsWith(".msi", Qt::CaseInsensitive); }
  bool zipped() const { return done.endsWith(".zip", Qt::CaseInsensitive); }
  void arrived(const QString &path) {
    done = path;
    progress->setValue(1000);
    progress->show();
    primary->setEnabled(true);
    primary->setText(installer() || zipped() ? "Close && Install" : "Show in Folder");
    say(QString("Downloaded%1: %2")
            .arg(info.sha256.isEmpty() ? QString() : QString(" and checked"), QDir::toNativeSeparators(path)));
    // Update means update: once it is here, Wavy-Flo closes (asking about
    // unsaved work first), puts it in place and opens again. Kept open, the
    // panel offers "Close & Install" for later.
    if (installer() || zipped()) {
      say("Downloaded. Wavy-Flo closes, updates and opens again...");
      QTimer::singleShot(700, this, [this] { finish(); });
    }
  }
  void finish() {
    if (!installer() && !zipped()) {
      QProcess::startDetached("explorer.exe", {"/select,", QDir::toNativeSeparators(done)});
      close();
      return;
    }
    const QString path = done;
    if (finishing) return; // pressed again while it already goes ahead
    // The running program cannot be replaced: Wavy-Flo closes (asking about
    // unsaved work first), then the installer -- or, for a zip, a small script
    // that unpacks it over this folder and opens Wavy-Flo again -- starts.
    if (closeEditor && !closeEditor()) {
      say("Wavy-Flo is still open, so the update has not started. Press Close & Install when ready.");
      return;
    }
    finishing = true;
    // Wavy-Flo's own Setup, told to update this very folder once this
    // Wavy-Flo has gone, and to open it again after.
    if (installer())
      QProcess::startDetached(path, {"--update", "--dir", QDir::toNativeSeparators(QCoreApplication::applicationDirPath()),
                                     "--wait-pid", QString::number(QCoreApplication::applicationPid())});
    else startZipInstall(path, QCoreApplication::applicationDirPath(), QCoreApplication::applicationFilePath(),
                         QCoreApplication::applicationPid());
    qApp->quit();
  }
};

// One check at a time.
QPointer<QNetworkReply> &pendingCheck() {
  static QPointer<QNetworkReply> pending;
  return pending;
}
}  // namespace

void checkForUpdates(QWidget *parent, bool quiet, std::function<bool()> closeEditor) {
  if (pendingCheck()) return;
  const QUrl address = sourceUrl(updateSource());
  if (!address.isValid() || address.isEmpty()) {
    if (!quiet) QMessageBox::information(parent, "Wavy-Flo Update", "No update address is set (Help > Update Address).");
    return;
  }
  QNetworkReply *reply = network().get(requestFor(address));
  pendingCheck() = reply;
  QPointer<QWidget> owner(parent);
  QObject::connect(reply, &QNetworkReply::finished, reply, [reply, owner, quiet, closeEditor, address] {
    reply->deleteLater();
    pendingCheck() = nullptr;
    if (!owner) return;
    if (reply->error() != QNetworkReply::NoError) {
      // Reached, but nothing published there yet: a new releases page.
      if (!quiet && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404) {
        QMessageBox::information(owner, "Wavy-Flo Update",
                                 QString("No version has been published yet. Wavy-Flo %1 is what there is.\n\n%2")
                                     .arg(kWavyFloVersion, updateSource()));
        return;
      }
      if (!quiet)
        QMessageBox::warning(owner, "Wavy-Flo Update",
                             "Could not ask for updates:\n" + reply->errorString() + "\n\n" + updateSource());
      return;
    }
    const UpdateInfo info = parseUpdateManifest(reply->readAll(), address);
    if (!info.valid) {
      if (!quiet) QMessageBox::warning(owner, "Wavy-Flo Update", "The update address did not answer with a version to download.");
      return;
    }
    if (compareVersions(info.version, kWavyFloVersion) <= 0) {
      if (!quiet)
        QMessageBox::information(owner, "Wavy-Flo Update", QString("Wavy-Flo %1 is the newest version.").arg(kWavyFloVersion));
      return;
    }
    if (quiet && QSettings("Wavy-Flo", "Wavy-Flo").value("update/skip").toString() == info.version) return;
    auto *panel = new UpdatePanel(owner, info, closeEditor);
    panel->show();
    panel->raise();
    panel->activateWindow();
  });
}
