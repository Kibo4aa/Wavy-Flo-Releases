// Wavy-Transition.wfx: Linear Wipe, Radial Wipe, Iris Wipe, Venetian Blinds and
// Block Dissolve. Completion 0 leaves the layer whole and 100 wipes it away;
// every slider can be keyframed. Each works pixel by pixel from where the pixel
// lies in the layer's raster (WFX_PER_PIXEL), so the host may run them a strip
// at a time.
#include "wavy_effect.h"
#include "pack_common.h"
#include <algorithm>
#include <cmath>

using namespace wfxpack;

namespace {
float *row(void *pixels, int32_t stride, int32_t y) {
  return reinterpret_cast<float *>(static_cast<unsigned char *>(pixels) + size_t(y) * size_t(stride));
}
struct Box { double x, y, w, h; };
Box boxOf(const WfxContext *context, int32_t width, int32_t height) {
  if (context->boxWidth > 0 && context->boxHeight > 0) return {context->boxX, context->boxY, context->boxWidth, context->boxHeight};
  const RasterPlace place = rasterPlace(context, width, height);
  return {0, 0, double(place.width), double(place.height)};
}
float smooth(float t) { return t * t * (3.f - 2.f * t); }

// The same on the card. Only coverage changes, so the colour passes through.
// atan2(+0, -0) is a half turn on the CPU; the card's atan is not defined there.
#define TRANSITION_GLSL \
  "vec4 trBox(vec2 s){vec4 b=wavyBox;if(!(b.z>0.&&b.w>0.))b=vec4(0.,0.,s);return b;}" \
  "int trWhole(float v){return int(v<0.?-floor(-v+.5):floor(v+.5));}" \
  "float trSmooth(float t){return t*t*(3.-2.*t);}" \
  "float trAtan(float y,float x){return (y==0.&&x==0.)?3.14159265:atan(y,x);}" \
  "const float trPi=3.14159265358979;" \
  /* floor(q / d) as the CPU's exactly rounded division gives it: the card's */ \
  /* division is a reciprocal and a multiply, and on a block's edge it lands */ \
  /* a hair short and puts the pixel in the block before. */ \
  "int trFloorDiv(float q,float d){int k=int(floor(q/d));if(float(k+1)*d<=q)++k;else if(float(k)*d>q)--k;return k;}"

// ---- Linear Wipe ---------------------------------------------------------------
enum { LCompletion, LAngle, LFeather };
int linearInert(const float *v, int32_t n) { return std::clamp(double(value(v, n, LCompletion, 0)), 0., 100.) / 100. <= 0; }
int linearProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float completion = float(std::clamp(double(value(v, n, LCompletion, 0)), 0., 100.) / 100.);
  if (completion <= 0.f) return 0;
  const double angle = double(value(v, n, LAngle, 90)) * 3.14159265358979323846 / 180.;
  const float feather = std::max(1.f, float(std::max(0., double(value(v, n, LFeather, 0)))));
  const Box box = boxOf(context, width, height);
  const float dx = float(std::cos(angle)), dy = float(std::sin(angle));
  // How far along the direction the layer's box reaches, corner to corner.
  float low = 1e30f, high = -1e30f;
  const double corners[4][2] = {{box.x, box.y}, {box.x + box.w, box.y}, {box.x, box.y + box.h}, {box.x + box.w, box.y + box.h}};
  for (const auto &corner : corners) {
    const float along = float(corner[0]) * dx + float(corner[1]) * dy;
    low = std::min(low, along); high = std::max(high, along);
  }
  const float edge = low - feather + (high - low + feather) * completion;
  const RasterPlace place = rasterPlace(context, width, height);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const float along = (float(place.x + x) + .5f) * dx + (float(place.y + y) + .5f) * dy;
        const float keep = std::clamp((along - edge) / feather, 0.f, 1.f);
        p[x * 4 + 3] *= keep * keep * (3.f - 2.f * keep);
      }
    }
  });
  return 0;
}
const char *linearShader = TRANSITION_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float completion=clamp(values[0],0.,100.)/100.;if(completion<=0.||v.a<=0.)return v;"
    "float angle=mod(values[1],360.)*trPi/180.;float feather=max(1.,max(0.,values[2]));vec4 b=trBox(s);"
    "float dx=cos(angle),dy=sin(angle);"
    "float a1=b.x*dx+b.y*dy,a2=(b.x+b.z)*dx+b.y*dy,a3=b.x*dx+(b.y+b.w)*dy,a4=(b.x+b.z)*dx+(b.y+b.w)*dy;"
    "float low=min(min(a1,a2),min(a3,a4)),high=max(max(a1,a2),max(a3,a4));"
    "float edge=low-feather+(high-low+feather)*completion;"
    "float keep=clamp((p.x*dx+p.y*dy-edge)/feather,0.,1.);return vec4(v.rgb,v.a*trSmooth(keep));}";
const WfxParameter linearParameters[] = {
    number("completion", "Completion", 0, 100, 0, .5f, " %"), number("angle", "Wipe Angle", -3600, 3600, 90, 1, "\xc2\xb0"),
    number("feather", "Feather", 0, 10000, 0, 1, " px")};

// ---- Radial Wipe ---------------------------------------------------------------
// Sweeps round a point, like the hand of a clock: from the start angle (0 =
// straight up) clockwise, counter-clockwise, or both ways at once.
enum { RCompletion, RStart, RFeather, RDirection, RCenterX, RCenterY };
int radialInert(const float *v, int32_t n) { return std::clamp(double(value(v, n, RCompletion, 0)), 0., 100.) / 100. <= 0; }
int radialProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float completion = float(std::clamp(double(value(v, n, RCompletion, 0)), 0., 100.) / 100.);
  if (completion <= 0.f) return 0;
  const int direction = std::clamp(int(std::lround(value(v, n, RDirection, 0))), 0, 2);
  const float start = value(v, n, RStart, 0);
  const float feather = std::max(1.f / 360.f, float(std::max(0., double(value(v, n, RFeather, 0))) / 360.));
  const Box box = boxOf(context, width, height);
  const float cx = float(box.x + box.w * std::clamp(double(value(v, n, RCenterX, 50)), -100., 200.) / 100.);
  const float cy = float(box.y + box.h * std::clamp(double(value(v, n, RCenterY, 50)), -100., 200.) / 100.);
  const float edge = -feather + (1.f + feather) * completion;
  const RasterPlace place = rasterPlace(context, width, height);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const float ry = (float(place.y + y) + .5f) - cy;
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const float rx = (float(place.x + x) + .5f) - cx;
        float turns = (std::atan2(rx, -ry) * 57.29577951f - start) / 360.f; // clockwise from the start
        turns -= std::floor(turns);
        const float progress = direction == 0 ? turns : direction == 1 ? 1.f - turns : 2.f * std::min(turns, 1.f - turns);
        const float keep = std::clamp((progress - edge) / feather, 0.f, 1.f);
        p[x * 4 + 3] *= keep * keep * (3.f - 2.f * keep);
      }
    }
  });
  return 0;
}
const char *radialShader = TRANSITION_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float completion=clamp(values[0],0.,100.)/100.;if(completion<=0.||v.a<=0.)return v;"
    "int direction=clamp(trWhole(values[3]),0,2);float start=values[1];"
    "float feather=max(1./360.,max(0.,values[2])/360.);vec4 b=trBox(s);"
    "float cx=b.x+b.z*clamp(values[4],-100.,200.)/100.,cy=b.y+b.w*clamp(values[5],-100.,200.)/100.;"
    "float edge=-feather+(1.+feather)*completion;float rx=p.x-cx,ry=p.y-cy;"
    "float turns=(trAtan(rx,-ry)*57.29577951-start)/360.;turns-=floor(turns);"
    "float progress=direction==0?turns:direction==1?1.-turns:2.*min(turns,1.-turns);"
    "float keep=clamp((progress-edge)/feather,0.,1.);return vec4(v.rgb,v.a*trSmooth(keep));}";
const WfxParameter radialParameters[] = {
    number("completion", "Completion", 0, 100, 0, .5f, " %"), number("start", "Start Angle", -3600, 3600, 0, 1, "\xc2\xb0"),
    number("feather", "Feather", 0, 360, 0, 1, "\xc2\xb0"),
    choice("direction", "Wipe", "Clockwise\nCounter-clockwise\nBoth", 3, 0),
    number("centerX", "Center X", -100, 200, 50, 1, " %"), number("centerY", "Center Y", -100, 200, 50, 1, " %")};

// ---- Iris Wipe -------------------------------------------------------------------
// A shape opens from a point and takes the layer away inside it -- or, Closing,
// the layer is kept only inside a shape that shrinks to nothing: the iris of an
// old film camera, a diamond, a square or a star.
enum { ICompletion, IShape, ICenterX, ICenterY, IFeather, IClosing, IRotation };
int irisInert(const float *v, int32_t n) { return std::clamp(double(value(v, n, ICompletion, 0)), 0., 100.) / 100. <= 0; }
// How far a point lies from the middle in the shape's own measure: the shape
// of every radius is the same shape.
float irisMeasure(int shape, float dx, float dy) {
  switch (shape) {
    case 1: return std::abs(dx) + std::abs(dy);                     // diamond
    case 2: return std::max(std::abs(dx), std::abs(dy));            // square
    case 3: {                                                       // a five-pointed star
      const float r = std::sqrt(dx * dx + dy * dy);
      if (r <= 0.f) return 0.f;
      const float a = std::atan2(dx, -dy);
      const float sector = 6.28318531f / 5.f;
      float t = std::fmod(a + 6.28318531f, sector) / sector;      // 0..1 across one point
      t = std::abs(t - .5f) * 2.f;                                 // 1 at a point, 0 between
      return r / (.55f + .45f * t);
    }
    default: return std::sqrt(dx * dx + dy * dy);                  // circle
  }
}
int irisProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float completion = float(std::clamp(double(value(v, n, ICompletion, 0)), 0., 100.) / 100.);
  if (completion <= 0.f) return 0;
  const int shape = std::clamp(int(std::lround(value(v, n, IShape, 0))), 0, 3);
  const Box box = boxOf(context, width, height);
  const float cx = float(box.x + box.w * std::clamp(double(value(v, n, ICenterX, 50)), -100., 200.) / 100.);
  const float cy = float(box.y + box.h * std::clamp(double(value(v, n, ICenterY, 50)), -100., 200.) / 100.);
  const float feather = std::max(1.f, float(std::max(0., double(value(v, n, IFeather, 0)) * context->scale)));
  const bool closing = value(v, n, IClosing, 0) > .5f;
  const double turn = double(value(v, n, IRotation, 0)) * 3.14159265358979323846 / 180.;
  const float cosT = float(std::cos(turn)), sinT = float(std::sin(turn));
  // Large enough at 100% to have passed every corner of the layer's box.
  float reach = 0.f;
  const double corners[4][2] = {{box.x, box.y}, {box.x + box.w, box.y}, {box.x, box.y + box.h}, {box.x + box.w, box.y + box.h}};
  for (const auto &corner : corners) {
    const float dx = float(corner[0]) - cx, dy = float(corner[1]) - cy;
    // A star's measure varies round it; the plain distance over its narrowest
    // makes sure the hollows have passed the corners too.
    reach = std::max(reach, shape == 3 ? std::sqrt(dx * dx + dy * dy) / .55f
                                       : irisMeasure(shape, dx * cosT + dy * sinT, -dx * sinT + dy * cosT));
  }
  const float radius = closing ? (1.f - completion) * (reach + feather) : completion * (reach + feather);
  const RasterPlace place = rasterPlace(context, width, height);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const float dy0 = float(place.y + y) + .5f - cy;
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const float dx0 = float(place.x + x) + .5f - cx;
        const float d = irisMeasure(shape, dx0 * cosT + dy0 * sinT, -dx0 * sinT + dy0 * cosT);
        // Opening: kept outside the shape; closing: kept inside it.
        const float keep = closing ? std::clamp((radius - d) / feather, 0.f, 1.f) : std::clamp((d - (radius - feather)) / feather, 0.f, 1.f);
        p[x * 4 + 3] *= smooth(keep);
      }
    }
  });
  return 0;
}
const char *irisShader = TRANSITION_GLSL
    "float irMeasure(int shape,float dx,float dy){"
    "if(shape==1)return abs(dx)+abs(dy);if(shape==2)return max(abs(dx),abs(dy));"
    "if(shape==3){float r=sqrt(dx*dx+dy*dy);if(r<=0.)return 0.;float a=trAtan(dx,-dy);float sector=6.28318531/5.;"
    "float q=a+6.28318531;float t=(q-sector*floor(q/sector))/sector;t=abs(t-.5)*2.;return r/(.55+.45*t);}"
    "return sqrt(dx*dx+dy*dy);}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float completion=clamp(values[0],0.,100.)/100.;if(completion<=0.||v.a<=0.)return v;"
    "int shape=clamp(trWhole(values[1]),0,3);vec4 b=trBox(s);"
    "float cx=b.x+b.z*clamp(values[2],-100.,200.)/100.,cy=b.y+b.w*clamp(values[3],-100.,200.)/100.;"
    "float feather=max(1.,max(0.,values[4]*wavyScale));bool closing=values[5]>.5;"
    "float turn=mod(values[6],360.)*trPi/180.;float cosT=cos(turn),sinT=sin(turn);float reach=0.;"
    "vec2 corners[4]=vec2[4](b.xy,vec2(b.x+b.z,b.y),vec2(b.x,b.y+b.w),b.xy+b.zw);"
    "for(int i=0;i<4;++i){float dx=corners[i].x-cx,dy=corners[i].y-cy;"
    "reach=max(reach,shape==3?sqrt(dx*dx+dy*dy)/.55:irMeasure(shape,dx*cosT+dy*sinT,-dx*sinT+dy*cosT));}"
    "float radius=closing?(1.-completion)*(reach+feather):completion*(reach+feather);"
    "float dx0=p.x-cx,dy0=p.y-cy;float d=irMeasure(shape,dx0*cosT+dy0*sinT,-dx0*sinT+dy0*cosT);"
    "float keep=closing?clamp((radius-d)/feather,0.,1.):clamp((d-(radius-feather))/feather,0.,1.);"
    "return vec4(v.rgb,v.a*trSmooth(keep));}";
const WfxParameter irisParameters[] = {
    number("completion", "Completion", 0, 100, 0, .5f, " %"), choice("shape", "Shape", "Circle\nDiamond\nSquare\nStar", 4, 0),
    number("centerX", "Center X", -100, 200, 50, 1, " %"), number("centerY", "Center Y", -100, 200, 50, 1, " %"),
    number("feather", "Feather", 0, 2000, 0, 1, " px"), toggle("closing", "Closing", false),
    number("rotation", "Rotation", -3600, 3600, 0, 1, "\xc2\xb0")};
const char *const irisHelp[] = {"0 leaves the layer whole, 100 takes it all away", "The shape that opens (or closes)",
                                "Where the shape opens from, across the layer", "Where the shape opens from, down the layer",
                                "How soft the shape's edge is", "Keep the layer inside a shape that shrinks, instead of opening a hole",
                                "How the shape is turned"};

// ---- Venetian Blinds --------------------------------------------------------------
// The layer cut into strips that each close up together, like a blind's slats.
enum { BCompletion, BAngle, BWidth, BFeather };
int blindsInert(const float *v, int32_t n) { return std::clamp(double(value(v, n, BCompletion, 0)), 0., 100.) / 100. <= 0; }
int blindsProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float completion = float(std::clamp(double(value(v, n, BCompletion, 0)), 0., 100.) / 100.);
  if (completion <= 0.f) return 0;
  const double angle = double(value(v, n, BAngle, 0)) * 3.14159265358979323846 / 180.;
  const float nx = float(-std::sin(angle)), ny = float(std::cos(angle)); // across the slats
  const float slat = float(std::max(2., std::clamp(double(value(v, n, BWidth, 40)), 1., 4000.) * context->scale));
  const float feather = std::clamp(float(std::max(0., double(value(v, n, BFeather, 0)) * context->scale)), 1.f, slat);
  const Box box = boxOf(context, width, height);
  const float edge = completion * (slat + feather) - feather;
  const RasterPlace place = rasterPlace(context, width, height);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const float py = float(place.y + y) + .5f - float(box.y);
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const float px = float(place.x + x) + .5f - float(box.x);
        float across = std::fmod(px * nx + py * ny, slat);
        if (across < 0.f) across += slat;
        p[x * 4 + 3] *= smooth(std::clamp((across - edge) / feather, 0.f, 1.f));
      }
    }
  });
  return 0;
}
const char *blindsShader = TRANSITION_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float completion=clamp(values[0],0.,100.)/100.;if(completion<=0.||v.a<=0.)return v;"
    "float angle=mod(values[1],360.)*trPi/180.;float nx=-sin(angle),ny=cos(angle);"
    "float slat=max(2.,clamp(values[2],1.,4000.)*wavyScale);float feather=clamp(max(0.,values[3]*wavyScale),1.,slat);"
    "vec4 b=trBox(s);float edge=completion*(slat+feather)-feather;"
    "float q=(p.x-b.x)*nx+(p.y-b.y)*ny;float across=q-slat*float(trFloorDiv(q,slat));"
    "return vec4(v.rgb,v.a*trSmooth(clamp((across-edge)/feather,0.,1.)));}";
const WfxParameter blindsParameters[] = {
    number("completion", "Completion", 0, 100, 0, .5f, " %"), number("angle", "Direction", -3600, 3600, 0, 1, "\xc2\xb0"),
    number("width", "Width", 1, 4000, 40, 1, " px"), number("feather", "Feather", 0, 4000, 0, 1, " px")};
const char *const blindsHelp[] = {"0 leaves the layer whole, 100 takes it all away", "0 gives slats across, 90 up and down",
                                  "How wide one slat is, in layer pixels", "How soft each slat's edge is"};

// ---- Block Dissolve ----------------------------------------------------------------
// The layer goes away in blocks, each at its own moment: a digital break-up.
enum { DCompletion, DWidth, DHeight, DSoftness, DSeed };
int dissolveInert(const float *v, int32_t n) { return std::clamp(double(value(v, n, DCompletion, 0)), 0., 100.) / 100. <= 0; }
int dissolveProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float completion = float(std::clamp(double(value(v, n, DCompletion, 0)), 0., 100.) / 100.);
  if (completion <= 0.f) return 0;
  const float bw = float(std::max(1., std::clamp(double(value(v, n, DWidth, 16)), 1., 4000.) * context->scale));
  const float bh = float(std::max(1., std::clamp(double(value(v, n, DHeight, 16)), 1., 4000.) * context->scale));
  const float soft = std::clamp(float(double(value(v, n, DSoftness, 10)) / 100.), .001f, 1.f);
  const uint32_t seed = uint32_t(int(value(v, n, DSeed, 1))) * 2654435761u;
  const Box box = boxOf(context, width, height);
  const float threshold = completion * (1.f + soft) - soft;
  const RasterPlace place = rasterPlace(context, width, height);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const int by = int(std::floor((float(place.y + y) + .5f - float(box.y)) / bh));
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const int bx = int(std::floor((float(place.x + x) + .5f - float(box.x)) / bw));
        uint32_t h = seed ^ (uint32_t(bx) * 374761393u) ^ (uint32_t(by) * 668265263u);
        h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
        const float moment = float(h & 0xffffff) / 16777216.f; // when this block goes, 0..1
        p[x * 4 + 3] *= std::clamp((moment - threshold) / soft, 0.f, 1.f);
      }
    }
  });
  return 0;
}
const char *dissolveShader = TRANSITION_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float completion=clamp(values[0],0.,100.)/100.;if(completion<=0.||v.a<=0.)return v;"
    "float bw=max(1.,clamp(values[1],1.,4000.)*wavyScale),bh=max(1.,clamp(values[2],1.,4000.)*wavyScale);"
    "float soft=clamp(values[3]/100.,.001,1.);uint seed=uint(int(values[4]))*2654435761u;vec4 b=trBox(s);"
    "float threshold=completion*(1.+soft)-soft;"
    "int by=trFloorDiv(p.y-b.y,bh),bx=trFloorDiv(p.x-b.x,bw);"
    "uint h=seed^(uint(bx)*374761393u)^(uint(by)*668265263u);h=(h^(h>>13u))*1274126177u;h^=h>>16u;"
    "float moment=float(h&0xffffffu)/16777216.;return vec4(v.rgb,v.a*clamp((moment-threshold)/soft,0.,1.));}";
const WfxParameter dissolveParameters[] = {
    number("completion", "Completion", 0, 100, 0, .5f, " %"), number("blockWidth", "Block Width", 1, 4000, 16, 1, " px"),
    number("blockHeight", "Block Height", 1, 4000, 16, 1, " px"), number("softness", "Softness", 0, 100, 10, 1, " %"),
    number("seed", "Seed", 0, 9999, 1, 1)};
const char *const dissolveHelp[] = {"0 leaves the layer whole, 100 takes it all away", "How wide one block is, in layer pixels",
                                    "How tall one block is, in layer pixels", "How gradually each block fades rather than blinks out",
                                    "Another seed, another order the blocks go in"};

#define COUNT(parameters) int32_t(sizeof(parameters) / sizeof(parameters[0])), parameters
const WfxEffect effects[] = {
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Linear Wipe", "Linear Wipe", "Transition", 4u, uint32_t(WFX_PER_PIXEL), COUNT(linearParameters),
     linearProcess, linearInert, nullptr, nullptr, nullptr, nullptr, nullptr, linearShader},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Radial Wipe", "Radial Wipe", "Transition", 4u, uint32_t(WFX_PER_PIXEL), COUNT(radialParameters),
     radialProcess, radialInert, nullptr, nullptr, nullptr, nullptr, nullptr, radialShader},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Iris Wipe", "Iris Wipe", "Transition", 4u, uint32_t(WFX_PER_PIXEL), COUNT(irisParameters),
     irisProcess, irisInert, nullptr, nullptr, nullptr, nullptr, nullptr, irisShader, nullptr, nullptr, nullptr, irisHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Venetian Blinds", "Venetian Blinds", "Transition", 4u, uint32_t(WFX_PER_PIXEL),
     COUNT(blindsParameters), blindsProcess, blindsInert, nullptr, nullptr, nullptr, nullptr, nullptr, blindsShader,
     nullptr, nullptr, nullptr, blindsHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Block Dissolve", "Block Dissolve", "Transition", 4u, uint32_t(WFX_PER_PIXEL),
     COUNT(dissolveParameters), dissolveProcess, dissolveInert, nullptr, nullptr, nullptr, nullptr, nullptr, dissolveShader,
     nullptr, nullptr, nullptr, dissolveHelp, nullptr}};
}  // namespace

WFX_EXPORT int32_t wfxEffectCount(void) { return int32_t(sizeof(effects) / sizeof(effects[0])); }
WFX_EXPORT const WfxEffect *wfxEffectAt(int32_t index) {
  return index >= 0 && index < wfxEffectCount() ? &effects[index] : nullptr;
}
