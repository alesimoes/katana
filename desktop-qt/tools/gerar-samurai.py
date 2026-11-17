#!/usr/bin/env python3
# Gera o mascote (samurai em pixel art 16x16), o titulo retro "KATANA" e os icones do katana-desktop.
# Uso: python3 desktop-qt/tools/gerar-samurai.py     (precisa de PIL; sem outras dependencias)
# Saidas em desktop-qt/icons/: samurai.png (16x16 logico; o QML amplia em escala inteira sem
# suavizacao), titulo-claro.png / titulo-escuro.png, katana.svg e katana-<tam>.png.
# SAMURAI_PREVIEW=arquivo.png grava uma previa.
# Paleta do personagem: 5 tons (contorno, sombra, base roxa do tema, luz, branco).
import os
from PIL import Image, ImageDraw

AQUI = os.path.dirname(os.path.abspath(__file__))
ICONS = os.path.join(AQUI, "..", "icons")

# "." transparente; O contorno; S sombra; M base; H luz; W branco
SPRITE = [
    "..H..........H..",
    "..HH........HH..",
    "...HH..MM..HH...",
    "....HMMMMMMH..H.",
    "...OMMMMMMMMO.H.",
    "...OOOOOOOOOO.H.",
    "...OWWOOOOWWO.H.",
    "...OWOOOOOOWO.H.",
    "...OOOOOOOOOO.H.",
    "....OMMMMMMO..H.",
    "..OOMMHHMMMOO.H.",
    "..OMOMMHMMMOMHHH",
    "..OMOMMMMMMOMOSO",
    "...OOSSSSSSOOOO.",
    "....OSOOOOSO....",
    "....OOO..OOO....",
]
PAL = {"O": "#26093f", "S": "#5b0f9a", "M": "#820ad1", "H": "#b66cf2", "W": "#ffffff"}
# no icone (fundo roxo) o personagem usa tons claros
PAL_ICO = {"O": "#2a0a47", "S": "#8f4fc9", "M": "#d9b3f7", "H": "#f1e3fc", "W": "#ffffff"}


def hexrgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5)) + (255,)


def sprite_img(pal):
    h, w = len(SPRITE), len(SPRITE[0])
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for y, row in enumerate(SPRITE):
        assert len(row) == w
        for x, ch in enumerate(row):
            if ch != ".":
                im.putpixel((x, y), hexrgb(pal[ch]))
    return im


# fonte 5x7 (so as letras de KATANA)
FONT = {
    "K": ["X...X", "X..X.", "X.X..", "XX...", "X.X..", "X..X.", "X...X"],
    "A": [".XXX.", "X...X", "X...X", "XXXXX", "X...X", "X...X", "X...X"],
    "T": ["XXXXX", "..X..", "..X..", "..X..", "..X..", "..X..", "..X.."],
    "N": ["X...X", "XX..X", "X.X.X", "X..XX", "X...X", "X...X", "X...X"],
}


def titulo(texto, topo, base, sombra):
    # letras blocadas 5x7, degrade vertical (topo -> base) e sombra de 1px na diagonal
    gap = 1
    w = len(texto) * 5 + (len(texto) - 1) * gap + 1
    im = Image.new("RGBA", (w, 8), (0, 0, 0, 0))
    t, b = hexrgb(topo), hexrgb(base)
    for passo in (0, 1):
        x0 = 0
        for ch in texto:
            for y, row in enumerate(FONT[ch]):
                k = y / 6
                col = hexrgb(sombra) if passo == 0 else \
                    tuple(round(t[i] + (b[i] - t[i]) * k) for i in range(3)) + (255,)
                for x, c in enumerate(row):
                    if c == "X":
                        im.putpixel((x0 + x + (1 - passo), y + (1 - passo)), col)
            x0 += 5 + gap
    return im


def icone512(pal):
    big = 512
    im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(im).rounded_rectangle((0, 0, big - 1, big - 1), 112, fill=hexrgb("#820ad1"))
    s = sprite_img(pal)
    k = 24
    sp = s.resize((s.width * k, s.height * k), Image.NEAREST)
    im.alpha_composite(sp, ((big - sp.width) // 2, (big - sp.height) // 2 + 4))
    return im


def main():
    s = sprite_img(PAL)
    s.save(os.path.join(ICONS, "samurai.png"))
    tc = titulo("KATANA", "#a24fe8", "#5b0f9a", "#d8c2ef")
    te = titulo("KATANA", "#e2c2fb", "#a35be0", "#3a1560")
    tc.save(os.path.join(ICONS, "titulo-claro.png"))
    te.save(os.path.join(ICONS, "titulo-escuro.png"))
    big = icone512(PAL_ICO)
    for t in (16, 32, 48, 64, 128, 256, 512):
        (big if t == 512 else big.resize((t, t), Image.LANCZOS)).save(os.path.join(ICONS, "katana-%d.png" % t))
    # svg equivalente (retangulos sem suavizacao)
    k, ox, oy = 24, (512 - 16 * 24) // 2, (512 - 16 * 24) // 2 + 4
    r = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512" shape-rendering="crispEdges">',
         '  <!-- gerado por tools/gerar-samurai.py: squircle roxo com o samurai em pixel art -->',
         '  <rect width="512" height="512" rx="112" fill="#820ad1"/>']
    for y, row in enumerate(SPRITE):
        for x, ch in enumerate(row):
            if ch != ".":
                r.append('  <rect x="%d" y="%d" width="%d" height="%d" fill="%s"/>' % (ox + x * k, oy + y * k, k, k, PAL_ICO[ch]))
    r.append("</svg>")
    open(os.path.join(ICONS, "katana.svg"), "w").write("\n".join(r) + "\n")
    p = os.environ.get("SAMURAI_PREVIEW")
    if p:
        pv = Image.new("RGBA", (640, 300), hexrgb("#faf9fc"))
        pv.alpha_composite(s.resize((256, 256), Image.NEAREST), (10, 10))
        pv.alpha_composite(big.resize((128, 128), Image.LANCZOS), (290, 10))
        pv.alpha_composite(tc.resize((tc.width * 4, tc.height * 4), Image.NEAREST), (290, 160))
        pv.save(p)


main()
