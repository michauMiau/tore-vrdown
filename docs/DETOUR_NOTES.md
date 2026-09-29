# Detour mechanics — and a bug that would have crashed every frame

Date: 2026-09-26

## The prologues, measured

Decoded from the unpacked Steamless `teardown.exe`:

```
beginRender  0x1405AB9D0
  48 89 5C 24 18      mov [rsp+0x18],rbx        5
  48 89 6C 24 20      mov [rsp+0x20],rbp        5
  56 57               push rsi, rdi            2
  41 56               push r14                 2
  48 83 EC 30         sub rsp, 0x30            4
                                          total = 18 bytes

endRender    0x1405AF270
  48 89 5C 24 18      mov [rsp+0x18],rbx        5
  55 56 57            push rbp/rsi/rdi          3
  41 56 41 57         push r14/r15              4
  48 83 EC 50         sub rsp, 0x50             4
                                          total = 16 bytes
```

## Why a fixed 14-byte steal is wrong

`endRender`'s prologue is 16 bytes, not 14. A classic 14-byte absolute jump
(`FF 25 00000000` + 8-byte address) steals exactly 14 bytes — which lands
*inside* the `48 83 EC 50` instruction:

```
  ...41 56 41 57 | 48 83    <- steal ends here
            EC 50          <- orphaned
```

The trampoline then replays the prologue without `sub rsp, 0x50`. Every
single call to `endRender` would run with the callee's stack frame unallocated
— and `endRender` uses `[rsp+0x84]` early on, so it would scribble over the
caller's stack. It would not crash immediately; it would corrupt memory
progressively and die somewhere unrelated minutes later.

This is the classic "detour splits an instruction" bug, and the crash signature
points nowhere near the cause.

## The fix

`measure_stealable()` walks the prologue instruction family and returns the
true length, so both hooks steal whole instructions:

```
48 89 5C/6C 24 nn   mov [rsp+disp8], reg     5 bytes
50..57               push rax..rdi           1 byte
41 50..41 57         push r8..r15            2 bytes
48 83 EC nn          sub rsp, imm8           4 bytes
```

The matcher is deliberately narrow. Both entry points stay inside this family
for their entire prologue, and a narrow matcher is auditable in a way a
half-written x86 decoder is not.

`build_detour()` then picks the jump form:

- `copy_len <= 5` → 5-byte relative `E9` jump
- `copy_len > 5` → 14-byte absolute `FF 25` jump

and the trampoline always ends with an absolute jump back to
`target + copy_len`, so the original function body runs completely intact.

Verified at runtime:

```
[VR] stealable: begin=18 end=16
[VR] hooks installed: begin=00006ffffab1b9d0(18) end=00006ffffab1f270(16)
```

## Also fixed

**Stack overflow in the detour writer.** An intermediate version built the
patched bytes in a 14-byte local and then `memcpy`'d an 8-byte address at
offset 8 — six bytes past the end. GCC caught it with `-Warray-bounds`. The
final version writes directly into the target's memory.

**Trampoline capacity.** Stolen bytes (up to 18) plus a 14-byte jump back must
fit; `TRAMP_CAP` is 64 and `install_hooks` refuses to install if a prologue
would not fit.
