#!/usr/bin/env python3
"""S37 - THE definitive resolver.

Symbolic forward tracking from each SteamInput dispatch.  Values tracked:
    P    the ISteamInput* the dispatcher returned in rax
    D1   *P            (the game holds a pointer-to-interface)
    VT   **P           the vtable
    K    the value of [VT+K] == the address of vtable slot K (so `call r` = slot K)

Everything else invalidates.
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
MAXSLOT = 0x178

MOV_R_MEM = re.compile(r"^(r(?:ax|bx|cx|dx|si|di|bp|sp|[89]|1[0-5])), "
                       r"qword ptr \[(r(?:ax|bx|cx|dx|si|di|bp|sp|[89]|1[0-5]))"
                       r"(?: \+ (0x[0-9a-f]+))?\]$")
CALL_MEM = re.compile(r"^qword ptr \[(r(?:ax|bx|cx|dx|si|di|bp|sp|[89]|1[0-5]))"
                      r" \+ (0x[0-9a-f]+)\]$")
CALL_REG = re.compile(r"^(r(?:ax|bx|cx|dx|si|di|bp|sp|[89]|1[0-5]))$")
LEA_RIP = re.compile(r"^(r(?:ax|bx|cx|dx|si|di|bp|sp|[89]|1[0-5])), \[rip \+ ")
ANY_REG = r"(?:r(?:ax|bx|cx|dx|si|di|bp|sp|[89]|1[0-5]))"
# any instruction that clobbers these
CLOBBER = re.compile(r"^(call|jmp|ret|div|mul|idiv|imul)[ ]" + ANY_REG + r"\b|"
                     r"^pop +" + ANY_REG + r"\b")

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
    8: "header x-check GetActionSetHandle | EnableDeviceCallbacks | TriggerSimpleHapticEvent",
    19: "header x-check GetAnalogActionHandle | GetStringForAnalogActionName | GetStringForDigitalActionName",
    23: "header x-check GetStringForAnalogActionName | GetGlyphForActionOrigin_Legacy | GetGlyphPNGForActionOrigin",
    27: "header x-check TriggerVibrationExtended | GetStringForAnalogActionName | GetStringForDigitalActionName",
    28: "header x-check TriggerSimpleHapticEvent | StopAnalogActionMomentum | TriggerHapticPulse | TriggerRepeatedHapticPulse",
    32: "header x-check ShowBindingPanel | EnableActionEventCallbacks | EnableDeviceCallbacks | TriggerSimpleHapticEvent",
    40: "header x-check GetRemotePlaySessionID | GetGlyphForXboxOrigin | GetStringForXboxOrigin",
    42: "header x-check SetDualSenseTriggerEffect | GetActionOriginFromXboxOrigin | GetRemotePlaySessionID",
    2: "header x-check SetInputActionManifestFilePath | BNewDataAvailable | BWaitForData",
    4: "header x-check BWaitForData | BNewDataAvailable | SetInputActionManifestFilePath",
    5: "header x-check BNewDataAvailable | BWaitForData | SetInputActionManifestFilePath",
    7: "header x-check EnableDeviceCallbacks | EnableActionEventCallbacks | TriggerSimpleHapticEvent",
    12: "header x-check DeactivateActionSetLayer | ActivateActionSetLayer | DeactivateAllActionSetLayers",
    13: "header x-check DeactivateAllActionSetLayers | ActivateActionSetLayer | DeactivateActionSetLayer",
    14: "header x-check GetActiveActionSetLayers-adjacent | ActivateActionSetLayer | DeactivateActionSetLayer | DeactivateAllActionSetLayers",
    25: "header x-check GetMotionData | GetGlyphForActionOrigin_Legacy | GetGlyphPNGForActionOrigin",
    41: "header x-check GetSessionInputConfigurationSettings | GetGlyphForXboxOrigin | GetStringForXboxOrigin",
    45: "UNMAPPED | GetActionOriginFromXboxOrigin | GetRemotePlaySessionID",
}


def name_of(slot):
    if slot in SLOT:
        return SLOT[slot], "PROVED"
    if slot in AMBIG:
        return AMBIG[slot], "AMBIGUOUS"
    return "UNMAPPED", "UNMAPPED"


def riptgt(p):
    d = disp_of(p)
    return None if d is None else p[0] + p[1] + d


# ---------- find all SteamInput dispatch sites
allst, sifn = [], set()
for r in tdlib.recs():
    if r[2] != "call" or "rip" not in r[3]:
        continue
    d = disp_of(r)
    if d is not None and r[0] + r[1] + d == DISPATCH:
        allst.append(r[0])
si = []
for a in allst:
    f = pd.func(a)
    sifn.add((f[0], f[1]))
    ins = [p for p in dis(f[0], 8000) if p[0] < f[1]]
    i = next((k for k, p in enumerate(ins) if p[0] == a), None)
    if i is not None and any(p[2] == "lea" and p[3].startswith("rcx,")
                              and "rip" in p[3] and riptgt(p) == SEL
                              for p in ins[max(0, i - 8):i]):
        si.append((a, f, ins, i))
print("dispatcher call sites total : %d" % len(allst))
print("selecting SteamInput         : %d" % len(si))

# ---------- resolve
res = []
for a, f, ins, i in si:
    st = {"rax": "P"}          # the dispatcher returned the interface in rax
    off = callat = shape = None
    for p in ins[i + 1:i + 44]:
        mn, op = p[2], p[3]
        if mn in ("ret", "jmp"):
            break
        m = CALL_MEM.match(op)
        if m and mn == "call":
            k = int(m.group(2), 16)
            if st.get(m.group(1)) == "VT" and k % 8 == 0 and k <= MAXSLOT:
                off, callat, shape = k, p[0], "call qword ptr [%s + 0x%X]" % (
                    m.group(1), k)
                break
        m = CALL_REG.match(op)
        if m and mn == "call":
            v = st.get(m.group(1))
            if isinstance(v, int) and v % 8 == 0 and v <= MAXSLOT:
                off, callat, shape = v, p[0], "call %s" % m.group(1)
                break
        m = MOV_R_MEM.match(op)
        if m and mn == "mov":
            dst, src, imm = m.group(1), m.group(2), m.group(3)
            v = st.get(src)
            if imm is None:
                if v == "P":
                    st[dst] = "D1"
                elif v == "D1":
                    st[dst] = "VT"
                else:
                    st[dst] = None
            else:
                k = int(imm, 16)
                if v == "VT" and k % 8 == 0 and k <= MAXSLOT:
                    st[dst] = k
                else:
                    st[dst] = None
            continue
        # anything else that writes a register we track -> invalidate
        mm = re.match(r"^" + ANY_REG + r", ", op)
        if mm and mn not in ("cmp", "test"):
            st[mm.group(0)[:-2]] = None
    res.append({"disp": a, "vcall": callat, "off": off, "shape": shape, "fn": f})

ok = [r for r in res if r["off"] is not None]
bad = [r for r in res if r["off"] is None]
print("resolved %d / %d   (%d UNRESOLVED)" % (len(ok), len(res), len(bad)))
for r in bad:
    print("   !! 0x%08X fn %08X..%08X" % (r["disp"], r["fn"][0], r["fn"][1]))

h = {}
for r in ok:
    h.setdefault(r["off"], []).append(r)
print("\n=== slot histogram ===")
for k in sorted(h):
    s = k // 8
    nm, prov = name_of(s)
    v = sorted(x["vcall"] for x in h[k])
    print("  slot %-2d off 0x%03X  %-34s %-10s %3d site(s)  vcall 0x%08X..0x%08X"
          % (s, k, nm, prov, len(h[k]), v[0], v[-1]))

import pickle
pickle.dump(res, open("/root/.hermes/cache/scratch/sites.pkl", "wb"))
print("\nwrote sites.pkl")
