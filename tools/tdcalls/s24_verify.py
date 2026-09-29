#!/usr/bin/env python3
"""S24 - hand-verify the 8 unresolved sites + the 0x4A5263 misparse.

Prints a wide, fully annotated window for each so the sequence can be read
directly from the bytes rather than trusted from a regex.
"""
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, iat, pdata, s as hexs

IM = tdlib.iat()
pd = pdata()
byaddr = tdlib.idx()

SITES = [0x004DDF1A, 0x004DB8E9, 0x004DF190, 0x004E01CB, 0x004E0280,
         0x004E0321, 0x004E03C8, 0x004E048A, 0x004E0B9E, 0x004E08D6]


def note(p):
    n = []
    if "rip" in p[3]:
        d = disp_of(p)
        if d is not None:
            t = p[0] + p[1] + d
            n.append("->0x%08X" % t)
            c = cstr(t, 30)
            if c.isprintable() and len(c) > 1:
                n.append("%r" % c)
            if p[2] == "call":
                n.append("IAT=%s" % (IM.get(t, ("?", "?"))[1],))
    return "  ; " + " ".join(n) if n else ""


for a in SITES:
    f = pd.func(a)
    print("\n" + "=" * 78)
    print("dispatch at 0x%08X   fn 0x%08X..0x%08X (%d bytes)"
          % (a, f[0], f[1], f[1] - f[0]))
    print("=" * 78)
    lo = f[0]
    while lo < a - 0x10 and byaddr.get(lo) is None:
        lo -= 1
    lo = max(f[0], lo)
    for p in dis(lo, 200):
        if p[0] >= f[1] or p[0] > a + 0x50:
            break
        print("  %08X %-6s %-42s %-24s%s%s"
              % (p[0], p[2], p[3], hexs(p[0], p[1]), note(p),
                 "   <<<<" if p[0] == a else ""))
