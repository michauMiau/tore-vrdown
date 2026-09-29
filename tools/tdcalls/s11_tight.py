#!/usr/bin/env python3
"""S11 - TIGHT dataflow proof, and the IAT-name-vs-usage conflict.

(A) Tight attribution.  My S10 classed a vtable call as ISteamInput merely
    because its FUNCTION also references the accessor slot.  That is too loose:
    a function can hold several interface pointers.  A call is ISteamInput only
    if the interface pointer feeding it comes from the accessor call:

        lea   rcx, [rip -> 0x00C48408]   ; &accessor
        call  qword ptr [rip -> SLOT]   ; returns ISteamInput*
        mov   rcx, qword ptr [rax]      ; ptr -> &iface
        mov   rax, qword ptr [rcx]      ; &iface -> vtable
        call  qword ptr [rax + K]

    We require the three instructions to be CONTIGUOUS (no intervening
    instruction that redefines rax).  Everything else is not attributed.

(B) The 0x009831B0 conflict.  The import directory names that slot
    steam_api64!SteamInternal_ContextInit, whose real signature returns a small
    int.  But here the return value is dereferenced as a pointer-to-pointer.
    Enumerate EVERY call of that slot in the whole binary and show what is
    passed and what is done with the result.
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

ACC_SLOT = 0x00C48408
DISPATCH = 0x009831B0
TARGET_SEL = {0x00C48408}
CALLP = re.compile(r"^qword ptr \[(\w+)\s*\+\s*(0x[0-9a-fA-F]+|\d+)\]$")

print("=" * 70)
print("(B) EVERY call of IAT slot 0x009831B0 in the whole binary")
print("=" * 70)
n = 0
for r in recs:
    if r[2] != "call" or "rip" not in r[3]:
        continue
    t = (r[0] + r[1] + disp_of(r)) if disp_of(r) is not None else None
    if t != DISPATCH:
        continue
    n += 1
    f = pd.func(r[0])
    prev = []
    a = r[0]
    for _ in range(6):
        a -= 1
        pr = byaddr.get(a)
        if pr:
            prev.append(pr)
            if len(prev) == 3:
                break
    prev.reverse()
    print("\n call at 0x%08X  fn %s" % (r[0], "%08X..%08X" % (f[0], f[1]) if f else "?"))
    for p in prev:
        note = ""
        if "rip" in p[3]:
            tt = p[0] + p[1] + disp_of(p)
            note = "  ; ->0x%08X" % tt
            c = cstr(tt, 24)
            if c and c.isprintable() and len(c) > 2:
                note += " %r" % c
        print("   %08X %-6s %-38s%s" % (p[0], p[2], p[3], note))
    print("   %08X %-6s %-38s  <== THE CALL" % (r[0], r[2], r[3]))
    nxt = byaddr.get(r[0] + r[1])
    if nxt:
        print("   %08X %-6s %-38s  <== first use of return"
              % (nxt[0], nxt[2], nxt[3]))
print("\n total call sites of 0x009831B0: %d" % n)
print(" import-directory name for that slot: %s" % (IM.get(0x009831B0),))

print("\n" + "=" * 70)
print("(A) TIGHT attribution: selector-slot lea, dispatch call, 2 derefs, vcall")
print("=" * 70)
tight = []
for r in recs:
    if r[2] != "call" or "rip" not in r[3]:
        continue
    t = (r[0] + r[1] + disp_of(r)) if disp_of(r) is not None else None
    if t != DISPATCH:
        continue
    lea = None
    a = r[0]
    for _ in range(14):
        a -= 1
        p = byaddr.get(a)
        if p and p[2] == "lea" and "rip" in p[3] and p[3].startswith("rcx,"):
            lea = p
            break
    if lea is None:
        continue
    sel = lea[0] + lea[1] + disp_of(lea)
    if sel not in TARGET_SEL:
        continue
    d1 = byaddr.get(r[0] + r[1])
    d2 = byaddr.get(d1[0] + d1[1]) if d1 else None
    c3 = byaddr.get(d2[0] + d2[1]) if d2 else None
    if not (d1 and d2 and c3):
        continue
    if not (d1[2] == "mov" and d1[3] == "rcx, qword ptr [rax]"
            and d2[2] == "mov" and d2[3] == "rax, qword ptr [rcx]"
            and c3[2] == "call"):
        continue
    m = CALLP.match(c3[3])
    if m:
        tight.append((c3[0], m.group(1), int(m.group(2), 0), sel, r[0]))

print("\n %d TIGHTLY-attributed ISteamInput vtable calls "
      "(selector 0x%08X proven, contiguous 2 derefs)"
      % (len(tight), ACC_SLOT))
agg = collections.Counter()
for a, reg, off, sel, ac in tight:
    agg[off] += 1
for off, c in sorted(agg.items()):
    print("   offset 0x%03X  slot %3d  count %3d  %s"
          % (off, off // 8, c,
             "*** BEYOND the 48-slot vtable (0x178) ***" if off > 0x178 else ""))

print("\n distinct call RVAs by offset:")
for off, c in sorted(agg.items()):
    addrs = [x[0] for x in tight if x[2] == off]
    if c <= 8:
        print("   0x%03X: %s" % (off, " ".join("%08X" % a for a in addrs)))
    else:
        print("   0x%03X: %d sites, %08X .. %08X"
              % (off, c, min(addrs), max(addrs)))
