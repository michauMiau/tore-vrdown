# Mod loading: what is proven, what is still open

## Proven, this session

**The enable path.** The user enabled the mod through the game's own
Options → Mods screen. That works and is reproducible: the game rewrites
`modlists/1.xml` (list name `Default`) and inserts the id itself. So
`modlists/1.xml` is the enable list and `mods.xml` is only a "shown in menu"
record — all 100 shipped mods sit at `shown="false"` there and none of them
is enabled by it. `seltime`/`subtime` are real unix timestamps. We had been
editing the wrong file for the whole project.

**ReShade was one blocker, and it is gone.** `d3d12.dll`, `ReShade.ini` and
`ReShade.log` moved aside to `*.reshadebak`. The "press any key" splash no
longer appears.

**SteamVR was a second, bigger blocker.** The game kept raising a modal
"Oczekiwanie na połączenie bezprzewodowe przez SteamVR czeka na połączenie
z bezprzewodowymi goglami…" prompt that swallowed clicks. The cause was one
of **our own scheduled tasks**: `tdvr_steamvr` ran

```
cmd /c cd /d "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64"
  && start "vrserver" vrserver.exe && ... && start "vrmonitor" vrmonitor.exe
```

on a time trigger, relaunching the VR stack on every cycle. Disabling it
killed the prompt for good — `vrserver`/`vrmonitor` are now absent and the
prompt does not come back. `tdvr_steamvr` must stay **Disabled**; the other
`tdvr_*` tasks (`tdvr_go`, `tdvr_go2`) must stay **Enabled**, because
`fresh_game.ps1` launches the game through `tdvr_go` and disabling it breaks
the start entirely (`HRESULT 0x80041326`).

Note: `C:\Program Files\Revive\openvr_api64.dll` exists — Revive is present
as an OpenVR provider, separate from SteamVR.

**Input method.** `keybd_event` works for the main menu when sent as
`VK_RETURN` (13): CPU visibly steps (13.5 → 17.5 → 20.9 → 25.4 s) and the
screen moves off the menu. `mouse_event`/`SendInput` clicks are **ignored**
by the game even when the cursor is placed exactly on a measured menu
target. This is why clicking through the UI never worked.

## Wrong turns, recorded so they are not retried

- **`version = 2` in `info.txt` is not the gate.** Shipped `lasergun` has
  `version = 2`; ours had `version = 1`; I changed it. But shipped
  `screenrecorder` has **no** `version` field at all and is a real working
  script mod, so the field is not required. The change is harmless but it
  fixed nothing.
- **Directory name mismatch is not the gate either.** The game wrote
  `builtin-teardown-vr` (hyphen) while our folder is `teardown_vr`
  (underscore), so I created a `teardown-vr` copy and listed both ids. No
  effect.
- **The `lasergun` clone is no longer a clean control.** When the user
  enabled our mod they also enabled `vr_probe_clone`, so "the clone of a
  working shipped mod is also silent" no longer proves the game refuses all
  user mods.
- **`log.txt` is 0 bytes and `log_sentry.txt` is stale (18.09).** The game
  writes no log in this session, so every conclusion about whether a mod
  loaded has been indirect. `options.xml` exposes no verbose/debug switch —
  its full element list has nothing matching log/debug/verbose/trace/dev.

## Still open

Mod code still produces no observable effect after the mod is enabled and
the two modal prompts are gone. The probes still show nothing: the pure
`#version 2` green-text draw, the `io.open` trace, and the
`savegame.mod.vrprobe.*` store write.

The next decisive test is `builtin-screenrecorder`, now added to the
modlist. It is a shipped mod with a file-writing side effect, but it needs a
UI button press, which is the one thing the input layer cannot currently do.
Alternatives: get a mouse click accepted (real `SendInput` with correct DPI
awareness, or a `WM_LBUTTONDOWN` post to the window), or verify by
behaviour rather than pixels — e.g. enable `builtin-speedometer` and read a
speed value out of `savegame.xml`.

## Working scripts (all under build/)

- `seq.ps1` + `seq.txt` — data-driven UI step runner. Reads steps from
  `C:\tdvr\seq.txt` (`fx fy` or `key VK`), captures `seq_NN.png` after each.
  This indirection exists because `run_in_session1.ps1` collapses forwarded
  arguments into one string.
- `ui.ps1` + `ui_target.txt` — single click at a measured fraction.
- `boot.ps1` — the launch sequence (ReShade splash, SteamVR prompt, menu).
- `whatis.ps1`, `clickfrac.ps1`, `play.ps1`, `dismiss.ps1` — capture and
  input helpers.

PowerShell note: never put a double quote inside a single-quoted literal
that also builds XML — it breaks the parser (`The string is missing the
terminator`). Use `[char]34` for quotes, as in `rectest.ps1`.
