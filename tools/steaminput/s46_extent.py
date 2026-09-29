#!/usr/bin/env python3
"""Pin down the exact extent of each sub-table: the ladder tables are separated
by an 8-byte pad, so a run of code VAs must stop there.  Check what actually
sits between 0x54B5B8+47*8 and 0x54B738, and recount honestly."""
import sys

sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/steaminput")
from sap_lib import Image  # noqa: E402

im = Image()
BASE = im.image_base
LAD = sorted({0x00: 0x54ADF0, 0x08: 0x54AE28, 0x10: 0x54AEB8, 0x18: 0x54AF70,
              0x20: 0x54B048, 0x28: 0x54B148, 0x30: 0x54B260, 0x38: 0x54B378,
              0x40: 0x54B498, 0x48: 0x54B5B8, 0x50: 0x54B738}.items())


def iscode(r):
    return 0x1000 <= r < 0x53B000


print("raw qwords around the v005 -> v006 boundary:")
for a in range(0x54B720, 0x54B748, 8):
    v = im.u64(a)
    r = v - BASE
    print("  0x%X  %016X  rva 0x%08X  %s" % (a, v, r & 0xFFFFFFFF,
                                             "code" if iscode(r) else "----"))

print("\ncount of consecutive code VAs from each sub-table start (cap 200):")
for k, vt in LAD:
    n = 0
    while n < 200 and iscode(im.u64(vt + 8 * n) - BASE):
        n += 1
    nxt = [v for _, v in LAD if v > vt]
    lim = (min(nxt) - vt) // 8 if nxt else 200
    print("  +0x%02X vt 0x%06X  run=%-3d  distance to next sub-table=%d  %s"
          % (k, vt, n, lim, "OK" if n == lim else "MISMATCH"))

print("\nper-sub-table: run length, and whether it equals the gap to the next")
for k, vt in LAD[:-1]:
    n = 0
    while n < 200 and iscode(im.u64(vt + 8 * n) - BASE):
        n += 1
    nxt = min(v for _, v in LAD if v > vt)
    print("  +0x%02X  0x%06X  entries=%d  ends 0x%06X  next 0x%06X  pad %d bytes"
          % (k, vt, n, vt + 8 * n, nxt, nxt - (vt + 8 * n)))
