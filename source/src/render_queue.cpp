#include "render_queue.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QUuid>
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

BelowNormalPriority::BelowNormalPriority() {
#ifdef Q_OS_WIN
  previous = GetThreadPriority(GetCurrentThread());
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}
BelowNormalPriority::~BelowNormalPriority() {
#ifdef Q_OS_WIN
  SetThreadPriority(GetCurrentThread(), previous);
#endif
}

QJsonObject renderQueueItemJson(const RenderQueueItem &item) {
  const RenderSettings &s = item.settings;
  return QJsonObject{{"id", item.id},
                     {"enabled", item.enabled},
                     {"composition", item.composition},
                     {"name", item.name},
                     {"output", item.output},
                     {"format", s.format},
                     {"processingDepth", s.processingDepth},
                     {"bitrateMbps", s.bitrateMbps},
                     {"includeAudio", s.includeAudio},
                     {"preserveTransparency", s.preserveTransparency},
                     {"firstFrame", s.firstFrame},
                     {"lastFrame", s.lastFrame},
                     // Kept so a finished render still reads as finished.
                     {"done", item.state == RenderQueueItem::Done}};
}

RenderQueueItem renderQueueItemFromJson(const QJsonObject &j) {
  RenderQueueItem item;
  item.id = j["id"].toString();
  if (item.id.isEmpty()) item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  item.enabled = j["enabled"].toBool(true);
  item.composition = j["composition"].toString();
  item.name = j["name"].toString("Main");
  item.output = j["output"].toString();
  RenderSettings &s = item.settings;
  s.format = std::clamp(j["format"].toInt(0), 0, 4);
  // 0: the project's depth, whatever it is when the item renders.
  const int depth = j["processingDepth"].toInt(0);
  s.processingDepth = depth == 32 || depth == 16 || depth == 8 ? depth : 0;
  s.bitrateMbps = std::clamp(j["bitrateMbps"].toInt(50), 1, 2000);
  s.includeAudio = j["includeAudio"].toBool(true);
  s.preserveTransparency = j["preserveTransparency"].toBool(false);
  s.firstFrame = std::max(0, j["firstFrame"].toInt(0));
  s.lastFrame = std::max(-1, j["lastFrame"].toInt(-1));
  if (j["done"].toBool()) {
    item.state = RenderQueueItem::Done;
    item.message = "Done";
  }
  return item;
}

QJsonArray renderQueueJson(const std::vector<RenderQueueItem> &items) {
  QJsonArray array;
  for (const auto &item : items) array.append(renderQueueItemJson(item));
  return array;
}

std::vector<RenderQueueItem> renderQueueFromJson(const QJsonArray &array) {
  std::vector<RenderQueueItem> items;
  for (const auto &value : array)
    if (value.isObject() && items.size() < 500) items.push_back(renderQueueItemFromJson(value.toObject()));
  return items;
}

Scene compositionScene(const Scene &scene, const QString &groupId) {
  if (groupId.isEmpty()) return scene;
  Scene isolated = scene;
  isolated.layers.clear();
  // Descendants as well as direct members: a nested group keeps owning its
  // own, and the direct members become the top level.
  QSet<QString> contained{groupId};
  for (bool more = true; more;) {
    more = false;
    for (const auto &candidate : scene.layers)
      if (candidate.type == "group" && contained.contains(candidate.groupId) && !contained.contains(candidate.id)) {
        contained.insert(candidate.id);
        more = true;
      }
  }
  for (const auto &candidate : scene.layers) {
    if (!contained.contains(candidate.groupId)) continue;
    isolated.layers.push_back(candidate);
    if (isolated.layers.back().groupId == groupId) isolated.layers.back().groupId.clear();
  }
  // Seen as it is drawn in the Main: a group that shares the Main's camera and
  // lights has them, and those of each level above it for as long as the
  // groups on the way share theirs too.
  for (QString level = groupId; !level.isEmpty();) {
    const Layer *group = nullptr;
    for (const auto &candidate : scene.layers)
      if (candidate.id == level) group = &candidate;
    if (!group || !group->sharesMainView || isolated.layers.empty()) break;
    addSharedView(isolated, scene, group->groupId);
    level = group->groupId;
  }
  isolated.selected = -1;
  return isolated;
}

namespace {
QString safeName(QString name) {
  static const QString forbidden = "<>:\"/\\|?*";
  for (QChar &c : name)
    if (forbidden.contains(c) || c.unicode() < 32) c = '_';
  name = name.trimmed();
  return name.isEmpty() ? QString("Render") : name;
}
QString extensionFor(int format) { return format == 0 ? ".mp4" : format >= 2 ? ".mov" : QString(); }
}  // namespace

QString defaultRenderOutput(const QString &folder, const QString &name, int format) {
  if (format == 1) return QDir(folder).absolutePath();
  return QDir(folder).absoluteFilePath(safeName(name) + extensionFor(format));
}

QString renderQueueTarget(const RenderQueueItem &item) {
  const int format = std::clamp(item.settings.format, 0, 4);
  if (format == 1) {
    // A folder of frames, beside anything made before rather than over it.
    const QDir parent(item.output.isEmpty() ? QDir::currentPath() : item.output);
    const QString base = safeName(item.name) + " frames";
    QString candidate = parent.absoluteFilePath(base);
    for (int n = 2; QFileInfo::exists(candidate) && n < 10000; ++n)
      candidate = parent.absoluteFilePath(base + QString(" %1").arg(n));
    return candidate;
  }
  QString target = item.output;
  const QString extension = extensionFor(format);
  if (!target.endsWith(extension, Qt::CaseInsensitive)) {
    // Given a folder, or a name with another ending: the Main's own name, with this one.
    const QFileInfo info(target);
    if (target.isEmpty() || info.isDir()) target = defaultRenderOutput(target.isEmpty() ? QDir::currentPath() : target, item.name, format);
    else target = info.absolutePath() + "/" + info.completeBaseName() + extension;
  }
  return target;
}

RenderQueueRunner::~RenderQueueRunner() {
  stop();
  wait();
}

void RenderQueueRunner::wait() {
  if (thread.joinable()) thread.join();
}

void RenderQueueRunner::start(std::vector<Job> jobs, std::function<QImage(Scene &)> render, Report report) {
  stop();
  wait();
  stopping = false;
  busy = true;
  thread = std::thread([this, jobs = std::move(jobs), render = std::move(render), report = std::move(report)]() mutable {
    for (auto &job : jobs) {
      if (stopping) {
        report(job.id, RenderQueueItem::Stopped, 0, 0, "Stopped before it began");
        continue;
      }
      report(job.id, RenderQueueItem::Rendering, 0, 1, "Starting…");
      QDir().mkpath(QFileInfo(job.target).absolutePath());
      QElapsedTimer clock;
      clock.start();
      const QString error = renderToFile(job.scene, job.settings, job.target, render,
                                         [&](int done, int total, const QString &label) {
                                           report(job.id, RenderQueueItem::Rendering, done, total, label);
                                           return !stopping.load();
                                         });
      const double seconds = clock.nsecsElapsed() / 1e9;
      if (error.isEmpty())
        report(job.id, RenderQueueItem::Done, 1, 1,
               QString("Done in %1").arg(seconds < 60 ? QString::number(seconds, 'f', 1) + " s"
                                                      : QString("%1 min %2 s").arg(int(seconds) / 60).arg(int(seconds) % 60)));
      else if (error == "Canceled") report(job.id, RenderQueueItem::Stopped, 0, 0, "Stopped");
      else report(job.id, RenderQueueItem::Failed, 0, 0, error);
    }
    busy = false;
    report(QString(), -1, 0, 0, QString());
  });
}
