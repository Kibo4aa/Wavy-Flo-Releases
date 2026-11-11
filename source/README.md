# Wavy-Flo

Wavy-Flo is a Windows editor for motion graphics and music videos: layers,
keyframes, text animation and handwriting, shapes, 3D layers with extrude and
lights, particles, a motion tracker, time remapping, beat markers from the
music, and effects that come as `.wfx` plugin packs. It is written in C++17
with Qt 6, and its first aim is to play back in real time on modest hardware.

- **Download the program:** https://github.com/Kibo4aa/Wavy-Flo-Releases/releases/latest
- **This folder:** the source code, published with the downloads in
  [source/](https://github.com/Kibo4aa/Wavy-Flo-Releases/tree/main/source).

## Building

Windows x64, Visual Studio 2022 or later (MSVC 19.44 tested), CMake 3.21+,
Ninja, and Qt 6.8 (6.8.3 `msvc2022_64` tested) with the Widgets, Multimedia,
Network, OpenGL, Svg and Test modules. In an x64 developer command prompt:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build
C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release build\Wavy-Flo.exe
```

`build\Wavy-Flo.exe` runs with the effect packs beside it in `build\effects`.
[BUILD.md](BUILD.md) has the optional CUDA plugin and the self-tests
(`Wavy-Flo.exe --self-test`, `--3d-self-test` and the others).

How the program works inside is written up in the other documents here:
[RENDERING.md](RENDERING.md), [RENDER-PIPELINE.md](RENDER-PIPELINE.md),
[PERFORMANCE.md](PERFORMANCE.md), [CAMERA-3D.md](CAMERA-3D.md),
[SHAPES-MASKS.md](SHAPES-MASKS.md), [SURFACE-EFFECTS.md](SURFACE-EFFECTS.md),
scripts (Wavy-Flo's own `wavy` API and After Effects' `.jsx`) in
[SCRIPTING.md](SCRIPTING.md), and the plugin SDK in [sdk/](sdk/README.md).

## Licence

Wavy-Flo is free software: you can redistribute it and/or modify it under the
terms of the **GNU General Public License, version 3 or (at your option) any
later version** ([LICENSE](LICENSE)) -- the licence family Blender uses.
Copyright (C) 2026 Keblackan and the Wavy-Flo contributors.

- What you make *with* Wavy-Flo -- videos, images, projects -- is yours, under
  any terms you like. The licence covers the program, not your work.
- **The plugin SDK** in [sdk/](sdk/) is under the **MIT License**
  ([sdk/LICENSE](sdk/LICENSE)), so effect packs can be written and shared under
  any licence.
- Libraries and other parts by others keep their own licences: see
  [THIRD-PARTY.md](THIRD-PARTY.md).

Wavy-Flo is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE. See the GNU General Public License for more details.

## Contributing

Bug reports, ideas, effects and code are all welcome: see
[CONTRIBUTING.md](CONTRIBUTING.md).

## Contributors

Thank you to everyone who has helped make Wavy-Flo.

[![Contributors](https://contrib.rocks/image?repo=Kibo4aa/Wavy-Flo-Releases)](https://github.com/Kibo4aa/Wavy-Flo-Releases/graphs/contributors)

Everyone who has helped, with code or without -- bug reports, translations,
demos, effect packs -- is named in [CREDITS.md](CREDITS.md), which Wavy-Flo
shows in **Help > About Wavy-Flo**.
