/* ============================ render: logo composto na imagem gerada ===============================
 * katana render logo --image render.png --logo logo.png --box x,y,w,h [--perspective x1,y1,x2,y2,x3,y3,x4,y4] [--blend] [--out saida.png]
 * katana render logo selftest
 *
 * Nenhum modelo de imagem escreve uma marca com fidelidade; o logo de verdade (PNG com alfa) entra depois, por cima do render, numa conta
 * simples na CPU. Sem rede neural e sem Poisson:
 *   --box x,y,w,h   retangulo de destino; o logo cabe nele sem deformar (centrado). Quatro numeros entre 0 e 1 com ponto decimal valem como fracao
 *                   da largura e da altura da imagem (util quando o tamanho do render ainda nao e conhecido).
 *   --perspective   os quatro cantos de destino do logo, em ordem: topo-esquerdo, topo-direito, baixo-direito, baixo-esquerdo (a mesma regra de
 *                   pixel ou fracao). O logo e deformado por uma homografia (resolvida por eliminacao de Gauss, amostra bilinear 2x2 por pixel,
 *                   logo grande demais e reduzido pela metade antes). Vale no lugar do --box.
 *   --blend         casa o logo com a parede: o brilho medio do anel em volta (e a cor dele, em parte) puxa a cor do logo (expoente 0.35, limitado),
 *                   e o gradiente de luz de baixa frequencia sob o logo (luminancia desfocada do render original) modula o resultado. Sem --blend: alfa puro.
 * Sem --out, grava ao lado do render, com "-logo" antes da extensao. */

typedef struct { int w, h; uint8_t *rgba; } SdlImg;

static int sdl_load_rgba(const char *path, SdlImg *o) {
    int64_t sz = 0; const uint8_t *b = map_file(path, &sz);
    if (!b) { fprintf(stderr, "render logo: nao consegui abrir %s\n", path); return 0; }
    Img im; char err[200] = ""; memset(&im, 0, sizeof im);
    im_want_alpha = 1;
    const int ok = img_decode(b, (size_t)sz, &im, 0, err, sizeof err);
    im_want_alpha = 0;
    if (!ok) { fprintf(stderr, "render logo: %s: %s\n", path, err); return 0; }
    o->w = im.w; o->h = im.h; o->rgba = xalloc((size_t)im.w * im.h, 4);
    for (size_t i = 0; i < (size_t)im.w * im.h; i++) { memcpy(o->rgba + 4 * i, im.rgb + 3 * i, 3); o->rgba[4 * i + 3] = im.alpha ? im.alpha[i] : 255; }
    free(im.rgb); free(im.alpha);
    return 1;
}

static int sdl_png_write_rgba(const char *path, const uint8_t *rgba, int w, int h) {
    const size_t rowb = (size_t)w * 4 + 1, rawn = rowb * (size_t)h;
    uint8_t *raw = xalloc(rawn, 1);
    for (int y = 0; y < h; y++) { raw[(size_t)y * rowb] = 0; memcpy(raw + (size_t)y * rowb + 1, rgba + (size_t)y * w * 4, (size_t)w * 4); }
    const size_t nblk = (rawn + 65534) / 65535, zn = 2 + rawn + nblk * 5 + 4;
    uint8_t *z = xalloc(zn, 1); size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawn; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    for (size_t p = 0; p < rawn; p += 65535) {
        const size_t l = rawn - p < 65535 ? rawn - p : 65535;
        z[o++] = p + l >= rawn ? 1 : 0; z[o++] = (uint8_t)l; z[o++] = (uint8_t)(l >> 8); z[o++] = (uint8_t)~l; z[o++] = (uint8_t)(~l >> 8);
        memcpy(z + o, raw + p, l); o += l;
    }
    sdp_be32(z + o, (b << 16) | a); o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return 0; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13]; sdp_be32(ihdr, (uint32_t)w); sdp_be32(ihdr + 4, (uint32_t)h); ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    sdp_chunk(f, "IHDR", ihdr, 13); sdp_chunk(f, "IDAT", z, o); sdp_chunk(f, "IEND", NULL, 0);
    const int ok = fclose(f) == 0;
    free(raw); free(z);
    return ok;
}

/* homografia que leva (x[i], y[i]) a (u[i], u[i]): h[0..7], h8 = 1. Eliminacao de Gauss com pivo parcial; 0 = quadrilatero degenerado */
static int sdl_homography(const double *x, const double *y, const double *u, const double *v, double *h) {
    double A[8][9];
    for (int i = 0; i < 4; i++) {
        const double r0[9] = { x[i], y[i], 1, 0, 0, 0, -u[i] * x[i], -u[i] * y[i], u[i] };
        const double r1[9] = { 0, 0, 0, x[i], y[i], 1, -v[i] * x[i], -v[i] * y[i], v[i] };
        memcpy(A[2 * i], r0, sizeof r0); memcpy(A[2 * i + 1], r1, sizeof r1);
    }
    for (int c = 0; c < 8; c++) {
        int p = c; for (int r = c + 1; r < 8; r++) if (fabs(A[r][c]) > fabs(A[p][c])) p = r;
        if (fabs(A[p][c]) < 1e-12) return 0;
        if (p != c) for (int k = 0; k < 9; k++) { const double t = A[c][k]; A[c][k] = A[p][k]; A[p][k] = t; }
        for (int r = 0; r < 8; r++) if (r != c) { const double f = A[r][c] / A[c][c]; for (int k = c; k < 9; k++) A[r][k] -= f * A[c][k]; }
    }
    for (int i = 0; i < 8; i++) h[i] = A[i][8] / A[i][i];
    return 1;
}

/* reduz pela metade (media 2x2, alfa premultiplicado) ate o logo ficar a menos de 2x do alvo */
static void sdl_shrink(SdlImg *g, double target_w, double target_h) {
    while (g->w >= 4 && g->h >= 4 && (g->w > 2.0 * target_w || g->h > 2.0 * target_h)) {
        const int nw = g->w / 2, nh = g->h / 2; uint8_t *n = xalloc((size_t)nw * nh, 4);
        for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {
            double pm[3] = { 0, 0, 0 }, a = 0;
            for (int k = 0; k < 4; k++) { const uint8_t *s = g->rgba + ((size_t)(2 * y + k / 2) * g->w + 2 * x + k % 2) * 4; for (int c = 0; c < 3; c++) pm[c] += s[c] * s[3]; a += s[3]; }
            uint8_t *d = n + ((size_t)y * nw + x) * 4;
            for (int c = 0; c < 3; c++) d[c] = a > 0 ? (uint8_t)(pm[c] / a + 0.5) : 0;
            d[3] = (uint8_t)(a / 4 + 0.5);
        }
        free(g->rgba); g->rgba = n; g->w = nw; g->h = nh;
    }
}

static double sdl_lum(double r, double g, double b) { return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0; }

/* compoe o logo no render (rgb W x H, no lugar). quad = x1,y1..x4,y4 (TL, TR, BR, BL); blend liga o casamento com a parede. Devolve 0 se o quadrilatero e invalido */
static int sdl_apply(uint8_t *rgb, int W, int H, SdlImg *lgp, const double *quad, int blend) {
    SdlImg logo = *lgp;
    double qx[4], qy[4], lu[4] = { 0, (double)logo.w, (double)logo.w, 0 }, lv[4] = { 0, 0, (double)logo.h, (double)logo.h }, hm[8];
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = 0; i < 4; i++) { qx[i] = quad[2 * i]; qy[i] = quad[2 * i + 1]; if (qx[i] < x0) x0 = qx[i]; if (qx[i] > x1) x1 = qx[i]; if (qy[i] < y0) y0 = qy[i]; if (qy[i] > y1) y1 = qy[i]; }
    if (x1 - x0 < 1 || y1 - y0 < 1) return 0;
    /* tamanho de destino do logo (maior lado de cada par de bordas opostas) para a reducao previa */
    const double ew = fmax(hypot(qx[1] - qx[0], qy[1] - qy[0]), hypot(qx[2] - qx[3], qy[2] - qy[3])), eh = fmax(hypot(qx[3] - qx[0], qy[3] - qy[0]), hypot(qx[2] - qx[1], qy[2] - qy[1]));
    const double ow = logo.w, oh = logo.h;
    sdl_shrink(&logo, ew, eh); *lgp = logo;                                  /* o chamador libera o buffer atual */
    for (int i = 0; i < 4; i++) { lu[i] *= logo.w / ow; lv[i] *= logo.h / oh; }
    if (!sdl_homography(qx, qy, lu, lv, hm)) return 0;
    const int bx0 = (int)floor(x0) < 0 ? 0 : (int)floor(x0), by0 = (int)floor(y0) < 0 ? 0 : (int)floor(y0);
    const int bx1 = (int)ceil(x1) > W ? W : (int)ceil(x1), by1 = (int)ceil(y1) > H ? H : (int)ceil(y1);
    if (bx1 <= bx0 || by1 <= by0) return 0;
    /* casamento: estatisticas do anel em volta e luminancia desfocada do render original */
    double Ls = 0, Rs = 0, Gs = 0, Bs = 0, Ll = 0, Lw = 0, gain = 1, tint[3] = { 1, 1, 1 };
    double *blur = NULL; int rad = 0;
    if (blend) {
        const int m = (int)fmax(4, 0.25 * fmin(bx1 - bx0, by1 - by0));
        const int rx0 = bx0 - m < 0 ? 0 : bx0 - m, ry0 = by0 - m < 0 ? 0 : by0 - m, rx1 = bx1 + m > W ? W : bx1 + m, ry1 = by1 + m > H ? H : by1 + m;
        double n = 0;
        for (int y = ry0; y < ry1; y++) for (int x = rx0; x < rx1; x++) {
            if (x >= bx0 && x < bx1 && y >= by0 && y < by1) continue;
            const uint8_t *p = rgb + ((size_t)y * W + x) * 3; Rs += p[0]; Gs += p[1]; Bs += p[2]; n++;
        }
        if (n < 1) { const uint8_t *p = rgb + ((size_t)by0 * W + bx0) * 3; Rs = p[0]; Gs = p[1]; Bs = p[2]; n = 1; }
        Rs /= n; Gs /= n; Bs /= n; Ls = sdl_lum(Rs, Gs, Bs);
        for (size_t i = 0; i < (size_t)logo.w * logo.h; i++) { const uint8_t *p = logo.rgba + 4 * i; Ll += sdl_lum(p[0], p[1], p[2]) * p[3]; Lw += p[3]; }
        Ll = Lw > 0 ? Ll / Lw : 0.5;
        gain = pow(fmax(Ls, 0.02) / fmax(Ll, 0.05), 0.35); if (gain < 0.55) gain = 0.55; if (gain > 1.5) gain = 1.5;
        const double ch[3] = { Rs, Gs, Bs };
        for (int c = 0; c < 3; c++) { double k = Ls > 0.02 ? ch[c] / 255.0 / Ls : 1.0; if (k < 0.7) k = 0.7; if (k > 1.3) k = 1.3; tint[c] = 1.0 + 0.35 * (k - 1.0); }
        /* luminancia desfocada (janela quadrada, imagem integral) da caixa do logo */
        rad = (int)fmax(3, fmin(bx1 - bx0, by1 - by0) / 4);
        blur = xalloc((size_t)(bx1 - bx0) * (by1 - by0), sizeof(double));
        const int ix0 = bx0 - rad < 0 ? 0 : bx0 - rad, iy0 = by0 - rad < 0 ? 0 : by0 - rad, ix1 = bx1 + rad > W ? W : bx1 + rad, iy1 = by1 + rad > H ? H : by1 + rad;
        const int iw = ix1 - ix0 + 1, ih = iy1 - iy0 + 1; double *I = xalloc((size_t)iw * ih, sizeof(double));
        for (int y = 1; y < ih; y++) { double row = 0; for (int x = 1; x < iw; x++) { const uint8_t *p = rgb + ((size_t)(iy0 + y - 1) * W + ix0 + x - 1) * 3; row += sdl_lum(p[0], p[1], p[2]); I[(size_t)y * iw + x] = I[(size_t)(y - 1) * iw + x] + row; } }
        for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
            const int a = (x - rad < ix0 ? ix0 : x - rad) - ix0, b = (y - rad < iy0 ? iy0 : y - rad) - iy0, c = (x + rad + 1 > ix1 ? ix1 : x + rad + 1) - ix0, d = (y + rad + 1 > iy1 ? iy1 : y + rad + 1) - iy0;
            blur[(size_t)(y - by0) * (bx1 - bx0) + (x - bx0)] = (I[(size_t)d * iw + c] - I[(size_t)b * iw + c] - I[(size_t)d * iw + a] + I[(size_t)b * iw + a]) / ((double)(c - a) * (d - b));
        }
        free(I);
    }
    for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
        double pm[3] = { 0, 0, 0 }, a = 0;
        for (int k = 0; k < 4; k++) {                                            /* 2x2 amostras por pixel */
            const double px = x + 0.25 + 0.5 * (k & 1), py = y + 0.25 + 0.5 * (k >> 1);
            const double dd = hm[6] * px + hm[7] * py + 1.0;
            if (fabs(dd) < 1e-12) continue;
            const double u = (hm[0] * px + hm[1] * py + hm[2]) / dd, v = (hm[3] * px + hm[4] * py + hm[5]) / dd;
            if (u < 0 || v < 0 || u >= logo.w || v >= logo.h) continue;
            double fu = u - 0.5, fv = v - 0.5; int iu = (int)floor(fu), iv = (int)floor(fv); fu -= iu; fv -= iv;
            double s[4] = { 0, 0, 0, 0 };                                         /* bilinear no premultiplicado */
            for (int j = 0; j < 4; j++) {
                int ux = iu + (j & 1), vy = iv + (j >> 1); ux = ux < 0 ? 0 : ux >= logo.w ? logo.w - 1 : ux; vy = vy < 0 ? 0 : vy >= logo.h ? logo.h - 1 : vy;
                const double wgt = ((j & 1) ? fu : 1 - fu) * ((j >> 1) ? fv : 1 - fv); const uint8_t *q = logo.rgba + ((size_t)vy * logo.w + ux) * 4;
                for (int c = 0; c < 3; c++) s[c] += wgt * q[c] * q[3] / 255.0;
                s[3] += wgt * q[3] / 255.0;
            }
            for (int c = 0; c < 3; c++) pm[c] += s[c]; a += s[3];
        }
        a /= 4.0; if (a <= 0) continue;
        uint8_t *p = rgb + ((size_t)y * W + x) * 3;
        double col[3]; for (int c = 0; c < 3; c++) col[c] = pm[c] / 4.0 / a;       /* cor sem alfa */
        if (blend) {
            double sh = 1.0;
            if (Ls > 0.02) { sh = 1.0 + 0.6 * (blur[(size_t)(y - by0) * (bx1 - bx0) + (x - bx0)] / Ls - 1.0); if (sh < 0.75) sh = 0.75; if (sh > 1.25) sh = 1.25; }
            for (int c = 0; c < 3; c++) col[c] *= gain * tint[c] * sh;
        }
        for (int c = 0; c < 3; c++) { double r = p[c] * (1.0 - a) + col[c] * a; p[c] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r + 0.5); }
    }
    free(blur);
    return 1;
}

/* "a,b,c,d..." -> n numeros; fracao = 1 se todos estao entre 0 e 1.5 e o texto tem ponto decimal */
static int sdl_nums(const char *s, double *v, int n, int *frac) {
    int k = 0; const char *p = s;
    while (k < n) {
        char *e; v[k] = strtod(p, &e);
        if (e == p) return 0;
        k++; p = e; if (*p == ',' || *p == ' ' || *p == ';') p++; else break;
    }
    if (k != n || *p) return 0;
    int small = 1; for (int i = 0; i < n; i++) if (v[i] < 0 || v[i] > 1.5) small = 0;
    *frac = small && strchr(s, '.') != NULL;
    return 1;
}

/* o comando inteiro em arquivos; box/persp: texto como na linha de comando (um dos dois); out = destino (pode ser o proprio render) */
static int sdl_run(const char *image, const char *logo_path, const char *box, const char *persp, int blend, const char *out) {
    int W, H;
    if (!sdp_peek_size(image, &W, &H)) { fprintf(stderr, "render logo: nao consegui ler %s\n", image); return 1; }
    uint8_t *rgb = sdp_load_image(image, W, H); if (!rgb) return 1;
    SdlImg lg; if (!sdl_load_rgba(logo_path, &lg)) return 1;
    double q[8], v[8]; int frac = 0;
    if (persp) {
        if (!sdl_nums(persp, v, 8, &frac)) { fprintf(stderr, "render logo: --perspective espera 8 numeros x1,y1,x2,y2,x3,y3,x4,y4 (topo-esquerdo, topo-direito, baixo-direito, baixo-esquerdo)\n"); return 1; }
        for (int i = 0; i < 8; i++) q[i] = v[i] * (frac ? (i & 1 ? H : W) : 1.0);
    } else if (box) {
        if (!sdl_nums(box, v, 4, &frac)) { fprintf(stderr, "render logo: --box espera x,y,w,h\n"); return 1; }
        if (frac) { v[0] *= W; v[2] *= W; v[1] *= H; v[3] *= H; }
        const double s = fmin(v[2] / lg.w, v[3] / lg.h), dw = lg.w * s, dh = lg.h * s, ox = v[0] + (v[2] - dw) / 2, oy = v[1] + (v[3] - dh) / 2;
        const double r[8] = { ox, oy, ox + dw, oy, ox + dw, oy + dh, ox, oy + dh }; memcpy(q, r, sizeof q);
    } else { fprintf(stderr, "render logo: faltam --box x,y,w,h ou --perspective\n"); return 1; }
    if (!sdl_apply(rgb, W, H, &lg, q, blend)) { fprintf(stderr, "render logo: caixa invalida ou fora da imagem\n"); return 1; }
    if (!sdp_png_write(out, rgb, W, H)) { fprintf(stderr, "render logo: nao consegui gravar %s\n", out); return 1; }
    fprintf(stderr, "[logo] %s: logo %dx%d composto%s em %dx%d\n", out, lg.w, lg.h, blend ? " com casamento de cor e luz" : "", W, H);
    free(rgb); free(lg.rgba);
    return 0;
}

/* ---------- teste com imagens sinteticas (nenhuma imagem de cliente) ---------- */
static int sdl_ck(const char *what, int ok) { printf("  %-62s %s\n", what, ok ? "OK" : "FALHOU"); return ok ? 0 : 1; }
static int sdl_near(const uint8_t *p, int r, int g, int b, int tol) { return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol; }

static int sdl_selftest(void) {
    int bad = 0; char pb[200], pl[200], po[200];
    snprintf(pb, sizeof pb, "/tmp/katana-logo-selftest-%d-bg.png", (int)getpid());
    snprintf(pl, sizeof pl, "/tmp/katana-logo-selftest-%d-logo.png", (int)getpid());
    snprintf(po, sizeof po, "/tmp/katana-logo-selftest-%d-out.png", (int)getpid());
    printf("katana render logo selftest (imagens sinteticas)\n");
    /* 1. PNG RGBA: ida e volta, alfa e cor sem alfa preservados */
    { uint8_t t[40 * 20 * 4]; for (int y = 0; y < 20; y++) for (int x = 0; x < 40; x++) { uint8_t *p = t + ((size_t)y * 40 + x) * 4; if (x < 20) { p[0] = 255; p[1] = 0; p[2] = 0; p[3] = 255; } else { p[0] = 0; p[1] = 0; p[2] = 255; p[3] = 0; } }
      SdlImg g; int ok = sdl_png_write_rgba(pl, t, 40, 20) && sdl_load_rgba(pl, &g);
      if (ok) { ok = g.w == 40 && g.h == 20 && sdl_near(g.rgba + ((size_t)5 * 40 + 5) * 4, 255, 0, 0, 0) && g.rgba[((size_t)5 * 40 + 5) * 4 + 3] == 255 && g.rgba[((size_t)5 * 40 + 30) * 4 + 3] == 0 && g.rgba[((size_t)5 * 40 + 30) * 4 + 2] == 255; free(g.rgba); }
      bad += sdl_ck("PNG com alfa: ida e volta, cor sem alfa preservada", ok); }
    /* fundo: parede cinza 100 de 200x100 com um gradiente vertical leve */
    enum { BW = 200, BH = 100 };
    uint8_t *bg = xalloc((size_t)BW * BH, 3), *w = xalloc((size_t)BW * BH, 3);
    for (int y = 0; y < BH; y++) for (int x = 0; x < BW; x++) for (int c = 0; c < 3; c++) bg[((size_t)y * BW + x) * 3 + c] = (uint8_t)(100 + y / 10);
    /* 2. caixa: o logo 40x20 (metade esquerda vermelha opaca, direita transparente) numa caixa 80x40 */
    { uint8_t t[40 * 20 * 4]; for (int y = 0; y < 20; y++) for (int x = 0; x < 40; x++) { uint8_t *p = t + ((size_t)y * 40 + x) * 4; p[0] = 255; p[1] = 0; p[2] = 0; p[3] = x < 20 ? 255 : 0; }
      sdl_png_write_rgba(pl, t, 40, 20); sdp_png_write(pb, bg, BW, BH);
      int rc = sdl_run(pb, pl, "20,30,80,40", NULL, 0, po);
      uint8_t *o = rc ? NULL : sdp_load_image(po, BW, BH);
      bad += sdl_ck("--box: executa e grava", o != NULL);
      if (o) {
          const uint8_t *in = o + ((size_t)50 * BW + 40) * 3, *tr = o + ((size_t)50 * BW + 80) * 3, *out1 = o + ((size_t)5 * BW + 5) * 3;
          bad += sdl_ck("--box: parte opaca do logo (vermelho) dentro da caixa", sdl_near(in, 255, 0, 0, 0));
          bad += sdl_ck("--box: parte transparente deixa o fundo como estava", tr[0] == bg[((size_t)50 * BW + 80) * 3]);
          int same = 1; for (int y = 0; y < BH && same; y++) for (int x = 0; x < BW; x++) { if (x >= 20 && x < 100 && y >= 30 && y < 70) continue; if (memcmp(o + ((size_t)y * BW + x) * 3, bg + ((size_t)y * BW + x) * 3, 3)) { same = 0; break; } }
          bad += sdl_ck("--box: nada fora da caixa muda", same && out1[0] == bg[((size_t)5 * BW + 5) * 3]);
          free(o);
      } }
    /* 3. perspectiva: quatro quadrantes de cores (TL vermelho, TR verde, BR azul, BL amarelo), trapezio */
    { uint8_t t[32 * 32 * 4]; for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { uint8_t *p = t + ((size_t)y * 32 + x) * 4; const int right = x >= 16, low = y >= 16;
          p[0] = !right && !low ? 255 : (!right && low ? 255 : 0); p[1] = right && !low ? 255 : (!right && low ? 255 : 0); p[2] = right && low ? 255 : 0; p[3] = 255; }
      sdl_png_write_rgba(pl, t, 32, 32);
      int rc = sdl_run(pb, pl, NULL, "60,10,140,20,170,90,30,80", 0, po);
      uint8_t *o = rc ? NULL : sdp_load_image(po, BW, BH);
      bad += sdl_ck("--perspective: executa e grava", o != NULL);
      if (o) {
          const uint8_t *tl = o + ((size_t)18 * BW + 70) * 3, *tr = o + ((size_t)26 * BW + 136) * 3, *br = o + ((size_t)82 * BW + 158) * 3, *bl = o + ((size_t)76 * BW + 42) * 3;
          bad += sdl_ck("--perspective: canto topo-esquerdo vermelho no lugar certo", sdl_near(tl, 255, 0, 0, 8));
          bad += sdl_ck("--perspective: canto topo-direito verde", sdl_near(tr, 0, 255, 0, 8));
          bad += sdl_ck("--perspective: canto baixo-direito azul", sdl_near(br, 0, 0, 255, 8));
          bad += sdl_ck("--perspective: canto baixo-esquerdo amarelo", sdl_near(bl, 255, 255, 0, 8));
          const uint8_t *outside = o + ((size_t)15 * BW + 40) * 3;          /* fora do trapezio (a borda de cima comeca em x=60) */
          bad += sdl_ck("--perspective: fora do quadrilatero nada muda", outside[0] == bg[((size_t)15 * BW + 40) * 3] && outside[2] == outside[0]);
          free(o);
      } }
    /* 4. casamento: logo branco opaco numa parede escura (60): sem --blend fica 255; com --blend escurece e continua mais claro que a parede */
    { for (size_t i = 0; i < (size_t)BW * BH * 3; i++) w[i] = (uint8_t)(60 + (i / 3 / BW) / 10);
      uint8_t t[40 * 20 * 4]; for (size_t i = 0; i < 40 * 20; i++) { t[4 * i] = t[4 * i + 1] = t[4 * i + 2] = 255; t[4 * i + 3] = i % 40 < 36 ? 255 : 0; }
      sdl_png_write_rgba(pl, t, 40, 20); sdp_png_write(pb, w, BW, BH);
      int rc = sdl_run(pb, pl, "60,30,80,40", NULL, 0, po); uint8_t *plain = rc ? NULL : sdp_load_image(po, BW, BH);
      rc = sdl_run(pb, pl, "60,30,80,40", NULL, 1, po); uint8_t *bl = rc ? NULL : sdp_load_image(po, BW, BH);
      if (plain && bl) {
          const uint8_t *a = plain + ((size_t)50 * BW + 100) * 3, *b = bl + ((size_t)50 * BW + 100) * 3, *wall = w + ((size_t)50 * BW + 100) * 3;
          bad += sdl_ck("sem --blend: branco do logo fica exato (255)", a[0] == 255 && a[1] == 255);
          bad += sdl_ck("--blend: logo mais escuro que o branco puro e mais claro que a parede", b[0] < 250 && b[0] > wall[0] + 20);
          bad += sdl_ck("--blend: sem fio de cor (cinza em parede cinza, canais a menos de 6)", abs(b[0] - b[1]) < 6 && abs(b[1] - b[2]) < 6);
          const uint8_t *tp = bl + ((size_t)50 * BW + 139) * 3;
          bad += sdl_ck("--blend: area transparente do logo deixa a parede", abs(tp[0] - w[((size_t)50 * BW + 139) * 3]) <= 1);
      } else bad += sdl_ck("--blend: executa", 0);
      free(plain); free(bl); }
    /* 5. fracoes e entradas invalidas */
    { double v[4]; int f = 0;
      bad += sdl_ck("--box em fracao (0.1,0.2,0.3,0.4) reconhecido", sdl_nums("0.1,0.2,0.3,0.4", v, 4, &f) && f && fabs(v[2] - 0.3) < 1e-9);
      bad += sdl_ck("--box em pixels (10,20,30,40) nao e fracao", sdl_nums("10,20,30,40", v, 4, &f) && !f);
      bad += sdl_ck("--box com tres numeros e recusado", !sdl_nums("1,2,3", v, 4, &f));
      double hq[8], px[4] = { 0, 10, 10, 0 }, py[4] = { 0, 0, 10, 10 }, qu[4] = { 0, 1, 1, 0 }, qv[4] = { 0, 0, 1, 1 };
      bad += sdl_ck("homografia: quadrilatero degenerado (4 pontos iguais) e recusado", !sdl_homography((double[]){ 5, 5, 5, 5 }, (double[]){ 5, 5, 5, 5 }, qu, qv, hq));
      bad += sdl_ck("homografia: quadrado -> quadrado unitario", sdl_homography(px, py, qu, qv, hq) && fabs(hq[0] - 0.1) < 1e-9 && fabs(hq[4] - 0.1) < 1e-9); }
    unlink(pb); unlink(pl); unlink(po);
    free(bg); free(w);
    printf("RESULTADO: %s\n", bad ? "FALHOU" : "todos passaram");
    return bad ? 1 : 0;
}

static int sdl_main(int argc, char **argv) {
    const char *image = NULL, *logo = NULL, *box = NULL, *persp = NULL, *out = NULL; int blend = 0;
    if (argc >= 1 && !strcmp(argv[0], "selftest")) return sdl_selftest();
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i]; const int more = i + 1 < argc;
        if (!strcmp(a, "--image") && more) image = argv[++i];
        else if (!strcmp(a, "--logo") && more) logo = argv[++i];
        else if (!strcmp(a, "--box") && more) box = argv[++i];
        else if (!strcmp(a, "--perspective") && more) persp = argv[++i];
        else if (!strcmp(a, "--out") && more) out = argv[++i];
        else if (!strcmp(a, "--blend")) blend = 1;
        else { fprintf(stderr, "render logo: opcao desconhecida ou sem valor: %s\n", a); return 1; }
    }
    if (!image || !logo || (!box && !persp)) {
        fprintf(stderr, "uso: katana render logo --image render.png --logo logo.png --box x,y,w,h [--perspective x1,y1,x2,y2,x3,y3,x4,y4] [--blend] [--out saida.png]\n"
                        "     katana render logo selftest\n");
        return 1;
    }
    char dflt[1200];
    if (!out) { snprintf(dflt, sizeof dflt, "%s", image); char *dot = strrchr(dflt, '.'); if (dot && !strchr(dot, '/')) *dot = 0; strncat(dflt, "-logo.png", sizeof dflt - strlen(dflt) - 1); out = dflt; }
    return sdl_run(image, logo, box, persp, blend, out);
}
