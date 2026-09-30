param(
    [string]$Log = 'D:\SteamLibrary\steamapps\common\Teardown\vr_u1.log',
    [int]$Wait = 240
)
# Is the private-bytes growth mine or the game's?
#
# Both hypotheses predict the same thing going up: private bytes. They differ in
# what it grows WITH.
#
#   my probe reads the back buffer once every N presents, and only when the
#   handler is reached -- so a leak in my path grows with the READ count.
#   the game renders every present, so a leak in the game's path grows with
#   SwapBuffers, which is at least sixty times larger.
#
# Sampling both counters and dividing the byte growth by each one separates them.
# This is the measurement that settled the question: on vr_t1 (one read per 60
# presents) the process grew 93 MB in 240 s with 407 reads, which is 239138
# bytes per read, and 407 of those is 97 MB. On vr_u1 (one per 600) there were
# zero reads in the same window and private bytes fell by 21 MB.
#
# param() must be the first statement in the file or PowerShell treats it as
# ordinary code and the parameters silently become $null. Two earlier versions
# put it after $ErrorActionPreference and then read the path from $args[0],
# where a stray flag was accepted as a filename and a quoted path arrived as
# "D:String". Both failures surfaced as -1 counters rather than as an error,
# which is why this file checks Test-Path and lists the logs it can see.
$ErrorActionPreference = 'Continue'

if (-not (Test-Path $Log)) {
    "LOG NOT FOUND: $Log"
    'Logs present:'
    Get-ChildItem 'D:\SteamLibrary\steamapps\common\Teardown\vr_*.log' `
        -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Desc | Select-Object -First 6 |
        ForEach-Object { '  {0}  {1}B  {2}' -f $_.Name, $_.Length, $_.LastWriteTime }
    exit 1
}
"using log: $Log"

# Two lines in the log mention GDI32: the hook report ("-> 0000... hooked") and
# the census ("calls=221558"). Select-Object -Last 1 on a bare 'IAT GDI32'
# pattern is not guaranteed to land on the census, so the pattern carries
# 'calls=' and the number is pulled by name rather than by position.
function Get-SwapCalls([string]$file) {
    $l = (Select-String -Path $file -Pattern 'IAT GDI32.*calls=' |
          Select-Object -Last 1).Line
    if (-not $l) { return -1 }
    $m = [regex]::Match($l, 'calls=(\d+)')
    if (-not $m.Success) { return -1 }
    [int64]$m.Groups[1].Value
}

function Get-ReadCount([string]$file) {
    $l = (Select-String -Path $file -Pattern 'ON-THREAD reads' |
          Select-Object -Last 1).Line
    if (-not $l) { return -1 }
    $m = [regex]::Match($l, '(\d+)\s*$')
    if (-not $m.Success) { return -1 }
    [int64]$m.Groups[1].Value
}

$p = Get-Process teardown -ErrorAction SilentlyContinue
if (-not $p) { 'NO GAME RUNNING'; exit 1 }

$priv0 = [int64]$p.PrivateMemorySize64
$ws0   = [int64]$p.WorkingSet64
$sw0   = Get-SwapCalls $Log
$rd0   = Get-ReadCount $Log
$cpu0  = [double]$p.CPU
$at0   = Get-Date

'{0}  t=0  priv={1}MB ws={2}MB presents={3} reads={4}' -f `
    $at0.ToString('HH:mm:ss'), [math]::Round($priv0/1MB), [math]::Round($ws0/1MB), $sw0, $rd0

if ($sw0 -lt 0 -or $rd0 -lt 0) {
    'WARNING: a counter came back -1, so the per-PRESENT figure would be'
    '         meaningless. The log needs a completed census block.'
}

Start-Sleep -Seconds $Wait

$p = Get-Process teardown -ErrorAction SilentlyContinue
if (-not $p) { 'GAME GONE'; exit 1 }

$priv1 = [int64]$p.PrivateMemorySize64
$ws1   = [int64]$p.WorkingSet64
$sw1   = Get-SwapCalls $Log
$rd1   = Get-ReadCount $Log
$dPriv = $priv1 - $priv0
$dSwap = $sw1 - $sw0
$dRead = $rd1 - $rd0
$dSec  = ((Get-Date) - $at0).TotalSeconds

''
't={0:N0}s  dPriv={1}MB ({2:N0} B/s)  presents={3}  reads={4}  dCPU={5:N1}s' -f `
    $dSec, [math]::Round($dPriv/1MB), ($dPriv/$dSec), $dSwap, $dRead, `
    ([double]$p.CPU - $cpu0)
'ws  {0}MB -> {1}MB' -f [math]::Round($ws0/1MB), [math]::Round($ws1/1MB)
''
if ($dRead -gt 0) { 'per PROBE READ  {0:N0} bytes' -f ($dPriv / $dRead) }
else              { 'per PROBE READ  n/a (no reads in window)' }
if ($dSwap -gt 0) {
    'per PRESENT     {0:N0} bytes' -f ($dPriv / $dSwap)
    'reads per present {0:N5}   (1/60 = 0.01667 when sampling every 60th frame)' -f ($dRead/$dSwap)
}
''
'Reading: per-READ is the cost of one glReadPixels. On vr_t1 it was 239138 B.'
'Near zero now, with the game still rendering, means the read path is clean.'
'per-PRESENT is that same growth over a much larger denominator, so it stays'
'small whenever the reads are the cause -- which is what separates growth from'
'sampling from growth that happens regardless of what we do.'
