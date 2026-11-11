#pragma once
#include <stdint.h>
#ifdef _WIN32
#define WAVY_EXPORT extern "C" __declspec(dllexport)
#else
#define WAVY_EXPORT extern "C" __attribute__((visibility("default")))
#endif
// ABI 1: synchronous in-place RGBA8 straight-alpha. Return 0 on success.
// Host retains memory. Do not store pixels after returning; honor row stride.
struct WavyEffectV1 {
  uint32_t abiVersion;
  const char *name;
  int (*available)();
  int (*process)(uint8_t *rgba, int width, int height, int stride,
                 float amount);
};
using WavyGetEffect = const WavyEffectV1 *(*)();
WAVY_EXPORT const WavyEffectV1 *wavyGetEffectV1();

// Optional extension: a plugin still exports V1 for discovery and 8-bit hosts.
// V2 is synchronous straight-alpha RGBA; stride is bytes, pixels owned by host.
// Supported depths are 16 (four uint16_t channels, 0..65535) and 32 (four
// IEEE float channels, RGB may exceed 1; alpha 0..1). Native endian. No retention.
struct WavyEffectV2 {
  uint32_t abiVersion;
  uint32_t supportedDepths; // bit 1 = RGBA16, bit 2 = RGBA32F
  int (*process)(void *rgba, int width, int height, int stride, int depth, float amount);
};
using WavyGetEffectV2 = const WavyEffectV2 *(*)();

// Optional: how this plugin may be spread across CPU cores. Export nothing and
// the host calls process() once with the whole image on one thread, exactly as
// before -- declaring nothing is always safe, only slower.
//
// When it is declared, the host cuts the image into horizontal bands and calls
// process() once per band on a different thread, each with a plain pointer,
// width, height and stride. The plugin is not told it happened and needs no
// threading code of its own.
struct WavyParallelV1 {
  uint32_t structSize;
  uint32_t abiVersion; // 1
  // Nonzero if process() may run on several threads at once: no mutable
  // globals, no shared scratch, nothing carried between calls.
  int32_t reentrant;
  // Rows above and below the row being written that process() reads. 0 means an
  // output pixel depends only on the input pixel in the same place, which is
  // true of nearly all colour work; a vertical blur of radius 8 declares 8.
  // Only rows matter -- a band is always the full width of the image.
  //
  // Declaring more than 0 carries one obligation: process() must not let its
  // own writes change its own reads. Copy the input first and write into the
  // host buffer from that copy. A filter that reads pixels it has already
  // overwritten gives a different answer depending on where a band begins, and
  // the seams will show.
  int32_t rowReach;
};
using WavyGetParallelV1 = const WavyParallelV1 *(*)();

// V4: controls. Until this existed an effect was a button that did one fixed
// thing, because the only value reaching process() was an amount the host
// always set to 1. A V4 plugin declares what it wants adjusted; the host draws
// the controls in its own panel, in its own style, remembers the values with
// the project, and hands them back as plain floats. The plugin draws no UI and
// links against no toolkit.
enum WavyParameterKind : uint32_t {
  WAVY_FLOAT = 1,  // one float, clamped to [minimum, maximum]
  WAVY_BOOL = 2,   // one float, 0 or 1
  WAVY_CHOICE = 3, // one float, 0 .. number of choices - 1, whole numbers
  WAVY_COLOR = 4   // four floats: red, green, blue, alpha, each 0..1
};
struct WavyParameter {
  uint32_t structSize;
  uint32_t kind;
  const char *id;      // stable ASCII, unique within the plugin, saved in the project
  const char *label;   // UTF-8, what the person reads
  float minimum, maximum, initial, step;
  const char *choices; // WAVY_CHOICE only: labels separated by newlines
};
// How many floats a parameter occupies. Everything is one except a colour.
static inline int wavyParameterFloats(const WavyParameter *p) {
  return p && p->kind == WAVY_COLOR ? 4 : 1;
}
struct WavyEffectV4 {
  uint32_t structSize, abiVersion; // 4
  uint32_t supportedDepths;        // bit 1 = RGBA8, bit 2 = RGBA16, bit 4 = RGBA32F
  int32_t parameterCount;
  const WavyParameter *parameters; // parameterCount entries, alive until unload
  // values holds the declared parameters in order, a colour taking four floats
  // and everything else one, so valueCount is the sum. The host guarantees
  // both the count and that every value is inside the range that was declared.
  int (*process)(void *pixels, int width, int height, int stride, int depth,
                 const float *values, int32_t valueCount);
};
using WavyGetEffectV4 = const WavyEffectV4 *(*)();
WAVY_EXPORT const WavyEffectV4 *wavyGetEffectV4();

// Optional: the same per-pixel maths written for the graphics card. You supply
// a GLSL function; the host compiles it into its own pass chain, so the effect
// runs on the card without your plugin owning a context, a texture, a shader
// program or one line of OpenGL.
//
// process() stays the truth. The shader is an optimisation the host takes only
// when a card is present, the snippet compiles, and -- this is the part worth
// reading twice -- the host has checked the two agree. Before it trusts a
// shader it compares test patches at defaults and parameter boundaries for
// each requested depth. A mismatch disables that depth for this session.
// This catches regressions; testing finite patches cannot prove equivalence
// for every image or combination of settings. Authors must keep both versions
// equivalent and test their own parameter combinations and HDR behaviour.
struct WavyShaderV1 {
  uint32_t structSize;
  uint32_t abiVersion; // 1
  // GLSL 3.30 core defining exactly:
  //   vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size)
  // colour is straight alpha. coord.xy is the pixel centre (x+.5, row+.5),
  // with row 0 matching the CPU buffer's first/top row; size is the raster.
  // The host supplies processingDepth (8, 16 or 32); HDR RGB may exceed 1 or
  // be negative at 32 bpc. Your parameters arrive in values[256], in the
  // same order and the same packing process() receives them, a colour taking
  // four, up to 64 parameters / 256 packed floats. Declare no uniforms, no
  // version and no outputs: the host writes
  // everything around your function.
  const char *source;
};
using WavyGetShaderV1 = const WavyShaderV1 *(*)();
WAVY_EXPORT const WavyShaderV1 *wavyGetShaderV1();
WAVY_EXPORT const WavyParallelV1 *wavyGetParallelV1();
