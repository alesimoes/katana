# Parte do tools/traintest/run.sh (tambem roda sozinha: sh tools/traintest/gemma4_part.sh, da raiz do repositorio).
# O agente no formato do Gemma 4 com o modelo minusculo (--arch gemma4 --template gemma4: as marcas do Gemma 4 com os
# tipos do modelo de verdade) e o professor de mentira no lugar do modelo. Os mesmos cenarios do ChatML: professor
# teimoso, pre-requisito de fora, plano antes de executar, limite de rodadas com e sem progresso, lembrete da lista;
# e os mecanismos portados: compactacao ao vivo dentro de um pedido longo e no /compact, nota de leitura no lugar do
# texto bruto, subagente. Conferencias do texto contra o template do modelo (tools/traintest/gemma4_golden.py) quando
# o GGUF do Gemma 4 esta em models/ (so o cabecalho e lido) e o jinja2 esta instalado.
if [ -z "$K" ]; then
  set -e
  K=$(pwd)/katana
  D=$(mktemp -d)
  [ -n "$G4_KEEP" ] || trap 'rm -rf "$D"' EXIT
  fail=0; G4_ALONE=1
fi
R4=$(pwd)
MOCK="$R4/tools/traintest/mock_teacher_prog.py"
GG=${G4_GGUF:-"$R4/models/gemma-4-26B-A4B-it-UD-Q4_K_M.gguf"}
python3 tools/tinymodel/make_tiny_gguf.py "$D/g4agent.gguf" --arch gemma4 --template gemma4 --no-eos >/dev/null
g4run() {     # g4run <pasta> <entrada> VAR=valor... : o agente no gemma4 minusculo com o professor de mentira
  _dir=$1; _in=$2; shift 2
  mkdir -p "$D/$_dir"
  (cd "$D/$_dir" && printf "$_in" | HOME="$D/home" KATANA_CPU=1 KATANA_TEACHER=1 KATANA_TOOLS=2 MOCK_TOOLS=2 \
     KATANA_TEACHER_CMD="python3 $MOCK" MOCK_TEACHER_LOG="$D/$_dir.jsonl" "$@" $K agent "$D/g4agent.gguf" >"$D/$_dir.out" 2>"$D/$_dir.err") || true
}
g4ok() { echo "gemma4: $1: ok"; }
g4bad() { echo "gemma4: $1: FALHOU"; shift; for f in "$@"; do grep -a "FALHA\|ok\": false" "$f" 2>/dev/null | cut -c1-500 | head -3; done; fail=1; }

# professor teimoso: a 2a leva o aviso, a 3a e a 4a nao rodam, a 4a ja manda encerrar
g4run g4w5 'leia o arquivo\n/exit\n' MOCK_INSIST=1
if grep -q "Insistencia contida." "$D/g4w5.out" && [ "$(grep -c '"ok": true' "$D/g4w5.jsonl")" = 5 ]; then g4ok "insistencia no erro"; else g4bad "insistencia no erro" "$D/g4w5.out"; fi
# pre-requisito de fora: nota na 3a falha em volta de fakesdk
mkdir -p "$D/g4w6"
printf '#!/bin/sh\n[ "$1" = --help ] && { echo "usage: fakesdk build|install|auth"; exit 0; }\necho "Error: Operation unsupported: the SDK is not installed (requires login)"; exit 1\n' >"$D/g4w6/fakesdk"; chmod +x "$D/g4w6/fakesdk"
g4run g4w6 'faca o app\n/exit\n' MOCK_BLOCKED=1
if grep -q "Bloqueio relatado e codigo escrito." "$D/g4w6.out" && [ -f "$D/g4w6/app.txt" ]; then g4ok "pre-requisito de fora"; else g4bad "pre-requisito de fora" "$D/g4w6.out"; fi
# limite de rodadas (KATANA_ROUNDS=4): com progresso passa do bloco; sem progresso a nota vem na 3a rodada
g4run g4w71 'trabalhe\n/exit\n' KATANA_ROUNDS=4 MOCK_ROUNDS=1
g4run g4w72 'trabalhe\n/exit\n' KATANA_ROUNDS=4 MOCK_ROUNDS=2
if grep -q "Dez arquivos gravados sem parar." "$D/g4w71.out" && [ "$(ls "$D/g4w71" | wc -l)" = 10 ] && grep -q "Limite relatado." "$D/g4w72.out"; then g4ok "limite de rodadas"
else g4bad "limite de rodadas" "$D/g4w71.out" "$D/g4w72.out"; fi
# lembrete da lista e mensagem nova que muda o plano (turnos do usuario no formato do Gemma 4)
g4run g4w8 'faca a lista\nesqueca a lista, faca outra coisa\n/exit\n' MOCK_REDIRECT=1 KATANA_CTX_DUMP="$D/g4w8.ctx"
if grep -q "Lembrete ok." "$D/g4w8.out" && grep -q "Plano novo." "$D/g4w8.out" && ! grep -q "FALHA" "$D/g4w8.out" \
   && grep -q "<turn|>
<|turn>user
\[katana: your todo list still has unfinished steps\]" "$D/g4w8.ctx"; then g4ok "lembrete da lista (turno do usuario)"
else g4bad "lembrete da lista" "$D/g4w8.out"; fi
# planejar antes de executar: nota na 3a e na 6a chamada sem lista; passo atual na 5a com lista
g4run g4w91 'trabalhe\n/exit\n' KATANA_UI=1 MOCK_PLAN=1
g4run g4w92 'trabalhe\n/exit\n' KATANA_UI=1 MOCK_PLAN=2
if grep -q "Plano cobrado." "$D/g4w91.out" && grep -q "Plano seguido." "$D/g4w92.out"; then g4ok "plano antes de executar"; else g4bad "plano antes de executar" "$D/g4w91.out" "$D/g4w92.out"; fi
# subagente: o professor chama Agent; o subagente roda no modelo minusculo (contexto limpo, chamadas nativas) e o
# contexto principal volta do disco
g4run g4wd 'procure\n/exit\n' MOCK_DELEGATE=1 KATANA_TEACHER_FEED=1 KATANA_CTX=24000 KATANA_SIDE_DUMP="$D/g4wd.side"
if grep -q "Subagente ok." "$D/g4wd.out" && grep -q "contexto principal carregado de volta" "$D/g4wd.err" \
   && grep -q "^<|turn>system" "$D/g4wd.side" && grep -q "<turn|>
<|turn>user
\[You are a sub-agent of katana" "$D/g4wd.side"; then g4ok "subagente (Agent)"
else g4bad "subagente" "$D/g4wd.out"; grep -a "subagente" "$D/g4wd.err" | head -3; fi
# KATANA_G4_SIDE=0: o subagente volta a ser recusado no Gemma 4
g4run g4wd0 'procure\n/exit\n' MOCK_DELEGATE=1 KATANA_G4_SIDE=0
if grep -q "delegate is not available for this model format" "$D/g4wd0.out"; then g4ok "KATANA_G4_SIDE=0 desliga o subagente"; else g4bad "KATANA_G4_SIDE=0" "$D/g4wd0.out"; fi
# nota de leitura: read_file de um arquivo grande vira nota escrita num contexto limpo (primeiro conjunto de ferramentas)
python3 -c "open('$D/g4big.txt','w').write(''.join('linha %d: %s\n' % (i, 'x'*60) for i in range(1,301)))"
python3 $R4/plugins/teacher-anthropic/test/mock_api.py 8203 "$D/g4rd.jsonl" "$D/g4big.txt" & G4RM=$!
sleep 1
mkdir -p "$D/g4w2"
(cd "$D/g4w2" && printf 'leia o arquivo\n/exit\n' | env HOME="$D/home" KATANA_CPU=1 KATANA_TOOLS=1 KATANA_DIGEST=2 KATANA_TEACHER_FEED=1 KATANA_TEACHER=1 KATANA_TEACHER_KEY=sk-mock-key KATANA_TEACHER_CMD=$R4/plugins/teacher-anthropic/run \
   KATANA_CTX=24000 KATANA_SIDE_DUMP="$D/g4w2.side" KATANA_TEACHER_URL=http://127.0.0.1:8203 $K agent "$D/g4agent.gguf" >/dev/null 2>"$D/g4w2.err") || true
kill $G4RM 2>/dev/null || true
if [ "$(grep -c 'condensed note of lines' "$D/g4rd.jsonl")" -ge 1 ] && grep -q "^=====digest" "$D/g4w2.side" \
   && grep -q "<turn|>
<|turn>model
<|channel>thought
<channel|>" "$D/g4w2.side"; then g4ok "nota de leitura no lugar do texto bruto"
else g4bad "nota de leitura"; grep -a "nota\|lendo" "$D/g4w2.err" | head -3; fi
# compactacao ao vivo num pedido longo (contexto pequeno) e no /compact: o resumo sai do cache, sem reler a conversa
g4run g4wc 'trabalhe longo\n/compact\n/exit\n' MOCK_LONG=1 KATANA_TEACHER_FEED=1 KATANA_CTX=16000 KATANA_COMPACT_DUMP="$D/g4wc.cdump" KATANA_CTX_DUMP="$D/g4wc.ctx"
if grep -q "Longo pronto." "$D/g4wc.out" && grep -q "o modelo resume o trabalho feito ate aqui" "$D/g4wc.err" && grep -q "contexto compactado" "$D/g4wc.err" \
   && grep -q "resumo feito a partir do cache" "$D/g4wc.err" && [ "$(grep -c '^=====$' "$D/g4wc.cdump")" -ge 2 ]; then g4ok "compactacao ao vivo (pedido longo e /compact)"
else g4bad "compactacao ao vivo" "$D/g4wc.out"; grep -a "compact\|resum" "$D/g4wc.err" | head -5; fi
g4run g4wc0 'trabalhe longo\n/exit\n' MOCK_LONG=1 KATANA_TEACHER_FEED=1 KATANA_CTX=16000 KATANA_COMPACT_LIVE=0
if grep -q "Longo pronto." "$D/g4wc0.out" && ! grep -q "o modelo resume o trabalho feito ate aqui" "$D/g4wc0.err" && grep -q "contexto compactado" "$D/g4wc0.err"; then g4ok "KATANA_COMPACT_LIVE=0 volta ao resumo num prompt novo"
else g4bad "KATANA_COMPACT_LIVE=0" "$D/g4wc0.out"; fi
# raciocinio ligado: pedido longo com compactacao e conversa curta; os turnos tem de ficar bem formados
g4run g4wt 'trabalhe longo\n/exit\n' MOCK_LONG=1 KATANA_TEACHER_FEED=1 KATANA_CTX=16000 KATANA_THINK=1 KATANA_CTX_DUMP="$D/g4wt.ctx"
if grep -q "Longo pronto." "$D/g4wt.out" && grep -q "contexto compactado" "$D/g4wt.err" && python3 - "$D/g4wt.ctx" <<'EOF'
import re, sys
t = open(sys.argv[1], errors="replace").read()
# canal de pensamento: cada abertura fecha antes da proxima abertura, de uma chamada ou do fim do turno
ok = t.startswith("<|turn>system\n<|think|>\n")
for seg in re.split(r"<\|channel>", t)[1:]:
    ok = ok and seg.startswith("thought\n") and "<channel|>" in seg.split("<|tool_call>")[0].split("<turn|>")[0]
sys.exit(0 if ok else 1)
EOF
then g4ok "raciocinio ligado: turnos bem formados com compactacao"; else g4bad "raciocinio ligado" "$D/g4wt.out"; tail -c 600 "$D/g4wt.ctx"; fi
# conferencia do texto contra o template do modelo de verdade (so o cabecalho do GGUF)
if [ -f "$GG" ] && python3 -c "import jinja2" 2>/dev/null; then
  for T in 2 1; do
    (cd "$D/g4w6" && HOME="$D/home" KATANA_TOOLS=$T $K agentsys json >"$D/g4js$T.txt" 2>/dev/null && HOME="$D/home" KATANA_TOOLS=$T $K agentsys gemma4 >"$D/g4s$T.txt" 2>/dev/null) || true
    for th in 0 1; do
      if R=$(python3 tools/traintest/gemma4_golden.py sys "$GG" "$D/g4js$T.txt" "$D/g4s$T.txt" $th); then g4ok "template: turno do sistema (conjunto $T, raciocinio $th): $R"
      else echo "gemma4: template: turno do sistema (conjunto $T, raciocinio $th): FALHOU"; echo "$R"; fail=1; fi
    done
  done
  for th in 0 1; do
    g4run g4wg$th 'grave e confira g.txt\nobrigado\n/exit\n' MOCK_GOLD=1 KATANA_THINK=$th KATANA_CTX_DUMP="$D/g4wg$th.ctx"
    (cd "$D/g4wg$th" && HOME="$D/home" KATANA_TOOLS=2 $K agentsys json >"$D/g4wgj.txt" 2>/dev/null) || true
    python3 - "$D/g4wg$th.ctx" "$D/g4wg$th.sysx" <<'EOF'
import sys
t = open(sys.argv[1], errors="replace").read()
a = t.index("\n") + 1
if t.startswith("<|turn>system\n<|think|>\n"): a += len("<|think|>\n")
open(sys.argv[2], "w").write(t[a:t.index("<turn|>\n<|turn>user\n")])
EOF
    if R=$(python3 tools/traintest/gemma4_golden.py conv "$GG" "$D/g4wgj.txt" "$D/g4wg$th.sysx" $th "$D/g4wg$th.ctx"); then g4ok "template: conversa (raciocinio $th): $R"
    else echo "gemma4: template: conversa (raciocinio $th): FALHOU"; echo "$R"; fail=1; fi
  done
  (cd "$D/g4wc" && HOME="$D/home" KATANA_TOOLS=2 $K agentsys json >"$D/g4wcj.txt" 2>/dev/null) || true
  if R=$(python3 tools/traintest/gemma4_golden.py compact "$GG" "$D/g4wcj.txt" "$D/g4wc.cdump"); then g4ok "template: pedido de compactacao ao vivo: $R"
  else echo "gemma4: template: pedido de compactacao ao vivo: FALHOU"; echo "$R"; fail=1; fi
else
  echo "gemma4: template: pulado (sem $GG ou sem jinja2)"
fi
if [ -n "$G4_ALONE" ]; then [ "$fail" = 0 ] && echo "gemma4: tudo ok" || { echo "gemma4: houve falhas"; exit 1; }; fi
