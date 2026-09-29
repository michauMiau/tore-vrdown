#!/usr/bin/env python3
"""FINAL ISteamInput006 vtable resolution.

Refinements over s41:
  * INDEX-EXACT bindings dominate IDENTITY.  If a name has any binding through
    the v005 (+0x48) or v006 (+0x50) sub-table, its slot number is known
    exactly, so the (impl,this) identity candidates -- which collide on shared
    "not implemented" stubs -- are discarded as noise for that name.
  * For the remaining names, we additionally search every sub-table for a
    maximal element-wise run against v006; a run of length>=2 with a constant
    offset is an INDEX-EXACT alignment and transfers indices too.
  * A name/slot pair is only reported PROVED when it is a forced edge, i.e. it
    appears in EVERY maximum matching of the bipartite graph.
"""
import json
import re
import struct
import sys

import capstone.x86_const as XC

sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/steaminput")
from sap_lib import Image  # noqa: E402

im = Image()
md = im.md()
BASE = im.image_base
LADDER = {0x00: 0x54ADF0, 0x08: 0x54AE28, 0x10: 0x54AEB8, 0x18: 0x54AF70,
          0x20: 0x54B048, 0x28: 0x54B148, 0x30: 0x54B260, 0x38: 0x54B378,
          0x40: 0x54B498, 0x48: 0x54B5B8, 0x50: 0x54B738}
V6, SEL = LADDER[0x50], 0x168


def resolve(rva, depth=0):
    if rva is None or depth > 8:
        return rva, 0
    adj = 0
    for ins in md.disasm(im.code(rva, 32), rva):
        o = ins.operands
        if ins.mnemonic == "sub" and len(o) == 2 and o[0].reg == XC.X86_REG_RCX \
                and o[1].type == 2:
            adj += o[1].imm
            continue
        if ins.mnemonic == "jmp" and len(o) == 1 and o[0].type == 2:
            t, ta = resolve(o[0].imm, depth + 1)
            return t, adj + ta
        break
    return rva, adj


def extent(vt, cap=90):
    out = []
    for i in range(cap):
        r = im.u64(vt + 8 * i) - BASE
        if not (0x1000 <= r < 0x53B000):
            break
        out.append(r)
    return out


EXT = {k: extent(v) for k, v in LADDER.items()}


def ident(vt, thisoff, i):
    r = extent(vt)[i]
    impl, adj = resolve(r)
    return (impl, thisoff - adj)


ID6 = [ident(V6, 0x50, i) for i in range(len(EXT[0x50]))]
ID = {k: [ident(v, k, i) for i in range(len(EXT[k]))] for k, v in LADDER.items()}
N = len(ID6)
ALIGN_OK = all(ID[0x48][i] == ID6[i] for i in range(min(len(ID[0x48]), N)))

# ---- search each sub-table for maximal constant-offset runs vs v006 ----
ALIGNMAP = {}
for k, ids in ID.items():
    if k == 0x50:
        continue
    best = []
    for off in range(-len(ids) + 1, N):
        run = 0
        for j, a in enumerate(ids):
            t = j + off
            if 0 <= t < N and a == ID6[t]:
                run += 1
            else:
                if run >= 2:
                    best.append((off, j - run, run))
                run = 0
        if run >= 2:
            best.append((off, len(ids) - run, run))
    if best:
        b = max(best, key=lambda x: x[2])
        ALIGNMAP[k] = b
        print("sub-table +0x%02X: element-wise run of %d at v006[%d:%d] (offset %+d)"
              % (k, b[2], b[0], b[0] + b[2], b[1]))
    else:
        print("sub-table +0x%02X: no run >= 2 against v006" % k)
print("v005(+0x48) element-wise identical to v006[0:%d]: %s"
      % (len(ID[0x48]), ALIGN_OK))

e = im.to_file(im.dirs[0][0])
f = struct.unpack_from("<IIHHIIIIIII", im.d, e)
base_, nname, funcsRva, namesRva, ordsRva = f[5], f[7], f[8], f[9], f[10]
EX = {}
for i in range(nname):
    o = im.u16(ordsRva + 2 * i)
    EX[im.cstr(im.u32(namesRva + 4 * i))] = (o + base_, im.u32(funcsRva + 4 * o))


def dispatches(rva):
    fs, fe = im.fn_start(rva), im.fn_end(rva)
    vt_of, slotreg, out = {}, {}, []
    for ins in md.disasm(im.code(fs, fe - fs), fs):
        o = ins.operands
        if ins.mnemonic == "mov" and len(o) == 2 and o[0].type == 1 and o[1].type == 3:
            m = o[1].mem
            if m.index == 0 and m.scale == 1 and m.base in vt_of:
                slotreg[o[0].reg] = (vt_of[m.base], m.disp)
            elif m.index == 0 and m.scale == 1 and m.base == XC.X86_REG_RCX \
                    and m.disp in LADDER:
                vt_of[o[0].reg] = m.disp
        if ins.mnemonic in ("jmp", "call") and len(o) == 1:
            if o[0].type == 3:
                m = o[0].mem
                if m.index == 0 and m.scale == 1 and m.base in vt_of:
                    out.append((vt_of[m.base], m.disp))
            elif o[0].type == 1 and o[0].reg in slotreg:
                out.append(slotreg[o[0].reg])
    return out


BIND = []
for full, (ordv, rva) in EX.items():
    if not full.startswith("SteamAPI_ISteamInput_"):
        continue
    nm = full[len("SteamAPI_ISteamInput_"):]
    seen = set()
    for (K, sl) in dispatches(rva):
        if K not in LADDER or sl % 8:
            continue
        i = sl // 8
        if i >= len(EXT[K]) or (K, i) in seen:
            continue
        seen.add((K, i))
        BIND.append(dict(nm=nm, K=K, i=i, ord=ordv, rva=rva,
                         impl=ID[K][i][0], obj=ID[K][i][1]))
# proved Init delegation: exported Init tail-calls [+0x08] slot 0, a leaf thunk
# `mov rax,[rcx+0x40]; add rcx,0x40; mov dl,1; jmp rax`  -> the impl it reaches
# is 0xB7E70, which is exactly v006 slot 0's implementation (see s39).
BIND.append(dict(nm="Init", K=0x50, i=0, ord=EX["SteamAPI_ISteamInput_Init"][0],
                 rva=EX["SteamAPI_ISteamInput_Init"][1],
                 impl=ID6[0][0], obj=ID6[0][1], special="CHAIN"))

CAND, EVKIND = {}, {}
for b in BIND:
    n, K, i = b["nm"], b["K"], b["i"]
    ex = set()
    if K == 0x50 and i < N:
        ex.add(i)
    elif K == 0x48 and ALIGN_OK and i < N:
        ex.add(i)
    elif K in ALIGNMAP:
        off, s, ln = ALIGNMAP[K]
        t = i + off
        if s <= t < s + ln and 0 <= t < N:
            ex.add(t)
    if ex:
        CAND.setdefault(n, {}).setdefault("EXACT", set()).update(ex)
    for j, a in enumerate(ID6):
        if a == (b["impl"], b["obj"]):
            CAND.setdefault(n, {}).setdefault("IDENT", set()).add(j)

FINAL = {n: (d.get("EXACT", set()), d.get("IDENT", set())) for n, d in CAND.items()}

adj = {n: (v[0] | v[1]) for n, v in FINAL.items()}


def msize(a):
    ms = {}

    def k(n, seen):
        for s in sorted(a[n]):
            if s in seen:
                continue
            seen.add(s)
            if s not in ms or k(ms[s], seen):
                ms[s] = n
                return True
        return False
    for n in sorted(a, key=lambda x: len(a[x])):
        k(n, set())
    return len(ms)


SIZE = msize(adj)
forced = {}
for n in adj:
    for s in sorted(adj[n]):
        a2 = {k: set(v) for k, v in adj.items()}
        a2[n].discard(s)
        if msize(a2) < SIZE:
            forced[s] = n
print("\nmaximum matching %d | forced edges %d" % (SIZE, len(forced)))
print("unplaced names: %s" % sorted(set(adj) - set(forced.values())))

HDR = open("/root/.hermes/cache/scratch/steamres/cppISteamInput006.cpp").read()
ORDER = []
for m in re.finditer(r"NTSTATUS ISteamInput_SteamInput006_([A-Za-z0-9_]+)\(", HDR):
    if m.group(1) not in ORDER:
        ORDER.append(m.group(1))

ROWS = []
for i in range(N):
    impl, obj = ID6[i]
    thunk = EXT[0x50][i]
    adjv = resolve(thunk)[1]
    if i in forced:
        n = forced[i]
        ex, _id = FINAL[n]
        Ks = [b["K"] for b in BIND if b["nm"] == n]
        if n == "Init":
            tier = "CHAIN"
        elif 0x50 in Ks:
            tier = "DIRECT"
        elif 0x48 in Ks and ALIGN_OK:
            tier = "ALIGNED-v005"
        else:
            tier = "ALIGNED-run"
        am = ALIGNMAP.get(Ks[0]) if Ks else None
        why = {
            "CHAIN": "export SteamAPI_ISteamInput_Init @0x%X tail-calls [+0x08] slot 0 = leaf 0xB7E60 `mov rax,[rcx+0x40]; add rcx,0x40; mov dl,1; jmp rax`, reaching impl 0xB7E70 = this slot's implementation"
                    % EX["SteamAPI_ISteamInput_Init"][1],
            "DIRECT": "export SteamAPI_ISteamInput_%s @0x%X tail-calls [obj+0x50] slot %d = this vtable directly"
                      % (n, EX["SteamAPI_ISteamInput_" + n][1], i),
            "ALIGNED-v005": "export SteamAPI_ISteamInput_%s @0x%X dispatches via sub-table +0x48 slot %d; v005[0:%d] proved element-wise identical to v006[0:%d], so the index transfers"
                            % (n, EX["SteamAPI_ISteamInput_" + n][1], i, len(ID[0x48]), len(ID[0x48])),
            "ALIGNED-run": "export SteamAPI_ISteamInput_%s @0x%X dispatches via sub-table +0x%02X, which is element-wise identical to v006[%d:%d], so the index transfers"
                           % ((n, EX["SteamAPI_ISteamInput_" + n][1], Ks[0], am[0], am[0] + am[2])
                              if am else (n, 0, 0, 0, 0)),
        }[tier]
        name, status = n, "PROVED"
    else:
        cands = sorted(m for m in adj if i in adj[m])
        ex_n = sorted(m for m in cands if i in FINAL[m][0])
        name, status = "UNMAPPED", "AMBIGUOUS"
        why = ("%d exported names share this entry's (impl 0x%X, this=0x%X) -- shared "
               "stub, no binary signal separates them: %s"
               % (len(cands), impl, obj, ", ".join(cands)))
        if ex_n:
            why = ("%d exported names reach this exact slot, mutually interchangeable: %s"
                   % (len(ex_n), ", ".join(ex_n)))
        if i < len(ORDER):
            why += "; cppISteamInput006.cpp order pos %d = %s (cross-check only)" % (i, ORDER[i])
    ROWS.append(dict(slot=i, off=8 * i, name=name, status=status,
                     tier=tier if i in forced else "AMBIGUOUS", why=why,
                     thunk=thunk, impl=impl, adj=adjv, obj=obj))

from collections import Counter  # noqa: E402
print("tiers: %s" % dict(Counter(r["tier"] for r in ROWS)))
json.dump(dict(vtable=V6, n=N, sel=SEL, rows=ROWS, align=ALIGN_OK,
               ladder={hex(k): v for k, v in LADDER.items()},
               ext={hex(k): len(v) for k, v in EXT.items()},
               alignmap={hex(k): v for k, v in ALIGNMAP.items()},
               forced={str(k): v for k, v in forced.items()},
               unplaced=sorted(set(adj) - set(forced.values())), header=ORDER),
          open("/root/.hermes/cache/scratch/steamres/vt006_FINAL2.json", "w"), indent=1)
print("wrote vt006_FINAL2.json")
for r in ROWS:
    print("%2d 0x%03X %-32s %-11s 0x%X -> 0x%X" % (r["slot"], r["off"], r["name"],
                                                    r["tier"], r["thunk"], r["impl"]))
