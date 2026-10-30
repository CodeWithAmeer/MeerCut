# COLOR_MANAGEMENT.md — MeerCut colour pipeline

MeerCut is an **SDR, Rec.709** editor. This document defines exactly what happens to colour from decoding to export,
what the effects and LUTs operate on, and how non-Rec.709 / HDR sources are treated. It makes no HDR claims.

## 1. Pipeline

| Stage | What happens | Where |
|---|---|---|
| Decode | YUV → 8-bit R'G'B' with the stream's own matrix (BT.709 / BT.601 / BT.2020 / SMPTE 240M / FCC; untagged: 709 at ≥ 720 lines, else 601) and range (limited or full) | `media/video_decoder.cpp`, `probe.cpp` |
| Working space | **Gamma-encoded Rec.709 R'G'B'**, straight alpha, 0..1 (no linearisation) — the space in which MeerCut's colour tools, blend modes and dissolves are defined | `render/` |
| Effects | float (32-bit) per channel; spatial passes (blur, sharpen) premultiplied; **every effect clamps its result to 0..1**; strength / masks mix in float | `effects_cpu.cpp`, `d3d11_backend.cpp` |
| Compositing | RGBA8 canvas (M0–M3 rules: opacity, blend modes, transitions in encoded space) | `cpu_backend.cpp`, GPU composite shader |
| Rounding | one 8-bit rounding at the end of a layer's effect chain | both backends |
| Encode | 8-bit R'G'B' → **BT.709 limited-range** YUV 4:2:0; streams tagged `bt709 / bt709 / bt709 / tv` | `media/video_encoder.cpp` |

Preview and export run the same `renderFrame()`; preview frames are rendered at the player size (sizes in effects
are defined per 1080 lines of picture height, so the look is the same at every size).

## 2. CPU / GPU tolerance

The Direct3D 11 path mirrors the CPU reference operation by operation in 32-bit float. Allowed difference: **at most
1 level (of 255) per channel**, measured on both adapters (RTX 2050, Intel UHD) for every effect with a GPU pass
(`EffectsGpuParity.*`); a negative control proves the comparison detects a one-unit parameter difference. Most
colour effects are bit-identical (max 0).

## 3. Alpha

Effects see straight (unpremultiplied) colour; transparent pixels keep their colour values but never bleed into
neighbours in spatial effects (premultiplied passes). Colour effects never change alpha.

## 4. LUTs

* Format: `.cube` (Adobe / Resolve): 3-D (2..129 per axis), 1-D (2..65536), or 1-D shaper + 3-D; `TITLE`,
  `DOMAIN_MIN/MAX`, `LUT_1D/3D_INPUT_RANGE`, comments, BOM, CRLF. Video-range LUTs (`LUT_IN/OUT_VIDEO_RANGE`) are
  rejected with a message rather than applied wrongly. Validation errors name the line.
* Applied to the working space (gamma-encoded Rec.709). A LUT made for log footage (V-Log, S-Log …) only looks right
  on footage that really is in that log encoding — MeerCut does not convert.
* Interpolation: **tetrahedral** (3-D), linear (1-D).
* Output clamped to 0..1; strength mixes the LUT result with the input.
* **Domain** (`DOMAIN_MIN/MAX` other than 0..1) — three behaviours exist (identity 2³ table, `DOMAIN_MAX 0.5`):

  | Implementation | Result | MeerCut option |
  |---|---|---|
  | `.cube` specification (Adobe, Resolve) | input 0..0.5 spans the table → `min(1, 2x)` | **Standard** (default) |
  | some editors | output scaled into the domain → `x / 2` | **Output scale** |
  | FFmpeg 9.0.1 `lut3d` | input scale clipped to ≤ 1 → `x` (domain ignored) | — |

  For ordinary LUTs (domain 0..1) all three agree: on a 216-patch chart every FFmpeg `lut3d` value lies within the
  integer bracket of MeerCut's result (FFmpeg truncates, MeerCut rounds).

## 5. HDR and wide-gamut sources

MeerCut **does not edit or output HDR**. What it does with HDR footage, so that it is safe rather than wrong:

* **Detection:** a video stream whose transfer characteristic is **PQ** (SMPTE ST 2084, `smpte2084`) or **HLG**
  (ARIB STD-B67, `arib-std-b67`) is HDR (`VideoStreamInfo::isHdr`, from the stream's colour tags).
* **Import** says so: *"PQ (SMPTE ST 2084) HDR video: MeerCut is an SDR editor - it shows and exports this as SDR
  Rec.709 with a simple tone mapping"*. **Export preflight** repeats it as a warning.
* **Conversion on decoding** (`media/hdr.cpp`, the same for preview, thumbnails and export):
  1. YUV → 16-bit R'G'B' with the stream's matrix (BT.2020) and range;
  2. EOTF to display light: PQ absolute nits; HLG inverse OETF + BT.2100 OOTF for a 1000-nit display (γ 1.2);
  3. BT.2020 → BT.709 primaries in linear light; colours outside Rec.709 are clipped;
  4. tone mapping relative to **HDR reference white 203 nits** (ITU-R BT.2408) = SDR 100 %: unchanged below 75 %,
     above it the brightest channel is compressed smoothly (extended Reinhard) so the content peak (1000 nits) lands
     on 100 %; all channels scaled together (hue kept);
  5. inverse BT.1886 (γ 2.4) → 8-bit.
  Reference white comes out at **241/255** (87.5 % light), leaving headroom for highlights.
* **Verified** (`HdrTransfer.*`, `HdrToneMap.*`, `HdrToSdrConversion.*`, `HdrDecode.*`, GUI `m4LutsAndHdr`): PQ/HLG
  transfer functions at their standard points (100 / 203 / 1000 / 10000 nits; HLG 0.5 → 1/12); 10-bit BT.2020 PQ and
  HLG test clips with exact code values (`tools/make_testmedia.ps1`) decode to neutral 241 for reference white and to
  the model's value ±3 for a saturated red; the preview shows 241/241/241; preview vs export 1.63/255.
* **Limits:** a simple, fixed tone mapping (no scene analysis, no per-shot peak from metadata, no HDR10+/Dolby Vision
  dynamic metadata); wide-gamut colours are clipped, not compressed; 8-bit output can band in smooth HDR gradients.
  BT.2020 **SDR** footage (non-PQ/HLG) is decoded with the BT.2020 matrix but is not gamut-converted to BT.709.

## 6. Evaluation: OpenColorIO and FFmpeg's LUT filter

**OpenColorIO 2.x** (BSD-3-Clause, ASWF) — evaluated from its documentation and design, **not installed or
measured** here (it is not in this build and adding it is a dependency decision for the project owner):

| Aspect | OpenColorIO | MeerCut |
|---|---|---|
| Scope | full colour management: config files (ACES etc.), colour spaces, displays/views, file transforms (.cube, .3dl, .csp, CLF …) | SDR Rec.709 working space + .cube LUTs |
| CPU / GPU | CPU processor + generated GLSL/HLSL shader text | hand-written CPU reference mirrored op-by-op in HLSL; parity tested (max 0–1 level) |
| .cube | reads Resolve/Iridas cube; tetrahedral interpolation selectable; domain per the format | tetrahedral; spec domain + an output-scale option |
| Dependencies | yaml-cpp, expat, pystring, Imath, minizip-ng (vcpkg port exists) | none beyond the existing code (≈ 450 lines) |
| Testability here | parity between its CPU and GPU paths is OCIO's responsibility; our pixel tests would compare against it | every step unit-tested (CPU / GPU parity, reference measurements, FFmpeg) |

**Decision:** not adopted. MeerCut's colour needs (SDR Rec.709, .cube, exact CPU/GPU parity, measured reference
behaviour) are met by the small in-house implementation. OCIO becomes worth its weight if MeerCut adds managed
workflows (ACES, log camera inputs with IDTs, HDR output, display calibration).

**FFmpeg 9.0.1 `lut3d`** — measured on a 216-patch chart with the test LUTs in `testmedia/luts/`:

* Interpolation: tetrahedral (default) agrees with MeerCut — every FFmpeg 8-bit value lies inside the integer bracket
  of MeerCut's float result (0 of 648 outside, for identity 33³, invert 2³, random 3³).
* Quantisation: FFmpeg **truncates** to 8 bits instead of rounding (mean bias −0.5…−0.74 levels); MeerCut rounds.
* Domain: FFmpeg clips its input scale to ≤ 1, so a `DOMAIN_MAX` below 1 is ignored (§4 table).
* FFmpeg's LUT filter is not used by MeerCut (avfilter is not part of the build); it serves as an independent check.

## 7. What is not colour-managed

* Titles and captions are drawn in the working space (sRGB-like R'G'B', no conversion).
* Exports are always 8-bit 4:2:0 BT.709 limited range.
