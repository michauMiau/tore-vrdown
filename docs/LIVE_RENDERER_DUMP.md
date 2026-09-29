# Live renderer dump — 2026-09-27, measured

This is real bytes read out of the running game. It is the ground truth for the
renderer layout, and it refutes what the comments in the source claimed.

## How it was taken

`build/dump_renderer.ps1` runs on the Windows box over SSH. It opens
`teardown.exe` with `PROCESS_VM_READ` only and copies bytes out. No injection,
no writes, no GPU work — which is why it can run while the game is in a level
and while another build is being edited.

It reads the `self=` pointer the mod logged (both `beginRender` and `endRender`
report the same one), dumps `self+0x000 .. self+0x2000` flat, then follows
every 8-aligned slot that points at committed memory, saving 256 bytes from
each. 81 files landed, 36 of them non-empty.

**The PowerShell 5.1 address problem is worth knowing about.** Every obvious way
to turn a `UInt64` into an `IntPtr` throws on an address with the top bit set:
`[int64]$v` overflows, `New-Object IntPtr` throws, `[IntPtr]::new($v)` has no
overload, `[IntPtr]::new([byte[]])` does not exist in .NET Framework. Worse, the
literal `0xFFFFFFFF` evaluates to `-1` as a signed `Int32`, so even a correct
`-band` is wrong. The fix is a two-argument `IntPtr(int low, int high)`
constructor, with a `static long Ptr(ulong v)` helper in the `Add-Type` block
doing `unchecked((long)v)` so the cast is a reinterpret rather than arithmetic.
`0xBC1B...` addresses are ordinary user-mode heap here; nothing exotic is going
on.

## What is actually in the renderer

No matrices. 0 of 81 dumped files contain a 4x4 that passes a plausible
`float4x4` test — non-zero diagonals in range, finite everywhere, non-zero
perspective sum. The test was run over every 0x10-aligned offset in every file.

The most repeated non-zero 8-byte values across the dump:

```
0x0000025A00000000  x51   high half of a user-mode heap pointer (0x25A... prefix)
0x0000000000000010  x23   16 - a size, a count, or a struct stride
0x00007FFE00000000  x23   0x7FFE high half, i.e. a system DLL
0xFFFFFFFF00000003  x19   integers, not pointers
0x0000000100000001  x19   more integers
0x0000000000000001  x16   more integers
0x0000001000000101  x15   more integers
0x00000000FFFFFFFF  x14   more integers
0x0000025A00000001  x13
0x0000000300000002  x13
```

So the renderer holds **D3D12/DXGI handles, sizes and counters**. That is
consistent with what `endRender` does with `+0x1130`: `mov r8d, 0x800 ; call
<upload>` is a size argument and a call, not a pre-filled array of matrices.

## What this settles

- **`renderer+0x1130` is not a constant-buffer ring of `float4x4` slots.** The
  source comment said so for many builds. A live 2048-byte read there, and now
  a follow-the-pointer survey across 81 files, both find no matrix data. The
  comment was written from the disassembly without ever reading the bytes.
- **The view/projection matrix is not reachable by walking renderer pointers.**
  Not in the first 0x2000 bytes, and not through any pointer in it. If the
  mod is going to rewrite matrices for stereo, it has to intercept the CPU-side
  code that fills the `SceneDynamicBuffer` — which is what `RE_MATRIX_PATH.md`
  is for — or copy into the structured buffer the game creates, rather than
  expecting to find a live matrix in the renderer.
- **The SDB is a structured buffer, not plain memory.** The game creates it with
  `createStructuredBuffer` at 0x1F4 bytes. It is reachable through a
  `ID3D12Resource` handle, not as a readable CPU pointer, so reading it from the
  mod requires either intercepting the upload or mapping a shadow copy.

## The pattern that produced this file

Four separate times in this project, a claim was written into a comment or a
note and then built on. All four were refuted by measurement:

| claim | where it came from | what killed it |
|---|---|---|
| `+0xE40` is the swapchain | code comment | true only for `endRender`'s object |
| `+0x1130` is a matrix ring | disassembly comment | byte dump: no matrix data |
| slot 9 needs two derefs | habit from C++ vtable patterns | D3D12 vtable slot 0 is data |
| "process alive" means the game runs | `Get-Process` | frozen game returns `True` |

Measure before believing, especially anything *you* wrote down earlier.
