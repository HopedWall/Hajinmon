"""Lists the game functions (.pdata) and the kernel imports of a decrypted XEX.

Usage:
    py tools/xex_imports.py <default.xex> <default.bin>

- .pdata: an 8-byte table entry per function (start address, then a packed
  field with the length in instructions in bits 8..29).
- Imports: for each library the XEX header lists the addresses of "records".
  Each record in the image holds (type << 24) | (library index << 16) | ordinal;
  type 0 = data slot (a variable, or the slot of a function), type 1 = function
  thunk. The kernel identifies functions by number (ordinal), not by name.
"""
import struct
import sys
from collections import defaultdict


def u32be(d, o): return struct.unpack_from(">I", d, o)[0]


def main():
    xex = open(sys.argv[1], "rb").read()
    img = open(sys.argv[2], "rb").read()
    hdrs = {u32be(xex, 0x18 + i * 8): u32be(xex, 0x1C + i * 8) for i in range(u32be(xex, 0x14))}
    base = hdrs[0x00010201]

    # .pdata
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    nsec = struct.unpack_from("<H", img, pe + 6)[0]
    sec = pe + 24 + struct.unpack_from("<H", img, pe + 20)[0]
    for i in range(nsec):
        name = img[sec + i * 40 : sec + i * 40 + 8].rstrip(b"\0")
        if name == b".pdata":
            vsize, va = struct.unpack_from("<II", img, sec + i * 40 + 8)
            funcs = []
            for p in range(va, va + vsize, 8):
                start, packed = struct.unpack_from(">II", img, p)
                if start:
                    funcs.append((start, ((packed >> 8) & 0x3FFFFF) * 4))
            sizes = sorted(s for _, s in funcs)
            print(f"Functions in .pdata: {len(funcs):,}")
            print(f"  median size {sizes[len(sizes) // 2]} bytes, largest {sizes[-1]:,} bytes")
            print(f"  first {funcs[0][0]:#010x}, last {funcs[-1][0]:#010x}\n")

    # Imports
    o = hdrs[0x000103FF]
    _, str_size, nlibs = struct.unpack_from(">3I", xex, o)
    names = [s.decode() for s in xex[o + 12 : o + 12 + str_size].split(b"\0") if s]
    p = o + 12 + str_size
    for _ in range(nlibs):
        rec_size = u32be(xex, p)
        name_idx, count = struct.unpack_from(">HH", xex, p + 36)
        by_type = defaultdict(list)
        for r in range(count):
            addr = u32be(xex, p + 40 + r * 4)
            value = u32be(img, addr - base)
            by_type[value >> 24].append((value & 0xFFFF, addr))
        # Every function also has a type 0 record (its slot in the address
        # table): the real variables are the type 0 records without a thunk.
        funcs = by_type[1]
        func_ordinals = {o for o, _ in funcs}
        variables = [v for v in by_type[0] if v[0] not in func_ordinals]
        print(f"{names[name_idx]}: {len(funcs)} functions, {len(variables)} variables")
        if variables:
            print("  variables (ordinals):", ", ".join(str(o) for o, _ in sorted(variables)))
        print("  functions (ordinals):", ", ".join(str(o) for o, _ in sorted(funcs)))
        print()
        p += rec_size


if __name__ == "__main__":
    main()
