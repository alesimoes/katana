#!/usr/bin/env python3
# Gera o icone do katana-desktop: squircle roxo em degrade com um K grosso em pixel art (grade 32x32),
# degrade branco->lilas e sombra roxa escura de 1 pixel. Saidas em ../icons: katana-<tam>.png e katana.svg.
import os
from PIL import Image, ImageDraw

AQUI = os.path.dirname(os.path.abspath(__file__))
ICONS = os.path.join(AQUI, "..", "icons")
GRADE, W, H = 32, 12, 14


def hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def hx(c):
    return "#%02x%02x%02x" % c


def letra_k():
    g = [[0] * W for _ in range(H)]
    for y in range(H):
        for x in range(3):
            g[y][x] = 1
        d = abs(y + 0.5 - H / 2)
        x_in = 3 + int(round(d * 0.95)) - 1
        for x in range(max(3, x_in), min(W, x_in + 3)):
            g[y][x] = 1
    return g


K = letra_k()
TOPO, BASE = hexrgb("#9a1fe8"), hexrgb("#6a07b0")
F1, F2, SOMBRA = hexrgb("#ffffff"), hexrgb("#d9a8ff"), hexrgb("#2d0a4e")
OX, OY = (GRADE - W) // 2, (GRADE - H) // 2


def pixels():
    # (x, y, cor) na grade; sombra primeiro, depois a face
    for y in range(H):
        for x in range(W):
            if K[y][x]:
                yield (OX + x + 1, OY + y + 1, SOMBRA)
    for y in range(H):
        for x in range(W):
            if K[y][x]:
                yield (OX + x, OY + y, lerp(F1, F2, y / H))


def render(n=512):
    s = n // GRADE
    img = Image.new("RGB", (n, n))
    d = ImageDraw.Draw(img)
    for y in range(n):
        d.line([(0, y), (n, y)], fill=lerp(TOPO, BASE, y / n))
    for x, y, c in pixels():
        d.rectangle([x * s, y * s, (x + 1) * s - 1, (y + 1) * s - 1], fill=c)
    m = Image.new("L", (n, n), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, n - 1, n - 1], n // 5, fill=255)
    out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    out.paste(img, (0, 0), m)
    return out


def main():
    big = render(512)
    for t in (16, 32, 48, 64, 128, 256, 512):
        (big if t == 512 else big.resize((t, t), Image.LANCZOS)).save(os.path.join(ICONS, "katana-%d.png" % t))
    s = 512 // GRADE
    r = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512" shape-rendering="crispEdges">',
         '  <!-- gerado por tools/gerar-icone-k.py: squircle roxo em degrade com um K em pixel art -->',
         '  <defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient></defs>' % (hx(TOPO), hx(BASE)),
         '  <rect width="512" height="512" rx="102" fill="url(#g)"/>']
    for x, y, c in pixels():
        r.append('  <rect x="%d" y="%d" width="%d" height="%d" fill="%s"/>' % (x * s, y * s, s, s, hx(c)))
    r.append("</svg>")
    open(os.path.join(ICONS, "katana.svg"), "w").write("\n".join(r) + "\n")


main()
