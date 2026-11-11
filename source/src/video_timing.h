#pragma once
#include "engine.h"
#include <algorithm>
#include <cmath>

struct VideoTiming {
  qint64 positionMs;
  bool active;
};

// The footage's own time, in seconds, at a layer frame: straight on from the
// in-point, or wherever Time Remap says.
inline double videoSourceSeconds(const Layer &layer, double layerFrame, int fps) {
  if (layer.timeRemap) return layer.p[TimeRemap].at(layerFrame);
  return layer.sourceOffsetMs / 1000. + layerFrame / std::max(1, fps);
}

inline VideoTiming videoTiming(const Layer &layer, double frame, int fps) {
  const double local = frame - layer.start;
  if (layer.timeRemap) {
    // Remapped time before the footage begins or after it ends holds the first
    // or last picture: a freeze, not a hole in the layer.
    const bool inLayer = layer.visible && local >= 0 && local < layer.duration;
    const double wanted = videoSourceSeconds(layer, local, fps) * 1000.;
    const double last = layer.sourceDurationMs > 0 ? layer.sourceDurationMs - 1 : std::max(0., wanted);
    return {qint64(std::clamp(wanted, 0., std::max(0., last))), inLayer};
  }
  const double source = layer.sourceOffsetMs + local * 1000. / std::max(1, fps);
  const bool active = layer.visible && local >= 0 && local < layer.duration &&
      source >= 0 && (layer.sourceDurationMs <= 0 || source < layer.sourceDurationMs);
  const double limit = layer.sourceDurationMs > 0 ? layer.sourceDurationMs - 1 : source;
  return {qint64(std::max(0., std::min(source, limit))), active};
}

inline void setVideoSourceDuration(Layer &layer, qint64 milliseconds, int fps) {
  if (milliseconds <= 0) return;
  layer.sourceDurationMs = double(milliseconds);
  if (layer.pendingVideoDuration) {
    layer.duration = std::max(1., std::ceil(
        (layer.sourceDurationMs - layer.sourceOffsetMs) * std::max(1, fps) / 1000.));
    layer.pendingVideoDuration = false;
  }
}
