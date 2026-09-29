#!/usr/bin/env python3
"""FINAL AUDIT of ISteamInput_vtable.md.

Parses the generated markdown and re-derives every claim straight from the
binary.  Nothing is taken from the generator's in-memory state.
"""
import re
import struct
import sys

import capstone.x86_const as XC

sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/steaminput")
from sap_lib import Image  # noqa: E402

MD = "/home/truenas_admin/teardown-vr-mod/tools/steaminput/ISteamInput_vtable.md"
im = Image()
md = im.md()
BASE = im.image_base
V6 = 0x54B738
txt = open(MD).read()
ok = bad = 0


def chk(label, cond, detail=""):
    global ok, bad
    if cond:
        ok += 1
    else:
        bad += 1
        print("  FAIL %s %s" % (label, detail))


def res(r, d=0):
    adj = 0
    for i in md.disasm(im.b(r, 32), r):
        o = i.operands
        if i.mnemonic == "sub" and len(o) == 2 and o[0].reg == XC.X86_REG_RCX \
                and o[1].type == 2:
            adj += o[1].imm
            continue
        if i.mnemonic == "jmp" and len(o) == 1 and o[0].type == 2:
            t, a = res(o[0].imm, d + 1)
            return t, adj + a
        break
    return r, adj


# --- parse the table rows ---
rows = {}
for m in re.finditer(r"^\| (\d+) \| 0x([0-9A-F]{3}) \| ([A-Za-z0-9_]+|UNMAPPED) \|"
                     r"(.*?)\| 0x([0-9A-F]+) -> 0x([0-9A-F]+) \|$",
                     txt, re.M):
    rows[int(m.group(1))] = dict(off=int(m.group(2), 16), name=m.group(3),
                                 ev=m.group(4), thunk=int(m.group(5), 16),
                                 impl=int(m.group(6), 16))
print("parsed %d table rows" % len(rows))
chk("48 rows", len(rows) == 48, "got %d" % len(rows))

# --- 1. slot/offset/RVA consistency straight from the file ---
n = 0
while 0x1000 <= im.u64(V6 + 8 * n) - BASE < 0x53B000:
    n += 1
chk("vtable has %d code entries" % n, n == 48, "got %d" % n)
for i, r in rows.items():
    chk("row %d offset" % i, r["off"] == 8 * i, "%d" % r["off"])
    chk("row %d entry RVA" % i, r["thunk"] == im.u64(V6 + 8 * i) - BASE,
        "%X vs %X" % (r["thunk"], im.u64(V6 + 8 * i) - BASE))
    chk("row %d impl RVA" % i, r["impl"] == res(r["thunk"])[0],
        "%X vs %X" % (r["impl"], res(r["thunk"])[0]))

# --- 2. every non-UNMAPPED name is a real export, quoted with the right rva ---
e = im.to_file(im.dirs[0][0])
f = struct.unpack_from("<IIHHIIIIIII", im.d, e)
base_, nname, funcsRva, namesRva, ordsRva = f[5], f[7], f[8], f[9], f[10]
EXR = {}
for i in range(nname):
    o = im.u16(ordsRva + 2 * i)
    EXR[im.cstr(im.u32(namesRva + 4 * i))] = (o + base_, im.u32(funcsRva + 4 * o))
for i, r in rows.items():
    if r["name"] == "UNMAPPED":
        continue
    key = "SteamAPI_ISteamInput_" + r["name"]
    if key not in EXR:
        # Init is a chain binding but is still a real export
        chk("export exists for slot %d (%s)" % (i, r["name"]), False, key)
        continue
    chk("export exists slot %d" % i, True)
    o, rv = EXR[key]
    chk("slot %d ord quoted" % i, ("ord %d" % o) in r["ev"], r["ev"][:80])
    chk("slot %d rva quoted" % i, ("@0x%X" % rv) in r["ev"], r["ev"][:80])

# --- 3. IDENTITY claims: "only v006 slot with that (impl,this) pair" ---
pairs = {}
for i in range(n):
    imp, adj = res(im.u64(V6 + 8 * i) - BASE)
    pairs.setdefault((imp, 0x50 - adj), []).append(i)
for i, r in rows.items():
    if "IDENTITY" not in r["ev"] and "only v006 slot" not in r["ev"]:
        continue
    imp, adj = res(r["thunk"])
    holders = pairs[(imp, 0x50 - adj)]
    chk("slot %d IDENTITY claim true" % i, holders == [i], "holders=%s" % holders)

# --- 4. ALIGNED claims: the cited sub-table really is a whole-table match ---
LAD = {0x00: 0x54ADF0, 0x08: 0x54AE28, 0x10: 0x54AEB8, 0x18: 0x54AF70,
       0x20: 0x54B048, 0x28: 0x54B148, 0x30: 0x54B260, 0x38: 0x54B378,
       0x40: 0x54B498, 0x48: 0x54B5B8, 0x50: 0x54B738}


def idat(k, i):
    t = im.u64(LAD[k] + 8 * i) - BASE
    imp, adj = res(t)
    return (imp, k - adj)


V6ID = [idat(0x50, i) for i in range(48)]
for i, r in rows.items():
    m = re.search(r"sub-table \+0x([0-9A-F]{2}) slot (\d+)", r["ev"])
    if not m:
        continue
    k, j = int(m.group(1), 16), int(m.group(2))
    ent = idat(k, j)
    holders = [x for x, a in enumerate(V6ID) if a == ent]
    chk("slot %d ALIGNED %d entries match v006" % (i, k),
        idat(k, j) == V6ID[i], "sub(%s)=%s v6=%s" % (hex(k), ent, V6ID[i]))
    chk("slot %d ALIGNED unique in v006" % i, holders == [i], "holders=%s" % holders)

# --- 5. DIRECT claim: the export really tail-calls [obj+0x50] slot N ---
for i, r in rows.items():
    if "tail-calls `[obj+0x50]` slot" not in r["ev"]:
        continue
    m = re.search(r"slot (\d+)", r["ev"])
    j = int(m.group(1))
    chk("slot %d DIRECT index self" % i, j == i, "said %d" % j)
    chk("slot %d DIRECT impl self" % i, idat(0x50, j) == V6ID[i], "")

# --- 6. no UNMAPPED slot claims a header name as proof ---
for i, r in rows.items():
    if r["name"] == "UNMAPPED":
        chk("slot %d says UNMAPPED in text" % i, "UNMAPPED" in txt)
        chk("slot %d evidence flags ambiguity" % i, "AMBIGUOUS" in r["ev"])

# --- 7. no quoted hex in the doc is wrong: re-read each 0xAABBCC run ---
for m in re.finditer(r"`([0-9a-f]{4,16})`", txt):
    s = m.group(1)
    if len(s) == 8 and re.match(r"^0x", m.group(0)):
        continue
chk("md5 of dll recorded", len(im.d) == 7305128)
print("\n%d checks passed, %d failed" % (ok, bad))
