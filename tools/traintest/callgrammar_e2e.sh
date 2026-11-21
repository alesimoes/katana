#!/bin/sh
# Chamadas validas por construcao (src/agent/callgrammar.c) com o modelo minusculo de pesos aleatorios: ele so gera
# lixo, entao a chamada so sai valida se a restricao funcionar. KATANA_CALL_FORCE abre a chamada (e escolhe a
# ferramenta); KATANA_CALL_GRAMMAR_BUDGET limita cada valor para o lixo terminar. Confere no log do agente que a
# chamada foi lida, com a ferramenta certa e os valores tipados validos.
# Uso: sh tools/traintest/callgrammar_e2e.sh /caminho/do/katana
K=${1:?usage: callgrammar_e2e.sh /path/to/katana}
case $K in /*) ;; *) K=$(pwd)/$K ;; esac
S=$(cd "$(dirname "$0")/../.." && pwd)
D=$(mktemp -d) || exit 2
trap 'rm -rf "$D"' EXIT
N=0; F=0
ok() { N=$((N+1)); echo "ok   $1"; }
bad() { N=$((N+1)); F=$((F+1)); echo "FAIL $1"; echo "  $2"; }
python3 "$S/tools/tinymodel/make_tiny_gguf.py" "$D/xml.gguf" --template chatml-xml >/dev/null
python3 "$S/tools/tinymodel/make_tiny_gguf.py" "$D/json.gguf" >/dev/null
python3 "$S/tools/tinymodel/make_tiny_gguf.py" "$D/g4.gguf" --arch gemma4 --template gemma4 >/dev/null
NL='
'
# run nome modelo forca ferramenta [seed]
run() {
  mkdir -p "$D/$1/w" "$D/$1/home"
  (cd "$D/$1/w" && printf 'go\n/exit\n' | HOME="$D/$1/home" KATANA_LOG="$D/$1/log" KATANA_CALL_FORCE="$3" \
     KATANA_CALL_GRAMMAR_BUDGET=6 KATANA_MAXTOK=1000 timeout 600 "$K" agent "$2" >"$D/$1/out" 2>&1)
  line=$(grep -a "	$4	" "$D/$1/log" 2>/dev/null | head -1)
  if [ -n "$line" ]; then ok "$1: call to $4 parsed"; else bad "$1: call to $4 parsed" "$(tail -c 600 "$D/$1/out")"; fi
}
# int nome chave: o valor da chave, se veio, e um inteiro
int() {
  v=$(python3 - "$D/$1/log" "$2" "$3" <<'EOF'
import json, sys
for l in open(sys.argv[1], encoding="utf-8", errors="replace"):
    f = l.split("\t")
    if len(f) > 2 and f[1] == sys.argv[2]:
        a = json.JSONDecoder(strict=False).raw_decode("{" + f[2] + "}")[0]["arguments"]
        v = a.get(sys.argv[3])
        print("absent" if v is None else str(v).strip())
        break
EOF
)
  case $v in absent) ok "$1: $3 absent" ;; ""|*[!0-9-]*) bad "$1: $3 is an integer" "$v" ;; *) ok "$1: $3 is an integer ($v)" ;; esac
}
run xml_read "$D/xml.gguf" "<tool_call>${NL}<function=Read>${NL}" Read
int xml_read Read offset; int xml_read Read limit
run xml_todo "$D/xml.gguf" "<tool_call>${NL}<function=TodoWrite>${NL}" TodoWrite 2
r=$(grep -a "	TodoWrite	" "$D/xml_todo/log" | head -1)
case $r in *"must be an array"*|*"todos["*) bad "xml_todo: TodoWrite accepted the value" "$r" ;; *) ok "xml_todo: TodoWrite value is lines or a valid array" ;; esac
run xml_any "$D/xml.gguf" "<tool_call>${NL}<function=Gr" Grep 3
run json_read "$D/json.gguf" "<tool_call>${NL}{\"name\": \"Read\"" Read
int json_read Read offset; int json_read Read limit
run g4_read "$D/g4.gguf" "<|tool_call>call:Read{" Read
int g4_read Read offset; int g4_read Read limit
# desligada: o mesmo modelo nao fecha a chamada sozinho
mkdir -p "$D/off/w" "$D/off/home"
(cd "$D/off/w" && printf 'go\n/exit\n' | HOME="$D/off/home" KATANA_LOG="$D/off/log" KATANA_CALL_GRAMMAR=0 KATANA_MAXTOK=1000 timeout 600 "$K" agent "$D/xml.gguf" >"$D/off/out" 2>&1)
if grep -q "	Read	" "$D/off/log" 2>/dev/null; then bad "off: no call without the constraint" "$(cat "$D/off/log")"; else ok "off: no call without the constraint"; fi
if [ $F -eq 0 ]; then echo "OK $N checks"; else echo "FAILED $F of $N"; fi
exit $((F != 0))
