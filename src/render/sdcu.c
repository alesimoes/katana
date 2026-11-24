/* ============================ render: Stable Diffusion 1.5 na GPU (CUDA) ===============================
 * UNet, ControlNet e VAE (codificador e decodificador) inteiros na VRAM, pesos em F16, ativacoes em F32 NHWC, produtos com mma f16 e acumulo
 * F32 (kernels de tools/cuda/sdkern.cu). Convolucao = GEMM implicito (sem im2col), com a norma por grupos + SiLU e a ampliacao 2x fundidas
 * no carregamento do ladrilho; atencao da UNet = flash (8 cabecas, sem matriz n x n); atencao do VAE (uma cabeca de 512) = S em blocos de linhas.
 * Os pesos sao carregados do GGUF sob demanda (a primeira passada) e ficam na VRAM; se UNet + ControlNet nao cabem juntos mas cada um cabe,
 * entra o modo "streaming": os pesos ja reordenados ficam na RAM e sobem a cada passada (UNet e ControlNet nao ficam juntos na VRAM).
 * Se nem isso cabe, ou algo falha, scu_* devolve 0 e a CPU (sdunet.c, sdvae.c) faz o trabalho, com a mesma conta.
 * Selecao: KATANA_BACKEND=cuda|vulkan|cpu (sem escolha: CUDA, depois Vulkan, depois CPU); KATANA_STABLE_CUDA=0 desliga so este caminho. */

/* g_sd_be_unet / g_sd_be_vae (em sdgen.c): quem processou de fato: 0 CPU, 1 CUDA, 2 Vulkan (linha "[stable] usado:") */
static int g_scu_be;        /* camada em uso: 1 CUDA, 2 Vulkan (render/sdvk.c) */

typedef struct { const SdgInfo *key; CUdptr_ w, b; uint16_t *host; int cin, cout, k, Kp, grp; } SdcW;
typedef struct { const SdgInfo *key; CUdptr_ p; int C, grp; } SdcV;
typedef struct { const SdgFile *g; int grp; } SdcM;          /* grp: 0 UNet, 1 ControlNet, 2 VAE */

static struct {
    int tried, ok, dead, stream, P, nctx;
    void *f_conv, *f_gn, *f_fl[3], *f_ln, *f_geglu, *f_cat, *f_add, *f_gemv, *f_sm, *f_tr, *f_cast;
    SdcW w[1024]; int nw;
    SdcV v[1024]; int nv;
    CUdptr_ x, y, t1, t2, t3, h, t, a, q16, pj, gg, kv16, ca, cb, act, tb, tb2, ctx, tin, eps, lat, cond, m1, m2, mres, sk[12], rs[12];
    int have_ws;
} g_sdc;

static int scu_cuda_init(void) {
    { const char *e = getenv("KATANA_STABLE_CUDA"); if (e && !atoi(e)) return 0; }
    { const char *be = getenv("KATANA_BACKEND"); if (be && *be && strcmp(be, "cuda")) return 0; }
    if (!cu_init()) return 0;
    void *m = cu_sdmod(); if (!m) return 0;
    g_sdc.f_conv = cu_fn(m, "sconv"); g_sdc.f_gn = cu_fn(m, "gn_coef");
    g_sdc.f_fl[0] = cu_fn(m, "flash40"); g_sdc.f_fl[1] = cu_fn(m, "flash80"); g_sdc.f_fl[2] = cu_fn(m, "flash160");
    g_sdc.f_ln = cu_fn(m, "lnorm"); g_sdc.f_geglu = cu_fn(m, "geglu"); g_sdc.f_cat = cu_fn(m, "cat2"); g_sdc.f_add = cu_fn(m, "addv");
    g_sdc.f_gemv = cu_fn(m, "gemv"); g_sdc.f_sm = cu_fn(m, "softmax_rows"); g_sdc.f_tr = cu_fn(m, "transp16"); g_sdc.f_cast = cu_fn(m, "cast16");
    if (!g_sdc.f_conv || !g_sdc.f_gn || !g_sdc.f_fl[0] || !g_sdc.f_fl[1] || !g_sdc.f_fl[2] || !g_sdc.f_ln || !g_sdc.f_geglu || !g_sdc.f_cat || !g_sdc.f_add || !g_sdc.f_gemv || !g_sdc.f_sm || !g_sdc.f_tr || !g_sdc.f_cast) return 0;
    cuapi.FuncSetAttribute(g_sdc.f_fl[2], 8, 43008);
    return 1;
}
/* escolhe a camada: KATANA_BACKEND=cuda|vulkan|cpu; sem escolha, CUDA e depois Vulkan; nenhuma = CPU */
static int scu_init(void) {
    if (g_sdc.tried) return g_sdc.ok;
    g_sdc.tried = 1;
    if (scu_cuda_init()) g_scu_be = 1;
    else if (svk_init()) g_scu_be = 2;
    g_sdc.ok = g_scu_be != 0;
    return g_sdc.ok;
}
static CUdptr_ scu_alloc(size_t n) { return g_scu_be == 2 ? svk_alloc(n) : cu_alloc(n); }
static void scu_free(CUdptr_ p) { if (g_scu_be == 2) svk_free(p); else cu_free(p); }
static int scu_up(CUdptr_ d, const void *h, size_t n) { return g_scu_be == 2 ? svk_up(d, h, n) : cu_up(d, h, n); }
static int scu_down(void *h, CUdptr_ d, size_t n) { return g_scu_be == 2 ? svk_down(h, d, n) : cu_down(h, d, n); }
static int scu_sync(void) { return g_scu_be == 2 ? svk_sync() : cu_sync(); }
static void scu_meminfo(size_t *fr) { if (g_scu_be == 2) *fr = svk_free_bytes(); else { size_t tot = 0; cuapi.MemGetInfo(fr, &tot); } }

/* ---------- pesos ---------- */
typedef struct { uint16_t *dst; const uint8_t *src; int type, cin, k, Kp; size_t row0; } SdcRo;
static void scu_ro_job(void *cx, int o0, int o1) {      /* linhas o0..o1 de uma parte: [o][i][ky][kx] -> [o][ky][kx][i] em F16, zeros ate Kp */
    const SdcRo *j = cx; const int K = j->k, cin = j->cin; const size_t es = j->type == 0 ? 4 : 2;
    for (int o = o0; o < o1; o++) {
        uint16_t *d = j->dst + (j->row0 + (size_t)o) * j->Kp;
        for (int i = 0; i < cin; i++)
            for (int ky = 0; ky < K; ky++)
                for (int kx = 0; kx < K; kx++) {
                    const uint8_t *s = j->src + ((((size_t)o * cin + i) * K + ky) * K + kx) * es;
                    uint16_t v;
                    if (j->type == 0) { float f; memcpy(&f, s, 4); v = _cvtss_sh(f, 0); } else memcpy(&v, s, 2);
                    d[((size_t)ky * K + kx) * cin + i] = v;
                }
    }
}
static int scu_up_w(SdcW *e) {
    const size_t n = (size_t)e->cout * e->Kp * 2;
    e->w = scu_alloc(n);
    if (!e->w) return 0;
    if (!scu_up(e->w, e->host, n)) { scu_free(e->w); e->w = 0; return 0; }
    return 1;
}
/* nb tensores <base>.weight empilhados nas linhas (q, k, v fundidos); bias: <base>.bias de cada um, empilhados */
static SdcW *scu_wget(const SdcM *m, int nb, const char *const *base, int bias) {
    char nm[260];
    snprintf(nm, sizeof nm, "%s.weight", base[0]);
    const SdgInfo *t0 = sdg_find(m->g, nm);
    if (!t0) { fprintf(stderr, "[stable] gpu: peso %s ausente\n", nm); return NULL; }
    for (int i = 0; i < g_sdc.nw; i++) {
        SdcW *e = &g_sdc.w[i];
        if (e->key == t0) { if (!e->w && !scu_up_w(e)) return NULL; return e; }
    }
    if (g_sdc.nw >= 1024) return NULL;
    const SdgInfo *ti[4]; int cin = 0, cout = 0, k = 1;
    for (int p = 0; p < nb; p++) {
        snprintf(nm, sizeof nm, "%s.weight", base[p]);
        ti[p] = sdg_find(m->g, nm);
        if (!ti[p] || ti[p]->type > 1) { fprintf(stderr, "[stable] gpu: peso %s ausente ou com tipo inesperado\n", nm); return NULL; }
        int ci, co, kk;
        if (ti[p]->nd == 4 && ti[p]->dim[0] == ti[p]->dim[1]) { kk = (int)ti[p]->dim[0]; ci = (int)ti[p]->dim[2]; co = (int)ti[p]->dim[3]; }
        else if (ti[p]->nd == 2) { kk = 1; ci = (int)ti[p]->dim[0]; co = (int)ti[p]->dim[1]; }
        else return NULL;
        if (p && (ci != cin || kk != k)) return NULL;
        cin = ci; k = kk; cout += co;
    }
    const int K = k * k * cin, Kp = (K + 31) / 32 * 32;
    SdcW *e = &g_sdc.w[g_sdc.nw];
    memset(e, 0, sizeof *e);
    e->key = t0; e->cin = cin; e->cout = cout; e->k = k; e->Kp = Kp; e->grp = m->grp;
    e->host = calloc((size_t)cout * Kp, 2);
    if (!e->host) return NULL;
    size_t row0 = 0;
    for (int p = 0; p < nb; p++) {
        const int co = ti[p]->nd == 4 ? (int)ti[p]->dim[3] : (int)ti[p]->dim[1];
        SdcRo j = { e->host, sdg_data(m->g, ti[p]), (int)ti[p]->type, cin, k, Kp, row0 };
        pool_for(scu_ro_job, &j, co, 8);
        row0 += (size_t)co;
    }
    if (bias) {
        float *hb = xalloc((size_t)cout, 4); size_t o = 0;
        for (int p = 0; p < nb; p++) {
            snprintf(nm, sizeof nm, "%s.bias", base[p]);
            const SdgInfo *b = sdg_find(m->g, nm);
            const int co = ti[p]->nd == 4 ? (int)ti[p]->dim[3] : (int)ti[p]->dim[1];
            if (!b || b->type != 0 || b->nd != 1 || (int)b->dim[0] != co) { fprintf(stderr, "[stable] gpu: vies %s ausente ou nao e F32\n", nm); free(hb); free(e->host); e->host = NULL; return NULL; }
            memcpy(hb + o, sdg_data(m->g, b), (size_t)co * 4); o += (size_t)co;
        }
        e->b = scu_alloc((size_t)cout * 4);
        const int ok = e->b && scu_up(e->b, hb, (size_t)cout * 4);
        free(hb);
        if (!ok) { scu_free(e->b); e->b = 0; free(e->host); e->host = NULL; return NULL; }
    }
    if (!scu_up_w(e)) { scu_free(e->b); e->b = 0; free(e->host); e->host = NULL; return NULL; }
    if (!g_sdc.stream) { free(e->host); e->host = NULL; }
    g_sdc.nw++;
    return e;
}
static SdcW *scu_w1(const SdcM *m, const char *base, int bias) { const char *b[1] = { base }; return scu_wget(m, 1, b, bias); }

/* ganho e vies (F32 [C] cada) de uma norma: um vetor 2C na VRAM */
static CUdptr_ scu_dvec(const SdcM *m, const char *base, int C) {
    char nm[260]; snprintf(nm, sizeof nm, "%s.weight", base);
    const SdgInfo *t = sdg_find(m->g, nm);
    if (!t) return 0;
    for (int i = 0; i < g_sdc.nv; i++) if (g_sdc.v[i].key == t) return g_sdc.v[i].p;
    if (g_sdc.nv >= 1024) return 0;
    int ok = 1;
    snprintf(nm, sizeof nm, "%s.weight", base); const float *gw = sdc_vec(m->g, nm, C, &ok);
    snprintf(nm, sizeof nm, "%s.bias", base); const float *gb = sdc_vec(m->g, nm, C, &ok);
    if (!ok) return 0;
    CUdptr_ p = scu_alloc((size_t)C * 8);
    if (!p || !scu_up(p, gw, (size_t)C * 4) || !scu_up(p + (size_t)C * 4, gb, (size_t)C * 4)) { scu_free(p); return 0; }
    g_sdc.v[g_sdc.nv++] = (SdcV){ t, p, C, m->grp };
    return p;
}

/* descarrega os pesos de um grupo da VRAM (keep_host: no modo streaming a copia reordenada fica na RAM); release: apaga tudo do grupo */
static void scu_unload(int grp, int release) {
    for (int i = 0; i < g_sdc.nw; i++) {
        SdcW *e = &g_sdc.w[i];
        if (!e->key || e->grp != grp) continue;
        scu_free(e->w); e->w = 0;
        if (release || !e->host) { scu_free(e->b); free(e->host); memset(e, 0, sizeof *e); }
    }
    if (release) for (int i = 0; i < g_sdc.nv; i++) if (g_sdc.v[i].key && g_sdc.v[i].grp == grp) { scu_free(g_sdc.v[i].p); memset(&g_sdc.v[i], 0, sizeof g_sdc.v[i]); }
}

/* ---------- lancamentos ---------- */
static int scu_conv(const SdcW *w, CUdptr_ X, CUdptr_ Y, CUdptr_ bias, CUdptr_ res, CUdptr_ ca, CUdptr_ cb, int silu, int H, int W, int up, int stride, int pt, int pl, int LH, int LW, float oscale, int out16) {
    const int Cin = w->cin, Cout = w->cout, ks = w->k, Kp = w->Kp; CUdptr_ wh = w->w;
    void *a[] = { &X, &wh, &bias, &res, &Y, &ca, &cb, &silu, &H, &W, (void *)&Cin, (void *)&Cout, (void *)&ks, &up, (void *)&Kp, &stride, &pt, &pl, &LH, &LW, &oscale, &out16 };
    if (g_scu_be == 2) return svk_conv(X, wh, bias, res, Y, ca, cb, silu, H, W, Cin, Cout, ks, up, Kp, stride, pt, pl, LH, LW, oscale, out16);
    return cu_launch(g_sdc.f_conv, (Cout + 127) / 128, (H * W + 127) / 128, 1, 256, 16384, a);
}
/* convolucao padrao: passo 1, preenchimento k/2, saida F32, vies do peso */
static int scu_cv(const SdcW *w, CUdptr_ X, CUdptr_ Y, CUdptr_ res, CUdptr_ ca, CUdptr_ cb, int silu, int H, int W) {
    return scu_conv(w, X, Y, w->b, res, ca, cb, silu, H, W, 0, 1, w->k / 2, w->k / 2, H, W, 1.0f, 0);
}
static int scu_gn(const SdcM *m, const char *base, CUdptr_ X, int n, int C, float eps) {
    CUdptr_ v = scu_dvec(m, base, C); if (!v) return 0;
    CUdptr_ be = v + (size_t)C * 4, ca = g_sdc.ca, cb = g_sdc.cb;
    void *a[] = { &X, &n, &C, &v, &be, &eps, &ca, &cb };
    if (g_scu_be == 2) return svk_gn(X, n, C, v, be, eps, ca, cb);
    return cu_launch(g_sdc.f_gn, 32, 1, 1, 256, 0, a);
}
static int scu_ln(const SdcM *m, const char *base, CUdptr_ X, CUdptr_ Y, int n, int C) {
    CUdptr_ v = scu_dvec(m, base, C); if (!v) return 0;
    CUdptr_ be = v + (size_t)C * 4; float eps = 1e-5f;
    void *a[] = { &X, &Y, &v, &be, &C, &eps };
    if (g_scu_be == 2) return svk_ln(X, Y, v, be, n, C, eps);
    return cu_launch(g_sdc.f_ln, n, 1, 1, 256, 0, a);
}
static int scu_gemv(const SdcW *w, CUdptr_ x, CUdptr_ b2, CUdptr_ y, int silu) {
    CUdptr_ wh = w->w, b1 = w->b; int Kp = w->Kp, nout = w->cout, K = w->cin;
    void *a[] = { &wh, &Kp, &x, &b1, &b2, &y, &nout, &K, &silu };
    if (g_scu_be == 2) return svk_gemv(wh, x, b1, b2, y, Kp, nout, K, silu);
    return cu_launch(g_sdc.f_gemv, (nout + 3) / 4, 1, 1, 128, 0, a);
}
static int scu_geglu(CUdptr_ pj, CUdptr_ gg, int n, int F) {
    if (g_scu_be == 2) return svk_geglu(pj, gg, n, F);
    void *ga[] = { &pj, &gg, &F };
    return cu_launch(g_sdc.f_geglu, n, 1, 1, 256, 0, ga);
}
static int scu_softmax(CUdptr_ S, int nr, int nc, float sc) {
    if (g_scu_be == 2) return svk_softmax(S, nr, nc, sc);
    void *a[] = { &S, &nc, &sc };
    return cu_launch(g_sdc.f_sm, nr, 1, 1, 256, 0, a);
}
static int scu_transp(CUdptr_ in, CUdptr_ out, int n, int C, int ld) {
    if (g_scu_be == 2) return svk_transp(in, out, n, C, ld);
    void *a[] = { &in, &out, &n, &C, &ld };
    return cu_launch(g_sdc.f_tr, (n + 31) / 32, (C + 31) / 32, 1, 256, 0, a);
}
static int scu_cat(CUdptr_ A, CUdptr_ B, CUdptr_ Br, CUdptr_ Y, int n, int Ca, int Cb) {
    void *a[] = { &A, &B, &Br, &Y, &Ca, &Cb };
    if (g_scu_be == 2) return svk_cat(A, B, Br, Y, n, Ca, Cb);
    return cu_launch(g_sdc.f_cat, n, 1, 1, 256, 0, a);
}
static int scu_addv(CUdptr_ Y, CUdptr_ X, long n) {
    void *a[] = { &Y, &X, &n };
    if (g_scu_be == 2) return svk_add(Y, X, n);
    return cu_launch(g_sdc.f_add, (int)((n + 255) / 256 > 4096 ? 4096 : (n + 255) / 256), 1, 1, 256, 0, a);
}
static int scu_flash(CUdptr_ Q, int ldq, CUdptr_ K, int ldk, CUdptr_ V, int ldv, CUdptr_ O, int nq, int nk, int C) {
    const int hd = C / 8, ix = hd == 40 ? 0 : hd == 80 ? 1 : hd == 160 ? 2 : -1;
    if (ix < 0) return 0;
    const int smem = ix == 0 ? 14336 : ix == 1 ? 22528 : 43008, ldo = C;
    const float sl2 = 1.4426950408889634f / sqrtf((float)hd);
    void *a[] = { &Q, &ldq, &K, &ldk, &V, &ldv, &O, &ldo, &nq, &nk, (void *)&sl2 };
    if (g_scu_be == 2) return svk_flash(Q, ldq, K, ldk, V, ldv, O, ldo, nq, nk, C, sl2);
    return cu_launch(g_sdc.f_fl[ix], (nq + 63) / 64, 8, 1, 128, smem, a);
}

/* ---------- area de trabalho ---------- */
static void scu_ws_free(void) {
    CUdptr_ *p[] = { &g_sdc.x, &g_sdc.y, &g_sdc.t1, &g_sdc.t2, &g_sdc.t3, &g_sdc.h, &g_sdc.t, &g_sdc.a, &g_sdc.q16, &g_sdc.pj, &g_sdc.gg, &g_sdc.kv16, &g_sdc.ca, &g_sdc.cb,
        &g_sdc.act, &g_sdc.tb, &g_sdc.tb2, &g_sdc.ctx, &g_sdc.tin, &g_sdc.eps, &g_sdc.lat, &g_sdc.cond, &g_sdc.m1, &g_sdc.m2, &g_sdc.mres };
    for (size_t i = 0; i < sizeof p / sizeof *p; i++) { scu_free(*p[i]); *p[i] = 0; }
    for (int i = 0; i < 12; i++) { scu_free(g_sdc.sk[i]); scu_free(g_sdc.rs[i]); g_sdc.sk[i] = g_sdc.rs[i] = 0; }
    g_sdc.have_ws = 0;
}
static int scu_ws_alloc(int P, int nctx) {
    if (g_sdc.have_ws && g_sdc.P == P && g_sdc.nctx >= nctx) return 1;
    scu_ws_free();
    const size_t p = (size_t)P;
    CUdptr_ *q[] = { &g_sdc.t1, &g_sdc.t2, &g_sdc.h, &g_sdc.t, &g_sdc.a, &g_sdc.cond };
    if (!(g_sdc.x = scu_alloc(p * 640 * 4)) || !(g_sdc.y = scu_alloc(p * 640 * 4))) return 0;
    for (int i = 0; i < 6; i++) if (!(*q[i] = scu_alloc(p * 320 * 4))) return 0;
    if (!(g_sdc.t3 = scu_alloc(p * 960 * 4)) || !(g_sdc.q16 = scu_alloc(p * 960 * 2)) || !(g_sdc.pj = scu_alloc(p * 2560 * 4)) || !(g_sdc.gg = scu_alloc(p * 1280 * 4))) return 0;
    if (!(g_sdc.kv16 = scu_alloc((size_t)nctx * 2560 * 2)) || !(g_sdc.ctx = scu_alloc((size_t)nctx * 768 * 4))) return 0;
    if (!(g_sdc.ca = scu_alloc(4096 * 4)) || !(g_sdc.cb = scu_alloc(4096 * 4)) || !(g_sdc.act = scu_alloc(1280 * 4)) || !(g_sdc.tb = scu_alloc(1280 * 4)) || !(g_sdc.tb2 = scu_alloc(1280 * 4)) || !(g_sdc.tin = scu_alloc(320 * 4))) return 0;
    if (!(g_sdc.eps = scu_alloc(p * 4 * 4)) || !(g_sdc.lat = scu_alloc(p * 4 * 4))) return 0;
    const size_t skn[12] = { p * 320, p * 320, p * 320, p / 4 * 320, p / 4 * 640, p / 4 * 640, p / 16 * 640, p / 16 * 1280, p / 16 * 1280, p / 64 * 1280, p / 64 * 1280, p / 64 * 1280 };
    for (int i = 0; i < 12; i++) if (!(g_sdc.sk[i] = scu_alloc(skn[i] * 4)) || !(g_sdc.rs[i] = scu_alloc(skn[i] * 4))) return 0;
    if (!(g_sdc.m1 = scu_alloc(p / 64 * 1280 * 4)) || !(g_sdc.m2 = scu_alloc(p / 64 * 1280 * 4)) || !(g_sdc.mres = scu_alloc(p / 64 * 1280 * 4))) return 0;
    g_sdc.P = P; g_sdc.nctx = nctx; g_sdc.have_ws = 1;
    return 1;
}
static size_t scu_ws_bytes(int P, int nctx) { return (size_t)P * (6 * 320 + 2 * 640 + 960 + 2560 + 1280 + 1620 * 2 + 16 + 20 * 3) * 4 + (size_t)P * 960 * 2 + (size_t)nctx * 3328 * 4 + ((size_t)64 << 20); }

/* ---------- camadas ---------- */
/* resnet: x[H*W][Cin] -> out[H*W][Cout]; temb: usa g_sdc.act (SiLU do passo); sem temb e o do VAE (eps 1e-6) */
static int scu_resnet(const SdcM *m, const char *pfx, CUdptr_ x, CUdptr_ out, int H, int W, int temb, int *Cout_o) {
    char b[240]; const int n = H * W; const float eps = temb ? 1e-5f : 1e-6f;
    snprintf(b, sizeof b, "%s.conv1", pfx); const SdcW *c1 = scu_w1(m, b, 1);
    snprintf(b, sizeof b, "%s.conv2", pfx); const SdcW *c2 = scu_w1(m, b, 1);
    if (!c1 || !c2) return 0;
    const int Cin = c1->cin, Cout = c1->cout;
    CUdptr_ bias1 = c1->b;
    if (temb) {
        snprintf(b, sizeof b, "%s.time_emb_proj", pfx); const SdcW *tp = scu_w1(m, b, 1);
        if (!tp || !scu_gemv(tp, g_sdc.act, c1->b, g_sdc.tb, 0)) return 0;
        bias1 = g_sdc.tb;
    }
    snprintf(b, sizeof b, "%s.norm1", pfx); if (!scu_gn(m, b, x, n, Cin, eps)) return 0;
    if (!scu_conv(c1, x, g_sdc.t1, bias1, 0, g_sdc.ca, g_sdc.cb, 1, H, W, 0, 1, 1, 1, H, W, 1.0f, 0)) return 0;
    snprintf(b, sizeof b, "%s.norm2", pfx); if (!scu_gn(m, b, g_sdc.t1, n, Cout, eps)) return 0;
    CUdptr_ res = x;
    snprintf(b, sizeof b, "%s.conv_shortcut.weight", pfx);
    if (sdg_find(m->g, b)) {
        snprintf(b, sizeof b, "%s.conv_shortcut", pfx); const SdcW *cs = scu_w1(m, b, 1);
        if (!cs || !scu_cv(cs, x, g_sdc.t2, 0, 0, 0, 0, H, W)) return 0;
        res = g_sdc.t2;
    } else if (Cin != Cout) return 0;
    if (!scu_cv(c2, g_sdc.t1, out, res, g_sdc.ca, g_sdc.cb, 1, H, W)) return 0;
    *Cout_o = Cout;
    return 1;
}

/* bloco de atencao da UNet (Transformer2D) no lugar: x[H*W][C]; contexto em g_sdc.ctx [nctx][768] */
static int scu_transformer(const SdcM *m, const char *pfx, CUdptr_ x, int H, int W, int C, int nctx) {
    char b[240], tb[220]; const int n = H * W;
    const SdcW *pin, *pout;
    snprintf(b, sizeof b, "%s.norm", pfx); if (!scu_gn(m, b, x, n, C, 1e-6f)) return 0;
    snprintf(b, sizeof b, "%s.proj_in", pfx); pin = scu_w1(m, b, 1);
    snprintf(b, sizeof b, "%s.proj_out", pfx); pout = scu_w1(m, b, 1);
    if (!pin || !pout || !scu_cv(pin, x, g_sdc.h, 0, g_sdc.ca, g_sdc.cb, 0, n, 1)) return 0;
    snprintf(tb, sizeof tb, "%s.transformer_blocks.0", pfx);
    CUdptr_ h = g_sdc.h, t = g_sdc.t, a = g_sdc.a, qkv = g_sdc.q16;
    {   /* autoatencao */
        char q[240], k[240], v[240];
        snprintf(q, sizeof q, "%s.attn1.to_q", tb); snprintf(k, sizeof k, "%s.attn1.to_k", tb); snprintf(v, sizeof v, "%s.attn1.to_v", tb);
        const char *bs[3] = { q, k, v };
        const SdcW *wqkv = scu_wget(m, 3, bs, 0);
        snprintf(b, sizeof b, "%s.attn1.to_out.0", tb); const SdcW *wo = scu_w1(m, b, 1);
        if (!wqkv || !wo) return 0;
        snprintf(b, sizeof b, "%s.norm1", tb); if (!scu_ln(m, b, h, t, n, C)) return 0;
        if (!scu_conv(wqkv, t, qkv, 0, 0, 0, 0, 0, n, 1, 0, 1, 0, 0, n, 1, 1.0f, 1)) return 0;
        if (!scu_flash(qkv, 3 * C, qkv + (size_t)C * 2, 3 * C, qkv + (size_t)C * 4, 3 * C, a, n, n, C)) return 0;
        if (!scu_conv(wo, a, h, wo->b, h, 0, 0, 0, n, 1, 0, 1, 0, 0, n, 1, 1.0f, 0)) return 0;
    }
    {   /* atencao ao texto */
        char k[240], v[240]; snprintf(k, sizeof k, "%s.attn2.to_k", tb); snprintf(v, sizeof v, "%s.attn2.to_v", tb);
        const char *bs[2] = { k, v };
        const SdcW *wkv = scu_wget(m, 2, bs, 0);
        snprintf(b, sizeof b, "%s.attn2.to_q", tb); const SdcW *wq = scu_w1(m, b, 0);
        snprintf(b, sizeof b, "%s.attn2.to_out.0", tb); const SdcW *wo = scu_w1(m, b, 1);
        if (!wkv || !wq || !wo) return 0;
        snprintf(b, sizeof b, "%s.norm2", tb); if (!scu_ln(m, b, h, t, n, C)) return 0;
        if (!scu_conv(wq, t, qkv, 0, 0, 0, 0, 0, n, 1, 0, 1, 0, 0, n, 1, 1.0f, 1)) return 0;
        if (!scu_conv(wkv, g_sdc.ctx, g_sdc.kv16, 0, 0, 0, 0, 0, nctx, 1, 0, 1, 0, 0, nctx, 1, 1.0f, 1)) return 0;
        if (!scu_flash(qkv, C, g_sdc.kv16, 2 * C, g_sdc.kv16 + (size_t)C * 2, 2 * C, a, n, nctx, C)) return 0;
        if (!scu_conv(wo, a, h, wo->b, h, 0, 0, 0, n, 1, 0, 1, 0, 0, n, 1, 1.0f, 0)) return 0;
    }
    {   /* GEGLU */
        snprintf(b, sizeof b, "%s.ff.net.0.proj", tb); const SdcW *w1 = scu_w1(m, b, 1);
        snprintf(b, sizeof b, "%s.ff.net.2", tb); const SdcW *w2 = scu_w1(m, b, 1);
        if (!w1 || !w2) return 0;
        const int F = w1->cout / 2;
        snprintf(b, sizeof b, "%s.norm3", tb); if (!scu_ln(m, b, h, t, n, C)) return 0;
        if (!scu_conv(w1, t, g_sdc.pj, w1->b, 0, 0, 0, 0, n, 1, 0, 1, 0, 0, n, 1, 1.0f, 0)) return 0;
        if (!scu_geglu(g_sdc.pj, g_sdc.gg, n, F)) return 0;
        if (!scu_conv(w2, g_sdc.gg, h, w2->b, h, 0, 0, 0, n, 1, 0, 1, 0, 0, n, 1, 1.0f, 0)) return 0;
    }
    return scu_cv(pout, h, x, x, 0, 0, 0, H, W);
}

/* emb do passo: SiLU(linear_2(SiLU(linear_1(sin/cos)))) em g_sdc.act */
static int scu_temb(const SdcM *m, const char *pfx, float t) {
    float sc[320]; char b[220];
    for (int i = 0; i < 160; i++) { const float a = t * expf(-logf(10000.0f) * (float)i / 160.0f); sc[i] = cosf(a); sc[160 + i] = sinf(a); }
    if (!scu_up(g_sdc.tin, sc, sizeof sc)) return 0;
    snprintf(b, sizeof b, "%s.time_embedding.linear_1", pfx); const SdcW *l1 = scu_w1(m, b, 1);
    snprintf(b, sizeof b, "%s.time_embedding.linear_2", pfx); const SdcW *l2 = scu_w1(m, b, 1);
    return l1 && l2 && scu_gemv(l1, g_sdc.tin, 0, g_sdc.tb2, 1) && scu_gemv(l2, g_sdc.tb2, 0, g_sdc.act, 1);
}

/* descida e meio; sk[0] ja tem o conv_in. O meio sai em g_sdc.m2 */
static int scu_encoder(const SdcM *m, const char *pfx, int H0, int W0, int nctx) {
    int H = H0, W = W0, C = 320, idx = 1; char b[220];
    CUdptr_ cur = g_sdc.sk[0];
    for (int blk = 0; blk < 4; blk++) {
        for (int l = 0; l < 2; l++) {
            int Co; CUdptr_ out = g_sdc.sk[idx++];
            snprintf(b, sizeof b, "%s.down_blocks.%d.resnets.%d", pfx, blk, l);
            if (!scu_resnet(m, b, cur, out, H, W, 1, &Co)) return 0;
            C = Co; cur = out;
            if (blk < 3) { snprintf(b, sizeof b, "%s.down_blocks.%d.attentions.%d", pfx, blk, l); if (!scu_transformer(m, b, cur, H, W, C, nctx)) return 0; }
        }
        if (blk < 3) {
            const int Ho = (H + 2 - 3) / 2 + 1, Wo = (W + 2 - 3) / 2 + 1;
            snprintf(b, sizeof b, "%s.down_blocks.%d.downsamplers.0.conv", pfx, blk); const SdcW *dw = scu_w1(m, b, 1);
            CUdptr_ out = g_sdc.sk[idx++];
            if (!dw || !scu_conv(dw, cur, out, dw->b, 0, 0, 0, 0, Ho, Wo, 0, 2, 1, 1, H, W, 1.0f, 0)) return 0;
            cur = out; H = Ho; W = Wo;
        }
    }
    int Co;
    snprintf(b, sizeof b, "%s.mid_block.resnets.0", pfx); if (!scu_resnet(m, b, cur, g_sdc.m1, H, W, 1, &Co)) return 0;
    snprintf(b, sizeof b, "%s.mid_block.attentions.0", pfx); if (!scu_transformer(m, b, g_sdc.m1, H, W, C, nctx)) return 0;
    snprintf(b, sizeof b, "%s.mid_block.resnets.1", pfx); return scu_resnet(m, b, g_sdc.m1, g_sdc.m2, H, W, 1, &Co);
}

static int scu_load_ctx(const float *ctx, int nctx) { return scu_up(g_sdc.ctx, ctx, (size_t)nctx * 768 * 4); }

/* ControlNet: lat ja em g_sdc.lat; residuos em rs[12] e mres */
static int scu_control(const SdgFile *ctrl, int H, int W, float t, float scale, int nctx) {
    const SdcM m = { ctrl, 1 }; char b[220];
    if (!scu_temb(&m, "ctrl", t)) return 0;
    const SdcW *ci = scu_w1(&m, "ctrl.conv_in", 1);
    if (!ci || !scu_cv(ci, g_sdc.lat, g_sdc.sk[0], g_sdc.cond, 0, 0, 0, H, W)) return 0;
    if (!scu_encoder(&m, "ctrl", H, W, nctx)) return 0;
    static const int dh[12] = { 1, 1, 1, 2, 2, 2, 4, 4, 4, 8, 8, 8 };
    for (int i = 0; i < 12; i++) {
        snprintf(b, sizeof b, "ctrl.controlnet_down_blocks.%d", i); const SdcW *z = scu_w1(&m, b, 1);
        if (!z || !scu_conv(z, g_sdc.sk[i], g_sdc.rs[i], z->b, 0, 0, 0, 0, H / dh[i], W / dh[i], 0, 1, 0, 0, H / dh[i], W / dh[i], scale, 0)) return 0;
    }
    const SdcW *z = scu_w1(&m, "ctrl.controlnet_mid_block", 1);
    return z && scu_conv(z, g_sdc.m2, g_sdc.mres, z->b, 0, 0, 0, 0, H / 8, W / 8, 0, 1, 0, 0, H / 8, W / 8, scale, 0);
}

/* UNet: lat em g_sdc.lat; eps em g_sdc.eps */
static int scu_unet(const SdgFile *g, int H0, int W0, float t, int nctx, int use_res) {
    const SdcM m = { g, 0 }; char b[220];
    if (!scu_temb(&m, "unet", t)) return 0;
    const SdcW *ci = scu_w1(&m, "unet.conv_in", 1);
    if (!ci || !scu_cv(ci, g_sdc.lat, g_sdc.sk[0], 0, 0, 0, 0, H0, W0)) return 0;
    if (!scu_encoder(&m, "unet", H0, W0, nctx)) return 0;
    int H = H0 / 8, W = W0 / 8, C = 1280, sn = 12;
    if (use_res && !scu_addv(g_sdc.m2, g_sdc.mres, (long)H * W * 1280)) return 0;
    static const int skc[12] = { 320, 320, 320, 320, 640, 640, 640, 1280, 1280, 1280, 1280, 1280 };
    CUdptr_ cur = g_sdc.m2, outs[2] = { g_sdc.x, g_sdc.y }; int oi = 0;
    for (int blk = 0; blk < 4; blk++) {
        for (int l = 0; l < 3; l++) {
            sn--;
            const int sc = skc[sn];
            if (!scu_cat(cur, g_sdc.sk[sn], use_res ? g_sdc.rs[sn] : 0, g_sdc.t3, H * W, C, sc)) return 0;
            int Co; CUdptr_ out = outs[oi]; oi ^= 1;
            snprintf(b, sizeof b, "unet.up_blocks.%d.resnets.%d", blk, l);
            if (!scu_resnet(&m, b, g_sdc.t3, out, H, W, 1, &Co)) return 0;
            C = Co; cur = out;
            if (blk > 0) { snprintf(b, sizeof b, "unet.up_blocks.%d.attentions.%d", blk, l); if (!scu_transformer(&m, b, cur, H, W, C, nctx)) return 0; }
        }
        if (blk < 3) {
            snprintf(b, sizeof b, "unet.up_blocks.%d.upsamplers.0.conv", blk); const SdcW *uw = scu_w1(&m, b, 1);
            CUdptr_ out = outs[oi]; oi ^= 1;
            if (!uw || !scu_conv(uw, cur, out, uw->b, 0, 0, 0, 0, 2 * H, 2 * W, 1, 1, 1, 1, 2 * H, 2 * W, 1.0f, 0)) return 0;
            cur = out; H *= 2; W *= 2;
        }
    }
    if (!scu_gn(&m, "unet.conv_norm_out", cur, H * W, C, 1e-5f)) return 0;
    const SdcW *co = scu_w1(&m, "unet.conv_out", 1);
    return co && scu_cv(co, cur, g_sdc.eps, 0, g_sdc.ca, g_sdc.cb, 1, H, W);
}

/* ---------- sessao ---------- */
static size_t scu_group_bytes(const SdgFile *g, const char *pfx) {
    size_t n = 0; const size_t l = strlen(pfx);
    for (int i = 0; i < g->n; i++) if (!strncmp(g->t[i].name, pfx, l)) n += (size_t)sdg_elems(&g->t[i]) * 2;
    return n;
}
/* 1 = a GPU pode rodar UNet (+ ControlNet) com este tamanho (residente ou em streaming); define g_sdc.stream */
static int scu_plan(const SdgFile *base, const SdgFile *ctrl, int h8, int w8, int nctx) {
    if (g_sdc.dead || !scu_init()) return 0;
    if (h8 % 8 || w8 % 8) return 0;
    size_t fr = 0; scu_meminfo(&fr);
    { const char *cap = getenv("KATANA_STABLE_VRAM_MB"); if (cap && (size_t)atoll(cap) << 20 < fr) fr = (size_t)atoll(cap) << 20; }      /* teste: finge menos VRAM livre */
    const size_t wu = scu_group_bytes(base, "unet."), wc = ctrl ? scu_group_bytes(ctrl, "ctrl.") : 0, ws = scu_ws_bytes(h8 * w8, nctx), lim = fr / 100 * 96;
    if (wu + wc + ws <= lim) g_sdc.stream = 0;
    else if ((wu > wc ? wu : wc) + ws <= lim) g_sdc.stream = 1;
    else { fprintf(stderr, "[stable] gpu: VRAM livre %zu MB nao basta (UNet %zu MB, ControlNet %zu MB, ativacoes %zu MB); usando a CPU\n", fr >> 20, wu >> 20, wc >> 20, ws >> 20); return 0; }
    if (g_sdc.stream) fprintf(stderr, "[stable] gpu: pesos em streaming (UNet e ControlNet nao cabem juntos)\n");
    return 1;
}
static void scu_abort(const char *what) {
    fprintf(stderr, "[stable] gpu: %s falhou; usando a CPU\n", what);
    g_sdc.dead = 1; g_sdc.stream = 0;
    scu_sync(); scu_unload(0, 1); scu_unload(1, 1); scu_unload(2, 1); scu_ws_free();
}

/* uma passada (ControlNet opcional + UNet): lat[h8*w8][4], ctx[nctx][768], eps[h8*w8][4]; 0 = nao deu (a CPU faz) */
static int scu_eps(const SdgFile *base, const SdgFile *ctrl, const float *lat, int h8, int w8, float t, const float *ctx, int nctx, float scale, float *eps) {
    if (g_sdc.dead || !scu_init()) return 0;
    const int P = h8 * w8;
    if (!scu_ws_alloc(P, nctx)) { scu_abort("alocar as ativacoes"); return 0; }
    if (!scu_up(g_sdc.lat, lat, (size_t)P * 16) || !scu_load_ctx(ctx, nctx)) { scu_abort("subir a entrada"); return 0; }
    if (ctrl) {
        if (!scu_control(ctrl, h8, w8, t, scale, nctx)) { scu_abort("o ControlNet"); return 0; }
        if (g_sdc.stream) { scu_sync(); scu_unload(1, 0); }
    }
    if (!scu_unet(base, h8, w8, t, nctx, ctrl != NULL)) { scu_abort("a UNet"); return 0; }
    if (!scu_sync() || !scu_down(eps, g_sdc.eps, (size_t)P * 16)) { scu_abort("a UNet"); return 0; }
    if (g_sdc.stream) scu_unload(0, 0);
    g_sd_be_unet = g_scu_be;
    return 1;
}

/* imagem de controle (bordas, [H*W][3]) -> g_sdc.cond [H/8*W/8][320]; 0 = nao deu */
static int scu_cond(const SdgFile *ctrl, const float *img, int H, int W, int nctx) {
    if (g_sdc.dead || !scu_init() || !scu_ws_alloc((H / 8) * (W / 8), nctx)) return 0;
    static const char *const nm[8] = { "ctrl.controlnet_cond_embedding.conv_in", "ctrl.controlnet_cond_embedding.blocks.0", "ctrl.controlnet_cond_embedding.blocks.1",
        "ctrl.controlnet_cond_embedding.blocks.2", "ctrl.controlnet_cond_embedding.blocks.3", "ctrl.controlnet_cond_embedding.blocks.4",
        "ctrl.controlnet_cond_embedding.blocks.5", "ctrl.controlnet_cond_embedding.conv_out" };
    static const int stride[8] = { 1, 1, 2, 1, 2, 1, 2, 1 };
    const SdcM m = { ctrl, 1 };
    const size_t big = (size_t)H * W * 16 * 4;
    CUdptr_ a = scu_alloc((size_t)H * W * 12 > big ? (size_t)H * W * 12 : big), b = scu_alloc(big);
    int ok = a && b && scu_up(a, img, (size_t)H * W * 12);
    int h = H, w = W;
    CUdptr_ cur = a, nxt = b;
    for (int i = 0; ok && i < 8; i++) {
        const SdcW *c = scu_w1(&m, nm[i], 1);
        if (!c) { ok = 0; break; }
        const int ho = (h + 2 - 3) / stride[i] + 1, wo = (w + 2 - 3) / stride[i] + 1;
        CUdptr_ out = i == 7 ? g_sdc.cond : nxt;
        ok = scu_conv(c, cur, out, c->b, 0, 0, 0, i > 0, ho, wo, 0, stride[i], 1, 1, h, w, 1.0f, 0);
        if (i < 7) { CUdptr_ t = cur; cur = nxt; nxt = t; }
        h = ho; w = wo;
    }
    if (ok) ok = scu_sync();
    scu_free(a); scu_free(b);
    scu_unload(1, 0);                      /* sem streaming isto so descarrega o que nao sera mais usado: os pesos do cond embedding */
    if (!ok) { scu_abort("o embedding da imagem de controle"); return 0; }
    return 1;
}

/* ---------- VAE ---------- */
/* atencao do meio (uma cabeca de C sobre n pixels): x += out(softmax(q k^T / sqrt(C)) v); x [n][C] no lugar */
static int scu_vae_attn(const SdcM *m, const char *pfx, CUdptr_ x, int n, int C) {
    char b[240];
    snprintf(b, sizeof b, "%s.group_norm", pfx); if (!scu_gn(m, b, x, n, C, 1e-6f)) return 0;
    snprintf(b, sizeof b, "%s.to_q", pfx); const SdcW *wq = scu_w1(m, b, 1);
    snprintf(b, sizeof b, "%s.to_k", pfx); const SdcW *wk = scu_w1(m, b, 1);
    snprintf(b, sizeof b, "%s.to_v", pfx); const SdcW *wv = scu_w1(m, b, 1);
    snprintf(b, sizeof b, "%s.to_out.0", pfx); const SdcW *wo = scu_w1(m, b, 1);
    if (!wq || !wk || !wv || !wo) return 0;
    const int Kp = (n + 31) / 32 * 32;
    const int QB = (int)(((size_t)64 << 20) / ((size_t)n * 4)) / 128 * 128 > 128 ? (int)(((size_t)64 << 20) / ((size_t)n * 4)) / 128 * 128 : 128;
    CUdptr_ q = scu_alloc((size_t)n * C * 4), v = scu_alloc((size_t)n * C * 4), k16 = scu_alloc((size_t)n * C * 2), vt = scu_alloc((size_t)C * Kp * 2), o = scu_alloc((size_t)n * C * 4), S = scu_alloc((size_t)QB * n * 4);
    int ok = q && v && k16 && vt && o && S;
    if (ok) ok = scu_conv(wq, x, q, wq->b, 0, g_sdc.ca, g_sdc.cb, 0, n, 1, 0, 1, 0, 0, n, 1, 1.0f, 0) && scu_conv(wk, x, k16, wk->b, 0, g_sdc.ca, g_sdc.cb, 0, n, 1, 0, 1, 0, 0, n, 1, 1.0f, 1)
              && scu_conv(wv, x, v, wv->b, 0, g_sdc.ca, g_sdc.cb, 0, n, 1, 0, 1, 0, 0, n, 1, 1.0f, 0);
    if (ok) ok = scu_transp(v, vt, n, C, Kp);
    /* S e V^T como "pesos" de dois produtos: S = Q K^T (Cout = n chaves, Kp = C), O = S V (Cin = n, Cout = C) */
    SdcW kw = { 0, k16, 0, 0, C, n, 1, C, 2 }, vw = { 0, vt, 0, 0, n, C, 1, Kp, 2 };
    for (int q0 = 0; ok && q0 < n; q0 += QB) {
        const int qb = q0 + QB > n ? n - q0 : QB;
        ok = scu_conv(&kw, q + (size_t)q0 * C * 4, S, 0, 0, 0, 0, 0, qb, 1, 0, 1, 0, 0, qb, 1, 1.0f, 0);
        if (ok) ok = scu_softmax(S, qb, n, 1.0f / sqrtf((float)C));
        if (ok) ok = scu_conv(&vw, S, o + (size_t)q0 * C * 4, 0, 0, 0, 0, 0, qb, 1, 0, 1, 0, 0, qb, 1, 1.0f, 0);
    }
    if (ok) ok = scu_conv(wo, o, x, wo->b, x, 0, 0, 0, n, 1, 0, 1, 0, 0, n, 1, 1.0f, 0);
    if (ok) ok = scu_sync();
    scu_free(q); scu_free(v); scu_free(k16); scu_free(vt); scu_free(o); scu_free(S);
    return ok;
}
typedef struct { CUdptr_ x, y, t1, t2; } SdcVb;
static int scu_vae_resnet(const SdcM *m, const char *pfx, SdcVb *vb, CUdptr_ x, CUdptr_ out, int H, int W, int *Co) {
    /* o resnet usa g_sdc.t1/t2 como temporarios: aponta-os para os do VAE durante a chamada */
    const CUdptr_ s1 = g_sdc.t1, s2 = g_sdc.t2; g_sdc.t1 = vb->t1; g_sdc.t2 = vb->t2;
    const int ok = scu_resnet(m, pfx, x, out, H, W, 0, Co);
    g_sdc.t1 = s1; g_sdc.t2 = s2;
    return ok;
}
static int scu_vae_mid(const SdcM *m, const char *pfx, SdcVb *vb, CUdptr_ *x, CUdptr_ *y, int H, int W, int C) {
    char b[220]; int Co;
    snprintf(b, sizeof b, "%s.resnets.0", pfx); if (!scu_vae_resnet(m, b, vb, *x, *y, H, W, &Co)) return 0;
    { CUdptr_ t = *x; *x = *y; *y = t; }
    snprintf(b, sizeof b, "%s.attentions.0", pfx); if (!scu_vae_attn(m, b, *x, H * W, C)) return 0;
    snprintf(b, sizeof b, "%s.resnets.1", pfx); if (!scu_vae_resnet(m, b, vb, *x, *y, H, W, &Co)) return 0;
    { CUdptr_ t = *x; *x = *y; *y = t; }
    return 1;
}
static int scu_vae_bufs(SdcVb *vb, size_t nx, size_t nt) {
    vb->x = scu_alloc(nx * 4); vb->y = scu_alloc(nx * 4); vb->t1 = scu_alloc(nt * 4); vb->t2 = scu_alloc(nt * 4);
    return vb->x && vb->y && vb->t1 && vb->t2;
}
static void scu_vae_free(SdcVb *vb) { scu_free(vb->x); scu_free(vb->y); scu_free(vb->t1); scu_free(vb->t2); }
static int scu_vae_pre(void) {          /* ca/cb dos GroupNorms; libera a area da UNet (o VAE roda depois do laco) */
    if (!g_sdc.ca) { g_sdc.ca = scu_alloc(4096 * 4); g_sdc.cb = scu_alloc(4096 * 4); }
    return g_sdc.ca && g_sdc.cb;
}

/* lat[h*w][4] (ja dividido pelo fator de escala) -> imagem [8h*8w][3] (memoria nova) ou NULL */
static float *scu_vae_decode(const SdgFile *g, const float *lat, int h, int w, int *Ho, int *Wo) {
    if (g_sdc.dead || !scu_init()) return NULL;
    scu_unload(0, 1); scu_unload(1, 1); scu_ws_free();
    const SdcM m = { g, 2 }; char b[220];
    if (!scu_vae_pre()) return NULL;
    const size_t n = (size_t)h * w;
    SdcVb vb; float *img = NULL; int ok = 1, H = h, W = w, C = 512, Co;
    CUdptr_ zd = scu_alloc(n * 16);
    if (!zd || !scu_up(zd, lat, n * 16) || !scu_vae_bufs(&vb, n * 16384, n * 64 * 128)) { ok = 0; goto done; }
    {
        CUdptr_ x = vb.x, y = vb.y;
        const SdcW *pq = scu_w1(&m, "vae.post_quant_conv", 1), *ci = scu_w1(&m, "vae.decoder.conv_in", 1);
        if (!pq || !ci || !scu_cv(pq, zd, y, 0, 0, 0, 0, H, W) || !scu_cv(ci, y, x, 0, 0, 0, 0, H, W)) { ok = 0; goto done; }
        if (!scu_vae_mid(&m, "vae.decoder.mid_block", &vb, &x, &y, H, W, C)) { ok = 0; goto done; }
        for (int blk = 0; blk < 4 && ok; blk++) {
            for (int l = 0; l < 3 && ok; l++) {
                snprintf(b, sizeof b, "vae.decoder.up_blocks.%d.resnets.%d", blk, l);
                ok = scu_vae_resnet(&m, b, &vb, x, y, H, W, &Co);
                if (ok) { CUdptr_ t = x; x = y; y = t; C = Co; }
            }
            if (ok && blk < 3) {
                snprintf(b, sizeof b, "vae.decoder.up_blocks.%d.upsamplers.0.conv", blk); const SdcW *uw = scu_w1(&m, b, 1);
                ok = uw && scu_conv(uw, x, y, uw->b, 0, 0, 0, 0, 2 * H, 2 * W, 1, 1, 1, 1, 2 * H, 2 * W, 1.0f, 0);
                if (ok) { CUdptr_ t = x; x = y; y = t; H *= 2; W *= 2; }
            }
        }
        const SdcW *co = scu_w1(&m, "vae.decoder.conv_out", 1);
        if (!ok || !co || !scu_gn(&m, "vae.decoder.conv_norm_out", x, H * W, C, 1e-6f)) { ok = 0; goto done; }
        if (!scu_cv(co, x, y, 0, g_sdc.ca, g_sdc.cb, 1, H, W) || !scu_sync()) { ok = 0; goto done; }
        img = xalloc((size_t)H * W * 3, 4);
        if (!scu_down(img, y, (size_t)H * W * 12)) { ok = 0; free(img); img = NULL; }
    }
done:
    scu_sync();
    scu_vae_free(&vb); scu_free(zd);
    scu_unload(2, 1);
    if (!ok || !img) { if (img) free(img); fprintf(stderr, "[stable] gpu: o VAE nao coube ou falhou; usando a CPU\n"); return NULL; }
    *Ho = H; *Wo = W; g_sd_be_vae = g_scu_be;
    return img;
}

/* img[H*W][3] em [-1, 1] -> media do latente [H/8*W/8][4] (memoria nova) ou NULL */
static float *scu_vae_encode(const SdgFile *g, const float *img, int H0, int W0, int *ho, int *wo) {
    if (g_sdc.dead || !scu_init()) return NULL;
    if (g_sdc.have_ws) { scu_unload(0, 1); scu_unload(1, 1); scu_ws_free(); }
    const SdcM m = { g, 2 }; char b[220];
    if (!scu_vae_pre()) return NULL;
    SdcVb vb; float *mean = NULL; int ok = 1, H = H0, W = W0, C = 128, Co;
    CUdptr_ id = scu_alloc((size_t)H * W * 12);
    memset(&vb, 0, sizeof vb);
    if (!id || !scu_up(id, img, (size_t)H * W * 12) || !scu_vae_bufs(&vb, (size_t)H * W * 128, (size_t)H * W * 128)) { ok = 0; goto done; }
    {
        CUdptr_ x = vb.x, y = vb.y;
        const SdcW *ci = scu_w1(&m, "vae.encoder.conv_in", 1);
        if (!ci || !scu_cv(ci, id, x, 0, 0, 0, 0, H, W)) { ok = 0; goto done; }
        for (int blk = 0; blk < 4 && ok; blk++) {
            for (int l = 0; l < 2 && ok; l++) {
                snprintf(b, sizeof b, "vae.encoder.down_blocks.%d.resnets.%d", blk, l);
                ok = scu_vae_resnet(&m, b, &vb, x, y, H, W, &Co);
                if (ok) { CUdptr_ t = x; x = y; y = t; C = Co; }
            }
            if (ok && blk < 3) {                    /* descida: preenchimento 0,1,0,1: so embaixo e a direita (os limites da entrada zeram o resto) */
                snprintf(b, sizeof b, "vae.encoder.down_blocks.%d.downsamplers.0.conv", blk); const SdcW *dw = scu_w1(&m, b, 1);
                const int Hn = (H + 1 - 3) / 2 + 1, Wn = (W + 1 - 3) / 2 + 1;
                ok = dw && scu_conv(dw, x, y, dw->b, 0, 0, 0, 0, Hn, Wn, 0, 2, 0, 0, H, W, 1.0f, 0);
                if (ok) { CUdptr_ t = x; x = y; y = t; H = Hn; W = Wn; }
            }
        }
        if (!ok || !scu_vae_mid(&m, "vae.encoder.mid_block", &vb, &x, &y, H, W, C)) { ok = 0; goto done; }
        const SdcW *co = scu_w1(&m, "vae.encoder.conv_out", 1), *qc = scu_w1(&m, "vae.quant_conv", 1);
        if (!co || !qc || !scu_gn(&m, "vae.encoder.conv_norm_out", x, H * W, C, 1e-6f)) { ok = 0; goto done; }
        if (!scu_cv(co, x, y, 0, g_sdc.ca, g_sdc.cb, 1, H, W) || !scu_cv(qc, y, x, 0, 0, 0, 0, H, W) || !scu_sync()) { ok = 0; goto done; }
        float *q8 = xalloc((size_t)H * W * 8, 4);
        if (!scu_down(q8, x, (size_t)H * W * 32)) { ok = 0; free(q8); goto done; }
        mean = xalloc((size_t)H * W * 4, 4);
        for (int i = 0; i < H * W; i++) memcpy(mean + (size_t)i * 4, q8 + (size_t)i * 8, 16);
        free(q8);
    }
done:
    scu_sync();
    scu_vae_free(&vb); scu_free(id);
    scu_unload(2, 1);
    if (!ok || !mean) { free(mean); fprintf(stderr, "[stable] gpu: o codificador do VAE nao coube ou falhou; usando a CPU\n"); return NULL; }
    *ho = H; *wo = W;
    return mean;
}

/* ---------- katana render stable-gpu-test [pasta-models/render]: a GPU contra a CPU (mesmos pesos, mesma entrada) ----------
 * UNet + ControlNet (uma passada, 256x192), decodificador e codificador do VAE. Mede o maior erro e o erro RMS relativos ao maior |valor| /
 * RMS da CPU; passa abaixo de 1e-2 (maior) e 3e-3 (RMS): as contas da GPU usam operandos F16 com acumulo F32 (a CPU e F32 puro). Sem GPU: "pulado", saida 0. */
static int scu_cmp(const char *what, const float *a, const float *r, size_t n, double secs, double lim_max, double lim_rms) {
    double mx = 0, mr = 0, se = 0, sr = 0;
    for (size_t i = 0; i < n; i++) { const double d = fabs((double)a[i] - r[i]); if (!(d <= mx)) mx = d; if (fabs(r[i]) > mr) mr = fabs(r[i]); se += d * d; sr += (double)r[i] * r[i]; }
    const double rm = mr > 0 ? mx / mr : mx, rr = sr > 0 ? sqrt(se / sr) : sqrt(se);
    const int ok = rm < lim_max && rr < lim_rms;
    printf("  %-30s %s  erro max %.3e  |ref| max %.3f  razao %.2e  rms relativo %.2e  (GPU %.2f s)\n", what, ok ? "OK    " : "FALHOU", mx, mr, rm, rr, secs);
    return ok ? 0 : 1;
}
static int scu_test_main(const char *mdir) {
    char p[1100]; SdgFile base, ctrl; int bad = 0;
    if (!scu_init()) { printf("stable-gpu-test: sem GPU utilizavel (CUDA ou Vulkan) (KATANA_BACKEND=%s): pulado\n", getenv("KATANA_BACKEND") ? getenv("KATANA_BACKEND") : ""); return 0; }
    snprintf(p, sizeof p, "%s/sd15-realistic-vision.gguf", mdir); if (!sdg_open(&base, p)) return 1;
    snprintf(p, sizeof p, "%s/sd15-controlnet-canny.gguf", mdir); if (!sdg_open(&ctrl, p)) return 1;
    const int W = 256, H = 192, h8 = H / 8, w8 = W / 8, nctx = 77;
    SdpRng rng = { 12345, 0, 0 };
    float *lat = sdn_alloc((size_t)h8 * w8 * 4), *ctx = sdn_alloc((size_t)nctx * 768), *img = sdn_alloc((size_t)W * H * 3);
    for (size_t i = 0; i < (size_t)h8 * w8 * 4; i++) lat[i] = sdp_rng_gauss(&rng);
    for (size_t i = 0; i < (size_t)nctx * 768; i++) ctx[i] = 0.5f * sdp_rng_gauss(&rng);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { const float v = (x % 37 == 0 || y % 29 == 0 || (x - y) % 53 == 0) ? 1.0f : 0.0f; for (int c = 0; c < 3; c++) img[((size_t)y * W + x) * 3 + c] = v; }
    printf("katana render stable-gpu-test (%s, %dx%d)\n", g_scu_be == 2 ? "Vulkan" : cu_dev.name, W, H);
    int hc, wc; float *cond = sdn_cond_embed(&ctrl, img, H, W, &hc, &wc);
    SdSkips res; float *mres = NULL; int hm, wm;
    float *ref = sdn_alloc((size_t)h8 * w8 * 4), *out = sdn_alloc((size_t)h8 * w8 * 4);
    if (!cond || !sdn_control(&ctrl, lat, h8, w8, 500.0f, ctx, nctx, cond, 1.0f, &res, &mres, &hm, &wm) || !sdn_unet(&base, lat, h8, w8, 500.0f, ctx, nctx, &res, mres, ref)) { fprintf(stderr, "stable-gpu-test: a CPU falhou\n"); return 1; }
    if (!scu_plan(&base, &ctrl, h8, w8, nctx)) { printf("  nao coube na GPU: pulado\n"); return 0; }
    double t0 = sdp_now();
    if (!scu_cond(&ctrl, img, H, W, nctx)) { printf("  cond embedding FALHOU\n"); return 1; }
    if (!scu_eps(&base, &ctrl, lat, h8, w8, 500.0f, ctx, nctx, 1.0f, out)) { printf("  UNet + ControlNet FALHOU\n"); return 1; }
    bad += scu_cmp("UNet + ControlNet (eps)", out, ref, (size_t)h8 * w8 * 4, sdp_now() - t0, 1e-2, 3e-3);
    t0 = sdp_now();
    if (!scu_eps(&base, NULL, lat, h8, w8, 500.0f, ctx, nctx, 1.0f, out)) { printf("  UNet FALHOU\n"); return 1; }
    if (!sdn_unet(&base, lat, h8, w8, 500.0f, ctx, nctx, NULL, NULL, ref)) return 1;
    bad += scu_cmp("UNet sem ControlNet (eps)", out, ref, (size_t)h8 * w8 * 4, sdp_now() - t0, 1e-2, 3e-3);
    int Ho, Wo, Hc, Wc;
    t0 = sdp_now(); float *dg = scu_vae_decode(&base, lat, h8, w8, &Ho, &Wo); const double td = sdp_now() - t0;
    float *dc = sdv_decode(&base, lat, h8, w8, &Hc, &Wc);
    if (!dg || !dc || Ho != Hc || Wo != Wc) { printf("  VAE decodificador FALHOU\n"); return 1; }
    bad += scu_cmp("VAE decodificador", dg, dc, (size_t)Ho * Wo * 3, td, 1e-2, 3e-3);
    for (size_t i = 0; i < (size_t)W * H * 3; i++) img[i] = dc[i] > 1 ? 1 : dc[i] < -1 ? -1 : dc[i];
    t0 = sdp_now(); float *eg = scu_vae_encode(&base, img, H, W, &hc, &wc); const double te = sdp_now() - t0;
    float *ec = sdv_encode(&base, img, H, W, &Hc, &Wc);
    if (!eg || !ec) { printf("  VAE codificador FALHOU\n"); return 1; }
    bad += scu_cmp("VAE codificador", eg, ec, (size_t)hc * wc * 4, te, 1e-2, 3e-3);
    printf("%s\n", bad ? "FALHOU" : "tudo OK");
    return bad ? 1 : 0;
}
