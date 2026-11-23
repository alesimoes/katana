#!/usr/bin/env python3
"""make_big_hf: writes a SPARSE Qwen3.5-MoE-shaped checkpoint (zeros, bf16) of a chosen size, to check that
`katana convert` streams: the peak memory must stay far below the checkpoint size. Standard library only.

  python3 tools/convert/make_big_hf.py OUTDIR TINY_HF_DIR [--layers 6] [--hidden 1024] [--experts 64] [--ff 512] [--shards 6]

TINY_HF_DIR (from make_tiny_hf.py) only lends its tokenizer files. The weights are never written: every shard is a
sparse file (header + truncate), so the checkpoint costs no disk and reads as zeros. Zero weights quantize to zero
blocks, which is enough for memory and I/O behaviour but says nothing about speed of the encoders on real data.
"""
import argparse, json, os, shutil, struct, sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("tiny")
    ap.add_argument("--layers", type=int, default=6)
    ap.add_argument("--hidden", type=int, default=1024)
    ap.add_argument("--experts", type=int, default=64)
    ap.add_argument("--ff", type=int, default=512)
    ap.add_argument("--shards", type=int, default=6)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for f in ("tokenizer.json", "tokenizer_config.json", "generation_config.json"):
        shutil.copy(os.path.join(a.tiny, f), os.path.join(a.out, f))
    with open(os.path.join(a.tiny, "config.json")) as f:
        cfg = json.load(f)
    t = cfg["text_config"]
    D, H, E, F, L = a.hidden, 8, a.experts, a.ff, a.layers
    hd, dk, dv, nk, nv, K = 128, 128, 128, 4, 8, 4
    t.update({"hidden_size": D, "num_hidden_layers": L, "num_attention_heads": H, "num_key_value_heads": 2, "head_dim": hd,
              "linear_key_head_dim": dk, "linear_value_head_dim": dv, "linear_num_key_heads": nk, "linear_num_value_heads": nv,
              "num_experts": E, "moe_intermediate_size": F, "shared_expert_intermediate_size": F, "full_attention_interval": 4,
              "layer_types": ["full_attention" if (l + 1) % 4 == 0 else "linear_attention" for l in range(L)],
              "rope_parameters": {"rope_theta": 10000000.0, "partial_rotary_factor": 0.25, "mrope_section": [11, 11, 10]}})
    V = t["vocab_size"]
    P = "model.language_model."
    T = [(P + "embed_tokens.weight", [V, D]), (P + "norm.weight", [D]), ("lm_head.weight", [V, D])]
    key, val = 2 * nk * dk, nv * dv
    for l in range(L):
        q = P + "layers.%d." % l
        T += [(q + "input_layernorm.weight", [D]), (q + "post_attention_layernorm.weight", [D])]
        if (l + 1) % 4 == 0:
            T += [(q + "self_attn.q_proj.weight", [2 * H * hd, D]), (q + "self_attn.k_proj.weight", [2 * hd, D]),
                  (q + "self_attn.v_proj.weight", [2 * hd, D]), (q + "self_attn.o_proj.weight", [D, H * hd]),
                  (q + "self_attn.q_norm.weight", [hd]), (q + "self_attn.k_norm.weight", [hd])]
        else:
            T += [(q + "linear_attn.in_proj_qkv.weight", [key + val, D]), (q + "linear_attn.in_proj_z.weight", [val, D]),
                  (q + "linear_attn.in_proj_b.weight", [nv, D]), (q + "linear_attn.in_proj_a.weight", [nv, D]),
                  (q + "linear_attn.conv1d.weight", [key + val, 1, K]), (q + "linear_attn.A_log", [nv]),
                  (q + "linear_attn.dt_bias", [nv]), (q + "linear_attn.norm.weight", [dv]), (q + "linear_attn.out_proj.weight", [D, val])]
        T += [(q + "mlp.gate.weight", [E, D]), (q + "mlp.experts.gate_up_proj", [E, 2 * F, D]), (q + "mlp.experts.down_proj", [E, D, F]),
              (q + "mlp.shared_expert.gate_proj.weight", [F, D]), (q + "mlp.shared_expert.up_proj.weight", [F, D]),
              (q + "mlp.shared_expert.down_proj.weight", [D, F]), (q + "mlp.shared_expert_gate.weight", [1, D])]
    per = -(-len(T) // a.shards)
    wm, total = {}, 0
    for i in range(a.shards):
        chunk = T[i * per : (i + 1) * per]
        if not chunk:
            continue
        fn = "model-%05d-of-%05d.safetensors" % (i + 1, a.shards)
        hdr, off = {"__metadata__": {"format": "pt"}}, 0
        for n, shape in chunk:
            c = 2
            for d in shape:
                c *= d
            hdr[n] = {"dtype": "BF16", "shape": shape, "data_offsets": [off, off + c]}
            off += c
            wm[n] = fn
        h = json.dumps(hdr, separators=(",", ":")).encode()
        h += b" " * ((-len(h)) % 8)
        with open(os.path.join(a.out, fn), "wb") as f:
            f.write(struct.pack("<Q", len(h)) + h)
            f.truncate(8 + len(h) + off)
        total += off
    with open(os.path.join(a.out, "model.safetensors.index.json"), "w") as f:
        json.dump({"metadata": {"total_size": total}, "weight_map": wm}, f)
    with open(os.path.join(a.out, "config.json"), "w") as f:
        json.dump(cfg, f, indent=1)
    print("%s: %d tensors, %d shards, %.2f GB (sparse)" % (a.out, len(T), a.shards, total / 1e9))


if __name__ == "__main__":
    sys.exit(main())
