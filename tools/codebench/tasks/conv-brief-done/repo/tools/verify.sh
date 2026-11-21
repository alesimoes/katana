#!/bin/sh
# the project's definition of done: tests pass, then the state of src/ is stamped
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
python3 -m unittest discover -s tests || exit 1
python3 tools/stamp.py || exit 1
echo "verified"
