"""Writes THIRD_PARTY_NOTICES.txt for a release from the licenses in the ReXGlue SDK checkout.

Usage:
    python tools/third_party_notices.py <rexglue-sdk dir> <output file>

Collects the license of the SDK itself and of every library in its thirdparty/
folder that can end up in the game runtime. Libraries used only by the SDK's
code generator, its tests or macOS are left out. For libraries without a license
file, the license header of their first source file is used.
"""
import os
import sys

# Not in the released binaries: code generator only, tests, macOS only.
EXCLUDED = {"catch2", "disasm", "inja", "moltenvk"}
# Library -> license files to use (relative to the library folder), when the
# automatic search would pick the wrong ones.
PREFERRED = {
    "FFmpeg": ["COPYING.LGPLv2.1"],
    "libmspack": ["libmspack/COPYING.LIB"],
    "sdl3": ["LICENSE.txt"],
    "xxHash": ["LICENSE"],
    "glslang": ["LICENSE.txt"],
}
# Libraries whose files carry no license text.
KNOWN = {
    "disruptorplus": "MIT License, Copyright (c) Lewis Baker.\n"
                     "https://github.com/lewissbaker/disruptorplus",
    "ffmpeg-overlay": "Build configuration for FFmpeg (see FFmpeg).",
    "tiny-aes-c": "The Unlicense (public domain).\nhttps://github.com/kokke/tiny-AES-c",
}
LICENSE_NAMES = ("license", "licence", "copying", "unlicense")
SOURCE_EXTENSIONS = (".h", ".hpp", ".c", ".cc", ".cpp")

HEADER = """Third-party notices for Hajinmon
================================

Hajinmon is distributed under the BSD 3-Clause License (see LICENSE). Its
binaries also contain the following software, under the licenses below.

- ReXGlue SDK (game runtime and GPU plugin), modified by patches/ in the
  Hajinmon repository: https://github.com/rexglue/rexglue-sdk
- FFmpeg (libavcodec, libavutil) and libmspack are used under the GNU Lesser
  General Public License. Their source code is part of the ReXGlue SDK
  repository (thirdparty/FFmpeg, thirdparty/libmspack) and is available from
  https://ffmpeg.org and https://www.cabextract.org.uk/libmspack/. The complete
  source and build scripts of Hajinmon are available in its repository, so the
  program can be rebuilt with modified versions of these libraries.
- Windows: the launcher contains the .NET runtime (MIT License, Copyright (c)
  .NET Foundation and Contributors) and the Microsoft Visual C++ runtime
  (redistributed under the Microsoft Visual Studio license terms).

"""


def license_files(lib_dir, name):
    if name in PREFERRED:
        return [os.path.join(lib_dir, f) for f in PREFERRED[name]
                if os.path.isfile(os.path.join(lib_dir, f))]
    found = []
    for depth_dir in (lib_dir,):
        for entry in sorted(os.listdir(depth_dir)):
            path = os.path.join(depth_dir, entry)
            if os.path.isfile(path) and entry.lower().startswith(LICENSE_NAMES):
                found.append(path)
    return found


def source_header(lib_dir):
    """Leading comment block of the first source file (for libraries without a license file)."""
    for root, _, files in sorted(os.walk(lib_dir)):
        for name in sorted(files):
            if name.endswith(SOURCE_EXTENSIONS):
                lines = []
                with open(os.path.join(root, name), encoding="utf-8", errors="replace") as f:
                    for line in f:
                        stripped = line.strip()
                        if stripped and not stripped.startswith(("//", "/*", "*", "*/", "#pragma once")):
                            break
                        lines.append(line.rstrip())
                        if len(lines) >= 60:
                            break
                text = "\n".join(lines).strip()
                if text:
                    return f"(from {os.path.relpath(os.path.join(root, name), lib_dir)})\n{text}"
    return "(no license information found in the source)"


def main():
    sdk, out = sys.argv[1], sys.argv[2]
    parts = [HEADER]
    with open(os.path.join(sdk, "LICENSE"), encoding="utf-8", errors="replace") as f:
        parts.append("ReXGlue SDK\n" + "-" * 11 + "\n" + f.read().strip() + "\n")
    thirdparty = os.path.join(sdk, "thirdparty")
    for name in sorted(os.listdir(thirdparty), key=str.lower):
        lib_dir = os.path.join(thirdparty, name)
        if not os.path.isdir(lib_dir) or name in EXCLUDED or not os.listdir(lib_dir):
            continue
        texts = []
        for path in license_files(lib_dir, name):
            with open(path, encoding="utf-8", errors="replace") as f:
                texts.append(f.read().strip())
        if not texts:
            texts.append(KNOWN.get(name) or source_header(lib_dir))
        parts.append(f"{name}\n{'-' * len(name)}\n" + "\n\n".join(texts) + "\n")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n\n".join(parts))
    print(f"Wrote {out} ({len(parts) - 1} components)")


if __name__ == "__main__":
    main()
