#!/bin/sh
# treino do v6 no Qwen3.6: 5 trechos de 60 passos (lote 4 = 1200 janelas); cada trecho continua do anterior (--resume com o estado do otimizador).
# STOP nesta pasta cancela entre os trechos.
cd /mnt/dados/cu-v6 || exit 1
export KATANA_THINK=0
M=/mnt/dados/source/katana/models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf
K=/mnt/dados/source/katana/katana
prev=""
for i in 1 2 3 4 5; do
  [ -e STOP ] && { echo "STOP antes do trecho $i" >> v6-train.log; exit 0; }
  r=""; [ -n "$prev" ] && r="--resume $prev"
  echo "== trecho $i $(date +%H:%M)" >> v6-train.log
  flock /tmp/katana-train-measure.lock $K train $M v6-data.jsonl --train 'blk.*.attn_*' --rank 8 --seq 512 --lr 1e-4 --sched const \
    --batch 4 --steps 60 --val 0 --final 0 --every 5 --no-tool-decl --seed $i $r --out v6-c$i.kspec >> v6-train.log 2>&1 || { echo "trecho $i falhou" >> v6-train.log; exit 1; }
  prev=v6-c$i.kspec
done
cp "$prev" v6.kspec; [ -e "$prev.kopt" ] && cp "$prev.kopt" v6.kspec.kopt
echo "== fim $(date +%H:%M)" >> v6-train.log; echo done > v6-train-done.txt
