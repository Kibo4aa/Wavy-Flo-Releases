#pragma once
#include "wavy_plugin.h"
#include "effect_graph.h"
#include <QHash>
#include <QMutex>

// A measurement belongs to a workload (plugin, depth and size class), not to
// the whole application. All timings include dispatch and GPU transfers.
struct PluginRouteCost {
  double wholeNsPerPixel = 0, cpu = 0, gpu = 0;
  int cpuSamples = 0, sinceProbe = 0, retryAfter = 0;
  bool chooseGpu(bool available);
  void note(bool card, double msPerMP);
};

// Returns a diagnostic on mismatch. Empty means these validation cases agree;
// this is a regression guard, not proof for every possible input to native code.
QString validatePluginShader(const WavyEffectV4 &, const WavyShaderV1 &, int depth);

class PluginRuntime {
  QMutex guard, serialCalls;
  QHash<QByteArray, PluginRouteCost> costs;
  QHash<QByteArray, QString> validation;
public:
  int processPixels(const void *identity, const WavyParallelV1 *, QImage &, const PluginPixelCall &);
  int process(const void *identity, const WavyEffectV4 &, const WavyShaderV1 *,
              const WavyParallelV1 *, QImage &, const QVector<float> &, bool allowGpu);
};
int checkPluginRuntime(QStringList &, bool gpu);
int benchmarkPluginRuntime(QStringList &);
