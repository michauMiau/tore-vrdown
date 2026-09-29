#!/usr/bin/env python3
"""S17 - trace the receiver of each GetDigitalActionData (0x88) candidate in the
Steam-Input code region, to find where the game caches ISteamInput*."""
import collections
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, iat, pdata, raw, s

IM = tdlib.iat()
pd = pdata()
byaddr = tdlib.idx()
CAND = [0x004DAA09, 0x004E08ED, 0x004E1AF0, 0x004E1C08, 0x0014011F,
        0x00160EFC, 0x00163EA8, 0x005994F9, 0x00599521, 0x0059ED2D,
        0x005AD831, 0x004B5AB1]


def note(p):
    n = ""
    if "rip" in p[3]:
        d = disp_of(p)
        if d is not None:
            t = p[0] + p[1] + d
            n = "  ; ->0x%08X" % t
            c = cstr(t, 32)
            if c.isprintable() and len(c) > 1:
                n += " %r" % c
            if p[2] == "call":
                n += " IAT=%s" % (IM.get(t, ("?","?"))[1],)
    return n


for a in CAND:
    f = pd.func(a)
    if not f:
        print("\n0x%08X  no pdata" % a)
        continue
    print("\n" + "=" * 74)
    print("function 0x%08X..0x%08X  (%d bytes)  call at 0x%08X"
          % (f[0], f[1], f[1] - f[0], a))
    print("=" * 74)
    lo = max(f[0], a - 0x50)
    hi = min(f[1], a + 0x40)
    for p in dis(lo, 400):
        if p[0] >= hi:
            break
        mark = "  <<<<" if p[0] == a else ""
        print("  %08X %-6s %-40s %-22s%s%s"
              % (p[0], p[2], p[3], s(p[0], p[1]), note(p), mark))
