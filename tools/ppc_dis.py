"""Mini PowerPC disassembler (only the most common instructions) for quick inspection.

Usage:
    py tools/ppc_dis.py <default.bin> <address> [count=40] [base=0x82000000]

This is not a complete disassembler: unknown instructions are printed as
".long". Use Ghidra/IDA for serious analysis.
"""
import struct
import sys

D_FORM = {
    14: "addi", 15: "addis", 32: "lwz", 33: "lwzu", 34: "lbz", 36: "stw", 37: "stwu",
    38: "stb", 40: "lhz", 44: "sth", 48: "lfs", 50: "lfd", 52: "stfs", 54: "stfd",
}
SPR = {8: "lr", 9: "ctr", 1: "xer"}


def disasm(word, addr):
    op = word >> 26
    rt, ra = (word >> 21) & 31, (word >> 16) & 31
    simm = struct.unpack(">h", struct.pack(">H", word & 0xFFFF))[0]
    if word == 0x4E800020:
        return "blr"
    if word == 0x4E800420:
        return "bctr"
    if word == 0x60000000:
        return "nop"
    if op == 14 and ra == 0:
        return f"li r{rt}, {simm}"
    if op == 15 and ra == 0:
        return f"lis r{rt}, {word & 0xFFFF:#x}"
    if op in D_FORM:
        reg = "f" if op in (48, 50, 52, 54) else "r"
        if op in (14, 15):
            return f"{D_FORM[op]} r{rt}, r{ra}, {simm}"
        return f"{D_FORM[op]} {reg}{rt}, {simm}(r{ra})"
    if op in (58, 62):
        names = {(58, 0): "ld", (58, 1): "ldu", (58, 2): "lwa", (62, 0): "std", (62, 1): "stdu"}
        name = names.get((op, word & 3), "?")
        return f"{name} r{rt}, {simm & ~3}(r{ra})"
    if op == 18:
        li = word & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        target = li if word & 2 else addr + li
        return f"b{'l' if word & 1 else ''} {target:#010x}"
    if op == 16:
        bd = word & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        return f"bc {rt},{ra}, {addr + bd:#010x}"
    if op == 11:
        return f"cmpwi cr{rt >> 2}, r{ra}, {simm}"
    if op == 10:
        return f"cmplwi cr{rt >> 2}, r{ra}, {word & 0xFFFF}"
    if op == 24:
        return f"ori r{ra}, r{rt}, {word & 0xFFFF:#x}"
    if op == 31:
        xo = (word >> 1) & 0x3FF
        rb = (word >> 11) & 31
        spr = ((word >> 16) & 31) | (((word >> 11) & 31) << 5)
        if xo == 339:
            return f"mf{SPR.get(spr, f'spr{spr}')} r{rt}"
        if xo == 467:
            return f"mt{SPR.get(spr, f'spr{spr}')} r{rt}"
        if xo == 19:
            return f"mfcr r{rt}"
        if xo == 144:
            return f"mtcrf {(word >> 12) & 0xFF:#x}, r{rt}"
        if xo == 444:
            return f"mr r{ra}, r{rt}" if rt == rb else f"or r{ra}, r{rt}, r{rb}"
        if xo == 231:
            return f"stvx v{rt}, r{ra}, r{rb}"
        if xo == 103:
            return f"lvx v{rt}, r{ra}, r{rb}"
        if xo == 266:
            return f"add r{rt}, r{ra}, r{rb}"
        if xo == 40:
            return f"subf r{rt}, r{ra}, r{rb}"
        if xo == 0:
            return f"cmpw cr{rt >> 2}, r{ra}, r{rb}"
        if xo == 26:
            return f"cntlzw r{ra}, r{rt}"
    if op == 4:
        return f"(VMX) {word:#010x}"
    return f".long {word:#010x}"


def main():
    img = open(sys.argv[1], "rb").read()
    addr = int(sys.argv[2], 0)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    base = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x82000000
    for a in range(addr, addr + count * 4, 4):
        word = struct.unpack_from(">I", img, a - base)[0]
        print(f"{a:#010x}:  {word:08x}  {disasm(word, a)}")


if __name__ == "__main__":
    main()
