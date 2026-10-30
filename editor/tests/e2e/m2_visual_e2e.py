"""End-to-end verification of the M2 visual features through the headless renderer.

Writes a version-2 project that uses keyframed position + scale, an ellipse mask with the Screen blend
mode, and a soft-edge-free Wipe transition, renders it with  fa-render --backend cpu  and
--backend gpu, then decodes every output frame and checks geometry against values computed here
independently from the project (not from the renderer):

  0.0 - 1.5 s  B (blue) on Video 2 at 50 % scale over A (red); X position keyframed -0.25 -> +0.25
               (linear) -> the blue run on row 600 must start at 960 + x*1920 - 480 and be 960 px long
  1.5 - 2.4 s  B full size with an ellipse mask (diameter 540 px, centre x = 960 + 0.3*1080) in Screen
               mode -> magenta run on row 540 from 1014 to 1554, red elsewhere
  2.5 - 3.5 s  Wipe (direction right, softness 0) from A to B on Video 1 -> blue up to x = p*1920
Both outputs must also agree with each other (CPU/GPU parity after encoding).

usage: python m2_visual_e2e.py <fa-render.exe> <work dir> [--testmedia DIR] [--perturb]
--perturb renders the mask 6 px (0.0056 layer heights) off its position: the check must then fail
(negative control proving the tolerances detect a real geometry error).
exit code 0 = pass, 1 = verification failure, 2 = setup/render failure
"""
import argparse
import json
import subprocess
import sys
from fractions import Fraction as F
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve()
REPO = HERE.parents[3]
W, H, FPS = 1920, 1080, 30


def anim(value, keys=None):
    j = {"value": value, "keys": []}
    for t, v in keys or []:
        j["keys"].append({"t": str(t) if t.denominator == 1 else f"{t.numerator}/{t.denominator}", "v": v,
                          "interp": "linear", "tangent": "linear"})
    return j


def write_project(media: Path, path: Path, perturb=False):
    def src(key, fname):
        return {"id": f"src-{key}", "path": str((media / fname).resolve()).replace("\\", "/"), "frameRate": "30",
                "duration": "10", "hasVideo": True, "hasAudio": False, "width": 1920, "height": 1080}
    v1 = [
        {"id": "a1", "source": "src-A", "start": "0", "in": "0", "duration": "3",
         "outTransition": {"type": "fa.transition.wipe", "duration": "1", "alignment": "centered", "direction": "right",
                           "softness": 0.0}},
        {"id": "b1", "source": "src-B", "start": "3", "in": "3", "duration": "2"},
    ]
    v2 = [
        {"id": "b-move", "source": "src-B", "start": "0", "in": "0", "duration": "3/2",
         "transform": {"scaleX": anim(0.5), "scaleY": anim(0.5),
                       "positionX": anim(0.0, [(F(0), -0.25), (F(3, 2), 0.25)])}},
        {"id": "b-mask", "source": "src-B", "start": "3/2", "in": "0", "duration": "9/10", "blend": "screen",
         "masks": [{"id": "m1", "shape": "ellipse", "centerX": anim(0.3 + (6 / 1080 if perturb else 0))}]},
    ]
    proj = {"format": "fa-project", "version": 2, "name": "m2_visual", "nextId": 100,
            "sources": [src("A", "A_red_1080p30_10s.mp4"), src("B", "B_blue_1080p30_10s.mp4")],
            "sequences": [{"id": "seq-1", "name": "m2", "width": W, "height": H, "frameRate": "30", "sampleRate": 48000,
                           "tracks": [{"id": "v1", "kind": "video", "clips": v1},
                                      {"id": "v2", "kind": "video", "clips": v2}]}]}
    path.write_text(json.dumps(proj, indent=2), encoding="utf-8")


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


def runs(mask_row):
    """(first, last) index of the longest True run."""
    idx = np.flatnonzero(mask_row)
    if idx.size == 0:
        return None
    splits = np.flatnonzero(np.diff(idx) > 1)
    starts = np.concatenate(([idx[0]], idx[splits + 1]))
    ends = np.concatenate((idx[splits], [idx[-1]]))
    k = np.argmax(ends - starts)
    return int(starts[k]), int(ends[k])


def blue(px):
    return (px[..., 2] > 120) & (px[..., 0] < 80)


def red(px):
    return (px[..., 0] > 120) & (px[..., 2] < 80)


def magenta(px):
    return (px[..., 0] > 150) & (px[..., 2] > 120)


def verify(fr, label):
    errors = []
    tol = 3
    for n, img in enumerate(fr):
        t = F(n, FPS)
        if t < F(3, 2):   # keyframed position at 50 % scale
            x = -0.25 + 0.5 * float(t / F(3, 2))
            exp0 = 960 + x * 1920 - 480
            r = runs(blue(img[600]))
            if r is None or abs(r[0] - exp0) > tol or abs((r[1] - r[0] + 1) - 960) > 2 * tol:
                errors.append(f"{label} frame {n}: blue run {r}, expected start {exp0:.1f} length 960")
            if not red(img[600, 100 if x > -0.2 else 1800]):
                errors.append(f"{label} frame {n}: background is not red")
        elif F(3, 2) <= t < F(12, 5):   # ellipse mask, Screen blend
            r = runs(magenta(img[540]))
            if r is None or abs(r[0] - 1014) > tol or abs(r[1] - 1553) > tol:
                errors.append(f"{label} frame {n}: magenta run {r}, expected (1014, 1553)")
            if not (red(img[540, 900]) and red(img[540, 1620])):
                errors.append(f"{label} frame {n}: outside the mask is not red")
        elif F(5, 2) <= t < F(7, 2):   # wipe right, no softness
            p = float((t - F(5, 2)) / 1)
            edge = p * 1920 - 0.5   # pixel centres with x + 0.5 < p * W are revealed
            row = img[540]
            if p > 0.02:
                rb = runs(blue(row))
                if rb is None or rb[0] > tol or abs(rb[1] - edge) > tol:
                    errors.append(f"{label} frame {n}: wipe edge {rb}, expected 0..{edge:.1f}")
            if p < 0.98 and not red(row[1915]):
                errors.append(f"{label} frame {n}: right side is not A (red)")
    return errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("renderer")
    ap.add_argument("workdir")
    ap.add_argument("--testmedia", default=str(REPO / "testmedia"))
    ap.add_argument("--perturb", action="store_true")
    a = ap.parse_args()
    work = Path(a.workdir) / ("m2_visual_perturbed" if a.perturb else "m2_visual")
    work.mkdir(parents=True, exist_ok=True)
    project = work / "m2_visual.json"
    write_project(Path(a.testmedia), project, a.perturb)
    outputs = {}
    for backend in ("cpu", "gpu"):
        out = work / f"m2_visual_{backend}.mp4"
        r = subprocess.run([a.renderer, str(project), str(out), "--backend", backend, "--crf", "10", "--no-audio"],
                           capture_output=True, text=True)
        print(r.stderr.strip().splitlines()[-1] if r.stderr.strip() else "(no output)")
        if r.returncode != 0:
            print(r.stderr)
            return 2
        outputs[backend] = frames(out)
        if len(outputs[backend]) != 150:
            print(f"{backend}: expected 150 frames, got {len(outputs[backend])}")
            return 1
    errors = verify(outputs["cpu"], "cpu") + verify(outputs["gpu"], "gpu")
    diffs = [float(np.abs(c - g).mean()) for c, g in zip(outputs["cpu"], outputs["gpu"])]
    print(f"CPU vs GPU output: mean abs difference per frame max {max(diffs):.3f}, average {sum(diffs) / len(diffs):.3f}")
    if max(diffs) > 1.0:
        errors.append(f"CPU and GPU exports differ: {max(diffs):.3f}")
    for e in errors[:30]:
        print("FAIL", e)
    print(f"{'PASS' if not errors else 'FAIL'}: 150 frames x 2 backends checked, {len(errors)} problems")
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
