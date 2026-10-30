# Building, testing and packaging MeerCut

MeerCut is written in C++20 and builds with CMake and vcpkg (manifest mode, pinned baseline) on Windows 11 x64. The
source is in `editor/`; the scripts that generate the built-in library, the test media and the release checks are in
`tools/`.

## 1. Prerequisites

| Tool | Version used for 1.0.0 | Install |
|---|---|---|
| Visual Studio 2022 Build Tools (MSVC v143, Windows 11 SDK 10.0.26100) | 17.14 / MSVC 14.44 | `winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100"` |
| CMake | 4.4.4 | `winget install Kitware.CMake` |
| Ninja | 1.13.2 | `winget install Ninja-build.Ninja` |
| Git | 2.55 | `winget install Git.Git` |
| vcpkg | registry commit `c52f7bd7` (pinned as `builtin-baseline` in `editor/vcpkg.json`) | `git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg`, then `C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics` |
| Python 3.12 with numpy and Pillow (tests and tools only) | 3.12 | `py -3.12 -m venv tools\.venv`, then `tools\.venv\Scripts\pip install numpy pillow` |
| FFmpeg command-line tools `ffmpeg` / `ffprobe` (tests and tools only) | 9.0 | `winget install Gyan.FFmpeg` |
| Inno Setup 6 (installer only) | 6.7 | `winget install JRSoftware.InnoSetup --scope user` |

The CMake presets (`editor/CMakePresets.json`) expect vcpkg in `C:\dev\vcpkg` and build into `C:\dev\fa-build\<preset>`,
outside the source tree. To use other folders, add a `CMakeUserPresets.json` next to it that inherits the presets and
sets `FA_BUILD_ROOT` and `VCPKG_ROOT` in its own `environment` block (the file is git-ignored).

## 2. Dependencies

Declared in `editor/vcpkg.json` and built from source by vcpkg on the first configure. The licences of everything that
ships are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

| Package | Version | Used for |
|---|---|---|
| FFmpeg (`avcodec, avformat, swscale, swresample, gpl, x264, x265, vpx, svt-av1, opus, dav1d, nvcodec, qsv`) | 9.0.2 | decoding, encoding, muxing ([EXPORT_FORMATS.md](EXPORT_FORMATS.md)) |
| x264 / x265 | core 165 / 4.3 (8-bit) | H.264 / HEVC encoders (GPL-2.0-or-later) |
| libvpx / SVT-AV1 / dav1d / Opus | 1.16.0 / 4.1.0 / 1.5.4 / 1.6.1 | VP9 and AV1 encoding, AV1 decoding, Opus audio |
| nv-codec-headers / oneVPL | 13.1.15.0 / 2.17.0 | NVIDIA NVENC and Intel Quick Sync interfaces (the codecs run in the graphics drivers) |
| Qt 6 `qtbase` (`gui, widgets, png, thread, testlib, windeployqt, harfbuzz`) | 6.11.2 | the user interface, the GUI tests and the text engine (LGPL-3.0, dynamically linked) |
| miniaudio | 0.11.25 | audio output (header-only) |
| nlohmann-json | 3.12 | project files |
| GoogleTest | 1.18 | unit tests |
| OpenCV (`opencv4`: `calib3d, intrinsics, thread`) | 4.14.0 | optical flow, motion tracking and stabilisation analysis |
| ncnn (`ncnn[vulkan]`) | 20260526 | Real-ESRGAN upscaling and RIFE interpolation on the CPU or a Vulkan GPU |
| whisper.cpp + ggml (`whisper-cpp[vulkan]`) | 1.8.6 + 0.26.0 | offline speech to text on the CPU or a Vulkan GPU |
| Vulkan headers / loader, glslang, shaderc, SPIRV-Tools | from the baseline | the Vulkan back ends above (shader compilers at build time, the loader at run time) |
| zlib | 1.3.2 | unpacking model archives, compressing cached mattes |
| ONNX Runtime, DirectML build | 1.24.4 | Background Removal (RVM, BiRefNet) and face detection (YuNet) on the CPU or, through Windows' own `DirectML.dll`, on a DirectX 12 GPU. Not a vcpkg port: `tools/fetch_m7_assets.py` downloads the NuGet package `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 into `C:\dev\third_party\onnxruntime-directml-1.24.4` (CMake cache `FA_ONNXRUNTIME_DIR`). |

AMD AMF is not built: the vcpkg `amd-amf` archive failed its integrity check, and it was not bypassed. Without a
binary cache the first build takes a few hours (FFmpeg, Qt and the AI libraries); later configures restore everything
from vcpkg's binary cache in seconds.

## 3. Build

```powershell
tools\.venv\Scripts\python.exe -I tools\fetch_m7_assets.py C:\dev\fa-models C:\dev\third_party   # ONNX Runtime (+ AI models for the tests)
.\editor\scripts\build.ps1                         # configure + build the msvc-release preset
.\editor\scripts\build.ps1 -Test                   # ... and run all tests
.\editor\scripts\build.ps1 -Preset msvc-debug -Test
```

or by hand:

```powershell
. .\editor\scripts\devenv.ps1          # MSVC x64 environment, CMake / Ninja / Git on PATH
cd editor
cmake --preset msvc-release            # configures into C:\dev\fa-build\msvc-release (runs vcpkg install)
cmake --build --preset msvc-release
ctest --preset msvc-release
```

Presets: `msvc-release` (RelWithDebInfo, for development and the tests), `msvc-debug`, and `msvc-dist` (optimised
Release for the packages, tests off). `-DFA_BUILD_GUI=OFF` builds the engine and the command-line tools without Qt.

Outputs in `C:\dev\fa-build\msvc-release\bin\`:

* `fa-editor.exe`: the editor (packaged as `MeerCut.exe`);
* `fa-render.exe`: the headless renderer (the same engine as the editor's export);
* `fa-probe.exe`: media, encoder and AI device information;
* `fa_unit_tests.exe`, `fa_text_tests.exe` and `fa_gui_tests.exe`: the tests;
* `fa-bench.exe` and `fa-ai-bench.exe`: benchmarks.

The build copies the Qt, FFmpeg and other DLLs, the Qt platform plugin and the built-in library (`editor/assets`) next
to them.

## 4. Tests

The tests need the synthetic test media, the AI test recordings and (for the AI tests) the models:

```powershell
.\tools\make_testmedia.ps1; .\tools\make_s3_testmedia.ps1          # synthetic clips, pictures and sounds (FFmpeg test sources)
tools\.venv\Scripts\python.exe tools\make_lut_tests.py; tools\.venv\Scripts\python.exe tools\make_chroma_chart.py
tools\.venv\Scripts\python.exe tools\make_motion_tests.py
tools\.venv\Scripts\python.exe -I tools\fetch_m7_testdata.py       # Blender open-movie footage, LibriSpeech and FLEURS samples
```

* `fetch_m7_testdata.py` checks every download against `testmedia/ai/manifest.json` (SHA-256, source and licence).
* An AI test whose model is missing reports *skipped*, never passed. The tests read the models from `FA_MODELS_DIR`
  (CMake cache, default `C:\dev\fa-models`).
* A few tests compare MeerCut's colour tools and chroma key with measurement data that is not part of this repository;
  without it they report *skipped*.

Running the tests:

* `ctest --preset msvc-release` runs everything: unit tests, end-to-end renders and GUI tests. `-L e2e` runs only the
  end-to-end renders, `-L gui` only the GUI tests.
* `fa_unit_tests.exe --gtest_filter=EditOps.*` runs a subset. `fa_unit_tests.exe --gtest_filter=Adapters/GpuParity.*`
  checks CPU / GPU parity on every Direct3D 11 adapter.
* The GUI tests open real windows: do not use the mouse or keyboard while they run.
  * Each test function is one CTest entry and writes `<build>\logs\gui_<name>.txt`.
  * You can also run one directly: `fa_gui_tests.exe m11SampleVideos -o out.txt,txt`.
  * With `FA_SCREENSHOT_DIR` set they save screenshots there; `readmeScreenshots` makes the pictures in the README.
* The end-to-end tests need `tools\.venv` (numpy) and `ffmpeg` / `ffprobe` on PATH.

## 5. Release packages

```powershell
.\editor\scripts\package.ps1 -Out C:\dev\fa-build\release     # msvc-dist build + packages; -SkipBuild reuses the last build
```

* **Version.** It comes from `project(MeerCut VERSION ...)` in `editor/CMakeLists.txt`. A pre-release adds a suffix:
  `cmake --preset msvc-dist -DMEERCUT_VERSION_SUFFIX=-rc2`.
* **`MeerCut-<version>-win64\`:** the program folder.
  * `MeerCut.exe`, `fa-render.exe` and `fa-probe.exe`.
  * Exactly the DLLs they import (read from their import tables; Windows' own DLLs are not copied), ONNX Runtime's
    provider bridge, the Qt plugins and the Microsoft C++ runtime (app-local).
  * `assets\`: the built-in library.
  * `licenses\`:
    * `LICENSE.txt`;
    * `THIRD_PARTY_NOTICES.txt`, built from each shipped component's own copyright file;
    * `SOURCE.txt`: the commit, the vcpkg baseline and every component's version;
    * `SAMPLE_VIDEOS.txt`: the sample videos' credits.
* **`MeerCut-<version>-win64-portable.zip`:** the same folder, zipped.
* **`MeerCut-<version>-win64-setup.exe`:** the installer (Inno Setup 6, `editor/installer/meercut.iss`). It installs
  per user by default, without administrator rights. Uninstalling removes only the program.
* **`MeerCut-<version>-source.zip`:** `git archive` of the commit the package was built from. It is only written from a
  clean working tree: with uncommitted changes the package is a test build, and `SOURCE.txt` says so.
* **`SHA256SUMS.txt`:** checksums of the installer, the ZIPs and the three programs.
* **Checks:**
  * `tools\check_package_isolation.ps1 -Package <folder> -Projects editor\examples` renders the example projects with
    only Windows on PATH and reports anything loaded from outside the package.
  * `tools\check_installer.ps1 -Setup <setup.exe>` installs, uses, reinstalls and uninstalls the installer in a work
    folder, and checks that the user's data is untouched.

The built-in library is generated, not hand-made, and both scripts give the same files when run again:

* `tools/make_audio_library.py`: music and sound effects, synthesised with numpy.
* `tools/make_sample_videos.py`: the sample videos, from the footage `fetch_m7_testdata.py` downloads.

The application icon `editor/apps/editor/meercut.ico` is drawn by `fa-icongen.exe`.

**User data** that MeerCut writes, and that the uninstaller keeps:

* `%LOCALAPPDATA%\MeerCut\`: autosaves, AI models, the library copies and the render cache;
* `%APPDATA%\MeerCut\`: presets and the LUT library;
* the registry key `HKEY_CURRENT_USER\Software\MeerCut`: preferences and window layout.

## 6. Command-line tools

```powershell
$bin = "C:\dev\fa-build\msvc-release\bin"
& $bin\fa-render.exe editor\examples\two_clips_cut.json C:\dev\out\two_clips.mp4
& $bin\fa-render.exe my.mcproj C:\dev\out\my.mp4 --crf 20 --preset veryfast
& $bin\fa-render.exe my.mcproj C:\dev\out\my.mp4 --format mp4-hevc --hw auto --size 1280x720 --fps 30000/1001
& $bin\fa-render.exe my.mcproj C:\dev\out\my.mov                   # the extension picks the format (.mov = ProRes)
& $bin\fa-render.exe editor\examples\two_clips_cut.json --check      # validate the project and its media only
& $bin\fa-probe.exe --encoders                                      # the export formats and the encoders that work here
& $bin\fa-probe.exe --ai                                            # CPU, GPUs and AI runtimes; the models' status
```

`fa-render --help` lists every option, including:

* `--sequence`, `--frames a:b`, `--backend cpu|gpu|auto`, `--list-gpus`;
* `--subtitles file.srt|.ass` (burn-in);
* `--format`, `--hw cpu|auto|nvenc|qsv|amf`, `--bit-depth 8|10`, `--bitrate`;
* the audio options;
* `--check` and `--quiet`.

The encoder used, and any hardware fallback, are printed. Exit codes: 0 ok, 1 usage, 2 project invalid or cannot be
rendered, 3 render failed.

`FA_GPU_PREFERENCE=power-saving` selects the integrated GPU instead of the discrete one.

In the editor, *Edit › Preferences* chooses GPU or CPU compositing and hardware video decoding (D3D11VA); the status
bar shows what the preview uses.
