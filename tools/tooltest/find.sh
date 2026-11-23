#!/bin/sh
# Testa o Find sem modelo: sh tools/tooltest/find.sh /caminho/do/katana
# Pasta temporaria propria (criada e apagada); no fim, a cadeia do caso real do greymod se a copia antiga existir
# (GREY_BEFORE, padrao /tmp/katana-grey-before). Uma linha por checagem; FAILED n ou OK n checks no fim.
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
export KATANA_LOG=${KATANA_LOG:-$(mktemp)}
[ -x "$K" ] || { echo "uso: $0 <binario katana>"; exit 2; }
T=$(mktemp -d) || exit 2
HOME=$T/home; export HOME; mkdir -p "$HOME"
unset KATANA_CACHE_DIR KATANA_FIND_CHARS
trap 'rm -rf "$T"' EXIT
N=0; BAD=0
out=
run() { out=$("$K" tool "$@" 2>/dev/null); }
ok()   { N=$((N+1)); echo "ok   $1"; }
fail() { N=$((N+1)); BAD=$((BAD+1)); echo "FAIL $1"; echo "     got: $(printf '%s' "$out" | head -c 700 | tr '\n' '|')"; }
has()  { case "$out" in *"$2"*) ok "$1";; *) fail "$1 (missing: $2)";; esac; }
hasnt(){ case "$out" in *"$2"*) fail "$1 (unexpected: $2)";; *) ok "$1";; esac; }
first(){ f=$(printf '%s\n' "$out" | sed -n 's/^1\. \([^:]*\):.*/\1/p'); if [ "$f" = "$2" ]; then ok "$1"; else fail "$1 (first: $f, want: $2)"; fi; }

# ---------------- projeto pequeno (sem git: caminhada) ----------------
mkdir -p "$T/p/src" "$T/p/docs" "$T/p/node_modules/lib" "$T/p/build" "$T/p/empty"
cd "$T/p" || exit 2
cat > src/sessions.js <<'EOF'
// lista as sessoes abertas no computador
export function listSessions(limit = 12) {
  const all = readSessionFiles();
  return all.slice(0, limit);
}
function readSessionFiles() {
  return [];
}
EOF
cat > src/server.js <<'EOF'
import { listSessions } from "./sessions.js";
function terminalSessionIds(list) {
  return new Set(list.map((s) => s.id));
}
export function onMessage(m) {
  const list = listSessions(m.limit);
  const live = terminalSessionIds(list);
  return { list, live };
}
EOF
cat > src/config.py <<'EOF'
import json
DEFAULT_PORT = 7879
def parse_config(path):
    """le o arquivo de configuracao da ponte"""
    with open(path) as f:
        return json.load(f)
class ConfigError(Exception):
    pass
EOF
cat > src/engine.c <<'EOF'
#include <stdio.h>
#define MAX_TOKENS 4096
typedef struct { int n; } Batch;
static int decode_batch(Batch *b) {
    return b->n;
}
int main(void) { Batch b = {0}; return decode_batch(&b); }
EOF
printf '# Bridge\n\n## Sessions on the phone\nThe phone shows the list of sessions from listSessions.\n' > docs/readme.md
printf 'export function listSessions() {}\n' > node_modules/lib/x.js
printf 'export function listSessions() {}\n' > build/out.js
printf 'abc\000def listSessions\n' > src/blob.js
awk 'BEGIN { for (i = 0; i < 9000; i++) print "function qqhugeword" i "() { return 1; }" }' > src/huge.js

run Find '{"query":"list sessions open on the computer"}'
first "search: o arquivo que define o que a frase descreve vem primeiro" "src/sessions.js"
has "search: faixa de linhas" "src/sessions.js:1-8"
has "search: simbolos definidos no trecho" "listSessions"
has "search: linha do trecho com numero (a linha casada leva >)" "2> export function listSessions"
has "search: dica de proximo passo" "Follow a symbol with Find mode=refs, or Read a part with offset and limit."
hasnt "search: node_modules fica de fora" "node_modules/lib"
hasnt "search: build fica de fora" "build/out.js"
hasnt "search: binario fica de fora" "blob.js"
ls .katana/cache/find-*.idx >/dev/null 2>&1 && fail "o Find nao grava indice proprio (find-*.idx)" || ok "o Find nao grava indice proprio (find-*.idx)"
run Find '{"query":"qqhugeword17"}'
has "arquivo acima de 300 KB nao entra" "No match"
once(){ n=$(printf '%s\n' "$out" | grep -c "ignored folder"); [ "$n" -eq 1 ] && ok "$1" || fail "$1 (frase das pastas ignoradas $n vezes)"; }
for qq in '{"query":"zzqxnotthere"}' '{"query":"\"zzq not there\""}' '{"query":"zzqx_not_there"}' '{"query":"where is zzq handled in the foo flow"}'; do
  run Find "$qq"; once "NOT FOUND: a frase das pastas ignoradas sai exatamente uma vez ($qq)"
done
run Find '{"query":"where is the config file parsed"}'
first "search: python" "src/config.py"
has "search: def do python e simbolo" "parse_config"
run Find '{"query":"decode batch"}'
first "search: C" "src/engine.c"
has "search: funcao static do C" "decode_batch"
run Find '{"query":"server message handler"}'
has "search: calls mostra o simbolo usado definido em outro arquivo" "calls: "
has "search: calls aponta a definicao (caminho relativo ao arquivo)" "listSessions (sessions.js:2-5)"
run Find '{"query":"read session files"}'
has "search: also referenced in" "also referenced in: src/server.js"

# ---------------- refs ----------------
run Find '{"query":"listSessions","mode":"refs"}'
has "refs: definicao" "src/sessions.js:2"
has "refs: arquivo que usa" "src/server.js (1):"
has "refs: contagem de usos" "Used 1 times in 1 files"
has "refs: funcao em volta do uso" "6 (in onMessage):"
hasnt "refs: o import nao conta como uso" "import { listSessions }"
hasnt "refs: node_modules fica de fora" "node_modules/lib"
run Find '{"query":"terminalSessionIds","mode":"refs"}'
has "refs: definicao no mesmo arquivo" "src/server.js:2"
has "refs: uso dentro de onMessage" "7 (in onMessage)"
run Find '{"query":"nothingLikeThis","mode":"refs"}'
has "refs: simbolo desconhecido" "No definition of nothingLikeThis"
run Find '{"query":"  ","mode":"refs"}'
has "refs: sem nome" "error:"

# ---------------- argumentos e limites ----------------
run Find '{}'
has "sem query: erro com exemplo" "error: missing \`query\`"
run Find '{"query":"x","mode":"grep"}'
has "modo invalido: erro" "error: unknown mode"
run Find '{"query":"listSessions","limit":1}'
has "limit 1: um candidato" "1. "
hasnt "limit 1: so um" "
2. "
i=0; while [ $i -lt 16 ]; do printf 'const kiwiFruit%d = "kiwi kiwi, a long line so that twelve candidates do not fit in the default answer size of the tool";\n' $i > src/k$i.js; i=$((i+1)); done
out=$(KATANA_FIND_CHARS=100000 "$K" tool Find '{"query":"kiwi","limit":50}' 2>/dev/null)
has "limit acima de 12 vira 12" "
12. "
hasnt "limit: nunca 13" "
13. "
run Find '{"query":"kiwi","limit":12}'
len=$(printf '%s' "$out" | wc -c)
[ "$len" -le 3100 ] && ok "saida padrao curta ($len caracteres)" || fail "saida padrao curta ($len caracteres)"
out=$(KATANA_FIND_CHARS=500 "$K" tool Find '{"query":"kiwi","limit":12}' 2>/dev/null)
has "saida cortada avisa" "more candidates"
run Find '{"query":"sessions","path":"docs"}'
first "path: so a subpasta" "docs/readme.md"
hasnt "path: nada fora dela" "src/"
run Find '{"query":"listSessions","path":"src/server.js"}'
first "path: um arquivo" "src/server.js"
run Find '{"query":"sessions","path":"nao-existe"}'
has "path inexistente: erro" "error:"

# ---------------- incremental ----------------
run Find '{"query":"zebracorn"}'
has "antes: palavra nova nao existe" "No match"
sleep 1
printf 'export function zebracornHandler() { return 42; }\n' > src/later.js
out=$("$K" tool Find '{"query":"zebracorn"}' 2>/dev/null)
first "arquivo novo entra no indice" "src/later.js"
printf 'export function giraffeHandler() { return 42; }\n' > src/later.js
run Find '{"query":"zebracorn"}'
has "arquivo alterado: termo antigo some" "No match"
run Find '{"query":"giraffe"}'
first "arquivo alterado: termo novo aparece" "src/later.js"
rm src/later.js
run Find '{"query":"giraffe"}'
has "arquivo apagado sai do indice" "No match"
err=$(KATANA_FIND_TIME=1 "$K" tool Find '{"query":"sessions"}' 2>&1 >/dev/null)
case "$err" in *"find: query"*) ok "KATANA_FIND_TIME escreve os tempos";; *) out=$err; fail "KATANA_FIND_TIME escreve os tempos";; esac

# ---------------- pasta vazia ----------------
mkdir -p "$T/vazia"; cd "$T/vazia" || exit 2
run Find '{"query":"anything"}'
has "pasta sem arquivos: nao ha indice do coder para ela" "the code index of"

# ---------------- tabela ----------------
run ToolSearch '{"query":"select:Find"}'
has "ToolSearch conhece o Find" "\"query\""

# ---------------- caso real: a cadeia do greymod antes da correcao ----------------
G=${GREY_BEFORE:-/tmp/katana-grey-before}
if [ -f "$G/bridge/sessions.mjs" ]; then
  cd "$G" || exit 2
  export KATANA_CACHE_DIR=$T/greycache
  # 1) a pergunta do usuario: o gkatana usa o mesmo canal do Grey que o outro comando de terminal
  run Find '{"query":"gkatana celular Grey"}'
  has "grey 1: gkatana leva ao install.sh (mesmo canal do outro comando)" "linux/install.sh"
  # 2) sessoes de terminal mostradas no celular
  run Find '{"query":"sessoes de terminal listadas no celular","path":"bridge"}'
  has "grey 2: server.mjs aparece" "bridge/server.mjs"
  has "grey 2: terminalSessionIds visivel" "terminalSessionIds"
  has "grey 2: channels.mjs visivel" "channels.mjs"
  # 3) quem usa terminalSessionIds: onMessage, junto com listSessions
  run Find '{"query":"terminalSessionIds","mode":"refs"}'
  has "grey 3: definicao em server.mjs" "bridge/server.mjs:114"
  has "grey 3: uso em onMessage" "(in onMessage)"
  # 4) de onde vem a lista: listSessions em sessions.mjs
  run Find '{"query":"listSessions","mode":"refs"}'
  has "grey 4: listSessions definida em sessions.mjs" "bridge/sessions.mjs:132"
  has "grey 4: usada no server.mjs" "bridge/server.mjs ("
  # 5) o canal: quem guarda os terminais ligados
  run Find '{"query":"createChannels","mode":"refs"}'
  has "grey 5: channels.mjs define o canal" "bridge/channels.mjs:"
  unset KATANA_CACHE_DIR
else
  echo "skip caso real do greymod: $G nao existe (git -C greymod worktree add --detach <pasta> 03ee8cf; GREY_BEFORE=<pasta>)"
fi

echo
if [ "$BAD" -gt 0 ]; then echo "FAILED $BAD of $N checks"; exit 1; fi
echo "OK $N checks"
