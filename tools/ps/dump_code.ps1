param([int]$Len = 0x8000)

$ErrorActionPreference = 'Stop'
Add-Type -Namespace N2 -Name M -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true)]
public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, UIntPtr n, out UIntPtr read);
[DllImport("kernel32.dll")]
public static extern bool CloseHandle(IntPtr h);
'@

$g = 'D:\SteamLibrary\steamapps\common\Teardown'
$vr = "$g\teardown_vr3.log"
$p = Get-Process teardown -ErrorAction SilentlyContinue
if (-not $p) { Write-Host "game not running"; exit 1 }

# Take the addresses from the log, not from a note. Hardcoded RVAs in an earlier
# version of this script were wrong and silently dumped the wrong bytes.
$line = Select-String -Path $vr -Pattern 'hooks installed via VTABLE' | Select-Object -Last 1
if (-not $line) { Write-Host "no hook line in the log"; exit 1 }
$bAddr = $eAddr = $base = $null
if ($line.Line -match 'begin=([0-9A-Fa-f]+)') { $bAddr = [Convert]::ToInt64($Matches[1], 16) }
if ($line.Line -match 'end=([0-9A-Fa-f]+)')   { $eAddr = [Convert]::ToInt64($Matches[1], 16) }
$im = Select-String -Path $vr -Pattern 'host image ([0-9A-Fa-f]+)' | Select-Object -Last 1
if ($im -and $im.Line -match 'host image ([0-9A-Fa-f]+)') { $base = [Convert]::ToInt64($Matches[1], 16) }
if (-not $bAddr -or -not $eAddr -or -not $base) { Write-Host "could not parse the log"; exit 1 }

Write-Host ("pid   {0}" -f $p.Id)
Write-Host ("base  0x{0:X}" -f $base)
Write-Host ("begin 0x{0:X}  rva 0x{1:X}" -f $bAddr, ($bAddr - $base))
Write-Host ("end   0x{0:X}  rva 0x{1:X}" -f $eAddr, ($eAddr - $base))

$h = [N2.M]::OpenProcess(0x0010, $false, $p.Id)
if ($h -eq [IntPtr]::Zero) { Write-Host "OpenProcess failed"; exit 1 }

$out = "$env:TEMP\tdvr_dump"
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Dump($addr, $len, $name) {
    $buf = New-Object byte[] $len
    $read = New-Object UIntPtr 0
    $n = New-Object UIntPtr ([uint64]$len)
    if (-not [N2.M]::ReadProcessMemory($h, [IntPtr]$addr, $buf, $n, [ref]$read)) {
        Write-Host "  $name FAILED"; return
    }
    [IO.File]::WriteAllBytes((Join-Path $out $name), $buf)
    Write-Host ("  {0,-18} {1} bytes" -f $name, $read.ToUInt64())
}

Dump ($bAddr - 0x400) $Len "begin_render.bin"
Dump ($eAddr - 0x400) $Len "end_render.bin"

[N2.M]::CloseHandle($h)
Write-Host "dump dir: $out"
Write-Host "done"
