#!/bin/zsh
# ppsspp-map.sh <map-url> <seconds> : run a map in PPSSPP with the memory report, tabulate heap over time
P=$HOME/.config/ppsspp/PSP/GAME/Unreal; I=$P/System/Unreal.ini; URL=$1; T=$2
sed -i '' "s|^CmdLine=.*|CmdLine=$URL -MEMDUMP -MALLINFO|" $I
rm -f $P/System/Unreal.log; pkill -f PPSSPPSDL; sleep 1
(/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL $P/EBOOT.PBP >/dev/null 2>&1 &)
sleep $T; pkill -f PPSSPPSDL; sleep 1
cp $P/System/Unreal.log "${PSP_WORK:-$HOME/.unreal-psp}/ppsspp-${URL%%.*}.log" 2>/dev/null
grep -a "LoadMap .* took" $P/System/Unreal.log | cut -c1-150
/usr/bin/python3 - $P/System/Unreal.log <<'PY'
import sys,re
L=open(sys.argv[1],'rb').read().decode('latin-1').split('\n'); n=0; last=None
for l in L:
    m=re.search(r'arena (\d+)KB kfree \d+KB tex (\d+)KB meshreload (\d+)KB',l)
    if m: last=m; n+=1; continue
    u=re.search(r'PSPPERF: heap used (\d+)KB free (\d+)KB \(arena (\d+)KB\)',l)
    if u and last and n%6==1: print('t=%3ds arena %5s used %5s free %4s tex %4s meshreload %4s'%((n-1)*5,last.group(1),u.group(1),u.group(2),last.group(2),last.group(3)))
print('intervals',n)
PY
grep -a "PSPSND: resident" $P/System/Unreal.log | tail -1 | cut -c1-110
echo "warnings: $(grep -a -c 'Exception\|Critical\|could not reload\|Out of memory' $P/System/Unreal.log)"
