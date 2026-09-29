#!/usr/bin/env python3
"""S10 - THE DELIVERABLE TABLE.

Join three independent, separately-proven facts:
  1. teardown.exe: the ISteamInput interface vtable calls, found by the
     double-deref dataflow fingerprint inside .pdata-bounded functions.
  2. steam_api64.dll: slot -> method name, resolved from the vtable the
     SteamInput006 factory actually installs (0x54B738, 48 entries).
  3. teardown.exe: which functions reach the SteamInput accessor slot 0xC48408,
     i.e. which of these interface calls are provably ISteamInput.
"""
import collections
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, iat, pdata, raw, riprefs, s

pd = pdata()
recs = tdlib.recs()
IM = tdlib.iat()
BASE = tdlib.img().image_base

# ---- slot -> name, from the proved vtable (48 entries) ---------------------
VT = {
    0: "Init", 1: "Shutdown", 2: None, 3: "RunFrame", 4: None, 5: None,
    6: "GetConnectedControllers", 7: None, 8: None, 9: "GetActionSetHandle",
    10: "ActivateActionSet", 11: "GetCurrentActionSet", 12: None, 13: None,
    14: None, 15: "GetActiveActionSetLayers", 16: "GetDigitalActionHandle",
    17: "GetDigitalActionData", 18: "GetDigitalActionOrigins", 19: None,
    20: "GetAnalogActionHandle", 21: "GetAnalogActionData",
    22: "GetAnalogActionOrigins", 23: None, 24: "GetGlyphSVGForActionOrigin",
    25: None, 26: "GetStringForActionOrigin", 27: None, 28: None,
    29: "GetMotionData", 30: "TriggerVibration",
    31: "TriggerVibrationExtended", 32: None, 33: "SetLEDColor",
    34: "Legacy_TriggerHapticPulse", 35: "Legacy_TriggerRepeatedHapticPulse",
    36: "ShowBindingPanel", 37: "GetInputTypeForHandle",
    38: "GetControllerForGamepadIndex", 39: "GetGamepadIndexForController",
    40: None, 41: None, 42: None, 43: "TranslateActionOrigin",
    44: "GetDeviceBindingRevision", 45: None,
    46: "GetSessionInputConfigurationSettings", 47: "SetDualSenseTriggerEffect",
}

# ---- which functions provably reach the SteamInput accessor ---------------
SI_FNS = set()
for k in range(-3, 4):
    sl = 0x00C48408 + k * 0x18
    for r in riprefs(sl):
        f = pd.func(r[0])
        if f:
            SI_FNS.add(f[0])

CALLP = re.compile(r"^qword ptr \[(\w+)\s*\+\s*(0x[0-9a-fA-F]+|\d+)\]$")
hits = []
for i in range(len(recs) - 2):
    r1, r2, r3 = recs[i], recs[i + 1], recs[i + 2]
    if (r1[2] == "mov" and r1[3] == "rcx, qword ptr [rax]"
            and r2[2] == "mov" and r2[3] == "rax, qword ptr [rcx]"
            and r3[2] == "call"):
        m = CALLP.match(r3[3])
        if m:
            hits.append((r3[0], m.group(1), int(m.group(2), 0)))

print("SteamInput-accessor-reachable .pdata functions: %d" % len(SI_FNS))
print("total double-deref interface call sites:        %d\n" % len(hits))

rows = []
for a, reg, off in hits:
    f = pd.func(a)
    fbeg = f[0] if f else -1
    slot = off // 8
    rows.append(dict(
        call=a, reg=reg, off=off, slot=slot,
        fn=fbeg, fnend=f[1] if f else 0,
        name=VT.get(slot) if off % 8 == 0 and slot in VT else None,
        steam=fbeg in SI_FNS,
        raw=s(a, 6)))

rows.sort(key=lambda r: (not r["steam"], r["fn"], r["call"]))
print("| call RVA | instruction | slot | method (proved) | in SteamInput fn | fn range |")
print("|---|---|---|---|---|---|")
for r in rows:
    if not r["steam"]:
        continue
    print("| 0x%08X | `call [%s + 0x%X]` | %d/0x%X | %s | YES | 0x%08X..0x%08X |"
          % (r["call"], r["reg"], r["off"], r["slot"], r["off"],
             r["name"] or "**UNMAPPED**", r["fn"], r["fnend"]))

print("\n\n=== NON-SteamInput interface vtable calls (other Steam interfaces) ===")
print("| call RVA | instruction | slot | fn range |")
print("|---|---|---|---|")
agg = collections.Counter()
for r in rows:
    if r["steam"]:
        continue
    agg[(r["off"] // 8, r["off"])] += 1
    print("| 0x%08X | `call [%s + 0x%X]` | %d | 0x%08X..0x%08X |"
          % (r["call"], r["reg"], r["off"], r["slot"], r["fn"], r["fnend"]))
