param(
    [string]$Log,
    [int]$Wait = 60
)
# The slope script answered "no growth". This asks the harder question: is that
# because the leak is gone, or because the number being divided is too small to
# see it?
#
# At 1 read per 600 presents and ~144 presents/s that is one read every ~4 s.
# Over a 60 s window that is 15 reads. A 240 KB-per-read leak would show as
# 3.6 MB over that window, which is visible. A leak five times smaller would not
# be. So the window has to be long enough that the expected signal is several
# times the noise, and the noise has to be measured rather than assumed.
$ErrorActionPreference = 'SilentlyContinue'
if (-not $Log) {
    $cand = @(Get-ChildItem 'D:\SteamLibrary\steamapps\common\Teardown\vr_*.log' |
              Sort-Object LastWriteTime -Descending)
    if (-not $cand) { 'no logs'; exit 1 }
    $Log = $cand[0].FullName
}
"using log: $Log"

function Get-Nums($path) {
    $c = Get-Content $path
    $swap = -1; $reads = -1
    for ($i = $c.Count - 1; $i -ge 0; $i--) {
        if ($swap -lt 0 -and $c[$i] -match 'SwapBuffers\s+calls=(\d+)') { $swap = [int]$Matches[1] }
        if ($reads -lt 0 -and $c[$i] -match 'ON-THREAD reads\s+(\d+)') { $reads = [int]$Matches[1] }
        if ($swap -ge 0 -and $reads -ge 0) { break }
    }
    # two counters can sit at different report passes, so accept a neighbour
    if ($reads -lt 0) {
        for ($i = $c.Count - 1; $i -ge 0; $i--) {
            if ($c[$i] -match 'ON-THREAD reads\s+(\d+)') { $reads = [int]$Matches[1]; break }
        }
    }
    $p = Get-Process teardown -EA SilentlyContinue
    if (-not $p) { 'no teardown process'; exit 1 }
    [pscustomobject]@{
        swap = $swap; reads = $reads
        priv = [int64]$p.PrivateMemorySize64
        ws = [int64]$p.WorkingSet64
        cpu = [double]$p.CPU
        t = Get-Date
    }
}

$a = Get-Nums $Log
"t=0    priv=$([math]::Round($a.priv/1MB))MB presents=$($a.swap) reads=$($a.reads)"
if ($a.reads -lt 0) { "no ON-THREAD reads line in this log; this build may not have the probe"; exit 1 }

Start-Sleep -Seconds $Wait
$b = Get-Nums $Log
$dt = ($b.t - $a.t).TotalSeconds
$dPriv = $b.priv - $a.priv
$dSwap = $b.swap - $a.swap
$dRead = $b.reads - $a.reads
"t=$([math]::Round($dt))s  dPriv=$([math]::Round($dPriv/1MB,1))MB ($([math]::Round($dPriv/$dt)) B/s)  presents=$dSwap  reads=$dRead"

if ($dRead -gt 0) {
    "per PROBE READ  $([math]::Round($dPriv/$dRead)) bytes"
} else {
    "no reads happened in this window, so per-read cannot be computed here"
}
if ($dSwap -gt 0) { "per PRESENT     $([math]::Round($dPriv/$dSwap)) bytes" }

# The part that makes a null result trustworthy: state the smallest leak this
# window could have hidden, instead of letting "no growth" pass for "no leak".
if ($dRead -gt 0) {
    $resolvable = [math]::Abs($dPriv) / $dRead
    "window can resolve a leak of about $resolvable B/read; a leak below that"
    "  would look identical to none, so this is a bound and not a zero"
} elseif ($dSwap -gt 0) {
    $resolvable = [math]::Abs($dPriv) / $dSwap
    "no reads, so bound is $([math]::Round($resolvable)) B/present"
}
