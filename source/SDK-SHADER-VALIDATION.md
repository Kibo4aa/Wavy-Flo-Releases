# Shader SDK validation — 12 September 2026

Release build with MSVC 14.44 and Qt 6.8.3. Hardware reported by OpenGL:
NVIDIA GeForce RTX 5050 Laptop GPU; host reports 20 logical CPU threads.

All 15 CTest suites pass, including the new `plugin_runtime` and `plugin_shader`
suites. The checks cover:

- Real worker dispatch after CPU calibration, and unchanged input on plugin failure.
- GPU selection, periodic remeasurement, CPU-only selection and failure backoff.
- Tint CPU/shader parity at 8, 16 and 32 bpc, every sample blend mode, alpha and HDR.
- Rejection of a shader that agrees only at the middle choice or clips HDR.
- Padded image rows, 256 packed uniform values, alternating custom/built-in programs.
- Plugin → Fill, Fill → plugin, and plugin → Glow through actual layer rendering
  and its raster cache, alongside existing animation and hide tests.

The benchmark runs full 1920×1080 RGBA8, without preview downscaling. Timings
are the median of five warm executions, after two warm-up runs. CPU uses the
existing learned band scheduler; GPU includes upload, execution and readback.

| Synthetic kernel | CPU worker route | GPU route |
| --- | ---: | ---: |
| 1 sine evaluation per RGB channel | 5.381 ms | 11.797 ms |
| 64 sine evaluations per RGB channel | 224.560 ms | 11.928 ms |

Both outputs passed pixel comparison. These workloads demonstrate the reason
for measured routing; they are not before/after engine speedups, Glow timings,
or guarantees about total preview FPS. Existing built-in wide-blur behaviour
and manual preview resolution controls were not changed in this work.

Run `Wavy-Flo.exe --plugin-shader-benchmark` to repeat on another machine. The
full report is written beside the executable as `plugin-shader-benchmark.txt`.
See [SHADER-SDK.md](sdk/SHADER-SDK.md) for the ABI and remaining transfer limits.
