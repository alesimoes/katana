#!/bin/bash
# Linux: gera spv/ para ladrilhos de ate 2 acumuladores por subgrupo e variantes do shader:
#   +db duplo buffer na memoria compartilhada, +k2 BK=128, +br B em linha-coluna (RowMajor).
# Uso: bash gen3.sh "0 1" "64x64w8 64x32s22" "+base +db +k2 +br +db+br"   (+base = sem variante)
G=${GLSLANG:-$HOME/.local/tc/gl/root/usr/bin/glslang}
cd "$(dirname "$0")"; mkdir -p spv
TYPES=$1; TILES=$2; VARS=$3
declare -A P=( [64x64]="64 64 2 2" [64x64w8]="64 64 4 2" [64x32w4]="64 32 4 1" [64x32s22]="64 32 2 2" [128x32w8]="128 32 4 2" [64x16w4]="64 16 4 1" [128x32]="128 32 4 1" )
rm -f cmds3.txt
for t in $TYPES; do for tl in $TILES; do for v in $VARS; do
  read bm bn sm sn <<< "${P[$tl]}"
  d=""; vn=""
  [[ $v == *+db* ]] && d="$d -DDB=1" && vn="$vn+db"
  [[ $v == *+k2* ]] && d="$d -DKS=2" && vn="$vn+k2"
  [[ $v == *+br* ]] && d="$d -DBR=1" && vn="$vn+br"
  [[ $v =~ \+pa([0-9]+) ]] && d="$d -DPADA=${BASH_REMATCH[1]}" && vn="$vn+pa${BASH_REMATCH[1]}"
  [[ $v =~ \+pb([0-9]+) ]] && d="$d -DPADB=${BASH_REMATCH[1]}" && vn="$vn+pb${BASH_REMATCH[1]}"
  for g in 0 1; do
    if [ $g = 1 ] && [[ $tl == *w4 || $tl == *w8 ]]; then continue; fi
    echo "$G -V --target-env vulkan1.3 -DWT=$t -DBM=$bm -DBN=$bn -DSM=$sm -DSN=$sn -DXF32=0 -DSCTAB=0 -DGLU=$g $d -o spv/t${t}_${tl}${vn}_x0_s0_g${g}.spv mm.comp" >> cmds3.txt
  done
done; done; done
xargs -P 6 -I{} sh -c "{} > /dev/null || echo FALHOU {}" < cmds3.txt
rm -f cmds3.txt
