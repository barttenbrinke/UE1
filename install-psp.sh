#!/usr/bin/env bash
#
# install-psp.sh — assemble a runnable PSP install of Unreal from the built
# EBOOT and the v200 CD contents in GAME_ASSETS/.
#
# Usage:
#   ./install-psp.sh <dest-GAME-dir>
#
#   # test in PPSSPP on the laptop
#   ./install-psp.sh ~/.config/ppsspp/PSP/GAME
#
#   # install to the real card
#   ./install-psp.sh /Volumes/PSP2/PSP/GAME
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ASSETS="$HERE/GAME_ASSETS"
EBOOT="$HERE/build-psp/Unreal/EBOOT.PBP"
DEST="${1:?usage: install-psp.sh <dest-PSP/GAME-dir>}/Unreal"

[[ -f "$EBOOT"   ]] || { echo "error: EBOOT not built: $EBOOT" >&2; exit 1; }
[[ -d "$ASSETS/System" ]] || { echo "error: no game assets at $ASSETS" >&2; exit 1; }

echo "==> installing to $DEST"
mkdir -p "$DEST"
cp "$EBOOT" "$DEST/EBOOT.PBP"

# Game data. Music is .umx tracker modules, which only the OpenAL driver can
# play -- it is copied anyway so it is there once audio works.
for d in System Maps Textures Sounds Music; do
  echo "    $d"
  rsync -rt --delete --exclude '._*' --exclude '.DS_Store' "$ASSETS/$d/" "$DEST/$d/"
done

# The port's own configs replace the CD's, per the upstream README.
cp "$HERE/Engine/Config/Default.ini" "$HERE/Engine/Config/Unreal.ini" "$DEST/System/"

# Windows binaries are dead weight on the PSP (~4MB).
find "$DEST/System" \( -iname '*.dll' -o -iname '*.exe' \) -delete

# Point the engine at the driver this build actually contains: the fixed
# pipeline GL driver rather than the GLES one. Audio now uses NOpenALDrv,
# with reverb compiled out (PSP_NO_EFX), so it is left alone.
# The "=" anchors matter: without them these also rewrite the [section] headers,
# leaving two [NOpenGLDrv.NOpenGLRenderDevice] blocks with the GLES driver's
# settings (UseVAO, UseBGRA) masquerading as the GL driver's.
sed -i '' \
  -e 's|=NOpenGLESDrv\.NOpenGLESRenderDevice|=NOpenGLDrv.NOpenGLRenderDevice|g' \
  -e 's|^ViewportX=.*|ViewportX=480|' \
  -e 's|^ViewportY=.*|ViewportY=272|' \
  -e 's|^StartupFullscreen=.*|StartupFullscreen=True|' \
  -e 's|^OutputRate=.*|OutputRate=22050|' \
  -e 's|^MusicInterpolation=.*|MusicInterpolation=0|' \
  "$DEST/System/Default.ini" "$DEST/System/Unreal.ini"

# PSP-specific tunables. Kept here rather than in Engine/Config so the upstream
# defaults (which the Vita and PC builds rely on) stay untouched.
if ! grep -q '^\[PSP\]' "$DEST/System/Unreal.ini"; then
  cat >> "$DEST/System/Unreal.ini" <<'PSPCFG'

[PSP]
MaxFPS=20         ; frame cap; GetMaxTickRate() returns 0 in single player
TextureMaxFPS=8   ; procedural (fire) texture regeneration rate; 0 = every frame
LightMaps=1       ; 0 drops the lightmap pass entirely (debug/bisect switch)
LightMapHz=0      ; dynamic lightmap rebuild rate; 0 = every frame
LightFX=1         ; torch/fire/water light waver effects
VertexLight=1     ; fold lightmap into vertex colour: one geometry pass, not two
LightScale=150    ; VertexLight brightness %, 100 matches the two-pass original
Gamma=130         ; base texture gamma %, 100 = off
VertexArrays=1    ; 0 = stock immediate-mode renderer (bisect switch)
TextureBudgetMB=8 ; resident GL texture memory before least-recently-used eviction; the heap is ~38MB and Unreal's own data takes ~26MB
PSPCFG
fi

# macOS writes a 4KB "._name" AppleDouble beside every file written to a
# FAT/exFAT volume, and the PSP lists those as "Corrupted Data". rsync creates
# them too, not just Finder, so clean up whenever we wrote to a real card.
if df -P "$DEST" | awk 'NR==2 {exit !($1 ~ /^\/dev\/disk/)}'; then
  VOL=$(df -P "$DEST" | awk 'NR==2 {print $6}')
  if [ "$VOL" != "/" ]; then
    echo "==> removing macOS sidecars from $VOL"
    dot_clean -m "$VOL" 2>&1 | grep -viE 'spotlight|bad pathname|operation not permitted' || true
    find "$VOL" -name '.DS_Store' -delete 2>/dev/null || true
    sync
  fi
fi

echo "==> done: $(du -sh "$DEST" | cut -f1) in $DEST"
grep -hE '^(ViewportX|ViewportY|GameRenderDevice|AudioDevice|ViewportManager)=' \
  "$DEST/System/Unreal.ini" | sed 's/^/    /' | sort -u
