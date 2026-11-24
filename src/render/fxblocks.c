/* ============================ render: ida com ativacoes guardadas e volta de cada bloco do FLUX.2 klein ===============================
 * fxr_dbl_fwd / fxr_dbl_bwd (bloco duplo), fxr_sgl_fwd / fxr_sgl_bwd (bloco simples) e o passo completo fxr_step (ida com pontos de
 * verificacao por bloco, perda de fluxo, volta bloco a bloco refazendo a ida de cada um). A ida daqui calcula o mesmo que fxd_double e
 * fxd_single (fxdit.c) e e conferida contra eles no selftest. */

typedef struct {
    const SdgInfo *iqkv, *tqkv, *iprj, *tprj, *im0, *im2, *tm0, *tm2;
    float *iq, *ik, *tq, *tk;
} FxrDW;
static int fxr_dw_load(const FxDit *F, int b, FxrDW *w) {
    memset(w, 0, sizeof *w);
    w->iqkv = fxd_t(F, "double_blocks.%d.img_attn.qkv.weight", b); w->tqkv = fxd_t(F, "double_blocks.%d.txt_attn.qkv.weight", b);
    w->iprj = fxd_t(F, "double_blocks.%d.img_attn.proj.weight", b); w->tprj = fxd_t(F, "double_blocks.%d.txt_attn.proj.weight", b);
    w->im0 = fxd_t(F, "double_blocks.%d.img_mlp.0.weight", b); w->im2 = fxd_t(F, "double_blocks.%d.img_mlp.2.weight", b);
    w->tm0 = fxd_t(F, "double_blocks.%d.txt_mlp.0.weight", b); w->tm2 = fxd_t(F, "double_blocks.%d.txt_mlp.2.weight", b);
    w->iq = fxd_scale(F, "double_blocks.%d.img_attn.norm.query_norm.scale", b); w->ik = fxd_scale(F, "double_blocks.%d.img_attn.norm.key_norm.scale", b);
    w->tq = fxd_scale(F, "double_blocks.%d.txt_attn.norm.query_norm.scale", b); w->tk = fxd_scale(F, "double_blocks.%d.txt_attn.norm.key_norm.scale", b);
    return w->iqkv && w->tqkv && w->iprj && w->tprj && w->im0 && w->im2 && w->tm0 && w->tm2 && w->iq && w->ik && w->tq && w->tk;
}
static void fxr_dw_free(FxrDW *w) { free(w->iq); free(w->ik); free(w->tq); free(w->tk); }

/* ativacoes de um bloco duplo (texto primeiro nas matrizes de atencao) */
typedef struct {
    int Ni, Nt;
    float *xt, *xi, *Q0, *K0, *Q, *K, *V, *A, *img1, *txt1, *xi2, *xt2, *hi, *ht, *hsi, *hst, *img2, *txt2;
} FxrDS;
static void fxr_ds_free(FxrDS *S) {
    float **p[] = { &S->xt, &S->xi, &S->Q0, &S->K0, &S->Q, &S->K, &S->V, &S->A, &S->img1, &S->txt1, &S->xi2, &S->xt2, &S->hi, &S->ht, &S->hsi, &S->hst, &S->img2, &S->txt2 };
    for (size_t i = 0; i < sizeof p / sizeof *p; i++) { free(*p[i]); *p[i] = NULL; }
}

/* mi/mt: modulacoes [6*3072]. Deixa a saida em S->img2/S->txt2. */
static int fxr_dbl_fwd(const FxDit *F, int b, const float *img, int Ni, const float *txt, int Nt, const float *mi, const float *mt, const float *cs, const float *sn, FxrDS *S) {
    const int D = FXD_DIM, N = Nt + Ni, M2 = 2 * FXD_MLP, mx = Ni > Nt ? Ni : Nt;
    FxrDW W; if (!fxr_dw_load(F, b, &W)) return 0;
    memset(S, 0, sizeof *S); S->Ni = Ni; S->Nt = Nt;
    S->xt = xalloc((size_t)Nt * D, 4); S->xi = xalloc((size_t)Ni * D, 4);
    S->Q0 = xalloc((size_t)N * D, 4); S->K0 = xalloc((size_t)N * D, 4); S->Q = xalloc((size_t)N * D, 4); S->K = xalloc((size_t)N * D, 4); S->V = xalloc((size_t)N * D, 4); S->A = xalloc((size_t)N * D, 4);
    float *qkv = xalloc((size_t)mx * 3 * D, 4), *o = xalloc((size_t)mx * D, 4);
    fxd_ln_mod(S->xt, txt, Nt, D, mt, mt + D); fxd_lin(F, qkv, S->xt, Nt, W.tqkv); fxd_split_qkv(qkv, (size_t)3 * D, Nt, S->Q0, S->K0, S->V);
    fxd_ln_mod(S->xi, img, Ni, D, mi, mi + D); fxd_lin(F, qkv, S->xi, Ni, W.iqkv); fxd_split_qkv(qkv, (size_t)3 * D, Ni, S->Q0 + (size_t)Nt * D, S->K0 + (size_t)Nt * D, S->V + (size_t)Nt * D);
    memcpy(S->Q, S->Q0, (size_t)N * D * 4); memcpy(S->K, S->K0, (size_t)N * D * 4);
    fxd_rms(S->Q, Nt, D, W.tq); fxd_rms(S->K, Nt, D, W.tk); fxd_rms(S->Q + (size_t)Nt * D, Ni, D, W.iq); fxd_rms(S->K + (size_t)Nt * D, Ni, D, W.ik);
    fxd_rope(S->Q, N, cs, sn); fxd_rope(S->K, N, cs, sn);
    fxd_attention(S->A, S->Q, S->K, S->V, N);
    S->img1 = xalloc((size_t)Ni * D, 4); memcpy(S->img1, img, (size_t)Ni * D * 4);
    S->txt1 = xalloc((size_t)Nt * D, 4); memcpy(S->txt1, txt, (size_t)Nt * D * 4);
    fxd_lin(F, o, S->A + (size_t)Nt * D, Ni, W.iprj); fxd_axpy_gate(S->img1, mi + 2 * D, o, Ni, D);
    fxd_lin(F, o, S->A, Nt, W.tprj); fxd_axpy_gate(S->txt1, mt + 2 * D, o, Nt, D);
    S->xi2 = xalloc((size_t)Ni * D, 4); S->xt2 = xalloc((size_t)Nt * D, 4);
    S->hi = xalloc((size_t)Ni * M2, 4); S->ht = xalloc((size_t)Nt * M2, 4); S->hsi = xalloc((size_t)Ni * FXD_MLP, 4); S->hst = xalloc((size_t)Nt * FXD_MLP, 4);
    S->img2 = xalloc((size_t)Ni * D, 4); memcpy(S->img2, S->img1, (size_t)Ni * D * 4);
    S->txt2 = xalloc((size_t)Nt * D, 4); memcpy(S->txt2, S->txt1, (size_t)Nt * D * 4);
    fxd_ln_mod(S->xi2, S->img1, Ni, D, mi + 3 * D, mi + 4 * D); fxd_lin(F, S->hi, S->xi2, Ni, W.im0); fxd_swiglu(S->hsi, S->hi, Ni, FXD_MLP, (size_t)M2);
    fxd_lin(F, o, S->hsi, Ni, W.im2); fxd_axpy_gate(S->img2, mi + 5 * D, o, Ni, D);
    fxd_ln_mod(S->xt2, S->txt1, Nt, D, mt + 3 * D, mt + 4 * D); fxd_lin(F, S->ht, S->xt2, Nt, W.tm0); fxd_swiglu(S->hst, S->ht, Nt, FXD_MLP, (size_t)M2);
    fxd_lin(F, o, S->hst, Nt, W.tm2); fxd_axpy_gate(S->txt2, mt + 5 * D, o, Nt, D);
    free(qkv); free(o); fxr_dw_free(&W);
    return 1;
}

/* dimg/dtxt: gradientes das saidas (entram) e das entradas (saem, no lugar). img, txt: as entradas do bloco (para a LN); S: da ida */
static int fxr_dbl_bwd(const FxDit *F, int b, const float *img, const float *txt, const FxrDS *S, const float *mi, const float *mt, const float *cs, const float *sn, float *dimg, float *dtxt) {
    const int D = FXD_DIM, Ni = S->Ni, Nt = S->Nt, N = Ni + Nt, M2 = 2 * FXD_MLP, mx = Ni > Nt ? Ni : Nt;
    FxrDW W; if (!fxr_dw_load(F, b, &W)) return 0;
    float *dy = xalloc((size_t)mx * D, 4), *dhs = xalloc((size_t)mx * FXD_MLP, 4), *dh = xalloc((size_t)mx * M2, 4), *dxn = xalloc((size_t)mx * D, 4);
    /* MLP de cada fluxo: saida = x1 + g2 * mlp(LN(x1)) */
    fxr_gate_mul(dy, mi + 5 * D, dimg, Ni, D);
    fxr_lin_bwd(F, dhs, dy, S->hsi, Ni, W.im2); fxr_swiglu_bwd(dh, (size_t)M2, dhs, (size_t)FXD_MLP, S->hi, (size_t)M2, Ni, FXD_MLP);
    memset(dxn, 0, (size_t)Ni * D * 4); { float *t = xalloc((size_t)Ni * D, 4); fxr_lin_bwd(F, t, dh, S->xi2, Ni, W.im0); fxr_ln_mod_bwd(dimg, NULL, NULL, t, S->img1, Ni, D, mi + 4 * D); free(t); }
    fxr_gate_mul(dy, mt + 5 * D, dtxt, Nt, D);
    fxr_lin_bwd(F, dhs, dy, S->hst, Nt, W.tm2); fxr_swiglu_bwd(dh, (size_t)M2, dhs, (size_t)FXD_MLP, S->ht, (size_t)M2, Nt, FXD_MLP);
    { float *t = xalloc((size_t)Nt * D, 4); fxr_lin_bwd(F, t, dh, S->xt2, Nt, W.tm0); fxr_ln_mod_bwd(dtxt, NULL, NULL, t, S->txt1, Nt, D, mt + 4 * D); free(t); }
    /* dimg/dtxt agora sao os gradientes de img1/txt1 (= x1 + ...); atencao: x1 = x0 + g1 * proj(A) */
    float *dA = xalloc((size_t)N * D, 4);
    fxr_gate_mul(dy, mi + 2 * D, dimg, Ni, D); fxr_lin_bwd(F, dA + (size_t)Nt * D, dy, S->A + (size_t)Nt * D, Ni, W.iprj);
    fxr_gate_mul(dy, mt + 2 * D, dtxt, Nt, D); fxr_lin_bwd(F, dA, dy, S->A, Nt, W.tprj);
    float *dQ = xalloc((size_t)N * D, 4), *dK = xalloc((size_t)N * D, 4), *dV = xalloc((size_t)N * D, 4);
    fxr_attention_bwd(dQ, dK, dV, S->Q, S->K, S->V, S->A, dA, N);
    fxr_rope_bwd(dQ, N, cs, sn); fxr_rope_bwd(dK, N, cs, sn);
    fxr_rms_bwd(dQ, S->Q0, Nt, D, W.tq); fxr_rms_bwd(dK, S->K0, Nt, D, W.tk);
    fxr_rms_bwd(dQ + (size_t)Nt * D, S->Q0 + (size_t)Nt * D, Ni, D, W.iq); fxr_rms_bwd(dK + (size_t)Nt * D, S->K0 + (size_t)Nt * D, Ni, D, W.ik);
    float *dqkv = xalloc((size_t)mx * 3 * D, 4);
    for (int s = 0; s < 2; s++) {                                   /* 0 = texto, 1 = imagem */
        const int n = s ? Ni : Nt; const size_t off = s ? (size_t)Nt * D : 0;
        for (int r = 0; r < n; r++) { float *q = dqkv + (size_t)r * 3 * D; memcpy(q, dQ + off + (size_t)r * D, (size_t)D * 4); memcpy(q + D, dK + off + (size_t)r * D, (size_t)D * 4); memcpy(q + 2 * D, dV + off + (size_t)r * D, (size_t)D * 4); }
        memset(dxn, 0, (size_t)n * D * 4);
        fxr_lin_bwd(F, dxn, dqkv, s ? S->xi : S->xt, n, s ? W.iqkv : W.tqkv);
        fxr_ln_mod_bwd(s ? dimg : dtxt, NULL, NULL, dxn, s ? img : txt, n, D, (s ? mi : mt) + D);
    }
    free(dy); free(dhs); free(dh); free(dxn); free(dA); free(dQ); free(dK); free(dV); free(dqkv); fxr_dw_free(&W);
    return 1;
}

/* ---- bloco simples ---- */
typedef struct { int N; float *xn, *h, *Q0, *K0, *Q, *K, *V, *A, *cat, *x1; } FxrSS;
static void fxr_ss_free(FxrSS *S) { float **p[] = { &S->xn, &S->h, &S->Q0, &S->K0, &S->Q, &S->K, &S->V, &S->A, &S->cat, &S->x1 }; for (size_t i = 0; i < sizeof p / sizeof *p; i++) { free(*p[i]); *p[i] = NULL; } }

static int fxr_sgl_fwd(const FxDit *F, int b, const float *x, int N, const float *ms, const float *cs, const float *sn, FxrSS *S) {
    const int D = FXD_DIM, L1 = 3 * D + 2 * FXD_MLP, C2 = D + FXD_MLP;
    const SdgInfo *l1 = fxd_t(F, "single_blocks.%d.linear1.weight", b), *l2 = fxd_t(F, "single_blocks.%d.linear2.weight", b);
    float *wq = fxd_scale(F, "single_blocks.%d.norm.query_norm.scale", b), *wk = fxd_scale(F, "single_blocks.%d.norm.key_norm.scale", b);
    if (!l1 || !l2 || !wq || !wk) return 0;
    memset(S, 0, sizeof *S); S->N = N;
    S->xn = xalloc((size_t)N * D, 4); S->h = xalloc((size_t)N * L1, 4);
    S->Q0 = xalloc((size_t)N * D, 4); S->K0 = xalloc((size_t)N * D, 4); S->Q = xalloc((size_t)N * D, 4); S->K = xalloc((size_t)N * D, 4); S->V = xalloc((size_t)N * D, 4); S->A = xalloc((size_t)N * D, 4);
    fxd_ln_mod(S->xn, x, N, D, ms, ms + D); fxd_lin(F, S->h, S->xn, N, l1);
    fxd_split_qkv(S->h, (size_t)L1, N, S->Q0, S->K0, S->V);
    memcpy(S->Q, S->Q0, (size_t)N * D * 4); memcpy(S->K, S->K0, (size_t)N * D * 4);
    fxd_rms(S->Q, N, D, wq); fxd_rms(S->K, N, D, wk); fxd_rope(S->Q, N, cs, sn); fxd_rope(S->K, N, cs, sn);
    fxd_attention(S->A, S->Q, S->K, S->V, N);
    S->cat = xalloc((size_t)N * C2, 4);
    for (int r = 0; r < N; r++) memcpy(S->cat + (size_t)r * C2, S->A + (size_t)r * D, (size_t)D * 4);
    fxd_swiglu2(S->cat + D, (size_t)C2, S->h + 3 * D, (size_t)L1, N, FXD_MLP);
    float *o = xalloc((size_t)N * D, 4);
    fxd_lin(F, o, S->cat, N, l2);
    S->x1 = xalloc((size_t)N * D, 4); memcpy(S->x1, x, (size_t)N * D * 4);
    fxd_axpy_gate(S->x1, ms + 2 * D, o, N, D);
    free(o); free(wq); free(wk);
    return 1;
}
/* dx: gradiente da saida (entra) e da entrada (sai, no lugar) */
static int fxr_sgl_bwd(const FxDit *F, int b, const float *x, const FxrSS *S, const float *ms, const float *cs, const float *sn, float *dx) {
    const int D = FXD_DIM, N = S->N, L1 = 3 * D + 2 * FXD_MLP, C2 = D + FXD_MLP;
    const SdgInfo *l1 = fxd_t(F, "single_blocks.%d.linear1.weight", b), *l2 = fxd_t(F, "single_blocks.%d.linear2.weight", b);
    float *wq = fxd_scale(F, "single_blocks.%d.norm.query_norm.scale", b), *wk = fxd_scale(F, "single_blocks.%d.norm.key_norm.scale", b);
    if (!l1 || !l2 || !wq || !wk) return 0;
    float *dy = xalloc((size_t)N * D, 4), *dcat = xalloc((size_t)N * C2, 4), *dA = xalloc((size_t)N * D, 4), *dh = xalloc((size_t)N * L1, 4);
    fxr_gate_mul(dy, ms + 2 * D, dx, N, D);
    fxr_lin_bwd(F, dcat, dy, S->cat, N, l2);
    for (int r = 0; r < N; r++) memcpy(dA + (size_t)r * D, dcat + (size_t)r * C2, (size_t)D * 4);
    fxr_swiglu_bwd(dh + 3 * D, (size_t)L1, dcat + D, (size_t)C2, S->h + 3 * D, (size_t)L1, N, FXD_MLP);
    float *dQ = xalloc((size_t)N * D, 4), *dK = xalloc((size_t)N * D, 4), *dV = xalloc((size_t)N * D, 4);
    fxr_attention_bwd(dQ, dK, dV, S->Q, S->K, S->V, S->A, dA, N);
    fxr_rope_bwd(dQ, N, cs, sn); fxr_rope_bwd(dK, N, cs, sn);
    fxr_rms_bwd(dQ, S->Q0, N, D, wq); fxr_rms_bwd(dK, S->K0, N, D, wk);
    for (int r = 0; r < N; r++) { float *q = dh + (size_t)r * L1; memcpy(q, dQ + (size_t)r * D, (size_t)D * 4); memcpy(q + D, dK + (size_t)r * D, (size_t)D * 4); memcpy(q + 2 * D, dV + (size_t)r * D, (size_t)D * 4); }
    float *dxn = xalloc((size_t)N * D, 4);
    fxr_lin_bwd(F, dxn, dh, S->xn, N, l1);
    fxr_ln_mod_bwd(dx, NULL, NULL, dxn, x, N, D, ms + D);
    free(dy); free(dcat); free(dA); free(dh); free(dQ); free(dK); free(dV); free(dxn); free(wq); free(wk);
    return 1;
}

/* ---- modulacoes e tabelas de um passo de tempo (as mesmas contas de fxd_forward) ---- */
typedef struct { float *mi, *mt, *ms, emb[2 * FXD_DIM]; } FxrMods;
static int fxr_mods(const FxDit *F, float t, FxrMods *M) {
    const int D = FXD_DIM; float temb[FXD_DIM], te[256], hb[FXD_DIM], st[FXD_DIM];
    const float tf = t * 1000.0f;
    for (int i = 0; i < 128; i++) { const float ex = -logf(10000.0f) * (float)i / 128.0f, a = tf * expf(ex); te[i] = cosf(a); te[128 + i] = sinf(a); }
    const SdgInfo *l1 = fxd_t(F, "time_in.in_layer.weight", 0), *l2 = fxd_t(F, "time_in.out_layer.weight", 0);
    const SdgInfo *wmi = fxd_t(F, "double_stream_modulation_img.lin.weight", 0), *wmt = fxd_t(F, "double_stream_modulation_txt.lin.weight", 0), *wms = fxd_t(F, "single_stream_modulation.lin.weight", 0);
    const SdgInfo *wf = fxd_t(F, "final_layer.adaLN_modulation.1.weight", 0);
    if (!l1 || !l2 || !wmi || !wmt || !wms || !wf) return 0;
    fxd_mv(F, hb, te, l1); for (int i = 0; i < D; i++) hb[i] = fxd_silu(hb[i]);
    fxd_mv(F, temb, hb, l2);
    for (int i = 0; i < D; i++) st[i] = fxd_silu(temb[i]);
    M->mi = xalloc((size_t)6 * D, 4); M->mt = xalloc((size_t)6 * D, 4); M->ms = xalloc((size_t)3 * D, 4);
    fxd_mv(F, M->mi, st, wmi); fxd_mv(F, M->mt, st, wmt); fxd_mv(F, M->ms, st, wms); fxd_mv(F, M->emb, st, wf);
    return 1;
}
static void fxr_mods_free(FxrMods *M) { free(M->mi); free(M->mt); free(M->ms); }

/* ---- passo de treino ----
 * img [Ni][128]: fichas do alvo ruidoso (as Ntg primeiras) e das referencias; ids como no fxg_run; txt [Nt][7680]; t = sigma;
 * target [Ntg][128] = velocidade alvo (ruido - latente limpo). Devolve a perda (media do erro quadratico nas Ntg*128 saidas) em *loss,
 * deixa em *pred (se nao NULL, [Ntg][128]) a velocidade prevista, e se back != 0 acumula os gradientes dos adaptadores (fxr_alloc_grads). */
static int fxr_step(const FxDit *F, const float *img, const float *ids_img, int Ni, int Ntg, const float *txt, const float *ids_txt, int Nt, float t, const float *target, int back, double *loss, float *pred) {
    const int D = FXD_DIM, N = Nt + Ni;
    FxrMods M; if (!fxr_mods(F, t, &M)) return 0;
    const SdgInfo *wi = fxd_t(F, "img_in.weight", 0), *wc = fxd_t(F, "txt_in.weight", 0), *wo = fxd_t(F, "final_layer.linear.weight", 0);
    if (!wi || !wc || !wo) return 0;
    float *x = xalloc((size_t)Ni * D, 4), *c = xalloc((size_t)Nt * D, 4);
    fxd_lin(F, x, img, Ni, wi); fxd_lin(F, c, txt, Nt, wc);
    float *cs = xalloc((size_t)N * 128, 4), *sn = xalloc((size_t)N * 128, 4);
    fxd_rope_table(cs, sn, ids_txt, Nt); fxd_rope_table(cs + (size_t)Nt * 128, sn + (size_t)Nt * 128, ids_img, Ni);
    /* ida com pontos de verificacao: entradas de cada bloco */
    float *xs[FXD_NDBL], *cc[FXD_NDBL], *zs[FXD_NSGL];
    for (int b = 0; b < FXD_NDBL; b++) {
        xs[b] = xalloc((size_t)Ni * D, 4); cc[b] = xalloc((size_t)Nt * D, 4); memcpy(xs[b], x, (size_t)Ni * D * 4); memcpy(cc[b], c, (size_t)Nt * D * 4);
        if (!fxd_double(F, b, x, Ni, c, Nt, M.mi, M.mt, cs, sn)) return 0;
    }
    float *z = xalloc((size_t)N * D, 4);
    memcpy(z, c, (size_t)Nt * D * 4); memcpy(z + (size_t)Nt * D, x, (size_t)Ni * D * 4);
    for (int b = 0; b < FXD_NSGL; b++) {
        zs[b] = back ? xalloc((size_t)N * D, 4) : NULL; if (zs[b]) memcpy(zs[b], z, (size_t)N * D * 4);
        if (!fxd_single(F, b, z, N, M.ms, cs, sn)) return 0;
    }
    /* camada final so nas fichas do alvo */
    float *y = xalloc((size_t)Ntg * D, 4), *out = xalloc((size_t)Ntg * FXD_INCH, 4);
    fxd_ln_mod(y, z + (size_t)Nt * D, Ntg, D, M.emb, M.emb + D); fxd_lin(F, out, y, Ntg, wo);
    double ls = 0; const size_t no = (size_t)Ntg * FXD_INCH;
    for (size_t i = 0; i < no; i++) { const double d = (double)out[i] - target[i]; ls += d * d; }
    *loss = ls / (double)no;
    if (pred) memcpy(pred, out, no * 4);
    int ok = 1;
    if (back) {
        float *dout = xalloc(no, 4), *dy = xalloc((size_t)Ntg * D, 4), *dz = xalloc((size_t)N * D, 4);
        for (size_t i = 0; i < no; i++) dout[i] = (float)(2.0 * ((double)out[i] - target[i]) / (double)no);
        fxr_dx(F, dy, dout, Ntg, wo);
        fxr_ln_mod_bwd(dz + (size_t)Nt * D, NULL, NULL, dy, z + (size_t)Nt * D, Ntg, D, M.emb + D);
        /* z aqui e a saida do bloco simples 19; para a LN da camada final precisamos dela: z ja e a saida (nao foi sobrescrita) */
        for (int b = FXD_NSGL - 1; b >= 0 && ok; b--) {
            FxrSS S; ok = fxr_sgl_fwd(F, b, zs[b], N, M.ms, cs, sn, &S) && fxr_sgl_bwd(F, b, zs[b], &S, M.ms, cs, sn, dz);
            fxr_ss_free(&S); free(zs[b]);
        }
        float *dx = xalloc((size_t)Ni * D, 4), *dc = xalloc((size_t)Nt * D, 4);
        memcpy(dc, dz, (size_t)Nt * D * 4); memcpy(dx, dz + (size_t)Nt * D, (size_t)Ni * D * 4);
        for (int b = FXD_NDBL - 1; b >= 0 && ok; b--) {
            FxrDS S; ok = fxr_dbl_fwd(F, b, xs[b], Ni, cc[b], Nt, M.mi, M.mt, cs, sn, &S) && fxr_dbl_bwd(F, b, xs[b], cc[b], &S, M.mi, M.mt, cs, sn, dx, dc);
            fxr_ds_free(&S);
        }
        free(dout); free(dy); free(dz); free(dx); free(dc);
    }
    for (int b = 0; b < FXD_NDBL; b++) { free(xs[b]); free(cc[b]); }
    free(x); free(c); free(cs); free(sn); free(z); free(y); free(out); fxr_mods_free(&M);
    return ok;
}
