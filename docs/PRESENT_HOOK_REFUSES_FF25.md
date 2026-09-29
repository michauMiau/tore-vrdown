# v41: the crash, and what the log actually proves

## The run

Injected `teardown_vr41.dll` into a clean, level-running teardown (pid 11496).
The user saw Sentry's "Send the crash dump to the developers?" dialog and the
game died. Log `teardown_vr41.log`, last written 15:47:29.

## What the log proves

The upload detour installed cleanly:

```
upload hook: installed at 00007ff754baaeb0, trampoline 00007ffe7e8cdda0,
             15 bytes stolen
```

**Every single Present install attempt was refused:**

```
present: found at renderer+0x1F0 = 00007ffe55856250 (vtable slot 9)
present: could not steal a safe prologue from 00007ffe55856250
         (first bytes 48 8B 81 E0)
```

That repeats for offsets 0x1F0, 0x430, 0x4C0, 0x5E0, 0xE50, 0x1090 - the same
address each time, which is a shared vtable, and then the scan gives up.

**So no Present hook was ever live.** `td_install_present` returned 0 every
time, `g_present_orig` stayed NULL, and the trampoline was never written. The
crash therefore did **not** come from the Present code path, which rules out the
newest code as the cause.

## The measured 0xE40 was not usable

```
present: cannot read renderer+0xE40
```

`td_read` refused it, so the offset is either unmapped at that address or the
object passed in is not the renderer. Both are consistent with the log: `rdx`
was tried, then `r9`, then `rcx`, and none of the three is an object with a
readable 0xE40.

The offline measurement in docs/PRESENT_CALLSITE.md is still correct as a
statement about endRender's code - `mov rdx,[rbx+0xE40]` feeding
`call [rax+0x48]` is real. What is wrong is the assumption that `rbx` in that
function is reachable as an argument to the uploader. It is not. `rbx` is a
local, not a parameter.

## What is still unexplained

The crash occurred after the Present scan gave up, on the way back to
`g_orig_upload(rcx, rdx, r8d, r9)`. The detour to the uploader was in place and
had already survived one call, so the failure is either:

- the render thread faulting inside the game's own uploader after the detour
  replayed, or
- Sentry reacting to something the hook did to the thread, or
- a fault in `td_capture_upload`'s memscan on a subsequent call, which the log
  cannot show because it only logs the first three.

**No minidump was written.** `enable_dumps.ps1` set LocalDumps for
teardown.exe (DumpType 2, DumpCount 10) and `C:\tdvr\dumps` is empty, and there
is no WER event 1000. The game ships `sentry.dll` and `.sentry-native`, and
Sentry's own handler runs before WER gets the event, so WER never records
anything. **The dumps have to be enabled before the next run, not after**, and
this note is written after the crash, so the next injection is the one that will
actually produce a dump.

`.sentry-native` is a 1-byte marker with timestamp 15:47:06, which is when
Sentry last initialised. It is not a crash report.

## Next step, in order

1. Re-run the injection with LocalDumps already on, and get a real minidump.
   Without a faulting address nothing else is worth doing.
2. If no dump appears, the fault is not a structured exception WER can catch, and
   the next instrument is a `try`/`except` around the remainder of
   `hooked_upload` so an access violation is logged with its address before it
   propagates.
3. Only then look at the Present install. `measure_stealable` refusing
   `48 8B 81 E0 ...` is a real problem - that is `mov rax,[rcx-0x20]`, a
   perfectly ordinary DXGI thunk prologue - but it is not today's crash.
