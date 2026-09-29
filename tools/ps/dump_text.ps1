# Dump the whole unpacked .text of a running teardown.exe to a file.
#
# Why this exists: the on-disk teardown.exe on the game machine is the PACKED
# build (sha256 c8228c87...), so static analysis of the file is worthless. The
# unpacked image only exists in the running process, which is where the working
# RVAs (0x5B35A0, 0x5B6E50) actually came from.
#
# The object offsets did NOT survive that trip: renderer+0x1130 was read from the
# steamless build on disk, a different file. So the constant-upload offset has to
# be measured here, on the live unpacked .text, rather than carried over.
#
# Reads PE headers from the live process to locate .text, then dumps the whole
# section. No hooks, no writes, nothing patched.

param(
    [int]$ProcId = 0,
    [string]$Out  = "C:\tdvr\text_dump.bin"
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Native {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr,
        byte[] buf, UIntPtr size, out UIntPtr read);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr h);

    // EnumerateProcessModules and friends. The base of teardown.exe has to be
    // found without the mod's log, because the point of this run is a process
    // that carries no mod at all.
    [DllImport("psapi.dll", SetLastError=true)]
    public static extern bool EnumProcessModules(IntPtr h, IntPtr[] mods,
        uint size, out uint need);
    [DllImport("psapi.dll", SetLastError=true)]
    public static extern bool GetModuleBaseName(IntPtr h, IntPtr mod,
        System.Text.StringBuilder name, uint size);

    public static IntPtr FindMainBase(IntPtr h) {
        IntPtr[] mods = new IntPtr[1024];
        uint need = 0;
        if (!EnumProcessModules(h, mods, (uint)(mods.Length * 8), out need)) {
            return IntPtr.Zero;
        }
        for (int i = 0; i < (int)(need / 8); i++) {
            System.Text.StringBuilder sb = new System.Text.StringBuilder(260);
            if (!GetModuleBaseName(h, mods[i], sb, 260)) continue;
            if (sb.ToString().Equals("teardown.exe",
                                     StringComparison.OrdinalIgnoreCase)) {
                return mods[i];
            }
        }
        return IntPtr.Zero;
    }
}
"@

if ($ProcId -le 0) {
    $p = Get-Process teardown -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { Write-Output "NO_PROCESS"; exit 1 }
    $ProcId = $p.Id
}
Write-Output "pid=$ProcId"

# PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
$h = [Native]::OpenProcess(0x410, $false, $ProcId)
if ($h -eq [IntPtr]::Zero) { Write-Output "OPEN_FAILED"; exit 1 }

function Read-Mem([IntPtr]$addr, [int]$len) {
    $buf = New-Object byte[] $len
    $got = [UIntPtr]::Zero
    # UIntPtr cannot be cast from an Int32 directly on this PowerShell build;
    # build it from the integer value instead.
    $sz = [UIntPtr]::new([uint64]$len)
    if (-not [Native]::ReadProcessMemory($h, $addr, $buf, $sz, [ref]$got)) {
        return $null
    }
    return $buf
}

# The base of teardown.exe is normally read out of the mod's log ("host base
# ..."), but the whole point of this run is a process with no mod in it, so that
# log does not exist. Module enumeration is used first and the log is only a
# fallback; if neither works there is nothing to do.
$base = [IntPtr]::Zero

$base = [Native]::FindMainBase($h)
if ($base -ne [IntPtr]::Zero) {
    Write-Output ("base=0x{0:X} (from module enumeration)" -f $base.ToInt64())
} else {
    Write-Output "module enumeration found no teardown.exe; trying the mod log"
    $log = Get-ChildItem 'D:\SteamLibrary\steamapps\common\Teardown\teardown_vr*.log' `
            -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Desc | Select-Object -First 1
    if ($log) {
        $m = Select-String -Path $log.FullName -Pattern 'host base ([0-9a-fA-F]+)' |
             Select-Object -First 1
        if ($m) { $base = [IntPtr]::new([Convert]::ToInt64($m.Matches[0].Groups[1].Value, 16)) }
    }
    if ($base -eq [IntPtr]::Zero) {
        Write-Output "NO_BASE"
        exit 1
    }
    Write-Output ("base=0x{0:X} (from mod log)" -f $base.ToInt64())
}

# Parse the PE header out of memory to find .text.
$dos = Read-Mem $base 0x1000
if (-not $dos) { Write-Output "READ_DOS_FAILED"; exit 1 }
$peOff = [BitConverter]::ToInt32($dos, 0x3C)
$hdr   = Read-Mem ([IntPtr]($base.ToInt64() + $peOff)) 0x400
if (-not $hdr) { Write-Output "READ_PE_FAILED"; exit 1 }

$nsec  = [BitConverter]::ToUInt16($hdr, 6)
$optsz = [BitConverter]::ToUInt16($hdr, 20)
Write-Output "sections=$nsec"

$secBase = $peOff + 24 + $optsz
$textRva = 0; $textSize = 0
for ($i = 0; $i -lt $nsec; $i++) {
    $s = Read-Mem ([IntPtr]($base.ToInt64() + $secBase + $i * 40)) 40
    if (-not $s) { continue }
    $name = [Text.Encoding]::ASCII.GetString($s, 0, 8).TrimEnd([char]0)
    $vsize  = [BitConverter]::ToUInt32($s, 8)
    $vaddr  = [BitConverter]::ToUInt32($s, 12)
    Write-Output ("  {0,-8} rva=0x{1:X8} vsize=0x{2:X}" -f $name, $vaddr, $vsize)
    if ($name -eq '.text') { $textRva = $vaddr; $textSize = $vsize }
}

if ($textSize -eq 0) { Write-Output "NO_TEXT"; exit 1 }
Write-Output ("dumping .text rva=0x{0:X} size=0x{1:X}" -f $textRva, $textSize)

$dst = [IO.File]::Create($Out)
$CHUNK = 0x100000
$done = 0
while ($done -lt $textSize) {
    $want = [Math]::Min($CHUNK, $textSize - $done)
    $b = Read-Mem ([IntPtr]($base.ToInt64() + $textRva + $done)) $want
    if (-not $b) { Write-Output ("  gap at 0x{0:X}" -f ($textRva + $done)); break }
    $dst.Write($b, 0, $want)
    $done += $want
}
$dst.Close()
[Native]::CloseHandle($h) | Out-Null

$fi = Get-Item $Out
Write-Output ("WROTE {0} bytes to {1}" -f $fi.Length, $Out)
Write-Output "TEXT_BASE={0:X}" -f ($base.ToInt64() + $textRva)
Write-Output "TEXT_RVA={0:X}" -f $textRva
Write-Output "OK"
