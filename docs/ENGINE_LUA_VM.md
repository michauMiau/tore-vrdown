# The engine's Lua VM, measured

All of this is from a probe written into `data/ui/menu.lua`'s `init()` and read
back out of `options.xml`, 2026-09-28. Nothing here is inferred from the
shipped source or from a stub file.

## Standard library

```lua
loadfile = function
io       = nil
os       = nil
```

`io` and `os` are **not present**. This closes a question that had been open
for hours: the reason no trace file ever appeared was not a wrong path, a
missing write permission, or a timing problem — `io.open` does not exist. Every
earlier "the write failed" reading of that was wrong, and the only way to tell
was to ask the VM for `type(io)` instead of inferring from a missing file.

Consequence: **the store is the only channel.** `savegame.*` and `options.*`
(see `STORE_TARGETS.md`); no files, no paths, no timestamps from Lua.

## A failed #include aborts the including file

The game log gives the exact chain:

```
Error compiling: data/script/common.lua
Error loading include: data/script/common.lua from: data/game.lua
Error loading include: data/game.lua from: data/ui/menu.lua
```

`menu.lua` includes `game.lua`, which includes `common.lua`. When an include
fails, the **including** file stops there. So a single uncompilable
`common.lua` halted `menu.lua` part-way — before it ever reached its own
`init()`, which is the only place frames and the mod bootstrap live. Nothing
ran, and no diagnostic key appeared, because the code that would have written
one was in the file that never got there.

This is why **the mod does not touch `common.lua` at all.** It is a helper
library with no lifecycle; `menu.lua` is the frame host. Editing the one file
that everything includes is the single riskiest thing to do here.

## `loadfile` with an absolute path works, from menu.lua

`TDVR_CHUNK = loadfile("D:/.../mods/teardown_vr/tdvr_boot.lua")` returns a
function and calling it runs the chunk. This is how the mod in `mods/` is
reached, and it is confirmed working from `menu.lua`'s `init()`.

The globals are spelled without `local`. The engine's compiler rejected
`local _f = loadfile(...)` in `common.lua`; `menu.lua` is compiled the same
way, so the blocks use a global temp. One leaked slot is cheaper than a file
that will not compile.

## Read the game's log

`%LOCALAPPDATA%\Teardown\log.txt` carries compile and include failures with the
full include chain. It was **0 bytes** through hours of runs that all read as
"the mod did not load". Checking it first would have found the compile error in
one step, and everything after that was downstream of it.

Rule for this project: **check `log.txt` before concluding anything about
whether the mod ran.** An absent key means "unknown", not "did not execute".
