# Run one full test cycle and report the crash-detector verdict, not a
# hand-picked number.
#
# Every earlier "the game is fine" claim in this project was made by reading
# Responding=True and a missing .dmp. This wrapper exists so the verdict comes
# from one place, with the numbers that back it, every time.
param(
    [string]$Label = 'run',
    [switch]$NoInject,
    [switch]$NoRestart,
    [int]$WaitSec = 45,
    [int]$DxgiTouch = -1,
    [string]$Flags = ''
)

function Say($s) { Write-Host $s }

Say "=== $Label ==="

# The flag file the DLL reads (tdvr_flags.txt, next to the DLL in the game
# folder). Writing it here, per run, is the only way the DLL sees it: the game is
# started by a scheduled task whose environment comes from the service, so a
# user-level environment variable never reaches it -- verified, the log kept
# saying TDVR_DXGI_TOUCH=0 after the variable was set and broadcast.
#
# Format: space-separated tokens. "sc" enables the swapchain capture hook, "1"
# makes the DXGI probe actually call DXGI, "0" makes it read-only.
# WHERE THE FLAGS FILE IS -- and the name must not be $flags.
#
# PowerShell variable names are case-INSENSITIVE, so a param block with [string]$Flags
# and a local $flagsPath is fine, but `$flags = 'D:\...'` is not: it is the SAME
# variable as $Flags, and it overwrote the parameter with the path. Every later
# `$Flags` then read the path back, and WriteAllText dutifully wrote the file's
# own location into the file. The read-back check passed, because the read-back
# is of what was just written -- it confirmed the write, never the intent.
# Symptom to recognise: the log says the flags are ON, and nothing changes.
$flagsPath = 'D:\SteamLibrary\steamapps\common\Teardown\tdvr_flags.txt'

# Write the flags file with .NET directly, not Set-Content, and read it back.
#
# Tokens: space- or comma-separated. "sc" = swapchain hook, "1" = DXGI probe
# actually calls DXGI, "0" = probe is read-only.
if ($Flags -ne '') {
    $norm = ($Flags -replace '[\s,]+', ',').Trim(',')
    [IO.File]::WriteAllText($flagsPath, $norm, [Text.Encoding]::ASCII)
    $read = [IO.File]::ReadAllText($flagsPath)
    Say "  tdvr_flags.txt = '$read'  ($($read.Length) B, verified by read-back)"
    if ($read -ne $norm) { Say '  *** flags file did not read back as written ***' }
    # Also check the tokens, so "the file was written" cannot be mistaken for
    # "the file says what we asked for".
    if ($norm -match '(?i)(^|,)sc(,|$)') { Say '    token "sc" present' }
    else { Say '    *** token "sc" NOT in the file ***' }
}
if ($DxgiTouch -ge 0 -and $Flags -eq '') {
    [IO.File]::WriteAllText($flagsPath, [string]$DxgiTouch, [Text.Encoding]::ASCII)
    Say "  tdvr_flags.txt = '$([IO.File]::ReadAllText($flagsPath))'  (verified by read-back)"
}
if (-not $NoRestart) {
    Get-Process teardown -EA SilentlyContinue | ForEach-Object {
        Say "  killing pid=$($_.Id)"; $_.Kill()
    }
    Start-Sleep -Seconds 3
    # The old DLL stays locked in the game's mapped image until the process is
    # gone, so Copy-Item fails with "used by another process" and the injector
    # then reports FAILED. Wait for the lock to clear before copying.
    for ($i = 0; $i -lt 25; $i++) {
        if (-not (Get-Process teardown -EA SilentlyContinue)) { break }
        Start-Sleep -Seconds 1
    }
    if (Get-Process teardown -EA SilentlyContinue) {
        Say '  a teardown process is still alive; not restarting'
        exit 1
    }
    Say '  previous process gone, DLL lock released'
    Start-ScheduledTask -TaskName 'tdvr_go'
    $p = $null
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Seconds 2
        $p = Get-Process teardown -EA SilentlyContinue
        if ($p) { break }
    }
    if (-not $p) { Say '  game did not start'; exit 1 }
    Say "  started pid=$($p.Id) at $($p.StartTime.ToString('HH:mm:ss'))"
    Start-Sleep -Seconds 10
}

if (-not $NoInject) {
    $p = Get-Process teardown -EA SilentlyContinue
    if (-not $p) { Say '  no process to inject into'; exit 1 }
    Copy-Item 'C:\tdvr\vr_bisectA.dll' 'D:\SteamLibrary\steamapps\common\Teardown\vr_bisectA.dll' -Force
    if (Test-Path 'C:\tdvr\tdvr_host.txt') {
        Copy-Item 'C:\tdvr\tdvr_host.txt' 'D:\SteamLibrary\steamapps\common\Teardown\tdvr_host.txt' -Force
    }
    $inj = 'C:\tdvr\injector.exe'
    if (-not (Test-Path $inj)) { $inj = 'D:\SteamLibrary\steamapps\common\Teardown\injector.exe' }
    $out = & $inj --attach --dll 'D:\SteamLibrary\steamapps\common\Teardown\vr_bisectA.dll' --noquit 2>&1
    Say "  inject: $(($out | Select-Object -Last 1))"
    Start-Sleep -Seconds 5
}

# Let it run, then judge. A verdict taken 5 s after injection proves nothing --
# the freeze in this project appeared about 50 s in.
if ($WaitSec -gt 0) { Say "  soaking ${WaitSec}s ..."; Start-Sleep -Seconds $WaitSec }

Remove-Item 'C:\tdvr\crashdet.json' -Force -EA SilentlyContinue
Start-ScheduledTask -TaskName 'tdvr_crashdet'
$dl = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $dl) {
    if (Test-Path 'C:\tdvr\crashdet.json') { break }
    Start-Sleep -Milliseconds 500
}
if (-not (Test-Path 'C:\tdvr\crashdet.json')) { Say '  detector produced nothing'; exit 1 }

$j = Get-Content 'C:\tdvr\crashdet.json' -Raw | ConvertFrom-Json
Say ''
Say ("VERDICT : {0}" -f $j.verdict)
Say ("reason  : {0}" -f $j.reason)
Say ("pid     : {0}  session {1}  threads {2}  ws {3}MB" -f $j.pid, $j.sessionId, $j.threadCount, $j.wsMB)
Say ("cpu     : {0}s / 5s   busy threads {1}" -f $j.cpuGrow, $j.busyThreads)
if ($j.gameWindow) {
    Say ("window  : {0} enabled={1} WM_NULL answered in {2}ms" -f `
        $j.gameWindow.handle, $j.gameWindow.enabled, $j.gameWindow.answerMs)
} else { Say 'window  : none identified' }
Say ("dialogs : {0}" -f $(if ($j.dialogs) { $j.dialogs.Count } else { 0 }))
Say ("log     : grew={0} bytes={1}" -f $j.logGrew, $j.logBytes)
$j.waitReasons.PSObject.Properties | Sort-Object Value -Descending | ForEach-Object {
    Say ("  wait {0,-24} {1}" -f $_.Name, $_.Value)
}

$log = 'D:\SteamLibrary\steamapps\common\Teardown\vr_bisectA.log'
if (Test-Path $log) {
    $sc = @(Get-Content $log | Where-Object { $_ -match '\[SC\]' })
    if ($sc.Count) {
        Say ''
        Say "--- [SC] lines: $($sc.Count) ---"
        $sc | Select-Object -Last 40 | ForEach-Object { Say ('  ' + $_.TrimEnd()) }
    }
    Say ''
    Say '--- log tail 8 ---'
    Get-Content $log -Tail 8 | ForEach-Object { Say ('  ' + $_.TrimEnd()) }
}
if ($j.verdict -eq 'WORKING') { exit 0 } else { exit 1 }
