# Lua Mod Design — Teardown VR

**Status:** written and unit-tested off-game. **Never run in the live game.**
Date: 2026-09-27. Applies to `lua/` in this repo.

---

## 1. What this is, and why it exists

Teardown exposes an official Lua modding API. The game loads a mod folder, runs
its scripts every frame, and hands them a large set of engine bindings. This is
the layer where VR logic that does not need to touch the renderer belongs:
head-pose tracking, game-event detection, and the haptics event channel that the
native DLL (`hook/teardown_vr.c`) forwards to the VR controllers.

**The single most important finding of the research phase is that the game
already has a complete haptic engine.** The binary registers `LoadHaptic`,
`PlayHaptic`, `PlayHapticDirectional`, `CreateHaptic`, `StopHaptic`,
`HapticIsPlaying` and `SetToolHaptic`, and ships 57 declarative `.xml` effect
files under `data/haptic/`. The format maps almost 1:1 onto OpenXR haptics. So
the haptics problem is not "can we make the pad buzz" — it already can. The
problem is that those effects are authored for gamepad rumble and we want them
driven by OpenXR controllers instead.

That reframes the Lua side: Lua's job is not to synthesize haptics, it is to
**decide *when* something haptic should happen and how strong it should be**, and
hand that decision to the native side. `vrhaptics.lua` does exactly that, and
optionally plays the game's own effects as a fallback so the mod is useful with
no DLL attached.

---

## 2. Ground truth for the API

Everything below was verified against files on disk, not recalled.

**Authoritative source:** `<Teardown>/data/script_defs.luau` — 11,447 lines, 752
documented functions with LuaLS-style `---@param` / `---@return` annotations and
worked examples. The game ships this as its API stub. It is far more reliable
than the binary's string table and it is what every "VERIFIED" claim in the Lua
source cites by line number.

**Corroborating evidence:** 2,838 shipped Lua files under `mods/` and `dlcs/`,
and `strings` over `teardown.exe.unpacked.exe`.

A mechanical audit (comments and string literals stripped, then every global
call extracted) confirms **every game function this mod calls appears in
`script_defs.luau`** — both direct calls and those passed by name to `pcall`.
There are no invented APIs. This audit is reproducible and is re-run by
`test/test_run.lua` ("API surface audit").

### Two corrections to earlier assumptions in this project

| Assumption | Reality |
|---|---|
| Mod metadata is JSON | It is a plain `info.txt` with `key = value` lines (`en_name`, `author`, `tags`, `version`, `preview`). See `mods/minigun/info.txt`. |
| Callbacks are `onUpdate` / `onRender` | A `main.lua` mod uses bare globals: `init`, `tick`, `update`, `draw`. See `mods/speedometer/main.lua`. The `client.` / `server.` prefixed form belongs to level scripts. |

### The pre-existing `lua_mod/teardown_vr.lua` was wrong

There was an earlier draft at `lua_mod/teardown_vr.lua` calling
`SetRegistryBool`, `GetRegistryBool`, `GetRegistryFloat`, `SetRegistryInt`.
**None of those functions exist.** Not in `script_defs.luau`, not in the binary's
string table, not in any of the 2,838 shipped mod scripts. The mod would have
called `nil` on its first line. It is left in place, untouched, as a record of
the wrong assumption; nothing references it.

The real store is `SetInt` / `GetInt` / `SetFloat` / `GetFloat` / `SetBool` /
`GetBool` / `SetString` / `GetString` / `HasKey` (`script_defs.luau:630–730`).
`SetInt`'s own doc example is `SetInt("score.levels.level1", 4)`, and shipped
mods demonstrably read a key written by another file
(`mods/folkrace/config/events.lua:78,86`). So it is a real cross-script channel,
not a per-script sandbox.

---

## 3. File layout

```
lua/
  info.txt              mod metadata, Teardown's key = value format
  main.lua              entrypoint: module load, frame loop, settings overlay
  vrbus.lua             the event bus + stereo block  (THE CONTRACT)
  vrhaptics.lua         event detection -> haptic events
  vrcamera.lua          head pose + per-eye parameters
  haptic/               8 custom .xml effects + README.txt
  merge.py              builds the single-file variant
  run_tests.sh          syntax check + both suites
  test/
    mock_teardown.lua   mock of the API, same signatures as script_defs.luau
    test_run.lua        64 assertions against the modular build
    test_merged.lua     23 assertions against the merged build
```

Install as `<Teardown>/mods/teardown_vr/`. `info.txt` and `main.lua` must be at
the mod root; the rest is looked up under the mod's virtual `MOD/` root.

---

## 4. The Lua → native event protocol

### 4.1 Why the key store

A mod has no shared memory, no socket, no writable file, no FFI. The only thing
that escapes the Lua sandbox into engine memory is the key/value store, because
the engine owns it. So the native side must read the same keys Lua writes, by
locating that store in the process. **This is the one thing not yet implemented:**
`grep -i "lua\|registry" hook/teardown_vr.c` returns **zero** matches. The native
side has no Lua, registry, or haptics code at all. The protocol below is
specified and ready; nobody is reading it yet.

### 4.2 Event ring

A 16-slot ring mirrored into the store at a fixed stride.

| Key | Type | Meaning |
|---|---|---|
| `vr.ev.version` | int | protocol version (currently `1`) — refuse to read a version you were not built for |
| `vr.ev.count` | int | valid slots, `0..16` |
| `vr.ev.head` | int | index of the newest slot |
| `vr.ev.seq` | int | events emitted since load, monotonic |
| `vr.ev.slot.<i>.id` | int | event id, see below |
| `vr.ev.slot.<i>.amp` | float | intensity `0..1` |
| `vr.ev.slot.<i>.ttl` | float | suggested duration, seconds |
| `vr.ev.slot.<i>.hand` | int | `0` both, `1` left, `2` right |
| `vr.ev.slot.<i>.x/y/z` | float | world position of the cause, metres |
| `vr.ev.slot.<i>.flags` | int | `VRBUS_FLAG_*` bitfield |

Event ids — **stable wire contract, never renumber, only append:**

| id | event | id | event |
|---|---|---|---|
| 1 | `DAMAGE_TAKEN` | 9 | `BREAK` |
| 2 | `DAMAGE_DEALT` | 10 | `GRAB` |
| 3 | `LANDING` | 11 | `JUMP` |
| 4 | `FALL_START` | 12 | `FOOTSTEP` |
| 5 | `TOOL_FIRE` | 13 | `VEHICLE_HIT` |
| 6 | `TOOL_SWITCH` | 14 | `VEHICLE_ENTER` |
| 7 | `TOOL_IMPACT` | 15 | `VEHICLE_EXIT` |
| 8 | `EXPLOSION` | 16 | `HEARTBEAT` |

Flags: `1 SURFACE`, `2 PLAYER`, `4 EXPLOSIVE`, `8 REMOTE`, `16 REPEAT`.

### 4.3 The read protocol (no locks)

A native reader does, once per frame:

1. read `vr.ev.count` → `n`
2. read slots `0 .. n-1` (plus `head` to know which are oldest)
3. clear by writing `vr.ev.count = 0`, or ignore events with `seq <= lastSeq`

**The writer publishes count LAST, after all six payload fields of a slot are
written.** So a reader that reads count first and only reads slots below it can
never observe a half-written event. That single ordering rule is the entire
concurrency contract. No locks, no atomics, no allocation, nothing to tear down.
The same rule governs the camera block (`vr.cam.valid` last) and the heartbeat.

If a reader is worried about the 32-bit store being torn, the practical answer is
that it should poll `seq` before and after and retry if it changed — `seq` is
only ever incremented after a complete event is visible.

### 4.4 Stereo block

Published every frame from `vrcamera.lua`:

| Key | Meaning |
|---|---|
| `vr.cam.ipd` / `vr.cam.sep` | eye separation, metres (default `0.064`) |
| `vr.cam.fov` | **horizontal** FOV, degrees — `SetCameraFov` documents horizontal (`script_defs.luau:8398`) |
| `vr.cam.near` / `vr.cam.far` | clip planes, metres |
| `vr.cam.x/y/z` | head position |
| `vr.cam.pitch/yaw/roll` | radians |
| `vr.cam.rx/ry/rz` | head right axis, unit length |
| `vr.cam.suppress` | 1 = native side should not stereo-render now |
| `vr.cam.valid` | 1 when the pose is fresh this frame; 0 = hold last frame |

The right axis is published rather than left to the native side to derive,
because deriving it means agreeing on a quaternion convention, and that
convention is not documented anywhere.

`vr.cam.valid` goes to 0 whenever `GetPlayerEyeTransform` fails, so the native
side holds its last good frame instead of rendering a garbage one.

### 4.5 Heartbeat and health

`vr.hb` advances once per second. If it stops, the Lua layer died. If it is
alive but `vr.ev.count` stays 0, nothing is happening. Those are different faults
and the native log should say which.

User settings round-trip through the same store under `vr.user.*`
(`enabled`, `ipd_mm`, `fov`, `sep_scale`), so they survive a level reload the
way any mod setting does.

---

## 5. Event detection — and its honest weakness

**The engine exposes no damage, explosion, collision, landing or weapon-fired
callback to Lua.** Verified: no such name exists in `script_defs.luau` or in the
binary's Lua name table. The only event API is `RegisterListenerTo` /
`UnregisterListener` / `TriggerEvent` (`script_defs.luau:8752–8792`), and all
three are marked **DEPRECATED**. The engine contributes exactly one event name
(`eLanguageChanged` in a 4-line `data/events.lua`). The 63-constant `EVENTS`
table that mods reference is folkrace's own private vocabulary, not a game API.

So every trigger here is a **poll and diff**:

| trigger | signal | source |
|---|---|---|
| damage taken | first difference of `GetPlayerHealth`, 0..1 | `script_defs.luau:5876` |
| landing | airborne + downward velocity, then a ground raycast hits | `GetPlayerVelocity` :5566, `QueryRaycast` :7363 |
| jump | rise past 4 m/s from a grounded state | same |
| footsteps | horizontal distance while grounded, every 2.1 m | same |
| tool fire | `GetToolAmmo` decreases | :6386 |
| tool switch | `GetPlayerTool` changes | :5926 |
| tool impact | `WatchBodyHit` / `GetBodyHit` impulse | **undocumented**, see below |
| vehicle enter/exit | `GetPlayerVehicle` 0 ↔ non-zero | :5627 |

This is the mod's main weakness and it should not be glossed over. Polling
misses sub-frame events, and the first-difference on health cannot tell *what*
hit you. It is the only thing available from this layer, and moving event
detection into the native side — where the real engine callbacks are — is the
obvious next step.

**Not implemented, rather than guessed:** explosion proximity (no
`GetFireCount`-style explosion query is exposed), object destruction (no
destruction event; would need every dynamic body tracked, which is an O(level)
scan per frame), and vehicle damage (vehicle health is not in `script_defs.luau`
at all).

### 5.1 The one genuinely undocumented API used

`WatchBodyHit`, `IsBodyHitted`, `GetBodyHitsCount` and `GetBodyHit` are declared
in the stub as bare `function f(...) end` with **no annotations and no examples**.
Their call convention is taken from three shipped mods that use them identically
(`mods/folkrace/m_village/script/village_grinder.lua:46-53`):

```lua
WatchBodyHit(body)
if IsBodyHitted(body) then
  for index = 0, GetBodyHitsCount(body) - 1 do
    local other_body, impulse, pos, normal, this_vel, other_vel = GetBodyHit(body, index)
```

The whole path is `pcall`-guarded per call. If the convention is wrong it throws,
the guard catches it, and tool-impact haptics quietly stop working. It does not
take the channel down. **This is the first thing to test in-game.**

---

## 6. Haptics: two paths

**Path A — publish to the bus (default).** Every detection becomes a
`(id, amp, ttl, hand, pos, flags)` record. The native side maps that to an
OpenXR haptic. This is the real path.

**Path B — play the game's own effects (opt-in, `tab` in the overlay).** Calls
`PlayHaptic(handle, amp)` so the mod is demonstrable on a gamepad with no DLL.
This works today, because the haptic engine is the game's, not ours.

`amp` is the detector's intensity scaled by the catalog entry's base amplitude.
`ttl` is advisory metadata for the native side only — `PlayHaptic` has no
duration argument; the curve's `<lifetime>` in the `.xml` owns the real duration
on the game-side path.

`haptic/` contains 8 hand-authored effects (VR-shaped landings, a heavier
explosion, a tool-fire curve with the left motor deliberately quieter so the
trigger hand reads as dominant). Format verified against all 57 vanilla files:
`<haptic_effect>`, `<lifetime>`, `<keypoints type="motor" index="0|1">`,
`<point pos="T V"/>` — **two space-separated floats, not comma-separated.**
Only four attribute names exist across the whole vanilla corpus (`pos`, `type`,
`index`, `src`) and these use nothing outside that set. They are not yet wired
into the catalog because a mod-local `.xml` has not been confirmed to load; see
§8.

---

## 7. Testing

`sh run_tests.sh` — **87 assertions, all passing** (64 modular + 23 merged).

`test/mock_teardown.lua` reimplements the API subset with the same signatures as
`script_defs.luau`, plus a configurable `mod_assets_available` flag to simulate
the mod asset root being mounted or not. The suites drive the real mod code
through damage, falls, landings, jumps, footsteps, tool fire, tool switch, tool
impacts, vehicle transitions, ring overflow, and a 1.5 Hz heartbeat.

The tests earn their keep — they caught a real bug that would have shipped:

> `VRHAPTICS.entryFor` was declared without `self` but called with a colon. Every
> event silently fell through to the unknown-id path, so **every event shipped
> with `ttl = 0` and `hand = 0` (both hands)**. The camera and native side would
> have received events with no duration and no hand routing. Fixed, and the
> suite now asserts `ttl > 0` and the per-event hand specifically to catch that
> class of bug.

They also cover the three failure modes that matter:
- a detector whose API throws → `tick` still completes;
- haptic assets that fail to load → counted, not fatal, events still published;
- **every module failing to load** → emergency bus, heartbeat still alive, no
  crash, failure reported on screen.

The last suite runs the **merged** build with `dofile` hard-disabled, because that
build is the fallback for §8's open question and a broken fallback is worse than
none.

---

## 8. Open questions — what still needs the real game

Ordered by how much they block everything else.

**1. Can the native side find the key store in memory? — blocks the entire
premise.** `hook/teardown_vr.c` has no Lua, registry, or haptics code. Until
someone locates the store the Lua bindings use, the protocol in §4 is
specified and untested. Everything else is downstream of this.

**2. Can a `main.lua` mod `dofile` its own `.lua` files? — blocks the haptics
detectors.** Not established by any shipped mod: 2,838 scripts, 13 `main.lua`
mods, and the only `dofile` calls in the entire corpus use an absolute path from
`GetString("game.levelpath")`, a different mechanism that does not apply here.
Handled two ways rather than guessed: `main.lua` probes candidate paths under
`pcall` at runtime, and `merge.py` builds a single-file build with zero `dofile`
dependencies (tested, §7). **Install the merged build if in doubt.**

**3. Does a mod-local `haptic/*.xml` load?** Vanilla paths work — proven by
`mods/folkrace/script/gadget_nitro.lua:76` loading `haptic/vehicle_turbo.xml`,
a file that exists only in `data/haptic/`. Mod-local `MOD/haptic/...` is used by
`mods/minigun/main.lua:100` and `mods/tg/script/gloogun.lua:35` but was never
confirmed here, so the catalog is split: mod-local paths for the custom effects,
vanilla for anything else.

**4. `WatchBodyHit` / `GetBodyHit` signatures** (§5.1) — the whole tool-impact
trigger. Guarded, needs a live test.

**5. `<lifetime>` units.** Assumed seconds. Values are plausible and every
sibling API documents seconds, but no doc comment exists for the XML schema.

**6. `PlayHapticDirectional`'s reference frame.** Signature is documented
(`(handle, direction: TVec, amplitude)`, `script_defs.luau:8871`) but has **zero
call sites in 2,838 shipped scripts**. Direction is assumed world-space. It is
off by default behind `self.directional` for exactly this reason.

**7. Camera units and axes.** Metres and radians, inferred from consistent usage
(speedometer converts m/s→km/h with `*3.6`; every shipped ground probe uses
`Vec(0,-1,0)` as down). No stub states units for transforms. `x` right, `y` up,
`-z` forward.

**8. Near/far planes.** No documented accessor. The defaults (`0.05` / `500`) are
the mod's own choice and should be replaced with the renderer's real projection
constants — see `docs/PROJECTION_DATA_PATH.md` and `docs/BUFFER_LAYOUT.md`.

**9. Does `draw()` run when the HUD is hidden** (pause, respawn)? If the overlay
misbehaves, gate it on `GetString("game.state")`. That key is read by shipped mods
but is not in the stub, which is why it is not used yet.

**10. Tuning is by judgement, not measurement.** Every threshold in
`VRHAPTICS.TUNE` (0.01 damage epsilon, 3–22 m/s landing range, 2.1 m stride,
0.06–0.30 s rate limits) is a reasoned guess from shipped code, and the haptic
curves in `haptic/` were designed by reading XML, not by listening to hardware.
Expect to retune all of it with the headset on.

---

## 9. If something is wrong, check in this order

| symptom | likely cause |
|---|---|
| `DebugPrint` shows `MODULE LOAD FAILED` | `dofile` did not resolve → install `merged/` |
| overlay says `cam=OFF` / `haptics=OFF` | a module failed; the error text is on screen |
| `vr.hb` not advancing | `init()` never ran, or the mod is not enabled in the game's mod list |
| `vr.hb` advancing, no events | detection thresholds too strict, or the player is genuinely idle |
| `vr.hb` advancing, `vr.cam.valid = 0` | `GetPlayerEyeTransform` is failing every frame |
| native side sees nothing at all | the key store was never located — see open question 1 |
| tool-impact haptics silent, everything else fine | `WatchBodyHit` convention is wrong — see §5.1 |

On screen: `menu_cancel` toggles the overlay. Inside it, `up`/`down` change FOV,
`r`/`f` change eye separation, `tab` toggles gamepad haptic playback.
