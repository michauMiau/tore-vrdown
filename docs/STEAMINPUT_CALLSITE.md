# Steam Input (ISteamInput) call sites — DECRYPTED Teardown build

**Binary analysed:** `/root/steamless/Teardown/teardown.exe`
PE32+, image base `0x140000000`, 30,333,440 bytes, decrypted/unpacked analysis build.

> ### ⚠ BUILD-SPECIFIC — RE-MEASURE BEFORE HOOKING RETAIL
> Every RVA in this document is valid **only** for the analysis build above. It is
> **not** the same file as the retail build on the live VM
> (`c8228c87`, 30,555,208 bytes) — different size, different content. Before
> anyone patches or hooks any address here on retail, it **must be re-measured
> on that binary**. The *structure* (selector global, 48-slot vtable, the
> `lea rcx,[rip+X]; call [rip+Y]; mov rcx,[rax]; mov rax,[rcx]; call [rax+K]`
> idiom) is a compiler property and will almost certainly survive; the numeric
> addresses almost certainly will not. Re-run
> `tools/tdcalls/s39_resolve.py` against the retail binary to regenerate the
> whole table.

---

## 1. Summary — the answer to the interception question

**Yes. The game reads controller state through `ISteamInput::GetDigitalActionData`
and `ISteamInput::GetAnalogActionData`, at exactly two chokepoints, and both are
hookable.**

| Chokepoint | RVA (vcall) | Slot | Called from | Calls/frame |
| --- | --- | --- | --- | --- |
| **Digital read** | **`0x004E08ED`** | **17 (`0x88`)** `GetDigitalActionData` | wrapper `0x004E0890..0x004E0940` | **37× per frame** |
| **Analog read** | **`0x004E020F`, `0x004E02C8`, `0x004E0369`, `0x004E040D`, `0x004E04D0`** | **21 (`0xA8`)** `GetAnalogActionData` | `0x004E016D..0x004E0530` | **5× per frame** |

The digital read is a **fully unrolled loop** of 37 `call 0x004E0890` sites in
`fn 0x004DFE8E..0x004E016D`, each passing a different action index in `r8b`.
Each wrapper call re-fetches the interface and makes exactly one
`call [r10+0x88]`. There is **no caching of the ISteamInput pointer across
the read** — the dispatch happens per read. That makes `0x004E08ED` a clean
hook: intercept it, return a synthetic `InputDigitalActionData_t` in `al`/`[rsp+0x40]`,
and the game consumes it as if it were hardware.

**Important correction to earlier notes in this project:** a
caller-trace previously suggested the hot action-read loop was around
`0x004DD30A`. That is **partially refuted**. `0x004DD30A` is real and is the
`lea rcx,[rip+...]` that selects the Steam Input interface — but the function
containing it (`fn 0x004DD2F0..0x004DDD70`) is **handle *creation***, not
reading. It is the init function that calls `GetDigitalActionHandle` 37 times
and `GetAnalogActionHandle` 5 times, one per action name. The actual *read*
loop is `fn 0x004DFE8E..0x004E016D`, reached through the wrapper at
`0x004E0890`.

---

## 2. Method — and the rules actually applied

This analysis is strict about four things that previously went wrong in this
project.

1. **`.pdata` unwind-table function boundaries, never a linear sweep.** All
   function starts/ends come from the PE exception directory. A linear sweep
   produces plausible-looking garbage.
2. **A rip-relative displacement is not an address.** Every rip target in this
   document was *resolved*: `target = rva + instruction_length + displacement`.
   No number here was found by searching a displacement column. Byte encodings
   are shown so each line can be re-verified directly.
3. **Dataflow, not co-location.** A vtable call counts as an ISteamInput call
   only if the pointer provably derives from the SteamInput selector:
   `lea rcx,[rip → 0x00C48408]` → `call qword ptr [rip → 0x009831B0]` →
   `rax` is the interface → tracked symbolically through register moves →
   the vtable slot load. The 263 total calls through the dispatcher include
   **194 that select other interfaces**; those are excluded.
4. **Slot numbers come from the real DLL, not from a header.** The vtable was
   resolved from `/root/steamless/Teardown/steam_api64.dll` (7,305,128 bytes,
   PE32+, base `0x180000000`), vtable at RVA `0x54B738`, **48 entries**.
   Proof chain and per-slot evidence: `tools/steaminput/ISteamInput_vtable.md`.
   **30 of 48 slots are binary-proved. 18 are genuinely ambiguous** because
   several exported names share one MSVC this-adjusting thunk — those are
   marked `AMBIGUOUS` in the table and are **not** guessed at.

Reproduce with:

```
cd /home/truenas_admin/teardown-vr-mod/tools/tdcalls
python3 s39_resolve.py     # 263 dispatch sites -> 69 Steam Input -> 0 unresolved
python3 s40_emit.py        # per-site table with arguments
```

### The interface fetch itself

There is exactly **one** RIP-relative reference to the string `SteamInput006`
in this binary. The selector global is:

| what | RVA | note |
| --- | --- | --- |
| Steam Input selector global | **`0x00C48408`** | `lea rcx, [rip + 0x76B0F7]` @ `0x004DD30A` resolves here |
| Steam interface dispatcher (IAT slot) | **`0x009831B0`** | the call target; import name reads `SteamInternal_ContextInit` but the **return type is a pointer-to-interface**, i.e. it is Steam's generic `CreateInterface`-style resolver |
| The returned interface is pointer-to-pointer | — | callers do `mov rcx,[rax]` then `mov rax,[rcx]` before the vtable load |

The returned `ISteamInput*` is **not** stored in a game global. Every one of
the 69 call sites re-resolves it through the selector. Consequence for hooking:
there is no single cached pointer to patch — you hook the slot call itself, or
you patch the `0x009831B0` IAT slot to return your own wrapper.

---

## 3. The table

All 69 call sites. `disp` = the interface fetch (`call qword ptr [rip → 0x009831B0]`)
RVA. `vcall` = the actual vtable call RVA (the instruction to patch/hook).
`arg` = the `rdx` argument.

### 3a. Handle creation — `fn 0x004DD2F0..0x004DDD70` (init, called from `0x004DF1A7`)

`GetDigitalActionHandle` (slot 16 / `0x80`), 37 sites. Each passes a literal
action-name string in `rdx`; the returned 8-byte handle is stored into a
container at `[rsi+0x58]` with the action index.

| disp | vcall | slot | method | rdx argument |
| ---: | ---: | ---: | --- | --- |
| `0x004DD311` | `0x004DD324` | 16 | `GetDigitalActionHandle` | `"flashlight"` @ `0x009DEEA0` |
| `0x004DD34D` | `0x004DD360` | 16 | `GetDigitalActionHandle` | `"interact"` @ `0x009A9530` |
| `0x004DD389` | `0x004DD39C` | 16 | `GetDigitalActionHandle` | `"jump"` @ `0x009DEEAC` |
| `0x004DD3C5` | `0x004DD3D8` | 16 | `GetDigitalActionHandle` | `"crouch"` @ `0x009DEEB4` |
| `0x004DD401` | `0x004DD414` | 16 | `GetDigitalActionHandle` | `"usetool"` @ `0x009DEEC0` |
| `0x004DD43D` | `0x004DD450` | 16 | `GetDigitalActionHandle` | `"grab"` @ `0x009DEEC8` |
| `0x004DD479` | `0x004DD48C` | 16 | `GetDigitalActionHandle` | `"handbrake"` @ `0x009DEF00` |
| `0x004DD4B5` | `0x004DD4C8` | 16 | `GetDigitalActionHandle` | `"map"` @ `0x009DEF0C` |
| `0x004DD4F1` | `0x004DD504` | 16 | `GetDigitalActionHandle` | `"scoreboard"` @ `0x009DEFD8` |
| `0x004DD52D` | `0x004DD540` | 16 | `GetDigitalActionHandle` | `"pause_menu"` @ `0x00A4D1B8` |
| `0x004DD569` | `0x004DD57C` | 16 | `GetDigitalActionHandle` | `"vehicle_action"` @ `0x009DEED0` |
| `0x004DD5A5` | `0x004DD5B8` | 16 | `GetDigitalActionHandle` | `"vehicle_lower"` @ `0x009DEEF0` |
| `0x004DD5E1` | `0x004DD5F4` | 16 | `GetDigitalActionHandle` | `"vehicle_raise"` @ `0x009DEEE0` |
| `0x004DD61D` | `0x004DD630` | 16 | `GetDigitalActionHandle` | `"scroll_down"` @ `0x009DEF28` |
| `0x004DD659` | `0x004DD66C` | 16 | `GetDigitalActionHandle` | `"scroll_up"` @ `0x009DEF18` |
| `0x004DD695` | `0x004DD6A8` | 16 | `GetDigitalActionHandle` | `"tool_group_prev"` @ `0x009DEF38` |
| `0x004DD6D1` | `0x004DD6E4` | 16 | `GetDigitalActionHandle` | `"tool_group_next"` @ `0x009DEF48` |
| `0x004DD70D` | `0x004DD720` | 16 | `GetDigitalActionHandle` | `"mouse_lmb"` @ `0x00A4D1C8` |
| `0x004DD749` | `0x004DD75C` | 16 | `GetDigitalActionHandle` | `"mouse_mmb"` @ `0x00A4D1D8` |
| `0x004DD785` | `0x004DD798` | 16 | `GetDigitalActionHandle` | `"mouse_rmb"` @ `0x00A4D1E8` |
| `0x004DD7C1` | `0x004DD7D4` | 16 | `GetDigitalActionHandle` | `"extra0"` @ `0x009DEF88` |
| `0x004DD7FD` | `0x004DD810` | 16 | `GetDigitalActionHandle` | `"extra1"` @ `0x009DEF90` |
| `0x004DD839` | `0x004DD84C` | 16 | `GetDigitalActionHandle` | `"extra2"` @ `0x009DEF98` |
| `0x004DD875` | `0x004DD888` | 16 | `GetDigitalActionHandle` | `"extra3"` @ `0x009DEFA0` |
| `0x004DD8B1` | `0x004DD8C4` | 16 | `GetDigitalActionHandle` | `"extra4"` @ `0x009DEFA8` |
| `0x004DD8ED` | `0x004DD900` | 16 | `GetDigitalActionHandle` | `"extra5"` @ `0x009DEFB0` |
| `0x004DD929` | `0x004DD93C` | 16 | `GetDigitalActionHandle` | `"extra6"` @ `0x009DEFB8` |
| `0x004DD965` | `0x004DD978` | 16 | `GetDigitalActionHandle` | `"menu_left"` @ `0x009DEFE8` |
| `0x004DD9A1` | `0x004DD9B4` | 16 | `GetDigitalActionHandle` | `"menu_right"` @ `0x009DEFF8` |
| `0x004DD9DD` | `0x004DD9F0` | 16 | `GetDigitalActionHandle` | `"menu_up"` @ `0x009DF008` |
| `0x004DDA19` | `0x004DDA2C` | 16 | `GetDigitalActionHandle` | `"menu_down"` @ `0x009DF010` |
| `0x004DDA55` | `0x004DDA68` | 16 | `GetDigitalActionHandle` | `"menu_next"` @ `0x009DF020` |
| `0x004DDA91` | `0x004DDAA4` | 16 | `GetDigitalActionHandle` | `"menu_prev"` @ `0x009DF030` |
| `0x004DDACD` | `0x004DDAE0` | 16 | `GetDigitalActionHandle` | `"menu_accept"` @ `0x009DF040` |
| `0x004DDB09` | `0x004DDB1C` | 16 | `GetDigitalActionHandle` | `"menu_cancel"` @ `0x009DF050` |
| `0x004DDB45` | `0x004DDB58` | 16 | `GetDigitalActionHandle` | `"photomode"` @ `0x009DEFC0` |
| `0x004DDB81` | `0x004DDB94` | 16 | `GetDigitalActionHandle` | `"zoom"` @ `0x009DEFCC` |

`GetAnalogActionHandle` (slot 20 / `0xA0`), 5 sites, same function:

| disp | vcall | slot | method | rdx argument |
| ---: | ---: | ---: | --- | --- |
| `0x004DDBBD` | `0x004DDBD0` | 20 | `GetAnalogActionHandle` | `"Move"` @ `0x009FAA5C` |
| `0x004DDBFC` | `0x004DDC0F` | 20 | `GetAnalogActionHandle` | `"Camera"` @ `0x00A4D1F4` |
| `0x004DDC3B` | `0x004DDC4E` | 20 | `GetAnalogActionHandle` | `"Mouse"` @ `0x009F3344` |
| `0x004DDC7A` | `0x004DDC8D` | 20 | `GetAnalogActionHandle` | `"throttle"` @ `0x00A4D200` |
| `0x004DDCB9` | `0x004DDCCC` | 20 | `GetAnalogActionHandle` | `"brake"` @ `0x00A4D20C` |

`GetActionSetHandle` (slot 9 / `0x48`), 3 sites, same function:

| disp | vcall | slot | method | rdx argument |
| ---: | ---: | ---: | --- | --- |
| `0x004DDCF8` | `0x004DDD0B` | 9 | `GetActionSetHandle` | `"PlayerControls"` @ `0x00A4D218` |
| `0x004DDD19` | `0x004DDD2C` | 9 | `GetActionSetHandle` | `"DrivingControls"` @ `0x00A4D228` |
| `0x004DDD3A` | `0x004DDD4D` | 9 | `GetActionSetHandle` | `"MenuControls"` @ `0x00A4D238` |

### 3b. THE READ PATH — `GetDigitalActionData`, slot 17

One vtable call, `0x004E08ED`, inside the wrapper `fn 0x004E0890..0x004E0940`,
reached from dispatch `0x004E08D6`. Verified bytes:

```
004E08CF  48 8d 0d 32 7b 76 00   lea    rcx, [rip + 0x767b32]   ; -> 0x00C48408
004E08D6  ff 15 d4 28 4a 00      call   qword ptr [rip + 0x4a28d4] ; -> 0x009831B0
004E08DC  4c 8b 06               mov    r8, qword ptr [rsi]     ; r8  = this
004E08DF  48 8d 54 24 40         lea    rdx, [rsp + 0x40]       ; rdx = &InputDigitalActionData_t
004E08E4  4c 8b cd               mov    r9, rbp                 ; r9  = eActionOrigin (controller handle)
004E08E7  48 8b 08               mov    rcx, qword ptr [rax]    ; rcx = interface
004E08EA  4c 8b 11               mov    r10, qword ptr [rcx]    ; r10 = vtable
004E08ED  41 ff 92 88 00 00 00   call   qword ptr [r10 + 0x88]  ; <<< SLOT 17
004E08F4  80 7c 24 41 00         cmp    byte ptr [rsp + 0x41], 0 ; .bState
004E08FB  0f b7 44 24 40         movzx  eax, word ptr [rsp + 0x40] ; .x & .y packed
```

**This is the single best hook in the binary.** The output struct is
`InputDigitalActionData_t` at `[rsp+0x40]`: word at `+0x40` is `x|y<<8`
(the Steam "active" bitmask), byte at `+0x41` is `bState`, and the
controller handle (`InputActionHandle_t`) is `r9`, which the wrapper computed
from the action index `r8b` by a hash-lookup table walk in
`fn 0x004DBBA0`. Results are written back to `[rbx+8]` / `[rbx+9]` / `[rbx+0xA]`
(pressed / pressed-this-frame / released-this-frame).

The 37 unrolled callers in `fn 0x004DFE8E..0x004E016D`, with the action index
passed in `r8b` and the action name from the 51-entry name table at
`0x009DEB58` (stride 16: `qword handle; qword pad; qword name; qword pad`):

| call site | `r8b` set at | idx | action name | call site | `r8b` set at | idx | action name |
| ---: | ---: | ---: | --- | ---: | ---: | ---: | --- |
| `0x004DFE99` | caller | — | (see note) | `0x004DFFFE` | `0x004DFFF2` | 29 | `extra1` |
| `0x004DFEAA` | `0x004DFE9E` | 6 | `interact` | `0x004E000F` | `0x004E0003` | 30 | `extra2` |
| `0x004DFEBB` | `0x004DFEAF` | 7 | `jump` | `0x004E0020` | `0x004E0014` | 31 | `extra3` |
| `0x004DFECC` | `0x004DFEC0` | 8 | `crouch` | `0x004E0031` | `0x004E0025` | 32 | `extra4` |
| `0x004DFEDD` | `0x004DFED1` | 9 | `usetool` | `0x004E0042` | `0x004E0036` | 33 | `extra5` |
| `0x004DFEEE` | `0x004DFEE2` | 10 | `grab` | `0x004E0053` | `0x004E0047` | 34 | `extra6` |
| `0x004DFEFF` | `0x004DFEF3` | 14 | `handbrake` | `0x004E0064` | `0x004E0058` | 44 | `menu_accept` |
| `0x004DFF10` | `0x004DFF04` | 15 | `map` | `0x004E0075` | `0x004E0069` | 45 | `menu_cancel` |
| `0x004DFF21` | `0x004DFF15` | 37 | `scoreboard` | `0x004E0086` | `0x004E007A` | 40 | `menu_up` |
| `0x004DFF32` | `0x004DFF26` | 16 | `pause` | `0x004E0097` | `0x004E008B` | 41 | `menu_down` |
| `0x004DFF43` | `0x004DFF37` | 11 | `vehicle_action` | `0x004E00A8` | `0x004E009F` | 38 | `menu_left` |
| `0x004DFF54` | `0x004DFF48` | 13 | `vehicle_lower` | `0x004E00B9` | `0x004E00AD` | 39 | `menu_right` |
| `0x004DFF65` | `0x004DFF59` | 12 | `vehicle_raise` | `0x004E00CA` | `0x004E00BE` | 42 | `menu_next` |
| `0x004DFF76` | `0x004DFF6A` | 18 | `scroll_down` | `0x004E00DB` | `0x004E00CF` | 43 | `menu_prev` |
| `0x004DFF87` | `0x004DFF7B` | 17 | `scroll_up` | `0x004E00EC` | `0x004E00E0` | 35 | `photomode` |
| `0x004DFF98` | `0x004DFF8F` | 19 | `tool_group_prev` | `0x004E00FD` | `0x004E00F1` | 36 | `zoom` |
| `0x004DFFA9` | `0x004DFF9D` | 20 | `tool_group_next` | | | | |
| `0x004DFFBA` | `0x004DFFAE` | 21 | `lmb` | | | | |
| `0x004DFFCB` | `0x004DFFBF` | 22 | `mmb` | | | | |
| `0x004DFFDC` | `0x004DFFD0` | 23 | `rmb` | | | | |
| `0x004DFFED` | `0x004DFFE1` | 28 | `extra0` | | | | |

The loop is software-pipelined: `mov r8b, N` after call *k* supplies call *k+1*'s
action index, so **36 of 37 indices are read from a `mov r8b` byte inside the
function**; the first (`0x004DFE99`) takes its index from the caller and is
**not resolvable from this function alone** — see UNVERIFIED §5.1. Results are
OR'd into `sil` (`or sil, al`), i.e. an "any action pressed" aggregate.

### 3c. THE READ PATH — `GetAnalogActionData`, slot 21

5 vtable calls, in `fn 0x004E016D..0x004E0530` (the function is split across two
`.pdata` entries). Shape: `mov r15,[rax]` / `mov rax,[r15]` /
`mov r12,[rax+0xA8]` / … / `call r12` — the vtable slot is loaded into `r12`
and called indirectly, which is why a naive `[rax+K]` scan misses these.

| disp | vcall | slot | method | controller index (rdx) | stores to |
| ---: | ---: | ---: | --- | --- | --- |
| `0x004E01CB` | `0x004E020F` | 21 | `GetAnalogActionData` | `edi` (caller-supplied) | `[rbx+0xD4]`, `[rbx+0xD8]` |
| `0x004E0280` | `0x004E02C8` | 21 | `GetAnalogActionData` | `3` | `[rbx+0xEC]` |
| `0x004E0321` | `0x004E0369` | 21 | `GetAnalogActionData` | `4` | `[rbx+0xF4]` |
| `0x004E03C8` | `0x004E040D` | 21 | `GetAnalogActionData` | `r13d` (caller-supplied) | `[rbx+0xDC]`, `[rbx+0xE0]` |
| `0x004E048A` | `0x004E04D0` | 21 | `GetAnalogActionData` | `2` | `[rbx+0xE4]`, `[rbx+0xE8]` |

The `rdx` argument here is a `ControllerHandle_t`, not a string. The handle is
produced by `call 0x004DBD70` — a hash-table lookup over the action-handle
container at `[rcx+0x10]` that emits `"Warning: more th…"` @ `0x0098EA68` when
a bucket chain exceeds 100 entries. Analog destinations are `float`s written
with `movss`, and are compared against the FLT epsilon `0x00986BF0` before
setting an "active" flag in `sil`.

### 3d. Everything else — 21 sites

| disp | vcall | slot | method | what it does |
| ---: | ---: | ---: | --- | --- |
| `0x004DF190` | `0x004DF19E` | 0 | `Init` | `mov dl,1`; on success (`al!=0`) calls the handle-creation fn and sets `[rbx+0x150]`, `[rbx+0x151]` |
| `0x004D97A8` | `0x004D97B4` | 1 | `Shutdown` | the actual vtable call |
| `0x004D9795` | *(shares `0x004D97B4`)* | 1 | `Shutdown` | **fetch + null-check only** — see note |
| `0x004DF8F9` | `0x004DF907` | 3 | `RunFrame` | per-frame pump; sits in `fn 0x004DF859..0x004DFA94` next to `ActivateActionSet` |
| `0x004DB84B` | `0x004DB85B` | 11 | `GetCurrentActionSet` | **fallback**: a 3-way `dec ecx`/`je` chain picks a cached handle from `[rdi+0x50]`/`[rdi+0x40]`/`[rdi+0x48]`; only the fallthrough fetches `GetCurrentActionSet`, `rdx = [r12]`, result into `rsi` |
| `0x004DF8A7` | `0x004DF8E1` | 10 | `ActivateActionSet` | `r8` picked by a 4-way `cmp sil,{0,1,3}` from `[rbx+0x40/0x48/0x50]`; `rdx` = `[rbx+0x38]` |
| `0x004DB8E9` | `0x004DB910` | 18 | `GetDigitalActionOrigins` | `mov r10,[rdx+0x90]; … ; call r10` |
| `0x004DB9BE` | `0x004DB9DE` | 22 | `GetAnalogActionOrigins` | — |
| `0x004DBA0E` | `0x004DBA2D` | **23** | **`AMBIGUOUS`** | `jmp qword ptr [rax+0xB8]` tail call; `r8=0, r9=0, edx=ebp` |
| `0x004DDF47` | `0x004DDF5B` | **23** | **`AMBIGUOUS`** | second call into the same ambiguous slot |
| `0x004DDEFC` | `0x004DDF0B` | 37 | `GetInputTypeForHandle` | — |
| `0x004DAC17` | `0x004DAC26` | 37 | `GetInputTypeForHandle` | `rdx` = `"gamepad_ds4"` @ `0x00A4D258` |
| `0x004E0B9E` | `0x004E0BB8` | 37 | `GetInputTypeForHandle` | `call r8` form |
| `0x004DDF1A` | `0x004DDF32` | 43 | `TranslateActionOrigin` | `call r9` form, in `fn 0x004DDE02..0x004DDF40` |
| `0x004E0BEF` | `0x004E0C16` | 31 | `TriggerVibrationExtended` | in the haptics fn `0x004E0A2C..0x004E0C31` |
| `0x004DE15C` | `0x004DE174` | 36 | `ShowBindingPanel` | `jmp` tail call; `rdx` = `[[rbx+0x38]]` |
| `0x004E0ABC` | `0x004E0AD3` | 47 | `SetDualSenseTriggerEffect` | `rdx` = `[[r15+0x38]]`; 0x78-byte trigger-effect struct built on stack at `[rbp-0x49]`, result copied to `[rdi]` |
| `0x004E0C92` | `0x004E0CBF` | 33 | `SetLEDColor` | `fn 0x004E0C31..0x004E0CDD` |

**Slot 23 is left UNMAPPED on purpose.** The binary proves *which* entry it
reaches (`impl 0xB7CA0`, `this=0x48`) but that same this-adjusting thunk is
shared by `GetGlyphForActionOrigin_Legacy`, `GetGlyphPNGForActionOrigin` and —
per the header order — `GetStringForAnalogActionName`. The binary cannot
separate them. Given the two call sites sit in glyph/label code
(`fn 0x004DBA07..0x004DBB0A` and `0x004DDF40..0x004DDF69`), the header
cross-check points at `GetStringForAnalogActionName`, but that is **not proof**
and is not used as a name anywhere in this project. The earlier
`TriggerVibration` guess in this codebase was wrong for a different reason and
is documented in `tools/steaminput/ISteamInput_vtable.md`.

---

## 4. Counts

| metric | value |
| --- | --- |
| calls through the generic Steam dispatcher `0x009831B0` | **263** |
| …of which select Steam Input (`0x00C48408`) | **69** |
| …unresolved after symbolic dataflow | **0** |
| …of which are a *second fetch of an interface that is null-checked but not called* | **1** (`0x004D9795`) |
| **distinct vtable-call instructions** | **68** (`0x004D9795` and `0x004D97A8` share the vcall at `0x004D97B4`) |
| RIP-relative refs to the string `SteamInput006` | **1** |
| distinct vtable slots touched | **18** (of 48) |
| `GetDigitalActionData` sites (the read chokepoint) | **1** (`0x004E08ED`), called 37×/frame |
| `GetAnalogActionData` sites (the read chokepoint) | **5** |

---

## 5. UNVERIFIED — what I could not prove

Everything below is honestly open. Do not treat it as fact.

**5.1 The first digital read's action index is unresolved.**
`0x004DFE99` is the first of the 37 reads, but its `r8b` is never set inside
`fn 0x004DFE8E..0x004E016D` — the pipelined `mov r8b, N` *after* a call
supplies the *next* call's index. `fn 0x004DFE8E..0x004E016D` has **no static
callers** in the indexed instruction set (verified: zero `call` sites resolve
to `0x004DFE8E`), so it is reached by a register-indirect or vtable call I did
not chase. I could not name this action. Indices 0-5 are all valid handle
table slots, so the candidates are `none`/`left`/`right`/`up`/`down`/`flashlight`
— but that is a guess, not a result. **To resolve: break on `0x004DFE99` in a
debugger and read `r8b`.**

**5.2 The two `edi`/`r13d` controller indices in the analog read are
caller-supplied** (`0x004E01CB` and `0x004E03C8`). They are `EInputControllerPad`
style indices, not action indices, and their provenance is inside
`fn 0x004E016D`'s caller, which I did not trace. Three of the five are literal
(`2`,`3`,`4`).

**5.3 Eighteen of 48 vtable slots are `AMBIGUOUS` in the real DLL** because
MSVC's this-adjusting thunks are shared between several exported names. The
report names **only** slots with `IDENTITY`, `DIRECT` or `ALIGNED` evidence.
Slot 23 is used by the game and is left unmapped. The per-slot evidence table
and the list of undecidable slots is in `tools/steaminput/ISteamInput_vtable.md`.

**5.4 The `0x009831B0` import name is misleading.** Raw import metadata calls it
`steam_api64!SteamInternal_ContextInit`, but every call site uses its return
value as a pointer-to-interface and dereferences it. The semantic role is Steam's
generic interface resolver. I have **not** proved which export that IAT slot
actually binds to at runtime, because the IAT in this dump is unbound (slots
hold hint/name RVAs, not resolved pointers). It does not affect the table — the
selector string and the vtable identity are proved independently — but do not
trust the import name.

**5.5 Not traced: the action-set activation and origin paths.** `RunFrame` at
`0x004DF907` is inside `fn 0x004DF859..0x004DFA94`; I established that it sits
beside `ActivateActionSet` and that the function is a per-frame update, but I
did not prove it is *the* top-level frame callback. Same for the glyph/origin
functions `fn 0x004DB887..0x004DBA07` and `fn 0x004DDE02..0x004DDF69` — the
calls are proved, their role is not.

**5.6 `0x00C48420` is NOT Steam Input.** While reading the neighbourhood I found
a second selector global `0x00C48420` used at `0x004E0974` with a **slot
`0x230`** vtable call at `0x004E09AC`. `0x230` is outside the 48-entry
`ISteamInput006` vtable, so this is a different Steam interface. It is
**excluded** from the table. I did not identify which interface it is.

Verified bytes:

```
004E0974  48 8d 0d a5 7a 76 00   lea    rcx, [rip + 0x767aa5]   ; -> 0x00C48420
004E097B  ff 15 2f 28 4a 00      call   qword ptr [rip + 0x4a282f] ; -> 0x009831B0
...
004E09AC  ff 90 30 02 00 00      call   qword ptr [rax + 0x230]  ; <<< NOT ISteamInput
```

**5.6b `0x004D9795` is not itself a vtable call.** Bytes:

```
004D9795  ff 15 15 9a 4a 00      call   qword ptr [rip + 0x4a9a15]  ; fetch, same selector
004D979B  48 83 38 00            cmp    qword ptr [rax], 0         ; null check ONLY
004D979F  74 17                  je     0x1404d97b8                 ; skip on null
004D97A1  48 8d 0d 60 ec 76 00   lea    rcx, [rip + 0x76ec60]        ; -> 0x00C48408
004D97A8  ff 15 02 9a 4a 00      call   qword ptr [rip + 0x4a9a02]  ; fetch again
004D97AE  48 8b 08               mov    rcx, qword ptr [rax]
004D97B1  48 8b 01               mov    rax, qword ptr [rcx]
004D97B4  ff 50 08               call   qword ptr [rax + 8]          ; <<< the only vcall
```

So `0x004D9795` is a *guard*: it obtains the interface and branches on null,
and the actual `Shutdown` happens at the shared vcall `0x004D97B4` reached via
`0x004D97A8`. It is listed as a site for completeness (it is a Steam Input
interface fetch) but the patchable instruction is `0x004D97B4`.

**5.6c `GetDigitalActionOrigins` at `0x004DB910` is proved by the slot load,
not the displacement.** Bytes:

```
004DB8F5  48 8b 08               mov    rcx, qword ptr [rax]    ; interface
004DB902  48 8b 0a               mov    rdx, qword ptr [rcx]    ; vtable
004DB905  4c 8b 8a 90 00 00 00   mov    r10, qword ptr [rdx + 0x90]  ; <<< slot 18
004DB90C  48 8b 12               mov    rdx, qword ptr [r12]
004DB910  41 ff d2               call   r10
```

Slot 18 = byte `0x90` = `GetDigitalActionOrigins`, `IDENTITY` evidence in the
DLL map (impl `0xB4FF0`, `this=0x38`, reached only by
`SteamAPI_ISteamInput_GetDigitalActionOrigins`, ord 353). The name is not
inferred from the offset.

**5.7 Retail build.** Everything above is analysis-build-only. See the warning
at the top.

---

## 6. Practical hooking guidance (from the evidence, not speculation)

- **Cleanest single point: `0x004E08ED`** (`call qword ptr [r10 + 0x88]`). One
  instruction, one slot, called 37× per frame from one wrapper. The output
  struct is a stack local at `[rsp+0x40]`, so a trampoline can write it
  directly; return in `al`.
- **Analog: `0x004E020F` / `0x004E02C8` / `0x004E0369` / `0x004E040D` /
  `0x004E04D0`.** These are `call r12` with the slot pre-loaded, so they are
  *not* single-byte patches — hook the vtable slot or patch the `mov r12,[rax+0xA8]`
  load at `0x004E01E1` et seq.
- **There is no cached `ISteamInput*` global to patch.** The interface is
  re-resolved through `0x00C48408` + `0x009831B0` at every one of the 69 sites.
  Redirecting `0x009831B0` to a wrapper that returns your own fake interface
  with your own vtable intercepts **all 69** call sites uniformly, and is
  probably the more robust approach for a mod that wants to also intercept
  `GetConnectedControllers` and friends.
- **Handle state is per-index, not per-controller**: handles are cached in a
  container reached through `[obj+0x58]`, indexed by the 51-entry table at
  `0x009DEB58`. A fake interface must return a stable, unique handle per
  action, because the game does not re-`GetDigitalActionHandle` every frame —
  only at init.
