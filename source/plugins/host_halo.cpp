#include "wavy_plugin.h"
#include "wavy_graph.h"
// No Qt, pixel loops, CPU threading or graphics API in this plugin.
static int describe(const WavyGraphRequest *request,const WavyGraphSink *sink) {
  if(!request || request->abiVersion!=3 || !sink || !sink->append)return 1;
  WavyOperation op{};op.structSize=sizeof(op);op.code=WAVY_FILL;
  op.values[0]=.15f;op.values[1]=.5f;op.values[2]=1;op.values[3]=1;
  if(sink->append(sink->context,&op))return 1;
  op={};op.structSize=sizeof(op);op.code=WAVY_HALO;
  op.values[0]=12;op.values[1]=.15f;op.values[2]=.5f;op.values[3]=1;op.values[4]=.7f;
  return sink->append(sink->context,&op);
}
WAVY_EXPORT const WavyEffectGraphV3 *wavyGetEffectGraphV3() {
  static WavyEffectGraphV3 api{sizeof(WavyEffectGraphV3),3,describe,"Host Halo (SDK V3 sample)"};
  return &api;
}
