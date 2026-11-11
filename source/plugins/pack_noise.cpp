// Wavy-Noise.wfx: Noise, Turbulent Noise, Displacement Map, Wave Warp, Ripple,
// Twirl, Bulge, Polar Coordinates and Chromatic Aberration.
#include "wavy_effect.h"
#include "pack_common.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

using namespace wfxpack;

namespace {
float *row(void *pixels, int32_t stride, int32_t y) {
  return reinterpret_cast<float *>(static_cast<unsigned char *>(pixels) + size_t(y) * size_t(stride));
}

// Improved Perlin noise in three dimensions: x and y across the picture, z as
// time, so a field can drift without any two frames being unrelated.
struct Perlin3 {
  uint8_t perm[512];
  explicit Perlin3(uint32_t seed) {
    uint8_t table[256];
    for (int i = 0; i < 256; ++i) table[i] = uint8_t(i);
    uint32_t state = seed * 2654435761u + 12345u;
    for (int i = 255; i > 0; --i) {
      state = state * 1664525u + 1013904223u;
      std::swap(table[i], table[(state >> 8) % uint32_t(i + 1)]);
    }
    for (int i = 0; i < 512; ++i) perm[i] = table[i & 255];
  }
  static float fade(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }
  static float grad(int hash, float x, float y, float z) {
    const int h = hash & 15;
    const float u = h < 8 ? x : y, v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
  }
  struct Slice { int Z; float z, w; };
  Slice slice(float z) const {
    const float floorZ = std::floor(z);
    const float fraction = z - floorZ;
    return {int(floorZ) & 255, fraction, fade(fraction)};
  }
  // x and y are never negative here (pixel positions times a frequency), so
  // truncating is flooring.
  float noise(float x, float y, const Slice &s) const {
    const int X = int(x), Y = int(y);
    x -= float(X); y -= float(Y);
    const float u = fade(x), v = fade(y);
    const int Xm = X & 255, Ym = Y & 255;
    const int A = perm[Xm] + Ym, AA = perm[A] + s.Z, AB = perm[A + 1] + s.Z, B = perm[Xm + 1] + Ym, BA = perm[B] + s.Z, BB = perm[B + 1] + s.Z;
    const float z = s.z, w = s.w;
    auto lerp = [](float t, float a, float b) { return a + t * (b - a); };
    return lerp(w, lerp(v, lerp(u, grad(perm[AA], x, y, z), grad(perm[BA], x - 1, y, z)),
                            lerp(u, grad(perm[AB], x, y - 1, z), grad(perm[BB], x - 1, y - 1, z))),
                    lerp(v, lerp(u, grad(perm[AA + 1], x, y, z - 1), grad(perm[BA + 1], x - 1, y, z - 1)),
                            lerp(u, grad(perm[AB + 1], x, y - 1, z - 1), grad(perm[BB + 1], x - 1, y - 1, z - 1))));
  }
};

// A field of noise over the whole image, values 0..1. Every octave is worked
// out on a grid of its own, as coarse as its wavelength allows, and
// interpolated up; an octave finer than two pixels is left out as aliasing.
struct NoiseField {
  int width = 0, height = 0;
  std::vector<float> value;
};
NoiseField fractalField(const WfxContext *context, int width, int height, double cell, int octaves, bool turbulent,
                        double z, uint32_t seed, double contrast, double brightness) {
  NoiseField field;
  field.width = width; field.height = height;
  field.value.assign(size_t(width) * size_t(height), 0.f);
  if (width <= 0 || height <= 0) return field;
  cell = std::max(2., cell);
  octaves = std::clamp(octaves, 1, 8);
  int used = 1;
  while (used < octaves && cell / double(1 << used) >= 2.) ++used;
  float norm = 0;
  { float amplitude = 1; for (int o = 0; o < used; ++o) { norm += amplitude; amplitude *= .5f; } }
  const Perlin3 perlin(seed);
  float *sum = field.value.data();
  std::vector<float> grid;
  float amplitude = 1;
  for (int o = 0; o < used; ++o, amplitude *= .5f) {
    const double wavelength = cell / double(1 << o);
    const float frequency = float(1. / wavelength), weight = amplitude;
    const auto zSlice = perlin.slice(float(z) * (1.f + .35f * float(o)));
    const int step = std::clamp(int(wavelength / 4.), 1, 8);
    auto sample = [&](float px, float py) {
      const float n = perlin.noise(px * frequency, py * frequency, zSlice);
      return weight * (turbulent ? std::abs(n) : n);
    };
    if (step == 1) {
      forRows(context, height, [&](int32_t begin, int32_t end) {
        for (int y = begin; y < end; ++y) {
          float *out = sum + size_t(y) * size_t(width);
          for (int x = 0; x < width; ++x) out[x] += sample(float(x), float(y));
        }
      });
      continue;
    }
    const int gw = (width + step - 1) / step + 2, gh = (height + step - 1) / step + 2;
    grid.resize(size_t(gw) * size_t(gh));
    forRows(context, gh, [&](int32_t begin, int32_t end) {
      for (int gy = begin; gy < end; ++gy)
        for (int gx = 0; gx < gw; ++gx) grid[size_t(gy) * size_t(gw) + size_t(gx)] = sample(float(gx * step), float(gy * step));
    });
    const float inverse = 1.f / float(step);
    forRows(context, height, [&](int32_t begin, int32_t end) {
      for (int y = begin; y < end; ++y) {
        const float fy = float(y) * inverse;
        const int y0 = int(fy); const float ty = fy - float(y0);
        const float *row0 = &grid[size_t(y0) * size_t(gw)], *row1 = &grid[size_t(std::min(y0 + 1, gh - 1)) * size_t(gw)];
        float *out = sum + size_t(y) * size_t(width);
        for (int x = 0; x < width; ++x) {
          const float fx = float(x) * inverse;
          const int x0 = int(fx); const float tx = fx - float(x0);
          out[x] += (row0[x0] * (1 - tx) + row0[x0 + 1] * tx) * (1 - ty) + (row1[x0] * (1 - tx) + row1[x0 + 1] * tx) * ty;
        }
      }
    });
  }
  const float scaleContrast = float(contrast), offset = float(brightness);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int y = begin; y < end; ++y) {
      float *out = sum + size_t(y) * size_t(width);
      for (int x = 0; x < width; ++x) {
        float v = out[x] / norm;
        v = turbulent ? std::min(1.f, v * 1.75f) : v * .85f + .5f; // perlin sits in about -.7..+.7
        out[x] = std::clamp((v - .5f) * scaleContrast + .5f + offset, 0.f, 1.f);
      }
    }
  });
  return field;
}

// The same field asked for again is not worked out again: dragging Amount,
// Blend or a colour leaves the noise itself alone, and a still noise costs
// nothing after its first frame. Kept small; a moving noise never hits it.
struct FieldKey {
  int width, height, octaves; double cell; bool turbulent; double z; uint32_t seed; double contrast, brightness;
  bool operator==(const FieldKey &o) const {
    return width == o.width && height == o.height && octaves == o.octaves && cell == o.cell && turbulent == o.turbulent &&
           z == o.z && seed == o.seed && contrast == o.contrast && brightness == o.brightness;
  }
};
std::shared_ptr<const NoiseField> cachedField(const WfxContext *context, const FieldKey &key) {
  struct Entry { FieldKey key; std::shared_ptr<const NoiseField> field; };
  static std::mutex guard;
  static std::vector<Entry> entries; // newest last
  {
    std::lock_guard<std::mutex> lock(guard);
    for (size_t i = 0; i < entries.size(); ++i)
      if (entries[i].key == key) {
        auto hit = entries[i].field;
        Entry moved = std::move(entries[i]);
        entries.erase(entries.begin() + std::ptrdiff_t(i));
        entries.push_back(std::move(moved));
        return hit;
      }
  }
  auto made = std::make_shared<const NoiseField>(fractalField(context, key.width, key.height, key.cell, key.octaves,
                                                              key.turbulent, key.z, key.seed, key.contrast, key.brightness));
  std::lock_guard<std::mutex> lock(guard);
  entries.push_back({key, made});
  size_t total = 0;
  for (const auto &entry : entries) total += entry.field->value.size() * sizeof(float);
  while (entries.size() > 1 && (entries.size() > 6 || total > size_t(160) << 20)) {
    total -= entries.front().field->value.size() * sizeof(float);
    entries.erase(entries.begin());
  }
  return made;
}

// How a generated colour meets the layer's own: 0 replaces it, then add,
// multiply, overlay, screen. amount is how much of the result is used.
float blendChannel(int mode, float base, float over, float amount) {
  float result = over;
  switch (mode) {
    case 1: result = base + over; break;
    case 2: result = base * over; break;
    case 3: result = base < .5f ? 2.f * base * over : 1.f - 2.f * (1.f - base) * (1.f - over); break;
    case 4: result = 1.f - (1.f - base) * (1.f - over); break;
    default: break;
  }
  return std::clamp(base + (result - base) * amount, 0.f, 1.f);
}
uint32_t hashCell(uint32_t seed, int gx, int gy, uint32_t channel) {
  uint32_t h = seed ^ (uint32_t(gx) * 374761393u) ^ (uint32_t(gy) * 668265263u) ^ (channel * 2246822519u);
  h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
  return h;
}
int whole(const float *v, int32_t n, int index, int fallback) { return int(std::lround(value(v, n, index, float(fallback)))); }

// ---- Noise -------------------------------------------------------------------
// White noise laid over the layer: one random value per cell, in grey or in
// colour. Static, or (Speed above zero) a fresh pattern that many times a
// second -- the flicker of a dead television.
int noiseInert(const float *v, int32_t n) { return std::clamp(divided(v, n, 0, 50, 100.), 0.f, 1.f) <= 0; }
int noiseVaries(const float *v, int32_t n) { return value(v, n, 4, 0) > 0; }
int noiseProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float amount = std::clamp(divided(v, n, 0, 50, 100.), 0.f, 1.f);
  if (amount <= 0) return 0;
  const int cell = std::max(1, int(std::lround(std::clamp(double(value(v, n, 1, 1)), .5, 200.) * context->scale)));
  const float colourful = std::clamp(divided(v, n, 2, 0, 100.), 0.f, 1.f);
  const int mode = std::clamp(whole(v, n, 3, 1), 0, 4);
  const double speed = std::max(0., double(value(v, n, 4, 0)));
  const uint32_t seed = uint32_t(whole(v, n, 5, 1)) * 2654435761u + uint32_t(std::floor(context->seconds * speed)) * 40503u;
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const int gy = y / cell;
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const int gx = x / cell;
        const float grey = float(hashCell(seed, gx, gy, 0) & 0xffff) / 65535.f;
        for (int c = 0; c < 3; ++c) {
          const float nn = grey * (1.f - colourful) + float(hashCell(seed, gx, gy, uint32_t(c) + 1) & 0xffff) / 65535.f * colourful;
          p[x * 4 + c] = blendChannel(mode, p[x * 4 + c], nn, amount);
        }
      }
    }
  });
  return 0;
}
const char *blends = "Replace\nAdd\nMultiply\nOverlay\nScreen";
const WfxParameter noiseParameters[] = {
    number("amount", "Amount", 0, 100, 50, 1, " %"), number("size", "Size", .5f, 200, 1, .5f, " px"),
    number("colour", "Colour", 0, 100, 0, 1, " %"), choice("mode", "Blend", blends, 5, 1),
    number("speed", "Speed (changes per second, 0 = still)", 0, 60, 0, 1), number("seed", "Seed", 0, 9999, 1, 1)};

// ---- Turbulent Noise ---------------------------------------------------------
// Fractal noise with a hand on every dial: clouds, smoke, water, energy.
// Mapped between two colours, then blended over the layer. Evolution moves
// through the noise's third dimension; Speed advances it with time.
enum { TType, TScale, TComplexity, TContrast, TBrightness, TEvolution, TSpeed, TSeed, TDark, TLight = TDark + 4,
       TMode = TLight + 4, TAmount };
int turbulentInert(const float *v, int32_t n) { return std::clamp(divided(v, n, TAmount, 100, 100.), 0.f, 1.f) <= 0; }
int turbulentVaries(const float *v, int32_t n) { return value(v, n, TSpeed, 1) > 0; }
int turbulentProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n,
                     const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float amount = std::clamp(divided(v, n, TAmount, 100, 100.), 0.f, 1.f);
  if (amount <= 0) return 0;
  const bool turbulent = whole(v, n, TType, 0) == 1;
  const double z = double(value(v, n, TEvolution, 0)) + std::max(0., double(value(v, n, TSpeed, 1))) * context->seconds;
  const FieldKey key{width, height, whole(v, n, TComplexity, 4), std::max(2., double(value(v, n, TScale, 200)) * context->scale),
                     turbulent, z, uint32_t(whole(v, n, TSeed, 1)),
                     std::clamp(double(value(v, n, TContrast, 100)), 0., 500.) / 100.,
                     std::clamp(double(value(v, n, TBrightness, 0)), -100., 100.) / 100.};
  const auto shared = cachedField(context, key);
  const NoiseField &field = *shared;
  const float d[3] = {value(v, n, TDark, 0), value(v, n, TDark + 1, 0), value(v, n, TDark + 2, 0)};
  const float b[3] = {value(v, n, TLight, 1), value(v, n, TLight + 1, 1), value(v, n, TLight + 2, 1)};
  const int mode = std::clamp(whole(v, n, TMode, 0), 0, 4);
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *p = row(pixels, stride, y);
      const float *values = &field.value[size_t(y) * size_t(width)];
      for (int32_t x = 0; x < width; ++x) {
        if (p[x * 4 + 3] <= 0.f) continue;
        const float nn = values[x];
        for (int c = 0; c < 3; ++c) p[x * 4 + c] = blendChannel(mode, p[x * 4 + c], d[c] + (b[c] - d[c]) * nn, amount);
      }
    }
  });
  return 0;
}
const WfxParameter turbulentParameters[] = {
    choice("type", "Type", "Fractal\nTurbulent", 2, 0), number("scale", "Scale", 4, 4000, 200, 1, " px"),
    number("complexity", "Complexity", 1, 8, 4, 1), number("contrast", "Contrast", 0, 500, 100, 1, " %"),
    number("brightness", "Brightness", -100, 100, 0, 1, " %"), number("evolution", "Evolution", -1000, 1000, 0, .05f),
    number("speed", "Speed (0 = still)", 0, 20, 1, .05f), number("seed", "Seed", 0, 9999, 1, 1),
    color("dark", "Dark", 0, 0, 0, 1), color("light", "Light", 1, 1, 1, 1), choice("mode", "Blend", blends, 5, 0),
    number("amount", "Amount", 0, 100, 100, 1, " %")};

// ---- Displacement Map --------------------------------------------------------
// Pushes the layer's pixels around by a noise map: heat haze, water, a picture
// shaken apart. The two directions read independent fields. Edge: what lies
// beyond the picture is transparent, held, or wrapped.
enum { DHorizontal, DVertical, DScale, DComplexity, DEvolution, DSpeed, DSeed, DEdge };
int displaceInert(const float *v, int32_t n) {
  return std::clamp(double(value(v, n, DHorizontal, 20)), -1000., 1000.) == 0 && std::clamp(double(value(v, n, DVertical, 20)), -1000., 1000.) == 0;
}
int displaceVaries(const float *v, int32_t n) { return value(v, n, DSpeed, 1) > 0; }
double displaceReach(const float *v, int32_t n, double) {
  const double amount = std::min(std::max(std::abs(double(value(v, n, DHorizontal, 20))), std::abs(double(value(v, n, DVertical, 20)))), 1000.);
  return amount > 0 ? std::ceil(amount / 8.) * 8. : 0;
}
int displaceProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n,
                    const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float horizontal = float(std::clamp(double(value(v, n, DHorizontal, 20)), -1000., 1000.) * context->scale);
  const float vertical = float(std::clamp(double(value(v, n, DVertical, 20)), -1000., 1000.) * context->scale);
  if ((horizontal == 0 && vertical == 0) || width <= 0 || height <= 0) return 0;
  const double z = double(value(v, n, DEvolution, 0)) + std::max(0., double(value(v, n, DSpeed, 1))) * context->seconds;
  const double cell = std::max(2., double(value(v, n, DScale, 120)) * context->scale);
  const int octaves = whole(v, n, DComplexity, 2);
  const uint32_t seed = uint32_t(whole(v, n, DSeed, 1));
  const int edge = std::clamp(whole(v, n, DEdge, 0), 0, 2);
  const auto fxShared = cachedField(context, {width, height, octaves, cell, false, z, seed, 1.6, 0.});
  const auto fyShared = cachedField(context, {width, height, octaves, cell, false, z, seed + 7919u, 1.6, 0.});
  const NoiseField &fx = *fxShared, &fy = *fyShared;
  // The picture as it was, in memory kept from the last frame: a fresh copy
  // of a 1080p float picture costs more in first touches than the move does.
  Scratch kept(size_t(width) * size_t(height) * 4);
  float *const source = kept.data();
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *to = &source[size_t(y) * size_t(width) * 4];
      std::memcpy(to, row(pixels, stride, y), size_t(width) * 16);
    }
  });
  auto texel = [&](int x, int y, int c) {
    if (edge == 2) { x = ((x % width) + width) % width; y = ((y % height) + height) % height; }
    else if (edge == 1) { x = std::clamp(x, 0, width - 1); y = std::clamp(y, 0, height - 1); }
    else if (x < 0 || y < 0 || x >= width || y >= height) return 0.f;
    return source[(size_t(y) * size_t(width) + size_t(x)) * 4 + size_t(c)];
  };
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *dest = row(pixels, stride, y);
      for (int32_t x = 0; x < width; ++x) {
        const size_t at = size_t(y) * size_t(width) + size_t(x);
        const float sx = float(x) + (fx.value[at] - .5f) * 2.f * horizontal;
        const float sy = float(y) + (fy.value[at] - .5f) * 2.f * vertical;
        const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
        const float tx = sx - float(x0), ty = sy - float(y0);
        float *d = dest + x * 4;
        if (x0 >= 0 && y0 >= 0 && x0 + 1 < width && y0 + 1 < height) {
          const float *r0 = &source[(size_t(y0) * size_t(width) + size_t(x0)) * 4];
          const float *r1 = &source[(size_t(y0 + 1) * size_t(width) + size_t(x0)) * 4];
          const float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty), w01 = (1 - tx) * ty, w11 = tx * ty;
          for (int c = 0; c < 4; ++c) d[c] = r0[c] * w00 + r0[4 + c] * w10 + r1[c] * w01 + r1[4 + c] * w11;
        } else {
          for (int c = 0; c < 4; ++c)
            d[c] = (texel(x0, y0, c) * (1 - tx) + texel(x0 + 1, y0, c) * tx) * (1 - ty) +
                   (texel(x0, y0 + 1, c) * (1 - tx) + texel(x0 + 1, y0 + 1, c) * tx) * ty;
        }
      }
    }
  });
  return 0;
}
const WfxParameter displaceParameters[] = {
    number("horizontal", "Horizontal", -1000, 1000, 20, 1, " px"), number("vertical", "Vertical", -1000, 1000, 20, 1, " px"),
    number("scale", "Map Scale", 4, 4000, 120, 1, " px"), number("complexity", "Map Complexity", 1, 8, 2, 1),
    number("evolution", "Evolution", -1000, 1000, 0, .05f), number("speed", "Speed (0 = still)", 0, 20, 1, .05f),
    number("seed", "Seed", 0, 9999, 1, 1), choice("edge", "Edges", "Transparent\nClamp\nWrap", 3, 0)};

// distort() for the card: where a pixel reads from, between pixels by its
// four neighbours, Edges saying what lies beyond (0 nothing, 1 the edge
// pixel, 2 the picture again). These effects work on premultiplied colour:
// the reads are multiplied in, and the result divided out again.
#define DISTORT_GLSL \
  "int noWhole(float v){return int(v<0.?-floor(-v+.5):floor(v+.5));}" \
  "vec4 noTexel(int x,int y,int edge){ivec2 sz=ivec2(sourceSize);" \
  "if(edge==2){x-=sz.x*int(floor(float(x)/float(sz.x)));y-=sz.y*int(floor(float(y)/float(sz.y)));}" \
  "else if(edge==1){x=clamp(x,0,sz.x-1);y=clamp(y,0,sz.y-1);}" \
  "vec4 t=wavyPixel(ivec2(x,y));return vec4(t.rgb*t.a,t.a);}" \
  "vec4 noDistort(vec2 at,int edge){vec2 q=at-.5;vec2 f=floor(q);ivec2 i=ivec2(f);vec2 t=q-f;" \
  "return (noTexel(i.x,i.y,edge)*(1.-t.x)+noTexel(i.x+1,i.y,edge)*t.x)*(1.-t.y)+" \
  "(noTexel(i.x,i.y+1,edge)*(1.-t.x)+noTexel(i.x+1,i.y+1,edge)*t.x)*t.y;}" \
  "vec4 noStraight(vec4 d){return d.a>0.?vec4(d.rgb/d.a,d.a):vec4(0.);}" \
  "vec4 noBox(vec2 s){vec4 b=wavyBox;if(!(b.z>0.&&b.w>0.))b=vec4(0.,0.,s);return b;}"

// Noise on the card. White noise is the same hash at every pixel. The fractal
// field takes two rounds: the first lays each seed's shuffled table out in the
// first pixels (the same shuffle as Perlin3's, run by the 256 pixels that hold
// it), the second works out the field at every pixel the way fractalField
// does -- an octave on its coarse grid is the four grid points round the
// pixel, interpolated with the same weights -- and reads the picture again
// through wavyOriginal().
#define PERLIN_GLSL \
  "vec4 pnTables(vec2 p,vec2 s,uint seed,int count){int k=int(p.y)*int(s.x)+int(p.x);if(k>=256*count)return vec4(0.);" \
  "uint state=(k<256?seed:seed+7919u)*2654435761u+12345u;int t[256];for(int j=0;j<256;++j)t[j]=j;" \
  "for(int j=255;j>0;--j){state=state*1664525u+1013904223u;int r=int((state>>8u)%uint(j+1));int keep=t[j];t[j]=t[r];t[r]=keep;}" \
  "return vec4(float(t[k&255]),0.,0.,1.);}" \
  "int pnPerm(int table,int i){int k=table*256+(i&255),w=int(sourceSize.x);return int(wavyPixel(ivec2(k%w,k/w)).x+.5);}" \
  "float pnFade(float t){return t*t*t*(t*(t*6.-15.)+10.);}" \
  "float pnGrad(int hash,float x,float y,float z){int h=hash&15;float u=h<8?x:y,v=h<4?y:((h==12||h==14)?x:z);" \
  "return ((h&1)!=0?-u:u)+((h&2)!=0?-v:v);}" \
  "float pnLerp(float t,float a,float b){return a+t*(b-a);}" \
  "float pnNoise(int table,float x,float y,int Z,float z,float w){int X=int(x),Y=int(y);x-=float(X);y-=float(Y);" \
  "float u=pnFade(x),v=pnFade(y);int Xm=X&255,Ym=Y&255;" \
  "int A=pnPerm(table,Xm)+Ym,AA=pnPerm(table,A)+Z,AB=pnPerm(table,A+1)+Z,B=pnPerm(table,Xm+1)+Ym," \
  "BA=pnPerm(table,B)+Z,BB=pnPerm(table,B+1)+Z;" \
  "return pnLerp(w,pnLerp(v,pnLerp(u,pnGrad(pnPerm(table,AA),x,y,z),pnGrad(pnPerm(table,BA),x-1.,y,z))," \
  "pnLerp(u,pnGrad(pnPerm(table,AB),x,y-1.,z),pnGrad(pnPerm(table,BB),x-1.,y-1.,z)))," \
  "pnLerp(v,pnLerp(u,pnGrad(pnPerm(table,AA+1),x,y,z-1.),pnGrad(pnPerm(table,BA+1),x-1.,y,z-1.))," \
  "pnLerp(u,pnGrad(pnPerm(table,AB+1),x,y-1.,z-1.),pnGrad(pnPerm(table,BB+1),x-1.,y-1.,z-1.))));}" \
  "float pnSample(int table,float px,float py,float f,int Z,float z,float w,bool turbulent,float weight){" \
  "float n=pnNoise(table,px*f,py*f,Z,z,w);return weight*(turbulent?abs(n):n);}" \
  "float pnField(int table,vec2 at,float cell,int octaves,bool turbulent,float z,float contrast,float brightness){" \
  "cell=max(2.,cell);octaves=clamp(octaves,1,8);int used=1;while(used<octaves&&cell/float(1<<used)>=2.)++used;" \
  "float norm=0.,amp=1.;for(int o=0;o<used;++o){norm+=amp;amp*=.5;}float sum=0.;amp=1.;" \
  "for(int o=0;o<used;++o){float wavelength=cell/float(1<<o),f=1./wavelength;" \
  "float zz=z*(1.+.35*float(o)),fz=floor(zz),zf=zz-fz;int Z=int(fz)&255;float zw=pnFade(zf);" \
  "int step=clamp(int(wavelength/4.),1,8);" \
  "if(step==1)sum+=pnSample(table,at.x,at.y,f,Z,zf,zw,turbulent,amp);" \
  "else{float inv=1./float(step),fx=at.x*inv,fy=at.y*inv;int x0=int(fx),y0=int(fy);float tx=fx-float(x0),ty=fy-float(y0);" \
  "float g00=pnSample(table,float(x0*step),float(y0*step),f,Z,zf,zw,turbulent,amp)," \
  "g10=pnSample(table,float((x0+1)*step),float(y0*step),f,Z,zf,zw,turbulent,amp)," \
  "g01=pnSample(table,float(x0*step),float((y0+1)*step),f,Z,zf,zw,turbulent,amp)," \
  "g11=pnSample(table,float((x0+1)*step),float((y0+1)*step),f,Z,zf,zw,turbulent,amp);" \
  "sum+=(g00*(1.-tx)+g10*tx)*(1.-ty)+(g01*(1.-tx)+g11*tx)*ty;}" \
  "amp*=.5;}" \
  "float val=sum/norm;val=turbulent?min(1.,val*1.75):val*.85+.5;return clamp((val-.5)*contrast+.5+brightness,0.,1.);}" \
  "float pnBlend(int mode,float base,float over,float amount){float r=over;if(mode==1)r=base+over;else if(mode==2)r=base*over;" \
  "else if(mode==3)r=base<.5?2.*base*over:1.-2.*(1.-base)*(1.-over);else if(mode==4)r=1.-(1.-base)*(1.-over);" \
  "return clamp(base+(r-base)*amount,0.,1.);}"
const char *noiseShader = DISTORT_GLSL PERLIN_GLSL
    "uint niHash(uint seed,int gx,int gy,uint c){uint h=seed^(uint(gx)*374761393u)^(uint(gy)*668265263u)^(c*2246822519u);"
    "h=(h^(h>>13u))*1274126177u;h^=h>>16u;return h;}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){float amount=clamp(values[0]/100.,0.,1.);if(amount<=0.||v.a<=0.)return v;"
    "int cell=max(1,noWhole(clamp(values[1],.5,200.)*wavyScale));float colourful=clamp(values[2]/100.,0.,1.);"
    "int mode=clamp(noWhole(values[3]),0,4);float x=wavySeconds*max(0.,values[4]);"
    "uint seed=uint(noWhole(values[5]))*2654435761u+uint(int(floor(x+max(1e-4,abs(x)*4e-7))))*40503u;"
    "int gx=int(p.x)/cell,gy=int(p.y)/cell;float grey=float(niHash(seed,gx,gy,0u)&0xffffu)/65535.;vec3 o;"
    "for(int c=0;c<3;++c){float nn=grey*(1.-colourful)+float(niHash(seed,gx,gy,uint(c)+1u)&0xffffu)/65535.*colourful;"
    "o[c]=pnBlend(mode,v[c],nn,amount);}"
    "return vec4(o,v.a);}";
// The table needs the raster to hold it: a picture under 16 by 16 is left to process().
int32_t turbulentShaderPasses(const float *v, int32_t n, const WfxContext *context) {
  if (!context || turbulentInert(v, n)) return 1;
  return int64_t(context->rasterWidth) * context->rasterHeight >= 256 ? 2 : 0;
}
const char *turbulentShader = DISTORT_GLSL PERLIN_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){float amount=clamp(values[17]/100.,0.,1.);if(amount<=0.)return v;"
    "if(wavyPass==0)return pnTables(p,s,uint(noWhole(values[7])),1);"
    "vec4 o=wavyOriginal(ivec2(p));if(o.a<=0.)return o;"
    "float z=values[5]+max(0.,values[6])*wavySeconds;"
    "float nn=pnField(0,floor(p),max(2.,values[1]*wavyScale),noWhole(values[2]),noWhole(values[0])==1,z,"
    "clamp(values[3],0.,500.)/100.,clamp(values[4],-100.,100.)/100.);"
    "int mode=clamp(noWhole(values[16]),0,4);vec3 d=vec3(values[8],values[9],values[10]),b=vec3(values[12],values[13],values[14]);"
    "vec3 r;for(int c=0;c<3;++c)r[c]=pnBlend(mode,o[c],d[c]+(b[c]-d[c])*nn,amount);return vec4(r,o.a);}";
int32_t displaceShaderPasses(const float *v, int32_t n, const WfxContext *context) {
  if (!context || displaceInert(v, n)) return 1;
  return int64_t(context->rasterWidth) * context->rasterHeight >= 512 ? 2 : 0;
}
const char *displaceShader = DISTORT_GLSL PERLIN_GLSL
    "vec4 dmTexel(int x,int y,int edge){ivec2 sz=textureSize(wavyOriginalTexture,0);"
    "if(edge==2){x-=sz.x*int(floor((float(x)+.5)/float(sz.x)));y-=sz.y*int(floor((float(y)+.5)/float(sz.y)));}"
    "else if(edge==1){x=clamp(x,0,sz.x-1);y=clamp(y,0,sz.y-1);}"
    "vec4 t=wavyOriginal(ivec2(x,y));return vec4(t.rgb*t.a,t.a);}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float horizontal=clamp(values[0],-1000.,1000.)*wavyScale,vertical=clamp(values[1],-1000.,1000.)*wavyScale;"
    "if(horizontal==0.&&vertical==0.)return v.a>0.?v:vec4(0.);"
    "if(wavyPass==0)return pnTables(p,s,uint(noWhole(values[6])),2);"
    "float z=values[4]+max(0.,values[5])*wavySeconds,cell=max(2.,values[2]*wavyScale);int octaves=noWhole(values[3]);"
    "vec2 at=floor(p);float fx=pnField(0,at,cell,octaves,false,z,1.6,0.),fy=pnField(1,at,cell,octaves,false,z,1.6,0.);"
    "float sx=at.x+(fx-.5)*2.*horizontal,sy=at.y+(fy-.5)*2.*vertical;int edge=clamp(noWhole(values[7]),0,2);"
    "int x0=int(floor(sx)),y0=int(floor(sy));float tx=sx-float(x0),ty=sy-float(y0);"
    "vec4 d=(dmTexel(x0,y0,edge)*(1.-tx)+dmTexel(x0+1,y0,edge)*tx)*(1.-ty)+(dmTexel(x0,y0+1,edge)*(1.-tx)+dmTexel(x0+1,y0+1,edge)*tx)*ty;"
    "return noStraight(d);}";
// ---- Wave Warp -----------------------------------------------------------------------
// The layer rippled by a wave running across it: a flag, water, a heat shimmer,
// a title that will not keep still. The wave runs along Direction (0 is up, 90
// to the right) and moves each pixel across it by up to Wave Height; Speed sets
// it travelling, in waves a second.
enum { WType, WHeight, WWidth, WDirection, WSpeed, WPhase, WEdge };
int waveInert(const float *v, int32_t n) { return std::abs(std::clamp(double(value(v, n, WHeight, 10)), -2000., 2000.)) < 1e-6; }
int waveVaries(const float *v, int32_t n) { return std::abs(value(v, n, WSpeed, 1)) > 1e-6f; }
double waveReach(const float *v, int32_t n, double) {
  const double height = std::abs(std::clamp(double(value(v, n, WHeight, 10)), -2000., 2000.));
  return height > 0 ? std::ceil(height / 8.) * 8. : 0;
}
int waveProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float amplitude = float(std::clamp(double(value(v, n, WHeight, 10)), -2000., 2000.) * context->scale);
  if (std::abs(amplitude) < 1e-6f || width <= 0 || height <= 0) return 0;
  const int type = std::clamp(whole(v, n, WType, 0), 0, 3);
  const double length = std::max(1., std::clamp(double(value(v, n, WWidth, 40)), 1., 20000.) * context->scale);
  const double direction = double(value(v, n, WDirection, 90)) * 3.14159265358979323846 / 180.;
  const float tx = float(std::sin(direction)), ty = float(-std::cos(direction)); // along the wave
  const float nx = float(std::cos(direction)), ny = float(std::sin(direction));  // across it
  const double turns = double(value(v, n, WPhase, 0)) / 360. + double(value(v, n, WSpeed, 1)) * context->seconds;
  const float phase = float((turns - std::floor(turns)) * 6.283185307179586);
  const float perPixel = float(6.283185307179586 / length);
  const int edge = std::clamp(whole(v, n, WEdge, 0), 0, 2);
  auto wave = [type](float s) {
    switch (type) {
      case 1: return float(std::asin(std::sin(s)) * .63661977f);         // triangle
      case 2: return std::clamp(std::sin(s) * 6.f, -1.f, 1.f);           // square, its steps a little rounded
      case 3: { const float t = s * .15915494f; return 2.f * (t - std::floor(t)) - 1.f; } // sawtooth
      default: return std::sin(s);                                        // sine
    }
  };
  Scratch kept(size_t(width) * size_t(height) * 4);
  float *const source = kept.data();
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y)
      std::memcpy(&source[size_t(y) * size_t(width) * 4], row(pixels, stride, y), size_t(width) * 16);
  });
  auto texel = [&](int x, int y, int c) {
    if (edge == 2) { x = ((x % width) + width) % width; y = ((y % height) + height) % height; }
    else if (edge == 1) { x = std::clamp(x, 0, width - 1); y = std::clamp(y, 0, height - 1); }
    else if (x < 0 || y < 0 || x >= width || y >= height) return 0.f;
    return source[(size_t(y) * size_t(width) + size_t(x)) * 4 + size_t(c)];
  };
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *dest = row(pixels, stride, y);
      const float py = float(y) + .5f;
      for (int32_t x = 0; x < width; ++x) {
        const float px = float(x) + .5f;
        const float shift = amplitude * wave((px * tx + py * ty) * perPixel + phase);
        const float sx = px - nx * shift - .5f, sy = py - ny * shift - .5f;
        const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
        const float fx = sx - float(x0), fy = sy - float(y0);
        float *d = dest + x * 4;
        if (x0 >= 0 && y0 >= 0 && x0 + 1 < width && y0 + 1 < height) {
          const float *r0 = &source[(size_t(y0) * size_t(width) + size_t(x0)) * 4];
          const float *r1 = &source[(size_t(y0 + 1) * size_t(width) + size_t(x0)) * 4];
          const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
          for (int c = 0; c < 4; ++c) d[c] = r0[c] * w00 + r0[4 + c] * w10 + r1[c] * w01 + r1[4 + c] * w11;
        } else {
          for (int c = 0; c < 4; ++c)
            d[c] = (texel(x0, y0, c) * (1 - fx) + texel(x0 + 1, y0, c) * fx) * (1 - fy) +
                   (texel(x0, y0 + 1, c) * (1 - fx) + texel(x0 + 1, y0 + 1, c) * fx) * fy;
        }
      }
    }
  });
  return 0;
}
const char *waveShader = DISTORT_GLSL
    "float waShape(int type,float s){"
    "if(type==1)return asin(sin(s))*.63661977;if(type==2)return clamp(sin(s)*6.,-1.,1.);"
    "if(type==3){float t=s*.15915494;return 2.*(t-floor(t))-1.;}return sin(s);}"
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float amplitude=clamp(values[1],-2000.,2000.)*wavyScale;if(abs(amplitude)<1e-6)return v;"
    "int type=clamp(noWhole(values[0]),0,3);float len=max(1.,clamp(values[2],1.,20000.)*wavyScale);"
    "float direction=mod(values[3],360.)*3.14159265358979/180.;"
    "float tx=sin(direction),ty=-cos(direction),nx=cos(direction),ny=sin(direction);"
    "float turns=values[5]/360.+values[4]*wavySeconds;float phase=(turns-floor(turns))*6.283185307;"
    "float perPixel=6.283185307/len;int edge=clamp(noWhole(values[6]),0,2);"
    "float shift=amplitude*waShape(type,(p.x*tx+p.y*ty)*perPixel+phase);"
    "return noStraight(noDistort(vec2(p.x-nx*shift,p.y-ny*shift),edge));}";
const WfxParameter waveParameters[] = {
    choice("type", "Wave Type", "Sine\nTriangle\nSquare\nSawtooth", 4, 0), number("height", "Wave Height", -2000, 2000, 10, .5f, " px"),
    number("width", "Wave Width", 1, 20000, 40, 1, " px"), number("direction", "Direction", -3600, 3600, 90, 1, "\xc2\xb0"),
    number("speed", "Wave Speed", -60, 60, 1, .05f, " /s"), number("phase", "Phase", -36000, 36000, 0, 1, "\xc2\xb0"),
    choice("edge", "Edges", "Transparent\nClamp\nWrap", 3, 0)};
const char *const waveHelp[] = {
    "The shape of the wave", "How far the wave moves a pixel, in layer pixels", "The length of one wave, in layer pixels",
    "Which way the wave runs: 0 up, 90 to the right", "How many waves pass a point each second (0 holds it still)",
    "Where along its length the wave starts", "What shows where the wave pulls in from beyond the layer"};

// ---- Distortions --------------------------------------------------------------------
// Every pixel of the result read from somewhere else in the picture, between
// pixels by its four neighbours. `from(px, py, sx, sy)` says where, for a
// pixel's middle; Edges says what lies beyond the picture.
template <class F>
void distort(void *pixels, int32_t width, int32_t height, int32_t stride, int edge, const WfxContext *context, const F &from) {
  Scratch kept(size_t(width) * size_t(height) * 4);
  float *const source = kept.data();
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y)
      std::memcpy(&source[size_t(y) * size_t(width) * 4], row(pixels, stride, y), size_t(width) * 16);
  });
  auto texel = [&](int x, int y, int c) {
    if (edge == 2) { x = ((x % width) + width) % width; y = ((y % height) + height) % height; }
    else if (edge == 1) { x = std::clamp(x, 0, width - 1); y = std::clamp(y, 0, height - 1); }
    else if (x < 0 || y < 0 || x >= width || y >= height) return 0.f;
    return source[(size_t(y) * size_t(width) + size_t(x)) * 4 + size_t(c)];
  };
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *dest = row(pixels, stride, y);
      for (int32_t x = 0; x < width; ++x) {
        float sxm, sym;
        from(float(x) + .5f, float(y) + .5f, sxm, sym);
        const float sx = sxm - .5f, sy = sym - .5f;
        const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
        const float fx = sx - float(x0), fy = sy - float(y0);
        float *d = dest + x * 4;
        if (x0 >= 0 && y0 >= 0 && x0 + 1 < width && y0 + 1 < height) {
          const float *r0 = &source[(size_t(y0) * size_t(width) + size_t(x0)) * 4];
          const float *r1 = &source[(size_t(y0 + 1) * size_t(width) + size_t(x0)) * 4];
          const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
          for (int c = 0; c < 4; ++c) d[c] = r0[c] * w00 + r0[4 + c] * w10 + r1[c] * w01 + r1[4 + c] * w11;
        } else {
          for (int c = 0; c < 4; ++c)
            d[c] = (texel(x0, y0, c) * (1 - fx) + texel(x0 + 1, y0, c) * fx) * (1 - fy) +
                   (texel(x0, y0 + 1, c) * (1 - fx) + texel(x0 + 1, y0 + 1, c) * fx) * fy;
        }
      }
    }
  });
}
struct Centre { float x, y; };
Centre centreOf(const WfxContext *context, int32_t width, int32_t height, float cxPercent, float cyPercent) {
  double bx = context->boxX, by = context->boxY, bw = context->boxWidth, bh = context->boxHeight;
  if (!(bw > 0 && bh > 0)) { bx = 0; by = 0; bw = width; bh = height; }
  return {float(bx + bw * std::clamp(double(cxPercent), -100., 200.) / 100.),
          float(by + bh * std::clamp(double(cyPercent), -100., 200.) / 100.)};
}

// ---- Ripple ----------------------------------------------------------------------------
// Rings running out from a point, as from a stone dropped in water: each pixel
// moved toward or away from the middle by a wave of its distance from it,
// fading out toward Radius.
enum { RAmplitude, RWavelength, RSpeed, RCentreX, RCentreY, RRadius, RPhase, REdge };
int rippleInert(const float *v, int32_t n) { return std::abs(std::clamp(double(value(v, n, RAmplitude, 8)), -1000., 1000.)) < 1e-6; }
int rippleVaries(const float *v, int32_t n) { return std::abs(value(v, n, RSpeed, 1)) > 1e-6f; }
double rippleReach(const float *v, int32_t n, double) {
  const double amplitude = std::abs(std::clamp(double(value(v, n, RAmplitude, 8)), -1000., 1000.));
  return amplitude > 0 ? std::ceil(amplitude / 8.) * 8. : 0;
}
int rippleProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float amplitude = float(std::clamp(double(value(v, n, RAmplitude, 8)), -1000., 1000.) * context->scale);
  if (std::abs(amplitude) < 1e-6f || width <= 0 || height <= 0) return 0;
  const float wavelength = float(std::max(1., std::clamp(double(value(v, n, RWavelength, 60)), 1., 20000.) * context->scale));
  const float radius = float(std::max(0., double(value(v, n, RRadius, 0))) * context->scale);
  const double turns = double(value(v, n, RPhase, 0)) / 360. - double(value(v, n, RSpeed, 1)) * context->seconds;
  const float phase = float((turns - std::floor(turns)) * 6.283185307179586);
  const Centre c = centreOf(context, width, height, value(v, n, RCentreX, 50), value(v, n, RCentreY, 50));
  const int edge = std::clamp(whole(v, n, REdge, 1), 0, 2);
  const float tau = 6.28318531f;
  distort(pixels, width, height, stride, edge, context, [&](float px, float py, float &sx, float &sy) {
    const float dx = px - c.x, dy = py - c.y;
    const float r = std::sqrt(dx * dx + dy * dy);
    float shift = amplitude * std::sin(r / wavelength * tau + phase);
    if (radius > 0) { const float t = std::clamp(1.f - r / radius, 0.f, 1.f); shift *= t * t * (3.f - 2.f * t); }
    // Calm at the very middle, growing over the first ring: at the point the
    // rings leave from there is no "outward" to push along, and pushed at full
    // strength the middle tore apart.
    { const float t = std::clamp(r / wavelength, 0.f, 1.f); shift *= t * t * (3.f - 2.f * t); }
    if (r > 1e-4f) { sx = px - dx / r * shift; sy = py - dy / r * shift; }
    else { sx = px; sy = py; }
  });
  return 0;
}
const char *rippleShader = DISTORT_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float amplitude=clamp(values[0],-1000.,1000.)*wavyScale;if(abs(amplitude)<1e-6)return v;"
    "float wavelength=max(1.,clamp(values[1],1.,20000.)*wavyScale);float radius=max(0.,values[5])*wavyScale;"
    "float turns=values[6]/360.-values[2]*wavySeconds;float phase=(turns-floor(turns))*6.283185307;"
    "vec4 b=noBox(s);vec2 c=vec2(b.x+b.z*clamp(values[3],-100.,200.)/100.,b.y+b.w*clamp(values[4],-100.,200.)/100.);"
    "int edge=clamp(noWhole(values[7]),0,2);float tau=6.28318531;"
    "vec2 d=p-c;float r=sqrt(d.x*d.x+d.y*d.y);float shift=amplitude*sin(r/wavelength*tau+phase);"
    "if(radius>0.){float t=clamp(1.-r/radius,0.,1.);shift*=t*t*(3.-2.*t);}"
    "{float t=clamp(r/wavelength,0.,1.);shift*=t*t*(3.-2.*t);}"
    "vec2 at=r>1e-4?p-d/r*shift:p;return noStraight(noDistort(at,edge));}";
const WfxParameter rippleParameters[] = {
    number("amplitude", "Amplitude", -1000, 1000, 8, .5f, " px"), number("wavelength", "Wavelength", 1, 20000, 60, 1, " px"),
    number("speed", "Speed", -60, 60, 1, .05f, " /s"), number("centerX", "Center X", -100, 200, 50, 1, " %"),
    number("centerY", "Center Y", -100, 200, 50, 1, " %"), number("radius", "Radius (0 = all)", 0, 20000, 0, 1, " px"),
    number("phase", "Phase", -36000, 36000, 0, 1, "\xc2\xb0"), choice("edge", "Edges", "Transparent\nClamp\nWrap", 3, 1)};
const char *const rippleHelp[] = {
    "How far a ring moves a pixel, in layer pixels", "The distance from one ring to the next", "How many rings leave the middle each second",
    "Where the rings start, across the layer", "Where the rings start, down the layer", "Where the rings have faded away (0 reaches everywhere)",
    "Where along its length the first ring is", "What shows where a ring pulls in from beyond the layer"};

// ---- Twirl --------------------------------------------------------------------------------
// The picture turned about a point, most at the middle and not at all at Radius:
// a whirlpool, a swirl transition when the angle is keyed.
enum { TAngle, TRadius, TCentreX, TCentreY };
int twirlInert(const float *v, int32_t n) { return std::abs(value(v, n, TAngle, 90)) < 1e-6f; }
int twirlProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float angle = float(double(value(v, n, TAngle, 90)) * 3.14159265358979323846 / 180.);
  if (std::abs(angle) < 1e-9f || width <= 0 || height <= 0) return 0;
  const Centre c = centreOf(context, width, height, value(v, n, TCentreX, 50), value(v, n, TCentreY, 50));
  float radius = float(std::max(0., double(value(v, n, TRadius, 0))) * context->scale);
  if (radius <= 0) {
    double bw = context->boxWidth, bh = context->boxHeight;
    if (!(bw > 0 && bh > 0)) { bw = width; bh = height; }
    radius = float(std::min(bw, bh) / 2.);
  }
  distort(pixels, width, height, stride, 1, context, [&](float px, float py, float &sx, float &sy) {
    const float dx = px - c.x, dy = py - c.y;
    const float r = std::sqrt(dx * dx + dy * dy);
    if (r >= radius) { sx = px; sy = py; return; }
    const float t = 1.f - r / radius;
    const float turn = -angle * t * t;
    const float cs = std::cos(turn), sn = std::sin(turn);
    sx = c.x + dx * cs - dy * sn;
    sy = c.y + dx * sn + dy * cs;
  });
  return 0;
}
const char *twirlShader = DISTORT_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float angle=values[0]*3.14159265358979/180.;if(abs(angle)<1e-9)return v;"
    "vec4 b=noBox(s);vec2 c=vec2(b.x+b.z*clamp(values[2],-100.,200.)/100.,b.y+b.w*clamp(values[3],-100.,200.)/100.);"
    "float radius=max(0.,values[1])*wavyScale;if(radius<=0.)radius=min(b.z,b.w)/2.;"
    "vec2 d=p-c;float r=sqrt(d.x*d.x+d.y*d.y);vec2 at=p;"
    "if(r<radius){float t=1.-r/radius;float turn=-angle*t*t;turn-=6.28318531*floor(turn/6.28318531);"
    "float cs=cos(turn),sn=sin(turn);at=vec2(c.x+d.x*cs-d.y*sn,c.y+d.x*sn+d.y*cs);}"
    "return noStraight(noDistort(at,1));}";
const WfxParameter twirlParameters[] = {
    number("angle", "Angle", -36000, 36000, 90, 1, "\xc2\xb0"), number("radius", "Radius (0 = to the edge)", 0, 20000, 0, 1, " px"),
    number("centerX", "Center X", -100, 200, 50, 1, " %"), number("centerY", "Center Y", -100, 200, 50, 1, " %")};
const char *const twirlHelp[] = {"How far the middle is turned; less toward Radius", "How far out the twirl reaches (0: to the layer's nearer edge)",
                                 "Where the twirl turns about, across the layer", "Where the twirl turns about, down the layer"};

// ---- Bulge --------------------------------------------------------------------------------
// The picture swelling out from a point as under a lens -- or, below 0, pinched in.
enum { BStrength, BRadius, BCentreX, BCentreY };
int bulgeInert(const float *v, int32_t n) { return std::abs(value(v, n, BStrength, 50)) < 1e-6f; }
int bulgeProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float strength = std::clamp(float(double(value(v, n, BStrength, 50)) / 100.), -.95f, .95f);
  if (std::abs(strength) < 1e-6f || width <= 0 || height <= 0) return 0;
  const Centre c = centreOf(context, width, height, value(v, n, BCentreX, 50), value(v, n, BCentreY, 50));
  float radius = float(std::max(0., double(value(v, n, BRadius, 0))) * context->scale);
  if (radius <= 0) {
    double bw = context->boxWidth, bh = context->boxHeight;
    if (!(bw > 0 && bh > 0)) { bw = width; bh = height; }
    radius = float(std::min(bw, bh) / 2.);
  }
  // A point a share t of the way out shows what lies t^p of the way out, p
  // above 1 for a bulge (closer in: magnified) and below for a pinch (further
  // out), and always above 0, so the edge stays put and nothing folds over.
  const float power = std::exp(strength * 1.2f);
  distort(pixels, width, height, stride, 1, context, [&](float px, float py, float &sx, float &sy) {
    const float dx = px - c.x, dy = py - c.y;
    const float r = std::sqrt(dx * dx + dy * dy);
    if (r >= radius || r <= 1e-4f) { sx = px; sy = py; return; }
    const float t = r / radius;
    const float k = std::pow(t, power) / t;
    sx = c.x + dx * k;
    sy = c.y + dy * k;
  });
  return 0;
}
const char *bulgeShader = DISTORT_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float strength=clamp(values[0]/100.,-.95,.95);if(abs(strength)<1e-6)return v;"
    "vec4 b=noBox(s);vec2 c=vec2(b.x+b.z*clamp(values[2],-100.,200.)/100.,b.y+b.w*clamp(values[3],-100.,200.)/100.);"
    "float radius=max(0.,values[1])*wavyScale;if(radius<=0.)radius=min(b.z,b.w)/2.;"
    "float power=exp(strength*1.2);vec2 d=p-c;float r=sqrt(d.x*d.x+d.y*d.y);vec2 at=p;"
    "if(r<radius&&r>1e-4){float t=r/radius;float k=pow(t,power)/t;at=c+d*k;}"
    "return noStraight(noDistort(at,1));}";
const WfxParameter bulgeParameters[] = {
    number("strength", "Strength", -100, 100, 50, 1, " %"), number("radius", "Radius (0 = to the edge)", 0, 20000, 0, 1, " px"),
    number("centerX", "Center X", -100, 200, 50, 1, " %"), number("centerY", "Center Y", -100, 200, 50, 1, " %")};
const char *const bulgeHelp[] = {"Above 0 swells the middle out, below 0 pinches it in", "How far out the bulge reaches (0: to the layer's nearer edge)",
                                 "Where the bulge is, across the layer", "Where the bulge is, down the layer"};

// ---- Chromatic Aberration ----------------------------------------------------------------
// The colours of a cheap lens failing to meet: red pushed out from a point and
// blue drawn in (or the other way round), more the further from it, so every
// edge away from the middle grows a coloured fringe. Amount is how far apart
// the two are at the layer's corners.
enum { CAAmount, CACentreX, CACentreY, CASwap };
int aberrationInert(const float *v, int32_t n) { return std::abs(std::clamp(double(value(v, n, CAAmount, 8)), -500., 500.)) < 1e-6; }
double aberrationReach(const float *v, int32_t n, double) {
  const double amount = std::abs(std::clamp(double(value(v, n, CAAmount, 8)), -500., 500.));
  return amount > 0 ? std::ceil(amount / 8.) * 8. : 0;
}
int aberrationProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n,
                      const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  float amount = float(std::clamp(double(value(v, n, CAAmount, 8)), -500., 500.) * context->scale);
  if (std::abs(amount) < 1e-6f || width <= 0 || height <= 0) return 0;
  if (value(v, n, CASwap, 0) > .5f) amount = -amount;
  const Centre c = centreOf(context, width, height, value(v, n, CACentreX, 50), value(v, n, CACentreY, 50));
  // How far the furthest corner of the box is from the middle: there the two
  // colours stand `amount` apart, half each way.
  double bx = context->boxX, by = context->boxY, bw = context->boxWidth, bh = context->boxHeight;
  if (!(bw > 0 && bh > 0)) { bx = 0; by = 0; bw = width; bh = height; }
  float far = 1.f;
  const double corners[4][2] = {{bx, by}, {bx + bw, by}, {bx, by + bh}, {bx + bw, by + bh}};
  for (const auto &corner : corners) far = std::max(far, float(std::hypot(corner[0] - c.x, corner[1] - c.y)));
  const float spread = amount / 2.f / far; // a share of the distance from the middle
  Scratch kept(size_t(width) * size_t(height) * 4);
  float *const source = kept.data();
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y)
      std::memcpy(&source[size_t(y) * size_t(width) * 4], row(pixels, stride, y), size_t(width) * 16);
  });
  // One channel (and its coverage) read between pixels. Beyond the picture its
  // edge goes on: a layer's own raster ends in clear room anyway, and a whole
  // frame (an Effect Layer) would otherwise lose a channel round its border.
  auto sample = [&](float sx, float sy, int channel, float &value, float &cover) {
    sx -= .5f; sy -= .5f;
    const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
    const float fx = sx - float(x0), fy = sy - float(y0);
    value = 0; cover = 0;
    const float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    const int xs[4] = {x0, x0 + 1, x0, x0 + 1}, ys[4] = {y0, y0, y0 + 1, y0 + 1};
    for (int k = 0; k < 4; ++k) {
      const int tx = std::clamp(xs[k], 0, width - 1), ty = std::clamp(ys[k], 0, height - 1);
      const float *t = &source[(size_t(ty) * size_t(width) + size_t(tx)) * 4];
      value += t[channel] * w[k];
      cover += t[3] * w[k];
    }
  };
  forRows(context, height, [&](int32_t begin, int32_t end) {
    for (int32_t y = begin; y < end; ++y) {
      float *d = row(pixels, stride, y);
      const float py = float(y) + .5f;
      for (int32_t x = 0; x < width; ++x) {
        const float px = float(x) + .5f;
        const float ox = (px - c.x) * spread, oy = (py - c.y) * spread;
        float red, redCover, blue, blueCover;
        // Red comes from nearer the middle (so it lands further out), blue from further.
        sample(px - ox, py - oy, 0, red, redCover);
        sample(px + ox, py + oy, 2, blue, blueCover);
        const float *self = &source[(size_t(y) * size_t(width) + size_t(x)) * 4];
        d[x * 4] = red;
        d[x * 4 + 1] = self[1];
        d[x * 4 + 2] = blue;
        d[x * 4 + 3] = std::max({self[3], redCover, blueCover});
      }
    }
  });
  return 0;
}
const char *aberrationShader = DISTORT_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float amount=clamp(values[0],-500.,500.)*wavyScale;if(abs(amount)<1e-6)return v;if(values[3]>.5)amount=-amount;"
    "vec4 b=noBox(s);vec2 c=vec2(b.x+b.z*clamp(values[1],-100.,200.)/100.,b.y+b.w*clamp(values[2],-100.,200.)/100.);"
    "float far=1.;far=max(far,length(b.xy-c));far=max(far,length(vec2(b.x+b.z,b.y)-c));"
    "far=max(far,length(vec2(b.x,b.y+b.w)-c));far=max(far,length(b.xy+b.zw-c));"
    "float spread=amount/2./far;vec2 o=(p-c)*spread;"
    "vec4 red=noDistort(p-o,1),blue=noDistort(p+o,1);vec4 self=vec4(v.rgb*v.a,v.a);"
    "return noStraight(vec4(red.r,self.g,blue.b,max(self.a,max(red.a,blue.a))));}";
const WfxParameter aberrationParameters[] = {
    number("amount", "Amount", -500, 500, 8, .5f, " px"), number("centerX", "Center X", -100, 200, 50, 1, " %"),
    number("centerY", "Center Y", -100, 200, 50, 1, " %"), toggle("swap", "Blue Outside", false)};
const char *const aberrationHelp[] = {
    "How far apart red and blue are at the layer's corners, in layer pixels", "Where the lens's middle is, across the layer",
    "Where the lens's middle is, down the layer", "Blue fringes outside and red inside, instead of the other way round"};

// ---- Polar Coordinates ----------------------------------------------------------------
// The layer's box bent round its middle: Rect to Polar rolls the picture into a
// ring (its top edge gathered at the middle, its bottom edge round the outside,
// left to right going clockwise from the top), Polar to Rect unrolls a ring
// into a strip. Amount goes part of the way, to key the bend.
enum { PolarAmount, PolarType, PolarEdge };
int polarInert(const float *v, int32_t n) { return std::clamp(divided(v, n, PolarAmount, 100, 100.), 0.f, 1.f) <= 0; }
int polarProcess(void *pixels, int32_t width, int32_t height, int32_t stride, const float *v, int32_t n, const WfxContext *context) {
  if (!context || context->depth != 32) return 1;
  const float amount = std::clamp(divided(v, n, PolarAmount, 100, 100.), 0.f, 1.f);
  if (amount <= 0.f || width <= 0 || height <= 0) return 0;
  double bx = context->boxX, by = context->boxY, bw = context->boxWidth, bh = context->boxHeight;
  if (!(bw > 0 && bh > 0)) { bx = 0; by = 0; bw = width; bh = height; }
  const float left = float(bx), top = float(by), w = float(bw), h = float(bh);
  const float cx = left + w / 2, cy = top + h / 2, outer = std::min(w, h) / 2;
  const bool toPolar = whole(v, n, PolarType, 0) == 0;
  const float tau = 6.28318531f;
  distort(pixels, width, height, stride, std::clamp(whole(v, n, PolarEdge, 2), 0, 2), context,
          [&](float px, float py, float &sx, float &sy) {
    float tx, ty;
    if (toPolar) {
      // Where this point is round the middle and how far out, read off the strip.
      const float dx = px - cx, dy = py - cy;
      float turn = std::atan2(dx, -dy) / tau;
      if (turn < 0) turn += 1;
      tx = left + turn * w;
      ty = top + std::sqrt(dx * dx + dy * dy) / outer * h;
    } else {
      // Across the strip is round the ring, down it is out from the middle.
      const float turn = (px - left) / w * tau, r = (py - top) / h * outer;
      tx = cx + r * std::sin(turn);
      ty = cy - r * std::cos(turn);
    }
    sx = px + (tx - px) * amount;
    sy = py + (ty - py) * amount;
  });
  return 0;
}
const char *polarShader = DISTORT_GLSL
    "vec4 wavyEffect(vec4 v,vec2 p,vec2 s){"
    "float amount=clamp(values[0]/100.,0.,1.);if(amount<=0.)return v;vec4 b=noBox(s);"
    "float left=b.x,top=b.y,w=b.z,h=b.w,cx=left+w/2.,cy=top+h/2.,outer=min(w,h)/2.;"
    "bool toPolar=noWhole(values[1])==0;int edge=clamp(noWhole(values[2]),0,2);float tau=6.28318531;float tx,ty;"
    "if(toPolar){float dx=p.x-cx,dy=p.y-cy;"
    // atan2(+0, -0) is a half turn on the CPU; the card's atan is not defined there.
    "float turn=(dx==0.&&dy==0.)?.5:atan(dx,-dy)/tau;if(turn<0.)turn+=1.;"
    "tx=left+turn*w;ty=top+sqrt(dx*dx+dy*dy)/outer*h;}"
    "else{float turn=(p.x-left)/w*tau,r=(p.y-top)/h*outer;tx=cx+r*sin(turn);ty=cy-r*cos(turn);}"
    "return noStraight(noDistort(vec2(p.x+(tx-p.x)*amount,p.y+(ty-p.y)*amount),edge));}";
const WfxParameter polarParameters[] = {
    number("amount", "Amount", 0, 100, 100, 1, " %"), choice("type", "Type", "Rect to Polar\nPolar to Rect", 2, 0),
    choice("edge", "Edges", "Transparent\nClamp\nWrap", 3, 2)};
const char *const polarHelp[] = {"How far the picture is bent (key it to roll the layer up)",
                                 "Roll the layer into a ring, or a ring out into a strip",
                                 "What shows where the bend reads from beyond the layer"};

#define COUNT(parameters) int32_t(sizeof(parameters) / sizeof(parameters[0])), parameters
const WfxEffect effects[] = {
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Noise", "Noise", "Noise & Distort", 4u, uint32_t(WFX_MOVES_ALONE),
     COUNT(noiseParameters), noiseProcess, noiseInert, noiseVaries, nullptr, nullptr, nullptr, nullptr, noiseShader},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Turbulent Noise", "Turbulent Noise", "Noise & Distort", 4u, uint32_t(WFX_MOVES_ALONE),
     COUNT(turbulentParameters), turbulentProcess, turbulentInert, turbulentVaries, nullptr, nullptr, nullptr, nullptr, turbulentShader,
     nullptr, nullptr, nullptr, nullptr, nullptr, turbulentShaderPasses},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Displacement Map", "Displacement Map", "Noise & Distort", 4u, uint32_t(WFX_MOVES_ALONE | WFX_PREMULTIPLIED),
     COUNT(displaceParameters), displaceProcess, displaceInert, displaceVaries, displaceReach, nullptr, nullptr, nullptr, displaceShader,
     nullptr, nullptr, nullptr, nullptr, nullptr, displaceShaderPasses},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Wave Warp", "Wave Warp", "Noise & Distort", 4u, uint32_t(WFX_MOVES_ALONE | WFX_PREMULTIPLIED),
     COUNT(waveParameters), waveProcess, waveInert, waveVaries, waveReach, nullptr, nullptr, nullptr, waveShader,
     nullptr, nullptr, nullptr, waveHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Ripple", "Ripple", "Noise & Distort", 4u, uint32_t(WFX_MOVES_ALONE | WFX_PREMULTIPLIED),
     COUNT(rippleParameters), rippleProcess, rippleInert, rippleVaries, rippleReach, nullptr, nullptr, nullptr, rippleShader,
     nullptr, nullptr, nullptr, rippleHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Twirl", "Twirl", "Noise & Distort", 4u, uint32_t(WFX_PREMULTIPLIED),
     COUNT(twirlParameters), twirlProcess, twirlInert, nullptr, nullptr, nullptr, nullptr, nullptr, twirlShader,
     nullptr, nullptr, nullptr, twirlHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Bulge", "Bulge", "Noise & Distort", 4u, uint32_t(WFX_PREMULTIPLIED),
     COUNT(bulgeParameters), bulgeProcess, bulgeInert, nullptr, nullptr, nullptr, nullptr, nullptr, bulgeShader,
     nullptr, nullptr, nullptr, bulgeHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Chromatic Aberration", "Chromatic Aberration", "Stylize", 4u, uint32_t(WFX_PREMULTIPLIED),
     COUNT(aberrationParameters), aberrationProcess, aberrationInert, nullptr, aberrationReach, nullptr, nullptr, nullptr, aberrationShader,
     nullptr, nullptr, nullptr, aberrationHelp, nullptr},
    {sizeof(WfxEffect), WFX_ABI_VERSION, "Polar Coordinates", "Polar Coordinates", "Noise & Distort", 4u, uint32_t(WFX_PREMULTIPLIED),
     COUNT(polarParameters), polarProcess, polarInert, nullptr, nullptr, nullptr, nullptr, nullptr, polarShader,
     nullptr, nullptr, nullptr, polarHelp, nullptr}};
}  // namespace

WFX_EXPORT int32_t wfxEffectCount(void) { return int32_t(sizeof(effects) / sizeof(effects[0])); }
WFX_EXPORT const WfxEffect *wfxEffectAt(int32_t index) {
  return index >= 0 && index < wfxEffectCount() ? &effects[index] : nullptr;
}
