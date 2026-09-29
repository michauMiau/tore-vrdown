# Architecture: modules stay in mods/, bootstrap lives in common.lua

Measured 2026-09-28. The design the user proposed works, and the constraints
are now known rather than guessed.

## The answer

Yes — modules can stay where they are, in `mods/teardown_vr/`, and
`data/script/common.lua` can load them. **But only via `loadfile` with an
absolute path.** Relative paths do not work at all.

## Measured path behaviour

From a file in `data/script/` (i.e. `common.lua`), each form recorded in the
store:

| form | result |
|---|---|
| `loadfile("D:/SteamLibrary/.../mods/teardown_vr/file.lua")` | `ok:function` — **works** |
| `loadfile("../mods/...")` | `ok:nil` — silently fails |
| `loadfile("tdvr_data_level.lua")` | `ok:nil` — silently fails |
| `dofile("D:/.../mods/.../file.lua")` | `ret=true` but **returns nil and the chunk does not run** |
| `dofile("mods/...")` | `ok:nil` |
| `dofile("tdvr_data_level.lua")` | `err:cannot open` |
| `load(io.open(abs):read("*a"))` | `err:[string "data/script/common.lua"...` |

And the decisive one:

```xml
tdvr_path_mods value="LOADED-from-mods"/>
tdvr_exec_loadfile value="CALLED-OK"/>
```

A file in the **mods directory** executed and wrote its own marker, reached
from `common.lua`. So the split the user asked about is viable.

## What this means for the split

- **`dofile` is a trap.** It returns without error and does nothing. Any code
  path that reaches for it will look like it works.
- **`loadfile` + explicit call is the only route that runs.** It returns a real
  function; calling it executes the chunk.
- **Absolute paths only.** Relative resolution in the game VM is not the
  process CWD. `../` appears in shipped `#include` directives and those work,
  because `#include` is a *preprocessor* directive with its own root — that
  must not be confused with `loadfile` at runtime.

## What is still unresolved

I tried to measure whether a mods-folder file has the same Lua environment as
a `data\` file (whether it can see `_G`, `clamp`, `GetInt`, `UiPush`,
`setmetatable`, `debug`, `os`, `io`, `coroutine`, `require`). Those probes
produced confusing results — a stale `perm_sees_g` survived from an earlier
run while later rounds landed nothing.

The most likely explanation is mundane: **`savegame.xml` is only written at
save time**, so a missing key does not distinguish "code did not run" from
"no save happened since". That is the same class of error as the earlier
`io.open` probe and I should have checked the save timing first. The next
attempt must force a save (or read the registry from the DLL) before drawing
any conclusion.

This matters for the design: if a mods-folder file has the same rights as
`data\`, there is no reason to split the code at all, and the only reason to
touch `common.lua` is the bootstrap line. If it is sandboxed, then the split
must be drawn along real capability lines rather than convenience.

## Cleanup status

Probe files created in the game directory that still need removal:
`mods/teardown_vr/tdvr_perm.lua`, `mods/teardown_vr/tdvr_include_probe.lua`,
`Teardown\tdvr_d1.lua`, `Teardown\tdvrsub\tdvr_d2.lua`, `C:\tdvr\tdvr_outside.lua`.
`data\script\common.lua` and `data\script\main.lua` are restored to their
original bytes (3350 / 6457) via `.orig`.
