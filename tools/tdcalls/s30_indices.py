#!/usr/bin/env python3
"""S30 - correct index extraction.

The unrolled read loop is a software pipeline: the `mov r8b, N` AFTER a call
sets the action index used by the NEXT call.  The first call's r8b comes from
the caller.  So: for each call, take the last `mov r8b, N` that appears after
the PREVIOUS call.
"""
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import dis, pdata, s as hexs

pd = pdata()
byaddr = tdlib.idx()
NAMES = {0: "none", 1: "left", 2: "right", 3: "up", 4: "down", 5: "flashlight",
         6: "interact", 7: "jump", 8: "crouch", 9: "usetool", 10: "grab",
         11: "vehicle_action", 12: "vehicle_raise", 13: "vehicle_lower",
         14: "handbrake", 15: "map", 16: "pause", 17: "scroll_up",
         18: "scroll_down", 19: "tool_group_prev", 20: "tool_group_next",
         21: "lmb", 22: "mmb", 23: "rmb", 24: "camerax", 25: "cameray",
         26: "mousex", 27: "mousey", 28: "extra0", 29: "extra1", 30: "extra2",
         31: "extra3", 32: "extra4", 33: "extra5", 34: "extra6",
         35: "photomode", 36: "zoom", 37: "scoreboard", 38: "menu_left",
         39: "menu_right", 40: "menu_up", 41: "menu_down", 42: "menu_next",
         43: "menu_prev", 44: "menu_accept", 45: "menu_cancel",
         46: "camera_view", 47: "l_stick_x", 48: "l_stick_y",
         49: "r_stick_x", 50: "r_stick_y"}

f = pd.func(0x004DFE8E)
ins = [p for p in dis(f[0], 2000) if p[0] < f[1]]
calls = [i for i, p in enumerate(ins)
         if p[2] == "call" and p[3] == "0x1404e0890"]
print("=== DIGITAL read loop, fn 0x%08X..0x%08X, %d calls ==="
      % (f[0], f[1], len(calls)))
prev = 0
for k, ci in enumerate(calls):
    lo = calls[k - 1] + 1 if k else 0
    idx = None
    at = None
    for j in range(lo, ci):
        m = re.match(r"^mov +r8b, (0x[0-9a-fA-F]+|[0-9]+)$", ins[j][3])
        if m:
            idx = int(m.group(1), 0)
            at = ins[j][0]
    tag = "%-14s" % NAMES.get(idx, "CALLER-SUPPLIED") if idx is not None \
        else "CALLER-SUPPLIED"
    print("  call 0x%08X  <- r8b set at %s = %2s  %s"
          % (ins[ci][0], ("0x%08X" % at) if at else "?", idx, tag))

print("\n=== the caller of 0x004DFE8E (supplies the first r8b) ===")
recs = tdlib.recs()
for r in recs:
    if r[2] == "call" and r[3] == "0x1404dfe8e":
        f2 = pd.func(r[0])
        print("  0x%08X in fn %08X..%08X  raw=%s"
              % (r[0], f2[0], f2[1], hexs(r[0], r[1])))
        for p in dis(f2[0], 40):
            if p[0] > r[0]:
                break
            if p[0] > r[0] - 0x20:
                print("     %08X %-6s %-38s %s"
                      % (p[0], p[2], p[3], hexs(p[0], p[1])))

print("\n=== ANALOG read loop, fn 0x004E016D: index -> dest field ===")
f = pd.func(0x004E016D)
ins = [p for p in dis(f[0], 2000) if p[0] < f[1]]
for i, p in enumerate(ins):
    if p[2] == "call" and p[3].startswith("0x009831B0"):
        idx = None
        dest = []
        slotload = None
        for j in range(i, min(i + 30, len(ins))):
            m = re.match(r"^mov +dword ptr \[rbp - 0x30\], (\w+)$", ins[j][3])
            if m and idx is None:
                idx = m.group(1)
            if re.match(r"^mov +r\d+, qword ptr \[rax \+ 0x[0-9a-f]+\]$", ins[j][3]):
                slotload = ins[j][3]
            if ins[j][2] == "movss" and "dword ptr [rbx +" in ins[j][3]:
                dest.append(ins[j][3].split(",")[0].strip())
        print("  disp 0x%08X  %-28s index=%-4s dest=%s"
              % (p[0], slotload or "?", idx or "?", ", ".join(dest) or "?"))
