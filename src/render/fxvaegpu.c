/* ============================ render: VAE do FLUX.2 na GPU (Vulkan, camada kvk) ===============================
 * Decodificador e codificador inteiros na VRAM: convolucao como produto de matrizes (im2col em blocos -> mm.comp de matriz cooperativa, pesos F32
 * em duas partes fp16 e ativacao em duas partes: o mesmo produto preciso do transformador), GroupNorm + SiLU, ampliacao 2x dentro do im2col,
 * soma do resnet junto da transposicao da saida. So a atencao do meio (uma cabeca de 512 sobre todos os pixels, uma vez por imagem) volta
 * a CPU. NHWC como na CPU. Devolve NULL (e o chamador usa a CPU) se nao ha GPU ou a VRAM nao basta. */

typedef struct { const SdgFile *g; char name[96]; KvkBuf w, b; size_t wpr; int cin, cout, k; } FxqConv;
static struct {
    FxqConv c[160]; int nc;
    KvkBuf buf[4], col, y, part, gb; size_t cbuf, ccol, cy, cgb;
    KvkPipe im2col, tpb, tpbr, gn[3];
    int ready; uint64_t wbytes;
    double t;
} fxq;

static int fxq_open(void) {
    fxv_init();
    if (!fxv.on) return 0;
    if (fxq.ready) return 1;
    fxq.im2col = kvk_pipe_create(fx_im2col_spv, sizeof fx_im2col_spv, 2, 52, NULL, NULL, 0);
    fxq.tpb = kvk_pipe_create(fx_tpb_spv, sizeof fx_tpb_spv, 3, 16, NULL, NULL, 0);
    fxq.tpbr = kvk_pipe_create(fx_tpbr_spv, sizeof fx_tpbr_spv, 4, 16, NULL, NULL, 0);
    fxq.gn[0] = kvk_pipe_create(fx_gn0_spv, sizeof fx_gn0_spv, 4, 16, NULL, NULL, 0);
    fxq.gn[1] = kvk_pipe_create(fx_gn1_spv, sizeof fx_gn1_spv, 4, 16, NULL, NULL, 0);
    fxq.gn[2] = kvk_pipe_create(fx_gn2_spv, sizeof fx_gn2_spv, 4, 16, NULL, NULL, 0);
    if (!fxq.im2col.p || !fxq.tpb.p || !fxq.tpbr.p || !fxq.gn[0].p || !fxq.gn[1].p || !fxq.gn[2].p) { fprintf(stderr, "flux: pipelines do VAE na GPU falharam (%s)\n", kvk_error()); return 0; }
    fxq.part = kvk_buf_create(2 * 32 * 64 * 4, KVK_USAGE_RW);
    if (!fxq.part.b) return 0;
    fxq.ready = 1;
    return 1;
}

typedef struct { const float *src; const uint16_t *s16; uint32_t *dst; int cols, rows, Kp, Rp; } FxqPack;
static void fxq_pack_job(void *cx, int g0, int g1) {
    const FxqPack *j = cx;
    for (int g = g0; g < g1; g++)
        for (int k = 0; k < j->Kp; k++)
            for (int r = 0; r < 32; r++) {
                const int row = g * 32 + r; float v = 0.0f;
                if (row < j->rows && k < j->cols) v = j->src ? j->src[(size_t)row * j->cols + k] : f16_to_f32(j->s16[(size_t)row * j->cols + k]);
                memcpy(&j->dst[((size_t)g * j->Kp + k) * 32 + r], &v, 4);
            }
}
/* convolucao do GGUF na GPU (pesos [saida][k*k*entrada] preenchidos ate Kp = multiplo de 64, linhas ate multiplo de 64) */
static const FxqConv *fxq_conv(const SdgFile *g, const char *name) {
    for (int i = 0; i < fxq.nc; i++) if (fxq.c[i].g == g && !strcmp(fxq.c[i].name, name)) return &fxq.c[i];
    if (fxq.nc >= 160 || strlen(name) >= sizeof fxq.c[0].name) return NULL;
    SdrConv c;
    if (!sdr_conv_load(g, name, &c)) return NULL;
    FxqConv *e = &fxq.c[fxq.nc]; memset(e, 0, sizeof *e);
    e->g = g; snprintf(e->name, sizeof e->name, "%s", name); e->cin = c.cin; e->cout = c.cout; e->k = c.k;
    const int Kd = c.k * c.k * c.cin, Kp = (Kd + 63) / 64 * 64, Rp = (c.cout + 63) / 64 * 64;
    const size_t bytes = (size_t)Rp * Kp * 4;
    uint32_t *tmp = malloc(bytes);
    if (!tmp) { sdr_conv_free(&c); return NULL; }
    FxqPack pj = { c.W.type == VT_F32 ? (const float *)c.W.p : NULL, c.W.type == VT_F32 ? NULL : (const uint16_t *)c.W.p, tmp, Kd, c.cout, Kp, Rp };
    pool_for(fxq_pack_job, &pj, Rp / 32, 1);
    e->w = kvk_buf_create(bytes, KVK_USAGE_RW); e->b = kvk_buf_create((size_t)Rp * 4, KVK_USAGE_RW);
    float *bz = calloc((size_t)Rp, 4); memcpy(bz, c.bias, (size_t)c.cout * 4);
    const int ok = e->w.b && e->b.b && kvk_buf_write(e->w, 0, tmp, bytes) && kvk_buf_write(e->b, 0, bz, (size_t)Rp * 4);
    free(tmp); free(bz); sdr_conv_free(&c);
    if (!ok) return NULL;
    e->wpr = (size_t)Kp; fxq.wbytes += bytes;
    fxq.nc++;
    return e;
}

/* y = conv(x) [+ res]: x NHWC [H][W][cin] (com up: imagem [H/2][W/2] ampliada 2x), saida [Ho*Wo][cout] */
static int fxq_run_conv(const FxqConv *c, KvkBuf x, int H, int W, int stride, int pt, int pl, int pb, int pr, int up, KvkBuf out, KvkBuf res, int *Ho_o, int *Wo_o) {
    const int K = c->k, Ho = (H + pt + pb - K) / stride + 1, Wo = (W + pl + pr - K) / stride + 1, N = Ho * Wo;
    const int Kd = K * K * c->cin, Kp = (int)c->wpr;
    if (Ho_o) *Ho_o = Ho;
    if (Wo_o) *Wo_o = Wo;
    int B = (int)(((size_t)96 << 20) / ((size_t)(Kp > c->cout ? Kp : c->cout) * 4)) / 64 * 64;
    if (B < 64) B = 64;
    if (B > N) B = N;
    if (!fxv_grow(&fxq.col, &fxq.ccol, (size_t)Kp * B * 4) || !fxv_grow(&fxq.y, &fxq.cy, (size_t)c->cout * B * 4)) return 0;
    int ok = 1;
    for (int p0 = 0; p0 < N && ok; p0 += B) {
        const int b = N - p0 < B ? N - p0 : B;
        const uint32_t p1[13] = { (uint32_t)H, (uint32_t)W, (uint32_t)c->cin, (uint32_t)K, (uint32_t)stride, (uint32_t)pt, (uint32_t)pl, (uint32_t)Wo, (uint32_t)b, (uint32_t)p0, (uint32_t)Kd, (uint32_t)Kp, (uint32_t)up };
        const size_t tot = (size_t)Kp * b, grp = (tot + 255) / 256;
        KvkBuf b1[2] = { x, fxq.col };
        ok &= kvk_dispatch(fxq.im2col, b1, 2, p1, 52, (uint32_t)(grp < 32768 ? grp : 32768), (uint32_t)((grp + 32767) / 32768), 1);
        ok &= vk_mm_pw(T_F32, c->w, 0, c->wpr, fxq.col, fxq.y, c->cout, Kp, b, 0, 1);
        const uint32_t p2[4] = { (uint32_t)c->cout, (uint32_t)b, 0, (uint32_t)((size_t)p0 * c->cout) };
        if (res.b) { KvkBuf b2[4] = { fxq.y, out, c->b, res }; ok &= kvk_dispatch(fxq.tpbr, b2, 4, p2, 16, (uint32_t)((b + 15) / 16), (uint32_t)((c->cout + 15) / 16), 1); }
        else { KvkBuf b2[3] = { fxq.y, out, c->b }; ok &= kvk_dispatch(fxq.tpb, b2, 3, p2, 16, (uint32_t)((b + 15) / 16), (uint32_t)((c->cout + 15) / 16), 1); }
    }
    return ok;
}
static int fxq_conv_named(const SdgFile *g, const char *name, KvkBuf x, int H, int W, int stride, int pad, int up, KvkBuf out, KvkBuf res) {
    const FxqConv *c = fxq_conv(g, name);
    return c && fxq_run_conv(c, x, H, W, stride, pad, pad, pad, pad, up, out, res, NULL, NULL);
}

typedef struct { const SdgFile *g; char name[96]; KvkBuf b; } FxqGn;
static FxqGn fxq_gns[96]; static int fxq_ngn;
static int fxq_gn(const SdgFile *g, const char *base, KvkBuf x, KvkBuf y, int n, int C, int silu) {
    FxqGn *e = NULL;
    for (int i = 0; i < fxq_ngn; i++) if (fxq_gns[i].g == g && !strcmp(fxq_gns[i].name, base)) { e = &fxq_gns[i]; break; }
    if (!e) {
        char nm[220]; int ok = 1;
        if (fxq_ngn >= 96 || strlen(base) >= sizeof e->name) return 0;
        snprintf(nm, sizeof nm, "%s.weight", base); const float *gw = sdc_vec(g, nm, C, &ok);
        snprintf(nm, sizeof nm, "%s.bias", base); const float *gbv = sdc_vec(g, nm, C, &ok);
        if (!ok) return 0;
        float *tmp = malloc((size_t)2 * C * 4); memcpy(tmp, gw, (size_t)C * 4); memcpy(tmp + C, gbv, (size_t)C * 4);
        e = &fxq_gns[fxq_ngn]; e->g = g; snprintf(e->name, sizeof e->name, "%s", base);
        e->b = kvk_buf_create((size_t)2 * C * 4, KVK_USAGE_RW);
        ok = e->b.b && kvk_buf_write(e->b, 0, tmp, (size_t)2 * C * 4); free(tmp);
        if (!ok) return 0;
        fxq_ngn++;
    }
    uint32_t p[4] = { (uint32_t)n, (uint32_t)C, (uint32_t)silu, 0 }; const float eps = 1e-6f; memcpy(&p[3], &eps, 4);
    KvkBuf b[4] = { x, fxq.part, y, e->b };
    int ok = kvk_dispatch(fxq.gn[0], b, 4, p, 16, 32 * 64, 1, 1);
    ok &= kvk_dispatch(fxq.gn[1], b, 4, p, 16, 32 * 64, 1, 1);
    const size_t tot = (size_t)n * C, grp = (tot + 255) / 256;
    ok &= kvk_dispatch(fxq.gn[2], b, 4, p, 16, (uint32_t)(grp < 32768 ? grp : 32768), (uint32_t)((grp + 32767) / 32768), 1);
    return ok;
}

/* resnet: x -> out (buffers distintos); t e u de trabalho */
static int fxq_resnet(const SdgFile *g, const char *pfx, KvkBuf x, KvkBuf out, KvkBuf t, KvkBuf u, int H, int W, int *Cout_o) {
    char b[220]; int cin, cout, ok = 1; const int n = H * W;
    snprintf(b, sizeof b, "%s.conv1", pfx); if (!sdn_conv_cin_cout(g, b, &cin, &cout)) return 0;
    snprintf(b, sizeof b, "%s.norm1", pfx); ok &= fxq_gn(g, b, x, t, n, cin, 1);
    snprintf(b, sizeof b, "%s.conv1", pfx); ok &= fxq_conv_named(g, b, t, H, W, 1, 1, 0, u, (KvkBuf){0});
    snprintf(b, sizeof b, "%s.norm2", pfx); ok &= fxq_gn(g, b, u, t, n, cout, 1);
    KvkBuf res = x;
    snprintf(b, sizeof b, "%s.conv_shortcut.weight", pfx);
    if (sdg_find(g, b)) { snprintf(b, sizeof b, "%s.conv_shortcut", pfx); ok &= fxq_conv_named(g, b, x, H, W, 1, 0, 0, u, (KvkBuf){0}); res = u; }
    else if (cin != cout) return 0;
    snprintf(b, sizeof b, "%s.conv2", pfx); ok &= fxq_conv_named(g, b, t, H, W, 1, 1, 0, out, res);
    *Cout_o = cout;
    return ok;
}

/* ---- o VAE inteiro ---- */
typedef struct { KvkBuf v[4]; } FxqBufs;
static int fxq_alloc(FxqBufs *B, size_t floats) {
    for (int i = 0; i < 4; i++) {
        if (fxq.buf[i].b && fxq.buf[i].size >= floats * 4) { B->v[i] = fxq.buf[i]; continue; }
        if (fxq.buf[i].b) kvk_buf_destroy(&fxq.buf[i]);
        fxq.buf[i] = kvk_buf_create(floats * 4, KVK_USAGE_RW);
        if (!fxq.buf[i].b) return 0;
        B->v[i] = fxq.buf[i];
    }
    return 1;
}
/* atencao do meio na CPU (uma cabeca de 512 sobre todos os pixels): volta x, roda sdv_attention, sobe */
static int fxq_mid_attention(const SdgFile *g, const char *pfx, KvkBuf x, int H, int W, int C) {
    const size_t n = (size_t)H * W * C;
    float *h = malloc(n * 4);
    char b[220]; snprintf(b, sizeof b, "%s.attentions.0", pfx);
    int ok = h && kvk_buf_read(x, 0, h, n * 4) && sdv_attention(g, b, h, H, W, C) && kvk_buf_write(x, 0, h, n * 4);
    free(h);
    return ok;
}
static int fxq_mid(const SdgFile *g, const char *pfx, FxqBufs *B, int *cur, int H, int W, int C) {
    char b[220]; int Co, ok = 1; KvkBuf *v = B->v; const int o = (*cur + 1) & 3;
    snprintf(b, sizeof b, "%s.resnets.0", pfx);
    ok &= fxq_resnet(g, b, v[*cur], v[o], v[(*cur + 2) & 3], v[(*cur + 3) & 3], H, W, &Co);
    *cur = o;
    if (ok) ok &= fxq_mid_attention(g, pfx, v[*cur], H, W, C);
    const int o2 = (*cur + 1) & 3;
    snprintf(b, sizeof b, "%s.resnets.1", pfx);
    if (ok) ok &= fxq_resnet(g, b, v[*cur], v[o2], v[(*cur + 2) & 3], v[(*cur + 3) & 3], H, W, &Co);
    *cur = o2;
    return ok;
}
static void fxq_release_dit(void) {      /* devolve a VRAM dos pesos do transformador antes do VAE (o proximo forward os sobe de novo) */
    for (int i = 0; i < fxv.nw; i++) if (fxv.w[i].gpu) kvk_buf_destroy(&fxv.w[i].w);
    fxv.nw = 0; fxv.wbytes = 0;
    KvkBuf *all[] = { &fxr.z, &fxr.xn, &fxr.h, &fxr.cat, &fxr.o, &fxr.mod, &fxr.cs, &fxr.sn, &fxv.x32, &fxv.xt, &fxv.y, &fxv.yt };
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) if (all[i]->b) kvk_buf_destroy(all[i]);
    fxr.N = 0; fxv.cx32 = fxv.cxt = fxv.cy = fxv.cyt = 0;
}

/* latente [H][W][32] (ja desfeita a normalizacao e o patchify) -> imagem [8H][8W][3] em memoria nova; NULL = usar a CPU */
static void fxq_dbg(const char *what, KvkBuf b, size_t n) {
    if (!getenv("FXQ_DBG")) return;
    float *h = malloc(n * 4); kvk_buf_read(b, 0, h, n * 4);
    double s = 0, a = 0; int nan = 0; for (size_t i = 0; i < n; i++) { if (h[i] != h[i]) nan++; else { s += h[i]; a += fabs(h[i]); } }
    fprintf(stderr, "  dbg %-28s n=%zu mean %.5f meanabs %.5f nan %d\n", what, n, s / n, a / n, nan); free(h);
}
static float *fxq_decode(const SdgFile *g, const float *z, int H, int W, int *Ho, int *Wo) {
    if (!fxq_open()) return NULL;
    const double t0 = sdt_now();
    fxq_release_dit();
    FxqBufs B; char b[220]; int ok = 1, C = 512;
    const size_t maxf = (size_t)H * W * 16384;     /* o maior mapa: a saida da ultima ampliacao, 64 H0 x 64 W0 pixels... de 256 canais */
    if (!fxq_alloc(&B, maxf)) { fprintf(stderr, "flux: VAE na GPU sem VRAM, CPU\n"); return NULL; }
    int cur = 0, o;
    if (!kvk_buf_write(B.v[0], 0, z, (size_t)H * W * 32 * 4)) return NULL;
    ok &= fxq_conv_named(g, "vae.post_quant_conv", B.v[0], H, W, 1, 0, 0, B.v[1], (KvkBuf){0});
    ok &= fxq_conv_named(g, "vae.decoder.conv_in", B.v[1], H, W, 1, 1, 0, B.v[0], (KvkBuf){0});
    cur = 0; fxq_dbg("conv_in", B.v[0], (size_t)H * W * 512);
    if (ok) ok &= fxq_mid(g, "vae.decoder.mid_block", &B, &cur, H, W, C);
    fxq_dbg("mid", B.v[cur], (size_t)H * W * 512);
    for (int blk = 0; blk < 4 && ok; blk++) {
        for (int l = 0; l < 3 && ok; l++) {
            snprintf(b, sizeof b, "vae.decoder.up_blocks.%d.resnets.%d", blk, l);
            o = (cur + 1) & 3;
            ok &= fxq_resnet(g, b, B.v[cur], B.v[o], B.v[(cur + 2) & 3], B.v[(cur + 3) & 3], H, W, &C);
            cur = o; fxq_dbg("resnet", B.v[cur], (size_t)H * W * C);
        }
        if (ok && blk < 3) {
            snprintf(b, sizeof b, "vae.decoder.up_blocks.%d.upsamplers.0.conv", blk);
            o = (cur + 1) & 3;
            ok &= fxq_conv_named(g, b, B.v[cur], 2 * H, 2 * W, 1, 1, 1, B.v[o], (KvkBuf){0});
            cur = o; H *= 2; W *= 2;
        }
    }
    float *img = NULL;
    if (ok) {
        o = (cur + 1) & 3;
        ok &= fxq_gn(g, "vae.decoder.conv_norm_out", B.v[cur], B.v[o], H * W, C, 1);
        ok &= fxq_conv_named(g, "vae.decoder.conv_out", B.v[o], H, W, 1, 1, 0, B.v[cur], (KvkBuf){0});
        if (ok) { img = xalloc((size_t)H * W * 3, 4); ok = kvk_buf_read(B.v[cur], 0, img, (size_t)H * W * 3 * 4); }
    }
    if (!ok) { fprintf(stderr, "flux: VAE na GPU falhou (%s), CPU\n", kvk_error()); free(img); return NULL; }
    fxq.t += sdt_now() - t0;
    *Ho = H; *Wo = W;
    return img;
}

/* imagem [H0][W0][3] em [-1, 1] -> media do latente [H0/8 * W0/8][32] em memoria nova; NULL = usar a CPU */
static float *fxq_encode(const SdgFile *g, const float *img, int H0, int W0, int *ho, int *wo) {
    if (!fxq_open()) return NULL;
    const double t0 = sdt_now();
    fxq_release_dit();
    FxqBufs B; char b[220]; int ok = 1, C = 128, H = H0, W = W0, cur, o;
    if (!fxq_alloc(&B, (size_t)H0 * W0 * 128)) { fprintf(stderr, "flux: VAE na GPU sem VRAM, CPU\n"); return NULL; }
    if (!kvk_buf_write(B.v[1], 0, img, (size_t)H * W * 3 * 4)) return NULL;
    ok &= fxq_conv_named(g, "vae.encoder.conv_in", B.v[1], H, W, 1, 1, 0, B.v[0], (KvkBuf){0});
    cur = 0;
    for (int blk = 0; blk < 4 && ok; blk++) {
        for (int l = 0; l < 2 && ok; l++) {
            snprintf(b, sizeof b, "vae.encoder.down_blocks.%d.resnets.%d", blk, l);
            o = (cur + 1) & 3;
            ok &= fxq_resnet(g, b, B.v[cur], B.v[o], B.v[(cur + 2) & 3], B.v[(cur + 3) & 3], H, W, &C);
            cur = o;
        }
        if (ok && blk < 3) {                                            /* descida: preenchimento 0,1,0,1 (em cima, esquerda, embaixo, direita) */
            snprintf(b, sizeof b, "vae.encoder.down_blocks.%d.downsamplers.0.conv", blk);
            const FxqConv *c = fxq_conv(g, b);
            o = (cur + 1) & 3; int Hn = 0, Wn = 0;
            ok &= c && fxq_run_conv(c, B.v[cur], H, W, 2, 0, 0, 1, 1, 0, B.v[o], (KvkBuf){0}, &Hn, &Wn);
            cur = o; H = Hn; W = Wn;
        }
    }
    if (ok) ok &= fxq_mid(g, "vae.encoder.mid_block", &B, &cur, H, W, C);
    float *mean = NULL;
    if (ok) {
        o = (cur + 1) & 3;
        ok &= fxq_gn(g, "vae.encoder.conv_norm_out", B.v[cur], B.v[o], H * W, C, 1);
        ok &= fxq_conv_named(g, "vae.encoder.conv_out", B.v[o], H, W, 1, 1, 0, B.v[cur], (KvkBuf){0});
        ok &= fxq_conv_named(g, "vae.quant_conv", B.v[cur], H, W, 1, 0, 0, B.v[o], (KvkBuf){0});
        if (ok) {
            float *q = xalloc((size_t)H * W * 64, 4); mean = xalloc((size_t)H * W * 32, 4);
            ok = kvk_buf_read(B.v[o], 0, q, (size_t)H * W * 64 * 4);
            if (ok) for (int i = 0; i < H * W; i++) memcpy(mean + (size_t)i * 32, q + (size_t)i * 64, 32 * 4);
            free(q);
        }
    }
    if (!ok) { fprintf(stderr, "flux: codificador do VAE na GPU falhou (%s), CPU\n", kvk_error()); free(mean); return NULL; }
    fxq.t += sdt_now() - t0;
    *ho = H; *wo = W;
    return mean;
}
