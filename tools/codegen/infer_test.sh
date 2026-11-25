#!/bin/sh
# Testes da inferencia de tipos do codegen (docs/codegen.md, "Type inference"): compila tools/codegen/codegencli.c, roda cada caso de tools/codegen/infer/cases_*.txt
# e a propriedade de soundness (tools/codegen/infer_prop.py). Formato de um caso:
#   ### nome
#   @ok            sem erros            @fail   com erros
#   @has TEXTO     a saida contem TEXTO (assinatura, let, use, erro, certeza)       @hasnt TEXTO   a saida nao contem
#   (o resto das linhas e o programa code spec)
# Uso: sh tools/codegen/infer_test.sh   (CC=gcc; SAN=1 liga ASan/UBSan; PROP=0 pula a propriedade; NPROP=numero de programas aleatorios)
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
T=$(mktemp -d) || exit 2; trap 'rm -rf "$T"' EXIT
CC=${CC:-gcc}; SANF=""; [ -n "$SAN" ] && SANF="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC -O1 -g -Wall -Wno-unused-function $SANF -I "$ROOT" "$HERE/codegencli.c" -o "$T/codegencli" -lm 2>"$T/cc.log" || { cat "$T/cc.log"; echo "FAIL build"; exit 1; }
[ -s "$T/cc.log" ] && { cat "$T/cc.log"; echo "FAIL build warnings"; exit 1; }
U="$T/codegencli"; N=0; BAD=0; V=${V:-0}
for f in "$HERE"/infer/cases_*.txt; do
  rm -rf "$T/c"; mkdir "$T/c"
  awk -v d="$T/c" '/^### /{n++; name=substr($0,5); print name > (d "/" sprintf("%03d", n) ".name"); prog=d "/" sprintf("%03d", n) ".kcode"; ef=d "/" sprintf("%03d", n) ".exp"; printf "" > prog; printf "" > ef; next}
    /^@/{print substr($0,2) > ef; next} {print > prog}' "$f"
  for nf in "$T"/c/*.name; do
    b=${nf%.name}; name=$(cat "$nf"); N=$((N+1)); out=$("$U" infer "$b.kcode" --lets --uses 2>&1); rc=$?; okc=1; why=""
    while IFS= read -r e; do
      kind=${e%% *}; arg=${e#* }; [ "$kind" = "$e" ] && arg=""
      case "$kind" in
        ok) [ $rc -eq 0 ] || { okc=0; why="expected no errors"; } ;;
        fail) [ $rc -ne 0 ] || { okc=0; why="expected errors"; } ;;
        has) case "$out" in *"$arg"*) ;; *) okc=0; why="missing: $arg" ;; esac ;;
        hasnt) case "$out" in *"$arg"*) okc=0; why="unexpected: $arg" ;; esac ;;
      esac
    done < "$b.exp"
    if [ $okc = 1 ]; then [ "$V" = 1 ] && echo "ok   $name"; else BAD=$((BAD+1)); echo "FAIL $name ($why)"; echo "$out" | grep -v '^--\|^nodes' | head -14 | sed 's/^/     | /'; fi
  done
done
# ---- as 15 tarefas da fase 1: a inferencia ligada ao validador (--infer) aceita todas e os testes dados continuam passando ----
for f in "$HERE"/tasks/*.kcode; do
  b=$(basename "$f" .kcode); N=$((N+1)); out=$("$U" check "$f" --infer 2>&1)
  case "$out" in *ok) ;; *) BAD=$((BAD+1)); echo "FAIL tarefa $b com --infer"; echo "$out" | head -5 | sed 's/^/     | /'; continue ;; esac
  case $(cat "$f") in *raw*) continue ;; esac
  N=$((N+1)); out=$("$U" run python "$f" --infer 2>&1); case "$out" in *" 0 failed"*) ;; *) BAD=$((BAD+1)); echo "FAIL tarefa $b roda com --infer"; echo "$out" | head -5 | sed 's/^/     | /' ;; esac
done
echo "cases: $N, failed: $BAD"
P=0
if [ "${PROP:-1}" != 0 ] && command -v python3 >/dev/null 2>&1; then
  o=$(python3 -I "$HERE/infer_prop.py" "$U" ${NPROP:-300} 2>&1); echo "$o" | tail -12
  case "$o" in *"property: ok"*) ;; *) P=1 ;; esac
fi
[ $BAD -eq 0 ] && [ $P -eq 0 ] && echo "OK infer" || echo "FAILED infer"
exit $((BAD + P))
