#!/bin/sh
# Treina o modelo local com as respostas que o professor deu no agente (modo professor), sem ninguem olhando:
#   1. espera o agente do desktop ficar parado (nenhuma chamada ao professor e a sessao sem gravar ha 10 minutos);
#   2. katana import --teacher: as sessoes do desktop viram dados com as ferramentas atuais, a perda so nas falas do professor;
#   3. mede o tempo de um passo de treino neste modelo e escolhe quantos passos cabem no tempo dado;
#   4. katana train com adaptadores de posto baixo so na atencao (roda na GPU depois, nao mexe nos especialistas);
#   5. confere que o modelo com a especializacao carrega e responde (na CPU, para nao disputar a placa);
#   6. grava tudo, com um relatorio, na pasta de saida. Nao aplica a especializacao em lugar nenhum.
# Uso: sh tools/train_from_teacher.sh [modelo.gguf] [horas de treino, padrao 5] [pasta de saida]
# Variaveis: WAIT_H (limite da espera, padrao 6), NO_WAIT=1 (nao espera), RANK (8), SEQ (512), LR (2e-4).
set -u
cd "$(dirname "$0")/.."
K=$(pwd)/katana
M=${1:-$(pwd)/models/Qwen-AgentWorld-35B-A3B-UD-Q4_K_M.gguf}
HOURS=${2:-5}
OUT=${3:-$HOME/.katana/treino/$(date +%Y%m%d-%H%M)}
SESS=$HOME/.katana/desktop/sessoes
RANK=${RANK:-8}; SEQ=${SEQ:-512}; LR=${LR:-2e-4}
mkdir -p "$OUT"
R="$OUT/report.txt"
say() { echo "$(date +%H:%M:%S) $*" | tee -a "$R"; }
say "modelo: $M"
say "saida: $OUT"

# 1. espera o agente parar
if [ "${NO_WAIT:-0}" != 1 ]; then
  LIM=$(( ${WAIT_H:-6} * 3600 )); T0=$(date +%s); QUIET=0
  while :; do
    AGORA=$(date +%s)
    LAST=$(ls -t "$SESS"/*.ksess 2>/dev/null | head -1)
    AGE=999999; [ -n "$LAST" ] && AGE=$(( AGORA - $(stat -c %Y "$LAST") ))
    if pgrep -f "system-prompt-file /tmp/katana_teacher_" >/dev/null 2>&1 || [ "$AGE" -lt 600 ]; then QUIET=0; else QUIET=$(( QUIET + 1 )); fi
    if ! pgrep -f "katana agent " >/dev/null 2>&1; then say "nenhum agente rodando: sigo"; break; fi
    if [ "$QUIET" -ge 3 ]; then say "agente parado ha mais de 10 minutos (ultima gravacao da sessao ha $AGE s): sigo"; break; fi
    if [ $(( AGORA - T0 )) -ge "$LIM" ]; then say "o agente ainda trabalhava depois de ${WAIT_H:-6} h de espera: DESISTI, nada foi treinado"; exit 2; fi
    sleep 30
  done
fi

# 2. dados
# cada sessao passa pelo katana import: as chamadas das ferramentas antigas viram as atuais e a conversa e montada de novo
# com o prompt de sistema de hoje; so as falas do professor contam. Sequencias com forma de chave (8-4-4-4-12 hex) sao
# mascaradas; REPLACE='REGEX=TEXTO' acrescenta outra troca.
"$K" agentsys | sed '$d' > "$OUT/sys.txt"
: > "$OUT/data.jsonl"; : > "$OUT/export.log"; NS=0
for f in "$SESS"/*.ksess; do
    [ -f "$f.teacher" ] || continue
    if "$K" import "$M" "$f" "$OUT/part.jsonl" --system "file:$OUT/sys.txt" --teacher \
        --replace '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}=00000000-0000-0000-0000-000000000000' \
        ${REPLACE:+--replace "$REPLACE"} >> "$OUT/export.log" 2>&1; then cat "$OUT/part.jsonl" >> "$OUT/data.jsonl"; NS=$(( NS + 1 )); fi
done
rm -f "$OUT/part.jsonl"
echo "import: $NS sessoes com falas do professor -> $OUT/data.jsonl" >> "$OUT/export.log"
say "$(tail -1 "$OUT/export.log")"
python3 - "$OUT/data.jsonl" >> "$R" 2>&1 <<'EOF'
import json, sys
n = f = c = 0
try:
    for l in open(sys.argv[1]):
        d = json.loads(l); n += 1; f += len(d["train_spans"]); c += sum(b - a for a, b in d["train_spans"])
except Exception as e:
    print("         dados: erro", e)
print("         dados: %d conversas, %d falas do professor, %d caracteres a aprender" % (n, f, c))
sys.exit(0 if f else 1)
EOF
if [ $? != 0 ]; then say "sem falas do professor nos dados: nada a treinar"; exit 3; fi

# 3. plano, memoria e tempo de um passo
# --final 0: sem a perda final em todas as janelas (no modelo de 35B na CPU ela custou de 1,3 a 2,4 h, mais que os passos)
OPTS="--train blk.*.attn_* --rank $RANK --seq $SEQ --lr $LR --final 0"
export KATANA_TRAIN_GPU=0      # produtos na CPU: o treino na placa de verdade ainda nao foi exercitado, e a placa pode estar com o desktop
A=$(date +%s); nice -n 10 "$K" train "$M" "$OUT/data.jsonl" --plan 1 $OPTS > "$OUT/plan.log" 2>&1; B=$(date +%s)
grep -a "^treino:\|^passo:\|^memoria:" "$OUT/plan.log" | sed 's/^/         /' >> "$R"
NEED_MB=$(grep -ao "tokens [0-9]* MB" "$OUT/plan.log" | grep -o "[0-9]*" | tail -1)
FREE_MB=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
say "memoria: o treino pede ${NEED_MB:-?} MB de ativacoes; disponivel agora $FREE_MB MB"
if [ -z "$NEED_MB" ] || [ $(( NEED_MB + 6000 )) -gt "$FREE_MB" ]; then say "memoria insuficiente (ou plano sem resposta): NAO treinei"; exit 4; fi
LOAD_S=$(( B - A ))
A=$(date +%s); nice -n 10 "$K" train "$M" "$OUT/data.jsonl" $OPTS --steps 2 --out "$OUT/measure.kspec" > "$OUT/measure.log" 2>&1; RC=$?; B=$(date +%s)
if [ $RC != 0 ]; then say "o treino de 2 passos falhou (codigo $RC): $(tail -2 "$OUT/measure.log" | tr '\n' ' ')"; exit 5; fi
# o tempo por passo vem do proprio treino (linha "velocidade"); o relogio de fora inclui a carga e a gravacao
STEP_S=$(grep -ao "([0-9.]* s por passo" "$OUT/measure.log" | grep -o "[0-9]*" | head -1)
[ -z "$STEP_S" ] && STEP_S=$(( (B - A - LOAD_S) / 2 )); STEP_S=$(( STEP_S + 1 ))
say "medido: carga $LOAD_S s, cerca de $STEP_S s por passo (janela de $SEQ tokens, produtos na CPU)"
grep -a "perda\|resultado" "$OUT/measure.log" | tail -3 | sed 's/^/         /' >> "$R"
rm -f "$OUT/measure.kspec"
STEPS=$(awk -v h="$HOURS" -v p="$STEP_S" 'BEGIN { n = int(h * 3600 / p); if (n > 400) n = 400; if (n < 4) n = 4; print n }')
say "treino: $STEPS passos (limite de $HOURS h)"

# 4. treino
A=$(date +%s)
nice -n 10 timeout $(awk -v h="$HOURS" 'BEGIN { print int(h * 3600 * 1.3 + 600) }') "$K" train "$M" "$OUT/data.jsonl" $OPTS --steps "$STEPS" \
  --log "$OUT/steps.jsonl" --out "$OUT/teacher.kspec" > "$OUT/train.log" 2>&1
RC=$?; B=$(date +%s)
say "treino terminou com codigo $RC em $(( (B - A) / 60 )) min"
grep -a "^resultado:\|perda media final\|perda final\|^velocidade" "$OUT/train.log" | tail -3 | sed 's/^/         /' >> "$R"
python3 - "$OUT/steps.jsonl" >> "$R" 2>&1 <<'EOF'
import json, sys
try:
    ls = [json.loads(l) for l in open(sys.argv[1])]
    tr = [x for x in ls if "loss" in x]
    if tr:
        k = max(1, len(tr) // 10)
        print("         perda nos passos: comeco %.3f (media dos %d primeiros), fim %.3f (media dos %d ultimos), %d passos registrados"
              % (sum(x["loss"] for x in tr[:k]) / k, k, sum(x["loss"] for x in tr[-k:]) / k, k, len(tr)))
except Exception as e:
    print("         registro dos passos: nao li (%s)" % e)
EOF
if [ ! -s "$OUT/teacher.kspec" ]; then say "nenhuma especializacao gravada"; exit 6; fi
say "especializacao: $OUT/teacher.kspec ($(( $(stat -c %s "$OUT/teacher.kspec") / 1024 )) KB)"

# 5. o modelo com a especializacao carrega e responde? (CPU; base e especializado, mesma pergunta, sem sorteio)
P='Em uma frase: o que voce faz primeiro quando um script de instalacao falha?'
KATANA_CPU=1 timeout 900 "$K" chat "$M" "$P" 60 0 > "$OUT/answer_base.txt" 2>/dev/null
KATANA_CPU=1 KATANA_SPEC="$OUT/teacher.kspec" timeout 900 "$K" chat "$M" "$P" 60 0 > "$OUT/answer_trained.txt" 2>"$OUT/resposta_treinado.err"
say "resposta do modelo base:     $(tr '\n' ' ' < "$OUT/answer_base.txt" | cut -c1-300)"
say "resposta com o treino:       $(tr '\n' ' ' < "$OUT/answer_trained.txt" | cut -c1-300)"
say "fim. Para usar: KATANA_SPEC=$OUT/teacher.kspec ./katana agent $M  (nada foi aplicado sozinho)"
