$ErrorActionPreference = 'Continue'
$root = 'D:\SteamLibrary\steamapps\common\Teardown'
$inj  = 'C:\tdvr\injector.exe'
# Default build. teardown_vr5.dll was the byte-identical twin of
# teardown_vr4.dll and has been removed by the build/ consolidation; inject
# teardown_vr4.dll instead, which is the same machine code. See build/INDEX.md.
$dll  = 'C:\tdvr\teardown_vr4.dll'
$name = [System.IO.Path]::GetFileNameWithoutExtension($dll)
$log  = Join-Path $root ($name + '.log')

"=== process ==="
$p = Get-Process teardown -ErrorAction SilentlyContinue
if (-not $p) { "no teardown running"; exit 1 }
"pid=" + $p.Id + " session=" + $p.SessionId + " responding=" + $p.Responding +
    " WS=" + [math]::Round($p.WorkingSet64/1MB) + "MB"

"=== inject $name ==="
$out = & $inj --attach --dll $dll --noquit 2>&1 | Out-String
($out.Trim() -split "`n" | Select-Object -Last 12) -join "`n"

# The injector writes tdvr_host.txt next to itself; the DLL reads it from the
# game folder. Bridge the two or the DLL cannot locate the image base.
$hs = 'C:\tdvr\tdvr_host.txt'
$hd = Join-Path $root 'tdvr_host.txt'
if (Test-Path $hs) {
    Copy-Item $hs $hd -Force
    "host file: " + (Get-Content $hd -Raw).Trim()
}

""
"=== waiting for the hook ==="
$deadline = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 4
    if (Test-Path $log) {
        $c = Get-Content $log -Raw -ErrorAction SilentlyContinue
        if ($c -match 'frame \d' -or $c -match 'ready' -or $c -match 'FAIL|cannot') { break }
    }
}

""
"=== $name log ==="
if (Test-Path $log) { Get-Content $log } else { "no log at $log" }

""
"=== process after ==="
$p = Get-Process teardown -ErrorAction SilentlyContinue
if ($p) { "pid=" + $p.Id + " responding=" + $p.Responding +
           " WS=" + [math]::Round($p.WorkingSet64/1MB) + "MB cpu=" + [math]::Round($p.CPU,1) + "s" }
else { "process gone" }
