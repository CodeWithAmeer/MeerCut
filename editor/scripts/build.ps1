# Configure, build and (optionally) test the engine.
#   .\editor\scripts\build.ps1 [-Preset msvc-release|msvc-debug] [-Test] [-TestArgs "-L e2e"]
# Full logs go to $env:FA_BUILD_ROOT\<preset>\logs\; a summary is printed.
param(
    [string]$Preset = "msvc-release",
    [switch]$Test,
    [string]$TestArgs = ""
)
. (Join-Path $PSScriptRoot "devenv.ps1")
$editor = Split-Path $PSScriptRoot
$logs = Join-Path $env:FA_BUILD_ROOT "$Preset\logs"
New-Item -ItemType Directory -Force $logs | Out-Null
Push-Location $editor
try {
    cmd /c "cmake --preset $Preset > `"$logs\configure.log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content "$logs\configure.log" -Tail 40; throw "configure failed ($logs\configure.log)" }
    cmd /c "cmake --build --preset $Preset > `"$logs\build.log`" 2>&1"
    $buildExit = $LASTEXITCODE
    Select-String -Path "$logs\build.log" -Pattern "error|warning C" | Select-Object -First 60 | ForEach-Object { $_.Line }
    if ($buildExit -ne 0) { throw "build failed ($logs\build.log)" }
    Write-Host "build OK"
    if ($Test) {
        cmd /c "ctest --preset $Preset $TestArgs > `"$logs\test.log`" 2>&1"
        $testExit = $LASTEXITCODE
        Get-Content "$logs\test.log" | Select-String -Pattern "Test +#|tests passed|tests failed|FAIL|Failed|Errors while" | ForEach-Object { $_.Line }
        if ($testExit -ne 0) { throw "tests failed ($logs\test.log)" }
    }
} finally {
    Pop-Location
}
