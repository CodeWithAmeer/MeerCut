"""Frame-accurate measurement of an exported video.

For each decoded output frame, reports:
  * idx   - 16-bit frame index read from the top strip written by
            make_testmedia.ps1 (-1 when the strip is ambiguous, e.g. mid-blend)
  * conf  - min distance of any strip cell from the 50% threshold (0..1)
  * R,G,B - mean colour of a centre region (default x 760..1160, y 340..740,
            scaled to the actual resolution) - used to measure blend ratios
  * mixB  - for red(A)/blue(B) tests: B / (R + B), i.e. fraction of clip B

Usage: python framecode.py <video> [--csv out.csv] [--every N]
"""
import argparse
import csv
import json
import subprocess
import sys

import numpy as np


def probe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                          "-show_entries", "stream=width,height,r_frame_rate",
                          "-of", "json", path], capture_output=True, text=True, check=True)
    s = json.loads(out.stdout)["streams"][0]
    num, den = map(int, s["r_frame_rate"].split("/"))
    return s["width"], s["height"], num / den


def frames(path, w, h):
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    size = w * h * 3
    while True:
        buf = proc.stdout.read(size)
        if len(buf) < size:
            break
        yield np.frombuffer(buf, np.uint8).reshape(h, w, 3)
    proc.wait()


def decode_strip(img):
    h, w, _ = img.shape
    sy = h / 1080.0
    sx = w / 1920.0
    row = img[int(10 * sy):int(50 * sy)].mean(axis=2)  # luma-ish
    bits, conf = 0, 1.0
    for b in range(16):
        cell = row[:, int((b * 120 + 20) * sx):int((b * 120 + 100) * sx)].mean() / 255.0
        conf = min(conf, abs(cell - 0.5) * 2)
        if cell > 0.5:
            bits |= 1 << b
    return (bits if conf > 0.5 else -1), conf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--csv")
    ap.add_argument("--every", type=int, default=1)
    a = ap.parse_args()
    w, h, fps = probe(a.video)
    sx, sy = w / 1920.0, h / 1080.0
    rows = []
    for n, img in enumerate(frames(a.video, w, h)):
        if n % a.every:
            continue
        idx, conf = decode_strip(img)
        reg = img[int(340 * sy):int(740 * sy), int(760 * sx):int(1160 * sx)].reshape(-1, 3).mean(axis=0)
        r, g, b = (float(x) for x in reg)
        mix = b / (r + b) if (r + b) > 1 else float("nan")
        rows.append({"frame": n, "t": round(n / fps, 4), "idx": idx, "conf": round(conf, 3),
                     "R": round(r, 1), "G": round(g, 1), "B": round(b, 1), "mixB": round(mix, 4)})
    if a.csv:
        with open(a.csv, "w", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=list(rows[0]))
            wr.writeheader()
            wr.writerows(rows)
    print(f"{a.video}: {w}x{h} @ {fps:.3f} fps, {len(rows)} frames analysed", file=sys.stderr)
    for r in rows:
        print(r["frame"], r["t"], r["idx"], r["conf"], r["R"], r["G"], r["B"], r["mixB"])


if __name__ == "__main__":
    main()
