# XR resets the GPU at xrCreateInstance — bisect result 2026-09-29

## What was measured

Every crash is the same fault, and `error.yaml` names the cause verbatim:

```
message:  "GPU hung/removed/reset, HRESULT=887a0005"
          887a0005 = DXGI_ERROR_DEVICE_RESET
minidump: EXCEPTION_ACCESS_VIOLATION
          teardown.exe rva 0x4ffcc8  read=0x1 at=0x0
          openxr_loader.dll + openxr_simulator.dll loaded
```

`read=0x1 at=0x0` is the game dereferencing a device that is gone: the GPU
stopped answering, Windows reset it, and the next use of the device faults.
It is not our code — our DLL is not on the stack in any of these dumps.

## The bisect ladder

Each row is a separate build, same machine, same run length, one variable:

| # | Build flag | What runs | Result |
|---|---|---|---|
| 1 | `-DTDVR_NO_XR=1` | no loader at all | **ALIVE**, `grow3s=4.09s` |
| 2 | `-DTDVR_XR_RUNTIME_ONLY=1` | loader + runtime resident, no instance | **ALIVE**, `grow3s=1.62s` |
| 3 | `-DTDVR_XR_INSTANCE_ONLY=1` | `xrCreateInstance`, then stop | **CRASH** |
| 4 | `-DTDVR_XR_LAYER0=1` | full chain, `layerCount=0` | **CRASH** |
| 5 | (default) | full chain, `layerCount=1`, copy off | **CRASH** |
| 6 | (default, copy on) | full chain, `layerCount=1`, copy on | **CRASH** |

Rows 3–6 are the same fault at the same rva. Rows 4–6 being identical also
rules out the two things that were the obvious suspects:

- **the image copy** (`CopyResource` into runtime-owned swapchain images) —
  it was on in row 6 and off in row 5, same crash
- **the compositing path** — `layerCount=0` in row 4 hands the runtime nothing
  to draw, and it still crashes

## Conclusion

`xrCreateInstance` against OpenXR-Simulator 1.0.27 is enough on its own to
hang the GPU, in Teardown's process, when the instance is created from a thread
that is inside the game's D3D12 device. An idle runtime in the same process is
harmless; the moment an instance exists, the reset happens.

This narrows the fault to runtime-internal work triggered by instance
creation, not to anything the mod submits later. The most probable mechanism
is the runtime enumerating or initialising the graphics device the same way the
game already is — but that is a hypothesis, not a finding, and the honest
statement is that we know exactly which call does it and nothing more.

## What this rules out for later work

The projection layer, `xrEndFrame` on its own thread, the image copy, the
swapchains and the stable frame loop were all built and all verified working
before this bisect. They are not the cause and none of them need to change.
If the instance is ever created successfully, that machinery is ready.

## The switches

All four live in `hook/bisect.h`, documented in place, default 0 (feature on):

- `TDVR_XR_COPY_ENABLED` — copy backbuffer into XR swapchain images
- `TDVR_XR_LAYER0` — submit `layerCount=0` instead of a real layer
- `TDVR_XR_RUNTIME_ONLY` — load the loader, create nothing
- `TDVR_XR_INSTANCE_ONLY` — create the instance, stop before `xrGetSystem`
- `TDVR_NO_XR` — the original master switch, no OpenXR at all
