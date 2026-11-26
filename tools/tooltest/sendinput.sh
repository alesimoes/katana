#!/bin/sh
# Testa o terminal do Bash e a ferramenta SendInput sem modelo: sh tools/tooltest/sendinput.sh ./katana
# Cada caso roda Bash e SendInput no mesmo processo (katana tool A '<json>' B '<json>'), porque a tabela de tarefas vive no
# processo; ao sair o katana encerra as tarefas. Sem terminal de controle nem KATANA_UI, uma pergunta de segredo e cancelada.
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
D=$(cd "$(dirname "$0")" && pwd)
[ -x "$K" ] || { echo "uso: $0 <katana>"; exit 2; }
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
export HOME="$T/home"; mkdir -p "$HOME" "$T/w"; cd "$T/w" || exit 2
export KATANA_LOG="$T/agente.log"     # o log das chamadas destes testes nao vai para o ~/.katana/agente.log do dono
unset KATANA_CONFIRM KATANA_CMD_TIMEOUT KATANA_UI
N=0; F=0
ok() { N=$((N+1)); echo "ok   $1"; }
bad() { N=$((N+1)); F=$((F+1)); echo "FAIL $1"; printf '%s\n' "$OUT" | head -20 | sed 's/^/     | /'; }
has() { case "$OUT" in *"$2"*) ok "$1";; *) bad "$1 (esperava: $2)";; esac; }
hasnt() { case "$OUT" in *"$2"*) bad "$1 (nao esperava: $2)";; *) ok "$1";; esac; }
run() { OUT=$("$K" tool "$@" 2>/dev/null </dev/null); }

# segundo plano: uma linha lida do terminal e devolvida na saida
run Bash '{"command":"echo prompt:; read -r x; echo got:$x","run_in_background":true}' SendInput '{"task_id":"1","text":"valor123"}'
has "em segundo plano avisa do SendInput" "answer with SendInput if you know the value"
has "SendInput entrega a linha" "got:valor123"
has "SendInput informa os bytes" "Typed 8 bytes into task 1 followed by a newline"
has "SendInput ve o fim da tarefa" "The task has exited with code 0"

# primeiro plano: a pergunta nao espera o timeout; vira tarefa e o SendInput responde
run Bash '{"command":"printf \"Name: \"; read -r n; echo hi:$n","timeout":60000}' SendInput '{"task_id":"1","text":"ana"}'
has "pergunta em primeiro plano" '[waiting for input: "Name:", task 1]'
has "politica de resposta" 'ask_user=true'
has "resposta chega" "hi:ana"

# duas perguntas seguidas (usuario e caminho): o SendInput devolve a segunda pergunta
run Bash '{"command":"printf \"User: \"; read -r u; printf \"Path to file: \"; read -r p; echo $u@$p"}' SendInput '{"task_id":"1","text":"ana"}' SendInput '{"task_id":"1","text":"/tmp/x"}'
has "segunda pergunta pelo SendInput" '[waiting for input: "Path to file:"'
has "segunda resposta" "ana@/tmp/x"

# newline=false: nada e lido ate a quebra; a segunda chamada completa a linha
run Bash '{"command":"read -r x; echo got:$x","run_in_background":true}' SendInput '{"task_id":"1","text":"ab","newline":false}' SendInput '{"task_id":"1","text":"cd"}'
has "newline false e depois o resto" "got:abcd"

# programa que le o terminal sem pergunta (cat): fim da entrada com eof
run Bash '{"command":"cat; echo done"}' SendInput '{"task_id":"1","text":"linha","eof":true}'
has "cat espera entrada" '[waiting for input: "", task 1]'
has "eof termina o cat" "done"

# segredo sem interface nem terminal: cancelado, o modelo so ve a marca
run Bash '{"command":"printf \"Password: \"; read -rs p || echo cancelled; echo len:${#p}"}'
has "segredo cancelado sem terminal" "[secret cancelled]"
hasnt "segredo nao vira pergunta ao modelo" "[waiting for input"

# segredo pela interface (KATANA_UI): marcador ASKINPUT <id> 1 e resposta na linha ASKINPUT <id><TAB><texto>
OUT=$(printf '\001ASKINPUT 1\tsenha-de-teste\n' | KATANA_UI=1 "$K" tool Bash '{"command":"printf \"Password: \"; read -rs p; echo; echo len:${#p}"}' 2>/dev/null)
has "marcador ASKINPUT mascarado para a interface" "ASKINPUT 1 1 Password:"
has "segredo chega ao comando" "len:14"
has "modelo ve so a marca" "[secret entered by the user]"
hasnt "segredo fora do resultado" "senha-de-teste"
OUT=$(cat "$KATANA_LOG" "$HOME"/.katana/jobs/* 2>/dev/null); hasnt "segredo fora do log e dos arquivos de saida" "senha-de-teste"

# SendInput numa pergunta de segredo: o texto do modelo e descartado
run Bash '{"command":"printf \"Password: \"; read -rs p; echo; echo got:$p","run_in_background":true}' Bash '{"command":"sleep 1"}' SendInput '{"task_id":"1","text":"modeltext"}'
has "SendInput recusa segredo" "your text was discarded"
hasnt "texto do modelo nao entra" "got:modeltext"

# programa de varias threads (principal em sigsuspend, outras em futex/poll/epoll) que pergunta um caminho sem quebra de
# linha e le a resposta depois de um epoll: detectado pelo texto em poucos segundos, com dicas achadas no disco
MT="$T/mtprompt"
cc -O1 -pthread "$D/mtprompt.c" -o "$MT" 2>/dev/null || { echo "FAIL nao compilou mtprompt.c"; exit 1; }
mkdir -p "$HOME/Downloads" && head -c 4096 /dev/zero > "$HOME/Downloads/Tool_16.xip" && touch "$HOME/Downloads/notes.txt"
S=$(date +%s)
run Bash "{\"command\":\"$MT\",\"timeout\":60000}" SendInput '{"task_id":"1","text":"/x/Tool.xip"}'
E=$(( $(date +%s) - S ))
has "varias threads: pergunta detectada" '[waiting for input: "Path to Tool.xip:", task 1]'
[ "$E" -le 8 ] && ok "varias threads: detectado em ${E}s (sem esperar o timeout)" || bad "varias threads: levou ${E}s"
has "dica de caminho: arquivo .xip em ~/Downloads" "$HOME/Downloads/Tool_16.xip\" (file found on disk, 4 KB"
hasnt "dica de caminho: outra extensao fica fora" "notes.txt"
has "contexto antes da pergunta na saida" "enter the path to the downloaded Tool.xip."
has "a resposta chega a thread leitora" "got path: /x/Tool.xip"
# em segundo plano: a nota vai no resultado da ferramenta seguinte, com o contexto de varias linhas
run Bash "{\"command\":\"$MT\",\"run_in_background\":true}" Bash '{"command":"sleep 4"}' TaskStop '{"task_id":"1"}'
has "segundo plano: pergunta detectada" '[background task 1 ('
has "segundo plano: contexto de varias linhas" "[prompt context (last lines of the output):
Logged in as test-user."
has "segundo plano: dica" "Tool_16.xip"
# sem falso positivo: comandos que so esperam o tempo passar (sleep; texto terminado em ':' sem quebra de linha e sleep)
run Bash '{"command":"sleep 4; echo fim"}'; hasnt "sleep 4 nao e pergunta" "[waiting for input"
run Bash '{"command":"printf Carregando:; sleep 5; echo fim"}'; hasnt "Carregando: e sleep nao e pergunta" "[waiting for input"
run Bash '{"command":"for i in 1 2 3 4 5; do printf Carregando:; sleep 1; done; echo"}'; hasnt "Carregando: repetido nao e pergunta" "[waiting for input"

# dicas de usuario (nome do sistema e git config user.email) e de sim/nao (o padrao entre colchetes primeiro)
printf '[user]\n\temail = ana@example.org\n' > "$HOME/.gitconfig"
run Bash '{"command":"printf \"Username: \"; read -r u; echo u:$u","timeout":30000}' TaskStop '{"task_id":"1"}'
has "dica de usuario: nome do sistema" "\"$(id -un)\" (user name on this computer)"
has "dica de usuario: e-mail do git" '"ana@example.org" (git config user.email)'
run Bash '{"command":"printf \"Continue? [y/N] \"; read -r a; echo a:$a","timeout":30000}' TaskStop '{"task_id":"1"}'
has "dica de sim/nao: padrao primeiro" '"n" (default shown in brackets); "y"'
run Bash '{"command":"printf \"Password: \"; read -rs p","timeout":30000}'
hasnt "segredo sem dicas" "[hints"

# ask_user: o modelo nao sabe; sem interface nem terminal a pergunta e cancelada (Ctrl+C no programa)
run Bash "{\"command\":\"$MT\",\"timeout\":60000}" SendInput '{"task_id":"1","ask_user":true,"ask":"Where is Tool.xip?"}'
has "ask_user sem interface: cancelado" "The user cancelled the question of task 1; Ctrl+C was sent to the task."
# ask_user na interface: marcador ASKINPUT nao mascarado com pergunta, contexto e dicas; a resposta volta ao modelo
OUT=$(printf '\001ASKINPUT 1\t/home/ana/Tool.xip\n' | KATANA_UI=1 "$K" tool Bash "{\"command\":\"$MT\",\"timeout\":60000}" SendInput '{"task_id":"1","ask_user":true,"ask":"Where is Tool.xip?"}' 2>/dev/null)
has "marcador ASKINPUT nao mascarado" "$(printf '\001')ASKINPUT 1 0 Path to Tool.xip:	Where is Tool.xip?	Logged in as test-user.\\nPlease download"
has "marcador leva as dicas" "$HOME/Downloads/Tool_16.xip"
has "resposta do usuario volta ao modelo" '[user answered: "/home/ana/Tool.xip"]'
has "resposta do usuario chega ao programa" "got path: /home/ana/Tool.xip"
OUT=$(printf '\001ASKINPUTCANCEL 1\n' | KATANA_UI=1 "$K" tool Bash "{\"command\":\"$MT\",\"timeout\":60000}" SendInput '{"task_id":"1","ask_user":true}' 2>/dev/null)
has "cancelar na interface" "The user cancelled the question of task 1"
has "cancelar encerra o programa (Ctrl+C)" "The task has exited with code 130"

# a mesma pergunta volta depois da resposta do modelo: aviso na primeira vez; na segunda o katana pergunta ao usuario
LOOP='{"command":"while :; do printf \"Path to file: \"; read -r p; [ \"$p\" = good ] && break; echo \"not found: $p\"; done; echo ok:$p","timeout":30000}'
OUT=$(printf '\001ASKINPUT 1\tgood\n' | KATANA_UI=1 "$K" tool Bash "$LOOP" SendInput '{"task_id":"1","text":"bad1"}' SendInput '{"task_id":"1","text":"bad2"}' 2>/dev/null)
has "reapresentacao: aviso ao modelo" "the same question came back after your answer"
has "reapresentacao: na segunda o usuario responde" "the same question came back twice after your answers, so katana asked the user"
has "reapresentacao: resposta do usuario" '[user answered: "good"]'
has "reapresentacao: o programa aceitou" "ok:good"

# sequencias de escape fora da saida
run Bash '{"command":"printf \"\\033[31mred\\033[0m \\033]0;title\\007ok\\r\\n\""}'
has "texto mantido" "red ok"
hasnt "sem ESC" "$(printf '\033')"

# PATH com as pastas usuais do usuario
mkdir -p "$HOME/.local/bin" && printf '#!/bin/sh\necho tool-found\n' > "$HOME/.local/bin/mytool" && chmod +x "$HOME/.local/bin/mytool"
run Bash '{"command":"mytool"}'; has "programa em ~/.local/bin" "tool-found"

# erros
run SendInput '{"task_id":"9","text":"x"}'; has "tarefa inexistente" "error: there is no task 9"
run SendInput '{"task_id":"1"}'; has "sem text" "error: missing text"
run Bash '{"command":"true","run_in_background":true}' Bash '{"command":"sleep 1"}' SendInput '{"task_id":"1","text":"x"}'
has "tarefa ja terminou" "error: task 1 is not running"
BIG=$(head -c 5000 /dev/zero | tr '\0' a)
run Bash '{"command":"read -r x","run_in_background":true}' SendInput "{\"task_id\":\"1\",\"text\":\"$BIG\"}"
has "texto grande recusado" "error: text is too long"
# TaskStop solta o terminal
run Bash '{"command":"read -r x; echo never","run_in_background":true}' TaskStop '{"task_id":"1"}' SendInput '{"task_id":"1","text":"x"}'
has "depois do TaskStop" "error: task 1 is not running"

echo "$N verificacoes, $F falhas"
[ "$F" = 0 ]
