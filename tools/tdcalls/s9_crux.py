#!/usr/bin/env python3
"""S9 - CRUX: what does the `call` right after `lea rcx,[accessor-slot]` do?

Pattern at 0x4DD30A:
    lea   rcx, [rip -> 0x00C48408]        ; &fnptr_slot
    call  qword ptr [rip -> 0x009831B0]  ; some IAT slot
    mov   rcx, qword ptr [rax]           ; deref the RETURN VALUE
    mov   rax, qword ptr [rcx]           ; vtable
    call  qword ptr [rax + K]

If that IAT slot were really steam_api64!SteamInternal_ContextInit (returns a
small enum), `mov rcx,[rax]` would fault.  So either
  (a) the slot is a game-side dispatcher, or
  (b) the import-directory name for that slot is being misapplied.
Test empirically: enumerate every accessor ref in the table and print the
instruction that FOLLOWS it, plus which IAT slot it calls.
"""
import collections
import struct
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, dis, disp_of, iat, pdata, raw, riprefs, s

IM = tdlib.iat()
pd = pdata()
recs = tdlib.recs()
byaddr = tdlib.idx()

# every accessor pointer slot we know of
ACC = {}
for k in (-3, -1, 0, 1):
    a = 0x00C48408 + k * 0x18
    q = struct.unpack("<Q", raw(a, 8))[0]
    if 0x140000000 <= q < 0x142000000:
        ACC[a] = q - 0x140000000

print("=== accessor slots in the function-pointer table ===")
for a, r in sorted(ACC.items()):
    f = pd.func(r)
    print("  slot 0x%08X -> fn 0x%08X" % (a, r))
    if f:
        for rr in dis(f[0], 12):
            note = ""
            if "rip" in rr[3]:
                t = rr[0] + rr[1] + disp_of(rr)
                note = "  ; ->0x%08X" % t
                if rr[2] == "call":
                    note += " IAT=%s" % (IM.get(t, "NOT-IAT"),)
                else:
                    n = cstr(t, 32)
                    if n.isprintable() and len(n) > 2:
                        note += " %r" % n
            print("      %08X %-6s %-38s%s" % (rr[0], rr[2], rr[3], note))

print("\n=== what instruction FOLLOWS each `lea rcx,[accessor-slot]`? ===")
tally = collections.Counter()
examples = {}
for a, fnrva in sorted(ACC.items()):
    for r in riprefs(a):
        nxt = byaddr.get(r[0] + r[1])
        if not nxt:
            continue
        if nxt[2] != "call":
            continue
        t = nxt[0] + nxt[1] + disp_of(nxt) if "rip" in nxt[3] else None
        key = (t, IM.get(t, ("?", "?"))[1])
        tally[key] += 1
        examples.setdefault(key, (r[0], nxt[0]))

for (t, nm), c in tally.most_common():
    e = examples[(t, nm)]
    print("  %3d x  call -> IAT 0x%08X  %-42s  (lea at %08X, call at %08X)"
          % (c, t if t else 0, nm, e[0], e[1]))
