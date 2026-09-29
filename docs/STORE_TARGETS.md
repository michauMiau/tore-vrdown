# Where the Lua store actually writes

Measured 2026-09-28. This closed two questions that had been open for hours,
and one of them had been quietly corrupting every result.

## The store maps to exactly two files

A test wrote one key per prefix and watched which file gained it:

| prefix written | lands in |
|---|---|
| `savegame.<key>` | `%LOCALAPPDATA%\Teardown\savegame.xml` |
| `options.<key>`  | `%LOCALAPPDATA%\Teardown\options.xml` |
| `<key>` (bare)   | **dropped** |
| `game.<key>`     | **dropped** |
| `vr.<key>`       | **dropped** |

So a key needs an explicit `savegame.` or `options.` prefix. A bare
`SetString("tdvrrun", ...)` is silently discarded, which looks exactly like
"the code never ran" — the same trap as the `io` probe and the stale-`tdvrerr`
read, from a third direction.

## `io` still unresolved, and now it does not matter

`io.open(..., "w")` wrote no file in either the mods directory or `C:\tdvr`,
while the store wrote fine in the same chunk. So `io` is either absent or
write-blocked, and the two cases were not separated. It no longer needs to be:
`savegame.` and `options.` are a reliable channel, and both were confirmed
writing from `common.lua`.

## Why earlier runs showed everything ABSENT

`savegame.xml` stayed at 118 bytes for hours and `log.txt` at 0. It is
written **at the end of a full game start**, not during the menu. A run that
only ever reached the main menu produced no store write at all, so every TDVR
key read back ABSENT — which reads as "the bootstrap never ran" and is wrong.

The saving begins once the game gets far enough into its own startup. That is
why the sequence that finally produced data was:

1. stop any running `teardown`
2. `fresh_game.ps1` (full launch, not a menu poke)
3. wait ~55 s
4. then read

`savegame.xml` went 118 -> 204 bytes and the probe keys were in it.

## The comment-newline bug, and why it hid for so long

`Set-Content -NoNewline` on the injected `common.lua` block glued the closing
marker to the first line of the original file:

```lua
-- ==== end TDVR bootstrap ====function clamp(value, mi, ma)
```

A line comment swallowing `function clamp(...)` breaks the parse of the whole
shipped helper library, and the TDVR block sits *above* it — so the file
looked correctly wired while nothing downstream worked. The symptom set
("everything ABSENT") is identical to the store-timing problem above, so the
two masked each other.

Both editors now keep the trailing newline, and `build/install_mod.ps1` always
rebuilds from a pristine `.tdvrorig` copy so blocks cannot stack.

`data/ui/menu.lua` was destroyed once while regenerating that backup (a
regex replace ran against a file that had already been truncated). It was
restored from `teardown-analysis/game_full/Teardown/data/ui/menu.lua` —
32,825 bytes, md5 `95ca9bc12ec7ab1704e3b66aa69ecd67`, with `init` at L67,
`tick(dt)` at L667 and `draw()` at L760, matching the layout recorded before
the damage. That copy is now kept at `build/gamebak/menu.lua.pristine`.

Lesson: never rewrite the source a `-Raw` regex replace is about to be applied
to. Write to a temp, verify the size, then move.
