#!/bin/sh
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$ROOT/.katana/state"
echo regen >> "$ROOT/.katana/state/regen-ran"
python3 "$ROOT/tools/seed.py" > "$ROOT/data/fixtures/catalog.json"
