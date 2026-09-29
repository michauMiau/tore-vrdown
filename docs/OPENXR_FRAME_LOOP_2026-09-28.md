# OpenXR frame loop works in Teardown (2026-09-28)

Measured end to end against the running game. No headset, no Steam, nothing
installed by the user. The chain below is quoted from the real log, not
described from the spec.

## What runs

`vr_bisectA.log`, Teardown on the VM at 192.168.1.6, simulator active through
`HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` = `C:\tdvr\openxr_simulator.json`.

    [VR] OpenXR: xrCreateInstance returned 0
    [VR] OpenXR: instance created at API 1.1.0
    [VR] OpenXR: system 1  vendor=0  tracking pos=1 orient=1
    [VR] OpenXR: view config offered: 2
    [VR] OpenXR: instance+system ready, session deferred to the render thread
    [VR] OpenXR: swapchain GetDevice(slot 7) -> hr=0x00000000 dev=...
    [VR] OpenXR: command queue on the renderer's device -> hr=0x00000000 queue=...
    [VR] OpenXR: graphics requirements minFeatureLevel=0xB000
    [VR] OpenXR: SESSION CREATED (was impossible from the load thread)
    [VR] OpenXR: LOCAL space created -- HMD pose is now readable
    [VR] OpenXR: session is now in state 2 (was 0)
    [VR] OpenXR: xrBeginSession -> 0 (from READY)
    [VR] OpenXR: session is now in state 3 (was 2)
    [VR] OpenXR: session is now in state 4 (was 3)
    [VR] OpenXR: session is now in state 5 (was 4)      <-- FOCUSED
    [VR] OpenXR: loop waits=31 ends=31 ready=1 err=0 tracked=1 pose=(0.000 0.000 0.000)

Stable for more than a minute: `waits` and `ends` climb together, `err=0`,
the local space is readable every frame, the process stays alive at ~1.7 GB.

## The three things that actually blocked it

### 1. The active runtime is not a loader

`openxr_simulator.dll` exports `xrNegotiateLoaderRuntimeInterface` and the
whole `xr*` API, but it does **not** export `xrGetInstanceProcAddr`. Loading it
directly and asking it for `xrGetInstanceProcAddr` fails, and the hook logged
`MISSING` and detached after 0 frames. What is needed is the Khronos
**loader**, which reads the active manifest and does the `dlopen` itself.

### 2. The SteamVR loader kills the process

Loading `SteamVR\bin\win64\openxr_loader.dll` inside the game terminates it at
`LoadLibrary` -- no error code, log stops after three lines. The Meta runtime
does the same. **Our own loader does not**, because it is self-contained: no
vrserver, no driver, nothing Steam. That is the whole difference, and it is why
`hook/teardown_vr.c` must try `C:\tdvr\openxr_loader.dll` **before** the SteamVR
paths. Reversing that order reinstates the crash.

### 3. xrBeginSession is what moves the session out of READY

A session that reaches `READY` and never gets `xrBeginSession` stays in state 2
forever, no matter how many frames the app submits. Frames were being counted
(`waits=21 ends=21`) while the runtime was never actually told the session had
started. Adding the call walks the full machine:

    IDLE(0) -> READY(2) -> SYNCHRONIZING(3) -> VISIBLE(4) -> FOCUSED(5)

The 5 -> 4 -> 5 wobble afterwards is the simulator toggling its preview window,
not a fault.

## Building our own loader

From `KhronosGroup/OpenXR-SDK`, with MinGW-w64:

    cmake .. -DCMAKE_SYSTEM_NAME=Windows \
      -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
      -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
      -DCMAKE_BUILD_TYPE=Release -DDYNAMIC_LOADER=ON -DBUILD_TESTS=OFF \
      -DCMAKE_SHARED_LINKER_FLAGS="-static -static-libgcc -static-libstdc++"

Two changes were needed:

* `src/loader/CMakeLists.txt` turns `-Werror=undef` into `-Wno-error=undef`
  under `if(MINGW)`. MinGW does not define `WINAPI_PARTITION_SYSTEM`, which is
  MSVC-only, so `xr_dependencies.h` trips the warning on every file. It is
  applied via `target_compile_options`, so setting `CMAKE_CXX_FLAGS` does not
  override it.
* The static link flags are mandatory, not cosmetic: without them the DLL
  imports `libwinpthread-1.dll`, `libgcc_s_seh-1.dll` and `libstdc++-6.dll`,
  which are not on the box, and `LoadLibrary` fails with error 126.

Result: 3.7 MB, 56 exports, loads and runs.

## Memory

The process grows from ~620 MB to ~2 GB during the first half-minute and then
holds steady. It is not a leak: our log is 93 KB, the simulator's is 8 KB, and
thread and handle counts are flat. First run without the frame loop sat at
~570 MB, so the growth is the runtime's own allocations once frames start.

## Left to do

* `stereo_apply(eye)` and a second XR image. Neither is in the game yet; the
  loop currently only submits frames.
* The D3D12 device still cannot be created by a plain client on this box:
  `D3D12CreateDevice` returns `E_NOTIMPL` on every adapter. It does not matter
  for the hook, because the device comes from the game's own swapchain, but it
  does mean a standalone test client cannot get past `xrCreateSession`.
