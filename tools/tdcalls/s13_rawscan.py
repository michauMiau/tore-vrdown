#!/usr/bin/env python3
"""S13 - INDEPENDENT re-derivation, using raw bytes only (no capstone index).

Question: are there really 263 `call qword ptr [rip+d]` into 0x009831B0?
If yes, the import-directory NAME for that slot cannot be
steam_api64!SteamInternal_ContextInit (a no-arg int-returning bootstrap).

This scans .text bytes for the 6-byte sequence FF 15 xx xx xx xx and computes
the target arithmetically.  A misaligned capstone index could not produce a
cluster of 263 correctly-aimed 6-byte forms by chance.
"""
import collections
import struct
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import cstr, img, raw

im = img()
sec = [x for x in im.sections if x["name"] == ".text"][0]
lo, hi = sec["rawptr"], sec["rawptr"] + sec["rsize"]
d = im.data
TLO, THI = 0x00982000, 0x00983210

tally = collections.Counter()
sites = collections.defaultdict(list)
i = lo
n = len(d)
while i < hi - 6:
    if d[i] == 0xFF and d[i + 1] == 0x15:
        disp = struct.unpack_from("<i", d, i + 2)[0]
        rva = sec["vaddr"] + (i - sec["rawptr"])
        t = rva + 6 + disp
        if TLO <= t <= THI:
            tally[t] += 1
            sites[t].append(rva)
        i += 6
    else:
        i += 1

print("=== raw FF 15 byte scan of .text, targets landing in the IAT ===")
print("  distinct IAT slots reached: %d, total call sites: %d"
      % (len(tally), sum(tally.values())))
print("\n  top 10:")
for t, c in tally.most_common(10):
    print("    %5d x  0x%08X" % (c, t))

print("\n=== the steam_api64.dll slots, by raw scan ===")
IM = tdlib.iat()
for t in sorted(sites):
    if 0x009831A0 <= t <= 0x00983208:
        print("  %5d x  0x%08X  %s" % (tally[t], t, IM.get(t, ("?", "?"))))

print("\n=== first 5 + last 5 raw sites for 0x009831B0 ===")
v = sites.get(0x009831B0, [])
for a in v[:5] + (["..."] if len(v) > 10 else []) + v[-5:]:
    if a == "...":
        print("    ...")
        continue
    b = raw(a, 6)
    disp = struct.unpack_from("<i", b, 2)[0]
    print("    %08X  %s  disp 0x%08X -> 0x%08X" % (a, b.hex(" "),
                                                   disp & 0xFFFFFFFF, a + 6 + disp))

# also: the neighbouring instruction pattern, to see if these are truly
# accessor-then-deref sites
print("\n=== instruction context at 0x004DD311 (the one we care about) ===")
print("  ", raw(0x004DD30A, 0x20).hex(" "))
print("   4DD30A lea rcx,[0x00C48408]")
print("   4DD311 ff 15 .. -> 0x%08X" % (0x004DD311 + 6
                                        + struct.unpack_from("<i", raw(0x004DD311, 6), 2)[0]))
