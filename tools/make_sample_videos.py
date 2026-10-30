"""MeerCut's sample videos (M11): short clips to practise editing with, shown under Media > Sample Videos.

  bridge_conversation, scifi_lab_montage   excerpts of *Tears of Steel* (c) Blender Foundation | mango.blender.org
  snowy_adventure, dragon_and_desert       excerpts of the *Sintel* trailer (c) Blender Foundation | durian.blender.org
                                           both CC BY 3.0 (https://creativecommons.org/licenses/by/3.0/): cut at
                                           their shot changes (30 ms sound fades at the cuts), Tears of Steel
                                           letterboxed to 1280x720 like the Sintel trailer, re-encoded; picture
                                           and sound are otherwise as released
  green_screen_practice                    MeerCut's own clip, drawn here (an animated badge on chroma green)

The footage comes from testmedia/ai/ (tools/fetch_m7_testdata.py downloads it; each file is checked against the
SHA-256 in testmedia/ai/manifest.json first). Every clip is 1280x720 at 24 fps (the first clip on an empty timeline
can set the project to that, and the others then match it), H.264 with a keyframe every second (quick to scrub),
tagged BT.709, AAC 160k.

usage: python -I make_sample_videos.py [out dir]
       (default: <repo>/editor/assets/samples; writes *.mp4 and samples.json)
"""
import hashlib
import json
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
FOOTAGE = ROOT / "testmedia" / "ai"
CC_BY = "https://creativecommons.org/licenses/by/3.0/"
TOS = "Tears of Steel (c) Blender Foundation | mango.blender.org"
SINTEL = "Sintel (c) Blender Foundation | durian.blender.org"

# id, title, source file, first frame, frames (24 fps; cut at the shot changes), poster second, description, credit
EXCERPTS = [
    ("bridge_conversation", "Bridge Conversation", "tears_of_steel_60s.mov", 0, 486, 9.0,
     "A futuristic skyline, then two people arguing on a canal bridge. Practise trimming, splitting and "
     "captions.", TOS),
    ("scifi_lab_montage", "Sci-Fi Lab Montage", "tears_of_steel_60s.mov", 486, 581, 12.0,
     "Holograms, a '40 years later' title, a lab and a brain under a robot eye. Practise transitions and "
     "effects.", TOS),
    ("snowy_adventure", "Snowy Adventure", "sintel_trailer_720p.mp4", 0, 571, 6.0,
     "Snowy mountains, a climber, an old man's story and a run through dark streets. Practise colour and "
     "speed.", SINTEL),
    ("dragon_and_desert", "Dragon and Desert", "sintel_trailer_720p.mp4", 571, 563, 8.5,
     "Dragons over a ruined city, a fight in the sky and the desert, ending on the title. Practise music "
     "and fades.", SINTEL),
]

ENCODE = ["-c:v", "libx264", "-preset", "slow", "-profile:v", "high", "-sc_threshold", "0", "-pix_fmt", "yuv420p",
          "-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709", "-color_range", "tv",
          "-movflags", "+faststart", "-fflags", "+bitexact", "-map_metadata", "-1"]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def probe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames", "-show_entries",
                          "stream=width,height,r_frame_rate,nb_read_frames:format=duration", "-of", "json", str(path)],
                         capture_output=True, check=True, text=True).stdout
    j = json.loads(out)
    s = j["streams"][0]
    num, den = (int(v) for v in s["r_frame_rate"].split("/"))
    return {"width": s["width"], "height": s["height"], "fps": round(num / den, 3), "frames": int(s["nb_read_frames"]),
            "seconds": round(float(j["format"]["duration"]), 3)}


def excerpt(src, first, frames, out, credit):
    start, length = first / 24, frames / 24
    fade = f"afade=t=in:d=0.03,afade=t=out:st={length - 0.03:.3f}:d=0.03"   # no click where the cut falls mid-sound
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", f"{start:.6f}", "-i", str(src), "-t", f"{length:.6f}",
                    "-map", "0:v:0", "-map", "0:a:0", "-frames:v", str(frames),
                    "-vf", f"pad={W}:{H}:(ow-iw)/2:(oh-ih)/2", *ENCODE, "-g", "24", "-crf", "21",   # letterboxed to 16:9
                    "-c:a", "aac", "-b:a", "160k", "-ar", "48000", "-ac", "2", "-af", fade,
                    "-metadata", f"comment=Excerpt of {credit}, CC BY 3.0 ({CC_BY}); cut and re-encoded for MeerCut's "
                                 "sample videos", str(out)], check=True)


# ---- the green-screen clip ---------------------------------------------------------------------------------------

W, H, FPS, SECONDS = 1280, 720, 24, 20
GREEN = (0, 177, 64)   # chroma green


def supersampled(size, draw, factor=4):
    """An RGBA picture drawn at `factor` times the size and reduced (smooth edges)."""
    big = Image.new("RGBA", (size[0] * factor, size[1] * factor), (0, 0, 0, 0))
    draw(ImageDraw.Draw(big), factor)
    return big.resize(size, Image.LANCZOS)


def badge_sprite(side=300):
    """An orange rounded square with a white rim and a play symbol."""
    def gradient(d, k):
        s = side * k
        for y in range(s):   # orange, lighter at the top
            a = y / (s - 1)
            col = tuple(int(round(c0 + (c1 - c0) * a)) for c0, c1 in zip((255, 166, 48), (240, 84, 44)))
            d.line([(0, y), (s, y)], fill=col + (255,))

    fill = supersampled((side, side), gradient, 1)
    mask = supersampled((side, side), lambda d, k: d.rounded_rectangle([6 * k, 6 * k, (side - 6) * k, (side - 6) * k],
                                                                       radius=70 * k, fill=(255, 255, 255, 255)))
    out = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    out.paste(fill, (0, 0), mask.getchannel("A"))

    def rim_and_play(d, k):
        d.rounded_rectangle([6 * k, 6 * k, (side - 6) * k, (side - 6) * k], radius=70 * k, outline=(255, 255, 255, 255),
                            width=12 * k)
        d.rounded_rectangle([30 * k, 30 * k, (side - 30) * k, side * 0.45 * k], radius=48 * k, fill=(255, 255, 255, 46))
        cx, cy, r = side * 0.53 * k, side * 0.5 * k, side * 0.24 * k   # optically centred triangle
        d.polygon([(cx - r * 0.8, cy - r), (cx - r * 0.8, cy + r), (cx + r, cy)], fill=(255, 255, 255, 255))

    return Image.alpha_composite(out, supersampled((side, side), rim_and_play))


def sparkle_sprite(side=72):
    def star(d, k):
        c, r, w = side * k / 2, side * k / 2, side * k * 0.09
        d.polygon([(c, c - r), (c + w, c - w), (c + r, c), (c + w, c + w), (c, c + r), (c - w, c + w), (c - r, c),
                   (c - w, c - w)], fill=(255, 250, 225, 255))
        d.ellipse([c - w * 1.6, c - w * 1.6, c + w * 1.6, c + w * 1.6], fill=(255, 220, 120, 255))

    return supersampled((side, side), star)


def ease_out_back(x):
    c = 1.4
    return 1 + (c + 1) * (x - 1) ** 3 + c * (x - 1) ** 2


def ease_in_out(x):
    return 0.5 - 0.5 * math.cos(math.pi * min(max(x, 0.0), 1.0))


def badge_pose(t):
    """Centre, scale and angle (degrees) of the badge at t seconds; None once it has gone."""
    bob = 22 * math.sin(2 * math.pi * t / 2.4)
    tilt = 5 * math.sin(2 * math.pi * t / 3.1)
    pulse = 1 + 0.03 * math.sin(2 * math.pi * t / 1.2)
    if t < 1.2:   # flies in from the left
        a = ease_out_back(t / 1.2)
        return -220 + (470 + 220) * a, 360, 0.9 + 0.1 * a, -25 * (1 - a)
    if t < 9.0:
        return 470, 360 + bob, pulse, tilt
    if t < 10.5:   # glides to the right, leaning into the move
        a = ease_in_out((t - 9.0) / 1.5)
        return 470 + 340 * a, 360 + bob * (1 - a) + 22 * math.sin(2 * math.pi * 10.5 / 2.4) * a, pulse, \
            tilt * (1 - a) - 9 * math.sin(math.pi * a)
    if t < 17.5:
        return 810, 360 + bob, pulse, tilt
    if t < 19.0:   # spins away
        a = ease_in_out((t - 17.5) / 1.5)
        return 810, 360 + bob, (1 - a) * pulse, tilt + 360 * a
    return None


def composite(img, sprite, x, y):
    """alpha_composite at (x, y), which may be partly off the picture."""
    if x >= img.width or y >= img.height or x + sprite.width <= 0 or y + sprite.height <= 0:
        return
    img.alpha_composite(sprite, (max(0, x), max(0, y)), (max(0, -x), max(0, -y)))


def green_frame(t, background, badge, sparkle):
    img = background.copy()
    pose = badge_pose(t)
    if pose:
        x, y, s, ang = pose
        b = badge.resize((max(1, round(badge.width * s)), max(1, round(badge.height * s))), Image.BICUBIC)
        b = b.rotate(ang, resample=Image.BICUBIC, expand=True)
        composite(img, b, round(x - b.width / 2), round(y - b.height / 2))
        for k in range(5):   # twinkling stars around it
            tw = math.sin(2 * math.pi * (t * 0.7 + k * 0.21))
            if tw <= 0.05:
                continue
            ang_k = 2 * math.pi * k / 5 + t * 0.4
            r = 210 * s
            sz = max(1, round(sparkle.width * tw * (0.6 + 0.1 * k) * s))
            sp = sparkle.resize((sz, sz), Image.BICUBIC)
            composite(img, sp, round(x + r * math.cos(ang_k) - sz / 2), round(y + r * math.sin(ang_k) - sz / 2))
    return img.convert("RGB")


def green_screen(out):
    # an evenly lit screen, a touch darker towards the corners like a real one
    yy, xx = np.mgrid[0:H, 0:W]
    fall = 1 - 0.06 * (((xx - W / 2) / (W / 2)) ** 2 + ((yy - H / 2) / (H / 2)) ** 2) / 2
    bg = (np.array(GREEN, dtype=float)[None, None, :] * fall[:, :, None]).round().clip(0, 255).astype(np.uint8)
    background = Image.fromarray(np.dstack([bg, np.full((H, W), 255, np.uint8)]))
    badge, sparkle = badge_sprite(), sparkle_sprite()
    enc = subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                            "-r", str(FPS), "-i", "-", "-vf", "scale=in_range=pc:out_range=tv:out_color_matrix=bt709:"
                            "flags=accurate_rnd+full_chroma_int,format=yuv420p", *ENCODE, "-g", "24", "-crf", "16",
                            "-metadata", "comment=MeerCut sample video (original)", str(out)],
                           stdin=subprocess.PIPE)
    for i in range(FPS * SECONDS):
        enc.stdin.write(green_frame(i / FPS, background, badge, sparkle).tobytes())
    enc.stdin.close()
    if enc.wait() != 0:
        raise RuntimeError("ffmpeg failed on the green-screen clip")


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "editor" / "assets" / "samples"
    out.mkdir(parents=True, exist_ok=True)
    manifest = {e["file"]: e for e in json.loads((FOOTAGE / "manifest.json").read_text(encoding="utf-8"))["files"]}
    items = []
    for ident, title, src, first, frames, poster, desc, credit in EXCERPTS:
        path = FOOTAGE / src
        if sha256(path) != manifest[src]["sha256"]:
            raise SystemExit(f"{src} does not match testmedia/ai/manifest.json; run tools/fetch_m7_testdata.py")
        target = out / f"{ident}.mp4"
        excerpt(path, first, frames, target, credit)
        info = probe(target)
        if info["frames"] != frames:
            raise SystemExit(f"{target.name}: {info['frames']} frames, expected {frames}")
        items.append({"id": "sample." + ident, "title": title, "file": target.name, "description": desc,
                      "poster": poster, **{k: info[k] for k in ("seconds", "width", "height", "fps")},
                      "credit": credit, "licence": "CC BY 3.0", "licenceUrl": CC_BY,
                      "changes": f"frames {first}-{first + frames - 1} of {src} ({manifest[src].get('source', '')}), "
                                 + ("letterboxed to 1280x720, " if src.startswith("tears") else "")
                                 + "re-encoded with 30 ms sound fades at the cuts"})
        print(ident, info, flush=True)
    target = out / "green_screen_practice.mp4"
    green_screen(target)
    info = probe(target)
    items.append({"id": "sample.green_screen_practice", "title": "Green Screen Practice", "file": target.name,
                  "description": "An animated badge on a green screen. Put it on a track above another clip and "
                                 "remove the green with Chroma Key.", "poster": 5.0,
                  **{k: info[k] for k in ("seconds", "width", "height", "fps")}, "credit": "", "licence": "",
                  "licenceUrl": "", "changes": ""})
    print("green_screen_practice", info, flush=True)
    (out / "samples.json").write_text(json.dumps({"format": "meercut-sample-videos", "version": 1, "items": items},
                                                 indent=1), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
