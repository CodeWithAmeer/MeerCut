"""M6 export formats end to end (EXP-007 parity where Filmora evidence exists, the standards elsewhere).

For every advertised format (and 10-bit, frame-rate / size, hardware-Auto and audio-codec variants) it
  1. renders a two-clip project (A red / 1 kHz, cut to B blue / 1.5 kHz) with fa-render,
  2. checks the container with ffprobe: codec, profile, pixel format, BT.709 primaries / transfer / matrix, limited
     range, chroma location (where the codec carries it), progressive, exact frame rate, time base, frame count,
     duration, brand and creation_time, audio codec / rate / channels, A/V start and length,
  3. decodes every frame and reads the frame-index strip and the clip colour (the right source frame at every output
     frame), and measures the tones (level per clip, the switch at the cut = A/V sync),
  4. compares the H.264 export's tags with Filmora's EXP-001 export (re-probed),
  5. writes everything to <work>/m6_export_results.json (the evidence for TEST_RESULTS / EXP-007).
With --negative-control it checks a deliberately untagged file against the H.264 expectations instead: that must
FAIL (the checker is not vacuous).

usage: python m6_export_e2e.py <fa-render.exe> <work dir> [--testmedia DIR] [--filmora FILE] [--negative-control]
exit code 0 = pass, 1 = verification failure, 2 = setup / render failure
"""
import argparse
import json
import math
import subprocess
import sys
from fractions import Fraction as F
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[3]
sys.path.insert(0, str(REPO / "tools"))
import framecode  # noqa: E402
import numpy as np  # noqa: E402

RATE = F(30)
SIZE = (640, 360)
CREATION = "2026-10-21T10:00:00.000000Z"
# A from 2 s for 1.5 s, then B from 5 s for 1.5 s: 90 frames at 30 fps, cut at 1.5 s.
CLIPS = [("A", "A_red_1080p30_10s.mp4", F(0), F(2), F(3, 2), "red", 1000),
         ("B", "B_blue_1080p30_10s.mp4", F(3, 2), F(5), F(3, 2), "blue", 1500)]
END = F(3)

# id, output name, extra fa-render args, expectations
CASES = [
    ("mp4-h264", "h264.mp4", [], dict(codec="h264", profile="High", pix="yuv420p", audio="aac", chroma="left",
                                      tb="1/30000", brand="mp42")),
    ("mp4-hevc", "hevc.mp4", [], dict(codec="hevc", profile="Main", pix="yuv420p", tag="hvc1", audio="aac",
                                      chroma="left", tb="1/30000", brand="mp42")),
    # 10-bit HEVC / VP9: this build's x265 and libvpx are 8-bit only, so these need a hardware encoder (Auto); on a
    # machine without one fa-render refuses them, which is recorded as "not available here", not as a pass
    ("mp4-hevc", "hevc10.mp4", ["--bit-depth", "10", "--hw", "auto"],
     dict(codec="hevc", profile="Main 10", pix="yuv420p10le", tag="hvc1", audio="aac", tb="1/30000", optional=True)),
    ("mov-hevc", "hevc.mov", ["--audio-codec", "pcm_s24le"], dict(codec="hevc", pix="yuv420p", tag="hvc1",
                                                                  audio="pcm_s24le", chroma="left", tb="1/30000")),
    ("mp4-av1", "av1.mp4", [], dict(codec="av1", pix="yuv420p", audio="aac", tb="1/30000", brand="mp42")),
    ("mkv-av1", "av1_10.mkv", ["--bit-depth", "10"], dict(codec="av1", pix="yuv420p10le", audio="opus")),
    ("webm-vp9", "vp9.webm", [], dict(codec="vp9", pix="yuv420p", audio="opus")),
    ("webm-vp9", "vp9_10.webm", ["--bit-depth", "10", "--hw", "auto"],
     dict(codec="vp9", profile="Profile 2", pix="yuv420p10le", audio="opus", optional=True)),
    ("mov-prores", "prores.mov", [], dict(codec="prores", pix="yuv422p10le", audio="pcm_s16le", tb="1/30000")),
    ("mov-prores", "prores_hq.mov", ["--prores-profile", "3"], dict(codec="prores", profile="HQ", pix="yuv422p10le",
                                                                    audio="pcm_s16le")),
    ("gif", "anim.gif", ["--fps", "15"], dict(codec="gif", rate=F(15), audio=None, gif=True)),
    ("wav", "audio.wav", [], dict(codec=None, audio="pcm_s16le")),
    ("wav", "audio24.wav", ["--audio-codec", "pcm_s24le"], dict(codec=None, audio="pcm_s24le")),
    ("flac", "audio.flac", [], dict(codec=None, audio="flac")),
    ("mp4-h264", "h264_2398_360p.mp4", ["--fps", "24000/1001", "--size", "320x180"],
     dict(codec="h264", pix="yuv420p", audio="aac", rate=F(24000, 1001), size=(320, 180), tb="1/24000")),
    ("mp4-h264", "h264_44k_mono.mp4", ["--audio-rate", "44100", "--audio-channels", "1"],
     dict(codec="h264", pix="yuv420p", audio="aac", hz=44100, ch=1)),
    ("mp4-h264", "h264_hw.mp4", ["--hw", "auto"], dict(codec="h264", pix="yuv420p", audio="aac", hw=True)),
    ("mp4-hevc", "hevc_hw.mp4", ["--hw", "auto"], dict(codec="hevc", pix="yuv420p", audio="aac", hw=True)),
    ("mp4-av1", "av1_hw.mp4", ["--hw", "auto"], dict(codec="av1", pix="yuv420p", audio="aac", hw=True)),
    ("webm-vp9", "vp9_hw.webm", ["--hw", "auto"], dict(codec="vp9", pix="yuv420p", audio="opus", hw=True)),
]
# The clips' colours as MeerCut reads its sources (untagged HD = BT.709, limited range: COLOR_MANAGEMENT.md); filled
# by source_colours(). FFmpeg's CLI reads untagged video as BT.601, so the test media's 0xC00000 made by FFmpeg
# shows as (208, 18, 0) here. A decoded export frame must show these within RGB_TOLERANCE levels.
CLIP_RGB = {}
RGB_TOLERANCE = 8


def source_colours(media: Path):
    for _, file, _, src_in, _, col, _ in CLIPS:
        raw = subprocess.run(["ffmpeg", "-v", "error", "-ss", str(float(src_in)), "-i", str(media / file), "-vf",
                              "setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt709,"
                              "crop=iw*0.2:ih*0.3:iw*0.4:ih*0.35", "-frames:v", "1", "-pix_fmt", "rgb24", "-f",
                              "rawvideo", "-"], capture_output=True).stdout
        CLIP_RGB[col] = tuple(np.frombuffer(raw, np.uint8).reshape(-1, 3).mean(axis=0))


def fstr(x):
    return str(x.numerator) if x.denominator == 1 else f"{x.numerator}/{x.denominator}"


def write_project(media: Path, path: Path):
    sources, vclips, aclips = [], [], []
    for i, (key, fname, start, src_in, dur, _, _) in enumerate(CLIPS):
        sources.append({"id": f"src-{key}", "path": str((media / fname).resolve()).replace("\\", "/"),
                        "frameRate": "30", "duration": "10", "hasVideo": True, "hasAudio": True})
        c = {"id": f"clip-v{i}", "source": f"src-{key}", "start": fstr(start), "in": fstr(src_in), "duration": fstr(dur),
             "link": f"l{i}"}
        vclips.append(c)
        aclips.append(dict(c, id=f"clip-a{i}"))
    proj = {"format": "fa-project", "version": 6, "name": "m6 export", "nextId": 100, "sources": sources,
            "sequences": [{"id": "s", "name": "m6", "width": SIZE[0], "height": SIZE[1], "frameRate": "30",
                           "sampleRate": 48000, "tracks": [{"id": "v1", "kind": "video", "clips": vclips},
                                                           {"id": "a1", "kind": "audio", "clips": aclips}]}]}
    path.write_text(json.dumps(proj, indent=2), encoding="utf-8")


def ffprobe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-show_entries",
                          "stream=index,codec_type,codec_name,profile,codec_tag_string,pix_fmt,width,height,color_range,"
                          "color_space,color_transfer,color_primaries,chroma_location,field_order,r_frame_rate,"
                          "avg_frame_rate,time_base,start_time,duration,nb_read_frames,sample_rate,channels,"
                          "bits_per_raw_sample:stream_tags:format=format_name,duration:format_tags",
                          "-of", "json", str(path)], capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError(out.stderr)
    return json.loads(out.stdout)


def first_frame_tags(path):
    """The colour tags of the first decoded frame: what a decoder takes from the bitstream itself (VUI, sequence or
    frame headers), which can differ from the container's tags that ffprobe reports per stream."""
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-read_intervals", "%+#1", "-show_entries",
                          "frame=color_range,color_space,color_primaries,color_transfer", "-of", "json", str(path)],
                         capture_output=True, text=True)
    frames = json.loads(out.stdout or "{}").get("frames", [])
    return frames[0] if frames else {}


def pcm(path, rate=48000):
    """The left channel as float samples (ffmpeg's -ac 1 downmix would add L and R at -3 dB each)."""
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-map", "0:a:0", "-af", "pan=mono|c0=c0", "-ar", str(rate),
                          "-f", "f32le", "-"], capture_output=True).stdout
    return np.frombuffer(raw, np.float32)


def amp(x, t0, f, win=0.02, rate=48000):
    i0, n = int(round(t0 * rate)), int(round(win * rate))
    seg = x[i0:i0 + n].astype(np.float64)
    if len(seg) < n:
        return 0.0
    ph = 2 * np.pi * f * np.arange(n) / rate
    return float(2 * np.hypot(seg @ np.cos(ph), seg @ np.sin(ph)) / n)


def colour(reg):
    r, g, b = reg
    if r > 120 and g < 70 and b < 70:
        return "red"
    if b > 120 and r < 70 and g < 70:
        return "blue"
    return "?"


def check(path, fmt, exp, stderr, failures, notes):
    info = ffprobe(path)
    vs = [s for s in info["streams"] if s["codec_type"] == "video"]
    aus = [s for s in info["streams"] if s["codec_type"] == "audio"]
    ftags = info.get("format", {}).get("tags", {})
    rate = exp.get("rate", RATE)
    size = exp.get("size", SIZE)
    frames_expected = math.ceil(END * rate)
    record = {"format": fmt, "file": path.name}

    def want(name, got, value):
        if got != value:
            failures.append(f"{path.name}: {name} {got!r}, expected {value!r}")

    if exp.get("codec"):
        if len(vs) != 1:
            failures.append(f"{path.name}: {len(vs)} video streams")
            return record
        v = vs[0]
        record["video"] = {k: v.get(k) for k in ("codec_name", "profile", "codec_tag_string", "pix_fmt", "width",
                                                 "height", "color_range", "color_space", "color_transfer",
                                                 "color_primaries", "chroma_location", "field_order", "r_frame_rate",
                                                 "avg_frame_rate", "time_base", "start_time", "duration",
                                                 "nb_read_frames")}
        record["video"]["encoder_tag"] = v.get("tags", {}).get("encoder")
        want("codec", v["codec_name"], exp["codec"])
        if "profile" in exp:
            want("profile", v.get("profile"), exp["profile"])
        if "pix" in exp:
            want("pix_fmt", v.get("pix_fmt"), exp["pix"])
        if "tag" in exp:
            want("codec tag", v.get("codec_tag_string"), exp["tag"])
        want("size", (v["width"], v["height"]), size)
        want("r_frame_rate", F(v["r_frame_rate"]), rate)
        want("frames", int(v.get("nb_read_frames", -1)), frames_expected)
        if "tb" in exp:
            want("time_base", v.get("time_base"), exp["tb"])
        if not exp.get("gif"):
            want("color_range", v.get("color_range"), "tv")
            for k in ("color_space", "color_transfer", "color_primaries"):
                want(k, v.get(k), "bt709")
            if "chroma" in exp:
                want("chroma_location", v.get("chroma_location"), exp["chroma"])
            if v.get("field_order") not in (None, "progressive"):
                failures.append(f"{path.name}: field_order {v.get('field_order')}")
            if "smpte2084" in json.dumps(v) or "arib-std-b67" in json.dumps(v) or "bt2020" in json.dumps(v):
                failures.append(f"{path.name}: HDR / BT.2020 tags must never be written")
            ft = first_frame_tags(path)
            record["frameTags"] = ft
            want("first frame color_range", ft.get("color_range"), "tv")
            for k in ("color_space", "color_transfer", "color_primaries"):
                want("first frame " + k, ft.get(k), "bt709")
        dur = float(v.get("duration") or info["format"]["duration"])
        if abs(dur - float(frames_expected / rate)) > 1.01 / float(rate):
            failures.append(f"{path.name}: video duration {dur:.4f} s, expected {float(frames_expected / rate):.4f} s")
        # every frame: strip index and clip colour
        w, h = v["width"], v["height"]
        bad = 0
        worst_rgb = 0.0   # largest error of a clip's colour, decoded the way a player would (by the file's own tags)
        for n, img in enumerate(framecode.frames(str(path), w, h)):
            t = F(n) / rate
            key, _, start, src_in, _, col, _ = CLIPS[0] if t < CLIPS[1][2] else CLIPS[1]
            sf = math.floor((src_in + (t - start)) * 30)
            idx, conf = framecode.decode_strip(img)
            reg = img[int(h * 0.35):int(h * 0.65), int(w * 0.4):int(w * 0.6)].reshape(-1, 3).mean(axis=0)
            worst_rgb = max(worst_rgb, float(np.abs(reg - np.array(CLIP_RGB[col])).max()))
            ok_idx = idx == sf or (exp.get("gif") and idx == -1)
            if not ok_idx or colour(reg) != col:
                bad += 1
                if bad <= 3:
                    failures.append(f"{path.name}: frame {n} shows index {idx} ({colour(reg)}), expected {sf} ({col})")
        record["framesChecked"] = frames_expected
        record["worstClipColourError"] = round(worst_rgb, 2)
        if bad:
            failures.append(f"{path.name}: {bad} wrong frame(s)")
        if worst_rgb > RGB_TOLERANCE:
            failures.append(f"{path.name}: a clip's colour is off by {worst_rgb:.1f} levels (tolerance {RGB_TOLERANCE})")
    elif vs:
        failures.append(f"{path.name}: unexpected video stream")

    if exp.get("audio"):
        if len(aus) != 1:
            failures.append(f"{path.name}: {len(aus)} audio streams")
            return record
        a = aus[0]
        record["audio"] = {k: a.get(k) for k in ("codec_name", "sample_rate", "channels", "start_time", "duration",
                                                 "bits_per_raw_sample")}
        want("audio codec", a["codec_name"], exp["audio"])
        want("sample rate", int(a["sample_rate"]), exp.get("hz", 48000))
        want("channels", int(a["channels"]), exp.get("ch", 2))
        x = pcm(path)
        lossless = exp["audio"] in ("pcm_s16le", "pcm_s24le", "flac")
        tol = 0.002 if lossless else 0.01
        for (_, _, start, _, dur, _, f) in CLIPS:
            mid = float(start + dur / 2)
            got = amp(x, mid, f)
            if abs(got - 0.125) > tol:
                failures.append(f"{path.name}: {f} Hz level {got:.4f} at {mid:.2f} s, expected 0.125")
        cut = float(CLIPS[1][2])
        cross = None
        for t in np.arange(cut - 0.02, cut + 0.02, 0.0005):
            p, q = amp(x, t, 1000, 0.002), amp(x, t, 1500, 0.002)
            if q / (p + q + 1e-9) > 0.5:
                cross = t + 0.001
                break
        record["audioSwitch"] = cross
        if cross is None or abs(cross - cut) > 0.003:
            failures.append(f"{path.name}: audio switches at {cross} s, the cut is at {cut} s (A/V sync)")
        vdur = float(END) if not vs else frames_expected / float(rate)
        adur = len(x) / 48000
        record["audioSeconds"] = adur
        if abs(adur - vdur) > 0.025:
            failures.append(f"{path.name}: audio lasts {adur:.4f} s, video {vdur:.4f} s")
    elif aus:
        failures.append(f"{path.name}: unexpected audio stream")

    if fmt in ("mp4-h264", "mp4-hevc", "mp4-av1"):
        want("major_brand", ftags.get("major_brand"), "mp42")
    if fmt not in ("gif", "wav", "flac"):
        want("creation_time", ftags.get("creation_time"), CREATION)
    record["formatTags"] = ftags
    if exp.get("hw"):
        line = next((l for l in stderr.splitlines() if l.startswith("wrote ")), "")
        enc = next((l for l in stderr.splitlines() if l.startswith("encoder: ")), "")
        record["hardware"] = {"summary": line, "note": enc}
        notes.append(f"{path.name}: {line.split(';')[-1].strip() if line else '?'} {enc}")
    return record


def compare_filmora(ours, filmora_file, failures):
    """Tags of our H.264 export vs Filmora's EXP-001 export (re-probed)."""
    if not filmora_file or not Path(filmora_file).exists():
        return {"note": "Filmora's EXP-001 file is not available on this machine; comparison skipped"}
    f = ffprobe(Path(filmora_file))
    fv = next(s for s in f["streams"] if s["codec_type"] == "video")
    ov = ours["video"]
    rows = {}
    for k in ("codec_name", "profile", "pix_fmt", "color_range", "color_space", "color_transfer", "color_primaries",
              "chroma_location", "field_order", "time_base"):
        rows[k] = {"filmora": fv.get(k), "meercut": ov.get(k), "same": fv.get(k) == ov.get(k)}
        if fv.get(k) != ov.get(k):
            failures.append(f"EXP-007: {k} differs from Filmora ({ov.get(k)} vs {fv.get(k)})")
    for k in ("major_brand",):
        rows[k] = {"filmora": f["format"]["tags"].get(k), "meercut": ours["formatTags"].get(k),
                   "same": f["format"]["tags"].get(k) == ours["formatTags"].get(k)}
        if not rows[k]["same"]:
            failures.append(f"EXP-007: {k} differs from Filmora")
    rows["compatible_brands"] = {"filmora": f["format"]["tags"].get("compatible_brands"),
                                 "meercut": ours["formatTags"].get("compatible_brands")}
    fa = next((s for s in f["streams"] if s["codec_type"] == "audio"), {})
    rows["audio"] = {"filmora": f"{fa.get('codec_name')} {fa.get('sample_rate')} Hz",
                     "meercut": f"{ours['audio']['codec_name']} {ours['audio']['sample_rate']} Hz",
                     "note": "MeerCut keeps the sequence rate (48 kHz); Filmora resampled to 44.1 kHz"}
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("renderer")
    ap.add_argument("workdir")
    ap.add_argument("--testmedia", default=str(REPO / "testmedia"))
    ap.add_argument("--filmora", default=str(REPO / "experiments" / "EXP-001_dissolve" / "b_handles.mp4"))
    ap.add_argument("--negative-control", action="store_true")
    a = ap.parse_args()
    work = Path(a.workdir) / "m6_export"
    work.mkdir(parents=True, exist_ok=True)
    failures, notes = [], []
    source_colours(Path(a.testmedia))
    if a.negative_control:
        # Every bad file must be rejected (exit 1, the test is WILL_FAIL); if the checker accepts one, exit 0.
        neg = work / "negative"
        neg.mkdir(exist_ok=True)
        # 1. an untagged, full-range 4:4:4 file must fail the H.264 expectations
        bad = neg / "untagged.mp4"
        r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", f"color=red:s={SIZE[0]}x{SIZE[1]}:r=30:d=3",
                            "-pix_fmt", "yuvj444p", "-c:v", "libx264", str(bad)], capture_output=True, text=True)
        if r.returncode != 0:
            print("SETUP FAILED", r.stderr)
            return 2
        check(bad, "mp4-h264", CASES[0][3], "", failures, notes)
        print("untagged.mp4:\n  " + ("\n  ".join(failures) or "no failures (the checker accepted an untagged file!)"))
        # 2. a correct VP9 export whose frame headers alone say "unknown" colour space (the container still says
        #    BT.709): what Quick Sync's VP9 encoder wrote before the fix; it must fail on the frame tags
        project = neg / "project.json"
        write_project(Path(a.testmedia), project)
        good, relabelled = neg / "vp9.webm", neg / "vp9_unknown_in_bitstream.webm"
        r1 = subprocess.run([a.renderer, str(project), str(good), "--format", "webm-vp9", "--preset", "veryfast",
                             "--creation-time", CREATION], capture_output=True, text=True)
        r2 = subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(good), "-c", "copy", "-bsf:v",
                             "vp9_metadata=color_space=unknown", str(relabelled)], capture_output=True, text=True)
        if r1.returncode != 0 or r2.returncode != 0:
            print("SETUP FAILED", r1.stderr, r2.stderr)
            return 2
        vp9_failures = []
        check(relabelled, "webm-vp9", next(c[3] for c in CASES if c[1] == "vp9.webm"), "", vp9_failures, notes)
        print("vp9_unknown_in_bitstream.webm:\n  " +
              ("\n  ".join(vp9_failures) or "no failures (the checker accepted a wrong bitstream colour space!)"))
        return 1 if failures and any("first frame color_space" in f for f in vp9_failures) else 0
    project = work / "project.json"
    write_project(Path(a.testmedia), project)
    results = {"cases": [], "notes": notes}
    for fmt, name, extra, exp in CASES:
        out = work / name
        if out.exists():
            out.unlink()
        cmd = [a.renderer, str(project), str(out), "--format", fmt, "--preset", "veryfast", "--creation-time", CREATION] + extra
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 and exp.get("optional") and ("cannot make" in r.stderr or "not offered" in r.stderr):
            notes.append(f"{name}: not available here: {r.stderr.strip().splitlines()[-1]}")
            results["cases"].append({"file": name, "format": fmt, "skipped": r.stderr.strip().splitlines()[-1]})
            continue
        if r.returncode != 0 or not out.exists():
            print(f"RENDER FAILED {name} (exit {r.returncode})\n{r.stderr}")
            failures.append(f"{name}: render failed: {r.stderr.strip().splitlines()[-1] if r.stderr.strip() else r.returncode}")
            continue
        try:
            rec = check(out, fmt, exp, r.stderr, failures, notes)
        except Exception as e:  # noqa: BLE001
            failures.append(f"{name}: check failed: {e}")
            continue
        rec["args"] = extra
        rec["bytes"] = out.stat().st_size
        results["cases"].append(rec)
    # lossless audio: the WAV (16-bit) and FLAC (16-bit) carry the same samples
    wav, flac = pcm(work / "audio.wav"), pcm(work / "audio.flac")
    if len(wav) != len(flac) or float(np.abs(wav - flac).max(initial=0)) > 1e-7:
        failures.append("WAV and FLAC exports differ")
    h264 = next((c for c in results["cases"] if c["file"] == "h264.mp4"), None)
    if h264:
        results["filmoraComparison"] = compare_filmora(h264, a.filmora, failures)
    results["failures"] = failures
    (work / "m6_export_results.json").write_text(json.dumps(results, indent=2, default=str), encoding="utf-8")
    for n in notes:
        print("hardware:", n)
    if failures:
        print("FAIL\n  " + "\n  ".join(failures))
        return 1
    print(f"PASS: {len(results['cases'])} exports checked; evidence in {work / 'm6_export_results.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
