#!/usr/bin/env python3
"""S5 - dump the real bytes/disasm around the claimed hot action-read loop,
and prove (not assume) the data flow from the accessor to each vtable call."""
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, iat, pdata, s

IM = tdlib.iat()
pd = pdata()
BASE = tdlib.img().image_base
SLOT = 0x00C48408


def note(r):
    n = []
    if "rip" in r[3]:
        t = r[0] + r[1] + disp_of(r)
        if r[2] == "call":
            n.append("IAT 0x%08X=%s" % (t, IM.get(t, "??")))
        else:
            n.append("->0x%08X" % t)
    return ("  ; " + " ".join(n)) if n else ""


def dump(beg, end, tag=""):
    print("\n--- %s 0x%08X..0x%08X ---" % (tag, beg, end))
    for r in dis(beg, 200):
        if r[0] >= end:
            break
        print("  %08X  %-6s %-40s %-22s%s"
              % (r[0], r[2], r[3], s(r[0], r[1]), note(r)))


# the prior report's claim: 0x4DD30A onwards
dump(0x004DD2F0, 0x004DD3D0, "function head (claimed hot loop)")
dump(0x004DDD40, 0x004DDD70, "function tail")

print("\n=== string check for the claimed action names ===")
for a in (0x009DEEA0, 0x009A9530, 0x009DEEAC, 0x009DEEB4):
    print("  0x%08X = %r" % (a, cstr(a, 20)))
