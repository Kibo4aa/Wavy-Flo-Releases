# Composition export

File > Render supports native Windows MP4 and PNG sequence without an FFmpeg
executable, plus optional MOV export via a separately launched FFmpeg CLI.

- MP4: H.264 video with optional AAC stereo audio at 48 kHz. Bitrate is adjustable
  from 1 to 2000 Mbps (50 Mbps initially, then the last accepted value is saved).
  This is a target; actual output and supported rates depend on the Windows encoder.
  Transparency is flattened against the composition background. The bitrate field
  is shown only for MP4; MOV presets and PNG sequence have no Mbps control.
- PNG Sequence: full-resolution images, optional alpha, and a separate PCM
  16-bit/48 kHz stereo audio.wav when enabled media contains audio. PNG image
  channels are 8-bit at 8 bpc processing and 16-bit at 16/32 bpc processing.
- MOV Animation: FFmpeg qtrle, RGB24 or ARGB, lossless.
- MOV PNG: FFmpeg png, RGB24 or RGBA, lossless.
- MOV ProRes 4444: FFmpeg prores_ks profile 4, optional alpha. This is an
  independent FFmpeg implementation, not an Apple-certified encoder.
- MOV audio is PCM 16-bit/48 kHz stereo, mixed with the same layer timing/mute
  rules as native export. ProRes converts the staged PNG to its codec pixel
  format; this does not create additional source precision. Processing can be
  8/16/32 bpc. PNG staging clips float values outside 0–1; MP4 and Animation
  are 8-bit output. MOV PNG preserves 16-bit channels at higher processing depths.
  See [precision details](PRECISION.md) for CPU fallback, plugins and HDR limits.

## Optional FFmpeg setup

Choose a MOV format, then Browse to a trusted LGPL `ffmpeg.exe`. The validated
path is saved. Discovery checks the saved path, `tools/ffmpeg/bin/ffmpeg.exe`
beside Wavy-Flo.exe, `ffmpeg.exe` beside it, then PATH. No FFmpeg CLI binary is
downloaded or bundled by this change. MP4 continues to use Windows H.264/AAC.

Before rendering, the exporter checks FFmpeg's license/version reports and
rejects GPL, nonfree and unrecognized builds. It also checks the required video
encoder, PCM encoder (when audio requested), and MOV muxer. Build reports cannot
prove binary provenance or legal compliance; use a trusted distribution.

FFmpeg is launched using QProcess and a QStringList of literal arguments, without
a shell or terminal window. It receives staged full-resolution PNG frames and
optional WAV, with explicit frame rate/count and composition duration. Pixel
format conversion is explicit and alpha pixel format fallback is disabled.
This uses temporary disk space for the whole sequence before encoding. Cancel
terminates the child process and removes staging; errors include FFmpeg's bounded
diagnostic output. Final movies are published via QSaveFile only after success.

Help > FFmpeg licenses and the render dialog's Licenses button show the embedded
[distribution notice](third_party/FFMPEG.md). Before bundling an executable,
include its exact corresponding source, license texts and build manifest. A link
to the FFmpeg homepage alone is not sufficient. No general legal clearance for
MOV/ProRes or the application's other dependencies is implied.

The MP4 sink writer requests hardware transforms; Windows and the installed
drivers determine whether a compatible hardware encoder is selected. Software
encoding may be used. Codec/format errors are reported with the Windows HRESULT.
Windows editions without Media Foundation require the corresponding Windows
media components. Imported formats must be supported by the installed native
decoders; this can differ from Qt preview support.

Video frames are obtained from a persistent Media Foundation source reader,
seeking to composition-mapped source time. Audio is decoded to temporary PCM,
then mixed in blocks using layer start/duration/source offset and mute/visibility.
The current UI still imports one video layer; standalone audio layers are not
implemented. Overlapping audio is summed with integer saturation, not normalization.
PNG and staged MOV audio are limited to the standard WAV 4 GB limit.

MP4 is encoded directly without staging every frame as PNG. Temporary media
and PCM caches are created beside the destination. Only a finalized movie or
completed sequence is published; cancel/failure cleans staging. Existing MP4s
are replaced through QSaveFile. Native decode/encode/finalization calls are
synchronous; cancellation is checked between frames and audio samples, so a
slow OS codec can temporarily delay UI response.

This change does not remove Qt's existing multimedia backend or third-party
dependencies elsewhere in the application; it replaces the export dependency.

Build/runtime tests were not run at the user's request. After building,
`Wavy-Flo.exe --ffmpeg-export-self-test` checks license rejection, exact encoder
matching, literal paths, frame duration, audio mapping and alpha arguments,
without requiring FFmpeg. CTest exposes it as ffmpeg_export_arguments. Actual
MOV encode/decode, alpha, cancellation and audio sync require a compatible
external binary and runtime verification.

`Wavy-Flo.exe --native-media-self-test` writes native-media-self-test.txt and
checks H.264/AAC encoding, native decoding, image orientation, frame time and
audio content/duration. CTest exposes it as native_media (non-Windows skips).

API references:
- https://learn.microsoft.com/en-us/windows/win32/medfound/tutorial--using-the-sink-writer-to-encode-video
- https://learn.microsoft.com/en-us/windows/win32/medfound/mf-mt-default-stride-attribute
