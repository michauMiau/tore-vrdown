#!/usr/bin/env python3
"""S35 - final: hand-resolve the 6, enumerate the 37 handle sites.

Operand strings here come from capstone and look like:
    'rcx, [rip + 0x76823d]'
    'call qword ptr [rip + 0x4a2fdf]'
so a SteamInput dispatch is a `call` whose rip target resolves to 0x009831B0,
and the selector is a preceding `lea rcx, [rip + ..]` -> 0x00C48408.
"""
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, pdata, s as hexs

pd = pdata()
byaddr = tdlib.idx()
DISPATCH = 0x009831B0
SEL = 0x00C48408


def riptgt(p):
    d = disp_of(p)
    return None if d is None else p[0] + p[1] + d


def is_dispatch(p):
    return (p[2] == "call" and "rip" in p[3] and riptgt(p) == DISPATCH)


def is_selector(p):
    return (p[2] == "lea" and p[3].startswith("rcx,")
            and "rip" in p[3] and riptgt(p) == SEL)


UNRES = [0x004E01CB, 0x004E0280, 0x004E0321, 0x004E03C8, 0x004E048A, 0x004E0B9E]
print("=== the 6 hand-resolved sites ===")
for d in UNRES:
    f = pd.func(d)
    ins = [p for p in dis(f[0], 3000) if p[0] < f[1]]
    i = next(k for k, p in enumerate(ins) if p[0] == d)
    slot = callat = None
    for j in range(i, min(i + 34, len(ins))):
        m = re.match(r"^mov +(r1[0-5]), qword ptr \[(r1[0-5]) \+ (0x[0-9a-f]+)\]$",
                     ins[j][3])
        if m and slot is None:
            slot = (m.group(1), int(m.group(3), 0), ins[j][0], m.group(2))
        m2 = re.match(r"^call (r1[0-5])$", ins[j][3])
        if m2 and slot and m2.group(1) == slot[0]:
            callat = ins[j][0]
            break
    if slot:
        print("  disp 0x%08X -> vcall 0x%08X  off 0x%03X slot %-2d  "
              "[%s <- [%s+0x%03x] @0x%08X]  fn %08X..%08X"
              % (d, callat or 0, slot[1], slot[1] // 8, slot[0], slot[3],
                 slot[1], slot[2], f[0], f[1]))
    else:
        print("  disp 0x%08X -> NOT FOUND   fn %08X..%08X" % (d, f[0], f[1]))

print("\n=== the handle-creation function 0x004DD2F0: every dispatch site ===")
f = pd.func(0x004DD311)
ins = [p for p in dis(f[0], 4000) if p[0] < f[1]]
dh, ah, other = [], [], []
for i, p in enumerate(ins):
    if not is_dispatch(p):
        continue
    if not any(is_selector(q) for q in ins[max(0, i - 6):i]):
        continue
    slot = None
    for j in range(i, min(i + 14, len(ins))):
        m = re.match(r"^mov +(?:rcx|rdx|edx|rax), qword ptr \[rax \+ (0x[0-9a-f]+)\]$",
                     ins[j][3])
        if m:
            slot = int(m.group(1), 0)
            break
    (dh if slot == 0x80 else ah if slot == 0xA0 else other).append((p[0], slot))
print("  GetDigitalActionHandle : %d" % len(dh))
print("  GetAnalogActionHandle  : %d" % len(ah))
print("  other slots            : %s" % other)
print("  digital RVAs:")
for k in range(0, len(dh), 8):
    print("    " + " ".join("0x%08X" % x[0] for x in dh[k:k + 8]))
print("  analog RVAs:")
for a in ah:
    print("    0x%08X" % a[0])

print("\n=== one handle site in full context (0x004DD311) ===")
for p in dis(0x004DD2F0, 0x40):
    note = ""
    if "rip" in p[3]:
        t = riptgt(p)
        if t is not None:
            note = "  ; ->0x%08X %r" % (t, cstr(t, 16))
    print("  %08X %-6s %-38s %-20s%s"
          % (p[0], p[2], p[3], hexs(p[0], p[1]), note))
