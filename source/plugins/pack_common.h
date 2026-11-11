#pragma once
// Small helpers shared by Wavy-Flo's own effect packs. Header-only and free of
// any toolkit, like the packs themselves.
#include "wavy_effect.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace wfxpack {

// Declarations, written the way they read in the panel.
inline WfxParameter number(const char *id, const char *label, float minimum, float maximum, float initial, float step,
                           const char *unit = nullptr, const char *section = nullptr) {
  return WfxParameter{sizeof(WfxParameter), WFX_FLOAT, id, label, minimum, maximum, initial, step,
                      nullptr, unit, section, 0u, {0, 0, 0, 0}};
}
inline WfxParameter toggle(const char *id, const char *label, bool initial, const char *section = nullptr) {
  return WfxParameter{sizeof(WfxParameter), WFX_BOOL, id, label, 0, 1, initial ? 1.f : 0.f, 1,
                      nullptr, nullptr, section, 0u, {0, 0, 0, 0}};
}
inline WfxParameter choice(const char *id, const char *label, const char *choices, int count, int initial) {
  return WfxParameter{sizeof(WfxParameter), WFX_CHOICE, id, label, 0, float(count - 1), float(initial), 1,
                      choices, nullptr, nullptr, 0u, {0, 0, 0, 0}};
}
// A twirl in Effect Setting holding the controls up to its groupEnd() (SDK 7).
inline WfxParameter group(const char *label, bool collapsed = false) {
  return WfxParameter{sizeof(WfxParameter), WFX_GROUP, nullptr, label, 0, 0, 0, 0,
                      nullptr, nullptr, nullptr, collapsed ? uint32_t(WFX_PARAM_COLLAPSED) : 0u, {0, 0, 0, 0}};
}
inline WfxParameter groupEnd() {
  return WfxParameter{sizeof(WfxParameter), WFX_GROUP_END, nullptr, "", 0, 0, 0, 0, nullptr, nullptr, nullptr, 0u, {0, 0, 0, 0}};
}
inline WfxParameter color(const char *id, const char *label, float r, float g, float b, float a) {
  return WfxParameter{sizeof(WfxParameter), WFX_COLOR, id, label, 0, 1, 0, 0,
                      nullptr, nullptr, nullptr, 0u, {r, g, b, a}};
}
// A colour chosen on a wheel, starting at the middle grey the wheels rest on.
inline WfxParameter wheel(const char *id, const char *label) {
  const float grey = 128.f / 255.f;
  return WfxParameter{sizeof(WfxParameter), WFX_COLOR, id, label, 0, 1, 0, 0,
                      nullptr, nullptr, nullptr, uint32_t(WFX_PARAM_WHEEL), {grey, grey, grey, 1}};
}
inline WfxParameter curve(const char *id, const char *label) {
  return WfxParameter{sizeof(WfxParameter), WFX_CURVE, id, label, 0, 1, 0, 0,
                      nullptr, nullptr, nullptr, 0u, {0, 0, 0, 0}};
}
inline WfxParameter layer(const char *id, const char *label, const char *section = nullptr) {
  return WfxParameter{sizeof(WfxParameter), WFX_LAYER, id, label, -1.f, 10000.f, -1.f, 1.f,
                      nullptr, nullptr, section, uint32_t(WFX_PARAM_STILL), {0, 0, 0, 0}};
}
inline WfxParameter mask(const char *id, const char *label, bool allowAll = false, const char *section = nullptr) {
  uint32_t flags = uint32_t(WFX_PARAM_STILL) | (allowAll ? uint32_t(WFX_PARAM_ALL_MASKS) : 0u);
  return WfxParameter{sizeof(WfxParameter), WFX_MASK, id, label, allowAll ? -2.f : -1.f, 10000.f, -1.f, 1.f,
                      nullptr, nullptr, section, flags, {0, 0, 0, 0}};
}
inline WfxParameter point(const char *id, const char *label, float defaultX = 0.f, float defaultY = 0.f, const char *section = nullptr) {
  return WfxParameter{sizeof(WfxParameter), WFX_POINT, id, label, -100000.f, 100000.f, 0.f, 1.f,
                      nullptr, " px", section, uint32_t(WFX_PARAM_STILL), {defaultX, defaultY, 0.f, 0.f}};
}
inline WfxParameter point3d(const char *id, const char *label, float defaultX = 0.f, float defaultY = 0.f, float defaultZ = 0.f, const char *section = nullptr) {
  return WfxParameter{sizeof(WfxParameter), WFX_POINT_3D, id, label, -100000.f, 100000.f, 0.f, 1.f,
                      nullptr, " px", section, uint32_t(WFX_PARAM_STILL), {defaultX, defaultY, defaultZ, 0.f}};
}
inline WfxParameter angle(const char *id, const char *label, float initial = 0.f, const char *section = nullptr) {
  return WfxParameter{sizeof(WfxParameter), WFX_FLOAT, id, label, -360000.f, 360000.f, initial, 1.f,
                      nullptr, "\xc2\xb0", section, uint32_t(WFX_PARAM_ANGLE), {0, 0, 0, 0}};
}

// A value by position, or its default when the host sent fewer.
inline float value(const float *values, int32_t count, int index, float fallback) {
  if (!values || index < 0 || index >= count) return fallback;
  const float v = values[index];
  return std::isfinite(v) ? v : fallback;
}
// The same, divided in double precision, the way the effects always divided a
// saved setting: 40 / 100. rounds to a float once, not twice.
inline float divided(const float *values, int32_t count, int index, float fallback, double by) {
  return float(double(value(values, count, index, fallback)) / by);
}

// Where the pixels handed over lie in the layer's raster, and the raster's
// size (WfxContext::originX and on): what a WFX_PER_PIXEL effect handed one
// strip of the raster measures positions by. The whole buffer is the raster
// for a host that does not say.
struct RasterPlace { int32_t x, y, width, height; };
inline RasterPlace rasterPlace(const WfxContext *context, int32_t width, int32_t height) {
  if (context && context->structSize >= offsetof(WfxContext, rasterHeight) + sizeof(int32_t) &&
      context->rasterWidth > 0 && context->rasterHeight > 0)
    return {context->originX, context->originY, context->rasterWidth, context->rasterHeight};
  return {0, 0, width, height};
}

// Rows split across the host's workers.
inline void forRows(const WfxContext *context, int32_t rows, void *user,
                    void (*job)(int32_t begin, int32_t end, void *user)) {
  if (context && context->parallelFor) context->parallelFor(rows, job, user);
  else job(0, rows, user);
}
// The same for a lambda. The lambda lives on the caller's stack until every
// worker has returned, which parallelFor promises.
template <class F> void forRows(const WfxContext *context, int32_t rows, const F &f) {
  forRows(context, rows, const_cast<F *>(&f), [](int32_t begin, int32_t end, void *user) {
    (*static_cast<const F *>(user))(begin, end);
  });
}

// A picture-sized buffer, handed back for the next frame when done with. A
// fresh 1080p float buffer costs more in the system's first touch of its
// pages than a blur's whole pass does; kept, the next frame finds it ready.
// Its contents are whatever was left in it.
class Scratch {
 public:
  explicit Scratch(size_t count) : count_(count) {
    if (count == 0) return;
    std::lock_guard<std::mutex> lock(guard());
    auto &kept = pool();
    auto best = kept.end();
    for (auto it = kept.begin(); it != kept.end(); ++it)
      if (it->second >= count && (best == kept.end() || it->second < best->second)) best = it;
    if (best != kept.end()) {
      data_ = std::move(best->first);
      size_ = best->second;
      kept.erase(best);
    }
    if (!data_) { data_.reset(new float[count]); size_ = count; }
  }
  ~Scratch() {
    if (!data_) return;
    std::lock_guard<std::mutex> lock(guard());
    auto &kept = pool();
    kept.emplace_back(std::move(data_), size_);
    // A few pictures' worth at most; the smallest go first.
    size_t total = 0;
    for (const auto &k : kept) total += k.second;
    while (kept.size() > 4 || total > size_t(96) * 1024 * 1024) {
      auto smallest = kept.begin();
      for (auto it = kept.begin(); it != kept.end(); ++it)
        if (it->second < smallest->second) smallest = it;
      total -= smallest->second;
      kept.erase(smallest);
    }
  }
  Scratch(const Scratch &) = delete;
  Scratch &operator=(const Scratch &) = delete;
  float *data() const { return data_.get(); }
  size_t size() const { return count_; }

 private:
  using Kept = std::vector<std::pair<std::unique_ptr<float[]>, size_t>>;
  static std::mutex &guard() { static std::mutex m; return m; }
  static Kept &pool() { static Kept kept; return kept; }
  std::unique_ptr<float[]> data_;
  size_t size_ = 0, count_ = 0;
};

}  // namespace wfxpack
