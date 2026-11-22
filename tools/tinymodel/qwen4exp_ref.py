#!/usr/bin/env python3
"""qwen4exp_ref: a tiny random qwen4exp model in GGUF (two parts) and its reference states and logits.

The forward pass is written in plain Python (no numpy, no torch) from the published definition of the architecture, with
the published conventions: RMS weights stored as w with 1 + w applied, log of the decay, value heads of one query/key
group next to each other, whole sequence at once, recurrences in float64. The GGUF is then written with the file
conventions (1 + w, -exp(A_log), value heads interleaved across the groups, split indexer projection, split gate/up).
So the test covers the tensor mapping as well as the arithmetic, and the incremental decode of katana is compared
with a reference that has no incremental state.

  python3 tools/tinymodel/qwen4exp_ref.py DIR [--seed 1] [--n 72] [--mutate NAME]
  ./katana q4check DIR/q4-00001-of-00002.gguf DIR/ref.bin

--mutate changes one rule of the reference (the file stays the same); q4check must then fail:
  nosparse (every position visible), rowpos (block keys rotated with the last position of the block), noeos (n-grams do
  not stop at end of text), silu (SiLU gate after the linear attention), nodilation (n-gram convolution without
  dilation), meaninject (no write gate), notile (value heads taken in file order), norelu (block scores without ReLU).
"""
import argparse, math, os, random, struct

from make_tiny_gguf import bytes_to_unicode, s, kv_str, kv_u32, kv_f32, kv_bool, kv_arr_str, kv_arr_i32


def kv_arr_u64(k, v): return s(k) + struct.pack("<I", 9) + struct.pack("<IQ", 10, len(v)) + struct.pack("<%dQ" % len(v), *v)


ap = argparse.ArgumentParser()
ap.add_argument("dir")
ap.add_argument("--seed", type=int, default=1)
ap.add_argument("--n", type=int, default=72)
ap.add_argument("--mutate", default="")
a = ap.parse_args()
rnd = random.Random(a.seed)

d, R, RANK, NL, EVERY = 16, 4, 8, 8, 4
D = d * R
HEADS, KV, HD, ROPE = 4, 2, 16, 8
IN, IHD, BUDGET, RATIO = 2, 8, 8, 4
DK, NG, NV, DV, CK = 8, 2, 6, 8, 4
INNER, C = NV * DV, 2 * NG * DK + NV * DV
E, USED, H = 8, 3, 64
NGRAM, HPN, PK, PD, PLE_L = 3, 2, 4, 32, 1
PE = (NGRAM - 1) * HPN * PD
BASE, EPS = 10000.0, 1e-6
VOC = [101, 103, 107, 109]
OFF = [sum(VOC[:i]) for i in range(4)]
ROWS = (sum(VOC) + 127) // 128 * 128
MULT = [2 * rnd.randrange(1 << 43) + 1 for _ in range(NGRAM)]

b2u = bytes_to_unicode()
tokens = [b2u[b] for b in range(256)] + ["<|endoftext|>", "<|im_start|>", "<|im_end|>"]
types = [1] * 256 + [3] * 3
V, EOS = len(tokens), 256


# ---------- values exactly as each type stores them
def f32(x): return struct.unpack("<f", struct.pack("<f", x))[0]
def f16(x): return struct.unpack("<e", struct.pack("<e", x))[0]


def bf16(x):
    return struct.unpack("<f", b"\0\0" + struct.pack("<f", x)[2:])[0]


def mat(rows, cols, scale, conv=f32): return [[conv(rnd.gauss(0, scale)) for _ in range(cols)] for _ in range(rows)]
def vec(n, scale, mean=0.0, conv=f32): return [conv(mean + rnd.gauss(0, scale)) for _ in range(n)]


def rand_q4_0(rows, cols):
    """32 values: fp16 scale, 16 bytes (low nibbles 0..15, high nibbles 16..31), value = d * (code - 8)"""
    blob, out = bytearray(), []
    for _ in range(rows):
        row = []
        for _ in range(cols // 32):
            sc = f16(rnd.uniform(0.02, 0.1) * rnd.choice((-1, 1)))
            q = [rnd.randrange(16) for _ in range(32)]
            blob += struct.pack("<e", sc) + bytes(q[i] | q[i + 16] << 4 for i in range(16))
            row += [sc * (x - 8) for x in q]
        out.append(row)
    return out, bytes(blob)


def rand_q2_0(rows, cols):
    """64 values: fp16 scale, 16 bytes with 4 codes each (first value in the low bits), value = d * (code - 1)"""
    blob, out = bytearray(), []
    for _ in range(rows):
        row = []
        for _ in range(cols // 64):
            sc = f16(rnd.uniform(0.02, 0.2) * rnd.choice((-1, 1)))
            q = [rnd.randrange(4) for _ in range(64)]
            blob += struct.pack("<e", sc) + bytes(q[4 * i] | q[4 * i + 1] << 2 | q[4 * i + 2] << 4 | q[4 * i + 3] << 6 for i in range(16))
            row += [sc * (x - 1) for x in q]
        out.append(row)
    return out, bytes(blob)


def hc_weights(inject):
    h = {"norm": vec(D, 0.3), "down": mat(RANK, D, 0.3, bf16), "up": mat(D, RANK, 0.6, bf16)}
    if inject: h["inj"] = mat(R, D, 0.3, bf16)
    return h


# ---------- weights, in the convention of the reference
W = {"emb": mat(V, d, 0.5), "head": mat(V, d, 0.5), "hc": hc_weights(False), "l": []}
for l in range(NL):
    p = {"hc_attn": hc_weights(True), "hc_ffn": hc_weights(True), "lin": (l + 1) % EVERY != 0}
    if p["lin"]:
        p.update(qkv=mat(C, d, 0.4), z=mat(INNER, d, 0.4), al=mat(NV, d, 0.3, bf16), be=mat(NV, d, 0.3, bf16), conv=mat(C, CK, 0.4),
                 a_log=[math.log(rnd.uniform(0.05, 4)) for _ in range(NV)], dt=vec(NV, 1.0, -1.0), norm=vec(DV, 0.2, 1.0), out=mat(d, INNER, 0.4))
    else:
        p.update(q=mat(2 * HEADS * HD, d, 0.4), k=mat(KV * HD, d, 0.4), v=mat(KV * HD, d, 0.4), o=mat(d, HEADS * HD, 0.3),
                 qn=vec(HD, 0.3), kn=vec(HD, 0.3), iqk=mat((IN + 1) * IHD, d, 0.5, bf16), iqn=vec(IHD, 0.3), ikn=vec(IHD, 0.3))
    p.update(router=mat(E, d, 0.6, bf16), gate=[mat(H, d, 0.3) for _ in range(E)], up=[mat(H, d, 0.3) for _ in range(E)],
             sg=mat(H, d, 0.3), su=mat(H, d, 0.3), sd=mat(d, H, 0.1), sgate=vec(d, 0.5, 0, bf16))
    dn, p["down_blob"] = rand_q2_0(E * d, H)
    p["down"] = [dn[e * d:(e + 1) * d] for e in range(E)]
    if l == PLE_L:
        p.update(pkey=mat(D, PE, 0.3, bf16), pval=mat(d, PE, 0.3, bf16), pconv=mat(D, PK, 0.4, f16), nk=vec(D, 0.2), nq=vec(D, 0.2), nc=vec(D, 0.2))
    W["l"].append(p)
TABLE, TABLE_BLOB = rand_q4_0(ROWS, PD)
IDS = [rnd.randrange(256) for _ in range(a.n)]
for i in range(0, a.n, 5): IDS[i] = IDS[max(0, i - 3)]
for i in (4, 5, 11, 30, 61):
    if i < a.n: IDS[i] = EOS


# ---------- GGUF
def tile(rows, width):
    """value heads: from group by group to interleaved across the groups"""
    rep, out = NV // NG, []
    for r in range(rep):
        for g in range(NG):
            h = g * rep + r
            out += rows[h * width:(h + 1) * width]
    return out


def flat(m): return [x for r in m for x in r]
def pack(t, v):
    if t == 0: return struct.pack("<%df" % len(v), *v)
    if t == 1: return struct.pack("<%de" % len(v), *v)
    return b"".join(struct.pack("<f", x)[2:] for x in v)


T = [("token_embd.weight", [d, V], 0, pack(0, flat(W["emb"]))), ("output.weight", [d, V], 0, pack(0, flat(W["head"])))]


def add(name, dims, t, v): T.append((name, dims, t, v if isinstance(v, bytes) else pack(t, v)))
def add_hc(pre, h):
    add(pre + "_norm.weight", [D], 0, [1 + x for x in h["norm"]])
    add(pre + "_down.weight", [D, RANK], 30, flat(h["down"]))
    add(pre + "_up.weight", [RANK, D], 30, flat(h["up"]))
    if "inj" in h: add(pre + "_inject.weight", [D, R], 30, flat(h["inj"]))


add_hc("output_hc", W["hc"])
for l, p in enumerate(W["l"]):
    b = "blk.%d." % l
    add_hc(b + "hc_attn", p["hc_attn"]); add_hc(b + "hc_ffn", p["hc_ffn"])
    if p["lin"]:
        kd = 2 * NG * DK
        add(b + "attn_qkv.weight", [d, C], 0, flat(p["qkv"][:kd] + tile(p["qkv"][kd:], DV)))
        add(b + "attn_gate.weight", [d, INNER], 0, flat(tile(p["z"], DV)))
        add(b + "ssm_alpha.weight", [d, NV], 30, flat(tile(p["al"], 1)))
        add(b + "ssm_beta.weight", [d, NV], 30, flat(tile(p["be"], 1)))
        add(b + "ssm_conv1d.weight", [CK, C], 0, flat(p["conv"][:kd] + tile(p["conv"][kd:], DV)))
        add(b + "ssm_a", [NV], 0, [-math.exp(x) for x in tile(p["a_log"], 1)])
        add(b + "ssm_dt.bias", [NV], 0, tile(p["dt"], 1))
        add(b + "ssm_norm.weight", [DV], 0, p["norm"])
        add(b + "ssm_out.weight", [INNER, d], 0, flat([tile(r, DV) for r in p["out"]]))
    else:
        add(b + "attn_q.weight", [d, 2 * HEADS * HD], 0, flat(p["q"]))
        add(b + "attn_k.weight", [d, KV * HD], 0, flat(p["k"]))
        add(b + "attn_v.weight", [d, KV * HD], 0, flat(p["v"]))
        add(b + "attn_output.weight", [HEADS * HD, d], 0, flat(p["o"]))
        add(b + "attn_q_norm.weight", [HD], 0, [1 + x for x in p["qn"]])
        add(b + "attn_k_norm.weight", [HD], 0, [1 + x for x in p["kn"]])
        add(b + "indexer.q_proj.weight", [d, IN * IHD], 30, flat(p["iqk"][:IN * IHD]))
        add(b + "indexer.k_proj.weight", [d, IHD], 30, flat(p["iqk"][IN * IHD:]))
        add(b + "indexer.q_norm.weight", [IHD], 0, [1 + x for x in p["iqn"]])
        add(b + "indexer.k_norm.weight", [IHD], 0, [1 + x for x in p["ikn"]])
    add(b + "ffn_gate_inp.weight", [d, E], 30, flat(p["router"]))
    add(b + "ffn_gate_exps.weight", [d, H, E], 0, flat(flat(p["gate"])))
    add(b + "ffn_up_exps.weight", [d, H, E], 0, flat(flat(p["up"])))
    add(b + "ffn_down_exps.weight", [H, d, E], 42, p["down_blob"])
    add(b + "ffn_gate_shexp.weight", [d, H], 0, flat(p["sg"]))
    add(b + "ffn_up_shexp.weight", [d, H], 0, flat(p["su"]))
    add(b + "ffn_down_shexp.weight", [H, d], 0, flat(p["sd"]))
    add(b + "ffn_gate_inp_shexp.weight", [d], 30, p["sgate"])
    if l == PLE_L:
        add(b + "ple_key.weight", [PE, D], 30, flat(p["pkey"]))
        add(b + "ple_value.weight", [PE, d], 30, flat(p["pval"]))
        add(b + "ple_conv1d.weight", [PK, D], 1, flat(p["pconv"]))
        for k, n in (("nk", "key"), ("nq", "query"), ("nc", "conv")): add(b + "ple_norm_%s.weight" % n, [D], 0, [1 + x for x in p[k]])

A = "qwen4exp"
tmpl = ("{% for message in messages %}{{'<|im_start|>' + message['role'] + '\\n' + message['content'] + '<|im_end|>' + '\\n'}}"
        "{% endfor %}{% if add_generation_prompt %}{{ '<|im_start|>assistant\\n<think>\\n' }}{% endif %}")
kvs = [kv_str("general.architecture", A), kv_u32(A + ".block_count", NL), kv_u32(A + ".context_length", 512), kv_u32(A + ".embedding_length", d),
       kv_u32(A + ".attention.head_count", HEADS), kv_u32(A + ".attention.head_count_kv", KV), kv_f32(A + ".rope.freq_base", BASE),
       kv_f32(A + ".attention.layer_norm_rms_epsilon", EPS), kv_u32(A + ".expert_count", E), kv_u32(A + ".expert_used_count", USED),
       kv_u32(A + ".attention.key_length", HD), kv_u32(A + ".attention.value_length", HD), kv_u32(A + ".expert_feed_forward_length", H),
       kv_u32(A + ".expert_shared_feed_forward_length", H), kv_u32(A + ".ssm.conv_kernel", CK), kv_u32(A + ".ssm.state_size", DK),
       kv_u32(A + ".ssm.group_count", NG), kv_u32(A + ".ssm.time_step_rank", NV), kv_u32(A + ".ssm.inner_size", INNER),
       kv_u32(A + ".full_attention_interval", EVERY), kv_u32(A + ".rope.dimension_count", ROPE), kv_u32(A + ".hyper_connection.count", R),
       kv_u32(A + ".hyper_connection.low_rank", RANK), kv_u32(A + ".attention.indexer.head_count", IN), kv_u32(A + ".attention.indexer.key_length", IHD),
       kv_u32(A + ".attention.indexer.top_k", BUDGET), kv_arr_i32(A + ".attention.compress_ratios", [0 if p["lin"] else RATIO for p in W["l"]]),
       kv_arr_i32(A + ".ple.layers", [PLE_L]), kv_u32(A + ".ple.ngram_size", NGRAM), kv_u32(A + ".ple.heads_per_ngram", HPN),
       kv_u32(A + ".ple.conv_kernel", PK), kv_u32(A + ".ple.eos_token_id", EOS), kv_u32(A + ".embedding_length_per_layer_input", PD),
       kv_arr_u64(A + ".ple.layer_multipliers", MULT), kv_arr_u64(A + ".ple.head_offsets", OFF), kv_arr_u64(A + ".ple.head_vocab_sizes", VOC),
       kv_str("tokenizer.ggml.model", "gpt2"), kv_str("tokenizer.ggml.pre", "qwen35"), kv_arr_str("tokenizer.ggml.tokens", tokens),
       kv_arr_i32("tokenizer.ggml.token_type", types), kv_arr_str("tokenizer.ggml.merges", ["h e"]), kv_u32("tokenizer.ggml.bos_token_id", EOS),
       kv_u32("tokenizer.ggml.eos_token_id", 258), kv_bool("tokenizer.ggml.add_bos_token", False), kv_str("tokenizer.chat_template", tmpl)]


def write(path, kvs, tensors):
    info, data, off = b"", [], 0
    for name, dims, t, blob in tensors:
        info += s(name) + struct.pack("<I", len(dims)) + b"".join(struct.pack("<Q", x) for x in dims) + struct.pack("<IQ", t, off)
        blob += b"\0" * ((-len(blob)) % 32)
        data.append(blob); off += len(blob)
    meta = b"GGUF" + struct.pack("<IQQ", 3, len(tensors), len(kvs)) + b"".join(kvs) + info
    with open(path, "wb") as f:
        f.write(meta + b"\0" * ((-len(meta)) % 32) + b"".join(data))


os.makedirs(a.dir, exist_ok=True)
sp = lambda i: [kv_u32("split.no", i), kv_u32("split.count", 2), kv_u32("split.tensors.count", len(T) + 1)]
write(os.path.join(a.dir, "q4-00001-of-00002.gguf"), kvs + sp(0), T)
write(os.path.join(a.dir, "q4-00002-of-00002.gguf"), sp(1), [("per_layer_token_embd.weight", [PD, ROWS], 2, TABLE_BLOB)])


# ---------- reference forward
def sig(x): return 1 / (1 + math.exp(-x)) if x > -700 else 0.0
def silu(x): return x * sig(x)
def mv(M, x): return [sum(map(float.__mul__, r, x)) for r in M]
def rms(x, w1):
    inv = 1 / math.sqrt(sum(v * v for v in x) / len(x) + EPS)
    return [v * inv * w for v, w in zip(x, w1)]
def p1(w): return [1 + v for v in w]
def rms_g(x, w):
    o = []
    for r in range(R): o += rms(x[r * d:(r + 1) * d], p1(w[r * d:(r + 1) * d]))
    return o


def rope(v, pos):
    o, h = list(v), ROPE // 2
    for i in range(h):
        ang = pos * BASE ** (-2 * i / ROPE)
        c, sn = math.cos(ang), math.sin(ang)
        o[i], o[h + i] = v[i] * c - v[h + i] * sn, v[h + i] * c + v[i] * sn
    return o


def hc_read(h, x):
    n = rms_g(x, h["norm"])
    m = [sig(v) for v in mv(h["up"], [silu(v / R) for v in mv(h["down"], n)])]
    mixed = [sum(m[r * d + i] * n[r * d + i] for r in range(R)) / R for i in range(d)]
    inj = [2 * sig(v / R) for v in mv(h["inj"], n)] if "inj" in h else None
    if a.mutate == "meaninject" and inj: inj = [1.0] * R
    return mixed, inj


def hc_write(x, y, inj): return [x[r * d + i] + inj[r] * y[i] for r in range(R) for i in range(d)]


def linear_attn(p, xs):
    n = len(xs)
    raw = [mv(p["qkv"], x) for x in xs]
    S = [[[0.0] * DV for _ in range(DK)] for _ in range(NV)]
    outs = []
    for t in range(n):
        y = []
        for ch in range(C):
            y.append(silu(sum(p["conv"][ch][CK - 1 - k] * raw[t - k][ch] for k in range(CK) if t - k >= 0)))
        l2 = lambda v: [x / math.sqrt(sum(u * u for u in v) + 1e-6) for x in v]
        q = [[x / math.sqrt(DK) for x in l2(y[g * DK:(g + 1) * DK])] for g in range(NG)]
        k = [l2(y[(NG + g) * DK:(NG + g + 1) * DK]) for g in range(NG)]
        v = [y[2 * NG * DK + h * DV:2 * NG * DK + (h + 1) * DV] for h in range(NV)]
        al, be, z = mv(p["al"], xs[t]), mv(p["be"], xs[t]), mv(p["z"], xs[t])
        o = []
        for h in range(NV):
            g = h % NG if a.mutate == "notile" else h // (NV // NG)
            dec = math.exp(-math.exp(p["a_log"][h]) * math.log1p(math.exp(al[h] + p["dt"][h])))
            Sh = S[h]
            for i in range(DK): Sh[i] = [u * dec for u in Sh[i]]
            kv = [sum(Sh[i][j] * k[g][i] for i in range(DK)) for j in range(DV)]
            dl = [(v[h][j] - kv[j]) * sig(be[h]) for j in range(DV)]
            for i in range(DK):
                for j in range(DV): Sh[i][j] += k[g][i] * dl[j]
            oh = rms([sum(Sh[i][j] * q[g][i] for i in range(DK)) for j in range(DV)], p["norm"])
            gate = silu if a.mutate == "silu" else sig
            o += [oh[j] * gate(z[h * DV + j]) for j in range(DV)]
        outs.append(mv(p["out"], o))
    return outs


SEEN = [0, 0]


def attention(p, xs):
    n = len(xs)
    qg, K, VV, iqk = [mv(p["q"], x) for x in xs], [mv(p["k"], x) for x in xs], [mv(p["v"], x) for x in xs], [mv(p["iqk"], x) for x in xs]
    K = [[rope(rms(K[t][h * HD:(h + 1) * HD], p1(p["kn"])), t) for h in range(KV)] for t in range(n)]
    outs = []
    for t in range(n):
        nb = (t + 1) // RATIO
        scores = []
        for b in range(nb):
            idx = range(b * RATIO, (b + 1) * RATIO)
            pooled = [sum(iqk[i][IN * IHD + j] for i in idx) / RATIO for j in range(IHD)]
            key = rope(rms(pooled, p1(p["ikn"])), idx[-1] if a.mutate == "rowpos" else idx[0])
            sc = 0.0
            for h in range(IN):
                q = rope(rms(iqk[t][h * IHD:(h + 1) * IHD], p1(p["iqn"])), t)
                u = sum(x * y for x, y in zip(q, key))
                sc += u if a.mutate == "norelu" else max(0.0, u)
            scores.append(sc / math.sqrt(IHD))
        top = sorted(range(nb), key=lambda b: (-scores[b], b))[:BUDGET // RATIO]
        vis = set(range(nb * RATIO, t + 1))
        for b in top: vis |= set(range(b * RATIO, (b + 1) * RATIO))
        if a.mutate == "nosparse": vis = set(range(t + 1))
        SEEN[0] += len(vis); SEEN[1] += t + 1
        o = []
        for h in range(HEADS):
            q = rope(rms(qg[t][2 * h * HD:(2 * h + 1) * HD], p1(p["qn"])), t)
            g = qg[t][(2 * h + 1) * HD:(2 * h + 2) * HD]
            kh = h // (HEADS // KV)
            vs = sorted(vis)
            e = [sum(x * y for x, y in zip(q, K[i][kh])) / math.sqrt(HD) for i in vs]
            mx = max(e)
            e = [math.exp(x - mx) for x in e]
            sm = sum(e)
            o += [sum(w / sm * VV[i][kh * HD + j] for w, i in zip(e, vs)) * sig(g[j]) for j in range(HD)]
        outs.append(mv(p["o"], o))
    return outs


def moe(p, x):
    lg = mv(p["router"], x)
    mx = max(lg)
    pr = [math.exp(v - mx) for v in lg]
    sm = sum(pr)
    pr = [v / sm for v in pr]
    top = sorted(range(E), key=lambda e: -pr[e])[:USED]
    tot = sum(pr[e] for e in top)
    o = [0.0] * d
    for e in top:
        y = mv(p["down"][e], [silu(g) * u for g, u in zip(mv(p["gate"][e], x), mv(p["up"][e], x))])
        o = [s0 + pr[e] / tot * v for s0, v in zip(o, y)]
    y = mv(p["sd"], [silu(g) * u for g, u in zip(mv(p["sg"], x), mv(p["su"], x))])
    w = sig(sum(u * v for u, v in zip(p["sgate"], x)))
    return [s0 + w * v for s0, v in zip(o, y)]


def ngram_rows(t):
    """hash rows of the n-grams that end at t; a token further back than the last end of text counts as end of text"""
    seg = 0
    for i in range(t - 1, -1, -1):
        if IDS[i] == EOS and a.mutate != "noeos":
            seg = i + 1
            break
    sh = [IDS[t - k] if t - k >= seg and (k == 0 or t - seg >= k) else EOS for k in range(NGRAM)]
    ids = []
    for g in range(2, NGRAM + 1):
        mix = sh[0] * MULT[0]
        for k in range(1, g): mix ^= sh[k] * MULT[k]
        for i in range(HPN):
            h = (g - 2) * HPN + i
            ids.append(mix % VOC[h] + OFF[h])
    return ids


def ple(p, X):
    gv, nm = [], []
    for t, x in enumerate(X):
        e = [v for r in ngram_rows(t) for v in TABLE[r]]
        k, v, q = rms_g(mv(p["pkey"], e), p["nk"]), mv(p["pval"], e), rms_g(x, p["nq"])
        o = []
        for r in range(R):
            g = sum(x1 * y1 for x1, y1 in zip(k[r * d:(r + 1) * d], q[r * d:(r + 1) * d])) / math.sqrt(d)
            g = math.copysign(math.sqrt(max(abs(g), 1e-6)), g) if g else 0.0
            o += [sig(g) * u for u in v]
        gv.append(o); nm.append(rms_g(o, p["nc"]))
    dil = 1 if a.mutate == "nodilation" else NGRAM
    out = []
    for t, x in enumerate(X):
        y = [silu(sum(p["pconv"][i][PK - 1 - k] * nm[t - k * dil][i] for k in range(PK) if t - k * dil >= 0)) for i in range(D)]
        out.append([x[i] + gv[t][i] + y[i] for i in range(D)])
    return out


X = [W["emb"][i] * R for i in IDS]
states = []
for l, p in enumerate(W["l"]):
    if l == PLE_L: X = ple(p, X)
    rd = [hc_read(p["hc_attn"], x) for x in X]
    ys = (linear_attn if p["lin"] else attention)(p, [r[0] for r in rd])
    X = [hc_write(x, y, r[1]) for x, y, r in zip(X, ys, rd)]
    nx = []
    for x in X:
        m, inj = hc_read(p["hc_ffn"], x)
        nx.append(hc_write(x, moe(p, m), inj))
    X = nx
    states.append(X)
logits = [mv(W["head"], hc_read(W["hc"], x)[0]) for x in X]
with open(os.path.join(a.dir, "ref.bin"), "wb") as f:
    f.write(b"Q4RF" + struct.pack("<4i", a.n, NL, D, V) + struct.pack("<%di" % a.n, *IDS))
    for t in range(a.n):
        for l in range(NL): f.write(struct.pack("<%df" % D, *states[l][t]))
    for t in range(a.n): f.write(struct.pack("<%df" % V, *logits[t]))
print("%s: %d tokens, %d layers, sparse attention sees %d of %d positions, largest logit %.2f"
      % (a.dir, a.n, NL, SEEN[0], SEEN[1], max(abs(v) for r in logits for v in r)))
