#!/bin/sh
# usage: sh tools/check.sh --suite fast|full
ROOT=$(cd "$(dirname "$0")/.." && pwd)
suite=""
[ "$1" = "--suite" ] && suite=$2
case "$suite" in
  fast) pat='test_fast*.py' ;;
  full) pat='test_*.py' ;;
  *) echo "usage: sh tools/check.sh --suite fast|full" >&2; exit 2 ;;
esac
cd "$ROOT" || exit 1
python3 -m unittest discover -s tests -p "$pat" || exit 1
mkdir -p .katana/state
echo "$suite" > .katana/state/tests-ran
echo "suite $suite ok"
