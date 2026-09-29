# The frame host is the level, not menu.lua

Measured 2026-09-28, and it refutes the assumption the whole bootstrap was
built on.

## The measurement

Independent counters injected into each entry point of `data/ui/menu.lua` —
nothing to do with the mod, so a zero is a fact about the file:

```
tdvm_init     = 1     <- init() ran
tdvm_tick1    ABSENT <- tick() never ran
tdvm_draw1    ABSENT <- draw() never ran
```

`menu.lua` **compiles, is loaded, and its `init()` executes** — the mod starts
there and `tdvrstate` proves it. But its `tick()` and `draw()` are never called.
So `menu.lua` is a state machine the engine drives with its own callbacks, and
global `tick`/`draw` in that file are not the frame loop.

This is why `tdvrbeat` read `ticks=0 draws=0` for hours while `tdvrstage`
read `started`: both were true at once, and nothing was wrong with the mod.

## Where frames actually live

```
data/level/<level>/script/*.lua
```

Level scripts define global `tick(dt)` / `draw()` and the engine calls them
while that level is loaded. `data/level/hub_carib/script/nointernet.lua` is a
`draw()`-only overlay; `mall/script/` exists alongside `mall/prefab/`, so the
`script/` sibling is the convention, not a coincidence. **These only run inside
a level** — which is the chicken-and-egg problem: the mod needs a level to get
a frame loop, and getting into a level needs frames.

## What this rules out

- Bootstrapping from `common.lua` — it has no lifecycle at all.
- Bootstrapping from `menu.lua` — works for `init()`, useless for frames.
- Reading a missing heartbeat as a mod fault. The mod was never ticked.

## The shape that follows

Two hosts, for two different jobs:

1. **`menu.lua` for startup.** Load the mod, read the store, run the autopilot.
   This part is measured working.
2. **A level script for the frame loop.** Ship
   `data/level/<level>/script/tdvr_vr.lua` (or put the driver in the mod and
   have the level script call into it). Only then does `tick`/`draw` have a
   host, and only then do camera and haptics have anything to run on.

The autopilot (`tdvr_campaign.lua`) already calls the engine's own
`saveAndStartLevel` → `StartLevel` path, which is what `menu.lua:966` does when
you pick a mission. It fires from the mod's tick, so it needs the same thing
every other part needs: one real level, entered once, to bootstrap the rest.

## The rule

**A global `tick`/`draw` is only a frame host if the engine is in a level.**
Nothing in the menu calls it. Check which of the three hosts you are in before
reading any counter as evidence.
