#!/usr/bin/env python3
"""S12 - AUDIT: is the import-directory name for a slot trustworthy here?

S11 reported 263 call sites of IAT slot 0x009831B0, which the import directory
names steam_api64!SteamInternal_ContextInit.  ContextInit is called once per
process, so 263 is impossible.  Either the rip-target arithmetic is wrong, or
the slot is shared/misnamed.  Verify by hand from raw bytes.
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

print("=== hand-verify 6 sample `call qword ptr [rip+disp]` into 0x9831B0 ===")
samples = [0x00140113, 0x0015FBEE, 0x001604B0, 0x004DD311, 0x00160826, 0x00160FE3]
byaddr = tdlib.idx()
for a in samples:
    r = byaddr.get(a)
    if r is None:
        print("  0x%08X  not in index" % a)
        continue
    b = raw(a, r[1])
    d = struct.unpack_from("<i", b, 2)[0]     # FF 15 disp32
    tgt = a + r[1] + d
    print("  %08X  bytes=%-14s len=%d  disp32=0x%08X (signed %d)  -> 0x%08X  [%s]"
          % (a, b.hex(" "), r[1], d & 0xFFFFFFFF, d, tgt, IM.get(tgt, "?")))

print("\n=== call-count per IAT slot, all 545 slots ===")
tally = collections.Counter()
for r in recs:
    if r[2] != "call" or "rip" not in r[3]:
        continue
    d = disp_of(r)
    if d is None:
        continue
    t = r[0] + r[1] + d
    if 0x00982000 <= t <= 0x00983210:
        tally[t] += 1
print("  distinct IAT slots called: %d" % len(tally))
for t, c in tally.most_common(12):
    print("  %4d x  0x%08X  %s" % (c, t, IM.get(t, ("?", "?"))))

print("\n=== does 0x009831B0 really sit in the steam descriptor's thunk range? ===")
im = tdlib.img()
d = im.data
e = struct.unpack_from("<I", d, 0x3C)[0]
opt = e + 4 + 20
ddr, = struct.unpack_from("<I", d, opt + 112 + 8)
p = im.to_file(ddr)
print("  import dir rva 0x%08X" % ddr)
while p + 20 <= len(d):
    oft, ts, fc, name_rva, ft = struct.unpack_from("<IIIII", d, p)
    if not (oft or name_rva or ft):
        break
    try:
        dll = cstr(im.to_file(name_rva))
    except ValueError:
        break
    if not dll or not all(32 <= ord(x) < 127 for x in dll):
        break
    n = 0
    t = im.to_file(oft or ft)
    while struct.unpack_from("<Q", d, t + n * 8)[0]:
        n += 1
        if n > 4000:
            break
    lo, hi = ft, ft + n * 8
    mark = "  <== contains 0x9831B0" if lo <= 0x009831B0 < hi else ""
    if mark or "steam" in dll.lower():
        print("  %-28s FirstThunk 0x%08X..0x%08X  %d imports%s"
              % (dll, lo, hi, n, mark))
    p += 20
