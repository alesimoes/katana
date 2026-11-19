#!/usr/bin/env python3
# Gera o samurai do katana em pixel art 48x48 (desktop-qt/icons/samurai.png e site/public/samurai.png).
# Uso: python3 desktop-qt/tools/gerar-samurai48.py [previa.png]     (precisa de PIL)
# Tecnica: o personagem e desenhado com poligonos e elipses (curvas de verdade) numa grade 8x maior, depois reduzido
# para 48x48 pegando a cor mais frequente de cada bloco (continua pixel art, com paleta fixa), e o contorno
# escuro sai automatico em volta da silhueta.
import math
import os
import sys
from collections import Counter
from PIL import Image, ImageDraw

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.abspath(os.path.join(AQUI, "..", ".."))
N = 48
S = 8

PAL = {
    "O": "#190a2c",   # contorno
    "K": "#2b1745",   # roupa escura
    "D": "#4a1285",   # roxo sombra
    "M": "#7d0fd0",   # roxo base
    "L": "#b36cf0",   # roxo luz
    "S": "#f4d2ad",   # pele
    "E": "#cf9468",   # pele sombra
    "R": "#e0245e",   # vermelho
    "Y": "#f5c542",   # ouro
    "Z": "#b8891f",   # ouro sombra
    "G": "#9f98b4",   # aco
    "W": "#ffffff",   # brilho
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
    d.line([(x0 * S, y0 * S), (x1 * S, y1 * S)], fill=rgb(PAL[c]), width=int(w * S))


def mirror(pts):
    return [(N - x, y) for x, y in pts]


def PM(pts, c):          # poligono e seu espelho
    P(pts, c)
    P(mirror(pts), c)


# ---------------- katana (empunhada na mao direita do desenho, lamina curva subindo) ----------------
blade = [(37.2, 27), (38.4, 24), (40.2, 18), (42.4, 11), (44.6, 4.2), (46.0, 1.2), (45.6, 5.5), (43.6, 13), (41.0, 21), (39.2, 26.5)]
P(blade, "G")
P([(37.8, 26.5), (39.0, 23), (41.0, 17), (43.2, 10), (45.2, 3.5), (45.9, 1.4), (45.0, 6), (42.8, 13), (40.4, 20), (38.6, 26)], "W")

# ---------------- pernas (hakama largo) ----------------
P([(14, 33), (34, 33), (37, 46.5), (25.2, 46.5), (24, 38), (22.8, 46.5), (11, 46.5)], "K")
P([(14, 33), (24, 33), (22.8, 46.5), (11, 46.5)], "D")
P([(24, 33), (34, 33), (37, 46.5), (25.2, 46.5)], "K")
L(16, 36, 13.5, 46, 0.5, "K")
L(31, 36, 33.5, 46, 0.5, "D")
E(17, 46.5, 5.5, 1.3, "O")
E(31, 46.5, 5.5, 1.3, "O")

# ---------------- bainha curta (wakizashi) no quadril esquerdo do desenho ----------------
L(14, 33.5, 7.5, 40, 2.2, "K")
L(14, 33.5, 11.5, 36, 1.1, "R")
E(14.3, 33.3, 1.3, 1.3, "Y")

# ---------------- tronco: armadura em placas ----------------
P([(16, 21), (32, 21), (33, 33.5), (15, 33.5)], "M")
P([(16, 21), (32, 21), (32.3, 24), (15.7, 24)], "L")
for y in (24.3, 27, 29.7):                    # camadas da couraca
    P([(15.6, y), (32.4, y), (32.6, y + 0.8), (15.4, y + 0.8)], "D")
    P([(15.4, y + 0.8), (32.6, y + 0.8), (32.8, y + 1.5), (15.2, y + 1.5)], "L")
P([(23.2, 21), (24.8, 21), (25.1, 33.5), (22.9, 33.5)], "R")     # cordao central
P([(14.8, 31.6), (33.2, 31.6), (33.4, 33.9), (14.6, 33.9)], "R") # obi
E(24, 32.7, 1.5, 1.3, "Y")

# ---------------- ombreiras (sode) em camadas, menores ----------------
for k, (c0, c1) in enumerate((("M", "L"), ("D", "M"), ("M", "L"))):
    y0 = 20.0 + k * 2.9
    P([(9.2 - k * 0.4, y0), (16.4, y0 - 0.3), (16.2, y0 + 3.1), (8.8 - k * 0.5, y0 + 2.9)], c0)
    P([(9.2 - k * 0.4, y0), (16.4, y0 - 0.3), (16.4, y0 + 0.8), (9.1 - k * 0.4, y0 + 0.9)], c1)
    P([(38.8 + k * 0.4, y0), (31.6, y0 - 0.3), (31.8, y0 + 3.1), (39.2 + k * 0.5, y0 + 2.9)], c0)
    P([(38.8 + k * 0.4, y0), (31.6, y0 - 0.3), (31.6, y0 + 0.8), (38.9 + k * 0.4, y0 + 0.9)], c1)

# ---------------- bracos ----------------
L(11, 27.5, 12.2, 32.4, 3.0, "K")                 # esquerdo (do desenho) segura a bainha
E(12.8, 33.4, 1.7, 1.7, "S")
L(37.5, 27, 38.2, 29.8, 3.0, "K")                 # direito: empunha a katana
E(38.3, 27.4, 2.0, 2.0, "S")

# ---------------- guarda e cabo da katana ----------------
P([(35.6, 26.2), (41.4, 25.2), (41.6, 26.6), (35.8, 27.7)], "Y")
P([(35.8, 27.2), (41.6, 26.2), (41.6, 26.9), (35.9, 27.9)], "Z")
L(38.6, 27.4, 38.0, 32.6, 1.7, "R")
L(38.9, 27.6, 38.3, 32.4, 0.5, "K")

# ---------------- cabeca (ordem: pescoco e rosto primeiro, elmo por cima) ----------------
# protetor de pescoco (shikoro): abas largas em camadas, atras do rosto
for k in range(3):
    y0 = 14.2 + k * 2.2
    PM([(16.0 - k * 1.2, y0), (24, y0), (24, y0 + 2.4), (14.6 - k * 1.7, y0 + 2.2)], "M" if k % 2 == 0 else "D")
    PM([(16.0 - k * 1.2, y0), (24, y0), (24, y0 + 0.7), (15.9 - k * 1.2, y0 + 0.8)], "L")
# rosto
E(24, 17.2, 6.5, 5.2, "S")
# mascara (menpo)
P([(17.9, 18.3), (30.1, 18.3), (28.6, 22.8), (24, 23.8), (19.4, 22.8)], "K")
P([(19.2, 19.3), (28.8, 19.3), (27.7, 21.2), (24, 22.0), (20.3, 21.2)], "D")
for x in (20.8, 22.5, 24.0, 25.5, 27.2):                                          # dentes
    P([(x - 0.35, 20.3), (x + 0.35, 20.3), (x + 0.3, 21.6), (x - 0.3, 21.6)], "W")
E(18.9, 19.0, 1.2, 1.0, "R"); E(29.1, 19.0, 1.2, 1.0, "R")                         # bochechas
# olhos furiosos
P([(18.4, 15.0), (22.4, 16.0), (22.4, 17.2), (18.6, 16.8)], "W")
P([(29.6, 15.0), (25.6, 16.0), (25.6, 17.2), (29.4, 16.8)], "W")
E(21.3, 16.5, 0.85, 0.85, "O"); E(26.7, 16.5, 0.85, 0.85, "O")
L(17.8, 14.4, 22.8, 15.9, 0.9, "K"); L(30.2, 14.4, 25.2, 15.9, 0.9, "K")          # sobrancelhas
# elmo (kabuto): cupula alta, aba (viseira) sobre a testa
E(24, 8.2, 9.4, 5.9, "M")
P([(15.2, 6.4), (32.8, 6.4), (30.6, 3.4), (17.4, 3.4)], "L")
P([(14.0, 12.2), (34.0, 12.2), (35.0, 14.4), (13.0, 14.4)], "D")                    # aba frontal
P([(14.0, 11.5), (34.0, 11.5), (34.1, 12.6), (13.9, 12.6)], "L")
for x in (18.2, 21.2, 24, 26.8, 29.8):                                               # rebites
    E(x, 9.6, 0.5, 0.5, "Z")
# chifres dourados curvos (maedate): arcos que sobem e abrem para fora
for sgn in (1, -1):
    prev = None
    for i in range(0, 15):
        t = i / 14
        x = 24 + sgn * (7.6 + 5.6 * t ** 0.85)
        y = 6.2 - 6.0 * t ** 0.9
        if prev:
            L(prev[0], prev[1], x, y, 1.9 - 0.9 * t, "Y")
        prev = (x, y)
E(24, 7.6, 1.7, 1.7, "Y")                                                            # emblema central
E(24, 7.6, 0.8, 0.8, "R")

# ---------------- reducao para pixel art ----------------
small = Image.new("RGBA", (N, N), (0, 0, 0, 0))
inv = {rgb(v): k for k, v in PAL.items()}
for y in range(N):
    for x in range(N):
        cnt = Counter()
        for yy in range(S):
            for xx in range(S):
                p = big.getpixel((x * S + xx, y * S + yy))
                cnt[p if p[3] else None] += 1
        best, n = cnt.most_common(1)[0]
        if best is None and n < S * S * 0.62:      # transparente so se for maioria clara
            cols = [(c, k) for c, k in cnt.most_common() if c is not None]
            best = cols[0][0] if cols else None
        if best is not None and (n >= S * S * 0.38 or best is not None):
            small.putpixel((x, y), best)

# alinha as cores exatamente a paleta (a reducao nao mistura, mas garante)
pal_rgb = list(inv.keys())
for y in range(N):
    for x in range(N):
        p = small.getpixel((x, y))
        if p[3]:
            best = min(pal_rgb, key=lambda q: sum((a - b) ** 2 for a, b in zip(q[:3], p[:3])))
            small.putpixel((x, y), best)

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
