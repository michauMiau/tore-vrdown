$ErrorActionPreference = 'Continue'
$root = 'D:\SteamLibrary\steamapps\common\Teardown'

"=== game process ==="
$p = Get-Process teardown -ErrorAction SilentlyContinue
if (-not $p) { "no teardown process"; exit 1 }
"teardown pid=" + $p.Id + " session=" + $p.SessionId + " responding=" + $p.Responding +
    " WS=" + [math]::Round($p.WorkingSet64/1MB) + "MB cpu=" + [math]::Round($p.CPU,1) + "s"

"=== inject the new build (teardown_vr4, layout auto-detect) ==="
$inj = 'C:\tdvr\injector.exe'
$dll = 'C:\tdvr\teardown_vr4.dll'
if (-not (Test-Path $inj)) { "injector.exe missing"; exit 1 }
if (-not (Test-Path $dll)) { "teardown_vr4.dll missing"; exit 1 }
"injector: $inj"
"dll:      $dll"

# --attach is the flag: without it the injector launches its own copy of the
# game, which would land in session 0 over SSH and never get a display.
# The injector writes tdvr_host.txt next to ITSELF (C:\tdvr), but the DLL reads
# it from the game folder. That worked for vr3 only because vr3 was injected by
# an older injector that wrote to the game directory. Copy it across so the DLL
# can find the image base.
$host_src = 'C:\tdvr\tdvr_host.txt'
$host_dst = Join-Path $root 'tdvr_host.txt'
if (Test-Path $host_src) {
    Copy-Item $host_src $host_dst -Force
    "tdvr_host.txt copied to the game folder:"
    "  " + (Get-Content $host_dst -Raw).Trim()
} else {
    "injector did not write $host_src"
}

$out = & $inj --attach --dll $dll --noquit 2>&1 | Out-String
"--- injector output ---"
$out.Trim()

"=== new log ==="
$log = Join-Path $root 'teardown_vr4.log'
if (Test-Path $log) {
    "log: $log  (" + (Get-Item $log).Length + " bytes)"
    Get-Content $log -Tail 40
} else {
    "no teardown_vr4.log yet"
}
