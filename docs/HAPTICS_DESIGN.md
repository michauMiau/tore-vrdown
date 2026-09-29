# Teardown → VR controller haptics: how the game does it, and how to route it to a headset

Date: 2026-09-27. Binary analysed: `/root/steamless/Teardown/teardown.exe` (decrypted unpacked build).
Image base `0x140000000`. All addresses below are **VA** unless marked RVA.

**Evidence rule used in this document.** Every claim is tagged:

- **[V] VERIFIED** — I ran a command against the file and quote the result.
- **[I] INFERRED** — reasoning on top of measurements, labelled as such.
- **[X] REFUTED** — a previously recorded claim that measurement contradicts.

Tools used: `tools/pe.py` (own PE parser, written for this task) and `tools/dis.py`
(pdata-driven Capstone disassembly — `.pdata` has 35 675 `RUNTIME_FUNCTION` entries,
so every function is disassembled from its own entry, which avoids the
linear-sweep desync that produces phantom xrefs).

---

## 0. Executive summary

1. **The game has no XInput, no DirectInput, no GameInput, no OpenXR.** VERIFIED.
   All 33 imported DLLs enumerated; zero input or XR libraries. Zero ASCII and
   zero UTF-16 occurrences of `xinput`/`GameInput`/`dinput`/`openxr`.
2. **Haptics terminate in the engine's own input backend**, behind a C++ virtual
   call — not in a Windows API. VERIFIED: a single virtual `call [rax+0x78]`.
3. **The clean interception point is `0x14018D520`**, which takes the already-resolved
   4-channel motor amplitude (4 floats) in `r8`. This is the last point at which
   real, final haptic data exists. VERIFIED as the sole caller path.
4. **Teardown's own amplitude scale is already 0.0–1.0**, clamped by a literal
   `1.0` constant. VERIFIED. It maps onto `XrHapticVibration.amplitude` with no
   rescaling.
5. **Recommended route: read the floats at `0x14018D520`, emit via OpenXR
   `xrApplyHapticFeedback` in a headless (`XR_MND_headless`) session.** Haptics are
   *independent of the display* — this is the key fact that makes flat-window VR
   viable for haptics even though it does not make it viable for stereo video.

**One important correction to the prior note** (`docs/HAPTICS_FOUND.md` §2 and §4):
that note claims haptics end in "a call to Steam Input" and proposes mapping the
XML to `XrHapticAction`/`xrCreateHapticAction`. Both halves of that are wrong, and
the second is wrong in a way that would produce non-compiling code. See §7.

---

## 1. What the game imports — VERIFIED

Own parser, data directory **[1]** only, 33 descriptors:

```
sentry.dll mimalloc.dll WSOCK32.dll WS2_32.dll OPENGL32.dll dxgi.dll dbghelp.dll
amd_ags_x64.dll KERNEL32.dll USER32.dll GDI32.dll ADVAPI32.dll SHELL32.dll ole32.dll
OLEAUT32.dll MSVCP140.dll IMM32.dll steam_api64.dll DSOUND.dll bcrypt.dll
VCRUNTIME140.dll VCRUNTIME140_1.dll api-ms-win-crt-*.dll (11)
```

Searched for `xinput|dinput|gameinput|rumble|vibrat|haptic|openvr|openxr` across every
thunk name and every DLL name: **no hits**. [V]

Byte-level search of the whole 30.5 MB image, ASCII **and** UTF-16LE: [V]

| needle | hits |
|---|---|
| `openxr` / `OpenXR` / `openxr_loader` | 0 |
| `xrCreateInstance` / `xrApplyHapticFeedback` / `xrCreateHapticAction` | 0 |
| `XR_RUNTIME` | 0 |
| `xinput` / `XInput` | 0 |
| `GameInput` | 0 (except the RTTI type name `.?AVGameInput@@`, see §3) |
| `dinput` | 0 |

The only XR-relevant string in the file is `vulkan` ×2, both inside the
`GL_NV_draw_vulkan_image` extension name. [V]

**Consequence:** there is nothing to hook in the IAT for haptics. The game never
calls a Windows rumble API. Any interception must be at the engine's own code.

### 1.1 What the game *does* import from Steam — VERIFIED

`steam_api64.dll`, all 12 thunks, IAT at RVA `0x9831A8`:

```
SteamAPI_GetHSteamUser                    SteamInternal_ContextInit
SteamInternal_FindOrCreateUserInterface    SteamAPI_RegisterCallback
SteamAPI_Shutdown                         SteamAPI_Init
SteamInternal_CreateInterface             SteamAPI_RunCallbacks
SteamAPI_UnregisterCallResult             SteamAPI_RegisterCallResult
SteamAPI_UnregisterCallback               SteamInternal_FindOrCreateGameServerInterface
```

The first three are the flat-C interface bootstrap. That is a **VERIFIED** fact
about the imports. It is **not** evidence that haptics flow through Steam Input —
see §7.1.

---

## 2. Haptic assets on disk — VERIFIED

`find <game> -ipath '*haptic*' -type f | wc -l` → **97** files, all plain XML,
all `.xml` (0 under any `haptic/` path is a `.tde`; the tree has 1360 `.tde` elsewhere).

| branch | count |
|---|---|
| `data/haptic/` | 57 |
| `mods/*/haptic/` | 21 |
| `dlcs/*/haptic/` | 19 |

`mods/{folkrace,tillaggaryd,wildwestheist,tg}` are byte-identical duplicates of
their `dlcs/` counterparts, so unique content is **58** files, not 97. [V]

### 2.1 The format

`data/haptic/gun_fire.xml`, verbatim: [V]

```xml
<haptic_effect>
<lifetime>0.1</lifetime>
<keypoints type="motor" index="0">
	<point pos="0.5 0.4"/>
</keypoints>
<keypoints type="motor" index="1">
	<point pos="0.5 0.4"/>
</keypoints>
<advanced_vibration>
	<file src="advanced/tools/gun0.wav"/>
</advanced_vibration>
</haptic_effect>
```

Full element census across all 97 files: `point` 256, `keypoints` 138, `lifetime` 106,
`haptic_effect` 97, `adaptive_trigger` 39, `strength` 33, `file` 30,
`advanced_vibration` 28, `position` 25, `start_position` 14, `end_position` 14,
`frequency` 6, `amplitude` 6. Attribute set is exactly `pos, type, index, src, channel`. [V]

`keypoints type` ∈ {`motor`: 134, `trigger`: 4}; `adaptive_trigger type` ∈
{`feedback`: 19, `weapon`: 14, `vibration`: 6}. [V]

`<point pos="T A"/>` is **(normalised time, amplitude)** — `T` in 0..1 across
`lifetime`, `A` in 0..1. [I] This reading is consistent with the disassembly in §4.2,
where the four accumulators are clamped with `minss` against a literal `1.0`.

### 2.2 The engine's Lua API — VERIFIED

From `data/script_defs.lua` and `data/script_defs.luau`:

```lua
function LoadHaptic(filepath) return "" end
function CreateHaptic(leftMotorRumble, rightMotorRumble, leftTriggerRumble, rightTriggerRumble) return "" end
function PlayHaptic(handle, amplitude) end
function PlayHapticDirectional(handle, direction, amplitude) end
function HapticIsPlaying(handle) return false end
function SetToolHaptic(id, handle, amplitude) end
function StopHaptic(handle) end
```

**`CreateHaptic` proves the internal model is exactly 4 channels**
(left motor, right motor, left trigger, right trigger). [V] This is a strong
independent confirmation of the disassembly in §4.

30 Lua files call these — e.g. `dlcs/space/script/gloogun.lua:245` uses
`clamp((maxHapticDist - impactDist) / maxHapticDist, 0, 1)` for distance-attenuated
amplitude. [V]

### 2.3 Settings gate

`data/ui/components/gamepad_options_view.lua` writes
`options.input.gamepad.vibration` with exactly two values: `"strong"` and `"off"`. [V]
`data/input_settings.xml:66` has `<parameter name="vibration_stength" value="strong" />`
(shipped typo). [V]

The string `options.input.gamepad.vibration` exists in `teardown.exe` at RVA
`0x990070`, but has **0 RIP-relative code references**. [V] Settings strings are
resolved at runtime through a registry, not by direct LEA. So there is no
"vibration on/off" branch to hook at a known address — **if the user has set
`off`, the game computes zero and the mod receives zero.** [I] The mod must
therefore expose its own override, not rely on the game's setting.

---

## 3. The haptic subsystem in the binary — VERIFIED

`.rdata` strings at RVA `0x9EEDB0..0x9EF3D0` hold **55** contiguous
`haptic/*.xml` name strings. They are reached only through a **55-entry pointer
array**, which lives in `.dataa`, not `.rdata`:

```
array RVA 0xBF3C70 (file off 0xBF2070), 55 * 8 = 440 bytes
  [ 0] 0x1409EEDB0 -> 'haptic/blowtorch_hit_metal.xml'
  [14] 0x1409EEF38 -> 'haptic/gun_fire.xml'
  [54] 0x1409EF3D0 -> 'haptic/ground_snow.xml'
  [55] 0x3F800000   <- not a pointer: array is exactly 55 entries
```

I decoded all 55 qwords; every one resolves to its `haptic/` string in order. [V]

RTTI class names present: `.?AVCSteamInput@@`, `.?AVGameInput@@`, `.?AVInputBackend@@`. [V]
`.?AVGameInput@@` is an MSVC type-descriptor name, so a `GameInput`-backed class
exists in the binary. But **`GameInput.dll` is not imported and the string
`GameInput` appears nowhere else** — so the class exists and the DLL does not. [V]
The RTTI implies an abstraction over input backends; it does not identify which
backend is live at runtime. [I] Do not assume Steam Input, do not assume GameInput.

A 4-entry table at `0x140C483D8` holds fetcher functions, including
`0x1404DA940` (the `SteamInternal_FindOrCreateUserInterface(user,"SteamInput006")`
call). [V] That function has **0 direct callers** and the table has **0 rip-relative
references** — it is reached indirectly, so the interface pointer is cached and
resolved at runtime. [V]

### 3.1 Named function map

| VA | len | role | how established |
|---|---|---|---|
| `0x1401A3D30` | 0x168 | name-table loader; `add rbx,8` stride-8 loop over all 55 | disassembly [V] |
| `0x1401A3EA0` | 0xD45 | XML parser; references **all** haptic tags | xref of every tag string [V] |
| `0x1401A0F90` | 0x13E | haptic subsystem init, calls the loader | caller chain [V] |
| `0x1401A5BC0` | 0x15C3 | per-effect playback; resolves 4 floats | caller chain [V] |
| `0x1401A59C0` | — | effect play, 17 callers | caller chain [V] |
| `0x1401A4DE0` | — | adaptive-trigger path, 35 callers | caller chain [V] |
| **`0x14018D520`** | 0x1096 | **terminal send, 1 caller** | caller chain [V] |
| `0x1404DA940` | 0x27 | SteamInput006 interface fetch | disassembly [V] |

---

## 4. The send path, traced end to end — VERIFIED

### 4.1 Terminal dispatch

`0x14018D520`, 1 caller (`0x140071BDC` in `0x140071230`). At `0x14018D9DB`:

```asm
0x14018d9bf  movzx  edx, byte ptr [r14 + 0x410]   ; controller / slot id
0x14018d9c7  mov    rcx, r15                        ; this  (input backend)
0x14018d9ca  call   qword ptr [rax + 0x48]          ; backend slot 9: is-active check
0x14018d9d1  mov    rax, qword ptr [r15]
0x14018d9d4  lea    rbx, [r14 + 0x2168]            ; arg5
0x14018d9db  lea    r8,  [r14 + 0x20e8]             ; <== 4 motor floats
0x14018d9ea  movzx  edx, byte ptr [r14 + 0x410]
0x14018d9f2  mov    rcx, r15
0x14018d9f5  call   qword ptr [rax + 0x78]          ; <== TERMINAL SEND
```

`r8` points at the resolved haptic output block. That block is written by
`0x1401A5BC0` at `0x1401A6C5D`: [V]

```asm
0x1401a6c5d  mov    rax, qword ptr [r11 + 0x38]    ; input manager
0x1401a6c61  movups xmmword ptr [rax + 0x20e8], xmm6   ; 4 floats: L,R,Ltrigger,Rtrigger
0x1401a6c68  movups xmmword ptr [rax + 0x20f8], xmm14
0x1401a6c70  movups xmmword ptr [rax + 0x2108], xmm15
0x1401a6c78  movups xmmword ptr [rax + 0x2118], xmm0
```

The four accumulators are packed and stored in a fixed order: [V]

```asm
0x1401a6c33  movss  dword ptr [rbp + 0xc], xmm4
0x1401a6c38  movss  dword ptr [rbp + 8], xmm5
0x1401a6c3d  movss  dword ptr [rbp + 4], xmm6
0x1401a6c42  movss  dword ptr [rbp],      xmm7
0x1401a6c47  movaps xmm6, xmmword ptr [rbp]      ; pack 4 -> 1
0x1401a6c61  movups xmmword ptr [rax + 0x20e8], xmm6
```

so the block at `+0x20E8` is `{xmm7, xmm6, xmm5, xmm4}` in that lane order. [V]

Within the per-effect accumulation loop the lanes come from
`[entry + 0x28/0x2C/0x30/0x34]`, and those same four offsets are written
individually during parse at `0x1401A6D4B`–`0x1401A6DD1`, so the ordering is
preserved end-to-end. [V]

**Which lane is which named channel is [I], not measured.** The loop selects groups
by `cmp dword [entry], 1` (a type tag) and the XML distinguishes `type="motor"`
from `type="trigger"` with `index="0"/"1"`, so lane 0/1 = motors and lane 2/3 =
triggers is the natural reading; the `CreateHaptic(leftMotor, rightMotor,
leftTrigger, rightTrigger)` Lua signature (§2.2) independently fixes that the set is
{motor, motor, trigger, trigger} in that order. [I]

The global holding the input manager is at **`0x141C710B0`** (RVA `0x1C710B0`),
resolved from two independent RIP-relative sites, `0x1401A5C87` and `0x1401A6541`. [V]

### 4.2 The amplitude scale is already 0.0–1.0 — VERIFIED

In `0x1401A5BC0` every channel is clamped against a literal:

```asm
0x1401a6e0a  maxss  xmm1, xmm7     ; 0.0
0x1401a6e0e  minss  xmm1, xmm8     ; xmm8 = 1.0  (0x140986C04)
0x1401a6c4b  movss  xmm8, dword ptr [rip + 0x7dffb0]   ; -> 0x140986C04
```

I read the target: **`1.0`**. [V] A second floor constant at `0x14098EA88` is
`1.1920929e-07` — a denormal epsilon, i.e. a divide-by-zero guard, not a scale. [V]

So Teardown's haptic output is **`float` in [0.0, 1.0] per channel**. This is
exactly `XrHapticVibration.amplitude`'s range. No rescaling needed. [V]

### 4.3 What is NOT in the binary

- `advanced_vibration` — **0 occurrences** in `teardown.exe`. [V] The tag is in
  28 shipped XML files but the engine binary has no reference to it.
  [X] The prior note implies the engine handles it. It does not appear to.
- `vibration_stength` — 0 occurrences (it lives only in `input_settings.xml`). [V]
- `trigger_effects_strength` — 0 occurrences. [V]

**Interpretation** `[I]`: the 19 `advanced_vibration` `<file src="…wav">` references
name `.wav` paths, and **there are no `.wav` files anywhere in the tree** — audio
ships as `data/snd/**/*.ogg.tde` and no `data/audio/` directory exists. Those 19
sources are almost certainly dead references in a shipped data format, not a live
audio-haptics channel. Do not build a plan on `advanced_vibration`; there is no
second audio channel to carry. The 4 floats are everything.

---

## 5. What the mod must do — the design

### 5.1 Why the terminal virtual is the right hook, and IAT is not

`0x14018D520` is the right point because it is the **last** moment at which the
data is final and the game's own code still runs normally. The alternatives:

| candidate | verdict |
|---|---|
| IAT of `XInputSetState` | **[X] does not exist** — nothing imported (§1) |
| IAT of Steam haptics | **[X] does not exist** — Steam thunks are only the 12 bootstrap calls (§1.1); `TriggerVibration` is a vtable method on a runtime pointer, not an import |
| `0x1401A5BC0` (per-effect) | works, but returns *resolved per-effect* data, not the merged frame output; more work to reconstruct |
| `0x14018D9F5` (single call insn) | equivalent to the above but needs a byte-detour — and this project has **measured** that byte-stealing detours hang the game (`README.md`, v38–v52) |

So: hook the **function** `0x14018D520`, or better, **vtable slot `0x78` of the
input-backend object**, which is exactly what the project's existing
`vtable_resolve.h` / `present.h` machinery already does successfully for
`Present` (`+0x48`).

### 5.2 The reading side

`r8` at the call site is `inputManager + 0x20E8`. Read 16 bytes = 4 floats.

```c
typedef struct TdHapticOut { float leftMotor, rightMotor, leftTrigger, rightTrigger; } TdHapticOut;
// rcx = input-manager object (r14 in the original frame)
// 0x20E8 offset is from the same object
static TdHapticOut read_haptics(uintptr_t mgr) {
    TdHapticOut h;
    __try { memcpy(&h, (const void*)(mgr + 0x20E8), sizeof h); }
    __except (EXCEPTION_EXECUTE_HANDLER) { memset(&h, 0, sizeof h); }
    return h;
}
```

**Non-blocking replacement: change the vtable slot `0x78` of the backend object to a
stub that (a) captures the 4 floats, (b) calls the original.** Because the object is
an engine class with a stable vtable (`InputBackend` RTTI present), slot `0x78` =
method #15 is a clean install point and needs no instruction patching — which
sidesteps the byte-stealing failure mode entirely.

**Do not** read the global `0x141C710B0` and poll it from a thread. The pointer is
a `.dataa` slot whose backing file region is only `0x107E000` bytes while
`VirtualSize` is `0x13A47E8`; the tail is zero-fill, and a polled read races the
game's writes. Reading `r8` inside the call is exact and race-free. [I]

### 5.3 The emitting side — OpenXR, in a headless session

**This is the crux of the design, and the answer is favourable.** Haptics do not
need the display. OpenXR defines this explicitly.

`xrApplyHapticFeedback` / `xrStopHapticFeedback` are **OpenXR 1.0 core**:

```c
XrResult xrApplyHapticFeedback(XrSession session,
                               const XrHapticActionInfo* hapticActionInfo,
                               const XrHapticBaseHeader* hapticFeedback);

typedef struct XrHapticVibration {
    XrStructureType type;   // XR_TYPE_HAPTIC_VIBRATION
    const void*     next;
    XrDuration      duration;   // NANOSECONDS
    float           frequency;  // 0 == XR_FREQUENCY_UNSPECIFIED
    float           amplitude;  // 0..1
} XrHapticVibration;

typedef struct XrHapticActionInfo {
    XrStructureType type;   // XR_TYPE_HAPTIC_ACTION_INFO
    const void*     next;
    XrAction        action;
    XrPath          subactionPath;   // /user/hand/left | /user/hand/right
} XrHapticActionInfo;
```

Minimum viable setup — **no rendering, no swapchain, no graphics binding**:

```c
// 1. instance, with XR_MND_headless in enabledExtensionNames
// 2. xrGetSystem  -> XR_SYSTEM_HAND_TRACKING_NOT_SUPPORTED is fine
// 3. ONE action, VIBRATION_OUTPUT, two subaction paths
XrActionCreateInfo aci = {XR_TYPE_ACTION_CREATE_INFO};
strcpy_s(aci.actionName, "haptic", "haptic");
aci.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;   // 100
aci.countSubactionPaths = 2; aci.subactionPaths = (XrPath[]){leftHand, rightHand};
xrCreateAction(actionSet, &aci, &vibrateAction);
// 4. bind to /user/hand/{left,right}/output/haptic per profile
//    suggest khr/simple_controller + oculus/touch_controller
//    + valve/index_controller + htc/vive_controller
// 5. xrCreateSession with NO XrGraphicsBinding  <- allowed only with XR_MND_headless
// 6. xrAttachSessionActionSets; xrBeginSession
```

**`XR_MND_headless` is the load-bearing extension.** Spec §12.193, quoted:

> If an application does not include an `XrGraphicsBinding*` structure in the next
> chain of `XrSessionCreateInfo`, the runtime **must** create a "headless" session
> that does not interact with the display. … The application **does not need to
> call `xrWaitFrame`, `xrBeginFrame`, or `xrEndFrame`**, unlike with non-headless
> sessions.

Runtime support, from the Khronos OpenXR Inventory matrix: [source:
`github.khronos.org/OpenXR-Inventory/runtime_extension_support.html`]

| | SteamVR | Meta (Quest Link) | Monado | WMR |
|---|---|---|---|---|
| core haptics | yes (1.0 core) | yes (1.0 core) | yes | not documented |
| `XR_MND_headless` | **supported** | **not supported** | supported | not supported |

Consequences, and they differ per runtime — this is the real-world fork:

- **SteamVR (Vive, Index, Quest-Via-SteamVR, Pimax):** `XR_MND_headless` works.
  Valve confirmed in the SteamVR 1.23.1/1.23.2 beta thread that headless sessions
  "proceed to the focused state and enable input regardless of the headset state",
  and that all headless null-deref bugs were fixed in 1.23.2. This is the good path.
- **Meta Quest Link (Oculus PC runtime):** does **not** support `XR_MND_headless`.
  A session without a graphics binding will fail. You would need a real graphics
  binding (a D3D11/D3D12 `XrGraphicsBindingD3D12KHR` chained in) while never
  submitting frames. This is unproven for Meta — [UNCERTAIN], must be tested.
  Meta does support `XR_FB_haptic_pcm` and `XR_FB_haptic_amplitude_envelope`.

Note the `README.md` line "no OpenXR loader on the machine" — the loader must be
shipped. `openxr_loader.dll` is loaded via plain `LoadLibrary` + `GetProcAddress`
from the mod; no manifest or `XR_RUNTIME_JSON` change is required, because the
loader discovers the system runtime through the registry. [I, standard OpenXR
loader behaviour]

### 5.4 Mapping Teardown's keyframe curve onto pulses

**There is no `xrCreateHapticAction`.** One `XrAction` is created once and reused
for the whole session. Spec §11.7, quoted:

> If a haptic event is sent to an action before a previous haptic event completes,
> the latest event will take precedence and the runtime must cancel all preceding
> incomplete haptic events on that action.

So the play model is **successive interrupting pulses**:

1. Keep a 100 Hz (10 ms) timer thread.
2. Each tick, take the 4 floats captured in §5.2 and emit one pulse per hand:
   ```c
   XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
   v.duration  = 20'000'000;                 // 20 ms, REAL nanoseconds
   v.amplitude = h.leftMotor;                 // already 0..1, no rescale
   v.frequency = XR_FREQUENCY_UNSPECIFIED;    // 0 -> runtime picks
   XrHapticActionInfo ai = {XR_TYPE_HAPTIC_ACTION_INFO};
   ai.action = vibrateAction; ai.subactionPath = leftHand;
   xrApplyHapticFeedback(session, &ai, (XrHapticBaseHeader*)&v);
   ```
3. Motor channel → hand. `leftMotor`→`/user/hand/left`, `rightMotor`→`/user/hand/right`.
4. **Trigger channels have no core OpenXR equivalent.** [V] Core
   `XrHapticVibration` is a single scalar amplitude per hand; there is no
   "position along trigger travel" channel. Options:
   - Use profile-specific output paths `/output/haptic_left_trigger` /
     `/output/haptic_right_trigger` where the profile exposes them (Oculus Touch
     does), and modulate amplitude.
   - Otherwise fold trigger energy into the same hand's motor amplitude.
   - `XR_EXT_haptic_parametric` gives true keyframe curves but is **not supported by
     any shipping runtime** per the Khronos matrix (0/6). Don't plan on it.
   - `XR_MNDX_force_feedback_curl` is **provisional**, not usable.

**Gotchas that will cost real time if unhandled** (all from the spec text):

- `duration` is **real nanoseconds**. There is a documented community report that
  Index haptics silently produce nothing at `duration` 300/1000 (ns) — use
  ≥10 ms. [UNCERTAIN, forum testimony, but consistent with the spec's
  "clamped to implementation-dependent ranges"]
- Durations and frequencies **are clamped to implementation-dependent ranges**;
  you cannot reproduce the envelope exactly. 10–20 ms slices are short enough to
  track the curve acceptably.
- `frequency` is the motor carrier, not your envelope rate. Leave it
  `XR_FREQUENCY_UNSPECIFIED`.
- **Haptics require focus.** If the session is not focused the runtime must return
  `XR_SESSION_NOT_FOCUSED` and fire nothing. Every pulse must tolerate this return
  code and be dropped, not asserted. Losing focus should also stop in-flight pulses.
- Unbound output action = silent no-op that still returns `XR_SUCCESS`. You cannot
  detect a binding failure from the return code; validate once via a pose action.

### 5.5 Steam Input as a haptic *source* — verified dead end for delivery

Worth recording so nobody re-treads it. Valve's own `ESteamInputType` enum has
**15 device classes and not one is a VR controller** — SteamController, XBox360,
XBoxOne, GenericGamepad, PS4, AppleMFi, Android, SwitchJoyConPair/Single, SwitchPro,
MobileTouch, PS3, PS5, SteamDeck. `GetConnectedControllers` therefore never
returns a VR controller handle, and Valve's docs state plainly that the call
"will be ignored for incompatible controller models". [CONFIRMED, Valve's header]

So: **Steam Input rumble cannot reach a SteamVR controller.** The project's
`steaminput/*.vdf` files contain `haptic_intensity` / `haptic_intensity_override`,
but those are a *per-binding UI scale* on that binding's own action — they do not
bridge gamepad rumble to a VR controller. [CONFIRMED]

This is a **confirmed negative**, and it is the single most useful negative in the
project: it removes the "just forward the game's rumble" family of designs.

### 5.6 If OpenXR is unavailable, the fallbacks

Ranked. All are worse than §5.3.

1. **SteamVR classic input API** — `IVRInput::TriggerHapticVibrationAction(handle,
   startSec, durationSec, frequency, amplitude)`. Same hardware, needs its own
   action manifest with a `"type": "vibration"` output action. Respects the user's
   per-binding SteamVR haptics settings. Good fallback if OpenXR init fails.
2. **Meta/Oculus PC SDK** — per-device, licence-gated, version-fragile. Last resort.
3. **GameInput `IGameInputDevice::SetRumbleState(GameInputRumbleParams*)`** — the
   rumble API does exist (`lowFrequency`, `highFrequency`, `leftTrigger`,
   `rightTrigger`) [the claim that GameInput has no rumble API is wrong for v2+],
   but a VR controller in SteamVR is not a GameInput device. Useful only for a
   physical gamepad buzzing alongside.

---

## 6. Implementation checklist

```
[ ] 1. Ship openxr_loader.dll next to the mod DLL; LoadLibrary + GetProcAddress.
       No manifest change, no XR_RUNTIME_JSON change.
[ ] 2. Create XrInstance with XR_MND_headless enabled.
[ ] 3. Create action set + ONE XR_ACTION_TYPE_VIBRATION_OUTPUT action,
       2 subaction paths (/user/hand/left, /user/hand/right).
[ ] 4. Suggest bindings for: khr/simple_controller, oculus/touch_controller,
       valve/index_controller, htc/vive_controller.
       (skip /user/hand/*/output/haptic_*_trigger unless profile supports it)
[ ] 5. xrCreateSession with NO graphics binding (SteamVR path).
       For Meta: chain XrGraphicsBindingD3D12KHR but never submit frames  [UNTESTED]
[ ] 6. Attach action sets, xrBeginSession, poll session state to FOCUSED.
[ ] 7. Install vtable hook: input-backend object, slot 0x78.
       Capture r8 = mgr+0x20E8 as 4 floats. Call the original.
       (Do NOT byte-steer: measured to hang the game in v38-v52.)
[ ] 8. 100 Hz emitter thread: 20 ms pulses, amplitude = captured float as-is,
       frequency = XR_FREQUENCY_UNSPECIFIED, tolerate XR_SESSION_NOT_FOCUSED.
[ ] 9. Own on/off + gain setting in the mod. The game's
       options.input.gamepad.vibration="off" produces zeros and cannot be relied on
       (the string has 0 code xrefs; it is registry-resolved).
[ ] 10. Log per-session: extensions actually enabled, session state, and the first
       N captured frames, to confirm non-zero floats before tuning anything.
```

**Validation order** (cheapest first, because a wrong assumption here invalidates
everything above):

1. Does the mod ever see **non-zero** floats at `mgr+0x20E8`? If not, the hook is
   wrong — stop. (Log raw floats + the `0x410` controller byte.)
2. Does `xrCreateSession` succeed headless on the target runtime? If `Meta`, this is
   the expected failure point.
3. Does one hard-coded 20 ms / amplitude 1.0 pulse produce a physical buzz? This
   isolates OpenXR from the capture path.
4. Only then wire 100 Hz playback of captured values.

Steps 1 and 3 are independent — if 1 passes and 3 fails, the problem is OpenXR, not
the hook. That split is the whole reason to build it in this order.

---

## 7. Corrections to `docs/HAPTICS_FOUND.md`

### 7.1 "The haptic layer ends in a call to Steam Input" — REFUTED [X]

The import table is fully enumerated (§1): `steam_api64.dll` contributes exactly 12
thunks, all of them interface bootstrap / lifetime / callbacks. Not one haptic
method is imported, because `TriggerVibration` is a **vtable method on a runtime-
obtained interface pointer**, not an import. The measurable end of the chain is a
**C++ virtual call `call [rax+0x78]`** on an engine object with `InputBackend`
RTTI (§4.1).

A first pass of this analysis *looked* like it confirmed the note: I found
`call [rax+0xF0]` in the Steam subsystem region and `0xF0 == 30 * 8`, which is
`TriggerVibration` in the 48-slot ISteamInput layout. **It is not.**
[REFUTED by measurement] Both of those sites have `lea r8, [rip+…]` pointing at
the string `"ImGuiBackend::mMesh"`, i.e. they are resource-loader calls, and
`0x1404DA9E0` is a Steam-workshop-URL builder
(`"https://steamcommunity.com/app/%llu/workshop/"`). Displacement alone does not
identify a vtable slot — the same offset means different methods on different
classes. That is exactly the class of unverified structural claim this project has
been burned by before, so it is worth stating plainly.

### 7.2 "This maps 1:1 to `XrHapticAction` / `xrCreateHapticAction`" — REFUTED [X]

**None of those symbols exist in shipping OpenXR.** In OpenXR 1.1.63 they appear
**zero** times in the spec and zero times in `openxr.h`;
`registry.khronos.org/OpenXR/specs/1.1/man/html/XrHapticAction.html` returns 404.
They are from the abandoned 0.9x experimental design. Haptics ship as a normal
**output action** (`XR_ACTION_TYPE_VIBRATION_OUTPUT`) driven by
`xrApplyHapticFeedback`. Code written against the note's names would not compile.

Also wrong in that table: `advanced_vibration` is **not in the binary at all**
(§4.3), and `adaptive_trigger` → `XrActionFeedback` is wrong — no such type
exists. There is no true force-feedback-curl equivalent in shipping core OpenXR.

### 7.3 "The parser is at 0x1A7A90" — off

Measured parser is `0x1401A3EA0` (len `0xD45`); the name-table loader that calls it
in a stride-8 loop is `0x1401A3D30`. 2 callers: the loader and `0x14047B410`
(the `LoadHaptic` script binding impl). [V]

### 7.4 "57 files / 57 embedded names" — 55

55 pointer-table entries and 55 embedded `haptic/*.xml` strings; 57 files on disk.
The two not embedded (`minigun_fire.xml`, `minigun_tool.xml`) are reached from Lua. [V]

---

## 8. Measured-but-not-resolved

Honest list, so the next session doesn't re-guess:

- **Which input backend is live at runtime.** RTTI shows `CSteamInput`,
  `GameInput`, `InputBackend` all compiled in, but `GameInput.dll` is not imported
  and the string appears nowhere else. Which concrete class is assigned to the
  object whose slot `0x78` we hook is **NOT MEASURED**. This does not block the
  design — the hook is on the base `InputBackend` vtable — but it is unknown.
- **The `0x20E8` lane order** is `{xmm7, xmm6, xmm5, xmm4}` [V], but the *naming*
  of those lanes as left/right motor and left/right trigger is [I] (§4.1).
  Cheap to settle: log the 4 floats on a gunshot and see which two move.
- **Whether Meta's runtime accepts a graphics-bound session that never submits
  frames.** Untested. This is the one genuine open risk to the Quest path.
- **`[r14+0x410]`**, the controller/slot byte passed as `edx`. Range and meaning
  unverified. Irrelevant to OpenXR (we send to both hands explicitly) but needed if
  the SteamVR-classic fallback is used.

---

## Appendix: reproducing the measurements

```bash
# imports (33 DLLs, no input/VR)
python3 tools/pe.py /root/steamless/Teardown/teardown.exe

# disassembly by VA (pdata-driven, desync-proof)
python3 tools/dis.py 0x14018D520        # terminal send
python3 tools/dis.py 0x1401A5BC0        # per-effect, 4 floats at the end
python3 tools/dis.py 0x1401A3D30        # 55-entry name-table loop
python3 tools/dis.py 0x1401A3EA0        # XML parser

# vtable call histogram over a range
python3 tools/dis.py --calls 0x1401A2000 0x1401A8000
```

`tools/pe.py` and `tools/dis.py` were written for this task and are in the repo
under `teardown-vr-mod/tools/`. Two bugs in `pe.py` were found and fixed while
writing it, both of which had produced confident-looking garbage: the
`NumberOfRvaAndSizes` offset (must be read as the last field of the optional
header, at `opt+108` for PE32+), and iterating all 16 data directories as if each
were an import table. Anyone reusing this parser should read those two comments.
