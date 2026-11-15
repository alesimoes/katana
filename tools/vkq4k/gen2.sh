#!/bin/bash
# gera spv/*.spv: uma variante por (tipo, ladrilho, ativacao fp32?, tabela de escalas?, glu?). Uso: bash gen2.sh [tipos]
G=C:/source/katana/ref/vk/glslang/bin/glslang.exe
cd "$(dirname "$0")"; mkdir -p spv
$G -V --target-env vulkan1.3 -o spv/red2.spv red2.comp >/dev/null || exit 1
$G -V --target-env vulkan1.3 -o spv/cvt.spv cvt.comp >/dev/null || exit 1
# nome:BM:BN:SM:SN:glu_ok
TILES="64x64:64:64:2:2:1 128x16:128:16:4:1:1 64x16:64:16:2:1:1 128x32:128:32:4:1:1 64x32:64:32:2:1:1 64x16w4:64:16:4:1:0 64x32w4:64:32:4:1:0"
TYPES=${1:-"0 1 2 3 4"}
rm -f jobs.txt
for t in $TYPES; do for tl in $TILES; do
  IFS=: read nm bm bn sm sn gok <<< "$tl"
  for x in 0 1; do for sc in 0 1; do
    if [ $sc = 1 ] && [ $t != 0 ] && [ $t != 2 ]; then continue; fi
    for g in 0 1; do
      if [ $g = 1 ] && [ $gok = 0 ]; then continue; fi
      echo "$t $nm $bm $bn $sm $sn $x $sc $g" >> jobs.txt
    done
  done; done
done; done
while read t nm bm bn sm sn x sc g; do
  f=spv/t${t}_${nm}_x${x}_s${sc}_g${g}.spv
  if [ -f $f ] && [ $f -nt mm.comp ]; then continue; fi
  echo "$G -V --target-env vulkan1.3 -DWT=$t -DBM=$bm -DBN=$bn -DSM=$sm -DSN=$sn -DXF32=$x -DSCTAB=$sc -DGLU=$g -o $f mm.comp"
done < jobs.txt > cmds.txt
xargs -P 8 -I{} sh -c "{} > /dev/null || echo FALHOU {}" < cmds.txt
rm -f cmds.txt
ls spv | wc -l
