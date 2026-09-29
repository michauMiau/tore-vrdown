# Continuous background monitor for one Teardown run, writing a timestamped
# log the agent can read once instead of polling interactively.
#
# The three mistakes this exists to avoid, all of which produced false "the game
# is fine" readings:
#   1. Judging liveness from the process. After the crash the pid still exists
#      and Responding still returns True, because the process is frozen rather
#      than gone. A flat CPU counter is the opposite of a healthy one.
#   2. Enumerating windows from session 0, where the game is not visible at all,
#      so "no windows" looks identical to "the dialog is gone".
#   3. Capturing a screenshot minutes late, by which time the Sentry dialog has
#      closed itself.
#
# So this runs inside session 1 via the scheduled task, and on every tick it:
#   - re-reads the process, its CPU and its responding flag
#   - enumerates visible windows, looking for the Sentry crash dialog
#   - compares the screen against the previous tick, so a frozen game is
#     reported as FROZEN rather than as alive
#   - on the first tick where the game stops moving, immediately screenshots
#     twice and stops
param(
    [int]$IntervalSeconds = 10,
    [int]$MaxSeconds = 1800,
    [string]$LogPath = ''
)

$ErrorActionPreference = 'Continue'

if ($LogPath -eq '') {
    $LogPath = 'C:\tdvr\monitor_' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log'
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Collections.Generic;

public class MON {
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr h);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    public class I { public IntPtr H; public string T; public string C; public uint P; public bool V; }
    static string S(IntPtr h, int cap, bool cls) {
        var sb = new StringBuilder(cap);
        if (cls) GetClassNameW(h, sb, cap); else GetWindowTextW(h, sb, cap);
        return sb.ToString();
    }
    public static List<I> Top() {
        var l = new List<I>();
        EnumWindows((h, p) => {
            if (GetParent(h) != IntPtr.Zero) return true;
            var i = new I { H = h, T = S(h, 512, false), C = S(h, 256, true), V = IsWindowVisible(h) };
            GetWindowThreadProcessId(h, out i.P);
            l.Add(i); return true;
        }, IntPtr.Zero);
        return l;
    }
}
'@
Add-Type -TypeDefinition $sig -Language CSharp

$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen

function Log($m) {
    $line = (Get-Date -Format 'HH:mm:ss') + ' ' + $m
    Add-Content -Path $LogPath -Value $line
}

function GrabPixels {
    $bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
    $g.Dispose()
    $r = [System.Drawing.Rectangle]::new(0, 0, $bmp.Width, $bmp.Height)
    $d = $bmp.LockBits($r, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                       [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $len = $d.Stride * $bmp.Height
    $buf = [byte[]]::new($len)
    [System.Runtime.InteropServices.Marshal]::Copy($d.Scan0, $buf, 0, $len)
    $bmp.UnlockBits($d); $bmp.Dispose()
    return ,$buf
}

function Shoot($path) {
    $bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

function DiffPct($a, $b2) {
    $n = [math]::Min($a.Length, $b2.Length)
    $d = 0
    for ($i = 0; $i -lt $n; $i += 16) {   # every 4th pixel is plenty
        if ($a[$i] -ne $b2[$i]) { $d++ }
    }
    return [math]::Round(100.0 * $d / [int]($n / 16), 3)
}

Log "monitor start interval=${IntervalSeconds}s max=${MaxSeconds}s log=$LogPath"

$p = Get-Process teardown -ErrorAction SilentlyContinue
if (-not $p) {
    Log 'teardown NOT RUNNING at monitor start'
    exit 1
}
$pid0 = $p.Id
Log "watching pid=$pid0"

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$prev = GrabPixels
$tick = 0
$frozenCount = 0
$lastCpu = -1.0
$lastCpuChangeAt = 0.0

while ($sw.Elapsed.TotalSeconds -lt $MaxSeconds) {
    Start-Sleep -Seconds $IntervalSeconds
    $tick++
    $t = [math]::Round($sw.Elapsed.TotalSeconds, 0)

    $alive = Get-Process -Id $pid0 -ErrorAction SilentlyContinue
    if (-not $alive) {
        Log "t=${t}s PROCESS GONE"
        Shoot ('C:\tdvr\dead_' + (Get-Date -Format 'HHmmss') + '.png')
        break
    }

    $cur = GrabPixels
    $d = DiffPct $prev $cur
    $prev = $cur

    # A flat screen across several consecutive ticks is the freeze. One tick of
    # no change is not enough: a pause or a menu transition looks the same.
    if ($d -lt 0.05) { $frozenCount++ } else { $frozenCount = 0 }

    $sentry = $false
    $gameWin = $false
    foreach ($w in [MON]::Top()) {
        if (-not $w.V) { continue }
        if ($w.T -like '*crash dump*') { $sentry = $true }
        if ($w.P -eq $pid0 -and $w.T -ne '') { $gameWin = $true }
    }

    # CPU that has not moved at all is the second, independent freeze signal.
    $cpuNote = ''
    if ($lastCpu -ge 0 -and [math]::Abs($alive.CPU - $lastCpu) -lt 0.05) {
        if ($lastCpuChangeAt -eq 0) { $lastCpuChangeAt = $t }
        $cpuNote = " cpu FROZEN since t=${lastCpuChangeAt}s"
    } else {
        $lastCpu = $alive.CPU
        $lastCpuChangeAt = 0
    }

    Log ("t=${t}s screen_diff=${d}% game_window=$gameWin sentry=$sentry responding=$($alive.Responding) cpu=$([math]::Round($alive.CPU,1))$cpuNote")

    if ($sentry) {
        Log 'SENTRY DIALOG IS UP'
        Shoot ('C:\tdvr\sentry_' + (Get-Date -Format 'HHmmss') + '.png')
        Log 'screenshot taken while the dialog is up'
    }

    if ($frozenCount -ge 3) {
        Log "FROZEN for $frozenCount consecutive ticks - treating as dead"
        Shoot ('C:\tdvr\frozen_' + (Get-Date -Format 'HHmmss') + '.png')
        Log 'screenshot taken at the freeze'
        break
    }
}

Log 'monitor end'
