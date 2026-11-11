#pragma once
#include <stdint.h>
// V3 extension to V1 discovery. C-layout, no Qt/STL or device handles cross ABI.
// All RGB values use the source working space, straight alpha. Alpha is unchanged.
enum WavyOperationCode : uint32_t {
  WAVY_RGB_AFFINE = 1, // rgb = rgb * values[0..2] + values[3..5]
  WAVY_EXPOSURE = 2,   // stops = values[0]
  WAVY_INVERT = 3,     // mix amount values[0], 0..1; HDR is not clamped
  WAVY_BLUR = 4,       // radius [0], exact three separable box passes
  WAVY_HALO = 5,       // radius [0], RGB [1..3], alpha [4], integer offset XY [5..6]
  WAVY_STROKE = 6,     // width [0], mode [1] (inside=0,outside=1,center=2), RGBA [2..5]
  WAVY_FILL = 7,       // RGBA [0..3], replaces RGB, multiplies existing alpha
  WAVY_GRADIENT = 8    // start RGB [0..2], end RGB [3..5], amount [6], angle [7],
                      // radial [8] (0/1), centre XY [9..10], extent WH [11..12]
};
struct WavyOperation { uint32_t structSize, code; float values[16]; };
struct WavyGraphRequest {
  uint32_t structSize, abiVersion;
  int32_t width, height, depth;
  float amount;
};
struct WavyGraphSink {
  uint32_t structSize;
  void *context;
  // Host copies op immediately. Nonzero = invalid/unsupported/full. Stop on error.
  int (*append)(void *context, const WavyOperation *op);
};
struct WavyEffectGraphV3 {
  uint32_t structSize, abiVersion; // 3
  // Pure deterministic description, no pixel access. Return 0 on success.
  // Request/sink pointers are valid only during this call. Do not retain them.
  int (*describe)(const WavyGraphRequest *, const WavyGraphSink *);
  const char *name = nullptr; // required for V3-only discovery; static UTF-8
};
using WavyGetEffectGraphV3 = const WavyEffectGraphV3 *(*)();
// Export wavyGetEffectGraphV3 alongside wavyGetEffectV1. Host prefers V3.
