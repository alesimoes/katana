#!/bin/sh
# Regenerates the whole training curriculum from zero with the binary at hand: every family, the default weights of the binary.
# usage: sh generate.sh <katana binary> <output folder> [conversations (default 3000)] [seed (default 2026)]
# Writes train.jsonl.gz, validation.jsonl.gz, the check reports, the per family counts and SHA256SUMS into <output folder>.
# The files depend on the generator's source, on the engine's prompt and tools, and on the vocabulary of the model (KATANA_VOCAB_MODEL,
# used by the x-docs family): a binary of another commit will not give the same bytes. Nothing here names a version.
B=${1:?binary}; OUT=${2:?output folder}; N=${3:-3000}; SEED=${4:-2026}
mkdir -p "$OUT" || exit 1
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(cd "$OUT" && pwd)
B=$(cd "$(dirname "$B")" && pwd)/$(basename "$B")
[ -n "$KATANA_VOCAB_MODEL" ] || { echo "set KATANA_VOCAB_MODEL to the model .gguf (its vocabulary is read by the x-docs family)" >&2; exit 1; }
W=$OUT/work; mkdir -p "$W" && cd "$W" || exit 1
SYS=mix:blocks=88,real=12

# 1. the pool and the holdout (values and cells the training file never has)
$B curriculum pool.jsonl --n "$N" --seed "$SEED" --system $SYS --holdout holdout.jsonl > gen.txt 2>&1 || { echo "generation failed, see $W/gen.txt" >&2; exit 1; }
# 2. selection by coverage of the pool (every tool of the table reaches its minimum first); without it the whole pool is the training file
$B curriculum pick pool.jsonl train.jsonl --n $((N * 2 / 3)) > pick.txt 2>&1 || cp pool.jsonl train.jsonl
cp holdout.jsonl validation.jsonl

# 3. check: the engine runs every conversation again against the real tools and compares it byte for byte.
# A few conversations (about 0.2%) are not repeatable because a real tool orders files by modification time and two files were written in the
# same instant: the lines the check reports are dropped (and listed in dropped.txt), then the file is checked again.
: > dropped.txt
settle() {
  f=$1
  for pass in 1 2 3 4 5 6; do
    $B curriculum check "$f" > "$f.chk" 2>&1
    bad=$(grep -o '^linha [0-9]*' "$f.chk" | awk '{print $2}')
    [ -z "$bad" ] && break
    echo "$f pass $pass: $(echo $bad)" >> dropped.txt
    echo "$bad" > bad.txt
    awk 'NR==FNR { b[$1] = 1; next } !(FNR in b)' bad.txt "$f" > "$f.tmp" && mv "$f.tmp" "$f"
  done
  $B curriculum check "$f" > "$f.chk" 2>&1
}
settle train.jsonl; cp train.jsonl.chk check-train.txt
settle validation.jsonl; cp validation.jsonl.chk check-validation.txt
$B curriculum check train.jsonl --coverage > coverage.txt 2>&1
$B curriculum variety train.jsonl validation.jsonl > variety.txt 2>&1

# 4. per family counts and the sha256 of the two data files
for f in train validation; do
  echo "$f"; grep -o '"family": "[^"]*"' $f.jsonl | sort | uniq -c | sort -rn
done > family-counts.txt
gzip -9n -c train.jsonl > "$OUT/train.jsonl.gz"
gzip -9n -c validation.jsonl > "$OUT/validation.jsonl.gz"
cp check-train.txt check-validation.txt coverage.txt family-counts.txt variety.txt dropped.txt "$OUT/"
cp "$HERE/generate.sh" "$HERE/train.sh" "$OUT/"
( cd "$OUT" && sha256sum train.jsonl.gz validation.jsonl.gz generate.sh train.sh > SHA256SUMS )
echo done > "$W/done.txt"
