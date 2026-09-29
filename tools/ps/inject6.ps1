$ErrorActionPreference = 'Continue'
$root = 'D:\SteamLibrary\steamapps\common\Teardown'
$inj  = 'C:\tdvr\injector.exe'
$dll  = 'C:\tdvr\teardown_vr6.dll'
$name = [System.IO.Path]::GetFileNameWithoutExtension($dll)
$log  = Join-Path $root ($name + '.log')

"=== process ==="
$p = Get-Process teardown -ErrorAction SilentlyContinue
if (-not $p) { "no teardown running - start it first"; exit 1 }
"pid=" + $p.Id + " session=" + $p.SessionId + " responding=" + $p.Responding +
    " WS=" + [math]::Round($p.WorkingSet64/1MB) + "MB cpu=" + [math]::Round($p.CPU,1) + "s"

"=== inject $name ==="
$out = & $inj --attach --dll $dll --noquit 2>&1 | Out-String
($out.Trim() -split "`n" | Select-Object -Last 8) -join "`n"

$hs = 'C:\tdvr\tdvr_host.txt'
$hd = Join-Path $root 'tdvr_host.txt'
if (Test-Path $hs) { Copy-Item $hs $hd -Force; "host: " + (Get-Content $hd -Raw).Trim() }

""
"=== waiting 30 s for projection data ==="
Start-Sleep -Seconds 30

""
"=== $name log (stereo lines only) ==="
if (Test-Path $log) {
    Get-Content $log | Where-Object { $_ -match 'stereo|Projection|eye|ready|hooks|renderer=' }
} else { "no log at $log" }

""
"=== log tail ==="
if (Test-Path $log) { Get-Content $log -Tail 8 }

""
"=== process after ==="
$p = Get-Process teardown -ErrorAction SilentlyContinue
if ($p) { "pid=" + $p.Id + " responding=" + $p.Responding +
           " WS=" + [math]::Round($p.WorkingSet64/1MB) + "MB cpu=" + [math]::Round($p.CPU,1) + "s" }
else { "PROCESS GONE" }
