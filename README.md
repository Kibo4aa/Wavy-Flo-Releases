<div align="center">

# Wavy-Flo

**Motion graphics and music videos, in real time, for Windows.**

Animated titles, particles, 3D lights, effects and keyframes, previewed as you work,
even on a computer without a strong graphics card.

[![Latest release](https://img.shields.io/github/v/release/Keblackan/Wavy-Flo-Releases?label=latest&color=00D4A0)](https://github.com/Keblackan/Wavy-Flo-Releases/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Keblackan/Wavy-Flo-Releases/total?color=5EA8FF)](https://github.com/Keblackan/Wavy-Flo-Releases/releases)
![Windows 10/11, 64-bit](https://img.shields.io/badge/Windows-10%20%7C%2011%20(64--bit)-F06080)

### [⬇ Download the newest Wavy-Flo](https://github.com/Keblackan/Wavy-Flo-Releases/releases/latest)

[What's new in 0.40](https://github.com/Keblackan/Wavy-Flo-Releases/releases/tag/v0.40)

<img src="docs/overview.png" alt="Wavy-Flo 0.40 in its new look, with the MV-Pop demo open" width="100%">

</div>

---

## Contents
- [Download and install](#download-and-install)
- [A new look in 0.40](#a-new-look-in-040)
- [Features](#features)
- [The workspace, part by part](#the-workspace-part-by-part)
- [Handy keys](#handy-keys)
- [Updates](#updates)

## Download and install
1. Get **Wavy-Flo-…-win64.zip** from [Releases](https://github.com/Keblackan/Wavy-Flo-Releases/releases/latest).
2. Unpack it anywhere, for example in Documents.
3. Open **Wavy-Flo.exe**. Nothing is installed and nothing is added to Windows.

Windows may say *"Windows protected your PC"* the first time: choose **More info → Run anyway**.

**Needs:** Windows 10 or 11, 64-bit. A graphics card helps but is not required: everything also renders on the processor, across all its cores.

---

## A new look in 0.40
- **Timeline:** Eye, Solo and Label columns, with a colour for each layer; Parent, Mode and TrkMat beside every name.
- **Panels on the right fold into one stack:** Info, Audio, Preview, Effects & Presets, Align, Character and Tracker. Open the ones you need and fold the rest.
- **Properties** has a tab for each kind of setting down its left side, and **+ Add Additional Transform** for more transforms on top of a layer's own.
- **Effect controls:** the stopwatch first, every value lined up in one column, a line between parameters, and After Effects-style dropdowns.
- **Masks:** Add, Subtract or None for each, with Mask Opacity, and Mask Feather and Mask Expansion you can keyframe.
- **Status bar:** how each frame is drawn (CPU, GPU or both), the preview cache, and what the computer is doing.

See [the workspace, part by part](#the-workspace-part-by-part) below.

---

## Features

### Text Animation
Letters, words or lines fly, spin, scale, fade, blur and change colour on their way in and out. You can stack several Text Animations on one text layer. Each one has its own **In** and **Out** strength, delay, order and range, and each can be switched off, reordered or removed from its heading. Presets give you a start in one click.

<img src="docs/text-animation.png" alt="Text Animation: the letters of a title on their way in, its animation open in Properties" width="100%">

### Graph Editor
Shape any animation with curves. The graph opens on the keyframes you picked, including those of a second or third Text Animation. Drag the handles to ease in and out.
- **Shift** keeps a handle level.
- **Ctrl+click** on the curve adds a keyframe.
- **Ctrl+click** on an interior keyframe removes it.

<img src="docs/graph-editor.png" alt="Graph Editor showing an eased Position curve" width="100%">

### Effects
Blur & Sharpen, Color Correction, Stylize, Noise & Distort, Transition, Generate and more, all in **Effects & Presets**. Effects stack on a layer in the order you set, each with its own on/off eye, and every number can be keyframed. Effects are plug-in packs (`.wfx`), so new ones can be added without a new Wavy-Flo.

<img src="docs/effects.png" alt="Wave Warp bending a title, from the Noise &amp; Distort effects" width="100%">

### 3D layers and lights
Turn any layer 3D, add a camera, and light the scene with spot, point and ambient lights that cast soft shadows.

<img src="docs/lights.png" alt="A spot light and a point light on 3D text and a star" width="100%">

### Effect Layer
One layer that grades or stylizes everything beneath it.

<img src="docs/effect-layer.png" alt="An Effect Layer turning the shapes beneath it grey while the title above stays in colour" width="100%">

### Expressions
Link one property to another, or to time, with a short expression: a trail of followers, wiggles, loops.

<img src="docs/expressions.png" alt="Shapes following each other with expressions" width="100%">

### And also
- **Particles:** sparks, petals and bokeh with emitters, gravity, wind and turbulence (above, at the top of the page).
- **Shapes and masks:**
  - rectangles, ellipses, polygons and stars;
  - the Pen for your own paths;
  - feathered masks, boolean shape operations and shape modifiers.
- **Fill and Stroke on the timeline bar.** Change the colour and stroke of a Solid, Shape or Text layer without leaving the timeline.

  <img src="docs/timeline-bar.png" alt="Fill, Stroke and stroke width beside the Graph Editor button" width="70%">
- **Music videos:**
  - audio waveform and beat markers on the timeline;
  - Pulse on Beats;
  - Time Remap and Posterize Time.
- **Motion tracker:** follow a point in a video and pin a layer to it, or steady the shot.
- **Motion blur**, track mattes and blend modes.
- **Render Queue** in the background, plus a frame-accurate preview cache.
- **Export:** MP4, PNG sequence, and MOV through FFmpeg.

---

## The workspace, part by part

<img src="docs/workspace.png" alt="The Wavy-Flo window with each part numbered" width="100%">

| # | Part | What it is for |
|---|------|----------------|
| 1 | **Menu** | File, Edit, Main, Layer, Tools, Window, Help and Animation. **Reset view** puts every panel back in its place. |
| 2 | **Tools** | Undo and Redo, then Select (V), Hand, Zoom, Rotate, Anchor Point, Shape (right-click for rectangle, ellipse, polygon or star), Pen, Mask and Text. |
| 3 | **Project** | Your Mains (compositions) and the files you import, with a search box and a filter by kind. Double-click a file to put it into the open Main. A picture that is missing or cannot be read is marked. |
| 4 | **Preview** | The picture as it will be rendered. Drag layers here to move them; Fit, zoom, quality (Full/Half/…), Transparency, Grid and Box sit underneath, the time on the right. |
| 5 | **Panels** | One stack of folding panels: **Info**, **Audio**, **Preview** (playback), **Effects & Presets** (every effect by category, with a search box; double-click one to add it to the selected layer), **Align** (layers to the frame or to each other, and the anchor point), **Character** (font, size, tracking, leading, fill and strokes) and **Tracker**. Click a title to open or fold it. |
| 6 | **Properties** | The selected layer's settings, by the tabs down its left side: Main Transform (with **+ Add Additional Transform**), Text Animation, Shape, Mask, Repeater, Lens, Light, Particles, Tracker and Time. |
| 7 | **Timeline / Graph** | Layers in time. Each row has Eye, Solo, Label colour, number, name, Parent, Mode (blend), TrkMat (track matte), sound, 3D and motion blur. Open a layer to see its properties and keyframes. The bar on top holds Fill, Stroke and stroke width for Solid, Shape and Text layers, and the Graph Editor. |
| 8 | **Status bar** | How the frames are drawn (CPU, GPU or both), the preview cache (in memory or on disk, and Clear), and what the computer is doing: CPU, RAM, GPU and how long each frame takes to draw. |

Every panel can be dragged by its title, floated, or closed. **Window** brings a closed one back.

---

## Handy keys
| Key | Does |
|-----|------|
| **V** | Select tool |
| **T** | Text tool |
| **U** | Show only the properties that have keyframes |
| **F9** | Easy Ease on the picked keyframes |
| **Ctrl+Z / Ctrl+Shift+Z** | Undo / Redo |
| **Shift** while dragging keyframes | Keep to one direction; in the Graph Editor, keep a handle level |
| **Ctrl** while dragging in the Preview | Do not snap |

---

## Updates
Wavy-Flo looks for a newer version as it opens. Press **Update**: it downloads the new version, checks it arrived whole, closes, installs and opens again by itself. Unsaved work is asked about first. **Later** and **Skip This Version** leave everything as it is.

What changed in each version is on the [Releases](https://github.com/Keblackan/Wavy-Flo-Releases/releases) page.

<sub>This repository holds only the downloads. Wavy-Flo's source is not here.</sub>
