#!/bin/sh
# Os comandos do agente morrem com ele (SIGKILL, SIGTERM, SIGINT, saida normal): sh tools/tooltest/reap.sh ./katana
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
[ -x "$K" ] || { echo "uso: $0 <katana>"; exit 2; }
export KATANA_LOG=${KATANA_LOG:-$(mktemp)}
T=$(mktemp -d) || exit 2
export HOME="$T/home"; mkdir -p "$HOME" "$T/w"; cd "$T/w" || exit 2
unset KATANA_CONFIRM KATANA_CMD_TIMEOUT KATANA_UI
N=0; F=0
ok() { N=$((N+1)); echo "ok   $1"; }
bad() { N=$((N+1)); F=$((F+1)); echo "FAIL $1"; }
# so os processos cujo pid o proprio teste gravou sao olhados (e mortos no fim se sobraram); vivo = existe e nao e zumbi
alive() { [ -r "/proc/$1/stat" ] && [ "$(sed 's/.*) \(.\).*/\1/' "/proc/$1/stat" 2>/dev/null)" != Z ]; }
leftover() { for f in "$T"/w/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -f "$T"/w/*.pid; }
trap 'leftover; rm -rf "$T"' EXIT
waitfile() { i=0; while [ ! -s "$1" ] && [ $i -lt 200 ]; do sleep 0.05; i=$((i+1)); done; [ -s "$1" ]; }
gone() { # nome arquivos...
    n=$1; shift
    for f in "$@"; do
        waitfile "$f" || { bad "$n: $f nunca comecou"; continue; }
        p=$(cat "$f"); i=0
        while alive "$p" && [ $i -lt 80 ]; do sleep 0.05; i=$((i+1)); done
        if alive "$p"; then bad "$n: $f ($p) ficou vivo"; else ok "$n: $f"; fi
    done
}
# neto que ignora SIGTERM, trabalho em segundo plano, comando que passou do prazo, e o da frente
JOB='{"command":"(trap \"\" TERM HUP; echo $BASHPID > grand.pid; while :; do sleep 1; done) & echo $$ > job.pid; sleep 300","run_in_background":true}'
LATE='{"command":"echo $$ > late.pid; while :; do sleep 1; done","timeout":300}'
LEFT='{"command":"sleep 300 & echo $! > left.pid"}'
FG='{"command":"echo $$ > fg.pid; while :; do sleep 1; done"}'
for sig in KILL TERM INT; do
    leftover
    # subshell com & ignora SIGINT; o python devolve o padrao antes do exec
    python3 -c 'import os,signal,sys; signal.signal(signal.SIGINT, signal.SIG_DFL); os.execv(sys.argv[1], sys.argv[1:])' \
        "$K" tool Bash "$JOB" Bash "$LATE" Bash "$LEFT" Bash "$FG" > /dev/null 2>&1 &
    KP=$!
    for f in job grand late left fg; do waitfile $f.pid || bad "SIG$sig: $f nao comecou"; done
    kill -$sig $KP
    [ $sig = INT ] || wait $KP 2>/dev/null
    if [ $sig = INT ]; then gone "SIG$sig (Ctrl+C: o comando da frente)" fg.pid; wait $KP 2>/dev/null; fi
    gone "SIG$sig" job.pid grand.pid late.pid left.pid fg.pid
done
leftover
"$K" tool Bash "$JOB" Bash "$LATE" Bash "$LEFT" > /dev/null 2>&1
gone "saida normal" job.pid grand.pid late.pid left.pid
# durante a sessao o filho deixado pelo comando continua (servidor iniciado com &)
leftover
OUT=$("$K" tool Bash "$LEFT" Bash '{"command":"sleep 0.5; kill -0 $(cat left.pid) && echo VIVO"}' 2>/dev/null)
case "$OUT" in *VIVO*) ok "processo com & vive enquanto a sessao vive";; *) bad "processo com & vive enquanto a sessao vive";; esac
gone "e morre com ela" left.pid
echo
if [ $F -gt 0 ]; then echo "FAILED $F de $N"; exit 1; fi
echo "OK $N checks"
