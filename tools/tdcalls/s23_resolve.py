#!/usr/bin/env python3
"""S23 - DEFINITIVE ISteamInput vtable-call resolver.

Handles all three shapes MSVC emits for a virtual call through a Steam-issued
interface pointer:

  (A) direct      mov rcx,[rax] ; mov rax,[rcx] ; call [rax+K]
  (B) 2nd deref   mov rcx,[rax] ; mov rax,[rcx] ; mov r10,[rax+K] ; call r10
      (or [rdx+K] when rax was already the vtable)
  (C) tail call   mov rcx,[rax] ; mov rax,[rcx] ; jmp  [rax+K]

For each of the 68 SteamInput dispatch sites, resolve the slot offset K.
Any site that resolves nothing is reported with its window so it can be
checked by hand rather than silently dropped.
"""
import collections
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, iat, pdata, raw, s as hexs

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
REGS = ("rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15")
IDXOF = re.compile(r"^(%s)$" % "|".join(REGS))
OFFPAT = re.compile(r"\+\s*(0x[0-9a-fA-F]+|\d+)\s*\]")

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

print("SteamInput dispatch sites: %d\n" % len(sites))


def window(a, n=30):
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
    if p[2] in ("add", "sub", "and", "or", "imul", "shl", "shr", "sar",
                "inc", "dec", "neg", "not", "bswap", "xchg", "popcnt"):
        return p[3].split(",")[0].strip() in ("rax", "eax", "ax", "al")
    return False


resolved = {}
for a in sites:
    win = window(a)
    got = None
    # locate the vtable pointer: first `mov X, [rax]` where X != rax
    vi = None
    for i, p in enumerate(win):
        if p[2] == "mov" and p[3].startswith("rcx, qword ptr [rax]"):
            vi = i
            break
        if p[2] == "mov" and re.match(r"^r\w+, qword ptr \[rax\]$", p[3]) \
                and not p[3].startswith("rax"):
            vi = i
            break
    if vi is None:
        resolved[a] = None
        continue
    # skip to the vtable load
    vt = None
    for j in range(vi + 1, min(vi + 6, len(win))):
        p = win[j]
        if p[2] == "mov" and re.match(r"^r\w+, qword ptr \[r\w+\]$", p[3]):
            vt = j
            break
    start = vt if vt is not None else vi
    # now look for the indirect call/jmp on a vtable-ish base
    for j in range(start, min(start + 12, len(win))):
        p = win[j]
        if p[2] in ("call", "jmp") and "qword ptr [" in p[3]:
            m = OFFPAT.search(p[3])
            if m:
                got = (p[0], int(m.group(1), 0), "direct " + p[2])
                break
        if p[2] == "mov":
            lhs, rhs = [x.strip() for x in p[3].split(",", 1)]
            m = re.match(r"^r\w+, qword ptr \[(r\w+)(.*)\]$", rhs)
            if m:
                off = 0
                om = OFFPAT.search(rhs)
                if om:
                    off = int(om.group(1), 0)
                    # remember: the NEXT call/jmp of that register is the vcall
                    for k in range(j + 1, min(j + 4, len(win))):
                        q = win[k]
                        if q[2] in ("call", "jmp") and q[3] == lhs:
                            got = (q[0], off, "via %s %s" % (lhs, q[2]))
                            break
                        if q[2] in ("call", "jmp"):
                            break
                    if got:
                        break
    resolved[a] = got

agg = collections.Counter()
rows = []
for a, g in sorted(resolved.items()):
    f = pd.func(a)
    if g:
        agg[g[1]] += 1
    rows.append((a, g, f))

print("=== vtable offsets called on ISteamInput, all 68 sites ===")
for off, c in sorted(agg.items()):
    sl = off // 8
    nm = VT.get(sl) if off % 8 == 0 and sl < NVSLOT else None
    warn = ""
    if off >= NVSLOT * 8:
        warn = "  *** OUTSIDE THE 48-SLOT VTABLE ***"
    elif off % 8:
        warn = "  *** not 8-byte aligned ***"
    print("  0x%03X  slot %3d  %3d sites  %-34s%s"
          % (off, sl, c, nm or "UNMAPPED", warn))
print("  unresolved: %d" % sum(1 for g in resolved.values() if not g))

print("\n=== per-site ===")
for a, g, f in rows:
    if g:
        sl = g[1] // 8
        nm = VT.get(sl) if g[1] % 8 == 0 and sl < NVSLOT else "???"
        print("  disp 0x%08X -> vcall 0x%08X off 0x%03X slot %3d  %-32s [%s]  fn %s"
              % (a, g[0], g[1], sl, nm, g[2], "%08X..%08X" % (f[0], f[1])))
    else:
        print("  disp 0x%08X -> UNRESOLVED   fn %08X..%08X" % (a, f[0], f[1]))
