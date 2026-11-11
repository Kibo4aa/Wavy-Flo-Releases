#pragma once
// The Render Queue's window: the list, the settings of the render picked in
// it, and Render / Stop. It knows the editor only through RenderQueueHost.
#include "render_queue.h"
#include <QWidget>
#include <functional>
#include <utility>
#include <vector>

class QTableWidget;
class QComboBox;
class QSpinBox;
class QCheckBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QGroupBox;
class QFormLayout;

struct RenderQueueHost {
  std::function<Scene()> snapshot;                                          // the project as it is now
  std::function<std::vector<std::pair<QString, QString>>()> compositions;   // id and name; "" is the Main
  std::function<QString()> currentComposition;                              // the one open in the editor
  std::function<int()> frames;                                              // how long the Main is
  std::function<QString()> projectFolder;
  std::function<std::function<QImage(Scene &)>()> renderer;
  std::function<void()> changed;                                            // the list changed
  std::function<void(const QString &)> status;                              // a line for the status bar
};

class RenderQueuePanel : public QWidget {
public:
  RenderQueuePanel(RenderQueueHost host, QWidget *parent = nullptr);
  ~RenderQueuePanel() override;
  void setItems(std::vector<RenderQueueItem> items);
  const std::vector<RenderQueueItem> &items() const { return list; }
  // The Main open in the editor, whole, as the last render added was made.
  void addCurrent();
  void renderAll();
  void stopAll();
  bool rendering() const { return runner.running(); }
  void waitUntilStopped() { runner.stop(); runner.wait(); }
  QTableWidget *table() const { return grid; }

private:
  void rebuild();
  void showSelected();
  void updateRow(int row);
  int selectedRow() const;
  RenderQueueItem *selected();
  void edited(const std::function<void(RenderQueueItem &)> &change);
  void report(const QString &id, int state, int done, int total, const QString &message);

  RenderQueueHost host;
  std::vector<RenderQueueItem> list;
  RenderQueueRunner runner;
  bool syncing = false;
  int framesInMain = 1;
  QTableWidget *grid = nullptr;
  QPushButton *addButton = nullptr, *duplicateButton = nullptr, *removeButton = nullptr, *upButton = nullptr,
              *downButton = nullptr, *renderButton = nullptr, *stopButton = nullptr, *openButton = nullptr,
              *wholeButton = nullptr, *browseButton = nullptr;
  QGroupBox *details = nullptr;
  QFormLayout *detailsForm = nullptr;
  QComboBox *composition = nullptr, *format = nullptr, *depth = nullptr;
  QSpinBox *from = nullptr, *to = nullptr, *bitrate = nullptr;
  QCheckBox *audio = nullptr, *alpha = nullptr;
  QLineEdit *output = nullptr;
  QLabel *overall = nullptr;
};
