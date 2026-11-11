#pragma once
#include <QColor>
#include <QStringList>
#include <functional>
class QWidget;

// A colour chooser that belongs to this application rather than to the
// platform: the same dark palette as every other panel, alpha where it is
// wanted, a hex field, and a screen eyedropper. Returns an invalid QColor when
// the user cancels, so call sites keep the QColorDialog::getColor shape.
//
// live, when given, is called with the colour while the dialog is open, so the
// edit can be seen as it is made. Calls are coalesced to at most one per pass
// of the event loop, and only for a colour that differs from the last one
// sent. Undoing a preview on Cancel is the caller's business.
QColor pickColor(QWidget *parent, QColor initial, const QString &title, bool alpha = true,
                 const std::function<void(const QColor &)> &live = {});

// Hex text in, colour out. Accepts #rgb, #rrggbb and #rrggbbaa, with or
// without the hash. Invalid text gives an invalid colour.
QColor colorFromHexText(QString text);
QString hexTextFromColor(QColor color, bool alpha);

int checkColorPicker(QStringList &failures);
