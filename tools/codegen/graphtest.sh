#!/bin/sh
# Testes do modulo de grafos e do otimizador do codegen (docs/codegen.md, "UIR-G"), sem modelo e sem o katana:
#   1. graphtest.c: dominadores, fronteira, componentes e lacos contra algoritmos ingenuos em grafos aleatorios e nos grafos dos livros; fluxo de dados;
#      SSA valida (todas as tarefas e um corpus gerado); relatorio exato em programas semeados; dobra e simplificacao com os casos de borda do Python
#   2. as tarefas otimizadas (com todos os passes) ainda passam os testes que o autor deu
#   3. teste diferencial: programas aleatorios validos, o Python antes e depois da otimizacao, as mesmas entradas, saidas identicas
# Uso: sh tools/codegen/graphtest.sh [programas do teste diferencial, 300 por padrao]       (SAN=1: ASan e UBSan)
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
N=${1:-300}
T=$(mktemp -d) || exit 2; trap 'rm -rf "$T"' EXIT
CC=${CC:-gcc}; SANF=""; [ -n "$SAN" ] && SANF="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
BAD=0; NCHK=0
ok()   { NCHK=$((NCHK+1)); echo "ok   $1"; }
fail() { NCHK=$((NCHK+1)); BAD=$((BAD+1)); echo "FAIL $1"; [ -n "$2" ] && echo "     $2"; }
$CC -O1 -g -Wall -Wno-unused-function $SANF -I "$ROOT" "$HERE/codegencli.c" -o "$T/codegencli" -lm 2>"$T/cc.log" || { cat "$T/cc.log"; echo "FAIL build codegencli"; exit 1; }
$CC -O1 -g -Wall -Wno-unused-function $SANF -I "$ROOT" "$HERE/graphtest.c" -o "$T/graphtest" -lm 2>>"$T/cc.log" || { cat "$T/cc.log"; echo "FAIL build graphtest"; exit 1; }
[ -s "$T/cc.log" ] && { cat "$T/cc.log"; fail "build warnings"; }
# corpus gerado para as invariantes da SSA e a idempotencia (programas validos aleatorios)
mkdir -p "$T/corpus"
python3 - "$HERE" "$T/corpus" <<'PY'
import sys, subprocess
sys.path.insert(0, sys.argv[1]); import fuzz_gen
for seed in range(1, 81):
    open(f"{sys.argv[2]}/g{seed:03d}.kcode", "w").write(fuzz_gen.gen_program(seed, scalar=(seed % 2 == 0))[0])
PY
o=$("$T/graphtest" "$HERE/tasks" "$T/corpus" 2>&1); echo "$o" | tail -12
case "$o" in *"OK: "*" 0 failed"*) ok "graphtest: $(echo "$o" | tail -1)";; *) fail "graphtest";; esac
# as tarefas otimizadas passam os testes do autor, com os passes padrao e com todos
for f in "$HERE"/tasks/*.kcode; do
  b=$(basename "$f" .kcode); st=$("$T/codegencli" stats "$f")
  case "$st" in *"raw 0"*) ;; *) continue;; esac
  for mode in "" "--all"; do
    r=$("$T/codegencli" optgen python "$f" --tests --recheck $mode 2>/dev/null | python3 -I - 2>&1)
    case "$r" in *FAIL*|*Traceback*|*Error*) fail "tarefa $b otimizada ($mode)" "$r";; *"test 1: ok"*) ok "tarefa $b otimizada $mode: os testes dados passam";; *) fail "tarefa $b otimizada ($mode): nenhum teste rodou" "$r";; esac
  done
done
# relatorio e IR pela linha de comando
o=$("$T/codegencli" ir "$HERE/tasks/06_binary_search.kcode"); case "$o" in *"verify: ok"*"phi"*|*"phi"*"verify: ok"*) ok "codegencli ir: CFG em SSA com phi e verificador";; *) fail "codegencli ir" "$o";; esac
o=$("$T/codegencli" flow "$HERE/tasks/07_safe_divide.kcode"); case "$o" in *"generation order (callees first): divide safeDivide"*) ok "codegencli flow: ordem de geracao, quem e chamado primeiro";; *) fail "codegencli flow" "$o";; esac
# diferencial
o=$(python3 "$HERE/difftest.py" "$T/codegencli" "$N" 1 2>&1); echo "$o" | tail -6
case "$o" in *"DIFF OK"*) ok "teste diferencial: $N programas, Python igual antes e depois da otimizacao";; *) fail "teste diferencial" "$(echo "$o" | tail -12)";; esac
echo; [ $BAD -eq 0 ] && echo "OK $NCHK checks" || echo "FAILED $BAD of $NCHK"
exit $BAD
