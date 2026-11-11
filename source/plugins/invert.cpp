#include "wavy_plugin.h"
#include "wavy_graph.h"
#include <algorithm>
static int available() { return 1; }
static int describeInvert(const WavyGraphRequest *request,const WavyGraphSink *sink) {
  if(!request || request->abiVersion!=3 || !sink || !sink->append)return 1;
  WavyOperation op{};op.structSize=sizeof(op);op.code=WAVY_INVERT;
  op.values[0]=std::clamp(request->amount,0.f,1.f);
  return sink->append(sink->context,&op);
}
WAVY_EXPORT const WavyEffectGraphV3 *wavyGetEffectGraphV3() {
  static WavyEffectGraphV3 graph{sizeof(WavyEffectGraphV3),3,describeInvert};return &graph;
}
static int process(uint8_t *rgba, int w, int h, int stride, float amount) {
  if (!rgba || w < 1 || h < 1 || stride < w * 4)
    return 1;
  amount = std::clamp(amount, 0.f, 1.f);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      for (int c = 0; c < 3; c++) {
        auto &v = rgba[y * stride + x * 4 + c];
        v = uint8_t(v * (1 - amount) + (255 - v) * amount + .5f);
      }
  return 0;
}
WAVY_EXPORT const WavyEffectV1 *wavyGetEffectV1() {
  static WavyEffectV1 api{1, "Invert (sample C++ plugin)", available, process};
  return &api;
}
static int processPrecision(void *pixels, int w, int h, int stride, int depth, float amount) {
  if(!pixels || w<1 || h<1 || (depth!=16 && depth!=32) || stride < w*(depth==16?8:16)) return 1;
  amount=std::clamp(amount,0.f,1.f);
  for(int y=0;y<h;++y) {
    auto *row=static_cast<uint8_t *>(pixels)+size_t(y)*stride;
    for(int x=0;x<w;++x) for(int c=0;c<3;++c) {
      if(depth==16) {auto &v=reinterpret_cast<uint16_t *>(row)[x*4+c]; v=uint16_t(v*(1-amount)+(65535-v)*amount+.5f);}
      else {auto &v=reinterpret_cast<float *>(row)[x*4+c]; v=v*(1-amount)+(1-v)*amount;}
    }
  }
  return 0;
}
WAVY_EXPORT const WavyEffectV2 *wavyGetEffectV2() {
  static WavyEffectV2 api{2,6,processPrecision}; return &api;
}
// Pointwise: an output pixel depends on the input pixel in its own place and
// nothing else, so the host can hand each core a slice of the real buffer with
// nothing copied either way. Nothing above mentions threads.
WAVY_EXPORT const WavyParallelV1 *wavyGetParallelV1() {
  static WavyParallelV1 split{sizeof(WavyParallelV1),1,1,0}; return &split;
}
