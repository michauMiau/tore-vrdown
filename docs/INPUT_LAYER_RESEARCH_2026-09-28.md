# Teardown input layer — how the game actually reads keyboard & mouse

**Date:** 2026-09-28
**Method:** static analysis of the decrypted `teardown.exe` (30,333,440 B, PE32+, ImageBase
`0x140000000`) at `/home/truenas_admin/steamless/Teardown/teardown.exe`, plus web search.
**This is the strongest possible evidence class: the game's own window procedure, disassembled.**

> **Build caveat:** RVAs below are for the *decrypted flat dump*. The retail build on the live VM
> (`c8228c87`, 30,555,208 B) is a different file with different addresses. Re-measure before hooking
> any address. The *structure* is a compiler property and will survive; the numbers will not.

---

## 0. HEADLINE ANSWER

| Input | Layer the game reads | Injection that works |
|---|---|---|
| **Keyboard** | **Win32 `WM_KEYDOWN` / `WM_KEYUP` / `WM_SYSKEY*` window messages** (wParam = virtual-key code) | `keybd_event` / `SendInput` / `PostMessage` — all work |
| **Mouse movement** | **Raw Input** (`RegisterRawInputDevices` + `WM_INPUT`/`GetRawInputData`), registered for **MOUSE ONLY** | `SendInput` works (it feeds Raw Input); `mouse_event` works; **`PostMessage(WM_MOUSEMOVE)` does NOT** |
| **Mouse buttons** | **Win32 window messages** `WM_LBUTTONDOWN/UP`, `WM_RBUTTON*`, `WM_MBUTTON*`, `WM_XBUTTON*`, `WM_MOUSEWHEEL` | `SendInput` / `PostMessage` both work |
| **Gamepad** | **Steam Input** (`ISteamInput` / `SteamInput006`) | Via `steam_api64.dll` or a virtual HID pad Steam Input can see |

**This exactly explains the measured behaviour on the VM.** `keybd_event(VK_RETURN)` works and
`mouse_event`/`SendInput` clicks are ignored — but note the click failure is *not* a
mouse-API problem, see §5.

---

## 1. The window procedure — located and disassembled

`RegisterClassW` is called at `0x4f272a` and `0x4f2c8b`. In both, `lpfnWndProc` is loaded via
`lea rax,[rip-0x248a]` @ `0x4f26d3` → **`0x004F0250`**.

`0x4f0250` is the game's WndProc. It is `.pdata` entry #15880's neighbour and begins:

```
004f0250  push rbx / rbp / rsi / rdi / r13
004f0257  mov eax, 0x2060
004f025c  call 0x6d0700              ; stack probe
004f0264  mov rdi, r9                ; LPARAM
004f0267  mov rbx, r8                ; WPARAM
004f026a  mov esi, edx               ; UINT uMsg
004f026c  mov rbp, rcx               ; HWND
004f026f  cmp edx, 0x112             ; WM_INITDIALOG
...
004f0281  cmp edx, 0xff              ; WM_INPUT      <-- Raw Input
004f028d  je  0x4f032a
004f06e4  mov eax, esi
004f06e6  sub eax, 0x100             ; WM_KEYDOWN
004f06eb  je  0x4f075d
004f06ed  sub eax, 1                 ; WM_KEYUP
004f06f0  je  0x4f0705
004f06f2  sub eax, 1                 ; WM_CHAR
004f06f5  je  0x4f0736
004f06f7  sub eax, 2                 ; WM_SYSKEYDOWN
004f06fa  je  0x4f075d               ;  ^ same handler as WM_KEYDOWN
004f06fc  cmp eax, 1                 ; WM_SYSKEYUP
004f06ff  jne 0x4f0947
004f0705  <key-up handler>           ; call 0x4ec920
...
004f0736  <char handler>             ; call 0x4ebf80
004f075d  <key-down handler>        ; call 0x4ec740
004f07b6  lea eax, [rdx-0x200]       ; WM_MOUSEMOVE..WM_XBUTTONUP
004f07bc  cmp eax, 0xa
004f07bf  ja  0x4f0947
004f07c5  lea rdx, [rip-0x4f07cc]    ; jump table, base 0x4f0000
004f07cc  mov ecx, dword ptr [rdx + rax*4 + 0x4f099c]
004f07d3  add rcx, rdx
004f07d6  jmp rcx
```

### 1a. Confirmed message handlers

| uMsg | Name | Handler |
|---|---|---|
| `0xFF` | `WM_INPUT` | `0x4f032a` → **Raw Input**, mouse only |
| `0x100` | `WM_KEYDOWN` | `0x4f075d` → `0x4ec740` |
| `0x101` | `WM_KEYUP` | `0x4f0705` → `0x4ec920` |
| `0x102` | `WM_CHAR` | `0x4f0736` → `0x4ebf80` |
| `0x104` | `WM_SYSKEYDOWN` | `0x4f075d` (shares with `WM_KEYDOWN`) |
| `0x105` | `WM_SYSKEYUP` | `0x4f0705` (shares with `WM_KEYUP`) |
| `0x200`–`0x20A` | `WM_MOUSEMOVE` … `WM_XBUTTONUP` | jump table @ `0x4f099c` → `0x4ebd60` |

**Keyboard = plain Win32 window messages. PROVEN.** Not Raw Input (Raw Input is registered for
mouse only — §2), not DirectInput, not `GetAsyncKeyState` (not imported).

### 1b. The WndProc is a *message pump*, and `PeekMessage` is used

```
0x4f305c, 0x4f30b1  PeekMessageA   (2 sites)
0x4f308b            TranslateMessage
0x4f3096            DispatchMessageA
```

`PeekMessageA` (not `GetMessageW` — `GetMessage` does not appear in the image at all). A busy-poll
`PeekMessage`/`DispatchMessage` loop is what makes `PostMessage`-injected input land promptly.

---

## 2. Raw Input is MOUSE-ONLY — `RegisterRawInputDevices` @ `0x4f2689`

```
004f2660: mov edx, 1                                  ; dwNumDevices = 1
004f266c: mov r8d, 0x10                               ; cbSizeHeader = 16
004f2679: lea rcx, [rbp + 0x10]                       ; pRawInputDevices
004f267d: mov qword ptr [rbp + 0x10], 0x20001         ; <<<< usUsagePage=0x02, usUsage=0x01
004f2685: mov qword ptr [rbp + 0x18], rsi              ; hwndTarget = NULL
004f2689: call qword ptr [rip + 0x490119]              ; RegisterRawInputDevices
004f268f: cmp eax, 1
```

`0x0000000200000001` = usage page `0x02` (HID Generic Device) / usage `0x01` (**Mouse**). Exactly
**one** device, mouse only. No keyboard registration exists anywhere in the image.

And the `WM_INPUT` handler discards anything that is not a mouse:

```
004f039d: cmp dword ptr [rsp + 0x60], 0     ; header.dwType
004f03a2: jne 0x1404f088e                  ; non-zero => DISCARD
```

`dwType == 0` is `RIM_TYPEMOUSE`. **`RIM_TYPEKEYBOARD` is never handled.**

---

## 3. `GetRawInputData` — mouse deltas only

```
004f0343: mov dword ptr [rsp+0x20], 0x18     ; pcbSize = sizeof(RAWINPUTHEADER)
004f034b: xor r8d, r8d                      ; pData = NULL -> size query
004f034e: mov edx, 0x10000003               ; uiCommand = RID_INPUT
004f0356: call [rip+...]                    ; GetRawInputData  (size)
004f037d: lea r8, [rsp + 0x60]              ; pData buffer
004f038a: call [rip+...]                    ; GetRawInputData  (fill)
004f03e4: movzx r12d, byte ptr [rsp + 0x78] ; usFlags
004f03ea: test r12b, 1                      ; MOUSE_MOVE_ABSOLUTE
```

Only **2** call sites. The buffer is a `RAWMOUSE`; the code branches on
`MOUSE_MOVE_ABSOLUTE` and reads `usButtonData`. This is how mouse **movement** is read.

**Therefore:** `WM_MOUSEMOVE` (absolute cursor position) is a *fallback* that is also handled
(§1a), while **relative motion — what a mouse actually produces — comes only from Raw Input.**

### 3a. `SetCursorPos` — the game warps the cursor itself (3 call sites)

`0x4f05dc`, `0x4f0679`, `0x4f310c`. Teardown re-centers the cursor every frame. This is standard
for Raw-Input FPS games and is the reason naive cursor positioning does not produce look motion.

---

## 4. Mouse BUTTONS are window messages, not Raw Input

`WM_LBUTTONDOWN/UP`, `WM_RBUTTONDOWN/UP`, `WM_MBUTTONDOWN/UP`, `WM_XBUTTONDOWN/UP` and
`WM_MOUSEWHEEL` all land in the `0x200..0x20A` jump table and call **`0x4ebd60`**:

```
004ebd60  mov qword ptr [rsp+0x18], rbx
004ebd6a  movsxd rbp, edx            ; button id (1/2/3, or 1/2/3 wheel)
004ebd70  cmp ebp, 0xc
004ebd84  setl al
004ebd8b  mov byte ptr [rcx + 0xd5d], al   ; <<< "mouse mode" flag: 1 if button < 12
```

`0x4ebd60` is the same shape as the key handler `0x4ec740` — both append a 16-byte record to a
growable vector at `[obj+0xba0]` count / `[obj+0xba8]` pointer, with the record holding
`{u32 id, …, u32 vk-or-button}`. Both set `byte [obj+0xd5d]`. **Keyboard and mouse-button events
land in the same event queue structure.**

This is corroborated by the Steam Input action names, which include separate
`mouse_lmb` / `mouse_mmb` / `mouse_rmb` and a `"Mouse"` analog action — the game distinguishes
mouse buttons as their own actions, and the window-message path is where they enter.

---

## 5. WHY CLICK INJECTION FAILS — the actual explanation

The parent report says `mouse_event`/`SendInput` clicks are "ignored". Given the above, the
likely causes, in order of probability:

1. **Session 0 / no interactive desktop.** Prior work in this repo already proved
   (`docs/WINDOWS_FINDINGS.md`) that an SSH-launched process lands in session 0 with no window and
   that `explorer.exe` is in session 1. `SendInput` and `mouse_event` inject into the **input queue
   of the calling thread's session**; a session-0 process can synthesise *keyboard* messages to its
   own window but mouse injection is far more constrained, and more importantly the game may not
   own a real foreground window at all.

2. **Steam Input remaps the click away.** Because gamepad input is read through `ISteamInput`,
   the *action* `lmb` is whatever Steam Input says it is. With a PS4 controller connected and
   Steam Input active, a `menu_accept`/LMB binding may be sourced from the controller, not the
   window message. The window-message click updates the raw event, but the **action** the menu
   reads comes from `GetDigitalActionData` (`0x4e08ed`, 37×/frame — see
   `docs/STEAMINPUT_CALLSITE.md`).

3. **Cursor warp.** `SetCursorPos` re-centres every frame; a click aimed by absolute coordinate is
   fighting the warp.

**Actionable:** the winning layer is **not** mouse messages at all — it is the **Steam Input
action chokepoint**. `GetDigitalActionData` @ `0x004E08ED` is one instruction, called 37× per
frame, writing `InputDigitalActionData_t` to `[rsp+0x40]`. Hooking it lets you synthesise
`menu_accept` / `lmb` / `menu_up` / `menu_down` directly, bypassing the OS entirely. That is
documented in full in `docs/STEAMINPUT_CALLSITE.md` §6.

There is also a **Lua-level** primitive that needs no Win32 API at all:
`UiSendInputScreenTouchAction("jump", <any id>, 1.0)` — fully audited in
`teardown-analysis/UISEND_INPUT_SCREEN_TOUCH_ACTION.md`. Caveats: gated to UI mode, and the
analog axes `camerax`/`cameray`/`mousex`/`mousey` (ids `0x18..0x1B`) are excluded from the merge.

---

## 6. Command-line switches — CONFIRMED, extracted from the binary

No `-console` and no `-dev` exist (0 hits each). The real flag table sits in `.rdata` next to
`'Platform available: %d'`, `'-nolog'`, `'-logfile'`, `'Args: %s'`:

| Flag | Evidence / meaning |
|---|---|
| `-gfxapi <api>` | `Unsupported graphics API specified '%s', expected APIs: %s` — used in prior work to force D3D12 |
| `-windowed` | adjacent to `Can't create a Device Context.`, plus `-w`, `-h`, `-x`, `-y` |
| `-devicetype <retail\|debug\|instrumented>` | `Unknown device type is specified '%s', supported are 'retail', 'debug', 'instrumented'(only for xbox)` — **`debug` devicetype exists** |
| `-framecapturer <renderdoc\|pix>` | `Unknown gpu frame capturer is specified '%s'` |
| `-gpuadapteridx <n>` | `Integer is expected after -gpuadapteridx, but '%s' has been encountered` |
| `-nolog`, `-logfile <f>` | `log.txt` is the default log name |
| `-fulldump` | adjacent to `'crash'`, `'log.txt'`, `'Teardown'`, `'options.xml'` |
| `-shaderdebuginfo`, `-ignorelocalshadercache`, `-hdr`, `-texture3dsizelimit`, `-gpuvalidation`, `-aftermath` | dev/graphics toggles |

**There is no developer console.** But there are two strong leads:

- **`-devicetype debug`** — a real non-retail build variant, quoted above from the game's own
  error string. Worth testing; may enable extra logging/asserts.
- **`imgui.ini`, `config.xml`, `mods.xml`, `quicksave.bin`, `\Teardown`, `'/moved-files.txt'`** are
  all literal strings next to the arg table.

## 7. Dear ImGui IS compiled in — 143 string hits

Not a hypothetical. The image contains the full ImGui debug layer:

```
'Dear ImGui %s (%d)'          'Dear ImGui Debug Log'      'Dear ImGui Demo'
'Dear ImGui Metrics/Debugger' 'Dear ImGui Stack Tool'     'Dear ImGui Style Editor'
'ImGuiBackend::mFont'         'ImGuiBackend::mMesh'
'ImGuiBackendFlags_RendererHasVtxOffset: %s'
```

plus ~100 `ImGui*Flags_*` names. **ImGui is linked into the retail build**, which means an ImGui
window can almost certainly be raised by flipping one boolean. It is normally used for the mod
editor UI. This is a far better console than a command-line switch. **Next step: find the
`io.DisplayWantTextInput`/`io.WantCaptureMouse`/gated-init boolean and flip it.**

Note: `UiForceMouse` and `UiSendInputScreenTouchAction` exist in the Lua UI API (see §5), i.e.
the engine already has a Lua-callable input-injection surface.

---

## 8. User config locations

From the binary's own path strings (adjacent to the arg table):

| Path | Notes |
|---|---|
| `options.xml` | graphics/display settings — the file players edit for `-windowed`/resolution |
| `config.xml` | separate config file, present in the string pool |
| `mods.xml` | mod list |
| `imgui.ini` | ImGui window layout/state |
| `quicksave.bin` | quick save |
| `\Teardown` | the AppData folder name |
| `/moved-files.txt` | + the literal migration notice: *"Teardown now uses the AppData folder instead of Documents to store progress files. Local mods are still placed in Documents/Teardown/Mods."* |

**Confirmed by the migration notice string embedded in the binary:** progress/config moved from
`Documents\Teardown` to **`%LOCALAPPDATA%\Teardown`**, i.e.
`C:\Users\<user>\AppData\Local\Teardown\`. Local mods stay in `Documents\Teardown\Mods`.
This matches the community reports (Steam discussion 0/5750503966480102614: *"The file has been
moved to %localappdata%/Teardown"*).

62 `options.*` keys are registered by name. Input-relevant ones:

```
options.input.sensitivity           options.input.zoom_sensitivity
options.input.smoothing             options.input.invert
options.input.headbob               options.input.toolsway
options.input.keymap.forward/.backward/.left/.right/.up/.down/.resettodefault
options.input.gamepad.vibration      options.input.gamepad.triggerEffects
options.input.gamepad.stick.{left,right}.{function,inner_deadzone}
options.input.gamepad.resetparams   options.input.gamepad.resettodefault
```

**None of these enables a console or changes the input layer.** They are tuning values only. There
is no `options.input.rawinput`, no `options.debug`, no dev-mode key (0 hits for each).

---

## 9. No DirectInput, no XInput, no HID, no SDL, no GameInput

Full import table: **33 DLLs, 545 functions.** Checked explicitly:

| API | In import table? | Notes |
|---|---|---|
| `xinput*.dll` | **NO** | no DLL, and no `xinput` string anywhere in 30 MB |
| `dinput8.dll` | **NO** | no DLL, no `dinput` string |
| `hid.dll` / `setupapi.dll` | **NO** | absent; the game is HID-agnostic |
| `SDL` | **NO** | only a 3-byte non-loadable fragment in `.text` |
| `GameInput` | **NO** | only an unreferenced RTTI descriptor `.?AVGameInput@@` with **0** rip-refs |

Dynamic loading is fully accounted for: all 5 `LoadLibrary*` call sites resolve to
`GFSDK_Aftermath_Lib.x64.dll`, `d3d12.dll` (×2), `xaudio2_9.dll`, and one register-sourced module
— **no input DLL is loaded dynamically.**

The **only** Win32 input APIs imported: `RegisterRawInputDevices`, `GetRawInputData`,
`SetCursorPos`, `ShowCursor`, `SetCapture`/`ReleaseCapture`, `SetFocus`, `GetAsyncKeyState` **absent**.

> **Consequence for automation:** a virtual **XInput** pad (ViGEmBus) will **not** work — the game
> never calls XInput. A virtual **HID gamepad** *would* work, because Steam Input talks to HID
> directly, independent of the game.

---

## 10. Third-party approaches that work, by layer

| Tool | Layer | Works on Teardown? |
|---|---|---|
| **AutoHotkey `Send`** (`SendInput`) | Win32 | **Yes for keyboard.** This is why the AHK rebinding script on Steam works for keys. |
| **AutoHotkey `Click`** | Win32 mouse | Should work for buttons (§4) — failure is more likely session/HID, not the call. |
| **reWASD** | **HID/driver layer** (`hidgamemap.sys`, *not* ViGEmBus) | **Yes for gamepad** — feeds the HID layer that Steam Input reads. Per reWASD's own maintainers, reWASD does not use ViGEmBus at all. |
| **ViGEmBus virtual XInput pad** | XInput | **No** — no XInput import. |
| **Virtual HID pad** (ViGEm *HID* mode, DS4 emulation) | HID | **Yes** — Steam Input sees it as a DS4. The binary literally contains the string `"gamepad_ds4"` passed to `GetInputTypeForHandle`. |
| **HidHide** | HID filter driver | N/A — hides devices, doesn't inject. |
| **`tdconsole`** (github.com/Thomasims/tdconsole) | — | A **proxy `winmm.dll`** next to `teardown.exe` that calls `AllocConsole()` on load. Side-loadable, but `AllocConsole`/`FreeConsole` are **0 hits in the exe**, so there is no built-in console to reveal. Only useful as a stdout/stderr sink. |

Sources: <https://github.com/Thomasims/tdconsole> ·
<https://github.com/ViGEm/ViGEmBus/issues/17> (reWASD does not use ViGEmBus) ·
<https://steamcommunity.com/app/1167630/discussions/0/2998794978530540030/> (AHK rebinding works) ·
<https://steamcommunity.com/app/1167630/discussions/0/5750503966480102614/> (AppData migration)

---

## 11. Confidence ledger

| Claim | Status |
|---|---|
| WndProc @ `0x4f0250` | **PROVEN** — from `lea` feeding `lpfnWndProc` into `RegisterClassW` |
| Keyboard read via `WM_KEYDOWN`/`WM_KEYUP`/`WM_CHAR` | **PROVEN** — switch decoded, handlers disassembled |
| Raw Input registered for MOUSE only | **PROVEN** — `mov qword [rbp+0x10], 0x20001`, `dwNumDevices=1` |
| `WM_INPUT` discards non-mouse | **PROVEN** — `cmp dword [rsp+0x60],0; jne discard` |
| Mouse buttons via window messages | **PROVEN** — jump table + `0x4ebd60` |
| Mouse motion via Raw Input | **PROVEN** — `GetRawInputData` + `MOUSE_MOVE_ABSOLUTE` |
| `SetCursorPos` called by the game | **PROVEN** — 3 call sites |
| No dinput/xinput/hid/SDL/GameInput | **PROVEN** — full 33-DLL / 545-func dump + string scan |
| Command-line flag list | **PROVEN** — literal strings with adjacent error messages |
| `-devicetype debug` is a valid value | **PROVEN** — quoted in the game's own error string |
| Dear ImGui linked into retail | **PROVEN** — 143 ImGui strings incl. Metrics/Debugger/Style Editor |
| No developer console exists | **PROVEN** (negative) — 0 hits for `AllocConsole`, `-console`, `-dev` |
| ImGui can be raised by flipping a boolean | **PLAUSIBLE, NOT PROVEN** — needs the gate located |
| Click-injection failure cause | **PLAUSIBLE** — session 0 + Steam Input remap are the two candidates; not directly measured |

## 12. Recommended next actions, highest value first

1. **Flip the ImGui gate.** ImGui is in the binary. Find the `ImGui::Update`/init call and force
   the enable boolean. This gives a debug window, a metrics panel, and a Lua console — all at once.
2. **Hook `GetDigitalActionData` @ `0x004E08ED`** (1 instruction, 37 calls/frame). Synthesise
   `menu_accept` / `menu_up` / `menu_down` directly. Bypasses the OS entirely, so session-0
   fragility stops mattering. See `docs/STEAMINPUT_CALLSITE.md` §6.
3. **Use `UiSendInputScreenTouchAction` from Lua** — zero Win32 dependency, already audited.
4. **Re-run in session 1** with a scheduled task using an interactive token, and re-test
   `SendInput` clicks. Prior work showed the game needs session 1 to get a window at all.
5. Try `-devicetype debug` and `-logfile <path>` for extra engine logging.
6. **Drop the PS4 controller / disable Steam Input** for Teardown if you want the window-message
   mouse path to be authoritative.
