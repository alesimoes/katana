#!/usr/bin/env python3
"""make_tiny_hf: writes a TINY Hugging Face checkpoint of the Qwen3.5/3.6 architecture (safetensors + config.json +
tokenizer files), for testing `katana convert` without the 60 GB model. Python standard library only.

The weights are NOT drawn a second time: they come from tools/tinymodel/make_tiny_gguf.py (same --seed, same options),
which is the generator of the tiny GGUF the engine tests use. This script writes them back in the Hugging Face layout
(the inverse of what `katana convert` does), so converting the checkpoint must give the same tensors as the generator
wrote directly. tools/convert/compare_gguf.py checks that, tensor by tensor.

  python3 tools/convert/make_tiny_hf.py OUTDIR --arch qwen35moe --seed 1 [--dtype f32|bf16|f16] [--shards N]
         [--per-expert] [--merge-pairs] [--template-file] [--tied]

--dtype bf16 / f16 also rounds the GGUF-side values (make_tiny_gguf --round), so the checkpoint holds exactly the same
numbers. --per-expert writes the old one-tensor-per-expert layout; the default is the fused gate_up_proj / down_proj
layout. --shards N splits the tensors over N files with a model.safetensors.index.json. --tied leaves out lm_head.
The inverse transforms here are written independently of the C code (they run the permutation the other way), which
is what makes the comparison a check and not an echo.
"""
import argparse, importlib.util, json, math, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load_gen():
    spec = importlib.util.spec_from_file_location("make_tiny_gguf", os.path.join(HERE, "..", "tinymodel", "make_tiny_gguf.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def regroup(flat, rowlen, off_rows, hd, nk, r):
    """GGUF order (tiled v heads) -> Hugging Face order (grouped by k head), on ROWS of `rowlen` values.
    The first off_rows rows stay; the rest are heads of hd rows: HF head b*r+a is GGUF head a*nk+b."""
    rows = len(flat) // rowlen
    out = list(flat[: off_rows * rowlen])
    nh = (rows - off_rows) // hd
    assert nh == nk * r and off_rows + nh * hd == rows, (rows, off_rows, hd, nk, r)
    for hh in range(nh):
        b, a = divmod(hh, r)
        g = a * nk + b
        s = (off_rows + g * hd) * rowlen
        out += flat[s : s + hd * rowlen]
    return out


def regroup_cols(flat, rows, cols, hd, nk, r):
    """Same on COLUMNS (ssm_out): every row of `cols` values is regrouped in heads of hd columns."""
    out = []
    nh = cols // hd
    assert nh == nk * r
    for i in range(rows):
        row = flat[i * cols : (i + 1) * cols]
        for hh in range(nh):
            b, a = divmod(hh, r)
            g = a * nk + b
            out += row[g * hd : (g + 1) * hd]
    return out


def enc(vals, dtype):
    n = len(vals)
    if dtype == "f32":
        return struct.pack("<%df" % n, *vals)
    if dtype == "f16":
        return struct.pack("<%de" % n, *vals)
    words = []
    for v in vals:                                     # bf16: values are already bf16-representable
        words.append(struct.unpack("<I", struct.pack("<f", v))[0] >> 16)
    return struct.pack("<%dH" % n, *words)


def write_safetensors(path, items, dtype):
    """items: [(name, shape, values)]"""
    dt = {"f32": "F32", "f16": "F16", "bf16": "BF16"}[dtype]
    hdr, blobs, off = {"__metadata__": {"format": "pt"}}, [], 0
    for name, shape, vals in items:
        b = enc(vals, dtype)
        hdr[name] = {"dtype": dt, "shape": shape, "data_offsets": [off, off + len(b)]}
        blobs.append(b)
        off += len(b)
    h = json.dumps(hdr, separators=(",", ":")).encode()
    h += b" " * ((-len(h)) % 8)
    with open(path, "wb") as f:
        f.write(struct.pack("<Q", len(h)) + h + b"".join(blobs))


def main():
    gen = load_gen()
    ap = argparse.ArgumentParser(parents=[gen.make_parser()], add_help=False, conflict_handler="resolve")
    ap.add_argument("--dtype", default="f32", choices=["f32", "bf16", "f16"])
    ap.add_argument("--shards", type=int, default=1)
    ap.add_argument("--per-expert", action="store_true")
    ap.add_argument("--merge-pairs", action="store_true", help="tokenizer.json merges as [a, b] pairs instead of 'a b'")
    ap.add_argument("--template-file", action="store_true", help="chat template in chat_template.jinja instead of tokenizer_config.json")
    ap.add_argument("--tied", action="store_true", help="leave out lm_head.weight (tied embeddings)")
    ap.add_argument("--decoys", action="store_true", help="add visual.* and mtp.* tensors the converter must skip")
    a = ap.parse_args()
    a.round = a.dtype if a.dtype != "f32" else "none"
    a.pre = "qwen35"
    a.arch = a.arch if a.arch in ("qwen35moe", "qwen35") else "qwen35moe"
    outdir = a.out
    kvs, tensors, I = gen.build(a)
    moe = I["moe"]
    nk, nv, dk, dv = I["ng"], I["nv"], I["dk"], I["dv"]
    r = nv // nk
    P = "model.language_model."
    items = []

    def add(name, shape, vals):
        n = 1
        for d in shape:
            n *= d
        assert n == len(vals), (name, shape, len(vals))
        items.append((name, shape, vals))

    T = {n: (d, v) for n, d, v in tensors}
    f32 = lambda x: struct.unpack("<f", struct.pack("<f", x))[0]
    minus1 = lambda v: [f32(x) - 1.0 for x in v]       # (x - 1) + 1 is exact in float32 for the norm values (1 +- 0.5)
    add(P + "embed_tokens.weight", [T["token_embd.weight"][0][1], T["token_embd.weight"][0][0]], T["token_embd.weight"][1])
    add(P + "norm.weight", [I["dim"]], minus1(T["output_norm.weight"][1]))
    if not a.tied:
        add("lm_head.weight", [T["output.weight"][0][1], T["output.weight"][0][0]], T["output.weight"][1])
    for l in range(I["layers"]):
        p, q = "blk.%d." % l, P + "layers.%d." % l
        g = lambda s: T[p + s]
        add(q + "input_layernorm.weight", [I["dim"]], minus1(g("attn_norm.weight")[1]))
        add(q + "post_attention_layernorm.weight", [I["dim"]], minus1(g("post_attention_norm.weight")[1]))
        if p + "attn_q.weight" in T:
            for hf, gg in (("q_proj", "attn_q"), ("k_proj", "attn_k"), ("v_proj", "attn_v"), ("o_proj", "attn_output")):
                d, v = g(gg + ".weight")
                add(q + "self_attn.%s.weight" % hf, [d[1], d[0]], v)
            add(q + "self_attn.q_norm.weight", [I["hd"]], minus1(g("attn_q_norm.weight")[1]))
            add(q + "self_attn.k_norm.weight", [I["hd"]], minus1(g("attn_k_norm.weight")[1]))
        else:
            d, v = g("attn_qkv.weight")
            add(q + "linear_attn.in_proj_qkv.weight", [d[1], d[0]], regroup(v, d[0], 2 * nk * dk, dv, nk, r))
            d, v = g("attn_gate.weight")
            add(q + "linear_attn.in_proj_z.weight", [d[1], d[0]], regroup(v, d[0], 0, dv, nk, r))
            d, v = g("ssm_beta.weight")
            add(q + "linear_attn.in_proj_b.weight", [d[1], d[0]], regroup(v, d[0], 0, 1, nk, r))
            d, v = g("ssm_alpha.weight")
            add(q + "linear_attn.in_proj_a.weight", [d[1], d[0]], regroup(v, d[0], 0, 1, nk, r))
            d, v = g("ssm_conv1d.weight")
            add(q + "linear_attn.conv1d.weight", [d[1], 1, d[0]], regroup(v, d[0], 2 * nk * dk, dv, nk, r))
            d, v = g("ssm_a")
            add(q + "linear_attn.A_log", [nv], [math.log(-x) for x in regroup(v, 1, 0, 1, nk, r)])
            d, v = g("ssm_dt.bias")
            add(q + "linear_attn.dt_bias", [nv], regroup(v, 1, 0, 1, nk, r))
            add(q + "linear_attn.norm.weight", [dv], g("ssm_norm.weight")[1])
            d, v = g("ssm_out.weight")
            add(q + "linear_attn.out_proj.weight", [d[1], d[0]], regroup_cols(v, d[1], d[0], dv, nk, r))
        if moe and p + "ffn_gate_inp.weight" in T:
            d, v = g("ffn_gate_inp.weight")
            add(q + "mlp.gate.weight", [d[1], d[0]], v)
            (dg, vg), (du, vu), (dd, vd) = g("ffn_gate_exps.weight"), g("ffn_up_exps.weight"), g("ffn_down_exps.weight")
            E, F, D = dg[2], dg[1], dg[0]
            if a.per_expert:
                for e in range(E):
                    add(q + "mlp.experts.%d.gate_proj.weight" % e, [F, D], vg[e * F * D : (e + 1) * F * D])
                    add(q + "mlp.experts.%d.up_proj.weight" % e, [F, D], vu[e * F * D : (e + 1) * F * D])
                    add(q + "mlp.experts.%d.down_proj.weight" % e, [dd[1], dd[0]], vd[e * dd[0] * dd[1] : (e + 1) * dd[0] * dd[1]])
            else:
                gu = []
                for e in range(E):
                    gu += vg[e * F * D : (e + 1) * F * D] + vu[e * F * D : (e + 1) * F * D]
                add(q + "mlp.experts.gate_up_proj", [E, 2 * F, D], gu)
                add(q + "mlp.experts.down_proj", [dd[2], dd[1], dd[0]], vd)
            if p + "ffn_gate_shexp.weight" in T:
                for hf, gg in (("gate_proj", "ffn_gate_shexp"), ("up_proj", "ffn_up_shexp"), ("down_proj", "ffn_down_shexp")):
                    d, v = g(gg + ".weight")
                    add(q + "mlp.shared_expert.%s.weight" % hf, [d[1], d[0]], v)
                d, v = g("ffn_gate_inp_shexp.weight")
                add(q + "mlp.shared_expert_gate.weight", [1, d[0]], v)
        else:
            for hf, gg in (("gate_proj", "ffn_gate"), ("up_proj", "ffn_up"), ("down_proj", "ffn_down")):
                d, v = g(gg + ".weight")
                add(q + "mlp.%s.weight" % hf, [d[1], d[0]], v)
    if a.decoys:
        add("model.visual.blocks.0.attn.qkv.weight", [4, 4], [0.5] * 16)
        add("mtp.fc.weight", [4, 4], [0.25] * 16)

    os.makedirs(outdir, exist_ok=True)
    nsh = max(1, min(a.shards, len(items)))
    per = -(-len(items) // nsh)
    wm = {}
    for i in range(nsh):
        chunk = items[i * per : (i + 1) * per]
        fn = "model.safetensors" if nsh == 1 else "model-%05d-of-%05d.safetensors" % (i + 1, nsh)
        write_safetensors(os.path.join(outdir, fn), chunk, a.dtype)
        for n, _, _ in chunk:
            wm[n] = fn
    if nsh > 1:
        with open(os.path.join(outdir, "model.safetensors.index.json"), "w") as f:
            json.dump({"metadata": {"total_size": 0}, "weight_map": wm}, f)

    cfg_text = {
        "model_type": "qwen3_5_moe_text" if moe else "qwen3_5_text", "hidden_size": I["dim"], "num_hidden_layers": I["layers"],
        "num_attention_heads": I["heads"], "num_key_value_heads": I["kvh"], "head_dim": I["hd"], "rms_norm_eps": 1e-5,
        "vocab_size": I["V"], "max_position_embeddings": I["ctx"], "full_attention_interval": 2,
        "linear_conv_kernel_dim": I["CK"], "linear_key_head_dim": dk, "linear_value_head_dim": dv,
        "linear_num_key_heads": nk, "linear_num_value_heads": nv,
        "layer_types": ["full_attention" if (l + 1) % 2 == 0 else "linear_attention" for l in range(I["layers"])],
        "rope_parameters": {"rope_theta": 10000.0, "partial_rotary_factor": 0.5, "mrope_section": [11, 11, 10], "rope_type": "default"},
        "bos_token_id": I["bos"], "eos_token_id": I["bos"], "tie_word_embeddings": bool(a.tied),
    }
    if moe:
        cfg_text.update({"num_experts": I["E"], "num_experts_per_tok": I["used"], "moe_intermediate_size": I["He"],
                         "shared_expert_intermediate_size": I["Hs"]})
    else:
        cfg_text["intermediate_size"] = I["hidden"]
    cfg = {"architectures": ["Qwen3_5MoeForConditionalGeneration" if moe else "Qwen3_5ForConditionalGeneration"],
           "model_type": "qwen3_5_moe" if moe else "qwen3_5", "text_config": cfg_text, "vision_config": {"depth": 1}}
    with open(os.path.join(outdir, "config.json"), "w") as f:
        json.dump(cfg, f, indent=1)

    toks, types = I["tokens"], I["types"]
    vocab = {t: i for i, t in enumerate(toks) if types[i] == 1}
    added = [{"id": i, "content": t, "special": True} for i, t in enumerate(toks) if types[i] != 1]
    merges = [m.split(" ") if a.merge_pairs else m for m in I["merges"]]
    with open(os.path.join(outdir, "tokenizer.json"), "w", encoding="utf-8") as f:
        json.dump({"version": "1.0", "added_tokens": added, "model": {"type": "BPE", "vocab": vocab, "merges": merges}}, f, ensure_ascii=False)
    tc = {"eos_token": toks[I["eos"]], "bos_token": None, "pad_token": None, "add_bos_token": False}
    if a.template_file:
        with open(os.path.join(outdir, "chat_template.jinja"), "w") as f:
            f.write(I["tmpl"])
    else:
        tc["chat_template"] = I["tmpl"]
    with open(os.path.join(outdir, "tokenizer_config.json"), "w", encoding="utf-8") as f:
        json.dump(tc, f, ensure_ascii=False)
    with open(os.path.join(outdir, "generation_config.json"), "w") as f:
        json.dump({"top_k": 20, "top_p": 0.95, "temperature": 0.6}, f)
    print("%s: %d tensors in %d file(s), dtype %s" % (outdir, len(items), nsh, a.dtype))


if __name__ == "__main__":
    sys.exit(main())
