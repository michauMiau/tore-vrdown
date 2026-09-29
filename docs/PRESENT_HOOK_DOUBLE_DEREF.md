# The double-dereference that blocked the Present hook (v54 → v57)

## What happened

v54 was the first build to try hooking `Present` through the swapchain's own
vtable instead of detouring code. It failed, and the log looked like the offset
was wrong:

```
[VR] present: reject slot9=234800000238c181 module=not in any loaded module
[VR] present: refusing to hook 0000023e5c019650 - td_find_present rejected it
```

`0x234800000238c181` is not an address. `0x23` in the top bits is a `float4`
exponent, so this is shader or scene data being read as if it were a pointer.

## The measurement that exposed it

v55 added a probe that deliberately did the simplest possible thing: read
`self+0xE40`, then read that object's first word, then read slot 9. It reported:

```
probe endRender #1: self=000001cf4a03b3b0  +0xE40 -> 000001cf4a2d2260
probe endRender #1:   vtable=00007ffe62d7c830  slot9=00007ffe62ca50e0
                      in_module=1   <- THIS IS THE SWAPCHAIN
```

Same object, same moment, two opposite verdicts, on adjacent lines of the same
log. The probe did one dereference. The install path did two.

## The bug

```c
// td_find_present, before
void** vt;
if (!td_read((uint64_t)(uintptr_t)obj, &vt, sizeof vt) || !vt) return NULL;
if (!td_read((uint64_t)(uintptr_t)vt,  &vt, sizeof vt)) return NULL;   // one step too far
//                                                                    slot 9 of vtable[0]

// td_find_present, after
void** vt = NULL;
if (!td_read((uint64_t)(uintptr_t)obj, &vt, sizeof vt) || !vt) return NULL;
```

An `IDXGISwapChain` is a C++ object whose **first word is already the pointer to
its vtable**. One dereference gives the vtable. The second read treated the
vtable itself as an object, read its first word — vtable slot 0 — and then
computed "slot 9" relative to that. For a D3D12 vtable, slot 0 is interface
data rather than a function pointer, which is exactly the `0x2348...` pattern
that appeared in the log.

`td_install_present` already dereferenced once and was correct. Only the
validator was wrong, which is why the install refused a perfectly good
swapchain on every single attempt.

## Two other things this run settled

**`self+0xE40` is correct, but only for the right object.** In v54 the install
ran from `beginRender`'s first calls and read `+0xE40` as
`0000023e5c019650`, whose slot 9 was float data. In v55 and later, reading
`+0xE40` from `endRender`'s object gives a real swapchain. The offset was never
the problem; the caller was. The install now lives in `hooked_end_render` only.

**A second copy of the DLL cannot co-exist.** Injecting v54 into a process
running v53 gave:

```
RTTI resolution FAILED (vtable slot 2 is hooked by teardown_vr53.dll,
an earlier build of this mod still loaded in this) - falling back to hardcoded RVAs
```

That guard is correct and worth keeping, but it means every comparison needs a
fresh game process. `fresh_game.ps1` handles this.

## Result

```
[VR] present: vtable=00007ffe62d7c830 slot 9 @+48 real=00007ffe62ca50e0 -> 00007ffe7f3e2330
[VR] present: HOOK LIVE. Real Present saved at 00007ffe62ca50e0
[VR] present: call 1 / 600 / 1200 / 13200 / 13800
```

Game ALIVE, no Sentry dialog, Present counted past 13 800 calls, no code bytes
touched.

## The lesson

Two implementations disagreed about the same object, and the one that was
simpler was right. When a validator rejects something that a direct measurement
confirms is valid, the validator is the bug — and the cheapest way to find it is
to write a second, deliberately naive implementation and compare.

Corollary: the reason this went unnoticed for four builds is that the probe and
the install were never run side by side on the same object. Every earlier run
had the scan reject things *and* the offset be wrong, which looked consistent.
