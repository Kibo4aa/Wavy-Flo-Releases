# Optional plugin shaders

Export `wavyGetShaderV1` alongside the V1 discovery entry point and V4 CPU
implementation. The descriptor holds a GLSL 3.30 function:

```glsl
vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size) {
    return vec4(colour.rgb * values[0], colour.a);
}
```

Wavy-Flo owns compilation, contexts, texture allocation and transfer. This is
a pointwise interface; use the V3 graph for built-in spatial operations such as
blur and halo. It does not translate arbitrary C++ into shaders automatically.

The host declares these inputs:

- `colour`: straight-alpha RGBA. RGB can exceed 1 or be negative at 32 bpc.
- `coord`: pixel centre `(x + 0.5, row + 0.5)`; row zero is the first/top CPU row.
- `size`: current input raster dimensions, not composition dimensions.
- `values[256]`: V4 parameters in declaration order; a colour occupies RGBA,
  every other kind occupies one float. Unused entries are zero.
- `processingDepth`: 8, 16 or 32. Preserve HDR at 32 if the CPU version does.

Do not declare a GLSL version, uniforms or fragment output. Return the same
straight-alpha result as `process()`. `plugins/tint.cpp` implements all three
depths, all three blend modes and a parallel CPU declaration.

## Validation and selection

Shaders are validated on the GUI GL thread at first eligible use, separately
for each depth. Two asymmetric patches cover transparency, colour ramps and,
at 32 bpc, negative and above-one RGB. The host exercises declared defaults,
parameter endpoints and ordinary choice entries. Validation results and shader
programs are retained for the session. A mismatch at 32 bpc does not disable an
otherwise matching 8-bit route. Finite tests are a regression guard, not proof
of equivalence for all inputs or parameter combinations.

The comparison allows 2 levels of 255 at 8 bpc, 4 of 65535 at 16 bpc, and
`0.0001 * max(1, abs(CPU channel))` at 32 bpc for floating-point arithmetic.
RGBA16 is uploaded/read back directly as unsigned 16-bit channels, avoiding
an intermediate Qt float conversion and its rounding at fractional alpha.

The CPU is always available. Its first run measures the whole-image kernel;
the second measures the learned worker strategy. GPU execution is then measured
including upload/readback. Decisions are separate per plugin, depth and image
size class, with hysteresis and occasional remeasurement of the slower route.
GPU failure backs off and uses the CPU input, without keeping partial GPU work.
An explicit CPU selection never dispatches a plugin shader. Timing data are
synchronised between preview and background render workers.

Exporting `wavyGetParallelV1` remains necessary to allow concurrent CPU calls.
`rowReach=0` requires pixels independent of neighbouring rows **and independent
of absolute row number/image height**: each worker receives a band. Do not opt
in if your result depends on where that band started. Non-reentrant CPU calls
through the plugin runtime are serialized. A plugin error leaves the raster
unchanged.

## Layer order and transfer costs

Plugins run at their displayed positions among built-in effects. For example,
Tint → Glow glows the tinted result, while Glow → Tint recolours the combined
result. Hidden effects are skipped; animated parameters participate in the
layer raster cache key.

The GPU pass runner can mix custom and built-in programs in one submission,
reuses texture storage and uploads only the supplied pixels at the chosen
depth. The layer host currently still crosses the CPU/GPU boundary at plugin
boundaries; consecutive plugin entries are not yet fused into one resident
composition graph. Complex chains can therefore remain transfer-bound. This
work does not establish a universal 1–3 ms preview time.

## Checks

Build the `WavyFlo` and `WavyTint` targets, then run:

```text
Wavy-Flo.exe --plugin-runtime-self-test
Wavy-Flo.exe --plugin-shader-self-test
Wavy-Flo.exe --plugin-shader-benchmark
```

Reports are written beside the executable. The benchmark measures actual
full-HD RGBA8 CPU worker execution and GPU upload/render/readback, with warm
median timing and pixel comparison. It does not measure total preview FPS.
GPU checks return 77 when hardware GL is unavailable.
