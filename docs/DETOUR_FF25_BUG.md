# The FF 25 detour bug that killed v32-v35

## Symptom

Four consecutive builds installed a detour on `0x5BAEB0`, logged

```
upload hook: installed at 00007ff754baaeb0, trampoline ..., 15 bytes stolen
=== detached after 1 frames ===
```

and the process died on the first call. No `capture: entered` line ever appeared,
so the log said the hook was never reached. It was reached. The process jumped
into garbage.

## Root cause

`build_detour()` encoded the absolute jump as

```c
p[0]=0xFF; p[1]=0x25;
p[2]=p[3]=p[4]=p[5]=p[6]=p[7]=0x00;
memcpy(p + 8, &hook, 8);          // <-- offset 8
for (size_t i = 14; i < DETOUR_LEN; ++i) p[i] = 0x90;
```

`FF 25 disp32` is `jmp qword ptr [rip + disp32]` and is **six** bytes long. A
RIP-relative operand is measured from the byte *after* the instruction, so with
`disp32 = 0` the CPU dereferences the qword at `target + 6`, not `target + 8`.

The code put the pointer two bytes too far, and then the nop loop starting at
byte 14 wrote `0x90` over the pointer's top two bytes. The observed bytes in
memory were

```
FF 25 | 00 00 00 00 | 00 10 28 b8 70 fe 7f | 90 90
      ^ disp32 = 0   ^^ 8 bytes at +8, high half clobbered by the nop fill
```

Read the way the CPU does, `[rip+0]` is `00 00 10 28` little-endian, so the
render thread jumped to `0x28100000` and the process died immediately. The
detour looked installed in the log because installation and correct
installation are indistinguishable from inside the log.

## Fix

```c
p[0] = 0xFF; p[1] = 0x25;
p[2] = p[3] = p[4] = p[5] = 0x00;      // disp32 = 0 -> dereference target+6
memcpy(p + 6, &hook, sizeof(void*));  // the 8-byte target, at target+6
for (size_t i = 14; i < DETOUR_LEN; ++i) p[i] = 0x90;
```

The tail padding starts at byte 14, after the pointer, so it cannot touch it.

## Second bug found in the same function

The trampoline was built as

```c
memcpy(trampoline, target, copy_len);   // AFTER target had been patched
```

so the trampoline held our own jump instead of the original prologue, and
`g_orig_upload` recursed into itself until the stack was exhausted. Fixed by
copying the stolen bytes *before* writing the patch. `build/test_detour.c`
regresses this: the trampoline must still start with the original bytes.

## Offline regression test

`build/test_detour.c` encodes the jump exactly as the real code does, then
decodes it the way the hardware does (`rip = insn_end`, `slot = rip + disp32`),
and asserts the resolved address is the hook. It also asserts DETOUR_LEN covers
the whole steal and that the trampoline holds the original prologue.

It prints what the old layout resolved to, which is the smoking gun:

```
old layout -> 0x7ff712345678cccc   (this is what killed v32..v35)
```

Result: `ALL CHECKS PASS`, `TEST_EXIT=0`.

## Lesson

A hook log that says "installed" cannot distinguish installed-and-working from
installed-and-corrupt. `build/probe_upload_patch.ps1` now reads the patched
bytes from *outside* the process with `ReadProcessMemory` and decodes the jump
the way the CPU does, so a bad encoding is visible without needing the game to
crash first. Run it after any detour change.
