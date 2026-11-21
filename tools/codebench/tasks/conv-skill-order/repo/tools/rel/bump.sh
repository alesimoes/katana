#!/bin/sh
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG="$ROOT/.katana/state/skill.log"
mkdir -p "$ROOT/.katana/state"
[ "$(tail -n 1 "$LOG" 2>/dev/null)" = "check" ] || { echo "run step 'check' first" >&2; exit 1; }
echo 1.5.0 > "$ROOT/VERSION"
echo bump >> "$LOG"
echo "bumped to 1.5.0"
