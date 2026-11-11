#pragma once
// Wavy-Flo's looks: Dark, the one every panel was drawn in; Grey, neutral
// greys as After Effects has them; and Light.
//
// Every other look is Dark turned, one colour at a time, so nothing drawn for
// Dark needs a second set of colours:
// - the greys of the panels, fields, lines and writing move along a lightness
//   curve -- Grey lifts them and takes the blue out of them, Light turns them
//   over so the panels go pale and the writing on them dark -- and keep their
//   order, so what stood out still stands out;
// - a colour of its own (the blue values, the keys, a label's colour, a
//   warning) keeps its hue; on Light it is taken deeper, to read on white.
// A few colours that carry the structure (the hairlines between rows) are
// given by hand, where turning them over would leave them invisible.
//
// Stylesheets are turned as they are set (Restyler, installed on the
// application); painted panels ask for their colours through ink(). A widget
// showing the person's own colour -- a swatch -- says so with the property
// "wfUserColour", and its background is left alone.
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QHash>
#include <QIcon>
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QRegularExpression>
#include <QSettings>
#include <QString>
#include <QStyle>
#include <QStyleFactory>
#include <QWidget>
#include <algorithm>
#include <functional>
#include <vector>

namespace ui_theme {
enum class Look { Dark = 0, Grey = 1, Light = 2 };

inline Look &look() {
  static Look current = Look::Dark;
  return current;
}
inline QString lookName(Look l) { return l == Look::Grey ? "Grey" : l == Look::Light ? "Light" : "Dark"; }
inline Look lookNamed(const QString &name) {
  return name.compare("Grey", Qt::CaseInsensitive) == 0 || name.compare("Gray", Qt::CaseInsensitive) == 0 ? Look::Grey
         : name.compare("Light", Qt::CaseInsensitive) == 0 ? Look::Light
                                                            : Look::Dark;
}

// Colours given by hand: where the curve would leave a line as pale as the
// panel under it. Keyed by Dark's colour (RGB, no alpha).
inline const QHash<QRgb, QRgb> &byHand(Look to) {
  static const QHash<QRgb, QRgb> grey{};
  // The hairlines between rows and around panels: darker than the panel on
  // Dark, and darker again on Light (turned over they went paler than it).
  static const QHash<QRgb, QRgb> light{
      {0x141416, 0xC9CBD2}, // the effect rows' and timeline's rules
      {0x18181C, 0xCCCED4}, {0x1A1A1E, 0xCFD1D7}, {0x202227, 0xC6C8CF}, {0x222429, 0xC9CBD1},
      {0x282828, 0xC4C4C8}, // bars' borders, the window's separators
      {0x0F1015, 0xA9ADB8}, // a swatch's frame
  };
  return to == Look::Grey ? grey : light;
}

// One colour of Dark, in the look asked for.
inline QColor turned(const QColor &dark, Look to = look()) {
  if (to == Look::Dark || !dark.isValid()) return dark;
  const auto &hand = byHand(to);
  if (!hand.isEmpty()) {
    auto found = hand.constFind(dark.rgb() & 0xFFFFFF);
    if (found != hand.cend()) {
      QColor c = QColor::fromRgb(*found);
      c.setAlphaF(dark.alphaF());
      return c;
    }
  }
  float h = 0, s = 0, l = 0, a = 1;
  dark.getHslF(&h, &s, &l, &a);
  if (h < 0) h = 0;
  // A colour of its own -- a value's blue, a key, a label, a warning -- rather
  // than one of the greys (Dark's greys lean blue, but only a little).
  const bool mark = s >= .35f && l >= .3f;
  if (to == Look::Grey) {
    if (mark) return dark;
    // A tinted dark -- a selected row's blue -- keeps its tint; a grey loses its blue.
    return QColor::fromHslF(h, s >= .35f ? s : 0.f, std::clamp(.09f + .91f * l, 0.f, 1.f), a);
  }
  // Light.
  if (mark) return QColor::fromHslF(h, s, std::min(l, .3f + .4f * l), a);
  // A shadow stays a shadow.
  if (l < .02f && a < .999f) return dark;
  // Turned over: Dark's panels (lightness .04-.25) go near white, its
  // writing (.6-1) goes dark grey to near black.
  const float pale = l <= .25f ? .995f - .78f * l : .80f - 1.0267f * (l - .25f);
  return QColor::fromHslF(h, s * .5f, std::clamp(pale, 0.f, 1.f), a);
}
// For painted panels: Dark's colour by its name, in the current look. Asked
// for at every paint, so each name is turned once per look.
inline QColor ink(const char *dark) {
  if (look() == Look::Dark) return QColor(QLatin1String(dark));
  thread_local QHash<quintptr, QColor> memo[3];
  auto &m = memo[int(look())];
  const auto key = quintptr(dark);
  if (auto found = m.constFind(key); found != m.cend()) return *found;
  const QColor c = turned(QColor(QLatin1String(dark)));
  m.insert(key, c);
  return c;
}
inline QColor ink(const QColor &dark) { return turned(dark); }
inline QColor ink(int r, int g, int b, int a = 255) { return turned(QColor(r, g, b, a)); }

// A stylesheet of Dark's colours in the look asked for: every #rgb, #rrggbb,
// #aarrggbb, rgb(), rgba(), white and black in it. With `keepBackgrounds` the
// background declarations stay as they are (a swatch's own colour).
// An icon drawn in Dark's inks, shown in the look that is on: every pixel's
// colour turned, its coverage kept. The icons are drawn once, when the panels
// are made; this turns them as they are shown, so a new look needs no new
// icons. Never for an icon showing the person's own colour (a label chip).
class TurnedIconEngine : public QIconEngine {
  QIcon source;
  QHash<QString, QPixmap> made;

public:
  explicit TurnedIconEngine(QIcon dark) : source(std::move(dark)) {}
  QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
    const QPixmap original = source.pixmap(size, scale, mode, state);
    if (look() == Look::Dark || original.isNull()) return original;
    const QString key = QString("%1|%2x%3|%4|%5|%6|%7")
                            .arg(int(look()))
                            .arg(size.width())
                            .arg(size.height())
                            .arg(int(mode))
                            .arg(int(state))
                            .arg(scale)
                            .arg(original.cacheKey());
    if (auto found = made.constFind(key); found != made.cend()) return *found;
    if (made.size() > 64) made.clear();
    QImage image = original.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
      auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
      for (int x = 0; x < image.width(); ++x) {
        const QRgb pixel = line[x];
        if (!qAlpha(pixel)) continue;
        const QColor t = turned(QColor(qRed(pixel), qGreen(pixel), qBlue(pixel)));
        line[x] = qRgba(t.red(), t.green(), t.blue(), qAlpha(pixel));
      }
    }
    QPixmap out = QPixmap::fromImage(image);
    out.setDevicePixelRatio(original.devicePixelRatio());
    made.insert(key, out);
    return out;
  }
  QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
    return scaledPixmap(size, mode, state, 1.0);
  }
  void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
    const qreal scale = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
    const QPixmap shown = scaledPixmap(rect.size(), mode, state, scale);
    if (shown.isNull()) return;
    const QSizeF logical = QSizeF(shown.size()) / shown.devicePixelRatio();
    const QPointF at(rect.x() + (rect.width() - logical.width()) / 2, rect.y() + (rect.height() - logical.height()) / 2);
    painter->drawPixmap(at, shown);
  }
  QSize actualSize(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
    return source.actualSize(size, mode, state);
  }
  QList<QSize> availableSizes(QIcon::Mode mode, QIcon::State state) override {
    return source.availableSizes(mode, state);
  }
  QString key() const override { return QStringLiteral("wfTurnedIcon"); }
  QIconEngine *clone() const override { return new TurnedIconEngine(source); }
};
inline QIcon themedIcon(const QIcon &dark) { return dark.isNull() ? dark : QIcon(new TurnedIconEngine(dark)); }
// A drawn mark beside a value (where a QLabel held a fixed picture), shown in
// the look that is on. The pointer goes through it to what it sits on.
class IconMark : public QWidget {
  QIcon icon;

public:
  explicit IconMark(const QIcon &shown, QWidget *parent = nullptr) : QWidget(parent), icon(shown) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    icon.paint(&p, rect());
  }
};

// A stylesheet that shows the person's own colour -- a swatch, the letters
// being typed -- starts with this, and is left whole in every look.
inline const QString &keepMarker() {
  static const QString marker("/*wf:keep*/");
  return marker;
}
inline QString turnedStyle(const QString &qss, Look to = look(), bool keepBackgrounds = false) {
  if (to == Look::Dark || qss.isEmpty() || qss.startsWith(keepMarker())) return qss;
  static QHash<QString, QString> memo[2][3];
  auto &m = memo[keepBackgrounds ? 1 : 0][int(to)];
  if (auto found = m.constFind(qss); found != m.cend()) return *found;
  if (m.size() > 20000) m.clear();
  static const QRegularExpression colour(
      R"((#[0-9a-fA-F]{8}\b|#[0-9a-fA-F]{6}\b|#[0-9a-fA-F]{3}\b)|rgba?\(\s*(\d{1,3})\s*,\s*(\d{1,3})\s*,\s*(\d{1,3})\s*(,\s*[0-9.]+%?\s*)?\)|\b(white|black)\b)",
      QRegularExpression::CaseInsensitiveOption);
  auto turnPart = [&](const QString &part) {
    QString out;
    out.reserve(part.size());
    qsizetype at = 0;
    auto it = colour.globalMatch(part);
    while (it.hasNext()) {
      const auto match = it.next();
      out += part.mid(at, match.capturedStart() - at);
      QString replaced = match.captured(0);
      if (!match.captured(1).isEmpty()) {
        const QString hex = match.captured(1);
        QColor c(hex);
        if (c.isValid()) {
          const QColor t = turned(c, to);
          replaced = hex.size() == 9 ? t.name(QColor::HexArgb) : t.name(QColor::HexRgb);
        }
      } else if (!match.captured(2).isEmpty()) {
        const QColor t = turned(QColor(match.captured(2).toInt(), match.captured(3).toInt(), match.captured(4).toInt()), to);
        const QString alpha = match.captured(5);
        replaced = alpha.isEmpty() ? QString("rgb(%1, %2, %3)").arg(t.red()).arg(t.green()).arg(t.blue())
                                   : QString("rgba(%1, %2, %3%4)").arg(t.red()).arg(t.green()).arg(t.blue()).arg(alpha);
      } else if (!match.captured(6).isEmpty()) {
        replaced = turned(QColor(match.captured(6).toLower()), to).name(QColor::HexRgb);
      }
      out += replaced;
      at = match.capturedEnd();
    }
    out += part.mid(at);
    return out;
  };
  QString result;
  if (!keepBackgrounds) {
    result = turnPart(qss);
    // The lists' arrow is a picture: a dark one on Light.
    if (to == Look::Light) result.replace(":/resources/dropdown.svg", ":/resources/dropdown-ink.svg");
  } else {
    // Declaration by declaration, leaving "background" and "background-color" alone.
    static const QRegularExpression declaration(R"(([^;{}]*)([;{}]|$))");
    auto it = declaration.globalMatch(qss);
    while (it.hasNext()) {
      const auto match = it.next();
      if (match.capturedLength(0) == 0) break;
      const QString body = match.captured(1);
      const QString name = body.section(':', 0, 0).trimmed().toLower();
      const bool own = body.contains(':') && (name == "background" || name == "background-color");
      result += own ? body : turnPart(body);
      result += match.captured(2);
    }
  }
  m.insert(qss, result);
  return result;
}

// Turns a widget's own stylesheet to the current look, keeping Dark's to turn
// again when the look changes -- or, on Dark, gives it Dark's back. Its own:
// one Wavy-Flo set, not one turned here.
inline void restyle(QWidget *w) {
  static bool busy = false;
  if (busy || !w) return;
  const QString now = w->styleSheet();
  const QVariant turnedBefore = w->property("wfTurnedStyle");
  const QString before = turnedBefore.toString();
  const bool ours = turnedBefore.isValid() && now == before;
  if (ours && w->property("wfTurnedLook").toInt() == int(look())) return;
  // Code that added to the stylesheet it read back added to the turned one:
  // what it added is Dark's, what it read is ours.
  const QString keptDark = w->property("wfDarkStyle").toString();
  const QString dark = ours ? keptDark
                       : turnedBefore.isValid() && !before.isEmpty() && now.startsWith(before)
                           ? keptDark + now.mid(before.size())
                       : turnedBefore.isValid() && !before.isEmpty() && now.endsWith(before)
                           ? now.left(now.size() - before.size()) + keptDark
                           : now;
  auto forget = [w] {
    w->setProperty("wfDarkStyle", QVariant());
    w->setProperty("wfTurnedStyle", QVariant());
    w->setProperty("wfTurnedLook", QVariant());
  };
  if (look() == Look::Dark || dark.isEmpty()) {
    if (turnedBefore.isValid()) forget();
    if (dark != now) {
      busy = true;
      w->setStyleSheet(dark);
      busy = false;
    }
    return;
  }
  const QString turnedNow = turnedStyle(dark, look(), w->property("wfUserColour").toBool());
  w->setProperty("wfDarkStyle", dark);
  w->setProperty("wfTurnedStyle", turnedNow);
  w->setProperty("wfTurnedLook", int(look()));
  if (turnedNow != now) {
    busy = true;
    w->setStyleSheet(turnedNow);
    busy = false;
  }
}
// A widget's stylesheet as it was set, in Dark's colours, whichever look is on.
inline QString darkStyle(const QWidget *w) {
  if (!w) return {};
  const QVariant turnedBefore = w->property("wfTurnedStyle");
  return turnedBefore.isValid() && w->styleSheet() == turnedBefore.toString() ? w->property("wfDarkStyle").toString()
                                                                              : w->styleSheet();
}
// A stylesheet set only when it differs from what the widget has -- in Dark's
// colours, whichever look is on.
inline void setStyleIfChanged(QWidget *w, const QString &dark) {
  if (!w) return;
  if (w->styleSheet() == dark) return;
  if (look() != Look::Dark && w->property("wfDarkStyle").toString() == dark &&
      w->styleSheet() == w->property("wfTurnedStyle").toString() && w->property("wfTurnedLook").toInt() == int(look()))
    return;
  w->setStyleSheet(dark);
}

// Installed on the application: a widget's stylesheet is turned when it is
// first polished and whenever it is set again; a widget that was hidden when
// the look changed is turned when it is shown.
class Restyler : public QObject {
public:
  using QObject::QObject;
  bool eventFilter(QObject *watched, QEvent *event) override {
    const auto type = event->type();
    if (type != QEvent::Show && type != QEvent::Polish && type != QEvent::StyleChange) return false;
    if (!watched->isWidgetType()) return false;
    auto *w = static_cast<QWidget *>(watched);
    if (type == QEvent::Show) {
      const QVariant turnedLook = w->property("wfTurnedLook");
      if (turnedLook.isValid() ? turnedLook.toInt() != int(look()) : look() != Look::Dark && !w->styleSheet().isEmpty())
        restyle(w);
    } else if (look() != Look::Dark) {
      restyle(w);
    }
    return false;
  }
};

// The application's own stylesheet and palette, in Dark's colours, kept to
// be turned again.
inline QString &appDarkStyle() {
  static QString style;
  return style;
}
inline QPalette &appDarkPalette() {
  static QPalette palette;
  return palette;
}
inline bool &appPaletteKept() {
  static bool kept = false;
  return kept;
}
// The palette Wavy-Flo sets names only some roles; the rest are the system's
// (light or dark, as Windows is set), so only the named ones are Dark's to
// turn. On Light the rest come from Fusion's own light palette.
inline QPalette turnedPalette(const QPalette &dark, Look to = look()) {
  if (to == Look::Dark) return dark;
  QPalette p = dark;
  if (to == Look::Light)
    if (QStyle *fusion = QStyleFactory::create("Fusion")) {
      p = fusion->standardPalette();
      delete fusion;
    }
  for (int group = 0; group < QPalette::NColorGroups; ++group)
    for (int role = 0; role < QPalette::NColorRoles; ++role) {
      const auto g = QPalette::ColorGroup(group);
      const auto r = QPalette::ColorRole(role);
      if (dark.isBrushSet(g, r)) p.setColor(g, r, turned(dark.color(g, r), to));
    }
  return p;
}
// The application's own stylesheet stays Dark's: setting it again restyles
// every widget there is (3.7 s with the whole window up). The main window
// carries the same rules first in its own stylesheet (appDarkStyle()), and
// that one is turned.
inline void setAppStyle(const QString &dark) {
  appDarkStyle() = dark;
  qApp->setStyleSheet(dark);
}
inline void setAppPalette(const QPalette &dark) {
  appDarkPalette() = dark;
  appPaletteKept() = true;
  QApplication::setPalette(turnedPalette(dark));
}

// What else must hear of a new look: panels that keep pictures of themselves.
inline std::vector<std::function<void()>> &listeners() {
  static std::vector<std::function<void()>> all;
  return all;
}

inline Look savedLook() {
  return lookNamed(QSettings("Wavy-Flo", "Wavy-Flo").value("theme", "Dark").toString());
}

// Everything in the look asked for: the palette, every shown widget's own
// stylesheet (a hidden one when it is shown), and every painted panel.
inline void apply(Look to) {
  look() = to;
  if (!QCoreApplication::instance() || !qApp) return;
  if (appPaletteKept()) QApplication::setPalette(turnedPalette(appDarkPalette()));
  for (QWidget *w : QApplication::allWidgets())
    if (w->isVisible() && (w->property("wfTurnedLook").isValid() || !w->styleSheet().isEmpty())) restyle(w);
  for (const auto &listener : listeners()) listener();
  for (QWidget *w : QApplication::allWidgets())
    if (w->isVisible()) w->update();
}
}  // namespace ui_theme
