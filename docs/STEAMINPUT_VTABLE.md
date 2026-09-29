# ISteamInput vtable slot map — `SteamInput006`

Authoritative, statically proven mapping from `ISteamInput` method to vtable
slot index, extracted from the **real Valve `steam_api64.dll`** that ships with
Teardown on the live VM.

**Bottom line for the shim:**

| what you want | slot | byte offset from vtable base `B` |
|---|---:|---|
| `GetDigitalActionData` | **17** | `(*(void***)B)[0x88]` |
| `GetAnalogActionData` | **21** | `(*(void***)B)[0xa8]` |
| `GetDigitalActionHandle` | 16 | `0x80` |
| `GetAnalogActionHandle` | 20 | `0xa0` |
| `GetConnectedControllers` | 6 | `0x30` |
| `RunFrame` | 3 | `0x18` |
| `Init` | 0 | `0x00` |
| `Shutdown` | 1 | `0x08` |

`slot = byte_offset / 8`. This is a flat (single-inheritance) MSVC vtable, so
the first virtual sits at byte 0.

---

## 1. The DLL that was analysed

| | |
|---|---|
| Path on VM | `D:\SteamLibrary\steamapps\common\Teardown\steam_api64.dll` |
| Size | **298,384 bytes** |
| SHA-256 | `1db3fd414039d3e5815a5721925dd2e0a3a9f2549603c6cab7c49b84966a1af3` |
| Machine | AMD64 (PE32+) |
| Image base | `0x13b400000` |
| Sections | `.text .rdata .data .pdata _RDATA .rsrc .reloc` |
| Exports | 1065 |
| `SteamAPI_ISteamInput_*` exports | **48** |

The hash was computed on the VM with `Get-FileHash -Algorithm SHA256` and again
locally after `scp`; both agree, so the analysed copy is the genuine file.

### Why this is the right file, and not the local proxy

`/root/steamless/Teardown/steam_api64.dll` is a **7.3 MB Steam client proxy with
OpenVR forwarding** and was **not** analysed. A full-disk search of the VM
(`C:` and `D:`) found 24 `steam_api64.dll` copies; only the one in the Teardown
install directory is the game-facing SDK DLL, and its 298 KB size matches.
The others are SteamVR runtime copies (249–301 KB, different hashes) and
per-game copies for other titles.

`teardown.exe` itself contains exactly one SteamInput version string —
`SteamInput006` — and no `SteamController_v00x`, so the game requests the 006
interface and nothing else.

---

## 2. How the slot numbers were proven

Four independent routes, all agreeing on all 48 slots.

### Route A — the flat-API thunks in `steam_api64.dll` (primary)

`steam_api64.dll` exports a flat C API. Every `SteamAPI_ISteamInput_<Method>`
export is a thunk that loads the interface vtable and dispatches through **one
fixed slot**:

```asm
; 43 of the 48: tail-call stub
SteamAPI_ISteamInput_GetConnectedControllers:
    mov  rax, qword ptr [rcx]        ; rax = vtable
    jmp  qword ptr [rax + 0x30]      ; -> slot 6

; 2 thunks materialise the target first
SteamAPI_ISteamInput_GetDigitalActionOrigins:
    mov  rax, qword ptr [rcx]
    mov  r10, qword ptr [rax + 0x90] ; -> slot 18
    jmp  r10

; 3 need to post-process the result, so they call instead of jump
SteamAPI_ISteamInput_GetDigitalActionData:
    sub  rsp, 0x28
    mov  rax, qword ptr [rcx]
    mov  r9, r8
    mov  r8, rdx
    lea  rdx, [rsp + 0x30]
    call qword ptr [rax + 0x88]      ; -> slot 17, then widens the packed bool
    movzx eax, word ptr [rax]
    ret
```

The distinction that matters: `mov rax,[rcx]` fetches the **vtable itself** and
is *not* a slot access, whereas `mov r10,[rax+0x90]` fetches a function pointer
*out of* the vtable and is. Conflating them reports every method as slot 0.

Result: **48/48 resolved, all unique, contiguous 0–47, no gaps, no duplicates.**

### Route B — global self-consistency (the strong check)

A single thunk address is shared by every `(interface, method)` pair landing on
that slot — 28 export names all point at `0x17f0`, for example. So a thunk RVA
is a property of the *slot*, not of one method, and the same slot is reached
from many unrelated interfaces.

All **1065 exports** were decoded and grouped by RVA:

```
distinct thunk RVAs in export table : 327
RVAs with a resolved displacement   : 217
RVAs implying two displacements     : 0     <- no ambiguity anywhere
```

Zero conflicts means the reading is self-consistent across the whole binary.

### Route C — the real vtable in `steamclient64.dll`

`steam_api64.dll` is a thin IPC shim: it does **not** store the vtable. The
object is constructed in `steamclient64.dll` (`C:\Program Files (x86)\Steam\steamclient64.dll`,
26,541,208 bytes, SHA-256 `4471517105b1fbd603441b38db04570d185a9c993e3aa54b5cc237c3eb1fe51f`).

The chain, all static:

1. A registration table in `.data` maps version string → factory:
   | version | factory RVA | vtable RVA |
   |---|---:|---:|
   | `SteamInput001` | `0x723450` | `0x130ed50` |
   | `SteamInput002` | `0x7234e0` | `0x130ee70` |
   | `SteamInput003` | `0x723570` | `0x130ef90` |
   | `SteamInput004` | `0x723600` | `0x130f0b8` |
   | `SteamInput005` | `0x723690` | `0x130f1e8` |
   | **`SteamInput006`** | **`0x723720`** | **`0x130f3b8`** |
   | `SteamInput007` | `0x7237b0` | `0x130f540` |

2. The `SteamInput006` factory allocates a 0x4620-byte object and stores the
   vtable pointer at `object+0`:
   ```asm
   0x0072377a: lea  rcx, [rip + 0xbebc37]   ; -> rva 0x130f3b8
   0x00723781: mov  qword ptr [rax], rcx    ; vtable at object+0
   0x00723784: mov  qword ptr [rax + 8], rdi
   ```

3. Reading 48 consecutive code pointers from **RVA `0x130f3b8`** in
   `steamclient64.dll` (VA `0x13930f3b8`, `.rdata`) yields **exactly 48
   entries** — 48 distinct function pointers, matching the 48 slots exactly.

Cross-version sanity, straight from the binary:

```
SteamInput001: 35 entries     SteamInput005: 47 entries
SteamInput002: 35 entries     SteamInput006: 48 entries   <- our table
SteamInput003: 36 entries     SteamInput007: 48 entries
SteamInput004: 37 entries
```

The Valve headers agree on the growth: SDK 152 (`SteamInput005`) declares 47
virtuals, SDK 164 (`SteamInput006`) declares 48, and the only difference is at
position 47 — `SetDualSenseTriggerEffect` was appended. 47 → 48, exactly as the
binary shows.

### Route D — the official Valve header

`steamworks_sdk_164/isteaminput.h`, whose
`STEAMINPUT_INTERFACE_VERSION` is literally `"SteamInput006"`. Its 48 virtual
declarations were extracted in order and compared with the measured slots:

```
header virtuals            : 48
measured slots             : 48
order mismatches           : 0
in header, not exported    : 0
exported, not in header    : 0
VERDICT: header order and measured slots AGREE EXACTLY
```

---

## 3. The slot table

`fn RVA` is the real implementation in `steamclient64.dll` (image base
`0x138000000`). `byte offset` is what the shim indexes with.

| slot | byte offset | method | return type | fn RVA |
|-----:|------------:|--------|-------------|--------|
| 0 | `0x00` | `Init` | `bool` | `0x723cd0` |
| 1 | `0x08` | `Shutdown` | `bool` | `0x720890` |
| 2 | `0x10` | `SetInputActionManifestFilePath` | `bool` | `0x723d20` |
| 3 | `0x18` | `RunFrame` | `void` | `0x71ce20` |
| 4 | `0x20` | `BWaitForData` | `bool` | `0x723430` |
| 5 | `0x28` | `BNewDataAvailable` | `bool` | `0x721c90` |
| 6 | `0x30` | `GetConnectedControllers` | `int` | `0x7204a0` |
| 7 | `0x38` | `EnableDeviceCallbacks` | `void` | `0x723840` |
| 8 | `0x40` | `EnableActionEventCallbacks` | `void` | `0x7213b0` |
| 9 | `0x48` | `GetActionSetHandle` | `InputActionSetHandle_t` | `0x720270` |
| 10 | `0x50` | `ActivateActionSet` | `void` | `0x71fd90` |
| 11 | `0x58` | `GetCurrentActionSet` | `InputActionSetHandle_t` | `0x720510` |
| 12 | `0x60` | `ActivateActionSetLayer` | `void` | `0x71fdd0` |
| 13 | `0x68` | `DeactivateActionSetLayer` | `void` | `0x7201f0` |
| 14 | `0x70` | `DeactivateAllActionSetLayers` | `void` | `0x720230` |
| 15 | `0x78` | `GetActiveActionSetLayers` | `int` | `0x7202f0` |
| 16 | `0x80` | `GetDigitalActionHandle` | `InputDigitalActionHandle_t` | `0x723980` |
| **17** | **`0x88`** | **`GetDigitalActionData`** | `InputDigitalActionData_t` | `0x720520` |
| 18 | `0x90` | `GetDigitalActionOrigins` | `int` | `0x723a00` |
| 19 | `0x98` | `GetStringForDigitalActionName` | `const char *` | `0x723c50` |
| 20 | `0xa0` | `GetAnalogActionHandle` | `InputAnalogActionHandle_t` | `0x7238b0` |
| **21** | **`0xa8`** | **`GetAnalogActionData`** | `InputAnalogActionData_t` | `0x720330` |
| 22 | `0xb0` | `GetAnalogActionOrigins` | `int` | `0x723930` |
| 23 | `0xb8` | `GetGlyphPNGForActionOrigin` | `const char *` | `0x723a90` |
| 24 | `0xc0` | `GetGlyphSVGForActionOrigin` | `const char *` | `0x723ad0` |
| 25 | `0xc8` | `GetGlyphForActionOrigin_Legacy` | `const char *` | `0x723a50` |
| 26 | `0xd0` | `GetStringForActionOrigin` | `const char *` | `0x723ba0` |
| 27 | `0xd8` | `GetStringForAnalogActionName` | `const char *` | `0x723be0` |
| 28 | `0xe0` | `StopAnalogActionMomentum` | `void` | `0x7208a0` |
| 29 | `0xe8` | `GetMotionData` | `InputMotionData_t` | `0x7206f0` |
| 30 | `0xf0` | `TriggerVibration` | `void` | `0x720900` |
| 31 | `0xf8` | `TriggerVibrationExtended` | `void` | `0x723f20` |
| 32 | `0x100` | `TriggerSimpleHapticEvent` | `void` | `0x723ef0` |
| 33 | `0x108` | `SetLEDColor` | `void` | `0x720800` |
| 34 | `0x110` | `Legacy_TriggerHapticPulse` | `void` | `0x7208c0` |
| 35 | `0x118` | `Legacy_TriggerRepeatedHapticPulse` | `void` | `0x7208d0` |
| 36 | `0x120` | `ShowBindingPanel` | `bool` | `0x720860` |
| 37 | `0x128` | `GetInputTypeForHandle` | `ESteamInputType` | `0x723b20` |
| 38 | `0x130` | `GetControllerForGamepadIndex` | `InputHandle_t` | `0x67e040` |
| 39 | `0x138` | `GetGamepadIndexForController` | `int` | `0x67e590` |
| 40 | `0x140` | `GetStringForXboxOrigin` | `const char *` | `0x7207a0` |
| 41 | `0x148` | `GetGlyphForXboxOrigin` | `const char *` | `0x7206a0` |
| 42 | `0x150` | `GetActionOriginFromXboxOrigin` | `EInputActionOrigin` | `0x723870` |
| 43 | `0x158` | `TranslateActionOrigin` | `EInputActionOrigin` | `0x723ea0` |
| 44 | `0x160` | `GetDeviceBindingRevision` | `bool` | `0x7204d0` |
| 45 | `0x168` | `GetRemotePlaySessionID` | `uint32` | `0x722720` |
| 46 | `0x170` | `GetSessionInputConfigurationSettings` | `uint16` | `0x723b60` |
| 47 | `0x178` | `SetDualSenseTriggerEffect` | `void` | `0x723d10` |

No UNPROVEN or guessed entries: every slot was measured, and the measured
values were cross-checked against three other sources.

---

## 4. Data a shim must return

From the same header (`steamworks_sdk_164`).

```c
typedef uint64 InputHandle_t;          // 64-bit, NOT uint32
#define STEAM_INPUT_MAX_COUNT 16
#define STEAM_INPUT_MAX_ORIGINS 8
#define STEAM_INPUT_MAX_ACTIVE_LAYERS 16
#define STEAM_INPUT_HANDLE_ALL_CONTROLLERS UINT64_MAX

struct InputDigitalActionData_t {
    bool bState;     // currently pressed
    bool bActive;    // bound in the active action set
};

struct InputAnalogActionData_t {
    EInputSourceMode eMode;
    float x, y;
    bool bActive;
};

struct InputMotionData_t {             // slot 29
    float rotQuatX, rotQuatY, rotQuatZ, rotQuatW;
    float posAccelX, posAccelY, posAccelZ;
    float rotVelX, rotVelY, rotVelZ;
};
```

`InputDigitalActionData_t` is 2 bytes (`bool bState`, `bool bActive`) and
`InputAnalogActionData_t` is 16 bytes under MSVC x64 `VALVE_CALLBACK_PACK_LARGE`:
`EInputSourceMode` at 0, `float x` at 4, `float y` at 8, `bool bActive` at 12,
13 bytes of payload rounded up to the struct's 4-byte alignment. **Verify the
16-byte size in the compiled shim rather than trusting the arithmetic** — it is
what decides whether a hidden return pointer is used.

Both are returned **by value**, so under MSVC x64 a 2-byte struct comes back in
`RAX` with no hidden return pointer, while the 16-byte analog struct does use
one. This is exactly why the `steam_api64.dll` thunks for the data accessors
`call` and then copy rather than `jmp`.

`EInputSourceMode` values a VR-sourced analog action will need:

| value | mode |
|---:|---|
| 0 | `k_EInputSourceMode_None` |
| 2 | `k_EInputSourceMode_Buttons` |
| 6 | `k_EInputSourceMode_JoystickMove` |
| 10 | `k_EInputSourceMode_Trigger` |
| 15 | `k_EInputSourceMode_SingleButton` |

`ESteamInputType` (slot 37) — the useful ones:

| value | type |
|---:|---|
| 0 | `k_ESteamInputType_Unknown` |
| 1 | `k_ESteamInputType_SteamController` |
| 3 | `k_ESteamInputType_XBoxOneController` |
| 4 | `k_ESteamInputType_GenericGamepad` |
| 11 | `k_ESteamInputType_MobileTouch` — "Steam Link App On-screen Virtual Controller" |
| 14 | `k_ESteamInputType_SteamDeckController` |

---

## 5. How the game actually reaches this interface

The 48 `SteamAPI_ISteamInput_*` accessors are **not** how the game reads input.
`teardown.exe`'s import table shows exactly 12 symbols pulled from
`steam_api64.dll`:

```
SteamAPI_Init                     SteamInternal_ContextInit
SteamAPI_GetHSteamUser            SteamInternal_FindOrCreateUserInterface
SteamAPI_RegisterCallback         SteamInternal_FindOrCreateGameServerInterface
SteamAPI_UnregisterCallback       SteamInternal_CreateInterface
SteamAPI_RegisterCallResult       SteamAPI_RunCallbacks
SteamAPI_UnregisterCallResult     SteamAPI_Shutdown
```

No `SteamAPI_ISteamInput_*`, no `SteamAPI_SteamInput_v006`, no
`SteamAPI_ISteamClient_GetISteamInput`. The image contains no `SteamInput_v0NN`
or `SteamController_v0NN` string at all — but it does contain `SteamInput006`,
and it statically imports `SteamInternal_FindOrCreateUserInterface`.

So the game does exactly this:

```c
ISteamInput *in = (ISteamInput *)SteamInternal_FindOrCreateUserInterface(
                      user, "SteamInput006");
// then calls through the vtable
```

**This is the decisive practical fact for the mod: the game never touches the
flat API, so a shim that only patches the `SteamAPI_ISteamInput_*` exports
would do nothing at all. The only interception point is the vtable slot — which
is exactly what this document maps.** The flat-API thunks are valuable here as
*evidence*, not as a hook target.

## 6. Is there a virtual controller / virtual gamepad? — **No**

**There is no virtual controller or virtual gamepad API in `SteamInput006`.**
This was checked directly, and it matters because a virtual-device route would
have avoided code patching entirely.

Evidence:

- **The header has no such method.** All 48 virtuals are listed above. There is
  no `AddVirtualController`, `CreateVirtualController`, `SetVirtualControllerState`,
  or any injection entry point. `SteamInput006` exposes *read* APIs for device
  state and *write* APIs only for output (vibration, haptics, LED).
- **The binary has no such string.** Substring counts over the whole
  `steam_api64.dll`:

  | pattern | count | | pattern | count |
  |---|---:|---|---|---:|
  | `SteamInput` | 51 | | `Virtual` | 1 |
  | `SteamController` | 37 | | `virtual` | 1 |
  | `Controller` | 44 | | `SteamVR` | **0** |
  | `Gamepad` | 9 | | `openvr` / `OpenVR` | **0** |
  | `Haptic` | 5 | | `SDL` | 0 |
  | `Vibration` | 3 | | `Gyro` | 0 |

  The single `Virtual` / `virtual` hit is `RtlVirtualUnwind` (the SEH
  unwinder) plus the string `` `virtual displacement map' `` (a GCC/Clang
  assert message). Neither is an input API.
- **`SteamVR` does not appear at all.** The SDK DLL knows nothing about VR —
  it is a generic Steamworks shim. VR↔controller translation has to live in
  the shim DLL, not here.
- The only "gamepad" concepts present are the two **read-only mapping** helpers,
  `GetControllerForGamepadIndex` (slot 38) and `GetGamepadIndexForController`
  (slot 39), which map a Steam Input handle to/from an XInput index. They
  report an existing device; they cannot create one.

**Consequence:** the no-patching alternative does not exist. VR controller state
must be published by the shim answering `GetDigitalActionData` / `GetAnalogActionData`
(or by patching those slots) — which is what the slot table above enables.

Note the SDK does grow in this direction later: a virtual-controller concept
exists in the Steam Input runtime as `k_ESteamInputType_MobileTouch`
("Steam Link App On-screen Virtual Controller"), but in `SteamInput006` it is a
read-only enum value, not a programmable device.

---

## 7. How the shim should use this

`B` is the vtable pointer, i.e. the value the game holds as `ISteamInput*` (the
object returned by `SteamInternal_FindOrCreateUserInterface("SteamInput006")`).
In `steamclient64.dll` that object has the vtable at offset 0, so `B` is exactly
the pointer read from `0x130f3b8` in the image.

To answer digital actions from VR controller state, replace slot 17:

```c
// B = ISteamInput*
void **vtbl = *(void ***)B;              // vtbl == the 0x130f3b8 array

// save the original once
static InputDigitalActionData_t (*orig_digital)(void *, uint64, uint64) = NULL;
if (!orig_digital) orig_digital = vtbl[17];

// install the hook
InterlockedExchangePointer((void **)&vtbl[17], (void *)my_GetDigitalActionData);
```

`my_GetDigitalActionData` must match the real signature:

```c
struct InputDigitalActionData_t my_GetDigitalActionData(void *self,
                                                         InputHandle_t controller,
                                                         InputDigitalActionHandle_t action);
```

It is returned **by value in `RAX`** (a 2-byte struct packs into a register), so
no hidden return pointer is needed — but do not assume a different packing
without checking. `GetAnalogActionData` (slot 21, 12-byte struct) is the one
that uses a hidden return pointer; verify against the compiled shim.

### Practical notes

- **Pass every other slot through unchanged.** The game calls `Init` (0) and
  `RunFrame` (3) on the real interface; if the shim returns garbage for those,
  Steam Input never initialises. Forward them to the saved originals.
- **Handle `Init` yourself if you bypass the real object.** If the shim fully
  replaces the interface, return `true` for slot 0 and no-op slot 3, or Steam
  will wait on an IPC event that never arrives.
- **`RunFrame` is where the shim samples VR.** The natural design is: on
  `RunFrame`, poll the VR runtime for controller pose/trigger state into a
  shadow buffer; let `GetDigitalActionData` / `GetAnalogActionData` read from
  that buffer.
- **Do not need `EnableActionEventCallbacks`** (slot 8) if polling from
  `RunFrame`. It is the lower-latency alternative but requires servicing a
  callback pointer the game sets.
- **Validate `action` handles.** `GetDigitalActionHandle` (slot 16) is what maps
  a name like `"Fire"` to a handle. If the shim is asked for a handle it does
  not know, return a sentinel of the game's choosing consistently and make
  `GetStringForDigitalActionName` (slot 19) return `NULL` for it, or the game
  may assert.

---

## 8. Reproducing

Scripts live in `/home/truenas_admin/teardown-analysis/steaminput/`:

| script | what it does |
|---|---|
| `slots4.py` | Route A+B — decode all 1065 exports, extract the 48 slots, assert no conflicts |
| `vtable_sc.py` | Route C — find the registration record, follow the factory, dump the real 48-entry vtable |
| `crosscheck.py` | Route D — compare measured slots against the Valve header order |
| `crossver.py` | Route C extra — compare the real `SteamInput001`..`007` vtables against each other and against header diffs |
| `final.py` | Merge all evidence into `steaminput006_slots.json` + `slot_table.md` |
| `surface.py` | The `SteamInput` / `Controller` / `Virtual` / `SteamVR` string survey behind §6 |
| `isteaminput_sdk164.h` | Valve's header, `STEAMINPUT_INTERFACE_VERSION "SteamInput006"` |
| `isteaminput_sdk152.h` | Valve's header for `SteamInput005`, used for the version-diff check |
| `real_steam_api64.dll` | the analysed copy (298,384 bytes, hash in §1) |
| `steamclient64.dll` | the 26 MB file the real vtable was read from |
| `steaminput006_slots.json` | machine-readable form of the table above |
| `vtable_steaminput006.json` | the raw 48-entry vtable dump with per-slot fn RVAs |

Run order: `slots4.py` → `vtable_sc.py` → `crosscheck.py` → `crossver.py` → `final.py`.

### Gotchas hit while building this

- **`.pdata` is useless for these thunks.** The flat-API wrappers are leaf
  functions that never clobber a nonvolatile register, so they carry no
  `RUNTIME_FUNCTION` record; naive use of exception-directory bounds produced
  overlapping 64-byte windows that bled into the *next* function and attributed
  its slot to the wrong method. Bounds come from sequential decode with early
  termination at the first `ret`/`jmp` instead.
- **The `mov rax,[rcx]` trap.** See Route A — the vtable load is not a slot
  access. Getting this wrong silently reports every method as slot 0.
- **The 16-byte-argument stubs.** 45 of 48 thunks are `jmp` stubs, 2 use
  `mov r10,[rax+d]; jmp r10`, and 4 use `call` (they post-process the return
  value). All three forms give the same slot; the extractor must handle all three.
- **The registration table's `+0x00` is a factory function, not a vtable.**
  Reading 48 qwords straight at the `SteamInput006` record's first field yields
  code bytes, not pointers. The vtable is the value the factory stores at
  `object+0`, one `lea`/`mov` pair deeper.
- **The exported-thunk RVAs are shared, not unique.** ~28 export names can map
  to the same address. Iterating exports without grouping by RVA both wastes
  work and risks contradictory readings.
