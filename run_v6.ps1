# Start the game, wait until it is genuinely rendering, inject vr_v6, watch.
#
# vr_v6 adds a pbuffer: our own 512x512 off-screen context, on its own thread,
# drawn and read back entirely by us. The point is to prove the pipeline
# without reading the game's back buffer, because that read was measured at
# 239138 bytes of private memory per call and it is the only thing in this
# project that has ever cost the game its memory.
#
# The shape of this script is copied from run_u1.ps1 rather than from a fresh
# design, because every step of that one was paid for: Steam's -applaunch is
# not the same as steam://rungameid (this script waited 45s for a log that
# could never appear), an open MetaXR Simulator makes the next launch silently
# fail, and injecting before the game renders lands the hook on a process that
# has not yet imported what it is about to import.
#
#   .\run_v1.ps1

$ErrorActionPreference = 'SilentlyContinue'
$out = 'C:\tdvr\run_v1.txt'
Remove-Item $out -Force -EA SilentlyContinue
function A($s) { Add-Content $out -Value ("[" + (Get-Date -Format 'HH:mm:ss') + "] " + $s); Write-Host $s }

$tag = [IO.Path]::GetFileNameWithoutExtension($MyInvocation.MyCommand.Path)
$gd  = 'D:\SteamLibrary\steamapps\common\Teardown'
$hf  = "$gd\tdvr_host.txt"
$lf  = "$gd\vr_v6.log"
$dll = 'C:\tdvr\vr_v6.dll'
$app = 1167630

A "=== vr_v6: pbuffer, our own target, no read of the game ==="
A ("dll " + (Get-Item $dll).Length + "B md5=" + (Get-FileHash $dll -Algorithm MD5).Hash)

# Prove the host binary before the injector is given anything to attach to.
# Every RVA in this project was measured against one specific teardown.exe.
$g = "$gd\teardown.exe"
$gsz = (Get-Item $g).Length
if ($gsz -ne 30555208) { A ("ABORT: host " + $gsz + "B, expected 30555208B"); exit }
A ("host " + $gsz + "B sha256=" + (Get-FileHash $g -Algorithm SHA256).Hash.Substring(0,32))

# An open MetaXR Simulator makes Steam believe the game is still running, and
# the next launch is refused without a message. Measured, not guessed.
$sim = @(Get-Process MetaXRSimulator)
if ($sim.Count) {
    A ("closing MetaXRSimulator: " + (@($sim.Id) -join ','))
    $sim | Stop-Process -Force -EA SilentlyContinue
    Start-Sleep -Seconds 5
}
A ("MetaXRSimulator running: " + @(Get-Process MetaXRSimulator).Count)

Remove-Item $lf, $hf -Force -EA SilentlyContinue

# Start the game through run.ps1, which registers a scheduled task in session 1
# with principal 'vm'. Starting Steam from the SSH session launches the game in
# session 0, where it has no desktop, never presents, and every GL_BACK read
# comes back black -- which is how a whole run of measurements came to mean
# nothing. The session is now checked, not assumed.
$lp = "C:\tdvr\run.ps1"
if (-not (Test-Path $lp)) { A "ABORT: C:\tdvr\run.ps1 is missing, this launcher is the only thing that can start the game in session 1"; exit }
$task = "tdvr_$tag"
$before = Get-ScheduledTaskInfo -TaskName $task -TaskPath '\tdvr\' -EA SilentlyContinue
$job = Start-Process powershell -ArgumentList "-NoProfile","-ExecutionPolicy","Bypass","-File",$lp,"$tag","$tag" -PassThru -WindowStyle Hidden
$job.WaitForExit(90000) | Out-Null

# run.ps1 puts THIS script in session 1. The game is a separate step and was
# lost when the Steam launch was replaced by that call: the loop then reported
# "no process yet" thirty times over, which looks exactly like Steam refusing
# and is really nothing having asked Steam at all.
# steam.exe directly, not steam://rungameid. A URI needs a desktop shell to be
# resolved and there is none over SSH, so the protocol handler silently did
# nothing while the loop below reported "no process yet" for two minutes.
$steamExe = "C:\Program Files (x86)\Steam\steam.exe"
if (-not (Test-Path $steamExe)) {
    $steamExe = @("D:\Steam\steam.exe","C:\Program Files\Steam\steam.exe") |
                Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $steamExe) { A "ABORT: steam.exe not found in any of the three usual places"; exit 1 }
$running = @(Get-Process steam -EA SilentlyContinue)
if ($running.Count -eq 0) {
    A "steam.exe is not running; starting it from the task's own session"
    Start-Process $steamExe
    Start-Sleep -Seconds 12
} else {
    A ("steam already running: " + (@($running.Id) -join ','))
}
A ("steam now: " + @(Get-Process steam -EA SilentlyContinue).Count + " process(es)")
Start-Process -FilePath $steamExe -ArgumentList "-applaunch", "$app"
A "asked Steam to run appid $app"

# Ask the scheduler, not a log file. The first version of this guard searched a
# file whose name it derived from the script name, got the name wrong, and
# aborted a task that had in fact started (LastTaskResult=0). A guard built on a
# guessed filename is a guard that will eventually stop a working measurement
# and report it as a failure, which is worse than having no guard at all.
$after = Get-ScheduledTaskInfo -TaskName $task -TaskPath '\tdvr\' -EA SilentlyContinue
A ("task $task lastRun=" + $after.LastRunTime + " result=" + $after.LastTaskResult)
$ran = $after.LastRunTime -and ($after.LastRunTime -ne $before.LastRunTime)
if (-not $ran) {
    A ("ABORT: the task never ran. An Interactive task cannot be started from")
    A "  session 0, which is where SSH arrives, and Windows refuses it silently:"
    A "  LastTaskResult stays 267011 (0x41303, has not yet run)."
    exit 1
}
$lr = if (Test-Path "C:\tdvr\${tag}_launch.txt") { Get-Content "C:\tdvr\${tag}_launch.txt" -Raw } else { "" }
A ("launcher said: " + ($lr -replace "[\r\n]+"," | "))

$gp = 0
for ($i = 1; $i -le 24; $i++) {
    Start-Sleep -Seconds 10
    $ps = @(Get-Process teardown)
    if ($ps.Count -eq 0) { A ("t=" + ($i*10) + "s  no process yet"); continue }
    $p = $ps[0]
    $a = $p.CPU
    Start-Sleep -Seconds 4
    $q = Get-Process -Id $p.Id -EA SilentlyContinue
    if (-not $q) { A ("t=" + ($i*10) + "s  pid=" + $p.Id + " vanished"); continue }
    $d = [math]::Round($q.CPU - $a, 2)
    A ("t=" + ($i*10) + "s  pid=" + $q.Id + " sess=" + $q.SessionId +
       " ws=" + [math]::Round($q.WorkingSet64/1MB) + "MB cpuDelta4s=" + $d)
    if ($q.SessionId -ne 1) {
        A ("ABORT: pid=" + $q.Id + " is in session " + $q.SessionId + ", not 1. A game in")
        A "  session 0 has no desktop: it never presents, so every frame read is"
        A "  black and every measurement taken from it is worthless. Stopping."
        exit
    }
    if ($d -gt 0.4) { $gp = $q.Id; A "  game is rendering in session 1"; break }
}
if ($gp -eq 0) { A "TIMEOUT: no rendering game after 4 min; nothing injected"; exit }
A ("target pid=" + $gp)

$pre = Get-Process -Id $gp
$preWs = [math]::Round($pre.WorkingSet64/1MB)
A ("pre-injection ws=" + $preWs + "MB")

$own = @((Get-Process -Id $gp).Modules | Where-Object { $_.ModuleName -like 'vr_*.dll' })
if ($own.Count -gt 0) { A ("ABORT: already holds " + ($own.ModuleName -join ',')); exit }
A "own vr modules: none"

$mod = (Get-Process -Id $gp).Modules | Where-Object { $_.ModuleName -like 'teardown*' } | Select-Object -First 1
if (-not $mod) { A "ABORT: teardown module not listed"; exit }
$base = $mod.BaseAddress.ToInt64()
Set-Content -Path $hf -Value $base.ToString('x') -Encoding ASCII -NoNewline
A ("tdvr_host.txt <- 0x" + $base.ToString('x'))

A "injecting vr_v6"
$o = & 'C:\tdvr\injector_fresh.exe' --attach --dll $dll --noquit --timeout 30000 2>&1 | Out-String
($o -split "`r?`n") | Where-Object { $_ -match 'LoadLibrary|resolved|\[!\]' } | ForEach-Object { A ("  inj " + $_.Trim()) }
if ($o -notmatch 'LoadLibraryA OK') { A "injector did not confirm; aborting before any conclusion"; exit }
A "confirmed"

# The pbuffer thread runs once at load and logs its result immediately, so a
# short watch is enough to read it. The long watch exists only to show that
# nothing about the game's memory moved -- the previous builds bled for twenty
# minutes, and a fast sample cannot see that.
for ($i = 1; $i -le 12; $i++) {
    Start-Sleep -Seconds 10
    $q = Get-Process -Id $gp -EA SilentlyContinue
    if (-not $q) { A ("*** GONE at " + ($i*10) + "s ***"); break }
    $ws = [math]::Round($q.WorkingSet64/1MB)
    $pv = [math]::Round($q.PrivateMemorySize64/1MB)
    $delta = $ws - $preWs
    $sz = if (Test-Path $lf) { (Get-Item $lf).Length } else { 0 }
    $trend = if ($delta -lt -100) { '*** BLEEDING ***' } elseif ($delta -gt 100) { 'growing' } else { 'stable' }
    A ("t=" + ($i*10) + "s ws=" + $ws + "MB priv=" + $pv + "MB (vs " + $preWs + ", " +
       $trend + ") log=" + $sz + "B")
}

A "--- pbuffer result ---"
if (Test-Path $lf) {
    Get-Content $lf | Where-Object { $_ -match 'PBUFFER|px:|readback|GL_|uniform|VARIED' } |
        ForEach-Object { A ("  " + $_) }
} else { A "NO LOG" }
$fin = Get-Process -Id $gp -EA SilentlyContinue
if ($fin) { A ("final ws=" + [math]::Round($fin.WorkingSet64/1MB) + "MB vs pre " + $preWs + "MB; alive=True") }
else { A "final alive=False" }
