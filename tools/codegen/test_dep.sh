#!/bin/sh
# Testes do grafo de dependencia e do code cache (docs/codegen.md, "Dependencia e cache"). Uso: sh tools/codegen/test_dep.sh   (SAN=1 liga ASan/UBSan)
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
T=$(mktemp -d) || exit 2; trap 'rm -rf "$T"' EXIT
CC=${CC:-gcc}; SANF=""; [ -n "$SAN" ] && SANF="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC -O1 -g -Wall -Wno-unused-function $SANF -I "$ROOT" "$HERE/codegencli.c" -o "$T/codegencli" -lm 2>"$T/cc.log" || { cat "$T/cc.log"; echo "FAIL build"; exit 1; }
[ -s "$T/cc.log" ] && { cat "$T/cc.log"; echo "FAIL build warnings"; exit 1; }
$CC -O1 -g -DCGX_GEN_VERSION=2 -Wno-unused-function -I "$ROOT" "$HERE/codegencli.c" -o "$T/codegencli2" -lm 2>/dev/null
U="$T/codegencli"; N=0; BAD=0; export KATANA_KATANA_CODEGEN_CACHE="$T/cache"
ok()   { N=$((N+1)); echo "ok   $1"; }
fail() { N=$((N+1)); BAD=$((BAD+1)); echo "FAIL $1"; [ -n "$2" ] && echo "     got: $(printf '%s' "$2" | head -c 700 | tr '\n' '|')"; }
has()  { case "$2" in *"$3"*) ok "$1";; *) fail "$1 (missing: $3)" "$2";; esac; }
hasnt(){ case "$2" in *"$3"*) fail "$1 (unexpected: $3)" "$2";; *) ok "$1";; esac; }
eq()   { [ "$2" = "$3" ] && ok "$1" || fail "$1" "A: $2 | B: $3"; }
ne()   { [ "$2" != "$3" ] && ok "$1" || fail "$1 (should differ)" "$2"; }

# ---------------- grafo ----------------
cat > "$T/g1.kcode" <<'XX'
fn top(a: int) -> int
  return mid(a) + 1
fn mid(a: int) -> int
  return leaf(a) * LIMIT
const LIMIT = 3
fn leaf(a: int) -> int
  return a
fn solo(a: int) -> int
  return a
XX
o=$("$U" dep "$T/g1.kcode"); has "ordem: dependencias primeiro, estavel" "$o" "order: LIMIT leaf mid top solo"
has "aresta de chamada" "$o" "edge top -> mid (call, line 2)"; has "aresta de constante" "$o" "edge mid -> LIMIT (use, line 4)"
has "sem ciclos" "$o" "cycle errors 0 recursion groups 0"
cat > "$T/g2.kcode" <<'XX'
fn solo(a: int) -> int
  return a
fn top(a: int) -> int
  return leaf(a)
fn leaf(a: int) -> int
  return a
XX
o=$("$U" dep "$T/g2.kcode"); has "empate resolvido pela ordem de declaracao" "$o" "order: solo leaf top"
# ciclos
printf 'const A = B + 1\nconst B = A + 1\nfn f(x: int) -> int\n  return x\n' > "$T/c1.kcode"; o=$("$U" dep "$T/c1.kcode" --nocheck); has "constantes que se leem: erro com caminho" "$o" "constant cycle: A -> B -> A"; has "mensagem diz como quebrar" "$o" "third constant"; has "contagem de erros" "$o" "cycle errors 1"
printf 'class A\n  field b: B\nclass B\n  field a: A\n' > "$T/c2.kcode"; o=$("$U" dep "$T/c2.kcode" --nocheck); has "campos de tipo que se contem" "$o" "type cycle: A -> B -> A"
printf 'class Node\n  field next: Node\n' > "$T/c3.kcode"; o=$("$U" dep "$T/c3.kcode" --nocheck); has "classe com campo do proprio tipo" "$o" "type cycle: Node"
printf 'fn fact(n: int) -> int\n  if n <= 1\n    return 1\n  return n * fact(n - 1)\n' > "$T/c4.kcode"; o=$("$U" dep "$T/c4.kcode"); has "auto-recursao permitida" "$o" "note: \`fact\` is recursive"; has "sem erro" "$o" "cycle errors 0 recursion groups 1"
o=$("$U" dep "$T/c4.kcode" --no-rec); has "auto-recursao proibida: mensagem" "$o" "calls itself, and recursion is not allowed here. Rewrite it as a loop"
cat > "$T/c5.kcode" <<'XX'
fn isEven(n: int) -> bool
  if n == 0
    return true
  return isOdd(n - 1)
fn isOdd(n: int) -> bool
  if n == 0
    return false
  return isEven(n - 1)
XX
o=$("$U" dep "$T/c5.kcode"); has "recursao mutua: grupo permitido" "$o" "mutual recursion group (2 units): isEven -> isOdd -> isEven"; has "contado como grupo" "$o" "recursion groups 1"
o=$("$U" dep "$T/c5.kcode" --no-rec); has "recursao mutua proibida: caminho com linhas" "$o" "isEven (line 4) -> isOdd (line 8) -> isEven"; has "e a saida" "$o" "merge them into one function"
printf 'const X = f()\nfn f() -> int\n  return X\n' > "$T/c6.kcode"; o=$("$U" dep "$T/c6.kcode" --nocheck); has "ciclo de inicializacao (const chama funcao que le a const)" "$o" "initialization cycle: X -> f -> X"
printf 'class P\n  field n: int\n  fn get() -> int\n    return self.n\nfn mk(p: P) -> int\n  return p.get()\n' > "$T/c7.kcode"; o=$("$U" dep "$T/c7.kcode"); has "uso de tipo da classe" "$o" "edge mk -> P (type"
# SCC grande: anel de 2000 funcoes, sem estourar pilha
awk 'BEGIN{n=2000; for(i=0;i<n;i++){printf "fn r%d(a: int) -> int\n  return r%d(a)\n", i, (i+1)%n}}' > "$T/ring.kcode"; o=$("$U" dep "$T/ring.kcode" 2>&1 | tail -2); has "anel de 2000 funcoes: um grupo" "$o" "recursion groups 1"
# ---------------- chaves ----------------
k1=$("$U" key "$T/g1.kcode")
printf 'fn   top( a : int )  ->  int\n\n    return   mid(a)+1\n\n\nfn mid(a:int)->int\n    return leaf(a)*LIMIT\nconst   LIMIT=3\nfn leaf(a: int) -> int\n  return a\nfn solo(a: int) -> int\n  return a\n' > "$T/g1b.kcode"
eq "chaves independem de espacos e linhas em branco" "$k1" "$("$U" key "$T/g1b.kcode")"
printf 'fn solo(a: int) -> int\n  return a\nfn leaf(a: int) -> int\n  return a\nconst LIMIT = 3\nfn mid(a: int) -> int\n  return leaf(a) * LIMIT\nfn top(a: int) -> int\n  return mid(a) + 1\n' > "$T/g1c.kcode"
eq "chaves independem da ordem de declaracao" "$(printf '%s\n' "$k1" | sort)" "$("$U" key "$T/g1c.kcode" | sort)"
printf 'fn def top(a: int) -> int\n  return mid(a) + 1\n' > /dev/null
sed 's/^fn /def /' "$T/g1.kcode" > "$T/g1d.kcode"; eq "chaves independem do apelido (def/fn)" "$k1" "$("$U" key "$T/g1d.kcode")"
sed 's/return leaf(a) \* LIMIT/return leaf(a) * LIMIT + 1/' "$T/g1.kcode" > "$T/g1e.kcode"; k2=$("$U" key "$T/g1e.kcode")
printf '%s\n' "$k1" > "$T/k1"; printf '%s\n' "$k2" > "$T/k2"; chg=$(paste "$T/k1" "$T/k2" | awk '$2!=$4{printf "%s ", $1}')
eq "mudou mid: mudam mid e top (dependente), nao LIMIT, leaf, solo" "$chg" "top mid "
sed 's/^const LIMIT = 3/const LIMIT = 4/' "$T/g1.kcode" > "$T/g1f.kcode"; "$U" key "$T/g1f.kcode" > "$T/k3"; paste "$T/k1" "$T/k3" | awk '$2!=$4{printf "%s ", $1}' > "$T/chg"; eq "mudou a constante: mudam ela, mid (le) e top (transitivo)" "$(cat $T/chg)" "top mid LIMIT "
k3=$("$U" key "$T/c5.kcode"); printf 'fn isEven(n: int) -> bool\n  if n == 0\n    return false\n  return isOdd(n - 1)\nfn isOdd(n: int) -> bool\n  if n == 0\n    return false\n  return isEven(n - 1)\n' > "$T/c5b.kcode"; printf '%s\n' "$k3" > "$T/k4"; "$U" key "$T/c5b.kcode" > "$T/k5"; paste "$T/k4" "$T/k5" | awk '$2!=$4{n++} END{print n}' > "$T/n"; eq "recursao mutua: mudar um membro muda o bloco todo" "$(cat $T/n)" "2"
# JSON equivale ao texto
cat > "$T/j.json" <<'XX'
[{"op":"FUNCTION","name":"f","params":[{"name":"a","type":"int"}],"returns":"int","body":[{"op":"RETURN","value":"a + 1"}]}]
XX
printf 'fn f(a: int) -> int\n  return a + 1\n' > "$T/j.kcode"; eq "chave de JSON = chave do texto" "$("$U" key "$T/j.json")" "$("$U" key "$T/j.kcode")"
o1=$("$U" dump "$T/j.json"); o2=$("$U" dump "$T/j.kcode"); eq "forma canonica identica" "$o1" "$o2"
eq "versao do gerador entra na chave" "x$("$U" key "$T/g1.kcode" | head -1 | cut -d' ' -f2 | cut -c1-16)" "x$("$U" key "$T/g1.kcode" | head -1 | cut -d' ' -f2)"
[ -x "$T/codegencli2" ] && { ne "versao do gerador muda todas as chaves" "$k1" "$("$T/codegencli2" key "$T/g1.kcode")"; }
# ---------------- cache ----------------
"$U" cache clear > /dev/null
all=1; for f in "$HERE"/tasks/*.kcode; do
  a=$("$U" gen python "$f" --tests 2>&1); b=$("$U" gen python "$f" --tests --cache 2>/dev/null); c=$("$U" gen python "$f" --tests --cache 2>/dev/null)
  [ "$a" = "$b" ] && [ "$a" = "$c" ] || { all=0; fail "cache: tarefa $(basename $f) difere do gerador direto" "$a"; }
done; [ $all = 1 ] && ok "15 tarefas: saida com cache (frio e quente) identica a do gerador direto"
"$U" cache clear > /dev/null
r=$("$U" gen python "$T/g1.kcode" --cache 2>&1 >/dev/null); has "frio: so erros" "$r" "hits 0 misses 5"
r=$("$U" gen python "$T/g1.kcode" --cache 2>&1 >/dev/null); has "quente: so acertos" "$r" "hits 5 misses 0"
ls "$T/cache" | grep -q '^\.tmp' && fail "sobrou arquivo temporario" || ok "escrita atomica: nenhum arquivo temporario sobrou"
r=$("$U" gen python "$T/g1e.kcode" --cache 2>&1 >/dev/null); has "mudou mid: so mid e top regeneram" "$r" "hits 3 misses 2"
r=$("$U" gen python "$T/g1f.kcode" --cache 2>&1 >/dev/null); has "mudou a constante: ela, mid e top regeneram" "$r" "hits 2 misses 3"
r=$("$U" gen python "$T/g1c.kcode" --cache 2>&1 >/dev/null); has "ordem de declaracao diferente: tudo acerta (unidades)" "$r" "hits 5 misses 0"
a=$("$U" gen python "$T/g1c.kcode" --cache 2>/dev/null); b=$("$U" gen python "$T/g1c.kcode"); eq "e o codigo sai na ordem do arquivo, igual ao direto" "$a" "$b"
r=$("$U" gen python "$T/g1.kcode" --cache --width 40 2>&1 >/dev/null); has "largura diferente: outra chave" "$r" "misses 5"
r=$("$U" gen python "$T/g1.kcode" --cache --no-comments 2>&1 >/dev/null); has "comentarios desligados: outra chave" "$r" "misses 5"
[ -x "$T/codegencli2" ] && { r=$("$T/codegencli2" gen python "$T/g1.kcode" --cache 2>&1 >/dev/null); has "versao do gerador diferente: nada acerta" "$r" "hits 0 misses 5"; }
KATANA_KATANA_CODEGEN_OPS="$HERE/ops/extra.ops" "$U" gen python "$T/g1.kcode" --cache 2>&1 >/dev/null | grep -q "misses 5" && ok "outro registro de operacoes: nada acerta" || fail "registro diferente deve invalidar"
# ordem topologica de geracao
printf 'fn top(a: int) -> int\n  return base(a) + 1\nfn base(a: int) -> int\n  return a\n' > "$T/t.kcode"
a=$("$U" gen python "$T/t.kcode" --cache --topo 2>/dev/null | grep -n 'def ' | head -1); has "--topo: a dependencia sai primeiro" "$a" "def base"
a=$("$U" gen python "$T/t.kcode" --cache 2>/dev/null | grep -n 'def ' | head -1); has "sem --topo: ordem do arquivo" "$a" "def top"
# corrupcao
"$U" cache clear >/dev/null; "$U" gen python "$T/g1.kcode" --cache >/dev/null 2>&1; ref=$("$U" gen python "$T/g1.kcode")
f=$(ls "$T"/cache/*.uc | head -1); printf 'XXXX' | dd of="$f" bs=1 seek=100 conv=notrunc 2>/dev/null
r=$("$U" gen python "$T/g1.kcode" --cache 2>"$T/err" ); eq "byte alterado: saida continua correta" "$r" "$ref"; has "e conta como corrompido" "$(cat $T/err)" "corrupt"
for f in "$T"/cache/*.uc; do head -c 30 "$f" > "$f.t"; mv "$f.t" "$f"; done; r=$("$U" gen python "$T/g1.kcode" --cache 2>/dev/null); eq "arquivos truncados: saida correta" "$r" "$ref"
for f in "$T"/cache/*.uc; do printf 'lixo' > "$f"; done; r=$("$U" gen python "$T/g1.kcode" --cache 2>/dev/null); eq "arquivos com lixo: saida correta" "$r" "$ref"
for f in "$T"/cache/*.uc; do : > "$f"; done; r=$("$U" gen python "$T/g1.kcode" --cache 2>/dev/null); eq "arquivos vazios: saida correta" "$r" "$ref"
r=$("$U" gen python "$T/g1.kcode" --cache 2>&1 >/dev/null); has "depois de reparar tudo acerta" "$r" "corrupt 0"
# pasta inutilizavel: gera sem cache
r=$("$U" gen python "$T/g1.kcode" --cache /proc/nao/pode 2>&1); has "pasta impossivel: gera mesmo assim" "$r" "def top"
# limite de tamanho e LRU
"$U" cache clear >/dev/null; export KATANA_KATANA_CODEGEN_CACHE_MAX=30000
for i in 1 2 3 4 5 6 7 8; do awk -v s=$i 'BEGIN{for(i=0;i<40;i++) printf "fn g%d_%d(a: int) -> int\n  return a + %d\n", s, i, i+s}' > "$T/m$i.kcode"; "$U" gen python "$T/m$i.kcode" --cache >/dev/null 2>&1; done
"$U" cache trim > "$T/trim"; sz=$(cut -d' ' -f2 "$T/trim"); [ "$sz" -le 30000 ] && ok "limite de tamanho respeitado ($sz <= 30000 bytes)" || fail "tamanho $sz" "$(cat $T/trim)"
du=$(cat "$T"/cache/*.uc | wc -c); [ "$du" -le 30000 ] && ok "soma dos arquivos <= limite" || fail "soma $du"
unset KATANA_KATANA_CODEGEN_CACHE_MAX
# LRU: o mais recente sobrevive
"$U" cache clear >/dev/null; export KATANA_KATANA_CODEGEN_CACHE_MAX=2400
printf 'fn aa(a: int) -> int\n  return a + 1\n' > "$T/l1.kcode"; printf 'fn bb(a: int) -> int\n  return a + 2\n' > "$T/l2.kcode"; printf 'fn cc(a: int) -> int\n  return a + 3\n' > "$T/l3.kcode"
"$U" gen python "$T/l1.kcode" --cache >/dev/null 2>&1; sleep 1.1; "$U" gen python "$T/l2.kcode" --cache >/dev/null 2>&1; sleep 1.1; "$U" gen python "$T/l1.kcode" --cache >/dev/null 2>&1
sleep 1.1; unset KATANA_KATANA_CODEGEN_CACHE_MAX; export KATANA_KATANA_CODEGEN_CACHE_MAX=250; "$U" cache trim >/dev/null; unset KATANA_KATANA_CODEGEN_CACHE_MAX
r1=$("$U" gen python "$T/l1.kcode" --cache 2>&1 >/dev/null); r2=$("$U" gen python "$T/l2.kcode" --cache 2>&1 >/dev/null)
case "$r1$r2" in *"misses 0"*) ok "LRU: ao despejar, o menos recente (l2) saiu e um recente ficou";; *) fail "LRU" "$r1 / $r2";; esac
# cache dir pela variavel de ambiente
p=$("$U" cache path); has "pasta pela variavel KATANA_KATANA_CODEGEN_CACHE" "$p" "$T/cache"
p=$(env -u KATANA_KATANA_CODEGEN_CACHE HOME=/tmp/hh_$$ "$U" cache path); has "pasta padrao ~/.katana/codegen-cache" "$p" "/tmp/hh_$$/.katana/codegen-cache"; rm -rf /tmp/hh_$$
# medicao
o=$("$U" cachebench 300 "$T/cb"); has "medicao: sem cache, frio, quente" "$o" "warm cache"; has "mudar a ultima: 1 erro" "$o" "(1 misses"; has "mudar a primeira: todas regeneram" "$o" "(300 misses"
echo "$o" | sed 's/^/     /'
echo; [ $BAD -eq 0 ] && echo "OK $N checks" || echo "FAILED $BAD of $N"
exit $BAD
