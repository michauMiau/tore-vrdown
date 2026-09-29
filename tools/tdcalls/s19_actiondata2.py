#!/usr/bin/env python3
"""S19 - THE key question: does the game call GetDigitalActionData (0x88) or
GetAnalogActionData (0xA8) on ISteamInput at all?

S15 (dataflow from the SteamInput selector) found NONE.  S16 found 20 sites at
0x88, but a hand-checked one (0x4DAA09) selects a DIFFERENT interface
(0x00BF2D70) and its result feeds a steamcommunity.com URL - that is a Steam
friends/social API, not input.

So: enumerate EVERY call of offset 0x88 / 0xA8 in .text and, for each, report
the selector slot that feeds the receiver.  A site is ISteamInput ONLY if the
selector is 0x00C48408.
"""
import collections
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, disp_of, iat, pdata, raw, s

IM = tdlib.iat()
pd = pd = pdata()
recs = tdlib.recs()
byaddr = tdlib.idx()
DISPATCH = 0x009831B0
SI_SEL = 0x00C48408

# name every selector slot by the interface accessor it holds
im = tdlib.img()
import struct
selname = {}
for sec in im.sections:
    if sec["name"] not in (".rdata", ".dataa"):
        continue
    lo, hi = sec["rawptr"], sec["rawptr"] + sec["rsize"]
    d = im.data
    i = lo
    while i + 8 <= hi:
        q = struct.unpack_from("<Q", d, i)[0]
        if 0x140000000 <= q < 0x142000000:
            r = q - 0x140000000
            selname[sec["vaddr"] + (i - sec["rawptr"])] = r
        i += 8

print("=== ALL vtable calls at 0x88 (GetDigitalActionData) and 0xA8 ===")
CALLP = re.compile(r"^qword ptr \[(\w+)(?:\s*\+\s*(0x[0-9a-fA-F]+|\d+))?\]$")
for want, label in ((0x88, "GetDigitalActionData"), (0xA8, "GetAnalogActionData")):
    print("\n############ offset 0x%02X  %s ############" % (want, label))
    n = 0
    for r in recs:
        if r[2] != "call":
            continue
        m = CALLP.match(r[3])
        if not m:
            continue
        off = int(m.group(2), 0) if m.group(2) else 0
        if off != want:
            continue
        n += 1
        reg = m.group(1)
        # look back for the dispatch that produced the receiver
        sel, selrva = None, None
        a = r[0]
        for _ in range(20):
            a -= 1
            p = byaddr.get(a)
            if p is None:
                continue
            if p[2] == "call" and "rip" in p[3]:
                d = disp_of(p)
                if d is not None and p[0] + p[1] + d == DISPATCH:
                    selrva = p[0]
                    q = selrva
                    for _ in range(14):
                        q -= 1
                        z = byaddr.get(q)
                        if z and z[2] == "lea" and "rip" in z[3] \
                                and z[3].startswith("rcx,"):
                            dd = disp_of(z)
                            if dd is not None:
                                sel = z[0] + z[1] + dd
                            break
                    break
        f = pd.func(r[0])
        tgt = selname.get(sel)
        istag = "  <<<<< ISteamInput" if sel == SI_SEL else ""
        print("  %08X  fn %-20s sel %s%s%s"
              % (r[0], ("%08X..%08X" % (f[0], f[1])) if f else "?",
                 ("0x%08X" % sel) if sel else "(none found)",
                 (" -> accessor 0x%08X" % tgt) if tgt else "", istag))
    print("  total %d sites" % n)
