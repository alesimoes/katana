#!/bin/sh
# Testa WebFetch e WebSearch (src/tools/webfetch.c e websearch.c). Uso: sh tools/tooltest/web.sh ./katana
# Tudo o que nao precisa de rede e verificado sempre; as verificacoes com rede so rodam se uma sonda curl funcionar.
K=${1:-./katana}
export KATANA_LOG=${KATANA_LOG:-$(mktemp)}     # o log das chamadas destes testes nao vai para o ~/.katana/agente.log do dono
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
FAIL=0; N=0
ok()   { N=$((N+1)); echo "ok   $1"; }
bad()  { N=$((N+1)); FAIL=$((FAIL+1)); echo "FAIL $1"; echo "     got: $(printf %s "$2" | head -c 400)"; }
# eq nome esperado obtido: igualdade exata
eq()   { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "$3"; fi; }
# has nome trecho obtido: contem o trecho (texto fixo)
has()  { if printf %s "$3" | grep -qF -- "$2"; then ok "$1"; else bad "$1" "$3"; fi; }
hasnt() { if printf %s "$3" | grep -qF -- "$2"; then bad "$1" "$3"; else ok "$1"; fi; }

# ---------- WebFetch sem rede ----------
r=$("$K" tool WebFetch '{"prompt":"x"}' 2>&1)
has "fetch: url ausente e erro" "error: missing url" "$r"
r=$("$K" tool WebFetch '{"url":"ftp://example.com/a","prompt":"x"}' 2>&1)
has "fetch: esquema invalido" "error: invalid URL" "$r"
r=$("$K" tool WebFetch '{"url":"example.com","prompt":"x"}' 2>&1)
has "fetch: sem esquema" "must start with http" "$r"
r=$("$K" tool WebFetch '{"url":"http://localhost:8080/x","prompt":"x"}' 2>&1)
has "fetch: localhost recusado" "error: WebFetch refuses local or internal hosts" "$r"
has "fetch: localhost manda usar curl no Bash" "use curl through Bash" "$r"
r=$("$K" tool WebFetch '{"url":"https://localhost/","prompt":"x"}' 2>&1)
has "fetch: https localhost recusado" "refuses local" "$r"
r=$("$K" tool WebFetch '{"url":"http://intranet/page","prompt":"x"}' 2>&1)
has "fetch: host sem ponto recusado" "refuses local" "$r"
r=$("$K" tool WebFetch '{"url":"http://127.0.0.1:9/","prompt":"x"}' 2>&1)
has "fetch: 127.0.0.1 recusado" "refuses local" "$r"
r=$("$K" tool WebFetch '{"url":"http://user:pw@localhost/","prompt":"x"}' 2>&1)
has "fetch: usuario@localhost recusado" "refuses local" "$r"
r=$("$K" tool WebFetch '{"url":"http://app.localhost/","prompt":"x"}' 2>&1)
has "fetch: subdominio .localhost recusado" "refuses local" "$r"
r=$(KATANA_WEBFETCH_DRY=1 "$K" tool WebFetch '{"url":"http://Example.com/a?b=1","prompt":"x"}' 2>&1)
eq "fetch: http vira https no cabecalho" "WebFetch https://Example.com/a?b=1 (upgraded from http to https) (dry run)" "$r"
r=$(KATANA_WEBFETCH_DRY=1 "$K" tool WebFetch '{"url":"https://example.com/a","prompt":"x"}' 2>&1)
eq "fetch: https fica como esta" "WebFetch https://example.com/a (dry run)" "$r"
r=$(KATANA_WEBFETCH_DRY=1 "$K" tool WebFetch '{"url":"https://example.com/a"}' 2>&1)
eq "fetch: prompt ausente e tolerado" "WebFetch https://example.com/a (dry run)" "$r"
r=$(KATANA_WEBFETCH_DRY=1 "$K" tool WebFetch '{"url":"  https://example.com/b  ","prompt":"x"}' 2>&1)
eq "fetch: espacos nas pontas da URL" "WebFetch https://example.com/b (dry run)" "$r"
r=$(KATANA_WEB=0 "$K" tool WebFetch '{"url":"https://example.com/","prompt":"x"}' 2>&1)
eq "fetch: internet desligada" "error: internet tools are disabled (KATANA_WEB=0)" "$r"
r=$(KATANA_WEB=0 "$K" tool WebSearch '{"query":"x"}' 2>&1)
eq "search: internet desligada" "error: internet tools are disabled (KATANA_WEB=0)" "$r"
r=$("$K" tool WebFetch '{"url":"http://localhost/","prompt":"x"}' WebFetch '{"url":"http://intranet/","prompt":"x"}' 2>&1)
has "fetch: varias chamadas em sequencia" "=== WebFetch" "$r"

# ---------- WebSearch sem rede (resultados fixos) ----------
FIX='1. Rust book
   https://doc.rust-lang.org/book/
   The Rust Programming Language
   (sources: bing)
2. Rust on Wikipedia
   https://en.wikipedia.org/wiki/Rust_(programming_language)
   Rust is a language.
3. Blog post about Rust
   http://www.example.com/blog/rust
   A post.
4. Notexample
   https://notexample.com/rust
   Not the same site.
5. Sub of wikipedia
   https://pt.wikipedia.org/wiki/Rust
   Em portugues.'
S() { KATANA_WEBSEARCH_FIXTURE="$FIX" "$K" tool WebSearch "$1" 2>&1; }
r=$(S '{"query":"rust"}')
has "search: sem filtro mantem tudo (5)" "5. Sub of wikipedia" "$r"
has "search: mantem os resumos" "The Rust Programming Language" "$r"
r=$(S '{"query":"rust","allowed_domains":["wikipedia.org"]}')
has "search: allowed inclui subdominio en." "1. Rust on Wikipedia" "$r"
has "search: allowed inclui subdominio pt. renumerado" "2. Sub of wikipedia" "$r"
hasnt "search: allowed tira os outros" "Rust book" "$r"
has "search: allowed diz quantos ficaram" "[2 of 5 results kept by allowed_domains]" "$r"
r=$(S '{"query":"rust","blocked_domains":["wikipedia.org","example.com"]}')
has "search: blocked mantem o livro" "1. Rust book" "$r"
has "search: blocked mantem notexample (nao e subdominio)" "2. Notexample" "$r"
hasnt "search: blocked tira www.example.com" "Blog post" "$r"
hasnt "search: blocked tira wikipedia" "Wikipedia" "$r"
has "search: blocked diz quantos ficaram" "[2 of 5 results kept by blocked_domains]" "$r"
r=$(S '{"query":"rust","allowed_domains":["https://WWW.Example.com/x"]}')
has "search: dominio normalizado (esquema, caixa, caminho)" "1. Blog post about Rust" "$r"
r=$(S '{"query":"rust","allowed_domains":["nada.org"]}')
eq "search: filtro tira tudo" "no results left after filtering: all 5 results were removed by allowed_domains. Search again without the filter or with other domains" "$r"
r=$(S '{"query":"rust","allowed_domains":["a.org"],"blocked_domains":["b.org"]}')
has "search: os dois e erro" "error: give allowed_domains or blocked_domains, not both" "$r"
r=$(S '{"query":"rust","allowed_domains":[],"blocked_domains":["b.org"]}')
has "search: array vazio nao conta" "5. Sub of wikipedia" "$r"
r=$(S '{"query":"rust","allowed_domains":"doc.rust-lang.org"}')
has "search: texto unico no lugar do array" "1. Rust book" "$r"
hasnt "search: texto unico filtra" "Wikipedia" "$r"
r=$(S '{"allowed_domains":["a.org"]}')
has "search: query ausente" "error: missing query" "$r"
r=$(S '{"query":""}')
has "search: query vazia" "error: missing query" "$r"
r=$(KATANA_WEBSEARCH_FIXTURE="" "$K" tool WebSearch '{"query":"zzz"}' 2>&1)
has "search: busca sem resultados" "no results for \"zzz\"" "$r"

# ---------- leitura em paralelo, varias consultas, orcamento, cache, WebFetch curto (servidor local, sem internet) ----------
export KATANA_CACHE_DIR="$T/cache"            # o cache dos testes nao vai para o ~/.katana do dono
HERE=$(dirname "$0")
if command -v python3 >/dev/null 2>&1; then
  python3 -I "$HERE/websrv.py" "$T/port" & SRV=$!
  trap 'kill $SRV 2>/dev/null; rm -rf "$T"' EXIT
  i=0; while [ ! -s "$T/port" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
  P=$(cat "$T/port" 2>/dev/null)
  B="http://127.0.0.1:$P"
  export KATANA_WEB_ALLOW_LOCAL=1
  export KATANA_EMBED=0                          # so a nota por palavras: com o servico de embeddings o trecho escolhido depende do modelo instalado
  export KATANA_WEB_OFFLINE_ROUTER=1             # o passo "outra fonte" da escada nao consulta a internet de verdade (Wikipedia etc.) para um 404 local
  nowms() { python3 -c 'import time; print(int(time.time()*1000))'; }
  entry() { printf '%s. %s\n   %s\n   %s\n   (sources: bing)\n' "$1" "$2" "$3" "$4"; }
  FIXL="$(entry 1 'Rust ownership guide' "$B/p1" 'About ownership')
$(entry 2 'Slow page' "$B/slow" 'It never answers in time')
$(entry 3 'Lifetimes' "$B/p2" 'About lifetimes')
$(entry 4 'Broken page' "$B/fail" 'Drops the connection')
$(entry 5 'Tomatoes' "$B/p3" 'Gardening')"
  W() { KATANA_WEBSEARCH_FIXTURE="$FIXL" "$K" tool WebSearch "$1" 2>&1; }
  t0=$(nowms); r=$(W '{"query":"rust borrow checker ownership","read":4}'); t1=$(nowms)
  ms=$((t1-t0))
  if [ $ms -lt 10500 ]; then ok "search read: slow page does not hold the result past the cap (${ms} ms)"; else bad "search read: took ${ms} ms" "$r"; fi
  has "search read: list comes first" "1. Rust ownership guide" "$r"
  has "search read: passages of the first page with its URL" "[1] $B/p1" "$r"
  has "search read: passage text matches the query" "borrow checker" "$r"
  has "search read: second good page read" "[3] $B/p2" "$r"
  hasnt "search read: irrelevant page gets no space" "[5] $B/p3" "$r"
  hasnt "search read: slow page has no passages" "[2] $B/slow" "$r"
  hasnt "search read: failing page has no passages" "[4] $B/fail" "$r"
  hasnt "search read: filler text is left out" "tomato plants" "$r"
  has "search read: footer reports failures" "2 failed or slow" "$r"
  has "search read: footer reports bytes" "bytes returned" "$r"
  last=$(printf %s "$r" | tail -n 1)
  case "$last" in "[web: search"*) ok "search read: last line is the timing line" ;; *) bad "search read: last line" "$last" ;; esac
  body=$(printf %s "$r" | sed '$d' | wc -c)
  if [ "$body" -le 4000 ]; then ok "search read: body within the 4000 byte budget ($body)"; else bad "search read: body is $body bytes" "$r"; fi
  # cache: segunda chamada sem as paginas lentas
  FIXC="$(entry 1 'Rust ownership guide' "$B/p1" 'About ownership')
$(entry 2 'Lifetimes' "$B/p2" 'About lifetimes')"
  rm -rf "$T/cache"
  r1=$(KATANA_WEBSEARCH_FIXTURE="$FIXC" "$K" tool WebSearch '{"query":"rust borrow checker"}' 2>&1)
  has "search read: default reads 3 pages (here 2 exist)" "[1] $B/p1" "$r1"
  has "search read: first call went to the network" "2 pages read" "$r1"
  hasnt "search read: first call is not cached" "some cached" "$r1"
  c1=$(curl -s "$B/count")
  r2=$(KATANA_WEBSEARCH_FIXTURE="$FIXC" "$K" tool WebSearch '{"query":"rust borrow checker"}' 2>&1)
  c2=$(curl -s "$B/count")
  has "cache: second call serves pages from the cache" "some cached" "$r2"
  eq "cache: the server saw no new request" "$c1" "$c2"
  r3=$("$K" tool WebFetch "{\"url\":\"$B/p1\",\"prompt\":\"borrow checker\"}" 2>&1)
  c3=$(curl -s "$B/count")
  has "cache: WebFetch of a page read by the search comes from the cache" "[web: from cache" "$r3"
  eq "cache: WebFetch made no request" "$c1" "$c3"
  rm -rf "$T/cache"
  r4=$(KATANA_WEB_CACHE=0 KATANA_WEBSEARCH_FIXTURE="$FIXC" "$K" tool WebSearch '{"query":"rust borrow checker"}' 2>&1)
  r5=$(KATANA_WEB_CACHE=0 "$K" tool WebFetch "{\"url\":\"$B/p1\",\"prompt\":\"borrow checker\"}" 2>&1)
  has "cache off: WebFetch goes to the network" "[web: network" "$r5"
  if [ -d "$T/cache" ] && [ -n "$(ls "$T/cache/web" 2>/dev/null)" ]; then bad "cache off: files were written" "$(ls "$T/cache/web")"; else ok "cache off: nothing written"; fi
  # varias consultas: duplicatas saem, a URL repetida sobe
  FIXQ="$(entry 1 'Tomatoes' "$B/p3" 'Gardening')
$(entry 2 'Rust ownership guide' "$B/p1" 'About ownership')"
  r=$(KATANA_WEBSEARCH_FIXTURE="$FIXQ" "$K" tool WebSearch '{"queries":["rust ownership","borrow checker rust","tomato"],"read":0}' 2>&1)
  n=$(printf %s "$r" | grep -cE '^[0-9]+\. ')
  eq "queries: duplicate URLs removed (2 unique of 6)" "2" "$n"
  has "queries: timing line present" "[web: search" "$r"
  r=$(KATANA_WEBSEARCH_FIXTURE="$FIXQ" "$K" tool WebSearch '{"queries":"[\"a b c d\"]","read":0}' 2>&1)
  has "queries: array sent as text is accepted" "1. " "$r"
  r=$(KATANA_WEBSEARCH_FIXTURE="$FIXQ" "$K" tool WebSearch '{"query":"rust ownership","read":0}' 2>&1)
  has "search: query with read 0 is the old list" "1. Tomatoes" "$r"
  # WebFetch curto por padrao
  r=$("$K" tool WebFetch "{\"url\":\"$B/long\",\"prompt\":\"optimizer inlining\"}" 2>&1)
  n=$(printf %s "$r" | wc -c)
  if [ "$n" -le 4400 ]; then ok "fetch default: short result for a 60 KB page ($n bytes)"; else bad "fetch default: $n bytes" "$r"; fi
  has "fetch default: passage that matches the prompt" "inlining pass" "$r"
  has "fetch default: says how long the page is and how to read more" "offset=<n>" "$r"
  has "fetch default: timing line" "[web: " "$r"
  r=$("$K" tool WebFetch "{\"url\":\"$B/long\",\"prompt\":\"zzzz qqqq\"}" 2>&1)
  has "fetch default: without a match, the start of the page within the budget" "Chapter 0 of the long page" "$r"
  n=$(printf %s "$r" | wc -c)
  if [ "$n" -le 4400 ]; then ok "fetch default: start of page within budget ($n bytes)"; else bad "fetch default: $n bytes" "$r"; fi
  r=$(KATANA_WEBSEARCH_FIXTURE="$FIXQ" "$K" tool WebSearch '{"query":"parser tokens tree","read":0}' WebFetch "{\"url\":\"$B/long\"}" 2>&1)
  has "fetch default: no prompt uses the words of the last search" "the words of the page title and of your last search" "$r"
  r=$("$K" tool WebFetch "{\"url\":\"$B/long\",\"prompt\":\"x\",\"offset\":20000}" 2>&1)
  has "fetch offset: exact content continues" "[page text bytes 20000-" "$r"
  n=$(printf %s "$r" | wc -c)
  if [ "$n" -gt 8000 ]; then ok "fetch offset: still returns the large exact slice ($n bytes)"; else bad "fetch offset: only $n bytes" "$r"; fi
  # sem segunda leitura: o resultado web enxuto termina com a linha [web: e o tool_digest a reconhece
  if grep -q 'strncmp(ln + 1, "\[web:", 5)' "$HERE/../../src/agent/delegate.c"; then ok "digest: tool_digest skips results that end in a [web: line"; else bad "digest: check missing in delegate.c" ""; fi
  # limpeza do cache: acima de 50 MB os mais antigos saem
  rm -rf "$T/cache"; mkdir -p "$T/cache/web"
  i=0; while [ $i -lt 56 ]; do dd if=/dev/zero of="$T/cache/web/p-old$i" bs=1048576 count=1 2>/dev/null; touch -d "2020-01-01 00:00:$((i%60))" "$T/cache/web/p-old$i"; i=$((i+1)); done
  "$K" tool WebFetch "{\"url\":\"$B/p1\",\"prompt\":\"borrow\"}" >/dev/null 2>&1
  sz=$(du -sk "$T/cache/web" | cut -f1)
  if [ "$sz" -le 51200 ] && ls "$T/cache/web" | grep -q '^p-[0-9a-f]'; then ok "cache: trimmed to the 50 MB cap, new entry kept (${sz} KB)"; else bad "cache: ${sz} KB" "$(ls "$T/cache/web" | head -3)"; fi
  # resposta HTTP de erro vira erro da ferramenta, com ou sem corpo; o texto da pagina de erro nao entra
  r=$("$K" tool WebFetch "{\"url\":\"$B/nopage\",\"prompt\":\"x\"}" 2>&1)
  has "fetch: 404 sem corpo e erro" "error: HTTP 404 (Not Found) from $B/nopage: there is no page at this address" "$r"
  r=$("$K" tool WebFetch "{\"url\":\"$B/gone\",\"prompt\":\"borrow checker\"}" 2>&1)
  case "$r" in "[web: error, via plain, 0 of 0 bytes of text, quality 0]"*) ok "fetch: 404 com corpo comeca com a linha de estado error";; *) bad "fetch: 404 com corpo comeca com a linha de estado error" "$r";; esac
  has "fetch: 404 com corpo traz o erro HTTP" "error: HTTP 404 (Not Found) from $B/gone" "$r"
  hasnt "fetch: o texto da pagina de erro nao entra" "friendly page" "$r"
  has "fetch: 404 manda procurar o endereco certo" "Another source: WebSearch with query \"gone\" and read a different site." "$r"
  has "fetch: 404 nao repete o endereco" "Do not fetch this address again" "$r"
  r=$("$K" tool WebFetch "{\"url\":\"$B/boom\",\"prompt\":\"x\"}" 2>&1)
  has "fetch: 500 e erro" "error: HTTP 500 (Internal Server Error)" "$r"
  r=$("$K" tool WebFetch "{\"url\":\"$B/gone\",\"prompt\":\"x\"}" 2>&1)
  has "fetch: o erro nao fica guardado como pagina" "error: HTTP 404" "$r"
  r=$("$K" tool WebFetch "{\"url\":\"$B/p2\",\"prompt\":\"lifetimes\"}" 2>&1)
  has "fetch: pagina boa continua vindo" "Lifetimes annotate" "$r"
  hasnt "fetch: a marca do codigo HTTP nao aparece no texto" "KATANA-HTTP-STATUS" "$r"
  # ---------- UIOE U9: o conector web.fetch (katana uioe ask), sem rede: servidor local ----------
  # o endereco digitado no pedido e lido antes do modelo mesmo com KATANA_UIOE_WEB=0; nada alem dessa URL sai; a pagina entra com URL e hora
  export KATANA_UIOE_LOG="$T/uioe.log"; mkdir -p "$T/proj"; rm -f "$T/uioe.log"
  UQ="what does $B/p1 say about the borrow checker"
  r=$(KATANA_UIOE_WEB=0 "$K" uioe ask "$UQ" --cwd "$T/proj" --explain --no-trace 2>&1)
  has "uioe web: URL do pedido e lida com a internet do UIOE desligada" "web.fetch" "$r"
  has "uioe web: a chamada rodou" "page         web.fetch" "$r"
  has "uioe web: sent mostra so a URL" "sent: page address $B/p1" "$r"
  has "uioe web: sai so o endereco digitado" "the address typed in the request to web.fetch: $B/p1" "$r"
  hasnt "uioe web: sem busca (os termos nao saem)" "search terms" "$r"
  has "uioe web: a busca fica fora da politica" "web search (policy: KATANA_UIOE_WEB=0)" "$r"
  has "uioe web: pagina e um no de fato" "web.fetch" "$r"
  r=$(KATANA_UIOE_WEB=0 "$K" uioe ask "$UQ" --cwd "$T/proj" --pack --no-trace 2>&1)
  has "uioe web: bloco traz a pagina com URL" "web page $B/p1 (read 20" "$r"
  has "uioe web: bloco rotula como nao verificado" "outside content" "$r"
  has "uioe web: bloco traz o trecho da pagina" "borrow checker" "$r"
  has "uioe web: Next manda responder da pagina" "Answer from the page above" "$r"
  r=$(KATANA_UIOE_WEB=0 "$K" uioe ask "$UQ" --cwd "$T/proj" --json --no-trace 2>&1)
  has "uioe web: json traz sent" "\"sent\":\"page address $B/p1\"" "$r"
  KATANA_UIOE_WEB=0 "$K" uioe ask "$UQ" --cwd "$T/proj" >/dev/null 2>&1
  has "uioe web: traco tem a linha sent:" "sent:   web.fetch | page address $B/p1" "$(cat "$T/uioe.log")"
  r=$(KATANA_WEB=0 "$K" uioe ask "$UQ" --cwd "$T/proj" --explain --no-trace 2>&1)
  hasnt "uioe web: KATANA_WEB=0 nao le a pagina" "page address" "$r"
  r=$(KATANA_UIOE_WEB=0 "$K" uioe ask "$B/gone" --cwd "$T/proj" --explain --no-trace 2>&1)
  has "uioe web: pagina que falha vira limite, nao erro" "the page was not read" "$r"
  r=$(KATANA_UIOE_WEB=0 "$K" uioe ask "search the web for latest rust release notes" --cwd "$T/proj" --pack --no-trace 2>&1)
  hasnt "uioe web: sem URL e sem a chave nada sai" "page address" "$r"
  r=$(KATANA_UIOE_WEB=0 "$K" uioe ask "search the web for latest rust release notes" --cwd "$T/proj" --explain --no-trace 2>&1)
  has "uioe web: sem a chave o Next propoe a busca" "Search the web for \"" "$r"
  has "uioe web: sem a chave nada sai da maquina" "sends out: nothing" "$r"
  r=$(KATANA_UIOE_WEB=0 "$K" uioe ask "search the web for latest rust release notes" --cwd "$T/proj" --explain --no-trace 2>&1 | grep -c "sent:")
  eq "uioe web: sem a chave nenhuma chamada tem sent" "0" "$r"
  unset KATANA_WEB_ALLOW_LOCAL
  kill $SRV 2>/dev/null
else
  echo "SKIPPED servidor local (sem python3)"
fi

# ---------- com rede ----------
if curl -sS -m 6 -o /dev/null https://example.com/ 2>/dev/null; then
  r=$("$K" tool WebFetch '{"url":"http://example.com/","prompt":"what is this page about"}' 2>&1)
  has "rede fetch: cabecalho com URL final e upgrade" "WebFetch https://example.com/ (upgraded from http to https): returned all" "$r"
  has "rede fetch: texto da pagina" "Example Domain" "$r"
  r=$("$K" tool WebFetch '{"url":"https://example.com/","prompt":"x","find":"domain"}' 2>&1)
  has "rede fetch: find (extra)" "passage" "$r"
  r=$("$K" tool WebFetch '{"url":"https://example.com/","prompt":"x","links":true}' 2>&1)
  has "rede fetch: links (extra)" "Links on https://example.com/" "$r"
  r=$("$K" tool WebFetch '{"url":"https://nao-existe-katana.invalid/","prompt":"x"}' 2>&1)
  has "rede fetch: host inexistente e erro" "error:" "$r"
  r=$("$K" tool WebSearch '{"query":"rust programming language","allowed_domains":["wikipedia.org"]}' 2>&1)
  case "$r" in
    error:*) bad "rede search: busca" "$r" ;;
    *)
      if printf %s "$r" | grep -E '^   https?://' | grep -qv 'wikipedia.org'; then bad "rede search: so wikipedia" "$r"; else ok "rede search: so wikipedia"; fi ;;
  esac
  # pagina longa: a selecao tem de respeitar o limite (cap do katana tool e grande; usa a Wikipedia)
  r=$("$K" tool WebFetch '{"url":"https://en.wikipedia.org/wiki/Rust_(programming_language)","prompt":"borrow checker ownership"}' 2>&1)
  has "rede fetch: pagina longa diz o que ficou sem ler" "bytes were not returned" "$r"
  has "rede fetch: pagina longa escolhe pelo prompt" "the passages that mention" "$r"
else
  echo "SKIPPED rede (sem conexao): fetch e busca de verdade"
fi

if [ "$FAIL" -eq 0 ]; then echo "OK $N checks"; else echo "FAILED $FAIL"; exit 1; fi
