#!/bin/sh
# Testa o terminal do Bash e a ferramenta SendInput sem modelo: sh tools/tooltest/sendinput.sh ./katana
# Cada caso roda Bash e SendInput no mesmo processo (katana tool A '<json>' B '<json>'), porque a tabela de tarefas vive no
# processo; ao sair o katana encerra as tarefas. Sem terminal de controle nem KATANA_UI, uma pergunta de segredo e cancelada.
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
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
has "em segundo plano avisa do SendInput" "answer it with SendInput"
has "SendInput entrega a linha" "got:valor123"
has "SendInput informa os bytes" "Typed 8 bytes into task 1 followed by a newline"
has "SendInput ve o fim da tarefa" "The task has exited with code 0"

# primeiro plano: a pergunta nao espera o timeout; vira tarefa e o SendInput responde
run Bash '{"command":"printf \"Name: \"; read -r n; echo hi:$n","timeout":60000}' SendInput '{"task_id":"1","text":"ana"}'
has "pergunta em primeiro plano" '[waiting for input: "Name:", task 1;'
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
has "cat espera entrada" '[waiting for input: "", task 1;'
has "eof termina o cat" "done"

# segredo sem interface nem terminal: cancelado, o modelo so ve a marca
run Bash '{"command":"printf \"Password: \"; read -rs p || echo cancelled; echo len:${#p}"}'
has "segredo cancelado sem terminal" "[secret cancelled]"
hasnt "segredo nao vira pergunta ao modelo" "[waiting for input"

# segredo pela interface (KATANA_UI): marcador SECRET e resposta na linha SECRET <id><TAB><texto>
OUT=$(printf '\001SECRET 1\tsenha-de-teste\n' | KATANA_UI=1 "$K" tool Bash '{"command":"printf \"Password: \"; read -rs p; echo; echo len:${#p}"}' 2>/dev/null)
has "marcador SECRET para a interface" "SECRET 1 Password:"
has "segredo chega ao comando" "len:14"
has "modelo ve so a marca" "[secret entered by the user]"
hasnt "segredo fora do resultado" "senha-de-teste"
OUT=$(cat "$KATANA_LOG" "$HOME"/.katana/jobs/* 2>/dev/null); hasnt "segredo fora do log e dos arquivos de saida" "senha-de-teste"

# SendInput numa pergunta de segredo: o texto do modelo e descartado
run Bash '{"command":"printf \"Password: \"; read -rs p; echo; echo got:$p","run_in_background":true}' Bash '{"command":"sleep 1"}' SendInput '{"task_id":"1","text":"modeltext"}'
has "SendInput recusa segredo" "it was asked from the user and your text was discarded"
hasnt "texto do modelo nao entra" "got:modeltext"

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
