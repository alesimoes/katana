#!/bin/sh
# Testes sem modelo do sistema de extensao: documentos de instrucoes (formato brief, camadas, never/ask, nota de fim de
# pedido), katana init, skills (formato ability, plano, needs, katana skill ...), MCP (add/remove/list/test/card, confianca,
# structuredContent, isError, paginacao, mcp__, list_changed, reconexao, tempo, prompts) e os comandos do agente.
# Uso: sh tools/tooltest/extend.sh /caminho/do/katana     (nada vai para o ~/.katana do dono: HOME temporario)
K=$1
[ -x "$K" ] || { echo "uso: $0 <binario katana>"; exit 2; }
K=$(cd "$(dirname "$K")" && pwd)/$(basename "$K")
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
export KATANA_LOG="$T/agente.log" KATANA_MCP_LOG="$T/mcp.log"
export HOME="$T/home"; mkdir -p "$HOME"
unset KATANA_MCP_CONFIG KATANA_MCP_TRUST KATANA_MCP_PREFIX KATANA_INSTR_MAX KATANA_MCP_TIMEOUT KATANA_UI KATANA_CONFIRM
N=0; F=0
ok()   { N=$((N+1)); echo "ok   $1"; }
bad()  { N=$((N+1)); F=$((F+1)); echo "FAIL $1"; echo "---- saida:"; printf '%s\n' "$2" | head -25; echo "----"; }
has()  { case $2 in *"$3"*) ok "$1";; *) bad "$1 (faltou: $3)" "$2";; esac; }
hasnt(){ case $2 in *"$3"*) bad "$1 (nao devia ter: $3)" "$2";; *) ok "$1";; esac; }
eq()   { [ "$2" = "$3" ] && ok "$1" || bad "$1 (esperado: $3)" "$2"; }
sysp() { "$K" agentsys 2>/dev/null; }
PY3=$(command -v python3) || { echo "sem python3: testes do MCP e do agente nao rodam"; PY3=""; }

########## instrucoes em camadas ##########
P="$T/proj"; mkdir -p "$P/.git" "$P/sub/deep" "$P/gen" "$HOME/.katana"
printf '# me: personal rules\nrules:\n  - Answer in short sentences.\nnever:\n  - never print secrets `cat *.pem`\n' > "$HOME/.katana/KATANA.md"
cat > "$P/KATANA.md" <<'EOF'
# proj: a small test project
build: sh build.sh
test: sh test.sh
rules:
  - Keep functions short.
never:
  - never force push `git push*--force*`
  - never edit generated files `gen/*`
ask:
  - ask before deleting folders `rm -r*`
done: `sh test.sh` passes
EOF
printf 'rules:\n  - Local override line.\ntest: sh test.sh --quick\n' > "$P/KATANA.local.md"
cd "$P"
S=$(sysp)
has "camada global no prompt" "$S" "Answer in short sentences."
has "camada do projeto no prompt" "$S" "Keep functions short."
has "camada local no prompt" "$S" "Local override line."
case $S in *"Answer in short"*"Keep functions short"*"Local override"*) ok "ordem: global, projeto, local";; *) bad "ordem das camadas" "$S";; esac
# teto total: o mais especifico tem prioridade
S=$(KATANA_INSTR_MAX=330 "$K" agentsys 2>/dev/null)
has "teto: a camada local sobrevive" "$S" "Local override line."
has "teto: o que passou do limite aponta o arquivo" "$S" "the file continues"
# AGENTS.md no lugar de KATANA.md
mkdir -p "$T/p2/.git"; printf '# p2: other\nrules:\n  - From agents file.\n' > "$T/p2/AGENTS.md"
S=$(cd "$T/p2" && sysp); has "AGENTS.md continua valendo" "$S" "From agents file."
# arquivo em prosa comum: passa como texto
mkdir -p "$T/p3/.git"; printf 'This project is written in prose.\nPlease be careful with it.\n' > "$T/p3/KATANA.md"
S=$(cd "$T/p3" && sysp); has "prosa comum vai como texto" "$S" "This project is written in prose."
# @include foi trocado por docs: a linha fica como esta
printf '# p4: x\nrules:\n  - A rule.\n@other.md\n' > "$T/p3/KATANA.md"; printf 'INCLUDED TEXT\n' > "$T/p3/other.md"
S=$(cd "$T/p3" && sysp); hasnt "sem @include" "$S" "INCLUDED TEXT"

########## KATANA.md de subpasta entregue uma vez ##########
printf '# sub: rules for sub\nrules:\n  - Sub folder rule.\n' > "$P/sub/KATANA.md"; echo hello > "$P/sub/a.txt"; echo top > "$P/x.txt"
R=$("$K" tool Read '{"file_path":"sub/a.txt"}' Read '{"file_path":"sub/a.txt"}' Read '{"file_path":"x.txt"}' 2>&1)
eq "subpasta: entregue uma vez" "$(printf '%s\n' "$R" | grep -c 'Sub folder rule')" "1"
has "subpasta: mostra o arquivo e a pasta" "$R" "Instructions for the folder"
R=$("$K" tool Read '{"file_path":"x.txt"}' 2>&1); hasnt "raiz do projeto nao e reentregue (ja esta no prompt)" "$R" "Instructions for the folder"
R=$("$K" tool Write '{"file_path":"sub/new.txt","content":"x"}' 2>&1); has "Write numa subpasta tambem entrega" "$R" "Sub folder rule."

########## never, ask, nota de fim de pedido ##########
R=$("$K" tool Bash '{"command":"git push origin main --force"}' 2>&1)
has "never recusa Bash e cita a regra" "$R" "refused by the project rule (never): never force push"
R=$("$K" tool Bash '{"command":"echo a && git push --force origin"}' 2>&1); has "never olha cada trecho do comando" "$R" "refused by the project rule"
R=$("$K" tool Bash '{"command":"echo fine"}' 2>&1); has "comando permitido roda" "$R" "fine"
R=$("$K" tool Write '{"file_path":"gen/out.c","content":"x"}' 2>&1); has "never recusa Write pelo caminho" "$R" "never edit generated files"
R=$("$K" tool Edit '{"file_path":"gen/out.c","old_string":"a","new_string":"b"}' 2>&1); has "never recusa Edit pelo caminho" "$R" "never edit generated files"
R=$(cd "$HOME" && cd "$P" && "$K" tool Bash '{"command":"cat k.pem"}' 2>&1); has "never da camada global tambem vale" "$R" "never print secrets"
mkdir -p "$P/old"
R=$("$K" tool Bash '{"command":"rm -r old"}' 2>&1 </dev/null); has "ask sem ninguem para perguntar recusa" "$R" "needs the user's yes"
[ -d "$P/old" ] && ok "ask recusado: nada foi apagado" || bad "ask recusado" "pasta apagada"
R=$(printf '\001ANS 1\n' | KATANA_UI=1 "$K" tool Bash '{"command":"rm -r old"}' 2>&1)
has "ask com resposta sim pergunta pelo caminho de confirmacao" "$R" "ASK action"
[ ! -d "$P/old" ] && ok "ask aceito: o comando rodou" || bad "ask aceito" "$R"
mkdir -p "$P/old"
R=$(printf '\001ANS 0\n' | KATANA_UI=1 "$K" tool Bash '{"command":"rm -r old"}' 2>&1); has "ask com resposta nao recusa" "$R" "needs the user's yes"
# nota: arquivo mudou e o test: nao rodou
R=$(KATANA_BRIEF_NOTE=1 "$K" tool Write '{"file_path":"sub/z.txt","content":"x"}' Read '{"file_path":"x.txt"}' 2>&1)
has "nota de fim: arquivo mudou sem rodar o teste" "$R" "has not run in this request"
has "nota de fim: cita o comando do test: (camada local vence)" "$R" "sh test.sh --quick"
R=$(KATANA_BRIEF_NOTE=1 "$K" tool Write '{"file_path":"sub/z.txt","content":"x"}' Bash '{"command":"sh test.sh --quick"}' 2>&1)
hasnt "nota de fim: some quando o teste rodou" "$R" "end-of-request note"
R=$(KATANA_BRIEF_NOTE=1 "$K" tool Read '{"file_path":"x.txt"}' Read '{"file_path":"x.txt"}' 2>&1); hasnt "nota de fim: so se arquivos mudaram" "$R" "end-of-request note"

# conferir com o exemplo do pedido
W='{"file_path":"sub/ex.txt","content":"x"}'; rm -f sub/ex.txt
R=$(KATANA_EXAMPLE_REQ='faz um script; a saida e assim: n=5' "$K" tool Write "$W" Read '{"file_path":"x.txt"}' 2>&1); has "exemplo: lembra de conferir com o pedido" "$R" "compare the result with the request itself"; has "exemplo: traz o pedido" "$R" "a saida e assim: n=5"
rm -f sub/ex.txt; R=$(KATANA_EXAMPLE_REQ='renomeia a funcao soma' "$K" tool Write "$W" Read '{"file_path":"x.txt"}' 2>&1); hasnt "exemplo: pedido sem exemplo nao tem nota" "$R" "example note"
R=$(KATANA_EXAMPLE_REQ='a saida e assim: n=5' "$K" tool Read '{"file_path":"x.txt"}' Read '{"file_path":"x.txt"}' 2>&1); hasnt "exemplo: so se arquivos mudaram" "$R" "example note"
rm -f sub/ex.txt; R=$(KATANA_EXAMPLE_NOTE=0 KATANA_EXAMPLE_REQ='a saida e assim: n=5' "$K" tool Write "$W" Read '{"file_path":"x.txt"}' 2>&1); hasnt "exemplo: desliga por ambiente" "$R" "example note"

########## camadas: chaves de um valor, blocos, never ##########
R=$("$K" brief show 2>&1)
has "mescla: test do mais especifico" "$R" "test: sh test.sh --quick"
has "mescla: blocos somam, geral primeiro" "$R" "Answer in short sentences."
has "mescla: never do geral e do projeto" "$R" "cat *.pem"
case $R in *"Answer in short"*"Keep functions short"*"Local override"*) ok "mescla: ordem dos blocos";; *) bad "ordem na mescla" "$R";; esac

########## lint: katana brief check ##########
cd "$T"
cat > ok.md <<'EOF'
# demo: a demo project
build: sh -c true
rules:
  - Keep it short.
never:
  - never force push `git push*--force*`
EOF
R=$("$K" brief check ok.md 2>&1); rc=$?
eq "lint: arquivo valido sai com 0" "$rc" "0"; has "lint: mostra tamanho e orcamento" "$R" "characters (budget 1500)"
printf '# d: x\nfoo: bar\nrules:\n  - A\n' > b1.md; R=$("$K" brief check b1.md 2>&1); rc=$?
has "lint: chave desconhecida" "$R" "unknown key 'foo:'"; eq "lint: erro sai com 1" "$rc" "1"
printf '# d: x\nrules:\n  - %s\n' "$(printf 'x%.0s' $(seq 1 120))" > b2.md; R=$("$K" brief check b2.md 2>&1); has "lint: regra com mais de 100 caracteres" "$R" "120 characters; 100 at most"
printf '# d: x\nThis is a prose paragraph outside any key.\nrules:\n  - A\n' > b3.md; R=$("$K" brief check b3.md 2>&1); has "lint: prosa fora das chaves" "$R" "prose line outside any key"
printf '# d: x\nrules:\n  - Maybe try to keep it short.\n  - Se possivel, use testes.\n' > b4.md; R=$("$K" brief check b4.md 2>&1); has "lint: palavra de duvida (ingles)" "$R" "hedging word"
has "lint: palavra de duvida (portugues)" "$(printf '%s' "$R" | grep -c 'Se possivel')" "1"
printf '# d: x\nrules:\n  - Same line.\n  - Same line.\n' > b5.md; R=$("$K" brief check b5.md 2>&1); has "lint: regra repetida" "$R" "duplicate item"
printf '# d: x\nnever:\n  - bad pattern `a[bc`\n  - no pattern here\n' > b6.md; R=$("$K" brief check b6.md 2>&1); rc=$?
has "lint: padrao que nao compila" "$R" "does not compile"; has "lint: item sem padrao e so orientacao" "$R" "guidance only"
printf '# d: x\nbuild: programa-que-nao-existe --all\ntest: ls\n' > b7.md; R=$("$K" brief check b7.md 2>&1); has "lint: programa fora do PATH" "$R" "programa-que-nao-existe"
hasnt "lint: programa que existe passa" "$R" "'ls'"
{ printf '# d: x\nlayout:\n'; for i in 1 2 3 4 5 6 7 8 9 10 11 12 13; do printf '  d%s/  holds %s\n' $i $i; done; } > b8.md; R=$("$K" brief check b8.md 2>&1); has "lint: layout com mais de 12 linhas" "$R" "layout has more than 12 lines"
{ printf '# d: x\nstyle:\n'; for i in 1 2 3 4 5 6 7; do printf '  - Style %s.\n' $i; done; } > b9.md; R=$("$K" brief check b9.md 2>&1); has "lint: style com mais de 6 linhas" "$R" "style has more than 6 lines"
{ printf '# d: x\nrules:\n'; for i in $(seq 1 60); do printf '  - Rule number %s says to keep things tidy.\n' $i; done; } > b10.md; R=$("$K" brief check b10.md 2>&1); has "lint: acima do orcamento de 1500" "$R" "recommended budget is 1500"
printf 'Plain markdown text only.\n' > b11.md; R=$("$K" brief check b11.md 2>&1); rc=$?
has "lint: prosa comum e aceita como texto" "$R" "accepted as plain text"; eq "lint: prosa comum sai com 0" "$rc" "0"
printf '# longname: this header has far too many words in it to be a short description ok\n' > b12.md; R=$("$K" brief check b12.md 2>&1); has "lint: cabecalho com mais de 12 palavras" "$R" "12 at most"
R=$("$K" brief show ok.md 2>&1); has "show: padroes impostos" "$R" "never (refuse) \`git push*--force*\`"

########## katana init em tres projetos ##########
mkdir -p "$T/i1/src" "$T/i1/tests" "$T/i1/docs"; cd "$T/i1"
printf 'all: build\nbuild:\n\tcc -o app src/main.c\ntest:\n\t./app --test\nrun:\n\t./app\n' > Makefile
printf 'int main(void){return 0;}\n' > src/main.c; printf 'x\n' > tests/t.c; printf 'guide\n' > docs/guide.md
printf '# Demo\n\nDemo is a tiny tool that counts words in files quickly and prints a table.\n' > README.md
R=$("$K" init 2>&1); rc=$?
eq "init: projeto Make sai com 0" "$rc" "0"
has "init: cabecalho com o README" "$R" "# i1: Demo is a tiny tool that counts words in files quickly and"
has "init: build do Makefile" "$R" "build: make build"; has "init: test do Makefile" "$R" "test: make test"; has "init: run do Makefile" "$R" "run: make run"
has "init: layout com palpite" "$R" "src/  source code"; has "init: layout tests" "$R" "tests/  tests"
has "init: docs apontados" "$R" "docs/guide.md"; has "init: done a partir do test" "$R" "done: \`make test\` passes"
"$K" init >/dev/null 2>&1; rc=$?; eq "init: nao sobrescreve sem --force" "$rc" "1"
"$K" init --force >/dev/null 2>&1; rc=$?; eq "init: --force sobrescreve" "$rc" "0"
R=$("$K" brief check KATANA.md 2>&1); rc=$?; eq "init: o que escreve passa no lint" "$rc" "0"
mkdir -p "$T/i2/modules"; cd "$T/i2"
printf '{"name":"x","scripts":{"build":"tsc","test":"jest","lint":"eslint ."}}\n' > package.json; printf 'export const a=1\n' > modules/a.ts; printf 'export const b=1\n' > modules/b.ts
R=$("$K" init 2>&1); has "init: npm build" "$R" "build: npm run build"; has "init: npm test" "$R" "test: npm test"; has "init: npm lint" "$R" "lint: npm run lint"
has "init: sem README, linguagem por arquivos" "$R" "# i2: TypeScript project"; has "init: layout por tipo de arquivo" "$R" "modules/  TypeScript files"
mkdir -p "$T/i3/tests"; cd "$T/i3"
printf '[build-system]\nrequires=["setuptools"]\n[tool.pytest]\n' > pyproject.toml; printf 'print(1)\n' > main.py; printf 'x\n' > tests/test_a.py; printf '#!/bin/sh\necho b\n' > build.sh
R=$("$K" init 2>&1); has "init: pytest" "$R" "test: python3 -m pytest"; has "init: build do pyproject" "$R" "build: python3 -m build"
mkdir -p "$T/i4"; cd "$T/i4"; printf 'module x\ngo 1.21\n' > go.mod; printf 'package main\n' > main.go
R=$("$K" init 2>&1); has "init: Go" "$R" "test: go test ./..."
mkdir -p "$T/i5"; cd "$T/i5"; printf 'package x\n' > Cargo.toml; R=$("$K" init 2>&1); has "init: Cargo" "$R" "build: cargo build"

########## skills ##########
mkdir -p "$T/sk"; cd "$T/sk"
R=$("$K" skill new deploy-app --description "Ship the app to the staging server" 2>&1); has "skill new: cria" "$R" "created"
[ -f .katana/skills/deploy-app/SKILL.md ] && ok "skill new: arquivo na pasta do projeto" || bad "skill new" "$R"
R=$("$K" skill new deploy-app 2>&1); has "skill new: nao sobrescreve" "$R" "already exists"
R=$("$K" skill new "bad name" 2>&1); has "skill new: valida o nome" "$R" "invalid skill name"
R=$("$K" skill new gl --global 2>&1); [ -f "$HOME/.katana/skills/gl/SKILL.md" ] && ok "skill new --global" || bad "skill new --global" "$R"
R=$("$K" skill check deploy-app 2>&1); rc=$?; has "skill check: modelo novo tem needs/steps validos" "$R" "checked"
eq "skill check: modelo novo passa" "$rc" "0"
R=$("$K" skill check gl 2>&1); rc=$?; has "skill check: when com o texto do modelo e erro" "$R" "template text"; eq "skill check: erro sai com 1" "$rc" "1"
R=$("$K" skill list 2>&1); has "skill list: projeto" "$R" "deploy-app"; has "skill list: origem" "$R" "project"; has "skill list: global" "$R" "global"
R=$("$K" skill show deploy-app 2>&1); has "skill show" "$R" "steps:"
R=$("$K" skill path 2>&1); has "skill path" "$R" ".katana/skills"
# skill escrita a mao no formato ability
mkdir -p .katana/skills/rel/scripts
cat > .katana/skills/rel/SKILL.md <<'EOF'
---
name: rel
when: Cut a release of a named version
needs: ls, nosuchprogram-zz, version
args: version
---
steps:
1. Check the working tree is clean => git status prints nothing
2. Tag the release as v$version => the tag exists
3. Write the notes for $version => notes file written
done: the tag v$version exists
report: the version, the tag and the notes file
fail: stop and say which check failed
EOF
printf 'echo hi\n' > .katana/skills/rel/scripts/run.sh
R=$("$K" tool Skill '{"skill":"rel","args":"version=1.2"}' TaskList '{}' 2>&1)
has "skill: needs ausente vai no topo" "$R" "MISSING: nosuchprogram-zz"
hasnt "skill: o que existe nao e reportado" "$R" "MISSING: ls"
has "skill: \$arg nos passos" "$R" "Tag the release as v1.2"
has "skill: \$arg no done" "$R" "done: the tag v1.2 exists"
has "skill: arquivos extras com caminho" "$R" "scripts/run.sh"
hasnt "skill: com needs ausente nao escreve o plano" "$R" "Plan written from the skill"
sed -i 's/needs: .*/needs: ls, version/' .katana/skills/rel/SKILL.md
R=$("$K" tool Skill '{"skill":"rel","args":"version=1.2"}' Skill '{"skill":"rel","args":"version=1.3"}' 2>&1)
eq "skill: plano escrito uma vez" "$(printf '%s\n' "$R" | grep -c 'Plan written from the skill: do step 1')" "1"
has "skill: o passo traz a verificacao" "$R" "1. Check the working tree is clean => git status prints nothing"
R=$("$K" tool Skill '{"skill":"rel"}' 2>&1); has "skill: needs de entrada sem valor e reportado" "$R" "MISSING: input version"
mkdir -p .katana/skills/free; { printf -- '---\nname: free\ndescription: A free text skill\n---\nJust do the thing carefully.\n' > .katana/skills/free/SKILL.md; }
R=$("$K" tool Skill '{"skill":"free"}' 2>&1); has "skill em Markdown livre carrega como texto" "$R" "Just do the thing carefully."; hasnt "skill livre nao escreve plano" "$R" "Plan written"
R=$("$K" skill check free 2>&1); has "skill check: texto livre e aviso" "$R" "no 'steps:' section"
# lint
mkdir -p .katana/skills/lint1; { printf -- '---\nname: lint1\n---\nsteps:\n'; for i in 1 2 3 4 5 6 7 8 9 10; do printf '%s. Run check %s\n' $i $i; done; printf '2. x\n'; } > .katana/skills/lint1/SKILL.md
R=$("$K" skill check lint1 2>&1); has "skill check: sem when" "$R" "missing 'when:'"; has "skill check: mais de 9 passos" "$R" "9 at most"; has "skill check: sem done" "$R" "no 'done:'"
mkdir -p .katana/skills/lint2; printf -- '---\nname: lint2\nwhen: x\n---\nsteps:\n1. Running the tests\n2. The tests pass\ndone: ok\n' > .katana/skills/lint2/SKILL.md
R=$("$K" skill check lint2 2>&1); has "skill check: passo que nao e imperativo" "$R" "imperative verb"
# linha das skills no prompt
S=$(sysp); has "prompt: linha das skills" "$S" "- Skills (load one with the Skill tool): "
has "prompt: name: when" "$S" "deploy-app: Ship the app to the staging server"
case $S in *"deploy-app"*"gl:"*) ok "prompt: projeto antes do global";; *) bad "ordem projeto/global" "$S";; esac
for i in 01 02 03 04 05 06 07 08 09 10 11 12; do mkdir -p ".katana/skills/m$i"; printf -- '---\nname: m%s\nwhen: %s\n---\nsteps:\n1. Do it\n' $i "$(printf 'long description %.0s' 1 2 3 4)" > ".katana/skills/m$i/SKILL.md"; done
S=$(sysp); L=$(printf '%s\n' "$S" | grep '^- Skills (load')
has "prompt: mais que 10 diz quantas ha" "$L" "more: call Skill to list them"
[ ${#L} -le 760 ] && ok "prompt: linha das skills limitada (${#L} caracteres)" || bad "linha das skills grande: ${#L}" "$L"
has "prompt: descricao cortada em 60" "$L" "..."
mkdir -p "$T/sk0"; cd "$T/sk0"; S=$(HOME="$T/nohome" "$K" agentsys 2>/dev/null); hasnt "prompt: sem skills nao ha linha" "$S" "- Skills (load"
cd "$T/sk"; "$K" skill remove m01 >/dev/null 2>&1; [ ! -d .katana/skills/m01 ] && ok "skill remove" || bad "skill remove" ""
R=$("$K" skill remove nao-existe 2>&1); has "skill remove: inexistente" "$R" "no skill named"

########## MCP ##########
if [ -n "$PY3" ]; then
SRV="$HERE/extend_server.py"
cd "$T/work" 2>/dev/null || { mkdir -p "$T/work/.git"; cd "$T/work"; }
R=$("$K" mcp list 2>&1); has "mcp list sem servidores" "$R" "No MCP servers configured"
R=$("$K" mcp add main --env TOKEN=abc -- "$PY3" "$SRV" main 2>&1); has "mcp add stdio" "$R" "added server 'main'"
has "mcp add: cria o arquivo de uso vazio" "$R" "usage file"; [ -f "$HOME/.katana/mcp/main.md" ] && ok "mcp add: arquivo mcp/<servidor>.md" || bad "usage file" "$R"
[ "$(stat -c %a "$HOME/.katana/mcp.json")" = "600" ] && ok "mcp add: arquivo so do dono (0600)" || bad "permissao do mcp.json" "$(stat -c %a "$HOME/.katana/mcp.json")"
R=$("$K" mcp add web --url http://127.0.0.1:9/mcp --header 'Authorization: Bearer x' 2>&1); has "mcp add http" "$R" "added server 'web'"
has "mcp add: o json tem as duas entradas" "$(cat "$HOME/.katana/mcp.json")" '"web"'
R=$("$K" mcp add "bad name" -- ls 2>&1); has "mcp add: valida o nome" "$R" "invalid name"
R=$("$K" mcp add main -- "$PY3" "$SRV" main 2>&1); has "mcp add: troca o existente" "$R" "replaced server 'main'"
R=$("$K" mcp list 2>&1); has "mcp list: servidor ok" "$R" "main [stdio] ok"; has "mcp list: origem" "$R" "from your file $HOME/.katana/mcp.json"
has "mcp list: paginacao (pagina 2)" "$R" "main__die"
has "mcp list: http fora do ar" "$R" "web [http] unavailable"
R=$("$K" mcp test main 2>&1); rc=$?; has "mcp test: conecta e mede" "$R" "connected in"; has "mcp test: conta ferramentas" "$R" "tools: 7"; has "mcp test: prompts" "$R" "prompts: 1"; eq "mcp test: sai com 0" "$rc" "0"
R=$("$K" mcp test web 2>&1); rc=$?; has "mcp test: falha em palavras simples" "$R" "FAILED"; eq "mcp test: falha sai com 1" "$rc" "1"
"$K" mcp add gone -- /nonexistent/prog >/dev/null 2>&1; R=$("$K" mcp test gone 2>&1); has "mcp test: comando inexistente" "$R" "was not found"
# KATANA_SESSION_ID: o servidor stdio recebe o id da sessao (sem arquivo: data-hora-pid do processo)
printf '#!/bin/sh\nprintf "%%s" "$KATANA_SESSION_ID" > "%s/sid.out"\nexec "%s" "%s" main\n' "$T" "$PY3" "$SRV" > "$T/sidsrv.sh"
"$K" mcp add sidsrv -- sh "$T/sidsrv.sh" >/dev/null 2>&1; rm -f "$T/sid.out"
R=$("$K" mcp test sidsrv 2>&1); has "KATANA_SESSION_ID: servidor conecta" "$R" "connected in"
SID=$(cat "$T/sid.out" 2>/dev/null); case $SID in [0-9]*-[0-9]*-[0-9]*) ok "KATANA_SESSION_ID sem sessao: data-hora-pid ($SID)";; *) bad "KATANA_SESSION_ID sem sessao" "$SID";; esac
R=$("$K" mcp test zzz 2>&1); has "mcp test: servidor desconhecido" "$R" "no server 'zzz'"
"$K" mcp remove gone >/dev/null 2>&1; "$K" mcp remove web >/dev/null 2>&1
R=$("$K" mcp remove web 2>&1); has "mcp remove: inexistente" "$R" "no server 'web'"
hasnt "mcp remove: saiu do arquivo" "$(cat "$HOME/.katana/mcp.json")" '"web"'
# chamadas
R=$("$K" tool main__echo '{"text":"hello"}' 2>&1); has "chamada de ferramenta" "$R" "hello"
R=$("$K" tool main__sum '{"a":2,"b":3}' 2>&1); has "structuredContent depois do texto" "$R" '[structured] {"sum":5,"args":{"a":2,"b":3}}'; has "structuredContent: o texto vem antes" "$R" "sum computed"
R=$("$K" tool main__fail '{}' 2>&1); has "isError vira erro" "$R" "error: boom happened"
R=$("$K" tool main__pic '{}' 2>&1); has "imagem descrita em uma linha" "$R" "[image image/png, 11 bytes: binary content not shown]"; has "resource_link descrito" "$R" "resource link file:///x.txt (x)"
R=$("$K" tool mcp__main__echo '{"text":"viaprefixo"}' 2>&1); has "aceita mcp__servidor__ferramenta" "$R" "viaprefixo"
R=$("$K" tool main__nope '{}' 2>&1); has "ferramenta desconhecida lista as do servidor" "$R" "Its tools: echo"
R=$(printf 'main__add_tool {}\nmain__new_tool {}\n' | "$K" toolrun 2>&1); has "list_changed: aviso no resultado" "$R" "tool list of this MCP server changed"; has "list_changed: a ferramenta nova funciona" "$R" "new ok"
R=$(printf 'main__echo {"text":"antes"}\nmain__die {}\nmain__echo {"text":"depois"}\n' | "$K" toolrun 2>&1)
has "servidor que morreu: erro claro" "$R" "error: MCP call failed"; has "servidor que morreu: reconecta" "$R" "depois"
R=$(KATANA_MCP_TIMEOUT=1 "$K" tool main__slow '{}' 2>&1); has "tempo esgotado em palavras claras" "$R" "got no answer in 1 s"
# cartao
R=$("$K" mcp card main 2>&1)
has "cartao: primeira linha com o instructions do servidor" "$R" "mcp main: Test server with arithmetic and echo tools"
has "cartao: obrigatorios primeiro, tipos, opcional, enum, vetor" "$R" "main__sum(a:number, b:number, mode?:(fast|slow), tags?:string[], flag?:boolean)"
has "cartao: arg texto sem sufixo" "$R" "main__echo(text) - Echo the text back"
has "cartao: sem argumentos" "$R" "main__fail()"
has "cartao: tamanho" "$R" "characters]"
printf 'what: Arithmetic helper\nfirst: call echo to check the link\norder: sum before echo\navoid: calling die\n' > "$HOME/.katana/mcp/main.md"
R=$("$K" mcp card main 2>&1)
has "cartao: what do arquivo de uso" "$R" "mcp main: Arithmetic helper"; has "cartao: first" "$R" "first: call echo to check the link"
has "cartao: order depois de first" "$(printf '%s\n' "$R" | grep -A1 '^first:')" "order: sum before echo"; has "cartao: avoid" "$R" "avoid: calling die"
# prompt de sistema: antes e depois
cd "$T/work"
rm -f "$HOME/.katana/mcp.json"; S0=$(sysp | tail -1)
"$K" mcp add main -- "$PY3" "$SRV" main >/dev/null 2>&1
S1=$(sysp); has "prompt: cartao do servidor" "$S1" "# MCP servers"; has "prompt: assinatura no cartao" "$S1" "main__sum(a:number"
M1=$(printf '%s\n' "$S1" | sed -n '/^# MCP servers/,/^# Environment/p'); hasnt "prompt: sem esquema JSON das ferramentas MCP" "$M1" '"properties"'
echo "tamanho do prompt: sem MCP $S0; com o servidor de teste: $(printf '%s\n' "$S1" | tail -1)"
# prefixo
"$K" mcp add pref --prefix mcp__ -- "$PY3" "$SRV" pref >/dev/null 2>&1
R=$("$K" mcp card pref 2>&1); has "prefixo por servidor na declaracao" "$R" "mcp__pref__echo(text)"
R=$("$K" tool mcp__pref__echo '{"text":"p"}' 2>&1); has "chamada com o prefixo declarado" "$R" "p"
R=$(KATANA_MCP_PREFIX=mcp__ "$K" mcp card main 2>&1); has "prefixo global por ambiente" "$R" "mcp__main__echo(text)"
R=$("$K" tool ToolSearch '{"query":"select:mcp__main__sum"}' 2>&1); has "ToolSearch aceita mcp__ e devolve o esquema" "$R" '"name": "main__sum"'
# muitos servidores: cartoes acima do limite viram so nomes
rm -f "$HOME/.katana/mcp.json"
for i in 1 2 3 4 5 6; do "$K" mcp add "srv$i" -- "$PY3" "$SRV" "m$i" >/dev/null 2>&1; done
S2=$(sysp); has "muitos servidores: cartao reduzido a nomes" "$S2" "srv1__echo, srv1__sum"
L2=$(printf '%s\n' "$S2" | tail -1); echo "tamanho do prompt com 6 servidores (reduzido): $L2"
# projeto: confianca
rm -f "$HOME/.katana/mcp.json"; rm -f "$HOME/.katana/mcp-trust"
mkdir -p "$T/pj/.git"; cd "$T/pj"
"$K" mcp add psrv --project -- "$PY3" "$SRV" proj >/dev/null 2>&1
[ -f .katana/mcp.json ] && ok "mcp add --project grava no projeto" || bad "mcp add --project" ""
R=$("$K" mcp list 2>&1); has "projeto: arquivo escrito por mcp add ja e confiavel" "$R" "psrv [stdio] ok"; has "projeto: mostra o arquivo de origem" "$R" "project file"
printf '{"mcpServers":{"evil":{"command":"%s","args":["%s","evil"]}}}\n' "$PY3" "$SRV" > .katana/mcp.json
R=$("$K" mcp list 2>&1 </dev/null); has "projeto nao confiavel: pulado sem terminal" "$R" "not trusted yet"; hasnt "projeto nao confiavel: nao inicia" "$R" "evil [stdio]"
R=$(printf '\001ANS 0\n' | KATANA_UI=1 "$K" mcp list 2>&1); has "confianca: pergunta pelo caminho de confirmacao" "$R" "ASK action"; hasnt "confianca negada: nao inicia" "$R" "evil [stdio]"
R=$(printf '\001ANS 1\n' | KATANA_UI=1 "$K" mcp list 2>&1); has "confianca aceita: inicia" "$R" "evil [stdio] ok"
R=$("$K" mcp list 2>&1 </dev/null); has "confianca lembrada por caminho e hash" "$R" "evil [stdio] ok"
printf '{"mcpServers":{"evil2":{"command":"%s","args":["%s","evil"]}}}\n' "$PY3" "$SRV" > .katana/mcp.json
R=$("$K" mcp list 2>&1 </dev/null); has "arquivo alterado: a confianca anterior nao vale" "$R" "not trusted yet"
R=$(KATANA_MCP_TRUST=1 "$K" mcp list 2>&1); has "KATANA_MCP_TRUST=1 confia sem perguntar" "$R" "evil2 [stdio] ok"
# o projeto vence o arquivo do usuario no mesmo nome
"$K" mcp add evil2 -- "$PY3" "$SRV" mine >/dev/null 2>&1
R=$(KATANA_MCP_TRUST=1 "$K" mcp list 2>&1); has "conflito de nome: o projeto vence" "$R" "from project file"
fi

########## comandos do agente (modelo minusculo + professor de mentira) ##########
if [ -n "$PY3" ] && [ -f "$HERE/../tinymodel/make_tiny_gguf.py" ]; then
G="$T/agent.gguf"; $PY3 "$HERE/../tinymodel/make_tiny_gguf.py" "$G" --template chatml-xml >/dev/null 2>&1
rm -f "$HOME/.katana/mcp.json" "$HOME/.katana/mcp-trust"; mkdir -p "$T/ag/.git"; cd "$T/ag"
printf 'print(1)\n' > main.py
"$K" mcp add main -- "$PY3" "$HERE/extend_server.py" main >/dev/null 2>&1
mkdir -p .katana/skills/greet; printf -- '---\nname: greet\nwhen: Greet someone by name\nargs: who\n---\nsteps:\n1. Say hello to $who => the greeting is printed\ndone: greeted\n' > .katana/skills/greet/SKILL.md
export MOCK_LOG="$T/mock.jsonl"
run_agent() { printf '%s\n/exit\n' "$1" | KATANA_CPU=1 KATANA_TEACHER=1 KATANA_MAXTOK=24 KATANA_TEACHER_CMD="python3 $HERE/mock_cli.py" "$K" agent "$G" 2>&1 >/dev/null; }
mkdir -p "$T/ss"; rm -f "$T/sid.out"
"$K" mcp add sidsrv -- sh "$T/sidsrv.sh" >/dev/null 2>&1
printf '/exit\n' | KATANA_CPU=1 KATANA_TEACHER=1 KATANA_MAXTOK=24 KATANA_TEACHER_CMD="python3 $HERE/mock_cli.py" "$K" agent "$G" --sessao "$T/ss/2026-11-22-7.ksess" >/dev/null 2>&1
eq "KATANA_SESSION_ID com --sessao: nome do .ksess sem extensao" "$(cat "$T/sid.out" 2>/dev/null)" "2026-11-22-7"
"$K" mcp remove sidsrv >/dev/null 2>&1
R=$(run_agent '/init')
[ -f KATANA.md ] && ok "/init escreve o KATANA.md sem modelo" || bad "/init" "$R"
has "/init: o modelo recebe o pedido de melhorar" "$(cat "$MOCK_LOG" 2>/dev/null)" "Improve it by reading the project"
R=$(run_agent '/init'); has "/init nao sobrescreve" "$R" "already exists"
R=$(run_agent '/skill new cleanup Remove temp files'); [ -f .katana/skills/cleanup/SKILL.md ] && ok "/skill new cria a skill" || bad "/skill new" "$R"
has "/skill new: o modelo recebe o pedido de escrever" "$(cat "$MOCK_LOG")" "Remove temp files"
R=$(run_agent '/skill list'); has "/skill list" "$R" "greet"
: > "$MOCK_LOG"; R=$(run_agent '/greet who=Ana'); has "/<skill> argumentos: a skill vai ao modelo" "$(cat "$MOCK_LOG")" "Say hello to Ana"
has "/<skill>: avisa o usuario" "$R" "skill greet loaded"
R=$(run_agent '/mcp'); has "/mcp: estado" "$R" "main [stdio] connected, 7 tools, 1 prompts"
R=$(run_agent '/mcp reconnect main'); has "/mcp reconnect" "$R" "reconnected, 7 tools"
R=$(run_agent '/mcp reconnect zzz'); has "/mcp reconnect: desconhecido" "$R" "no server 'zzz'"
: > "$MOCK_LOG"; R=$(run_agent '/main:review a.c performance'); has "/servidor:prompt vai ao modelo como mensagem do usuario" "$(cat "$MOCK_LOG")" "Please review a.c focusing on performance"
R=$(run_agent '/main:review'); has "/servidor:prompt: argumento obrigatorio" "$R" "needs the argument 'file'"
R=$(run_agent '/main:nope'); has "/servidor:prompt: prompt desconhecido" "$R" "has 1 prompt(s): review"
R=$(run_agent '/plan'); has "comando embutido nao e tomado por skill" "$R" "plan mode"
R=$(run_agent '/zzzz'); has "comando desconhecido continua desconhecido" "$R" "unknown command"
fi

echo
[ "$F" = 0 ] && echo "OK $N checks" || echo "FAILED $F of $N checks"
exit $F
