# Hunt the upload SOURCE: the elements of the array at renderer+0xE70.
#
# WHY THIS SCRIPT EXISTS
#
# The verified disassembly of endRender is:
#
#   005AF28B  add  rcx, 0xE70
#   005AF292  call 0x5B3780              ; getter
#   005AF29F  lea  rcx, [rbx + 0x1130]
#   005AF2A6  mov  r8d, 0x800
#   005AF2AC  mov  rdx, rax              ; <-- rax is the SOURCE
#   005AF2AF  call 0x5B3280              ; upload
#
# and 0x5B3780 is a three-instruction getter:
#
#   005B3780  movsxd rdx, dword ptr [rcx+0x50]    ; index
#   005B3784  mov    rax, qword ptr [rcx+0x48]    ; array
#   005B3788  mov    rax, qword ptr [rax+rdx*8]   ; element
#   005B378C  ret
#
# So the object at renderer+0xE70 has {+0x48 = array pointer, +0x50 = count},
# and the upload's SOURCE is array[count-1] (the last element, since the index
# is read from the same struct and the getter uses it directly).
#
# +0x1130 is a DESTINATION. Everything the project assumed about reading
# matrices out of the renderer was looking at the wrong end of the copy.
#
# WHAT THIS SCRIPT DOES
# Reads the +0xE70 pair, walks the array, dumps each element, and tests every
# 0x10-aligned offset for a plausible float4x4. Read-only, so it is safe to
# run while the user is using the box for something else.

param(
    [string]$LogPath = 'D:\SteamLibrary\steamapps\common\Teardown\teardown_vr61.log',
    [string]$DumpDir = 'C:\tdvr\ehunt',
    [int]$ElemSize = 0x200,
    [int]$MaxElem = 24
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null

$self = $null
if (Test-Path $LogPath) {
    $m = Select-String -Path $LogPath -Pattern 'self=([0-9a-fA-F]{16})' |
         Select-Object -Last 1
    if ($m) { $self = [Convert]::ToUInt64($m.Matches[0].Groups[1].Value, 16) }
}
if (-not $self) { Write-Output "no self= in $LogPath - is the mod loaded?"; exit 1 }
Write-Output ("renderer = 0x{0:X}" -f $self)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class H {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint a, bool i, int p);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, int n, out IntPtr r);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr h);
    public static long Ptr(ulong v) { return unchecked((long)v); }
}
"@

$proc = Get-Process teardown -ErrorAction SilentlyContinue
if (-not $proc) { Write-Output "teardown.exe is not running"; exit 1 }
$h = [H]::OpenProcess(0x0010, $false, $proc.Id)
if ($h -eq [IntPtr]::Zero) { Write-Output "OpenProcess failed"; exit 1 }

function Read-Bytes([uint64]$addr, [int]$len) {
    $b = New-Object byte[] $len
    $r = [IntPtr]::Zero
    $a = [IntPtr]::new([H]::Ptr($addr))
    if ([H]::ReadProcessMemory($h, $a, $b, $len, [ref]$r)) { return $b }
    return $null
}

# --- the {array, count} pair at +0xE70 -------------------------------------
# rcx passed to the getter was renderer+0xE70, and the getter reads [rcx+0x48]
# and [rcx+0x50]. So relative to renderer the fields are +0xE70+0x48 and
# +0xE70+0x50.
$pair = Read-Bytes ($self + 0xE70) 0x60
if (-not $pair) { Write-Output "cannot read renderer+0xE70"; [H]::CloseHandle($h)|Out-Null; exit 1 }

$arrPtr = [BitConverter]::ToUInt64($pair, 0x48)
$count  = [BitConverter]::ToUInt32($pair, 0x50)
Write-Output ("+0xE70 object: array=0x{0:X16}  count={1}" -f $arrPtr, $count)

# Also read the pair that lives directly on the renderer, for comparison.
$arr0 = Read-Bytes ($self + 0xE40) 0x40
if ($arr0) {
    $a2 = [BitConverter]::ToUInt64($arr0, 0x08)
    $c2 = [BitConverter]::ToUInt32($arr0, 0x10)
    Write-Output ("+0xE40 object: array=0x{0:X16}  count={1}" -f $a2, $c2)
}

if ($arrPtr -eq 0 -or $count -eq 0) {
    Write-Output "no array or count is zero - is the renderer in a level yet?"
    [H]::CloseHandle($h)|Out-Null
    exit 1
}
if ($count -gt 4096) {
    Write-Output ("count {0} is implausible, clamping to 4096" -f $count)
    $count = 4096
}

# --- walk the array --------------------------------------------------------
$idxBuf = Read-Bytes $arrPtr ($count * 8)
if (-not $idxBuf) { Write-Output "cannot read the array"; [H]::CloseHandle($h)|Out-Null; exit 1 }

$hits = 0
$checked = 0
for ($i = 0; $i -lt $count; $i++) {
    if ($i -ge $MaxElem) { Write-Output ("  (stopping at {0} of {1} elements)" -f $MaxElem, $count); break }
    $ptr = [BitConverter]::ToUInt64($idxBuf, $i * 8)
    if ($ptr -lt 0x10000) { continue }
    $elem = Read-Bytes $ptr $ElemSize
    if (-not $elem) { continue }
    $checked++

    [IO.File]::WriteAllBytes((Join-Path $DumpDir ("elem_{0:D3}.bin" -f $i)), $elem)

    # A float4x4 that is not uninitialised heap: finite everywhere, three
    # non-zero diagonals in a sane range, non-zero perspective sum.
    for ($o = 0; $o + 64 -le $ElemSize; $o += 0x10) {
        $f = @()
        for ($k = 0; $k -lt 16; $k++) { $f += [BitConverter]::ToSingle($elem, $o + $k * 4) }
        $bad = $false
        foreach ($v in $f) {
            if ([double]::IsNaN($v) -or [double]::IsInfinity($v)) { $bad = $true; break }
        }
        if ($bad) { continue }
        $d0 = [math]::Abs($f[0]); $d5 = [math]::Abs($f[5]); $d10 = [math]::Abs($f[10])
        if ($d0 -le 0.01 -or $d0 -ge 1000) { continue }
        if ($d5 -le 0.01 -or $d5 -ge 1000) { continue }
        if ($d10 -le 0.01 -or $d10 -ge 1000) { continue }
        if ([math]::Abs($f[15]) -le 0.01) { continue }
        $persp = $f[3] + $f[7] + $f[11]
        if ([math]::Abs($persp) -lt 0.0001) { continue }

        $hits++
        Write-Output ("  MATRIX elem[{0}] +0x{1:X3}  d=({2:N3},{3:N3},{4:N3},{5:N3})  t=({6:N2},{7:N2},{8:N2})  persp={9:N3}" -f `
            $i, $o, $f[0], $f[5], $f[10], $f[15], $f[12], $f[13], $f[14], $persp)
    }
}

Write-Output ("elements readable: {0}   matrix-shaped hits: {1}" -f $checked, $hits)
if ($hits -eq 0) {
    Write-Output "no matrix found in any element. Raw hex of the LAST element follows,"
    Write-Output "because that is the one the getter's index selects."
    $last = [BitConverter]::ToUInt64($idxBuf, ($count - 1) * 8)
    if ($last -ge 0x10000) {
        $e = Read-Bytes $last 0x100
        if ($e) {
            for ($o = 0; $o -lt 0x100; $o += 16) {
                $hex = ''
                for ($k = 0; $k -lt 16; $k++) { $hex += ('{0:X2} ' -f $e[$o + $k]) }
                Write-Output ("    +0x{0:X3}  {1}" -f $o, $hex)
            }
        }
    }
}
[H]::CloseHandle($h)|Out-Null
Write-Output "done - no bytes were written to the target"
