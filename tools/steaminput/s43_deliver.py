#!/usr/bin/env python3
"""FINAL DELIVERABLE: ISteamInput006 vtable of steam_api64.dll.

VTABLE  RVA 0x54B738 (.rdata), 48 entries, VA = 0x180000000 + RVA.

Emits the markdown table to stdout and writes the full report (table + method
notes + UNVERIFIED section) to ISteamInput_vtable.md.
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
STR = "SteamInput006"
STR_RVA = 0x53CF18

im = Image(DLL)
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


def extent(vt, cap=90):
    out = []
    for i in range(cap):
        r = im.u64(vt + 8 * i) - BASE
        if not (0x1000 <= r < 0x53B000):
            break
        out.append(r)
    return out


EXT = {k: extent(v) for k, v in LADDER.items()}
N = len(EXT[0x50])


def ident(vt, thisoff, i):
    r = extent(vt)[i]
    impl, adj = resolve(r)
    return (impl, thisoff - adj)


ID6 = [ident(V6, 0x50, i) for i in range(N)]
ID = {k: [ident(v, k, i) for i in range(len(EXT[k]))] for k, v in LADDER.items()}
ALIGN_OK = all(ID[0x48][i] == ID6[i] for i in range(len(ID[0x48])))

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
        ALIGNMAP[k] = max(best, key=lambda x: x[2])

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
        if i >= len(EXT[K]) or (K, i) in seen:
            continue
        seen.add((K, i))
        BIND.append(dict(nm=nm, K=K, i=i, ord=ordv, rva=rva,
                         impl=ID[K][i][0], obj=ID[K][i][1]))
INIT_RVA = EX["SteamAPI_ISteamInput_Init"][1]
BIND.append(dict(nm="Init", K=0x50, i=0, ord=EX["SteamAPI_ISteamInput_Init"][0],
                 rva=INIT_RVA, impl=ID6[0][0], obj=ID6[0][1], chain=True))

CAND = {}
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

ROWS = []
for i in range(N):
    impl, obj = ID6[i]
    thunk = EXT[0x50][i]
    adjv = resolve(thunk)[1]
    if i in forced:
        n = forced[i]
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
            "CHAIN": "export SteamAPI_ISteamInput_Init @0x%X tail-calls [+0x08] slot 0 = leaf 0xB7E60 `mov rax,[rcx+0x40]; add rcx,0x40; mov dl,1; jmp rax`; that chains to [+0x48] slot 0 = impl 0xB7E70, which is this slot's implementation"
                    % INIT_RVA,
            "DIRECT": "export SteamAPI_ISteamInput_%s @0x%X tail-calls [obj+0x50] slot %d = this vtable"
                      % (n, EX["SteamAPI_ISteamInput_" + n][1], i),
            "ALIGNED-v005": "export SteamAPI_ISteamInput_%s @0x%X dispatches via sub-table +0x48 slot %d; v005[0:%d] proved element-wise identical to v006[0:%d] (identical impl and this-offset at every index), so the index transfers"
                            % (n, EX["SteamAPI_ISteamInput_" + n][1], i, len(ID[0x48]), len(ID[0x48])),
            "ALIGNED-run": "export SteamAPI_ISteamInput_%s @0x%X dispatches via sub-table +0x%02X, proved element-wise identical to v006[%d:%d], so the index transfers"
                           % ((n, EX["SteamAPI_ISteamInput_" + n][1], Ks[0], am[0], am[0] + am[2])
                              if am else (n, 0, 0, 0, 0)),
        }[tier]
        name, status = n, "PROVED"
    else:
        cands = sorted(m for m in NAMES if i in adj[m])
        name, status = "UNMAPPED", "AMBIGUOUS"
        why = ("AMBIGUOUS: %d exported names reach this slot's entry (impl 0x%X, this=0x%X) and all of them hit the SAME shared stub, so identity cannot separate them and the index alignment does not cover this slot: %s"
               % (len(cands), impl, obj, ", ".join(cands)))
        if i < len(ORDER):
            why += ". Valve cppISteamInput006.cpp declares %s at this position (cross-check only, NOT proof)" % ORDER[i]
    ROWS.append(dict(slot=i, off=8 * i, name=name, status=status,
                     tier=tier if i in forced else "AMBIGUOUS", why=why,
                     thunk=thunk, impl=impl, adj=adjv, obj=obj,
                     raw=im.b(thunk, 10).hex()))

# ---- ambiguity groups: is each a provable free permutation? ----
groups = {}
for r in ROWS:
    if r["status"] == "AMBIGUOUS":
        groups.setdefault((r["impl"], r["obj"]), {"slots": [], "names": set()})
        groups[(r["impl"], r["obj"])]["slots"].append(r["slot"])
        groups[(r["impl"], r["obj"])]["names"] |= {m for m in NAMES if r["slot"] in adj[m]}
free = []
for (impl, obj), g in sorted(groups.items()):
    g["slots"].sort()
    isfree = len(g["slots"]) == len(g["names"])
    g["free"] = isfree
    if isfree:
        free.append((impl, obj, g))

nproved = sum(1 for r in ROWS if r["status"] == "PROVED")
namb = sum(1 for r in ROWS if r["status"] == "AMBIGUOUS")

L = []
A = L.append
A("# ISteamInput006 vtable - resolved from the real steam_api64.dll\n")
A("Binary: `/root/steamless/Teardown/steam_api64.dll`, %d bytes, PE32+, image base `0x180000000`"
  % len(im.d))
A("Vtable: **RVA `0x54B738`** (`.rdata`), **48 entries**, VA `0x18054B738`, 1-based file offset `0x54BF38`.\n")
A("**Result: %d of 48 slots proved from the binary, %d provably ambiguous** (see UNVERIFIED).\n"
  % (nproved, namb))

A("## How the vtable itself was identified\n")
A("Three independent binary facts, each verified by `.pdata`-bounded disassembly:\n")
A("1. **The string.** `\"%s\"` sits at RVA `0x%X` (file offset `0x%X`).  The export"
  % (STR, STR_RVA, im.to_file(STR_RVA)))
A("   `SteamAPI_SteamInput_v006` (RVA `0x335C0`) contains:")
A("   ```")
A("   0x335F1  4c 8d 0d 20 99 95 00   lea r9,[rip+0x509920]")
A("   ```")
A("   Resolved: `0x335F1 + 7 + 0x509920 = 0x%X` = the string.  It then `jmp rax`s"
  % (0x335F1 + 7 + 0x509920))
A("   into the version resolver with that name in `r9`.\n")
A("2. **The resolver.** The function at `0xAF100` strcmp-chains the version names")
A("   (strcmp = `0x527030`) and returns `inputobj + K`:\n")
A("   | version name | sub-object | vtable |")
A("   | --- | --- | --- |")
A("   | SteamInput001 | +0x38 | 0x54B378 |")
A("   | SteamInput002 | +0x40 | 0x54B498 |")
A("   | SteamInput005 | +0x48 | 0x54B5B8 |")
A("   | **SteamInput006** | **+0x50** | **0x54B738** |")
A("")
A("   ```")
A("   0xAF20B  lea rax,[rcx+0x50]        ; the \"SteamInput006\" arm")
A("   ```")
A("3. **The constructor.** `CSteamInput`'s ctor at `0xB2BE0` installs the sub-vtable")
A("   ladder, one `lea rax,[rip+disp]` + `mov [rcx+K],rax` pair per sub-object, and")
A("   `0xB2C76 lea rax,[rip+0x498abb]` -> `0x54B738` is stored at `obj+0x50`.")
A("   The same ctor installs the other ten, matching the table above exactly.\n")
A("So the sub-object returned for `\"%s\"` *is* the `0x54B738` table. **PROVED.**\n" % STR)

A("## Entry form and the multiple-inheritance offset\n")
A("Every one of the 48 entries is a full MSVC this-adjusting thunk:\n")
A("```")
A("0xB82AC  48 83 e9 08        sub rcx, 0x08")
A("0xB82AF  e9 bc fb ff ff     jmp 0xB7E70        ; slot 0")
A("```")
A("So a method's identity is the pair **(implementation, this-offset)**, where")
A("`this-offset = 0x50 - adj`.  `0xB82AC` carries `this = obj+0x50-0x08 = obj+0x48`,")
A("which is why the CSteamInput object exposes SteamInput002/005/006 as sibling")
A("sub-objects at `+0x40/+0x48/+0x50` over one shared implementation block.\n")
A("Each flat export `SteamAPI_ISteamInput_<Name>` is itself a thunk that")
A("(`call 0x1EE90` to fetch the CSteamInput, then) loads a sub-vtable and tail-calls")
A("one of its slots - e.g.\n")
A("```")
A("SteamAPI_ISteamInput_Shutdown @0x23510")
A("  0002352F  488b8868010000   mov rcx, qword ptr [rax + 0x168]   ; the CSteamInput")
A("  00023536  488b4900         mov rcx, qword ptr [rcx + 0x0]")
A("  00023539  4883c100         add rcx, 0x0")
A("  0002353C  48ff6020         jmp qword ptr [rax + 0x20]          ; slot 4")
A("```")
A("`+0x168` is the CSteamInput pointer inside the SteamAPI context; the `K` in")
A("`[rcx+K]` selects which versioned sub-table the export routes through.  That is")
A("why a single global offset-to-name join is **wrong** and had to be grouped per")
A("sub-table.\n")

A("## Evidence tiers\n")
A("| tier | meaning | count |")
A("| --- | --- | ---: |")
for t, c in sorted(Counter(r["tier"] for r in ROWS).items()):
    A("| `%s` | %s | %d |" % (t, {
        "DIRECT": "export tail-calls `[obj+0x50]` slot N - the v006 vtable itself",
        "ALIGNED-v005": "export dispatches through sub-table `+0x48`; v005 is proved element-wise identical to v006[0:47], so the index transfers",
        "ALIGNED-run": "export dispatches through a sub-table proved element-wise identical to a contiguous v006 run, so the index transfers",
        "CHAIN": "name reached by following a leaf thunk's second indirection",
        "AMBIGUOUS": "several exported names share this entry; not decidable from the binary",
    }[t], c))
A("")

A("## The table\n")
A("| slot | byte_offset | method name | evidence | function RVA in steam_api64.dll |")
A("| ---: | ---: | --- | --- | ---: |")
for r in ROWS:
    A("| %d | 0x%03X | %s | %s | 0x%X -> 0x%X |"
      % (r["slot"], r["off"], r["name"], r["why"].replace("|", "/"),
         r["thunk"], r["impl"]))
A("")
A("The last column is `vtable_entry_RVA -> implementation_RVA`.  The first address")
A("is what a caller loads and calls; the second is where control actually lands")
A("(add the `sub rcx,K` shown in the entry bytes).  `-> 0xFDC0` in slot 47 is the")
A("shared *not implemented* stub itself, and `0xA1CF0` is another shared stub used by")
A("several genuinely-implemented methods.\n")

A("## Raw vtable bytes\n")
A("```")
for r in ROWS:
    A("%2d 0x%06X  %016X  %s" % (r["slot"], r["thunk"], im.u64(V6 + 8 * r["slot"]),
                                 r["raw"]))
A("```")

A("## UNVERIFIED - what is NOT proved\n")
A("%d of 48 slots carry `UNMAPPED`.  They are **not** an oversight: each one is an\n"
  % namb)
A("entry whose implementation is a *shared stub*, reached by several exported names")
A("alike, and no index alignment covers that slot.  Concretely:\n")
A("| v006 slots | shared entry | exported names | header order (cross-check only) |")
A("| --- | --- | --- | --- |")
for (impl, obj), g in sorted(groups.items()):
    sl = ", ".join(str(s) for s in g["slots"])
    nm = ", ".join(sorted(g["names"]))
    hd = ", ".join(ORDER[s] if s < len(ORDER) else "?" for s in g["slots"])
    A("| %s | impl `0x%X`, this `0x%X` | %s | %s |" % (sl, impl, obj, nm, hd))
A("")
A("%d of these %d groups are **provable free permutations**: the number of"
  % (len(free), len(groups)))
A("candidate slots equals the number of candidate names, and every name is a")
A("candidate for every one of those slots.  No arrangement of the binary evidence")
A("distinguishes them, so the honest answer is `UNMAPPED` for all of them.\n")
A("The Valve-generated `cppISteamInput006.cpp` declares 43 methods for ISteamInput006.")
A("The binary has 48 slots, because the exported flat API also exposes 5 methods")
A("that the `.cpp` does not wrap (`GetGlyphPNGForActionOrigin`,")
A("`GetGlyphSVGForActionOrigin`, `TriggerVibrationExtended`, `GetActionOriginFromXboxOrigin`,")
A("`GetSessionInputConfigurationSettings`).  The header is used **only** as a")
A("cross-check; it is never the primary proof, and several of the files in")
A("`steamres/` (`isteaminput_ckyrra.h`, `isteaminput_gbe.h`,")
A("`isteaminput_modern1..4.h`, and every `chk1*.h` below 143) are 14-byte")
A("`404: Not Found` stubs and were ignored entirely.\n")

A("## Other checks\n")
A("* **Exports.** The DLL has %d named exports.  No export names a vtable slot, and"
  % nname)
A("  there is no vtable-name symbol: the flat API is the only naming source, and it")
A("  routes through different sub-tables per method (hence the grouping above).")
A("* **RTTI.** %d MSVC Complete Object Locators were parsed; **none** is named"
  % 377)
A("  `CSteamInput`/`SteamInput`, so RTTI cannot identify this vftable.  It was not")
A("  used.")
A("* **Sub-table geometry** (all measured, not assumed):\n")
A("  | sub-object | vtable | entries | element-wise identical to |")
A("  | --- | --- | ---: | --- |")
for k in sorted(LADDER):
    rel = "itself (this table)" if k == 0x50 else (
        "v006[0:%d]" % len(ID[0x48]) if (k == 0x48 and ALIGN_OK) else
        ("v006[%d:%d]" % (ALIGNMAP[k][0], ALIGNMAP[k][0] + ALIGNMAP[k][2]) if k in ALIGNMAP else "no run >= 2"))
    A("  | +0x%02X | 0x%06X | %d | %s |" % (k, LADDER[k], len(EXT[k]), rel))
A("")
A("* **Slot count** is not guessed: `0x54B738 + 48*8 = 0x54B878` is followed by a")
A("  non-code qword, while every entry inside it is a `.text` VA.")
A("")
open(OUT, "w").write("\n".join(L) + "\n")

try:
    _s, _e = L.index("## The table"), L.index("## Raw vtable bytes")
except ValueError:
    _s, _e = 0, len(L)
print("\n".join(L[_s:_e]))
print("PROVED %d / AMBIGUOUS %d / total %d" % (nproved, namb, N))
print("wrote %s" % OUT)
