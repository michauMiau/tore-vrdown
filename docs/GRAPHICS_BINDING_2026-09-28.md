# Graphcs binding: the confirmed reason xrCreateSession is refused

Date: 2026-09-28. Supersedes `docs/OPENXR_2026-09-28.md` (which called the
error `LIMIT_REACHED`) and refines `docs/GRAPHICS_DEVICE_2026-09-28.md`.

## The two corrections

**1. `-38` was never `XR_ERROR_LIMIT_REACHED`.** From the SDK header:

```
XR_ERROR_LIMIT_REACHED            = -10   (openxr.h:163)
XR_ERROR_CALL_ORDER_INVALID       = -37   (openxr.h:189)
XR_ERROR_GRAPHICS_DEVICE_INVALID  = -38   (openxr.h:190)
```

The session-limit gate exists in `vrclient_x64.dll` at `0x18003bf30`, logs
`xrCreateSession: Reached limit of supported number of sessions`, and returns
`0xfffffff6` = **-10** on a different path. So "too many sessions" was never the
answer to any run in this project. The number `-38` was being read wrong because
`X->ResultToString` was never bound: it was resolved against `XR_NULL_HANDLE`
before the instance existed, so `tdvr_xr_err()`'s guard was permanently false and
every error printed numerically. Binding it to the real instance made the runtime
name its own code: `XR_ERROR_GRAPHICS_DEVICE_INVALID`.

**2. The cause is stated verbatim in SteamVR's per-app client log**, not just
inferred from disassembly. `C:\Program Files (x86)\Steam\logs\xrclient_teardown.txt`:

```
[Error] - xrCreateSession: No binding struct was provided
[Warning] - xrCreateSession: Ignoring unsupported structs in next chain of type: 45
```

`45` = `XR_TYPE_VIEW_CONFIGURATION_PROPERTIES`. The `XrViewConfigurationProperties`
I was chaining in is not a graphics binding, the runtime discards it, and then
finds no binding at all. This also explains why the error was identical with and
without `si.next`: both shapes had no binding.

## Confirmed in the binary

`vrclient_x64.dll` (5075096 B, named by `steamxr_win64.json` as
`runtime.library_path = bin\vrclient_x64.dll`):

- `0x18003fbbc  mov edi,0xffffffda` paired with `lea rdx` ->
  `"xrCreateSession: No binding struct was provided"`; `edi` -> `eax` at
  `0x18003fd8e` and returned. `0xffffffda` = -38.
- The chain walk accepts only these struct types:
  - `0x3b9b3378` D3D11
  - `0x3b9b2ba8` Vulkan
  - `0x3b9b3760` D3D12
  - `0x3b9b23d8..db` OpenGL win32/xlib/xcb/wayland
  - `0x3b9b27c1` OpenGLES
  - `0x3b9b8584` EGL_MNDX
- A D3D11 binding whose `device` member is NULL also returns -38
  (`0x18006bc9b`: `cmp QWORD PTR [rdx+0x10],0x0`).

So the accepted set is closed and known, and a null device is as fatal as a
missing binding. Anything I chain must carry a real, live device pointer.

## What the game actually runs on

Measured from the game's own log, and it decides which binding struct to use:

- Default: `Graphics API: OpenGL`, `Compute shader support: false`. Teardown's
  lighting is compute-shader raycasting, so the GL path cannot render.
- With `options.xml` `gfxapi=1`: `Graphics API: D3D12`,
  `Compute shader support: true`, renders normally on the RTX 4070.
- The `0x887A0022 DXGI_ERROR_SWAP_CHAIN_NOT_STILL` failure seen earlier was a
  session-0 artifact (no desktop, no window), not a runtime problem. The game
  rewrites `options.xml` after a failed attempt, so re-check the flag after
  every launch.

Therefore: **D3D12 binding, `XrGraphicsBindingD3D12KHR`**, and the game must be
running with `gfxapi=1` in the same session it is injected into.

## The ordering rule

`xrCreateInstance` and `xrGetSystem` are device-free and both already succeed
early, on the DLL-load thread. `xrCreateSession` is not: it must be called where a
device exists. Calling it from the init thread can only ever produce -38, which is
exactly what happened. The fix is two changes together, not one:

1. `gfxapi=1` so the renderer actually creates a D3D12 device.
2. `xrCreateSession` from inside the renderer hook, with
   `XrGraphicsBindingD3D12KHR{device, commandQueue}` chained into `si.next`,
   ahead of the view-configuration properties.

Half of this without the other still fails, and the failure will look identical.

## Evidence that hooks and XR now coexist

First run with both live, same process, process alive:

```
[VR] present: vtable=00007ffe2da3c830 slot 9 @+48 real=00007ffe2d9650e0 -> HOOK LIVE
[VR] probe endRender #1:   vtable=00007ffe2da3c830  slot9=00007ffe2d9650e0  in_module=1  <- THIS IS THE SWAPCHAIN
[VR] OpenXR: xrCreateInstance returned 0
[VR] OpenXR: instance created at API 1.1.0
[VR] OpenXR: system 1152964986855752170  vendor=10462  tracking pos=1 orient=1
[VR] OpenXR: view config offered: 2
[VR] OpenXR: blend modes for view 2 -> XR_SUCCESS (n=1)
[VR] OpenXR: session flags=0x chain=no  -> XR_ERROR_GRAPHICS_DEVICE_INVALID
[VR] OpenXR: session flags=0x chain=yes -> XR_ERROR_GRAPHICS_DEVICE_INVALID
VERDICT: ALIVE
```

SteamVR, same second:

```
[Info] - New Connect message from D:\SteamLibrary\steamapps\common\Teardown\teardown.exe (VRApplication_OpenXRInstance) 8400
[Info] - Using existing HMD null.Null Serial Number
[Info] - Update ZeroPose for the chaperone universe 2
```

## Two blockers that were not blockers

**`TDVR_NO_XR` defaulted to 1.** `hook/bisect.h` disabled the whole OpenXR path
for every build, because runtime loading used to kill the process and a hook crash
could not be separated from a runtime crash. That reason expired when the null
driver was enabled: `xrCreateInstance` now returns `XR_SUCCESS` and the process
survives. Default is back to `0`; only the hook-bisect variants turn XR off, so a
hook crash is never confused with a runtime crash.

**The injector was never called correctly.** `injector.exe` takes options, not a
bare PID:

```
[!] unknown option: 6136
Usage: C:\tdvr\injector.exe [options]
  --attach   inject into an already-running teardown.exe
```

Every "injection produced no log" result in this project was this one argument
mistake. The correct call is `--attach --dll <path> --noquit`. The injector also
writes `tdvr_host.txt` next to *itself* (`C:\tdvr`) while the DLL reads it from
the *game* folder, so the copy is required; without it the DLL logs
`no host file at: tdvr_host.txt` and stops before resolving anything.

## Honest gap

A standalone probe could not confirm the binding end-to-end: launched outside
Steam it gets `-2` with `Not looking for a good app key because Steam didn't start
this app` / `VRInitError_Init_Internal`, and neither `SteamAppId=284160` nor
`steam_appid.txt` beside the exe satisfied it. The binding hypothesis is proven
by disassembly plus SteamVR's own log for a real Steam-launched app, not by a
successful session I created myself. One run of the real game with the binding in
`next` closes the loop.
