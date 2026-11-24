#!/bin/sh
# Equivalencia da decodificacao especulativa por copia (src/engine/lookup.c): em temperatura 0 a saida com
# KATANA_LOOKUP=1 tem de ser byte a byte a de sem ela. Uso: sh tools/spectest.sh <modelo.gguf> [max_tokens] [prompt.txt...]
# Sem prompts, usa dois embutidos (codigo repetitivo e texto livre). Roda na CPU (KATANA_CPU=1), que e o caminho em lote.
# Sem modelo, roda so o lookuptest no modelo minusculo (tools/tinymodel). Sai com 0 se tudo igual.
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
st=0
if [ -z "$1" ]; then
  python3 tools/tinymodel/make_tiny_gguf.py "$T/tiny.gguf" --arch qwen35moe >/dev/null || exit 1
  for f in 0 1; do
    KATANA_CPU=1 KATANA_LOOKUP_FORCED=$f ./katana lookuptest "$T/tiny.gguf" 300 2>&1 | tail -2
    KATANA_CPU=1 KATANA_LOOKUP_FORCED=$f ./katana lookuptest "$T/tiny.gguf" 300 >/dev/null 2>&1 || { echo "FALHOU (forced=$f)"; st=1; }
  done
  exit $st
fi
M=$1; N=${2:-200}; shift; shift
if [ $# -eq 0 ]; then
  printf 'int add(int a, int b) { return a + b; }\nint sub(int a, int b) { return a - b; }\nRepeat the two functions above exactly, then add a mul function.' > "$T/p1.txt"
  printf 'Write three sentences about why the sky is blue.' > "$T/p2.txt"
  set -- "$T/p1.txt" "$T/p2.txt"
fi
for p in "$@"; do
  KATANA_CPU=1 KATANA_THINK=0 ./katana chat "$M" "@$p" "$N" 0 >"$T/a.out" 2>"$T/a.err"
  KATANA_CPU=1 KATANA_THINK=0 KATANA_LOOKUP=1 ./katana chat "$M" "@$p" "$N" 0 >"$T/b.out" 2>"$T/b.err"
  if cmp -s "$T/a.out" "$T/b.out"; then echo "IGUAL  $p  $(grep -h '^\[lookup' "$T/b.err" | cut -c1-90)"; else echo "DIFERENTE  $p"; st=1; fi
done
exit $st
