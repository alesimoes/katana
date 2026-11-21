#!/bin/sh
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG="$ROOT/.katana/state/skill.log"
mkdir -p "$ROOT/.katana/state"
mkdir -p "$ROOT/backups"
tar cf "$ROOT/backups/uploads.tar" -C "$ROOT" uploads || exit 1
echo files-tar >> "$LOG"
echo "archived"
