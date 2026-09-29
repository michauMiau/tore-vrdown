# Binary protection — the real blocker

## Finding

`teardown.exe` ships with its **`.text` section encrypted at rest** and
decrypted by a protector stub at process start.

Measured section entropy:

| Section | Entropy | Meaning |
|---|---|---|
| `.text`  | **8.000** | encrypted / packed — uniform random |
| `.rdata` | 5.399 | normal read-only data |
| `.data`  | 0.265 | normal (mostly zero-filled) |
| `.bind`  | 7.960 | the protector stub itself |

An entropy of exactly 8.000 over the whole 9.4 MB `.text` means the code is
not merely compressed — it is indistinguishable from random data on disk.
Disassembling it directly produces garbage:

```
1408b7e90:  bb 22 94 e8 37    mov ebx,0x37e89422
1408b7e95:  10 20             adc BYTE PTR [rax],ah
```

## The protector

Entry point RVA `0x2014310` lands in a section called **`.bind`**, not `.text`.
That stub:

```asm
142014310:  call   0x142014315        ; get EIP
142014315:  push   rax
            push   rbx / rcx / rdx / rsi / rdi / rbp
            push   r8..r15
            mov    rcx, [rsp+0x78]
            sub    rcx, 0x5
            and    rsp, -16
            call   0x1420143d0        ; <- the real unpacker
            ...
            mov    rsp, rcx
            pop    r15..rax
            ret
```

There is also a self-relative indirect jump using an XOR-masked displacement:

```asm
142014382:  sub    rdx, 0xffffffffed59d0e5
14201438c:  mov    edx, DWORD PTR [rdx]
14201438e:  xor    edx, 0x807de135
142014394:  movsxd rdx, edx
142014397:  add    rax, rdx
```

The `int3` sled after the stub (`cc cc cc ...`) is typical protector padding.

This is a commercial protector (Enigma Virtual Box / VMProtect family), applied
by whatever repack the user obtained — note the accompanying `crack.sh`,
`legit.sh`, `steam_emu.ini` and `teardown.exe.unpacked.exe` in the same folder.

## What this changes

### Dead ends
- **Static disassembly of game code is off the table.** No function addresses
  can be read from the file. The `TRendererD3D12::beginRender` etc. strings in
  `.rdata` are log labels only — confirmed by zero cross-references from
  `.text` (which is unreadable anyway) and by them not being contained in any
  `.pdata` unwind entry.
- Pattern-scanning the file for function prologues will not work.
- No `objdump`/`Ghidra`/`IDA` import of this binary yields usable results
  until it is unpacked first.

### Still fine
- `.rdata` is intact: all HLSL shader source, all `mub*` uniform names, the
  `DECLARE_CONSTANT_BUFFER` declarations, class/method name strings.
- `.data` is intact.
- The 73 Lua scripts in `data/script/` are plaintext.
- Dynamic analysis on a live process sees **decrypted** code in memory.

## The unlock

Everything below depends on getting an unpacked image.

### Option A — the `teardown.exe.unpacked.exe` already in the folder
The user has a Steamless-style unpacker tool, and a `teardown.exe.unpacked.exe`
sits next to the original. **Check its entropy first** — if it is a plain PE
with entropy ~6, the hard part is already done.

### Option B — dump from a live process
Run the game under a debugger, let the unpacker finish, then dump the `.text`
range from memory once it is decrypted:

1. Start the game, attach x64dbg / WinDbg after the main menu appears
2. `!address teardown` style lookup, or read the module base
3. Dump `[base + 0x1000, base + 0x97D000)` (the `.text` RVA range)
4. Reassemble the dump as a flat binary and rebuild a PE around it, or load it
   into IDA/Ghidra as a raw dump

This is the standard approach and it works — the packer only protects the
on-disk image, not the running process.

### Option C — hook instead of unpack
If dumping is troublesome, skip static analysis entirely: hook
`D3D12CreateDevice` / `CreateSwapChainForHwnd` in the IAT of the *running*
process, where the real code is already decrypted. The VR mod works fine
without ever seeing the unpacked binary.

## Recommended next step

Check `teardown.exe.unpacked.exe` first — if the user already has a decrypted
image, all of the earlier static work becomes valid and much faster.
Otherwise go with Option C: pure runtime hooking, no offline RE needed.
