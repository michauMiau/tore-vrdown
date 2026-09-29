--[[===========================================================================
  mock_teardown.lua -- a mock of the Teardown Lua API, for testing off-game.

  This is a TEST HARNESS, not part of the shipped mod. It implements the
  subset of the API the mod touches, with the same signatures as
  data/script_defs.luau, plus an instrumented version of the body-hit watch
  API so the impact path can actually be exercised.

  Run:  lua5.1 test/mock_teardown.lua
============================================================================]]

local M = {}

--[[---------------------------------------------------------------------------
  The key store. Same semantics as the real one: flat, string-keyed, typed.
-------------------------------------------------------------------------------]]
M.store_int    = {}
M.store_float  = {}
M.store_bool   = {}
M.store_string = {}

--[[---------------------------------------------------------------------------
  Call counters, so tests can assert a function was actually used.
-------------------------------------------------------------------------------]]
M.calls = {}
local function bump(name)
  M.calls[name] = (M.calls[name] or 0) + 1
end
M.bump = bump

--[[---------------------------------------------------------------------------
  Simulated world state
-------------------------------------------------------------------------------]]
M.world = {
  time      = 0.0,
  health    = 1.0,
  pos       = { 0.0, 0.0, 0.0 },
  vel       = { 0.0, 0.0, 0.0 },
  up        = { 0.0, 1.0, 0.0 },
  pitch     = 0.0,
  yaw       = 0.0,
  tool      = "gun",
  ammo      = 100,
  vehicle   = 0,
  grounded  = true,
  groundDist = 0.0,     -- distance the down-probe reports
  localPlayer = 0,
  playerValid = true,
  -- Body-hit watch state
  toolBody   = 0,
  toolBodyHit = false,
  toolBodyHits = {},    -- list of {other=, impulse=, pos=, normal=}
  inputs     = {},      -- name -> true (InputPressed returns this)
}

--[[---------------------------------------------------------------------------
  Loads. dofile() records every path a caller tries, so main.lua's module
  probe can be observed. Set M.dofile_ok[path] = true to let a path succeed.
-------------------------------------------------------------------------------]]
M.dofile_attempts = {}
M.dofile_ok = {
  ["MOD/vrbus.lua"]=true,
  ["MOD/vrhaptics.lua"]=true,
  ["MOD/vrcamera.lua"]=true,
  ["MOD/vrinput.lua"]=true,
}
M.log = {}
M.haptics_loaded = {}
M.haptics_played = {}

--[[---------------------------------------------------------------------------
  API implementations
-------------------------------------------------------------------------------]]
function M.install(G)
  -- --- key store, script_defs.luau:630-730 ---------------------------------
  G.HasKey = function(key)
    return M.store_int[key] ~= nil or M.store_float[key] ~= nil
        or M.store_bool[key] ~= nil or M.store_string[key] ~= nil
  end
  G.SetInt = function(k, v) bump("SetInt");   M.store_int[k] = v end
  G.GetInt = function(k) bump("GetInt");   return M.store_int[k] or 0 end
  G.SetFloat = function(k, v) bump("SetFloat"); M.store_float[k] = v end
  G.GetFloat = function(k) bump("GetFloat"); return M.store_float[k] or 0.0 end
  G.SetBool = function(k, v) bump("SetBool"); M.store_bool[k] = v end
  G.GetBool = function(k) bump("GetBool"); return M.store_bool[k] or false end
  G.SetString = function(k, v) bump("SetString"); M.store_string[k] = v end
  G.GetString = function(k) bump("GetString"); return M.store_string[k] or "" end
  G.SetValue = function(k, v) bump("SetValue"); M.store_float[k] = v end
  G.GetValue = function(k) return M.store_float[k] or 0.0 end

  G.DebugPrint = function(s) bump("DebugPrint"); M.log[#M.log+1] = tostring(s) end
  G.HasFile   = function(p) bump("HasFile"); return M.dofile_ok[p] == true end
  -- Capture the REAL dofile before shadowing it with the mock.
  local real_dofile = dofile
  G.dofile    = function(p)
    bump("dofile")
    M.dofile_attempts[#M.dofile_attempts+1] = p
    if M.dofile_ok[p] ~= true then
      error("cannot open " .. tostring(p), 2)
    end
    -- Resolve relative to the mod dir, stripping the MOD/ virtual root, so
    -- tests exercise the real module files.
    return real_dofile(M.moddir .. "/" .. (p:gsub("^MOD/", "")))
  end
  M.real_dofile = real_dofile

  -- --- time, script_defs.luau:138,157 ---------------------------------------
  G.GetTime = function() bump("GetTime"); return M.world.time end
  G.GetTimeStep = function() bump("GetTimeStep"); return 1.0/60.0 end

  -- --- vec/transform, :1047-1172, :1478 -------------------------------------
  local function vec(x, y, z) return { x or 0.0, y or 0.0, z or 0.0 } end
  G.Vec = vec
  G.VecAdd = function(a,b) bump("VecAdd"); return vec(a[1]+b[1], a[2]+b[2], a[3]+b[3]) end
  G.VecSub = function(a,b) bump("VecSub"); return vec(a[1]-b[1], a[2]-b[2], a[3]-b[3]) end
  G.VecScale = function(v,s) bump("VecScale"); return vec(v[1]*s, v[2]*s, v[3]*s) end
  G.VecLength = function(v) bump("VecLength"); return math.sqrt(v[1]^2+v[2]^2+v[3]^2) end
  G.VecNormalize = function(v)
    bump("VecNormalize")
    local l = G.VecLength(v)
    if l < 1e-9 then return vec() end
    return vec(v[1]/l, v[2]/l, v[3]/l)
  end
  G.VecDot = function(a,b) bump("VecDot"); return a[1]*b[1]+a[2]*b[2]+a[3]*b[3] end
  G.Transform = function(p, r) return { pos = p or vec(), rot = r or {0,0,0,1} } end
  G.QuatRotateVec = function(q, v) bump("QuatRotateVec"); return v end
  G.TransformStr = function(t) return "T" end

  -- --- player, script_defs.luau:5221-5470 -----------------------------------
  local W = M.world
  G.GetLocalPlayer = function() bump("GetLocalPlayer"); return W.localPlayer end
  G.IsPlayerValid = function(id) bump("IsPlayerValid"); return W.playerValid end
  G.GetPlayerCount = function() bump("GetPlayerCount"); return 1 end
  G.IsPlayerLocal = function(id) bump("IsPlayerLocal"); return true end
  G.IsPlayerHost = function(id) bump("IsPlayerHost"); return true end
  G.GetPlayerName = function(id) bump("GetPlayerName"); return "host" end
  G.GetPlayerHealth = function(id)
    bump("GetPlayerHealth"); return W.health end
  G.SetPlayerHealth = function(h, id) bump("SetPlayerHealth"); W.health = h end
  G.GetPlayerPos = function(id) bump("GetPlayerPos"); return vec(W.pos[1],W.pos[2],W.pos[3]) end
  G.SetPlayerPos = function(p, id) W.pos = {p[1],p[2],p[3]} end
  G.GetPlayerVelocity = function(id) bump("GetPlayerVelocity"); return vec(W.vel[1],W.vel[2],W.vel[3]) end
  G.SetPlayerVelocity = function(v, id) W.vel = {v[1],v[2],v[3]} end
  G.GetPlayerUp = function(id) bump("GetPlayerUp"); return vec(W.up[1],W.up[2],W.up[3]) end
  G.GetPlayerPitch = function(id) bump("GetPlayerPitch"); return W.pitch end
  G.GetPlayerYaw = function(id) bump("GetPlayerYaw"); return W.yaw end
  G.GetPlayerTool = function(id) bump("GetPlayerTool"); return W.tool end
  G.SetPlayerTool = function(t, id) W.tool = t end
  G.GetPlayerVehicle = function(id) bump("GetPlayerVehicle"); return W.vehicle end
  G.GetVehicleBody = function(v) return v * 100 end
  G.GetToolAmmo = function(t, id) bump("GetToolAmmo"); return W.ammo end
  G.SetToolAmmo = function(t,a,id) W.ammo = a end
  G.IsToolEnabled = function(t, id) return true end
  G.GetToolBody = function(id) bump("GetToolBody"); return W.toolBody end
  G.GetPlayerEyeTransform = function(id)
    bump("GetPlayerEyeTransform")
    return { pos = vec(W.pos[1],W.pos[2],W.pos[3]), rot = {0,0,0,1} }
  end
  G.GetPlayerTransform = function(id)
    bump("GetPlayerTransform")
    return { pos = vec(W.pos[1],W.pos[2],W.pos[3]), rot = {0,0,0,1} }
  end
  G.GetCameraTransform = function()
    bump("GetCameraTransform")
    return { pos = vec(W.pos[1],W.pos[2],W.pos[3]), rot = {0,0,0,1} }
  end
  G.SetCameraTransform = function(t, fov) bump("SetCameraTransform") end
  G.SetCameraFov = function(d) bump("SetCameraFov"); M.lastFov = d end
  G.SetPlayerWalkingSpeed = function(s, id) bump("SetPlayerWalkingSpeed") end
  G.GetPlayerWalkingSpeed = function(id) return 4.0 end

  -- --- physics, script_defs.luau:2010-2903 ---------------------------------
  G.QueryRaycast = function(o, d, maxDist, radius, rejectT)
    bump("QueryRaycast")
    if W.grounded then return true, W.groundDist, vec(0,1,0), 42 end
    return false, maxDist, vec(0,1,0), 0
  end
  G.QueryAabbShapes = function(a,b) bump("QueryAabbShapes"); return {} end
  G.GetBodyVelocity = function(b) bump("GetBodyVelocity"); return vec() end
  G.GetBodyTransform = function(b) bump("GetBodyTransform"); return {pos=vec(), rot={0,0,0,1}} end
  G.GetBodyBounds = function(b) bump("GetBodyBounds"); return vec(-1,-1,-1), vec(1,1,1) end
  G.IsBodyBroken = function(b) bump("IsBodyBroken"); return false end
  G.IsBodyDynamic = function(b) bump("IsBodyDynamic"); return true end
  G.IsBodyActive = function(b) bump("IsBodyActive"); return true end
  G.GetShapeBody = function(s) bump("GetShapeBody"); return 0 end
  G.GetShapeWorldTransform = function(s) bump("GetShapeWorldTransform"); return {pos=vec(),rot={0,0,0,1}} end
  G.GetShapeMaterialAtPosition = function(s, p, u)
    bump("GetShapeMaterialAtPosition"); return "wood", 0,0,0,0,0 end
  G.Explosion = function(p, s, id) bump("Explosion") end
  G.ApplyPlayerDamage = function(t, d, c, i) bump("ApplyPlayerDamage") end

  -- Body-hit watch API -- UNDOCUMENTED in the stub; this mirrors the call
  -- pattern used by mods/folkrace/m_village/script/{village_grinder,trash_hole,
  -- water_tower}.lua.
  G.WatchBodyHit = function(b) bump("WatchBodyHit") end
  G.IsBodyHitted = function(b) bump("IsBodyHitted"); return W.toolBodyHit end
  G.GetBodyHitsCount = function(b)
    bump("GetBodyHitsCount"); return #W.toolBodyHits end
  G.GetBodyHit = function(b, i)
    bump("GetBodyHit")
    local h = W.toolBodyHits[i+1]
    if h == nil then return nil end
    return h.other, h.impulse, h.pos or vec(), h.normal or vec(0,1,0),
           vec(), vec()
  end

  -- --- input, script_defs.luau:185-227 --------------------------------------
  G.InputPressed = function(i, id)
    bump("InputPressed"); return W.inputs[i] == true end
  G.InputDown = function(i, id) bump("InputDown"); return W.inputs[i] == true end
  G.InputReleased = function(i, id) bump("InputReleased"); return false end
  G.InputValue = function(i, id) bump("InputValue"); return 0.0 end

  -- --- haptics, script_defs.luau:8810-8927 ---------------------------------
  G.LoadHaptic = function(path)
    bump("LoadHaptic")
    -- Mirror the game: vanilla data/haptic/ paths resolve, mod-local .xml files
    -- resolve ONLY if the mod's own asset root is mounted. The latter is the
    -- unverified bit this harness makes configurable.
    local vanilla = {
      ["haptic/gun_fire.xml"]=true, ["haptic/gun_hit.xml"]=true,
      ["haptic/damage_fall.xml"]=true, ["haptic/damage_explosion.xml"]=true,
      ["haptic/vehicle_damage.xml"]=true, ["haptic/rifle_fire.xml"]=true,
    }
    if vanilla[path] or M.mod_assets_available then
      -- A mod-local file must actually exist on disk, exactly as the game
      -- would require.
      local rel = path:gsub("^MOD/", "")
      local f = io.open(M.moddir .. "/" .. rel, "r")
      if f ~= nil then
        f:close()
        M.haptics_loaded[#M.haptics_loaded+1] = path
        return "h:" .. path
      end
      if vanilla[path] then
        M.haptics_loaded[#M.haptics_loaded+1] = path
        return "h:" .. path
      end
    end
    M.haptics_loaded[#M.haptics_loaded+1] = "MISS:" .. path
    return ""
  end
  G.CreateHaptic = function(a,b,c,d) bump("CreateHaptic"); return "h:created" end
  G.PlayHaptic = function(h, amp)
    bump("PlayHaptic")
    M.haptics_played[#M.haptics_played+1] = { h=h, amp=amp, dir=false }
  end
  G.PlayHapticDirectional = function(h, d, amp)
    bump("PlayHapticDirectional")
    M.haptics_played[#M.haptics_played+1] = { h=h, amp=amp, dir=true, vec={d[1],d[2],d[3]} }
  end
  G.HapticIsPlaying = function(h) bump("HapticIsPlaying"); return false end
  G.StopHaptic = function(h) bump("StopHaptic") end
  G.SetToolHaptic = function(i,h,a) bump("SetToolHaptic") end

  -- --- UI, script_defs.luau:9132-10333 --------------------------------------
  G.UiPush = function() bump("UiPush") end
  G.UiPop = function() bump("UiPop") end
  G.UiText = function(t, mv, mc) bump("UiText"); M.drawn[#M.drawn+1] = t; return 0,0,0,0,"" end
  G.UiColor = function(r,g,b,a) bump("UiColor") end
  G.UiResetColor = function() bump("UiResetColor") end
  G.UiTranslate = function(x,y) bump("UiTranslate") end
  G.UiAlign = function(s) bump("UiAlign") end
  G.UiFont = function(p,s) bump("UiFont") end
  G.UiRect = function(w,h) bump("UiRect") end
  G.UiWidth = function() return 1920 end
  G.UiHeight = function() return 1080 end
  G.UiTextButton = function(t,w,h) bump("UiTextButton"); return false end

  M.drawn = {}
  return G
end

--[[---------------------------------------------------------------------------
  Test helpers
-------------------------------------------------------------------------------]]

--[[---------------------------------------------------------------------------
  Run n frames of dt, advancing the simulated clock.

  The mock world integrates position from velocity, the way the engine would,
  so detectors that accumulate distance (footsteps) see real movement. Without
  this, a velocity-only world makes every distance-based detector look broken.
-------------------------------------------------------------------------------]]
function M.runFrames(G, n, dt)
  dt = dt or (1.0/60.0)
  for i = 1, n do
    M.world.time = M.world.time + dt
    M.integrate(dt)
    G.tick(dt)
  end
end

--[[---------------------------------------------------------------------------
  integrate(dt) -- move the simulated player by its current velocity.
-------------------------------------------------------------------------------]]
function M.integrate(dt)
  local W = M.world
  if W.noPhysics then return end
  W.pos[1] = W.pos[1] + W.vel[1] * dt
  W.pos[2] = W.pos[2] + W.vel[2] * dt
  W.pos[3] = W.pos[3] + W.vel[3] * dt
end

-- All events currently published in the ring, oldest first.
function M.events()
  local n = M.store_int["vr.ev.count"] or 0
  local out = {}
  -- Walk the ring from oldest to newest.
  local head = M.store_int["vr.ev.head"] or 0
  local slots = 16
  for k = n - 1, 0, -1 do
    local idx = ((head - k) % slots)
    local b = "vr.ev.slot." .. idx .. "."
    out[#out+1] = {
      id    = M.store_int[b.."id"],
      amp   = M.store_float[b.."amp"],
      ttl   = M.store_float[b.."ttl"],
      hand  = M.store_int[b.."hand"],
      flags = M.store_int[b.."flags"],
      x     = M.store_float[b.."x"],
      y     = M.store_float[b.."y"],
      z     = M.store_float[b.."z"],
    }
  end
  return out
end

function M.findEvents(id)
  local out = {}
  for _, e in ipairs(M.events()) do
    if e.id == id then out[#out+1] = e end
  end
  return out
end

function M.clearEvents()
  M.store_int["vr.ev.count"] = 0
end

function M.lastEventId()
  local evs = M.events()
  if #evs == 0 then return nil end
  return evs[#evs].id
end

return M
