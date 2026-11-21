#!/bin/sh
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LOG="$ROOT/.katana/state/skill.log"
mkdir -p "$ROOT/.katana/state"
v=$1; c=$2
[ -f "$ROOT/dist/app-$v.pack" ] || { echo "pack $v first" >&2; exit 1; }
case "$c" in stable|beta|nightly) ;; *) echo "bad channel" >&2; exit 2 ;; esac
echo "push version=$v channel=$c" >> "$LOG"
echo "pushed $v to $c"
