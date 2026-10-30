# Generates deterministic, measurable test media with FFmpeg.
# Every video carries a 16-bit binary frame index in a strip at the top
# (16 cells of 120x60 px, LSB on the left, white = 1) so that exported
# output can be decoded frame-by-frame with tools/framecode.py.
param([string]$Out = (Join-Path $PSScriptRoot "..\testmedia"))
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $Out | Out-Null

# geq expression: top 60 px = binary frame index, rest untouched.
$code = "lum='if(lt(Y,60),if(lt(mod(X,120),6),16,235*mod(floor(N/pow(2,floor(X/120))),2)+16*(1-mod(floor(N/pow(2,floor(X/120))),2))),lum(X,Y))':cb='if(lt(Y,60),128,cb(X,Y))':cr='if(lt(Y,60),128,cr(X,Y))'"
$label = "drawtext=fontfile='C\:/Windows/Fonts/arial.ttf':text='%{n}':x=(w-tw)/2:y=h-200:fontsize=120:fontcolor=white:box=1:boxcolor=black@0.6"

function Clip($name, $src, $extra, $hz = 1000) {
    $vf = "$extra,format=yuv420p,$label,geq=$code"
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i $src -f lavfi -i "sine=frequency=${hz}:sample_rate=48000:duration=10" `
        -vf $vf -c:v libx264 -preset veryfast -crf 12 -g 30 -pix_fmt yuv420p -c:a aac -b:a 192k -shortest (Join-Path $Out $name)
}

# A/B solid clips: transitions are measured by mixing ratio of red vs blue.
Clip "A_red_1080p30_10s.mp4"  "color=c=0xC00000:s=1920x1080:r=30:d=10" "null"
# B carries a 1.5 kHz tone so audio crossfades can be separated per clip.
Clip "B_blue_1080p30_10s.mp4" "color=c=0x0000C0:s=1920x1080:r=30:d=10" "null" 1500
# Moving pattern: motion/transform/speed tests.
Clip "C_testsrc2_1080p30_10s.mp4" "testsrc2=s=1920x1080:r=30:d=10" "null"
# Green screen with moving white disc (chroma key).
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "color=c=0x00B140:s=1920x1080:r=30:d=10" `
    -vf "geq=lum='if(lt(hypot(X-(400+120*T*10),Y-540),180),235,lum(X,Y))':cb='if(lt(hypot(X-(400+120*T*10),Y-540),180),128,cb(X,Y))':cr='if(lt(hypot(X-(400+120*T*10),Y-540),180),128,cr(X,Y))',format=yuv420p" `
    -c:v libx264 -preset veryfast -crf 12 -pix_fmt yuv420p (Join-Path $Out "D_greenscreen_disc_1080p30_10s.mp4")
# Stills: geometry grid and colour reference.
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "color=c=gray:s=1920x1080,drawgrid=w=120:h=120:t=2:c=white,drawbox=x=900:y=480:w=120:h=120:c=red:t=fill" -frames:v 1 (Join-Path $Out "E_grid_1080p.png")
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "smptehdbars=s=1920x1080" -frames:v 1 (Join-Path $Out "F_smptebars_1080p.png")
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "gradients=s=1920x1080:c0=black:c1=white:x0=0:y0=0:x1=1919:y1=0:n=2" -frames:v 1 (Join-Path $Out "G_gray_ramp_1080p.png")
# Audio reference: 10 s 1 kHz tone at -12 dBFS.
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=10" -af "volume=-12dB" (Join-Path $Out "H_tone1k_-12dB_10s.wav")
# HDR (M4): 10-bit BT.2020 PQ / HLG with exact code values (limited range): left half reference white (BT.2408:
# PQ 0.5807 = 203 nits -> Y 573; HLG 0.75 -> Y 721; Cb = Cr = 512), right half R'G'B' 0.6 / 0.3 / 0.3
# (Y 396, Cb 475, Cr 646 with the BT.2020 matrix).
foreach ($k in @(@("pq", "smpte2084", 573), @("hlg", "arib-std-b67", 721))) {
    $trc = $k[1]; $wy = $k[2]
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i "color=c=black:s=1920x1080:r=30:d=2" `
        -vf "format=yuv420p10le,geq=lum='if(lt(X,960),$wy,396)':cb='if(lt(X,480),512,475)':cr='if(lt(X,480),512,646)',setparams=range=tv:color_primaries=bt2020:color_trc=${trc}:colorspace=bt2020nc" `
        -c:v libx264 -profile:v high10 -preset veryfast -crf 4 -color_primaries bt2020 -color_trc $trc -colorspace bt2020nc -color_range tv `
        -x264-params "colorprim=bt2020:transfer=${trc}:colormatrix=bt2020nc" (Join-Path $Out ("P_hdr_" + $k[0] + "_1080p30_2s.mp4"))
}
Get-ChildItem $Out | Select-Object Name, Length
