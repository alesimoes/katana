#!/bin/sh
# Testes de ListMcpResourcesTool, ReadMcpResourceTool, WaitForMcpServers, ToolSearch, Skill e ReportFindings.
# Uso: sh tools/tooltest/meta.sh /caminho/do/katana
K=$1
[ -x "$K" ] || { echo "uso: $0 <binario katana>"; exit 2; }
K=$(cd "$(dirname "$K")" && pwd)/$(basename "$K")
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
N=0; F=0
ok()   { N=$((N+1)); echo "ok   $1"; }
bad()  { N=$((N+1)); F=$((F+1)); echo "FAIL $1"; echo "---- saida:"; printf '%s\n' "$2" | head -20; echo "----"; }
has()  { case $2 in *"$3"*) ok "$1";; *) bad "$1 (faltou: $3)" "$2";; esac; }
hasnt(){ case $2 in *"$3"*) bad "$1 (nao devia ter: $3)" "$2";; *) ok "$1";; esac; }
eq()   { [ "$2" = "$3" ] && ok "$1" || bad "$1 (esperado: $3)" "$2"; }

export HOME="$T/home"; mkdir -p "$HOME" "$T/work"
cd "$T/work" || exit 2

########## MCP ##########
if command -v python3 >/dev/null 2>&1; then
cat > "$T/fake.py" <<'PY'
import sys, json
mode = sys.argv[1]
def send(o): sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    m = json.loads(line)
    meth, i = m.get("method"), m.get("id")
    if i is None: continue
    if meth == "initialize":
        caps = {"tools": {}}
        if mode == "res": caps["resources"] = {}
        send({"jsonrpc": "2.0", "id": i, "result": {"protocolVersion": "2025-06-18", "capabilities": caps, "serverInfo": {"name": "fake-" + mode, "version": "1.0"}}})
    elif meth == "tools/list":
        send({"jsonrpc": "2.0", "id": i, "result": {"tools": [{"name": "ping", "description": "Ping the fake server " + mode, "inputSchema": {"type": "object", "properties": {"host": {"type": "string"}}, "required": ["host"]}}]}})
    elif meth == "resources/list" and mode == "res":
        cur = (m.get("params") or {}).get("cursor")
        if not cur:
            send({"jsonrpc": "2.0", "id": i, "result": {"resources": [{"uri": "file:///a.txt", "name": "a.txt", "mimeType": "text/plain", "description": "First\nfile | with pipe"}], "nextCursor": "p2"}})
        else:
            send({"jsonrpc": "2.0", "id": i, "result": {"resources": [{"uri": "file:///img.png", "name": "img", "mimeType": "image/png"}]}})
    elif meth == "resources/read" and mode == "res":
        u = m["params"]["uri"]
        if u == "file:///a.txt":
            send({"jsonrpc": "2.0", "id": i, "result": {"contents": [{"uri": u, "mimeType": "text/plain", "text": "hello\nworld"}]}})
        elif u == "file:///img.png":
            send({"jsonrpc": "2.0", "id": i, "result": {"contents": [{"uri": u, "mimeType": "image/png", "blob": "aGVsbG8="}]}})
        else:
            send({"jsonrpc": "2.0", "id": i, "error": {"code": -32002, "message": "Resource not found: " + u}})
    else:
        send({"jsonrpc": "2.0", "id": i, "error": {"code": -32601, "message": "Method not found"}})
PY
PY3=$(command -v python3)
export KATANA_MCP_CONFIG="$T/none.json"
R=$("$K" tool ListMcpResourcesTool '{}'); has "sem servidores: mensagem clara" "$R" "No MCP servers are configured"
R=$("$K" tool WaitForMcpServers '{}'); has "wait sem servidores" "$R" "No MCP servers are configured"

cat > "$T/mcp.json" <<EOF
{"mcpServers": {
  "withres": {"command": "$PY3", "args": ["$T/fake.py", "res"]},
  "nores": {"command": "$PY3", "args": ["$T/fake.py", "nores"]},
  "broken": {"command": "$T/does-not-exist"}
}}
EOF
export KATANA_MCP_CONFIG="$T/mcp.json"

R=$("$K" tool ListMcpResourcesTool '{}' 2>/dev/null)
has "lista: recurso texto (com paginacao)" "$R" "withres | file:///a.txt | a.txt | text/plain | First file   with pipe"
has "lista: segunda pagina" "$R" "withres | file:///img.png | img | image/png |"
hasnt "lista: servidor sem recursos ignorado" "$R" "nores"
eq "lista: duas linhas" "$(printf '%s\n' "$R" | wc -l | tr -d ' ')" "2"
R=$("$K" tool ListMcpResourcesTool '{"server":"withres"}' 2>/dev/null)
has "lista com server" "$R" "withres | file:///a.txt"
R=$("$K" tool ListMcpResourcesTool '{"server":"nores"}' 2>/dev/null)
eq "lista de servidor sem recursos" "$R" "MCP server nores does not offer resources."
R=$("$K" tool ListMcpResourcesTool '{"server":"zzz"}' 2>/dev/null)
has "lista: servidor desconhecido" "$R" "error: unknown MCP server 'zzz'. Configured servers: withres, nores, broken"
R=$("$K" tool ListMcpResourcesTool '{"server":"broken"}' 2>/dev/null)
has "lista: servidor quebrado" "$R" "error: MCP server broken is unavailable"

R=$("$K" tool ReadMcpResourceTool '{"server":"withres","uri":"file:///a.txt"}' 2>/dev/null)
has "read: marca de conteudo externo" "$R" "[external content from MCP resource withres file:///a.txt"
has "read: uri e texto" "$R" "--- file:///a.txt
hello
world"
R=$("$K" tool ReadMcpResourceTool '{"server":"withres","uri":"file:///img.png"}' 2>/dev/null)
has "read: blob so mimeType e tamanho" "$R" "[binary content, mimeType image/png, 5 bytes]"
hasnt "read: blob nao vaza base64" "$R" "aGVsbG8"
R=$("$K" tool ReadMcpResourceTool '{"server":"withres","uri":"file:///nope"}' 2>/dev/null)
has "read: uri desconhecida" "$R" "error: MCP server withres could not read file:///nope: Resource not found: file:///nope"
R=$("$K" tool ReadMcpResourceTool '{"server":"nores","uri":"file:///a.txt"}' 2>/dev/null)
has "read: servidor sem recursos" "$R" "error: MCP server nores could not read file:///a.txt: Method not found"
R=$("$K" tool ReadMcpResourceTool '{"server":"zzz","uri":"x"}' 2>/dev/null)
has "read: servidor desconhecido" "$R" "error: unknown MCP server 'zzz'"
R=$("$K" tool ReadMcpResourceTool '{"server":"withres"}' 2>/dev/null)
has "read: falta uri" "$R" "error: server and uri are required"

R=$("$K" tool WaitForMcpServers '{}' 2>/dev/null)
has "wait: conectado" "$R" "withres: connected, 1 tools, server fake-res 1.0"
has "wait: falhou" "$R" "broken: failed ("
has "wait: totais" "$R" "2 connected, 1 failed or unknown."
R=$("$K" tool WaitForMcpServers '{"servers":["nores","zzz"],"timeout_ms":500}' 2>/dev/null)
has "wait: lista de nomes" "$R" "nores: connected"
has "wait: nome desconhecido" "$R" "zzz: unknown server"
hasnt "wait: so os pedidos" "$R" "withres:"

R=$("$K" tool ToolSearch '{"query":"select:withres__ping"}' 2>/dev/null)
has "toolsearch: select MCP" "$R" '{"type": "function", "function": {"name": "withres__ping", "description": "Ping the fake server res (MCP server withres)", "parameters": {"type":"object","properties":{"host":{"type":"string"}},"required":["host"]}}}'
R=$("$K" tool ToolSearch '{"query":"ping","max_results":1}' 2>/dev/null)
eq "toolsearch: max_results" "$(printf '%s\n' "$R" | wc -l | tr -d ' ')" "1"
R=$("$K" tool ToolSearch '{"query":"+nores ping"}' 2>/dev/null)
has "toolsearch: +palavra no nome" "$R" '"name": "nores__ping"'
hasnt "toolsearch: +palavra exclui os outros" "$R" 'withres__ping'
else
echo "SKIPPED mcp (python3 ausente)"
fi

########## ToolSearch ##########
export KATANA_MCP_CONFIG="$T/none.json"
R=$("$K" tool ToolSearch '{"query":"zzzqqq"}')
eq "toolsearch: sem resultado" "$R" "No matching tools"
R=$("$K" tool ToolSearch '{"query":"select:NoSuchTool"}')
eq "toolsearch: select desconhecido" "$R" "No matching tools"
R=$("$K" tool ToolSearch '{}'); has "toolsearch: sem query" "$R" "error: query is required"
R=$("$K" tool ToolSearch '{"query":"select:Read"}')
case $R in
  "No matching tools") echo "SKIPPED toolsearch da tabela (table.c ainda sem ct_tool_info)";;
  *) has "toolsearch: select Read" "$R" '{"type": "function", "function": {"name": "Read", "description": '
     has "toolsearch: esquema do Read" "$R" '"parameters": {"type": "object", "properties": {'
     R=$("$K" tool ToolSearch '{"query":"select:read,GLOB"}')
     eq "toolsearch: select ignora maiusculas, 2 ferramentas" "$(printf '%s\n' "$R" | wc -l | tr -d ' ')" "2"
     R=$("$K" tool ToolSearch '{"query":"+notebook"}')
     has "toolsearch: +notebook" "$R" '"name": "NotebookEdit"'
     R=$("$K" tool ToolSearch '{"query":"search the web","max_results":2}')
     has "toolsearch: palavras" "$R" '"name": "Web'
     eq "toolsearch: max 2" "$(printf '%s\n' "$R" | wc -l | tr -d ' ')" "2";;
esac

########## Skill ##########
mkdir -p "$T/work/.katana/skills/deploy" "$T/work/.config/katana/skills/deploy" "$T/work/.config/katana/skills/review" "$HOME/.katana/skills/hk" "$HOME/.config/katana/skills/hc" "$HOME/.config/katana/skills/review"
printf -- '---\nname: deploy\ndescription: "Deploy the app"\n---\n# Deploy katana\nRun ./deploy.sh\n' > "$T/work/.katana/skills/deploy/SKILL.md"
printf -- '---\nname: deploy\ndescription: Second deploy\n---\nSECOND VERSION\n' > "$T/work/.config/katana/skills/deploy/SKILL.md"
printf 'No front matter here.\nSecond line.\n' > "$T/work/.config/katana/skills/review/SKILL.md"
printf -- '---\ndescription: From home katana\n---\nHK body\n' > "$HOME/.katana/skills/hk/SKILL.md"
printf -- '---\ndescription: From home config\n---\nHC body\n' > "$HOME/.config/katana/skills/hc/SKILL.md"
printf 'HOME REVIEW\n' > "$HOME/.config/katana/skills/review/SKILL.md"
printf 'SECRET\n' > "$T/secret"

R=$("$K" tool Skill '{"skill":"deploy"}')
has "skill: linha de cabecalho com base" "$R" "Loaded skill: deploy (base directory: $T/work/.katana/skills/deploy)"
has "skill: corpo" "$R" "Run ./deploy.sh"
hasnt "skill: sem front matter" "$R" "description:"
hasnt "skill: .katana vence .config" "$R" "SECOND VERSION"
R=$("$K" tool Skill '{"skill":"review"}')
has "skill: pasta de trabalho vence a home" "$R" "No front matter here."
hasnt "skill: sem cabecalho, corpo inteiro" "$R" "HOME REVIEW"
R=$("$K" tool Skill '{"skill":"hk"}'); has "skill: home .katana" "$R" "HK body"
R=$("$K" tool Skill '{"skill":"hc"}'); has "skill: home .config" "$R" "HC body"
R=$("$K" tool Skill '{"skill":"hk","args":"fast mode"}')
has "skill: args" "$R" "## Arguments
fast mode"
R=$("$K" tool Skill '{"skill":"howto-nao-existe"}' 2>&1)
has "skill: desconhecida lista" "$R" "error: unknown skill 'howto-nao-existe'."
has "skill: lista com descricao" "$R" "- deploy: Deploy the app"
has "skill: lista home" "$R" "- hc: From home config"
has "skill: lista embutidas" "$R" "- web: built-in recipe"
eq "skill: deploy aparece uma vez" "$(printf '%s\n' "$R" | grep -c '^- deploy')" "1"
R=$("$K" tool Skill '{}'); has "skill: sem nome lista" "$R" "Available skills:"
R=$("$K" tool Skill '{"skill":"web"}'); has "skill: embutida" "$R" "Loaded skill: web (base directory: built-in)"
has "skill: embutida traz a receita" "$R" "WEB: 1) web_search"
R=$("$K" tool Skill '{"skill":"../../secret"}'); has "skill: sem .. e barras" "$R" "error: invalid skill name"
hasnt "skill: nao leu fora" "$R" "SECRET"
R=$("$K" tool Skill '{"skill":"/etc/passwd"}'); has "skill: sem barra" "$R" "error: invalid skill name"
R=$("$K" tool Skill '{"skill":"all"}'); has "skill: all nao e skill" "$R" "error: unknown skill 'all'."

########## ReportFindings ##########
R=$("$K" tool ReportFindings '{"findings":[]}'); eq "findings: vazio" "$R" "No findings"
R=$("$K" tool ReportFindings '{"findings":[],"level":"high"}'); eq "findings: vazio com nivel" "$R" "No findings (level: high)"
J='{"findings":[{"file":"src/a.c","line":42,"summary":"Null deref","failure_scenario":"empty input","category":"bug","short_summary":"null","verdict":"real","outcome":"fixed"},{"file":"b.c","summary":"Leak with \"quotes\"","failure_scenario":"loop of 1e6"}],"level":"medium"}'
R=$("$K" tool ReportFindings "$J")
has "findings: cabecalho" "$R" "2 findings (level: medium), most severe first:"
has "findings: primeiro" "$R" "1. src/a.c:42 [bug]
   Null deref
   Failure scenario: empty input"
has "findings: campos opcionais" "$R" "   Short summary: null
   Verdict: real
   Outcome: fixed"
has "findings: segundo sem linha" "$R" '2. b.c
   Leak with "quotes"'
R=$("$K" tool ReportFindings '{"findings":[{"file":"a.c","summary":"x"}]}'); eq "findings: falta failure_scenario" "$R" "error: findings[0] is missing failure_scenario (file, summary and failure_scenario are required)"
R=$("$K" tool ReportFindings '{"findings":[{"file":"a.c","summary":"x","failure_scenario":"y"},{"summary":"x","failure_scenario":"y"}]}'); has "findings: falta file no segundo" "$R" "error: findings[1] is missing file"
R=$("$K" tool ReportFindings '{"findings":["texto"]}'); has "findings: item nao objeto" "$R" "error: findings[0] must be an object"
R=$("$K" tool ReportFindings '{}'); has "findings: sem o array" "$R" "error: findings must be an array"
R=$("$K" tool ReportFindings '{"findings":"x"}'); has "findings: nao e array" "$R" "error: findings must be an array"
R=$(KATANA_UI=1 "$K" tool ReportFindings "$J" | tr '\001' '@' | head -2)
TAB=$(printf '\t')
eq "findings: marcadores da interface" "$R" "@FINDING src/a.c${TAB}42${TAB}bug${TAB}Null deref
@FINDING b.c${TAB}${TAB}${TAB}Leak with \"quotes\""
R=$("$K" tool ReportFindings "$J" | tr '\001' '@')
hasnt "findings: sem KATANA_UI nao emite marcador" "$R" '@FINDING'

if [ $F -eq 0 ]; then echo "OK $N checks"; else echo "FAILED $F"; exit 1; fi
