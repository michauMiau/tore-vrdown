# Locating the CPU-side SceneDynamicBuffer — findings

Date: 2026-09-26. Build analysed: `/root/steamless/Teardown/teardown.exe`
(unpacked Steamless repack, `.text` entropy 6.474).

## What we know from the GPU side

Extracted from the embedded HLSL, `SceneDynamicBuffer` is register space
`b1` and contains at least:

```hlsl
float4x4 mubVpMatrix;        // view-projection
float4   mubViewXDir;        // camera right
float4   mubViewYDir;        // camera up
float4   mubProjectionData;  // projection params
```

Plus `mubOldViewMatrix` referenced by the TAA history resolve.

## The search, and why it failed

Approach 1 — **find the fill function.** Scanned all 35 675 `.pdata`
functions for float-store density. 1 880 functions have >= 8 float stores;
the top 110 candidates are all generic matrix/particle code, none identifiable
as the per-frame scene upload.

Approach 2 — **find a 4x16-byte matrix store.** Scanned every
`movups`/`movaps` with a displacement, grouped by function + base register,
looking for four stores at stride 16. 49 functions matched across the whole
binary; **only 2 fall inside the renderer range 0x580000-0x5C0000**
(`0x582AA0`, `0x58A560`). Both turn out to be IAT/vtable dispatch, not struct
fill.

Approach 3 — **follow the upload call from `endRender`.** The
`mov r8d, 0x800` / `cmp edi, 0x8007000e` pair in `endRender` is a
D3D12 debug-layer constant, not a buffer size I could pin to the scene buffer.
The function at `0x5B3280` has 12 callers and is a utility, not the scene
upload.

## The real obstacle

```
0x5AB9D0 (beginRender) called from 0 sites
0x5AF270 (endRender)  called from 0 sites
```

Both are **virtual** — invoked through vtable dispatch, not direct `E8` calls.
So the natural "find the caller, read what it passes" chain simply does not
exist in the static binary. The pointers at RVA `0xA831B0` / `0xA831B8` are
RTTI-style metadata (neighbouring slots hold values like `0x400000001`), not a
plain vtable, and they have zero rip-relative xrefs.

## Conclusion: stop guessing, start measuring

Static analysis has hit its limit. There are two ways forward, and the second
is strictly better:

1. **Keep guessing offsets** — expensive, low confidence, and we already
   burned several passes on it.

2. **Dump the buffer at runtime.** In the `beginRender` hook, walk the
   renderer's constant-buffer upload and find the 256-byte block whose first
   64 bytes are a plausible VP matrix (last row ≈ `0, 0, 0, 1` for a
   standard projection, orthonormable right/up vectors). That locates the
   struct exactly, with no guessing.

Option 2 needs a live renderer, which needs the RTX 4070. Everything that
blocks it is a hardware limit, not a code limit — see `docs/PROTON_TEST.md`.

## Next step on the gaming PC

Add a scanner to the hook: on the first N frames after `beginRender`, dump
candidate 256-byte blocks and log the base address of the one that validates
as a VP matrix. Then `VR_SetSceneBuffer(base)` points at it, and stereo can
start writing per-eye matrices.

Until that address is known, stereo stays disabled and the game plays flat —
the intended fail-safe.
