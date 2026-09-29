#!/usr/bin/env python3
"""S6 - what is actually AT the IAT slots in the file image?

The prior report treats 0x009831B0 as steam_api64!SteamInternal_ContextInit
because the import directory says so.  But the call pattern at 0x4DD30A is
  lea  rcx, [rip -> 0x00C48408]      ; ADDRESS of the accessor fn ptr slot
  call qword ptr [rip -> 0x009831B0] ; IAT slot
If slot 0x009831B0 really were SteamInternal_ContextInit, the game would be
passing &fnptr as HSteamUser - nonsense.  Check the raw bytes of the slot.
"""
import struct
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")
import tdlib
from tdlib import iat, pdata, raw, s

IM = tdlib.iat()
BASE = tdlib.img().image_base
pd = pdata()

print("=== raw contents of the steam_api64 IAT slots as they sit in the file ===")
for slot, (dll, nm) in sorted(IM.items()):
    q = struct.unpack("<Q", raw(slot, 8))[0]
    tag = ""
    if BASE <= q < BASE + 0x02000000:
        r = q - BASE
        tag = "  -> in-image rva 0x%08X" % r
        if pd.is_start(r):
            tag += " (pdata fn start)"
    elif q == 0:
        tag = "  (ZERO - unresolved in this dump)"
    print("  0x%08X  %-40s = %016X%s" % (slot, nm, q, tag))

print("\n=== the full IAT region 0x009831A0..0x00983210 ===")
a = 0x009831A0
while a < 0x00983210:
    q = struct.unpack("<Q", raw(a, 8))[0]
    print("  0x%08X : %016X %s" % (a, q, s(a, 8)))
    a += 8
