# Find where Teardown keeps a Lua script's persisted key/value store.
#
# WHY
#
# The Lua side is written and tested. The protocol (docs/LUA_MOD_DESIGN.md) sends
# haptics and input over a small key/value bus. The native side has to read the
# same store from inside the process, because the alternative (Lua calling a
# native export) is not available: the mod sandbox exposes no FFI and no
# require, and the function list in data/script_defs.luau has no binding call.
#
# So the question is concrete: where does SetInt put its bytes?
#
# WHAT TO MEASURE
#
# In data/script_defs.luau, SetInt is documented. Read the doc comment to get
# the real signature and any stated default store name. Then search the binary
# for the store: Teardown mods are per-mod directories, and a store is usually
# a table in the mod's own state, a per-mod settings file on disk, or a string
# map inside the script VM. Three candidate shapes, in order of likelihood:
#
#   1. a file under the mod directory (mods/<modname>/ or the save dir)
#   2. a hash map keyed by string, reachable from the mod's Lua state
#   3. a fixed struct the engine mirrors for the settings UI
#
# For the file shape, list the mod directories and look for a file that changes
# when a script calls SetInt. For the memory shapes, find what SetInt itself
# does: the string "SetInt" appears in the binary as part of a registration
# table, and the registration table entry usually sits next to the function
# pointer that implements it. Tracing that pointer gives the write, and the
# write tells you the layout.
#
# READ-ONLY. Do not start the game, do not inject, do not write to the mod
# directory: the user is using the machine.
#
# Report: the store's identity (file path, or RVA of the code that writes it),
# the layout of one entry, and how the mod name selects the store. If it cannot
# be determined, say exactly which of the three shapes you ruled out and how,
# rather than guessing. An honest "the file shape is ruled out because X" is
# worth more than a confident wrong offset.
#
# Write the findings to /home/truenas_admin/teardown-vr-mod/docs/LUA_STORE_LOCATION.md
