# Lua CAN inject game input — no native shim needed

**This closes the input architecture.** Until now the plan required a native
DLL shim on the ISteamInput vtable (slots 17 and 21) to make a VR controller
drive the game. That is no longer necessary, and the native shim should be
dropped from the critical path.

## The function

`data/script_defs.luau:11180`

```lua
function UiSendInputScreenTouchAction(actionId: string, touchId: number, value: number) end
```

Documented in the game as: *"Send input action with given value and actionId
produced from given touchId."* It takes a **game action name**, not a button
name — `"jump"`, `"crouch"`, `"usetool"` — and it creates the input event.

## It is real, not a menu-only API

The proof that matters is who calls it in the shipped corpus. Not a menu
script, and not a demo — the creative mode HUD, which runs **while you are
playing**:

`data/script/creativemode.lua`

| line | call | context |
|---:|---|---|
| 2678 | `UiSendInputScreenTouchAction(DOWN_TOUCH_ACTION, downTouchId, 1)` | `DOWN_TOUCH_ACTION = "extra1"` (line 277) |
| 2692 | `UiSendInputScreenTouchAction(UP_TOUCH_ACTION, upTouchId, 1)` | `UP_TOUCH_ACTION = "extra0"` (line 276) |
| 2709 | `UiSendInputScreenTouchAction("grab", drawTouchId, 1)` | move an object |
| 2711 | `UiSendInputScreenTouchAction("usetool", drawTouchId, 1)` | use tool |
| 2851 | `UiSendInputScreenTouchAction("crouch", eraseTouchId, 1)` | erase button |
| 2867 | `UiSendInputScreenTouchAction("tool_group_next", saveTouchId, 1)` | save button |

`data/script/spawn.lua:1630` does the same with `"interact"`. The game drives
its own HUD buttons by naming game actions, from Lua, in-game.

## What this means for the shim decision

The two routes to a working VR controller, and what happened to each:

| route | status |
|---|---|
| native shim on ISteamInput slot 17/21 | **no longer required.** verified slots, not built |
| remap via `options.input.keymap.<action>` | works, but a remap cannot *create* events — it only says which key drives which action |
| **`UiSendInputScreenTouchAction`** | **creates events from Lua. this is the route.** |

The remap finding and this one fit together and are not in conflict: a remap
changes what a physical key does, this function manufactures input. The first
was a dead end for injection; the second is the injection primitive.

## What still has to be tested

**Unverified, and it is the whole point of this file.** The doc example
passes a touch id obtained from a real touch:

```lua
local touchId = UiGetScreenTouchIdStartedInRect(100, 100)
if touchId ~= 0 then
    UiSetScreenTouchIdHandled(touchId)
    UiSendInputScreenTouchAction("down", touchId, 1)
end
```

So the open question is whether `touchId` must correspond to a real, live touch
or is only used to attribute the event. Three outcomes, in decreasing order of
how much they simplify the mod:

1. **Any non-zero id works.** Then a VR mod invents ids per controller, holds
   an action for N frames and releases it. Simplest.
2. **The id must be live, but a synthesised touch can be created.** Slightly
   more machinery, same result.
3. **Only genuine touches are accepted.** Then input must ride on a real touch
   — which a VR controller can synthesise once we know how, but that is a
   second problem.

`creativemode.lua:2851` is a hint toward (1): the call happens in the
`elseif UiWasScreenTouchCompletedWithoutLeavingCircle(...)` branch, so
`eraseTouchId` there is `0` — the id that failed the "started in circle" test.
The shipped game passes a **zero** touch id to a function documented to want
one. Whether the game's own HUD is the only caller that can get away with it,
or the id genuinely is only attribution, is not settled by reading.

Test it in-game with a single line, no controller:

```lua
function tick()
    UiSendInputScreenTouchAction("jump", 1, 1)
    UiSendInputScreenTouchAction("jump", 1, 0)
end
```

If the player jumps, case 1 holds and the native shim can be dropped from the
project entirely.

## The action names

The set of injectable actions is the 45 game actions in
`data/input_settings.xml` (documented in `docs/CONTROL_SCHEME.md`) — not the
button names. The full family of related screen-touch calls is
`script_defs.luau:11180-11447`: `UiHasInputScreenTouchAction`,
`UiGetInputScreenTouchAction`, `UiGetScreenTouchIdInRect` and a dozen more.

## The other blocker is untouched

`vrinput.lua` currently **reads and publishes, but injects nothing.** This
document names the call that fixes it. Whether the mod loads at all in the
running game is still unproven — see the note in `docs/STATUS.md`.
