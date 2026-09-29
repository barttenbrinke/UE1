#!/usr/bin/env bash
#
# fetch-assets.sh — assemble the game data for install-psp.sh from two freely
# downloadable sources, verified by checksum, into GAME_ASSETS/:
#
#   * Unreal Gold disc image (OldUnreal hosts it with Epic Games' permission):
#     Maps, Textures, Sounds and Music. These files are byte-identical to the
#     original 1998 release. Gold's own System/*.u are the v226 code and are
#     not usable here, so they are left out.
#   * Unreal Special Edition (the 1998 demo bundled with Sound Blaster Live!
#     cards, hosted on archive.org): System/. Its Core.u, Engine.u, Fire.u
#     and UnrealI.u are byte-identical to the retail v200 packages this engine
#     was built from, and its .int strings are a superset of retail's.
#
# If you own the original 1998 CD you do not need this script: copy its
# System, Maps, Textures, Sounds and Music folders into GAME_ASSETS/ instead.
#
# Usage:
#   ./fetch-assets.sh                    # ~780 MB download, ~600 MB unpacked into GAME_ASSETS/
#   ./fetch-assets.sh --keep             # keep the downloads in the repo root afterwards
#   ./fetch-assets.sh --dest <dir>       # unpack somewhere else
#
# The Epic Games Terms of Service apply to the use and distribution of this
# game and supersede any other end user agreement that may accompany it:
#   https://legal.epicgames.com/en-US/epicgames/tos
# This script asks you to accept them, as OldUnreal's own installers do. The
# sources, size and checksum below are the ones OldUnreal's installer uses
# (github.com/OldUnreal/FullGameInstallers). Nothing from the disc is ever
# committed to this repository: GAME_ASSETS/ is ignored by git.
#
# The demo archive is not covered by that permission; it is a game demo that
# was given away with hardware and is preserved on archive.org. Nothing from
# it is committed here either.
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
ISO_SIZE=676734976
ISO_SHA256=7e360d0cc9e5533f38859819fd3cbfea7c475ecd428f9f433b5f8e1d5742cbca
ISO_SOURCES=(
  https://files.oldunreal.net/UNREAL_GOLD.ISO
  https://files2.oldunreal.net/UNREAL_GOLD.ISO
  https://files3.oldunreal.net/UNREAL_GOLD.ISO
  https://archive.org/download/totallyunreal/UNREAL_GOLD.ISO
)
DEMO="$HERE/UnrealSpecialEdition.7z"
DEMO_SIZE=128609961
DEMO_SHA256=c158b030b39987aebbcbcd766b281b0b6020263a17b1989f8fd2c63a25a63855
DEMO_SOURCES=(
  https://archive.org/download/unreal-special-edition.-7z/UnrealSpecialEdition.7z
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
[[ -n "$UNPACK" ]] || { echo "error: need 7z, 7zz or bsdtar to unpack the downloads" >&2; exit 1; }

checksum() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# fetch <file> <size> <sha256> <url...>: download to <file> from the first
# source whose copy verifies; reuse an existing verified copy.
fetch() {
  local out="$1" size="$2" sum="$3"; shift 3
  if [[ -f "$out" && "$(stat -f%z "$out" 2>/dev/null || stat -c%s "$out")" -eq "$size" && "$(checksum "$out")" == "$sum" ]]; then
    echo "==> using existing $out"; return 0
  fi
  local url got
  for url in "$@"; do
    echo "==> downloading $url"
    if curl -L --fail --retry 5 --retry-all-errors -C - -o "$out" "$url"; then
      got="$(checksum "$out")"
      [[ "$got" == "$sum" ]] && return 0
      echo "    checksum mismatch ($got), trying the next source" >&2
      rm -f "$out"
    fi
  done
  echo "error: could not download a verified copy of $(basename "$out")" >&2
  return 1
}

unpack() {   # <archive> <dir>
  case "$UNPACK" in
    7z|7zz) "$UNPACK" x -y -o"$2" "$1" >/dev/null ;;
    bsdtar) bsdtar -xf "$1" -C "$2" ;;
  esac
}

# take <unpacked-dir> <folder...>: copy the named game folders (found at the
# root or one level down) into $ASSETS.
take() {
  local tmp="$1"; shift
  local sysdir gamedir d src
  sysdir="$(find "$tmp" -maxdepth 3 -type d -iname System | head -1)"
  [[ -n "$sysdir" ]] || { echo "error: no System folder found in the archive" >&2; exit 1; }
  gamedir="$(dirname "$sysdir")"
  for d in "$@"; do
    src="$(find "$gamedir" -maxdepth 1 -type d -iname "$d" | head -1)"
    if [[ -n "$src" ]]; then
      echo "    $d"
      rm -rf "$ASSETS/$d"
      cp -R "$src" "$ASSETS/$d"
      chmod -R u+w "$ASSETS/$d"
    else
      echo "    (no $d folder in the archive)"
    fi
  done
}

fetch "$ISO"  "$ISO_SIZE"  "$ISO_SHA256"  "${ISO_SOURCES[@]}"
fetch "$DEMO" "$DEMO_SIZE" "$DEMO_SHA256" "${DEMO_SOURCES[@]}"

mkdir -p "$ASSETS"
echo "==> unpacking Unreal Gold (Maps, Textures, Sounds, Music)"
TMP="$(mktemp -d "$HERE/.iso-unpack.XXXXXX")"
unpack "$ISO" "$TMP"
take "$TMP" Maps Textures Sounds Music
rm -rf "$TMP"

echo "==> unpacking Unreal Special Edition (System)"
TMP="$(mktemp -d "$HERE/.iso-unpack.XXXXXX")"
unpack "$DEMO" "$TMP"
take "$TMP" System
rm -rf "$TMP"
# Windows binaries and the demo's own configs are not used; install-psp.sh
# writes the port's configs.
find "$ASSETS/System" \( -iname '*.dll' -o -iname '*.exe' -o -iname '*.ini' \) -delete
[[ "$KEEP" == 1 ]] || rm -f "$ISO" "$DEMO"

echo "==> game data in $ASSETS ($(du -sh "$ASSETS" | cut -f1)); next: ./build-psp.sh && ./install-psp.sh <PSP/GAME dir>"
