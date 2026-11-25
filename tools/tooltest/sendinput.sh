#!/bin/sh
# Testa a ferramenta SendInput sem modelo: sh tools/tooltest/sendinput.sh ./katana
# Cada caso roda Bash (run_in_background) e SendInput no mesmo processo (katana tool A '<json>' B '<json>'), porque a
# tabela de tarefas vive no processo; ao sair o katana encerra as tarefas.
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
[ -x "$K" ] || { echo "uso: $0 <katana>"; exit 2; }
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
export HOME="$T/home"; mkdir -p "$HOME" "$T/w"; cd "$T/w" || exit 2
export KATANA_LOG="$T/agente.log"     # o log das chamadas destes testes nao vai para o ~/.katana/agente.log do dono
unset KATANA_CONFIRM KATANA_CMD_TIMEOUT KATANA_UI
N=0; F=0
ok() { N=$((N+1)); echo "ok   $1"; }
bad() { N=$((N+1)); F=$((F+1)); echo "FAIL $1"; printf '%s\n' "$OUT" | head -20 | sed 's/^/     | /'; }
has() { case "$OUT" in *"$2"*) ok "$1";; *) bad "$1 (esperava: $2)";; esac; }
hasnt() { case "$OUT" in *"$2"*) bad "$1 (nao esperava: $2)";; *) ok "$1";; esac; }
run() { OUT=$("$K" tool "$@" 2>/dev/null); }

# uma linha lida do stdin e devolvida na saida
run Bash '{"command":"echo prompt:; read -r x; echo got:$x","run_in_background":true}' SendInput '{"task_id":"1","text":"segredo123"}'
has "em segundo plano avisa do SendInput" "answer with SendInput"
has "SendInput entrega a linha" "got:segredo123"
has "SendInput informa os bytes" "Typed 10 bytes into task 1 followed by a newline"
OUT=$(printf '%s\n' "$OUT" | sed -n '/=== SendInput/,$p')
hasnt "resultado do SendInput sem eco do texto" "Typed segredo123"
# (o log de acoes e o marcador TOOL so passam pelo laco do agente, nao por katana tool: a mascara, job_redact em
#  src/agent/jobs.c, nao e coberta aqui)

# newline=false: nada e lido ate a quebra; a segunda chamada completa a linha
run Bash '{"command":"read -r x; echo got:$x","run_in_background":true}' SendInput '{"task_id":"1","text":"ab","newline":false}' SendInput '{"task_id":"1","text":"cd"}'
has "newline false e depois o resto" "got:abcd"

# duas perguntas seguidas (usuario e codigo)
run Bash '{"command":"read -r u; echo user:$u; read -r c; echo code:$c","run_in_background":true}' SendInput '{"task_id":"1","text":"ana"}' SendInput '{"task_id":"1","text":"424242"}'
has "primeira resposta" "user:ana"
has "segunda resposta" "code:424242"

# erros
run SendInput '{"task_id":"9","text":"x"}'; has "tarefa inexistente" "error: there is no task 9"
run SendInput '{"task_id":"1"}'; has "sem text" "error: missing text"
run Bash '{"command":"true","run_in_background":true}' Bash '{"command":"sleep 1"}' SendInput '{"task_id":"1","text":"x"}'
has "tarefa ja terminou" "error: task 1 is not running"
BIG=$(head -c 5000 /dev/zero | tr '\0' a)
run Bash '{"command":"read -r x","run_in_background":true}' SendInput "{\"task_id\":\"1\",\"text\":\"$BIG\"}"
has "texto grande recusado" "error: text is too long"
# movido para segundo plano no timeout: stdin fechado, sem entrada
run Bash '{"command":"sleep 0; sleep 5","timeout":500}' SendInput '{"task_id":"1","text":"x"}'
has "movido no timeout nao aceita entrada" "started in the foreground with stdin closed"
# TaskStop fecha o stdin
run Bash '{"command":"read -r x; echo never","run_in_background":true}' TaskStop '{"task_id":"1"}' SendInput '{"task_id":"1","text":"x"}'
has "depois do TaskStop" "error: task 1 is not running"
# primeiro plano continua com stdin fechado
run Bash '{"command":"cat; echo done"}'; has "primeiro plano com stdin fechado" "done"

echo "$N verificacoes, $F falhas"
[ "$F" = 0 ]
