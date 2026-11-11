# Optional FFmpeg export

Wavy-Flo can invoke FFmpeg as a separate command-line program to export MOV.
FFmpeg is developed by the FFmpeg contributors; it is not owned by Wavy-Flo.
The exporter does not link FFmpeg libraries into the application.

FFmpeg is licensed under LGPL 2.1 or later by default. Optional components can
change a build to GPL or make it non-redistributable. This exporter checks the
selected executable's `-L`, `-version`, encoder and muxer reports and accepts
only builds reporting LGPL, without GPL/nonfree flags. This is a compatibility
and configuration check, not a legal audit or a guarantee of patent clearance.

No FFmpeg CLI binary is supplied by this source change. Users may select their
own LGPL executable. Its accompanying license and corresponding source apply.
Qt Multimedia and its shipped libraries have separate distribution obligations;
this integration does not audit or replace them.

## If distributing FFmpeg with Wavy-Flo

- Use a verified build without `--enable-gpl` and `--enable-nonfree`; review all
  enabled external libraries as well. Do not include libx264/libx265 GPL builds.
- Keep the binary's original copyright notices and full applicable license texts.
- Supply the complete corresponding FFmpeg source for the exact shipped binary,
  including modifications, patches, build configuration and required build scripts.
  Provide a working download alongside the binary and identify its version/hash.
  Linking only to the FFmpeg home page or unrelated latest source is insufficient.
- Include attribution and the source location in the app, distribution notices,
  EULA and download page. Do not claim ownership of FFmpeg or restrict rights
  granted by its license. Do not prohibit the reverse engineering allowed by it.
- Keep a release manifest with executable SHA-256, FFmpeg version, configuration,
  origin, license version and matching source archive/location.

An external process is an architectural choice, not an automatic exemption from
distribution requirements. Modifying FFmpeg is permitted under its license;
modifications must be included in corresponding source when distributed.
Free downloads, voluntary donations and paid third-party plugins do not waive
third-party license requirements. Codec/container patents and trademarks are
separate from the copyright license of FFmpeg's code. No Apple certification or
endorsement is claimed for the ProRes implementation.

## Official references

- [FFmpeg license and distribution guidance](https://ffmpeg.org/legal.html)
- [FFmpeg source and download information](https://ffmpeg.org/download.html)
- [LGPL 2.1 text](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)
- [FFmpeg command-line documentation](https://ffmpeg.org/ffmpeg.html)

This notice describes the optional integration. It is not a substitute for the
license texts and matching source package required when shipping a binary.
