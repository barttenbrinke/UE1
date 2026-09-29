#!/bin/zsh
# pull-save.sh <slot> : copy Save<slot>.usa (and any hub files Save<slot>N.usa) from the card to psplink_host/save and into the PPSSPP Save folder
setopt nullglob
S="${PSP_WORK:-$HOME/.unreal-psp}"   # working dir: pspsh fifo/log, hw-*.log; created if missing
mkdir -p "$S"
H="${PSP_HOST:-$S/psplink_host}"   # usbhostfs root: Unreal.prx, EBOOT.PBP, Unreal-lazy.ini, Unreal.int; N=$1; P=$HOME/.config/ppsspp/PSP/GAME/Unreal/Save
pkill usbhostfs_pc; pkill -f pspsh_drive.py; pkill pspsh; sleep 1; rm -f $S/pspsh.in $S/pspsh.log; rm -f $H/save/Save${N}*.usa
(cd $S && usbhostfs_pc $H > $S/usbhostfs.log 2>&1 &)
for i in $(seq 1 120); do grep -q "Connected to device" $S/usbhostfs.log && break; sleep 1; done
grep -q "Connected to device" $S/usbhostfs.log || { echo "NO USB"; pkill usbhostfs_pc; exit 1; }
(cd $S && python3 "$(dirname "$0")/pspsh_drive.py" "$S" > $S/pspsh_drive.err 2>&1 &)
sleep 25; echo "ls" > $S/pspsh.in; sleep 5
grep -a -q "Unreal.prx" $S/pspsh.log || { echo "SHELL DEAD"; echo "reset" > $S/pspsh.in; sleep 5; pkill pspsh; pkill -f pspsh_drive.py; pkill usbhostfs_pc; exit 1; }
echo "ls ms0:/PSP/GAME/Unreal/Save" > $S/pspsh.in; sleep 8
FILES=$(grep -a -o " Save${N}[0-9]*\.usa" $S/pspsh.log | tr -d ' ' | sort -u)
echo "on card: $FILES"
for F in $FILES; do
  echo "cp ms0:/PSP/GAME/Unreal/Save/$F host0:/save/$F" > $S/pspsh.in
  for i in $(seq 1 60); do grep -a -q "cp ms0:/PSP/GAME/Unreal/Save/$F -> " $S/pspsh.log && sleep 3 && break; sleep 2; done
done
echo "reset" > $S/pspsh.in; sleep 15
pkill pspsh; pkill -f pspsh_drive.py; pkill usbhostfs_pc; sleep 2
F=( $H/save/Save${N}*.usa ); [[ ${#F} -gt 0 ]] || { echo "NOTHING PULLED"; exit 1; }; ls -la $F; cp $F $P/ && echo "copied to PPSSPP Save/"
