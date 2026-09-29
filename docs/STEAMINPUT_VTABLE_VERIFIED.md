# Independent verification of the ISteamInput vtable slots

`verify_slots.py` re-derives the slot indices in `STEAMINPUT_VTABLE.md` from
the raw bytes of the real Valve DLL, sharing no code with the analysis that
produced the report. It exists because that report's own notes list three
ways the analysis went wrong before it got the right answer, and each of those
failures produces a plausible-looking wrong number rather than an obvious one.

## Input

| | |
|---|---|
| path | `D:\SteamLibrary\steamapps\common\Teardown\steam_api64.dll` |
| size | 298,384 bytes |
| sha256 | `1db3fd414039d3e5815a5721925dd2e0a3a9f2549603c6cab7c49b84966a1af3` |

The hash was computed on the VM and again locally after `scp`; both agree. This
is the real Valve SDK DLL. A 7.3 MB file of the same name exists elsewhere on
the machine and is a proxy — analysing that one would be analysing the wrong
binary.

## Result: 6/6 confirmed

| method | claimed | measured | form |
|---|---:|---:|---|
| `RunFrame` | 3 | **3** | `jmp qword [rax+0x18]` |
| `GetConnectedControllers` | 6 | **6** | `jmp qword [rax+0x30]` |
| `GetDigitalActionHandle` | 16 | **16** | `jmp qword [rax+0x80]` |
| `GetDigitalActionData` | 17 | **17** | `call qword [rax+0x88]` |
| `GetAnalogActionHandle` | 20 | **20** | `jmp qword [rax+0xa0]` |
| `GetAnalogActionData` | 21 | **21** | `call qword [rax+0xa8]` |

## Whole-table consistency

Measured across all 45 `SteamAPI_ISteamInput_*` thunks:

- **45 exports → 45 distinct slots.** No duplicate slot, which a real vtable
  cannot have.
- **Gaps at 0, 18, 22 only.** Those three slots have no flat-API thunk, which
  is exactly why they are absent. A contiguous 0–47 vtable with three
  unthunked holes is the expected shape, not a hole in the analysis.

This matters as a cross-check: a decoder that returns a wrong slot usually
returns the *neighbour's* slot, which shows up immediately as a duplicate. The
first version of this script did precisely that and reported three methods on
slot 19 and two on slot 29.

## Four thunk shapes, all of which appear in this DLL

```
1. pure jump
   48 8b 01                     mov rax,[rcx]
   48 ff 60 18                  jmp qword [rax+0x18]        slot 3

2. tail call through a copied pointer
   48 8b 01                     mov rax,[rcx]
   4c 8b 90 b0 00 00 00         mov r10,[rax+0xb0]          slot 22
   49 ff e2                     jmp r10

3. slot 0
   48 8b 01                     mov rax,[rcx]
   48 ff 20                     jmp qword [rax]             slot 0

4. direct call, with prologue
   40 53                        push rbx
   48 83 ec 30                  sub  rsp,0x30
   48 8b 02                     mov rax,[rdx]
   4c 8b d2                     mov r10,rdx
   49 8b d9                     mov r11,rbx
   48 8d 54 24 20               lea  rdx,[rsp+0x20]
   49 8b ca                     mov  rcx,r10
   ff 90 a8 00 00 00            call qword [rax+0xa8]       slot 21
```

## Three ways this goes wrong

Each of these returned a wrong number silently. They are worth recording
because the wrong answer is a neighbouring plausible slot, not an obvious
failure.

**1. `mov rax,[rcx]` is a vtable load, not a slot access.** Counting it
reports all 48 methods as slot 0.

**2. `jmp r10` is the dispatch, but carries no displacement.** The slot is in
the `mov r10,[rax+0xb0]` immediately before it. A scan that only accepts
memory-form `FF /4` skips these entirely and drifts into the next thunk.

**3. Thunks are `CC`-padded and 16-byte aligned.** Without stopping at the
INT3 run, a shape-2 thunk is followed into the next thunk's bytes and that
thunk's slot is reported for it. This is what produced the duplicates above.
It is also why `.pdata` is no use as a boundary here: these are leaf functions
with no `RUNTIME_FUNCTION` records, so the unwind table has nothing to say.

**Also: `AddressOfNames` and `AddressOfFunctions` are not parallel arrays.**
`AddressOfNames` is sorted by name, `AddressOfFunctions` is in ordinal order,
and the mapping runs through `AddressOfNameOrdinals`. Indexing both with the
same `i` resolves names to plausible but unrelated thunks — in this script it
found 1,065 exports and zero `ISteamInput` ones before the ordinal table was
added.

## Reproduce

```sh
scp vm@192.168.1.6:"D:/SteamLibrary/steamapps/common/Teardown/steam_api64.dll" /tmp/sa64.dll
python3 verify_slots.py /tmp/sa64.dll
```

Exit status 0 only when all six claimed slots match and the table has no
duplicates.
