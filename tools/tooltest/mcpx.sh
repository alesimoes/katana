#!/bin/sh
# Testes sem modelo e sem rede do que o cliente MCP tem alem do basico: ${VAR} na configuracao, roots, avisos de lista,
# anotacoes, .mcp.json, elicitation, SSE, katana mcp serve, OAuth. Os servidores falsos sao em C (tools/tooltest/mcpfake.c,
# compilado aqui) e escutam so em 127.0.0.1. Uso: sh tools/tooltest/mcpx.sh /caminho/do/katana
K=$1
[ -x "$K" ] || { echo "uso: $0 <binario katana>"; exit 2; }
K=$(cd "$(dirname "$K")" && pwd)/$(basename "$K")
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
export KATANA_LOG="$T/agente.log" KATANA_MCP_LOG="$T/mcp.log"
export HOME="$T/home"; mkdir -p "$HOME"
unset KATANA_MCP_CONFIG KATANA_MCP_TRUST KATANA_MCP_PREFIX KATANA_MCP_TIMEOUT KATANA_UI KATANA_CONFIRM KATANA_MCP_STRICT
N=0; F=0
ok()   { N=$((N+1)); echo "ok   $1"; }
bad()  { N=$((N+1)); F=$((F+1)); echo "FAIL $1"; echo "---- saida:"; printf '%s\n' "$2" | head -25; echo "----"; }
has()  { case $2 in *"$3"*) ok "$1";; *) bad "$1 (faltou: $3)" "$2";; esac; }
hasnt(){ case $2 in *"$3"*) bad "$1 (nao devia ter: $3)" "$2";; *) ok "$1";; esac; }
eq()   { [ "$2" = "$3" ] && ok "$1" || bad "$1 (esperado: $3)" "$2"; }
CC=${CC:-cc}
FAKE="$T/mcpfake"
$CC -O1 -w -o "$FAKE" "$HERE/mcpfake.c" || { echo "nao compilou mcpfake.c"; exit 2; }
P="$T/proj"; mkdir -p "$P/.git"; cd "$P"

########## a: ${VAR} e ${VAR:-padrao} ##########
mkdir -p "$HOME/.katana"
cat > "$HOME/.katana/mcp.json" <<EOF
{"mcpServers":{
 "ex":{"command":"\${KT_FAKE}","args":["env","\${KT_ARG:-fallback}","a\${KT_ARG2}b","\$\${literal}"],"env":{"KT_SECRET":"tok-\${KT_TOKEN}"}},
 "gone":{"command":"\${KT_FAKE}","args":["env"],"env":{"KT_SECRET":"\${KT_MISSING_ONE}"}}
}}
EOF
export KT_PATH=p KT_FAKE="$FAKE" KT_TOKEN=s3cr3tvalue KT_ARG2=MID
R=$("$K" mcp list 2>&1)
has "a: servidor com variaveis conecta" "$R" "ex [stdio] ok"
has "a: variavel ausente: servidor pulado" "$R" "gone [stdio] unavailable"
has "a: variavel ausente: erro nomeia a variavel" "$R" "KT_MISSING_ONE is not set"
has "a: erro diz o campo" "$R" "field env"
R=$("$K" tool ex__argv '{}' 2>&1)
has "a: padrao quando a variavel falta" "$R" "[fallback]"
has "a: variavel no meio do texto" "$R" "[aMIDb]"
has "a: \$\${ vira \${ literal" "$R" '[${literal}]'
R=$("$K" tool ex__env '{"name":"KT_SECRET"}' 2>&1)
has "a: env expandido chega ao servidor" "$R" "KT_SECRET=tok-s3cr3tvalue"
R=$("$K" mcp list 2>&1; cat "$T/mcp.log")
hasnt "a: o segredo nao aparece na listagem nem no log" "$R" "s3cr3tvalue"
R=$(KT_ARG=given "$K" tool ex__argv '{}' 2>&1); has "a: variavel definida vence o padrao" "$R" "[given]"
R=$(KT_ARG= "$K" tool ex__argv '{}' 2>&1); has "a: variavel vazia usa o padrao" "$R" "[fallback]"
# url e headers
cat > "$HOME/.katana/mcp.json" <<'EOF'
{"mcpServers":{"web":{"url":"http://127.0.0.1:9/${KT_PATH}","headers":{"Authorization":"Bearer ${KT_NOPE}"}}}}
EOF
R=$("$K" mcp list 2>&1); has "a: header com variavel ausente: pulado" "$R" "KT_NOPE is not set"; has "a: campo headers" "$R" "field headers"
cat > "$HOME/.katana/mcp.json" <<'EOF'
{"mcpServers":{"web":{"url":"http://127.0.0.1:9/${KT_NOPE2}"}}}
EOF
R=$("$K" mcp list 2>&1); has "a: url com variavel ausente: pulado" "$R" "field url"
R=$("$K" mcp test web 2>&1); has "a: mcp test mostra o erro" "$R" "KT_NOPE2 is not set"

########## b: roots ##########
cat > "$HOME/.katana/mcp.json" <<EOF
{"mcpServers":{"rt":{"command":"$FAKE","args":["roots"]}}}
EOF
R=$("$K" tool rt__caps '{}' 2>&1)
has "b: declara roots com listChanged" "$R" '"roots":{"listChanged":true}'
has "b: oferece a revisao 2025-11-25" "$R" '2025-11-25'
R=$("$K" tool rt__roots '{}' 2>&1)
has "b: roots/list responde a pasta de trabalho como file://" "$R" "file://$P"
has "b: roots/list traz o nome da pasta" "$R" '"name":"proj"'
mkdir -p "$T/my proj"
R=$(cd "$T/my proj" && "$K" tool rt__roots '{}' 2>&1)
has "b: caracteres especiais na URI" "$R" "my%20proj"
R=$(printf 'rt__rcount {}\n!cd %s\nrt__rcount {}\n!cd %s\nrt__rcount {}\n' "$T/my proj" "$T/my proj" | "$K" toolrun 2>&1)
has "b: sem mudanca de pasta: nenhum aviso" "$R" "roots_changed=0"
has "b: trocar de pasta envia roots/list_changed" "$R" "roots_changed=1"
R=$(printf 'rt__rcount {}\n!cd %s\n!cd %s\nrt__rcount {}\n' "$T/my proj" "$P" | "$K" toolrun 2>&1)
has "b: ida e volta enviam dois avisos" "$R" "roots_changed=2"

########## c: resources/list_changed e prompts/list_changed ##########
R=$(printf 'rt__plistcount {}\nListMcpResourcesTool {}\nrt__mutate {}\nrt__plistcount {}\nListMcpResourcesTool {}\n' | "$K" toolrun 2>&1)
has "c: recursos antes" "$R" "file:///r1"
has "c: aviso dos recursos no resultado da chamada" "$R" "the resource list of this MCP server changed"
has "c: aviso dos prompts no resultado da chamada" "$R" "the prompt list of this MCP server changed"
has "c: prompts relidos uma vez" "$R" "prompts_list=2"
has "c: recursos depois" "$R" "file:///r2"
has "c: lista inicial de prompts lida uma vez" "$R" "prompts_list=1"
R=$(printf 'rt__plistcount {}\nrt__plistcount {}\n' | "$K" toolrun 2>&1)
hasnt "c: sem mudanca, sem aviso" "$R" "list of this MCP server changed"

echo "mcpx: $N testes, $F falhas"
[ "$F" = 0 ]
