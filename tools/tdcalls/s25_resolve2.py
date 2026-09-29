#!/usr/bin/env python3
"""S25 - FINAL resolver, covering every call shape MSVC emits.

Shapes handled, each verified against the bytes above:
 (A) call [reg+K]                     after mov rcx,[rax]; mov rax,[rcx]
 (B) mov tmp,[reg+K]; call tmp        (0x4DB8E9: r10 = [rdx+0x90]; call r10)
 (C) jmp  [reg+K]  (tail call)        (0x4DBA2D: jmp [rax+0xB8])
 (D) 3-level deref  mov a,[b+K]; call a   (0x4DDF1A: r9 = [rdx+0x158])

A site is only reported when the receiver chain demonstrably starts at the
SteamInput dispatch.  Everything else is listed as unresolved for hand review.
"""
import collections
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, disp_of, iat, pdata, raw, s as hexs

IM = tdlib.iat()
pd = pdata()
recs = tdlib.recs()
byaddr = tdlib.idx()
DISPATCH = 0x009831B0
SI_SEL = 0x00C48408

VT = {
    0: "Init", 1: "Shutdown", 2: None, 3: "RunFrame", 4: None, 5: None,
    6: "GetConnectedControllers", 7: None, 8: None, 9: "GetActionSetHandle",
    10: "ActivateActionSet", 11: "GetCurrentActionSet", 12: None, 13: None,
    14: None, 15: "GetActiveActionSetLayers", 16: "GetDigitalActionHandle",
    17: "GetDigitalActionData", 18: "GetDigitalActionOrigins", 19: None,
    20: "GetAnalogActionHandle", 21: "GetAnalogActionData",
    22: "GetAnalogActionOrigins", 23: None, 24: "GetGlyphSVGForActionOrigin",
    25: None, 26: "GetStringForActionOrigin", 27: None, 28: None,
    29: "GetMotionData", 30: "TriggerVibration",
    31: "TriggerVibrationExtended", 32: None, 33: "SetLEDColor",
    34: "Legacy_TriggerHapticPulse", 35: "Legacy_TriggerRepeatedHapticPulse",
    36: "ShowBindingPanel", 37: "GetInputTypeForHandle",
    38: "GetControllerForGamepadIndex", 39: "GetGamepadIndexForController",
    40: None, 41: None, 42: None, 43: "TranslateActionOrigin",
    44: "GetDeviceBindingRevision", 45: None,
    46: "GetSessionInputConfigurationSettings", 47: "SetDualSenseTriggerEffect",
}
NVSLOT = 48
MEMOP = re.compile(r"^(r\w+), qword ptr \[(r\w+)((?:\s*\+\s*0x[0-9a-fA-F]+)?)\]$")

sites = []
for r in recs:
    if r[2] != "call" or "rip" not in r[3]:
        continue
    d = disp_of(r)
    if d is None or r[0] + r[1] + d != DISPATCH:
        continue
    a, sel = r[0], None
    for _ in range(14):
        a -= 1
        p = byaddr.get(a)
        if p and p[2] == "lea" and "rip" in p[3] and p[3].startswith("rcx,"):
            dd = disp_of(p)
            if dd is not None:
                sel = p[0] + p[1] + dd
            break
    if sel == SI_SEL:
        sites.append(r[0])


def window(a, n=34):
    win, cur = [], a + 6
    for _ in range(n):
        p = byaddr.get(cur)
        if p is None:
            break
        win.append(p)
        cur += p[1]
    return win


def writes_rax(p):
    if p[2] in ("mov", "movzx", "movsxd", "lea", "pop", "movq"):
        return p[3].split(",")[0].strip() in ("rax", "eax", "ax", "al")
    if p[2] == "xor":
        return p[3].split(",")[0].strip() in ("rax", "eax", "ax", "al")
    if p[2] in ("add", "sub", "and", "or", "imul", "shl", "shr", "sar", "inc",
                "dec", "neg", "not", "bswap", "xchg"):
        return p[3].split(",")[0].strip() in ("rax", "eax", "ax", "al")
    return False


def parse_off(txt):
    m = re.search(r"\+\s*(0x[0-9a-fA-F]+|\d+)\s*\]", txt)
    return int(m.group(1), 0) if m else 0


resolved = {}
for a in sites:
    win = window(a)
    got = None
    # find the vtable pointer: the first `mov rcx,[rax]`-like load of [rax]
    vi = None
    for i, p in enumerate(win):
        if p[2] == "mov" and re.match(r"^r\w+, qword ptr \[rax\]$", p[3]):
            vi = i
            break
    if vi is None:
        resolved[a] = (None, "no [rax] load")
        continue
    if any(writes_rax(p) for p in win[:vi]):
        resolved[a] = (None, "rax clobbered before deref")
        continue
    # walk the chain: rax (iface ptr) -> R (vtable) -> slot
    reg = None          # register holding the vtable
    cur = vi
    for j in range(vi, min(vi + 14, len(win))):
        p = win[j]
        # (A)/(C) direct call/jmp through [reg+off]
        if p[2] in ("call", "jmp") and "qword ptr [" in p[3] \
                and not (p[2] == "call" and "rip" in p[3]):
            m = re.match(r"^qword ptr \[(r\w+)(.*)\]$", p[3])
            if m and m.group(1) in ("rax", "rcx", "r10", "r11", "rdx"):
                got = (parse_off(p[3]), p[0], "direct " + p[2])
                break
        # (B)/(D) mov tmp,[reg+off] then call/jmp tmp
        m = MEMOP.match(p[3]) if p[2] == "mov" else None
        if m:
            tmp, base, rest = m.group(1), m.group(2), m.group(3)
            if not rest:                      # plain mov tmp,[base]  -> 2nd deref
                if j + 1 < len(win):
                    q = win[j + 1]
                    # if next is mov reg,[tmp] -> we have vtable; else slot load
                    m2 = MEMOP.match(q[3]) if q[2] == "mov" else None
                    if m2 and m2.group(2) == tmp and not m2.group(3):
                        reg = m2.group(1)
                        continue
                    # else treat as direct slot load: tmp = [base+off]
                    off = parse_off(p[3])
                    for k in range(j + 1, min(j + 3, len(win))):
                        z = win[k]
                        if z[2] in ("call", "jmp") and z[3] == tmp:
                            got = (off, z[0], "via %s" % tmp)
                            break
                        if z[2] in ("call", "jmp"):
                            break
                    if got:
                        break
            else:                              # mov tmp,[base+off] = slot load
                off = parse_off(p[3])
                for k in range(j + 1, min(j + 3, len(win))):
                    z = win[k]
                    if z[2] in ("call", "jmp") and z[3] == tmp:
                        got = (off, z[0], "via %s" % tmp)
                        break
                    if z[2] in ("call", "jmp"):
                        break
                if got:
                    break
    resolved[a] = got if got else (None, "chain not recognised")

agg = collections.Counter()
for a in sites:
    g = resolved[a]
    if g and g[0] is not None:
        agg[g[0]] += 1

print("=== %d SteamInput dispatch sites ===" % len(sites))
print("\n=== vtable offsets called on ISteamInput ===")
for off, c in sorted(agg.items()):
    sl = off // 8
    nm = VT.get(sl) if off % 8 == 0 and sl < NVSLOT else None
    warn = "  *** OUTSIDE 48-SLOT VTABLE ***" if off >= NVSLOT * 8 else \
           ("  *** not 8-byte aligned ***" if off % 8 else "")
    print("  0x%03X  slot %3d  %3d sites  %-34s%s"
          % (off, sl, c, nm or "UNMAPPED", warn))
print("  unresolved: %d" % sum(1 for a in sites if not resolved[a]))

print("\n=== per-site (non-0x80 rows collapsed) ===")
for a in sorted(sites):
    g = resolved[a]
    f = pd.func(a)
    if g and g[0] is not None:
        sl = g[0] // 8
        nm = VT.get(sl) if g[0] % 8 == 0 and sl < NVSLOT else "???"
        if g[0] == 0x80:
            continue
        print("  disp 0x%08X -> vcall 0x%08X  off 0x%03X  slot %3d  %-32s [%s]  fn %08X..%08X"
              % (a, g[1], g[0], sl, nm, g[2], f[0], f[1]))
    else:
        print("  disp 0x%08X -> UNRESOLVED (%s)  fn %08X..%08X"
              % (a, g[1] if g else "?", f[0], f[1]))

z = [a for a in sites if resolved[a] and resolved[a][0] == 0x80]
print("\n  (+ %d sites at off 0x080 GetDigitalActionHandle, first %s last %s)"
      % (len(z), "%08X" % min(z), "%08X" % max(z)))
