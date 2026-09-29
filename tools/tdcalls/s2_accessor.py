#!/usr/bin/env python3
"""S2 - the interface acquisition. Disassemble the .pdata function that
references "SteamInput006", decode the IAT slots it calls, and follow where
the returned ISteamInput* is stored."""
import struct
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import dis, disp_of, iat, pdata, s, cstr, raw

IM = tdlib.iat()
BASE = tdlib.img().image_base
print("image base 0x%X" % BASE)
print("\n--- steam_api64.dll IAT slots ---")
for slot, (dll, nm) in sorted(IM.items()):
    if "steam" in dll.lower():
        print("  slot 0x%08X  %s!%s" % (slot, dll, nm))

FN = 0x004DA940
f = pdata().func(FN)
print("\n=== accessor fn 0x%08X..0x%08X (%d bytes) unwind 0x%08X"
      % (f[0], f[1], f[1] - f[0], f[2]))
for r in dis(f[0], 40):
    note = ""
    if r[2] == "call" and "rip" in r[3]:
        t = r[0] + r[1] + disp_of(r)
        note = "  ; IAT 0x%08X = %s" % (t, IM.get(t, "NOT-AN-IAT-SLOT"))
    if "rip" in r[3]:
        t = r[0] + r[1] + disp_of(r)
        note += "  ; ->0x%08X %r" % (t, cstr(t, 24))
    print("  %08X  %-6s %-40s %s%s" % (r[0], r[2], r[3], s(r[0], r[1]), note))
