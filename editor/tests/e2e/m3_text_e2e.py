"""End-to-end verification of M3 titles, title animation and subtitle burn-in through the headless renderer.

Writes a version-3 project with one title clip and renders it with  fa-render --subtitles <file.srt>  (the
real SRT parser adds the captions on a subtitle track before rendering) with --backend cpu and --backend
gpu. Every output frame is decoded and checked against values computed here from the project and the
SRT (not from the renderer):

  title     "HIHI" (Arial, size 100, white) centred at y = 432 px, Type Writer In of 0.8 s:
            visible letters = round(4 t / 0.8) (EXP-003 formula) -> counted as separate ink runs along the
            text's row band; letters appear in place (the first letter's left edge never moves)
  caption 1 0.5 - 2.0 s "Burned in caption", bottom-anchored on the subtitle line y = 0.92 H = 993.6 px,
            centred horizontally; exactly from frame 15 to frame 59
  caption 2 2.5 - 3.5 s "{\an8}Top line": top-anchored at y = 0.08 H = 86.4 px
  caption 3 3.5 - 4.0 s "<font color="#FFFF00">Yellow</font>": yellow ink on the subtitle line
Both outputs must also agree with each other (CPU/GPU parity after encoding).

usage: python m3_text_e2e.py <fa-render.exe> <work dir> [--perturb]
--perturb shifts caption 1 by one frame in the SRT given to the renderer: the check must then fail
(negative control proving the frame-exact timing check detects an off-by-one).
exit code 0 = pass, 1 = verification failure, 2 = setup/render failure
"""
import argparse
import json
import subprocess
import sys
from fractions import Fraction as F
from pathlib import Path

import numpy as np

W, H, FPS, FRAMES = 1920, 1080, 30, 120
TITLE_Y = 432                      # 0.5 H + positionY (-0.1) x H
SUB_LINE = 0.92 * H                # caption anchor line (bottom-anchored)
TOP_LINE = 0.08 * H                # {\an8} captions: top anchor


def write_inputs(work: Path, perturb: bool):
    title = {"id": "t1", "start": "0", "in": "0", "duration": "4",
             "title": {"text": "HIHI", "style": {"font": "Arial", "size": 100},
                       "in": {"preset": "typewriter", "duration": "4/5"}},
             "transform": {"positionY": {"value": -0.1, "keys": []}}}
    proj = {"format": "fa-project", "version": 3, "name": "m3_text", "nextId": 100, "sources": [],
            "sequences": [{"id": "seq-1", "name": "m3", "width": W, "height": H, "frameRate": "30",
                           "sampleRate": 48000,
                           "tracks": [{"id": "v1", "kind": "video", "clips": [title]}]}]}
    project = work / "m3_text.json"
    project.write_text(json.dumps(proj, indent=2), encoding="utf-8")
    first = "00:00:00,533" if perturb else "00:00:00,500"   # +1 frame (0.533 s -> frame 16)
    srt = (f"1\r\n{first} --> 00:00:02,000\r\nBurned in caption\r\n\r\n"
           "2\r\n00:00:02,500 --> 00:00:03,500\r\n{\\an8}Top line\r\n\r\n"
           "3\r\n00:00:03,500 --> 00:00:04,000\r\n<font color=\"#FFFF00\">Yellow</font>\r\n\r\n")
    subs = work / "m3_text.srt"
    subs.write_bytes(srt.encode("utf-8"))
    return project, subs


def frames(path):
    cmd = ["ffmpeg", "-v", "error", "-i", str(path), "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    size = W * H * 3
    out = []
    while True:
        buf = proc.stdout.read(size)
        if len(buf) < size:
            break
        out.append(np.frombuffer(buf, np.uint8).reshape(H, W, 3).astype(np.int16))
    proc.wait()
    return out


def bright(px):
    return px.max(axis=-1) > 128


def yellow(px):
    return (px[..., 0] > 200) & (px[..., 1] > 200) & (px[..., 2] < 90)


def column_runs(mask):
    """Number of separate runs of ink columns, and the first ink column."""
    cols = mask.any(axis=0)
    idx = np.flatnonzero(cols)
    if idx.size == 0:
        return 0, None
    return int(1 + np.count_nonzero(np.diff(idx) > 2)), int(idx[0])


def ink_rows(mask):
    rows = np.flatnonzero(mask.any(axis=1))
    return (int(rows[0]), int(rows[-1])) if rows.size else None


def ink_cols(mask):
    cols = np.flatnonzero(mask.any(axis=0))
    return (int(cols[0]), int(cols[-1])) if cols.size else None


def verify(fr, label):
    errors = []
    first_left = None
    for n, img in enumerate(fr):
        t = F(n, FPS)
        # title: Type Writer letters (skip the exact half-way frames where rounding is a tie)
        band = bright(img[TITLE_Y - 120:TITLE_Y + 120])
        letters, left = column_runs(band)
        num = 4 * t / F(4, 5)
        if num.denominator != 2:
            expect = min(4, int(num + F(1, 2)))
            if letters != expect:
                errors.append(f"{label} frame {n}: {letters} letters typed, expected {expect}")
        if left is not None:
            first_left = left if first_left is None else first_left
            if abs(left - first_left) > 1:
                errors.append(f"{label} frame {n}: typed text starts at x={left}, earlier {first_left} (must not move)")
        # captions
        bottom = bright(img[int(SUB_LINE) - 120:int(SUB_LINE) + 30])
        top = bright(img[int(TOP_LINE) - 30:int(TOP_LINE) + 120])
        c1 = F(1, 2) <= t < 2
        c2 = F(5, 2) <= t < F(7, 2)
        c3 = F(7, 2) <= t < 4
        rows = ink_rows(bottom)
        if c1 or c3:
            if rows is None:
                errors.append(f"{label} frame {n}: no caption on the subtitle line")
            else:
                y1 = int(SUB_LINE) - 120 + rows[1]
                if not (SUB_LINE - 40 <= y1 <= SUB_LINE + 2):
                    errors.append(f"{label} frame {n}: caption ink ends at y={y1}, expected just above {SUB_LINE:.1f}")
                cols = ink_cols(bottom)
                if abs((cols[0] + cols[1]) / 2 - W / 2) > 8:
                    errors.append(f"{label} frame {n}: caption not centred ({cols})")
        elif rows is not None:
            errors.append(f"{label} frame {n}: unexpected ink on the subtitle line")
        if c3 and np.count_nonzero(yellow(img[int(SUB_LINE) - 120:int(SUB_LINE) + 30])) < 300:
            errors.append(f"{label} frame {n}: the <font color> caption is not yellow")
        trows = ink_rows(top)
        if c2:
            if trows is None:
                errors.append(f"{label} frame {n}: no {{\\an8}} caption at the top")
            else:
                y0 = int(TOP_LINE) - 30 + trows[0]
                if not (TOP_LINE - 2 <= y0 <= TOP_LINE + 40):
                    errors.append(f"{label} frame {n}: top caption ink starts at y={y0}, expected just below {TOP_LINE:.1f}")
        elif trows is not None:
            errors.append(f"{label} frame {n}: unexpected ink at the top")
    return errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("renderer")
    ap.add_argument("workdir")
    ap.add_argument("--testmedia", default="")   # accepted for symmetry with the other e2e scripts
    ap.add_argument("--perturb", action="store_true")
    a = ap.parse_args()
    work = Path(a.workdir) / ("m3_text_perturbed" if a.perturb else "m3_text")
    work.mkdir(parents=True, exist_ok=True)
    project, subs = write_inputs(work, a.perturb)
    # the checks use the unperturbed timing; --perturb only changes what the renderer is given
    outputs = {}
    for backend in ("cpu", "gpu"):
        out = work / f"m3_text_{backend}.mp4"
        r = subprocess.run([a.renderer, str(project), str(out), "--subtitles", str(subs), "--backend", backend,
                            "--crf", "10", "--no-audio"], capture_output=True, text=True)
        lines = r.stderr.strip().splitlines()
        for line in lines:
            if line.startswith("subtitles:"):
                print(line)
        print(lines[-1] if lines else "(no output)")
        if r.returncode != 0:
            print(r.stderr)
            return 2
        if any("not imported exactly" in line for line in lines):
            print("FAIL: the test SRT uses only supported features but warnings were printed")
            return 1
        outputs[backend] = frames(out)
        if len(outputs[backend]) != FRAMES:
            print(f"{backend}: expected {FRAMES} frames, got {len(outputs[backend])}")
            return 1
    errors = verify(outputs["cpu"], "cpu") + verify(outputs["gpu"], "gpu")
    diffs = [float(np.abs(c - g).mean()) for c, g in zip(outputs["cpu"], outputs["gpu"])]
    print(f"CPU vs GPU output: mean abs difference per frame max {max(diffs):.3f}, average {sum(diffs) / len(diffs):.3f}")
    if max(diffs) > 1.0:
        errors.append(f"CPU and GPU exports differ: {max(diffs):.3f}")
    for e in errors[:30]:
        print("FAIL", e)
    print(f"{'PASS' if not errors else 'FAIL'}: {FRAMES} frames x 2 backends checked, {len(errors)} problems")
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
