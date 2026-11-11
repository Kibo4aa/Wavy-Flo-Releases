// Glass Wobble: an SDK 10 pack whose effect is a shader and nothing else.
//
// There is no process() here and no threading code. Wavy-Flo runs the shader
// on the graphics card and, compiled for the processor from the same text, on
// every core where there is no card -- the same picture either way. What is
// left in C is what the host asks about the values without drawing: whether
// the effect does anything (inert), whether it moves with time (varies), and
// how far it carries a pixel (reach). An effect written as text (.wfxs, see
// EFFECT-TEXT.md) says even those as expressions and needs no compiler at all.
#include "wavy_effect.h"
#include <cstddef>

namespace {
const WfxParameter parameters[] = {
    {sizeof(WfxParameter), WFX_FLOAT, "amount", "Amount", 0, 100, 12, 0.5f, nullptr, " px", nullptr, 0, {}},
    {sizeof(WfxParameter), WFX_FLOAT, "size", "Size", 4, 400, 60, 1, nullptr, " px", nullptr, 0, {}},
    {sizeof(WfxParameter), WFX_FLOAT, "speed", "Speed", 0, 10, 1, 0.1f, nullptr, nullptr, nullptr, 0, {}},
    {sizeof(WfxParameter), WFX_FLOAT, "seed", "Seed", 0, 1000, 1, 1, nullptr, nullptr, nullptr, WFX_PARAM_STILL, {}},
    {sizeof(WfxParameter), WFX_BOOL, "edges", "Hold Edges", 0, 1, 1, 1, nullptr, nullptr, nullptr, 0, {}},
};
const char *const help[] = {
    "How far the glass pushes the picture, in layer pixels.",
    "How big the ripples in the glass are.",
    "How fast the glass moves. 0 holds it still.",
    "Another seed, another sheet of glass.",
    "Repeat the layer's edge where the glass looks past it, instead of leaving it clear.",
};

// The parameters reach the shader by name (param_amount, ...), and
// wavyNoise/wavySample are the host's: they give the same numbers on every
// card and on the processor.
const char *shader = R"(
vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size) {
  float cell = max(param_size * wavyScale, 1.0);
  float time = wavySeconds * param_speed;
  vec2 p = coord / cell + vec2(time * 0.37, time * 0.21);
  // Two noises a little apart give the push across and down.
  vec2 push = vec2(wavyNoise(p, param_seed), wavyNoise(p + vec2(17.3, 5.1), param_seed)) - 0.5;
  vec2 from = coord + push * 2.0 * param_amount * wavyScale;
  return param_edges ? wavySampleEdge(from) : wavySample(from);
}
)";

int inert(const float *values, int32_t count) { return count < 1 || values[0] <= 0; }
int varies(const float *values, int32_t count) { return count > 2 && values[2] > 0; }
double reach(const float *values, int32_t count, double scale) {
  // Rounded up to whole tens, so dragging Amount does not resize the layer's raster every step.
  const double amount = count > 0 ? values[0] * scale : 0;
  return amount <= 0 ? 0 : 10 * (int(amount / 10) + 1);
}

WfxEffect effect() {
  WfxEffect e{};
  e.structSize = sizeof(WfxEffect);
  e.abiVersion = WFX_ABI_VERSION;
  e.id = "Glass Wobble";
  e.name = "Glass Wobble";
  e.category = "Noise & Distort";
  e.supportedDepths = 4u;
  e.flags = WFX_MOVES_ALONE;
  e.parameterCount = int32_t(sizeof(parameters) / sizeof(parameters[0]));
  e.parameters = parameters;
  e.process = nullptr; // the shader is the effect
  e.inert = inert;
  e.varies = varies;
  e.reach = reach;
  e.shader = shader;
  e.help = help;
  return e;
}
const WfxEffect glassWobble = effect();
}  // namespace

WFX_EXPORT int32_t wfxEffectCount(void) { return 1; }
WFX_EXPORT const WfxEffect *wfxEffectAt(int32_t index) { return index == 0 ? &glassWobble : nullptr; }
