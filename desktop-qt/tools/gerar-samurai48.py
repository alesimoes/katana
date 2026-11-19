#!/usr/bin/env python3
# Gera o samurai do katana em pixel art 48x48, em blocos (desktop-qt/icons/samurai.png e site/public/samurai.png).
# Uso: python3 desktop-qt/tools/gerar-samurai48.py [previa.png]     (precisa de PIL)
# Estilo "voxel": cabeca cubica, tronco, bracos e pernas retangulares, katana reta. Cada peca e um bloco com luz
# em cima e na esquerda e sombra embaixo e na direita. E o mesmo samurai de antes: coque, faixa branca com o
# circulo vermelho, bigode, quimono azul de gola branca, faixa (obi) roxa, hakama preto, katana erguida e outra
# na cintura. O roxo do projeto so aparece na faixa. Contorno escuro automatico em volta da silhueta.
import os
import sys
from PIL import Image

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.abspath(os.path.join(AQUI, "..", ".."))
N = 48

C = {
    "O": "#140b1f",   # contorno
    "H": "#1d1b26", "h": "#34324a", "j": "#0f0e15",       # cabelo: base, luz, sombra
    "S": "#f2caa2", "s": "#fbe0c4", "e": "#cf9468",       # pele: base, luz, sombra
    "W": "#f4f1ea", "w": "#c9c5bc",                       # branco, branco sombra
    "N": "#2a3560", "n": "#43518a", "m": "#1b2347",       # quimono: base, luz, sombra
    "K": "#23232d", "k": "#3d3f52", "q": "#16161d",       # hakama
    "R": "#d4302f", "r": "#a31f1f",                       # vermelho
    "P": "#8a1fe0", "p": "#b36cf0", "u": "#5e0fa3",       # roxo do katana (obi)
    "Y": "#f2c14e", "Z": "#b8891f",                       # ouro
    "G": "#aab2c4", "g": "#7d869b",                       # aco
    "B": "#6b4a2f", "b": "#4a3220",                       # sandalia
}

g = [[None] * N for _ in range(N)]


def px(x, y, k):
    if 0 <= x < N and 0 <= y < N:
        g[y][x] = k


def fill(x0, y0, x1, y1, k):
    """retangulo [x0,x1) x [y0,y1)"""
    for y in range(y0, y1):
        for x in range(x0, x1):
            px(x, y, k)


def box(x0, y0, x1, y1, base, light, dark):
    """bloco com luz em cima/esquerda e sombra embaixo/direita"""
    fill(x0, y0, x1, y1, base)
    fill(x0, y0, x1, y0 + 1, light)
    fill(x0, y0, x0 + 1, y1, light)
    fill(x0, y1 - 1, x1, y1, dark)
    fill(x1 - 1, y0, x1, y1, dark)
    px(x0, y1 - 1, base)
    px(x1 - 1, y0, base)


# ---------------- katana na cintura: bainha horizontal preta, cabo vermelho, ponteira dourada ----------------
fill(6, 33, 21, 35, "K"); fill(6, 33, 21, 34, "k")
fill(5, 33, 7, 35, "Y"); px(5, 33, "Z")
fill(20, 31, 25, 33, "R"); fill(20, 32, 25, 33, "r")                  # cabo saindo da faixa
fill(19, 32, 21, 34, "Y")                                              # guarda

# ---------------- pernas (hakama) e pes ----------------
box(17, 35, 24, 45, "K", "k", "q")
box(24, 35, 31, 45, "K", "k", "q")
fill(24, 36, 25, 45, "q")                                              # vao entre as pernas
box(16, 45, 24, 47, "W", "W", "w")
box(24, 45, 32, 47, "W", "W", "w")
fill(16, 47, 24, 48, "B"); fill(24, 47, 32, 48, "B")
fill(16, 47, 24, 48, "B"); px(23, 47, "b"); px(31, 47, "b")

# ---------------- tronco: quimono ----------------
box(16, 21, 32, 35, "N", "n", "m")
# gola de quimono em V (escada): bordas brancas com o peito de pele no meio
for xa, xb, y in ((20, 28, 21), (21, 27, 23), (22, 26, 25), (23, 25, 27)):
    fill(xa, y, xb, y + 2, "W")
for xa, xb, y in ((22, 26, 21), (23, 25, 23)):
    fill(xa, y, xb, y + 2, "e")
# obi (faixa roxa) com no
box(16, 31, 32, 34, "P", "p", "u")
box(22, 30, 27, 36, "P", "p", "u")

# ---------------- katana erguida, reta: cabo, guarda, lamina (desenhada antes dos bracos: a mao cobre o cabo) ----------------
fill(37, 29, 40, 37, "K"); fill(37, 30, 38, 37, "k")                   # cabo
fill(37, 31, 40, 32, "w"); fill(37, 34, 40, 35, "w")                   # trama do cabo
box(34, 27, 43, 29, "Y", "Y", "Z")                                     # guarda (tsuba)
fill(37, 3, 40, 27, "G"); fill(39, 3, 40, 27, "g"); fill(37, 3, 38, 27, "W")  # lamina
fill(37, 2, 39, 3, "W"); fill(38, 1, 39, 2, "W")                       # ponta

# ---------------- bracos ----------------
box(11, 21, 16, 33, "N", "n", "m")                                     # braco esquerdo (manga)
box(11, 33, 16, 36, "S", "s", "e")                                     # mao
box(32, 21, 37, 33, "N", "n", "m")                                     # braco direito (manga)
box(33, 33, 40, 36, "S", "s", "e")                                     # mao fechada no cabo

# ---------------- cabeca: cubo ----------------
fill(20, 20, 28, 22, "e")                                              # pescoco
box(17, 7, 31, 21, "S", "s", "e")                                      # rosto
fill(17, 7, 31, 12, "H"); fill(17, 7, 31, 8, "h"); fill(17, 7, 18, 12, "h"); fill(30, 8, 31, 12, "j")   # cabelo
fill(17, 12, 19, 15, "H"); fill(29, 12, 31, 15, "H")                  # costeletas
fill(21, 3, 27, 7, "H"); fill(21, 3, 27, 4, "h"); fill(26, 4, 27, 7, "j")     # coque
fill(21, 5, 27, 6, "R")                                                # fita do coque
# faixa branca na testa (hachimaki) com o circulo vermelho e a ponta que balanca
fill(17, 10, 31, 13, "W"); fill(17, 12, 31, 13, "w")
fill(23, 10, 25, 13, "R")
fill(31, 11, 35, 13, "W"); fill(33, 13, 35, 15, "w")
# sobrancelhas e olhos
fill(19, 14, 23, 15, "H"); fill(25, 14, 29, 15, "H")
fill(19, 15, 23, 17, "W"); fill(25, 15, 29, 17, "W")
fill(21, 15, 23, 17, "O"); fill(27, 15, 29, 17, "O")
# nariz, bigode, boca
fill(23, 17, 25, 18, "e")
fill(20, 18, 28, 20, "H")
fill(22, 20, 26, 21, "r")

# ---------------- contorno automatico (4-vizinhos) ----------------
img = Image.new("RGBA", (N, N), (0, 0, 0, 0))


def rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5)) + (255,)


for y in range(N):
    for x in range(N):
        if g[y][x]:
            img.putpixel((x, y), rgb(C[g[y][x]]))
out = img.copy()
for y in range(N):
    for x in range(N):
        if g[y][x] is None:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < N and 0 <= yy < N and g[yy][xx]:
                    out.putpixel((x, y), rgb(C["O"]))
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
