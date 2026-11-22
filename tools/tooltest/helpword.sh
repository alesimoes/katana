#!/bin/sh
# katana help <palavra>: so as linhas das tres ajudas (opcoes, subcomandos, comandos do console) que tem a palavra.
# uso: sh tools/tooltest/helpword.sh [caminho do katana]
K=${1:-./katana}
fail=0
ok() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
o=$("$K" help undo); rc=$?
ok "undo: achou, codigo 0" '[ $rc -eq 0 ]'
ok "undo: secao do console e a linha do /undo" 'echo "$o" | grep -q "^console /help:" && echo "$o" | grep -q "/undo "'
ok "undo: so a linha pedida (2 linhas)" '[ "$(echo "$o" | wc -l)" -eq 2 ]'
o=$("$K" help turns)
ok "turns: a opcao --max-turns, na secao das opcoes" 'echo "$o" | grep -q "^katana --help:" && echo "$o" | grep -q -- "--max-turns"'
o=$("$K" help HASH)
ok "sem diferenciar maiusculas: katana verify" 'echo "$o" | grep -q "katana verify"'
o=$("$K" help update upgrade); rc=$?
ok "nada encontrado: codigo 1 e a frase" '[ $rc -eq 1 ] && echo "$o" | grep -q "no help line has"'
o=$("$K" help serve)
ok "linha de continuacao acompanha a de cima" 'echo "$o" | grep -q "KATANA_API_KEY"'
a=$("$K" --help | wc -c); b=$("$K" help continue | wc -c)
ok "o recorte e menor que a ajuda inteira ($b < $a)" '[ "$b" -lt "$a" ] && [ "$b" -lt 300 ]'
exit $fail
