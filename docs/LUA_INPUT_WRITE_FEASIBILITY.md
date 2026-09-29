# Can a Teardown Lua mod WRITE or SIMULATE controller input?

**Date:** 2026-09-27
**Question:** is there any way, from a Lua mod, to create input state — a button press, an
axis value, a stick deflection — that the game then treats as real player input?
**Answer:** **No.** There is no Lua API that writes input, and no Lua-reachable path
(native binding, store key, data file, or console command) that manufactures an input event.

This is a *searched* negative. Every claim below names the search that produced it, and the
searches were run against three independent surfaces: the shipped API reference
(`data/script_defs.luau`, 753 declared functions), the shipped Lua corpus
(`mods/` + `dlcs/`, 1073 scripts, plus 348 vanilla scripts in `data/`), and the game binary
(`teardown.exe`, 30,333,440 B).

Three things that *look* like write paths, and precisely why each one fails, are in §4.
Two partial capabilities that are real but are not input fabrication are in §5.

---

## 1. The exhaustive API sweep

`data/script_defs.luau` declares 753 functions. Extracting every declaration
(`^\s*(local\s+)?function\s+NAME`) gives the complete vocabulary the engine exposes to Lua.
Filtering that list on `input|gamepad|pad|key|button|axis|stick|trigger|virtual|inject|simulate|
emulate|device|controller|binding|remap|action|touch|menu|layout` yields **90 candidate
functions**. Every one falls into exactly one of these buckets:

| Bucket | Count | Examples |
|---|---|---|
| Pure readers | 22 | `InputDown` `InputPressed` `InputReleased` `InputValue` `InputLastPressedKey` `LastInputDevice` `IsControllerButtonDown` `HasInputController` `GetActionByButton` `GetButtonsByAction` `GetKeyByAction` `GetLayoutActions` |
| Clear/forget (subtract, never add) | 3 | `InputClear` `InputResetOnTransition` `DisablePlayerInput` |
| Suppressors / gates | 4 | `UiDisableInput` `UiEnableInput` `UiReceivesInput` `UiForceMouse` |
| UI cursor + touch *introspection* | 30 | `UiGetCursorPos` `UiGetMousePos` `UiGetScreenTouchIdInRect` `UiIsScreenTouchIdHandled` `UiSetCursorState` |
| Touch → action *mapping* (see §4.1) | 4 | `UiSendInputScreenTouchAction` `UiGetInputScreenTouchAction` `UiHasInputScreenTouchAction` `UiSetScreenTouchIdHandled` |
| Store, haptics, unrelated false positives | 27 | `SetString` `CreateHaptic` `FindTrigger` `QuatAxisAngle` … |

`UiSetCursorState` is worth one line because its name invites misreading: it sets
**cursor visibility** (0 show / 1 hide / 2 hide+lock, `script_defs.luau:11124`). It does not
write a cursor *position*. Positions come from `UiGetCursorPos`/`UiGetMousePos`, which are readers.

**Searched negative — names that do not exist.** Verified with `strings -n 2` over the whole
binary and `grep` over all 1421 scripts:

```
SetInputState  SetInput  WriteInput  AddInput  VirtualInput  SetKeyState  PressButton
HoldButton     SetButtonState  SetAxisValue  SetGamepad  ForceInput  AutoInput
InputHelper    InputProxy  InjectInput  SimulateInput  SendKey  KeyEvent  PostMessage
```
zero hits in `script_defs.luau`, zero in `mods/`+`dlcs/`, zero as strings in `teardown.exe`.
(`SetInput` ×5 in the corpus is a local table named `resetInputButtonParams`; `SetCursorPos`
exists in the binary only as a **Win32 USER32 import**, not a Teardown API.)

**Searched negative — no device registration.** There is no `RegisterInputDevice`,
`RegisterGamepad`, `AddController`, `RegisterBackend`, or similar. The engine's controller layer
is reached from Lua *only* through the read functions above. Controller enumeration is
`GetConnectedControllers`-shaped and lives below Lua.

---

## 2. The engine never imports a synthetic-input API

From the import directory (33 DLLs, 545 functions), decoded from the PE headers:

| API family | Present? |
|---|---|
| `xinput*` (XInput, XInput1_4, …) | **NO** — no DLL, and no ASCII string containing `xinput` anywhere in the image |
| `dinput*` / DirectInput | **NO** |
| `hid.dll` / `HIDCLASS` / `setupapi` | **NO** |
| `gameinput` / `IGamInput` | **NO** — one 15-byte RTTI fragment `.?AVGameInput@@` exists but has **zero** rip-relative references from `.text` |
| `sdl*` | **NO** — only a 3-byte `"SDL"` fragment inside `.text`, not a loadable path |
| **`SendInput`, `keybd_event`, `mouse_event`** | **NO** |
| **`PostMessage`, `SendMessage`** | **NO** |
| `ISteamInput` (`SteamInput006`) | **YES** — obtained dynamically from `steam_api64.dll` |

The game reads controller state through Steamworks `ISteamInput` and registers Raw Input for
the **MOUSE class only** (usage page 0x02 / usage 0x01), never for gamepads.

**Architectural consequence for the VR mod:** the game never calls XInput, so a virtual XInput
device cannot work. The injection point is the Steam Input layer / `steam_api64.dll`, not
`xinput1_4.dll`.

Also searched and absent: `emulat`, `synthes`, `hijack`, `spoof`, `fakedinput`, `fakeinput`
(0 hits across 756,119 extracted strings). And a setter regex
`^(Set|Write|Add|Push|Apply|Force|Inject|Simulate|Emulate|Virtual).*(Input|Key|Button|Axis|
Controller|Pad|Gamepad|Binding|Action|Device)` over all strings: **0 matches**.

---

## 3. The state that the readers read, and who writes it

I located every native Lua binding for the input API by resolving each name string to its
single VA pointer, then reading the registration record. (Two tables exist: an engine table at
`0xC46880` and a UI table at `0xC47440`, both stride `0x18` = `{nameptr, pad, codeptr}`. The
readers are in the engine table; the `Ui*` touch functions are in the UI table. The stride
matters: the name pointer is at field offset **+0x00** and the code pointer at **+0x10**.)

| Lua name | string RVA | binding RVA | pdata extent |
|---|---|---|---|
| `InputPressed` | `0xA41BA0` | `0x4AD9D0` | `0x4AD9D0..0x4ADBC5` |
| `InputReleased` | `0xA41BB8` | `0x4AD4E0` | `0x4AD4E0..0x4AD6D5` |
| `InputDown` | `0xA41BC8` | `0x4ADC10` | `0x4ADC10..0x4ADE10` |
| `InputValue` | `0xA41BD8` | `0x4ADEF0` | — |
| `InputLastPressedKey` | `0xA41B88` | `0x4AD7D0` | `0x4AD7D0..0x4AD9C5` |
| `InputClear` | `0xA41C60` | `0x4B1F90` | `0x4B1F90..0x4B1FBD` |
| `InputResetOnTransition` | `0xA41C70` | `0x4AE5F0` | — |
| `LastInputDevice` | `0xA41C88` | `0x4AFD10` | — |
| `IsControllerButtonDown` | `0xA41C08` | `0x4B1F60` | — |
| `HasInputController` | `0xA427A8` | `0x4AE310` | — |

All of them share the same prologue. From `InputDown` (`0x4ADC10`):

```
004adc33  call 0x62a270        ; string -> action id (the 51-name table)
004adc3d  call 0x49caa0        ; lua getglobal/top-level fetch
004adc45  mov  rcx, qword ptr [rsi + 0x418]
004adc4c  test rcx, rcx
004adc51  mov  rax, qword ptr [rcx + 0xac88]   ; per-player InputState
004adc5d  lea  rbx, [rax + 0xd0]               ; base of the per-player state block
004adc64  cmp  byte ptr [rsi + 0xb3], 0        ; "is this context allowed to read"
004adc6b  je   0x4adc8b
```

So the readers resolve a **per-player** state block at `[lua+0x418] → +0xAC88 → +0xD0`, and
index it by the action id returned from the name table. They only ever **load** from it.

### The action id table — fully recovered

The initializer at `0x18ABE0` walks a table at `0x9DEB50` of 51 records, stride `0x10`,
layout `{u8 id, pad, char* name}`, upper bound `0x9DEE80`. Dumping it gives the complete,
authoritative name→id map (`table51.py`), ids contiguous `0x00..0x32`:

```
 0 none          1 left         2 right        3 up           4 down
 5 flashlight    6 interact     7 jump         8 crouch       9 usetool
10 grab         11 vehicle_action               12 vehicle_raise
13 vehicle_lower 14 handbrake  15 map         16 pause
17 scroll_up    18 scroll_down 19 tool_group_prev              20 tool_group_next
21 lmb         22 mmb          23 rmb         24 camerax      25 cameray
26 mousex      27 mousey       28 extra0  ...  34 extra6
35 photomode   36 zoom         37 scoreboard  38 menu_left  ... 45 menu_cancel
46 camera_view 47 l_stick_x   48 l_stick_y   49 r_stick_x   50 r_stick_y
```

Note ids 47–50: `l_stick_x/y`, `r_stick_x/y` exist in the table but are **not** in
`input_settings.xml`. Raw stick axes are addressable by name.

### Who writes the state array — a searched negative

I scanned **every function in `.text` independently** using `.pdata` `RUNTIME_FUNCTION`
bounds (35,675 functions, 2,146,793 instructions decoded, no cross-function desync), looking
for any instruction touching the input state window. The object that owns it is constructed at
`0x18A5D0`, which pins the layout exactly:

```
0018a5fc  lea  rcx, [rdi + 0xd60]
0018a605  mov  r8d, 0x198                 ; 0x198 = 51 * 8  -> 51 slots, 8 bytes each
0018a60b  call 0x9120f1                   ; memset the value array
0018a612  mov  dword ptr [rdi + 0xef8], ebp ; touch-record count = 0
0018a61f  mov  qword ptr [rdi + 0xf00], rax ; touch-record vector
0018a626  mov  dword ptr [rdi + 0xefc], 0x33
```

Each slot is `{float value; u8 down; u8 pressed; u8 released}` — the three edge bytes are
cleared per frame at `0x18D520` (`mov word ptr [rcx+0xd65],0`, `mov byte ptr [rcx+0xd64],0`, …).

The **complete** set of functions that write this array:

| Writer | What it does | Lua-reachable? |
|---|---|---|
| `0x71230` → `0x18D520` (1 caller, the per-frame input driver) | polls hardware, resolves bindings, synthesises edge bytes | no — engine frame |
| `0x6D1D0` (4 callers, `0x79420` `0x461FA0` `0x4A2AA0`) | **clones** the whole 51-slot block slot-by-slot (`mov rax,[rdi+0xd60]; mov [rbx+0xd60],rax`, stride 8, 26 pairs) | no |
| `0x81FF0` (`mov qword ptr [rdi+0xd90], rbp` ×36) | **zeroes** the whole block | no |
| `0x83940` (37 refs) | zeroing/reset of the high slots | no |

Every one of them is a whole-object clear, a whole-object clone, or the hardware poll. **There
is no per-slot writer that originates a value from anything but a real device event.** A clone
can *restore* a state that was previously captured; it cannot manufacture one, because its
source is itself only ever written by the poll.

---

## 4. The three false leads, and exactly why each fails

### 4.1 `UiSendInputScreenTouchAction` — the only function that *emits* an action

This is the one real candidate, and it deserves a careful answer because its name and its
signature both suggest injection:

```lua
function UiSendInputScreenTouchAction(actionId: string, touchId: number, value: number) end
```

Binding: string RVA `0xA45238`, record at `0xC47FB0` (UI table index 122), code
`0x4BCB30`, extent `0x4BCB30..0x4BCC51`. Full disassembly of the binding:

```
004bcb44  mov  rsi, r8              ; arg3 = value
004bcb47  mov  rbp, rdx             ; arg2 = touchId
004bcb4a  mov  rdi, rcx             ; lua state
004bcb4d  mov  rbx, qword ptr [rcx + 0x418]
004bcb62  cmp  byte ptr [rdi + 0xb3], 0
004bcb69  jne  0x4bcba0
   ... (reads a global option; if unset, calls 0x691e70/0x691ca0 and RETURNS)
004bcba0  lea  rcx, [rsp+0x40]
004bcba5  call 0x4ff9b0             ; build lua string
004bcbb9  call 0x62a380             ; push arg
004bcbc9  mov  byte ptr [rsp + 0x70], 0x33   ; sentinel 0x33 = "unknown"
004bcbd8  call 0x18b8a0             ; resolve actionId string -> 1-byte id
004bcbdd  test al, al
004bcbdf  jne  0x4bcbe5
004bcbe1  xor  dl, dl               ; UNKNOWN NAME -> return false, no write
004bcbe5  mov  rax, qword ptr [rip + 0x17b44c4]   ; global input object
004bcbec  mov  rax, qword ptr [rax + 0x38]
004bcbf0  movsxd rcx, dword ptr [rax + 0xef8]      ; record count
004bcbf7  test ecx, ecx
004bcbf9  jle  0x4bcc21                            ; empty -> return false
004bcbfe  xor  ecx, ecx
004bcc00  mov  rax, qword ptr [rax + 0xf00]        ; record vector
004bcc07  movzx r8d, byte ptr [rsp + 0x70]         ; our id
004bcc10  cmp  byte ptr [rax], r8b                  ; SCAN for matching record id
004bcc13  je   0x4bcc4c
004bcc15  inc  rcx
004bcc18  add  rax, 0xc
004bcc1f  jl   0x4bcc10
004bcc21  xor  edx, edx              ; not found -> return false
004bcc23  mov  rcx, rsi              ; found: rcx = the touchId argument
004bcc26  call 0x628d20              ; -> 0x621DF0  record packer
004bcc4c  mov  dl, 1                 ; return true
```

Three independent reasons this cannot fabricate input:

1. **It is a query, not a setter.** The function *scans* a record vector for a record whose id
   already matches, and returns a boolean. `0x628D20` → `0x621DF0` is a **Lua argument
   packer**, not an input store: it writes into the caller's argument buffer
   (`[r8]=value; [r8+8]=1; rcx+0x10 += 0x10` — a 16-byte arg slot). It does not touch the
   51-slot state array.
2. **The vector it reads is UI-owned, and nothing merges it into gameplay state.** The 34
   functions referencing the `0xEF0..0xF14` record vector were classified by also checking
   whether they touch the state array or the remap table. The input-system ones are
   `0x3D550` (reset), `0x6D1D0` (clone), `0x18A5D0` (construct), `0x18AED0`/`0x18B0F0`/
   `0x18B220`/`0x18D478` (vector push/pop), and `0x18D520` (the poll). `0x18D520`'s use of the
   vector is at `0x18E3F0..`, where it compares a **touch id** against UI touch geometry via
   `0x4EAFD0`/`0x4ED3E0` and *removes* matched records — it does not write a slot value.
3. **The corpus never calls it with a fabricated id.** 55 call sites, all in `data/` (10 files)
   plus exactly one mod, `mods/islaestocastica/assets/script/turret.lua`. Every one obtains
   the touchId from a `UiGetScreenTouchIdStarted*` family function, which returns **0** when
   no finger is down, and gates on `touchId ~= 0`:

   ```lua
   -- mods/islaestocastica/assets/script/turret.lua:379  (mirrored in data/level/carib/script/turret.lua)
   local shootTouchId = UiGetScreenTouchIdStartedInCircle(shootSize / 2.0)
   if shootTouchId ~= 0 then
       UiTouchPressDefaultColorFilter()
       UiSetScreenTouchIdHandled(shootTouchId)
       UiSendInputScreenTouchAction("usetool", shootTouchId, 1)
   end
   ```

   The `~= 0` gate is a Lua-side convention, but the function it guards produces no effect
   with a nonexistent touch — consistent with the binding being a lookup.

So: touch input can be *mapped* to gameplay actions, and a real finger is always required.
It cannot create input out of nothing.

### 4.2 The key/value store as an input bridge

The store (`SetInt/GetInt/SetFloat/GetFloat/SetBool/GetBool/SetString/GetString/HasKey/ListKeys`)
is a real, shared string tree in process memory that mods can write. The engine reads it too.
I enumerated every dotted key in the binary and classified each input-vocabulary key by what
the engine does with the loaded value:

| Key | RVA | Class | Effect |
|---|---|---|---|
| `options.input.keymap.%s` | `0x98FFD0` | **remap table** | value is a *key name*; the engine resolves it to a physical input at lookup time. Changes **which key** drives an action. Does not create input. |
| `options.input.gamepad.bindings.%s.%s` | `0x990008` | **remap table** | value is `"%s %d %d"` = button name + 2 ints, written into the bindings table |
| `game.disableinput` | — | **suppression gate** | bool that only ever subtracts input; cleared to 0 in the same function |
| `game.player.disableinput` | — | **suppression gate** | as above |
| `game.updateinputmapping` | — | **one-shot re-resolve trigger** | causes the mapping tables to be re-read; the engine consumes and clears it |
| `options.input.headbob` | — | scalar param | read by `mods/tg/script/sonicboom.lua:169` |

**No key is a state source.** There is no store key whose value the input layer reads as the
current input value. The remap keys change the *name→action* table; the disable keys subtract.
Writing `options.input.keymap.forward = "q"` makes a real `q` keystroke drive `forward` — it
does not make `forward` true.

This is worth stating plainly because it is the most natural thing to try: **the store is a
sink for input state, not a source.** The engine *writes* 18 `game.player.*` / `game.vehicle.*`
keys *out of* live state every frame; nothing writes *into* it from the other direction.

### 4.3 Runtime data-file writes

Can a mod rewrite `input_settings.xml` (or the per-user `AppData\Local\Teardown\options.xml`)
and make the game reload it? **No.**

- **No file-write API exists in the corpus.** Zero hits for `io.open`, `io.write`, `io.popen`,
  `os.remove`, `os.rename`, `os.execute`, `WriteFile`, `SaveFile`, `SaveConfig`, `ExportFile`
  across all 1421 scripts. The 140 `io.` matches are substrings of asset names
  (`ui/…/io.ogg`, `io.menumusic`). `dofile`/`loadstring` (16/24 uses) only *read* `.lua` from
  the mod folder or eval `"return <expr>"` — no write-back.
- **The settings files are loaded by engine-init code, not by a Lua record.** The loaders for
  `data/input_settings.xml` (string at `0x9DF378`) and `config.xml` (`0x9A5958`) have no
  registration entry pointing at them from either Lua binding table.
- **The config reload is reachable per-frame but not from Lua.** The re-resolve path runs from
  the input driver every frame, gated on `game.updateinputmapping`. So a mod *can* force a
  remap to be re-read without a restart (see §5.1) — but that re-reads the store/tables, and
  no Lua primitive can put new bytes into `input_settings.xml` in the first place.

`Command()` (195 corpus call sites) reaches real engine namespaces, and the input-related ones
are all *reset* verbs: `options.input.gamepad.resettodefault`, `options.input.keymap.resettodefault`,
`options.input.gamepad.resetparams`, `game.steam.showbindingpanel`. None writes input.

---

## 5. What a mod *can* do (real, verified, and not input fabrication)

These are the only partial capabilities. Neither creates input; both are documented here so the
architecture decision is made on complete information.

### 5.1 Runtime rebinding of a physical key to an action — confirmed working

A mod can change which real key or button drives a gameplay action, with no restart:

```lua
SetString("options.input.keymap.forward", "q")
SetBool("game.updateinputmapping", true)   -- one-shot; the engine clears it
```

`game.updateinputmapping` is read at three sites in the input driver and **consumed and
cleared** (`store_set(key,0)`) after use. `mods/folkrace/main/script/main_ui.lua:739` uses
exactly this in `drawExpansionOptions()`.

The *reverse* direction — reading which key is bound to an action — is used by mods today:
`mods/splitfieldestate/mplib/inputactions.lua:118` does
`local registryKey = GetString("options.input.keymap." .. action)` and then feeds the resulting
**key name** into `InputPressed(...)`. So the store→input path mods use is a *read* path.

**What this is not:** a remapped key still requires a real hardware event. Zero shipped mods
write any `options.input.*` key — all 33 such writers are in 3 vanilla `data/` files. The
vanilla rebind UI (`data/ui/components/input_options_logic.lua:253`) is the only writer, and it
derives the value from `string.lower(InputLastPressedKey())`, i.e. from a real keypress:

```lua
GameInput.newKeyLogic = function(newKeyDialog)
    local key = string.lower(InputLastPressedKey())
    ...
        SetString(newKeyDialog.currentCallerInfo.actionName, key)
```

### 5.2 Mods can suppress and clear input

`SetBool("game.disableinput", true)`, `game.player.disableinput`, `DisablePlayerInput(p)`,
`InputClear()`, `InputResetOnTransition()`. All **subtract** input. `InputClear` makes the game
"forget" held input (`script_defs.luau:231`); it can make a held key read as released, and it
can never make an unheld key read as pressed.

### 5.3 The complete write-API inventory

Across the corpus, 155 distinct `Set*/Write*/Add*/Push*/Apply*/Force*` names are called
(8,781 call sites). 112 are engine-declared; **43 are script-local mod helpers**. The
engine-declared set contains **no input function at all** — it is physics (`SetBodyVelocity`,
`ApplyBodyImpulse`), rendering, camera, tool/player/vehicle state, and the store. The three
script-local names that could look suspicious are benign:

```lua
-- writes the VEHICLE's own control fields, not input
function SetVehicleControls(veh_drive, veh_steer, veh_target)
    vehicle.control.drive = veh_drive
    ...
end

-- only store booleans
function SetFakePause(bool)
    SetBool("hud.hide", bool); SetBool("game.paused", bool) ...
end

-- name is misleading: writes store colors
function WriteRegistryInfo(tr, path)
    SetColor(path .. ".pos", unpack(tr.pos))
end
```

---

## 6. Architectural conclusion

For a VR mod, the input layer is **read-only from Lua**. That makes the architecture decision
for you:

**There is no Lua path to drive input. VR input must be injected below the Lua VM.**

The concrete implication, from §2: the game reads controllers through Steamworks `ISteamInput`
obtained dynamically from `steam_api64.dll` and never calls XInput. So the injection point is
the **Steam Input layer / `steam_api64.dll`**, not a virtual XInput device, and not Lua.

The three realistic injection strategies, in order of how well the evidence supports them:

1. **Hook the `ISteamInput` vtable / its `GetDigitalActionData` / `GetAnalogActionData`
   results.** The action names are compile-time constants resolved at init through
   `GetDigitalActionHandle` / `GetAnalogActionHandle` / `GetActionSetHandle` (vtable slots
   `+0x80` / `+0xA0` / `+0x48`), in an init-time function with only two callers. Feeding
   values at the vtable return boundary gives you the full 51-action set, including the four
   axes a VR mod most wants: `camerax` (24), `cameray` (25), and the stick axes
   `l_stick_x/y` (47/48), `r_stick_x/y` (49/50).
2. **Write the state array directly.** It is a per-player block at `[lua+0x418] → +0xAC88 →
   +0xD0`, 51 slots × 8 bytes, each `{float value; u8 down; u8 pressed; u8 released}`, with
   the value array confirmed at object `+0xD60` sized `0x198`. Writing slot `id*8` sets the
   value the Lua readers return. Because the array is re-polled each frame, a write is a
   one-frame injection and must be re-applied per frame from a hook.
3. **The clone at `0x6D1D0`** copies the whole block and has 4 engine-frame callers, so it can
   restore a captured state. It cannot originate one — do not rely on it as a source.

A Lua mod remains the right tool for everything *downstream* of input: it can already read all
51 actions (`InputDown`/`InputPressed`/`InputValue`), suppress input, and rebind real keys at
runtime. It simply cannot originate a press.

---

## Appendix — reproducing this

Analysis scripts written for this task, in `/home/truenas_admin/teardown-analysis/uiver/`:

| Script | What it does |
|---|---|
| `tdlib.py` | RVA↔file-offset for the real (non-flat) PE section table, `.pdata` function bounds, string finder |
| `dis.py` / `disa.py` | disassembly windows; `disa.py` annotates rip-relative operands with resolved strings and callee extents |
| `table51.py` | dumps the 51-entry action name→id table at `0x9DEB50` |
| `callers.py` | `.text` scan for `E8 rel32` call sites, attributed to `.pdata` functions |
| `fnscan.py` | **function-bounded** scan (35,675 fns / 2.15M instructions) for all refs to the state array — the rigorous negative |
| `classify.py` | attributes record-vector accessors to the input system vs offset reuse |
| `readers.py` / `findreaders.py` | locate the native bindings of every reader via name-string → VA → table record |
| `touchrefs.py` | all functions touching `0xEF0..0xF14` |

**Method notes / corrections worth recording:**

- The PE is **not** a flat dump. `tdmap.py` shows `.text vaddr 0x1000, raw 0x400`,
  `.rdata vaddr 0x982000, raw 0x980C00` — so RVA ≠ file offset, and the `.dataa` section spans
  RVA `0xBF2000`..`0x1F97E8` against raw `0xBF0400`..`0x1C72400. Getting this wrong silently
  produces plausible-looking but wrong bytes.
- Registration records are stride `0x18` = `{nameptr(+0x00), pad, codeptr(+0x10)}`. Reading
  the code pointer at `+0x08` yields **0** and silently misattributes every binding.
- A single linear disassembly pass over `.text` desyncs and produces **false negatives** on
  write-site counts. All negatives in §3 come from the `.pdata`-bounded scan.
- A rip-relative displacement is not an address: `disp = target - (rva + instr_len)`.
- Use `strings -n 2`, not `-n 4`, or 3-char names are silently dropped.

**Game was never executed.** All findings are static.
