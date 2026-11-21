#!/bin/sh
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG="$ROOT/.katana/state/skill.log"
mkdir -p "$ROOT/.katana/state"
[ "$(tail -n 1 "$LOG" 2>/dev/null)" = "bump" ] || { echo "run step 'bump' first" >&2; exit 1; }
echo "# $(cat "$ROOT/VERSION")" > "$ROOT/RELEASE_NOTES.md"
echo notes >> "$LOG"
echo "notes written"
