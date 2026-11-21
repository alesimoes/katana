#!/bin/sh
# Testa as ferramentas Bash, TaskStop e Monitor sem modelo: sh tools/tooltest/bash.sh ./katana
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
export KATANA_LOG=${KATANA_LOG:-$(mktemp)}     # o log das chamadas destes testes nao vai para o ~/.katana/agente.log do dono
[ -x "$K" ] || { echo "uso: $0 <katana>"; exit 2; }
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
export HOME="$T/home"; mkdir -p "$HOME" "$T/w"; cd "$T/w" || exit 2
W=$(pwd -P)
unset KATANA_CONFIRM KATANA_CMD_TIMEOUT KATANA_UI
N=0; F=0
ok() { N=$((N+1)); echo "ok   $1"; }
bad() { N=$((N+1)); F=$((F+1)); echo "FAIL $1"; printf '%s\n' "$OUT" | head -20 | sed 's/^/     | /'; }
has() { case "$OUT" in *"$2"*) ok "$1";; *) bad "$1 (esperava: $2)";; esac; }
hasnt() { case "$OUT" in *"$2"*) bad "$1 (nao esperava: $2)";; *) ok "$1";; esac; }
run() { OUT=$("$K" tool "$@" 2>/dev/null); }
J="$HOME/.katana/jobs"

run Bash '{"command":"echo hello"}'; [ "$OUT" = "hello" ] && ok "saida simples" || bad "saida simples"
run Bash '{"command":"echo out; echo err >&2"}'; has "stderr junto" "err"
run Bash '{"command":"exit 3"}'; has "codigo de saida" "Exit code 3"
run Bash '{"command":"true"}'; [ "$OUT" = "(no output)" ] && ok "sem saida" || bad "sem saida"
run Bash '{"command":"cat; echo done"}'; has "stdin fechado" "done"
run Bash '{}'; has "sem command" "error: missing command"
run Bash '{"command":"   "}'; has "command vazio" "error: missing command"
run Bash '{"command":"echo x","description":"prints x"}'; [ "$OUT" = "x" ] && ok "description aceita" || bad "description aceita"
run Bash '{"command":"[[ 1 == 1 ]] && echo bashsyntax"}'; has "shell e bash" "bashsyntax"
# codigo 1 que nao e falha
run Bash '{"command":"grep nada /dev/null"}'; has "grep 1: codigo informado" "Exit code 1"; hasnt "grep 1: sem dica" "hint"
run Bash '{"command":"cd . && git diff --no-index /dev/null /dev/null; exit 1"}'; has "exit 1 comum" "Exit code 1"
run Bash '{"command":"python3 -c \"import nao_existe_mod\""}'; has "falha com dica (diagnose)" "[hint: the Python module nao_existe_mod is missing"
# pasta persiste, ambiente nao
run Bash '{"command":"mkdir -p a/b; cd a/b"}' Bash '{"command":"pwd"}'; has "cd persiste" "$W/a/b"
run Bash '{"command":"cd a"}' Bash '{"command":"cd /"}' Bash '{"command":"pwd"}'
has "cd fora volta" "Shell cwd was reset to $W"
case "$OUT" in *"=== Bash
$W")  ok "pasta depois do reset";; *) bad "pasta depois do reset";; esac
run Bash '{"command":"export ZZ=1"}' Bash '{"command":"echo \"[${ZZ:-vazio}]\""}'; has "ambiente nao persiste" "[vazio]"
run Bash '{"command":"cd a; exit 0"}' Bash '{"command":"pwd"}'; has "cd com exit persiste" "$W/a"
# saida grande
run Bash '{"command":"seq 1 20000"}'; has "grande: arquivo" "Full output saved to: $J/"; has "grande: previa" "Preview (first"
P=$(printf '%s\n' "$OUT" | sed -n 's/.*saved to: \(.*\)$/\1/p' | head -1)
[ -f "$P" ] && [ "$(wc -l < "$P")" -eq 20000 ] && ok "grande: arquivo completo" || bad "grande: arquivo completo"
[ ${#OUT} -le 2400 ] && ok "grande: previa curta" || bad "grande: previa curta (${#OUT})"
run Bash '{"command":"seq 1 20000; exit 2"}'; has "falha grande: inicio" "1
2
3
"; has "falha grande: fim" "20000"; has "falha grande: omitido" "bytes omitted"; has "falha grande: codigo" "Exit code 2"
[ ${#OUT} -le 11500 ] && ok "falha grande: limite" || bad "falha grande: limite (${#OUT})"
run Bash '{"command":"seq 1 1200"}'; has "media inline" "1200"; hasnt "media sem arquivo" "saved to"
# timeout
run Bash '{"command":"echo start; sleep 4; echo fim","timeout":800}' TaskStop '{"task_id":"1"}' TaskStop '{"task_id":"1"}'
has "timeout: segundo plano" "Command did not complete within its 0.8s timeout and was moved to the background with ID: 1"
has "timeout: saida ate agora" "start"; has "TaskStop" "Successfully stopped task: 1"; has "TaskStop de novo" "error: task 1 is not running"
rm -rf "$J"; run Bash '{"command":"echo a; sleep 2; echo fim","timeout":500}' Bash '{"command":"sleep 2.5; cat '"$J"'/*-1.log"}'
has "segundo plano continua" "fim"
run Bash '{"command":"sleep 5","timeout":300}'; has "sleep morre" "timed out after 0.3s and was killed"; has "sleep codigo" "Exit code 124"
# run_in_background
rm -rf "$J"; run Bash '{"command":"echo bg; sleep 1; echo bgfim","run_in_background":true}' Bash '{"command":"sleep 2; cat '"$J"'/*-1.log"}'
has "fundo: id" "Command running in background with ID: 1"; has "fundo: saida no arquivo" "bgfim"
run Bash '{"command":"sudo ls","run_in_background":true}'; has "fundo sem sudo" "error: a background command cannot use sudo"
run Bash '{"command":"sleep 30","run_in_background":"true"}' TaskStop '{"task_id":1}'; has "TaskStop numero" "Successfully stopped task: 1"
run TaskStop '{"task_id":"9"}'; has "TaskStop inexistente" "error: there is no task 9"
run TaskStop '{}'; has "TaskStop sem id" "error: missing task_id"
run Bash '{"command":"cd a"}' Bash '{"command":"pwd > where.txt","run_in_background":true}' Bash '{"command":"sleep 0.5; cat where.txt"}'; has "fundo usa a pasta do shell" "$W/a"
# Monitor
rm -rf "$J"; run Monitor '{"command":"echo m1; sleep 10; echo m2","description":"watch","timeout_ms":1000}' Bash '{"command":"sleep 2.5; cat '"$J"'/*-1.log"}' TaskStop '{"task_id":"1"}'
has "Monitor: id" "Monitor started with ID: 1 (stops after 1 s)"; has "Monitor: linha" "m1"; hasnt "Monitor: prazo" "m2"; has "Monitor: parou no prazo" "is not running"
run Monitor '{"description":"x"}'; has "Monitor sem command" "error: missing command"
# ferramentas antigas
run run_command '{"command":"echo velho"}'; has "run_command continua" "velho"
echo
if [ $F -gt 0 ]; then echo "FAILED $F"; exit 1; fi
echo "OK $N checks"
