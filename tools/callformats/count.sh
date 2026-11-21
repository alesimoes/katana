#!/bin/sh
# Conta os tokens de cada arquivo gerado por render_calls.py com o tokenizador de cada modelo (sem carregar os pesos).
# Uso: sh tools/callformats/count.sh <pasta> <modelo.gguf>...
d=$1; shift
for m in "$@"; do
  echo "# $(basename "$m")"
  for f in "$d"/*.txt; do
    n=$(./katana tokenize "$m" "$f" 2>/dev/null | head -1 | cut -d' ' -f1)
    echo "$(basename "$f" .txt) $n"
  done
done
