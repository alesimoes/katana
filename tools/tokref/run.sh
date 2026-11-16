#!/bin/sh
# Teste dourado: gera a referencia e compara com o `katana toktest` (compilar antes: sh build.sh).
# Uso: sh tools/tokref/run.sh [--bin ./katana] [--detalhe N] [modelo ...]
cd "$(dirname "$0")/../.." || exit 1
python3 -I tools/tokref/ref.py || exit 1
python3 -I tools/tokref/compare.py "$@"
