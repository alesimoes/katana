#!/bin/sh
# Comparison of fine-tuning methods on a tiny model with a learnable synthetic task (seconds per run, any machine).
#
#   sh tools/traintest/methods.sh [OUTDIR]     (from the repository root, with ./katana built)
#
# 1. a tiny llama base is trained on general text (so it has something to forget);
# 2. each method trains rank-8 adapters on the same task data, 3 seeds, same steps/batch/window (equal training tokens);
#    the file kept is the state of the LAST step (--eval larger than --steps: one validation at the end), except that
#    --ema picks between raw and averaged weights on val.jsonl;
# 3. `katana compare` measures every result on test.jsonl (never seen in training) and on drift.jsonl (general text):
#    held-out loss, loss on tool-call tokens, drift loss and mean KL(base || specialization) on the drift file;
# 4. tools/traintest/methods_table.py prints the mean and spread over the seeds.
# Real models are not touched. Results here say what happens on a 0.1M-parameter model, not on the real ones.
set -e
set -f
K=$(pwd)/katana
D=${1:-$(mktemp -d)}
mkdir -p "$D"
echo "work dir: $D"
python3 tools/tinymodel/make_tiny_gguf.py "$D/tiny.gguf" >/dev/null
python3 tools/traintest/methods_data.py "$D"
export KATANA_TRAIN_GPU=0
$K train "$D/tiny.gguf" "$D/pretrain.jsonl" --val 0 --train all --steps 400 --lr 3e-3 --batch 4 --seq 64 --final 0 --out "$D/pre.kspec" >"$D/pretrain.log" 2>&1
$K merge "$D/tiny.gguf" "$D/pre.kspec" "$D/base.gguf" >/dev/null 2>&1
M="$D/base.gguf"
STEPS=${STEPS:-300}
COMMON="--val-data $D/val.jsonl --rank 8 --train blk.* --batch 4 --steps $STEPS --lr 3e-3 --seq 64 --eval 100000 --allow-worse --final 0"
run() {   # nome, opcoes extras
  name=$1; shift
  for seed in 1 2 3; do
    $K train $M $D/train.jsonl $COMMON --seed $seed "$@" --out "$D/$name.s$seed.kspec" >"$D/$name.s$seed.log" 2>&1 || echo "falhou: $name seed $seed"
  done
  $K compare $M --val-data $D/test.jsonl --drift-data $D/drift.jsonl --seq 64 "$D/$name.s1.kspec" "$D/$name.s2.kspec" "$D/$name.s3.kspec" 2>/dev/null \
    | grep "^compare-json" | sed "s/^compare-json: /$name /" >> "$D/results.txt"
}
: > "$D/results.txt"
run lora
run ema0.9 --ema 0.9
run neftune5 --neft 5
run neftune15 --neft 15
run dropout0.1 --adapter-dropout 0.1
run dropout0.3 --adapter-dropout 0.3
run anchor0.5 --anchor 0.5
run anchor2 --anchor 2
run smooth0.1 --label-smoothing 0.1
run toolw3 --tool-weight 3
run dora --dora
run muon --optimizer muon
run muon_lr3 --optimizer muon --lr 1e-3
run pissa --pissa
run pissa_lr3 --pissa --lr 1e-3
python3 tools/traintest/methods_table.py "$D/results.txt"
