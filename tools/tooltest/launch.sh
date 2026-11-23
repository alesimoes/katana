#!/bin/sh
# Testes do console na pasta atual e do modo headless (docs/console.md): escolha do modelo, install/uninstall, sessoes por
# pasta, adopt, lista de ferramentas permitidas, --strict-mcp-config e pedidos headless de ponta a ponta (modelo minusculo
# de mentira dirigido pelo professor de mentira de tools/traintest, sem GPU e sem modelo de verdade).
# Cada teste usa um HOME temporario: nada toca o ~/.katana de quem roda.
# Uso: sh tools/tooltest/launch.sh /caminho/do/katana   (a partir da raiz do repositorio)
K=$1
[ -x "$K" ] || { echo "uso: $0 <binario katana>"; exit 2; }
K=$(cd "$(dirname "$K")" && pwd)/$(basename "$K")
R=$(pwd)
[ -f "$R/tools/traintest/mock_teacher_prog.py" ] || { echo "rode a partir da raiz do repositorio"; exit 2; }
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
N=0; F=0
ok()   { N=$((N+1)); echo "ok   $1"; }
bad()  { N=$((N+1)); F=$((F+1)); echo "FAIL $1"; echo "---- saida:"; printf '%s\n' "$2" | head -25; echo "----"; }
has()  { case $2 in *"$3"*) ok "$1";; *) bad "$1 (faltou: $3)" "$2";; esac; }
hasnt(){ case $2 in *"$3"*) bad "$1 (nao devia ter: $3)" "$2";; *) ok "$1";; esac; }
eq()   { [ "$2" = "$3" ] && ok "$1" || bad "$1 (esperado: $3)" "$2"; }
field(){ printf '%s\n' "$1" | sed -n "s/^$2=//p" | head -1; }

export KATANA_CPU=1                     # nunca a placa de video: so o modelo minusculo
export KATANA_LOG="$T/agente.log" KATANA_MCP_LOG="$T/mcp.log"
unset KATANA_TOOLS KATANA_MODEL KATANA_MODELS KATANA_TEACHER_CMD KATANA_ALLOWED_TOOLS KATANA_DISALLOWED_TOOLS
export HOME="$T/home"; mkdir -p "$HOME" "$T/work" "$T/other"
export KATANA_LAUNCH_DRYRUN=1          # so decide e imprime (mode=, model=, why=...), sem carregar o modelo
dry() { ( cd "${DIR:-$T/work}" && "$K" "$@" 2>&1 ); }

########## 1. escolha do modelo ##########
M="$T/models"; mkdir -p "$M"
export KATANA_MODELS="$M"
OUT=$(dry); RC=$?
has "sem modelo instalado: diz o que fazer" "$OUT" "katana catalog"
has "sem modelo instalado: pull" "$OUT" "katana pull"
( cd "$T/work" && KATANA_LAUNCH_DRYRUN=1 "$K" >/dev/null 2>&1 ); eq "sem modelo instalado: codigo de saida 2" "$?" "2"

: > "$M/alfa-small.gguf"; head -c 1000 /dev/zero > "$M/beta-big.gguf"; head -c 500 /dev/zero > "$M/Beta-Mid.gguf"
OUT=$(dry)
eq "o maior modelo instalado" "$(field "$OUT" model)" "$M/beta-big.gguf"
has "o maior: o motivo aparece" "$(field "$OUT" why)" "maior"
eq "modo console sem argumentos" "$(field "$OUT" mode)" "console"
OUT=$(dry --model alfa)
eq "--model por parte do nome" "$(field "$OUT" model)" "$M/alfa-small.gguf"
OUT=$(dry --model alfa-small.gguf)
eq "--model pelo nome exato" "$(field "$OUT" model)" "$M/alfa-small.gguf"
OUT=$(dry --model BETA-MID)
eq "--model ignora maiusculas" "$(field "$OUT" model)" "$M/Beta-Mid.gguf"
OUT=$(dry --model "$M/alfa-small.gguf")
eq "--model por caminho" "$(field "$OUT" model)" "$M/alfa-small.gguf"
OUT=$(dry --model beta)
has "--model ambiguo lista os candidatos (1)" "$OUT" "beta-big.gguf"
has "--model ambiguo lista os candidatos (2)" "$OUT" "Beta-Mid.gguf"
( cd "$T/work" && "$K" --model beta >/dev/null 2>&1 ); eq "--model ambiguo: codigo 2" "$?" "2"
OUT=$(dry --model nao-existe); has "--model inexistente" "$OUT" "nenhum modelo chamado"

# catalogo: o recomendado, se instalado (memoria simulada: modelos com especialistas pedem ~1,4x o tamanho)
: > "$M/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf"
OUT=$(KATANA_LAUNCH_RAM_GB=64 dry)
eq "recomendado do catalogo instalado" "$(field "$OUT" model)" "$M/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf"
has "recomendado: o motivo" "$(field "$OUT" why)" "recomendado do catalogo"
OUT=$(KATANA_LAUNCH_RAM_GB=8 dry)
eq "sem memoria para o recomendado: o maior" "$(field "$OUT" model)" "$M/beta-big.gguf"

# ordem: ultimo do aplicativo de mesa < katana.cfg do usuario < katana.cfg do projeto < KATANA_MODEL < --model
mkdir -p "$HOME/.katana/desktop"; printf '[General]\nultimoModelo=alfa-small.gguf\n' > "$HOME/.katana/desktop/ui.ini"
OUT=$(KATANA_LAUNCH_RAM_GB=64 dry)
eq "o ultimo usado no aplicativo de mesa" "$(field "$OUT" model)" "$M/alfa-small.gguf"
has "aplicativo de mesa: o motivo" "$(field "$OUT" why)" "aplicativo de mesa"
printf 'ultimoModelo=nao-existe.gguf\n' > "$HOME/.katana/desktop/ui.ini"
OUT=$(KATANA_LAUNCH_RAM_GB=8 dry); eq "ultimo do aplicativo que nao existe mais e ignorado" "$(field "$OUT" model)" "$M/beta-big.gguf"
printf 'ultimoModelo=alfa-small.gguf\n' > "$HOME/.katana/desktop/ui.ini"
printf 'gpu=x\nmodel=Beta-Mid\n' > "$HOME/.katana/katana.cfg"
OUT=$(dry); eq "model= do katana.cfg do usuario" "$(field "$OUT" model)" "$M/Beta-Mid.gguf"
has "katana.cfg: o motivo" "$(field "$OUT" why)" "katana.cfg"
mkdir -p "$T/work/.katana"; printf 'model=alfa-small.gguf\n' > "$T/work/.katana/katana.cfg"
OUT=$(dry); eq "model= do projeto vale mais que o do usuario" "$(field "$OUT" model)" "$M/alfa-small.gguf"
OUT=$(KATANA_MODEL=beta-big dry); eq "KATANA_MODEL vale mais que o katana.cfg" "$(field "$OUT" model)" "$M/beta-big.gguf"
OUT=$(KATANA_MODEL=beta-big dry --model Beta-Mid); eq "--model vale mais que KATANA_MODEL" "$(field "$OUT" model)" "$M/Beta-Mid.gguf"
rm -rf "$T/work/.katana" "$HOME/.katana/katana.cfg" "$HOME/.katana/desktop"

# opcoes: primeira mensagem, subcomando conhecido nao vira mensagem
OUT=$(dry "corrija o teste"); eq "primeira mensagem" "$(field "$OUT" first)" "corrija o teste"
OUT=$(dry --model alfa "e agora?"); eq "opcoes e primeira mensagem" "$(field "$OUT" first)" "e agora?"
OUT=$( cd "$T/work" && "$K" hw "$M" 2>&1 | head -2 ); has "hw continua sendo subcomando" "$OUT" "ram_gb"
OUT=$(dry --bogus); has "opcao desconhecida e recusada" "$OUT" "opcao desconhecida"
OUT=$(dry --help); has "--help lista as opcoes" "$OUT" "--allowed-tools"

########## 2. install / uninstall ##########
mkdir -p "$T/app/models"; cp "$K" "$T/app/katana"; head -c 700 /dev/zero > "$T/app/models/lado.gguf"
unset KATANA_MODELS
LNK="$T/lnk"
OUT=$(cd "$T/other" && "$T/app/katana" install --dir "$LNK" 2>&1); RC=$?
eq "install: codigo 0" "$RC" "0"
[ -L "$LNK/katana" ] && ok "install: cria o link" || bad "install: cria o link" "$OUT"
eq "install: o link aponta para o binario" "$(readlink "$LNK/katana")" "$T/app/katana"
has "install: avisa que a pasta nao esta no PATH" "$OUT" "nao esta no PATH"
has "install: diz como acrescentar" "$OUT" "export PATH="
has "install: grava models= do lado do binario" "$(cat "$HOME/.katana/katana.cfg")" "models=$T/app/models"
OUT=$(cd "$T/other" && PATH="$LNK:$PATH" "$T/app/katana" install --dir "$LNK" 2>&1); has "install de novo: ja instalado" "$OUT" "ja instalado"
hasnt "install com a pasta no PATH: sem aviso" "$OUT" "nao esta no PATH"
# rodando pelo link, de outra pasta: acha a pasta de modelos do lado do binario real
OUT=$(cd "$T/other" && "$LNK/katana" 2>&1)
eq "pelo link, de outra pasta: modelo do lado do binario" "$(field "$OUT" model)" "$T/app/models/lado.gguf"
rm -f "$HOME/.katana/katana.cfg"
OUT=$(cd "$T/other" && "$LNK/katana" 2>&1)
eq "sem katana.cfg: models ao lado do caminho real do binario" "$(field "$OUT" models_dir)" "$T/app/models"
# outro programa com o mesmo nome no destino
rm -f "$LNK/katana"; printf '#!/bin/sh\n' > "$LNK/katana"
OUT=$(cd "$T/other" && "$T/app/katana" install --dir "$LNK" 2>&1); RC=$?
eq "install sobre arquivo alheio: recusa" "$RC" "1"
has "install sobre arquivo alheio: pede --force" "$OUT" "--force"
OUT=$(cd "$T/other" && "$T/app/katana" install --dir "$LNK" --force 2>&1); [ -L "$LNK/katana" ] && ok "install --force troca" || bad "install --force troca" "$OUT"
OUT=$("$T/app/katana" uninstall --dir "$LNK" 2>&1); [ -e "$LNK/katana" ] && bad "uninstall remove o link" "$OUT" || ok "uninstall remove o link"
OUT=$("$T/app/katana" uninstall --dir "$LNK" 2>&1); has "uninstall sem link: nada a remover" "$OUT" "nada a remover"
rm -rf "$HOME/.katana"
export KATANA_MODELS="$M"

########## 3. ferramentas permitidas (sem modelo) ##########
cd "$T/work" || exit 2
OUT=$("$K" tool Bash '{"arguments":{"command":"echo oi-do-bash"}}' 2>&1); has "sem lista: a ferramenta roda" "$OUT" "oi-do-bash"
OUT=$(KATANA_ALLOWED_TOOLS=Read "$K" tool Bash '{"arguments":{"command":"echo oi-do-bash"}}' 2>&1)
has "fora da lista de permitidas: recusada" "$OUT" "not allowed"
hasnt "fora da lista: nao rodou" "$OUT" "oi-do-bash"
OUT=$(KATANA_ALLOWED_TOOLS='Re*,Grep' "$K" tool Glob '{"arguments":{"pattern":"*"}}' 2>&1); has "glob na lista: Glob fora" "$OUT" "not allowed"
OUT=$(KATANA_ALLOWED_TOOLS='Bash(echo:*),Grep' "$K" tool Bash '{"arguments":{"command":"echo permitido"}}' 2>&1); has "padrao com argumentos vale pelo nome" "$OUT" "permitido"
OUT=$(KATANA_DISALLOWED_TOOLS=Bash "$K" tool Bash '{"arguments":{"command":"echo oi-do-bash"}}' 2>&1); has "disallowed recusa" "$OUT" "not allowed"
OUT=$(KATANA_ALLOWED_TOOLS='Bash' KATANA_DISALLOWED_TOOLS='Ba*' "$K" tool Bash '{"arguments":{"command":"echo x"}}' 2>&1); has "disallowed vence allowed" "$OUT" "not allowed"
# o que nao e permitido nao e declarado ao modelo
OUT=$(KATANA_ALLOWED_TOOLS='Read,Grep' "$K" agentsys 2>&1)
has "declarada: permitida" "$OUT" '"name": "Read"'
hasnt "nao declarada: Bash" "$OUT" '"name": "Bash"'
hasnt "nao declarada: Write" "$OUT" '"name": "Write"'
OUT=$("$K" agentsys 2>&1); has "sem lista: Bash declarada" "$OUT" '"name": "Bash"'
# prompt de sistema trocado e acrescentado
printf 'Voce e o assistente do projeto ZZ.' > "$T/sys.txt"
OUT=$(KATANA_SYSTEM_FILE="$T/sys.txt" KATANA_SYSTEM_APPEND="REGRA-EXTRA-QQ" "$K" agentsys 2>&1)
has "--system-prompt-file troca as regras" "$OUT" "assistente do projeto ZZ"
hasnt "--system-prompt-file: as regras antigas saem" "$OUT" "How to work"
has "--system-prompt-file: ferramentas ficam" "$OUT" '"name": "Bash"'
has "--append-system-prompt acrescenta" "$OUT" "REGRA-EXTRA-QQ"

########## 4. adopt ##########
mkdir -p "$T/proj" "$T/src/skills/alfa/sub" "$T/src/skills/beta" "$T/src/skills/nao-e-skill"
printf '# Regras\nUse tabs.\n' > "$T/src/instr.md"
cat > "$T/src/mcp.json" <<'EOF'
{"mcpServers": {"um": {"command": "/bin/true", "args": ["a"]}, "dois": {"url": "http://127.0.0.1:1/mcp"}}}
EOF
printf -- '---\nname: alfa\ndescription: faz alfa\n---\nPassos de alfa\n' > "$T/src/skills/alfa/SKILL.md"; printf 'x\n' > "$T/src/skills/alfa/sub/extra.txt"
printf 'Beta\n' > "$T/src/skills/beta/SKILL.md"; printf 'x\n' > "$T/src/skills/nao-e-skill/leia.txt"
OUT=$("$K" adopt "$T/proj" --instructions "$T/src/instr.md" --mcp "$T/src/mcp.json" --skills "$T/src/skills" --dry-run 2>&1)
has "adopt --dry-run diz o que faria" "$OUT" "copiaria"
[ -e "$T/proj/KATANA.md" ] && bad "adopt --dry-run nao grava" "$OUT" || ok "adopt --dry-run nao grava"
OUT=$("$K" adopt "$T/proj" --instructions "$T/src/instr.md" --mcp "$T/src/mcp.json" --skills "$T/src/skills" 2>&1); RC=$?
eq "adopt: codigo 0" "$RC" "0"
eq "adopt: KATANA.md igual ao original" "$(cat "$T/proj/KATANA.md")" "$(cat "$T/src/instr.md")"
has "adopt: indica o /init" "$OUT" "/init"
has "adopt: mcp.json tem o servidor um" "$(cat "$T/proj/.katana/mcp.json")" '"um"'
has "adopt: mcp.json tem o servidor dois" "$(cat "$T/proj/.katana/mcp.json")" '"dois"'
[ -f "$T/proj/.katana/skills/alfa/SKILL.md" ] && [ -f "$T/proj/.katana/skills/alfa/sub/extra.txt" ] && [ -f "$T/proj/.katana/skills/beta/SKILL.md" ] && ok "adopt: skills copiadas com o conteudo" || bad "adopt: skills copiadas" "$OUT"
[ -e "$T/proj/.katana/skills/nao-e-skill" ] && bad "adopt: pasta sem SKILL.md fica de fora" "$OUT" || ok "adopt: pasta sem SKILL.md fica de fora"
has "adopt: diz o que conferir" "$OUT" "Confira"
# nunca sobrescreve sem --force
printf 'MEU\n' > "$T/proj/KATANA.md"
OUT=$("$K" adopt "$T/proj" --instructions "$T/src/instr.md" 2>&1); eq "adopt nao sobrescreve" "$(cat "$T/proj/KATANA.md")" "MEU"; has "adopt avisa que pulou" "$OUT" "ja existe"
"$K" adopt "$T/proj" --instructions "$T/src/instr.md" --force >/dev/null 2>&1; eq "adopt --force sobrescreve" "$(cat "$T/proj/KATANA.md")" "$(cat "$T/src/instr.md")"
# junta com servidores que ja existiam
printf '{"mcpServers": {"um": {"command": "/bin/false"}, "tres": {"command": "/bin/true"}}}\n' > "$T/src/mcp2.json"
"$K" adopt "$T/proj" --mcp "$T/src/mcp2.json" >/dev/null 2>&1
MJ=$(cat "$T/proj/.katana/mcp.json")
has "adopt junta: o novo entra" "$MJ" '"tres"'; has "adopt junta: o antigo fica" "$MJ" '"dois"'; has "adopt junta: o existente nao e trocado" "$MJ" '"/bin/true"'
hasnt "adopt junta: sem --force o existente nao muda" "$MJ" "/bin/false"
"$K" adopt "$T/proj" --mcp "$T/src/mcp2.json" --force >/dev/null 2>&1; has "adopt --force troca o servidor" "$(cat "$T/proj/.katana/mcp.json")" "/bin/false"
printf 'nao e json\n' > "$T/src/ruim.json"; OUT=$("$K" adopt "$T/proj" --mcp "$T/src/ruim.json" 2>&1); has "adopt: arquivo sem mcpServers" "$OUT" 'nao tem "mcpServers"'
OUT=$("$K" adopt "$T/proj" 2>&1); RC=$?; eq "adopt sem caminhos: erro" "$RC" "1"

########## 5. pedidos headless de ponta a ponta (modelo minusculo + professor de mentira) ##########
unset KATANA_LAUNCH_DRYRUN
if ! command -v python3 >/dev/null 2>&1; then echo "sem python3: testes headless pulados"; else
python3 "$R/tools/tinymodel/make_tiny_gguf.py" "$T/agent.gguf" --template chatml-xml >/dev/null || { echo "nao gerei o modelo minusculo"; exit 2; }
mkdir -p "$T/hm"; cp "$T/agent.gguf" "$T/hm/agent.gguf"
export KATANA_MODELS="$T/hm"
TENV="KATANA_TEACHER=1 KATANA_TOOLS=2 MOCK_TOOLS=2 KATANA_TEACHER_CMD='python3 $R/tools/traintest/mock_teacher_prog.py'"
hl() { # pasta de trabalho, depois os argumentos do katana
  _d=$1; shift; mkdir -p "$_d"; ( cd "$_d" && env $TENV MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" "$@" 2>"$T/err.txt" ); }
jq_() { python3 -c "import json,sys; d=json.loads(sys.stdin.read()); print(eval(sys.argv[1]))" "$1"; }

# texto
OUT=$(hl "$T/h1" -p "grave e confira"); RC=$?
eq "headless texto: codigo 0" "$RC" "0"
eq "headless texto: so a resposta final no stdout" "$OUT" "Pronto: ferramentas novas conferidas."
[ "$(cat "$T/h1/nota2.txt")" = "$(printf 'linha 1\nlinha dois')" ] && ok "headless: as ferramentas rodaram na pasta de trabalho" || bad "headless: arquivo gravado" "$(cat "$T/h1/nota2.txt" 2>&1)"
has "headless: o modelo escolhido sai no erro padrao" "$(cat "$T/err.txt")" "modelo $T/hm/agent.gguf"
# pedido pela entrada padrao
OUT=$(cd "$T/h2" 2>/dev/null || mkdir -p "$T/h2"; cd "$T/h2" && printf 'grave e confira\n' | env $TENV MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" -p 2>/dev/null); RC=$?
eq "headless pela entrada padrao" "$OUT" "Pronto: ferramentas novas conferidas."
# json
OUT=$(hl "$T/h3" -p "grave e confira" --output-format json --quiet)
eq "json: um objeto so" "$(printf '%s\n' "$OUT" | wc -l | tr -d ' ')" "1"
eq "json: result" "$(printf '%s' "$OUT" | jq_ 'd["result"]')" "Pronto: ferramentas novas conferidas."
eq "json: is_error" "$(printf '%s' "$OUT" | jq_ 'd["is_error"]')" "False"
eq "json: type" "$(printf '%s' "$OUT" | jq_ 'd["type"]')" "result"
eq "json: num_turns" "$(printf '%s' "$OUT" | jq_ 'd["num_turns"]')" "4"
eq "json: duration_ms numero" "$(printf '%s' "$OUT" | jq_ 'isinstance(d["duration_ms"], (int, float)) and d["duration_ms"] > 0')" "True"
eq "json: usage" "$(printf '%s' "$OUT" | jq_ 'd["usage"]["prompt_tokens"] > 0 and d["usage"]["completion_tokens"] > 0')" "True"
eq "json: tool_calls (nomes)" "$(printf '%s' "$OUT" | jq_ '",".join(c["name"] for c in d["tool_calls"])')" "Write,Edit,Bash,Grep"
eq "json: tool_calls (argumentos)" "$(printf '%s' "$OUT" | jq_ 'd["tool_calls"][1]["arguments"]["new_string"]')" "linha dois"
eq "json: tool_calls (is_error)" "$(printf '%s' "$OUT" | jq_ 'any(c["is_error"] for c in d["tool_calls"])')" "False"
SID=$(printf '%s' "$OUT" | jq_ 'd["session_id"]')
[ -f "$HOME/.katana/sessions/"*"/$SID.ksess" ] && ok "json: session_id e o arquivo da sessao da pasta" || bad "json: session_id" "$SID $(ls "$HOME/.katana/sessions/"* 2>&1)"
# stream-json: ordem dos eventos
OUT=$(hl "$T/h4" -p "grave e confira" --output-format stream-json --quiet)
ORDER=$(printf '%s\n' "$OUT" | python3 -c "
import json, sys
ev = [json.loads(l) for l in sys.stdin if l.strip()]
print(' '.join(e['type'] + (':' + e['name'] if 'name' in e else '') for e in ev))")
eq "stream-json: ordem dos eventos" "$ORDER" "text tool_call:Write tool_result:Write tool_call:Edit tool_result:Edit tool_call:Bash tool_result:Bash tool_call:Grep tool_result:Grep text result"
eq "stream-json: o primeiro texto" "$(printf '%s\n' "$OUT" | head -1 | jq_ 'd["text"]')" "Gravo o arquivo."
eq "stream-json: tool_call traz os argumentos" "$(printf '%s\n' "$OUT" | sed -n 2p | jq_ 'd["arguments"]["file_path"]')" "nota2.txt"
eq "stream-json: tool_result traz is_error e summary" "$(printf '%s\n' "$OUT" | sed -n 3p | jq_ 'd["is_error"] is False and "nota2.txt" in d["summary"]')" "True"
eq "stream-json: o ultimo e o resultado igual ao json" "$(printf '%s\n' "$OUT" | tail -1 | jq_ 'd["result"] + str(d["num_turns"]) + str(len(d["tool_calls"]))')" "Pronto: ferramentas novas conferidas.44"
# plano
mkdir -p "$T/h5"
OUT=$( cd "$T/h5" && env $TENV MOCK_PLAN=2 MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" -p "trabalhe" --output-format stream-json --quiet 2>/dev/null )
PL=$(printf '%s\n' "$OUT" | python3 -c "
import json, sys
ev = [json.loads(l) for l in sys.stdin if l.strip()]
p = [e for e in ev if e['type'] == 'plan']
print(len(p), ','.join(s['status'][0] for s in p[0]['steps']) if p else '-', ','.join(s['status'][0] for s in p[-1]['steps']) if p else '-')")
eq "stream-json: evento plan (2 mudancas da lista; primeiro passo em andamento; depois concluido)" "$PL" "2 i,p,p c,i,p"
# sem sessao
rm -rf "$HOME/.katana/sessions"
OUT=$(hl "$T/h6" -p "grave e confira" --output-format json --quiet --no-session)
eq "--no-session: session_id nulo" "$(printf '%s' "$OUT" | jq_ 'd["session_id"]')" "None"
[ -d "$HOME/.katana/sessions" ] && bad "--no-session: nada gravado" "$(ls -R "$HOME/.katana/sessions")" || ok "--no-session: nada gravado"
# --cwd e --session-id (e retomar pelo mesmo id)
mkdir -p "$T/h7"
OUT=$(cd "$T/other" && env $TENV MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" -p "grave e confira" --cwd "$T/h7" --session-id meu-id-1 --output-format json --quiet)
[ -f "$T/h7/nota2.txt" ] && ok "--cwd: trabalhou na pasta dada" || bad "--cwd" "$OUT"
eq "--session-id: o id volta no resultado" "$(printf '%s' "$OUT" | jq_ 'd["session_id"]')" "meu-id-1"
[ -f "$HOME/.katana/sessions/"*"/meu-id-1.ksess" ] && ok "--session-id: arquivo com esse nome" || bad "--session-id arquivo" "$(ls -R "$HOME/.katana/sessions")"
# --max-turns e erro (codigo 1)
OUT=$(cd "$T/h8" 2>/dev/null || mkdir -p "$T/h8"; cd "$T/h8" && env $TENV MOCK_SILENT=1 MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" -p "faca" --max-turns 2 --output-format json --quiet 2>"$T/err.txt"); RC=$?
eq "sem resposta final: codigo 1" "$RC" "1"
eq "sem resposta final: is_error" "$(printf '%s' "$OUT" | jq_ 'd["is_error"]')" "True"
eq "sem resposta final: subtype" "$(printf '%s' "$OUT" | jq_ 'd["subtype"]')" "error_max_turns"
eq "--max-turns: so 2 rodadas" "$(printf '%s' "$OUT" | jq_ 'd["num_turns"]')" "2"
has "sem resposta final: o motivo no erro padrao" "$(cat "$T/err.txt")" "max turns"
OUT=$(cd "$T/h8" && env $TENV MOCK_SILENT=1 MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" -p "faca" --max-turns 2 --quiet 2>/dev/null); RC=$?
eq "erro em texto: codigo 1 e nada no stdout" "$RC$OUT" "1"
OUT=$(cd "$T/h8" && env $TENV MOCK_ROUNDS=2 KATANA_ROUNDS=40 MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" -p "leia" --max-turns 4 --quiet 2>/dev/null)
eq "--max-turns 4: o limite chega ao modelo como nas rodadas do motor" "$OUT" "Limite relatado."
# erros de uso: codigo 2
( cd "$T/h8" && "$K" -p >/dev/null 2>&1 </dev/null ); [ $? -ne 0 ] && ok "-p sem pedido nem entrada: erro" || bad "-p sem pedido" ""
( cd "$T/h8" && "$K" -p "x" --output-format xml >/dev/null 2>&1 ); eq "--output-format invalido: codigo 2" "$?" "2"
( cd "$T/h8" && "$K" -p "x" --mcp-config "$T/nao.json" >/dev/null 2>&1 ); eq "--mcp-config inexistente: codigo 2" "$?" "2"

# servidor MCP de teste (stdio) por --mcp-config, com lista de permitidas e --strict-mcp-config
cat > "$T/fake.py" <<'PY'
import sys, json
def send(o): sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    m = json.loads(line)
    meth, i = m.get("method"), m.get("id")
    if i is None: continue
    if meth == "initialize":
        send({"jsonrpc": "2.0", "id": i, "result": {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}}, "serverInfo": {"name": "fake-" + sys.argv[1], "version": "1.0"}}})
    elif meth == "tools/list":
        send({"jsonrpc": "2.0", "id": i, "result": {"tools": [{"name": "ping", "description": "Ping a host", "inputSchema": {"type": "object", "properties": {"host": {"type": "string"}}, "required": ["host"]}}]}})
    elif meth == "tools/call":
        a = m["params"].get("arguments", {})
        send({"jsonrpc": "2.0", "id": i, "result": {"content": [{"type": "text", "text": "pong " + str(a.get("host"))}]}})
    else:
        send({"jsonrpc": "2.0", "id": i, "error": {"code": -32601, "message": "Method not found"}})
PY
PY3=$(command -v python3)
cat > "$T/mcp-fake.json" <<EOF
{"mcpServers": {"fake": {"command": "$PY3", "args": ["$T/fake.py", "fake"]}}}
EOF
mkdir -p "$HOME/.katana"; cat > "$HOME/.katana/mcp.json" <<EOF
{"mcpServers": {"userone": {"command": "$PY3", "args": ["$T/fake.py", "userone"]}}}
EOF
mkdir -p "$T/h9/.katana"; cat > "$T/h9/.katana/mcp.json" <<EOF
{"mcpServers": {"projone": {"command": "$PY3", "args": ["$T/fake.py", "projone"]}}}
EOF
OUT=$(cd "$T/h9" && env $TENV MOCK_MCP=1 MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" -p "use o servidor" --mcp-config "$T/mcp-fake.json" --allowed-tools 'fake__ping,Read' --output-format json 2>"$T/err.txt"); RC=$?
eq "MCP + lista: codigo 0" "$RC" "0"
eq "MCP + lista: o modelo viu so as ferramentas permitidas e respondeu" "$(printf '%s' "$OUT" | jq_ 'd["result"]')" "MCP ok: pong alvo; Bash recusado."
eq "MCP + lista: tool_calls" "$(printf '%s' "$OUT" | jq_ '",".join(c["name"] + ":" + str(c["is_error"]) for c in d["tool_calls"])')" "fake__ping:False,Bash:True"
OUT=$(cd "$T/h9" && env $TENV MOCK_MCP=1 MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" -p "use o servidor" --mcp-config "$T/mcp-fake.json" --allowed-tools 'mcp__fake__ping,Read' --output-format json --quiet 2>/dev/null)
eq "MCP: a lista aceita a grafia mcp__servidor__ferramenta" "$(printf '%s' "$OUT" | jq_ 'd["result"]')" "MCP ok: pong alvo; Bash recusado."
OUT=$(cd "$T/h9" && env $TENV MOCK_MCP=1 MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" -p "use o servidor" --mcp-config "{\"mcpServers\": {\"fake\": {\"command\": \"$PY3\", \"args\": [\"$T/fake.py\", \"fake\"]}}}" --allowed-tools 'fake__*,Read' --output-format json --quiet 2>/dev/null)
eq "MCP: configuracao em JSON direto na linha de comando" "$(printf '%s' "$OUT" | jq_ 'd["result"]')" "MCP ok: pong alvo; Bash recusado."
# --strict-mcp-config: so os servidores dados (o do usuario e o do projeto ficam de fora)
( cd "$T/h9" && env $TENV MOCK_MCP=1 MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" -p "use o servidor" --mcp-config "$T/mcp-fake.json" --strict-mcp-config --allowed-tools 'fake__ping,Read' --output-format json >/dev/null 2>"$T/err.txt" )
ERR=$(cat "$T/err.txt")
has "strict: o servidor dado sobe" "$ERR" "[mcp fake:"
hasnt "strict: o do usuario fica de fora" "$ERR" "userone"
hasnt "strict: o do projeto fica de fora" "$ERR" "projone"
( cd "$T/h9" && env $TENV MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" -p "grave e confira" --strict-mcp-config --output-format json >/dev/null 2>"$T/err.txt" )
hasnt "strict sem --mcp-config: nenhum servidor sobe (usuario)" "$(cat "$T/err.txt")" "userone"
hasnt "strict sem --mcp-config: nenhum servidor sobe (projeto)" "$(cat "$T/err.txt")" "projone"
( cd "$T/h9" && env $TENV MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" -p "grave e confira" --output-format json >/dev/null 2>"$T/err.txt" )
has "sem strict: o servidor do usuario sobe" "$(cat "$T/err.txt")" "[mcp userone:"
rm -rf "$HOME/.katana/mcp.json"

########## 6. sessoes por pasta: lista e continuar ##########
rm -rf "$HOME/.katana/sessions"
hl "$T/s1" -p "grave e confira" --session-id aaa-1 --quiet >/dev/null
sleep 1
hl "$T/s1" -p "leia o arquivo" --session-id bbb-2 --quiet >/dev/null
mkdir -p "$T/repo/.git" "$T/repo/sub"
hl "$T/repo" -p "grave e confira" --session-id raiz-1 --quiet >/dev/null
export KATANA_LAUNCH_DRYRUN=1
OUT=$(cd "$T/s1" && "$K" -c 2>&1 </dev/null)
eq "-c: a sessao mais recente da pasta" "$(basename "$(field "$OUT" resume)")" "bbb-2.ksess"
OUT=$(cd "$T/s1" && "$K" -r 2>&1 </dev/null)
has "-r: lista com a primeira mensagem (1)" "$OUT" "leia o arquivo"
has "-r: lista com a primeira mensagem (2)" "$OUT" "grave e confira"
has "-r: mostra os pedidos de cada sessao" "$OUT" "1 turn"
AID=$(basename "$(ls "$HOME/.katana/sessions/"*/aaa-1.ksess)" .ksess)
OUT=$(cd "$T/s1" && "$K" --resume 20000101-000000 2>&1 </dev/null); has "--resume de um id que nao existe" "$OUT" "nenhuma sessao"
OUT=$(cd "$T/repo/sub" && "$K" -c 2>&1 </dev/null)
eq "-c numa subpasta: usa a raiz do repositorio" "$(basename "$(field "$OUT" resume)")" "raiz-1.ksess"
OUT=$(cd "$T/other" && "$K" -c 2>&1 </dev/null)
has "-c numa pasta sem sessoes: avisa" "$OUT" "nenhuma sessao anterior"
eq "-c numa pasta sem sessoes: sessao nova" "$(field "$OUT" resume)" ""
ls "$HOME/.katana/sessions/"*/index >/dev/null 2>&1 && ok "o index de sessoes da pasta existe" || bad "index" "$(ls -R "$HOME/.katana/sessions")"
has "o index tem uma linha por sessao" "$(cat "$HOME/.katana/sessions/"*/index | cut -f1 | sort | tr '\n' ' ')" "aaa-1 bbb-2 raiz-1"
unset KATANA_LAUNCH_DRYRUN
# continua de verdade: -c retoma a conversa (o modelo minusculo carrega; o professor responde)
OUT=$(cd "$T/s1" && env $TENV MOCK_TEACHER_LOG="$T/cli.jsonl" "$K" -c -p "e agora?" --output-format json 2>"$T/err.txt"); RC=$?
eq "-c com -p: continua a sessao e responde (codigo)" "$RC" "0"
has "-c com -p: a sessao anterior foi retomada" "$(cat "$T/err.txt")" "sessao retomada"

########## 7. console rico e editor de linha, num terminal de mentira (pty) ##########
mkdir -p "$T/ptyhome"; cp "$T/agent.gguf" "$T/hm/second.gguf"
OUT=$(python3 "$R/tools/tooltest/console_pty.py" "$K" "$T/hm/agent.gguf" "$T/p1" "$T/ptyhome" "$R/tools/traintest/mock_teacher_prog.py" 2>&1)
printf '%s\n' "$OUT" | grep -v '^pty:'
PN=$(printf '%s\n' "$OUT" | grep -c '^\(ok\|FAIL\) '); PF=$(printf '%s\n' "$OUT" | grep -c '^FAIL ')
N=$((N+PN)); F=$((F+PF))
[ "$PN" -gt 0 ] || { N=$((N+1)); F=$((F+1)); echo "FAIL pty: nenhuma verificacao rodou"; printf '%s\n' "$OUT" | tail -5; }
fi


########## 8. renderizador de Markdown do console (sem modelo) ##########
B=$(printf '\033[1m'); B0=$(printf '\033[22m'); C=$(printf '\033[36m'); C0=$(printf '\033[39m'); Z=$(printf '\033[0m'); D=$(printf '\033[2m')
md() { printf -- "$1" | "$K" mdtest $2; }
eq "markdown: negrito" "$(md 'Use **forte** aqui.\n')" "Use ${B}forte${B0} aqui."
eq "markdown: codigo em linha" "$(md 'Rode `ls -l` agora.\n')" "Rode ${C}ls -l${C0} agora."
eq "markdown: titulo vira linha em negrito" "$(md '## Titulo\ntexto\n')" "${B}Titulo${Z}
texto"
eq "markdown: lista" "$(md '- um\n* dois\n  - tres\n')" "$(printf '\342\200\242 um\n\342\200\242 dois\n  \342\200\242 tres')"
eq "markdown: asterisco solto e conta" "$(md '2 * 3 = 6\n')" "2 * 3 = 6"
eq "markdown: bloco de codigo (a linguagem some, as linhas ficam apagadas)" "$(md 'a\n```c\nint x;\n```\nb\n')" "a
${D}int x;
${Z}b"
eq "markdown: a chamada de ferramenta em XML some" "$(md 'Antes<tool_call>\n<function=Bash>\n<parameter=command>\nls\n</parameter>\n</function>\n</tool_call>Depois\n')" "AntesDepois"
eq "markdown: a chamada de ferramenta do gemma some" "$(md 'Oi <|tool_call>call:Bash{command:x}<tool_call|>fim\n')" "Oi fim"
eq "markdown: um < comum passa" "$(md 'a < b e <b>x</b>\n')" "a < b e <b>x</b>"
eq "markdown: sem cores no modo plano" "$(md 'Use **forte** e `cod`.\n# T\n' plain)" "Use forte e cod.
T"
eq "markdown: negrito aberto fecha no fim da linha" "$(md '**sem fechar\nlinha\n')" "${B}sem fechar${Z}
linha"

echo
echo "$N verificacoes, $F falhas"
[ "$F" = 0 ]
