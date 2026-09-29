# Build and run

## Cross compile

The project is built on Linux, targeting Windows x64, with mingw-w64.

```sh
sudo apt install mingw-w64     # Debian / Ubuntu
sh build.sh
```

`build.sh` produces `build/teardown_vr.dll` and `build/injector.exe`, strips
them, and prints the injector's import table. Expected output ends with:

```
  imported DLLs: KERNEL32.dll msvcrt.dll USER32.dll
```

If `x86_64-w64-mingw32-gcc` is missing the script says so and exits 1 rather
than producing a half-built DLL.

## Why not build on Windows

It works, MSYS2 UCRT64 with `pacman -S mingw-w64-ucrt-x86_64-gcc`. The reason
it is not the documented path is that every SEH-based reader has to be avoided
here, and mingw on Windows is not a different problem from mingw on Linux in
any way that matters to this codebase. The build only needs C99 and Win32.

## Defender

Defender flags `injector.exe`. It is a false positive and it cannot be avoided
without changing what the program does. The injector calls `OpenProcess`,
`VirtualAllocEx`, `WriteProcessMemory` and `CreateRemoteThread`, which is the
definition of DLL injection, and every game mod, profiler and debugger on
Windows does the same. `build/DEFENDER.md` has the details and the per-file
exclusion steps. Allow-list the file, do not exclude the whole game folder.

## The session 0 and session 1 problem

This is the part that costs the most time if you skip it.

SSH on Windows lands in session 0. Session 0 has its own window station, and
the desktop the game runs on is not in it. Three consequences, all of which
produced wrong conclusions before they were understood:

1. Enumerating windows from SSH finds no game window, which is indistinguishable
   from the window having closed.
2. Taking a screenshot from SSH captures the session 0 desktop, which is a
   blank framebuffer.
3. Anything that needs to interact with the game, including sending a keypress
   past the legal warning screen, does nothing at all.

So anything that touches windows, input, or the screen has to run as an
interactive scheduled task in session 1, using the interactive account. The
wrapper is `build/run_in_session1.ps1`.

```powershell
# from SSH, session 0
.\build\run_in_session1.ps1 -Script C:\tdvr\monitor_run.ps1 -WaitSeconds 300
```

It registers a task with `LogonType Interactive` and `RunLevel Highest` against
the local account `vm`, starts it, waits, prints the report, and unregisters.
A task started this way has no console and nothing captures its stdout, so the
wrapper redirects all output to a timestamped report file under `C:\tdvr\` and
prints it. Without that redirect the script runs to completion and every line is
lost, which looks exactly like the script having done nothing.

`vm` must be the display name of the local interactive account. Passing a
different display name fails with `0x80070534`.

## What counts as a healthy run

`Get-Process` and `Responding` both report a frozen game as healthy, because a
frozen game keeps its pid and its message pump. A flat CPU counter means the
opposite of healthy, since a live renderer burns CPU. Use
`build/monitor_run.ps1`, which checks two independent signals together:

- screen pixel change between consecutive captures
- a CPU counter that has stopped moving

One signal alone is not enough. A live game with a still camera shows about
0.35% pixel change. The monitor screenshots the instant either signal fires,
because a Sentry dialog closes itself if you are slow.

The other half of the run is `build/run_input.ps1`, which drives the menus from
inside session 1 and presses past the legal warning. Without that keypress the
game sits on a static screen producing no frames, and the run proves nothing.

## The full run

```powershell
.\build\run_in_session1.ps1 -Script C:\tdvr\full_run.ps1 -WaitSeconds 300
```

`build/full_run.ps1` does this in order:

1. `fresh_game.ps1`. Kill every `teardown` process, archive the old logs, relaunch
   into session 1. A failed archive is reported as a failure, not printed as
   "archived", because a silent archive failure means the next run reads the
   previous build's counters.
2. Wait for the game to be genuinely busy. `probe_is_rendering.ps1` samples
   process CPU over a few seconds. A game sitting in a menu burns nothing, and
   reading "no frames" from a process that never rendered says nothing about the
   hook.
3. Inject the build.
4. `run_input.ps1` to get into a level.
5. Read the log and report what was actually reached.

A fresh process per comparison is mandatory. Injecting a second build into a
process that already has one loaded produces:

```
RTTI resolution FAILED (vtable slot 2 is hooked by teardown_vr53.dll,
an earlier build of this mod still loaded in this)
```

The guard is correct. Keep it, and still use a fresh process every time.

## Injecting by hand

```bat
build\injector.exe --pid <PID> --dll "C:\tdvr\build\teardown_vr.dll"
```

The path handed to the remote `LoadLibraryA` must be absolute. The remote
process resolves relative paths against its own current directory, not the
injector's, so `teardown_vr.dll` alone silently fails.

Other options: `--attach` to find the process yourself, `--game` to launch it,
`--nowindow` to skip the main-window wait, `--noquit` to leave the game running
afterwards, `--delay` and `--timeout` in milliseconds.

`LoadLibraryA` on an already-loaded module returns the existing handle and does
nothing. Rebuilding the DLL and injecting again silently runs the old code and
leaves the old log in place, so the injector says so instead of reporting
success.

Logs land in the game directory as `teardown_vr.log`, next to `teardown.exe`,
and `injector.log` next to the injector.

## A healthy log

```
[VR] === teardown_vr.dll build 2026-09-26-dupdet (x64) ===
[VR] === init thread, pid 12345 ===
[VR] host image 00007ff7545f0000
[VR] resolved via RTTI: vtable=00007ff755071d80 (rva 0xA81D80)
[VR] hooks installed via VTABLE: begin=... end=...
[VR] present: vtable=00007ffe62d7c830 slot 9 @+48 real=00007ffe62ca50e0 -> ...
[VR] present: HOOK LIVE. Real Present saved at 00007ffe62ca50e0
[VR] present: call 1 / 600 / 1200 / 13200
[VR] beginRender 4320 (self=0000025c55107af0)
[VR] endRender 4440 (self=0000025c55107af0)
```

If you see `RTTI resolution FAILED` and `fallback to hardcoded RVAs`, the mod is
running on hardcoded addresses and will break on the next game patch. If you see
`ABORT: beginRender prologue mismatch`, the game updated and RTTI failed, and the
mod left the game untouched on purpose.

## Local tests

Five tests in `build/` are plain executables that run on Linux. They are the
fast feedback loop: a change to the Present install, the read guard, the
trampoline or the prologue measurer is covered without touching the game.

```sh
cd build
for t in test_present_patch test_memscan test_detour test_atomic test_stealable; do
    ./$t || echo "FAILED: $t"
done
```

All five passed on 2026-09-27. `test_present_patch` and `test_memscan` are the
two worth running after any change to `hook/present.h` or `hook/memscan.h`.

A test that fails for the wrong reason is worse than no test, because it teaches
you to distrust the code that is actually correct. `test_present_patch` failed
three times for reasons that had nothing to do with the install path, all three
documented in `docs/NIGHT_LOG_2026-09-27.md`: overlapping stack layout for three
globals, a five-element write into a four-element region array, and a fake
`VirtualProtect` that only accepted addresses in the code page while the code
under test writes to the vtable. AddressSanitizer caught all three.

## Workflow rules worth keeping

- One change per build. v52 against v53 is the only comparison in this project
  that meant anything, and it was only meaningful because exactly one thing
  differed.
- Never conclude the game is fine from a process check. Use `monitor_run.ps1`.
- Always press past the legal warning before concluding anything.
- Do not inject while the machine is being used for something else.
