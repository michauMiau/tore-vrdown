# Run send_input.ps1 INSIDE session 1.
#
# This is the part that makes the difference between working and silently doing
# nothing. SendInput delivers input to the foreground window of the calling
# process's session. This script is launched over SSH, which runs in session 0;
# the game's window lives in session 1. Input injected from session 0 does not
# reach it, so every attempt to drive the game from SSH has been a no-op.
#
# The fix is the same mechanism that already starts the game in session 1: a
# scheduled task with Interactive logon type, triggered on demand, with the
# same UserId. The task body runs send_input.ps1, which writes
# C:\tdvr\sendinput.log; this script then waits and prints that log.
#
# Interactive logon requires UserId as a SID, not DOMAIN\USER: the DOMAIN\USER
# form fails with 0x80070534 on this box. The SID is read from the currently
# logged-on interactive user rather than hardcoded.

param(
    [ValidateSet('Menu', 'Level')]
    [string]$Mode = 'Menu',
    [int]$HoldSeconds = 6,
    [int]$WaitSeconds = 90
)

$ErrorActionPreference = 'Stop'
$root = 'C:\tdvr'
$log  = Join-Path $root 'sendinput.log'
$ps1  = Join-Path $root 'send_input.ps1'
$task = 'tdvr_input'

if (-not (Test-Path $ps1)) { "missing $ps1"; exit 1 }

# --- who is logged on interactively (session 1) ---
#
# Process.UserId is the SECURITY_TOKEN_NOT_NULL-relative id of the process
# token, not the account SID, and it comes back empty here. Interactive logon
# tasks need the real SID (the DOMAIN\USER form fails with 0x80070534 here).
#
# Getting the SID without any P/Invoke.
#
# Win32_UserAccount has no row for this local account, so a Name+Domain lookup
# finds nothing even though GetOwner() returns VMWARE\vm correctly, and
# OpenProcessToken via Add-Type does not compile (the marshaller binds the out
# IntPtr as a return value, so "!" cannot be applied to System.IntPtr; with that
# worked around the next error is an unused catch variable promoted to an
# error). None of that is worth fighting.
#
# The tdvr_go task that already launches the game successfully uses exactly
# this: whoami /user, which prints the SID for the account the script runs as.
# The scheduled task runs as the same interactive user, so the SID is the same
# one, and reading it from a real command is more reliable than synthesising it.
$sidStr = (whoami /user | Select-String -Pattern 'S-1-[\d-]+' |
           Select-Object -First 1).Matches[0].Value
if (-not $sidStr) { "whoami /user returned no SID"; exit 1 }
"interactive SID: $sidStr"

# --- build the one-shot task ---
if (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $task -Confirm:$false
    "removed old task $task"
}

$arg = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden ' +
       "-File `"$ps1`" -Mode $Mode -HoldSeconds $HoldSeconds"
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arg
$principal = New-ScheduledTaskPrincipal -UserId $sidStr -LogonType Interactive `
           -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries `
           -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 5)

Register-ScheduledTask -TaskName $task -Action $action -Principal $principal `
                       -Settings $settings -Force | Out-Null
"registered $task (interactive, session 1)"

# --- clear the previous run's log so the output cannot be mistaken for new ---
if (Test-Path $log) { Remove-Item $log -Force; "cleared old sendinput.log" }

Start-ScheduledTask -TaskName $task
"started $task"

# --- wait for it to write DONE, or time out ---
$deadline = (Get-Date).AddSeconds($WaitSeconds)
$done = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    if (Test-Path $log) {
        $c = Get-Content $log -ErrorAction SilentlyContinue
        if ($c -match 'DONE') { $done = $true; break }
    }
    $t = Get-ScheduledTask -TaskName $task
    if ($t.State -ne 'Running') { Start-Sleep -Seconds 2; break }
}

Start-Sleep -Seconds 2
""
"=== sendinput.log ==="
if (Test-Path $log) {
    Get-Content $log
} else {
    "NO LOG - the task never wrote anything."
    "task state: " + (Get-ScheduledTask -TaskName $task).State
    "last result: " + (Get-ScheduledTaskInfo -TaskName $task).LastTaskResult
    exit 1
}
""
if ($done) { "OK"; exit 0 }
"INCOMPLETE (no DONE marker)"
exit 1
