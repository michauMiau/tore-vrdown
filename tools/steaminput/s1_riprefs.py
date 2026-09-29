#!/usr/bin/env python3
"""S1 - find every rip-relative reference to a target RVA, using the capstone
index (NOT a linear byte sweep) so instruction boundaries are already correct.

target_rva = insn_rva + insn_length + disp32      <- the rule from INPUT_API_FINDINGS
"""
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/steaminput")
import tdlib
from tdlib import Pdata, disp_of, dis, raw, s, cstr

rec, idx = tdlib._load()
pd = Pdata()
print("index %d recs, .pdata %d entries" % (len(rec), pd.n))

TARGETS = {
    0x00A4B6B0: "SteamInput006",
    0x00A4B678: "SteamClient020",
}


def riprefs(target):
    hits = []
    for r in rec:
        d = disp_of(r)
        if d is None:
            continue
        t = r[0] + r[1] + d
        if t == target:
            hits.append(r)
    return hits


for t, name in TARGETS.items():
    print("\n=== rip-relative refs to 0x%08X (%r)  bytes=%r" % (t, cstr(t), s(t, 20)))
    hits = riprefs(t)
    print("    %d hits" % len(hits))
    for r in hits:
        f = pd.func(r[0])
        print("    %08X  %-7s %-34s  fn=%s  raw=%s"
              % (r[0], r[2], r[3], ("%08X..%08X" % f) if f else "NONE",
                 s(r[0], r[1])))
