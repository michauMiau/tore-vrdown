#!/usr/bin/env python3
"""S3 - locate the accessor function pointer, then find EVERY vtable call
made on the ISteamInput* it returns.

Method: the accessor stores the returned interface pointer at the address it is
given.  The prior report claims the accessor's address 0x4DA940 is stored at
RVA 0x00C48408.  Verify that by bytes, then find all references to that data
slot, then within each referencing function find `call qword ptr [reg+disp]`
and classify.

CRITICAL: we cannot know a priori WHICH [reg+disp] calls are Steam Input
vtable calls, so we do NOT guess.  We instead report the full census of
indirect calls in the functions that touch the accessor, plus the strong
signal: the SteamInput006 accessor is only ever reached through that one
data slot, and the interface pointer flows from it.
"""
import struct
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, pdata, recs, riprefs, s, raw

BASE = tdlib.img().image_base
ACC = 0x004DA940
pd = pdata()
im = tdlib.img()

# ---- 1. is the accessor's VA stored anywhere in .rdata/.dataa? -------------
accva = BASE + ACC
pat = struct.pack("<Q", accva)
print("=== searching .rdata/.dataa for qword 0x%016X (= accessor VA) ==="
      % accva)
slots = []
for sec in im.sections:
    if sec["name"] not in (".rdata", ".dataa"):
        continue
    lo, hi = sec["rawptr"], sec["rawptr"] + sec["rsize"]
    d = im.data
    i = lo
    while True:
        i = d.find(pat, i, hi)
        if i < 0:
            break
        slots.append(sec["vaddr"] + (i - sec["rawptr"]))
        i += 1
print("  found %d slot(s): %s" % (len(slots), ["0x%08X" % x for x in slots]))
for sl in slots:
    # dump a window of neighbours so the object layout is visible
    print("\n  --- data window around 0x%08X ---" % sl)
    for k in range(-3, 6):
        a = sl + k * 8
        try:
            q = struct.unpack("<Q", raw(a, 8))[0]
        except Exception:
            continue
        tag = ""
        if BASE <= q < BASE + 0x02000000:
            r = q - BASE
            tag = " -> rva 0x%08X" % r
            if pd.is_start(r):
                tag += " (pdata fn start)"
        print("    [%+3d] 0x%08X : %016X%s" % (k, a, q, tag))
