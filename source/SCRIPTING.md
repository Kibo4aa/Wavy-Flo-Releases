# Scripting

Wavy-Flo runs JavaScript, and its own API for it is **`wavy`**. Nothing in it
is held back: every layer, every property and keyframe (its easing, its
handles, its motion-path handles and timing keys, its expression), every
effect with every parameter, text, text animation, particles, masks, shapes,
markers, the editor's menus and playhead, events, pictures and their pixels,
the network, files, programs, timers, and windows and docked panels of the
script's own. A layer can also be read and changed whole, as the project file
keeps it, so whatever Wavy-Flo learns to do, a script can do too.

Scripts written for After Effects (ExtendScript `.jsx`, ScriptUI, and
compiled `.jsxbin`) run as well, through a layer over the same calls -- see
[After Effects scripts](#after-effects-scripts) -- and one script can use both.

## Running scripts

| Where | What |
|---|---|
| **File > Scripts** | Run Script File..., then every script in the Scripts folders, then Open Scripts Folder. |
| **Window > Script Panels** | Every script in a `ScriptUI Panels` folder, opened as a docked panel. |
| **Scripts/Startup** | Every script in a Scripts folder's `Startup` folder runs as Wavy-Flo opens; what it adds (menu items, panels, `wavy.on`) stays for the session. A script there is a plug-in. |
| `Wavy-Flo.exe project.wavyflo --run-script file.js` | Opens the project, then runs the script once the window is up. |

Scripts are looked for in two folders, each with `ScriptUI Panels` and
`Startup` inside:

- `Scripts` beside `Wavy-Flo.exe` (the examples are put here by the build), and
- your own, `Documents/Wavy-Flo/Scripts` (File > Scripts > Open Scripts Folder
  makes it).

`.js`, `.jsx`, `.jsxbin` and `.wfs` files are listed.

**One run is one Undo step**, however many layers and keys it touched, menu
commands included; `wavy.undoGroup(fn)` makes steps of its own inside a run. A
script that stops half way keeps what it did and is still one step. In a
panel or window, each button press is one step. When a script throws, the
message names the file and the line, the line of an `#include`d file included.

`#include "file"`, `//@include "file"`, `#includepath`, `#target`,
`#targetengine` and `#script` work as in ExtendScript.

## The `wavy` API

Times are frames of the Main (`wavy.project.fps` turns them into seconds).
Colours are `[r, g, b, a]` from 0 to 1. Scale and Opacity are fractions
(`1` is 100 %), as the project keeps them.

```js
var comp = wavy.project.active;                 // the composition shown
var title = comp.addText('Hello', { fontSize: 120 });
title.position = [comp.width / 2, comp.height / 2];
title.scale.setAt(0, [0, 0]);
title.scale.setAt(10, [1, 1]);
title.scale.ease(10, 'easeOut');
title.addEffect('Glow');
title.pulseOnBeats({ move: 'scale', strength: 0.15 });
```

### `wavy.project`

| Member | |
|---|---|
| `width`, `height`, `frames`, `frame` | read and set |
| `fps` | read |
| `depth` | 8, 16 or 32 bits per channel (Project Settings) |
| `background`, `linearLight`, `motionBlur`, `playRange` (`[start, end]` or `null`) | read and set |
| `file` | the project's path (`""` before it is saved) |
| `main`, `active`, `compositions` | the Main, the composition shown, all of them |
| `allLayers`, `selection` | every layer (groups' too); the selected ones |
| `markers`, `addMarker(frame, label, beat)`, `setMarker(i, frame, label)`, `removeMarker(i)` | |
| `lineUp(layers, { from, step, onMarkers })` | Line Up in Time: one after another, or on the markers |
| `effectNames` | what `layer.addEffect` takes |
| `layer(id)`, `composition(name)`, `select(layers)` | |
| `save(path?)`, `importFiles(paths)`, `undo()`, `redo()` | |
| `seconds(frames)`, `framesOf(seconds)` | |

### A composition (the Main, or a group)

`name`, `width`, `height`, `fps`, `frames`, `frame`, `layers` (top first),
`layer(nameOrNumber)`, `isMain`, `groupLayer`.

New layers go on top: `add(type, options)` with type `text`, `shape`, `fill`,
`empty`, `camera`, `light`, `effect`, `group` or `particles`, or
`addText(words, options)`, `addShape(kind, options)`, `addFill(colour, options)`,
`addEmpty`, `addCamera`, `addLight`, `addGroup(name)`, `addEffectLayer`.
Options are `name`, `color`, `width`, `height`, `start`, `duration`,
`shapeKind`, `text`, and any other layer field.

`group(layers, name)` moves layers into a new group; `open()` shows it;
`render(frame, path, width?, { transparent })` writes a frame as a picture;
`renderImage(frame, width?, { transparent })` gives it as pixels
(`wavy.image`) -- both over the composition's background unless transparent.

### Movies

```js
var file = wavy.project.main.renderVideo('D:/Renders/title.mp4', {
  from: 0, to: 149,
  onProgress: function (done, total) { wavy.app.status(done + ' / ' + total); }
});
```

`composition.renderVideo(file, options)` renders and waits: it gives the file
(a PNG sequence's folder), `null` when `onProgress` returned false to stop
it, and throws when the render fails. Options: `format` (`"mp4"`, `"png"` --
a sequence --, `"mov"` Animation, `"movpng"`, `"prores"` 4444), `from`, `to`
(frames, both rendered), `depth` (8, 16, 32; the project's when left out),
`bitrate` (Mb/s), `audio`, `alpha`, `onProgress(done, total)`.

`wavy.renderQueue` is File > Render Queue: renders lined up and run on their
own thread while work goes on. `add(composition, file, options)` (or
`composition.addToRenderQueue(file, options)`) puts one in and gives its id;
`items` lists them -- `{id, name, composition, output, target, state
("queued", "rendering", "done", "failed", "stopped"), done, total, message}`;
`render()`, `stop()`, `rendering`, `remove(id)`, `clear()`.
`wavy.on("renderQueue", fn)` hears the list change and a run finish.

### Text Animation

```js
var title = wavy.project.active.addText('Wavy-Flo moves', { fontSize: 120 });
title.addTextAnimator({ preset: 'Word Pop', from: 0, duration: 24 });
var out = title.addTextAnimator({ separateOut: true, outY: -320, outOpacity: 0, order: 'random' });
out.animateOut(90, 104, 'easeIn');
```

A text layer's animations are stacked, each acting on its letters (or words,
or lines) as its **In** goes from 0 (away, as its settings put them) to 100
(at rest) and its **Out** from 0 (at rest) to 100 (gone), one letter after
another in its order.

- `layer.addTextAnimator(options)` puts one at the bottom of the stack: a
  preset (`preset: "Fade Up"`, `"Slide In"`, `"Pop"`, `"Spin In"`, `"Drop
  Down"`, `"Typewriter"`, `"Blur In"`, `"Spread In"`, `"Word Pop"`, `"Line
  Slide"`, `"Color Flash"`, `"Wiggle Float"`, the person's own --
  `wavy.textPresets` lists them), settings, or both. `from` / `duration` /
  `ease` key its In (a preset comes in from the playhead over 20 frames).
- `layer.textAnimators` lists them, top first.
- An animator has `name`, `enabled`, `index`, `edited` (open in the panel),
  `settings`, `summary`, `get(key)`, `set(key, value)` or `set({...})`;
  `in` and `out` (keyable properties); `animateIn(from, to, ease)`,
  `animateOut(from, to, ease)`; `range("start" | "end" | "offset")` (keyable);
  `edit()`, `moveTo(index)`, `moveUp()`, `moveDown()`, `duplicate()`,
  `remove()`.
- Settings: `x`, `y`, `scale`, `rotation`, `opacity` (where the letters are
  when away); `delay` (frames between letters); `order` (`"leftToRight"`,
  `"rightToLeft"`, `"topToBottom"`, `"bottomToTop"`, `"fromMiddle"`,
  `"fromEdges"`, `"random"`); `basedOn` (`"letters"`, `"words"`, `"lines"`);
  `mode` (`"same"`, `"random"` -- each its own way --, `"alternate"`); `seed`;
  `pop`; `blur`, `tracking`, `skew`; `fillColor` and
  `colorAmount`; `wiggleSpeed`, `wigglePosition`, `wiggleRotation`,
  `wiggleScale`, `wiggleOpacity`, `wigglePosterize`; `rangeStart`,
  `rangeEnd`, `rangeOffset`, `rangeShape`; with `separateOut`, the Out's own
  `outX`, `outY`, `outScale`, `outRotation`, `outOpacity`, `outDelay`,
  `outBlur`, `outTracking`, `outSkew`; `write` (handwriting); `name`.

### A layer

- Fields, read and set: `name`, `visible`, `isolated`, `locked`, `threeD`, `blendMode`,
  `motionBlur`, `color`, `labelColor`, `text`, `font`, `fontStyle`, `fontSize`,
  `tracking`, `leading`, `align`, `bold`, `fauxBold`, `fauxItalic`, `capitals`,
  `paragraphWidth`, `strokeOnly`, `strokeWidth`, `strokeColor`, `strokes`,
  `blur`, `glow`, `shadow`, `separatePosition`, `separateScale`, `lockScale`,
  `timeRemap`, `start`, `duration`, `end`, `path`, `parent`, `trackMatte`,
  `trackMatteInverted`, `audioEnabled`, `selected`.
- Read: `type`, `kind`, `index`, `composition`, `effects`.
- Transform: `position`, `scale`, `anchor` (vectors), `rotation`, `opacity`.
  Each can be assigned (`layer.opacity = 0.5`) or used as a property.
- More transforms on top of the first: `addTransform(name)` gives one with its
  own `position`, `scale`, `anchor`, `rotation` and `remove()`;
  `transforms` lists them.
- Any property: `prop('positionX')`, `prop('Position X')`, `prop(5)`, or an
  effect's curve as `prop('Glow/radius')`; `props()` lists them.
- Whole: `data` is the layer as the project file has it; `patch({...})`
  merges a change into it. `textAnimation`, `particles`, `masks`,
  `shapePoints` read and set those parts the same way.
- Masks: `addMask(points, { mode, feather, expansion, opacity, inverted,
  name })` with points from `wavy.path`; `setMask(i, change)`, `removeMask(i)`.
- Effects: `addEffect(name, settings)`, `effect(name)`, `removeEffect(name)`.
- `pulseOnBeats({ move: 'scale' | 'opacity' | 'bounce' | 'kick', strength,
  settle, every, handMarkers })`: keys that jump on the beat markers.
- Presets: `savePreset(file)`, `applyPreset(fileOrPreset)`, `preset()` -- the
  layer's whole look (effects, text, shape, transform, keys), nothing left out.
- `duplicate()`, `remove()`, `moveTo(index)`, `moveToTop()`, `moveToBottom()`,
  `bounds(frame?)`, `toComposition(point, frame?)`, `select()`.

### A property

`value` (at the playhead, expression and all), `valueAt(frame)`,
`keyedAt(frame)` (without the expression), `set(v)` (a key at the playhead
when it has keys), `setAt(frame, v)`, `keys`, `addKey(frame)`,
`removeKey(frame)`, `clearKeys()`, `ease(frame, 'linear' | 'hold' | 'easeIn' |
'easeOut' | 'easeInOut')`, `setKey(frame, data)` (any part of a key: `ease`,
`custom`, `spatial`, `spatialIn`, `spatialOut`, `continuous`, `timing` ...),
`handles(frame)` / `setHandles(frame, out, in)` (the graph's handles as
`[frame, value]` points), `reverseKeys(first?, last?)`, `expression`,
`expressionEnabled`, `expressionError`. Vectors (Position, Scale, Anchor
Point) take arrays and have `keyFrames`.

### Expression Controls

```js
var rig = wavy.project.main.addEmpty({ name: 'Rig' });
var speed = rig.addControl('slider', 'Speed', 90);
box.rotation.expression = 'time * thisComp.layer("Rig").' + speed.reference;
// → time * thisComp.layer("Rig").effect("Speed")("Slider")
```

Named values a layer carries for expressions to read, any number of each
kind: `"slider"`, `"angle"`, `"checkbox"`, `"point"`, `"color"`, `"layer"`.
They key, show in the timeline (under Effect) and in Effect Setting (a group
of their own: a number, an angle dial, a switch, a point with a pick button, a
colour in Wavy-Flo's picker, a layer list, each with its stopwatch; "+ Add
Control"; right-click a line to rename it, remove it or copy how an expression
reads it), and take expressions like any property. The Effects panel adds
them too (Expression Controls).

- `layer.addControl(kind, name, value)`, `layer.controls`,
  `layer.control(name)`; `layer.addEffect("Slider Control")` makes one too.
- A control has `name` (renaming it), `kind`, `value` (a number, `[x, y]`,
  `[r, g, b, a]`, or a layer), `setAt(frame, value)`, `property` (the keyable
  property or vector), `keys`, `reference` (how an expression reads it) and
  `remove()`.
- In expressions: `effect("Speed")("Slider")`, `effect("Speed")(1)`,
  `effect("Speed").param("Slider")`, `thisComp.layer("Rig").effect("Aim")("Point")`,
  `effect("Target")("Layer").position` (a Layer control is its layer); and any
  other effect's setting by its name, `effect("Wavy Blur")("amount")`.

### Motion tracking

```js
var shot = wavy.project.main.layer('Shot');               // a video layer
var corner = shot.addTrackPoint('Corner', [120, -40]);     // at the playhead
var result = corner.track({ to: shot.end - 1 });          // { frames, lostAt, lowest, error }
corner.applyTo(wavy.project.main.layer('Label'));         // Position keys that ride on it
```

A point is followed through a video layer's footage on a thread of its own
while the editor keeps drawing. Places are in the layer's own pixels (the
footage's, from its middle), frames are the Main's.

- `layer.addTrackPoint(name, [x, y], { frame, feature, search })`,
  `layer.trackPoints`, `layer.trackPoint(name)`.
- A point has `name` / `rename(to)`, `feature`, `search`, `samples` (`[{frame,
  x, y, confidence}]`), `at(frame)`, `place(frame, [x, y])` (by hand),
  `clear()`, `remove()`;
  `track({ from, to, backwards, lostBelow, onProgress })` (waited for;
  `onProgress(done, total)` returning false stops it) gives `{ frames, lostAt,
  lowest, stopped, error }`;
  `applyTo(layer, referenceFrame)` keys a layer's Position to ride on the
  point; `stabilize(referenceFrame)` keys the shot's Anchor Point so it holds
  still about the point.

### Effects

`wavy.effects.list` is every effect there is -- Wavy-Flo's own, the `.wfx`
packs', plug-ins', OpenFX's -- each `{ name, label, category, kind, params }`,
and every parameter `{ id, label, kind, min, max, default, step, choices,
unit, section, animatable }`. `wavy.effects.describe(name)` finds one,
`wavy.effects.inCategory(text)` a category's.

On a layer, an effect has `enabled`, `settings`, `get(id)`, `set(id, value)`,
`curve(id)` (the setting as a keyable property) and `remove()`.

### Paths

`wavy.path` makes the points shapes and masks are drawn with -- each
`{ p: [x, y], in: [dx, dy], out: [dx, dy] }`: `rect(x, y, w, h)`,
`ellipse(cx, cy, rx, ry)`, `star(cx, cy, outer, inner, count)`,
`polygon([[x, y], ...])`, `smooth([[x, y], ...], closed)` (through the points,
smoothly), `join(a, b, ...)` (several as one), `point(...)`.

### The editor: `wavy.app`

| Member | |
|---|---|
| `frame` | the playhead; moving it is not an Undo step |
| `playing`, `play()`, `stop()` | |
| `commands` | every menu item, as `"File/Save Project"` |
| `command(path)` | runs one, as if clicked -- anything the menus do, a script can |
| `addCommand(path, fn, shortcut?)` | a menu item of the script's own (`"Scripts/My Tool"`, or in any menu), with a shortcut; gives `{ remove() }` |
| `status(text, ms?)` | a line in the status bar |
| `version` | Wavy-Flo's |

### Events: `wavy.on(event, fn)`, `wavy.off(event, fn?)`

| Event | Told |
|---|---|
| `frame` | the playhead moved: its frame |
| `selection` | the selection changed: the layers |
| `play`, `stop` | playback started or stopped: the frame |
| `change` | the project changed (not by this script itself) |
| `save`, `open` | the project was saved or opened: its path |
| `new` | a new project |

A script listening stays alive -- for good when it runs from Startup -- until
`wavy.script.end()`, which also takes away its menu items, timers and panels.

### Windows and panels: `wavy.ui`

```js
wavy.ui.panel('Title Tools', function (ui) {
  var size = ui.slider('Size', 10, 400, 120);
  var fill = ui.color('Fill', [1, 0.8, 0.2, 1]);
  ui.button('Make Title', function () {
    wavy.project.active.addText('Hello', { fontSize: size.value, color: fill.value });
  });
});
```

`wavy.ui.panel(title, build)` docks in the editor; `wavy.ui.window(title,
build)` floats beside it; `wavy.ui.dialog(title, build)` waits for OK (true)
or Cancel. `build` gets the lines to add: `label(text)`, `button(text, fn)`,
`slider(label, min, max, value, fn)`, `number(label, value, { min, max, step,
decimals }, fn)`, `checkbox(label, value, fn)`, `dropdown(label, items, index,
fn)` (and its `.text`), `text(label, value, fn)`, `color(label, rgba, fn)` (in
Wavy-Flo's own colour picker), `progress(value)`, `picture(wavy.image)`,
`list(items, fn)`, `separator()`, `row(build)`, `section(title, build, open)`.
Each control has `value`, `enabled`, `visible`, `onChange`. ScriptUI's
`Window` and controls work too, and mix with these.

### Pictures: `wavy.image`

`wavy.image.render(frame, width?, composition?)`, `wavy.image.load(file)`,
`wavy.image.create(w, h, rgba)` give a picture with `width`, `height`,
`pixel(x, y)`, `setPixel(x, y, [r, g, b, a])`, `pixels(x?, y?, w?, h?)`,
`setPixels(x, y, w, h, values)`, `average(...)`, `scaled(w, h)`, `crop(...)`,
`save(file)`, `toLayer(name)` and `free()`. Pixels are 0..255.

### The network: `wavy.net`

`get(url, options)`, `post(url, body, options)`, `request(method, url,
options)` give `{ ok, status, headers, body, error, json() }`;
`getJSON(url)`, `download(url, file)`. Options: `headers`, `body` (an object
is sent as JSON), `timeout` (ms). `http`, `https` and `file` addresses.

### Everything else

- `wavy.undoGroup(fn)`: what `fn` does as an Undo step of its own.
- `wavy.fs`: `read`, `write`, `append`, `readJSON`, `writeJSON`, `exists`,
  `isFolder`, `list`, `makeFolder`, `remove`, `copy`, `move`, `stat`,
  `native`, `folder('documents' | 'desktop' | 'temp' | 'home' | 'program' |
  'scripts' | 'appData' | 'userData')`, `openFile`, `saveFile`,
  `chooseFolder`, `open(pathOrUrl)`.
- `wavy.timer`: `after(ms, fn)`, `every(ms, fn)`, `cancel(id)`.
- `wavy.settings`: `get(section, key, fallback)`, `set`, `has` -- kept
  between runs.
- `wavy.system`: `run(command, timeoutMs)` gives `{code, output, error}`,
  `env(name)`, `os`, `windows`, `sleep(ms)`, `open(url)`.
- `wavy.log(...)`, `wavy.alert`, `wavy.confirm`, `wavy.prompt`,
  `wavy.version`, `wavy.script.file` / `.folder` / `.end()`.

## After Effects scripts

Scripts made for After Effects run through a layer of its names over the same
calls: `app.project`, `CompItem`, layers, `property(...)`, keyframes and their
ease, `TextDocument`, markers, effects by match name, ScriptUI windows and
docked panels, `File`, `Folder`, `$`. Scale and Opacity are percentages there,
as After Effects has them. Wavy-Flo answers as itself (`app.name` is
`"Wavy-Flo"`).

**Compiled scripts** (`.jsxbin`, and `eval("@JSXBIN@...")` inside a `.jsx`)
are read back into the script they were made from and run as that, in memory
-- what they say is never shown or saved. Errors name the line of the
original script. XML written in the language itself (E4X) is the one thing
that cannot run; a script using it stops there and says so.

What the After Effects layer does not reach yet stops with a message naming
it: text animators, masks and shape contents drawn through After Effects'
own calls (`wavy` has them all: `layer.textAnimation`, `addMask`,
`shapePoints`), `.ffx` presets, rendering through `app.project.renderQueue`,
and extensions (CEP/HTML panels).

## Examples

`source/scripts`, put in `Scripts` by the build:

- `Wavy - Pop-In Title.js` -- a title that pops in at the playhead.
- `Wavy - Kinetic Title.js` -- three Text Animations stacked: words pop in,
  the title floats, its letters scatter out.
- `Wavy - Render Main.js` -- a dialog, then a movie of the composition with a
  progress window that can stop it.
- `Wavy - Rig Spin.js` -- a Spin control on each selected layer and an
  expression on its Rotation that reads it.
- `Wavy - Track Point.js` -- a window: place a point in the selected video
  layer, track it either way, attach another layer to it or stabilize the shot.
- `Wavy - Beat Tools.js` -- a window: pulse the selected layers on the beat,
  line them up on the markers.
- `Wavy - Frame Colour.js` -- the colour of the frame, as a fill layer behind it.
- `AE - Stagger Layers.jsx` -- staggers the selected layers, written as for
  After Effects, with a ScriptUI dialog.
- `ScriptUI Panels/Wavy Quick Tools.jsx` -- a docked panel: add a title or a
  background, add an effect to the selected layers, centre them, fade them in.

## Under the hood

`src/script_host.cpp` holds the engine (Qt's `QJSEngine`), the native bridge
the APIs stand on (`__wf`), the preprocessor, the events and the widgets;
`src/jsxbin.cpp` reads compiled scripts; `resources/scripting/wavy.js`,
`wavyui.js`, `scriptui.js` and `ae.js` are the APIs. `--scripts-self-test`
checks them against a stand-in editor, `--scripts-editor-self-test` runs
scripts -- the examples included -- through the real one,
`--jsxbin-self-test <folder>` reads back every `.jsxbin` under a folder, and
`--script-self-test-run <file>` runs a script on an empty project of its own
with no editor (its log in `script-self-test-run.txt`) -- for trying scripts
out and rendering what they make.
