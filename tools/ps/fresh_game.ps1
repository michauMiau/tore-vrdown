$ErrorActionPreference = 'Stop'
$root = 'D:\SteamLibrary\steamapps\common\Teardown'

"=== 1. close the game and every injected build ==="
Get-Process teardown -ErrorAction SilentlyContinue | ForEach-Object {
    "killing pid " + $_.Id
    try { $_.Kill() } catch { "  kill failed: " + $_.Exception.Message; continue }
    # WaitForExit returns a bool, and letting it print leaves a stray "True" in
    # the transcript. Assign it, then check the process is really gone.
    if (-not $_.WaitForExit(30000)) { "  did not exit in 30s"; continue }
}
Start-Sleep -Seconds 3
if (Get-Process teardown -ErrorAction SilentlyContinue) { "still running"; exit 1 }
"game closed"

# The log files live in the game directory and the previous run may still be
# flushing one, so a rename can fail with a sharing violation. Retrying briefly
# beats aborting the whole test over a file that is about to be released.
#
# This block used to be a lie: the Move-Item failed, the script printed
# "archived" anyway, and the next test read the previous build's frame counters
# as if they were fresh. A failed archive must be loud, not a printed string.
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$archiveFailed = $false
Get-ChildItem $root -Filter 'teardown_vr*.log' | ForEach-Object {
    $dest = Join-Path $root ("_old_" + $stamp + "_" + $_.Name)
    $done = $false
    foreach ($attempt in 1..10) {
        try {
            Move-Item $_.FullName $dest -Force
            $done = $true
            break
        } catch {
            if ($attempt -eq 10) {
                "ARCHIVE FAILED: $($_.Name) is still locked by another process"
                $archiveFailed = $true
            } else { Start-Sleep -Milliseconds 500 }
        }
    }
    if ($done) { "archived " + $_.Name }
}
if ($archiveFailed) {
    "a log file is locked; the next run would read stale counters. Aborting."
    exit 1
}
if (Test-Path (Join-Path $root 'tdvr_host.txt')) {
    Remove-Item (Join-Path $root 'tdvr_host.txt') -Force
    "removed stale tdvr_host.txt"
}

"=== 2. start via the scheduled task (session 1, real desktop) ==="
Start-ScheduledTask -TaskName 'tdvr_go'
$deadline = (Get-Date).AddSeconds(120)
$seen = $null
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 4
    $p = Get-Process teardown -ErrorAction SilentlyContinue
    if ($p) {
        if (-not $seen) { "pid=" + $p.Id + " session=" + $p.SessionId; $seen = $p.Id }
        # wait until it is actually rendering, not just spawned
        if ($p.WorkingSet64 -gt 400MB) {
            "rendering now, WS=" + [math]::Round($p.WorkingSet64/1MB) + "MB"
            break
        }
    }
}
if (-not (Get-Process teardown -ErrorAction SilentlyContinue)) { "game did not start"; exit 1 }
Start-Sleep -Seconds 20
$p = Get-Process teardown -ErrorAction SilentlyContinue
"pid=" + $p.Id + " responding=" + $p.Responding +
    " WS=" + [math]::Round($p.WorkingSet64/1MB) + "MB cpu=" + [math]::Round($p.CPU,1) + "s"
