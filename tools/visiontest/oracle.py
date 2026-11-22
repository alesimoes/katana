#!/usr/bin/env python3
"""Vision oracle: every stage of src/vision against an independent implementation.

Written from the published definitions (image processor, vision encoder, position rule, interpolation); shares no code
with the engine. Needs numpy and Pillow (decoders and bicubic resize are compared with Pillow pixel by pixel).

  python3 tools/visiontest/oracle.py ./katana            # exit code 0 = every case agrees
"""
import io, math, os, random, struct, subprocess, sys, tempfile, zlib
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tinymodel"))
import check_graph

K = os.path.abspath(sys.argv[1])
TMP = tempfile.mkdtemp()
bad = 0


def ok(c, what):
    global bad
    bad += not c
    print("%-4s %s" % ("ok" if c else "FAIL", what), flush=True)


def run(*a, env=None):
    return subprocess.run([K] + [str(x) for x in a], capture_output=True, text=True, errors="replace", env=dict(os.environ, **(env or {})))


def ppm(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(int)


# ---- decoders ----
def picture(w, h, seed=0):
    r = np.random.RandomState(seed)
    y, x = np.mgrid[0:h, 0:w]
    a = np.stack([(x * 255 / max(w - 1, 1)), (y * 255 / max(h - 1, 1)), 127 + 120 * np.sin(x / 7.0 + y / 11.0)], -1)
    im = Image.fromarray(np.clip(a + r.normal(0, 6, a.shape), 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(im)
    d.ellipse([w // 4, h // 4, w // 2, h // 2], fill=(250, 20, 30))
    d.line([0, 0, w, h], fill=(0, 0, 255), width=2)
    return im


def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))


def adam7(a, ct, bd):
    """interlaced PNG from an array [h][w][channels] (8 or 16 bits)"""
    h, w = a.shape[:2]
    raw = b""
    for x0, y0, dx, dy in ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)):
        s = a[y0::dy, x0::dx]
        if s.size == 0: continue
        for row in s: raw += b"\0" + (row.astype(">u2").tobytes() if bd == 16 else row.astype(np.uint8).tobytes())
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, bd, ct, 0, 0, 1)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def dec(data, name):
    p = os.path.join(TMP, "i." + name); open(p, "wb").write(data)
    r = run("vision", "decode", p, os.path.join(TMP, "o.ppm"))
    return (ppm(os.path.join(TMP, "o.ppm")), "") if r.returncode == 0 else (None, r.stderr.strip())


def white(im):
    """alpha over white, same rounding as the engine"""
    a = np.asarray(im.convert("RGBA")).astype(int); al = a[..., 3:]
    return (a[..., :3] * al + 255 * (255 - al) + 127) // 255


def decoders():
    for w, h in ((1, 1), (17, 5), (97, 64), (260, 131)):
        base = picture(w, h)
        for mode in ("RGB", "L", "1", "P", "RGBA", "LA", "PA4"):
            im = base
            if mode in ("RGBA", "LA"):
                im = base.convert("RGB").convert(mode); al = Image.fromarray((np.mgrid[0:h, 0:w][1] * 255 // max(w - 1, 1)).astype(np.uint8)); im.putalpha(al)
            elif mode == "PA4": im = base.quantize(13)
            elif mode != "RGB": im = base.convert(mode)
            b = io.BytesIO(); im.save(b, "PNG")
            got, e = dec(b.getvalue(), "png")
            want = white(im) if mode in ("RGBA", "LA") else np.asarray(im.convert("RGB")).astype(int)
            ok(got is not None and (got == want).all(), "png %s %dx%d %s" % (mode, w, h, e))
        a = np.asarray(base)
        got, e = dec(adam7(a, 2, 8), "png"); ok(got is not None and (got == a).all(), "png interlaced %dx%d %s" % (w, h, e))
        got, e = dec(adam7(a.astype(int) * 257, 2, 16), "png"); ok(got is not None and (got == a).all(), "png interlaced 16 bits %dx%d %s" % (w, h, e))
        b = io.BytesIO(); base.save(b, "BMP"); got, e = dec(b.getvalue(), "bmp"); ok(got is not None and (got == a).all(), "bmp 24 %dx%d %s" % (w, h, e))
        for m in ("L", "P", "1"):
            b = io.BytesIO(); base.convert(m).save(b, "BMP"); got, e = dec(b.getvalue(), "bmp")
            ok(got is not None and (got == np.asarray(base.convert(m).convert("RGB"))).all(), "bmp %s %dx%d %s" % (m, w, h, e))
        for q in (30, 75, 95, 100):
            for ss in (0, 1, 2):
                for prog in (False, True):
                    b = io.BytesIO(); base.save(b, "JPEG", quality=q, subsampling=ss, progressive=prog)
                    got, e = dec(b.getvalue(), "jpg")
                    want = np.asarray(Image.open(io.BytesIO(b.getvalue())).convert("RGB")).astype(int)
                    d = -1 if got is None else np.abs(got - want).max()
                    ok(d == 0, "jpeg q%d 4:%s %s %dx%d: largest difference %d %s" % (q, ("44", "22", "20")[ss], "progressive" if prog else "baseline", w, h, d, e))
        b = io.BytesIO(); base.convert("L").save(b, "JPEG", quality=80); got, e = dec(b.getvalue(), "jpg")
        ok(got is not None and (got == np.asarray(Image.open(io.BytesIO(b.getvalue())).convert("RGB"))).all(), "jpeg gray %s" % e)
        b = io.BytesIO(); base.save(b, "JPEG", quality=80, restart_marker_blocks=3); got, e = dec(b.getvalue(), "jpg")
        ok(got is not None and (got == np.asarray(Image.open(io.BytesIO(b.getvalue())).convert("RGB"))).all(), "jpeg restart markers %s" % e)
        from PIL import ImageOps
        for o in range(1, 9):
            ex = Image.Exif(); ex[0x0112] = o
            b = io.BytesIO(); base.save(b, "JPEG", quality=90, exif=ex); got, e = dec(b.getvalue(), "jpg")
            want = np.asarray(ImageOps.exif_transpose(Image.open(io.BytesIO(b.getvalue()))).convert("RGB")).astype(int)
            ok(got is not None and got.shape == want.shape and (got == want).all(), "jpeg orientation %d %s" % (o, e))
        b = io.BytesIO(); base.convert("CMYK").save(b, "JPEG", quality=95); got, e = dec(b.getvalue(), "jpg")
        want = np.asarray(Image.open(io.BytesIO(b.getvalue())).convert("RGB")).astype(int)
        ok(got is not None and np.abs(got - want).max() <= 3, "jpeg cmyk %s" % e)
    for name, data in (("bomb", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 60000, 60000, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"\0" * 100000)) + chunk(b"IEND", b"")),
                       ("short data", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 100, 100, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"\0" * 1000)) + chunk(b"IEND", b"")),
                       ("long data", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 10, 10, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"\0" * 1000000)) + chunk(b"IEND", b"")),
                       ("webp", b"RIFF\0\0\0\0WEBPVP8 " + b"\0" * 100), ("empty", b""), ("text", b"hello world, not an image at all")):
        got, e = dec(data, "png"); ok(got is None and e.startswith("error:"), "refused: %s (%s)" % (name, e[-90:]))


# ---- size rule and bicubic resize ----
def smart(height, width, factor, mn, mx):
    if max(height, width) / min(height, width) > 200: return None
    h = round(height / factor) * factor; w = round(width / factor) * factor
    if h * w > mx:
        b = math.sqrt((height * width) / mx)
        h = max(factor, math.floor(height / b / factor) * factor); w = max(factor, math.floor(width / b / factor) * factor)
    elif h * w < mn:
        b = math.sqrt(mn / (height * width)); h = math.ceil(height * b / factor) * factor; w = math.ceil(width * b / factor) * factor
    return h, w


def resizing():
    rnd = random.Random(3); n = miss = 0
    for _ in range(300):
        h, w, t = rnd.choice([rnd.randint(1, 60), rnd.randint(50, 5000), 16 * rnd.randint(1, 100)]), rnd.choice([rnd.randint(1, 60), rnd.randint(50, 5000), 16 * rnd.randint(1, 100)]), rnd.choice([64, 256, 1000, 1280, 4096])
        want = smart(h, w, 32, 65536, t * 1024); got = run("vision", "size", h, w, t).stdout.split()
        n += 1; miss += (got != ["invalid"]) if want is None else [int(x) for x in got] != list(want)
    ok(miss == 0, "size rule: %d cases, %d different" % (n, miss))
    for (w, h), (ow, oh) in (((97, 64), (64, 64)), ((97, 64), (320, 256)), ((1, 1), (64, 96)), ((640, 33), (1280, 64)), ((1537, 911), (1216, 704)), ((300, 300), (300, 64)), ((64, 777), (96, 777)), ((3000, 2000), (1216, 800))):
        im = picture(w, h, w); p = os.path.join(TMP, "r.png"); im.save(p)
        run("vision", "resize", p, ow, oh, os.path.join(TMP, "o.ppm"))
        d = np.abs(ppm(os.path.join(TMP, "o.ppm")) - np.asarray(im.resize((ow, oh), Image.BICUBIC)).astype(int)).max()
        ok(d == 0, "bicubic %dx%d -> %dx%d: largest difference %d" % (w, h, ow, oh, d))


# ---- vision encoder ----
def gguf(path, kvs, tensors):
    s = lambda t: struct.pack("<Q", len(t)) + t.encode()
    def kv(k, v):
        if isinstance(v, str): return s(k) + struct.pack("<I", 8) + s(v)
        if isinstance(v, bool): return s(k) + struct.pack("<IB", 7, v)
        if isinstance(v, int): return s(k) + struct.pack("<II", 4, v)
        if isinstance(v, float): return s(k) + struct.pack("<If", 6, v)
        return s(k) + struct.pack("<IIQ", 9, 6, len(v)) + struct.pack("<%df" % len(v), *v)
    info = data = b""
    for n, a in tensors:
        f16 = a.dtype == np.float16
        info += s(n) + struct.pack("<I", a.ndim) + b"".join(struct.pack("<Q", d) for d in a.shape[::-1]) + struct.pack("<IQ", 1 if f16 else 0, len(data))
        b = a.tobytes(); data += b + b"\0" * (-len(b) % 32)
    h = b"GGUF" + struct.pack("<IQQ", 3, len(tensors), len(kvs)) + b"".join(kv(k, v) for k, v in kvs) + info
    open(path, "wb").write(h + b"\0" * (-len(h) % 32) + data)


def projector(path, d, ff, depth, heads, out, side=6, seed=1, dtype=np.float16, base="tiny"):
    r = np.random.RandomState(seed)
    m = lambda *s: r.normal(0, 1.0 / math.sqrt(s[-1]), s).astype(dtype)
    v = lambda n, c=0.0: (c + r.normal(0, 0.1, n)).astype(np.float32)
    T = [("v.patch_embd.weight", m(d, 3, 16, 16)), ("v.patch_embd.weight.1", m(d, 3, 16, 16)), ("v.patch_embd.bias", v(d)),
         ("v.position_embd.weight", r.normal(0, 0.3, (side * side, d)).astype(np.float32)), ("v.post_ln.weight", v(d, 1)), ("v.post_ln.bias", v(d)),
         ("mm.0.weight", m(4 * d, 4 * d)), ("mm.0.bias", v(4 * d)), ("mm.2.weight", m(out, 4 * d)), ("mm.2.bias", v(out))]
    for l in range(depth):
        p = "v.blk.%d." % l
        T += [(p + "ln1.weight", v(d, 1)), (p + "ln1.bias", v(d)), (p + "ln2.weight", v(d, 1)), (p + "ln2.bias", v(d)),
              (p + "attn_qkv.weight", m(3 * d, d)), (p + "attn_qkv.bias", v(3 * d)), (p + "attn_out.weight", m(d, d)), (p + "attn_out.bias", v(d)),
              (p + "ffn_up.weight", m(ff, d)), (p + "ffn_up.bias", v(ff)), (p + "ffn_down.weight", m(d, ff)), (p + "ffn_down.bias", v(d))]
    gguf(path, [("general.architecture", "clip"), ("general.basename", base), ("clip.projector_type", "qwen3vl_merger"), ("clip.vision.projection_dim", out),
                ("clip.vision.patch_size", 16), ("clip.vision.embedding_length", d), ("clip.vision.feed_forward_length", ff), ("clip.vision.block_count", depth),
                ("clip.vision.attention.head_count", heads), ("clip.vision.image_mean", [0.5, 0.45, 0.4]), ("clip.vision.image_std", [0.5, 0.55, 0.25]),
                ("clip.vision.spatial_merge_size", 2), ("clip.vision.attention.layer_norm_epsilon", 1e-6)], T)
    return {n: a.astype(np.float64) for n, a in T}


def order(gh, gw):
    """patch order: merge blocks in raster order, 2x2 inside"""
    return [(by + y, bx + x) for by in range(0, gh, 2) for bx in range(0, gw, 2) for y in range(2) for x in range(2)]


def encoder(T, a, heads, depth, mean, std):
    """a: resized image [h][w][3]; returns every stage in double precision"""
    out = {}
    gh, gw = a.shape[0] // 16, a.shape[1] // 16
    x = ((a / 255.0 - np.array(mean)) / np.array(std)).transpose(2, 0, 1)
    P = np.stack([x[:, r * 16:r * 16 + 16, c * 16:c * 16 + 16].reshape(-1) for r, c in order(gh, gw)])
    out["patches0"] = P
    d = T["v.patch_embd.weight"].shape[0]
    W = T["v.patch_embd.weight"].reshape(d, -1) + T["v.patch_embd.weight.1"].reshape(d, -1)      # the still image is both frames
    X = P @ W.T + T["v.patch_embd.bias"]
    pos = T["v.position_embd.weight"]; S = int(round(math.sqrt(len(pos))))
    for i, (r, c) in enumerate(order(gh, gw)):                                                    # bilinear, aligned corners
        sy, sx = r * (S - 1) / max(gh - 1, 1), c * (S - 1) / max(gw - 1, 1)
        for yy in (math.floor(sy), math.floor(sy) + 1):
            for xx in (math.floor(sx), math.floor(sx) + 1):
                X[i] += max(0, 1 - abs(sy - yy)) * max(0, 1 - abs(sx - xx)) * pos[min(yy, S - 1) * S + min(xx, S - 1)]
    out["embed0"] = X.copy()
    hd = d // heads
    inv = 1.0 / 10000.0 ** (np.arange(0, hd // 2, 2) / (hd // 2))
    ang = np.array([np.concatenate([r * inv, c * inv]) for r, c in order(gh, gw)]); ang = np.concatenate([ang, ang], -1)
    half = lambda t: np.concatenate([-t[..., hd // 2:], t[..., :hd // 2]], -1)
    def ln(t, n):
        mu = t.mean(-1, keepdims=True); return (t - mu) / np.sqrt(((t - mu) ** 2).mean(-1, keepdims=True) + 1e-6) * T[n + ".weight"] + T[n + ".bias"]
    for l in range(depth):
        p = "v.blk.%d." % l
        q = ln(X, p + "ln1") @ T[p + "attn_qkv.weight"].T + T[p + "attn_qkv.bias"]
        Q, Ky, V = [q[:, i * d:(i + 1) * d].reshape(-1, heads, hd).transpose(1, 0, 2) for i in range(3)]
        Q = Q * np.cos(ang) + half(Q) * np.sin(ang); Ky = Ky * np.cos(ang) + half(Ky) * np.sin(ang)
        s = Q @ Ky.transpose(0, 2, 1) / math.sqrt(hd); s = np.exp(s - s.max(-1, keepdims=True)); s /= s.sum(-1, keepdims=True)
        o = (s @ V).transpose(1, 0, 2).reshape(-1, d)
        X = X + o @ T[p + "attn_out.weight"].T + T[p + "attn_out.bias"]
        f = ln(X, p + "ln2") @ T[p + "ffn_up.weight"].T + T[p + "ffn_up.bias"]
        f = 0.5 * f * (1 + np.tanh(math.sqrt(2 / math.pi) * (f + 0.044715 * f ** 3)))
        X = X + f @ T[p + "ffn_down.weight"].T + T[p + "ffn_down.bias"]
        out["block%d" % l] = X.copy()
    m = ln(X, "v.post_ln").reshape(len(X) // 4, 4 * d) @ T["mm.0.weight"].T + T["mm.0.bias"]
    m = 0.5 * m * (1 + np.vectorize(math.erf)(m / math.sqrt(2)))
    out["merger0"] = m @ T["mm.2.weight"].T + T["mm.2.bias"]
    return out


def stages(path):
    d = open(path, "rb").read(); p = 0; r = {}
    while p < len(d):
        n = d[p:p + 32].rstrip(b"\0").decode(); k, = struct.unpack_from("<Q", d, p + 32)
        r[n] = np.frombuffer(d, np.float32, k, p + 40); p += 40 + 4 * k
    return r


def vit():
    for d, ff, depth, heads, out, (w, h), tk, dt in ((32, 72, 3, 4, 24, (200, 120), 70, np.float16), (48, 100, 2, 2, 64, (90, 400), 100, np.float32), (144, 538, 2, 2, 40, (320, 320), 120, np.float16), (72, 64, 2, 1, 7, (40, 4000), 200, np.float16)):
        pj = os.path.join(TMP, "p.gguf"); T = projector(pj, d, ff, depth, heads, out, dtype=dt, seed=d)
        im = picture(w, h, d); ip = os.path.join(TMP, "v.png"); im.save(ip)
        oh, ow = smart(h, w, 32, 65536, tk * 1024)
        want = encoder(T, np.asarray(im.resize((ow, oh), Image.BICUBIC)).astype(float), heads, depth, [0.5, 0.45, 0.4], [0.5, 0.55, 0.25])
        for mode, env in (("widest", {}), ("avx2", {"KATANA_VISION_AVX2": "1"})):
            r = run("vision", "stages", pj, ip, os.path.join(TMP, "s.bin"), tk, env=env)
            got = stages(os.path.join(TMP, "s.bin")) if r.returncode == 0 else {}
            worst = 0
            for n, a in want.items():
                e = np.abs(got[n] - a.reshape(-1)).max() / np.abs(a).max() if n in got and got[n].size == a.size else 9
                worst = max(worst, e)
            ok(worst < 1e-5 and len(got) == len(want), "encoder width %d, %d heads, grid %dx%d, %s: %d stages, largest relative difference %.1e" % (d, heads, oh // 16, ow // 16, mode, len(want), worst))


# ---- three-axis positions and the language model with image vectors ----
def positions(gw, gh, before, after):
    p = [(i, i, i) for i in range(before)]
    p += [(before, before + r, before + c) for r in range(gh) for c in range(gw)]
    n = before + max(gw, gh)
    return p + [(n + i,) * 3 for i in range(after)]


def language():
    for a in ((5, 3, 4, 6), (3, 5, 0, 3), (1, 64, 2, 2), (8, 8, 10, 0)):
        got = [tuple(map(int, x.split())) for x in run("vision", "positions", *a).stdout.splitlines()]
        ok(got == positions(*a), "positions: grid %dx%d, %d tokens before, %d after" % a)
    for extra in ([], ["--kv", "2", "--used", "4"]):
        m = os.path.join(TMP, "tiny-model.gguf")
        subprocess.run([sys.executable, os.path.join(HERE, "..", "tinymodel", "make_tiny_gguf.py"), m, "--arch", "qwen35moe", "--seed", "7", "--vision"] + extra, check=True, stdout=subprocess.DEVNULL)
        pj = os.path.join(TMP, "mmproj-tiny.gguf"); PT = projector(pj, 32, 72, 2, 4, 64, seed=4)
        kv, T = check_graph.load(m)
        im = picture(150, 110, 2); ip = os.path.join(TMP, "l.png"); im.save(ip)
        oh, ow = smart(110, 150, 32, 65536, 64 * 1024)
        E = encoder(PT, np.asarray(im.resize((ow, oh), Image.BICUBIC)).astype(float), 4, 2, [0.5, 0.45, 0.4], [0.5, 0.55, 0.25])["merger0"]
        gw, gh = ow // 32, oh // 32
        tf = os.path.join(TMP, "t.txt")
        def tok(t):
            open(tf, "w").write(t); return [int(x) for x in run("tokenize", m, "--ids", tf).stdout.strip().splitlines()[-1].split()]
        st, en = tok("<|vision_start|>")[0], tok("<|vision_end|>")[0]
        post = tok("the hello and other\n")
        for ns in (1, 2):      # one image; the same image twice with text between
            ids, rows, pos, cur = [], {}, [], 0
            for _ in range(ns):
                ids.append(st); pos.append((cur,) * 3); cur += 1
                for i in range(gw * gh): rows[len(ids)] = list(E[i]); ids.append(0); pos.append((cur, cur + i // gw, cur + i % gw))
                cur += max(gw, gh)
                ids.append(en); pos.append((cur,) * 3); cur += 1
            for t in post: ids.append(t); pos.append((cur,) * 3); cur += 1
            ref = check_graph.qwen(kv, T, ids, 1, rows, pos)
            for mode, env in (("batch", {}), ("one by one", {"KATANA_QN_BATCH": "0"}), ("KV fp16", {"KATANA_CPU_KV16": "1"})):
                f = os.path.join(TMP, "l.bin")
                if os.path.exists(f): os.remove(f)
                r = run("run", m, "the hello and other\n", "1", "0", *(["--image", ip] * ns), env=dict({"KATANA_CPU": "1", "KATANA_DUMP_LOGITS": f, "KATANA_SPEC": "", "KATANA_IMAGE_TOKENS": "64", "KATANA_MMPROJ": pj}, **env))
                g = struct.unpack("<%df" % len(ref), open(f, "rb").read()) if os.path.exists(f) else [9e9] * len(ref)
                d = max(abs(x - y) for x, y in zip(g, ref)); mx = max(abs(x) for x in ref)
                ok(d < (3e-2 if "fp16" in mode else 1e-4) * mx, "language model, %d image(s) of %d tokens, %s %s: largest difference %.1e of %.1f %s" % (ns, gw * gh, " ".join(extra), mode, d, mx, r.stderr.strip()[-200:] if d > 1e8 else ""))


only = sys.argv[2:]
for n, f in (("decoders", decoders), ("resizing", resizing), ("vit", vit), ("language", language)):
    if not only or n in only: f()
print("FAILED %d" % bad if bad else "OK")
sys.exit(1 if bad else 0)
