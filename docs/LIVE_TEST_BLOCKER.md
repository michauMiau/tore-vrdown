# The live-game blocker was ReShade, not the mod system

**Every "the Lua mod does not load" conclusion in this project was wrong, and
the reason was a screenshot.** Three separate mods, three game restarts, and
all of them produced a byte-identical 67,562-byte capture. That is not a mod
failing to draw — that is a placeholder surface being photographed.

## What the window actually was

`PrintWindow` and `CopyFromScreen` both returned the game's window rect
(88,29 .. 1592,998, client 1488x930, `WS_VISIBLE`, not iconic) — and showed a
ReShade splash:

> ReShade 6.8.0 — *Odwiedź strong https://reshade.me…*
> **ReShade zostało zainstalowane! Naciśnij klawisz 'Home', aby rozpocząć samouczek.**
> W erze występują migotające światła… Oczekiwanie SteamVR czeka… **Naciśnij dowolny klawisz, kontynuować**

The game was parked on a **modal "press any key" prompt** and had never
reached the mod system at all. `mods.xml`, `shown="true"`, the canary, the
touch probe — none of it had been given a chance to run.

Read with tesseract 5.5.0; the earlier ASCII rendering of the same pixels was
unreadable and vision was unavailable, which is why this went unnoticed.

## The measurement that settled it

Capturing the window four times, 4 s apart, while watching CPU:

```
sample 1  cpu=11.47s  bytes=83590
sample 2  cpu=11.73s  bytes=83590
sample 3  cpu=11.95s  bytes=83590
sample 4  cpu=12.19s  bytes=83590
window pixels differing across 12s: 0 / 1457376
VERDICT: THE WINDOW IS STATIC. Not a live render.
```

**Zero** differing pixels out of 1,457,376, with CPU climbing the whole time. A
game that is rendering cannot produce a static window. After dismissing the
splash, the same measurement returned:

```
window pixels differing across 12s: 3614001 / 1457376
VERDICT: the window content is changing.
```

## Input on this VM: only one mechanism works

| mechanism | reaches the game? | evidence |
|---|---|---|
| `keybd_event` | **yes** | dismissed the ReShade splash; moved the main-menu cursor (610 changed px in the menu row, at the boundary between *Graj* and *Graj wieloosobowa*) |
| `SendKeys` | no | `ArgumentException: Nieprawidłowe słowo kluczowe "SPACE"`; valid tokens changed nothing |
| `mouse_event` | no | clicked the measured centres of *Graj* and *Opcje*; menu unchanged |
| `SendInput` | returns 1, no effect | the event is queued and the game ignores it |

Focus must be stolen with `AttachThreadInput` against the game's own input
queue. `SetForegroundWindow` alone left a different window focused
(`title='A'` vs the game's `title='T'`), and `SendKeys` then went there.

## Two PowerShell traps on this box

**`run_in_session1.ps1` collapses `-ScriptArgs` into one argument.** Verified
with a two-parameter echo script: `-ScriptArgs hello,world` arrived as the
single string `'hello,world'`. Anything that takes a list must read it from a
file, not the command line. Earlier attempts "silently used defaults" because
of this.

**A PowerShell pipeline inside a double-quoted `ssh` command is eaten by
cmd.exe** — `ForEach-Object is not recognized`. Put the logic in a `.ps1` and
invoke it with `-File`. This was already known and I still hit it.

## Menu item positions (measured, not guessed)

Main menu text occupies y=134..144 of a 969-tall window:

| item | x centre | fraction |
|---|---:|---:|
| TEARDOWN | 193 | 0.1287 |
| Graj | 481 | 0.3201 |
| Graj wieloosobowa | 718 | 0.4781 |
| Postacie | 925 | 0.6154 |
| **Opcje** | 1145 | **0.7616** |
| Wyjdz | 1367 | 0.9092 |

## What is still open

The game now renders live and accepts `keybd_event`, so the mod-menu route is
reachable — but this session ended before the Options→Mods path was walked.
Nothing here claims the mod loads. That is still unproven.

The correction that matters: **the harness was the broken part, not the mod.**
