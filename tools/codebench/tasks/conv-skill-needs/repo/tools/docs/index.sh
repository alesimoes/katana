#!/bin/sh
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG="$ROOT/.katana/state/skill.log"
mkdir -p "$ROOT/.katana/state"
mkdir -p "$ROOT/docs/build"
grep -h '^# ' "$ROOT"/docs/src/*.md > "$ROOT/docs/build/index.txt"
echo index >> "$LOG"
echo indexed
