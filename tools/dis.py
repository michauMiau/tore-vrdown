"""Disassemble named functions from teardown.exe and report indirect calls.

pdata-driven (desync-proof).  Usage:
  python3 dis.py <fn1,fn2,...>          # by VA
  python3 dis.py --calls 0x1C0 0x1C8    # histogram of call [reg+disp] in .text
"""
import struct, sys, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PATH = "/root/steamless/Teardown/teardown.exe"
D = open(PATH, "rb").read()

# section table (verified against objdump -h)
SECS = [(".text",  0x1000,   0x98076C, 0x400),
        (".rdata", 0x982000, 0x26F7A4, 0x980C00),
        (".dataa", 0xBF2000, 0x107E000, 0xBF0400),
        (".pdata", 0x1F97000, 0x68844, 0x1C6E400)]
IB = 0x140000000


def r2o(rva):
    for n, v, sz, r in SECS:
        if v <= rva < v + sz:
            return r + (rva - v)
    return None


def o2r(off):
    for n, v, sz, r in SECS:
        if r <= off < r + sz:
            return v + (off - r)
    return None


def fns():
    """-> dict start_va -> (end_va, size)"""
    d = {}
    o = 0x1C6E400
    n = 0x68844 // 12
    for i in range(n):
        b, e, u = struct.unpack_from("<III", D, o + i * 12)
        if b == 0 and e == 0:
            continue
        d[IB + b] = (IB + e, e - b)
    return d


FN = fns()
MD = Cs(CS_ARCH_X86, CS_MODE_64)
MD.detail = True
MD.skipdata = True


def disasm(va, size):
    rva = va - IB
    off = r2o(rva)
    if off is None:
        return []
    code = D[off:off + size]
    out = []
    for ins in MD.disasm(code, va):
        out.append(ins)
    return out


def enclosing(va):
    best = None
    for s, (e, sz) in FN.items():
        if s <= va < e:
            if best is None or s > best[0]:
                best = (s, e, sz)
    return best


def show(va, size=None):
    if size is None:
        p = enclosing(va)
        if not p:
            print(f"no pdata for {va:#x}")
            return
        s, e, sz = p
        size = sz
        print(f"### func {s:#x} len {sz:#x}")
    for ins in disasm(va, size):
        mark = ""
        if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("qword ptr [rip"):
            # rip-rel indirect
            tgt = ins.operands[0].mem.disp + ins.address + ins.size
            mark = f"   ; -> {tgt:#x}"
        elif ins.mnemonic in ("call", "jmp") and "rip" in ins.op_str:
            pass
        if ins.mnemonic == "call" and "[" in ins.op_str and "rip" not in ins.op_str:
            mark = "   ; INDIRECT"
        print(f"{ins.address:#x}  {ins.mnemonic:<8} {ins.op_str}{mark}")


def indirect_hist(lo, hi):
    """histogram of call [reg+disp8/disp32] displacements, funcs in [lo,hi)"""
    h = collections.Counter()
    where = collections.defaultdict(set)
    nfun = 0
    for s, (e, sz) in sorted(FN.items()):
        if not (lo <= s < hi):
            continue
        nfun += 1
        off = r2o(s - IB)
        if off is None:
            continue
        for ins in MD.disasm(D[off:off + sz], s):
            if ins.mnemonic != "call":
                continue
            op = ins.operands[0]
            if op.type != 3:  # MEM
                continue
            m = op.value.mem
            if m.base == 0:  # rip-relative
                continue
            if m.index:
                continue
            h[m.disp] += 1
            if m.disp < 0:
                where[m.disp].add(s)
    print(f"scanned {nfun} funcs in [{lo:#x},{hi:#x})")
    for disp, c in h.most_common(40):
        w = sorted(where.get(disp, []))[:6]
        ws = " ".join(f"{x:#x}" for x in w)
        print(f"  call [reg+{disp:#06x}]  x{c:<5} in {ws}")


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "--calls":
        lo = int(a[1], 0)
        hi = int(a[2], 0) if len(a) > 2 else lo + 0x100000
        indirect_hist(lo, hi)
    else:
        for x in a:
            show(int(x, 0))
            print()
