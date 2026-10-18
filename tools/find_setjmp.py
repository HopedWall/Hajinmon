"""Looks for setjmp/longjmp candidates in a decrypted Xbox 360 image.

Usage:
    py tools/find_setjmp.py <default.bin> [base=0x82000000]

setjmp saves the non-volatile registers (r14-r31, f14-f31) into the buffer
pointed to by r3; longjmp reloads them from the same buffer. For each function
listed in .pdata we count the store/load instructions that use r3 as the base:
    std  rS, d(rA)  opcode 62 (xo 0)     ld  rD, d(rA)  opcode 58 (xo 0)
    stw  rS, d(rA)  opcode 36            lwz rD, d(rA)  opcode 32
    stfd fS, d(rA)  opcode 54            lfd fD, d(rA)  opcode 50
All PowerPC instructions are 32-bit big-endian: the opcode is in the top 6
bits, followed by rS/rD (5 bits), rA (5 bits) and the displacement (16 bits).
Note: hand-written leaf functions may not have a .pdata entry; setjmp/longjmp
in DOA4 were eventually found with a scan of the whole code section.
"""
import struct
import sys

STORES = {62: "std", 36: "stw", 54: "stfd"}
LOADS = {58: "ld", 32: "lwz", 50: "lfd"}


def decode(word):
    op = word >> 26
    rs = (word >> 21) & 31
    ra = (word >> 16) & 31
    d = word & 0xFFFF
    if op in (62, 58):
        if d & 3:  # stdu/ldu/lwa: not interesting here
            return None
        d &= 0xFFFC
    if d & 0x8000:
        d -= 0x10000
    return op, rs, ra, d


def pdata_functions(img):
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    nsec = struct.unpack_from("<H", img, pe + 6)[0]
    sec = pe + 24 + struct.unpack_from("<H", img, pe + 20)[0]
    for i in range(nsec):
        s = sec + i * 40
        if img[s : s + 8].rstrip(b"\0") == b".pdata":
            vsize, va = struct.unpack_from("<II", img, s + 8)
            for p in range(va, va + vsize, 8):
                start, packed = struct.unpack_from(">II", img, p)
                if start:
                    yield start, ((packed >> 8) & 0x3FFFFF) * 4


def main():
    img = open(sys.argv[1], "rb").read()
    base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x82000000
    results = []
    for start, size in pdata_functions(img):
        off = start - base
        stores, loads = set(), set()
        for p in range(off, off + size, 4):
            dec = decode(struct.unpack_from(">I", img, p)[0])
            if not dec or dec[2] != 3:
                continue
            op, rs, _, _ = dec
            if op in STORES and rs >= 13:
                stores.add((STORES[op], rs))
            elif op in LOADS and rs >= 13:
                loads.add((LOADS[op], rs))
        if len(stores) >= 15 or len(loads) >= 15:
            results.append((start, size, stores, loads))

    for start, size, stores, loads in sorted(results, key=lambda r: -max(len(r[2]), len(r[3]))):
        kind = "setjmp?" if len(stores) > len(loads) else "longjmp?"
        print(f"{start:#010x}  {size:5} bytes  stores to r3: {len(stores):2}  loads from r3: {len(loads):2}  -> {kind}")


if __name__ == "__main__":
    main()
