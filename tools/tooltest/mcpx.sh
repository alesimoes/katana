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

########## d: anotacoes das ferramentas ##########
R=$("$K" mcp list 2>&1)
has "d: list mostra somente leitura e idempotente" "$R" "rt__peek [read-only, idempotent]"
has "d: list mostra destrutiva e mundo aberto" "$R" "rt__wipe [destructive, open-world]"
has "d: list mostra o title (campo da ferramenta)" "$R" "title: Peek at it"
has "d: list mostra o title das anotacoes" "$R" "title: Wipe all"
has "d: ferramenta sem dicas sem etiqueta" "$R" "rt__argv: the program arguments"
R=$(KATANA_CONFIRM=1 "$K" tool rt__argv '{}' 2>&1 </dev/null)
has "d: KATANA_CONFIRM pergunta (sem usuario: nega)" "$R" "the user denied"
R=$(KATANA_CONFIRM=1 "$K" tool rt__peek '{}' 2>&1 </dev/null)
has "d: somente leitura de servidor do usuario dispensa a pergunta" "$R" "peek ran"
R=$(KATANA_CONFIRM=1 "$K" tool rt__liar '{}' 2>&1 </dev/null)
has "d: dica contraditoria nao dispensa a pergunta" "$R" "the user denied"
R=$(KATANA_CONFIRM=1 "$K" tool rt__wipe '{}' 2>&1 </dev/null)
has "d: destrutiva continua perguntando" "$R" "the user denied"
R=$(printf 'rt__wipe {}\n\001ANS 1\n' | KATANA_CONFIRM=1 KATANA_UI=1 "$K" toolrun 2>&1)
has "d: o texto da pergunta traz as dicas, marcadas como nao verificadas" "$R" "dicas do servidor (nao verificadas): destructive, open-world"
has "d: depois do sim a chamada roda" "$R" "wipe ran"
R=$(printf '!plan on\nrt__peek {}\nrt__wipe {}\nrt__argv {}\nrt__liar {}\n' | "$K" toolrun 2>&1)
has "d: plano: somente leitura de servidor do usuario roda" "$R" "peek ran"
hasnt "d: plano: destrutiva nao roda" "$R" "wipe ran"
hasnt "d: plano: ferramenta sem dica nao roda" "$R" "the program arguments"
hasnt "d: plano: dica contraditoria nao roda" "$R" "liar ran"
has "d: plano recusa em palavras claras" "$R" "plan mode is on"
# servidor de projeto: as dicas nao valem nada
mkdir -p "$P/.katana"
printf '{"mcpServers":{"pj":{"command":"%s","args":["roots"]}}}\n' "$FAKE" > "$P/.katana/mcp.json"
R=$(KATANA_MCP_TRUST=1 KATANA_CONFIRM=1 "$K" tool pj__peek '{}' 2>&1 </dev/null)
has "d: servidor de projeto: somente leitura nao dispensa a pergunta" "$R" "the user denied"
R=$(printf '!plan on\npj__peek {}\n' | KATANA_MCP_TRUST=1 "$K" toolrun 2>&1)
has "d: servidor de projeto: plano recusa mesmo com a dica" "$R" "plan mode is on"
rm -rf "$P/.katana"
R=$(KATANA_MCP_CONFIRM_DESTRUCTIVE=1 "$K" tool rt__wipe '{}' 2>&1 </dev/null)
has "d: KATANA_MCP_CONFIRM_DESTRUCTIVE pergunta so nas destrutivas" "$R" "the user denied"
R=$(KATANA_MCP_CONFIRM_DESTRUCTIVE=1 "$K" tool rt__argv '{}' 2>&1 </dev/null)
hasnt "d: ... e as outras seguem sem pergunta" "$R" "the user denied"

########## e: .mcp.json na raiz do projeto ##########
PE="$T/pe"; mkdir -p "$PE/.git" "$PE/src/deep"; cd "$PE"
printf '{"mcpServers":{"shared":{"command":"%s","args":["argv","user"]}}}\n' "$FAKE" > "$HOME/.katana/mcp.json"
rm -f "$HOME/.katana/mcp-trust"
printf '{"mcpServers":{"pa":{"type":"stdio","command":"%s","args":["argv","pa"]},"shared":{"command":"%s","args":["argv","root"]}}}\n' "$FAKE" "$FAKE" > "$PE/.mcp.json"
R=$("$K" mcp list 2>&1 </dev/null)
has "e: .mcp.json nao confiavel: pulado sem terminal" "$R" "not trusted yet"
has "e: a mensagem cita o arquivo" "$R" ".mcp.json"
hasnt "e: nao inicia" "$R" "pa [stdio]"
R=$(printf '\001ANS 1\n' | KATANA_UI=1 "$K" mcp list 2>&1)
has "e: a mesma pergunta de confianca" "$R" "ASK action"
has "e: aceito: o servidor do .mcp.json conecta" "$R" "pa [stdio] ok"
has "e: o servidor diz de onde veio" "$R" "from project file $PE/.mcp.json"
R=$("$K" mcp list 2>&1 </dev/null)
has "e: confianca lembrada por caminho e conteudo" "$R" "pa [stdio] ok"
R=$("$K" tool shared__argv '{}' 2>&1 </dev/null)
has "e: .mcp.json vence o arquivo do usuario" "$R" "[root]"
printf '{"mcpServers":{"shared":{"command":"%s","args":["argv","katana"]}}}\n' "$FAKE" > "$PE/.katana/mcp.json" 2>/dev/null || { mkdir -p "$PE/.katana"; printf '{"mcpServers":{"shared":{"command":"%s","args":["argv","katana"]}}}\n' "$FAKE" > "$PE/.katana/mcp.json"; }
R=$(KATANA_MCP_TRUST=1 "$K" tool shared__argv '{}' 2>&1 </dev/null)
has "e: .katana/mcp.json vence o .mcp.json" "$R" "[katana]"
R=$(KATANA_MCP_TRUST=1 "$K" mcp list 2>&1 </dev/null)
has "e: os dois arquivos de projeto somam servidores" "$R" "pa [stdio] ok"
R=$(cd "$PE/src/deep" && "$K" tool pa__argv '{}' 2>&1 </dev/null)
has "e: achado a partir de uma subpasta" "$R" "[pa]"
printf '{"mcpServers":{"pa":{"command":"%s","args":["argv","changed"]}}}\n' "$FAKE" > "$PE/.mcp.json"
R=$(rm -f "$PE/.katana/mcp.json"; "$K" mcp list 2>&1 </dev/null)
has "e: .mcp.json alterado: a confianca anterior nao vale" "$R" "not trusted yet"
R=$(KATANA_MCP_STRICT=1 KATANA_MCP_TRUST=1 "$K" mcp list 2>&1 </dev/null)
hasnt "e: KATANA_MCP_STRICT ignora o .mcp.json" "$R" "pa [stdio]"
rm -f "$PE/.mcp.json"
R=$("$K" tool shared__argv '{}' 2>&1 </dev/null)
has "e: sem arquivos de projeto, volta ao do usuario" "$R" "[user]"
cd "$P"

########## f: elicitation ##########
printf '{"mcpServers":{"rt":{"command":"%s","args":["elicit"]}}}\n' "$FAKE" > "$HOME/.katana/mcp.json"
R=$("$K" tool rt__caps '{}' 2>&1)
has "f: declara elicitation form" "$R" '"elicitation":{"form":{}}'
R=$(printf 'rt__ask {}\n\001ANS Answer\n\001ANS Alice\n\001ANS abc\n\001ANS 30\n\001ANS yes\n\001ANS green\n\001ANS a,c\n' | KATANA_UI=1 "$K" toolrun 2>&1)
has "f: a pergunta diz qual servidor pede" "$R" "CHOICE MCP server 'rt' asks you for information: Need your details"
has "f: campos pedidos na ordem, com titulo" "$R" "[mcp rt] Your name (required)"
has "f: aceito: o conteudo volta tipado" "$R" '"content":{"name":"Alice","age":30,"agree":true,"color":"green","tags":["a","c"]}'
has "f: aceito: acao accept" "$R" '"action":"accept"'
R=$(printf 'rt__ask {}\n\001ANS Answer\n\001ANS Bob\n\001ANS 200\n\001ANS 200\n\001ANS 200\n' | KATANA_UI=1 "$K" toolrun 2>&1)
has "f: valor fora do limite tres vezes: cancela" "$R" '"action":"cancel"'
R=$(printf 'rt__ask {}\n\001ANS Answer\n\001ANS Bob\n\001ANS 40\n\001ANS \n\001ANS \n\001ANS \n' | KATANA_UI=1 "$K" toolrun 2>&1)
has "f: opcionais vazios sao pulados" "$R" '"content":{"name":"Bob","age":40}'
R=$(printf 'rt__ask {}\n\001ANS Decline\n' | KATANA_UI=1 "$K" toolrun 2>&1)
has "f: recusar devolve decline" "$R" '"action":"decline"'
R=$(printf 'rt__ask {}\n\001ANS\n' | KATANA_UI=1 "$K" toolrun 2>&1)
has "f: cancelar (Stop) devolve cancel" "$R" '"action":"cancel"'
R=$(printf 'rt__ask {}\n\001ANS Answer\n\001ANS\n' | KATANA_UI=1 "$K" toolrun 2>&1)
has "f: campo obrigatorio cancelado: cancel" "$R" '"action":"cancel"'
R=$("$K" tool rt__ask '{}' 2>&1 </dev/null)
has "f: sem ninguem para responder (sem tela): decline" "$R" '"action":"decline"'
has "f: ... e diz por que" "$R" "nobody to ask"
R=$(printf 'rt__askurl {}\n' | KATANA_UI=1 "$K" toolrun 2>&1)
has "f: modo url nao declarado: erro ao servidor" "$R" '"code":-32602'
R=$(printf 'rt__askbad {}\n' | KATANA_UI=1 "$K" toolrun 2>&1)
has "f: campo aninhado: erro ao servidor" "$R" '"code":-32602'
R=$(printf 'rt__askpw {}\n\001ANS Answer\n\001ANS 0\n' | KATANA_UI=1 "$K" toolrun 2>&1)
has "f: campo de senha: pede confirmacao explicita" "$R" "ASK action	send what looks like a secret ('password') to the MCP server 'rt'"
has "f: senha nao confirmada: decline" "$R" '"action":"decline"'
R=$(printf 'rt__askpw {}\n\001ANS Answer\n\001ANS 1\n\001ANS hunter2xyz\n' | KATANA_UI=1 "$K" toolrun 2>&1)
has "f: senha confirmada: segue" "$R" '"content":{"password":"hunter2xyz"}'
hasnt "f: o valor digitado nao vai para o log do MCP" "$(cat "$T/mcp.log")" "hunter2xyz"
has "f: o log guarda so a decisao" "$(cat "$T/mcp.log")" "elicitation"

echo "mcpx: $N testes, $F falhas"
[ "$F" = 0 ]
