#!/usr/bin/env python3
"""S20 - If the game never calls GetDigitalActionData, HOW does it read input?

Hypothesis: Teardown reads action state through the handle container at
[rsi+0x58] and the values come from a different source.  Find the reader:
callers of the accessor 0x004DA940's slot, the RunFrame site (0x18), and any
function that loads from the [obj+0x58] container then does a single-deref
vtable call (`mov rax,[rcx]; call [rax+K]`) - the cached-interface shape.
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
DISPATCH = 0x009831B0
SI_SEL = 0x00C48408

# 1. all dispatch sites that select Steam Input, with the vtable offset called
CALLP = re.compile(r"^qword ptr \[(\w+)\s*\+\s*(0x[0-9a-fA-F]+|\d+)\]$")
si_sites = []
for r in recs:
    if r[2] != "call" or "rip" not in r[3]:
        continue
    d = disp_of(r)
    if d is None or r[0] + r[1] + d != DISPATCH:
        continue
    a, sel = r[0], None
    for _ in range(14):
        a -= 1
        p = byaddr.get(a)
        if p and p[2] == "lea" and "rip" in p[3] and p[3].startswith("rcx,"):
            dd = disp_of(p)
            if dd is not None:
                sel = p[0] + p[1] + dd
            break
    si_sites.append((r[0], sel))

print("=== %d dispatch sites total; SteamInput ones follow ===" % len(si_sites))
si = [(a, sl) for a, sl in si_sites if sl == SI_SEL]
print("=== %d select 0x%08X (SteamInput006) ===\n" % (len(si), SI_SEL))
byfn = collections.Counter()
for a, sl in si:
    f = pd.func(a)
    byfn[f[0] if f else -1] += 1
for k, c in byfn.most_common():
    f = pd.func(k) if k >= 0 else None
    print("  %3d x  fn %s" % (c, ("%08X..%08X" % (f[0], f[1])) if f else "NO-PDATA"))

# 2. in each such function, EVERY indirect call with its offset
print("\n=== vtable offsets reached from SteamInput-selecting dispatch sites ===")
offs = collections.Counter()
detail = collections.defaultdict(list)
for a, sl in si:
    f = pd.func(a)
    if not f:
        continue
    r2 = byaddr.get(a + 6)
    chain = None
    cur = a + 6
    for _ in range(20):
        p = byaddr.get(cur)
        if p is None:
            break
        if p[2] == "mov" and p[3] == "rcx, qword ptr [rax]":
            q = byaddr.get(p[0] + p[1])
            if q and q[2] == "mov" and q[3] == "rax, qword ptr [rcx]":
                c = byaddr.get(q[0] + q[1])
                if c and c[2] == "call":
                    m = CALLP.match(c[3])
                    if m:
                        chain = (c[0], int(m.group(2), 0))
                        break
        if p[2] in ("mov", "lea", "pop") and p[3].split(",")[0].strip() in ("rax", "eax", "al", "ax"):
            break
        cur += p[1]
    if chain:
        offs[chain[1]] += 1
        detail[chain[1]].append(chain[0])
for off, c in sorted(offs.items()):
    print("  off 0x%03X slot %3d  %3d sites" % (off, off // 8, c))
    if c <= 10:
        print("        " + " ".join("%08X" % a for a in detail[off]))

# 3. The RunFrame site - where is it?
print("\n=== function containing the RunFrame (0x18) call ===")
f = pd.func(0x004DF907)
print("fn 0x%08X..0x%08X (%d bytes)" % (f[0], f[1], f[1] - f[0]))
for p in dis(f[0], 120):
    if p[0] >= f[1]:
        break
    if p[0] > 0x004DF8A0 and p[0] < 0x004DF930:
        note = ""
        if "rip" in p[3]:
            t = p[0] + p[1] + disp_of(p)
            note = "  ; ->0x%08X %r" % (t, cstr(t, 24))
        print("  %08X %-6s %-38s %-22s%s" % (p[0], p[2], p[3], s(p[0], p[1]), note))
