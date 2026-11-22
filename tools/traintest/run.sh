#!/bin/sh
# Teste do treino do katana sem modelo de verdade (segundos, qualquer maquina):
#  1. gradcheck nos layouts do modelo minusculo: llama; qwen2 (GQA, vies); base quantizada Q8_0; qwen3 (norma de
#     q/k por cabeca); qwen3moe com compartilhado e camada densa inicial; olmoe (norma no vetor, MoE sem renormalizar);
#     qwen35moe (layout do Qwen3.6: Gated DeltaNet + atencao com portao + MoE) e qwen35 denso; gemma4 (layout do
#     Gemma 4 26B-A4B: janela deslizante, normas extras, GELU, FFN denso em paralelo com o MoE, softcap), denso e Q8_0
#  2. treino com tensores inteiros, com adaptadores sobre a base quantizada e com especialistas quentes num MoE
#  3. o servidor com KATANA_SPEC responde exatamente o que foi treinado (temperatura 0)
#  4. katana import: conversas de fora viram dados de treino; as chamadas voltam pelos leitores do agente
#  5. katana merge: o GGUF fundido responde igual ao base com o .kspec
#  6. katana distill: um professor (de mentira, aqui) resolve uma tarefa do codebench com as ferramentas do katana
#  7. treino na GPU: o codigo do shader do produto roda na CPU (7 formatos, 3 modos) e numa GPU de mentira dentro do
#     katana (gradcheck com todos os produtos nela); se houver GPU Vulkan de verdade, gradcheck nela em 5 layouts
#  8. validacao: 60 conversas de "repetir a palavra", 10 separadas; lote de 4, a perda nelas tem que cair (o modelo
#     generaliza) e a parada antecipada guarda o melhor passo
#  9. katana export (sessao real do agente com acentos e ideogramas: igual ao exportador em Python) e katana learn
#     com tarefas de mentira no katana codebench (tools/traintest/mock_bench): coleta, filtro, treino, prova; a rodada 1
#     tem que ser aceita e a 2 nao
# 10. modo professor do agente (/teacher) com uma API de mensagens de mentira: pedidos aceitos, sessao retomada com o
#     professor ligado no meio, katana export --teacher
# 11. professor pelo programa claude (de mentira) sem chave; read_file so com o caminho continua de onde parou, cada
#     trecho vira nota e o arquivo ja lido devolve as notas
# 12. katana import com selecao (--train-tools, --verified, --plan-first...) e katana specialize nos tres tipos
# 13. a noite em um comando (katana.cfg), gravacao que falha em voz alta (disco cheio simulado e /dev/full), conferencia dos dados,
#     --wait-idle, --status e katana knowledge quiz
# 13. katana curriculum: conversas das convencoes geradas, conferidas (check) e importadas em ChatML JSON/XML e Gemma 4
# 13. o comando katana: console na pasta atual e modo headless (tools/tooltest/launch.sh: escolha do modelo, install, sessoes por
#     pasta, adopt, lista de ferramentas, katana -p em text/json/stream-json, MCP por --mcp-config, console num terminal de mentira)
# Uso: sh tools/traintest/run.sh   (a partir da raiz do repositorio, com ./katana compilado)
set -e
export KATANA_TOOLS=1      # os passos antigos usam o primeiro conjunto de ferramentas; o passo do segundo conjunto pede 2 (o padrao)
K=$(pwd)/katana
D=$(mktemp -d)
trap 'rm -rf "$D"; kill $SRV 2>/dev/null || true' EXIT
python3 tools/tinymodel/make_tiny_gguf.py "$D/llama.gguf" >/dev/null
python3 tools/tinymodel/make_tiny_gguf.py "$D/qwen2.gguf" --arch qwen2 --kv 2 >/dev/null
$K quantize "$D/llama.gguf" "$D/q8.gguf" Q8_0 >/dev/null 2>&1
python3 tools/tinymodel/make_tiny_gguf.py "$D/qwen3.gguf" --arch qwen3 >/dev/null
python3 tools/tinymodel/make_tiny_gguf.py "$D/moe.gguf" --arch qwen3moe --shared --dense 1 --kv 2 >/dev/null
python3 tools/tinymodel/make_tiny_gguf.py "$D/olmoe.gguf" --arch olmoe --used 3 >/dev/null
python3 tools/tinymodel/make_tiny_gguf.py "$D/q35.gguf" --arch qwen35moe --kv 2 >/dev/null
python3 tools/tinymodel/make_tiny_gguf.py "$D/q35d.gguf" --arch qwen35 --kv 2 >/dev/null
python3 tools/tinymodel/make_tiny_gguf.py "$D/g4.gguf" --arch gemma4 >/dev/null
python3 tools/tinymodel/make_tiny_gguf.py "$D/g4d.gguf" --arch gemma4 --dense 2 >/dev/null
$K quantize "$D/g4.gguf" "$D/g4q8.gguf" Q8_0 >/dev/null 2>&1
fail=0
for m in llama qwen2 q8 qwen3 moe olmoe q35 q35d g4 g4d g4q8; do
  if KATANA_TRAIN_GPU=0 $K gradcheck "$D/$m.gguf" 12 2>/dev/null | grep -q "gradcheck: ok"; then echo "gradcheck $m: ok"; else echo "gradcheck $m: FALHOU"; fail=1; fi
done
# treino na GPU (vk/shaders/tmm.comp)
gcc -O1 -mavx2 -mfma -mf16c -w -I . -I src -I vk/include tools/traintest/tmm_ref.c -o "$D/tmm_ref" -lm -lpthread -ldl && "$D/tmm_ref" "$D/tmm_cases.bin"
if command -v g++ >/dev/null 2>&1; then
  g++ -std=c++20 -O1 -pthread -I vk/shaders tools/traintest/tmm_emu.cpp -o "$D/tmm_emu" && "$D/tmm_emu" "$D/tmm_cases.bin" | tail -1 | grep -q "tmm_emu: ok" \
    && echo "shader do treino na CPU (7 formatos x 3 modos): ok" || { echo "shader do treino: FALHOU"; fail=1; }
  g++ -std=c++20 -O2 -c -I vk/shaders tools/traintest/tg_emu.cpp -o "$D/tg_emu.o" \
    && gcc -O2 -mavx2 -mfma -mf16c -ffast-math -w -DKATANA_TG_EMU -I . -I src -I vk/include src/katana.c "$D/tg_emu.o" -o "$D/katana_emu" -lm -lpthread -ldl -lstdc++
  for e in "" "KATANA_TRAIN_VRAM=0 KATANA_TRAIN_GPU_CHUNK=0.004"; do
    if env KATANA_TRAIN_GPU_MIN=0 $e "$D/katana_emu" gradcheck "$D/llama.gguf" 12 2>/dev/null | grep -q "gradcheck: ok"; then echo "gradcheck na GPU de mentira ${e:+(pesos em fatias de 4 KB)}: ok"; else echo "gradcheck na GPU de mentira $e: FALHOU"; fail=1; fi
  done
else echo "sem g++: testes do shader do treino pulados"; fi
if KATANA_TRAIN_GPU_MIN=0 $K gradcheck "$D/llama.gguf" 4 2>&1 | grep -q "treino na GPU"; then
  for m in llama q8 moe q35 g4; do
    if KATANA_TRAIN_GPU_MIN=0 $K gradcheck "$D/$m.gguf" 12 2>/dev/null | grep -q "gradcheck: ok"; then echo "gradcheck na GPU $m: ok"; else echo "gradcheck na GPU $m: FALHOU"; fail=1; fi
  done
else echo "sem GPU Vulkan: gradcheck na GPU de verdade pulado"; fi
python3 - "$D/sft.jsonl" "$D/sft_think.jsonl" <<'EOF'
import json, sys
sys_ = "<|im_start|>system\nYou are katana.<|im_end|>\n"
for out, think in ((sys.argv[1], False), (sys.argv[2], True)):     # think: o modelo abre a resposta com <think> (qwen35)
    rows = []
    for q, a in [("what is katana?", "katana is a local engine."), ("who made it?", "Ale made it in C."), ("say hi", "hi there!")]:
        pre = sys_ + "<|im_start|>user\n" + q + "<|im_end|>\n<|im_start|>assistant\n" + ("<think>\n" if think else "")
        text = pre + ("\n</think>\n\n" if think else "") + a + "<|im_end|>"
        rows.append({"text": text, "train_spans": [[len(pre), len(text)]]})
    open(out, "w").write("\n".join(json.dumps(r) for r in rows) + "\n")
EOF
check_spec() {   # modelo, especializacao, porta
  KATANA_SPEC="$2" $K serve "$1" "$3" >"$D/srv.log" 2>&1 & SRV=$!
  # espera o servidor responder (ate 30 s): com a maquina carregada 1 s fixo nao bastava e o passo falhava sem aviso
  python3 -c "
import sys, time, urllib.request
for _ in range(150):
    try: urllib.request.urlopen('http://127.0.0.1:%s/health' % sys.argv[1], timeout=2); break
    except Exception: time.sleep(0.2)
" "$3"
  python3 - "$3" <<'EOF' || fail=1
import json, sys, urllib.request
want = {"what is katana?": "katana is a local engine.", "who made it?": "Ale made it in C.", "say hi": "hi there!"}
bad = 0
for q, a in want.items():
    body = {"messages": [{"role": "system", "content": "You are katana."}, {"role": "user", "content": q}], "temperature": 0, "max_tokens": 40}
    r = urllib.request.Request("http://127.0.0.1:%s/v1/chat/completions" % sys.argv[1], data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    got = json.load(urllib.request.urlopen(r))["choices"][0]["message"]["content"]
    if got != a: bad += 1; print("  %r -> %r (esperado %r)" % (q, got, a))
print("  respostas treinadas: %d de %d" % (len(want) - bad, len(want)))
sys.exit(1 if bad else 0)
EOF
  kill $SRV; wait $SRV 2>/dev/null || true
}
$K train "$D/llama.gguf" "$D/sft.jsonl" --steps 150 --lr 3e-3 --seq 128 --out "$D/full.kspec" 2>/dev/null | grep "perda media final"
check_spec "$D/llama.gguf" "$D/full.kspec" 8191
$K train "$D/q8.gguf" "$D/sft.jsonl" --train "blk.*,output_norm.weight" --rank 8 --steps 400 --lr 4e-3 --seq 128 --out "$D/adapt.kspec" 2>/dev/null | grep "perda media final"
check_spec "$D/q8.gguf" "$D/adapt.kspec" 8192
# KATANA_SPEC com uma pasta: aplica os .kspec deste modelo base e pula o de outro (full.kspec e do llama.gguf)
mkdir -p "$D/specs" && cp "$D/adapt.kspec" "$D/full.kspec" "$D/specs/"
check_spec "$D/q8.gguf" "$D/specs" 8203
if grep -q "especializacoes de .*: 1 aplicadas de 2" "$D/srv.log"; then echo "pasta de especializacoes: ok (1 aplicada, 1 de outro modelo pulada)"; else echo "pasta de especializacoes: FALHOU"; fail=1; fi
$K train "$D/moe.gguf" "$D/sft.jsonl" --experts 2 --train "blk.*.attn_*,blk.*.ffn_gate_inp.weight,blk.*shexp*,output_norm.weight" --rank 8 \
   --steps 500 --lr 5e-3 --seq 128 --out "$D/moe.kspec" 2>/dev/null | grep -E "quentes|perda media final"
check_spec "$D/moe.gguf" "$D/moe.kspec" 8193
$K train "$D/q35.gguf" "$D/sft_think.jsonl" --rank 8 --train "blk.*,output_norm.weight" --steps 400 --lr 5e-3 --seq 128 \
   --out "$D/q35.kspec" 2>/dev/null | grep "perda media final"
check_spec "$D/q35.gguf" "$D/q35.kspec" 8194
$K train "$D/g4.gguf" "$D/sft.jsonl" --experts 2 --train "blk.*.attn_*,blk.*.ffn_gate_inp.*,blk.*norm*,output_norm.weight" --rank 8 \
   --steps 500 --lr 5e-3 --seq 128 --out "$D/g4.kspec" 2>/dev/null | grep "perda media final"
check_spec "$D/g4.gguf" "$D/g4.kspec" 8199
# validacao separada por conversa, lote com gradiente acumulado, melhor passo e parada antecipada
python3 - "$D/copy.jsonl" <<'EOF'
import json, sys, random
random.seed(5)
sys_ = "<|im_start|>system\nYou are katana.<|im_end|>\n"
syl = ["ka", "ta", "na", "ro", "mi", "ze", "lu", "po", "fe", "di", "su", "bo"]
rows = []
for i in range(60):
    w = "".join(random.choice(syl) for _ in range(3))
    pre = sys_ + "<|im_start|>user\nrepeat " + w + "<|im_end|>\n<|im_start|>assistant\n"
    text = pre + w + " " + w + "<|im_end|>"
    rows.append({"text": text, "train_spans": [[len(pre), len(text)]]})
open(sys.argv[1], "w").write("\n".join(json.dumps(r) for r in rows) + "\n")
EOF
$K train "$D/llama.gguf" "$D/copy.jsonl" --batch 4 --steps 400 --lr 3e-3 --seq 64 --eval 25 --patience 4 --log "$D/log.jsonl" --out "$D/copy.kspec" 2>/dev/null \
  | grep "^resultado:" | sed 's/^resultado: //' > "$D/res.json"
python3 - "$D/res.json" "$D/log.jsonl" <<'EOF' || fail=1
import json, sys
r = json.load(open(sys.argv[1])); log = [json.loads(l) for l in open(sys.argv[2])]
first = next(x["val_loss"] for x in log if "val_loss" in x)
ok = r["val_lines"] == 10 and r["val_loss"] is not None and r["val_loss"] < 0.3 * first
print("validacao: perda nas conversas separadas %.3f -> %.3f: %s" % (first, r["val_loss"], "ok" if ok else "FALHOU"))
sys.exit(0 if ok else 1)
EOF
$K train "$D/llama.gguf" "$D/copy.jsonl" --batch 4 --steps 400 --lr 0.05 --sched const --seq 64 --eval 20 --patience 3 --out "$D/bad.kspec" 2>/dev/null \
  | grep -q "parada antecipada" && echo "parada antecipada: ok" || { echo "parada antecipada: FALHOU"; fail=1; }
# katana export: sessao do agente -> dados (mesmos trechos que o exportador em Python, com texto nao ASCII)
python3 tools/tinymodel/make_tiny_gguf.py "$D/agent.gguf" --template chatml-xml >/dev/null
mkdir -p "$D/home"
printf 'oi, tudo bem? \303\247\303\243 \346\227\245\346\234\254\n/exit\n' | HOME="$D/home" $K agent "$D/agent.gguf" --sessao "$D/s1.ksess" >/dev/null 2>&1 || true
$K export "$D/exp_c.jsonl" "$D/s1.ksess" >/dev/null 2>&1
python3 tools/dataset/ksess2jsonl.py "$D/s1.ksess" -o "$D/exp_p.jsonl" >/dev/null 2>&1
python3 - "$D/exp_c.jsonl" "$D/exp_p.jsonl" <<'EOF' || fail=1
import json, sys
c = json.loads(open(sys.argv[1]).readline()); p = json.loads(open(sys.argv[2]).readline())
a, b = c["train_spans"][-1]
ok = c["train_spans"] == p["train_spans"] and c["text"][:a].endswith("assistant\n") and c["text"][a:b].endswith("<|im_end|>")
print("export: %s" % ("ok" if ok else "FALHOU %s %s" % (c["train_spans"], p["train_spans"])))
sys.exit(0 if ok else 1)
EOF
# katana learn com o codebench de mentira
# as tarefas do codebench de mentira (t1..t8: repo vazio, teste escondido que decide pelo nome e por KATANA_SPEC) sao montadas aqui
for t in t1 t2 t3 t4 t5 t6 t7 t8; do
  mkdir -p "$D/bench/tasks/$t/repo" "$D/bench/tasks/$t/hidden"
  echo "task $t: say ok" > "$D/bench/tasks/$t/instruction.md"; : > "$D/bench/tasks/$t/repo/.keep"
  sed "s/@TASK@/$t/" tools/traintest/mock_bench/test_hidden.py > "$D/bench/tasks/$t/hidden/test_hidden_mock.py"
done
$K learn "$D/agent.gguf" --bench "$D/bench" --dir "$D/learn" --rounds 2 --repeat 2 -- --steps 30 --seq 256 >/dev/null 2>&1
python3 - "$D/learn/learn.jsonl" <<'EOF' || fail=1
import json, sys
r = [json.loads(l) for l in open(sys.argv[1])]
ok = len(r) == 3 and r[1]["accepted"] and not r[2]["accepted"] and r[1]["data"] == 6 and r[2]["data"] == 12 and r[1]["train"]["steps"] == 30
print("learn: %s" % ("ok (rodada 1 aceita, rodada 2 recusada, dados acumulados 6 -> 12)" if ok else "FALHOU %s" % r))
sys.exit(0 if ok else 1)
EOF
# modo professor do agente: a API de mensagens de mentira (tools/traintest/mock_anthropic.py) recusa pedido fora das
# regras; o professor grava um arquivo, confere e responde; depois a sessao e retomada com o professor ligado no
# meio da conversa (historico refeito a partir da sessao) e o export --teacher marca so as falas dele
python3 tools/traintest/mock_anthropic.py 8199 "$D/tch_req.jsonl" & TMOCK=$!
sleep 1
mkdir -p "$D/tw"
TENV="HOME=$D/home KATANA_CPU=1 KATANA_MAXTOK=24 ANTHROPIC_API_KEY=sk-mock-key KATANA_TEACHER_URL=http://127.0.0.1:8199"
(cd "$D/tw" && printf 'grave uma nota em nota.txt\n/exit\n' | env $TENV KATANA_TEACHER=1 $K agent "$D/agent.gguf" --sessao "$D/tch.ksess" >/dev/null 2>&1) || true
(cd "$D/tw" && printf 'diga oi\n/teacher on\ngrave a nota de novo\n/exit\n' | env $TENV $K agent "$D/agent.gguf" --retomar "$D/tch.ksess" >/dev/null 2>&1) || true
kill $TMOCK 2>/dev/null || true
$K export "$D/tch.jsonl" "$D/tch.ksess" --teacher >/dev/null 2>&1 || true
python3 - "$D/tch_req.jsonl" "$D/tch.jsonl" "$D/tw/nota.txt" <<'EOF' || fail=1
import json, sys
try:
    reqs = [json.loads(l) for l in open(sys.argv[1])]
    t = json.loads(open(sys.argv[2]).readline())
    ok = len(reqs) == 6 and all(r["status"] == 200 for r in reqs) and len(t["train_spans"]) == 6 and open(sys.argv[3]).read().startswith("linha 1\n")
    why = [(r["status"], r["why"]) for r in reqs if r["status"] != 200]
except Exception as e:
    ok, why = False, e
print("professor: %s" % ("ok (6 pedidos aceitos, arquivo gravado, 6 falas do professor nos dados)" if ok else "FALHOU %s" % (why,)))
sys.exit(0 if ok else 1)
EOF
# professor pelo Claude Code da maquina (programa de mentira no lugar do claude) e leitura com posicao guardada:
# o professor le 5 vezes o mesmo arquivo so com o caminho; 4 trechos em sequencia, cada um vira nota, a 5a devolve as notas
mkdir -p "$D/tw2"
(cd "$D/tw2" && printf 'grave uma nota em nota.txt\n/exit\n' | env -u ANTHROPIC_API_KEY HOME="$D/home" KATANA_CPU=1 KATANA_TEACHER=1 \
   KATANA_TEACHER_CLI="$OLDPWD/tools/traintest/mock_claude.py" MOCK_CLAUDE_LOG="$D/cli.jsonl" $K agent "$D/agent.gguf" >"$D/cli_out.txt" 2>/dev/null) || true
# insistencia no erro: o professor repete a mesma chamada que falha; a 2a leva um aviso, a 3a e a 4a nao rodam, e a 4a
# ja leva a ordem de encerrar e relatar ao usuario
mkdir -p "$D/tw5"
(cd "$D/tw5" && printf 'leia o arquivo\n/exit\n' | env -u ANTHROPIC_API_KEY HOME="$D/home" KATANA_CPU=1 KATANA_TEACHER=1 KATANA_TOOLS=2 MOCK_INSIST=1 MOCK_TOOLS=2 \
   KATANA_TEACHER_CLI="$OLDPWD/tools/traintest/mock_claude.py" MOCK_CLAUDE_LOG="$D/cli5.jsonl" $K agent "$D/agent.gguf" >"$D/cli5_out.txt" 2>/dev/null) || true
if grep -q "Insistencia contida." "$D/cli5_out.txt" 2>/dev/null && [ "$(grep -c '"ok": true' "$D/cli5.jsonl" 2>/dev/null)" = 5 ]; then
  echo "insistencia no erro: ok (aviso na 2a, recusa na 3a e na 4a, encerra e relata)"
else echo "insistencia no erro: FALHOU"; tail -3 "$D/cli5_out.txt" 2>/dev/null | cut -c1-600; fail=1; fi
# pre-requisito de fora: tres falhas em volta do mesmo programa levam a nota que o nomeia e manda seguir com o codigo
mkdir -p "$D/tw6"
printf '#!/bin/sh\n[ "$1" = --help ] && { echo "usage: fakesdk build|install|auth"; exit 0; }\necho "Error: Operation unsupported: the SDK is not installed (requires login)"; exit 1\n' >"$D/tw6/fakesdk"; chmod +x "$D/tw6/fakesdk"
(cd "$D/tw6" && printf 'faca o app\n/exit\n' | env -u ANTHROPIC_API_KEY HOME="$D/home" KATANA_CPU=1 KATANA_TEACHER=1 KATANA_TOOLS=2 MOCK_BLOCKED=1 MOCK_TOOLS=2 \
   KATANA_TEACHER_CLI="$OLDPWD/tools/traintest/mock_claude.py" MOCK_CLAUDE_LOG="$D/cli6.jsonl" $K agent "$D/agent.gguf" >"$D/cli6_out.txt" 2>/dev/null) || true
if grep -q "Bloqueio relatado e codigo escrito." "$D/cli6_out.txt" 2>/dev/null && [ -f "$D/tw6/app.txt" ]; then
  echo "pre-requisito de fora: ok (nota na 3a falha em volta de fakesdk, o codigo e escrito depois)"
else echo "pre-requisito de fora: FALHOU"; grep -a "FALHA" "$D/cli6_out.txt" 2>/dev/null | cut -c1-600 | head -3; fail=1; fi
# limite de rodadas por blocos (KATANA_ROUNDS=4): com progresso passa do bloco; sem progresso para no fim do primeiro
for R in 1 2; do
  mkdir -p "$D/tw7$R"
  (cd "$D/tw7$R" && printf 'trabalhe\n/exit\n' | env -u ANTHROPIC_API_KEY HOME="$D/home" KATANA_CPU=1 KATANA_TEACHER=1 KATANA_TOOLS=2 KATANA_ROUNDS=4 MOCK_ROUNDS=$R MOCK_TOOLS=2 \
     KATANA_TEACHER_CLI="$OLDPWD/tools/traintest/mock_claude.py" MOCK_CLAUDE_LOG="$D/cli7$R.jsonl" $K agent "$D/agent.gguf" >"$D/cli7${R}_out.txt" 2>/dev/null) || true
done
if grep -q "Dez arquivos gravados sem parar." "$D/cli71_out.txt" 2>/dev/null && [ "$(ls "$D/tw71" | wc -l)" = 10 ] && grep -q "Limite relatado." "$D/cli72_out.txt" 2>/dev/null; then
  echo "limite de rodadas: ok (10 rodadas com progresso num bloco de 4; sem progresso, nota na 3a rodada)"
else echo "limite de rodadas: FALHOU"; grep -a "FALHA" "$D/cli71_out.txt" "$D/cli72_out.txt" 2>/dev/null | cut -c1-300 | head -3; fail=1; fi
# o usuario muda o plano: o lembrete da lista e a mensagem nova dizem que a mensagem mais nova vem primeiro
mkdir -p "$D/tw8"
(cd "$D/tw8" && printf 'faca a lista\nesqueca a lista, faca outra coisa\n/exit\n' | env -u ANTHROPIC_API_KEY HOME="$D/home" KATANA_CPU=1 KATANA_TEACHER=1 KATANA_TOOLS=2 MOCK_REDIRECT=1 MOCK_TOOLS=2 \
   KATANA_TEACHER_CLI="$OLDPWD/tools/traintest/mock_claude.py" MOCK_CLAUDE_LOG="$D/cli8.jsonl" $K agent "$D/agent.gguf" >"$D/cli8_out.txt" 2>/dev/null) || true
if grep -q "Lembrete ok." "$D/cli8_out.txt" 2>/dev/null && grep -q "Plano novo." "$D/cli8_out.txt" && ! grep -q "FALHA" "$D/cli8_out.txt"; then
  echo "mensagem nova muda o plano: ok (lembrete da lista e pedido novo dizem que a mensagem nova vem primeiro)"
else echo "mensagem nova muda o plano: FALHOU"; grep -a "FALHA" "$D/cli8_out.txt" 2>/dev/null | cut -c1-600 | head -3; fail=1; fi
# planejar antes de executar: sem lista a 3a e a 6a chamada levam a nota; com lista o passo atual aparece e o proximo e nomeado
for R in 1 2; do
  mkdir -p "$D/tw9$R"
  (cd "$D/tw9$R" && printf 'trabalhe\n/exit\n' | env -u ANTHROPIC_API_KEY HOME="$D/home" KATANA_CPU=1 KATANA_UI=1 KATANA_TEACHER=1 KATANA_TOOLS=2 MOCK_PLAN=$R MOCK_TOOLS=2 \
     KATANA_TEACHER_CLI="$OLDPWD/tools/traintest/mock_claude.py" MOCK_CLAUDE_LOG="$D/cli9$R.jsonl" $K agent "$D/agent.gguf" >"$D/cli9${R}_out.txt" 2>/dev/null) || true
done
if grep -q "Plano cobrado." "$D/cli91_out.txt" 2>/dev/null && grep -q "Plano seguido." "$D/cli92_out.txt" 2>/dev/null \
   && grep -aq "TOOL Bash	.*echo 1.*	Mostra um numero$" "$D/cli92_out.txt"; then
  echo "plano antes de executar: ok (nota na 3a e na 6a chamada sem lista; passo atual na 5a; proximo passo no TodoWrite; descricao no marcador TOOL)"
else echo "plano antes de executar: FALHOU"; grep -a "FALHA" "$D/cli91_out.txt" "$D/cli92_out.txt" 2>/dev/null | cut -c1-600 | head -3; fail=1; fi
# katana agentlog: registro com os dois conjuntos, tabulacao e quebra de linha dentro do resultado
printf '2026-11-21 10:00:00\tBash\t"arguments": {"command": "x"}\tError: a Exit code 1\n2026-11-21 10:00:01\tBash\t"arguments": {"command": "x"}\tError: a\twith tab Exit code 1\nsecond line\n2026-11-21 10:00:02\tread_file\t{"path": "a"}\tok\n2026-11-21 10:00:03\tread_file\t{"path": "a"}\terror: b\n2026-11-21 10:00:04\tread_file\t{"path": "b"}\terror: b\n2026-11-21 10:00:05\tRead\t{"file_path": "c"}\terror: c\n' >"$D/al.log"
AL=$($K agentlog "$D/al.log" 2>&1)
if echo "$AL" | grep -q "^agent log .*: 6 calls .* 5 failed (83%), 2 identical repeats" && echo "$AL" | grep -q "^read_file  *3  *2  *67%  *1" \
   && echo "$AL" | grep -q "^ *2  read_file  *error: b$" && echo "$AL" | grep -q "^ *3  2026-11-21 10:00:03 to 10:00:05  read_file, Read$" \
   && echo "$AL" | grep -q "Error: a with tab Exit code 1 second line"; then echo "katana agentlog: ok (chamadas, falhas, repeticoes, erros, sequencias)"
else echo "katana agentlog: FALHOU"; echo "$AL" | head -20; fail=1; fi
# o texto do professor chega em pedacos e tem que aparecer inteiro e uma vez so na saida (acento, aspas, quebra de linha, emoji)
if [ "$(grep -c '"ok": true' "$D/cli.jsonl" 2>/dev/null)" = 3 ] && grep -q "linha 1" "$D/tw2/nota.txt" 2>/dev/null \
   && [ "$(grep -c 'Vou gravar o arquivo: ação "um"' "$D/cli_out.txt")" = 1 ] && [ "$(grep -c '^e 😀 dois\.' "$D/cli_out.txt")" = 1 ] \
   && [ "$(grep -c 'Pronto: nota.txt gravado e conferido\.' "$D/cli_out.txt")" = 1 ]; then echo "professor pelo claude -p (texto aos poucos): ok"; else echo "professor pelo claude -p: FALHOU"; fail=1; fi
# o segundo conjunto de ferramentas (KATANA_TOOLS=2): Write, Edit, Bash e Grep pelo laco inteiro
mkdir -p "$D/tw3"
(cd "$D/tw3" && printf 'grave e confira nota2.txt\n/exit\n' | env -u ANTHROPIC_API_KEY HOME="$D/home" KATANA_CPU=1 KATANA_TEACHER=1 KATANA_TOOLS=2 MOCK_TOOLS=2 \
   KATANA_TEACHER_CLI="$OLDPWD/tools/traintest/mock_claude.py" MOCK_CLAUDE_LOG="$D/cli3.jsonl" $K agent "$D/agent.gguf" >"$D/cli3_out.txt" 2>/dev/null) || true
if [ "$(grep -c '"ok": true' "$D/cli3.jsonl" 2>/dev/null)" = 4 ] && grep -q "linha dois" "$D/tw3/nota2.txt" 2>/dev/null \
   && grep -q 'Pronto: ferramentas novas conferidas' "$D/cli3_out.txt"; then echo "segundo conjunto de ferramentas (Write, Edit, Bash, Grep) pelo laco: ok"
else echo "segundo conjunto de ferramentas: FALHOU"; grep -a "FALHA\|ok\": false" "$D/cli3_out.txt" "$D/cli3.jsonl" | head -5; fail=1; fi
python3 -c "open('$D/big.txt','w').write(''.join('linha %d: %s\n' % (i, 'x'*60) for i in range(1,301)))"
python3 tools/traintest/mock_anthropic.py 8201 "$D/rd_req.jsonl" "$D/big.txt" & RMOCK=$!
sleep 1
(cd "$D/tw2" && printf 'leia o arquivo\n/exit\n' | env HOME="$D/home" KATANA_CPU=1 KATANA_DIGEST=2 KATANA_TEACHER_FEED=1 KATANA_TEACHER=1 ANTHROPIC_API_KEY=sk-mock-key \
   KATANA_TEACHER_URL=http://127.0.0.1:8201 $K agent "$D/agent.gguf" >/dev/null 2>&1) || true
kill $RMOCK 2>/dev/null || true
python3 - "$D/rd_req.jsonl" <<'EOF' || fail=1
import json, sys
try:
    rs = [json.loads(l) for l in open(sys.argv[1])]
    res = [x["content"] for m in rs[-1]["request"]["messages"] for x in m["content"] if x["type"] == "tool_result"]
    ok = len(res) == 5 and all(r["status"] == 200 for r in rs)
    for i, (a, b) in enumerate([(1, 84), (85, 167), (168, 250), (251, 300)]):
        ok = ok and ("condensed note of lines %d-%d" % (a, b)) in res[i]
    ok = ok and "already read this whole file (300 lines)" in res[4] and "[lines 168-250]" in res[4] and max(len(r) for r in res) < 3000
except Exception as e:
    ok = False; res = e
print("leitura com posicao e notas: %s" % ("ok (4 trechos em sequencia, 5a leitura devolve as notas)" if ok else "FALHOU %s" % (res,)))
sys.exit(0 if ok else 1)
EOF
# katana import: conversa no formato de mensagens (com chamada de ferramenta) e ShareGPT -> dados de treino -> treino
printf '%s\n' \
 '{"messages":[{"role":"system","content":"You are katana."},{"role":"user","content":"what is katana?"},{"role":"assistant","content":"","tool_calls":[{"id":"c1","type":"function","function":{"name":"recall","arguments":"{\"query\": \"katana\"}"}}]},{"role":"tool","tool_call_id":"c1","content":"a local engine"},{"role":"assistant","content":"katana is a local engine."}]}' \
 '{"conversations":[{"from":"system","value":"You are katana."},{"from":"human","value":"say hi"},{"from":"gpt","value":"hi there!"}]}' > "$D/chats.jsonl"
if $K import "$D/llama.gguf" "$D/chats.jsonl" "$D/imp.jsonl" --keep-tool-names 2>/dev/null | grep -q "^import: 2 de 2"; then echo "import: ok"; else echo "import: FALHOU"; fail=1; fi
$K train "$D/llama.gguf" "$D/imp.jsonl" --steps 150 --lr 3e-3 --seq 256 --out "$D/imp.kspec" 2>/dev/null | grep "perda media final"
gcc -O1 -mavx2 -mfma -mf16c -w -I . -I src -I vk/include tools/traintest/import_calls.c -o "$D/import_calls" -lm -lpthread -ldl
if "$D/import_calls" >/dev/null; then echo "chamadas importadas lidas pelo agente nos 4 formatos: ok"; else echo "chamadas importadas: FALHOU"; fail=1; fi
# transcricoes de um agente de programacao -> formato de mensagens -> katana import (src/app/transcript.c)
mkdir -p "$D/tr"
python3 - "$D/tr/a.jsonl" <<'EOF'
import json, sys
def U(c, **k): return dict({"type": "user", "uuid": "u%d" % len(R), "message": {"role": "user", "content": c}}, **k)
def A(mid, blocks, **k): return dict({"type": "assistant", "uuid": "a%d" % len(R), "message": {"role": "assistant", "id": mid, "content": blocks}}, **k)
R = []
R.append({"type": "mode", "mode": "x"})
R.append(U("<system-reminder>\nignore\n</system-reminder>\nlist the files in /tmp/other-7/work"))
R.append(U("meta text", isMeta=True))
R.append(A("m1", [{"type": "thinking", "thinking": "I will list them."}]))
R.append(A("m1", [{"type": "text", "text": "Listing."}]))
R.append(A("m1", [{"type": "tool_use", "id": "t1", "name": "Bash", "input": {"command": "ls /tmp/other-7/work", "timeout": 5000}}]))
R.append(U([{"type": "tool_result", "tool_use_id": "t1", "content": [{"type": "text", "text": "a.txt\n" + "x" * 500}, {"type": "image", "source": {}}]}]))
R.append(A("m2", [{"type": "text", "text": "There is one file: a.txt."}]))
R.append(R[-1])                                    # registro repetido (sessao retomada): entra uma vez
R.append(U("now delete it"))
R.append(A("m3", [{"type": "tool_use", "id": "t2", "name": "Bash", "input": {"command": "rm a.txt"}}]))
R.append(U([{"type": "text", "text": "[Request interrupted by user for tool use]"}]))
R.append(U("say the secret word"))
R.append(A("m4", [{"type": "text", "text": "ok"}]))
R.append(A("m5", [{"type": "text", "text": "the word is FORBIDDEN"}]))
R.append(U("report back"))
R.append(A("m6", [{"type": "tool_use", "id": "t3", "name": "SubagentHandback", "input": {"message": "done"}}], toolEndsTurn=True))
open(sys.argv[1], "w").write("".join(json.dumps(r) + "\n" for r in R))
EOF
$K import "$D/llama.gguf" "$D/tr" "$D/tr_train.jsonl" --system none --max-result 120 --replace '/tmp/other-[0-9]+=/tmp/katana-work' --cut-matching 'forbidden' > "$D/tr.out" 2>&1 || true
if python3 - "$D/tr_train.jsonl" "$D/tr.out" <<'EOF'
import json, sys
T = [json.loads(l)["text"] for l in open(sys.argv[1])]
log = open(sys.argv[2]).read()
ok = len(T) == 3 and "1 arquivos, 4 episodios, 3 gravados (4 falas do modelo, 2 chamadas" in log and "import: 3 de 3" in log
a = "".join(T)
ok = ok and "list the files in /tmp/katana-work/work" in T[0] and "ls /tmp/katana-work/work" in T[0] and "5000" in T[0] and "Listing." in T[0]
ok = ok and "bytes omitted" in T[0] and "[image]" in T[0] and T[0].count("There is one file: a.txt.") == 1
ok = ok and "say the secret word" in T[1] and "SubagentHandback" in T[2]
ok = ok and not any(x in a for x in ("FORBIDDEN", "rm a.txt", "meta text", "ignore", "/tmp/other-7", "delete it"))
sys.exit(0 if ok else 1)
EOF
then echo "import de transcricoes: ok (blocos juntos, avisos fora, interrompido fora, troca e corte)"; else echo "import de transcricoes: FALHOU"; cat "$D/tr.out"; fail=1; fi
# katana knowledge: notas das sessoes anteriores (src/app/knowledge.c), sem modelo
mkdir -p "$D/kw/alpha/.git" "$D/kw/alpha/sub" "$D/kw/beta" "$D/kt/proj-a" "$D/kt/proj-b"
python3 - "$D" <<'EOF2'
import json, sys, os
D = sys.argv[1]
def mk(path, cwd, ep):
    R = []
    n = [0]
    def rec(t, msg, ts):
        n[0] += 1
        return {"type": t, "uuid": "%s-%d" % (os.path.basename(path), n[0]), "cwd": cwd, "timestamp": ts, "message": msg}
    for i, e in enumerate(ep):
        ts = e["ts"]
        R.append(rec("user", {"role": "user", "content": e["req"]}, ts))
        for j, (name, inp, res) in enumerate(e.get("calls", [])):
            tid = "t%d_%d" % (i, j)
            R.append(rec("assistant", {"role": "assistant", "id": "m%d_%d" % (i, j), "content": [{"type": "tool_use", "id": tid, "name": name, "input": inp}]}, ts))
            R.append(rec("user", {"role": "user", "content": [{"type": "tool_result", "tool_use_id": tid, "content": res}]}, ts))
        R.append(rec("assistant", {"role": "assistant", "id": "f%d" % i, "content": [{"type": "text", "text": e["out"]}]}, ts))
    open(path, "w").write("".join(json.dumps(r) + "\n" for r in R))
a, b = D + "/kw/alpha", D + "/kw/beta"
mk(D + "/kt/proj-a/s1.jsonl", a, [
  {"ts": "2026-11-10T10:00:00Z", "req": "add a retry to the uploader", "out": "Added retry with backoff to the uploader. Key 12345678-1234-1234-1234-123456789abc was removed.",
   "calls": [("Write", {"file_path": a + "/up.py", "content": "x"}, "ok"), ("Edit", {"file_path": a + "/up.py", "old_string": "a", "new_string": "b"}, "ok"),
             ("Bash", {"command": "make", "description": "Build the project"}, "Exit code 2\nmake: *** No rule to make target 'all'"),
             ("Bash", {"command": "make clean && make", "description": "Rebuild from scratch"}, "built"),
             ("Bash", {"command": "curl -H \"Authorization: Bearer abcdef123456\" http://x/api --token=zzzSECRET99 -d password=hunter22", "description": "Call the api"}, "{}"),
             ("Bash", {"command": "ls"}, "a b")]},
  {"ts": "2026-11-10T11:00:00Z", "req": "fix the parser", "out": "Parser fixed."}])
mk(D + "/kt/proj-a/s2.jsonl", a, [{"ts": "2026-11-12T09:00:00Z", "req": "update the uploader docs", "out": "Docs updated for the uploader and the retry.\nforbidden-line here\nSecond line kept."}])
mk(D + "/kt/proj-b/s3.jsonl", b, [{"ts": "2026-11-11T09:00:00Z", "req": "tune the retry loop", "out": "Done, nothing about the other word."}])
EOF2
find "$D/kt" -name '*.jsonl' -exec touch -d '2 hours ago' {} +
KO="$D/kn_out"
$K knowledge build "$D/kt" --out "$KO" --cut-matching 'forbidden-line' --replace 'zzz=yyy' > "$D/kn1.out" 2>&1 || true
$K knowledge build "$D/kt" --out "$KO" > "$D/kn2.out" 2>&1 || true
touch "$D/kt/proj-b/s3.jsonl"
$K knowledge build "$D/kt" --out "$KO" > "$D/kn3.out" 2>&1 || true
$K knowledge search uploader retry --dir "$KO" > "$D/kn_s1.out" 2>&1 || true
$K knowledge search retry --project beta --dir "$KO" > "$D/kn_s2.out" 2>&1 || true
$K knowledge stats "$KO" > "$D/kn_st.out" 2>&1 || true
$K knowledge corpus "$KO" "$D/kc.jsonl" --holdout "$D/kch.jsonl" > "$D/kc.out" 2>&1 || true
$K knowledge corpus "$KO" "$D/kc2.jsonl" --project alpha --max-per-project 8 > "$D/kc2.out" 2>&1 || true
$K knowledge stats "$D/kc.jsonl" > "$D/kc_st.out" 2>&1 || true
if python3 - "$KO" "$D" <<'EOF2'
import sys, glob, os
KO, D = sys.argv[1], sys.argv[2]
rd = lambda p: open(p).read()
na = sorted(glob.glob(KO + "/alpha/2026-*.md"))
nb = glob.glob(KO + "/beta/2026-*.md")
ok = len(na) == 2 and len(nb) == 1 and os.path.exists(KO + "/INDEX.md") and os.path.exists(KO + "/alpha/INDEX.md")
n1 = [rd(p) for p in na if "2026-11-10" in p]; ok = ok and len(n1) == 1
n1 = n1[0] if n1 else ""
n2 = [rd(p) for p in na if "2026-11-12" in p][0]
ok = ok and "add-a-retry-to-the-uploader" in "".join(na) and "Episodes: 2" in n1
ok = ok and "> add a retry to the uploader" in n1 and "> Added retry with backoff" in n1 and "## Episode 2" in n1
ok = ok and "- " + D + "/kw/alpha/up.py" in n1 and n1.count("/up.py") == 1
ok = ok and "- Rebuild from scratch: make clean && make" in n1 and "- Call the api: curl" in n1
ok = ok and "Build the project: make" not in n1 and "\n- ls\n" not in n1
ok = ok and "`make` failed (make: *** No rule to make target 'all') -> `make clean && make` worked" in n1
ok = ok and "[key]" in n1 and "12345678-1234" not in n1
ok = ok and "abcdef123456" not in n1 and "yyySECRET99" not in n1 and "hunter22" not in n1 and "Bearer [masked]" in n1 and "password=[masked]" in n1
ok = ok and "forbidden-line" not in n2 and "Second line kept." in n2
ok = ok and "Folder: " + D + "/kw/alpha" in rd(KO + "/alpha/INDEX.md") and "[alpha]" in rd(KO + "/INDEX.md")
sys.exit(0 if ok else 1)
EOF2
then echo "knowledge build: ok (notas, pedido, fim, arquivos, comandos, erro vencido, segredos, corte, indices)"; else echo "knowledge build: FALHOU"; cat "$D/kn1.out"; fail=1; fi
if grep -q "3 notas escritas" "$D/kn1.out" && grep -q "0 notas escritas, 3 ja atualizadas" "$D/kn2.out" && grep -q "1 notas escritas, 2 ja atualizadas" "$D/kn3.out"; then echo "knowledge incremental: ok (pula o que esta em dia, refaz o que mudou)"; else echo "knowledge incremental: FALHOU"; cat "$D/kn1.out" "$D/kn2.out" "$D/kn3.out"; fail=1; fi
if python3 - "$D" <<'EOF2'
import sys
D = sys.argv[1]
s1 = open(D + "/kn_s1.out").read().split("\n")
paths = [l.split(" (episode")[0] for l in s1 if " (episode " in l]
s2 = open(D + "/kn_s2.out").read()
st = open(D + "/kn_st.out").read()
ok = len(paths) == 3 and "2026-11-12" in paths[0] and "2026-11-10" in paths[1] and "/beta/" in paths[2]
ok = ok and "2 of 2 words" in s1[0] and "request: update the uploader docs" in s1[1] and "outcome: Docs updated" in s1[2] and "1 of 2 words" in "".join(s1[-4:])
ok = ok and "/alpha/" not in s2 and "tune the retry loop" in s2
ok = ok and "projetos: 2" in st and "notas: 3" in st and "episodios: 4" in st and "nota mais nova" in st
sys.exit(0 if ok else 1)
EOF2
then echo "knowledge search e stats: ok"; else echo "knowledge search/stats: FALHOU"; cat "$D/kn_s1.out" "$D/kn_st.out"; fail=1; fi
if python3 - "$D" <<'EOF2'
import sys, json
D = sys.argv[1]
L = lambda n: [json.loads(l) for l in open(D + "/" + n)]
tr, ho, c2 = L("kc.jsonl"), L("kch.jsonl"), L("kc2.jsonl")
QA = [(m["messages"][0]["content"], m["messages"][1]["content"], m["project"]) for m in tr]
txt = open(D + "/kc.jsonl").read() + open(D + "/kch.jsonl").read()
def has(q, a, p="alpha"): return any(q in x and a in y and pr == p for x, y, pr in QA)
ok = has("add a retry to the uploader", "Added retry with backoff") and has("add a retry to the uploader", "up.py")
ok = ok and has("rebuild from scratch", "`make clean && make`") and has("No rule to make target", "What worked: `make clean && make`")
ok = ok and has("What is alpha?", D + "/kw/alpha") and has("Where is alpha?", D + "/kw/alpha") and has("main files of alpha", "up.py (")
ok = ok and not any(x in txt for x in ("hunter22", "abcdef123456", "12345678-1234", "SECRET99"))
ok = ok and all("alpha" in q or "beta" in q for q, a, p in QA)
tq = set(q for q, a, p in QA)
ok = ok and len(ho) > 0 and len(tr) == 2 * len(ho) and not any(m["messages"][0]["content"] in tq for m in ho)
ok = ok and len(c2) == 8 and all(m["project"] == "alpha" for m in c2)
q2 = " ".join(m["messages"][0]["content"] for m in c2); ok = ok and "update the uploader docs" in q2 and "fix the parser" not in q2
sys.exit(0 if ok else 1)
EOF2
then echo "knowledge corpus: ok (pares fieis as notas, sem segredo, teto, holdout com outra formulacao)"; else echo "knowledge corpus: FALHOU"; cat "$D/kc.out" "$D/kc2.out"; fail=1; fi
if grep -q "projetos: 2\|beta: .* pares" "$D/kc_st.out" && grep -q "total: .* pares" "$D/kc_st.out" && grep -q "janelas de 512 tokens (estimativa" "$D/kc_st.out"; then echo "knowledge stats do corpus: ok"; else echo "knowledge stats do corpus: FALHOU"; cat "$D/kc_st.out"; fail=1; fi
# chamadas do primeiro conjunto de ferramentas viram as do segundo (nome e argumentos); sem equivalente: a fala nao treina
python3 - "$D/old.jsonl" <<'EOF'
import json, sys
def conv(calls):
    m = [{"role": "user", "content": "do it"}]
    for i, (n, a) in enumerate(calls):
        m.append({"role": "assistant", "content": "", "tool_calls": [{"id": "c%d" % i, "type": "function", "function": {"name": n, "arguments": json.dumps(a)}}]})
        m.append({"role": "tool", "tool_call_id": "c%d" % i, "content": "ok"})
    m.append({"role": "assistant", "content": "done"})
    return json.dumps({"messages": m}) + "\n"
open(sys.argv[1], "w").write(
    conv([("read_file", {"path": "a.c", "offset": 3}), ("run_command", {"command": "make", "timeout_s": 30, "background": True}),
          ("edit_file", {"path": "a.c", "old": "x", "new": "y"}), ("search_files", {"pattern": "foo(", "path": "src"}),
          ("list_dir", {"path": "my dir"}), ("todo", {"steps": "[x] read\n[ ] fix it"}), ("ask_user", {"question": "which?", "options": "one | two"}),
          ("delegate", {"task": "find where the parser lives and report"}), ("find_file", {"name": "notes"}), ("write_file", {"path": "b.c", "content": "int x;"})])
    + conv([("remember", {"fact": "x"})]) + conv([("write_file", {"path": "b", "content": "c", "append": True})]))
EOF
$K import "$D/llama.gguf" "$D/old.jsonl" "$D/old_train.jsonl" --system none > "$D/old.out" 2>&1 || true
if python3 - "$D/old_train.jsonl" "$D/old.out" <<'EOF'
import json, sys
R = [json.loads(l) for l in open(sys.argv[1])]
T = [r["text"] for r in R]
log = open(sys.argv[2]).read()
ok = len(T) == 3 and "import: 3 de 3" in log and "convertidas" in log and "1 falas com chamada sem equivalente" in log
ok = ok and [len(r["train_spans"]) for r in R] == [11, 1, 2] and "cat >> 'b' <<'KATANA_EOF'" in T[2]
t = T[0] if T else ""
want = ['"name": "Read", "arguments": {"file_path": "a.c", "offset": 3}', '"name": "Bash", "arguments": {"command": "make", "timeout": 30000, "run_in_background": true}',
        '"name": "Edit", "arguments": {"file_path": "a.c", "old_string": "x", "new_string": "y"}',
        '"name": "Grep", "arguments": {"pattern": "foo(", "path": "src", "output_mode": "content", "literal": true}',
        '"name": "Bash", "arguments": {"command": "ls -la \'my dir\'"}',
        '"name": "TodoWrite", "arguments": {"todos": [{"content": "read", "status": "completed"}, {"content": "fix it", "status": "pending"}]}',
        '"name": "AskUserQuestion", "arguments": {"questions": [{"question": "which?", "options": [{"label": "one"}, {"label": "two"}]}]}',
        '"name": "Agent", "arguments": {"description": "find where the parser lives", "prompt": "find where the parser lives and report"}',
        '"name": "Glob", "arguments": {"pattern": "**/*notes*"}', '"name": "Write", "arguments": {"file_path": "b.c", "content": "int x;"}',
        "[resultado da ferramenta Read,"]
miss = [w for w in want if w not in t]
ok = ok and not miss and "read_file" not in t and "run_command" not in t
if not ok: print(miss, log)
sys.exit(0 if ok else 1)
EOF
then echo "ferramentas do primeiro conjunto -> segundo: ok (11 chamadas convertidas; a fala sem equivalente fica fora do treino)"; else echo "ferramentas do primeiro conjunto -> segundo: FALHOU"; fail=1; fi
# katana merge: o GGUF fundido responde igual ao modelo base com o .kspec (adaptadores sobre Q8_0; MoE; qwen35moe)
$K merge "$D/q8.gguf" "$D/adapt.kspec" "$D/q8_merged.gguf" >/dev/null 2>&1 && check_spec "$D/q8_merged.gguf" "" 8195 || { echo "merge q8: FALHOU"; fail=1; }
$K merge "$D/moe.gguf" "$D/moe.kspec" "$D/moe_merged.gguf" >/dev/null 2>&1 && check_spec "$D/moe_merged.gguf" "" 8196 || { echo "merge moe: FALHOU"; fail=1; }
$K merge "$D/q35.gguf" "$D/q35.kspec" "$D/q35_merged.gguf" >/dev/null 2>&1 && check_spec "$D/q35_merged.gguf" "" 8197 || { echo "merge q35: FALHOU"; fail=1; }
$K merge "$D/g4.gguf" "$D/g4.kspec" "$D/g4_merged.gguf" >/dev/null 2>&1 && check_spec "$D/g4_merged.gguf" "" 8200 || { echo "merge gemma4: FALHOU"; fail=1; }
# katana distill: professor de mentira resolve a tarefa bank com as ferramentas do katana; testes escondidos decidem
python3 tools/traintest/mock_teacher.py 8198 tools/codebench/tasks/bank/solution/bank.py & MOCK=$!
sleep 1
TEACHER_API_KEY=secret-key $K codebench run --katana "$K" --teacher http://127.0.0.1:8198/v1 --teacher-model mock \
   --distill-out "$D/teacher.jsonl" --out "$D/teacher_bench.jsonl" --tasks bank,rotate --timeout 120 >/dev/null 2>&1
kill $MOCK 2>/dev/null || true
if python3 -c "import json,sys; r=[json.loads(l) for l in open(sys.argv[1])]; sys.exit(0 if [x['passed'] for x in r]==[True,False] else 1)" "$D/teacher.jsonl"; then
  echo "distill: ok (bank resolvido e aprovado; rotate reprovado)"
else echo "distill: FALHOU"; fail=1; fi
if $K import "$D/llama.gguf" "$D/teacher.jsonl" "$D/teacher_train.jsonl" 2>/dev/null | grep -q "^import: 1 de 2"; then echo "import do professor (so a aprovada): ok"; else echo "import do professor: FALHOU"; fail=1; fi
# 11b. katana codebench (o juiz em C): verify, compare, prazo, limpeza e medidas do registro, sem modelo
. tools/traintest/codebench_part.sh
# 12. selecao do katana import e katana specialize (teacher, tools, code)
. tools/traintest/specialize_part.sh
# chamadas de ferramenta validas por construcao (src/agent/callgrammar.c): automato sem modelo e modelo aleatorio
if $K calltest > "$D/ct.txt"; then echo "calltest: ok ($(tail -1 "$D/ct.txt"))"; else echo "calltest: FALHOU"; grep FAILED "$D/ct.txt"; fail=1; fi
if KATANA_TOOLS=2 sh tools/traintest/callgrammar_e2e.sh "$K" > "$D/cg_e2e.txt" 2>&1; then echo "chamadas validas com modelo aleatorio: ok ($(tail -1 "$D/cg_e2e.txt"))"; else echo "chamadas validas com modelo aleatorio: FALHOU"; grep -a FAIL "$D/cg_e2e.txt"; fail=1; fi
. tools/traintest/specialize_oneshot.sh
# 13. katana curriculum: 200 conversas das convencoes (KATANA.md, skills, MCP), check, import nos tres formatos
$K curriculum "$D/cur.jsonl" --n 200 --seed 3 --holdout "$D/cur_hold.jsonl" >/dev/null 2>&1 || { echo "curriculum: FALHOU (gerador)"; fail=1; }
if $K curriculum check "$D/cur.jsonl" 2>&1 | grep -q "^check: 200 conversas, .* 0 com problema" && $K curriculum check "$D/cur_hold.jsonl" 2>&1 | grep -q " 0 com problema"; then echo "curriculum check: ok"; else echo "curriculum check: FALHOU"; fail=1; fi
for t in chatml chatml-xml gemma4; do
  python3 tools/tinymodel/make_tiny_gguf.py "$D/cur_$t.gguf" --template $t >/dev/null
  if $K import "$D/cur_$t.gguf" "$D/cur.jsonl" "$D/cur_$t.out.jsonl" --system keep 2>/dev/null | grep -q "^import: 200 de 200"; then echo "curriculum import $t: ok"; else echo "curriculum import $t: FALHOU"; fail=1; fi
done
# 13. katana sem subcomando (console) e katana -p (headless), com o professor de mentira
if sh tools/tooltest/launch.sh "$K" > "$D/launch.out" 2>&1; then echo "launch (console e headless): ok ($(tail -1 "$D/launch.out"))"
else echo "launch (console e headless): FALHOU"; grep -A8 '^FAIL' "$D/launch.out" | head -30; fail=1; fi
if [ $fail = 0 ]; then echo "traintest: ok"; else echo "traintest: FALHOU"; exit 1; fi
