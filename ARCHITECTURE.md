# Architecture

## What the mod is

One DLL, `teardown_vr.dll`, loaded into a running `teardown.exe`. It patches three
vtable entries and does nothing else to the game's memory. The intent is to take
over the frame and render it twice with a different camera per eye.

Teardown is closed source and ships packed, so every offset in this project
either came from a decrypted repack, from a live-process measurement, or from a
scan. The `TRendererD3D12` vtable is resolved through RTTI, which lives in
`.rdata` and is not encrypted even in the protected build (measured entropy
5.399 against 8.000 for `.text`). A game patch moves the code and RTTI still
finds it.

## The three hooks

| Hook | How it is found | Verified |
|---|---|---|
| `beginRender` | RTTI, `TRendererD3D12` vtable slot 2 | yes, per-frame, live |
| `endRender` | RTTI, `TRendererD3D12` vtable slot 3 | yes, per-frame, live |
| `IDXGISwapChain::Present` | vtable slot 9, at `renderer+0xE40` | yes, 25 800 calls in v57 |

`hooked_begin_render` and `hooked_end_render` do nothing but count, probe and
log. `hooked_end_render` is also the only place the Present install runs, which
matters and is explained below.

The vtable for the renderer is found by walking the Complete Object Locator to
`".?AVTRendererD3D12@@"`. Slot indices are 2 and 3. A hardcoded RVA fallback
exists for when RTTI resolution fails, and it logs loudly that it will break on
the next patch.

## Vtable hooks, and why not byte-stealing

A vtable entry is a pointer. Writing it needs no instruction-length decoder, no
stolen prologue and no trampoline, so there is no way to leave a
half-overwritten instruction behind.

Byte-stealing is the other option: overwrite the first N bytes of a function
with a jump, and put the displaced bytes in a trampoline so the original still
runs. It works when the stolen length lands exactly on an instruction boundary.
`build_detour` in `hook/teardown_vr.c` does implement it and it produces a
correct trampoline; `build/test_detour` verifies the jump layout.

## Byte-stealing is banned

It is not a style preference. Builds v38 through v52 all died the same way, and
the cause is documented in `docs/ROOT_CAUSE_BYTE_STEALING.md`.

| Build | Vtable hooks | Byte-stealing detours | Result |
|---|---|---|---|
| v52 | yes | 2, stealing 5 and 15 bytes | froze 10 to 20 s after injection, mid-level |
| v53 | yes | none | 12 840 frames, no crash dialog |

The two detours were `tdvr_patch_direct_call` on the SceneDynamicBuffer bind
call site, stealing 5 bytes, and `td_install_upload_hook` on the upload
consumer at RVA `0x5BAEB0`, stealing 15 bytes. Both rewrite the instruction
stream of a function that is actively running.

The failure is not an access violation. A stolen length that does not end on an
instruction boundary turns a prologue into a different instruction sequence, and
that sequence runs for a while before it goes somewhere invalid. The signature
is a game that freezes, a Sentry dialog, and no faulting address and no minidump.

The cost of diagnosing that was higher than the feature was worth. Rule: if a
function has to be intercepted, patch a vtable entry or an import, or use a
hardware breakpoint. `build_detour` stays compiled in and tested, but
`install_hooks` installs no detour. Where a detour used to be installed, the
code now logs that it is disabled and why.

## Renderer object offsets

These come from disassembly of the decrypted repack and from live memory dumps.
Only the first four are load-bearing.

| Offset | Type | Meaning | Confidence |
|---|---|---|---|
| `+0xE40` | ptr | `IDXGISwapChain*` | verified for the `endRender` object only |
| `+0xE58` | ptr | `ID3D12CommandQueue*` | from disassembly |
| `+0xEF8` | ptr | frame context array, stride `0x30` | from disassembly |
| `+0xB90` | ptr | SceneDynamicBuffer, engine's own resource type | verified from disassembly |
| `+0xDA8` | int32 | frame index | from disassembly |
| `+0x1130` | struct | unknown | **not** the matrix ring. See below |

`renderer+0xE40` was read from the object `beginRender` receives in v54 and came
back as scene data, whose slot 9 read as `0x234800000238c181`. A `0x23` in the
top bits is a `float4` exponent, not an address. The same offset read from the
object `endRender` receives gives a real swapchain. The offset was never wrong,
the caller was. The install now lives in `hooked_end_render` only, after a
probe confirms the value is a swapchain.

`renderer+0x1130` is not a ring of 0x40-byte matrix slots, and
`renderer+0x1128` is not a slot count. 2048 bytes read from `+0x1130` contained
pointers reinterpreted as floats, values around `1e25`, and ASCII in the
neighbouring words. A scan of roughly 2 GB of the live process found no matrix
ring. A night-log note claims otherwise; that note is wrong, and it is left in
place with a correction at the top.

## The Present hook, and the double dereference

`Present` is vtable slot 9, byte offset `0x48`, on `IDXGISwapChain` and on
`IDXGISwapChain1/2/3` which inherit it. The call site was confirmed by
disassembly: 47 sites in `.text` call `qword ptr [rax+0x48]`, and exactly one is
fed by a member load of the same object.

The install was blocked for four builds (v54 to v57) by a bug in the validator
rather than in the install. The validator did two dereferences:

```c
// before
if (!td_read((uint64_t)(uintptr_t)obj, &vt, sizeof vt) || !vt) return NULL;
if (!td_read((uint64_t)(uintptr_t)vt,  &vt, sizeof vt)) return NULL;   // one step too far

// after
void** vt = NULL;
if (!td_read((uint64_t)(uintptr_t)obj, &vt, sizeof vt) || !vt) return NULL;
```

An `IDXGISwapChain`'s first word is already the pointer to its vtable. One
dereference gives the vtable. The second read treated the vtable as an object,
read its first word, which is interface data rather than a function pointer,
and computed "slot 9" relative to that. `td_install_present` was already
correct, so a good swapchain was refused on every single attempt.

What found it was a second, deliberately naive probe reading the same object at
the same moment, printing both verdicts on adjacent lines of the same log. When
a validator rejects something a direct measurement confirms is valid, the
validator is the bug. `build/test_present_patch` now covers the install path
under AddressSanitizer.

## Reading game memory

`td_read` in `hook/teardown_vr.c` is the only way this project reads the game's
address space. It replaced two earlier attempts that both caused crashes.

**VirtualQuery on the start address.** The API describes only the region
containing the start address, not the whole 4 KB, so a pointer 100 bytes before
the end of a committed region passed the check and the following `memcpy` read
past it. v24 died on its first frame after the walk reported 7454 candidate
pointers, several of them at the very end of their regions.

**VirtualQuery region by region.** Fixed the torn read. v25 still died, after
14 944 frames rather than immediately, which is the signature of a race: the
walk confirmed a page was readable a few microseconds before the render thread
released it. A thousand frames a second is a thousand chances.

**What replaced it.** `ReadProcessMemory` on our own process, using the
`GetCurrentProcess()` pseudo-handle. No privileges needed, no `OpenProcess`. The
kernel probes the whole range atomically with respect to the caller and returns
FALSE for an unreadable page, so a race produces a short read instead of a
fault. A `VirtualQuery` walk is kept as a fast path that rejects most candidates
without a syscall. Every byte still goes through the kernel, so the fast path is
an optimisation and never the thing that makes a read safe.

SEH would also have caught the fault, but this toolchain cannot express it.
mingw's `excpt.h` gives `__try1` and `__except1` and no `__try` or `__except`,
and gcc 14 posix has no `-fseh-exceptions`.

## The SceneDynamicBuffer

This is the buffer stereo needs, because it holds the camera matrices the GPU
receives each frame. It is not found yet.

What is known. The function at RVA `0x087F40` is the only one in the whole
33 MB image, 35 710 functions, that reads `renderer+0xB90`, and it is
per-frame. Each read goes through `0x5DF000`, which reads a type tag at offset 0
and branches on 4 for texture and 8 for buffer. The descriptor it fills is 24
bytes: a resource pointer, an offset, a size and a type. So the engine has its
own abstract resource type and `+0xB90` points at one of those, not at an
`ID3D12Resource`. That is why searching for `mov rdx,[reg+0xB90]` next to a D3D12
call never matched anything.

The call chain, from `docs/SDB_BREAKTHROUGH_RESOURCE_TYPE.md`:

```
0x071CE0..0x074927   the UpdateSubresources caller
  0x071102           call 0x087F40          7725 bytes, 178 calls
    0x08869E         call 0x9E130          builds and uploads the 500-byte SDB
    0x088788         call [rax+0x128]      UpdateSubresources, +0xB98, r9d=0x30
    0x0887A5         call 0x5DF000         binds the SDB to the render slot
```

`0x9E130` is not called per frame. Two live hooks proved that, 0 hits over 6600
and 11 760 frames. Hooking it anyway was worse than not hooking it, because it
produced a green build, a healthy process, and no stereo.

`hook/memscan.h` is the current approach and is read-only: walk what the renderer
already points at and recognise a 500-byte buffer by the shape of its contents.
A candidate needs a plausible projection at `+0x000`, a `pd` vector at `+0x070`,
and, at `+0x140`, a `mubOldStableVpMatrix` that is very close to
`mubStableVpMatrix` at `+0x100`, because both are temporally filtered. That last
test is the strong one. `build/test_memscan` throws 20 000 random 500-byte blocks
at it and gets 0 matches.

## OpenXR

`xr_try_load` calls `xrNegotiateLoaderApiInterface` and gives up. No loader is
installed on the test machine, so `g_xr_ready` stays 0 and the mod renders flat.
`openxr/` is empty.

## Exports

```
VR_Enable(int)          VR_IsEnabled()        VR_SetIPD(float mm)
VR_GetFrameCount()      VR_GetRenderer()      VR_GetSwapchain()
VR_IsXRReady()          VR_GetSceneBuffer()
```

`VR_Enable` is honest: with no SceneDynamicBuffer address there is no stereo
path, and the mod is a frame counter plus a Present hook. The old `docs/BUILD.md`
lists a `VR_HookSwapchain` and a `VR_SetSceneBuffer` export that do not exist in
`teardown_vr.c`. Treat that document as history.

## DllMain

Strictly minimal. It calls `DisableThreadLibraryCalls` and starts one thread.
That thread writes the build stamp, tries OpenXR, waits up to 10 s for the
injector to hand over the image base through `tdvr_host.txt`, then installs
hooks. Nothing that takes the loader lock is called from `DllMain`, which rules
out the whole CRT.
