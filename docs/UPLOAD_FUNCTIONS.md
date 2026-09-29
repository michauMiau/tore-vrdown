# Upload paths, 2026-09-27 wieczór

Method: intersect functions that call `UpdateSubresources`/`UpdateSubresources2`
(ID3D12Resource vtable slots 0x128/0x130) with functions touching renderer+0xB90
(the SceneDynamicBuffer resource). Boundaries from .pdata, not backwards scanning.

## The result

    .pdata functions: 35710
    instructions decoded: 2172628

    A) functions calling UpdateSubresources/2 : 20
    B) functions referencing +0xB90           : 0
    A and B                                : 0

Zero. Not "few" — zero. `find_sdb_writer.py` on the same image does find 8 refs
to +0xB80 and 1 to +0xB90 (in 0x9E130), so the scanner sees the field. The
conclusion is structural: **+0xB90 is never read by a direct
`mov reg,[reg+0xB90]`.** The SceneDynamicBuffer is reached through a pointer —
the renderer keeps buffers in an array/struct and the uploader receives
`ID3D12Resource*` as an argument.

So the search stops being "find the field in the renderer" and becomes
"find which of the 20 uploaders is on the per-frame path".

## The 20 uploaders, with verdicts

Ruled out by inspection:

| fn | verdict |
|---|---|
| 0x5B1B70 | `SetRenderTarget(slot, target, flags)` — writes `[rax+0x10] = 2/3/4` (D3D12_RT), walks `+0x24A*8`, reads `+0xE40` swapchain. Render-target binding, not an upload. |
| 0x5BB820 | device-lost handler — walks the DXGI chain, `GetDebugLayerInterface`, sets `[rbx+0x68] = removed`. Not on the frame path. |

Note 0x5B1B70 is adjacent to beginRender (0x5B35A0) and does read
`[rdi+0xE40]`, which independently confirms the swapchain offset.

Still open, most likely first:

| fn | note |
|---|---|
| 0x87F40..0x089D6D | ~8 KB, the largest uploader. Most likely to contain a real frame path. |
| 0x5B1CB0 | `UpdateSubresources2` with size 0xFC0 (4032 B). Not the 500-byte SDB. |
| 0x900B0..0x0907C4 | 1.8 KB |
| 0x71CE0..0x074927 | 11 KB |

## Why the field reference approach is now wrong

A)∩B) = 0 kills the hypothesis that motivated the script. Keep A), drop B), and
identify the per-frame one by the call chain from beginRender instead —
which is what the .pdata bounds now make possible: every call target can be
resolved to a real function entry, which is what the crashed trace got wrong.
