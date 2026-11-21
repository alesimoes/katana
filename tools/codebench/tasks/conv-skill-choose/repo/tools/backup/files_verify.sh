#!/bin/sh
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG="$ROOT/.katana/state/skill.log"
mkdir -p "$ROOT/.katana/state"
tar tf "$ROOT/backups/uploads.tar" > /dev/null || exit 1
echo files-verify >> "$LOG"
echo "files backup verified"
