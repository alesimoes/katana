#!/bin/sh
# v6: pool, selecao por cobertura, holdout e check (mesma receita da v3 a v5); vocabulario so para o x-docs
export KATANA_VOCAB_MODEL=/mnt/dados/source/katana/models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf
B=./k6
$B curriculum v6-pool.jsonl --n 1800 --seed 2026 --system mix:blocks=88,real=12 > v6-gen.txt 2>&1
$B curriculum v6-holdout-tmp.jsonl --n 1900 --seed 2026 --holdout v6-holdout-raw.jsonl --system mix:blocks=88,real=12 > v6-genh.txt 2>&1
$B curriculum pick v6-pool.jsonl v6-raw.jsonl --n 520 > v6-pick.txt 2>&1
$B curriculum check v6-raw.jsonl > v6-check.txt 2>&1
$B curriculum check v6-holdout-raw.jsonl > v6-holdout-check.txt 2>&1
echo done > v6-done.txt
