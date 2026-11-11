// Wavy-Stylize.wfx: Tint, RGB Split, Film Grain, Vignette, Tile Repeat, Gradient
// Map, Posterize, Halftone, Scanlines, Light Sweep, Threshold, Find Edges and
// Emboss.
#include "wavy_effect.h"
#include "pack_common.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>
// A pixel is four floats, one SSE register: RGB Split reads its two shifted
// pictures a whole pixel at a time where the processor has it (every x64 one),
// and Film Grain and Vignette work on four pixels at once.
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define WAVY_STYLIZE_SSE 1
#endif

using namespace wfxpack;

namespace {
float *row(void *pixels, int32_t stride, int32_t y) {
  return reinterpret_cast<float *>(static_cast<unsigned char *>(pixels) + size_t(y) * size_t(stride));
}

#ifdef WAVY_STYLIZE_SSE
// Four pixels at a time, to the bit what the loops that take one at a time
// make: every expression in its own order, divps and sqrtps rounding as divss
// and sqrtss do, and no fused multiply-add (this build has none). Inlined by
// force: left to itself the compiler called Film Grain's four pixels, and each
// of their three channels, as functions of their own -- 2.8 ms a 960x540
// picture on one core, 2.2 inlined.
#ifdef _MSC_VER
#define WAVY_STYLIZE_INLINE __forceinline
#else
#define WAVY_STYLIZE_INLINE inline __attribute__((always_inline))
#endif
// std::clamp(v, 0.f, 1.f) as the library does it: 1 < v gives 1, v < 0 gives
// +0, and everything else passes as it is -- NaN and -0 too, which minps and
// maxps would not let through.
WAVY_STYLIZE_INLINE __m128 clampUnit(__m128 v) {
  const __m128 one = _mm_set1_ps(1.f), above = _mm_cmplt_ps(one, v);
  v = _mm_andnot_ps(_mm_cmplt_ps(v, _mm_setzero_ps()), v);
  return _mm_or_ps(_mm_andnot_ps(above, v), _mm_and_ps(above, one));
}
// Where `mask` is set the first value, elsewhere the second, bit for bit.
WAVY_STYLIZE_INLINE __m128 pick(__m128 mask, __m128 a, __m128 b) {
  return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b));
}
// `sum` (first + something) as SSE makes it with `first` in front: when both
// are NaN the processor hands back the first one's, and the compiler may put
// either operand first (it turned Film Grain's luma round). `first` comes out
// of arithmetic, so a NaN in it is already quiet.
WAVY_STYLIZE_INLINE __m128 firstNaN(__m128 first, __m128 sum) { return pick(_mm_cmpunord_ps(first, first), first, sum); }
// a * k in four uint32 lanes, the low 32 bits as C++ keeps them. SSE2 only
// multiplies lanes 0 and 2, into 64 bits (pmulld is SSE4.1), so lanes 1 and 3
// are moved down for a second multiply and the four low halves put together.
WAVY_STYLIZE_INLINE __m128i multiplyLow(__m128i a, __m128i k) {
  const __m128i even = _mm_mul_epu32(a, k), odd = _mm_mul_epu32(_mm_srli_epi64(a, 32), _mm_srli_epi64(k, 32));
  return _mm_unpacklo_epi32(_mm_shuffle_epi32(even, _MM_SHUFFLE(0, 0, 2, 0)), _mm_shuffle_epi32(odd, _MM_SHUFFLE(0, 0, 2, 0)));
}
// A row four pixels at a time, turned into one register per channel -- red,
// green, blue and alpha of pixels x to x + 3 -- for `work` to change the
// colour of, and stored back when it says something changed. The last one to
// three pixels are made up to four with clear ones, which the effects leave
// alone. Alpha goes back as it came.
template <class Work> void quads(float *p, int32_t width, const Work &work) {
  for (int32_t x = 0; x < width; x += 4) {
    alignas(16) float spare[16];
    float *q = p + size_t(x) * 4;
    const int32_t count = std::min<int32_t>(4, width - x);
    if (count < 4) {
      std::memset(spare, 0, sizeof spare);
      std::memcpy(spare, q, size_t(count) * 16);
      q = spare;
    }
    __m128 r = _mm_loadu_ps(q), g = _mm_loadu_ps(q + 4), b = _mm_loadu_ps(q + 8), a = _mm_loadu_ps(q + 12);
    _MM_TRANSPOSE4_PS(r, g, b, a);
    if (!work(x, r, g, b, a)) continue;
    _MM_TRANSPOSE4_PS(r, g, b, a);
    _mm_storeu_ps(q, r);
    _mm_storeu_ps(q + 4, g);
    _mm_storeu_ps(q + 8, b);
    _mm_storeu_ps(q + 12, a);
    if (count < 4) std::memcpy(p + size_t(x) * 4, spare, size_t(count) * 16);
  }
}
#endif

// ---- Tint ------------------------------------------------------------------
// Mixes every pixel towards one colour; a per-pixel effect, so it can join a
// run of colour grades.
struct TintState {
  float keep, add[3];
  void apply(float *p, int32_t count) const {
    for (int32_t x = 0; x < count; ++x)
      for (int c = 0; c < 3; ++c) p[x * 4 + c] = p[x * 4 + c] * keep + add[c];
  }
};
TintState tintFrom(const float *v, int32_t n) {
  const float amount = std::clamp(divided(v, n, 4, 100, 100.), 0.f, 1.f);
  TintState s{};
  s.keep = 1.f - amount;
  for (int c = 0; c < 3; ++c) s.add[c] = float(double(value(v, n, c, 1)) * amount);
  return s;
}
void *tintPrepare(const float *v, int32_t n, const WfxContext *) { return new (std::nothrow) TintState(tintFrom(v, n)); }
void tintSpan(float *p, int32_t count, const void *s) { static_cast<const TintState *>(s)->apply(p, count); }
void tintRelease(void *s) { delete static_cast<TintState *>(s); }
int tintInert(const float *v, int32_t n) { return std::clamp(divided(v, n, 4, 100, 100.), 0.f, 1.f) <= 0; }
int tintProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  if (tintInert(v, n)) return 0;
  const TintState state = tintFrom(v, n);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) state.apply(row(pixels, stride, y), width);
  });
  return 0;
}
const WfxParameter tintParameters[] = {color("color", "Color", 1, 1, 1, 1), number("amount", "Amount", 0, 100, 100, 1, " %")};
const char *tintShader =
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){float a=clamp(values[4]/100.,0.,1.);"
    "return vec4(v.rgb*(1.-a)+vec3(values[0],values[1],values[2])*a,v.a);}";

// ---- RGB Split -------------------------------------------------------------
// The red and the blue picture pulled apart along a direction, the way a cheap
// lens (or a glitch) throws colour fringes off every edge. Done on
// premultiplied colour so a fringe is coloured light with its own coverage;
// coverage is the widest of the three pictures'. Amount is in layer pixels.
double splitAmount(const float *v, int32_t n, double scale) { return std::clamp(double(value(v, n, 0, 4)), 0., 100.) * scale; }
int splitInert(const float *v, int32_t n) { return std::clamp(double(value(v, n, 0, 4)), 0., 100.) < .01; }
double splitReach(const float *v, int32_t n, double) {
  // In layer pixels, rounded up so nudging the amount does not resize the
  // raster at every step; the host scales it.
  const double amount = std::clamp(double(value(v, n, 0, 4)), 0., 100.);
  return amount > 0 ? std::ceil(amount / 8.) * 8. : 0;
}
int splitProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const double amount = splitAmount(v, n, context->scale);
  if (amount < .01 || width <= 0 || height <= 0) return 0;
  const double angle = double(value(v, n, 1, 0)) * 3.14159265358979323846 / 180.;
  const float dx = float(std::cos(angle) * amount), dy = float(std::sin(angle) * amount);
  // The picture as it was, read while the result is written over it. Kept for
  // the next frame rather than made fresh: a new 1080p float copy, zeroed and
  // first touched page by page on one core, cost more than the split itself.
  const Scratch kept(size_t(width) * size_t(height) * 4);
  float *const source = kept.data();
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *to = &source[size_t(y) * size_t(width) * 4];
      std::memcpy(to, row(pixels, stride, y), size_t(width) * 16);
    }
  });
  auto at = [&](int x, int y, int channel) {
    if (x < 0 || y < 0 || x >= width || y >= height) return 0.f;
    return source[(size_t(y) * size_t(width) + size_t(x)) * 4 + size_t(channel)];
  };
  auto sample = [&](float fx, float fy, int channel) {
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float tx = fx - float(x0), ty = fy - float(y0);
    return (at(x0, y0, channel) * (1 - tx) + at(x0 + 1, y0, channel) * tx) * (1 - ty) +
           (at(x0, y0 + 1, channel) * (1 - tx) + at(x0 + 1, y0 + 1, channel) * tx) * ty;
  };
  // The shift is the same for every pixel, so the four bilinear weights are
  // too; everywhere the neighbourhood is inside the picture that is a handful
  // of multiplies with no edge checks, and the borders take the general route.
  struct Tap { int ox, oy; float w00, w10, w01, w11; };
  auto makeTap = [](float shiftX, float shiftY) {
    const int ox = int(std::floor(shiftX)), oy = int(std::floor(shiftY));
    const float tx = shiftX - float(ox), ty = shiftY - float(oy);
    return Tap{ox, oy, (1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
  };
  const Tap redTap = makeTap(-dx, -dy), blueTap = makeTap(dx, dy);
  auto inside = [&](const Tap &t, int x, int y) { return x + t.ox >= 0 && y + t.oy >= 0 && x + t.ox + 1 < width && y + t.oy + 1 < height; };
  auto fast = [&](const Tap &t, int x, int y, int channel) {
    const float *r0 = &source[(size_t(y + t.oy) * size_t(width) + size_t(x + t.ox)) * 4 + size_t(channel)];
    const float *r1 = &source[(size_t(y + t.oy + 1) * size_t(width) + size_t(x + t.ox)) * 4 + size_t(channel)];
    return r0[0] * t.w00 + r0[4] * t.w10 + r1[0] * t.w01 + r1[4] * t.w11;
  };
#ifdef WAVY_STYLIZE_SSE
  // Both shifted pixels whole, in the same order of operations as fast(), so
  // the result is the same to the bit.
  const __m128 r00 = _mm_set1_ps(redTap.w00), r10 = _mm_set1_ps(redTap.w10), r01 = _mm_set1_ps(redTap.w01),
               r11 = _mm_set1_ps(redTap.w11), b00 = _mm_set1_ps(blueTap.w00), b10 = _mm_set1_ps(blueTap.w10),
               b01 = _mm_set1_ps(blueTap.w01), b11 = _mm_set1_ps(blueTap.w11);
  const size_t line = size_t(width) * 4;
  auto tap4 = [&](const Tap &t, __m128 w00, __m128 w10, __m128 w01, __m128 w11, int x, int y) {
    const float *a = &source[(size_t(y + t.oy) * size_t(width) + size_t(x + t.ox)) * 4], *c = a + line;
    return _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_loadu_ps(a), w00), _mm_mul_ps(_mm_loadu_ps(a + 4), w10)),
                                 _mm_mul_ps(_mm_loadu_ps(c), w01)),
                      _mm_mul_ps(_mm_loadu_ps(c + 4), w11));
  };
#endif
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *dest = row(pixels, stride, y);
      const float *own = &source[size_t(y) * size_t(width) * 4];
      // Where on this row both shifts land wholly inside the picture: there no
      // pixel needs its edges checked. It was asked of every pixel, eight
      // comparisons each, and was most of the effect's time.
      int left = 0, right = 0;
      if (y + std::min(redTap.oy, blueTap.oy) >= 0 && y + std::max(redTap.oy, blueTap.oy) + 1 < height) {
        left = std::min(int(width), std::max({0, -redTap.ox, -blueTap.ox}));
        right = std::max(left, std::min({int(width), int(width) - 1 - redTap.ox, int(width) - 1 - blueTap.ox}));
      }
      for (int32_t x = left; x < right; ++x) {
        float *d = dest + x * 4;
        const float ownAlpha = own[x * 4 + 3];
#ifdef WAVY_STYLIZE_SSE
        alignas(16) float red[4], blue[4];
        _mm_store_ps(red, tap4(redTap, r00, r10, r01, r11, x, y));
        _mm_store_ps(blue, tap4(blueTap, b00, b10, b01, b11, x, y));
        if (red[3] <= 0.f && blue[3] <= 0.f && ownAlpha <= 0.f) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
        d[0] = red[0];
        d[1] = own[x * 4 + 1];
        d[2] = blue[2];
        d[3] = std::max({red[3], blue[3], ownAlpha});
#else
        const float redAlpha = fast(redTap, x, y, 3), blueAlpha = fast(blueTap, x, y, 3);
        if (redAlpha <= 0.f && blueAlpha <= 0.f && ownAlpha <= 0.f) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
        d[0] = fast(redTap, x, y, 0);
        d[1] = own[x * 4 + 1];
        d[2] = fast(blueTap, x, y, 2);
        d[3] = std::max({redAlpha, blueAlpha, ownAlpha});
#endif
      }
      for (int32_t x = 0; x < width; ++x) {
        if (x == left && right > left) x = right; // the inside is done
        if (x >= width) break;
        float *d = dest + x * 4;
        float redAlpha, blueAlpha;
        const float ownAlpha = own[x * 4 + 3];
        const bool quick = inside(redTap, x, y) && inside(blueTap, x, y);
        const float fx = float(x), fy = float(y);
        redAlpha = quick ? fast(redTap, x, y, 3) : sample(fx - dx, fy - dy, 3);
        blueAlpha = quick ? fast(blueTap, x, y, 3) : sample(fx + dx, fy + dy, 3);
        if (redAlpha <= 0.f && blueAlpha <= 0.f && ownAlpha <= 0.f) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
        d[0] = quick ? fast(redTap, x, y, 0) : sample(fx - dx, fy - dy, 0);
        d[1] = own[x * 4 + 1];
        d[2] = quick ? fast(blueTap, x, y, 2) : sample(fx + dx, fy + dy, 2);
        d[3] = std::max({redAlpha, blueAlpha, ownAlpha});
      }
    }
  });
  return 0;
}
// On the card: the two shifted pictures read with the same weights, from the
// premultiplied colour, pixel positions counted from the corner as here.
const char *splitShader =
    "vec4 spTexel(int x,int y){vec4 t=wavyPixel(ivec2(x,y));return vec4(t.rgb*t.a,t.a);}"
    "vec4 spSample(float fx,float fy){int x0=int(floor(fx)),y0=int(floor(fy));float tx=fx-float(x0),ty=fy-float(y0);"
    "return (spTexel(x0,y0)*(1.-tx)+spTexel(x0+1,y0)*tx)*(1.-ty)+(spTexel(x0,y0+1)*(1.-tx)+spTexel(x0+1,y0+1)*tx)*ty;}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float amount=clamp(values[0],0.,100.)*wavyScale;if(amount<.01)return v;"
    "float angle=mod(values[1],360.)*3.14159265358979/180.;float dx=cos(angle)*amount,dy=sin(angle)*amount;"
    "float x=floor(p.x),y=floor(p.y);vec4 own=vec4(v.rgb*v.a,v.a);"
    "vec4 red=spSample(x-dx,y-dy),blue=spSample(x+dx,y+dy);"
    "if(red.a<=0.&&blue.a<=0.&&own.a<=0.)return vec4(0.);"
    "vec4 d=vec4(red.r,own.g,blue.b,max(max(red.a,blue.a),own.a));"
    "return d.a>0.?vec4(d.rgb/d.a,d.a):vec4(0.);}";
const WfxParameter splitParameters[] = {number("amount", "Amount", 0, 100, 4, .5f, " px"),
                                        number("angle", "Angle", -360, 360, 0, 1, "\xc2\xb0")};

// ---- Film Grain ------------------------------------------------------------
// Fine noise laid over the colour, strongest in the midtones (a real emulsion
// shows least in pure black and pure white), the same on every render because
// it depends only on where a pixel is and on the seed -- and, Animated, on
// which of Rate's steps per second the frame falls in: grain that boils the way
// film's does, the same frame always the same grain.
enum { GrainAmount, GrainSize, GrainColour, GrainSeed, GrainAnimated, GrainRate };
#ifdef WAVY_STYLIZE_SSE
// Four pixels of a row (quads), the loop in grainProcess to the bit: the same
// hash on four columns, whose shares come from a table made once a call (the
// host hands over sixteen rows at a time) instead of an integer division a
// pixel. A 960x540 picture on one core: 7.4 ms one pixel at a time, 2.2 four
// at a time (an efficiency core 10.3 and 4.6).
struct GrainQuads {
  const uint32_t *columns; // each column's share of the hash
  __m128i line;            // the seed's and the row's
  __m128 colour, grey, amount;
  WAVY_STYLIZE_INLINE __m128 noise(__m128i h) const {
    h = multiplyLow(_mm_xor_si128(h, _mm_srli_epi32(h, 13)), _mm_set1_epi32(int(1274126177u)));
    h = _mm_xor_si128(h, _mm_srli_epi32(h, 16));
    return _mm_sub_ps(_mm_div_ps(_mm_cvtepi32_ps(_mm_and_si128(h, _mm_set1_epi32(0xffff))), _mm_set1_ps(32767.5f)),
                      _mm_set1_ps(1.f));
  }
  // One channel: the grain of `channel` (1 to 3) laid on `v`.
  WAVY_STYLIZE_INLINE __m128 lay(__m128i hashed, uint32_t channel, __m128 greyPart, __m128 weight, __m128 v, bool nan) const {
    const __m128 nn = _mm_add_ps(_mm_mul_ps(noise(_mm_xor_si128(hashed, _mm_set1_epi32(int(channel * 2246822519u)))), colour),
                                 greyPart);
    const __m128 lift = _mm_mul_ps(_mm_mul_ps(_mm_mul_ps(nn, amount), _mm_set1_ps(.5f)), weight), sum = _mm_add_ps(lift, v);
    return clampUnit(nan ? firstNaN(lift, sum) : sum);
  }
  WAVY_STYLIZE_INLINE bool operator()(int32_t x, __m128 &r, __m128 &g, __m128 &b, const __m128 &a) const {
    const __m128 clear = _mm_cmple_ps(a, _mm_setzero_ps());
    const int skipped = _mm_movemask_ps(clear);
    if (skipped == 15) return false;
    const __m128 green = _mm_mul_ps(g, _mm_set1_ps(.7152f)), red = _mm_mul_ps(r, _mm_set1_ps(.2126f)),
                 blue = _mm_mul_ps(b, _mm_set1_ps(.0722f));
    __m128 luma = _mm_add_ps(_mm_add_ps(green, red), blue);
    // Only a NaN in the colour lets two NaNs meet (in the luma, then with the
    // pixel's own); such pixels have the compiled loop's order spelled out:
    // green before red, the grain before the pixel.
    const bool nan = _mm_movemask_ps(_mm_cmpunord_ps(luma, luma)) != 0;
    if (nan) {
      const __m128 sum = firstNaN(green, _mm_add_ps(green, red));
      luma = firstNaN(sum, _mm_add_ps(sum, blue));
    }
    const __m128 level = clampUnit(luma);
    const __m128 weight = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(_mm_set1_ps(1.f), level), _mm_mul_ps(level, _mm_set1_ps(.65f * 4.f))),
                                     _mm_set1_ps(.35f));
    const __m128i hashed = _mm_xor_si128(line, _mm_loadu_si128(reinterpret_cast<const __m128i *>(columns + x)));
    const __m128 greyPart = _mm_mul_ps(grey, noise(hashed));
    __m128 nr = lay(hashed, 1u, greyPart, weight, r, nan), ng = lay(hashed, 2u, greyPart, weight, g, nan),
           nb = lay(hashed, 3u, greyPart, weight, b, nan);
    if (skipped) {
      nr = pick(clear, r, nr);
      ng = pick(clear, g, ng);
      nb = pick(clear, b, nb);
    }
    r = nr;
    g = ng;
    b = nb;
    return true;
  }
};
#endif
int grainVaries(const float *v, int32_t n) { return value(v, n, GrainAnimated, 0) > .5f; }
int grainInert(const float *v, int32_t n) { return std::clamp(divided(v, n, 0, 20, 100.), 0.f, 1.f) <= 0; }
int grainProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float amount = std::clamp(divided(v, n, 0, 20, 100.), 0.f, 1.f);
  if (amount <= 0) return 0;
  const int cell = std::max(1, int(std::lround(std::clamp(double(value(v, n, 1, 1.5f)), .5, 20.) * context->scale)));
  const float colourful = std::clamp(divided(v, n, 2, 30, 100.), 0.f, 1.f);
  int seedValue = int(value(v, n, GrainSeed, 1));
  if (grainVaries(v, n)) {
    const double rate = std::clamp(double(value(v, n, GrainRate, 24)), 1., 120.);
    seedValue += int(std::floor(context->seconds * rate + 1e-9)) * 7919;
  }
  const uint32_t seed = uint32_t(seedValue) * 2654435761u;
  auto noise = [&](int gx, int gy, uint32_t channel) {
    uint32_t h = seed ^ (uint32_t(gx) * 374761393u) ^ (uint32_t(gy) * 668265263u) ^ (channel * 2246822519u);
    h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
    return float(h & 0xffff) / 32767.5f - 1.f;
  };
  // The grain is the raster's, wherever the strip handed over lies in it.
  const RasterPlace place = rasterPlace(context, width, height);
#ifdef WAVY_STYLIZE_SSE
  // Four pixels at a time (GrainQuads above), the loop below to the bit.
  std::unique_ptr<uint32_t[]> columns(width > 0 && height > 0 ? new (std::nothrow) uint32_t[size_t(width) + 3] : nullptr);
  if (columns) {
    for (int32_t x = 0; x < width; ++x) columns[size_t(x)] = uint32_t((place.x + x) / cell) * 374761393u;
    for (size_t x = size_t(width); x < size_t(width) + 3; ++x) columns[x] = 0u;
    forRows(context, height, [&](int32_t begin, int32_t end) {
      for (int32_t y = begin; y < end; ++y) {
        const int gy = (place.y + y) / cell;
        const GrainQuads work{columns.get(), _mm_set1_epi32(int(seed ^ (uint32_t(gy) * 668265263u))), _mm_set1_ps(colourful),
                              _mm_set1_ps(1.f - colourful), _mm_set1_ps(amount)};
        quads(row(pixels, stride, y), width, work);
      }
    });
    return 0;
  }
#endif
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const int gy = (place.y + y) / cell;
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const int gx = (place.x + x) / cell;
        const float luma = .2126f * p[x * 4] + .7152f * p[x * 4 + 1] + .0722f * p[x * 4 + 2];
        const float weight = .35f + .65f * 4.f * std::clamp(luma, 0.f, 1.f) * (1.f - std::clamp(luma, 0.f, 1.f));
        const float grey = noise(gx, gy, 0);
        for (int c = 0; c < 3; ++c) {
          const float nn = grey * (1.f - colourful) + noise(gx, gy, uint32_t(c) + 1) * colourful;
          p[x * 4 + c] = std::clamp(p[x * 4 + c] + nn * amount * .5f * weight, 0.f, 1.f);
        }
      }
    }
  });
  return 0;
}
const WfxParameter grainParameters[] = {
    number("amount", "Amount", 0, 100, 20, 1, " %"), number("size", "Size", .5f, 20, 1.5f, .1f, " px"),
    number("colour", "Colour", 0, 100, 30, 1, " %"), number("seed", "Seed", 0, 9999, 1, 1),
    toggle("animated", "Animated", false), number("rate", "Grain Rate", 1, 120, 24, 1, " /s")};
const char *const grainHelp[] = {
    "How strong the grain is", "How large one grain is, in layer pixels", "How much of the grain is coloured rather than grey",
    "Another seed, another grain", "The grain changes as time passes, like film's", "How many times a second Animated grain changes"};
// The same grain on the card: the hash is integer arithmetic, so it is the
// same grain to the bit. Which of Rate's steps a moment falls in is measured
// with a little give, because the card has the time to single precision only.
const char *grainShader =
    "float grainNoise(uint seed,int gx,int gy,uint c){"
    "uint h=seed^(uint(gx)*374761393u)^(uint(gy)*668265263u)^(c*2246822519u);"
    "h=(h^(h>>13u))*1274126177u;h^=h>>16u;return float(h&0xffffu)/32767.5-1.;}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float amount=clamp(values[0]/100.,0.,1.);if(amount<=0.||v.a<=0.)return v;"
    "int cell=max(1,int(floor(clamp(values[1],.5,20.)*wavyScale+.5)));"
    "float colourful=clamp(values[2]/100.,0.,1.);int seedValue=int(values[3]);"
    "if(values[4]>.5){float steps=wavySeconds*clamp(values[5],1.,120.);"
    "seedValue+=int(floor(steps+max(1e-4,abs(steps)*4e-7)))*7919;}"
    "uint seed=uint(seedValue)*2654435761u;int gx=int(p.x)/cell,gy=int(p.y)/cell;"
    "float l=clamp(.2126*v.r+.7152*v.g+.0722*v.b,0.,1.);float weight=.35+.65*4.*l*(1.-l);"
    "float grey=grainNoise(seed,gx,gy,0u);vec3 o;"
    "for(int c=0;c<3;++c){float nn=grey*(1.-colourful)+grainNoise(seed,gx,gy,uint(c)+1u)*colourful;"
    "o[c]=clamp(v[c]+nn*amount*.5*weight,0.,1.);}"
    "return vec4(o,v.a);}";
uint32_t grainControlState(const float *v, int32_t n, int32_t parameter) {
  return parameter == GrainRate && !grainVaries(v, n) ? uint32_t(WFX_CONTROL_DIMMED) : 0u;
}

// ---- Vignette --------------------------------------------------------------
// Darkens toward the edges of the layer's own box, so a full-frame picture
// darkens toward the frame's corners. Colour turns it toward another colour
// than black (a warm edge, a white fade); Roundness makes the oval follow a
// circle instead of the box's own proportions.
enum { VigAmount, VigSize, VigSoftness, VigColour, VigRoundness = VigColour + 4 };
#ifdef WAVY_STYLIZE_SSE
// Four pixels of a row (quads), the loop in vignetteProcess to the bit. How
// far across the box a column lies does not change down the rows, so nx * nx
// comes from a table made once a call (the host hands over sixteen rows at a
// time), which leaves a pixel one square root and two divisions, four pixels
// to an instruction -- and four pixels all in the untouched middle, or all
// out where the edge is at its full, not even those. A 960x540 picture on one
// core: 2.7 ms one pixel at a time, 0.9 four at a time (an efficiency core
// 5.8 and 1.6).
struct VignetteQuads {
  const float *across; // nx * nx of each column
  __m128 down;         // ny * ny of the row
  __m128 start, span, amount, tint[3];
  bool tinted;
  // Up to `inside` (nx * nx + ny * ny) the factor is 1, from `outside` on it
  // is `edge`, 1 - amount; see vignetteProcess.
  __m128 inside, outside, edge;
  // (d - start) / span, d the distance from the centre.
  WAVY_STYLIZE_INLINE __m128 reach(__m128 s) const {
    return _mm_div_ps(_mm_sub_ps(_mm_div_ps(_mm_sqrt_ps(s), _mm_set1_ps(1.41421356f)), start), span);
  }
  WAVY_STYLIZE_INLINE __m128 factorAt(__m128 reached) const {
    const __m128 t = clampUnit(reached);
    const __m128 smooth = _mm_mul_ps(_mm_sub_ps(_mm_set1_ps(3.f), _mm_add_ps(t, t)), _mm_mul_ps(t, t));
    return _mm_sub_ps(_mm_set1_ps(1.f), _mm_mul_ps(smooth, amount));
  }
  WAVY_STYLIZE_INLINE bool operator()(int32_t x, __m128 &r, __m128 &g, __m128 &b, const __m128 &a) const {
    const __m128 clear = _mm_cmple_ps(a, _mm_setzero_ps());
    const int skipped = _mm_movemask_ps(clear);
    if (skipped == 15) return false;
    const __m128 s = _mm_add_ps(_mm_loadu_ps(across + x), down);
    const __m128 factor = _mm_movemask_ps(_mm_cmple_ps(s, inside)) == 15    ? _mm_set1_ps(1.f)
                          : _mm_movemask_ps(_mm_cmpge_ps(s, outside)) == 15 ? edge
                                                                             : factorAt(reach(s));
    __m128 nr = _mm_mul_ps(factor, r), ng = _mm_mul_ps(factor, g), nb = _mm_mul_ps(factor, b);
    if (tinted) {
      const __m128 rest = _mm_sub_ps(_mm_set1_ps(1.f), factor);
      nr = _mm_add_ps(_mm_mul_ps(rest, tint[0]), nr);
      ng = _mm_add_ps(_mm_mul_ps(rest, tint[1]), ng);
      nb = _mm_add_ps(_mm_mul_ps(rest, tint[2]), nb);
    }
    if (skipped) {
      nr = pick(clear, r, nr);
      ng = pick(clear, g, ng);
      nb = pick(clear, b, nb);
    }
    r = nr;
    g = ng;
    b = nb;
    return true;
  }
};
#endif
int vignetteInert(const float *v, int32_t n) { return std::clamp(divided(v, n, 0, 40, 100.), 0.f, 1.f) <= 0; }
int vignetteProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float amount = std::clamp(divided(v, n, 0, 40, 100.), 0.f, 1.f);
  if (amount <= 0) return 0;
  const float start = std::clamp(divided(v, n, 1, 60, 100.), 0.f, 1.f) * .9f;
  const float soft = std::clamp(divided(v, n, 2, 60, 100.), 0.f, 1.f);
  const RasterPlace place = rasterPlace(context, width, height);
  double bx = context->boxX, by = context->boxY, bw = context->boxWidth, bh = context->boxHeight;
  if (!(bw > 0 && bh > 0)) { bx = 0; by = 0; bw = place.width; bh = place.height; }
  const float cx = float(bx + bw / 2.), cy = float(by + bh / 2.);
  float halfW = std::max(1.f, float(bw) / 2.f), halfH = std::max(1.f, float(bh) / 2.f);
  const float round = std::clamp(divided(v, n, VigRoundness, 0, 100.), 0.f, 1.f);
  if (round > 0.f) {
    const float circle = std::sqrt(halfW * halfH);
    halfW += (circle - halfW) * round;
    halfH += (circle - halfH) * round;
  }
  const float tint[3] = {std::clamp(value(v, n, VigColour, 0), 0.f, 1.f), std::clamp(value(v, n, VigColour + 1, 0), 0.f, 1.f),
                         std::clamp(value(v, n, VigColour + 2, 0), 0.f, 1.f)};
  const bool tinted = tint[0] > 0.f || tint[1] > 0.f || tint[2] > 0.f;
  const float span = std::max(.05f, soft * (1.f - start) + .05f);
#ifdef WAVY_STYLIZE_SSE
  // Four pixels at a time (VignetteQuads above), the loop below to the bit.
  // With the box's centre and half sizes finite (the host's are) a NaN can
  // only come from the colour, so no two NaNs ever meet and the order the
  // compiler gives an operation's operands cannot show; a box that is not
  // takes the loop below.
  const bool finite = std::isfinite(cx) && std::isfinite(cy) && std::isfinite(halfW) && std::isfinite(halfH);
  std::unique_ptr<float[]> across(finite && width > 0 && height > 0 ? new (std::nothrow) float[size_t(width) + 3] : nullptr);
  if (across) {
    for (int32_t x = 0; x < width; ++x) {
      const float nx = (float(place.x + x) + .5f - cx) / halfW;
      across[size_t(x)] = nx * nx;
    }
    for (size_t x = size_t(width); x < size_t(width) + 3; ++x) across[x] = 0.f;
    VignetteQuads work{across.get(), _mm_setzero_ps(), _mm_set1_ps(start), _mm_set1_ps(span), _mm_set1_ps(amount),
                       {_mm_set1_ps(tint[0]), _mm_set1_ps(tint[1]), _mm_set1_ps(tint[2])}, tinted};
    // Where the factor is 1 and where it is 1 - amount, as values of
    // nx * nx + ny * ny. Each step from it to (d - start) / span rounds a
    // function that never falls (a square root, a subtraction, divisions by
    // positive numbers), so neither does the whole: the largest s reaching at
    // most 0 (smoothstep 0) and the smallest reaching at least 1 (smoothstep
    // 1) bound every other s. Both are found by halving among the floats' bit
    // patterns, which for s >= 0 run in the floats' own order; s = 0 reaches
    // -start / span <= 0 and +inf reaches +inf.
    auto reachOf = [&](uint32_t pattern) {
      float s;
      std::memcpy(&s, &pattern, 4);
      return _mm_cvtss_f32(work.reach(_mm_set1_ps(s)));
    };
    uint32_t inner = 0, beyond = 0x7f800000u; // reachOf(inner) <= 0 < reachOf(beyond)
    while (beyond - inner > 1) {
      const uint32_t middle = inner + (beyond - inner) / 2;
      (reachOf(middle) <= 0.f ? inner : beyond) = middle;
    }
    uint32_t before = 0, outer = 0x7f800000u; // reachOf(before) < 1 <= reachOf(outer)
    while (outer - before > 1) {
      const uint32_t middle = before + (outer - before) / 2;
      (reachOf(middle) >= 1.f ? outer : before) = middle;
    }
    float bounds[2];
    std::memcpy(&bounds[0], &inner, 4);
    std::memcpy(&bounds[1], &outer, 4);
    work.inside = _mm_set1_ps(bounds[0]);
    work.outside = _mm_set1_ps(bounds[1]);
    work.edge = work.factorAt(_mm_set1_ps(1.f));
    forRows(context, height, [&](int32_t begin, int32_t end) {
      VignetteQuads line = work;
      for (int32_t y = begin; y < end; ++y) {
        const float ny = (float(place.y + y) + .5f - cy) / halfH;
        line.down = _mm_set1_ps(ny * ny);
        quads(row(pixels, stride, y), width, line);
      }
    });
    return 0;
  }
#endif
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const float ny = (float(place.y + y) + .5f - cy) / halfH;
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const float nx = (float(place.x + x) + .5f - cx) / halfW;
        const float d = std::sqrt(nx * nx + ny * ny) / 1.41421356f;
        float t = std::clamp((d - start) / span, 0.f, 1.f);
        t = t * t * (3.f - 2.f * t);
        const float factor = 1.f - amount * t;
        p[x * 4] *= factor; p[x * 4 + 1] *= factor; p[x * 4 + 2] *= factor;
        if (tinted)
          for (int c = 0; c < 3; ++c) p[x * 4 + c] += tint[c] * (1.f - factor);
      }
    }
  });
  return 0;
}
const char *vignetteShader =
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float amount=clamp(values[0]/100.,0.,1.);if(amount<=0.||v.a<=0.)return v;"
    "float start=clamp(values[1]/100.,0.,1.)*.9;float soft=clamp(values[2]/100.,0.,1.);"
    "vec4 b=wavyBox;if(!(b.z>0.&&b.w>0.))b=vec4(0.,0.,s);"
    "vec2 c=b.xy+b.zw/2.;float hw=max(1.,b.z/2.),hh=max(1.,b.w/2.);"
    "float roundness=clamp(values[7]/100.,0.,1.);"
    "if(roundness>0.){float circle=sqrt(hw*hh);hw+=(circle-hw)*roundness;hh+=(circle-hh)*roundness;}"
    "vec3 tint=clamp(vec3(values[3],values[4],values[5]),0.,1.);"
    "float span=max(.05,soft*(1.-start)+.05);"
    "vec2 n=(p-c)/vec2(hw,hh);float d=sqrt(n.x*n.x+n.y*n.y)/1.41421356;"
    "float t=clamp((d-start)/span,0.,1.);t=t*t*(3.-2.*t);float f=1.-amount*t;"
    "vec3 o=v.rgb*f;if(tint.r>0.||tint.g>0.||tint.b>0.)o+=tint*(1.-f);"
    "return vec4(o,v.a);}";
const WfxParameter vignetteParameters[] = {
    number("amount", "Amount", 0, 100, 40, 1, " %"), number("size", "Size", 0, 100, 60, 1, " %"),
    number("softness", "Softness", 0, 100, 60, 1, " %"), color("color", "Colour", 0, 0, 0, 1),
    number("roundness", "Roundness", 0, 100, 0, 1, " %")};
const char *const vignetteHelp[] = {
    "How far the edges go toward the colour", "How much of the middle is left untouched",
    "How gradually the edge fades in", "What the edges turn toward: black darkens, white fades out, a colour tints",
    "0 follows the layer's proportions, 100 is a circle"};

// ---- Tile Repeat -------------------------------------------------------------
// The layer repeated as tiles: smaller than itself to make a pattern, spaced
// apart by a gap, carried past its own edges to fill the frame behind a layer
// that turns or moves, mirrored so neighbours meet without a seam, staggered
// like bricks, every other row sent the other way, and shifted in steps -- a
// step is one tile and the gap after it, and 100% is one whole step, so keying
// 0 to 100% scrolls the pattern in a loop that closes exactly, whatever size
// the layer is. Every tile is the whole layer, so any pixel can come from
// anywhere in it: the host hands over the whole layer (WFX_WHOLE_LAYER), and
// the room it continues into around the layer is Extend (reach).
// Laid out in Effect Setting as twirls (SDK 7); the groups take no values, so
// the values stay in the order projects have always kept them.
const WfxParameter tileParameters[] = {
    number("tileWidth", "Tile Width", 1, 1000, 100, 1, " %"), number("tileHeight", "Tile Height", 1, 1000, 100, 1, " %"),
    group("Gap"),
    number("gapX", "Gap X", 0, 1000, 0, 1, " %"), number("gapY", "Gap Y", 0, 1000, 0, 1, " %"),
    groupEnd(),
    group("Shift"),
    number("shiftX", "Shift X", -100000, 100000, 0, 1, " %"), number("shiftY", "Shift Y", -100000, 100000, 0, 1, " %"),
    groupEnd(),
    group("Edges"),
    number("extend", "Extend", 0, 4000, 0, 1, " px"), toggle("mirror", "Mirror Edges", false),
    groupEnd(),
    group("Stagger"),
    number("stagger", "Stagger", 0, 100, 0, 1, " %"), choice("staggerAlong", "Stagger Along", "Rows\nColumns", 2, 0),
    groupEnd(),
    group("Alternate"),
    number("alternate", "Alternate", -100000, 100000, 0, 1, " %"), toggle("alternateUpDown", "Columns Up / Down", false),
    groupEnd()};
// What each line of Tile Repeat is for, in the order above.
const char *const tileHelp[] = {
    "How wide each tile is, as a part of the layer's width (100% = the layer itself).",
    "How tall each tile is, as a part of the layer's height.",
    "Empty space between the tiles.",
    "Space between tiles side by side, as a part of a tile's width.",
    "Space between rows of tiles, as a part of a tile's height.",
    nullptr,
    "Slides the whole pattern. 100% is one tile and its gap, so keying 0 to 100% loops without a jump.",
    "Slides the pattern sideways, in tiles (100% = one tile and its gap).",
    "Slides the pattern up or down, in tiles (100% = one tile and its gap).",
    nullptr,
    "How far the tiles carry on past the layer, and how neighbouring tiles meet.",
    "How far past the layer's own edges the tiles continue, in pixels. At 0 only the layer's own box is filled.",
    "Every other tile is flipped, so neighbours meet without a seam.",
    nullptr,
    "Offsets every other row (or column) once, like bricks.",
    "How far every other row is offset, as a part of a tile (50% = bricks).",
    "Offset every other row, or every other column.",
    nullptr,
    "Moves neighbouring rows the opposite way: row A left, row B right, row A left...",
    "How far row A moves left and row B moves right, in tiles. Key it 0 to 100% for a seamless loop.",
    "Columns instead of rows: column A moves up, column B moves down.",
    nullptr};
static_assert(sizeof(tileHelp) / sizeof(tileHelp[0]) == sizeof(tileParameters) / sizeof(tileParameters[0]),
              "one help line for each entry of tileParameters");
// Stagger Along says which way to stagger; with no stagger it has nothing to do.
uint32_t tileControlState(const float *v, int32_t n, int32_t parameter) {
  constexpr int32_t kStaggerAlong = 16;
  return parameter == kStaggerAlong && value(v, n, 8, 0) == 0 ? uint32_t(WFX_CONTROL_DIMMED) : 0u;
}
struct TileValues {
  double width, height, gapX, gapY, shiftX, shiftY, extend, stagger, alternate; // gaps in tiles, shifts in steps
  bool mirror, columns, upDown;
};
TileValues tileValues(const float *v, int32_t n) {
  return {std::clamp(double(value(v, n, 0, 100)), 1., 1000.) / 100.,
          std::clamp(double(value(v, n, 1, 100)), 1., 1000.) / 100.,
          std::clamp(double(value(v, n, 2, 0)), 0., 1000.) / 100.,
          std::clamp(double(value(v, n, 3, 0)), 0., 1000.) / 100.,
          std::clamp(double(value(v, n, 4, 0)), -100000., 100000.) / 100.,
          std::clamp(double(value(v, n, 5, 0)), -100000., 100000.) / 100.,
          std::clamp(double(value(v, n, 6, 0)), 0., 4000.),
          std::clamp(double(value(v, n, 8, 0)), 0., 100.) / 100.,
          std::clamp(double(value(v, n, 10, 0)), -100000., 100000.) / 100.,
          value(v, n, 7, 0) >= .5f, value(v, n, 9, 0) >= .5f, value(v, n, 11, 0) >= .5f};
}
int tileInert(const float *v, int32_t n) {
  // One tile the size of the layer, where the layer is, going nowhere else:
  // the layer as it was. (Mirroring a single tile's neighbours changes nothing
  // no one can see.)
  const TileValues t = tileValues(v, n);
  return t.width == 1 && t.height == 1 && t.gapX == 0 && t.gapY == 0 && t.shiftX == 0 && t.shiftY == 0 &&
         t.extend == 0 && t.stagger == 0 && t.alternate == 0;
}
double tileReach(const float *v, int32_t n, double) {
  // Rounded up so dragging Extend does not resize the raster at every step.
  const double extend = tileValues(v, n).extend;
  return extend > 0 ? std::ceil(extend / 16.) * 16. : 0;
}
int tileProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32 || width <= 0 || height <= 0) return 1;
  const TileValues t = tileValues(v, n);
  const double scale = context->scale > 0 ? context->scale : 1.;
  // The layer's own pixels, which every tile shows. Whole pixels of the raster.
  const int bx0 = std::clamp(int(std::floor(context->boxX + 1e-6)), 0, width), by0 = std::clamp(int(std::floor(context->boxY + 1e-6)), 0, height);
  const int bx1 = std::clamp(int(std::ceil(context->boxX + context->boxWidth - 1e-6)), 0, width);
  const int by1 = std::clamp(int(std::ceil(context->boxY + context->boxHeight - 1e-6)), 0, height);
  const int bw = bx1 - bx0, bh = by1 - by0;
  if (bw <= 0 || bh <= 0) return 0;
  std::vector<float> layer(size_t(bw) * size_t(bh) * 4);
  for (int y = 0; y < bh; ++y) std::memcpy(&layer[size_t(y) * size_t(bw) * 4], row(pixels, stride, by0 + y) + size_t(bx0) * 4, size_t(bw) * 16);
  // A tile is the whole layer at Tile Width x Tile Height of its exact size
  // (not its size rounded to whole pixels, or a loop of one tile would creep),
  // centred on the layer's centre and shifted by parts of a step. A step is
  // the tile and the gap after it; the tile sits in the middle of its step.
  const double boxX = context->boxX, boxY = context->boxY, boxW = context->boxWidth, boxH = context->boxHeight;
  const double tileW = boxW * t.width, tileH = boxH * t.height;
  const double stepW = tileW * (1 + t.gapX), stepH = tileH * (1 + t.gapY);
  const double centreX = boxX + boxW / 2. + t.shiftX * stepW, centreY = boxY + boxH / 2. + t.shiftY * stepH;
  const bool gaps = t.gapX > 0 || t.gapY > 0;
  // Where tiles go: the layer grown by Extend, within the raster (which the
  // host may have cut to the part of the frame being drawn).
  const double grow = t.extend * scale;
  const int fx0 = std::clamp(int(std::floor(bx0 - grow)), 0, width), fx1 = std::clamp(int(std::ceil(bx1 + grow)), 0, width);
  const int fy0 = std::clamp(int(std::floor(by0 - grow)), 0, height), fy1 = std::clamp(int(std::ceil(by1 + grow)), 0, height);
  // A pixel's place along the pattern, in steps. A box with no width or height
  // that still covers a pixel (0 wide, but starting part way into one), or one
  // so thin that a place runs past every number, makes tiles with nowhere to
  // take their pixels from: the layer is left as it was, the way a box covering
  // no pixel is. A place grows steadily across the area filled, so its two
  // ends tell.
  auto place = [](double at, double centre, double step) { return (at + .5 - centre) / step + .5; };
  if (!std::isfinite(place(fx0, centreX, stepW)) || !std::isfinite(place(fx1 - 1, centreX, stepW)) ||
      !std::isfinite(place(fy0, centreY, stepH)) || !std::isfinite(place(fy1 - 1, centreY, stepH)))
    return 0;
  auto odd = [](double index) { const long long i = (long long)std::floor(index); return ((i % 2) + 2) % 2 == 1; };
  // Bilinear, held inside the layer: a tile's edge takes the layer's edge
  // pixels, not the empty room around it, so tiles meet without a dark seam.
  auto sample = [&](double sx, double sy, float *out) {
    sx = std::clamp(sx - .5, 0., bw - 1.);
    sy = std::clamp(sy - .5, 0., bh - 1.);
    const int x0 = int(sx), y0 = int(sy);
    const int x1 = std::min(x0 + 1, bw - 1), y1 = std::min(y0 + 1, bh - 1);
    const float tx = float(sx - x0), ty = float(sy - y0);
    const float *a = &layer[(size_t(y0) * size_t(bw) + size_t(x0)) * 4], *b = &layer[(size_t(y0) * size_t(bw) + size_t(x1)) * 4];
    const float *c = &layer[(size_t(y1) * size_t(bw) + size_t(x0)) * 4], *d = &layer[(size_t(y1) * size_t(bw) + size_t(x1)) * 4];
    for (int k = 0; k < 4; ++k) out[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
  };
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *line = row(pixels, stride, y);
      if (y < fy0 || y >= fy1) { std::memset(line, 0, size_t(width) * 16); continue; }
      std::memset(line, 0, size_t(fx0) * 16);
      std::memset(line + size_t(fx1) * 4, 0, size_t(width - fx1) * 16);
      const double v0 = (y + .5 - centreY) / stepH + .5;
      for (int32_t x = fx0; x < fx1; ++x) {
        double u = (x + .5 - centreX) / stepW + .5, vv = v0;
        // Every other row (or column) moved along by part of a step: bricks.
        if (t.stagger > 0) {
          if (!t.columns) { if (odd(vv)) u += t.stagger; }
          else if (odd(u)) vv += t.stagger;
        }
        // Every other row the other way: row A goes left as B goes right --
        // or, Up / Down, column A goes up as B goes down.
        if (t.alternate != 0) {
          if (!t.upDown) u += odd(vv) ? -t.alternate : t.alternate;
          else vv += odd(u) ? -t.alternate : t.alternate;
        }
        double fu = u - std::floor(u), fv = vv - std::floor(vv);
        float *out = line + size_t(x) * 4;
        double cover = 1;
        if (gaps) {
          // Where the pixel falls in its tile; outside 0..1 is the gap. A tile's
          // edge covers the pixels it crosses in proportion, so it stays smooth
          // wherever it lands between pixels. Only across a gap: where tiles
          // meet edge to edge, each pixel is whole.
          auto across = [](double &f, double gap, double size) {
            if (gap <= 0) return 1.;
            f = (f - .5) * (1 + gap) + .5;
            const double at = f * size;
            f = std::clamp(f, 0., 1.);
            return std::clamp(std::min(at + .5, size) - std::max(at - .5, 0.), 0., 1.);
          };
          cover = across(fu, t.gapX, tileW) * across(fv, t.gapY, tileH);
          if (cover <= 0) { std::memset(out, 0, 16); continue; }
        }
        if (t.mirror) {
          if (odd(u)) fu = 1 - fu;
          if (odd(vv)) fv = 1 - fv;
        }
        sample(boxX - bx0 + fu * boxW, boxY - by0 + fv * boxH, out);
        // Premultiplied, so the part of the pixel the tile covers is all four.
        if (cover < 1)
          for (int k = 0; k < 4; ++k) out[k] *= float(cover);
      }
    }
  });
  return 0;
}
// On the card, the same tiling at every pixel. Shift and Alternate count
// steps, and the pattern comes round again every two (odd rows and mirrored
// tiles come in pairs), so they are taken round to two steps first -- on the
// percent itself, which takes nothing off a whole float -- and a long scroll
// keeps single precision's worth of position.
const char *tileShader =
    "vec4 tlTexel(int x,int y){vec4 t=wavyPixel(ivec2(x,y));return vec4(t.rgb*t.a,t.a);}"
    "bool tlOdd(float index){float i=floor(index);return i-2.*floor(i*.5)==1.;}"
    "float tlAcross(inout float f,float gap,float size){if(gap<=0.)return 1.;f=(f-.5)*(1.+gap)+.5;float at=f*size;"
    "f=clamp(f,0.,1.);return clamp(min(at+.5,size)-max(at-.5,0.),0.,1.);}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float tw=clamp(values[0],1.,1000.)/100.,th=clamp(values[1],1.,1000.)/100.;"
    "float gapX=clamp(values[2],0.,1000.)/100.,gapY=clamp(values[3],0.,1000.)/100.;"
    "float shiftX=mod(clamp(values[4],-100000.,100000.),200.)/100.,shiftY=mod(clamp(values[5],-100000.,100000.),200.)/100.;"
    "float grow=clamp(values[6],0.,4000.)*(wavyScale>0.?wavyScale:1.),stagger=clamp(values[8],0.,100.)/100.;"
    "float alternate=mod(clamp(values[10],-100000.,100000.),200.)/100.;"
    "bool mirror=values[7]>=.5,columns=values[9]>=.5,upDown=values[11]>=.5;"
    "vec4 b=wavyBox;int W=int(s.x),H=int(s.y);"
    "int bx0=clamp(int(floor(b.x+1e-6)),0,W),by0=clamp(int(floor(b.y+1e-6)),0,H);"
    "int bx1=clamp(int(ceil(b.x+b.z-1e-6)),0,W),by1=clamp(int(ceil(b.y+b.w-1e-6)),0,H);"
    "int bw=bx1-bx0,bh=by1-by0;if(bw<=0||bh<=0)return v.a>0.?v:vec4(0.);"
    "float tileW=b.z*tw,tileH=b.w*th,stepW=tileW*(1.+gapX),stepH=tileH*(1.+gapY);"
    "float centreX=b.x+b.z/2.+shiftX*stepW,centreY=b.y+b.w/2.+shiftY*stepH;"
    "int fx0=clamp(int(floor(float(bx0)-grow)),0,W),fx1=clamp(int(ceil(float(bx1)+grow)),0,W);"
    "int fy0=clamp(int(floor(float(by0)-grow)),0,H),fy1=clamp(int(ceil(float(by1)+grow)),0,H);"
    "ivec2 at=ivec2(p);if(at.x<fx0||at.x>=fx1||at.y<fy0||at.y>=fy1)return vec4(0.);"
    "float u=(p.x-centreX)/stepW+.5,vv=(p.y-centreY)/stepH+.5;"
    "if(stagger>0.){if(!columns){if(tlOdd(vv))u+=stagger;}else if(tlOdd(u))vv+=stagger;}"
    "if(alternate!=0.){if(!upDown)u+=tlOdd(vv)?-alternate:alternate;else vv+=tlOdd(u)?-alternate:alternate;}"
    "float fu=u-floor(u),fv=vv-floor(vv),cover=1.;"
    "if(gapX>0.||gapY>0.){cover=tlAcross(fu,gapX,tileW)*tlAcross(fv,gapY,tileH);if(cover<=0.)return vec4(0.);}"
    "if(mirror){if(tlOdd(u))fu=1.-fu;if(tlOdd(vv))fv=1.-fv;}"
    "float sx=clamp(b.x-float(bx0)+fu*b.z-.5,0.,float(bw-1)),sy=clamp(b.y-float(by0)+fv*b.w-.5,0.,float(bh-1));"
    "int x0=int(sx),y0=int(sy),x1=min(x0+1,bw-1),y1=min(y0+1,bh-1);float tx=sx-float(x0),ty=sy-float(y0);"
    "vec4 o=(tlTexel(bx0+x0,by0+y0)*(1.-tx)+tlTexel(bx0+x1,by0+y0)*tx)*(1.-ty)+"
    "(tlTexel(bx0+x0,by0+y1)*(1.-tx)+tlTexel(bx0+x1,by0+y1)*tx)*ty;"
    "o*=cover;return o.a>0.?vec4(o.rgb/o.a,o.a):vec4(0.);}";

// ---- Gradient Map --------------------------------------------------------------
// Each pixel's brightness picks a colour along a ramp of three -- shadows,
// midtones, highlights -- the duotone and tritone looks of posters and music
// videos. Pixel by pixel, so it joins a run of grades over 8-bit pixels.
enum { MapShadows = 0, MapMidtones = 4, MapHighlights = 8, MapMidpoint = 12, MapAmount = 13 };
struct MapState {
  float shadows[3], midtones[3], highlights[3], midpoint, amount;
  void apply(float *p, int32_t count) const {
    for (int32_t x = 0; x < count; ++x) {
      if (p[x * 4 + 3] <= 0.f) continue;
      const float luma = std::clamp(.2126f * p[x * 4] + .7152f * p[x * 4 + 1] + .0722f * p[x * 4 + 2], 0.f, 1.f);
      for (int c = 0; c < 3; ++c) {
        const float mapped = luma < midpoint
                                 ? shadows[c] + (midtones[c] - shadows[c]) * (luma / midpoint)
                                 : midtones[c] + (highlights[c] - midtones[c]) * ((luma - midpoint) / (1.f - midpoint));
        p[x * 4 + c] = p[x * 4 + c] * (1.f - amount) + mapped * amount;
      }
    }
  }
};
MapState mapFrom(const float *v, int32_t n) {
  MapState s{};
  for (int c = 0; c < 3; ++c) {
    s.shadows[c] = value(v, n, MapShadows + c, c == 2 ? .22f : .06f);
    s.midtones[c] = value(v, n, MapMidtones + c, c == 0 ? .93f : c == 1 ? .27f : .45f);
    s.highlights[c] = value(v, n, MapHighlights + c, c == 2 ? .78f : .98f);
  }
  s.midpoint = std::clamp(divided(v, n, MapMidpoint, 50, 100.), .01f, .99f);
  s.amount = std::clamp(divided(v, n, MapAmount, 100, 100.), 0.f, 1.f);
  return s;
}
int mapInert(const float *v, int32_t n) { return std::clamp(divided(v, n, MapAmount, 100, 100.), 0.f, 1.f) <= 0; }
void *mapPrepare(const float *v, int32_t n, const WfxContext *) { return new (std::nothrow) MapState(mapFrom(v, n)); }
void mapSpan(float *p, int32_t count, const void *s) { static_cast<const MapState *>(s)->apply(p, count); }
void mapRelease(void *s) { delete static_cast<MapState *>(s); }
int mapProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  if (mapInert(v, n)) return 0;
  const MapState state = mapFrom(v, n);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) state.apply(row(pixels, stride, y), width);
  });
  return 0;
}
const WfxParameter mapParameters[] = {
    color("shadows", "Shadows", .06f, .06f, .22f, 1), color("midtones", "Midtones", .93f, .27f, .45f, 1),
    color("highlights", "Highlights", .98f, .98f, .78f, 1), number("midpoint", "Midpoint", 1, 99, 50, 1, " %"),
    number("amount", "Amount", 0, 100, 100, 1, " %")};
const char *mapShader =
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){float a=clamp(values[13]/100.,0.,1.);float m=clamp(values[12]/100.,.01,.99);"
    "float L=clamp(dot(v.rgb,vec3(.2126,.7152,.0722)),0.,1.);"
    "vec3 S=vec3(values[0],values[1],values[2]),M=vec3(values[4],values[5],values[6]),H=vec3(values[8],values[9],values[10]);"
    "vec3 c=L<m?S+(M-S)*(L/m):M+(H-M)*((L-m)/(1.-m));return vec4(v.rgb*(1.-a)+c*a,v.a);}";
const char *const mapHelp[] = {"The colour the darkest parts become", "The colour the middle tones become",
                               "The colour the brightest parts become", "Which brightness gets the Midtones colour",
                               "How much of the mapped colour replaces the picture"};

// ---- Posterize -------------------------------------------------------------------
// Each channel held to a few levels: flat bands of colour, a screen-print look.
struct PosterizeState {
  float levels;
  void apply(float *p, int32_t count) const {
    for (int32_t x = 0; x < count; ++x) {
      if (p[x * 4 + 3] <= 0.f) continue;
      for (int c = 0; c < 3; ++c)
        p[x * 4 + c] = std::floor(std::clamp(p[x * 4 + c], 0.f, 1.f) * (levels - 1.f) + .5f) / (levels - 1.f);
    }
  }
};
float posterizeLevels(const float *v, int32_t n) { return std::round(std::clamp(value(v, n, 0, 6), 2.f, 256.f)); }
int posterizeInert(const float *v, int32_t n) { return posterizeLevels(v, n) >= 256.f; }
void *posterizePrepare(const float *v, int32_t n, const WfxContext *) { return new (std::nothrow) PosterizeState{posterizeLevels(v, n)}; }
void posterizeSpan(float *p, int32_t count, const void *s) { static_cast<const PosterizeState *>(s)->apply(p, count); }
void posterizeRelease(void *s) { delete static_cast<PosterizeState *>(s); }
int posterizeProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  if (posterizeInert(v, n)) return 0;
  const PosterizeState state{posterizeLevels(v, n)};
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) state.apply(row(pixels, stride, y), width);
  });
  return 0;
}
const WfxParameter posterizeParameters[] = {number("levels", "Levels", 2, 256, 6, 1)};
const char *posterizeShader =
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){float l=floor(clamp(values[0],2.,256.)+.5)-1.;"
    "return vec4(floor(clamp(v.rgb,0.,1.)*l+.5)/l,v.a);}";
const char *const posterizeHelp[] = {"How many steps each colour channel keeps (256 leaves the picture alone)"};

// ---- Halftone ----------------------------------------------------------------------
// The picture as printed dots on a turned screen: the darker a place, the
// bigger its dot. Each pixel is measured against the dot of the screen cell it
// falls in, so it works pixel by pixel; in Colour, red, green and blue get dots
// of their own, the way inks overprint.
enum { HalfSize, HalfAngle, HalfMode, HalfSoftness, HalfAmount };
int halftoneInert(const float *v, int32_t n) { return std::clamp(divided(v, n, HalfAmount, 100, 100.), 0.f, 1.f) <= 0; }
int halftoneProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n,
                    const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float amount = std::clamp(divided(v, n, HalfAmount, 100, 100.), 0.f, 1.f);
  if (amount <= 0.f) return 0;
  const float size = float(std::max(1., std::clamp(double(value(v, n, HalfSize, 10)), 2., 400.) * context->scale));
  const double angle = double(value(v, n, HalfAngle, 45)) * 3.14159265358979323846 / 180.;
  const float cosA = float(std::cos(angle)), sinA = float(std::sin(angle));
  const int mode = std::clamp(int(std::lround(value(v, n, HalfMode, 0))), 0, 2);
  // An edge about a pixel wide at least, so the dots are smooth; Softness widens it.
  const float soft = std::max(2.f / size, std::clamp(divided(v, n, HalfSoftness, 20, 100.), 0.f, 1.f) * .5f);
  const RasterPlace place = rasterPlace(context, width, height);
  // The screen's spot function: 1 in the middle of a cell, 0 at its corners. A
  // place is inked where the spot rises above its lightness, so dots grow with
  // darkness, touch as a chequerboard at half and cover the cell at black.
  auto ink = [&](float darkness, float spot) {
    return std::clamp((spot - (1.f - std::clamp(darkness, 0.f, 1.f))) / soft + .5f, 0.f, 1.f);
  };
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const float py = float(place.y + y) + .5f;
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const float px = float(place.x + x) + .5f;
        const float u = (px * cosA + py * sinA) / size, w = (-px * sinA + py * cosA) / size;
        const float tau = 6.28318531f;
        const float spotValue = .5f + .25f * (std::cos(tau * u) + std::cos(tau * w));
        float out[3];
        if (mode == 0) {
          for (int c = 0; c < 3; ++c) out[c] = 1.f - ink(1.f - std::clamp(p[x * 4 + c], 0.f, 1.f), spotValue);
        } else {
          const float luma = std::clamp(.2126f * p[x * 4] + .7152f * p[x * 4 + 1] + .0722f * p[x * 4 + 2], 0.f, 1.f);
          const float dot = mode == 1 ? 1.f - ink(1.f - luma, spotValue) : ink(luma, spotValue);
          out[0] = out[1] = out[2] = dot;
        }
        for (int c = 0; c < 3; ++c) p[x * 4 + c] = p[x * 4 + c] * (1.f - amount) + out[c] * amount;
      }
    }
  });
  return 0;
}
const WfxParameter halftoneParameters[] = {
    number("size", "Dot Size", 2, 400, 10, .5f, " px"), number("angle", "Screen Angle", -360, 360, 45, 1, "\xc2\xb0"),
    choice("mode", "Dots", "Colour\nBlack on white\nWhite on black", 3, 0), number("softness", "Softness", 0, 100, 20, 1, " %"),
    number("amount", "Amount", 0, 100, 100, 1, " %")};
const char *const halftoneHelp[] = {"The distance from one dot to the next, in layer pixels", "How the rows of dots are turned",
                                    "Colour: a dot per channel, like inks; or one set of dots in black or white",
                                    "How soft the edge of each dot is", "How much of the dotted picture replaces the picture"};
// On the card: the same screen, each cell's phase taken round to one turn
// before the cosine, which the card has good only near nought.
const char *halftoneShader =
    "float htInk(float darkness,float spot,float soft){return clamp((spot-(1.-clamp(darkness,0.,1.)))/soft+.5,0.,1.);}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float amount=clamp(values[4]/100.,0.,1.);if(amount<=0.||v.a<=0.)return v;"
    "float size=max(1.,clamp(values[0],2.,400.)*wavyScale);"
    "float angle=mod(values[1],360.)*3.14159265358979/180.;float cA=cos(angle),sA=sin(angle);"
    "int mode=clamp(int(floor(values[2]+.5)),0,2);float soft=max(2./size,clamp(values[3]/100.,0.,1.)*.5);"
    "float u=(p.x*cA+p.y*sA)/size,w=(-p.x*sA+p.y*cA)/size;"
    "float spot=.5+.25*(cos(6.28318531*(u-floor(u)))+cos(6.28318531*(w-floor(w))));vec3 o;"
    "if(mode==0){for(int c=0;c<3;++c)o[c]=1.-htInk(1.-clamp(v[c],0.,1.),spot,soft);}"
    "else{float l=clamp(.2126*v.r+.7152*v.g+.0722*v.b,0.,1.);o=vec3(mode==1?1.-htInk(1.-l,spot,soft):htInk(l,spot,soft));}"
    "return vec4(v.rgb*(1.-amount)+o*amount,v.a);}";

// ---- Scanlines -------------------------------------------------------------------
// The dark lines of a cathode-ray screen, at any angle, drifting if they have a
// speed, with the screen's red-green-blue stripes if asked for.
enum { ScanSpacing, ScanThickness, ScanIntensity, ScanAngle, ScanSpeed, ScanMask, ScanSoftness };
int scanInert(const float *v, int32_t n) { return std::clamp(divided(v, n, ScanIntensity, 40, 100.), 0.f, 1.f) <= 0; }
int scanVaries(const float *v, int32_t n) { return std::abs(value(v, n, ScanSpeed, 0)) > 1e-6f; }
int scanProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float intensity = std::clamp(divided(v, n, ScanIntensity, 40, 100.), 0.f, 1.f);
  if (intensity <= 0.f) return 0;
  const float spacing = float(std::max(1., std::clamp(double(value(v, n, ScanSpacing, 4)), 1., 400.) * context->scale));
  const float thickness = std::clamp(divided(v, n, ScanThickness, 50, 100.), 0.f, 1.f);
  const double angle = double(value(v, n, ScanAngle, 0)) * 3.14159265358979323846 / 180.;
  const float cosA = float(std::cos(angle)), sinA = float(std::sin(angle));
  const float drift = float(std::fmod(double(value(v, n, ScanSpeed, 0)) * context->scale * context->seconds, double(spacing)));
  const bool mask = value(v, n, ScanMask, 0) > .5f;
  const float soft = std::max(1.f / spacing, std::clamp(divided(v, n, ScanSoftness, 50, 100.), 0.f, 1.f) * .5f);
  const float stripe = std::max(1.f, spacing / 3.f);
  const RasterPlace place = rasterPlace(context, width, height);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const float py = float(place.y + y) + .5f;
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const float px = float(place.x + x) + .5f;
        // Across the lines, in lines: the fraction of a line this pixel is at.
        float across = (-px * sinA + py * cosA - drift) / spacing;
        across -= std::floor(across);
        // Dark in the middle `thickness` of every line, with soft shoulders.
        const float fromMiddle = std::abs(across - .5f) * 2.f; // 0 middle of the line, 1 between lines
        const float dark = std::clamp((thickness - fromMiddle) / soft + .5f, 0.f, 1.f) * intensity;
        float keep[3] = {1.f - dark, 1.f - dark, 1.f - dark};
        if (mask) {
          const float along = px * cosA + py * sinA;
          const int lit = int(std::floor(along / stripe)) % 3;
          for (int c = 0; c < 3; ++c)
            if (c != (lit + 3) % 3) keep[c] *= 1.f - .6f * intensity;
        }
        for (int c = 0; c < 3; ++c) p[x * 4 + c] *= keep[c];
      }
    }
  });
  return 0;
}
const WfxParameter scanParameters[] = {
    number("spacing", "Spacing", 1, 400, 4, .5f, " px"), number("thickness", "Thickness", 0, 100, 50, 1, " %"),
    number("intensity", "Intensity", 0, 100, 40, 1, " %"), number("angle", "Angle", -360, 360, 0, 1, "\xc2\xb0"),
    number("speed", "Scroll", -4000, 4000, 0, 1, " px/s"), toggle("mask", "RGB Mask", false),
    number("softness", "Softness", 0, 100, 50, 1, " %")};
const char *const scanHelp[] = {"The distance from one line to the next, in layer pixels", "How much of each line is dark",
                                "How dark the lines are", "0 runs the lines across, 90 up and down",
                                "How fast the lines drift, in layer pixels a second", "The red, green and blue stripes of a screen",
                                "How soft the edges of the lines are"};
// On the card: the drift taken off the way fmod does (toward nought), and the
// lit stripe counted from a place a sixth of a stripe off any whole number, so
// the card's rougher division cannot tip it into the next one.
const char *scanShader =
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float intensity=clamp(values[2]/100.,0.,1.);if(intensity<=0.||v.a<=0.)return v;"
    "float spacing=max(1.,clamp(values[0],1.,400.)*wavyScale);float thickness=clamp(values[1]/100.,0.,1.);"
    "float angle=mod(values[3],360.)*3.14159265358979/180.;float cA=cos(angle),sA=sin(angle);"
    "float travel=values[4]*wavyScale*wavySeconds;float drift=travel-spacing*trunc(travel/spacing);"
    "float soft=max(1./spacing,clamp(values[6]/100.,0.,1.)*.5);float stripe=max(1.,spacing/3.);"
    "float across=(-p.x*sA+p.y*cA-drift)/spacing;across-=floor(across);"
    "float dark=clamp((thickness-abs(across-.5)*2.)/soft+.5,0.,1.)*intensity;vec3 keep=vec3(1.-dark);"
    "if(values[5]>.5){float f=floor((p.x*cA+p.y*sA)/stripe);int lit=int(f-3.*floor((f+.5)/3.));"
    "for(int c=0;c<3;++c)if(c!=lit)keep[c]*=1.-.6*intensity;}"
    "return vec4(v.rgb*keep,v.a);}";

// ---- Light Sweep -------------------------------------------------------------------
// A band of light passing over the layer -- the shine that runs across a title
// or a logo. Only what the layer covers lights up. Position puts the band
// across the layer's box (0 just before its first edge, 100 just past its
// last); Speed sends it across by itself, that many times a second.
enum { SweepPosition, SweepAngle, SweepWidth, SweepSoftness, SweepIntensity, SweepColour,
       SweepSpeed = SweepColour + 4 };
int sweepInert(const float *v, int32_t n) { return std::clamp(divided(v, n, SweepIntensity, 80, 100.), 0.f, 2.f) <= 0; }
int sweepVaries(const float *v, int32_t n) { return std::abs(value(v, n, SweepSpeed, 0)) > 1e-6f; }
int sweepProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float intensity = std::clamp(divided(v, n, SweepIntensity, 80, 100.), 0.f, 2.f);
  if (intensity <= 0.f) return 0;
  const RasterPlace place = rasterPlace(context, width, height);
  double bx = context->boxX, by = context->boxY, bw = context->boxWidth, bh = context->boxHeight;
  if (!(bw > 0 && bh > 0)) { bx = 0; by = 0; bw = place.width; bh = place.height; }
  const double angle = double(value(v, n, SweepAngle, 15)) * 3.14159265358979323846 / 180.;
  const float dx = float(std::cos(angle)), dy = float(std::sin(angle)); // the way the light travels
  const float half = float(std::max(1., std::clamp(double(value(v, n, SweepWidth, 80)), 1., 20000.) * context->scale)) / 2.f;
  const float soft = std::clamp(divided(v, n, SweepSoftness, 70, 100.), 0.f, 1.f);
  const float core = half * (1.f - soft);
  double position = double(value(v, n, SweepPosition, 50)) / 100.;
  const double speed = value(v, n, SweepSpeed, 0);
  if (std::abs(speed) > 1e-6) { const double turns = position + speed * context->seconds; position = turns - std::floor(turns); }
  float low = 1e30f, high = -1e30f;
  const double corners[4][2] = {{bx, by}, {bx + bw, by}, {bx, by + bh}, {bx + bw, by + bh}};
  for (const auto &corner : corners) {
    const float along = float(corner[0]) * dx + float(corner[1]) * dy;
    low = std::min(low, along); high = std::max(high, along);
  }
  const float middle = float(low - half + (high - low + 2.f * half) * position);
  const float tint[3] = {std::clamp(value(v, n, SweepColour, 1), 0.f, 1.f), std::clamp(value(v, n, SweepColour + 1, 1), 0.f, 1.f),
                         std::clamp(value(v, n, SweepColour + 2, 1), 0.f, 1.f)};
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const float py = float(place.y + y) + .5f;
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const float px = float(place.x + x) + .5f;
        const float distance = std::abs(px * dx + py * dy - middle);
        if (distance >= half) continue;
        float k = distance <= core ? 1.f : (half - distance) / std::max(1e-3f, half - core);
        k = k * k * (3.f - 2.f * k) * intensity;
        for (int c = 0; c < 3; ++c) p[x * 4 + c] = std::min(1.f, p[x * 4 + c] + tint[c] * k);
      }
    }
  });
  return 0;
}
const WfxParameter sweepParameters[] = {
    number("position", "Position", -100, 200, 50, .5f, " %"), number("angle", "Direction", -3600, 3600, 15, 1, "\xc2\xb0"),
    number("width", "Width", 1, 20000, 80, 1, " px"), number("softness", "Softness", 0, 100, 70, 1, " %"),
    number("intensity", "Intensity", 0, 200, 80, 1, " %"), color("color", "Colour", 1, 1, 1, 1),
    number("speed", "Speed (0 = by Position)", -20, 20, 0, .05f, " /s")};
const char *const sweepHelp[] = {
    "Where the band is across the layer: key it from 0 to 100 for one pass", "Which way the light travels (0 is to the right)",
    "How wide the band of light is, in layer pixels", "How gradually the band fades at its edges", "How bright the light is",
    "The colour of the light", "Passes a second by itself (0 leaves it to Position)"};
const char *sweepShader =
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float intensity=clamp(values[4]/100.,0.,2.);if(intensity<=0.||v.a<=0.)return v;"
    "vec4 b=wavyBox;if(!(b.z>0.&&b.w>0.))b=vec4(0.,0.,s);"
    "float angle=mod(values[1],360.)*3.14159265358979/180.;float dx=cos(angle),dy=sin(angle);"
    "float hw=max(1.,clamp(values[2],1.,20000.)*wavyScale)/2.;float soft=clamp(values[3]/100.,0.,1.);float core=hw*(1.-soft);"
    "float position=values[0]/100.;"
    "if(abs(values[9])>1e-6){float turns=position+values[9]*wavySeconds;position=turns-floor(turns);}"
    "float a0=b.x*dx+b.y*dy,a1=(b.x+b.z)*dx+b.y*dy,a2=b.x*dx+(b.y+b.w)*dy,a3=(b.x+b.z)*dx+(b.y+b.w)*dy;"
    "float low=min(min(a0,a1),min(a2,a3)),high=max(max(a0,a1),max(a2,a3));"
    "float middle=low-hw+(high-low+2.*hw)*position;"
    "float gap=abs(p.x*dx+p.y*dy-middle);if(gap>=hw)return v;"
    "float k=gap<=core?1.:(hw-gap)/max(1e-3,hw-core);k=k*k*(3.-2.*k)*intensity;"
    "return vec4(min(vec3(1.),v.rgb+clamp(vec3(values[5],values[6],values[7]),0.,1.)*k),v.a);}";

// ---- Threshold ---------------------------------------------------------------------
// Every pixel black or white by its brightness: a stark, photocopied look, or a
// matte made from a picture. Softness turns the hard step into a ramp.
struct ThresholdState {
  float level, soft;
  void apply(float *p, int32_t count) const {
    for (int32_t x = 0; x < count; ++x) {
      if (p[x * 4 + 3] <= 0.f) continue;
      const float luma = .2126f * p[x * 4] + .7152f * p[x * 4 + 1] + .0722f * p[x * 4 + 2];
      float out;
      if (soft <= 0.f) out = luma >= level ? 1.f : 0.f;
      else {
        const float t = std::clamp((luma - level) / soft + .5f, 0.f, 1.f);
        out = t * t * (3.f - 2.f * t);
      }
      p[x * 4] = p[x * 4 + 1] = p[x * 4 + 2] = out;
    }
  }
};
ThresholdState thresholdState(const float *v, int32_t n) {
  return {std::clamp(divided(v, n, 0, 128, 255.), 0.f, 1.f), std::clamp(divided(v, n, 1, 0, 100.), 0.f, 1.f)};
}
int thresholdInert(const float *, int32_t) { return 0; }
void *thresholdPrepare(const float *v, int32_t n, const WfxContext *) { return new (std::nothrow) ThresholdState(thresholdState(v, n)); }
void thresholdSpan(float *p, int32_t count, const void *s) { static_cast<const ThresholdState *>(s)->apply(p, count); }
void thresholdRelease(void *s) { delete static_cast<ThresholdState *>(s); }
int thresholdProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const ThresholdState state = thresholdState(v, n);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) state.apply(row(pixels, stride, y), width);
  });
  return 0;
}
const WfxParameter thresholdParameters[] = {number("level", "Level", 0, 255, 128, 1),
                                            number("softness", "Softness", 0, 100, 0, .5f, " %")};
const char *thresholdShader =
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){if(v.a<=0.)return v;float level=clamp(values[0]/255.,0.,1.);"
    "float soft=clamp(values[1]/100.,0.,1.);float l=dot(v.rgb,vec3(.2126,.7152,.0722));float o;"
    "if(soft<=0.)o=l>=level?1.:0.;else{float t=clamp((l-level)/soft+.5,0.,1.);o=t*t*(3.-2.*t);}"
    "return vec4(vec3(o),v.a);}";
const char *const thresholdHelp[] = {"How bright a pixel must be to turn white (out of 255)",
                                     "How gradually black turns to white around Level (0 is a hard step)"};

// ---- Find Edges and Emboss ---------------------------------------------------------
// Both read each pixel's neighbours, so the picture as it was is kept to read
// from while the result is written. Beyond the picture its edge pixel repeats.
struct Neighbours {
  const float *source;
  int32_t width, height;
  const float *at(int x, int y) const {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return &source[(size_t(y) * size_t(width) + size_t(x)) * 4];
  }
};
template <class F>
void withNeighbours(void *pixels, int32_t width, int32_t height, int32_t stride, const WfxContext *context, const F &each) {
  Scratch kept(size_t(width) * size_t(height) * 4);
  float *const source = kept.data();
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) std::memcpy(&source[size_t(y) * size_t(width) * 4], row(pixels, stride, y), size_t(width) * 16);
  });
  const Neighbours around{source, width, height};
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *d = row(pixels, stride, y);
      for (int32_t x = 0; x < width; ++x) each(around, x, y, d + x * 4);
    }
  });
}
// Find Edges: where the colour changes fast (a Sobel gradient per channel),
// dark lines on white, like a pencil tracing -- or, Inverted, glowing lines on
// black. Blend brings back some of the picture.
enum { EdgeInvert, EdgeBlend, EdgeStrength };
int edgesInert(const float *, int32_t) { return 0; }
int edgesProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  if (width <= 0 || height <= 0) return 0;
  const bool inverted = value(v, n, EdgeInvert, 0) > .5f;
  const float blend = std::clamp(divided(v, n, EdgeBlend, 0, 100.), 0.f, 1.f);
  const float strength = std::clamp(divided(v, n, EdgeStrength, 100, 100.), 0.f, 10.f);
  withNeighbours(pixels, width, height, stride, context, [&](const Neighbours &a, int x, int y, float *d) {
    const float *tl = a.at(x - 1, y - 1), *t = a.at(x, y - 1), *tr = a.at(x + 1, y - 1), *l = a.at(x - 1, y),
                *r = a.at(x + 1, y), *bl = a.at(x - 1, y + 1), *b = a.at(x, y + 1), *br = a.at(x + 1, y + 1), *self = a.at(x, y);
    if (self[3] <= 0.f) return;
    for (int c = 0; c < 3; ++c) {
      const float gx = (tr[c] + 2 * r[c] + br[c]) - (tl[c] + 2 * l[c] + bl[c]);
      const float gy = (bl[c] + 2 * b[c] + br[c]) - (tl[c] + 2 * t[c] + tr[c]);
      const float edge = std::min(1.f, std::sqrt(gx * gx + gy * gy) * .25f * strength);
      const float shown = inverted ? edge : 1.f - edge;
      d[c] = shown + (self[c] - shown) * blend;
    }
  });
  return 0;
}
// On the card: the same neighbours, the edge pixel repeating beyond the picture.
const char *edgesShader =
    "vec4 feAt(ivec2 q){return wavyPixel(clamp(q,ivec2(0),ivec2(sourceSize)-1));}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "if(v.a<=0.)return v;bool inverted=values[0]>.5;"
    "float blend=clamp(values[1]/100.,0.,1.),strength=clamp(values[2]/100.,0.,10.);ivec2 q=ivec2(p);"
    "vec3 tl=feAt(q+ivec2(-1,-1)).rgb,t=feAt(q+ivec2(0,-1)).rgb,tr=feAt(q+ivec2(1,-1)).rgb,l=feAt(q+ivec2(-1,0)).rgb,"
    "r=feAt(q+ivec2(1,0)).rgb,bl=feAt(q+ivec2(-1,1)).rgb,b=feAt(q+ivec2(0,1)).rgb,br=feAt(q+ivec2(1,1)).rgb;"
    "vec3 gx=(tr+2.*r+br)-(tl+2.*l+bl),gy=(bl+2.*b+br)-(tl+2.*t+tr);"
    "vec3 edge=min(vec3(1.),sqrt(gx*gx+gy*gy)*.25*strength);vec3 shown=inverted?edge:1.-edge;"
    "return vec4(shown+(v.rgb-shown)*blend,v.a);}";
const WfxParameter edgesParameters[] = {toggle("invert", "Invert", false), number("blend", "Blend with Original", 0, 100, 0, 1, " %"),
                                        number("strength", "Strength", 0, 1000, 100, 1, " %")};
const char *const edgesHelp[] = {"Glowing lines on black instead of dark lines on white",
                                 "How much of the picture shows through the lines", "How strongly a change of colour is drawn"};
// Emboss: the picture pressed into grey metal, lit from Direction: each pixel
// the difference between its neighbour toward the light and the one away,
// Relief pixels apart, on middle grey. Blend brings back the colour.
enum { EmbossDirection, EmbossRelief, EmbossContrast, EmbossBlend };
int embossInert(const float *, int32_t) { return 0; }
int embossProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  if (width <= 0 || height <= 0) return 0;
  const double angle = double(value(v, n, EmbossDirection, 135)) * 3.14159265358979323846 / 180.;
  const float relief = float(std::clamp(double(value(v, n, EmbossRelief, 2)), .5, 20.) * (context->scale > 0 ? context->scale : 1.));
  const float contrast = std::clamp(divided(v, n, EmbossContrast, 100, 100.), 0.f, 20.f);
  const float blend = std::clamp(divided(v, n, EmbossBlend, 0, 100.), 0.f, 1.f);
  const float ux = float(std::cos(angle)) * relief, uy = -float(std::sin(angle)) * relief;
  withNeighbours(pixels, width, height, stride, context, [&](const Neighbours &a, int x, int y, float *d) {
    const float *self = a.at(x, y);
    if (self[3] <= 0.f) return;
    // Read between pixels: the light can come from any angle.
    auto read = [&](float fx, float fy, int c) {
      const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
      const float tx = fx - float(x0), ty = fy - float(y0);
      return (a.at(x0, y0)[c] * (1 - tx) + a.at(x0 + 1, y0)[c] * tx) * (1 - ty) +
             (a.at(x0, y0 + 1)[c] * (1 - tx) + a.at(x0 + 1, y0 + 1)[c] * tx) * ty;
    };
    float lit = 0;
    for (int c = 0; c < 3; ++c) {
      const float weight = c == 0 ? .2126f : c == 1 ? .7152f : .0722f;
      lit += weight * (read(float(x) + ux, float(y) + uy, c) - read(float(x) - ux, float(y) - uy, c));
    }
    const float grey = std::clamp(.5f + lit * contrast, 0.f, 1.f);
    for (int c = 0; c < 3; ++c) d[c] = grey + (self[c] - grey) * blend;
  });
  return 0;
}
const char *embossShader =
    "vec4 emAt(int x,int y){return wavyPixel(clamp(ivec2(x,y),ivec2(0),ivec2(sourceSize)-1));}"
    "vec3 emRead(float fx,float fy){int x0=int(floor(fx)),y0=int(floor(fy));float tx=fx-float(x0),ty=fy-float(y0);"
    "return ((emAt(x0,y0)*(1.-tx)+emAt(x0+1,y0)*tx)*(1.-ty)+(emAt(x0,y0+1)*(1.-tx)+emAt(x0+1,y0+1)*tx)*ty).rgb;}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "if(v.a<=0.)return v;float angle=mod(values[0],360.)*3.14159265358979/180.;"
    "float relief=clamp(values[1],.5,20.)*(wavyScale>0.?wavyScale:1.);"
    "float contrast=clamp(values[2]/100.,0.,20.),blend=clamp(values[3]/100.,0.,1.);"
    "float ux=cos(angle)*relief,uy=-sin(angle)*relief;float x=floor(p.x),y=floor(p.y);"
    "vec3 d=emRead(x+ux,y+uy)-emRead(x-ux,y-uy);float lit=.2126*d.r+.7152*d.g+.0722*d.b;"
    "float grey=clamp(.5+lit*contrast,0.,1.);return vec4(vec3(grey)+(v.rgb-vec3(grey))*blend,v.a);}";
const WfxParameter embossParameters[] = {
    number("direction", "Direction", -3600, 3600, 135, 1, "\xc2\xb0"), number("relief", "Relief", .5f, 20, 2, .1f, " px"),
    number("contrast", "Contrast", 0, 2000, 100, 1, " %"), number("blend", "Blend with Original", 0, 100, 0, 1, " %")};
const char *const embossHelp[] = {"Where the light comes from (0 is from the right, 90 from above)",
                                  "How deep the pressing is, in layer pixels", "How strongly the raised parts stand out",
                                  "How much of the colour shows through"};

#define COUNT(parameters) int32_t(sizeof(parameters) / sizeof(parameters[0])), parameters
const WfxEffect effects[] = {
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Tint", "Tint", "Stylize", 4u, 0u, COUNT(tintParameters), tintProcess,
     tintInert, nullptr, nullptr, tintPrepare, tintSpan, tintRelease, tintShader},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "RGB Split", "RGB Split", "Stylize", 4u, uint32_t(WFX_PREMULTIPLIED), COUNT(splitParameters),
     splitProcess, splitInert, nullptr, splitReach, nullptr, nullptr, nullptr, splitShader},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Film Grain", "Film Grain", "Stylize", 4u,
     uint32_t(WFX_PER_PIXEL) | uint32_t(WFX_MOVES_ALONE), COUNT(grainParameters),
     grainProcess, grainInert, grainVaries, nullptr, nullptr, nullptr, nullptr, grainShader,
     nullptr, nullptr, nullptr, grainHelp, grainControlState},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Vignette", "Vignette", "Stylize", 4u, uint32_t(WFX_PER_PIXEL), COUNT(vignetteParameters),
     vignetteProcess, vignetteInert, nullptr, nullptr, nullptr, nullptr, nullptr, vignetteShader,
     nullptr, nullptr, nullptr, vignetteHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Gradient Map", "Gradient Map", "Color Correction", 4u, uint32_t(WFX_PER_PIXEL),
     COUNT(mapParameters), mapProcess, mapInert, nullptr, nullptr, mapPrepare, mapSpan, mapRelease, mapShader,
     nullptr, nullptr, nullptr, mapHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Posterize", "Posterize", "Stylize", 4u, uint32_t(WFX_PER_PIXEL),
     COUNT(posterizeParameters), posterizeProcess, posterizeInert, nullptr, nullptr, posterizePrepare, posterizeSpan,
     posterizeRelease, posterizeShader, nullptr, nullptr, nullptr, posterizeHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Halftone", "Halftone", "Stylize", 4u, uint32_t(WFX_PER_PIXEL),
     COUNT(halftoneParameters), halftoneProcess, halftoneInert, nullptr, nullptr, nullptr, nullptr, nullptr, halftoneShader,
     nullptr, nullptr, nullptr, halftoneHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Scanlines", "Scanlines", "Stylize", 4u,
     uint32_t(WFX_PER_PIXEL) | uint32_t(WFX_MOVES_ALONE), COUNT(scanParameters), scanProcess, scanInert, scanVaries,
     nullptr, nullptr, nullptr, nullptr, scanShader, nullptr, nullptr, nullptr, scanHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Light Sweep", "Light Sweep", "Stylize", 4u,
     uint32_t(WFX_PER_PIXEL) | uint32_t(WFX_MOVES_ALONE), COUNT(sweepParameters), sweepProcess, sweepInert, sweepVaries,
     nullptr, nullptr, nullptr, nullptr, sweepShader, nullptr, nullptr, nullptr, sweepHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Threshold", "Threshold", "Color Correction", 4u, uint32_t(WFX_PER_PIXEL),
     COUNT(thresholdParameters), thresholdProcess, thresholdInert, nullptr, nullptr, thresholdPrepare, thresholdSpan,
     thresholdRelease, thresholdShader, nullptr, nullptr, nullptr, thresholdHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Find Edges", "Find Edges", "Stylize", 4u, 0u,
     COUNT(edgesParameters), edgesProcess, edgesInert, nullptr, nullptr, nullptr, nullptr, nullptr, edgesShader,
     nullptr, nullptr, nullptr, edgesHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Emboss", "Emboss", "Stylize", 4u, 0u,
     COUNT(embossParameters), embossProcess, embossInert, nullptr, nullptr, nullptr, nullptr, nullptr, embossShader,
     nullptr, nullptr, nullptr, embossHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Tile Repeat", "Tile Repeat", "Stylize", 4u,
     uint32_t(WFX_PREMULTIPLIED) | uint32_t(WFX_WHOLE_LAYER), COUNT(tileParameters),
     tileProcess, tileInert, nullptr, tileReach, nullptr, nullptr, nullptr, tileShader,
     nullptr, nullptr, nullptr, tileHelp, tileControlState}};
}  // namespace

WFX_EXPORT int32_t wfxEffectCount(void) { return int32_t(sizeof(effects) / sizeof(effects[0])); }
WFX_EXPORT const WfxEffect *wfxEffectAt(int32_t index) {
  return index >= 0 && index < wfxEffectCount() ? &effects[index] : nullptr;
}
