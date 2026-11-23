#!/bin/sh
# Gera src/app/web.inc: a pagina do katana web (web/, os .js de desktop-qt/qml, fontes e icones do desktop) em base64,
# para o servidor (src/app/webui.c) entregar sem ler nada do disco. Chamado por build.sh. Uso: tools/genweb.sh [saida]
cd "$(dirname "$0")/.." || exit 1
OUT=${1:-src/app/web.inc}
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
mime() {
  case "$1" in
    *.html) echo "text/html; charset=utf-8";; *.css) echo "text/css; charset=utf-8";; *.js) echo "text/javascript; charset=utf-8";;
    *.png) echo "image/png";; *.ttf) echo "font/ttf";; *.svg) echo "image/svg+xml";; *) echo "application/octet-stream";;
  esac
}
# um arquivo: nome publicado, caminho de origem
add() {
  [ -f "$2" ] || { echo "genweb: falta $2" >&2; exit 1; }
  printf '{"%s","%s",%s,\n' "$1" "$(mime "$1")" "$(wc -c < "$2" | tr -d ' ')"
  base64 -w 120 "$2" | sed 's/.*/"&"/'
  echo '},'
}
# .js do desktop (QML "pragma library"): sem a diretiva, dentro de uma funcao que devolve as funcoes usadas pela pagina
wrap() {  # nome publicado, origem, variavel, exportadas
  { echo "var $3=(function(){"; grep -v '^\.pragma library' "$2"; echo "return {$4};"; echo "})();"; } > "$T/$1"
  add "js/lib/$1" "$T/$1"
}
{
  echo "/* gerado por tools/genweb.sh (nao editar): a pagina do katana web, em base64 */"
  echo "static const WebFile WEB_FILES[] = {"
  add index.html web/index.html
  for f in web/css/*.css; do add "css/$(basename "$f")" "$f"; done
  for f in web/js/*.js; do add "js/$(basename "$f")" "$f"; done
  wrap Markdown.js desktop-qt/qml/Markdown.js Md "parse:parse"
  wrap Highlight.js desktop-qt/qml/Highlight.js Hl "html:html"
  wrap Verbs.js desktop-qt/qml/Verbs.js V "verbo:verbo,alvo:alvo,titulo:titulo,resumo:resumo,dur:dur,realcavel:realcavel,realce:realce"
  for f in desktop-qt/fonts/*.ttf; do add "fonts/$(basename "$f")" "$f"; done
  for n in katana-64 katana-k titulo-claro titulo-escuro; do add "img/$n.png" "desktop-qt/icons/$n.png"; done
  echo "{0,0,0,0}};"
} > "$T/web.inc" || exit 1
mv "$T/web.inc" "$OUT"
