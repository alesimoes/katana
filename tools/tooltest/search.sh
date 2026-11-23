#!/bin/sh
# Testa as ferramentas Glob e Grep (src/tools/filewalk.c, grep.c). Uso: sh tools/tooltest/search.sh ./katana
# Nao compila nada; cria uma pasta temporaria e remove no fim. O Grep usa sempre o motor proprio (src/tools/regex.c;
# a sintaxe em detalhe fica em regex.sh), entao cada verificacao roda uma vez por modo de listagem (git e sem git).
# Tambem cobre as tolerancias do Grep, o .gitignore fora de repositorio git e o tempo de uma busca simples em src/.
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
export KATANA_LOG=${KATANA_LOG:-$(mktemp)}     # o log das chamadas destes testes nao vai para o ~/.katana/agente.log do dono
[ -x "$K" ] || { echo "uso: $0 <binario do katana>"; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
N=0; F=0
chk() {   # nome esperado obtido
    N=$((N+1))
    if [ "$2" = "$3" ]; then echo "ok   $1"; else F=$((F+1)); echo "FAIL $1"; echo "  esperado: $(printf '%s' "$2" | head -c 600)"; echo "  obtido:   $(printf '%s' "$3" | head -c 600)"; fi
}
run() { ( cd "$D" && "$K" tool "$@" 2>&1 ); }     # roda dentro da pasta de teste $D

mkfix() {   # cria a arvore de teste em $D
    mkdir -p "$D/sub/deep" "$D/build" "$D/many" "$D/emptyd"
    printf 'int main() {\n  return 0;\n}\n// TODO one\n' > "$D/a.c"
    printf '#define X 1\n// todo two\n' > "$D/b.h"
    printf 'def f():\n    pass  # TODO three\n' > "$D/sub/c.py"
    printf '{"k": 1}\n' > "$D/sub/deep/d.json"
    printf 'TODO\000binary\n' > "$D/bin.dat"
    printf 'TODO in log\n' > "$D/ignored.log"
    printf 'TODO in build\n' > "$D/build/e.c"
    printf 'alpha\nbeta\ngamma\n' > "$D/notes.txt"
    printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab\n' > "$D/slow.txt"
    printf 'end here\r\nnext\r\n' > "$D/crlf.txt"
    printf '*.log\nbuild/\n' > "$D/.gitignore"
    i=0; while [ $i -lt 105 ]; do : > "$D/many/f$i.txt"; i=$((i+1)); done
    touch -d "2026-01-01 10:00:00" "$D/a.c" "$D/sub/deep/d.json" "$D/notes.txt" "$D/many"/*.txt
    touch -d '2026-01-02 10:00:00' "$D/sub/c.py"
    touch -d '2026-01-03 10:00:00' "$D/b.h"
    touch -d '2026-01-04 10:00:00' "$D/many/f7.txt"
}

# ---------------- Glob ----------------
D=$T/g; mkdir -p "$D"; mkfix; D=$(cd "$D" && pwd -P)
( cd "$D" && git init -q . && git add .gitignore )
chk "Glob *.c so na pasta" "$D/a.c" "$(run Glob '{"pattern":"*.c"}')"
chk "Glob **/*.py profundidade" "$D/sub/c.py" "$(run Glob '{"pattern":"**/*.py"}')"
chk "Glob {c,h} ordem por data (mais novo primeiro)" "$D/b.h
$D/a.c" "$(run Glob '{"pattern":"*.{c,h}"}')"
chk "Glob **/*.{json,py} (c.py mais novo)" "$D/sub/c.py
$D/sub/deep/d.json" "$(run Glob '{"pattern":"**/*.{json,py}"}')"
chk "Glob com pasta no padrao" "$D/sub/deep/d.json" "$(run Glob '{"pattern":"sub/**/*.json"}')"
chk "Glob sem ** nao desce" "No files found" "$(run Glob '{"pattern":"*.json"}')"
chk "Glob classe [ab]" "$D/b.h
$D/a.c" "$(run Glob '{"pattern":"[ab].*"}')"
chk "Glob ? e negacao [!a]" "$D/b.h" "$(run Glob '{"pattern":"[!a].?"}')"
chk "Glob nada" "No files found" "$(run Glob '{"pattern":"**/*.zzz"}')"
chk "Glob path" "$D/sub/c.py" "$(run Glob '{"pattern":"*.py","path":"sub"}')"
chk "Glob path absoluto" "$D/sub/c.py" "$(run Glob "{\"pattern\":\"*.py\",\"path\":\"$D/sub\"}")"
chk "Glob nao pula gitignored" "$D/ignored.log" "$(run Glob '{"pattern":"**/*.log"}')"
chk "Glob pula .git" "No files found" "$(run Glob '{"pattern":"**/HEAD"}')"
chk "Glob path inexistente" "error: path does not exist: nope. Check the spelling, or call Glob on a parent folder to see what is there." "$(run Glob '{"pattern":"*","path":"nope"}')"
chk "Glob path e arquivo" "error: path is not a directory: a.c. Pass the containing folder in \`path\` and the file name in \`pattern\`." "$(run Glob '{"pattern":"*","path":"a.c"}')"
chk "Glob sem pattern" "error: missing \`pattern\`. Example: {\"pattern\": \"**/*.c\"}" "$(run Glob '{}')"
chk "Glob path undefined = padrao" "$D/a.c" "$(run Glob '{"pattern":"*.c","path":"undefined"}')"
R=$(run Glob '{"pattern":"many/*.txt"}')
chk "Glob limite 100: linhas" "101" "$(printf '%s\n' "$R" | wc -l | tr -d ' ')"
chk "Glob limite 100: mais novo primeiro" "$D/many/f7.txt" "$(printf '%s\n' "$R" | head -1)"
chk "Glob limite 100: aviso" "(Results are truncated: showing 100 of 105 matching files. Use a more specific \`pattern\` or \`path\` to narrow the search.)" "$(printf '%s\n' "$R" | tail -1)"
chk "Glob padrao absoluto dentro de path" "$D/sub/c.py" "$(run Glob "{\"pattern\":\"$D/sub/*.py\"}")"
chk "Glob ferramenta antiga continua" "a.c" "$(run glob '{"pattern":"*.c"}' | head -1)"

# ---------------- Grep (mesmas verificacoes numa pasta com git e numa sem git) ----------------
LIM='(Output limited to 1 entries starting at offset 0; more results exist. Use offset=1 to continue, or narrow with path, glob or type.)'
grepsuite() {   # $1 rotulo
    L=$1
    chk "$L arquivos (padrao), mais novo primeiro" "Found 2 files
sub/c.py
a.c" "$(run Grep '{"pattern":"TODO"}')"
    chk "$L -i inclui b.h" "Found 3 files
b.h
sub/c.py
a.c" "$(run Grep '{"pattern":"TODO","-i":true}')"
    chk "$L content com linha" "a.c:4:// TODO one
sub/c.py:2:    pass  # TODO three" "$(run Grep '{"pattern":"TODO","output_mode":"content"}')"
    chk "$L content -n false" "a.c:// TODO one
sub/c.py:    pass  # TODO three" "$(run Grep '{"pattern":"TODO","output_mode":"content","-n":false}')"
    chk "$L -C 1" "a.c-1-int main() {
a.c:2:  return 0;
a.c-3-}" "$(run Grep '{"pattern":"return","output_mode":"content","-C":1}')"
    chk "$L context (alias de -C)" "a.c-1-int main() {
a.c:2:  return 0;
a.c-3-}" "$(run Grep '{"pattern":"return","output_mode":"content","context":1}')"
    chk "$L -A 1" "a.c:2:  return 0;
a.c-3-}" "$(run Grep '{"pattern":"return","output_mode":"content","-A":1}')"
    chk "$L -B 1" "a.c-1-int main() {
a.c:2:  return 0;" "$(run Grep '{"pattern":"return","output_mode":"content","-B":1}')"
    chk "$L separador -- entre grupos" "a.c:1:int main() {
a.c-2-  return 0;
--
a.c:4:// TODO one" "$(run Grep '{"pattern":"main|TODO","path":"a.c","output_mode":"content","-A":1}')"
    chk "$L count" "a.c:1
sub/c.py:1
Found 2 total matches across 2 files." "$(run Grep '{"pattern":"TODO","output_mode":"count"}')"
    chk "$L glob *.py" "Found 1 file
sub/c.py" "$(run Grep '{"pattern":"TODO","glob":"*.py"}')"
    chk "$L glob com chaves" "Found 2 files
b.h
a.c" "$(run Grep '{"pattern":"TODO","-i":true,"glob":"*.{c,h}"}')"
    chk "$L type py" "Found 1 file
sub/c.py" "$(run Grep '{"pattern":"TODO","type":"py"}')"
    chk "$L type h (so b.h)" "Found 1 file
b.h" "$(run Grep '{"pattern":"define","type":"h"}')"
    chk "$L type desconhecido" "error: unknown type \"zzz\". Known types: c, cpp, h, py, js, ts, rust, go, java, sh, md, json, yaml, html, css, qml, toml, txt, xml, sql, rb, php, cs, kt, swift, lua. For anything else use \`glob\`." "$(run Grep '{"pattern":"x","type":"zzz"}')"
    chk "$L sem resultado" "No matches found" "$(run Grep '{"pattern":"zzzzqq"}')"
    chk "$L head_limit 1" "a.c:4:// TODO one
$LIM" "$(run Grep '{"pattern":"TODO","output_mode":"content","head_limit":1}')"
    chk "$L offset 1" "sub/c.py:2:    pass  # TODO three" "$(run Grep '{"pattern":"TODO","output_mode":"content","offset":1}')"
    chk "$L offset alem do fim" "No entries at this offset (the result has 2 entries; offset=99)." "$(run Grep '{"pattern":"TODO","output_mode":"content","offset":99}')"
    chk "$L head_limit 1 em arquivos" "Found 2 files
sub/c.py
$LIM" "$(run Grep '{"pattern":"TODO","head_limit":1}')"
    chk "$L count com head_limit mostra total geral" "a.c:1
$LIM
Found 2 total matches across 2 files." "$(run Grep '{"pattern":"TODO","output_mode":"count","head_limit":1}')"
    chk "$L head_limit 0 sem limite" "Found 3 files
b.h
sub/c.py
a.c" "$(run Grep '{"pattern":"TODO","-i":true,"head_limit":0}')"
    chk "$L multiline" "notes.txt:1:alpha
notes.txt:2:beta" "$(run Grep '{"pattern":"alpha\\nbeta","multiline":true,"output_mode":"content"}')"
    chk "$L multiline (arquivos)" "Found 1 file
notes.txt" "$(run Grep '{"pattern":"alpha.*gamma","multiline":true}')"
    chk "$L sem multiline nao cruza linhas" "No matches found" "$(run Grep '{"pattern":"alpha\\nbeta"}')"
    chk "$L multiline count" "notes.txt:1
Found 1 total match across 1 file." "$(run Grep '{"pattern":"alpha\\nbeta","multiline":true,"output_mode":"count"}')"
    chk "$L arquivo direto" "a.c:2:  return 0;" "$(run Grep '{"pattern":"return","path":"a.c","output_mode":"content"}')"
    chk "$L arquivo ignorado passado direto" "ignored.log:1:TODO in log" "$(run Grep '{"pattern":"TODO","path":"ignored.log","output_mode":"content"}')"
    chk "$L path pasta" "sub/c.py:2:    pass  # TODO three" "$(run Grep '{"pattern":"TODO","path":"sub","output_mode":"content"}')"
    chk "$L chave solta {" "a.c:1:int main() {" "$(run Grep '{"pattern":"main\\(\\) {","output_mode":"content"}')"
    chk "$L \\d" "a.c:2:  return 0;" "$(run Grep '{"pattern":"return \\d","output_mode":"content"}')"
    chk "$L \\w e \\s" "a.c:2:  return 0;" "$(run Grep '{"pattern":"^\\s+return\\s\\w;$","output_mode":"content"}')"
    chk "$L intervalo {2}" "a.c:2:  return 0;" "$(run Grep '{"pattern":"^ {2}ret","output_mode":"content"}')"
    chk "$L padrao invalido vira texto" "[pattern is not a valid regular expression (missing ) to close the group opened at position 1); searched for it as plain text]
No matches found" "$(run Grep '{"pattern":"(abc"}')"
    chk "$L padrao invalido achado como texto" "[pattern is not a valid regular expression (missing ) to close the group opened at position 5); searched for it as plain text]
a.c:1:int main() {" "$(run Grep '{"pattern":"main(","output_mode":"content"}')"
    chk "$L regex sem resultado, texto com" "[the regular expression matched nothing; searched for the pattern as plain text instead (escape . * + ? ( ) [ ] { } | ^ \$ \\ with a backslash to match them literally, or pass \"literal\": true)]
a.c:1:int main() {" "$(run Grep '{"pattern":"main() {","output_mode":"content"}')"
    chk "$L regex com resultado nao tenta texto" "Found 1 file
a.c" "$(run Grep '{"pattern":"main()"}')"
    chk "$L dica de caixa" "No matches found; 1 match when ignoring case: pass \"-i\": true" "$(run Grep '{"pattern":"TODO two"}')"
    chk "$L dica de caixa (contagem)" "No matches found; 3 matches when ignoring case: pass \"-i\": true" "$(run Grep '{"pattern":"Todo"}')"
    chk "$L dica de filtro glob" "No matches found in the files selected by \`glob\`; without that filter there is 1 match in 1 file: .h (1 file). Change or drop the filter." "$(run Grep '{"pattern":"define","glob":"*.c"}')"
    chk "$L dica de filtro type" "No matches found in the files selected by \`type\`; without that filter there are 2 matches in 2 files: .c (1 file), .py (1 file). Change or drop the filter." "$(run Grep '{"pattern":"TODO","type":"json"}')"
    chk "$L path com nome unico" "error: path does not exist: deep. Check the spelling, or call Glob on a parent folder to see what is there. There is exactly one \"deep\" under the working folder: sub/deep" "$(run Grep '{"pattern":"k","path":"deep"}')"
    chk "$L path errado com arquivo unico" "error: path does not exist: x/c.py. Check the spelling, or call Glob on a parent folder to see what is there. There is exactly one \"c.py\" under the working folder: sub/c.py" "$(run Grep '{"pattern":"k","path":"x/c.py"}')"
    chk "$L alias include" "Found 1 file
sub/c.py" "$(run Grep '{"pattern":"TODO","include":"*.py"}')"
    chk "$L alias case_insensitive" "Found 3 files
b.h
sub/c.py
a.c" "$(run Grep '{"pattern":"todo","case_insensitive":true}')"
    chk "$L alias ignore_case" "b.h:2:// todo two" "$(run Grep '{"pattern":"TODO TW","ignore_case":true,"output_mode":"content"}')"
    chk "$L regex false" "a.c:1:int main() {" "$(run Grep '{"pattern":"main() {","regex":false,"output_mode":"content"}')"
    chk "$L fixed_strings" "a.c:1:int main() {" "$(run Grep '{"pattern":"() {","fixed_strings":true,"output_mode":"content"}')"
    chk "$L literal" "No matches found" "$(run Grep '{"pattern":"TODO|define","literal":true}')"
    chk "$L regex como nome do padrao" "Found 1 file
b.h" "$(run Grep '{"regex":"def(ine)?\\b X"}')"
    chk "$L output_mode files" "Found 2 files
sub/c.py
a.c" "$(run Grep '{"pattern":"TODO","output_mode":"files"}')"
    chk "$L output_mode lines" "a.c:4:// TODO one
sub/c.py:2:    pass  # TODO three" "$(run Grep '{"pattern":"TODO","output_mode":"lines"}')"
    chk "$L output_mode matches" "a.c:4:// TODO one
sub/c.py:2:    pass  # TODO three" "$(run Grep '{"pattern":"TODO","output_mode":"matches"}')"
    chk "$L nenhum arquivo passa o glob" "No files were searched: none of the $NALL files under . passes the \`glob\` filter. Change or drop it." "$(run Grep '{"pattern":"x","glob":"*.zzz"}')"
    chk "$L pasta vazia" "No files were searched: emptyd has no files that are not ignored (.git, .gitignore entries, node_modules and similar folders are skipped)." "$(run Grep '{"pattern":"x","path":"emptyd"}')"
    chk "$L lookbehind e classe Unicode" "a.c:4:// TODO one" "$(run Grep '{"pattern":"(?<=// )TODO\\s\\p{Ll}+$","output_mode":"content"}')"
    chk "$L (?i) no padrao" "Found 3 files
b.h
sub/c.py
a.c" "$(run Grep '{"pattern":"(?i)todo"}')"
    chk "$L padrao lento vira erro" "error: pattern too slow on this input; simplify it (it needed more than 10000000 steps at slow.txt:1). Avoid nested quantifiers such as (a+)+ and long .* chains." "$(run Grep '{"pattern":"(a+)+$","path":"slow.txt"}')"
    chk "$L multiline com ^ em cada linha" "notes.txt:2:beta
notes.txt:3:gamma" "$(run Grep '{"pattern":"^beta\\n^gamma$","multiline":true,"output_mode":"content"}')"
    chk "$L CRLF: \$ antes do \\r" "crlf.txt:1:end here" "$(run Grep '{"pattern":"here$","path":"crlf.txt","output_mode":"content"}')"
    chk "$L sem pattern" "error: missing \`pattern\`. Example: {\"pattern\": \"TODO\", \"output_mode\": \"content\"}" "$(run Grep '{}')"
    chk "$L output_mode invalido" "error: output_mode must be \"files_with_matches\", \"content\" or \"count\", not \"zzz\"" "$(run Grep '{"pattern":"x","output_mode":"zzz"}')"
    chk "$L path inexistente" "error: path does not exist: nope. Check the spelling, or call Glob on a parent folder to see what is there." "$(run Grep '{"pattern":"x","path":"nope"}')"
    chk "$L binario ignorado mesmo direto" "No matches found" "$(run Grep '{"pattern":"TODO","path":"bin.dat"}')"
    chk "$L booleano como texto (-i \"true\")" "Found 3 files
b.h
sub/c.py
a.c" "$(run Grep '{"pattern":"TODO","-i":"true"}')"
}
D=$T/g; NALL=$(cd "$D" && git ls-files --cached --others --exclude-standard | wc -l | tr -d ' '); grepsuite "git:"
D=$T/n; mkdir -p "$D"; mkfix; D=$(cd "$D" && pwd -P)
NALL=$(cd "$D" && find . -type f ! -name '*.log' ! -path './build/*' | wc -l | tr -d ' '); grepsuite "walk:"
# ---------------- .gitignore sem git ----------------
D=$T/ig; mkdir -p "$D"; D=$(cd "$D" && pwd -P)
for f in a.log keep.log rootonly.txt sub/rootonly.txt build/x.c sub/build/y.c x/gen/z.c gen/w.c x/gen/z.h docs/a/b/q.tmp \
         docs/q.tmp docs/r.txt '#hash.txt' sub/local.txt local.txt out/keep.txt out/drop.txt; do
    mkdir -p "$D/$(dirname "$f")"; printf 'NEEDLE\n' > "$D/$f"
done
printf '# comentario\n*.log\n!keep.log\n/rootonly.txt\nbuild/\n**/gen/*.c\ndocs/**/*.tmp\n\\#hash.txt\nout/*\n!out/keep.txt\n' > "$D/.gitignore"
printf 'local.txt\n' > "$D/sub/.gitignore"
chk "gitignore: negacao, ancora, pasta, **, comentario, escape" "docs/r.txt:1:NEEDLE
keep.log:1:NEEDLE
local.txt:1:NEEDLE
out/keep.txt:1:NEEDLE
sub/rootonly.txt:1:NEEDLE
x/gen/z.h:1:NEEDLE" "$(run Grep '{"pattern":"NEEDLE","output_mode":"content"}')"
chk "gitignore da pasta acima vale numa subpasta" "sub/rootonly.txt:1:NEEDLE" "$(run Grep '{"pattern":"NEEDLE","path":"sub","output_mode":"content"}')"
chk "pasta ignorada passada direto e lida" "Found 1 file
sub/build/y.c" "$(run Grep '{"pattern":"NEEDLE","path":"sub/build"}')"
chk "gitignore: arquivo ignorado passado direto e lido" "a.log:1:NEEDLE" "$(run Grep '{"pattern":"NEEDLE","path":"a.log","output_mode":"content"}')"
chk "gitignore: ** casa zero pastas" "No files were searched: gen has no files that are not ignored (.git, .gitignore entries, node_modules and similar folders are skipped)." "$(run Grep '{"pattern":"NEEDLE","path":"gen"}')"

# ---------------- tempo: palavra simples em src/ deste repositorio ----------------
R=$(cd "$(dirname "$0")/../.." && pwd)
if [ -d "$R/src" ]; then
    for q in '{"pattern":"rx_search","path":"src"}' '{"pattern":"rx_search","path":"src","-i":true}' '{"pattern":"static\\s+int\\s+\\w+\\(","path":"src","output_mode":"count"}'; do
        t0=$(date +%s%N); o=$(cd "$R" && "$K" tool Grep "$q" 2>&1); t1=$(date +%s%N)
        ms=$(( (t1 - t0) / 1000000 )); echo "     $q: $ms ms ($(printf '%s\n' "$o" | head -1))"
        chk "busca em src/ abaixo de 1 s: $q" "1" "$([ $ms -lt 1000 ] && echo 1 || echo 0)"
    done
fi


echo
if [ $F -eq 0 ]; then echo "OK $N checks"; else echo "FAILED $F"; exit 1; fi
