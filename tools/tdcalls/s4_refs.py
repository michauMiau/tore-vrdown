#!/usr/bin/env python3
"""S4 - every rip-relative reference to the accessor-pointer slot 0x00C48408,
grouped by .pdata function.  For each referencing function, list the
indirect `call qword ptr [reg+disp8/disp32]` instructions (the vtable calls)
with their slot offsets, so the ISteamInput vtable census can be built."""
import collections
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, iat, pdata, riprefs, s

IM = tdlib.iat()
pd = pdata()
SLOT = 0x00C48408

hits = riprefs(SLOT)
print("=== %d rip-relative refs to 0x%08X (accessor fn ptr slot) ==="
      % (len(hits), SLOT))
fns = collections.OrderedDict()
for r in hits:
    f = pd.func(r[0])
    key = f[0] if f else -1
    fns.setdefault(key, []).append(r)
print("in %d distinct .pdata functions" % len(fns))
for k in fns:
    f = pd.func(k) if k >= 0 else None
    if f:
        print("  fn 0x%08X..0x%08X  %5d bytes  %2d refs to slot"
              % (f[0], f[1], f[1] - f[0], len(fns[k])))
    else:
        print("  *** ref at 0x%08X is in NO .pdata function (%d refs) ***"
              % (k, len(fns[k])))

CALLPAT = re.compile(r"qword ptr \[(\w+)(?:\s*\+\s*(0x[0-9a-f]+|\d+))?\]")


def vtable_calls(fbeg, fend):
    """indirect calls through [reg+off] inside a .pdata function."""
    out = []
    a = fbeg
    while a < fend:
        r = tdlib.idx().get(a)
        if r is None:
            a += 1
            continue
        if r[2] == "call" and "qword ptr [" in r[3]:
            m = CALLPAT.search(r[3])
            if m:
                reg = m.group(1)
                off = m.group(2)
                off = int(off, 0) if off else 0
                out.append((a, r, reg, off))
        a += r[1]
    return out


print("\n=== vtable calls per function (only functions that reach the accessor) ===")
for k, rs in fns.items():
    f = pd.func(k)
    if not f:
        continue
    fbeg, fend = f[0], f[1]
    vc = vtable_calls(f[0], f[1])
    # only show slots that are plausible ISteamInput vtable offsets
    hot = [x for x in vc if 0 < x[3] <= 0x178]
    print("\nfn 0x%08X..0x%08X : %d indirect calls, %d with off in 0x8..0x178"
          % (f[0], f[1], len(vc), len(hot)))
    for a, r, reg, off in hot[:12]:
        print("    %08X  %-6s %-34s reg=%s off=0x%02X slot=%d  raw=%s"
              % (a, r[2], r[3], reg, off, off // 8, s(a, r[1])))
    if len(hot) > 12:
        print("    ... %d more" % (len(hot) - 12))
