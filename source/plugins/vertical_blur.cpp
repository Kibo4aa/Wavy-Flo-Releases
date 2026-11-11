#include "wavy_plugin.h"
#include <algorithm>
#include <cstring>
#include <vector>

// A sample plugin that reads outside the row it writes, which is the case the
// host cannot split blindly. It declares how far it reaches; the host gives
// each band those extra rows from an untouched copy and takes back only the
// rows the band owns, so the picture is the same whether it ran on one core or
// twenty. Nothing here mentions threads.
static const int kReach = 8;

static int available() { return 1; }

static int process(uint8_t *rgba, int width, int height, int stride, float amount) {
  if (!rgba || width < 1 || height < 1 || stride < width * 4) return 1;
  amount = std::clamp(amount, 0.f, 1.f);
  const int reach = std::max(1, int(kReach * amount + .5f));
  // The obligation that comes with declaring a reach: read from a copy, so a
  // row this function has already written can never change what it reads next.
  // Without it the answer would depend on where a band happened to begin, and
  // the seams would show.
  std::vector<uint8_t> source(size_t(height) * size_t(width) * 4);
  for (int y = 0; y < height; ++y)
    memcpy(source.data() + size_t(y) * width * 4, rgba + size_t(y) * stride, size_t(width) * 4);

  for (int y = 0; y < height; ++y) {
    uint8_t *out = rgba + size_t(y) * stride;
    for (int x = 0; x < width; ++x)
      for (int c = 0; c < 4; ++c) {
        int total = 0, taken = 0;
        for (int dy = -reach; dy <= reach; ++dy) {
          const int at = y + dy;
          if (at < 0 || at >= height) continue;
          total += source[(size_t(at) * width + x) * 4 + c];
          ++taken;
        }
        out[x * 4 + c] = uint8_t(total / std::max(1, taken));
      }
  }
  return 0;
}

WAVY_EXPORT const WavyEffectV1 *wavyGetEffectV1() {
  static WavyEffectV1 api{1, "Vertical Blur (SDK reach sample)", available, process};
  return &api;
}

// Reentrant: process() keeps nothing between calls and touches no global that
// changes. rowReach: it reads up to kReach rows either side of the row it
// writes. That pair is all the host needs to spread this across every core.
WAVY_EXPORT const WavyParallelV1 *wavyGetParallelV1() {
  static WavyParallelV1 split{sizeof(WavyParallelV1), 1, 1, kReach};
  return &split;
}
