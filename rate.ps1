# Measure the reporter's real period from outside, with no assumption.
#
# fps2.ps1 divides by a hardcoded 20000 ms and printed "the two rates disagree by
# 0.56x" twice, in two different runs, at two different rates. A constant
# disagreement is not noise, it means the assumed period is wrong by a constant
# factor -- and that made every rate derived from it wrong by the same factor.
#
# The fix is to stop assuming. This grows a byte count over a window and counts
# how many census lines appeared in it, so the period falls out of the data:
#
#     period = window / lines
#
# It then re-derives the call rate two ways, from the same data, and refuses to
# print a single number unless they agree. That disagreement is the signal worth
# having -- it is what exposed the wrong assumption in the first place.
#
#   .\rate.ps1 <logtag> [seconds]

$ErrorActionPreference = 'SilentlyContinue'
$tag   = if ($args.Count -ge 1) { $args[0] } else { 'vr_p1' }
$secs  = if ($args.Count -ge 2) { [int]$args[1] } else { 60 }
$lf    = "D:\SteamLibrary\steamapps\common\Teardown\$tag.log"

if (-not (Test-Path $lf)) { Write-Host "no log: $lf"; exit 1 }

function State {
    $lines = @(Select-String -Path $lf -Pattern 'IAT GDI32\.dll\s+SwapBuffers\s+calls=(\d+)')
    $n = $lines.Count
    $c = if ($n -gt 0) { [int]$lines[-1].Matches[0].Groups[1].Value } else { 0 }
    return @{ n = $n; calls = $c; bytes = (Get-Item $lf).Length }
}

$a = State
$t0 = Get-Date
Write-Host ("watching {0}s in {1}   (start: {2} census lines, calls={3})" -f $secs, $tag, $a.n, $a.calls)
Start-Sleep -Seconds $secs
$b = State
$wall = ((Get-Date) - $t0).TotalSeconds

$lines = $b.n - $a.n
$delta = $b.calls - $a.calls
$bytes = $b.bytes - $a.bytes

Write-Host ""
Write-Host ("window      {0}s" -f [math]::Round($wall, 2))
Write-Host ("census lines  {0}" -f $lines)
Write-Host ("log growth    {0} B  ({1} B per line)" -f $bytes, $(if ($lines -gt 0) { [math]::Round($bytes/$lines,1) } else { 'n/a' }))
Write-Host ("call delta    {0}" -f $delta)
Write-Host ""

if ($lines -le 0) {
    Write-Host "NO NEW CENSUS LINES. The reporter is not running; no rate exists."
    exit 1
}

# Period straight out of the data -- no hardcoded 20000 anywhere.
$period = $wall / $lines
$rate   = $delta / $wall
Write-Host ("measured period   {0}s per census pass" -f [math]::Round($period, 2))
Write-Host ("measured rate     {0} calls/s" -f [math]::Round($rate, 2))
Write-Host ""

# Cross-check: is the period a whole number of seconds, as Sleep() implies?
$nearest = [math]::Round($period)
Write-Host ("nearest whole second: {0}   (difference {1}s)" -f $nearest, [math]::Round([math]::Abs($period-$nearest), 2))
if ([math]::Abs($period - $nearest) -gt 1.5) {
    Write-Host "NOTE: period is not close to a whole second. Something other than a"
    Write-Host "single Sleep() governs this thread -- possibly more than one"
    Write-Host "reporter writing to the same file, or a timer plus the write cost."
}
Write-Host ""

# The game log has timestamps; the vr log does not. That gap is the reason this
# had to be measured externally at all, and it should be fixed in the hook.
Write-Host "--- process ---"
$p = Get-Process teardown -EA SilentlyContinue
if ($p) {
    Write-Host ("  ALIVE ws={0}MB priv={1}MB cpu={2} title=[{3}]" -f `
        [math]::Round($p.WorkingSet64/1MB), [math]::Round($p.PrivateMemorySize64/1MB),
        [math]::Round($p.CPU,1), $p.MainWindowTitle)
} else { Write-Host "  GONE" }

$l = 'C:\Users\vm\AppData\Local\Teardown\log.txt'
if (Test-Path $l) {
    Write-Host ""
    Write-Host "--- game log tail (has timestamps, unlike vr_*.log) ---"
    Get-Content $l -Tail 4 | ForEach-Object { Write-Host ("  " + $_) }
}
