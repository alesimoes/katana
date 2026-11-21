#!/bin/sh
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG="$ROOT/.katana/state/skill.log"
mkdir -p "$ROOT/.katana/state"
[ "$(tail -n 1 "$LOG" 2>/dev/null)" = "notes" ] || { echo "run step 'notes' first" >&2; exit 1; }
mkdir -p "$ROOT/dist"; touch "$ROOT/dist/READY"
echo seal >> "$LOG"
echo "sealed"
