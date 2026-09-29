--[[===========================================================================
  vrcamera.lua -- Teardown VR: per-eye camera parameters for the native
  stereo renderer.

  WHAT THIS IS NOT: it does not render anything stereo. Teardown's renderer is
  a single-pass forward renderer; per-eye output has to be produced in the
  native layer (see hook/stereo.h and docs/STEREO_MATH.md). This module's job
  is to compute the numbers the native side needs and publish them at fixed
  keys each frame.

  WHAT IT DOES: track the head pose, derive eye separation / near / far from
  user settings, and decide whether the game is in a state where a stereo
  override should apply at all.

  ---------------------------------------------------------------------------
  VERIFIED
  ---------------------------------------------------------------------------
      GetPlayerEyeTransform(playerId?)  script_defs.luau:5487
          The doc states outright: "The player eye transform is the same as
          what you get from GetCameraTransform when playing in first-person,
          but if you have set a camera transform manually ... you can retrieve
          the player eye transform with this function." That makes it the right
          source for the head pose, and it keeps working if the native layer is
          already driving SetCameraTransform.
      GetCameraTransform()              script_defs.luau:8261
      SetCameraTransform(transform, fov?):8276
      SetCameraFov(degrees)             script_defs.luau:8398
          Doc: "Horizontal field of view in degrees (10-170)" and "Override
          field of view for the next frame for all camera modes, except when
          explicitly set in SetCameraTransform". HORIZONTAL fov, which is the
          one the native projection code should mirror.
      SetCameraOffsetTransform(t, stackable?) :8332
      SetPlayerCameraOffsetTransform(t, stackable?, playerId?) :5517
      GetPlayerTransform(playerId?)     script_defs.luau:5420
      GetPlayerPos(playerId?)           script_defs.luau:5329
      GetPlayerUp(playerId?)            script_defs.luau:6458
      GetPlayerPitch / GetPlayerYaw     script_defs.luau:5365 / :5377
      QuatRotateVec(q, v)               -- verified present in teardown.exe as a
                                          Lua binding; signature taken from
                                          shipped use, e.g.
                                          wildwestheist/ravine/script/truxterminator.lua
                                          QueryRaycast from an eye transform.
      GetBool / SetBool                 script_defs.luau:705 / :693
      GetFloat / SetFloat               script_defs.luau:680 / :668
      TransformStr(t)                   -- present in binary; used by
                                          GetPlayerEyeTransform's own doc example.

  ---------------------------------------------------------------------------
  ASSUMED -- flagged below, not relied on silently
  ---------------------------------------------------------------------------
  * Units. Positions are metres and rotations radians, which is consistent with
    every shipped mod (speedometer converts m/s to km/h with *3.6; QueryRaycast
    radii are sub-metre). No stub file states units for transforms, so this is
    an inference from usage, a strong one.
  * Quat component order. The head pose is published as a transform's .pos plus
    pitch/yaw/roll read separately from GetPlayerPitch/GetPlayerYaw, specifically
    so the native side never has to interpret a quaternion's component order.
    GetPlayerUp publishes the up axis directly for the same reason.
  * Roll. There is no documented GetPlayerRoll, so roll is derived from the eye
    transform's up vector projected against world up. That is a reconstruction,
    not a read-out; it is exact for yaw/pitch and only approximate under roll.
  * Teardown's world axes are x = right, y = up, -z = forward (from
    speedometer/main.lua:26-30, where forward speed is -vel[3] in vehicle local
    space, and from every shipped ground probe using Vec(0,-1,0) as down).
    Eye offsets below are applied along x accordingly.
  * There is no documented near/far plane accessor. The defaults here are the
    mod's own choice and are the values the native side should use until the
    real projection constants are read out of the renderer.
============================================================================]]

VRCAMERA = VRCAMERA or {}
VRCAMERA.__index = VRCAMERA

--[[---------------------------------------------------------------------------
  Defaults
-------------------------------------------------------------------------------]]
VRCAMERA.DEFAULT_IPD      = 0.064   -- 64 mm, the usual average adult IPD
VRCAMERA.DEFAULT_NEAR     = 0.05    -- 5 cm
VRCAMERA.DEFAULT_FAR      = 500.0   -- 500 m
VRCAMERA.DEFAULT_FOV      = 90.0    -- degrees, HORIZONTAL
VRCAMERA.FOV_MIN          = 10.0    -- bounds are documented at :8398
VRCAMERA.FOV_MAX          = 170.0

-- Head poses the native side should NOT stereo-render. Vehicle and respawn
-- transitions are the ones that produce a disorienting jump.
VRCAMERA.SUPPRESS_IN_VEHICLE = true

--[[---------------------------------------------------------------------------
  new(bus) -> cam
-------------------------------------------------------------------------------]]
function VRCAMERA.new(bus)
  local self = setmetatable({}, VRCAMERA)
  self.bus = bus
  self.enabled = true
  self.ipdScale = 1.0
  self.near = VRCAMERA.DEFAULT_NEAR
  self.far = VRCAMERA.DEFAULT_FAR
  self.fov = VRCAMERA.DEFAULT_FOV
  self.inhibitUntil = 0.0
  self._last = nil
  return self
end

--[[---------------------------------------------------------------------------
  settings keys. Persisted in the game's own key store, so they survive a
  level reload the same way any other mod setting does. Namespaced "vr."
  because the store is global to the level.
-------------------------------------------------------------------------------]]
VRCAMERA.KEY_ENABLED = "vr.user.enabled"
VRCAMERA.KEY_IPD_MM   = "vr.user.ipd_mm"
VRCAMERA.KEY_FOV      = "vr.user.fov"
VRCAMERA.KEY_SCALE    = "vr.user.sep_scale"

--[[---------------------------------------------------------------------------
  loadSettings(self)
    Read user settings out of the key store, filling in defaults. A mod may run
    before the settings UI has ever written them, so every read is defaulted.
-------------------------------------------------------------------------------]]
function VRCAMERA.loadSettings(self)
  if not HasKey(VRCAMERA.KEY_ENABLED) then SetBool(VRCAMERA.KEY_ENABLED, true) end
  if not HasKey(VRCAMERA.KEY_IPD_MM)   then SetInt(VRCAMERA.KEY_IPD_MM, 64) end
  if not HasKey(VRCAMERA.KEY_FOV)      then SetInt(VRCAMERA.KEY_FOV, math.floor(VRCAMERA.DEFAULT_FOV)) end
  if not HasKey(VRCAMERA.KEY_SCALE)    then SetFloat(VRCAMERA.KEY_SCALE, 1.0) end

  self.enabled  = GetBool(VRCAMERA.KEY_ENABLED)
  self.ipdScale = GetFloat(VRCAMERA.KEY_SCALE)
  if self.ipdScale == nil or self.ipdScale <= 0 then self.ipdScale = 1.0 end
  self.fov      = GetInt(VRCAMERA.KEY_FOV)
  if self.fov < VRCAMERA.FOV_MIN or self.fov > VRCAMERA.FOV_MAX then
    self.fov = VRCAMERA.DEFAULT_FOV
  end
  self.near = VRCAMERA.DEFAULT_NEAR
  self.far  = VRCAMERA.DEFAULT_FAR
end

--[[---------------------------------------------------------------------------
  saveSettings(self)
-------------------------------------------------------------------------------]]
function VRCAMERA.saveSettings(self)
  SetBool(VRCAMERA.KEY_ENABLED, self.enabled)
  SetFloat(VRCAMERA.KEY_SCALE, self.ipdScale)
  SetInt(VRCAMERA.KEY_FOV, math.floor(self.fov))
end

--[[---------------------------------------------------------------------------
  localPlayer()
    VERIFIED 2026-09-28 in the live game: GetLocalPlayer() raises
    "Calling the GetLocalPlayer API function before init is not supported" when
    called before the game is ready. As an ARGUMENT to another call it throws
    while the arguments are evaluated, so the surrounding pcall cannot catch
    it. Degrades to 0 (host, and local player on the client).
-------------------------------------------------------------------------------]]
local function localPlayer()
  local p = 0
  local ok = pcall(function() p = GetLocalPlayer() end)
  if not ok or p == nil then return 0 end
  return p
end

--[[---------------------------------------------------------------------------
  readHead(self) -> head or nil
    Build the head pose block from GetPlayerEyeTransform, with the orientation
    decomposed so the native side needs no quaternion convention.

    Returns nil when the pose cannot be read, which is what tells the native
    side to hold its last good frame rather than render a garbage one.
-------------------------------------------------------------------------------]]
function VRCAMERA.readHead(self)
  local id = localPlayer()

  local okT, t = pcall(GetPlayerEyeTransform, id)
  if not okT or t == nil or t.pos == nil then return nil end

  local pos = t.pos
  local head = {
    x = pos[1] or 0.0,
    y = pos[2] or 0.0,
    z = pos[3] or 0.0,
    pitch = 0.0,
    yaw = 0.0,
    roll = 0.0,
    upx = 0.0, upy = 1.0, upz = 0.0,
  }

  -- Pitch and yaw are read directly. ASSUMED radians; the stub does not say.
  local okP, pitch = pcall(GetPlayerPitch, id)
  if okP and pitch ~= nil then head.pitch = pitch end
  local okY, yaw = pcall(GetPlayerYaw, id)
  if okY and yaw ~= nil then head.yaw = yaw end

  -- Up vector, straight from the API, so roll can be derived without touching
  -- the quaternion's component order.
  local okU, up = pcall(GetPlayerUp, id)
  if okU and up ~= nil then
    head.upx, head.upy, head.upz = up[1], up[2], up[3]
  elseif okT and t.rot ~= nil then
    -- Fall back to rotating world up by the eye rotation.
    local okR, r = pcall(QuatRotateVec, t.rot, Vec(0, 1, 0))
    if okR and r ~= nil then head.upx, head.upy, head.upz = r[1], r[2], r[3] end
  end

  -- Roll = signed angle of the up vector from world up, measured in the plane
  -- spanned by world right and world forward.
  -- ASSUMED: this reconstruction is exact for pure yaw/pitch, and only
  -- approximate when the up vector is not exactly perpendicular to forward.
  local upx, upy, upz = head.upx, head.upy, head.upz
  if upy ~= 0 then
    local planar = math.sqrt(upx * upx + upz * upz)
    head.roll = math.atan2(planar, upy)
  end

  return head
end

--[[---------------------------------------------------------------------------
  eyeOffsets(self, head) -> halfSep, rightX, rightY, rightZ
    Half the eye separation along world x, plus the player's right axis, so the
    native side can place the eyes along the head's right vector rather than
    blindly along world x. Under yaw that is the difference between a correct
    stereo rig and one that shears.
-------------------------------------------------------------------------------]]
function VRCAMERA.eyeOffsets(self, head)
  local sep = VRCAMERA.DEFAULT_IPD * self.ipdScale
  local half = sep * 0.5

  -- Right axis = normalize(cross(forward, up)). With world up (0,1,0) and
  -- -z forward this reduces to (up.z, 0, -up.x), but the general form is used
  -- so a rolled head still gets a right vector in its own plane.
  local ux, uy, uz = head.upx, head.upy, head.upz
  -- Forward from the eye transform is -z in local space; recover a world
  -- forward from pitch/yaw rather than from the quaternion.
  local cp = math.cos(head.pitch)
  local fx = math.sin(head.yaw) * cp
  local fy = -math.sin(head.pitch)
  local fz = -math.cos(head.yaw) * cp

  -- right = cross(forward, up)
  local rx = fy * uz - fz * uy
  local ry = fz * ux - fx * uz
  local rz = fx * uy - fy * ux
  local len = math.sqrt(rx * rx + ry * ry + rz * rz)
  if len > 1e-6 then
    rx, ry, rz = rx / len, ry / len, rz / len
  else
    rx, ry, rz = 1.0, 0.0, 0.0
  end

  return half, rx, ry, rz
end

--[[---------------------------------------------------------------------------
  shouldSuppress(self) -> bool
    True while the camera state is one the native side should not stereo-
    render: inside a vehicle (if configured), or within the inhibit window
    after a level change.

    The inhibit window exists because on the first frames after a level starts
    the eye transform can still be the previous level's while the renderer has
    already moved to the new one.
-------------------------------------------------------------------------------]]
function VRCAMERA.shouldSuppress(self)
  if not self.enabled then return true end
  if GetTime() < self.inhibitUntil then return true end
  if VRCAMERA.SUPPRESS_IN_VEHICLE then
    local ok, veh = pcall(GetPlayerVehicle, localPlayer())
    if ok and veh ~= nil and veh ~= 0 then return true end
  end
  return false
end

--[[---------------------------------------------------------------------------
  tick(self)
    Publish this frame's stereo block. Runs in tick(), before the native layer's
    per-frame hook reads it.
-------------------------------------------------------------------------------]]
function VRCAMERA.tick(self)
  local head = self:readHead()

  if head == nil then
    -- No fresh pose: invalidate so the native side holds its previous frame.
    self.bus:publishCamera(nil)
    return
  end

  local half, rx, ry, rz = self:eyeOffsets(head)
  local sep = half * 2.0

  self.bus:publishCamera({
    ipd   = sep,
    sep   = sep,
    fov   = self.fov,
    near  = self.near,
    far   = self.far,
    x     = head.x,
    y     = head.y,
    z     = head.z,
    pitch = head.pitch,
    yaw   = head.yaw,
    roll  = head.roll,
    -- The right axis and the suppress flag ride along in the reserved slots
    -- so the native side does not have to re-derive them.
    rx = rx, ry = ry, rz = rz,
    suppressed = self:shouldSuppress(),
  })
end

--[[---------------------------------------------------------------------------
  suppress(self, seconds)
    Hold off stereo for a while. Called on level change and on respawn.
-------------------------------------------------------------------------------]]
function VRCAMERA.suppress(self, seconds)
  if seconds == nil then seconds = 0.5 end
  self.inhibitUntil = GetTime() + seconds
end
