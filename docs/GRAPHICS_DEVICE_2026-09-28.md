# OpenXR session refused: the real reason is a missing graphics device

Date: 2026-09-28. Machine: 192.168.1.6 (vm). Status: measured, not inferred.

## The finding

`xrCreateSession` has returned `XR_RESULT_LIMIT_REACHED (-38)` in every run since
the null driver was enabled. That reading came from `tdvr_xr_err()` falling back to
a numeric string, because `X->ResultToString` was never actually bound: it was
resolved once against `XR_NULL_HANDLE` before the instance existed, so the guard in
`tdvr_xr_err()` (`X->ResultToString && X->have_instance`) was always false.

After binding `xrResultToString` against the real instance, the runtime names the
code itself:

```
OpenXR: runtime names -38 as 'XR_ERROR_GRAPHICS_DEVICE_INVALID'
OpenXR: session flags=0x chain=no  -> XR_ERROR_GRAPHICS_DEVICE_INVALID
OpenXR: session flags=0x chain=yes -> XR_ERROR_GRAPHICS_DEVICE_INVALID
```

`-38` is `XR_ERROR_GRAPHICS_DEVICE_INVALID`. It is not a limit, not a handle bug, not
a bad `systemId`, and not caused by the `si.next` chain (both shapes fail
identically). The number was carrying a meaning I had been guessing at for hours.

## What the runtime actually wants

An `XrSession` must be bound to a graphics device. SteamVR refuses to create a
session that has no D3D/DXGI/Vulkan device behind it, and `XR_ERROR_GRAPHICS_DEVICE_INVALID`
is the spec's way of saying exactly that.

The whole XR path currently runs from the DLL's init thread, at DLL-load time:

```
[VR] looking for the game image, up to 10 s ...
[VR] host base 7ff7545f0000 handed over via tdvr_host.txt
[VR] ready
[VR] OpenXR: tdvr_xr_init entered          <-- init thread, no graphics device yet
```

At that moment the renderer has not created a swapchain, a device, or anything else.
There is nothing valid to hand the runtime, so it refuses. The `-18` seen earlier
(systemId = 0) and the `-38` were two faces of the same ordering bug: the session was
being asked for before the frame pipeline existed.

## What is now proven working

For the first time, hooks and the OpenXR runtime run together in one process, and the
process survives:

```
[VR] resolved via RTTI: vtable=00007ff755071d80 (rva 0xA81D80)
[VR] hooks installed via VTABLE: begin=... end=...
[VR] present: vtable=00007ffe2da3c830 slot 9 @+48 real=00007ffe2d9650e0 -> HOOK LIVE
[VR] present: call 1  swapchain=0000026f4d69f620 sync=0000026f8e53a708
[VR] probe endRender #1:   vtable=00007ffe2da3c830  slot9=00007ffe2d9650e0  in_module=1  <- THIS IS THE SWAPCHAIN
[VR] OpenXR: xrCreateInstance returned 0
[VR] OpenXR: instance created at API 1.1.0
[VR] OpenXR: system 1152964986855752170  vendor=10462  tracking pos=1 orient=1
[VR] OpenXR: view config offered: 2
[VR] OpenXR: blend modes for view 2 -> XR_SUCCESS (n=1)
[VR] OpenXR: session flags=0x chain=no -> XR_ERROR_GRAPHICS_DEVICE_INVALID
```

SteamVR, same second:

```
[Info] - New Connect message from D:\SteamLibrary\steamapps\common\Teardown\teardown.exe (VRApplication_OpenXRInstance) 8400
[Info] - Using existing HMD null.Null Serial Number
[Info] - Update ZeroPose for the chaperone universe 2
```

The swapchain at `self+0xE40` is a real swapchain, not a candidate. That object is
where the graphics device has to come from.

## Two blockers that were not blockers

**`TDVR_NO_XR` defaulted to 1.** `hook/bisect.h` had disabled the entire OpenXR path
for every build, including the one being tested, because runtime loading used to kill
the process and a hook crash could not be separated from a runtime crash. That reason
expired when the null driver was enabled: `xrCreateInstance` now returns `XR_SUCCESS`
and the process survives. Default is back to 0. Only the hook-bisect variants turn XR
off now, and they must, so a hook crash is never confused with a runtime crash.

**The injector was never being called correctly.** `injector.exe` takes options, not
a bare PID:

```
[warn] no LoadLibrary confirmation in injector output
[!] unknown option: 6136
Usage: C:\tdvr\injector.exe [options]
  --attach   inject into an already-running teardown.exe
```

Every "injection produced no log" result in this project was this one argument
mistake, not a failing hook. Correct call is `--attach --dll <path> --noquit`.
The injector also writes `tdvr_host.txt` next to *itself* (`C:\tdvr`) while the DLL
reads it from the *game* folder, so the copy is required; without it the DLL logs
`no host file at: tdvr_host.txt` and stops before resolving anything.

## Next step

Create the session where a device exists, not where the DLL loads:

1. In `beginRender`, on the first call, walk the confirmed swapchain to its parent
   device (`QueryInterface` on the swapchain's parent, or the device the vtable
   implies) and capture the `ID3D11Device`/`ID3D12Device` plus its `IDXGIFactory`.
2. Call `xrCreateSession` there, with an `XrGraphicsBinding*` chained into
   `XrSessionCreateInfo.next` carrying those pointers.
3. Keep `xrCreateInstance` and `xrGetSystem` where they are — those are device-free
   and both already succeed.
4. Report which interface actually came back, because Teardown's renderer is not
   known to be D3D11, D3D12 or Vulkan, and the binding struct must match it exactly.

`docs/OPENXR_2026-09-28.md` predates this and calls the error `LIMIT_REACHED`; that
was wrong and is superseded by this document.
