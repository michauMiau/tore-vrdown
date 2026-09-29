# One full test run: fresh game, inject, enter a level, read the log.
#
# This exists so a measurement does not depend on the user being at the
# computer. Every previous run needed someone to click "commence" by hand, and
# that is both a bottleneck and a source of noise: the click lands at an
# unknown moment relative to the injection, so "no frames" could mean "the level
# had not started yet" rather than "the frame hook is dead".
#
# The order is deliberate:
#
#   1. fresh_game.ps1  - close everything, archive logs, relaunch into session 1
#   2. wait for the game to be genuinely busy, not merely spawned
#   3. inject the build
#   4. run_input.ps1 - drive the menus from inside session 1
#   5. read the log and report what was actually reached
#
# Step 2 is the one that was missing and it is the one that caused every wrong
# conclusion so far. probe_is_rendering.ps1 samples the process CPU over a few
# seconds: a game sitting in a menu burns nothing, and reading "no frames" from
# a process that never rendered says nothing about the hook. So the run refuses
# to inject until the game is already drawing.

param(
    [string]$Build = 'teardown_vr39',
    [int]$Mode = 2          # 0 = Menu, 2 = Level
)

$ErrorActionPreference = 'Continue'
$root = 'C:\tdvr'
$game = 'D:\SteamLibrary\steamapps\common\Teardown'

function Step([string]$m) {
    Write-Output ""
    Write-Output ("##### " + $m)
}

Step "1/5  fresh game in session 1"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$root\fresh_game.ps1"

Step "2/5  wait until the game is actually drawing"
# A menu is idle. Only proceed once the process is burning CPU, because every
# frame-related measurement taken against an idle process is meaningless.
$busy = $false
$deadline = (Get-Date).AddSeconds(150)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $p = Get-Process teardown -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { continue }
    $c1 = $p.CPU
    Start-Sleep -Seconds 5
    $p.Refresh()
    $dc = $p.CPU - $c1
    Write-Output ("  cpu delta over 5 s: {0:N2} s   WS={1}MB" -f `
                  $dc, [math]::Round($p.WorkingSet64/1MB))
    if ($dc -gt 0.4) { $busy = $true; break }
}
if (-not $busy) {
    "GAME NOT RENDERING - stopping before injection."
    "  A measurement against an idle process cannot distinguish a dead frame"
    "  hook from a game that has not started drawing yet."
    exit 2
}
"  => the game is drawing"

Step "3/5  inject $Build"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$root\inject_run.ps1" $Build

Step "4/5  drive the menus from session 1"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$root\run_input.ps1" `
    -Mode $(if ($Mode -eq 0) { 'Menu' } else { 'Level' }) -WaitSeconds 60

Step "5/5  read the log"
$log = Join-Path $game ($Build + '.log')
if (-not (Test-Path $log)) { "NO LOG $log"; exit 1 }

$lines = Get-Content $log
Write-Output ("$log  :  $($lines.Count) lines")
""
Write-Output "--- what was reached ---"
$reach = @(
    @{ k = 'resolved via RTTI';   v = 'renderer located' },
    @{ k = 'hooks installed';     v = 'frame hooks live'  },
    @{ k = 'upload hook: installed'; v = 'uploader hooked' },
    @{ k = 'ready';               v = 'init finished'    },
    @{ k = 'scan frame';          v = 'beginRender entered' },
    @{ k = 'frame \d+ idx=';      v = 'endRender, per 120 frames' },
    @{ k = 'capture: entered';    v = 'uploader called'  },
    @{ k = 'detached';            v = 'DLL detached'     }
)
foreach ($r in $reach) {
    $n = ($lines | Select-String -Pattern $r.k).Count
    $mark = if ($n -gt 0) { "YES ($n)" } else { "no" }
    Write-Output ("  {0,-22} {1}" -f $r.v, $mark)
}

$frameN = $lines | Select-String -Pattern '^\[VR\] frame (\d+)' |
          ForEach-Object { [int]$_.Matches[0].Groups[1].Value }
if ($frameN) {
    Write-Output ""
    Write-Output ("  HIGHEST FRAME COUNT: {0}" -f (($frameN | Measure-Object -Maximum).Maximum))
    Write-Output "  => the frame hooks ARE being reached. Stereo work can proceed."
} else {
    Write-Output ""
    Write-Output "  NO frame N line (they are written every 120 frames)."
    $scan = $lines | Select-String -Pattern 'scan frame (\d+)' |
            ForEach-Object { [int]$_.Matches[0].Groups[1].Value }
    if ($scan) {
        Write-Output ("  beginRender was entered at least {0} times." -f `
                      (($scan | Measure-Object -Maximum).Maximum))
        Write-Output "  If the game is drawing, the frame hook is alive and simply"
        Write-Output "  has not reached 120 frames yet. Wait and re-read."
    } else {
        Write-Output "  beginRender was NEVER entered, even though the game is"
        Write-Output "  drawing. The vtable slots are not the per-frame path."
    }
}

Write-Output ""
Write-Output "--- last 12 lines ---"
$lines | Select-Object -Last 12

Write-Output ""
Write-Output "--- process ---"
$p = Get-Process teardown -ErrorAction SilentlyContinue | Select-Object -First 1
if ($p) {
    "ALIVE pid=$($p.Id) cpu=$([math]::Round($p.CPU,1))s responding=$($p.Responding)"
    exit 0
} else {
    "PROCESS GONE"
    exit 1
}
