#!/usr/bin/env python3
# Gera icons/samurai.png (64x64, pixel art) para a tela de boas-vindas do katana-desktop.
# Uso: python3 desktop-qt/tools/gerar-samurai.py   (precisa de PIL)
# SAMURAI_PREVIEW=arquivo.png grava tambem uma previa ampliada 8x.
# O desenho e composto por primitivas sobre uma grade 64x64 com paleta fixa; o contorno escuro
# do samurai e gerado automaticamente em volta de todos os pixels da figura.
import os
from PIL import Image

N = 64
PAL = {
    "o": "#1a1220",  # contorno
    "moon": "#e9d3fb", "moon2": "#d2aef3",
    "star": "#ffffff",
    "ko": "#3b1d63", "ko2": "#5a2f93",  # kabuto / armadura
    "dk": "#2b1650",  # hakama
    "ki": "#6d35b8", "ki2": "#8a52d6",  # kimono
    "gold": "#f0c030", "gold2": "#b8860b",
    "skin": "#f2c9a0", "skin2": "#d9a679",
    "wh": "#f6efe8", "red": "#c8283c", "red2": "#8f1a2a",
    "bl": "#f2f6fb", "bl2": "#9fb2c8", "bl3": "#6f86a3",
    "ground": "#2b1650", "pet": "#f4a6d0",
}


def hexrgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5)) + (255,)


class Layer:
    def __init__(self):
        self.p = {}

    def px(self, x, y, c):
        if 0 <= x < N and 0 <= y < N:
            self.p[(x, y)] = c

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.px(x, y, c)

    def line(self, x0, y0, x1, y1, c, w=1):
        dx, dy = abs(x1 - x0), abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx - dy
        while True:
            for k in range(w):
                # espessura: engorda na horizontal se a linha e mais vertical, e vice-versa
                if dx < dy:
                    self.px(x0 + k, y0, c)
                else:
                    self.px(x0, y0 + k, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    def poly(self, pts, c):
        ys = [p[1] for p in pts]
        for y in range(min(ys), max(ys) + 1):
            xs = []
            for i in range(len(pts)):
                (xa, ya), (xb, yb) = pts[i], pts[(i + 1) % len(pts)]
                if ya == yb:
                    continue
                if min(ya, yb) <= y < max(ya, yb) or y == max(ya, yb) == max(ys):
                    xs.append(xa + (y - ya) * (xb - xa) / (yb - ya))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                for x in range(round(xs[i]), round(xs[i + 1]) + 1):
                    self.px(x, y, c)

    def disc(self, cx, cy, r, c):
        for y in range(N):
            for x in range(N):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    self.px(x, y, c)


moon = Layer()
moon.disc(32, 31, 27, "moon")
for (cx, cy, r) in ((17, 22, 4), (47, 40, 5), (44, 16, 3), (20, 44, 3)):
    for y in range(N):
        for x in range(N):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r and (x, y) in moon.p:
                moon.px(x, y, "moon2")
for (x, y), c in list(moon.p.items()):
    if (x - 32) ** 2 + (y - 31) ** 2 > 25 ** 2:
        moon.px(x, y, "moon2")

fig = Layer()
# bainha (saya) no quadril esquerdo do samurai (lado esquerdo da imagem)
fig.line(24, 37, 12, 54, "ko", 2)
fig.line(25, 37, 13, 54, "red2", 1)
fig.px(11, 55, "gold")
# pernas (hakama)
fig.poly([(23, 36), (38, 36), (42, 54), (19, 54)], "dk")
fig.line(30, 46, 30, 54, "ko", 1)
fig.line(26, 40, 24, 53, "ko", 1)
fig.line(35, 40, 38, 53, "ko", 1)
# pes
fig.rect(19, 54, 28, 56, "wh")
fig.rect(32, 54, 42, 56, "wh")
fig.rect(19, 56, 28, 56, "red2")
fig.rect(32, 56, 42, 56, "red2")
# tronco (kimono)
fig.rect(23, 23, 38, 37, "ki")
fig.rect(23, 23, 25, 37, "ki2")
fig.line(27, 23, 30, 30, "wh")
fig.line(34, 23, 31, 30, "wh")
fig.line(28, 23, 30, 28, "wh")
fig.line(33, 23, 31, 28, "wh")
# obi
fig.rect(22, 33, 39, 36, "gold")
fig.rect(22, 36, 39, 36, "gold2")
fig.rect(29, 33, 32, 38, "red")
fig.rect(29, 38, 32, 39, "red2")
# ombreiras (sode)
fig.rect(17, 23, 23, 31, "ko")
fig.rect(17, 24, 23, 24, "gold")
fig.rect(17, 27, 23, 27, "ko2")
fig.rect(17, 30, 23, 30, "gold2")
fig.rect(38, 23, 44, 29, "ko")
fig.rect(38, 24, 44, 24, "gold")
fig.rect(38, 27, 44, 27, "ko2")
# braco direito do samurai (lado direito da imagem), segurando a katana
fig.poly([(40, 28), (46, 30), (48, 36), (42, 37)], "ki2")
fig.rect(43, 33, 46, 36, "ki")
fig.rect(44, 31, 47, 34, "skin")
# cabo (tsuka)
fig.line(44, 38, 49, 31, "red", 2)
fig.line(45, 37, 48, 33, "red2", 1)
# guarda (tsuba)
fig.line(46, 30, 51, 33, "gold", 1)
fig.line(47, 29, 52, 32, "gold2", 1)
# lamina
fig.line(50, 29, 60, 4, "bl", 2)
fig.line(51, 29, 61, 5, "bl2", 1)
fig.line(49, 29, 59, 4, "bl3", 1)
fig.px(60, 3, "bl")
fig.px(61, 2, "bl")
# braco esquerdo na bainha
fig.poly([(18, 30), (23, 30), (24, 37), (19, 38)], "ki2")
fig.rect(21, 37, 24, 39, "skin")
# cabeca
fig.rect(26, 15, 35, 23, "skin")
fig.rect(26, 21, 35, 23, "skin2")
fig.rect(28, 22, 33, 23, "dk")
fig.rect(28, 18, 29, 18, "o")
fig.rect(32, 18, 33, 18, "o")
fig.rect(27, 17, 30, 17, "ko")
fig.rect(31, 17, 34, 17, "ko")
# pescoco protegido (shikoro)
fig.rect(23, 15, 26, 22, "ko")
fig.rect(35, 15, 38, 22, "ko")
fig.rect(23, 18, 26, 18, "ko2")
fig.rect(35, 18, 38, 18, "ko2")
fig.rect(23, 21, 26, 21, "gold2")
fig.rect(35, 21, 38, 21, "gold2")
# kabuto (capacete)
fig.poly([(25, 15), (36, 15), (35, 8), (31, 6), (28, 6), (26, 8)], "ko")
fig.rect(27, 8, 29, 10, "ko2")
fig.rect(22, 14, 39, 15, "ko")
fig.rect(22, 14, 39, 14, "gold")
fig.rect(29, 12, 32, 13, "gold")
# chifres (maedate)
fig.line(28, 8, 24, 1, "gold", 1)
fig.line(29, 8, 25, 1, "gold", 1)
fig.line(33, 8, 37, 1, "gold", 1)
fig.line(32, 8, 36, 1, "gold", 1)
fig.px(24, 1, "gold2")
fig.px(37, 1, "gold2")

# contorno automatico da figura
outline = {}
for (x, y) in fig.p:
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            q = (x + dx, y + dy)
            if q not in fig.p and 0 <= q[0] < N and 0 <= q[1] < N:
                outline[q] = "o"

img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
pix = img.load()


def put(layer):
    for (x, y), c in layer.items():
        pix[x, y] = hexrgb(PAL[c])


ground = Layer()
ground.rect(6, 57, 57, 59, "ground")
ground.rect(9, 60, 54, 60, "ground")
put(moon.p)
put(ground.p)
put(outline)
put(fig.p)
for (x, y) in ((6, 8), (58, 14), (10, 3), (53, 5), (3, 30), (61, 40), (14, 12), (49, 11)):
    if pix[x, y][3] == 0:
        pix[x, y] = hexrgb(PAL["star"])
for (x, y) in ((8, 36), (52, 48), (55, 24), (12, 48), (47, 56), (5, 18)):
    pix[x, y] = hexrgb(PAL["pet"])

dest = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "icons", "samurai.png")
img.save(dest)
prev = os.environ.get("SAMURAI_PREVIEW")
if prev:
    bg = Image.new("RGBA", (N * 8, N * 8), (34, 31, 38, 255))
    bg.alpha_composite(img.resize((N * 8, N * 8), Image.NEAREST))
    bg.save(prev)
print("gravado", os.path.normpath(dest))
