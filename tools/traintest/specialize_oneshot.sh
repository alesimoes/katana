set +e   # os comandos abaixo falham de proposito e o codigo de saida e conferido a mao
# a rotina de uma noite em um comando (katana specialize <modelo>), gravacao que falha em voz alta, espera pela maquina
# livre, relatorio e katana knowledge quiz. Usa o que specialize_part.sh deixou: $D, $K, $SM (modelo minusculo do agente),
# $D/sel (transcricoes), $D/sp_sess (sessao com falas do professor). Precisa de bash/sh com python3.
OS="$D/os"; mkdir -p "$OS"
H1="$OS/home1"; mkdir -p "$H1/.katana/desktop/sessoes"
cp "$D"/sp_sess/* "$H1/.katana/desktop/sessoes/" 2>/dev/null || true
printf '# comentario\ntranscripts=%s\ntraining-dir=%s/trn\nreplace=hello=HELLO\nreplace=gcc=GCC\ncut-matching=zzzzqqqq\n' "$D/sel" "$OS" > "$H1/.katana/katana.cfg"
# --- 1. um comando so: nada alem do modelo (e passos curtos, para o teste) ---
mkdir -p "$OS/tmpcheck"
TMPDIR="$OS/tmpcheck" KATANA_TOOLS=2 HOME="$H1" $K specialize "$SM" --steps 12 --seq 256 --lr 3e-3 --margin-mb 0 --no-check > "$OS/one1.out" 2>&1 || true
sleep 1
TMPDIR="$OS/tmpcheck" KATANA_TOOLS=2 HOME="$H1" $K specialize "$SM" --steps 12 --seq 256 --lr 3e-3 --margin-mb 0 --no-check > "$OS/one2.out" 2>&1 || true
if python3 - "$OS" "$H1" "$D" <<'PY'
import json, sys, glob, os
O, H, DD = sys.argv[1], sys.argv[2], sys.argv[3]
bad = []
def chk(c, w):
    if not c: bad.append(w)
h = [json.loads(l) for l in open(O + "/trn/history.jsonl")]
chk(len(h) == 2 and h[0]["night"] == 1 and h[1]["night"] == 2 and h[0]["resumed_from"] is None and h[1]["resumed_from"], "historico com 2 noites e retomada")
chk(h[0]["model"].endswith("agent.gguf"), "modelo no historico")
chk(sum(h[0]["windows"].values()) == 12 and sum(h[0]["available"].values()) > 12, "janelas treinadas e disponiveis")
d1 = sorted(glob.glob(O + "/trn/all-*/data.jsonl"))
txt = open(d1[0]).read()
chk("HELLO" in txt and "GCC" in txt, "replace do katana.cfg aplicado")
chk('"source": "knowledge"' in txt and '"source": "tools"' in txt and '"source": "code"' in txt and ('"source": "teacher"' in txt or not os.path.exists(DD + "/tch.ksess")), "as fontes no mesmo arquivo")
chk(os.path.getsize(O + "/trn/corpus.jsonl") > 0 and os.path.getsize(O + "/trn/holdout.jsonl") > 0, "corpus e holdout na pasta de treino")
chk(os.path.exists(H + "/.katana/knowledge/INDEX.md"), "base de conhecimento em ~/.katana/knowledge")
chk(len(glob.glob(O + "/trn/all-*/katana.kspec")) == 2, "dois katana.kspec")
chk(os.listdir(O + "/tmpcheck") == [], "nada em TMPDIR")
rep = open(sorted(glob.glob(O + "/trn/all-*/report.txt"))[-1]).read()
chk("noite 2 desta especializacao" in rep and "KATANA_SPEC=" in rep and "mkdir -p" in rep and "knowledge quiz" in rep and "janelas treinadas ate agora" in rep and "perda por noite" in rep, "relatorio final")
print("noite 1 e 2: janelas %s de %s" % (h[1]["windows"], h[1]["available"]))
sys.exit(1 if bad else 0)
PY
then echo "specialize em um comando (cfg: transcripts, training-dir, replace, cut-matching; conhecimento, corpus, sessoes, 2 noites): ok"; else echo "specialize em um comando: FALHOU"; tail -8 "$OS/one1.out"; fail=1; fi
HOME="$H1" $K specialize --status > "$OS/status.out" 2>&1 || true
if grep -q "noite 2 desta" "$OS/status.out" && grep -q "KATANA_SPEC=.*katana agent" "$OS/status.out" && grep -q "cp .*\.katana/specs" "$OS/status.out"; then echo "specialize --status: ok"; else echo "specialize --status: FALHOU"; cat "$OS/status.out"; fail=1; fi
# a semente muda a cada noite: as duas noites nao treinam as mesmas janelas na mesma ordem (a perda da 2a noite nao e igual)
python3 - "$OS/trn/history.jsonl" <<'PY' && echo "noites com sementes diferentes (perdas diferentes): ok" || { echo "sementes: FALHOU"; fail=1; }
import json, sys
h = [json.loads(l) for l in open(sys.argv[1])]
sys.exit(0 if h[0]["loss_first10"] != h[1]["loss_first10"] else 1)
PY
# --- 2. gravacao que falha: disco cheio simulado (KATANA_FAIL_WRITE_AFTER) e real (/dev/full) ---
mkdir -p "$OS/ff"
for lim in 200 40000; do
  KATANA_FAIL_WRITE_AFTER=$lim KATANA_TOOLS=2 HOME="$H1" $K specialize "$SM" --out "$OS/ff/all-$lim" --no-refresh --dry-run --margin-mb 0 > "$OS/ff$lim.out" 2>&1; rc=$?
  if [ $rc -ne 0 ] && grep -q "PAROU\|ERRO" "$OS/ff$lim.out" && ! grep -q "dry-run: parei" "$OS/ff$lim.out" && grep -qi "No space left\|disco cheio" "$OS/ff$lim.out" "$OS/ff/all-$lim"/*.log 2>/dev/null; then echo "specialize com disco cheio (limite $lim bytes): falhou em voz alta, codigo $rc: ok"; else echo "specialize com disco cheio ($lim): FALHOU (codigo $rc)"; tail -5 "$OS/ff$lim.out"; fail=1; fi
done
$K import "$SM" "$D/sel" /dev/full --system none > "$OS/full.out" 2>&1; rc=$?
if [ $rc -ne 0 ] && grep -q "No space left on device" "$OS/full.out"; then echo "import em /dev/full (ENOSPC de verdade): codigo $rc e mensagem: ok"; else echo "import em /dev/full: FALHOU (codigo $rc)"; cat "$OS/full.out" | tail -3; fail=1; fi
# conferencia de um arquivo de dados cortado ao meio e com linha invalida
D1=$(ls "$OS"/trn/all-*/data.jsonl | head -1)
$K specialize --verify-data "$D1" > "$OS/ver_ok.out" 2>&1 && grep -q "todas objetos JSON inteiros" "$OS/ver_ok.out" || { echo "verify-data do arquivo inteiro: FALHOU"; fail=1; }
head -c $(( $(wc -c < "$D1") * 2 / 3 )) "$D1" > "$OS/cut.jsonl"
if $K specialize --verify-data "$OS/cut.jsonl" > "$OS/ver_cut.out" 2>&1; then echo "verify-data: arquivo cortado ao meio PASSOU: FALHOU"; fail=1; else echo "verify-data recusa o arquivo cortado ao meio: ok"; fi
{ head -2 "$D1"; echo '{"text": "x", "train_spans": [[0, 1]'; echo '{"source": "tools", "text": "x"}'; } > "$OS/bad.jsonl"
if $K specialize --verify-data "$OS/bad.jsonl" > "$OS/ver_bad.out" 2>&1; then echo "verify-data: linhas invalidas PASSARAM: FALHOU"; fail=1; else echo "verify-data recusa linhas JSON invalidas e sem train_spans: ok"; fi
# conhecimento: build e corpus com disco cheio simulado; o corpus antigo fica como estava
cp "$OS/trn/corpus.jsonl" "$OS/corpus_before.jsonl"
KATANA_FAIL_WRITE_AFTER=500 $K knowledge corpus "$H1/.katana/knowledge" "$OS/trn/corpus.jsonl" --holdout "$OS/trn/holdout.jsonl" > "$OS/kc.out" 2>&1; rc=$?
if [ $rc -ne 0 ] && grep -q "ERRO" "$OS/kc.out" && cmp -s "$OS/corpus_before.jsonl" "$OS/trn/corpus.jsonl" && [ ! -e "$OS/trn/corpus.jsonl.part" ]; then echo "knowledge corpus com disco cheio: falhou em voz alta e o corpus antigo ficou intacto: ok"; else echo "knowledge corpus com disco cheio: FALHOU (codigo $rc)"; fail=1; fi
KATANA_FAIL_WRITE_AFTER=300 $K knowledge build "$D/sel" --out "$OS/kfull" --force > "$OS/kb.out" 2>&1; rc=$?
if [ $rc -ne 0 ] && grep -q "ERRO" "$OS/kb.out" && ! ls "$OS"/kfull/*/*.part >/dev/null 2>&1; then echo "knowledge build com disco cheio: falhou em voz alta, sem arquivo pela metade: ok"; else echo "knowledge build com disco cheio: FALHOU (codigo $rc)"; fail=1; fi
# --- 3. --wait-idle: um agente de mentira rodando ---
H3="$OS/home3"; mkdir -p "$H3/.katana/desktop/sessoes"
touch "$H3/.katana/desktop/sessoes/x.ksess"
bash -c 'exec -a "katana agent" sleep 90' & FAKE=$!
sleep 1
KATANA_IDLE_POLL=1 KATANA_TOOLS=2 HOME="$H3" $K specialize "$SM" --from "$D/sel" --out "$OS/w1/all-1" --no-refresh --dry-run --margin-mb 0 --wait-idle 0.0012 > "$OS/w1.out" 2>&1; rc=$?
if [ $rc -eq 2 ] && grep -q "DESISTI" "$OS/w1.out" && [ ! -e "$OS/w1/all-1/data.jsonl" -o ! -s "$OS/w1/all-1/data.jsonl" ]; then echo "--wait-idle com o agente trabalhando: desistiu no limite sem treinar (codigo 2): ok"; else echo "--wait-idle ocupado: FALHOU (codigo $rc)"; tail -3 "$OS/w1.out"; fail=1; fi
touch -d '2 hours ago' "$H3/.katana/desktop/sessoes/x.ksess"
KATANA_IDLE_POLL=1 KATANA_TOOLS=2 HOME="$H3" $K specialize "$SM" --from "$D/sel" --out "$OS/w2/all-1" --no-refresh --dry-run --margin-mb 0 --wait-idle 0.05 > "$OS/w2.out" 2>&1; rc=$?
if [ $rc -eq 0 ] && grep -q "agente parado ha mais de" "$OS/w2.out" && grep -q "dry-run: parei" "$OS/w2.out"; then echo "--wait-idle com a sessao parada ha 2 h: seguiu depois de 3 consultas: ok"; else echo "--wait-idle parado: FALHOU (codigo $rc)"; tail -3 "$OS/w2.out"; fail=1; fi
kill $FAKE 2>/dev/null; wait $FAKE 2>/dev/null
if pgrep -f "katana agent " >/dev/null 2>&1; then echo "--wait-idle sem agente rodando: PULADO (ha um katana agent de verdade rodando nesta maquina)"; else
KATANA_TOOLS=2 HOME="$H3" $K specialize "$SM" --from "$D/sel" --out "$OS/w3/all-1" --no-refresh --dry-run --margin-mb 0 --wait-idle > "$OS/w3.out" 2>&1; rc=$?
if [ $rc -eq 0 ] && grep -q "nenhum agente rodando: sigo" "$OS/w3.out"; then echo "--wait-idle sem agente rodando: seguiu na hora: ok"; else echo "--wait-idle sem agente: FALHOU (codigo $rc)"; fail=1; fi; fi
# --- 4. katana knowledge quiz ---
python3 - "$OS" <<'PY'
import json, sys
O = sys.argv[1]
qa = [("Where do the specializations live?", "They live in ~/.katana/specs and the file is katana.kspec."),
      ("How is the model trained?", "With `katana specialize` on 35 conversations, 512 windows."),
      ("Which models are the references?", "Qwen3.6 and Gemma, both on Linux."),
      ("Which command lists the notes?", "Run `katana knowledge search` inside ~/.katana/knowledge.")]
open(O + "/hold.jsonl", "w").write("".join(json.dumps({"messages": [{"role": "user", "content": q}, {"role": "assistant", "content": a}], "project": "p"}) + "\n" for q, a in qa))
open(O + "/ans_perfect.txt", "w").write("".join(a + "\n" for q, a in qa))
open(O + "/ans_half.txt", "w").write("They live in ~/.katana/specs.\nSomething else entirely.\nQwen3.6 and Gemma.\nnothing\n")
open(O + "/ans_wrong.txt", "w").write("I do not know.\nNo idea.\nUnknown.\nNothing.\n")
PY
$K knowledge quiz "$SM" "$OS/hold.jsonl" --limit 3 --max-tokens 6 --out "$OS/quiz_tiny.jsonl" > "$OS/q_tiny.out" 2>&1; rc=$?
if [ $rc -eq 0 ] && python3 - "$OS/q_tiny.out" "$OS/quiz_tiny.jsonl" <<'PY'
import re, sys, json
t = open(sys.argv[1]).read()
m = re.search(r"quiz: 3 perguntas, (\d+) com nota.*nota media ([0-9.]+), (\d+) com nota >= 0.50", t)
rows = [json.loads(l) for l in open(sys.argv[2])]
sys.exit(0 if m and float(m.group(2)) < 0.3 and len(rows) == 3 and all("score" in r for r in rows) else 1)
PY
then echo "knowledge quiz no modelo minusculo (o comando roda, notas perto de zero, arquivo de respostas): ok"; else echo "knowledge quiz no modelo minusculo: FALHOU"; tail -3 "$OS/q_tiny.out"; fail=1; fi
qz() { KATANA_QUIZ_ANSWERS="$OS/ans_$1.txt" $K knowledge quiz "$SM" "$OS/hold.jsonl" 2>&1; }
qz perfect | grep -q "nota media 1.000, 4 com nota >= 0.50 (100%)" && qz wrong | grep -q "nota media 0.000, 0 com nota >= 0.50 (0%)" && qz half | grep -q "nota media 0\.[12][0-9]*, [12] com nota >= 0.50" \
  && echo "knowledge quiz com respostas injetadas (perfeita 1.000, errada 0.000, metade entre as duas): ok" || { echo "knowledge quiz injetado: FALHOU"; qz perfect; qz half; fail=1; }
# --- 5. fonte conventions (--curriculum): tres conversas escritas a mao; o prompt de sistema delas fica ---
python3 - "$OS/curr.jsonl" <<'PY'
import json, sys
sysm = "You are an agent. PROJECT-FILE-MARKER: run `make check` before every commit. Skills: deploy-skill. MCP servers: notes-card."
rows = []
for q, a in [("what do I run before a commit?", "Run `make check` first, as the project file says."),
             ("how do I deploy?", "Use the deploy-skill."),
             ("how do I read my notes?", "Call the notes MCP server.")]:
    rows.append({"messages": [{"role": "system", "content": sysm}, {"role": "user", "content": q}, {"role": "assistant", "content": a}], "source": "curriculum", "family": "t"})
open(sys.argv[1], "w").write("".join(json.dumps(r) + "\n" for r in rows))
PY
KATANA_TOOLS=2 HOME="$H1" $K specialize "$SM" --out "$OS/cv/all-1" --from "$D/sel" --curriculum "$OS/curr.jsonl" --no-refresh --dry-run --margin-mb 0 > "$OS/cv.out" 2>&1 || true
if python3 - "$OS/cv/all-1/data.jsonl" <<'PY'
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
c = [r for r in rows if r["source"] == "conventions"]
o = [r for r in rows if r["source"] != "conventions"]
ok = len(c) == 3 and all("PROJECT-FILE-MARKER" in r["text"] for r in c) and all("PROJECT-FILE-MARKER" not in r["text"] for r in o)
ok = ok and all(r["train_spans"] for r in c) and any(r["source"] == "tools" for r in rows)
sys.exit(0 if ok else 1)
PY
then echo "conventions: 3 conversas com o prompt de sistema proprio mantido, as outras fontes com o do agente: ok"; else echo "conventions: FALHOU"; tail -5 "$OS/cv.out"; fail=1; fi
if $K curriculum --help >/dev/null 2>&1; then echo "katana curriculum existe: geracao automatica na noite (nao testada aqui)"; else
  KATANA_TOOLS=2 HOME="$H1" $K specialize "$SM" --out "$OS/cv/all-2" --from "$D/sel" --no-refresh --dry-run --margin-mb 0 > "$OS/cv2.out" 2>&1 || true
  grep -q "ainda nao tem o subcomando katana curriculum" "$OS/cv2.out" && echo "sem katana curriculum: a fonte fica de fora com uma linha dizendo: ok" || { echo "conventions sem subcomando: FALHOU"; fail=1; }; fi
KATANA_TOOLS=2 HOME="$H1" $K specialize "$SM" conventions --out "$OS/trn/conventions-1" --curriculum "$OS/curr.jsonl" --steps 4 --seq 256 --lr 3e-3 --margin-mb 0 --no-check > "$OS/cv3.out" 2>&1 || true
[ -s "$OS/trn/conventions-1/conventions.kspec" ] && grep -q "conventions" "$OS/trn/history.jsonl" && echo "specialize conventions (so a fonte, 4 passos): ok" || { echo "specialize conventions: FALHOU"; tail -4 "$OS/cv3.out"; fail=1; }
set -e
