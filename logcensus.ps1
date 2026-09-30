$dir = 'D:\SteamLibrary\steamapps\common\Teardown'
Get-ChildItem "$dir\vr_*.log" | Sort-Object Name | ForEach-Object {
    $n = $_.Name
    $c = Get-Content $_.FullName
    $swap = $c | Select-String -Pattern 'SwapBuffers\s+calls=(\d+)' | ForEach-Object {
        if ($_ -match 'SwapBuffers\s+calls=(\d+)') { [int]$Matches[1] } }
    $last = if ($swap) { ($swap | Measure-Object -Maximum).Maximum } else { -1 }
    $px   = $c | Select-String -Pattern 'on-thread: uniform (\d+), VARIED (\d+)' | Select-Object -Last 1
    $txt  = if ($px) { $px.Line.Trim() } else { 'no on-thread line' }
    $vehp = @($c | Select-String -Pattern 'VEH|0xC000|exception').Count
    "{0,-16} size={1,-8} maxSwap={2,-10} vehLines={3,-4} {4}" -f $n, $_.Length, $last, $vehp, $txt
}
