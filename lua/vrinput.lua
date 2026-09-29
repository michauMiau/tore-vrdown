--- Teardown VR — action-driven input layer.
--
-- WHY THIS EXISTS
--
-- The game exposes InputDown/InputPressed/InputValue for READING, and nothing
-- for writing. Driving the game by moving a camera transform every frame was
-- rejected: it is a bottleneck, it fights the game's own camera, and it does
-- not give buttons.
--
-- What the game DOES expose is a remap table it writes itself from Lua:
--
--   data/ui/components/input_options_logic.lua:259
--     SetString(self.conflictingInfo.actionName, key)
--   with actionName values "options.input.keymap.jump", ".crouch",
--   ".interact", ".flashlight", ".camera_view", ...
--
-- So a mod can rebind any action to any key. That is the lever: VR input
-- declares which action each controller input drives, and this layer reads it
-- back. Remapping cannot CREATE button state, it decides which key drives which
-- action; the state itself has to come from a real device, which is what the
-- native side provides.
--
-- This file is the Lua half. It contains no offsets, no FFI, and no native
-- calls. Everything it uses is a documented mod-facing API.
--
-- CALLBACK SHAPE: main.lua mods use bare globals init/tick/update/draw.
-- GetTimeStep() is a fixed 1/60 inside update() but the real frame time in
-- tick()/draw(); anything integrating over dt must run in tick().
--
-- World axes: x right, y up, -z forward. Metres, radians.

local ACTION = {
	-- movement
	UP = "up", DOWN = "down", LEFT = "left", RIGHT = "right",
	JUMP = "jump", CROUCH = "crouch",
	-- hands
	USETOOL = "usetool", GRAB = "grab", INTERACT = "interact",
	FLASHLIGHT = "flashlight",
	-- tools and camera
	TOOL_GROUP_NEXT = "tool_group_next", TOOL_GROUP_PREV = "tool_group_prev",
	SCOREBOARD = "scoreboard", MAP = "map", PAUSE = "pause", PHOTOMODE = "photomode",
	ZOOM = "zoom",
	-- menu
	MENU_ACCEPT = "menu_accept", MENU_CANCEL = "menu_cancel",
	MENU_UP = "menu_up", MENU_DOWN = "menu_down",
	MENU_LEFT = "menu_left", MENU_RIGHT = "menu_right",
	MENU_NEXT = "menu_next", MENU_PREV = "menu_prev",
	-- driving
	HANDBRAKE = "handbrake", VEHICLE_ACTION = "vehicle_action",
	VEHICLE_RAISE = "vehicle_raise", VEHICLE_LOWER = "vehicle_lower",
	-- reserved
	EXTRA0 = "extra0", EXTRA1 = "extra1", EXTRA2 = "extra2", EXTRA3 = "extra3",
	EXTRA4 = "extra4", EXTRA5 = "extra5", EXTRA6 = "extra6",
}

local KEYPREFIX = "options.input.keymap."

local CHANNELS = {
	{ key = "vr.enabled",  type = "bool",  default = true,  desc = "master on/off" },
	{ key = "vr.hands",   type = "bool",  default = true,  desc = "two-hand locomotion" },
	{ key = "vr.snap",    type = "float", default = 30,    desc = "turn snap degrees" },
	{ key = "vr.scale",   type = "float", default = 1.0,   desc = "movement speed scale" },
	{ key = "vr.deadzone",type = "float", default = 0.12,  desc = "stick deadzone" },
	{ key = "vr.debug",   type = "bool",  default = false, desc = "overlay" },
}

local state = {
	enabled = true,
	hands = true,
	snap = 30,
	scale = 1.0,
	deadzone = 0.12,
	debug = false,

	player = 0,
	bindings = {},      -- action -> key string, read back from the store
	injected = {},      -- action -> true while we hold it down
	pendingRelease = {},-- action -> true, pressed last frame, release next tick
	heldSince = {},     -- action -> frame number, for hold duration
	handYaw = 0,        -- accumulated snap turn, radians
	lastHmdOk = false,
	frames = 0,
}

-- ---------------------------------------------------------------------------
-- store helpers. Mods do NOT get a private store: the engine rewrites only the
-- key prefix, so everything we own lives under savegame.mod.teardownvr.<key>.
-- Keys outside [a-z0-9-], or starting with -/_, are silently dropped from the
-- save file while staying in memory, so keep the names boring.
-- ---------------------------------------------------------------------------

local function storeKey(k)
	return "savegame.mod.teardownvr." .. k
end

local function readConfig()
	for _, c in ipairs(CHANNELS) do
		local v
		if c.type == "bool" then
			v = HasKey(storeKey(c.key)) and GetBool(storeKey(c.key)) or c.default
		elseif c.type == "float" then
			v = HasKey(storeKey(c.key)) and GetFloat(storeKey(c.key)) or c.default
		else
			v = HasKey(storeKey(c.key)) and GetInt(storeKey(c.key)) or c.default
		end
		state[c.key:match("vr%.(.+)")] = v
	end
end

-- Rebind one action to one key, through the game's own remap store.
local function bind(action, key)
	if not action or not key or key == "" then
		return false
	end
	SetString(KEYPREFIX .. action, key)
	-- The game caches action icons in its UI table, so a rebind has to drop
	-- that cache or the UI keeps drawing the old glyphs. Guarded because this
	-- runs before the UI exists during init, and outside the game the table
	-- does not exist at all.
	if _G.Ui then
		_G.Ui.ActionIcons = nil
	end
	return true
end

local function unbind(action)
	if not action then
		return false
	end
	SetString(KEYPREFIX .. action, "")
	return true
end

-- Read back what the game currently has bound for an action.
local function bindingOf(action)
	local k = GetString(KEYPREFIX .. action)
	if k == nil or k == "" then
		return ""
	end
	return string.lower(k)
end

-- ---------------------------------------------------------------------------
-- input adapter. The native layer publishes controller state into the same
-- store; these are the reads. If nothing is publishing, the reads simply report
-- nothing pressed and the game is driven by whatever real hardware exists.
-- ---------------------------------------------------------------------------

local function channel(name)
	-- digital, 0/1
	return GetFloat(storeKey("in." .. name)) or 0
end

local function axis(name)
	return GetFloat(storeKey("axis." .. name)) or 0
end

local function dz(v)
	local a = math.abs(v)
	if a < state.deadzone then
		return 0
	end
	return (v / a) * ((a - state.deadzone) / (1 - state.deadzone))
end

-- Release bookkeeping. We cannot set an action down from Lua, so this tracks
-- INTENT: it is the state the native injector mirrors into the real input
-- layer.
--
-- A press must PERSIST for at least one frame before it can be released.
-- hold() followed immediately by release() inside the same tick would mark the
-- action down and up again within one frame, and the native side polling once
-- per frame would never observe it: the pulse would be invisible. So releases
-- are deferred to the next tick via pendingRelease, and a fresh press cancels a
-- pending one. This is a real bug class, not a test artefact - it is exactly
-- what a snap turn or a tool swap does.
local function hold(action)
	state.pendingRelease[action] = nil
	if not state.injected[action] then
		state.injected[action] = true
		bind(action, action)
		state.heldSince[action] = state.frames
	end
end

local function release(action)
	if state.injected[action] then
		state.pendingRelease[action] = true
	end
end

-- Called at the top of tick: a press scheduled last frame is now old enough to
-- be released, and any action that has not been re-pressed stays down.
local function serviceReleases()
	for action in pairs(state.pendingRelease) do
		state.injected[action] = nil
		state.pendingRelease[action] = nil
		state.heldSince[action] = nil
		unbind(action)
	end
end

-- ---------------------------------------------------------------------------
-- lifecycle
-- ---------------------------------------------------------------------------

-- GetLocalPlayer() raises "before init is not supported" if the mod starts
-- too early, so it is guarded and degrades to player 0.
local function localPlayer()
	local p = 0
	local ok = pcall(function() p = GetLocalPlayer() end)
	if not ok or p == nil then return 0 end
	return p
end

local function init()
	readConfig()
	state.player = localPlayer()
	-- snapshot the current scheme so the overlay and any debug output can show
	-- what the game thinks is bound, rather than what we assume.
	for _, a in pairs(ACTION) do
		state.bindings[a] = bindingOf(a)
	end
	state.lastHmdOk = true
end

local function tick(dt)
	if not state.enabled then
		return
	end
	-- GetTimeStep() is a fixed 1/60 inside update() but the real frame time in
	-- tick(), and the game calls tick() with no argument at all. So nil is the
	-- normal case, not an error case, and the fallback has to be the real
	-- frame time rather than a hardcoded constant.
	if dt == nil or dt <= 0 or dt > 1 then
		dt = GetTimeStep()
		if dt == nil or dt <= 0 or dt > 1 then
			-- a paused frame or a first frame; 1/60 is the engine's own fixed
			-- step so the accumulator cannot run away
			dt = 1 / 60
		end
	end
	state.frames = state.frames + 1
	serviceReleases()

	if state.hands then
		-- Snap turn. The accumulator is in radians and is advanced by the
		-- DEADZONED yaw, because raw stick noise is what makes snap turn
		-- drift. Firing resets only the amount consumed, so a large held
		-- rotation still turns repeatedly instead of once.
		local yaw = dz(axis("headyaw"))
		local threshold = math.rad(state.snap)
		if math.abs(yaw) > 0.001 then
			-- scaled by dt so the turn rate is the same at 60 and 144 Hz;
			-- without this, snap turn speed is a property of the framerate
			state.handYaw = state.handYaw + yaw * dt
		end
		if state.handYaw >= threshold then
			hold(ACTION.MENU_RIGHT)
			release(ACTION.MENU_RIGHT)
			state.handYaw = state.handYaw - threshold
		elseif state.handYaw <= -threshold then
			hold(ACTION.MENU_LEFT)
			release(ACTION.MENU_LEFT)
			state.handYaw = state.handYaw + threshold
		end
	end

	-- discrete buttons map straight through
	if channel("trigger") > 0.5 then
		hold(ACTION.USETOOL)
	else
		release(ACTION.USETOOL)
	end
	if channel("grip") > 0.5 then
		hold(ACTION.GRAB)
	else
		release(ACTION.GRAB)
	end
	if channel("a") > 0.5 then
		hold(ACTION.INTERACT)
	else
		release(ACTION.INTERACT)
	end
	if channel("b") > 0.5 then
		hold(ACTION.JUMP)
	else
		release(ACTION.JUMP)
	end

	-- report what the GAME sees, which is the only honest liveness signal
	local gameSeesAny = false
	for _, a in pairs(ACTION) do
		if InputDown(a) then
			gameSeesAny = true
			break
		end
	end
	state.lastHmdOk = gameSeesAny
end

local function draw()
	if not state.debug then
		return
	end
	UiPush()
	UiAlign("left top")
	UiFont("regular.ttf", 14)
	UiColor(1, 1, 1, 0.85)
	UiText(string.format("VR layer: %s  frames %d  dt %.4f",
		state.enabled and "on" or "off", state.frames, GetTimeStep()), 12, 12)
	UiText(string.format("bindings held: %d", (function()
		local n = 0
		for _ in pairs(state.injected) do n = n + 1 end
		return n
	end)()), 12, 30)
	UiPop()
end

-- The lifecycle callbacks above are declared as globals because that is the
-- shape the game expects -- but that is also the shape that collides. Every
-- mod in this folder defines init/tick/draw, so whichever file is probed LAST
-- wins and silently replaces the mod's own top-level entrypoint with an
-- unrelated module's callback. vrCandidates is iterated with pairs(), whose
-- order is undefined, so which file wins changes between runs.
--
-- Owning the global is correct for a stand-alone mod; owning it from a
-- library module is not. Keep the three names local, and publish the module
-- table under a namespaced global that main.lua can call explicitly. The
-- local copies are aliased below so the bodies stay readable.
local M = {
	ACTION = ACTION,
	state = state,
	bind = bind,
	unbind = unbind,
	bindingOf = bindingOf,
	readConfig = readConfig,
	-- the lifecycle callbacks are globals in the file, because that is how the
	-- game invokes them; expose them here too so a test does not have to reach
	-- into _G and so it fails loudly if they ever stop being defined
	init = init,
	tick = tick,
	draw = draw,
}

_G.vrinput = M

-- No `return M` here. merge.py concatenates this file with the others and with
-- main.lua, and a `return` in the middle of that concatenation makes everything
-- after it unreachable: the merged build would parse as "file ends right after
-- the return", which is exactly the `'<eof>' expected` error luac reports.
--
-- Assigning the global is enough. The test harness reads _G.vrinput, and the
-- game only ever calls the mod's own entrypoint.
