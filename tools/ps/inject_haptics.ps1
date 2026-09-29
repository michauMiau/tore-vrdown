$ErrorActionPreference = 'Continue'
$root = 'D:\SteamLibrary\steamapps\common\Teardown'

"=== inject the VR mod into the running game ==="
$inj = 'C:\tdvr\injector.exe'
$dll = 'C:\tdvr\teardown_vr3.dll'
if (-not (Test-Path $inj)) { "  injector missing at $inj" } else {
    (Start-Process -FilePath $inj -ArgumentList '--attach', '--pid', '0', '--dll', $dll, '--noquit', '--nowindow' -Wait -PassThru -NoNewWindow) 2>&1 | ForEach-Object { "  " + $_ }
    "  exit=" + $LASTEXITCODE
}

Start-Sleep -Seconds 6

"=== VR log ==="
$vr = "$root\teardown_vr3.log"
if (Test-Path $vr) { Get-Content $vr -Tail 14 | ForEach-Object { "  " + $_ } } else { "  no VR log" }

"=== game log: graphics backend ==="
$lg = "$env:LOCALAPPDATA\Teardown\log.txt"
if (Test-Path $lg) {
    Get-Content $lg | Select-String -Pattern 'API|Compute|Renderer|adapter|shader' |
        Select-Object -First 14 | ForEach-Object { "  " + $_.ToString().Trim() }
}

"=== the built-in haptics folder ==="
"  --- data\haptic ---"
Get-ChildItem "$root\data\haptic" -Recurse -File -ErrorAction SilentlyContinue |
    Select-Object -First 20 | ForEach-Object { "  " + $_.FullName.Replace($root, '') + "  " + $_.Length + " B" }
"  count: " + (Get-ChildItem "$root\data\haptic" -Recurse -File -ErrorAction SilentlyContinue).Count

"  --- a sample haptic file, verbatim ---"
$sample = Get-ChildItem "$root\data\haptic" -Recurse -File -Filter '*.xml' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($sample) {
    "  FILE: " + $sample.FullName.Replace($root, '')
    Get-Content $sample.FullName | Select-Object -First 40 | ForEach-Object { "    " + $_ }
} else { "  no xml in data\haptic" }

"  --- dir listing of data\haptic, one level ---"
Get-ChildItem "$root\data\haptic" -ErrorAction SilentlyContinue |
    Select-Object Mode, Name, Length | Format-Table -AutoSize

"=== haptics strings in the game binary (steam_api / steaminput) ==="
"  steaminput folder:"
Get-ChildItem "$root\steaminput" -ErrorAction SilentlyContinue | ForEach-Object { "  " + $_.Name }
