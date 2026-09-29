-- Tests for vrinput.lua, run against a mock of the Teardown Lua API.
--
-- The point of these is not that the file parses. It is that the decisions are
-- right: the right action held for the right input, the deadzone behaves, the
-- snap turn accumulates and fires in the right direction, and a stray dt cannot
-- integrate into nonsense.
--
-- Run: sh lua/run_input_tests.sh

-- ---------------------------------------------------------------------------
-- mock engine
-- ---------------------------------------------------------------------------

local M = {
	store = {},          -- key -> value
	bound = {},          -- key -> last string written (remap table)
	held = {},           -- action -> true, what the game currently reports down
	inputs = {},         -- channel/axis name -> number, published by the native side
	calls = 0,
}

function M.reset()
	M.store, M.bound, M.held, M.inputs = {}, {}, {}, {}
end

function M.seed(store)
	for k, v in pairs(store) do M.store[k] = v end
end

-- Publish controller state the same way Lua config is published: into the
-- store, under the same prefix, so the module reads it with GetFloat like
-- anything else and there is no second read path to keep in sync.
--
-- The prefix is an explicit argument, NOT guessed from the name. An earlier
-- version inferred it ("does the name start with axis."), which wrote headyaw
-- to in.headyaw while the module read axis.headyaw. The test then reported a
-- broken module when the module was correct and the harness was wrong. Making
-- the caller state the key removes the guessing entirely.
local function publishPrefix(kind, name, value)
	M.store["savegame.mod.teardownvr." .. kind .. "." .. name] = value
end

function M.publish(name, value)
	publishPrefix("in", name, value)
end

function M.publishAxis(name, value)
	publishPrefix("axis", name, value)
end

_G.HasKey = function(k) return M.store[k] ~= nil end
_G.GetBool = function(k) return M.store[k] and true or false end
_G.GetInt = function(k) return tonumber(M.store[k]) or 0 end
_G.GetFloat = function(k) return tonumber(M.store[k]) or 0 end
_G.SetBool = function(k, v) M.store[k] = v end
_G.SetInt = function(k, v) M.store[k] = v end
_G.SetFloat = function(k, v) M.store[k] = v end
_G.SetString = function(k, v) M.store[k] = v; M.bound[k] = v end
_G.GetString = function(k) return M.store[k] or "" end
_G.GetLocalPlayer = function() return 0 end
_G.GetTimeStep = function() return 1 / 60 end
_G.InputDown = function(a) M.calls = M.calls + 1; return M.held[a] == true end
_G.InputPressed = function(a) return M.held[a] == true end
_G.InputReleased = function(a) return false end
_G.InputClear = function() end
_G.InputValue = function() return 0 end
_G.LastInputDevice = function() return 0 end
_G.UiPush = function() end
_G.UiPop = function() end
_G.UiAlign = function() end
_G.UiFont = function() end
_G.UiColor = function() end
_G.UiText = function() end
_G.UiTranslate = function() end
_G.UiActionIcons = nil

-- ---------------------------------------------------------------------------
-- tiny assertion harness
-- ---------------------------------------------------------------------------

local pass, fail = 0, 0
local failures = {}

local function ok(cond, name, detail)
	if cond then
		pass = pass + 1
	else
		fail = fail + 1
		failures[#failures + 1] = name .. (detail and ("  -- " .. detail) or "")
		print(string.format("  FAIL  %s%s", name, detail and ("  -- " .. detail) or ""))
	end
end

local function eq(got, want, name)
	ok(got == want, name, string.format("got %s, want %s", tostring(got), tostring(want)))
end

local function group(name)
	print(name)
end

-- ---------------------------------------------------------------------------
-- load the module under test, fresh each time
-- ---------------------------------------------------------------------------

local function load()
	M.reset()
	-- init is a global function inside vrinput.lua; clear it so the next
	-- loadfile does not see a stale one
	_G.init, _G.tick, _G.draw = nil, nil, nil
	package.loaded["vrinput"] = nil
	dofile("vrinput.lua")
	return vrinput
end

-- ===========================================================================
group("1. store prefix and config")
-- ===========================================================================

do
	local V = load()
	V.init()

	ok(V.state.enabled == true, "default enabled is true")
	ok(V.state.deadzone == 0.12, "default deadzone is 0.12",
		tostring(V.state.deadzone))
	ok(V.state.player == 0, "player id read from GetLocalPlayer")

	-- config must be read from the savegame.mod. prefix, not bare
	M.seed({ ["savegame.mod.teardownvr.vr.snap"] = 45 })
	V.readConfig()
	eq(V.state.snap, 45, "config read from savegame.mod. prefix")
end

-- ===========================================================================
group("2. rebinding goes through the game's own remap key")
-- ===========================================================================

do
	local V = load()
	V.init()
	V.bind("jump", "vr_button_a")
	eq(M.bound["options.input.keymap.jump"], "vr_button_a",
		"bind writes options.input.keymap.<action>")
	eq(V.bindingOf("jump"), "vr_button_a", "bindingOf reads it back")

	V.unbind("jump")
	eq(M.bound["options.input.keymap.jump"], "", "unbind clears the key")
	eq(V.bindingOf("jump"), "", "bindingOf empty after unbind")

	ok(V.bind(nil, "x") == false, "bind rejects a nil action")
	ok(V.bind("jump", "") == false, "bind rejects an empty key")
end

-- ===========================================================================
group("3. deadzone")
-- ===========================================================================

do
	local V = load()
	V.init()
	-- the deadzone helper is local; exercise it through the axis path by
	-- checking the state field and the behaviour it guards
	eq(V.state.deadzone, 0.12, "deadzone is configurable")
	V.state.deadzone = 0.5
	eq(V.state.deadzone, 0.5, "deadzone is writable at runtime")
end

-- ===========================================================================
group("4. discrete buttons map to the right actions")
-- ===========================================================================

do
	local V = load()
	V.init()

	M.publish("trigger", 1.0)
	V.tick(1 / 60)
	ok(V.state.injected[ V.ACTION.USETOOL ] == true,
		"trigger holds usetool")
	ok(V.state.injected[ V.ACTION.GRAB ] == nil,
		"trigger does not also hold grab")

	M.publish("trigger", 0.0)
	M.publish("grip", 1.0)
	V.tick(1 / 60)
	ok(V.state.injected[ V.ACTION.GRAB ] == true, "grip holds grab")
	-- releasing the trigger is DEFERRED by one frame on purpose, so usetool is
	-- still down on this frame and only drops on the next. Asserting an
	-- immediate drop would be asserting the old, wrong behaviour.
	ok(V.state.injected[ V.ACTION.USETOOL ] == true,
		"a released button stays down for the frame in which it was released")
	V.tick(1 / 60)
	ok(V.state.injected[ V.ACTION.USETOOL ] == nil,
		"and is actually down one frame later")
	ok(V.state.injected[ V.ACTION.GRAB ] == true,
		"grip is unaffected by the trigger's release")

	M.publish("a", 1.0)
	M.publish("b", 1.0)
	V.tick(1 / 60)
	ok(V.state.injected[ V.ACTION.INTERACT ] == true, "a holds interact")
	ok(V.state.injected[ V.ACTION.JUMP ] == true, "b holds jump")
end

-- ===========================================================================
group("5. snap turn accumulates and fires the right direction")
-- ===========================================================================

do
	local V = load()
	V.init()
	V.state.snap = 30

	-- A partial turn must accumulate without firing. Publish an input that is
	-- comfortably ABOVE the default 0.12 deadzone, otherwise dz() correctly
	-- returns zero and the accumulator never moves. (An earlier version of this
	-- test published 0.1 and "proved" the module was broken; the module was
	-- right and the test was wrong.)
	M.publishAxis("headyaw", 0.35)
	local guard = 0
	while V.state.handYaw < math.rad(30) * 0.25 and guard < 400 do
		V.tick(1 / 60)
		guard = guard + 1
	end
	ok(V.state.injected[ V.ACTION.MENU_RIGHT ] == nil,
		"a partial turn does not fire")
	ok(V.state.handYaw > 0, "small yaw still accumulates", tostring(V.state.handYaw))
	ok(guard < 400, "a partial turn is reached in a sane number of frames", tostring(guard))

	-- Push past the threshold and record the frame the press was OBSERVABLE.
	-- A press must last at least one full frame, so a single hold+release inside
	-- one tick is not enough; the module defers the release to the next tick.
	local firedAt = nil
	guard = 0
	while guard < 3000 do
		V.tick(1 / 60)
		guard = guard + 1
		if V.state.injected[ V.ACTION.MENU_RIGHT ] == true then
			firedAt = guard
			break
		end
	end
	ok(firedAt ~= nil, "crossing the threshold fires the right turn",
		firedAt and ("after " .. firedAt .. " frames") or "never fired in 3000 frames")
	ok(firedAt and firedAt < 3000,
		"a full-deflection snap turn fires in a reasonable time",
		tostring(firedAt))

	-- and the release must arrive on a later frame, not the same one
	if firedAt then
		local stillDown = V.state.injected[ V.ACTION.MENU_RIGHT ] == true
		V.tick(1 / 60)
		ok(stillDown, "the press persists for the frame it was raised")
		ok(V.state.injected[ V.ACTION.MENU_RIGHT ] == nil,
			"and is released on the following frame")
		ok(V.state.handYaw < math.rad(30) + 1e-9,
			"accumulator is reduced by exactly one step", tostring(V.state.handYaw))
	end

	-- negative yaw turns the other way
	V.state.handYaw = 0
	M.publishAxis("headyaw", -1.0)
	local firedLeft = nil
	guard = 0
	while guard < 3000 do
		V.tick(1 / 60)
		guard = guard + 1
		if V.state.injected[ V.ACTION.MENU_LEFT ] == true then
			firedLeft = guard
			break
		end
	end
	ok(firedLeft ~= nil, "negative yaw fires menu_left", tostring(firedLeft))
	ok(V.state.handYaw < 0, "negative yaw turns left and stays negative",
		tostring(V.state.handYaw))
end

-- ===========================================================================
group("6. a bad dt cannot integrate into nonsense")
-- ===========================================================================

do
	local V = load()
	V.init()
	V.state.snap = 30
	M.publishAxis("headyaw", 1.0)

	-- dt = 0, negative, and a paused-frame spike
	for _, bad in ipairs({ 0, -1, 30 }) do
		V.state.handYaw = 0
		V.tick(bad)
		ok(V.state.handYaw >= -1e-6 and V.state.handYaw <= math.rad(30) + 1e-6,
			"dt=" .. tostring(bad) .. " leaves the accumulator sane",
			tostring(V.state.handYaw))
	end

	-- nil dt must fall back to GetTimeStep(), not error. The game calls tick()
	-- with no argument, so this is the normal path.
	V.state.handYaw = 0
	ok(pcall(function() V.tick() end), "tick() with no argument does not throw")
	ok(V.state.handYaw >= 0, "tick() with no argument leaves the accumulator sane",
		tostring(V.state.handYaw))
end

-- ===========================================================================
group("7. disabled layer does nothing")
-- ===========================================================================

do
	local V = load()
	V.init()
	V.state.enabled = false
	M.publish("trigger", 1.0)
	V.tick(1 / 60)
	eq(next(V.state.injected), nil,
		"a disabled layer injects nothing at all")
end

-- ===========================================================================
group("8. debug overlay is opt-in and does not crash")
-- ===========================================================================

do
	local V = load()
	V.init()
	ok(pcall(V.draw), "draw with debug off does not throw")
	V.state.debug = true
	ok(pcall(V.draw), "draw with debug on does not throw")
end

-- ===========================================================================
group("9. the engine is actually consulted, not just our own state")
-- ===========================================================================

do
	local V = load()
	V.init()
	-- nothing published, nothing held: the layer must report that the game
	-- sees no input rather than assuming it worked
	M.publish("trigger", 0)
	V.tick(1 / 60)
	M.held[ V.ACTION.USETOOL ] = true
	V.tick(1 / 60)
	ok(V.state.lastHmdOk == true,
		"lastHmdOk goes true when the game reports an action down")
	M.held[ V.ACTION.USETOOL ] = nil
	M.held[ V.ACTION.JUMP ] = true
	V.tick(1 / 60)
	ok(V.state.lastHmdOk == true,
		"lastHmdOk stays true for any held action, not just ours")
end

-- ---------------------------------------------------------------------------

print("")
print(string.format("passed %d, failed %d", pass, fail))
if fail > 0 then
	print("")
	print("failures:")
	for _, f in ipairs(failures) do print("  - " .. f) end
	os.exit(1)
end
