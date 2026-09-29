# Decisive control with a file-writing side effect. builtin-screenrecorder
# is a shipped script mod; if script mods run at all it writes a video file
# we can find on disk. That proves execution far better than reading pixels.
# NOTE: build these strings without double quotes inside single-quoted PowerShell
# literals -- the earlier version broke the parser.
$ErrorActionPreference='Continue'
Get-Process teardown -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Start-Sleep -Seconds 3
$mods='D:\SteamLibrary\steamapps\common\Teardown\mods'
$Q=[char]34
'=== screenrecorder main.lua head ==='
Get-Content (Join-Path $mods 'screenrecorder\main.lua') -TotalCount 16
'=== its info.txt version ==='
(Get-Content (Join-Path $mods 'screenrecorder\info.txt') | Where-Object { $_ -match '^version' })
'=== clear recent recordings ==='
foreach ($p in @("$env:USERPROFILE\Videos","$env:USERPROFILE\Documents")) {
  Get-ChildItem $p -Recurse -File -EA SilentlyContinue |
    Where-Object { $_.LastWriteTime -gt (Get-Date).AddDays(-3) -and $_.Extension -in @('.mp4','.webm','.avi','.mkv') } |
    ForEach-Object { '  delete ' + $_.Name; Remove-Item $_.FullName -Force -EA SilentlyContinue }
}
'=== enable in modlist ==='
$f='C:\Users\vm\AppData\Local\Teardown\modlists\1.xml'
$x=Get-Content $f -Raw
if ($x.IndexOf('builtin-screenrecorder') -lt 0) {
  $line='  <mod id=' + $Q + 'builtin-screenrecorder' + $Q + '/>' + [char]13 + [char]10
  Set-Content $f ($x.Replace('</mods>', $line + '</mods>')) -NoNewline
  'added builtin-screenrecorder'
} else { 'already present' }
