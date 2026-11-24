/* ============================ render: FLUX.2 klein na GPU (CUDA) ===============================
 * O mesmo transformador de fxdit.c com tudo na placa: pesos quantizados residentes na VRAM (Q4_K, Q5_K, BF16 como estao no GGUF; nunca
 * F16/F32), produtos por mma.sync (cu_ptx.c), atencao fundida (tools/cuda/kern.cu), o fluxo residual em F32 e as entradas dos produtos em F16.
 * So os vetores pequenos (embedding do passo e modulacoes, n = 1) ficam na CPU. Sem placa, sem libcuda ou sem VRAM suficiente,
 * fxc_forward() devolve 0 e o chamador usa a CPU. KATANA_BACKEND=cpu/vulkan ou KATANA_CPU=1 desligam; KATANA_FLUX_CUDA=0 tambem. */

#define FXC_NW 320
static struct {
    int tried, ok;
    void *mg, *mk;                                 /* modulo dos produtos, modulo dos kernels de apoio */
    void *f_gemm[3];                               /* bf16, q4k, q5k */
    void *f_cast, *f_ln, *f_qkv, *f_swi, *f_att, *f_attx;
    int prec;                                      /* 1 = produtos de 3 termos (padrao, dentro da tolerancia), 0 = F16 simples (rapido) */
    int smem[3];
    const SdgInfo *wt[FXC_NW]; CUdptr_ wd[FXC_NW]; int nw; size_t wbytes;
    int stages;
    size_t wbudget, streamed; double stream_s;
    CUdptr_ stg[2]; size_t stgcap[2];
} g_fc;
static double g_fc_tm[4];                          /* produtos, atencao, resto, bytes */
static double g_fc_flop;

static int fxc_gemm_stages(void) { const char *e = getenv("KATANA_CUDA_STAGES"); int s = e ? atoi(e) : 3; return s < 2 ? 2 : s > 3 ? 3 : s; }

static int fxc_init(void) {
    if (g_fc.tried) return g_fc.ok;
    g_fc.tried = 1;
    const char *e = getenv("KATANA_FLUX_CUDA");
    if (e && !atoi(e)) return 0;
    { const char *be = getenv("KATANA_BACKEND");   /* cuda|vulkan|cpu: quem nao e o escolhido fica de fora (sem escolha: CUDA, depois Vulkan, depois CPU) */
      if (be && *be && strcmp(be, "cuda")) return 0; }
    if (!cu_init()) return 0;
    const int S = g_fc.stages = fxc_gemm_stages();
    { const char *pe = getenv("KATANA_CUDA_PREC"); g_fc.prec = !(pe && !strcmp(pe, "fast")); }
    char *ptx = malloc(8 << 20), *p = ptx;
    p += sprintf(p, ".version 7.0\n.target sm_80\n.address_size 64\n.extern .shared .align 16 .b8 sm[];\n");
    cpx_b = p;
    if (g_fc.prec) { cpx_gemmx(GQ_BF16, S, 7, "gemm_bf16"); cpx_gemmx(GQ_Q4K, S, 7, "gemm_q4k"); cpx_gemmx(GQ_Q5K, S, 7, "gemm_q5k"); g_fc.smem[0] = S * 8192 * 3; g_fc.smem[1] = g_fc.smem[2] = S * 8192 * 4; }
    else { cpx_gemm(GQ_BF16, S, "gemm_bf16"); cpx_gemm(GQ_Q4K, S, "gemm_q4k"); cpx_gemm(GQ_Q5K, S, "gemm_q5k"); g_fc.smem[0] = g_fc.smem[1] = g_fc.smem[2] = S * 32768; }
    g_fc.mg = cu_module(ptx); free(ptx);
    g_fc.mk = cu_kmod();
    if (!g_fc.mg || !g_fc.mk) return 0;
    g_fc.f_gemm[0] = cu_fn(g_fc.mg, "gemm_bf16"); g_fc.f_gemm[1] = cu_fn(g_fc.mg, "gemm_q4k"); g_fc.f_gemm[2] = cu_fn(g_fc.mg, "gemm_q5k");
    g_fc.f_cast = cu_fn(g_fc.mk, "cast_f32_f16"); g_fc.f_ln = cu_fn(g_fc.mk, "ln_mod"); g_fc.f_qkv = cu_fn(g_fc.mk, "qkv_prep");
    g_fc.f_swi = cu_fn(g_fc.mk, "swiglu"); g_fc.f_att = cu_fn(g_fc.mk, "attn"); g_fc.f_attx = cu_fn(g_fc.mk, "attnx");
    for (int i = 0; i < 3; i++) if (!g_fc.f_gemm[i]) return 0;
    if (!g_fc.f_cast || !g_fc.f_ln || !g_fc.f_qkv || !g_fc.f_swi || !g_fc.f_att || !g_fc.f_attx) return 0;
    for (int i = 0; i < 3; i++) cuapi.FuncSetAttribute(g_fc.f_gemm[i], 8, g_fc.smem[i]);
    cuapi.FuncSetAttribute(g_fc.f_att, 8, 65536); cuapi.FuncSetAttribute(g_fc.f_attx, 8, 65536);
    g_fc.ok = 1;
    if (cu_dev.debug) fprintf(stderr, "cuda: %s, %d SMs, %.0f MHz, %.1f TFLOPS f16 (pico), %.0f GB/s, VRAM livre %.2f GB\n", cu_dev.name, cu_dev.sm_count,
                              cu_dev.clock_khz / 1000.0, cu_dev.peak_tflops_f16, cu_dev.peak_gbps, (double)cu_dev.vram_free / 1e9);
    return 1;
}

/* peso na VRAM (so os bytes quantizados do GGUF). Residente enquanto couber no orcamento (g_fc.wbudget, calculado a cada passo depois das ativacoes);
 * o que nao cabe vai por "streaming": e enviado antes do uso para um de dois buffers de passagem (slot 0 ou 1; um bloco usa ate dois pesos ao mesmo tempo)
 * e fica valido ate o proximo envio no mesmo slot. Devolve 0 so se nem o buffer de passagem coube (o chamador usa a CPU). */
static size_t fxc_tbytes(const SdgInfo *t) {
    const size_t cols = (size_t)t->dim[0], rows = t->nd > 1 ? (size_t)t->dim[1] : 1;
    return t->type == T_F32 ? cols * rows * 4 : t->type == T_BF16 ? cols * rows * 2 : t->type == T_Q4_K ? rows * (cols / 256) * 144 : rows * (cols / 256) * 176;
}
static CUdptr_ fxc_weight(const FxDit *F, const SdgInfo *t, int slot) {
    for (int i = 0; i < g_fc.nw; i++) if (g_fc.wt[i] == t) return g_fc.wd[i];
    const size_t bytes = fxc_tbytes(t);
    if (g_fc.nw < FXC_NW && (bytes < ((size_t)1 << 20) || g_fc.wbytes + bytes <= g_fc.wbudget)) {
        CUdptr_ d = cu_alloc(bytes); if (!d) return 0;
        if (!cu_up(d, sdg_data(&F->g, t), bytes)) { cu_free(d); return 0; }
        g_fc.wt[g_fc.nw] = t; g_fc.wd[g_fc.nw++] = d; g_fc.wbytes += bytes;
        return d;
    }
    if (g_fc.stgcap[slot] < bytes) {                      /* buffer de passagem: cresce ate o maior peso */
        cu_sync(); cu_free(g_fc.stg[slot]); g_fc.stg[slot] = 0; g_fc.stgcap[slot] = 0;
        const size_t cap = (bytes + ((size_t)4 << 20) - 1) / ((size_t)4 << 20) * ((size_t)4 << 20);
        g_fc.stg[slot] = cu_alloc(cap); if (!g_fc.stg[slot]) return 0;
        g_fc.stgcap[slot] = cap;
    }
    const double t0 = sdt_now();
    if (!cu_sync() || !cu_up(g_fc.stg[slot], sdg_data(&F->g, t), bytes)) return 0;
    g_fc.streamed += bytes; g_fc.stream_s += sdt_now() - t0;
    return g_fc.stg[slot];
}
static void fxc_release(void) {
    cu_sync();
    for (int i = 0; i < g_fc.nw; i++) cu_free(g_fc.wd[i]);
    g_fc.nw = 0; g_fc.wbytes = 0;
    for (int s = 0; s < 2; s++) { cu_free(g_fc.stg[s]); g_fc.stg[s] = 0; g_fc.stgcap[s] = 0; }
}
/* chamada pelo VAE: devolve a VRAM dos pesos do transformador (voltam no proximo passo) */
static void fxc_release_all(void) { if (g_fc.ok) fxc_release(); }

static double g_fc_t0;
#define FXC_PROF_BEGIN() do { if (g_fxd_prof) { cu_sync(); g_fc_t0 = sdt_now(); } } while (0)
#define FXC_PROF_END(k) do { if (g_fxd_prof) { cu_sync(); g_fc_tm[k] += sdt_now() - g_fc_t0; } } while (0)

/* Y[n][rows] = X[n][K] (F16, linhas ate multiplo de 128 alocadas) * W^T; modo 0 grava F32, 1 Y += G[col]*acc, 2 grava F16 */
static int fxc_linw(CUdptr_ W, CUdptr_ X, CUdptr_ XL, int n, const SdgInfo *t, CUdptr_ Y, int mode, CUdptr_ G) {
    const int K = (int)t->dim[0], rows = (int)t->dim[1];
    const int ty = t->type == T_BF16 ? 0 : t->type == T_Q4_K ? 1 : 2;
    void *fn = g_fc.f_gemm[ty];
    void *a0[] = { &X, (void *)&W, &Y, &G, &n, &rows, (void *)&K, &mode };
    void *a1[] = { &X, &XL, (void *)&W, &Y, &G, &n, &rows, (void *)&K, &mode };
    FXC_PROF_BEGIN();
    if (!cu_launch(fn, (rows + 127) / 128, (n + 127) / 128, 1, 256, g_fc.smem[ty], g_fc.prec ? a1 : a0)) return 0;
    FXC_PROF_END(0);
    g_fc_flop += 2.0 * n * rows * (double)K;
    return 1;
}
static int fxc_lin(const FxDit *F, CUdptr_ X, CUdptr_ XL, int n, const SdgInfo *t, CUdptr_ Y, int mode, CUdptr_ G) {
    const CUdptr_ W = fxc_weight(F, t, 0); if (!W) return 0;
    return fxc_linw(W, X, XL, n, t, Y, mode, G);
}
static CUdptr_ fxc_scale_dev(const FxDit *F, const char *fmt, int b) {
    char nm[200]; snprintf(nm, sizeof nm, fmt, b);
    const SdgInfo *t = sdg_find(&F->g, nm); if (!t) return 0;
    return fxc_weight(F, t, 0);
}
static int fxc_ln(CUdptr_ X, CUdptr_ Y, CUdptr_ YL, int n, CUdptr_ shift, CUdptr_ scale) {
    int D = FXD_DIM; void *a[] = { &X, &Y, &YL, &shift, &scale, &D };
    FXC_PROF_BEGIN(); const int r = cu_launch(g_fc.f_ln, n, 1, 1, 256, 0, a); FXC_PROF_END(2); return r;
}
static int fxc_qkv(CUdptr_ src, int ld, int n, CUdptr_ Q, CUdptr_ K, CUdptr_ V, CUdptr_ Ql, CUdptr_ Kl, CUdptr_ Vl, CUdptr_ wq, CUdptr_ wk, CUdptr_ cs, CUdptr_ sn) {
    void *a[] = { &src, &ld, &Q, &K, &V, &Ql, &Kl, &Vl, &wq, &wk, &cs, &sn };
    FXC_PROF_BEGIN(); const int r = cu_launch(g_fc.f_qkv, n, 1, 1, 768, 0, a); FXC_PROF_END(2); return r;
}
static int fxc_swiglu(CUdptr_ H, int ldh, CUdptr_ Y, CUdptr_ YL, int ldy, int n, int M) {
    void *a[] = { &H, &ldh, &Y, &YL, &ldy, &M };
    FXC_PROF_BEGIN(); const int r = cu_launch(g_fc.f_swi, n, 1, 1, 256, 0, a); FXC_PROF_END(2); return r;
}
static int fxc_attn(CUdptr_ Q, CUdptr_ K, CUdptr_ V, CUdptr_ Ql, CUdptr_ Kl, CUdptr_ Vl, CUdptr_ O, CUdptr_ Ol, int N, int ldo) {
    float sl = 1.4426950408889634f / sqrtf((float)FXD_HD);
    void *a0[] = { &Q, &K, &V, &O, &N, &ldo, &sl }, *a1[] = { &Q, &K, &V, &Ql, &Kl, &Vl, &O, &Ol, &N, &ldo, &sl };
    FXC_PROF_BEGIN(); const int r = cu_launch(g_fc.prec ? g_fc.f_attx : g_fc.f_att, (N + 63) / 64, FXD_HEADS, 1, 128, 65536, g_fc.prec ? a1 : a0); FXC_PROF_END(1);
    g_fc_flop += 4.0 * FXD_HEADS * FXD_HD * (double)N * N; return r;
}
static int fxc_cast(CUdptr_ x, CUdptr_ y, CUdptr_ yl, long n) {
    void *a[] = { &x, &y, &yl, &n }; return cu_launch(g_fc.f_cast, (int)((n + 255) / 256 > 4096 ? 4096 : (n + 255) / 256), 1, 1, 256, 0, a);
}

#define FXC_PAD(n) (((n) + 127) / 128 * 128)


typedef struct { CUdptr_ p[64]; int n; } FxcMem;
static CUdptr_ fxc_buf(FxcMem *m, size_t bytes, int zero) {
    CUdptr_ d = cu_alloc(bytes); if (!d) return 0;
    if (zero) cu_zero(d, bytes);
    m->p[m->n++] = d; return d;
}
static void fxc_buf_free(FxcMem *m) { for (int i = 0; i < m->n; i++) cu_free(m->p[i]); m->n = 0; }

static int fxc_up_f32(FxcMem *m, CUdptr_ *d, const float *h, size_t n) { *d = fxc_buf(m, n * 4, 0); return *d && cu_up(*d, h, n * 4); }
static void fxc_probe(float *dst, CUdptr_ src, size_t n) { if (dst) cu_down(dst, src, n * 4); }

/* Ativacoes de um passo (todas na VRAM durante o passo). As lineares grandes (qkv, mlp) trabalham em blocos de CH linhas: dqkv, dh, dhs, dxn
 * so tem CH linhas, e o que e [N][...] inteiro e so o fluxo residual dz (F32), Q K V (F16, + metade baixa) e dcat (a saida da atencao dos blocos
 * simples; os duplos usam o mesmo buffer como dA). Sem nenhuma matriz N x N (a atencao e fundida, com softmax online). */
typedef struct {
    int N, Ni, Nt, CH;
    CUdptr_ dmod, dcs, dsn, demb, dimg, dimgL, dtxt, dtxtL, dz, dxn, dxnL, dqkv, Q, K, V, QL, KL, VL, dh, dhs, dhsL, dcat, dcatL, dout;
    size_t bytes;                  /* total pedido */
} FxcAct;

/* aloca (ou so soma, se dry) as ativacoes para CH; 0 se faltou VRAM */
static int fxc_acts(FxcMem *mem, FxcAct *a, int Ni, int Nt, int CH, int dry) {
    const int D = FXD_DIM, N = Nt + Ni, L1 = 3 * D + 2 * FXD_MLP, C2 = D + FXD_MLP;
    const int lo = g_fc.prec;
    a->N = N; a->Ni = Ni; a->Nt = Nt; a->CH = CH; a->bytes = 0;
#define ABUF(var, nbytes, zero) do { const size_t b_ = (nbytes); a->bytes += b_; if (!dry) { a->var = fxc_buf(mem, b_, zero); if (!a->var) return 0; } } while (0)
#define ABUFL(var, nbytes, zero) do { if (lo) ABUF(var, nbytes, zero); else a->var = 0; } while (0)
    ABUF(dmod, (size_t)15 * D * 4, 0); ABUF(dcs, (size_t)N * 128 * 4, 0); ABUF(dsn, (size_t)N * 128 * 4, 0); ABUF(demb, (size_t)2 * D * 4, 0);
    ABUF(dimg, (size_t)FXC_PAD(Ni) * FXD_INCH * 2, 1); ABUFL(dimgL, (size_t)FXC_PAD(Ni) * FXD_INCH * 2, 1);
    ABUF(dtxt, (size_t)FXC_PAD(Nt) * FXD_TXTCH * 2, 1); ABUFL(dtxtL, (size_t)FXC_PAD(Nt) * FXD_TXTCH * 2, 1);
    ABUF(dz, (size_t)N * D * 4, 0);
    ABUF(dxn, (size_t)CH * D * 2, 1); ABUFL(dxnL, (size_t)CH * D * 2, 1);
    ABUF(dqkv, (size_t)CH * 3 * D * 4, 0);
    ABUF(Q, (size_t)N * D * 2, 0); ABUF(K, (size_t)N * D * 2, 0); ABUF(V, (size_t)N * D * 2, 0);
    ABUFL(QL, (size_t)N * D * 2, 0); ABUFL(KL, (size_t)N * D * 2, 0); ABUFL(VL, (size_t)N * D * 2, 0);
    ABUF(dh, (size_t)CH * L1 * 4, 0);
    ABUF(dhs, (size_t)CH * FXD_MLP * 2, 1); ABUFL(dhsL, (size_t)CH * FXD_MLP * 2, 1);
    ABUF(dcat, (size_t)FXC_PAD(N) * C2 * 2, 1); ABUFL(dcatL, (size_t)FXC_PAD(N) * C2 * 2, 1);
    ABUF(dout, (size_t)Ni * FXD_INCH * 4, 0);
#undef ABUF
#undef ABUFL
    return 1;
}

#define OFF(p, b) ((p) ? (p) + (b) : (CUdptr_)0)
/* LayerNorm modulada + produto (qkv) + RoPE/RMSNorm das linhas [0, n) de z, escritas nas fichas [ro, ro+n) de Q K V; em blocos de CH linhas */
static int fxc_ph_qkv(const FxDit *F, const FxcAct *A, CUdptr_ zp, int n, CUdptr_ sh, CUdptr_ sc, const SdgInfo *t, int ro, CUdptr_ wq, CUdptr_ wk) {
    const int D = FXD_DIM;
    const CUdptr_ W = fxc_weight(F, t, 0); if (!W) return 0;
    for (int r0 = 0; r0 < n; r0 += A->CH) {
        const int m = n - r0 < A->CH ? n - r0 : A->CH; const size_t o = (size_t)(ro + r0) * D * 2;
        if (!fxc_ln(zp + (CUdptr_)r0 * D * 4, A->dxn, A->dxnL, m, sh, sc) || !fxc_linw(W, A->dxn, A->dxnL, m, t, A->dqkv, 0, 0)) return 0;
        if (!fxc_qkv(A->dqkv, 3 * D, m, A->Q + o, A->K + o, A->V + o, OFF(A->QL, o), OFF(A->KL, o), OFF(A->VL, o), wq, wk, A->dcs + (CUdptr_)(ro + r0) * 128 * 4, A->dsn + (CUdptr_)(ro + r0) * 128 * 4)) return 0;
    }
    return 1;
}
/* MLP dos blocos duplos: z += gate * mlp2(swiglu(mlp0(LN(z)))), em blocos de CH linhas */
static int fxc_ph_mlp(const FxDit *F, const FxcAct *A, CUdptr_ zp, int n, CUdptr_ sh, CUdptr_ sc, const SdgInfo *t0, const SdgInfo *t2, CUdptr_ gate) {
    const int D = FXD_DIM;
    const CUdptr_ W0 = fxc_weight(F, t0, 0), W2 = fxc_weight(F, t2, 1); if (!W0 || !W2) return 0;
    for (int r0 = 0; r0 < n; r0 += A->CH) {
        const int m = n - r0 < A->CH ? n - r0 : A->CH; const CUdptr_ z = zp + (CUdptr_)r0 * D * 4;
        if (!fxc_ln(z, A->dxn, A->dxnL, m, sh, sc) || !fxc_linw(W0, A->dxn, A->dxnL, m, t0, A->dh, 0, 0)) return 0;
        if (!fxc_swiglu(A->dh, 2 * FXD_MLP, A->dhs, A->dhsL, FXD_MLP, m, FXD_MLP) || !fxc_linw(W2, A->dhs, A->dhsL, m, t2, z, 1, gate)) return 0;
    }
    return 1;
}

/* devolve 1 se calculou na GPU; 0 se nao deu (o chamador usa a CPU). Mesmas contas de fxd_forward. */
static int fxc_forward(const FxDit *F, const float *img, const float *ids_img, int Ni, const float *txt, const float *ids_txt, int Nt, float t, float *out, const FxdProbe *pr) {
    if (!fxc_init()) return 0;
    const int D = FXD_DIM, N = Nt + Ni, L1 = 3 * D + 2 * FXD_MLP, C2 = D + FXD_MLP;
    FxcMem mem = { {0}, 0 };
    int ok = 0;
    float *mod = NULL, *cs = NULL, *sn = NULL;
    const double tstart = sdt_now(); g_fc.streamed = 0; g_fc.stream_s = 0;
    /* 1. embedding do passo e modulacoes (vetores, na CPU) */
    float temb[FXD_DIM], te[256], hbuf[FXD_DIM], st[FXD_DIM];
    mod = xalloc((size_t)15 * D, 4); float *mi = mod, *mt = mod + 6 * D, *ms = mod + 12 * D;
    {
        const float tf = t * 1000.0f;
        for (int i = 0; i < 128; i++) { const float ex = -logf(10000.0f) * (float)i / 128.0f, a = tf * expf(ex); te[i] = cosf(a); te[128 + i] = sinf(a); }
        const SdgInfo *l1 = fxd_t(F, "time_in.in_layer.weight", 0), *l2 = fxd_t(F, "time_in.out_layer.weight", 0);
        const SdgInfo *wmi = fxd_t(F, "double_stream_modulation_img.lin.weight", 0), *wmt = fxd_t(F, "double_stream_modulation_txt.lin.weight", 0), *wms = fxd_t(F, "single_stream_modulation.lin.weight", 0);
        if (!l1 || !l2 || !wmi || !wmt || !wms) { free(mod); return 0; }
        fxd_mv(F, hbuf, te, l1);
        for (int i = 0; i < D; i++) hbuf[i] = fxd_silu(hbuf[i]);
        fxd_mv(F, temb, hbuf, l2);
        for (int i = 0; i < D; i++) st[i] = fxd_silu(temb[i]);
        fxd_mv(F, mi, st, wmi); fxd_mv(F, mt, st, wmt); fxd_mv(F, ms, st, wms);
    }
    if (pr && pr->temb) memcpy(pr->temb, temb, D * 4);
    const SdgInfo *wi = fxd_t(F, "img_in.weight", 0), *wc = fxd_t(F, "txt_in.weight", 0);
    const SdgInfo *wf = fxd_t(F, "final_layer.adaLN_modulation.1.weight", 0), *wo = fxd_t(F, "final_layer.linear.weight", 0);
    if (!wi || !wc || !wf || !wo) { free(mod); return 0; }
    float emb[2 * FXD_DIM]; fxd_mv(F, emb, st, wf);
    cs = xalloc((size_t)N * 128, 4); sn = xalloc((size_t)N * 128, 4);
    fxd_rope_table(cs, sn, ids_txt, Nt); fxd_rope_table(cs + (size_t)Nt * 128, sn + (size_t)Nt * 128, ids_img, Ni);
    /* 2. memoria: ativacoes primeiro (blocos de linhas o maior possivel que caiba), o que sobra e o orcamento dos pesos */
    FxcAct A;
    {
        int CH = 1024; { const char *e = getenv("KATANA_CUDA_CHUNK"); if (e && atoi(e) >= 128) CH = atoi(e) / 128 * 128; }
        const size_t MARG = (size_t)96 << 20, STG = (size_t)2 * (64 << 20);      /* folga do driver; dois buffers de passagem */
        size_t fr = 0, tot = 0; cuapi.MemGetInfo(&fr, &tot);
        const int ch0 = FXC_PAD(N);
        int ch = CH < ch0 ? CH : ch0;
        for (;;) { fxc_acts(NULL, &A, Ni, Nt, ch, 1); if (A.bytes + MARG + STG <= fr + g_fc.wbytes || ch <= 128) break; ch = ch / 2 / 128 * 128; if (ch < 128) ch = 128; }
        if (A.bytes + MARG + STG > fr + g_fc.wbytes) { fprintf(stderr, "flux: ativacoes de %d fichas pedem %.2f GB e a VRAM livre e %.2f GB\n", N, A.bytes / 1e9, (fr + g_fc.wbytes) / 1e9); goto done; }
        if (g_fc.wbytes && A.bytes + MARG + STG > fr) fxc_release();             /* os pesos residentes do passo anterior atrapalham: reenviados abaixo */
        cuapi.MemGetInfo(&fr, &tot);
        g_fc.wbudget = g_fc.wbytes + (fr > A.bytes + MARG + STG ? fr - A.bytes - MARG - STG : 0);
        { const char *e = getenv("KATANA_CUDA_WBUDGET_MB"); if (e && g_fc.wbudget > (size_t)atoi(e) << 20) { g_fc.wbudget = (size_t)atoi(e) << 20; if (g_fc.wbytes > g_fc.wbudget) { fxc_release(); } } }   /* teste do streaming */
        if (cu_dev.debug) fprintf(stderr, "cuda: passo N=%d (img %d + txt %d): ativacoes %.0f MB (blocos de %d linhas), pesos residentes ate %.0f MB, VRAM livre %.0f MB\n",
                                  N, Ni, Nt, A.bytes / 1048576.0, ch, g_fc.wbudget / 1048576.0, fr / 1048576.0);
        if (!fxc_acts(&mem, &A, Ni, Nt, ch, 0)) goto done;
    }
    {
        const CUdptr_ dmod = A.dmod, dcs = A.dcs, dsn = A.dsn, demb = A.demb, dz = A.dz, dxn = A.dxn, dxnL = A.dxnL;
        if (!cu_up(dmod, mod, (size_t)15 * D * 4) || !cu_up(dcs, cs, (size_t)N * 128 * 4) || !cu_up(dsn, sn, (size_t)N * 128 * 4) || !cu_up(demb, emb, 2 * D * 4)) goto done;
        {   /* entradas: F32 -> F16 (linhas preenchidas), as copias F32 so existem durante a conversao */
            FxcMem tmp = { {0}, 0 }; CUdptr_ fimg, ftxt;
            const int r = fxc_up_f32(&tmp, &fimg, img, (size_t)Ni * FXD_INCH) && fxc_up_f32(&tmp, &ftxt, txt, (size_t)Nt * FXD_TXTCH)
                          && fxc_cast(fimg, A.dimg, A.dimgL, (long)Ni * FXD_INCH) && fxc_cast(ftxt, A.dtxt, A.dtxtL, (long)Nt * FXD_TXTCH) && cu_sync();
            fxc_buf_free(&tmp);
            if (!r) goto done;
        }
        const CUdptr_ zt = dz, zi = dz + (CUdptr_)Nt * D * 4, dQ = A.Q, dK = A.K, dV = A.V, dQL = A.QL, dKL = A.KL, dVL = A.VL;
        const CUdptr_ dA = A.dcat, dAL = A.dcatL, dcat = A.dcat, dcatL = A.dcatL;
        /* 3. projecoes de entrada: texto e imagem direto no fluxo residual z = [texto; imagem] */
        if (!fxc_lin(F, A.dtxt, A.dtxtL, Nt, wc, zt, 0, 0) || !fxc_lin(F, A.dimg, A.dimgL, Ni, wi, zi, 0, 0)) goto done;
        if (pr) { fxc_probe(pr->x_emb, zi, (size_t)Ni * D); fxc_probe(pr->c_emb, zt, (size_t)Nt * D); }
        const size_t NT2 = (size_t)Nt * D * 2;       /* bytes de Nt linhas F16 */
        /* 5. blocos duplos */
        const CUdptr_ mI = dmod, mT = dmod + 6 * D * 4;
        for (int b = 0; b < FXD_NDBL; b++) {
            const SdgInfo *iqkv = fxd_t(F, "double_blocks.%d.img_attn.qkv.weight", b), *tqkv = fxd_t(F, "double_blocks.%d.txt_attn.qkv.weight", b);
            const SdgInfo *iprj = fxd_t(F, "double_blocks.%d.img_attn.proj.weight", b), *tprj = fxd_t(F, "double_blocks.%d.txt_attn.proj.weight", b);
            const SdgInfo *im0 = fxd_t(F, "double_blocks.%d.img_mlp.0.weight", b), *im2 = fxd_t(F, "double_blocks.%d.img_mlp.2.weight", b);
            const SdgInfo *tm0 = fxd_t(F, "double_blocks.%d.txt_mlp.0.weight", b), *tm2 = fxd_t(F, "double_blocks.%d.txt_mlp.2.weight", b);
            const CUdptr_ iq = fxc_scale_dev(F, "double_blocks.%d.img_attn.norm.query_norm.scale", b), ik = fxc_scale_dev(F, "double_blocks.%d.img_attn.norm.key_norm.scale", b);
            const CUdptr_ tq = fxc_scale_dev(F, "double_blocks.%d.txt_attn.norm.query_norm.scale", b), tk = fxc_scale_dev(F, "double_blocks.%d.txt_attn.norm.key_norm.scale", b);
            if (!iqkv || !tqkv || !iprj || !tprj || !im0 || !im2 || !tm0 || !tm2 || !iq || !ik || !tq || !tk) goto done;
            const size_t B4 = (size_t)D * 4;       /* bytes de um vetor de modulacao */
            const CUdptr_ i_sh1 = mI, i_sc1 = mI + B4, i_g1 = mI + 2 * B4, i_sh2 = mI + 3 * B4, i_sc2 = mI + 4 * B4, i_g2 = mI + 5 * B4;
            const CUdptr_ t_sh1 = mT, t_sc1 = mT + B4, t_g1 = mT + 2 * B4, t_sh2 = mT + 3 * B4, t_sc2 = mT + 4 * B4, t_g2 = mT + 5 * B4;
            if (!fxc_ph_qkv(F, &A, zt, Nt, t_sh1, t_sc1, tqkv, 0, tq, tk)) goto done;
            if (!fxc_ph_qkv(F, &A, zi, Ni, i_sh1, i_sc1, iqkv, Nt, iq, ik)) goto done;
            if (!fxc_attn(dQ, dK, dV, dQL, dKL, dVL, dA, dAL, N, D)) goto done;
            if (!fxc_lin(F, dA + NT2, OFF(dAL, NT2), Ni, iprj, zi, 1, i_g1) || !fxc_lin(F, dA, dAL, Nt, tprj, zt, 1, t_g1)) goto done;
            if (!fxc_ph_mlp(F, &A, zi, Ni, i_sh2, i_sc2, im0, im2, i_g2) || !fxc_ph_mlp(F, &A, zt, Nt, t_sh2, t_sc2, tm0, tm2, t_g2)) goto done;
            if (pr) { fxc_probe(pr->dimg[b], zi, (size_t)Ni * D); fxc_probe(pr->dtxt[b], zt, (size_t)Nt * D); }
        }
        /* 6. blocos simples sobre z */
        const CUdptr_ mS = dmod + 12 * D * 4;
        for (int b = 0; b < FXD_NSGL; b++) {
            const SdgInfo *l1 = fxd_t(F, "single_blocks.%d.linear1.weight", b), *l2 = fxd_t(F, "single_blocks.%d.linear2.weight", b);
            const CUdptr_ wq = fxc_scale_dev(F, "single_blocks.%d.norm.query_norm.scale", b), wk = fxc_scale_dev(F, "single_blocks.%d.norm.key_norm.scale", b);
            if (!l1 || !l2 || !wq || !wk) goto done;
            if ((int)l1->dim[1] != L1 || (int)l2->dim[0] != C2) goto done;
            const CUdptr_ W1 = fxc_weight(F, l1, 0); if (!W1) goto done;
            for (int r0 = 0; r0 < N; r0 += A.CH) {                  /* linear1 em blocos: q k v vao para Q K V, o SwiGLU para as colunas D.. de dcat */
                const int m = N - r0 < A.CH ? N - r0 : A.CH; const size_t o = (size_t)r0 * D * 2, oc = (size_t)r0 * C2 * 2 + (size_t)D * 2;
                if (!fxc_ln(dz + (CUdptr_)r0 * D * 4, dxn, dxnL, m, mS, mS + (CUdptr_)D * 4) || !fxc_linw(W1, dxn, dxnL, m, l1, A.dh, 0, 0)) goto done;
                if (!fxc_qkv(A.dh, L1, m, dQ + o, dK + o, dV + o, OFF(dQL, o), OFF(dKL, o), OFF(dVL, o), wq, wk, dcs + (CUdptr_)r0 * 128 * 4, dsn + (CUdptr_)r0 * 128 * 4)) goto done;
                if (!fxc_swiglu(A.dh + (CUdptr_)3 * D * 4, L1, dcat + oc, OFF(dcatL, oc), C2, m, FXD_MLP)) goto done;
            }
            if (!fxc_attn(dQ, dK, dV, dQL, dKL, dVL, dcat, dcatL, N, C2)) goto done;
            if (!fxc_lin(F, dcat, dcatL, N, l2, dz, 1, mS + (CUdptr_)2 * D * 4)) goto done;
            if (pr && pr->sgl[b]) fxc_probe(pr->sgl[b], dz, (size_t)N * D);
        }
        /* 7. camada final */
        const CUdptr_ Wo = fxc_weight(F, wo, 0); if (!Wo) goto done;
        for (int r0 = 0; r0 < Ni; r0 += A.CH) {
            const int m = Ni - r0 < A.CH ? Ni - r0 : A.CH;
            if (!fxc_ln(zi + (CUdptr_)r0 * D * 4, dxn, dxnL, m, demb, demb + (CUdptr_)D * 4) || !fxc_linw(Wo, dxn, dxnL, m, wo, A.dout + (CUdptr_)r0 * FXD_INCH * 4, 0, 0)) goto done;
        }
        if (!cu_sync() || !cu_down(out, A.dout, (size_t)Ni * FXD_INCH * 4)) goto done;
        ok = 1;
    }
done:
    cu_sync();
    fxc_buf_free(&mem); free(mod); free(cs); free(sn);
    if (ok) g_fx_be_dit = 1;
    if (ok && cu_dev.debug) fprintf(stderr, "cuda: passo %.2f s (N=%d), pesos residentes %.0f MB, enviados por streaming %.0f MB em %.2f s\n", sdt_now() - tstart, N, g_fc.wbytes / 1048576.0, g_fc.streamed / 1048576.0, g_fc.stream_s);
    if (!ok) { fprintf(stderr, "flux: a GPU (CUDA) nao conseguiu o passo (VRAM livre insuficiente?); usando a CPU\n"); fxc_release(); g_fc.ok = 0; }
    return ok;
}

static int fxc_enabled(void) { return fxc_init(); }
