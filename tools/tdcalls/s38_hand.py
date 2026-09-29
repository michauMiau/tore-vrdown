#!/usr/bin/env python3
"""S38 - hand-resolve the 10 the symbolic tracker could not model, and
identify the 69th SteamInput dispatch site (0x004E0C92, never seen before)."""
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, pdata, s as hexs

pd = pdata()
byaddr = tdlib.idx()

UNRES = [0x004D9795, 0x004D97A8, 0x004DBA0E, 0x004DE15C, 0x004DF190,
         0x004DF8A7, 0x004E01CB, 0x004E0280, 0x004E0321, 0x004E03C8]
for d in UNRES:
    f = pd.func(d)
    ins = [p for p in dis(f[0], 8000) if p[0] < f[1]]
    i = next(k for k, p in enumerate(ins) if p[0] == d)
    print("=== disp 0x%08X  fn %08X..%08X ===" % (d, f[0], f[1]))
    for p in ins[i:i + 16]:
        note = ""
        if "rip" in p[3]:
            dd = disp_of(p)
            if dd is not None:
                t = p[0] + p[1] + dd
                note = "  ; ->0x%08X %r" % (t, cstr(t, 18))
        print("   %08X %-6s %-40s %-20s%s"
              % (p[0], p[2], p[3], hexs(p[0], p[1]), note))
    print()

print("=== the 69th site 0x004E0C92 in fn 004E0C31..004E0CDD ===")
f = pd.func(0x004E0C92)
for p in dis(f[0], f[1] - f[0]):
    note = ""
    if "rip" in p[3]:
        dd = disp_of(p)
        if dd is not None:
            t = p[0] + p[1] + dd
            note = "  ; ->0x%08X %r" % (t, cstr(t, 18))
    print("   %08X %-6s %-40s %-20s%s"
          % (p[0], p[2], p[3], hexs(p[0], p[1]), note))
print("\n=== fn 004E047A..004E0530 (0x004E048A) ===")
f = pd.func(0x004E048A)
for p in dis(f[0], f[1] - f[0]):
    note = ""
    if "rip" in p[3]:
        dd = disp_of(p)
        if dd is not None:
            t = p[0] + p[1] + dd
            note = "  ; ->0x%08X %r" % (t, cstr(t, 18))
    print("   %08X %-6s %-40s %-20s%s"
          % (p[0], p[2], p[3], hexs(p[0], p[1]), note))
