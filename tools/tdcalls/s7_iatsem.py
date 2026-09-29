#!/usr/bin/env python3
"""S7 - verify the steam IAT import names from the raw hint-name entries, and
explain the `lea rcx,[slot] ; call [IAT]` idiom seen at 0x4DD30A."""
import struct
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, raw, s, pdata

pd = pdata()
print("=== raw hint/name behind each steam IAT slot ===")
for slot in range(0x009831A8, 0x00983208, 8):
    q = struct.unpack("<Q", raw(slot, 8))[0]
    # FirstThunk is unbound here: the qword IS the hint/name RVA
    try:
        hn = q
        hint = struct.unpack("<H", raw(hn, 2))[0]
        name = cstr(hn + 2, 80)
    except Exception as e:
        hint, name = None, "ERR %s" % e
    print("  slot 0x%08X  hn_rva 0x%08X  hint %-5s %r" % (slot, q, hint, name))

print("\n=== the accessor-pointer table around 0x00C48408 (stride probe) ===")
im = tdlib.img()
for k in range(-4, 10):
    a = 0x00C483F0 + k * 0x18
    q = struct.unpack("<Q", raw(a, 8))[0]
    tag = ""
    if 0x140000000 <= q < 0x142000000:
        r = q - 0x140000000
        tag = " rva 0x%08X" % r
        if pd.is_start(r):
            tag += " FN-START"
    print("  0x%08X : %016X%s" % (a, q, tag))

print("\n=== string constants the table entries might carry ===")
for a in (0x00C483E0, 0x00C483E8, 0x00C48438, 0x00C48440, 0x00C48448):
    try:
        print("  0x%08X : %016X  %r" % (a, struct.unpack("<Q", raw(a, 8))[0],
                                        cstr(a, 24)))
    except Exception as e:
        print("  0x%08X : err %s" % (a, e))
