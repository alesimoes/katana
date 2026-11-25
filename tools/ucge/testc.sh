#!/bin/sh
# Testes do backend C do UCGE (docs/ucge.md, "Backend C"): runtime em dia, as tarefas sem RAW compilam sem aviso (gcc -std=c99 -Wall -Wextra
# -Wpedantic) e passam nos testes dados, casos escritos a mao e programas aleatorios comparados com o Python gerado, mensagens de recusa.
# Uso: sh tools/ucge/testc.sh        (SAN=1 liga ASan e UBSan; N=60 muda o numero de programas aleatorios)
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
T=$(mktemp -d) || exit 2; trap 'rm -rf "$T"' EXIT
CC=${CC:-gcc}; SANF=""; [ -n "$SAN" ] && SANF="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC -O1 -g -Wall -Wno-unused-function $SANF -I "$ROOT" "$HERE/ucgecli.c" -o "$T/ucgecli" -lm 2>"$T/cc.log" || { cat "$T/cc.log"; echo "FAIL build"; exit 1; }
[ -s "$T/cc.log" ] && { cat "$T/cc.log"; echo "FAIL build warnings"; exit 1; }
U="$T/ucgecli"; N=0; BAD=0
ok()   { N=$((N+1)); echo "ok   $1"; }
fail() { N=$((N+1)); BAD=$((BAD+1)); echo "FAIL $1"; [ -n "$2" ] && echo "     got: $(printf '%s' "$2" | head -c 600 | tr '\n' '|')"; }
has()  { case "$2" in *"$3"*) ok "$1";; *) fail "$1 (missing: $3)" "$2";; esac; }

python3 "$HERE/rt2inc.py" --check && ok "runtime embutido em dia (ucgec_rt.inc = rt/cgrt.h)" || fail "ucgec_rt.inc desatualizado: rode python3 tools/ucge/rt2inc.py"

# ---- as tarefas sem RAW: C compila sem aviso e passa nos testes dados ----
for f in "$HERE"/tasks/*.csp; do
  b=$(basename "$f" .csp); st=$("$U" stats "$f")
  case "$st" in *"raw 0"*) o=$("$U" run c "$f"); case "$o" in *" 0 failed"*) ok "tarefa $b: C compila sem aviso e os testes passam";; *) fail "tarefa $b em C" "$o";; esac;; esac
done
for f in 14_json_total 15_fetch_title; do o=$("$U" gen c "$HERE/tasks/$f.csp"); has "tarefa $f: RAW sem versao em C e recusado" "$o" "has no \`raw c\` version"; done

# ---- determinismo e largura ----
a=$("$U" gen c "$HERE/tasks/06_binary_search.csp"); b=$("$U" gen c "$HERE/tasks/06_binary_search.csp"); [ "$a" = "$b" ] && ok "determinismo do C" || fail "determinismo do C"
o=$("$U" gen c "$HERE/tasks/06_binary_search.csp" --width 40); has "largura 40 quebra os argumentos" "$o" "cg_li_get("

# ---- recusas e construcoes especiais ----
cat > "$T/r.csp" <<'XX'
fn twice(x: int) -> int
  raw c defines y
    CgV y = cg_vint(x * 2);
  return y
test twice(4) == 8
XX
o=$("$U" run c "$T/r.csp"); has "raw c entra verbatim; defines e um CgV" "$o" "1 passed, 0 failed"
printf 'fn f(a: int) -> int\n  let x = a\n  x /= 2\n  return x\n' > "$T/d.csp"; o=$("$U" gen c "$T/d.csp"); has "/= em int e recusado com a saida" "$o" "would store a float"
printf 'fn f(a: int) -> int\n  return a\ntest f(1) == 2\n' > "$T/t.csp"; o=$("$U" run c "$T/t.csp"); has "teste que falha e reportado" "$o" "1 failed"
printf 'fn f(a: int) -> int\n  throw "boom"\n  return a\ntest f(1) raises\ntest f(1) == 1\n' > "$T/t2.csp"; o=$("$U" run c "$T/t2.csp"); has "raises passa e erro nao capturado vira falha" "$o" "1 passed, 1 failed"
printf 'fn f(p) -> int\n  try\n    return len(p)\n  finally\n    print("x")\ntest f("abc") == 3\n' > "$T/t3.csp"; o=$("$U" run c "$T/t3.csp"); has "any (parametro sem tipo) vira valor etiquetado" "$o" "1 passed, 0 failed"

# ---- casos escritos a mao e programas aleatorios: saida de Python e de C iguais ----
o=$(python3 "$HERE/difftest.py" "$U" --cases "$HERE/cases" ${SAN:+--san} 2>&1 | tail -3); has "casos a mao: Python e C iguais" "$o" "ok"
case "$o" in *fail*|*diff*) echo "$o";; esac
o=$(python3 "$HERE/difftest.py" "$U" --cases "$HERE/cases_c" ${SAN:+--san} 2>&1 | tail -3); has "casos so do C (return no finally): Python e C iguais" "$o" "ok 1"
o=$(python3 "$HERE/difftest.py" "$U" -n "${N_RANDOM:-40}" --seed 1 ${SAN:+--san} 2>&1 | tail -3); has "programas aleatorios: Python e C iguais" "$o" "ok ${N_RANDOM:-40};"
echo; [ $BAD -eq 0 ] && echo "OK $N checks" || echo "FAILED $BAD of $N"
exit $BAD
