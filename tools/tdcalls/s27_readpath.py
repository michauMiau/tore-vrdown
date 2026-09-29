#!/usr/bin/env python3
"""S27 - the read path, verified end to end.

  (1) Who calls 0x004E0890 (the GetDigitalActionData wrapper)?
  (2) The 0x4E016D function: which action index does each GetAnalogActionData
      read, and where do the results land?
  (3) The RunFrame site context: is it in a per-frame update?
"""
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, iat, pdata, raw, s as hexs

IM = tdlib.iat()
pd = pdata()
recs = tdlib.recs()
byaddr = tdlib.idx()

print("=== callers of 0x004E0890 (GetDigitalActionData wrapper) ===")
for r in recs:
    if r[2] != "call" or not r[3].startswith("0x"):
        continue
    try:
        t = int(r[3], 16) - 0x140000000
    except ValueError:
        continue
    if t == 0x004E0890:
        f = pd.func(r[0])
        print("   %08X  in fn %08X..%08X   raw=%s"
              % (r[0], f[0], f[1], hexs(r[0], r[1])))

print("\n=== callers of 0x004E016D and 0x004E047A (GetAnalogActionData) ===")
for want in (0x004E016D, 0x004E047A):
    print("  target 0x%08X:" % want)
    for r in recs:
        if r[2] != "call" or not r[3].startswith("0x"):
            continue
        try:
            t = int(r[3], 16) - 0x140000000
        except ValueError:
            continue
        if t == want:
            f = pd.func(r[0])
            print("     %08X  in fn %08X..%08X" % (r[0], f[0], f[1]))

print("\n=== the analog-read block: index immediates and destination fields ===")
f = pd.func(0x004E016D)
print("fn 0x%08X..0x%08X (%d bytes)" % (f[0], f[1], f[1] - f[0]))
blocks = []
cur = None
for p in dis(f[0], 400):
    if p[0] >= f[1]:
        break
    if p[2] == "call" and p[3].startswith("0x009831B0"):
        cur = {"dispatch": p[0], "imm": None, "imm_at": None, "vcall": None,
               "off": None, "dest": []}
        blocks.append(cur)
    if cur is not None:
        if p[2] == "mov" and p[3].startswith("dword ptr [rbp - 0x30]"):
            mm = re.search(r", (\w+)$", p[3])
            if mm:
                cur["imm"] = mm.group(1)
                cur["imm_at"] = p[0]
        if p[2] == "mov" and re.match(r"^r\d+, qword ptr \[rax \+ 0x[0-9a-f]+\]$", p[3]):
            cur["off"] = p[3]
            cur["off_at"] = p[0]
        if p[2] == "call" and not p[3].startswith("0x") and cur["vcall"] is None \
                and "rip" not in p[3]:
            cur["vcall"] = p[0]
        if p[2] == "movss" and "dword ptr [rbx +" in p[3]:
            cur["dest"].append(p[3].split(",")[0].strip())
for b in blocks:
    print("  disp 0x%08X  index=%-6s (at %08X)  slotload=%-28s (at %s)  vcall=%s  stores: %s"
          % (b["dispatch"], b["imm"], b["imm_at"], b["off"], b.get("off_at"),
             ("0x%08X" % b["vcall"]) if b["vcall"] else "-", ", ".join(b["dest"])))

print("\n=== the digital-read function 0x004E0890 head ===")
f = pd.func(0x004E0890)
print("fn 0x%08X..0x%08X (%d bytes)" % (f[0], f[1], f[1] - f[0]))
print("=== the RunFrame fn 0x004DF859: what encloses the 0x18 call ===")
f = pd.func(0x004DF8F9)
print("fn 0x%08X..0x%08X (%d bytes)" % (f[0], f[1], f[1] - f[0]))
for p in dis(max(f[0], 0x004DF8F2), 12):
    note = ""
    if "rip" in p[3]:
        d = disp_of(p)
        if d is not None:
            t = p[0] + p[1] + d
            note = "  ; ->0x%08X %r" % (t, cstr(t, 22))
    print("  %08X %-6s %-42s %-24s%s" % (p[0], p[2], p[3], hexs(p[0], p[1]), note))
