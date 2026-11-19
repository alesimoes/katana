#!/usr/bin/env python3
# Gera o samurai do katana em pixel art 32x32 (desktop-qt/icons/samurai.png e site/public/samurai.png).
# Uso: python3 desktop-qt/tools/gerar-samurai32.py [previa.png]     (precisa de PIL)
# O personagem e desenhado com formas simples (retangulos, elipses, linhas) numa grade 32x32, e o contorno
# escuro sai automatico: todo pixel vazio vizinho de um pixel pintado vira contorno.
import os
import sys
from PIL import Image

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.abspath(os.path.join(AQUI, "..", ".."))
N = 32

PAL = {
    "O": "#1a0a2e",   # contorno
    "D": "#3a0f66",   # roxo escuro (sombra)
    "M": "#7a0cc8",   # roxo base
    "L": "#b66cf2",   # roxo claro (luz)
    "S": "#f3cfa8",   # pele
    "E": "#c98f68",   # pele sombra
    "R": "#e0245e",   # vermelho (cordas, faixa)
    "Y": "#f2c14e",   # ouro
    "G": "#aaa3bd",   # aco
    "W": "#ffffff",   # brilho
    "K": "#241636",   # armadura escura
}

g = [["."] * N for _ in range(N)]


def px(x, y, c):
    if 0 <= x < N and 0 <= y < N:
        g[y][x] = c


def rect(x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px(x, y, c)


def ell(cx, cy, rx, ry, c):
    for y in range(N):
        for x in range(N):
            if ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0:
                px(x, y, c)


def line(x0, y0, x1, y1, c):
    n = max(abs(x1 - x0), abs(y1 - y0))
    for i in range(n + 1):
        t = i / n if n else 0
        px(round(x0 + (x1 - x0) * t), round(y0 + (y1 - y0) * t), c)


def sym(x):  # espelha em torno do eixo entre as colunas 15 e 16
    return N - 1 - x


# ---- katana (empunhada na mao direita do desenho, lamina sobe na diagonal sem sair da grade) ----
line(26, 22, 29, 3, "G")
line(27, 22, 30, 3, "W")
px(29, 2, "G"); px(30, 2, "W"); px(30, 1, "W")           # ponta
rect(24, 22, 28, 23, "Y")                                  # tsuba
line(24, 28, 25, 24, "R"); line(25, 28, 26, 24, "K")       # cabo enrolado

# ---- pernas e saia (kusazuri) ----
rect(10, 27, 21, 29, "D")
for x in (10, 13, 16, 19):
    rect(x, 27, x + 2, 29, "M")
    px(x, 27, "L"); px(x + 1, 27, "L")
    rect(x + 2, 27, x + 2, 29, "D")
rect(11, 30, 14, 31, "K"); rect(17, 30, 20, 31, "K")   # pes
rect(10, 30, 14, 30, "D"); rect(17, 30, 21, 30, "D")

# ---- tronco (do) ----
rect(10, 17, 21, 26, "M")
rect(10, 17, 21, 17, "L")
for y in (19, 21, 23, 25):          # placas horizontais
    rect(10, y, 21, y, "D")
    rect(10, y + 1, 21, y + 1, "L") if y + 1 <= 26 else None
rect(15, 17, 16, 26, "R")           # cordao central
rect(11, 22, 20, 22, "R")           # faixa (obi)
px(15, 22, "Y"); px(16, 22, "Y")

# ---- ombreiras (sode) ----
for (x0, x1) in ((5, 9), (22, 26)):
    rect(x0, 17, x1, 21, "M")
    rect(x0, 17, x1, 17, "L")
    rect(x0, 19, x1, 19, "D")
    rect(x0, 21, x1, 21, "D")
# bracos
rect(6, 22, 9, 26, "K"); rect(22, 22, 25, 26, "K")
rect(6, 27, 9, 27, "S"); rect(22, 27, 25, 27, "S")      # maos

# ---- cabeca ----
rect(11, 12, 20, 17, "S")                               # rosto
rect(11, 16, 20, 17, "K")                               # mascara (menpo) cobre a boca
rect(12, 16, 19, 16, "D")
px(13, 17, "W"); px(15, 17, "W"); px(16, 17, "W"); px(18, 17, "W")   # dentes da mascara
rect(11, 12, 20, 12, "E")                               # sombra da viseira
rect(12, 13, 14, 13, "K"); rect(17, 13, 19, 13, "K")   # sobrancelhas / fenda dos olhos
px(13, 14, "W"); px(18, 14, "W")                        # olhos
px(14, 14, "K"); px(17, 14, "K")

# ---- elmo (kabuto) ----
ell(15.5, 8.5, 7.2, 5.2, "M")
rect(8, 11, 23, 11, "D")                                # aba
rect(9, 10, 22, 10, "M")
for x in range(9, 23):
    px(x, 4 + (1 if x in (9, 22) else 0), "L") if x in (12, 13, 14, 15, 16, 17, 18, 19) else None
rect(12, 6, 19, 6, "L")
rect(11, 8, 20, 8, "D")                                 # friso
px(15, 9, "Y"); px(16, 9, "Y"); px(15, 10, "Y"); px(16, 10, "Y")  # emblema dourado
# protecao do pescoco (shikoro): abas laterais
rect(7, 12, 10, 15, "M"); rect(21, 12, 24, 15, "M")
rect(7, 14, 10, 14, "D"); rect(21, 14, 24, 14, "D")
rect(7, 12, 10, 12, "L"); rect(21, 12, 24, 12, "L")
# chifres dourados (maedate)
for (x, y) in ((11, 4), (10, 3), (9, 2), (8, 1), (8, 0)):
    px(x, y, "Y"); px(sym(x), y, "Y")
for (x, y) in ((12, 4), (11, 3), (10, 2), (9, 1)):
    px(x, y, "Y"); px(sym(x), y, "Y")

# ---- contorno automatico ----
out = [row[:] for row in g]
for y in range(N):
    for x in range(N):
        if g[y][x] == ".":
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < N and 0 <= yy < N and g[yy][xx] != ".":
                    out[y][x] = "O"
                    break

im = Image.new("RGBA", (N, N), (0, 0, 0, 0))
for y in range(N):
    for x in range(N):
        c = out[y][x]
        if c != ".":
            h = PAL[c]
            im.putpixel((x, y), tuple(int(h[i:i + 2], 16) for i in (1, 3, 5)) + (255,))

if __name__ == "__main__":
    prev = sys.argv[1] if len(sys.argv) > 1 else None
    if prev:
        bg = Image.new("RGBA", (N * 12 * 2 + 30, N * 12 + 20), (250, 249, 245, 255))
        big = im.resize((N * 12, N * 12), Image.NEAREST)
        bg.alpha_composite(big, (10, 10))
        dk = Image.new("RGBA", (N * 12, N * 12), (12, 9, 18, 255))
        dk.alpha_composite(big)
        bg.paste(dk, (N * 12 + 20, 10))
        bg.save(prev)
    else:
        im.save(os.path.join(RAIZ, "desktop-qt", "icons", "samurai.png"))
        im.save(os.path.join(RAIZ, "site", "public", "samurai.png"))
        print("samurai.png 32x32 gravado")
