# Hybrid rendering

The default `Tools > GPU Composition` option attempts hardware OpenGL 3.3.
The same output path serves the preview, PNG export and PNG sequence export.

CPU workers prepare text and shape drawing commands in a reusable thread pool.
An in-memory cache (64 MiB) reuses unchanged commands across frames, including
when position, rotation or scale changes. GPU QPainter replays these commands,
uploads image/video frame textures and composites transformed layers with alpha.
Motion blur accumulates its eight samples in a floating-point GPU framebuffer;
only the resolved frame is read back to the CPU. Static frames avoid redundant
motion-blur samples, except at layer in/out boundaries.

If hardware GL creation or allocation fails, the existing multicore CPU path
is used. Uncheck GPU Composition to explicitly select that path. The render
backend information and preview timecode tooltip show the selected path and
frame processing time. CUDA/Metal exposure remains a separate optional stage.

## Current limitations

- Blur, Glow and Shadow still use the existing Qt CPU effect implementation.
  They are composited by the GPU, but the effect filters themselves are not GPU
  shaders. These effects still force the CPU-only renderer to process serially.
- The final frame is read back to a QImage. Preview painting, GPU submission and
  export encoding are synchronous. This is not yet a zero-copy, asynchronous
  game-engine pipeline and does not guarantee a particular frame rate.
- GPU and CPU antialiasing can differ slightly. Hardware validation is required.
- Video decoding follows the Qt multimedia backend; sequence export still
  rejects video layers. This change does not add video encoding or NVENC.
- More active CPU cores alone do not establish a speedup. Measure frame time on
  representative projects and compare CPU/GPU output before relying on the path.

## Build and validation

Build with Wavy-Flo Builder. The CMake build now links Qt OpenGL and copies
`Qt6OpenGL.dll` from the installed Qt SDK into the project root. Keep that DLL
with the application when distributing it.

`Wavy-Flo.exe --gpu-self-test` compares CPU and GPU outputs for alpha, transforms,
text, images and motion blur and writes `gpu-self-test.txt`. Exit code 77 means
hardware OpenGL was unavailable, not that GPU validation passed.

The source changes were prepared without compiling or running the application,
at the user's request. Runtime correctness and performance are not yet verified.
