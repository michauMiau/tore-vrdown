# Liveness watch. Reads CPU, never a crash dialog.
#
# Why this file exists: on 2026-09-30 the owner's game froze and the agent did
# not know. The trap was checking the wrong thing. Sentry/sentry-native is NOT
# a crash signal -- its event files, session.json and the "send crash dump?"
# window all carry the process start time to the second and read level=fatal
# while the game renders at full speed. build/game_state.ps1 trusted Sentry and
# reported CRASHED for a live game twice, which invented a whole wrong suspect.
#
# So: two independent signals, both cheap, neither Sentry.
#   1. CPU delta over a sample. A frozen render thread stops burning cycles.
#   2. Whether the instrumentation log is still growing.
#
# A frozen game and a dead game look identical to a one-shot check, so this
# samples over time and keeps a rolling history. It also records window
# enumeration, because "frozen" often means a modal dialog is holding the
# message loop -- which is exactly the Sentry case, and it is only a crash if
# the CPU is also flat.
#
#   .\watchdog.ps1 [interval-seconds] [max-samples]
#
# Writes C:\tdvr\watchdog.log. Exits on its own after max-samples.

param([int]$Interval = 20, [int]$MaxSamples = 270)   # 20s * 270 = 90 min

$ErrorActionPreference = 'SilentlyContinue'
$log = 'C:\tdvr\watchdog.log'
$gd  = 'D:\SteamLibrary\steamapps\common\Teardown'

function W($s) {
    $line = (Get-Date -Format 'HH:mm:ss') + '  ' + $s
    Add-Content $log -Value $line
    Write-Host $line
}

W "=== watchdog start: every ${Interval}s, $MaxSamples samples, ~$([int]($Interval*$MaxSamples/60)) min ==="

# Window enumeration, so a frozen-behind-a-dialog case is distinguishable
# from a frozen-render-thread case. The Sentry window is one of these.
Add-Type -Namespace TDW -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
public delegate bool EnumProc(IntPtr h, IntPtr l);
'@

function Get-GameWindows {
    $list = New-Object System.Collections.ArrayList
    $cb = [TDW.U+EnumProc] {
        param($h, $l)
        $sb = New-Object System.Text.StringBuilder 512
        [void][TDW.U]::GetWindowText($h, $sb, 512)
        $t = $sb.ToString()
        if ($t -and [TDW.U]::IsWindowVisible($h)) {
            $p = 0
            [void][TDW.U]::GetWindowThreadProcessId($h, [ref]$p)
            if ($p -gt 0) { [void]$list.Add(("pid=$p '" + $t + "'")) }
        }
        return $true
    }
    [void][TDW.U]::EnumWindows($cb, [IntPtr]::Zero)
    return $list
}

$prevCpu   = @{}     # pid -> last CPU reading
$prevLogSz = @{}     # pid -> last size of its vr_*.log
$flatRuns  = @{}     # pid -> consecutive samples with no CPU movement
$reported  = @{}     # pid -> state we already announced

for ($i = 1; $i -le $MaxSamples; $i++) {
    $ps = @(Get-Process Teardown)
    if ($ps.Count -eq 0) {
        $hadGame = ($prevCpu.Count -gt 0)
        if ($hadGame -and -not $reported['-gone']) {
            W "*** GAME PROCESS GONE (had been running) ***"
            $reported['-gone'] = $true
        }
        if ($i -eq 1) { W "no teardown.exe yet; waiting" }
        Start-Sleep -Seconds $Interval
        continue
    }

    $report = @()
    foreach ($p in $ps) {
        $id = $p.Id
        # A DLL of mine still in the process is the difference between a frozen
        # game and a frozen game I broke, so it goes in every line.
        $own = @((Get-Process -Id $id).Modules | Where-Object { $_.ModuleName -like 'vr_*.dll' })
        $ownStr = if ($own.Count -gt 0) { ($own.ModuleName) -join ',' } else { '-' }

        $vrlog = @(Get-ChildItem $gd -Filter 'vr_*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 1)
        $logSz = if ($vrlog.Count -gt 0) { $vrlog[0].Length } else { 0 }
        $logNm = if ($vrlog.Count -gt 0) { $vrlog[0].Name } else { '-' }

        $first = -not $prevCpu.ContainsKey($id)
        $d = 0
        if (-not $first) { $d = [math]::Round($p.CPU - $prevCpu[$id], 3) }
        $prevCpu[$id] = $p.CPU

        $grew = 0
        if ($prevLogSz.ContainsKey($id)) { $grew = $logSz - $prevLogSz[$id] }
        $prevLogSz[$id] = $logSz

        if (-not $first -and $d -le 0.05) {
            $flatRuns[$id] = 1 + $(if ($flatRuns.ContainsKey($id)) { $flatRuns[$id] } else { 0 })
        } else {
            $flatRuns[$id] = 0
        }

        # Alive needs BOTH signals moving. A growing log with a live CPU is the
        # only thing this project will call healthy.
        $state = 'ALIVE'
        if (-not $first) {
            # A verdict needs a delta. The first sample has none by
            # construction, and calling it FROZEN produced a false alarm on
            # every single start -- caught live at 01:17:30 on pid 12816, which
            # was rendering at 2.6s CPU per 20s sample twenty seconds later.
            if ($flatRuns[$id] -ge 2) { $state = 'FLAT' }
            if (-not $p.Responding)     { $state = 'NOT_RESPONDING' }
            if ($d -le 0.05 -and $grew -le 0) { $state = 'FROZEN' }
        } else { $state = 'SAMPLING' }

        # Announce each state once, not every sample, so the log stays readable.
        if ($reported[$id] -ne $state) {
            $report += ("  *** pid=$id STATE $state *** cpuDelta=${d}s logGrew=${grew}B own=$ownStr")
            $reported[$id] = $state
        }

        $report += ("  pid=$id $state cpuDelta=${d}s ws=$([math]::Round($p.WorkingSet64/1MB))MB " +
                    "resp=$($p.Responding) log=$logNm/$logSzB own=$ownStr flat=$($flatRuns[$id])")
    }

    foreach ($r in $report) { W $r }
    Start-Sleep -Seconds $Interval
}

W "=== watchdog finished: $MaxSamples samples ==="
