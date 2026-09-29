#!/usr/bin/env python3
"""S32 - fix the operand regex (p[3] has NO mnemonic) and dump the analog path."""
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, pdata, s as hexs

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
IMM = re.compile(r"^r8b, (0x[0-9a-fA-F]+|[0-9]+)$")

f = pd.func(0x004DFE8E)
ins = [p for p in dis(f[0], 2000) if p[0] < f[1]]
calls = [i for i, p in enumerate(ins)
         if p[2] == "call" and p[3] == "0x1404e0890"]
print("=== DIGITAL read: fn 0x%08X..0x%08X, %d wrapper calls ==="
      % (f[0], f[1], len(calls)))
rows = []
for k, ci in enumerate(calls):
    lo = calls[k - 1] + 1 if k else 0
    idx = at = None
    for j in range(lo, ci):
        m = IMM.match(ins[j][3])
        if m:
            idx, at = int(m.group(1), 0), ins[j][0]
    rows.append((ins[ci][0], idx, at))
for a, i, at in rows:
    print("  call 0x%08X  idx %-4s %-16s (r8b set at 0x%08X)"
          % (a, i, NAMES.get(i, "CALLER-SUPPLIED") if i is not None
             else "CALLER-SUPPLIED", at or 0))
print("  resolved %d/%d" % (sum(1 for r in rows if r[1] is not None), len(rows)))

print("\n=== ANALOG read fn 0x004E016D: full annotated disasm (first 0x60) ===")
f = pd.func(0x004E016D)
for p in dis(f[0], 0x60):
    note = ""
    if "rip" in p[3]:
        d = disp_of(p)
        if d is not None:
            t = p[0] + p[1] + d
            note = "  ; ->0x%08X %r" % (t, cstr(t, 16))
    print("  %08X %-6s %-40s %-22s%s"
          % (p[0], p[2], p[3], hexs(p[0], p[1]), note))
print("\n=== one `call r12` site in context (around 0x004E01E8) ===")
for p in dis(0x004E01C0, 0x60):
    note = ""
    if "rip" in p[3]:
        d = disp_of(p)
        if d is not None:
            t = p[0] + p[1] + d
            note = "  ; ->0x%08X %r" % (t, cstr(t, 16))
    print("  %08X %-6s %-40s %-22s%s"
          % (p[0], p[2], p[3], hexs(p[0], p[1]), note))
print("\n=== helper 0x004DBD70 (the per-read helper) head ===")
f = pd.func(0x004DBD70)
print("fn 0x%08X..0x%08X (%d bytes)" % (f[0], f[1], f[1] - f[0]))
for p in dis(f[0], 0x50):
    note = ""
    if "rip" in p[3]:
        d = disp_of(p)
        if d is not None:
            t = p[0] + p[1] + d
            note = "  ; ->0x%08X %r" % (t, cstr(t, 16))
    print("  %08X %-6s %-40s %-22s%s"
          % (p[0], p[2], p[3], hexs(p[0], p[1]), note))
