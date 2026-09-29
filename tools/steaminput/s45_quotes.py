#!/usr/bin/env python3
"""Emit EXACT, re-read bytes for every quotation used in ISteamInput_vtable.md.
Nothing is written from memory: each line is disassembled from the file."""
import struct
import sys

import capstone.x86_const as XC

sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/steaminput")
from sap_lib import Image  # noqa: E402

im = Image()
md = im.md()
BASE = im.image_base
V6 = 0x54B738

print("RCX const =", hex(XC.X86_REG_RCX))


def show(rva, n, label):
    fs = im.fn_start(rva)
    fe = im.fn_end(rva)
    print("\n--- %s : rva 0x%X  (.pdata 0x%X..0x%X)" % (label, rva, fs or 0, fe or 0))
    for i in md.disasm(im.b(rva, n), rva):
        print("  0x%X  %-18s  %-6s %s" % (i.address, i.bytes.hex(),
                                         i.mnemonic, i.op_str))


# 1. the exported v006 factory
show(0x335C0, 0x40, "SteamAPI_SteamInput_v006")

# 2. the version resolver arm for SteamInput006
show(0xAF1F0, 0x30, "GetISteamInputByVersion near the SteamInput006 arm")

# 3. the CSteamInput ctor store of the v006 vtable
show(0xB2C60, 0x30, "CSteamInput ctor: store 0x54B738 at obj+0x50")

# 4. vtable entry for slot 0
t0 = im.u64(V6) - BASE
show(t0, 12, "v006 slot 0 entry (0x%X)" % t0)

# 5. a real flat-export thunk, quoted verbatim
show(0x22E90, 0x40, "SteamAPI_ISteamInput_ActivateActionSetLayer")

# 6. the Init leaf thunk
show(0xB7E60, 12, "leaf thunk reached by exported Init")

# 7. exact resolution arithmetic
for (rva, n, what) in ((0x335F1, 7, "factory lea r9"), (0xB2C76, 7, "ctor lea rax")):
    b = im.b(rva, n)
    disp = struct.unpack_from("<i", b, 3)[0]
    print("\n%s @0x%X bytes=%s disp=0x%X -> target 0x%X (%s)"
          % (what, rva, b.hex(), disp, rva + n + disp, im.cstr(rva + n + disp)
             if 0x53B000 <= rva + n + disp < 0x654000 else "?"))

# 8. v005 vs v006 element-wise identity, with the RIGHT rcx constant
def res(r, d=0):
    adj = 0
    for i in md.disasm(im.b(r, 32), r):
        o = i.operands
        if i.mnemonic == "sub" and len(o) == 2 and o[0].reg == XC.X86_REG_RCX \
                and o[1].type == 2:
            adj += o[1].imm
            continue
        if i.mnemonic == "jmp" and len(o) == 1 and o[0].type == 2:
            t2, a2 = res(o[0].imm, d + 1)
            return t2, adj + a2
        break
    return r, adj


V5 = 0x54B5B8
same, diff = 0, []
for i in range(47):
    a5 = res(im.u64(V5 + 8 * i) - BASE)
    a6 = res(im.u64(V6 + 8 * i) - BASE)
    if a5 == a6:
        same += 1
    else:
        diff.append((i, "%08X/%02X" % (a5[0], 0x48 - a5[1]),
                     "%08X/%02X" % (a6[0], 0x50 - a6[1])))
print("\nv005[0..46] vs v006[0..46] identical (impl, adj): %d/47  diffs=%s"
      % (same, diff))
print("v005 entries:", sum(1 for i in range(90)
                           if 0x1000 <= im.u64(V5 + 8 * i) - BASE < 0x53B000))
print("v006 entries:", sum(1 for i in range(90)
                           if 0x1000 <= im.u64(V6 + 8 * i) - BASE < 0x53B000))
