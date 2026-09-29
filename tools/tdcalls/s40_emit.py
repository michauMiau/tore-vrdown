#!/usr/bin/env python3
"""S40 - emit the complete per-site table with arguments, ready for the report."""
import pickle
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, pdata, s as hexs

pd = pdata()
byaddr = tdlib.idx()
res = pickle.load(open("/root/.hermes/cache/scratch/sites.pkl", "rb"))

res = [r for r in res if r["off"] is not None]
# the two Shutdown sites the tracker could not model -- hand-verified from S38:
#   disp 0x004D9795 / 0x004D97A8 -> rcx=[rax]; rax=[rcx]; call [rax+8]  @0x004D97B4
res.append({"disp": 0x004D97A8, "vcall": 0x004D97B4, "off": 0x008,
            "shape": "call qword ptr [rax + 8]", "fn0": 0x004D9770, "fn1": 0x004D987A})
res.append({"disp": 0x004D9795, "vcall": 0x004D97B4, "off": 0x008,
            "shape": "call qword ptr [rax + 8]", "fn0": 0x004D9770, "fn1": 0x004D987A})
print("carried in from s39: %d, plus 2 hand-resolved Shutdown = %d"
      % (len(res) - 2, len(res)))

ACTION = {6: "interact", 7: "jump", 8: "crouch", 9: "usetool", 10: "grab",
          11: "vehicle_action", 12: "vehicle_raise", 13: "vehicle_lower",
          14: "handbrake", 15: "map", 16: "pause", 17: "scroll_up",
          18: "scroll_down", 19: "tool_group_prev", 20: "tool_group_next",
          21: "lmb", 22: "mmb", 23: "rmb", 28: "extra0", 29: "extra1",
          30: "extra2", 31: "extra3", 32: "extra4", 33: "extra5", 34: "extra6",
          35: "photomode", 36: "zoom", 37: "scoreboard", 38: "menu_left",
          39: "menu_right", 40: "menu_up", 41: "menu_down", 42: "menu_next",
          43: "menu_prev", 44: "menu_accept", 45: "menu_cancel",
          47: "l_stick_x", 48: "l_stick_y", 49: "r_stick_x", 50: "r_stick_y",
          24: "camerax", 25: "cameray", 26: "mousex", 27: "mousey",
          46: "camera_view"}


def argof(r):
    """the rdx argument: a literal string, or a small struct on the stack"""
    f = (r["fn0"], r["fn1"])
    ins = [p for p in dis(f[0], 8000) if p[0] < f[1]]
    i = next((k for k, p in enumerate(ins) if p[0] == r["disp"]), None)
    if i is None:
        return "?"
    for p in ins[i:i + 20]:
        m = re.match(r"^rdx, \[rip \+ 0x([0-9a-f]+)\]$", p[3])
        if m and p[2] == "lea":
            d = disp_of(p)
            t = p[0] + p[1] + d
            nm = cstr(t, 20)
            if nm and all(32 <= ord(c) < 127 for c in nm):
                return "%r @0x%08X" % (nm, t)
            return "&0x%08X" % t
        m = re.match(r"^dword ptr \[rbp - 0x([0-9a-f]+)\], (\w+)$", p[3])
        if m and p[2] == "mov":
            imm = m.group(2)
            v = int(imm, 0) if imm.startswith("0x") else None
            if v is not None:
                return "%d @[rbp-0x%s]" % (v, m.group(1))
            return "%s @[rbp-0x%s]" % (imm, m.group(1))
        m = re.match(r"^dword ptr \[rbp - 0x([0-9a-f]+)\], (0x[0-9a-f]+)$", p[3])
        if m and p[2] == "mov":
            return "%d @[rbp-0x%s]" % (int(m.group(2), 16), m.group(1))
    return "?"


rows = []
for r in sorted(res, key=lambda z: (z["fn0"], z["disp"])):
    rows.append((r, argof(r)))

print("total sites: %d" % len(rows))
print()
for r, a in rows:
    print("0x%08X  0x%08X  0x%03X  %-3d  %-32s  %s"
          % (r["disp"], r["vcall"], r["off"], r["off"] // 8,
             r["shape"], a))
