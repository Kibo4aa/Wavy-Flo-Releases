#pragma once
#include "engine.h"
#include <functional>

// These are editor properties rather than transient dialog values.  Keeping
// them outside renderExport also makes the settings visible before a render is
// started and gives every entry point (menu and Properties panel) one source of
// truth.
struct RenderSettings {
  int format = 0; // MP4, PNG sequence, MOV Animation, MOV PNG, MOV ProRes 4444
  // 8, 16 or 32 bits per channel; 0 the project's own (Project Settings),
  // as After Effects' "Current Settings".
  int processingDepth = 0;
  int bitrateMbps = 50;
  bool includeAudio = true;
  bool preserveTransparency = false;
  QString ffmpegPath;
  // The frames rendered, both included; -1 is the Main's last. The output
  // starts at its own frame 0 either way, its sound in step with its picture.
  int firstFrame = 0, lastFrame = -1;
};

QStringList renderFormatNames();
// Opens the Render Composition settings dialog, then renders when accepted.
void renderExport(QWidget *parent, Scene scene,
                  std::function<QImage(Scene &)> render);
void renderExport(QWidget *parent, Scene scene, const RenderSettings &settings,
                  std::function<QImage(Scene &)> render);
// The render itself, with no dialogs: what the Render menu runs after asking
// where, and what a render queue runs item after item. `progress(done, total,
// label)` hears how far it has got and returns false to stop. Returns why it
// failed ("Canceled" when stopped), or nothing when the file is written.
// `target` is the movie's path, or the folder a PNG sequence becomes.
QString renderToFile(Scene scene, const RenderSettings &settings, const QString &target,
                     std::function<QImage(Scene &)> render,
                     const std::function<bool(int, int, const QString &)> &progress, bool checkFFmpeg = true);
