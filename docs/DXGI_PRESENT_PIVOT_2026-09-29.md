# DXGI present pivot — path B step 1

Date: 2026-09-29. Supersedes the DXGI conclusions in
`XR_CRASH_BISECT_2026-09-29.md`.

## What changed

`CreateDXGIFactory1` used to fail with `hr=0x80004002` (E_NOTIMPL) and a NULL
factory. That was read as "DXGI is unavailable in this process" and fed into a
chain of conclusions: no D3D12, install the Agility SDK, and
`xrCreateSession` refusing to bind.

All three interface ids had been written from memory, and all three were wrong:

| interface       | what was in the file                            | correct, from dxgi.idl                     |
|-----------------|-------------------------------------------------|--------------------------------------------|
| IDXGIFactory    | `{7b7166ec-21c7-46ae-8e06-1a3b0e6c3e6c}`        | `{7B7166EC-21C7-44AE-B21A-C9AE321AE369}`    |
| IDXGIFactory1   | same as Factory (a copy)                        | `{770AAE78-F26F-4DBA-A829-253C83D1B387}`    |
| IDXGIFactory2   | `{50c83a1c-e072-4c93-898b-fbb639d95310}`        | `{50C83A1C-E072-4C48-87B0-3630FA36A6D0}`    |

E_NOTIMPL is what you get when the runtime is asked for an interface it has
never heard of, so this looked exactly like a missing DXGI. It was a typo.

## Measured, in the live process (pid 8364)

```
CreateDXGIFactory1(id#0 IID_IDXGIFactory2) -> hr=0x00000000
factory=0x0000021...  vtable=0x7FFE9E002FF0
slots 0..25: all 26 resolve inside dxgi.dll

CreateSwapChain        = 0x7FFE9DF4F2D0
CreateSwapChainForHwnd = 0x7FFE9DF51170
EnumAdapters1          = 0x7FFE9DF86E40

adapter 1: "NVIDIA GeForce RTX 4070"            vendor=0x10DE  VRAM=12010 MB
adapter 2: "Microsoft Basic Render Driver"      vendor=0x1414  VRAM=0 MB
EnumAdapters1(3) -> hr=0x887A0002 (DXGI_ERROR_NOT_FOUND)   clean end
grow3s=3.17s -> ALIVE
```

Factory slot numbers, from the inheritance chain in `dxgi.idl`, all confirmed by
the pointers landing in dxgi.dll:

```
IUnknown        0 QueryInterface   1 AddRef   2 Release
IDXGIObject     3 SetPrivateData   4 SetPrivateDataInterface
                5 GetPrivateData   6 GetParent
IDXGIFactory    7 EnumAdapters     8 MakeWindowAssociation
                9 GetWindowAssociation   10 CreateSwapChain
               11 CreateSoftwareAdapter
IDXGIFactory1  12 EnumAdapters1   13 IsCurrent
IDXGIFactory2  14 IsWindowedStereo 15 CreateSwapChainForHwnd
               16 CreateSwapChainForCoreWindow ...
```

IDXGIAdapter1, likewise: `8 GetDesc`, `9 GetDesc1`, `10..12` the
hardware-content-protection and video-memory-budget tail.

`DXGI_ADAPTER_DESC1` is **312 bytes** (`Description[128]` WCHAR = 256, then
VendorId 256, DeviceId 260, SubSysId 264, Revision 268, DedicatedVideoMemory
272, DedicatedSystemMemory 280, SharedSystemMemory 288, AdapterLuid 296, Flags
304). A 264-byte buffer writes 48 bytes past the end.

## The three bugs in the probe itself

1. **Wrong interface ids** — above.
2. **`void** vt = (void**)factory;`** This assigns the object's own address and
   never reads anything, so every "slot" was a field of the factory at offsets
   8, 16, 24... Slots 1-16 came back as garbage and `0xFFFFFFFF`; slots 17-25
   came back as plausible `dxgi.dll` addresses. **Half the table looked right
   and was believed for two builds.** The catch was a contradiction inside a
   single run: the vtable dump showed `GetDesc` at a good address while the
   direct slot read of the same interface returned `0xFFFFFFFF`.
3. **`desc[264]` instead of `desc[312]`** — 48 bytes of stack smash inside
   `GetDesc`.

Also: a vtable slot is `obj[0][n]`, not `obj[n+1]`. Passing an
already-dereferenced vtable and adding 1 reads the object's data.

## A frozen process is not a dead process, and not a live one

Calling the bad `GetDesc` pointer did not kill the game. Sentry caught the
access violation, raised its modal dialog, and the process stayed resident with
`grow3s=0`. Three distinct states, all of which occur here:

| state            | signal                                    |
|------------------|-------------------------------------------|
| dead             | no process                                |
| frozen (dialog)  | process present, `grow3s=0`              |
| alive            | process present, `grow3s>0`               |

Only the third may be reported as success.

## Runtime switches: a file, not an environment variable

The probe's work is gated on `TDVR_DXGI_TOUCH`, read from `tdvr_flags.txt`
next to the DLL in the game folder — **not** from an environment variable.

Setting `TDVR_DXGI_TOUCH` in the user environment and broadcasting
`WM_SETTINGCHANGE` did not work: the game is started by scheduled task
`tdvr_go`, whose process environment comes from the service, and the log kept
saying `TDVR_DXGI_TOUCH=0` after the variable was set and confirmed present in
the profile. `xcycle.ps1 -DxgiTouch 0|1` writes the file per run, which is
where the value now comes from. The resolved value is logged, never the
requested one.

| flag | probe behaviour | 90 s soak |
|---|---|---|
| `0` | reads the module handle and the export address, calls nothing | WORKING |
| `1` | creates a factory, dumps 26 slots, walks adapters, releases | WORKING |

The `1` run is the one that produced every measurement above
(`log = 9882 B`). Both survived, so creating our own DXGI factory is **not**
what froze the earlier process.

That leaves the earlier `STALLED` unexplained rather than disproved. It was
observed on pid 17936 — launched 09:37, which predates the GUID fix, and
carrying a DLL that was still doing the broken `E_NOTIMPL` walk. The three
`STALLED` readings all came from that single process. It has not reproduced
since, but nor has it been deliberately reproduced, so it stays open.

## The stale status file that cost a day

```
live:     teardown pid=14984  started 10:05:40
file:     flicker_status.json  written 08:05:47  pid=17152
          frame=1  totalSubmissions=1  lastLayerCount=0
```

`build/xflicker.ps1` now refuses to print those numbers unless the pid in the
file matches the live process, and says `*** STALE ***` instead. A zero in a
two-hour-old file describes a dead run, not a fault.

## Consequence for the D3D12 conclusion

`build/xrprobe_sim.exe` reports `D3D12CreateDevice=E_NOTIMPL` on every adapter,
and that is the recorded reason for the Agility-SDK line of thinking. The
failure shape is now known to be reachable by hand-written interface ids, so
that measurement is **not** trustworthy until repeated with ids and entry points
taken from headers. Do not act on it again before re-measuring.

## Next step

Hook `CreateSwapChainForHwnd` (0x7FFE9DF51170, measured but ASLR-dependent —
resolve it at runtime from the factory vtable, never hardcode). Capture the
returned `IDXGISwapChain`, then hook `Present` (slot 8) to obtain a real
`ID3D12Resource` back buffer. That is the backbuffer the OpenXR projection
layer needs, and it does not depend on any engine symbol.

---

# UPDATE, later the same day: the create hook was tried and it does not fire

The "next step" above was carried out and **failed in a way that narrows things
down**. `hook/present_hook.h` installs both slots, reads them back VERIFIED, and
the hook is never called — not once, in any configuration.

## Three causes removed, one at a time

**1. The game was on D3D11, not D3D12.** `options.xml` was found with
```xml
<gfxapi value="0"/>
<d3d12support value="0"/>
```
D3D11's `IDXGIFactory` creates its swapchain internally, so the game never calls
`CreateSwapChainForHwnd` at all. `go_live.bat` used to force these to `1` — but
`golive_inner.bat` is the launch path now, and it was silently missing the step.
Forcing both to `1` before launch is now part of `golive_inner.bat`.

**2. The hook was installed after the swapchain already existed.** With D3D12 on,
the log ordering was unambiguous:
```
line 40  [hit] hooked_begin_render call #1
line 52  [SC] === swapchain capture hook ===
```
The renderer was already running. `tdvr_early_present_hook()` was moved to the
very top of `init_thread`, ahead of the 10 s host-image search and
`install_hooks()`. After the move:
```
line 31  [SC] installed: csc=1 cscw=1
line 45  ready
line 60  [hit] hooked_begin_render call #1
```
In place two log lines before the first frame. Still never fires.

**3. The shared-vtable assumption is dead.** This was the load-bearing claim —
"dxgi.dll implements `IDXGIFactory2` from one shared table, so patching our own
factory's table also patches the game's". A full 26-slot dump confirms our
factory's table is a genuine `IDXGIFactory2` table (all 26 slots are `dxgi.dll`
thunks; slot 10 `0x7ffe9df4f2d0`, slot 15 `0x7ffe9df51170`). So the table we
patched is real. The game's factory is simply not on it.

Timing is ruled out, D3D12 is on, and the table is valid. That leaves
`D3D12Core.dll` (the Agility SDK) implementing its own DXGI factory — it is
loaded at `0x7ffe5bad0000`, size `0x359000`.

## Agility SDK IS present — earlier conclusion retracted

`D3D12Core.dll` is loaded in the game process, alongside `D3D12.dll`, `dxgi.dll`
and `nvwgf2umx.dll`. The earlier "no Agility SDK, no usable D3D12" line of
thinking was wrong. `D3D12CreateDevice=E_NOTIMPL` in `build/xrprobe_sim.exe` is a
separate problem in a separate process and is not evidence about the game.

## The heap scan is rejected code, and it killed the process

`scan_for_swapchain()` found 40 candidate objects across 21 distinct vtables —
dxgi's own internal factories, adapters, output objects, none of them the game's
swapchain — and called `QueryInterface` on them. Result: `GONE`, no minidump.
Calling an interface method on an object you have merely pattern-matched is not
survivable. It is now a comment.

Two real bugs lived inside it:

- `IID_ID3D12Resource` was wrong from memory (`cd3f4f26-…`). From
  `/usr/x86_64-w64-mingw32/include/d3d12.h` it is
  `696442be-a72e-4059-bc79-5b5c98040fad`.
- The filter `else if (v != vtab) continue;` kept only the first matching vtable
  and skipped the rest — 2 of the 40 candidates found were ever tested.

## `self+0xE40` is not a swapchain

The old `endRender` hook reports a swapchain at `self+0xE40` with
`vtable=0x7ffe5bddc830` and logs `in_module=1`. Module mapping says:
```
vtable 0x00007FFE5BDDC830 -> D3D12Core.dll +0x30C830
slot9  0x00007FFE7B1720E0 -> vr_bisectA.dll +0x20E0
```
`in_module=1` only proved the pointer was inside *some* module. The vtable is
`D3D12Core.dll`, not `dxgi.dll`, and `slot9` points at our own hooked
`endRender`. `GetBuffer` on it returning `hr=0` with a NULL out means nothing.
Do not confuse it with the dead `TRendererD3D12` vtable (RVA `0xA81D80`, 48
slots, zero calls) from the earlier bisect.

## Launcher

`C:\tdvr\golive_inner.bat` is the single launch path, in session 1:
force `gfxapi=1` / `d3d12support=1` → `injector.exe --game <exe> --dll <dll>
--nowindow --noquit`. `--nowindow` is load-bearing: without it the injector waits
for the game window, which is the delay that made the hook late. With no
`--attach` the injector launches the game itself, so start and attach happen in
one process in one session.

## Where this actually leaves the create hook

The game's real `IDXGIFactory` object has not been obtained. Getting it from the
*device* rather than from `dxgi.dll` is the way forward: `ID3D12Device` →
`GetNodeCount`/`GetAdapter` → `IDXGIAdapter::GetParent` → `IDXGIFactory4`, then
patch that object's own vtable. The device is already reachable through the path
that produced `GetDevice -> hr=0 dev=0x...` in the log, so this does not depend on
any engine symbol either.

---

# UPDATE 2: the device walk, and what it revealed

## `ID3D12Device` does NOT inherit `IDXGIDevice`

From `d3d12.h`:
```
ID3D12Object : public IUnknown      (slots 0-2 only)
ID3D12Device : public ID3D12Object
```
There is no `GetAdapter` on an `ID3D12Device` at all. Calling vtable slot 10 as
`GetAdapter` invoked an unrelated D3D12 method with mismatched arguments, and it
**hung** — the log stopped dead at `[SC] device=...` with no result, no crash, no
minidump, every run. A wrong slot in this direction is a hang, not a crash.

The correct route is `QueryInterface(IID_IDXGIDevice)` first, then
`GetAdapter(slot 10)` on *that*. QI is slot 0 and is right by definition, so the
only value that must be right before any call is the GUID.

## The render thread must not do the walk

The first version called `GetAdapter` inline from `tdvr_xr_bind_device_from_swapchain`,
which runs on the render thread. `GetAdapter` takes a driver lock and can block
behind the frame being presented. It is now queued: `tdvr_factory_walk_later()`
takes an AddRef on the render thread (cheap, no lock) and hands the pointer to the
existing worker, which does `GetAdapter` and `GetParent` and balances the
reference afterwards.

## The identity probe: the object is a real ID3D12Device that hides IDXGIDevice

```
QI IID_ID3D12Device        -> 0x00000000  obj=0x155780862b0  vtable=0x7ffe5bd218c8
QI IID_ID3D12Object        -> 0x00000000  obj=0x155780862b0  vtable=0x7ffe5bd218c8
QI IID_ID3D12CommandQueue  -> 0x80004002  (correct: it is a device, not a queue)
QI IID_IDXGIDevice         -> 0x80004002  (unexpected)
QI IID_IDXGIFactory2       -> 0x80004002  (expected on a device)
```

`IID_IDXGIDevice` was verified twice against `dxgi.h` (line 1989 and the
`MIDL_INTERFACE` string on 1992) and is correct. The command queue rejection is
the control that proves the object is not a queue and not a coincidence. So the
`ID3D12Device` here — created through `D3D12Core.dll`, the Agility SDK — does not
expose the DXGI view of itself.

That means the device→adapter route is closed, and it is worth not re-deriving it
again. The swapchain hook on `dxgi.dll`'s factory remains the live path, and its
failure is still unexplained.

## What DID work: the session finally started

Forcing D3D12 plus taking the device from the renderer's own swapchain got the
OpenXR session up, which it had never been in any prior run:
```
OpenXR: xrBeginSession -> 0 (from READY)
OpenXR: loop waits=1 ends=0 ready=1 err=0 tracked=1
OpenXR: projection submissions=1 of 0 ends (swapchains L=1 R=1 1280x720 fmt=29)
```
`READY` is the state the whole project was aiming at. Note the honest reading of
the same line: `waits=1 ends=0` and `1 of 0 ends` — one wait, zero completed
frames, so the frame loop is not yet turning over. The session exists; the loop
does not yet run.

## One more recalled GUID, caught by reading instead

`IID_ID3D12Object` was first written as `9a bc 9d e4 48 4c 2a fc`. The header
(`d3d12.h:3041`) says `9f 94 f4 31 cb 56 c3 b8`. The wrong value would have made
the identity probe lie in the opposite direction — reporting a device as
non-D3D12 — which is exactly the class of error that produced this project's
earliest false conclusions. Every GUID in this file is now read from
`/usr/x86_64-w64-mingw32/include/{dxgi,d3d12}.h`, never recalled.

---

# UPDATE 3: the frame loop gets to EndFrame, and EndFrame never returns

The session reaching `READY` was real progress but the loop behind it is still not
turning over. `waits=1 ends=0` was the symptom; this run localised it exactly.

## The handoff trace, producer and consumer

Neither side of the render→XR-thread handoff was logged, so the log could not
distinguish "never handed over" from "handed over and never picked up" from
"picked up and hung". Both sides now log:

```
[END] handed over: wait=1 pending=1 layerCount=1 views=2 time=234052222279111
[END] woke: busy=1 pending=1 done=0 errors=0 dropped=0
[END] slot: type=0xC layerCount=1 viewCount=2 time=234052222279111
<nothing, ever>
```

That is decisive. The producer handed a frame over, the consumer woke, `busy=1`
proves the slot was claimed, `type=0xC` is `XR_TYPE_FRAME_END_INFO`, and
`viewCount=2` is correct. The next statement in that thread is
`X->EndFrame(X->session, &s.ei)` and **nothing is ever printed after it**.

The thread is inside `xrEndFrame`. A thread snapshot confirms it: 21 of 55
threads are not parked in `Wait/UserRequest`, and the frame-end thread is one of
them.

## Compositing is NOT the cause — layerCount=0 behaves the same

The old hypothesis in these comments was that the runtime's composition of the
projection layer contends with the game's in-flight frame on the same device. A
diagnostic build with `TDVR_XR_LAYER0=1` — which hands over `layerCount=0`, so the
runtime is told to composite nothing — behaves **identically**:

```
[END] slot: type=0xC layerCount=0 viewCount=2
OpenXR: loop waits=1 ends=0 ...
```

Same stall, same place. So the contention theory is dead, and it is worth killing
explicitly because it has been the standing explanation for a long time.

## What is left

`xrEndFrame` blocks even when asked to end an empty frame. That points at the
runtime's frame-completion path rather than at anything about layers, images or
the copy — and `fence=0` in the log is a red herring, because the fence belongs to
the `TDVR_XR_COPY_ENABLED=0` copy path, which is off by default and never runs
here.

The next thing to test is whether the stall is in the runtime waiting for the
application to release something, or in the runtime's own present. The cheapest
discriminator is a run with no frame loop at all — session created, `BeginSession`
called, but `tdvr_xr_poll` never entered — which separates "EndFrame cannot be
called from this process" from "EndFrame cannot be called while the game's
Present is outstanding".

## State of the machine

```
xrCreateInstance       XR_SUCCESS
xrGetSystem            XR_SUCCESS
xrCreateSession        XR_SUCCESS   (device from the renderer's own swapchain)
xrBeginSession         XR_SUCCESS   READY -> SYNCHRONIZING -> VISIBLE -> FOCUSED
xrWaitFrame            XR_SUCCESS   shouldRender=1
xrBeginFrame           XR_SUCCESS
xrLocateSpace/Views    XR_SUCCESS   2 views, flags=0xf
xrAcquireSwapchainImage XR_SUCCESS  x2, idx=0
xrEndFrame             NEVER RETURNS
waits=1 ends=0 tracked=1 err=0
```

Everything up to and including acquiring both eye images works. The loop is one
call past the end.

---

# UPDATE 4: EndFrame is a victim, not the cause

`XR_NOLOOP` (mode 12) exists to answer exactly one question, and it answered it
by ruining the previous conclusion.

## The control run

Session created, `xrBeginSession` succeeded, state machine walked
`2 -> 3 -> 4 -> 5` — and the frame loop was never entered. `xrWaitFrame`,
`xrBeginFrame` and `xrEndFrame` were **not called at all**.

```
xrBeginSession -> 0
state 2 -> 3 -> 4 -> 5
TDVR_XR_NOLOOP=1 -- session and events run, the frame loop is not entered at all
loop waits=0 ends=0
VERDICT : STALLED
```

The game stalls identically. With `EndFrame` never reached, `EndFrame` cannot be
what hangs. The "[END] slot:" line that looked like the point of death is just
the last thing the consumer logged before entering a call it never comes back
from — the stall and the missing log line are two symptoms of one cause, not a
cause and an effect.

This also retires the `layerCount=0` result from UPDATE 3: it was read as "the
stall is not about layers", which is still true, but it was measured on a run
whose stall is not about `EndFrame` either. Both experiments shared the real
cause and neither could see it.

## Where the log actually stops

```
[SC] >> device QueryInterface(IID_IDXGIDevice) -> hr=0x80004002
[SC] the device does not expose IDXGIDevice ...
endRender 2 survived the original call
```

`endRender` returns normally. The game stops **after** our hook hands control
back, with an `IDXGIDevice`-less D3D12 device and a live XR session bound to it
still resident in the process.

## What the two runs share

Normal run: session live, stall right after `endRender`.
NOLOOP run: session live, no `WaitFrame`/`EndFrame` at all, stall right after the
same `endRender`.

The invariant across both is **a live XR session bound to the game's own D3D12
device, with the game rendering through that same device**. The frame loop is not
part of it. That is the thing to attack next, and the session creation point is
where it happens — `tdvr_xr_try_session()` takes the renderer's device and queue
verbatim and hands them to `xrCreateSession` on the render thread.

`IID_IDXGIDevice` returning `E_NOINTERFACE` is a separate, closed question: the
GUID is verified against `dxgi.h`, and the identity probe proves the object is a
D3D12 device, not a queue. It stops the adapter walk and nothing else. It is not
the stall.

---

# UPDATE 5: the "swapchain" was never a swapchain

`TDVR_XR_OWN_DEVICE` (mode 14) was built to test whether *sharing* the renderer's
device is the cause. It never ran — and the reason it never ran is the more
important result of the two.

## The giveaway that was in the log the whole time

```
probe endRender #1: ... <- THIS IS THE SWAPCHAIN
swapchain GetDevice(slot 7) -> hr=0x00000000 dev=00000167df4c2920
swapchain GetDesc -> 0x0 (BufferDesc first words: 00000000 00000000 00000000 00000000)
GetBuffer(slot 9) -> hr=0x00000000 backbuffer=0000000000000000
```

`GetDesc` returns `S_OK` with an **all-zero `DXGI_SWAP_CHAIN_DESC`**, and
`GetBuffer` returns `S_OK` with a **null buffer**. A real `IDXGISwapChain` cannot
do that. `DXGI_SWAP_CHAIN_DESC` starts with
`BufferDesc{Width, Height, Format, ScanChain, Scaling}`; an implementation that
returns success without writing any of it is a stub. These are not a swapchain
with a bad descriptor — they are a different interface that answers success.

## Module attribution settles it

```
object  -> not in any module (heap)
vtable  -> D3D12Core.dll+0x2518C8
slot9   -> D3D12Core.dll+0x2350E0
```

A vtable lives in the module that implements the interface. Both the vtable and
slot 9 are inside **`D3D12Core.dll`** — the Agility SDK — not in `dxgi.dll` and not
in the game. So the object at `endRender+0xE40` is an Agility-SDK object, and the
code was treating it as an `IDXGISwapChain` because its slots *answered* like
one. The success return values were the tell; the log line claiming otherwise was
written by the code being fooled.

This is the same conclusion UPDATE 2 reached from a different angle, now with the
vtable's owning module named rather than inferred from a slot pointing at our own
hook.

## Why reading the object failed, and why that is not a bug

`Marshal.ReadInt64` on the object's address raised `AccessViolationException`.
That is the expected and correct outcome, not a broken script: the object belongs
to another process, so its pages are not readable from here and never were. There
is no version of this that works, and the same reasoning retires any idea of
inspecting the object's fields from outside. Facts about the object can only come
from calls *into* it, which is what the log does.

## Consequence

`xrCreateSession` was being handed a device obtained through
`GetDevice(IID_ID3D12Device)` on a **fake** swapchain. That device was probably
real — the `CreateCommandQueue` on it succeeded and the identity probe confirms
`IID_ID3D12Device`/`IID_ID3D12Object` — but it was reached through an object that
is not what we assumed, so nothing downstream of that assumption is trustworthy.

`TDVR_XR_OWN_DEVICE` is the right next experiment and is now unblocked: the
fallback path it sits behind was returning early because the queue creation
happened before the mode check, and because the whole function is gated on
`GetDesc` returning a usable size. That gate is the thing to fix — the mode must
be able to run without a swapchain at all, since its whole point is to not use
one.

---

# UPDATE 6: two real bugs, and the stall moves again

Getting mode 14 to actually run turned up more than it was built to test.

## Bug 1: `TD_PRESENT_SLOT` was 9, and slot 9 is not Present

```
IDXGISwapChainVtbl, counted from dxgi.h:
  0 QueryInterface   1 AddRef   2 Release
  3 SetPrivateData   4 SetPrivateDataInterface   5 GetPrivateData
  6 GetParent        7 GetDevice (0x38)
  8 Present (0x40)   9 GetBuffer (0x48)
 10 SetFullscreenState   11 GetFullscreenState   12 GetDesc
```

`present.h` had `#define TD_PRESENT_SLOT 9` and a comment cheerfully explaining
that `0x48` is Present, with a disassembly excerpt backing it up. Both were
wrong: the code was patching `GetBuffer`, saving it as "the original Present",
and reading it back VERIFIED. The corrected build shows two different addresses,
which is the proof the two slots are different functions:

```
slot 8 @+40 real=00007ffe5bd08200
slot 9 @+48 real=00007ffe5bb350e0
```

This is the same failure the whole project has been fighting: a number confirmed
against a comment instead of against the header. The comment cited a call site
`call qword ptr [rax+0x48]`; that address is real, but it is `GetBuffer` being
called on something, and nothing in the excerpt justified calling it Present.

## Bug 2: mode 14 ran on the render thread, where a driver lock hangs

Placing the mode at the top of `tdvr_xr_bind_device_from_swapchain()` got it past
the fake swapchain, and then it hung — because that function runs on the render
thread with a frame in flight, and `D3D12CreateDevice` takes a driver lock. The
log stopped dead at:

```
OpenXR: endRender frame 1: swapchain=... present_orig=...
```

with no `D3D12CreateDevice -> hr=` line. The mode is now a flag handed to the
existing worker instead, and the worker does run it:

```
[SC] worker thread 0000000000000d7c
[SC] worker: TDVR_XR_OWN_DEVICE -- creating a private device+queue here
```

...and then **also never returns**. No `hr=`, no `private device=`, nothing.

So the stall is not specific to the render thread and not specific to the game's
device. It reproduces in a bare `D3D12CreateDevice(NULL, ...)` on an otherwise
idle worker thread, in a process that has an OpenXR instance and a system but no
session.

## What that leaves

Both an `xrCreateSession` on a shared device and a `D3D12CreateDevice` on a
private one hang the process. The common factor is not the device and not the
thread: it is an OpenXR instance existing in the game process while the game's
D3D12 is being touched. Everything measured before this assumed the frame loop or
the device binding was the variable. It is neither.

The honest next question is whether the runtime is holding a lock that the game's
own device creation contends with, and the cheapest way to answer it is an
instance-only run (`TDVR_XR_INSTANCE_ONLY=1`, already built) where a private
device is created *after* `xrCreateInstance` — that combination has never been
tested, and it is the one remaining cell in the table.

## Also fixed while in here

- `vr_log` is not threadsafe and the render thread and worker now log
  concurrently; the log shows interleaved lines. Not a crash risk (the write is
  a single call into the CRT) but it makes ordering unreadable, so line order
  across the two threads is not evidence of anything.
- `tdvr_xr_try_session` takes `tdvr_xr*`, not `void*`; the forward declaration
  written from memory produced a conflicting-type error, and the real signature
  is now copied from `xr_session.h`.

---

# UPDATE 7: the whole thing reduces to one fact

The last untested cell in the table has been tested, and it answers the question
every earlier run was circling.

## The run

`TDVR_XR_INSTANCE_ONLY=1`, extended to create a private D3D12 device right after
`xrCreateInstance` and before anything else:

```
instance created at API 1.1.0
XR_INSTANCE_ONLY build: instance created, stopping before xrGetSystem
INSTANCE_ONLY -- now asking D3D12CreateDevice for a private device, with an
                 instance alive and no session
<log ends, 890 bytes>
VERDICT : STALLED
```

**A bare `D3D12CreateDevice(NULL, ...)` hangs.** In that process there is:

- no `xrGetSystem`, no session, no reference space, no frame loop
- no swapchain, no backbuffer, no projection layer
- no contact with the game's renderer, its device, or its `IDXGISwapChain`
- no `D3D12Core.dll`, no Agility SDK, no `dxgi.dll` call from our side
- one `ID3D12Device` argument, which is `NULL` — so the adapter is left entirely
  to the runtime

The only thing in the process that could plausibly hold a driver lock is the
OpenXR instance. Everything else has been excluded by measurement.

## Why the earlier runs looked different

They were not different. Each one had an instance alive, and each one went on to
touch D3D12 — `GetDevice`, `CreateCommandQueue`, `xrCreateSession` — and each one
stopped at the first such call. The apparent variation between them (EndFrame
versus the frame loop versus the session binding) was the *log position* moving,
not the cause. `xrEndFrame` never hung; it was simply the next D3D12-adjacent
call after the ones that already had.

This retires, in one stroke, the whole class of hypotheses this file has been
accumulating:

| retired | why |
|---|---|
| layer composition contention | no session, no layer, still hangs |
| frame loop / EndFrame | NOLOOP and INSTANCE_ONLY both stall, no loop at all |
| shared vs private device | both stall, the private one is a fresh `NULL`-adapter device |
| render thread driver lock | worker thread, no frame in flight, still hangs |
| the fake swapchain | never touched in this run |
| Agility SDK / D3D12Core | not loaded, never called |

## What is actually happening

An OpenXR runtime's `xrCreateInstance` is doing something that makes subsequent
D3D12 device creation block indefinitely in this environment. The environment
part is not incidental: `GPUID: 900` with `Computer=VMware`, an RTX 4070 passed
through a virtual GPU, and `nvlddmkm` event ID 153 in the log. A runtime that
enumerates adapters through DXGI at instance creation will contend with the
virtual display adapter stack in a way bare-metal hardware does not.

That is a hypothesis, and it is labelled as one. What is *measured* is the
single sentence at the top of this section. The next experiment that would
discriminate it is trivial and cheap: load the loader and call
`xrCreateInstance` with **no** graphics extensions requested at all —
`TDVR_XR_NO_D3D12_EXT=1` exists for exactly this and was only ever tested
*combined with* a session before. Instance-only, no extensions, then
`D3D12CreateDevice`.

## That experiment was run immediately, and it closes the question

```
INSTANCE_ONLY -- now asking D3D12CreateDevice for a private device, with an
instance alive and no session (NO_D3D12_EXT=1: no graphics extension was
requested)
<log ends, 961 bytes>
VERDICT : STALLED
```

Still hangs, with `enabledExtensionCount = 0`. So the extension is not it either —
my adapter-enumeration theory is wrong, and it was the most plausible thing left
to try. The runtime is not reaching for D3D12 because we asked it to; simply
existing is enough.

The narrowing is now complete and there is nothing left to bisect between:

```
loader resident, no instance ......... D3D12CreateDevice fine (game ALIVE)
instance, no extensions .............. D3D12CreateDevice HANGS
instance + XR_KHR_D3D12_enable ....... D3D12CreateDevice HANGS
```

`xrCreateInstance` is the boundary. Whatever it does — and it is not a graphics
binding, and not adapter enumeration, and not the extension list — leaves D3D12
device creation permanently blocked in this process.

## Consequence for the project

Strategy B — hooking the game's real `CreateSwapChain` to get a real
`IDXGISwapChain` — is not blocked by any of the above. The DXGI work stands on
its own and is worth finishing regardless. But VR-in-Teardown through this
runtime on this machine is not reachable while an instance poisons D3D12 device
creation, and no amount of frame-loop correctness gets past that.
