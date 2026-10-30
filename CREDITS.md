# Credits and attribution

MeerCut is made by **CodeWithAmeer**. Official GitHub: <https://github.com/CodeWithAmeer>.

## Media in the sample videos, the screenshots and the tests

The sample videos, the screenshots in `docs/screenshots/` (which show frames of the sample videos) and some tests use
works by others. All of them are under licences that allow this with attribution:

| Work | Author / source | Licence | Used in |
|---|---|---|---|
| *Sintel* (trailer, 720p) | (c) copyright Blender Foundation, durian.blender.org | [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) | AI tests (downloaded by `tools/fetch_m7_testdata.py`) |
| *Tears of Steel* (60 s excerpt from 00:00:20, 720p) | (c) copyright Blender Foundation, mango.blender.org | [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) | AI tests (downloaded by `tools/fetch_m7_testdata.py`) |
| LibriSpeech dev-clean, speaker 1272 (10 utterances; transcripts committed in `testmedia/ai/librispeech/`) | V. Panayotov, G. Chen, D. Povey, S. Khudanpur, openslr.org/12 | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) | Speech to Text tests |
| FLEURS (Arabic, Spanish, French samples; transcripts committed in `testmedia/ai/fleurs/`) | Google, A. Conneau et al. | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) | Speech to Text tests |
| `jfk.wav` (J. F. Kennedy inaugural address excerpt) | from whisper.cpp v1.8.6 `samples/` | public domain | Speech to Text tests |
| *Tears of Steel*: "Bridge Conversation" and "Sci-Fi Lab Montage" (frames 0-485 and 486-1066 of the 60 s excerpt above) | (c) copyright Blender Foundation, mango.blender.org | [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) | Media > Sample Videos (`editor/assets/samples/`, shipped with MeerCut); `docs/screenshots/` |
| *Sintel* trailer: "Snowy Adventure" and "Dragon and Desert" (frames 0-570 and 571-1133) | (c) copyright Blender Foundation, durian.blender.org | [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) | Media > Sample Videos (`editor/assets/samples/`, shipped with MeerCut); `docs/screenshots/` |

The sample videos are cut at shot changes by `tools/make_sample_videos.py`, with 30 ms sound fades at the cuts;
the *Tears of Steel* clips are letterboxed to 1280x720 like the *Sintel* trailer, and all are re-encoded. Nothing else is
changed. The app shows these credits under Media > Sample Videos. MeerCut is not affiliated with or endorsed by the
Blender Foundation. "Green Screen Practice" is MeerCut's own clip, drawn by the same script.

Every other test video, picture, LUT and sound was generated for this project by `tools/make_testmedia.ps1`,
`tools/make_s3_testmedia.ps1`, `tools/make_lut_tests.py`, `tools/make_chroma_chart.py` and `tools/make_motion_tests.py`,
using FFmpeg's test sources. The third-party recordings are not committed: `tools/fetch_m7_testdata.py` downloads them
and checks them against `testmedia/ai/manifest.json`, which records each file's SHA-256, source and licence.

Changes: in the screenshots, the sample videos are shown edited with MeerCut's own features (trimmed, with a title,
transitions, an effect, a look and AI background removal).

## Built-in library (music, sound effects, stickers, templates, motion presets)

Everything in MeerCut's Audio, Stickers and Templates pages and its Animation tab was made for MeerCut and is
released with it under its licence (GPL-3.0-or-later). None of it is copied, sampled or traced from Filmora or any
other product or library.

* **Music and sound effects** (`editor/assets/audio/`) are synthesised by `tools/make_audio_library.py`. The script
  uses NumPy oscillators, filtered noise, plucked-string models and a simple reverb, with no samples or recordings,
  and FFmpeg encodes the result to AAC. Re-running the script gives the same audio.
* **Stickers** are drawn with QPainter by `editor/src/text/stickers.cpp`.
* **Templates** are built from MeerCut's own titles, stickers and placeholders by `editor/src/app/templates.cpp`.
* **Sample videos** (Media › Sample Videos): "Green Screen Practice" is MeerCut's own, drawn by
  `tools/make_sample_videos.py`. The other four are the credited Blender Foundation excerpts in the table above, not
  MeerCut's work.
* **Motion presets** are keyframe recipes in `editor/src/model/motion_presets.cpp`.

The category names (for example "Subscribe", "Countdown" and "Split Screen") are the ordinary names such content goes
by.

## AI models

MeerCut does not distribute any model. Their authors, papers, code licences and weight licences are listed in
[AI_MODELS.md](AI_MODELS.md), and the Model Manager shows them before anything is downloaded.

## Software

MeerCut is built on Qt 6, FFmpeg (with x264, x265, libvpx, SVT-AV1, dav1d, Opus, libvpl, nv-codec-headers), OpenCV,
HarfBuzz, FreeType, ONNX Runtime, ncnn, whisper.cpp / ggml, the Vulkan loader, miniaudio, nlohmann/json and their
dependencies. The licence text of every shipped component is in `licenses/THIRD_PARTY_NOTICES.txt` of each release,
generated by `editor/scripts/package.ps1` from the components' own copyright files.

The installer is built with [Inno Setup](https://jrsoftware.org/isinfo.php) (Jordan Russell and Martijn Laan).
