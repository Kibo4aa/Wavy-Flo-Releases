#pragma once

#include <QWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QMenu>
#include <QSettings>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QMouseEvent>
#include <QStyle>
#include <QApplication>
#include <QFrame>
#include <QVector>
#include <QMap>
#include <QSlider>
#include <QProgressBar>
#include <QLineEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QGridLayout>
#include <QDialog>
#include <QPointer>
#include <QDockWidget>
#include <QMainWindow>
#include <QTimer>
#include <QScreen>
#include <QGuiApplication>
#include <algorithm>
#include <functional>

#include "dock_panel.h"

class StackedPanelDrawer;
void syncDockPanels(QMainWindow *window);

class StackedPanelDock : public DockPanel {
  StackedPanelDrawer *m_drawer = nullptr;
public:
  StackedPanelDock(StackedPanelDrawer *drawer, const QString &title, QMainWindow *parent = nullptr);
  StackedPanelDrawer *drawer() const { return m_drawer; }
protected:
  void closeEvent(QCloseEvent *event) override;
};

// ============================================================================
// DrawerResizeHandle: Handle di bagian bawah laci untuk mengatur tinggi bebas
// ============================================================================
class DrawerResizeHandle : public QWidget {
  StackedPanelDrawer *m_drawer = nullptr;
  QPoint m_pressPos;
  int m_startHeight = 0;
  bool m_dragging = false;

public:
  explicit DrawerResizeHandle(StackedPanelDrawer *drawer);

protected:
  void paintEvent(QPaintEvent *e) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void mouseDoubleClickEvent(QMouseEvent *e) override;
};

// ============================================================================
// StackedPanelDrawer: Satu laci panel bergaya Adobe After Effects
// Mendukung: buka/tutup, reorder drag-and-drop, resizable tinggi, undock/float
// ============================================================================
class StackedPanelDrawer : public QWidget {
  QString m_id;
  QString m_title;
  int m_defaultOrder = 0;
  QVBoxLayout *m_boxLayout = nullptr;
  QWidget *m_header = nullptr;
  QLabel *m_titleLabel = nullptr;
  QToolButton *m_menuBtn = nullptr;
  QToolButton *m_closeBtn = nullptr;
  QWidget *m_contentContainer = nullptr;
  QVBoxLayout *m_contentLayout = nullptr;
  QWidget *m_content = nullptr;
  DrawerResizeHandle *m_resizeHandle = nullptr;
  QPointer<QDialog> m_floatingDialog;
  QPointer<StackedPanelDock> m_workspaceDock;
  QPointer<QScrollArea> m_dockScroll;
  QTimer *m_saveTimer = nullptr;
  bool m_open = false;
  int m_customHeight = -1;
  int m_defaultMinHeight = 100;

  QPoint m_pressPos;
  bool m_dragStarted = false;
  // A drawer moves only when taken by its title's words (onTitleWords); a
  // press elsewhere on the header that then wanders off does nothing.
  bool m_pressOnWords = false, m_movedAway = false;

public:
  std::function<void(StackedPanelDrawer *, bool)> onToggled;
  std::function<void(StackedPanelDrawer *)> onCloseRequested;
  std::function<void(StackedPanelDrawer *)> onCloseOthersRequested;
  std::function<void(StackedPanelDrawer *)> onMoveUpRequested;
  std::function<void(StackedPanelDrawer *)> onMoveDownRequested;
  std::function<void()> onResetOrderRequested;
  std::function<void(StackedPanelDrawer *, const QPoint &)> onDragMoved;
  std::function<void(StackedPanelDrawer *)> onDragFinished;
  std::function<void(StackedPanelDrawer *, const QPoint &)> onDragOutOfStack;

  StackedPanelDrawer(const QString &id, const QString &title, QWidget *content,
                     bool defaultOpen = false, int defaultMinHeight = 120,
                     QWidget *parent = nullptr)
      : QWidget(parent), m_id(id), m_title(title), m_content(content),
        m_defaultMinHeight(defaultMinHeight) {
    setObjectName("drawer/" + id);
    m_boxLayout = new QVBoxLayout(this);
    m_boxLayout->setContentsMargins(0, 0, 0, 0);
    m_boxLayout->setSpacing(0);

    // 1. Header Bar (~26px)
    m_header = new QWidget(this);
    m_header->setObjectName("drawerHeader");
    m_header->setFixedHeight(26);
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->setStyleSheet(
        "QWidget#drawerHeader {"
        "  background: #232323;"
        "  border-top: 1px solid #1C1C1C;"
        "  border-bottom: 1px solid #181818;"
        "}"
        "QWidget#drawerHeader:hover {"
        "  background: #2B2B2B;"
        "}");

    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(10, 0, 6, 0);
    headerLayout->setSpacing(6);

    // Title label
    m_titleLabel = new QLabel(m_title, m_header);
    m_titleLabel->setStyleSheet(
        "color: #D4D4D4; font-size: 11px; font-weight: 600; letter-spacing: 0.2px; background: transparent;");
    headerLayout->addWidget(m_titleLabel);
    headerLayout->addStretch(1);

    // Burger / Panel Options Menu Button (≡)
    m_menuBtn = new QToolButton(m_header);
    m_menuBtn->setObjectName("drawerMenuBtn");
    m_menuBtn->setFixedSize(18, 18);
    m_menuBtn->setToolTip(m_title + " Options");
    m_menuBtn->setCursor(Qt::ArrowCursor);
    m_menuBtn->setStyleSheet(
        "QToolButton#drawerMenuBtn {"
        "  background: transparent;"
        "  border: none;"
        "  color: #8E8E8E;"
        "  font-size: 13px;"
        "  font-weight: bold;"
        "}"
        "QToolButton#drawerMenuBtn:hover {"
        "  color: #FFFFFF;"
        "  background: #333333;"
        "  border-radius: 2px;"
        "}");
    m_menuBtn->setText("≡");

    connect(m_menuBtn, &QToolButton::clicked, this, [this] {
      showMenu();
    });

    headerLayout->addWidget(m_menuBtn);

    // Close Button (×)
    m_closeBtn = new QToolButton(m_header);
    m_closeBtn->setObjectName("drawerCloseBtn");
    m_closeBtn->setFixedSize(18, 18);
    m_closeBtn->setToolTip("Close " + m_title + " Panel");
    m_closeBtn->setCursor(Qt::ArrowCursor);
    m_closeBtn->setStyleSheet(
        "QToolButton#drawerCloseBtn {"
        "  background: transparent;"
        "  border: none;"
        "  color: #8E8E8E;"
        "  font-size: 14px;"
        "  font-weight: bold;"
        "}"
        "QToolButton#drawerCloseBtn:hover {"
        "  color: #FF5252;"
        "  background: #333333;"
        "  border-radius: 2px;"
        "}");
    m_closeBtn->setText("×");

    connect(m_closeBtn, &QToolButton::clicked, this, [this] {
      closePanel();
    });

    headerLayout->addWidget(m_closeBtn);
    m_boxLayout->addWidget(m_header);

    // 2. Content Container (permanent slot index 1)
    m_contentContainer = new QWidget(this);
    m_contentContainer->setObjectName("drawerContentContainer");
    m_contentLayout = new QVBoxLayout(m_contentContainer);
    m_contentLayout->setContentsMargins(0, 0, 0, 0);
    m_contentLayout->setSpacing(0);
    if (m_content) {
      m_content->setParent(m_contentContainer);
      m_contentLayout->addWidget(m_content);
    }
    m_boxLayout->addWidget(m_contentContainer);

    // 3. Resize Handle di bagian bawah laci saat terbuka (permanent slot index 2)
    m_resizeHandle = new DrawerResizeHandle(this);
    m_boxLayout->addWidget(m_resizeHandle);

    // Install event filter untuk drag reorder & click toggle pada header & title
    m_header->installEventFilter(this);
    m_titleLabel->installEventFilter(this);

    // Restore saved height if any
    QSettings settings("Wavy-Flo", "Wavy-Flo");
    int savedHeight = settings.value("stackedPanels/" + m_id + "/height", -1).toInt();
    if (savedHeight > 0) {
      setCustomHeight(qMax(24, savedHeight), false);
    } else if (m_content) {
      m_content->setMinimumSize(0, 0);
    }

    // Load saved state or default
    bool savedOpen = settings.value("stackedPanels/" + m_id + "/open", defaultOpen).toBool();
    setOpen(savedOpen, false);
  }

  void applyUiScale(double scale) {
    int hH = std::max(18, (int)std::round(26.0 * scale));
    if (m_header) {
      m_header->setFixedHeight(hH);
    }
    int titleFontSize = std::max(8, (int)std::round(11.0 * scale));
    if (m_titleLabel) {
      m_titleLabel->setStyleSheet(
          QString("color: #D4D4D4; font-size: %1px; font-weight: 600; letter-spacing: 0.2px; background: transparent;").arg(titleFontSize));
    }
    int btnSz = std::max(14, (int)std::round(18.0 * scale));
    int menuFontSize = std::max(9, (int)std::round(13.0 * scale));
    int closeFontSize = std::max(10, (int)std::round(14.0 * scale));
    if (m_menuBtn) {
      m_menuBtn->setFixedSize(btnSz, btnSz);
      m_menuBtn->setStyleSheet(
          QString("QToolButton#drawerMenuBtn { background: transparent; border: none; color: #8E8E8E; font-size: %1px; font-weight: bold; } QToolButton#drawerMenuBtn:hover { color: #FFFFFF; background: #333333; border-radius: 2px; }").arg(menuFontSize));
    }
    if (m_closeBtn) {
      m_closeBtn->setFixedSize(btnSz, btnSz);
      m_closeBtn->setStyleSheet(
          QString("QToolButton#drawerCloseBtn { background: transparent; border: none; color: #8E8E8E; font-size: %1px; font-weight: bold; } QToolButton#drawerCloseBtn:hover { color: #FF5252; background: #333333; border-radius: 2px; }").arg(closeFontSize));
    }
    if (m_resizeHandle) {
      m_resizeHandle->setFixedHeight(std::max(4, (int)std::round(7.0 * scale)));
    }
  }

  const QString &id() const { return m_id; }
  const QString &title() const { return m_title; }
  QWidget *content() const { return m_content; }
  bool isOpen() const { return m_open; }
  bool isFloating() const {
    if (m_workspaceDock) return m_workspaceDock->isFloating();
    return false;
  }
  bool isDockedInWorkspace() const { return m_workspaceDock != nullptr; }
  QDockWidget *workspaceDock() const { return m_workspaceDock.data(); }
  int defaultOrder() const { return m_defaultOrder; }
  void setDefaultOrder(int o) { m_defaultOrder = o; }
  int defaultMinHeight() const { return m_defaultMinHeight; }
  int customHeight() const { return m_customHeight; }

  QMainWindow *findMainWindow() const {
    const QWidget *w = this;
    while (w) {
      if (auto *mw = qobject_cast<const QMainWindow *>(w)) {
        return const_cast<QMainWindow *>(mw);
      }
      w = w->parentWidget();
    }
    return nullptr;
  }

  void setOpen(bool open, bool save = true) {
    m_open = open;
    const bool inDrawer = (!m_workspaceDock);
    if (m_contentContainer) {
      m_contentContainer->setVisible(m_open && inDrawer);
    }
    if (m_content) {
      m_content->setVisible(m_open && inDrawer);
    }
    if (m_resizeHandle) {
      m_resizeHandle->setVisible(m_open && inDrawer);
    }
    m_titleLabel->setStyleSheet(m_open
        ? "color: #FFFFFF; font-size: 11px; font-weight: 600; letter-spacing: 0.2px; background: transparent;"
        : "color: #9E9E9E; font-size: 11px; font-weight: 500; letter-spacing: 0.2px; background: transparent;");

    if (save) {
      if (!m_saveTimer) {
        m_saveTimer = new QTimer(this);
        m_saveTimer->setSingleShot(true);
        connect(m_saveTimer, &QTimer::timeout, this, [this] {
          QSettings settings("Wavy-Flo", "Wavy-Flo");
          settings.setValue("stackedPanels/" + m_id + "/open", m_open);
        });
      }
      m_saveTimer->start(200);
    }

    if (onToggled) {
      onToggled(this, m_open);
    }
  }

  void toggleOpen() {
    setOpen(!m_open);
  }

  void setContent(QWidget *content) {
    if (m_content == content) return;
    if (m_content && m_contentLayout) {
      m_contentLayout->removeWidget(m_content);
    }
    m_content = content;
    if (m_content && m_contentContainer && m_contentLayout) {
      m_content->setParent(m_contentContainer);
      m_contentLayout->addWidget(m_content);
      const bool inDrawer = (!m_workspaceDock);
      m_content->setVisible(m_open && inDrawer);
      if (m_customHeight > 0) {
        m_content->setFixedHeight(m_customHeight);
      } else {
        m_content->setMinimumSize(0, 0);
      }
    }
  }

  void setCustomHeight(int h, bool save = true) {
    m_customHeight = qMax(24, h);
    if (m_content) {
      m_content->setFixedHeight(m_customHeight);
    }
    if (save) {
      QSettings settings("Wavy-Flo", "Wavy-Flo");
      settings.setValue("stackedPanels/" + m_id + "/height", m_customHeight);
    }
  }

  void resetCustomHeight() {
    m_customHeight = -1;
    if (m_content) {
      m_content->setMinimumSize(0, 0);
      m_content->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    }
    QSettings settings("Wavy-Flo", "Wavy-Flo");
    settings.remove("stackedPanels/" + m_id + "/height");
  }

  void positionFloatingDock(const QPoint &headerGlobal) {
    if (!m_workspaceDock) return;
    int preferredH = qMax(m_defaultMinHeight + 50, 160);
    if (m_content) {
      preferredH = qMax(preferredH, m_content->sizeHint().height() + 50);
    }
    const QSize targetSize = m_workspaceDock->sizeHint().expandedTo(QSize(280, qMin(420, preferredH)));
    m_workspaceDock->resize(targetSize);

    int targetX = headerGlobal.x() - targetSize.width() - 16;
    int targetY = headerGlobal.y();

    QScreen *screen = QGuiApplication::screenAt(headerGlobal);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
      const QRect avail = screen->availableGeometry();
      targetX = qBound(avail.left() + 20, targetX, avail.right() - targetSize.width() - 20);
      targetY = qBound(avail.top() + 30, targetY, avail.bottom() - targetSize.height() - 20);
    }
    m_workspaceDock->move(targetX, targetY);
  }

  // Floating window undock / redock
  void floatPanel() {
    QPoint preferredPos;
    if (m_header && m_header->isVisible()) {
      preferredPos = m_header->mapToGlobal(QPoint(0, 0));
    } else {
      preferredPos = QCursor::pos();
    }

    if (m_workspaceDock) {
      const bool wasFloating = m_workspaceDock->isFloating();
      m_workspaceDock->setFloating(true);
      m_workspaceDock->show();
      m_workspaceDock->raise();
      m_workspaceDock->activateWindow();
      if (!wasFloating) {
        positionFloatingDock(preferredPos);
      }
      if (auto *mw = findMainWindow()) {
        syncDockPanels(mw);
      }
      return;
    }
    undockToWorkspace();
    if (m_workspaceDock) {
      m_workspaceDock->setFloating(true);
      m_workspaceDock->show();
      m_workspaceDock->raise();
      m_workspaceDock->activateWindow();
      positionFloatingDock(preferredPos);
      if (auto *mw = findMainWindow()) {
        syncDockPanels(mw);
      }
    }
  }

  void redockPanel() {
    redockFromWorkspace();
  }

  void undockToWorkspace(QMainWindow *mainWindow = nullptr) {
    if (m_workspaceDock || !m_content) return;
    if (!mainWindow) mainWindow = findMainWindow();
    if (!mainWindow) return;

    if (m_contentLayout) {
      m_contentLayout->removeWidget(m_content);
    }

    m_content->setMinimumSize(0, 0);
    m_content->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);

    auto *dock = new StackedPanelDock(this, m_title, mainWindow);
    dock->setMinimumWidth(200);
    dock->setMinimumHeight(50);

    QWidget *dockWidget = m_content;
    if (!qobject_cast<QScrollArea *>(m_content)) {
      auto *scroll = new QScrollArea(dock);
      scroll->setObjectName("dockScroll_" + m_id);
      scroll->setFrameShape(QFrame::NoFrame);
      scroll->setWidgetResizable(true);
      scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
      scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
      scroll->setMinimumWidth(180);
      scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
      scroll->setStyleSheet(
          "QScrollArea { background: #1E1E1E; border: none; } "
          "QScrollBar:vertical { background: #1A1A1A; width: 6px; margin: 0; } "
          "QScrollBar::handle:vertical { background: #383838; border-radius: 3px; min-height: 20px; } "
          "QScrollBar::handle:vertical:hover { background: #555555; } "
          "QScrollBar:horizontal { background: #1A1A1A; height: 6px; margin: 0; } "
          "QScrollBar::handle:horizontal { background: #383838; border-radius: 3px; min-width: 20px; } "
          "QScrollBar::handle:horizontal:hover { background: #555555; } "
          "QScrollBar:add-line, QScrollBar::sub-line { width: 0; height: 0; }");
      scroll->setWidget(m_content);
      scroll->setMouseTracking(true);
      scroll->setAttribute(Qt::WA_Hover, true);
      if (scroll->viewport()) {
        scroll->viewport()->setMouseTracking(true);
        scroll->viewport()->setAttribute(Qt::WA_Hover, true);
      }
      m_dockScroll = scroll;
      dockWidget = scroll;
    }

    dock->setWidget(dockWidget);
    m_content->show();

    if (m_contentContainer) m_contentContainer->hide();
    if (m_resizeHandle) m_resizeHandle->hide();

    // The drawer completely leaves the stack drawer list:
    this->hide();

    m_workspaceDock = dock;
    mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
    mainWindow->resizeDocks({dock}, {260}, Qt::Horizontal);
    dock->show();
    dock->raise();
    dock->syncTitleBar();

    QTimer::singleShot(0, mainWindow, [mainWindow] { syncDockPanels(mainWindow); });
  }

  void redockFromWorkspace() {
    if (!m_workspaceDock) return;
    auto *dock = m_workspaceDock.data();
    m_workspaceDock = nullptr;

    if (m_dockScroll) {
      if (m_dockScroll->widget() == m_content) {
        m_dockScroll->takeWidget();
      }
      m_dockScroll->deleteLater();
      m_dockScroll = nullptr;
    }

    if (dock) {
      if (dock->widget() == m_content) {
        dock->setWidget(nullptr);
      }
      dock->disconnect(this);
      dock->hide();
      dock->deleteLater();
    }

    if (m_content && m_contentContainer && m_contentLayout) {
      m_content->setParent(m_contentContainer);
      m_contentLayout->addWidget(m_content);
      if (m_customHeight > 0) {
        m_content->setFixedHeight(m_customHeight);
      } else {
        m_content->setMinimumSize(0, 0);
        m_content->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
      }
      m_content->setVisible(m_open);
    }

    if (m_contentContainer) {
      m_contentContainer->setVisible(m_open);
    }
    if (m_resizeHandle) {
      m_resizeHandle->setVisible(m_open);
    }
    m_titleLabel->setText(m_title);

    // The drawer returns to the stack drawer list:
    this->show();

    if (auto *mw = findMainWindow()) {
      QTimer::singleShot(0, mw, [mw] { syncDockPanels(mw); });
    }
  }

  void closeDrawer() {
    this->hide();
    setOpen(false, true);
    QSettings settings("Wavy-Flo", "Wavy-Flo");
    settings.setValue("stackedPanels/" + m_id + "/closed", true);
    if (auto *mw = findMainWindow()) {
      QTimer::singleShot(0, mw, [mw] { syncDockPanels(mw); });
    }
  }

  void closeFromWorkspace() {
    if (m_workspaceDock) {
      auto *dock = m_workspaceDock.data();
      m_workspaceDock = nullptr;
      if (m_dockScroll) {
        if (m_dockScroll->widget() == m_content) {
          m_dockScroll->takeWidget();
        }
        m_dockScroll->deleteLater();
        m_dockScroll = nullptr;
      }
      if (dock) {
        dock->hide();
        dock->deleteLater();
      }
    }
    if (m_content && m_contentContainer && m_contentLayout) {
      m_content->setParent(m_contentContainer);
      m_contentLayout->addWidget(m_content);
      m_content->setVisible(false);
    }
    if (m_contentContainer) m_contentContainer->setVisible(false);
    if (m_resizeHandle) m_resizeHandle->setVisible(false);
    m_open = false;
    this->hide();
    QSettings settings("Wavy-Flo", "Wavy-Flo");
    settings.setValue("stackedPanels/" + m_id + "/closed", true);
    if (auto *mw = findMainWindow()) {
      QTimer::singleShot(0, mw, [mw] { syncDockPanels(mw); });
    }
  }

  void closePanel() {
    if (isDockedInWorkspace()) {
      closeFromWorkspace();
    } else {
      closeDrawer();
    }
  }

  bool isPanelClosed() const {
    if (isDockedInWorkspace()) {
      return !m_workspaceDock || !m_workspaceDock->isVisible();
    }
    return isHidden();
  }

  bool eventFilter(QObject *watched, QEvent *event) override {
    if (watched == m_header || watched == m_titleLabel) {
      if (event->type() == QEvent::ContextMenu) {
        showMenu();
        return true;
      }
      if (event->type() == QEvent::MouseButtonDblClick) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
          const QPoint p = me->globalPosition().toPoint();
          const bool onMenu = m_menuBtn && m_menuBtn->rect().contains(m_menuBtn->mapFromGlobal(p));
          const bool onClose = m_closeBtn && m_closeBtn->rect().contains(m_closeBtn->mapFromGlobal(p));
          if (!onMenu && !onClose) {
            toggleOpen();
            return true;
          }
        }
      } else if (event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
          const QPoint p = me->globalPosition().toPoint();
          const bool onMenu = m_menuBtn && m_menuBtn->rect().contains(m_menuBtn->mapFromGlobal(p));
          const bool onClose = m_closeBtn && m_closeBtn->rect().contains(m_closeBtn->mapFromGlobal(p));
          if (!onMenu && !onClose) {
            m_pressPos = me->globalPosition().toPoint();
            m_dragStarted = false;
            m_pressOnWords = onTitleWords(m_titleLabel, m_pressPos);
            m_movedAway = false;
            return true;
          }
        }
      } else if (event->type() == QEvent::MouseMove) {
        auto *me = static_cast<QMouseEvent *>(event);
        if ((me->buttons() & Qt::LeftButton) && !m_pressPos.isNull()) {
          const QPoint curGlobal = me->globalPosition().toPoint();
          const QPoint diff = curGlobal - m_pressPos;
          if (!m_dragStarted && diff.manhattanLength() > 6) {
            if (!m_pressOnWords) {
              m_movedAway = true;
              return true;
            }
            m_dragStarted = true;
            QApplication::setOverrideCursor(Qt::ClosedHandCursor);
          }
          if (m_dragStarted) {
            const QPoint localPos = mapFromGlobal(curGlobal);
            const bool draggedOutOfStack = (localPos.x() < -12 && diff.x() < -20);
            if (draggedOutOfStack && onDragOutOfStack && !isDockedInWorkspace() && !isFloating()) {
              QApplication::restoreOverrideCursor();
              m_pressPos = {};
              m_dragStarted = false;
              onDragOutOfStack(this, curGlobal);
              return true;
            }
            if (onDragMoved) {
              onDragMoved(this, curGlobal);
              return true;
            }
          }
        }
      } else if (event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && !m_pressPos.isNull()) {
          if (m_dragStarted) {
            QApplication::restoreOverrideCursor();
            if (onDragFinished) onDragFinished(this);
          } else if (!m_movedAway) {
            toggleOpen();
          }
          m_pressPos = {};
          m_dragStarted = false;
          m_movedAway = false;
          return true;
        }
      }
    }
    return QWidget::eventFilter(watched, event);
  }



public:
  void showMenu() {
    QMenu menu(this);
    menu.setStyleSheet(
        "QMenu { background: #222222; border: 1px solid #333333; color: #E2E2E2; padding: 4px; font-size: 11px; } "
        "QMenu::item { padding: 4px 18px 4px 18px; border-radius: 2px; } "
        "QMenu::item:selected { background: #2A52BE; color: #FFFFFF; }");

    auto *floatAct = menu.addAction("Undock / Float Panel ↗");
    connect(floatAct, &QAction::triggered, this, [this] { floatPanel(); });

    auto *closeAct = menu.addAction("Close Panel");
    connect(closeAct, &QAction::triggered, this, [this] {
      closePanel();
    });

    menu.exec(QCursor::pos());
  }
};

// Implementasi StackedPanelDock
inline StackedPanelDock::StackedPanelDock(StackedPanelDrawer *drawer, const QString &title, QMainWindow *parent)
    : DockPanel(title, parent), m_drawer(drawer) {
  setObjectName("dock_panel_" + drawer->id());
  setProperty("stackedDrawerId", drawer->id());
  setAllowedAreas(Qt::AllDockWidgetAreas);
  setFeatures(QDockWidget::DockWidgetMovable |
              QDockWidget::DockWidgetFloatable |
              QDockWidget::DockWidgetClosable);
}

inline void StackedPanelDock::closeEvent(QCloseEvent *event) {
  event->accept();
  if (m_drawer) {
    m_drawer->closeFromWorkspace();
  }
}

// Implementasi DrawerResizeHandle
inline DrawerResizeHandle::DrawerResizeHandle(StackedPanelDrawer *drawer)
    : QWidget(drawer), m_drawer(drawer) {
  setFixedHeight(7);
  setCursor(Qt::SizeVerCursor);
  setToolTip("Drag vertically to resize panel height · Double-click to reset");
}

inline void DrawerResizeHandle::paintEvent(QPaintEvent *e) {
  Q_UNUSED(e);
  QPainter p(this);
  p.setPen(Qt::NoPen);
  p.fillRect(rect(), ui_theme::ink("#1C1C1C"));
  p.setBrush(underMouse() || m_dragging ? ui_theme::ink("#3E6AE1") : ui_theme::ink("#4A4A4A"));
  const int cx = width() / 2;
  const int cy = height() / 2;
  p.drawRoundedRect(QRect(cx - 16, cy - 1, 32, 2), 1, 1);
}

inline void DrawerResizeHandle::mousePressEvent(QMouseEvent *e) {
  if (e->button() == Qt::LeftButton && m_drawer && m_drawer->content()) {
    m_dragging = true;
    m_pressPos = e->globalPosition().toPoint();
    m_startHeight = m_drawer->content()->height();
    grabMouse();
    update();
    e->accept();
    return;
  }
  QWidget::mousePressEvent(e);
}

inline void DrawerResizeHandle::mouseMoveEvent(QMouseEvent *e) {
  if (m_dragging && m_drawer && m_drawer->content()) {
    int dy = e->globalPosition().toPoint().y() - m_pressPos.y();
    int minH = qMax(40, m_drawer->defaultMinHeight());
    int newH = qMax(minH, m_startHeight + dy);
    m_drawer->setCustomHeight(newH, false);
    e->accept();
    return;
  }
  QWidget::mouseMoveEvent(e);
}

inline void DrawerResizeHandle::mouseReleaseEvent(QMouseEvent *e) {
  if (m_dragging) {
    m_dragging = false;
    releaseMouse();
    if (m_drawer && m_drawer->customHeight() > 0) {
      m_drawer->setCustomHeight(m_drawer->customHeight(), true);
    }
    update();
    e->accept();
    return;
  }
  QWidget::mouseReleaseEvent(e);
}

inline void DrawerResizeHandle::mouseDoubleClickEvent(QMouseEvent *e) {
  if (m_drawer) {
    m_drawer->resetCustomHeight();
    e->accept();
    return;
  }
  QWidget::mouseDoubleClickEvent(e);
}

// ============================================================================
// StackedPanelGroup: Kontainer kolom vertikal laci berderet ke bawah
// Mendukung drag reorder antar laci dengan visual feedback & menu navigasi
// ============================================================================
class StackedPanelGroup : public QWidget {
  QScrollArea *m_scroll = nullptr;
  QWidget *m_container = nullptr;
  QVBoxLayout *m_layout = nullptr;
  QMap<QString, StackedPanelDrawer *> m_drawers;
  QVector<StackedPanelDrawer *> m_orderedDrawers;

public:
  std::function<void(StackedPanelDrawer *, const QPoint &)> onDragOutOfStack;

  StackedPanelGroup(QWidget *parent = nullptr) : QWidget(parent) {
    setObjectName("stackedPanelGroup");
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_scroll = new QScrollArea(this);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setStyleSheet(
        "QScrollArea { background: #1E1E1E; border: none; } "
        "QScrollBar:vertical { background: #1A1A1A; width: 6px; margin: 0; } "
        "QScrollBar::handle:vertical { background: #383838; border-radius: 3px; min-height: 20px; } "
        "QScrollBar::handle:vertical:hover { background: #555555; } "
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; } "
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }");

    m_container = new QWidget(m_scroll);
    m_container->setObjectName("stackedPanelContainer");
    m_container->setStyleSheet("QWidget#stackedPanelContainer { background: #1E1E1E; }");

    m_layout = new QVBoxLayout(m_container);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    m_scroll->setWidget(m_container);
    rootLayout->addWidget(m_scroll);
  }

  StackedPanelDrawer *addDrawer(const QString &id, const QString &title, QWidget *content,
                                bool defaultOpen = false, int defaultMinHeight = 120, int order = -1) {
    if (m_drawers.contains(id)) {
      return m_drawers[id];
    }

    auto *drawer = new StackedPanelDrawer(id, title, content, defaultOpen, defaultMinHeight, m_container);
    drawer->setDefaultOrder(order >= 0 ? order : m_orderedDrawers.size() + 1);

    drawer->onCloseRequested = [this](StackedPanelDrawer *d) {
      d->setOpen(false);
    };
    drawer->onCloseOthersRequested = [this](StackedPanelDrawer *target) {
      for (auto *d : m_orderedDrawers) {
        if (d != target) d->setOpen(false);
      }
      target->setOpen(true);
    };

    drawer->onMoveUpRequested = [this](StackedPanelDrawer *d) {
      moveDrawerUp(d);
    };
    drawer->onMoveDownRequested = [this](StackedPanelDrawer *d) {
      moveDrawerDown(d);
    };
    drawer->onResetOrderRequested = [this] {
      resetDefaultOrder();
    };

    drawer->onDragMoved = [this](StackedPanelDrawer *dragged, const QPoint &globalPos) {
      handleDrawerDrag(dragged, globalPos);
    };
    drawer->onDragFinished = [this](StackedPanelDrawer *dragged) {
      Q_UNUSED(dragged);
    };
    drawer->onDragOutOfStack = [this](StackedPanelDrawer *dragged, const QPoint &globalPos) {
      if (onDragOutOfStack) onDragOutOfStack(dragged, globalPos);
    };

    m_drawers[id] = drawer;
    m_orderedDrawers.append(drawer);

    rebuildLayout();
    return drawer;
  }

  StackedPanelDrawer *drawer(const QString &id) const {
    return m_drawers.value(id, nullptr);
  }

  bool isDrawerOpen(const QString &id) const {
    if (auto *d = drawer(id)) return d->isOpen();
    return false;
  }

  void setDrawerOpen(const QString &id, bool open) {
    if (auto *d = drawer(id)) {
      d->setOpen(open);
      if (open) {
        m_scroll->ensureWidgetVisible(d);
      }
    }
  }

  void openAndFocus(const QString &id) {
    if (auto *d = drawer(id)) {
      if (d->isDockedInWorkspace() && d->workspaceDock()) {
        d->workspaceDock()->show();
        d->workspaceDock()->raise();
      } else {
        if (d->isHidden()) {
          d->show();
        }
        setDrawerOpen(id, true);
        if (m_scroll) m_scroll->ensureWidgetVisible(d);
      }
      QSettings settings("Wavy-Flo", "Wavy-Flo");
      settings.setValue("stackedPanels/" + id + "/closed", false);
      if (auto *mw = d->findMainWindow()) {
        syncDockPanels(mw);
      }
      if (d->content()) {
        d->content()->setFocus();
      }
    }
  }

  const QVector<StackedPanelDrawer *> &drawers() const {
    return m_orderedDrawers;
  }

  void moveDrawerUp(StackedPanelDrawer *d) {
    int idx = m_orderedDrawers.indexOf(d);
    if (idx > 0) {
      m_orderedDrawers.move(idx, idx - 1);
      rebuildLayout();
      saveOrder();
    }
  }

  void moveDrawerDown(StackedPanelDrawer *d) {
    int idx = m_orderedDrawers.indexOf(d);
    if (idx >= 0 && idx < m_orderedDrawers.size() - 1) {
      m_orderedDrawers.move(idx, idx + 1);
      rebuildLayout();
      saveOrder();
    }
  }

  void resetDefaultOrder() {
    QSettings settings("Wavy-Flo", "Wavy-Flo");
    settings.remove("stackedPanels/order");
    std::sort(m_orderedDrawers.begin(), m_orderedDrawers.end(), [](StackedPanelDrawer *a, StackedPanelDrawer *b) {
      return a->defaultOrder() < b->defaultOrder();
    });
    rebuildLayout();
  }

  void restoreOrder() {
    QSettings settings("Wavy-Flo", "Wavy-Flo");
    QStringList savedOrder = settings.value("stackedPanels/order").toStringList();
    if (savedOrder.isEmpty()) return;

    QVector<StackedPanelDrawer *> newOrder;
    for (const QString &id : savedOrder) {
      if (auto *d = m_drawers.value(id, nullptr)) {
        newOrder.append(d);
      }
    }
    for (auto *d : m_orderedDrawers) {
      if (!newOrder.contains(d)) {
        newOrder.append(d);
      }
    }
    m_orderedDrawers = newOrder;
    saveOrder();
    rebuildLayout();
  }

  void applyUiScale(double scale) {
    for (auto *d : m_drawers) {
      if (d) d->applyUiScale(scale);
    }
    update();
  }

private:
  void handleDrawerDrag(StackedPanelDrawer *dragged, const QPoint &globalPos) {
    const QPoint localPos = m_container->mapFromGlobal(globalPos);
    int fromIdx = m_orderedDrawers.indexOf(dragged);
    if (fromIdx < 0) return;

    // After Effects style: Dragging header out of the stack column into workspace docks it
    if (localPos.x() < 0 && !dragged->isDockedInWorkspace() && !dragged->isFloating()) {
      if (onDragOutOfStack) {
        onDragOutOfStack(dragged, globalPos);
        return;
      }
      QApplication::restoreOverrideCursor();
      dragged->undockToWorkspace(dragged->findMainWindow());
      if (auto *dock = dragged->workspaceDock()) {
        dock->setFloating(true);
        dock->move(globalPos - QPoint(60, 14));
      }
      return;
    }

    for (int i = 0; i < m_orderedDrawers.size(); ++i) {
      if (i == fromIdx) continue;
      auto *d = m_orderedDrawers[i];
      int headerTop = d->geometry().top();
      int headerBottom = headerTop + 26;
      if (localPos.y() >= headerTop - 5 && localPos.y() <= headerBottom + 5) {
        m_orderedDrawers.move(fromIdx, i);
        rebuildLayout();
        saveOrder();
        break;
      }
    }
  }

  void saveOrder() {
    QStringList order;
    for (auto *d : m_orderedDrawers) order << d->id();
    QSettings("Wavy-Flo", "Wavy-Flo").setValue("stackedPanels/order", order);
  }

public:
  int drawersInStackCount() const {
    int count = 0;
    for (auto *d : m_orderedDrawers) {
      if (d && !d->isDockedInWorkspace() && !d->isFloating() && !d->isHidden()) {
        count++;
      }
    }
    return count;
  }

  void updateEmptyState() {
    const bool empty = (drawersInStackCount() == 0);
    if (m_scroll) m_scroll->setVisible(!empty);
    setToolTip(empty ? "Panel Stack (Collapsed) · Drag panel here to redock" : "");
    update();
  }

  void rebuildLayout() {
    while (QLayoutItem *item = m_layout->takeAt(0)) {
      delete item;
    }
    for (auto *d : m_orderedDrawers) {
      m_layout->addWidget(d);
    }
    m_layout->addStretch(1);
    updateEmptyState();
  }

protected:
  void paintEvent(QPaintEvent *e) override {
    QWidget::paintEvent(e);
    if (drawersInStackCount() == 0) {
      QPainter p(this);
      p.fillRect(rect(), ui_theme::ink("#161616"));
      p.setPen(ui_theme::ink("#2C2C2C"));
      p.drawLine(0, 0, 0, height());
      p.setPen(ui_theme::ink("#777777"));
      QFont f = font();
      f.setBold(true);
      if (width() <= 40) {
        f.setPixelSize(9);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 3);
        p.setFont(f);
        p.save();
        p.translate(width() / 2 + 3, qMax(20, height() / 2 - 30));
        p.rotate(90);
        p.drawText(0, 0, "STACK");
        p.restore();
      } else {
        f.setPixelSize(11);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 0);
        p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, "Drop panel here to dock into Stack");
      }
    }
  }
};

// ============================================================================
// ExtensionPanelManager: Arsitektur Ekstensibel Pendaftaran Panel Baru & Pihak Ketiga
// ============================================================================
class ExtensionPanelManager {
  StackedPanelGroup *m_group = nullptr;

public:
  explicit ExtensionPanelManager(StackedPanelGroup *group) : m_group(group) {}

  StackedPanelDrawer *registerPanel(const QString &id, const QString &title,
                                    QWidget *widget, bool defaultOpen = false, int minHeight = 120, int order = 100) {
    if (!m_group) return nullptr;
    return m_group->addDrawer(id, title, widget, defaultOpen, minHeight, order);
  }

  void scanExtensions(const QString &appDir) {
    if (!m_group) return;

    // Not Scripts/ScriptUI Panels: Window > Script Panels runs those. A drawer
    // here only showed the file's name, an empty panel in every stack.
    QStringList searchPaths = {
        appDir + "/extensions",
        appDir + "/plugins/ui"
    };

    for (const QString &dirPath : searchPaths) {
      QDir dir(dirPath);
      if (!dir.exists()) continue;

      const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
      for (const auto &info : entries) {
        QString extName = info.baseName();
        if (info.suffix().compare("jsx", Qt::CaseInsensitive) == 0 ||
            info.suffix().compare("js", Qt::CaseInsensitive) == 0 ||
            info.isDir()) {
          auto *panelRoot = new QWidget;
          panelRoot->setStyleSheet("background: #1A1A1A; border: 1px dashed #333333; margin: 4px;");
          auto *pLayout = new QVBoxLayout(panelRoot);
          pLayout->setContentsMargins(8, 8, 8, 8);
          auto *lbl = new QLabel("Third-Party Extension:\n" + info.fileName());
          lbl->setStyleSheet("color: #A0A0A0; font-size: 11px;");
          pLayout->addWidget(lbl);

          registerPanel("ext_" + extName.toLower(), extName, panelRoot, false, 120);
        }
      }
    }
  }
};
