# Third-party software

Wavy-Flo uses the following software by others. Each keeps its own licence.

| Component | Used for | Licence |
|---|---|---|
| [Qt 6](https://www.qt.io/) (Core, Gui, Widgets, Multimedia, Network, OpenGL, Svg, Test) | the whole program, linked as DLLs | LGPL-3.0 (also available under GPL-3.0); the licence texts ship in the release's `licenses` folder |
| [FFmpeg](https://ffmpeg.org/) libraries used by Qt Multimedia, and the optional FFmpeg program for MOV export | video decoding; export | LGPL-2.1 or later -- see [third_party/FFMPEG.md](third_party/FFMPEG.md) |
| Microsoft Visual C++ runtime | shipped beside the program | Microsoft's redistributable terms |
