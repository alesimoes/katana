#!/usr/bin/env python3
"""check_graph: an independent, plain-Python forward pass of the Qwen3.6 (qwen35moe) and Gemma 4 graphs.

It is written from the published architecture definitions, shares no code with the engine and works on the tiny random
models of make_tiny_gguf.py (float32 tensors). It compares the logits of the last prompt token with the ones katana
dumps (KATANA_DUMP_LOGITS), for several prompt lengths (longer than the sliding window), in batch and token by token.

  python3 tools/tinymodel/check_graph.py ./katana            # exit code 0 = every case agrees
"""
import math, os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tokref"))
import ggufmeta


def load(path):
    kv = ggufmeta.meta(path)
    d = open(path, "rb").read()
    n_t, n_kv = struct.unpack_from("<QQ", d, 8)
    p = 24

    def rs():
        nonlocal p
        n, = struct.unpack_from("<Q", d, p); p += 8
        v = d[p:p + n]; p += n
        return v.decode("utf-8", "surrogateescape")

    def skip(t):
        nonlocal p
        if t == 8: rs()
        elif t == 9:
            et, n = struct.unpack_from("<IQ", d, p); p += 12
            for _ in range(n): skip(et)
        else: p += struct.calcsize(ggufmeta.SZ[t])
    for _ in range(n_kv):
        rs(); t, = struct.unpack_from("<I", d, p); p += 4; skip(t)
    info = []
    for _ in range(n_t):
        nm = rs(); nd, = struct.unpack_from("<I", d, p); p += 4
        dims = struct.unpack_from("<%dQ" % nd, d, p); p += 8 * nd
        ty, off = struct.unpack_from("<IQ", d, p); p += 12
        assert ty == 0
        info.append((nm, dims, off))
    base = (p + 31) // 32 * 32
    T = {}
    for nm, dims, off in info:
        n = 1
        for x in dims: n *= x
        v = list(struct.unpack_from("<%df" % n, d, base + off))
        T[nm] = v if len(dims) == 1 else [v[i * dims[0]:(i + 1) * dims[0]] for i in range(n // dims[0])]
    return kv, T


dot = lambda a, b: sum(x * y for x, y in zip(a, b))
mv = lambda W, x: [dot(r, x) for r in W]
sig = lambda x: 1 / (1 + math.exp(-x))
silu = lambda x: x * sig(x)
gelu = lambda x: 0.5 * x * (1 + math.tanh(math.sqrt(2 / math.pi) * (x + 0.044715 * x ** 3)))
softplus = lambda x: math.log1p(math.exp(x)) if x < 30 else x


def softmax(v):
    m = max(v); e = [math.exp(x - m) for x in v]; s = sum(e)
    return [x / s for x in e]


def rms(x, w, eps):
    k = 1 / math.sqrt(sum(v * v for v in x) / len(x) + eps)
    return [v * k * (g if w else 1.0) for v, g in zip(x, w or x)]


def rope(v, pos, freq, att=1.0):
    """rotates the first 2*len(freq) entries (halves layout); att multiplies cos and sin. pos may be three axes (image
    tokens): frequency i uses axis i mod 3"""
    h = len(freq); o = list(v)
    for i, f in enumerate(freq):
        p = pos[i % 3] if isinstance(pos, (list, tuple)) else pos
        c, s = math.cos(p * f) * att, math.sin(p * f) * att
        o[i], o[h + i] = v[i] * c - v[h + i] * s, v[i] * s + v[h + i] * c
    return o


def yarn(base, dim, factor, orig):
    fr = [base ** (-2 * i / dim) for i in range(dim // 2)]
    if factor <= 1: return fr, 1.0
    corr = lambda rot: dim * math.log(orig / (rot * 2 * math.pi)) / (2 * math.log(base))
    lo, hi = max(math.floor(corr(32)), 0), min(math.ceil(corr(1)), dim - 1)
    if lo == hi: hi += 0.001
    out = []
    for i, f in enumerate(fr):
        ramp = min(max((i - lo) / (hi - lo), 0), 1)
        out.append(f / factor * ramp + f * (1 - ramp))
    return out, 0.1 * math.log(factor) + 1.0


def attend(q, K, V, scale):
    w = softmax([dot(q, k) * scale for k in K])
    o = [0.0] * len(q)
    for a, v in zip(w, V):
        for i in range(len(o)): o[i] += a * v[i]
    return o


def moe(x, logits, K, exp):
    """softmax over all experts, top-K, weights renormalised; exp(e) gives the output of expert e"""
    p = softmax(logits)
    top = sorted(range(len(p)), key=lambda e: -p[e])[:K]
    s = sum(p[e] for e in top)
    y = [0.0] * len(x)
    for e in top:
        o = exp(e)
        for i in range(len(y)): y[i] += p[e] / s * o[i]
    return y


def qwen(kv, T, ids, factor, rows=None, pos3=None):
    """rows: position -> ready input vector (image token); pos3: position -> (t, h, w)"""
    A = kv["general.architecture"]
    g = lambda k, d=None: kv.get(A + "." + k, d)
    dim, heads, eps = g("embedding_length"), g("attention.head_count"), g("attention.layer_norm_rms_epsilon")
    nkv, hd, every = g("attention.head_count_kv"), g("attention.key_length"), g("full_attention_interval")
    dk, ng, nv, inner, CK = g("ssm.state_size"), g("ssm.group_count"), g("ssm.time_step_rank"), g("ssm.inner_size"), g("ssm.conv_kernel")
    dv, He, K = inner // nv, g("expert_feed_forward_length"), g("expert_used_count")
    freq, att = yarn(g("rope.freq_base"), g("rope.dimension_count"), factor, g("context_length"))
    X = [list(rows[p] if rows and p in rows else T["token_embd.weight"][t]) for p, t in enumerate(ids)]
    P = pos3 or list(range(len(ids)))
    for l in range(g("block_count")):
        t = lambda n: T["blk.%d.%s" % (l, n)]
        N = [rms(x, t("attn_norm.weight"), eps) for x in X]
        if (l + 1) % every:
            mixed = [mv(t("attn_qkv.weight"), x) for x in N]
            S = [[[0.0] * dv for _ in range(dk)] for _ in range(nv)]
            for p, x in enumerate(N):
                C = len(mixed[p])
                y = []
                for ch in range(C):
                    w = t("ssm_conv1d.weight")[ch]
                    y.append(silu(sum(w[CK - 1 - j] * mixed[p - j][ch] for j in range(CK) if p - j >= 0)))
                l2 = lambda v: [a / math.sqrt(sum(b * b for b in v) + eps) for a in v]
                q = [l2(y[i * dk:(i + 1) * dk]) for i in range(ng)]
                k = [l2(y[(ng + i) * dk:(ng + i + 1) * dk]) for i in range(ng)]
                v = [y[2 * ng * dk + i * dv:2 * ng * dk + (i + 1) * dv] for i in range(nv)]
                al, be, z = mv(t("ssm_alpha.weight"), x), mv(t("ssm_beta.weight"), x), mv(t("attn_gate.weight"), x)
                o = []
                for h in range(nv):
                    kk, qq = k[h % ng], [a / math.sqrt(dk) for a in q[h % ng]]      # value heads are tiled over the groups in the file
                    dec = math.exp(t("ssm_a")[h] * softplus(al[h] + t("ssm_dt.bias")[h]))
                    b = sig(be[h])
                    Sh = S[h]
                    for i in range(dk):
                        for j in range(dv): Sh[i][j] *= dec
                    mem = [sum(Sh[i][j] * kk[i] for i in range(dk)) for j in range(dv)]
                    dl = [b * (v[h][j] - mem[j]) for j in range(dv)]
                    for i in range(dk):
                        for j in range(dv): Sh[i][j] += kk[i] * dl[j]
                    r = rms([sum(Sh[i][j] * qq[i] for i in range(dk)) for j in range(dv)], t("ssm_norm.weight"), eps)
                    o += [r[j] * silu(z[h * dv + j]) for j in range(dv)]
                X[p] = [a + b for a, b in zip(X[p], mv(t("ssm_out.weight"), o))]
        else:
            Ks, Vs = [[] for _ in range(nkv)], [[] for _ in range(nkv)]
            for p, x in enumerate(N):
                qg, k, v = mv(t("attn_q.weight"), x), mv(t("attn_k.weight"), x), mv(t("attn_v.weight"), x)
                for h in range(nkv):
                    Ks[h].append(rope(rms(k[h * hd:(h + 1) * hd], t("attn_k_norm.weight"), eps), P[p], freq, att))
                    Vs[h].append(v[h * hd:(h + 1) * hd])
                o = []
                for h in range(heads):
                    q = rope(rms(qg[2 * h * hd:(2 * h + 1) * hd], t("attn_q_norm.weight"), eps), P[p], freq, att)
                    r = attend(q, Ks[h * nkv // heads], Vs[h * nkv // heads], hd ** -0.5)
                    o += [a * sig(b) for a, b in zip(r, qg[(2 * h + 1) * hd:(2 * h + 2) * hd])]
                X[p] = [a + b for a, b in zip(X[p], mv(t("attn_output.weight"), o))]
        for p, x0 in enumerate(X):
            x = rms(x0, t("post_attention_norm.weight"), eps)

            def ex(e):
                a, b = mv(t("ffn_gate_exps.weight")[e * He:(e + 1) * He], x), mv(t("ffn_up_exps.weight")[e * He:(e + 1) * He], x)
                return mv(t("ffn_down_exps.weight")[e * dim:(e + 1) * dim], [silu(i) * j for i, j in zip(a, b)])
            y = moe(x, mv(t("ffn_gate_inp.weight"), x), K, ex)
            a, b = mv(t("ffn_gate_shexp.weight"), x), mv(t("ffn_up_shexp.weight"), x)
            sh, gt = mv(t("ffn_down_shexp.weight"), [silu(i) * j for i, j in zip(a, b)]), sig(dot(t("ffn_gate_inp_shexp.weight"), x))
            X[p] = [a + b + gt * c for a, b, c in zip(x0, y, sh)]
    return mv(T["output.weight"], rms(X[-1], T["output_norm.weight"], eps))


def gemma(kv, T, ids, factor):
    A = "gemma4"
    g = lambda k, d=None: kv.get(A + "." + k, d)
    dim, heads, eps, win = g("embedding_length"), g("attention.head_count"), g("attention.layer_norm_rms_epsilon"), g("attention.sliding_window")
    He, K, cap = g("expert_feed_forward_length"), g("expert_used_count"), g("final_logit_softcapping")
    X = [[v * math.sqrt(dim) for v in T["token_embd.weight"][t]] for t in ids]
    for l in range(g("block_count")):
        t = lambda n: T["blk.%d.%s" % (l, n)]
        loc = g("attention.sliding_window_pattern")[l]
        hd, nkv = g("attention.key_length_swa" if loc else "attention.key_length"), g("attention.head_count_kv")[l]
        base = g("rope.freq_base_swa" if loc else "rope.freq_base")
        freq = [base ** (-2 * i / hd) for i in range(hd // 2)]
        Ks, Vs = [[] for _ in range(nkv)], [[] for _ in range(nkv)]
        N = [rms(x, t("attn_norm.weight"), eps) for x in X]
        for p, x in enumerate(N):
            q, k = mv(t("attn_q.weight"), x), mv(t("attn_k.weight"), x)
            v = mv(t("attn_v.weight"), x) if "blk.%d.attn_v.weight" % l in T else k
            for h in range(nkv):
                Ks[h].append(rope(rms(k[h * hd:(h + 1) * hd], t("attn_k_norm.weight"), eps), p, freq))
                Vs[h].append(rms(v[h * hd:(h + 1) * hd], None, eps))
            o = []
            lo = max(0, p - win + 1) if loc else 0
            for h in range(heads):
                qq = rope(rms(q[h * hd:(h + 1) * hd], t("attn_q_norm.weight"), eps), p, freq)
                o += attend(qq, Ks[h * nkv // heads][lo:], Vs[h * nkv // heads][lo:], 1.0)
            X[p] = [a + b for a, b in zip(X[p], rms(mv(t("attn_output.weight"), o), t("post_attention_norm.weight"), eps))]
        for p, x0 in enumerate(X):
            x = rms(x0, t("ffn_norm.weight"), eps)
            y = mv(t("ffn_down.weight"), [gelu(a) * b for a, b in zip(mv(t("ffn_gate.weight"), x), mv(t("ffn_up.weight"), x))])
            if "blk.%d.ffn_gate_inp.weight" % l in T:
                y = rms(y, t("post_ffw_norm_1.weight"), eps)
                r = [a * s / math.sqrt(dim) for a, s in zip(rms(x0, None, eps), t("ffn_gate_inp.scale"))]
                z = rms(x0, t("pre_ffw_norm_2.weight"), eps)
                gu, dn, es = t("ffn_gate_up_exps.weight"), t("ffn_down_exps.weight"), t("ffn_down_exps.scale")

                def ex(e):
                    a, b = mv(gu[2 * e * He:(2 * e + 1) * He], z), mv(gu[(2 * e + 1) * He:(2 * e + 2) * He], z)
                    return [es[e] * v for v in mv(dn[e * dim:(e + 1) * dim], [gelu(i) * j for i, j in zip(a, b)])]
                m = rms(moe(z, mv(t("ffn_gate_inp.weight"), r), K, ex), t("post_ffw_norm_2.weight"), eps)
                y = [a + b for a, b in zip(y, m)]
            X[p] = [(a + b) * t("layer_output_scale.weight")[0] for a, b in zip(x0, rms(y, t("post_ffw_norm.weight"), eps))]
    return [cap * math.tanh(v / cap) for v in mv(T["token_embd.weight"], rms(X[-1], T["output_norm.weight"], eps))]


def main():
    k = os.path.abspath(sys.argv[1])
    bad = 0
    text = "the hello and an other line: 12 + 7 = 19, ok? in the end\n"
    with tempfile.TemporaryDirectory() as tmp:
        for arch, extra in (("qwen35moe", []), ("qwen35moe", ["--kv", "2", "--used", "4"]), ("gemma4", []), ("gemma4", ["--used", "4"])):
            m = os.path.join(tmp, "m.gguf")
            subprocess.run([sys.executable, os.path.join(HERE, "make_tiny_gguf.py"), m, "--arch", arch, "--seed", "5"] + extra, check=True, stdout=subprocess.DEVNULL)
            kv, T = load(m)
            for nch, scale in ((3, 1), (17, 1), (len(text), 1), (40, 4)):
                if scale > 1 and arch == "gemma4": continue
                t = text[:nch]
                tf = os.path.join(tmp, "t.txt"); open(tf, "w").write(t)
                out = subprocess.run([k, "tokenize", m, "--ids", tf], capture_output=True, text=True).stdout.strip().splitlines()[-1]
                ids = [int(x) for x in out.split()]
                ref = (gemma if arch == "gemma4" else qwen)(kv, T, ids, scale)
                for mode, env in (("batch", {}), ("one by one", {"KATANA_QN_BATCH": "0", "KATANA_G4_BATCH": "0"}), ("KV fp16", {"KATANA_CPU_KV16": "1"})):
                    f = os.path.join(tmp, "l.bin")
                    e = dict(os.environ, KATANA_CPU="1", KATANA_DUMP_LOGITS=f, KATANA_SPEC="", **env)
                    if scale > 1: e["KATANA_ROPE_SCALE"] = str(scale)
                    subprocess.run([k, "run", m, t, "1", "0"], env=e, capture_output=True)
                    got = struct.unpack("<%df" % len(ref), open(f, "rb").read()); os.remove(f)
                    d = max(abs(a - b) for a, b in zip(got, ref)); mx = max(abs(x) for x in ref)
                    ok = d < (3e-2 if mode == "KV fp16" else 1e-4) * mx
                    bad += not ok
                    print("%-4s %-9s %-18s %2d tokens rope x%d %-10s max difference %.2e of %.2f" % ("ok" if ok else "FAIL", arch, " ".join(extra), len(ids), scale, mode, d, mx))
    print("FAILED %d" % bad if bad else "OK")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
