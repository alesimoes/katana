#!/bin/sh
# Testes dos backends JavaScript e TypeScript do codegen (docs/codegen.md, "Backends JS e TS"), sem modelo e sem o katana:
#   - as 13 tarefas sem RAW geradas em JS e rodadas no node (e em TS, compiladas com o tsc de KATANA_CODEGEN_TSC, se houver); as de RAW so geram;
#   - Python x JS (x TS) em programas de borda escritos a mao (tools/codegen/js/edge_*.kcode) e em programas aleatorios (jsfuzz.py);
#   - estilo do codigo gerado e mensagens de erro do gerador.
# Uso: sh tools/codegen/jstest.sh [N]     (SAN=1 compila com ASan/UBSan)   N = programas aleatorios (padrao 60); KATANA_CODEGEN_TSC=/caminho/do/tsc inclui TypeScript (ex.: npm i typescript; node_modules/.bin/tsc)
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
T=$(mktemp -d) || exit 2; trap 'rm -rf "$T"' EXIT
NFUZZ=${1:-60}; CC=${CC:-gcc}; SANF=""; [ -n "$SAN" ] && SANF="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
command -v node >/dev/null || { echo "SKIP: node nao encontrado"; exit 0; }
$CC -O1 -g -Wall -Wno-unused-function $SANF -I "$ROOT" "$HERE/codegencli.c" -o "$T/codegencli" -lm 2>"$T/cc.log" || { cat "$T/cc.log"; echo "FAIL build"; exit 1; }
[ -s "$T/cc.log" ] && { cat "$T/cc.log"; echo "FAIL build warnings"; exit 1; }
U="$T/codegencli"; N=0; BAD=0
ok()   { N=$((N+1)); echo "ok   $1"; }
fail() { N=$((N+1)); BAD=$((BAD+1)); echo "FAIL $1"; [ -n "$2" ] && echo "     got: $(printf '%s' "$2" | head -c 700 | tr '\n' '|')"; }
has()  { case "$2" in *"$3"*) ok "$1";; *) fail "$1 (missing: $3)" "$2";; esac; }
hasnt(){ case "$2" in *"$3"*) fail "$1 (unexpected: $3)" "$2";; *) ok "$1";; esac; }

# ---- as tarefas ----
for f in "$HERE"/tasks/*.kcode; do
  b=$(basename "$f" .kcode); st=$("$U" stats "$f")
  case "$st" in
    *"raw 0"*)
      o=$("$U" run js "$f"); case "$o" in *" 0 failed"*) ok "js $b: os testes passam no node";; *) fail "js $b" "$o";; esac
      if [ -n "$KATANA_CODEGEN_TSC" ]; then o=$("$U" run ts "$f"); case "$o" in *" 0 failed"*) ok "ts $b: tsc --strict e os testes passam";; *) fail "ts $b" "$o";; esac; fi;;
    *) o=$("$U" gen js "$f"); case "$o" in *"function "*) ok "js $b: usa RAW (gera; nao confiavel)";; *"has no \`raw js\` version"*) ok "js $b: RAW so em Python: erro que lista as versoes";; *) fail "js $b" "$o";; esac;;
  esac
done
o=$("$U" gen js "$HERE/tasks/15_fetch_title.kcode"); has "RAW sem versao js: mensagem lista as versoes" "$o" "has no \`raw js\` version (it has: python )"
o=$("$U" gen js "$HERE/tasks/14_json_total.kcode"); has "RAW js entra no corpo da funcao" "$o" 'const data = JSON.parse(require("fs").readFileSync(path, "utf8"));'
hasnt "RAW presente: sem \"use strict\" global" "$o" "use strict"
echo '{"items": [{"price": 1.5}, {"price": 2}]}' > "$T/d.json"
"$U" gen js "$HERE/tasks/14_json_total.kcode" > "$T/t14.js"; printf 'console.log(totalFromFile(process.argv[2]));\n' >> "$T/t14.js"
o=$(node "$T/t14.js" "$T/d.json" 2>&1); [ "$o" = "3.5" ] && ok "tarefa 14 roda com um arquivo real (any: _iter/_ix)" || fail "tarefa 14 com arquivo" "$o"

# ---- diferencial Python x JS (x TS) ----
for f in "$HERE"/js/edge_*.kcode; do
  o=$(python3 "$HERE/js/jsdiff.py" "$U" "$f" 2>&1); case "$o" in "ok "*) ok "diferencial $(basename "$f"): $(echo "$o" | sed 's/.*: //')";; *) fail "diferencial $(basename "$f")" "$o";; esac
done
o=$(python3 "$HERE/js/jsfuzz.py" "$U" "$NFUZZ" 1 "$T/fails" 2>&1 | tail -6); case "$o" in *"divergentes 0, invalidos (gerador) 0"*) ok "aleatorio: $(echo "$o" | tail -1)";; *) fail "aleatorio" "$o";; esac

# ---- estilo e erros do gerador ----
printf 'fn f(xs: list[int], s: str) -> str\n  let t = 0\n  let u = 1\n  for x in xs\n    t += x\n  return "sum " + str(t) + " of " + s + "\\n"\n' > "$T/s.kcode"
o=$("$U" gen js "$T/s.kcode" --no-comments); has "const quando nao reatribuido" "$o" "const u = 1;"; has "let quando reatribuido" "$o" "let t = 0;"; has "template string" "$o" '`sum ${t} of ${s}\n`'
has "for-of em lista" "$o" "for (const x of xs)"
o=$("$U" gen ts "$T/s.kcode"); has "TS: tipos dos parametros e do retorno" "$o" "function f(xs: number[], s: string): string"; has "TS: modulo" "$o" "export {};"
printf 'fn f(n: int) -> int\n  let m = len([1])\n  for i in 0..len([1, 2]) + n\n    n = n + i\n  return n // 2 + n %% 3 + m\n' > "$T/l.kcode"
o=$("$U" gen js "$T/l.kcode"); has "limite do laco avaliado uma vez" "$o" "for (let i = 0, _e1 = [1, 2].length + n; i < _e1; i++)"; has "// por literal: Math.floor" "$o" "Math.floor(n / 2)"; has "% por literal e operando desconhecido: ajudante" "$o" "_mod(n, 3)"
printf 'fn f(class: int) -> int\n  return class\n' > "$T/k.kcode"; o=$("$U" gen js "$T/k.kcode"); has "palavra reservada do JavaScript" "$o" "is a JavaScript keyword"
printf 'fn f(Math: int) -> int\n  return Math\n' > "$T/k.kcode"; o=$("$U" gen js "$T/k.kcode"); has "global que o codigo gerado usa" "$o" "would shadow a JavaScript global"
printf 'fn f(_x: int) -> int\n  return _x\n' > "$T/k.kcode"; o=$("$U" gen js "$T/k.kcode"); has "prefixo _ reservado" "$o" "reserved for the generated helpers"
printf 'fn f() -> int\n  return 9007199254740993\n' > "$T/k.kcode"; o=$("$U" gen js "$T/k.kcode"); has "inteiro fora de 2^53" "$o" "outside the exact range"
a=$("$U" gen js "$T/l.kcode"); b=$("$U" gen js "$T/l.kcode"); [ "$a" = "$b" ] && ok "determinismo" || fail "determinismo"
o=$("$U" gen js "$HERE/tasks/04_factorial.kcode" --width 30); has "largura 30 quebra" "$o" "function factorial(n) {"
node --check /dev/null 2>/dev/null
for f in "$HERE"/js/edge_*.kcode; do "$U" gen js "$f" --tests > "$T/c.js"; o=$(node --check "$T/c.js" 2>&1) && ok "node --check $(basename "$f")" || fail "node --check $(basename "$f")" "$o"; done
echo; [ $BAD -eq 0 ] && echo "OK $N checks" || echo "FAILED $BAD of $N"
exit $BAD
