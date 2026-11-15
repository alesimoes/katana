#!/bin/bash
# resume sweeps: melhor linha (maior T) por (tipo, forma, lote), e a do ladrilho 64x64 sem split para comparar
# uso: bash best.sh arquivo...
cat "$@" | awk '/ lote /{
  key=$1" "$6"x"$8" b"$10; sub(":","",key)
  t=$(NF); for(i=1;i<=NF;i++) if($i=="T") t=$(i-1);
  ms=0; for(i=1;i<=NF;i++) if($i=="ms") ms=$(i-1);
  tag=$2" "$5
  if(!(key in bt) || t+0>bt[key]+0){bt[key]=t; bm[key]=ms; btag[key]=tag}
  if($2=="64x64" && $5=="sp1"){bs[key]=t; bsm[key]=ms}
} END{for(k in bt) printf "%-22s melhor %-12s %.4f ms %.2f T   | 64x64 sp1: %.4f ms %.2f T\n",k,btag[k],bm[k],bt[k],bsm[k],bs[k]}' | sort -k2,2 -k3,3V -s
