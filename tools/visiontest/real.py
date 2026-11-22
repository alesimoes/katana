#!/usr/bin/env python3
"""Real weights: describes generated images and checks expected facts; compares CPU and GPU logits.

  python3 tools/visiontest/real.py ./katana MODEL.gguf [facts] [logits] [--keep DIR] [--only name,name]
The projector must be next to the model (or KATANA_MMPROJ). Needs Pillow and a font (DejaVu). One run takes the GPU:
do not start it while somebody else is using the machine.
"""
import os, re, subprocess, sys, tempfile, time
from PIL import Image, ImageDraw, ImageFont
K, M = os.path.abspath(sys.argv[1]), sys.argv[2]
OPT = sys.argv[3:]
D = OPT[OPT.index("--keep") + 1] if "--keep" in OPT else tempfile.mkdtemp()
os.makedirs(D, exist_ok=True)
ONLY = OPT[OPT.index("--only") + 1].split(",") if "--only" in OPT else None


def font(n, bold=False, mono=False):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans%s%s.ttf" % ("Mono" if mono else "", "-Bold" if bold else ""),):
        if os.path.exists(p): return ImageFont.truetype(p, n)
    return ImageFont.load_default(n)


def new(w, h, c="white"):
    im = Image.new("RGB", (w, h), c); return im, ImageDraw.Draw(im)


def shapes():
    im, d = new(640, 480)
    d.ellipse([40, 60, 240, 260], fill=(220, 20, 20)); d.rectangle([360, 60, 600, 240], fill=(20, 60, 220)); d.polygon([(320, 280), (200, 450), (440, 450)], fill=(20, 160, 40))
    return im


def english():
    im, d = new(800, 300)
    d.text((40, 60), "Invoice number 48213", fill="black", font=font(54, True)); d.text((40, 160), "Total due: $1,275.50 by March 9", fill="black", font=font(40))
    return im


def portuguese():
    im, d = new(800, 300, (250, 245, 220))
    d.text((40, 60), "Atenção: reunião às 14h30", fill=(120, 0, 0), font=font(50, True)); d.text((40, 160), "Sala 7, trazer o relatório de vendas", fill="black", font=font(38))
    return im


def chart():
    im, d = new(900, 600)
    d.text((280, 20), "Sales by quarter 2025", fill="black", font=font(34, True))
    for i, (q, v) in enumerate((("Q1", 120), ("Q2", 310), ("Q3", 220), ("Q4", 90))):
        x = 140 + i * 180; d.rectangle([x, 520 - v * 1.3, x + 110, 520], fill=(40, 110, 190)); d.text((x + 25, 528), q, fill="black", font=font(30)); d.text((x + 25, 520 - v * 1.3 - 36), str(v), fill="black", font=font(28))
    d.line([100, 520, 860, 520], fill="black", width=3)
    return im


def screenshot():
    im, d = new(1920, 1080, (30, 32, 38))
    d.rectangle([0, 0, 1920, 44], fill=(52, 56, 66)); d.text((20, 8), "Settings - Editor", fill="white", font=font(22))
    d.rectangle([0, 44, 320, 1080], fill=(40, 43, 52))
    for i, t in enumerate(("General", "Appearance", "Network", "Accounts", "Updates")): d.text((40, 90 + 60 * i), t, fill=(255, 255, 255) if t != "Network" else (255, 190, 60), font=font(26))
    d.text((380, 90), "Network", fill="white", font=font(44, True)); d.text((380, 190), "Proxy address: 10.0.4.17", fill=(220, 220, 220), font=font(28)); d.text((380, 250), "Port: 8443", fill=(220, 220, 220), font=font(28))
    d.rectangle([380, 340, 560, 400], fill=(200, 60, 60)); d.text((418, 352), "Disconnect", fill="white", font=font(22)); d.rectangle([600, 340, 780, 400], fill=(60, 140, 70)); d.text((655, 352), "Save", fill="white", font=font(22))
    d.text((380, 480), "error: connection refused (code 111)", fill=(255, 110, 110), font=font(26, mono=True))
    return im


CASES = [("shapes", shapes, "Which shapes are in the image, what colour is each one and where is it? Answer in one short paragraph.", [r"red", r"circle", r"blue", r"rectangle|square", r"green", r"triangle"]),
         ("english", english, "Read all the text in the image.", [r"48213", r"1,?275\.50", r"March 9"]),
         ("portuguese", portuguese, "Leia todo o texto da imagem e responda em português.", [r"reuni[aã]o", r"14h30", r"[Ss]ala 7", r"relat[oó]rio"]),
         ("chart", chart, "What does this chart show? Say the title, the value of each bar and which quarter is the highest.", [r"2025", r"120", r"310", r"220", r"90", r"(Q2[^.]*(highest|largest|peak|most))|((highest|largest|peak|most)[^.]*Q2)"]),
         ("screenshot", screenshot, "This is a screenshot. Which settings page is open, what are the proxy address and the port, which buttons are there with their colours, and what is the error message?", [r"Network", r"10\.0\.4\.17", r"8443", r"Disconnect", r"Save", r"red", r"green", r"refused", r"111"])]
for n, f, _, _ in CASES: f().save("%s/%s.png" % (D, n))


def run(q, img, n, env=None, extra=()):
    t = time.time()
    r = subprocess.run([K, "chat", M, q, str(n), "0", "--image", img, *extra], capture_output=True, text=True, errors="replace", env=dict(os.environ, KATANA_THINK="0", **(env or {})))
    return r.stdout, r.stderr, time.time() - t


bad = 0
if not OPT or "facts" in OPT:
    for n, _, q, want in CASES:
        if ONLY and n not in ONLY: continue
        out, err, dt = run(q, "%s/%s.png" % (D, n), 260)
        miss = [w for w in want if not re.search(w, out, re.I)]
        bad += bool(miss)
        print("%-4s %s: %d of %d facts, %.0f s%s\n     %s" % ("FAIL" if miss else "ok", n, len(want) - len(miss), len(want), dt, " missing " + str(miss) if miss else "", " ".join(out.split())[:700]))
        for l in err.splitlines():
            if "[vision" in l or l.startswith("[prompt") or l.startswith("[image"): print("     " + l)
        if "[vision" not in err: print(err[-1500:])
        sys.stdout.flush()
if "logits" in OPT:
    import numpy as np
    img = D + "/shapes.png"; L = {}
    for nm, e in (("CPU", {"KATANA_CPU": "1"}), ("GPU, attention on the CPU", {"KATANA_QN_CPUATT": "1"}), ("GPU, attention on the GPU", {"KATANA_QN_CPUATT": "0"})):
        f = D + "/l.bin"
        if os.path.exists(f): os.remove(f)
        _, err, dt = run("What is in the image?", img, 1, dict(e, KATANA_DUMP_LOGITS=f, KATANA_CTX="2048"))
        if not os.path.exists(f): print("FAIL", nm, err[-800:]); bad += 1; continue
        L[nm] = np.frombuffer(open(f, "rb").read(), np.float32).astype(float)
        c = L["CPU"]; g = L[nm]
        cos = (c @ g) / np.linalg.norm(c) / np.linalg.norm(g)
        p = lambda x: np.exp(x - x.max()) / np.exp(x - x.max()).sum()
        kl = float((p(c) * (np.log(p(c) + 1e-12) - np.log(p(g) + 1e-12))).sum())
        print("     %s: %.0f s, best token %d, cosine with the CPU %.6f, KL %.2e, largest difference %.2f" % (nm, dt, g.argmax(), cos, kl, np.abs(c - g).max()))
        bad += cos < 0.999 or g.argmax() != c.argmax()
print("FAILED %d" % bad if bad else "OK")
sys.exit(1 if bad else 0)
