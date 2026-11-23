#!/bin/sh
# Testa Read, Write, Edit e NotebookEdit sem modelo: sh tools/tooltest/files.sh /caminho/do/katana
# Trabalha numa pasta temporaria que cria e apaga. Uma linha por checagem; FAILED n ou OK n checks no fim.
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
export KATANA_LOG=${KATANA_LOG:-$(mktemp)}     # o log das chamadas destes testes nao vai para o ~/.katana/agente.log do dono
[ -x "$K" ] || { echo "uso: $0 <binario katana>"; exit 2; }
T=$(mktemp -d) || exit 2
HOME_SAVE=$HOME
HOME=$T/home; export HOME; mkdir -p "$HOME"
trap 'rm -rf "$T"' EXIT
cd "$T" || exit 2
P=$(pwd -P)
N=0; BAD=0
out=
run() { out=$("$K" tool "$@" 2>/dev/null); }
ok()   { N=$((N+1)); echo "ok   $1"; }
fail() { N=$((N+1)); BAD=$((BAD+1)); echo "FAIL $1"; echo "     got: $(printf '%s' "$out" | head -c 600 | tr '\n' '|')"; }
has()  { case "$out" in *"$2"*) ok "$1";; *) fail "$1 (missing: $2)";; esac; }
hasnt(){ case "$out" in *"$2"*) fail "$1 (unexpected: $2)";; *) ok "$1";; esac; }
eq()   { if [ "$out" = "$2" ]; then ok "$1"; else fail "$1 (want: $2)"; fi; }
fileis() { if [ "$(cat "$2")" = "$3" ]; then ok "$1"; else N=$((N+1)); BAD=$((BAD+1)); echo "FAIL $1"; echo "     file: $(head -c 400 "$2" | tr '\n' '|')"; fi; }
TAB=$(printf '\t')

# ---------------- Read ----------------
printf 'alpha\nbeta\ngamma\n' > a.txt
run Read '{"file_path":"'$P'/a.txt"}'
eq "Read: linhas numeradas" "     1${TAB}alpha
     2${TAB}beta
     3${TAB}gamma"
run Read '{"file_path":"a.txt"}'
has "Read: caminho relativo aceito" "     2${TAB}beta"
run Read '{"file_path":"a.txt","offset":2,"limit":1}'
has "Read: offset e limit mostram a linha" "     2${TAB}beta"
hasnt "Read: offset e limit nao mostram outras" "gamma"
has "Read: faixa parcial avisa o total" "of 3 total"
run Read '{"file_path":"a.txt","offset":"2"}'
has "Read: offset como texto" "     2${TAB}beta"
run Read '{"file_path":"a.txt","offset":9}'
has "Read: offset alem do fim da o numero de linhas" "has 3 lines"
run Read '{"file_path":"nao-existe.txt"}'
has "Read: arquivo inexistente" "error: "
has "Read: inexistente diz a pasta" "working folder"
: > empty.txt
run Read '{"file_path":"empty.txt"}'
has "Read: arquivo vazio" "exists but is empty"
mkdir -p dir
run Read '{"file_path":"dir"}'
has "Read: diretorio e erro" "is a directory"
has "Read: diretorio manda usar ls" "ls"
run Read '{}'
has "Read: sem file_path" "error: missing file_path"
printf 'PK\003\004zz' > x.zip
run Read '{"file_path":"x.zip"}'
has "Read: extensao binaria recusada" "binary format"
printf 'ab\000cd\n' > b.dat
run Read '{"file_path":"b.dat"}'
has "Read: NUL recusado" "binary file"
printf '\211PNG' > i.png
run Read '{"file_path":"i.png"}'
has "Read: imagem diz que nao ha visao" "no vision"
printf 'sem fim de linha' > nonl.txt
run Read '{"file_path":"nonl.txt"}'
eq "Read: ultima linha sem newline" "     1${TAB}sem fim de linha"
printf 'a\r\nb\r\n' > crlf.txt
run Read '{"file_path":"crlf.txt"}'
eq "Read: CRLF sem o CR" "     1${TAB}a
     2${TAB}b"
# arquivo longo: 5000 linhas, limite padrao de 2000
i=0; : > long.txt; seq 1 5000 | sed 's/^/linha /' > long.txt
# uma chamada so usa o cap do agente (12800 bytes): o trecho e cortado pelo tamanho
run Read '{"file_path":"long.txt","limit":2500}'
has "Read: cap pequeno corta pelo tamanho" "stopped by the size limit"
has "Read: cap pequeno diz o total" "of 5000 total"
[ ${#out} -le 12800 ] && ok "Read: resultado dentro do cap de 12800" || fail "Read: resultado maior que o cap"
# varias chamadas no mesmo processo usam um cap grande: aparecem os limites de linhas
run Read '{"file_path":"a.txt"}' Read '{"file_path":"long.txt","limit":2500}'
has "Read: limite de 2500 linhas respeitado" "  2500${TAB}linha 2500"
run Read '{"file_path":"a.txt"}' Read '{"file_path":"long.txt","offset":1}'
has "Read: padrao 2000 linhas" "  2000${TAB}linha 2000"
hasnt "Read: padrao nao passa de 2000" "linha 2001"
has "Read: PARTIAL view" "PARTIAL view: returned lines 1-2000 of 5000 total"
has "Read: PARTIAL diz como continuar" "offset=2001"
# arquivo grande (> 256 KB) sem offset/limit
seq 1 80000 > big.txt
run Read '{"file_path":"big.txt"}'
has "Read: arquivo grande inteiro recusado" "too large to read whole"
run Read '{"file_path":"big.txt","offset":79990}'
has "Read: arquivo grande com offset funciona" " 80000${TAB}80000"
# o cap: usa o limite de tamanho do resultado (cap pequeno via muitas linhas largas)
awk 'BEGIN{for(i=1;i<=3000;i++){s="";for(j=0;j<30;j++)s=s "xxxxxxxxxx";print s}}' > wide.txt
run Read '{"file_path":"a.txt"}' Read '{"file_path":"wide.txt","limit":3000}'
has "Read: limite de tamanho avisa PARTIAL" "stopped by the size limit"
[ ${#out} -le 262144 ] && ok "Read: resultado dentro do cap" || fail "Read: resultado maior que o cap"
awk 'BEGIN{s="";for(i=0;i<5000;i++)s=s "y";print s; print "fim"}' > longline.txt
run Read '{"file_path":"longline.txt"}'
has "Read: linha muito longa cortada" "[line truncated, 5000 bytes]"
has "Read: linha seguinte aparece" "     2${TAB}fim"
# mapa de arquivo grande (Read sem offset e sem limit)
awk 'BEGIN{print "#include <stdio.h>"; print "#define LIMITE 10"; for(i=1;i<=50;i++){printf "static int func_%d(int x) {\n    int y = x + %d;\n    return y;\n}\n\n", i, i}; print "struct ponto {"; print "    int x;"; print "};"}' > big.c
run Read '{"file_path":"big.c"}'
has "Read mapa: cabecalho com linhas e bytes" "lines, "
has "Read mapa: lista simbolos com a linha" "func_2(int x)"
has "Read mapa: #define principal" "#define LIMITE 10"
has "Read mapa: struct" "struct ponto"
has "Read mapa: primeiras linhas" "     1${TAB}#include <stdio.h>"
has "Read mapa: nota final" "this is its map and its start"
hasnt "Read mapa: nao traz o arquivo inteiro" "func_30(int x) {"
run Read '{"file_path":"big.c","offset":1,"limit":5}'
hasnt "Read mapa: com limit nada muda" "[map:"
run Read '{"file_path":"big.c","offset":6}'
hasnt "Read mapa: com offset nada muda" "this is its map"
export KATANA_READ_OUTLINE=0
run Read '{"file_path":"big.c"}'
unset KATANA_READ_OUTLINE
hasnt "Read mapa: KATANA_READ_OUTLINE=0 desliga" "[map:"
has "Read mapa: desligado mostra o arquivo" "func_30"
seq 1 250 | sed 's/^/## Titulo /' > big.md
run Read '{"file_path":"big.md"}'
has "Read mapa: markdown mostra titulos" "## Titulo 101"
printf 'def a():\n    pass\nclass B:\n    def m(self):\n        pass\n' > small.py
run Read '{"file_path":"small.py"}'
hasnt "Read mapa: arquivo pequeno nao muda" "[map:"
seq 1 400 > big2.txt
run Read '{"file_path":"big2.txt"}'
hasnt "Read mapa: extensao sem mapa nao muda" "this is its map"
# PDF
if command -v pdftotext >/dev/null 2>&1; then
  cat > mk.py <<'PY'
import sys
# PDF minimo de uma pagina com o texto HELLO
objs=[b"<</Type/Catalog/Pages 2 0 R>>",b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 100]/Contents 4 0 R/Resources<</Font<</F1 5 0 R>>>>>>",
b"<</Length 44>>\nstream\nBT /F1 18 Tf 10 50 Td (HELLOPDF) Tj ET\nendstream",
b"<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>"]
o=b"%PDF-1.4\n";off=[]
for i,x in enumerate(objs):
    off.append(len(o)); o+=("%d 0 obj\n"%(i+1)).encode()+x+b"\nendobj\n"
xr=len(o); o+=("xref\n0 %d\n0000000000 65535 f \n"%(len(objs)+1)).encode()
for f in off: o+=("%010d 00000 n \n"%f).encode()
o+=("trailer\n<</Size %d/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n"%(len(objs)+1,xr)).encode()
open(sys.argv[1],"wb").write(o)
PY
  python3 mk.py t.pdf
  run Read '{"file_path":"t.pdf"}'
  has "Read: PDF como texto" "HELLOPDF"
  run Read '{"file_path":"t.pdf","pages":"1"}'
  has "Read: PDF com pages" "HELLOPDF"
  run Read '{"file_path":"t.pdf","pages":"x"}'
  has "Read: pages invalido" "pages must look like"
  run Read '{"file_path":"t.pdf","pages":"1-30"}'
  has "Read: mais de 20 paginas" "at most 20 pages"
else
  echo "skip PDF (pdftotext ausente)"
fi
# caderno
cat > nb.ipynb <<'EOF'
{
 "cells": [
  {
   "cell_type": "markdown",
   "id": "aaa111",
   "metadata": {},
   "source": [
    "# Titulo\n",
    "texto"
   ]
  },
  {
   "cell_type": "code",
   "execution_count": 3,
   "id": "bbb222",
   "metadata": {"tags": ["x"]},
   "outputs": [
    {
     "name": "stdout",
     "output_type": "stream",
     "text": [
      "saida 42\n"
     ]
    }
   ],
   "source": [
    "print(42)"
   ]
  }
 ],
 "metadata": {},
 "nbformat": 4,
 "nbformat_minor": 5
}
EOF
cp nb.ipynb nb.orig
run Read '{"file_path":"nb.ipynb"}'
has "Read: caderno mostra id" 'id="bbb222" type="code"'
has "Read: caderno mostra fonte" "print(42)"
has "Read: caderno mostra saida" "saida 42"
has "Read: caderno mostra markdown" "# Titulo"

# ---------------- Write ----------------
run Write '{"file_path":"'$P'/new/sub/w.txt","content":"um\ndois\n"}'
has "Write: arquivo novo" "File created successfully"
fileis "Write: conteudo gravado e pastas criadas" new/sub/w.txt "um
dois"
run Write '{"file_path":"w2.txt","content":"relativo"}'
has "Write: caminho relativo" "File created successfully at: $P/w2.txt"
run Write '{"file_path":"a.txt","content":"X"}'
has "Write: sobrescrever sem ler e erro" "have not read"
fileis "Write: arquivo intacto depois do erro" a.txt "alpha
beta
gamma"
run Read '{"file_path":"a.txt","limit":1}' Write '{"file_path":"a.txt","content":"X"}'
has "Write: leitura parcial nao basta" "only read part of"
run Read '{"file_path":"a.txt"}' Write '{"file_path":"a.txt","content":"novo\n"}'
has "Write: depois de Read inteiro sobrescreve" "has been overwritten"
fileis "Write: sobrescreveu" a.txt "novo"
run Read '{"file_path":"a.txt"}' Write '{"file_path":"a.txt","content":"de novo"}' Write '{"file_path":"a.txt","content":"e outra"}'
fileis "Write: depois de Write nao precisa ler de novo" a.txt "e outra"
run Write '{"file_path":"empty-new.txt","content":""}'
has "Write: conteudo vazio" "0 bytes"
[ -f empty-new.txt ] && ok "Write: arquivo vazio criado" || fail "Write: arquivo vazio criado"
run Write '{"file_path":"w.txt"}'
has "Write: sem content" "error: missing content"
run Write '{"content":"x"}'
has "Write: sem file_path" "error: missing file_path"
run Write '{"file_path":"dir","content":"x"}'
has "Write: diretorio" "is a directory"
# conteudo grande (sem limite de 64 KB)
BIGC=$(awk 'BEGIN{for(i=0;i<4000;i++)printf "linha %d de conteudo grande\\n", i}')
run Write '{"file_path":"grande.txt","content":"'"$BIGC"'"}'
has "Write: conteudo de 120 KB" "File created"
SZ=$(wc -c < grande.txt)
[ "$SZ" -gt 100000 ] && ok "Write: tamanho gravado ($SZ bytes)" || fail "Write: tamanho gravado ($SZ)"
run Write '{"file_path":"u.txt","content":"ação 😀 \"q\"\tfim"}'
fileis "Write: escapes unicode" u.txt "$(printf 'a\303\247\303\243o \360\237\230\200 "q"\tfim')"
run Write '{"file_path":"s.py","content":"def f(:\n  pass\n"}'
has "Write: code_check acusa sintaxe" "syntax check FAILED"
run Write '{"file_path":"ok.py","content":"x = 1\n"}'
has "Write: code_check ok" "syntax check: ok"
printf '\377\376bin\n' > latin.txt
run Read '{"file_path":"latin.txt"}' Write '{"file_path":"latin.txt","content":"a�b"}'
has "Write: U+FFFD sobre arquivo nao UTF-8" "not valid UTF-8"
if [ "$(id -u)" != 0 ]; then
  mkdir -p ro; chmod 555 ro
  run Write '{"file_path":"ro/x.txt","content":"x"}'
  has "Write: permissao negada manda usar Bash com sudo" "Bash with sudo"
  chmod 755 ro
fi

# ---------------- Read por diferenca ----------------
python3 -c "
for i in range(1,101): print('linha numero %d com algum texto' % i)" > d.txt
export KATANA_READ_DELTA=1
run Read '{"file_path":"d.txt"}' Read '{"file_path":"d.txt"}'
has "Read delta: igual" "has not changed since you read it"
run Read '{"file_path":"d.txt"}' Read '{"file_path":"d.txt"}' Read '{"file_path":"d.txt"}'
if [ "$(printf '%s' "$out" | grep -c 'linha numero 50 ')" = 2 ]; then ok "Read delta: na terceira vez vai inteiro"; else fail "Read delta: terceira vez"; fi
run Read '{"file_path":"d.txt"}' Edit '{"file_path":"d.txt","old_string":"linha numero 50 com","new_string":"NOVA\nlinha 50 com"}' Read '{"file_path":"d.txt"}'
has "Read delta: so o trecho" "only the changed part is shown (lines 47-54 of 101)"
has "Read delta: deslocamento" "1 line further down"
has "Read delta: linha mudada com numero" "    50${TAB}NOVA"
if [ "$(printf '%s' "$out" | grep -c 'linha numero 10 ')" = 1 ]; then ok "Read delta: o resto nao se repete"; else fail "Read delta: resto"; fi
run Read '{"file_path":"d.txt"}' Bash '{"command":"python3 -c \"print(chr(10).join(str(i)*30 for i in range(100)))\" > d.txt"}' Read '{"file_path":"d.txt"}'
has "Read delta: mudou muito, vai inteiro" "   100${TAB}9999"
run Read '{"file_path":"d.txt","limit":5}' Read '{"file_path":"d.txt"}'
hasnt "Read delta: depois de leitura parcial vai inteiro" "has not changed"
run Read '{"file_path":"d.txt"}' Edit '{"file_path":"d.txt","old_string":"'$(printf '99%.0s' $(seq 30))'","new_string":""}' Read '{"file_path":"d.txt"}'
has "Read delta: mudanca na ultima linha" "only the changed part is shown"
unset KATANA_READ_DELTA
run Read '{"file_path":"d.txt"}' Read '{"file_path":"d.txt"}'
hasnt "Read delta: desligado fora do agente" "has not changed"

# ---------------- Edit com varias trocas ----------------
printf 'um\ndois\ntres\nquatro\ncinco\n' > m.txt
R='{"file_path":"m.txt"}'
run Read "$R" Edit '{"file_path":"m.txt","edits":[{"old_string":"dois","new_string":"2"},{"old_string":"cinco","new_string":"5\n6"},{"old_string":"um","new_string":"1\n1b"}]}'
has "edits: array aplica" "(3 edits, 3 replacements)"
has "edits: linha final da segunda troca (a terceira, acima, empurrou)" "edit 2, line 6:"
fileis "edits: arquivo" m.txt "1
1b
2
tres
quatro
5
6"
run Read "$R" Edit '{"file_path":"m.txt","edits":"<<<<<<<\n2\n=======\nDOIS \"x\" \\n\n>>>>>>>\n<<<<<<< old\nquatro\n=======\n>>>>>>> new\n"}'
has "edits: blocos de texto" "(2 edits, 2 replacements)"
fileis "edits: bloco cru, e bloco sem texto novo apaga a linha" m.txt '1
1b
DOIS "x" \n
tres
5
6'
# a lista de trocas dentro de um texto (chamada em XML: todo valor chega como texto) vale como a lista
printf 'alfa\nbeta\n' > mx.txt
run Read '{"file_path":"mx.txt"}' Edit '{"file_path":"mx.txt","edits":"[{\"old_string\":\"alfa\",\"new_string\":\"A\"},{\"old_string\":\"beta\",\"new_string\":\"B\"}]"}'
has "edits: lista dentro de texto" "(2 edits, 2 replacements)"
fileis "edits: lista dentro de texto, arquivo" mx.txt 'A
B'
cp m.txt m0.txt
run Read "$R" Edit '{"file_path":"m.txt","edits":"<<<<<<<\ntres\n=======\n3\n>>>>>>>\n<<<<<<<\nnaoha\n=======\nx\n>>>>>>>"}'
has "edits: uma falha diz qual" "edit 2 of 2: old_string was not found"
has "edits: uma falha nao muda nada" "Nothing was changed."
if cmp -s m.txt m0.txt; then ok "edits: tudo ou nada"; else fail "edits: tudo ou nada"; fi
run Read "$R" Edit '{"file_path":"m.txt","edits":[{"old_string":"1","new_string":"9"}]}'
has "edits: ambigua" "matches 2 places"
run Read "$R" Edit '{"file_path":"m.txt","edits":[{"old_string":"1","new_string":"9","replace_all":true},{"old_string":"9b","new_string":"nove"}]}'
has "edits: replace_all por item, e o seguinte ve o texto novo" "(2 edits, 3 replacements)"
run Read "$R" Edit '{"file_path":"m.txt","edits":"<<<<<<<\ntres\n=======\na\n=======\nb\n>>>>>>>"}'
has "edits: dois separadores" "two ======= lines"
run Read "$R" Edit '{"file_path":"m.txt","edits":"<<<<<<<\ntres\n=======\na"}'
has "edits: bloco aberto" "is not closed"
run Read "$R" Edit '{"file_path":"m.txt","edits":"troque tres por 3"}'
has "edits: texto sem bloco" "expected a line <<<<<<<"
run Read "$R" Edit '{"file_path":"m.txt","edits":[]}'
has "edits: vazio" "edits is empty"
run Read "$R" Edit '{"file_path":"m.txt","old_string":"tres","new_string":"3","edits":[{"old_string":"a","new_string":"b"}]}'
has "edits: nao junto com old_string" "not both"
run Edit '{"file_path":"m0.txt","edits":[{"old_string":"tres","new_string":"3"}]}'
has "edits: exige leitura" "not read"
run Read "$R" Edit '{"file_path":"m.txt"}'
has "Edit: sem nada diz o que falta" "missing old_string"
run Read "$R" Edit '{"file_path":"m.txt","edits":"[{\"old_string\": \"tres\", \"new_string\": \"3\"}]"}'
has "edits: array dentro de texto (chamada em XML)" "(1 edits, 1 replacement)"
printf 'a\r\nb\r\nc\r\n' > crlf.txt
run Read '{"file_path":"crlf.txt"}' Edit '{"file_path":"crlf.txt","edits":"<<<<<<<\nb\n=======\n>>>>>>>"}'
if [ "$(od -An -c crlf.txt | tr -d ' \n')" = 'a\r\nc\r\n' ]; then ok "edits: CRLF, apaga a linha inteira"; else fail "edits: CRLF"; fi

# ---------------- Edit ----------------
printf 'um\ndois\ntres\ndois\nquatro\n' > e.txt
run Edit '{"file_path":"e.txt","old_string":"um","new_string":"UM"}'
has "Edit: sem ler antes e erro" "not read"
run Read '{"file_path":"e.txt","limit":2}' Edit '{"file_path":"e.txt","old_string":"um","new_string":"UM"}'
has "Edit: leitura parcial e erro" "only read part of"
run Read '{"file_path":"e.txt"}' Edit '{"file_path":"e.txt","old_string":"um","new_string":"UM"}'
has "Edit: depois de Read" "has been updated (1 replacement, first at line 1)"
has "Edit: mostra contexto numerado" "     1${TAB}UM"
has "Edit: contexto inclui linhas vizinhas" "     3${TAB}tres"
fileis "Edit: arquivo mudou" e.txt "UM
dois
tres
dois
quatro"
run Read '{"file_path":"e.txt"}' Edit '{"file_path":"e.txt","old_string":"um","new_string":"UM"}' Edit '{"file_path":"e.txt","old_string":"tres","new_string":"TRES"}'
has "Edit: depois de Edit nao precisa ler de novo" "has been updated"
run Read '{"file_path":"e.txt"}' Edit '{"file_path":"e.txt","old_string":"dois","new_string":"2"}'
has "Edit: varias ocorrencias" "matches 2 places"
has "Edit: varias ocorrencias oferece replace_all" "replace_all"
fileis "Edit: nada gravado com ambiguidade" e.txt "UM
dois
TRES
dois
quatro"
run Read '{"file_path":"e.txt"}' Edit '{"file_path":"e.txt","old_string":"naoexiste","new_string":"x"}'
has "Edit: zero ocorrencias" "0 matches"
run Read '{"file_path":"e.txt"}' Edit '{"file_path":"e.txt","old_string":"dois","new_string":"2","replace_all":true}'
has "Edit: replace_all" "2 replacements, first at line 2"
fileis "Edit: replace_all trocou as duas" e.txt "UM
2
TRES
2
quatro"
run Edit '{"file_path":"e.txt","old_string":"UM","new_string":"UM"}'
has "Edit: iguais" "identical"
run Edit '{"file_path":"e.txt","old_string":"UM"}'
has "Edit: sem new_string" "missing new_string"
run Edit '{"file_path":"e.txt","new_string":"x"}'
has "Edit: sem old_string" "missing old_string"
run Edit '{"file_path":"naoha.txt","old_string":"a","new_string":"b"}'
has "Edit: arquivo inexistente" "does not exist"
run Edit '{"file_path":"dir","old_string":"a","new_string":"b"}'
has "Edit: diretorio" "is a directory"
run Edit '{"file_path":"novo-por-edit.txt","old_string":"","new_string":"criado\n"}'
has "Edit: old_string vazio cria o arquivo" "File created successfully"
fileis "Edit: conteudo do arquivo criado" novo-por-edit.txt "criado"
run Read '{"file_path":"e.txt"}' Edit '{"file_path":"e.txt","old_string":"","new_string":"x"}'
has "Edit: old_string vazio em arquivo com conteudo" "already has content"
# (estado 3, arquivo mudado no disco depois do Read, nao da para testar: o registro vive so dentro de um processo)
# UTF-8 invalido
printf 'caf\351 ola\n' > l1.txt
run Read '{"file_path":"l1.txt"}' Edit '{"file_path":"l1.txt","old_string":"ola","new_string":"oi"}'
has "Edit: nao UTF-8 recusado" "not a valid UTF-8"
# acentos preservados
printf 'ação aqui\n' > ac.txt
run Read '{"file_path":"ac.txt"}' Edit '{"file_path":"ac.txt","old_string":"aqui","new_string":"ali"}'
fileis "Edit: UTF-8 preservado" ac.txt "ação ali"
# code_check
printf 'x = 1\n' > c.py
run Read '{"file_path":"c.py"}' Edit '{"file_path":"c.py","old_string":"x = 1","new_string":"x = ("}'
has "Edit: code_check acusa erro" "syntax check FAILED"
# multilinha e contexto
printf 'l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\n' > m.txt
run Read '{"file_path":"m.txt"}' Edit '{"file_path":"m.txt","old_string":"l5","new_string":"A\nB"}'
has "Edit: numero da linha da mudanca" "first at line 5"
has "Edit: contexto com tres linhas antes" "     2${TAB}l2"
has "Edit: contexto mostra as linhas novas" "     6${TAB}B"
# caminho relativo e fora do alcance do ~
printf 'rel\n' > r.txt
run Read '{"file_path":"r.txt"}' Edit '{"file_path":"./r.txt","old_string":"rel","new_string":"REL"}'
fileis "Edit: caminho relativo com ./" r.txt "REL"
# o caminho com .. leva ao mesmo arquivo
run Read '{"file_path":"dir/../r.txt"}' Edit '{"file_path":"'$P'/r.txt","old_string":"REL","new_string":"rel2"}'
fileis "Edit: mesmo arquivo por caminhos diferentes" r.txt "rel2"

# ---------------- NotebookEdit ----------------
cp nb.orig nb.ipynb
run NotebookEdit '{"notebook_path":"nb.ipynb","cell_id":"bbb222","new_source":"print(\"oi\")\nx = 2"}'
has "NB: replace" "Replaced the source of cell bbb222 (code)"
has "NB: replace avisa que limpou saidas" "outputs and execution count were cleared"
run Read '{"file_path":"nb.ipynb"}'
has "NB: fonte nova lida" 'print("oi")'
hasnt "NB: saida antiga sumiu" "saida 42"
python3 - "$P/nb.ipynb" <<'PY' && ok "NB: JSON valido depois do replace" || fail "NB: JSON valido depois do replace"
import json,sys
d=json.load(open(sys.argv[1]))
c=d["cells"][1]
assert c["source"]==['print("oi")\n','x = 2'], c["source"]
assert c["outputs"]==[] and c["execution_count"] is None
assert d["cells"][0]["source"]==["# Titulo\n","texto"]
assert c["metadata"]=={"tags":["x"]}
PY
# a celula 0 fica byte a byte
head -12 nb.orig > a.part; head -12 nb.ipynb > b.part
cmp -s a.part b.part && ok "NB: celula nao tocada identica byte a byte" || fail "NB: celula nao tocada identica byte a byte"
tail -4 nb.orig > a.part; tail -4 nb.ipynb > b.part
cmp -s a.part b.part && ok "NB: final do arquivo identico" || fail "NB: final do arquivo identico"
run NotebookEdit '{"notebook_path":"nb.ipynb","cell_id":"aaa111","new_source":"novo texto","cell_type":"code"}'
has "NB: replace muda o tipo" "changed its type from markdown to code"
python3 - "$P/nb.ipynb" <<'PY' && ok "NB: tipo mudado para code com saidas vazias" || fail "NB: tipo mudado para code com saidas vazias"
import json,sys
c=json.load(open(sys.argv[1]))["cells"][0]
assert c["cell_type"]=="code" and c["id"]=="aaa111" and c["outputs"]==[] and c["source"]==["novo texto"], c
PY
run NotebookEdit '{"notebook_path":"nb.ipynb","cell_id":"aaa111","new_source":"x","edit_mode":"insert","cell_type":"markdown"}'
has "NB: insert depois da celula" "Inserted a new markdown cell"
has "NB: insert diz 3 celulas" "now has 3 cells"
python3 - "$P/nb.ipynb" <<'PY' && ok "NB: insert na posicao certa e com id" || fail "NB: insert na posicao certa e com id"
import json,sys
cs=json.load(open(sys.argv[1]))["cells"]
assert [c["cell_type"] for c in cs]==["code","markdown","code"], cs
assert len(cs[1]["id"])==8 and cs[1]["source"]==["x"] and "outputs" not in cs[1]
assert cs[2]["id"]=="bbb222"
PY
run NotebookEdit '{"notebook_path":"nb.ipynb","new_source":"print(0)","edit_mode":"insert","cell_type":"code"}'
has "NB: insert no inicio" "at the start"
python3 - "$P/nb.ipynb" <<'PY' && ok "NB: insert no inicio" || fail "NB: insert no inicio conteudo"
import json,sys
cs=json.load(open(sys.argv[1]))["cells"]
assert len(cs)==4 and cs[0]["source"]==["print(0)"] and cs[0]["cell_type"]=="code" and cs[0]["outputs"]==[] and cs[0]["execution_count"] is None, cs[0]
PY
run NotebookEdit '{"notebook_path":"nb.ipynb","new_source":"","edit_mode":"insert"}'
has "NB: insert sem cell_type" "cell_type (code or markdown) is required"
run NotebookEdit '{"notebook_path":"nb.ipynb","cell_id":"bbb222","edit_mode":"delete"}'
has "NB: delete" "Deleted cell bbb222"
has "NB: delete conta celulas" "now has 3 cells"
python3 - "$P/nb.ipynb" <<'PY' && ok "NB: delete removeu a celula certa" || fail "NB: delete removeu a celula certa"
import json,sys
cs=json.load(open(sys.argv[1]))["cells"]
assert [c["id"] for c in cs if "id" in c][-1] != "bbb222" and len(cs)==3
PY
run NotebookEdit '{"notebook_path":"nb.ipynb","cell_id":"zzz","new_source":"x"}'
has "NB: id inexistente lista os ids" "no cell with id \"zzz\""
run NotebookEdit '{"notebook_path":"nb.ipynb","new_source":"x"}'
has "NB: replace sem cell_id" "cell_id is required"
run NotebookEdit '{"notebook_path":"nb.ipynb","cell_id":"aaa111","edit_mode":"replace"}'
has "NB: replace sem new_source" "missing new_source"
run NotebookEdit '{"notebook_path":"nb.ipynb","cell_id":"aaa111","new_source":"x","edit_mode":"mover"}'
has "NB: edit_mode invalido" "edit_mode must be"
run NotebookEdit '{"notebook_path":"a.txt","cell_id":"x","new_source":"x"}'
has "NB: nao e .ipynb" "not a notebook"
run NotebookEdit '{"notebook_path":"nada.ipynb","cell_id":"x","new_source":"x"}'
has "NB: arquivo inexistente" "does not exist"
echo 'isto nao e json' > bad.ipynb
run NotebookEdit '{"notebook_path":"bad.ipynb","cell_id":"x","new_source":"x"}'
has "NB: JSON invalido" "not a valid notebook"
# sem ids (nbformat 4.2): cell-N
cat > old.ipynb <<'EOF'
{"cells":[{"cell_type":"code","execution_count":1,"metadata":{},"outputs":[],"source":["a = 1"]},{"cell_type":"markdown","metadata":{},"source":["texto"]}],"metadata":{},"nbformat":4,"nbformat_minor":2}
EOF
run Read '{"file_path":"old.ipynb"}'
has "NB: Read mostra cell-0" 'id="cell-0"'
run NotebookEdit '{"notebook_path":"old.ipynb","cell_id":"cell-1","new_source":"outro"}'
has "NB: cell-N como id" "Replaced the source of cell cell-1"
run NotebookEdit '{"notebook_path":"old.ipynb","cell_id":"cell-0","new_source":"b = 2","edit_mode":"insert","cell_type":"code"}'
has "NB: insert sem ids no arquivo" "Inserted a new code cell"
python3 - "$P/old.ipynb" <<'PY' && ok "NB: arquivo compacto continua valido e sem id novo" || fail "NB: arquivo compacto continua valido e sem id novo"
import json,sys
d=json.load(open(sys.argv[1])); cs=d["cells"]
assert len(cs)==3 and "id" not in cs[1] and cs[1]["source"]==["b = 2"] and cs[2]["source"]==["outro"], cs
PY
run NotebookEdit '{"notebook_path":"old.ipynb","cell_id":"cell-0","edit_mode":"delete"}'
run NotebookEdit '{"notebook_path":"old.ipynb","cell_id":"cell-0","edit_mode":"delete"}'
run NotebookEdit '{"notebook_path":"old.ipynb","cell_id":"cell-0","edit_mode":"delete"}'
has "NB: apagar a ultima celula" "now has 0 cells"
run NotebookEdit '{"notebook_path":"old.ipynb","new_source":"so uma","edit_mode":"insert","cell_type":"markdown"}'
has "NB: insert em caderno sem celulas" "now has 1 cell"
python3 - "$P/old.ipynb" <<'PY' && ok "NB: caderno de uma celula valido" || fail "NB: caderno de uma celula valido"
import json,sys
cs=json.load(open(sys.argv[1]))["cells"]
assert len(cs)==1 and cs[0]["source"]==["so uma"]
PY

HOME=$HOME_SAVE
if [ "$BAD" -eq 0 ]; then echo "OK $N checks"; exit 0; else echo "FAILED $BAD (de $N checks)"; exit 1; fi
