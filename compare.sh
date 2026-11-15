#!/bin/bash
# compare.sh <modelo.gguf> <dir do llama.cpp> [rodadas] [tokens]
# Alterna katana e llama.cpp no mesmo modelo e na mesma pergunta (formato ChatML), com
# amostragem gulosa, e mostra a velocidade de cada rodada. Alternar tira da conta o
# aquecimento da maquina, que muda as medidas ao longo do tempo.
model="$1"; ll="$2"; n="${3:-3}"; tok="${4:-200}"
q="${KATANA_Q:-Explain how a hash table works and write a C implementation with insert and lookup. /no_think}"
here="$(cd "$(dirname "$0")" && pwd)"
for i in $(seq 1 "$n"); do
  k=$("$here/katana.exe" chat "$model" "$q" "$tok" 0 2>&1 | grep '^\[prompt' | sed 's/.*prompt: [0-9]* tokens, \([0-9.]*\) tok.*geracao: [0-9]* tokens, \([0-9.]*\) tok.*/prompt \1  geracao \2/')
  l=$("$ll/llama-completion.exe" -m "$model" -ngl 99 -ot "exps=CPU" -no-cnv --temp 0 -n "$tok" -p "<|im_start|>user
$q<|im_end|>
<|im_start|>assistant
" 2>&1 | grep 'eval time' | sed 's/.*( *[0-9.]* ms per token, *\([0-9.]*\) tokens per second.*/\1/' | tr '\n' ' ' | awk '{print "prompt " $1 "  geracao " $2}')
  echo "rodada $i | katana: $k | llama.cpp: $l | GPU $(nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader) C"
done
