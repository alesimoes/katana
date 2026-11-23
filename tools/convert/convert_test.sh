#!/bin/sh
# katana convert: confere a conversao de checkpoints minusculos contra o gerador de GGUF (tools/tinymodel), sem modelo de verdade.
# Uso: sh tools/convert/convert_test.sh   (da raiz do repositorio, com ./katana compilado)
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
export KATANA_CPU=1
fail=0
# nome, opcoes do checkpoint HF, opcoes do gerador (mesma semente e arquitetura), tolerancia de ssm_a
run() {
  n=$1; hfopt=$2; genopt=$3; tol=$4
  python3 -I tools/tinymodel/make_tiny_gguf.py "$D/$n.ref.gguf" --pre qwen35 $genopt >/dev/null || { fail=1; return; }
  python3 -I tools/convert/make_tiny_hf.py "$D/$n.hf" $hfopt >/dev/null || { fail=1; return; }
  ./katana convert "$D/$n.hf" "$D/$n.gguf" f32 >/dev/null 2>&1 || { echo "$n: convert falhou"; fail=1; return; }
  if python3 -I tools/convert/compare_gguf.py "$D/$n.ref.gguf" "$D/$n.gguf" --tol "$tol" | grep -q " 0 problem"; then echo "$n: ok"; else echo "$n: FALHOU"; fail=1; fi
}
run moe-f32    "--arch qwen35moe --seed 1 --decoys" "--arch qwen35moe --seed 1" 1e-6
run moe-shards "--arch qwen35moe --seed 2 --per-expert --shards 5 --merge-pairs --template-file" "--arch qwen35moe --seed 2" 1e-6
run moe-bf16   "--arch qwen35moe --seed 3 --dtype bf16 --shards 3" "--arch qwen35moe --seed 3 --round bf16" 1e-2
run moe-f16    "--arch qwen35moe --seed 4 --dtype f16 --per-expert" "--arch qwen35moe --seed 4 --round f16" 1e-2
run dense      "--arch qwen35 --seed 5" "--arch qwen35 --seed 5" 1e-6
# o motor carrega e gera nos outros tipos de saida
for ty in f16 bf16 q8_0 q4_k_m; do
  ./katana convert "$D/moe-f32.hf" "$D/o.$ty.gguf" $ty >/dev/null 2>&1 || { echo "tipo $ty: convert falhou"; fail=1; continue; }
  if ./katana run "$D/o.$ty.gguf" "hello there" 8 0 2>&1 | grep -q "geracao: 8 tokens"; then echo "tipo $ty: carrega e gera, ok"; else echo "tipo $ty: FALHOU"; fail=1; fi
done
[ $fail = 0 ] && echo "convert_test: OK" || echo "convert_test: FALHOU"
exit $fail
