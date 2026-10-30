# Stage 3: install -> use -> reinstall -> uninstall test of the MeerCut installer, on this PC (per user, no admin).
#   .\tools\check_installer.ps1 -Setup <...-setup.exe> [-Work C:\dev\fa-out\install-test] [-Gui] [-Json out.json]
# Installs silently into <Work>\app with the .mcproj association, checks the files, version, uninstall entry, Start
# menu shortcut and association; exports an example project with the installed fa-render (ffprobe-checked); with -Gui
# opens that project in the installed MeerCut.exe through its file association and closes it; reinstalls over the
# installation; uninstalls silently and checks that the program, its shortcut, uninstall entry and association are
# gone while the user's data is untouched: the MeerCut data folders (file list + SHA-256 before and after), the
# preferences in the registry, and the project / export written in <Work>.
param([Parameter(Mandatory)][string]$Setup, [string]$Work = "C:\dev\fa-out\install-test", [switch]$Gui,
      [string]$Json = "", [string]$Example = "")
$ErrorActionPreference = "Stop"
$Setup = (Resolve-Path $Setup).Path
$repo = Split-Path $PSScriptRoot
if (-not $Example) { $Example = Join-Path $repo "editor\examples\m6_export_sample.json" }
# the installer's own AppId (a copy here went stale when the rename gave MeerCut a new one)
$appId = [regex]::Match((Get-Content (Join-Path $repo "editor\installer\meercut.iss") -Raw), 'AppId=\{(\{[0-9A-Fa-f-]+\})').Groups[1].Value
if (-not $appId) { throw "no AppId in editor\installer\meercut.iss" }
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\${appId}_is1"
$shortcut = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\MeerCut\MeerCut.lnk"
$app = Join-Path $Work "app"
$checks = New-Object System.Collections.ArrayList
function Check([string]$name, [bool]$ok, [string]$detail = "") {
    [void]$checks.Add([pscustomobject]@{ check = $name; result = $(if ($ok) { "PASS" } else { "FAIL" }); detail = $detail })
    Write-Host ("{0}  {1}  {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $name, $detail)
}
function Snapshot-UserData {
    $roots = @((Join-Path $env:LOCALAPPDATA "MeerCut"), (Join-Path $env:APPDATA "MeerCut"))
    $list = @()
    foreach ($r in $roots) {
        if (-not (Test-Path $r)) { continue }
        Get-ChildItem $r -Recurse -File -Force -ErrorAction SilentlyContinue | Where-Object { $_.Length -lt 512MB } | ForEach-Object {
            $list += "{0}|{1}|{2}" -f $_.FullName, $_.Length, (Get-FileHash -Algorithm SHA256 $_.FullName).Hash
        }
    }
    $reg = if (Test-Path "HKCU:\Software\MeerCut") { (Get-ChildItem "HKCU:\Software\MeerCut" -Recurse | Out-String) } else { "" }
    return @{ files = ($list | Sort-Object); registry = $reg }
}
if (Test-Path $uninstallKey) { throw "MeerCut is already installed for this user ($uninstallKey): uninstall it first" }
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force $Work | Out-Null
$extKeyBefore = Test-Path "HKCU:\Software\Classes\.mcproj"
$ErrorActionPreference = "Continue"   # the programs log to stderr (Windows PowerShell 5.1 would stop on it)

# ---- install
$t = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process $Setup -ArgumentList "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER /DIR=`"$app`" /TASKS=`"assoc`" /LOG=`"$Work\install.log`"" -PassThru -Wait
Check "install exit code" ($p.ExitCode -eq 0) "exit $($p.ExitCode), $([math]::Round($t.Elapsed.TotalSeconds, 1)) s"
$exe = Join-Path $app "MeerCut.exe"
Check "program files installed" ((Test-Path $exe) -and (Test-Path (Join-Path $app "fa-render.exe")) -and (Test-Path (Join-Path $app "platforms\qwindows.dll")) -and (Test-Path (Join-Path $app "licenses\THIRD_PARTY_NOTICES.txt"))) "$((Get-ChildItem $app -Recurse -File).Count) files"
$vi = (Get-Item $exe).VersionInfo
Check "version resource" ($vi.ProductName -eq "MeerCut" -and $vi.ProductVersion) "$($vi.ProductName) $($vi.ProductVersion)"
Check "uninstall entry (per user)" (Test-Path $uninstallKey) ((Get-ItemProperty $uninstallKey -ErrorAction SilentlyContinue).DisplayName)
Check "Start menu shortcut" (Test-Path $shortcut) $shortcut
Check ".mcproj association" ((Get-ItemProperty "HKCU:\Software\Classes\MeerCut.Project\shell\open\command" -ErrorAction SilentlyContinue).'(default)' -like "*MeerCut.exe*")

# ---- use: export with the installed renderer, probe the result
$proj = Join-Path $Work "project.mcproj"
$text = Get-Content $Example -Raw
$exDir = (Split-Path (Resolve-Path $Example).Path).Replace('\', '/')
$text = [regex]::Replace($text, '"path":\s*"(\.\./[^"]+)"', { param($m) '"path": "' + ([IO.Path]::GetFullPath((Join-Path $exDir $m.Groups[1].Value))).Replace('\', '/') + '"' })
Set-Content -Encoding utf8 $proj $text
$out = Join-Path $Work "export.mp4"
$t.Restart()
& (Join-Path $app "fa-render.exe") $proj $out *> (Join-Path $Work "render.log")
$renderOk = $LASTEXITCODE -eq 0 -and (Test-Path $out)
$probe = if ($renderOk) { ffprobe -v error -show_entries stream=codec_type,codec_name,width,height -of csv=p=0 $out } else { "" }
Check "export with the installed fa-render" ($renderOk -and ($probe -join " ") -match "video") "$([math]::Round($t.Elapsed.TotalSeconds, 1)) s; $($probe -join '; ')"
$probeOut = & (Join-Path $app "fa-probe.exe") $out 2>&1 | Out-String
Check "installed fa-probe reads the export" ($LASTEXITCODE -eq 0) (($probeOut -split "`n" | Select-Object -First 1).Trim())

if ($Gui) {   # opens the project through the file association, like a double-click in Explorer
    $before_ = @(Get-Process MeerCut -ErrorAction SilentlyContinue).Id
    Start-Process $proj
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $lp = $null
    while (-not $lp -and $sw.ElapsedMilliseconds -lt 30000) {
        Start-Sleep -Milliseconds 200
        $lp = Get-Process MeerCut -ErrorAction SilentlyContinue | Where-Object { $before_ -notcontains $_.Id -and $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    }
    Check "opens a .mcproj through the association" ($null -ne $lp -and $lp.Path -eq $exe) "$($lp.Path); title '$($lp.MainWindowTitle)'"
    if ($lp) {
        Start-Sleep -Seconds 3
        $lp.Refresh()
        Check "the project is open in the window title" ($lp.MainWindowTitle -like "*project*") $lp.MainWindowTitle
        $null = $lp.CloseMainWindow()
        if (-not $lp.WaitForExit(15000)) { $lp.Kill(); Check "closes normally" $false "killed" } else { Check "closes normally" $true }
    }
}

# ---- the user's data as it is after using MeerCut, plus marker files in each data folder (removed at the end), so
# that "unchanged after uninstalling" is checked on real content even on a PC without earlier MeerCut data
$markers = @((Join-Path $env:LOCALAPPDATA "MeerCut\install-test-marker.txt"), (Join-Path $env:LOCALAPPDATA "MeerCut\models\install-test-marker.txt"),
             (Join-Path $env:LOCALAPPDATA "MeerCut\autosave\install-test-marker.txt"), (Join-Path $env:APPDATA "MeerCut\MeerCut\install-test-marker.txt"))
$createdMarkers = @(); $createdDirs = @()
foreach ($m in $markers) {
    if (Test-Path $m) { continue }
    $d = Split-Path $m
    while (-not (Test-Path $d)) { $createdDirs += $d; $d = Split-Path $d }
    New-Item -ItemType Directory -Force (Split-Path $m) | Out-Null
    Set-Content -Encoding ascii $m "MeerCut installer test marker $(Get-Date -Format o)"
    $createdMarkers += $m
}
$before = Snapshot-UserData
Write-Host "user data before reinstall / uninstall: $($before.files.Count) files, registry $(if ($before.registry) { 'present' } else { 'absent' })"

# ---- reinstall over the installation
$p = Start-Process $Setup -ArgumentList "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER /DIR=`"$app`" /TASKS=`"assoc`" /LOG=`"$Work\reinstall.log`"" -PassThru -Wait
Check "reinstall exit code" ($p.ExitCode -eq 0) "exit $($p.ExitCode)"
Check "reinstalled program runs" ((& (Join-Path $app "fa-probe.exe") $out 2>&1 | Out-String) -and $LASTEXITCODE -eq 0)

# ---- uninstall
$uninst = (Get-ItemProperty $uninstallKey).UninstallString.Trim('"')
$p = Start-Process $uninst -ArgumentList "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=`"$Work\uninstall.log`"" -PassThru -Wait
Start-Sleep -Seconds 3   # the uninstaller finishes from a temporary copy
Check "uninstall exit code" ($p.ExitCode -eq 0) "exit $($p.ExitCode)"
Check "program folder removed" (-not (Test-Path $exe)) $(if (Test-Path $app) { "left: " + ((Get-ChildItem $app -Recurse -Force | ForEach-Object Name) -join ", ") } else { "gone" })
Check "uninstall entry removed" (-not (Test-Path $uninstallKey))
Check "Start menu shortcut removed" (-not (Test-Path $shortcut))
Check ".mcproj association removed" ((-not (Test-Path "HKCU:\Software\Classes\MeerCut.Project")) -and ((Test-Path "HKCU:\Software\Classes\.mcproj") -eq $extKeyBefore))
$after = Snapshot-UserData
$same = (($before.files -join "`n") -eq ($after.files -join "`n"))
Check "user data folders unchanged" $same "$($after.files.Count) files"
Check "preferences in the registry unchanged" ($before.registry -eq $after.registry)
Check "the user's project and export kept" ((Test-Path $proj) -and (Test-Path $out))

foreach ($m in $createdMarkers) { Remove-Item $m -ErrorAction SilentlyContinue }   # only the markers this test made
foreach ($d in $createdDirs) { if ((Test-Path $d) -and -not (Get-ChildItem $d -Force)) { Remove-Item $d } }   # and their folders, if still empty

$failed = @($checks | Where-Object result -eq "FAIL").Count
Write-Host "$($checks.Count - $failed) / $($checks.Count) checks passed"
if ($Json) { @{ setup = $Setup; setupSha256 = (Get-FileHash -Algorithm SHA256 $Setup).Hash.ToLower(); checks = $checks } | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 $Json }
