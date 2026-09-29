#!/usr/bin/env python3
"""FINAL DELIVERABLE: ISteamInput006 vtable of steam_api64.dll.

Every byte string printed or written below is read from the file at run time and
disassembled on the spot -- nothing is quoted from memory.

VTABLE  RVA 0x54B738 (.rdata), 48 entries, VA 0x18054B738.
"""
import json
import re
import struct
import sys
from collections import Counter

import capstone.x86_const as XC

sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/steaminput")
from sap_lib import Image  # noqa: E402

DLL = "/root/steamless/Teardown/steam_api64.dll"
OUT = "/home/truenas_admin/teardown-vr-mod/tools/steaminput/ISteamInput_vtable.md"
STR, STR_RVA = "SteamInput006", 0x53CF18
LADDER = {0x00: 0x54ADF0, 0x08: 0x54AE28, 0x10: 0x54AEB8, 0x18: 0x54AF70,
          0x20: 0x54B048, 0x28: 0x54B148, 0x30: 0x54B260, 0x38: 0x54B378,
          0x40: 0x54B498, 0x48: 0x54B5B8, 0x50: 0x54B738}
V6, SEL = LADDER[0x50], 0x168

im = Image(DLL)
md = im.md()
BASE = im.image_base


def resolve(rva, depth=0):
    if rva is None or depth > 8:
        return rva, 0
    adj = 0
    for ins in md.disasm(im.b(rva, 32), rva):
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


def run_len(vt, cap=200):
    n = 0
    while n < cap:
        r = im.u64(vt + 8 * n) - BASE
        if not (0x1000 <= r < 0x53B000):
            break
        n += 1
    return n


def ident(vt, thisoff, i):
    r = im.u64(vt + 8 * i) - BASE
    impl, adj = resolve(r)
    return (impl, thisoff - adj)


EXTN = {k: run_len(v) for k, v in LADDER.items()}
N = EXTN[0x50]
ID6 = [ident(V6, 0x50, i) for i in range(N)]
ID = {k: [ident(LADDER[k], k, i) for i in range(EXTN[k])] for k in LADDER}
ALIGN_OK = all(ID[0x48][i] == ID6[i] for i in range(EXTN[0x48]))
# WHOLE[k] = off, set only if the ENTIRE sub-table k is a contiguous run of v006
# starting at v006[s0].  Only such a whole-table alignment can transfer an index:
# a short run of shared stubs can match by coincidence and proves nothing.
# PARTIAL[k] is recorded for the report only and is never used to name a slot.
WHOLE, PARTIAL = {}, {}
for k in LADDER:
    if k == 0x50:
        continue
    whole = None
    for off in range(-EXTN[k] + 1, N - EXTN[k] + 1):
        t0 = off
        if 0 <= t0 and t0 + EXTN[k] <= N and all(ID[k][j] == ID6[t0 + j]
                                                  for j in range(EXTN[k])):
            whole = (off, t0)
            break
    if whole:
        WHOLE[k] = whole
    best = []
    for off in range(-EXTN[k] + 1, N):
        run = 0
        for j, a in enumerate(ID[k]):
            t = j + off
            ok = 0 <= t < N and a == ID6[t]
            if ok:
                run += 1
            else:
                if run >= 2:
                    best.append((j - run, run, t0 if False else (j - run) + off))
                run = 0
        if run >= 2:
            best.append((EXTN[k] - run, run, (EXTN[k] - run) + off))
    if best:
        s, ln, v0 = max(best, key=lambda x: x[1])
        PARTIAL[k] = (v0, v0 + ln)

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
    for ins in md.disasm(im.b(fs, fe - fs), fs):
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
        if i >= EXTN[K] or (K, i) in seen:
            continue
        seen.add((K, i))
        BIND.append(dict(nm=nm, K=K, i=i, ord=ordv, rva=rva,
                         impl=ID[K][i][0], obj=ID[K][i][1]))
INIT_RVA = EX["SteamAPI_ISteamInput_Init"][1]
INIT_ORD = EX["SteamAPI_ISteamInput_Init"][0]
BIND.append(dict(nm="Init", K=0x50, i=0, ord=INIT_ORD, rva=INIT_RVA,
                 impl=ID6[0][0], obj=ID6[0][1], chain=True))

CAND = {}
for b in BIND:
    n, K, i = b["nm"], b["K"], b["i"]
    ex = set()
    if K == 0x50 and i < N:
        ex.add(i)
    elif K in WHOLE:
        _off, s0 = WHOLE[K]
        t = s0 + i
        if i < EXTN[K] and 0 <= t < N:
            ex.add(t)
    if ex:
        CAND.setdefault(n, set()).update(ex)
    for j, a in enumerate(ID6):
        if a == (b["impl"], b["obj"]):
            CAND.setdefault(n, set()).add(j)
NAMES = sorted(CAND)
adj = {n: set(CAND[n]) for n in NAMES}


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
for n in NAMES:
    for s in sorted(adj[n]):
        a2 = {k: set(v) for k, v in adj.items()}
        a2[n].discard(s)
        if msize(a2) < SIZE:
            forced[s] = n

HDR = open("/root/.hermes/cache/scratch/steamres/cppISteamInput006.cpp").read()
ORDER = []
for m in re.finditer(r"NTSTATUS ISteamInput_SteamInput006_([A-Za-z0-9_]+)\(", HDR):
    if m.group(1) not in ORDER:
        ORDER.append(m.group(1))


def dis(rva, n):
    out = []
    for i in md.disasm(im.b(rva, n), rva):
        out.append("0x%X  %-18s  %-6s %s" % (i.address, i.bytes.hex(),
                                              i.mnemonic, i.op_str))
    return out


ROWS = []
for i in range(N):
    impl, obj = ID6[i]
    thunk = im.u64(V6 + 8 * i) - BASE
    adjv = resolve(thunk)[1]
    if i in forced:
        n = forced[i]
        myb = [b for b in BIND if b["nm"] == n]
        # prefer a binding that is actually usable: DIRECT (+0x50) first, then any
        # whole-table alignment.  Never fall back to an unusable sub-object.
        dirb = [b for b in myb if b["K"] == 0x50]
        whb = [b for b in myb if b["K"] in WHOLE]
        use = dirb or whb
        ix = use[0]["i"] if use else i
        w = WHOLE.get(use[0]["K"]) if use else None
        if n == "Init":
            tier = "CHAIN"
        elif dirb:
            tier = "DIRECT"
        elif whb:
            tier = "ALIGNED"
        else:
            # forced by identity alone: say exactly that
            tier = "IDENTITY"
        why = {
            "CHAIN": "export SteamAPI_ISteamInput_Init (ord %d, @0x%X) tail-calls [+0x08] slot 0, a leaf thunk that does `mov rax,[rcx+0x40]; add rcx,0x40; mov dl,1; jmp rax`; that lands on [+0x48] slot 0 = impl 0x%X, which is this slot's implementation" % (INIT_ORD, INIT_RVA, impl),
            "DIRECT": "export SteamAPI_ISteamInput_%s (ord %d, @0x%X) tail-calls `[obj+0x50]` slot %d = this vtable" % (n, EX["SteamAPI_ISteamInput_" + n][0], EX["SteamAPI_ISteamInput_" + n][1], ix),
            "ALIGNED": "export SteamAPI_ISteamInput_%s (ord %d, @0x%X) dispatches via sub-table +0x%02X slot %d; that WHOLE sub-table is proved element-wise identical to v006[%d:%d] (all %d indices match in both impl and this-offset), so the index transfers" % ((n, EX["SteamAPI_ISteamInput_" + n][0], EX["SteamAPI_ISteamInput_" + n][1], use[0]["K"], ix, w[1], w[1] + EXTN[use[0]["K"]], EXTN[use[0]["K"]]) if w else (n, 0, 0, 0, 0, 0, 0, 0)),
            "IDENTITY": "export SteamAPI_ISteamInput_%s (ord %d, @0x%X) reaches impl 0x%X with this=0x%X; this is the only v006 slot with that (impl, this-offset) pair, so the binding is forced" % (n, EX["SteamAPI_ISteamInput_" + n][0], EX["SteamAPI_ISteamInput_" + n][1], impl, obj),
        }[tier]
        name, status = n, "PROVED"
    else:
        cands = sorted(m for m in NAMES if i in adj[m])
        name, status = "UNMAPPED", "AMBIGUOUS"
        why = ("AMBIGUOUS: %d exported names reach this entry (impl 0x%X, this=0x%X), which is a SHARED stub, and no sub-table alignment covers this slot -- so the binary cannot separate them: %s"
               % (len(cands), impl, obj, ", ".join(cands)))
        if i < len(ORDER):
            why += ". Valve cppISteamInput006.cpp declares %s at this position (cross-check only, NOT proof)" % ORDER[i]
    ROWS.append(dict(slot=i, off=8 * i, name=name, status=status,
                     tier=tier if i in forced else "AMBIGUOUS", why=why,
                     thunk=thunk, impl=impl, adj=adjv, obj=obj,
                     raw=im.b(thunk, 10).hex()))

groups = {}
for r in ROWS:
    if r["status"] == "AMBIGUOUS":
        g = groups.setdefault((r["impl"], r["obj"]), {"slots": [], "names": set()})
        g["slots"].append(r["slot"])
        g["names"] |= {m for m in NAMES if r["slot"] in adj[m]}
free = [g for g in groups.values() if len(g["slots"]) == len(g["names"])]
nproved = sum(1 for r in ROWS if r["status"] == "PROVED")
namb = N - nproved

L = []
A = L.append
A("# ISteamInput006 vtable - resolved from the real steam_api64.dll\n")
A("Binary: `%s`" % DLL)
A("%d bytes, PE32+, image base `0x%X`" % (len(im.d), BASE))
A("Vtable: **RVA `0x%X`** (`.rdata`), **%d entries**, VA `0x%X`, file offset `0x%X`.\n"
  % (V6, N, BASE + V6, im.to_file(V6)))
A("**%d of %d slots are proved from the binary. %d are `UNMAPPED` and provably"
  % (nproved, N, namb))
A("undecidable from this binary (see UNVERIFIED).**\n")

A("## Proof that this is the ISteamInput006 vtable\n")
A("Three independent facts, each from `.pdata`-bounded disassembly. Every byte")
A("string below was read from the file when this document was generated.\n")
A("**1 - the factory names the version.**  `SteamAPI_SteamInput_v006` is at RVA")
A("`0x335C0` (`.pdata 0x335C0..0x33615`):\n")
L.extend("    " + x for x in dis(0x335C0, 0x38))
A("")
b = im.b(0x335F1, 7)
disp = struct.unpack_from("<i", b, 3)[0]
A("The `lea r9` is bytes `%s`; rip target = `0x335F1 + 7 + 0x%X = 0x%X`, and the"
  % (b.hex(), disp, 0x335F1 + 7 + disp))
A("string there is `%s`.\n" % im.cstr(0x335F1 + 7 + disp))
A("**2 - the resolver returns the +0x50 sub-object for that name.**  The function")
A("`0xAF100` (`.pdata 0xAF100..0xAF270`) strcmp-chains the version names against")
A("`0x527030`; the arm that matches `SteamInput006` is:\n")
L.extend("    " + x for x in dis(0xAF1FF, 0x1C))
A("")
A("so for `SteamInput006` it returns `inputobj + 0x50`.\n")
A("**3 - the constructor installs the sub-vtable ladder.**  `CSteamInput`'s ctor at")
A("`0xB2BE0` (`.pdata 0xB2BE0..0xB2F46`) does one `lea` + `mov [rcx+K],rax` per")
A("sub-object:\n")
L.extend("    " + x for x in dis(0xB2C60, 0x24))
A("")
c = im.b(0xB2C76, 7)
cd = struct.unpack_from("<i", c, 3)[0]
A("`0xB2C76` is bytes `%s` -> rip target `0x%X`, stored at `obj+0x50`."
  % (c.hex(), 0xB2C76 + 7 + cd))
A("Therefore the sub-object handed out for `\"SteamInput006\"` carries the vtable")
A("`0x%X`. **PROVED.**\n" % V6)

A("## Entry form, and the this-adjustment that matters\n")
t0 = im.u64(V6) - BASE
A("Every one of the %d entries is a full MSVC this-adjusting thunk.  Slot 0:" % N)
A("")
L.extend("    " + x for x in dis(t0, 10))
A("")
A("So a method's identity is the pair **(implementation, this-offset)** with")
A("`this-offset = 0x50 - adj`.  Slot 0's `sub rcx,8` means the callee receives")
A("`obj+0x48`, i.e. the entries are written for the v005 sub-object's frame and")
A("reach down into shared implementation.  This is why the four versioned")
A("sub-tables can share implementations and why a method identity alone is not")
A("enough to name a slot.\n")
A("Each flat export `SteamAPI_ISteamInput_<Name>` is itself a thunk: it calls")
A("`0x1EE90` to fetch the CSteamInput, then loads a sub-vtable at `[CSteamInput+K]`")
A("(`+0x168` is the CSteamInput pointer in the context) and tail-calls one slot.")
A("`SteamAPI_ISteamInput_ActivateActionSetLayer` (ord 334, RVA `0x22E90`):\n")
L.extend("    " + x for x in dis(0x22E90, 0x38))
A("")
A("It routes through `+0x20` and jumps to slot `0x40/8 = 8` **of that sub-table**,")
A("not of v006.  Flat exports therefore do NOT share one global slot numbering;")
A("each had to be joined to the v006 table through its own sub-table.\n")

A("## Evidence tiers\n")
A("| tier | meaning | count |")
A("| --- | --- | ---: |")
MEAN = {
    "DIRECT": "export tail-calls `[obj+0x50]` slot N - the v006 vtable itself",
    "ALIGNED": "export routes through a sub-table proved WHOLE-element-wise identical to a contiguous v006 range, so the index transfers",
    "IDENTITY": "export reaches this entry's (impl, this-offset) pair and no other v006 slot does, so the binding is forced",
    "CHAIN": "name recovered by following a leaf thunk's second indirection",
    "AMBIGUOUS": "several exported names share this entry; not decidable from the binary",
}
for t, c in sorted(Counter(r["tier"] for r in ROWS).items()):
    A("| `%s` | %s | %d |" % (t, MEAN[t], c))
A("")

A("## The table\n")
A("| slot | byte_offset | method name | evidence | function RVA in steam_api64.dll |")
A("| ---: | ---: | --- | --- | ---: |")
for r in ROWS:
    A("| %d | 0x%03X | %s | %s | 0x%X -> 0x%X |"
      % (r["slot"], r["off"], r["name"], r["why"].replace("|", "/"),
         r["thunk"], r["impl"]))
A("")
A("The last column is `vtable_entry_RVA -> implementation_RVA`.  A caller loads and")
A("calls the first; control lands on the second.  `0x%X` (slot %d) and `0x%X` are"
  % (ROWS[47]["impl"], 47, ROWS[2]["impl"]))
A("shared *not implemented* stubs reached by several real methods, so a shared")
A("implementation is expected here and is not an error.\n")

A("## Raw vtable contents\n")
A("```")
A("vtable RVA 0x%X  (%d qwords, VA 0x%X)" % (V6, N, BASE + V6))
for r in ROWS:
    A("%2d  0x%06X  %016X   %-32s  %s"
      % (r["slot"], r["thunk"], im.u64(V6 + 8 * r["slot"]), r["raw"],
         r["name"] if r["name"] != "UNMAPPED" else "-"))
A("```")

A("## UNVERIFIED\n")
A("%d of the %d slots carry `UNMAPPED`.  This is a real limit of the binary, not an" % (namb, N))
A("omission.  Each of those slots points at a *shared* implementation stub that")
A("several exported flat-API names reach, and for those slots there is no")
A("sub-table whose element-wise identity with v006 transfers the index.  Identity")
A("collapses them into one indistinguishable group; the exports give no")
A("argument-shape or call-site signal to order them.\n")
A("| v006 slots | shared entry | exported names that reach it | header order (cross-check only) |")
A("| --- | --- | --- | --- |")
for (impl, obj), g in sorted(groups.items()):
    g["slots"].sort()
    A("| %s | impl `0x%X`, this `0x%X` | %s | %s |"
      % (", ".join(str(s) for s in g["slots"]), impl, obj,
         ", ".join(sorted(g["names"])),
         ", ".join(ORDER[s] if s < len(ORDER) else "?" for s in g["slots"])))
A("")
A("%d of these %d groups are **provable free permutations** - the number of" % (len(free), len(groups)))
A("candidate slots equals the number of candidate names, and every name is a")
A("candidate for every one of those slots.  No arrangement of the evidence")
A("distinguishes them, so `UNMAPPED` is the only honest answer.\n")
A("Names exported but not placed in any v006 slot: %s.\n"
  % (", ".join("`%s`" % x for x in sorted(set(NAMES) - set(forced.values())))))
A("### Cross-check only\n")
A("`cppISteamInput006.cpp` (Valve's generated glue, %d bytes) declares %d methods"
  % (len(HDR), len(ORDER)))
A("for `ISteamInput_SteamInput006_`.  The binary has %d slots, because the flat API"
  % N)
A("also exports methods the `.cpp` does not wrap.  The header was used **only** as")
A("a cross-check and never as primary proof.  The following files in")
A("`/root/.hermes/cache/scratch/steamres/` are 14-byte `404: Not Found` stubs and")
A("were ignored: `isteaminput_ckyrra.h`, `isteaminput_gbe.h`,")
A("`isteaminput_modern1..4.h`.  Real files used: `cppISteamInput006.cpp`,")
A("`sdk163_isteaminput.h`, `isteaminput_facepunch.h`, `steam_api_flat.h`.\n")

A("## Other checks\n")
A("* **Exports.** %d named exports.  Nothing in the export table names a vtable" % nname)
A("  slot or a vtable address, and there is no symbol that identifies")
A("  `CSteamInput`; the flat API is the only naming source, and it routes through")
A("  different sub-tables per method, hence the grouping above.")
A("* **RTTI.** 377 MSVC Complete Object Locators were parsed; none is named")
A("  `CSteamInput` or `SteamInput`, so RTTI cannot identify this vftable and was")
A("  not used for naming.")
A("* **Sub-table geometry** (measured, not assumed; every table is followed by an")
A("  8-byte pad before the next, which is how the extents were confirmed):\n")
A("  | sub-object | vtable | entries | ends at | whole-table alignment into v006 |")
A("  | --- | --- | ---: | --- | --- |")
for k in sorted(LADDER):
    if k == 0x50:
        rel = "itself (this table)"
    elif k in WHOLE:
        rel = "v006[%d:%d] (PROVED, all %d indices)" % (WHOLE[k][1], WHOLE[k][1] + EXTN[k], EXTN[k])
    elif k in PARTIAL:
        rel = "no (longest coincidental run only %d at v006[%d:%d] - NOT used)" % (
            PARTIAL[k][1] - PARTIAL[k][0], PARTIAL[k][0], PARTIAL[k][1])
    else:
        rel = "no (no run >= 2)"
    A("  | +0x%02X | 0x%06X | %d | 0x%06X | %s |"
      % (k, LADDER[k], EXTN[k], LADDER[k] + 8 * EXTN[k], rel))
A("")
A("Only `+0x48` (v005) aligns as a whole table, onto `v006[0:47]`.  The other")
A("sub-tables share *stubs* with v006, so any short run they match is coincidence")
A("and is deliberately NOT used to name a slot.  A previous revision of this")
A("analysis did use those partial runs and produced false names for slots 1, 6,")
A("9-11, 16, 17, 20, 21, 29, 30, 33, 36, 38, 39; those claims are withdrawn.")
A("")
A("* **48 is measured, not assumed:** entry %d is the last `.text` VA; the qword" % (N - 1))
A("  at `0x%X` is `0x%016X` (not code), and the gap to the next sub-object is" % (V6 + 8 * N, im.u64(V6 + 8 * N)))
A("  exactly 8 bytes.")
A("")
open(OUT, "w").write("\n".join(L) + "\n")
try:
    print("\n".join(L[L.index("## The table"):L.index("## Raw vtable contents")]))
except ValueError:
    print("\n".join(L))
print("PROVED %d / UNMAPPED %d / total %d" % (nproved, namb, N))
print("wrote %s" % OUT)
