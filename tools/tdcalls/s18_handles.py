#!/usr/bin/env python3
"""S18 - Where does the game STORE the ISteamInput* and the action handles?

The 36 GetDigitalActionHandle calls at 0x4DD2F0-0x4DDD70 are unrolled, one per
action.  Watch what happens to each returned handle:
    call [rax+0x80]        ; GetDigitalActionHandle(handle*)  -> EAX/RAX
    lea  r8,  [rbp + 0x10]
    mov  byte [rbp-0x10], <action index>
    lea  rdx, [rbp - 0x10]
    mov  rbx, rax
    lea  rcx, [rsi + 0x58]
    call 0x1404db1f0       ; store into the object at rsi+0x58
So handles are cached in a container owned by [rsi+0x58].  Then find the
function that READS them and calls GetDigitalActionData.
"""
import collections
import re
import struct
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, iat, pdata, raw, riprefs, s

IM = tdlib.iat()
pd = pdata()
recs = tdlib.recs()
byaddr = tdlib.idx()

print("=== the per-action block, with the handle-store call 0x1404DB1F0 ===")
f = pd.func(0x004DD2F0)
print("function 0x%08X..0x%08X (%d bytes), 45 refs to the accessor slot"
      % (f[0], f[1], f[1] - f[0]))
for p in dis(0x004DD2F0, 60):
    note = ""
    if "rip" in p[3]:
        t = p[0] + p[1] + disp_of(p)
        c = cstr(t, 20)
        note = "  ; ->0x%08X %r" % (t, c) if c.isprintable() and len(c) > 1 \
            else "  ; ->0x%08X" % t
    if p[2] == "call" and p[3].startswith("0x"):
        note += "   [direct]"
    print("  %08X %-6s %-38s %-22s%s" % (p[0], p[2], p[3], s(p[0], p[1]), note))

print("\n=== who calls 0x1404DB1F0 (the handle-store helper)? ===")
tgt = 0x004DB1F0
n = 0
for r in recs:
    if r[2] == "call" and r[3].startswith("0x"):
        a = r[3]
        try:
            t = int(a, 16) - 0x140000000
        except ValueError:
            continue
        if t == tgt:
            n += 1
            ff = pd.func(r[0])
            print("   %08X  in fn %s" % (r[0],
                  ("%08X..%08X" % (ff[0], ff[1])) if ff else "?"))
print("   total %d call sites" % n)

print("\n=== disassembly of 0x004DB1F0 ===")
ff = pd.func(0x004DB1F0)
print("fn 0x%08X..0x%08X (%d bytes)" % (ff[0], ff[1], ff[1] - ff[0]))
for p in dis(ff[0], 60):
    if p[0] >= ff[1]:
        break
    note = ""
    if "rip" in p[3]:
        t = p[0] + p[1] + disp_of(p)
        note = "  ; ->0x%08X" % t
    print("  %08X %-6s %-40s %-22s%s" % (p[0], p[2], p[3], s(p[0], p[1]), note))
