# Dot-source this script to get an MSVC x64 developer environment plus CMake/Ninja/Git on PATH:
#   . .\scripts\devenv.ps1
# Works in Windows PowerShell 5.1 and PowerShell 7. Leaves the caller's $ErrorActionPreference unchanged.
$faSavedEap = $ErrorActionPreference
$ErrorActionPreference = "Stop"
try {

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - install Visual Studio 2022 Build Tools (see BUILD.md)" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio installation with the MSVC x64 toolset found" }

$installerDir = Split-Path $vswhere
if ($env:Path -notlike "*$installerDir*") { $env:Path = "$installerDir;$env:Path" }   # vcvars calls vswhere
if (-not $env:VSCMD_VER) {
    Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}

$extra = @("C:\Program Files\Git\cmd", "C:\Program Files\CMake\bin")
$ninja = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if ($ninja) { $extra += $ninja.DirectoryName }
foreach ($p in $extra) { if ((Test-Path $p) -and ($env:Path -notlike "*$p*")) { $env:Path = "$p;$env:Path" } }

if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "C:\dev\vcpkg" }
if (-not $env:FA_BUILD_ROOT) { $env:FA_BUILD_ROOT = "C:/dev/fa-build" }
Write-Host "MSVC dev env ready: cl=$((Get-Command cl).Source)"
} finally {
    $ErrorActionPreference = $faSavedEap
}
