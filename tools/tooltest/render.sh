#!/bin/sh
# teste da ferramenta Render sem modelo nem imagem de verdade: KATANA_RENDER_BIN aponta para um executavel de mentira que anota os argumentos
# uso: sh tools/tooltest/render.sh ./katana
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
export KATANA_LOG=${KATANA_LOG:-$(mktemp)}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
export HOME="$T/home"; mkdir -p "$HOME"
N=0; F=0
has() { N=$((N+1)); if printf '%s' "$2" | grep -qF -- "$3"; then echo "ok   $1"; else echo "FAIL $1: wanted [$3] in:"; printf '%s\n' "$2" | sed 's/^/     /'; F=$((F+1)); fi; }
hasnt() { N=$((N+1)); if printf '%s' "$2" | grep -qF -- "$3"; then echo "FAIL $1: unexpected [$3]"; F=$((F+1)); else echo "ok   $1"; fi; }
run() { "$K" tool "$@" 2>/dev/null; }
# executavel de mentira: grava os argumentos (um por linha) em args.txt e escreve a saida pedida em --out; FAKE_RC e o codigo de saida; FAKE_NOOUT nao escreve
cat > fake.sh <<'EOS'
#!/bin/sh
printf '%s\n' "$@" > "$PWD/args.txt"
prev=""; for a in "$@"; do [ "$prev" = "--out" ] && out="$a"; prev="$a"; done
[ -z "$FAKE_NOOUT" ] && [ -n "$out" ] && printf 'PNGDATA' > "$out"
echo "[render] progress line"; echo "[render] done"
exit ${FAKE_RC:-0}
EOS
chmod +x fake.sh
export KATANA_RENDER_BIN="$T/fake.sh"
printf 'x' > view.png; printf 'MATERIAIS\n' > brief.md

o=$(run Render '{}'); has "needs request or brief" "$o" "error: give a request"
o=$(run Render '{"request":"a room","engine":"dalle"}'); has "bad engine" "$o" "error: engine must be stable or flux"
o=$(run Render '{"request":"a room","image":"nope.png"}'); has "missing image" "$o" "error: cannot read the image nope.png"
o=$(run Render '{"brief":"nope.md"}'); has "missing brief" "$o" "error: cannot read the brief file nope.md"
o=$(run Render '{"request":"a room","image":"view.png","out":"view.png"}'); has "never overwrites the view" "$o" "error: out is the same file as image"

o=$(run Render '{"request":"a bright reception","image":"view.png"}'); a=$(cat args.txt)
has "default engine stable, default output next to the image" "$o" "view-render-stable.png"
has "result says rendered" "$o" "rendered with stable:"
has "output file exists" "$(ls)" "view-render-stable.png"
has "args: render generate" "$a" "generate"
has "args: engine stable" "$(printf '%s' "$a" | tr '\n' ' ')" "--engine stable --init view.png --nl a bright reception --out"

o=$(run Render '{"request":"it'"'"'s a \"quote\" $(touch pwned) `id`","image":"view.png","engine":"flux","out":"final.png"}'); a=$(cat args.txt)
has "flux with out" "$o" "final.png"
has "request reaches the engine as one argument, quotes intact" "$a" "it's a \"quote\" \$(touch pwned) \`id\`"
hasnt "no shell injection" "$(ls)" "pwned"

o=$(run Render '{"brief":"brief.md","image":"view.png","engine":"flux","request":"extra: warmer light"}'); a=$(cat args.txt | tr '\n' ' ')
has "brief passed" "$a" "--brief brief.md"
has "extra request kept" "$a" "--nl extra: warmer light"
has "result mentions brief" "$o" "prompt from the brief file"

o=$(run Render '{"request":"no image","engine":"flux"}'); a=$(cat args.txt | tr '\n' ' ')
hasnt "no image: no --init" "$a" "--init"
has "no image: default output in the working folder" "$o" "$T/render-flux.png"

o=$(FAKE_RC=3 run Render '{"request":"x","image":"view.png"}'); has "failed render reports the exit code and the last lines" "$o" "error: render failed (exit code 3): [render] progress line"
rm -f view-render-stable.png
o=$(FAKE_NOOUT=1 run Render '{"request":"x","image":"view.png"}'); has "no file written is an error" "$o" "was not written"
o=$(run ToolSearch '{"query":"select:Render"}'); has "the tool is in the registry (ToolSearch finds it)" "$o" "Render"

echo "$N checks, $F failed"
[ "$F" = 0 ]
