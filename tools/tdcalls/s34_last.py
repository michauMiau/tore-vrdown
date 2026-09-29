#!/usr/bin/env python3
"""S34 - (a) resolve the 6 chains the resolver did not model (r15/rbx as the
base of the vtable, then `call r12`), and (b) enumerate all 37
GetDigitalActionHandle sites with the action name each one asks for."""
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, pdata, s as hexs

pd = pdata()
byaddr = tdlib.idx()
NAMES = ["none", "left", "right", "up", "down", "flashlight", "interact",
         "jump", "crouch", "usetool", "grab", "vehicle_action",
         "vehicle_raise", "vehicle_lower", "handbrake", "map", "pause",
         "scroll_up", "scroll_down", "tool_group_prev", "tool_group_next",
         "lmb", "mmb", "rmb", "camerax", "cameray", "mousex", "mousey",
         "extra0", "extra1", "extra2", "extra3", "extra4", "extra5", "extra6",
         "photomode", "zoom", "scoreboard", "menu_left", "menu_right",
         "menu_up", "menu_down", "menu_next", "menu_prev", "menu_accept",
         "menu_cancel", "camera_view", "l_stick_x", "l_stick_y",
         "r_stick_x", "r_stick_y"]

UNRES = [0x004E01CB, 0x004E0280, 0x004E0321, 0x004E03C8, 0x004E048A, 0x004E0B9E]
print("=== the 6 hand-resolved sites: vtable load + call reg ===")
for d in UNRES:
    f = pd.func(d)
    ins = [p for p in dis(f[0], 3000) if p[0] < f[1]]
    i = next(k for k, p in enumerate(ins) if p[0] == d)
    slot = callat = None
    for j in range(i, min(i + 34, len(ins))):
        m = re.match(r"^mov +(r\d+), qword ptr \[(r\d+|r1[0-5]) \+ (0x[0-9a-fA-F]+)\]$",
                     ins[j][3])
        if m:
            slot = (m.group(1), int(m.group(3), 0), ins[j][0], m.group(2))
        m2 = re.match(r"^call (r1[0-5])$", ins[j][3])
        if m2 and slot and m2.group(1) == slot[0]:
            callat = ins[j][0]
            break
    if slot:
        print("  disp 0x%08X -> vcall 0x%08X  off 0x%03X slot %-2d  (load at "
              "0x%08X from %s, call reg %s)  fn %08X..%08X"
              % (d, callat or 0, slot[1], slot[1] // 8, slot[2], slot[3],
                 slot[0], f[0], f[1]))
    else:
        print("  disp 0x%08X -> NOT FOUND by this pass  fn %08X..%08X"
              % (d, f[0], f[1]))

print("\n=== all GetDigitalActionHandle (off 0x080) sites in fn 0x004DD2F0 ===")
f = pd.func(0x004DD311)
ins = [p for p in dis(f[0], 4000) if p[0] < f[1]]
dh, ah = [], []
for i, p in enumerate(ins):
    if p[2] != "call" or p[3] != "0x009831b0":
        continue
    slot = None
    for j in range(i, min(i + 12, len(ins))):
        m = re.match(r"^mov +(?:rcx|edx|rdx), qword ptr \[rax \+ (0x[0-9a-fA-F]+)\]$",
                     ins[j][3])
        if m:
            slot = int(m.group(1), 0)
            break
    if slot == 0x80:
        dh.append(p[0])
    elif slot == 0xA0:
        ah.append(p[0])
print("  %d GetDigitalActionHandle, %d GetAnalogActionHandle" % (len(dh), len(ah)))
print("  digital:", " ".join("%08X" % x for x in dh))
print("  analog :", " ".join("%08X" % x for x in ah))

print("\n=== the name argument (rdx) for each GetDigitalActionHandle site ===")
print("   -- these pass a pointer to a literal action name, or a table entry --")
for k, d in enumerate(dh):
    i = next(ix for ix, p in enumerate(ins) if p[0] == d)
    arg = None
    for j in range(max(0, i - 8), i + 12):
        m = re.match(r"^lea +(r\d+), \[rip \+ (0x[0-9a-fA-F]+)\]$", ins[j][3])
        if m and m.group(1) in ("rdx", "r8"):
            dsp = disp_of(ins[j])
            if dsp is not None:
                t = ins[j][0] + ins[j][1] + dsp
                arg = (m.group(1), t, cstr(t, 18))
    if k < 8 or k >= len(dh) - 4:
        print("   0x%08X  %s" % (d, ("%s ->0x%08X %r" % arg) if arg else "?"))
print("   ... (%d sites total; first 4 and last 4 shown)" % len(dh))
