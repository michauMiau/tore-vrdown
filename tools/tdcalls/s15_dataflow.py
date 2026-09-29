#!/usr/bin/env python3
"""S15 - FINAL dataflow.  Proper register tracking, not contiguity.

Real sequence at 0x4DD30A (note the `lea rdx` between the call and the derefs -
it is the method's 2nd argument, not a break in the chain):

    004DD30A  lea  rcx, [rip -> 0x00C48408]     ; &selector for ISteamInput
    004DD311  call qword ptr [rip -> 0x009831B0]; returns ISteamInput*
    004DD317  lea  rdx, [rip -> "flashlight"]   ; arg2 of the virtual call
    004DD31E  mov  rcx, [rax]                   ; ptr -> &iface
    004DD321  mov  rax, [rcx]                   ; &iface -> vtable
    004DD324  call qword ptr [rax + K]          ; virtual call

Rule: after the dispatch call, walk forward; allow only instructions that do
NOT define rax; the chain must terminate in
    mov rcx,[rax] ; mov rax,[rcx] ; call [reg+off]
Any instruction writing rax in between kills the chain.
"""
import collections
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, disp_of, iat, pdata, raw, riprefs, s

IM = tdlib.iat()
pd = pdata()
recs = tdlib.recs()
byaddr = tdlib.idx()

DISPATCH = 0x009831B0
SI_SEL = 0x00C48408
VTB = 0x54B738

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

CALLP = re.compile(r"^qword ptr \[(\w+)\s*\+\s*(0x[0-9a-fA-F]+|\d+)\]$")


def defines_rax(r):
    """does this instruction write rax?"""
    m, ops = r[2], r[3]
    if m in ("mov", "movzx", "movsxd", "lea", "pop", "lods"):
        first = ops.split(",")[0].strip()
        return first in ("rax", "eax", "ax", "al")
    if m in ("add", "sub", "and", "or", "xor", "imul", "shl", "shr", "sar", "or", "inc", "dec", "neg", "not", "bswap"):
        if m == "xor" and ops.startswith("eax, eax"):
            return True
        return ops.split(",")[0].strip() in ("rax", "eax", "ax", "al")
    if m in ("call", "ret", "push", "nop", "int3", "cmp", "test", "jmp", "jne", "je", "ja", "jb", "jl", "jg", "jle", "jge", "js", "jns", "jo", "jno", "movups", "movaps", "movdqa", "movdqu", "cmpps", "setz", "sete", "setne", "cwtl", "cdqe"):
        return False
    return True     # unknown -> assume it kills the chain


results = []
for r in recs:
    if r[2] != "call" or "rip" not in r[3]:
        continue
    t = r[0] + r[1] + disp_of(r)
    if t != DISPATCH:
        continue
    # selector lea
    a, sel = r[0], None
    for _ in range(14):
        a -= 1
        p = byaddr.get(a)
        if p and p[2] == "lea" and "rip" in p[3] and p[3].startswith("rcx,"):
            sel = p[0] + p[1] + disp_of(p)
            break
    if sel != SI_SEL:
        continue
    # walk forward for the deref chain
    a = r[0] + r[1]
    seen = []
    chain = None
    for _ in range(24):
        p = byaddr.get(a)
        if p is None:
            break
        seen.append(p)
        if p[2] == "mov" and p[3] == "rcx, qword ptr [rax]":
            q = byaddr.get(p[0] + p[1])
            if q and q[2] == "mov" and q[3] == "rax, qword ptr [rcx]":
                c = byaddr.get(q[0] + q[1])
                if c and c[2] == "call":
                    m = CALLP.match(c[3])
                    if m:
                        chain = (c[0], m.group(1), int(m.group(2), 0))
                        break
        if defines_rax(p):
            break
        a += p[1]
    if chain:
        results.append((chain[0], chain[1], chain[2], r[0], seen))

print("=== ISteamInput vtable calls, PROVEN by dataflow from selector 0x%08X ==="
      % SI_SEL)
print("    %d call sites\n" % len(results))
agg = collections.Counter()
for c, reg, off, dc, seen in results:
    agg[off] += 1
print("--- histogram ---")
for off, n in sorted(agg.items()):
    nm = VT.get(off // 8) if off % 8 == 0 and off // 8 < NVSLOT else None
    warn = ""
    if off >= NVSLOT * 8:
        warn = "   *** OFFSET 0x%X EXCEEDS THE %d-SLOT VTABLE ***" % (off, NVSLOT)
    elif nm is None:
        warn = "   (slot undecidable from steam_api64.dll)"
    print("  off 0x%03X  slot %3d  count %3d  %-34s%s"
          % (off, off // 8, n, nm or "UNMAPPED", warn))

print("\n--- full site list ---")
for c, reg, off, dc, seen in sorted(results):
    f = pd.func(c)
    nm = VT.get(off // 8) if off % 8 == 0 and off // 8 < NVSLOT else None
    # argument strings referenced between the dispatch and the vcall
    args = []
    for p in seen:
        if p[2] == "lea" and "rip" in p[3]:
            tt = p[0] + p[1] + disp_of(p)
            if tt == SI_SEL or tt == DISPATCH:
                continue
            txt = cstr(tt, 28)
            if txt.isprintable() and len(txt) > 1:
                args.append("%s=%r" % (p[3].split(",")[0].strip(), txt))
    print("  %08X  call [%-4s + 0x%03X] slot %3d  %-32s fn %s  args: %s"
          % (c, reg, off, off // 8, nm or "UNMAPPED",
             ("%08X..%08X" % (f[0], f[1])) if f else "?", ", ".join(args) or "-"))
