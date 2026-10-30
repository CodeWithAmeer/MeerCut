"""Ground-truth clips for tracking / stabilisation (1920x1080, 30 fps, 5 s, H.264 BT.709-tagged).

L_track_1080p30_5s.mp4 : blurred-noise grey background; 160x160 checker/texture patch whose centre follows
                         x = 960 + 140*t, y = 540 + 120*sin(2*pi*t/5)  (t seconds). Path saved as JSON.
M_shaky_1080p30_5s.mp4 : static scene (noise texture + grid + red 80x80 marker at centre) translated each frame
                         by a seeded random-walk jitter (dx, dy); jitter saved as JSON.
"""
import json
import os
import subprocess

import numpy as np

OUT = os.path.join(os.path.dirname(__file__), "..", "testmedia")
W, H, FPS, N = 1920, 1080, 30, 150
rng = np.random.default_rng(7)


def blur(a, k=9):
    ker = np.ones(k) / k
    a = np.apply_along_axis(lambda r: np.convolve(r, ker, "same"), 1, a)
    return np.apply_along_axis(lambda c: np.convolve(c, ker, "same"), 0, a)


def writer(path):
    return subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                             "-r", str(FPS), "-i", "-", "-c:v", "libx264", "-crf", "14", "-pix_fmt", "yuv420p",
                             "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
                             "-vf", "scale=out_color_matrix=bt709", path], stdin=subprocess.PIPE)


# --- tracking clip
bg = (blur(rng.uniform(60, 190, (H, W)), 15)).clip(0, 255)
bg = np.repeat(bg[..., None], 3, 2).astype(np.uint8)
patch = np.zeros((160, 160, 3), np.uint8)
yy, xx = np.mgrid[0:160, 0:160]
patch[((xx // 20 + yy // 20) % 2) == 0] = (240, 240, 240)
patch[((xx // 20 + yy // 20) % 2) == 1] = (30, 30, 30)
patch[60:100, 60:100] = (220, 40, 40)
path = []
p = writer(os.path.join(OUT, "L_track_1080p30_5s.mp4"))
for n in range(N):
    t = n / FPS
    cx, cy = 960 + 140 * t, 540 + 120 * np.sin(2 * np.pi * t / 5)
    path.append((n, round(cx, 3), round(cy, 3)))
    f = bg.copy()
    x0, y0 = int(round(cx - 80)), int(round(cy - 80))
    f[y0:y0 + 160, x0:x0 + 160] = patch
    p.stdin.write(f.tobytes())
p.stdin.close(); p.wait()
json.dump({"centre_path": path, "note": "patch centre in pixels, integer-rounded placement"},
          open(os.path.join(OUT, "L_track_1080p30_5s.path.json"), "w"))

# --- shaky clip
pad = 80
scene = (blur(rng.uniform(40, 215, (H + 2 * pad, W + 2 * pad)), 7)).clip(0, 255)
scene = np.repeat(scene[..., None], 3, 2).astype(np.uint8)
scene[:, ::120] = 255
scene[::120, :] = 255
cy0, cx0 = (H + 2 * pad) // 2, (W + 2 * pad) // 2
scene[cy0 - 40:cy0 + 40, cx0 - 40:cx0 + 40] = (230, 20, 20)
jit = []
dx = dy = 0.0
p = writer(os.path.join(OUT, "M_shaky_1080p30_5s.mp4"))
for n in range(N):
    dx = float(np.clip(0.7 * dx + rng.normal(0, 9), -60, 60))
    dy = float(np.clip(0.7 * dy + rng.normal(0, 9), -60, 60))
    ix, iy = int(round(dx)), int(round(dy))
    f = scene[pad + iy:pad + iy + H, pad + ix:pad + ix + W]
    jit.append((n, ix, iy))
    p.stdin.write(np.ascontiguousarray(f).tobytes())
p.stdin.close(); p.wait()
json.dump({"jitter": jit, "note": "frame shows scene shifted so that marker appears at centre - (dx,dy)"},
          open(os.path.join(OUT, "M_shaky_1080p30_5s.jitter.json"), "w"))
print("ok")
