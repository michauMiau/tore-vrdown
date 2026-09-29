# MEASURED 2026-09-28 — the first real hook measurement, and the crash

This is the first time the native hook has ever been confirmed live. Everything
before this was inference.

## What was wrong with the test method, before anything else

The DLL was copied into the game folder and the game was started, and then
waited on. It never loaded. `Get-Process … | .Modules` showed 116 modules —
D3D12, D3D12Core, steam_api64, sentry — and no `teardown_vr.dll`.

The game does not load it. `injector.exe` does: `CreateRemoteThread` +
`LoadLibraryA`, per `docs/BUILD.md`. Nothing in this project had run the
injector in this session, so there was no log and no module, and the earlier
"the hook is not confirmed live" conclusion was right for the wrong reason.

## The result

```
[VR] === init thread, pid 4652 ===
[VR] no OpenXR loader -> "flat rendering"
[VR] host base 7ff7545f0000 handed over via tdvr_host.txt
[VR] resolved via RTTI: vtable=00007ff755071d80 (rva 0xA81D80)
[VR] hooks installed via VTABLE: begin=00007ff754ba35a0 end=00007ff754ba6e50
[VR] present: HOOK LIVE. Real Present saved at 00007ffe5b6550e0
[VR] ready
```

All three hooks are live and driving frames:

```
beginRender 5400   endRender 5400   present: call 10800
```

Two `beginRender` per `present` — the renderer draws twice per swap, and
`self` is the same pointer every frame (`000001f9742bb590`), which is what
makes a per-eye split possible at all. That is the fact the whole stereo design
depends on, and it is now measured rather than assumed.

## Two blockers, both real

**1. No OpenXR loader → the DLL itself prints `"flat rendering"`.**

So SteamVR is not attached to the host in session 1, and there is no HMD pose
to read. The build is not broken; it is telling the truth about the runtime it
can see. There is no OpenVR/SteamVR reference anywhere in `hook/` either, so
even with a runtime present nothing would consume the pose yet.

**2. `stereo_apply` never ran.** Grepping the whole run log for
`stereo|scene` returns nothing. Consistent with `docs/STATE_2026-09-28.md`:
the function is written, the caller does not exist.

## The crash

After ~8640 `beginRender` calls the game died.

```
[VR] beginRender 8640
```

and then nothing — no error line, no exception, no violation, nothing in the
65 KB log. The Sentry event:

```
level=fatal  platform=native  release=Teardown@2.1.0.20260914_1027_5db58979f8
sdk=sentry.native 0.15.2  integration=crashpad
```

`game_state.ps1` reported `CRASHED` while the process still existed with
`Responding=True`, a visible window, and the CPU counter still advancing — which
is the false-liveness case that detector exists for.

The crash landed at 8640 `beginRender`, not at frame 1, and no log line marks
the transition. The byte-stealing detours are compiled out in this build
(`V53: byte-stealing detours disabled`), so the earlier root cause documented in
`docs/ROOT_CAUSE_BYTE_STEALING.md` is not obviously in play here — but a
vtable hook that runs every frame is still a candidate, and a run that
survived 8640 frames is not a run that proved anything about frame 9000.

**Not established:** why it crashed. Candidates not yet separated: the vtable
detour itself, the `self+0xE40` swapchain read, the Present vtable walk, or
something unrelated. The log has no error to point at, and Sentry caught the
crash after the process was already dead, so there is no fault address and no
minidump.

## What is proven and what is not

**Proven:**
- the injector works, the DLL loads, and the three hooks are live
- the renderer calls `beginRender` twice per `present`, on a stable `self`
- the build's own OpenXR probe reports no runtime

**Not proven:**
- that the hooks are safe past ~8600 frames
- any HMD input
- any stereo output, which still has no caller

## Next step, and it is a bisect, not a feature

Three builds, each disabling one thing, run for longer than 8640 frames:

1. vtable hooks off entirely — establishes a clean baseline
2. `beginRender`/`endRender` hooked, `Present` not
3. all three, `self+0xE40` read disabled

Whichever one survives 20000 frames names the culprit. No new code — only
turning off what already exists, which is the one variable that has not been
controlled in this project yet.
