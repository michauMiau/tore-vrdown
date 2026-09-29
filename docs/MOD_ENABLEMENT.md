# Enabling the mod in-game, and the remaining blocker

The user enabled the mod through the game's own Options → Mods screen. That
works and is reproducible: after doing it, the game **rewrites**
`modlists/1.xml` and inserts the id itself.

## What enabling the mod actually changed

`modlists/1.xml` (list name `Default`) before and after, as the game left it:

```xml
<mod id="teardown_vr"/>          <- our earlier hand edit, still there
<mod id="steam-2515871265"/>
...
<mod id="vr_probe_clone"/>       <- our clone, still there
<mod id="builtin-vr_probe_clone"/>
<mod id="builtin-teardown-vr"/>  <- ADDED BY THE GAME, the id the menu wrote
<mod id="builtin-teardown_vr"/>  <- our hand-written duplicate
```

So the enable path is confirmed: **the menu writes to `modlists/1.xml`**, not to
`mods.xml`. `mods.xml` only records what is *shown* in the menu — all 100
shipped mods sit at `shown="false"` there and none of them are enabled by it.
`seltime`/`subtime` are real unix timestamps ("last selected in menu"), not
flags. We had been editing the wrong file for the whole project.

## The mod still does not run

Verified four ways, with the mod enabled through the game's own menu:

| probe | observable | result |
|---|---|---|
| `lua_pure.lua` — 427 B, `#version 2`, `server.init`/`server.tick`/`client.draw`, `UiText("PURE-V2-LOADED")` | green pixels in the capture | 14 px, clustered at (768–773, 525–530) — a letter in the logo, not a 60 px text draw |
| `lua_writetest2.lua` | `io.open` trace file | not created |
| `lua_nofile.lua` | `savegame.mod.vrprobe.*` in `savegame.xml` | absent — the store the game itself persists never changed |
| clone of shipped `lasergun` with only `main.lua` replaced | same probes | also nothing |

`io.open` being sandboxed would explain the trace file, but **not** the store
probe: `SetInt`/`SetBool` into `savegame.mod.*` is the game's own mechanism
and cannot be blocked by the mod sandbox. Combined with the `lasergun` clone
failing identically, the conclusion is that mod code is not being executed at
all, not that a particular API is unavailable.

## A second modal prompt, found by reading the screen

Installing tesseract (`apt-get install tesseract-ocr`, 5.5.0) made the captures
readable, and that immediately changed the picture. OCR of the live window
showed a second blocking screen after the ReShade splash:

> **Oczekiwanie na połączenie bezprzewodowe przez SteamVR czeka na
> połączenie z bezprzewodowymi goglami…**

A click in the centre of the window dismisses it. **SPACE, ENTER, ESC and
letter keys do not** — verified by capturing after each and OCR-ing: the
prompt text is present in all four key cases and absent only in the click case.
`keybd_event` moves the main-menu cursor but does not activate items; for this
prompt only a real mouse click works.

`build/boot.ps1` encodes the correct sequence: space (ReShade) → click centre
(SteamVR prompt) → space.

## Still open

The game now reaches a live, animating main menu (133,438 distinct colours in
the capture, versus 676 for the original dead-window captures), and the mod is
enabled by the game's own menu, and it still does not run. The next thing to
check is whether script mods load at all in this build — the decisive test is
enabling a *shipped* script mod through the same menu (e.g. `lasergun`,
`speedometer`) and seeing whether its own output appears. If a shipped mod also
stays silent, the problem is the game/session, not our mod.
