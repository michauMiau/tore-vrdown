#!/usr/bin/env python3
"""S41 - audit the report against the resolved data.  Any drift is a bug in
the report, not in the data, so this is a self-check on the deliverable."""
import pickle
import re
import sys
sys.path.insert(0, "/home/truenas_admin/teardown-vr-mod/tools/tdcalls")

DOC = "/home/truenas_admin/teardown-vr-mod/docs/STEAMINPUT_CALLSITE.md"
res = [r for r in pickle.load(
    open("/root/.hermes/cache/scratch/sites.pkl", "rb")) if r["off"] is not None]
res.append({"disp": 0x004D97A8, "vcall": 0x004D97B4, "off": 0x008,
            "shape": "", "fn0": 0x004D9770, "fn1": 0x004D987A})
res.append({"disp": 0x004D9795, "vcall": 0x004D97B4, "off": 0x008,
            "shape": "", "fn0": 0x004D9770, "fn1": 0x004D987A})
txt = open(DOC).read()

fails = []


def want(cond, msg):
    if not cond:
        fails.append(msg)


print("sites in data: %d" % len(res))
for r in sorted(res, key=lambda z: z["disp"]):
    d = "0x%08X" % r["disp"]
    v = "0x%08X" % r["vcall"]
    want(d in txt, "dispatch RVA %s missing from report" % d)
    want(v in txt, "vcall RVA %s missing from report" % v)

# every slot mentioned must be a real slot
for s in set(r["off"] // 8 for r in res):
    want(0 <= s <= 47, "slot %d out of vtable range" % s)

# the two read chokepoints must be present and named
want("0x004E08ED" in txt, "digital read chokepoint missing")
want("0x004E04D0" in txt, "last analog read chokepoint missing")

# counts
want("**263**" in txt, "dispatcher count wrong")
want("**69**" in txt, "SteamInput site count wrong")

# the retail warning must be up top
head = txt[:2500].lower()
want("re-measure" in head, "retail-build warning not near the top")
want("retail" in head, "retail-build warning missing")

# unverified section must be explicit
want("## 5. UNVERIFIED" in txt, "no UNVERIFIED section")
want("5.1" in txt and "caller" in txt, "unresolved first-index not disclosed")
want("AMBIGUOUS" in txt, "ambiguous slots not flagged")

# slot 23 must NOT be given a confident name
for bad in ("GetStringForAnalogActionName at slot 23",
            "slot 23 = GetStringForAnalogActionName",
            "`0x0B8` | `GetStringForAnalogActionName`"):
    want(bad not in txt, "slot 23 given a hard name: %r" % bad)

print("checked %d sites" % len(res))
if fails:
    print("\nFAILURES (%d):" % len(fails))
    for f in fails:
        print("  -", f)
    sys.exit(1)
print("\nALL CHECKS PASSED")
