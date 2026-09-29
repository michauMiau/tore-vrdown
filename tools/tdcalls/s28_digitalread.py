#!/usr/bin/env python3
"""S28 - the digital-read function 0x004DFE8E: what action does each of the 38
GetDigitalActionData calls read, and where does the result land?"""
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, iat, pdata, s as hexs

pd = pdata()
byaddr = tdlib.idx()
F = 0x004DFE8E
f = pd.func(F)
print("fn 0x%08X..0x%08X  (%d bytes)" % (f[0], f[1], f[1] - f[0]))

rows = []
ins = dis(f[0], 1200)
for i, p in enumerate(ins):
    if p[0] >= f[1]:
        break
    if p[2] == "call" and p[3] == "0x1404e0890":
        # inspect the 8 preceding instructions
        ctx = ins[max(0, i - 9):i]
        arg = None
        store = None
        for q in ctx:
            if q[2] == "mov" and re.match(r"^d?x?[a-z0-9]+, (?:dword|qword) ptr \[rsp \+ ", q[3]):
                mm = re.search(r"^(\w+), ", q[3])
                if mm and q[3].count("]") == 1:
                    arg = mm.group(1)
            if q[2].startswith("mov") and "+" in q[3] and "rsp" not in q[3] \
                    and q[3].split(",")[1].strip() in ("eax", "ebx", "ecx", "edx", "esi", "edi"):
                store = q[3]
        rows.append((p[0], arg, store))
print("\n%d call sites of the GetDigitalActionData wrapper:" % len(rows))
for a, arg, store in rows:
    print("  %08X  arg-reg %-6s  result-> %s" % (a, arg or "?", store or "?"))

print("\n=== full disassembly of the first 3 blocks ===")
cnt = 0
for p in dis(f[0], 1200):
    if p[0] >= 0x004DFF30:
        break
    note = ""
    if "rip" in p[3]:
        d = disp_of(p)
        if d is not None:
            t = p[0] + p[1] + d
            note = "  ; ->0x%08X %r" % (t, cstr(t, 22))
    print("  %08X %-6s %-42s %-24s%s" % (p[0], p[2], p[3], hexs(p[0], p[1]), note))
