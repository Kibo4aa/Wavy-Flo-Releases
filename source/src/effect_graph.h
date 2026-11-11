#pragma once
#include "wavy_graph.h"
#include <QImage>
#include <QStringList>
#include <functional>
int executeEffectGraph(const WavyEffectGraphV3 *, QImage &, bool gpu, float amount=1);

// One call into a plugin: a pointer, a size and a stride, nothing else.
using PluginPixelCall = std::function<int(void *pixels, int width, int height, int stride)>;
// Runs a plugin's pixel function over the whole image on as many cores as the
// plugin said were safe, and on one if it said nothing. The plugin sees a plain
// image either way, so no plugin has to contain threading code to get the
// machine it is running on. Returns the plugin's own result: zero for success.
// cost, when given, is what this plugin took per pixel the last time it ran
// whole, in nanoseconds; the caller keeps one per plugin and it starts at zero.
// It is how the host learns whether a plugin is worth spreading at all: an
// effect that costs a millisecond for an entire frame loses more to the
// threading than it could ever win back, and measuring beats guessing.
int runPluginAcrossCores(QImage &, bool reentrant, int rowReach, const PluginPixelCall &,
                         double *cost = nullptr);
int checkEffectGraph(QStringList &);
int checkEffectGraphGpu(QStringList &);
