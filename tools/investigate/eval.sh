#!/usr/bin/env bash
# Bancada de avaliacao da investigacao de codigo: roda `katana -p` em cada pergunta de questions.json e mede.
#   eval.sh [id ...]            sem ids, todas as perguntas
# Variaveis: TOOLS (padrao Read,Grep,Glob; com Find: TOOLS=Read,Grep,Glob,Find), TIMEOUT (segundos, padrao 900),
#   MAXTURNS (30), EVAL_OUT (pasta dos registros; padrao ./eval-out/<data>), INVEST_REPOS (copias), KATANA (binario).
# Uma linha por pergunta: id, chamadas, tokens de entrada somados, maior contexto, repetidas, veredito, arquivos, palavras, segundos.
# Os arquivos <id>.out (resposta), .err (progresso) e .log (registro das ferramentas) ficam em EVAL_OUT.
here="$(cd "$(dirname "$0")" && pwd)"
KATANA="${KATANA:-katana}"
TOOLS="${TOOLS:-Read,Grep,Glob}"
TIMEOUT="${TIMEOUT:-900}"
MAXTURNS="${MAXTURNS:-30}"
OUT="${EVAL_OUT:-$PWD/eval-out/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
cd "$here" || exit 1
ids=("$@")
[ ${#ids[@]} -eq 0 ] && mapfile -t ids < <(python3 -c "import json;print('\n'.join(q['id'] for q in json.load(open('questions.json'))))")
printf 'id\tchamadas\ttokens_entrada\tmaior_ctx\trepetidas\tveredito\tarquivos\tpalavras\tsegundos\n' | tee "$OUT/summary.tsv"
for id in "${ids[@]}"; do
  wt="$(python3 setup_repos.py "$id" | awk '{print $2}')"
  q="$(python3 -c "import json,sys;print([q for q in json.load(open('questions.json')) if q['id']==sys.argv[1]][0]['question'])" "$id")"
  rm -f "$OUT/$id.log"
  t0=$(date +%s)
  # timeout guarda o PID do filho; nada de pkill/pgrep por texto
  KATANA_LOG="$OUT/$id.log" timeout "$TIMEOUT" "$KATANA" -p "$q" --cwd "$wt" --allowed-tools "$TOOLS" \
      --max-turns "$MAXTURNS" --no-session > "$OUT/$id.out" 2> "$OUT/$id.err"
  rc=$?
  secs=$(( $(date +%s) - t0 ))
  [ $rc -eq 124 ] && echo "[timeout de ${TIMEOUT} s]" >> "$OUT/$id.err"
  python3 score.py "$id" "$OUT/$id.log" "$OUT/$id.err" "$OUT/$id.out" "$secs" | tee -a "$OUT/summary.tsv"
done
