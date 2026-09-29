# Read the live camera matrix at renderer+0x22AC and confirm it is a real
# view-projection matrix.
#
# WHY THIS SCRIPT
#
# Static analysis says the chain is:
#
#   00087742  call 0x5E7AD0                 compute proj*view
#   0008774A  movups [r15+0x22AC], xmm0     matrix -> renderer+0x22AC
#   00087752  movups [r15+0x22BC], xmm1
#   0008775E  movups [r15+0x22CC], xmm0
#   0008776A  movups [r15+0x22DC], xmm1     4 x 16 = 64 bytes
#
#   0009D4A6  movaps [rsp+0x30], xmm0       copy into the 500-byte SDB
#   ...
#   0009D7DD  call qword [rax+0x128]        UpdateSubresources
#
# If +0x22AC really holds the view-projection matrix, then reading it from the
# live process during a level must produce a matrix that satisfies the same
# properties a real VP matrix has:
#
#   - every element finite (no NaN, no inf)
#   - the diagonal non-zero and in a sane range
#   - m[3] + m[7] + m[11] (the perspective term) non-zero
#   - the matrix CHANGES between samples while the camera is moving
#
# The last one is the real test. A constant 64 bytes could be anything: an
# identity left over from init, or uninitialised heap that happens to look
# plausible. Only a value that tracks the camera is the camera matrix.
#
# Read-only, so it is safe to run during a level.

param(
    [string]$LogPath = 'D:\SteamLibrary\steamapps\common\Teardown\teardown_vr61.log',
    [int]$Samples = 6,
    [int]$IntervalMs = 700
)

$ErrorActionPreference = 'Stop'

$self = $null
if (Test-Path $LogPath) {
    $m = Select-String -Path $LogPath -Pattern 'self=([0-9a-fA-F]{16})' | Select-Object -Last 1
    if ($m) { $self = [Convert]::ToUInt64($m.Matches[0].Groups[1].Value, 16) }
}
if (-not $self) { "no self= in $LogPath - is the mod loaded?"; exit 1 }
"renderer = 0x{0:X}" -f $self

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class M {
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
if (-not $proc) { 'teardown.exe is not running'; exit 1 }
$h = [M]::OpenProcess(0x0010, $false, $proc.Id)
if ($h -eq [IntPtr]::Zero) { 'OpenProcess failed'; exit 1 }

$OFF = 0x22AC
$prev = $null
$changed = 0
$plausible = 0

for ($s = 0; $s -lt $Samples; $s++) {
    $buf = New-Object byte[] 64
    $r = [IntPtr]::Zero
    $addr = [IntPtr]::new([M]::Ptr($self + $OFF))
    if (-not [M]::ReadProcessMemory($h, $addr, $buf, 64, [ref]$r)) {
        "sample $s : read failed"
        continue
    }

    $f = @()
    for ($k = 0; $k -lt 16; $k++) { $f += [BitConverter]::ToSingle($buf, $k * 4) }

    $finite = $true
    foreach ($v in $f) { if ([double]::IsNaN($v) -or [double]::IsInfinity($v)) { $finite = $false } }

    $d0 = [math]::Abs($f[0]); $d5 = [math]::Abs($f[5]); $d10 = [math]::Abs($f[10])
    $persp = $f[3] + $f[7] + $f[11]
    $okDiag = ($d0 -gt 0.001 -and $d0 -lt 1e4) -and ($d5 -gt 0.001 -and $d5 -lt 1e4) -and ($d10 -gt 0.001 -and $d10 -lt 1e4)
    $okPersp = [math]::Abs($persp) -gt 1e-6
    $good = $finite -and $okDiag -and $okPersp
    if ($good) { $plausible++ }

    $cur = ($f | ForEach-Object { $_.ToString('G6') }) -join ','
    $differs = $false
    if ($null -ne $prev) {
        $differs = ($cur -ne $prev)
        if ($differs) { $changed++ }
    }
    $prev = $cur

    "sample $s  plausible=$good  changed=$differs"
    "   d=({0:G4},{1:G4},{2:G4},{3:G4})  persp={4:G4}" -f $f[0], $f[5], $f[10], $f[15], $persp
    "   row0: {0:G4} {1:G4} {2:G4} {3:G4}" -f $f[0], $f[1], $f[2], $f[3]
    "   row1: {0:G4} {1:G4} {2:G4} {3:G4}" -f $f[4], $f[5], $f[6], $f[7]

    Start-Sleep -Milliseconds $IntervalMs
}

[M]::CloseHandle($h) | Out-Null

''
"plausible samples: $plausible of $Samples"
"changed vs previous: $changed of $($Samples - 1) transitions"
''
if ($plausible -eq 0) {
    'VERDICT: never plausible. renderer+0x22AC does not hold a VP matrix as this test defines it.'
} elseif ($changed -eq 0) {
    'VERDICT: plausible but NEVER CHANGED. Could be an identity matrix or a stale copy, not a live camera.'
    '          This is exactly the case the tighter test exists to catch.'
} else {
    'VERDICT: plausible AND tracking. renderer+0x22AC is the live view-projection matrix.'
    '          This is a readable CPU-side camera matrix and the stereo seam is open.'
}
