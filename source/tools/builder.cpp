#include <QtWidgets>
#include <QProcess>

struct BuildDiagnostic {
  QString severity, code, message, file;
  int line = 0, column = 0;
};

static QList<BuildDiagnostic> parseDiagnostics(const QString &output) {
  QList<BuildDiagnostic> result;
  const QRegularExpression msvc(
      R"(^(.+)\((\d+)(?:,(\d+))?\):\s*(fatal error|error|warning)\s+([A-Za-z]+\d+):\s*(.+)$)",
      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpression gcc(
      R"(^(.+):(\d+):(\d+):\s*(fatal error|error|warning):\s*(.+)$)",
      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpression linker(
      R"(^(.+?)\s*:\s*(fatal error|error|warning)\s+([A-Za-z]+\d+):\s*(.+)$)",
      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpression cmake(
      R"(^CMake\s+(Error|Warning)\s+at\s+(.+):(\d+)(?:\s+\([^)]*\))?:?\s*(.*)$)",
      QRegularExpression::CaseInsensitiveOption);
  for (QString line : output.split('\n')) {
    line = line.trimmed();
    if (line.isEmpty()) continue;
    BuildDiagnostic diagnostic;
    auto match = msvc.match(line);
    if (match.hasMatch()) {
      diagnostic.file = QDir::fromNativeSeparators(match.captured(1));
      diagnostic.line = match.captured(2).toInt();
      diagnostic.column = match.captured(3).toInt();
      diagnostic.severity = match.captured(4).toLower();
      diagnostic.code = match.captured(5).toUpper();
      diagnostic.message = match.captured(6).trimmed();
    } else if ((match = gcc.match(line)).hasMatch()) {
      diagnostic.file = QDir::fromNativeSeparators(match.captured(1));
      diagnostic.line = match.captured(2).toInt();
      diagnostic.column = match.captured(3).toInt();
      diagnostic.severity = match.captured(4).toLower();
      diagnostic.message = match.captured(5).trimmed();
    } else if ((match = linker.match(line)).hasMatch()) {
      diagnostic.file = QDir::fromNativeSeparators(match.captured(1));
      diagnostic.severity = match.captured(2).toLower();
      diagnostic.code = match.captured(3).toUpper();
      diagnostic.message = match.captured(4).trimmed();
    } else if ((match = cmake.match(line)).hasMatch()) {
      diagnostic.severity = match.captured(1).toLower();
      diagnostic.file = QDir::fromNativeSeparators(match.captured(2));
      diagnostic.line = match.captured(3).toInt();
      diagnostic.code = "CMake";
      diagnostic.message = match.captured(4).trimmed();
    } else if (line.startsWith("ninja: error:", Qt::CaseInsensitive)) {
      diagnostic.severity = "error";
      diagnostic.code = "Ninja";
      diagnostic.message = line.mid(13).trimmed();
    } else continue;
    result << diagnostic;
  }
  return result;
}

static QString diagnosticHint(const BuildDiagnostic &d) {
  const QString code = d.code.toUpper();
  const QString text = d.message.toLower();
  if (code == "C2065" || text.contains("undeclared identifier"))
    return "Check the name for a typo, then declare it before use or include the header that declares it.";
  if (code == "C2039" || text.contains("is not a member"))
    return "The selected type does not contain that member. Check the object's type and the API/member name.";
  if (code == "C2664" || text.contains("cannot convert argument"))
    return "The function call uses an incompatible argument. Compare each argument with the function signature.";
  if (code == "C2143" || code == "C2059" || text.contains("syntax error"))
    return "Inspect this line and the line immediately above it for a missing semicolon, bracket, parenthesis, or comma.";
  if (code == "C1083" || text.contains("no such file") || text.contains("cannot open include"))
    return "The compiler cannot find a required file. Check the include name, dependency installation, and include paths.";
  if (code == "C1128" || text.contains("number of sections exceeded"))
    return "This source creates more COFF sections than MSVC's old object-file limit. Wavy-Flo enables /bigobj in CMake; configure the project again, then rebuild.";
  if (code == "LNK2019" || code == "LNK2001" || text.contains("unresolved external"))
    return "A declaration has no linked implementation. Add the source/library that defines it or correct the signature.";
  if (code == "LNK1168" || text.contains("cannot open") && text.contains(".exe"))
    return "The executable is still running or locked. Close Wavy-Flo, then build again.";
  if (d.severity.contains("warning"))
    return "The build may continue, but review this warning because it can indicate unsafe or unintended behavior.";
  if (code == "CMAKE")
    return "Check the referenced CMake file and configuration message, then configure the project again.";
  if (code == "NINJA")
    return "Ninja could not complete the requested build step. The earlier compiler error usually contains the root cause.";
  return "Read the compiler message with the referenced source line. Fix the first error, rebuild, then review any remaining errors.";
}

static bool installBuild(const QString &root, QString &error) {
  const QString source = root + "/build-msvc1444/Wavy-Flo.exe";
  const QString target = root + "/Wavy-Flo.exe";
  const QString sourcePdb = root + "/build-msvc1444/Wavy-Flo.pdb";
  const QString targetPdb = root + "/Wavy-Flo.pdb";
  const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QString staged = root + "/Wavy-Flo.install-" + token;
  const QString stagedPdb = root + "/Wavy-Flo.pdb.install-" + token;
  if (!QFile::exists(source) || !QFile::exists(sourcePdb) ||
      !QFile::copy(source, staged) || !QFile::copy(sourcePdb, stagedPdb)) {
    QFile::remove(staged);
    QFile::remove(stagedPdb);
    error = "Cannot stage the compiled executable and debug symbols. Check disk space, folder permissions, and Wavy-Flo.pdb.";
    return false;
  }
  const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz");
  const QString backup = root + "/Wavy-Flo.before-builder-" + stamp;
  const QString backupPdb = root + "/Wavy-Flo.pdb.before-builder-" + stamp;
  bool hadPrevious = QFile::exists(target);
  bool hadPreviousPdb = QFile::exists(targetPdb);
  if (hadPrevious && !QFile::rename(target, backup)) {
    QFile::remove(staged);
    QFile::remove(stagedPdb);
    error = "Cannot replace Wavy-Flo.exe. Close Wavy-Flo and try again. The previous executable is unchanged.";
    return false;
  }
  if (hadPreviousPdb && !QFile::rename(targetPdb, backupPdb)) {
    if (hadPrevious) QFile::rename(backup, target);
    QFile::remove(staged);
    QFile::remove(stagedPdb);
    error = "Cannot replace Wavy-Flo.pdb. The previous executable and symbols are unchanged.";
    return false;
  }
  if (!QFile::rename(staged, target)) {
    bool restored = !hadPrevious || QFile::rename(backup, target);
    if (hadPreviousPdb) QFile::rename(backupPdb, targetPdb);
    QFile::remove(staged);
    QFile::remove(stagedPdb);
    error = restored ? "Installation failed. The previous executable was restored."
                     : "Installation failed. Your previous executable is saved at: " + backup;
    return false;
  }
  if (!QFile::rename(stagedPdb, targetPdb)) {
    QFile::remove(target);
    if (hadPrevious) QFile::rename(backup, target);
    if (hadPreviousPdb) QFile::rename(backupPdb, targetPdb);
    QFile::remove(stagedPdb);
    error = "Debug-symbol installation failed. The previous executable and symbols were restored.";
    return false;
  }
  return true;
}

static bool isProjectRoot(const QString &directory) {
  return QFile::exists(directory + "/build-wavy-flo.cmd") &&
         QFile::exists(directory + "/source/CMakeLists.txt");
}
// The project folder, found from wherever the user pointed at or the builder
// happens to sit: the folder itself, a Wavy-Flo folder inside it, or one of the
// folders above it. Pointing at source/ or a build folder, or running a copy of
// the builder from beside the project, is an easy slip and should not be an error.
// Empty when there is no project to be found.
static QString findProjectRoot(const QString &start) {
  QDir directory(QDir(start).absolutePath());
  for (int level = 0; level < 5; ++level) {
    const QString here = directory.absolutePath();
    if (isProjectRoot(here)) return here;
    if (isProjectRoot(here + "/Wavy-Flo")) return here + "/Wavy-Flo";
    if (!directory.cdUp()) break;
  }
  return {};
}

class Builder : public QWidget {
  QProcess process;
  QProcess runProcess;
  QTimer crashWatch;
  QPlainTextEdit *log;
  QTabWidget *resultTabs;
  QTreeWidget *problems;
  QTextBrowser *problemDetails;
  QLabel *status;
  QPushButton *build, *launch, *choose;
  QLineEdit *folder;
  QProgressBar *progress;
  QString activeRoot;
  QString lastCrashSignature;
  QString pendingCrashSignature;
  QElapsedTimer crashStable;
  bool running = false, verify;
  enum DiagnosticRole { FileRole = Qt::UserRole + 1, LineRole, ColumnRole,
                        MessageRole, HintRole, FullReportRole };
  void append(const QString &message) {
    log->moveCursor(QTextCursor::End);
    log->insertPlainText(message);
    log->ensureCursorVisible();
  }
  void showDiagnostic(QTreeWidgetItem *item) {
    if (!item) {
      problemDetails->setHtml("<span style='color:#8e97a4'>Select a problem to see details.</span>");
      return;
    }
    const QString file = item->data(0, FileRole).toString();
    const int line = item->data(0, LineRole).toInt();
    const int column = item->data(0, ColumnRole).toInt();
    const QString where = file.isEmpty() ? "Build system"
        : QDir::toNativeSeparators(file) + (line ? QString(":%1%2").arg(line).arg(column ? QString(":%1").arg(column) : "") : "");
    problemDetails->setHtml(
        "<b style='color:#dfe7f2'>Where</b><br>" + where.toHtmlEscaped() +
        "<br><br><b style='color:#dfe7f2'>Why</b><br>" +
        item->data(0, MessageRole).toString().toHtmlEscaped() +
        "<br><br><b style='color:#8fd3ff'>How to fix</b><br>" +
        item->data(0, HintRole).toString().toHtmlEscaped() +
        (item->data(0, FullReportRole).toString().isEmpty() ? QString() :
         "<br><br><b style='color:#dfe7f2'>Crash report</b><pre style='color:#aeb7c4'>" +
         item->data(0, FullReportRole).toString().toHtmlEscaped() + "</pre>"));
  }
  int analyzeDiagnostics() {
    problems->clear();
    const auto diagnostics = parseDiagnostics(log->toPlainText());
    int errors = 0, warnings = 0;
    for (const auto &d : diagnostics) {
      const bool warning = d.severity.contains("warning");
      warning ? ++warnings : ++errors;
      const QString location = d.file.isEmpty() ? "Build"
          : QFileInfo(d.file).fileName() + (d.line ? QString(":%1").arg(d.line) : "");
      auto *item = new QTreeWidgetItem(
          {warning ? "Warning" : "Error", d.code, location, d.message});
      item->setData(0, FileRole, d.file);
      item->setData(0, LineRole, d.line);
      item->setData(0, ColumnRole, d.column);
      item->setData(0, MessageRole, d.message);
      item->setData(0, HintRole, diagnosticHint(d));
      item->setToolTip(2, QDir::toNativeSeparators(d.file));
      const QColor color = warning ? QColor("#e7b35f") : QColor("#ff7785");
      for (int column = 0; column < 4; ++column) item->setForeground(column, color);
      problems->addTopLevelItem(item);
    }
    problems->resizeColumnToContents(0);
    problems->resizeColumnToContents(1);
    problems->resizeColumnToContents(2);
    if (problems->topLevelItemCount()) {
      problems->setCurrentItem(problems->topLevelItem(0));
      showDiagnostic(problems->topLevelItem(0));
      resultTabs->setTabText(1, QString("Problems (%1)").arg(errors + warnings));
    } else {
      problemDetails->setHtml("<b>No structured compiler diagnostics were detected.</b><br>See Output for the complete build log.");
      resultTabs->setTabText(1, "Problems (0)");
    }
    return errors;
  }
  void showBuilderProblem(const QString &code, const QString &message,
                          const QString &hint) {
    problems->clear();
    auto *item = new QTreeWidgetItem({"Error", code, "Builder", message});
    item->setData(0, MessageRole, message);
    item->setData(0, HintRole, hint);
    for (int column = 0; column < 4; ++column)
      item->setForeground(column, QColor("#ff7785"));
    problems->addTopLevelItem(item);
    problems->setCurrentItem(item);
    showDiagnostic(item);
    resultTabs->setTabText(1, "Problems (1)");
    resultTabs->setCurrentIndex(1);
  }
  QString resolveCrashSource(const QString &leaf) const {
    if (leaf.isEmpty()) return {};
    QDirIterator files(activeRoot + "/source", {leaf}, QDir::Files,
                       QDirIterator::Subdirectories);
    return files.hasNext() ? files.next() : leaf;
  }
  bool importCrashReport() {
    const QString root = activeRoot.isEmpty() ? QDir(folder->text()).absolutePath()
                                               : activeRoot;
    const QString path = root + "/crash-report.txt";
    QFileInfo info(path);
    if (!info.exists()) return false;
    const QString signature = QString("%1:%2:%3")
        .arg(path).arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch());
    if (signature == lastCrashSignature) return false;
    if (signature != pendingCrashSignature) {
      pendingCrashSignature = signature;
      crashStable.restart();
      return false;
    }
    if (!crashStable.isValid() || crashStable.elapsed() < 400) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QString report = QString::fromLocal8Bit(file.readAll());
    if (report.trimmed().isEmpty()) return false;
    lastCrashSignature = signature;
    pendingCrashSignature.clear();

    auto captured = [&](const QString &pattern) {
      return QRegularExpression(pattern, QRegularExpression::MultilineOption)
          .match(report).captured(1).trimmed();
    };
    const QString cause = captured(R"(^Cause:\s*(.+)$)");
    const QString detail = captured(R"(^Detail:\s*(.+)$)");
    const QString memory = captured(R"(^(Tried to .+)$)");
    QString fileName;
    int line = 0;
    const QRegularExpression location(R"(\(([^()\r\n:]+):(\d+)\))");
    auto locations = location.globalMatch(report);
    if (locations.hasNext()) {
      const auto match = locations.next();
      fileName = match.captured(1);
      line = match.captured(2).toInt();
    }
    const QString source = resolveCrashSource(fileName);
    QString message = cause.isEmpty() ? "Wavy-Flo stopped after an unhandled crash." : cause;
    if (!detail.isEmpty()) message += " — " + detail;
    if (!memory.isEmpty()) message += " — " + memory;
    QString hint =
        "Open the first Wavy-Flo source frame and inspect object lifetime, pointer validity, and the values used immediately before this call.";
    if (memory.contains("0x0", Qt::CaseInsensitive) ||
        cause.contains("memory it does not own", Qt::CaseInsensitive))
      hint = "This is an invalid memory access. Check for a null, dangling, or concurrently modified object at the first Wavy-Flo source frame.";
    else if (cause.contains("stack", Qt::CaseInsensitive))
      hint = "Check for recursion without a terminating condition or an unusually large local allocation.";
    else if (cause.contains("C++", Qt::CaseInsensitive))
      hint = "Inspect the reported exception message and add handling at the boundary where this operation can fail.";

    problems->clear();
    const QString code = detail.contains("0x")
        ? QRegularExpression(R"((0x[0-9a-fA-F]+))").match(detail).captured(1)
        : "CRASH";
    const QString place = fileName.isEmpty() ? "Runtime"
        : fileName + (line ? QString(":%1").arg(line) : "");
    auto *item = new QTreeWidgetItem({"Crash", code, place, message});
    item->setData(0, FileRole, source);
    item->setData(0, LineRole, line);
    item->setData(0, MessageRole, message);
    item->setData(0, HintRole, hint);
    item->setData(0, FullReportRole, report);
    for (int column = 0; column < 4; ++column)
      item->setForeground(column, QColor("#ff5f76"));
    problems->addTopLevelItem(item);
    problems->setCurrentItem(item);
    problems->resizeColumnToContents(0);
    problems->resizeColumnToContents(1);
    problems->resizeColumnToContents(2);
    resultTabs->setTabText(1, "Problems (1 crash)");
    resultTabs->setCurrentIndex(1);
    status->setText("Wavy-Flo crashed. The crash report was loaded into Problems.");
    raise();
    activateWindow();
    if (qApp->arguments().contains("--crash-monitor-self-test")) {
      QFile resultFile(QCoreApplication::applicationDirPath() +
                       "/builder-crash-monitor-test.txt");
      if (resultFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        resultFile.write(("PASS\n" + message + "\n" + place).toUtf8());
      QTimer::singleShot(0, qApp, [] { qApp->exit(0); });
    }
    return true;
  }
  bool launchInstalled(const QStringList &arguments = {}) {
    if (runProcess.state() != QProcess::NotRunning) {
      raise();
      activateWindow();
      return true;
    }
    runProcess.setWorkingDirectory(activeRoot);
    runProcess.setProgram(activeRoot + "/Wavy-Flo.exe");
    runProcess.setArguments(arguments);
    runProcess.start();
    if (runProcess.waitForStarted(3000)) {
      launch->setEnabled(false);
      launch->setText("Debug session running…");
      build->setEnabled(false);
      status->setText("Debug session running. Builder is monitoring Wavy-Flo for crashes.");
      return true;
    }
    QMessageBox::warning(this, "Cannot open Wavy-Flo",
                         "The build succeeded, but Wavy-Flo could not be opened. Check its runtime DLLs.");
    return false;
  }
  void openDiagnostic(QTreeWidgetItem *item) {
    if (!item) return;
    const QString file = item->data(0, FileRole).toString();
    if (file.isEmpty() || !QFile::exists(file)) return;
    const int line = item->data(0, LineRole).toInt();
    const int column = item->data(0, ColumnRole).toInt();
    const QString code = QStandardPaths::findExecutable("code.cmd").isEmpty()
        ? QStandardPaths::findExecutable("code.exe")
        : QStandardPaths::findExecutable("code.cmd");
    if (!code.isEmpty() && QProcess::startDetached(
            code, {"--goto", QString("%1:%2:%3").arg(file).arg(std::max(1, line)).arg(std::max(1, column))}))
      return;
#ifdef Q_OS_WIN
    const QString visualStudio =
        "C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/devenv.exe";
    if (QFile::exists(visualStudio) && QProcess::startDetached(
            visualStudio, {file, "/Command", QString("Edit.Goto %1").arg(std::max(1, line))}))
      return;
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(file));
  }
  void finish(bool success, const QString &message) {
    if (!running) return;
    running = false;
    append("\n" + message + "\n");
    status->setText(message);
    progress->setRange(0, 1);
    progress->setValue(success ? 1 : 0);
    build->setEnabled(true);
    choose->setEnabled(true);
    folder->setEnabled(true);
    launch->setEnabled(QFile::exists(activeRoot + "/Wavy-Flo.exe"));
    QFile output(activeRoot + "/builder-last-build.log");
    if (output.open(QIODevice::WriteOnly | QIODevice::Truncate)) output.write(log->toPlainText().toUtf8());
    if (verify) QTimer::singleShot(0, qApp, [success] { qApp->exit(success ? 0 : 1); });
  }
public:
  explicit Builder(bool verification = false) : verify(verification) {
    setWindowTitle("Wavy-Flo Builder");
    resize(980, 700);
    setMinimumSize(700, 500);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(14);
    auto *heading = new QLabel("Wavy-Flo Builder");
    heading->setStyleSheet("font-size:24px; font-weight:600;");
    layout->addWidget(heading);
    layout->addWidget(new QLabel("Compile your project and install the latest Wavy-Flo executable."));
    layout->addWidget(new QLabel("Project folder"));
    auto *pathRow = new QHBoxLayout;
    folder = new QLineEdit;
    QString root = findProjectRoot(QCoreApplication::applicationDirPath());
    if (root.isEmpty()) root = QCoreApplication::applicationDirPath();
    folder->setText(QDir::toNativeSeparators(root));
    choose = new QPushButton("Browse…");
    pathRow->addWidget(folder, 1);
    pathRow->addWidget(choose);
    layout->addLayout(pathRow);
    connect(choose, &QPushButton::clicked, this, [this] {
      QString selected = QFileDialog::getExistingDirectory(this, "Choose Wavy-Flo project", folder->text());
      if (selected.isEmpty()) return;
      const QString found = findProjectRoot(selected);
      folder->setText(QDir::toNativeSeparators(found.isEmpty() ? selected : found));
    });
    auto *actions = new QHBoxLayout;
    build = new QPushButton("Build and Run");
    build->setObjectName("primary");
    launch = new QPushButton("Open Wavy-Flo");
    auto *openFolder = new QPushButton("Open Folder");
    actions->addWidget(build);
    actions->addWidget(launch);
    actions->addWidget(openFolder);
    actions->addStretch();
    layout->addLayout(actions);
    status = new QLabel("Ready to build. Visual Studio and the bundled Qt SDK are required.");
    status->setWordWrap(true);
    layout->addWidget(status);
    progress = new QProgressBar;
    progress->setRange(0, 1);
    progress->setValue(0);
    progress->setTextVisible(false);
    progress->setFixedHeight(4);
    layout->addWidget(progress);
    log = new QPlainTextEdit;
    log->setReadOnly(true);
    log->setMaximumBlockCount(15000);
    log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log->setPlaceholderText("Build output will appear here.");
    resultTabs = new QTabWidget;
    resultTabs->setObjectName("resultTabs");
    resultTabs->addTab(log, "Output");
    auto *problemPage = new QWidget;
    auto *problemLayout = new QVBoxLayout(problemPage);
    problemLayout->setContentsMargins(0, 0, 0, 0);
    problemLayout->setSpacing(7);
    auto *problemHint = new QLabel(
        "Select a problem for Where / Why / How to fix. Double-click it to open the source line.");
    problemHint->setStyleSheet("color:#8e97a4;padding:4px 7px 0 7px;");
    problemLayout->addWidget(problemHint);
    problems = new QTreeWidget;
    problems->setObjectName("problems");
    problems->setHeaderLabels({"Severity", "Code", "File", "Description"});
    problems->setRootIsDecorated(false);
    problems->setAlternatingRowColors(true);
    problems->setSelectionBehavior(QAbstractItemView::SelectRows);
    problems->header()->setStretchLastSection(true);
    problemDetails = new QTextBrowser;
    problemDetails->setObjectName("problemDetails");
    problemDetails->setOpenExternalLinks(false);
    problemDetails->setMaximumHeight(145);
    problemDetails->setHtml("<span style='color:#8e97a4'>Build problems and repair suggestions will appear here.</span>");
    auto *problemSplit = new QSplitter(Qt::Vertical);
    problemSplit->addWidget(problems);
    problemSplit->addWidget(problemDetails);
    problemSplit->setStretchFactor(0, 1);
    problemLayout->addWidget(problemSplit);
    resultTabs->addTab(problemPage, "Problems (0)");
    layout->addWidget(resultTabs, 1);
    auto *hint = new QLabel("Your previous executable is kept as a backup. Build logs are saved in the project folder.");
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#a0a7b2;");
    layout->addWidget(hint);
    setStyleSheet("QWidget{background:#202124;color:#e4e7ec;font-size:12px;}"
      "QLineEdit,QPlainTextEdit{background:#17181b;border:1px solid #383c44;border-radius:5px;padding:8px;}"
      "QTreeWidget,QTextBrowser{background:#17181b;border:1px solid #383c44;border-radius:5px;}"
      "QHeaderView::section{background:#292d34;color:#aeb7c4;border:0;border-right:1px solid #3b414b;padding:6px;}"
      "QTabWidget::pane{border:1px solid #383c44;border-radius:5px;}"
      "QTabBar::tab{background:#292d34;color:#aeb7c4;padding:7px 14px;margin-right:2px;}"
      "QTabBar::tab:selected{background:#326b9f;color:white;}"
      "QPushButton{background:#30343b;border:1px solid #454c57;border-radius:5px;padding:9px 14px;}"
      "QPushButton:hover{background:#3b4350;}QPushButton:disabled{color:#747b86;}"
      "QPushButton#primary{background:#326b9f;border-color:#588aba;}"
      "QProgressBar{border:0;background:#30343b;}QProgressBar::chunk{background:#77baff;}");
    connect(build, &QPushButton::clicked, this, &Builder::startBuild);
    connect(launch, &QPushButton::clicked, this, [this] {
      activeRoot = QDir(folder->text()).absolutePath();
      if (const QString found = findProjectRoot(activeRoot); !found.isEmpty()) activeRoot = found;
      if (!QFile::exists(activeRoot + "/Wavy-Flo.exe")) {
        QMessageBox::warning(this, "Cannot open Wavy-Flo",
                             "Build the project first, and check that its runtime DLLs are present.");
        return;
      }
      launchInstalled();
    });
    connect(openFolder, &QPushButton::clicked, this, [this] {
      QDesktopServices::openUrl(QUrl::fromLocalFile(folder->text()));
    });
    connect(problems, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *current) { showDiagnostic(current); });
    connect(problems, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item) {
      openDiagnostic(item);
    });
    process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
      append(QString::fromLocal8Bit(process.readAllStandardOutput()));
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart) {
        const QString message = "Cannot start the compiler: " + process.errorString();
        showBuilderProblem("START", message,
            "Install Visual Studio C++ tools, then verify build-wavy-flo.cmd and the compiler paths.");
        finish(false, message);
      }
    });
    connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus state) {
      append(QString::fromLocal8Bit(process.readAllStandardOutput()));
      if (state != QProcess::NormalExit || code != 0) {
        const int errors = analyzeDiagnostics();
        resultTabs->setCurrentIndex(errors || problems->topLevelItemCount() ? 1 : 0);
        finish(false, errors
            ? QString("Build failed with %1 error%2. Select a problem to see where, why, and how to fix it.")
                  .arg(errors).arg(errors == 1 ? "" : "s")
            : "Build failed. See Output for the complete compiler message. The installed executable was not replaced.");
        return;
      }
      QString error;
      if (!installBuild(activeRoot, error)) {
        showBuilderProblem("INSTALL", error,
            "Close Wavy-Flo if it is running, check folder permissions and disk space, then build again.");
        finish(false, error);
        return;
      }
      problems->clear();
      resultTabs->setTabText(1, "Problems (0)");
      finish(true, "Build complete. Wavy-Flo.exe was updated and is starting now.");
      if (!verify) launchInstalled();
    });
    connect(&runProcess, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus state) {
      launch->setText("Open Wavy-Flo");
      launch->setEnabled(QFile::exists(activeRoot + "/Wavy-Flo.exe"));
      if (!running) build->setEnabled(true);
      QTimer::singleShot(100, this, [this, code, state] {
        if (importCrashReport()) return;
        if (state != QProcess::NormalExit || code != 0) {
          showBuilderProblem("RUNTIME",
              QString("Wavy-Flo stopped abnormally with exit code %1.").arg(code),
              "No crash-report.txt was produced. Check Windows Event Viewer and wavy-flo-log.txt for the last recorded operation.");
          status->setText("Wavy-Flo stopped abnormally. See Problems.");
        } else status->setText("Debug session ended normally.");
      });
    });
    connect(&runProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
      if (error != QProcess::FailedToStart) return;
      launch->setText("Open Wavy-Flo");
      launch->setEnabled(true);
      if (!running) build->setEnabled(true);
      showBuilderProblem("RUNTIME", "Wavy-Flo could not start: " + runProcess.errorString(),
                         "Check that Wavy-Flo.exe and its Qt runtime DLLs are installed in the project folder.");
    });
    crashWatch.setInterval(750);
    connect(&crashWatch, &QTimer::timeout, this, [this] { importCrashReport(); });
    if (!qApp->arguments().contains("--crash-monitor-self-test")) {
      crashWatch.start();
      activeRoot = QDir(folder->text()).absolutePath();
      QTimer::singleShot(0, this, [this] { importCrashReport(); });
    }
  }
  void startCrashMonitorTest() {
    activeRoot = QCoreApplication::applicationDirPath();
    QFile::remove(activeRoot + "/crash-report.txt");
    lastCrashSignature.clear();
    pendingCrashSignature.clear();
    crashWatch.start();
    if (!launchInstalled({"--crash-on-purpose", "--self-test"})) {
      QFile resultFile(activeRoot + "/builder-crash-monitor-test.txt");
      if (resultFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        resultFile.write("FAIL: Wavy-Flo did not start");
      qApp->exit(2);
      return;
    }
    QTimer::singleShot(10000, this, [this] {
      if (!lastCrashSignature.isEmpty()) return;
      QFile resultFile(activeRoot + "/builder-crash-monitor-test.txt");
      if (resultFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        resultFile.write("FAIL: crash report was not imported");
      qApp->exit(3);
    });
  }
  void startBuild() {
    if (running) return;
    activeRoot = QDir(folder->text()).absolutePath();
    // Correct the folder to the real project when it was pointed near it.
    if (const QString found = findProjectRoot(activeRoot); !found.isEmpty() && found != activeRoot) {
      activeRoot = found;
      folder->setText(QDir::toNativeSeparators(found));
    }
    log->clear();
    problems->clear();
    problemDetails->setHtml("<span style='color:#8e97a4'>Build problems and repair suggestions will appear here.</span>");
    resultTabs->setTabText(1, "Problems (0)");
    resultTabs->setCurrentIndex(0);
    running = true;
    if (!QFile::exists(activeRoot + "/build-wavy-flo.cmd") || !QFile::exists(activeRoot + "/source/CMakeLists.txt")) {
      QStringList missing;
      if (!QFile::exists(activeRoot + "/build-wavy-flo.cmd")) missing << "build-wavy-flo.cmd";
      if (!QFile::exists(activeRoot + "/source/CMakeLists.txt")) missing << "source/CMakeLists.txt";
      const QString message =
          "Choose the project folder containing build-wavy-flo.cmd and source/CMakeLists.txt. "
          "Looked in " + QDir::toNativeSeparators(activeRoot) + " and did not find: " + missing.join(", ") + ".";
      showBuilderProblem("PROJECT", message,
          "Press Browse and select the Wavy-Flo root folder, not its source or build subfolder.");
      finish(false, message);
      return;
    }
    build->setEnabled(false);
    launch->setEnabled(false);
    choose->setEnabled(false);
    folder->setEnabled(false);
    progress->setRange(0, 0);
    status->setText("Building… You can follow the compiler output below.");
    append("Project: " + activeRoot + "\n\n");
    process.setWorkingDirectory(activeRoot);
    process.setProgram(qEnvironmentVariable("COMSPEC", "C:/Windows/System32/cmd.exe"));
#ifdef Q_OS_WIN
    process.setNativeArguments("/d /s /c \"call build-wavy-flo.cmd\"");
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
      args->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#else
    process.setArguments({"/c", "build-wavy-flo.cmd"});
#endif
    process.start();
  }
protected:
  void closeEvent(QCloseEvent *event) override {
    if (running) {
      status->setText("Build is still running. Please wait for it to finish before closing.");
      event->ignore();
    } else if (runProcess.state() != QProcess::NotRunning) {
      status->setText("Debug session is still running. Close Wavy-Flo before closing Builder.");
      event->ignore();
    } else event->accept();
  }
};

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setStyle("Fusion");
  if (app.arguments().contains("--self-test")) {
    const QString sample =
        "D:\\Project\\source\\main.cpp(42,7): error C2065: 'thing': undeclared identifier\n"
        "D:\\Project\\source\\main.cpp(51): warning C4244: conversion may lose data\n"
        "D:\\Project\\source\\main.cpp : fatal error C1128: number of sections exceeded object file format limit: compile with /bigobj\n"
        "Wavy-Flo.exe : fatal error LNK1168: cannot open Wavy-Flo.exe for writing\n";
    const auto diagnostics = parseDiagnostics(sample);
    if (diagnostics.size() != 4) return 10;
    if (diagnostics[0].line != 42 || diagnostics[0].column != 7 ||
        diagnostics[0].code != "C2065" || diagnostics[0].severity != "error") return 11;
    if (!diagnosticHint(diagnostics[0]).contains("declare", Qt::CaseInsensitive)) return 12;
    if (!diagnosticHint(diagnostics[2]).contains("/bigobj", Qt::CaseInsensitive)) return 13;
    if (!diagnosticHint(diagnostics[3]).contains("Close Wavy-Flo", Qt::CaseInsensitive)) return 14;
    QTemporaryDir dir;
    QDir(dir.path()).mkpath("build-msvc1444");
    QString error;
    if (installBuild(dir.path(), error)) return 1;
    QFile compiled(dir.path() + "/build-msvc1444/Wavy-Flo.exe");
    compiled.open(QIODevice::WriteOnly); compiled.write("new executable"); compiled.close();
    QFile symbols(dir.path() + "/build-msvc1444/Wavy-Flo.pdb");
    symbols.open(QIODevice::WriteOnly); symbols.write("new symbols"); symbols.close();
    QFile installed(dir.path() + "/Wavy-Flo.exe");
    installed.open(QIODevice::WriteOnly); installed.write("previous executable"); installed.close();
    QFile installedSymbols(dir.path() + "/Wavy-Flo.pdb");
    installedSymbols.open(QIODevice::WriteOnly); installedSymbols.write("previous symbols"); installedSymbols.close();
    if (!installBuild(dir.path(), error)) return 2;
    installed.open(QIODevice::ReadOnly);
    if (installed.readAll() != "new executable") return 3;
    installedSymbols.open(QIODevice::ReadOnly);
    if (installedSymbols.readAll() != "new symbols") return 6;
    auto backups = QDir(dir.path()).entryList({"Wavy-Flo.before-builder-*"}, QDir::Files);
    if (backups.size() != 1) return 4;
    QFile backup(dir.path() + "/" + backups.first()); backup.open(QIODevice::ReadOnly);
    if (backup.readAll() != "previous executable") return 5;
    auto symbolBackups = QDir(dir.path()).entryList({"Wavy-Flo.pdb.before-builder-*"}, QDir::Files);
    if (symbolBackups.size() != 1) return 7;
    QFile symbolBackup(dir.path() + "/" + symbolBackups.first());
    symbolBackup.open(QIODevice::ReadOnly);
    return symbolBackup.readAll() == "previous symbols" ? 0 : 8;
  }
  bool verify = app.arguments().contains("--verify-build");
  Builder builder(verify);
  if (app.arguments().contains("--crash-monitor-self-test")) {
    builder.show();
    QTimer::singleShot(0, &builder, &Builder::startCrashMonitorTest);
  } else if (app.arguments().contains("--screenshot")) {
    builder.show();
    QTimer::singleShot(300, &builder, [&] {
      builder.grab().save(QCoreApplication::applicationDirPath() + "/builder-preview.png");
      app.quit();
    });
  } else if (verify) QTimer::singleShot(0, &builder, &Builder::startBuild);
  else builder.show();
  return app.exec();
}
