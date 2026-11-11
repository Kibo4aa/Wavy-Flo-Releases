# Wavy-Flo host-operation SDK V3

This is a draft C++17 ABI for ordered host-managed effects. The plugin describes
operations; Wavy-Flo owns pixels, allocation, multicore scheduling and GPU
submission. No Qt, STL object or graphics handle crosses the ABI. The header
changed during this development: rebuild all V3 plugins against the current
wavy_graph.h. It is not a stable binary release yet.

## Writing a plugin

Export `wavyGetEffectGraphV3` with C linkage. Return a static descriptor containing
`sizeof(WavyEffectGraphV3), 3, describe, "Your name"`. The static UTF-8 name is
required for a V3-only plugin. `plugins/host_halo.cpp` is a complete V3-only
Fill -> Halo example with no pixel loops or graphics code. `plugins/invert.cpp`
exports V1, V2 and V3 to demonstrate backwards compatibility. Old hosts require
the optional V1/V2 exports; the current host prefers V3 when valid.

The describe callback receives width, height, depth (8/16/32) and amount.
Zero-initialize WavyOperation, set structSize and append through the sink.
Check each return value. The host copies operations immediately. Return zero
only on success; retain no pointers and perform no device or pixel operations.
Descriptors, names and callbacks must live until DLL unload. Description must
be deterministic for the request and must not depend on invocation count.

## Available operations

Parameters are `values[index]` in the header, in the order below:

| Operation | Parameters |
|---|---|
| RGB_AFFINE | gain RGB [0..2], bias RGB [3..5] |
| EXPOSURE | stops [0], -32..32 |
| INVERT | amount [0], 0..1 |
| BLUR | radius [0], 0..8192 |
| HALO (Glow/Shadow) | radius [0], RGB [1..3], alpha [4], integer offset XY [5..6] |
| STROKE | width [0] 0..200, mode [1] Inside=0/Outside=1/Center=2, RGBA [2..5] |
| FILL | RGBA [0..3] |
| GRADIENT | start RGB [0..2], end RGB [3..5], amount [6], angle [7], radial flag [8], centre XY [9..10], extent WH [11..12] |

Colours/amounts/alpha are 0..1. Coordinates and radii are pixels in the submitted
image, not composition coordinates. The image is a fixed canvas: outside pixels
are transparent and output is clipped to the same dimensions. A plugin must
use request dimensions to implement relative sizing. Halo with zero offset is
Glow; a displaced Halo is Shadow. Blur is three radius-ceil(R/2) box passes per
axis, a Gaussian approximation. Radius zero is identity blur. Stroke uses the
host's alpha>=0.5 Euclidean contour; soft-alpha source limitations still apply.
Fill replaces RGB and multiplies coverage by its alpha. Gradient preserves alpha.

The host validates finite values, ranges, integer flags, struct sizes and a
256-operation limit before modifying output. Unsupported operations fail the
whole graph. Native plugins remain trusted in-process code, not sandboxed code.

## Execution and quality

Every operation consumes the preceding result. Adjacent affine-compatible
operations, including Exposure and Invert, fuse into one pass. Spatial operations
preserve order and prevent fusion across them. No intermediate integer clamp is
inserted. Float working RGB can be negative or exceed 1; alpha is preserved unless
an operation explicitly changes coverage. Final integer conversion quantizes and
clamps. The source working space is used without implicit colour linearization.

CPU mode uses the multicore pool and scoped suppression of GPU work. GPU mode
submits supported chains once, using full-size textures throughout, with one
upload/readback. Blur box radii above 32, and exact Stroke, currently select CPU
for the entire chain to avoid transfers between every operation. GPU failure
also falls back to CPU. This SDK path never invokes coarse blur/downscaling.
Built-in Effect List optimizations remain separate and may use their existing
coarse wide-blur path. GPU mode does not mean every operation runs on GPU.

## Integration

Plugins are discovered from the executable's effects directory. The existing
external plugin selector applies them to the completed composition for preview
and export. It currently passes amount=1. V3-only plugins are accepted without a
V1 process function; V1/V2 plugins keep their old path.

This is not yet a per-layer Effect List registry, custom parameter UI, multi-input
DAG, node-cache service or arbitrary shader ABI. External-plugin preview remains
synchronous; final plugin images are not cached by the SDK. Graphs fuse inside
one plugin invocation, not across all plugins/layers. Existing arbitrary C++ CPU
code cannot be automatically converted into GPU code. No 1-3 ms guarantee is made.

## Build and tests

Existing CMake targets build WavyInvert and the new WavyHostHalo library into
`build-msvc1444/effects`. Copy the built DLL into `effects` beside the installed
Wavy-Flo.exe to use it. This change does not automatically install DLLs.

`--effect-graph-self-test` covers fusion order, HDR/alpha, 8/16/32-bit images,
Fill -> Halo, exact wide blur, invalid graphs and limits.
`--effect-graph-gpu-self-test` compares a complete Exposure -> Halo -> Blur chain
against CPU, including HDR and alpha; returns 77 if GPU is unavailable.
CTest registers both tests. No application build/runtime tests or benchmark was
run during this update, following the user's preference to build themselves.
