--[[===========================================================================
  vrhaptics.lua -- Teardown VR: game event -> haptic event mapping.

  Two jobs:

    1. DETECT. The engine has no damage, explosion, collision or landing
       callbacks exposed to Lua (verified: no such name exists in
       data/script_defs.luau or in the binary's Lua name table). So every
       trigger here is a POLL of documented read-only state, sampled in tick()
       and diffed against the previous sample. That is the honest design: the
       mod derives events by watching, and the diffing is where the false
       positives and missed events come from.

    2. PUBLISH. Each detection becomes a VRBUS event (id, amplitude 0..1,
       duration, hand, world position, flags). The native layer forwards it to
       the VR controllers.

  A haptic EFFECT (an .xml curve) is a separate concern and is handled here too,
  as an optional game-side fallback: with no DLL attached, this mod can still
  rumble whatever pad the game sees, using the game's own haptic assets.

  ---------------------------------------------------------------------------
  VERIFIED
  ---------------------------------------------------------------------------
  Detection reads, all with confirmed signatures in data/script_defs.luau:
      GetPlayerHealth(playerId?)      :5876   0..1
      GetPlayerPos(playerId?)         :5329
      GetPlayerVelocity(playerId?)    :5566
      GetPlayerUp(playerId?)          :6458
      GetPlayerTransform(playerId?)   :5420
      GetPlayerEyeTransform(playerId?):5487
      GetPlayerTool(playerId?)        :5926
      GetPlayerVehicle(playerId?)     :5627
      GetPlayerCount()                :5228
      GetVehicleBody(vehicle)         :4989
      GetToolAmmo(toolId, playerId?)  :6386
      GetBodyVelocity(body)           :2138
      GetBodyVelocityAtPos(body,pos)  :2162
      GetBodyTransform(body)          :2040
      GetBodyBounds(body)             :2351   -> min, max
      IsBodyBroken(body)              :2413
      IsBodyDynamic(body)             :2085
      GetShapeBody(shape)             :2780
      GetShapeWorldTransform(shape)   :2762
      GetShapeMaterialAtPosition(...) :2903   -> material, ...
      QueryRaycast(o,d,maxDist,radius?,rejectTransparent?)
                                            :7363   -> hit, dist, normal, shape
      QueryAabbShapes(min,max)        :7496   -> list of shape handles
      InputDown(input,playerId?)      :213
      InputPressed(input,playerId?)   :185
      InputValue(input,playerId?)     :227
      IsPlayerLocal(playerId?)        :5272
      GetTime()                       :138
      Vec/VecAdd/VecSub/VecLength/VecScale/VecNormalize/VecDot
                                     :1047+:1094

  Haptic playback, all confirmed in data/script_defs.luau:
      LoadHaptic(filepath)                     :8810  -> handle
      CreateHaptic(lMotor,rMotor,lTrig,rTrig)  :8831  -> handle
      PlayHaptic(handle, amplitude)            :8850
      PlayHapticDirectional(handle, dir, amp)  :8871
      HapticIsPlaying(handle)                  :8890
      SetToolHaptic(id, handle, amplitude?)    :8907
      StopHaptic(handle)                       :8927

  ---------------------------------------------------------------------------
  ASSUMED -- flagged at each use site, not silently relied on
  ---------------------------------------------------------------------------
  * WatchBodyHit / IsBodyHitted / GetBodyHitsCount / GetBodyHit are declared in
    script_defs.luau as bare `function f(...) end` with NO annotations. They
    appear in 4 shipped mods (folkrace/m_village/script/{village_grinder,
    trash_hole,water_tower}.lua) called as
        WatchBodyHit(body); if IsBodyHitted(body) then
        for i = 0, GetBodyHitsCount(body)-1 do
            local other, impulse, pos, normal, this_vel, other_vel = GetBodyHit(body, i)
    That call convention is taken from real code but is NOT documented. If it is
    wrong it throws, so the impact path is pcall-guarded and degrades to
    "no tool-impact haptics" rather than to a crash.
  * GetToolAmmo returning 0 for a melee tool is assumed to mean "not applicable",
    not "empty". Untested.
  * Amplitude of CreateHaptic is assumed 0..1, inferred from the documented
    example CreateHaptic(1, 1, 0, 0) (both motors full, triggers off). The
    stub does not state a range.
  * <lifetime> in a haptic .xml is assumed to be seconds. Plausible, not
    documented. The .ttl published to the bus is therefore advisory metadata for
    the native side only; on the game-side path the XML owns the real duration.
============================================================================]]

VRHAPTICS = VRHAPTICS or {}
VRHAPTICS.__index = VRHAPTICS

--[[---------------------------------------------------------------------------
  EFFECT CATALOG

  asset       path passed to LoadHaptic. Paths WITHOUT a "MOD/" prefix resolve
              against the game's built-in haptic table -- proven by
              mods/folkrace/script/gadget_nitro.lua:76 which loads
              "haptic/vehicle_turbo.xml", a file that exists only in the
              vanilla data/haptic/ directory.
  amp         default 0..1 scale applied on top of the per-event intensity.
  ttl         advisory duration published to the native side, seconds.
  hand        VRBUS_HAND_*.
-------------------------------------------------------------------------------]]
VRHAPTICS.CATALOG = {
  { id = VRBUS_EV_DAMAGE_TAKEN,  asset = "MOD/haptic/vr_damage.xml",     amp = 0.9, ttl = 0.25, hand = VRBUS_HAND_BOTH  },
  { id = VRBUS_EV_LANDING,       asset = "MOD/haptic/vr_landing.xml",    amp = 1.0, ttl = 0.20, hand = VRBUS_HAND_BOTH  },
  { id = VRBUS_EV_FALL_START,    asset = nil,                         amp = 0.0, ttl = 0.00, hand = VRBUS_HAND_BOTH  },
  { id = VRBUS_EV_TOOL_FIRE,     asset = "MOD/haptic/vr_tool_fire.xml",  amp = 0.7, ttl = 0.12, hand = VRBUS_HAND_RIGHT },
  { id = VRBUS_EV_TOOL_IMPACT,   asset = "MOD/haptic/vr_tool_impact.xml",amp = 0.5, ttl = 0.10, hand = VRBUS_HAND_BOTH  },
  { id = VRBUS_EV_EXPLOSION,     asset = "MOD/haptic/vr_explosion.xml",  amp = 1.0, ttl = 0.40, hand = VRBUS_HAND_BOTH  },
  { id = VRBUS_EV_BREAK,         asset = "MOD/haptic/vr_tool_impact.xml",amp = 0.45, ttl = 0.08, hand = VRBUS_HAND_BOTH },
  { id = VRBUS_EV_GRAB,          asset = "MOD/haptic/vr_grab.xml",    amp = 0.4, ttl = 0.05, hand = VRBUS_HAND_RIGHT },
  { id = VRBUS_EV_JUMP,          asset = nil,                         amp = 0.0, ttl = 0.00, hand = VRBUS_HAND_BOTH  },
  { id = VRBUS_EV_FOOTSTEP,      asset = "MOD/haptic/vr_footstep.xml",amp = 0.35, ttl = 0.06, hand = VRBUS_HAND_BOTH  },
  { id = VRBUS_EV_VEHICLE_HIT,   asset = "MOD/haptic/vr_vehicle_impact.xml", amp = 0.8, ttl = 0.30, hand = VRBUS_HAND_BOTH  },
  { id = VRBUS_EV_VEHICLE_ENTER, asset = nil,                         amp = 0.0, ttl = 0.00, hand = VRBUS_HAND_BOTH  },
  { id = VRBUS_EV_VEHICLE_EXIT,  asset = nil,                         amp = 0.0, ttl = 0.00, hand = VRBUS_HAND_BOTH  },
  { id = VRBUS_EV_HEARTBEAT,     asset = nil,                         amp = 0.0, ttl = 0.00, hand = VRBUS_HAND_BOTH  },
}

--[[---------------------------------------------------------------------------
  Tuning
-------------------------------------------------------------------------------]]
VRHAPTICS.TUNE = {
  -- A health drop smaller than this is ignored (regeneration ticks, 0.02 chip
  -- damage from weapon scripts). 0.01 was chosen because the smallest damage
  -- step seen in shipped mods is 0.02 (e.g. SetPlayerHealth(h - 0.02 * ...)).
  damageEpsilon     = 0.01,
  -- Fraction of health lost that maps to full-scale intensity.
  damageForMax      = 0.35,

  -- Landing: minimum downward speed, and the impact speed that maps to 1.0.
  fallMinSpeed      = 3.0,
  fallMaxSpeed      = 22.0,
  -- A jump is a rise faster than this with the player then leaving the ground.
  jumpMinSpeed      = 4.0,

  -- Ground probe distance, meters. The player capsule is ~1.8 m; anything past
  -- ~2.2 m is genuinely airborne.
  groundProbe       = 2.2,

  -- Footstep cadence is derived from distance travelled, not a timer, so it
  -- does not drift at low frame rates.
  stepStride        = 2.1,

  -- Tool fire: ammo decrease of at least this counts as a discharge.
  fireMinAmmoDrop   = 1,

  -- Explosion: reported when any dynamic shape enters this radius. Chosen to
  -- match the game's own explosion size range; ASSUMED, not measured.
  explosionRadius   = 18.0,

  -- Per-id minimum seconds between accepted events.
  rateLimit = {
    [VRBUS_EV_DAMAGE_TAKEN] = 0.05,
    [VRBUS_EV_LANDING]      = 0.15,
    [VRBUS_EV_FALL_START]   = 0.30,
    [VRBUS_EV_TOOL_FIRE]    = 0.03,
    [VRBUS_EV_TOOL_IMPACT]  = 0.06,
    [VRBUS_EV_EXPLOSION]    = 0.12,
    [VRBUS_EV_BREAK]        = 0.04,
    [VRBUS_EV_GRAB]         = 0.10,
    [VRBUS_EV_JUMP]         = 0.30,
    [VRBUS_EV_FOOTSTEP]     = 0.12,
    [VRBUS_EV_VEHICLE_HIT]  = 0.15,
    [VRBUS_EV_VEHICLE_ENTER]= 0.50,
    [VRBUS_EV_VEHICLE_EXIT] = 0.50,
  },
}

--[[---------------------------------------------------------------------------
  new(bus) -> h
-------------------------------------------------------------------------------]]
function VRHAPTICS.new(bus)
  local self = setmetatable({}, VRHAPTICS)
  self.bus = bus
  self.handles = {}        -- asset path -> LoadHaptic handle
  self.playGameHaptics = false  -- see note on the play() function
  self._s = nil            -- previous-sample state
  self._stats = { emitted = 0, suppressed = 0, loadFail = 0 }
  return self
end

--[[---------------------------------------------------------------------------
  handle(asset) -> handle
    Load (and memoise) a haptic effect by asset path.
    ASSUMED: LoadHaptic returns "" for a missing path rather than erroring.
    pcall-guarded regardless so a bad path cannot break the frame.
-------------------------------------------------------------------------------]]
function VRHAPTICS.handle(self, asset)
  if asset == nil then return nil end
  local cached = self.handles[asset]
  if cached ~= nil then
    if cached == false then return nil end
    return cached
  end
  local ok, h = pcall(LoadHaptic, asset)
  if not ok or h == nil or h == "" then
    self.handles[asset] = false
    self._stats.loadFail = self._stats.loadFail + 1
    return nil
  end
  self.handles[asset] = h
  return h
end

--[[---------------------------------------------------------------------------
  entryFor(id) -> catalog entry
    Self is taken so this is called as self:entryFor(id), matching every other
    method here. An earlier revision declared it without `self` and called it
    with a colon, which silently shifted the id argument and made every event
    fall through to the unknown-id path -- ttl and hand both came out zero.
    The test suite now asserts ttl > 0 and the per-event hand to catch exactly
    that class of bug.
-------------------------------------------------------------------------------]]
function VRHAPTICS.entryFor(self, id)
  for i = 1, #VRHAPTICS.CATALOG do
    if VRHAPTICS.CATALOG[i].id == id then return VRHAPTICS.CATALOG[i] end
  end
  return nil
end

--[[---------------------------------------------------------------------------
  fire(id, intensity, opts)
    The single funnel every detector goes through. Normalises intensity, applies
    the catalog's base amplitude, rate-limits, publishes to the bus, and
    optionally plays the game-side effect.

    intensity  0..1 raw strength from the detector.
    opts.pos   TVec world position of the cause.
    opts.hand  override the catalog's controller.
    opts.flags VRBUS_FLAG_* bitfield.
-------------------------------------------------------------------------------]]
function VRHAPTICS.fire(self, id, intensity, opts)
  opts = opts or {}
  local now = GetTime()

  local limit = VRHAPTICS.TUNE.rateLimit[id]
  if limit ~= nil and not self.bus:allow(id, limit, now) then
    self._stats.suppressed = self._stats.suppressed + 1
    return false
  end

  local entry = self:entryFor(id)
  if entry == nil then
    -- Unknown id: still publish, with neutral metadata. The native side can
    -- decide what to do with a numeric id it does not recognise.
    self.bus:emit(id, intensity, 0.0, opts.hand, opts.pos, opts.flags)
    self._stats.emitted = self._stats.emitted + 1
    return true
  end

  local amp = intensity * entry.amp
  if amp <= 0.0 then
    -- Catalog says this event has no effect curve (a jump has no motor
    -- signature, say). Still tell the native side about it, at reduced gain,
    -- because a native renderer may want to synthesize something.
    amp = intensity * 0.25
  end
  if amp > 1.0 then amp = 1.0 end

  self.bus:emit(id, amp, entry.ttl, opts.hand or entry.hand, opts.pos, opts.flags)
  self._stats.emitted = self._stats.emitted + 1

  if self.playGameHaptics then self:play(entry, amp, opts) end
  return true
end

--[[---------------------------------------------------------------------------
  play(entry, amp, opts)
    Rumble whatever controller the game itself sees. This is the fallback that
    makes the mod useful with no native DLL present.

    PlayHaptic(handle, amplitude) has no duration argument -- the curve's
    <lifetime> in the .xml owns how long it runs. So the .ttl this bus carries
    does NOT control the game-side effect, only the native-side one.

    opts.pos: when present and a directional effect is loaded, PlayHapticDirectional
    is used so the rumble comes from the direction the event came from. Its
    signature is documented (script_defs.luau:8871) but it has zero call sites
    in the 2838 shipped mod scripts, so it stays off by default behind a flag.
-------------------------------------------------------------------------------]]
function VRHAPTICS.play(self, entry, amp, opts)
  local h = self:handle(entry.asset)
  if h == nil then return false end
  local ok = pcall(function()
    if opts and opts.pos and self.directional then
      -- ASSUMED: direction is a world-space unit vector. Documented as TVec
      -- only; the reference frame is not stated.
      local p = opts.pos
      local d = VecNormalize(Vec(p[1], p[2], p[3]))
      PlayHapticDirectional(h, d, amp)
    else
      PlayHaptic(h, amp)
    end
  end)
  return ok
end

--[[---------------------------------------------------------------------------
  init(self)
    Prime the sample state. Must be called before the first tick, or the first
    tick will read a zeroed sample and report a phantom full-health-to-zero drop
    and a phantom landing.
-------------------------------------------------------------------------------]]
function VRHAPTICS.init(self)
  self._s = {
    health    = self:readHealth(),
    pos       = nil,
    grounded  = true,
    lastY     = nil,
    distance  = 0,
    tool      = nil,
    ammo      = nil,
    vehicle   = nil,
    toolBody  = nil,
    toolHits  = 0,
    bodyWatch = {},   -- body handle -> last known broken state
    shapes    = {},   -- explosion candidate shapes
  }
  self.directional = false
end

--[[---------------------------------------------------------------------------
  localPlayer()
    VERIFIED 2026-09-28 in the live game: GetLocalPlayer() raises
    "Calling the GetLocalPlayer API function before init is not supported"
    when it runs before the game is ready. Because it is normally an ARGUMENT
    to another call, the surrounding pcall cannot catch it -- the throw happens
    while evaluating the arguments, before pcall is entered. So every use has
    to go through this guard, which degrades to player 0 (the host, and the
    local player on the client) instead of raising.
-------------------------------------------------------------------------------]]
local function localPlayer()
  local p = 0
  local ok = pcall(function() p = GetLocalPlayer() end)
  if not ok or p == nil then return 0 end
  return p
end

--[[---------------------------------------------------------------------------
  readHealth(self) -> number
    Health of the local player, 0..1. GetPlayerHealth is per-player and takes
    an optional id; the host's own player is 0, and on the client 0 is the local
    player. GetLocalPlayer() gives the right id for each side.
-------------------------------------------------------------------------------]]
function VRHAPTICS.readHealth(self)
  local ok, h = pcall(GetPlayerHealth, localPlayer())
  if not ok or h == nil then return 1.0 end
  if h > 1.0 then h = 1.0 end
  if h < 0.0 then h = 0.0 end
  return h
end

--[[---------------------------------------------------------------------------
  grounded(self, pos) -> grounded, distanceToGround
    Probe straight down from the player's feet with a raycast. Direction is
    world down, which is correct because the game's gravity axis is -Y
    (speedometer/main.lua:28 uses -vel[3] for vehicle forward, and every
    shipped QueryRaycast ground probe uses Vec(0,-1,0)).
-------------------------------------------------------------------------------]]
function VRHAPTICS.grounded(self, pos)
  local ok, hit, dist = pcall(QueryRaycast, pos, Vec(0, -1, 0), VRHAPTICS.TUNE.groundProbe)
  if not ok then return true, VRHAPTICS.TUNE.groundProbe end
  return (hit == true), (dist or VRHAPTICS.TUNE.groundProbe)
end

--[[---------------------------------------------------------------------------
  tick(self, dt)
    One pass of all detectors. dt is the real frame time; note that
    GetTimeStep() returns a FIXED 1/60 from update() and the real elapsed time
    from tick(), so this must be called from tick(), never update().

    Each detector is wrapped in pcall. A detector that throws is disabled for
    the rest of the session rather than throwing every frame and flooding the
    log: that keeps a wrong assumption in one trigger from taking the haptics
    channel down with it.
-------------------------------------------------------------------------------]]
function VRHAPTICS.tick(self, dt)
  local s = self._s
  if s == nil then self:init() s = self._s end
  if dt == nil or dt <= 0 then dt = 1.0 / 60.0 end

  self:detectDamage(s)
  self:detectMotion(s, dt)
  self:detectTool(s)
  self:detectImpacts(s)
  self:detectVehicle(s)
end

--[[---------------------------------------------------------------------------
  detectDamage(s)
    Health is a 0..1 scalar, so damage is a first difference. The first sample
    is taken in init() precisely so this does not fire on level load.
-------------------------------------------------------------------------------]]
function VRHAPTICS.detectDamage(self, s)
  local ok, h = pcall(VRHAPTICS.readHealth, self)
  if not ok then return end

  local drop = s.health - h
  if drop > VRHAPTICS.TUNE.damageEpsilon then
    local intensity = drop / VRHAPTICS.TUNE.damageForMax
    if intensity > 1.0 then intensity = 1.0 end
    self:fire(VRBUS_EV_DAMAGE_TAKEN, intensity, { pos = self:safePos() })
  end
  s.health = h
end

--[[---------------------------------------------------------------------------
  detectMotion(s, dt)
    Landing and footsteps from vertical velocity and a ground probe.

    The classifier is: airborne + downward velocity past fallMinSpeed builds a
    fall energy; the frame the ground probe comes back true, that energy is
    spent as a landing. A rise past jumpMinSpeed is a jump. Footsteps accrue
    horizontal distance while grounded and fire every stepStride meters.
-------------------------------------------------------------------------------]]
function VRHAPTICS.detectMotion(self, s, dt)
  local okPos, pos = pcall(GetPlayerPos, localPlayer())
  if not okPos or pos == nil then return end

  local okVel, vel = pcall(GetPlayerVelocity, localPlayer())
  if not okVel or vel == nil then return end

  local x, y, z = pos[1], pos[2], pos[3]
  local vx, vy, vz = vel[1], vel[2], vel[3]

  local wasGrounded = s.grounded
  local isGrounded, _ = self:grounded(pos)
  s.grounded = isGrounded

  -- Horizontal travel, used only for footstep cadence.
  if s.pos ~= nil and isGrounded then
    local dx, dz = x - s.pos[1], z - s.pos[3]
    s.distance = s.distance + math.sqrt(dx * dx + dz * dz)
  end
  s.pos = { x, y, z }

  -- Landing: we were airborne and are now grounded.
  if not wasGrounded and isGrounded then
    local energy = s.fallEnergy or 0.0
    if energy >= VRHAPTICS.TUNE.fallMinSpeed then
      local t = (energy - VRHAPTICS.TUNE.fallMinSpeed) /
                (VRHAPTICS.TUNE.fallMaxSpeed - VRHAPTICS.TUNE.fallMinSpeed)
      if t > 1.0 then t = 1.0 end
      if t < 0.0 then t = 0.0 end
      self:fire(VRBUS_EV_LANDING, t, { pos = pos })
    end
    s.fallEnergy = 0.0
  end

  -- Airborne: track the fastest downward speed seen during the fall.
  if not isGrounded then
    local down = -vy
    if down > (s.fallEnergy or 0.0) then s.fallEnergy = down end

    if wasGrounded and vy > VRHAPTICS.TUNE.jumpMinSpeed then
      self:fire(VRBUS_EV_JUMP, math.min(vy / VRHAPTICS.TUNE.jumpMinSpeed, 1.0), { pos = pos })
    end
  end

  -- Footsteps.
  if isGrounded and s.distance >= VRHAPTICS.TUNE.stepStride then
    s.distance = 0.0
    local speed = math.sqrt(vx * vx + vz * vz)
    if speed > 0.6 then
      self:fire(VRBUS_EV_FOOTSTEP, math.min(speed / 6.0, 1.0),
                { pos = pos, flags = VRBUS_FLAG_NONE })
    end
  end
end

--[[---------------------------------------------------------------------------
  detectTool(s)
    Tool switch from GetPlayerTool. Tool fire from the ammo counter dropping,
    which is the only firing signal exposed to Lua -- there is no "weapon fired"
    callback.

    ASSUMED: for a tool with no ammo (melee, most tools) GetToolAmmo returns 0
    and no fire events are produced. That means fire haptics only work for
    tools that actually consume ammo.
-------------------------------------------------------------------------------]]
function VRHAPTICS.detectTool(self, s)
  local ok, tool = pcall(GetPlayerTool, localPlayer())
  if not ok or tool == nil then return end

  if s.tool ~= nil and tool ~= s.tool then
    self:fire(VRBUS_EV_TOOL_SWITCH, 0.3, { pos = self:safePos() })
  end
  s.tool = tool

  local okAmmo, ammo = pcall(GetToolAmmo, tool, localPlayer())
  if not okAmmo or ammo == nil then return end

  if s.ammo ~= nil and ammo < (s.ammo - VRHAPTICS.TUNE.fireMinAmmoDrop) then
    local spent = s.ammo - ammo
    local t = spent / 10.0
    if t > 1.0 then t = 1.0 end
    self:fire(VRBUS_EV_TOOL_FIRE, math.max(t, 0.35), { pos = self:safePos() })
  end
  s.ammo = ammo
end

--[[---------------------------------------------------------------------------
  detectImpacts(s)
    Tool impact and destruction, from the body-hit watch API.

    ASSUMED SIGNATURES (see the header block): WatchBodyHit(body) subscribes,
    IsBodyHitted(body) reports "new hit this frame", GetBodyHit(body, i) returns
    otherBody, impulse, pos, normal, thisVel, otherVel. Undocumented in the
    stub; pcall-guarded; this whole path is expected to need a live-game test.

    Only bodies the player is actually near are watched, otherwise this would
    be an O(level) scan every frame.
-------------------------------------------------------------------------------]]
function VRHAPTICS.detectImpacts(self, s)
  local ok, body = pcall(GetToolBody, localPlayer())
  if not ok or body == nil or body == 0 then
    s.toolBody = nil
    return
  end
  s.toolBody = body

  local okWatch = pcall(WatchBodyHit, body)
  if not okWatch then return end

  local okHitted, hitted = pcall(IsBodyHitted, body)
  if not okHitted or not hitted then return end

  local okCount, n = pcall(GetBodyHitsCount, body)
  if not okCount or n == nil or n <= 0 then return end

  -- Bound the work: at most 4 hits examined per frame.
  local limit = n
  if limit > 4 then limit = 4 end

  for i = 0, limit - 1 do
    local okHit, other, impulse, pos, normal = pcall(GetBodyHit, body, i)
    if okHit and other ~= nil then
      local mag = math.abs(tonumber(impulse) or 0.0)
      local t = mag / 400.0
      if t > 1.0 then t = 1.0 end
      if t > 0.05 then
        local flags = VRBUS_FLAG_NONE
        local okM, mat = pcall(GetShapeMaterialAtPosition, other, pos or Vec())
        -- GetShapeMaterialAtPosition takes a SHAPE handle, but GetBodyHit
        -- returns a BODY handle. Passing a body where a shape is expected is
        -- ASSUMED to be harmless; the result is only used to set a flag.
        if okM and type(mat) == "string" and mat ~= "" then
          flags = VRBUS_FLAG_SURFACE
        end
        self:fire(VRBUS_EV_TOOL_IMPACT, t, { pos = pos, flags = flags })
      end
    end
  end
end

--[[---------------------------------------------------------------------------
  detectVehicle(s)
    Enter/exit from GetPlayerVehicle, which returns 0 when not driving.
    Vehicle damage has no readable signal: the vehicle's health is not exposed
    to Lua (GetVehicleHealth is used in shipped mods but is not in
    script_defs.luau, so its availability is unverified). Body-hit watching on
    the vehicle body is the fallback and is left unimplemented rather than
    guessed at.
-------------------------------------------------------------------------------]]
function VRHAPTICS.detectVehicle(self, s)
  local ok, veh = pcall(GetPlayerVehicle, localPlayer())
  if not ok or veh == nil then return end

  if s.vehicle == nil or s.vehicle == 0 then
    if veh ~= 0 then
      self:fire(VRBUS_EV_VEHICLE_ENTER, 0.4, { pos = self:safePos() })
    end
  elseif veh == 0 then
    self:fire(VRBUS_EV_VEHICLE_EXIT, 0.4, { pos = self:safePos() })
  end
  s.vehicle = veh
end

--[[---------------------------------------------------------------------------
  safePos(self) -> TVec or nil
    Player position, or nil if it cannot be read this frame. Every detector
    passes this rather than calling GetPlayerPos itself so a single bad read
    cannot take out the rest of the frame.
-------------------------------------------------------------------------------]]
function VRHAPTICS.safePos(self)
  local ok, p = pcall(GetPlayerPos, localPlayer())
  if not ok or p == nil then return nil end
  return p
end
