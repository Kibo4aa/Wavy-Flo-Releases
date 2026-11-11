// Wavy-Motion.wfx: effects for music videos and motion graphics -- Glitch,
// Shake, Mirror, Kaleidoscope, Pixelate and Speed Lines, Directional and Zoom
// Blur, Sharpen, Light Rays, and Posterize Time (whose stepping the host does).
#include "wavy_effect.h"
#include "pack_common.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
// A pixel is four floats, which is one SSE register: the blurs' inner loops
// work a pixel at a time in them where the processor has it (every x64 one).
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define WAVY_MOTION_SSE 1
#endif

using namespace wfxpack;

namespace {
constexpr double kPi = 3.14159265358979323846;
float *row(void *pixels, int32_t stride, int32_t y) {
  return reinterpret_cast<float *>(static_cast<unsigned char *>(pixels) + size_t(y) * size_t(stride));
}

// Bilinear at a point in pixel coordinates (a pixel's centre is at x + .5) of
// a picture `w` pixels wide with rows one after another; nothing outside it.
inline void bilinear(const float *px, int w, int h, double x, double y, float *out) {
  x -= .5; y -= .5;
  const double fx = std::floor(x), fy = std::floor(y);
  const int x0 = int(fx), y0 = int(fy);
  const float tx = float(x - fx), ty = float(y - fy);
  if (x0 >= 0 && y0 >= 0 && x0 + 1 < w && y0 + 1 < h) {
    const float *a = px + (size_t(y0) * size_t(w) + size_t(x0)) * 4, *c = a + size_t(w) * 4;
    const float wa = (1 - tx) * (1 - ty), wb = tx * (1 - ty), wc = (1 - tx) * ty, wd = tx * ty;
    for (int k = 0; k < 4; ++k) out[k] = a[k] * wa + a[4 + k] * wb + c[k] * wc + c[4 + k] * wd;
    return;
  }
  out[0] = out[1] = out[2] = out[3] = 0;
  if (x0 + 1 < 0 || y0 + 1 < 0 || x0 >= w || y0 >= h) return;
  auto add = [&](int px_, int py_, float weight) {
    if (px_ < 0 || py_ < 0 || px_ >= w || py_ >= h || weight <= 0) return;
    const float *p = px + (size_t(py_) * size_t(w) + size_t(px_)) * 4;
    for (int k = 0; k < 4; ++k) out[k] += p[k] * weight;
  };
  add(x0, y0, (1 - tx) * (1 - ty)); add(x0 + 1, y0, tx * (1 - ty));
  add(x0, y0 + 1, (1 - tx) * ty); add(x0 + 1, y0 + 1, tx * ty);
}
// The picture as it came in, for effects whose pixels come from elsewhere in it.
struct Source {
  Scratch buffer;
  const float *px;
  int w = 0, h = 0;
  Source(void *pixels, int32_t width, int32_t height, int32_t stride, const WfxContext *context)
      : buffer(size_t(width) * size_t(height) * 4), px(buffer.data()), w(width), h(height) {
    forRows(context, height, [&](int32_t begin, int32_t end) {
      for (int32_t y = begin; y < end; ++y)
        std::memcpy(buffer.data() + size_t(y) * size_t(w) * 4, row(pixels, stride, y), size_t(w) * 16);
    });
  }
  void sample(double x, double y, float *out) const { bilinear(px, w, h, x, y, out); }
};

// The layer's own box in the raster, or the whole raster when there is none.
struct Box { double x, y, w, h; };
Box boxOf(const WfxContext *context, int32_t width, int32_t height) {
  if (context->boxWidth > 0 && context->boxHeight > 0) return {context->boxX, context->boxY, context->boxWidth, context->boxHeight};
  return {0, 0, double(width), double(height)};
}
// A coordinate folded back into lo..hi, as if the picture were mirrored
// beyond its edges: no empty corners, no seams.
double reflect(double v, double lo, double hi) {
  const double span = hi - lo;
  if (span <= 0) return lo;
  const double period = span * 2;
  double m = std::fmod(v - lo, period);
  if (m < 0) m += period;
  if (m > span) m = period - m;
  return lo + std::clamp(m, .5, std::max(.5, span - .5));
}

uint32_t hash(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  uint32_t h = a * 2654435761u ^ b * 374761393u ^ c * 668265263u ^ d * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
float unit(uint32_t h) { return float(h & 0xffffff) / 16777216.f; } // 0..1

// The same for the card. The shaders take straight colour and give it back;
// these effects work on premultiplied colour, so they multiply it in as they
// read and divide it out as they finish. Angles that can grow large are taken
// round to one turn before a sine: the card's is only good near nought.
#define MOTION_GLSL \
  "uint moHash(uint a,uint b,uint c,uint d){uint h=a*2654435761u^b*374761393u^c*668265263u^d*2246822519u;" \
  "h=(h^(h>>13u))*1274126177u;return h^(h>>16u);}" \
  "float moUnit(uint h){return float(h&0xffffffu)/16777216.;}" \
  "int moWhole(float v){return int(v<0.?-floor(-v+.5):floor(v+.5));}" \
  "vec4 moBox(vec2 s){vec4 b=wavyBox;if(!(b.z>0.&&b.w>0.))b=vec4(0.,0.,s);return b;}" \
  "vec4 moTexel(int x,int y){vec4 t=wavyPixel(ivec2(x,y));return vec4(t.rgb*t.a,t.a);}" \
  "vec4 moSample(vec2 q){q-=.5;vec2 f=floor(q);ivec2 i=ivec2(f);vec2 t=q-f;" \
  "return moTexel(i.x,i.y)*((1.-t.x)*(1.-t.y))+moTexel(i.x+1,i.y)*(t.x*(1.-t.y))+" \
  "moTexel(i.x,i.y+1)*((1.-t.x)*t.y)+moTexel(i.x+1,i.y+1)*(t.x*t.y);}" \
  "vec4 moStraight(vec4 d){return d.a>0.?vec4(d.rgb/d.a,d.a):vec4(0.);}" \
  "vec4 moKept(vec4 v){return v.a>0.?v:vec4(0.);}"
// Smooth noise in -1..1 along time, different for every seed and channel.
double drift(double t, uint32_t seed, uint32_t channel) {
  auto one = [&](double at, uint32_t octave) {
    const double i = std::floor(at), f = at - i;
    const double s = f * f * (3 - 2 * f);
    const double a = unit(hash(seed, uint32_t(int64_t(i)), channel, octave)) * 2. - 1.;
    const double b = unit(hash(seed, uint32_t(int64_t(i) + 1), channel, octave)) * 2. - 1.;
    return a + (b - a) * s;
  };
  return std::clamp(.72 * one(t, 1) + .28 * one(t * 2.3 + 17.1, 2), -1., 1.);
}

// ---- Glitch ------------------------------------------------------------------
// The picture torn into strips that jump sideways, with the colours pulled
// apart where they do, re-dealt several times a second. Strips and jumps are
// counted on the layer's own box, so a frame always tears the same way.
float glitchAmount(const float *v, int32_t n) { return std::clamp(value(v, n, 0, 40), 0.f, 400.f); }
float glitchSplit(const float *v, int32_t n) { return std::clamp(value(v, n, 3, 8), 0.f, 100.f); }
int glitchInert(const float *v, int32_t n) {
  return (glitchAmount(v, n) < .01f && glitchSplit(v, n) < .01f) || std::clamp(value(v, n, 2, 35), 0.f, 100.f) <= 0;
}
int glitchVaries(const float *v, int32_t n) { return value(v, n, 4, 12) > 0; }
double glitchReach(const float *v, int32_t n, double) {
  const double r = double(glitchAmount(v, n)) + double(glitchSplit(v, n));
  return r > 0 ? std::ceil(r / 16.) * 16. : 0;
}
int glitchProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32 || width <= 0 || height <= 0) return 1;
  if (glitchInert(v, n)) return 0;
  const double scale = context->scale > 0 ? context->scale : 1.;
  const double amount = glitchAmount(v, n) * scale, split = glitchSplit(v, n) * scale;
  const int slices = std::clamp(int(std::lround(value(v, n, 1, 24))), 1, 200);
  const float density = std::clamp(value(v, n, 2, 35), 0.f, 100.f) / 100.f;
  const double speed = std::clamp(double(value(v, n, 4, 12)), 0., 60.);
  const uint32_t seed = uint32_t(int(value(v, n, 5, 1)));
  const uint32_t step = speed > 0 ? uint32_t(int64_t(std::floor(context->seconds * speed + 1e-6))) : 0u;
  const Box box = boxOf(context, width, height);
  const double sliceH = std::max(1., box.h / slices);
  const double offset = unit(hash(seed, step, 7u, 0u));
  forRows(context, height, [&](int32_t begin, int32_t end) {
    std::vector<float> own(size_t(width) * 4);
    for (int32_t y = begin; y < end; ++y) {
      const int slice = int(std::floor((y + .5 - box.y) / sliceH + offset));
      if (unit(hash(seed, step, uint32_t(slice), 1u)) >= density) continue;
      const double shift = (unit(hash(seed, step, uint32_t(slice), 2u)) * 2. - 1.) * amount;
      // Some strips tear the colour apart, some only move.
      const double fringe = unit(hash(seed, step, uint32_t(slice), 3u)) < .6f ? split : 0.;
      float *p = row(pixels, stride, y);
      std::memcpy(own.data(), p, size_t(width) * 16);
      auto sample = [&](double x, int channel) {
        x -= .5;
        const int x0 = int(std::floor(x));
        const float t = float(x - x0);
        const float a = x0 >= 0 && x0 < width ? own[size_t(x0) * 4 + size_t(channel)] : 0.f;
        const float b = x0 + 1 >= 0 && x0 + 1 < width ? own[size_t(x0 + 1) * 4 + size_t(channel)] : 0.f;
        return a * (1 - t) + b * t;
      };
      for (int32_t x = 0; x < width; ++x) {
        const double from = x + .5 - shift;
        const float ra = sample(from - fringe, 3), ga = sample(from, 3), ba = sample(from + fringe, 3);
        float *d = p + size_t(x) * 4;
        d[0] = sample(from - fringe, 0);
        d[1] = sample(from, 1);
        d[2] = sample(from + fringe, 2);
        d[3] = std::max({ra, ga, ba});
      }
    }
  });
  return 0;
}
const char *glitchShader = MOTION_GLSL
    "vec4 moRow(float x,int y){x-=.5;int x0=int(floor(x));float t=x-float(x0);"
    "return moTexel(x0,y)*(1.-t)+moTexel(x0+1,y)*t;}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float amountV=clamp(values[0],0.,400.),splitV=clamp(values[3],0.,100.),densityV=clamp(values[2],0.,100.);"
    "if((amountV<.01&&splitV<.01)||densityV<=0.)return v;"
    "float scale=wavyScale>0.?wavyScale:1.;float amount=amountV*scale,split=splitV*scale;"
    "int slices=clamp(moWhole(values[1]),1,200);float density=densityV/100.;"
    "float speed=clamp(values[4],0.,60.);uint seed=uint(int(values[5]));uint step=0u;"
    "if(speed>0.){float x=wavySeconds*speed;step=uint(int(floor(x+max(1e-6,abs(x)*4e-7))));}"
    "vec4 box=moBox(s);float sliceH=max(1.,box.w/float(slices));"
    "float offset=moUnit(moHash(seed,step,7u,0u));int slice=int(floor((p.y-box.y)/sliceH+offset));"
    "if(moUnit(moHash(seed,step,uint(slice),1u))>=density)return moKept(v);"
    "float shift=(moUnit(moHash(seed,step,uint(slice),2u))*2.-1.)*amount;"
    "float fringe=moUnit(moHash(seed,step,uint(slice),3u))<.6?split:0.;"
    "int y=int(p.y);float from=p.x-shift;"
    "vec4 r=moRow(from-fringe,y),g=moRow(from,y),b=moRow(from+fringe,y);"
    "return moStraight(vec4(r.r,g.g,b.b,max(r.a,max(g.a,b.a))));}";
const WfxParameter glitchParameters[] = {
    number("amount", "Amount", 0, 400, 40, 1, " px"), number("slices", "Strips", 1, 200, 24, 1),
    number("density", "Density", 0, 100, 35, 1, " %"), number("split", "Colour Split", 0, 100, 8, .5f, " px"),
    number("speed", "Speed", 0, 60, 12, .5f, " /s"), number("seed", "Seed", 0, 9999, 1, 1)};
const char *const glitchHelp[] = {
    "How far a torn strip can jump sideways, in pixels.",
    "How many strips the layer is cut into, top to bottom.",
    "How many of the strips tear at once.",
    "How far the red and blue are pulled apart on a torn strip.",
    "How many times a second the tearing is dealt again. Zero keeps one tear still.",
    "Another number, another pattern of tears."};

// ---- Shake -------------------------------------------------------------------
// The picture jolted around, as if the camera were handheld or hit by a kick
// drum: a smooth wander in position and angle, with a zoom to keep the edges
// out of the frame, or edges mirrored so none show.
struct ShakeValues { double amount, rotation, speed, zoom; uint32_t seed; bool mirror; };
ShakeValues shakeValues(const float *v, int32_t n) {
  return {std::clamp(double(value(v, n, 0, 20)), 0., 1000.), std::clamp(double(value(v, n, 1, 2)), 0., 180.),
          std::clamp(double(value(v, n, 2, 8)), 0., 60.), std::clamp(double(value(v, n, 3, 100)), 10., 400.) / 100.,
          uint32_t(int(value(v, n, 4, 1))), value(v, n, 5, 0) >= .5f};
}
int shakeInert(const float *v, int32_t n) {
  const ShakeValues s = shakeValues(v, n);
  return s.amount < .01 && s.rotation < .001 && std::abs(s.zoom - 1) < 1e-6;
}
int shakeVaries(const float *v, int32_t n) {
  const ShakeValues s = shakeValues(v, n);
  return s.speed > 0 && (s.amount >= .01 || s.rotation >= .001);
}
double shakeReach(const float *v, int32_t n, double) {
  const ShakeValues s = shakeValues(v, n);
  return s.mirror || s.amount <= 0 ? 0 : std::ceil(s.amount / 16.) * 16.;
}
int shakeProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32 || width <= 0 || height <= 0) return 1;
  if (shakeInert(v, n)) return 0;
  const ShakeValues s = shakeValues(v, n);
  const double scale = context->scale > 0 ? context->scale : 1.;
  const double t = context->seconds * s.speed;
  const double ox = drift(t, s.seed, 0) * s.amount * scale, oy = drift(t, s.seed, 1) * s.amount * scale;
  const double angle = drift(t, s.seed, 2) * s.rotation * kPi / 180.;
  const Box box = boxOf(context, width, height);
  const double cx = box.x + box.w / 2, cy = box.y + box.h / 2;
  const double cs = std::cos(-angle) / s.zoom, sn = std::sin(-angle) / s.zoom;
  const Source source(pixels, width, height, stride, context);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const double dy = y + .5 - cy - oy;
      for (int32_t x = 0; x < width; ++x) {
        const double dx = x + .5 - cx - ox;
        double sx = cx + dx * cs - dy * sn, sy = cy + dx * sn + dy * cs;
        if (s.mirror) { sx = reflect(sx, box.x, box.x + box.w); sy = reflect(sy, box.y, box.y + box.h); }
        source.sample(sx, sy, p + size_t(x) * 4);
      }
    }
  });
  return 0;
}
const WfxParameter shakeParameters[] = {
    number("amount", "Amount", 0, 1000, 20, 1, " px"), number("rotation", "Rotation", 0, 180, 2, .1f, "\xc2\xb0"),
    number("speed", "Speed", 0, 60, 8, .1f, " /s"), number("zoom", "Zoom", 10, 400, 100, 1, " %"),
    number("seed", "Seed", 0, 9999, 1, 1), toggle("mirror", "Mirror Edges", false)};
const char *const shakeHelp[] = {
    "How far the picture wanders from its place, in pixels.",
    "How far it tips either way, in degrees.",
    "How many times a second it heads somewhere new. Key it up on the drops for harder hits.",
    "Enlarges the picture so its edges stay out of the frame while it shakes.",
    "Another number, another shake.",
    "Fills what the shake uncovers with the picture mirrored, instead of leaving it empty."};
// On the card: the same wander from the same hashes, worked out at every pixel.
const char *shakeShader = MOTION_GLSL
    "float shOne(float at,uint seed,uint channel,uint octave){float i=floor(at),f=at-i;float s=f*f*(3.-2.*f);"
    "float a=moUnit(moHash(seed,uint(int(i)),channel,octave))*2.-1.,b=moUnit(moHash(seed,uint(int(i)+1),channel,octave))*2.-1.;"
    "return a+(b-a)*s;}"
    "float shDrift(float t,uint seed,uint channel){"
    "return clamp(.72*shOne(t,seed,channel,1u)+.28*shOne(t*2.3+17.1,seed,channel,2u),-1.,1.);}"
    "float shReflect(float v,float lo,float hi){float span=hi-lo;if(span<=0.)return lo;float period=span*2.;"
    "float m=mod(v-lo,period);if(m>span)m=period-m;return lo+clamp(m,.5,max(.5,span-.5));}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float amount=clamp(values[0],0.,1000.),rotation=clamp(values[1],0.,180.),speed=clamp(values[2],0.,60.);"
    "float zoom=clamp(values[3],10.,400.)/100.;uint seed=uint(int(values[4]));"
    "if(amount<.01&&rotation<.001&&abs(zoom-1.)<1e-6)return moKept(v);"
    "float scale=wavyScale>0.?wavyScale:1.;float t=wavySeconds*speed;"
    "float ox=shDrift(t,seed,0u)*amount*scale,oy=shDrift(t,seed,1u)*amount*scale;"
    "float angle=shDrift(t,seed,2u)*rotation*3.14159265358979/180.;"
    "vec4 box=moBox(s);float cx=box.x+box.z/2.,cy=box.y+box.w/2.;"
    "float cs=cos(-angle)/zoom,sn=sin(-angle)/zoom;float dx=p.x-cx-ox,dy=p.y-cy-oy;"
    "float sx=cx+dx*cs-dy*sn,sy=cy+dx*sn+dy*cs;"
    "if(values[5]>=.5){sx=shReflect(sx,box.x,box.x+box.z);sy=shReflect(sy,box.y,box.y+box.w);}"
    "return moStraight(moSample(vec2(sx,sy)));}";

// ---- Mirror ------------------------------------------------------------------
// One half of the layer folded over the other, at a line that can be moved.
int mirrorProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32 || width <= 0 || height <= 0) return 1;
  const int mode = std::clamp(int(std::lround(value(v, n, 0, 0))), 0, 4);
  const Box box = boxOf(context, width, height);
  const double fx = box.x + box.w * std::clamp(double(value(v, n, 1, 50)), 0., 100.) / 100.;
  const double fy = box.y + box.h * std::clamp(double(value(v, n, 2, 50)), 0., 100.) / 100.;
  const bool leftToRight = mode == 0 || mode == 4, rightToLeft = mode == 1;
  const bool topToBottom = mode == 2 || mode == 4, bottomToTop = mode == 3;
  const Source source(pixels, width, height, stride, context);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      double sy = y + .5;
      if ((topToBottom && sy > fy) || (bottomToTop && sy < fy)) sy = 2 * fy - sy;
      for (int32_t x = 0; x < width; ++x) {
        double sx = x + .5;
        if ((leftToRight && sx > fx) || (rightToLeft && sx < fx)) sx = 2 * fx - sx;
        if (sx == x + .5 && sy == y + .5) continue;
        source.sample(sx, sy, p + size_t(x) * 4);
      }
    }
  });
  return 0;
}
const WfxParameter mirrorParameters[] = {
    choice("mode", "Fold", "Left onto Right\nRight onto Left\nTop onto Bottom\nBottom onto Top\nFour Ways", 5, 0),
    number("x", "Line X", 0, 100, 50, .1f, " %"), number("y", "Line Y", 0, 100, 50, .1f, " %")};
const char *const mirrorHelp[] = {
    "Which half is folded onto which. Four Ways folds the top left quarter onto the other three.",
    "Where the up-and-down fold line sits, across the layer.",
    "Where the side-to-side fold line sits, down the layer."};
uint32_t mirrorControlState(const float *v, int32_t n, int32_t parameter) {
  const int mode = std::clamp(int(std::lround(value(v, n, 0, 0))), 0, 4);
  if (parameter == 1 && (mode == 2 || mode == 3)) return uint32_t(WFX_CONTROL_DIMMED);
  if (parameter == 2 && (mode == 0 || mode == 1)) return uint32_t(WFX_CONTROL_DIMMED);
  return 0u;
}
const char *mirrorShader = MOTION_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "int mode=clamp(moWhole(values[0]),0,4);vec4 box=moBox(s);"
    "float fx=box.x+box.z*clamp(values[1],0.,100.)/100.,fy=box.y+box.w*clamp(values[2],0.,100.)/100.;"
    "float sx=p.x,sy=p.y;"
    "if(((mode==2||mode==4)&&sy>fy)||(mode==3&&sy<fy))sy=2.*fy-sy;"
    "if(((mode==0||mode==4)&&sx>fx)||(mode==1&&sx<fx))sx=2.*fx-sx;"
    "if(sx==p.x&&sy==p.y)return moKept(v);"
    "return moStraight(moSample(vec2(sx,sy)));}";

// ---- Kaleidoscope --------------------------------------------------------------
// The layer cut into wedges around a centre, each the mirror of its neighbour.
// Turning it makes the pattern bloom; the picture beyond the layer's edge is
// its mirror, so no wedge runs empty.
int kaleidoProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32 || width <= 0 || height <= 0) return 1;
  const int segments = std::clamp(int(std::lround(value(v, n, 0, 6))), 2, 64);
  const double turn = double(value(v, n, 1, 0)) * kPi / 180.;
  const Box box = boxOf(context, width, height);
  const double cx = box.x + box.w * std::clamp(double(value(v, n, 2, 50)), -100., 200.) / 100.;
  const double cy = box.y + box.h * std::clamp(double(value(v, n, 3, 50)), -100., 200.) / 100.;
  const double zoom = std::clamp(double(value(v, n, 4, 100)), 10., 1000.) / 100.;
  const double wedge = 2 * kPi / segments;
  const Source source(pixels, width, height, stride, context);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const double dy = y + .5 - cy;
      for (int32_t x = 0; x < width; ++x) {
        const double dx = x + .5 - cx;
        const double r = std::sqrt(dx * dx + dy * dy) / zoom;
        double a = std::atan2(dy, dx) - turn;
        double m = std::fmod(a, wedge);
        if (m < 0) m += wedge;
        if (m > wedge / 2) m = wedge - m;
        a = m + turn;
        const double sx = reflect(cx + r * std::cos(a), box.x, box.x + box.w);
        const double sy = reflect(cy + r * std::sin(a), box.y, box.y + box.h);
        source.sample(sx, sy, p + size_t(x) * 4);
      }
    }
  });
  return 0;
}
const char *kaleidoShader = MOTION_GLSL
    "float kaReflect(float v,float lo,float hi){float span=hi-lo;if(span<=0.)return lo;float period=span*2.;"
    "float m=mod(v-lo,period);if(m>span)m=period-m;return lo+clamp(m,.5,max(.5,span-.5));}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "const float pi=3.14159265358979;int segments=clamp(moWhole(values[0]),2,64);"
    "float turn=mod(values[1],360.)*pi/180.;vec4 box=moBox(s);"
    "float cx=box.x+box.z*clamp(values[2],-100.,200.)/100.,cy=box.y+box.w*clamp(values[3],-100.,200.)/100.;"
    "float zoom=clamp(values[4],10.,1000.)/100.;float wedge=2.*pi/float(segments);"
    "vec2 d=p-vec2(cx,cy);float r=sqrt(d.x*d.x+d.y*d.y)/zoom;"
    "float a=(r>0.?atan(d.y,d.x):0.)-turn;float m=mod(a,wedge);if(m>wedge/2.)m=wedge-m;a=m+turn;"
    "float sx=kaReflect(cx+r*cos(a),box.x,box.x+box.z),sy=kaReflect(cy+r*sin(a),box.y,box.y+box.w);"
    "return moStraight(moSample(vec2(sx,sy)));}";
const WfxParameter kaleidoParameters[] = {
    number("segments", "Segments", 2, 64, 6, 1), number("angle", "Angle", -36000, 36000, 0, .5f, "\xc2\xb0"),
    number("x", "Centre X", -100, 200, 50, .1f, " %"), number("y", "Centre Y", -100, 200, 50, .1f, " %"),
    number("zoom", "Zoom", 10, 1000, 100, 1, " %")};
const char *const kaleidoHelp[] = {
    "How many mirrored wedges go round the centre.",
    "Turns the wedges over the picture. Key it to make the pattern bloom.",
    "Where the centre is, across the layer.",
    "Where the centre is, down the layer.",
    "Enlarges the pattern around its centre."};

// ---- Pixelate ------------------------------------------------------------------
// The picture as square blocks, each the average of what it covers. The blocks
// grow from the layer's centre, so keying the size keeps the middle still.
int pixelateInert(const float *v, int32_t n) { return value(v, n, 0, 16) <= 1; }
int pixelateProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32 || width <= 0 || height <= 0) return 1;
  if (pixelateInert(v, n)) return 0;
  const double scale = context->scale > 0 ? context->scale : 1.;
  const double size = std::max(1., std::clamp(double(value(v, n, 0, 16)), 1., 1000.) * scale);
  const Box box = boxOf(context, width, height);
  const double cx = box.x + box.w / 2, cy = box.y + box.h / 2;
  auto cellOf = [&](double at, double centre) { return int(std::floor((at - centre) / size + .5)); };
  const int firstX = cellOf(.5, cx), lastX = cellOf(width - .5, cx);
  const int firstY = cellOf(.5, cy), lastY = cellOf(height - .5, cy);
  const int columns = lastX - firstX + 1, rows = lastY - firstY + 1;
  std::vector<int> cellX(static_cast<size_t>(width)), cellY(static_cast<size_t>(height));
  for (int x = 0; x < width; ++x) cellX[size_t(x)] = cellOf(x + .5, cx) - firstX;
  for (int y = 0; y < height; ++y) cellY[size_t(y)] = cellOf(y + .5, cy) - firstY;
  // The image rows each row of blocks covers.
  std::vector<int> rowStart(size_t(rows) + 1, height);
  for (int y = height - 1; y >= 0; --y) rowStart[size_t(cellY[size_t(y)])] = y;
  for (int r = rows - 1; r >= 0; --r) rowStart[size_t(r)] = std::min(rowStart[size_t(r)], rowStart[size_t(r) + 1]);
  // Every image row's sums under each block, on all the cores (there are too
  // few rows of blocks to share out); then every row adds up the rows of its
  // own row of blocks and writes the averages.
  const size_t line = size_t(columns) * 5;
  std::vector<float> partial(size_t(height) * line);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *s = &partial[size_t(y) * line];
      const float *p = row(pixels, stride, y);
      for (int x = 0; x < width; ++x) {
        float *c = s + size_t(cellX[size_t(x)]) * 5;
        for (int k = 0; k < 4; ++k) c[k] += p[x * 4 + k];
        c[4] += 1;
      }
    }
  });
  forRows(context, height, [&](int32_t begin, int32_t end) {
    std::vector<double> sum(line);
    int made = -1;
    for (int32_t y = begin; y < end; ++y) {
      const int r = cellY[size_t(y)];
      if (r != made) {
        std::fill(sum.begin(), sum.end(), 0.);
        for (int yy = rowStart[size_t(r)]; yy < rowStart[size_t(r) + 1]; ++yy) {
          const float *s = &partial[size_t(yy) * line];
          for (size_t i = 0; i < line; ++i) sum[i] += s[i];
        }
        for (int c = 0; c < columns; ++c) {
          double *s = &sum[size_t(c) * 5];
          if (s[4] > 0) for (int k = 0; k < 4; ++k) s[k] /= s[4];
        }
        made = r;
      }
      float *p = row(pixels, stride, y);
      for (int x = 0; x < width; ++x) {
        const double *s = &sum[size_t(cellX[size_t(x)]) * 5];
        for (int k = 0; k < 4; ++k) p[x * 4 + k] = float(s[k]);
      }
    }
  });
  return 0;
}
const WfxParameter pixelateParameters[] = {number("size", "Block Size", 1, 1000, 16, 1, " px")};
const char *const pixelateHelp[] = {"How big each block is, in pixels. 1 leaves the picture as it is."};
// On the card in two rounds: each pixel the average of its block's share of
// its row, then the average of those down the block. A block's edges are
// found by multiplying out from the centre rather than dividing, so a pixel
// on an edge lands in the block the CPU puts it in. Each pixel reads its whole
// block across and down, so past a size the CPU's sums are left to do it.
int32_t pixelateShaderPasses(const float *v, int32_t n, const WfxContext *context) {
  if (pixelateInert(v, n) || !context) return 1;
  const double scale = context->scale > 0 ? context->scale : 1.;
  return std::max(1., std::clamp(double(value(v, n, 0, 16)), 1., 1000.) * scale) > 256 ? 0 : 2;
}
const char *pixelateShader = MOTION_GLSL
    "float pxCell(float at,float c,float size){float k=floor((at-c)/size+.5);"
    "if(at<c+(k-.5)*size)k-=1.;else if(at>=c+(k+.5)*size)k+=1.;return k;}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "if(values[0]<=1.)return moKept(v);float scale=wavyScale>0.?wavyScale:1.;"
    "float size=max(1.,clamp(values[0],1.,1000.)*scale);vec4 box=moBox(s);bool across=wavyPass==0;"
    "float c=across?box.x+box.z/2.:box.y+box.w/2.,at=across?p.x:p.y;int limit=across?int(s.x):int(s.y);"
    "float k=pxCell(at,c,size);"
    "int first=max(0,int(ceil(c+(k-.5)*size-.5))-1),last=min(limit-1,int(ceil(c+(k+.5)*size-.5)));"
    "vec4 sum=vec4(0.);float count=0.;"
    "for(int i=first;i<=last;++i){if(pxCell(float(i)+.5,c,size)!=k)continue;"
    "sum+=across?moTexel(i,int(p.y)):moTexel(int(p.x),i);count+=1.;}"
    "return moStraight(sum/max(count,1.));}";

// ---- Directional Blur ------------------------------------------------------------
// Every pixel smeared along one direction, the way a fast pan streaks.
//
// Both blurs here are a few passes of four samples, each pass four times finer
// than the one before: three passes average 64 evenly spaced samples for the
// price of twelve, so a long smear costs little more than a short one.
constexpr int kCombTaps = 4;
// How many passes keep the samples no more than a pixel and a half apart over
// `span` pixels.
int combPasses(double span) {
  int passes = 1;
  for (double samples = kCombTaps; samples * 1.5 < span && passes < 6; samples *= kCombTaps) ++passes;
  return passes;
}
// Runs the passes, each reading the whole picture the one before left and
// writing a row at a time: blurRow(pass, picture, y, out). They go back and
// forth between two buffers, and the last writes straight into the layer.
template <class BlurRow>
void runPasses(void *pixels, int32_t width, int32_t height, int32_t stride, const WfxContext *context, int passes,
               const BlurRow &blurRow) {
  const size_t count = size_t(width) * size_t(height) * 4;
  Scratch first(count), second(passes > 1 ? count : 0);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y)
      std::memcpy(first.data() + size_t(y) * size_t(width) * 4, row(pixels, stride, y), size_t(width) * 16);
  });
  const float *from = first.data();
  float *spare = second.data();
  for (int pass = 0; pass < passes; ++pass) {
    const bool last = pass + 1 == passes;
    forRows(context, height, [&](int32_t begin, int32_t end) {
      for (int32_t y = begin; y < end; ++y)
        blurRow(pass, from, y, last ? row(pixels, stride, y) : spare + size_t(y) * size_t(width) * 4);
    });
    if (!last) {
      from = spare;
      spare = spare == second.data() ? first.data() : second.data();
    }
  }
}
// A long smear, done in time that does not grow with its length. The picture
// is sheared so the smear's direction runs along its rows (or columns), each
// line is averaged over the smear's length with a running sum -- one add and
// one take-away a pixel however long the box -- and the result is sheared
// back. Only the part of each line over the picture is worked on, and every
// pass reads memory in order. The two shears each blend neighbouring rows,
// which softens across the smear by under a pixel.
//
// Pixels of four floats, `count` of them at `in`, with at least `pad` pixels
// of nothing before and after (pad >= half + 2). Each becomes the average over
// a box `half` wide either side of its centre, written to `out`, `outStep`
// floats apart.
void slideBox(const float *in, int count, double half, float *out, size_t outStep) {
  // Pixel k covers [k, k+1); the box of pixel x covers [x + .5 - half,
  // x + .5 + half). As x steps on by one the box does too, so its whole pixels
  // are a running sum, and its two part-covered ends take the same shares
  // every time.
  const double low0 = .5 - half, high0 = .5 + half;
  const int firstWhole = int(std::floor(low0)), lastPart = int(std::floor(high0));
  const float lowShare = float(1 - (low0 - firstWhole)), highShare = float(high0 - lastPart);
  const float across = float(1. / (2 * half));
#ifdef WAVY_MOTION_SSE
  __m128 inner = _mm_setzero_ps();
  for (int k = firstWhole + 1; k < lastPart; ++k) inner = _mm_add_ps(inner, _mm_loadu_ps(in + ptrdiff_t(k) * 4));
  const __m128 lowW = _mm_set1_ps(lowShare), highW = _mm_set1_ps(highShare), scale = _mm_set1_ps(across);
  const bool one = lastPart == firstWhole;
  for (int x = 0; x < count; ++x) {
    const float *lo = in + ptrdiff_t(firstWhole + x) * 4, *hi = in + ptrdiff_t(lastPart + x) * 4;
    const __m128 h = _mm_loadu_ps(hi);
    __m128 total = one ? _mm_mul_ps(h, _mm_set1_ps(float(2 * half)))
                       : _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_loadu_ps(lo), lowW), inner), _mm_mul_ps(h, highW));
    _mm_storeu_ps(out + size_t(x) * outStep, _mm_mul_ps(total, scale));
    if (lastPart > firstWhole + 1) inner = _mm_add_ps(inner, _mm_sub_ps(h, _mm_loadu_ps(lo + 4)));
  }
#else
  float inner[4] = {0, 0, 0, 0};
  for (int k = firstWhole + 1; k < lastPart; ++k)
    for (int c = 0; c < 4; ++c) inner[c] += in[ptrdiff_t(k) * 4 + c];
  for (int x = 0; x < count; ++x) {
    const float *lo = in + ptrdiff_t(firstWhole + x) * 4, *hi = in + ptrdiff_t(lastPart + x) * 4;
    float *o = out + size_t(x) * outStep;
    for (int c = 0; c < 4; ++c) {
      const float total = lastPart == firstWhole ? hi[c] * float(2 * half) : lo[c] * lowShare + inner[c] + hi[c] * highShare;
      o[c] = total * across;
      if (lastPart > firstWhole + 1) inner[c] += hi[c] - lo[4 + c];
    }
  }
#endif
}
// a + (b - a) * f, a pixel at a time.
inline void blendPixel(const float *a, const float *b, float f, float *out) {
#ifdef WAVY_MOTION_SSE
  const __m128 va = _mm_loadu_ps(a);
  _mm_storeu_ps(out, _mm_add_ps(va, _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(b), va), _mm_set1_ps(f))));
#else
  for (int c = 0; c < 4; ++c) out[c] = a[c] + (b[c] - a[c]) * f;
#endif
}
void shearSmear(void *pixels, int32_t width, int32_t height, int32_t stride, const WfxContext *context,
                double ux, double uy, double length) {
  static const float nothing[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  auto pixel = [&](int x, int y) -> const float * {
    return x >= 0 && y >= 0 && x < width && y < height ? row(pixels, stride, y) + size_t(x) * 4 : nothing;
  };
  // Where along a sheared line it is over the picture (from -1 to size, so
  // the blends at the edge are in), with two to spare: first and last index.
  auto span = [](double j, double offset, double slope, int size, int along) {
    double first = 0, last = along - 1;
    if (std::abs(slope) > 1e-12) {
      const double a = (-1 - j + offset) / slope, b = (size - j + offset) / slope;
      first = std::min(a, b); last = std::max(a, b);
    } else if (j - offset < -1 || j - offset > size) {
      return std::pair<int, int>(0, -1);
    }
    return std::pair<int, int>(std::max(0, int(std::floor(first)) - 2), std::min(along - 1, int(std::ceil(last)) + 2));
  };
  if (std::abs(ux) >= std::abs(uy)) {
    // Closer to side to side: lines of the sheared picture run along the smear.
    // Line j, column x is the picture at (x, j - offset + x * rise).
    const double rise = uy / ux, half = length / 2 * std::abs(ux);
    const double offset = rise > 0 ? std::ceil(rise * (width - 1)) + 1 : 1;
    const int lines = height + int(std::ceil(std::abs(rise) * (width - 1))) + 2;
    const size_t line = size_t(width) * 4;
    const int pad = int(std::ceil(half)) + 2;
    // A line's drop at column x is the same for every line: whole rows and the
    // part of one, worked out once. Going back, the drop is the other way.
    std::vector<int> drop(static_cast<size_t>(width)), back(static_cast<size_t>(width));
    std::vector<float> dropPart(static_cast<size_t>(width)), backPart(static_cast<size_t>(width));
    for (int x = 0; x < width; ++x) {
      const double d = x * rise, fd = std::floor(d), b = -x * rise, fb = std::floor(b);
      drop[size_t(x)] = int(fd); dropPart[size_t(x)] = float(d - fd);
      back[size_t(x)] = int(fb); backPart[size_t(x)] = float(b - fb);
    }
    const int shift = int(offset);
    Scratch sheared(size_t(lines) * line);
    float *const s = sheared.data();
    forRows(context, lines, [&](int32_t begin, int32_t end) {
      std::vector<float> run((size_t(width) + 2 * size_t(pad)) * 4);
      for (int32_t j = begin; j < end; ++j) {
        const auto [first, last] = span(j, offset, rise, height, width);
        if (last < first) continue;
        const int count = last - first + 1;
        float *values = run.data() + size_t(pad) * 4;
        std::fill(run.begin(), run.begin() + ptrdiff_t(pad) * 4, 0.f);
        std::fill(values + size_t(count) * 4, values + size_t(count + pad) * 4, 0.f);
        for (int x = first; x <= last; ++x) {
          const int y0 = j - shift + drop[size_t(x)];
          blendPixel(pixel(x, y0), pixel(x, y0 + 1), dropPart[size_t(x)], values + size_t(x - first) * 4);
        }
        slideBox(values, count, half, s + size_t(j) * line + size_t(first) * 4, 4);
      }
    });
    forRows(context, height, [&](int32_t begin, int32_t end) {
      for (int32_t y = begin; y < end; ++y) {
        float *out = row(pixels, stride, y);
        for (int x = 0; x < width; ++x) {
          const int j0 = y + shift + back[size_t(x)];
          blendPixel(s + size_t(j0) * line + size_t(x) * 4, s + size_t(j0 + 1) * line + size_t(x) * 4, backPart[size_t(x)],
                     out + size_t(x) * 4);
        }
      }
    });
    return;
  }
  // Closer to up and down: columns of the sheared picture run along the smear.
  // Row y, column i is the picture at (i - offset + y * lean, y).
  const double lean = ux / uy, half = length / 2 * std::abs(uy);
  const double offset = lean > 0 ? std::ceil(lean * (height - 1)) + 1 : 1;
  const int columns = width + int(std::ceil(std::abs(lean) * (height - 1))) + 2;
  const size_t wide = size_t(columns) * 4;
  const int pad = int(std::ceil(half)) + 2;
  Scratch sheared(size_t(height) * wide), smeared(size_t(height) * wide);
  float *const s = sheared.data();
  float *const m = smeared.data();
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *out = s + size_t(y) * wide;
      // Nothing where the row is off the picture: the columns read it.
      // Picture column i - offset + y * lean runs from -1 to width here.
      const int first = std::max(0, int(std::floor(-1 + offset - y * lean)) - 2);
      const int last = std::min(columns - 1, int(std::ceil(width + offset - y * lean)) + 2);
      std::fill(out, out + size_t(std::max(0, first)) * 4, 0.f);
      if (last >= first) std::fill(out + size_t(last + 1) * 4, out + wide, 0.f);
      else { std::fill(out, out + wide, 0.f); continue; }
      const float *source = row(pixels, stride, y);
      // The whole row moves by the same amount: a whole number of pixels and
      // the part of one.
      const double moved = y * lean - offset, whole = std::floor(moved);
      const int by = int(whole);
      const float part = float(moved - whole);
      for (int i = first; i <= last; ++i) {
        const int x0 = i + by;
        const float *a = x0 >= 0 && x0 < width ? source + size_t(x0) * 4 : nothing;
        const float *b = x0 + 1 >= 0 && x0 + 1 < width ? source + size_t(x0 + 1) * 4 : nothing;
        blendPixel(a, b, part, out + size_t(i) * 4);
      }
    }
  });
  forRows(context, columns, [&](int32_t begin, int32_t end) {
    std::vector<float> run((size_t(height) + 2 * size_t(pad)) * 4, 0.f);
    std::vector<float> done(size_t(height) * 4);
    for (int32_t i = begin; i < end; ++i) {
      float *values = run.data() + size_t(pad) * 4;
      for (int y = 0; y < height; ++y) std::memcpy(values + size_t(y) * 4, s + size_t(y) * wide + size_t(i) * 4, 16);
      slideBox(values, height, half, done.data(), 4);
      for (int y = 0; y < height; ++y) std::memcpy(m + size_t(y) * wide + size_t(i) * 4, done.data() + size_t(y) * 4, 16);
    }
  });
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *out = row(pixels, stride, y);
      const float *from = m + size_t(y) * wide;
      const double moved = offset - y * lean, whole = std::floor(moved);
      const int by = int(whole);
      const float part = float(moved - whole);
      for (int x = 0; x < width; ++x)
        blendPixel(from + size_t(x + by) * 4, from + size_t(x + by + 1) * 4, part, out + size_t(x) * 4);
    }
  });
}
constexpr double kShearFrom = 24;
double directionalLength(const float *v, int32_t n) { return std::clamp(double(value(v, n, 1, 20)), 0., 1000.); }
int directionalInert(const float *v, int32_t n) { return directionalLength(v, n) < .5; }
double directionalReach(const float *v, int32_t n, double) {
  const double length = directionalLength(v, n);
  return length > 0 ? std::ceil(length / 2. / 16.) * 16. : 0;
}
int directionalProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32 || width <= 0 || height <= 0) return 1;
  if (directionalInert(v, n)) return 0;
  const double scale = context->scale > 0 ? context->scale : 1.;
  const double length = directionalLength(v, n) * scale;
  if (length < .5) return 0;
  const double angle = double(value(v, n, 0, 0)) * kPi / 180.;
  const double ux = std::cos(angle), uy = std::sin(angle);
  // Past a few passes of samples the running sum is quicker.
  if (length > kShearFrom) {
    shearSmear(pixels, width, height, stride, context, ux, uy, length);
    return 0;
  }
  // N samples a length/N apart, centred on the pixel. Every pixel takes the
  // same four shifts in a pass, so their whole pixels and bilinear weights
  // are worked out once.
  struct Tap { int ox, oy; float w00, w10, w01, w11; double dx, dy; };
  const int passes = combPasses(length);
  std::vector<std::array<Tap, kCombTaps>> taps(static_cast<size_t>(passes));
  double step = length / std::pow(double(kCombTaps), passes);
  for (int pass = 0; pass < passes; ++pass, step *= kCombTaps)
    for (int i = 0; i < kCombTaps; ++i) {
      const double along = (i - (kCombTaps - 1) / 2.) * step, dx = ux * along, dy = uy * along;
      const double fx = std::floor(dx), fy = std::floor(dy);
      const float tx = float(dx - fx), ty = float(dy - fy);
      taps[size_t(pass)][size_t(i)] = {int(fx), int(fy), (1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty, dx, dy};
    }
  const size_t line = size_t(width) * 4;
  runPasses(pixels, width, height, stride, context, passes, [&](int pass, const float *from, int32_t y, float *out) {
    const auto &four = taps[size_t(pass)];
    // Where on this row all four taps land inside the picture.
    int left = 0, right = width;
    bool rowInside = true;
    for (const Tap &t : four) {
      rowInside = rowInside && y + t.oy >= 0 && y + t.oy + 1 < height;
      left = std::max(left, -t.ox);
      right = std::min(right, width - 1 - t.ox);
    }
    if (!rowInside) { left = 0; right = 0; }
    for (int32_t x = 0; x < width; ++x) {
      float sum[4] = {0, 0, 0, 0};
      if (x >= left && x < right) {
#ifdef WAVY_MOTION_SSE
        __m128 total = _mm_setzero_ps();
        for (const Tap &t : four) {
          const float *a = from + size_t(y + t.oy) * line + size_t(x + t.ox) * 4, *c = a + line;
          total = _mm_add_ps(total, _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_loadu_ps(a), _mm_set1_ps(t.w00)),
                                                          _mm_mul_ps(_mm_loadu_ps(a + 4), _mm_set1_ps(t.w10))),
                                               _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(c), _mm_set1_ps(t.w01)),
                                                          _mm_mul_ps(_mm_loadu_ps(c + 4), _mm_set1_ps(t.w11)))));
        }
        _mm_storeu_ps(out + size_t(x) * 4, _mm_mul_ps(total, _mm_set1_ps(1.f / kCombTaps)));
        continue;
#else
        for (const Tap &t : four) {
          const float *a = from + size_t(y + t.oy) * line + size_t(x + t.ox) * 4, *c = a + line;
          for (int k = 0; k < 4; ++k) sum[k] += a[k] * t.w00 + a[4 + k] * t.w10 + c[k] * t.w01 + c[4 + k] * t.w11;
        }
#endif
      } else {
        float tap[4];
        for (const Tap &t : four) {
          bilinear(from, width, height, x + .5 + t.dx, y + .5 + t.dy, tap);
          for (int k = 0; k < 4; ++k) sum[k] += tap[k];
        }
      }
      for (int k = 0; k < 4; ++k) out[x * 4 + k] = sum[k] * (1.f / kCombTaps);
    }
  });
  return 0;
}
const WfxParameter directionalParameters[] = {
    number("angle", "Direction", -360, 360, 0, 1, "\xc2\xb0"), number("length", "Length", 0, 1000, 20, 1, " px")};
const char *const directionalHelp[] = {"Which way the picture is smeared. 0 is side to side, 90 up and down.",
                                       "How long the smear is, in pixels."};
// On the card, the short smears: the same rounds of four taps. A long one is
// the running sum along the shear, a picture of the CPU's own, and is left to
// process().
int32_t directionalShaderPasses(const float *v, int32_t n, const WfxContext *context) {
  if (directionalInert(v, n) || !context) return 1;
  const double length = directionalLength(v, n) * (context->scale > 0 ? context->scale : 1.);
  if (length < .5) return 1;
  return length > kShearFrom ? 0 : combPasses(length);
}
const char *directionalShader = MOTION_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float scale=wavyScale>0.?wavyScale:1.;float len=clamp(values[1],0.,1000.)*scale;"
    "if(clamp(values[1],0.,1000.)<.5||len<.5)return moKept(v);"
    "float angle=mod(values[0],360.)*3.14159265358979/180.;vec2 u=vec2(cos(angle),sin(angle));"
    "float step=len/pow(4.,float(wavyPasses))*pow(4.,float(wavyPass));"
    "vec4 sum=vec4(0.);for(int i=0;i<4;++i)sum+=moSample(p+u*((float(i)-1.5)*step));"
    "return moStraight(sum*.25);}";

// ---- Zoom Blur -------------------------------------------------------------------
// Every pixel smeared toward a centre, as if the camera rushed in: a drop, a
// punch-in on the beat. The samples are spaced evenly in scale, from the
// picture as it is to the picture shrunk by Amount toward the centre.
//
// A shrunk picture is the picture scaled about the centre, so for one row of
// the result each sample comes from one row of the source (two, for the
// bilinear), stepped across it at a fixed pace: the row and its weights are
// worked out once per row, and the loop across it is a few multiplies, reading
// the source in order.
double zoomAmount(const float *v, int32_t n) { return std::clamp(double(value(v, n, 0, 20)), 0., 95.) / 100.; }
int zoomInert(const float *v, int32_t n) { return zoomAmount(v, n) <= 0; }
double zoomReachWithData(const float *v, int32_t n, double, const WfxContext *context) {
  // A streak runs outward from the centre by as much as the picture is
  // shrunk toward it. Asked at the start only, so a keyed amount gets a fair
  // share of room anyway.
  if (!context || !(context->boxWidth > 0 && context->boxHeight > 0)) return 0;
  const double a = std::max(zoomAmount(v, n), .35);
  const double reach = std::hypot(context->boxWidth, context->boxHeight) / 2 * a / (1 - a);
  return std::ceil(reach / 32.) * 32.;
}
// The smear itself, toward (cx, cy), by `amount` of the way: Zoom Blur's, and
// Light Rays' on the bright parts of a layer.
void zoomSmear(void *pixels, int32_t width, int32_t height, int32_t stride, const WfxContext *context, double cx, double cy,
               double amount) {
  double farthest = 0;
  for (double x : {0., double(width)})
    for (double y : {0., double(height)}) farthest = std::max(farthest, std::hypot(x - cx, y - cy));
  const int passes = combPasses(farthest * amount);
  const double total = -std::log(1 - amount);
  const double samples = std::pow(double(kCombTaps), passes);
  std::vector<std::array<double, kCombTaps>> shrink(static_cast<size_t>(passes));
  double step = total / (samples - 1);
  for (int pass = 0; pass < passes; ++pass, step *= kCombTaps)
    for (int i = 0; i < kCombTaps; ++i) shrink[size_t(pass)][size_t(i)] = std::exp(-i * step);
  const size_t line = size_t(width) * 4;
  // Rows above and below the picture read as nothing.
  const std::vector<float> nothing(line, 0.f);
  runPasses(pixels, width, height, stride, context, passes, [&](int pass, const float *from, int32_t y, float *out) {
    std::fill(out, out + line, 0.f);
    for (double f : shrink[size_t(pass)]) {
      // Source row: where this row lands shrunk toward the centre.
      const double sy = cy + (y + .5 - cy) * f - .5, fy = std::floor(sy);
      const int y0 = int(fy);
      const float ty = float(sy - fy), uy = 1 - ty;
      const bool have0 = y0 >= 0 && y0 < height, have1 = y0 + 1 >= 0 && y0 + 1 < height;
      if (!have0 && !have1) continue;
      const float *r0 = have0 ? from + size_t(y0) * line : nothing.data();
      const float *r1 = have1 ? from + size_t(y0 + 1) * line : nothing.data();
      // Source column of result column x: start + f * x.
      const double start = cx + (.5 - cx) * f - .5;
      // The columns whose two source pixels are both inside the picture.
      int left = std::clamp(int(std::ceil(-start / f)), 0, width);
      int right = std::clamp(int(std::ceil((width - 1 - start) / f)), left, width);
      while (left < right && start + f * left < 0) ++left;
      while (right > left && start + f * (right - 1) >= width - 1) --right;
      auto edge = [&](int x) {
        const double sx = start + f * x, fx = std::floor(sx);
        const int x0 = int(fx);
        const float tx = float(sx - fx);
        float *o = out + size_t(x) * 4;
        for (int side = 0; side < 2; ++side) {
          const int at = x0 + side;
          if (at < 0 || at >= width) continue;
          const float wx = side ? tx : 1 - tx;
          const float *a = r0 + size_t(at) * 4, *b = r1 + size_t(at) * 4;
          for (int c = 0; c < 4; ++c) o[c] += wx * (a[c] * uy + b[c] * ty);
        }
      };
      for (int x = 0; x < left; ++x) edge(x);
      const float fs = float(f);
      const double base = start + f * left;
      for (int x = left; x < right; ++x) {
        const float sx = float(base) + fs * float(x - left);
        const int x0 = std::min(int(sx), width - 2);
        const float tx = std::min(sx - float(x0), 1.f), ux = 1 - tx;
        const float *a = r0 + size_t(x0) * 4, *b = r1 + size_t(x0) * 4;
        float *o = out + size_t(x) * 4;
#ifdef WAVY_MOTION_SSE
        const __m128 u = _mm_set1_ps(ux), t = _mm_set1_ps(tx);
        const __m128 top = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(a), u), _mm_mul_ps(_mm_loadu_ps(a + 4), t));
        const __m128 bottom = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(b), u), _mm_mul_ps(_mm_loadu_ps(b + 4), t));
        const __m128 both = _mm_add_ps(_mm_mul_ps(top, _mm_set1_ps(uy)), _mm_mul_ps(bottom, _mm_set1_ps(ty)));
        _mm_storeu_ps(o, _mm_add_ps(_mm_loadu_ps(o), both));
#else
        for (int c = 0; c < 4; ++c)
          o[c] += (a[c] * ux + a[4 + c] * tx) * uy + (b[c] * ux + b[4 + c] * tx) * ty;
#endif
      }
      for (int x = right; x < width; ++x) edge(x);
    }
    for (size_t i = 0; i < line; ++i) out[i] *= 1.f / kCombTaps;
  });
}
int zoomProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32 || width <= 0 || height <= 0) return 1;
  const double amount = zoomAmount(v, n);
  if (amount <= 0) return 0;
  const Box box = boxOf(context, width, height);
  const double cx = box.x + box.w * std::clamp(double(value(v, n, 1, 50)), -100., 200.) / 100.;
  const double cy = box.y + box.h * std::clamp(double(value(v, n, 2, 50)), -100., 200.) / 100.;
  zoomSmear(pixels, width, height, stride, context, cx, cy, amount);
  return 0;
}
// On the card, the same rounds: each pass averages four shrunk copies of what
// the last one made, the shrinking four times as strong each round.
int32_t zoomShaderPasses(const float *v, int32_t n, const WfxContext *context) {
  const double amount = zoomAmount(v, n);
  if (amount <= 0 || !context) return 1;
  const double width = context->rasterWidth, height = context->rasterHeight;
  const Box box = boxOf(context, int32_t(width), int32_t(height));
  const double cx = box.x + box.w * std::clamp(double(value(v, n, 1, 50)), -100., 200.) / 100.;
  const double cy = box.y + box.h * std::clamp(double(value(v, n, 2, 50)), -100., 200.) / 100.;
  double farthest = 0;
  for (double x : {0., width})
    for (double y : {0., height}) farthest = std::max(farthest, std::hypot(x - cx, y - cy));
  return combPasses(farthest * amount);
}
const char *zoomShader = MOTION_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float amount=clamp(values[0],0.,95.)/100.;if(amount<=0.)return v;vec4 box=moBox(s);"
    "float cx=box.x+box.z*clamp(values[1],-100.,200.)/100.,cy=box.y+box.w*clamp(values[2],-100.,200.)/100.;"
    "float total=-log(1.-amount);float step=total/(pow(4.,float(wavyPasses))-1.)*pow(4.,float(wavyPass));"
    "vec4 sum=vec4(0.);for(int i=0;i<4;++i){float f=exp(-float(i)*step);"
    "sum+=moSample(vec2(cx,cy)+(p-vec2(cx,cy))*f);}"
    "return moStraight(sum*.25);}";
const WfxParameter zoomParameters[] = {
    number("amount", "Amount", 0, 95, 20, .5f, " %"), number("x", "Centre X", -100, 200, 50, .1f, " %"),
    number("y", "Centre Y", -100, 200, 50, .1f, " %")};
const char *const zoomHelp[] = {"How far every pixel is smeared toward the centre.",
                                "Where the centre is, across the layer.", "Where the centre is, down the layer."};

// ---- Light Rays ---------------------------------------------------------------------
// Light streaming out from a point through the bright parts of the layer -- the
// beams round a title in front of a sun, through a logo's letters. The parts
// brighter than Threshold, tinted, are smeared out from the light the way Zoom
// Blur smears (its own passes, so long rays cost little more than short ones)
// and added over the layer, past its edges too.
enum { RayIntensity, RayLength, RayCentreX, RayCentreY, RayThreshold, RayColour, RayOnly = RayColour + 4 };
double raysLength(const float *v, int32_t n) { return std::clamp(double(value(v, n, RayLength, 40)), 0., 95.) / 100.; }
int raysInert(const float *v, int32_t n) {
  return std::clamp(divided(v, n, RayIntensity, 100, 100.), 0.f, 4.f) <= 0 && value(v, n, RayOnly, 0) <= .5f;
}
double raysReachWithData(const float *v, int32_t n, double, const WfxContext *context) {
  if (!context || !(context->boxWidth > 0 && context->boxHeight > 0)) return 0;
  const double a = std::max(raysLength(v, n), .35);
  return std::ceil(std::hypot(context->boxWidth, context->boxHeight) / 2 * a / (1 - a) / 32.) * 32.;
}
int raysProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32 || width <= 0 || height <= 0) return 1;
  if (raysInert(v, n)) return 0;
  const float intensity = std::clamp(divided(v, n, RayIntensity, 100, 100.), 0.f, 4.f);
  const double length = raysLength(v, n);
  const float threshold = std::clamp(divided(v, n, RayThreshold, 50, 100.), 0.f, .99f);
  const float tint[3] = {std::clamp(value(v, n, RayColour, 1), 0.f, 1.f), std::clamp(value(v, n, RayColour + 1, .9f), 0.f, 1.f),
                         std::clamp(value(v, n, RayColour + 2, .7f), 0.f, 1.f)};
  const bool raysOnly = value(v, n, RayOnly, 0) > .5f;
  const Box box = boxOf(context, width, height);
  const double cx = box.x + box.w * std::clamp(double(value(v, n, RayCentreX, 50)), -100., 200.) / 100.;
  const double cy = box.y + box.h * std::clamp(double(value(v, n, RayCentreY, 30)), -100., 200.) / 100.;
  const size_t line = size_t(width) * 4;
  Scratch keptLayer(line * size_t(height));
  float *const layer = keptLayer.data();
  // The layer kept, and in its place the light: what is brighter than Threshold, tinted.
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      std::memcpy(layer + size_t(y) * line, p, line * sizeof(float));
      for (int32_t x = 0; x < width; ++x) {
        float *d = p + size_t(x) * 4;
        const float luma = .2126f * d[0] + .7152f * d[1] + .0722f * d[2];
        const float k = std::max(0.f, luma - threshold * d[3]) / (1.f - threshold);
        d[0] = k * tint[0]; d[1] = k * tint[1]; d[2] = k * tint[2]; d[3] = k;
      }
    }
  });
  if (length > 0) zoomSmear(pixels, width, height, stride, context, cx, cy, length);
  // Added over the layer: light, so it only brightens, and it shows where the
  // layer is clear as well (its own coverage grows to hold it).
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *d = row(pixels, stride, y);
      const float *own = layer + size_t(y) * line;
      for (int32_t x = 0; x < width; ++x) {
        float *p = d + size_t(x) * 4;
        const float *o = own + size_t(x) * 4;
        float rgb[3];
        for (int c = 0; c < 3; ++c) rgb[c] = std::min(1.f, (raysOnly ? 0.f : o[c]) + p[c] * intensity);
        const float alpha = std::min(1.f, std::max({raysOnly ? 0.f : o[3], rgb[0], rgb[1], rgb[2]}));
        p[0] = rgb[0]; p[1] = rgb[1]; p[2] = rgb[2]; p[3] = alpha;
      }
    }
  });
  return 0;
}
const WfxParameter raysParameters[] = {
    number("intensity", "Intensity", 0, 400, 100, 1, " %"), number("length", "Length", 0, 95, 40, .5f, " %"),
    number("centerX", "Center X", -100, 200, 50, .5f, " %"), number("centerY", "Center Y", -100, 200, 30, .5f, " %"),
    number("threshold", "Threshold", 0, 99, 50, 1, " %"), color("color", "Colour", 1, .9f, .7f, 1),
    toggle("raysOnly", "Rays Only", false)};
const char *const raysHelp[] = {
    "How bright the rays are", "How far the rays stream out from the light", "Where the light is, across the layer",
    "Where the light is, down the layer", "How bright a part must be to give off rays", "The colour of the light",
    "Show only the rays, not the layer they come from"};
// On the card: the light picked out, Zoom Blur's rounds of smearing, and last
// the light added over the layer as it came (wavyOriginal). Between rounds the
// light is kept premultiplied, as process() keeps it.
int32_t raysShaderPasses(const float *v, int32_t n, const WfxContext *context) {
  if (raysInert(v, n) || !context) return 1;
  const double length = raysLength(v, n);
  if (length <= 0) return 2;
  const double width = context->rasterWidth, height = context->rasterHeight;
  const Box box = boxOf(context, int32_t(width), int32_t(height));
  const double cx = box.x + box.w * std::clamp(double(value(v, n, RayCentreX, 50)), -100., 200.) / 100.;
  const double cy = box.y + box.h * std::clamp(double(value(v, n, RayCentreY, 30)), -100., 200.) / 100.;
  double farthest = 0;
  for (double x : {0., width})
    for (double y : {0., height}) farthest = std::max(farthest, std::hypot(x - cx, y - cy));
  return combPasses(farthest * length) + 2;
}
const char *raysShader = MOTION_GLSL
    "vec4 rySample(vec2 q){q-=.5;vec2 f=floor(q);ivec2 i=ivec2(f);vec2 t=q-f;"
    "return wavyPixel(i)*((1.-t.x)*(1.-t.y))+wavyPixel(i+ivec2(1,0))*(t.x*(1.-t.y))+"
    "wavyPixel(i+ivec2(0,1))*((1.-t.x)*t.y)+wavyPixel(i+ivec2(1,1))*(t.x*t.y);}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float intensity=clamp(values[0]/100.,0.,4.);bool only=values[9]>.5;if(intensity<=0.&&!only)return moKept(v);"
    "if(wavyPass==0){float threshold=clamp(values[4]/100.,0.,.99);vec3 tint=clamp(vec3(values[5],values[6],values[7]),0.,1.);"
    "vec4 d=vec4(v.rgb*v.a,v.a);float k=max(0.,.2126*d.r+.7152*d.g+.0722*d.b-threshold*d.a)/(1.-threshold);"
    "return vec4(k*tint,k);}"
    "if(wavyPass<wavyPasses-1){float amount=clamp(values[1],0.,95.)/100.;vec4 box=moBox(s);"
    "vec2 c=vec2(box.x+box.z*clamp(values[2],-100.,200.)/100.,box.y+box.w*clamp(values[3],-100.,200.)/100.);"
    "float total=-log(1.-amount);float step=total/(pow(4.,float(wavyPasses-2))-1.)*pow(4.,float(wavyPass-1));"
    "vec4 sum=vec4(0.);for(int i=0;i<4;++i)sum+=rySample(c+(p-c)*exp(-float(i)*step));return sum*.25;}"
    "vec4 light=v,o=wavyOriginal(ivec2(p));o=only?vec4(0.):vec4(o.rgb*o.a,o.a);"
    "vec3 rgb=min(vec3(1.),o.rgb+light.rgb*intensity);float alpha=min(1.,max(max(o.a,rgb.r),max(rgb.g,rgb.b)));"
    "return moStraight(vec4(rgb,alpha));}";

// ---- Speed Lines -------------------------------------------------------------------
// Streaks of speed drawn over the layer, the way comics and anime show a rush:
// side by side across it (Horizontal, at any angle), or all pointing in at a
// centre (Radial), leaving the middle clear for what the eye should land on.
// Dealt anew several times a second, so they flicker as they do on paper.
struct SpeedValues {
  int mode, count;
  double thickness, length, variation, opacity, speed, travel, angle, cx, cy;
  float colour[4];
  bool linesOnly;
  uint32_t seed;
};
SpeedValues speedValues(const float *v, int32_t n) {
  SpeedValues s{};
  s.mode = std::clamp(int(std::lround(value(v, n, 0, 0))), 0, 1);
  s.count = std::clamp(int(std::lround(value(v, n, 1, 60))), 0, 2000);
  s.thickness = std::clamp(double(value(v, n, 2, 4)), .1, 400.);
  s.length = std::clamp(double(value(v, n, 3, 60)), 1., 100.) / 100.;
  s.variation = std::clamp(double(value(v, n, 4, 60)), 0., 100.) / 100.;
  for (int c = 0; c < 4; ++c) s.colour[c] = std::clamp(value(v, n, 5 + c, 1), 0.f, 1.f);
  s.opacity = std::clamp(double(value(v, n, 9, 100)), 0., 100.) / 100.;
  s.speed = std::clamp(double(value(v, n, 10, 12)), 0., 60.);
  s.travel = std::clamp(double(value(v, n, 11, 0)), -20000., 20000.);
  s.angle = double(value(v, n, 12, 0));
  s.cx = std::clamp(double(value(v, n, 13, 50)), -100., 200.) / 100.;
  s.cy = std::clamp(double(value(v, n, 14, 50)), -100., 200.) / 100.;
  s.linesOnly = value(v, n, 15, 0) >= .5f;
  s.seed = uint32_t(int(value(v, n, 16, 1)));
  return s;
}
int speedInert(const float *v, int32_t n) {
  const SpeedValues s = speedValues(v, n);
  return !s.linesOnly && (s.count < 1 || s.opacity <= 0 || s.colour[3] <= 0);
}
int speedVaries(const float *v, int32_t n) {
  const SpeedValues s = speedValues(v, n);
  return s.count > 0 && (s.speed > 0 || (s.mode == 0 && std::abs(s.travel) > 0));
}
uint32_t speedControlState(const float *v, int32_t n, int32_t parameter) {
  const int mode = speedValues(v, n).mode;
  if (mode == 1 && (parameter == 8 || parameter == 9)) return uint32_t(WFX_CONTROL_HIDDEN);  // travel, angle
  if (mode == 0 && (parameter == 10 || parameter == 11)) return uint32_t(WFX_CONTROL_HIDDEN); // centre
  return 0u;
}
int speedProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32 || width <= 0 || height <= 0) return 1;
  const SpeedValues s = speedValues(v, n);
  if (speedInert(v, n)) return 0;
  const double scale = context->scale > 0 ? context->scale : 1.;
  const Box box = boxOf(context, width, height);
  const uint32_t step = s.speed > 0 ? uint32_t(int64_t(std::floor(context->seconds * s.speed + 1e-6))) : 0u;
  auto random = [&](int line, uint32_t what) { return double(unit(hash(s.seed, step, uint32_t(line), what))); };
  const float alpha = float(s.opacity) * s.colour[3];
  // The colour laid over with this much cover, premultiplied as the pixels are.
  auto lay = [&](float *d, float cover) {
    const float a = cover * alpha;
    if (a <= 0) return;
    for (int c = 0; c < 3; ++c) d[c] = s.colour[c] * a + d[c] * (1 - a);
    d[3] = a + d[3] * (1 - a);
  };
  struct Line { double across, from, to, half, cosine = 1, sine = 0; };
  std::vector<Line> lines(size_t(std::max(0, s.count)));
  if (s.mode == 0) {
    // Side by side along `angle`, spread across the whole box.
    const double a = s.angle * kPi / 180., ux = std::cos(a), uy = std::sin(a);
    const double alongHalf = (std::abs(box.w * ux) + std::abs(box.h * uy)) / 2;
    const double acrossHalf = (std::abs(box.w * uy) + std::abs(box.h * ux)) / 2;
    double widest = 0;
    for (int i = 0; i < s.count; ++i) {
      Line &l = lines[size_t(i)];
      l.across = (random(i, 0) * 2 - 1) * (acrossHalf + s.thickness * scale);
      const double length = s.length * 2 * alongHalf * (1 - s.variation * random(i, 1) * .8);
      // Travel slides every streak along, wrapping round past the far side.
      const double span = 2 * alongHalf + length;
      double middle = (random(i, 2) * 2 - 1) * alongHalf + s.travel * scale * context->seconds;
      middle = std::fmod(middle + alongHalf + length / 2, span);
      if (middle < 0) middle += span;
      middle -= alongHalf + length / 2;
      l.from = middle - length / 2;
      l.to = middle + length / 2;
      l.half = s.thickness * scale / 2 * (1 - s.variation * random(i, 3) * .7);
      widest = std::max(widest, l.half);
    }
    std::sort(lines.begin(), lines.end(), [](const Line &p, const Line &q) { return p.across < q.across; });
    const double cx = box.x + box.w / 2, cy = box.y + box.h / 2;
    forRows(context, height, [&](int32_t begin, int32_t end) {
      for (int32_t y = begin; y < end; ++y) {
        float *p = row(pixels, stride, y);
        // The streaks that can reach this row at all: across runs straight
        // along it, so between its two ends.
        const double py = y + .5 - cy;
        const double acrossA = -(.5 - cx) * uy + py * ux, acrossB = -(width - .5 - cx) * uy + py * ux;
        auto rowFirst = std::lower_bound(lines.begin(), lines.end(), std::min(acrossA, acrossB) - widest - 1,
                                         [](const Line &l, double at) { return l.across < at; });
        auto rowLast = std::upper_bound(lines.begin(), lines.end(), std::max(acrossA, acrossB) + widest + 1,
                                        [](double at, const Line &l) { return at < l.across; });
        if (rowFirst == rowLast && !s.linesOnly) continue;
        for (int32_t x = 0; x < width; ++x) {
          float *d = p + size_t(x) * 4;
          if (s.linesOnly) d[0] = d[1] = d[2] = d[3] = 0;
          if (rowFirst == rowLast) continue;
          const double px = x + .5 - cx;
          const double along = px * ux + py * uy, across = -px * uy + py * ux;
          // Only the streaks near enough across can cover it.
          float cover = 0;
          for (auto it = rowFirst; it != rowLast; ++it) {
            if (std::abs(it->across - across) > widest + 1) continue;
            if (along <= it->from || along >= it->to) continue;
            // Thickest in the middle, drawn to a point at both ends.
            const double t = (along - it->from) / (it->to - it->from);
            const double half = it->half * std::sqrt(std::sin(kPi * t));
            cover = std::max(cover, float(std::clamp(half - std::abs(across - it->across) + .5, 0., 1.)));
          }
          lay(d, cover);
        }
      }
    });
    return 0;
  }
  // Radial: every streak points at the centre, thick at the edge and drawn to
  // a point on its way in; Length is how far in from the edge it reaches.
  const double cx = box.x + box.w * s.cx, cy = box.y + box.h * s.cy;
  double outer = 0;
  for (double x : {box.x, box.x + box.w})
    for (double y : {box.y, box.y + box.h}) outer = std::max(outer, std::hypot(x - cx, y - cy));
  double innermost = outer, widestAngle = 0;
  for (int i = 0; i < s.count; ++i) {
    Line &l = lines[size_t(i)];
    l.across = random(i, 0) * 2 * kPi - kPi;  // which way from the centre
    l.from = outer * (1 - s.length * (1 - s.variation * random(i, 1) * .8));  // where its point is
    l.to = outer;
    l.half = s.thickness * scale / 2 * (1 - s.variation * random(i, 3) * .7);
    l.cosine = std::cos(l.across);
    l.sine = std::sin(l.across);
    innermost = std::min(innermost, l.from);
    widestAngle = std::max(widestAngle, (l.half + 1) / std::max(1., l.from));
  }
  std::sort(lines.begin(), lines.end(), [](const Line &p, const Line &q) { return p.across < q.across; });
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      for (int32_t x = 0; x < width; ++x) {
        float *d = p + size_t(x) * 4;
        if (s.linesOnly) d[0] = d[1] = d[2] = d[3] = 0;
        const double dx = x + .5 - cx, dy = y + .5 - cy, r = std::hypot(dx, dy);
        if (r <= innermost || lines.empty()) continue;
        const double at = std::atan2(dy, dx);
        // The streaks within reach of this angle, round the circle both ways.
        const double window = std::min(kPi, widestAngle * std::max(1., outer / r));
        float cover = 0;
        auto look = [&](double from, double to, double shift) {
          auto it = std::lower_bound(lines.begin(), lines.end(), from,
                                     [](const Line &l, double a) { return l.across < a; });
          for (; it != lines.end() && it->across <= to; ++it) {
            if (r <= it->from) continue;
            // How far off the streak's line, across it: the streak's own
            // direction is the same whichever way round the circle it was found.
            (void)shift;
            if (dx * it->cosine + dy * it->sine <= 0) continue;
            const double off = std::abs(dx * it->sine - dy * it->cosine);
            if (off > it->half + 1) continue;
            const double half = it->half * std::pow((r - it->from) / std::max(1e-6, it->to - it->from), .8);
            cover = std::max(cover, float(std::clamp(half - off + .5, 0., 1.)));
          }
        };
        look(at - window, at + window, 0);
        if (at - window < -kPi) look(at - window + 2 * kPi, kPi, -2 * kPi);
        if (at + window > kPi) look(-kPi, at + window - 2 * kPi, 2 * kPi);
        lay(d, cover);
      }
    }
  });
  return 0;
}
const WfxParameter speedParameters[] = {
    choice("mode", "Mode", "Horizontal\nRadial", 2, 0),
    number("count", "Lines", 0, 2000, 60, 1),
    number("thickness", "Thickness", .1f, 400, 4, .1f, " px"),
    number("length", "Length", 1, 100, 60, 1, " %"),
    number("variation", "Variation", 0, 100, 60, 1, " %"),
    color("colour", "Color", 1, 1, 1, 1),
    number("opacity", "Opacity", 0, 100, 100, 1, " %"),
    number("speed", "Speed", 0, 60, 12, .5f, " /s"),
    number("travel", "Travel", -20000, 20000, 0, 10, " px/s"),
    number("angle", "Direction", -360, 360, 0, 1, "\xc2\xb0"),
    number("x", "Centre X", -100, 200, 50, .1f, " %"),
    number("y", "Centre Y", -100, 200, 50, .1f, " %"),
    toggle("linesOnly", "Lines Only", false),
    number("seed", "Seed", 0, 9999, 1, 1)};
const char *const speedHelp[] = {
    "Horizontal: streaks side by side across the layer. Radial: streaks pointing in at a centre.",
    "How many streaks.",
    "How thick a streak is at its thickest, in pixels.",
    "Horizontal: how long a streak is, against the layer. Radial: how far in from the edge it reaches.",
    "How much the streaks differ from each other in length and thickness.",
    "The streaks' colour.",
    "How solid the streaks are.",
    "How many times a second the streaks are dealt again. Zero keeps one set still.",
    "Horizontal: how fast every streak slides along, in pixels a second.",
    "Horizontal: which way the streaks run. 0 is side to side, 90 up and down.",
    "Radial: where the streaks point, across the layer.",
    "Radial: where the streaks point, down the layer.",
    "Only the streaks, without the layer under them: for a layer of its own over others.",
    "Another number, another set of streaks."};
// On the card while the streaks are few enough for every pixel to look at all
// of them. Each is dealt from the same hashes, and one too far across to cover
// a pixel is passed over the way process()'s search passes it over, so the
// sorting and searching there change nothing.
int32_t speedShaderPasses(const float *v, int32_t n, const WfxContext *) { return speedValues(v, n).count > 500 ? 0 : 1; }
const char *speedShader = MOTION_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "const float pi=3.14159265358979;"
    "int mode=clamp(moWhole(values[0]),0,1),count=clamp(moWhole(values[1]),0,2000);"
    "float thickness=clamp(values[2],.1,400.),len=clamp(values[3],1.,100.)/100.,variation=clamp(values[4],0.,100.)/100.;"
    "vec4 colour=clamp(vec4(values[5],values[6],values[7],values[8]),0.,1.);float opacity=clamp(values[9],0.,100.)/100.;"
    "float speed=clamp(values[10],0.,60.),travel=clamp(values[11],-20000.,20000.);"
    "bool only=values[15]>=.5;uint seed=uint(int(values[16]));"
    "if(!only&&(count<1||opacity<=0.||colour.a<=0.))return moKept(v);"
    "float scale=wavyScale>0.?wavyScale:1.;vec4 box=moBox(s);uint step=0u;"
    "if(speed>0.){float x=wavySeconds*speed;step=uint(int(floor(x+max(1e-6,abs(x)*4e-7))));}"
    "vec4 d=only?vec4(0.):vec4(v.rgb*v.a,v.a);float cover=0.;"
    "if(mode==0){float a=mod(values[12],360.)*pi/180.;float ux=cos(a),uy=sin(a);"
    "float alongHalf=(abs(box.z*ux)+abs(box.w*uy))/2.,acrossHalf=(abs(box.z*uy)+abs(box.w*ux))/2.;"
    "vec2 q=p-(box.xy+box.zw/2.);float along=q.x*ux+q.y*uy,across=-q.x*uy+q.y*ux;"
    "for(int i=0;i<count;++i){"
    "float lineAcross=(moUnit(moHash(seed,step,uint(i),0u))*2.-1.)*(acrossHalf+thickness*scale);"
    "float hw=thickness*scale/2.*(1.-variation*moUnit(moHash(seed,step,uint(i),3u))*.7);"
    "float off=abs(lineAcross-across);if(off>hw+1.)continue;"
    "float lineLength=len*2.*alongHalf*(1.-variation*moUnit(moHash(seed,step,uint(i),1u))*.8);"
    "float span=2.*alongHalf+lineLength;"
    "float middle=(moUnit(moHash(seed,step,uint(i),2u))*2.-1.)*alongHalf+travel*scale*wavySeconds+alongHalf+lineLength/2.;"
    "middle-=span*trunc(middle/span);if(middle<0.)middle+=span;middle-=alongHalf+lineLength/2.;"
    "float from=middle-lineLength/2.,to=middle+lineLength/2.;if(along<=from||along>=to)continue;"
    "float w=hw*sqrt(sin(pi*((along-from)/(to-from))));cover=max(cover,clamp(w-off+.5,0.,1.));}"
    "}else{"
    "vec2 c=vec2(box.x+box.z*clamp(values[13],-100.,200.)/100.,box.y+box.w*clamp(values[14],-100.,200.)/100.);"
    "float outer=max(max(length(box.xy-c),length(vec2(box.x+box.z,box.y)-c)),"
    "max(length(vec2(box.x,box.y+box.w)-c),length(box.xy+box.zw-c)));"
    "float innermost=outer,widestAngle=0.;"
    "for(int i=0;i<count;++i){float from=outer*(1.-len*(1.-variation*moUnit(moHash(seed,step,uint(i),1u))*.8));"
    "float hw=thickness*scale/2.*(1.-variation*moUnit(moHash(seed,step,uint(i),3u))*.7);"
    "innermost=min(innermost,from);widestAngle=max(widestAngle,(hw+1.)/max(1.,from));}"
    "float dx=p.x-c.x,dy=p.y-c.y,r=sqrt(dx*dx+dy*dy);"
    "if(r>innermost&&count>0){float at=atan(dy,dx);float window=min(pi,widestAngle*max(1.,outer/r));"
    "for(int i=0;i<count;++i){float dir=moUnit(moHash(seed,step,uint(i),0u))*2.*pi-pi;"
    "if(!((dir>=at-window&&dir<=at+window)||(at-window<-pi&&dir>=at-window+2.*pi)||(at+window>pi&&dir<=at+window-2.*pi)))continue;"
    "float from=outer*(1.-len*(1.-variation*moUnit(moHash(seed,step,uint(i),1u))*.8));if(r<=from)continue;"
    "float cs=cos(dir),sn=sin(dir);if(dx*cs+dy*sn<=0.)continue;float off=abs(dx*sn-dy*cs);"
    "float hw=thickness*scale/2.*(1.-variation*moUnit(moHash(seed,step,uint(i),3u))*.7);if(off>hw+1.)continue;"
    "float w=hw*pow((r-from)/max(1e-6,outer-from),.8);cover=max(cover,clamp(w-off+.5,0.,1.));}}}"
    "float a=cover*(opacity*colour.a);if(a>0.)d=vec4(colour.rgb*a+d.rgb*(1.-a),a+d.a*(1.-a));"
    "return moStraight(d);}";

// ---- Posterize Time ------------------------------------------------------------------
// The layer moved at a lower frame rate than the composition: every
// animation of it held for a few frames and then jumping on, the stepped look
// of hand-drawn animation. The host steps the layer's own clock; the pixels
// are left as they are, so there is nothing to do here.
int posterizeTimeInert(const float *, int32_t) { return 1; }
int posterizeTimeProcess(void *, int32_t, int32_t, int32_t, const float *, int32_t, const WfxContext *) { return 0; }
const WfxParameter posterizeTimeParameters[] = {number("rate", "Frame Rate", 1, 120, 12, 1, " fps")};
const char *const posterizeTimeHelp[] = {
    "How many times a second the layer moves on. 12 is the stepped look of animation drawn on twos; "
    "at the composition's own rate or more, nothing changes."};

// ---- Sharpen ---------------------------------------------------------------------
// Edges made crisper the way an unsharp mask does it: the picture blurred by
// Radius, and each pixel pushed away from its blurred self by Amount. Threshold
// leaves alone the small differences -- skin, sky, grain -- and sharpens only
// real edges. Colour only: the coverage stays as it is.
enum { SharpAmount, SharpRadius, SharpThreshold };
int sharpenInert(const float *v, int32_t n) { return std::clamp(divided(v, n, SharpAmount, 100, 100.), 0.f, 5.f) <= 0; }
int sharpenProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32 || width <= 0 || height <= 0) return 1;
  const float amount = std::clamp(divided(v, n, SharpAmount, 100, 100.), 0.f, 5.f);
  if (amount <= 0) return 0;
  const double sigma = std::clamp(double(value(v, n, SharpRadius, 1.5f)), .3, 30.) * (context->scale > 0 ? context->scale : 1.);
  const float threshold = std::clamp(divided(v, n, SharpThreshold, 0, 255.), 0.f, 1.f);
  const int half = std::max(1, int(std::ceil(sigma * 3)));
  std::vector<float> kernel(size_t(half) * 2 + 1);
  double sum = 0;
  for (int i = -half; i <= half; ++i) sum += kernel[size_t(i + half)] = float(std::exp(-double(i) * i / (2 * sigma * sigma)));
  for (float &k : kernel) k = float(k / sum);
  const size_t line = size_t(width) * 4;
  Scratch keptOriginal(line * size_t(height)), keptAcross(line * size_t(height));
  float *const original = keptOriginal.data(), *const across = keptAcross.data();
  // The picture as it was, and blurred across.
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *o = original + size_t(y) * line;
      std::memcpy(o, row(pixels, stride, y), line * sizeof(float));
      float *a = across + size_t(y) * line;
      for (int32_t x = 0; x < width; ++x) {
        float acc[3] = {0, 0, 0};
        for (int i = -half; i <= half; ++i) {
          const float *t = o + size_t(std::clamp(x + i, 0, width - 1)) * 4;
          const float k = kernel[size_t(i + half)];
          for (int c = 0; c < 3; ++c) acc[c] += t[c] * k;
        }
        for (int c = 0; c < 3; ++c) a[size_t(x) * 4 + c] = acc[c];
      }
    }
  });
  // Blurred down as well, and each pixel pushed away from it.
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *d = row(pixels, stride, y);
      const float *o = original + size_t(y) * line;
      for (int32_t x = 0; x < width; ++x) {
        const float alpha = o[size_t(x) * 4 + 3];
        if (alpha <= 0.f) continue;
        float blurred[3] = {0, 0, 0};
        for (int i = -half; i <= half; ++i) {
          const float *t = across + size_t(std::clamp(y + i, 0, height - 1)) * line + size_t(x) * 4;
          const float k = kernel[size_t(i + half)];
          for (int c = 0; c < 3; ++c) blurred[c] += t[c] * k;
        }
        for (int c = 0; c < 3; ++c) {
          const float self = o[size_t(x) * 4 + c], difference = self - blurred[c];
          if (std::abs(difference) < threshold * alpha) continue;
          d[size_t(x) * 4 + c] = std::clamp(self + difference * amount, 0.f, alpha);
        }
      }
    }
  });
  return 0;
}
const WfxParameter sharpenParameters[] = {number("amount", "Amount", 0, 500, 100, 1, " %"),
                                          number("radius", "Radius", .3f, 30, 1.5f, .1f, " px"),
                                          number("threshold", "Threshold", 0, 255, 0, 1)};
const char *const sharpenHelp[] = {"How much crisper the edges are made", "How wide an edge is taken to be, in layer pixels",
                                   "How different neighbours must be before they are sharpened (out of 255): keeps skin and sky smooth"};
// On the card in two rounds: blurred across (the premultiplied colour kept as
// it is between them), then blurred down and each pixel pushed away from it,
// its own colour read again from the picture as it came (wavyOriginal).
int32_t sharpenShaderPasses(const float *v, int32_t n, const WfxContext *) { return sharpenInert(v, n) ? 1 : 2; }
const char *sharpenShader = MOTION_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float amount=clamp(values[0]/100.,0.,5.);ivec2 at=ivec2(p);vec4 own=wavyOriginal(at);"
    "if(amount<=0.)return moKept(own);"
    "float sigma=clamp(values[1],.3,30.)*(wavyScale>0.?wavyScale:1.);float threshold=clamp(values[2]/255.,0.,1.);"
    "int reach=max(1,int(ceil(sigma*3.)));float spread=2.*sigma*sigma,total=0.;"
    "for(int i=-reach;i<=reach;++i)total+=exp(-float(i*i)/spread);vec3 acc=vec3(0.);"
    "if(wavyPass==0){for(int i=-reach;i<=reach;++i){vec4 t=wavyPixel(ivec2(clamp(at.x+i,0,int(s.x)-1),at.y));"
    "acc+=t.rgb*t.a*(exp(-float(i*i)/spread)/total);}return vec4(acc,1.);}"
    "if(own.a<=0.)return vec4(0.);"
    "for(int i=-reach;i<=reach;++i)acc+=wavyPixel(ivec2(at.x,clamp(at.y+i,0,int(s.y)-1))).rgb*(exp(-float(i*i)/spread)/total);"
    "vec3 self=own.rgb*own.a,d=self;"
    "for(int c=0;c<3;++c){float diff=self[c]-acc[c];if(abs(diff)>=threshold*own.a)d[c]=clamp(self[c]+diff*amount,0.,own.a);}"
    "return vec4(d/own.a,own.a);}";

#define COUNT(parameters) int32_t(sizeof(parameters) / sizeof(parameters[0])), parameters
#define HELP(help, parameters)                                                                                  \
  static_assert(sizeof(help) / sizeof(help[0]) == sizeof(parameters) / sizeof(parameters[0]),                   \
                "one help line for each entry of " #parameters)
HELP(glitchHelp, glitchParameters);
HELP(shakeHelp, shakeParameters);
HELP(mirrorHelp, mirrorParameters);
HELP(kaleidoHelp, kaleidoParameters);
HELP(pixelateHelp, pixelateParameters);
HELP(directionalHelp, directionalParameters);
HELP(zoomHelp, zoomParameters);
HELP(speedHelp, speedParameters);
HELP(posterizeTimeHelp, posterizeTimeParameters);
constexpr uint32_t kMoved = uint32_t(WFX_PREMULTIPLIED);
constexpr uint32_t kWhole = uint32_t(WFX_PREMULTIPLIED) | uint32_t(WFX_WHOLE_LAYER);
const WfxEffect effects[] = {
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Glitch", "Glitch", "Music Video", 4u, kMoved | uint32_t(WFX_MOVES_ALONE),
     COUNT(glitchParameters), glitchProcess, glitchInert, glitchVaries, glitchReach, nullptr, nullptr, nullptr, glitchShader,
     nullptr, nullptr, nullptr, glitchHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Shake", "Shake", "Music Video", 4u, kWhole | uint32_t(WFX_MOVES_ALONE),
     COUNT(shakeParameters), shakeProcess, shakeInert, shakeVaries, shakeReach, nullptr, nullptr, nullptr, shakeShader,
     nullptr, nullptr, nullptr, shakeHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Mirror", "Mirror", "Music Video", 4u, kWhole, COUNT(mirrorParameters),
     mirrorProcess, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, mirrorShader,
     nullptr, nullptr, nullptr, mirrorHelp, mirrorControlState},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Kaleidoscope", "Kaleidoscope", "Music Video", 4u, kWhole,
     COUNT(kaleidoParameters), kaleidoProcess, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, kaleidoShader,
     nullptr, nullptr, nullptr, kaleidoHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Pixelate", "Pixelate", "Music Video", 4u, kWhole,
     COUNT(pixelateParameters), pixelateProcess, pixelateInert, nullptr, nullptr, nullptr, nullptr, nullptr, pixelateShader,
     nullptr, nullptr, nullptr, pixelateHelp, nullptr, pixelateShaderPasses},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Directional Blur", "Directional Blur", "Blur & Sharpen", 4u, kMoved,
     COUNT(directionalParameters), directionalProcess, directionalInert, nullptr, directionalReach, nullptr, nullptr,
     nullptr, directionalShader, nullptr, nullptr, nullptr, directionalHelp, nullptr, directionalShaderPasses},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Zoom Blur", "Zoom Blur", "Blur & Sharpen", 4u, kWhole,
     COUNT(zoomParameters), zoomProcess, zoomInert, nullptr, nullptr, nullptr, nullptr, nullptr, zoomShader,
     zoomReachWithData, nullptr, nullptr, zoomHelp, nullptr, zoomShaderPasses},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Sharpen", "Sharpen", "Blur & Sharpen", 4u, uint32_t(WFX_PREMULTIPLIED),
     COUNT(sharpenParameters), sharpenProcess, sharpenInert, nullptr, nullptr, nullptr, nullptr, nullptr, sharpenShader,
     nullptr, nullptr, nullptr, sharpenHelp, nullptr, sharpenShaderPasses},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Light Rays", "Light Rays", "Stylize", 4u, uint32_t(WFX_PREMULTIPLIED),
     COUNT(raysParameters), raysProcess, raysInert, nullptr, nullptr, nullptr, nullptr, nullptr, raysShader,
     raysReachWithData, nullptr, nullptr, raysHelp, nullptr, raysShaderPasses},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Speed Lines", "Speed Lines", "Music Video", 4u, kWhole | uint32_t(WFX_MOVES_ALONE),
     COUNT(speedParameters), speedProcess, speedInert, speedVaries, nullptr, nullptr, nullptr, nullptr, speedShader,
     nullptr, nullptr, nullptr, speedHelp, speedControlState, speedShaderPasses},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Posterize Time", "Posterize Time", "Time", 4u, 0u,
     COUNT(posterizeTimeParameters), posterizeTimeProcess, posterizeTimeInert, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr, posterizeTimeHelp, nullptr}};
}  // namespace

WFX_EXPORT int32_t wfxEffectCount(void) { return int32_t(sizeof(effects) / sizeof(effects[0])); }
WFX_EXPORT const WfxEffect *wfxEffectAt(int32_t index) {
  return index >= 0 && index < wfxEffectCount() ? &effects[index] : nullptr;
}
