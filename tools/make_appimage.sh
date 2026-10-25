#!/bin/bash
# Builds build/appimage/Hajinmon-<version>-x86_64.AppImage: the recompiled game for
# Linux / Steam Deck (Vulkan only). Like the Windows release it contains no game
# data: on the first start it asks for the player's own Dead or Alive 4 .iso.
#
# Usage: tools/make_appimage.sh [game build dir] [SDK library dir] [SDK source dir]
#   game build dir:  where HajinmonGame was built for Linux
#                    (default: doa4/out/build/linux-amd64-release)
#   SDK library dir: librexruntime.so (default: external/rexglue-sdk/out/install/linux-amd64/lib)
#   SDK source dir:  for the license files (default: external/rexglue-sdk)
# Needs: the game built for Linux (see README.md), python3, file, and network
# access the first time (appimagetool is downloaded to ~/.cache/hajinmon).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/doa4/out/build/linux-amd64-release}"
SDK_LIB="${2:-$ROOT/external/rexglue-sdk/out/install/linux-amd64/lib}"
SDK_SRC="${3:-$ROOT/external/rexglue-sdk}"
VERSION="$(sed -n 's:.*<Version>\(.*\)</Version>.*:\1:p' "$ROOT/launcher/Hajinmon.Launcher.csproj")"
OUT="$ROOT/build/appimage"
APPDIR="$OUT/Hajinmon.AppDir"
TOOL_DIR="$HOME/.cache/hajinmon"
TOOL="$TOOL_DIR/appimagetool-x86_64.AppImage"

if [ ! -x "$BUILD/HajinmonGame" ]; then
    echo "HajinmonGame not found in $BUILD: build the game for Linux first." >&2
    exit 1
fi

echo "[1/4] Collecting the game and its libraries..."
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"
cp "$BUILD/HajinmonGame" "$APPDIR/usr/bin/"
# The ReXGlue runtime and the Xenos GPU plugin (loaded at runtime from next to the
# executable), plus any other library the build resolved outside the system.
# Libraries are looked up next to the executable ($ORIGIN), in the build folder and
# in the SDK install; system libraries stay on the host.
cp "$BUILD"/*.so* "$APPDIR/usr/bin/" 2>/dev/null || true
for binary in "$BUILD/HajinmonGame" "$BUILD"/*.so*; do
    [ -f "$binary" ] || continue
    LD_LIBRARY_PATH="$BUILD:$SDK_LIB" ldd "$binary" | awk '/=> \// { print $3 }' | while read -r lib; do
        case "$lib" in
            /lib/*|/usr/lib/*) ;;
            *) cp -n "$lib" "$APPDIR/usr/bin/" ;;
        esac
    done
done
if LD_LIBRARY_PATH="$APPDIR/usr/bin" ldd "$APPDIR/usr/bin/HajinmonGame" | grep -q "not found"; then
    echo "Missing libraries:" >&2
    LD_LIBRARY_PATH="$APPDIR/usr/bin" ldd "$APPDIR/usr/bin/HajinmonGame" | grep "not found" >&2
    exit 1
fi
strip --strip-unneeded "$APPDIR"/usr/bin/* 2>/dev/null || true
# Licenses: Hajinmon's, and those of the SDK and its third-party libraries.
mkdir -p "$APPDIR/usr/share/doc/hajinmon"
cp "$ROOT/LICENSE" "$APPDIR/usr/share/doc/hajinmon/LICENSE"
cp "$SDK_SRC/LICENSE" "$APPDIR/usr/share/doc/hajinmon/LICENSE-rexglue-sdk"
python3 "$ROOT/tools/third_party_notices.py" "$SDK_SRC" \
    "$APPDIR/usr/share/doc/hajinmon/THIRD_PARTY_NOTICES.txt"
# The native Plume renderer and its shader compiler, in builds that have them.
if [ -f "$BUILD/libdxcompiler.so" ]; then
    cp "$ROOT/external/plume/LICENSE" "$APPDIR/usr/share/doc/hajinmon/LICENSE-plume"
    cp "$ROOT/renderer/LICENSE-dxc.txt" "$APPDIR/usr/share/doc/hajinmon/LICENSE-dxc"
    cp "$ROOT/external/FidelityFX-FSR/license.txt" "$APPDIR/usr/share/doc/hajinmon/LICENSE-fidelityfx-fsr"
fi

echo "[2/4] Writing the launcher script..."
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/bash
# Starts Hajinmon. Arguments:
#   <path>          the Dead or Alive 4 .iso (or extracted game folder) to use
#   --change-game   ask again for the game
#   --<option>=...  passed to the game (e.g. --resolution_scale=2, --no-fullscreen,
#                   --renderer=plume for the experimental native renderer)
HERE="$(dirname "$(readlink -f "$0")")"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/Hajinmon"
GAME_FILE="$CONFIG_DIR/game_path"
mkdir -p "$CONFIG_DIR"

game=""
options=()
for arg in "$@"; do
    case "$arg" in
        --change-game) rm -f "$GAME_FILE" ;;
        # The native renderer presents to X11 windows (XWayland on Wayland desktops).
        --renderer=plume) options+=("$arg"); export SDL_VIDEO_DRIVER="${SDL_VIDEO_DRIVER:-x11}" ;;
        --*) options+=("$arg") ;;
        *) game="$arg" ;;
    esac
done
if [ -z "$game" ] && [ -f "$GAME_FILE" ]; then
    game="$(cat "$GAME_FILE")"
fi

if [ -z "$game" ] || [ ! -e "$game" ]; then
    title="Select your Dead or Alive 4 disc image (.iso)"
    if command -v zenity >/dev/null 2>&1; then
        game="$(zenity --file-selection --title="$title" \
            --file-filter="Xbox 360 disc image (*.iso) | *.iso *.ISO" \
            --file-filter="All files | *")" || exit 1
    elif command -v kdialog >/dev/null 2>&1; then
        game="$(kdialog --title "$title" --getopenfilename "$HOME" \
            "*.iso *.ISO|Xbox 360 disc image")" || exit 1
    else
        echo "Usage: $(basename "${APPIMAGE:-$0}") /path/to/DeadOrAlive4.iso" >&2
        exit 1
    fi
fi
[ -n "$game" ] || exit 1
printf '%s' "$game" > "$GAME_FILE"

export LD_LIBRARY_PATH="$HERE/usr/bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# No tearing by default (mailbox or FIFO presentation); --vulkan_allow_present_mode_immediate
# brings it back for variable refresh rate displays.
exec "$HERE/usr/bin/HajinmonGame" --game_data_root="$game" --gpu_plugin=xenos \
    --alloc_top_down --fullscreen --no-vulkan_allow_present_mode_immediate \
    --no-vulkan_allow_present_mode_fifo_relaxed "${options[@]}"
EOF
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/hajinmon.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Hajinmon
Comment=Dead or Alive 4 recompiled for PC
Exec=AppRun
Icon=hajinmon
Categories=Game;
Terminal=false
X-AppImage-Version=$VERSION
EOF

# A plain original icon (no game artwork): a white "H" on a dark red square.
python3 - "$APPDIR/hajinmon.png" <<'EOF'
import struct, sys, zlib
size = 256
rows = []
for y in range(size):
    row = bytearray(b"\0")
    for x in range(size):
        inside_h = 64 <= y < 192 and (64 <= x < 104 or 152 <= x < 192 or (104 <= x < 152 and 112 <= y < 144))
        row += bytes((255, 255, 255, 255) if inside_h else (150, 20, 30, 255))
    rows.append(bytes(row))
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
                              + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
EOF
ln -sf hajinmon.png "$APPDIR/.DirIcon"

echo "[3/4] Getting appimagetool..."
if [ ! -x "$TOOL" ]; then
    mkdir -p "$TOOL_DIR"
    wget -q -O "$TOOL" \
        https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$TOOL"
fi

echo "[4/4] Building the AppImage..."
TARGET="$OUT/Hajinmon-$VERSION-x86_64.AppImage"
rm -f "$TARGET"
# Extract-and-run: works without FUSE (WSL, containers).
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$TOOL" --no-appstream "$APPDIR" "$TARGET" >/dev/null
echo "AppImage ready: $TARGET ($(du -h "$TARGET" | cut -f1))"
