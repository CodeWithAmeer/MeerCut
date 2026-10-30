# AI_MODELS.md — MeerCut's local AI models: sources, licences, hashes, installation (M7)

Every AI feature of MeerCut runs on the user's PC. There is no cloud service, account, subscription, paid API or
upload of media. **No model is bundled with MeerCut.** Models are downloaded only when the user asks for them in
*AI Tools › Model Manager* (the dialog shows name, size, source and both licences and asks for confirmation), always
from the upstream project's own release, and every file is checked against the SHA-256 recorded in MeerCut
(`editor/src/ai/models.cpp`) before it is installed. A failed, cancelled or mismatched download installs nothing.
Models obtained separately can be installed with *Install from Folder* (same checks). Once installed, the models run
with no network connection: the downloader (`fa::ai::installModel`, WinHTTP) is the only network code in the AI
subsystem.

Verified, not assumed: a test runs every model with the downloader's request counter unchanged; ONNX
Runtime's telemetry events are switched off; each model file's SHA-256 is checked again on its first use (remembered in
`.verified` in its folder), so a damaged file is refused instead of computing wrong pictures.

Model folder: `%LOCALAPPDATA%\MeerCut\models\<model id>\` (changeable in the Model Manager; `MEERCUT_MODELS`
overrides it, e.g. for tests). `fa-probe --ai [--verify]` prints the devices and every model's status.

## Inference runtimes (code licences)

| Runtime | Version | Licence | Used for | How it is obtained |
|---|---|---|---|---|
| ONNX Runtime (DirectML build) | 1.24.4 | MIT | RVM, BiRefNet, YuNet (CPU, or GPU through DirectML) | NuGet `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 (`onnxruntime.dll`, `onnxruntime_providers_shared.dll`), shipped next to the editor |
| DirectML | Windows' own (System32, 1.15.6 here) | Windows component | GPU execution of the ONNX models | **not shipped**: the proprietary DirectML redistributable (`Microsoft.AI.DirectML`) is not used; Windows' DirectML.dll is a system library |
| ncnn | 20260526 (vcpkg) | BSD-3-Clause | Real-ESRGAN, RIFE (CPU, or GPU through Vulkan) | built from source by vcpkg |
| whisper.cpp + ggml | 1.8.6 + 0.26.0 (vcpkg) | MIT | Speech to Text (CPU, or GPU through Vulkan) | built from source by vcpkg |
| Vulkan loader / headers, glslang, shaderc | vcpkg | Apache-2.0 / BSD / MIT | GPU back ends of ncnn and ggml | built from source by vcpkg; the GPU drivers provide the Vulkan implementation |

Windows ML (Windows App SDK) was evaluated: it selects execution providers delivered through Windows Update and needs
the Windows App SDK runtime. MeerCut uses ONNX Runtime with Windows' DirectML instead, which needs no extra runtime,
works on both GPUs of the test laptop and keeps every shipped binary open source.

## Models

All weights are downloaded from the upstream release by the user; MeerCut does not redistribute them. The code and
weight licences were checked separately; where a project states no separate licence for its weights, this is said.

| Id | Model | Used for | Upstream release (pinned) | Size | Code licence | Weights licence |
|---|---|---|---|---|---|---|
| `rvm-mobilenetv3` | Robust Video Matting, MobileNetV3, fp32 ONNX (opset 12) | Background Removal (people, video, temporally stable) | github.com/PeterL1n/RobustVideoMatting release v1.0.0 | 14.3 MB | GPL-3.0 | not stated separately; released with the GPL-3.0 repository (treated as GPL-3.0, compatible with MeerCut's GPL-3.0-or-later) |
| `birefnet-general-tiny` | BiRefNet general, Swin-T backbone, ONNX (optional) | Background Removal of general objects (pictures, short clips) | github.com/ZhengPeng7/BiRefNet release v1 | 213.6 MB | MIT | MIT (released by the author) |
| `yunet-face` | YuNet face detector 2023mar, ONNX | Auto Reframe | huggingface.co/opencv/face_detection_yunet @ 3cc26e7 (OpenCV Zoo) | 0.2 MB | MIT | MIT |
| `whisper-base` | Whisper base, multilingual, ggml | Speech to Text (default) | huggingface.co/ggerganov/whisper.cpp @ 5359861 | 141.1 MB | MIT | MIT (OpenAI Whisper) |
| `whisper-base.en` | Whisper base.en, English only (optional) | Speech to Text | same | 141.1 MB | MIT | MIT |
| `whisper-tiny` | Whisper tiny, multilingual (optional) | Speech to Text (fastest) | same | 74.1 MB | MIT | MIT |
| `whisper-small` | Whisper small, multilingual (optional) | Speech to Text (most accurate offered) | same | 465.0 MB | MIT | MIT |
| `rife-v4.6` | RIFE 4.6, ncnn | AI Interpolation | github.com/nihui/rife-ncnn-vulkan @ a7532fc, models/rife-v4.6 | 10.1 MB | MIT | MIT (ncnn conversion in the MIT repository; not stated separately) |
| `realesrgan` | Real-ESRGAN x4plus + realesr-animevideov3 x2/x3/x4, ncnn | Video Upscaling | github.com/xinntao/Real-ESRGAN release v0.2.5.0, the models of `realesrgan-ncnn-vulkan-20220424-windows.zip` (its program files are not used) | 43.4 MB download, 36.0 MB installed | BSD-3-Clause | not stated separately; released with the BSD-3-Clause repository |

Credits: Lin et al., *Robust High-Resolution Video Matting with Temporal Guidance* (WACV 2022); Zheng et al.,
*Bilateral Reference for High-Resolution Dichotomous Image Segmentation* (CAAI AIR 2024); Wu, Peng, Yu, *YuNet*
(MIR 2023); Radford et al., *Robust Speech Recognition via Large-Scale Weak Supervision* (OpenAI 2022), ggml
conversion by the whisper.cpp project; Huang et al., *Real-Time Intermediate Flow Estimation for Video Frame
Interpolation* (ECCV 2022), ncnn port by nihui; Wang et al., *Real-ESRGAN* (ICCVW 2021).

Not used, and why: BRIA RMBG (weights licensed for non-commercial use only); CUDA / TensorRT builds (proprietary
runtimes, several hundred MB, NVIDIA only); the DirectML redistributable (proprietary licence); community ONNX exports
of Real-ESRGAN / RIFE (no upstream provenance); models hosted only on Google Drive (no reproducible, hash-pinned URL).

## SHA-256 of every installed file

| Model | File | Bytes | SHA-256 |
|---|---|---|---|
| rvm-mobilenetv3 | rvm_mobilenetv3_fp32.onnx | 14975696 | 88d4531297118f595bf2fd60f6f566aec2e559393802d1f436c380f0cbbd2828 |
| birefnet-general-tiny | BiRefNet-general-bb_swin_v1_tiny-epoch_232.onnx | 224005088 | 5600024376f572a557870a5eb0afb1e5961636bef4e1e22132025467d0f03333 |
| yunet-face | face_detection_yunet_2023mar.onnx | 232589 | 8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4 |
| whisper-base | ggml-base.bin | 147951465 | 60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe |
| whisper-base.en | ggml-base.en.bin | 147964211 | a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002 |
| whisper-tiny | ggml-tiny.bin | 77691713 | be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21 |
| whisper-small | ggml-small.bin | 487601967 | 1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b |
| rife-v4.6 | flownet.param | 16532 | 724569596bcd1e7b9fa50455c604777ebed99746d2ef40aa86e31b5725f1053c |
| rife-v4.6 | flownet.bin | 10614320 | f334ed2260149ce0188a6dcf049844e8b0cdd912e01cbcfb63553157d2508958 |
| realesrgan | realesrgan-x4plus.param | 116029 | 35330ececcea33b6c397a72548e788d5d53becee4734c50b7fada36e89f10a86 |
| realesrgan | realesrgan-x4plus.bin | 33424520 | 713ee713b0353afaa27976f0563a64a5043bd70b9bd8936c2e26e25ebcdbcddf |
| realesrgan | realesr-animevideov3-x2.param | 3173 | b88ff4f00ebf019a7fdac17fdd45a7fd3665d37509efc5baf2e4da2e24420a04 |
| realesrgan | realesr-animevideov3-x3.param | 3173 | d1a5755008791d09b57e3425fc9dd0bd26b00fdf79c606210bc0e693f8230881 |
| realesrgan | realesr-animevideov3-x4.param | 3077 | 850a248e7c14c27e5bd8cf7265113a9441036a7db63963bb8aa5169d788a435e |
| realesrgan | realesr-animevideov3-x2/x3/x4.bin (one file, three copies) | 1247368 | 548a36f9c3f4ab8da56cd3b13badf23968bee207b396dad14d04b830e5f2ab2d |
| (archive) | realesrgan-ncnn-vulkan-20220424-windows.zip | 45474481 | abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d |

The four Whisper hashes are identical to the SHA-256 that Hugging Face lists for those files (Git LFS object ids).
The others were recorded from the upstream release on 2026-10-21 (GitHub lists no digests for those assets).

## Test data (development only, not distributed)

`tools/fetch_m7_testdata.py` downloads into `testmedia/ai/` (media git-ignored; `manifest.json` with every SHA-256 and
the transcripts are tracked): 73 LibriSpeech dev-clean utterances (CC BY 4.0), 10 FLEURS test utterances each in
Spanish, French and Arabic (CC BY 4.0), `jfk.wav` (public domain), the Sintel trailer (CC BY 3.0, Blender Foundation)
and a 60 s excerpt of Tears of Steel (CC BY 3.0, Blender Foundation).

## Performance and limitations

### Summary per model (reference laptop: i5-12450H, RTX 2050 4 GB, Intel UHD; warm = after the first frame)

| Model (id, version) | Runtime, devices | Download / installed | Cold start | Warm speed | Peak RAM | Peak VRAM | Accuracy (measured here) | Unsupported / limits |
|---|---|---|---|---|---|---|---|---|
| Robust Video Matting (`rvm-mobilenetv3`, v1.0.0 fp32) | ONNX Runtime: DirectML (either GPU) or CPU | 14.3 MB | session 0.8–1.8 s + first frame 0.8–1.2 s | 1280 × 534, quality fast / standard / high: DirectML 29–37 / 42–59 / 65–75 ms (13–34 fps), CPU 49–50 / 79–82 / 131–140 ms | 445 MB (DirectML), 315–344 MB (CPU) | 157–252 MB | people alpha 0.991 over 48 frames, sky 0.000; matte flicker 0.0063 (recurrent) | alpha video is not exported; still pictures cannot be backgrounds |
| BiRefNet general tiny (`birefnet-general-tiny`, v1) | ONNX Runtime: CPU (GPU only above 4.5 GB free) | 213.6 MB | 23–33 s first frame (CPU) | ~41 s per frame (CPU) | 4.9 GB | out of memory on both GPUs | people 1.000 / 0.987, sky 0.000 | **not practical for video on this laptop**: asked first with the time; GPUs with ≤ 4.5 GB free |
| YuNet (`yunet-face`, 2023mar) | ONNX Runtime: DirectML or CPU | 0.2 MB | < 1 s | Auto Reframe analysis of 7 s (faces every 0.2 s + scene detection): 1.5–4.5 s | - | small | boxes within 0.003 of OpenCV's YuNet; faces inside the 9:16 crop 27 / 27 | faces only (other subjects: an M5 motion track) |
| Whisper tiny / base / small (`whisper-*`, ggml) + base.en | whisper.cpp: Vulkan GPU or CPU | 74 / 141 / 465 MB (base.en 141 MB) | model load 0.3–1.5 s | GPU 9× / 7–9× / 4.4× real time; CPU 1.6× / 0.8× / 0.2× | - | 275 / 394 / 885 MB | WER base (small): LibriSpeech 7.6 % (6.3), FLEURS Spanish 9.3 % (5.5), French 30 % (18), Arabic 38 % (21) | no speaker labels; Debug builds' CPU path ~30× slower |
| RIFE 4.6 (`rife-v4.6`, ncnn) | ncnn: Vulkan GPU or CPU | 10.1 MB | 1.7–1.8 s (7.5 s at the first Vulkan use after a reboot) | 360p / 720p / 1080p: GPU 56 / 214 / 448 ms, CPU 180 / 871 / 1920 ms | GPU 240 / 397 / 593 MB, CPU 375 / 957 / 1552 MB | 103 / 254 / 435 MB | middle frame 36.6 dB (optical flow 34.7, blending 33.6) on 5 real shots; CPU = GPU 61 dB | 4K untested; frames are prepared, not computed during playback |
| Real-ESRGAN x4plus (`realesrgan`, general) | ncnn: Vulkan GPU or CPU | 43.4 MB download (all Real-ESRGAN files), 36.0 MB installed | 1.8–3.7 s | 640 × 360: 4.0 s per frame (GPU), 1280 × 720: 16.7 s (GPU); CPU ~19 s at 320 × 180 (≈ 5 min at 720p) | 360–739 MB (GPU), 2.5 GB (CPU) | 1.5–1.9 GB (192-px tiles) | clean 2x: 36.3 / 39.6 dB (Lanczos 38.7 / 40.1); H.264-compressed: 32.6 / 36.3 dB (Lanczos 32.4 / 35.3) | **impractical on a CPU** (asked first with the time); no 3x |
| Real-ESRGAN animevideov3 (`realesrgan`, anime) | ncnn: Vulkan GPU or CPU | (included above) | 0.9–3.0 s | 2x 640 × 360: 109 ms (GPU), 2.2 s (CPU); 2x 1280 × 720: 455 ms; 3x / 4x 640 × 360: 134 / 167 ms | 246–329 MB (GPU), 602–662 MB (CPU) | 163–269 MB | clean 2x 38.6 / 37.5 dB; compressed 33.4 / 35.1 dB; lifts live action by up to 3.4 levels per channel | trained on drawn pictures |

Scene Detection uses no model (signal analysis): 520 frames/s at 320 px; Sintel trailer 10 / 11 hard cuts frame-exact,
0 false, 3 / 8 gradual transitions.

Measured on the development laptop (i5-12450H, 6 ONNX Runtime threads; RTX 2050 4 GB through DirectML), `fa-ai-bench matte`, Tears of Steel 1280 × 534. *Cold*: creating a session takes 0.8–1.8 s and the first DirectML frame compiles the graph (0.8–1.2 s; ~13 s once after a driver update, when the driver's shader cache is empty); the CPU's first frame is 0.07–0.16 s. *Warm* figures below are the mean of frames 2–48 (inference alone: 25 / 42 / 56 ms on DirectML, 44 / 73 / 126 ms on the CPU; the rest is converting the picture to tensors and back). Graphics memory of the process on the RTX 2050 while RVM runs: 157–252 MB local + ~95 MB shared (DXGI). Standard quality on DirectML measured 42–59 ms per frame in different runs (thermal and power state).

| Model | Device | Time per frame | Peak RAM | Quality (bridge shot) |
|---|---|---|---|---|
| RVM, fast (320 px) | DirectML | 29–37 ms (27–34 fps) | 445 MB | |
| RVM, fast | CPU | 49–50 ms (20 fps) | 315 MB | |
| RVM, standard (480 px) | DirectML | 42–59 ms (17–24 fps) | 446 MB | people 1.000 / 0.991, sky 0.000 |
| RVM, standard | CPU | 79–82 ms (12–13 fps) | 336 MB | |
| RVM, high (640 px) | DirectML | 65–75 ms (13–15 fps) | 456 MB | |
| RVM, high | CPU | 131–140 ms (7–8 fps) | 344 MB | |
| BiRefNet tiny (1024²) | DirectML, RTX 2050 | **out of memory** (reported after ~10 s) | | |
| BiRefNet tiny | DirectML, Intel UHD (shared memory) | **out of memory** (after a 123 s session start) | | |
| BiRefNet tiny | CPU | ~41 s (first frame 23–33 s) | 4.9 GB | people 1.000 / 0.987, sky 0.000 |

**BiRefNet is not a practical video tool on this laptop**, and MeerCut says so instead of starting it silently: both GPUs run out of memory (its ONNX export has a fixed 1024 × 1024 input) and the CPU needs ~41 s and ~5 GB per frame (a 10 s clip at 24 fps: ~2.7 hours). Investigated: the upstream release (github.com/ZhengPeng7/BiRefNet, release v1, 28 assets listed through the GitHub API) has no smaller ONNX - the Swin-T export used here (224 MB) is the smallest; `BiRefNet-general-resolution_512x512-fp16` is a Swin-L model (940 MB), `BiRefNet_lite-general-2K` runs at 2560 × 1440, and the dynamic-resolution models exist only as PyTorch checkpoints (a conversion would have to be hosted by MeerCut, which D84 rules out). U²-Net / IS-Net were not adopted: their weights are distributed through Google Drive or third-party mirrors (no pinned upstream URL with a hash). Behaviour: when a model is not expected to fit the GPU (`cutoutFitsGpu`), *Preview This Frame* and *Analyse Clip* ask first, stating the frames, the CPU time (frames × the measured seconds per frame) and the memory; a GPU that runs out of memory during an analysis stops it with `GpuMemoryError` and the panel asks the same question - the CPU is never chosen silently.

Background Removal limitations: video with an alpha channel cannot be exported (the cutout is real soft alpha inside the project and composites over the tracks below; no M6 export format carries alpha, and adding one would change the exporter and both compositing back ends); still pictures cannot be backgrounds (MeerCut has no still-image clips yet: its FFmpeg has no PNG decoder) - backgrounds are colours or videos; no background blur; RVM is trained on people and ignores other objects.

Speech to Text (whisper.cpp, measured): whisper-base runs at 7–9× real time on the RTX 2050 (Vulkan, peak 394 MB of graphics memory) and 0.8× on the CPU; whisper-small 4.4× / 0.2× (885 MB); tiny 9× / 1.6× (275 MB). Accuracy (WER, base / small): LibriSpeech English 7.6 % / 6.3 %, FLEURS Spanish 9.3 % / 5.5 %, French 30 % / 18 %, Arabic 38 % / 21 %. Limitations: no speaker identification, no translation, no word-level timing; French and Arabic need the small model for usable captions; small on the CPU is slower than real time (0.2×).

AI Interpolation and Video Upscaling (ncnn on Vulkan, measured): RIFE 4.6 takes 56 / 214 / 448 ms per frame at 360p / 720p / 1080p on the RTX 2050 (peak 435 MB of graphics memory at 1080p) and 180 / 871 / 1920 ms on the CPU, so the CPU is a practical fallback (about 4x slower). Real-ESRGAN anime: 109 ms per 640 × 360 frame on the GPU, 2.2 s on the CPU. **Real-ESRGAN general (x4plus) is slow**: 4.0 s per 640 × 360 frame and 16.7 s per 1280 × 720 frame on the RTX 2050 (1.5–1.9 GB of graphics memory with 192-px tiles), about 19 s per 320 × 180 frame on the CPU (2.5 GB RAM) - about 5 minutes per 720p frame, so one second of 720p video would take about 2 hours. It stays available on the GPU with the time shown and confirmed; on a PC without a usable GPU the editor asks with that estimate before anything starts. Untested: 4K (RIFE's memory grows with the picture; an out-of-memory failure is an error, never a silent switch to the CPU), other GPUs. Quality notes: Real-ESRGAN is a restoration model - on clean footage it scores below Lanczos (PSNR / SSIM); on H.264-compressed low-resolution footage the general model scores 0.2–1.0 dB above it and both models have a higher SSIM; the anime model lifts live action by up to ~3 levels per channel; the realesr-animevideov3 2x and 3x files are the 4x network with a resize at the end (same weights).

Auto Reframe (YuNet, measured): analysing 7 s of Tears of Steel (faces every 0.2 s, every frame through the scene detector) takes 1.5–4.5 s on DirectML. Scene Detection uses no model (520 frames/s at 320 px).

### Licence inventory and compatibility (MeerCut: GPL-3.0-or-later; checked 2026-10-22)

| Component | Licence | Shipped with MeerCut | Compatible with distributing MeerCut | Obligations / open points |
|---|---|---|---|---|
| ONNX Runtime 1.24.4 (DirectML build) | MIT | yes (`onnxruntime.dll`, `onnxruntime_providers_shared.dll`) | yes | include its LICENSE and `ThirdPartyNotices.txt` from the NuGet package; telemetry events are disabled at run time |
| DirectML.dll | Windows component | no (Windows' own, System32) | yes (system library) | the redistributable DirectML package is not used (proprietary licence) |
| ncnn 20260526 | BSD-3-Clause | yes (vcpkg build) | yes | include its licence text |
| whisper.cpp 1.8.6 + ggml 0.26.0 | MIT | yes | yes | include the licence texts |
| Vulkan loader, glslang, shaderc (vcpkg) | Apache-2.0 / BSD-3-Clause / MIT | yes, where linked | yes (Apache-2.0 is compatible with GPL-3.0) | include the licence and NOTICE texts |
| RVM weights (`rvm_mobilenetv3_fp32.onnx`) | none stated for the weights; RVM code GPL-3.0 (MeerCut uses none of RVM's code) | no: downloaded by the user from the upstream release | not distributed, so no conflict | **open**: the training data includes Adobe Image Matting and Distinctions-646 (obtained from their authors); whether the weights may be used commercially is not established - say so to users before release |
| BiRefNet general tiny | MIT | no | not distributed | training data (DIS5K) terms not verified |
| YuNet 2023mar | MIT | no | not distributed | credit the authors |
| Whisper (ggml) | MIT (code and weights) | no | not distributed | none beyond attribution |
| RIFE 4.6 (ncnn) | MIT (repositories); weights not separately stated | no | not distributed | training data (Vimeo-90K) terms not verified |
| Real-ESRGAN (ncnn files of v0.2.5.0) | BSD-3-Clause (repository); weights not separately stated | no | not distributed | training data (DIV2K, Flickr2K, OST, anime videos) terms not verified |

Conclusion: nothing MeerCut ships conflicts with GPL-3.0-or-later; no model is redistributed (each is fetched from its
upstream release on the user's request), so the models' terms bind the user's use, not MeerCut's distribution. Before
release planning (M8): ship the runtimes' licence and notice files with the installer, and resolve or clearly label the
commercial-use status of the RVM weights and the unverified training-data terms above.

### Licence evidence (checked 2026-10-21)

* **RVM**: the README states "Code is re-released under GPL-3.0 license"; the v1.0.0 release carries the weights without a separate licence statement. Its training documentation lists VideoMatte240K, ImageMatte (Distinctions-646 and Adobe Image Matting, "contact their authors to acquire"), DVM backgrounds, COCO, YouTubeVIS 2021 and Supervisely Person: the terms of those datasets are their providers'. MeerCut does not redistribute the weights (the user downloads them from the upstream release), and its own code is GPL-3.0-or-later, so the GPL code licence is compatible; whether the *weights* may be used commercially is not stated by the authors - recorded as unresolved.
* **BiRefNet**: repository LICENSE "MIT License, Copyright (c) 2024 ZhengPeng"; the Hugging Face model card's metadata says `license: mit`; the general model is trained on DIS-TR (DIS5K) - that dataset's terms were not verified. Not redistributed by MeerCut.
* **YuNet**: the OpenCV Zoo model card (huggingface.co/opencv/face_detection_yunet) states "All files in this directory are licensed under MIT License"; credit Wu, Peng, Yu (libfacedetection).
* **RIFE 4.6**: rife-ncnn-vulkan (nihui) LICENSE "MIT License"; the ncnn files `models/rife-v4.6/flownet.{param,bin}` are part of that repository at the pinned commit a7532fc; they are converted from hzwer's Practical-RIFE v4.6, whose repository is MIT-licensed (ECCV2022-RIFE likewise). No separate licence for the weights is stated. Training data (Vimeo-90K) terms not verified. Downloaded from the pinned commit by the user; not redistributed.
* **Real-ESRGAN**: xinntao/Real-ESRGAN LICENSE "BSD 3-Clause License"; the models are assets of its own release v0.2.5.0 (`realesrgan-ncnn-vulkan-20220424-windows.zip`; only the `.param` / `.bin` files are installed, its executable is not used). No separate licence for the weights is stated. Training data (DIV2K, Flickr2K, OST; anime videos for animevideov3) terms not verified. Downloaded by the user; not redistributed.
* **Whisper**: the OpenAI repository's LICENSE is "MIT License, Copyright (c) 2022 OpenAI" and its README states "Whisper's code and model weights are released under the MIT License"; the ggml files (huggingface.co/ggerganov/ whisper.cpp, `license: mit`) are those weights converted to ggml format.
