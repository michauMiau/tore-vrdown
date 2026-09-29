# game_state.ps1 — the single source of truth for "is the game actually alive?"
#
# WHY THIS EXISTS: `Get-Process` + Responding is NOT a liveness check on this
# game. Measured: after a crash Get-Process still returns the pid, Responding
# stays True, and the CPU counter just FREEZES. Judging liveness by a flat CPU
# counter is exactly backwards — a rendering game burns CPU, a dead one stops.
# Meanwhile the user sees a frozen level, a Sentry dialog, and a cmd window.
#
# So liveness is decided by four independent signals, and the verdict is only
# "ALIVE" when the window is present AND visible AND the CPU is MOVING:
#
#   1. process exists
#   2. a top-level window exists for that pid
#   3. that window is visible and not minimized  (IsWindowVisible)
#   4. CPU advanced between two samples        <- the real liveness signal
#   5. the Sentry crash dialog is not up       (this is the crash tell)
#
# Runs in session 1, because SSH is session 0 and enumerates zero windows.
param(
    [int]$SampleSeconds = 4,
    [string]$Report = ''
)

$ErrorActionPreference = 'SilentlyContinue'

$GameDir = 'D:\SteamLibrary\steamapps\common\Teardown'

if ($Report -eq '') {
    $Report = 'C:\tdvr\gamestate.txt'
}

$out = @()
function say($s) { $script:out += $s; Write-Output $s }

$p1 = Get-Process teardown | Select-Object -First 1
if (-not $p1) {
    say 'VERDICT: NOT RUNNING (no teardown process)'
    $out | Set-Content $Report
    exit 0
}

$pid1 = $p1.Id
$cpu1 = $p1.CPU
$ws1 = [math]::Round($p1.WorkingSet64 / 1MB)
say "pid=$pid1  cpu1=$cpu1  ws=${ws1}MB  responding=$($p1.Responding)"

# --- window enumeration via P/Invoke; Process.MainWindowHandle is unreliable
#     for a game that owns a swapchain across more than one window.
$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  public static string Title(IntPtr h) {
    int n = GetWindowTextLength(h);
    if (n == 0) return "";
    StringBuilder sb = new StringBuilder(n + 2);
    GetWindowText(h, sb, sb.Capacity);
    return sb.ToString();
  }
}
'@
Add-Type -TypeDefinition $sig -ErrorAction SilentlyContinue

$found = New-Object System.Collections.ArrayList
# These MUST be $script: scoped. A delegate callback runs in its own scope, so
# a plain $found inside it is a DIFFERENT (empty) variable -- which is exactly
# how the first run of this script printed a VISIBLE Teardown window and then
# reported "NO WINDOW". A self-contradicting verdict is the tell that a
# collection was not actually shared with the callback.
$script:found = $found
$cb = [W+EnumProc]{
    param($h, $l)
    $wpid = 0
    [W]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
    if ($wpid -eq $script:pid1) {
        $t = [W]::Title($h)
        $vis = [W]::IsWindowVisible($h)
        $ico = [W]::IsIconic($h)
        [void]$script:found.Add([pscustomobject]@{
            h = $h; title = $t; visible = $vis; minimized = $ico
        })
    }
    return $true
}
[W]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null

$sent = New-Object System.Collections.ArrayList
$script:sent = $sent

if ($found.Count -eq 0) {
    say 'windows: NONE for this pid'
} else {
    foreach ($w in $found) {
        $state = if (-not $w.visible) { 'HIDDEN' } elseif ($w.minimized) { 'MINIMIZED' } else { 'VISIBLE' }
        $ttl = $w.title
        if ($ttl.Length -gt 60) { $ttl = $ttl.Substring(0, 60) }
        say ("window 0x{0:X}  {1,-10} title='{2}'" -f [int64]$w.h, $state, $ttl)
    }
}

$sent = New-Object System.Collections.ArrayList
$script:sent = $sent
$cb2 = [W+EnumProc]{
    param($h, $l)
    if ([W]::IsWindowVisible($h)) {
        $t = [W]::Title($h)
        if ($t -match 'Sentry|crash dump|developers|crashpad') {
            [void]$script:sent.Add($t)
        }
    }
    return $true
}
[W]::EnumWindows($cb2, [IntPtr]::Zero) | Out-Null
if ($sent.Count -eq 0) {
    say 'sentry dialog: not present'
} else {
    foreach ($s in $sent) { say "sentry dialog: '$s'" }
}

# --- the real liveness signal: does CPU move?
Start-Sleep -Seconds $SampleSeconds
$p2 = Get-Process -Id $pid1
if (-not $p2) {
    say "VERDICT: DEAD (process gone after ${SampleSeconds}s)"
    $out | Set-Content $Report
    exit 0
}
$cpu2 = $p2.CPU
$delta = $cpu2 - $cpu1
$ws2 = [math]::Round($p2.WorkingSet64 / 1MB)
say "cpu2=$cpu2  delta=$([math]::Round($delta,2))s over ${SampleSeconds}s  ws=${ws2}MB"

$hasWindow = ($found | Where-Object { $_.visible -and -not $_.minimized }).Count -gt 0
$hasSentry = $sent.Count -gt 0
$cpuMoving = ($delta -gt 0.05)

# Sentry is NOT a crash tell on its own, and this script got that wrong twice.
# sentry-native writes the event envelope when the SESSION starts -- measured:
# __sentry-event, session.json, settings.dat and both breadcrumbs all carry the
# process start time to the second, and the envelope already reads
# "level=fatal" while the game is rendering at full speed. The Explorer's
# "send the crash dump?" dialog then survives the dead process and stays up
# indefinitely. So neither the file's timestamp nor the window's existence says
# anything about THIS run.
#
# The real tell is the same one the verdict already had: does CPU advance.
say "sentry: reported for reference only -- its envelope is written at session"
say "        start, so it can NOT distinguish a live game from a dead one."
$hasSentry = $false

if (-not $cpuMoving) {
    say 'VERDICT: CRASHED or FROZEN (CPU has not advanced over ' + $SampleSeconds + 's)'
} elseif ($cpuMoving -and $hasWindow) {
    say 'VERDICT: ALIVE (window visible, CPU advancing)'
} elseif (-not $hasWindow) {
    say 'VERDICT: NO WINDOW (process burns CPU but nothing visible)'
} else {
    say 'VERDICT: UNKNOWN'
}

$out | Set-Content $Report
