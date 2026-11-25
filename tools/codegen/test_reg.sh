#!/bin/sh
# Testes do registro de operacoes (docs/ucge.md, "Registro de operacoes"): carga de dados, extensao sem recompilar, regras de simplificacao.
# Uso: sh tools/ucge/test_reg.sh        (CC=gcc; SAN=1 liga ASan/UBSan)
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
T=$(mktemp -d) || exit 2; trap 'rm -rf "$T"' EXIT
CC=${CC:-gcc}; SANF=""; [ -n "$SAN" ] && SANF="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC -O1 -g -Wall -Wno-unused-function $SANF -I "$ROOT" "$HERE/ucgecli.c" -o "$T/ucgecli" -lm 2>"$T/cc.log" || { cat "$T/cc.log"; echo "FAIL build"; exit 1; }
[ -s "$T/cc.log" ] && { cat "$T/cc.log"; echo "FAIL build warnings"; exit 1; }
U="$T/ucgecli"; N=0; BAD=0
ok()   { N=$((N+1)); echo "ok   $1"; }
fail() { N=$((N+1)); BAD=$((BAD+1)); echo "FAIL $1"; [ -n "$2" ] && echo "     got: $(printf '%s' "$2" | head -c 500 | tr '\n' '|')"; }
has()  { case "$2" in *"$3"*) ok "$1";; *) fail "$1 (missing: $3)" "$2";; esac; }
hasnt(){ case "$2" in *"$3"*) fail "$1 (unexpected: $3)" "$2";; *) ok "$1";; esac; }

o=$("$U" ops); n=$(printf '%s\n' "$o" | tail -n +2 | wc -l); [ "$n" = 31 ] && ok "registro: 31 operacoes embutidas" || fail "registro: $n operacoes" "$o"
has "lista: nome, nivel e efeito" "$o" "push        2     L1  mutates void"
# toda operacao tem py, js e ts (modelo ou unsupported explicito)
"$U" ops --dump > "$T/d1.ops"
miss=""; for op in $(printf '%s\n' "$o" | tail -n +2 | cut -d' ' -f1); do
  blk=$(awk -v n="$op" '$1=="op"{on=($2==n)} on' "$T/d1.ops")
  for t in py js ts; do printf '%s\n' "$blk" | grep -q "^  $t\(\[[a-z]*\]\)\? " || miss="$miss $op/$t"; done
done
[ -z "$miss" ] && ok "toda operacao tem py, js e ts (ou unsupported)" || fail "sem mapeamento:" "$miss"
# o dump volta a ser lido e da o mesmo dump (carregador e dado)
"$U" --ops "$T/d1.ops" ops --dump > "$T/d2.ops"; cmp -s "$T/d1.ops" "$T/d2.ops" && ok "dump -> carga -> dump identico" || fail "dump round trip" "$(diff "$T/d1.ops" "$T/d2.ops" | head -5)"
# extensao sem recompilar
cat > "$T/c.csp" <<'XX'
fn f(a: int, s: str) -> int
  let b = clamp(a, 0, 10)
  return b + count(s, "x")
test f(15, "axbx") == 12
test f(-3, "") == 0
XX
o=$("$U" check "$T/c.csp"); has "sem o arquivo: clamp e desconhecido" "$o" "unknown function \`clamp\`"
o=$("$U" --ops "$HERE/ops/extra.ops" run python "$T/c.csp"); has "operacao do usuario gera e roda" "$o" "2 passed, 0 failed"
o=$("$U" --ops "$HERE/ops/extra.ops" gen python "$T/c.csp"); has "modelo de texto aplicado" "$o" "min(max(a, 0), 10)"; has "modelo com {*1}" "$o" 's.count("x")'
o=$(KATANA_UCGE_OPS="$HERE/ops/extra.ops" "$U" check "$T/c.csp"); has "variavel KATANA_UCGE_OPS" "$o" "ok"
o=$("$U" --ops "$HERE/ops/extra.ops" ops); has "operacao do usuario na lista" "$o" "clamp"
printf 'fn f(a: int) -> int\n  return clamp(a, 1)\n' > "$T/c2.csp"; o=$("$U" --ops "$HERE/ops/extra.ops" check "$T/c2.csp"); has "aridade vem do registro" "$o" "clamp called with 2 arguments (expected clamp(x, lo, hi))"
printf 'fn f(a: str) -> int\n  return clamp(a, 1, 2)\n' > "$T/c3.csp"; o=$("$U" --ops "$HERE/ops/extra.ops" check "$T/c3.csp"); has "tipo do argumento vem do registro" "$o" "clamp(x, lo, hi): argument 1 expected a number, found str"
printf 'fn clamp(a: int) -> int\n  return a\n' > "$T/c4.csp"; o=$("$U" --ops "$HERE/ops/extra.ops" check "$T/c4.csp"); has "funcao com nome de operacao do usuario" "$o" "has the name of a built-in"
printf 'fn f(a: int) -> int\n  oldprint(a)\n  return a\n' > "$T/c5.csp"; o=$("$U" --ops "$HERE/ops/extra.ops" gen python "$T/c5.csp"); has "unsupported explicito traz a mensagem" "$o" "oldprint is only available for the web targets"
# erros do arquivo de dados
printf 'op foo 1 1 level=9\n' > "$T/b1.ops"; o=$("$U" --ops "$T/b1.ops" ops 2>&1); has "arquivo: nivel invalido com linha" "$o" "line 1: level must be 1, 2 or 3"
printf 'op foo 1 1\n  arg 0 banana : x\n' > "$T/b2.ops"; o=$("$U" --ops "$T/b2.ops" ops 2>&1); has "arquivo: tipo desconhecido" "$o" "line 2: unknown type in"
printf 'op foo 1 1\n  zzz 1\n' > "$T/b3.ops"; o=$("$U" --ops "$T/b3.ops" ops 2>&1); has "arquivo: linha desconhecida" "$o" "line 2: unknown line \`zzz\`"
printf 'rule (ADD x 0 => x\n' > "$T/b4.ops"; o=$("$U" --ops "$T/b4.ops" ops 2>&1); has "arquivo: regra mal formada" "$o" "line 1: bad rule"
o=$("$U" --ops /nao/existe ops 2>&1); has "arquivo inexistente" "$o" "cannot open"
# simplificacao algebrica
simp() { printf '%s\n' "$3" > "$T/s.csp"; o=$("$U" gen python "$T/s.csp" --simp 2>&1); has "$1" "$o" "$2"; }
simp "x + 0 => x" "return a" 'fn f(a: int) -> int
  return a + 0'
simp "1 * x => x" "return a" 'fn f(a: int) -> int
  return 1 * a'
simp "x * 0 => 0 (int, pura)" "return 0" 'fn f(a: int) -> int
  return a * 0'
simp "x * 0 mantido se x chama funcao" "g(a) * 0" 'fn g(a: int) -> int
  return a
fn f(a: int) -> int
  return g(a) * 0'
simp "x // 0 nao e tocado por x * 0" "a // b * 0" 'fn f(a: int, b: int) -> int
  return a // b * 0'
simp "x - x => 0" "return 0" 'fn f(a: int) -> int
  return a - a'
simp "x / 1 mantido (divisao verdadeira)" "a / 1" 'fn f(a: int) -> float
  return a / 1'
simp "float + 0 mantido" "a + 0" 'fn f(a: float) -> float
  return a + 0'
simp "not not b" "return b" 'fn f(b: bool) -> bool
  return not (not b)'
simp "b and true" "return b" 'fn f(b: bool) -> bool
  return b and true'
simp "b or true (pura) => true" "return True" 'fn f(b: bool) -> bool
  return b or true'
simp "abs(abs(x))" "return abs(a)" 'fn f(a: int) -> int
  return abs(abs(a))'
simp "upper(upper(s))" "return s.upper()" 'fn f(s: str) -> str
  return upper(upper(s))'
simp "reverse(reverse(s)) so em str" "return s" 'fn f(s: str) -> str
  return reverse(reverse(s))'
simp "reverse(reverse(xs)) mantido em lista" "[::-1][::-1]" 'fn f(xs: list[int]) -> list[int]
  return reverse(reverse(xs))'
simp "ponto fixo: (a + 0) * 1" "return a" 'fn f(a: int) -> int
  return (a + 0) * 1'
simp "sem regra aplicavel: nada muda" "simplified 0" 'fn f(a: int) -> int
  return a'
printf 'fn f(a: int) -> int\n  return clamp(clamp(a, 0, 5), 0, 5)\n' > "$T/u.csp"; o=$("$U" --ops "$HERE/ops/extra.ops" gen python "$T/u.csp" --simp 2>&1); has "regra do usuario" "$o" "return min(max(a, 0), 5)"
# a simplificacao preserva o comportamento: roda as 15 tarefas simplificadas
good=0; for f in "$HERE"/tasks/*.csp; do st=$("$U" stats "$f"); case "$st" in *"raw 0"*) o=$("$U" gen python "$f" --simp --tests 2>/dev/null >"$T/g.py"; python3 -I "$T/g.py" 2>&1); case "$o" in *FAIL*|*Traceback*) fail "tarefa simplificada $(basename $f)" "$o";; *) good=$((good+1));; esac;; esac; done
[ $good = 13 ] && ok "13 tarefas sem RAW simplificadas continuam passando" || fail "tarefas simplificadas: $good"
echo; [ $BAD -eq 0 ] && echo "OK $N checks" || echo "FAILED $BAD of $N"
exit $BAD
