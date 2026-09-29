#!/usr/bin/env python3
"""Shared helpers for the teardown.exe Steam Input call-site analysis.

Method rules enforced here (hard-won in this project):
  1. Function bounds come from the .pdata unwind table, never a linear sweep.
  2. A rip-relative disp32 is NOT an address: resolve target = rva + len + disp.
     Searching for a target RVA inside the displacement column is meaningless.
  3. Nothing is asserted without reading the bytes.
"""
import bisect
import pickle
import struct
import sys

PE = "/root/steamless/Teardown/teardown.exe"
PICKLE = "/root/.hermes/cache/scratch/td_index.pickle"
sys.path.insert(0, "/home/truenas_admin/teardown-analysis")
from tdmap import Image  # noqa: E402

_REC = None
_IDX = None
_IMG = None
_PD = None
_IAT = None


def _load():
    global _REC, _IDX
    if _REC is None:
        _REC = pickle.load(open(PICKLE, "rb"))
        _IDX = {r[0]: r for r in _REC}
    return _REC, _IDX


def recs():
    return _load()[0]


def idx():
    return _load()[1]


def img():
    global _IMG
    if _IMG is None:
        _IMG = Image(PE)
    return _IMG


def raw(rva, n):
    im = img()
    fo = im.to_file(rva)
    return im.data[fo:fo + n]


def s(rva, n=16):
    return raw(rva, n).hex(" ")


def cstr(rva, maxn=128):
    im = img()
    fo = im.to_file(rva)
    b = b""
    i = fo
    end = min(fo + maxn, len(im.data))
    while i < end and im.data[i]:
        b += bytes([im.data[i]])
        i += 1
    return b.decode("latin1", "replace")


class Pdata:
    def __init__(self):
        im = img()
        sec = [x for x in im.sections if x["name"] == ".pdata"][0]
        d = im.data
        self.n = sec["rsize"] // 12
        self.ent = []
        fo = sec["rawptr"]
        for i in range(self.n):
            b, e, u = struct.unpack_from("<III", d, fo + i * 12)
            if b < e:
                self.ent.append((b, e, u))
        self.ent.sort()
        self.starts = [x[0] for x in self.ent]
        self.starts_set = set(self.starts)

    def func(self, rva):
        """(begin, end, unwind) of the .pdata entry containing rva, else None."""
        i = bisect.bisect_right(self.starts, rva) - 1
        if i < 0:
            return None
        return self.ent[i] if self.ent[i][0] <= rva < self.ent[i][1] else None

    def is_start(self, rva):
        return rva in self.starts_set


def pdata():
    global _PD
    if _PD is None:
        _PD = Pdata()
    return _PD


def dis(rva, n=1):
    i = idx()
    out = []
    a = rva
    for _ in range(n):
        r = i.get(a)
        if r is None:
            break
        out.append(r)
        a += r[1]
    return out


def show(rva, n=1, annotate=None):
    for r in dis(rva, n):
        extra = ""
        if annotate:
            extra = annotate(r) or ""
        print("  %08X  %-6s %-38s %s%s" % (r[0], r[2], r[3], s(r[0], r[1]), extra))


def disp_of(r):
    """disp32 of a rip-relative operand string, or None."""
    if r is None:
        return None
    ops = r[3]
    i = ops.find("rip")
    if i < 0:
        return None
    j = i + 3
    while j < len(ops) and ops[j] in " +":
        j += 1
    sign = 1
    if j < len(ops) and ops[j] == "-":
        sign = -1
        j += 1
    if ops[j:j + 2].lower() == "0x":
        j += 2
    k = j
    while k < len(ops) and ops[k] in "0123456789abcdefABCDEF":
        k += 1
    if k == j:
        return None
    return sign * int(ops[j:k], 16)


def riprefs(target):
    """Every index record whose rip-relative disp32 resolves exactly to target."""
    out = []
    for r in recs():
        d = disp_of(r)
        if d is None:
            continue
        if r[0] + r[1] + d == target:
            out.append(r)
    return out


def imports():
    im = img()
    d = im.data
    e = struct.unpack_from("<I", d, 0x3C)[0]
    opt = e + 4 + 20
    ddr, = struct.unpack_from("<I", d, opt + 112 + 8)

    def c(off):
        o = b""
        i = off
        while i < len(d) and d[i]:
            o += bytes([d[i]])
            i += 1
        return o.decode("latin1")

    p = im.to_file(ddr)
    out = []
    while p + 20 <= len(d):
        oft, ts, fc, name_rva, ft = struct.unpack_from("<IIIII", d, p)
        if not (oft or name_rva or ft):
            break
        try:
            dll = c(im.to_file(name_rva))
        except ValueError:
            break
        if not dll or not all(32 <= ord(x) < 127 for x in dll):
            break
        t = im.to_file(oft or ft)
        i = 0
        while True:
            thunk = struct.unpack_from("<Q", d, t + i * 8)[0]
            if thunk == 0:
                break
            nm = ("ordinal#%d" % (thunk & 0xFFFF)) if thunk & (1 << 63) \
                else c(im.to_file(thunk) + 2)
            out.append((dll, nm, ft + i * 8))
            i += 1
            if i > 4000:
                break
        p += 20
    return out


def iat():
    global _IAT
    if _IAT is None:
        _IAT = {slot: (dll, nm) for dll, nm, slot in imports()}
    return _IAT
