"""Downloads the M7 test recordings and footage into testmedia/ai/ (git-ignored media;
testmedia/ai/manifest.json, which is tracked, records every file's source, licence and SHA-256).

  librispeech/  73 real utterances with reference transcripts: LibriSpeech dev-clean sample (hf-internal-testing/
                librispeech_asr_dummy), CC BY 4.0, via the Hugging Face datasets-server
  fleurs/       10 utterances each in Spanish (es_419), French (fr_fr) and Arabic (ar_eg) with transcripts: FLEURS
                test split (google/fleurs), CC BY 4.0; the audio archive is streamed and only its first members read
  jfk.wav       J. F. Kennedy, inaugural address excerpt (public domain), whisper.cpp v1.8.6 sample
  sintel_trailer_720p.mp4   Sintel trailer (c) Blender Foundation, CC BY 3.0 (durian.blender.org)
  tears_of_steel_60s.mov    60 s of Tears of Steel (c) Blender Foundation, CC BY 3.0 (mango.blender.org), cut
                            with ffmpeg stream copy from the 720p release without downloading the whole film

usage: python fetch_m7_testdata.py [out dir]   (default: <repo>/testmedia/ai)
"""
import csv
import hashlib
import io
import json
import subprocess
import sys
import tarfile
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UA = {"User-Agent": "MeerCut-dev-fetch/1.0"}


def get(url, timeout=120):
    return urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=timeout)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def librispeech(out, entries):
    d = out / "librispeech"
    d.mkdir(parents=True, exist_ok=True)
    api = ("https://datasets-server.huggingface.co/rows?dataset=hf-internal-testing/librispeech_asr_dummy"
           "&config=clean&split=validation&offset={}&length=100")
    rows = json.load(get(api.format(0)))["rows"]
    refs = {}
    for r in rows:
        row = r["row"]
        target = d / f"{row['id']}.flac"
        if not target.exists():
            target.write_bytes(get(row["audio"][0]["src"]).read())
        refs[target.name] = {"text": row["text"], "speaker": row["speaker_id"]}
        entries.append({"file": f"librispeech/{target.name}", "sha256": sha256(target), "licence": "CC BY 4.0",
                        "source": "LibriSpeech dev-clean via hf-internal-testing/librispeech_asr_dummy"})
    (d / "transcripts.json").write_text(json.dumps(refs, indent=1, ensure_ascii=False), encoding="utf-8")
    print(f"librispeech: {len(refs)} utterances", flush=True)


def fleurs(out, entries, lang, count=10):
    d = out / "fleurs" / lang
    d.mkdir(parents=True, exist_ok=True)
    base = f"https://huggingface.co/datasets/google/fleurs/resolve/main/data/{lang}"
    tsv = get(f"{base}/test.tsv").read().decode("utf-8")
    meta = {}
    for row in csv.reader(io.StringIO(tsv), delimiter="\t", quoting=csv.QUOTE_NONE):
        if len(row) >= 4:
            meta[row[1]] = {"raw": row[2], "normalised": row[3]}
    refs = {}
    with get(f"{base}/audio/test.tar.gz", timeout=300) as stream:
        with tarfile.open(fileobj=stream, mode="r|gz") as tar:
            for m in tar:
                if not m.isfile() or not m.name.endswith(".wav"):
                    continue
                name = Path(m.name).name
                if name not in meta:
                    continue
                target = d / name
                data = tar.extractfile(m).read()
                if not target.exists():
                    target.write_bytes(data)
                refs[name] = meta[name]
                entries.append({"file": f"fleurs/{lang}/{name}", "sha256": sha256(target), "licence": "CC BY 4.0",
                                "source": f"google/fleurs {lang} test"})
                if len(refs) >= count:
                    break
    (d / "transcripts.json").write_text(json.dumps(refs, indent=1, ensure_ascii=False), encoding="utf-8")
    print(f"fleurs {lang}: {len(refs)} utterances", flush=True)


def plain(out, entries, name, url, licence, source):
    target = out / name
    if not target.exists():
        target.write_bytes(get(url, timeout=300).read())
    entries.append({"file": name, "sha256": sha256(target), "licence": licence, "source": source, "url": url})
    print(f"{name}: {target.stat().st_size} bytes", flush=True)


def tears_of_steel(out, entries):
    target = out / "tears_of_steel_60s.mov"
    url = "https://download.blender.org/demo/movies/ToS/tears_of_steel_720p.mov"
    if not target.exists():
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", "00:00:20", "-i", url, "-t", "60", "-map", "0:v:0",
                        "-map", "0:a:0?", "-c", "copy", str(target)], check=True)
    entries.append({"file": target.name, "sha256": sha256(target), "licence": "CC BY 3.0 (c) Blender Foundation",
                    "source": url + " from 00:00:20, 60 s, stream copy"})
    print(f"{target.name}: {target.stat().st_size} bytes", flush=True)


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "testmedia" / "ai"
    out.mkdir(parents=True, exist_ok=True)
    entries = []
    plain(out, entries, "jfk.wav", "https://raw.githubusercontent.com/ggml-org/whisper.cpp/v1.8.6/samples/jfk.wav",
          "public domain", "whisper.cpp v1.8.6 samples/jfk.wav")
    plain(out, entries, "sintel_trailer_720p.mp4", "https://download.blender.org/durian/trailer/sintel_trailer-720p.mp4",
          "CC BY 3.0 (c) Blender Foundation", "durian.blender.org")
    tears_of_steel(out, entries)
    librispeech(out, entries)
    for lang in ("es_419", "fr_fr", "ar_eg"):
        fleurs(out, entries, lang)
    (out / "manifest.json").write_text(json.dumps({"files": entries}, indent=1, ensure_ascii=False), encoding="utf-8")
    print("files:", len(entries))


if __name__ == "__main__":
    main()
