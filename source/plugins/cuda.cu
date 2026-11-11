#include "wavy_plugin.h"
#include <cmath>
#include <cuda_runtime.h>
__global__ void exposureKernel(unsigned char *p, int width, int height,
                               int stride, float gain) {
  int x = blockIdx.x * blockDim.x + threadIdx.x,
      y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height)
    return;
  for (int c = 0; c < 3; c++) {
    int i = y * stride + x * 4 + c;
    p[i] = (unsigned char)(fminf(255.f, fmaxf(0.f, p[i] * gain)) + .5f);
  }
}
static int available() {
  int n = 0;
  return cudaGetDeviceCount(&n) == cudaSuccess && n > 0;
}
static int process(uint8_t *rgba, int w, int h, int stride, float stops) {
  if (!rgba || w < 1 || h < 1 || stride < w * 4)
    return 1;
  unsigned char *device = nullptr;
  size_t bytes = size_t(stride) * h;
  if (cudaMalloc(&device, bytes) != cudaSuccess)
    return 2;
  cudaError_t result = cudaMemcpy(device, rgba, bytes, cudaMemcpyHostToDevice);
  if (result == cudaSuccess) {
    exposureKernel<<<dim3((w + 15) / 16, (h + 15) / 16), dim3(16, 16)>>>(
        device, w, h, stride, exp2f(stops));
    result = cudaGetLastError();
    if (result == cudaSuccess)
      result = cudaMemcpy(rgba, device, bytes, cudaMemcpyDeviceToHost);
  }
  cudaFree(device);
  return result == cudaSuccess ? 0 : 3;
}
WAVY_EXPORT const WavyEffectV1 *wavyGetEffectV1() {
  static WavyEffectV1 api{1, "CUDA exposure", available, process};
  return &api;
}
