#include "wavy_plugin.h"
#include <algorithm>
#include <cstring>

// A sample plugin with controls. It declares what it wants adjusted and the
// host draws the panel, remembers the values with the project and hands them
// back as plain floats. There is no toolkit here, no drawing code, and no
// threading: the parallel declaration at the bottom is what gets it every core.

static const WavyParameter kParameters[] = {
    {sizeof(WavyParameter), WAVY_COLOR, "tint", "Tint", 0, 1, 1, 0, nullptr},
    {sizeof(WavyParameter), WAVY_FLOAT, "amount", "Amount", 0, 1, .5f, .01f, nullptr},
    {sizeof(WavyParameter), WAVY_CHOICE, "mode", "Mode", 0, 2, 0, 1,
     "Mix\nMultiply\nScreen"},
    {sizeof(WavyParameter), WAVY_BOOL, "preserveLuma", "Keep brightness", 0, 1, 1, 1,
     nullptr},
};

static int available() { return 1; }

// Straight alpha, so the colour channels stand on their own and nothing has to
// be divided out first.
static float blend(float source, float tint, int mode) {
  if (mode == 1) return source * tint;
  if (mode == 2) return 1 - (1 - source) * (1 - tint);
  return tint;
}

static int process(void *pixels, int width, int height, int stride, int depth,
                   const float *values, int32_t valueCount) {
  if (!pixels || width < 1 || height < 1 || !values || valueCount < 7) return 1;
  const float tint[3] = {std::clamp(values[0], 0.f, 1.f), std::clamp(values[1], 0.f, 1.f),
                         std::clamp(values[2], 0.f, 1.f)};
  const float strength = std::clamp(values[3] * values[4], 0.f, 1.f); // alpha * amount
  const int mode = int(values[5] + .5f);
  const bool keepLuma = values[6] >= .5f;
  const int bytes = depth == 8 ? 1 : depth == 16 ? 2 : 4;
  if (stride < width * 4 * bytes) return 1;

  for (int y = 0; y < height; ++y) {
    auto *row = static_cast<uint8_t *>(pixels) + size_t(y) * stride;
    for (int x = 0; x < width; ++x) {
      float channel[3];
      for (int c = 0; c < 3; ++c) {
        if (depth == 8) channel[c] = reinterpret_cast<uint8_t *>(row)[x * 4 + c] / 255.f;
        else if (depth == 16) channel[c] = reinterpret_cast<uint16_t *>(row)[x * 4 + c] / 65535.f;
        else channel[c] = reinterpret_cast<float *>(row)[x * 4 + c];
      }
      const float before = .2126f * channel[0] + .7152f * channel[1] + .0722f * channel[2];
      for (int c = 0; c < 3; ++c)
        channel[c] = channel[c] * (1 - strength) + blend(channel[c], tint[c], mode) * strength;
      if (keepLuma) {
        const float after = .2126f * channel[0] + .7152f * channel[1] + .0722f * channel[2];
        if (after > 1e-5f)
          for (int c = 0; c < 3; ++c) channel[c] *= before / after;
      }
      for (int c = 0; c < 3; ++c) {
        // Only the 32-bit path may exceed one: the others have nowhere to put it.
        if (depth == 8)
          reinterpret_cast<uint8_t *>(row)[x * 4 + c] =
              uint8_t(std::clamp(channel[c], 0.f, 1.f) * 255 + .5f);
        else if (depth == 16)
          reinterpret_cast<uint16_t *>(row)[x * 4 + c] =
              uint16_t(std::clamp(channel[c], 0.f, 1.f) * 65535 + .5f);
        else reinterpret_cast<float *>(row)[x * 4 + c] = std::max(0.f, channel[c]);
      }
    }
  }
  return 0;
}

WAVY_EXPORT const WavyEffectV4 *wavyGetEffectV4() {
  static WavyEffectV4 api{sizeof(WavyEffectV4), 4, 1 | 2 | 4,
                           int32_t(sizeof(kParameters) / sizeof(kParameters[0])), kParameters,
                           process};
  return &api;
}

// V1 is still what the host discovers a plugin by, and what an older host would
// call. It runs the same code at the values the plugin was born with.
static int processV1(uint8_t *rgba, int width, int height, int stride, float amount) {
  const float fallback[7] = {1, 1, 1, 1, std::clamp(amount, 0.f, 1.f) * .5f, 0, 1};
  return process(rgba, width, height, stride, 8, fallback, 7);
}
WAVY_EXPORT const WavyEffectV1 *wavyGetEffectV1() {
  static WavyEffectV1 api{1, "Tint (SDK controls sample)", available, processV1};
  return &api;
}

// The same maths again, for the card. Deliberately written line for line
// against process() above rather than cleverly: the host checks the two agree
// before it trusts this, and a shader that drifts is simply dropped, so there
// is nothing to gain by being clever and a working effect to lose.
static const char *kShader = R"(
vec3 blendAt(vec3 source, vec3 tint, int mode) {
  if (mode == 1) return source * tint;
  if (mode == 2) return vec3(1.0) - (vec3(1.0) - source) * (vec3(1.0) - tint);
  return tint;
}
vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size) {
  vec3 tint = clamp(vec3(values[0], values[1], values[2]), 0.0, 1.0);
  float strength = clamp(values[3] * values[4], 0.0, 1.0);
  int mode = int(values[5] + 0.5);
  bool keepLuma = values[6] >= 0.5;
  vec3 channel = colour.rgb;
  float before = dot(channel, vec3(0.2126, 0.7152, 0.0722));
  channel = mix(channel, blendAt(channel, tint, mode), strength);
  if (keepLuma) {
    float after = dot(channel, vec3(0.2126, 0.7152, 0.0722));
    if (after > 1e-5) channel *= before / after;
  }
  return vec4(processingDepth == 32 ? max(channel, vec3(0.0))
                                    : clamp(channel, 0.0, 1.0), colour.a);
}
)";
WAVY_EXPORT const WavyShaderV1 *wavyGetShaderV1() {
  static WavyShaderV1 api{sizeof(WavyShaderV1), 1, kShader};
  return &api;
}

// Pointwise and holding nothing between calls, so the host spreads it across
// every core -- if it measures that doing so is worth it.
WAVY_EXPORT const WavyParallelV1 *wavyGetParallelV1() {
  static WavyParallelV1 split{sizeof(WavyParallelV1), 1, 1, 0};
  return &split;
}
