#!/usr/bin/env bash
#
# fetch-assets.sh — download the Unreal Gold disc image that OldUnreal hosts
# with Epic Games' permission, verify it, and unpack the game data into
# GAME_ASSETS/ for install-psp.sh.
#
# Usage:
#   ./fetch-assets.sh                    # ~650 MB download, ~450 MB unpacked into GAME_ASSETS/
#   ./fetch-assets.sh --keep             # keep the .ISO in the repo root afterwards
#   ./fetch-assets.sh --dest <dir>       # unpack somewhere else (e.g. GAME_ASSETS_GOLD to keep a v200 set)
#
# The Epic Games Terms of Service apply to the use and distribution of this
# game and supersede any other end user agreement that may accompany it:
#   https://legal.epicgames.com/en-US/epicgames/tos
# This script asks you to accept them, as OldUnreal's own installers do. The
# sources, size and checksum below are the ones OldUnreal's installer uses
# (github.com/OldUnreal/FullGameInstallers). Nothing from the disc is ever
# committed to this repository: GAME_ASSETS/ is ignored by git.
#
# Note: Unreal Gold ships the v226 game data. This engine is the v200 source;
# whether it loads v226 packages is being verified -- see README.md.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ASSETS="$HERE/GAME_ASSETS"
KEEP=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --keep) KEEP=1; shift ;;
    --dest) ASSETS="${2:?--dest needs a directory}"; shift 2 ;;
    *) echo "usage: fetch-assets.sh [--keep] [--dest <dir>]" >&2; exit 2 ;;
  esac
done
ISO="$HERE/UNREAL_GOLD.ISO"
SIZE=676734976
SHA256=7e360d0cc9e5533f38859819fd3cbfea7c475ecd428f9f433b5f8e1d5742cbca
SOURCES=(
  https://files.oldunreal.net/UNREAL_GOLD.ISO
  https://files2.oldunreal.net/UNREAL_GOLD.ISO
  https://files3.oldunreal.net/UNREAL_GOLD.ISO
  https://archive.org/download/totallyunreal/UNREAL_GOLD.ISO
)

if [[ -d "$ASSETS/System" ]]; then
  echo "$ASSETS already holds game data; remove it first if you want a fresh copy." >&2
  exit 0
fi

cat <<'EOF'
The Epic Games Terms of Service apply to the use and distribution of this
game, and they supersede any other end user agreements that may accompany it.
You may read the Terms of Service at:
  https://legal.epicgames.com/en-US/epicgames/tos
EOF
read -r -p "Do you accept the Epic Games Terms of Service? [y/N] " ANSWER
[[ "$ANSWER" =~ ^[Yy] ]] || { echo "Not accepted; nothing downloaded."; exit 1; }

command -v curl >/dev/null || { echo "error: curl is required" >&2; exit 1; }
UNPACK=""
if command -v 7z >/dev/null; then UNPACK=7z; elif command -v 7zz >/dev/null; then UNPACK=7zz; elif command -v bsdtar >/dev/null; then UNPACK=bsdtar; fi
[[ -n "$UNPACK" ]] || { echo "error: need 7z, 7zz or bsdtar to unpack the disc image" >&2; exit 1; }

checksum() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

if [[ -f "$ISO" && "$(stat -f%z "$ISO" 2>/dev/null || stat -c%s "$ISO")" -eq "$SIZE" && "$(checksum "$ISO")" == "$SHA256" ]]; then
  echo "==> using existing $ISO"
else
  for URL in "${SOURCES[@]}"; do
    echo "==> downloading $URL"
    if curl -L --fail --retry 3 -C - -o "$ISO" "$URL"; then
      GOT="$(checksum "$ISO")"
      [[ "$GOT" == "$SHA256" ]] && break
      echo "    checksum mismatch ($GOT), trying the next source" >&2
      rm -f "$ISO"
    fi
  done
  [[ -f "$ISO" && "$(checksum "$ISO")" == "$SHA256" ]] || { echo "error: could not download a verified disc image" >&2; exit 1; }
fi

echo "==> unpacking"
TMP="$(mktemp -d "$HERE/.iso-unpack.XXXXXX")"
case "$UNPACK" in
  7z|7zz) "$UNPACK" x -y -o"$TMP" "$ISO" >/dev/null ;;
  bsdtar) bsdtar -xf "$ISO" -C "$TMP" ;;
esac

# The game folders may sit at the root of the disc or one level down.
SYSDIR="$(find "$TMP" -maxdepth 3 -type d -iname System | head -1)"
[[ -n "$SYSDIR" ]] || { echo "error: no System folder found in the disc image" >&2; exit 1; }
GAMEDIR="$(dirname "$SYSDIR")"
mkdir -p "$ASSETS"
for d in System Maps Textures Sounds Music; do
  SRC="$(find "$GAMEDIR" -maxdepth 1 -type d -iname "$d" | head -1)"
  if [[ -n "$SRC" ]]; then
    echo "    $d"
    rm -rf "$ASSETS/$d"
    cp -R "$SRC" "$ASSETS/$d"
  else
    echo "    (no $d folder on the disc)"
  fi
done
rm -rf "$TMP"
[[ "$KEEP" == 1 ]] || rm -f "$ISO"

echo "==> game data in $ASSETS ($(du -sh "$ASSETS" | cut -f1)); next: ./build-psp.sh && ./install-psp.sh <PSP/GAME dir>"
