# Register + run a script in session 1, the session the game actually runs in.
# Generic wrapper: -Script C:\tdvr\foo.ps1 [-ScriptArgs 'a','b']
param(
    [string]$Script,
    [string[]]$ScriptArgs = @(),
    [int]$TaskName = 0,
    [int]$WaitSeconds = 0,
    [string]$Report = ''
)

$ErrorActionPreference = 'Stop'

if ($TaskName -eq 0) { $TaskName = 1 + [int](Get-Random -Minimum 0 -Maximum 99999) }
$name = "tdvr_run$TaskName"

# A scheduled task started this way has no console to print to and nothing
# captures its stdout, so without redirecting to a file the script runs to
# completion and every line of its output is lost. That is how an earlier
# version of this wrapper looked like it had produced nothing at all.
if ($Report -eq '') {
    $Report = 'C:\tdvr\report_' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '_' + $name + '.txt'
}
$inner = $Script
$argList = '-NoProfile -ExecutionPolicy Bypass -Command ' +
           ('"& { & ''' + $inner + ''' ' +
            (($ScriptArgs | ForEach-Object { "'''" + $_ + "'''" }) -join ' ') +
            ' } *> ''' + $Report + '''')

$a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $argList
# 'vm' is the local account; a display name fails with 0x80070534.
$p = New-ScheduledTaskPrincipal -UserId 'vm' -LogonType Interactive -RunLevel Highest

Register-ScheduledTask -TaskName $name -Action $a -Principal $p -Force | Out-Null
Start-ScheduledTask -TaskName $name

if ($WaitSeconds -le 0) { $WaitSeconds = 40 }
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $WaitSeconds) {
    Start-Sleep -Seconds 3
    $st = Get-ScheduledTask -TaskName $name
    if ($st.State -ne 'Running') { break }
}

$info = Get-ScheduledTaskInfo -TaskName $name
"LastTaskResult = " + $info.LastTaskResult
if (Test-Path $Report) {
    '--- report: ' + $Report + ' ---'
    Get-Content $Report
} else {
    'NO REPORT FILE: the task did not produce output. Event log:'
    Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-TaskScheduler/Operational' } -MaxEvents 20 -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -like "*$name*" } | Select-Object -First 3 |
        ForEach-Object { 'event ' + $_.Id + ': ' + $_.Message }
}
Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
