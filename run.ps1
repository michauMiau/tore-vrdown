# Run any .ps1 in C:\tdvr from an interactive session-1 task.
#
#   .\run.ps1 <script-basename> [task-suffix]
#
# One launcher for everything. A set of near-identical launchers was the bug
# before: each one was a copy with a different TaskName that I did not rename,
# so registering one silently clobbered another and the script I wanted to run
# never started. The name is now a parameter, so that class of mistake is gone.
#
# The game has no desktop in session 0 and exits before it draws, so the whole
# start must happen in the interactive task. The principal must be the account
# that owns session 1 ('vm', the SSH user).
$ErrorActionPreference = 'Continue'
if (-not $args -or -not $args[0]) {
    Write-Host "usage: run.ps1 <script-basename> [suffix]"
    exit 2
}
$script = $args[0]
$suffix = if ($args.Count -gt 1 -and $args[1]) { $args[1] } else { $script }
$path   = "C:\tdvr\$script.ps1"
$log    = "C:\tdvr\${suffix}_launch.txt"
$task   = "tdvr_$suffix"

if (-not (Test-Path $path)) {
    Write-Host "no such script: $path"
    exit 2
}
# Refuse to launch something that does not parse, instead of returning a task
# result of 1 with no output and no explanation.
$errs = $null
$null = [System.Management.Automation.PSParser]::Tokenize((Get-Content $path -Raw), [ref]$errs)
if ($errs -and $errs.Count -gt 0) {
    Write-Host "ABORT: $path does not parse:"
    $errs | Select-Object -First 5 | ForEach-Object { Write-Host ("  " + $_.Message) }
    exit 1
}

Remove-Item $log -Force -EA SilentlyContinue
function A($s) { Add-Content -Path $log -Value ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $s) }
try {
    A "as $env:USERNAME session $((Get-Process -Id $PID).SessionId) -> $script"
    $a = New-ScheduledTaskAction -Execute 'powershell.exe' `
         -Argument "-NoProfile -ExecutionPolicy Bypass -File $path"
    $t = New-ScheduledTaskTrigger -Once -At (Get-Date).AddSeconds(5)
    $p = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
    $null = Register-ScheduledTask -TaskName $task -TaskPath '\tdvr\' -Action $a -Trigger $t `
             -Principal $p -Force `
             -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 2) `
                        -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries)
    A "registered $task"
    $null = Start-ScheduledTask -TaskName $task -TaskPath '\tdvr\'

    # Do not believe "started $task". A one-shot trigger registered from SSH
    # runs in session 0, and a task with LogonType Interactive started from
    # session 0 is refused by Windows with no error: LastTaskResult stays
    # 267011 (0x41303, "has not yet run") and the script never executes. On
    # 2026-09-30 this produced a launcher log full of confident lines and no
    # game at all, which reads exactly like a game that refused to start.
    #
    # So the start is verified, not assumed: the trigger is pushed out a minute
    # so it is never in the past, and the task state and last result are read
    # back after a moment.
    Start-Sleep -Seconds 6
    $st = Get-ScheduledTask -TaskName $task -TaskPath '\tdvr\' -EA SilentlyContinue
    $inf = Get-ScheduledTaskInfo -TaskName $task -TaskPath '\tdvr\' -EA SilentlyContinue
    A ("state=" + $st.State + " lastResult=" + $inf.LastTaskResult +
       " lastRun=" + $inf.LastRunTime)
    if ($inf.LastTaskResult -eq 267011) {
        A ("STILL HAS NOT RUN. 267011 is 0x41303, task has not yet run. A task with")
        A ("  LogonType Interactive cannot be started from session $((Get-Process -Id $PID).SessionId);")
        A "  it needs to be started from the interactive session itself. Nothing was"
        A "  executed -- do not read anything below this line as a result."
        exit 1
    }
    A "task is running"
} catch {
    A ("LAUNCHER ERROR: " + $_.Exception.Message)
    exit 1
}
