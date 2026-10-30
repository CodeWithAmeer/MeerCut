"""Exact-RGB chroma-key test chart (PNG, 1920x1080) -> testmedia/I_chroma_chart_1080p.png + layout JSON.

Background: key green (0,177,64). Row 1 (y 150-330): green->white mixes 0,10,...,100 %.
Row 2 (y 380-560): green->mid-grey(128) mixes 0..100 %. Row 3 small patches (y 590-630) avoided (watermark).
Row 4 (y 900-1000, below the free-plan watermark band): reference colours white, grey, black, red, blue,
skin, yellow, cyan, magenta, dark green (0,100,40), light green (120,220,140).
Strip (y 1020-1060): horizontal gradient green -> white over x 160..1760 for edge/feather softness.
"""
import json
import os

import numpy as np
from PIL import Image

OUT = os.path.join(os.path.dirname(__file__), "..", "testmedia")
G = np.array([0, 177, 64], float)
W = np.array([255, 255, 255], float)
M = np.array([128, 128, 128], float)
img = np.zeros((1080, 1920, 3), float) + G
layout = {"key_green": G.tolist(), "patches": []}
for i in range(11):
    f = i / 10
    x0 = 70 + i * 165
    for row, (y0, y1, other, tag) in enumerate(((150, 330, W, "g2white"), (380, 560, M, "g2grey"))):
        c = (1 - f) * G + f * other
        img[y0 + 20:y1 - 20, x0 + 20:x0 + 145] = c
        layout["patches"].append({"tag": f"{tag}_{int(f*100)}", "rgb": c.round().tolist(),
                                  "box": [x0 + 30, y0 + 30, x0 + 135, y1 - 30]})
refs = [("white", W), ("grey", M), ("black", [0, 0, 0]), ("red", [220, 30, 30]), ("blue", [30, 60, 220]),
        ("skin", [224, 172, 105]), ("yellow", [240, 220, 40]), ("cyan", [40, 220, 230]), ("magenta", [220, 40, 200]),
        ("dark_green", [0, 100, 40]), ("light_green", [120, 220, 140])]
for i, (name, c) in enumerate(refs):
    x0 = 70 + i * 165
    img[900:1000, x0 + 20:x0 + 145] = c
    layout["patches"].append({"tag": f"ref_{name}", "rgb": list(map(float, c)), "box": [x0 + 30, 910, x0 + 135, 990]})
xs = np.arange(160, 1760)
f = (xs - 160) / (1760 - 160)
img[1020:1060, 160:1760] = (1 - f)[:, None] * G + f[:, None] * W
layout["gradient"] = {"y": [1025, 1055], "x": [160, 1760], "from": G.tolist(), "to": W.tolist()}
Image.fromarray(img.round().astype(np.uint8)).save(os.path.join(OUT, "I_chroma_chart_1080p.png"))
json.dump(layout, open(os.path.join(OUT, "I_chroma_chart_1080p.layout.json"), "w"), indent=1)
print("wrote chart + layout")
