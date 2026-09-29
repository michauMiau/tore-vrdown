# PROVEN: no script mod in this install executes. 2026-09-28

This is the end of the mod-loading investigation. It is a negative, it is
measured, and it closes the question.

## The test

I stopped testing our own mod and modified a **shipped** mod instead:
`mods/speedometer/main.lua`, which is enabled (`shown="true"` in `mods.xml`,
and present in `modlists/1.xml`). Two independent tells were appended:

1. `draw()` wrapped to fill the whole window with magenta via `UiRect`
2. `SetInt("savegame.mod.tdvrprobe_ran", 777)` — the game's own persist path

## The result

| probe | expected if mods run | measured |
|---|---|---|
| magenta pixels in a full-desktop capture | a large fraction of the frame | **0 of 441,000 sampled** |
| `tdvrprobe` in `savegame.xml` | present | **absent** |
| capture was a live render | — | 190,349 distinct colours, so the game was genuinely running |

The capture had 190k distinct colours, so this is not a dead-window or
modal-prompt artefact. The game was rendering. The mod code did not run.

## What this rules out

Everything below was tested and is **not** the cause:

- our folder name (`teardown_vr` vs the game's `builtin-teardown-vr`)
- `version = 1` vs `version = 2` in `info.txt` (and shipped `screenrecorder`
  has no `version` field at all, so the field is not even required)
- `#version 2` and the `server.init`/`client.draw` spelling (both spellings
  tested side by side in `lua_dual.lua`, neither fired)
- `io.open` being sandboxed (the store write cannot be sandboxed)
- the two modal prompts (ReShade and SteamVR are both fixed and gone)

A shipped mod with a trivially visible side effect does not run. The
conclusion is that **script mods do not execute in this install or this
session at all** — it is not a property of our mod.

## Cleanup done

- `speedometer/main.lua` restored from `.orig` (1880 bytes, probe text
  verified absent)
- throwaway `mods/teardown-vr` (hyphen) directory removed
- `mods/vr_probe_clone` removed — it was also enabled and was never a valid
  control
- our `info.txt` `version` returned to `1`

## Where this leaves the project

The Lua route is closed, on two independent grounds:

1. Lua cannot inject input — `docs/LUA_INPUT_WRITE_FEASIBILITY.md` and
   `docs/LUA_INJECTION_FOUND.md` (refuted, see `fact_id=64`)
2. Lua cannot even be used to observe or drive the game, because it does not
   run here

So the project rests on the native path, and that path has a verified
chokepoint: the game reads controller state through
`call qword ptr [r10+0x88]` at RVA `0x4E08ED` — vtable slot 17,
`GetDigitalActionData`, 37 times per frame, writing the result to a stack
local. A trampoline there can synthesise the state. That is the work now.
