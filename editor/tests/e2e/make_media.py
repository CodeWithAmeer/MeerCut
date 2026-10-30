"""Generate the extra frame-coded test clips the M0 end-to-end tests need (NTSC 30000/1001 rate).

Same strip encoding as tools/make_testmedia.ps1 (16 cells of 120x60 px at 1920 width, LSB left,
white = 1) so tools/framecode.py reads them. Existing Stage 1 media are never touched.

usage: python make_media.py <testmedia dir>      (skips files that already exist)
"""
import subprocess
import sys
from pathlib import Path

CODE = ("lum='if(lt(Y,60*H/1080),if(lt(mod(X,120*W/1920),6*W/1920),16,"
        "235*mod(floor(N/pow(2,floor(X/(120*W/1920)))),2)+16*(1-mod(floor(N/pow(2,floor(X/(120*W/1920)))),2))),lum(X,Y))'"
        ":cb='if(lt(Y,60*H/1080),128,cb(X,Y))':cr='if(lt(Y,60*H/1080),128,cr(X,Y))'")

CLIPS = [
    # name, colour, size, rate, seconds, tone (Hz; amplitude 1/8 like the Stage 1 A/B clips)
    ("N_green_720p2997_6s.mp4", "0x00A000", "1280x720", "30000/1001", 6, 1000),
    ("O_magenta_720p2997_6s.mp4", "0xB000B0", "1280x720", "30000/1001", 6, 1500),
]


def has_audio(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a", "-show_entries", "stream=index",
                          "-of", "csv=p=0", str(path)], capture_output=True, text=True)
    return bool(out.stdout.strip())


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[3] / "testmedia")
    out.mkdir(parents=True, exist_ok=True)
    for name, colour, size, rate, secs, tone in CLIPS:
        dst = out / name
        if dst.exists() and has_audio(dst):
            print("exists:", dst)
            continue
        vf = f"format=yuv420p,geq={CODE}"
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
               "-f", "lavfi", "-i", f"color=c={colour}:s={size}:r={rate}:d={secs}",
               "-f", "lavfi", "-i", f"sine=frequency={tone}:sample_rate=48000:duration={secs + 1}",
               "-vf", vf, "-c:v", "libx264", "-preset", "veryfast", "-crf", "12", "-g", "30",
               "-pix_fmt", "yuv420p", "-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709",
               "-c:a", "aac", "-b:a", "192k", "-shortest", str(dst)]
        subprocess.run(cmd, check=True)
        print("wrote:", dst)


if __name__ == "__main__":
    main()
