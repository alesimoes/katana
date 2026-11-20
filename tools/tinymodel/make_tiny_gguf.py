#!/usr/bin/env python3
"""make_tiny_gguf: writes a tiny llama-architecture GGUF with random weights (about 1 MB).

It produces nonsense text, on purpose: it exists to test everything around the model (the HTTP server and its
APIs, streaming, stop sequences, queues, sessions, tokenizer plumbing) on any machine in a second, without
downloading a real model. Byte-level BPE vocabulary (every byte has a token), ChatML special tokens and a ChatML
chat template, so katana treats it like a Qwen-style chat model.

  python3 tools/tinymodel/make_tiny_gguf.py /tmp/tiny.gguf [--seed 1] [--no-eos]

--no-eos makes the end-of-turn token unlikely, so answers run until max_tokens (for testing length limits).
--arch qwen2 --kv 2 gives the other attention layout (RoPE in halves, q/k/v bias, grouped-query attention).
--arch qwen3 adds per-head q/k norm; --arch qwen3moe / olmoe give mixture-of-experts layers (--experts, --used,
--shared, --dense). --arch qwen35moe (Qwen3.6 layout) / qwen35 (dense): Gated DeltaNet layers alternating with gated
attention (partial RoPE, per-head q/k norm, sigmoid output gate); qwen35moe uses MoE with a gated shared expert.
"""
import argparse, random, struct, sys


def bytes_to_unicode():
    bs = list(range(ord("!"), ord("~") + 1)) + list(range(ord("¡"), ord("¬") + 1)) + list(range(ord("®"), ord("ÿ") + 1))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256 + n)
            n += 1
    return {b: chr(c) for b, c in zip(bs, cs)}


def s(x):
    b = x.encode("utf-8")
    return struct.pack("<Q", len(b)) + b


def kv_str(k, v): return s(k) + struct.pack("<I", 8) + s(v)
def kv_u32(k, v): return s(k) + struct.pack("<I", 4) + struct.pack("<I", v)
def kv_f32(k, v): return s(k) + struct.pack("<I", 6) + struct.pack("<f", v)
def kv_bool(k, v): return s(k) + struct.pack("<I", 7) + struct.pack("<B", 1 if v else 0)


def kv_arr_str(k, vals):
    return s(k) + struct.pack("<I", 9) + struct.pack("<IQ", 8, len(vals)) + b"".join(s(v) for v in vals)


def kv_arr_i32(k, vals):
    return s(k) + struct.pack("<I", 9) + struct.pack("<IQ", 5, len(vals)) + b"".join(struct.pack("<i", v) for v in vals)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--no-eos", action="store_true")
    ap.add_argument("--arch", default="llama", choices=["llama", "qwen2", "qwen3", "qwen3moe", "olmoe", "qwen35moe", "qwen35"],
                    help="qwen2: RoPE in halves and q/k/v bias; qwen3: per-head q/k norm; qwen3moe, olmoe: mixture of experts")
    ap.add_argument("--experts", type=int, default=8, help="MoE: experts per layer")
    ap.add_argument("--used", type=int, default=2, help="MoE: experts per token")
    ap.add_argument("--shared", action="store_true", help="MoE: add a shared expert with a sigmoid gate")
    ap.add_argument("--dense", type=int, default=0, help="MoE: leading dense layers")
    ap.add_argument("--template", default="chatml", choices=["chatml", "chatml-xml", "gemma4", "glm"],
                    help="chat template written to the file (decides the chat and tool-call format katana uses)")
    ap.add_argument("--kv", type=int, default=4, help="key/value heads (fewer than 4 = grouped-query attention)")
    a = ap.parse_args()
    rnd = random.Random(a.seed)

    b2u = bytes_to_unicode()
    tokens = [b2u[b] for b in range(256)]
    merges = []
    for pair in ["h e", "l l", "o Ġ", "Ġ t", "t h", "e r", "i n", "a n", "Ġ a", "o u"]:
        x, y = pair.split()
        merges.append(pair)
        tokens.append(x + y)
    types = [1] * len(tokens)
    specials = ["<|endoftext|>", "<|im_start|>", "<|im_end|>"]
    for sp in specials:
        tokens.append(sp)
        types.append(3)
    eos = tokens.index("<|im_end|>")
    V = len(tokens)

    dim, heads, hidden, layers, ctx = 64, 4, 128, 2, 512
    A, kvh = a.arch, a.kv
    kvd = dim // heads * kvh
    hd = dim // heads
    moe = A in ("qwen3moe", "olmoe", "qwen35moe")
    hyb = A in ("qwen35moe", "qwen35")
    if A == "qwen35moe": a.shared = True
    dk, ng, nv, dv, CK = 16, 2, 4, 16, 4            # DeltaNet: largura de q/k, grupos, cabecas v, largura v, janela da convolucao
    inner, CH = nv * dv, 2 * ng * dk + nv * dv
    E, He, Hs = a.experts, 32, 48
    tmpl = ("{% for message in messages %}{{'<|im_start|>' + message['role'] + '\\n' + message['content'] + '<|im_end|>' + '\\n'}}"
            "{% endfor %}{% if add_generation_prompt %}{{ '<|im_start|>assistant\\n' }}{% endif %}")
    if a.template == "chatml-xml":   # chamadas de ferramenta em XML, como no template do Qwen3.6
        tmpl += "{# tools: <tool_call><function=name><parameter=k>v</parameter></function></tool_call> #}"
    elif a.template == "gemma4":
        tmpl = "{{ '<|turn>' + role + '\\n' + content + '<turn|>' }}{# <|tool_call>call:name{} #}"
    elif a.template == "glm":
        tmpl = "[gMASK]<sop>{{ '<|user|>' + content + '<|assistant|>' }}{# <tool_call>name<arg_key>k</arg_key><arg_value>v</arg_value></tool_call> </think> #}"
    kvs = [
        kv_str("general.architecture", A),
        kv_str("general.name", "katana-tiny-test"),
        kv_u32("general.alignment", 32),
        kv_u32(A + ".context_length", ctx),
        kv_u32(A + ".embedding_length", dim),
        kv_u32(A + ".block_count", layers),
        kv_u32(A + ".feed_forward_length", hidden),
        kv_u32(A + ".attention.head_count", heads),
        kv_u32(A + ".attention.head_count_kv", kvh),
        kv_f32(A + ".rope.freq_base", 10000.0),
        kv_f32(A + ".attention.layer_norm_rms_epsilon", 1e-5),
        *([kv_u32(A + ".expert_count", E), kv_u32(A + ".expert_used_count", a.used), kv_u32(A + ".expert_feed_forward_length", He)] if moe else []),
        *([kv_u32(A + ".expert_shared_feed_forward_length", Hs)] if moe and a.shared else []),
        *([kv_u32(A + ".leading_dense_block_count", a.dense)] if moe and a.dense else []),
        *([kv_u32(A + ".ssm.conv_kernel", CK), kv_u32(A + ".ssm.state_size", dk), kv_u32(A + ".ssm.group_count", ng),
           kv_u32(A + ".ssm.time_step_rank", nv), kv_u32(A + ".ssm.inner_size", inner), kv_u32(A + ".full_attention_interval", 2),
           kv_u32(A + ".rope.dimension_count", hd // 2), kv_u32(A + ".attention.key_length", hd)] if hyb else []),
        kv_str("tokenizer.ggml.model", "gpt2"),
        kv_str("tokenizer.ggml.pre", "default"),
        kv_arr_str("tokenizer.ggml.tokens", tokens),
        kv_arr_i32("tokenizer.ggml.token_type", types),
        kv_arr_str("tokenizer.ggml.merges", merges),
        kv_u32("tokenizer.ggml.bos_token_id", tokens.index("<|endoftext|>")),
        kv_u32("tokenizer.ggml.eos_token_id", eos),
        kv_bool("tokenizer.ggml.add_bos_token", False),
        kv_str("tokenizer.chat_template", tmpl),
    ]

    def rand(n, scale):
        return [rnd.gauss(0.0, scale) for _ in range(n)]

    tensors = [("token_embd.weight", [dim, V], rand(V * dim, 0.5)),
               ("output_norm.weight", [dim], [1.0] * dim)]
    out = rand(V * dim, 0.5)
    if a.no_eos:
        for i in range(dim):
            out[eos * dim + i] = 0.0
    tensors.append(("output.weight", [dim, V], out))
    for l in range(layers):
        p = "blk.%d." % l
        tensors += [(p + "attn_norm.weight", [dim], [1.0] * dim)]
        lin = hyb and (l + 1) % 2 != 0
        if lin:
            tensors += [(p + "attn_qkv.weight", [dim, CH], rand(dim * CH, 0.1)),
                        (p + "attn_gate.weight", [dim, inner], rand(dim * inner, 0.1)),
                        (p + "ssm_alpha.weight", [dim, nv], rand(dim * nv, 0.1)),
                        (p + "ssm_beta.weight", [dim, nv], rand(dim * nv, 0.1)),
                        (p + "ssm_out.weight", [inner, dim], rand(dim * inner, 0.1)),
                        (p + "ssm_conv1d.weight", [CK, CH], rand(CK * CH, 0.4)),
                        (p + "ssm_a", [nv], [-0.5 - rnd.random() for _ in range(nv)]),
                        (p + "ssm_dt.bias", [nv], rand(nv, 0.5)),
                        (p + "ssm_norm.weight", [dv], [1.0 + rnd.gauss(0, 0.1) for _ in range(dv)])]
        else:
            qrows = 2 * dim if hyb else dim
            tensors += [(p + "attn_q.weight", [dim, qrows], rand(dim * qrows, 0.1)),
                        (p + "attn_k.weight", [dim, kvd], rand(dim * kvd, 0.1)),
                        (p + "attn_v.weight", [dim, kvd], rand(dim * kvd, 0.1)),
                        (p + "attn_output.weight", [dim, dim], rand(dim * dim, 0.1))]
        tensors += [(p + ("post_attention_norm.weight" if hyb else "ffn_norm.weight"), [dim], [1.0] * dim)]
        if moe and l >= a.dense:
            tensors += [(p + "ffn_gate_inp.weight", [dim, E], rand(dim * E, 0.3)),
                        (p + "ffn_gate_exps.weight", [dim, He, E], rand(dim * He * E, 0.1)),
                        (p + "ffn_up_exps.weight", [dim, He, E], rand(dim * He * E, 0.1)),
                        (p + "ffn_down_exps.weight", [He, dim, E], rand(dim * He * E, 0.1))]
            if a.shared:
                tensors += [(p + "ffn_gate_shexp.weight", [dim, Hs], rand(dim * Hs, 0.1)),
                            (p + "ffn_up_shexp.weight", [dim, Hs], rand(dim * Hs, 0.1)),
                            (p + "ffn_down_shexp.weight", [Hs, dim], rand(dim * Hs, 0.1)),
                            (p + "ffn_gate_inp_shexp.weight", [dim], rand(dim, 0.3))]
        else:
            tensors += [(p + "ffn_gate.weight", [dim, hidden], rand(dim * hidden, 0.1)),
                        (p + "ffn_up.weight", [dim, hidden], rand(dim * hidden, 0.1)),
                        (p + "ffn_down.weight", [hidden, dim], rand(dim * hidden, 0.1))]
        if A in ("qwen3", "qwen3moe") or (hyb and not lin):
            tensors += [(p + "attn_q_norm.weight", [hd], [1.0 + rnd.gauss(0, 0.1) for _ in range(hd)]),
                        (p + "attn_k_norm.weight", [hd], [1.0 + rnd.gauss(0, 0.1) for _ in range(hd)])]
        if A == "olmoe":
            tensors += [(p + "attn_q_norm.weight", [dim], [1.0 + rnd.gauss(0, 0.1) for _ in range(dim)]),
                        (p + "attn_k_norm.weight", [kvd], [1.0 + rnd.gauss(0, 0.1) for _ in range(kvd)])]
        if A == "qwen2":
            tensors += [(p + "attn_q.bias", [dim], rand(dim, 0.1)), (p + "attn_k.bias", [kvd], rand(kvd, 0.1)),
                        (p + "attn_v.bias", [kvd], rand(kvd, 0.1))]

    head = b"GGUF" + struct.pack("<IQQ", 3, len(tensors), len(kvs)) + b"".join(kvs)
    info, data, off = b"", b"", 0
    for name, dims, vals in tensors:
        info += s(name) + struct.pack("<I", len(dims)) + b"".join(struct.pack("<Q", d) for d in dims)
        info += struct.pack("<IQ", 0, off)
        blob = struct.pack("<%df" % len(vals), *vals)
        blob += b"\0" * ((-len(blob)) % 32)
        data += blob
        off += len(blob)
    meta = head + info
    meta += b"\0" * ((-len(meta)) % 32)
    with open(a.out, "wb") as f:
        f.write(meta + data)
    print("%s: %d tokens, %d tensors, %d bytes" % (a.out, V, len(tensors), len(meta) + len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
