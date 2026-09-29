#!/usr/bin/env python3
"""S39 - FINAL resolver: symbolic tracking + the extra call shapes.

Extra shapes handled relative to s37:
  * `jmp qword ptr [rax+K]`  (tail call)   -> slot K
  * `call qword ptr [rax]`   (slot 0)     -> off 0
  * the vtable load happening BEFORE an
    intervening `call <helper>` that does not touch the tracked regs
  * the interface held in r15/rbx etc.
"""
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import dis, disp_of, pdata

pd = pdata()
byaddr = tdlib.idx()
DISPATCH = 0x009831B0
SEL = 0x00C48408
MAXSLOT = 0x178
R = r"r(?:ax|bx|cx|dx|si|di|bp|sp|[89]|1[0-5])"
MOV_R_MEM = re.compile(r"^(" + R + r"), qword ptr \[(" + R + r")"
                       r"(?: \+ (0x[0-9a-f]+))?\]$")
CALL_MEM = re.compile(r"^qword ptr \[(" + R + r")(?: \+ (0x[0-9a-f]+))?\]$")
CALL_REG = re.compile(r"^(" + R + r")$")
# `call <direct>` / `jmp <direct>` to a known game helper -- these do NOT clobber
# the GPRs we track, so they are skipped rather than treated as invalidating.
HELPER = re.compile(r"^0x140[0-9a-f]{6}$")


def riptgt(p):
    d = disp_of(p)
    return None if d is None else p[0] + p[1] + d


def resolve(ins, i):
    st = {"rax": "P"}
    for p in ins[i + 1:i + 48]:
        mn, op = p[2], p[3]
        if mn in ("ret",):
            break
        if mn in ("call", "jmp"):
            if HELPER.match(op):
                continue                      # direct game call, regs survive
            m = CALL_MEM.match(op)
            if m:
                k = int(m.group(2), 16) if m.group(2) else 0
                if st.get(m.group(1)) == "VT" and k % 8 == 0 and k <= MAXSLOT:
                    return k, p[0], "%s qword ptr [%s%s]" % (
                        mn, m.group(1),
                        (" + 0x%X" % k) if m.group(2) else "")
                break
            m = CALL_REG.match(op)
            if m:
                v = st.get(m.group(1))
                if isinstance(v, int) and v % 8 == 0 and v <= MAXSLOT:
                    return v, p[0], "%s %s" % (mn, m.group(1))
                break
            break                           # unknown call shape -> give up
        m = MOV_R_MEM.match(op)
        if m and mn == "mov":
            dst, src, imm = m.group(1), m.group(2), m.group(3)
            v = st.get(src)
            if imm is None:
                st[dst] = {"P": "D1", "D1": "VT"}.get(v, None)
            else:
                k = int(imm, 16)
                st[dst] = k if (v == "VT" and k % 8 == 0 and k <= MAXSLOT) else None
            continue
        mm = re.match(r"^(" + R + r"), ", op)
        if mm and mn not in ("cmp", "test"):
            st[mm.group(1)] = None
    return None, None, None


allst = []
for r in tdlib.recs():
    if r[2] != "call" or "rip" not in r[3]:
        continue
    d = disp_of(r)
    if d is not None and r[0] + r[1] + d == DISPATCH:
        allst.append(r[0])

si = []
for a in allst:
    f = pd.func(a)
    ins = [p for p in dis(f[0], 8000) if p[0] < f[1]]
    i = next((k for k, p in enumerate(ins) if p[0] == a), None)
    if i is None:
        continue
    if any(p[2] == "lea" and p[3].startswith("rcx,") and "rip" in p[3]
           and riptgt(p) == SEL for p in ins[max(0, i - 8):i]):
        si.append((a, f, ins, i))

print("dispatcher sites %d, SteamInput %d" % (len(allst), len(si)))
res = []
for a, f, ins, i in si:
    off, vc, sh = resolve(ins, i)
    res.append({"disp": a, "vcall": vc, "off": off, "shape": sh,
                "fn0": f[0], "fn1": f[1]})
ok = [r for r in res if r["off"] is not None]
print("resolved %d / %d" % (len(ok), len(res)))
for r in res:
    if r["off"] is None:
        print("   !! 0x%08X fn %08X..%08X" % (r["disp"], r["fn0"], r["fn1"]))
h = {}
for r in ok:
    h.setdefault(r["off"], []).append(r)
print("\n=== histogram ===")
for k in sorted(h):
    v = sorted(x["vcall"] for x in h[k])
    print("  slot %-2d off 0x%03X  %3d site(s)  vcall 0x%08X..0x%08X"
          % (k // 8, k, len(h[k]), v[0], v[-1]))
import pickle
pickle.dump(res, open("/root/.hermes/cache/scratch/sites.pkl", "wb"))
print("\nwrote sites.pkl with %d records" % len(res))
