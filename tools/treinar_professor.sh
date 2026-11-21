#!/bin/sh
# Treina o modelo local com as respostas que o professor deu no agente (modo professor), sem ninguem olhando:
#   1. espera o agente do desktop ficar parado (nenhuma chamada ao professor e a sessao sem gravar ha 10 minutos);
#   2. katana export --teacher: as sessoes do desktop viram dados, com a perda so nas falas do professor;
#   3. mede o tempo de um passo de treino neste modelo e escolhe quantos passos cabem no tempo dado;
#   4. katana train com adaptadores de posto baixo so na atencao (roda na GPU depois, nao mexe nos especialistas);
#   5. confere que o modelo com a especializacao carrega e responde (na CPU, para nao disputar a placa);
#   6. grava tudo, com um relatorio, na pasta de saida. Nao aplica a especializacao em lugar nenhum.
# Uso: sh tools/treinar_professor.sh [modelo.gguf] [horas de treino, padrao 5] [pasta de saida]
# Variaveis: ESPERA_H (limite da espera, padrao 6), SEM_ESPERA=1 (nao espera), POSTO (8), JANELA (512), LR (2e-4).
set -u
cd "$(dirname "$0")/.."
K=$(pwd)/katana
M=${1:-$(pwd)/models/Qwen-AgentWorld-35B-A3B-UD-Q4_K_M.gguf}
HORAS=${2:-5}
OUT=${3:-$HOME/.katana/treino/$(date +%Y%m%d-%H%M)}
SESS=$HOME/.katana/desktop/sessoes
POSTO=${POSTO:-8}; JANELA=${JANELA:-512}; LR=${LR:-2e-4}
mkdir -p "$OUT"
R="$OUT/relatorio.txt"
say() { echo "$(date +%H:%M:%S) $*" | tee -a "$R"; }
say "modelo: $M"
say "saida: $OUT"

# 1. espera o agente parar
if [ "${SEM_ESPERA:-0}" != 1 ]; then
  LIM=$(( ${ESPERA_H:-6} * 3600 )); T0=$(date +%s); QUIETO=0
  while :; do
    AGORA=$(date +%s)
    ULT=$(ls -t "$SESS"/*.ksess 2>/dev/null | head -1)
    IDADE=999999; [ -n "$ULT" ] && IDADE=$(( AGORA - $(stat -c %Y "$ULT") ))
    if pgrep -f "system-prompt-file /tmp/katana_teacher_" >/dev/null 2>&1 || [ "$IDADE" -lt 600 ]; then QUIETO=0; else QUIETO=$(( QUIETO + 1 )); fi
    if ! pgrep -f "katana agent " >/dev/null 2>&1; then say "nenhum agente rodando: sigo"; break; fi
    if [ "$QUIETO" -ge 3 ]; then say "agente parado ha mais de 10 minutos (ultima gravacao da sessao ha $IDADE s): sigo"; break; fi
    if [ $(( AGORA - T0 )) -ge "$LIM" ]; then say "o agente ainda trabalhava depois de ${ESPERA_H:-6} h de espera: DESISTI, nada foi treinado"; exit 2; fi
    sleep 30
  done
fi

# 2. dados
"$K" export "$OUT/dados.jsonl" "$SESS" --teacher > "$OUT/export.log" 2>&1
say "$(tail -1 "$OUT/export.log")"
python3 - "$OUT/dados.jsonl" >> "$R" 2>&1 <<'EOF'
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
OPC="--train blk.*.attn_* --rank $POSTO --seq $JANELA --lr $LR"
export KATANA_TRAIN_GPU=0      # produtos na CPU: o treino na placa de verdade ainda nao foi exercitado, e a placa pode estar com o desktop
A=$(date +%s); nice -n 10 "$K" train "$M" "$OUT/dados.jsonl" --plan 1 $OPC > "$OUT/plano.log" 2>&1; B=$(date +%s)
grep -a "^treino:\|^passo:\|^memoria:" "$OUT/plano.log" | sed 's/^/         /' >> "$R"
PRECISA=$(grep -ao "tokens [0-9]* MB" "$OUT/plano.log" | grep -o "[0-9]*" | tail -1)
LIVRE=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
say "memoria: o treino pede ${PRECISA:-?} MB de ativacoes; disponivel agora $LIVRE MB"
if [ -z "$PRECISA" ] || [ $(( PRECISA + 6000 )) -gt "$LIVRE" ]; then say "memoria insuficiente (ou plano sem resposta): NAO treinei"; exit 4; fi
CARGA=$(( B - A ))
A=$(date +%s); nice -n 10 "$K" train "$M" "$OUT/dados.jsonl" $OPC --steps 2 --out "$OUT/medida.kspec" > "$OUT/medida.log" 2>&1; RC=$?; B=$(date +%s)
if [ $RC != 0 ]; then say "o treino de 2 passos falhou (codigo $RC): $(tail -2 "$OUT/medida.log" | tr '\n' ' ')"; exit 5; fi
PASSO=$(( (B - A - CARGA) / 2 )); [ "$PASSO" -lt 1 ] && PASSO=1
say "medido: carga $CARGA s, cerca de $PASSO s por passo (janela de $JANELA tokens, produtos na CPU)"
grep -a "perda\|resultado" "$OUT/medida.log" | tail -3 | sed 's/^/         /' >> "$R"
rm -f "$OUT/medida.kspec"
PASSOS=$(awk -v h="$HORAS" -v p="$PASSO" 'BEGIN { n = int(h * 3600 / p); if (n > 400) n = 400; if (n < 4) n = 4; print n }')
say "treino: $PASSOS passos (limite de $HORAS h)"

# 4. treino
A=$(date +%s)
nice -n 10 timeout $(awk -v h="$HORAS" 'BEGIN { print int(h * 3600 * 1.3 + 600) }') "$K" train "$M" "$OUT/dados.jsonl" $OPC --steps "$PASSOS" \
  --log "$OUT/passos.jsonl" --out "$OUT/professor.kspec" > "$OUT/treino.log" 2>&1
RC=$?; B=$(date +%s)
say "treino terminou com codigo $RC em $(( (B - A) / 60 )) min"
grep -a "^resultado:\|perda media final" "$OUT/treino.log" | tail -2 | sed 's/^/         /' >> "$R"
python3 - "$OUT/passos.jsonl" >> "$R" 2>&1 <<'EOF'
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
if [ ! -s "$OUT/professor.kspec" ]; then say "nenhuma especializacao gravada"; exit 6; fi
say "especializacao: $OUT/professor.kspec ($(( $(stat -c %s "$OUT/professor.kspec") / 1024 )) KB)"

# 5. o modelo com a especializacao carrega e responde? (CPU; base e especializado, mesma pergunta, sem sorteio)
P='Em uma frase: o que voce faz primeiro quando um script de instalacao falha?'
KATANA_CPU=1 timeout 900 "$K" chat "$M" "$P" 60 0 > "$OUT/resposta_base.txt" 2>/dev/null
KATANA_CPU=1 KATANA_SPEC="$OUT/professor.kspec" timeout 900 "$K" chat "$M" "$P" 60 0 > "$OUT/resposta_treinado.txt" 2>"$OUT/resposta_treinado.err"
say "resposta do modelo base:     $(tr '\n' ' ' < "$OUT/resposta_base.txt" | cut -c1-300)"
say "resposta com o treino:       $(tr '\n' ' ' < "$OUT/resposta_treinado.txt" | cut -c1-300)"
say "fim. Para usar: KATANA_SPEC=$OUT/professor.kspec ./katana agent $M  (nada foi aplicado sozinho)"
