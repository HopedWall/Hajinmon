#!/bin/bash
# Linux version of tools/setup_plume.cmd: clones Plume, the DXC binaries and
# FidelityFX FSR at the same pinned commits, into external/plume,
# external/dxc-bin and external/FidelityFX-FSR.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMMIT="$(sed -n 's/^set "COMMIT=\(.*\)"/\1/p' "$ROOT/tools/setup_plume.cmd" | tr -d '\r')"
DXC_COMMIT="$(sed -n 's/^set "DXC_COMMIT=\(.*\)"/\1/p' "$ROOT/tools/setup_plume.cmd" | tr -d '\r')"
FSR_COMMIT="$(sed -n 's/^set "FSR_COMMIT=\(.*\)"/\1/p' "$ROOT/tools/setup_plume.cmd" | tr -d '\r')"

[ -f "$ROOT/external/plume/CMakeLists.txt" ] || git clone https://github.com/renderbag/plume "$ROOT/external/plume"
git -C "$ROOT/external/plume" fetch --quiet origin
git -C "$ROOT/external/plume" checkout --quiet "$COMMIT"
git -C "$ROOT/external/plume" submodule update --init --recursive
echo "Plume ready at $COMMIT."

[ -f "$ROOT/external/dxc-bin/inc/dxcapi.h" ] || git clone https://github.com/renderbag/dxc-bin "$ROOT/external/dxc-bin"
git -C "$ROOT/external/dxc-bin" fetch --quiet origin
git -C "$ROOT/external/dxc-bin" checkout --quiet "$DXC_COMMIT"
echo "DXC ready at $DXC_COMMIT."

[ -f "$ROOT/external/FidelityFX-FSR/ffx-fsr/ffx_fsr1.h" ] || git clone https://github.com/GPUOpen-Effects/FidelityFX-FSR "$ROOT/external/FidelityFX-FSR"
git -C "$ROOT/external/FidelityFX-FSR" fetch --quiet origin
git -C "$ROOT/external/FidelityFX-FSR" checkout --quiet "$FSR_COMMIT"
echo "FidelityFX FSR ready at $FSR_COMMIT."
