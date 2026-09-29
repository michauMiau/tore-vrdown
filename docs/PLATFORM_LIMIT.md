# Why the container test stops here — the game's own log

Date: 2026-09-26

Source of truth: `pfx/drive_c/users/steamuser/AppData/Local/Teardown/log.txt`

## 1. What the game actually does

```
[Render] Graphics API: D3D12
[Render] Failed to found any suitable GPU adapter
[Render] TRendererD3D12::createDeviceD3D12:2631
         FindAvaliableHwAdapter(...) return HRESULT=80004005
[Render] Initialization error!!!
[Render] Graphics API: OpenGL          <-- deliberate fallback
[Render] Vendor: Mesa / Renderer: llvmpipe (LLVM 19.1.7, 256 bits)
[Render] Total VRAM: 0 Mb
[Render] Compute shader support: false
[Render] Can't load shader cache, platform is wrong 'x64_dx12', expected 'x64_gl'
[Init]  Starting up in resolution: 1280x720
[Init]  CreateMasteringVoice failed
```

`80004005` is `E_FAIL`. D3D12 enumerates adapters through `vkd3d-proton`,
which surfaces the lavapipe CPU device; the game's own hardware check
rejects it and `createDeviceD3D12` fails outright. Teardown then falls back
to OpenGL by design, where Mesa hands it llvmpipe.

This reframes the earlier splash observation. The splash was never evidence
of a working renderer — the game painted it, then died on the next step,
with and without a DLL injected.

## 2. Why llvmpipe cannot finish the job

Two independent hard stops, either one fatal:

1. **`Compute shader support: false`** — Teardown's lighting is
   compute-shader raycasting, not hardware RT. With no compute support the
   deferred lighting pass has nothing to run on.
2. **`platform is wrong 'x64_dx12', expected 'x64_gl'`** — the shipped
   shader cache is compiled for the D3D12 backend. The OpenGL path must
   recompile from source, which on a CPU rasteriser is not viable.

`CreateMasteringVoice failed` is **separate** — the container has no audio
device (ALSA errors in every run's stderr). Not the cause of the renderer
failure, and not worth fixing here.

## 3. D3D12 is the real path, OpenGL is a fallback

String census in the unpacked binary:

```
RendererD3D12   29 occurrences   (beginRender, endRender, createDeviceD3D12, ...)
RendererOpenGL   3 occurrences   (partialFbo, clearFbo, ATI_meminfo)
```

29 vs 3. `TRendererOpenGL` has a full RTTI record (`.?AVTRendererOpenGL@@`)
but far fewer call sites — the compatibility path, not the shipping
renderer. The D3D12 analysis stays the right target regardless of which
backend a user runs.

## 4. What this rules out

- Forcing `gfxapi` in `options.xml` — it was already `1` and still landed on
  llvmpipe; there is no GPU to select.
- The DLL, detours, injector — all independently verified. Injection
  succeeds, hooks install at expected addresses, process survives.
  `beginRender` is never called because **no frame is rendered**.
- More CPU threads or RAM. The container reports 15886 Mb and 4 threads;
  neither is the constraint.

## What IS verified and matters

- The DLL loads into a live `teardown.exe` under Wine/Proton
- Both detours land on `base + 0x5AB9D0` and `base + 0x5AF270` across
  **different ASLR bases** (0x6ffffbb00000, 0x6ffffa570000, 0x6ffffa600000)
- The prologue steal is instruction-exact: `begin=18 end=16`
- Injection into a game already running 370 s causes no destabilisation
- The injector runs the full flow unattended: launch → wait for window →
  find PID → inject → exit 0
- Stereo off-axis maths is proven correct by `hook/verify_stereo.py`
  (`ALL CHECKS PASS`) — independently of any hardware

## Conclusion

Not a code problem, not a hook problem, not a Proton problem. The game
itself reports `E_FAIL` finding a GPU adapter. Every remaining test needs
the RTX 4070.
