--[[===========================================================================
  test_run.lua -- executes the mod against the mock API and asserts behaviour.

  Run:  lua5.1 test/test_run.lua

  This is a real test, not a syntax check. It drives the mod through damage,
  falls, landings, tool fire, impacts and vehicle transitions and asserts that
  the right event ids appear in the ring buffer with sane amplitudes, that the
  rate limiter works, and that nothing throws.
============================================================================]]

package.path = "./?.lua;" .. package.path

local M = dofile("test/mock_teardown.lua")
M.moddir = "."

local G = _G
M.install(G)
-- Shadowing dofile in the global env breaks the test file's own loads; put
-- the real one back and let the test use it directly.
local load_file = M.real_dofile

--[[---------------------------------------------------------------------------
  Tiny assertion harness
-------------------------------------------------------------------------------]]
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

local function eq(a, b, name)
  ok(a == b, name, string.format("expected %s, got %s", tostring(b), tostring(a)))
end

local function section(t) print("\n== " .. t .. " " .. string.rep("=", math.max(4, 66 - #t))) end

--[[---------------------------------------------------------------------------
  Load the mod
-------------------------------------------------------------------------------]]
section("loading")
load_file("main.lua")
G.init()

-- Rate-limiter reset, used by sections that need a fresh event budget.
local function resetLimiter() vrhap._last = {} end

-- main.lua iterates vrCandidates with pairs(), so probe order is NOT
-- deterministic. Assert that every module was probed, not that a particular one
-- came first: with four modules the previous "bus is first" assertion was
-- checking a property of the hash order, not of the loader.
local probed = {}
for _, p in ipairs(M.dofile_attempts) do probed[p] = true end
ok(probed["MOD/vrbus.lua"] == true, "vrbus was probed",
   table.concat(M.dofile_attempts, ", "))
ok(probed["MOD/vrhaptics.lua"] == true, "vrhaptics was probed")
ok(probed["MOD/vrcamera.lua"] == true, "vrcamera was probed")
ok(probed["MOD/vrinput.lua"] == true, "vrinput was probed")
ok(vrModules.bus == true, "vrbus loaded")
ok(vrModules.haptics == true, "vrhaptics loaded")
ok(vrModules.camera == true, "vrcamera loaded")
ok(vrModules.input == true, "vrinput loaded",
   tostring(vrLoadErrors and vrLoadErrors.input))
ok(vrModules.all == true, "all modules loaded")
ok(next(M.log) ~= nil and M.log[1]:find("teardown_vr"), "init logged via DebugPrint")

eq(M.store_int["vr.ev.version"], 1, "protocol version published")
eq(M.store_int["vr.ev.count"], 0, "ring starts empty")

--[[---------------------------------------------------------------------------
  Camera block
-------------------------------------------------------------------------------]]
section("camera / stereo block")
M.world.pos = { 1.0, 2.0, 3.0 }
M.runFrames(G, 2)
eq(M.store_int["vr.cam.valid"], 1, "cam.valid set")
eq(M.store_float["vr.cam.x"], 1.0, "cam.x tracks player")
eq(M.store_float["vr.cam.y"], 2.0, "cam.y tracks player")
eq(M.store_float["vr.cam.z"], 3.0, "cam.z tracks player")
ok(M.store_float["vr.cam.fov"] > 0, "cam.fov positive")
ok(M.store_float["vr.cam.ipd"] > 0, "cam.ipd positive")
ok(M.store_float["vr.cam.ipd"] >= 0.062 and M.store_float["vr.cam.ipd"] <= 0.066,
   "cam.ipd near 64mm default", tostring(M.store_float["vr.cam.ipd"]))
eq(M.store_int["vr.cam.suppress"], 0, "not suppressed on ground")

-- yaw should tilt the right axis off world x
M.world.yaw = math.pi / 2   -- face +x
M.runFrames(G, 1)
local rx, rz = M.store_float["vr.cam.rx"], M.store_float["vr.cam.rz"]
ok(math.abs(math.sqrt(rx*rx + rz*rz) - 1.0) < 1e-4, "right axis is unit length",
   string.format("%.4f,%.4f", rx, rz))
ok(math.abs(rx) < 0.2, "right axis follows yaw (rx ~ 0 when facing +x)",
   string.format("rx=%.4f rz=%.4f", rx, rz))
M.world.yaw = 0.0

-- suppression in vehicle
M.world.vehicle = 7
M.runFrames(G, 1)
eq(M.store_int["vr.cam.suppress"], 1, "suppressed while in vehicle")
M.world.vehicle = 0
M.runFrames(G, 1)
eq(M.store_int["vr.cam.suppress"], 0, "unsuppressed after exiting vehicle")

--[[---------------------------------------------------------------------------
  Heartbeat
-------------------------------------------------------------------------------]]
section("heartbeat")
local hb0 = M.store_int["vr.hb"] or 0
M.runFrames(G, 120)   -- 2 seconds at 1/60
ok((M.store_int["vr.hb"] or 0) > hb0, "heartbeat advanced",
   string.format("%s -> %s", tostring(hb0), tostring(M.store_int["vr.hb"])))

--[[---------------------------------------------------------------------------
  No spurious events on level load
-------------------------------------------------------------------------------]]
section("no false events at rest")
-- Start from a known-still state: earlier sections move the player and change
-- health, and leftover velocity would legitimately produce a footstep.
M.world.pos   = { 0.0, 0.0, 0.0 }
M.world.vel   = { 0.0, 0.0, 0.0 }
M.world.grounded = true
M.world.groundDist = 0.0
M.world.health = 1.0
vrhap:init()                 -- re-prime, as a level load would
M.clearEvents()
local emitted0 = vrhap._stats.emitted
M.runFrames(G, 60)
eq(#M.events(), 0, "no events while standing still")
eq(vrhap._stats.emitted, emitted0, "emitter count unchanged at rest",
   string.format("%d -> %d", emitted0, vrhap._stats.emitted))

--[[---------------------------------------------------------------------------
  Damage
-------------------------------------------------------------------------------]]
section("damage detection")
M.clearEvents()
M.world.health = 1.0
M.runFrames(G, 2)
M.world.health = 0.90          -- 0.10 lost, epsilon is 0.01
M.runFrames(G, 1)
local ev = M.findEvents(1)     -- VRBUS_EV_DAMAGE_TAKEN
ok(#ev >= 1, "DAMAGE_TAKEN emitted on health drop", "count=" .. #ev)
if #ev >= 1 then
  ok(ev[#ev].amp > 0.2, "damage amplitude scaled up", "amp=" .. tostring(ev[#ev].amp))
  ok(ev[#ev].amp <= 1.0, "damage amplitude <= 1")
  ok(ev[#ev].ttl > 0, "damage carries a duration", "ttl=" .. tostring(ev[#ev].ttl))
end

-- sub-epsilon damage must be ignored
M.clearEvents()
M.world.health = 0.899
M.runFrames(G, 1)
eq(#M.events(), 0, "sub-epsilon damage ignored")

-- big hit saturates at 1. Step health down gradually past the rate limiter,
-- since a 0.8 jump in one frame is exactly what a death looks like.
M.clearEvents()
for step = 1, 6 do
  M.world.health = 1.0 - (0.2 * step)
  M.runFrames(G, 2)
end
local big = M.findEvents(1)
ok(#big >= 1, "large damage produced events", "count=" .. #big)
if #big >= 1 then
  ok(big[#big].amp <= 1.0, "large damage clamped to 1.0", tostring(big[#big].amp))
  ok(big[#big].amp > 0.5, "large damage reaches near full scale", tostring(big[#big].amp))
end
M.world.health = 1.0
M.runFrames(G, 2)
M.clearEvents()

--[[---------------------------------------------------------------------------
  Landing
-------------------------------------------------------------------------------]]
section("landing detection")
M.clearEvents()
M.world.pos   = { 0.0, 50.0, 0.0 }
M.world.vel   = { 0.0, -8.0, 0.0 }
M.world.grounded = false
M.runFrames(G, 20)            -- fall, building energy
eq(#M.findEvents(3), 0, "no landing while airborne")

M.world.grounded = true       -- touchdown
M.world.vel = { 0.0, 0.0, 0.0 }
M.runFrames(G, 1)
local land = M.findEvents(3)  -- VRBUS_EV_LANDING
ok(#land >= 1, "LANDING emitted on touchdown", "count=" .. #land)
if #land >= 1 then
  ok(land[#land].amp > 0.0, "landing amplitude > 0")
  ok(land[#land].ttl > 0, "landing has duration")
end

-- rate limit: repeated touchdowns collapse
M.clearEvents()
for _ = 1, 10 do
  M.world.grounded = false
  M.runFrames(G, 2)
  M.world.grounded = true
  M.runFrames(G, 1)
end
ok(#M.findEvents(3) < 10, "landing rate limiter engaged",
   "#landings=" .. #M.findEvents(3))

-- small hop should not trigger a landing
M.clearEvents()
M.world.pos = { 0.0, 5.0, 0.0 }
M.world.grounded = false
M.world.vel = { 0.0, -1.0, 0.0 }
M.runFrames(G, 5)
M.world.grounded = true
M.runFrames(G, 1)
eq(#M.findEvents(3), 0, "sub-threshold fall produces no landing")

--[[---------------------------------------------------------------------------
  Jump
-------------------------------------------------------------------------------]]
section("jump detection")
M.clearEvents()
M.world.pos = { 0.0, 1.0, 0.0 }
M.world.vel = { 0.0, 6.0, 0.0 }
M.world.grounded = true
M.runFrames(G, 1)
M.world.grounded = false
M.runFrames(G, 3)
ok(#M.findEvents(11) >= 1, "JUMP emitted on upward launch", "count=" .. #M.findEvents(11))

--[[---------------------------------------------------------------------------
  Footsteps
-------------------------------------------------------------------------------]]
section("footsteps")
M.clearEvents()
M.world.pos = { 0.0, 0.0, 0.0 }
M.world.vel = { 0.0, 0.0, 3.0 }
M.world.grounded = true
M.runFrames(G, 120)
ok(#M.findEvents(12) >= 1, "FOOTSTEP emitted while walking",
   "count=" .. #M.findEvents(12))

--[[---------------------------------------------------------------------------
  Tool fire
-------------------------------------------------------------------------------]]
section("tool fire")
M.clearEvents()
M.world.tool = "gun"
M.world.ammo = 100
M.runFrames(G, 2)
M.world.ammo = 98
M.runFrames(G, 1)
local fires = M.findEvents(5)  -- VRBUS_EV_TOOL_FIRE
ok(#fires >= 1, "TOOL_FIRE emitted on ammo drop", "count=" .. #fires)
if #fires >= 1 then
  eq(fires[#fires].hand, 2, "tool fire routed to right hand")
end

-- tool switch
M.clearEvents()
M.world.tool = "rifle"
M.runFrames(G, 1)
ok(#M.findEvents(6) >= 1, "TOOL_SWITCH emitted on tool change")

--[[---------------------------------------------------------------------------
  Tool impact
-------------------------------------------------------------------------------]]
section("tool impact (undocumented hit API)")
M.clearEvents()
M.world.toolBody = 500
M.world.toolBodyHit = true
M.world.toolBodyHits = { { other=501, impulse=300.0, pos={1,2,3}, normal={0,1,0} } }
M.runFrames(G, 1)
local imp = M.findEvents(7)   -- VRBUS_EV_TOOL_IMPACT
ok(#imp >= 1, "TOOL_IMPACT emitted from hit impulse", "count=" .. #imp)
if #imp >= 1 then
  ok(imp[#imp].amp > 0, "impact amplitude > 0")
  ok((imp[#imp].flags % 2) == 1, "SURFACE flag set when material known",
     "flags=" .. tostring(imp[#imp].flags))
  eq(imp[#imp].x, 1.0, "impact carries world position")
end
M.world.toolBodyHit = false
M.world.toolBodyHits = {}

-- tiny impulse below the noise floor is dropped
M.clearEvents()
M.world.toolBody = 500
M.world.toolBodyHit = true
M.world.toolBodyHits = { { other=501, impulse=1.0, pos={0,0,0} } }
M.runFrames(G, 1)
eq(#M.findEvents(7), 0, "negligible impulse produces no impact event")
M.world.toolBodyHit = false
M.world.toolBodyHits = {}

--[[---------------------------------------------------------------------------
  Vehicle enter / exit
-------------------------------------------------------------------------------]]
section("vehicle transitions")
M.clearEvents()
M.world.vehicle = 0
M.runFrames(G, 2)
M.world.vehicle = 12
M.runFrames(G, 1)
ok(#M.findEvents(14) >= 1, "VEHICLE_ENTER emitted")
M.world.vehicle = 0
M.runFrames(G, 1)
ok(#M.findEvents(15) >= 1, "VEHICLE_EXIT emitted")

--[[---------------------------------------------------------------------------
  Ring buffer mechanics
-------------------------------------------------------------------------------]]
section("ring buffer")
M.clearEvents()
M.world.health = 1.0
M.runFrames(G, 2)
-- hammer the bus directly to overflow the 16-slot ring
M.store_int["vr.ev.seq_before"] = M.store_int["vr.ev.seq"] or 0
for i = 1, 40 do
  M.world.health = 1.0 - (0.05 * (i % 2))
  M.runFrames(G, 2)
end
ok(M.store_int["vr.ev.count"] <= 16, "ring never exceeds slot count",
   "count=" .. tostring(M.store_int["vr.ev.count"]))
local seqNow = M.store_int["vr.ev.seq"] or 0
local seqPrev = M.store_int["vr.ev.seq_before"] or 0
ok(seqNow >= seqPrev, "seq is monotonic", string.format("%s -> %s", tostring(seqPrev), tostring(seqNow)))

--[[---------------------------------------------------------------------------
  Robustness: API failures must not propagate
-------------------------------------------------------------------------------]]
section("robustness against API failure")
M.clearEvents()
local realHealth = G.GetPlayerHealth
G.GetPlayerHealth = function() error("synthetic failure") end
local okp, err = pcall(G.tick, 1.0/60.0)
G.GetPlayerHealth = realHealth
ok(okp, "tick survives a throwing GetPlayerHealth", tostring(err))

G.GetPlayerVehicle = function() error("synthetic vehicle failure") end
ok(pcall(G.tick, 1.0/60.0), "tick survives a throwing GetPlayerVehicle")
G.GetPlayerVehicle = function() return M.world.vehicle end

G.GetToolBody = function() error("synthetic tool failure") end
ok(pcall(G.tick, 1.0/60.0), "tick survives a throwing GetToolBody")
G.GetToolBody = function() return M.world.toolBody end

--[[---------------------------------------------------------------------------
  Game-side haptic playback
-------------------------------------------------------------------------------]]
section("game-side haptic playback")
vrhap.playGameHaptics = true
M.haptics_played = {}
M.mod_assets_available = true      -- mod-local haptic/*.xml resolvable
vrhap.handles = {}                 -- force a fresh LoadHaptic
resetLimiter()
M.clearEvents()
M.world.health = 1.0
M.runFrames(G, 2)
M.world.health = 0.85
M.runFrames(G, 1)
ok(#M.haptics_played >= 1, "PlayHaptic called when game haptics enabled",
   "count=" .. #M.haptics_played)
if #M.haptics_played >= 1 then
  ok(M.haptics_played[1].amp > 0 and M.haptics_played[1].amp <= 1,
     "played amplitude in 0..1", tostring(M.haptics_played[1].amp))
  ok(M.haptics_played[1].h ~= nil, "played a real handle", tostring(M.haptics_played[1].h))
end
vrhap.playGameHaptics = false
eq(vrhap._stats.loadFail, 0, "no haptic asset load failures",
   "failures=" .. tostring(vrhap._stats.loadFail))


section("haptic assets resolve")
-- Handles are only ever loaded inside play(), which runs solely when
-- game-pad playback is on. Both of these cases are about that path.
vrhap.playGameHaptics = true
M.haptics_loaded = {}
vrhap.handles = {}            -- drop memoised handles so LoadHaptic runs again
vrhap._stats.loadFail = 0
resetLimiter()
M.mod_assets_available = true
M.world.health = 1.0
M.runFrames(G, 2)
M.world.health = 0.9
M.runFrames(G, 1)
local nLoaded = 0
for _ in pairs(vrhap.handles) do nLoaded = nLoaded + 1 end
ok(nLoaded > 0, "catalog assets were loaded", "count=" .. tostring(nLoaded))
eq(vrhap._stats.loadFail, 0, "all catalog assets load when mod root is mounted",
   "failures=" .. tostring(vrhap._stats.loadFail))

-- With the mod asset root unavailable, every mod-local load must fail cleanly
-- and the mod must keep running -- haptics degrade, nothing crashes.
M.haptics_loaded = {}
vrhap.handles = {}
vrhap._stats.loadFail = 0
resetLimiter()
M.mod_assets_available = false
M.world.health = 1.0
M.runFrames(G, 2)
M.world.health = 0.9
M.runFrames(G, 1)
ok(vrhap._stats.loadFail > 0, "missing assets are counted, not fatal",
   "failures=" .. tostring(vrhap._stats.loadFail))
ok(#M.findEvents(1) >= 1, "bus events still emitted with no haptic assets")
ok(pcall(G.tick, 1.0/60.0), "tick survives with zero loadable haptic assets")
vrhap.playGameHaptics = false
M.mod_assets_available = true
vrhap.handles = {}
vrhap._stats.loadFail = 0
resetLimiter()

--[[---------------------------------------------------------------------------
  API surface audit: nothing the mod calls that the game does not have
-------------------------------------------------------------------------------]]
section("API surface audit")
M.calls = {}
M.runFrames(G, 30)
G.draw()

-- Every global the mod called, that it does not itself define.
local own = {}
for _, name in ipairs({"init","tick","update","draw","vrLoadModules","tryLoad",
                       "vrDrawOverlay","vrAdjustFov","vrAdjustScale"}) do
  own[name] = true
end
local KNOWN_API = {
  HasKey=1,SetInt=1,GetInt=1,SetFloat=1,GetFloat=1,SetBool=1,GetBool=1,
  SetString=1,GetString=1,SetValue=1,DebugPrint=1,HasFile=1,dofile=1,
  GetTime=1,GetTimeStep=1,
  Vec=1,VecAdd=1,VecSub=1,VecScale=1,VecLength=1,VecNormalize=1,VecDot=1,
  Transform=1,QuatRotateVec=1,TransformStr=1,
  GetLocalPlayer=1,IsPlayerValid=1,GetPlayerCount=1,IsPlayerLocal=1,
  IsPlayerHost=1,GetPlayerName=1,GetPlayerHealth=1,SetPlayerHealth=1,
  GetPlayerPos=1,GetPlayerVelocity=1,GetPlayerUp=1,GetPlayerPitch=1,
  GetPlayerYaw=1,GetPlayerTool=1,SetPlayerTool=1,GetPlayerVehicle=1,
  GetVehicleBody=1,GetToolAmmo=1,SetToolAmmo=1,IsToolEnabled=1,GetToolBody=1,
  GetPlayerEyeTransform=1,GetPlayerTransform=1,GetCameraTransform=1,
  SetCameraTransform=1,SetCameraFov=1,SetPlayerWalkingSpeed=1,
  GetPlayerWalkingSpeed=1,
  QueryRaycast=1,QueryAabbShapes=1,GetBodyVelocity=1,GetBodyTransform=1,
  GetBodyBounds=1,IsBodyBroken=1,IsBodyDynamic=1,IsBodyActive=1,
  GetShapeBody=1,GetShapeWorldTransform=1,GetShapeMaterialAtPosition=1,
  Explosion=1,ApplyPlayerDamage=1,
  WatchBodyHit=1,IsBodyHitted=1,GetBodyHitsCount=1,GetBodyHit=1,
  InputPressed=1,InputDown=1,InputReleased=1,InputValue=1,
  LoadHaptic=1,CreateHaptic=1,PlayHaptic=1,PlayHapticDirectional=1,
  HapticIsPlaying=1,StopHaptic=1,SetToolHaptic=1,
  UiPush=1,UiPop=1,UiText=1,UiColor=1,UiResetColor=1,UiTranslate=1,
  UiAlign=1,UiFont=1,UiRect=1,UiTextButton=1,
}
local unexpected = {}
for name in pairs(M.calls) do
  if not KNOWN_API[name] then unexpected[#unexpected+1] = name end
end
eq(#unexpected, 0, "mod calls no API outside the verified set",
   table.concat(unexpected, ", "))

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
