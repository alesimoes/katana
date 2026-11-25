#!/bin/sh
# Testes do backend Go do UCGE (docs/ucge.md, "Backend Go"): gofmt aceita a saida sem mudar, go vet sem aviso, as tarefas sem RAW passam nos testes
# dados, casos escritos a mao e programas aleatorios comparados com o Python gerado. Sem `go` no PATH o script so avisa e sai com sucesso.
# Uso: sh tools/ucge/testgo.sh        (N_RANDOM=40 muda o numero de programas aleatorios)
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
command -v go >/dev/null 2>&1 && command -v gofmt >/dev/null 2>&1 || { echo "SKIP: go nao encontrado (o backend Go so e gerado, nao validado)"; exit 0; }
T=$(mktemp -d) || exit 2; trap 'rm -rf "$T"' EXIT
export GOCACHE=${GOCACHE:-$T/gocache} GO111MODULE=off
CC=${CC:-gcc}
$CC -O1 -g -Wall -Wno-unused-function -I "$ROOT" "$HERE/ucgecli.c" -o "$T/ucgecli" -lm 2>"$T/cc.log" || { cat "$T/cc.log"; echo "FAIL build"; exit 1; }
[ -s "$T/cc.log" ] && { cat "$T/cc.log"; echo "FAIL build warnings"; exit 1; }
U="$T/ucgecli"; N=0; BAD=0
ok()   { N=$((N+1)); echo "ok   $1"; }
fail() { N=$((N+1)); BAD=$((BAD+1)); echo "FAIL $1"; [ -n "$2" ] && echo "     got: $(printf '%s' "$2" | head -c 600 | tr '\n' '|')"; }
has()  { case "$2" in *"$3"*) ok "$1";; *) fail "$1 (missing: $3)" "$2";; esac; }

python3 "$HERE/rt2inc.py" --check && ok "runtime embutido em dia (ucgego_rt.inc = rt/cgrt.go)" || fail "ucgego_rt.inc desatualizado: rode python3 tools/ucge/rt2inc.py"
for f in "$HERE"/tasks/*.csp; do
  b=$(basename "$f" .csp); st=$("$U" stats "$f")
  case "$st" in *"raw 0"*) o=$("$U" run go "$f"); case "$o" in *" 0 failed"*) ok "tarefa $b: Go (gofmt, vet, testes) ok";; *) fail "tarefa $b em Go" "$o";; esac;; esac
done
o=$("$U" gen go "$HERE/tasks/14_json_total.csp"); has "RAW e recusado no Go" "$o" "have no Go version"
a=$("$U" gen go "$HERE/tasks/06_binary_search.csp"); b=$("$U" gen go "$HERE/tasks/06_binary_search.csp"); [ "$a" = "$b" ] && ok "determinismo do Go" || fail "determinismo do Go"
printf 'fn f(a: int) -> int\n  throw "boom"\n  return a\ntest f(1) raises\ntest f(1) == 1\n' > "$T/t2.csp"; o=$("$U" run go "$T/t2.csp"); has "raises passa e erro nao capturado vira falha" "$o" "1 passed, 1 failed"
o=$(python3 "$HERE/difftest_targets.py" "$U" --cases "$HERE/cases" --target go 2>&1 | tail -3); has "casos a mao: Python e Go iguais" "$o" "ok 5"
o=$(python3 "$HERE/difftest_targets.py" "$U" -n "${N_RANDOM:-40}" --seed 1 --target go 2>&1 | tail -3); has "programas aleatorios: Python e Go iguais" "$o" "ok ${N_RANDOM:-40};"
echo; [ $BAD -eq 0 ] && echo "OK $N checks" || echo "FAILED $BAD of $N"
exit $BAD
