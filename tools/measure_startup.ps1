# Stage 3: MeerCut's startup time and memory, measured from outside the process.
#   .\tools\measure_startup.ps1 -Exe <...\MeerCut.exe> [-Runs 5] [-Json out.json]
# Per run: process start -> main window created, -> input idle (the event loop runs, the window is usable), then the
# working set and private memory 3 s later with an empty project. The window is closed normally afterwards (an empty
# project closes without a question). The first run after a reboot or a new build is the "cold" one.
param([Parameter(Mandatory)][string]$Exe, [int]$Runs = 5, [string]$Json = "")
$ErrorActionPreference = "Stop"
$measured = @()
for ($i = 1; $i -le $Runs; $i++) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $Exe -PassThru
    $null = $p.Handle   # keeps the handle: ExitCode is empty without it
    while ($p.MainWindowHandle -eq 0 -and $sw.ElapsedMilliseconds -lt 60000) {
        Start-Sleep -Milliseconds 5
        $p.Refresh()
    }
    $window = $sw.ElapsedMilliseconds
    $null = $p.WaitForInputIdle(60000)
    $idle = $sw.ElapsedMilliseconds
    Start-Sleep -Seconds 3
    $p.Refresh()
    $run = [ordered]@{ run = $i; window_ms = $window; input_idle_ms = $idle
                       working_set_mb = [math]::Round($p.WorkingSet64 / 1MB, 1)
                       private_mb = [math]::Round($p.PrivateMemorySize64 / 1MB, 1) }
    $null = $p.CloseMainWindow()
    if (-not $p.WaitForExit(15000)) { $p.Kill(); $run.closed = "killed" } else { $run.closed = "normally (exit $($p.ExitCode))" }
    $measured += [pscustomobject]$run
    Start-Sleep -Seconds 1
}
$measured | Format-Table -AutoSize | Out-String | Write-Host
$sorted = $measured | Sort-Object input_idle_ms
$median = $sorted[[int][math]::Floor(($sorted.Count - 1) / 2)]
Write-Host ("median: window {0} ms, input idle {1} ms, working set {2} MB" -f $median.window_ms, $median.input_idle_ms, $median.working_set_mb)
if ($Json) { @{ exe = $Exe; runs = $measured } | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 $Json }
