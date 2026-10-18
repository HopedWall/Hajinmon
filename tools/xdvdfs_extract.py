"""Minimal extractor for Xbox / Xbox 360 disc images (XDVDFS file system).

Usage:
    py tools/xdvdfs_extract.py <image.iso> <output_dir> [--list]

Format overview:
- The disc is split into 2048-byte sectors.
- The game partition starts at an offset that depends on the disc type
  (XGD1/XGD2/XGD3). Sector 32 of the partition holds the volume descriptor:
  the magic string "MICROSOFT*XBOX*MEDIA" followed by the sector and size of
  the root directory.
- Each directory is a binary tree of entries. Each entry is:
      u16 left   offset (in 4-byte units) of the left child, 0 = none
      u16 right  offset of the right child, 0 = none
      u32 sector start sector (relative to the partition)
      u32 size   size in bytes
      u8  attr   attributes (0x10 = directory)
      u8  nlen   name length
      char name[nlen]
  All values are little-endian.
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITION_OFFSETS = {"XDVDFS": 0, "XGD1": 0x18300000, "XGD2": 0xFD90000, "XGD3": 0x2080000}
ATTR_DIRECTORY = 0x10


def find_partition(f):
    for kind, off in PARTITION_OFFSETS.items():
        f.seek(off + 32 * SECTOR)
        hdr = f.read(28)
        if hdr[:20] == MAGIC:
            root_sector, root_size = struct.unpack("<II", hdr[20:28])
            return kind, off, root_sector, root_size
    raise SystemExit("XDVDFS volume descriptor not found")


def walk_dir(f, base, sector, size):
    """Returns the entries (name, sector, size, attributes) of a directory."""
    if size == 0:
        return []
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    entries = []
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, sec, fsize, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF:  # padding at the end of a sector
            continue
        name = data[off + 14 : off + 14 + nlen].decode("latin-1")
        entries.append((name, sec, fsize, attr))
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
    return sorted(entries, key=lambda e: e[0].lower())


def extract(f, base, sector, size, out_dir, rel, list_only):
    for name, sec, fsize, attr in walk_dir(f, base, sector, size):
        path = os.path.join(rel, name)
        if attr & ATTR_DIRECTORY:
            print(f"{'<DIR>':>12}  {path}{os.sep}")
            extract(f, base, sec, fsize, out_dir, path, list_only)
            continue
        print(f"{fsize:>12,}  {path}")
        if list_only:
            continue
        dest = os.path.join(out_dir, path)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        f.seek(base + sec * SECTOR)
        remaining = fsize
        with open(dest, "wb") as o:
            while remaining:
                chunk = f.read(min(remaining, 16 * 1024 * 1024))
                if not chunk:
                    raise IOError(f"Unexpected end of image while reading {path}")
                o.write(chunk)
                remaining -= len(chunk)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    list_only = "--list" in sys.argv
    if len(args) != 2:
        raise SystemExit(__doc__)
    iso, out_dir = args
    with open(iso, "rb") as f:
        kind, base, root_sector, root_size = find_partition(f)
        print(f"Disc type: {kind}, partition at {base:#x}, root at sector {root_sector}")
        extract(f, base, root_sector, root_size, out_dir, "", list_only)


if __name__ == "__main__":
    main()
