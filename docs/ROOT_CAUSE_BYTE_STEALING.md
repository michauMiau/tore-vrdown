# ROOT CAUSE: byte-stealing detours froze the game (found v53, 2026-09-27)

## Summary

The crash that ended every build from v38 through v52 was caused by the two
**byte-stealing detours**, not by the vtable hooks, not by the memory reads, and
not by the Present scan.

| build | vtable hooks | detours | result |
|-------|--------------|---------|--------|
| v52   | yes          | 2 (5 + 15 bytes) | froze 10–20 s after injection, during a level |
| v53   | yes          | **none** | stable, 12 840 frames, no Sentry dialog |

## What the detours were

- `tdvr_patch_direct_call` on the SDB bind call site: **5 bytes** stolen
- `td_install_upload_hook` on the upload consumer at RVA `0x5BAEB0`: **15 bytes**
  stolen, redirecting through a trampoline built by `build_detour`

Both rewrite the instruction stream of a function that is actively executing.
If the stolen length does not end exactly on an instruction boundary, the
prologue becomes a different instruction sequence. The failure is therefore not
an immediate access violation — it is a corrupted code path that runs fine for
a while and then freezes.

That is the observed signature: the game froze within seconds, the Sentry dialog
appeared, and there was no faulting address and no minidump. Sentry's
`__sentry-event` recorded `level=fatal` with no exception detail.

## Why the diagnosis took this long

The measurement was wrong for most of the project, in three separate ways:

1. **Liveness was judged from the process.** After the freeze, `Get-Process`
   still found the pid and `Responding` still returned True, because the process
   was frozen rather than dead. A flat CPU counter was read as "rendering
   slowly, but alive" when it actually meant the exact opposite: a live renderer
   consumes CPU, a dead one stops.

2. **Some runs never got past the legal warning.** Without a keypress the game
   sat on a static screen, which produced a perfectly stable-looking process
   that proved nothing.

3. **Windows were enumerated from session 0.** SSH lands in session 0, which has
   a different window station, so the game was invisible there and "no windows"
   was indistinguishable from "the dialog closed".

The fix is `build/monitor_run.ps1`, run in session 1 via
`build/run_in_session1.ps1`. It reports two independent freeze signals — screen
pixel change between consecutive captures, and a CPU counter that has stopped
moving — and screenshots the instant either fires. One signal alone is not
enough: a live game with a still camera shows ~0.35 % pixel change.

## Second correction: the frame hooks were never broken

`beginRender` logged `1` call and `endRender` logged none, in every build from
v50 to v52. That read as "the vtable hooks are not the per-frame path". It was
wrong: the builds were dying so fast that the counters barely moved. In v53, with
the detours gone, the same hooks read:

```
[VR] beginRender 4320 (self=0000025c55107af0)
[VR] endRender 4440 (self=0000025c55107af0)
```

Both hooks are per-frame and both receive the same renderer object. So
`self + 0xE40` is a valid swapchain read and the whole frame path is sound. The
frame logic written from v42 onward was correct in principle; it simply never
had a surviving process to run in.

## What is not yet known

- Whether the SceneDynamicBuffer can be read without byte-stealing. It has to be
  read at the upload consumer, which is exactly the function that cannot be
  detoured safely. A vtable patch on the swapchain's `Present` (slot 9) avoids
  touching the game's code entirely and is the next step.
- Whether `hooked_end_render` is still called once per frame or twice, now that
  the process survives long enough to tell.

## Rules for future changes

1. **Do not steal bytes.** If a function must be intercepted, patch a vtable
   entry or an import, or use hardware breakpoints. `build_detour` exists and
   works, but the failure mode is a delayed freeze with no diagnostic, which
   costs far more than the feature is worth.
2. **Validate every measurement tool against a known-good state.** A test that
   cannot fail is not a test. This applied to the RTTI resolver earlier and to
   the liveness checks here.
3. **Never conclude "the game is fine" from a process check.** Use
   `monitor_run.ps1`, and always press the key past the legal warning first.
