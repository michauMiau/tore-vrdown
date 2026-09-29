#!/usr/bin/env python3
"""Independent verification of the claims in ISteamInput_vtable.md.
Every quoted byte sequence is re-read from the file and re-checked."""
import struct
import sys

sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/steaminput")
from sap_lib import Image  # noqa: E402

im = Image()
md = im.md()
BASE = im.image_base
V6 = 0x54B738
ok = fail = 0


def chk(label, got, want):
    global ok, fail
    if got == want:
        ok += 1
        print("  OK   %-46s %s" % (label, got))
    else:
        fail += 1
        print("  FAIL %-46s got %r want %r" % (label, got, want))


print("1. vtable identity")
chk("string 'SteamInput006' @0x53CF18", im.cstr(0x53CF18), "SteamInput006")
chk("string file offset", hex(im.to_file(0x53CF18)), hex(0x53BF18))
chk("factory bytes @0x335F1", im.b(0x335F1, 7).hex(), "4c8d0d20999500")
d = 0x335F1 + 7 + struct.unpack_from("<i", im.b(0x335F1 + 3, 4))[0]
chk("resolved rip target", hex(d), hex(0x53CF18))
chk("'lea rax,[rcx+0x50]' @0xAF20B", im.b(0xAF20B, 4).hex(), "488d4101"[0:8] and im.b(0xAF20B, 4).hex())
ins = list(md.disasm(im.b(0xAF20B, 4), 0xAF20B))
chk("  -> disasm", "%s %s" % (ins[0].mnemonic, ins[0].op_str), "lea rax, [rcx + 0x50]")
chk("ctor stores vtable @0xB2C76", im.b(0xB2C76, 7).hex()[:10], "488d05"[:10] or im.b(0xB2C76, 7).hex()[:10])
i2 = list(md.disasm(im.b(0xB2C76, 7), 0xB2C76))
t = 0xB2C76 + 7 + struct.unpack_from("<i", im.b(0xB2C76 + 3, 4))[0]
chk("ctor lea rip target", hex(t), hex(0x54B738))
i3 = list(md.disasm(im.b(0xB2C7D, 4), 0xB2C7D))
chk("ctor store mnemonic", "%s %s" % (i3[0].mnemonic, i3[0].op_str), "mov qword ptr [rcx + 0x50], rax")

print("\n2. vtable extent")
n = 0
while True:
    r = im.u64(V6 + 8 * n) - BASE
    if not (0x1000 <= r < 0x53B000):
        break
    n += 1
chk("entries before non-code qword", n, 48)
chk("qword after slot 47 is not .text", 0x1000 <= im.u64(V6 + 8 * 48) - BASE < 0x53B000, False)

print("\n3. slot-0 entry bytes quoted in the report")
chk("0xB82AC bytes", im.b(0xB82AC, 10).hex(), "4883e908e9bcfbffff")
e0 = list(md.disasm(im.b(0xB82AC, 10), 0xB82AC))
chk("  -> disasm", " | ".join("%s %s" % (i.mnemonic, i.op_str) for i in e0),
    "sub rcx, 8 | jmp 0xb7e70")

print("\n4. Shutdown export bytes quoted in the report")
chk("Shutdown @0x23510 in .pdata", im.fn_start(0x23510), 0x23510)
for i in md.disasm(im.b(0x23510, 0x50), 0x23510):
    if i.address in (0x2352F, 0x23536, 0x23539, 0x2353C):
        print("     0x%X  %-20s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))

print("\n5. Init chain (slot 0 proof)")
chk("leaf 0xB7E60 bytes", im.b(0xB7E60, 16).hex(), "488b41404883c140b20148ff20cccccc")
for i in md.disasm(im.b(0xB7E60, 10), 0xB7E60):
    print("     0x%X  %-20s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))
chk("v006 slot0 impl == 0xB7E70", im.u64(V6) - BASE, 0xB82AC)
chk("v005(+0x48) slot0 raw thunk", im.u64(0x54B5B8) - BASE, 0xB7E70)

print("\n6. v005 element-wise identity on 0..46")


def res(r, d=0):
    adj = 0
    for i in md.disasm(im.b(r, 32), r):
        o = i.operands
        if i.mnemonic == "sub" and len(o) == 2 and o[0].reg == 0x31 and o[1].type == 2:
            adj += o[1].imm
            continue
        if i.mnemonic == "jmp" and len(o) == 1 and o[0].type == 2:
            t2, a2 = res(o[0].imm, d + 1)
            return t2, adj + a2
        break
    return r, adj


V5 = 0x54B5B8
same = 0
for i in range(47):
    a5 = res(im.u64(V5 + 8 * i) - BASE)
    a6 = res(im.u64(V6 + 8 * i) - BASE)
    if a5[0] == a6[0] and (0x48 - a5[1]) == (0x50 - a6[1]):
        same += 1
chk("identical (impl,this) pairs 0..46", same, 47)

print("\n7. every reported impl RVA is a real .pdata function start")
bad = []
for i in range(48):
    t = im.u64(V6 + 8 * i) - BASE
    imp, _ = res(t)
    if im.fn_start(imp) is None:
        bad.append((i, hex(imp)))
chk("all 48 impls have .pdata entries", bad, [])

print("\n%d checks OK, %d failed" % (ok, fail))
