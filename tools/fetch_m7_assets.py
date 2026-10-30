"""Downloads the M7 development assets: the ONNX Runtime DirectML package, the AI models
and nothing else. Every file is written to a .part file first, hashed (SHA-256) and only then renamed; files that
already exist with the expected hash are skipped. The editor itself downloads models through its model manager
(src/ai/model_catalog.cpp) with the same URLs and hashes; this script is for development and tests.

usage: python fetch_m7_assets.py <models dir> <third-party dir> [--only name,name]
Prints a JSON manifest (name, url, bytes, sha256) at the end.
"""
import hashlib
import json
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

ASSETS = [
    # name, url, file name, expected size (bytes, from the source's own listing), expected sha256 (None = record)
    ("onnxruntime-directml", "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.24.4",
     "microsoft.ml.onnxruntime.directml.1.24.4.nupkg", None, None),
    ("rvm-mobilenetv3", "https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp32.onnx",
     "rvm_mobilenetv3_fp32.onnx", 14975696, None),
    ("birefnet-general-tiny",
     "https://github.com/ZhengPeng7/BiRefNet/releases/download/v1/BiRefNet-general-bb_swin_v1_tiny-epoch_232.onnx",
     "BiRefNet-general-bb_swin_v1_tiny-epoch_232.onnx", 224005088, None),
    ("yunet", "https://huggingface.co/opencv/face_detection_yunet/resolve/main/face_detection_yunet_2023mar.onnx",
     "face_detection_yunet_2023mar.onnx", None, None),
    ("whisper-tiny", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin", "ggml-tiny.bin", None, None),
    ("whisper-base", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin", "ggml-base.bin", None, None),
    ("whisper-base.en", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin", "ggml-base.en.bin",
     None, None),
    ("whisper-small", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin", "ggml-small.bin", None,
     None),
    ("rife-v4.6-param",
     "https://raw.githubusercontent.com/nihui/rife-ncnn-vulkan/a7532fc3f9f8f008cd6eecd6f2ffe2a9698e0cf7/models/rife-v4.6/flownet.param",
     "rife-v4.6/flownet.param", 16532, None),
    ("rife-v4.6-bin",
     "https://raw.githubusercontent.com/nihui/rife-ncnn-vulkan/a7532fc3f9f8f008cd6eecd6f2ffe2a9698e0cf7/models/rife-v4.6/flownet.bin",
     "rife-v4.6/flownet.bin", 10614320, None),
    ("realesrgan-ncnn-windows",
     "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip",
     "realesrgan-ncnn-vulkan-20220424-windows.zip", 45474481, None),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(url, target, size):
    target.parent.mkdir(parents=True, exist_ok=True)
    part = target.with_name(target.name + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": "MeerCut-dev-fetch/1.0"})
    with urllib.request.urlopen(req, timeout=120) as r, open(part, "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    got = part.stat().st_size
    if size is not None and got != size:
        part.unlink()
        raise RuntimeError(f"{url}: {got} bytes, the source lists {size}")
    part.replace(target)


def main():
    models, third = Path(sys.argv[1]), Path(sys.argv[2])
    only = None
    if "--only" in sys.argv:
        only = set(sys.argv[sys.argv.index("--only") + 1].split(","))
    manifest = []
    for name, url, file, size, want in ASSETS:
        if only and name not in only:
            continue
        target = (third if name == "onnxruntime-directml" else models) / file
        if not target.exists():
            print(f"downloading {name} ...", flush=True)
            fetch(url, target, size)
        digest = sha256(target)
        if want and digest != want:
            raise RuntimeError(f"{name}: sha256 {digest}, expected {want}")
        manifest.append({"name": name, "url": url, "file": str(target), "bytes": target.stat().st_size, "sha256": digest})
        print(f"{name}: {target.stat().st_size} bytes sha256 {digest}", flush=True)
    # the ONNX Runtime package and the Real-ESRGAN pack are zip files: unpack what is needed
    nupkg = third / "microsoft.ml.onnxruntime.directml.1.24.4.nupkg"
    ort_dir = third / "onnxruntime-directml-1.24.4"
    if nupkg.exists() and not ort_dir.exists():
        with zipfile.ZipFile(nupkg) as z:
            z.extractall(ort_dir)
    esr = models / "realesrgan-ncnn-vulkan-20220424-windows.zip"
    if esr.exists() and not (models / "realesrgan").exists():
        with zipfile.ZipFile(esr) as z:
            for n in z.namelist():
                if "/models/" in n.replace("\\", "/") or n.replace("\\", "/").startswith("models/"):
                    if n.endswith("/"):
                        continue
                    out = models / "realesrgan" / Path(n).name
                    out.parent.mkdir(parents=True, exist_ok=True)
                    out.write_bytes(z.read(n))
    print(json.dumps(manifest, indent=1))


if __name__ == "__main__":
    main()
