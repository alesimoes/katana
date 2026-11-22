#!/bin/sh
# qwen4exp: estados por camada e logits do katana contra a referencia em Python puro (qwen4exp_ref.py), sem modelo de verdade.
# Sementes e tamanhos, caches KV fp32/fp16/Q8_0, e mutacoes da referencia que o teste tem que acusar.
# Uso: sh tools/tinymodel/qwen4exp_test.sh   (da raiz do repositorio, com ./katana compilado)
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
export KATANA_CPU=1
fail=0
chk() { ./katana q4check "$D/q4-00001-of-00002.gguf" "$D/ref.bin" 2>/dev/null | tail -1 | grep -q "q4check: OK"; }
for c in "1 72" "2 72" "3 72" "4 9" "5 200"; do
  set -- $c
  python3 tools/tinymodel/qwen4exp_ref.py "$D" --seed "$1" --n "$2" >/dev/null || fail=1
  if chk; then echo "semente $1, $2 tokens: ok"; else echo "semente $1, $2 tokens: FALHOU"; fail=1; fi
done
python3 tools/tinymodel/qwen4exp_ref.py "$D" --seed 1 >/dev/null
if KATANA_CPU_KV16=1 chk; then echo "cache fp16: ok"; else echo "cache fp16: FALHOU"; fail=1; fi
# todas as regras da referencia, uma errada por vez: o teste precisa falhar
for m in nosparse rowpos noeos silu nodilation meaninject notile norelu; do
  python3 tools/tinymodel/qwen4exp_ref.py "$D" --seed 1 --mutate $m >/dev/null || fail=1
  if chk; then echo "mutacao $m: passou (FALHOU)"; fail=1; else echo "mutacao $m: acusada, ok"; fi
done
# partes: a segunda sozinha e a primeira sem a segunda tem que dar erro claro
./katana info "$D/q4-00002-of-00002.gguf" 2>&1 | grep -q "abra a primeira" && echo "parte 2 sozinha: erro claro, ok" || { echo "parte 2 sozinha: FALHOU"; fail=1; }
rm "$D/q4-00002-of-00002.gguf"
./katana q4check "$D/q4-00001-of-00002.gguf" "$D/ref.bin" 2>&1 | grep -q "Nao consegui abrir.*00002" && echo "sem a parte 2: erro claro, ok" || { echo "sem a parte 2: FALHOU"; fail=1; }
exit $fail
