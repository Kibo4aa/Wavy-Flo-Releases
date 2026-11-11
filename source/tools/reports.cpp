// WavyFloReports: fetches the reports sent from Wavy-Flo's Help > Report a
// Problem (and anything else posted in the report channel) into a folder, one
// folder a report with its words and its files, and marks each one fetched in
// the channel with a reaction so it is not fetched twice.
//
//   WavyFloReports [folder]         fetch new reports into folder
//   WavyFloReports setup [folder]   ask for the bot's token and the channel,
//                                   check them, and write folder/config.json
// (folder: by default "reports" beside the folder this program is in)
//
// The folder holds config.json, written by hand and never put in a repository:
//   { "token": "<the bot's token>", "channel": "<the report channel's id>" }
// and state.json, kept by this program: the newest message fetched so far.
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
QTextStream out(stdout);
QTextStream err(stderr);
const QString api = "https://discord.com/api/v10";

QJsonObject readJson(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return QJsonDocument::fromJson(file.readAll()).object();
}
bool writeJson(const QString &path, const QJsonObject &object) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(QJsonDocument(object).toJson()) > 0;
}

struct Reply {
  int code = 0;
  QByteArray body;
  QString error;
};
// One request, waited for; a 429 is waited out as Discord asks and tried again.
Reply call(QNetworkAccessManager &network, const QString &verb, const QUrl &url, const QString &token) {
  for (int attempt = 0; attempt < 5; ++attempt) {
    QNetworkRequest request(url);
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bot " + token.toUtf8());
    request.setHeader(QNetworkRequest::UserAgentHeader, "DiscordBot (https://github.com/Kibo4aa/Wavy-Flo-Releases, 1) WavyFloReports");
    if (verb == "PUT") request.setHeader(QNetworkRequest::ContentLengthHeader, 0);
    QNetworkReply *reply = network.sendCustomRequest(request, verb.toUtf8(), QByteArray());
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec();
    Reply result;
    result.code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    result.error = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
    if (!reply->isFinished()) result.error = "timed out";
    reply->deleteLater();
    if (result.code == 429) {
      const double wait = QJsonDocument::fromJson(result.body).object().value("retry_after").toDouble(1);
      QThread::msleep(ulong(std::clamp(wait, .1, 30.) * 1000));
      continue;
    }
    return result;
  }
  return {429, {}, "rate limited"};
}

QString safeName(QString name) {
  name.replace(QRegularExpression(R"([\\/:*?"<>|])"), "_");
  return name.left(120);
}
// A line typed at the console; with `hidden`, not shown as it is typed.
QString ask(const QString &prompt, bool hidden) {
  out << prompt << Qt::flush;
#ifdef Q_OS_WIN
  HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  const bool console = hidden && GetConsoleMode(input, &mode);
  if (console) SetConsoleMode(input, mode & ~DWORD(ENABLE_ECHO_INPUT));
#endif
  std::string line;
  std::getline(std::cin, line);
#ifdef Q_OS_WIN
  if (console) { SetConsoleMode(input, mode); out << "\n" << Qt::flush; }
#endif
  return QString::fromStdString(line).trimmed();
}

// Asks for the token and the channel, checks each against Discord, and
// writes config.json once both work.
int setup(const QString &folder) {
  QNetworkAccessManager network;
  out << "Wavy-Flo reports: setting up\n\n"
         "1. In the Discord Developer Portal, open your application > Bot > Reset Token, and copy the token.\n";
  const QString token = ask("   Paste the token here (it is not shown) and press Enter: ", true);
  const Reply me = call(network, "GET", QUrl(api + "/users/@me"), token);
  if (me.code != 200) {
    err << "   Discord does not accept that token (HTTP " << me.code << "). Copy it again and run setup again.\n";
    return 2;
  }
  out << "   Token works: the bot is \"" << QJsonDocument::fromJson(me.body).object().value("username").toString() << "\".\n\n";
  const Reply app = call(network, "GET", QUrl(api + "/oauth2/applications/@me"), token);
  const QString appId = QJsonDocument::fromJson(app.body).object().value("id").toString();
  if (!appId.isEmpty()) {
    // View Channel (1024) + Add Reactions (64) + Read Message History (65536).
    out << "2. Open this link, choose the Wavy-Flo server and press Authorize:\n   "
        << "https://discord.com/oauth2/authorize?client_id=" << appId << "&scope=bot&permissions=66624\n"
        << "   (If the report channel is private, also add the bot to it: channel settings > Permissions.)\n";
    ask("   Press Enter when the bot is in the server. ", false);
  }
  out << "\n3. In Discord, right-click the report channel > Copy Channel ID\n"
         "   (needs User Settings > Advanced > Developer Mode on).\n";
  QString channel;
  for (int attempt = 0; attempt < 5; ++attempt) {
    channel = ask("   Paste the channel ID and press Enter: ", false);
    channel.remove(QRegularExpression("[^0-9]"));
    const Reply seen = call(network, "GET", QUrl(api + "/channels/" + channel), token);
    const Reply read = call(network, "GET", QUrl(api + "/channels/" + channel + "/messages?limit=1"), token);
    if (seen.code == 200 && read.code == 200) {
      out << "   The bot can read #" << QJsonDocument::fromJson(seen.body).object().value("name").toString() << ".\n";
      // A message with nothing in it to the bot: Message Content Intent is off.
      const QJsonArray last = QJsonDocument::fromJson(read.body).array();
      if (!last.isEmpty()) {
        const QJsonObject m = last.first().toObject();
        if (m.value("content").toString().isEmpty() && m.value("embeds").toArray().isEmpty() && m.value("attachments").toArray().isEmpty())
          out << "   Note: the newest message reads as empty. Turn on Bot > Message Content Intent in the Developer Portal.\n";
      }
      break;
    }
    err << "   The bot cannot read that channel (HTTP " << seen.code << "/" << read.code << "): "
        << (seen.code == 404 ? "no channel with that ID, or the bot is not in that server." : "it needs View Channel and Read Message History there.")
        << "\n";
    channel.clear();
  }
  if (channel.isEmpty()) return 3;
  QDir().mkpath(folder);
  if (!writeJson(folder + "/config.json", QJsonObject{{"token", token}, {"channel", channel}})) {
    err << "   Could not write " << QDir::toNativeSeparators(folder + "/config.json") << "\n";
    return 4;
  }
  out << "\nDone. Saved to " << QDir::toNativeSeparators(folder + "/config.json")
      << " (keep this file to yourself).\nFrom now on, \"perbarui laporan\" fetches the reports.\n";
  return 0;
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QStringList args = app.arguments().mid(1);
  const bool settingUp = !args.isEmpty() && args.first() == "setup";
  if (settingUp) args.removeFirst();
  const QString folder = QDir(args.isEmpty() ? QCoreApplication::applicationDirPath() + "/../reports" : args.first()).absolutePath();
  if (settingUp) {
    const int result = setup(folder);
    ask("\nPress Enter to close. ", false);
    return result;
  }
  const QJsonObject config = readJson(folder + "/config.json");
  const QString token = config.value("token").toString().trimmed();
  const QString channel = config.value("channel").toString().trimmed();
  if (token.isEmpty() || channel.isEmpty()) {
    err << "No token or channel in " << QDir::toNativeSeparators(folder + "/config.json") << "\n"
        << "It should read: { \"token\": \"<bot token>\", \"channel\": \"<channel id>\" }\n";
    return 2;
  }
  QJsonObject state = readJson(folder + "/state.json");
  QString after = state.value("after").toString("0");

  QNetworkAccessManager network;
  // Every message after the last one fetched, oldest first, a hundred at a time.
  std::vector<QJsonObject> messages;
  for (;;) {
    const Reply page = call(network, "GET", QUrl(QString("%1/channels/%2/messages?limit=100&after=%3").arg(api, channel, after)), token);
    if (page.code != 200) {
      err << "Could not read the channel: HTTP " << page.code << " " << page.error << " " << QString::fromUtf8(page.body).left(300) << "\n";
      if (page.code == 401) err << "The token is not accepted.\n";
      if (page.code == 403) err << "The bot cannot see that channel (View Channel and Read Message History).\n";
      return 3;
    }
    const QJsonArray batch = QJsonDocument::fromJson(page.body).array();
    if (batch.isEmpty()) break;
    for (const auto &value : batch) messages.push_back(value.toObject());
    QString newest = after;
    for (const auto &value : batch) {
      const QString id = value.toObject().value("id").toString();
      if (id.toULongLong() > newest.toULongLong()) newest = id;
    }
    if (newest == after) break;
    after = newest;
    if (batch.size() < 100) break;
  }
  std::sort(messages.begin(), messages.end(), [](const QJsonObject &a, const QJsonObject &b) {
    return a.value("id").toString().toULongLong() < b.value("id").toString().toULongLong();
  });

  int fetched = 0;
  for (const QJsonObject &message : messages) {
    const QString id = message.value("id").toString();
    const QDateTime when = QDateTime::fromString(message.value("timestamp").toString(), Qt::ISODate).toLocalTime();
    const QJsonArray embeds = message.value("embeds").toArray();
    const QJsonObject embed = embeds.isEmpty() ? QJsonObject() : embeds.first().toObject();
    QString title = embed.value("title").toString();
    if (title.isEmpty()) title = message.value("content").toString().section('\n', 0, 0);
    if (title.isEmpty()) title = "(no words)";
    const QString dir = folder + "/" + when.toString("yyyy-MM-dd_HHmm") + "_" + id;
    QDir().mkpath(dir);

    QString text;
    QTextStream md(&text);
    md << "# " << title << "\n\n";
    md << "- Sent: " << when.toString("yyyy-MM-dd HH:mm:ss") << "\n";
    md << "- From: " << message.value("author").toObject().value("username").toString()
       << (message.contains("webhook_id") ? " (Wavy-Flo)" : "") << "\n";
    md << "- Message: " << id << "\n";
    for (const auto &f : embed.value("fields").toArray())
      md << "- " << f.toObject().value("name").toString() << ": " << f.toObject().value("value").toString() << "\n";
    if (embed.contains("footer")) md << "- " << embed.value("footer").toObject().value("text").toString() << "\n";
    md << "\n";
    if (!message.value("content").toString().isEmpty()) md << message.value("content").toString() << "\n\n";
    if (!embed.value("description").toString().isEmpty()) md << embed.value("description").toString() << "\n\n";

    const QJsonArray files = message.value("attachments").toArray();
    if (!files.isEmpty()) md << "## Files\n\n";
    for (const auto &value : files) {
      const QJsonObject file = value.toObject();
      const QString name = safeName(file.value("filename").toString());
      const Reply got = call(network, "GET", QUrl(file.value("url").toString()), QString());
      QFile saved(dir + "/" + name);
      if (got.code == 200 && saved.open(QIODevice::WriteOnly)) {
        saved.write(got.body);
        md << "- " << name << " (" << got.body.size() / 1024 << " KB)\n";
      } else {
        md << "- " << name << " (could not be downloaded: HTTP " << got.code << ")\n";
      }
    }
    QFile report(dir + "/report.md");
    if (report.open(QIODevice::WriteOnly | QIODevice::Truncate)) report.write(text.toUtf8());

    // Fetched: a reaction on it in the channel. (Not being allowed to react
    // does not stop the fetch.)
    call(network, "PUT", QUrl(api + "/channels/" + channel + "/messages/" + id + "/reactions/%E2%9C%85/@me"), token);
    out << QDir::toNativeSeparators(dir) << "\n  " << title << "\n";
    ++fetched;
    state["after"] = id;
    writeJson(folder + "/state.json", state);
  }
  if (!messages.empty()) {
    state["after"] = messages.back().value("id").toString();
    writeJson(folder + "/state.json", state);
  }
  out << fetched << (fetched == 1 ? " new report" : " new reports") << " fetched into " << QDir::toNativeSeparators(folder) << "\n";
  return 0;
}
