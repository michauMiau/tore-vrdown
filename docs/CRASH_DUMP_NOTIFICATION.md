# The "crash dump" notification — what it actually is

## What the user reported

A window titled "crash dump" appeared on the desktop while trying to enter a
level. The user calls it a crash dump because that is what the window says.

## What the machine shows

```
WER 1000 (Application Error):     none, ever
*.dmp in C:\tdvr:                 none
*.dmp in C:\tdumps:               none
*.dmp in the game directory:      none
werfault.exe running:            none (window already dismissed)
WER events in the last hour:     only Security-SPP licence noise

.sentry-native     27.09.2026 15:02:49   size = 1 byte
sentry.dll         26.09.2026 19:04:02   398848 bytes
```

A 1-byte `.sentry-native` is Sentry's **lock/cleanup sentinel**, not a captured
crash. A real Sentry native crash leaves a `*.run` directory with metadata and
`settings.dat`, which is what earlier sessions showed.

So the dialog is not a WER crash report. It is almost certainly the game's own
sentry.dll reporting a handled fault (an exception it catches and writes up),
surfaced as a notification window. Nothing in the Windows crash-reporting path
fired, which is consistent with a process that is still alive at the time and
with the process in fact being alive afterwards.

## Timing contradicts the obvious attribution

The window appeared at **14:48:55**. v39 was injected at roughly **14:49**,
into pid 8348, which was already rendering. The `.sentry-native` timestamp of
15:02:49 is the later sentinel written when that process did exit.

So the dialog preceded the v39 injection. It cannot be attributed to v39.

## What did kill pid 8348

```
[VR] === detached after 0 frames ===
```

The DLL logged a clean detach with a frame count of zero, and no WER event
followed. This is the same late-death signature seen with v38: the process
survives injection and then ends minutes later with no dump. For v38 that was
traced to the torn patch in build_detour() and fixed by operand-first ordering
(892 million torn reads before, 0 after, in build/test_atomic.c).

v39 carries that fix and still ended with `detached after 0 frames`. So either:

  a) another non-atomic write remains, or
  b) the death is the game's own, not the mod's.

Note that `beginRender` was never entered in that run even though the game was
demonstrably drawing. That means the frame path the mod hooks was not executing,
which removes the most obvious suspect and leaves (a) or (b).

## Honest position

I cannot attribute the "crash dump" window to the mod. The evidence says it
happened before v39 was injected, no Windows crash report exists, and the file
the game writes is a 1-byte sentinel. What I can say is that pid 8348 did die,
that its log ends in a clean detach rather than a fault, and that the v38 race
fix is verified present in v39.

## Next measurement that would settle it

`build/full_run.ps1` already refuses to inject into a process that is not
drawing. Two additions make attribution possible:

  1. Snapshot whether teardown.exe is running immediately before injection, and
     record that pid in the log, so any later death is unambiguous.
  2. On exit, report the exact exit time against the injection time, so a
     "died 12 minutes later" case is distinguishable from "died immediately".

Without those, "the game crashed" and "the mod killed it" remain the same
observation, which is what caused the earlier false conclusions.
