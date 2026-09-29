# Correction: renderer+0xB90 is not the SceneDynamicBuffer

> **SUPERSEDED 2026-09-27, and the correction below is itself wrong.**
> A subagent tracing the SDB with `.pdata` unwind-table boundaries proved that
> `+0xB90` **is** the `SceneDynamicBuffer` handle, stored as a qword by the
> renderer's own constructor. The two instructions quoted throughout this
> document are real, but they belong to a different function (`0x3E000`, a
> 0x121-byte serialization blob) on a different type. I verified that the bytes
> exist and never checked whose they were.
>
# Live measurement contradicts part of this: renderer+0x22AC reads as ZERO
#
# 2026-09-27, after the report was written. build/read_vp_matrix.ps1 read 64
# bytes at renderer+0x22AC six times during an active level (12240 endRender
# calls, self=000002077a470570, CPU climbing, the same object throughout) and got
# 0.0000 in every slot, every sample. The stricter test also exists specifically
# so a plausible-but-constant read is rejected, and here the read was not even
# plausible.
#
# So the static chain is real but the offsets are not on THIS object, or the
# writer is not running in this build's live path. Two details from the
# disassembly suggest the latter:
#
#   0008770D  call 0x5E7AD0
#   00087715  movups [r12], xmm0          <-- writes to r12, not r15
#   00087742  call 0x5E7AD0
#   0008774A  movups [r15+0x22AC], xmm0  <-- and to r15
#
# There are two adjacent producers, writing to two different base registers.
# r12 is used for a plain matrix at offset 0, r15 for the field at 0x22AC. Whether
# r15 is TRendererD3D12 or a different, larger object is exactly what a live
# measurement would have settled and did not.
#
# Treat the report's addresses as correct for the DECRYPTED ANALYSIS BUILD and
# unconfirmed for the RUNNING Steam build. They are not the same binary. Do not
# write to any of these offsets on the strength of this document alone.
#
# The method in the report is the part that carried over and is worth keeping:
# use .pdata unwind-table boundaries, not a linear sweep, or every excerpt in
# the older docs is misaligned garbage.

## The verified proof that `+0xB90` is the SDB:
>
> ```
> 0009B404  C7 44 24 24 F4 01 00 00    mov  dword [rsp+0x24], 0x1F4   ; 500
> 0009B416  4C 8D 0D 5B 5A 90 00       lea  r9, [rip+0x905A5B]       ; "Renderer::SceneDynamicBuffer"
> 0009B429  FF 90 20 01 00 00          call qword [rax+0x120]
> 0009B437  48 89 83 90 0B 00 00       mov  qword [rbx+0xB90], rax   ; <-- 8-byte store
> ```
>
> The camera matrix is readable from CPU memory at `renderer+0x22AC` (64 bytes,
> four `movups` at `0x8774A`..`0x8776A`), and the injection point is `0x9D4A6`,
> where the 500-byte buffer is still plain stack memory. See
> `docs/RE_MATRIX_PATH.md` and the "The pattern" section below, which now
> records this as the tenth refuted claim and, unlike the others, an error made
> from real evidence attributed to the wrong owner.
>
> Everything else in this file was true when written and is kept for the record.

The project notes have said for several builds that the `SceneDynamicBuffer`
lives at `renderer+0xB90`, created by `createStructuredBuffer` with size
`0x1F4` and stored there. **That is wrong, and the disassembly says so.**

A live 8 KB read of the renderer showed `+0xB90` as zero, which did not match
the notes. Instead of trusting either side, the disassembly settles it.

## What the code actually does with 0xB90

It is used as a single byte, not a pointer:

```
00040324  movzx eax, byte ptr [rdi + 0xB90]
0004032B  mov    byte ptr [rbx + 0xB90], al
```

And it sits inside a run of byte copies, not pointer copies:

```
000401AA  movzx eax, byte ptr [rdi + 0xB74]
000401B1  mov    byte ptr [rbx + 0xB74], al
000401AA  movzx eax, byte ptr [rdi + 0xB75]
...
00040304  movzx eax, byte ptr [rdi + 0xB8D]
00040317  mov    byte ptr [rbx + 0xB8D], al
```

`0xB74` through `0xB93` is a contiguous byte array being copied one byte at a
time, then a `dword` at `0xB94`, `0xB98`, `0xB9C`. This is the object's
**copy constructor**, copying a flag block. `+0xB90` is one of those flags.

## And 0xBA0, which has the highest reference count

`+0xBA0` is referenced 95 times, `+0xB90` only 30, and `+0x9C0` 665 times. But
`+0xBA0` is not a resource handle either. In the same copy constructor it gets
address-taken rather than byte-copied:

```
000401B7  lea rdx, [rdi + 0xBA0]
000401C5  lea rcx, [rbx + 0xBA0]
```

and it is destroyed like a plain pointer:

```
0008459B  mov  rcx, qword ptr [rbx + 0xBA0]
000845A2  test rcx, rcx
000845A5  je   0x1400845AD
000845A7  call 0x1405C1DD0
```

The same `test rcx,rcx` / `call 0x5C1DD0` sequence runs over the whole run
`0xB80, 0xB88, 0xB90, 0xB98, 0xBA0, 0xBA8, 0xBB0, 0xBB8, 0xBC0, 0xBC8, 0xBD0,
0xBD8`, and `0x82350..0x8239D` zeroes that entire run in sequence. It is a
destructed array of twelve owned objects, not one GPU resource.

## Why the notes were wrong

They were built from the string search that found `"Renderer::SceneDynamicBuffer"`
at rva `0x99BF08` and followed one `lea [rip+disp]` reference. That reference
proves a constructor exists and is called once from `0x8368A`. It does **not**
prove the field offset, because the compiler is free to store the result
anywhere, and the `mov r8d, 0x1F4` seen nearby is a size argument to a generic
helper, not a field size.

`0xB90` is a flag byte inside the renderer's flag block. The real SDB handle is
somewhere else, and nothing measured so far has located it.

## What still stands

The verified upload chain is unaffected, because it came from the actual
disassembly of `endRender` rather than from the offset notes:

```
005AF28B  add  rcx, 0xE70
005AF292  call 0x5B3780          ; getter: array[count]
005AF29F  lea  rcx, [rbx + 0x1130]
005AF2A6  mov  r8d, 0x800
005AF2AC  mov  rdx, rax          ; source = an element of the +0xE70 array
005AF2AF  call 0x5B3280          ; upload
```

`+0x1130` is a destination the upload writes into. The source is an element of
the array at `renderer+0xE70`. That is the lead worth following, and it does not
depend on `0xB90` being anything.

## The lesson, sixth occurrence

The SDB offset was not measured. It was inferred from a string search plus one
reference, written down as fact, and built on. A single live read contradicted
it, and the disassembly confirmed which side was wrong. Same shape as every
other one in this project.
