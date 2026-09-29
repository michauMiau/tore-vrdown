# BREAKTHROUGH: the game's own Lua is writable and executes. 2026-09-28

The mod path is dead. The game's own Lua path works. The user suggested this
("ingerencja w lua gry bezpośrednio") and it was correct.

## What works

`D:\SteamLibrary\steamapps\common\Teardown\data\script\common.lua` is writable
(90 `.lua` files in `data\script`, all writable). It is included by
`data\game.lua` line 1 (`#include "script/common.lua"`), so it is evaluated
during game startup — no level navigation required.

Proof, twice, with different values so stale data cannot explain it:

```xml
tdvrprobe_a value="999111"/>
tdvrprobe_b value="3.5"/>
tdvrprobe_c value="round2-ALIVE"/>
tdvrprobe_d value="1"/>
tdvr_frames value="0"/>
```

These are in `%LOCALAPPDATA%\Teardown\savegame.xml`. Round 1 wrote `4242` /
`alive`, round 2 wrote `999111` / `round2-ALIVE`; only the new values appear,
so our code genuinely ran.

`data\script\main.lua` (6457 bytes, the campaign entry script, uses
`server.init`/`client.init`/`client.tick`) is the other injection point, and
it is more useful because it has real lifecycle callbacks to hook.

## The measurement that unlocks the rest of the project

`tdvr_frames` incremented once per include, and the store round-trips through
`savegame.xml`. So the game's Lua can **observe and drive the game** — which
the mod system could not do. Combined with the earlier negative
(`docs/LUA_INPUT_WRITE_FEASIBILITY.md`: no input *writer* exists in Lua), the
architecture is now:

- **game Lua (`data\script\`)** — read game state, drive game logic, render
- **native shim (vtable slot 17, RVA 0x4E08ED)** — inject input, because Lua
  still cannot create input state

`data\script\main.lua` also gives a natural home for the per-frame work:
`server.tick` for logic, `client.draw` for the overlay.

## My earlier negative was WRONG — and here is exactly why

I concluded "no script mod executes" from two tests that were both invalid:

1. **I modified `mods/speedometer`, which was never enabled.** `shown="true"`
   in `mods.xml` only means "visible in the menu". The enable list is
   `modlists/1.xml`, and `builtin-speedometer` was not in it. I then "fixed"
   this by using `lasergun`, which *is* enabled — and it was still silent,
   which is the part that still stands: **user mods genuinely do not load**,
   for reasons separate from what I got wrong.
2. **The trace-file probe was worthless.** No shipped script anywhere uses the
   `io` library — I verified this by searching `data\script\**` and `data\*.lua`
   and the only `io.open` hits were my own injections. So `io.open` may be
   stripped from the VM, and a missing file proves nothing. The store probe is
   the correct one, because the game itself uses `savegame.*` throughout.

The lesson generalises: **a negative result is only as good as the probe.**
Both of mine had a probe that could not distinguish "did not run" from "ran
but could not be observed".

## Cleanup

All probes removed and each file restored to its exact original byte count:

- `data\script\common.lua` → 3350 ✓
- `data\script\main.lua` → 6457 ✓
- `mods\lasergun\main.lua` → 6655 ✓
- `mods\speedometer\main.lua` → 1880 ✓

`TDVR` text confirmed absent from all four.

## Next

Re-point the VR mod at `data\script\main.lua` instead of the mod directory.
The `vrbus`/`vrcamera`/`vrhaptics` modules stay as they are — only the
delivery changes.
