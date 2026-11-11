#pragma once
// The preview's sound for a layer whose time is remapped. The media player
// plays forward at one speed only, so this decodes the footage's sound once,
// on a thread of its own, and plays it along the Time Remap curve itself --
// slowed, sped up, backwards, silent while held -- pulled by an audio sink as
// the preview plays. The samples are the ones export writes.
#include "engine.h"
#include <QIODevice>
#include <future>
#include <memory>
#include <vector>

class RemapSound : public QIODevice {
public:
  explicit RemapSound(QObject *parent = nullptr);
  // Decodes this footage's sound (16-bit stereo, 48 kHz) in the background.
  void load(const QString &path);
  // The sound as it is, for tests.
  void setPcm(std::vector<int16_t> pcm);
  // Whether the sound has been decoded (and there was any).
  bool ready();
  QString path() const { return source; }
  // Plays from composition frame `compFrame` on, following `layer`, round the
  // composition's `sceneFrames` the way playback loops.
  void begin(const Layer &layer, int fps, int sceneFrames, double compFrame);
  bool isSequential() const override { return true; }
  qint64 bytesAvailable() const override { return QIODevice::bytesAvailable() + (1 << 16); }

protected:
  qint64 readData(char *data, qint64 maxSize) override;
  qint64 writeData(const char *, qint64) override { return -1; }

private:
  QString source;
  std::shared_ptr<const std::vector<int16_t>> pcm;
  std::future<std::shared_ptr<const std::vector<int16_t>>> decoding;
  Layer layer;
  int fps = 30, frames = 0;
  qint64 nextSample = 0; // composition sample the next read starts at
};
