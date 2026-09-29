# Roadmap

Facts first, then assumptions, in that order. Anything not in the verified
section is a guess and should be treated as one, including by the person who
wrote it.

## Verified

Each of these was measured on a live process or on disassembly of a decrypted
image, and the measurement is reproducible.

| Fact | Evidence |
|---|---|
| The Present vtable hook works, installed by writing slot 9 (`+0x48`) | v57, 25 800 Present calls, game alive, no code bytes touched |
| Removing both byte-stealing detours made the build stable | v52 froze 10 to 20 s after injection; v53 ran 12 840 frames, 3+ min in a level, no dialog |
| `beginRender` and `endRender` are both per-frame and receive the same object | v53 log: `beginRender 4320 (self=0000025c55107af0)`, `endRender 4440 (self=0000025c55107af0)` |
| `renderer+0xE40` is the `IDXGISwapChain*`, for the object `endRender` receives | 47 sites call `[rax+0x48]`, exactly one fed by a member load of the same object. Reading it from `beginRender`'s object gives scene data instead |
| Present is vtable slot 9 on `IDXGISwapChain` and 1/2/3 | inheritance, and the call site disassembly |
| A swapchain's first word is its vtable pointer. Exactly one dereference | four builds, v54 to v57, refused a good swapchain because the validator did two |
| The SceneDynamicBuffer resource is the engine's own abstract type, not `ID3D12Resource` | `0x5DF000` reads a type tag at offset 0 and fills a 24-byte descriptor: pointer, offset, size, type |
| `0x087F40` is the only function in the image that reads `renderer+0xB90`, and it is per-frame | 33 MB image, 35 710 functions from `.pdata` |
| `0x9E130` is not called per frame | two live hooks, 0 hits over 6600 and 11 760 frames |
| RTTI resolution survives a game patch where hardcoded RVAs do not | vtable RVA moved `0xA859A0` to `0xA81D80` and beginRender moved by over 4 MB; slots moved a few hundred bytes |
| `.rdata` is not encrypted even in the protected build | entropy 5.399 against 8.000 for `.text` |
| The Steamless repack ships decrypted `.text` | entropy 6.474 against 8.000 |
| The stereo maths is correct, checked independently of the engine | `teardown-analysis/verify_stereo_independent.py`, 100+ assertions, both matrix conventions plus the engine path. See `docs/STEREO_MATH_VERIFIED.md` |
| Haptics are native to the game, 57 XML files, format understood, parser located | `docs/HAPTICS_FOUND.md` |
| The offset path for the eye is `mubProjectionData.z`, not the matrix | confirmed from HLSL and from CPU values. Simpler and safer than the matrix |
| The build is clean | injector imports `KERNEL32.dll`, `msvcrt.dll`, `USER32.dll` only |
| The 500-byte SDB layout: 5 identity `float4x4`, `pd` at `+0x70`, `pd.z` is a raw world offset in metres | confirmed from CPU |
| The injector is safe at the warning splash | hooks install, game keeps running |

## Refuted

Do not build on these. Several documents in `docs/` still assert them.

| Claim | What is actually true | Where it went wrong |
|---|---|---|
| Byte-stealing detours are safe | They killed v38 to v52 with a delayed freeze and no dump | `docs/ROOT_CAUSE_BYTE_STEALING.md` is the correction |
| `renderer+0x1130` is a ring of 0x40-byte matrix slots, count at `+0x1128` | 2048 bytes from `+0x1130` were pointers-as-floats around `1e25` plus ASCII. A ~2 GB live scan found no matrix ring | `docs/NIGHT_LOG_2026-09-27.md`, corrected at the top of that file |
| `renderer+0xE40` is the swapchain, unqualified | Only for the object `endRender` receives | `docs/PRESENT_HOOK_DOUBLE_DEREF.md` |
| The SceneDynamicBuffer is an `ID3D12Resource*` | The engine has its own abstract resource type | `docs/SDB_BREAKTHROUGH_RESOURCE_TYPE.md` |
| The frame hooks were not the per-frame path | They are. v50 to v52 read `beginRender 1` because the process was dying too fast to count | `docs/ROOT_CAUSE_BYTE_STEALING.md` |
| `0x9E130` is the per-frame SDB writer | 0 hits in two live runs. `0x087F40` is the per-frame pass | `docs/SDB_OWNER.md` |
| Callee RVAs from `beginRender` are function entries | They were call-site addresses. Three of twenty "functions" were the middle of other code, one of them `mov [rcx+4],edx; ret; padding` | `docs/STATE_2026-09-27-evening.md` |
| The image chunks are in address order | The filenames are hex, sorting them as text put a 16 MB chunk before a 2 MB one, and 30 scripts read zeros where `.text` was | `docs/IMAGE_SORT_BUG.md` |
| Exports include `VR_HookSwapchain` and `VR_SetSceneBuffer` | Neither exists in `teardown_vr.c` | `docs/BUILD.md` is history |

## Assumed, not verified

Everything here is a hypothesis. Each names the test that would settle it.

**The SceneDynamicBuffer is only on the GPU.** The `0x087F40` path builds a
descriptor and calls through an internal uploader, so a live CPU copy may never
exist between frames. Settle it by reading `renderer+0xB90`'s resource and
walking its fields from inside `hooked_end_render`, where the per-frame caller
is guaranteed to run.

**Stereo needs the frame to run twice.** The hooks are per-frame now, but
nothing has been observed to drive them twice. Until a second call per display
frame is measured, the mechanism for two eyes is unproven.

**The eye index can be alternated per Present call.** That is true if the game
draws one eye per Present. If it draws both into one Present, alternating
produces the wrong image rather than an obvious failure. Settle it by logging
the frame context contents on consecutive Present calls.

**Offscreen render targets, one per eye.** Assumed from how the engine binds
targets at `+0x24A*8`, never observed in stereo.

**`openxr/` will be filled in.** The directory is empty, there is no loader on
the test machine, and `xr_try_load` currently just fails. A runtime has to be
obtained and loaded before any of the stereo maths reaches a headset.

**Haptics interception point.** The format and the parser are found, the
interception point is not. See `docs/HAPTICS_FOUND.md`.

**`renderer+0x1130` is a string and ID table.** The bytes support it, the
consumer is not identified. `0x5BAEB0` was assumed to be the uploader and is
not, so whatever consumes `+0x1130` is still unknown.

**Lighting cost doubles with two eyes.** Compute-shader raycasts dominate.
Unmeasured.

**The Lua bridge works.** `lua_mod/teardown_vr.lua` calls `GetRegistryFloat` on
`teardown_vr/input/left/x` and three siblings. Nothing writes those keys.

## Next steps, in the order that resolves the most

1. Find the CPU-side SceneDynamicBuffer, or prove there isn't one. Everything
   else depends on it. Read-only, from `hooked_end_render`.
2. Establish whether the engine can be driven twice per frame at all, by
   counting calls with a frame-context dump on each. This decides whether the
   per-eye design is viable at all.
3. If yes, write one per-eye projection using `mubProjectionData.z` rather than
   the matrix. The path is confirmed and it is simpler.
4. Obtain an OpenXR loader and prove `xr_try_load` succeeds. Nothing reaches a
   headset before this.
5. Haptics interception, then controllers, then the Lua bridge.

Every step must be a vtable patch, an import patch, or a read. No byte-stealing,
permanently. The reasoning is in ARCHITECTURE.md and
`docs/ROOT_CAUSE_BYTE_STEALING.md`.

## Documents, and how much to trust them

`docs/` holds 59 documents, 197 KB, written during the session. They contradict
each other wherever the understanding changed, and the newer document is not
always the one with the better claim. Read the header of each one and check its
date. The refuted table above lists the specific claims that are wrong.

The three worth reading in full first:

- `docs/ROOT_CAUSE_BYTE_STEALING.md`. Why the ban exists, and why the liveness
  checks that hid it for 15 builds were themselves wrong.
- `docs/PRESENT_HOOK_DOUBLE_DEREF.md`. A validator that refused a good object
  for four builds because it was more careful than the thing it was checking.
- `docs/NIGHT_LOG_2026-09-27.md`. The measurement lessons, and the test that
  failed three times for reasons unrelated to the code under test.

`docs/RE_MATRIX_PATH.md` and `docs/HAPTICS_DESIGN.md` were being written when
this roadmap was assembled and are not present. If they appear later, treat them
as unverified until checked against the tables above.
