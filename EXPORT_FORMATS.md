# EXPORT_FORMATS.md — MeerCut export formats, encoders and licences

The formats are MeerCut's own list. Whether each one can be made **on a given machine** is decided at run time:
an encoder must be compiled into the deployed FFmpeg **and** open a real session that encodes frames. Hardware
encoders are never assumed from the GPU model. `fa-probe --encoders` prints the live matrix; the table below is that
output on the development laptop.

## Formats

| Id | Container (muxer) | Video | Audio (first = default) | Bit depth |
|---|---|---|---|---|
| `mp4-h264` | MP4 (`mp4`) | H.264 | AAC | 8 |
| `mp4-hevc` | MP4 | HEVC (`hvc1`) | AAC | 8, 10 (10 needs a hardware encoder in this build) |
| `mov-hevc` | MOV (`mov`) | HEVC (`hvc1`) | AAC, PCM 16, PCM 24 | 8, 10 (as above) |
| `mp4-av1` | MP4 | AV1 | AAC, Opus | 8, 10 |
| `mkv-av1` | Matroska (`matroska`) | AV1 | Opus, AAC, FLAC | 8, 10 |
| `webm-vp9` | WebM (`webm`) | VP9 | Opus | 8, 10 (10 needs a hardware encoder in this build) |
| `mov-prores` | MOV | Apple ProRes 422 via `prores_ks` (Proxy / LT / 422 / HQ) | PCM 16, PCM 24, AAC | 10 (4:2:2) |
| `gif` | GIF | GIF (one 256-colour median-cut palette for the whole export, ordered dithering) | none | 8 |
| `wav` | WAV (RF64 above 4 GB) | — | PCM 16, PCM 24 | — |
| `flac` | FLAC | — | FLAC 16 / 24 | — |

Every video stream is SDR **BT.709** (primaries, transfer and matrix tagged), limited ("tv") range, progressive, chroma
sited left where the codec stores it (H.264 / HEVC / AV1 / VP9), constant frame rate. MP4 / MOV video tracks use a
1000 × fps time scale (1/30000 at 30 fps, 1/24000 at 23.976). MP4 files get the `mp42` major
brand and `creation_time` tags. **No HDR:** 10-bit output is the same 8-bit RGB pipeline written with 10-bit
precision; nothing is tagged PQ / HLG / BT.2020.

## Encoders on this machine (RTX 2050 driver 610.78, Intel UHD Graphics 31.0.101.3688, FFmpeg 9.0.2)

| Encoder | Family | Build | 8-bit | 10-bit | Licence / runtime |
|---|---|---|---|---|---|
| `libx264` | CPU | yes | works | — | x264, GPL-2.0-or-later |
| `h264_nvenc` | NVENC | yes | works | — | nv-codec-headers (MIT); encoder in the NVIDIA driver |
| `h264_qsv` | Quick Sync | yes | works | — | oneVPL dispatcher (MIT); encoder in the Intel driver |
| `h264_amf` | AMF | **no** | — | — | not built (vcpkg `amd-amf` archive failed its hash check) |
| `libx265` | CPU | yes | works | **fails** ("Invalid argument": vcpkg's x265 is 8-bit only) | x265, GPL-2.0-or-later |
| `hevc_nvenc` | NVENC | yes | works | works | as above |
| `hevc_qsv` | Quick Sync | yes | works | works | as above |
| `libsvtav1` | CPU | yes | works | works | SVT-AV1, BSD-3-Clause-Clear + AOMedia patent licence |
| `av1_nvenc` | NVENC | yes | **fails** (the RTX 2050 has no AV1 encoder) | fails | as above |
| `av1_qsv` | Quick Sync | yes | **fails** ("Function not implemented": no AV1 encoding on this iGPU) | fails | as above |
| `libvpx-vp9` | CPU | yes | works | **fails** (vcpkg's libvpx is built without high bit depth) | libvpx, BSD-3-Clause + WebM patent grant |
| `vp9_qsv` | Quick Sync | yes | works (see the notes below) | works | as above |
| `prores_ks` | CPU | yes | — | works | FFmpeg, LGPL-2.1-or-later |
| `gif` | CPU | yes | works | — | FFmpeg, LGPL-2.1-or-later (palette and dithering: MeerCut) |
| `aac`, `pcm_s16le`, `pcm_s24le`, `flac` | audio | yes | works | | FFmpeg, LGPL-2.1-or-later |
| `libopus` | audio | yes | works | | Opus, BSD-3-Clause |

Decoding added with the rebuild: dav1d (BSD-2-Clause) for AV1 input. The FFmpeg build as a whole is GPL
(`--enable-gpl` for x264 / x265), compatible with MeerCut's GPL-3.0-or-later. No non-free component (no fdk-aac).
Patent licensing of H.264 / HEVC / AAC is the distributor's concern; AV1, VP9 and Opus come with royalty-free patent
licences from their bodies.

**Choosing an encoder** (`chooseVideoEncoder`): *CPU* = the CPU encoder. *NVENC / Quick Sync / AMF* = that encoder if
its session opens at the export's size, bit depth and frame rate, else the CPU encoder. *Auto* = NVENC, then Quick
Sync, then AMF, then CPU. Every fallback is reported with the reason (export report, `fa-render` output, export
window). If a hardware encoder fails after its test session, the export is redone with the CPU encoder and that is
reported too. A bit depth is offered only where some allowed encoder works (here: 10-bit HEVC and VP9 need Auto,
NVENC or Quick Sync; 10-bit AV1 works on the CPU).

On this laptop Auto picks `h264_nvenc`, `hevc_nvenc`, `vp9_qsv`, and for AV1 falls back to `libsvtav1` (both
hardware AV1 sessions fail; never assumed).

**VP9 on Quick Sync needs three corrections (found by the export quality and memory checks, all in `video_encoder.cpp`):**
1. Its frame headers said colour space *unknown* (only the WebM container said BT.709), so decoders that read the
   bitstream used BT.601 and shifted the colours (the red test clip decoded about 17 levels off). FFmpeg's
   `vp9_metadata` bitstream filter now writes BT.709 / limited range into every frame header.
2. It ignores constant-quality mode (ICQ): quality 1, 8, 16 and 24 all gave the same 0.82 MB file (36 dB), and
   FFmpeg clips its constant-QP mode to QP 51. Its quality value is therefore a VBR target of 0.14 bit per pixel at
   24, doubling every 3.5 steps (Small file .. High: 38.2 .. 44.2 dB on the 720p `m4_effects` example; Standard
   41.2 dB, x264 Standard 41.5 dB).
3. With FFmpeg's default rate-control buffer it never returns from a frame larger than the buffer (noise at 1080p
   2.7 Mbit/s hangs, and so does FFmpeg's own CLI); the buffer is now at least one raw frame and 4 s of the bit rate.
   `ExportFormats.EncoderPathsWriteWhatTheyClaim` encodes noise at Small file and High (it hung before the fix).

## Verification (`tests/e2e/m6_export_e2e.py`)

All 20 exports of the end-to-end test (every format; 10-bit HEVC / AV1 / VP9; ProRes HQ; PCM 24 in MOV; 23.976 fps at 320×180; 44.1 kHz mono;
hardware Auto for H.264 / HEVC / AV1 / VP9) pass: codec, profile, pixel format, BT.709 tags, range, chroma location,
progressive, **the same tags in the first decoded frame** (what a decoder takes from the bitstream itself),
exact frame rate, time base, decoded frame count, duration; **the clips' colours decoded within 8 levels of the
sources read as BT.709** (measured 1–4); **every frame shows the right source frame
(frame-index strip) and clip colour**; tone levels per clip (lossy ±0.01, lossless ±0.002); the audio switches tone
0.5 ms after the cut in every file (A/V sync); audio and video lengths agree within 25 ms; WAV and FLAC (16-bit)
carry identical samples; creation_time as requested. The negative control must reject two files: an untagged
full-range 4:4:4 H.264, and a correct VP9 export whose frame headers alone were relabelled "unknown" (rejected on the
first frame's colour space and on the decoded colour, 17 levels off).

## Limitations (recorded, not fixed)

* This build's x265 and libvpx are 8-bit only: 10-bit HEVC / VP9 need NVENC or Quick Sync. Fixing it means building
  those libraries with high bit depth (a vcpkg port change); not done yet.
* No AV1 hardware encoding on this laptop (RTX 2050, UHD Graphics): AV1 exports use SVT-AV1 on the CPU.
* AMF is not built (the vcpkg `amd-amf` archive failed its integrity check); there was no AMD GPU to test it on.
* Another frame rate is made by sampling the timeline at the output instants (frames repeat or drop; no blending).
* Opus in MP4 plays in browsers and VLC but not in every player; the export window says so.
* GIF uses one palette for the whole export (no per-frame palettes), so very colourful long GIFs band more.
* The quality presets map to each encoder's own scale; they are not equal quality: at Standard NVENC spends about 2x
  x264's bits for about 5 dB more PSNR, and VP9 on Quick Sync (a bit-rate target) spends more than libvpx on simple
  pictures. Opening a hardware session costs 1–2 s, so short exports finish sooner on the CPU.
* Memory at 1080p is mostly the encoder's: SVT-AV1 about 1.45 GB per export process, x265 about 0.87 GB, x264 0.67 GB,
  NVENC 0.44 GB, Quick Sync 0.28–0.36 GB, ProRes 0.23 GB.

