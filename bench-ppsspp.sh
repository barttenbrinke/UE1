#!/usr/bin/env bash
#
# bench-ppsspp.sh -- run the intro flyby in PPSSPP uncapped for N seconds and
# print a timeline of frame times, so the dips (full castle view, big music)
# can be compared between builds. Uses the profiling build by default because
# its PSPPERF lines carry the engine breakdown. Not a hardware number: the
# emulator hides CPU cost, but heavier frames still show as dips.
#
# Usage: ./bench-ppsspp.sh [seconds] [EBOOT.PBP]
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SECS="${1:-120}"
EBOOT="${2:-$HERE/build-psp-prof/Unreal/EBOOT.PBP}"
P="$HOME/.config/ppsspp/PSP/GAME/Unreal"
PPSSPP=/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL

pkill -f PPSSPPSDL 2>/dev/null || true; sleep 1
cp "$EBOOT" "$P/EBOOT.PBP"
sed -i '' 's|^MaxFPS=.*|MaxFPS=0          ; bench: uncapped|' "$P/System/Unreal.ini"
rm -f "$P/System/Unreal.log"
("$PPSSPP" "$P/EBOOT.PBP" >/dev/null 2>&1 &)
sleep "$SECS"
pkill -f PPSSPPSDL 2>/dev/null || true; sleep 1
sed -i '' 's|^MaxFPS=.*|MaxFPS=20         ; frame cap; GetMaxTickRate() returns 0 in single player|' "$P/System/Unreal.ini"

echo "build $(md5 -q "$EBOOT" | cut -c1-8), ${SECS}s uncapped, $(git -C "$HERE" log --oneline -1 | cut -c1-60)"
printf "%7s %6s %7s | %s\n" "t(s)" "fps" "ms/frm" "engine ms per 100 frames (illum occl mesh polyv) | renderer ms (image complex gouraud tile)"
awk '
  /PSPPERF: 100 frames in/ { t+=$6; fps=$8; ms=$10; sub(/\(/,"",ms); tt=t }
  /PSPPERF:   renddev/ { img=$9; cx=$11; gr=$13; tl=$15 }
  /PSPPERF:   engine:/ { il=$5; oc=$7; me=$9; pv=$11;
     printf "%7.1f %6s %7s | %6s %6s %6s %6s | %6s %6s %6s %6s\n", tt, fps, ms, il, oc, me, pv, img, cx, gr, tl;
     if (fps+0 < min || min==0) { min=fps+0; tmin=tt } n++; sum+=fps }
  END { if (n) printf "intervals %d, mean %.1f fps, worst %.1f fps at t=%.0fs\n", n, sum/n, min, tmin }
' "$P/System/Unreal.log"
