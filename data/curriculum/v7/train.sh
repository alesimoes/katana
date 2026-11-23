#!/bin/sh
# training of v7 on Qwen3.6: CHUNKS chunks of STEPS steps each (batch 4); every chunk continues from the previous one (--resume, with the optimizer state).
# usage: sh train.sh [chunks] [steps per chunk]       (default 5 x 60 = 300 steps = 1200 windows, as in v6; NOT run by whoever wrote it)
# A file STOP in the work folder cancels between chunks. Same hyper-parameters as v6.
CHUNKS=${1:-5}; STEPS=${2:-60}
cd /mnt/dados/cu-v7 || exit 1
export KATANA_THINK=0
M=/mnt/dados/source/katana/models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf
K=/mnt/dados/source/katana/katana
# the import depends on the model's tokenizer and chat format
[ -s v7-data.jsonl ] || { gunzip -c v7-train.jsonl.gz > v7-train.jsonl && $K import $M v7-train.jsonl v7-data.jsonl --system keep --mask-prefill; } || exit 1
prev=""
i=1
while [ "$i" -le "$CHUNKS" ]; do
  [ -e STOP ] && { echo "STOP before chunk $i" >> v7-train.log; exit 0; }
  r=""; [ -n "$prev" ] && r="--resume $prev"
  echo "== chunk $i $(date +%H:%M)" >> v7-train.log
  flock /tmp/katana-train-measure.lock $K train $M v7-data.jsonl --train 'blk.*.attn_*' --rank 8 --seq 512 --lr 1e-4 --sched const \
    --batch 4 --steps "$STEPS" --val 0 --final 0 --every 5 --no-tool-decl --seed $i $r --out v7-c$i.kspec >> v7-train.log 2>&1 || { echo "chunk $i failed" >> v7-train.log; exit 1; }
  prev=v7-c$i.kspec
  i=$((i + 1))
done
cp "$prev" v7.kspec; [ -e "$prev.kopt" ] && cp "$prev.kopt" v7.kspec.kopt
echo "== end $(date +%H:%M)" >> v7-train.log; echo done > v7-train-done.txt
