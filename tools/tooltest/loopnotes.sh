#!/bin/sh
# Testa as notas que o laco do agente anexa aos resultados e a decisao do fim do pedido, sem modelo:
# `katana toolrun notes` le linhas "<ferramenta> <json>", "USER" (pedido novo) e "END" (o modelo respondeu sem chamar nada).
# Uso: sh tools/tooltest/loopnotes.sh /caminho/do/katana
K=${1:?usage: loopnotes.sh /path/to/katana}
export KATANA_LOG=${KATANA_LOG:-$(mktemp)}     # o log das chamadas destes testes nao vai para o ~/.katana/agente.log do dono
case $K in /*) ;; *) K=$(pwd)/$K ;; esac
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
T=$(cd "$T" && pwd -P)
export HOME="$T/home"; mkdir -p "$HOME" "$T/w"; cd "$T/w" || exit 2
unset KATANA_UI KATANA_CONFIRM
N=0; F=0
has() { N=$((N+1)); case $3 in *"$2"*) echo "ok   $1" ;; *) F=$((F+1)); echo "FAIL $1"; echo "  faltou: $2"; echo "  obtido: $3" ;; esac; }
no() { N=$((N+1)); case $3 in *"$2"*) F=$((F+1)); echo "FAIL $1"; echo "  nao devia ter: $2"; echo "  obtido: $3" ;; *) echo "ok   $1" ;; esac; }
r() { "$K" toolrun notes 2>/dev/null; }
printf 'a = 1\nb = 2\n' > a.txt; printf 'c = 3\n' > b.txt; printf 'd = 4\n' > c.txt

# ---------- nota "plan first" ----------
# pergunta respondida so com leituras: sete chamadas, nenhuma nota
o=$(printf '%s\n' USER 'Glob {"pattern":"*.txt"}' 'Read {"file_path":"a.txt"}' 'Read {"file_path":"b.txt"}' 'Grep {"pattern":"= 3"}' 'Read {"file_path":"c.txt"}' 'Grep {"pattern":"= 4"}' 'Glob {"pattern":"*.md"}' | r)
no "so leituras: nao pede plano na 3a chamada" "plan first" "$o"
no "so leituras: nao pede plano na 6a chamada" "still no plan" "$o"
# tres comandos: a nota vem na 3a chamada
o=$(printf '%s\n' USER 'Bash {"command":"echo 1"}' 'Bash {"command":"echo 2"}' 'Bash {"command":"echo 3"}' | r)
has "tres comandos: pede o plano" "[katana: plan first. This request already took 3 tool calls without a plan." "$o"
# leituras no meio nao contam: a nota vem no 3o comando (5a chamada), nao antes
o=$(printf '%s\n' USER 'Read {"file_path":"a.txt"}' 'Bash {"command":"echo 1"}' 'Read {"file_path":"b.txt"}' 'Bash {"command":"echo 2"}' | r)
no "dois comandos e duas leituras: sem nota" "plan first" "$o"
o=$(printf '%s\n' USER 'Read {"file_path":"a.txt"}' 'Bash {"command":"echo 1"}' 'Read {"file_path":"b.txt"}' 'Bash {"command":"echo 2"}' 'Bash {"command":"echo 3"}' | r)
has "terceiro comando depois de leituras: pede o plano" "plan first. This request already took 5 tool calls" "$o"
# a nota nao vem pendurada numa leitura feita depois do segundo comando
o=$(printf '%s\n' USER 'Bash {"command":"echo 1"}' 'Bash {"command":"echo 2"}' 'Read {"file_path":"a.txt"}' 'Read {"file_path":"b.txt"}' | r)
no "leitura depois de dois comandos: sem nota" "plan first" "$o"
# com plano escrito nao ha nota; pedido novo recomeca a conta
o=$(printf '%s\n' USER 'TodoWrite {"todos":"[>] um\n[ ] dois\n[ ] tres"}' 'Bash {"command":"echo 1"}' 'Bash {"command":"echo 2"}' 'Bash {"command":"echo 3"}' | r)
no "com plano: sem nota" "plan first" "$o"
o=$(printf '%s\n' USER 'Bash {"command":"echo 1"}' 'Bash {"command":"echo 2"}' USER 'Bash {"command":"echo 3"}' 'Read {"file_path":"a.txt"}' | r)
no "pedido novo recomeca a conta" "plan first" "$o"
# seis comandos sem plano: a segunda nota
o=$(printf '%s\n' USER 'Bash {"command":"echo 1"}' 'Bash {"command":"echo 2"}' 'Bash {"command":"echo 3"}' 'Bash {"command":"echo 4"}' 'Bash {"command":"echo 5"}' 'Bash {"command":"echo 6"}' | r)
has "seis comandos: a segunda nota" "[katana: still no plan after 6 tool calls." "$o"

# ---------- fim do pedido: lembrete de passos em aberto ----------
# o usuario so pediu para anotar tarefas: nem lembrete, nem tarefa fechada pelo motor
o=$(printf '%s\n' USER 'TaskCreate {"subject":"Review the parser","description":"later"}' 'TaskCreate {"subject":"Update the docs","description":"later"}' END 'TaskList {}' | r)
has "duas tarefas anotadas: sem lembrete" "[end: open=2 autofinish=0 nudge=0]" "$o"
o=$(printf '%s\n' USER 'TaskCreate {"subject":"Review the parser","description":"later"}' END 'TaskList {}' | r)
has "uma tarefa anotada: o motor nao a fecha" "[end: open=1 autofinish=0 nudge=0]" "$o"
has "uma tarefa anotada: continua pendente" "#1 [pending] Review the parser" "$o"
# tarefa que o modelo comecou e largou: lembrete
o=$(printf '%s\n' USER 'TaskCreate {"subject":"A","description":"d"}' 'TaskCreate {"subject":"B","description":"d"}' 'TaskUpdate {"taskId":"1","status":"in_progress"}' END | r)
has "tarefa comecada e largada: lembrete" "[end: open=2 autofinish=0 nudge=1]" "$o"
# plano do modelo (TodoWrite) com passos em aberto: lembrete; com so o ultimo em aberto: o motor fecha
o=$(printf '%s\n' USER 'TodoWrite {"todos":"[>] um\n[ ] dois\n[ ] tres"}' END | r)
has "plano com tres passos em aberto: lembrete" "[end: open=3 autofinish=0 nudge=1]" "$o"
o=$(printf '%s\n' USER 'TodoWrite {"todos":"[>] um\n[ ] dois\n[ ] tres"}' 'TodoWrite {"done":2}' END | r)
has "plano com so o ultimo passo em aberto: o motor fecha" "[end: open=1 autofinish=1 nudge=0]" "$o"
# tarefas anotadas num pedido, pergunta comum no seguinte: nada acontece com elas
o=$(printf '%s\n' USER 'TaskCreate {"subject":"A","description":"d"}' END USER 'Read {"file_path":"a.txt"}' END 'TaskList {}' | r)
has "tarefa anotada antes, pergunta depois: continua pendente" "#1 [pending] A" "$o"
# tarefa anotada e depois concluida pelo modelo com TaskUpdate: nada em aberto
o=$(printf '%s\n' USER 'TaskCreate {"subject":"A","description":"d"}' 'TaskUpdate {"taskId":"1","status":"completed"}' END | r)
has "tarefa concluida: nada em aberto" "[end: open=0 autofinish=0 nudge=0]" "$o"

# ---------- repeticao de chamada identica que deu certo ----------
awk 'BEGIN{for(i=1;i<=20;i++) print "linha " i}' > r.txt
o=$(printf '%s\n' USER 'Read {"file_path":"r.txt","offset":5,"limit":3}' 'Read {"file_path":"r.txt","offset":5,"limit":3}' | r)
has "repeticao: 1a chamada devolve o conteudo" "     5	linha 5" "$o"
has "repeticao: 2a chamada vira nota com o passo" "you already made this exact call at step 1 of this request" "$o"
o=$(printf '%s\n' USER 'Read {"file_path":"r.txt","offset":5,"limit":3}' 'Read {"file_path":"r.txt","offset":5,"limit":3}' 'Read {"file_path":"r.txt","offset":5,"limit":3}' 'Read {"file_path":"r.txt","offset":5,"limit":3}' | r)
has "repeticao: 3a repeticao fica firme" "is the 4th time you make this exact call" "$o"
has "repeticao: 3a repeticao cita o que fazer" "Grep (output_mode content, -n -C 2)" "$o"
o=$(printf '%s\n' USER 'Read {"file_path":"r.txt","offset":5,"limit":3}' 'Read {"file_path":"r.txt","offset":6,"limit":3}' | r)
no "repeticao: argumentos diferentes rodam" "you already made" "$o"
o=$(printf '%s\n' USER 'Grep {"pattern":"linha 7","output_mode":"content"}' 'Grep {"pattern":"linha 7","output_mode":"content"}' | r)
has "repeticao: Grep tambem" "you already made this exact call at step 1" "$o"
o=$(printf '%s\n' USER 'Glob {"pattern":"*.txt"}' 'Glob {"pattern":"*.txt"}' | r)
has "repeticao: Glob tambem" "you already made this exact call at step 1" "$o"
# Bash nao e coberto; uma escrita zera a tabela (a leitura depois pode dar outro resultado)
o=$(printf '%s\n' USER 'Bash {"command":"echo oi"}' 'Bash {"command":"echo oi"}' | r)
no "repeticao: Bash nao e coberto" "you already made" "$o"
o=$(printf '%s\n' USER 'Read {"file_path":"r.txt","offset":5,"limit":3}' 'Bash {"command":"echo x >> r.txt"}' 'Read {"file_path":"r.txt","offset":5,"limit":3}' | r)
no "repeticao: depois de um Bash a leitura roda" "you already made" "$o"
# pedido novo zera
o=$(printf '%s\n' USER 'Read {"file_path":"r.txt","offset":5,"limit":3}' USER 'Read {"file_path":"r.txt","offset":5,"limit":3}' | r)
no "repeticao: pedido novo recomeca" "you already made" "$o"
# chamada que falhou nao conta (o laco ja trata a insistencia no erro)
o=$(printf '%s\n' USER 'Read {"file_path":"nao-existe.txt"}' 'Read {"file_path":"nao-existe.txt"}' | r)
no "repeticao: chamada com erro nao vira nota" "you already made" "$o"
# desligada
o=$(printf '%s\n' USER 'Read {"file_path":"r.txt","offset":5,"limit":3}' 'Read {"file_path":"r.txt","offset":5,"limit":3}' | KATANA_REPEAT_NOTE=0 "$K" toolrun notes 2>/dev/null)
no "repeticao: KATANA_REPEAT_NOTE=0 desliga" "you already made" "$o"
# leitura inteira repetida: a nota de repeticao vem primeiro e a de leitura por diferenca nao se soma a ela
awk 'BEGIN{for(i=1;i<=30;i++) print "linha " i}' > w.txt
o=$(printf '%s\n' USER 'Read {"file_path":"w.txt"}' 'Read {"file_path":"w.txt"}' | r)
has "repeticao: leitura inteira repetida vira nota" "you already made this exact call at step 1" "$o"
no "repeticao: sem nota de leitura por diferenca junto" "has not changed since you read it" "$o"

if [ "$F" -eq 0 ]; then echo "OK $N checks"; exit 0; fi
echo "FAILED $F of $N"; exit 1
