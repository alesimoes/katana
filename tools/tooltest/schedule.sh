#!/bin/sh
# Testa CronCreate, CronDelete, CronList e ScheduleWakeup sem modelo (katana tool) e a entrega (gancho KATANA_SCHED_TEST).
# Uso: tools/tooltest/schedule.sh <caminho-do-katana>
# Precisa de date do GNU; os testes de horario de verao usam /usr/share/zoneinfo. Com KATANA_SCHED_NOW os ids sao
# deterministas (mesma sequencia em cada processo), o que permite criar num processo e apagar no seguinte.
KAT=$(realpath "$1" 2>/dev/null)
[ -x "$KAT" ] || { echo "katana nao encontrado: $1"; exit 2; }
OK=0; FAIL=0
HOME=$(mktemp -d); export HOME; trap 'rm -rf "$HOME"' EXIT
unset KATANA_SCHED_TEST KATANA_SCHED_NOW

pass() { OK=$((OK+1)); echo "ok   $1"; }
fail() { FAIL=$((FAIL+1)); echo "FALHA $1"; [ -n "$2" ] && printf '%s\n' "$2" | head -6 | sed 's/^/     | /'; }
# eq <nome> <esperado> <obtido>
eq() { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1 (esperado [$2])" "obtido: $3"; fi; }
# has <nome> <trecho fixo> <texto>
has() { if printf '%s' "$3" | grep -qF -- "$2"; then pass "$1"; else fail "$1 (esperava conter [$2])" "$3"; fi; }
# epoch <TZ> <data e hora local>
epoch() { TZ="$1" date -d "$2" +%s; }
# run <TZ> <now epoch> <ferramenta> <json> [...]
run() { _tz=$1; _now=$2; shift 2; TZ=$_tz KATANA_SCHED_NOW=$_now "$KAT" tool "$@" 2>/dev/null; }
# deliver <TZ> <now epoch> <instante do teste> <ferramenta> <json> [...]: o que o ultimo CronList (com o gancho) imprime
deliver() {
    _tz=$1; _now=$2; _at=$3; shift 3
    TZ=$_tz KATANA_SCHED_NOW=$_now KATANA_SCHED_TEST=$_at "$KAT" tool "$@" CronList '{}' 2>/dev/null | sed -n '/^=== CronList/,$p' | tail -n +2
}
# horario do primeiro disparo de uma expressao criada em <now local>
next() { run "$1" "$(epoch "$1" "$2")" CronCreate "{\"cron\":\"$3\",\"prompt\":\"p\"}" | sed -n 's/.*Next fire: \([0-9-]* [0-9:]* [A-Za-z0-9+-]*\)\..*/\1/p'; }
# n <TZ> <now local> <expr> <esperado>
n() { eq "next [$3] depois de $2 ($1)" "$4" "$(next "$1" "$2" "$3")"; }
idof() { sed -n 's/^Scheduled [a-z-]* task \([0-9a-f]\{8\}\) .*/\1/p' | head -1; }

U=UTC
NOW='2026-11-21 14:30:00'      # sabado
T0=$(epoch $U "$NOW")

# ---- proximo disparo (UTC) ----
n $U "$NOW" '* * * * *'         '2026-11-21 14:31 UTC'
n $U "$NOW" '*/5 * * * *'       '2026-11-21 14:35 UTC'
n $U "$NOW" '30 14 * * *'       '2026-11-22 14:30 UTC'
n $U "$NOW" '31 14 * * *'       '2026-11-21 14:31 UTC'
n $U "$NOW" '0 0 * * *'         '2026-11-22 00:00 UTC'
n $U "$NOW" '0 9 * * 1-5'       '2026-11-23 09:00 UTC'
n $U "$NOW" '0 9 * * 1,3,5'     '2026-11-23 09:00 UTC'
n $U "$NOW" '15 10 * * 0'       '2026-11-22 10:15 UTC'
n $U "$NOW" '15 10 * * 7'       '2026-11-22 10:15 UTC'
n $U "$NOW" '0 12 * * 5-7'      '2026-11-22 12:00 UTC'
n $U "$NOW" '0 12 * * 6'        '2026-11-28 12:00 UTC'
n $U "$NOW" '0 0 1 * *'         '2026-12-01 00:00 UTC'
n $U "$NOW" '0 0 1 1 *'         '2027-01-01 00:00 UTC'
n $U "$NOW" '5 4 * 12 *'        '2026-12-01 04:05 UTC'
n $U "$NOW" '0-30/10 15 * * *'  '2026-11-21 15:00 UTC'
n $U "$NOW" '10-50/20 15 * * *' '2026-11-21 15:10 UTC'
n $U "$NOW" '*/15 */6 * * *'    '2026-11-21 18:00 UTC'
n $U "$NOW" '7,2,45 15 * * *'   '2026-11-21 15:02 UTC'
n $U "$NOW" '0 0 * 3 *'         '2027-03-01 00:00 UTC'
n $U "$NOW" '59 23 31 12 *'     '2026-12-31 23:59 UTC'
n $U "$NOW" '0 0 */10 * *'      '2026-12-01 00:00 UTC'
n $U "$NOW" '  0   8  *  *  *  ' '2026-11-22 08:00 UTC'
n $U "$NOW" '0 8 25 * 1'        '2026-11-23 08:00 UTC'     # dia 25 OU segunda: vale a segunda
n $U "$NOW" '0 8 22 * 1'        '2026-11-22 08:00 UTC'     # dia 22 OU segunda: vale o dia 22
n $U "$NOW" '0 8 22 * *'        '2026-11-22 08:00 UTC'     # so o dia do mes
n $U "$NOW" '0 8 * * 1'         '2026-11-23 08:00 UTC'     # so o dia da semana
n $U '2026-11-21 14:30:00' '30 14 * * *' '2026-11-22 14:30 UTC'    # o minuto atual nao conta
n $U '2026-11-21 14:30:45' '31 14 * * *' '2026-11-21 14:31 UTC'
# fim de mes, de ano, bissexto
n $U '2026-01-30 12:00:00' '0 0 31 * *'  '2026-01-31 00:00 UTC'
n $U '2026-02-01 12:00:00' '0 0 31 * *'  '2026-03-31 00:00 UTC'
n $U '2026-04-15 12:00:00' '0 0 31 * *'  '2026-05-31 00:00 UTC'
n $U '2026-12-31 23:59:30' '0 0 * * *'   '2027-01-01 00:00 UTC'
n $U '2027-06-01 00:00:00' '0 0 29 2 *'  '2028-02-29 00:00 UTC'
n $U '2028-02-28 12:00:00' '0 0 * * *'   '2028-02-29 00:00 UTC'
n $U '2028-03-01 00:00:00' '0 0 29 2 *'  '2032-02-29 00:00 UTC'
n $U '2026-03-01 00:00:00' '0 0 29 2 *'  '2028-02-29 00:00 UTC'
n $U '2092-03-01 00:00:00' '0 0 29 2 *'  '2096-02-29 00:00 UTC'
has "alem de 5 anos: erro" 'never matches' "$(run $U $(epoch $U '2096-03-01 00:00:00') CronCreate '{"cron":"0 0 29 2 *","prompt":"p"}')"

# ---- horario de verao ----
if [ -e /usr/share/zoneinfo/America/New_York ]; then
    NY=America/New_York
    # primavera 2026-03-08: 02:00 vira 03:00
    n $NY '2026-03-07 12:00:00' '30 1 * * *'  '2026-03-08 01:30 EST'
    n $NY '2026-03-08 01:40:00' '0 3 * * *'   '2026-03-08 03:00 EDT'
    n $NY '2026-03-08 01:59:00' '* * * * *'   '2026-03-08 03:00 EDT'
    n $NY '2026-03-08 03:00:00' '30 2 * * *'  '2026-03-09 02:30 EDT'
    n $NY '2026-03-07 12:00:00' '0 0 8 3 *'   '2026-03-08 00:00 EST'
    # outono 2026-11-01: 02:00 EDT volta a 01:00 EST (01:30 ocorre duas vezes)
    n $NY '2026-10-31 23:00:00' '30 1 * * *'  '2026-11-01 01:30 EDT'
    n $NY '2026-11-01 02:10:00' '30 1 * * *'  '2026-11-02 01:30 EST'
    n $NY '2026-11-01 00:10:00' '0 */2 * * *' '2026-11-01 02:00 EST'
    n $NY '2026-11-02 00:00:00' '0 0 * * *'   '2026-11-03 00:00 EST'
else echo "(sem zoneinfo: testes de horario de verao pulados)"; fi
if [ -e /usr/share/zoneinfo/Asia/Kolkata ]; then      # fuso com meia hora
    n Asia/Kolkata '2026-11-21 14:30:00' '0 0 * * *'  '2026-11-22 00:00 IST'
    n Asia/Kolkata '2026-11-21 14:30:00' '45 15 * * *' '2026-11-21 15:45 IST'
fi

# ---- erros de expressao: precisam nomear o campo ----
bad() {   # <nome> <expr> <trecho esperado>
    out=$(run $U $T0 CronCreate "{\"cron\":\"$2\",\"prompt\":\"p\"}")
    has "$1" "$3" "$out"
    case "$out" in error:*) pass "$1 comeca com error:";; *) fail "$1 deveria comecar com error:" "$out";; esac
}
bad err-min '60 * * * *'       "bad minute field '60'"
bad err-min2 'x * * * *'       "bad minute field 'x'"
bad err-hour '0 24 * * *'      "bad hour field '24'"
bad err-dom '0 0 0 * *'        "bad day-of-month field '0'"
bad err-dom2 '0 0 32 * *'      "bad day-of-month field '32'"
bad err-mon '0 0 1 13 *'       "bad month field '13'"
bad err-mon2 '0 0 1 jan *'     "bad month field 'jan'"
bad err-dow '0 0 * * 8'        "bad day-of-week field '8'"
bad err-range '5-3 * * * *'    "bad minute field '5-3'"
bad err-step '*/0 * * * *'     "bad minute field '*/0'"
bad err-step2 '*/ * * * *'     "bad minute field '*/'"
bad err-list '1,,2 * * * *'    "bad minute field '1,,2'"
bad err-list2 '1, * * * *'     "bad minute field '1,'"
bad err-neg '-1 * * * *'       "bad minute field '-1'"
bad err-few '* * * *'          "expected exactly 5 fields"
bad err-many '* * * * * *'     "expected exactly 5 fields"
bad err-feb30 '0 0 30 2 *'     "never matches"
bad err-feb31 '0 0 31 2 *'     "never matches"
bad err-apr31 '0 0 31 4 *'     "never matches"
out=$(run $U $T0 CronCreate '{"cron":"* * * * *"}');  has "prompt ausente" "error: CronCreate needs a non-empty \`prompt\`" "$out"
out=$(run $U $T0 CronCreate '{"prompt":"x"}');         has "cron ausente" "error: CronCreate needs \`cron\`" "$out"
out=$(run $U $T0 CronCreate '{"cron":"","prompt":"x"}'); has "cron vazio" "error: CronCreate needs \`cron\`" "$out"

# ---- CronCreate: resultado, id, expiracao ----
out=$(run $U $T0 CronCreate '{"cron":"*/5 * * * *","prompt":"verificar build"}')
id=$(printf '%s' "$out" | idof)
eq "create recorrente (padrao)" "Scheduled recurring task $id (*/5 * * * *). Next fire: 2026-11-21 14:35 UTC. Recurring tasks expire automatically 7 days after creation (2026-11-28 14:30 UTC)." "$out"
eq "id tem 8 caracteres hex" 1 "$(printf '%s\n' "$id" | grep -c '^[0-9a-f]\{8\}$')"
out=$(run $U $T0 CronCreate '{"cron":"0 18 * * *","prompt":"um","recurring":false}')
has "create unica" "one-shot task" "$out"
has "create unica: horario e remocao" "It fires once at 2026-11-21 18:00 UTC and is then removed." "$out"
has "recurring como texto false" "one-shot" "$(run $U $T0 CronCreate '{"cron":"0 18 * * *","prompt":"um","recurring":"false"}')"
has "recurring true" "recurring task" "$(run $U $T0 CronCreate '{"cron":"0 18 * * *","prompt":"um","recurring":true}')"
out=$(run $U $T0 CronCreate '{"cron":"* * * * *","prompt":"a"}' CronCreate '{"cron":"* * * * *","prompt":"b"}' CronCreate '{"cron":"* * * * *","prompt":"c"}')
eq "tres ids distintos" 3 "$(printf '%s\n' "$out" | sed -n 's/^Scheduled [a-z-]* task \([0-9a-f]\{8\}\) .*/\1/p' | sort -u | wc -l | tr -d ' ')"

# ---- CronList ----
eq "list vazia" "No scheduled tasks" "$(run $U $T0 CronList '{}')"
out=$(run $U $T0 CronCreate '{"cron":"*/5 * * * *","prompt":"primeiro"}' CronCreate '{"cron":"0 18 * * *","prompt":"segundo","recurring":false}' CronList '{}')
lst=$(printf '%s\n' "$out" | sed -n '/^=== CronList/,$p' | tail -n +2)
eq "list: duas linhas" 2 "$(printf '%s\n' "$lst" | wc -l | tr -d ' ')"
has "list: linha 1" "  */5 * * * *  recurring  next 2026-11-21 14:35 UTC  primeiro" "$lst"
has "list: linha 2" "  0 18 * * *  one-shot  next 2026-11-21 18:00 UTC  segundo" "$lst"
long=$(printf 'x%.0s' $(seq 1 100)); want80=$(printf 'x%.0s' $(seq 1 80))
last=$(run $U $T0 CronCreate "{\"cron\":\"* * * * *\",\"prompt\":\"$long\"}" CronList '{}' | tail -1)
has "list corta o prompt em 80 e marca com ..." "next 2026-11-21 14:31 UTC  $want80..." "$last"
case "$last" in *"${want80}x"*) fail "list passou de 80 caracteres";; *) pass "list: no maximo 80";; esac
last=$(run $U $T0 CronCreate '{"cron":"* * * * *","prompt":"curto"}' CronList '{}' | tail -1)
eq "list: prompt curto sem reticencias" 0 "$(printf '%s\n' "$last" | grep -c '\.\.\.')"
has "list: quebra de linha vira espaco" "linha1 linha2" "$(run $U $T0 CronCreate '{"cron":"* * * * *","prompt":"linha1\nlinha2"}' CronList '{}' | tail -1)"
acc=$(printf 'ç%.0s' $(seq 1 100))
last=$(run $U $T0 CronCreate "{\"cron\":\"* * * * *\",\"prompt\":\"$acc\"}" CronList '{}' | tail -1)
if printf '%s\n' "$last" | iconv -f UTF-8 -t UTF-8 >/dev/null 2>&1; then pass "list: UTF-8 continua valido"; else fail "list: UTF-8 cortado no meio" "$last"; fi
eq "list: 80 caracteres UTF-8" 80 "$(printf '%s\n' "$last" | sed 's/.*UTF-8//; s/.*  \(ç*\)\.\.\..*/\1/' | wc -m | tr -d ' ' | awk '{print $1-1}')"

# ---- CronDelete (ids deterministas com KATANA_SCHED_NOW: o id do 1o create e o mesmo em cada processo) ----
id=$(run $U $T0 CronCreate '{"cron":"* * * * *","prompt":"x"}' | idof)
out=$(run $U $T0 CronCreate '{"cron":"* * * * *","prompt":"x"}' CronDelete "{\"id\":\"$id\"}" CronList '{}' CronDelete "{\"id\":\"$id\"}")
has "delete: removido" "Deleted scheduled task $id" "$out"
has "list depois de apagar" "No scheduled tasks" "$out"
has "delete repetido: erro com lista de ids" "error: no scheduled task with id '$id'. Existing ids: (none)" "$out"
out=$(run $U $T0 CronCreate '{"cron":"* * * * *","prompt":"x"}' CronDelete '{"id":"zzz"}')
has "delete desconhecido lista os ids" "error: no scheduled task with id 'zzz'. Existing ids: $id" "$out"
has "delete sem id" "error: CronDelete needs \`id\`" "$(run $U $T0 CronDelete '{}')"
eq "nada persiste entre processos" "No scheduled tasks" "$(run $U $T0 CronList '{}')"

# ---- limite de 50 tarefas ----
set --
i=0; while [ $i -lt 51 ]; do set -- "$@" CronCreate "{\"cron\":\"*/5 * * * *\",\"prompt\":\"t$i\"}"; i=$((i+1)); done
many=$(run $U $T0 "$@")
eq "50 tarefas criadas" 50 "$(printf '%s\n' "$many" | grep -c '^Scheduled recurring task')"
has "a 51a e recusada" "error: too many scheduled tasks (maximum 50)" "$many"

# ---- entrega (gancho KATANA_SCHED_TEST) ----
has "antes do horario nada vence" "nothing due" "$(deliver $U $T0 $T0 CronCreate '{"cron":"* * * * *","prompt":"tique"}')"
d=$(deliver $U $T0 $((T0+60)) CronCreate '{"cron":"* * * * *","prompt":"tique"}')
has "no horario entrega, com label e prompt" "delivered [cron " "$d"
has "entrega traz o prompt" "] tique" "$d"
eq "entrega uma vez so" 1 "$(printf '%s\n' "$d" | grep -c '^delivered')"
d=$(deliver $U $T0 $((T0+3*3600)) CronCreate '{"cron":"* * * * *","prompt":"tique"}')
eq "sem recuperar execucoes perdidas (3 h = 180 minutos, uma entrega)" 1 "$(printf '%s\n' "$d" | grep -c '^delivered')"
out=$(TZ=$U KATANA_SCHED_NOW=$T0 KATANA_SCHED_TEST=$((T0+4*3600)) "$KAT" tool CronCreate '{"cron":"0 18 * * *","prompt":"uma vez","recurring":false}' CronList '{}' CronList '{}' 2>/dev/null)
l1=$(printf '%s\n' "$out" | sed -n '/^=== CronList/,$p' | tail -n +2 | head -3)
has "unica entregue" "delivered [cron " "$l1"
eq "unica removida depois de entregue" "No scheduled tasks|nothing due" "$(printf '%s\n' "$out" | tail -2 | tr '\n' '|' | sed 's/|$//')"
d=$(deliver $U $T0 $((T0+7200)) CronCreate '{"cron":"0 16 * * *","prompt":"das 16","recurring":false}' CronCreate '{"cron":"0 15 * * *","prompt":"das 15","recurring":false}')
eq "duas vencidas: duas entregas" 2 "$(printf '%s\n' "$d" | grep -c '^delivered')"
has "a mais antiga (15h) primeiro" "] das 15" "$(printf '%s\n' "$d" | grep '^delivered' | head -1)"
has "depois a de 16h" "] das 16" "$(printf '%s\n' "$d" | grep '^delivered' | tail -1)"
d=$(deliver $U $T0 $((T0+1740)) CronCreate '{"cron":"0 * * * *","prompt":"hora"}')
has "recorrente a 15:00: aos 14:59 nao vence" "nothing due" "$d"
has "recorrente a 15:00 entrega aos 15:00" "delivered [cron " "$(deliver $U $T0 $((T0+1800)) CronCreate '{"cron":"0 * * * *","prompt":"hora"}')"
E=$((T0 + 7*86400))
has "recorrente vale um minuto antes dos 7 dias" "delivered [cron " "$(deliver $U $T0 $((E-60)) CronCreate '{"cron":"0 12 * * *","prompt":"diaria"}')"
has "recorrente expira aos 7 dias: nao entrega" "nothing due" "$(deliver $U $T0 $E CronCreate '{"cron":"0 12 * * *","prompt":"diaria"}')"
eq "lista vazia depois de expirar" "No scheduled tasks" "$(TZ=$U KATANA_SCHED_NOW=$((T0+8*86400)) "$KAT" tool CronList '{}' 2>/dev/null)"
has "lista mostra a nao expirada" "recurring" "$(run $U $T0 CronCreate '{"cron":"0 12 * * *","prompt":"a"}' CronList '{}' | tail -1)"
has "unica atrasada 40 dias ainda entrega (so recorrentes expiram)" "delivered [cron " "$(deliver $U $T0 $((T0+40*86400)) CronCreate '{"cron":"0 12 25 12 *","prompt":"natal","recurring":false}')"
# depois da entrega o recorrente avanca para o proximo horario (visto por um segundo instante no mesmo processo nao e possivel
# com o gancho; confere que a entrega foi unica mesmo com o relogio 2 dias a frente e que o proximo e futuro)
d=$(deliver $U $T0 $((T0+2*86400)) CronCreate '{"cron":"0 12 * * *","prompt":"diaria"}')
eq "recorrente atrasada 2 dias: uma entrega" 1 "$(printf '%s\n' "$d" | grep -c '^delivered')"

# ---- ScheduleWakeup ----
eq "wakeup agendado" "Wakeup scheduled in 300 seconds, at 2026-11-21 14:35 UTC. Reason: build." "$(run $U $T0 ScheduleWakeup '{"delaySeconds":300,"prompt":"ver build","reason":"build"}')"
has "wakeup sem reason" "Wakeup scheduled in 300 seconds, at 2026-11-21 14:35 UTC." "$(run $U $T0 ScheduleWakeup '{"delaySeconds":300,"prompt":"x"}')"
has "delay minimo 60" "in 60 seconds (clamped to 60..3600), at 2026-11-21 14:31 UTC." "$(run $U $T0 ScheduleWakeup '{"delaySeconds":5,"prompt":"x"}')"
has "delay maximo 3600" "in 3600 seconds (clamped to 60..3600), at 2026-11-21 15:30 UTC." "$(run $U $T0 ScheduleWakeup '{"delaySeconds":99999,"prompt":"x"}')"
has "delay como texto" "in 120 seconds, at" "$(run $U $T0 ScheduleWakeup '{"delaySeconds":"120","prompt":"x"}')"
has "sem delaySeconds" "error: ScheduleWakeup needs \`delaySeconds\`" "$(run $U $T0 ScheduleWakeup '{"prompt":"x"}')"
has "sem prompt" "error: ScheduleWakeup needs a non-empty \`prompt\`" "$(run $U $T0 ScheduleWakeup '{"delaySeconds":60}')"
eq "stop sem pendente" "No pending wakeup to cancel" "$(run $U $T0 ScheduleWakeup '{"stop":true}')"
has "stop cancela e ignora os outros campos" "Cancelled the pending wakeup" "$(run $U $T0 ScheduleWakeup '{"delaySeconds":300,"prompt":"a"}' ScheduleWakeup '{"stop":true,"delaySeconds":1}')"
has "nova chamada substitui" "Replaced the previous pending wakeup." "$(run $U $T0 ScheduleWakeup '{"delaySeconds":300,"prompt":"a"}' ScheduleWakeup '{"delaySeconds":600,"prompt":"b"}')"
eq "wakeup antes da hora" "No scheduled tasks
nothing due" "$(deliver $U $T0 $((T0+299)) ScheduleWakeup '{"delaySeconds":300,"prompt":"acordar"}')"
eq "wakeup entregue" "No scheduled tasks
delivered [wakeup] acordar" "$(deliver $U $T0 $((T0+300)) ScheduleWakeup '{"delaySeconds":300,"prompt":"acordar"}')"
has "o substituto vale: b so em 600 (nada em 300)" "nothing due" "$(deliver $U $T0 $((T0+300)) ScheduleWakeup '{"delaySeconds":300,"prompt":"a"}' ScheduleWakeup '{"delaySeconds":600,"prompt":"b"}')"
has "o substituto entrega b" "delivered [wakeup] b" "$(deliver $U $T0 $((T0+600)) ScheduleWakeup '{"delaySeconds":300,"prompt":"a"}' ScheduleWakeup '{"delaySeconds":600,"prompt":"b"}')"
has "wakeup cancelado nao entrega" "nothing due" "$(deliver $U $T0 $((T0+9999)) ScheduleWakeup '{"delaySeconds":300,"prompt":"a"}' ScheduleWakeup '{"stop":true}')"
d=$(deliver $U $T0 $((T0+9999)) ScheduleWakeup '{"delaySeconds":300,"prompt":"a"}')
eq "wakeup entrega uma vez so" 1 "$(printf '%s\n' "$d" | grep -c '^delivered')"
d=$(deliver $U $T0 $((T0+3600)) CronCreate '{"cron":"40 14 * * *","prompt":"cron40","recurring":false}' ScheduleWakeup '{"delaySeconds":300,"prompt":"wake5"}')
has "wakeup (14:35) entregue antes do cron (14:40)" "delivered [wakeup] wake5" "$(printf '%s\n' "$d" | grep '^delivered' | head -1)"
has "depois o cron" "] cron40" "$(printf '%s\n' "$d" | grep '^delivered' | tail -1)"
# wakeup nao aparece no CronList e nao conta no limite de 50
eq "wakeup nao entra no CronList" "No scheduled tasks" "$(run $U $T0 ScheduleWakeup '{"delaySeconds":300,"prompt":"a"}' CronList '{}' | tail -1)"

# ---- esquema vindo da tabela (o modelo ve os nomes e os obrigatorios) ----
sys=$("$KAT" agentsys json 2>/dev/null)
for nm in CronCreate CronDelete CronList ScheduleWakeup; do
    case "$sys" in *"$nm"*) pass "agentsys lista $nm";; *) echo "(agentsys nao lista $nm: depende do registro no agente, fora deste arquivo)";; esac
done

echo
if [ $FAIL -eq 0 ]; then echo "OK $OK checks"; exit 0; else echo "FAILED $FAIL"; exit 1; fi
