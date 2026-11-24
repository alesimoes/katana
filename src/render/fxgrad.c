/* ============================ render: testes de gradiente do treino de LoRA do FLUX.2 klein ===============================
 * katana render flux-train selftest-grad   (derivadas direcionais por diferencas finitas centrais contra a volta analitica)
 * Cada teste sorteia uma direcao v, mede (f(x + e v) - f(x - e v)) / 2e e compara com <g, v>, g = gradiente analitico. A perda e
 * sempre uma soma ponderada da saida com pesos sorteados, acumulada em double. Os blocos usam os pesos quantizados reais do GGUF
 * com poucas fichas (a conta de cada ficha e independente do numero delas, a atencao e testada com n que nao e multiplo dos blocos). */

typedef struct { float *p[256]; const float *g[256]; size_t n[256]; int k; } FxrPS;
static void fxr_ps_add(FxrPS *s, float *p, const float *g, size_t n) { s->p[s->k] = p; s->g[s->k] = g; s->n[s->k] = n; s->k++; }
typedef double (*FxrF)(void *);

static double fxr_gauss(SdpRng *r) { return (double)sdp_rng_gauss(r); }

/* devolve o erro relativo |fd - an| / max(|fd|, |an|); imprime */
static double fxr_fd(const char *what, FxrF f, void *cx, FxrPS *s, double eps, SdpRng *rng, double *worst) {
    float *v[256]; double an = 0;
    for (int a = 0; a < s->k; a++) {
        double ss = 0; for (size_t i = 0; i < s->n[a]; i++) ss += (double)s->p[a][i] * s->p[a][i];
        const double rms = sqrt(ss / (double)s->n[a]) + 1e-12;
        v[a] = xalloc(s->n[a], 4);
        for (size_t i = 0; i < s->n[a]; i++) { v[a][i] = (float)(fxr_gauss(rng) * rms); an += (double)s->g[a][i] * v[a][i]; }
    }
    float *save[256];
    for (int a = 0; a < s->k; a++) { save[a] = xalloc(s->n[a], 4); memcpy(save[a], s->p[a], s->n[a] * 4); }
    double fv[2];
    for (int sg = 0; sg < 2; sg++) {
        for (int a = 0; a < s->k; a++) for (size_t i = 0; i < s->n[a]; i++) s->p[a][i] = save[a][i] + (float)((sg ? -eps : eps) * v[a][i]);
        fv[sg] = f(cx);
    }
    for (int a = 0; a < s->k; a++) { memcpy(s->p[a], save[a], s->n[a] * 4); free(save[a]); free(v[a]); }
    const double fd = (fv[0] - fv[1]) / (2.0 * eps), den = fmax(fabs(fd), fabs(an)), rel = den > 0 ? fabs(fd - an) / den : 0.0;
    printf("  %-44s fd %+.6e  analitico %+.6e  erro relativo %.2e\n", what, fd, an, rel);
    if (worst && rel > *worst) *worst = rel;
    return rel;
}
static void fxr_randn(float *x, size_t n, double sd, SdpRng *r) { for (size_t i = 0; i < n; i++) x[i] = (float)(fxr_gauss(r) * sd); }
static double fxr_dot(const float *a, const float *b, size_t n) { double s = 0; for (size_t i = 0; i < n; i++) s += (double)a[i] * b[i]; return s; }

/* ---- primitivas ---- */
typedef struct { float *Q, *K, *V, *O, *w; int n; } FxrTa;
static double fxr_ta_f(void *c) { FxrTa *a = c; fxd_attention_simple(a->O, a->Q, a->K, a->V, a->n); return fxr_dot(a->O, a->w, (size_t)a->n * FXD_DIM); }
typedef struct { float *X, *w, *g; int n; } FxrTr;
static double fxr_tr_f(void *c) { FxrTr *a = c; float *Y = xalloc((size_t)a->n * FXD_DIM, 4); memcpy(Y, a->X, (size_t)a->n * FXD_DIM * 4); fxd_rms(Y, a->n, FXD_DIM, a->w); const double r = fxr_dot(Y, a->g, (size_t)a->n * FXD_DIM); free(Y); return r; }
typedef struct { float *X, *sh, *sc, *g; int n; } FxrTl;
static double fxr_tl_f(void *c) { FxrTl *a = c; float *Y = xalloc((size_t)a->n * FXD_DIM, 4); fxd_ln_mod(Y, a->X, a->n, FXD_DIM, a->sh, a->sc); const double r = fxr_dot(Y, a->g, (size_t)a->n * FXD_DIM); free(Y); return r; }
typedef struct { float *H, *g; int n; } FxrTs;
static double fxr_ts_f(void *c) { FxrTs *a = c; float *Y = xalloc((size_t)a->n * FXD_MLP, 4); fxd_swiglu(Y, a->H, a->n, FXD_MLP, (size_t)2 * FXD_MLP); const double r = fxr_dot(Y, a->g, (size_t)a->n * FXD_MLP); free(Y); return r; }

static int fxr_test_prims(SdpRng *rng, double *worst) {
    const int D = FXD_DIM;
    printf("primitivas (n pequeno, dimensoes reais do modelo):\n");
    { /* atencao: n = 70 (nao e multiplo do bloco de 48 nem do painel) */
        const int n = 70; const size_t sz = (size_t)n * D;
        FxrTa a = { xalloc(sz, 4), xalloc(sz, 4), xalloc(sz, 4), xalloc(sz, 4), xalloc(sz, 4), n };
        fxr_randn(a.Q, sz, 0.7, rng); fxr_randn(a.K, sz, 0.7, rng); fxr_randn(a.V, sz, 1.0, rng); fxr_randn(a.w, sz, 1.0, rng);
        fxd_attention_simple(a.O, a.Q, a.K, a.V, n);
        float *dQ = xalloc(sz, 4), *dK = xalloc(sz, 4), *dV = xalloc(sz, 4);
        fxr_attention_bwd(dQ, dK, dV, a.Q, a.K, a.V, a.O, a.w, n);
        FxrPS s = {0}; fxr_ps_add(&s, a.Q, dQ, sz); fxr_fd("atencao d/dQ", fxr_ta_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_add(&s, a.K, dK, sz); fxr_fd("atencao d/dK", fxr_ta_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_add(&s, a.V, dV, sz); fxr_fd("atencao d/dV", fxr_ta_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_add(&s, a.Q, dQ, sz); fxr_ps_add(&s, a.K, dK, sz); fxr_ps_add(&s, a.V, dV, sz); fxr_fd("atencao d/(Q,K,V) juntos", fxr_ta_f, &a, &s, 1e-2, rng, worst);
        free(a.Q); free(a.K); free(a.V); free(a.O); free(a.w); free(dQ); free(dK); free(dV);
    }
    { /* RMSNorm por cabeca */
        const int n = 5; const size_t sz = (size_t)n * D;
        FxrTr a = { xalloc(sz, 4), xalloc(FXD_HD, 4), xalloc(sz, 4), n };
        fxr_randn(a.X, sz, 1.3, rng); fxr_randn(a.w, FXD_HD, 1.0, rng); fxr_randn(a.g, sz, 1.0, rng);
        float *dX = xalloc(sz, 4); memcpy(dX, a.g, sz * 4); fxr_rms_bwd(dX, a.X, n, D, a.w);
        FxrPS s = {0}; fxr_ps_add(&s, a.X, dX, sz); fxr_fd("RMSNorm por cabeca d/dX", fxr_tr_f, &a, &s, 1e-2, rng, worst);
        free(a.X); free(a.w); free(a.g); free(dX);
    }
    { /* RoPE: <R x, g> = <x, R^T g> */
        const int n = 9; const size_t sz = (size_t)n * D;
        float *ids = xalloc((size_t)n * 4, 4), *cs = xalloc((size_t)n * 128, 4), *sn = xalloc((size_t)n * 128, 4);
        for (int i = 0; i < n; i++) { ids[i * 4] = (float)(i % 3); ids[i * 4 + 1] = (float)(i / 3); ids[i * 4 + 2] = (float)(i * 7 % 11); ids[i * 4 + 3] = (float)(i % 2); }
        fxd_rope_table(cs, sn, ids, n);
        float *x = xalloc(sz, 4), *g = xalloc(sz, 4), *y = xalloc(sz, 4), *gx = xalloc(sz, 4);
        fxr_randn(x, sz, 1.0, rng); fxr_randn(g, sz, 1.0, rng); memcpy(y, x, sz * 4); fxd_rope(y, n, cs, sn); memcpy(gx, g, sz * 4); fxr_rope_bwd(gx, n, cs, sn);
        const double a = fxr_dot(y, g, sz), b = fxr_dot(x, gx, sz), rel = fabs(a - b) / fmax(fabs(a), 1e-12);
        printf("  %-44s <Rx,g> %+.6e  <x,R^T g> %+.6e  erro relativo %.2e\n", "RoPE (4 eixos), rotacao inversa", a, b, rel);
        if (rel > *worst) *worst = rel;
        /* ida e volta: R^T R x = x */
        memcpy(gx, y, sz * 4); fxr_rope_bwd(gx, n, cs, sn); double mx = 0; for (size_t i = 0; i < sz; i++) mx = fmax(mx, fabs(gx[i] - x[i]));
        printf("  %-44s erro max %.2e\n", "RoPE: R^T R x = x", mx);
        free(ids); free(cs); free(sn); free(x); free(g); free(y); free(gx);
    }
    { /* LayerNorm modulada: X, shift, scale */
        const int n = 6; const size_t sz = (size_t)n * D;
        FxrTl a = { xalloc(sz, 4), xalloc(D, 4), xalloc(D, 4), xalloc(sz, 4), n };
        fxr_randn(a.X, sz, 1.5, rng); fxr_randn(a.sh, D, 0.3, rng); fxr_randn(a.sc, D, 0.3, rng); fxr_randn(a.g, sz, 1.0, rng);
        float *dX = xalloc(sz, 4), *dsh = xalloc(D, 4), *dsc = xalloc(D, 4);
        fxr_ln_mod_bwd(dX, dsh, dsc, a.g, a.X, n, D, a.sc);
        FxrPS s = {0}; fxr_ps_add(&s, a.X, dX, sz); fxr_fd("LayerNorm modulada (adaLN) d/dX", fxr_tl_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_add(&s, a.sh, dsh, D); fxr_fd("adaLN d/shift", fxr_tl_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_add(&s, a.sc, dsc, D); fxr_fd("adaLN d/scale", fxr_tl_f, &a, &s, 1e-2, rng, worst);
        free(a.X); free(a.sh); free(a.sc); free(a.g); free(dX); free(dsh); free(dsc);
    }
    { /* SwiGLU */
        const int n = 4; const size_t sz = (size_t)n * 2 * FXD_MLP;
        FxrTs a = { xalloc(sz, 4), xalloc((size_t)n * FXD_MLP, 4), n };
        fxr_randn(a.H, sz, 1.2, rng); fxr_randn(a.g, (size_t)n * FXD_MLP, 1.0, rng);
        float *dH = xalloc(sz, 4); fxr_swiglu_bwd(dH, (size_t)2 * FXD_MLP, a.g, (size_t)FXD_MLP, a.H, (size_t)2 * FXD_MLP, n, FXD_MLP);
        FxrPS s = {0}; fxr_ps_add(&s, a.H, dH, sz); fxr_fd("SwiGLU d/H", fxr_ts_f, &a, &s, 1e-2, rng, worst);
        free(a.H); free(a.g); free(dH);
    }
    return 0;
}

/* ---- blocos ---- */
typedef struct {
    const FxDit *F; int b, Ni, Nt;
    float *img, *txt, *wi, *wt, *mi, *mt, *cs, *sn;
} FxrTd;
static double fxr_td_f(void *c) {
    FxrTd *a = c; FxrDS S;
    if (!fxr_dbl_fwd(a->F, a->b, a->img, a->Ni, a->txt, a->Nt, a->mi, a->mt, a->cs, a->sn, &S)) return NAN;
    const double r = fxr_dot(S.img2, a->wi, (size_t)a->Ni * FXD_DIM) + fxr_dot(S.txt2, a->wt, (size_t)a->Nt * FXD_DIM);
    fxr_ds_free(&S); return r;
}
typedef struct { const FxDit *F; int b, N; float *x, *w, *ms, *cs, *sn; } FxrTg;
static double fxr_tg_f(void *c) {
    FxrTg *a = c; FxrSS S;
    if (!fxr_sgl_fwd(a->F, a->b, a->x, a->N, a->ms, a->cs, a->sn, &S)) return NAN;
    const double r = fxr_dot(S.x1, a->w, (size_t)a->N * FXD_DIM);
    fxr_ss_free(&S); return r;
}
static void fxr_ids_small(float *ids_img, int Ni, float *ids_txt, int Nt) {
    int W = 1; while (W * W < Ni) W++;
    for (int i = 0; i < Ni; i++) { ids_img[i * 4] = 0; ids_img[i * 4 + 1] = (float)(i / W); ids_img[i * 4 + 2] = (float)(i % W); ids_img[i * 4 + 3] = 0; }
    for (int i = 0; i < Nt; i++) { ids_txt[i * 4] = 0; ids_txt[i * 4 + 1] = 0; ids_txt[i * 4 + 2] = 0; ids_txt[i * 4 + 3] = (float)i; }
}
/* adaptador de teste: A como criado, B sorteado pequeno (para os dois lados do adaptador afetarem a saida) */
static void fxr_lora_test_init(const FxDit *F, SdpRng *rng) {
    fxl_create(F, 16, 16.0f, 4242); g_fxl_on = 1; g_fxl_store.scale = 1.0f;
    for (int i = 0; i < g_fxl_store.n; i++) { FxlMod *m = &g_fxl_store.m[i]; fxr_randn(m->B, (size_t)m->rows * m->r, 0.05, rng); }
    fxr_alloc_grads();
}
static void fxr_ps_lora(FxrPS *s, const char *prefix) {
    for (int i = 0; i < g_fxl_store.n; i++) { FxlMod *m = &g_fxl_store.m[i]; if (strncmp(m->name, prefix, strlen(prefix))) continue; fxr_ps_add(s, m->A, m->gA, (size_t)m->r * m->cols); fxr_ps_add(s, m->B, m->gB, (size_t)m->rows * m->r); }
}

static int fxr_test_blocks(const FxDit *F, SdpRng *rng, double *worst) {
    const int D = FXD_DIM, Ni = 6, Nt = 5, N = Ni + Nt;
    float *ids_i = xalloc((size_t)Ni * 4, 4), *ids_t = xalloc((size_t)Nt * 4, 4), *cs = xalloc((size_t)N * 128, 4), *sn = xalloc((size_t)N * 128, 4);
    fxr_ids_small(ids_i, Ni, ids_t, Nt); fxd_rope_table(cs, sn, ids_t, Nt); fxd_rope_table(cs + (size_t)Nt * 128, sn + (size_t)Nt * 128, ids_i, Ni);
    fxr_lora_test_init(F, rng); g_fxr_train = 1;
    printf("bloco duplo 1 (pesos Q4_K/Q5_K/BF16 reais, adaptadores de posto 16 em 8 matrizes, %d fichas de imagem + %d de texto):\n", Ni, Nt);
    {
        FxrTd a = { F, 1, Ni, Nt, xalloc((size_t)Ni * D, 4), xalloc((size_t)Nt * D, 4), xalloc((size_t)Ni * D, 4), xalloc((size_t)Nt * D, 4), xalloc((size_t)6 * D, 4), xalloc((size_t)6 * D, 4), cs, sn };
        fxr_randn(a.img, (size_t)Ni * D, 1.0, rng); fxr_randn(a.txt, (size_t)Nt * D, 1.0, rng); fxr_randn(a.wi, (size_t)Ni * D, 1.0, rng); fxr_randn(a.wt, (size_t)Nt * D, 1.0, rng);
        fxr_randn(a.mi, (size_t)6 * D, 0.3, rng); fxr_randn(a.mt, (size_t)6 * D, 0.3, rng);
        for (int k = 0; k < D; k++) { a.mi[2 * D + k] += 0.7f; a.mi[5 * D + k] += 0.7f; a.mt[2 * D + k] += 0.7f; a.mt[5 * D + k] += 0.7f; }
        FxrDS S; if (!fxr_dbl_fwd(F, 1, a.img, Ni, a.txt, Nt, a.mi, a.mt, cs, sn, &S)) return 1;
        /* a ida guardada tem de ser igual a fxd_double */
        float *xi = xalloc((size_t)Ni * D, 4), *xt = xalloc((size_t)Nt * D, 4); memcpy(xi, a.img, (size_t)Ni * D * 4); memcpy(xt, a.txt, (size_t)Nt * D * 4);
        fxd_double(F, 1, xi, Ni, xt, Nt, a.mi, a.mt, cs, sn);
        double mx = 0; for (size_t i = 0; i < (size_t)Ni * D; i++) mx = fmax(mx, fabs(xi[i] - S.img2[i])); for (size_t i = 0; i < (size_t)Nt * D; i++) mx = fmax(mx, fabs(xt[i] - S.txt2[i]));
        printf("  ida guardada x fxd_double (com adaptador)       erro max %.2e\n", mx);
        fxr_ds_free(&S); free(xi); free(xt);
        /* volta analitica */
        fxr_zero_grads();
        if (!fxr_dbl_fwd(F, 1, a.img, Ni, a.txt, Nt, a.mi, a.mt, cs, sn, &S)) return 1;
        float *dimg = xalloc((size_t)Ni * D, 4), *dtxt = xalloc((size_t)Nt * D, 4); memcpy(dimg, a.wi, (size_t)Ni * D * 4); memcpy(dtxt, a.wt, (size_t)Nt * D * 4);
        if (!fxr_dbl_bwd(F, 1, a.img, a.txt, &S, a.mi, a.mt, cs, sn, dimg, dtxt)) return 1;
        fxr_ds_free(&S);
        FxrPS s = {0}; fxr_ps_add(&s, a.img, dimg, (size_t)Ni * D); fxr_fd("entrada de imagem d/dx", fxr_td_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_add(&s, a.txt, dtxt, (size_t)Nt * D); fxr_fd("entrada de texto d/dc", fxr_td_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_add(&s, a.img, dimg, (size_t)Ni * D); fxr_ps_add(&s, a.txt, dtxt, (size_t)Nt * D); fxr_fd("entradas juntas", fxr_td_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_lora(&s, "double_blocks.1.img_attn.qkv"); fxr_fd("LoRA img_attn.qkv (A e B)", fxr_td_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_lora(&s, "double_blocks.1.img_attn.proj"); fxr_fd("LoRA img_attn.proj", fxr_td_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_lora(&s, "double_blocks.1.img_mlp"); fxr_fd("LoRA img_mlp.0 e .2", fxr_td_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_lora(&s, "double_blocks.1.txt_attn"); fxr_fd("LoRA txt_attn.qkv e .proj", fxr_td_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_lora(&s, "double_blocks.1.txt_mlp"); fxr_fd("LoRA txt_mlp.0 e .2", fxr_td_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_lora(&s, "double_blocks.1."); fxr_fd("LoRA do bloco inteiro (8 matrizes)", fxr_td_f, &a, &s, 1e-2, rng, worst);
        free(a.img); free(a.txt); free(a.wi); free(a.wt); free(a.mi); free(a.mt); free(dimg); free(dtxt);
    }
    printf("bloco simples 3 (%d fichas):\n", N);
    {
        FxrTg a = { F, 3, N, xalloc((size_t)N * D, 4), xalloc((size_t)N * D, 4), xalloc((size_t)3 * D, 4), cs, sn };
        fxr_randn(a.x, (size_t)N * D, 1.0, rng); fxr_randn(a.w, (size_t)N * D, 1.0, rng); fxr_randn(a.ms, (size_t)3 * D, 0.3, rng);
        for (int k = 0; k < D; k++) a.ms[2 * D + k] += 0.7f;
        FxrSS S; if (!fxr_sgl_fwd(F, 3, a.x, N, a.ms, cs, sn, &S)) return 1;
        float *x1 = xalloc((size_t)N * D, 4); memcpy(x1, a.x, (size_t)N * D * 4); fxd_single(F, 3, x1, N, a.ms, cs, sn);
        double mx = 0; for (size_t i = 0; i < (size_t)N * D; i++) mx = fmax(mx, fabs(x1[i] - S.x1[i]));
        printf("  ida guardada x fxd_single (com adaptador)       erro max %.2e\n", mx);
        fxr_ss_free(&S); free(x1);
        fxr_zero_grads();
        if (!fxr_sgl_fwd(F, 3, a.x, N, a.ms, cs, sn, &S)) return 1;
        float *dx = xalloc((size_t)N * D, 4); memcpy(dx, a.w, (size_t)N * D * 4);
        if (!fxr_sgl_bwd(F, 3, a.x, &S, a.ms, cs, sn, dx)) return 1;
        fxr_ss_free(&S);
        FxrPS s = {0}; fxr_ps_add(&s, a.x, dx, (size_t)N * D); fxr_fd("entrada d/dx", fxr_tg_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_lora(&s, "single_blocks.3.linear1"); fxr_fd("LoRA linear1 (A e B)", fxr_tg_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_lora(&s, "single_blocks.3.linear2"); fxr_fd("LoRA linear2", fxr_tg_f, &a, &s, 1e-2, rng, worst);
        s.k = 0; fxr_ps_lora(&s, "single_blocks.3."); fxr_fd("LoRA do bloco inteiro", fxr_tg_f, &a, &s, 1e-2, rng, worst);
        free(a.x); free(a.w); free(a.ms); free(dx);
    }
    free(ids_i); free(ids_t); free(cs); free(sn);
    return 0;
}

/* ---- modelo inteiro (25 blocos, ponto de verificacao por bloco) contra diferencas finitas nos adaptadores, e a ida contra fxd_forward ---- */
typedef struct { const FxDit *F; float *img, *ids_i, *txt, *ids_t, *tgt; int Ni, Ntg, Nt; float t; } FxrTm;
static double fxr_tm_f(void *c) { FxrTm *a = c; double l; if (!fxr_step(a->F, a->img, a->ids_i, a->Ni, a->Ntg, a->txt, a->ids_t, a->Nt, a->t, a->tgt, 0, &l, NULL)) return NAN; return l; }

static int fxr_test_model(const FxDit *F, SdpRng *rng, double *worst) {
    const int Ni = 6, Ntg = 4, Nt = 5;
    FxrTm a = { F, xalloc((size_t)Ni * 128, 4), xalloc((size_t)Ni * 4, 4), xalloc((size_t)Nt * FXD_TXTCH, 4), xalloc((size_t)Nt * 4, 4), xalloc((size_t)Ntg * 128, 4), Ni, Ntg, Nt, 0.6f };
    fxr_randn(a.img, (size_t)Ni * 128, 1.0, rng); fxr_randn(a.txt, (size_t)Nt * FXD_TXTCH, 2.5, rng); fxr_randn(a.tgt, (size_t)Ntg * 128, 1.0, rng);
    fxr_ids_small(a.ids_i, Ni, a.ids_t, Nt);
    fxr_lora_test_init(F, rng); g_fxr_train = 1;
    printf("modelo inteiro (5 duplos + 20 simples, %d fichas de imagem das quais %d de alvo, %d de texto, t = %.1f):\n", Ni, Ntg, Nt, a.t);
    double l0, l1; float *p1 = xalloc((size_t)Ntg * 128, 4), *full = xalloc((size_t)Ni * 128, 4);
    fxr_zero_grads();
    if (!fxr_step(F, a.img, a.ids_i, Ni, Ntg, a.txt, a.ids_t, Nt, a.t, a.tgt, 1, &l0, p1)) return 1;
    if (!fxd_forward(F, a.img, a.ids_i, Ni, a.txt, a.ids_t, Nt, a.t, full, NULL)) return 1;
    double mx = 0, mr = 0; for (size_t i = 0; i < (size_t)Ntg * 128; i++) { mx = fmax(mx, fabs(p1[i] - full[i])); mr = fmax(mr, fabs(full[i])); }
    printf("  ida do treino x fxd_forward (com adaptador)     erro max %.2e  (|saida| max %.2f)\n", mx, mr);
    (void)l1;
    FxrPS s = {0};
    s.k = 0; fxr_ps_lora(&s, "double_blocks.0."); fxr_fd("perda: LoRA do bloco duplo 0", fxr_tm_f, &a, &s, 1e-2, rng, worst);
    s.k = 0; fxr_ps_lora(&s, "double_blocks.4."); fxr_fd("perda: LoRA do bloco duplo 4", fxr_tm_f, &a, &s, 1e-2, rng, worst);
    s.k = 0; fxr_ps_lora(&s, "single_blocks.0."); fxr_fd("perda: LoRA do bloco simples 0", fxr_tm_f, &a, &s, 1e-2, rng, worst);
    s.k = 0; fxr_ps_lora(&s, "single_blocks.19."); fxr_fd("perda: LoRA do bloco simples 19", fxr_tm_f, &a, &s, 1e-2, rng, worst);
    s.k = 0; fxr_ps_lora(&s, ""); fxr_fd("perda: LoRA das 80 matrizes juntas", fxr_tm_f, &a, &s, 1e-2, rng, worst);
    return 0;
}

/* adaptador somado a saida x produto com W + escala s B A explicito (desquantizado) */
static int fxr_test_merge(const FxDit *F, SdpRng *rng) {
    const char *nm = "double_blocks.0.img_attn.qkv.weight"; const SdgInfo *t = sdg_find(&F->g, nm);
    const FxlMod *m = NULL; for (int i = 0; i < g_fxl_store.n; i++) if (!strcmp(g_fxl_store.m[i].name, nm)) m = &g_fxl_store.m[i];
    if (!t || !m) return 1;
    const int cols = m->cols, rows = m->rows, n = 7; const float sc = g_fxl_store.scale * m->s;
    float *Wd = xalloc((size_t)rows * cols, 4), *X = xalloc((size_t)n * cols, 4), *Y1 = xalloc((size_t)n * rows, 4), *Y0 = xalloc((size_t)n * rows, 4);
    for (int o = 0; o < rows; o++) {
        fxd_row(t, sdg_data(&F->g, t), o, cols, Wd + (size_t)o * cols);
        for (int k = 0; k < m->r; k++) { const float b = sc * m->B[(size_t)o * m->r + k]; const float *a = m->A + (size_t)k * cols; float *w = Wd + (size_t)o * cols; for (int c = 0; c < cols; c++) w[c] += b * a[c]; }
    }
    fxr_randn(X, (size_t)n * cols, 1.0, rng);
    g_fxl_on = 0; fxd_lin(F, Y0, X, n, t); g_fxl_on = 1; fxd_lin(F, Y1, X, n, t);
    double mx = 0, mr = 0, eff = 0;
    for (int i = 0; i < n; i++) for (int o = 0; o < rows; o++) {
        double s = 0; const float *w = Wd + (size_t)o * cols, *x = X + (size_t)i * cols; for (int c = 0; c < cols; c++) s += (double)w[c] * x[c];
        mx = fmax(mx, fabs(s - Y1[(size_t)i * rows + o])); mr = fmax(mr, fabs(s)); eff = fmax(eff, fabs(Y1[(size_t)i * rows + o] - Y0[(size_t)i * rows + o]));
    }
    printf("fundido x explicito (%s, 7 linhas): erro max %.2e, |saida| max %.2f (efeito do adaptador: %.2e)\n", nm, mx, mr, eff);
    free(Wd); free(X); free(Y0); free(Y1);
    return mx / mr < 1e-4 ? 0 : 1;
}

static int fxr_selftest_grad(void) {
    char gp[1100]; snprintf(gp, sizeof gp, "%s/flux2klein/gguf/flux-2-klein-4b-Q4_K_M.gguf", models_dir());
    static FxDit F; if (!fxd_open(&F, gp)) return 1;
    SdpRng rng = { 20261124, 0, 0 }; double worst = 0;
    const double t0 = sdt_now();
    fxr_test_prims(&rng, &worst);
    fxr_test_blocks(&F, &rng, &worst);
    fxr_test_model(&F, &rng, &worst);
    const int bm = fxr_test_merge(&F, &rng);
    /* arquivo: gravar, recarregar, mesma saida */
    int bf = 0;
    {
        char path[] = "/tmp/fxlora-selftest.fxlora"; const FxlMod *m0 = &g_fxl_store.m[0];
        float *keepA = xalloc((size_t)m0->r * m0->cols, 4); memcpy(keepA, m0->A, (size_t)m0->r * m0->cols * 4);
        if (!fxl_save(path)) bf = 1;
        else { g_fxl_on = 0; memset(&g_fxl_store, 0, sizeof g_fxl_store); if (!fxl_load(&F, path, 0.5f)) bf = 1; else { bf = memcmp(keepA, g_fxl_store.m[0].A, (size_t)m0->r * m0->cols * 4) != 0 || g_fxl_store.n != 80; } }
        printf(".fxlora gravar e carregar (hash do GGUF base conferido, escala 0.5 aplicada ao carregar): %s\n", bf ? "FALHOU" : "igual, 80 modulos");
        unlink(path);
    }
    printf("pior erro relativo das diferencas finitas: %.2e   tempo total %.0f s\n", worst, sdt_now() - t0);
    const int ok = worst < 2e-2 && !bm && !bf;
    printf("RESULTADO: %s\n", ok ? "todos passaram" : "FALHOU");
    return ok ? 0 : 1;
}
