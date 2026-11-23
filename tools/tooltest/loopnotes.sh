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
o=$(printf '%s\n' USER 'Grep {"pattern":"a ="}' 'Read {"file_path":"a.txt"}' 'Read {"file_path":"b.txt"}' 'Grep {"pattern":"= 3"}' 'Read {"file_path":"c.txt"}' 'Grep {"pattern":"= 4"}' 'Grep {"pattern":"d ="}' | r)
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
# plano do modelo (TodoWrite) com passos em aberto: lembrete; com so o ultimo em aberto: o motor fecha
o=$(printf '%s\n' USER 'TodoWrite {"todos":"[>] um\n[ ] dois\n[ ] tres"}' END | r)
has "plano com tres passos em aberto: lembrete" "[end: open=3 autofinish=0 nudge=1]" "$o"
o=$(printf '%s\n' USER 'TodoWrite {"todos":"[>] um\n[ ] dois\n[ ] tres"}' 'TodoWrite {"done":2}' END | r)
has "plano com so o ultimo passo em aberto: o motor fecha" "[end: open=1 autofinish=1 nudge=0]" "$o"

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
# leitura inteira repetida: a nota de repeticao vem primeiro e a de leitura por diferenca nao se soma a ela
awk 'BEGIN{for(i=1;i<=30;i++) print "linha " i}' > w.txt
o=$(printf '%s\n' USER 'Read {"file_path":"w.txt"}' 'Read {"file_path":"w.txt"}' | r)
has "repeticao: leitura inteira repetida vira nota" "you already made this exact call at step 1" "$o"
no "repeticao: sem nota de leitura por diferenca junto" "has not changed since you read it" "$o"

# ---------- leitura por diferenca (facts.c): o arquivo relido inteiro, com a leitura anterior ainda no contexto ----------
TAB=$(printf '\t')
python3 -c "
for i in range(1,101): print('linha numero %d com algum texto' % i)" > d.txt
o=$(printf '%s\n' USER 'Read {"file_path":"d.txt"}' 'Edit {"file_path":"d.txt","old_string":"linha numero 50 com","new_string":"NOVA\nlinha 50 com"}' 'Read {"file_path":"d.txt"}' | r)
has "Read delta: so o trecho" "only the changed part is shown (lines 47-54 of 101)" "$o"
has "Read delta: deslocamento" "1 line further down" "$o"
has "Read delta: linha mudada com numero" "    50${TAB}NOVA" "$o"
if [ "$(printf '%s' "$o" | grep -c 'linha numero 10 ')" = 1 ]; then N=$((N+1)); echo "ok   Read delta: o resto nao se repete"; else N=$((N+1)); F=$((F+1)); echo "FAIL Read delta: o resto nao se repete"; fi
o=$(printf '%s\n' USER 'Read {"file_path":"d.txt"}' 'Bash {"command":"python3 -c \"print(chr(10).join(str(i)*30 for i in range(100)))\" > d.txt"}' 'Read {"file_path":"d.txt"}' | r)
has "Read delta: mudou muito, vai inteiro" "   100${TAB}9999" "$o"
python3 -c "
for i in range(1,101): print('linha numero %d com algum texto' % i)" > d.txt
o=$(printf '%s\n' USER 'Read {"file_path":"d.txt"}' 'Edit {"file_path":"d.txt","old_string":"linha numero 100 com algum texto","new_string":"ultima mudou"}' 'Read {"file_path":"d.txt"}' | r)
has "Read delta: mudanca na ultima linha" "only the changed part is shown" "$o"


# ---------- cursor do Read: faixa deslocada de 1 linha e limite por arquivo ----------
awk 'BEGIN{for(i=1;i<=400;i++) print "linha numero " i}' > s.txt
o=$(printf '%s\n' USER 'Read {"file_path":"s.txt","offset":100,"limit":80}' 'Read {"file_path":"s.txt","offset":101,"limit":80}' | r)
has "offset deslocado: nota com o intervalo ja visto" "lines 101-179 of" "$o"
has "offset deslocado: continua do primeiro trecho novo" "from line 180]" "$o"
has "offset deslocado: entrega a linha nova" "   180${TAB}linha numero 180" "$o"
o=$(printf '%s\n' USER 'Read {"file_path":"s.txt","offset":100,"limit":80}' 'Read {"file_path":"s.txt","offset":120,"limit":40}' | r)
has "faixa toda lida: segue do primeiro trecho novo" "lines 120-179 of" "$o"
has "faixa toda lida: o trecho novo vem" "   180${TAB}linha numero 180" "$o"
awk 'BEGIN{for(i=1;i<=50;i++) print "curto " i}' > t.txt
o=$(printf '%s\n' USER 'Read {"file_path":"t.txt","offset":1,"limit":50}' 'Read {"file_path":"t.txt","offset":10,"limit":10}' | r)
has "nada novo no arquivo: nota already seen" "already seen lines 10-19 of" "$o"
has "nada novo no arquivo: manda usar Grep ou Symbols" "use Grep (-n -C 2) or Symbols" "$o"
o=$(printf '%s\n' USER 'Read {"file_path":"s.txt","offset":100,"limit":80}' 'Read {"file_path":"s.txt","offset":300,"limit":50}' | r)
no "faixa nova: sem nota de ja visto" "already" "$o"
# arquivo mudado: o que foi lido antes deixa de valer
o=$(printf '%s\n' USER 'Read {"file_path":"s.txt","offset":100,"limit":80}' 'Bash {"command":"echo extra >> s.txt"}' 'Read {"file_path":"s.txt","offset":101,"limit":80}' | r)
no "arquivo mudou: rele a faixa" "already" "$o"
awk 'BEGIN{for(i=1;i<=400;i++) print "linha numero " i}' > s.txt
# limite por arquivo: a 6a leitura do mesmo arquivo leva a nota
o=$(printf '%s\n' USER 'Read {"file_path":"s.txt","offset":1,"limit":10}' 'Read {"file_path":"s.txt","offset":50,"limit":10}' 'Read {"file_path":"s.txt","offset":100,"limit":10}' 'Read {"file_path":"s.txt","offset":150,"limit":10}' 'Read {"file_path":"s.txt","offset":200,"limit":10}' | r)
no "5 leituras do mesmo arquivo: sem nota" "read number" "$o"
o=$(printf '%s\n' USER 'Read {"file_path":"s.txt","offset":1,"limit":10}' 'Read {"file_path":"s.txt","offset":50,"limit":10}' 'Read {"file_path":"s.txt","offset":100,"limit":10}' 'Read {"file_path":"s.txt","offset":150,"limit":10}' 'Read {"file_path":"s.txt","offset":200,"limit":10}' 'Read {"file_path":"s.txt","offset":250,"limit":10}' | r)
has "6a leitura do mesmo arquivo: nota" "read number 6 of" "$o"
has "6a leitura: aponta Grep e Symbols" "Use Grep (-n -C 2) or Symbols for the exact spot" "$o"
o=$(printf '%s\n' USER 'Read {"file_path":"s.txt","offset":1,"limit":10}' 'Read {"file_path":"s.txt","offset":50,"limit":10}' 'Read {"file_path":"s.txt","offset":100,"limit":10}' 'Read {"file_path":"s.txt","offset":150,"limit":10}' 'Read {"file_path":"s.txt","offset":200,"limit":10}' USER 'Read {"file_path":"s.txt","offset":250,"limit":10}' | r)
no "pedido novo zera a conta de leituras" "read number" "$o"

if [ "$F" -eq 0 ]; then echo "OK $N checks"; exit 0; fi
echo "FAILED $F of $N"; exit 1
