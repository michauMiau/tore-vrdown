// bisect.h — which hooks are compiled in, for the 2026-09-28 crash bisect.
//
// The first ever live run crashed after ~8640 beginRender calls with no error
// line in the log, so the culprit is one of the things that are already there.
// Guessing is what this project has been doing wrong; instead each variable is
// turned off on its own and the build is run past the failure point.
//
// Every switch defaults to the full build, so an unmodified compile behaves
// exactly as it did. Define -DTDVR_NO_HOOKS / -DTDVR_NO_PRESENT /
// -DTDVR_NO_SWAPCHAIN_READ at compile time to disable one thing.
//
// Build time is under a second, so three variants is not a chore.

#ifndef TDVR_BISECT_H
#define TDVR_BISECT_H

// 1. All renderer hooks off. The DLL still loads and still logs its own probe
//    results, so this is a clean baseline: if the game still dies with nothing
//    hooked, the crash is not ours.
#ifndef TDVR_NO_HOOKS
#define TDVR_HOOKS_ENABLED 1
#else
#define TDVR_HOOKS_ENABLED 0
#endif

// 2. beginRender/endRender hooked, Present left alone. The Present path does a
//    vtable walk on a per-frame object, which is the most pointer-chasing code
//    in the build and the least necessary for finding the crash.
#ifndef TDVR_NO_PRESENT
#define TDVR_PRESENT_ENABLED 1
#else
#define TDVR_PRESENT_ENABLED 0
#endif

// 3. All three hooks live, but the self+0xE40 swapchain read is skipped. That
//    read is the one place the build dereferences a pointer it did not create
//    and then compares against an expected vtable.
#ifndef TDVR_NO_SWAPCHAIN_READ
#define TDVR_SWAPCHAIN_READ_ENABLED 1
#else
#define TDVR_SWAPCHAIN_READ_ENABLED 0
#endif

// 4. Only beginRender is hooked, endRender is left alone. The two vtable detours
//    were the only difference between the stable variant B and the crashing
//    variant C, and they are patched through the same mechanism on the same
//    object, so "which one" is the question the first bisect could not answer.
#ifndef TDVR_NO_END_HOOK
#define TDVR_END_HOOK_ENABLED 1
#else
#define TDVR_END_HOOK_ENABLED 0
#endif

// 5. Only endRender is hooked. Variant 4 with the roles swapped: if the crash
//    follows endRender in both directions, the end hook is the culprit, and if
//    it only ever happens with both, the two detours together are the problem.
#ifndef TDVR_NO_BEGIN_HOOK
#define TDVR_BEGIN_HOOK_ENABLED 1
#else
#define TDVR_BEGIN_HOOK_ENABLED 0
#endif

// 6. Skip the OpenXR runtime. The default flipped from "always on" back to
//    "always off" on 2026-09-28 for one reason only: every build that touched
//    the runtime died inside LoadLibraryA, so a crash could not be attributed
//    to the hooks. That reason is GONE. With the SteamVR null driver enabled
//    (driver_null.enable=true in steamvr.vrsettings) xrCreateInstance returns
//    XR_SUCCESS and the process survives. So XR is ON by default again, and
//    only the hook-bisect variants turn it off -- they must, so that a hook
//    crash is never confused with a runtime crash.
//
//    Measured, not assumed: no-HMD SteamVR killed the process, Meta killed the
//    process, SteamVR + null driver did not.
#ifndef TDVR_NO_XR
#define TDVR_NO_XR 0
#endif
#if TDVR_NO_XR
#define TDVR_XR_ENABLED 0
#else
#define TDVR_XR_ENABLED 1
#endif

// 7. Copy the game's backbuffer into the XR swapchain images: OFF as of
//    2026-09-29, and that default is a measurement, not a guess.
//
//    With the copy on, the layer submits fine (lastLayerCount=1,
//    projectionSubmissions climbing, the simulator's "waiting for stereo
//    projection" is gone) and then the GPU stops responding. error.yaml from the
//    resulting dump says, verbatim:
//
//        "GPU hung/removed/reset, HRESULT=887a0005"
//
//    887a0005 is DXGI_ERROR_DEVICE_RESET. The minidump for the same run has its
//    access violation at teardown.exe rva 0x4ffcc8 with read=0x1 at=0x0 -- the
//    game dereferencing a device that is gone -- and our DLL is loaded but NOT on
//    the stack. So the sequence is: the copy wedges the GPU, Windows resets it,
//    and the game then faults on the dead device.
//
//    Why the copy would wedge it is not yet known. The leading suspicion is
//    ownership: the swapchain images belong to the runtime, and CopyResource into
//    a resource whose state the runtime owns can conflict with what the runtime
//    is doing with it. That is a hypothesis, not a finding, and the honest next
//    step is to get a stable baseline back first, then re-enable one eye at a
//    time (TDVR_XR_COPY_EYE1) rather than both at once.
//
//    So the default now is: the loop runs, the layer submits, the preview is
//    empty. A stable, working pipeline is worth more than an unstable one with
//    pixels in it, and it is what the rest of the integration is built on.
#ifndef TDVR_NO_XR_COPY
#define TDVR_XR_COPY_ENABLED 0
#else
#define TDVR_XR_COPY_ENABLED 1
#endif

// 8. Copy only the left eye, when the copy is re-enabled. Halving the work is
//    the cheapest way to find out whether the reset scales with copy volume or
//    happens on the very first one.
#ifndef TDVR_XR_COPY_EYE1
#define TDVR_XR_COPY_EYE1 0
#endif

// 9. Hand xrEndFrame a real projection layer, or an empty one.
//
//    Set to 1 to submit layerCount = 0: the loop and the session lifecycle stay
//    exactly as they are, but the runtime is given nothing to composite. Both
//    crash runs measured so far report "GPU hung/removed/reset, HRESULT=887a0005"
//    -- DXGI_ERROR_DEVICE_RESET -- with the fault at teardown.exe rva 0x4ffcc8,
//    identically with the image copy on and off, and the game runs clean for the
//    whole test window with TDVR_NO_XR=1. So the trigger is XR, not the copy; this
//    switch narrows it to the compositing step itself.
#ifndef TDVR_XR_LAYER0
#define TDVR_XR_LAYER0 0
#endif

// 10. Load the OpenXR loader and stop -- no xrCreateInstance, no session, no
//     loop. The runtime is resident in the game's address space and idle.
//
//     This is the last control between "XR off" (no loader at all, game clean)
//     and "XR on" (game resets the GPU). If the game dies here, the runtime
//     being loaded is enough on its own and the whole integration has to start
//     from a different assumption. If it survives, the trigger is somewhere in
//     the instance/session/loop chain and can be bisected downward from there.
#ifndef TDVR_XR_RUNTIME_ONLY
#define TDVR_XR_RUNTIME_ONLY 0
#endif

// 11. Create the instance, then stop -- before xrGetSystem, before the session.
//
//     The boundary below TDVR_XR_RUNTIME_ONLY, which already proved that an idle
//     runtime resident in the game's process is harmless. If the game survives
//     with an instance too, the reset starts at xrGetSystem or the session
//     binding, and neither of those touches the GPU on its own, which would
//     point at the graphics binding rather than at anything we submit later.
#ifndef TDVR_XR_INSTANCE_ONLY
#define TDVR_XR_INSTANCE_ONLY 0
#endif

// 12. Session running, but the frame loop is never entered.
//
//     xrEndFrame blocks even with layerCount=0, so the stall is in the runtime's
//     frame-completion path rather than in compositing. That still leaves two
//     candidates, and this separates them:
//       (a) the runtime cannot complete a frame in this process at all, or
//       (b) it can, but only not while the game's own Present is outstanding.
//     With the loop suppressed, xrWaitFrame/xrBeginFrame/xrEndFrame are never
//     called, so a surviving game is evidence for (a) being the real limit --
//     EndFrame itself is the blocker. This mode only creates the session and
//     runs the event/state machine, which is what makes it a clean control: the
//     difference from a normal run is exactly the frame loop.
#ifndef TDVR_XR_NOLOOP
#define TDVR_XR_NOLOOP 0
#endif

// 14. Session on a device of our own instead of the renderer's.
//
//     NOLOOP proved the stall survives without the frame loop, so the invariant
//     across both runs is a live XR session bound to the game's own D3D12 device
//     while the game renders through that same device. Two things are tangled in
//     that: the session existing at all, and the device being shared. This
//     separates them -- tdvr_xr_make_device_and_queue() already exists and is
//     unused, so with this on, the session gets a private device+queue and the
//     renderer keeps its own untouched.
//
//     SURVIVES  -> sharing the device is the cause, and the fix is our own device.
//     STALLS    -> merely having a session alive is the cause, and the
//                  game/Agility-SDK/runtime combination is what cannot coexist.
#ifndef TDVR_XR_OWN_DEVICE
#define TDVR_XR_OWN_DEVICE 0
#endif

// 13. Create the instance without asking for any graphics extension.
//
//     xrCreateInstance is where the GPU reset starts, and the only thing this
//     code asks for at that point is XR_KHR_D3D12_enable. The runtime's own log
//     prints "Settings restored: profile=Quest 3" inside xrCreateInstance and
//     then stops, which is consistent with the runtime setting up a graphics
//     device in the game process the moment the extension is requested.
//
//     If the GPU survives with no extension requested, the extension request is
//     the trigger and the way forward is to create the instance before the game
//     has a device, or to hand the runtime a device of its own instead of
//     sharing the game's. If it still resets, the trigger is instance creation
//     itself and the extension is innocent.
#ifndef TDVR_XR_NO_D3D12_EXT
#define TDVR_XR_NO_D3D12_EXT 0
#endif

// 13. Instrument EVERY slot of the renderer vtable with a counting stub and
//     report the deltas twice a second.
//
//     This is the pivot, and it exists because the two slot numbers the whole
//     project is built on are wrong. TD_SLOT_BEGIN_RENDER=2 and
//     TD_SLOT_END_RENDER=3 point at two other virtual functions: at pid 8468
//     both slots were patched and read back as our own code, the vtable has at
//     least 32 slots, and NOT ONE of them held the address the resolver
//     reported for endRender (0x7FF754BA6E50) -- beginRender was never even
//     resolved (0x0). Neither hook has ever fired, so the OpenXR chain, the
//     device binding that hangs off endRender, and every result derived from
//     "the hook is installed" were all measured against a dead DLL.
//
//     The census does not need anyone to know which slot is which. Every slot
//     counts its own calls and forwards to the real implementation, so the game
//     runs normally and the methods that tick once per frame identify
//     themselves. Slots 2 and 3, if they are hot, are still being counted, which
//     is also the first direct evidence of whether they were ever the frame
//     hooks at all.
//
//     Each stub is hand-assembled, 24 bytes:
//         48 B8 <counter>    mov rax, counter
//         FF 00              inc qword ptr [rax]
//         48 B8 <original>   mov rax, real
//         FF E0              jmp rax
//     Slots whose first byte is 0xC3 or 0xCC are skipped: those are stub and
//     padding entries, and wrapping one would fault on the first call.
#ifndef TDVR_SLOT_CENSUS
#define TDVR_SLOT_CENSUS 0
#endif

// 14. Run the DXGI factory probe: create our own IDXGIFactory2, verify which
//     vtable slots really resolve into dxgi.dll, and report what the system's
//     adapters are.
//
//     This is path B, the pivot away from the engine's C++ object graph. The
//     measured reason: the renderer RTTI vtable accepted 48 counting stubs and
//     was never called once while the game demonstrably rendered, so the frame
//     hooks cannot be reached by class name from here. DXGI is underneath any
//     D3D12 game, and d3d12.dll, dxgi.dll, d3d11.dll and nvapi64.dll are all
//     loaded in the process on an RTX 4070.
//
//     It also re-tests the adapter walk, because the earlier
//     "D3D12CreateDevice returns E_NOTIMPL on every adapter" conclusion came
//     from a hand-rolled enumeration and may have been a bug in that rather than
//     a missing Agility SDK -- which matters a lot, because that E_NOTIMPL is
//     the recorded reason xrCreateSession refused the binding.
#ifndef TDVR_DXGI_PROBE
#define TDVR_DXGI_PROBE 0
#endif

// Swapchain capture: patch CreateSwapChain / CreateSwapChainForHwnd on dxgi.dll's
// shared factory vtable, and inspect whatever comes back on a worker thread.
//
// Path B, and the replacement for the renderer vtable hook that was measured dead
// (48 slots instrumented, zero calls, while the game ran at 8.19s of CPU growth
// per 3 s). dxgi.dll implements IDXGIFactory2 from one vtable, so the factory we
// create ourselves shares the game's -- which is a claim for the hook to confirm
// by firing, not to assume.
//
// Still off by default: it patches a slot in a live system DLL, and the exact
// behaviour under resize has not been observed yet. Enabled at runtime by
// "sc" in tdvr_flags.txt, so the flag needs no rebuild.
#ifndef TDVR_PRESENT_HOOK
#define TDVR_PRESENT_HOOK 0
#endif

// How many slots to instrument. The live vtable had at least 32 readable
// entries and the resolver's own two targets were in none of them, so the
// number is set well past what was previously assumed; a slot past the end of
// the real vtable is null and is skipped by the `if (!real) continue` check.
#define TDVR_CENSUS_SLOTS 48

// The bisect variant compiled in, for the log line, so a crash log says which
// build produced it without needing to guess from a filename.
#if TDVR_HOOKS_ENABLED == 0
#define TDVR_BISECT_NAME "A/none"
#elif TDVR_PRESENT_ENABLED == 0
#define TDVR_BISECT_NAME "B/no-present"
#elif TDVR_SWAPCHAIN_READ_ENABLED == 0
#define TDVR_BISECT_NAME "C/no-swapchain-read"
#elif TDVR_END_HOOK_ENABLED == 0
#define TDVR_BISECT_NAME "E/begin-only"
#elif TDVR_BEGIN_HOOK_ENABLED == 0
#define TDVR_BISECT_NAME "F/end-only"
#else
#define TDVR_BISECT_NAME "D/full"
#endif

#endif // TDVR_BISECT_H
