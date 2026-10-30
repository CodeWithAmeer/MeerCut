# Release packaging for Windows 11 x64.
#   .\editor\scripts\package.ps1 [-SkipBuild] [-Out C:\dev\fa-build\release]
# 1. an optimised build (preset msvc-dist: Release, no tests)
# 2. the program folder MeerCut-<version>-win64\: MeerCut.exe (the editor), fa-render.exe, fa-probe.exe, exactly the DLLs
#    they import (read from their import tables, transitively; Windows' own DLLs are not copied), the DLLs loaded at run
#    time by name (ONNX Runtime's provider bridge), the Qt plugins the editor uses, the Microsoft C++ runtime
#    (app-local, as Microsoft's redistribution terms allow), and licenses\ (LICENSE.txt = GPL-3.0, THIRD_PARTY_NOTICES.txt
#    built from each shipped vcpkg port's own copyright file, SOURCE.txt)
# 3. the portable ZIP, the installer (Inno Setup 6, when installed), the source archive of the commit (git archive;
#    only from a clean working tree) and SHA256SUMS.txt
param([switch]$SkipBuild, [string]$Out = "C:\dev\fa-build\release")
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "devenv.ps1")
$editor = Split-Path $PSScriptRoot
$repo = Split-Path $editor
$build = Join-Path $env:FA_BUILD_ROOT "msvc-dist"

if (-not $SkipBuild) {
    Push-Location $editor
    try {
        cmd /c "cmake --preset msvc-dist > `"$env:FA_BUILD_ROOT\msvc-dist-configure.log`" 2>&1"
        if ($LASTEXITCODE -ne 0) { throw "configure failed: $env:FA_BUILD_ROOT\msvc-dist-configure.log" }
        cmd /c "cmake --build --preset msvc-dist --target fa-editor fa-render fa-probe > `"$env:FA_BUILD_ROOT\msvc-dist-build.log`" 2>&1"
        if ($LASTEXITCODE -ne 0) { throw "build failed: $env:FA_BUILD_ROOT\msvc-dist-build.log" }
    } finally { Pop-Location }
}

$bin = Join-Path $build "bin"
$vcpkg = Join-Path $build "vcpkg_installed"
$label = (Select-String -Path (Join-Path $build "generated\fa\core\version.h") -Pattern 'MEERCUT_VERSION_LABEL "(.*)"').Matches[0].Groups[1].Value
$name = "MeerCut-$label-win64"
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$stage = Join-Path $Out $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage, (Join-Path $stage "licenses") | Out-Null

# ---- executables
Copy-Item (Join-Path $bin "fa-editor.exe") (Join-Path $stage "MeerCut.exe")
Copy-Item (Join-Path $bin "fa-render.exe"), (Join-Path $bin "fa-probe.exe") $stage
# M9: MeerCut's own library content (synthesised music and sound effects); M11: the sample videos
Copy-Item -Recurse (Join-Path $editor "assets") (Join-Path $stage "assets")

# ---- Qt plugins: the platform, the native style, image formats for icons
$qtPlugins = Join-Path $vcpkg "x64-windows\Qt6\plugins"
$plugins = @("platforms\qwindows.dll")
foreach ($sub in "styles", "imageformats") {
    if (Test-Path (Join-Path $qtPlugins $sub)) {
        Get-ChildItem (Join-Path $qtPlugins $sub) -Filter *.dll | ForEach-Object { $plugins += "$sub\$($_.Name)" }
    }
}
foreach ($p in $plugins) {
    $dir = Join-Path $stage (Split-Path $p)
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item (Join-Path $qtPlugins $p) $dir
}

# ---- DLL closure from the import tables
$redist = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC" -Directory |
    Where-Object { $_.Name -match '^\d' } | Sort-Object Name | Select-Object -Last 1
$crt = @()
foreach ($d in "Microsoft.VC143.CRT", "Microsoft.VC143.OpenMP") {
    $crt += Get-ChildItem (Join-Path $redist.FullName "x64\$d") -Filter *.dll
}
$crtByName = @{}
foreach ($f in $crt) { $crtByName[$f.Name.ToLower()] = $f.FullName }
$shipped = @{}
$queue = New-Object System.Collections.Queue
Get-ChildItem $stage -Recurse -Include *.exe, *.dll | ForEach-Object { $queue.Enqueue($_.FullName) }
# loaded at run time by name (not in any import table)
foreach ($n in "onnxruntime_providers_shared.dll") {
    Copy-Item (Join-Path $bin $n) $stage
    $shipped[$n.ToLower()] = $true
    $queue.Enqueue((Join-Path $stage $n))
}
while ($queue.Count -gt 0) {
    $file = $queue.Dequeue()
    $deps = dumpbin /nologo /dependents $file | Where-Object { $_ -match '^\s+(\S+\.dll)\s*$' } | ForEach-Object { $Matches[1] }
    foreach ($d in $deps) {
        $k = $d.ToLower()
        if ($shipped.ContainsKey($k)) { continue }
        $local = Join-Path $bin $d
        if (Test-Path $local) {
            Copy-Item $local $stage
            $shipped[$k] = $true
            $queue.Enqueue((Join-Path $stage $d))
        } elseif ($crtByName.ContainsKey($k)) {
            Copy-Item $crtByName[$k] $stage
            $shipped[$k] = $true
        }   # anything else is part of Windows (KERNEL32, USER32, d3d11, dxgi, api-ms-win-crt-* ...)
    }
}
# the whole C++ runtime set (a DLL may load a sibling at run time)
foreach ($f in $crt) { if (-not $shipped.ContainsKey($f.Name.ToLower())) { Copy-Item $f.FullName $stage; $shipped[$f.Name.ToLower()] = $true } }

# ---- licences: MeerCut, every shipped component, the source offer
Copy-Item (Join-Path $repo "LICENSE") (Join-Path $stage "licenses\LICENSE.txt")
$dllToPort = @{}
Get-ChildItem (Join-Path $vcpkg "vcpkg\info") -Filter *.list | ForEach-Object {
    $port = ($_.BaseName -split '_')[0]
    foreach ($line in Get-Content $_.FullName) {
        if ($line -match '^x64-windows/(bin|Qt6/plugins/.+)/([^/]+\.dll)$') { $dllToPort[$Matches[2].ToLower()] = $port }
    }
}
# Every port vcpkg built for this (test-free) build: shipped as a DLL, linked statically, or header-only in MeerCut's
# own code - all of them end up in the programs. Build-only helpers are left out.
$ports = @{}
Get-ChildItem (Join-Path $vcpkg "vcpkg\info") -Filter *.list | ForEach-Object {
    $port = ($_.BaseName -split '_')[0]
    if ($port -notmatch '^(vcpkg-|pkgconf$|gtest$)') { $ports[$port] = $true }
}
$unmapped = @($shipped.Keys | Where-Object { -not $dllToPort.ContainsKey($_) -and -not $crtByName.ContainsKey($_) -and $_ -notlike "onnxruntime*" })
if ($unmapped.Count) { throw "shipped DLLs with no known origin (add their licence): $($unmapped -join ', ')" }
$notices = New-Object System.Text.StringBuilder
[void]$notices.AppendLine("MeerCut $label - third-party components and their licences")
[void]$notices.AppendLine("MeerCut itself is licensed under the GNU General Public License version 3 or later (LICENSE.txt).")
[void]$notices.AppendLine("Because FFmpeg is built here with x264 and x265 (GPL), the distribution as a whole is GPL-3.0-or-later.")
[void]$notices.AppendLine("Version of every component: the vcpkg baseline and ports listed in SOURCE.txt.")
[void]$notices.AppendLine("")
foreach ($p in ($ports.Keys | Sort-Object)) {
    $copyright = Join-Path $vcpkg "x64-windows\share\$p\copyright"
    if (-not (Test-Path $copyright)) { continue }
    [void]$notices.AppendLine(("=" * 100))
    [void]$notices.AppendLine("$p")
    [void]$notices.AppendLine(("=" * 100))
    [void]$notices.AppendLine((Get-Content $copyright -Raw))
}
$ort = "C:\dev\third_party\onnxruntime-directml-1.24.4"
[void]$notices.AppendLine(("=" * 100)); [void]$notices.AppendLine("ONNX Runtime 1.24.4 (Microsoft.ML.OnnxRuntime.DirectML)"); [void]$notices.AppendLine(("=" * 100))
[void]$notices.AppendLine((Get-Content (Join-Path $ort "LICENSE") -Raw))
[void]$notices.AppendLine((Get-Content (Join-Path $ort "ThirdPartyNotices.txt") -Raw))
[void]$notices.AppendLine(("=" * 100)); [void]$notices.AppendLine("Microsoft Visual C++ runtime (app-local)"); [void]$notices.AppendLine(("=" * 100))
[void]$notices.AppendLine("vcruntime / msvcp / vcomp DLLs from Visual Studio 2022 ($($redist.Name)), redistributed under the Microsoft")
[void]$notices.AppendLine("Software License Terms for Visual Studio (Distributable Code). DirectML.dll is not shipped: Windows provides it.")
[void]$notices.AppendLine("")
[void]$notices.AppendLine("AI models are not part of this distribution: the Model Manager downloads them from their own projects on request;")
[void]$notices.AppendLine("their licences are listed in AI_MODELS.md of the source repository and shown in the Model Manager.")
[void]$notices.AppendLine("")
[void]$notices.AppendLine("The sample videos (assets\samples) include excerpts of Blender Foundation films under CC BY 3.0: see SAMPLE_VIDEOS.txt.")
Set-Content -Encoding utf8 (Join-Path $stage "licenses\THIRD_PARTY_NOTICES.txt") $notices.ToString()

# ---- M11: the sample videos' credits (CC BY 3.0 asks for them wherever the clips go)
$samplesIndex = Join-Path $editor "assets\samples\samples.json"
if (-not (Test-Path $samplesIndex)) { throw "the sample videos' index is missing: $samplesIndex" }
$sampleLines = @("MeerCut $label - sample videos (Media > Sample Videos; the files are in assets\samples)", "")
foreach ($s in (Get-Content $samplesIndex -Raw -Encoding UTF8 | ConvertFrom-Json).items) {
    if ($s.credit) {
        $sampleLines += "$($s.title) ($($s.file)): $($s.credit)"
        $sampleLines += "  Licence: $($s.licence) - $($s.licenceUrl)"
        $sampleLines += "  Changes: $($s.changes)"
    } else {
        $sampleLines += "$($s.title) ($($s.file)): MeerCut's own clip, released with MeerCut (GPL-3.0-or-later)"
    }
    $sampleLines += ""
}
$sampleLines += "If you share a video made with one of the Blender Foundation clips, credit it the same way."
$sampleLines += "MeerCut is not affiliated with or endorsed by the Blender Foundation."
Set-Content -Encoding utf8 (Join-Path $stage "licenses\SAMPLE_VIDEOS.txt") $sampleLines

$status = Get-Content (Join-Path $vcpkg "vcpkg\status") -Raw
$baseline = (Get-Content (Join-Path $editor "vcpkg.json") -Raw | ConvertFrom-Json).'builtin-baseline'
$portList = ($ports.Keys | Sort-Object | ForEach-Object {
    $m = [regex]::Match($status, "Package: $([regex]::Escape($_))\r?\nVersion: ([^\r\n]+)")
    "  $_ $($m.Groups[1].Value)"
}) -join "`r`n"
$commit = (git -C $repo rev-parse HEAD).Trim()
# a package is a release only when built from a commit: tracked files with uncommitted changes are reported
$dirty = @(git -C $repo status --porcelain --untracked-files=no)
$sourceNote = "(the same files as this release)"
if ($dirty.Count) {
    $sourceNote = "PLUS UNCOMMITTED CHANGES - a test build, not a release"
    Write-Warning "uncommitted changes in $($dirty.Count) tracked file(s): this package is a test build, not a release"
}
Set-Content -Encoding utf8 (Join-Path $stage "licenses\SOURCE.txt") @"
MeerCut $label - corresponding source

MeerCut is made by CodeWithAmeer (https://github.com/CodeWithAmeer). It is free software under the GNU General Public License
version 3 or later. This binary distribution contains
GPL-licensed components (FFmpeg built with x264 and x265). Their complete corresponding source is:

* MeerCut: the source repository at commit $commit $sourceNote, including editor/vcpkg.json,
  which pins every third-party component.
* Third-party components: built from source by vcpkg at baseline $baseline (github.com/microsoft/vcpkg); each port
  (with the patches it applies, in the vcpkg repository at that baseline) downloads its upstream source archive,
  recorded with its SHA-512 in the port. Ports and versions shipped here:
$portList
* ONNX Runtime 1.24.4: the NuGet package Microsoft.ML.OnnxRuntime.DirectML 1.24.4 (MIT; source at
  github.com/microsoft/onnxruntime, tag v1.24.4).

Whoever distributes this binary must also make that source available under the same terms (GPL-3.0 section 6),
for example by publishing the source archive of this release next to it.
"@

# ---- documentation shipped with the program
Copy-Item (Join-Path $repo "README.md") (Join-Path $stage "README.md") -ErrorAction SilentlyContinue
Copy-Item (Join-Path $repo "CHANGELOG.md") (Join-Path $stage "CHANGELOG.md") -ErrorAction SilentlyContinue
Copy-Item (Join-Path $repo "CREDITS.md") (Join-Path $stage "CREDITS.md")   # the library's and the sample videos' credits

# ---- portable ZIP
$zip = Join-Path $Out "$name-portable.zip"
if (Test-Path $zip) { Remove-Item $zip }
# Entry names with "/" as the ZIP format requires: under Windows PowerShell 5.1 both Compress-Archive and
# ZipFile.CreateFromDirectory write "\" (Windows unpacks that, other tools make file names containing backslashes)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zs = [IO.File]::Open($zip, [IO.FileMode]::CreateNew)
$za = New-Object IO.Compression.ZipArchive($zs, [IO.Compression.ZipArchiveMode]::Create)
try {
    Get-ChildItem $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
        $entry = "$name/" + $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $_.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $za.Dispose()
    $zs.Dispose()
}

# ---- installer (Inno Setup 6)
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
          "C:\Program Files\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
$setup = $null
if ($iscc) {
    & $iscc /Q "/DAppVersion=$label" "/DNumericVersion=$((Select-String -Path (Join-Path $build 'generated\fa\core\version.h') -Pattern 'MEERCUT_VERSION "(.*)"').Matches[0].Groups[1].Value)" `
        "/DStageDir=$stage" "/DOutputDir=$Out" "/DOutputName=$name-setup" (Join-Path $editor "installer\meercut.iss")
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed" }
    $setup = Join-Path $Out "$name-setup.exe"
} else {
    Write-Warning "Inno Setup 6 (ISCC.exe) not found: no installer built"
}

# ---- the corresponding source (GPL-3.0 section 6): the commit this package was built from, as git archives it
$src = $null
if ($dirty.Count) {
    Write-Warning "no source archive: a release's source is its commit, and the working tree has uncommitted changes"
} else {
    $src = Join-Path $Out "MeerCut-$label-source.zip"
    if (Test-Path $src) { Remove-Item $src }
    git -C $repo archive --format=zip "--prefix=MeerCut-$label-source/" -o $src $commit
    if ($LASTEXITCODE -ne 0) { throw "git archive failed" }
}

# ---- checksums
$sums = Join-Path $Out "SHA256SUMS.txt"
$files = @($zip) + @($setup | Where-Object { $_ }) + @($src | Where-Object { $_ }) +
         @((Join-Path $stage "MeerCut.exe"), (Join-Path $stage "fa-render.exe"), (Join-Path $stage "fa-probe.exe"))
$sumLines = $files | ForEach-Object {
    $h = Get-FileHash -Algorithm SHA256 $_
    "$($h.Hash.ToLower())  $($_.Substring($Out.TrimEnd('\').Length + 1).Replace('\', '/'))"
}
# LF line ends: "sha256sum -c SHA256SUMS.txt" (Linux, macOS, Git Bash) reads a CR as part of the file name
[IO.File]::WriteAllText($sums, (($sumLines -join "`n") + "`n"), [Text.Encoding]::ASCII)
$dlls = (Get-ChildItem $stage -Recurse -Filter *.dll).Count
$size = "{0:N1} MB" -f ((Get-ChildItem $stage -Recurse | Measure-Object Length -Sum).Sum / 1MB)
Write-Host "packaged $name : $dlls DLLs, $size; ZIP $zip; installer $setup; source $src; checksums $sums"
