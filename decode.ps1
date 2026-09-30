$x = [IO.File]::ReadAllText("C:\tdvr\v6b64.txt")
[IO.File]::WriteAllBytes("C:\tdvr\run_v6.ps1", [Convert]::FromBase64String($x))
"md5   " + (Get-FileHash C:\tdvr\run_v6.ps1 -Algorithm MD5).Hash
"bytes " + (Get-Item C:\tdvr\run_v6.ps1).Length
$e = $null
[void][System.Management.Automation.Language.Parser]::ParseFile("C:\tdvr\run_v6.ps1", [ref]$null, [ref]$e)
if ($e) { "PARSE FAIL"; $e | ForEach-Object { "  " + $_.Message } } else { "parse=OK" }
Remove-Item C:\tdvr\v6b64.txt -EA 0
