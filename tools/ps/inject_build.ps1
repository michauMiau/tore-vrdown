# Inject a build into the running teardown and report its log.
#
# Why parametrised: inject_any.ps1 hardcoded teardown_vr5.dll, so every build
# meant editing the script, and a stale edit silently injected an old build while
# the log name made it look current. -Dll makes the build explicit at the call
# site and the log is derived from it.
#
# The host file bridge is kept: the injector writes tdvr_host.txt next to
# itself in C:\tdvr, while the DLL looks for it in the game folder, so without
# the copy the DLL cannot find the image base.

param(
    [string]$Dll = 'C:\tdvr\teardown_vr41.dll',
    [int]$WaitSeconds = 90
)

$ErrorActionPreference = 'Continue'
$root = 'D:\SteamLibrary\steamapps\common\Teardown'
$inj  = 'C:\tdvr\injector.exe'
$name = [System.IO.Path]::GetFileNameWithoutExtension($Dll)
$log  = Join-Path $root ($name + '.log')

if (-not (Test-Path $Dll)) { "MISSING DLL: $Dll"; exit 1 }
if (-not (Test-Path $inj)) { "MISSING INJECTOR: $inj"; exit 1 }

"=== process ==="
$p = Get-Process teardown -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "NO_PROCESS"; exit 1 }
"pid=" + $p.Id + " session=" + $p.SessionId + " responding=" + $p.Responding +
    " WS=" + [math]::Round($p.WorkingSet64/1MB) + "MB"

"=== inject $name ==="
$out = & $inj --attach --dll $Dll --noquit 2>&1 | Out-String
($out.Trim() -split "`n" | Select-Object -Last 12) -join "`n"

$hs = 'C:\tdvr\tdvr_host.txt'
$hd = Join-Path $root 'tdvr_host.txt'
if (Test-Path $hs) {
    Copy-Item $hs $hd -Force
    "host file: " + (Get-Content $hd -Raw).Trim()
}

"=== waiting ${WaitSeconds}s for the hook ==="
$deadline = (Get-Date).AddSeconds($WaitSeconds)
$last = ''
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    if (-not (Get-Process -Id $p.Id -ErrorAction SilentlyContinue)) {
        "PROCESS DIED during the wait"
        break
    }
    if (Test-Path $log) {
        $c = Get-Content $log -Raw -ErrorAction SilentlyContinue
        if ($c -and $c -ne $last) {
            $last = $c
            if ($c -match 'present:|capture:|detached|HOOK LIVE|FAIL|cannot') { break }
        }
    }
}

$p2 = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if ($p2) {
    "process alive: responding=" + $p2.Responding +
        " WS=" + [math]::Round($p2.WorkingSet64/1MB) + "MB" +
        " cpu=" + [math]::Round($p2.CPU,1) + "s"
} else {
    "PROCESS GONE"
}

"=== $name log ==="
if (Test-Path $log) { Get-Content $log } else { "no log at $log" }
