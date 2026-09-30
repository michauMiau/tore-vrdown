$lf = $args[0]
$pat = $args[1]
if (-not (Test-Path $lf)) { "NO LOG: $lf"; exit }
Get-Content $lf | Select-String -Pattern $pat | ForEach-Object { $_.Line }
