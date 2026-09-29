#!/usr/bin/env python3
"""S1 - find every rip-relative reference to a target RVA, using the cached
capstone index (correct instruction boundaries) rather than a byte sweep.

target_rva = insn_rva + insn_length + disp32
"""
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, pdata, riprefs, s

recs = tdlib.recs()
pd = pdata()
print("index %d recs, .pdata %d entries" % (len(recs), pd.n))

for t in (0x00A4B6B0, 0x00A4B678):
    print("\n=== rip-relative refs to 0x%08X %r  bytes=%s"
          % (t, cstr(t), s(t, 24)))
    hits = riprefs(t)
    print("    %d hits" % len(hits))
    for r in hits:
        f = pd.func(r[0])
        print("    %08X  %-6s %-34s fn=%-20s raw=%s"
              % (r[0], r[2], r[3],
                 ("%08X..%08X" % (f[0], f[1])) if f else "NO-PDATA",
                 s(r[0], r[1])))
