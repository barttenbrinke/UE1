#!/usr/bin/env bash
#
# build-psp.sh — configure and build the PSP EBOOT (and, with --psplink, the
# PSPLink PRX used for debugging on the console).
#
# Usage:
#   ./build-psp.sh              # build-psp/Unreal/EBOOT.PBP
#   ./build-psp.sh --psplink    # also build-psplink/Unreal/Unreal.prx
#
# Requires the pspdev toolchain (psp-gcc, psp-pacman) with PSPDEV set or
# psp-config on the PATH, plus the packages listed in README.md.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PSPDEV="${PSPDEV:-$(dirname "$(dirname "$(command -v psp-gcc)")")}"
TOOLCHAIN="$PSPDEV/psp/share/pspdev.cmake"
[[ -f "$TOOLCHAIN" ]] || { echo "error: toolchain file not found at $TOOLCHAIN (is PSPDEV set?)" >&2; exit 1; }
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

# The same options for every PSP build: no editor, no networking, the fixed
# pipeline GL driver (pspgl), the OpenAL-API audio driver over the port's own
# mixer, everything in one PRX.
COMMON=(
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DCMAKE_C_FLAGS="-I$PSPDEV/psp/include -I$PSPDEV/psp/sdk/include -DPSP"
  -DBUILD_EDITOR=OFF -DBUILD_IPDRV=OFF
  -DBUILD_NOPENGLDRV=ON -DBUILD_NOPENGLESDRV=OFF
  -DBUILD_NOPENALDRV=ON -DBUILD_NULLSOUNDDRV=ON
  -DBUILD_PRX=ON
)

configure() {   # <build-dir> <extra cmake args...>
  local dir="$1"; shift
  if [[ ! -f "$dir/CMakeCache.txt" ]]; then
    cmake -G "Unix Makefiles" -B "$dir" "${COMMON[@]}" "$@" "$HERE/Source"
  fi
}

echo "==> card build"
configure "$HERE/build-psp" -DCMAKE_CXX_FLAGS="-I$PSPDEV/psp/include -I$PSPDEV/psp/sdk/include -DPSP" -DPSPLINK=OFF
cmake --build "$HERE/build-psp" -j"$JOBS"
echo "    $HERE/build-psp/Unreal/EBOOT.PBP"

if [[ "${1:-}" == "--psplink" ]]; then
  echo "==> PSPLink build (log mirrored to pspsh, engine cycle counters kept)"
  configure "$HERE/build-psplink" -DCMAKE_CXX_FLAGS="-I$PSPDEV/psp/include -I$PSPDEV/psp/sdk/include -DPSP -DPSP_KEEP_UCLOCK" -DPSPLINK=ON
  cmake --build "$HERE/build-psplink" -j"$JOBS"
  echo "    $HERE/build-psplink/Unreal/Unreal.prx"
fi
