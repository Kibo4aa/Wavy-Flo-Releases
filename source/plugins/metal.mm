#include "wavy_plugin.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstring>
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLComputePipelineState> pipeline;
static int available() {
  @autoreleasepool {
    if (pipeline)
      return 1;
    device = MTLCreateSystemDefaultDevice();
    if (!device)
      return 0;
    NSError *error = nil;
    NSString *source =
        @"#include <metal_stdlib>\nusing namespace metal;\nkernel void "
        @"grade(device uchar* p [[buffer(0)]],constant uint4& info "
        @"[[buffer(1)]],constant float& gain [[buffer(2)]],uint2 q "
        @"[[thread_position_in_grid]]){if(q.x>=info.x||q.y>=info.y)return;for("
        @"uint c=0;c<3;c++){uint "
        @"i=q.y*info.z+q.x*4+c;p[i]=uchar(clamp(float(p[i])*gain,0.0f,255.0f)+"
        @"0.5f);}}";
    id<MTLLibrary> library = [device newLibraryWithSource:source
                                                  options:nil
                                                    error:&error];
    if (!library)
      return 0;
    pipeline = [device
        newComputePipelineStateWithFunction:[library
                                                newFunctionWithName:@"grade"]
                                      error:&error];
    queue = [device newCommandQueue];
    return pipeline && queue;
  }
}
static int process(uint8_t *rgba, int w, int h, int stride, float stops) {
  @autoreleasepool {
    if (!rgba || w < 1 || h < 1 || stride < w * 4 || !available())
      return 1;
    size_t bytes = size_t(stride) * h;
    id<MTLBuffer> buffer =
        [device newBufferWithBytes:rgba
                            length:bytes
                           options:MTLResourceStorageModeShared];
    if (!buffer)
      return 2;
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmd computeCommandEncoder];
    uint32_t info[4] = {(uint32_t)w, (uint32_t)h, (uint32_t)stride, 0};
    float gain = std::exp2(stops);
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:buffer offset:0 atIndex:0];
    [encoder setBytes:info length:sizeof(info) atIndex:1];
    [encoder setBytes:&gain length:sizeof(gain) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(w, h, 1)
        threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [encoder endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    if (cmd.status != MTLCommandBufferStatusCompleted)
      return 3;
    std::memcpy(rgba, buffer.contents, bytes);
    return 0;
  }
}
WAVY_EXPORT const WavyEffectV1 *wavyGetEffectV1() {
  static WavyEffectV1 api{1, "Metal exposure", available, process};
  return &api;
}
