/* ============================ render: transformador do FLUX.2 klein 4B (CPU) ===============================
 * katana render flux-dit selftest [pasta-referencia] [flux2-klein.gguf]
 *
 * O Flux2Transformer2DModel inteiro: projecoes de entrada, embedding do passo, modulacoes, 5 blocos duplos (fluxos de imagem e de texto com
 * atencao conjunta), 20 blocos simples (atencao e MLP em paralelo num so produto) e a camada final com normalizacao adaptativa.
 * Os pesos ficam no GGUF mapeado, com os nomes do formato original do modelo (double_blocks.N.img_attn.qkv.weight ...), em F32, BF16,
 * Q4_K e Q5_K. Cada produto desquantiza o peso direto para os paineis do produto de matrizes (vision/tensor.c) e roda em float32;
 * nada alem do GGUF mapeado fica em memoria entre duas chamadas.
 *
 * Formas (config.json): dim 3072 = 24 cabecas x 128; entrada de imagem 128 canais (latente 32 x patch 2x2); texto 7680 (tres camadas do
 * codificador de texto concatenadas); MLP de razao 3 (9216) com SwiGLU; RoPE em 4 eixos de 32 (t, h, w, l), theta 2000.
 * Ordem dos tokens na atencao: texto primeiro, imagem depois. Modulacao: shift, scale, gate; a camada final usa (shift, scale).
 *
 * Interface:  fxd_open(F, caminho)   fxd_forward(F, img, ids_img, Ni, txt, ids_txt, Nt, t, out, sonda)
 *   img  [Ni][128] latente empacotado;  ids_img/ids_txt [N][4] posicoes (t, h, w, l) em float;  txt [Nt][7680];  t em [0, 1] (sigma);
 *   out  [Ni][128] (a velocidade prevista).  sonda (pode ser NULL) recebe copias dos estados intermediarios para o teste. */

#define FXD_DIM 3072
#define FXD_HEADS 24
#define FXD_HD 128
#define FXD_MLP 9216
#define FXD_INCH 128
#define FXD_TXTCH 7680
#define FXD_NDBL 5
#define FXD_NSGL 20

typedef struct { SdgFile g; } FxDit;

/* destinos opcionais (buffers do chamador) para os estados intermediarios */
typedef struct {
    float *temb;                       /* [3072] */
    float *x_emb, *c_emb;              /* [Ni][3072], [Nt][3072] */
    float *dimg[FXD_NDBL], *dtxt[FXD_NDBL];   /* saida de cada bloco duplo */
    float *sgl[FXD_NSGL];              /* saida de cada bloco simples: [Nt+Ni][3072] */
} FxdProbe;

static int fxd_open(FxDit *F, const char *path) { return sdg_open(&F->g, path); }

static const SdgInfo *fxd_t(const FxDit *F, const char *fmt, int a) {
    char nm[200]; snprintf(nm, sizeof nm, fmt, a);
    const SdgInfo *t = sdg_find(&F->g, nm);
    if (!t) fprintf(stderr, "flux: tensor %s nao existe no GGUF\n", nm);
    return t;
}

/* uma linha (cols valores) de um peso em float */
static void fxd_row(const SdgInfo *t, const uint8_t *data, int row, int cols, float *out) {
    switch (t->type) {
    case T_F32: memcpy(out, data + (size_t)row * cols * 4, (size_t)cols * 4); break;
    case T_BF16: { const uint16_t *h = (const uint16_t *)data + (size_t)row * cols; for (int i = 0; i < cols; i++) { const uint32_t b = (uint32_t)h[i] << 16; memcpy(out + i, &b, 4); } break; }
    case T_Q4_K: dequant(t->type, data + (size_t)row * (cols / 256) * 144, out, (size_t)cols); break;
    case T_Q5_K: dequant(t->type, data + (size_t)row * (cols / 256) * 176, out, (size_t)cols); break;
    case T_F16: dequant(t->type, data + (size_t)row * cols * 2, out, (size_t)cols); break;
    default: fprintf(stderr, "flux: tipo de peso %u nao suportado\n", t->type); memset(out, 0, (size_t)cols * 4);
    }
}

/* paineis do produto: NR linhas do peso viram um painel [cols][NR] (o mesmo layout de vt_pack, com a origem desquantizada) */
typedef struct { const SdgInfo *t; const uint8_t *data; float *B; int rows, cols, NR; } FxdPackJob;
static void fxd_pack_job(void *cx, int j0, int j1) {
    const FxdPackJob *j = cx;
    const int c = j->cols, NR = j->NR;
    float *row = vt_alloc((size_t)c * 4);
    for (int p = j0; p < j1; p++) {
        float *o = j->B + (size_t)p * c * NR;
        for (int r = 0; r < NR; r++) {
            const int rr = p * NR + r;
            if (rr >= j->rows) { for (int i = 0; i < c; i++) o[(size_t)i * NR + r] = 0.0f; continue; }
            fxd_row(j->t, j->data, rr, c, row);
            for (int i = 0; i < c; i++) o[(size_t)i * NR + r] = row[i];
        }
    }
    vt_free(row);
}

/* Y[n][rows] = X[n][cols] * W^T (sem vies). O GGUF guarda [ne0 = entrada, ne1 = saida]. */
static void fxd_lin(const FxDit *F, float *Y, const float *X, int n, const SdgInfo *t) {
    const int cols = (int)t->dim[0], rows = (int)t->dim[1], NR = vt_nr(), np = (rows + NR - 1) / NR;
    float *B = vt_alloc((size_t)np * NR * cols * 4);
    FxdPackJob pj = { t, sdg_data(&F->g, t), B, rows, cols, NR };
    pool_for(fxd_pack_job, &pj, np, 1);
    VtLinJob j = { Y, X, B, NULL, n, cols, rows, (size_t)rows, (size_t)cols };
    pool_for(vt_lin_job, &j, (n + VT_ROWS - 1) / VT_ROWS, 1);
    vt_free(B);
}

/* um vetor (n = 1): y[rows] = W x, sem empacotar */
typedef struct { const SdgInfo *t; const uint8_t *data; const float *x; float *y; int cols; } FxdMv;
static void fxd_mv_job(void *cx, int r0, int r1) {
    const FxdMv *j = cx;
    float *row = xalloc((size_t)j->cols, 4);
    for (int r = r0; r < r1; r++) {
        fxd_row(j->t, j->data, r, j->cols, row);
        double s = 0; for (int i = 0; i < j->cols; i++) s += (double)row[i] * j->x[i];
        j->y[r] = (float)s;
    }
    free(row);
}
static void fxd_mv(const FxDit *F, float *y, const float *x, const SdgInfo *t) {
    FxdMv j = { t, sdg_data(&F->g, t), x, y, (int)t->dim[0] };
    pool_for(fxd_mv_job, &j, (int)t->dim[1], 16);
}

static float fxd_silu(float v) { return v / (1.0f + expf(-v)); }

/* LayerNorm sem parametros (eps 1e-6) e modulacao: Y = LN(X) * (1 + scale) + shift, por linha de d valores */
typedef struct { float *Y; const float *X, *shift, *scale; int d; } FxdLn;
static void fxd_ln_job(void *cx, int r0, int r1) {
    const FxdLn *j = cx; const int d = j->d;
    for (int r = r0; r < r1; r++) {
        const float *x = j->X + (size_t)r * d; float *y = j->Y + (size_t)r * d;
        double s = 0, s2 = 0;
        for (int i = 0; i < d; i++) { s += x[i]; s2 += (double)x[i] * x[i]; }
        const double mean = s / d, var = s2 / d - mean * mean;
        const float m = (float)mean, inv = (float)(1.0 / sqrt((var > 0 ? var : 0) + 1e-6));
        for (int i = 0; i < d; i++) y[i] = (x[i] - m) * inv * (1.0f + j->scale[i]) + j->shift[i];
    }
}
static void fxd_ln_mod(float *Y, const float *X, int n, int d, const float *shift, const float *scale) {
    FxdLn j = { Y, X, shift, scale, d };
    pool_for(fxd_ln_job, &j, n, 8);
}

/* RMSNorm por cabeca (eps 1e-6) com ganho de 128: X[n][heads*128] no lugar */
typedef struct { float *X; const float *w; int stride; } FxdRms;
static void fxd_rms_job(void *cx, int i0, int i1) {
    const FxdRms *j = cx;
    for (int i = i0; i < i1; i++) {                          /* i = token * 24 + cabeca */
        float *x = j->X + (size_t)(i / FXD_HEADS) * j->stride + (size_t)(i % FXD_HEADS) * FXD_HD;
        double s = 0; for (int k = 0; k < FXD_HD; k++) s += (double)x[k] * x[k];
        const float inv = (float)(1.0 / sqrt(s / FXD_HD + 1e-6));
        for (int k = 0; k < FXD_HD; k++) x[k] = x[k] * inv * j->w[k];
    }
}
static void fxd_rms(float *X, int n, int stride, const float *w) { FxdRms j = { X, w, stride }; pool_for(fxd_rms_job, &j, n * FXD_HEADS, 16); }

/* RoPE: tabelas cos/sin [n][128] dos ids [n][4]: eixo a usa as dims 32a..32a+31, frequencias 1/theta^(2j/32), cada uma repetida em par */
static void fxd_rope_table(float *cs, float *sn, const float *ids, int n) {
    for (int i = 0; i < n; i++)
        for (int a = 0; a < 4; a++)
            for (int jj = 0; jj < 16; jj++) {
                const double fr = 1.0 / pow(2000.0, (double)(2 * jj) / 32.0), ang = (double)ids[(size_t)i * 4 + a] * fr;
                const float c = (float)cos(ang), s = (float)sin(ang);
                cs[(size_t)i * 128 + a * 32 + 2 * jj] = cs[(size_t)i * 128 + a * 32 + 2 * jj + 1] = c;
                sn[(size_t)i * 128 + a * 32 + 2 * jj] = sn[(size_t)i * 128 + a * 32 + 2 * jj + 1] = s;
            }
}
typedef struct { float *X; const float *cs, *sn; } FxdRope;
static void fxd_rope_job(void *cx, int i0, int i1) {
    const FxdRope *j = cx;
    for (int i = i0; i < i1; i++) {
        const int tok = i / FXD_HEADS, h = i % FXD_HEADS;
        float *x = j->X + (size_t)tok * FXD_DIM + (size_t)h * FXD_HD;
        const float *c = j->cs + (size_t)tok * 128, *s = j->sn + (size_t)tok * 128;
        for (int k = 0; k < FXD_HD; k += 2) {
            const float a = x[k], b = x[k + 1];
            x[k] = a * c[k] - b * s[k];
            x[k + 1] = b * c[k + 1] + a * s[k + 1];
        }
    }
}
static void fxd_rope(float *X, int n, const float *cs, const float *sn) { FxdRope j = { X, cs, sn }; pool_for(fxd_rope_job, &j, n * FXD_HEADS, 16); }

/* atencao: Q, K, V [n][3072] (cabeca h nas colunas 128h..128h+127), sem mascara; O [n][3072] */
typedef struct { const float *Q, *K, *V; float *O; int n; } FxdAtt;
static void fxd_att_job(void *cx, int i0, int i1) {
    const FxdAtt *j = cx; const int n = j->n;
    float *sc = xalloc((size_t)n, 4);
    const float scale = 1.0f / sqrtf((float)FXD_HD);
    for (int i = i0; i < i1; i++) {                          /* i = cabeca * n + consulta */
        const int h = i / n, q = i % n;
        const float *qv = j->Q + (size_t)q * FXD_DIM + (size_t)h * FXD_HD;
        float mx = -1e30f;
        for (int k = 0; k < n; k++) {
            const float *kv = j->K + (size_t)k * FXD_DIM + (size_t)h * FXD_HD;
            float s = 0; for (int d = 0; d < FXD_HD; d++) s += qv[d] * kv[d];
            sc[k] = s * scale; if (sc[k] > mx) mx = sc[k];
        }
        double den = 0; for (int k = 0; k < n; k++) { sc[k] = expf(sc[k] - mx); den += sc[k]; }
        const float inv = (float)(1.0 / den);
        float *o = j->O + (size_t)q * FXD_DIM + (size_t)h * FXD_HD;
        for (int d = 0; d < FXD_HD; d++) o[d] = 0;
        for (int k = 0; k < n; k++) {
            const float w = sc[k] * inv; const float *vv = j->V + (size_t)k * FXD_DIM + (size_t)h * FXD_HD;
            for (int d = 0; d < FXD_HD; d++) o[d] += w * vv[d];
        }
    }
    free(sc);
}
static void fxd_attention(float *O, const float *Q, const float *K, const float *V, int n) { FxdAtt j = { Q, K, V, O, n }; pool_for(fxd_att_job, &j, n * FXD_HEADS, 4); }

/* SwiGLU: H[n][2m] -> Y[n][m] = silu(H[:, :m]) * H[:, m:], com passo de entrada ldh */
typedef struct { float *Y; const float *H; int m; size_t ldh; } FxdSw;
static void fxd_sw_job(void *cx, int r0, int r1) {
    const FxdSw *j = cx;
    for (int r = r0; r < r1; r++) { const float *h = j->H + (size_t)r * j->ldh; float *y = j->Y + (size_t)r * j->m; for (int i = 0; i < j->m; i++) y[i] = fxd_silu(h[i]) * h[j->m + i]; }
}
static void fxd_swiglu(float *Y, const float *H, int n, int m, size_t ldh) { FxdSw j = { Y, H, m, ldh }; pool_for(fxd_sw_job, &j, n, 4); }

static void fxd_axpy_gate(float *X, const float *G, const float *Y, int n, int d) {       /* X += gate * Y (gate por coluna) */
    for (int r = 0; r < n; r++) for (int i = 0; i < d; i++) X[(size_t)r * d + i] += G[i] * Y[(size_t)r * d + i];
}


/* ganho de 128 de uma norma de q/k (F32 no GGUF); o chamador libera */
static float *fxd_scale(const FxDit *F, const char *fmt, int a) {
    char nm[200]; snprintf(nm, sizeof nm, fmt, a);
    uint64_t n = 0; float *w = sdg_floats(&F->g, nm, &n);
    if (w && n != FXD_HD) { free(w); return NULL; }
    return w;
}

/* um bloco duplo: img [Ni][3072] e txt [Nt][3072] mudam no lugar; mi/mt = modulacoes de imagem e de texto [6*3072] */
static int fxd_double(const FxDit *F, int b, float *img, int Ni, float *txt, int Nt, const float *mi, const float *mt, const float *cs, const float *sn) {
    const int D = FXD_DIM, N = Nt + Ni;
    const SdgInfo *iqkv = fxd_t(F, "double_blocks.%d.img_attn.qkv.weight", b), *tqkv = fxd_t(F, "double_blocks.%d.txt_attn.qkv.weight", b);
    const SdgInfo *iprj = fxd_t(F, "double_blocks.%d.img_attn.proj.weight", b), *tprj = fxd_t(F, "double_blocks.%d.txt_attn.proj.weight", b);
    const SdgInfo *im0 = fxd_t(F, "double_blocks.%d.img_mlp.0.weight", b), *im2 = fxd_t(F, "double_blocks.%d.img_mlp.2.weight", b);
    const SdgInfo *tm0 = fxd_t(F, "double_blocks.%d.txt_mlp.0.weight", b), *tm2 = fxd_t(F, "double_blocks.%d.txt_mlp.2.weight", b);
    float *iq = fxd_scale(F, "double_blocks.%d.img_attn.norm.query_norm.scale", b), *ik = fxd_scale(F, "double_blocks.%d.img_attn.norm.key_norm.scale", b);
    float *tq = fxd_scale(F, "double_blocks.%d.txt_attn.norm.query_norm.scale", b), *tk = fxd_scale(F, "double_blocks.%d.txt_attn.norm.key_norm.scale", b);
    if (!iqkv || !tqkv || !iprj || !tprj || !im0 || !im2 || !tm0 || !tm2 || !iq || !ik || !tq || !tk) return 0;
    const float *i_sh1 = mi, *i_sc1 = mi + D, *i_g1 = mi + 2 * D, *i_sh2 = mi + 3 * D, *i_sc2 = mi + 4 * D, *i_g2 = mi + 5 * D;
    const float *t_sh1 = mt, *t_sc1 = mt + D, *t_g1 = mt + 2 * D, *t_sh2 = mt + 3 * D, *t_sc2 = mt + 4 * D, *t_g2 = mt + 5 * D;
    float *xn = xalloc((size_t)(Ni > Nt ? Ni : Nt) * D, 4);
    float *qkv = xalloc((size_t)(Ni > Nt ? Ni : Nt) * 3 * D, 4);
    float *Q = xalloc((size_t)N * D, 4), *K = xalloc((size_t)N * D, 4), *V = xalloc((size_t)N * D, 4), *A = xalloc((size_t)N * D, 4);
    /* texto primeiro, imagem depois */
    fxd_ln_mod(xn, txt, Nt, D, t_sh1, t_sc1); fxd_lin(F, qkv, xn, Nt, tqkv);
    for (int r = 0; r < Nt; r++) { memcpy(Q + (size_t)r * D, qkv + (size_t)r * 3 * D, (size_t)D * 4); memcpy(K + (size_t)r * D, qkv + (size_t)r * 3 * D + D, (size_t)D * 4); memcpy(V + (size_t)r * D, qkv + (size_t)r * 3 * D + 2 * D, (size_t)D * 4); }
    fxd_rms(Q, Nt, D, tq); fxd_rms(K, Nt, D, tk);
    fxd_ln_mod(xn, img, Ni, D, i_sh1, i_sc1); fxd_lin(F, qkv, xn, Ni, iqkv);
    for (int r = 0; r < Ni; r++) { const size_t o = (size_t)(Nt + r) * D; memcpy(Q + o, qkv + (size_t)r * 3 * D, (size_t)D * 4); memcpy(K + o, qkv + (size_t)r * 3 * D + D, (size_t)D * 4); memcpy(V + o, qkv + (size_t)r * 3 * D + 2 * D, (size_t)D * 4); }
    fxd_rms(Q + (size_t)Nt * D, Ni, D, iq); fxd_rms(K + (size_t)Nt * D, Ni, D, ik);
    fxd_rope(Q, N, cs, sn); fxd_rope(K, N, cs, sn);
    fxd_attention(A, Q, K, V, N);
    float *o = xalloc((size_t)(Ni > Nt ? Ni : Nt) * D, 4);
    fxd_lin(F, o, A + (size_t)Nt * D, Ni, iprj); fxd_axpy_gate(img, i_g1, o, Ni, D);
    fxd_lin(F, o, A, Nt, tprj); fxd_axpy_gate(txt, t_g1, o, Nt, D);
    /* MLP de cada fluxo */
    const int M2 = 2 * FXD_MLP;
    float *h = xalloc((size_t)(Ni > Nt ? Ni : Nt) * M2, 4), *hs = xalloc((size_t)(Ni > Nt ? Ni : Nt) * FXD_MLP, 4);
    fxd_ln_mod(xn, img, Ni, D, i_sh2, i_sc2); fxd_lin(F, h, xn, Ni, im0); fxd_swiglu(hs, h, Ni, FXD_MLP, (size_t)M2); fxd_lin(F, o, hs, Ni, im2); fxd_axpy_gate(img, i_g2, o, Ni, D);
    fxd_ln_mod(xn, txt, Nt, D, t_sh2, t_sc2); fxd_lin(F, h, xn, Nt, tm0); fxd_swiglu(hs, h, Nt, FXD_MLP, (size_t)M2); fxd_lin(F, o, hs, Nt, tm2); fxd_axpy_gate(txt, t_g2, o, Nt, D);
    free(xn); free(qkv); free(Q); free(K); free(V); free(A); free(o); free(h); free(hs); free(iq); free(ik); free(tq); free(tk);
    return 1;
}

/* um bloco simples sobre x [N][3072] (texto + imagem): atencao e MLP em paralelo; ms = modulacao [3*3072] */
static int fxd_single(const FxDit *F, int b, float *x, int N, const float *ms, const float *cs, const float *sn) {
    const int D = FXD_DIM, L1 = 3 * D + 2 * FXD_MLP;
    const SdgInfo *l1 = fxd_t(F, "single_blocks.%d.linear1.weight", b), *l2 = fxd_t(F, "single_blocks.%d.linear2.weight", b);
    float *wq = fxd_scale(F, "single_blocks.%d.norm.query_norm.scale", b), *wk = fxd_scale(F, "single_blocks.%d.norm.key_norm.scale", b);
    if (!l1 || !l2 || !wq || !wk) return 0;
    if ((int)l1->dim[1] != L1 || (int)l2->dim[0] != D + FXD_MLP) { fprintf(stderr, "flux: formas inesperadas no bloco simples %d\n", b); return 0; }
    float *xn = xalloc((size_t)N * D, 4), *h = xalloc((size_t)N * L1, 4);
    float *Q = xalloc((size_t)N * D, 4), *K = xalloc((size_t)N * D, 4), *V = xalloc((size_t)N * D, 4), *A = xalloc((size_t)N * D, 4);
    fxd_ln_mod(xn, x, N, D, ms, ms + D);
    fxd_lin(F, h, xn, N, l1);
    for (int r = 0; r < N; r++) { const float *p = h + (size_t)r * L1; memcpy(Q + (size_t)r * D, p, (size_t)D * 4); memcpy(K + (size_t)r * D, p + D, (size_t)D * 4); memcpy(V + (size_t)r * D, p + 2 * D, (size_t)D * 4); }
    fxd_rms(Q, N, D, wq); fxd_rms(K, N, D, wk);
    fxd_rope(Q, N, cs, sn); fxd_rope(K, N, cs, sn);
    fxd_attention(A, Q, K, V, N);
    const int C2 = D + FXD_MLP;
    float *cat = xalloc((size_t)N * C2, 4), *o = xalloc((size_t)N * D, 4);
    for (int r = 0; r < N; r++) memcpy(cat + (size_t)r * C2, A + (size_t)r * D, (size_t)D * 4);
    {   /* parte do MLP: silu(primeira metade) * segunda metade, escrita depois das D colunas da atencao */
        for (int r = 0; r < N; r++) { const float *hm = h + (size_t)r * L1 + 3 * D; float *y = cat + (size_t)r * C2 + D; for (int i = 0; i < FXD_MLP; i++) y[i] = fxd_silu(hm[i]) * hm[FXD_MLP + i]; }
    }
    fxd_lin(F, o, cat, N, l2);
    fxd_axpy_gate(x, ms + 2 * D, o, N, D);
    free(xn); free(h); free(Q); free(K); free(V); free(A); free(cat); free(o); free(wq); free(wk);
    return 1;
}

static void fxd_probe_copy(float *dst, const float *src, size_t n) { if (dst) memcpy(dst, src, n * 4); }

static int fxd_forward(const FxDit *F, const float *img, const float *ids_img, int Ni, const float *txt, const float *ids_txt, int Nt, float t, float *out, const FxdProbe *pr) {
    const int D = FXD_DIM, N = Nt + Ni;
    /* 1. embedding do passo: senoidal de 256 (cos, sin), depois linear, SiLU, linear */
    float temb[FXD_DIM], te[256], hbuf[FXD_DIM];
    {
        const float tf = t * 1000.0f;
        for (int i = 0; i < 128; i++) { const float ex = -logf(10000.0f) * (float)i / 128.0f, a = tf * expf(ex); te[i] = cosf(a); te[128 + i] = sinf(a); }
        const SdgInfo *l1 = fxd_t(F, "time_in.in_layer.weight", 0), *l2 = fxd_t(F, "time_in.out_layer.weight", 0);
        if (!l1 || !l2) return 0;
        fxd_mv(F, hbuf, te, l1);
        for (int i = 0; i < D; i++) hbuf[i] = fxd_silu(hbuf[i]);
        fxd_mv(F, temb, hbuf, l2);
    }
    fxd_probe_copy(pr ? pr->temb : NULL, temb, D);
    /* 2. modulacoes: linear(SiLU(temb)) */
    float st[FXD_DIM]; for (int i = 0; i < D; i++) st[i] = fxd_silu(temb[i]);
    float *mi = xalloc((size_t)6 * D, 4), *mt = xalloc((size_t)6 * D, 4), *ms = xalloc((size_t)3 * D, 4);
    const SdgInfo *wmi = fxd_t(F, "double_stream_modulation_img.lin.weight", 0), *wmt = fxd_t(F, "double_stream_modulation_txt.lin.weight", 0), *wms = fxd_t(F, "single_stream_modulation.lin.weight", 0);
    if (!wmi || !wmt || !wms) return 0;
    fxd_mv(F, mi, st, wmi); fxd_mv(F, mt, st, wmt); fxd_mv(F, ms, st, wms);
    /* 3. projecoes de entrada */
    const SdgInfo *wi = fxd_t(F, "img_in.weight", 0), *wc = fxd_t(F, "txt_in.weight", 0);
    if (!wi || !wc) return 0;
    float *x = xalloc((size_t)Ni * D, 4), *c = xalloc((size_t)Nt * D, 4);
    fxd_lin(F, x, img, Ni, wi); fxd_lin(F, c, txt, Nt, wc);
    fxd_probe_copy(pr ? pr->x_emb : NULL, x, (size_t)Ni * D); fxd_probe_copy(pr ? pr->c_emb : NULL, c, (size_t)Nt * D);
    /* 4. RoPE: texto primeiro, imagem depois */
    float *cs = xalloc((size_t)N * 128, 4), *sn = xalloc((size_t)N * 128, 4);
    fxd_rope_table(cs, sn, ids_txt, Nt); fxd_rope_table(cs + (size_t)Nt * 128, sn + (size_t)Nt * 128, ids_img, Ni);
    /* 5. blocos duplos */
    for (int b = 0; b < FXD_NDBL; b++) {
        if (!fxd_double(F, b, x, Ni, c, Nt, mi, mt, cs, sn)) return 0;
        if (pr) { fxd_probe_copy(pr->dimg[b], x, (size_t)Ni * D); fxd_probe_copy(pr->dtxt[b], c, (size_t)Nt * D); }
    }
    /* 6. blocos simples sobre [texto; imagem] */
    float *z = xalloc((size_t)N * D, 4);
    memcpy(z, c, (size_t)Nt * D * 4); memcpy(z + (size_t)Nt * D, x, (size_t)Ni * D * 4);
    for (int b = 0; b < FXD_NSGL; b++) {
        if (!fxd_single(F, b, z, N, ms, cs, sn)) return 0;
        if (pr) fxd_probe_copy(pr->sgl[b], z, (size_t)N * D);
    }
    /* 7. camada final: so os tokens de imagem; (shift, scale) = linear(SiLU(temb)); LN, modulacao, projecao */
    const SdgInfo *wf = fxd_t(F, "final_layer.adaLN_modulation.1.weight", 0), *wo = fxd_t(F, "final_layer.linear.weight", 0);
    if (!wf || !wo) return 0;
    float emb[2 * FXD_DIM], *y = xalloc((size_t)Ni * D, 4);
    fxd_mv(F, emb, st, wf);
    fxd_ln_mod(y, z + (size_t)Nt * D, Ni, D, emb, emb + D);
    fxd_lin(F, out, y, Ni, wo);
    free(mi); free(mt); free(ms); free(x); free(c); free(cs); free(sn); free(z); free(y);
    return 1;
}

/* ---------- teste: contra a referencia em float32 (ref_dit.py) ---------- */
static float *fxd_ref(const char *dir, const char *name, size_t *n) { return sdt_ref(dir, name, n); }
static int fxd_cmp(const char *what, const float *a, const float *b, size_t n) {
    double mx = 0, mr = 0, sum = 0;
    for (size_t i = 0; i < n; i++) { const double d = fabs((double)a[i] - b[i]); if (!(d <= mx)) mx = d; sum += d; if (fabs(b[i]) > mr) mr = fabs(b[i]); }
    const double ratio = mr > 0 ? mx / mr : mx, mean = sum / (double)n / (mr > 0 ? mr : 1.0);
    const int ok = ratio < 1e-3;
    printf("  %-30s %s  erro max %.3e  |ref| max %.3f  razao %.2e  media %.1e\n", what, ok ? "OK    " : "FALHOU", mx, mr, ratio, mean);
    return ok ? 0 : 1;
}

static int fxd_main(int argc, char **argv) {
    if (argc < 1 || strcmp(argv[0], "selftest")) { fprintf(stderr, "uso: katana render flux-dit selftest [pasta-referencia] [flux2-klein.gguf]\n"); return 1; }
    const char *dir = argc > 1 ? argv[1] : "/mnt/dados/render-ref/fluxB";
    char gp[1100]; snprintf(gp, sizeof gp, "%s/flux2klein/gguf/flux-2-klein-4b-Q4_K_M.gguf", models_dir());
    const char *gguf = argc > 2 ? argv[2] : gp;
    FxDit F; if (!fxd_open(&F, gguf)) return 1;
    size_t n_img, n_txt, n_ids_i, n_ids_t;
    float *img = fxd_ref(dir, "in_img", &n_img), *txt = fxd_ref(dir, "in_txt", &n_txt), *ii = fxd_ref(dir, "ids_img", &n_ids_i), *it = fxd_ref(dir, "ids_txt", &n_ids_t);
    if (!img || !txt || !ii || !it) return 1;
    const int Ni = (int)(n_img / FXD_INCH), Nt = (int)(n_txt / FXD_TXTCH), N = Ni + Nt;
    FxdProbe pr; memset(&pr, 0, sizeof pr);
    float temb[FXD_DIM]; pr.temb = temb;
    pr.x_emb = xalloc((size_t)Ni * FXD_DIM, 4); pr.c_emb = xalloc((size_t)Nt * FXD_DIM, 4);
    for (int b = 0; b < FXD_NDBL; b++) { pr.dimg[b] = xalloc((size_t)Ni * FXD_DIM, 4); pr.dtxt[b] = xalloc((size_t)Nt * FXD_DIM, 4); }
    const int sg[3] = { 0, 4, 19 };
    for (int k = 0; k < 3; k++) pr.sgl[sg[k]] = xalloc((size_t)N * FXD_DIM, 4);
    float *out = xalloc((size_t)Ni * FXD_INCH, 4);
    if (!fxd_forward(&F, img, ii, Ni, txt, it, Nt, 0.5f, out, &pr)) return 1;
    int bad = 0; size_t n; char nm[64];
    float *r = fxd_ref(dir, "temb", &n); bad += fxd_cmp("embedding do passo", temb, r, n); free(r);
    r = fxd_ref(dir, "x_emb", &n); bad += fxd_cmp("img_in", pr.x_emb, r, n); free(r);
    r = fxd_ref(dir, "c_emb", &n); bad += fxd_cmp("txt_in", pr.c_emb, r, n); free(r);
    for (int b = 0; b < FXD_NDBL; b++) {
        snprintf(nm, sizeof nm, "dbl%d_img", b); r = fxd_ref(dir, nm, &n); snprintf(nm, sizeof nm, "bloco duplo %d (imagem)", b); bad += fxd_cmp(nm, pr.dimg[b], r, n); free(r);
        snprintf(nm, sizeof nm, "dbl%d_txt", b); r = fxd_ref(dir, nm, &n); snprintf(nm, sizeof nm, "bloco duplo %d (texto)", b); bad += fxd_cmp(nm, pr.dtxt[b], r, n); free(r);
    }
    for (int k = 0; k < 3; k++) { snprintf(nm, sizeof nm, "sgl%d", sg[k]); r = fxd_ref(dir, nm, &n); snprintf(nm, sizeof nm, "bloco simples %d", sg[k]); bad += fxd_cmp(nm, pr.sgl[sg[k]], r, n); free(r); }
    r = fxd_ref(dir, "out", &n); bad += fxd_cmp("saida final [Ni][128]", out, r, n); free(r);
    printf("RESULTADO: %s\n", bad ? "FALHOU" : "todos passaram");
    return bad ? 1 : 0;
}
