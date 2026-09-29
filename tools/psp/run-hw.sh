#!/bin/zsh
# run-hw.sh <label> <seconds> [prx args...] : run psplink_host/Unreal.prx on the PSP over PSPLink, save the log as hw-<label>.log
S="${PSP_WORK:-$HOME/.unreal-psp}"   # working dir: pspsh fifo/log, hw-*.log; created if missing
mkdir -p "$S"
H="${PSP_HOST:-$S/psplink_host}"   # usbhostfs root: Unreal.prx, EBOOT.PBP, Unreal-lazy.ini, Unreal.int; L=$1; T=$2; shift 2
pkill usbhostfs_pc; pkill -f pspsh_drive.py; pkill pspsh; sleep 1; rm -f $S/pspsh.in $S/pspsh.log
(cd $S && usbhostfs_pc $H > $S/usbhostfs.log 2>&1 &)
for i in $(seq 1 600); do grep -q "Connected to device" $S/usbhostfs.log && break; sleep 1; done
grep -q "Connected to device" $S/usbhostfs.log || { echo "$L: NO USB"; pkill usbhostfs_pc; exit 1; }
(cd $S && python3 "$(dirname "$0")/pspsh_drive.py" "$S" > $S/pspsh_drive.err 2>&1 &)
sleep 20; echo "ls" > $S/pspsh.in; sleep 4
echo "./Unreal.prx $*" > $S/pspsh.in; sleep $T
echo "reset" > $S/pspsh.in; sleep 20
pkill pspsh; pkill -f pspsh_drive.py; pkill usbhostfs_pc; sleep 2
cp $S/pspsh.log $S/hw-$L.log
echo "== $L ($*): $(grep -a -c 'frames in' $S/hw-$L.log) intervals, exceptions: $(grep -a -c -i 'exception' $S/hw-$L.log)"
grep -a "occlusion min size\|tiny polys" $S/hw-$L.log | head -2 | cut -c1-100
grep -a "PSPPERF:" $S/hw-$L.log | sed 's/.*PSPPERF://' | awk '/frames in/ { fps=$6; ms=$8; sub(/\(/,"",ms) } /engine:/ { il=$3; oc=$5; me=$7; pv=$9 } /occlusion:/ { cl=$3; ra=$5; sp=$7; bx=$9 } /mesh:/ { printf "%5s fps %3s ms | il %4s oc %5s me %5s pv %5s | cl %4s ra %4s sp %4s bx %4s\n", fps, ms, il, oc, me, pv, cl, ra, sp, bx }' | head -26
