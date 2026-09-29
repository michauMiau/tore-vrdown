#!/usr/bin/env python3
"""S21 - COMPLETE ACCOUNTING of all 68 SteamInput-selecting dispatch sites.

For each, walk forward printing every instruction until rax is redefined, and
record what became of the returned ISteamInput*.  Also re-examine the sites
that earlier per-function scans reported (e.g. 0x4DF7E5 off 0x30) to see
whether they really are Steam Input.
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


def kills_rax(p):
    if p[2] in ("mov", "movzx", "movsxd", "lea", "pop"):
        return p[3].split(",")[0].strip() in ("rax", "eax", "ax", "al")
    if p[2] in ("xor",) and p[3].startswith(("rax", "eax")):
        return True
    if p[2] in ("add", "sub", "and", "or", "imul", "shl", "shr", "sar", "inc",
                "dec", "neg", "not", "bswap", "movq", "movd", "xchg"):
        return p[3].split(",")[0].strip() in ("rax", "eax", "ax", "al", "r", "e")
    if p[2] in ("call", "ret", "jmp", "nop", "int3", "cmp", "test", "push",
                "lea", "endbr64", "cdqe", "cqo", "leave", "stos", "movs",
                "movups", "movaps", "movdqa", "setz", "sete", "setne",
                "movl", "movss", "movsd", "addss", "subss", "cvtsi2ss",
                "unpck", "pxor"):
        return False
    return True


rows = []
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
    if sel != SI_SEL:
        continue
    cur = r[0] + r[1]
    seq, vcall = [], None
    for _ in range(26):
        p = byaddr.get(cur)
        if p is None:
            break
        seq.append(p)
        if p[2] == "mov" and p[3] == "rcx, qword ptr [rax]":
            q = byaddr.get(p[0] + p[1])
            if q and q[2] == "mov" and q[3] == "rax, qword ptr [rcx]":
                c = byaddr.get(q[0] + q[1])
                if c and c[2] == "call":
                    m = CALLP.match(c[3])
                    if m:
                        vcall = (c[0], int(m.group(2), 0))
        if p is seq[-1] and vcall and p[2] == "call" and p[3].startswith("qword ptr ["):
            pass
        if p[2] == "call" and "qword ptr [r" in p[3] and "+" in p[3] and vcall is None:
            m = CALLP.match(p[3])
            if m:
                vcall = (p[0], int(m.group(2), 0))
        if kills_rax(p) and not (p[2] == "call"):
            break
        cur += p[1]
    f = pd.func(r[0])
    rows.append((r[0], vcall, seq, f))

print("=== 68 SteamInput dispatch sites: what happened to the pointer ===")
nv = sum(1 for _, v, _, _ in rows if v)
print("  %d produce a vtable call, %d do not\n" % (nv, len(rows) - nv))
agg = collections.Counter()
for a, v, seq, f in rows:
    if v:
        agg[v[1]] += 1
    else:
        print("  NO VTABLE CALL at 0x%08X  fn %s" % (a, "%08X..%08X" % (f[0], f[1])))
        for p in seq[:10]:
            print("       %08X %-6s %s" % (p[0], p[2], p[3]))
print("\n=== offsets ===")
for off, c in sorted(agg.items()):
    print("  0x%03X slot %3d  %3d" % (off, off // 8, c))

print("\n=== re-examine the 0x4DF760..0x4DF859 function (s4 reported off 0x30) ===")
f = pd.func(0x004DF7E5)
print("fn 0x%08X..0x%08X" % (f[0], f[1]))
for p in dis(max(f[0], 0x004DF7B0), 60):
    if p[0] >= f[1]:
        break
    note = ""
    if "rip" in p[3]:
        dd = disp_of(p)
        if dd is not None:
            t = p[0] + p[1] + dd
            note = "  ; ->0x%08X" % t
            c = cstr(t, 20)
            if c.isprintable() and len(c) > 1:
                note += " %r" % c
    print("  %08X %-6s %-38s %-22s%s" % (p[0], p[2], p[3], hexs(p[0], p[1]), note))
