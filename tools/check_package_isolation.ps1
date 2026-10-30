# Stage 3: approximates a clean Windows 11 machine for a packaged MeerCut (no second machine, no Windows Sandbox).
#   .\tools\check_package_isolation.ps1 -Package <MeerCut-...-win64 folder> [-Json out.json]
# 1. Static: every DLL imported by any program or DLL in the package (dumpbin /dependents) must be in the package or be
#    a Windows component in System32 (product name "Microsoft Windows Operating System"; API sets api-ms-* / ext-ms-*
#    are resolved by the loader). Anything else - a DLL that only some PCs have (e.g. a Visual C++ runtime installed
#    system-wide, Vulkan from a GPU driver) - is reported.
# 2. Dynamic: runs the packaged programs with PATH reduced to Windows' folders and records every module each process
#    actually loaded (while fa-render exports projects that exercise decoding, GPU compositing, encoding, text and the
#    AI runtimes). A module from outside the package, C:\Windows or the GPU driver store fails the check.
param([Parameter(Mandatory)][string]$Package, [string]$Json = "", [string]$Models = "C:\dev\fa-models",
      [string]$Projects = "", [switch]$StaticOnly)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "..\editor\scripts\devenv.ps1") | Out-Null
$Package = (Resolve-Path $Package).Path
$sys = Join-Path $env:WINDIR "System32"
$result = [ordered]@{ package = $Package; static = @(); dynamic = @(); problems = @() }

# ---- 1. static closure
$inPackage = @{}
Get-ChildItem $Package -Recurse -Include *.dll, *.exe | ForEach-Object { $inPackage[$_.Name.ToLower()] = $_.FullName }
$external = @{}
foreach ($f in $inPackage.Values) {
    $deps = dumpbin /nologo /dependents $f | Where-Object { $_ -match '^\s+(\S+\.dll)\s*$' } | ForEach-Object { $Matches[1] }
    foreach ($d in $deps) {
        $k = $d.ToLower()
        if ($inPackage.ContainsKey($k)) { continue }
        if (-not $external.ContainsKey($k)) { $external[$k] = @() }
        $external[$k] += (Split-Path $f -Leaf)
    }
}
foreach ($k in ($external.Keys | Sort-Object)) {
    $entry = [ordered]@{ dll = $k; importedBy = ($external[$k] | Sort-Object -Unique) -join ", " }
    if ($k -like "api-ms-*" -or $k -like "ext-ms-*") {
        $entry.verdict = "API set (resolved by the Windows loader)"
    } else {
        $p = Join-Path $sys $k
        if (Test-Path $p) {
            $vi = (Get-Item $p).VersionInfo
            $entry.product = $vi.ProductName
            $sig = Get-AuthenticodeSignature $p
            $windowsSigned = $sig.Status -eq "Valid" -and $sig.SignerCertificate.Subject -like "CN=Microsoft Windows,*"
            if ($vi.ProductName -match "Microsoft.*Windows.*Operating System") { $entry.verdict = "Windows component" }
            elseif ($windowsSigned) { $entry.verdict = "Windows component (signed by Microsoft Windows; e.g. Windows' own ICU)" }
            else { $entry.verdict = "NOT a Windows component"; $result.problems += "static: $k ($($vi.ProductName))" }
        } else {
            $entry.verdict = "MISSING"; $result.problems += "static: $k not in the package nor in System32"
        }
    }
    $result.static += [pscustomobject]$entry
}

if ($StaticOnly) {
    $result.static | Format-Table dll, verdict, product, importedBy -AutoSize | Out-String -Width 220 | Write-Host
    if ($result.problems.Count) { Write-Host "PROBLEMS:"; $result.problems | ForEach-Object { Write-Host "  $_" } }
    else { Write-Host "static check: every import is in the package or part of Windows" }
    if ($Json) { $result | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 $Json }
    return
}

# ---- 2. dynamic: modules actually loaded with a Windows-only PATH
$allowed = @($Package, $env:WINDIR)
# Vulkan implicit layers registered on this PC (e.g. a screen recorder's overlay) are injected by the Vulkan loader
# into every Vulkan program: they belong to the PC, not to the package, and are reported separately
$layerDlls = @{}
foreach ($root in "HKLM:\SOFTWARE\Khronos\Vulkan\ImplicitLayers", "HKCU:\SOFTWARE\Khronos\Vulkan\ImplicitLayers") {
    if (-not (Test-Path $root)) { continue }
    foreach ($manifest in (Get-Item $root).Property) {
        try {
            $j = Get-Content $manifest -Raw | ConvertFrom-Json
            foreach ($l in @($j.layer) + @($j.layers)) {
                if ($l -and $l.library_path) {
                    $lib = [IO.Path]::GetFullPath((Join-Path (Split-Path $manifest) $l.library_path))
                    $layerDlls[$lib.ToLower()] = $l.name
                }
            }
        } catch { }
    }
}
$result.environment = @()
function Test-Modules([System.Diagnostics.Process]$proc, [string]$what) {
    $mods = @()
    try { $proc.Refresh(); $mods = $proc.Modules | ForEach-Object { $_.FileName } } catch { return @() }
    $bad = $mods | Where-Object { $m = $_; -not ($allowed | Where-Object { $m.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) }) }
    return [pscustomobject]@{ what = $what; modules = $mods.Count; outside = @($bad) }
}
$ffprobe = (Get-Command ffprobe).Source   # a test tool, not part of the package: resolved before PATH is reduced
$savedPath = $env:Path
$env:Path = "$sys;$env:WINDIR;$sys\Wbem;$sys\WindowsPowerShell\v1.0"
$ErrorActionPreference = "Continue"   # the programs log to stderr (Windows PowerShell 5.1 would stop on it)
$env:MEERCUT_MODELS = $Models
try {
    # fa-probe: media + AI devices (ONNX Runtime CPU / DirectML, ncnn Vulkan, whisper Vulkan)
    $out = & (Join-Path $Package "fa-probe.exe") --ai 2>&1 | Out-String
    $result.probe = $out.Trim()
    # encoders: hardware sessions (NVENC, Quick Sync) load their runtimes by name - they must still open with the
    # restricted DLL search of the packaged programs
    $result.encoders = (& (Join-Path $Package "fa-probe.exe") --encoders 2>&1 | Out-String).Trim()
    # fa-render on each project, sampling the loaded modules while it runs
    if ($Projects) {
        foreach ($proj in Get-ChildItem $Projects -Filter *.json) {
            $outFile = Join-Path $env:TEMP ("meercut_iso_" + $proj.BaseName + ".mp4")
            $p = Start-Process (Join-Path $Package "fa-render.exe") -ArgumentList "`"$($proj.FullName)`" `"$outFile`"" `
                 -PassThru -NoNewWindow -RedirectStandardOutput "$outFile.log" -RedirectStandardError "$outFile.err"
            $null = $p.Handle   # keeps the process handle, without which ExitCode stays empty
            $seen = @{}; $outside = @{}
            while (-not $p.HasExited) {
                $s = Test-Modules $p $proj.Name
                if ($s) { foreach ($m in $s.outside) { $outside[$m] = $true }; $seen["n"] = [math]::Max([int]$seen["n"], $s.modules) }
                Start-Sleep -Milliseconds 200
            }
            $p.WaitForExit()
            $layers = @($outside.Keys | Where-Object { $layerDlls.ContainsKey($_.ToLower()) })
            $foreign = @($outside.Keys | Where-Object { -not $layerDlls.ContainsKey($_.ToLower()) })
            $probe = if (Test-Path $outFile) { (& $ffprobe -v error -show_entries stream=codec_type,codec_name,width,height -of csv=p=0 $outFile 2>$null) -join "; " } else { "" }
            $entry = [ordered]@{ what = "fa-render " + $proj.Name; exit = $p.ExitCode; maxModules = $seen["n"]
                                 outside = $foreign; vulkanLayersOfThisPC = $layers; output = $probe
                                 lastLine = ((Get-Content "$outFile.log" -ErrorAction SilentlyContinue) | Select-Object -Last 1) }
            if ($p.ExitCode -ne 0 -or -not $probe) { $result.problems += "fa-render $($proj.Name): exit $($p.ExitCode), output '$probe'" }
            foreach ($m in $foreign) { $result.problems += "dynamic: $($proj.Name) loaded $m" }
            foreach ($m in $layers) {
                $note = "$m (Vulkan implicit layer '$($layerDlls[$m.ToLower()])' registered on this PC)"
                if ($result.environment -notcontains $note) { $result.environment += $note }
            }
            $result.dynamic += [pscustomobject]$entry
            Remove-Item $outFile, "$outFile.log", "$outFile.err" -ErrorAction SilentlyContinue
        }
    }
    # the editor: start it, let it initialise (Qt platform, styles, D3D11, audio device), record, close
    $p = Start-Process (Join-Path $Package "MeerCut.exe") -PassThru
    $null = $p.WaitForInputIdle(60000)
    Start-Sleep -Seconds 4
    $s = Test-Modules $p "MeerCut.exe (idle, empty project)"
    $result.dynamic += $s
    foreach ($m in $s.outside) { $result.problems += "dynamic: MeerCut.exe loaded $m" }
    $null = $p.CloseMainWindow()
    if (-not $p.WaitForExit(15000)) { $p.Kill() }
} finally {
    $env:Path = $savedPath
    Remove-Item Env:\MEERCUT_MODELS -ErrorAction SilentlyContinue
}
$result.static | Format-Table dll, verdict, importedBy -AutoSize | Out-String -Width 200 | Write-Host
$result.dynamic | Format-List | Out-String -Width 200 | Write-Host
Write-Host "encoders (fa-probe --encoders):"; Write-Host $result.encoders
if ($result.environment.Count) { Write-Host "loaded from this PC's environment (not the package):"; $result.environment | ForEach-Object { Write-Host "  $_" } }
if ($result.problems.Count) { Write-Host "PROBLEMS:"; $result.problems | ForEach-Object { Write-Host "  $_" } }
else { Write-Host "no problems: every import is in the package or part of Windows; nothing loaded from elsewhere" }
if ($Json) { $result | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 $Json }
