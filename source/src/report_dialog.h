#pragma once
// Help > Report a Problem: what went wrong, in the user's own words, with
// pictures if they like and the crash report and log if they agree, sent to
// the Wavy-Flo Discord server's report channel through its webhook.
#include <QImage>
#include <QString>

class QWidget;

// `preview` is the picture in the viewer now (offered as an attachment);
// `graphics` says what draws it (the card's name, or the processor).
void showReportDialog(QWidget *parent, const QImage &preview, const QString &graphics);
// Whether this build was made with a report channel to send to.
bool reportsConfigured();
