# katana codebench sem modelo (lido por tools/traintest/run.sh com ". tools/traintest/codebench_part.sh"; usa $K, $D e fail)
#  a. verify em todas as tarefas: cada uma falha intacta e passa com a solucao; nenhuma pasta temporaria sobra
#  b. compare em dois arquivos escritos a mao, com resposta conhecida (inclusive linhas sem as medidas novas)
#  c. um agente de mentira (--katana) que trava: o prazo mata o grupo inteiro e a tarefa conta como estouro
#  d. uma tarefa cujo preparo deixa um servidor em segundo plano e cujo agente deixa outro processo: os dois somem
#  e. medidas do registro do agente (chamadas, falhas, recusadas, repetidas, rodadas, ultimo turno em texto, tokens)
#  f. preparo que falha: a tarefa vira uma linha "skipped"
CBT="$D/cbtmp"; mkdir -p "$CBT"
if TMPDIR="$CBT" $K codebench verify > "$D/cb_verify.txt" 2>&1 && grep -q "^63 tasks, 0 broken" "$D/cb_verify.txt" && [ -z "$(ls "$CBT" | grep '^cb_')" ]; then
  echo "codebench verify: ok ($(tail -1 "$D/cb_verify.txt"))"
else echo "codebench verify: FALHOU"; grep -B1 -A3 "BROKEN\|skipped" "$D/cb_verify.txt" | head -20; ls "$CBT" | grep '^cb_' | head -3; fail=1; fi
# b. compare
cat > "$D/cb_a.jsonl" <<'EOF'
{"label": "a", "task": "t1", "passed": true, "seconds": 10.0, "tool_calls": 4, "gen_tokens": 100, "timed_out": false}
{"label": "a", "task": "t2", "passed": false, "seconds": 20.0, "tool_calls": 6, "gen_tokens": 200, "timed_out": false}
{"label": "a", "task": "t3", "passed": false, "seconds": 30.0, "tool_calls": 8, "gen_tokens": 300, "timed_out": false}
{"label": "a", "task": "t4", "passed": true, "seconds": 40.0, "tool_calls": 2, "gen_tokens": 400, "timed_out": false}
{"label": "a", "task": "t5", "passed": false, "seconds": 50.0, "tool_calls": 0, "gen_tokens": 0, "timed_out": true}
{"label": "a", "task": "t6", "passed": false, "seconds": 60.0, "tool_calls": 4, "gen_tokens": 600, "timed_out": false}
{"label": "a", "task": "t7", "passed": true, "seconds": 1.0, "tool_calls": 1, "gen_tokens": 1, "timed_out": false}
EOF
cat > "$D/cb_b.jsonl" <<'EOF'
{"label": "b", "task": "t1", "passed": true, "seconds": 10.0, "tool_calls": 4, "gen_tokens": 100, "timed_out": false, "calls": 4, "failed_calls": 1, "refused_calls": 0, "repeated_calls": 1, "rounds": 5, "last_text": true, "tokens_read": 900, "tokens_written": 100}
{"label": "b", "task": "t2", "passed": true, "seconds": 20.0, "tool_calls": 6, "gen_tokens": 200, "timed_out": false, "calls": 6, "failed_calls": 2, "refused_calls": 1, "repeated_calls": 0, "rounds": 7, "last_text": true, "tokens_read": 1900, "tokens_written": 200}
{"label": "b", "task": "t3", "passed": true, "seconds": 30.0, "tool_calls": 8, "gen_tokens": 300, "timed_out": false, "calls": 8, "failed_calls": 0, "refused_calls": 0, "repeated_calls": 0, "rounds": 9, "last_text": false, "tokens_read": 2900, "tokens_written": 300}
{"label": "b", "task": "t4", "passed": false, "seconds": 40.0, "tool_calls": 2, "gen_tokens": 400, "timed_out": false, "calls": 2, "failed_calls": 1, "refused_calls": 0, "repeated_calls": 0, "rounds": 3, "last_text": true, "tokens_read": 3900, "tokens_written": 400}
{"label": "b", "task": "t5", "passed": true, "seconds": 50.0, "tool_calls": 0, "gen_tokens": 0, "timed_out": false, "calls": 0, "failed_calls": 0, "refused_calls": 0, "repeated_calls": 0, "rounds": 1, "last_text": true, "tokens_read": 100, "tokens_written": 10}
{"label": "b", "task": "t6", "passed": true, "seconds": 60.0, "tool_calls": 4, "gen_tokens": 600, "timed_out": false, "calls": 4, "failed_calls": 0, "refused_calls": 0, "repeated_calls": 0, "rounds": 5, "last_text": true, "tokens_read": 500, "tokens_written": 600}
{"label": "b", "task": "t8", "passed": true, "seconds": 1.0, "tool_calls": 1, "gen_tokens": 1, "timed_out": false}
EOF
$K codebench compare "$D/cb_a.jsonl" "$D/cb_b.jsonl" > "$D/cb_cmp.txt" 2>&1 || true
# 6 tarefas em comum; A resolve t1 e t4 (2), B resolve t1, t2, t3, t5, t6 (5); B ganha 4 e perde 1: p = 2 * (1 + 5) / 32 = 0.375
if grep -qx "tasks in common: 6" "$D/cb_cmp.txt" && grep -qF "solved   B ($D/cb_b.jsonl): 5/6 solved" "$D/cb_cmp.txt" && grep -qF "A ($D/cb_a.jsonl): 2/6 solved" "$D/cb_cmp.txt" \
   && grep -qx "newly solved by B: 4 \['t2', 't3', 't5', 't6'\]" "$D/cb_cmp.txt" && grep -qx "lost by B:         1 \['t4'\]" "$D/cb_cmp.txt" \
   && grep -q "exact sign test p = 0.3750" "$D/cb_cmp.txt" && grep -q "mean seconds     A 35.0   B 35.0" "$D/cb_cmp.txt" \
   && grep -q "mean calls *A -   B 4.0" "$D/cb_cmp.txt" && grep -q "last turn was text *A 0/0   B 5/6" "$D/cb_cmp.txt"; then echo "codebench compare: ok (4 ganhas, 1 perdida, p = 0.3750)"
else echo "codebench compare: FALHOU"; cat "$D/cb_cmp.txt"; fail=1; fi
# c, d, e, f: tarefas de mentira e um agente de mentira
FX="$D/cbfx"; mkdir -p "$FX"
for t in hang bg metrics nosetup; do
  mkdir -p "$FX/tasks/$t/repo" "$FX/tasks/$t/hidden"
  echo "task $t: do it" > "$FX/tasks/$t/instruction.md"; : > "$FX/tasks/$t/repo/.keep"
  printf 'import os, unittest\nclass T(unittest.TestCase):\n    def test_done(self):\n        self.assertTrue(os.path.exists("done.txt"))\n' > "$FX/tasks/$t/hidden/test_hidden_$t.py"
done
cat > "$FX/tasks/bg/setup.py" <<'EOF'
import os, subprocess, sys

def setup(work):
    p = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(4712)"], start_new_session=True)
    open(os.environ["CBFX_PID"], "w").write(str(p.pid))

    def stop():
        p.terminate()
        p.wait()
    return dict(vars={}, stop=stop)
EOF
cat > "$FX/tasks/nosetup/setup.py" <<'EOF'
def setup(work):
    raise RuntimeError("sem internet de mentira")
EOF
cat > "$FX/agent.sh" <<'EOF'
#!/bin/sh
# agente de mentira do teste do codebench (o juiz o chama no lugar do katana: agent MODELO)
case "$STANDIN" in
hang) sleep 4711 & sleep 4711 ;;
bg) sleep 4713 & echo $! > "$CBFX_BGPID"; touch done.txt ;;
metrics)
  printf '2026-11-21 10:00:0%s\tbash\t{"cmd": "ls"}\t[exit code 0]\n' 1 >> "$KATANA_LOG"
  printf '2026-11-21 10:00:0%s\tbash\t{"cmd": "ls"}\t[exit code 0]\n' 2 >> "$KATANA_LOG"
  printf '2026-11-21 10:00:0%s\tread_file\t{"path": "x"}\terror: no such file\n' 3 >> "$KATANA_LOG"
  {
    echo "[entrada: 100 tokens, 1.0 tok/s | resposta: 10 tokens, 1.0 tok/s | contexto 110 de 4096 (2%)]"
    echo "[ferramenta bash {\"cmd\": \"ls\"}]"; echo "[ferramenta bash: 20 bytes de resultado]"
    echo "[ferramenta bash {\"cmd\": \"ls\"}]"; echo "[ferramenta bash: 20 bytes de resultado]"
    echo "[ferramenta read_file {\"path\": \"x\"}]"; echo "[ferramenta read_file: 30 bytes de resultado]"
    echo "[ferramenta bash {\"cmd\": \"ls\"}]"; echo "[ferramenta bash: 200 bytes de resultado]"
    echo "[entrada: 50 tokens, 1.0 tok/s | resposta: 7 tokens, 1.0 tok/s | contexto 170 de 4096 (4%)]"
  } >&2
  touch done.txt ;;
esac
EOF
chmod +x "$FX/agent.sh"
cbrun() { TMPDIR="$CBT" CBFX_PID="$FX/pid" CBFX_BGPID="$FX/bgpid" $K codebench run --dir "$FX" --katana "$FX/agent.sh" --model "$D/llama.gguf" "$@" >> "$FX/run.out" 2>&1; }
T0=$(date +%s)
cbrun --tasks hang --timeout 2 --out "$FX/hang.jsonl" --env STANDIN=hang
T1=$(date +%s)
if [ $((T1 - T0)) -lt 12 ] && grep -q '"passed": false' "$FX/hang.jsonl" && grep -q '"timed_out": true' "$FX/hang.jsonl" && ! pgrep -f "sleep 4711" > /dev/null; then echo "codebench prazo: ok (agente que trava morto em $((T1 - T0)) s, com o grupo inteiro)"
else echo "codebench prazo: FALHOU"; cat "$FX/hang.jsonl"; fail=1; pkill -f "sleep 4711" 2>/dev/null || true; fi
cbrun --tasks bg --timeout 20 --out "$FX/bg.jsonl" --env STANDIN=bg
sleep 1
if grep -q '"passed": true' "$FX/bg.jsonl" && [ -s "$FX/pid" ] && ! kill -0 "$(cat "$FX/pid")" 2>/dev/null && ! kill -0 "$(cat "$FX/bgpid")" 2>/dev/null \
   && [ -z "$(ls "$CBT" | grep '^cb_')" ]; then echo "codebench limpeza: ok (servidor do preparo e processo do agente encerrados, sem pasta temporaria)"
else echo "codebench limpeza: FALHOU"; cat "$FX/bg.jsonl"; ls "$CBT" | grep '^cb_'; fail=1; kill "$(cat "$FX/pid")" "$(cat "$FX/bgpid")" 2>/dev/null || true; fi
cbrun --tasks metrics --timeout 20 --out "$FX/metrics.jsonl" --env STANDIN=metrics
cbrun --tasks nosetup --timeout 20 --out "$FX/nosetup.jsonl"
python3 - "$FX/metrics.jsonl" "$FX/nosetup.jsonl" <<'EOF' || fail=1
import json, sys
r = json.loads(open(sys.argv[1]).readline())
s = json.loads(open(sys.argv[2]).readline())
want = dict(passed=True, calls=3, failed_calls=1, refused_calls=1, repeated_calls=1, rounds=2, last_text=True, tokens_read=150, tokens_written=17, gen_tokens=17, tool_calls=4, timed_out=False)
bad = {k: (r.get(k), v) for k, v in want.items() if r.get(k) != v}
ok = not bad and list(r)[:7] == ["label", "task", "passed", "seconds", "tool_calls", "gen_tokens", "timed_out"] and s.get("skipped") is True and s["passed"] is False
print("codebench medidas do registro e tarefa pulada: %s" % ("ok" if ok else "FALHOU %s %s" % (bad, s)))
sys.exit(0 if ok else 1)
EOF
