#!/bin/sh
# Teste do treino do katana sem modelo de verdade (segundos, qualquer maquina):
#  1. gradcheck nos tres layouts do modelo minusculo (llama; qwen2 com GQA e vies; base quantizada Q8_0)
#  2. treino com tensores inteiros e com adaptadores sobre a base quantizada
#  3. o servidor com KATANA_SPEC responde exatamente o que foi treinado (temperatura 0)
# Uso: sh tools/traintest/run.sh   (a partir da raiz do repositorio, com ./katana compilado)
set -e
K=./katana
D=$(mktemp -d)
trap 'rm -rf "$D"; kill $SRV 2>/dev/null || true' EXIT
python3 tools/tinymodel/make_tiny_gguf.py "$D/llama.gguf" >/dev/null
python3 tools/tinymodel/make_tiny_gguf.py "$D/qwen2.gguf" --arch qwen2 --kv 2 >/dev/null
$K quantize "$D/llama.gguf" "$D/q8.gguf" Q8_0 >/dev/null 2>&1
fail=0
for m in llama qwen2 q8; do
  if $K gradcheck "$D/$m.gguf" 12 2>/dev/null | grep -q "gradcheck: ok"; then echo "gradcheck $m: ok"; else echo "gradcheck $m: FALHOU"; fail=1; fi
done
python3 - "$D/sft.jsonl" <<'EOF'
import json, sys
sys_ = "<|im_start|>system\nYou are katana.<|im_end|>\n"
rows = []
for q, a in [("what is katana?", "katana is a local engine."), ("who made it?", "Ale made it in C."), ("say hi", "hi there!")]:
    pre = sys_ + "<|im_start|>user\n" + q + "<|im_end|>\n<|im_start|>assistant\n"
    text = pre + a + "<|im_end|>"
    rows.append({"text": text, "train_spans": [[len(pre), len(text)]]})
open(sys.argv[1], "w").write("\n".join(json.dumps(r) for r in rows) + "\n")
EOF
check_spec() {   # modelo, especializacao, porta
  KATANA_SPEC="$2" $K serve "$1" "$3" >"$D/srv.log" 2>&1 & SRV=$!
  sleep 1
  python3 - "$3" <<'EOF' || fail=1
import json, sys, urllib.request
want = {"what is katana?": "katana is a local engine.", "who made it?": "Ale made it in C.", "say hi": "hi there!"}
bad = 0
for q, a in want.items():
    body = {"messages": [{"role": "system", "content": "You are katana."}, {"role": "user", "content": q}], "temperature": 0, "max_tokens": 30}
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
if [ $fail = 0 ]; then echo "traintest: ok"; else echo "traintest: FALHOU"; exit 1; fi
