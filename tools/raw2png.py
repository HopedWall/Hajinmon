"""Converts the doa4 .raw frame captures (src/frame_capture.cpp) to PNG.

Usage:
    py tools/raw2png.py <capture.raw> [<other.raw> ...]

.raw format: "RAWI", width, height, stride (uint32 little-endian), then the
rows as R8 G8 B8 X8. The PNG is written next to the .raw file using only the
standard library (zlib).
"""
import struct
import sys
import zlib


def png_chunk(kind, data):
    chunk = kind + data
    return struct.pack(">I", len(data)) + chunk + struct.pack(">I", zlib.crc32(chunk) & 0xFFFFFFFF)


def convert(path):
    data = open(path, "rb").read()
    magic, width, height, stride = struct.unpack_from("<4sIII", data, 0)
    if magic != b"RAWI":
        raise SystemExit(f"{path}: not a doa4 capture")
    pixels = data[16:]
    rows = bytearray()
    for y in range(height):
        row = pixels[y * stride : y * stride + width * 4]
        rows.append(0)  # PNG filter "none"
        for x in range(width):
            rows += row[x * 4 : x * 4 + 3]  # drop the X byte
    png = b"\x89PNG\r\n\x1a\n"
    png += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += png_chunk(b"IDAT", zlib.compress(bytes(rows), 6))
    png += png_chunk(b"IEND", b"")
    out = path.rsplit(".", 1)[0] + ".png"
    open(out, "wb").write(png)
    print(f"{out} ({width}x{height})")


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        convert(arg)
