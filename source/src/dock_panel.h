#pragma once

#include <QDockWidget>
#include <QMainWindow>
#include <QWidget>
#include <QLabel>
#include <QToolButton>
#include <QHBoxLayout>
#include <QPainter>
#include <QPointer>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QHoverEvent>
#include <QApplication>
#include <QScreen>
#include <QGuiApplication>
#include <algorithm>
#include "ui_theme.h"

// Where a panel is taken hold of to move, dock or float it: the words of its
// title, with a little room round them. Not the empty bar beside them -- a
// press there meant for something else kept pulling panels out of place and
// spoiling the workspace. The same for every panel and every stacked drawer.
inline bool onTitleWords(const QLabel *label, const QPoint &global) {
  if (!label || !label->isVisible() || label->text().isEmpty()) return false;
  const int words = std::min(label->width(), label->fontMetrics().horizontalAdvance(label->text()) + 12);
  int left = 0;
  if (label->alignment() & Qt::AlignHCenter) left = (label->width() - words) / 2;
  else if (label->alignment() & Qt::AlignRight) left = label->width() - words;
  return QRect(left, 0, words, label->height()).adjusted(-4, -4, 4, 4).contains(label->mapFromGlobal(global));
}

// The empty part of a panel's title bar: pressed, it takes the press so the
// dock under it never starts a move of its own; it moves nothing itself.
class TitleSpacer : public QWidget {
public:
  TitleSpacer(QWidget *parent = nullptr) : QWidget(parent) {
    setMinimumWidth(0);
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
  }
  QSize sizeHint() const override { return QSize(0, 0); }
  QSize minimumSizeHint() const override { return QSize(0, 0); }
protected:
  void mousePressEvent(QMouseEvent *e) override {
    if (e->button() == Qt::LeftButton) e->accept();
    else QWidget::mousePressEvent(e);
  }
  void mouseMoveEvent(QMouseEvent *e) override { QWidget::mouseMoveEvent(e); }
  void mouseDoubleClickEvent(QMouseEvent *e) override {
    if (e->button() == Qt::LeftButton) e->accept();
    else QWidget::mouseDoubleClickEvent(e);
  }
};

class CollapsedDockTitle : public QWidget {
public:
  CollapsedDockTitle(QWidget *parent = nullptr) : QWidget(parent) {
    setObjectName("collapsedDockTitle");
    setFixedHeight(0);
    setMinimumSize(0, 0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  }
  QSize sizeHint() const override { return QSize(0, 0); }
  QSize minimumSizeHint() const override { return QSize(0, 0); }
};

class CustomDockTitle : public QWidget {
  int titleHeight = 28;
public:
  CustomDockTitle(QDockWidget *dock) : QWidget(dock) {
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setObjectName("customDockTitle");
    setFixedHeight(28);
    setMinimumWidth(0);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 6, 0);
    layout->setSpacing(4);

    auto *label = new QLabel(dock->windowTitle(), this);
    label->setObjectName("customDockTitleLabel");
    label->setStyleSheet("color: #C2C6D4; font-size: 11px; font-weight: 600; background: transparent;");
    label->setProperty("dockHandle", true); // what the panel is moved by (dockPressAllowed)
    label->setMinimumWidth(0);
    layout->addWidget(label);

    auto *spacer = new TitleSpacer(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    spacer->setMinimumWidth(0);
    layout->addWidget(spacer);

    auto *floatBtn = new QToolButton(this);
    floatBtn->setObjectName("floatTimeline");
    floatBtn->setText("\u2197");
    floatBtn->setFixedSize(18, 18);
    floatBtn->setStyleSheet("QToolButton { padding: 0; border: 0; background: transparent; color: #8A8A8A; font-size: 11px; border-radius: 3px; } QToolButton:hover { color: #2D8CFF; background: #2A2A2A; }");
    QObject::connect(floatBtn, &QToolButton::clicked, dock, [dock, floatBtn] {
      const bool willFloat = !dock->isFloating();
      const QPoint btnPos = floatBtn->mapToGlobal(QPoint(0, 0));
      dock->setFloating(willFloat);
      if (willFloat) {
        dock->show();
        dock->raise();
        const QSize targetSize = dock->sizeHint().expandedTo(QSize(280, 360));
        dock->resize(targetSize);
        int targetX = btnPos.x() - targetSize.width() + 40;
        int targetY = btnPos.y() + 20;
        QScreen *screen = QGuiApplication::screenAt(btnPos);
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) {
          const QRect avail = screen->availableGeometry();
          targetX = qBound(avail.left() + 20, targetX, avail.right() - targetSize.width() - 20);
          targetY = qBound(avail.top() + 30, targetY, avail.bottom() - targetSize.height() - 20);
        }
        dock->move(targetX, targetY);
      }
    });
    layout->addWidget(floatBtn);

    auto *closeBtn = new QToolButton(this);
    closeBtn->setObjectName("closeTimeline");
    closeBtn->setText("\u00d7");
    closeBtn->setFixedSize(18, 18);
    closeBtn->setStyleSheet("QToolButton { padding: 0; border: 0; background: transparent; color: #8A8A8A; font-size: 13px; border-radius: 3px; } QToolButton:hover { color: #FF5252; background: #2A2A2A; }");
    QObject::connect(closeBtn, &QToolButton::clicked, dock, &QDockWidget::close);
    layout->addWidget(closeBtn);
  }

  void applyUiScale(double scale) {
    titleHeight = std::max(18, (int)std::round(28.0 * scale));
    setFixedHeight(titleHeight);
    int titleFontSize = std::max(8, (int)std::round(11.0 * scale));
    if (auto *label = findChild<QLabel *>("customDockTitleLabel")) {
      label->setStyleSheet(QString("color: #C2C6D4; font-size: %1px; font-weight: 600; background: transparent;").arg(titleFontSize));
    }
    int btnSz = std::max(14, (int)std::round(18.0 * scale));
    if (auto *floatBtn = findChild<QToolButton *>("floatTimeline")) {
      floatBtn->setFixedSize(btnSz, btnSz);
      floatBtn->setStyleSheet(QString("QToolButton { padding: 0; border: 0; background: transparent; color: #8A8A8A; font-size: %1px; border-radius: 3px; } QToolButton:hover { color: #2D8CFF; background: #2A2A2A; }").arg(titleFontSize));
    }
    if (auto *closeBtn = findChild<QToolButton *>("closeTimeline")) {
      closeBtn->setFixedSize(btnSz, btnSz);
      int closeFontSize = std::max(9, (int)std::round(13.0 * scale));
      closeBtn->setStyleSheet(QString("QToolButton { padding: 0; border: 0; background: transparent; color: #8A8A8A; font-size: %1px; border-radius: 3px; } QToolButton:hover { color: #FF5252; background: #2A2A2A; }").arg(closeFontSize));
    }
    update();
  }

  QSize sizeHint() const override { return QSize(0, titleHeight); }
  QSize minimumSizeHint() const override { return QSize(0, titleHeight); }

protected:
  void mousePressEvent(QMouseEvent *e) override {
    if (e->button() == Qt::LeftButton) e->accept();
    else QWidget::mousePressEvent(e);
  }
  // A double click floats the panel, or docks it back -- on its title's words.
  void mouseDoubleClickEvent(QMouseEvent *e) override {
    if (e->button() == Qt::LeftButton) {
      auto *dock = qobject_cast<QDockWidget *>(parentWidget());
      if (dock && onTitleWords(findChild<QLabel *>("customDockTitleLabel"), e->globalPosition().toPoint()))
        dock->setFloating(!dock->isFloating());
      e->accept();
      return;
    }
    QWidget::mouseDoubleClickEvent(e);
  }
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.fillRect(rect(), ui_theme::ink("#1E1E1E"));
    p.setPen(ui_theme::ink("#282828"));
    p.drawLine(0, height() - 1, width(), height() - 1);
  }
};

// ============================================================================
// FloatingDockResizeFilter: After Effects style edge/corner resizing for floating docks
// ============================================================================
class FloatingDockResizeFilter : public QObject {
public:
  enum Edge {
    EdgeNone   = 0,
    EdgeLeft   = 1 << 0,
    EdgeRight  = 1 << 1,
    EdgeTop    = 1 << 2,
    EdgeBottom = 1 << 3
  };

private:
  QPointer<QDockWidget> m_resizingDock;
  int m_activeEdges = EdgeNone;
  QPoint m_pressPos;
  QRect m_startGeometry;
  bool m_isResizing = false;
  bool m_overrideCursorActive = false;
  Qt::CursorShape m_currentCursorShape = Qt::ArrowCursor;

public:
  explicit FloatingDockResizeFilter(QObject *parent = nullptr) : QObject(parent) {
    setObjectName("floatingDockResizeFilter");
    if (qApp) {
      qApp->installEventFilter(this);
    }
  }

  static FloatingDockResizeFilter *instance() {
    static FloatingDockResizeFilter *s_instance = nullptr;
    if (!s_instance && qApp) {
      s_instance = new FloatingDockResizeFilter(qApp);
    }
    return s_instance;
  }

  static void ensureInstalled() {
    instance();
  }

  static Qt::CursorShape cursorForEdges(int edges) {
    if ((edges & EdgeLeft && edges & EdgeTop) || (edges & EdgeRight && edges & EdgeBottom))
      return Qt::SizeFDiagCursor;
    if ((edges & EdgeRight && edges & EdgeTop) || (edges & EdgeLeft && edges & EdgeBottom))
      return Qt::SizeBDiagCursor;
    if (edges & EdgeLeft || edges & EdgeRight)
      return Qt::SizeHorCursor;
    if (edges & EdgeTop || edges & EdgeBottom)
      return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
  }

  static QDockWidget *findFloatingDock(QWidget *w) {
    for (QWidget *p = w; p; p = p->parentWidget()) {
      if (auto *dock = qobject_cast<QDockWidget *>(p)) {
        if (dock->isFloating() && dock->isVisible()) return dock;
      }
    }
    return nullptr;
  }

  int getEdges(QDockWidget *dock, const QPoint &mouseGlobal) const {
    if (!dock || !dock->isFloating() || !dock->isVisible()) return EdgeNone;

    const QRect dockRect(dock->mapToGlobal(QPoint(0, 0)), dock->size());
    int margin = 7;
    if (QGuiApplication::primaryScreen()) {
      margin = qMax(6, (int)std::round(6.0 * QGuiApplication::primaryScreen()->logicalDotsPerInch() / 96.0));
    }

    if (!dockRect.adjusted(-margin, -margin, margin, margin).contains(mouseGlobal)) {
      return EdgeNone;
    }

    const bool onLeft   = std::abs(mouseGlobal.x() - dockRect.left()) <= margin;
    const bool onRight  = std::abs(mouseGlobal.x() - dockRect.right()) <= margin;
    const bool onTop    = std::abs(mouseGlobal.y() - dockRect.top()) <= margin;
    const bool onBottom = std::abs(mouseGlobal.y() - dockRect.bottom()) <= margin;

    int edges = EdgeNone;
    if (onLeft) edges |= EdgeLeft;
    else if (onRight) edges |= EdgeRight;

    if (onTop) edges |= EdgeTop;
    else if (onBottom) edges |= EdgeBottom;

    if (edges != EdgeNone) {
      for (const char *btnName : {"closeTimeline", "floatTimeline", "drawerCloseBtn", "drawerMenuBtn"}) {
        if (auto *btn = dock->findChild<QToolButton *>(btnName)) {
          if (btn->isVisible() && btn->rect().contains(btn->mapFromGlobal(mouseGlobal))) {
            return EdgeNone;
          }
        }
      }
    }
    return edges;
  }

  void updateCursor(int edges) {
    if (edges != EdgeNone) {
      Qt::CursorShape desired = cursorForEdges(edges);
      if (!m_overrideCursorActive || m_currentCursorShape != desired) {
        if (m_overrideCursorActive) QApplication::restoreOverrideCursor();
        QApplication::setOverrideCursor(desired);
        m_overrideCursorActive = true;
        m_currentCursorShape = desired;
      }
    } else {
      if (m_overrideCursorActive && !m_isResizing) {
        QApplication::restoreOverrideCursor();
        m_overrideCursorActive = false;
        m_currentCursorShape = Qt::ArrowCursor;
      }
    }
  }

  void cancelResize() {
    if (m_isResizing) {
      m_isResizing = false;
      if (m_resizingDock) {
        m_resizingDock->setGeometry(m_startGeometry);
      }
      m_resizingDock = nullptr;
      m_activeEdges = EdgeNone;
    }
    if (m_overrideCursorActive) {
      QApplication::restoreOverrideCursor();
      m_overrideCursorActive = false;
      m_currentCursorShape = Qt::ArrowCursor;
    }
  }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() == QEvent::KeyPress) {
      auto *ke = static_cast<QKeyEvent *>(event);
      if (ke->key() == Qt::Key_Escape && m_isResizing) {
        cancelResize();
        return true;
      }
    }

    if (event->type() == QEvent::ApplicationDeactivate) {
      if (m_isResizing) {
        cancelResize();
      }
    }

    if (m_isResizing) {
      if (!m_resizingDock || !m_resizingDock->isVisible() || !m_resizingDock->isFloating()) {
        cancelResize();
        return false;
      }

      if (event->type() == QEvent::MouseMove) {
        auto *me = static_cast<QMouseEvent *>(event);
        const QPoint curPos = me->globalPosition().toPoint();
        const QPoint delta = curPos - m_pressPos;

        QRect newGeom = m_startGeometry;
        QSize minSize = m_resizingDock->minimumSizeHint().expandedTo(QSize(180, 60));
        if (m_resizingDock->minimumWidth() > 0)
          minSize.setWidth(qMax(minSize.width(), m_resizingDock->minimumWidth()));
        if (m_resizingDock->minimumHeight() > 0)
          minSize.setHeight(qMax(minSize.height(), m_resizingDock->minimumHeight()));

        if (m_activeEdges & EdgeRight) {
          int newW = qMax(minSize.width(), m_startGeometry.width() + delta.x());
          newGeom.setWidth(newW);
        }
        if (m_activeEdges & EdgeBottom) {
          int newH = qMax(minSize.height(), m_startGeometry.height() + delta.y());
          newGeom.setHeight(newH);
        }
        if (m_activeEdges & EdgeLeft) {
          int newW = m_startGeometry.width() - delta.x();
          if (newW >= minSize.width()) {
            newGeom.setLeft(m_startGeometry.left() + delta.x());
          } else {
            newGeom.setLeft(m_startGeometry.right() - minSize.width() + 1);
          }
        }
        if (m_activeEdges & EdgeTop) {
          int newH = m_startGeometry.height() - delta.y();
          if (newH >= minSize.height()) {
            newGeom.setTop(m_startGeometry.top() + delta.y());
          } else {
            newGeom.setTop(m_startGeometry.bottom() - minSize.height() + 1);
          }
        }

        m_resizingDock->setGeometry(newGeom);
        return true;
      }

      if (event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
          m_isResizing = false;
          m_resizingDock = nullptr;
          m_activeEdges = EdgeNone;
          if (m_overrideCursorActive) {
            QApplication::restoreOverrideCursor();
            m_overrideCursorActive = false;
            m_currentCursorShape = Qt::ArrowCursor;
          }
          return true;
        }
      }
      return false;
    }

    auto *w = qobject_cast<QWidget *>(watched);
    if (!w) return false;

    if (event->type() == QEvent::MouseMove || event->type() == QEvent::HoverMove) {
      QPoint mouseGlobal;
      if (event->type() == QEvent::MouseMove) {
        mouseGlobal = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
      } else {
        mouseGlobal = w->mapToGlobal(static_cast<QHoverEvent *>(event)->position().toPoint());
      }

      auto *dock = findFloatingDock(w);
      if (dock) {
        int edges = getEdges(dock, mouseGlobal);
        updateCursor(edges);
      } else {
        if (m_overrideCursorActive && !m_isResizing) {
          QApplication::restoreOverrideCursor();
          m_overrideCursorActive = false;
          m_currentCursorShape = Qt::ArrowCursor;
        }
      }
    }

    if (event->type() == QEvent::MouseButtonPress) {
      auto *me = static_cast<QMouseEvent *>(event);
      if (me->button() == Qt::LeftButton) {
        const QPoint mouseGlobal = me->globalPosition().toPoint();
        auto *dock = findFloatingDock(w);
        if (dock) {
          int edges = getEdges(dock, mouseGlobal);
          if (edges != EdgeNone) {
            m_isResizing = true;
            m_resizingDock = dock;
            m_activeEdges = edges;
            m_pressPos = mouseGlobal;
            m_startGeometry = dock->geometry();
            return true;
          }
        }
      }
    }

    if (event->type() == QEvent::Leave) {
      if (m_overrideCursorActive && !m_isResizing) {
        const QPoint mouseGlobal = QCursor::pos();
        auto *dock = findFloatingDock(w);
        if (!dock || getEdges(dock, mouseGlobal) == EdgeNone) {
          QApplication::restoreOverrideCursor();
          m_overrideCursorActive = false;
          m_currentCursorShape = Qt::ArrowCursor;
        }
      }
    }

    return false;
  }
};

// A dock tab already supplies the panel name and is the handle used to move it.
// Collapse the duplicate native title row while the dock belongs to a tab
// stack. A floating or standalone panel gets Qt's native title bar back.
class DockPanel : public QDockWidget {
  QPointer<CollapsedDockTitle> collapsedTitle;
  QPointer<CustomDockTitle> customTitle;
public:
  DockPanel(const QString &title, QWidget *parent = nullptr) : QDockWidget(title, parent) {
    setMinimumSize(0, 0);
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    FloatingDockResizeFilter::ensureInstalled();
    setupDockResize();
  }
  DockPanel(QWidget *parent = nullptr) : QDockWidget(parent) {
    setMinimumSize(0, 0);
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    FloatingDockResizeFilter::ensureInstalled();
    setupDockResize();
  }

  void setupDockResize() {
    connect(this, &QDockWidget::topLevelChanged, this, [this](bool floating) {
      if (floating) {
        updateHoverTracking();
      }
    });
  }

  void enableHoverRecursively(QWidget *w) {
    if (!w) return;
    w->setMouseTracking(true);
    w->setAttribute(Qt::WA_Hover, true);
  }

  void updateHoverTracking() {
    enableHoverRecursively(this);
    enableHoverRecursively(widget());
    enableHoverRecursively(titleBarWidget());
  }

protected:
  void showEvent(QShowEvent *event) override {
    QDockWidget::showEvent(event);
    if (isFloating()) {
      updateHoverTracking();
    }
  }
  void changeEvent(QEvent *event) override {
    QDockWidget::changeEvent(event);
    if (event->type() == QEvent::ParentChange && isFloating()) {
      updateHoverTracking();
    }
  }
public:
  QSize minimumSizeHint() const override {
    if (isFloating()) return QSize(180, 60);
    if (objectName() == "Timeline / Graph") return QSize(200, 120);
    if (objectName() == "Composition") return QSize(240, 160);
    if (objectName() == "Panels" && maximumWidth() <= 32) return QSize(24, 60);
    auto *mainWindow = qobject_cast<const QMainWindow *>(parent());
    if (mainWindow) {
      auto area = mainWindow->dockWidgetArea(const_cast<DockPanel *>(this));
      if (area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea) {
        return QSize(200, 50);
      }
    }
    return QSize(180, 50);
  }

  QSize sizeHint() const override {
    if (objectName() == "Timeline / Graph") return QSize(800, 280);
    if (objectName() == "Composition") return QSize(960, 540);
    if (objectName() == "Panels") return (maximumWidth() <= 32 ? QSize(24, 600) : QSize(280, 600));
    if (objectName() == "Project") return QSize(220, 600);
    if (objectName() == "Properties" || objectName().contains("properties")) return QSize(260, 400);
    return QSize(240, 360);
  }

  bool sharesTab() const {
    if (isFloating()) return false;
    auto *mainWindow = qobject_cast<const QMainWindow *>(parent());
    return mainWindow && !mainWindow->tabifiedDockWidgets(const_cast<DockPanel *>(this)).isEmpty();
  }
  bool usesCollapsedTitle() const {
    return collapsedTitle && titleBarWidget() == collapsedTitle;
  }
  void syncTitleBar() {
    if (objectName() == "Panels") {
      if (!collapsedTitle) {
        collapsedTitle = new CollapsedDockTitle(this);
      }
      if (titleBarWidget() != collapsedTitle) setTitleBarWidget(collapsedTitle);
      return;
    }
    auto *current = titleBarWidget();
    // Timeline has its own useful title/buttons. Never replace custom chrome.
    if (current && current != collapsedTitle && current != customTitle) return;
    if (sharesTab()) {
      if (!collapsedTitle) {
        collapsedTitle = new CollapsedDockTitle(this);
      }
      if (current != collapsedTitle) setTitleBarWidget(collapsedTitle);
    } else {
      if (!customTitle) {
        customTitle = new CustomDockTitle(this);
      }
      if (current != customTitle) setTitleBarWidget(customTitle);
    }
  }
};
