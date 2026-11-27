/* ============================ render: decodificador do VAE do FLUX.2 klein na GPU (CUDA) ===============================
 * Convolucao como GEMM implicito com mma (sem im2col; a ampliacao 2x e a norma por grupos + SiLU entram no carregamento do ladrilho),
 * atencao do meio por produtos (S = Q K^T, softmax, S V). Ativacoes em F32 NHWC na VRAM, pesos em duas metades F16 (alto + residuo) e
 * produtos de 3 termos (padrao, ~22 bits) ou 1 termo (KATANA_CUDA_PREC=fast). Sem placa ou sem VRAM: fxcv_decode devolve NULL e a CPU decodifica.
 * O codificador (imagens de referencia) continua na CPU. */

typedef struct { const SdgInfo *t; CUdptr_ wh, wl, bias; int Kp, cin, cout, k; } FxcvConv;
static FxcvConv g_fcv_c[96]; static int g_fcv_n;
static struct { int tried, ok; void *f_conv, *f_convx, *f_gn, *f_wsplit, *f_sm; } g_fcv;

static int fxcv_init(void) {
    if (g_fcv.tried) return g_fcv.ok;
    g_fcv.tried = 1;
    const char *e = getenv("KATANA_FLUX_CUDA"); if (e && !atoi(e)) return 0;
    { const char *be = getenv("KATANA_BACKEND"); if (be && *be && strcmp(be, "cuda")) return 0; }
    if (!cu_init()) return 0;
    void *m = cu_kmod(); if (!m) return 0;
    g_fcv.f_conv = cu_fn(m, "conv"); g_fcv.f_convx = cu_fn(m, "convx"); g_fcv.f_gn = cu_fn(m, "gn_coef"); g_fcv.f_wsplit = cu_fn(m, "wsplit"); g_fcv.f_sm = cu_fn(m, "softmax_rows");
    if (!g_fcv.f_conv || !g_fcv.f_convx || !g_fcv.f_gn || !g_fcv.f_wsplit || !g_fcv.f_sm) return 0;
    cuapi.FuncSetAttribute(g_fcv.f_conv, 8, 16384); cuapi.FuncSetAttribute(g_fcv.f_convx, 8, 32768);
    g_fcv.ok = 1;
    return 1;
}
static int fxcv_prec(void) { const char *pe = getenv("KATANA_CUDA_PREC"); return !(pe && !strcmp(pe, "fast")); }

static int fxcv_wsplit(CUdptr_ src, long srows, long scols, int trans, int Kp, CUdptr_ wh, CUdptr_ wl) {
    void *a[] = { &src, &srows, &scols, &trans, &Kp, &wh, &wl };
    const long tot = (trans ? scols : srows) * (long)Kp;
    return cu_launch(g_fcv.f_wsplit, (int)((tot + 255) / 256 > 8192 ? 8192 : (tot + 255) / 256), 1, 1, 256, 0, a);
}

/* peso (conv [cout][cin][k][k] ou linear [out][in]) na VRAM como metades F16 [cout][Kp], com k*k*cin reordenado (ky, kx, cin) */
static FxcvConv *fxcv_conv(const SdgFile *g, const char *base) {
    char nm[220]; snprintf(nm, sizeof nm, "%s.weight", base);
    const SdgInfo *w = sdg_find(g, nm); if (!w) return NULL;
    for (int i = 0; i < g_fcv_n; i++) if (g_fcv_c[i].t == w) return &g_fcv_c[i];
    if (g_fcv_n >= 96) return NULL;
    FxcvConv *c = &g_fcv_c[g_fcv_n];
    int k, cin, cout;
    if (w->nd == 4) { k = (int)w->dim[0]; cin = (int)w->dim[2]; cout = (int)w->dim[3]; } else if (w->nd == 2) { k = 1; cin = (int)w->dim[0]; cout = (int)w->dim[1]; } else return NULL;
    if (w->type > 1) return NULL;
    const int K = k * k * cin, Kp = (K + 31) / 32 * 32;
    const size_t n = (size_t)cout * K;
    float *h = xalloc(n, 4);
    const uint8_t *src = sdg_data(g, w);
    for (int o = 0; o < cout; o++)
        for (int i = 0; i < cin; i++)
            for (int ky = 0; ky < k; ky++)
                for (int kx = 0; kx < k; kx++) {
                    const size_t si = (((size_t)o * cin + i) * k + ky) * k + kx, di = (size_t)o * K + ((size_t)ky * k + kx) * cin + i;
#ifdef __F16C__
                    h[di] = w->type == 0 ? ((const float *)src)[si] : _cvtsh_ss(((const uint16_t *)src)[si]);
#else
                    h[di] = w->type == 0 ? ((const float *)src)[si] : f16_to_f32(((const uint16_t *)src)[si]);    /* porte ARM, 2026-11-27: sem SIMD */
#endif
                }
    snprintf(nm, sizeof nm, "%s.bias", base);
    const SdgInfo *b = sdg_find(g, nm);
    CUdptr_ tmp = cu_alloc(n * 4), wh = cu_alloc((size_t)cout * Kp * 2), wl = cu_alloc((size_t)cout * Kp * 2), bd = b ? cu_alloc((size_t)cout * 4) : 0;
    int ok = tmp && wh && wl && (!b || bd) && cu_up(tmp, h, n * 4) && fxcv_wsplit(tmp, cout, K, 0, Kp, wh, wl) && cu_sync();
    if (ok && b) ok = cu_up(bd, sdg_data(g, b), (size_t)cout * 4);
    cu_free(tmp); free(h);
    if (!ok) { cu_free(wh); cu_free(wl); cu_free(bd); return NULL; }
    *c = (FxcvConv){ w, wh, wl, bd, Kp, cin, cout, k };
    g_fcv_n++;
    return c;
}
static void fxc_release_all(void);                                /* fxdit_cu.c: pesos do transformador */
static void fxcv_release(void) { for (int i = 0; i < g_fcv_n; i++) { cu_free(g_fcv_c[i].wh); cu_free(g_fcv_c[i].wl); cu_free(g_fcv_c[i].bias); } g_fcv_n = 0; }

static int fxcv_run(CUdptr_ wh, CUdptr_ wl, CUdptr_ bias, int Kp, int cin, int cout, int k, CUdptr_ X, CUdptr_ Y, CUdptr_ res, CUdptr_ ca, CUdptr_ cb, int silu, int H, int W, int up) {
    const int pr = fxcv_prec(); CUdptr_ nul = 0;
    void *a0[] = { &X, &wh, &bias, &res, &Y, &ca, &cb, &silu, &H, &W, &cin, &cout, &k, &up, &Kp };
    void *a1[] = { &X, &wh, &wl, &bias, &res, &Y, &ca, &cb, &silu, &H, &W, &cin, &cout, &k, &up, &Kp };
    (void)nul;
    return cu_launch(pr ? g_fcv.f_convx : g_fcv.f_conv, (cout + 127) / 128, (H * W + 127) / 128, 1, 256, pr ? 32768 : 16384, pr ? a1 : a0);
}
static int fxcv_conv_run(const FxcvConv *c, CUdptr_ X, CUdptr_ Y, CUdptr_ res, CUdptr_ ca, CUdptr_ cb, int silu, int H, int W, int up) {
    return fxcv_run(c->wh, c->wl, c->bias, c->Kp, c->cin, c->cout, c->k, X, Y, res, ca, cb, silu, H, W, up);
}

/* coeficientes da norma por grupos de <base>.weight/.bias: ca[C], cb[C] */
static int fxcv_gn(const SdgFile *g, const char *base, CUdptr_ X, int n, int C, CUdptr_ ca, CUdptr_ cb, CUdptr_ tmpv) {
    char nm[220]; int ok = 1;
    snprintf(nm, sizeof nm, "%s.weight", base); const float *gw = sdc_vec(g, nm, C, &ok);
    snprintf(nm, sizeof nm, "%s.bias", base); const float *gb = sdc_vec(g, nm, C, &ok);
    if (!ok) return 0;
    if (!cu_up(tmpv, gw, (size_t)C * 4) || !cu_up(tmpv + (size_t)C * 4, gb, (size_t)C * 4)) return 0;
    float eps = 1e-6f; CUdptr_ be = tmpv + (size_t)C * 4;
    void *a[] = { &X, &n, &C, &tmpv, &be, &eps, &ca, &cb };
    return cu_launch(g_fcv.f_gn, 32, 1, 1, 256, 0, a);
}

typedef struct { CUdptr_ b[6]; CUdptr_ ca, cb, tv; CUdptr_ list[16]; int nl; } FxcvMem;

/* resnet: x[H*W][Cin] -> out[H*W][Cout]; t1, t2 temporarios */
static int fxcv_resnet(const SdgFile *g, const char *pfx, CUdptr_ x, CUdptr_ out, CUdptr_ t1, CUdptr_ t2, int H, int W, FxcvMem *M, int *Cout_o) {
    char b[220]; const int n = H * W;
    snprintf(b, sizeof b, "%s.conv1", pfx); FxcvConv *c1 = fxcv_conv(g, b);
    snprintf(b, sizeof b, "%s.conv2", pfx); FxcvConv *c2 = fxcv_conv(g, b);
    if (!c1 || !c2) return 0;
    const int Cin = c1->cin, Cout = c1->cout;
    snprintf(b, sizeof b, "%s.norm1", pfx); if (!fxcv_gn(g, b, x, n, Cin, M->ca, M->cb, M->tv)) return 0;
    if (!fxcv_conv_run(c1, x, t1, 0, M->ca, M->cb, 1, H, W, 0)) return 0;
    snprintf(b, sizeof b, "%s.norm2", pfx); if (!fxcv_gn(g, b, t1, n, Cout, M->ca, M->cb, M->tv)) return 0;
    snprintf(b, sizeof b, "%s.conv_shortcut", pfx); FxcvConv *cs = fxcv_conv(g, b);
    CUdptr_ res = x;
    if (cs) { if (!fxcv_conv_run(cs, x, t2, 0, 0, 0, 0, H, W, 0)) return 0; res = t2; }
    else if (Cin != Cout) return 0;
    if (!fxcv_conv_run(c2, t1, out, res, M->ca, M->cb, 1, H, W, 0)) return 0;
    *Cout_o = Cout;
    return 1;
}

/* atencao do meio (uma cabeca de C sobre todos os pixels): x += out(softmax(q k^T / sqrt(C)) v); x no lugar. S = Q K^T e calculada em blocos de R linhas
 * (R n floats, no maximo 256 MB), entao nao existe a matriz n x n inteira (a 1920x1080 seriam 4 GB); o resultado e o mesmo. O resultado (x + saida) fica em q. */
static int fxcv_attn(const SdgFile *g, const char *pfx, CUdptr_ x, int H, int W, int C, CUdptr_ q, CUdptr_ k, CUdptr_ v, CUdptr_ o_unused, FxcvMem *M) {
    const int n = H * W; char b[220]; (void)o_unused;
    snprintf(b, sizeof b, "%s.group_norm", pfx); if (!fxcv_gn(g, b, x, n, C, M->ca, M->cb, M->tv)) return 0;
    snprintf(b, sizeof b, "%s.to_q", pfx); FxcvConv *cq = fxcv_conv(g, b);
    snprintf(b, sizeof b, "%s.to_k", pfx); FxcvConv *ck = fxcv_conv(g, b);
    snprintf(b, sizeof b, "%s.to_v", pfx); FxcvConv *cv = fxcv_conv(g, b);
    snprintf(b, sizeof b, "%s.to_out.0", pfx); FxcvConv *co = fxcv_conv(g, b);
    if (!cq || !ck || !cv || !co) return 0;
    if (!fxcv_conv_run(cq, x, q, 0, M->ca, M->cb, 0, n, 1, 0) || !fxcv_conv_run(ck, x, k, 0, M->ca, M->cb, 0, n, 1, 0) || !fxcv_conv_run(cv, x, v, 0, M->ca, M->cb, 0, n, 1, 0)) return 0;
    const int Kp2 = (n + 31) / 32 * 32;
    long R = (long)(((size_t)256 << 20) / ((size_t)n * 4)); if (R > n) R = n; if (R < 32) R = 32;
    CUdptr_ S = cu_alloc((size_t)R * n * 4), o = cu_alloc((size_t)n * C * 4), kh = cu_alloc((size_t)n * C * 2), kl = cu_alloc((size_t)n * C * 2), vh = cu_alloc((size_t)C * Kp2 * 2), vl = cu_alloc((size_t)C * Kp2 * 2);
    int ok = S && o && kh && kl && vh && vl;
    if (ok) ok = fxcv_wsplit(k, n, C, 0, C, kh, kl) && fxcv_wsplit(v, n, C, 1, Kp2, vh, vl);
    for (long r0 = 0; ok && r0 < n; r0 += R) {
        const int m = (int)(n - r0 < R ? n - r0 : R);
        ok = fxcv_run(kh, kl, 0, C, C, n, 1, q + (CUdptr_)r0 * C * 4, S, 0, 0, 0, 0, m, 1, 0);            /* S[i][j] = sum_d q[i][d] k[j][d] */
        if (ok) { float sc = 1.0f / sqrtf((float)C); int nn = n; void *a[] = { &S, &nn, &sc }; ok = cu_launch(g_fcv.f_sm, m, 1, 1, 256, 0, a); }
        if (ok) ok = fxcv_run(vh, vl, 0, Kp2, n, C, 1, S, o + (CUdptr_)r0 * C * 4, 0, 0, 0, 0, m, 1, 0);   /* O[i][d] = sum_j S[i][j] v[j][d] */
    }
    if (ok) ok = cu_sync();
    cu_free(S); cu_free(kh); cu_free(kl); cu_free(vh); cu_free(vl);
    if (ok) ok = fxcv_conv_run(co, o, q, x, 0, 0, 0, n, 1, 0);
    if (ok) ok = cu_sync();
    cu_free(o);
    return ok;
}

/* fichas normalizadas -> imagem [8*2th * 8*2tw][3] (memoria nova) ou NULL se nao deu (o chamador usa a CPU) */
static float *fxcv_decode_one(const FxVae *v, const float *tok, int th, int tw, int *Ho, int *Wo) {
    if (!fxcv_init()) return NULL;
    const SdgFile *g = &v->g; char b[220];
    int H = 2 * th, W = 2 * tw, C = 512, ok = 1;
    size_t mx = (size_t)H * W * 512, hh = H, ww = W; int cc = 512;
    for (int blk = 0; blk < 4; blk++) { const int ch[4] = { 512, 512, 256, 128 }; cc = ch[blk]; size_t s = hh * ww * (size_t)cc; if (s > mx) mx = s; if (blk < 3) { hh *= 2; ww *= 2; s = hh * ww * (size_t)cc; if (s > mx) mx = s; } }
    CUdptr_ buf[4] = { 0 }, ca = cu_alloc(512 * 4), cb = cu_alloc(512 * 4), tv = cu_alloc(1024 * 4), zd = 0;
    const size_t sz = mx * 4;
    float *img = NULL, *z = fxv_unpatch(v, tok, th, tw);
    int nb = 0; for (; nb < 4; nb++) { buf[nb] = cu_alloc(sz); if (!buf[nb]) break; }
    const int need = 4;
    FxcvMem M = { {0}, ca, cb, tv, {0}, 0 };
    if (nb < need || !ca || !cb || !tv) { ok = 0; goto done; }
    zd = cu_alloc((size_t)H * W * 32 * 4);
    if (!zd || !cu_up(zd, z, (size_t)H * W * 32 * 4)) { ok = 0; goto done; }
    {
        CUdptr_ x = buf[0], y = buf[1], t1 = buf[2], t2 = buf[3];
        FxcvConv *pq = fxcv_conv(g, "vae.post_quant_conv"), *ci = fxcv_conv(g, "vae.decoder.conv_in");
        if (!pq || !ci) { ok = 0; goto done; }
        if (!fxcv_conv_run(pq, zd, y, 0, 0, 0, 0, H, W, 0) || !fxcv_conv_run(ci, y, x, 0, 0, 0, 0, H, W, 0)) { ok = 0; goto done; }
        /* meio: resnet, atencao, resnet */
        int Co;
        snprintf(b, sizeof b, "vae.decoder.mid_block.resnets.0");
        if (!fxcv_resnet(g, b, x, y, t1, t2, H, W, &M, &Co)) { ok = 0; goto done; }
        CUdptr_ tmp = x; x = y; y = tmp;                                      /* x = saida do resnet */
        if (!fxcv_attn(g, "vae.decoder.mid_block.attentions.0", x, H, W, C, t1, t2, y, 0, &M)) { ok = 0; goto done; }
        tmp = x; x = t1; t1 = tmp;                                            /* o resultado da atencao esta em t1 */
        snprintf(b, sizeof b, "vae.decoder.mid_block.resnets.1");
        if (!fxcv_resnet(g, b, x, y, t1, t2, H, W, &M, &Co)) { ok = 0; goto done; }
        tmp = x; x = y; y = tmp;
        for (int blk = 0; blk < 4 && ok; blk++) {
            for (int l = 0; l < 3 && ok; l++) {
                snprintf(b, sizeof b, "vae.decoder.up_blocks.%d.resnets.%d", blk, l);
                ok &= fxcv_resnet(g, b, x, y, t1, t2, H, W, &M, &Co);
                if (ok) { tmp = x; x = y; y = tmp; C = Co; }
            }
            if (ok && blk < 3) {
                snprintf(b, sizeof b, "vae.decoder.up_blocks.%d.upsamplers.0.conv", blk);
                FxcvConv *cu = fxcv_conv(g, b);
                if (!cu) { ok = 0; break; }
                ok &= fxcv_conv_run(cu, x, y, 0, 0, 0, 0, 2 * H, 2 * W, 1);
                if (ok) { tmp = x; x = y; y = tmp; H *= 2; W *= 2; }
            }
        }
        FxcvConv *co = fxcv_conv(g, "vae.decoder.conv_out");
        if (!ok || !co || !fxcv_gn(g, "vae.decoder.conv_norm_out", x, H * W, C, M.ca, M.cb, M.tv)) { ok = 0; goto done; }
        if (!fxcv_conv_run(co, x, y, 0, M.ca, M.cb, 1, H, W, 0) || !cu_sync()) { ok = 0; goto done; }
        img = xalloc((size_t)H * W * 3, 4);
        if (!cu_down(img, y, (size_t)H * W * 3 * 4)) { ok = 0; free(img); img = NULL; }
    }
done:
    cu_sync();
    for (int i = 0; i < 4; i++) cu_free(buf[i]);
    cu_free(ca); cu_free(cb); cu_free(tv); cu_free(zd); free(z);
    if (!ok || !img) { if (img) free(img); fprintf(stderr, "flux-vae: a GPU (CUDA) nao conseguiu; usando a CPU\n"); return NULL; }
    *Ho = H; *Wo = W;
    g_fx_be_vae = 1;
    return img;
}

/* VRAM que a decodificacao inteira de th x tw fichas pede: 4 buffers F32 do maior nivel, pesos F16 em duas metades (~0,25 GB), atencao do meio */
static size_t fxcv_need(int th, int tw) {
    const size_t H = 2 * (size_t)th, W = 2 * (size_t)tw;
    size_t mx = H * W * 512; size_t hh = H, ww = W;
    for (int blk = 0; blk < 4; blk++) { const int ch[4] = { 512, 512, 256, 128 }; size_t s = hh * ww * ch[blk]; if (s > mx) mx = s; if (blk < 3) { hh *= 2; ww *= 2; s = hh * ww * ch[blk]; if (s > mx) mx = s; } }
    const size_t n = H * W;
    return 4 * mx * 4 + ((size_t)256 << 20) + (size_t)3 * n * 512 * 4 + ((size_t)320 << 20);
}

/* Decodifica th x tw fichas. Se a imagem inteira cabe na VRAM livre (depois de devolver os pesos do transformador), e exatamente o mesmo calculo de
 * antes. Se nao, em ladrilhos de fichas com sobreposicao e mistura linear (como o VAE em ladrilhos do diffusers): cada ladrilho tem as proprias
 * estatisticas de GroupNorm e a propria atencao do meio, entao a imagem difere um pouco da decodificacao inteira (medido em docs/render.md). */
static float *fxcv_decode(const FxVae *v, const float *tok, int th, int tw, int *Ho, int *Wo) {
    if (!fxcv_init()) return NULL;
    size_t fr = 0, tot = 0; cuapi.MemGetInfo(&fr, &tot);
    if (fxcv_need(th, tw) > fr) { fxc_release_all(); cuapi.MemGetInfo(&fr, &tot); }
    const size_t avail = fr > ((size_t)64 << 20) ? fr - ((size_t)64 << 20) : 0;
    { const char *e = getenv("KATANA_CUDA_VAE_TILE"); (void)e; }
    int ty = th, tx = tw;
    { const char *e = getenv("KATANA_CUDA_VAE_TILE"); if (e && atoi(e) > 0) { ty = th < atoi(e) ? th : atoi(e); tx = tw < atoi(e) ? tw : atoi(e); } }
    while ((ty > 8 || tx > 8) && fxcv_need(ty, tx) > avail) { if (ty >= tx) ty = ty * 3 / 4; else tx = tx * 3 / 4; }
    if (cu_dev.debug) fprintf(stderr, "cuda: VAE %dx%d fichas, VRAM livre %.0f MB, pede %.0f MB; %s\n", tw, th, fr / 1048576.0, fxcv_need(th, tw) / 1048576.0,
                              (ty == th && tx == tw) ? "imagem inteira" : "em ladrilhos");
    if (ty == th && tx == tw) return fxcv_decode_one(v, tok, th, tw, Ho, Wo);
    if (fxcv_need(ty, tx) > avail) return NULL;
    /* ladrilhos de ty x tx fichas, sobreposicao de ov fichas (16 pixels por ficha na saida) */
    const int ov = 6;
    int sy = ty - ov, sx = tx - ov; if (sy < 1) sy = 1; if (sx < 1) sx = 1;
    const int OH = 16 * th, OW = 16 * tw;
    float *acc = xalloc((size_t)OH * OW * 3, 4), *wsum = xalloc((size_t)OH * OW, 4);
    float *sub = xalloc((size_t)ty * tx * 128, 4);
    int ok = 1, ntile = 0;
    for (int y0 = 0; ok; y0 += sy) {
        if (y0 + ty > th) y0 = th - ty;
        for (int x0 = 0; ok; x0 += sx) {
            if (x0 + tx > tw) x0 = tw - tx;
            for (int r = 0; r < ty; r++) memcpy(sub + (size_t)r * tx * 128, tok + ((size_t)(y0 + r) * tw + x0) * 128, (size_t)tx * 128 * 4);
            int ho, wo; float *im = fxcv_decode_one(v, sub, ty, tx, &ho, &wo);
            if (!im) { ok = 0; break; }
            ntile++;
            for (int r = 0; r < ho; r++) {
                const int gy = 16 * y0 + r;
                float wy = 1.0f; { const int e = 16 * ov / 2; if (y0 > 0 && r < 2 * e) wy = (float)(r + 1) / (2 * e + 1); if (y0 + ty < th && ho - 1 - r < 2 * e) { const float w2 = (float)(ho - r) / (2 * e + 1); if (w2 < wy) wy = w2; } }
                for (int c = 0; c < wo; c++) {
                    const int gx = 16 * x0 + c;
                    float wx = 1.0f; { const int e = 16 * ov / 2; if (x0 > 0 && c < 2 * e) wx = (float)(c + 1) / (2 * e + 1); if (x0 + tx < tw && wo - 1 - c < 2 * e) { const float w2 = (float)(wo - c) / (2 * e + 1); if (w2 < wx) wx = w2; } }
                    const float w = wy * wx; float *a = acc + ((size_t)gy * OW + gx) * 3; const float *p = im + ((size_t)r * wo + c) * 3;
                    a[0] += w * p[0]; a[1] += w * p[1]; a[2] += w * p[2]; wsum[(size_t)gy * OW + gx] += w;
                }
            }
            free(im);
            if (x0 + tx >= tw) break;
        }
        if (y0 + ty >= th) break;
    }
    free(sub);
    if (!ok) { free(acc); free(wsum); return NULL; }
    for (size_t i = 0; i < (size_t)OH * OW; i++) { const float iw = 1.0f / wsum[i]; acc[i * 3] *= iw; acc[i * 3 + 1] *= iw; acc[i * 3 + 2] *= iw; }
    free(wsum);
    if (cu_dev.debug) fprintf(stderr, "cuda: VAE em %d ladrilhos de %dx%d fichas\n", ntile, tx, ty);
    *Ho = OH; *Wo = OW;
    return acc;
}
