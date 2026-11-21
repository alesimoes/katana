#!/bin/sh
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LOG="$ROOT/.katana/state/skill.log"
mkdir -p "$ROOT/.katana/state"
v=$1
case "$v" in [0-9]*.[0-9]*.[0-9]*) ;; *) echo "bad version" >&2; exit 2 ;; esac
mkdir -p "$ROOT/dist"
echo "pack $v" > "$ROOT/dist/app-$v.pack"
echo "pack version=$v" >> "$LOG"
echo "packed $v"
