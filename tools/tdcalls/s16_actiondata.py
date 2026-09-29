#!/usr/bin/env python3
"""S16 - WHERE DOES THE GAME READ ACTION DATA?

The 51 dataflow-proven ISteamInput calls are all *handle* creation
(GetDigitalActionHandle x36, GetAnalogActionHandle x4, GetActionSetHandle x3,
RunFrame, GetAnalogActionOrigins, GetInputTypeForHandle, GetCurrentActionSet,
Shutdown).  None is GetDigitalActionData (slot 17 / 0x88) or
GetAnalogActionData (slot 21 / 0xA8) -- the actual VALUE reads.

Those calls exist (a loose scan found 4 at 0x88), so the game must cache the
ISteamInput* in a member field and call the vtable on the cached pointer,
bypassing the dispatch.  Find those receivers.
"""
import collections
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, iat, pdata, raw, riprefs, s

IM = tdlib.iat()
pd = pdata()
recs = tdlib.recs()
byaddr = tdlib.idx()
IMGB = tdlib.img().image_base

INTEREST = {0x88: "GetDigitalActionData", 0xA8: "GetAnalogActionData",
            0x90: "GetDigitalActionOrigins", 0x18: "RunFrame",
            0x48: "GetActionSetHandle", 0x80: "GetDigitalActionHandle",
            0xA0: "GetAnalogActionHandle", 0x30: "GetConnectedControllers"}

print("=== every vtable call at the interesting offsets, with the RECEIVER trace ===")
CALLP = re.compile(r"^qword ptr \[(\w+)(?:\s*\+\s*(0x[0-9a-fA-F]+|\d+))?\]$")
byoff = collections.defaultdict(list)
for r in recs:
    if r[2] != "call":
        continue
    m = CALLP.match(r[3])
    if not m:
        continue
    off = int(m.group(2), 0) if m.group(2) else 0
    if off in INTEREST:
        byoff[off].append((r[0], m.group(1)))

for off in sorted(byoff):
    print("\n--- offset 0x%02X  %s : %d sites ---"
          % (off, INTEREST[off], len(byoff[off])))
    for a, reg in byoff[off][:80]:
        f = pd.func(a)
        # walk back to find how `reg` was loaded
        cur, trace = a, []
        for _ in range(12):
            cur -= 1
            p = byaddr.get(cur)
            if p is None:
                continue
            trace.append(p)
            if p[2] == "mov" and p[3].split(",")[0].strip() == reg:
                break
        trace.reverse()
        print("   %08X  fn %-20s raw=%s"
              % (a, ("%08X..%08X" % (f[0], f[1])) if f else "?", s(a, 6)))
        for p in trace:
            note = ""
            if "rip" in p[3]:
                t = p[0] + p[1] + disp_of(p)
                note = "  ; ->0x%08X" % t
                c = cstr(t, 28)
                if c.isprintable() and len(c) > 1:
                    note += " %r" % c
            print("       %08X %-6s %-38s%s" % (p[0], p[2], p[3], note))
