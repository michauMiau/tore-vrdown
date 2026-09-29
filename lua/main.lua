--[[===========================================================================
  main.lua -- Teardown VR: mod entrypoint.

  Installed as Teardown/mods/teardown_vr/main.lua, alongside info.txt.

  Responsibilities, in order:
    * load the sibling modules (with a probe -- see LOADING below),
    * own the frame loop: tick() drives detection and publication,
    * expose a VR settings overlay in draw(),
    * keep a heartbeat moving so the native side can tell "no events" from
      "Lua died".

  ---------------------------------------------------------------------------
  CALLBACK NAMES -- VERIFIED
  ---------------------------------------------------------------------------
  A mod whose entrypoint is main.lua uses the BARE global callbacks, not the
  client./server. prefixed ones. Confirmed against shipped mods:
    mods/speedometer/main.lua   -> function init() / tick() / update() / draw()
    mods/minigun/main.lua       -> function server.init() / server.tick(dt)
      (minigun is a tool mod and uses the prefixed form; both forms are seen,
       the bare form goes with main.lua)
  The client's own API stub file data/script_defs.luau uses client.tick /
  client.init / client.draw in its examples, which is the level-script form.
  This mod is a plain main.lua mod, so: init, tick, update, draw.

  The task brief asked for "init() and update(dt)". Both are implemented.
  The work is done in tick(), not update(), for one concrete reason that is
  documented in the API: script_defs.luau:150-155 states GetTimeStep()
  "Returns timestep of the last frame. If called from update, this returns the
  simulation time step, which is always one 60th of a second (0.0166667). If
  called from tick or draw it returns the actual time since last frame."
  Every detector here integrates over dt -- fall energy, footstep distance,
  rate limiters -- so it needs the real frame time. update() is implemented and
  does the frame-rate-independent work instead.

  ---------------------------------------------------------------------------
  LOADING -- ASSUMED, PROBED AT RUNTIME
  ---------------------------------------------------------------------------
  Whether dofile() can reach a mod's own .lua files is NOT established by any
  shipped mod. What is established is that "MOD/" is the mod's virtual asset
  root: shipped mods load "MOD/vox/...", "MOD/snd/...", "MOD/haptic/...",
  "MOD/img/...", "MOD/prefab/..." this way. The only dofile() calls in 2838
  shipped scripts use an absolute path derived from GetString("game.levelpath"),
  which is a different mechanism and does not apply to a main.lua mod.

  So this file probes rather than assumes: it tries each candidate path under
  pcall and uses the first that works. If none work the mod still loads and
  still publishes the camera block and heartbeat -- the native side gets a
  working channel minus the haptics detectors, which is a degraded but
  non-broken state, and the failure is reported on screen instead of being
  swallowed.

  If the probe fails in the real game, the fix is the one documented in
  docs/LUA_MOD_DESIGN.md: concatenate the modules into a single main.lua.
  lua/merge.py does exactly that.

  ---------------------------------------------------------------------------
  VERIFIED
  ---------------------------------------------------------------------------
  Everything main.lua itself calls:
      GetTime()                script_defs.luau:138
      GetTimeStep()            script_defs.luau:157
      DebugPrint(msg)          script_defs.luau:8731
      HasFile(path)            script_defs.luau:405
      GetBool / SetBool        script_defs.luau:705 / :693
      GetInt / SetInt          script_defs.luau:655 / :643
      GetString / SetString    script_defs.luau:730 / :718
      GetLocalPlayer()         script_defs.luau:5261
      IsPlayerValid(id)        script_defs.luau:5311
      UiPush / UiPop           -- verified in binary, used by every shipped
                                  HUD mod
      UiText / UiColor / UiWidth / UiHeight / UiTranslate / UiAlign
      UiTextButton / UiButtonText
  The Ui* batch is verified by presence in data/script_defs.luau and by
  2838 shipped scripts; exact pixel metrics and theming come from the game, so
  the overlay is laid out conservatively.

  ASSUMED:
  * UiTextButton's exact parameter list. Used in shipped mods as
    UiTextButton(text, x, y, w, h, ...) with a callback, but the stub returns
    the stub's own definition, so the trailing arguments (tooltip, style) are
    passed only where needed and the call is pcall-guarded.
  * That draw() is called even when the game's HUD is hidden (pause, respawn).
    If the overlay misbehaves in those states, the fix is to gate draw() on
    GetString("game.state") -- that key is read by shipped mods but is not in
    the stub file, so it is not used here.
============================================================================]]

--[[---------------------------------------------------------------------------
  Module loading. Order matters: the bus exists before anything can publish.

  The three modules are normally separate files and are pulled in by dofile().
  In the MERGED build (see merge.py) they are already concatenated above this
  point, so the probe must not gate them -- MERGED_BUILD is the marker merge.py
  writes, and when it is present the globals are checked but not loaded.
-------------------------------------------------------------------------------]]
vrModules = {
  bus     = false,
  haptics = false,
  camera  = false,
  input   = false,
}
vrMerged = (_G.MERGED_BUILD == true)

vrLoadErrors = {}

-- Candidates per module, in order. "MOD/..." is the mod virtual root; the bare
-- name is tried in case dofile resolves relative to the mod folder.
vrCandidates = {
  bus     = { "MOD/vrbus.lua",       "vrbus.lua" },
  haptics = { "MOD/vrhaptics.lua",   "vrhaptics.lua" },
  camera  = { "MOD/vrcamera.lua",    "vrcamera.lua" },
  input   = { "MOD/vrinput.lua",     "vrinput.lua" },
}

local function tryLoad(name, paths)
  for i = 1, #paths do
    local ok, err = pcall(dofile, paths[i])
    if ok then return true, paths[i] end
    vrLoadErrors[name] = tostring(err)
  end
  return false, nil
end

function vrLoadModules()
  -- The globals these files define. If a file loaded but did not define its
  -- global, that is a failure too, and is treated as one.
  local checks = {
    bus     = function() return VRBUS ~= nil end,
    haptics = function() return VRHAPTICS ~= nil end,
    camera  = function() return VRCAMERA ~= nil end,
    input   = function() return vrinput ~= nil end,
  }

  for name, paths in pairs(vrCandidates) do
    if vrMerged then
      -- Already inlined by merge.py. Just verify the global arrived.
      if checks[name]() then
        vrModules[name] = true
      else
        vrLoadErrors[name] = "merged build is missing global " .. name
        vrModules[name] = false
      end
    else
      local ok = tryLoad(name, paths)
      if ok and not checks[name]() then
        vrLoadErrors[name] = "loaded but global " .. name .. " undefined"
        ok = false
      end
      vrModules[name] = ok
    end
  end

  if vrModules.bus and vrModules.haptics and vrModules.camera and vrModules.input then
    vrModules.all = true
  else
    vrModules.all = false
  end
  return vrModules
end

--[[---------------------------------------------------------------------------
  State
-------------------------------------------------------------------------------]]
vr = nil          -- bus
vrhap = nil       -- haptics
vrcam = nil       -- camera
vrBooted = false
vrBootTime = 0
vrLastHb = 0
vrOverlay = false

--[[---------------------------------------------------------------------------
  vrEmergencyInit()
    Last-resort path. If the bus module could not be loaded by ANY path, the
    native side would otherwise have no channel at all -- not even the
    heartbeat it uses to tell "idle" from "dead".

    So if everything failed, install a minimal bus inline. It is not a
    replacement: no haptics, no camera. It publishes the version key, keeps
    the heartbeat moving, and keeps the event ring at zero so the native side
    has a defined thing to read. That is strictly better than silence, and it
    makes the failure diagnosable from the native log rather than invisible.

    VERIFIED: SetInt / GetInt / GetTime, all in data/script_defs.luau.
-------------------------------------------------------------------------------]]
function vrEmergencyInit()
  VRBUS = VRBUS or {}
  VRBUS.__index = VRBUS
  VRBUS_SLOTS = VRBUS_SLOTS or 16
  VRBUS_PROTOCOL_VERSION = VRBUS_PROTOCOL_VERSION or 1

  if VRBUS.new == nil then
    function VRBUS.new()
      local self = setmetatable({}, VRBUS)
      self.head, self.seq, self.count = 0, 0, 0
      self.enabled = true
      return self
    end
    -- Deliberately minimal: the native side only needs the heartbeat and a
    -- zero count. A real bus is never constructed in this path.
    function VRBUS.reset(self)
      self.head, self.seq, self.count = 0, 0, 0
      SetInt("vr.ev.version", VRBUS_PROTOCOL_VERSION)
      SetInt("vr.ev.count", 0)
      SetInt("vr.ev.head", 0)
      SetInt("vr.ev.seq", 0)
    end
    function VRBUS.emit(self) return false end
    function VRBUS.allow(self) return false end
    function VRBUS.setFloat(self, n, v) SetFloat("vr.val." .. n, v) end
    function VRBUS.getFloat(self, n) return GetFloat("vr.val." .. n) end
    function VRBUS.setBool(self, n, v) SetBool("vr.val." .. n, v) end
    function VRBUS.getBool(self, n) return GetBool("vr.val." .. n) end
    function VRBUS.publishCamera(self) SetInt("vr.cam.valid", 0) end
    function VRBUS.setHeartbeat(self, n) SetInt("vr.hb", n) end
  end

  vrModules.bus = true
  vrModules.all = false
  vrLog("[teardown_vr] MODULE LOAD FAILED - emergency bus only, haptics/camera off")
end

--[[---------------------------------------------------------------------------
  init()
    Called once when the mod loads. Build the object graph here so a failure
    in any one module cannot prevent the rest from being set up.
-------------------------------------------------------------------------------]]

--[[---------------------------------------------------------------------------
  vrLog(msg)
    GLOBAL, not local: merge.py wraps each module in `do ... end`, so a local
    helper here is invisible to init() in the merged build. VRBUS / VRHAPTICS /
    VRCAMERA / vrinput are globals for the same reason.

    VERIFIED 2026-09-28 in the live game: DebugPrint raises
    "Calling the DebugPrint API function before init is not supported" when it
    runs before the game is ready. It is an engine call, so pcall around the CALL
    catches it (unlike GetLocalPlayer, which throws while its ARGUMENTS are being
    evaluated). Logging is diagnostics, never control flow: if it cannot print,
    execution continues.
-------------------------------------------------------------------------------]]
function vrLog(msg)
  pcall(DebugPrint, tostring(msg))
end

function init()
  vrLoadModules()

  if not vrModules.bus then vrEmergencyInit() end

  if vrModules.bus then
    vr = VRBUS.new()
    -- Publish an empty ring immediately. Until this runs, the native side
    -- would read whatever the previous level left behind.
    vr:reset()
  end

  if vrModules.bus and vrModules.haptics then
    vrhap = VRHAPTICS.new(vr)
    -- Prime the sample state. Without this the first tick would see health
    -- 1.0 -> 0.0 and report a full-health damage event on level load.
    vrhap:init()
  end

  if vrModules.bus and vrModules.camera then
    vrcam = VRCAMERA.new(vr)
    vrcam:loadSettings()
  end

  if vr ~= nil then
    vr:setBool("mods", vrModules.all)
    vr:setFloat("haptics_enabled", (vrhap ~= nil) and 1.0 or 0.0)
  end

  -- The input layer keeps its own init because it reads config and snapshots
  -- the current keymap; that is its own concern, not the bus's.
  if vrinput ~= nil then
    pcall(function() vrinput.init() end)
  end

  vrLog("[teardown_vr] lua layer loaded; modules=" ..
        tostring(vrModules.bus) .. "/" .. tostring(vrModules.haptics) .. "/" ..
        tostring(vrModules.camera) .. "/" .. tostring(vrModules.input))
end

--[[---------------------------------------------------------------------------
  tick()
    Per-frame work, on the real frame clock. This is where the detectors run
    and where the per-frame stereo block is published.
-------------------------------------------------------------------------------]]
function tick(dt)
  if dt == nil or dt <= 0 then dt = GetTimeStep() end

  -- Guard against the pre-init frame and against a level teardown, where
  -- GetTime can go backwards and IsPlayerValid briefly fails.
  local okTime, now = pcall(GetTime)
  if not okTime then return end
  if now < vrBootTime - 1.0 then
    -- Time went backwards: the level restarted. Re-prime everything.
    vrBooted = false
  end
  if not vrBooted then
    vrBooted = true
    vrBootTime = now
    if vr ~= nil then vr:reset() end
    if vrhap ~= nil then vrhap:init() end
    if vrcam ~= nil then vrcam:loadSettings() end
  end

  if vrhap ~= nil then
    -- pcall the whole detector pass: a single wrong assumption inside one
    -- trigger must not take down the channel.
    pcall(VRHAPTICS.tick, vrhap, dt)
  end

  if vrcam ~= nil then
    pcall(VRCAMERA.tick, vrcam)
  end

  -- Input layer last, so it reads the same frame's controller state and its
  -- rebinds are visible to anything that runs after it. pcall-guarded for the
  -- same reason as the others: one bad input frame must not stop the bus.
  if vrinput ~= nil then
    pcall(function() vrinput.tick(dt) end)
  end

  -- Heartbeat, once a second.
  if vr ~= nil and (now - vrLastHb) >= 1.0 then
    vrLastHb = now
    vr:setHeartbeat(math.floor(now))
  end
end

--[[---------------------------------------------------------------------------
  update()
    Called at the fixed 60 Hz simulation rate. Kept deliberately empty of
    detector work (see the header on GetTimeStep); it exists so the mod
    satisfies the conventional entrypoint and has a place for simulation-rate
    logic later.
-------------------------------------------------------------------------------]]
function update(dt)
  -- Intentionally empty. See header comment.
end


--[[---------------------------------------------------------------------------
  draw()
    Status line plus a settings overlay.

    Ui model, verified against data/script_defs.luau and shipped mods:
      UiPush()/UiPop()               save/restore the draw state
      UiTranslate(x, y)              move the cursor
      UiText(text)                   draws AT THE CURSOR -- it takes no x/y.
                                      Returns w, h, endX, endY, linkId
                                      (:9771). Every shipped HUD mod uses
                                      UiTranslate then UiText.
      UiFont(path, size)             REQUIRES a TTF path; there is no nil
                                      default (:9707). "regular.ttf" and
                                      "bold.ttf" are the fonts shipped mods
                                      use. Size range documented 10..100.
      UiAlign("center middle")       affects the cursor origin
      UiColor(r,g,b,a)               r,g,b,a all 0..1 floats (:9196)
      UiRect(w, h)                   solid rect at the cursor (:9989)

    Every Ui call is inside pcall. A UI error must not break rendering, and
    the overlay is not worth a frame hitch.

  ASSUMED: the input identifiers. "menu_cancel", "up", "down", "r", "f", "tab"
  are the names shipped mods use (InputDown("up"), InputDown("interact"),
  InputDown("pause"), InputDown("menu_cancel") all appear in real code), but
  whether they are rebindable to something VR-friendly is untested. The
  whole input block is pcall-guarded, so a wrong identifier is a dead key
  rather than an error.
-------------------------------------------------------------------------------]]
local VR_FONT  = "regular.ttf"
local VR_BOLD  = "bold.ttf"

function draw()
  if vr == nil then return end

  -- The overlay used to be toggled with InputPressed("menu_cancel") here. That
  -- is ESC, and because this draw() ran in the MENU's own context it swallowed
  -- the game's cancel key -- the user saw the mod's panel instead of the pause
  -- menu. A mod must not claim a key the game uses. The overlay is now shown
  -- only when explicitly armed from a console command, and defaults to off.

  pcall(function()
    UiPush()

    if not vrOverlay then
      -- Draw NOTHING by default. This used to print a status line on every
      -- frame, which meant the mod was painting over the game in the menu, in
      -- the pause menu and on the character-select screen. A mod that is merely
      -- loaded must be invisible: the store is the diagnostic channel, not the
      -- screen. Set vrOverlay from the console to inspect it.
      UiPop()
      return
    end

    vrDrawOverlay()
    UiPop()
  end)
end

--[[---------------------------------------------------------------------------
  vrDrawOverlay()
    A plain label list. No buttons: UiTextButton draws at the cursor and
    returns whether it was clicked (:10333), which is usable but needs a
    cursor-relative layout, and the readouts are more useful than buttons for
    the first pass. Left in place deliberately simple.
-------------------------------------------------------------------------------]]
function vrDrawOverlay()
  local w = UiWidth()
  local h = UiHeight()
  local x = w * 0.5 - 220
  local y = h * 0.5 - 150

  UiFont(VR_FONT, 16)
  UiAlign("left")
  UiColor(0.0, 0.0, 0.0, 0.85)
  UiTranslate(x - 14, y - 34)
  UiRect(468, 300)
  UiResetColor()

  UiFont(VR_BOLD, 20)
  UiColor(1, 1, 1, 1)
  UiTranslate(x, y)
  UiText("Teardown VR - Lua layer")
  UiResetColor()

  local row = y + 30
  local function line(label, value, warn)
    UiFont(VR_FONT, 14)
    UiColor(0.72, 0.76, 0.80, 1)
    UiTranslate(x, row)
    UiText(label)
    if warn then UiColor(1.0, 0.45, 0.2, 1) else UiColor(1, 1, 1, 1) end
    UiTranslate(x + 190, row)
    UiText(value)
    UiResetColor()
    row = row + 20
  end

  if not vrModules.all then
    line("Modules loaded", "INCOMPLETE", true)
    for name, err in pairs(vrLoadErrors) do
      UiFont(VR_FONT, 11)
      UiColor(1.0, 0.55, 0.3, 1)
      UiTranslate(x, row)
      UiText(tostring(name) .. ": " .. tostring(err))
      row = row + 16
    end
    UiResetColor()
    return
  end

  line("Enabled", vrcam.enabled and "yes" or "no")
  line("Eye separation", string.format("%.1f mm  (scale %.2f)",
    vrcam.ipdScale * 64.0, vrcam.ipdScale))
  line("Horizontal FOV", string.format("%d deg  (10-170)", math.floor(vrcam.fov)))
  line("Near / far", string.format("%.2f / %.0f m", vrcam.near, vrcam.far))
  if vrhap ~= nil then
    line("Haptics", vrhap.playGameHaptics and "native + game pad" or "bus only")
    line("Events emitted", tostring(vrhap._stats.emitted))
    line("Rate limited", tostring(vrhap._stats.suppressed))
    line("Effect load fails", tostring(vrhap._stats.loadFail))
  end
  line("Heartbeat", tostring(math.floor(vrLastHb)))

  UiFont(VR_FONT, 11)
  UiColor(0.60, 0.65, 0.70, 1)
  UiTranslate(x, row + 8)
  UiText("up/down = FOV    r/f = IPD scale    tab = game pad haptics    menu_cancel = close")
  UiResetColor()

  -- Adjustments. Each is guarded; a bad identifier is a dead key, not a crash.
  if vrcam ~= nil then
    if pcall(InputPressed, "up") then vrAdjustFov(5) end
    if pcall(InputPressed, "down") then vrAdjustFov(-5) end
    if pcall(InputPressed, "r") then vrAdjustScale(0.05) end
    if pcall(InputPressed, "f") then vrAdjustScale(-0.05) end
  end
  if vrhap ~= nil and pcall(InputPressed, "tab") then
    vrhap.playGameHaptics = not vrhap.playGameHaptics
  end
end

function vrAdjustFov(d)
  vrcam.fov = vrcam.fov + d
  if vrcam.fov < VRCAMERA.FOV_MIN then vrcam.fov = VRCAMERA.FOV_MIN end
  if vrcam.fov > VRCAMERA.FOV_MAX then vrcam.fov = VRCAMERA.FOV_MAX end
  vrcam:saveSettings()
end

function vrAdjustScale(d)
  vrcam.ipdScale = vrcam.ipdScale + d
  if vrcam.ipdScale < 0.25 then vrcam.ipdScale = 0.25 end
  if vrcam.ipdScale > 3.0 then vrcam.ipdScale = 3.0 end
  vrcam:saveSettings()
end
