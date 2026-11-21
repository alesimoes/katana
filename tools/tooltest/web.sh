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
  has "rede fetch: pagina longa escolhe pelo prompt" "the passages that mention the words of the prompt" "$r"
else
  echo "SKIPPED rede (sem conexao): fetch e busca de verdade"
fi

if [ "$FAIL" -eq 0 ]; then echo "OK $N checks"; else echo "FAILED $FAIL"; exit 1; fi
