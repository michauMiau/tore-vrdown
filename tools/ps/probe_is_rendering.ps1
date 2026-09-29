# Is the game rendering at all, or sitting in a menu?
#
# Why: v36..v38 all reported "call=1" on 0x5BAEB0 and hooked_begin_render ran
# once. If the game never renders a frame, then the whole finding "this uploader
# is not per-frame" is really "the game is not drawing", and every conclusion
# drawn from those builds is about a menu, not about rendering.
#
# This does three independent things, none of which touch the game's memory:
#
#   1. Reads the mod's own log and reports how many times beginRender ran
#      (the log's "frame N" lines come from hooked_end_render, so instead it
#      counts the "scan frame" lines and the last line written).
#   2. Samples the process CPU time twice, a few seconds apart. A game drawing
#      at 60 FPS burns CPU continuously; a menu is nearly idle.
#   3. Checks the window title and whether the process is responding, which
#      distinguishes "running but paused/menu" from "hung".
#
# A CPU delta near zero across the sample window is the signal that nothing is
# being rendered, which is the thing to fix before hunting uploaders again.

param([int]$SampleSeconds = 8, [string]$LogTag = "")

$ErrorActionPreference = 'Stop'

$pr = Get-Process teardown -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $pr) { Write-Output "NO_PROCESS"; exit 1 }

Write-Output ("pid={0}  threads={1}  responding={2}" -f $pr.Id, $pr.Threads.Count, $pr.Responding)
Write-Output ("title='{0}'" -f $pr.MainWindowTitle)

# --- 1. the mod's own view ---
if ($LogTag) {
    $log = "D:\SteamLibrary\steamapps\common\Teardown\teardown_vr$LogTag.log"
    if (Test-Path $log) {
        $lines = Get-Content $log
        Write-Output ""
        Write-Output "=== log $LogTag : $($lines.Count) lines, last written $((Get-Item $log).LastWriteTime) ==="

        $scan  = ($lines | Select-String -Pattern 'scan frame').Count
        $fnum  = ($lines | Select-String -Pattern '^\[VR\] frame \d').Count
        $cap   = ($lines | Select-String -Pattern 'capture: entered').Count
        Write-Output "  'scan frame' lines : $scan"
        Write-Output "  'frame N' lines    : $fnum   (one every 120 frames)"
        Write-Output "  'capture: entered' : $cap"

        $m = $lines | Select-String -Pattern 'scan frame (\d+)' |
             ForEach-Object { [int]$_.Matches[0].Groups[1].Value }
        if ($m) { Write-Output "  highest scan frame : $(($m | Measure-Object -Maximum).Maximum)" }

        $f = $lines | Select-String -Pattern '^\[VR\] frame (\d+)' |
             ForEach-Object { [int]$_.Matches[0].Groups[1].Value }
        if ($f) { Write-Output "  highest frame N    : $(($f | Measure-Object -Maximum).Maximum)" }

        Write-Output "  --- last 3 lines ---"
        $lines | Select-Object -Last 3 | ForEach-Object { Write-Output "    $_" }
    } else {
        Write-Output "NO_LOG $log"
    }
}

# --- 2. is it burning CPU right now ---
Write-Output ""
Write-Output "=== CPU sample over $SampleSeconds s ==="
$c1 = $pr.CPU
$t1 = Get-Date
Start-Sleep -Seconds $SampleSeconds
$pr.Refresh()
$c2 = $pr.CPU
$t2 = Get-Date
$dt = ($t2 - $t1).TotalSeconds
$dc = $c2 - $c1

$ws1 = $pr.WorkingSet64
Start-Sleep -Seconds 2
$pr.Refresh()
$ws2 = $pr.WorkingSet64
$dwsmb = [math]::Round((($ws2 - $ws1) / 1MB), 1)

Write-Output ("  cpu {0:N2} s -> {1:N2} s   delta {2:N2} s over {3:N1} s" -f $c1, $c2, $dc, $dt)
Write-Output ("  working set {0:N0} MB -> {1:N0} MB   (change {2} MB in 2 s)" -f ($ws1/1MB), ($ws2/1MB), $dwsmb)
Write-Output ("  threads {0}" -f $pr.Threads.Count)

# --- 3. verdict ---
$pct = if ($dt -gt 0) { 100.0 * $dc / $dt } else { 0 }
Write-Output ""
Write-Output "=== verdict ==="
Write-Output ("  CPU utilisation during the sample: {0:N1} %" -f $pct)
if ($pct -lt 5) {
    Write-Output "  => NOT RENDERING. The process is alive but drawing nothing."
    Write-Output "     A game rendering at 60 FPS on this box would show tens of"
    Write-Output "     percent of one core. Below 5% means the game is in a menu,"
    Write-Output "     a loading screen, or otherwise not presenting frames."
    Write-Output ""
    Write-Output "     Everything measured in v36..v38 describes a non-rendering"
    Write-Output "     process. The '0x5BAEB0 is called once' result is consistent"
    Write-Output "     with startup init in a menu and says nothing about the"
    Write-Output "     per-frame upload path."
} else {
    Write-Output "  => The process is working. Frames may be being drawn."
    Write-Output "     If the mod still logs no 'frame N', the frame hooks are"
    Write-Output "     not being reached even though the game is busy."
}
Write-Output "OK"
