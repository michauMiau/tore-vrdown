# Read the simulator's flicker_status.json and say WHETHER it belongs to the
# process currently running.
#
# The file has a "pid" field, and a stale file from a previous run is worse than
# no file at all: it reports a plausible frame counter and a plausible submission
# count for a process that is long gone, which reads exactly like the XR frame
# loop working. In this project that mistake cost a full day -- a
# "projectionSubmissions=46" figure was read off a dead run.
#
# So: compare the pid in the file against the live teardown pid, and report
# STALE explicitly rather than passing the numbers through.
$f = Join-Path $env:LOCALAPPDATA 'OpenXR-Simulator\flicker_status.json'
$p = Get-Process teardown -EA SilentlyContinue

Write-Host '--- live process ---'
if ($p) {
    Write-Host ("  teardown pid={0} started {1} session {2}" -f `
        $p.Id, $p.StartTime.ToString('HH:mm:ss'), $p.SessionId)
} else {
    Write-Host '  teardown is NOT running'
}

Write-Host ''
Write-Host '--- flicker_status.json ---'
if (-not (Test-Path $f)) { Write-Host "  no file at $f"; exit 0 }
$fi = Get-Item $f
Write-Host ("  written {0}  ({1} B)" -f $fi.LastWriteTime, $fi.Length)
try {
    $j = Get-Content $f -Raw | ConvertFrom-Json
} catch {
    Write-Host ("  unreadable: {0}" -f $_.Exception.Message); exit 0
}
$j.PSObject.Properties | ForEach-Object {
    Write-Host ("  {0,-22} = {1}" -f $_.Name, $_.Value)
}

Write-Host ''
if ($p) {
    if ($j.pid -eq $p.Id) {
        Write-Host ("  MATCHES the live game (pid {0}) -- these numbers are current" -f $p.Id)
        exit 0
    } else {
        Write-Host ("  *** STALE *** the file is from pid {0}, the game is pid {1}." -f $j.pid, $p.Id)
        Write-Host '      Every number above belongs to a dead run. Ignore it.'
        exit 1
    }
} else {
    Write-Host '  no game process to compare against.'
    exit 1
}
