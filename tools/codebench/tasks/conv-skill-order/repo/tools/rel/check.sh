#!/bin/sh
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG="$ROOT/.katana/state/skill.log"
mkdir -p "$ROOT/.katana/state"
echo check >> "$LOG"
echo "check ok"
