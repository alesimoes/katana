#!/bin/sh
# Training on the working model: CHUNKS chunks of STEPS steps each (batch 4); every chunk continues from the previous one (--resume, with the
# optimizer state). usage: sh train.sh <folder made by generate.sh> [chunks] [steps per chunk]   (default 5 x 60 = 300 steps = 1200 windows)
# Environment: KATANA_MODEL (the .gguf to specialize), KATANA_BIN (default: ./katana). A file STOP in the folder cancels between chunks.
D=${1:?folder made by generate.sh}; CHUNKS=${2:-5}; STEPS=${3:-60}
M=${KATANA_MODEL:?set KATANA_MODEL to the model .gguf}; K=${KATANA_BIN:-./katana}
K=$(cd "$(dirname "$K")" && pwd)/$(basename "$K")
cd "$D" || exit 1
export KATANA_THINK=0
# the import depends on the model's tokenizer and chat format
[ -s data.jsonl ] || { gunzip -c train.jsonl.gz > train.jsonl && $K import "$M" train.jsonl data.jsonl --system keep --mask-prefill; } || exit 1
prev=""
i=1
while [ "$i" -le "$CHUNKS" ]; do
  [ -e STOP ] && { echo "STOP before chunk $i" >> train.log; exit 0; }
  r=""; [ -n "$prev" ] && r="--resume $prev"
  echo "== chunk $i $(date +%H:%M)" >> train.log
  flock /tmp/katana-train-measure.lock $K train "$M" data.jsonl --train 'blk.*.attn_*' --rank 8 --seq 512 --lr 1e-4 --sched const \
    --batch 4 --steps "$STEPS" --val 0 --final 0 --every 5 --no-tool-decl --seed $i $r --out c$i.kspec >> train.log 2>&1 || { echo "chunk $i failed" >> train.log; exit 1; }
  prev=c$i.kspec
  i=$((i + 1))
done
cp "$prev" model.kspec; [ -e "$prev.kopt" ] && cp "$prev.kopt" model.kspec.kopt
echo "== end $(date +%H:%M)" >> train.log; echo done > train-done.txt
