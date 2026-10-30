# MeerCut 1.0.0

The first public release of **MeerCut**, a free, open-source video editor for Windows 11 by
[CodeWithAmeer](https://github.com/CodeWithAmeer). It covers multitrack editing, titles, 19 transitions, 30 effects,
46 filters, colour grading, an audio mixer, export to the common formats, and AI tools that run on your own PC. There
is no account, no watermark and no telemetry.

## Download

| File | What it is |
|---|---|
| `MeerCut-1.0.0-win64-setup.exe` | **Installer** (recommended). Installs for the current user without administrator rights, or for all users. |
| `MeerCut-1.0.0-win64-portable.zip` | **Portable.** Unzip anywhere and run `MeerCut.exe`. |
| `MeerCut-1.0.0-source.zip` | The complete source code of this release. |
| `SHA256SUMS.txt` | SHA-256 checksums. |

**Check the download.**

* PowerShell: `Get-FileHash .\MeerCut-1.0.0-win64-setup.exe -Algorithm SHA256`, then compare with `SHA256SUMS.txt`.
* Linux, macOS or Git Bash: `sha256sum -c SHA256SUMS.txt`.

**The files are not code-signed.** Windows SmartScreen may show "Windows protected your PC". If the checksum matches,
choose *More info › Run anyway*.

## Requirements

* **Windows 11, 64-bit.** The installer accepts Windows 10 1809 or later, but that is untested.
* **Disk space:** about 200 MB.
* **GPU:** a DirectX 11 GPU is optional; without one the CPU renders the same picture.
* **AI:** the AI tools use DirectML (part of Windows) or Vulkan, or the CPU, which is much slower.
* **AI models are not included.** *AI Tools › Model Manager* downloads the ones you choose, with their licences
  ([AI_MODELS.md](../AI_MODELS.md)).

## Highlights

* **Editing.**
  * A multitrack timeline: snapping, split, ripple delete, trim, In / Out, "Delete this gap", unlimited undo.
  * Keyframes, masks and 14 blend modes.
  * Autosave and crash recovery.
* **Library.**
  * 54 title templates, 19 transitions, 30 effects and 46 filters.
  * 93 stickers, 34 templates, 17 music beds and 40 sound effects.
  * *Media › Sample Videos*: five practice clips.
* **Colour and motion.**
  * HSL, Curves, Colour Wheels and .cube LUTs.
  * Speed ramps, optical flow, motion tracking and stabilisation.
* **Audio.** Automation, crossfades, a 10-band EQ, ducking, noise reduction, loudness normalisation and a limiter.
* **Export.**
  * MP4, MOV, MKV, WebM, GIF, WAV and FLAC.
  * NVIDIA and Intel hardware encoders, with a reported CPU fallback.
* **Local AI.** Background Removal, Speech to Text, Auto Reframe, AI Interpolation and Video Upscaling.

See the [README](../README.md) for the full feature list and screenshots, and the [CHANGELOG](../CHANGELOG.md).

## Known limitations

* Windows 11 x64 only. No HDR or alpha-channel export. AMD AMF encoders are not built.
* Tested on one laptop: Intel Core i5-12450H, 8 GB RAM, Intel UHD Graphics + NVIDIA GeForce RTX 2050. Other GPUs,
  other hardware and Windows 10 are untested. It was not tested on a clean Windows installation.
* Real-ESRGAN x4plus upscaling is slow: about 17 s per 720p frame on an RTX 2050, and minutes per frame on a CPU.
* Scene Detection finds hard cuts reliably, slow dissolves less reliably.
* The commercial-use status of some AI model weights is unclear (see [AI_MODELS.md](../AI_MODELS.md)). The weights
  are not distributed with MeerCut.

## Credits

The sample videos *Bridge Conversation* and *Sci-Fi Lab Montage* are excerpts of *Tears of Steel*, (c) Blender
Foundation | mango.blender.org. *Snowy Adventure* and *Dragon and Desert* are excerpts of *Sintel*, (c) Blender
Foundation | durian.blender.org. Both films are licensed under [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/).
MeerCut is not affiliated with or endorsed by the Blender Foundation.

MeerCut is licensed under the GPL-3.0-or-later. The third-party components and their licences are listed in
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) and included in the packages.
