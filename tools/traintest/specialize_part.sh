# selecao do import (--min-calls, --drop-failed-streak, --verified, --require-tools, --train-tools, --train-final,
# --train-first, --plan-first, --plan-updates) e katana specialize nos tres tipos, no modelo minusculo
mkdir -p "$D/sel/projA" "$D/sel/projB"
python3 - "$D/sel" <<'EOF'
import json, sys
R = []; N = [0]
def nid(): N[0] += 1; return N[0]
def U(c): R.append({"type": "user", "uuid": "u%d" % nid(), "message": {"role": "user", "content": c}})
def A(blocks): R.append({"type": "assistant", "uuid": "a%d" % nid(), "message": {"role": "assistant", "id": "m%d" % N[0], "content": blocks}})
def call(name, inp, res):
    tid = "t%d" % nid()
    A([{"type": "tool_use", "id": tid, "name": name, "input": inp}])
    U([{"type": "tool_result", "tool_use_id": tid, "content": res}])
def ep(req, calls, final):
    U(req)
    for c in calls: call(*c)
    A([{"type": "text", "text": final}])
def dump(path):
    global R
    open(path, "w").write("".join(json.dumps(r) + "\n" for r in R)); R = []
# A: le, escreve, compila e roda (verificada)
ep("write hello.c and run it", [("Bash", {"command": "ls", "description": "List the project files"}, "x.txt"),
   ("Write", {"file_path": "hello.c", "content": "int main(){return 0;}"}, "File created successfully at: hello.c"),
   ("Bash", {"command": "gcc hello.c", "description": "Compile hello.c"}, "ok"),
   ("Bash", {"command": "./a.out", "description": "Run the program"}, "hi")], "Done: hello.c compiles and runs.")
# C: so duas chamadas
ep("what is in x.txt", [("Read", {"file_path": "x.txt"}, "abc"), ("Bash", {"command": "wc x.txt", "description": "Count lines"}, "1")], "It holds abc.")
dump(sys.argv[1] + "/projA/a.jsonl")
# B: quatro falhas seguidas
ep("build it", [("Bash", {"command": "make", "description": "Build"}, "error 1\nExit code 2")] * 4 + [("Write", {"file_path": "m", "content": "x"}, "ok")], "I could not build it.")
# D: escreve e o comando depois falha (nao verificada)
ep("fix y.c", [("Read", {"file_path": "y.c"}, "int y;"), ("Edit", {"file_path": "y.c", "old_string": "y", "new_string": "z"}, "ok"),
   ("Bash", {"command": "gcc -c y.c", "description": "Compile y.c"}, "y.c:1: error\nExit code 1"),
   ("Bash", {"command": "gcc -c y.c -w", "description": "Compile y.c again"}, "bad\nExit code 1")], "It still fails.")
dump(sys.argv[1] + "/projB/b.jsonl")
EOF
SM="$D/agent.gguf"
sel() { out=$1; shift; $K import "$SM" "$D/sel" "$D/sel_$out.jsonl" --system none "$@" > "$D/sel_$out.log" 2>&1 || true; }
sel base
sel mc --min-calls 4 --drop-failed-streak 4
sel ver --verified
sel req --require-tools Grep
sel tt --train-tools Write,Edit --train-final
sel tf --train-first 1
sel plan --plan-first --plan-updates --min-calls 4 --drop-failed-streak 4 --train-first 2 --train-tools TodoWrite,Write,Edit --train-final
if python3 - "$D" <<'EOF'
import json, sys
D = sys.argv[1]
def L(n): return open("%s/sel_%s.log" % (D, n)).read()
def J(n):
    try: return [json.loads(l) for l in open("%s/sel_%s.jsonl" % (D, n))]
    except FileNotFoundError: return []
bad = []
def chk(c, what):
    if not c: bad.append(what)
chk(len(J("base")) == 4 and "pasta projA: 2 episodios, 2 gravados" in L("base") and "pasta projB: 2 episodios, 2 gravados" in L("base"), "base e contagem por pasta")
chk(len(J("mc")) == 2 and "1  fewer tool calls than --min-calls" in L("mc") and "1  a streak of failed tool results" in L("mc"), "min-calls e drop-failed-streak")
v = J("ver"); chk(len(v) == 1 and "hello.c" in v[0]["text"] and "no successful Bash after the last Write/Edit" in L("ver") and "no Write/Edit call" in L("ver"), "verified")
chk(not J("req") and "4  calls none of the --require-tools" in L("req"), "require-tools")
t = J("tt"); chk([len(x["train_spans"]) for x in t] == [2, 1, 2, 2] and "Write" in L("tt") and "import_stats: kept=4" in L("tt"), "train-tools e train-final")
chk([len(x["train_spans"]) for x in J("tf")] == [1, 1, 1, 1], "train-first")
p = J("plan"); txt = p[0]["text"] if p else ""
chk(len(p) == 2 and txt.count("<function=TodoWrite>") == 4 and "Todos updated: 4 tasks (0 completed, 1 in progress, 3 pending)." in txt
    and '"content": "List the project files", "status": "in_progress"' in txt and "Compile hello.c" in txt
    and "<parameter=description>\nCompile hello.c" in txt and "conversas com plano inserido (--plan-first, derivado das chamadas): 2" in L("plan"), "plan-first")
if p:
    a, b = p[0]["train_spans"][0]
    print("plano (primeira fala): %d caracteres, cerca de %d tokens (estimativa, 3,3 caracteres por token)" % (b - a, (b - a) / 3.3))
    chk("TodoWrite" in txt[a:b], "o plano conta para a perda")
if bad: print("falhou:", bad)
sys.exit(1 if bad else 0)
EOF
then echo "selecao do import (min-calls, falhas seguidas, verified, require/train-tools, train-final/first, plan-first): ok"; else echo "selecao do import: FALHOU"; fail=1; fi
# katana specialize: tools (transcricoes), code (planejado) e teacher (sessoes com .teacher), poucos passos
mkdir -p "$D/sp_sess"
[ -f "$D/tch.ksess" ] && cp "$D/tch.ksess" "$D/tch.ksess.teacher" "$D/sp_sess/" 2>/dev/null || true
for kind in tools code teacher; do
  if [ $kind = teacher ]; then src="--sessions $D/sp_sess"; else src="--from $D/sel"; fi
  HOME="$D/home" $K specialize "$SM" $kind $src --out "$D/sp_$kind" --steps 3 --seq 256 --margin-mb 0 --replace 'hello=HELLO' > "$D/sp_$kind.out" 2>&1 || true
  if [ -s "$D/sp_$kind/$kind.kspec" ] && grep -q "KATANA_SPEC=$D/sp_$kind/$kind.kspec" "$D/sp_$kind/report.txt" && grep -q '"steps": 3' "$D/sp_$kind/train.log" \
     && [ -s "$D/sp_$kind/plan.log" ] && [ -s "$D/sp_$kind/steps.jsonl" ]; then echo "specialize $kind: ok"; else echo "specialize $kind: FALHOU"; tail -5 "$D/sp_$kind.out"; fail=1; fi
done
grep -q "TodoWrite" "$D/sp_code/data.jsonl" && grep -q "HELLO" "$D/sp_code/data.jsonl" && ! grep -q "<tools>" "$D/sp_tools/sys.txt" && grep -q "<tools>" "$D/sp_code/sys.txt" \
  && echo "specialize: plano nos dados de code, troca aplicada, prompt de tools sem declaracoes: ok" || { echo "specialize (dados): FALHOU"; fail=1; }
HOME="$D/home" $K specialize "$SM" code --from "$D/sel" --out "$D/sp_dry" --dry-run --margin-mb 0 > /dev/null 2>&1 || true
[ -s "$D/sp_dry/plan.log" ] && [ ! -e "$D/sp_dry/code.kspec" ] && echo "specialize --dry-run: ok" || { echo "specialize --dry-run: FALHOU"; fail=1; }
HOME="$D/home" $K specialize "$SM" code --from "$D/sel" --out "$D/sp_mem" --dry-run --margin-mb 99999999 > /dev/null 2>&1 && { echo "specialize sem memoria: FALHOU"; fail=1; } || echo "specialize recusa sem memoria: ok"
# conhecimento (corpus de perguntas e respostas), all com mistura, retomada entre noites, marca no .kspec e no GGUF
python3 - "$D/corpus.jsonl" <<'PY'
import json, sys
qa = [("what is katana?", "A local inference engine written in C."), ("where do specializations live?", "In ~/.katana/specs."),
      ("which models are the references?", "Qwen3.6-35B-A3B and Gemma 4 26B-A4B.")]
open(sys.argv[1], "w").write("".join(json.dumps({"messages": [{"role": "user", "content": q}, {"role": "assistant", "content": a}]}) + "\n" for q, a in qa))
PY
HOME="$D/home" $K specialize "$SM" knowledge --corpus "$D/corpus.jsonl" --out "$D/sp_know" --steps 3 --seq 256 --margin-mb 0 --no-check > "$D/sp_know.out" 2>&1 || true
[ -s "$D/sp_know/knowledge.kspec" ] && grep -q '"source": "knowledge"' "$D/sp_know/data.jsonl" && echo "specialize knowledge: ok" || { echo "specialize knowledge: FALHOU"; tail -5 "$D/sp_know.out"; fail=1; }
SPA="--from $D/sel --sessions $D/sp_sess --corpus $D/corpus.jsonl --steps 20 --seq 256 --lr 3e-3 --margin-mb 0 --no-check --mix tools=50,code=30,knowledge=20,teacher=0"
HOME="$D/home" $K specialize "$SM" $SPA --out "$D/sp_root/all-1" > "$D/sp_all1.out" 2>&1 || true
sleep 1
HOME="$D/home" $K specialize "$SM" $SPA --out "$D/sp_root/all-2" > "$D/sp_all2.out" 2>&1 || true
if python3 - "$D" <<'PY'
import json, sys
D = sys.argv[1]
h = [json.loads(l) for l in open(D + "/sp_root/history.jsonl")]
t2 = open(D + "/sp_root/all-2/train.log").read()
mix = [l for l in t2.splitlines() if l.startswith("mistura realizada:")]
ok = len(h) == 2 and h[0]["resumed_from"] is None and h[1]["resumed_from"].endswith("all-1/katana.kspec") and "retomado de" in t2
ok = ok and h[1]["loss_first10"] < h[0]["loss_first10"] - 0.5 and set(h[0]["sources"]) >= {"tools", "code", "knowledge"}
ok = ok and bool(mix) and "tools 10 janelas (50%)" in mix[0] and "code 6 janelas (30%)" in mix[0] and "knowledge 4 janelas (20%)" in mix[0]
print("all: noite 1 perda %.3f -> %.3f; noite 2 (retomada) comeca em %.3f; %s" % (h[0]["loss_first10"], h[0]["loss_last10"], h[1]["loss_first10"], mix[0] if mix else "sem mistura"))
fl = lambda p: open(p, "rb").read()[-8:]
ok = ok and fl(D + "/sp_root/all-2/katana.kspec") == b"KSXF\x01\x00\x00\x00" and fl(D + "/sp_code/code.kspec") == b"KSXF\x00\x00\x00\x00"
sys.exit(0 if ok else 1)
PY
then echo "specialize all: ok (mistura exata, segunda noite retoma da primeira, historico, marca no .kspec)"; else echo "specialize all: FALHOU"; tail -5 "$D/sp_all2.out"; fail=1; fi
# a marca tira a secao de ferramentas do prompt do agente: com KATANA_SPEC e no GGUF fundido (katana merge)
$K merge "$SM" "$D/sp_root/all-2/katana.kspec" "$D/sp_merged.gguf" >/dev/null 2>&1 || true
mkdir -p "$D/sp_ag"
(cd "$D/sp_ag" && printf 'oi\n/exit\n' | env HOME="$D/home" KATANA_MAXTOK=4 $K agent "$SM" --sessao "$D/sp_ag/base.ksess" >/dev/null 2>&1) || true
(cd "$D/sp_ag" && printf 'oi\n/exit\n' | env HOME="$D/home" KATANA_MAXTOK=4 KATANA_SPEC="$D/sp_root/all-2/katana.kspec" $K agent "$SM" --sessao "$D/sp_ag/spec.ksess" >/dev/null 2>&1) || true
(cd "$D/sp_ag" && printf 'oi\n/exit\n' | env HOME="$D/home" KATANA_MAXTOK=4 $K agent "$D/sp_merged.gguf" --sessao "$D/sp_ag/merged.ksess" >/dev/null 2>&1) || true
if python3 - "$D/sp_ag" <<'PY'
import sys
r = lambda n: open(sys.argv[1] + "/" + n, "rb").read()
sys.exit(0 if b"<tools>" in r("base.ksess") and b"<tools>" not in r("spec.ksess") and b"<tools>" not in r("merged.ksess") else 1)
PY
then echo "marca sem declaracoes de ferramentas (KATANA_SPEC e GGUF fundido): ok"; else echo "marca sem declaracoes: FALHOU"; fail=1; fi
