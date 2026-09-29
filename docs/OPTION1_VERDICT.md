# Option 1 was tested. Here is the honest result and the decision.

## What option 1 needed

Intercept the CPU code that fills the camera data before it reaches the GPU
descriptor. For a D3D12 deferred renderer that means finding the call that
writes `mubVpMatrix` and `mubVpInvMatrix` into the constant buffer the shader
reads.

## What the shader source proves

The HLSL is embedded in the binary and is unambiguous:

```
gl_Position = mubVpMatrix * vec4(aPosition, 1.0);
```

and in the geometry raymarcher:

```
float3 precalc = plane.w * float3(mubVpMatrix[0][3], mubVpMatrix[1][3], mubVpMatrix[2][3]);
```

`mub` is the engine's global material uniform buffer. It is a `cbuffer` bound
through a descriptor, and a descriptor points at GPU memory. The CPU never
holds a readable copy of it in the renderer object, which is consistent with
every CPU read in this project failing to find a matrix.

## What the search produced

| lead | result |
|---|---|
| `mubVpMatrix` string at file 0xBFAD8C | inside `.dataa`, shader source blob, not a code reference |
| `SceneDynamicBuffer` at 0x99FA82 | inside `.rdata`, a name table, zero rip-relative references to it |
| `createStructuredBuffer(..., 0x1F4)` | 94 occurrences of the 0x1F4 constant in `.text`, no way to pick the right one without tracing each |
| paired `lea rdx` / `lea r9` at 0x9AD29 | turns out to be lighting maths, `sqrtss` / `divss` on coordinates, a loop with `add rdi,0x600` |

The `0x1F4` search is the honest blocker. 94 candidates, each needing its
surrounding code read to see whether it allocates a constant buffer or
something unrelated. That is several hours of reading with no guarantee,
against a 2 to 3 day budget already partly spent.

One tooling note worth keeping: `find_riprefs.py` was initially computing
`i + 7 + disp` on a file offset instead of an RVA, so every reported target was
off by `0x400`. `.text` is `raw 0x400`, `vaddr 0x1000`. The corrected
relation is `rva = i - 0x400 + 0x1000`. Before the fix it reported 86 hits
into the name table; after it, zero, which is the correct answer, because a
`std::string` table is not referenced by rip-relative addressing.

## Decision

Option 3, using existing ReShade, not writing it.

ReShade already wraps the swapchain, already supports DX12, and already has a
VR framework. Writing a DX12 swapchain proxy from scratch would mean
implementing multisample resolve, resource barriers, a resize path, and
command queue handling correctly, which is a large project on its own and is
the kind of work that produces a crash loop rather than a working headset.

The work in this repository is not wasted and it is not thrown away. What it
established:

- a working vtable-hook injection chain with no byte-stealing, proven stable
  across long sessions
- a verified `IDXGISwapChain::Present` hook, 25 800 calls with the game alive
- a correct single-dereference vtable validator, with an offline test for it
- a read-only memory prober that works without injecting
- a hard-refuted list of six offsets that were previously written down as fact

The Present hook is exactly the seam ReShade-style wrapping needs. The mod
becomes the thing that drives the render loop and the haptic layer, and the
stereo implementation is supplied by a component that already solves the
DX12 wrapper problem.

## The rule this project keeps relearning

Eight times now, an offset or a layout was written down from a comment, a
string reference, or a habit, and treated as fact. Eight times a real
measurement contradicted it. The next one is the SDB field, and the cost of
finding it by brute force exceeds the value, so it goes in the refuted list
with the reason attached.
