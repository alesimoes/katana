#!/bin/sh
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LOG="$ROOT/.katana/state/skill.log"
mkdir -p "$ROOT/.katana/state"
c=$1
grep -q "channel=$c" "$LOG" 2>/dev/null || { echo "nothing pushed to $c" >&2; exit 1; }
echo "status channel=$c" >> "$LOG"
echo "$c ok"
