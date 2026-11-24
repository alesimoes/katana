"""Referencia float32 do gradiente do LoRA (autograd do PyTorch) sobre o Flux2Transformer2DModel a partir do GGUF Q4_K_M.
Uso: python3 ref_lora_grad.py <pasta do selftest-dump>; compara com os gradientes calculados em C (katana render flux-train selftest-dump <pasta>).
Leitor de GGUF e desquantizadores K-quant escritos aqui, sem codigo do katana; nomes convertidos pelo conversor oficial do diffusers."""
import sys, struct, json, os
sys.path.insert(0, "/mnt/dados/render-ref/lib")
import numpy as np, torch
from diffusers.loaders.single_file_utils import convert_flux2_transformer_checkpoint_to_diffusers
from diffusers import Flux2Transformer2DModel

G = "/mnt/dados/source/katana/models/flux2klein/gguf/flux-2-klein-4b-Q4_K_M.gguf"
CFG = "/mnt/dados/source/katana/models/flux2klein/FLUX.2-klein-4B/transformer/config.json"
OUT = sys.argv[1]
torch.set_num_threads(8)

def rd(f, fmt): return struct.unpack(fmt, f.read(struct.calcsize(fmt)))
def rstr(f): (n,) = rd(f, "<Q"); return f.read(n).decode()
def rval(f, t):
    if t == 8: return rstr(f)
    if t == 9:
        (et,) = rd(f, "<I"); (n,) = rd(f, "<Q"); return [rval(f, et) for _ in range(n)]
    return rd(f, {0: "<B", 1: "<b", 2: "<H", 3: "<h", 4: "<I", 5: "<i", 6: "<f", 7: "<?", 10: "<Q", 11: "<q", 12: "<d"}[t])[0]

def scale_min(sc):                      # sc [nb,12] uint8 -> scales, mins [nb,8] (get_scale_min_k4)
    s = np.zeros((sc.shape[0], 8), np.float32); m = np.zeros_like(s)
    for j in range(8):
        if j < 4: s[:, j] = sc[:, j] & 63; m[:, j] = sc[:, j + 4] & 63
        else:
            s[:, j] = (sc[:, j + 4] & 0xF) | ((sc[:, j - 4] >> 6) << 4)
            m[:, j] = (sc[:, j + 4] >> 4) | ((sc[:, j] >> 6) << 4)
    return s, m

def deq_q4k(b, n):
    nb = n // 256; a = np.frombuffer(b, np.uint8).reshape(nb, 144)
    d = a[:, 0:2].copy().view(np.float16).astype(np.float32); dm = a[:, 2:4].copy().view(np.float16).astype(np.float32)
    s, m = scale_min(a[:, 4:16]); q = a[:, 16:]
    out = np.zeros((nb, 256), np.float32)
    for g in range(4):                  # 4 grupos de 64 valores: 32 baixos (sub-bloco 2g) e 32 altos (2g+1)
        qq = q[:, 32 * g:32 * g + 32]
        out[:, 64 * g:64 * g + 32] = d * s[:, 2 * g:2 * g + 1] * (qq & 0xF) - dm * m[:, 2 * g:2 * g + 1]
        out[:, 64 * g + 32:64 * g + 64] = d * s[:, 2 * g + 1:2 * g + 2] * (qq >> 4) - dm * m[:, 2 * g + 1:2 * g + 2]
    return out.reshape(-1)

def deq_q5k(b, n):
    nb = n // 256; a = np.frombuffer(b, np.uint8).reshape(nb, 176)
    d = a[:, 0:2].copy().view(np.float16).astype(np.float32); dm = a[:, 2:4].copy().view(np.float16).astype(np.float32)
    s, m = scale_min(a[:, 4:16]); qh = a[:, 16:48]; q = a[:, 48:]
    out = np.zeros((nb, 256), np.float32)
    for g in range(4):
        qq = q[:, 32 * g:32 * g + 32]
        hi1 = ((qh >> (2 * g)) & 1).astype(np.float32) * 16; hi2 = ((qh >> (2 * g + 1)) & 1).astype(np.float32) * 16
        out[:, 64 * g:64 * g + 32] = d * s[:, 2 * g:2 * g + 1] * ((qq & 0xF) + hi1) - dm * m[:, 2 * g:2 * g + 1]
        out[:, 64 * g + 32:64 * g + 64] = d * s[:, 2 * g + 1:2 * g + 2] * ((qq >> 4) + hi2) - dm * m[:, 2 * g + 1:2 * g + 2]
    return out.reshape(-1)

def deq_bf16(b, n):
    return (np.frombuffer(b, np.uint16).astype(np.uint32) << 16).view(np.float32)

f = open(G, "rb"); assert f.read(4) == b"GGUF"
ver, nt, nkv = rd(f, "<IQQ")
for _ in range(nkv): rstr(f); (t,) = rd(f, "<I"); rval(f, t)
infos = []
for _ in range(nt):
    name = rstr(f); (nd,) = rd(f, "<I"); dims = list(rd(f, "<%dQ" % nd)); (ty,) = rd(f, "<I"); (off,) = rd(f, "<Q"); infos.append((name, dims, ty, off))
start = (f.tell() + 31) // 32 * 32
BS = {0: (1, 4), 12: (256, 144), 13: (256, 176), 30: (1, 2)}
sd = {}
for name, dims, ty, off in infos:
    n = int(np.prod(dims)); bl, by = BS[ty]; f.seek(start + off); raw = f.read(n // bl * by)
    v = np.frombuffer(raw, np.float32) if ty == 0 else deq_q4k(raw, n) if ty == 12 else deq_q5k(raw, n) if ty == 13 else deq_bf16(raw, n)
    sd[name] = torch.from_numpy(np.ascontiguousarray(v.reshape(list(reversed(dims)))).copy())
print("tensores", len(sd), flush=True)
sd = convert_flux2_transformer_checkpoint_to_diffusers(sd)
cfg = json.load(open(CFG)); cfg = {k: v for k, v in cfg.items() if not k.startswith("_")}
with torch.device("meta"):
    model = Flux2Transformer2DModel(**cfg)
print(model.load_state_dict(sd, assign=True, strict=True), flush=True)
model.eval()


import struct as _st
def read_fxlora(path):
    f = open(path, "rb"); assert f.read(8) == b"FXLORA01"; rd(f, "<QQ"); (n,) = rd(f, "<I"); mods = {}
    for _ in range(n):
        (nl,) = rd(f, "<I"); name = f.read(nl).decode(); rows, cols = rd(f, "<QQ"); (r,) = rd(f, "<i"); (s,) = rd(f, "<f")
        A = np.frombuffer(f.read(4 * r * cols), np.float32).reshape(r, cols).copy(); B = np.frombuffer(f.read(4 * rows * r), np.float32).reshape(rows, r).copy()
        mods[name] = (rows, cols, r, s, A, B)
    return mods
def rb(name, shape=None):
    a = np.fromfile(OUT + "/" + name, np.float32)
    return torch.from_numpy(a.reshape(shape) if shape else a)
lo = read_fxlora(OUT + "/lora.fxlora"); gr = read_fxlora(OUT + "/grads.fxlora")
leaf = {k: (torch.tensor(v[4], requires_grad=True), torch.tensor(v[5], requires_grad=True), v[3]) for k, v in lo.items()}
import torch.nn.functional as Fn
def patch(lin, key, r0, r1):
    A, B, s = leaf[key + ".weight"]
    W = lin.weight; 
    def fwd(x, lin=lin, A=A, B=B, s=s, r0=r0, r1=r1):
        return Fn.linear(x, lin.weight) + s * Fn.linear(Fn.linear(x, A), B[r0:r1])
    lin.forward = fwd
D = 3072
for i, b in enumerate(model.transformer_blocks):
    for st, (q, k, v, o, ff) in (("img", (b.attn.to_q, b.attn.to_k, b.attn.to_v, b.attn.to_out[0], b.ff)), ("txt", (b.attn.add_q_proj, b.attn.add_k_proj, b.attn.add_v_proj, b.attn.to_add_out, b.ff_context))):
        base = "double_blocks.%d.%s_" % (i, st)
        patch(q, base + "attn.qkv", 0, D); patch(k, base + "attn.qkv", D, 2 * D); patch(v, base + "attn.qkv", 2 * D, 3 * D)
        patch(o, base + "attn.proj", 0, D); patch(ff.linear_in, base + "mlp.0", 0, 2 * 9216); patch(ff.linear_out, base + "mlp.2", 0, D)
for i, b in enumerate(model.single_transformer_blocks):
    patch(b.attn.to_qkv_mlp_proj, "single_blocks.%d.linear1" % i, 0, 3 * D + 2 * 9216); patch(b.attn.to_out, "single_blocks.%d.linear2" % i, 0, D)
x = rb("in_img.bin").reshape(1, -1, 128); txt = rb("in_txt.bin").reshape(1, -1, 7680); tgt = rb("target.bin").reshape(1, -1, 128)
ii = rb("ids_img.bin").reshape(1, -1, 4); it = rb("ids_txt.bin").reshape(1, -1, 4); t = rb("t.bin"); Ntg = tgt.shape[1]
out = model(hidden_states=x, encoder_hidden_states=txt, timestep=t, img_ids=ii, txt_ids=it, guidance=None, return_dict=False)[0]
loss = ((out[:, :Ntg] - tgt) ** 2).mean(); loss.backward()
cl = float(rb("loss.bin")[0]); pc = rb("pred.bin").reshape(1, Ntg, 128)
print("perda torch %.8f  perda C %.8f  erro relativo %.2e" % (float(loss), cl, abs(float(loss) - cl) / abs(cl)))
print("previsao: erro max %.2e  |pred| max %.3f" % (float((out[:, :Ntg] - pc).abs().max()), float(pc.abs().max())))
worst = 0.0; rows = []
for k, (A, B, s) in leaf.items():
    for nm, tg, cg in (("A", A.grad, gr[k][4]), ("B", B.grad, gr[k][5])):
        cg = torch.from_numpy(cg); den = float(tg.abs().max()) + 1e-30
        rel = float((tg - cg).abs().max()) / den; worst = max(worst, rel); rows.append((rel, k, nm, den))
rows.sort(reverse=True)
print("gradientes dos 80 modulos (A e B = 160 tensores): erro max / |grad torch| max  pior %.2e  mediana %.2e" % (worst, rows[len(rows) // 2][0]))
for r in rows[:5]: print("  pior: %.2e  %s.%s  (|grad| max %.3e)" % r)
tot_t = sum(float((A.grad ** 2).sum() + (B.grad ** 2).sum()) for A, B, s in leaf.values()) ** 0.5
tot_c = sum(float((gr[k][4] ** 2).sum() + (gr[k][5] ** 2).sum()) for k in gr) ** 0.5
print("norma global do gradiente: torch %.6e  C %.6e  razao %.6f" % (tot_t, tot_c, tot_c / tot_t))
