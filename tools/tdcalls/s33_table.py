#!/usr/bin/env python3
"""S33 - emit the definitive per-call-site table, joining:
     - the resolver output (rva, slot, method)
     - the function each site lives in
     - the argument setup
   into a markdown table.  Output to /root/.hermes/cache/scratch/table_final.md
"""
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import pdata

pd = pdata()

# ---- authoritative slot -> name map, from ISteamInput_vtable.md (binary-proved)
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
    2: "BNewDataAvailable | BWaitForData | SetInputActionManifestFilePath",
    4: "BNewDataAvailable | BWaitForData | SetInputActionManifestFilePath",
    5: "BNewDataAvailable | BWaitForData | SetInputActionManifestFilePath",
    7: "EnableActionEventCallbacks | EnableDeviceCallbacks | TriggerSimpleHapticEvent",
    8: "GetActionSetHandle(?) OR EnableDeviceCallbacks/TriggerSimpleHapticEvent",
    12: "ActivateActionSetLayer | DeactivateActionSetLayer | DeactivateAllActionSetLayers",
    13: "ActivateActionSetLayer | DeactivateActionSetLayer | DeactivateAllActionSetLayers",
    14: "ActivateActionSetLayer | DeactivateActionSetLayer | DeactivateAllActionSetLayers",
    19: "GetStringForAnalogActionName | GetStringForDigitalActionName (header cross-check: GetAnalogActionHandle)",
    23: "GetGlyphForActionOrigin_Legacy | GetGlyphPNGForActionOrigin (header cross-check: GetStringForAnalogActionName)",
    25: "GetGlyphForActionOrigin_Legacy | GetGlyphPNGForActionOrigin (header cross-check: GetMotionData)",
    27: "GetStringForAnalogActionName | GetStringForDigitalActionName (header cross-check: TriggerVibrationExtended)",
    28: "StopAnalogActionMomentum | TriggerHapticPulse | TriggerRepeatedHapticPulse (header cross-check: TriggerSimpleHapticEvent)",
    32: "EnableActionEventCallbacks | EnableDeviceCallbacks | TriggerSimpleHapticEvent (header cross-check: ShowBindingPanel)",
    40: "GetGlyphForXboxOrigin | GetStringForXboxOrigin (header cross-check: GetRemotePlaySessionID)",
    41: "GetGlyphForXboxOrigin | GetStringForXboxOrigin (header cross-check: GetSessionInputConfigurationSettings)",
    42: "GetActionOriginFromXboxOrigin | GetRemotePlaySessionID (header cross-check: SetDualSenseTriggerEffect)",
    45: "GetActionOriginFromXboxOrigin | GetRemotePlaySessionID",
}

# ---- parse the resolver output
rows = []
txt = open("/root/.hermes/cache/scratch/resolved.txt").read()
# lines look like: 0x004DF2F0  slot 16  0x80  GetDigitalActionHandle
for m in re.finditer(
        r"^(0x[0-9A-Fa-f]{8})\s+slot\s+(\d+)\s+0x([0-9a-f]+)\s+(\S+)?\s*(.*)$",
        txt, re.M):
    rva = int(m.group(1), 16)
    slot = int(m.group(2))
    rows.append((rva, slot, int(m.group(3), 16), m.group(4) or "", m.group(5).strip()))

print("parsed %d rows from resolved.txt" % len(rows))
byfn = {}
for rva, slot, off, nm, rest in rows:
    f = pd.func(rva)
    byfn.setdefault((f[0], f[1]), []).append((rva, slot, off, nm, rest))

print("across %d .pdata functions" % len(byfn))
for (a, b) in sorted(byfn):
    sites = byfn[(a, b)]
    names = {}
    for _, slot, _, nm, _ in sites:
        names[slot] = SLOT.get(slot) or ("AMBIG:" + AMBIG.get(slot, "?"))
    print("\nfn 0x%08X..0x%08X  (%d bytes)  %d site(s)"
          % (a, b, b - a, len(sites)))
    for rva, slot, off, nm, rest in sites:
        print("   0x%08X  slot %-2d off 0x%03X  %s"
              % (rva, slot, off, names[slot]))
