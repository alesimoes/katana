#!/bin/sh
# Teste do treino do katana sem modelo de verdade (segundos, qualquer maquina):
#  1. gradcheck nos layouts do modelo minusculo: llama; qwen2 (GQA, vies); base quantizada Q8_0; qwen3 (norma de
#     q/k por cabeca); qwen3moe com compartilhado e camada densa inicial; olmoe (norma no vetor, MoE sem renormalizar);
#     qwen35moe (layout do Qwen3.6: Gated DeltaNet + atencao com portao + MoE) e qwen35 denso
#  2. treino com tensores inteiros, com adaptadores sobre a base quantizada e com especialistas quentes num MoE
#  3. o servidor com KATANA_SPEC responde exatamente o que foi treinado (temperatura 0)
# Uso: sh tools/traintest/run.sh   (a partir da raiz do repositorio, com ./katana compilado)
set -e
K=./katana
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
fail=0
for m in llama qwen2 q8 qwen3 moe olmoe q35 q35d; do
  if $K gradcheck "$D/$m.gguf" 12 2>/dev/null | grep -q "gradcheck: ok"; then echo "gradcheck $m: ok"; else echo "gradcheck $m: FALHOU"; fail=1; fi
done
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
  sleep 1
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
$K train "$D/moe.gguf" "$D/sft.jsonl" --experts 2 --train "blk.*.attn_*,blk.*.ffn_gate_inp.weight,blk.*shexp*,output_norm.weight" --rank 8 \
   --steps 500 --lr 5e-3 --seq 128 --out "$D/moe.kspec" 2>/dev/null | grep -E "quentes|perda media final"
check_spec "$D/moe.gguf" "$D/moe.kspec" 8193
$K train "$D/q35.gguf" "$D/sft_think.jsonl" --rank 8 --train "blk.*,output_norm.weight" --steps 400 --lr 5e-3 --seq 128 \
   --out "$D/q35.kspec" 2>/dev/null | grep "perda media final"
check_spec "$D/q35.gguf" "$D/q35.kspec" 8194
if [ $fail = 0 ]; then echo "traintest: ok"; else echo "traintest: FALHOU"; exit 1; fi
