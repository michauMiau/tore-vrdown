# Is 0x5BAEB0 actually being called, or not?
#
# v32 through v35 all install the detour on 0x5BAEB0 and never log
# "capture: entered". Four builds, four identical outcomes. Two explanations
# remain, and they need different fixes:
#
#   a) The function is genuinely not called in this session. endRender's call
#      at 0x5B6E8F is on a path the game does not take here, or the vtable
#      hook points at an endRender that is not the one the game runs.
#   b) The detour is installed but the call comes from a different module, or
#      the patch did not land where the call goes.
#
# This does not patch anything. It reads the bytes at the call site and at the
# target from OUTSIDE the game, via ReadProcessMemory, and reports what is
# actually there. If the target still has the original prologue, the detour is
# not present in memory and (a) is proven. If the target has our FF 25 jump,
# the patch landed and the function is simply never called.
#
# Reading from outside is deliberate: the in-process log cannot distinguish
# "never called" from "called and returned before logging" once the process is
# involved, and the process is exactly what is in question.

param([int]$ProcId = 0)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class N2 {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr,
        byte[] buf, UIntPtr size, out UIntPtr read);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr h);
}
"@

if ($ProcId -le 0) {
    $pr = Get-Process teardown -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $pr) { Write-Output "NO_PROCESS"; exit 1 }
    $ProcId = $pr.Id
}
Write-Output "pid=$ProcId"

$h = [N2]::OpenProcess(0x410, $false, $ProcId)
if ($h -eq [IntPtr]::Zero) { Write-Output "OPEN_FAILED"; exit 1 }

$base = $null
$log = Get-ChildItem 'D:\SteamLibrary\steamapps\common\Teardown\teardown_vr*.log' |
       Sort-Object LastWriteTime -Desc | Select-Object -First 1
if ($log) {
    $m = Select-String -Path $log.FullName -Pattern 'host base ([0-9a-fA-F]+)' |
         Select-Object -First 1
    if ($m) { $base = [Convert]::ToInt64($m.Matches[0].Groups[1].Value, 16) }
}
if ($null -eq $base) { Write-Output "NO_BASE"; exit 1 }
Write-Output ("base=0x{0:X}" -f $base)

function Peek([long]$addr, [int]$len) {
    $buf = New-Object byte[] $len
    $got = [UIntPtr]::Zero
    $sz = [UIntPtr]::new([uint64]$len)
    if (-not [N2]::ReadProcessMemory($h, [IntPtr]$addr, $buf, $sz, [ref]$got)) {
        return $null
    }
    return $buf
}

function Hex([byte[]]$b) {
    ($b | ForEach-Object { $_.ToString('x2') }) -join ' '
}

# endRender and the consumer.
$SITES = @(
    @{ name = 'endRender';            rva = 0x5B6E50; n = 21 },
    @{ name = 'call to consumer';      rva = 0x5B6E8F; n = 5  },
    @{ name = 'consumer 0x5BAEB0';     rva = 0x5BAEB0; n = 20 }
)

foreach ($s in $SITES) {
    $addr = $base + $s.rva
    $b = Peek $addr $s.n
    if (-not $b) { Write-Output ("{0,-22} UNREADABLE at 0x{1:X}" -f $s.name, $addr); continue }
    Write-Output ("{0,-22} 0x{1:X}  {2}" -f $s.name, $s.rva, (Hex $b))
}

Write-Output ""
Write-Output "=== interpretation ==="
$call = Peek ($base + 0x5B6E8F) 5
$fn   = Peek ($base + 0x5BAEB0) 20

if ($call -and $call[0] -eq 0xE8) {
    Write-Output "call site still holds E8: the call was NOT patched."
    Write-Output "That is expected, we patch the function entry, not the site."
} elseif ($call) {
    Write-Output ("call site starts {0}, not E8" -f (Hex $call))
}

if ($fn) {
    $orig = @(0x4C,0x89,0x4C,0x24,0x20,0x53,0x55,0x56,0x57,0x41,0x54,0x48,0x83,0xEC,0x20)
    $isOrig = $true
    for ($i = 0; $i -lt $orig.Count; $i++) { if ($fn[$i] -ne $orig[$i]) { $isOrig = $false; break } }
    if ($isOrig) {
        Write-Output "consumer still has its ORIGINAL prologue."
        Write-Output ""
        Write-Output "  => the patch is not in memory."
        Write-Output "     Either a second DLL copy re-patched, or VirtualProtect"
        Write-Output "     silently failed, or the module we wrote to is a"
        Write-Output "     different mapping than the one the game calls."
    } else {
        Write-Output "consumer entry has been patched (not the original prologue)."
        if ($fn[0] -eq 0xFF -and $fn[1] -eq 0x25) {
            # The 8-byte address of an "FF 25 disp32" absolute indirect jump
            # starts at offset 6, not 8: FF 25 then a 4-byte displacement of 0,
            # then the pointer. Reading it from offset 8 picks up two bytes of
            # the displacement and the high half of the pointer, which
            # produces a nonsense address like 0x907FFE... and makes a correct
            # patch look corrupt.
            $jmpTarget = [BitConverter]::ToInt64($fn, 6)
            Write-Output ("     jump -> 0x{0:X}" -f $jmpTarget)
            if ($jmpTarget -gt 0xFFFF -and $jmpTarget -lt 0x7FFFFFFFFFFF) {
                Write-Output "     (plausible user-mode address)"
            } else {
                Write-Output "     (NOT a plausible address - patch is corrupt)"
            }
            Write-Output ""
            Write-Output "  => the patch IS in memory and points into our DLL."
            Write-Output "     If no capture line was logged, the function is"
            Write-Output "     genuinely not called on this path."
        } elseif ($fn[0] -eq 0xE9) {
            Write-Output "     relative 5-byte jump"
        }
    }
}

[N2]::CloseHandle($h) | Out-Null
Write-Output "OK"
