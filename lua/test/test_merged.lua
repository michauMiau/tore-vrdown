--[[===========================================================================
  test_merged.lua -- exercises the MERGED single-file build.

  Run:  python3 merge.py && lua5.1 test/test_merged.lua

  Why this is a separate test: the merged build is the fallback for when
  dofile() from a main.lua mod turns out not to work in the real game. If that
  fallback is itself broken, the mod has no working path at all. So it is
  tested with dofile() HARD DISABLED -- any attempt to load a sibling file is
  an error, not a fallback.
============================================================================]]

package.path = "./?.lua;" .. package.path

local M = dofile("test/mock_teardown.lua")
M.moddir = "merged"
M.mod_assets_available = true
M.install(_G)
local load_file = M.real_dofile

local passed, failed = 0, 0
local failures = {}
local function ok(cond, name, detail)
  if cond then
    passed = passed + 1
    print(string.format("  PASS  %s", name))
  else
    failed = failed + 1
    failures[#failures+1] = name .. (detail and ("  -- " .. detail) or "")
    print(string.format("  FAIL  %s%s", name, detail and ("  -- " .. detail) or ""))
  end
end
local function section(t)
  print("\n== " .. t .. " " .. string.rep("=", math.max(4, 66 - #t)))
end

--[[---------------------------------------------------------------------------
  Hard-disable dofile before the merged file is even loaded.
-------------------------------------------------------------------------------]]
local dofileAttempts = {}
_G.dofile = function(p)
  dofileAttempts[#dofileAttempts+1] = p
  error("dofile() is disabled in this test", 2)
end

section("merged build loads with dofile disabled")
local okLoad, err = pcall(load_file, "merged/main.lua")
ok(okLoad, "merged/main.lua compiles and runs", tostring(err))
if not okLoad then
  print("\n  passed 0, failed 1")
  os.exit(1)
end

_G.init()

ok(#dofileAttempts == 0, "no dofile attempt at all",
   "attempts=" .. #dofileAttempts .. " " .. tostring(dofileAttempts[1]))
ok(vrMerged == true, "MERGED_BUILD marker detected")
ok(vrModules.bus == true,     "bus available")
ok(vrModules.haptics == true, "haptics available")
ok(vrModules.camera == true,  "camera available")
ok(vrModules.all == true,     "all modules available")

--[[---------------------------------------------------------------------------
  Behaviour parity: the merged build must detect the same events.
-------------------------------------------------------------------------------]]
section("behaviour parity")
local W = M.world

W.pos = { 1.0, 2.0, 3.0 }
W.vel = { 0.0, 0.0, 0.0 }
W.grounded = true
W.groundDist = 0.0
W.health = 1.0
vrhap:init()
M.clearEvents()
M.runFrames(_G, 30)

eq_ok = ok(M.store_int["vr.cam.valid"] == 1, "camera block published")
eq_ok = ok(M.store_float["vr.cam.x"] == 1.0, "cam.x correct",
           tostring(M.store_float["vr.cam.x"]))
ok(M.store_float["vr.cam.ipd"] > 0.05 and M.store_float["vr.cam.ipd"] < 0.08,
   "cam.ipd plausible", tostring(M.store_float["vr.cam.ipd"]))
ok(#M.events() == 0, "no spurious events at rest", "count=" .. #M.events())

-- damage
M.clearEvents()
W.health = 0.85
M.runFrames(_G, 1)
local dmg = M.findEvents(1)
ok(#dmg >= 1, "damage detected", "count=" .. #dmg)
if #dmg >= 1 then
  ok(dmg[#dmg].amp > 0 and dmg[#dmg].amp <= 1.0, "damage amplitude sane",
     tostring(dmg[#dmg].amp))
  ok(dmg[#dmg].ttl > 0, "damage carries duration", tostring(dmg[#dmg].ttl))
end
W.health = 1.0
M.runFrames(_G, 2)

-- landing
M.clearEvents()
W.pos = { 0.0, 60.0, 0.0 }
W.vel = { 0.0, -12.0, 0.0 }
W.grounded = false
M.runFrames(_G, 25)
ok(#M.findEvents(3) == 0, "no landing while airborne")
W.grounded = true
W.vel = { 0.0, 0.0, 0.0 }
M.runFrames(_G, 1)
ok(#M.findEvents(3) >= 1, "landing detected on touchdown", "count=" .. #M.findEvents(3))

-- footsteps
M.clearEvents()
W.pos = { 0.0, 0.0, 0.0 }
W.vel = { 0.0, 0.0, 4.0 }
W.grounded = true
M.runFrames(_G, 120)
ok(#M.findEvents(12) >= 1, "footsteps detected while walking",
   "count=" .. #M.findEvents(12))

-- tool fire
M.clearEvents()
W.tool = "gun"
W.ammo = 100
M.runFrames(_G, 2)
W.ammo = 97
M.runFrames(_G, 1)
ok(#M.findEvents(5) >= 1, "tool fire detected", "count=" .. #M.findEvents(5))

-- haptic asset load
vrhap.playGameHaptics = true
vrhap.handles = {}
vrhap._stats.loadFail = 0
vrhap._last = {}
M.haptics_played = {}
W.health = 1.0
M.runFrames(_G, 2)
W.health = 0.9
M.runFrames(_G, 1)
ok(vrhap._stats.loadFail == 0, "haptic assets load in merged build",
   "failures=" .. tostring(vrhap._stats.loadFail))
ok(#M.haptics_played >= 1, "PlayHaptic invoked in merged build",
   "count=" .. #M.haptics_played)

--[[---------------------------------------------------------------------------
  Draw and heartbeat
-------------------------------------------------------------------------------]]
section("heartbeat and draw")
M.world.inputs["menu_cancel"] = true
-- The overlay is off by default, on purpose: a loaded mod must not paint over
-- the game in the menu or the pause screen. Set from the console to inspect it.
-- Arm it here, because this assertion is about what the overlay DRAWS, not
-- about whether it is on by default.
vrOverlay = true
local okDraw = pcall(_G.draw)
ok(okDraw, "draw() runs")
local found = false
for _, s in ipairs(M.drawn) do if s:find("Teardown VR") then found = true end end
ok(found, "overlay content rendered", "lines=" .. #M.drawn)

-- And it must really be off again when not armed, or the mod would be
-- painting over the game in every menu.
vrOverlay = false
M.drawn = {}
pcall(_G.draw)
ok(#M.drawn == 0, "overlay draws nothing when not armed", "lines=" .. #M.drawn)

M.world.inputs["menu_cancel"] = nil

local hb0 = M.store_int["vr.hb"] or 0
M.runFrames(_G, 120)
ok((M.store_int["vr.hb"] or 0) > hb0, "heartbeat advances",
   string.format("%s -> %s", tostring(hb0), tostring(M.store_int["vr.hb"])))

--[[---------------------------------------------------------------------------
  Report
-------------------------------------------------------------------------------]]
print("\n" .. string.rep("=", 70))
print(string.format("  passed %d, failed %d", passed, failed))
if failed > 0 then
  print("\n  FAILURES:")
  for _, f in ipairs(failures) do print("    - " .. f) end
end
print(string.rep("=", 70))
os.exit(failed == 0 and 0 or 1)
