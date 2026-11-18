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

echo "== run_command: stderr, stdin, timeout, truncagem"
t "run: stderr visivel"         'boom'                  run_command '{"command":"echo boom >&2"}'
t "run: codigo de saida"        'exit code 3'           run_command '{"command":"echo out; exit 3"}'
t "run: stdout e stderr juntos" 'ERRO'                  run_command '{"command":"echo OK; echo ERRO >&2"}'
t "run: stdin fechado"          'got:\[\]'              run_command '{"command":"read x; echo got:[$x]"}'
t "run: pede senha, sem travar" 'in their own terminal: echo' run_command '{"command":"echo \"sudo: a password is required\" >&2; exit 1"}'
if command -v sudo >/dev/null 2>&1; then
  S0=$(date +%s)
  t "run: sudo -n nao trava"    'exit code'             run_command '{"command":"sudo -n true"}'
  [ $(( $(date +%s) - S0 )) -lt 20 ] && { OK=$((OK+1)); echo "ok   run: sudo -n voltou rapido"; } || { FAIL=$((FAIL+1)); echo "FALHA run: sudo demorou"; }
fi
S0=$(date +%s)
t "run: timeout avisa"          'timeout apos 2 s'      run_command '{"command":"echo antes; sleep 30; echo depois","timeout_s":2}'
[ $(( $(date +%s) - S0 )) -lt 15 ] && { OK=$((OK+1)); echo "ok   run: timeout matou rapido"; } || { FAIL=$((FAIL+1)); echo "FALHA run: timeout demorou"; }
t "run: timeout mantem a saida parcial" 'antes'         run_command '{"command":"echo antes; sleep 30","timeout_s":1}'
t "run: timeout em string"      'timeout apos 1 s'      run_command '{"command":"sleep 30","timeout_s":"1"}'
t "run: segundo plano nao prende" 'exit code 0'         run_command '{"command":"(sleep 3 &) ; echo oi","timeout_s":30}'
out=$("$KAT" tool run_command '{"command":"seq 1 5000"}' 2>/dev/null)
printf '%s' "$out" | grep -q '^1$' && printf '%s' "$out" | grep -q '^5000$' && printf '%s' "$out" | grep -q 'bytes omitted' && { OK=$((OK+1)); echo "ok   run: truncagem mantem inicio e fim"; } || { FAIL=$((FAIL+1)); echo "FALHA run: truncagem"; }
GRP="$T/grp.pid"
"$KAT" tool run_command "{\"command\":\"sleep 60 & echo \$! > $GRP; wait\",\"timeout_s\":1}" >/dev/null 2>&1
sleep 0.5
if kill -0 "$(cat "$GRP")" 2>/dev/null; then FAIL=$((FAIL+1)); echo "FALHA run: filho sobrou apos timeout"; kill -9 "$(cat "$GRP")"; else OK=$((OK+1)); echo "ok   run: grupo de processos morto no timeout"; fi

echo "== sudo com senha pedida ao usuario (sudo falso)"
mkdir -p fakebin; cat > fakebin/sudo <<'EOS'
#!/bin/sh
case "$1" in -n) exit 1;; esac
A=0; for x; do [ "$x" = -A ] && A=1; done
[ $A = 1 ] || { echo "sudo: a terminal is required to read the password"; exit 1; }
P=$("$SUDO_ASKPASS"); [ "$P" = secret ] || { echo "sudo: incorrect password"; exit 1; }
while [ "$1" = -A ] || [ "$1" = -k ] || [ "$1" = -v ]; do shift; done
[ $# -gt 0 ] && exec "$@"; exit 0
EOS
chmod +x fakebin/sudo
SU=$(printf 'run_command {"command":"sudo echo ola-root"}\n\001PASS secret\nrun_command {"command":"sudo echo de-novo"}\n' | PATH="$PWD/fakebin:$PATH" KATANA_UI=1 "$KAT" toolrun 2>/dev/null)
if echo "$SU" | grep -q "ola-root" && echo "$SU" | grep -q "de-novo" && [ "$(echo "$SU" | grep -c NEEDPASS)" = 1 ]; then OK=$((OK+1)); echo "ok   sudo: pede a senha uma vez e reaproveita"; else FAIL=$((FAIL+1)); echo "FALHA sudo: $SU"; fi

SD=$(printf 'run_command {"command":"echo \\"it'"'"'s-root\\"; echo dois"}\n\001PASS secret\n' | PATH="$PWD/fakebin:$PATH" KATANA_UI=1 KATANA_SUDO_FORCE=1 "$KAT" toolrun 2>/dev/null)
if echo "$SD" | grep -q "it's-root" && echo "$SD" | grep -q "dois" && echo "$SD" | grep -q NEEDPASS; then OK=$((OK+1)); echo "ok   sudo por padrao: comando inteiro (com aspas) roda via sudo -A sh -c"; else FAIL=$((FAIL+1)); echo "FALHA sudo padrao: $SD"; fi

echo "== terminal, read_log, terminal_screen (uma sessao do katana com varias chamadas)"
printf 'a\nERRO: falhou aqui\nb\nWARN algo\nc\n' > app.log
TR=$("$KAT" toolrun 2>/dev/null <<'EOT'
terminal {"action":"list"}
terminal {"action":"open","name":"t1"}
terminal {"action":"send","text":"cd /tmp"}
terminal {"action":"send","text":"MINHA=valor42"}
terminal {"action":"send","text":"pwd; echo var=$MINHA"}
terminal {"action":"send","text":"echo erro-visto >&2"}
terminal {"action":"send","text":"read -p 'senha? ' x","enter":true}
terminal {"action":"read","lines":3,"wait_ms":200}
terminal {"action":"interrupt"}
terminal {"action":"send","text":"echo parcial","enter":false,"wait_ms":300}
terminal {"action":"read","wait_ms":200}
terminal {"action":"open","name":"t2"}
terminal {"action":"list"}
terminal {"action":"send","name":"t2","text":"echo so-no-t2; pwd"}
terminal {"action":"send"}
terminal {"action":"close","name":"t2"}
terminal {"action":"close","name":"t1"}
terminal {"action":"send","text":"x"}
read_log {"source":"commands","lines":50}
read_log {"source":"commands","filter":"so-no-t2"}
EOT
)
chk() { printf '%s' "$TR" | grep -q -- "$2" && { OK=$((OK+1)); echo "ok   $1"; } || { FAIL=$((FAIL+1)); echo "FALHA $1 (esperava /$2/)"; printf '%s\n' "$TR" | head -80 | sed 's/^/     | /'; }; }
chk "terminal: list vazio"             'no sessions'
chk "terminal: open"                   'session t1 opened'
chk "terminal: cd persiste"            '^/tmp$'
chk "terminal: variavel persiste"      'var=valor42'
chk "terminal: stderr visivel"         'erro-visto'
chk "terminal: prompt interativo"      'senha?'
chk "terminal: texto sem Enter"        'parcial'
chk "terminal: duas sessoes"           't2: pid'
chk "terminal: sessao isolada"         'so-no-t2'
chk "terminal: erro sem nome ambiguo"  'several sessions'
chk "terminal: close"                  'session t2 closed'
chk "terminal: sem sessao"             'no terminal session is open'
chk "read_log commands: lista"         '\[terminal\]'
chk "read_log commands: filtro"        'so-no-t2'
t "read_log: arquivo com filtro"       'ERRO: falhou aqui'     read_log '{"source":"app.log","filter":"erro"}'
t "read_log: filtro regex"             'WARN algo'             read_log '{"source":"app.log","filter":"^(WARN|ERRO)"}'
out=$("$KAT" tool read_log '{"source":"app.log","filter":"erro"}' 2>/dev/null)
printf '%s' "$out" | grep -q '^b$' && { FAIL=$((FAIL+1)); echo "FALHA read_log: filtro nao filtrou"; } || { OK=$((OK+1)); echo "ok   read_log: filtro exclui o resto"; }
t "read_log: ultimas linhas"           '^c$'                   read_log '{"source":"app.log","lines":2}'
t "read_log: inexistente"              'cannot open'        read_log '{"source":"nao-existe.log"}'
t "read_log: binario recusado"         'binary'                read_log "{\"source\":\"$KAT\"}"
t "read_log: sem source"               'missing source'        read_log '{}'
t "read_log: agent"                    'run_command'           read_log '{"source":"agent","lines":500}'
t "read_log: unit invalida"            'invalid unit'          read_log '{"source":"journal","unit":"a;b"}'
t "read_log: sem comandos no processo" 'no commands run yet'   read_log '{"source":"commands"}'
: > noperm.log; chmod 000 noperm.log
[ "$(id -u)" != 0 ] && t "read_log: sem permissao" 'permission denied' read_log '{"source":"noperm.log"}'
if command -v journalctl >/dev/null 2>&1; then
  out=$("$KAT" tool read_log '{"source":"journal","lines":3}' 2>/dev/null)
  case "$out" in *journal*|*"permission denied"*) OK=$((OK+1)); echo "ok   read_log: journal (resposta valida)";; *) FAIL=$((FAIL+1)); echo "FALHA read_log: journal: $out";; esac
fi
out=$("$KAT" tool read_log '{"source":"dmesg","lines":3}' 2>/dev/null)
case "$out" in *dmesg*|*"permission denied"*|*"not available"*) OK=$((OK+1)); echo "ok   read_log: dmesg (resposta valida ou dica de permissao)";; *) FAIL=$((FAIL+1)); echo "FALHA read_log: dmesg: $out";; esac
if [ -r /var/log/syslog ] || [ -r /var/log/messages ]; then t "read_log: syslog" 'lines' read_log '{"source":"syslog","lines":3}'
else out=$("$KAT" tool read_log '{"source":"syslog"}' 2>/dev/null); case "$out" in *error*) OK=$((OK+1)); echo "ok   read_log: syslog ilegivel da erro claro";; *) FAIL=$((FAIL+1)); echo "FALHA syslog: $out";; esac; fi

if command -v tmux >/dev/null 2>&1; then
  tmux -L katana_t new-session -d -s kt -x 100 -y 30 "echo ERRO-NA-TELA-DO-USUARIO; sleep 30"
  sleep 0.5
  TMUX= TMUX_PANE= t "terminal_screen: tmux real, pane dado" 'ERRO-NA-TELA' terminal_screen '{"pane":"kt"}'
  tmux -L katana_t kill-server 2>/dev/null
else echo "pula  terminal_screen com tmux real (tmux nao instalado)"; fi
# tmux falso no PATH: confere o comando montado e a escolha do outro painel
mkdir -p fake
cat > fake/tmux <<'EOT'
#!/bin/sh
echo "$@" >> "$TMUX_ARGS"
case "$1" in
  list-panes) printf '%%1\n%%2\n' ;;
  capture-pane) echo "TELA-FALSA" ;;
esac
EOT
chmod +x fake/tmux
export TMUX_ARGS="$T/tmux.args"
PATH="$PWD/fake:$PATH" TMUX=/tmp/x,1,0 TMUX_PANE=%1 t "terminal_screen: tmux falso" 'TELA-FALSA' terminal_screen '{"lines":20}'
grep -q 'capture-pane -p -t %2 -S -20' "$TMUX_ARGS" && { OK=$((OK+1)); echo "ok   terminal_screen: capture-pane -p -t outro-painel -S -N"; } || { FAIL=$((FAIL+1)); echo "FALHA terminal_screen: args: $(cat "$TMUX_ARGS")"; }
PATH="$PWD/fake:$PATH" TMUX= t "terminal_screen: fora do tmux" 'not running inside tmux' terminal_screen '{}'
out=$(PATH=/nonexistent-dir TMUX= "$KAT" tool terminal_screen '{}' 2>/dev/null)
case "$out" in *"tmux is not installed"*) OK=$((OK+1)); echo "ok   terminal_screen: sem tmux";; *) FAIL=$((FAIL+1)); echo "FALHA terminal_screen: sem tmux: $out";; esac
t "terminal_screen: pane invalido" 'invalid pane' terminal_screen '{"pane":"a;b"}'

echo "== log"
[ -s "$KATANA_LOG" ] && { OK=$((OK+1)); echo "ok   agente.log gravado"; } || { FAIL=$((FAIL+1)); echo "FALHA agente.log vazio"; }

echo; echo "passaram: $OK  falharam: $FAIL"
cd / && rm -rf "$T"
[ "$FAIL" = 0 ]
