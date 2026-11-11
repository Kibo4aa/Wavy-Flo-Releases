# Effects written as text: .wfxs

An effect can be one text file: a description, then a shader. Put it in the
`effects` folder beside `Wavy-Flo.exe` (or a folder of its own inside it),
or in your own folder, `Documents\Wavy-Flo\Effects`, and it is an effect
like any other: it is
listed in the effect browser, its controls show in Effect Setting, and every
control can be keyframed. Nothing is compiled by the author.

## Writing one inside Wavy-Flo

**Tools > Effects > New Effect** (or **New Effect...** under the effect list)
asks for a name, a category and a starting point -- a colour change, a
distortion, a two-pass blur, a moving pattern, or a blank one -- writes the
file into your folder, and opens it in the effect editor:

- the text is checked as you type; a mistake is marked at its line, and the
  line in the message below is a link to it;
- with **Live** on, the effect in the preview changes as you type, before it
  is saved (closing without saving puts back what the file holds);
- **+ Parameter** adds a control to the description, with an id no other has;
- **Guide** lists what the shader can use, the effect's own parameters
  first; a click puts a name where the cursor is;
- **Save** (Ctrl+S) writes the file, and Wavy-Flo reads it again at once;
- **Check** runs `--check-effect` (below) and shows the report and picture;
- **Apply to Layer** puts the effect on the selected layer.

Right-click an effect written as text in the effect list to edit it -- or, for
one that came with Wavy-Flo, to read it or make a copy of your own. Your
folder is watched: an effect saved there by another editor is read again, and
the preview follows. Tools > Effects > Read Effects Again reads every one
again; Effects That Did Not Load says why, and opens the file at the line.

An effect changed while Wavy-Flo runs keeps its id, so the layers using it
pick up the new version; the pictures cached of them are drawn again, the
rest are kept. Changing the `id` itself is changing which effect it is:
layers saved with the old id no longer find it.

**One shader, run everywhere.** Wavy-Flo runs the shader on the graphics card.
Where there is no card, or where the frame is drawn on the processor anyway (a
render, a busy preview), it runs the *same text*, compiled for the processor
and spread over every core, 64 pixels at a time. There is no second version to
write and no threading or SIMD code to get right: the host does all of it.
Wavy-Flo's own 52 shaders run this way too, and `--effect-packs-self-test`
holds each to the effect's own C++ to within 0.002.

```glsl
/* Wavy-Flo effect
{
  "id": "Duotone",
  "name": "Duotone",
  "category": "Color Correction",
  "perPixel": true,
  "parameters": [
    {"id": "dark", "kind": "color", "label": "Shadows", "default": "#1b1446"},
    {"id": "light", "kind": "color", "label": "Highlights", "default": "#ffc46b"},
    {"id": "response", "kind": "curve", "label": "Response"},
    {"id": "amount", "label": "Amount", "min": 0, "max": 100, "default": 100, "unit": " %"}
  ],
  "inert": "param_amount <= 0.0"
}
*/
vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size) {
  float level = param_response(wavyLuma(colour.rgb));
  vec3 tone = mix(param_dark.rgb, param_light.rgb, level);
  return vec4(mix(colour.rgb, tone, param_amount / 100.0), colour.a);
}
```

`examples/duotone.wfxs` and `examples/soft-bloom.wfxs` are complete effects;
the build copies them into `effects/`.

## The description

The file starts with a comment whose first words are `Wavy-Flo effect`,
followed by one JSON object:

| Key | |
|---|---|
| `id` | Required. Stable: projects save it. |
| `name` | What the effect browser shows (the id when left out). |
| `category` | Where the browser files it ("Plugins" when left out). |
| `parameters` | The controls, in order (below). At most 64. |
| `perPixel` | `true` when each pixel out depends only on the same pixel in, where it lies and the values. The host then runs the effect a strip of rows at a time with whatever comes after it. |
| `movesAlone` | `true` when the picture can change with time while no value does (a noise with a speed). See `varies`. |
| `wholeLayer` | `true` when any pixel may come from anywhere in the layer (tiling, mirroring). |
| `inert` | An expression: true when, at these values, the effect changes nothing. The host then skips it. |
| `varies` | With `movesAlone`, an expression: true when the picture moves with time at these values. |
| `reach` | An expression: how far, in layer pixels, the effect carries a pixel beyond where it was. The host makes that much room around the layer. |
| `passes` | An expression: how many times the shader runs in a row (1 to 64); `wavyPass` says which run it is. |
| `controls` | `{"<parameter id>": "<expression>"}`: `WAVY_SHOWN`, `WAVY_DIMMED` or `WAVY_HIDDEN` for that control at these values. |

Expressions are GLSL, worked out by the host from the values whenever it needs
the answer. They read the parameters by name (`param_amount`), and `wavyScale`;
`passes` also reads `wavyBox`, `wavySeconds`, `wavyFrame` and `sourceSize` (the
raster). A key Wavy-Flo does not know is refused, so a misspelt one is never
silently ignored.

### Parameters

Each is an object with `id` (letters, digits and `_`: it names `param_<id>` in
the shader), `label`, and `kind`:

| kind | value in the shader | keys |
|---|---|---|
| `float` (the default) | `float` | `min` (0), `max` (100), `default`, `step`, `unit` |
| `angle` | `float`, degrees, on a dial | `min`, `max`, `default` |
| `bool` | `bool` | `default` |
| `choice` | `int`, 0 first | `choices`: `["Soft", "Hard"]`, `default` |
| `color` | `vec4`, straight, 0..1 | `default`: `"#rrggbb"`, `"#rrggbbaa"` or `[r, g, b, a]`; `wheel` |
| `curve` | a function: `param_<id>(x)` | five points from 0 to 1, starting as the straight line |
| `point` | `vec2` | `default`: `[x, y]` |
| `point3d` | `vec3` | `default`: `[x, y, z]` |
| `group` / `end` | | a twirl named by `label` around the controls between them; `collapsed` |

Every parameter may also have `help` (shown when the pointer rests on the
control), `section` (a heading above it) and `still` (no stopwatch).

A `float` without a `step` gets one to suit its range: 1 across a range of
100 or more, 0.1 below that, 0.01 below 1; an `angle`, 0.1.

## The shader

GLSL 3.30. Define exactly

```glsl
vec4 wavyEffect(vec4 colour, vec2 coord, vec2 size)
```

`colour` is the pixel, straight alpha; `coord` its centre in the raster
(x + .5, row + .5, the top row first); `size` the raster's size. Return the
pixel's new colour, straight alpha. Besides the parameters it can use:

| | |
|---|---|
| `wavyPixel(ivec2 p)` | any pixel of the picture entering the effect; clear beyond the raster |
| `wavySample(vec2 p)` | the picture between pixels (`p` in raster pixels), mixed in premultiplied colour; clear beyond the raster |
| `wavySampleEdge(vec2 p)` | the same, the edge held instead |
| `wavyOriginal(ivec2 p)` | in a later pass, the picture as it came to the first |
| `wavySeconds`, `wavyFrame` | the layer's own time |
| `wavyScale` | raster pixels per layer pixel: .5 in a half-size preview. Multiply sizes given in layer pixels by it. |
| `wavyBox` | the layer's content in the raster: x, y, width, height |
| `wavyPass`, `wavyPasses` | which run this is, of how many |
| `wavyRandom(vec2 p, float seed)` | 0..1, the same for the same whole pixel and seed |
| `wavyNoise(vec2 p, float seed)` | smooth value noise, 0..1 |
| `wavyHash(uint)`, `wavyLuma(vec3)`, `wavyPremultiply(vec4)`, `wavyUnpremultiply(vec4)` | |

Between its own passes an effect may keep whatever it likes in the four
channels (Soft Bloom keeps its glow premultiplied): what a pass returns is what
the next one reads. Only the last pass's colour must be straight alpha again.

### What gives the same picture everywhere

- The helpers above give the same numbers on every card and on the processor.
  `fract(sin(dot(p, k)) * 43758.5)` does not: the card's `sin` is not the
  processor's, and that formula magnifies the difference into a different
  random number. Use `wavyRandom` and `wavyNoise`.
- `texture()` on the card blends neighbours with 8 bits of where-between;
  `wavySample` blends with full precision on both.
- `discard` leaves the pixel clear. Return `vec4(0.0)` to say so plainly.

### What the processor does not take

Structs, `switch`, derivatives (`dFdx`, `dFdy`, `fwidth`), and texture
functions other than `texelFetch`, `texture` and `textureSize`. Everything else
in a fragment shader is there: every type up to `mat4`, arrays, functions with
`in`/`out`/`inout`, loops with `break` and `continue`, early `return`, the
preprocessor (`#define` with arguments, `#if`). A loop that never ends is cut
off after 262144 turns rather than hanging the program.

## When something is wrong

An effect that cannot be used is not loaded, and Wavy-Flo says why in
`wavy-flo-log.txt` beside `Wavy-Flo.exe` (which Help > Report a Problem sends
along): a key it does not know, a parameter without an id, an expression or a
shader that does not compile -- with the **line in your file**:

```
Effect not loaded: duotone.wfxs: its shader does not compile: line 24: unknown name tones
```

Before handing an effect out, run

```
Wavy-Flo.exe --check-effect path\to\duotone.wfxs
```

It loads the file as Wavy-Flo would, compiles the shader for the processor and
for the card, runs both over a test picture at the effect's defaults and with
every slider at either end, says whether they draw the same picture, times them
at 1920x1080, and saves a picture of the result beside the file. The report is
`duotone.wfxs.check.txt`; the exit code is 0 when everything holds.

## As a C pack (SDK 10)

A compiled `.wfx` pack can do the same: fill in `shader` and leave `process`
null (`WFX_ABI_VERSION` 10). The shader gets the same parameters by name and
the same helpers, and the host runs it everywhere. `examples/glass_wobble.cpp`
is a complete pack; what it keeps in C is only `inert`, `varies` and `reach`.
