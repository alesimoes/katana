#!/usr/bin/env python3
# Gera o samurai do katana em pixel art 48x48 (desktop-qt/icons/samurai.png e site/public/samurai.png).
# Uso: python3 desktop-qt/tools/gerar-samurai48.py [previa.png]     (precisa de PIL)
# Um samurai de verdade: cabelo preso no alto (chonmage), faixa branca na testa, rosto de pele com olhos
# estreitos, quimono escuro de gola branca cruzada, hakama largo, faixa (obi) roxa, katana erguida na mao
# direita e outra na cintura. O roxo do projeto so aparece na faixa e no cordao.
# Tecnica: poligonos e elipses numa grade 8x maior, reduzidos para 48x48 pela cor mais frequente de cada
# bloco (continua pixel art, com paleta fixa), e contorno escuro automatico em volta da silhueta.
import os
import sys
from collections import Counter
from PIL import Image, ImageDraw

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.abspath(os.path.join(AQUI, "..", ".."))
N = 48
S = 8

PAL = {
    "O": "#140b1f",   # contorno
    "H": "#1d1b26",   # cabelo
    "h": "#3a3750",   # brilho do cabelo
    "S": "#f2caa2",   # pele
    "E": "#cf9468",   # pele sombra
    "W": "#f4f1ea",   # branco (gola, faixa, tabi)
    "w": "#c9c5bc",   # branco sombra
    "N": "#2a3560",   # quimono azul-noite
    "n": "#43518a",   # quimono luz
    "K": "#23232d",   # hakama escuro
    "k": "#3d3f52",   # hakama luz
    "R": "#d4302f",   # vermelho
    "P": "#8a1fe0",   # roxo do katana (obi)
    "p": "#b36cf0",   # roxo luz
    "Y": "#f2c14e",   # ouro
    "Z": "#b8891f",   # ouro sombra
    "G": "#aab2c4",   # aco
    "B": "#6b4a2f",   # marrom (sandalia)
}


def rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5)) + (255,)


big = Image.new("RGBA", (N * S, N * S), (0, 0, 0, 0))
d = ImageDraw.Draw(big)


def P(pts, c):
    d.polygon([(x * S, y * S) for x, y in pts], fill=rgb(PAL[c]))


def E(cx, cy, rx, ry, c):
    d.ellipse([(cx - rx) * S, (cy - ry) * S, (cx + rx) * S, (cy + ry) * S], fill=rgb(PAL[c]))


def L(x0, y0, x1, y1, w, c):
    d.line([(x0 * S, y0 * S), (x1 * S, y1 * S)], fill=rgb(PAL[c]), width=max(1, int(w * S)))


# ---------------- katana erguida (mao direita do desenho): lamina diagonal, fio branco ----------------
P([(38.0, 23.6), (39.7, 23.0), (45.8, 2.4), (44.6, 2.0)], "G")
P([(38.8, 23.2), (39.7, 23.0), (45.8, 2.4), (45.2, 2.2)], "W")
P([(45.0, 2.2), (46.6, 0.8), (45.8, 2.6)], "W")                       # ponta
# ---------------- katana na cintura (bainha preta atras do corpo, aparece a esquerda) ----------------
L(19, 32.4, 8.2, 41.8, 2.1, "K")
L(19, 32.4, 8.2, 41.8, 0.5, "k")
E(8.0, 42.0, 1.1, 1.1, "Y")                                           # ponteira dourada
L(21, 31.4, 24.6, 30.2, 1.7, "R")                                     # cabo vermelho saindo da faixa
E(20.6, 31.8, 1.2, 1.2, "Y")                                          # guarda

# ---------------- pernas: hakama largo com pregas ----------------
P([(15, 32.5), (33, 32.5), (37.5, 45.4), (26.0, 45.4), (24, 38.6), (22.0, 45.4), (10.5, 45.4)], "K")
P([(15, 32.5), (24, 32.5), (22.0, 45.4), (10.5, 45.4)], "K")
L(17.2, 34.5, 14.6, 45, 0.55, "k"); L(20.6, 34.5, 19.0, 45, 0.55, "k")
L(27.4, 34.5, 29.0, 45, 0.55, "k"); L(30.8, 34.5, 33.4, 45, 0.55, "k")
# pes: tabi branco e sandalia
P([(11.0, 45.2), (21.4, 45.2), (21.4, 46.6), (10.2, 46.6)], "W")
P([(26.6, 45.2), (37.0, 45.2), (37.8, 46.6), (26.6, 46.6)], "W")
P([(10.2, 46.6), (21.6, 46.6), (21.6, 47.4), (10.0, 47.4)], "B")
P([(26.4, 46.6), (37.8, 46.6), (38.0, 47.4), (26.4, 47.4)], "B")

# ---------------- tronco: quimono azul-noite ----------------
P([(14.5, 20.2), (33.5, 20.2), (34.8, 33.0), (13.2, 33.0)], "N")
P([(14.5, 20.2), (24, 20.2), (24, 33.0), (13.2, 33.0)], "N")
L(29.5, 24, 31.8, 31.5, 0.6, "n"); L(16.5, 24, 15.5, 31.5, 0.6, "n")  # dobras
# gola branca cruzada (V) e gola vermelha por dentro
P([(20.2, 19.6), (27.8, 19.6), (24.0, 28.4)], "R")
P([(19.4, 19.4), (22.8, 19.4), (28.4, 31.4), (26.2, 31.4)], "W")
P([(28.6, 19.4), (25.2, 19.4), (19.6, 31.4), (21.8, 31.4)], "w")
# obi (faixa) roxa com no
P([(13.4, 30.6), (34.6, 30.6), (34.9, 33.8), (13.1, 33.8)], "P")
P([(13.4, 30.6), (34.6, 30.6), (34.7, 31.6), (13.3, 31.6)], "p")
P([(23.0, 30.4), (27.4, 30.4), (28.4, 34.6), (22.2, 34.6)], "P")

# ---------------- bracos e mangas (kimono largo) ----------------
# braco esquerdo (do desenho): manga larga pendente, mao no cabo da katana da cintura
P([(14.6, 20.4), (9.6, 22.0), (7.2, 31.0), (12.4, 31.6), (14.0, 25.0)], "N")
P([(9.6, 22.0), (7.2, 31.0), (8.6, 31.2), (10.6, 22.6)], "n")
E(20.6, 31.2, 1.7, 1.7, "S")
# braco direito: levantado, manga cai do cotovelo; punho no cabo da katana
P([(33.4, 20.4), (39.4, 22.0), (41.6, 29.6), (35.2, 28.6), (33.6, 24.0)], "N")
P([(39.4, 22.0), (41.6, 29.6), (40.2, 29.8), (38.4, 22.8)], "n")
L(37.4, 23.0, 36.8, 28.6, 1.7, "K")                                   # cabo preto
L(37.2, 24.6, 37.0, 27.6, 0.45, "w")                                  # trama do cabo
P([(35.8, 22.6), (40.4, 21.4), (40.6, 22.6), (36.0, 23.9)], "Y")      # guarda (tsuba)
E(37.7, 23.7, 1.8, 1.8, "S")                                          # mao

# ---------------- pescoco e cabeca ----------------
P([(21.8, 17.2), (26.2, 17.2), (26.6, 20.4), (21.4, 20.4)], "E")
# nuca/cabelo atras e coque (chonmage)
E(24, 8.4, 7.2, 5.4, "H")
P([(22.6, 3.9), (25.4, 3.9), (26.0, 1.2), (22.0, 1.2)], "H")          # coque
E(24, 1.6, 1.7, 1.2, "H")
L(22.2, 3.7, 25.8, 3.7, 0.7, "R")                                     # fita do coque
# rosto
E(24, 12.8, 6.2, 5.8, "S")
P([(17.9, 12.6), (30.1, 12.6), (28.6, 17.2), (24, 18.6), (19.4, 17.2)], "S")
P([(17.9, 9.4), (30.1, 9.4), (30.1, 12.4), (17.9, 12.4)], "S")
# cabelo frontal (testa)
P([(17.4, 11.0), (17.6, 7.6), (24, 5.2), (30.4, 7.6), (30.6, 11.0), (29.2, 8.4), (24, 7.0), (18.8, 8.4)], "H")
# faixa branca na testa (hachimaki) com circulo vermelho e pontas
P([(17.2, 8.6), (30.8, 8.6), (30.8, 10.6), (17.2, 10.6)], "W")
P([(17.2, 10.1), (30.8, 10.1), (30.8, 10.6), (17.2, 10.6)], "w")
E(24, 9.6, 1.0, 1.0, "R")
P([(30.6, 9.0), (34.6, 10.6), (33.8, 12.2), (30.6, 10.4)], "W")        # ponta da faixa (balanca)
P([(30.6, 10.2), (33.6, 13.4), (32.4, 13.6), (30.4, 11.0)], "w")
# sobrancelhas e olhos estreitos
L(18.6, 11.6, 22.4, 12.4, 0.95, "H"); L(29.4, 11.6, 25.6, 12.4, 0.95, "H")
P([(18.8, 12.9), (22.2, 13.2), (22.2, 14.2), (19.2, 14.3)], "W")
P([(29.2, 12.9), (25.8, 13.2), (25.8, 14.2), (28.8, 14.3)], "W")
E(21.0, 13.7, 0.8, 0.8, "O"); E(27.0, 13.7, 0.8, 0.8, "O")
# nariz, bigode e boca
L(24.0, 14.0, 23.7, 15.6, 0.55, "E")
P([(20.6, 16.0), (24, 16.5), (27.4, 16.0), (27.0, 16.9), (24, 16.9), (21.0, 16.9)], "H")   # bigode
L(22.6, 17.8, 25.4, 17.8, 0.5, "R")

# ---------------- reducao para pixel art ----------------
small = Image.new("RGBA", (N, N), (0, 0, 0, 0))
for y in range(N):
    for x in range(N):
        cnt = Counter()
        for yy in range(S):
            for xx in range(S):
                p = big.getpixel((x * S + xx, y * S + yy))
                cnt[p if p[3] else None] += 1
        best, n = cnt.most_common(1)[0]
        if best is None and n < S * S * 0.6:            # vazio so se for maioria clara
            cols = [c for c, k in cnt.most_common() if c is not None]
            best = cols[0] if cols else None
        if best is not None:
            small.putpixel((x, y), best)

pal_rgb = [rgb(v) for v in PAL.values()]
for y in range(N):
    for x in range(N):
        p = small.getpixel((x, y))
        if p[3]:
            small.putpixel((x, y), min(pal_rgb, key=lambda q: sum((a - b) ** 2 for a, b in zip(q[:3], p[:3]))))

# contorno automatico (4-vizinhos)
out = small.copy()
for y in range(N):
    for x in range(N):
        if small.getpixel((x, y))[3] == 0:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < N and 0 <= yy < N and small.getpixel((xx, yy))[3]:
                    out.putpixel((x, y), rgb(PAL["O"]))
                    break
im = out

if __name__ == "__main__":
    prev = sys.argv[1] if len(sys.argv) > 1 else None
    if prev:
        k = 10
        w = N * k
        bg = Image.new("RGBA", (w * 2 + 30, w + 20), (250, 249, 245, 255))
        bigp = im.resize((w, w), Image.NEAREST)
        bg.alpha_composite(bigp, (10, 10))
        dk = Image.new("RGBA", (w, w), (12, 9, 18, 255))
        dk.alpha_composite(bigp)
        bg.paste(dk, (w + 20, 10))
        bg.save(prev)
    else:
        im.save(os.path.join(RAIZ, "desktop-qt", "icons", "samurai.png"))
        im.save(os.path.join(RAIZ, "site", "public", "samurai.png"))
        print("samurai.png 48x48 gravado")
