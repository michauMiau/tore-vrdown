#!/usr/bin/env python3
"""S29 - extract the action-index sequence for the digital read loop, and the
analog read loop, straight from the `mov r8b, imm` / `mov [rbp-0x30], imm`
bytes that precede each call."""
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import dis, pdata

pd = pdata()
byaddr = tdlib.idx()

print("=== DIGITAL read: fn 0x004DFE8E, 37 calls to 0x004E0890 ===")
f = pd.func(0x004DFE8E)
ins = [p for p in dis(f[0], 2000) if p[0] < f[1]]
seq = []
for i, p in enumerate(ins):
    if p[2] == "call" and p[3] == "0x1404e0890":
        idx = None
        for q in ins[max(0, i - 6):i]:
            if q[2] == "mov" and q[3].startswith("r8b, 0x"):
                idx = int(q[3].split(",")[1].strip(), 16)
        seq.append((p[0], idx))
print("  indices: %s" % ", ".join(str(x[1]) for x in seq))
print("  count %d, any None: %s" % (len(seq), any(x[1] is None for x in seq)))
print("  call rvas: %s" % " ".join("%08X" % x[0] for x in seq))

print("\n=== ANALOG read: fn 0x004E016D, 5 GetAnalogActionData sites ===")
f = pd.func(0x004E016D)
ins = [p for p in dis(f[0], 2000) if p[0] < f[1]]
seq2 = []
for i, p in enumerate(ins):
    if p[2] == "call" and p[3].startswith("0x009831B0"):
        idx = None
        for q in ins[max(0, i - 6):i]:
            m = re.match(r"^mov +dword ptr \[rbp - 0x30\], (\w+)$", q[3])
            if m:
                idx = m.group(1)
        seq2.append((p[0], idx))
print("  disp sites: %d" % len(seq2))
for a, i in seq2:
    print("    0x%08X  index %s" % (a, i))

print("\n=== the action-name table at 0x009DEB58, stride 16, 51 entries ===")
import struct
for k in range(51):
    e = 0x009DEB58 + k * 16
    ptr = struct.unpack("<Q", tdlib.raw(e, 8))[0] - 0x140000000
    name = tdlib.cstr(ptr, 20)
    used = (k in [x[1] for x in seq] or k in [2, 3, 4])
    print("   idx %2d  entry 0x%08X  name 0x%08X %-16r %s"
          % (k, e, ptr, name, "<-- read in digital loop" if used else ""))
