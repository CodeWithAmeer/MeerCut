# Start the editor built by build.ps1:  .\editor\scripts\run-editor.ps1 [project.lcproj | media files...]
param([Parameter(ValueFromRemainingArguments = $true)] [string[]]$Files)
$root = if ($env:FA_BUILD_ROOT) { $env:FA_BUILD_ROOT } else { "C:/dev/fa-build" }
$exe = Join-Path $root "msvc-release/bin/fa-editor.exe"
if (-not (Test-Path $exe)) { throw "fa-editor.exe not found at $exe - build it first: .\editor\scripts\build.ps1" }
$resolved = @($Files | Where-Object { $_ } | ForEach-Object { (Resolve-Path $_).Path })
if ($resolved.Count -gt 0) { Start-Process -FilePath $exe -ArgumentList ($resolved | ForEach-Object { "`"$_`"" }) }
else { Start-Process -FilePath $exe }
