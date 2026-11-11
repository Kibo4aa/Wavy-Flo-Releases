# Wavy-Flo effects: .wfx packs

Effects are shared libraries with the extension **.wfx**, placed in the
`effects` folder beside `Wavy-Flo.exe`, or in a folder of their own inside
it (a maker's folder, as After Effects' Plug-ins folder takes them). This is
Wavy-Flo's own C ABI, not Adobe's SDK or OpenFX; OpenFX plug-ins
(`.ofx.bundle`) go in the same folder, which is read before the other
OpenFX folders. Wavy-Flo's own colour, stylize, noise and transition
effects ship this way, in four packs built on exactly the header described
here:

| Pack | Effects |
|---|---|
| `Wavy-Color.wfx` | Curves, HSL, Levels, Exposure, Brightness & Contrast, Vibrance, Color Balance, Three-Way Color Corrector, Temperature & Tint, Black & White |
| `Wavy-Stylize.wfx` | Tint, RGB Split, Film Grain, Vignette |
| `Wavy-Noise.wfx` | Noise, Turbulent Noise, Displacement Map |
| `Wavy-Transition.wfx` | Linear Wipe, Radial Wipe |

What the drawing itself depends on stays in the application: Fill, Gradient,
Stroke, Glow, Drop Shadow and Gaussian Blur. Take a pack out of the folder and
its effects disappear from the effect browser; a project that uses them opens
with a warning naming what is missing, and draws without them.

## The quickest way: write the shader, Wavy-Flo runs it everywhere (SDK 10)

An effect can be **one GLSL shader and nothing else**. Wavy-Flo runs it on the
graphics card, and -- compiled from the same text for the processor -- on every
core where there is no card or where the frame is drawn there anyway (a render,
a busy preview), 64 pixels at a time. No second version in C++, no threads, no
SIMD, no format conversions: the host does all of it, and the two give the same
picture (every one of Wavy-Flo's own 52 shaders is held to its C++ to within
0.002 by `--effect-packs-self-test`; in practice they agree to 1e-5).

- **As text** (`.wfxs`): a JSON description in a comment, then the shader. No
  compiler, no build: drop the file in `effects/` or `Documents\Wavy-Flo\Effects`
  -- or write it inside Wavy-Flo (Tools > Effects > New Effect), where it is
  checked as you type and drawn in the preview before it is saved. See
  [EFFECT-TEXT.md](EFFECT-TEXT.md), `examples/duotone.wfxs` and
  `examples/soft-bloom.wfxs`.
- **As a pack** (`.wfx`, `WFX_ABI_VERSION` 10): fill in `shader`, leave
  `process` null. See `examples/glass_wobble.cpp`.

Either way the shader reads its parameters by name (`param_amount`) and gets
helpers that give the same numbers on every card and processor (`wavySample`,
`wavyRandom`, `wavyNoise`, ...). Check an effect before handing it out:

```
Wavy-Flo.exe --check-effect path\to\my-effect.wfxs
```

compiles it for the processor and the card, runs both over a test picture at
its defaults and with its sliders at either end, says whether they draw the
same picture, times them at 1920x1080, and saves a picture of the result
beside the file (`my-effect.wfxs.check.txt`). An effect that does not load is
also named, with why, in `wavy-flo-log.txt`.

## Writing a pack (SDK v10, `wavy_effect.h`)

A pack exports two functions:

```c
WFX_EXPORT int32_t wfxEffectCount(void);
WFX_EXPORT const WfxEffect *wfxEffectAt(int32_t index);
```

Each `WfxEffect` names itself (`id` is saved in projects, `name` is shown,
`category` files it in the browser), declares its parameters, and supplies
`process()`. Everything is plain C and static: no toolkit, no allocation the
host must free. See `plugins/pack_color.cpp` and friends for complete packs.

**Parameters.** `WFX_FLOAT`, `WFX_BOOL`, `WFX_CHOICE`, `WFX_COLOR` (four
floats) and `WFX_CURVE` (five points, 0..1, starting as the straight line).
Each has an `id` (stable, saved), a `label`, a range, a step, an optional
`unit` shown after the number and an optional `section` heading. The host
draws the controls in its own panel -- a colour with `WFX_PARAM_WHEEL` on a
hue wheel, consecutive curves as the channels of one curve editor -- saves the
values with the layer, and gives every number, switch and list a stopwatch so
it can be keyframed (`WFX_PARAM_STILL` opts out). Values reach the effect as
floats in declared order, already clamped to the declared range.

**Pixels.** `process(pixels, width, height, stride, values, count, context)`
receives straight-alpha RGBA in the widest format the effect supports
(`supportedDepths`: 1 = RGBA8, 2 = RGBA16, 4 = RGBA32F); with
`WFX_PREMULTIPLIED` it receives premultiplied colour instead, which is right
for anything that moves or mixes pixels. Work in place, return 0, keep nothing.

**Context.** `WfxContext` carries the layer's time (`seconds`, `frame`), the
raster `scale` (0.5 in a half-resolution preview, so sizes given in layer
pixels stay the same on screen), the layer's content box within the raster,
and `parallelFor`, which spreads rows over the host's own workers.

**Optional, for speed and correctness:**

- `inert(values)` -- nonzero when the effect would change nothing; the host
  skips it and a layer needs no raster for it.
- `varies(values)` with `WFX_MOVES_ALONE` -- the picture changes with time
  (a noise with a speed), so each frame is drawn anew.
- `reach(values, scale)` -- how far, in layer pixels, the effect carries a
  pixel; the host leaves that much room around the layer.
- `WFX_WHOLE_LAYER` -- any pixel may come from anywhere in the layer (a
  tiling, a mirror). The host then always hands over the layer's whole box,
  even when it draws only the part on screen or one band of the frame; the
  room around it (reach) is still given only where it shows, so write the
  tiles into whatever raster you get. Tile Repeat in Wavy-Stylize uses it.
- `WFX_PER_PIXEL` -- each pixel out depends on nothing but the same pixel in,
  where it lies and the values (no neighbours, nothing measured over the
  picture). Effects at the end of a layer's chain that all say so are run a
  strip of sixteen rows at a time, the conversions to float and back included,
  so a full frame's float copy never goes through memory. A strip's pixel
  (x, y) is the raster's (`originX + x`, `originY + y`); the box and
  `rasterWidth`/`rasterHeight` describe the whole raster. Those four fields
  were added after ABI 7: read them only when `structSize` reaches them (the
  packs' `rasterPlace()` in `plugins/pack_common.h` does). Film Grain and
  Vignette use it: a grain over a 1080p Effect Layer went from 20 to 13 ms.
- `prepare` / `span` / `release` -- for per-pixel effects. The host runs a
  run of them together straight over 8-bit pixels, without widening the whole
  picture to float for each.
- `shader` -- the same maths as a GLSL 3.30 body
  `vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size)` reading
  `uniform float values[256]`. The host puts it in its own chain on the card,
  beside its own passes, so the picture never leaves the card between effects.

**On the graphics card: one field.** `process()` is the effect; `shader` is the
same effect for the card, and filling it in is all an effect does to run there.
Wavy-Flo compiles it, runs it in the layer's chain next to its own effects and
the other packs' (the picture stays on the card from the first to the last),
and draws the result straight into the frame. Where there is no card, where the
shader does not compile, or where `shaderPasses()` returns 0, the frame is drawn
with `process()` instead -- so keep both, giving the same picture: Wavy-Flo's own
packs are held to that by `--effect-packs-self-test`. From SDK 10 `process()`
may be left out altogether: the host then compiles the shader for the processor
as well, and that one shader is the effect everywhere (see above).

```c
// Desaturate: values[0] is the first float parameter, "Amount" 0..1.
static const char kDesaturate[] = R"(
vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size) {
  float grey = dot(colour.rgb, vec3(.2126, .7152, .0722));
  return vec4(mix(colour.rgb, vec3(grey), values[0]), colour.a);
})";
// ... in the WfxEffect:  .shader = kDesaturate,
```

What the body can use besides its own pixel (SDK 8): `wavyPixel(ivec2)` reads
any pixel of the picture entering it -- neighbours for a blur, an edge or a
displacement -- transparent beyond the raster; `wavySeconds`, `wavyFrame`,
`wavyScale` and `wavyBox` carry the layer's time, the raster's scale and the
layer's box in it; with `shaderPasses()` the body runs several times in a row,
`wavyPass` saying which (`wavyOriginal(ivec2)` reads the picture as it came to
the first). Colour in and out is straight alpha. Every effect in Wavy-Flo's own
packs is written this way, from Film Grain to Turbulent Noise; their sources in
`plugins/` are the examples.

**Data and buttons (SDK v6).** An effect that keeps more than numbers -- a
whole grade built in a window of its own, a table -- declares a `WFX_DATA`
parameter: bytes the host saves with the layer and hands back in
`context->data` on every call. A `WFX_BUTTON` parameter is a button in the
effect's controls; pressing it calls `pressed(button, press)` away from the
interface thread, with the layer's data, the values at the playhead and the
**picture going into the effect** at full resolution (straight RGBA32F).
`pressed()` may open a window and wait for it; what it hands `setData()` is
kept, as one undoable change, when it returns 0, and `message()` shows the
person a line. `describe()` puts a line about the data under the controls,
and `reachWithData()` answers `reach()` for an effect whose reach depends on
its data. Neither kind takes a float in the value list. Packs made for v5 load
unchanged. A colour-look pack, say, is built this way: one look as its data, one
"Edit Look..." button that opens its own editor with the frame.

**How the controls read (SDK v7).** Effect Setting shows an effect as a list:
every control one line -- the stopwatch, the name, the value right beside it
as text that is dragged or clicked -- with twirls folding together what belongs
together. The effect lays that list out itself:

- `WFX_GROUP` / `WFX_GROUP_END` entries in `parameters` open and close a twirl
  named by the group's `label`; groups nest, and `WFX_PARAM_COLLAPSED` starts
  one shut. They take no values and have no id, so adding groups to an effect
  changes nothing in the projects that use it. A parameter's `section`, as
  before, starts a twirl of its own that runs to the next section.
- `help` -- one line per entry of `parameters`, in the same order (null where
  there is nothing to say), shown when the pointer rests on the control.
- `controlState(values, count, parameter)` -- asked after every change with
  the values at the playhead: `WFX_CONTROL_DIMMED` greys a control that has
  nothing to do at these values (it keeps its value), `WFX_CONTROL_HIDDEN`
  takes its line away. For a group it covers the whole group.

```c
const WfxParameter params[] = {
    number("size", "Size", 0, 100, 50, 1, " %"),
    group("Edges", /*collapsed*/ true),
      toggle("mirror", "Mirror", false),
      number("softness", "Softness", 0, 100, 0, 1, " px"),
    groupEnd()};
const char *const help[] = {"How big...", "What happens at the edges.", "Flips...", "Blurs...", nullptr};
```

Tile Repeat in Wavy-Stylize is laid out this way (Gap, Shift, Edges,
Stagger, Alternate), and dims Stagger Along while there is no stagger. Packs
made for v5 and v6 load unchanged and read as before, their sections folding.

Older single-effect plugins (the V1 to V4 entry points below) load from .wfx
files too.

---

# Single-effect plugins (SDK v1 to v4)

This is Wavy-Flo's own C ABI, not Adobe's SDK or OpenFX. Include `wavy_plugin.h` and export `wavyGetEffectV1`. The returned structure and function pointers must remain valid until the DLL is unloaded. `name` is UTF-8, ABI version must be 1.

`process(rgba,width,height,stride,amount)` receives mutable RGBA8 straight-alpha pixels. Respect stride, preserve alpha unless your effect deliberately changes it, process synchronously, return 0 on success. Never retain the pixel pointer. The host owns it. Current host invokes frame effects with amount=1. The exposure backends use amount as exposure stops. Calls are on the UI/render thread in this preview; do not assume that will remain true in future SDKs.

## Controls (V4)

Until V4 an effect was a button that did one fixed thing: the only value
reaching `process` was an `amount` the host always set to 1. A V4 plugin
declares what it wants adjusted, and the host draws the controls in its own
panel and its own style, saves the values with the project, and hands them back
as plain floats. The plugin draws no UI and links against no toolkit.

Export `wavyGetEffectV4` returning a static descriptor: `sizeof(WavyEffectV4)`,
`4`, the depths you support (bit 1 = RGBA8, bit 2 = RGBA16, bit 4 = RGBA32F),
how many parameters, a pointer to them, and your `process`.

Four kinds: `WAVY_FLOAT` (a spin box between `minimum` and `maximum`, stepping
by `step`), `WAVY_BOOL` (a checkbox, 0 or 1), `WAVY_CHOICE` (a dropdown whose
labels are `choices` separated by newlines, value 0..count-1), and
`WAVY_COLOR` (the application's own colour picker, with alpha).

`values` arrives in declared order. Everything takes one float except a colour,
which takes four — red, green, blue, alpha, each 0..1 — so four parameters can
come to seven values. `valueCount` is always the sum, and every value has
already been clamped to the range you declared: a project file can be edited by
hand, and the host does not pass on what you said you would not accept.

`id` is what ends up in the saved project, so keep it stable across versions;
`label` is what a person reads and can change freely. A descriptor that fails
validation — a bad `structSize`, a duplicate id, a range the wrong way round,
more than 64 parameters — is refused and the plugin still runs through its V1
entry point, because refusing the controls should not remove the effect.

`plugins/tint.cpp` is a complete example: a colour, a float, a dropdown and a
checkbox, working at 8, 16 and 32 bits, with a V1 entry point for older hosts
and a parallel declaration so it gets every core. It contains no threading code,
no drawing code and no toolkit.

Your effect goes into a layer's own stack, not onto the finished frame. It
appears in the effects browser under Plugins; double-clicking applies it to the
selected layer, its controls appear in Effect Settings beside Glow and Blur, it
can be reordered among them, hidden without being removed, and it sits inside
the raster cache -- so a layer that has not changed is not put through your
effect again.

Your effect runs per layer carrying it, in its displayed position among built-in
effects. A CPU kernel may receive several band calls when it declares safe
parallelism. The host obtains a raster for plugin layers; an optional shader
can process that raster on the GPU before it is composited.

Your parameters can be animated. Every float, checkbox and dropdown gets a
stopwatch beside it in Effect Settings; switching it on gives that parameter a
curve, starting flat at whatever it was already showing so nothing jumps. From
then on moving the control sets a keyframe at the playhead, and the parameter
appears as its own row in the timeline under Effect, where its keys can be
moved, eased and deleted like any other. Switching the stopwatch off keeps the
value that was on screen rather than snapping back.

You do not have to do anything for this: your process() simply receives the
value for the frame being rendered. Colours are the exception and are still
constants -- animating one needs four curves and a picker that can key them.

## Getting the machine for free

Export `wavyGetParallelV1` and the host spreads your `process` across every
CPU core by cutting the image into horizontal bands and calling you once per
band, on separate threads, each with a plain pointer, width, height and stride.
Your code does not change and contains no threading of its own. Export nothing
and you are called once with the whole image on one thread, as before: silence
stays the safe answer.

Two fields say what is safe. `reentrant` is nonzero when `process` may run on
several threads at once -- no mutable globals, no shared scratch, nothing kept
between calls. `rowReach` is how many rows above and below the row you are
writing that you read: `0` for anything pointwise, which is nearly all colour
work, and a vertical blur of radius 8 declares 8. Only rows matter; a band is
always the full width.

A reach above zero brings one obligation: **your writes must not change your
reads.** Copy the input before you start and write into the host buffer from
that copy. A filter that reads pixels it has already overwritten gives a
different answer depending on where a band begins, and the seams will show. The
host gives each band its extra rows from an untouched copy and takes back only
the rows that band owns, so a plugin that keeps the obligation produces the same
picture on one core and on twenty. `plugins/invert.cpp` declares reach 0;
`plugins/vertical_blur.cpp` declares 8 and shows the copy.

The host also decides when spreading is worth it, because it is not always.
Threading costs a fraction of a millisecond in dispatch, and inverting a 1080p
frame takes about one millisecond on a single core -- spread across twenty it
measured *slower*. So the first frame runs whole and is timed, and only an
effect that proved expensive is spread after that. Measured here on twenty
cores: a pointwise invert stays at 1.1 ms either way, and a vertical blur of
reach 8 goes from 258 ms to 45 ms, **5.8x**, with identical pixels. You declare
what is safe; the host decides what is worth it.

There is a way to get the GPU as well, and it is a different shape: a V3 graph
plugin does not touch pixels at all. It describes operations and Wavy-Flo runs
them, on the GPU where there is one and across every core where there is not.
See GRAPH-SDK.md. If your effect can be described with the operations listed
there, prefer V3: the host already implements their CPU and GPU kernels.
Custom pointwise maths can instead use the optional shader extension alongside
V4; see [SHADER-SDK.md](SHADER-SDK.md) for validation, depth, timing and fallback.

Build `plugins/invert.cpp` as a shared library, put it into the application's `effects` folder, restart the app, and choose it in Render / Effects. The included CMake target does this. GPU exposure backends live next to the executable as WavyCuda or WavyMetal and use the same ABI.

Plugins are native code running with the application's privileges. Only load trusted binaries. ABI versioning is explicit; future parameter/schema support should introduce new capabilities rather than silently changing this struct.

# Python tools

Tools are regular Python 3 files, executed in a subprocess. stdin receives the current project JSON; stdout must contain exactly one JSON object `{"commands":[...]}`. Send diagnostics to stderr. Timeout is 15 seconds; max 1000 commands and 200 layers. Python runs with normal user privileges; this is not a security sandbox.

Supported commands:
- add_text: text, name, x, y. Appends a text layer and selects it.
- set: layer (zero-based index), property, value. Sets the base value. Existing keyframes still take precedence.
- keyframe: layer, property, frame (local to layer), value, easing. Inserts/replaces an animated value.

Property names: positionX, positionY, scaleX, scaleY, rotation, opacity. Scale and opacity use 1=100%. Rotation is degrees. Easing: linear, easeIn, easeOut, easeInOut, hold.

Commands are applied as one undoable action. Invalid command batches restore the previous scene. See tools/fade_title.py. This first API does not yet create custom dock panels, draw custom UI, or implement per-pixel Python effects.
