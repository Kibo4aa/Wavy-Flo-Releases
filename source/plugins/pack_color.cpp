// Wavy-Color.wfx: the colour-correction effects. Every one of them treats each
// pixel on its own, so each offers a span() the host can run several of in one
// pass, and a shader body for the graphics card.
#include "wavy_effect.h"
#include "pack_common.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <new>

using namespace wfxpack;

namespace {
// ---- shared colour maths ---------------------------------------------------
struct Rgb { float r = 0, g = 0, b = 0; };
struct Hsl { float h = 0, s = 0, l = 0; };

Hsl rgbToHsl(Rgb c) {
  c.r = std::clamp(c.r, 0.f, 1.f); c.g = std::clamp(c.g, 0.f, 1.f); c.b = std::clamp(c.b, 0.f, 1.f);
  const float high = std::max({c.r, c.g, c.b}), low = std::min({c.r, c.g, c.b});
  const float d = high - low, l = (high + low) * .5f;
  if (d < 1e-7f) return {0, 0, l};
  // (g-b)/d lies within -1..1 when red is highest, so the textbook fmod(.., 6)
  // is that value itself, without the cost of an fmod on every pixel.
  float h = high == c.r ? (c.g - c.b) / d : high == c.g ? (c.b - c.r) / d + 2.f : (c.r - c.g) / d + 4.f;
  h /= 6.f; if (h < 0) h += 1.f;
  return {h, d / (1.f - std::abs(2.f * l - 1.f)), l};
}
Rgb hslToRgb(Hsl h) {
  h.h -= std::floor(h.h); h.s = std::clamp(h.s, 0.f, 1.f); h.l = std::clamp(h.l, 0.f, 1.f);
  if (h.s < 1e-7f) return {h.l, h.l, h.l};
  const float q = h.l < .5f ? h.l * (1 + h.s) : h.l + h.s - h.l * h.s, p = 2 * h.l - q;
  // One sixth of the hue circle at a time: in each, one channel is at the top,
  // one at the bottom and one on its way between.
  const float scaled = h.h * 6; const int sector = std::min(5, int(scaled)); const float f = scaled - float(sector);
  const float rising = p + (q - p) * f, falling = p + (q - p) * (1 - f);
  switch (sector) {
    case 0: return {q, rising, p};
    case 1: return {falling, q, p};
    case 2: return {p, q, rising};
    case 3: return {p, falling, q};
    case 4: return {rising, p, q};
    default: return {q, p, falling};
  }
}
float luma(Rgb c) { return c.r * .2126f + c.g * .7152f + c.b * .0722f; }

// Runs a per-pixel colour function over straight float RGBA, skipping pixels
// nobody can see.
template <class F> void gradeSpan(float *p, int32_t count, const F &f) {
  for (int32_t x = 0; x < count; ++x) {
    if (p[x * 4 + 3] <= 0.f) continue;
    const Rgb graded = f(Rgb{p[x * 4], p[x * 4 + 1], p[x * 4 + 2]});
    p[x * 4] = graded.r; p[x * 4 + 1] = graded.g; p[x * 4 + 2] = graded.b;
  }
}

// One channel's curve, looked up rather than worked out: 4096 steps across
// 0..1, read between with a straight line; worked out exactly outside 0..1 and
// below 1/64, where a steep power curve would not survive the straight line.
struct PowerTable {
  float exponent = 1;
  std::array<float, 4097> values{};
  explicit PowerTable(float e) : exponent(e) {
    for (int i = 0; i <= 4096; ++i) values[size_t(i)] = std::pow(std::max(0.f, float(i) / 4096.f), exponent);
  }
  float operator()(float x) const {
    if (!(x >= 1.f / 64 && x <= 1.f)) return std::pow(std::max(0.f, x), exponent);
    const float at = x * 4096.f; const int i = std::min(4095, int(at)); const float t = at - float(i);
    return values[size_t(i)] + (values[size_t(i) + 1] - values[size_t(i)]) * t;
  }
};

// The shader helpers every body here may use.
#define WFX_GLSL_HELPERS                                                                                    \
  "vec3 rgb2hsl(vec3 c){float hi=max(c.r,max(c.g,c.b)),lo=min(c.r,min(c.g,c.b));float d=hi-lo,l=(hi+lo)*.5;" \
  "if(d<1e-7)return vec3(0,0,l);float h=hi==c.r?mod((c.g-c.b)/d,6.):hi==c.g?(c.b-c.r)/d+2.:(c.r-c.g)/d+4.;" \
  "return vec3(h/6.,d/max(1e-7,1.-abs(2.*l-1.)),l);}"                                                      \
  "float huepart(float p,float q,float t){if(t<0.)t+=1.;if(t>1.)t-=1.;if(t<1./6.)return p+(q-p)*6.*t;"      \
  "if(t<.5)return q;if(t<2./3.)return p+(q-p)*(2./3.-t)*6.;return p;}"                                       \
  "vec3 hsl2rgb(vec3 h){h.x=fract(h.x);h.yz=clamp(h.yz,0.,1.);if(h.y<1e-7)return vec3(h.z);"               \
  "float q=h.z<.5?h.z*(1.+h.y):h.z+h.y-h.z*h.y,p=2.*h.z-q;"                                                  \
  "return vec3(huepart(p,q,h.x+1./3.),huepart(p,q,h.x),huepart(p,q,h.x-1./3.));}"                             \
  "float luma(vec3 c){return dot(c,vec3(.2126,.7152,.0722));}"

// Every effect here: prepare() makes the state, span() applies it, process()
// runs span() over rows on the host's workers.
template <class State> int processRows(void *pixels, int32_t width, int32_t height, int32_t stride,
                                       const float *values, int32_t count, const WfxContext *context,
                                       void *(*prepare)(const float *, int32_t, const WfxContext *),
                                       void (*span)(float *, int32_t, const void *)) {
  if (!context || context->depth != 32) return 1;
  State *state = static_cast<State *>(prepare(values, count, context));
  if (!state) return 0;
  struct Job { unsigned char *bits; int32_t stride, width; const void *state; void (*span)(float *, int32_t, const void *); } job{
      static_cast<unsigned char *>(pixels), stride, width, state, span};
  forRows(context, height, &job, [](int32_t begin, int32_t end, void *user) {
    auto *j = static_cast<Job *>(user);
    for (int32_t y = begin; y < end; ++y) j->span(reinterpret_cast<float *>(j->bits + size_t(y) * size_t(j->stride)), j->width, j->state);
  });
  delete state;
  return 0;
}
#define WFX_GRADE_ENTRY(State, name)                                                                                   \
  void *name##Prepare(const float *v, int32_t n, const WfxContext *c);                                                 \
  void name##Span(float *p, int32_t count, const void *s) { static_cast<const State *>(s)->apply(p, count); }        \
  void name##Release(void *s) { delete static_cast<State *>(s); }                                                      \
  int name##Process(void *px, int32_t w, int32_t h, int32_t stride, const float *v, int32_t n, const WfxContext *c) { \
    return processRows<State>(px, w, h, stride, v, n, c, name##Prepare, name##Span);                                  \
  }

// ---- Curves ------------------------------------------------------------------
using Curve = std::array<float, 5>;
float sampleCurve(const Curve &curve, float value) {
  value = std::clamp(value, 0.f, 1.f); const float at = value * 4;
  const int segment = std::min(3, int(at)); const float t = at - segment;
  return curve[size_t(segment)] * (1 - t) + curve[size_t(segment + 1)] * t;
}
bool identity(const Curve &curve) {
  for (int i = 0; i < 5; ++i) if (std::abs(curve[size_t(i)] - i * .25f) > 1e-6f) return false;
  return true;
}
Curve curveAt(const float *v, int32_t n, int at) {
  Curve c{0, .25f, .5f, .75f, 1};
  for (int i = 0; i < 5; ++i) c[size_t(i)] = std::clamp(value(v, n, at + i, c[size_t(i)]), 0.f, 1.f);
  return c;
}
struct CurvesState {
  Curve master, red, green, blue;
  void apply(float *p, int32_t count) const {
    gradeSpan(p, count, [&](Rgb c) {
      c = {sampleCurve(master, c.r), sampleCurve(master, c.g), sampleCurve(master, c.b)};
      return Rgb{sampleCurve(red, c.r), sampleCurve(green, c.g), sampleCurve(blue, c.b)};
    });
  }
};
WFX_GRADE_ENTRY(CurvesState, curves)
void *curvesPrepare(const float *v, int32_t n, const WfxContext *) {
  return new (std::nothrow) CurvesState{curveAt(v, n, 0), curveAt(v, n, 5), curveAt(v, n, 10), curveAt(v, n, 15)};
}
int curvesInert(const float *v, int32_t n) {
  return identity(curveAt(v, n, 0)) && identity(curveAt(v, n, 5)) && identity(curveAt(v, n, 10)) && identity(curveAt(v, n, 15));
}
const WfxParameter curvesParameters[] = {
    curve("master", "Master"), curve("red", "Red"), curve("green", "Green"), curve("blue", "Blue")};
const char *curvesShader =
    "float c5(float x,int o){x=clamp(x,0.,1.);float at=x*4.;int i=min(3,int(at));float t=at-float(i);"
    "return mix(values[o+i],values[o+i+1],t);}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){vec3 c=vec3(c5(v.r,0),c5(v.g,0),c5(v.b,0));"
    "return vec4(c5(c.r,5),c5(c.g,10),c5(c.b,15),v.a);}";

// ---- HSL -------------------------------------------------------------------
struct HslState {
  float hue, saturation, lightness;
  void apply(float *p, int32_t count) const {
    gradeSpan(p, count, [&](Rgb c) {
      auto h = rgbToHsl(c); h.h += hue;
      h.s = saturation < 0 ? h.s * (1 + saturation) : h.s + (1 - h.s) * saturation;
      h.l = lightness < 0 ? h.l * (1 + lightness) : h.l + (1 - h.l) * lightness;
      return hslToRgb(h);
    });
  }
};
WFX_GRADE_ENTRY(HslState, hsl)
HslState hslFrom(const float *v, int32_t n) {
  return {divided(v, n, 0, 0, 360.), std::clamp(divided(v, n, 1, 0, 100.), -1.f, 1.f),
          std::clamp(divided(v, n, 2, 0, 100.), -1.f, 1.f)};
}
void *hslPrepare(const float *v, int32_t n, const WfxContext *) { return new (std::nothrow) HslState(hslFrom(v, n)); }
int hslInert(const float *v, int32_t n) { const auto s = hslFrom(v, n); return s.hue == 0 && s.saturation == 0 && s.lightness == 0; }
const WfxParameter hslParameters[] = {
    number("hue", "Hue", -180, 180, 0, 1, "\xc2\xb0"), number("saturation", "Saturation", -100, 100, 0, 1, " %"),
    number("lightness", "Lightness", -100, 100, 0, 1, " %")};
const char *hslShader = WFX_GLSL_HELPERS
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){float hue=values[0]/360.,sat=clamp(values[1]/100.,-1.,1.),"
    "light=clamp(values[2]/100.,-1.,1.);vec3 h=rgb2hsl(clamp(v.rgb,0.,1.));h.x+=hue;"
    "h.y=sat<0.?h.y*(1.+sat):h.y+(1.-h.y)*sat;h.z=light<0.?h.z*(1.+light):h.z+(1.-h.z)*light;"
    "return vec4(hsl2rgb(h),v.a);}";

// ---- Levels ----------------------------------------------------------------
struct LevelsState {
  float inBlack, inWhite, gamma, outBlack, outWhite, span;
  PowerTable power;
  void apply(float *p, int32_t count) const {
    gradeSpan(p, count, [&](Rgb c) {
      auto level = [&](float v) {
        const float normalized = std::clamp((v - inBlack) / span, 0.f, 1.f);
        return outBlack + (outWhite - outBlack) * (gamma == 1 ? normalized : power(normalized));
      };
      return Rgb{level(c.r), level(c.g), level(c.b)};
    });
  }
};
WFX_GRADE_ENTRY(LevelsState, levels)
void levelsNumbers(const float *v, int32_t n, float out[5]) {
  out[0] = std::clamp(divided(v, n, 0, 0, 255.), 0.f, 1.f);
  out[1] = std::clamp(divided(v, n, 1, 255, 255.), 0.f, 1.f);
  out[2] = std::clamp(value(v, n, 2, 1), .01f, 10.f);
  out[3] = std::clamp(divided(v, n, 3, 0, 255.), 0.f, 1.f);
  out[4] = std::clamp(divided(v, n, 4, 255, 255.), 0.f, 1.f);
}
void *levelsPrepare(const float *v, int32_t n, const WfxContext *) {
  float k[5]; levelsNumbers(v, n, k);
  return new (std::nothrow) LevelsState{k[0], k[1], k[2], k[3], k[4], std::max(1e-6f, k[1] - k[0]), PowerTable(1.f / k[2])};
}
int levelsInert(const float *v, int32_t n) {
  float k[5]; levelsNumbers(v, n, k);
  return k[0] == 0 && k[1] == 1 && k[2] == 1 && k[3] == 0 && k[4] == 1;
}
const WfxParameter levelsParameters[] = {
    number("inputBlack", "Input Black", 0, 254, 0, 1), number("inputWhite", "Input White", 1, 255, 255, 1),
    number("gamma", "Gamma", .01f, 10, 1, .01f), number("outputBlack", "Output Black", 0, 255, 0, 1),
    number("outputWhite", "Output White", 0, 255, 255, 1)};
const char *levelsShader =
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){float ib=clamp(values[0]/255.,0.,1.),iw=clamp(values[1]/255.,0.,1.),"
    "g=clamp(values[2],.01,10.),ob=clamp(values[3]/255.,0.,1.),ow=clamp(values[4]/255.,0.,1.);"
    "float span=max(1e-6,iw-ib);vec3 n=clamp((v.rgb-ib)/span,0.,1.);"
    "return vec4(mix(vec3(ob),vec3(ow),pow(n,vec3(1./max(.01,g)))),v.a);}";

// ---- Exposure --------------------------------------------------------------
struct ExposureState {
  float gain, offset, gamma;
  PowerTable power;
  void apply(float *p, int32_t count) const {
    gradeSpan(p, count, [&](Rgb c) {
      auto grade = [&](float v) { const float u = std::max(0.f, v * gain + offset); return gamma == 1 ? u : power(u); };
      return Rgb{grade(c.r), grade(c.g), grade(c.b)};
    });
  }
};
WFX_GRADE_ENTRY(ExposureState, exposure)
void exposureNumbers(const float *v, int32_t n, float &exposure, float &offset, float &gamma) {
  exposure = std::clamp(value(v, n, 0, 0), -10.f, 10.f);
  offset = std::clamp(value(v, n, 1, 0), -1.f, 1.f);
  gamma = std::clamp(value(v, n, 2, 1), .01f, 10.f);
}
void *exposurePrepare(const float *v, int32_t n, const WfxContext *) {
  float exposure, offset, gamma; exposureNumbers(v, n, exposure, offset, gamma);
  return new (std::nothrow) ExposureState{std::exp2(exposure), offset, gamma, PowerTable(1.f / gamma)};
}
int exposureInert(const float *v, int32_t n) {
  float exposure, offset, gamma; exposureNumbers(v, n, exposure, offset, gamma);
  return exposure == 0 && offset == 0 && gamma == 1;
}
const WfxParameter exposureParameters[] = {
    number("exposure", "Exposure", -10, 10, 0, .01f, " stops"), number("offset", "Offset", -1, 1, 0, .001f),
    number("gamma", "Gamma", .01f, 10, 1, .01f)};
const char *exposureShader =
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){float e=clamp(values[0],-10.,10.),o=clamp(values[1],-1.,1.),"
    "g=clamp(values[2],.01,10.);return vec4(pow(max(vec3(0),v.rgb*exp2(e)+o),vec3(1./max(.01,g))),v.a);}";

// ---- Brightness & Contrast -------------------------------------------------
struct BrightnessState {
  float brightness, slope;
  void apply(float *p, int32_t count) const {
    gradeSpan(p, count, [&](Rgb c) {
      auto grade = [&](float v) { return std::clamp((v - .5f) * slope + .5f + brightness, 0.f, 1.f); };
      return Rgb{grade(c.r), grade(c.g), grade(c.b)};
    });
  }
};
WFX_GRADE_ENTRY(BrightnessState, brightness)
void *brightnessPrepare(const float *v, int32_t n, const WfxContext *) {
  const float brightness = std::clamp(divided(v, n, 0, 0, 100.), -1.f, 1.f);
  const float contrast = std::clamp(divided(v, n, 1, 0, 100.), -1.f, 1.f);
  const float slope = contrast >= 0 ? 1.f / (1.f - std::min(.99f, contrast)) : 1.f + contrast;
  return new (std::nothrow) BrightnessState{brightness, slope};
}
int brightnessInert(const float *v, int32_t n) {
  return std::clamp(divided(v, n, 0, 0, 100.), -1.f, 1.f) == 0 && std::clamp(divided(v, n, 1, 0, 100.), -1.f, 1.f) == 0;
}
const WfxParameter brightnessParameters[] = {
    number("brightness", "Brightness", -100, 100, 0, 1), number("contrast", "Contrast", -100, 100, 0, 1)};
const char *brightnessShader =
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){float b=clamp(values[0]/100.,-1.,1.),k=clamp(values[1]/100.,-1.,1.);"
    "float slope=k>=0.?1./(1.-min(.99,k)):1.+k;return vec4(clamp((v.rgb-.5)*slope+.5+b,0.,1.),v.a);}";

// ---- Vibrance --------------------------------------------------------------
struct VibranceState {
  float vibrance, saturation;
  void apply(float *p, int32_t count) const {
    gradeSpan(p, count, [&](Rgb c) {
      const float high = std::max({c.r, c.g, c.b}), low = std::min({c.r, c.g, c.b});
      const float chroma = std::clamp(high - low, 0.f, 1.f), gray = luma(c);
      const float selective = vibrance >= 0 ? vibrance * (1 - chroma) : vibrance;
      const float factor = std::max(0.f, (1 + selective) * (1 + saturation));
      return Rgb{std::clamp(gray + (c.r - gray) * factor, 0.f, 1.f), std::clamp(gray + (c.g - gray) * factor, 0.f, 1.f),
                 std::clamp(gray + (c.b - gray) * factor, 0.f, 1.f)};
    });
  }
};
WFX_GRADE_ENTRY(VibranceState, vibrance)
VibranceState vibranceFrom(const float *v, int32_t n) {
  return {std::clamp(divided(v, n, 0, 0, 100.), -1.f, 1.f), std::clamp(divided(v, n, 1, 0, 100.), -1.f, 1.f)};
}
void *vibrancePrepare(const float *v, int32_t n, const WfxContext *) { return new (std::nothrow) VibranceState(vibranceFrom(v, n)); }
int vibranceInert(const float *v, int32_t n) { const auto s = vibranceFrom(v, n); return s.vibrance == 0 && s.saturation == 0; }
const WfxParameter vibranceParameters[] = {
    number("vibrance", "Vibrance", -100, 100, 0, 1), number("saturation", "Saturation", -100, 100, 0, 1)};
const char *vibranceShader = WFX_GLSL_HELPERS
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){float vib=clamp(values[0]/100.,-1.,1.),sat=clamp(values[1]/100.,-1.,1.);"
    "float hi=max(v.r,max(v.g,v.b)),lo=min(v.r,min(v.g,v.b)),chroma=clamp(hi-lo,0.,1.),gray=luma(v.rgb);"
    "float selective=vib>=0.?vib*(1.-chroma):vib;float factor=max(0.,(1.+selective)*(1.+sat));"
    "return vec4(clamp(vec3(gray)+(v.rgb-gray)*factor,0.,1.),v.a);}";

// ---- Color Balance ---------------------------------------------------------
struct BalanceState {
  Rgb shadows, mids, highs;
  bool preserve;
  void apply(float *p, int32_t count) const {
    gradeSpan(p, count, [&](Rgb c) {
      const float before = luma(c), sw = (1 - before) * (1 - before), hw = before * before, mw = std::max(0.f, 1 - sw - hw);
      Rgb out{std::clamp(c.r + (shadows.r * sw + mids.r * mw + highs.r * hw) * .5f, 0.f, 1.f),
              std::clamp(c.g + (shadows.g * sw + mids.g * mw + highs.g * hw) * .5f, 0.f, 1.f),
              std::clamp(c.b + (shadows.b * sw + mids.b * mw + highs.b * hw) * .5f, 0.f, 1.f)};
      if (preserve) {
        const float correction = before - luma(out);
        out.r = std::clamp(out.r + correction, 0.f, 1.f); out.g = std::clamp(out.g + correction, 0.f, 1.f);
        out.b = std::clamp(out.b + correction, 0.f, 1.f);
      }
      return out;
    });
  }
};
WFX_GRADE_ENTRY(BalanceState, balance)
BalanceState balanceFrom(const float *v, int32_t n) {
  auto at = [&](int i) { return divided(v, n, i, 0, 100.); };
  return {{at(0), at(1), at(2)}, {at(3), at(4), at(5)}, {at(6), at(7), at(8)}, value(v, n, 9, 1) >= .5f};
}
void *balancePrepare(const float *v, int32_t n, const WfxContext *) { return new (std::nothrow) BalanceState(balanceFrom(v, n)); }
int balanceInert(const float *v, int32_t n) {
  for (int i = 0; i < 9; ++i) if (value(v, n, i, 0) != 0) return 0;
  return 1;
}
// Folded by range in Effect Setting (SDK 7); Preserve Luminosity is for all three.
const WfxParameter balanceParameters[] = {
    group("Shadows"),
    number("shadowsRed", "Red / Cyan", -100, 100, 0, 1), number("shadowsGreen", "Green / Magenta", -100, 100, 0, 1),
    number("shadowsBlue", "Blue / Yellow", -100, 100, 0, 1),
    groupEnd(),
    group("Midtones"),
    number("midtonesRed", "Red / Cyan", -100, 100, 0, 1), number("midtonesGreen", "Green / Magenta", -100, 100, 0, 1),
    number("midtonesBlue", "Blue / Yellow", -100, 100, 0, 1),
    groupEnd(),
    group("Highlights"),
    number("highlightsRed", "Red / Cyan", -100, 100, 0, 1), number("highlightsGreen", "Green / Magenta", -100, 100, 0, 1),
    number("highlightsBlue", "Blue / Yellow", -100, 100, 0, 1),
    groupEnd(),
    toggle("preserveLuminosity", "Preserve Luminosity", true)};
const char *balanceShader = WFX_GLSL_HELPERS
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){vec3 sh=vec3(values[0],values[1],values[2])/100.,"
    "mi=vec3(values[3],values[4],values[5])/100.,hl=vec3(values[6],values[7],values[8])/100.;"
    "float before=luma(v.rgb),sw=(1.-before)*(1.-before),hw=before*before,mw=max(0.,1.-sw-hw);"
    "vec3 o=clamp(v.rgb+(sh*sw+mi*mw+hl*hw)*.5,0.,1.);if(values[9]>=.5)o=clamp(o+(before-luma(o)),0.,1.);"
    "return vec4(o,v.a);}";

// ---- Three-Way Color Corrector ----------------------------------------------
Rgb direction(const float *v, int32_t n, int at) {
  const float grey = 128.f / 255.f;
  const Rgb c{value(v, n, at, grey), value(v, n, at + 1, grey), value(v, n, at + 2, grey)};
  const float neutral = luma(c);
  return {c.r - neutral, c.g - neutral, c.b - neutral};
}
struct ThreeWayState {
  Rgb shadows, mids, highs;
  float lift, gamma, gain, saturation, balance;
  PowerTable power;
  void apply(float *p, int32_t count) const {
    gradeSpan(p, count, [&](Rgb c) {
      auto lifted = [&](float x) { const float u = std::max(0.f, (x + lift) * gain); return gamma == 1 ? u : power(u); };
      c = {lifted(c.r), lifted(c.g), lifted(c.b)};
      const float tone = std::clamp(luma(c) + balance * .25f, 0.f, 1.f);
      const float sw = (1 - tone) * (1 - tone), hw = tone * tone, mw = std::max(0.f, 1 - sw - hw);
      c = {std::clamp(c.r + (shadows.r * sw + mids.r * mw + highs.r * hw) * .65f, 0.f, 1.f),
           std::clamp(c.g + (shadows.g * sw + mids.g * mw + highs.g * hw) * .65f, 0.f, 1.f),
           std::clamp(c.b + (shadows.b * sw + mids.b * mw + highs.b * hw) * .65f, 0.f, 1.f)};
      const float gray = luma(c), factor = std::max(0.f, 1 + saturation);
      return Rgb{std::clamp(gray + (c.r - gray) * factor, 0.f, 1.f), std::clamp(gray + (c.g - gray) * factor, 0.f, 1.f),
                 std::clamp(gray + (c.b - gray) * factor, 0.f, 1.f)};
    });
  }
};
WFX_GRADE_ENTRY(ThreeWayState, threeWay)
void *threeWayPrepare(const float *v, int32_t n, const WfxContext *) {
  const float gamma = std::clamp(value(v, n, 13, 1), .01f, 10.f);
  return new (std::nothrow) ThreeWayState{direction(v, n, 0), direction(v, n, 4), direction(v, n, 8),
                                          std::clamp(value(v, n, 12, 0), -1.f, 1.f), gamma,
                                          std::clamp(value(v, n, 14, 1), 0.f, 4.f),
                                          std::clamp(divided(v, n, 15, 0, 100.), -1.f, 2.f),
                                          std::clamp(divided(v, n, 16, 0, 100.), -1.f, 1.f), PowerTable(1.f / gamma)};
}
int threeWayInert(const float *v, int32_t n) {
  auto zero = [](Rgb c) { return c.r == 0 && c.g == 0 && c.b == 0; };
  return zero(direction(v, n, 0)) && zero(direction(v, n, 4)) && zero(direction(v, n, 8)) &&
         std::clamp(value(v, n, 12, 0), -1.f, 1.f) == 0 && std::clamp(value(v, n, 13, 1), .01f, 10.f) == 1 &&
         std::clamp(value(v, n, 14, 1), 0.f, 4.f) == 1 && std::clamp(divided(v, n, 15, 0, 100.), -1.f, 2.f) == 0;
}
const WfxParameter threeWayParameters[] = {
    wheel("shadows", "Shadows"), wheel("midtones", "Midtones"), wheel("highlights", "Highlights"),
    number("lift", "Lift", -1, 1, 0, .01f), number("gamma", "Gamma", .01f, 10, 1, .01f), number("gain", "Gain", 0, 4, 1, .01f),
    number("saturation", "Saturation", -100, 200, 0, 1), number("balance", "Tonal Balance", -100, 100, 0, 1)};
const char *threeWayShader = WFX_GLSL_HELPERS
    "vec3 dir(int o){vec3 c=vec3(values[o],values[o+1],values[o+2]);return c-luma(c);}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){float lift=clamp(values[12],-1.,1.),g=clamp(values[13],.01,10.),"
    "gain=clamp(values[14],0.,4.),sat=clamp(values[15]/100.,-1.,2.),bal=clamp(values[16]/100.,-1.,1.);"
    "vec3 c=pow(max(vec3(0),(v.rgb+lift)*gain),vec3(1./max(.01,g)));"
    "float tone=clamp(luma(c)+bal*.25,0.,1.),sw=(1.-tone)*(1.-tone),hw=tone*tone,mw=max(0.,1.-sw-hw);"
    "c=clamp(c+(dir(0)*sw+dir(4)*mw+dir(8)*hw)*.65,0.,1.);float gray=luma(c),f=max(0.,1.+sat);"
    "return vec4(clamp(vec3(gray)+(c-gray)*f,0.,1.),v.a);}";

// ---- Temperature & Tint ----------------------------------------------------
struct TemperatureState {
  float temperature, tint;
  void apply(float *p, int32_t count) const {
    gradeSpan(p, count, [&](Rgb c) {
      return Rgb{std::clamp(c.r + temperature * .18f + tint * .08f, 0.f, 1.f), std::clamp(c.g - tint * .16f, 0.f, 1.f),
                 std::clamp(c.b - temperature * .18f + tint * .08f, 0.f, 1.f)};
    });
  }
};
WFX_GRADE_ENTRY(TemperatureState, temperature)
TemperatureState temperatureFrom(const float *v, int32_t n) {
  return {std::clamp(divided(v, n, 0, 0, 100.), -1.f, 1.f), std::clamp(divided(v, n, 1, 0, 100.), -1.f, 1.f)};
}
void *temperaturePrepare(const float *v, int32_t n, const WfxContext *) { return new (std::nothrow) TemperatureState(temperatureFrom(v, n)); }
int temperatureInert(const float *v, int32_t n) { const auto s = temperatureFrom(v, n); return s.temperature == 0 && s.tint == 0; }
const WfxParameter temperatureParameters[] = {
    number("temperature", "Temperature", -100, 100, 0, 1), number("tint", "Tint", -100, 100, 0, 1)};
const char *temperatureShader =
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){float t=clamp(values[0]/100.,-1.,1.),k=clamp(values[1]/100.,-1.,1.);"
    "return vec4(clamp(v.rgb+vec3(t*.18+k*.08,-k*.16,-t*.18+k*.08),0.,1.),v.a);}";

// ---- Black & White ---------------------------------------------------------
struct BlackWhiteState {
  std::array<float, 6> weights;
  float tintAmount, tintRed, tintGreen, tintBlue;
  void apply(float *p, int32_t count) const {
    gradeSpan(p, count, [&](Rgb c) {
      const auto hsl = rgbToHsl(c); const float at = hsl.h * 6;
      const int sector = std::min(5, int(at)); const float f = at - sector;
      const float weight = weights[size_t(sector)] * (1 - f) + weights[size_t(sector == 5 ? 0 : sector + 1)] * f;
      const float gray = std::clamp(luma(c) * weight, 0.f, 1.f);
      return Rgb{gray * (1 - tintAmount) + gray * tintRed * tintAmount, gray * (1 - tintAmount) + gray * tintGreen * tintAmount,
                 gray * (1 - tintAmount) + gray * tintBlue * tintAmount};
    });
  }
};
WFX_GRADE_ENTRY(BlackWhiteState, blackWhite)
void *blackWhitePrepare(const float *v, int32_t n, const WfxContext *) {
  auto state = new (std::nothrow) BlackWhiteState{};
  if (!state) return nullptr;
  for (int i = 0; i < 6; ++i) state->weights[size_t(i)] = divided(v, n, i, 100, 100.);
  state->tintRed = value(v, n, 6, 1); state->tintGreen = value(v, n, 7, 1); state->tintBlue = value(v, n, 8, 1);
  state->tintAmount = std::clamp(divided(v, n, 10, 0, 100.), 0.f, 1.f);
  return state;
}
const WfxParameter blackWhiteParameters[] = {
    number("reds", "Reds", -200, 300, 100, 1, " %"), number("yellows", "Yellows", -200, 300, 100, 1, " %"),
    number("greens", "Greens", -200, 300, 100, 1, " %"), number("cyans", "Cyans", -200, 300, 100, 1, " %"),
    number("blues", "Blues", -200, 300, 100, 1, " %"), number("magentas", "Magentas", -200, 300, 100, 1, " %"),
    color("tintColor", "Tint Color", 1, 1, 1, 1), number("tintAmount", "Tint Amount", 0, 100, 0, 1, " %")};
const char *blackWhiteShader = WFX_GLSL_HELPERS
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){vec3 h=rgb2hsl(clamp(v.rgb,0.,1.));float at=h.x*6.,f=fract(at);int sct=int(at);"
    "int nx=(sct+1)%6;float w=mix(values[sct]/100.,values[nx]/100.,f);"
    "float gray=clamp(luma(v.rgb)*w,0.,1.);vec3 mono=vec3(gray);"
    "return vec4(mix(mono,mono*vec3(values[6],values[7],values[8]),clamp(values[10]/100.,0.,1.)),v.a);}";

// ---- the pack ---------------------------------------------------------------
#define GRADE(id, name, prefix, parameters)                                                                    \
  WfxEffect {                                                                                                  \
    sizeof(WfxEffect), WFX_ABI_VERSION, id, name, "Color Correction", 4u, 0u,                                  \
        int32_t(sizeof(parameters) / sizeof(parameters[0])), parameters, prefix##Process, prefix##Inert,       \
        nullptr, nullptr, prefix##Prepare, prefix##Span, prefix##Release, prefix##Shader                      \
  }
int blackWhiteInert(const float *, int32_t) { return 0; }
const WfxEffect effects[] = {
    GRADE("Curves", "Curves", curves, curvesParameters),
    GRADE("HSL", "HSL", hsl, hslParameters),
    GRADE("Levels", "Levels", levels, levelsParameters),
    GRADE("Exposure", "Exposure", exposure, exposureParameters),
    GRADE("Brightness & Contrast", "Brightness & Contrast", brightness, brightnessParameters),
    GRADE("Vibrance", "Vibrance", vibrance, vibranceParameters),
    GRADE("Color Balance", "Color Balance", balance, balanceParameters),
    GRADE("Three-Way Color Corrector", "Three-Way Color Corrector", threeWay, threeWayParameters),
    GRADE("Temperature & Tint", "Temperature & Tint", temperature, temperatureParameters),
    GRADE("Black & White", "Black & White", blackWhite, blackWhiteParameters)};
}  // namespace

WFX_EXPORT int32_t wfxEffectCount(void) { return int32_t(sizeof(effects) / sizeof(effects[0])); }
WFX_EXPORT const WfxEffect *wfxEffectAt(int32_t index) {
  return index >= 0 && index < wfxEffectCount() ? &effects[index] : nullptr;
}
