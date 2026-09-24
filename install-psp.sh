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

# Music: the tracker mixer is CPU the PSP does not have, so every .umx is
# also rendered to Music/<song>.wav (11025 Hz mono 16-bit, section order as
# stored) and the PSP audio driver streams that to a hardware channel instead.
# Needs xmp (brew install libxmp xmp); without it the console falls back to
# mixing the module in real time. Renders are derived from the CD data and
# stay out of the repo. Existing renders are kept.
if command -v xmp >/dev/null 2>&1; then
  echo "    Music renders"
  for f in "$ASSETS"/Music/*.umx; do
    n=$(basename "$f" .umx)
    [[ -f "$DEST/Music/$n.wav" ]] || xmp --nocmd -d wav -o "$DEST/Music/$n.wav" -f 11025 -m -F "$f" >/dev/null 2>&1 || echo "    (render failed: $n)"
  done
else
  echo "    (xmp not installed: no music renders, the PSP will mix modules itself)"
fi

# The port's own configs replace the CD's, per the upstream README.
cp "$HERE/Engine/Config/Default.ini" "$HERE/Engine/Config/Unreal.ini" "$DEST/System/"

# Windows binaries are dead weight on the PSP (~4MB).
find "$DEST/System" \( -iname '*.dll' -o -iname '*.exe' \) -delete

# Point the engine at the driver this build actually contains: the fixed
# (Shiny surfaces render the scene twice through every mirror: off. Coronas
# and high-detail actors stay on; the cost is small and the look matters.)
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
  -e '/^\[NOpenGLDrv.NOpenGLRenderDevice\]/,/^\[/ s|^ShinySurfaces=.*|ShinySurfaces=False|' \
  -e '/^\[NOpenGLDrv.NOpenGLRenderDevice\]/,/^\[/ s|^HighDetailActors=.*|HighDetailActors=True|' \
  -e '/^\[NOpenGLDrv.NOpenGLRenderDevice\]/,/^\[/ s|^Coronas=.*|Coronas=True|' \
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
Gamma=143         ; base texture gamma %, 100 = off
VertexArrays=1    ; 0 = stock immediate-mode renderer (bisect switch)
TextureBudgetMB=8 ; resident GL texture memory before least-recently-used eviction; the heap is ~38MB and Unreal's own data takes ~26MB
PSPCFG
fi

# PSP-2000 controls (one stick), the PSP shooter convention (Coded Arms, MoH
# Heroes, PSP Quake): the stick MOVES, the face buttons LOOK, triggers fire,
# D-pad jumps/ducks/switches weapons. The alternative -- face buttons move,
# stick looks -- is the four Joy1-4 lines as MoveBackward/StrafeRight/
# StrafeLeft/MoveForward with JoyX="Axis aTurn speed=0.5" and
# JoyY="Axis aLookUp speed=0.5" (inverted). See PSP-CONTROLS.txt.
# SDL's PSP pad: Joy1=Cross Joy2=Circle Joy3=Square Joy4=Triangle Joy5=Select
# Joy7=Start Joy10=L Joy11=R, JoyX/JoyY=stick, JoyPov*=D-pad. While Select is
# held the driver reports the D-pad as Joy14/Joy6/Joy15/Joy16 (up/down/left/right).
sed -i '' \
  -e 's|^Joy1=.*|Joy1=LookDown|' \
  -e 's|^Joy2=.*|Joy2=TurnRight|' \
  -e 's|^Joy3=.*|Joy3=TurnLeft|' \
  -e 's|^Joy4=.*|Joy4=LookUp|' \
  -e 's|^Joy5=.*|Joy5=ActivateTranslator|' \
  -e 's|^Joy7=.*|Joy7=ShowMenu|' \
  -e 's|^Joy10=.*|Joy10=AltFire|' \
  -e 's|^Joy11=.*|Joy11=Fire|' \
  -e 's|^JoyX=.*|JoyX=Axis aStrafe speed=1|' \
  -e 's|^JoyY=.*|JoyY=Axis aBaseY speed=1|' \
  -e 's|^JoyU=.*|JoyU=|' \
  -e 's|^JoyV=.*|JoyV=|' \
  -e 's|^JoyPovUp=.*|JoyPovUp=Jump|' \
  -e 's|^JoyPovDown=.*|JoyPovDown=Duck|' \
  -e 's|^JoyPovLeft=.*|JoyPovLeft=PrevWeapon|' \
  -e 's|^JoyPovRight=.*|JoyPovRight=NextWeapon|' \
  -e 's|^Joy14=.*|Joy14=InventoryActivate|' \
  -e 's|^Joy6=.*|Joy6=ActivateTranslator|' \
  -e 's|^Joy15=.*|Joy15=InventoryPrevious|' \
  -e 's|^Joy16=.*|Joy16=InventoryNext|' \
  "$DEST/System/Unreal.ini"
# The nub drifts a little at rest: a 20% dead zone.
sed -i '' '/^\[NSDLDrv.NSDLClient\]/,/^\[/ s|^DeadZoneXYZ=.*|DeadZoneXYZ=0.2|' "$DEST/System/Unreal.ini"

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
