// Wavy-Pop.wfx: the look of an anime or Vocaloid music video -- Pattern,
// Sunburst, Sparkles, Light Leak and TV Screen. Pattern and Sunburst fill the
// layer they are on (a Fill layer for the whole frame, or a title, filled with
// dots); Sparkles and Light Leak lay light over it, clear parts included; TV
// Screen puts the whole picture inside a rounded, slightly bulging screen.
#include "wavy_effect.h"
#include "pack_common.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

using namespace wfxpack;

namespace {
constexpr float kTau = 6.28318531f;
constexpr double kPi = 3.14159265358979323846;

float *row(void *pixels, int32_t stride, int32_t y) {
  return reinterpret_cast<float *>(static_cast<unsigned char *>(pixels) + size_t(stride) * size_t(y));
}
struct Rect { float x, y, w, h; };
// The layer's box in raster pixels; the whole raster where the host gives none.
Rect boxOf(const WfxContext *context, int32_t width, int32_t height) {
  double bx = context->boxX, by = context->boxY, bw = context->boxWidth, bh = context->boxHeight;
  if (!(bw > 0 && bh > 0)) {
    const RasterPlace place = rasterPlace(context, width, height);
    bx = 0; by = 0; bw = place.width; bh = place.height;
  }
  return {float(bx), float(by), float(bw), float(bh)};
}
float smooth01(float t) {
  t = std::clamp(t, 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}
uint32_t scramble(uint32_t h) {
  h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15; h *= 0x846ca68bU; h ^= h >> 16;
  return h;
}
// A number in [0, 1) that depends only on a cell, a seed and which number of
// the cell's it is: the same on every frame and in every strip.
float hashed(int a, int b, uint32_t seed, uint32_t which) {
  const uint32_t h = scramble(uint32_t(a) * 0x9E3779B1u ^ scramble(uint32_t(b) * 0x85EBCA6Bu + which * 0x27D4EB2Fu) ^
                              scramble(seed + 0x165667B1u));
  return float(h >> 8) / 16777216.f;
}
struct Colour { float r, g, b, a; };
Colour colourAt(const float *v, int32_t n, int index, Colour fallback) {
  return {std::clamp(value(v, n, index, fallback.r), 0.f, 1.f), std::clamp(value(v, n, index + 1, fallback.g), 0.f, 1.f),
          std::clamp(value(v, n, index + 2, fallback.b), 0.f, 1.f), std::clamp(value(v, n, index + 3, fallback.a), 0.f, 1.f)};
}
int whole(const float *v, int32_t n, int index, int fallback) { return int(std::lround(value(v, n, index, float(fallback)))); }

// The helpers above, for the card: the same hash to the bit, the same box.
// The shaders take straight colour and give it back; the effects that work on
// premultiplied colour multiply it in and divide it out again themselves. An
// angle that turns with time is taken round to one turn first: the card's sine
// is only good near nought.
#define POP_GLSL \
  "uint popScramble(uint h){h^=h>>16u;h*=0x7feb352du;h^=h>>15u;h*=0x846ca68bu;h^=h>>16u;return h;}" \
  "float popHashed(int a,int b,uint seed,uint which){uint h=popScramble(uint(a)*0x9E3779B1u^" \
  "popScramble(uint(b)*0x85EBCA6Bu+which*0x27D4EB2Fu)^popScramble(seed+0x165667B1u));return float(h>>8u)/16777216.;}" \
  "vec4 popBox(vec2 s){vec4 b=wavyBox;if(!(b.z>0.&&b.w>0.))b=vec4(0.,0.,s);return b;}" \
  "int popWhole(float v){return int(v<0.?-floor(-v+.5):floor(v+.5));}" \
  "float popSmooth(float t){t=clamp(t,0.,1.);return t*t*(3.-2.*t);}" \
  "vec4 popStraight(vec4 d){return d.a>0.?vec4(d.rgb/d.a,d.a):vec4(0.);}" \
  "float popTurn(float x){return 6.28318531*(x-floor(x));}"
// fillWith() below, for the card.
#define POP_FILL_GLSL \
  "vec4 popFill(vec4 p,float cover,vec4 a,vec4 b,float opacity){if(p.a<=0.)return p;" \
  "float pa=a.a*cover+b.a*(1.-cover);vec3 pre=a.rgb*a.a*cover+b.rgb*b.a*(1.-cover);" \
  "float kept=1.-opacity;float alpha=opacity*pa+kept;if(alpha<=0.)return vec4(p.rgb,0.);" \
  "return vec4((opacity*pre+kept*p.rgb)/alpha,p.a*alpha);}" \
  "vec4 popColour(int i){return clamp(vec4(values[i],values[i+1],values[i+2],values[i+3]),0.,1.);}"

// A drawn picture laid into the layer: where the layer is, its colours become
// the picture's (by Opacity), and where the picture is clear the layer is too.
// Straight colour in and out; `cover` is how much of colour A the place shows.
void fillWith(float *p, float cover, const Colour &a, const Colour &b, float opacity) {
  const float own = p[3];
  if (own <= 0.f) return;
  const float pa = a.a * cover + b.a * (1.f - cover);
  float pre[3] = {a.r * a.a * cover + b.r * b.a * (1.f - cover), a.g * a.a * cover + b.g * b.a * (1.f - cover),
                  a.b * a.a * cover + b.b * b.a * (1.f - cover)};
  const float kept = 1.f - opacity;
  const float alpha = opacity * pa + kept;
  if (alpha <= 0.f) { p[3] = 0.f; return; }
  for (int c = 0; c < 3; ++c) p[c] = (opacity * pre[c] + kept * p[c]) / alpha;
  p[3] = own * alpha;
}

// ---- Pattern -------------------------------------------------------------------------
// Polka dots, stripes, a checkerboard, graph paper, diamonds or hearts, in two
// colours, any size and angle, able to scroll by itself. Colour B clear leaves
// only the pattern: a title made of dots.
enum { PatType, PatA, PatB = PatA + 4, PatSize = PatB + 4, PatAngle, PatThickness, PatScrollX, PatScrollY, PatSoftness, PatOpacity };
int patternInert(const float *v, int32_t n) { return std::clamp(divided(v, n, PatOpacity, 100, 100.), 0.f, 1.f) <= 0; }
int patternVaries(const float *v, int32_t n) {
  return std::abs(value(v, n, PatScrollX, 0)) > 1e-6f || std::abs(value(v, n, PatScrollY, 0)) > 1e-6f;
}
float fract(float x) { return x - std::floor(x); }
// How much of colour A shows at (u, v), in cells; `aa` is half a pixel, in cells.
float patternCover(int type, float u, float v, float thick, float aa) {
  const float fu = fract(u), fv = fract(v);
  auto edge = [&](float distance) { return 1.f - smooth01(distance / (2.f * aa) + .5f); };  // inside where distance < 0
  switch (type) {
    case 1: // stripes, across u
      return edge(std::abs(fu - .5f) - thick * .5f);
    case 2: { // checkerboard: a square wave each way, softened over a pixel
      auto square = [&](float x) { return std::clamp((.25f - std::abs(fract(x * .5f) - .5f)) * 2.f / (2.f * aa), -1.f, 1.f); };
      return .5f + .5f * square(u) * square(v);
    }
    case 3: { // graph paper: lines along every cell edge
      const float half = thick * .125f;
      const float du = std::min(fu, 1.f - fu), dv = std::min(fv, 1.f - fv);
      return std::max(edge(du - half), edge(dv - half));
    }
    case 4: // diamonds
      return edge((std::abs(fu - .5f) + std::abs(fv - .5f) - thick * .5f) * .7071f);
    case 5: { // hearts
      const float s = std::max(.05f, thick * .55f);
      auto heart = [&](float x, float y) {
        x = (x - .5f) / s * 1.15f;
        y = -(y - .5f) / s * 1.15f + .15f;
        const float q = x * x + y * y - 1.f;
        return q * q * q - x * x * y * y * y;
      };
      const float h = heart(fu, fv), e = .002f;
      const float gx = (heart(fu + e, fv) - heart(fu - e, fv)) / (2 * e), gy = (heart(fu, fv + e) - heart(fu, fv - e)) / (2 * e);
      return edge(h / std::max(1e-4f, std::sqrt(gx * gx + gy * gy)));
    }
    default: { // dots
      const float du = fu - .5f, dv = fv - .5f;
      return edge(std::sqrt(du * du + dv * dv) - thick * .5f);
    }
  }
}
int patternProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float opacity = std::clamp(divided(v, n, PatOpacity, 100, 100.), 0.f, 1.f);
  if (opacity <= 0.f) return 0;
  const int type = std::clamp(whole(v, n, PatType, 0), 0, 5);
  const Colour a = colourAt(v, n, PatA, {1, 1, 1, 1}), b = colourAt(v, n, PatB, {1, .45f, .62f, 1});
  const double scale = context->scale > 0 ? context->scale : 1.;
  const float size = float(std::max(1., std::clamp(double(value(v, n, PatSize, 40)), 2., 2000.) * scale));
  const double angle = double(value(v, n, PatAngle, 0)) * kPi / 180.;
  const float cs = float(std::cos(angle)), sn = float(std::sin(angle));
  const float thick = std::clamp(divided(v, n, PatThickness, 50, 100.), 0.f, 1.f);
  const float aa = std::max(.5f, std::clamp(value(v, n, PatSoftness, 1), 0.f, 200.f) * float(scale) * .5f) / size;
  const float shiftX = float(double(value(v, n, PatScrollX, 0)) * scale * context->seconds / size);
  const float shiftY = float(double(value(v, n, PatScrollY, 0)) * scale * context->seconds / size);
  const Rect box = boxOf(context, width, height);
  const RasterPlace place = rasterPlace(context, width, height);
  // Measured from the box's middle -- a dot, a square, a diamond sits right in
  // the middle of the layer -- so the pattern stays put on the layer however
  // much room the raster has round it.
  const float cx = box.x + box.w / 2, cy = box.y + box.h / 2;
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const float py = float(place.y + y) + .5f - cy;
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const float px = float(place.x + x) + .5f - cx;
        const float u = (px * cs + py * sn) / size - shiftX + .5f, w = (-px * sn + py * cs) / size - shiftY + .5f;
        fillWith(p + x * 4, patternCover(type, u, w, thick, aa), a, b, opacity);
      }
    }
  });
  return 0;
}
const WfxParameter patternParameters[] = {
    choice("type", "Pattern", "Dots\nStripes\nCheckerboard\nGraph Paper\nDiamonds\nHearts", 6, 0),
    color("colorA", "Colour A", 1, 1, 1, 1), color("colorB", "Colour B", 1, .45f, .62f, 1),
    number("size", "Size", 2, 2000, 40, 1, " px"), number("angle", "Angle", -3600, 3600, 0, 1, "\xc2\xb0"),
    number("thickness", "Thickness", 0, 100, 50, 1, " %"), number("scrollX", "Scroll Across", -5000, 5000, 0, 1, " px/s"),
    number("scrollY", "Scroll Down", -5000, 5000, 0, 1, " px/s"), number("softness", "Softness", 0, 200, 1, .1f, " px"),
    number("opacity", "Opacity", 0, 100, 100, 1, " %")};
const char *const patternHelp[] = {
    "What is drawn", "The pattern's colour (dots, stripes, lines...)", "The colour between (clear for the pattern alone)",
    "How big one repeat is, in layer pixels", "How the pattern is turned", "How much of each repeat Colour A takes",
    "How fast the pattern slides across, by itself", "How fast the pattern slides down, by itself",
    "How soft the pattern's edges are", "How much of the layer's own colour the pattern replaces"};
const char *patternShader = POP_GLSL POP_FILL_GLSL
    "float patEdge(float d,float aa){return 1.-popSmooth(d/(2.*aa)+.5);}"
    "float patSquare(float x,float aa){float f=x*.5;return clamp((.25-abs(f-floor(f)-.5))*2./(2.*aa),-1.,1.);}"
    "float patHeart(float x,float y,float s){x=(x-.5)/s*1.15;y=-(y-.5)/s*1.15+.15;float q=x*x+y*y-1.;return q*q*q-x*x*y*y*y;}"
    "float patCover(int type,float u,float v,float thick,float aa){float fu=u-floor(u),fv=v-floor(v);"
    "if(type==1)return patEdge(abs(fu-.5)-thick*.5,aa);"
    "if(type==2)return .5+.5*patSquare(u,aa)*patSquare(v,aa);"
    "if(type==3){float h=thick*.125;return max(patEdge(min(fu,1.-fu)-h,aa),patEdge(min(fv,1.-fv)-h,aa));}"
    "if(type==4)return patEdge((abs(fu-.5)+abs(fv-.5)-thick*.5)*.7071,aa);"
    "if(type==5){float s=max(.05,thick*.55);float h=patHeart(fu,fv,s),e=.002;"
    "float gx=(patHeart(fu+e,fv,s)-patHeart(fu-e,fv,s))/(2.*e),gy=(patHeart(fu,fv+e,s)-patHeart(fu,fv-e,s))/(2.*e);"
    "return patEdge(h/max(1e-4,sqrt(gx*gx+gy*gy)),aa);}"
    "float du=fu-.5,dv=fv-.5;return patEdge(sqrt(du*du+dv*dv)-thick*.5,aa);}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float opacity=clamp(values[15]/100.,0.,1.);if(opacity<=0.||v.a<=0.)return v;"
    "int type=clamp(popWhole(values[0]),0,5);float scale=wavyScale>0.?wavyScale:1.;"
    "float size=max(1.,clamp(values[9],2.,2000.)*scale);"
    "float angle=mod(values[10],360.)*3.14159265358979/180.;float cs=cos(angle),sn=sin(angle);"
    "float thick=clamp(values[11]/100.,0.,1.);float aa=max(.5,clamp(values[14],0.,200.)*scale*.5)/size;"
    "float shiftX=values[12]*scale*wavySeconds/size,shiftY=values[13]*scale*wavySeconds/size;"
    "vec4 box=popBox(s);vec2 d=p-(box.xy+box.zw/2.);"
    "float u=(d.x*cs+d.y*sn)/size-shiftX+.5,w=(-d.x*sn+d.y*cs)/size-shiftY+.5;"
    "return popFill(v,patCover(type,u,w,thick,aa),popColour(1),popColour(5),opacity);}";

// ---- Sunburst ------------------------------------------------------------------------
// Rays of two colours round a point, the burst behind a hero in a chorus,
// turning by itself; Twist curls them into a spiral, Fade melts them away
// from the middle out.
enum { SunRays, SunA, SunB = SunA + 4, SunCentreX = SunB + 4, SunCentreY, SunAngle, SunSpin, SunTwist, SunFade, SunOpacity };
int sunInert(const float *v, int32_t n) { return std::clamp(divided(v, n, SunOpacity, 100, 100.), 0.f, 1.f) <= 0; }
int sunVaries(const float *v, int32_t n) { return std::abs(value(v, n, SunSpin, 20)) > 1e-6f; }
int sunProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float opacity = std::clamp(divided(v, n, SunOpacity, 100, 100.), 0.f, 1.f);
  if (opacity <= 0.f) return 0;
  const float rays = float(std::clamp(whole(v, n, SunRays, 16), 2, 200));
  const Colour a = colourAt(v, n, SunA, {1, .82f, .3f, 1}), b = colourAt(v, n, SunB, {1, .93f, .55f, 1});
  const Rect box = boxOf(context, width, height);
  const float cx = box.x + box.w * std::clamp(divided(v, n, SunCentreX, 50, 100.), -1.f, 2.f);
  const float cy = box.y + box.h * std::clamp(divided(v, n, SunCentreY, 50, 100.), -1.f, 2.f);
  const double turn = (double(value(v, n, SunAngle, 0)) + double(value(v, n, SunSpin, 20)) * context->seconds) / 360.;
  const float start = float(turn - std::floor(turn));
  const double scale = context->scale > 0 ? context->scale : 1.;
  // Degrees of curl per 100 layer pixels out.
  const float twist = float(double(value(v, n, SunTwist, 0)) / 360. / (100. * scale));
  const float fade = std::clamp(divided(v, n, SunFade, 0, 100.), 0.f, 1.f);
  const float reach = std::max(1.f, std::hypot(box.w, box.h) / 2);
  const RasterPlace place = rasterPlace(context, width, height);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const float dy = float(place.y + y) + .5f - cy;
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const float dx = float(place.x + x) + .5f - cx;
        const float r = std::sqrt(dx * dx + dy * dy);
        // Around the middle in turns, then in rays: A on the first half of each,
        // the first one starting at Angle (0 is to the right).
        const float around = std::atan2(dy, dx) / kTau + start + twist * r;
        const float s = fract(around * rays);
        // Half a pixel, measured round the circle at this distance, in rays.
        const float aa = std::max(1e-4f, rays / (kTau * std::max(r, .5f)) * .5f);
        const float d = std::abs(s - .25f) - .25f;  // inside A where negative
        float cover = 1.f - smooth01(d / (2.f * aa) + .5f);
        if (r < 2.f) cover = .5f; // the very middle is both
        if (fade > 0.f) cover *= 1.f - smooth01(r / (reach * std::max(.02f, 1.f - fade + .02f)));
        fillWith(p + x * 4, cover, a, b, opacity);
      }
    }
  });
  return 0;
}
const WfxParameter sunParameters[] = {
    number("rays", "Rays", 2, 200, 16, 1), color("colorA", "Colour A", 1, .82f, .3f, 1), color("colorB", "Colour B", 1, .93f, .55f, 1),
    number("centerX", "Center X", -100, 200, 50, .5f, " %"), number("centerY", "Center Y", -100, 200, 50, .5f, " %"),
    number("angle", "Angle", -3600, 3600, 0, 1, "\xc2\xb0"), number("spin", "Spin", -720, 720, 20, 1, "\xc2\xb0/s"),
    number("twist", "Twist", -720, 720, 0, 1, "\xc2\xb0"), number("fade", "Fade", 0, 100, 0, 1, " %"),
    number("opacity", "Opacity", 0, 100, 100, 1, " %")};
const char *const sunHelp[] = {
    "How many rays of each colour", "The rays' colour", "The colour between the rays", "Where the middle is, across the layer",
    "Where the middle is, down the layer", "How the rays are turned", "How fast they turn by themselves, degrees a second",
    "How much they curl, degrees per 100 pixels out", "How soon the rays melt into Colour B going out",
    "How much of the layer's own colour the burst replaces"};
// On the card. Angle is taken round to one turn before Spin's time is added,
// so a big Angle costs no precision; a whole turn more or less from the arc
// tangent moves every ray by a whole ray, which changes nothing.
const char *sunShader = POP_GLSL POP_FILL_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float opacity=clamp(values[15]/100.,0.,1.);if(opacity<=0.||v.a<=0.)return v;"
    "float rays=float(clamp(popWhole(values[0]),2,200));vec4 box=popBox(s);"
    "float cx=box.x+box.z*clamp(values[9]/100.,-1.,2.),cy=box.y+box.w*clamp(values[10]/100.,-1.,2.);"
    "float turn=mod(values[11],360.)/360.+values[12]*wavySeconds/360.;float start=turn-floor(turn);"
    "float scale=wavyScale>0.?wavyScale:1.;float twist=values[13]/360./(100.*scale);"
    "float fade=clamp(values[14]/100.,0.,1.);float reach=max(1.,length(box.zw)/2.);"
    "float dx=p.x-cx,dy=p.y-cy;float r=sqrt(dx*dx+dy*dy);float cover=.5;"
    "if(r>=2.){float t=(atan(dy,dx)/6.28318531+start+twist*r)*rays;float ray=t-floor(t);"
    "float aa=max(1e-4,rays/(6.28318531*max(r,.5))*.5);cover=1.-popSmooth((abs(ray-.25)-.25)/(2.*aa)+.5);}"
    "if(fade>0.)cover*=1.-popSmooth(r/(reach*max(.02,1.-fade+.02)));"
    "return popFill(v,cover,popColour(1),popColour(5),opacity);}";

// ---- Sparkles ------------------------------------------------------------------------
// Four-pointed glints scattered over the layer and twinkling, each in its own
// time: the sparkle round an idol, the shine on a title. One may sit in each
// cell of a grid laid over the box, so a pixel only asks the cells near it.
enum { SparkCount, SparkSize, SparkVariation, SparkColour, SparkTwinkle = SparkColour + 4, SparkGlow, SparkSeed, SparkAngle, SparkIntensity };
int sparkInert(const float *v, int32_t n) { return std::clamp(divided(v, n, SparkIntensity, 100, 100.), 0.f, 4.f) <= 0; }
int sparkVaries(const float *v, int32_t n) { return std::abs(value(v, n, SparkTwinkle, 1.5f)) > 1e-6f; }
int sparkProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float intensity = std::clamp(divided(v, n, SparkIntensity, 100, 100.), 0.f, 4.f);
  if (intensity <= 0.f) return 0;
  const Rect box = boxOf(context, width, height);
  if (box.w < 1 || box.h < 1) return 0;
  const int count = std::clamp(whole(v, n, SparkCount, 40), 1, 2000);
  const double scale = context->scale > 0 ? context->scale : 1.;
  const float size = float(std::max(1., std::clamp(double(value(v, n, SparkSize, 28)), 1., 1000.) * scale));
  const float variation = std::clamp(divided(v, n, SparkVariation, 60, 100.), 0.f, 1.f);
  const Colour colour = colourAt(v, n, SparkColour, {1, 1, 1, 1});
  const float twinkle = value(v, n, SparkTwinkle, 1.5f);
  const float glow = std::clamp(divided(v, n, SparkGlow, 50, 100.), 0.f, 2.f);
  const uint32_t seed = uint32_t(std::max(0, whole(v, n, SparkSeed, 1)));
  const double angle = double(value(v, n, SparkAngle, 0)) * kPi / 180.;
  const float cs = float(std::cos(angle)), sn = float(std::sin(angle));
  const int across = std::max(1, int(std::lround(std::sqrt(double(count) * box.w / box.h))));
  const int down = std::max(1, int(std::lround(double(count) / across)));
  const float cellW = box.w / float(across), cellH = box.h / float(down);
  const float seconds = float(context->seconds);
  const RasterPlace place = rasterPlace(context, width, height);
  struct Spark { float x, y, size, light; };
  // Every cell's sparkle worked out once, before the rows.
  auto make = [&](int i, int j) {
    Spark s;
    s.x = box.x + (float(i) + .15f + .7f * hashed(i, j, seed, 1)) * cellW;
    s.y = box.y + (float(j) + .15f + .7f * hashed(i, j, seed, 2)) * cellH;
    s.size = size * (1.f - variation * hashed(i, j, seed, 3));
    const float phase = hashed(i, j, seed, 4), rate = .6f + .8f * hashed(i, j, seed, 5);
    if (std::abs(twinkle) > 1e-6f) {
      const float wave = std::max(0.f, std::sin(kTau * (seconds * twinkle * rate + phase)));
      s.light = wave * wave;
    } else {
      s.light = .4f + .6f * phase;
    }
    return s;
  };
  std::vector<Spark> sparks(size_t(across) * size_t(down));
  for (int j = 0; j < down; ++j)
    for (int i = 0; i < across; ++i) sparks[size_t(j) * size_t(across) + size_t(i)] = make(i, j);
  auto sparkOf = [&](int i, int j) -> const Spark & { return sparks[size_t(j) * size_t(across) + size_t(i)]; };
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const float py = float(place.y + y) + .5f;
      const int j0 = std::max(0, int(std::floor((py - size - box.y) / cellH))),
                j1 = std::min(down - 1, int(std::floor((py + size - box.y) / cellH)));
      if (j0 > j1) continue;
      for (int32_t x = 0; x < width; ++x) {
        const float px = float(place.x + x) + .5f;
        const int i0 = std::max(0, int(std::floor((px - size - box.x) / cellW))),
                  i1 = std::min(across - 1, int(std::floor((px + size - box.x) / cellW)));
        float light = 0.f;
        for (int j = j0; j <= j1; ++j)
          for (int i = i0; i <= i1; ++i) {
            const Spark &s = sparkOf(i, j);
            if (s.light <= 0.f) continue;
            const float ox = px - s.x, oy = py - s.y;
            if (std::abs(ox) > s.size || std::abs(oy) > s.size) continue;
            // Turned by Angle, then the four rays along the axes.
            const float ax = std::abs(ox * cs + oy * sn), ay = std::abs(-ox * sn + oy * cs);
            const float thin = std::max(.7f, s.size * .035f);
            auto ray = [&](float along, float across) {
              const float t = std::max(0.f, 1.f - along / s.size);
              return t * t * t * std::exp(-(across * across) / (thin * thin));
            };
            const float d2 = (ox * ox + oy * oy) / (s.size * s.size);
            const float core = std::exp(-d2 / .006f);
            const float halo = glow * .45f * std::exp(-d2 / .05f);
            light += s.light * (ray(ax, ay) + ray(ay, ax) + core + halo);
          }
        if (light <= 0.f) continue;
        float *d = p + x * 4;
        light = std::min(1.f, light * intensity * colour.a);
        // Light added: premultiplied, so it shows over clear parts too.
        d[0] = std::min(1.f, d[0] + colour.r * light);
        d[1] = std::min(1.f, d[1] + colour.g * light);
        d[2] = std::min(1.f, d[2] + colour.b * light);
        d[3] = std::min(1.f, std::max({d[3], d[0], d[1], d[2]}));
      }
    }
  });
  return 0;
}
const char *sparkShader = POP_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float intensity=clamp(values[11]/100.,0.,4.);if(intensity<=0.)return v;"
    "vec4 box=popBox(s);if(box.z<1.||box.w<1.)return v;"
    "int count=clamp(popWhole(values[0]),1,2000);float scale=wavyScale>0.?wavyScale:1.;"
    "float size=max(1.,clamp(values[1],1.,1000.)*scale);float variation=clamp(values[2]/100.,0.,1.);"
    "vec4 colour=clamp(vec4(values[3],values[4],values[5],values[6]),0.,1.);float twinkle=values[7];"
    "float glow=clamp(values[8]/100.,0.,2.);uint seed=uint(max(0,popWhole(values[9])));"
    "float angle=values[10]*3.14159265358979/180.;float cs=cos(angle),sn=sin(angle);"
    "int across=max(1,popWhole(sqrt(float(count)*box.z/box.w)));int down=max(1,popWhole(float(count)/float(across)));"
    "float cellW=box.z/float(across),cellH=box.w/float(down);"
    "int j0=max(0,int(floor((p.y-size-box.y)/cellH))),j1=min(down-1,int(floor((p.y+size-box.y)/cellH)));"
    "int i0=max(0,int(floor((p.x-size-box.x)/cellW))),i1=min(across-1,int(floor((p.x+size-box.x)/cellW)));"
    "float light=0.;"
    "for(int j=j0;j<=j1;++j)for(int i=i0;i<=i1;++i){"
    "float sx=box.x+(float(i)+.15+.7*popHashed(i,j,seed,1u))*cellW,sy=box.y+(float(j)+.15+.7*popHashed(i,j,seed,2u))*cellH;"
    "float ssize=size*(1.-variation*popHashed(i,j,seed,3u));"
    "float phase=popHashed(i,j,seed,4u),rate=.6+.8*popHashed(i,j,seed,5u);float slight;"
    "if(abs(twinkle)>1e-6){float wave=max(0.,sin(popTurn(wavySeconds*twinkle*rate+phase)));slight=wave*wave;}"
    "else slight=.4+.6*phase;"
    "if(slight<=0.)continue;float ox=p.x-sx,oy=p.y-sy;if(abs(ox)>ssize||abs(oy)>ssize)continue;"
    "float ax=abs(ox*cs+oy*sn),ay=abs(-ox*sn+oy*cs);float thin=max(.7,ssize*.035);"
    "float t1=max(0.,1.-ax/ssize),t2=max(0.,1.-ay/ssize);"
    "float rays=t1*t1*t1*exp(-(ay*ay)/(thin*thin))+t2*t2*t2*exp(-(ax*ax)/(thin*thin));"
    "float d2=(ox*ox+oy*oy)/(ssize*ssize);"
    "light+=slight*(rays+exp(-d2/.006)+glow*.45*exp(-d2/.05));}"
    "if(light<=0.)return v.a>0.?v:vec4(0.);"
    "vec4 d=vec4(v.rgb*v.a,v.a);light=min(1.,light*intensity*colour.a);"
    "d.rgb=min(vec3(1.),d.rgb+colour.rgb*light);d.a=min(1.,max(max(d.a,d.r),max(d.g,d.b)));"
    "return popStraight(d);}";
const WfxParameter sparkParameters[] = {
    number("count", "Count", 1, 2000, 40, 1), number("size", "Size", 1, 1000, 28, .5f, " px"),
    number("variation", "Size Variation", 0, 100, 60, 1, " %"), color("color", "Colour", 1, 1, 1, 1),
    number("twinkle", "Twinkle", -20, 20, 1.5f, .05f, " /s"), number("glow", "Glow", 0, 200, 50, 1, " %"),
    number("seed", "Seed", 0, 100000, 1, 1), number("angle", "Angle", -3600, 3600, 0, 1, "\xc2\xb0"),
    number("intensity", "Intensity", 0, 400, 100, 1, " %")};
const char *const sparkHelp[] = {
    "About how many sparkles there are", "How long a sparkle's rays reach, in layer pixels", "How much smaller some are than others",
    "The sparkles' colour", "How many times a second each twinkles (0 holds them lit)", "The soft light round each one",
    "Another scatter of sparkles", "How the sparkles' rays are turned", "How bright they are"};

// ---- Light Leak ----------------------------------------------------------------------
// Warm light bleeding in, as through the side of an old camera: a few soft
// patches of colour drifting slowly across, screened over the layer.
enum { LeakIntensity, LeakA, LeakB = LeakA + 4, LeakSize = LeakB + 4, LeakSpeed, LeakSeed };
int leakInert(const float *v, int32_t n) { return std::clamp(divided(v, n, LeakIntensity, 70, 100.), 0.f, 2.f) <= 0; }
int leakVaries(const float *v, int32_t n) { return std::abs(value(v, n, LeakSpeed, .15f)) > 1e-6f; }
int leakProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float intensity = std::clamp(divided(v, n, LeakIntensity, 70, 100.), 0.f, 2.f);
  if (intensity <= 0.f) return 0;
  const Colour a = colourAt(v, n, LeakA, {1, .55f, .2f, 1}), b = colourAt(v, n, LeakB, {1, .25f, .55f, 1});
  const Rect box = boxOf(context, width, height);
  const float size = std::clamp(divided(v, n, LeakSize, 45, 100.), .02f, 3.f) * std::hypot(box.w, box.h) / 2;
  const double speed = value(v, n, LeakSpeed, .15f);
  const uint32_t seed = uint32_t(std::max(0, whole(v, n, LeakSeed, 1)));
  struct Patch { float x, y, radius, weight; Colour colour; };
  Patch patches[3];
  for (int k = 0; k < 3; ++k) {
    const double t = context->seconds * speed;
    const double along = t * (.7 + .25 * k) + hashed(k, 7, seed, 1), down = t * (.5 + .2 * k) + hashed(k, 7, seed, 2);
    patches[k].x = box.x + box.w * float(.5 + .55 * std::sin(kTau * along));
    patches[k].y = box.y + box.h * float(.5 + .45 * std::cos(kTau * down));
    patches[k].radius = size * (.6f + .5f * hashed(k, 7, seed, 3));
    patches[k].weight = .55f + .45f * float(std::sin(kTau * (t * .9 + hashed(k, 7, seed, 4))) * .5 + .5);
    patches[k].colour = k == 1 ? b : a;
  }
  const RasterPlace place = rasterPlace(context, width, height);
  // A round patch's falloff is its falloff across times its falloff down, so
  // each column's share is worked out once for all rows, and a pixel is three
  // multiplies a patch instead of an exp.
  std::vector<float> across(size_t(width) * 3);
  for (int k = 0; k < 3; ++k)
    for (int32_t x = 0; x < width; ++x) {
      const float dx = (float(place.x + x) + .5f - patches[k].x) / patches[k].radius;
      across[size_t(k) * size_t(width) + size_t(x)] = std::exp(-dx * dx * 1.6f);
    }
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const float py = float(place.y + y) + .5f;
      float down[3][3];
      for (int k = 0; k < 3; ++k) {
        const float dy = (py - patches[k].y) / patches[k].radius;
        const float w = patches[k].weight * std::exp(-dy * dy * 1.6f) * patches[k].colour.a;
        down[k][0] = patches[k].colour.r * w; down[k][1] = patches[k].colour.g * w; down[k][2] = patches[k].colour.b * w;
      }
      for (int32_t x = 0; x < width; ++x) {
        float light[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) {
          const float a = across[size_t(k) * size_t(width) + size_t(x)];
          light[0] += down[k][0] * a; light[1] += down[k][1] * a; light[2] += down[k][2] * a;
        }
        float *d = p + x * 4;
        for (int c = 0; c < 3; ++c) {
          const float l = std::min(1.f, light[c] * intensity);
          d[c] = d[c] + l * (1.f - d[c]); // screened over what is there
        }
        d[3] = std::min(1.f, std::max({d[3], d[0], d[1], d[2]}));
      }
    }
  });
  return 0;
}
const char *leakShader = POP_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float intensity=clamp(values[0]/100.,0.,2.);if(intensity<=0.)return v;"
    "vec4 ca=clamp(vec4(values[1],values[2],values[3],values[4]),0.,1.),cb=clamp(vec4(values[5],values[6],values[7],values[8]),0.,1.);"
    "vec4 box=popBox(s);float size=clamp(values[9]/100.,.02,3.)*length(box.zw)/2.;"
    "float speed=values[10];uint seed=uint(max(0,popWhole(values[11])));float t=wavySeconds*speed;"
    "vec3 light=vec3(0.);"
    "for(int k=0;k<3;++k){"
    "float along=t*(.7+.25*float(k))+popHashed(k,7,seed,1u),down=t*(.5+.2*float(k))+popHashed(k,7,seed,2u);"
    "float x=box.x+box.z*(.5+.55*sin(popTurn(along))),y=box.y+box.w*(.5+.45*cos(popTurn(down)));"
    "float radius=size*(.6+.5*popHashed(k,7,seed,3u));"
    "float weight=.55+.45*(sin(popTurn(t*.9+popHashed(k,7,seed,4u)))*.5+.5);"
    "vec4 c=k==1?cb:ca;float dx=(p.x-x)/radius,dy=(p.y-y)/radius;"
    "light+=c.rgb*(weight*exp(-dy*dy*1.6)*c.a)*exp(-dx*dx*1.6);}"
    "vec4 d=vec4(v.rgb*v.a,v.a);vec3 l=min(vec3(1.),light*intensity);d.rgb=d.rgb+l*(1.-d.rgb);"
    "d.a=min(1.,max(max(d.a,d.r),max(d.g,d.b)));return popStraight(d);}";
const WfxParameter leakParameters[] = {
    number("intensity", "Intensity", 0, 200, 70, 1, " %"), color("colorA", "Colour A", 1, .55f, .2f, 1),
    color("colorB", "Colour B", 1, .25f, .55f, 1), number("size", "Size", 2, 300, 45, 1, " %"),
    number("speed", "Drift", -5, 5, .15f, .01f, " /s"), number("seed", "Seed", 0, 100000, 1, 1)};
const char *const leakHelp[] = {"How strong the light is", "The warm light's colour", "The second patch's colour",
                                "How big the patches are, against the layer", "How fast they drift (0 holds them still)",
                                "Another arrangement of the patches"};

// ---- TV Screen ------------------------------------------------------------------------
// The whole picture shown on an old television: inside a rounded screen set in
// a coloured frame, bulging a little toward the viewer, darker toward its rim.
enum { TvBorder, TvRoundness, TvCurve, TvShade, TvShadeWidth, TvFrame };
int tvInert(const float *, int32_t) { return 0; }
int tvProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  if (width <= 0 || height <= 0) return 0;
  const Rect box = boxOf(context, width, height);
  if (box.w < 2 || box.h < 2) return 0;
  const float shorter = std::min(box.w, box.h);
  const float border = std::clamp(divided(v, n, TvBorder, 7, 100.), 0.f, .45f) * shorter;
  const float hx = std::max(1.f, box.w / 2 - border), hy = std::max(1.f, box.h / 2 - border);
  const float corner = std::clamp(divided(v, n, TvRoundness, 35, 100.), 0.f, 1.f) * std::min(hx, hy);
  const float curve = std::clamp(divided(v, n, TvCurve, 25, 100.), 0.f, 1.f) * .3f;
  const float shade = std::clamp(divided(v, n, TvShade, 40, 100.), 0.f, 1.f);
  const float shadeWidth = std::max(1.f, std::clamp(divided(v, n, TvShadeWidth, 10, 100.), 0.f, 1.f) * std::min(hx, hy));
  const Colour frame = colourAt(v, n, TvFrame, {.91f, .36f, .2f, 1});
  const float cx = box.x + box.w / 2, cy = box.y + box.h / 2;
  const size_t line = size_t(width) * 4;
  Scratch kept(line * size_t(height));
  float *const source = kept.data();
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) std::memcpy(source + size_t(y) * line, row(pixels, stride, y), line * sizeof(float));
  });
  auto sample = [&](float sx, float sy, float *out) {
    sx -= .5f; sy -= .5f;
    const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
    const float fx = sx - float(x0), fy = sy - float(y0);
    const float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    if (x0 >= 0 && y0 >= 0 && x0 + 1 < width && y0 + 1 < height) {
      const float *a = source + size_t(y0) * line + size_t(x0) * 4, *b = a + line;
      for (int c = 0; c < 4; ++c) out[c] = a[c] * w[0] + a[4 + c] * w[1] + b[c] * w[2] + b[4 + c] * w[3];
      return;
    }
    const int xs[4] = {x0, x0 + 1, x0, x0 + 1}, ys[4] = {y0, y0, y0 + 1, y0 + 1};
    out[0] = out[1] = out[2] = out[3] = 0;
    for (int k = 0; k < 4; ++k) {
      if (xs[k] < 0 || ys[k] < 0 || xs[k] >= width || ys[k] >= height) continue;
      const float *t = source + size_t(ys[k]) * line + size_t(xs[k]) * 4;
      for (int c = 0; c < 4; ++c) out[c] += t[c] * w[k];
    }
  };
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *d = row(pixels, stride, y);
      const float py = float(y) + .5f;
      for (int32_t x = 0; x < width; ++x) {
        float *o = d + size_t(x) * 4;
        const float px = float(x) + .5f;
        // Outside the layer's box there is no set.
        if (px < box.x || py < box.y || px > box.x + box.w || py > box.y + box.h) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
        const float qx = px - cx, qy = py - cy;
        // How far outside the rounded screen (negative inside). Away from the
        // corners that is just the nearer side.
        const float ex = std::abs(qx) - (hx - corner), ey = std::abs(qy) - (hy - corner);
        const float outside = (ex <= 0.f || ey <= 0.f)
                                  ? std::max(ex, ey) - corner
                                  : std::sqrt(ex * ex + ey * ey) - corner;
        const float cover = std::clamp(.5f - outside, 0.f, 1.f);
        float screen[4] = {0, 0, 0, 0};
        if (cover > 0.f) {
          // The whole picture fills the screen, magnified a little in the
          // middle as the glass bulges.
          const float u = qx / hx, w = qy / hy;
          const float bulge = 1.f - curve * (1.f - std::min(1.f, (u * u + w * w) * .5f));
          sample(cx + u * bulge * box.w / 2, cy + w * bulge * box.h / 2, screen);
          const float dark = shade * (1.f - smooth01(-outside / shadeWidth));
          for (int c = 0; c < 3; ++c) screen[c] *= 1.f - dark;
        }
        for (int c = 0; c < 3; ++c) o[c] = screen[c] * cover + (c == 0 ? frame.r : c == 1 ? frame.g : frame.b) * frame.a * (1.f - cover);
        o[3] = screen[3] * cover + frame.a * (1.f - cover);
      }
    }
  });
  return 0;
}
const char *tvShader = POP_GLSL
    "vec4 tvTexel(int x,int y){vec4 t=wavyPixel(ivec2(x,y));return vec4(t.rgb*t.a,t.a);}"
    "vec4 tvSample(float sx,float sy){sx-=.5;sy-=.5;int x0=int(floor(sx)),y0=int(floor(sy));"
    "float fx=sx-float(x0),fy=sy-float(y0);"
    "return tvTexel(x0,y0)*((1.-fx)*(1.-fy))+tvTexel(x0+1,y0)*(fx*(1.-fy))+tvTexel(x0,y0+1)*((1.-fx)*fy)+tvTexel(x0+1,y0+1)*(fx*fy);}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "vec4 box=popBox(s);if(box.z<2.||box.w<2.)return v;"
    "if(p.x<box.x||p.y<box.y||p.x>box.x+box.z||p.y>box.y+box.w)return vec4(0.);"
    "float shorter=min(box.z,box.w);float border=clamp(values[0]/100.,0.,.45)*shorter;"
    "float hx=max(1.,box.z/2.-border),hy=max(1.,box.w/2.-border);"
    "float corner=clamp(values[1]/100.,0.,1.)*min(hx,hy);float curve=clamp(values[2]/100.,0.,1.)*.3;"
    "float shade=clamp(values[3]/100.,0.,1.);float shadeWidth=max(1.,clamp(values[4]/100.,0.,1.)*min(hx,hy));"
    "vec4 frame=clamp(vec4(values[5],values[6],values[7],values[8]),0.,1.);"
    "float cx=box.x+box.z/2.,cy=box.y+box.w/2.;float qx=p.x-cx,qy=p.y-cy;"
    "float ex=abs(qx)-(hx-corner),ey=abs(qy)-(hy-corner);"
    "float outside=(ex<=0.||ey<=0.)?max(ex,ey)-corner:sqrt(ex*ex+ey*ey)-corner;"
    "float cover=clamp(.5-outside,0.,1.);vec4 screen=vec4(0.);"
    "if(cover>0.){float u=qx/hx,w=qy/hy;float bulge=1.-curve*(1.-min(1.,(u*u+w*w)*.5));"
    "screen=tvSample(cx+u*bulge*box.z/2.,cy+w*bulge*box.w/2.);"
    "float dark=shade*(1.-popSmooth(-outside/shadeWidth));screen.rgb*=1.-dark;}"
    "vec4 o=vec4(screen.rgb*cover+frame.rgb*frame.a*(1.-cover),screen.a*cover+frame.a*(1.-cover));"
    "return popStraight(o);}";
const WfxParameter tvParameters[] = {
    number("border", "Border", 0, 45, 7, .5f, " %"), number("roundness", "Roundness", 0, 100, 35, 1, " %"),
    number("curve", "Curve", 0, 100, 25, 1, " %"), number("shade", "Edge Shade", 0, 100, 40, 1, " %"),
    number("shadeWidth", "Shade Width", 0, 100, 10, 1, " %"), color("frame", "Frame Colour", .91f, .36f, .2f, 1)};
const char *const tvHelp[] = {"How wide the frame round the screen is", "How round the screen's corners are",
                              "How much the glass bulges toward the viewer", "How much darker the screen is toward its rim",
                              "How far in from the rim the darkening reaches", "The frame's colour (clear for none)"};

#define COUNT(parameters) int32_t(sizeof(parameters) / sizeof(parameters[0])), parameters
const WfxEffect effects[] = {
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Pattern", "Pattern", "Generate", 4u, uint32_t(WFX_PER_PIXEL) | uint32_t(WFX_MOVES_ALONE),
     COUNT(patternParameters), patternProcess, patternInert, patternVaries, nullptr, nullptr, nullptr, nullptr, patternShader,
     nullptr, nullptr, nullptr, patternHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Sunburst", "Sunburst", "Generate", 4u, uint32_t(WFX_PER_PIXEL) | uint32_t(WFX_MOVES_ALONE),
     COUNT(sunParameters), sunProcess, sunInert, sunVaries, nullptr, nullptr, nullptr, nullptr, sunShader,
     nullptr, nullptr, nullptr, sunHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Sparkles", "Sparkles", "Music Video", 4u,
     uint32_t(WFX_PER_PIXEL) | uint32_t(WFX_MOVES_ALONE) | uint32_t(WFX_PREMULTIPLIED),
     COUNT(sparkParameters), sparkProcess, sparkInert, sparkVaries, nullptr, nullptr, nullptr, nullptr, sparkShader,
     nullptr, nullptr, nullptr, sparkHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Light Leak", "Light Leak", "Music Video", 4u,
     uint32_t(WFX_PER_PIXEL) | uint32_t(WFX_MOVES_ALONE) | uint32_t(WFX_PREMULTIPLIED),
     COUNT(leakParameters), leakProcess, leakInert, leakVaries, nullptr, nullptr, nullptr, nullptr, leakShader,
     nullptr, nullptr, nullptr, leakHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "TV Screen", "TV Screen", "Music Video", 4u,
     uint32_t(WFX_PREMULTIPLIED) | uint32_t(WFX_WHOLE_LAYER),
     COUNT(tvParameters), tvProcess, tvInert, nullptr, nullptr, nullptr, nullptr, nullptr, tvShader,
     nullptr, nullptr, nullptr, tvHelp, nullptr}};
}  // namespace

WFX_EXPORT int32_t wfxEffectCount(void) { return int32_t(sizeof(effects) / sizeof(effects[0])); }
WFX_EXPORT const WfxEffect *wfxEffectAt(int32_t index) {
  return index >= 0 && index < wfxEffectCount() ? &effects[index] : nullptr;
}
