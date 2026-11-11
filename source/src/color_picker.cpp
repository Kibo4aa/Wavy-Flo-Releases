#include "color_picker.h"
#include <QtWidgets>
#include <algorithm>
#include <cmath>
#include "ui_theme.h"

namespace {

QPixmap checkerTile() {
  QPixmap tile(16, 16);
  tile.fill(QColor("#3a3d43"));
  QPainter p(&tile);
  p.fillRect(0, 0, 8, 8, QColor("#4a4e55"));
  p.fillRect(8, 8, 8, 8, QColor("#4a4e55"));
  return tile;
}

// Hue, saturation and value are kept as the source of truth for the whole
// dialog. Deriving them from the QColor on every change would throw the hue
// away whenever the colour passes through black, grey or white, and the
// saturation square would jump under the pointer as the user drags into a
// corner.
struct Hsva {
  double h = 0, s = 0, v = 0, a = 1;
  QColor color() const {
    // Back to RGB, because QColor::operator== compares the colour space too
    // and every call site stores and serializes plain RGB.
    // Rounded to the eight bits a channel box and the hex field show: the
    // round trip through HSV otherwise leaves a sixteen-bit remainder, and a
    // colour typed as 12, 200, 34 would not compare equal to 12, 200, 34.
    const QColor rgb = QColor::fromHsvF(std::clamp(h, 0., 1.), std::clamp(s, 0., 1.),
                                        std::clamp(v, 0., 1.), std::clamp(a, 0., 1.)).toRgb();
    return QColor(rgb.red(), rgb.green(), rgb.blue(), rgb.alpha());
  }
  // Derived here rather than through QColor::hueF(), which rounds hue to whole
  // degrees: that is coarse enough to shift a channel, so simply opening this
  // dialog on a colour and pressing OK could change it.
  void setColor(const QColor &c) {
    const double r = c.redF(), g = c.greenF(), b = c.blueF();
    const double high = std::max({r, g, b}), low = std::min({r, g, b});
    const double range = high - low;
    if (range > 0) { // an achromatic colour has no hue of its own: keep the last
      double hue = high == r   ? std::fmod((g - b) / range + 6., 6.)
                 : high == g   ? (b - r) / range + 2.
                               : (r - g) / range + 4.;
      h = hue / 6.;
    }
    s = high > 0 ? range / high : 0;
    v = high;
    a = c.alphaF();
  }
};

// Drawn rather than shipped: one small glyph is not worth a resource file, and
// this way it follows the palette if the theme ever moves.
QIcon eyedropperIcon() {
  QPixmap pix(20, 20);
  pix.fill(Qt::transparent);
  QPainter p(&pix);
  p.setRenderHint(QPainter::Antialiasing);
  p.translate(10, 10);
  p.rotate(45); // built upright, then tilted so the nib points down and left
  p.setPen(Qt::NoPen);
  p.setBrush(QColor("#d9dde5"));
  // The bulb has to be almost three times the width of the barrel, or the two
  // merge into one tapering blob and the glyph reads as a droplet.
  p.drawRoundedRect(QRectF(-3.6, -8.4, 7.2, 5.6), 2.6, 2.6); // bulb
  p.drawRect(QRectF(-2.5, -3.0, 5.0, 1.4));                  // collar
  p.drawRect(QRectF(-1.3, -1.6, 2.6, 6.0));                  // barrel
  QPainterPath nib;
  nib.moveTo(-1.3, 4.4);
  nib.lineTo(1.3, 4.4);
  nib.lineTo(0, 8.4);
  nib.closeSubpath();
  p.drawPath(nib);
  return QIcon(pix);
}

// Saturation across, value down, for one hue.
class SaturationField : public QWidget {
public:
  Hsva *hsva;
  std::function<void()> changed;
  explicit SaturationField(Hsva *state) : hsva(state) {
    setFixedSize(248, 196);
    setCursor(Qt::CrossCursor);
  }
  void paintEvent(QPaintEvent *) override {
    if (cache.size() != size() || std::abs(cachedHue - hsva->h) > 1e-6) {
      cachedHue = hsva->h;
      cache = QImage(size(), QImage::Format_RGB32);
      for (int y = 0; y < height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(cache.scanLine(y));
        const double v = 1. - double(y) / std::max(1, height() - 1);
        for (int x = 0; x < width(); ++x)
          line[x] = QColor::fromHsvF(cachedHue, double(x) / std::max(1, width() - 1), v).rgb();
      }
    }
    QPainter p(this);
    p.drawImage(0, 0, cache);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF at(hsva->s * (width() - 1), (1 - hsva->v) * (height() - 1));
    // Two rings, so the marker stays visible on both light and dark areas.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 190), 1));
    p.drawEllipse(at, 6.5, 6.5);
    p.setPen(QPen(QColor(255, 255, 255, 230), 1.6));
    p.drawEllipse(at, 5.2, 5.2);
    p.setPen(QPen(QColor("#6f7783"), 1));
    p.drawRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5));
  }
  void mousePressEvent(QMouseEvent *e) override { track(e->position()); }
  void mouseMoveEvent(QMouseEvent *e) override { track(e->position()); }

private:
  QImage cache;
  double cachedHue = -1;
  void track(QPointF at) {
    hsva->s = std::clamp(at.x() / std::max(1, width() - 1), 0., 1.);
    hsva->v = std::clamp(1. - at.y() / std::max(1, height() - 1), 0., 1.);
    update();
    if (changed) changed();
  }
};

// A vertical strip: hue when alphaMode is false, the colour's own alpha when
// it is true. One widget, because the interaction is identical.
class Strip : public QWidget {
public:
  Hsva *hsva;
  bool alphaMode;
  std::function<void()> changed;
  Strip(Hsva *state, bool alpha) : hsva(state), alphaMode(alpha) {
    setFixedSize(22, 196);
    setCursor(Qt::PointingHandCursor);
  }
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    const QRectF bar = QRectF(rect()).adjusted(.5, .5, -.5, -.5);
    if (alphaMode) {
      p.fillRect(bar, QBrush(checkerTile()));
      QLinearGradient gradient(0, 0, 0, height());
      auto opaque = hsva->color();
      opaque.setAlphaF(1);
      gradient.setColorAt(0, opaque);
      opaque.setAlphaF(0);
      gradient.setColorAt(1, opaque);
      p.fillRect(bar, gradient);
    } else {
      QLinearGradient gradient(0, 0, 0, height());
      for (int stop = 0; stop <= 6; ++stop)
        gradient.setColorAt(stop / 6., QColor::fromHsvF(stop / 6., 1, 1));
      p.fillRect(bar, gradient);
    }
    p.setPen(QPen(QColor("#6f7783"), 1));
    p.drawRect(bar);
    const double position = alphaMode ? 1 - hsva->a : hsva->h;
    const double y = position * (height() - 1);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 190), 3));
    p.drawLine(QPointF(1, y), QPointF(width() - 1, y));
    p.setPen(QPen(QColor("#ffffff"), 1.4));
    p.drawLine(QPointF(1, y), QPointF(width() - 1, y));
  }
  void mousePressEvent(QMouseEvent *e) override { track(e->position().y()); }
  void mouseMoveEvent(QMouseEvent *e) override { track(e->position().y()); }

private:
  void track(double y) {
    const double position = std::clamp(y / std::max(1, height() - 1), 0., 1.);
    if (alphaMode) hsva->a = 1 - position; else hsva->h = position;
    update();
    if (changed) changed();
  }
};

class ColorPicker : public QDialog {
public:
  Hsva hsva;
  bool withAlpha;
  bool syncing = false;
  SaturationField *field;
  Strip *hue, *alpha;
  QLineEdit *hex;
  QSpinBox *channels[4]{};  // red, green, blue, alpha
  QSpinBox *hsvBoxes[3]{};  // hue degrees, saturation %, value %
  QToolButton *picking = nullptr;
  bool sampling = false;
  std::function<void(const QColor &)> live;
  QColor liveSent;
  bool livePending = false;

  ColorPicker(QWidget *parent, QColor initial, const QString &title, bool alphaEnabled,
              std::function<void(const QColor &)> preview = {})
      : QDialog(parent), withAlpha(alphaEnabled), live(std::move(preview)) {
    setWindowTitle(title);
    hsva.setColor(initial);
    if (!withAlpha) hsva.a = 1;
    liveSent = hsva.color();

    field = new SaturationField(&hsva);
    hue = new Strip(&hsva, false);
    alpha = new Strip(&hsva, true);
    hex = new QLineEdit;
    hex->setFixedWidth(96); // 96 + spacing + the 26px eyedropper == fieldWidth

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 14, 14, 12);
    outer->setSpacing(12);
    // Every control in here has a size of its own, so the dialog takes the
    // size of its contents. Dragging it wider only ever produced empty space.
    outer->setSizeConstraint(QLayout::SetFixedSize);
    auto *top = new QHBoxLayout;
    top->setSpacing(10);
    top->addWidget(field);
    top->addWidget(hue);
    if (withAlpha) top->addWidget(alpha);

    auto *side = new QVBoxLayout;
    side->setSpacing(8);
    const int fieldWidth = 128;
    auto *numbers = new QFormLayout;
    numbers->setContentsMargins(0, 0, 0, 0);
    numbers->setHorizontalSpacing(8);
    numbers->setVerticalSpacing(5);
    auto *eyedropper = new QToolButton;
    eyedropper->setObjectName("colorEyedropper");
    eyedropper->setIcon(ui_theme::themedIcon(eyedropperIcon()));
    eyedropper->setIconSize(QSize(20, 20));
    eyedropper->setFixedSize(26, 26);
    eyedropper->setCheckable(true);
    eyedropper->setToolTip("Pick a colour from anywhere on screen");
    auto *hexRow = new QWidget;
    auto *hexLayout = new QHBoxLayout(hexRow);
    hexLayout->setContentsMargins(0, 0, 0, 0);
    hexLayout->setSpacing(6);
    hexLayout->addWidget(hex);
    hexLayout->addWidget(eyedropper);
    numbers->addRow("Hex", hexRow);
    const char *channelNames[4]{"R", "G", "B", "A"};
    for (int i = 0; i < (withAlpha ? 4 : 3); ++i) {
      channels[i] = new QSpinBox;
      channels[i]->setObjectName(QString("colorChannel/") + channelNames[i]);
      channels[i]->setRange(0, 255);
      channels[i]->setFixedWidth(fieldWidth);
      numbers->addRow(channelNames[i], channels[i]);
    }
    const char *hsvNames[3]{"H", "S", "V"};
    const char *hsvSuffix[3]{"°", " %", " %"};
    for (int i = 0; i < 3; ++i) {
      hsvBoxes[i] = new QSpinBox;
      hsvBoxes[i]->setObjectName(QString("colorHsv/") + hsvNames[i]);
      hsvBoxes[i]->setRange(0, i == 0 ? 359 : 100);
      hsvBoxes[i]->setSuffix(hsvSuffix[i]);
      hsvBoxes[i]->setFixedWidth(fieldWidth);
      hsvBoxes[i]->setWrapping(i == 0);
      numbers->addRow(hsvNames[i], hsvBoxes[i]);
    }
    side->addLayout(numbers);
    side->addStretch();
    top->addLayout(side);
    outer->addLayout(top);

    // A short row of anchors -- the greys and primaries a composition keeps
    // coming back to -- rather than a wall of swatches nobody reads.
    auto *presets = new QHBoxLayout;
    presets->setSpacing(5);
    for (QString name : {"#000000", "#404040", "#808080", "#c0c0c0", "#ffffff",
                         "#ff3b30", "#ff9500", "#ffcc00", "#34c759", "#32ade6",
                         "#8175ee", "#ff2d95"}) {
      auto *swatch = new QPushButton;
      swatch->setObjectName("colorPreset/" + name);
      swatch->setFixedSize(22, 22);
      swatch->setToolTip(name);
      swatch->setStyleSheet(QString("background:%1;border:1px solid #55595f;border-radius:3px;").arg(name));
      connect(swatch, &QPushButton::clicked, this, [this, name] {
        QColor picked(name);
        picked.setAlphaF(hsva.a);
        hsva.setColor(picked);
        syncFrom(nullptr);
      });
      presets->addWidget(swatch);
    }
    presets->addStretch();
    outer->addLayout(presets);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    field->changed = [this] { syncFrom(field); };
    hue->changed = [this] { syncFrom(hue); };
    alpha->changed = [this] { syncFrom(alpha); };
    connect(hex, &QLineEdit::textEdited, this, [this](const QString &text) {
      if (syncing) return;
      auto parsed = colorFromHexText(text);
      if (!parsed.isValid()) return;
      if (!withAlpha) parsed.setAlphaF(1);
      hsva.setColor(parsed);
      syncFrom(hex);
    });
    for (int i = 0; i < 4; ++i) {
      if (!channels[i]) continue;
      connect(channels[i], &QSpinBox::valueChanged, this, [this](int) {
        if (syncing) return;
        QColor next(channels[0]->value(), channels[1]->value(), channels[2]->value(),
                    channels[3] ? channels[3]->value() : 255);
        hsva.setColor(next);
        syncFrom(channels[0]);
      });
    }
    for (auto *box : hsvBoxes)
      connect(box, &QSpinBox::valueChanged, this, [this](int) {
        if (syncing) return;
        hsva.h = hsvBoxes[0]->value() / 360.;
        hsva.s = hsvBoxes[1]->value() / 100.;
        hsva.v = hsvBoxes[2]->value() / 100.;
        syncFrom(hsvBoxes[0]);
      });
    connect(eyedropper, &QToolButton::clicked, this, [this] {
      sampling = true;
      grabMouse(Qt::CrossCursor);
    });
    picking = eyedropper;
    syncFrom(nullptr);
  }

  QColor selected() const { return hsva.color(); }

  // Everything reads from hsva; the widget that caused the change is left
  // alone so an edit in progress is not rewritten under the caret.
  void syncFrom(QWidget *source) {
    syncing = true;
    const auto color = hsva.color();
    if (source != hex) hex->setText(hexTextFromColor(color, withAlpha));
    if (source != channels[0]) {
      const int values[4]{color.red(), color.green(), color.blue(), color.alpha()};
      for (int i = 0; i < 4; ++i)
        if (channels[i]) channels[i]->setValue(values[i]);
    }
    if (source != hsvBoxes[0]) {
      hsvBoxes[0]->setValue(qRound(hsva.h * 360) % 360);
      hsvBoxes[1]->setValue(qRound(hsva.s * 100));
      hsvBoxes[2]->setValue(qRound(hsva.v * 100));
    }
    if (source != field) field->update();
    if (source != hue) hue->update();
    alpha->update();
    syncing = false;
    showLive();
  }

  // Dragging across the field changes the colour on every mouse move, far more
  // often than a composition can redraw. The colour is handed over once the
  // moves already queued have been read, so the preview shows where the
  // pointer is rather than working through where it has been.
  void showLive() {
    if (!live || livePending) return;
    livePending = true;
    QTimer::singleShot(0, this, [this] {
      livePending = false;
      const QColor color = hsva.color();
      if (color == liveSent) return;
      liveSent = color;
      live(color);
    });
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (!sampling) { QDialog::mouseReleaseEvent(event); return; }
    sampling = false;
    releaseMouse();
    if (picking) picking->setChecked(false);
    const auto at = event->globalPosition().toPoint();
    if (auto *screen = QGuiApplication::screenAt(at)) {
      const auto shot = screen->grabWindow(0, at.x(), at.y(), 1, 1).toImage();
      if (!shot.isNull()) {
        auto picked = shot.pixelColor(0, 0);
        picked.setAlphaF(hsva.a);
        hsva.setColor(picked);
        syncFrom(nullptr);
      }
    }
  }
};

} // namespace

QColor colorFromHexText(QString text) {
  text = text.trimmed();
  if (text.startsWith('#')) text.remove(0, 1);
  if (text.size() != 3 && text.size() != 6 && text.size() != 8) return {};
  bool ok = false;
  text.toUInt(&ok, 16);
  if (!ok) return {};
  if (text.size() == 8) {
    // Qt reads #AARRGGBB; the field is written and read as #RRGGBBAA.
    return QColor("#" + text.right(2) + text.left(6));
  }
  return QColor("#" + text);
}

QString hexTextFromColor(QColor color, bool alpha) {
  auto text = QString("#%1%2%3")
                  .arg(color.red(), 2, 16, QChar('0'))
                  .arg(color.green(), 2, 16, QChar('0'))
                  .arg(color.blue(), 2, 16, QChar('0'));
  if (alpha && color.alpha() != 255)
    text += QString("%1").arg(color.alpha(), 2, 16, QChar('0'));
  return text;
}

QColor pickColor(QWidget *parent, QColor initial, const QString &title, bool alpha,
                 const std::function<void(const QColor &)> &live) {
  if (!initial.isValid()) initial = Qt::white;
  ColorPicker picker(parent, initial, title, alpha, live);
  if (picker.exec() != QDialog::Accepted) return {};
  return picker.selected();
}

int checkColorPicker(QStringList &failures) {
  auto check = [&](bool ok, QString message) { if (!ok) failures << message; };
  check(colorFromHexText("#ff8800") == QColor(255, 136, 0), "six digit hex");
  check(colorFromHexText("ff8800") == QColor(255, 136, 0), "hex without a hash");
  check(colorFromHexText("  #F80  ") == QColor(255, 136, 0), "three digit hex and surrounding space");
  check(colorFromHexText("#ff880080").alpha() == 128, "eight digit hex carries alpha");
  check(colorFromHexText("#ff880080") == QColor(255, 136, 0, 128), "eight digit hex keeps its channels in order");
  check(!colorFromHexText("#gg0000").isValid(), "non hexadecimal text is rejected");
  check(!colorFromHexText("#ff00").isValid(), "hex of the wrong length is rejected");
  check(hexTextFromColor(QColor(255, 136, 0), true) == "#ff8800", "opaque colours need no alpha digits");
  check(hexTextFromColor(QColor(255, 136, 0, 128), true) == "#ff880080", "hex round trips through the field");
  check(colorFromHexText(hexTextFromColor(QColor(18, 52, 86, 120), true)) == QColor(18, 52, 86, 120),
        "hex round trips back to the same colour");

  ColorPicker picker(nullptr, QColor(120, 40, 200, 90), "Test", true);
  check(picker.selected() == QColor(120, 40, 200, 90), "the dialog opens on the colour it was given");
  // A colour that passes through black keeps the hue the user was working in:
  // deriving hue from the colour each time would silently reset it to red.
  const double hueBefore = picker.hsva.h;
  picker.hsva.setColor(Qt::black);
  check(std::abs(picker.hsva.h - hueBefore) < 1e-9, "hue survives a colour with no hue of its own");
  picker.hsva.v = 1;
  picker.hsva.s = 1;
  check(std::abs(picker.selected().hueF() - hueBefore) < .01, "the hue comes back when value does");

  picker.hsva.setColor(QColor(10, 20, 30, 255));
  picker.syncFrom(nullptr);
  check(picker.hex->text() == "#0a141e", "the hex field follows the colour");
  check(picker.channels[0]->value() == 10 && picker.channels[2]->value() == 30,
        "the channel boxes follow the colour");

  {
    std::vector<QColor> shown;
    ColorPicker previewing(nullptr, QColor(200, 10, 10), "Live", false,
                           [&](const QColor &color) { shown.push_back(color); });
    QCoreApplication::processEvents();
    check(shown.empty(), "opening the picker shows nothing new: the colour has not changed");
    // Three edits before the event loop gets back round, the way a drag
    // arrives: one preview, of the last of them.
    previewing.channels[0]->setValue(20);
    previewing.channels[1]->setValue(120);
    previewing.channels[2]->setValue(220);
    QCoreApplication::processEvents();
    check(shown.size() == 1 && shown.back() == QColor(20, 120, 220),
          "edits made together are previewed once, with the latest colour");
    previewing.channels[0]->setValue(12);
    previewing.channels[1]->setValue(200);
    previewing.channels[2]->setValue(34);
    QCoreApplication::processEvents();
    check(shown.size() == 2 && shown.back() == QColor(12, 200, 34),
          "the colour handed over is exactly the one the boxes show");
    shown.pop_back();
    previewing.syncFrom(nullptr);
    QCoreApplication::processEvents();
    check(shown.size() == 1, "and a colour already shown is not sent again");
    previewing.hsva.setColor(QColor(255, 255, 0));
    previewing.syncFrom(nullptr);
    QCoreApplication::processEvents();
    check(shown.size() == 2 && shown.back() == QColor(255, 255, 0),
          "a swatch or eyedropper colour is previewed as well");
  }
  return failures.isEmpty() ? 0 : 1;
}
