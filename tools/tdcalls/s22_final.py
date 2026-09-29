#!/usr/bin/env python3
"""S22 - definitive per-site offset table for all 68 SteamInput sites.

The S21 forward walk bailed out too early.  Here: for each dispatch site, print
a 24-instruction window so the sequence can be read, and separately apply the
correct rule - find `mov rcx,[rax]` then `mov rax,[rcx]` then `call [reg+off]`
ALLOWING arbitrary instructions between (as long as none writes rax in a way
that breaks the chain).  Report both so discrepancies are visible.
"""
import collections
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, iat, pdata, raw, s as hexs

IM = tdlib.iat()
pd = pdata()
recs = tdlib.recs()
byaddr = tdlib.idx()
DISPATCH = 0x009831B0
SI_SEL = 0x00C48408
CALLP = re.compile(r"^qword ptr \[(\w+)\s*\+\s*(0x[0-9a-fA-F]+|\d+)\]$")

sites = []
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
    if sel == SI_SEL:
        sites.append(r[0])

print("SteamInput dispatch sites: %d" % len(sites))

WRITE_RAX = {"rax", "eax", "ax", "al", "ah"}
results = {}
for a in sites:
    # collect the next 30 instructions
    win, cur = [], a + 6
    for _ in range(30):
        p = byaddr.get(cur)
        if p is None:
            break
        win.append(p)
        cur += p[1]
    # find the deref pattern
    vcall = None
    for i, p in enumerate(win):
        if p[2] == "mov" and p[3] == "rcx, qword ptr [rax]":
            # ensure nothing between a+6 and here writes rax
            bad = False
            for q in win[:i]:
                if q[2] in ("mov", "movzx", "movsxd", "lea", "pop") and \
                        q[3].split(",")[0].strip() in WRITE_RAX:
                    bad = True
                    break
                if q[2] in ("xor",) and q[3].split(",")[0].strip() in WRITE_RAX:
                    bad = True
                    break
            if bad:
                continue
            if i + 2 < len(win) and win[i + 1][2] == "mov" \
                    and win[i + 1][3] == "rax, qword ptr [rcx]" \
                    and win[i + 2][2] == "call":
                m = CALLP.match(win[i + 2][3])
                if m:
                    vcall = (win[i + 2][0], int(m.group(2), 0))
                    break
    results[a] = vcall

agg = collections.Counter()
for a, v in results.items():
    if v:
        agg[v[1]] += 1
print("\n=== histogram of vtable offsets called on ISteamInput ===")
for off, c in sorted(agg.items()):
    print("  0x%03X  slot %3d  count %3d" % (off, off // 8, c))
print("  unattributed sites: %d" % sum(1 for v in results.values() if not v))

print("\n=== per-site ===")
for a in sorted(results):
    v = results[a]
    f = pd.func(a)
    if v:
        print("  0x%08X -> call 0x%08X  off 0x%03X slot %3d   fn %s"
              % (a, v[0], v[1], v[1] // 8, "%08X..%08X" % (f[0], f[1])))
    else:
        print("  0x%08X -> NO vtable call in the following 30 insns   fn %s"
              % (a, "%08X..%08X" % (f[0], f[1])))

print("\n\n########## WINDOWS for the unattributed sites ##########")
for a in sorted(results):
    if results[a]:
        continue
    f = pd.func(a)
    print("\n--- site 0x%08X in fn %08X..%08X ---" % (a, f[0], f[1]))
    for p in dis(a + 6, 26):
        if p[0] >= f[1]:
            break
        note = ""
        if "rip" in p[3]:
            dd = disp_of(p)
            if dd is not None:
                t = p[0] + p[1] + dd
                note = "  ; ->0x%08X" % t
                c = cstr(t, 22)
                if c.isprintable() and len(c) > 1:
                    note += " %r" % c
        print("   %08X %-6s %-40s%s" % (p[0], p[2], p[3], note))
