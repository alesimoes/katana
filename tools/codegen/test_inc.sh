#!/bin/sh
# Testes da AST incremental (docs/codegen.md): compila tools/codegen/codegencli.c e roda tools/codegen/test_inc.py (precisa de python3).  SAN=1 liga ASan/UBSan.
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
T=$(mktemp -d) || exit 2; trap 'rm -rf "$T"' EXIT
CC=${CC:-gcc}; SANF=""; [ -n "$SAN" ] && SANF="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC -O1 -g -Wall -Wno-unused-function $SANF -I "$ROOT" "$HERE/codegencli.c" -o "$T/codegencli" -lm 2>"$T/cc.log" || { cat "$T/cc.log"; echo "FAIL build"; exit 1; }
[ -s "$T/cc.log" ] && { cat "$T/cc.log"; echo "FAIL build warnings"; exit 1; }
python3 "$HERE/test_inc.py" "$T/codegencli"
