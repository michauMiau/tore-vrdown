# Teardown VR — Reverse Engineering Findings

Analysis date: 2026-09-26
Binary: `Teardown/teardown.exe` (30,555,208 bytes, x86-64 PE32+)

## Renderer Architecture

Custom engine, class `TRendererD3D12` + `TRendererOpenGL` (both backends compiled in).

### Key classes / methods (from binary strings)

```
TRendererD3D12::createDeviceD3D12       - D3D12 device creation
TRendererD3D12::beginRender            - frame begin  <-- BEST HOOK POINT
TRendererD3D12::endRender              - frame end
TRendererD3D12::initFrameContexts      - per-frame context alloc
TRendererD3D12::createGPUMemAllocator  - GPU memory
TRendererD3D12::createSrvDescriptorHeaps
TRendererD3D12::createUploadResources
TRendererD3D12::resizeInternal
TRendererD3D12::mSwapChain / mCommandQueue / mCommandList / mFence

TSwapChainD3D12::initSwapChain
TSwapChainD3D12::initBackBuffers
TSwapChainD3D12::present               <-- classic Present hook
TSwapChainD3D12::resize
TSwapChainD3D12::getFullScreen
```

### Hook point recommendation

`Present` is the *late* hook — by then the frame is already composited to one eye.
`beginRender` is the *correct* hook: it runs before any per-eye matrix is set,
so we can inject stereo matrices before the G-buffer pass.

## Scene Buffers (HLSL embedded in binary as plaintext)

The engine embeds its **entire HLSL shader source as strings** in `teardown.exe`.
This gives us the exact uniform layout.

### SceneDynamicBuffer (slot 1) — per-frame, per-eye data
```hlsl
DECLARE_CONSTANT_BUFFER(SceneDynamicBuffer, 1)
    float4x4 mubVpMatrix;      // <-- view-projection matrix (THE hook target)
    float4 mubViewXDir;        // xyz - dir, w - unused
    float4 mubViewYDir;
    float4 mubProjectionData;  // used by computePixelDir()
```

`computePixelDir()` reconstructs the ray direction from these:
```hlsl
float3 computePixelDir(float2 texCoord)
    float2 screenCoord = texCoordToScreenCoord(texCoord).xy;
    return mubViewXDir.xyz * mad(screenCoord.x, mubProjectionData.r, mubProjectionData.z) +
           mubViewYDir.xyz * mad(screenCoord.y, mubProjectionData.g, mubProjectionData.w) -
           ...
```

**Implication:** for stereo we must write, per eye:
- `mubVpMatrix` = Projection(per-eye) * View(per-eye)
- `mubViewXDir`, `mubViewYDir` = per-eye basis vectors
- `mubProjectionData` = per-eye tan(fov/2) terms

### SceneUpdatableBuffer (slot 2)
```hlsl
DECLARE_CONSTANT_BUFFER(SceneUpdatableBuffer, 2)
    float4 mubPixelSize;        // rg - PixelSize, ba - 1/PixelSize
    float4 mubNativePixelSize;  // rg - after upscale
    float4 mubNearFar;          // r - Near, g - Far, b - 1/Far, a - (near-far)
```

### Other relevant uniforms
```
mubCameraPos, mubOutlineCameraPos     - camera positions (fog, outlines)
mubMvpMatrix, mubModelMatrix          - per-object
mubOldViewMatrix, mubOldStableVpMatrix - TAA reprojection (per-eye history!)
mubInvFar, mubIsoForward, mubNearFar
```

**`mubOldViewMatrix` / `mubOldStableVpMatrix` are the TAA problem:**
they hold the *previous frame's* matrix. In stereo these must be tracked
per-eye, or TAA smears/ghosts across the two eyes.

## TAA (Temporal Anti-Aliasing)

Confirmed compute-shader based:
```hlsl
DECLARE_CONSTANT_BUFFER(TemporalAADynamicBuffer, 5)
DECLARE_TEXTURE2D(uTaaOutput, 16);
#define kTemporalAaCsThreadX 16
#define kTemporalAaCsThreadY 16
float4 mubTemporalAAParams; // rg - offset, ba - PixelSize
float3 jitter = (blueNoise3(blueNoiseTc) - float3(0.5f, 0.5f, 0.5f));
```

Uses blue-noise jitter. Ping-pong FBOs: `mLinear16BitDepthFbo[0]/[1]`,
`mDisocclusionMaskFbo[0]/[1]`.

**Stereo TAA requirement:** per-eye history buffers. Either:
- (a) run TAA twice (once per eye, double cost), or
- (b) accept ghosting and disable TAA in VR, relying on MSAA/reprojection instead.

## Ray Tracing / Lighting

Compute-shader based (not hardware RT cores), as user correctly noted.
Ambient/visibility via `raycastShadowVolume`, `raycastShadowVolumeSparse`,
`raycastShadowVolumeSuperSparse`.

**Perf implication:** this is the dominant GPU cost. Stereo = 2x dispatch
unless we share visibility volumes between eyes (they're nearly identical
for small IPD — a good optimisation opportunity).

## No Native VR

Zero OpenXR / OpenVR / SteamVR strings in the binary. Nothing to disable,
nothing to enable — VR must be built from scratch.

## Lua Layer

- 73 game scripts in `data/script/*.lua` — **plaintext, unencrypted**
- Built-in scripts in `data/built-in/script/`
- Game logic (campaign, challenges) is Lua; engine is binary
- Campaign scripts use only `GetPlayerCameraTransform` — no direct render access
- `SetCameraTransform`, `SetCameraFov`, `SetCameraDof`, `RequestFirstPerson` exist
  in the public API → enough for **head-tracked camera** without touching the renderer

## Other findings

- `steam_emu.ini` + cracked `steam_api64.dll` present (no-Steam patch)
- `sentry.dll` for error reporting
- `mimalloc.dll` — custom allocator
- `dxcompiler.dll` / `dxil.dll` — DXIL shader compilation at runtime
  → **shaders are compiled at runtime from the embedded HLSL**; we can
    potentially patch shader source in memory
- `WinPixEventRuntime.dll` — PIX event runtime (profiling hook possible)
