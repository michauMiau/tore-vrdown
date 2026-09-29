--[[===========================================================================
  tdvr_boot.lua -- production bootstrap.

  Reaches the mod out of mods/teardown_vr/ and starts it, using only what has
  been measured to work in the game's Lua VM:

    * loadfile() with an ABSOLUTE path returns a real function. Relative
      paths resolve to nil. dofile() returns without error and runs nothing,
      so it must not be used.
    * loadfile() COMPILES the chunk. It does not run it, and the game only
      calls init/tick/draw for scripts it registered itself. A chunk reached
      this way has to be started by hand -- calling init() is the whole
      reason this file exists.
    * The store (savegame.*) is the only reliable observation channel. No
      shipped script uses the io library, so a missing trace file proves
      nothing.

  INSTALL
  ---------------------------------------------------------------------------
  Loaded from data/ui/menu.lua, NOT data/script/common.lua.

      TDVR_CHUNK = loadfile("D:/SteamLibrary/steamapps/common/Teardown/mods/teardown_vr/tdvr_boot.lua")
      if TDVR_CHUNK then TDVR_CHUNK() end

  common.lua must stay untouched. menu.lua includes game.lua, which includes
  common.lua, and a failed include ABORTS the including file -- so an
  uncompilable common.lua stopped menu.lua part-way, before it ever reached
  its own init(). The version of this file that used to live in common.lua
  never ran for exactly that reason. See docs/ENGINE_LUA_VM.md.

  MEASURED RESULT
  ---------------------------------------------------------------------------
  The mod loads and initialises: bus/haptics/camera/input all come up and the
  bus object exists (savegame.tdvrstate), and the store shows the run reached
  "started". menu.lua's tick() and draw() both call back into this file, so
  TDVR.ticked / TDVR.drawn are live counters now.

  The remaining open question is the frame host: the game has to be in a level
  for menu.lua's tick to keep running, and the autopilot in tdvr_campaign.lua
  is what gets it there unattended.
]]

local MOD_DIR = "D:/SteamLibrary/steamapps/common/Teardown/mods/teardown_vr/"
local MAIN    = MOD_DIR .. "main.lua"
local CAMPAIGN = MOD_DIR .. "tdvr_campaign.lua"

local TDVR = {}
TDVR.started = false
TDVR.run = ((_G.TDVR and _G.TDVR.run) or 0) + 1
TDVR.ticked  = 0
TDVR.drawn   = 0
TDVR.errors  = {}

-- Publish the table immediately, before anything can be loaded. The companion
-- files (tdvr_campaign.lua) reach it as a global, and TDVR.load() runs them
-- while the assignment that used to sit at the bottom of this file had not
-- happened yet -- so campaignPoll indexed a nil global and died on load.
_G.TDVR = TDVR

--[[---------------------------------------------------------------------------
  heartbeat
    There is NO other channel. Measured in the engine's VM (docs/ENGINE_LUA_VM.md):
    io = nil, os = nil. There is no file, no timestamp and no stderr available
    from Lua, so the store is the only place a value can land.

    Two prefixes, because neither alone is enough:

    options.* is rewritten by the game during the menu, so it shows up early.
    savegame.* is written only at the end of a full startup -- a run that stopped
    at the menu wrote nothing at all, and every key then read back ABSENT, which
    is indistinguishable from "the code never ran".

    Writing both makes a stale store distinguishable from a live one, so a
    missing key is never read on faith.
-------------------------------------------------------------------------------]]
function TDVR.heartbeat()
	local line = "run=" .. TDVR.run ..
		" ticks=" .. TDVR.ticked ..
		" draws=" .. TDVR.drawn ..
		" started=" .. tostring(TDVR.started) ..
		" errs=" .. #TDVR.errors ..
		" bus=" .. tostring(_G.vr and _G.vr.bus)
	-- options.* is the channel that updates during the menu.
	SetString("options.tdvrbeat", line)
end

local function note(where, err)
	TDVR.errors[#TDVR.errors + 1] = where .. ": " .. tostring(err)
	-- The store is the diagnostic channel that actually survives. Keep the
	-- key short and flat: keys outside [a-z0-9-] are dropped on serialisation.
	SetString("savegame.tdvrerr", TDVR.errors[#TDVR.errors])
end

--[[---------------------------------------------------------------------------
  Load the mod chunk and run its body. This defines VRBUS / VRHAPTICS /
  VRCAMERA / vrinput plus init/tick/draw as globals.
-------------------------------------------------------------------------------]]
TDVR.campaign = nil

function TDVR.load()
	local chunk = loadfile(MAIN)
	if not chunk then
		note("loadfile", "returned nil for " .. MAIN)
		return false
	end
	local ok, err = pcall(chunk)
	if not ok then
		note("chunk", err)
		return false
	end

	-- The autopilot is a separate file so the campaign logic stays out of the
	-- merged build, and so a failure in it can never take the mod down with it.
	local camp = loadfile(CAMPAIGN)
	if camp then
		local ok2, err2 = pcall(camp)
		if not ok2 then
			note("campaign", err2)
		elseif type(TDVR.campaignPoll) ~= "function" then
			note("campaign", "loaded but defines no campaignPoll")
		end
	else
		note("campaign", "no " .. CAMPAIGN)
	end
	return true
end

--[[---------------------------------------------------------------------------
  Start it. loadfile compiled the chunk but never ran the lifecycle, so
  without this the mod sits there fully defined and completely inert:
  vrModules would read all-false, no bus would exist, and nothing would
  reach the native side.
-------------------------------------------------------------------------------]]
function TDVR.start()
	-- Mark entry BEFORE anything can fail, and into options.*, which measured
	-- out as the file the game rewrites during the menu. savegame.* is only
	-- written at the end of a full startup, so it cannot answer "did this run
	-- get here" -- a run that stopped at the menu wrote nothing at all, and
	-- every key then read back ABSENT, which reads as "never ran" and is wrong.
	SetString("options.tdvrstage", "enter-start")
	if TDVR.started then return true end
	-- Stamp the run and CLEAR any error left by a previous one before doing
	-- anything else. tdvrerr is only ever written on failure, so without this
	-- a value from an older run reads as a current failure -- which is exactly
	-- the wrong conclusion to draw from it.
	SetInt("savegame.tdvrrun", TDVR.run)
	SetString("savegame.tdvrerr", "")
	SetString("options.tdvrstage", "load")
	if not TDVR.load() then
		SetString("options.tdvrstage", "load-failed")
		return false
	end

	SetString("options.tdvrstage", "init")
	if type(_G.init) ~= "function" then
		note("init", "mod defines no global init")
		SetString("options.tdvrstage", "no-init")
		return false
	end
	local ok, err = pcall(_G.init)
	if not ok then
		note("init", err)
		-- Not fatal: the bus may still be usable, so report and carry on.
		SetString("options.tdvrstage", "init-threw")
	end

	-- Report what actually came up, by the names the mod really uses.
	SetString("savegame.tdvrstate",
		"run=" .. TDVR.run .. " " ..
		"bus="    .. tostring(_G.vrModules and _G.vrModules.bus)    ..
		" hap="   .. tostring(_G.vrModules and _G.vrModules.haptics) ..
		" cam="   .. tostring(_G.vrModules and _G.vrModules.camera)  ..
		" in="    .. tostring(_G.vrModules and _G.vrModules.input)   ..
		" busobj=" .. tostring(_G.vr ~= nil))

	TDVR.started = true
	SetString("options.tdvrstage", "started")
	-- Write once immediately, so the file's existence and mtime answer "did the
	-- bootstrap run" even if the game never reaches a frame.
	TDVR.heartbeat()
	return true
end

--[[---------------------------------------------------------------------------
  Per-frame driving. menu.lua's tick() and draw() both call in here, so these
  run for as long as the game keeps that file alive.
-------------------------------------------------------------------------------]]
function TDVR.tick(dt)
	if not TDVR.started then return end
	TDVR.ticked = TDVR.ticked + 1
	-- The autopilot has to be polled from the frame loop, because getting into a
	-- level is what keeps this loop running in the first place. Wrapped: a fault
	-- here must not cost the mod its heartbeat.
	if type(TDVR.campaignPoll) == "function" then
		pcall(TDVR.campaignPoll)
	end
	if type(_G.tick) == "function" then
		pcall(_G.tick, dt or (GetTimeStep and GetTimeStep() or 0.016))
	end
	-- Heartbeat every frame, not every 60: this file is the only channel that
	-- reflects the present, so staleness has to be visible immediately. Writing
	-- one short line per frame is cheap and the game is the only writer.
	TDVR.heartbeat()
end

function TDVR.draw()
	if not TDVR.started then return end
	TDVR.drawn = TDVR.drawn + 1
	if type(_G.draw) == "function" then
		pcall(_G.draw)
	end
	TDVR.heartbeat()
end

--[[---------------------------------------------------------------------------
  Start immediately on load. The caller is a bootstrap that runs once, so
  deferring would mean the caller has to remember; and TDVR.start() is
  idempotent, so a second call is harmless.
-------------------------------------------------------------------------------]]
TDVR.start()

_G.TDVR = TDVR
