#pragma once
// Wavy-Flo's effect controls: the pieces every effect's settings are built
// from, so every effect -- built in, from a .wfx pack, an older plugin, the
// text animator -- reads the same way.
//
// A setting is one line: its name, its value, and the key diamond when it can
// be animated. Every value starts at the same place, flush left in its own
// column -- a number, a list, a switch and a colour alike -- and is plain text
// until the pointer is over it. Settings that belong together fold under a
// heading. What an effect declares (sdk/wavy_effect.h: kinds, groups, help,
// dimming) decides what the lines are; this decides how they look.
#include <QAbstractButton>
#include <QEvent>
#include <QPointer>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QListView>
#include <QStyledItemDelegate>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGridLayout>
#include <QPainterPath>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPolygonF>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStyle>
#include <QStyleOption>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <vector>

#include <cmath>
#include <functional>
#include <QMouseEvent>

#include "ui_theme.h"

namespace fxui {
// The kit's colours: After Effects dark charcoal palette with signature blue hot-text.
// Each in the look that is on (ui_theme): these are Dark's.
inline QColor valueInk() { return ui_theme::ink("#2D8CFF"); }
inline QColor nameInk() { return ui_theme::ink("#D0D0D0"); }
inline QColor dimInk() { return ui_theme::ink("#757575"); }
inline QColor hoverFill() { return ui_theme::ink("#2A2A2A"); }
// The hairline under every line, and the guide down the left of a group
// tying its lines to its heading (matching timeline rows: #141416).
inline QColor ruleInk() { return ui_theme::ink("#141416"); }
inline QColor guideInk() { return ui_theme::ink("#282828"); }
inline QFont lineFont(QFont base) {
  base.setPixelSize(11);
  return base;
}
// A chevron: After Effects solid disclosure twirl triangle.
inline void chevron(QPainter &p, QPointF c, bool down, const QColor &ink) {
  p.setPen(Qt::NoPen);
  p.setBrush(ink);
  if (down) {
    p.drawPolygon(QPolygonF{c + QPointF(-3.5, -2.0), c + QPointF(3.5, -2.0), c + QPointF(0, 2.5)});
  } else {
    p.drawPolygon(QPolygonF{c + QPointF(-2.0, -3.5), c + QPointF(2.5, 0), c + QPointF(-2.0, 3.5)});
  }
}
}  // namespace fxui

// After Effects style rotary angle dial: circular knob with needle pointer.
class WavyAngleDial : public QWidget {
public:
  double value = 0.0;
  std::function<void(double)> changed;
  std::function<void()> scrubStarted;
  std::function<void()> scrubEnded;

  explicit WavyAngleDial(QWidget *parent = nullptr) : QWidget(parent) {
    setFixedSize(20, 20);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
  }

  void setValue(double deg) {
    if (std::abs(value - deg) > 1e-4) {
      value = deg;
      update();
    }
  }

protected:
  bool dragging = false;
  double startMouseAngle = 0;
  double startValue = 0;

  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QPointF c(width() / 2.0, height() / 2.0);
    const double r = 8.5;

    // Dial background & rim
    const QColor rimColor = underMouse() || dragging ? ui_theme::ink("#505050") : ui_theme::ink("#383838");
    p.setPen(QPen(rimColor, 1.2));
    p.setBrush(ui_theme::ink("#181818"));
    p.drawEllipse(c, r, r);

    // 4 quadrant tick marks (0° North, 90° East, 180° South, 270° West)
    p.setPen(QPen(ui_theme::ink("#444444"), 1.0));
    p.drawLine(c + QPointF(0, -r + 1), c + QPointF(0, -r + 2.6));
    p.drawLine(c + QPointF(r - 1, 0), c + QPointF(r - 2.6, 0));
    p.drawLine(c + QPointF(0, r - 1), c + QPointF(0, r - 2.6));
    p.drawLine(c + QPointF(-r + 1, 0), c + QPointF(-r + 2.6, 0));

    // Pointer needle
    const double rad = (value - 90.0) * 3.14159265358979323846 / 180.0;
    const QPointF tip = c + QPointF(std::cos(rad) * (r - 1.2), std::sin(rad) * (r - 1.2));
    const QColor needleColor = isEnabled() ? (underMouse() || dragging ? ui_theme::ink("#5DADE2") : ui_theme::ink("#2D8CFF")) : ui_theme::ink("#505050");

    p.setPen(QPen(needleColor, 1.4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(c, tip);

    p.setPen(Qt::NoPen);
    p.setBrush(needleColor);
    p.drawEllipse(tip, 1.1, 1.1);
    p.drawEllipse(c, 1.5, 1.5);
  }

  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton) {
      dragging = true;
      const QPointF diff = event->position() - QPointF(width() / 2.0, height() / 2.0);
      startMouseAngle = std::atan2(diff.y(), diff.x()) * 180.0 / 3.14159265358979323846;
      startValue = value;
      if (scrubStarted) scrubStarted();
      event->accept();
    }
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    if (dragging) {
      const QPointF diff = event->position() - QPointF(width() / 2.0, height() / 2.0);
      double currentMouseAngle = std::atan2(diff.y(), diff.x()) * 180.0 / 3.14159265358979323846;
      double delta = currentMouseAngle - startMouseAngle;
      while (delta > 180.0) delta -= 360.0;
      while (delta < -180.0) delta += 360.0;
      startMouseAngle = currentMouseAngle;
      startValue += delta;

      double newVal = startValue;
      if (event->modifiers() & Qt::ShiftModifier) {
        newVal = std::round(newVal / 45.0) * 45.0;
      }
      setValue(newVal);
      if (changed) changed(newVal);
      event->accept();
    }
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (dragging) {
      dragging = false;
      if (scrubEnded) scrubEnded();
      event->accept();
    }
  }
};

// After Effects style stopwatch icon: an outline while the value is constant,
// glowing AE blue with watch hands when it follows keyframes.
class FxKey : public QToolButton {
public:
  explicit FxKey(QWidget *parent = nullptr) : QToolButton(parent) {
    setCheckable(true);
    setFixedSize(16, 16);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::NoFocus);
  }
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool on = isChecked();
    const QColor ink = !isEnabled() ? ui_theme::ink("#444444") : on ? ui_theme::ink("#2D8CFF")
                       : underMouse() ? ui_theme::ink("#E0E0E0") : ui_theme::ink("#707070");
    const QPointF c(8.0, 9.5);
    const double r = 5.0;

    // Top pusher/stem
    p.setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::FlatCap));
    p.drawLine(QPointF(6.2, 2.2), QPointF(9.8, 2.2));
    p.drawLine(QPointF(8.0, 2.2), QPointF(8.0, 4.2));
    // Angled lap button (classic stopwatch silhouette)
    p.drawLine(QPointF(11.8, 4.2), QPointF(13.0, 3.0));

    // Circular dial body
    p.setPen(QPen(ink, 1.25));
    p.setBrush(on ? ui_theme::ink(52, 152, 219, 35) : QColor(Qt::transparent));
    p.drawEllipse(c, r, r);

    // Watch hands
    if (on) {
      p.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(c, c + QPointF(0, -3.2));
      p.drawLine(c, c + QPointF(2.4, 0.4));
      p.setPen(Qt::NoPen);
      p.setBrush(ink);
      p.drawEllipse(c, 1.2, 1.2);
    } else {
      p.setPen(QPen(ink, 1.1, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(c, c + QPointF(0, -2.6));
      p.drawLine(c, c + QPointF(1.8, 0.0));
      p.drawPoint(c);
    }
  }
};

// The buttons at the front of a heading -- an effect's, a Text Animation's --
// drawn rather than taken from a font, an icon or a style, so they read the
// same everywhere: the fold, the eye that shows or hides, the bin that
// removes. Tool buttons underneath, so code looking for QToolButtons by name
// still finds them.
class FxHeadButton : public QToolButton {
public:
  explicit FxHeadButton(int size = 22, QWidget *parent = nullptr) : QToolButton(parent) {
    setFixedSize(size, size);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::NoFocus);
    setAutoRaise(true);
  }
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (isDown() && isEnabled()) {
      p.setPen(Qt::NoPen);
      p.setBrush(pressedFill());
      p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
    } else if (underMouse() && isEnabled()) {
      p.setPen(Qt::NoPen);
      p.setBrush(hoverFill());
      p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
    }
    draw(p, QRectF(rect()).center());
  }
  virtual QColor hoverFill() const { return ui_theme::ink(255, 255, 255, 22); }
  virtual QColor pressedFill() const { return ui_theme::ink(255, 255, 255, 45); }
  virtual void draw(QPainter &p, QPointF c) = 0;
};
// Open or folded: a chevron, down when open.
class FxChevron : public FxHeadButton {
public:
  explicit FxChevron(int size = 18, QWidget *parent = nullptr) : FxHeadButton(size, parent) { setCheckable(true); }
protected:
  QColor hoverFill() const override { return ui_theme::ink(255, 255, 255, 20); }
  QColor pressedFill() const override { return ui_theme::ink(255, 255, 255, 42); }
  void draw(QPainter &p, QPointF c) override {
    const bool down = isDown();
    const bool hover = underMouse();
    QColor ink = down ? ui_theme::ink("#2D8CFF") : (hover ? ui_theme::ink("#FFFFFF") : ui_theme::ink("#9AA1B2"));
    fxui::chevron(p, c + (down ? QPointF(0, 0.5) : QPointF(0, 0)), isChecked(), ink);
  }
};
// Shown or hidden: After Effects [fx] badge switch.
class FxEye : public FxHeadButton {
public:
  explicit FxEye(int size = 20, QWidget *parent = nullptr) : FxHeadButton(size, parent) {
    setCheckable(true);
    setFixedSize(22, 16);
  }
protected:
  void draw(QPainter &p, QPointF c) override {
    const bool on = isChecked();
    const bool hover = underMouse() && isEnabled();
    const bool down = isDown();
    const QRectF box(c.x() - 10, c.y() - 7.5, 20, 15);

    if (on) {
      if (down) {
        p.setPen(QPen(ui_theme::ink("#1F75D5"), 1.2));
        p.setBrush(ui_theme::ink(22, 34, 50));
      } else if (hover) {
        p.setPen(QPen(ui_theme::ink("#4DA6FF"), 1.2));
        p.setBrush(ui_theme::ink(36, 52, 74));
      } else {
        p.setPen(QPen(ui_theme::ink("#2D8CFF"), 1.0));
        p.setBrush(ui_theme::ink(26, 36, 50));
      }
    } else {
      if (down) {
        p.setPen(QPen(ui_theme::ink("#383838"), 1.0));
        p.setBrush(ui_theme::ink(18, 18, 18));
      } else if (hover) {
        p.setPen(QPen(ui_theme::ink("#4E4E54"), 1.0));
        p.setBrush(ui_theme::ink(30, 30, 32));
      } else {
        p.setPen(QPen(ui_theme::ink("#323236"), 1.0));
        p.setBrush(ui_theme::ink(22, 22, 24));
      }
    }
    p.drawRoundedRect(box, 3.0, 3.0);

    QFont f = font();
    f.setFamily("Segoe UI");
    f.setPixelSize(10);
    f.setBold(true);
    f.setItalic(true);
    p.setFont(f);
    QColor textInk = on ? (down ? ui_theme::ink("#80BFFF") : (hover ? ui_theme::ink("#FFFFFF") : ui_theme::ink("#2D8CFF")))
                        : (hover ? ui_theme::ink("#8E8E94") : ui_theme::ink("#5A5A60"));
    p.setPen(textInk);
    p.drawText(box.adjusted(0, -1, 0, -1), Qt::AlignCenter, "fx");

    if (!on) {
      p.setPen(QPen(hover ? ui_theme::ink("#8E8E94") : ui_theme::ink("#66666A"), 1.2, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(box.topLeft() + QPointF(3.5, 11.5), box.topRight() + QPointF(-3.5, 3.5));
    }
  }
};
// Remove: a bin, red under the pointer.
class FxBin : public FxHeadButton {
public:
  explicit FxBin(int size = 22, QWidget *parent = nullptr) : FxHeadButton(size, parent) {}
protected:
  QColor hoverFill() const override { return ui_theme::ink("#5A2A2E"); }
  void draw(QPainter &p, QPointF c) override {
    const QColor ink = underMouse() ? ui_theme::ink("#FF8A8A") : ui_theme::ink("#9AA0B0");
    p.setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawLine(c + QPointF(-5.5, -4), c + QPointF(5.5, -4));
    p.drawPolyline(QPolygonF{c + QPointF(-2, -4), c + QPointF(-1.5, -6), c + QPointF(1.5, -6), c + QPointF(2, -4)});
    p.drawPolyline(QPolygonF{c + QPointF(-4.2, -2.5), c + QPointF(-3.4, 6), c + QPointF(3.4, 6), c + QPointF(4.2, -2.5)});
    p.drawLine(c + QPointF(-1.2, -1), c + QPointF(-1, 4));
    p.drawLine(c + QPointF(1.2, -1), c + QPointF(1, 4));
  }
};

// A group's heading: a chevron saying whether it is open, then its name in
// small capitals. The guide line of its lines starts under the chevron.
// Clicked anywhere, it folds. A tool
// button underneath, so a panel that kept a QToolButton for its heading keeps
// working as it was.
class FxFold : public QToolButton {
public:
  explicit FxFold(const QString &title, QWidget *parent = nullptr) : QToolButton(parent) {
    setText(title);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::NoFocus);
    setFixedHeight(20);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }
  QSize sizeHint() const override { return QSize(160, 20); }
  QSize minimumSizeHint() const override { return QSize(60, 20); }
  static constexpr int kChevron = 5; // the chevron's centre, from the heading's left
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor ink = !isEnabled() ? fxui::dimInk() : underMouse() ? ui_theme::ink("#E6E9F0") : ui_theme::ink("#8C93A7");
    QFont type = font();
    type.setPixelSize(10);
    type.setWeight(QFont::DemiBold);
    type.setLetterSpacing(QFont::AbsoluteSpacing, .8);
    p.setFont(type);
    p.setPen(ink);
    fxui::chevron(p, QPointF(kChevron, height() / 2. + .5), isChecked(), ink);
    p.setPen(ink);
    const QString title = QFontMetrics(type).elidedText(text().toUpper(), Qt::ElideRight, std::max(0, width() - 18));
    p.drawText(QRect(14, 0, width() - 16, height()), Qt::AlignLeft | Qt::AlignVCenter, title);
  }
};

// A setting's name: as wide as the name column when there is room, and ending
// in "..." (the whole name on the pointer) when the panel is narrow, rather
// than pushing the value and its key off the edge.
class FxName : public QLabel {
  QString full, help;
  int preferred;
public:
  FxName(const QString &name, const QString &help, int preferred) : full(name), help(help), preferred(preferred) {
    setProperty("fxName", true);
    setFont(fxui::lineFont(font()));
    // The height of a value, centred the same way, so a name and its value
    // share one baseline.
    setFixedHeight(18);
    setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMaximumWidth(preferred);
    setText(full);
    setToolTip(help);
  }
  QSize sizeHint() const override { return QSize(preferred, QLabel::sizeHint().height()); }
  QSize minimumSizeHint() const override { return QSize(44, QLabel::minimumSizeHint().height()); }
protected:
  void resizeEvent(QResizeEvent *event) override {
    QLabel::resizeEvent(event);
    const QString shown = fontMetrics().elidedText(full, Qt::ElideRight, std::max(0, width() - 4));
    if (text() != shown) setText(shown);
    setToolTip(!help.isEmpty() ? help : shown != full ? full : QString());
  }
};

// A line that gives way when its panel is narrowed: a long name (a picture's
// file name, say) loses its middle to "..." -- the start and the extension
// stay -- with the whole of it on the pointer, rather than holding the panel
// as wide as the name.
class FxElidedLabel : public QLabel {
  QString full;
public:
  explicit FxElidedLabel(QWidget *parent = nullptr) : QLabel(parent) {
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  }
  void setFullText(const QString &text) {
    full = text;
    updateGeometry();
    refit();
  }
  QString fullText() const { return full; }
  QSize sizeHint() const override {
    return QSize(fontMetrics().horizontalAdvance(full) + 4, QLabel::sizeHint().height());
  }
  QSize minimumSizeHint() const override { return QSize(0, QLabel::minimumSizeHint().height()); }
protected:
  void resizeEvent(QResizeEvent *event) override {
    QLabel::resizeEvent(event);
    refit();
  }
  // A style sheet can change the font: bold to plain, say.
  void changeEvent(QEvent *event) override {
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) refit();
  }
private:
  void refit() {
    const QString shown = fontMetrics().elidedText(full, Qt::ElideMiddle, std::max(0, width() - 4));
    if (text() != shown) setText(shown);
    setToolTip(shown != full ? full : QString());
  }
};

// After Effects style custom delegate for dropdown items: comfortable height,
// generous padding, and signature blue highlight on active selection.
class FxChoiceDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &idx) const override {
    QSize s = QStyledItemDelegate::sizeHint(opt, idx);
    s.setHeight(std::max(s.height(), 22));
    return s;
  }
  void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override {
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);

    const bool isSelected = (opt.state & QStyle::State_Selected);
    const bool isHovered = (opt.state & QStyle::State_MouseOver);

    QRectF itemRect = QRectF(opt.rect).adjusted(2, 1, -2, -1);
    if (isSelected) {
      p->setPen(Qt::NoPen);
      p->setBrush(ui_theme::ink("#2D8CFF"));
      p->drawRoundedRect(itemRect, 3, 3);
    } else if (isHovered) {
      p->setPen(Qt::NoPen);
      p->setBrush(ui_theme::ink("#2C3240"));
      p->drawRoundedRect(itemRect, 3, 3);
    }

    QFont f = opt.font;
    if (isSelected) f.setBold(true);
    p->setFont(f);
    p->setPen(isSelected ? ui_theme::ink("#FFFFFF") : isHovered ? ui_theme::ink("#FFFFFF") : ui_theme::ink("#D8D8E2"));
    QRect textRect = opt.rect.adjusted(12, 0, -12, 0);
    p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, idx.data().toString());

    p->restore();
  }
};

// A list: its choice as text, flush left like every other value, with a small
// chevron after it; popup view is generously sized and styled like After Effects.
class FxChoice : public QComboBox {
public:
  explicit FxChoice(QWidget *parent = nullptr) : QComboBox(parent) {
    setFont(fxui::lineFont(font()));
    setFixedHeight(18);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto *lv = new QListView(this);
    lv->setItemDelegate(new FxChoiceDelegate(this));
    lv->setTextElideMode(Qt::ElideNone);
    lv->setStyleSheet(
      "QListView {"
      "  background-color: #1E1E22;"
      "  border: 1px solid #3A3A44;"
      "  border-radius: 4px;"
      "  padding: 3px 1px;"
      "  outline: none;"
      "}"
    );
    setView(lv);
  }

  QSize sizeHint() const override {
    int widest = 0;
    for (int i = 0; i < count(); ++i) widest = std::max(widest, fontMetrics().horizontalAdvance(itemText(i)));
    return QSize(widest + 24, 18);
  }

  // Never cut: the longest choice (up to the value column) stays readable.
  QSize minimumSizeHint() const override { return QSize(std::min(sizeHint().width(), 100), 18); }

  void showPopup() override {
    int maxW = 0;
    QFontMetrics fm(font());
    for (int i = 0; i < count(); ++i) {
      maxW = std::max(maxW, fm.horizontalAdvance(itemText(i)));
    }
    const int neededW = std::max(width() + 16, maxW + 40);
    if (view()) {
      view()->setTextElideMode(Qt::ElideNone);
      view()->setMinimumWidth(neededW);
    }
    QComboBox::showPopup();
    if (QWidget *popup = findChild<QFrame *>()) {
      popup->setMinimumWidth(neededW);
      popup->resize(std::max(popup->width(), neededW), popup->height());
    }
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (isEnabled() && (underMouse() || hasFocus())) {
      p.setPen(QPen(ui_theme::ink("#383842"), 1.0));
      p.setBrush(ui_theme::ink("#26262B"));
      p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    }
    const QColor ink = isEnabled() ? (underMouse() ? ui_theme::ink("#59A5FF") : fxui::valueInk()) : fxui::dimInk();
    p.setFont(font());
    p.setPen(ink);
    const QRect area = rect().adjusted(-1, 0, -14, 0);
    const QString shown = fontMetrics().elidedText(currentText(), Qt::ElideRight, area.width());
    p.drawText(area, Qt::AlignLeft | Qt::AlignVCenter, shown);
    const int after = std::min(area.left() + fontMetrics().horizontalAdvance(shown) + 6, width() - 7);
    fxui::chevron(p, QPointF(after, height() / 2. + .5), true, ink);
  }
};

// A boxed list whose popup is never narrower than its longest choice. In a
// narrow column the popup took the box's own width, and a layer's name -- even
// "None" -- came out as "...".
class FxWideCombo : public QComboBox {
public:
  using QComboBox::QComboBox;
  void showPopup() override {
    QListView *list = qobject_cast<QListView *>(view());
    const QFontMetrics metrics(list ? list->font() : font());
    int widest = 0;
    for (int i = 0; i < count(); ++i) widest = std::max(widest, metrics.horizontalAdvance(itemText(i)));
    const int needed = std::max(width(), widest + 40);
    if (list) {
      list->setTextElideMode(Qt::ElideNone);
      list->setMinimumWidth(needed);
    }
    QComboBox::showPopup();
    if (QWidget *popup = view() ? view()->parentWidget() : nullptr)
      if (popup->width() < needed) popup->resize(needed, popup->height());
  }
};

// The value column of a line: it asks for the column's width whatever it
// holds, so the keys after a number, a switch and a list all line up.
class FxSlot : public QWidget {
  int preferred;
public:
  explicit FxSlot(int preferred) : preferred(preferred) { setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed); }
  QSize sizeHint() const override { return QSize(preferred, 18); }
};

// Buttons at the end of a set of lines: side by side while they fit, one above
// the other in a panel too narrow for that, and never cut.
class FxButtons : public QWidget {
  QBoxLayout *box;
  std::vector<QPushButton *> buttons;
public:
  FxButtons() {
    box = new QBoxLayout(QBoxLayout::LeftToRight, this);
    box->setContentsMargins(10, 8, 8, 0);
    box->setSpacing(6);
  }
  void add(QPushButton *button) {
    button->setMinimumWidth(40);
    buttons.push_back(button);
    box->addWidget(button, 1);
  }
  QSize minimumSizeHint() const override {
    int widest = 0;
    for (auto *b : buttons) widest = std::max(widest, b->sizeHint().width());
    return QSize(widest + 18, QWidget::minimumSizeHint().height());
  }
protected:
  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    int needed = 18 + box->spacing() * std::max(0, int(buttons.size()) - 1);
    for (auto *b : buttons) needed += b->sizeHint().width();
    const auto direction = width() >= needed ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom;
    if (box->direction() != direction) box->setDirection(direction);
  }
};

// How a colour swatch shows its colour.
inline QString fxSwatchStyle(const QColor &colour) {
  return QString("/*wf:keep*/QPushButton { background:%1; border:1px solid #50566A; border-radius:2px; padding:0; }"
                 "QPushButton:hover { border-color:#D8DCE6; }"
                 "QPushButton:disabled { background:#2A2E3B; border-color:#3A3F4E; }")
      .arg(colour.name());
}

// A switch with its words beside it (a plain QCheckBox), its box drawn with a
// border that shows on the panel. Left to Fusion, the box's outline was
// darker than a dark panel by a shade, and an empty one could not be seen.
// Ticked: blue with a white tick.
inline QString fxCheckBoxStyle(const QString &ink = "#C0C4D4", int fontPx = 11) {
  return QString("QCheckBox { color:%1; font-size:%2px; spacing:6px; }"
                 "QCheckBox::indicator { width:12px; height:12px; border:1px solid #7A8296; border-radius:3px;"
                 " background:#15171C; }"
                 "QCheckBox::indicator:hover { border-color:#2D8CFF; }"
                 "QCheckBox::indicator:checked { background:#2D8CFF; border-color:#2D8CFF; image:url(:/resources/check.svg); }"
                 "QCheckBox::indicator:checked:hover { background:#4DA3FF; border-color:#4DA3FF; }"
                 "QCheckBox::indicator:disabled { border-color:#454A58; background:transparent; }"
                 "QCheckBox::indicator:checked:disabled { background:#34445E; border-color:#34445E; }"
                 "QCheckBox:disabled { color:#5C6070; }")
      .arg(ink)
      .arg(fontPx);
}

// A colour: a small swatch, flush left.
inline QPushButton *fxSwatch() {
  auto *swatch = new QPushButton;
  swatch->setFixedSize(30, 12);
  swatch->setCursor(Qt::PointingHandCursor);
  return swatch;
}
// A switch: a small square, filled when on. No text of its own; its name is
// in the name column like everything else's.
inline QCheckBox *fxCheck() {
  auto *box = new QCheckBox;
  box->setCursor(Qt::PointingHandCursor);
  return box;
}

// One name column for all the lines of a panel. As the panel narrows, every
// name narrows by the same amount -- keeping room for the widest value that
// is showing -- so the values always start in one place, whatever each line
// holds and however deep its group. Worked out after the panel has been laid
// out, whenever its size or what it shows changes.
class FxColumns : public QObject {
public:
  struct Name {
    QPointer<QLabel> label; // an FxName, or a form's label
    QPointer<QWidget> value;
    int preferred; // the name's width when there is room: less a group's indent
    int gap;       // between the name and its value
    int after;     // what the line needs after its value: the key, the margin
    QString full;  // a form's label: its whole text, shortened here as it narrows
  };
  std::vector<Name> names;
  explicit FxColumns(QWidget *body) : QObject(body), body(body) { body->installEventFilter(this); }
  bool eventFilter(QObject *watched, QEvent *event) override {
    if ((watched == body || (viewport && watched == viewport)) &&
        (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show))
      schedule();
    return false;
  }
  void schedule() {
    if (pending) return;
    pending = true;
    QMetaObject::invokeMethod(this, [this] { pending = false; fit(); }, Qt::QueuedConnection);
  }
  void fit() {
    if (!body) return;
    // In a scroll area the room is what the area shows, not the panel: the
    // names' own widths would otherwise hold the panel wide and let values
    // run off the side it shows.
    // A panel moved somewhere else (Text Animation goes from Effect Setting to
    // its own tab) is no longer shown by the area it was first found in:
    // measured against that one, every name shrank to its least.
    if (viewport && !viewport->isAncestorOf(body)) {
      viewport->removeEventFilter(this);
      viewport = nullptr;
    }
    if (!viewport)
      for (QWidget *w = body->parentWidget(); w; w = w->parentWidget())
        if (auto *scroll = qobject_cast<QScrollArea *>(w)) {
          viewport = scroll->viewport();
          viewport->installEventFilter(this);
          break;
        }
    int width = body->width();
    if (viewport) width = std::min(width, viewport->width() - body->mapTo(viewport, QPoint()).x());
    int reserve = 0;
    for (const auto &n : names)
      if (n.value && n.label && n.label->isVisibleTo(body))
        reserve = std::max(reserve, std::max(n.value->minimumSizeHint().width(), n.value->minimumWidth()));
    for (const auto &n : names) {
      if (!n.label) continue;
      const int left = n.label->mapTo(body, QPoint()).x();
      const int w = std::clamp(width - left - n.gap - reserve - n.after, 44, n.preferred);
      if (n.label->minimumWidth() != w || n.label->maximumWidth() != w) n.label->setFixedWidth(w);
      // An FxName shortens itself; a form's plain label is shortened here,
      // with an ellipsis and its whole text on the pointer.
      if (!n.full.isEmpty()) {
        const QString shown = n.label->fontMetrics().elidedText(n.full, Qt::ElideRight, std::max(0, w - 4));
        if (n.label->text() != shown) n.label->setText(shown);
        if (shown != n.full && n.label->toolTip().isEmpty()) n.label->setToolTip(n.full);
      }
    }
  }
private:
  QPointer<QWidget> body, viewport;
  bool pending = false;
};

// One line of the lines: a hairline under it, so the eye follows a name to
// its value, and the guide of every group it is in down its left, so what
// belongs to which heading reads at a glance.
class FxLine : public QWidget {
public:
  int depth, indent, start;
  FxLine(int depth, int indent, int start) : depth(depth), indent(indent), start(start) {}
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    QStyleOption option;
    option.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &option, &p, this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(fxui::ruleInk(), 1));
    p.drawLine(0, height() - 1, width(), height() - 1);
    p.setPen(QPen(fxui::guideInk(), 1));
    for (int level = 0; level < depth; ++level) {
      const int x = start + level * indent + FxFold::kChevron;
      p.drawLine(x, 0, x, height() - 1);
    }
  }
};

// The same lines for a panel laid out as a form: a black hairline between
// one row and the next, and -- for a form inside a group (the property
// "fxGuide") -- the group's guide down its left, under the heading's chevron.
// Drawn on the widget holding the form, in the gaps between its rows.
class FxFormLines : public QObject {
public:
  FxFormLines(QFormLayout *form, QWidget *holder) : QObject(holder), form(form), holder(holder) {
    holder->installEventFilter(this);
  }
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (watched != holder || event->type() != QEvent::Paint || !form) return false;
    QPainter p(holder);
    p.setPen(fxui::ruleInk());
    const int gap = std::max(0, form->verticalSpacing());
    for (int row = 0; row < form->rowCount(); ++row) {
      QRect area;
      for (auto role : {QFormLayout::LabelRole, QFormLayout::FieldRole, QFormLayout::SpanningRole})
        if (QLayoutItem *item = form->itemAt(row, role)) {
          if (QWidget *w = item->widget(); w && w->isHidden()) continue;
          if (!item->geometry().isEmpty()) area = area.united(item->geometry());
        }
      if (area.isEmpty()) continue;
      const int y = area.bottom() + 1 + gap / 2;
      p.drawLine(0, y, holder->width(), y);
    }
    if (form->property("fxGuide").toBool()) {
      const QRect inside = form->geometry();
      const int x = FxFold::kChevron;
      p.setPen(fxui::guideInk());
      p.drawLine(x, inside.top(), x, inside.bottom());
    }
    return false;
  }
private:
  QPointer<QFormLayout> form;
  QWidget *holder;
};

// The same hairlines for a panel laid out as a grid (Text Setting): under
// every row from `firstRow` on that shows anything.
class FxGridLines : public QObject {
public:
  FxGridLines(QGridLayout *grid, QWidget *holder, int firstRow = 0)
      : QObject(holder), grid(grid), holder(holder), firstRow(firstRow) {
    holder->installEventFilter(this);
  }
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (watched != holder || event->type() != QEvent::Paint || !grid) return false;
    QPainter p(holder);
    p.setPen(fxui::ruleInk());
    const int gap = std::max(0, grid->verticalSpacing());
    for (int row = firstRow; row < grid->rowCount(); ++row) {
      int bottom = -1;
      for (int i = 0; i < grid->count(); ++i) {
        int r, c, rs, cs;
        grid->getItemPosition(i, &r, &c, &rs, &cs);
        if (r + rs - 1 != row) continue;
        QLayoutItem *item = grid->itemAt(i);
        if (QWidget *w = item->widget(); w && w->isHidden()) continue;
        if (!item->geometry().isEmpty()) bottom = std::max(bottom, item->geometry().bottom());
      }
      if (bottom < 0) continue;
      const int y = bottom + 1 + gap / 2;
      p.drawLine(0, y, holder->width(), y);
    }
    return false;
  }
private:
  QPointer<QGridLayout> grid;
  QWidget *holder;
  int firstRow;
};

// The lines of one effect's settings.
class FxRows {
public:
  static constexpr int kLine = 20, kIndent = 14, kStart = 10, kNames = 140, kValues = 100, kKey = 16;
  explicit FxRows(QWidget *body) {
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    levels.push_back(layout);
    columns = new FxColumns(body);
  }
  int depth() const { return int(levels.size()) - 1; }
  // One line: the key stopwatch on the left, the name, and the value.
  QWidget *add(const QString &name, QWidget *value, QToolButton *key = nullptr, const QString &help = {}) {
    auto *row = line();
    auto *h = static_cast<QHBoxLayout *>(row->layout());
    // After Effects layout: Stopwatch on the left
    if (key) h->addWidget(key, 0, Qt::AlignVCenter);
    else h->addSpacing(kKey);

    const int preferred = std::max(64, kNames - depth() * kIndent);
    auto *label = new FxName(name, help, preferred);
    h->addWidget(label, 0, Qt::AlignVCenter);

    // Values in a column of their own, flush left, so every value starts in the same place.
    const int wanted = value ? std::max(kValues, std::min(value->sizeHint().width(), value->maximumWidth())) : kValues;
    auto *slot = new FxSlot(wanted);
    slot->setMaximumWidth(wanted);
    columns->names.push_back({label, slot, preferred, 6, 8, QString()});
    auto *inSlot = new QHBoxLayout(slot);
    inSlot->setContentsMargins(0, 0, 0, 0);
    inSlot->setSpacing(0);
    if (value) inSlot->addWidget(value, 0, Qt::AlignLeft | Qt::AlignVCenter);
    inSlot->addStretch(1);
    h->addWidget(slot, 0, Qt::AlignVCenter);
    h->addStretch(1);
    return row;
  }
  // Something that needs the room -- a curve, colour wheels, a note -- or a
  // button (`fill` off keeps its own width), starting where the names do.
  QWidget *addWide(QWidget *widget, bool fill = true) {
    auto *row = new FxLine(depth(), kIndent, kStart);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(kStart + depth() * kIndent, 2, 8, 3);
    h->setSpacing(0);
    h->addWidget(widget, fill ? 1 : 0);
    if (!fill) h->addStretch(1);
    levels.back()->addWidget(row);
    return row;
  }
  // A group under a heading that folds it: what is added until end() goes in.
  // Returns the whole group, which hides and dims as one.
  QWidget *begin(const QString &title, bool open, const QString &help, const QString &objectName) {
    auto *box = new QWidget;
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto *headerRow = new FxLine(depth(), kIndent, kStart);
    headerRow->setFixedHeight(kLine);
    auto *h = new QHBoxLayout(headerRow);
    h->setContentsMargins(kStart + depth() * kIndent, 0, 8, 1);
    h->setSpacing(0);
    auto *fold = new FxFold(title);
    fold->setObjectName(objectName);
    fold->setChecked(open);
    fold->setToolTip(help);
    h->addWidget(fold);
    auto *contents = new QWidget;
    auto *inside = new QVBoxLayout(contents);
    inside->setContentsMargins(0, 0, 0, 0);
    inside->setSpacing(0);
    v->addWidget(headerRow);
    v->addWidget(contents);
    contents->setVisible(open);
    QObject::connect(fold, &QAbstractButton::toggled, contents, [contents](bool on) { contents->setVisible(on); });
    levels.back()->addWidget(box);
    levels.push_back(inside);
    return box;
  }
  void end() {
    if (levels.size() > 1) levels.pop_back();
  }
  // How the kit looks, for the widget holding the lines. Numbers are
  // ScrubSpins marked lineValue (ScrubSpin::compact()).
  static QString style() {
    return "QLabel { color: #D0D0D0; background: transparent; border: none; font-size: 11px; }"
           "QLabel[fxNote=\"true\"] { color: #808080; }"
           "QWidget[fxRow=\"true\"]:hover { background: #262626; }"
           "QAbstractSpinBox { background: #181818; border: 1px solid #303030; border-radius: 3px; color: #DEDEDE; padding: 1px 4px; font-size: 11px; }"
           "QAbstractSpinBox[lineValue=\"true\"] { background: transparent; border: none; color: #2D8CFF; padding: 0; margin: 0; font-size: 11px; font-weight: 500; }"
           "QAbstractSpinBox[lineValue=\"true\"]:hover { background: transparent; border: none; color: #59A5FF; }"
           "QAbstractSpinBox[lineValue=\"true\"]:focus { background: transparent; border: none; color: #59A5FF; }"
           "QAbstractSpinBox[lineValue=\"true\"] QLineEdit { background: transparent; border: none; padding: 0; margin: 0; color: #2D8CFF; }"
           "QAbstractSpinBox[lineValue=\"true\"] QLineEdit:focus { background: transparent; border: none; color: #59A5FF; }"
           "QAbstractSpinBox:disabled, QAbstractSpinBox[lineValue=\"true\"]:disabled { color: #555555; }"
           "QComboBox { background: #1E1E1E; border: 1px solid #333333; border-radius: 3px; color: #DEDEDE; padding: 1px 6px; font-size: 11px; }"
           "QComboBox:hover { border-color: #2D8CFF; }"
           "QComboBox::drop-down { border: none; width: 14px; }"
           "QComboBox QAbstractItemView { background-color: #1E1E22; border: 1px solid #3A3A44; border-radius: 4px; padding: 3px 1px; outline: none; }"
           "QComboBox QAbstractItemView::item { min-height: 22px; padding: 3px 12px; border-radius: 3px; color: #D8D8E2; }"
           "QComboBox QAbstractItemView::item:hover { background-color: #2C3240; color: #FFFFFF; }"
           "QComboBox QAbstractItemView::item:selected { background-color: #2D8CFF; color: #FFFFFF; font-weight: 600; }"
           "QCheckBox { background: transparent; spacing: 0; }"
           "QCheckBox::indicator { width: 12px; height: 12px; border: 1px solid #444444; border-radius: 2px; background: #1E1E1E; }"
           "QCheckBox::indicator:hover { border-color: #2D8CFF; }"
           "QCheckBox::indicator:checked { background: #2D8CFF; border-color: #2D8CFF; }"
           "QCheckBox::indicator:disabled { border-color: #333333; }"
           "QPushButton { background: #2A2A2A; border: 1px solid #383838; border-radius: 3px; color: #DEDEDE; font-size: 11px; padding: 2px 10px; }"
           "QPushButton:hover { border-color: #4A4A4A; background: #323232; color: #FFFFFF; }"
           "QPushButton:disabled { color: #555555; }";
  }
private:
  std::vector<QVBoxLayout *> levels;
  FxColumns *columns = nullptr;
  QWidget *line() {
    auto *row = new FxLine(depth(), kIndent, kStart);
    row->setProperty("fxRow", true);
    row->setAttribute(Qt::WA_StyledBackground);
    row->setAttribute(Qt::WA_Hover);
    row->setFixedHeight(kLine);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(kStart + depth() * kIndent, 0, 8, 1);
    h->setSpacing(6);
    levels.back()->addWidget(row);
    return row;
  }
};
