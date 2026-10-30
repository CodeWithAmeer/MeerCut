# Changelog

All notable changes to MeerCut are listed here. Versions follow [Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-10-29

The first public release. MeerCut was developed before this in a private repository, in milestones that were each
tested before the next began; see *How MeerCut was made* in the [README](README.md).

### Editing
- Projects with any resolution and frame rate, a media pool, and a multitrack timeline with snapping, split, ripple
  delete, trim, In / Out marks, unlimited undo and "Delete this gap".
- Clipboard (Ctrl+X / C / V / D), a clip menu and 13 colour marks.
- Atomic saves, autosave and crash recovery.

### Picture, text and effects
- Transform with handles in the player, keyframes, masks and 14 blend modes.
- 19 transitions on a Direct3D 11 compositor with a CPU renderer that matches it.
- Titles and captions with 54 templates and 36 animations, right-to-left and complex scripts, SRT and ASS / SSA.
- 30 effects and 46 filters (including 43 looks), adjustment layers, masked effects, colour tools (Basic, HSL,
  Curves, Colour Wheels, .cube LUTs) and HDR-to-SDR tone mapping.

### Time, motion and audio
- Speed changes, reverse, freeze frames, speed ramps, frame blending and optical flow.
- Motion tracking and stabilisation, and Render Preview.
- Volume automation, fades, crossfades, a 10-band EQ, auto ducking, noise and hum reduction, loudness normalisation
  (ITU-R BS.1770-4), a limiter and a level meter.

### Export
- MP4, MOV, MKV, WebM, GIF, WAV and FLAC with H.264, HEVC, AV1, VP9 and ProRes.
- NVIDIA NVENC and Intel Quick Sync encoders, with a reported CPU fallback.
- The headless renderer `fa-render`.

### Local AI
- Background Removal, Speech to Text, Auto Reframe, AI Interpolation and Video Upscaling, running on the GPU
  (DirectML or Vulkan) or the CPU.
- Models are downloaded only on request and are checked by SHA-256.
- Scene Detection, which uses signal analysis, not AI.

### Library
- 17 music beds, 40 sound effects, 93 stickers, 34 templates and 26 motion presets, all made for MeerCut.
- Media › Sample Videos: five practice clips, four of them from Blender Foundation films (CC BY 3.0).

### Fixed in the final checks before release
- The Audio page's Preview, Add to Timeline and Favorite buttons lay over its search field instead of under the list.
- User-interface text no longer names other products or refers to internal measurement records.
