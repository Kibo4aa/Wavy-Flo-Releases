#include "report_dialog.h"
#include "image_files.h"
#include "updater.h"
#include <QBuffer>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHttpMultiPart>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QSysInfo>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>

// The channel's webhook, set when Wavy-Flo is built (CMakeLists.txt reads it
// from report-webhook.txt beside the sources, a file kept out of the
// repository): a webhook can only post, and only to that one channel.
#ifndef WAVY_REPORT_WEBHOOK
#define WAVY_REPORT_WEBHOOK ""
#endif

namespace {
constexpr qint64 kMaxPictureBytes = 3 * 1024 * 1024;   // each, after it is made smaller
constexpr qint64 kMaxReportBytes = 9 * 1024 * 1024;    // Discord takes 10 MB a message
constexpr int kMaxPictures = 4;   // pictures and videos together

// Videos are sent as they are; their type by their ending.
QString videoType(const QString &path) {
  const QString suffix = QFileInfo(path).suffix().toLower();
  if (suffix == "mp4" || suffix == "m4v") return "video/mp4";
  if (suffix == "mov") return "video/quicktime";
  if (suffix == "webm") return "video/webm";
  if (suffix == "mkv") return "video/x-matroska";
  return {};
}
constexpr int kMinSecondsBetween = 60;

QString webhook() { return QString::fromUtf8(WAVY_REPORT_WEBHOOK).trimmed(); }

// Who the person is, taken out of what is sent: their Windows user name is in
// every path in the log and the crash report.
QString withoutUserName(QString text) {
  const QString home = QDir::toNativeSeparators(QDir::homePath());
  const QString user = QDir(QDir::homePath()).dirName();
  text.replace(home, "%USERPROFILE%", Qt::CaseInsensitive);
  text.replace(QDir::homePath(), "%USERPROFILE%", Qt::CaseInsensitive);
  if (user.size() >= 3) text.replace(user, "<user>", Qt::CaseInsensitive);
  return text;
}

// The last `lines` lines of a text file beside the program, or nothing.
QString besideProgram(const QString &name, int lines) {
  QFile file(QCoreApplication::applicationDirPath() + "/" + name);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
  QStringList all = QString::fromUtf8(file.readAll()).split('\n');
  if (all.size() > lines) all = all.mid(all.size() - lines);
  return withoutUserName(all.join('\n'));
}

// The newest crash report beside the program: crash-report.txt is renamed
// with its time when the next run starts, so look at every one of them.
QString newestCrashReport() {
  const QFileInfoList reports =
      QDir(QCoreApplication::applicationDirPath()).entryInfoList({"crash-report*.txt"}, QDir::Files, QDir::Time);
  if (reports.isEmpty()) return {};
  QFile file(reports.first().absoluteFilePath());
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
  return withoutUserName(QString::fromUtf8(file.readAll()).left(60000));
}

// A picture made small enough to send: no side over 1920, PNG while that is
// small enough, JPEG when it is not.
QByteArray pictureBytes(QImage image, QString *format) {
  if (image.width() > 1920 || image.height() > 1920) image = image.scaled(1920, 1920, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  *format = "png";
  if (bytes.size() > kMaxPictureBytes) {
    bytes.clear();
    buffer.seek(0);
    image.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPG", 85);
    *format = "jpg";
  }
  return bytes;
}

struct Attachment {
  QString name;
  QByteArray bytes;
  QString type;
};
}  // namespace

bool reportsConfigured() { return webhook().startsWith("https://"); }

void showReportDialog(QWidget *parent, const QImage &preview, const QString &graphics) {
  QDialog dialog(parent);
  dialog.setWindowTitle("Report a Problem");
  dialog.resize(560, 560);
  auto *layout = new QVBoxLayout(&dialog);

  auto *intro = new QLabel("Tell us what went wrong. It goes to the Wavy-Flo team's report channel, "
                           "where it is read and fixed.");
  intro->setWordWrap(true);
  layout->addWidget(intro);

  layout->addWidget(new QLabel("What went wrong?"));
  auto *issue = new QPlainTextEdit;
  issue->setPlaceholderText("What happened, and what you were doing when it did. "
                            "The more exactly you say it, the quicker it is found.");
  layout->addWidget(issue, 1);

  auto *mediaLabel = new QLabel(QString("Pictures and videos (optional), up to %1 MB in all. For a bigger video, "
                                        "put a Google Drive or YouTube link to it in the words above.")
                                    .arg(kMaxReportBytes / (1024 * 1024)));
  mediaLabel->setWordWrap(true);
  layout->addWidget(mediaLabel);
  auto *pictures = new QListWidget;
  pictures->setMaximumHeight(84);
  layout->addWidget(pictures);
  auto *pictureRow = new QHBoxLayout;
  auto *addPicture = new QPushButton("Add picture or video…");
  auto *removePicture = new QPushButton("Remove");
  pictureRow->addWidget(addPicture);
  pictureRow->addWidget(removePicture);
  pictureRow->addStretch(1);
  layout->addLayout(pictureRow);

  auto *withPreview = new QCheckBox("Add the picture in the viewer now");
  withPreview->setEnabled(!preview.isNull());
  layout->addWidget(withPreview);
  const QString crash = newestCrashReport();
  const QString log = besideProgram("wavy-flo-log.txt", 400);
  auto *withLogs = new QCheckBox("Add the crash report and log (your Windows user name is left out)");
  withLogs->setChecked(!crash.isEmpty() || !log.isEmpty());
  withLogs->setEnabled(!crash.isEmpty() || !log.isEmpty());
  layout->addWidget(withLogs);

  layout->addWidget(new QLabel("Your Discord name or e-mail, if you would like an answer (optional)"));
  auto *contact = new QLineEdit;
  layout->addWidget(contact);

  auto *status = new QLabel;
  status->setWordWrap(true);
  layout->addWidget(status);

  auto *buttons = new QDialogButtonBox;
  auto *showSent = buttons->addButton("Show what is sent", QDialogButtonBox::HelpRole);
  auto *send = buttons->addButton("Send", QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Cancel);
  layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  QStringList picturePaths;
  QObject::connect(addPicture, &QPushButton::clicked, &dialog, [&] {
    const QStringList chosen = QFileDialog::getOpenFileNames(
        &dialog, "Add pictures or videos", {},
        "Pictures and videos (*.png *.jpg *.jpeg *.bmp *.webp *.gif *.mp4 *.m4v *.mov *.webm *.mkv)");
    for (const QString &path : chosen) {
      if (picturePaths.size() >= kMaxPictures) {
        status->setText(QString("Up to %1 pictures and videos.").arg(kMaxPictures));
        break;
      }
      if (picturePaths.contains(path)) continue;
      const bool video = !videoType(path).isEmpty();
      const double megabytes = QFileInfo(path).size() / (1024. * 1024);
      if (video && QFileInfo(path).size() > kMaxReportBytes) {
        status->setText(QString("%1 is %2 MB, too big to send. Put a Google Drive or YouTube link to it in the words instead.")
                            .arg(QFileInfo(path).fileName()).arg(megabytes, 0, 'f', 1));
        continue;
      }
      if (!video && !canReadImageFile(path)) continue;
      picturePaths << path;
      pictures->addItem(QFileInfo(path).fileName() + (video ? QString("  (video, %1 MB)").arg(megabytes, 0, 'f', 1) : QString()));
    }
  });
  QObject::connect(removePicture, &QPushButton::clicked, &dialog, [&] {
    const int row = pictures->currentRow();
    if (row < 0) return;
    picturePaths.removeAt(row);
    delete pictures->takeItem(row);
  });

  const QString system = QSysInfo::prettyProductName() + " (" + QSysInfo::currentCpuArchitecture() + ")";
  const QString reportId = QString::number(QRandomGenerator::global()->generate(), 16).rightJustified(8, '0');
  // The words sent, as the channel will show them.
  auto summary = [&] {
    QString text;
    QTextStream out(&text);
    out << "Version: " << kWavyFloVersion << "\nSystem: " << system << "\nGraphics: " << (graphics.isEmpty() ? "-" : graphics)
        << "\nReport: " << reportId << "\n";
    if (!contact->text().trimmed().isEmpty()) out << "Contact: " << contact->text().trimmed() << "\n";
    out << "\n" << issue->toPlainText().trimmed() << "\n";
    return text;
  };
  QObject::connect(showSent, &QPushButton::clicked, &dialog, [&] {
    QString text = summary();
    text += QString("\nPictures and videos: %1%2\n").arg(picturePaths.size()).arg(withPreview->isChecked() ? " + the viewer" : "");
    if (withLogs->isChecked()) {
      if (!crash.isEmpty()) text += "\n--- crash report ---\n" + crash + "\n";
      if (!log.isEmpty()) text += "\n--- log (last 400 lines) ---\n" + log + "\n";
    }
    QDialog shown(&dialog);
    shown.setWindowTitle("What is sent");
    shown.resize(640, 520);
    auto *box = new QVBoxLayout(&shown);
    auto *view = new QPlainTextEdit(text);
    view->setReadOnly(true);
    box->addWidget(view);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(close, &QDialogButtonBox::rejected, &shown, &QDialog::reject);
    box->addWidget(close);
    shown.exec();
  });

  QNetworkAccessManager network;
  QObject::connect(send, &QPushButton::clicked, &dialog, [&] {
    const QString words = issue->toPlainText().trimmed();
    if (words.size() < 10) {
      status->setText("Please say a little more about what went wrong.");
      return;
    }
    if (!reportsConfigured()) {
      status->setText("This copy of Wavy-Flo was built without a report channel to send to.");
      return;
    }
    QSettings settings("Wavy-Flo", "Wavy-Flo");
    const qint64 last = settings.value("reports/lastSent").toLongLong();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (now - last < kMinSecondsBetween) {
      status->setText(QString("A report was just sent. Please wait %1 seconds.").arg(kMinSecondsBetween - (now - last)));
      return;
    }

    std::vector<Attachment> files;
    qint64 total = 0;
    auto add = [&](const QString &name, const QByteArray &bytes, const QString &type) {
      if (bytes.isEmpty()) return;
      total += bytes.size();
      files.push_back({name, bytes, type});
    };
    int n = 0;
    auto addPictureImage = [&](const QImage &image, const QString &base) {
      QString format;
      const QByteArray bytes = pictureBytes(image, &format);
      add(QString("%1-%2.%3").arg(base).arg(++n).arg(format), bytes, format == "png" ? "image/png" : "image/jpeg");
    };
    if (withPreview->isChecked() && !preview.isNull()) addPictureImage(preview, "viewer");
    for (const QString &path : picturePaths) {
      const QString type = videoType(path);
      if (type.isEmpty()) {
        addPictureImage(readImageFile(path), "picture");
        continue;
      }
      QFile video(path);
      if (video.open(QIODevice::ReadOnly))
        add(QString("video-%1.%2").arg(++n).arg(QFileInfo(path).suffix().toLower()), video.readAll(), type);
    }
    if (withLogs->isChecked()) {
      add("crash-report.txt", crash.toUtf8(), "text/plain");
      add("wavy-flo-log.txt", log.toUtf8(), "text/plain");
    }
    // Said, not dropped: Discord refuses a message over its limit whole.
    if (total > kMaxReportBytes) {
      status->setText(QString("The pictures and videos come to %1 MB, more than the %2 MB a report can carry. "
                              "Remove one, or put a Google Drive or YouTube link in the words instead.")
                          .arg(total / (1024. * 1024), 0, 'f', 1).arg(kMaxReportBytes / (1024 * 1024)));
      return;
    }

    // One message: the words in an embed, the files with it.
    QJsonObject embed{{"title", words.section('\n', 0, 0).left(240)},
                      {"description", words.left(3900)},
                      {"color", 0x00D4A0},
                      {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                      {"footer", QJsonObject{{"text", "Report " + reportId}}}};
    QJsonArray fields{QJsonObject{{"name", "Version"}, {"value", QString(kWavyFloVersion)}, {"inline", true}},
                      QJsonObject{{"name", "System"}, {"value", system.left(1000)}, {"inline", true}},
                      QJsonObject{{"name", "Graphics"}, {"value", (graphics.isEmpty() ? QString("-") : graphics).left(1000)}}};
    if (!contact->text().trimmed().isEmpty())
      fields.append(QJsonObject{{"name", "Contact"}, {"value", contact->text().trimmed().left(200)}});
    embed["fields"] = fields;
    QJsonArray attachments;
    for (size_t i = 0; i < files.size(); ++i)
      attachments.append(QJsonObject{{"id", int(i)}, {"filename", files[i].name}});
    const QJsonObject payload{{"username", "Wavy-Flo report"},
                              {"embeds", QJsonArray{embed}},
                              {"attachments", attachments},
                              {"allowed_mentions", QJsonObject{{"parse", QJsonArray{}}}}};

    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart json;
    json.setHeader(QNetworkRequest::ContentDispositionHeader, "form-data; name=\"payload_json\"");
    json.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    json.setBody(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    multi->append(json);
    for (size_t i = 0; i < files.size(); ++i) {
      QHttpPart part;
      part.setHeader(QNetworkRequest::ContentDispositionHeader,
                     QString("form-data; name=\"files[%1]\"; filename=\"%2\"").arg(i).arg(files[i].name));
      part.setHeader(QNetworkRequest::ContentTypeHeader, files[i].type);
      part.setBody(files[i].bytes);
      multi->append(part);
    }
    QNetworkRequest request{QUrl(webhook() + (webhook().contains('?') ? "&" : "?") + "wait=true")};
    request.setHeader(QNetworkRequest::UserAgentHeader, QString("Wavy-Flo/") + kWavyFloVersion);
    send->setEnabled(false);
    status->setText("Sending…");
    QNetworkReply *reply = network.post(request, multi);
    multi->setParent(reply);
    QObject::connect(reply, &QNetworkReply::finished, &dialog, [&, reply] {
      reply->deleteLater();
      const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      if (reply->error() == QNetworkReply::NoError && code >= 200 && code < 300) {
        QSettings("Wavy-Flo", "Wavy-Flo").setValue("reports/lastSent", QDateTime::currentSecsSinceEpoch());
        QMessageBox::information(&dialog, "Report a Problem",
                                 QString("Thank you. Your report has been sent (report %1).").arg(reportId));
        dialog.accept();
        return;
      }
      send->setEnabled(true);
      status->setText(QString("It could not be sent (%1). Check the internet connection and try again.")
                          .arg(code ? QString("HTTP %1").arg(code) : reply->errorString()));
    });
  });

  if (!reportsConfigured())
    status->setText("This copy of Wavy-Flo was built without a report channel: reports cannot be sent from it.");
  dialog.exec();
}
