"""LUT test inputs (original, generated):
  testmedia/K_color_grid_1080p.png  - 24x13 grid of seeded random RGB patches (exact PNG) + layout JSON
  testmedia/luts/lut_identity_33.cube          - identity, size 33 (sanity)
  testmedia/luts/lut_random_3.cube             - size 3, random smooth mapping (distinguishes trilinear vs tetrahedral)
  testmedia/luts/lut_invert_2.cube             - size 2, RGB invert (exact for both interpolations)
  testmedia/luts/lut_domain_half_2.cube        - size 2 identity with DOMAIN_MAX 0.5 0.5 0.5 (tests DOMAIN handling)
"""
import json
import os

import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(__file__), "..", "testmedia")
LUTS = os.path.join(ROOT, "luts")
os.makedirs(LUTS, exist_ok=True)
rng = np.random.default_rng(42)

# colour grid (rows avoid watermark band 640-872 by leaving it as neutral grey filler)
cols, rows = 24, 13
pw, ph = 1920 // cols, 1080 // rows
img = np.full((1080, 1920, 3), 128, np.uint8)
patches = []
for r in range(rows):
    y0 = r * ph
    if 630 <= y0 + ph and y0 <= 880:
        continue
    for c in range(cols):
        rgb = rng.integers(0, 256, 3)
        x0 = c * pw
        img[y0:y0 + ph, x0:x0 + pw] = rgb
        patches.append({"rgb": rgb.tolist(), "box": [x0 + 12, y0 + 12, x0 + pw - 12, y0 + ph - 12]})
Image.fromarray(img).save(os.path.join(ROOT, "K_color_grid_1080p.png"))
json.dump({"patches": patches}, open(os.path.join(ROOT, "K_color_grid_1080p.layout.json"), "w"))


def write_cube(path, size, table, title, domain_max=None):
    with open(path, "w", newline="\n") as f:
        f.write(f'TITLE "{title}"\n')
        if domain_max is not None:
            f.write("DOMAIN_MIN 0.0 0.0 0.0\n")
            f.write("DOMAIN_MAX {0} {0} {0}\n".format(domain_max))
        f.write(f"LUT_3D_SIZE {size}\n")
        # .cube order: red fastest, then green, then blue
        for b in range(size):
            for g in range(size):
                for r in range(size):
                    v = table[r, g, b]
                    f.write(f"{v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
    np.save(path.replace(".cube", ".npy"), table)


def identity(size):
    ax = np.linspace(0, 1, size)
    R, G, B = np.meshgrid(ax, ax, ax, indexing="ij")
    return np.stack([R, G, B], axis=-1)


write_cube(os.path.join(LUTS, "lut_identity_33.cube"), 33, identity(33), "identity33")
t = identity(3) + rng.uniform(-0.25, 0.25, (3, 3, 3, 3))
write_cube(os.path.join(LUTS, "lut_random_3.cube"), 3, np.clip(t, 0, 1), "random3")
write_cube(os.path.join(LUTS, "lut_invert_2.cube"), 2, 1 - identity(2), "invert2")
write_cube(os.path.join(LUTS, "lut_domain_half_2.cube"), 2, identity(2), "domainhalf", domain_max=0.5)
print("patches:", len(patches), "luts:", sorted(os.listdir(LUTS)))
