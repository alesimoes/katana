#!/bin/sh
# Teste das ferramentas TodoWrite, TaskCreate/Update/List/Get, AskUserQuestion, EnterPlanMode, ExitPlanMode.
# Uso: sh tools/tooltest/tasks.sh /caminho/do/katana
# Nao compila nada. Respostas ao usuario entram por pipe: sem KATANA_UI a entrada nao e terminal (ninguem responde);
# com KATANA_UI=1 vale o protocolo da interface (marcador CHOICE na saida, resposta na linha "\001ANS texto").
K=${1:?usage: tasks.sh /path/to/katana}
export KATANA_LOG=${KATANA_LOG:-$(mktemp)}     # o log das chamadas destes testes nao vai para o ~/.katana/agente.log do dono
case $K in /*) ;; *) K=$(pwd)/$K ;; esac
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
cd "$T" || exit 2
N=0; F=0
SOH=$(printf '\001')

# chk nome esperado obtido (comparacao exata)
chk() { N=$((N+1)); if [ "$2" = "$3" ]; then echo "ok   $1"; else F=$((F+1)); echo "FAIL $1"; echo "  esperado: $2"; echo "  obtido:   $3"; fi; }
# has nome trecho obtido (contem)
has() { N=$((N+1)); case $3 in *"$2"*) echo "ok   $1" ;; *) F=$((F+1)); echo "FAIL $1"; echo "  faltou: $2"; echo "  obtido: $3" ;; esac; }
# no nome trecho obtido (nao contem)
no() { N=$((N+1)); case $3 in *"$2"*) F=$((F+1)); echo "FAIL $1"; echo "  nao devia ter: $2"; echo "  obtido: $3" ;; *) echo "ok   $1" ;; esac; }
t() { "$K" tool "$@" 2>/dev/null </dev/null; }                  # sem UI, sem entrada
u() { ans=$1; shift; printf '%s\n' "$ans" | KATANA_UI=1 "$K" tool "$@" 2>/dev/null; }   # com UI; $ans: linhas de resposta

W='{"todos":[{"content":"Run tests","status":"completed","activeForm":"Running tests"},{"content":"Fix bug","status":"in_progress","activeForm":"Fixing bug"},{"content":"Write docs","status":"pending","activeForm":"Writing docs"}]}'

# ---------- TodoWrite ----------
chk "TodoWrite resultado" "Todos updated: 3 tasks (1 completed, 1 in progress, 1 pending). Now step 2 of 3: Fix bug. Do it, then mark it completed." "$(t TodoWrite "$W")"
o=$(KATANA_UI=1 "$K" tool TodoWrite "$W" 2>/dev/null </dev/null)
chk "TodoWrite marcador TODO igual ao do todo antigo" "${SOH}TODO [x] Run tests\\n[ ] Fix bug\\n[ ] Write docs\\n" "$(printf '%s\n' "$o" | sed -n '1p' | tr -d '\n')"
# plano em linhas de texto (mais barato que o array): [x] feito, [>] ou [~] em andamento, [ ] ou nada pendente
chk "TodoWrite em linhas" "Todos updated: 4 tasks (1 completed, 1 in progress, 2 pending)." "$(t TodoWrite '{"todos":"[x] Run tests\n[>] Fix bug\n[ ] Write docs\nShip it"}')"
chk "TodoWrite em linhas com [~] e numeracao" "Todos updated: 3 tasks (1 completed, 1 in progress, 1 pending)." "$(t TodoWrite '{"todos":"1. [X] a\n2. [~] b\n- [ ] c"}')"
has "TodoWrite em linhas vazio" "error: todos has no steps" "$(t TodoWrite '{"todos":"\n  \n"}')"
o=$(KATANA_UI=1 "$K" tool TodoWrite '{"todos":"[x] Run tests\n[ ] Fix bug"}' 2>/dev/null </dev/null)
chk "TodoWrite em linhas marcador TODO" "${SOH}TODO [x] Run tests\\n[ ] Fix bug\\n" "$(printf '%s\n' "$o" | sed -n '1p' | tr -d '\n')"
chk "TodoWrite array dentro de texto continua array" "Todos updated: 1 tasks (0 completed, 0 in progress, 1 pending)." "$(t TodoWrite '{"todos":"[{\"content\":\"a\",\"status\":\"pending\",\"activeForm\":\"A\"}]"}')"
has "TodoWrite sem UI nao emite marcador" "Todos updated" "$(t TodoWrite "$W")"
no  "TodoWrite sem UI sem SOH" "$SOH" "$(t TodoWrite "$W")"
has "TodoWrite 2 em andamento avisa" "Warning: more than one task is in_progress" "$(t TodoWrite '{"todos":[{"content":"a","status":"in_progress","activeForm":"A"},{"content":"b","status":"in_progress","activeForm":"B"}]}')"
chk "TodoWrite lista vazia limpa" "Todos updated: 0 tasks (0 completed, 0 in progress, 0 pending)." "$(t TodoWrite '{"todos":[]}')"
has "TodoWrite status invalido" 'error: todos[1].status is "done"; it must be pending, in_progress or completed' "$(t TodoWrite '{"todos":[{"content":"a","status":"pending","activeForm":"A"},{"content":"b","status":"done","activeForm":"B"}]}')"
has "TodoWrite sem status" "error: todos[0].status is missing" "$(t TodoWrite '{"todos":[{"content":"a","activeForm":"A"}]}')"
has "TodoWrite sem content" "error: todos[0].content is missing or empty" "$(t TodoWrite '{"todos":[{"content":"","status":"pending","activeForm":"A"}]}')"
has "TodoWrite item nao e objeto" "error: todos[0] must be an object" "$(t TodoWrite '{"todos":["x"]}')"
has "TodoWrite sem todos" "error: todos must be an array" "$(t TodoWrite '{}')"
has "TodoWrite todos texto vira um passo (forma em linhas)" "Todos updated: 1 tasks (0 completed, 0 in progress, 1 pending)." "$(t TodoWrite '{"todos":"a, b"}')"
has "TodoWrite todos numero continua recusado" "error: todos must be an array" "$(t TodoWrite '{"todos":3}')"
chk "TodoWrite erro nao altera a lista" "#1 [pending] x" "$(t TodoWrite '{"todos":[{"content":"x","status":"pending","activeForm":"X"}]}' TodoWrite '{"todos":[{"content":"y","status":"bad","activeForm":"Y"}]}' TaskList '{}' | tail -n 1)"
has "TodoWrite texto com aspas e acentos" 'He said "hi" cafe' "$(t TodoWrite '{"todos":[{"content":"He said \"hi\" cafe","status":"pending","activeForm":"A"}]}' TaskList '{}')"

# ---------- TaskCreate / Update / List / Get (mesmo processo para dividir a lista) ----------
o=$(t TaskCreate '{"subject":"First","description":"do first","activeForm":"Doing first"}' TaskCreate '{"subject":"Second","description":"do second"}' TaskList '{}')
chk "TaskCreate dois, TaskList" "=== TaskCreate
Task #1 created: First
=== TaskCreate
Task #2 created: Second
=== TaskList
#1 [pending] First
#2 [pending] Second" "$o"
o=$(t TaskCreate '{"subject":"A","description":"d"}' TaskUpdate '{"taskId":"1","status":"in_progress"}' TaskGet '{"taskId":"1"}')
has "TaskUpdate in_progress" "Task #1 updated: A [in_progress]" "$o"
has "TaskGet campos" "id: 1
subject: A
status: in_progress
activeForm: A
description: d" "$o"
o=$(t TaskCreate '{"subject":"A","description":"d"}' TaskUpdate '{"taskId":1,"subject":"A2","description":"d2","activeForm":"Doing A2","status":"completed"}' TaskGet '{"taskId":"#1"}')
has "TaskUpdate campos (id numero)" "Task #1 updated: A2 [completed]" "$o"
has "TaskGet id com # e campos novos" "subject: A2
status: completed
activeForm: Doing A2
description: d2" "$o"
o=$(t TaskCreate '{"subject":"A","description":"d"}' TaskCreate '{"subject":"B","description":"d"}' TaskUpdate '{"taskId":"1","status":"deleted"}' TaskCreate '{"subject":"C","description":"d"}' TaskList '{}')
has "TaskUpdate deleted" "Task #1 deleted. 1 tasks left." "$o"
has "ids estaveis depois de apagar" "Task #3 created: C" "$o"
has "lista depois de apagar" "#2 [pending] B
#3 [pending] C" "$o"
no  "apagada some da lista" "#1 [pending] A" "$o"
has "dois em andamento avisa" "Warning: more than one" "$(t TaskCreate '{"subject":"A","description":"d"}' TaskCreate '{"subject":"B","description":"d"}' TaskUpdate '{"taskId":"1","status":"in_progress"}' TaskUpdate '{"taskId":"2","status":"in_progress"}')"
chk "TaskList vazia" "No tasks." "$(t TaskList '{}')"
has "TaskCreate sem subject" "error: subject is required" "$(t TaskCreate '{"description":"x"}')"
has "TaskUpdate id desconhecido lista ids" "error: no task with id 9. Valid ids: 1, 2" "$(t TaskCreate '{"subject":"A","description":"d"}' TaskCreate '{"subject":"B","description":"d"}' TaskUpdate '{"taskId":"9","status":"completed"}')"
has "TaskGet id desconhecido" "error: no task with id 5. Valid ids: none, the list is empty" "$(t TaskGet '{"taskId":"5"}')"
has "TaskGet sem taskId" "error: taskId is required" "$(t TaskGet '{}')"
has "TaskGet id nao numerico" 'error: taskId "abc" is not a number' "$(t TaskGet '{"taskId":"abc"}')"
has "TaskUpdate status invalido" 'error: status "done" is invalid; use pending, in_progress, completed or deleted' "$(t TaskCreate '{"subject":"A","description":"d"}' TaskUpdate '{"taskId":"1","status":"done"}')"
has "TaskUpdate subject vazio" "error: subject cannot be empty" "$(t TaskCreate '{"subject":"A","description":"d"}' TaskUpdate '{"taskId":"1","subject":""}')"
# TodoWrite e Task* dividem a lista; ids novos a cada TodoWrite
o=$(t TodoWrite "$W" TaskUpdate '{"taskId":"3","status":"completed"}' TaskList '{}')
has "TodoWrite depois TaskUpdate" "#3 [completed] Write docs" "$o"
# marcador igual ao do todo antigo apos Task*
o=$(KATANA_UI=1 "$K" tool TaskCreate '{"subject":"One","description":"d"}' TaskCreate '{"subject":"Two","description":"d"}' TaskUpdate '{"taskId":"1","status":"completed"}' 2>/dev/null </dev/null | grep "${SOH}TODO" | tail -n 1)
chk "marcador TODO apos TaskUpdate" "${SOH}TODO [x] One\\n[ ] Two\\n" "$o"
o=$(KATANA_UI=1 "$K" tool TaskCreate '{"subject":"One","description":"d"}' TaskUpdate '{"taskId":"1","status":"deleted"}' 2>/dev/null </dev/null | grep "${SOH}TODO" | tail -n 1)
chk "marcador TODO com lista vazia" "${SOH}TODO " "$o"

# ---------- AskUserQuestion ----------
Q='{"questions":[{"question":"Which database?","header":"DB","options":[{"label":"Postgres","description":"relational"},{"label":"SQLite","description":"file based"}],"multiSelect":false}]}'
has "sem ninguem para responder" "error: nobody can answer right now" "$(t AskUserQuestion "$Q")"
o=$(u "${SOH}ANS Postgres" AskUserQuestion "$Q")
has "UI: marcador CHOICE com rotulos" "${SOH}CHOICE [DB] Which database? Options: (1) Postgres = relational; (2) SQLite = file based.	Postgres	SQLite" "$o"
has "UI: resposta por rotulo" "User answers:
1. Which database? -> Postgres" "$o"
has "UI: rotulo sem diferenciar maiusculas" "-> SQLite" "$(u "${SOH}ANS sqlite" AskUserQuestion "$Q")"
has "UI: texto livre" "1. Which database? -> (free text) Use DuckDB, please." "$(u "${SOH}ANS Use DuckDB, please." AskUserQuestion "$Q")"
has "UI: sem resposta cancela" "the user gave no answer (cancelled)" "$(u "${SOH}ANS" AskUserQuestion "$Q")"
M='{"questions":[{"question":"Which features?","header":"Features","multiSelect":true,"options":[{"label":"Auth","description":"login"},{"label":"Billing","description":"pay"},{"label":"Admin","description":"panel"}]}]}'
o=$(u "${SOH}ANS Auth, admin" AskUserQuestion "$M")
has "multiSelect avisa de virgulas" "type the labels separated by commas" "$o"
has "multiSelect varios rotulos" "1. Which features? -> Auth, Admin" "$o"
has "multiSelect um rotulo" "-> Billing" "$(u "${SOH}ANS Billing" AskUserQuestion "$M")"
has "multiSelect misto vira texto livre" "-> (free text) Auth, Other" "$(u "${SOH}ANS Auth, Other" AskUserQuestion "$M")"
no  "single nao avisa de virgulas" "separated by commas" "$(u "${SOH}ANS Postgres" AskUserQuestion "$Q")"
TWO='{"questions":[{"question":"Q one?","header":"One","options":[{"label":"A","description":"a"},{"label":"B","description":"b"}],"multiSelect":false},{"question":"Q two?","header":"Two","options":[{"label":"X","description":"x"},{"label":"Y","description":"y"},{"label":"Z","description":"z"},{"label":"W","description":"w"}],"multiSelect":false}]}'
o=$(u "${SOH}ANS B
${SOH}ANS Z" AskUserQuestion "$TWO")
chk "duas perguntas, uma por vez" "User answers:
1. Q one? -> B
2. Q two? -> Z" "$(printf '%s\n' "$o" | grep -v "$SOH")"
chk "duas perguntas: dois marcadores CHOICE" "2" "$(printf '%s\n' "$o" | grep -c "${SOH}CHOICE")"
o=$(u "${SOH}ANS B" AskUserQuestion "$TWO")
has "cancelada no meio devolve as respostas ja dadas" "1. Q one? -> B
2. cancelled" "$o"
# forma antiga
has "forma antiga question+options" "1. Pick one? -> two" "$(u "${SOH}ANS two" AskUserQuestion '{"question":"Pick one?","options":"one|two|three"}')"
has "forma antiga com lista de textos" "1. Pick one? -> b" "$(u "${SOH}ANS b" AskUserQuestion '{"question":"Pick one?","options":["a","b"]}')"
# validacao
has "sem questions" "error: questions is required" "$(t AskUserQuestion '{}')"
has "questions vazio" "error: questions is empty" "$(t AskUserQuestion '{"questions":[]}')"
has "questions nao e lista" "error: questions must be an array" "$(t AskUserQuestion '{"questions":"x"}')"
has "5 perguntas" "error: too many questions; ask at most 4 at a time" "$(t AskUserQuestion '{"questions":[{"question":"1","options":["a","b"]},{"question":"2","options":["a","b"]},{"question":"3","options":["a","b"]},{"question":"4","options":["a","b"]},{"question":"5","options":["a","b"]}]}')"
has "pergunta sem texto" "error: questions[0].question is missing or empty" "$(t AskUserQuestion '{"questions":[{"header":"h","options":["a","b"]}]}')"
has "sem options" "error: questions[0].options is missing" "$(t AskUserQuestion '{"questions":[{"question":"q?","header":"h"}]}')"
has "uma opcao so" "error: questions[0].options has 1 usable option(s); it needs 2 to 4" "$(t AskUserQuestion '{"questions":[{"question":"q?","header":"h","options":[{"label":"a","description":"d"}]}]}')"
has "cinco opcoes" "error: questions[0].options has 5 options; the maximum is 4" "$(t AskUserQuestion '{"questions":[{"question":"q?","header":"h","options":["a","b","c","d","e"]}]}')"
has "rotulos repetidos" 'error: questions[1].options has the label "A" twice' "$(t AskUserQuestion '{"questions":[{"question":"q?","options":["a","b"]},{"question":"r?","options":["a","A"]}]}')"
has "item nao e objeto" "error: questions[0] must be an object" "$(t AskUserQuestion '{"questions":["q"]}')"

# ---------- EnterPlanMode / ExitPlanMode ----------
has "ExitPlanMode fora do modo plano" "error: plan mode is not on" "$(t ExitPlanMode '{"plan":"1. x"}')"
o=$(KATANA_UI=1 "$K" tool EnterPlanMode '{}' 2>/dev/null </dev/null)
has "EnterPlanMode liga" "Plan mode is ON" "$o"
has "EnterPlanMode emite MODE plan" "${SOH}MODE plan" "$o"
has "EnterPlanMode de novo" "Plan mode was already on" "$(t EnterPlanMode '{}' EnterPlanMode '{}')"
P='{"plan":"1. Read the code\n2. Change it"}'
has "ExitPlanMode sem plano" "error: plan is required" "$(t EnterPlanMode '{}' ExitPlanMode '{}')"
has "ExitPlanMode sem ninguem" "error: nobody can answer right now" "$(t EnterPlanMode '{}' ExitPlanMode "$P")"
o=$( (printf '%s\n' "${SOH}ANS Approve") | KATANA_UI=1 "$K" tool EnterPlanMode '{}' ExitPlanMode "$P" 2>/dev/null)
has "ExitPlanMode mostra o plano na pergunta" "${SOH}CHOICE Plan for approval: 1. Read the code 2. Change it Approve it, or type what to change.	Approve	Keep planning" "$o"
has "ExitPlanMode aprovado" "The user approved the plan. Plan mode is OFF" "$o"
chk "ExitPlanMode aprovado emite MODE normal" "${SOH}MODE normal" "$(printf '%s\n' "$o" | grep "${SOH}MODE normal")"
o=$( (printf '%s\n' "${SOH}ANS Approve") | KATANA_UI=1 "$K" tool EnterPlanMode '{}' ExitPlanMode "$P" ExitPlanMode "$P" 2>/dev/null)
has "depois de aprovar o modo plano esta desligado" "error: plan mode is not on" "$o"
o=$( (printf '%s\n' "${SOH}ANS Add tests first") | KATANA_UI=1 "$K" tool EnterPlanMode '{}' ExitPlanMode "$P" 2>/dev/null)
has "feedback em texto livre" "Plan mode stays ON. Their feedback: Add tests first" "$o"
no  "feedback nao desliga o modo" "${SOH}MODE normal" "$o"
o=$( (printf '%s\n%s\n' "${SOH}ANS Keep planning" "${SOH}ANS yes") | KATANA_UI=1 "$K" tool EnterPlanMode '{}' ExitPlanMode "$P" ExitPlanMode "$P" 2>/dev/null)
has "Keep planning mantem o modo" "gave no details. Plan mode stays ON" "$o"
has "depois de Keep planning ainda aprova" "The user approved the plan" "$o"
o=$( (printf '%s\n' "${SOH}ANS") | KATANA_UI=1 "$K" tool EnterPlanMode '{}' ExitPlanMode "$P" 2>/dev/null)
has "cancelado mantem o modo" "the user gave no answer (cancelled)" "$o"
no  "cancelado nao desliga" "${SOH}MODE normal" "$o"

# ---------- dispatcher normal (uma ferramenta) e modo plano ----------
has "uma ferramenta pelo despacho normal" "Task #1 created: Solo" "$(t TaskCreate '{"subject":"Solo","description":"d"}')"
# chamada em XML: a lista chega como texto com o JSON dentro; a ferramenta recebe a lista
has "TodoWrite com a lista em texto" "Todos updated: 2 tasks (0 completed, 1 in progress, 1 pending)" "$(t TodoWrite '{"todos": "[{\"content\": \"ler\", \"status\": \"pending\"}, {\"content\": \"corrigir\", \"status\": \"in_progress\"}]"}')"
has "AskUserQuestion com a lista em texto" "Qual? -> A" "$(u "${SOH}ANS A" AskUserQuestion '{"questions": "[{\"question\": \"Qual?\", \"options\": [{\"label\": \"A\"}, {\"label\": \"B\"}]}]"}' | grep -v "$SOH")"
has "texto que nao e JSON e um plano de um passo" "Todos updated: 1 tasks" "$(t TodoWrite '{"todos": "ler e corrigir"}')"
# as ferramentas antigas continuam
has "todo antigo continua" "list saved: 0 of 2 done. Next: a" "$("$K" tool todo '{"steps":"a\nb"}' 2>/dev/null </dev/null)"
has "ask_user antigo continua" "the user answered: ok" "$(printf '%s\n' "${SOH}ANS ok" | KATANA_UI=1 "$K" tool ask_user '{"question":"q?","options":"ok|no"}' 2>/dev/null | grep -v "$SOH")"

if [ "$F" -eq 0 ]; then echo "OK $N checks"; exit 0; fi
echo "FAILED $F of $N"; exit 1
