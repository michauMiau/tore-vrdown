"""Minimal PE parser: imports, exports, IAT entries, RVA->file offset.

VERIFIED-ONLY tool: every value printed is read from the file bytes.
No guessing about engine internals.
"""
import struct, sys

IMAGE_BASE_DEFAULT = 0x140000000


class PE:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        if d[:2] != b"MZ":
            raise ValueError("not MZ")
        self.e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
        assert d[self.e_lfanew:self.e_lfanew + 4] == b"PE\0\0"
        coff = self.e_lfanew + 4
        self.machine, self.nsec, self.tstamp, _, _, self.opthdr_sz, self.chars = \
            struct.unpack_from("<HHIIIHH", d, coff)
        opt = coff + 20
        self.magic = struct.unpack_from("<H", d, opt)[0]
        pe32p = self.magic == 0x20B
        self.pe32p = pe32p
        self.image_base = struct.unpack_from("<Q" if pe32p else "<I", d, opt + 24)[0]
        self.sec_align, self.file_align = struct.unpack_from("<II", d, opt + 32)
        self.image_size = struct.unpack_from("<I", d, opt + 56)[0]
        # NumberOfRvaAndSizes is the LAST field of the optional header.
        # PE32+: optional header is 112 bytes, PE32: 96.  Reading it from the
        # wrong offset silently yields garbage data directories.
        nsz_off = opt + 108 if pe32p else opt + 92
        self.ndd = struct.unpack_from("<I", d, nsz_off)[0]
        self.ddoff = nsz_off + 4
        # sections
        self.sections = []
        so = opt + self.opthdr_sz
        for i in range(self.nsec):
            b = so + i * 40
            name = d[b:b + 8].rstrip(b"\0").decode("latin1")
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", d, b + 8)
            self.sections.append(dict(name=name, vsize=vsize, vaddr=vaddr,
                                      rsize=rsize, raddr=raddr))

    def rva2off(self, rva):
        for s in self.sections:
            if s["vaddr"] <= rva < s["vaddr"] + max(s["vsize"], s["rsize"]):
                return s["raddr"] + (rva - s["vaddr"])
        return None

    def cstr(self, off):
        e = self.d.index(b"\0", off)
        return self.d[off:e].decode("latin1")

    def imports(self):
        """-> list of (dll, [(name_or_ordinal, iat_rva), ...])
        Only data directory [1] is the import table; iterating the others as
        if they were descriptors produces garbage.
        """
        out = []
        rva = struct.unpack_from("<I", self.d, self.ddoff + 1 * 8)[0]
        if rva == 0:
            return out
        o = self.rva2off(rva)
        if o is None:
            return out
        i = 0
        while True:
            b = o + i * 20
            if b + 20 > len(self.d):
                break
            ilt_rva, ts, fwd, nm, ft = struct.unpack_from("<IIIII", self.d, b)
            if ilt_rva == 0 and nm == 0 and ft == 0:
                break
            nmo = self.rva2off(nm)
            if nmo is None:
                break
            dll = self.cstr(nmo)
            thunk = ilt_rva if ilt_rva else ft
            to = self.rva2off(thunk)
            ents = []
            k = 0
            while True:
                if to + k * 8 + 8 > len(self.d):
                    break
                v = struct.unpack_from("<Q", self.d, to + k * 8)[0]
                if v == 0:
                    break
                if v & (1 << 63):  # ordinal
                    ents.append((("ord", v & 0xFFFF), ft + k * 8))
                elif v > 0xFFFFFFFF:
                    # BOUND import: the loader already patched the ILT with the
                    # live address, so the hint/name RVA is gone.  Record the
                    # slot and keep the bound target; do not try to read a name.
                    ents.append((("<bound:%016X>" % v), ft + k * 8))
                else:
                    ho = self.rva2off(v & 0x7FFFFFFF)
                    if ho is None:
                        ents.append((("<unmapped:%08X>" % v), ft + k * 8))
                    else:
                        ents.append((self.cstr(ho + 2), ft + k * 8))
                k += 1
            out.append((dll, ents, ft, nm))
        return out

    def exports(self):
        o = self.rva2off(self.ddoff)
        er, = struct.unpack_from("<I", self.d, o)
        if er == 0:
            return []
        eo = self.rva2off(er)
        ch, ts, mj, mn, nname, nbase, nn, ne, base2, nrd, npo, ner = \
            struct.unpack_from("<IIHHIIIIIII", self.d, eo)
        res = []
        for i in range(nn):
            nrva, ordr = struct.unpack_from("<II", self.d, self.rva2off(npo) + i * 8)
            res.append((self.cstr(self.rva2off(nrva)), ordr + nbase))
        return res

    def delay_imports(self):
        """delay-load descriptors -> (dll, [(name, iat_rva)])"""
        # data dir 13
        dd = self.ddoff + 13 * 8
        rva = struct.unpack_from("<I", self.d, dd)[0]
        if rva == 0:
            return []
        o = self.rva2off(rva)
        out = []
        i = 0
        while True:
            attrs, nm, mh, mha, iat, int_, bi, ui, ts = \
                struct.unpack_from("<IIIIIIIII", self.d, o + i * 32)
            if nm == 0 and iat == 0:
                break
            dll = self.cstr(self.rva2off(nm))
            to = self.rva2off(int_ if int_ else iat)
            ents = []
            k = 0
            while True:
                v = struct.unpack_from("<Q", self.d, to + k * 8)[0]
                if v == 0:
                    break
                if v & (1 << 63):
                    ents.append((("ord", v & 0xFFFF), iat + k * 8))
                else:
                    ho = self.rva2off(v & 0x7FFFFFFF)
                    ents.append((self.cstr(ho + 2), iat + k * 8))
                k += 1
            out.append((dll, ents))
            i += 1
        return out

    def va(self, rva):
        return self.image_base + rva


if __name__ == "__main__":
    pe = PE(sys.argv[1])
    print(f"# {sys.argv[1]}")
    print(f"# image_base=0x{pe.image_base:X} magic=0x{pe.magic:X} "
          f"nsec={pe.nsec} ndd={pe.ndd} image_size=0x{pe.image_size:X}")
    for s in pe.sections:
        print(f"#  sec {s['name']:<10} vaddr=0x{s['vaddr']:08X} vsize=0x{s['vsize']:08X} "
              f"raddr=0x{s['raddr']:08X} rsize=0x{s['rsize']:08X}")
    print(f"\n## IMPORTS ({sum(len(e) for _,e,_,_ in pe.imports())} thunks)")
    for dll, ents, ft, nm in pe.imports():
        print(f"[{dll}]  iat_rva=0x{ft:X}  ({len(ents)})")
        for name, iat in ents:
            print(f"    {name!s:<50} iat_rva=0x{iat:08X}")
    print("\n## DELAY IMPORTS")
    for dll, ents in pe.delay_imports():
        print(f"[{dll}] ({len(ents)})")
        for name, iat in ents:
            print(f"    {name!s:<50} iat_rva=0x{iat:08X}")
