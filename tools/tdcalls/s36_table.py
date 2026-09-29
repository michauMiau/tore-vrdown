#!/usr/bin/env python3
"""S36 - THE definitive table generator.

Walks the whole binary once, finds every SteamInput dispatch (selector lea ->
0x00C48408, then call qword ptr [rip -> 0x009831B0]), and resolves the vtable
call that consumes the returned pointer.  Emits markdown.
"""
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, pdata, s as hexs

pd = pdata()
byaddr = tdlib.idx()
DISPATCH = 0x009831B0
SEL = 0x00C48408
IMAGEBASE = 0x140000000

SLOT = {
    0: "Init", 1: "Shutdown", 3: "RunFrame", 6: "GetConnectedControllers",
    9: "GetActionSetHandle", 10: "ActivateActionSet", 11: "GetCurrentActionSet",
    15: "GetActiveActionSetLayers", 16: "GetDigitalActionHandle",
    17: "GetDigitalActionData", 18: "GetDigitalActionOrigins",
    20: "GetAnalogActionHandle", 21: "GetAnalogActionData",
    22: "GetAnalogActionOrigins", 24: "GetGlyphSVGForActionOrigin",
    26: "GetStringForActionOrigin", 29: "GetMotionData",
    30: "TriggerVibration", 31: "TriggerVibrationExtended",
    33: "SetLEDColor", 34: "Legacy_TriggerHapticPulse",
    35: "Legacy_TriggerRepeatedHapticPulse", 36: "ShowBindingPanel",
    37: "GetInputTypeForHandle", 38: "GetControllerForGamepadIndex",
    39: "GetGamepadIndexForController", 43: "TranslateActionOrigin",
    44: "GetDeviceBindingRevision", 46: "GetSessionInputConfigurationSettings",
    47: "SetDualSenseTriggerEffect",
}
AMBIG = {
    8: "GetActionSetHandle (header x-check) | EnableDeviceCallbacks | TriggerSimpleHapticEvent",
    19: "GetAnalogActionHandle (header x-check) | GetStringForAnalogActionName | GetStringForDigitalActionName",
    23: "GetStringForAnalogActionName (header x-check) | GetGlyphForActionOrigin_Legacy | GetGlyphPNGForActionOrigin",
    27: "TriggerVibrationExtended (header x-check) | GetStringForAnalogActionName | GetStringForDigitalActionName",
    28: "TriggerSimpleHapticEvent (header x-check) | StopAnalogActionMomentum | TriggerHapticPulse | TriggerRepeatedHapticPulse",
    32: "ShowBindingPanel (header x-check) | EnableActionEventCallbacks | EnableDeviceCallbacks | TriggerSimpleHapticEvent",
    40: "GetRemotePlaySessionID (header x-check) | GetGlyphForXboxOrigin | GetStringForXboxOrigin",
    42: "SetDualSenseTriggerEffect (header x-check) | GetActionOriginFromXboxOrigin | GetRemotePlaySessionID",
}


def riptgt(p):
    d = disp_of(p)
    return None if d is None else p[0] + p[1] + d


def name_of(slot):
    if slot in SLOT:
        return SLOT[slot]
    if slot in AMBIG:
        return "**AMBIGUOUS** " + AMBIG[slot]
    return "**UNMAPPED**"


# ---- collect all dispatch sites
sites = []
recs = tdlib.recs()
for r in recs:
    if r[2] != "call" or "rip" not in r[3]:
        continue
    d = disp_of(r)
    if d is None or r[0] + r[1] + d != DISPATCH:
        continue
    sites.append(r[0])
print("total calls through the dispatcher 0x%08X : %d" % (DISPATCH, len(sites)))

# ---- keep only the ones that select SteamInput
si = []
for a in sites:
    f = pd.func(a)
    ins = [p for p in dis(f[0], 6000) if p[0] < f[1]]
    idx = next((k for k, p in enumerate(ins) if p[0] == a), None)
    if idx is None:
        continue
    if any(p[2] == "lea" and p[3].startswith("rcx,") and "rip" in p[3]
           and riptgt(p) == SEL for p in ins[max(0, idx - 8):idx]):
        si.append((a, f))
print("of which select SteamInput: %d" % len(si))

# ---- resolve each
out = []
for a, f in si:
    ins = [p for p in dis(f[0], 6000) if p[0] < f[1]]
    i = next(k for k, p in enumerate(ins) if p[0] == a)
    win = ins[i:i + 40]
    slot = callat = shape = None
    base = None
    for p in win:
        m = re.match(r"^mov +(r1[0-5]), qword ptr \[(r1[0-5])\] \+ ", p[3])
        m2 = re.match(r"^mov +(r1[0-5]), qword ptr \[(r1[0-5]) \+ (0x[0-9a-f]+)\]$",
                      p[3])
        if m2 and slot is None and p[0] > a:
            slot = int(m2.group(3), 0)
            base = m2.group(2)
            shape = "direct"
        m3 = re.match(r"^call qword ptr \[(r1[0-5]) \+ (0x[0-9a-f]+)\]$", p[3])
        if m3 and slot is not None and int(m3.group(2), 0) == slot \
                and m3.group(1) == (base or "rax"):
            callat = p[0]
            break
    # the `mov r10,[rdx+0x90]; call r10` and `mov r12,[rax+0xA8]; call r12` forms
    if callat is None:
        loaded = {}
        for p in win:
            m = re.match(r"^mov +(r1[0-5]), qword ptr \[(r1[0-5]) \+ (0x[0-9a-f]+)\]$",
                         p[3])
            if m:
                loaded[m.group(1)] = (int(m.group(3), 0), m.group(2), p[0])
            m2 = re.match(r"^call (r1[0-5])$", p[3])
            if m2 and m2.group(1) in loaded:
                off, bs, lat = loaded[m2.group(1)]
                if off % 8 == 0 and off <= 0x178:
                    slot, base, callat, shape = off, bs, p[0], "via %s" % m2.group(1)
                    break
    out.append({"disp": a, "vcall": callat, "off": slot,
                "slot": (slot // 8) if slot is not None and slot % 8 == 0 else None,
                "shape": shape, "fn": f})

print("resolved %d / %d" % (sum(1 for o in out if o["off"] is not None), len(out)))
import pickle
pickle.dump(out, open("/root/.hermes/cache/scratch/sites.pkl", "wb"))
for o in sorted(out, key=lambda z: z["disp"]):
    if o["off"] is None:
        print("  UNRESOLVED disp 0x%08X fn %08X..%08X" % (o["disp"], o["fn"][0], o["fn"][1]))
print("\nslot histogram:")
h = {}
for o in out:
    if o["slot"] is not None:
        h.setdefault(o["slot"], []).append(o["disp"])
for k in sorted(h):
    print("  slot %-2d off 0x%03X  %-38s %d site(s)  first 0x%08X last 0x%08X"
          % (k, k * 8, name_of(k), len(h[k]), min(h[k]), max(h[k])))
