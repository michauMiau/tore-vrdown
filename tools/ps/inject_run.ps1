$ErrorActionPreference = 'Continue'
$root = 'D:\SteamLibrary\steamapps\common\Teardown'
$inj  = 'C:\tdvr\injector.exe'
$name = $args[0]
# Default build. teardown_vr8.dll was the byte-identical twin of
# teardown_vr7.dll and has been removed by the build/ consolidation; inject
# teardown_vr7.dll instead, which is the same machine code. See build/INDEX.md.
if (-not $name) { $name = 'teardown_vr7' }
$dll  = "C:\tdvr\$name.dll"
$log  = Join-Path $root ($name + '.log')

"=== process ==="
$p = Get-Process teardown -ErrorAction SilentlyContinue
if (-not $p) { "no teardown running"; exit 1 }
"pid=" + $p.Id + " session=" + $p.SessionId + " responding=" + $p.Responding +
    " WS=" + [math]::Round($p.WorkingSet64/1MB) + "MB cpu=" + [math]::Round($p.CPU,1) + "s"

"=== loaded vr modules already in the process ==="
$existing = (Get-Process teardown).Modules |
            Where-Object { $_.ModuleName -like 'teardown_vr*' } |
            ForEach-Object { $_.ModuleName }
if ($existing) { "  " + ($existing -join ', ') } else { "  none" }

"=== inject $name ==="
$out = & $inj --attach --dll $dll --noquit 2>&1 | Out-String
($out.Trim() -split "`n" | Select-Object -Last 6) -join "`n"

$hs = 'C:\tdvr\tdvr_host.txt'
$hd = Join-Path $root 'tdvr_host.txt'
if (Test-Path $hs) { Copy-Item $hs $hd -Force }

""
"=== waiting 40 s ==="
Start-Sleep -Seconds 40

""
"=== $name log ==="
if (Test-Path $log) { Get-Content $log | Select-Object -First 45 } else { "no log" }

""
"=== process after ==="
$p = Get-Process teardown -ErrorAction SilentlyContinue
if ($p) { "pid=" + $p.Id + " responding=" + $p.Responding +
           " WS=" + [math]::Round($p.WorkingSet64/1MB) + "MB cpu=" + [math]::Round($p.CPU,1) + "s" }
else { "PROCESS GONE" }
