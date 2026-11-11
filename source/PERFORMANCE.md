# Performance changes and resource monitor

Undo/redo now stores Scene snapshots. Qt image and JSON data are implicitly
shared until modified, avoiding PNG compression, Base64 conversion and image
decoding on every checkpoint. The existing 30-checkpoint history limit remains.
Snapshots can still retain deleted footage until the history is discarded.

Preview targets the panel's display pixels, including zoom and device pixel
ratio. Previously it rendered at least the composition resolution even in a
small panel. Source images are unchanged; export still requests composition
resolution. Opaque/transparent property evaluation is unchanged. Fully
transparent layers and offscreen non-text layers are skipped before painting.
Static GPU frames bypass the two motion-blur accumulation/resolve passes.

The status bar shows process CPU (normalized to total logical CPU capacity),
resident RAM in GB and percentage of total physical RAM, and the busiest
available Windows process GPU-engine counter. This is not total GPU use or
VRAM usage. GPU engines include graphics/copy/video and the displayed maximum
is not a sum across adapters or engines. Unsupported counters show N/A.
Sampling runs in a background task approximately once per second. If rendering
blocks the GUI, displaying a completed sample can still be delayed.

The application already has a CPU worker pool and GPU composition, but the
renderer still returns frames synchronously to the UI and reads GPU output
back to RAM. Blur/glow/shadow now use float-buffer effects with GPU attempts and multicore CPU fallback. These changes do not
guarantee realtime playback for arbitrary numbers of 4K layers.

Build and runtime benchmarking were not run at the user's request. Snapshot
sharing/isolation checks were added to --3d-self-test. After building, compare
the same project, viewport size and slider gesture, and inspect the frame-time
tooltip next to the viewer timecode along with the new resource counters.

## Hybrid pixel effects (preview and export)

The shared paintComposition path now applies exposure, Gradient, and raster
Stroke compositing using OpenGL 3.3 float textures when called on the GUI
thread with GPU enabled. Stroke distance fields use parallel CPU row/column
passes and exact Euclidean distances. Worker-thread rendering falls back to
CPU pixel operations; nested pool work runs inline to avoid deadlocks.

Blur runs six separable box passes with a single upload/readback. Radii above
32 pixels per box pass use the CPU sliding window, whose work does not grow
with the kernel diameter. Glow and shadow reuse this blur and composite the
halo on GPU when available. Buffer transfers between separate effects remain;
this is not a fully GPU-resident pipeline. No speedup has been benchmarked.

The frame status distinguishes GPU composition from CPU composition with GPU
effects. The backend detail lists successful GPU pixel operations per frame;
this is not GPU utilization. High-depth composition still rasterizes on CPU.
UI painting, plugins, media decoding and encoding are not universally GPU jobs.

Source-only validation added: --gpu-pixels-self-test checks HDR exposure,
alpha, orientation, and six-pass blur coefficients; returns 77 without a usable
GPU. These checks and application compilation have NOT been run. Preview is
still synchronous, so expensive frames can still delay slider interaction.
## Large-layer preview optimization (2026-09-10)

Full/Half/Third/Quarter in the viewer footer divide the display-resolution
render dimensions by 1/2/3/4. The choice persists in QSettings and invalidates
the existing preview cache. Export dimensions and source assets are unchanged.
Full retains the previous display-pixel target (including zoom and HiDPI),
not necessarily one pixel per composition pixel.

Effect rasterization no longer clamps its scale to at least 1. Large footage
shown small, or rendered at reduced preview resolution, now gets an effect
buffer at the target scale. This changes the sampling of small effects when
viewed downscaled. CPU blur traverses adjacent columns in tiles rather than
striding the entire image for each column; workers own disjoint output ranges.
GPU textures reuse allocations at the same dimensions and ping-pong blur
textures explicitly use non-mipmapped filters.

GPU backend details now report initialization failures, including context
creation and software rendering. Existing local test reports said OpenGL
context unavailable; those historical results do not establish availability
in the current interactive application session.

New source tests cover tiled blur versus direct convolution, including partial
tiles and transparent borders, plus GPU texture reuse/resizing. No build or
runtime test was performed for this update, per user preference. There is no
measured speedup or realtime guarantee yet; synchronous render and GPU/RAM
transfers between individual effects remain.

## User-selectable compositor (2026-09-10)

The bottom-left status control selects CPU compositing or GPU compositing.
It synchronizes with Tools > GPU Composition and persists render/preferGpu.
Both preview and export consult this setting. CPU disables built-in GPU pixel
effects and the legacy CUDA/Metal exposure path. External plugins and media
codecs retain their own implementation choices.

Plain CPU composition uses the render pool's full logical-worker capacity,
limited by available rows; the old 32-worker ceiling is removed. Frames with
pixel effects run the outer composition once and parallelize inside effect
passes, avoiding repeated padded regions and nested pool deadlock. Small passes
can use fewer workers. This does not mean every stage is parallel or that all
cores will remain at 100%. No build/runtime validation was run for this change.

## Extreme zoom / layer scale (2026-09-10)

Separate content extent padding from the neighbourhood required to evaluate
visible effect pixels. Previously the 12-layer-unit safety margin was also
added around the viewport; at 5641% layer scale this could add hundreds of
screen pixels on each side even for a pointwise Gradient. The viewport margin
now starts at two target pixels. Spatial blur/glow/shadow and raster Stroke
add their required support separately; blur support is summed for chained
passes. Gradient coordinates continue to use the original layer bounds.

A source regression case compares a cropped 400%-target render of a 5641%
layer with the corresponding full-render crop. This update has not been built
or benchmarked. Large spatial effect radii still require neighbourhood pixels;
the change does not guarantee every extreme-scale effect will be realtime.

## Effect List follow-up (2026-09-10)

All current entries were reviewed: Gradient, Stroke, Gaussian Blur, Glow,
Drop Shadow and Text-Animation. This is an incremental optimization, not a
fully GPU-resident effect graph or an automatic CPU-plugin-to-GPU compiler.

* Gradient skips a zero-amount pass; transparent Stroke skips distance fields.
* Stroke hashes alpha using row workers, hashes once for both sides, and shares
  immutable cached distance arrays instead of copying them on each slider edit.
* Gaussian Blur reuses scratch buffers between its CPU passes.
* CPU Glow/Shadow blur one alpha channel and directly composite premultiplied
  colour. Integer shadow offsets now match the GPU offset convention.
* GPU Glow/Shadow (supported box radii <=32) retain the original and blurred
  image on GPU until compositing, then read back once. Larger radii retain the
  linear-time CPU fallback. GPU blur still processes RGBA internally.
* Text glyph contours use a bounded per-thread/per-font cache, independent of
  animation time and layer transforms. Text layout and poses still need CPU.

Shared applyLayerEffects ordering and the selected compositor remain intact.
CPU mode disables built-in GPU operations; GPU mode can fall back to CPU.
Distinct effect calls still transfer images, so GPU can remain slower in some
workloads. Existing C++ plugin ABIs are CPU buffer interfaces; no new public
host-operation SDK or general GPU plugin API was implemented in this update.

Source regression tests compare alpha-only halo against RGBA convolution and
fused GPU halo against separate passes. They were not built or executed, per
user preference. No numerical speedup or realtime guarantee has been measured.

## Playback invalidation and opaque halo viewport (2026-09-10)

Playback advances at integer composition frames and avoids duplicate ticks.
Time-only panel refresh preserves the current preview image. The cached image
is reused across time only for scenes without keyed properties, video or active
text animation and with unchanged layer visibility intervals. Motion-blur edge
samples are conservatively excluded. Ordinary edits still invalidate preview.

Oversized halo-only stacks first sample the visible region (plus AA guard).
If every source alpha is exactly opaque, Glow/Shadow behind that source cannot
change visible pixels, so their large neighbourhood is not allocated. This is
an exact opaque-region shortcut, not a reduced-quality blur. Raster Stroke,
Gradient and Gaussian Blur stacks do not use this shortcut. Transparent edges
retain the ordinary effect path. The probe can add some work when it fails.

Source regression cases cover static reuse, layer end, keyed/video invalidation
and equivalence of an opaque viewport with/without an oversized glow. Build and
runtime checks were not run. Heavy changing frames remain synchronous; no claim
of universal realtime performance is made. Cached playback retains the last
render-cost metric, which is not a measurement of current playback FPS.

## Asynchronous CPU preview (2026-09-10)

Built-in CPU preview now renders a Scene snapshot on a dedicated coordinator
thread, which can dispatch work to the existing multicore pool. There is only
one active preview future. New edits coalesce until it completes; results from
an obsolete edit revision, viewport, backend or text-edit session are discarded.
Playback can show the most recent completed frame while preparing the next.
The UI polls without waiting. Closing the preview waits for its owned future.

GPU preview, external plugin preview and export retain their existing paths.
This change improves UI responsiveness; it does not make a 400 ms render run
in 33 ms. Frames can still be skipped under load. No automatic quality decrease
was introduced. Static reuse now compares evaluated properties (including
shutter samples), so keys that have finished no longer force identical frames
to be rendered again. Text animation/video remain conservative invalidations.

Source tests cover a background snapshot isolated from edits and reuse after
the final key. Application build/runtime checks were not run, per user choice.

## Review after concurrent edits (2026-09-10)

Preserved the newer effected-raster cache, resident GPU effect stack, Fill and
main transforms. Motion-blur sample detection now requires a visible active
layer with motion blur enabled. Keyed values are compared across the shutter
samples, so settled keys do not cause eight identical renders. Parent keys are
still considered when a visible child may need motion blur. Text animation
with motion blur remains conservative. This applies to CPU and GPU renderers.

Fixed CPU blur downsampling to include remainder rows/columns and fixed the
upper/left bilinear clamp. The newer coarse wide-blur approximation is retained.
Regression checks were added for these cases; no build/runtime tests or timing
measurements were performed, per the user's existing build preference.

## Display zoom separated from render resolution

Preview's target resolution now uses the fitted panel size, device pixel ratio
and Full/Half/Third/Quarter only. Zoom changes the displayed image rectangle
and visible crop; it no longer multiplies target dimensions or effect kernels.
At 400%, pixels of the same preview resolution are magnified. Panel resizing
still changes the fitted target. Export remains at composition resolution.
Only source inspection was performed; no build/runtime test was run.

## Wide single-Glow raster split

For a single wide Glow with no subsequent raster effects, no main transform,
and full layer opacity, rasterize the halo source directly at a scale targeting
24 pixels of blur radius. Cache that halo separately and draw the sharp source
at the normal compositor resolution. This avoids allocating a large full-detail
source plus expanded halo border only to downsample it immediately. Other
stacks retain the ordered path. The halo is an approximation at reduced sampling
resolution, consistent with the existing wide-blur approach; the source stays
sharp. CPU and GPU compositors share this path; halo processing uses the backend
when available. Regression source checks validate halo tint/source isolation.
No build, runtime test or speed measurement was run for this change.

## The cliff at a hundred layers

Stacking layers past a certain point did not slow the preview down, it stopped
it: a hundred and twenty large glowing layers took **1060 ms a frame** where
thirty took 8 ms. The effected-raster cache was fixed at 128 MiB, and the
finished rasters for that scene come to roughly twice that.

A least-recently-used cache walked in the same order every frame is its own
worst case. By the time the last layer is drawn, the first has been evicted to
make room for it; the next frame starts at the first layer and misses, evicting
the second, and so on forever. The hit rate does not fall in proportion to the
overflow — it goes to **zero**. Measured, effect cache hits at a hundred and
twenty layers: none at all, and 858 ms of glow recomputed every single frame.

Two changes. The budget is now an eighth of physical memory, clamped between the
old fixed figure and 1.5 GiB, because 128 MiB was chosen for compositions much
smaller than the ones people actually build. And each frame now claims the room
it uses: an entry served from the cache claims its own size, a new entry is
only admitted if the frame has room left, and beyond that the raster is returned
to the caller but not kept. That is what removes the cliff. The layers that fit
stay fit from frame to frame, and a scene too large for any budget keeps
whatever fraction of itself fits instead of losing all of it.

| large glowing layers | before | after |
|---|---|---|
| 30 | 8.3 ms | 5.3 ms |
| 120 | 1060.7 ms | **15.5 ms** |
| 300 | 6549.9 ms | **~130 ms** |

The regression test sets the budget deliberately too small for sixty glowing
layers and requires that some are still served. Before the fix it is zero of
sixty; after it, five. Five is not a good hit rate — it is not meant to be. It
is the difference between a slope and a cliff.

What remains at three hundred large overlapping layers is the compositing
itself, with every effect already cached and nothing left to recompute: three
hundred big alpha blends into one frame, bound by memory bandwidth. Banding that
across cores was measured and is far worse — `workers = 1` whenever a layer
carries a pixel effect stands, because each band recomputes the effects for its
own slice, and forcing it turned a 130 ms frame into 40 seconds.

## A hundred plain text layers

Deep stacks with no effects at all were still heavy, and taking the frame apart
found the time in two places that had nothing to do with the layers.

**Every text layer was treated as an effect.** `hasPixelEffect` listed
`type=="text"`, `"fill"`, `"shape"` and parametric shapes alongside blur, glow
and shadow, and any one of them put the whole frame on a single core. The reason
that rule exists is real — a band recomputes the padded neighbourhood of an
effect for its own slice, and forcing bands on three hundred glowing layers
turned a 130 ms frame into forty seconds — but it does not apply to a small
vector layer. Its raster is cached whole, every band looks up the same one and
blits its own slice, and the work divides properly.

`compositionBands` now asks the question the mechanism actually poses: a frame
divides unless some layer carries a pixel effect, or is a vector layer too large
to be cached whole and therefore clipped to whoever asks for it. The size is
estimated from the layer's own scale rather than measured after its transform,
so the threshold is a quarter of the cache-whole limit — erring small costs a
missed division of labour, erring large costs the forty seconds. It is a
function rather than a condition buried in `renderCPU` so the rule can be
asserted directly instead of inferred from a stopwatch.

**Every frame allocated two full-size buffers.** `sampleRegion` painted into one
and returned a converted copy in another. Allocating eight megabytes costs
nothing until it is touched; touching it costs 1.4 ms in page faults, because
Windows hands large blocks back to the system when they are freed and faults
them in again next time. Measured: allocate 0.01 ms, allocate and fill 1.62 ms,
fill a buffer that already exists 0.26 ms. The painting buffer never leaves the
function, so it is now kept between frames, per thread because bands paint on
threads of their own.

| | before | after |
|---|---|---|
| 1 text layer, whole frame | 2.7 ms | 1.8 ms |
| 100 text layers, whole frame | 8.3 ms | ~4.5 ms |
| 100 text layers, one drag frame | 10.3 ms | ~7.2 ms |
| the suite's own 120-layer benchmark, warm | 9.5 ms | 6.2 ms |

One trade worth naming: banding makes the *cold* frame do more total work, since
every band misses the cache at once and builds the same source. Warm frames
duplicate nothing — all bands hit the one cached raster — so the gain holds on
any number of cores while the cost falls only on the first frame after a change.

Two things measured and left alone. `refresh()` costs 0.08 ms per mouse move at
any layer count, so the panels are not the problem. And the timeline already
skips rows outside the exposed rectangle, which holds its repaint flat at about
1.3 ms whether the composition has one layer or two hundred.
