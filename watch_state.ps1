param(
    [int]$Samples = 12,
    [int]$Every = 15,
    [string]$Log = 'D:\SteamLibrary\steamapps\common\Teardown\vr_v6.log'
)
$ErrorActionPreference = 'SilentlyContinue'

function Snap($tag) {
    $p = Get-Process teardown -EA SilentlyContinue
    if (-not $p) { Write-Output "$tag GONE"; return $null }
    $sz = 0
    if (Test-Path $Log) { $sz = (Get-Item $Log).Length }
    $line = '{0} {1} pid={2} sess={3} cpu={4} ws={5}MB priv={6}MB log={7}B' -f `
        (Get-Date -Format HH:mm:ss), $tag, $p.Id, $p.SessionId,
        [math]::Round($p.CPU, 1), [math]::Round($p.WorkingSet64 / 1MB),
        [math]::Round($p.PrivateMemorySize64 / 1MB), $sz
    Write-Output $line
    return $p
}

$a = Snap 'T0'
if (-not $a) { Write-Output 'no teardown process'; exit 1 }
$c0 = $a.CPU
$w0 = [math]::Round($a.WorkingSet64 / 1MB)
$p0 = [math]::Round($a.PrivateMemorySize64 / 1MB)
Write-Output "baseline ws=$w0 MB priv=$p0 MB cpu=$([math]::Round($c0,1)) sess=$($a.SessionId)"

Start-Sleep -Seconds 10
$b = Get-Process -Id $a.Id -EA SilentlyContinue
if ($b) {
    $d = [math]::Round($b.CPU - $c0, 2)
    Write-Output "cpu delta over 10s = $d s  (above 0.05 means it is rendering)"
}

$prevCpu = $b.CPU
for ($i = 1; $i -le $Samples; $i++) {
    Start-Sleep -Seconds $Every
    $q = Get-Process -Id $a.Id -EA SilentlyContinue
    if (-not $q) { Write-Output "T+$($i*$Every)s GONE"; break }
    $d = [math]::Round($q.CPU - $prevCpu, 1)
    $prevCpu = $q.CPU
    $ws = [math]::Round($q.WorkingSet64 / 1MB)
    $pv = [math]::Round($q.PrivateMemorySize64 / 1MB)
    $dw = $ws - $w0
    $dp = $pv - $p0
    $trend = if ($dp -lt -200) { 'BLEEDING' } elseif ($dp -gt 200) { 'growing' } else { 'stable' }
    Write-Output "T+$($i*$Every)s cpu=$d ws=$ws MB (d$dw) priv=$pv MB (d$dp) $trend"
}
Write-Output '--- last log lines ---'
if (Test-Path $Log) { Get-Content $Log -Tail 8 }
