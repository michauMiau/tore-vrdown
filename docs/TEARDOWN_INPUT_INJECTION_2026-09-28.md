# Teardown input injection — measured results (2026-09-28)

**Verdict: `PostMessage(WM_MOUSEMOVE + WM_LBUTTONDOWN + WM_LBUTTONUP)` is the working
mouse path, and it is cursor-independent.** Use it. It beats `keybd_event`/`mouse_event`
because it needs no real cursor, no foreground activation, and no timing.

All results below were measured on the live VM against a running `teardown.exe`
(Polish UI, window 1722x999, client 1706x960) in session 1. Acceptance was
**OCR of the resulting screen**, not a counter and not a pixel diff.

---

## 0. The prior conclusion was wrong, and why

Earlier work concluded "mouse_event and SendInput clicks are IGNORED by the game".
That is **refuted**. The real story:

- The screen those clicks were aimed at was the **"UWAGA / Naciśnij dowolny klawisz"**
  photosensitivity splash, not the main menu. A splash that says *press any key*
  cannot be dismissed by a click, so every click test correctly reported "ignored".
- The machine has **two displays**: `DISPLAY1` primary 1920x1080 at (0,0), and
  `DISPLAY2` at **(-1050,0) 1050x1680**. The cursor was parked at x=-222/-500,
  i.e. **on the secondary monitor, off the game**. Any cursor-position-dependent
  click was aimed at nothing.

Both are measurement errors, not game behaviour. Fix the screen state before
blaming the target.

---

## 1. What the game reads (static, from its own WndProc)

Decompiled `teardown.exe` WndProc at RVA `0x4F0250`:

| Input | Layer | Injection that works |
|---|---|---|
| Keyboard | `WM_KEYDOWN/WM_KEYUP/WM_CHAR` (wParam = VK) | `keybd_event`, `SendInput`, `PostMessage` |
| Mouse **movement** | **Raw Input** (`WM_INPUT`, mouse-only; keyboard Raw Input discarded) | only a real cursor move |
| Mouse **buttons** | `WM_LBUTTONDOWN/UP`, `WM_RBUTTON*`, `WM_MBUTTON*`, `WM_XBUTTON*`, `WM_MOUSEWHEEL` | `PostMessage` (best), `SendInput`, `mouse_event` |
| Gamepad | `ISteamInput` / `SteamInput006` via `steam_api64.dll` | virtual HID pad, or Steam Input |

**No** DirectInput, XInput, HID, SDL or GameInput — verified against the full
import table plus all dynamic `LoadLibrary` sites. This is why a ViGEmBus
XInput virtual pad cannot work: the game imports no XInput at all. It wants
**HID**.

---

## 2. Measured matrix (each: reset to menu, one method, then OCR the screen)

| # | Method | Result |
|---|---|---|
| 1 | `keybd_event(VK_RETURN)` | **WORKS** — cleared the splash, advanced the menu |
| 2 | `keybd_event(VK_SPACE/ESC/arrows)` | works (dismiss/back) |
| 3 | `PostMessage WM_KEYDOWN/WM_KEYUP` | works |
| 4 | `SendMessage WM_KEYDOWN` | works (but synchronous — use sparingly) |
| 5 | `mouse_event` LDOWN/LUP with cursor on target | **WORKS** — opened OPCJE |
| 6 | **`PostMessage` WM_MOUSEMOVE + LBUTTONDOWN + LBUTTONUP** | **WORKS — best method** |
| 7 | `PostMessage` LBUTTONDOWN/UP without the move | works (move is optional) |
| 8 | `SendInput` MOUSEEVENTF_ABSOLUTE move + click | **FAILS — menu unchanged** |
| 9 | `SendInput` MOUSEEVENTF_MOVE (relative) | **no cursor movement; crashed the game once** (crashpad dump appeared) |
| 10 | `PostMessage` WM_LBUTTONDBLCLK | **FAILS — menu unchanged** |

### The one result that settles it

`PostMessage` click on **Opcje** with the cursor deliberately parked at
**(-500, 500)** — off the primary monitor, nowhere near the game:

```
[PostMessage click Opcje, cursor parked offscreen] before=916 after=0  cursor=-500,500
   -> screen OCR: "OPCJE  Ekran Grafika Dzwiek Rozgrywka Sterowanie  Ekran MSI G24C4"
[PostMessage click Graj, cursor parked offscreen]  before=911 after=942 cursor=-500,500
   -> screen OCR: "Graj ... Kampania Sandbox Wyzwania Rozszerzenia Menedżer modów"
```

Both opened, with the cursor nowhere near the target. **`PostMessage` is
cursor-independent; `mouse_event` is not.** That is the entire difference
between rows 5/6 and the "clicks are ignored" conclusion.

---

## 3. The exact working call

```powershell
$r = New-Object U+RECT; [void][U]::GetClientRect($h,[ref]$r)   # CLIENT, not window
$cx = [int](($r.R-$r.L)*$fx); $cy = [int](($r.B-$r.T)*$fy)      # client fractions
$lp = [IntPtr](($cy -band 0xFFFF) -shl 16 -bor ($cx -band 0xFFFF))
[void][U]::PostMessageW($h,0x0200,[IntPtr]0,$lp)   # WM_MOUSEMOVE
Start-Sleep -Milliseconds 120
[void][U]::PostMessageW($h,0x0201,[IntPtr]1,$lp)   # WM_LBUTTONDOWN
Start-Sleep -Milliseconds 120
[void][U]::PostMessageW($h,0x0202,[IntPtr]0,$lp)   # WM_LBUTTONUP
```

Measured menu-button client fractions (client 1706x960):

| Button | fx | fy |
|---|---|---|
| Graj | 0.264 | 0.082 |
| Gra wieloosobowa | 0.476 | 0.082 |
| Postać | 0.620 | 0.082 |
| Opcje | 0.710 | 0.082 |
| Wyjdź | 0.910 | 0.082 |

Notes:
- `lParam` must be **client**-relative packed coords (`HIWORD=y, LOWORD=x`).
- ~120 ms between messages. Much less and the game coalesces the click.
- `WM_LBUTTONDBLCLK` (0x203) does **not** substitute for down+up.
- No `SetForegroundWindow`/`AttachThreadInput` needed for `PostMessage`, but
  focus is still needed for **`keybd_event`** (it goes to the focused window).

---

## 4. Pitfalls that cost time, all measured

1. **Never use CPU time as the acceptance signal here.** Baseline rendering
   burns ~1.0-1.7 s of CPU per 1.6 s sample *with no input at all*, so every
   method looks like "it worked". A rising counter proves nothing.
2. **A full-frame pixel diff is worthless**: the menu background is an animated
   scene, so consecutive frames differ by 100k+ pixels on the header strip
   alone. Diff only the fixed menu strip, and confirm with OCR.
3. **Byte-identical captures mean a splash or a modal, not a frozen game.**
   The "UWAGA" splash gives two identical frames 3 s apart while the process
   burns CPU.
4. **`MainWindowHandle` is unreliable** — it returns `0` or a stale handle
   during scene transitions. Enumerate `EnumWindows` for the pid and take the
   largest **visible** window with a non-zero rect.
5. **A callback runs in its own scope.** Inside an `EnumWindows` delegate, plain
   function locals assigned in the callback are discarded — use `$script:`.
   Getting this wrong made `GetGameWindow` silently return a zero rect and the
   whole harness appear to hang with no error.
6. **`Add-Type` is fine to re-run**; duplicate class names do not throw. Two
   "the harness silently dies" bugs turned out to be scope bugs, not Add-Type.
7. The box is **not headless and not a VM** — it has an NVIDIA GPU and a real
   controller. The hostname `vm` is meaningless.
8. Game-restart noise: the window handle changes across scene transitions
   (0x520bb0 -> 0x4d0ba8 -> 0x3000abe -> 0x940a6a across one session). Re-resolve
   it every run; never cache it.

---

## 5. Still unproven / open

- **Gamepad.** Game reads `ISteamInput`, no XInput. A ViGEm **X360** virtual pad
  cannot work; a virtual **HID/DS4** pad (ViGEm `vigem_target_ds4_alloc`, or
  `vgamepad`) should. Not tested — ViGEmBus is not installed
  (`vigembus present: False`).
- **No debug console exists.** `AllocConsole` has zero references in the exe and
  there is no `-console`/`-dev` flag. Real flags are `-gfxapi -windowed
  -devicetype{retail|debug|instrumented} -framecapturer -gpuvalidation -nolog
  -logfile -fulldump`. Dear ImGui **is** compiled in (143 strings incl. "Dear
  ImGui Metrics/Debugger") but the gate boolean was not located — unproven.
- **`UiSendInputScreenTouchAction`**: a native Lua binding (see
  `teardown-analysis/UISEND_INPUT_SCREEN_TOUCH_ACTION.md`) that writes a UI
  action with a caller-supplied action name and an unvalidated touch id. That is
  a far better injection primitive than synthetic Win32 input if a mod host is
  available. Static-read only, never executed.
