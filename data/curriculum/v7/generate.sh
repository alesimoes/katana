#!/bin/sh
# v7: regenerates EVERYTHING with the binary at hand (the families of v1 to v6 and the five new ones); nothing is copied from the v6 files.
# usage: sh generate.sh <katana binary> <work folder>      (the finished files are written next to this script)
# The files depend on the generator's source, on the engine's prompt and tools, and on the vocabulary of the model (KATANA_VOCAB_MODEL,
# used by the x-docs family): a binary of another commit will not give the same bytes.
B=${1:-./katana}; W=${2:-./v7-work}
HERE=$(cd "$(dirname "$0")" && pwd)
export KATANA_VOCAB_MODEL=${KATANA_VOCAB_MODEL:-/mnt/dados/source/katana/models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf}
B=$(cd "$(dirname "$B")" && pwd)/$(basename "$B")
mkdir -p "$W" && cd "$W" || exit 1
SYS=mix:blocks=88,real=12
# the families of v1 to v6 with the weights of v6 (unchanged, so their proportions stay the same)
OLD=x-fix=24,x-add=16,x-rename=14,x-find=10,x-explain=10,x-run=8,x-config=24,x-clean=18,x-investigate=12,x-web=26,x-concept=58,x-tools=36,x-skill=10,x-rare=95,x-ps1=22,x-mcp=60,x-ability=30,x-make=40,x-docs=30,x-find6=30,x-batch=20,x-script=20
# the new families of v7
NEW=x-symbols=40,x-find7=25,x-place=20,x-cursor=15,x-katananotes=15

# 1. pools and holdouts (same recipe as v3 to v6: pool, selection by coverage, holdout with reserved slot values)
$B curriculum v7-pool-old.jsonl --n 1800 --seed 2026 --families $OLD --system $SYS > v7-gen-old.txt 2>&1
$B curriculum v7-hold-tmp.jsonl --n 1900 --seed 2026 --families $OLD --system $SYS --holdout v7-hold-old.jsonl > v7-gen-hold-old.txt 2>&1
$B curriculum v7-pool-new.jsonl --n 800 --seed 2027 --families $NEW --system $SYS > v7-gen-new.txt 2>&1
$B curriculum v7-hold-tmp2.jsonl --n 1 --seed 2027 --families $NEW --system $SYS --holdout v7-hold-new.jsonl > v7-gen-hold-new.txt 2>&1

# 2. the old families: 520 by coverage, plus extra conversations of the v6 families that pick takes few of (same as v6)
$B curriculum pick v7-pool-old.jsonl v7-pick-old.jsonl --n 520 > v7-pick.txt 2>&1
cp v7-pick-old.jsonl v7-raw-old.jsonl
for fam in x-find6:90 x-batch:60 x-script:50 x-docs:20; do
  name=${fam%%:*}; n=${fam##*:}
  grep "\"family\": \"$name\"" v7-pool-old.jsonl | grep -vxF -f v7-pick-old.jsonl | head -n "$n" >> v7-raw-old.jsonl
done
# 3. the new families: a fixed count each, taken from the pool in order
: > v7-raw-new.jsonl
for fam in x-symbols:110 x-find7:70 x-place:60 x-cursor:40 x-katananotes:50; do
  name=${fam%%:*}; n=${fam##*:}
  grep "\"family\": \"$name\"" v7-pool-new.jsonl | head -n "$n" >> v7-raw-new.jsonl
done
cat v7-raw-old.jsonl v7-raw-new.jsonl > v7-train.jsonl
cat v7-hold-old.jsonl v7-hold-new.jsonl > v7-validation.jsonl

# 4. check: the engine runs every conversation again against the real tools and compares it byte for byte.
# A few conversations (about 0.2%) are not repeatable because a real tool orders files by modification time and two files were written in the
# same instant (a Grep that lists two files): the lines the check reports are dropped (and listed in dropped.txt), then the file is checked again.
: > dropped.txt
settle() {
  f=$1
  for pass in 1 2 3; do
    $B curriculum check "$f" > "$f.chk" 2>&1
    bad=$(grep -o '^linha [0-9]*' "$f.chk" | awk '{print $2}')
    [ -z "$bad" ] && break
    echo "$f pass $pass: $(echo $bad)" >> dropped.txt
    echo "$bad" > bad.txt
    awk 'NR==FNR { b[$1] = 1; next } !(FNR in b)' bad.txt "$f" > "$f.tmp" && mv "$f.tmp" "$f"
  done
  $B curriculum check "$f" > "$f.chk" 2>&1
}
settle v7-train.jsonl; cp v7-train.jsonl.chk check-train.txt
settle v7-validation.jsonl; cp v7-validation.jsonl.chk check-validation.txt
$B curriculum variety v7-train.jsonl v7-validation.jsonl > variety.txt 2>&1
# 5. per family counts and the sha256 of the two data files
for f in v7-train v7-validation; do
  echo "$f"; grep -o '"family": "[^"]*"' $f.jsonl | sort | uniq -c | sort -rn
done > family-counts.txt
gzip -9n -c v7-train.jsonl > "$HERE/v7-train.jsonl.gz"
gzip -9n -c v7-validation.jsonl > "$HERE/v7-validation.jsonl.gz"
cp check-train.txt check-validation.txt family-counts.txt variety.txt dropped.txt "$HERE/"
( cd "$HERE" && sha256sum v7-train.jsonl.gz v7-validation.jsonl.gz generate.sh train.sh > SHA256SUMS )
echo done > v7-done.txt
