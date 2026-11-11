# Render pipeline

The layer pipeline is source rasterization (including Main Transform), ordered
effects, then composition with the layer transform and mask. CPU and GPU effects
consume the same source image. The preview divisor stays the explicitly selected
one and the layer's own pixels are always resolved at it.

Blurred output is the one exception, and it applies to the blur alone. A box
blur of half-width r carries no detail finer than r, so a wide one is resolved
on an image reduced by r/12 -- the kernel stays at least twelve samples wide --
and interpolated back to full size. Measured against the full-resolution
reference on a hard-edged test: mean 0.005, worst 0.021 in linear float
(1.4/255 and 5.3/255). Over a whole rendered frame of text with Glow at 12x
layer scale the difference was mean 0.041/255, worst 1/255, and at 1x and 4x
there was none at all. Frame cost on that scene fell from 105.7 ms to 53.2 ms
at 12x, and from 35.7 ms to 3.5 ms at 4x. What remains at extreme scale is the
pair of full-resolution sweeps the reduction and interpolation themselves cost;
drawing the halo's source small instead of reducing a full-resolution one is
the step that removes those.

An effected raster larger than a megapixel is no longer read back off the card.
The passes leave it in a texture and it is drawn into the frame from there,
carrying the layer's transform and opacity by hand. Below that size it is still
read back and cached, because a few megabytes are cheap and a cached result
spares the whole chain on the next frame -- translating a small layer hits that
cache. A masked layer always comes back as pixels, because a clip path is
something native drawing cannot see. Measured on twenty glowing layers at six
times scale, 1920x1080: readback was 602 ms of a 989 ms frame, for 285 MB per
frame that the painter then uploaded again.

Wide blurs on the card are resolved at the resolution they can express, by the
same r/12 rule the CPU uses, with the reduced image held to at least 64 pixels
on its shorter side. This matters more on the GPU than on the CPU: a box wider
than 32 cannot be walked with a running sum in a shader and falls back to
parallel prefix sums, which is log2(width) full-resolution passes per axis and
six axis passes -- seventy-eight passes over every pixel of the raster for one
glow. On the same scene the effect passes fell from 235 ms to 28 ms, and the
frame from 464 ms to 219 ms. gpuBlurPasses stays exact and is still the
reference the GPU blur is tested against; gpuSoftenPasses is the resolved one.
What remains on that scene is the 74.6 Mpx of source raster the layers are
rasterized into and uploaded, which is the next thing to cut.

A layer whose only pixel effect is one halo -- a Glow or a Drop Shadow, with no
blur, no pixel Stroke, Gradient or Fill, and nothing else that would blur the
halo's own result -- is drawn in two pieces instead of one. The halo is resolved
at the scale its blur can express and cached there; the layer itself goes into
the frame as the vectors it already is, at full resolution, with no raster of
its own. At full opacity the two forms produce the same pixels (measured worst
6/255, mean under 0.6/255 for both Glow and Drop Shadow, at two and three times
scale); below full opacity they weight the halo showing through the layer
differently, so a faded layer keeps the raster.

This applies only above the megapixel threshold. Below it the finished pixels
are cached whole and blitting them back is cheaper than drawing the layer again:
twenty glowing layers at twice scale, one of them being dragged, cost 10.3 ms
the cached way against 30.2 ms the split way. At six times scale, where nothing
caches, the same scene is 729.7 ms against 81.0 ms on the CPU and 218.2 ms
against 8.3 ms on the GPU.

The CPU composition is painted into a premultiplied target and converted back
once at the end. Qt's raster engine composites in premultiplied form, so a
straight-alpha target makes it convert on every operation -- and a composition
is nothing but operations. Measured on twenty glowing text layers at six times
scale: 52.4 ms to 23.0 ms, of which putting the halos into the frame fell from
40.8 ms to 11.6 ms. The pixels are unchanged; all twelve suites pass either way.

Source images have a separate 64 MiB cache. Completed effects have a 128 MiB
cache. These limits cover cached images, not active render buffers. Source keys
exclude pixel effects but include effects folded into vector drawing. Small
rasters (up to 1,048,576 pixels) keep their full bounds so moving them does not
invalidate the effect cache. Larger rasters retain visible-region cropping with
the required effect neighbourhood. Scaling or changing that region can still
require recomputation.

The preview diagnostic tooltip reports source and effect stage times and cache
hits. These are aggregate work counters sampled over the render interval, not
GPU timestamp queries; effect time includes transfers and readback. Concurrent
render work can contribute to the totals. Frame time remains the end-to-end
measurement.

The CPU Glow composite uses a contiguous, parallel source-over pass when the
halo has no offset. Wide GPU blur uses full-resolution prefix sums, with the
same transparent-boundary box filter as CPU (subject to float rounding).

Remaining costs include GPU readback into QImage, large full-resolution effect
neighbourhoods, and source/effect cache invalidation on scale changes. These
changes do not establish a realtime performance guarantee on low-end hardware.

Validation added to the existing GPU self-test checks CPU cache reuse when a
small effected layer is translated. Existing precision and GPU pixel tests cover
blur and premultiplied alpha. Build and runtime tests must be run before claiming
measured speedups.

Release validation (2026-09-10): all 11 CTest suites passed on the local
RTX 5050 Laptop GPU. The installed executable was opened and text with Glow
was checked on GPU and CPU, including a CPU position drag. These checks do not
benchmark the previously reported extreme-scale scene. Built-in affine vector
layers share CPU raster coverage before GPU composition; the temporary MSAA
composition change was removed. Pixel-aligned offscreen bounds fixed the
CPU/GPU discrepancy at reduced resolutions. Perspective geometry keeps its
direct drawing path to avoid unbounded inverse-viewport rasters.

## Pre-rendered frames

Frames rendered ahead of the playhead are kept in memory. The green strip
under the timeline ruler is what is ready.

`Editor::stepPreRender` continues while playback is running. It starts at the
next frame, because the current frame belongs to the foreground renderer, and
wraps around at the end of the composition. Look-ahead jobs have lower pool
priority and use at most one third of the CPU cores. The foreground frame keeps
the other cores and its queued jobs pass background jobs, so filling the cache
does not turn playback into a wait for its own pre-render. On a one-core machine
look-ahead pauses during playback because there is no CPU time to run both. A
pointer gesture still cancels it immediately. Frames are always rendered at
the full preview target rather than the visible slice, so panning and zooming
do not empty the cache. The budget is a
control in the status bar beside the compositing backend (`preRenderBudget`,
persisted as `render/cacheBudget`), from off to 16 GB, because a title sequence
and a three-minute music video want very different numbers. The readout beside
it says how many seconds are held and what they cost; its tooltip converts the
budget into seconds at the current preview size, which is the number that
actually answers "will this hold my sequence".

### What an edit costs

The cache is only worth having if it never shows a picture of something that has
since changed: a missing frame costs a render nobody notices, a stale frame is a
lie on screen that the person watching cannot detect. Invalidation is therefore
conservative, but it is not blanket. `preRenderSignature` (`src/prerender.cpp`)
takes the scene apart:

- **Per layer** — everything that reaches a pixel is hashed into one key: type,
  text and typography, colours, stroke, blur/glow/shadow, all four effect
  objects, the main transform, masks and shape points, visibility, the source
  path and `QImage::cacheKey`, and every property with all of its keyframes.
  Names and panel-expansion flags are deliberately excluded — they change
  several times a minute and change nothing on screen.
- **Ancestors** — a layer's key folds in its parents' keys, so moving a parent
  invalidates everything hanging off it.
- **What is behind it** — a layer's key folds in the ids stacked behind it, in
  order. Named that way round on purpose: putting a new layer in front of
  everything changes nothing behind it, so adding a layer costs only the frames
  that layer is actually on screen for. Restacking two layers changes both.
- **Spans** — each layer reaches `[start, start + duration)` plus one frame of
  slack for the motion-blur shutter and fractional in-points. A changed layer
  drops the frames in its span before the edit *and* after it, because a layer
  dragged along the timeline takes two stretches with it.

Some things cannot be attributed to one layer and are global — any change to
them empties the cache: composition size, fps, duration, processing depth,
background, global motion blur, exposure, the preview target, the compositing
backend, the camera once anything in the scene is 3D, and a layer turning 3D at
all (which can reorder two other layers that were in separate depth groups
before, at frames it is nowhere near). Duplicate layer ids also fall back to
whole-scene invalidation rather than guessing.

Video frames are never stored. They are decoded by the player as the playhead
moves, so a cached one would be the same picture forever; those spans are marked
unstable and are skipped when choosing what to render as well as refused by
`store`.

A completed render is checked against `PreRenderSignature::frameKey` for the
frame it was rendered for — not against the whole scene — so an edit somewhere
else on the timeline while a frame was in flight does not throw the answer away.

The signature is taken on every edit, on the typing path, so the self-test
holds it to under 8 ms for eighty layers with keyframed properties.

### Filling, and knowing when to stop

The cache lives in RAM only, and the budget is offered against what the machine
actually has: `maximumCacheBudget` is three fifths of physical memory, and
larger sizes are shown greyed out rather than hidden, so someone on a small
machine can see that the software knows what it is sitting on. A budget larger
than the machine holds is not a bigger cache, it is swapping, and swapping a
frame back in is slower than rendering it again.

Each wake-up renders a slice of frames rather than a single one. One frame per
tick capped throughput at four frames a second regardless of how cheap a frame
was, so six seconds of an empty timeline took three quarters of a minute to
fill; the worker now keeps going until 120 ms are spent or the queue it was
handed runs out.

Filling also stops once the budget is full. It cannot simply keep going: a
composition needing more frames than the budget holds would render a frame,
evict an older one to make room, and find that older one missing on the next
pass -- a core burned forever on a bar that never completes. At Full quality a
1080p frame is 8.3 MB, so 512 MB holds 61 of them, two seconds at 30 fps; the
same budget at Half quality holds 247. Once full, the only work still worth
doing is trading a frame far ahead of the playhead for one nearer to it, which
is what `PreRenderCache::furthestAhead` is for.

### What a frame costs to keep

Frames are stored as runs, not as pixels. `packImage` codes each row as tokens
-- a quint16 whose top bit says "one pixel repeated N times" or "N pixels as
they are" -- and the plain rows are kept instead whenever that loses, which
happens only on noise. It is lossless, so a frame off the cache is the frame the
renderer made, bit for bit.

Motion graphics are almost all flat: a transparent or single-colour background,
solid fills, antialiased edges only where something actually is. Measured on one
title over a 1920x1080 background, opaque or transparent alike: **8100 KB raw,
40 KB packed, 198x**, 3.8 ms to pack and 2.3 ms to unpack against 2.7-8.0 ms to
render the frame in the first place. Two minutes of that sequence is 144 MB
instead of 29 GB. `at()` keeps the last frame it unpacked, because paints
arrive in bursts for one frame.

Frames that render alike are rendered once and held once. The batch asks
`sameCompositionPixels` -- the same test the live preview uses to decide it can
keep showing what it has -- and hands back the identical QImage, which
`shareFrom` records without a second copy; the budget is charged once per
buffer through a reference count, so a title standing still for three seconds is
ninety frames of timeline and one picture.

What a frame costs depends on how busy the finished picture is, not on how many
layers made it: a thousand layers still composite down to one frame of the
composition size. Measured at 1920x1080, rendering frame 10 of each:

| scene | render | kept as | per frame | ratio | 512 MB reaches |
|---|---|---|---|---|---|
| one moving title | 8.8 ms | runs | 40 KB | 202x | 7.3 min |
| forty glowing text layers | 309 ms | planes | 280 KB | 28.9x | 62 s |
| the ceiling, nothing compressible | | rows | 8100 KB | 1.0x | 2.1 s |

Runs alone managed 1.8x on the glowing scene, because a glow is gradient nearly
everywhere and no two neighbouring pixels are alike. Splitting the row into
channels, coding each byte against the one to its left and deflating the result
takes it to 28.9x -- sixteen times smaller -- and reads back in 14 ms rather
than the 20 ms per-pixel runs cost there. Storing costs 43.7 ms against a 309 ms
render. Runs still win on a flat frame, 202x against 173x and at half the cost
to decode, so store tries them first and only pays for the second codec when
the first one came back worse than sixteen to one.

So two minutes at 1080p cannot exceed 29 GB however heavy the scene, and 4K is
four times that -- but neither number is ever reached, because the budget is a
hard ceiling. What a heavy scene loses is not memory, it is reach: at 512 MB the
title caches seven minutes and the forty-layer scene caches four seconds.

Coding each frame against its neighbour, the way a video codec does, was
measured and rejected. It wins hugely in one case -- still footage, 6733 KB down
to 6 KB -- and loses in every other, because when everything on screen is moving
the difference between two frames is busier than either frame: 40 KB becomes 57,
4455 becomes 4814, 9775 becomes 11914. One case winning does not pay for keeping
reference frames alive through eviction and invalidation. For a heavy scene the
lever that does work is preview resolution, which is already a control: halving
it quarters the pixels and quadruples the reach.

A disk cache, for sequences longer than RAM will hold, is not built yet -- and
is much less pressing now that a frame costs forty kilobytes rather than eight
megabytes.

## Which backend actually runs

Ticking GPU is permission, not an instruction. The card pays a fixed cost before
it draws anything — context, upload, render, readback into a QImage — and at
1920x1080 on an RTX 5050 that is seven to eight milliseconds. A light frame never
earns it back. Measured, same machine, same composition size:

| scene | CPU | GPU | |
|---|---|---|---|
| one plain title | 3.8 ms | 8.6 ms | CPU by 2.2x |
| one title with glow | 4.0 ms | 7.4 ms | CPU by 1.9x |
| forty plain layers | 10.9 ms | 8.2 ms | GPU by 1.3x |
| forty glowing layers | 13.3 ms | 7.8 ms | GPU by 1.7x |

So the box used to make the common case worse, and twice over: ticking it also
turned the asynchronous CPU pipeline off — `asyncCpu` was gated on `!preferGpu`
— which put every frame back on the UI thread. A scene the card was slower at
became a slower render that also blocked the interface.

`Editor::chooseBackend` now decides per frame from what each backend has been
costing per megapixel, learned as it goes and averaged so that one slow frame
cannot decide the next thousand. Neither measured yet means each gets one turn;
after that the cheaper one runs, and every hundred and twentieth frame the other
gets a turn back, because a scene grows heavier as effects are added and the
answer changes with it. The CPU path stays asynchronous whenever the CPU is
chosen, which is the half of the fault that had nothing to do with the card.

When the box is ticked and the card is not used, the corner now says "CPU is
faster for this scene" rather than staying silent. Silence was what made a
working feature look broken: the box was ticked, the picture was identical, and
nothing explained why.

One honest limit: pre-rendered frames are always CPU-rendered, so a frame taken
from the green bar never touches the card. That is not a fault. A frame already
rendered has nothing left for the card to do, and the card earns its place on
the frames the cache does not have — which are exactly the ones being edited.

## Effects in a layer's own stack

A plugin effect is no longer applied to the finished frame. It goes into a
layer's effect stack beside Glow and Blur: `appliedEffects[<plugin name>]` turns
it on, `hiddenEffects` turns it off without removing it, `surfaceEffects[<plugin
name>]` holds its parameter values, and `effectOrder` places it among the
built-in effects. `layerEffectOrder` used to return only the six names the
engine knows; it now also keeps a recorded name the layer is actually carrying,
and appends anything applied that nobody recorded an order for. The engine never
learns what a plugin is — only that the layer applies something by that name.

The engine calls out through `HostLayerEffects`, registered globally rather than
threaded through every render entry point, because plugins belong to the
application rather than to one render and the workers need them too. Three
callbacks: `active` (cheap, asked by the gates that decide whether a layer needs
a raster at all), `identity` (folded into the raster key), and `apply` (run on
one named effect at its actual position in the stack, in place). The engine
interleaves built-ins and host callbacks; plugins are not appended after the
built-in chain. The local frame time reaches each callback for animated values.

Three shortcuts had to learn about it, because each of them skips the raster
stage that plugins live in: the gate that decides a layer needs rasterising at
all, the single-halo path that draws a layer as vectors over a blurred copy, and
the proof that an opaque fill covering the clip hides everything behind it. A
layer carrying a plugin obtains an intermediate raster: direct resident
composition would leave no pixels for a CPU-only plugin. A V4 plugin may use
its optional shader on this raster, with measured CPU fallback. V3 plugins run
their graph at the same position in the stack. The plugin/built-in boundary
still incurs conversions and, when GPU kernels run, transfer costs.

The raster is premultiplied, because that is what Qt's painters want. The SDK
has always promised plugins straight alpha, so the host converts on the way in
and back on the way out. On a semi-transparent pixel the two are different
colours and a plugin has no way to tell, so quietly handing over the wrong one
would be a bug nobody could see the cause of.

What this buys the machines that need it most: a plugin effect now sits inside
the effected-raster cache. A layer that has not changed is not re-effected, the
same as for Glow and Blur — so on a scene where one layer moves, the plugin on
the other twenty costs nothing at all. That is worth more to an i3 with no
graphics card than any GPU path could be.

### Animating a parameter the enum has no room for

A plugin declares its own parameters, so they can have no slot in the property
enum. `Layer::extraCurves` holds curves for them, named `<effect>/<parameter>`,
and `Layer::at(index)` addresses the fixed properties below `PropertyCount` and
the extra curves above it. That one indirection is what lets the timeline carry
a plugin parameter without knowing what a plugin is: rows, keys, dragging,
easing and the graph editor all work from a property index and needed no other
change. `Layer::at` is deliberately total — a row can outlive the rebuild that
made it by one repaint, and an index naming nothing gives a scratch curve rather
than reaching into whatever is next in memory.

The host contribution to `effectedRasterKey` is what makes this correct, and it
took two tries to prove. The first test asked whether the host callback returned
different values at two times, which it does whether or not the key uses it, so
removing the contribution left everything passing. Rendering two actual frames
and comparing them caught it at once: for a plain text layer the frame time is
not part of the raster key, so without the contribution frame 30 silently reuses
the raster built for frame 0 and the animation never appears at all.

Colours are still constants. Animating one needs four curves and a picker that
can key them.
