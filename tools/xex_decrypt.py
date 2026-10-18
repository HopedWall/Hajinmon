"""Decrypts and decompresses an XEX2, producing the PE image as laid out in memory.

Usage:
    py tools/xex_decrypt.py <default.xex> <output.bin>

How it works:
1. The security info (offset 0x150) holds the file's AES key, encrypted with
   the "retail" key shared by all consoles (or with an all-zero key for
   devkit executables).
2. The image data (from the PE data offset to the end of the file) is
   encrypted with AES-128-CBC, zero IV, using the file key.
3. With "basic" compression the decrypted data is a sequence of blocks
   described in the File format info: (data bytes, zero bytes to append).

The output must be loaded at the base address given in the header
(e.g. 0x82000000). Only "none" and "basic" compression are supported; DOA4
does not need LZX.
"""
import struct
import sys

from Crypto.Cipher import AES

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
DEVKIT_KEY = bytes(16)


def u32(d, o): return struct.unpack_from(">I", d, o)[0]


def optional_headers(data):
    count = u32(data, 0x14)
    return {u32(data, 0x18 + i * 8): u32(data, 0x1C + i * 8) for i in range(count)}


def decrypt_image(data):
    pe_off, sec_off = u32(data, 0x08), u32(data, 0x10)
    image_size = u32(data, sec_off + 4)
    encrypted_key = data[sec_off + 0x150 : sec_off + 0x160]

    fmt = optional_headers(data)[0x000003FF]
    info_size = u32(data, fmt)
    encryption, compression = struct.unpack_from(">HH", data, fmt + 4)
    if compression not in (0, 1):
        raise SystemExit(f"Compression {compression} is not supported (only none/basic)")

    payload = data[pe_off:]
    if encryption == 1:
        payload = payload[: len(payload) // 16 * 16]
        for name, master in (("retail", RETAIL_KEY), ("devkit", DEVKIT_KEY)):
            file_key = AES.new(master, AES.MODE_ECB).decrypt(encrypted_key)
            plain = AES.new(file_key, AES.MODE_CBC, iv=bytes(16)).decrypt(payload)
            if plain[:2] == b"MZ":
                print(f"{name} key: ok (file key {file_key.hex().upper()})")
                payload = plain
                break
        else:
            raise SystemExit("Decryption failed: no key produces an MZ header")

    if compression == 0:
        image = bytearray(payload[:image_size])
    else:
        image = bytearray()
        src = 0
        for p in range(fmt + 8, fmt + info_size, 8):
            data_size, zero_size = struct.unpack_from(">II", data, p)
            image += payload[src : src + data_size]
            image += bytes(zero_size)
            src += data_size
    image = image[:image_size]
    image += bytes(image_size - len(image))
    return image


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    data = open(sys.argv[1], "rb").read()
    image = decrypt_image(data)
    open(sys.argv[2], "wb").write(image)
    base = optional_headers(data).get(0x00010201, 0)
    print(f"Wrote {len(image):,} bytes to {sys.argv[2]} (load at {base:#010x})")


if __name__ == "__main__":
    main()
