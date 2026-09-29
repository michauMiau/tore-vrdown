#!/usr/bin/env python3
"""PE analysis library for steam_api64.dll (PE32+, image base 0x180000000).

RVA-centric: every address you hand in or get out is an RVA unless the name
says `va`.  Function boundaries come from .pdata RUNTIME_FUNCTION records --
never from a linear sweep.
"""
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = "/root/steamless/Teardown/steam_api64.dll"
IMAGE_BASE = 0x180000000


class Image:
    def __init__(self, path=DLL):
        self.path = path
        with open(path, "rb") as fh:
            self.d = fh.read()
        d = self.d
        if d[:2] != b"MZ":
            raise ValueError("not an MZ image")
        self.pe_off = struct.unpack_from("<I", d, 0x3C)[0]
        if d[self.pe_off:self.pe_off + 4] != b"PE\x00\x00":
            raise ValueError("not a PE image")
        coff = self.pe_off + 4
        nsec, = struct.unpack_from("<H", d, coff + 2)
        optsz, = struct.unpack_from("<H", d, coff + 16)
        opt = coff + 20
        magic, = struct.unpack_from("<H", d, opt)
        if magic != 0x20B:
            raise ValueError("not PE32+ (magic 0x%X)" % magic)
        self.image_base = struct.unpack_from("<Q", d, opt + 24)[0]
        assert self.image_base == IMAGE_BASE, hex(self.image_base)
        ndd, = struct.unpack_from("<I", d, opt + 108)
        ddir = opt + 112
        self.dirs = [struct.unpack_from("<II", d, ddir + 8 * i) for i in range(ndd)]
        sec = opt + optsz
        self.sections = []
        for i in range(nsec):
            o = sec + 40 * i
            nm = d[o:o + 8].rstrip(b"\x00").decode("ascii", "replace")
            vs, va, rs, raw = struct.unpack_from("<IIII", d, o + 8)
            self.sections.append((va, vs, raw, rs, nm))
        self._pdata = None
        self._md = None

    # ---- rva / file ----
    def to_file(self, rva):
        for va, vs, raw, rs, nm in self.sections:
            if va <= rva < va + max(vs, rs):
                return raw + (rva - va)
        raise ValueError("rva 0x%X not in any section" % rva)

    def to_rva(self, foff):
        for va, vs, raw, rs, nm in self.sections:
            if raw <= foff < raw + rs:
                return va + (foff - raw)
        raise ValueError("file offset 0x%X not in any section" % foff)

    def sec_of(self, rva):
        for va, vs, raw, rs, nm in self.sections:
            if va <= rva < va + max(vs, rs):
                return nm
        return "?"

    def code(self, rva, n):
        return self.b(rva, n)

    # ---- primitive reads ----
    def u16(self, rva):
        return struct.unpack_from("<H", self.d, self.to_file(rva))[0]

    def i16(self, rva):
        return struct.unpack_from("<h", self.d, self.to_file(rva))[0]

    def u32(self, rva):
        return struct.unpack_from("<I", self.d, self.to_file(rva))[0]

    def i32(self, rva):
        return struct.unpack_from("<i", self.d, self.to_file(rva))[0]

    def u64(self, rva):
        return struct.unpack_from("<Q", self.d, self.to_file(rva))[0]

    def b(self, rva, n=1):
        return self.d[self.to_file(rva):self.to_file(rva) + n]

    def cstr(self, rva, maxn=256):
        off = self.to_file(rva)
        z = self.d.find(b"\x00", off, off + maxn)
        return self.d[off:z if z > 0 else off + maxn].decode("ascii", "replace")

    def va(self, rva):
        return self.image_base + rva

    def rva_of_va(self, va):
        return va - self.image_base

    # ---- .pdata ----
    def _load_pdata(self):
        if self._pdata is not None:
            return
        rva, size = self.dirs[3]           # IMAGE_DIRECTORY_ENTRY_EXCEPTION
        n = size // 12
        off = self.to_file(rva)
        self._pdata = [struct.unpack_from("<III", self.d, off + 12 * k)
                       for k in range(n)]
        self._pdata.sort()

    def nfuncs(self):
        self._load_pdata()
        return len(self._pdata)

    def _find_fn(self, rva):
        """Return (start, end, unwind) for the function containing rva."""
        self._load_pdata()
        lo, hi = 0, len(self._pdata) - 1
        best = None
        while lo <= hi:
            mid = (lo + hi) // 2
            st, en, uw = self._pdata[mid]
            if rva < st:
                hi = mid - 1
            elif rva >= en:
                lo = mid + 1
            else:
                return st, en, uw
        # not inside: maybe it *is* a start
        for st, en, uw in self._pdata:
            if rva == st:
                return st, en, uw
        return best

    def fn_start(self, rva):
        f = self._find_fn(rva)
        return f[0] if f else None

    def fn_end(self, rva):
        f = self._find_fn(rva)
        return f[1] if f else None

    def fn_unwind(self, rva):
        f = self._find_fn(rva)
        return f[2] if f else None

    def is_exe(self, rva):
        return self.fn_start(rva) is not None

    # ---- disassembly ----
    def md(self):
        if self._md is None:
            self._md = Cs(CS_ARCH_X86, CS_MODE_64)
            self._md.detail = True
        return self._md

    def disas(self, rva, count=40, md=None):
        md = md or self.md()
        st = self.fn_start(rva) or rva
        en = self.fn_end(rva)
        n = (en - st) if en else 0x100
        out = []
        for k, ins in enumerate(md.disasm(self.b(st, n), self.va(st))):
            out.append(ins)
            if len(out) >= count:
                break
        return out

    def dump(self):
        self._load_pdata()
        print("dll      %s (%d bytes)" % (self.path, len(self.d)))
        print("image    0x%X" % self.image_base)
        for va, vs, raw, rs, nm in self.sections:
            print("  %-8s va 0x%08X vs 0x%08X raw 0x%08X rs 0x%08X"
                  % (nm, va, vs, raw, rs))
        print("pdata    %d runtime functions" % len(self._pdata))


if __name__ == "__main__":
    Image().dump()
