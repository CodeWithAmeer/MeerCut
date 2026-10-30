# Third-party components

MeerCut is licensed under the GNU General Public License, version 3 or later ([LICENSE](LICENSE)). The Windows packages
include the third-party components below. FFmpeg is built with x264 and x265 (GPL-2.0-or-later), so **the
distribution as a whole is GPL-3.0-or-later**.

Every package carries these files in `licenses\`:

* `THIRD_PARTY_NOTICES.txt`: the complete copyright and licence text of each shipped component, generated from the
  component's own copyright file when the package is built;
* `LICENSE.txt`;
* `SOURCE.txt`: where to get the corresponding source of every component.

The versions are pinned by `editor/vcpkg.json` (vcpkg baseline `c52f7bd748185ef0e0daf033d9f2ddfc8a3449ea`). vcpkg
builds each component from its upstream source archive, which is verified by SHA-512.

| Component | Version | Licence | What MeerCut uses it for |
|---|---|---|---|
| Qt (`qtbase`) | 6.11.2 | LGPL-3.0-only (dynamically linked) | user interface, text layout |
| FFmpeg (libavcodec, libavformat, libavutil, libswscale, libswresample) | 9.0.2 | LGPL-2.1-or-later; built with `--enable-gpl` (x264, x265), so GPL-2.0-or-later as built | decoding, encoding, muxing |
| x264 | 0.165.3222 | GPL-2.0-or-later | H.264 encoder |
| x265 | 4.3 | GPL-2.0-or-later | HEVC encoder |
| SVT-AV1 | 4.1.0 | BSD-3-Clause-Clear, with the Alliance for Open Media Patent License 1.0 | AV1 encoder |
| dav1d | 1.5.4 | BSD-2-Clause | AV1 decoder |
| libvpx | 1.16.0 | BSD-3-Clause, with the WebM patent grant | VP9 encoder and decoder |
| Opus | 1.6.1 | BSD-3-Clause | Opus audio |
| nv-codec-headers (`ffnvcodec`) | 13.1.15.0 | MIT | NVIDIA NVENC / NVDEC interface (the codecs are part of the NVIDIA driver) |
| oneVPL dispatcher (`libvpl`) | 2.17.0 | MIT | Intel Quick Sync interface (the codecs are part of the Intel driver) |
| OpenCV (core, imgproc, video, calib3d, features2d, flann) | 4.14.0 | Apache-2.0 | optical flow, motion tracking, stabilisation analysis |
| FAST corner detector (`fastfeat`) | 391d5e9 | BSD-3-Clause | feature detection, through OpenCV |
| ncnn | 20260526 | BSD-3-Clause | RIFE and Real-ESRGAN inference on the CPU or a Vulkan GPU |
| whisper.cpp | 1.8.6 | MIT | speech to text |
| ggml | 0.26.0 | MIT | the inference library of whisper.cpp |
| ONNX Runtime, DirectML build (NuGet `Microsoft.ML.OnnxRuntime.DirectML`) | 1.24.4 | MIT (third-party notices included) | Background Removal and face detection on the CPU or through DirectML. `DirectML.dll` is part of Windows and is not shipped. |
| Vulkan loader | 1.4.357.0 | Apache-2.0 | GPU inference through Vulkan |
| Vulkan headers, SPIR-V headers, SPIR-V Tools, glslang, shaderc, OpenGL / EGL registry headers | from the baseline | Apache-2.0, MIT, BSD-3-Clause (Khronos Group, Google) | building the Vulkan shaders of ncnn and ggml |
| FreeType | 2.14.3 | FreeType License (FTL), or GPL-2.0-or-later | font rasterisation (Qt) |
| HarfBuzz | 14.5.1 | MIT | text shaping (Qt) |
| PCRE2 | 10.49 | BSD-3-Clause | regular expressions (Qt) |
| double-conversion | 3.4.0 | BSD-3-Clause | number formatting (Qt) |
| md4c | 0.6.0 | MIT | Markdown parsing (Qt) |
| libpng | 1.6.59 | libpng-2.0 | PNG images |
| zlib | 1.3.2 | Zlib | compression |
| Brotli | 1.2.0 | MIT | compression (FreeType) |
| bzip2 | 1.0.8 | bzip2-1.0.6 | compression (FreeType) |
| nlohmann/json | 3.12.0 | MIT | project files (compiled in) |
| miniaudio | 0.11.25 | Unlicense or MIT-0 | audio output (compiled in) |
| Microsoft Visual C++ runtime (Visual Studio 2022) | 14.x | Microsoft Software License Terms, Distributable Code | the C++ runtime, shipped next to the programs |

**AI models are not part of MeerCut.** *AI Tools › Model Manager* downloads them on request from their upstream
projects and shows their licences. [AI_MODELS.md](AI_MODELS.md) lists each model and its terms.

**Media.**

* The built-in music, sound effects, stickers, templates and the green-screen sample video are MeerCut's own and are
  released with it.
* Four sample videos are excerpts of Blender Foundation films under CC BY 3.0: see [CREDITS.md](CREDITS.md).

**Patent licensing.** Patents covering H.264, HEVC and AAC are a matter for distributors. AV1, VP9 and Opus come with
royalty-free patent licences from their bodies.
