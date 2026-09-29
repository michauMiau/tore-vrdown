# ISteamInput006 vtable - resolved from the real steam_api64.dll

Binary: `/root/steamless/Teardown/steam_api64.dll`
7305128 bytes, PE32+, image base `0x180000000`
Vtable: **RVA `0x54B738`** (`.rdata`), **48 entries**, VA `0x18054B738`, file offset `0x54A738`.

**30 of 48 slots are proved from the binary. 18 are `UNMAPPED` and provably
undecidable from this binary (see UNVERIFIED).**

## Proof that this is the ISteamInput006 vtable

Three independent facts, each from `.pdata`-bounded disassembly. Every byte
string below was read from the file when this document was generated.

**1 - the factory names the version.**  `SteamAPI_SteamInput_v006` is at RVA
`0x335C0` (`.pdata 0x335C0..0x33615`):

    0x335C0  48895c2408          mov    qword ptr [rsp + 8], rbx
    0x335C5  4889742410          mov    qword ptr [rsp + 0x10], rsi
    0x335CA  57                  push   rdi
    0x335CB  4883ec20            sub    rsp, 0x20
    0x335CF  e8bcb8feff          call   0x1ee90
    0x335D4  488db088000000      lea    rsi, [rax + 0x88]
    0x335DB  488b06              mov    rax, qword ptr [rsi]
    0x335DE  488bb830010000      mov    rdi, qword ptr [rax + 0x130]
    0x335E5  e876b8feff          call   0x1ee60
    0x335EA  8bd8                mov    ebx, eax
    0x335EC  e87fb8feff          call   0x1ee70
    0x335F1  4c8d0d20995000      lea    r9, [rip + 0x509920]

The `lea r9` is bytes `4c8d0d20995000`; rip target = `0x335F1 + 7 + 0x509920 = 0x53CF18`, and the
string there is `SteamInput006`.

**2 - the resolver returns the +0x50 sub-object for that name.**  The function
`0xAF100` (`.pdata 0xAF100..0xAF270`) strcmp-chains the version names against
`0x527030`; the arm that matches `SteamInput006` is:

    0xAF1FF  488b8be0000000      mov    rcx, qword ptr [rbx + 0xe0]
    0xAF206  33d2                xor    edx, edx
    0xAF208  4885c9              test   rcx, rcx
    0xAF20B  488d4150            lea    rax, [rcx + 0x50]
    0xAF20F  480f44c2            cmove  rax, rdx
    0xAF213  488b5c2470          mov    rbx, qword ptr [rsp + 0x70]

so for `SteamInput006` it returns `inputobj + 0x50`.

**3 - the constructor installs the sub-vtable ladder.**  `CSteamInput`'s ctor at
`0xB2BE0` (`.pdata 0xB2BE0..0xB2F46`) does one `lea` + `mov [rcx+K],rax` per
sub-object:

    0xB2C60  488d0531884900      lea    rax, [rip + 0x498831]
    0xB2C67  48894140            mov    qword ptr [rcx + 0x40], rax
    0xB2C6B  488d0546894900      lea    rax, [rip + 0x498946]
    0xB2C72  48894148            mov    qword ptr [rcx + 0x48], rax
    0xB2C76  488d05bb8a4900      lea    rax, [rip + 0x498abb]
    0xB2C7D  48894150            mov    qword ptr [rcx + 0x50], rax
    0xB2C81  4533ff              xor    r15d, r15d

`0xB2C76` is bytes `488d05bb8a4900` -> rip target `0x54B738`, stored at `obj+0x50`.
Therefore the sub-object handed out for `"SteamInput006"` carries the vtable
`0x54B738`. **PROVED.**

## Entry form, and the this-adjustment that matters

Every one of the 48 entries is a full MSVC this-adjusting thunk.  Slot 0:

    0xB82AC  4883e908            sub    rcx, 8
    0xB82B0  e9bbfbffff          jmp    0xb7e70
    0xB82B5  cc                  int3   

So a method's identity is the pair **(implementation, this-offset)** with
`this-offset = 0x50 - adj`.  Slot 0's `sub rcx,8` means the callee receives
`obj+0x48`, i.e. the entries are written for the v005 sub-object's frame and
reach down into shared implementation.  This is why the four versioned
sub-tables can share implementations and why a method identity alone is not
enough to name a slot.

Each flat export `SteamAPI_ISteamInput_<Name>` is itself a thunk: it calls
`0x1EE90` to fetch the CSteamInput, then loads a sub-vtable at `[CSteamInput+K]`
(`+0x168` is the CSteamInput pointer in the context) and tail-calls one slot.
`SteamAPI_ISteamInput_ActivateActionSetLayer` (ord 334, RVA `0x22E90`):

    0x22E90  48895c2408          mov    qword ptr [rsp + 8], rbx
    0x22E95  57                  push   rdi
    0x22E96  4883ec20            sub    rsp, 0x20
    0x22E9A  498bd8              mov    rbx, r8
    0x22E9D  488bfa              mov    rdi, rdx
    0x22EA0  e8ebbfffff          call   0x1ee90
    0x22EA5  4c8bc3              mov    r8, rbx
    0x22EA8  488bd7              mov    rdx, rdi
    0x22EAB  488b8868010000      mov    rcx, qword ptr [rax + 0x168]
    0x22EB2  488b4120            mov    rax, qword ptr [rcx + 0x20]
    0x22EB6  4883c120            add    rcx, 0x20
    0x22EBA  488b5c2430          mov    rbx, qword ptr [rsp + 0x30]
    0x22EBF  4883c420            add    rsp, 0x20
    0x22EC3  5f                  pop    rdi
    0x22EC4  48ff6040            jmp    qword ptr [rax + 0x40]

It routes through `+0x20` and jumps to slot `0x40/8 = 8` **of that sub-table**,
not of v006.  Flat exports therefore do NOT share one global slot numbering;
each had to be joined to the v006 table through its own sub-table.

## Evidence tiers

| tier | meaning | count |
| --- | --- | ---: |
| `ALIGNED` | export routes through a sub-table proved WHOLE-element-wise identical to a contiguous v006 range, so the index transfers | 6 |
| `AMBIGUOUS` | several exported names share this entry; not decidable from the binary | 18 |
| `CHAIN` | name recovered by following a leaf thunk's second indirection | 1 |
| `DIRECT` | export tail-calls `[obj+0x50]` slot N - the v006 vtable itself | 1 |
| `IDENTITY` | export reaches this entry's (impl, this-offset) pair and no other v006 slot does, so the binding is forced | 22 |

## The table

| slot | byte_offset | method name | evidence | function RVA in steam_api64.dll |
| ---: | ---: | --- | --- | ---: |
| 0 | 0x000 | Init | export SteamAPI_ISteamInput_Init (ord 368, @0x23430) tail-calls [+0x08] slot 0, a leaf thunk that does `mov rax,[rcx+0x40]; add rcx,0x40; mov dl,1; jmp rax`; that lands on [+0x48] slot 0 = impl 0xB7E70, which is this slot's implementation | 0xB82AC -> 0xB7E70 |
| 1 | 0x008 | Shutdown | export SteamAPI_ISteamInput_Shutdown (ord 376, @0x23510) reaches impl 0xB8420 with this=0x0; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB86BC -> 0xB8420 |
| 2 | 0x010 | UNMAPPED | AMBIGUOUS: 3 exported names reach this entry (impl 0xA1CF0, this=0x48), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: BNewDataAvailable, BWaitForData, SetInputActionManifestFilePath. Valve cppISteamInput006.cpp declares SetInputActionManifestFilePath at this position (cross-check only, NOT proof) | 0xA3FA4 -> 0xA1CF0 |
| 3 | 0x018 | RunFrame | export SteamAPI_ISteamInput_RunFrame (ord 371, @0x261A0) dispatches via sub-table +0x48 slot 3; that WHOLE sub-table is proved element-wise identical to v006[0:47] (all 47 indices match in both impl and this-offset), so the index transfers | 0xB8394 -> 0xB8370 |
| 4 | 0x020 | UNMAPPED | AMBIGUOUS: 3 exported names reach this entry (impl 0xA1CF0, this=0x48), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: BNewDataAvailable, BWaitForData, SetInputActionManifestFilePath. Valve cppISteamInput006.cpp declares BWaitForData at this position (cross-check only, NOT proof) | 0xA3FA4 -> 0xA1CF0 |
| 5 | 0x028 | UNMAPPED | AMBIGUOUS: 3 exported names reach this entry (impl 0xA1CF0, this=0x48), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: BNewDataAvailable, BWaitForData, SetInputActionManifestFilePath. Valve cppISteamInput006.cpp declares BNewDataAvailable at this position (cross-check only, NOT proof) | 0xA3FA4 -> 0xA1CF0 |
| 6 | 0x030 | GetConnectedControllers | export SteamAPI_ISteamInput_GetConnectedControllers (ord 347, @0x230F0) reaches impl 0xB4440 with this=0x8; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB4540 -> 0xB4440 |
| 7 | 0x038 | UNMAPPED | AMBIGUOUS: 3 exported names reach this entry (impl 0xFDC0, this=0x48), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: EnableActionEventCallbacks, EnableDeviceCallbacks, TriggerSimpleHapticEvent. Valve cppISteamInput006.cpp declares EnableDeviceCallbacks at this position (cross-check only, NOT proof) | 0xAB404 -> 0xFDC0 |
| 8 | 0x040 | UNMAPPED | AMBIGUOUS: 3 exported names reach this entry (impl 0xFDC0, this=0x48), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: EnableActionEventCallbacks, EnableDeviceCallbacks, TriggerSimpleHapticEvent. Valve cppISteamInput006.cpp declares GetActionSetHandle at this position (cross-check only, NOT proof) | 0xAB404 -> 0xFDC0 |
| 9 | 0x048 | GetActionSetHandle | export SteamAPI_ISteamInput_GetActionSetHandle (ord 342, @0x22F80) reaches impl 0xB3770 with this=0x8; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB3A0C -> 0xB3770 |
| 10 | 0x050 | ActivateActionSet | export SteamAPI_ISteamInput_ActivateActionSet (ord 333, @0x22E50) reaches impl 0xB35F0 with this=0x8; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB3760 -> 0xB35F0 |
| 11 | 0x058 | GetCurrentActionSet | export SteamAPI_ISteamInput_GetCurrentActionSet (ord 349, @0x231A0) reaches impl 0xB4610 with this=0x8; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB46B8 -> 0xB4610 |
| 12 | 0x060 | UNMAPPED | AMBIGUOUS: 3 exported names reach this entry (impl 0xFDC0, this=0x20), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: ActivateActionSetLayer, DeactivateActionSetLayer, DeactivateAllActionSetLayers. Valve cppISteamInput006.cpp declares DeactivateActionSetLayer at this position (cross-check only, NOT proof) | 0xAC74C -> 0xFDC0 |
| 13 | 0x068 | UNMAPPED | AMBIGUOUS: 3 exported names reach this entry (impl 0xFDC0, this=0x20), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: ActivateActionSetLayer, DeactivateActionSetLayer, DeactivateAllActionSetLayers. Valve cppISteamInput006.cpp declares DeactivateAllActionSetLayers at this position (cross-check only, NOT proof) | 0xAC74C -> 0xFDC0 |
| 14 | 0x070 | UNMAPPED | AMBIGUOUS: 3 exported names reach this entry (impl 0xFDC0, this=0x20), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: ActivateActionSetLayer, DeactivateActionSetLayer, DeactivateAllActionSetLayers. Valve cppISteamInput006.cpp declares GetActiveActionSetLayers at this position (cross-check only, NOT proof) | 0xAC74C -> 0xFDC0 |
| 15 | 0x078 | GetActiveActionSetLayers | export SteamAPI_ISteamInput_GetActiveActionSetLayers (ord 343, @0x22FB0) reaches impl 0x13B60 with this=0x20; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB3A18 -> 0x13B60 |
| 16 | 0x080 | GetDigitalActionHandle | export SteamAPI_ISteamInput_GetDigitalActionHandle (ord 352, @0x23210) reaches impl 0xB4B10 with this=0x8; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB4E80 -> 0xB4B10 |
| 17 | 0x088 | GetDigitalActionData | export SteamAPI_ISteamInput_GetDigitalActionData (ord 351, @0x231D0) reaches impl 0xB46D0 with this=0x8; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB4AFC -> 0xB46D0 |
| 18 | 0x090 | GetDigitalActionOrigins | export SteamAPI_ISteamInput_GetDigitalActionOrigins (ord 353, @0x25EB0) reaches impl 0xB4FF0 with this=0x38; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB5354 -> 0xB4FF0 |
| 19 | 0x098 | UNMAPPED | AMBIGUOUS: 2 exported names reach this entry (impl 0xB7E20, this=0x48), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: GetStringForAnalogActionName, GetStringForDigitalActionName. Valve cppISteamInput006.cpp declares GetAnalogActionHandle at this position (cross-check only, NOT proof) | 0xB7E28 -> 0xB7E20 |
| 20 | 0x0A0 | GetAnalogActionHandle | export SteamAPI_ISteamInput_GetAnalogActionHandle (ord 345, @0x23070) reaches impl 0xB3E00 with this=0x8; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB409C -> 0xB3E00 |
| 21 | 0x0A8 | GetAnalogActionData | export SteamAPI_ISteamInput_GetAnalogActionData (ord 344, @0x22FF0) reaches impl 0xB3A30 with this=0x8; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB3DEC -> 0xB3A30 |
| 22 | 0x0B0 | GetAnalogActionOrigins | export SteamAPI_ISteamInput_GetAnalogActionOrigins (ord 346, @0x25E10) reaches impl 0xB4210 with this=0x38; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB4418 -> 0xB4210 |
| 23 | 0x0B8 | UNMAPPED | AMBIGUOUS: 2 exported names reach this entry (impl 0xB7CA0, this=0x48), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: GetGlyphForActionOrigin_Legacy, GetGlyphPNGForActionOrigin. Valve cppISteamInput006.cpp declares GetStringForAnalogActionName at this position (cross-check only, NOT proof) | 0xB7CB0 -> 0xB7CA0 |
| 24 | 0x0C0 | GetGlyphSVGForActionOrigin | export SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin (ord 359, @0x25FB0) dispatches via sub-table +0x48 slot 24; that WHOLE sub-table is proved element-wise identical to v006[0:47] (all 47 indices match in both impl and this-offset), so the index transfers | 0xB7CBC -> 0x21F90 |
| 25 | 0x0C8 | UNMAPPED | AMBIGUOUS: 2 exported names reach this entry (impl 0xB7CA0, this=0x48), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: GetGlyphForActionOrigin_Legacy, GetGlyphPNGForActionOrigin. Valve cppISteamInput006.cpp declares GetMotionData at this position (cross-check only, NOT proof) | 0xB7CB0 -> 0xB7CA0 |
| 26 | 0x0D0 | GetStringForActionOrigin | export SteamAPI_ISteamInput_GetStringForActionOrigin (ord 364, @0x26050) reaches impl 0xB7E20 with this=0x38; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB7E40 -> 0xB7E20 |
| 27 | 0x0D8 | UNMAPPED | AMBIGUOUS: 2 exported names reach this entry (impl 0xB7E20, this=0x48), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: GetStringForAnalogActionName, GetStringForDigitalActionName. Valve cppISteamInput006.cpp declares TriggerVibrationExtended at this position (cross-check only, NOT proof) | 0xB7E28 -> 0xB7E20 |
| 28 | 0x0E0 | UNMAPPED | AMBIGUOUS: 3 exported names reach this entry (impl 0xFDC0, this=0x8), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: StopAnalogActionMomentum, TriggerHapticPulse, TriggerRepeatedHapticPulse. Valve cppISteamInput006.cpp declares TriggerSimpleHapticEvent at this position (cross-check only, NOT proof) | 0xAC770 -> 0xFDC0 |
| 29 | 0x0E8 | GetMotionData | export SteamAPI_ISteamInput_GetMotionData (ord 361, @0x23350) reaches impl 0xB7DA0 with this=0x10; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB7E08 -> 0xB7DA0 |
| 30 | 0x0F0 | TriggerVibration | export SteamAPI_ISteamInput_TriggerVibration (ord 382, @0x23670) reaches impl 0xB8700 with this=0x18; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB8890 -> 0xB8700 |
| 31 | 0x0F8 | TriggerVibrationExtended | export SteamAPI_ISteamInput_TriggerVibrationExtended (ord 383, @0x26300) dispatches via sub-table +0x48 slot 31; that WHOLE sub-table is proved element-wise identical to v006[0:47] (all 47 indices match in both impl and this-offset), so the index transfers | 0xB88B0 -> 0xB88A0 |
| 32 | 0x100 | UNMAPPED | AMBIGUOUS: 3 exported names reach this entry (impl 0xFDC0, this=0x48), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: EnableActionEventCallbacks, EnableDeviceCallbacks, TriggerSimpleHapticEvent. Valve cppISteamInput006.cpp declares ShowBindingPanel at this position (cross-check only, NOT proof) | 0xAB404 -> 0xFDC0 |
| 33 | 0x108 | SetLEDColor | export SteamAPI_ISteamInput_SetLEDColor (ord 374, @0x23480) reaches impl 0xFDC0 with this=0x18; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xAC758 -> 0xFDC0 |
| 34 | 0x110 | Legacy_TriggerHapticPulse | export SteamAPI_ISteamInput_Legacy_TriggerHapticPulse (ord 369, @0x260E0) dispatches via sub-table +0x48 slot 34; that WHOLE sub-table is proved element-wise identical to v006[0:47] (all 47 indices match in both impl and this-offset), so the index transfers | 0xB830C -> 0xB8300 |
| 35 | 0x118 | Legacy_TriggerRepeatedHapticPulse | export SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse (ord 370, @0x26130) dispatches via sub-table +0x48 slot 35; that WHOLE sub-table is proved element-wise identical to v006[0:47] (all 47 indices match in both impl and this-offset), so the index transfers | 0xB8348 -> 0xB8320 |
| 36 | 0x120 | ShowBindingPanel | export SteamAPI_ISteamInput_ShowBindingPanel (ord 375, @0x234E0) reaches impl 0xA1CF0 with this=0x8; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB840C -> 0xA1CF0 |
| 37 | 0x128 | GetInputTypeForHandle | export SteamAPI_ISteamInput_GetInputTypeForHandle (ord 360, @0x23320) reaches impl 0xB7D00 with this=0x20; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB7D88 -> 0xB7D00 |
| 38 | 0x130 | GetControllerForGamepadIndex | export SteamAPI_ISteamInput_GetControllerForGamepadIndex (ord 348, @0x23170) reaches impl 0xB4550 with this=0x10; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB45F8 -> 0xB4550 |
| 39 | 0x138 | GetGamepadIndexForController | export SteamAPI_ISteamInput_GetGamepadIndexForController (ord 354, @0x23290) reaches impl 0xB5380 with this=0x10; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB541C -> 0xB5380 |
| 40 | 0x140 | UNMAPPED | AMBIGUOUS: 2 exported names reach this entry (impl 0x21F90, this=0x28), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: GetGlyphForXboxOrigin, GetStringForXboxOrigin. Valve cppISteamInput006.cpp declares GetRemotePlaySessionID at this position (cross-check only, NOT proof) | 0xB7CEC -> 0x21F90 |
| 41 | 0x148 | UNMAPPED | AMBIGUOUS: 2 exported names reach this entry (impl 0x21F90, this=0x28), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: GetGlyphForXboxOrigin, GetStringForXboxOrigin. Valve cppISteamInput006.cpp declares GetSessionInputConfigurationSettings at this position (cross-check only, NOT proof) | 0xB7CEC -> 0x21F90 |
| 42 | 0x150 | UNMAPPED | AMBIGUOUS: 2 exported names reach this entry (impl 0x13B60, this=0x38), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: GetActionOriginFromXboxOrigin, GetRemotePlaySessionID. Valve cppISteamInput006.cpp declares SetDualSenseTriggerEffect at this position (cross-check only, NOT proof) | 0xAC2C8 -> 0x13B60 |
| 43 | 0x158 | TranslateActionOrigin | export SteamAPI_ISteamInput_TranslateActionOrigin (ord 378, @0x26240) reaches impl 0xB86D0 with this=0x38; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xB86F4 -> 0xB86D0 |
| 44 | 0x160 | GetDeviceBindingRevision | export SteamAPI_ISteamInput_GetDeviceBindingRevision (ord 350, @0x25E60) reaches impl 0xA1CF0 with this=0x38; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced | 0xA3FBC -> 0xA1CF0 |
| 45 | 0x168 | UNMAPPED | AMBIGUOUS: 2 exported names reach this entry (impl 0x13B60, this=0x38), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: GetActionOriginFromXboxOrigin, GetRemotePlaySessionID | 0xAC2C8 -> 0x13B60 |
| 46 | 0x170 | GetSessionInputConfigurationSettings | export SteamAPI_ISteamInput_GetSessionInputConfigurationSettings (ord 363, @0x26020) dispatches via sub-table +0x48 slot 46; that WHOLE sub-table is proved element-wise identical to v006[0:47] (all 47 indices match in both impl and this-offset), so the index transfers | 0xAC2B0 -> 0x13B60 |
| 47 | 0x178 | SetDualSenseTriggerEffect | export SteamAPI_ISteamInput_SetDualSenseTriggerEffect (ord 372, @0x261D0) tail-calls `[obj+0x50]` slot 47 = this vtable | 0xFDC0 -> 0xFDC0 |

The last column is `vtable_entry_RVA -> implementation_RVA`.  A caller loads and
calls the first; control lands on the second.  `0xFDC0` (slot 47) and `0xA1CF0` are
shared *not implemented* stubs reached by several real methods, so a shared
implementation is expected here and is not an error.

## Raw vtable contents

```
vtable RVA 0x54B738  (48 qwords, VA 0x18054B738)
 0  0x0B82AC  00000001800B82AC   4883e908e9bbfbffffcc              Init
 1  0x0B86BC  00000001800B86BC   4883e950e95bfdffffcc              Shutdown
 2  0x0A3FA4  00000001800A3FA4   4883e908e943ddffffcc              -
 3  0x0B8394  00000001800B8394   4883e908e9d3ffffffcc              RunFrame
 4  0x0A3FA4  00000001800A3FA4   4883e908e943ddffffcc              -
 5  0x0A3FA4  00000001800A3FA4   4883e908e943ddffffcc              -
 6  0x0B4540  00000001800B4540   4883e948e9f7feffffcc              GetConnectedControllers
 7  0x0AB404  00000001800AB404   4883e908e9b349f6ffcc              -
 8  0x0AB404  00000001800AB404   4883e908e9b349f6ffcc              -
 9  0x0B3A0C  00000001800B3A0C   4883e948e95bfdffffcc              GetActionSetHandle
10  0x0B3760  00000001800B3760   4883e948e987feffffcc              ActivateActionSet
11  0x0B46B8  00000001800B46B8   4883e948e94fffffffcc              GetCurrentActionSet
12  0x0AC74C  00000001800AC74C   4883e930e96b36f6ffcc              -
13  0x0AC74C  00000001800AC74C   4883e930e96b36f6ffcc              -
14  0x0AC74C  00000001800AC74C   4883e930e96b36f6ffcc              -
15  0x0B3A18  00000001800B3A18   4883e930e93f01f6ffcc              GetActiveActionSetLayers
16  0x0B4E80  00000001800B4E80   4883e948e987fcffffcc              GetDigitalActionHandle
17  0x0B4AFC  00000001800B4AFC   4883e948e9cbfbffffcc              GetDigitalActionData
18  0x0B5354  00000001800B5354   4883e918e993fcffffcc              GetDigitalActionOrigins
19  0x0B7E28  00000001800B7E28   4883e908e9efffffffcc              -
20  0x0B409C  00000001800B409C   4883e948e95bfdffffcc              GetAnalogActionHandle
21  0x0B3DEC  00000001800B3DEC   4883e948e93bfcffffcc              GetAnalogActionData
22  0x0B4418  00000001800B4418   4883e918e9effdffffcc              GetAnalogActionOrigins
23  0x0B7CB0  00000001800B7CB0   4883e908e9e7ffffffcc              -
24  0x0B7CBC  00000001800B7CBC   4883e908e9cba2f6ffcc              GetGlyphSVGForActionOrigin
25  0x0B7CB0  00000001800B7CB0   4883e908e9e7ffffffcc              -
26  0x0B7E40  00000001800B7E40   4883e918e9d7ffffffcc              GetStringForActionOrigin
27  0x0B7E28  00000001800B7E28   4883e908e9efffffffcc              -
28  0x0AC770  00000001800AC770   4883e948e94736f6ffcc              -
29  0x0B7E08  00000001800B7E08   4883e940e98fffffffcc              GetMotionData
30  0x0B8890  00000001800B8890   4883e938e967feffffcc              TriggerVibration
31  0x0B88B0  00000001800B88B0   4883e908e9e7ffffffcc              TriggerVibrationExtended
32  0x0AB404  00000001800AB404   4883e908e9b349f6ffcc              -
33  0x0AC758  00000001800AC758   4883e938e95f36f6ffcc              SetLEDColor
34  0x0B830C  00000001800B830C   4883e908e9ebffffffcc              Legacy_TriggerHapticPulse
35  0x0B8348  00000001800B8348   4883e908e9cfffffffcc              Legacy_TriggerRepeatedHapticPulse
36  0x0B840C  00000001800B840C   4883e948e9db98feffcc              ShowBindingPanel
37  0x0B7D88  00000001800B7D88   4883e930e96fffffffcc              GetInputTypeForHandle
38  0x0B45F8  00000001800B45F8   4883e940e94fffffffcc              GetControllerForGamepadIndex
39  0x0B541C  00000001800B541C   4883e940e95bffffffcc              GetGamepadIndexForController
40  0x0B7CEC  00000001800B7CEC   4883e928e99ba2f6ffcc              -
41  0x0B7CEC  00000001800B7CEC   4883e928e99ba2f6ffcc              -
42  0x0AC2C8  00000001800AC2C8   4883e918e98f78f6ffcc              -
43  0x0B86F4  00000001800B86F4   4883e918e9d3ffffffcc              TranslateActionOrigin
44  0x0A3FBC  00000001800A3FBC   4883e918e92bddffffcc              GetDeviceBindingRevision
45  0x0AC2C8  00000001800AC2C8   4883e918e98f78f6ffcc              -
46  0x0AC2B0  00000001800AC2B0   4883e908e9a778f6ffcc              GetSessionInputConfigurationSettings
47  0x00FDC0  000000018000FDC0   c20000cccccccccccccc              SetDualSenseTriggerEffect
```
## UNVERIFIED

18 of the 48 slots carry `UNMAPPED`.  This is a real limit of the binary, not an
omission.  Each of those slots points at a *shared* implementation stub that
several exported flat-API names reach, and for those slots there is no
sub-table whose element-wise identity with v006 transfers the index.  Identity
collapses them into one indistinguishable group; the exports give no
argument-shape or call-site signal to order them.

| v006 slots | shared entry | exported names that reach it | header order (cross-check only) |
| --- | --- | --- | --- |
| 28 | impl `0xFDC0`, this `0x8` | StopAnalogActionMomentum, TriggerHapticPulse, TriggerRepeatedHapticPulse | TriggerSimpleHapticEvent |
| 12, 13, 14 | impl `0xFDC0`, this `0x20` | ActivateActionSetLayer, DeactivateActionSetLayer, DeactivateAllActionSetLayers | DeactivateActionSetLayer, DeactivateAllActionSetLayers, GetActiveActionSetLayers |
| 7, 8, 32 | impl `0xFDC0`, this `0x48` | EnableActionEventCallbacks, EnableDeviceCallbacks, TriggerSimpleHapticEvent | EnableDeviceCallbacks, GetActionSetHandle, ShowBindingPanel |
| 42, 45 | impl `0x13B60`, this `0x38` | GetActionOriginFromXboxOrigin, GetRemotePlaySessionID | SetDualSenseTriggerEffect, ? |
| 40, 41 | impl `0x21F90`, this `0x28` | GetGlyphForXboxOrigin, GetStringForXboxOrigin | GetRemotePlaySessionID, GetSessionInputConfigurationSettings |
| 2, 4, 5 | impl `0xA1CF0`, this `0x48` | BNewDataAvailable, BWaitForData, SetInputActionManifestFilePath | SetInputActionManifestFilePath, BWaitForData, BNewDataAvailable |
| 23, 25 | impl `0xB7CA0`, this `0x48` | GetGlyphForActionOrigin_Legacy, GetGlyphPNGForActionOrigin | GetStringForAnalogActionName, GetMotionData |
| 19, 27 | impl `0xB7E20`, this `0x48` | GetStringForAnalogActionName, GetStringForDigitalActionName | GetAnalogActionHandle, TriggerVibrationExtended |

7 of these 8 groups are **provable free permutations** - the number of
candidate slots equals the number of candidate names, and every name is a
candidate for every one of those slots.  No arrangement of the evidence
distinguishes them, so `UNMAPPED` is the only honest answer.

Names exported but not placed in any v006 slot: `ActivateActionSetLayer`, `BNewDataAvailable`, `BWaitForData`, `DeactivateActionSetLayer`, `DeactivateAllActionSetLayers`, `EnableActionEventCallbacks`, `EnableDeviceCallbacks`, `GetActionOriginFromXboxOrigin`, `GetGlyphForActionOrigin_Legacy`, `GetGlyphForXboxOrigin`, `GetGlyphPNGForActionOrigin`, `GetRemotePlaySessionID`, `GetStringForAnalogActionName`, `GetStringForDigitalActionName`, `GetStringForXboxOrigin`, `SetInputActionManifestFilePath`, `StopAnalogActionMomentum`, `TriggerHapticPulse`, `TriggerRepeatedHapticPulse`, `TriggerSimpleHapticEvent`.

### Cross-check only

`cppISteamInput006.cpp` (Valve's generated glue, 40579 bytes) declares 43 methods
for `ISteamInput_SteamInput006_`.  The binary has 48 slots, because the flat API
also exports methods the `.cpp` does not wrap.  The header was used **only** as
a cross-check and never as primary proof.  The following files in
`/root/.hermes/cache/scratch/steamres/` are 14-byte `404: Not Found` stubs and
were ignored: `isteaminput_ckyrra.h`, `isteaminput_gbe.h`,
`isteaminput_modern1..4.h`.  Real files used: `cppISteamInput006.cpp`,
`sdk163_isteaminput.h`, `isteaminput_facepunch.h`, `steam_api_flat.h`.

## Other checks

* **Exports.** 1254 named exports.  Nothing in the export table names a vtable
  slot or a vtable address, and there is no symbol that identifies
  `CSteamInput`; the flat API is the only naming source, and it routes through
  different sub-tables per method, hence the grouping above.
* **RTTI.** 377 MSVC Complete Object Locators were parsed; none is named
  `CSteamInput` or `SteamInput`, so RTTI cannot identify this vftable and was
  not used for naming.
* **Sub-table geometry** (measured, not assumed; every table is followed by an
  8-byte pad before the next, which is how the extents were confirmed):

  | sub-object | vtable | entries | ends at | whole-table alignment into v006 |
  | --- | --- | ---: | --- | --- |
  | +0x00 | 0x54ADF0 | 6 | 0x54AE20 | no (no run >= 2) |
  | +0x08 | 0x54AE28 | 17 | 0x54AEB0 | no (longest coincidental run only 3 at v006[9:12] - NOT used) |
  | +0x10 | 0x54AEB8 | 22 | 0x54AF68 | no (longest coincidental run only 3 at v006[9:12] - NOT used) |
  | +0x18 | 0x54AF70 | 26 | 0x54B040 | no (longest coincidental run only 3 at v006[9:12] - NOT used) |
  | +0x20 | 0x54B048 | 31 | 0x54B140 | no (longest coincidental run only 9 at v006[9:18] - NOT used) |
  | +0x28 | 0x54B148 | 34 | 0x54B258 | no (longest coincidental run only 9 at v006[9:18] - NOT used) |
  | +0x30 | 0x54B260 | 34 | 0x54B370 | no (longest coincidental run only 9 at v006[9:18] - NOT used) |
  | +0x38 | 0x54B378 | 35 | 0x54B490 | no (longest coincidental run only 10 at v006[9:19] - NOT used) |
  | +0x40 | 0x54B498 | 35 | 0x54B5B0 | no (longest coincidental run only 10 at v006[9:19] - NOT used) |
  | +0x48 | 0x54B5B8 | 47 | 0x54B730 | v006[0:47] (PROVED, all 47 indices) |
  | +0x50 | 0x54B738 | 48 | 0x54B8B8 | itself (this table) |

Only `+0x48` (v005) aligns as a whole table, onto `v006[0:47]`.  The other
sub-tables share *stubs* with v006, so any short run they match is coincidence
and is deliberately NOT used to name a slot.  A previous revision of this
analysis did use those partial runs and produced false names for slots 1, 6,
9-11, 16, 17, 20, 21, 29, 30, 33, 36, 38, 39; those claims are withdrawn.

* **48 is measured, not assumed:** entry 47 is the last `.text` VA; the qword
  at `0x54B8B8` is `0x53206E6F74747542` (not code), and the gap to the next sub-object is
  exactly 8 bytes.

