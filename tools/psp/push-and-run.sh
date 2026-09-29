#!/bin/zsh
# push-and-run.sh <label> <seconds> [prx args...] : push staged EBOOT + Unreal-lazy.ini to the card (md5-verified), then run Unreal.prx
S="${PSP_WORK:-$HOME/.unreal-psp}"   # working dir: pspsh fifo/log, hw-*.log; created if missing
mkdir -p "$S"
H="${PSP_HOST:-$S/psplink_host}"   # usbhostfs root: Unreal.prx, EBOOT.PBP, Unreal-lazy.ini, Unreal.int; L=$1; T=$2; shift 2
pkill usbhostfs_pc; pkill -f pspsh_drive.py; pkill pspsh; sleep 1; rm -f $S/pspsh.in $S/pspsh.log $H/eboot.verify $H/ini.verify
(cd $S && usbhostfs_pc $H > $S/usbhostfs.log 2>&1 &)
for i in $(seq 1 120); do grep -q "Connected to device" $S/usbhostfs.log && break; sleep 1; done
grep -q "Connected to device" $S/usbhostfs.log || { echo "$L: NO USB"; pkill usbhostfs_pc; exit 1; }
(cd $S && python3 "$(dirname "$0")/pspsh_drive.py" "$S" > $S/pspsh_drive.err 2>&1 &)
sleep 20; echo "ls" > $S/pspsh.in; sleep 4
grep -q "Unreal.prx" $S/pspsh.log || { echo "$L: SHELL DEAD"; echo "reset" > $S/pspsh.in; sleep 5; pkill pspsh; pkill -f pspsh_drive.py; pkill usbhostfs_pc; exit 1; }
echo "cp host0:/EBOOT.PBP ms0:/PSP/GAME/Unreal/EBOOT.PBP" > $S/pspsh.in; sleep 40
echo "cp ms0:/PSP/GAME/Unreal/EBOOT.PBP host0:/eboot.verify" > $S/pspsh.in; sleep 40
echo "cp host0:/Unreal-lazy.ini ms0:/PSP/GAME/Unreal/System/Unreal.ini" > $S/pspsh.in; sleep 8
echo "cp ms0:/PSP/GAME/Unreal/System/Unreal.ini host0:/ini.verify" > $S/pspsh.in; sleep 8
if [ -f $H/Unreal.int ]; then
  echo "cp host0:/Unreal.int ms0:/PSP/GAME/Unreal/System/Unreal.int" > $S/pspsh.in; sleep 6
  echo "cp ms0:/PSP/GAME/Unreal/System/Unreal.int host0:/int.verify" > $S/pspsh.in; sleep 6
  echo "int   $(md5 -q $H/Unreal.int) / $(md5 -q $H/int.verify 2>/dev/null)"
fi
echo "eboot $(md5 -q $H/EBOOT.PBP) / $(md5 -q $H/eboot.verify 2>/dev/null)"
echo "ini   $(md5 -q $H/Unreal-lazy.ini) / $(md5 -q $H/ini.verify 2>/dev/null)"
echo "./Unreal.prx $*" > $S/pspsh.in; sleep $T
echo "reset" > $S/pspsh.in; sleep 20
pkill pspsh; pkill -f pspsh_drive.py; pkill usbhostfs_pc; sleep 2
cp $S/pspsh.log $S/hw-$L.log
echo "== $L ($*): $(grep -a -c 'frames in' $S/hw-$L.log) intervals, exceptions: $(grep -a -c -i 'exception' $S/hw-$L.log)"
grep -a "PSPPERF: LoadMap" $S/hw-$L.log | cut -c1-400
echo done
