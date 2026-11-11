#pragma once
// The Render Queue: renders lined up to run one after another on a thread of
// their own, so work on the project goes on while they run. The list is kept
// with the project; what each item renders is the project as it was the
// moment Render was pressed.
#include "render_export.h"
#include <QJsonArray>
#include <QJsonObject>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

struct RenderQueueItem {
  enum State { Queued, Rendering, Done, Failed, Stopped };
  QString id;
  bool enabled = true;
  // Empty: the whole Main. Otherwise a group, rendered as a Main of its own.
  QString composition;
  QString name;
  RenderSettings settings; // format, depth, sound, and the frames (firstFrame, lastFrame)
  // A movie's file, or the folder a PNG sequence is made in.
  QString output;
  // While and after it runs; not saved.
  State state = Queued;
  QString target; // where it went (or is going): set as Render starts
  QString message;
  int done = 0, total = 0;
};

QJsonObject renderQueueItemJson(const RenderQueueItem &);
RenderQueueItem renderQueueItemFromJson(const QJsonObject &);
QJsonArray renderQueueJson(const std::vector<RenderQueueItem> &);
std::vector<RenderQueueItem> renderQueueFromJson(const QJsonArray &);

// A group as a Main of its own: its layers and those of the groups inside it,
// its own members at the top. Empty id: the scene as it is.
Scene compositionScene(const Scene &, const QString &groupId);
// Where an item's render lands: a movie's file with the ending its format
// needs, or, for a PNG sequence, a folder named after the Main inside the
// output folder -- one not there yet, so nothing is written over.
QString renderQueueTarget(const RenderQueueItem &);
// A file name for a Main's render in a folder: the Main's name, made safe.
QString defaultRenderOutput(const QString &folder, const QString &name, int format);

class RenderQueueRunner {
public:
  struct Job { QString id; Scene scene; RenderSettings settings; QString target; };
  // Heard on the runner's thread: which item, how it stands, how far it has
  // got, and what happened. An empty id means the whole run is over.
  using Report = std::function<void(const QString &id, int state, int done, int total, const QString &message)>;
  ~RenderQueueRunner();
  // Renders the jobs one after another; returns at once.
  void start(std::vector<Job> jobs, std::function<QImage(Scene &)> render, Report report);
  // The render under way stops at its next frame; the rest do not start.
  void stop() { stopping = true; }
  bool running() const { return busy; }
  void wait();
private:
  std::thread thread;
  std::atomic<bool> stopping{false}, busy{false};
};

// For as long as it lives, the thread it was made on runs below normal
// priority, so what the person is doing now is given the processor first.
class BelowNormalPriority {
public:
  BelowNormalPriority();
  ~BelowNormalPriority();
private:
  int previous = 0;
};

int checkRenderQueue(QStringList &report); // --render-queue-self-test
