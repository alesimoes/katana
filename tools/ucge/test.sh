#!/bin/sh
# Testes do nucleo do UCGE (docs/ucge.md), sem modelo e sem o katana: compila tools/ucge/ucgecli.c e confere
# parser, validador, geracao em Python, execucao dos testes dados, JSON equivalente, RAW e determinismo.
# Uso: sh tools/ucge/test.sh        (CC=gcc por padrao; SAN=1 compila com -fsanitize=address,undefined)
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
T=$(mktemp -d) || exit 2; trap 'rm -rf "$T"' EXIT
CC=${CC:-gcc}; SANF=""; [ -n "$SAN" ] && SANF="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC -O1 -g -Wall -Wno-unused-function $SANF -I "$ROOT" "$HERE/ucgecli.c" -o "$T/ucgecli" -lm 2>"$T/cc.log" || { cat "$T/cc.log"; echo "FAIL build"; exit 1; }
[ -s "$T/cc.log" ] && { cat "$T/cc.log"; echo "FAIL build warnings"; exit 1; }
U="$T/ucgecli"; N=0; BAD=0
ok()   { N=$((N+1)); echo "ok   $1"; }
fail() { N=$((N+1)); BAD=$((BAD+1)); echo "FAIL $1"; [ -n "$2" ] && echo "     got: $(printf '%s' "$2" | head -c 600 | tr '\n' '|')"; }
has()  { case "$2" in *"$3"*) ok "$1";; *) fail "$1 (missing: $3)" "$2";; esac; }
hasnt(){ case "$2" in *"$3"*) fail "$1 (unexpected: $3)" "$2";; *) ok "$1";; esac; }

o=$("$U" selftest); has "selftest: tipos, unificacao, pretty-printer, interning" "$o" "selftest: ok"

# ---- as 15 tarefas: gera, roda os testes dados, e conta a cobertura sem RAW ----
noraw=0; total=0
for f in "$HERE"/tasks/*.csp; do
  b=$(basename "$f" .csp); total=$((total+1))
  st=$("$U" stats "$f"); case "$st" in *"raw 0"*) noraw=$((noraw+1)); r=1;; *) r=0;; esac
  if [ $r = 1 ]; then o=$("$U" run python "$f"); case "$o" in *" 0 failed"*) ok "tarefa $b: gerado em Python e os testes passam";; *) fail "tarefa $b" "$o";; esac
  else o=$("$U" gen python "$f"); case "$o" in *"def "*) ok "tarefa $b: usa RAW (gera; nao confiavel)";; *) fail "tarefa $b" "$o";; esac; fi
done
[ "$total" = 15 ] && ok "15 tarefas" || fail "numero de tarefas: $total"
[ "$noraw" = 13 ] && ok "cobertura sem RAW: 13 de 15" || fail "cobertura sem RAW: $noraw de $total"

# ---- erros do validador: linha e o que se esperava ----
chk() { printf '%s\n' "$3" > "$T/e.csp"; o=$("$U" check "$T/e.csp"); has "$1" "$o" "$2"; }
chk "variavel nao declarada" "line 2: variable \`b\` is not declared" 'fn f(a: int) -> int
  return b'
chk "parametro duplicado" "duplicate parameter \`a\`" 'fn f(a: int, a: int) -> int
  return a'
chk "retorno ausente" "some path reaches the end without \`return\`" 'fn f(a: int) -> int
  if a > 0
    return 1'
chk "tipo incompativel (ADD int e str)" "ADD of int and str" 'fn f(a: int) -> int
  return a + "x"'
chk "retorno com tipo errado" "declares \`-> str\` but returns int" 'fn f(a: int) -> str
  return a'
chk "condicao nao booleana" "must be bool, found int" 'fn f(a: int) -> int
  if a
    return 1
  return 0'
chk "funcao desconhecida" "unknown function \`foo\`" 'fn f(a: int) -> int
  return foo(a)'
chk "aridade" "called with 2 arguments, expected 1" 'fn g(a: int) -> int
  return a
fn f(a: int) -> int
  return g(a, 1)'
chk "redeclaracao/sombra" "already declared in an outer block" 'fn f(a: int) -> int
  let s = 0
  for i in 0..a
    let s = 1
  return s'
chk "atribuicao sem let" "variable \`z\` is not declared" 'fn f(a: int) -> int
  z = 2
  return z'
chk "metodo em lista" "lists, strings and maps have no methods" 'fn f() -> int
  let xs = [1]
  xs.push(2)
  return 0'
chk "break fora de laco" "\`break\` outside a loop" 'fn f() -> int
  break
  return 0'
chk "bloco sem indentacao" "expected an indented block after the function header" 'fn f() -> int
return 1'
chk "token inesperado (o que esperava)" "expected a parameter name, found the end of the line" 'fn f(
  return 1'
chk "nome de funcao embutida" "has the name of a built-in" 'fn len(a: int) -> int
  return a'
chk "chave de mapa nao-str" "map keys must be str" 'fn f() -> int
  let m = {1: 2}
  return 0'
chk "import fora da UIR" "imports are not part of the UIR" 'import os'
chk "heranca fora da UIR" "inheritance is not part of the UIR" 'class A extends B
  field x: int = 0'
chk "campo inexistente" "class \`A\` has no field \`y\`" 'class A
  field x: int = 0
fn f(a: A) -> int
  return a.y'
chk "raw com linguagem desconhecida" "unknown raw language \`cobol\`" 'fn f() -> int
  raw cobol
    DISPLAY 1
  return 1'
o=$("$U" check "$HERE/tasks/01_sum_default.csp"); has "especificacao valida" "$o" "ok"

# ---- geracao: estilo, largura, determinismo, JSON equivalente, RAW ----
printf 'fn f(a: int) -> int\n  return a\n' > "$T/d.csp"
a=$("$U" gen python "$T/d.csp"); b=$("$U" gen python "$T/d.csp"); [ "$a" = "$b" ] && ok "determinismo: mesma especificacao, mesmo codigo" || fail "determinismo"
cat > "$T/w.csp" <<'XX'
fn longCall(alpha: int, beta: int, gamma: int, delta: int, epsilon: int, zeta: int, eta: int) -> int
  return alpha
XX
o=$("$U" gen python "$T/w.csp" --width 200); has "largura 200: cabecalho numa linha" "$o" "def longCall(alpha: int, beta: int, gamma: int, delta: int, epsilon: int, zeta: int, eta: int) -> int:"
o=$("$U" gen python "$T/w.csp" --width 60); has "largura 60: parametros quebrados" "$o" "def longCall(
    alpha: int,"
cat > "$T/c.csp" <<'XX'
# comentario do autor
fn f(a: int) -> int
  doc soma um
  # passo
  return a + 1
XX
o=$("$U" gen python "$T/c.csp"); has "docstring e comentario" "$o" '"""soma um"""'; has "comentario de linha" "$o" "# passo"
o=$("$U" gen python "$T/c.csp" --no-comments); hasnt "sem comentarios" "$o" "soma um"
cat > "$T/j.json" <<'XX'
[{"op":"FUNCTION","name":"sumValues","params":[{"name":"a","type":"int"},{"name":"b","type":"int","default":0}],"returns":"int","body":[{"op":"RETURN","value":{"op":"ADD","args":["a","b"]}}]},
 {"op":"TEST","expr":"sumValues(2, 3)","expect":5}]
XX
o=$("$U" run python "$T/j.json"); has "JSON equivalente roda" "$o" "tests: 1 passed, 0 failed"
a=$("$U" gen python "$T/j.json" | head -2); b=$("$U" gen python "$HERE/tasks/01_sum_default.csp" | head -2); [ "$a" = "$b" ] && ok "JSON e texto geram o mesmo codigo" || fail "JSON x texto" "$a / $b"
printf '[{"op":"FOO"}]' > "$T/bad.json"; o=$("$U" check "$T/bad.json"); has "JSON: operacao desconhecida lista as validas" "$o" 'unknown operation "FOO" (expected FUNCTION'
cat > "$T/r.csp" <<'XX'
fn twice(x: int) -> int
  raw python defines y
    y = x * 2
  return y
test twice(4) == 8
XX
o=$("$U" run python "$T/r.csp"); has "RAW entra como no opaco e roda" "$o" "tests: 1 passed, 0 failed"
cat > "$T/r2.csp" <<'XX'
fn twice(x: int) -> int
  raw js defines y
    const y = x * 2;
  return y
XX
o=$("$U" gen python "$T/r2.csp"); has "RAW sem versao para o alvo" "$o" "has no \`raw python\` version"
cat > "$T/m.csp" <<'XX'
fn firstWord(s: str, sep: str = " ", acc: list[str] = []) -> str
  let parts = split(s, sep)
  return parts[0]
test firstWord("ab cd") == "ab"
XX
o=$("$U" gen python "$T/m.csp"); has "default mutavel vira None" "$o" "acc=None"; has "default mutavel criado no corpo" "$o" "if acc is None:"
o=$("$U" run python "$T/m.csp"); has "default mutavel: teste passa" "$o" "1 passed"
printf 'fn f(class: int) -> int\n  return class\n' > "$T/k.csp"; o=$("$U" gen python "$T/k.csp"); has "palavra reservada do Python" "$o" "is a Python keyword"
printf 'fn f(a: int) -> int\n  return a\ntest f(1) == 2\n' > "$T/t.csp"; o=$("$U" run python "$T/t.csp"); has "teste que falha e reportado" "$o" "1 failed"
o=$("$U" stats "$HERE/tasks/08_counter_class.csp"); has "stats: classe e nivel L3" "$o" "classes 1"; has "stats nivel" "$o" "level L3"
echo; [ $BAD -eq 0 ] && echo "OK $N checks" || echo "FAILED $BAD of $N"
exit $BAD
