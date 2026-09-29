# Frame host: `data/ui/menu.lua`

Measured 2026-09-28 in the live game. This is the answer to "who calls
`init`/`tick`/`draw`", and it is not the mod system and not `common.lua`.

## The chain

- `data/game.lua:1` is `#include "script/common.lua"`. `common.lua` defines
  helper functions only (`clamp`, `trim`, `startsWith`, `splitString`,
  `hasWord`, `smoothstep`, `progressBar`, `GetTextSize`, ...). **It defines no
  `init`, `tick`, or `draw`.** Anything bootstrapped there runs exactly once.
- `data/ui/menu.lua` is 32,825 bytes and defines the real bare lifecycle:

  ```
  L67   function init()
  L667  function tick(dt)
  L760  function draw()
  ```

  plus `isMainMenuVisible`, `handleCommand`, `handleIntent`, `resumeCampaign`,
  `tryStartMission`, `openModsMenu`, and the window helpers.
- **Nothing `#include`s `ui/menu.lua`.** It is loaded directly by the engine,
  so it is the root of the Lua lifecycle. That is the whole answer: the
  engine owns the global `init`/`tick`/`draw` and `menu.lua` defines them.
- `menu.lua:8` is `#include "script/challenge.lua"`, which is why
  `data/script/challenge.lua` and its eight siblings (`camerasweep`,
  `challengeescape`, `challengefetch`, `challengehunted`, `challengemayhem`,
  `destroyalarm`, `fetchchopper`, `huntedchopper`) have bare `init`/`tick`/
  `draw`: they are pulled into a scope that the engine ticks. So bare
  callbacks are the *included* form, and a mod is the *loader* form -- the
  mod system must define them itself.

## Consequence

A mod reached through `common.lua` is initialised but never ticked. To get
frames it must be wired into the one host that has them: `ui/menu.lua`.

Two things follow, and they are not symmetric:

1. `init` can stay in the `common.lua` bootstrap. It runs once, and once is
   all that is needed to build the object graph.
2. `tick`/`draw` need a call site inside `ui/menu.lua`, appended after its own
   definitions so the host's logic is untouched and the mod is additive.

`ui/menu.lua` is the main menu. Whether its `tick` keeps running once a
campaign level is loaded is **not yet measured** -- that is the next
question, and it decides whether the mod gets frames in-game or only in the
menu. Do not assume either way; check it.

### MEASURED: it ticks, but in the menu

Wiring the two calls into `menu.lua` works and is idempotent (running the
installer twice leaves exactly 2 markers, `menu.lua` 33,029 bytes). Result:

```
tdvrrun      = 1
tdvrstate    = run=1 bus=true hap=true cam=true in=true busobj=true
tdvr_ticks   = 326700   -> 342540 on the next read
tdvrerr      = ABSENT
```

So the mod now gets frames. The open question is *where*: the automated
navigation never reached a campaign level (the game was still on the main
menu, `cpu=123.9`, screenshot OCR shows `TEARDOWN Graj` / `Galeria`), so
these ticks are menu ticks. Whether `ui/menu.lua`'s `tick` survives a level
load is still unproven, and campaign input automation is currently blocked
because `mouse_event` and `SendInput` are ignored by the game -- only
`keybd_event` works, so the level has to be entered by keyboard alone.

That distinction is the next thing to settle. Everything up to it is done:
mod loads, initialises, all four modules up, bus constructed, and ticked.

## Consequence for `TDVR.tick`

`lua/tdvr_boot.lua` exposes `TDVR.tick(dt)` / `TDVR.draw()` as plain functions
precisely because nothing calls them yet. The wiring is a two-line addition to
`ui/menu.lua`:

```lua
-- ==== TDVR (teardown VR) ====
if _G.TDVR then
  pcall(_G.TDVR.tick, dt)
end
-- ==== end TDVR ====
```

in `tick`, and the `draw` equivalent in `draw`. Guarded, because a bootstrap
that failed to load must not take the menu down with it.
