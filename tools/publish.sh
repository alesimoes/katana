#!/bin/sh
# Publishes the katana of this working copy as the installed one: build, checks, then an atomic swap.
# sh tools/publish.sh [--model M.gguf]   (with a model, also runs its selftest). Sessions already open keep the old program.
# The previous programs stay in ~/.katana/versions (last 5); sh tools/publish.sh --rollback goes back one.
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)"); DST=$ROOT/katana; KEEP=$HOME/.katana/versions
mkdir -p "$KEEP"
swap() { cp "$1" "$DST.new" && chmod 755 "$DST.new" && mv -f "$DST.new" "$DST"; }
if [ "${1:-}" = --rollback ]; then
  L=$(ls -t "$KEEP" | sed -n 1p); [ -n "$L" ] || { echo "no previous version"; exit 1; }
  swap "$KEEP/$L" && rm -f "$KEEP/$L" && "$DST" version; exit
fi
MODEL=; [ "${1:-}" = --model ] && MODEL=${2:-}
sh build.sh > /dev/null 2> "$KEEP/../publish.log" || { echo "build failed: ~/.katana/publish.log"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
./katana calltest > "$T/o" 2>&1 || { echo "calltest failed"; tail -3 "$T/o"; exit 1; }
HOME=$T ./katana compacttest > "$T/o" 2>&1 || { echo "compacttest failed"; tail -3 "$T/o"; exit 1; }
for t in files tasks meta misc extend agents; do
  nice -n 10 timeout 300 sh tools/tooltest/$t.sh ./katana > "$T/o" 2>&1 || { echo "$t failed"; grep FAIL "$T/o" | head -5; exit 1; }
done
if [ -n "$MODEL" ]; then timeout 600 ./katana selftest "$MODEL" > "$T/o" 2>&1; grep -q "argmax" "$T/o" && ! grep -q FALH "$T/o" || { echo "selftest failed"; tail -5 "$T/o"; exit 1; }; fi
[ -x "$DST" ] && cp -p "$DST" "$KEEP/katana-$(date +%Y%m%d-%H%M%S)"
ls -t "$KEEP" | sed -n '6,$p' | while read -r f; do rm -f "$KEEP/$f"; done
swap ./katana && echo "published: $("$DST" version)"
