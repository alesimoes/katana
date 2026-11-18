#!/bin/sh
# Testa as ferramentas do agente sem modelo e sem GPU, usando `katana tool <nome> <json>`.
# Uso: tools/testar_ferramentas.sh [caminho-do-katana]   (precisa de git; pdftotext e rede sao opcionais)
# Roda numa pasta temporaria com HOME isolado; sai com codigo != 0 se algum teste falhar.
KAT=$(realpath "${1:-$(dirname "$0")/../katana}")
[ -x "$KAT" ] || { echo "katana nao encontrado: $KAT"; exit 2; }
T=$(mktemp -d)
export HOME="$T/home" KATANA_LOG="$T/agente.log" KATANA_MEM="$T/mem.txt"
mkdir -p "$HOME" "$T/w"
cd "$T/w" || exit 1
OK=0; FAIL=0
# t <descricao> <padrao esperado na saida> <ferramenta> <json>
t() {
  out=$("$KAT" tool "$3" "$4" 2>/dev/null)
  if printf '%s' "$out" | grep -q -- "$2"; then OK=$((OK+1)); echo "ok   $1"
  else FAIL=$((FAIL+1)); echo "FALHA $1 (esperava /$2/)"; printf '%s\n' "$out" | head -5 | sed 's/^/     | /'; fi
}

printf 'linha um\nfoo = 1\nlinha tres\nfoo = 1\n' > a.txt
mkdir -p src/sub; printf 'int x;\n' > src/a.c; printf 'int y;\n' > src/sub/b.c; printf 'x' > src/sub/c.h; printf 'z' > top.c

echo "== edit_file"
t "edit: nao unico"            'appears 2 times'       edit_file '{"path":"a.txt","old":"foo = 1","new":"foo = 2"}'
t "edit: nao existe"           'not found'             edit_file '{"path":"a.txt","old":"inexistente","new":"x"}'
t "edit: unico + diff"         'first at line 3'       edit_file '{"path":"a.txt","old":"linha tres","new":"LINHA 3"}'
t "edit: diff mostra - e +"    '+ linha tres'             edit_file '{"path":"a.txt","old":"LINHA 3","new":"linha tres"}'
t "edit: replace_all"          '2 replacements'        edit_file '{"path":"a.txt","old":"foo = 1","new":"foo = 9","replace_all":true}'
t "edit: replace_all string"   '2 replacements'        edit_file '{"path":"a.txt","old":"foo = 9","new":"foo = 7","replace_all":"true"}'
t "edit: caminho fora"         'invalid path'          edit_file '{"path":"../x","old":"a","new":"b"}'
t "edit: old vazio"            'missing or empty old'  edit_file '{"path":"a.txt","old":"","new":"b"}'
t "edit: multilinha"           'ok: edited'            edit_file '{"path":"a.txt","old":"linha um\nfoo = 7","new":"UM\nbar"}'
grep -q '^UM$' a.txt && grep -q '^bar$' a.txt && { OK=$((OK+1)); echo "ok   edit: arquivo gravado"; } || { FAIL=$((FAIL+1)); echo "FALHA edit: conteudo"; }

echo "== glob"
t "glob: **/*.c"               'src/sub/b.c'           glob '{"pattern":"**/*.c"}'
t "glob: *.c basename"         'src/a.c'               glob '{"pattern":"*.c"}'
t "glob: src/*.c nao cruza"    'src/a.c'               glob '{"pattern":"src/*.c"}'
[ "$("$KAT" tool glob '{"pattern":"src/*.c"}' 2>/dev/null | grep -c 'b.c')" = 0 ] && { OK=$((OK+1)); echo "ok   glob: * nao cruza /"; } || { FAIL=$((FAIL+1)); echo "FALHA glob: * cruzou /"; }
t "glob: root"                 'src/sub/c.h'           glob '{"pattern":"**/*.h","root":"src"}'
t "glob: sem resultado"        'no files match'        glob '{"pattern":"*.zzz"}'
t "glob: ?"                    'top.c'                 glob '{"pattern":"to?.c"}'

echo "== git"
t "git: init"                  'exit code 0'                   run_command '{"command":"git init -q . && git config user.email t@t && git config user.name t"}'
t "git: status"                'a.txt'                 git '{"args":"status --short"}'
t "git: add"                   'ok (no output)'        git '{"args":"add a.txt top.c"}'
t "git: commit"                'primeiro'              git '{"args":"commit -m \"primeiro commit\""}'
t "git: log"                   'primeiro commit'       git '{"args":"log --oneline"}'
t "git: branch"                'ok'                    git '{"args":"branch nova"}'
t "git: diff"                  'ok'                    git '{"args":"diff"}'
t "git: push --force recusado" 'refused'               git '{"args":"push --force origin main"}'
t "git: push -f recusado"      'KATANA_GIT_UNSAFE'     git '{"args":"push -f"}'
t "git: reset --hard recusado" 'reset --hard'          git '{"args":"reset --hard HEAD"}'
t "git: subcomando proibido"   'not allowed'           git '{"args":"config --global x y"}'
t "git: injecao com ;"         'error'                 git '{"args":"status; touch pwned"}'
[ ! -e pwned ] && { OK=$((OK+1)); echo "ok   git: sem injecao de shell"; } || { FAIL=$((FAIL+1)); echo "FALHA git: injecao"; }
t "git: upload-pack recusado"  'not allowed'           git '{"args":"clone --upload-pack=touch x y"}'
KATANA_GIT_UNSAFE=1 t "git: reset com UNSAFE" 'HEAD is now' git '{"args":"reset --hard HEAD"}'

echo "== read_document"
printf '<html><head><style>p{color:red}</style><script>var x=1;</script></head><body><h1>Titulo</h1><p>Ola &amp; mundo</p></body></html>' > d.html
t "html: texto limpo"          'Ola & mundo'           read_document '{"path":"d.html"}'
out=$("$KAT" tool read_document '{"path":"d.html"}' 2>/dev/null)
case "$out" in *"var x"*|*"color:red"*) FAIL=$((FAIL+1)); echo "FALHA html: script/estilo vazou";; *) OK=$((OK+1)); echo "ok   html: sem script/estilo";; esac
t "texto simples"              'UM'                    read_document '{"path":"a.txt"}'
printf '\0\1\2\3' > bin.dat
t "binario recusado"           'binary'                read_document '{"path":"bin.dat"}'
if command -v pdftotext >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
  python3 - <<'PY'
b=b"%PDF-1.1\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 100]/Contents 4 0 R/Resources<</Font<</F1 5 0 R>>>>>>endobj\n"
s=b"BT /F1 18 Tf 10 50 Td (Ola PDF katana) Tj ET"
b+=b"4 0 obj<</Length %d>>stream\n"%len(s)+s+b"\nendstream endobj\n5 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\ntrailer<</Root 1 0 R>>\n%%EOF\n"
open("d.pdf","wb").write(b)
PY
  t "pdf via pdftotext"        'Ola PDF katana'        read_document '{"path":"d.pdf"}'
else echo "pula  pdf (sem pdftotext/python3)"; fi

echo "== install_package"
t "install: gerenciador ruim"  'must be one of'        install_package '{"manager":"brew","name":"x"}'
t "install: nome com ;"        'invalid character'     install_package '{"manager":"pip","name":"x; rm -rf /"}'
t "install: nome com -"        'invalid package name'  install_package '{"manager":"pip","name":"--index-url=http://x"}'

echo "== download_file"
printf 'conteudo do teste\n' > srv.txt
EXPECT=$(sha256sum srv.txt | cut -d' ' -f1)
if command -v python3 >/dev/null 2>&1 && command -v curl >/dev/null 2>&1; then
  python3 -m http.server 18765 --bind 127.0.0.1 >/dev/null 2>&1 &
  SP=$!; sleep 1
  t "download: ok + sha256"    "$EXPECT"               download_file '{"url":"http://127.0.0.1:18765/srv.txt","path":"baixado.txt"}'
  t "download: tamanho"        '18 bytes'              download_file '{"url":"http://127.0.0.1:18765/srv.txt","path":"baixado2.txt"}'
  head -c 3000000 /dev/zero > big.bin
  KATANA_DL_MAX_MB=1 t "download: limite" 'error: download failed' download_file '{"url":"http://127.0.0.1:18765/big.bin","path":"big2.bin"}'
  [ ! -e big2.bin ] && [ ! -e big2.bin.part ] && { OK=$((OK+1)); echo "ok   download: sem sobra do arquivo grande"; } || { FAIL=$((FAIL+1)); echo "FALHA download: sobrou arquivo"; }
  t "download: 404"            'download failed'       download_file '{"url":"http://127.0.0.1:18765/nao.txt","path":"n.txt"}'
  kill $SP 2>/dev/null
else echo "pula  download (sem python3/curl)"; fi
t "download: caminho fora"     'invalid path'          download_file '{"url":"http://x/y","path":"../z"}'
t "download: url ruim"         'invalid URL'           download_file '{"url":"file:///etc/passwd","path":"z"}'

echo "== log"
[ -s "$KATANA_LOG" ] && { OK=$((OK+1)); echo "ok   agente.log gravado"; } || { FAIL=$((FAIL+1)); echo "FALHA agente.log vazio"; }

echo; echo "passaram: $OK  falharam: $FAIL"
cd / && rm -rf "$T"
[ "$FAIL" = 0 ]
