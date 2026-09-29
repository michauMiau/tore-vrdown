#!/usr/bin/env python3
"""S14 - WHICH interface does each of the 263 `call [0x009831B0]` sites select?

The dispatch takes ONE argument in rcx = the ADDRESS of a function-pointer
table slot.  If all 263 pass 0x00C48408 then all 263 are ISteamInput.  If some
pass 0x00C483D8 / 0x00C483F0 / 0x00C48420 they are other Steam interfaces
and must NOT be reported as Steam Input.

Also: the import-directory name for 0x009831B0 is 'SteamInternal_ContextInit'.
A 263-call count refutes that name, so we name it from behaviour instead.
"""
import collections
import struct
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, disp_of, iat, pdata, raw, s

IM = tdlib.iat()
pd = pdata()
recs = tdlib.recs()
byaddr = tdlib.idx()

SLOT = 0x009831B0
# every qword in .dataa that holds an in-image function pointer -> a
# candidate "interface selector" slot
im = tdlib.img()
sel = {}
for sec in im.sections:
    if sec["name"] not in (".rdata", ".dataa"):
        continue
    lo, hi = sec["rawptr"], sec["rawptr"] + sec["rsize"]
    d = im.data
    i = lo
    while i + 8 <= hi:
        q = struct.unpack_from("<Q", d, i)[0]
        if 0x140001000 <= q < 0x142000000:
            r = q - 0x140000000
            if pd.is_start(r):
                sel[sec["vaddr"] + (i - sec["rawptr"])] = r
        i += 8

print("=== candidate interface-selector slots: %d in .rdata/.dataa ===" % len(sel))

# which selectors are passed to the 0x009831B0 dispatch?
tally = collections.Counter()
where = collections.defaultdict(list)
unmatched = []
for r in recs:
    if r[2] != "call" or "rip" not in r[3]:
        continue
    if r[0] + r[1] + disp_of(r) != SLOT:
        continue
    # look back for the `lea rcx,[rip+..]` that set the selector
    a, found = r[0], None
    for _ in range(14):
        a -= 1
        p = byaddr.get(a)
        if p is None:
            continue
        if p[2] == "lea" and "rip" in p[3] and p[3].startswith("rcx,"):
            found = p[0] + p[1] + disp_of(p)
            break
    if found is None:
        unmatched.append(r[0])
        continue
    tally[found] += 1
    where[found].append(r[0])

print("\n=== selectors passed to the dispatch (call [0x%08X]) ===" % SLOT)
for t, c in tally.most_common():
    tgt = sel.get(t)
    tag = ""
    if tgt:
        tag = " -> fn 0x%08X" % tgt
    # if that fn is one of the four steam interface accessors, name it
    names = {
        0x004DA920: "SteamClient020", 0x004DA940: "SteamInput006",
        0x004DA970: "STEAMUGC_INTERFACE_VERSION016",
        0x004DA9A0: "STEAMUSERSTATS_INTERFACE_VERSION012",
    }
    if tgt in names:
        tag += "  (%s)" % names[tgt]
    print("  %4d x  selector 0x%08X%s" % (c, t, tag))
print("\n  call sites with no `lea rcx,[rip]` found in the preceding 14 insns: %d"
      % len(unmatched))
for a in unmatched[:20]:
    print("     %08X" % a)
