# Follow the frame-record pointers and look for a projection matrix inside.
#
# beginRender indexes a table of records through renderer+0xDA8 / renderer+0xEF8.
# Each record is 48 bytes and holds pointers, not a matrix: the live float in
# every record is 15.03x, which is a render scale or a timing value, not a focal
# length. So the matrix, if the CPU ever touches it, is reached by following the
# pointers stored in the record.
#
# Three levels deep, 512 bytes at each hop, and classify every 64-byte aligned
# block as a matrix or not. The classifier is deliberately strict, because the
# earlier loose version accepted uninitialised memory full of zeros and NaN and
# produced nothing but noise.

$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace N12 {
    public static class M {
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, UIntPtr n, out UIntPtr read);
        [DllImport("kernel32.dll")]
        public static extern bool CloseHandle(IntPtr h);
    }
}
'@

$vr = 'D:\SteamLibrary\steamapps\common\Teardown\teardown_vr3.log'
$p  = Get-Process teardown -ErrorAction SilentlyContinue
if (-not $p) { Write-Host "game not running"; exit 1 }
$lines = Get-Content $vr
$rLine = $lines | Select-String -Pattern 'renderer=([0-9A-Fa-f]+)' | Select-Object -Last 1
if (-not $rLine) { Write-Host "no renderer= line"; exit 1 }
$null = $rLine.Line -match 'renderer=([0-9A-Fa-f]+)'
$obj = [Convert]::ToInt64($Matches[1], 16)

$h = [N12.M]::OpenProcess(0x0010, $false, $p.Id)
if ($h -eq [IntPtr]::Zero) { Write-Host "OpenProcess failed"; exit 1 }

function TdBytes([int64]$addr, [int]$size) {
    $buf = New-Object byte[] $size
    $r = New-Object UIntPtr 0
    $n = New-Object UIntPtr ([uint64]$size)
    if ([N12.M]::ReadProcessMemory($h, [IntPtr]$addr, $buf, $n, [ref]$r)) { return ,$buf }
    return $null
}
function TdQ([int64]$addr) { $b = TdBytes $addr 8; if ($b) { return [BitConverter]::ToInt64($b, 0) } return 0 }
function TdI([int64]$addr) { $b = TdBytes $addr 4; if ($b) { return [BitConverter]::ToInt32($b, 0) } return 0 }

function TdIsPtr([int64]$v) {
    if ($v -eq 0) { return $false }
    if (($v -band 0xFFFF000000000000) -ne 0) { return $false }
    return ($v -ge 0x10000 -and $v -lt 0x7FFFFFFFFFFF)
}

# A projection has a finite perspective term and a sane focal length. Anything
# with a NaN, an infinity, or an all-zero row is rejected outright.
function TdScore([byte[]]$raw, [int]$off) {
    $m = New-Object double[] 16
    for ($k = 0; $k -lt 16; $k++) {
        $f = [BitConverter]::ToSingle($raw, $off + $k * 4)
        if ([double]::IsNaN($f) -or [double]::IsInfinity($f)) { return 0 }
        $m[$k] = $f
    }
    $m11 = $m[11]
    if ([Math]::Abs([Math]::Abs($m11) - 1.0) -gt 0.01) { return 0 }
    $m00 = [Math]::Abs($m[0]); $m05 = [Math]::Abs($m[5])
    if ($m00 -lt 0.05 -or $m00 -gt 20) { return 0 }
    if ($m05 -lt 0.05 -or $m05 -gt 20) { return 0 }
    if ($m00 -lt 0.001 -and $m05 -lt 0.001) { return 0 }
    return 1
}

$idx  = TdI ($obj + 0xDA8)
$tbl  = TdQ ($obj + 0xEF8)
Write-Host ("renderer = 0x{0:X}  index = {1}  table = 0x{2:X}" -f $obj, $idx, $tbl)
if ($tbl -eq 0) { Write-Host "table null"; exit 1 }

$RecSize = 48
$Depth = 3
$Chunk = 512
$seen = @{}
$queue = New-Object System.Collections.Queue

for ($i = 0; $i -lt 6; $i++) {
    $rec = $tbl + $i * $RecSize
    for ($k = 0; $k -lt 6; $k++) {
        $v = TdQ ($rec + $k * 8)
        if (TdIsPtr $v -and -not $seen.ContainsKey($v)) {
            $seen[$v] = 0
            $queue.Enqueue(@($v, 0, "rec[$i]+$($k*8)"))
        }
    }
}

$found = 0
while ($queue.Count -gt 0 -and $found -lt 20) {
    $item = $queue.Dequeue()
    $addr = [int64]$item[0]; $depth = [int]$item[1]; $from = $item[2]
    $raw = TdBytes $addr $Chunk
    if (-not $raw) { continue }
    for ($off = 0; $off -le $Chunk - 64; $off += 4) {
        if ((TdScore $raw $off) -eq 1) {
            $found++
            Write-Host ("`n=== projection candidate at 0x{0:X} (from {1}, depth {2}) ===" -f ($addr + $off), $from, $depth)
            for ($r = 0; $r -lt 4; $r++) {
                $vals = @()
                for ($c = 0; $c -lt 4; $c++) {
                    $vals += ('{0,10:F4}' -f [BitConverter]::ToSingle($raw, $off + ($r*4 + $c) * 4))
                }
                Write-Host ("   [" + ($vals -join ' ') + "]")
            }
        }
    }
    if ($depth -lt $Depth) {
        for ($k = 0; $k -lt 16; $k++) {
            $v = TdQ ($addr + $k * 8)
            if (TdIsPtr $v -and -not $seen.ContainsKey($v)) {
                $seen[$v] = 0
                $queue.Enqueue(@($v, $depth + 1, ("0x{0:X}" -f ($addr + $k*8))))
            }
        }
    }
}

Write-Host ("`ntotal candidates: {0}   (searched {1} regions)" -f $found, $seen.Count)
[N12.M]::CloseHandle($h)
