#include "render_queue_panel.h"
#include "ffmpeg_export.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString shortFormat(int format) {
  static const QStringList names{"MP4", "PNG sequence", "MOV Animation", "MOV PNG", "MOV ProRes 4444"};
  return names.value(std::clamp(format, 0, 4));
}
}  // namespace

RenderQueuePanel::RenderQueuePanel(RenderQueueHost h, QWidget *parent) : QWidget(parent), host(std::move(h)) {
  setObjectName("renderQueue");
  setWindowTitle("Render Queue");
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(8);
  auto *heading = new QLabel("Render Queue");
  heading->setStyleSheet("color:#9090A8;font-size:10px;font-weight:700;letter-spacing:1.4px;text-transform:uppercase;");
  layout->addWidget(heading);
  auto *explain = new QLabel("Renders run here one after another while you keep working. Each renders the project as it "
                             "is when Render is pressed.");
  explain->setWordWrap(true);
  explain->setStyleSheet("color:#9090A8;font-size:10px;");
  layout->addWidget(explain);

  auto *tools = new QHBoxLayout;
  auto button = [&](QPushButton *&made, const QString &text, const QString &name, const QString &tip) {
    made = new QPushButton(text);
    made->setObjectName("renderQueue/" + name);
    made->setToolTip(tip);
    tools->addWidget(made);
  };
  button(addButton, "Add Main", "Add", "Adds the Main open in the editor, whole (Ctrl+M)");
  button(duplicateButton, "Duplicate", "Duplicate", "A copy of the selected render, to change and render again");
  button(removeButton, "Remove", "Remove", "Takes the selected render off the queue");
  button(upButton, "Move Up", "Up", "Renders it sooner");
  button(downButton, "Move Down", "Down", "Renders it later");
  tools->addStretch();
  layout->addLayout(tools);

  grid = new QTableWidget(0, 6);
  grid->setObjectName("renderQueue/List");
  grid->setHorizontalHeaderLabels({"", "Main", "Frames", "Format", "Output", "Status"});
  grid->setSelectionBehavior(QAbstractItemView::SelectRows);
  grid->setSelectionMode(QAbstractItemView::SingleSelection);
  grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
  grid->verticalHeader()->hide();
  grid->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  grid->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
  grid->setMinimumHeight(150);
  layout->addWidget(grid, 1);

  details = new QGroupBox("Selected render");
  details->setObjectName("renderQueue/Details");
  auto *form = detailsForm = new QFormLayout(details);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setContentsMargins(12, 24, 12, 10); // below the group's title
  composition = new QComboBox;
  composition->setObjectName("renderQueue/Main");
  form->addRow("Main", composition);
  auto *frames = new QWidget;
  auto *framesLayout = new QHBoxLayout(frames);
  framesLayout->setContentsMargins(0, 0, 0, 0);
  from = new QSpinBox;
  from->setObjectName("renderQueue/From");
  to = new QSpinBox;
  to->setObjectName("renderQueue/To");
  for (auto *spin : {from, to}) spin->setRange(0, 1000000);
  wholeButton = new QPushButton("Whole Main");
  wholeButton->setObjectName("renderQueue/Whole");
  framesLayout->addWidget(from);
  framesLayout->addWidget(new QLabel("to"));
  framesLayout->addWidget(to);
  framesLayout->addWidget(wholeButton);
  framesLayout->addStretch();
  form->addRow("Frames", frames);
  format = new QComboBox;
  format->setObjectName("renderQueue/Format");
  format->addItems(renderFormatNames());
  form->addRow("Format", format);
  depth = new QComboBox;
  depth->setObjectName("renderQueue/Depth");
  depth->addItem("As the project", 0);
  depth->addItem("8 bpc", 8);
  depth->addItem("16 bpc", 16);
  depth->addItem("32 bpc float", 32);
  form->addRow("Color depth", depth);
  bitrate = new QSpinBox;
  bitrate->setObjectName("renderQueue/Bitrate");
  bitrate->setRange(1, 2000);
  bitrate->setSuffix(" Mbps");
  form->addRow("Video bitrate", bitrate);
  audio = new QCheckBox("Include audio");
  audio->setObjectName("renderQueue/Audio");
  form->addRow(audio);
  alpha = new QCheckBox("Preserve transparency");
  alpha->setObjectName("renderQueue/Alpha");
  form->addRow(alpha);
  auto *outputRow = new QWidget;
  auto *outputLayout = new QHBoxLayout(outputRow);
  outputLayout->setContentsMargins(0, 0, 0, 0);
  output = new QLineEdit;
  output->setObjectName("renderQueue/Output");
  browseButton = new QPushButton("Browse…");
  outputLayout->addWidget(output, 1);
  outputLayout->addWidget(browseButton);
  form->addRow("Output", outputRow);
  layout->addWidget(details);

  auto *bottom = new QHBoxLayout;
  renderButton = new QPushButton("Render");
  renderButton->setObjectName("renderQueue/Render");
  renderButton->setToolTip("Renders every render switched on that is not done yet, one after another");
  renderButton->setStyleSheet("QPushButton { background:#1F8F70; color:white; font-weight:700; padding:5px 18px; }"
                              "QPushButton:disabled { background:#3A4050; color:#8890A0; }");
  stopButton = new QPushButton("Stop");
  stopButton->setObjectName("renderQueue/Stop");
  overall = new QLabel;
  overall->setObjectName("renderQueue/Overall");
  overall->setWordWrap(true);
  openButton = new QPushButton("Open Output Folder");
  openButton->setObjectName("renderQueue/Open");
  bottom->addWidget(renderButton);
  bottom->addWidget(stopButton);
  bottom->addWidget(overall, 1);
  bottom->addWidget(openButton);
  layout->addLayout(bottom);

  connect(grid, &QTableWidget::itemSelectionChanged, this, [this] { showSelected(); });
  connect(grid, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *cell) {
    if (syncing || !cell || cell->column() != 0) return;
    const int row = cell->row();
    if (row < 0 || row >= int(list.size())) return;
    list[size_t(row)].enabled = cell->checkState() == Qt::Checked;
    if (host.changed) host.changed();
  });
  connect(addButton, &QPushButton::clicked, this, [this] { addCurrent(); });
  connect(duplicateButton, &QPushButton::clicked, this, [this] {
    const int row = selectedRow();
    if (row < 0) return;
    RenderQueueItem copy = list[size_t(row)];
    copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    copy.state = RenderQueueItem::Queued;
    copy.message.clear();
    copy.done = copy.total = 0;
    list.insert(list.begin() + row + 1, copy);
    rebuild();
    grid->selectRow(row + 1);
    if (host.changed) host.changed();
  });
  connect(removeButton, &QPushButton::clicked, this, [this] {
    const int row = selectedRow();
    if (row < 0 || list[size_t(row)].state == RenderQueueItem::Rendering) return;
    list.erase(list.begin() + row);
    rebuild();
    if (!list.empty()) grid->selectRow(std::min(row, int(list.size()) - 1));
    if (host.changed) host.changed();
  });
  auto move = [this](int by) {
    const int row = selectedRow(), other = row + by;
    if (row < 0 || other < 0 || other >= int(list.size())) return;
    std::swap(list[size_t(row)], list[size_t(other)]);
    rebuild();
    grid->selectRow(other);
    if (host.changed) host.changed();
  };
  connect(upButton, &QPushButton::clicked, this, [move] { move(-1); });
  connect(downButton, &QPushButton::clicked, this, [move] { move(1); });
  connect(composition, &QComboBox::activated, this, [this](int index) {
    const QString id = composition->itemData(index).toString(), name = composition->itemText(index);
    edited([&](RenderQueueItem &item) {
      // An output named after the Main follows the Main it renders.
      const QString folder = item.settings.format == 1 ? item.output : QFileInfo(item.output).absolutePath();
      if (item.output.isEmpty() || item.output == defaultRenderOutput(folder, item.name, item.settings.format))
        item.output = defaultRenderOutput(folder, name, item.settings.format);
      item.composition = id;
      item.name = name;
    });
  });
  connect(from, &QSpinBox::valueChanged, this, [this](int value) {
    edited([&](RenderQueueItem &item) {
      item.settings.firstFrame = value;
      if (item.settings.lastFrame >= 0 && item.settings.lastFrame < value) item.settings.lastFrame = value;
    });
  });
  connect(to, &QSpinBox::valueChanged, this, [this](int value) {
    // Up to the Main's last frame: "to the end", so it follows the Main if it grows.
    edited([&](RenderQueueItem &item) { item.settings.lastFrame = value >= framesInMain - 1 ? -1 : value; });
  });
  connect(wholeButton, &QPushButton::clicked, this, [this] {
    edited([](RenderQueueItem &item) {
      item.settings.firstFrame = 0;
      item.settings.lastFrame = -1;
    });
  });
  connect(format, &QComboBox::activated, this, [this](int index) {
    edited([&](RenderQueueItem &item) {
      const int before = item.settings.format;
      if (before == index) return;
      const QString folder = before == 1 ? item.output : QFileInfo(item.output).absolutePath();
      item.settings.format = index;
      item.output = defaultRenderOutput(folder.isEmpty() ? host.projectFolder() : folder, item.name, index);
      if (index == 0) item.settings.preserveTransparency = false;
    });
  });
  connect(depth, &QComboBox::activated, this, [this](int index) {
    const int bits = depth->itemData(index).toInt();
    edited([&](RenderQueueItem &item) { item.settings.processingDepth = bits; });
  });
  connect(bitrate, &QSpinBox::valueChanged, this, [this](int value) {
    edited([&](RenderQueueItem &item) { item.settings.bitrateMbps = value; });
  });
  connect(audio, &QCheckBox::toggled, this, [this](bool on) {
    edited([&](RenderQueueItem &item) { item.settings.includeAudio = on; });
  });
  connect(alpha, &QCheckBox::toggled, this, [this](bool on) {
    edited([&](RenderQueueItem &item) { item.settings.preserveTransparency = on; });
  });
  connect(output, &QLineEdit::editingFinished, this, [this] {
    const QString text = output->text().trimmed();
    edited([&](RenderQueueItem &item) { item.output = text; });
  });
  connect(browseButton, &QPushButton::clicked, this, [this] {
    const RenderQueueItem *item = selected();
    if (!item) return;
    QString chosen;
    if (item->settings.format == 1)
      chosen = QFileDialog::getExistingDirectory(this, "Folder for the frames", item->output);
    else
      chosen = QFileDialog::getSaveFileName(this, "Render to", item->output,
                                            item->settings.format == 0 ? "MP4 (*.mp4)" : "MOV (*.mov)");
    if (chosen.isEmpty()) return;
    edited([&](RenderQueueItem &edit) { edit.output = chosen; });
  });
  connect(renderButton, &QPushButton::clicked, this, [this] { renderAll(); });
  connect(stopButton, &QPushButton::clicked, this, [this] { stopAll(); });
  connect(openButton, &QPushButton::clicked, this, [this] {
    const RenderQueueItem *item = selected();
    if (!item && !list.empty()) item = &list.front();
    if (!item) return;
    const QString folder = item->settings.format == 1 ? item->output : QFileInfo(item->output).absolutePath();
    QDir().mkpath(folder);
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
  });
  rebuild();
}

RenderQueuePanel::~RenderQueuePanel() {
  runner.stop();
  runner.wait();
}

void RenderQueuePanel::setItems(std::vector<RenderQueueItem> items) {
  if (runner.running()) return; // the list being rendered stays until it is done
  list = std::move(items);
  rebuild();
}

int RenderQueuePanel::selectedRow() const {
  const auto rows = grid->selectionModel() ? grid->selectionModel()->selectedRows() : QModelIndexList();
  if (rows.isEmpty()) return -1;
  const int row = rows.front().row();
  return row >= 0 && row < int(list.size()) ? row : -1;
}

RenderQueueItem *RenderQueuePanel::selected() {
  const int row = selectedRow();
  return row < 0 ? nullptr : &list[size_t(row)];
}

void RenderQueuePanel::edited(const std::function<void(RenderQueueItem &)> &change) {
  if (syncing) return;
  RenderQueueItem *item = selected();
  if (!item || item->state == RenderQueueItem::Rendering) return;
  change(*item);
  // Changed, it is a render still to make.
  if (item->state != RenderQueueItem::Queued) {
    item->state = RenderQueueItem::Queued;
    item->message.clear();
  }
  updateRow(selectedRow());
  showSelected();
  if (host.changed) host.changed();
}

void RenderQueuePanel::addCurrent() {
  const QString id = host.currentComposition ? host.currentComposition() : QString();
  QString name = "Main";
  if (host.compositions)
    for (const auto &entry : host.compositions())
      if (entry.first == id) name = entry.second;
  RenderQueueItem item;
  item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  item.composition = id;
  item.name = name;
  if (!list.empty()) item.settings = list.back().settings;
  else {
    // As the Render dialog was last set.
    QSettings settings;
    item.settings.format = std::clamp(settings.value("render/format", 0).toInt(), 0, 4);
    const int bits = settings.value("render/depth", 0).toInt();
    item.settings.processingDepth = bits == 8 || bits == 16 || bits == 32 ? bits : 0;
    item.settings.bitrateMbps = std::clamp(settings.value("render/mp4BitrateMbps", 50).toInt(), 1, 2000);
    item.settings.includeAudio = settings.value("render/includeAudio", true).toBool();
    item.settings.preserveTransparency = settings.value("render/preserveTransparency", false).toBool();
  }
  item.settings.firstFrame = 0;
  item.settings.lastFrame = -1;
  // The Main with a Play Range set renders that range, the way a work area
  // does: what is being worked on is what goes out. The From and To fields
  // still take it anywhere.
  if (id.isEmpty() && host.snapshot) {
    const Scene now = host.snapshot();
    if (hasPlayRange(now)) {
      item.settings.firstFrame = playFirst(now);
      item.settings.lastFrame = playLast(now) >= now.frames - 1 ? -1 : playLast(now);
    }
  }
  const QString folder = !list.empty() ? (list.back().settings.format == 1 ? list.back().output
                                                                           : QFileInfo(list.back().output).absolutePath())
                                       : host.projectFolder ? host.projectFolder() : QDir::currentPath();
  item.output = defaultRenderOutput(folder, name, item.settings.format);
  list.push_back(item);
  rebuild();
  grid->selectRow(int(list.size()) - 1);
  if (host.changed) host.changed();
  if (host.status) host.status("Added “" + name + "” to the Render Queue");
}

void RenderQueuePanel::renderAll() {
  if (runner.running()) return;
  const Scene project = host.snapshot ? host.snapshot() : Scene();
  std::vector<RenderQueueRunner::Job> jobs;
  const QString ffmpeg = findExportFFmpeg();
  for (auto &item : list) {
    if (!item.enabled || item.state == RenderQueueItem::Done) continue;
    RenderQueueRunner::Job job;
    job.id = item.id;
    job.scene = compositionScene(project, item.composition);
    job.settings = item.settings;
    job.settings.ffmpegPath = ffmpeg;
    job.target = renderQueueTarget(item);
    item.target = job.target;
    item.state = RenderQueueItem::Queued;
    item.message = "Waiting";
    item.done = item.total = 0;
    jobs.push_back(std::move(job));
  }
  if (jobs.empty()) {
    overall->setText("Nothing to render: every render here is done or switched off.");
    return;
  }
  rebuild();
  overall->setText(QString("Rendering %1 of them…").arg(jobs.size()));
  renderButton->setEnabled(false);
  runner.start(std::move(jobs), host.renderer ? host.renderer() : std::function<QImage(Scene &)>(),
               [this](const QString &id, int state, int done, int total, const QString &message) {
                 QMetaObject::invokeMethod(this, [=] { report(id, state, done, total, message); }, Qt::QueuedConnection);
               });
  showSelected();
}

void RenderQueuePanel::stopAll() {
  if (!runner.running()) return;
  runner.stop();
  overall->setText("Stopping…");
}

void RenderQueuePanel::report(const QString &id, int state, int done, int total, const QString &message) {
  if (id.isEmpty()) {
    // The whole run is over.
    runner.wait();
    int made = 0, failed = 0, stopped = 0;
    for (const auto &item : list) {
      made += item.state == RenderQueueItem::Done;
      failed += item.state == RenderQueueItem::Failed;
      stopped += item.state == RenderQueueItem::Stopped;
    }
    const QString summary = QString("Render Queue finished: %1 done%2%3.").arg(made)
                                .arg(failed ? QString(", %1 failed").arg(failed) : QString())
                                .arg(stopped ? QString(", %1 stopped").arg(stopped) : QString());
    overall->setText(summary);
    renderButton->setEnabled(true);
    if (host.status) host.status(summary);
    if (host.changed) host.changed(); // a finished render is kept as finished
    showSelected();
    return;
  }
  for (int row = 0; row < int(list.size()); ++row) {
    RenderQueueItem &item = list[size_t(row)];
    if (item.id != id) continue;
    item.state = RenderQueueItem::State(state);
    item.done = done;
    item.total = total;
    item.message = message;
    updateRow(row);
    if (state == RenderQueueItem::Rendering)
      overall->setText("“" + item.name + "”: " + message);
    if (row == selectedRow()) showSelected();
    break;
  }
}

void RenderQueuePanel::rebuild() {
  syncing = true;
  framesInMain = std::max(1, host.frames ? host.frames() : 1);
  grid->setRowCount(int(list.size()));
  syncing = false;
  for (int row = 0; row < int(list.size()); ++row) updateRow(row);
  showSelected();
}

void RenderQueuePanel::updateRow(int row) {
  if (row < 0 || row >= int(list.size())) return;
  const RenderQueueItem &item = list[size_t(row)];
  syncing = true;
  auto cell = [&](int column) {
    QTableWidgetItem *c = grid->item(row, column);
    if (!c) {
      c = new QTableWidgetItem;
      grid->setItem(row, column, c);
    }
    return c;
  };
  QTableWidgetItem *on = cell(0);
  on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
  on->setCheckState(item.enabled ? Qt::Checked : Qt::Unchecked);
  on->setToolTip("Rendered when Render is pressed");
  cell(1)->setText(item.name);
  const int last = item.settings.lastFrame < 0 ? framesInMain - 1 : item.settings.lastFrame;
  cell(2)->setText(QString("%1–%2").arg(item.settings.firstFrame).arg(last));
  cell(2)->setToolTip(QString("%1 frames").arg(std::max(0, last - item.settings.firstFrame + 1)));
  cell(3)->setText(shortFormat(item.settings.format));
  cell(4)->setText(item.settings.format == 1 ? QDir(item.output).dirName() + "/" : QFileInfo(item.output).fileName());
  cell(4)->setToolTip(item.output);
  QString status;
  QColor colour("#C8CCD8");
  switch (item.state) {
  case RenderQueueItem::Queued: status = item.message.isEmpty() ? "Queued" : item.message; break;
  case RenderQueueItem::Rendering:
    status = item.total > 0 ? QString("Rendering %1%").arg(100 * item.done / std::max(1, item.total)) : "Rendering";
    colour = QColor("#2D8CFF");
    break;
  case RenderQueueItem::Done: status = item.message.isEmpty() ? "Done" : item.message; colour = QColor("#7FD67F"); break;
  case RenderQueueItem::Failed: status = "Failed: " + item.message; colour = QColor("#FF7A7A"); break;
  case RenderQueueItem::Stopped: status = item.message.isEmpty() ? "Stopped" : item.message; colour = QColor("#E0B050"); break;
  }
  cell(5)->setText(status);
  cell(5)->setToolTip(item.message);
  cell(5)->setForeground(colour);
  syncing = false;
}

void RenderQueuePanel::showSelected() {
  const RenderQueueItem *item = selected();
  const bool running = runner.running();
  duplicateButton->setEnabled(item);
  removeButton->setEnabled(item && item->state != RenderQueueItem::Rendering);
  upButton->setEnabled(item && selectedRow() > 0);
  downButton->setEnabled(item && selectedRow() + 1 < int(list.size()));
  stopButton->setEnabled(running);
  renderButton->setEnabled(!running && !list.empty());
  openButton->setEnabled(!list.empty());
  details->setEnabled(item && item->state != RenderQueueItem::Rendering);
  if (!item) return;
  syncing = true;
  composition->clear();
  if (host.compositions)
    for (const auto &entry : host.compositions()) composition->addItem(entry.second, entry.first);
  int index = composition->findData(item->composition);
  if (index < 0) {
    composition->addItem(item->name + " (gone)", item->composition);
    index = composition->count() - 1;
  }
  composition->setCurrentIndex(index);
  from->setMaximum(framesInMain - 1);
  to->setMaximum(framesInMain - 1);
  from->setValue(std::min(item->settings.firstFrame, framesInMain - 1));
  to->setValue(item->settings.lastFrame < 0 ? framesInMain - 1 : std::min(item->settings.lastFrame, framesInMain - 1));
  format->setCurrentIndex(std::clamp(item->settings.format, 0, 4));
  // "As the project (8 bpc)": what that is now.
  if (host.snapshot) depth->setItemText(0, QString("As the project (%1 bpc)").arg(host.snapshot().processingDepth));
  depth->setCurrentIndex(std::max(0, depth->findData(item->settings.processingDepth)));
  bitrate->setValue(item->settings.bitrateMbps);
  // Only what the format has: a bitrate for MP4, transparency for the rest.
  detailsForm->setRowVisible(bitrate, item->settings.format == 0);
  audio->setChecked(item->settings.includeAudio);
  alpha->setChecked(item->settings.preserveTransparency);
  detailsForm->setRowVisible(alpha, item->settings.format != 0);
  output->setText(item->output);
  syncing = false;
}
