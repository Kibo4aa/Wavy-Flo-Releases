#pragma once
// Window > UI Scaling, done the way Windows changes a display's scale: Qt's own
// high-DPI factor on every screen, and every window told its new pixel ratio
// at once. Everything Wavy-Flo draws -- text, icons, stylesheets, painted
// panels, fixed sizes -- is in device-independent pixels, so all of it grows
// together, live, and nothing has to be laid out twice.
//
// Before, the scale was written by hand into the window's stylesheet, the
// toolbar, the timeline and the panel titles; everything else (the Project and
// Character panels, the timeline's rows and words, hundreds of fixed sizes)
// stayed at 100%, and at 200% the window was half grown.
//
// The factor goes on top of the display's own scale (a 150% display at 200%
// is 300%), the way Qt's QT_SCALE_FACTOR does, but it can change while the
// program runs: QHighDpiScaling's per-screen factor, then each window's pixel
// ratio and geometry brought up to date -- what Qt does itself when a window
// moves to a monitor with another scale.
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QtGui/private/qguiapplication_p.h>
#include <QtGui/private/qhighdpiscaling_p.h>
#include <QtGui/private/qwindow_p.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qwindowsysteminterface.h>

namespace ui_scale {
inline qreal &factor() {
  static qreal current = 1.0;
  return current;
}

// Every window on the screens: its pixel ratio from the new factor, and its
// geometry read again from the system's (the same physical window, now fewer
// device-independent pixels wide), so its widgets lay themselves out again.
inline void refreshWindows() {
  QGuiApplicationPrivate::resetCachedDevicePixelRatio();
  for (QWindow *window : QGuiApplication::allWindows()) {
    QWindowPrivate::get(window)->updateDevicePixelRatio();
    if (QPlatformWindow *platform = window->handle())
      QWindowSystemInterface::handleGeometryChange<QWindowSystemInterface::SynchronousDelivery>(window,
                                                                                             platform->geometry());
  }
}

// The scale for every screen, now and for a monitor plugged in later.
inline void apply(qreal wanted) {
  if (!qGuiApp || wanted <= 0) return;
  static bool watching = false;
  if (!watching) {
    watching = true;
    QObject::connect(qGuiApp, &QGuiApplication::screenAdded, qGuiApp, [](QScreen *screen) {
      QHighDpiScaling::setScreenFactor(screen, factor());
      refreshWindows();
    });
  }
  if (qFuzzyCompare(wanted, factor())) return;
  factor() = wanted;
  for (QScreen *screen : QGuiApplication::screens()) QHighDpiScaling::setScreenFactor(screen, wanted);
  refreshWindows();
}
}  // namespace ui_scale
