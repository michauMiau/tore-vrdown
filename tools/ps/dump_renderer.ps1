# Dump live renderer memory to disk for offline analysis.
#
# Read-only: opens the target with PROCESS_VM_READ and copies bytes out. It
# does not write, does not inject, and does not touch the GPU, so it is safe to
# run while the game is in a level and while another build is being edited.
#
# Session 0 (over SSH) can open the process for memory reads - that part needs
# no desktop. It only fails when something has to SEE a window, which is why
# the screenshot tooling needs a scheduled task and this does not.
#
# Two things are dumped:
#   1. the renderer object, self+0x000 .. self+0x2000, so the layout can be
#      mapped from real bytes instead of from comments
#   2. every 8-byte slot in that range that looks like a pointer, followed for
#      0x100 bytes, so the SceneDynamicBuffer can be found by following
#      references rather than by guessing offsets
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File dump_renderer.ps1
#   powershell ... -DumpDir D:\somewhere

param(
    [string]$LogPath = 'D:\SteamLibrary\steamapps\common\Teardown\teardown_vr61.log',
    [string]$DumpDir = 'C:\tdvr\dump',
    [int]$Span = 0x2000,
    [int]$Follow = 0x100,
    [int]$Max = 80
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null

# Find the most recent "self=..." line the mod logged. beginRender and endRender
# both report it and both agree, so whichever appears last is the live object.
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
public class Mem {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint a, bool i, int p);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, int n, out IntPtr r);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr h);

    // Read the renderer's candidate pointers here rather than in PowerShell.
    // Every PowerShell route failed: [int64]v, [uint64]v, New-Object IntPtr
    // and [IntPtr]::new all throw for an address with the top bit set, and
    // the literal 0xFFFFFFFF evaluates to -1 as a signed Int32 in 5.1, so
    // even a correct -band is wrong. Inside C# the value is a real ulong and
    // the cast is a reinterpret, not an arithmetic operation.
    public static long Ptr(ulong v) { return unchecked((long)v); }
}
"@

$proc = Get-Process teardown -ErrorAction SilentlyContinue
if (-not $proc) { Write-Output "teardown.exe is not running"; exit 1 }

$h = [Mem]::OpenProcess(0x0010, $false, $proc.Id)   # PROCESS_VM_READ
if ($h -eq [IntPtr]::Zero) { Write-Output "OpenProcess failed"; exit 1 }

# ---- 1. flat dump of the renderer ----------------------------------------
$buf = New-Object byte[] $Span
$read = [IntPtr]::Zero
$selfAddr = [IntPtr]::new([Mem]::Ptr($self))
$ok = [Mem]::ReadProcessMemory($h, $selfAddr, $buf, $Span, [ref]$read)
if (-not $ok) { Write-Output "renderer read failed"; [Mem]::CloseHandle($h) | Out-Null; exit 1 }

$flat = Join-Path $DumpDir 'renderer_flat.bin'
[IO.File]::WriteAllBytes($flat, $buf)
Write-Output ("wrote {0}  ({1} bytes)" -f $flat, [int]$read.ToInt64())

# ---- 2. follow candidate pointers ---------------------------------------
# A slot counts as a candidate if it is 8-aligned, above 0x10000, and points at
# committed memory. That is deliberately loose: this is a survey, and a wrong
# guess costs a file rather than a crash.
$idx = 0
foreach ($i in 0..([int]($Span / 8) - 1)) {
    $v = [BitConverter]::ToUInt64($buf, $i * 8)
    if ($v -lt 0x10000) { continue }
    if (($v -band [uint64]7) -ne 0) { continue }

    $t = New-Object byte[] $Follow
    $tread = [IntPtr]::Zero
    # Build the address from its raw bytes, never through a numeric cast.
    # [int64]v, New-Object IntPtr and [IntPtr]v all throw "value is too large
    # for Int64" for any address with the top bit set, and 0xBC1B... is a
    # perfectly ordinary user-mode heap address in this process. PowerShell 5.1
    # has no IntPtr(byte[]) constructor, so use the two-Int32 constructor with
    # the low and high halves taken as unsigned 32-bit words - each fits in an
    # Int32, and the pair reconstructs the full 64-bit address exactly.
    $addr = [IntPtr]::new([Mem]::Ptr($v))
    if (-not [Mem]::ReadProcessMemory($h, $addr, $t, $Follow, [ref]$tread)) { continue }
    $tn = [int]$tread.ToInt64()
    if ($tn -lt 64) { continue }

    $path = Join-Path $DumpDir ("ptr_{0:X3}_to_{1:X}.bin" -f ($i * 8), $v)
    [IO.File]::WriteAllBytes($path, $t)
    Write-Output ("  +0x{0:X4} -> 0x{1:X}   {2} bytes" -f ($i * 8), $v, $tread)
    $idx++
    if ($idx -ge $Max) { Write-Output "  (stopping at $Max candidates)"; break }
}

Write-Output ("candidate pointers followed: {0}" -f $idx)
[Mem]::CloseHandle($h) | Out-Null
Write-Output "done - no bytes were written to the target process"
