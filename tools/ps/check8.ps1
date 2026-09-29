$root = 'D:\SteamLibrary\steamapps\common\Teardown'
# Default build. teardown_vr8.dll was the byte-identical twin of
# teardown_vr7.dll and has been removed by the build/ consolidation; use
# teardown_vr7.dll instead, which is the same machine code. See build/INDEX.md.
$name = $args[0]; if (-not $name) { $name = 'teardown_vr7' }
$log = Join-Path $root ($name + '.log')

"=== stereo / scene buffer lines ==="
if (Test-Path $log) {
    $lines = Get-Content $log
    "total lines: " + $lines.Count
    $s = $lines | Where-Object { $_ -match 'stereo|scene|CANDIDATE|locked|Projection|implausible' }
    if ($s) { $s | Select-Object -First 25 } else { "no stereo/scene lines at all" }
} else { "no log" }

""
"=== log tail ==="
Get-Content $log -Tail 6

""
"=== frame counter growth over 15 s ==="
function Frames {
    $m = Select-String -Path $log -Pattern 'frame (\d+)' -AllMatches | Select-Object -Last 1
    if ($m) { [int]$m.Matches[0].Groups[1].Value } else { -1 }
}
$a = Frames; Start-Sleep -Seconds 15; $b = Frames
"t0=$a t1=$b"
if ($a -ge 0 -and $b -gt $a) { "fps = " + [math]::Round(($b-$a)/15, 1) } else { "no growth" }

""
"=== process ==="
$p = Get-Process teardown -ErrorAction SilentlyContinue
if ($p) { "pid=" + $p.Id + " responding=" + $p.Responding +
           " WS=" + [math]::Round($p.WorkingSet64/1MB) + "MB cpu=" + [math]::Round($p.CPU,1) + "s" }
else { "gone" }
