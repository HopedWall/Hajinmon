"""Prints the header information of an Xbox 360 executable (XEX2).

Usage:
    py tools/xex_info.py <default.xex>

The XEX2 format (all big-endian):
    0x00 "XEX2"
    0x04 module flags
    0x08 offset of the PE data (the actual image, encrypted/compressed)
    0x10 offset of the security info (signature, AES key, load address...)
    0x14 number of optional headers, followed by (key, value) pairs.
         If the low byte of the key is 0x00 or 0x01 the value is the data
         itself, otherwise it is a file offset where the data is stored.
"""
import struct
import sys

HEADER_NAMES = {
    0x000002FF: "Resource info",
    0x000003FF: "File format info",
    0x00010001: "Original base address",
    0x00010100: "Entry point",
    0x00010201: "Image base address",
    0x000103FF: "Import libraries",
    0x00018002: "Checksum / timestamp",
    0x000183FF: "Original PE name",
    0x000200FF: "Static libraries",
    0x00020104: "TLS info",
    0x00020200: "Default stack size",
    0x00020301: "Default filesystem cache size",
    0x00020401: "Default heap size",
    0x00030000: "System flags",
    0x00040006: "Execution info",
    0x00040201: "Title workspace size",
    0x00040310: "Game ratings",
    0x00040404: "LAN key",
    0x000405FF: "Xbox 360 logo",
    0x000406FF: "Multidisc media IDs",
    0x000407FF: "Alternate title IDs",
    0x00040801: "Additional title memory",
    0x00E10402: "Exports by name",
}
ENCRYPTION = {0: "none", 1: "AES (normal)"}
COMPRESSION = {0: "none", 1: "basic (uncompressed blocks + zeros)", 2: "normal (LZX)", 3: "delta"}


def u16(d, o): return struct.unpack_from(">H", d, o)[0]
def u32(d, o): return struct.unpack_from(">I", d, o)[0]


def cstr(d, o):
    return d[o : d.index(b"\0", o)].decode("latin-1")


def main():
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"XEX2":
        raise SystemExit("Not an XEX2 file")
    flags, pe_off, _, sec_off, nhdr = struct.unpack_from(">5I", data, 4)
    print(f"Module flags        : {flags:#010x}")
    print(f"PE data offset      : {pe_off:#x}  (image = {len(data) - pe_off:,} bytes in the file)")

    image_size = u32(data, sec_off + 4)
    load_addr = u32(data, sec_off + 0x110)
    region = u32(data, sec_off + 0x178)
    print(f"Load address        : {load_addr:#010x}")
    print(f"Image size          : {image_size:,} bytes ({image_size / 1024 / 1024:.1f} MB in memory)")
    print(f"Allowed regions     : {region:#010x}")
    print()

    headers = {}
    for i in range(nhdr):
        key, val = struct.unpack_from(">II", data, 0x18 + i * 8)
        headers[key] = val
        print(f"  [{key:#010x}] {HEADER_NAMES.get(key, '?'):30} = {val:#010x}")
    print()

    if 0x000003FF in headers:
        o = headers[0x000003FF]
        enc, comp = u16(data, o + 4), u16(data, o + 6)
        print(f"Encryption          : {ENCRYPTION.get(enc, enc)}")
        print(f"Compression         : {COMPRESSION.get(comp, comp)}")
    if 0x00010100 in headers:
        print(f"Entry point         : {headers[0x00010100]:#010x}")
    if 0x000183FF in headers:
        print(f"Original PE name    : {cstr(data, headers[0x000183FF] + 4)}")
    if 0x00040006 in headers:
        o = headers[0x00040006]
        media_id, version, base_version, title_id = struct.unpack_from(">4I", data, o)
        platform, exe_type, disc, ndisc = data[o + 16 : o + 20]
        print(f"Title ID            : {title_id:08X}")
        print(f"Version             : {version:#010x} (base {base_version:#010x})")
        print(f"Disc                : {disc}/{ndisc}")
    if 0x00020200 in headers:
        print(f"Default stack       : {headers[0x00020200]:,} bytes")
    if 0x00020104 in headers:
        o = headers[0x00020104]
        slots, raw_addr, data_size, raw_size = struct.unpack_from(">4I", data, o)
        print(f"TLS                 : {slots} slots, {data_size} bytes of data")

    if 0x000200FF in headers:
        o = headers[0x000200FF]
        size = u32(data, o)
        print("\nStatic libraries (version of the Xbox 360 SDK used to build the game):")
        for p in range(o + 4, o + size, 16):
            name = data[p : p + 8].rstrip(b"\0").decode("latin-1")
            major, minor, build, qfe = struct.unpack_from(">4H", data, p + 8)
            print(f"  {name:10} {major}.{minor}.{build}.{qfe & 0xFF}")

    if 0x000103FF in headers:
        o = headers[0x000103FF]
        size, str_size, nlibs = struct.unpack_from(">3I", data, o)
        names, p = [], o + 12
        while p < o + 12 + str_size:
            if data[p]:
                s = cstr(data, p)
                names.append(s)
                p += len(s)
            p += 1
        print("\nImported system libraries (functions the PC runtime has to implement):")
        p = o + 12 + str_size
        for _ in range(nlibs):
            rec_size = u32(data, p)
            version, min_version = u32(data, p + 28), u32(data, p + 32)
            name_idx, count = u16(data, p + 36), u16(data, p + 38)
            v = lambda x: f"{x >> 28}.{(x >> 24) & 0xF}.{(x >> 8) & 0xFFFF}.{x & 0xFF}"
            # each function import takes 2 records (variable + thunk), each variable import 1
            print(f"  {names[name_idx]:12} v{v(version)}  {count} records")
            p += rec_size


if __name__ == "__main__":
    main()
