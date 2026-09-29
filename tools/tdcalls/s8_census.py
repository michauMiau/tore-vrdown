#!/usr/bin/env python3
"""S8 - FULL-TEXT census of the ISteamInput call idiom.

Fingerprint of a game-owned interface vtable call (not a raw byte pattern -
these are the index's real instruction boundaries):

      mov  rcx, qword ptr [rax]      ; obj -> &iface      (1st deref)
      mov  rax, qword ptr [rcx]      ; &iface -> vtable   (2nd deref)
      call qword ptr [rax + K]       ; virtual call

The two derefs are what distinguish a Steam-issued interface vtable from a
game class vtable (which needs only one).  Report every occurrence, the slot
offset, and the enclosing .pdata function.  We do NOT assume these are all
Steam Input; the caller reports them all and the surrounding code decides.
"""
import collections
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import pdata, s

pd = pdata()
recs = tdlib.recs()
byaddr = tdlib.idx()

DEREF1 = "qword ptr [rax]"
DEREF2 = "qword ptr [rcx]"
CALLP = re.compile(r"^qword ptr \[(\w+)\s*\+\s*(0x[0-9a-fA-F]+|\d+)\]$")

hits = []
n = 0
for i in range(len(recs) - 2):
    r1, r2, r3 = recs[i], recs[i + 1], recs[i + 2]
    if (r1[2] == "mov" and r1[3] == "rcx, " + DEREF1
            and r2[2] == "mov" and r2[3] == "rax, " + DEREF2
            and r3[2] == "call"):
        m = CALLP.match(r3[3])
        if m:
            hits.append((r3[0], m.group(1), int(m.group(2), 0)))
    n += 1

print("=== %d 'mov rcx,[rax]; mov rax,[rcx]; call [reg+off]' sites in .text ==="
      % len(hits))
byfn = collections.OrderedDict()
byslot = collections.Counter()
for a, reg, off in hits:
    f = pd.func(a)
    key = f[0] if f else -1
    byfn.setdefault(key, []).append((a, reg, off))
    byslot[off] += 1

print("\n--- histogram of slot byte-offsets ---")
for off, c in sorted(byslot.items()):
    print("  off 0x%03X  slot %3d  count %3d" % (off, off // 8, c))

print("\n--- by enclosing .pdata function ---")
for k, v in byfn.items():
    f = pd.func(k) if k >= 0 else None
    offs = sorted(set(x[2] for x in v))
    print("  fn %s  %3d sites  offsets %s"
          % (("%08X..%08X" % (f[0], f[1])) if f else "NO-PDATA",
             len(v), " ".join("0x%02X" % o for o in offs)))
    for a, reg, off in v:
        print("       %08X  call [%-4s + 0x%03X] slot %3d  raw=%s"
              % (a, reg, off, off // 8, s(a, 6)))
