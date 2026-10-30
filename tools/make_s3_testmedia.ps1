# Stage 3 robustness media (tools/make_testmedia.ps1 makes the regular test media). Every picture carries the same
# frame-index strip as the Stage 1 media (16 cells across the top; editor/tests/support/framecode.h reads it), so a
# test can tell exactly which source frame is shown.
#   R_vfr_30_15_10s.mp4      variable frame rate: 30 fps for 5 s, then every second frame only (15 fps); timestamps kept
#   S_offset_start_10s.ts    MPEG-TS whose timestamps start at about 3.9 s (an output offset of 2.5 s + the muxer delay)
#   T_truncated.mp4          a 10 s faststart MP4 cut off at 60 % of its bytes (the end of the file is missing)
#   U_corrupt_middle.mp4     the same file with 64 KB of its middle overwritten
#   V_garbage.mp4            200 KB of pseudo-random bytes named .mp4
#   W_empty.mp4              0 bytes
param([string]$Out = (Join-Path $PSScriptRoot "..\testmedia"))
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $Out | Out-Null

$code = "lum='if(lt(Y,60),if(lt(mod(X,120),6),16,235*mod(floor(N/pow(2,floor(X/120))),2)+16*(1-mod(floor(N/pow(2,floor(X/120))),2))),lum(X,Y))':cb='if(lt(Y,60),128,cb(X,Y))':cr='if(lt(Y,60),128,cr(X,Y))'"
$src = "testsrc2=s=1920x1080:r=30:d=10"

# variable frame rate: the strip is drawn before frames are dropped, so it shows the original frame number
ffmpeg -hide_banner -loglevel error -y -f lavfi -i $src -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=10" `
    -vf "format=yuv420p,geq=$code,select='lt(n\,150)+gte(n\,150)*not(mod(n\,2))'" -fps_mode vfr `
    -c:v libx264 -preset veryfast -crf 14 -pix_fmt yuv420p -c:a aac -b:a 128k -shortest (Join-Path $Out "R_vfr_30_15_10s.mp4")

ffmpeg -hide_banner -loglevel error -y -f lavfi -i $src -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=10" `
    -vf "format=yuv420p,geq=$code" -c:v libx264 -preset veryfast -crf 14 -g 30 -pix_fmt yuv420p -c:a aac -b:a 128k `
    -shortest -output_ts_offset 2.5 (Join-Path $Out "S_offset_start_10s.ts")

$whole = Join-Path $Out "_faststart_tmp.mp4"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i $src -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=10" `
    -vf "format=yuv420p,geq=$code" -c:v libx264 -preset veryfast -crf 14 -g 30 -pix_fmt yuv420p -c:a aac -b:a 128k `
    -shortest -movflags +faststart $whole
$bytes = [System.IO.File]::ReadAllBytes($whole)
$fs = [System.IO.File]::Create((Join-Path $Out "T_truncated.mp4"))
$fs.Write($bytes, 0, [int]($bytes.Length * 0.6))
$fs.Close()
$mid = [int]($bytes.Length / 2)
for ($i = 0; $i -lt 65536; $i++) { $bytes[$mid + $i] = 0xA5 }
[System.IO.File]::WriteAllBytes((Join-Path $Out "U_corrupt_middle.mp4"), $bytes)
Remove-Item $whole

$rng = New-Object System.Random 7
$garbage = New-Object byte[] 204800
$rng.NextBytes($garbage)
[System.IO.File]::WriteAllBytes((Join-Path $Out "V_garbage.mp4"), $garbage)
[System.IO.File]::WriteAllBytes((Join-Path $Out "W_empty.mp4"), (New-Object byte[] 0))
Write-Host "Stage 3 test media written to $Out"
