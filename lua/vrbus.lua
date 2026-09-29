--[[===========================================================================
  vrbus.lua -- Teardown VR: the event / state bus.

  THIS IS THE CONTRACT BETWEEN LUA AND THE NATIVE VR LAYER (teardown_vr.dll).

  The game has no shared-memory bus, no sockets, and no filesystem a mod can
  write to. What it DOES have is a global key/value store exposed to every Lua
  script through SetInt/GetInt, SetFloat/GetFloat, SetBool/GetBool and
  SetString/GetString. That store is the only channel that can carry state out
  of a mod. The native side reads it by locating the same keys in memory.

  Everything here is written so the native side only ever has to read a small,
  fixed set of keys. See docs/LUA_MOD_DESIGN.md for the full protocol.

  ---------------------------------------------------------------------------
  EVENT PROTOCOL (v1)
  ---------------------------------------------------------------------------
  A ring buffer of the most recent VR events is mirrored into the key store at
  a fixed stride. The native side reads:

      "vr.ev.count"   int    number of valid slots in "vr.ev.slot" (0..VRBUS_SLOTS)
      "vr.ev.head"    int    total events ever emitted (monotonic, wraps in Lua)
      "vr.ev.seq"     int    sequence number of the newest event
      "vr.ev.slot.<i>.id"    int   VRBUS id enum (see EV below)
      "vr.ev.slot.<i>.amp"   float 0..1 scaled intensity
      "vr.ev.slot.<i>.ttl"   float seconds the effect should last
      "vr.ev.slot.<i>.hand"  int   0 = both, 1 = left, 2 = right
      "vr.ev.slot.<i>.x"     float world x of the cause (meters)
      "vr.ev.slot.<i>.y"     float world y
      "vr.ev.slot.<i>.z"     float world z
      "vr.ev.slot.<i>.flags" int   VRBUS_FLAG_* bitfield

  The count is what makes this safe to read without locking: the writer only
  publishes an event after its four fields are already written, and it publishes
  the new count LAST. A reader that reads "vr.ev.count" first, then reads only
  slots below that count, can never observe a half-written event. That is the
  whole concurrency contract. No locks, no allocation, no teardown of anything.

  ---------------------------------------------------------------------------
  VERIFIED vs ASSUMED
  ---------------------------------------------------------------------------
  VERIFIED -- every function this file calls is in the game's own API stub file,
  data/script_defs.luau, with a matching signature:
      SetInt(key, value, sync?)      script_defs.luau:643
      GetInt(key)                    script_defs.luau:655
      SetFloat(key, value, sync?)    script_defs.luau:668
      GetFloat(key)                  script_defs.luau:680
      SetBool(key, value, sync?)     script_defs.luau:693
      GetBool(key)                   script_defs.luau:705
      SetString(key, value, sync?)   script_defs.luau:718
      GetString(key)                 script_defs.luau:730
      HasKey(key)                    script_defs.luau:630
      DebugPrint(message)            script_defs.luau:8731
  Their use as a cross-script store is not just allowed but documented -- the
  SetInt doc example is literally `SetInt("score.levels.level1", 4)`, and shipped
  mods read a key that another file wrote (mods/folkrace/config/events.lua:78,86).

  ASSUMED -- see the notes on each item:
    * that the native layer can locate this key store in memory. Not yet
      implemented anywhere in hook/teardown_vr.c. This is the single biggest
      open question in the whole design.
    * that the engine does not resize the key store between our writes and the
      native read. The store is fixed at level load in every shipped mod's usage.
============================================================================]]

VRBUS = VRBUS or {}
VRBUS.__index = VRBUS

-- Number of event slots mirrored into the key store. Power of two so index
-- wrapping is a cheap AND rather than a modulo.
VRBUS_SLOTS = 16

-- Bump when the slot layout changes so the native side can refuse to read a
-- version it was not built against.
VRBUS_PROTOCOL_VERSION = 1

-- Key namespace. Keep short; every event rewrites 6 keys.
VRBUS_PREFIX = "vr.ev."

--[[---------------------------------------------------------------------------
  Event ids. These integers are the stable wire contract -- the native side
  switches on them. Never renumber, only append.
-------------------------------------------------------------------------------]]
VRBUS_EV_NONE          = 0
VRBUS_EV_DAMAGE_TAKEN  = 1   -- the local player lost health
VRBUS_EV_DAMAGE_DEALT  = 2   -- the local player damaged something else
VRBUS_EV_LANDING       = 3   -- the player hit the ground after falling
VRBUS_EV_FALL_START    = 4   -- the player left the ground going downward
VRBUS_EV_TOOL_FIRE     = 5   -- the equipped tool discharged
VRBUS_EV_TOOL_SWITCH   = 6   -- tool changed
VRBUS_EV_TOOL_IMPACT   = 7   -- the held tool struck geometry
VRBUS_EV_EXPLOSION     = 8   -- an explosion within earshot
VRBUS_EV_BREAK         = 9   -- a tracked body was destroyed
VRBUS_EV_GRAB         = 10   -- grabbed / released a body
VRBUS_EV_JUMP         = 11
VRBUS_EV_FOOTSTEP     = 12
VRBUS_EV_VEHICLE_HIT  = 13   -- something struck the vehicle the player drives
VRBUS_EV_VEHICLE_ENTER = 14
VRBUS_EV_VEHICLE_EXIT  = 15
VRBUS_EV_HEARTBEAT    = 16   -- once per second; proves the channel is alive

--[[---------------------------------------------------------------------------
  Event flags (bitfield, OR'd together into slot.flags)
-------------------------------------------------------------------------------]]
VRBUS_FLAG_NONE       = 0
VRBUS_FLAG_SURFACE    = 1     -- struck surface material is known
VRBUS_FLAG_PLAYER     = 2     -- caused by another player
VRBUS_FLAG_EXPLOSIVE  = 4     -- explosive energy involved
VRBUS_FLAG_REMOTE     = 8     -- cause is far from the player
VRBUS_FLAG_REPEAT     = 16    -- suppressed by a repeat-rate limiter

--[[---------------------------------------------------------------------------
  Which controller an event should rumble.
-------------------------------------------------------------------------------]]
VRBUS_HAND_BOTH  = 0
VRBUS_HAND_LEFT  = 1
VRBUS_HAND_RIGHT = 2

local slot  = {}   -- ring buffer index
local count = 0   -- how many slots are currently published
local seq   = 0   -- monotonic event counter

--[[---------------------------------------------------------------------------
  new() -> bus
    Construct a bus. All key names are derived from VRBUS_PREFIX.
-------------------------------------------------------------------------------]]
function VRBUS.new()
  local self = setmetatable({}, VRBUS)
  self.head  = 0
  self.seq   = 0
  self.count = 0
  self.enabled = true
  return self
end

--[[---------------------------------------------------------------------------
  reset()
    Clear the published ring. Called on level load so a stale ring from the
    previous level is never read by the native side.
-------------------------------------------------------------------------------]]
function VRBUS.reset(self)
  self.head  = 0
  self.seq   = 0
  self.count = 0
  SetInt(VRBUS_PREFIX .. "version", VRBUS_PROTOCOL_VERSION)
  SetInt(VRBUS_PREFIX .. "count", 0)
  SetInt(VRBUS_PREFIX .. "head", 0)
  SetInt(VRBUS_PREFIX .. "seq", 0)
end

--[[---------------------------------------------------------------------------
  emit(id, amp, ttl, hand, pos, flags) -> boolean
    Publish one event. amp is clamped to 0..1, ttl is clamped to >= 0.

    Returns true if the event was published, false if it was dropped (disabled
    or degenerate amplitude).

    Ordering matters and is part of the contract: fields first, count last.
-------------------------------------------------------------------------------]]
function VRBUS.emit(self, id, amp, ttl, hand, pos, flags)
  if not self.enabled then return false end
  if id == nil or id == VRBUS_EV_NONE then return false end

  amp = tonumber(amp) or 0.0
  if amp <= 0.0 then return false end
  if amp > 1.0 then amp = 1.0 end

  ttl = tonumber(ttl) or 0.0
  if ttl < 0.0 then ttl = 0.0 end

  flags = flags or VRBUS_FLAG_NONE
  hand  = hand or VRBUS_HAND_BOTH

  local x, y, z = 0.0, 0.0, 0.0
  if pos ~= nil then
    x = tonumber(pos[1]) or 0.0
    y = tonumber(pos[2]) or 0.0
    z = tonumber(pos[3]) or 0.0
  end

  self.head  = (self.head + 1) % VRBUS_SLOTS
  self.seq   = self.seq + 1
  local base = VRBUS_PREFIX .. "slot." .. self.head .. "."

  -- Payload first. A reader gated on "count" cannot see these until it is set.
  SetInt(base .. "id",    id)
  SetFloat(base .. "amp",  amp)
  SetFloat(base .. "ttl",  ttl)
  SetInt(base .. "hand",   hand)
  SetFloat(base .. "x",    x)
  SetFloat(base .. "y",    y)
  SetFloat(base .. "z",    z)
  SetInt(base .. "flags",  flags)

  -- Publish LAST. This is the release store the native side reads first.
  self.count = self.count + 1
  if self.count > VRBUS_SLOTS then self.count = VRBUS_SLOTS end
  SetInt(VRBUS_PREFIX .. "count", self.count)
  SetInt(VRBUS_PREFIX .. "head",  self.head)
  SetInt(VRBUS_PREFIX .. "seq",   self.seq)
  return true
end

--[[---------------------------------------------------------------------------
  Rate limiting. Explosions and impacts cluster; without this the controller
  motors get a buzz that reads as static rather than as an event.
  Returns true if the caller should proceed.
-------------------------------------------------------------------------------]]
function VRBUS.allow(self, id, minInterval, now)
  if not self._last then self._last = {} end
  local prev = self._last[id]
  if prev ~= nil and (now - prev) < minInterval then
    self._suppressed = self._suppressed + 1
    return false
  end
  self._last[id] = now
  return true
end

--[[---------------------------------------------------------------------------
  setKey / getKey -- generic scalar channel, same store, for values that are
  not events (VR state, user settings). Same publish-last discipline.
-------------------------------------------------------------------------------]]
function VRBUS.setFloat(self, name, value)
  SetFloat("vr.val." .. name, tonumber(value) or 0.0)
end

function VRBUS.getFloat(self, name)
  return GetFloat("vr.val." .. name)
end

function VRBUS.setBool(self, name, value)
  SetBool("vr.val." .. name, value and true or false)
end

function VRBUS.getBool(self, name)
  return GetBool("vr.val." .. name)
end

--[[---------------------------------------------------------------------------
  publishCamera(t)
    Mirrors the per-eye stereo block. The native renderer needs these every
    frame; they are plain scalars at fixed keys rather than slots.

      "vr.cam.ipd"        float  interpupillary distance, meters
      "vr.cam.sep"        float  eye separation (== ipd by default, exposed
                               separately so the user can scale it)
      "vr.cam.fov"        float  horizontal FOV, degrees (what SetCameraFov takes)
      "vr.cam.near"       float  near plane, meters
      "vr.cam.far"        float  far plane, meters
      "vr.cam.x"          float  head x
      "vr.cam.y"          float  head y
      "vr.cam.z"          float  head z
      "vr.cam.pitch"      float  head pitch, radians
      "vr.cam.yaw"        float  head yaw, radians
      "vr.cam.roll"       float  head roll, radians
      "vr.cam.rx"         float  head right axis, x
      "vr.cam.ry"         float  head right axis, y
      "vr.cam.rz"         float  head right axis, z
      "vr.cam.suppress"   int    1 = native side should not stereo-render now
      "vr.cam.valid"      int    1 when the transform is fresh this frame
-------------------------------------------------------------------------------]]
function VRBUS.publishCamera(self, c)
  if c == nil then
    SetInt("vr.cam.valid", 0)
    return
  end
  SetFloat("vr.cam.ipd",  c.ipd  or 0.064)
  SetFloat("vr.cam.sep",  c.sep  or c.ipd or 0.064)
  SetFloat("vr.cam.fov",  c.fov  or 90.0)
  SetFloat("vr.cam.near", c.near or 0.05)
  SetFloat("vr.cam.far",  c.far  or 500.0)
  SetFloat("vr.cam.x",    c.x    or 0.0)
  SetFloat("vr.cam.y",    c.y    or 0.0)
  SetFloat("vr.cam.z",    c.z    or 0.0)
  SetFloat("vr.cam.pitch", c.pitch or 0.0)
  SetFloat("vr.cam.yaw",   c.yaw   or 0.0)
  SetFloat("vr.cam.roll",  c.roll  or 0.0)
  SetFloat("vr.cam.rx",    c.rx    or 1.0)
  SetFloat("vr.cam.ry",    c.ry    or 0.0)
  SetFloat("vr.cam.rz",    c.rz    or 0.0)
  SetInt("vr.cam.suppress", (c.suppressed and 1) or 0)
  -- Published last; same discipline as the event ring.
  SetInt("vr.cam.valid", 1)
end

--[[---------------------------------------------------------------------------
  setHeartbeat(n)
    Increments once per second from main.lua. If this stops moving, the native
    side knows the Lua layer died rather than the channel being broken.
-------------------------------------------------------------------------------]]
function VRBUS.setHeartbeat(self, n)
  SetInt("vr.hb", n)
end
