Teardown VR -- custom haptic effects
===================================

These are VR-shaped replacements for some of the game's built-in haptics,
authored in the game's own declarative format so they load with
LoadHaptic("MOD/haptic/<name>.xml").

They are NOT currently wired into VRHAPTICS.CATALOG. The catalog still points
at the vanilla built-in paths (haptic/gun_fire.xml, haptic/damage_fall.xml, ...)
because those are proven to resolve: mods/folkrace/script/gadget_nitro.lua:76
loads "haptic/vehicle_turbo.xml", a file that exists only in the vanilla
data/haptic/ directory, which proves the bare path form works.

Switching the catalog to these files is a one-line-per-row change, but it must
first be confirmed in a live game that a mod-local .xml loads at all. That is
listed as an open question in docs/LUA_MOD_DESIGN.md. Do not assume it.

FORMAT -- verified against the 57 files in the game's data/haptic/ directory

    <haptic_effect>
      <lifetime>SECONDS</lifetime>              -- root level, plain float
      <keypoints type="motor" index="0">        -- index 0 = left, 1 = right
        <point pos="T V"/>                      -- T,V are two floats, SPACE
                                                  -- separated, not comma
      </keypoints>
      <keypoints type="motor" index="1"> ... </keypoints>
      [<advanced_vibration><file src="..."/></advanced_vibration>]
      [<adaptive_trigger type="weapon|feedback|vibration" index="0|1"> ... </adaptive_trigger>]
    </haptic_effect>

    T is normalized time 0..1 across the effect, V is amplitude 0..1.
    Points interpolate; order is ascending in T in every vanilla file.
    Exactly four attribute names appear across all 57 vanilla files: pos, type,
    index, src. Nothing here uses anything outside that set.

    The advanced_vibration src paths (advanced/tools/gun0.wav) resolve against
    a root that is NOT the haptic directory and that could not be located on
    disk. None of these effects use advanced_vibration, so nothing depends on
    that unresolved base.

WHY THESE EXIST

  vr_landing          sharp 1->0 over 0.25 s. The vanilla damage_fall.xml is a
                      single point at full amplitude for 0.1 s, which reads as
                      a click rather than an impact.
  vr_damage           duller and longer than landing -- you should feel hurt,
                      not thumped.
  vr_explosion        0.6 s with a long tail, vs the vanilla 1 s flat.
  vr_tool_fire        left motor deliberately quieter (0.3 vs 0.7) so the
                      trigger hand reads as dominant.
  vr_tool_impact      single crisp tick per struck surface.
  vr_footstep         very light, 0.06 s.
  vr_grab             soft click.
  vr_vehicle_impact   heavy, 0.35 s.

These curves are tuned by ear-equivalent judgement, not measured against real
hardware. They are a starting point. Expect to iterate.
