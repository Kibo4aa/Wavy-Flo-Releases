# Processing precision

New compositions default to 1920 x 1080, 30 fps, 6 seconds. Existing project
dimensions are preserved. Composition Settings offers 8 bpc, 16 bpc unsigned
integer and 32 bpc float; the selection is saved with the project. Render can
override it for that export without changing the editor scene.

16 bpc uses QImage RGBA64. 32 bpc uses RGBA32FPx4 and preserves finite RGB
values outside 0–1 through CPU drawing, exposure, float effects, motion blur
and project save/load. Display conversion still depends on the SDR Qt viewer.
This adds numerical precision, not a color-managed linear-light/HDR display
pipeline. No automatic color-space/ICC conversion was added.

The existing GPU compositor returns RGBA8, so 16/32 bpc deliberately select
the CPU renderer. Image bands use the existing worker pool. Built-in blur,
glow and shadow use float intermediates instead of QPixmap/QGraphicsEffect
when processing above 8 bpc. Blur uses a three-box Gaussian approximation;
its appearance may differ slightly from Qt's 8-bit filter implementation.

Imported images retain their decoded QImage format; importing 16-bit images
raises an 8-bit composition to 16, and float images raise it to 32. Increasing
precision does not reconstruct information absent from an 8-bit source.
Float image layers are stored as losslessly compressed, little-endian RGBA32F
data in project JSON. Older applications cannot read these new float layers.
Unsigned 16-bit layers remain embedded as 16-bit PNG.

PSD: composite RGB/grayscale 8/16/32 bpc, raw, PackBits RLE, and ZIP without
prediction. Still one flattened image, maximum file 100 MB and 32 megapixels.
ZIP prediction, CMYK, PSB, separate Photoshop layers and composite alpha are
not supported. Native video decode remains 8-bit, even in a high-bpc project.

Plugin extension: `wavyGetEffectV2` is optional alongside V1 discovery. See
sdk/wavy_plugin.h. It provides straight-alpha RGBA16/32F buffers and byte
stride, with capability bits. V1-only effects are skipped with a message in
high-depth preview, and block export rather than silently quantizing it.
The sample Invert plugin now implements V2 and must be rebuilt to use it.
Legacy CUDA/Metal exposure plugins remain 8-bit; high-depth exposure uses CPU.

Exports:
- MP4 and MOV Animation output 8-bit after high-precision processing.
- PNG sequence / Export PNG / MOV PNG output 16-bit channels when processing
  in 16 or 32 bpc; float RGB outside 0–1 is clipped at PNG conversion.
- ProRes converts staged PNG16 to its encoder format; this is not float HDR
  export. Float-preserving EXR export has not been added.
- MP4 target bitrate remains adjustable 1–2000 Mbps; last selection is saved.

Source tests: `--precision-self-test` / CTest `precision` check HDR drawing,
exposure, 16-bit detail, PNG16 roundtrip, float project roundtrip, PSD16/32
samples/truncation and HDR blur. Build and runtime tests have not been executed
because the user builds the app themselves. Real files, plugins and output
compatibility still need runtime verification after building.
