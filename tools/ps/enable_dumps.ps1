# Enable LocalDumps for teardown.exe, so the next crash leaves a file to read.
#
# The game ships sentry-native, which uploads the crash and keeps nothing
# locally: WER's ReportArchive was empty, no Application event 1000 was ever
# written, and the only artefact was a 516-byte sentry event saying
# "level: fatal". That is not enough to work from.
#
# With LocalDumps on, Windows writes a full minidump to a folder we choose, and
# WER stops swallowing the event. This is a diagnostic change to the machine,
# not to the game, and it stays on so future crashes are always available.

$ErrorActionPreference = 'Stop'
$dumpDir = 'C:\tdvr\dumps'
$exe = 'teardown.exe'

New-Item -ItemType Directory -Path $dumpDir -Force | Out-Null

$k = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps'
New-Item -Path $k -Force | Out-Null
Set-ItemProperty -Path $k -Name 'DumpFolder' -Value $dumpDir
Set-ItemProperty -Path $k -Name 'DumpType'  -Value 2   # full dump
Set-ItemProperty -Path $k -Name 'DumpCount' -Value 10

# Per-application override. The machine-wide key would also capture every other
# crash on the box; this narrows it to the game so the folder stays readable.
$a = Join-Path $k $exe
New-Item -Path $a -Force | Out-Null
Set-ItemProperty -Path $a -Name 'DumpFolder' -Value $dumpDir
Set-ItemProperty -Path $a -Name 'DumpType'  -Value 2
Set-ItemProperty -Path $a -Name 'DumpCount' -Value 10

# WER has to be told to keep going locally, or the upload path short-circuits
# before the dump is written.
Set-ItemProperty -Path $k -Name 'SendDebugMessages' -Value 1 -ErrorAction SilentlyContinue

"=== LocalDumps configured ==="
Get-ItemProperty $a | Format-List DumpFolder, DumpType, DumpCount
"folder: $dumpDir"
