#!/usr/bin/env python3
"""S31 - final data for the report.  Fixes the imm8/imm32 regex bug."""
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
IMM = re.compile(r"^mov +r8b, (0x[0-9a-fA-F]+|[0-9]+)$")

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
    print("  0x%08X  r8b<-0x%08X  idx %-4s %s"
          % (a, at or 0, i, NAMES.get(i, "CALLER-SUPPLIED") if i is not None
             else "CALLER-SUPPLIED"))
print("  resolved: %d/%d" % (sum(1 for r in rows if r[1] is not None), len(rows)))

print("\n=== who calls 0x004DFE8E / 0x004E016D ===")
recs = tdlib.recs()
for want, tag in ((0x004DFE8E, "digital read"), (0x004E016D, "analog read"),
                  (0x004E0890, "GetDigitalActionData wrapper"),
                  (0x004E047A, "GetAnalogActionData?")):
    hits = []
    for r in recs:
        if r[2] != "call":
            continue
        for pref in ("0x140", "0x14000", "0x"):
            pass
        t = r[3]
        try:
            v = int(t, 16)
        except (ValueError, TypeError):
            continue
        if v - 0x140000000 == want:
            hits.append(r[0])
    print("  0x%08X (%s): %d caller(s) %s"
          % (want, tag, len(hits), " ".join("%08X" % h for h in hits[:8])))

print("\n=== ANALOG read sites: any dispatch in 0x004E016D ===")
f = pd.func(0x004E016D)
print("fn 0x%08X..0x%08X (%d bytes)" % (f[0], f[1], f[1] - f[0]))
ins = [p for p in dis(f[0], 3000) if p[0] < f[1]]
print("  decoded %d insns" % len(ins))
for p in ins:
    if p[2] == "call" and not p[3].startswith("0x140") and "rip" not in p[3]:
        print("   0x%08X  call %-22s %s" % (p[0], p[3], hexs(p[0], p[1])))
    if p[2] == "call" and p[3].startswith("0x140"):
        print("   0x%08X  call %-22s %s" % (p[0], p[3], hexs(p[0], p[1])))
