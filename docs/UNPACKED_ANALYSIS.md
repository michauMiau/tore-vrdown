# Unpacked binary — real function addresses

Source: `Teardown, steamless.zip` → `Teardown/teardown.exe`
(30,333,440 bytes, `.text` entropy **6.474** — decrypted, usable)

The `.original.exe` in the same folder is the protected build
(`.text` entropy 8.000, `.bind` protector stub). The repack's
`teardown.exe` is the already-unpacked image, so we disassemble it directly.

> **This changes the whole approach.** The protected `teardown.exe` from the
> phone upload is not needed for analysis. The Steamless repack ships a
> decrypted `teardown.exe`, which means full static RE is available:
> real addresses, real control flow, real disassembly.

Image base: `0x140000000`

## Sections

| Section | RVA | Raw | Entropy |
|---|---|---|---|
| `.text`  | `0x00001000` | `0x00000400` | 6.474 |
| `.rdata` | `0x00982000` | `0x00980C00` | 5.399 |
| `.dataa` | `0x00BF2000` | `0x00BF0400` | 0.265 |
| `.pdata` | `0x01F97000` | `0x01C6E400` | 6.591 |
| `.rsrca` | `0x02000000` | `0x01CD6E00` | 5.095 |
| `.reloc` | `0x02009000` | `0x01CDFC00` | 5.357 |

Note: the repacker renamed `.data` → `.dataa` and `.rsrc` → `.rsrca`.
Entry point `0x006D0DE4` (inside `.text`).

## Located functions (via `.pdata` unwind entries + xref scan)

`TRendererD3D12::beginRender` label is at `.rdata` `0x00A85E78` and has
**2 cross-references**, both inside one function:

| Function | RVA range | Size |
|---|---|---|
| `beginRender` wrapper | `0x005AB9D0` – `0x005ABCAA` | 730 B |
| `endRender` wrapper   | `0x005AF270` – `0x005AF67C` | 1036 B |
| `present` wrapper     | `0x005B3FDA` – `0x005B4095` | 187 B |
| `createDeviceD3D12`  | `0x005AD300` – `0x005ADF94` | 3220 B |

### beginRender disassembly (key part)

```asm
005AB9D0:  mov  QWORD PTR [rsp+0x18],rbx
005AB9D5:  mov  QWORD PTR [rsp+0x20],rbp
005AB9DA:  push rsi / rdi / r14
005AB9DE:  sub  rsp,0x30
005AB9E2:  mov  rdi,rcx                          ; rdi = this (TRendererD3D12*)
005AB9E5:  mov  BYTE PTR [rcx+0xCC],1            ; rendering flag = true
005AB9EC:  call 0x14050A2F0
005AB9F1:  mov  QWORD PTR [rdi+0xF70],rax
005AB9F8:  movsxd rax,DWORD PTR [rdi+0xDA8]     ; frame index
005AB9FF:  lea  rbx,[rax+rax*2]                  ; *3
005ABA03:  shl  rbx,0x4                          ; *16  -> stride 48
005ABA07:  add  rbx,QWORD PTR [rdi+0xEF8]        ; &frameContexts[idx]
005ABA0E:  mov  rcx,QWORD PTR [rdi+0xE58]        ; command queue
005ABA15:  mov  rax,QWORD PTR [rcx]
005ABA18:  call  QWORD PTR [rax+0x40]            ; slot 8  = ID3D12CommandQueue::Signal/Wait
005ABA1B:  mov  rdx,QWORD PTR [rbx+0x8]
005ABA1F:  cmp  rax,rdx
005ABA22:  jae  0x1405ABA30
005ABA24:  lea  rcx,[rdi+0xE58]
005ABA2B:  call 0x1405A2910                      ; grow the queue
...
005ABA44:  mov  rcx,QWORD PTR [rbx+0x10]
005ABA48:  mov  rax,QWORD PTR [rcx]
005ABA4B:  call  QWORD PTR [rax+0x8]             ; slot 1  = AddRef
005ABA4E:  mov  rcx,QWORD PTR [rbx]
005ABA51:  mov  rax,QWORD PTR [rcx]
005ABA54:  call  QWORD PTR [rax+0x40]            ; slot 8
005ABA57:  mov  esi,eax
005ABA59:  lea  rbp,[rip+0x4D0220]               ; 0x140A7BC80
005ABA60:  lea  r14,[rip+0x4D0181]               ; 0x140A7BBE8
...
005ABB27:  call  QWORD PTR [rax+0x50]            ; slot 10
005ABBFF:  call  QWORD PTR [rax+0xE0]            ; slot 28
005ABC59:  call  QWORD PTR [rax+0xF8]            ; slot 31
```

### Recovered member offsets on `TRendererD3D12`

| Offset | Type | Meaning |
|---|---|---|
| `+0xCC` | `BYTE` | rendering-active flag (set to 1 at frame start) |
| `+0xDA8` | `int32` | current frame index |
| `+0xE58` | ptr | `ID3D12CommandQueue` |
| `+0xEF8` | ptr | frame-context array base, **stride 48 bytes** |
| `+0xF70` | ptr | result of the call at `0x14050A2F0` |
| `+0xF88` | ptr | something with a sub-object at `+0x38` |

The `call [rax+0x40]` = vtable slot 8 on `ID3D12CommandQueue` is
`ID3D12CommandQueue::Signal`, and the `cmp rax, [rbx+8]` / grow-call pattern
is a classic "wait until the GPU has caught up, otherwise reallocate" guard.

### present wrapper (187 B)

```asm
005B3FDE:  lea  r8,[rip+0x4D4893]     ; "TSwapChainD3D12::present"
005B3FE5:  lea  rax,[rip+0x4D48AC]
005B3FEC:  mov  r9d,0x21D              ; 541 = line number
005B3FF2:  lea  rdx,[rip+0x4C7BCF]
005B3FFE:  mov  ecx,0x3
005B4003:  call 0x140691D20            ; assertion / logging helper
005B4008:  cmp  edi,0x8007000E         ; DXGI_ERROR_DEVICE_REMOVED
005B400E:  jne  0x1405B4049
...
005B4079:  mov  rcx,QWORD PTR [rbp+0x58]     ; swapchain object
005B407D:  mov  rax,QWORD PTR [rcx]
005B4080:  call QWORD PTR [rax+0x120]        ; slot 36 = GetLastPresentTime
005B4086:  mov  DWORD PTR [rbp+0x50],eax
005B4089:  add  rsp,0x30
...       ret
```

The actual `IDXGISwapChain::Present` call is *not* in this wrapper — this is
only the error/status handler. The real Present happens in a callee.

## endRender wrapper (1036 B) — contains the real Present

```asm
005AF270:  mov  QWORD PTR [rsp+0x18],rbx
005AF280:  mov  rbx,rcx                     ; rbx = this
005AF28B:  add  rcx,0xE70
005AF292:  call 0x1405B3780
005AF29F:  lea  rcx,[rbx+0x1130]
005AF2A6:  mov  r8d,0x800                   ; 2048 bytes
005AF2AC:  mov  rdx,rax
005AF2AF:  call 0x1405B3280                 ; upload 0x800-byte block
005AF2B4:  mov  rdx,QWORD PTR [rbx+0xE40]   ; <-- swapchain member
005AF2BB:  lea  rcx,[rbx+0x1130]
005AF2C2:  call 0x1405B2C80
005AF2C7:  mov  rcx,QWORD PTR [rbx+0xE40]
005AF2CE:  mov  rax,QWORD PTR [rcx]
005AF2D1:  call QWORD PTR [rax+0x48]        ; slot 9 = IDXGISwapChain::Present
005AF2D4:  ...
005AF2F0:  lea  r8,[rip+...]                ; "TRendererD3D12::endRender"
005AF2FE:  mov  r9d,0x205                   ; 517 = line number
```

Two important finds here:

1. **`this + 0xE40` is the `IDXGISwapChain*`.** This is the single most
   valuable offset we have — it hands us the swapchain directly, no
   discovery or vtable scanning needed.

   **QUALIFIED 2026-09-27: only for the object `endRender` receives.** The same
   offset read from the object `beginRender` receives in v54 held scene data,
   whose vtable slot 9 read back as `0x234800000238c181`. The offset was never
   wrong, the caller was. The Present install now lives in `hooked_end_render`
   only. See `docs/PRESENT_HOOK_DOUBLE_DEREF.md`.
2. **The real Present is `call [rax+0x48]` at `0x1405AF2D1`** (vtable slot 9),
   not in the `present` label wrapper. `this + 0x1130` is a helper object used
   for the 0x800-byte upload, and `this + 0xE70` is another sub-object touched
   first.

## Where to hook

**Best: `beginRender` at `0x1405AB9D0`.**
- It is the frame entry point, before any matrices are set
- `this` = renderer, and from `this` we can reach the frame contexts and,
  through them, the per-frame constant buffer slots
- Hooking here lets us run the frame twice with different camera data

**Also useful: `endRender` at `0x1405AF270`** as the closer of the pair.

Both are ordinary C++ member functions (`rcx` = `this`, no fancy calling
convention), so a 14-byte `mov rax, imm64; jmp rax` trampoline at the entry
point is enough to redirect them.

## Recovered `TRendererD3D12` member map (consolidated)

| Offset | Type | Meaning |
|---|---|---|
| `+0xCC`   | `BYTE`   | rendering-active flag, set to 1 at frame start |
| `+0xD68`  | `uint64` | cleared in endRender (fence/counter) |
| `+0xD70`  | `uint64` | cleared in endRender |
| `+0xDA8`  | `int32`  | current frame index |
| `+0xE40`  | ptr      | **`IDXGISwapChain*`** |
| `+0xE58`  | ptr      | `ID3D12CommandQueue*` |
| `+0xE70`  | struct   | sub-object touched at endRender start |
| `+0xEF8`  | ptr      | frame-context array base, **stride 48** |
| `+0xF70`  | ptr      | stored from call at `0x14050A2F0` |
| `+0xF88`  | ptr      | object with sub-field at `+0x38` |
| `+0x1130` | struct   | upload helper used with the 0x800-byte block |
| `+0x1180` | struct   | written in endRender tail |

### Frame context layout (stride 0x30)

| Offset | Meaning |
|---|---|
| `+0x00` | object with a vtable (slot 8 called in beginRender) |
| `+0x08` | fence value the queue is compared against |
| `+0x10` | refcounted object (`AddRef` at slot 1) |

## SceneDynamicBuffer — confirmed

`DECLARE_CONSTANT_BUFFER(SceneDynamicBuffer, 1)` in the embedded HLSL:

```hlsl
DECLARE_CONSTANT_BUFFER(SceneDynamicBuffer, 1)
{
	float4x4 mubVpMatrix;        // 0x00
	float4 mubViewXDir;          // 0x40  xyz dir, w unused
	float4 mubViewYDir;          // 0x50
	float4 mubProjectionData;    // 0x60
	...
}
```

Other buffers that carry a `mubVpMatrix` and would need the same treatment:
- `EditorVoxDynamicBuffer` (slot 1) — also has `mubVpInvMatrix`
- `LinesBuffer` (slot 3)
- plus per-object `GBufferPerObjectBuffer`, `LitPerObjectBuffer`, etc.

`mubVpMatrix` is consumed by every G-buffer pass, the TAA reprojection
(`mubOldStableVpMatrix`), the visibility/raycast compute shaders, particles,
water and debug lines — so it is the single highest-leverage value to override.
