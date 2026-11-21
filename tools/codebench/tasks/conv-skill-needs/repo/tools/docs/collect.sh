#!/bin/sh
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG="$ROOT/.katana/state/skill.log"
mkdir -p "$ROOT/.katana/state"
mkdir -p "$ROOT/docs/build"
cat "$ROOT"/docs/src/*.md > "$ROOT/docs/build/collected.txt"
echo collect >> "$LOG"
echo collected
