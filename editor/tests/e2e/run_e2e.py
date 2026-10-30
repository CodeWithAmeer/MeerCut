"""End-to-end render verification for the M0 headless renderer.

For one named case it
  1. writes an fa-project JSON that references real frame-coded test media,
  2. runs  fa-render <project> <out.mp4> --map <map.csv>,
  3. checks the container with ffprobe (codec, size, exact frame rate, frame count, duration),
  4. decodes EVERY output frame (tools/framecode.py) and compares, frame by frame,
       - the embedded source frame index and the clip colour with expectations computed here
         independently from the project using exact fractions, and
       - for dissolves, the measured mix ratio with the linear ramp,
  5. checks that the renderer's own frame map agrees with the independent expectation.

usage: python run_e2e.py <fa-render.exe> <case> <work dir> [--testmedia DIR]
exit code 0 = pass, 1 = verification failure, 2 = setup/render failure
"""
import argparse
import csv
import json
import math
import subprocess
import sys
from fractions import Fraction as F
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[3]                      # the repository root
sys.path.insert(0, str(REPO / "tools"))
import framecode  # noqa: E402  (Stage 1 frame-index reader)
import numpy as np  # noqa: E402

# Source media: rate, frame count, colour class used to identify the clip on screen.
MEDIA = {
    "A": ("A_red_1080p30_10s.mp4", F(30), 300, "red"),
    "B": ("B_blue_1080p30_10s.mp4", F(30), 300, "blue"),
    "N": ("N_green_720p2997_6s.mp4", F(30000, 1001), 180, "green"),
    "O": ("O_magenta_720p2997_6s.mp4", F(30000, 1001), 180, "magenta"),
}
TONE = {"A": 1000, "B": 1500, "N": 1000, "O": 1500}   # Hz, amplitude 0.125 in every clip


def fstr(x: F) -> str:
    return str(x.numerator) if x.denominator == 1 else f"{x.numerator}/{x.denominator}"


def frames_at(n, rate):
    return F(n) / rate


# ---- cases ------------------------------------------------------------------------------------
# Each clip: (source key, timeline start s, source in s, duration s); all exact fractions.

def case_def(name):
    r30, ntsc, r24 = F(30), F(30000, 1001), F(24)
    if name == "cut_30fps":
        # Two clips, hard cut at 2 s; both trimmed (A from 2 s, B from 5 s).
        return dict(rate=r30, size=(1920, 1080), clips=[
            ("A", F(0), F(2), F(2)),
            ("B", F(2), F(5), F(2))])
    if name == "cut_2997fps":
        # Native NTSC media in an NTSC sequence; cut at frame 90, odd in-points.
        return dict(rate=ntsc, size=(1280, 720), clips=[
            ("N", F(0), frames_at(30, ntsc), frames_at(90, ntsc)),
            ("O", frames_at(90, ntsc), frames_at(11, ntsc), frames_at(97, ntsc))])
    if name == "mixed_2997_from_30":
        # 30 fps media in a 29.97 sequence: frames are sampled by time (floor), so one source frame
        # is skipped every 1000 output frames. B's in-point sits 0.95 frame into source frame 135, so
        # the skip happens at output frame 50 of B (135.95 + 1.001 * 50 = 186.0).
        return dict(rate=ntsc, size=(1920, 1080), clips=[
            ("A", F(0), F(0), frames_at(75, ntsc)),
            ("B", frames_at(75, ntsc), F(2719, 600), frames_at(75, ntsc))])
    if name == "mixed_24_from_30":
        return dict(rate=r24, size=(1920, 1080), clips=[
            ("B", F(0), F(1), F(2)),
            ("A", F(2), F(7, 3) - F(1, 3), F(3, 2))])
    if name == "dissolve_30fps":
        # 1 s centred dissolve on the cut at 2 s: A tail handle and B head handle exist.
        return dict(rate=r30, size=(1920, 1080), clips=[
            ("A", F(0), F(3), F(2)),
            ("B", F(2), F(4), F(2))],
            transition=dict(type="fa.transition.dissolve", duration=F(1), alignment="centered"))
    raise SystemExit(f"unknown case {name}")


CASES = ["cut_30fps", "cut_2997fps", "mixed_2997_from_30", "mixed_24_from_30", "dissolve_30fps"]


def write_project(case, media_dir: Path, path: Path):
    d = case_def(case)
    sources, clips = {}, []
    for i, (key, start, src_in, dur) in enumerate(d["clips"]):
        fname, rate, nframes, _ = MEDIA[key]
        sources[key] = {"id": f"src-{key}", "path": str((media_dir / fname).resolve()).replace("\\", "/"),
                        "frameRate": fstr(rate), "duration": fstr(F(nframes) / rate), "hasVideo": True,
                        "hasAudio": True}
        clip = {"id": f"clip-{i}", "source": f"src-{key}", "start": fstr(start), "in": fstr(src_in),
                "duration": fstr(dur)}
        if i == 0 and "transition" in d:
            t = d["transition"]
            clip["outTransition"] = {"type": t["type"], "duration": fstr(t["duration"]), "alignment": t["alignment"]}
        clips.append(clip)
    # linked audio track mirroring the video clips (each clip plays its own file's audio)
    audio = []
    for i, c in enumerate(clips):
        clip_a = {k: v for k, v in c.items() if k != "outTransition"}
        clip_a["id"] = f"aclip-{i}"
        clip_a["link"] = f"link-{i}"
        c["link"] = f"link-{i}"
        audio.append(clip_a)
    proj = {"format": "fa-project", "version": 1, "name": case, "nextId": 100,
            "sources": list(sources.values()),
            "sequences": [{"id": "seq-1", "name": case, "width": d["size"][0], "height": d["size"][1],
                           "frameRate": fstr(d["rate"]), "sampleRate": 48000,
                           "tracks": [{"id": "v1", "kind": "video", "clips": clips},
                                      {"id": "a1", "kind": "audio", "clips": audio}]}]}
    path.write_text(json.dumps(proj, indent=2), encoding="utf-8")
    return d


# ---- independent expectation ------------------------------------------------------------------

def src_frame(key, t: F):
    _, rate, nframes, _ = MEDIA[key]
    return min(max(math.floor(t * rate), 0), nframes - 1)   # floor = frame visible at t; hold at edges


def expected_frames(d):
    rate = d["rate"]
    clips = d["clips"]
    end = max(start + dur for _, start, _, dur in clips)
    total = math.ceil(end * rate)
    trans = d.get("transition")
    out = []
    for n in range(total):
        t = F(n) / rate
        exp = None
        if trans:
            (ka, sa, ia, da), (kb, sb, ib, db) = clips[0], clips[1]
            cut = sa + da
            dur = trans["duration"]
            start = {"centered": cut - dur / 2, "endsAtCut": cut - dur, "startsAtCut": cut}[trans["alignment"]]
            if start <= t < start + dur:
                exp = {"mix": (t - start) / dur, "a": (ka, src_frame(ka, ia + (t - sa))),
                       "b": (kb, src_frame(kb, ib + (t - sb)))}
        if exp is None:
            for key, start, src_in, dur in clips:
                if start <= t < start + dur:
                    exp = {"mix": None, "a": (key, src_frame(key, src_in + (t - start)))}
                    break
        out.append(exp)
    return out


def colour_class(r, g, b):
    if r > 120 and g < 60 and b < 60:
        return "red"
    if b > 120 and r < 60 and g < 60:
        return "blue"
    if g > 100 and r < 60 and b < 60:
        return "green"
    if r > 100 and b > 100 and g < 60:
        return "magenta"
    return "?"


# ---- main ---------------------------------------------------------------------------------------

def ffprobe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames",
                          "-show_entries", "stream=codec_name,width,height,r_frame_rate,avg_frame_rate,nb_frames,"
                          "nb_read_frames,duration,pix_fmt,color_space,color_range",
                          "-show_entries", "format=duration,format_name", "-of", "json", str(path)],
                         capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("renderer")
    ap.add_argument("case", choices=CASES)
    ap.add_argument("workdir")
    ap.add_argument("--testmedia", default=str(REPO / "testmedia"))
    ap.add_argument("--perturb", action="store_true",
                    help="negative control: shift the 2nd clip's in-point by one source frame in the rendered "
                         "project while keeping the original expectations -> verification must FAIL")
    a = ap.parse_args()

    work = Path(a.workdir) / (a.case + ("_perturbed" if a.perturb else ""))
    work.mkdir(parents=True, exist_ok=True)
    media_dir = Path(a.testmedia)
    project, out, fmap = work / "project.json", work / "out.mp4", work / "map.csv"
    for p in (out, fmap):
        if p.exists():
            p.unlink()
    d = write_project(a.case, media_dir, project)
    if a.perturb:
        proj = json.loads(project.read_text(encoding="utf-8"))
        clip = proj["sequences"][0]["tracks"][0]["clips"][1]
        key = d["clips"][1][0]
        clip["in"] = fstr(F(clip["in"]) + 1 / MEDIA[key][1])
        project.write_text(json.dumps(proj, indent=2), encoding="utf-8")

    r = subprocess.run([a.renderer, str(project), str(out), "--map", str(fmap), "--quiet"],
                       capture_output=True, text=True)
    if r.returncode != 0 or not out.exists():
        print(f"RENDER FAILED (exit {r.returncode})\n{r.stdout}\n{r.stderr}")
        return 2
    if r.stderr.strip():
        print("renderer messages:\n" + r.stderr.strip())

    failures = []
    exp = expected_frames(d)
    rate = d["rate"]

    # 3. container checks
    info = ffprobe(out)
    st = info["streams"][0]
    checks = {
        "codec": (st["codec_name"], "h264"),
        "width": (st["width"], d["size"][0]),
        "height": (st["height"], d["size"][1]),
        "r_frame_rate": (F(st["r_frame_rate"]), rate),
        "avg_frame_rate": (F(st["avg_frame_rate"]), rate),
        "nb_frames": (int(st["nb_frames"]), len(exp)),
        "decoded frames": (int(st["nb_read_frames"]), len(exp)),
        "pix_fmt": (st["pix_fmt"], "yuv420p"),
        "color_space": (st.get("color_space"), "bt709"),
    }
    for k, (got, want) in checks.items():
        if got != want:
            failures.append(f"container {k}: got {got}, expected {want}")
    dur = float(st["duration"])
    if abs(dur - float(len(exp) / rate)) > 0.5 / float(rate):
        failures.append(f"container duration {dur} s, expected {float(len(exp) / rate):.6f} s")

    # 4. every frame
    w, h, _ = framecode.probe(str(out))
    sx, sy = w / 1920.0, h / 1080.0
    measured = []
    for n, img in enumerate(framecode.frames(str(out), w, h)):
        idx, conf = framecode.decode_strip(img)
        reg = img[int(340 * sy):int(740 * sy), int(760 * sx):int(1160 * sx)].reshape(-1, 3).mean(axis=0)
        measured.append((idx, conf, reg))
    if len(measured) > len(exp):
        failures.append(f"{len(measured) - len(exp)} extra output frame(s)")
    # pure level of each clip's dominant channel, from its unmixed frames (for dissolve ratios)
    ch = {"red": 0, "green": 1, "blue": 2}
    pure = {}
    for (idx, conf, reg), e in zip(measured, exp):
        if e["mix"] is None and MEDIA[e["a"][0]][3] in ch:
            pure.setdefault(e["a"][0], []).append(reg[ch[MEDIA[e["a"][0]][3]]])
    pure = {k: sum(v) / len(v) for k, v in pure.items()}

    n_checked = 0
    mix_err = 0.0
    mix_pairs = []
    for n, ((idx, conf, reg), e) in enumerate(zip(measured, exp)):
        cls = colour_class(*reg)
        if e["mix"] is None:
            key, sf = e["a"]
            want_cls = MEDIA[key][3]
            if idx != sf or cls != want_cls:
                failures.append(f"frame {n} (t={float(F(n) / rate):.4f}s): got index {idx} ({cls}, conf {conf:.2f}), "
                                f"expected {key} frame {sf} ({want_cls})")
        else:
            # Dissolve of two solid colours: measure the share of B's dominant channel.
            ka, kb = e["a"][0], e["b"][0]
            ma = reg[ch[MEDIA[ka][3]]] / pure[ka]
            mb = reg[ch[MEDIA[kb][3]]] / pure[kb]
            got_mix = mb / (ma + mb) if ma + mb > 0.01 else float("nan")
            want = float(e["mix"])
            mix_err = max(mix_err, abs(got_mix - want))
            mix_pairs.append((want, got_mix))
            # 8-bit limited-range YUV 4:2:0 output: one Cb code step moves blue by ~1.86 levels, i.e.
            # ~0.009 of mix on a 200-level signal; Y and Cb rounding can add up -> 0.015 per frame.
            if not abs(got_mix - want) <= 0.015:
                failures.append(f"frame {n}: dissolve mix {got_mix:.4f}, expected {want:.4f}")
            if e["a"][1] == e["b"][1] and idx not in (-1, e["a"][1]):
                failures.append(f"frame {n}: strip {idx} but both sides show frame {e['a'][1]}")
        n_checked += 1
    if n_checked != len(exp):
        failures.append(f"decoded {n_checked} frames, expected {len(exp)}")
    mix_note = ""
    if mix_pairs:
        # The curve as a whole must be the linear ramp: small mean error, slope 1, intercept 0.
        wants = [p[0] for p in mix_pairs]
        gots = [p[1] for p in mix_pairs]
        mean_err = sum(abs(a - b) for a, b in mix_pairs) / len(mix_pairs)
        mw, mg = sum(wants) / len(wants), sum(gots) / len(gots)
        slope = sum((a - mw) * (b - mg) for a, b in mix_pairs) / sum((a - mw) ** 2 for a in wants)
        intercept = mg - slope * mw
        mix_note = f"; dissolve: {len(mix_pairs)} frames, mean error {mean_err:.4f}, fit slope {slope:.4f} intercept {intercept:+.4f}"
        if mean_err > 0.005 or abs(slope - 1) > 0.02 or abs(intercept) > 0.01:
            failures.append(f"dissolve curve is not the linear ramp (mean error {mean_err:.4f}, slope {slope:.4f}, "
                            f"intercept {intercept:+.4f})")

    # 4b. audio: AAC stream present, same length as the video, right tone per clip, switch at the cut
    audio_note = ""
    ainfo = json.loads(subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
         "stream=codec_name,sample_rate,channels,duration", "-of", "json", str(out)],
        capture_output=True, text=True, check=True).stdout).get("streams", [])
    if not ainfo:
        failures.append("no audio stream in the output")
    else:
        a0 = ainfo[0]
        if a0["codec_name"] != "aac" or int(a0["sample_rate"]) != 48000 or int(a0["channels"]) != 2:
            failures.append(f"audio stream {a0}, expected aac 48000 Hz stereo")
        video_len = float(len(exp) / rate)
        if abs(float(a0["duration"]) - video_len) > 0.03:
            failures.append(f"audio duration {a0['duration']} s, video {video_len:.4f} s")
        raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(out), "-vn", "-af", "pan=mono|c0=c0", "-ar", "48000",
                              "-f", "f32le", "-"], capture_output=True).stdout
        pcm = np.frombuffer(raw, np.float32)

        def amp(t0, f, win=0.010):
            i0, n = int(round(t0 * 48000)), int(round(win * 48000))
            seg = pcm[i0:i0 + n].astype(np.float64)
            if len(seg) < n:
                return 0.0
            ph = 2 * np.pi * f * np.arange(n) / 48000
            return float(2 * np.hypot(seg @ np.cos(ph), seg @ np.sin(ph)) / n)

        for key, start, src_in, dur in d["clips"]:
            mid = float(start + dur / 2)
            want = TONE[key]
            other = 1500 if want == 1000 else 1000
            a_want, a_other = amp(mid, want), amp(mid, other)
            if not (0.10 < a_want < 0.15 and a_other < 0.01):
                failures.append(f"audio at {mid:.3f} s: {want} Hz amp {a_want:.4f}, {other} Hz amp {a_other:.4f}")
        cut = float(d["clips"][1][1])   # second clip start
        ka, kb = d["clips"][0][0], d["clips"][1][0]
        if TONE[ka] != TONE[kb]:
            cross = None
            for t in np.arange(cut - 0.02, cut + 0.02, 0.0005):
                x, y = amp(t, TONE[ka], 0.002), amp(t, TONE[kb], 0.002)
                if y / (x + y + 1e-9) > 0.5:
                    cross = t + 0.001   # centre of the 2 ms window
                    break
            if cross is None or abs(cross - cut) > 0.003:
                failures.append(f"audio switches at {cross} s but the cut is at {cut:.4f} s (A/V sync)")
            else:
                audio_note = f"; audio switches at {cross:.4f} s (cut {cut:.4f} s)"


    # 5. renderer frame map vs independent expectation
    with fmap.open(newline="") as f:
        rows = list(csv.DictReader(f))
    by_frame = {}
    for row in rows:
        by_frame.setdefault(int(row["frame"]), {})[row["role"]] = row
    for n, e in enumerate(exp):
        got = by_frame.get(n, {})
        for role in ("a", "b"):
            if role not in e:
                continue
            key, sf = e[role]
            g = got.get(role)
            if not g or g["source"] != f"src-{key}" or int(g["src_frame"]) != sf:
                failures.append(f"frame map {n}/{role}: {g and (g['source'], g['src_frame'])}, expected {(key, sf)}")

    print(f"case {a.case}: {len(exp)} frames @ {fstr(rate)} fps, {d['size'][0]}x{d['size'][1]}; "
          f"checked {n_checked} frames" + (f"; max dissolve mix error {mix_err:.4f}" if mix_err else "") + mix_note
          + audio_note)
    if failures:
        print(f"FAIL: {len(failures)} problem(s)")
        for msg in failures[:40]:
            print("  " + msg)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
