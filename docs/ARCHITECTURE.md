# MeerCut architecture overview

MeerCut is a native Windows application in C++20. The editing engine has no Qt dependency: the same code renders the
preview, the editor's export and the headless `fa-render`. The Qt user interface sits on top of it.

## Source layout (`editor/`)

| Folder | Library | What it contains |
|---|---|---|
| `src/core` | `fa_core` | rational time and frame rates, images, the application's data folders, the version |
| `src/anim` | `fa_anim` | keyframed values (`Animatable`) with linear, hold, ease and Bézier interpolation |
| `src/model` | `fa_model` | the project model (sequences, tracks, clips, effects, titles, stickers), edit operations as undoable commands, JSON project files (`.mcproj`), speed ramps (time maps), motion presets, subtitles |
| `src/media` | `fa_media` | FFmpeg probing, video / audio decoding (HDR to SDR conversion), encoding and the export formats, encoder probing |
| `src/render` | `fa_render` | the renderer: timeline evaluation into layers, compositing, effects, transitions, LUTs and colour tools on two back ends; audio mixing, EQ, ducking, loudness and cleanup; the AI matte cache |
| `src/text` | `fa_text` | the title engine (layout, styles, In / Loop / Out animations), title templates and stickers |
| `src/vision` | `fa_vision` | optical flow, motion tracking and stabilisation (on OpenCV), and scene detection |
| `src/ai` | `fa_ai` | the model catalogue and downloads; inference on ONNX Runtime, ncnn and whisper.cpp; background matting, speech to text and captions, face detection and auto reframe, frame interpolation, upscaling |
| `src/playback` | `fa_playback` | the playback clock, audio output (miniaudio), the preview renderer and the Render Preview cache |
| `src/app` | `fa_app` | the Qt editor: main window, resource pages, player, inspector, timeline, dialogs, library pages, templates |
| `apps/` | | `editor` (MeerCut.exe), `render` (fa-render), `probe` (fa-probe), `bench` and `ai_bench` (benchmarks), `icongen` (the icon) |
| `assets/` | | the built-in library: music, sound effects and sample videos (with `library.json` and `samples.json`) |
| `installer/`, `scripts/` | | the Inno Setup script; build, package and environment scripts |
| `tests/` | | `unit` and `text` (GoogleTest), `gui` (Qt Test, real windows), `e2e` (end-to-end renders checked frame by frame) |

## Rendering

* **Working space.** Gamma-encoded Rec.709 R'G'B' with straight alpha
  ([COLOR_MANAGEMENT.md](../COLOR_MANAGEMENT.md)).
* **Building a frame.** `renderFrame()` evaluates the timeline at one frame: the visible clips, their transforms,
  masks, effects, transitions, titles and stickers. The result is a list of layers that a back end composites.
* **Two back ends.**
  * The CPU back end is the reference.
  * The Direct3D 11 back end mirrors it operation by operation in HLSL.
  * Every effect, transition and blend mode is tested to match within one 8-bit level, on each GPU of the test
    machine.
  * *Auto* uses the GPU for frames that need compositing work and the CPU for plain frames.
* **Effects** are described by a registry (`effects_registry.cpp`): their parameters, categories and identity tests.
  Each effect has a CPU function and a shader; per-frame values that both need (random seeds, shake paths) are
  computed once on the CPU.
* **Titles and stickers** are drawn by the text engine at the size they are shown, then composited like video
  frames.
* **Preview and export** run the same code; the preview renders at the player's size.

## Editing model

* **Undo.** Every change is a command on the undo stack. Gestures (drags, slider moves) merge into one undo step.
* **Files.** Projects are JSON with a format version. Older projects open unchanged; Lumacut's `.lcproj` files open as
  MeerCut projects.
* **Keyframe time.** Keyframes are stored in source time, so trimming a clip keeps its animation in place.

## AI

* **Models.** They are not shipped. The catalogue (`src/ai/models.cpp`) names each model's upstream URL, SHA-256 and
  licence. The Model Manager downloads a model only when asked, and its hash is checked again on first use.
* **Background analysis.** Long AI work (mattes, interpolated frames, upscaling) runs as background analysis with
  progress and Cancel, and is cached on disk. MeerCut estimates the time and asks first when it would be long.
* **Devices.** GPU inference uses DirectML (ONNX Runtime) or Vulkan (ncnn, ggml). The CPU is always available, and a
  GPU failure is reported rather than hidden.

## Tests

* `ctest --preset msvc-release` runs about 500 tests:
  * unit tests of the model, renderer, effects, audio, media and AI code;
  * end-to-end renders checked frame by frame (with frame-index strips) and against `ffprobe`;
  * GUI tests that drive the real editor windows.
* Every release is tested with the Release and Debug builds. See [BUILD.md](../BUILD.md).
