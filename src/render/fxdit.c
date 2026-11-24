/* ============================ render: transformador do FLUX.2 klein 4B (CPU) ===============================
 * katana render flux-dit selftest [pasta-referencia] [flux2-klein.gguf]
 *
 * O Flux2Transformer2DModel inteiro: projecoes de entrada, embedding do passo, modulacoes, 5 blocos duplos (fluxos de imagem e de texto com
 * atencao conjunta), 20 blocos simples (atencao e MLP em paralelo num so produto) e a camada final com normalizacao adaptativa.
 * Os pesos ficam no GGUF mapeado, com os nomes do formato original do modelo (double_blocks.N.img_attn.qkv.weight ...), em F32, BF16,
 * Q4_K e Q5_K. Cada peso e desquantizado uma unica vez para os paineis do produto de matrizes (vision/tensor.c) e fica em cache
 * (~15 GB em float32, KATANA_FLUX_WEIGHTS muda isso); os produtos rodam em float32 em tarefas de duas dimensoes (fatia do peso x bloco de
 * linhas) e a atencao e feita por produtos de matrizes em blocos de consultas, sem materializar a matriz n x n. Comandos de apoio:
 * `katana render flux-dit bench [Ni] [Nt]` (tempo por etapa) e `attn-test [n]` (atencao por produtos contra a versao simples).
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

/* contadores de tempo por etapa (KATANA_FLUX_PROF=1 ou o comando bench): empacotamento/desquantizacao, produtos, atencao */
enum { FT_PACK, FT_GEMM, FT_ATT, FT_N };
static double g_fxd_tm[FT_N]; static int g_fxd_prof;
#define FXD_TIC() (g_fxd_prof ? sdt_now() : 0.0)
#define FXD_TOC(k, t0) do { if (g_fxd_prof) g_fxd_tm[k] += sdt_now() - (t0); } while (0)

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
typedef struct { const SdgInfo *t; const uint8_t *data; float *B; uint16_t *H; int rows, cols, NR; } FxdPackJob;
static void fxd_pack_job(void *cx, int j0, int j1) {
    const FxdPackJob *j = cx;
    const int c = j->cols, NR = j->NR;
    float *row = vt_alloc((size_t)c * 4);
    for (int p = j0; p < j1; p++) {
        float *o = j->B ? j->B + (size_t)p * c * NR : NULL;
        uint16_t *oh = j->H ? j->H + (size_t)p * c * NR : NULL;
        for (int r = 0; r < NR; r++) {
            const int rr = p * NR + r;
            if (rr >= j->rows) { for (int i = 0; i < c; i++) { if (oh) oh[(size_t)i * NR + r] = 0; else o[(size_t)i * NR + r] = 0.0f; } continue; }
            fxd_row(j->t, j->data, rr, c, row);
            if (oh) for (int i = 0; i < c; i++) oh[(size_t)i * NR + r] = _cvtss_sh(row[i], 0);
            else for (int i = 0; i < c; i++) o[(size_t)i * NR + r] = row[i];
        }
    }
    vt_free(row);
}

/* ---- produto com pesos em cache: paineis F16 (ou F32), montados uma vez por tensor e reaproveitados em todas as chamadas ----
 * KATANA_FLUX_WEIGHTS: f32 (padrao, paineis float: ~15 GB, exato), f16 (metade da memoria; o peso desquantizado e arredondado para 10 bits de
 * mantissa, a conversao acontece dentro do laco do produto; o erro da saida passa de 1e-3 e o produto fica mais lento, so serve a quem nao tem
 * a memoria), none (desquantiza a cada chamada, o caminho original). */
typedef struct { const SdgInfo *t; float *f; uint16_t *h; } FxdWE;
static FxdWE g_fxd_wc[320]; static int g_fxd_nwc;
static int fxd_wmode(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("KATANA_FLUX_WEIGHTS"); m = !e || !strcmp(e, "f32") ? 2 : !strcmp(e, "f16") ? 1 : 0; }
    return m;
}
static const FxdWE *fxd_wget(const FxDit *F, const SdgInfo *t) {
    for (int i = 0; i < g_fxd_nwc; i++) if (g_fxd_wc[i].t == t) return &g_fxd_wc[i];
    if (g_fxd_nwc >= 320) return NULL;
    const int cols = (int)t->dim[0], rows = (int)t->dim[1], NR = vt_nr(), np = (rows + NR - 1) / NR, mode = fxd_wmode();
    FxdWE *e = &g_fxd_wc[g_fxd_nwc++]; e->t = t;
    FxdPackJob pj = { t, sdg_data(&F->g, t), NULL, NULL, rows, cols, NR };
    if (mode == 1) { e->h = vt_alloc((size_t)np * NR * cols * 2); pj.H = e->h; } else { e->f = vt_alloc((size_t)np * NR * cols * 4); pj.B = e->f; }
    pool_for(fxd_pack_job, &pj, np, 1);
    return e;
}

/* nucleo com painel F16: igual ao do produto de vision/tensor.c, com b carregado como metades e convertido */
VT_512 VT_INL void fxd_k512h(const int mr, const float *a, size_t lda, const uint16_t *b, int k, float *c, size_t ldc, int add) {
    __m512 x[12], y[12];
#pragma GCC unroll 12
    for (int i = 0; i < mr; i++) { x[i] = _mm512_setzero_ps(); y[i] = _mm512_setzero_ps(); }
    for (int p = 0; p < k; p++, b += 32) {
        const __m512 b0 = _mm512_cvtph_ps(_mm256_load_si256((const __m256i *)b)), b1 = _mm512_cvtph_ps(_mm256_load_si256((const __m256i *)(b + 16)));
#pragma GCC unroll 12
        for (int i = 0; i < mr; i++) {
            const __m512 v = _mm512_set1_ps(a[(size_t)i * lda + p]);
            x[i] = _mm512_fmadd_ps(v, b0, x[i]); y[i] = _mm512_fmadd_ps(v, b1, y[i]);
        }
    }
#pragma GCC unroll 12
    for (int i = 0; i < mr; i++) {
        float *o = c + (size_t)i * ldc;
        if (add) { x[i] = _mm512_add_ps(x[i], _mm512_loadu_ps(o)); y[i] = _mm512_add_ps(y[i], _mm512_loadu_ps(o + 16)); }
        _mm512_storeu_ps(o, x[i]); _mm512_storeu_ps(o + 16, y[i]);
    }
}
VT_512 static void fxd_kern512h(int mr, const float *a, size_t lda, const uint16_t *b, int k, float *c, size_t ldc, int add) {
    switch (mr) {
#define K(n) case n: fxd_k512h(n, a, lda, b, k, c, ldc, add); break;
    K(1) K(2) K(3) K(4) K(5) K(6) K(7) K(8) K(9) K(10) K(11) K(12)
#undef K
    }
}
VT_INL void fxd_k256h(const int mr, const float *a, size_t lda, const uint16_t *b, int k, float *c, size_t ldc, int add) {
    __m256 x[6], y[6];
#pragma GCC unroll 6
    for (int i = 0; i < mr; i++) { x[i] = _mm256_setzero_ps(); y[i] = _mm256_setzero_ps(); }
    for (int p = 0; p < k; p++, b += 16) {
        const __m256 b0 = _mm256_cvtph_ps(_mm_load_si128((const __m128i *)b)), b1 = _mm256_cvtph_ps(_mm_load_si128((const __m128i *)(b + 8)));
#pragma GCC unroll 6
        for (int i = 0; i < mr; i++) {
            const __m256 v = _mm256_broadcast_ss(a + (size_t)i * lda + p);
            x[i] = _mm256_fmadd_ps(v, b0, x[i]); y[i] = _mm256_fmadd_ps(v, b1, y[i]);
        }
    }
#pragma GCC unroll 6
    for (int i = 0; i < mr; i++) {
        float *o = c + (size_t)i * ldc;
        if (add) { x[i] = _mm256_add_ps(x[i], _mm256_loadu_ps(o)); y[i] = _mm256_add_ps(y[i], _mm256_loadu_ps(o + 8)); }
        _mm256_storeu_ps(o, x[i]); _mm256_storeu_ps(o + 8, y[i]);
    }
}
static void fxd_kern256h(int mr, const float *a, size_t lda, const uint16_t *b, int k, float *c, size_t ldc, int add) {
    switch (mr) {
#define K(n) case n: fxd_k256h(n, a, lda, b, k, c, ldc, add); break;
    K(1) K(2) K(3) K(4) K(5) K(6)
#undef K
    }
}
/* C[n][m] = A[n][k] * paineis F16, so os paineis pa..pb-1 (colunas de C pa*NR em diante) */
static void fxd_gemm_h(float *C, size_t ldc, const float *A, size_t lda, const uint16_t *B, int n, int k, int m, int pa, int pb) {
    const int wide = vt_wide(), NR = wide ? 32 : 16, MR = wide ? 12 : 6, KC = 512;
    for (int p0 = 0; p0 < k; p0 += KC) {
        const int kc = k - p0 < KC ? k - p0 : KC, add = p0 > 0;
        for (int p = pa; p < pb; p++) {
            const int j = p * NR, w = m - j < NR ? m - j : NR;
            const uint16_t *b = B + (size_t)p * k * NR + (size_t)p0 * NR;
            for (int i = 0; i < n; i += MR) {
                const int mr = n - i < MR ? n - i : MR;
                const float *a = A + (size_t)i * lda + p0;
                float *c = C + (size_t)i * ldc + j;
                if (w == NR) { if (wide) fxd_kern512h(mr, a, lda, b, kc, c, ldc, add); else fxd_kern256h(mr, a, lda, b, kc, c, ldc, add); continue; }
                float t[12 * 32];
                if (add) for (int r = 0; r < mr; r++) { memset(t + r * NR, 0, (size_t)NR * 4); memcpy(t + r * NR, c + (size_t)r * ldc, (size_t)w * 4); }
                if (wide) fxd_kern512h(mr, a, lda, b, kc, t, (size_t)NR, add); else fxd_kern256h(mr, a, lda, b, kc, t, (size_t)NR, add);
                for (int r = 0; r < mr; r++) memcpy(c + (size_t)r * ldc, t + r * NR, (size_t)w * 4);
            }
        }
    }
}

/* tarefas em duas dimensoes: grupos de paineis (fatia do peso que cabe em ~2 MB) x blocos de FXD_RB linhas; o indice varre os blocos de
 * linhas por dentro, assim as threads que rodam juntas dividem a mesma fatia de peso na cache. Poucas linhas (texto) ainda enchem as threads. */
#define FXD_RB 48
typedef struct { float *Y; const float *X; const FxdWE *w; int n, k, m, NR, G, np, nrb; } FxdGj;
static void fxd_gj_job(void *cx, int j0, int j1) {
    const FxdGj *j = cx;
    for (int idx = j0; idx < j1; idx++) {
        const int pg = idx / j->nrb, rb = idx % j->nrb, r0 = rb * FXD_RB, nn = j->n - r0 < FXD_RB ? j->n - r0 : FXD_RB;
        const int pa = pg * j->G, pb = pa + j->G < j->np ? pa + j->G : j->np;
        float *y = j->Y + (size_t)r0 * j->m; const float *x = j->X + (size_t)r0 * j->k;
        if (j->w->h) fxd_gemm_h(y, (size_t)j->m, x, (size_t)j->k, j->w->h, nn, j->k, j->m, pa, pb);
        else { const int c0 = pa * j->NR, mm = (pb * j->NR < j->m ? pb * j->NR : j->m) - c0; vt_gemm_packed(y + c0, (size_t)j->m, x, (size_t)j->k, j->w->f + (size_t)pa * j->k * j->NR, nn, j->k, mm); }
    }
}

/* Y[n][rows] = X[n][cols] * W^T (sem vies). O GGUF guarda [ne0 = entrada, ne1 = saida]. */
static void fxd_lin0(const FxDit *F, float *Y, const float *X, int n, const SdgInfo *t) {
    const int cols = (int)t->dim[0], rows = (int)t->dim[1], NR = vt_nr(), np = (rows + NR - 1) / NR;
    { const double t0 = FXD_TIC(); if (fxv_lin(&F->g, Y, X, n, t)) { FXD_TOC(FT_GEMM, t0); return; } }
    if (fxd_wmode()) {
        double t0 = FXD_TIC();
        const FxdWE *w = fxd_wget(F, t);
        FXD_TOC(FT_PACK, t0); t0 = FXD_TIC();
        if (w) {
            const size_t es = w->h ? 2 : 4;
            int G = (int)((2u << 20) / ((size_t)cols * NR * es)); if (G < 1) G = 1;
            const int nrb = (n + FXD_RB - 1) / FXD_RB, npg = (np + G - 1) / G;
            FxdGj j = { Y, X, w, n, cols, rows, NR, G, np, nrb };
            pool_for(fxd_gj_job, &j, npg * nrb, 1);
            FXD_TOC(FT_GEMM, t0);
            return;
        }
    }
    double t0 = FXD_TIC();
    float *B = vt_alloc((size_t)np * NR * cols * 4);
    FxdPackJob pj = { t, sdg_data(&F->g, t), B, NULL, rows, cols, NR };
    pool_for(fxd_pack_job, &pj, np, 1);
    FXD_TOC(FT_PACK, t0); t0 = FXD_TIC();
    VtLinJob j = { Y, X, B, NULL, n, cols, rows, (size_t)rows, (size_t)cols };
    pool_for(vt_lin_job, &j, (n + VT_ROWS - 1) / VT_ROWS, 1);
    vt_free(B);
    FXD_TOC(FT_GEMM, t0);
}

/* adaptador de baixo posto (fxlora.c): Y += escala * (X A^T) B^T quando ha um .fxlora ativo e ele cobre este peso; sem adaptador, so um teste de inteiro */
static int g_fxl_on;
static void fxl_apply(float *Y, const float *X, int n, const SdgInfo *t);       /* fxlora.c */
static void fxd_lin(const FxDit *F, float *Y, const float *X, int n, const SdgInfo *t) {
    fxd_lin0(F, Y, X, n, t);
    if (g_fxl_on) fxl_apply(Y, X, n, t);
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
static void fxd_attention_simple(float *O, const float *Q, const float *K, const float *V, int n) { FxdAtt j = { Q, K, V, O, n }; pool_for(fxd_att_job, &j, n * FXD_HEADS, 4); }

/* atencao por produtos de matrizes: por cabeca, K vira paineis [painel de n][128][NR] (ja multiplicado por 1/sqrt(128)) e V paineis
 * [painel de colunas][n][NR]; cada bloco de FXD_AQ consultas faz sc = Q K^T (produto), softmax por linha, O = sc V (produto). A matriz de
 * pontuacoes nunca existe inteira: so FXD_AQ x n por tarefa (~260 KB com 2700 tokens). Cabecas e blocos de consultas rodam em paralelo. */
#define FXD_AQ 48
typedef struct { const float *Q, *K, *V; float *O, *KP, *VP; int n, NR, np, nv, nqb; float scale; } FxdAg;
static void fxd_ag_packk(void *cx, int j0, int j1) {
    const FxdAg *j = cx;
    for (int idx = j0; idx < j1; idx++) {
        const int h = idx / j->np, p = idx % j->np;
        for (int r = 0; r < j->NR; r++) {
            const int t = p * j->NR + r;
            const float *k = t < j->n ? j->K + (size_t)t * FXD_DIM + (size_t)h * FXD_HD : NULL;
            float *o = j->KP + ((size_t)h * j->np + p) * FXD_HD * j->NR + r;
            for (int i = 0; i < FXD_HD; i++) o[(size_t)i * j->NR] = k ? k[i] * j->scale : 0.0f;
        }
    }
}
static void fxd_ag_packv(void *cx, int j0, int j1) {
    const FxdAg *j = cx;
    for (int idx = j0; idx < j1; idx++) {
        const int h = idx / j->nv, pv = idx % j->nv;
        const int w = FXD_HD - pv * j->NR < j->NR ? FXD_HD - pv * j->NR : j->NR;
        for (int t = 0; t < j->n; t++) {
            float *o = j->VP + ((size_t)h * j->nv + pv) * j->n * j->NR + (size_t)t * j->NR;
            memset(o, 0, (size_t)j->NR * 4);
            memcpy(o, j->V + (size_t)t * FXD_DIM + (size_t)h * FXD_HD + (size_t)pv * j->NR, (size_t)w * 4);
        }
    }
}
static void fxd_ag_job(void *cx, int j0, int j1) {
    const FxdAg *j = cx;
    float *sc = vt_alloc((size_t)FXD_AQ * j->n * 4);
    for (int idx = j0; idx < j1; idx++) {
        const int h = idx / j->nqb, b = idx % j->nqb, t0 = b * FXD_AQ, nq = j->n - t0 < FXD_AQ ? j->n - t0 : FXD_AQ;
        vt_gemm_packed(sc, (size_t)j->n, j->Q + (size_t)t0 * FXD_DIM + (size_t)h * FXD_HD, (size_t)FXD_DIM, j->KP + (size_t)h * j->np * FXD_HD * j->NR, nq, FXD_HD, j->n);
        for (int t = 0; t < nq; t++) vt_softmax(sc + (size_t)t * j->n, j->n);
        vt_gemm_packed(j->O + (size_t)t0 * FXD_DIM + (size_t)h * FXD_HD, (size_t)FXD_DIM, sc, (size_t)j->n, j->VP + (size_t)h * j->nv * j->n * j->NR, nq, j->n, FXD_HD);
    }
    vt_free(sc);
}
static void fxd_attention_gemm(float *O, const float *Q, const float *K, const float *V, int n) {
    const int NR = vt_nr(), np = (n + NR - 1) / NR, nv = (FXD_HD + NR - 1) / NR, nqb = (n + FXD_AQ - 1) / FXD_AQ;
    FxdAg j = { Q, K, V, O, vt_alloc((size_t)FXD_HEADS * np * NR * FXD_HD * 4), vt_alloc((size_t)FXD_HEADS * nv * NR * n * 4), n, NR, np, nv, nqb, 1.0f / sqrtf((float)FXD_HD) };
    pool_for(fxd_ag_packk, &j, FXD_HEADS * np, 4);
    pool_for(fxd_ag_packv, &j, FXD_HEADS * nv, 1);
    pool_for(fxd_ag_job, &j, FXD_HEADS * nqb, 1);
    vt_free(j.KP); vt_free(j.VP);
}
static void fxd_attention(float *O, const float *Q, const float *K, const float *V, int n) {
    const double t0 = FXD_TIC();
    static int simple = -1; if (simple < 0) { const char *e = getenv("KATANA_FLUX_ATT"); simple = e && !strcmp(e, "simple"); }
    if (simple) fxd_attention_simple(O, Q, K, V, n); else fxd_attention_gemm(O, Q, K, V, n);
    FXD_TOC(FT_ATT, t0);
}

/* SwiGLU: H[n][2m] -> Y[n][m] = silu(H[:, :m]) * H[:, m:], com passo de entrada ldh */
typedef struct { float *Y; const float *H; int m; size_t ldh; } FxdSw;
static void fxd_sw_job(void *cx, int r0, int r1) {
    const FxdSw *j = cx;
    for (int r = r0; r < r1; r++) { const float *h = j->H + (size_t)r * j->ldh; float *y = j->Y + (size_t)r * j->m; for (int i = 0; i < j->m; i++) y[i] = fxd_silu(h[i]) * h[j->m + i]; }
}
static void fxd_swiglu(float *Y, const float *H, int n, int m, size_t ldh) { FxdSw j = { Y, H, m, ldh }; pool_for(fxd_sw_job, &j, n, 4); }

typedef struct { float *X; const float *G, *Y; int d; } FxdAx;
static void fxd_ax_job(void *cx, int r0, int r1) {
    const FxdAx *j = cx;
    for (int r = r0; r < r1; r++) for (int i = 0; i < j->d; i++) j->X[(size_t)r * j->d + i] += j->G[i] * j->Y[(size_t)r * j->d + i];
}
static void fxd_axpy_gate(float *X, const float *G, const float *Y, int n, int d) {       /* X += gate * Y (gate por coluna) */
    FxdAx j = { X, G, Y, d }; pool_for(fxd_ax_job, &j, n, 8);
}

/* qkv de um produto fundido [n][ld] (q, k, v lado a lado, 3072 cada) para tres matrizes [n][3072], a partir da coluna c0 de cada linha */
typedef struct { const float *src; float *Q, *K, *V; size_t ld, c0; } FxdSplit;
static void fxd_split_job(void *cx, int r0, int r1) {
    const FxdSplit *j = cx;
    for (int r = r0; r < r1; r++) {
        const float *p = j->src + (size_t)r * j->ld + j->c0;
        memcpy(j->Q + (size_t)r * FXD_DIM, p, (size_t)FXD_DIM * 4); memcpy(j->K + (size_t)r * FXD_DIM, p + FXD_DIM, (size_t)FXD_DIM * 4); memcpy(j->V + (size_t)r * FXD_DIM, p + 2 * FXD_DIM, (size_t)FXD_DIM * 4);
    }
}
static void fxd_split_qkv(const float *src, size_t ld, int n, float *Q, float *K, float *V) { FxdSplit j = { src, Q, K, V, ld, 0 }; pool_for(fxd_split_job, &j, n, 8); }

/* SwiGLU com saida em passo proprio: Y[r*ldy + i] = silu(H[r*ldh + i]) * H[r*ldh + m + i] */
typedef struct { float *Y; const float *H; int m; size_t ldh, ldy; } FxdSw2;
static void fxd_sw2_job(void *cx, int r0, int r1) {
    const FxdSw2 *j = cx;
    for (int r = r0; r < r1; r++) { const float *h = j->H + (size_t)r * j->ldh; float *y = j->Y + (size_t)r * j->ldy; for (int i = 0; i < j->m; i++) y[i] = fxd_silu(h[i]) * h[j->m + i]; }
}
static void fxd_swiglu2(float *Y, size_t ldy, const float *H, size_t ldh, int n, int m) { FxdSw2 j = { Y, H, m, ldh, ldy }; pool_for(fxd_sw2_job, &j, n, 4); }


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
    fxd_split_qkv(qkv, (size_t)3 * D, Nt, Q, K, V);
    fxd_rms(Q, Nt, D, tq); fxd_rms(K, Nt, D, tk);
    fxd_ln_mod(xn, img, Ni, D, i_sh1, i_sc1); fxd_lin(F, qkv, xn, Ni, iqkv);
    fxd_split_qkv(qkv, (size_t)3 * D, Ni, Q + (size_t)Nt * D, K + (size_t)Nt * D, V + (size_t)Nt * D);
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
    fxd_split_qkv(h, (size_t)L1, N, Q, K, V);
    fxd_rms(Q, N, D, wq); fxd_rms(K, N, D, wk);
    fxd_rope(Q, N, cs, sn); fxd_rope(K, N, cs, sn);
    fxd_attention(A, Q, K, V, N);
    const int C2 = D + FXD_MLP;
    float *cat = xalloc((size_t)N * C2, 4), *o = xalloc((size_t)N * D, 4);
    for (int r = 0; r < N; r++) memcpy(cat + (size_t)r * C2, A + (size_t)r * D, (size_t)D * 4);
    fxd_swiglu2(cat + D, (size_t)C2, h + 3 * D, (size_t)L1, N, FXD_MLP);     /* parte do MLP: silu(primeira metade) * segunda, depois das D colunas da atencao */
    fxd_lin(F, o, cat, N, l2);
    fxd_axpy_gate(x, ms + 2 * D, o, N, D);
    free(xn); free(h); free(Q); free(K); free(V); free(A); free(cat); free(o); free(wq); free(wk);
    return 1;
}

static void fxd_probe_copy(float *dst, const float *src, size_t n) { if (dst) memcpy(dst, src, n * 4); }


/* ---- transformador na GPU: texto + imagem ficam na VRAM do primeiro bloco ao ultimo (fxvk.c). Devolve 1 com z [Nt+Ni][3072] preenchido,
 * 0 se a GPU nao pode (sem GPU, pesos que nao cabem na VRAM, falha): o chamador roda os blocos na CPU. Os tensores sao os de fxd_double / fxd_single. */
static int fxd_gpu_blocks(const FxDit *F, const float *x, const float *c, int Ni, int Nt, const float *mi, const float *mt, const float *ms, const float *cs, const float *sn, float *z, const FxdProbe *pr) {
    fxv_init();
    const int D = FXD_DIM, N = Nt + Ni;
    if (!fxv.on) return 0;
    static const char *dn[8] = { "double_blocks.%d.txt_attn.qkv.weight", "double_blocks.%d.img_attn.qkv.weight", "double_blocks.%d.txt_attn.proj.weight", "double_blocks.%d.img_attn.proj.weight",
                                 "double_blocks.%d.txt_mlp.0.weight", "double_blocks.%d.txt_mlp.2.weight", "double_blocks.%d.img_mlp.0.weight", "double_blocks.%d.img_mlp.2.weight" };
    const FxvW *dw[FXD_NDBL][8], *sw[FXD_NSGL][2];
    for (int b = 0; b < FXD_NDBL; b++) for (int k = 0; k < 8; k++) { const SdgInfo *t = fxd_t(F, dn[k], b); if (!t || !(dw[b][k] = fxv_weight(&F->g, t))) return 0; }
    for (int b = 0; b < FXD_NSGL; b++) {
        const SdgInfo *a = fxd_t(F, "single_blocks.%d.linear1.weight", b), *e = fxd_t(F, "single_blocks.%d.linear2.weight", b);
        if (!a || !e || !(sw[b][0] = fxv_weight(&F->g, a)) || !(sw[b][1] = fxv_weight(&F->g, e))) return 0;
    }
    if (!fxv_res_open(N)) return 0;
    const double t0 = sdt_now();
    /* ganhos das normas de q/k: bloco duplo b em nw[b*512 + {0: q txt, 128: k txt, 256: q img, 384: k img}], simples em nw[(5+b)*512 + {q, k}] */
    static int nw_done;
    if (!nw_done) {
        float *nw = xalloc((size_t)25 * 512, 4);
        for (int b = 0; b < FXD_NDBL; b++) {
            const char *nm[4] = { "double_blocks.%d.txt_attn.norm.query_norm.scale", "double_blocks.%d.txt_attn.norm.key_norm.scale", "double_blocks.%d.img_attn.norm.query_norm.scale", "double_blocks.%d.img_attn.norm.key_norm.scale" };
            for (int k = 0; k < 4; k++) { float *w = fxd_scale(F, nm[k], b); if (!w) return 0; memcpy(nw + (size_t)b * 512 + k * 128, w, 512); free(w); }
        }
        for (int b = 0; b < FXD_NSGL; b++) {
            float *q = fxd_scale(F, "single_blocks.%d.norm.query_norm.scale", b), *k = fxd_scale(F, "single_blocks.%d.norm.key_norm.scale", b);
            if (!q || !k) return 0;
            memcpy(nw + (size_t)(5 + b) * 512, q, 512); memcpy(nw + (size_t)(5 + b) * 512 + 128, k, 512); free(q); free(k);
        }
        if (!kvk_buf_write(fxr.nw, 0, nw, (size_t)25 * 512 * 4)) return 0;
        free(nw); nw_done = 1;
    }
    const size_t DD = (size_t)D, MI = 0, MT = 6 * DD, MS = 12 * DD;
    int ok = kvk_buf_write(fxr.z, 0, c, (size_t)Nt * D * 4) && kvk_buf_write(fxr.z, (size_t)Nt * D * 4, x, (size_t)Ni * D * 4)
          && kvk_buf_write(fxr.mod, MI * 4, mi, 6 * DD * 4) && kvk_buf_write(fxr.mod, MT * 4, mt, 6 * DD * 4) && kvk_buf_write(fxr.mod, MS * 4, ms, 3 * DD * 4)
          && kvk_buf_write(fxr.cs, 0, cs, (size_t)N * 128 * 4) && kvk_buf_write(fxr.sn, 0, sn, (size_t)N * 128 * 4);
    if (!ok) return 0;
    const size_t zi = (size_t)Nt * D;       /* inicio da imagem em z (elementos) */
    const int M2 = 2 * FXD_MLP;
    for (int b = 0; b < FXD_NDBL && ok; b++) {
        const FxvW **w = dw[b];                 /* 0 tqkv 1 iqkv 2 tprj 3 iprj 4 tm0 5 tm2 6 im0 7 im2 */
        ok &= fxv_ln(fxr.z, 0, 0, Nt, MT, MT + DD, fxr.xn);
        ok &= fxv_gemm(w[0], fxr.xn, 0, Nt, fxr.h, 0);
        ok &= fxv_ln(fxr.z, zi, zi, Ni, MI, MI + DD, fxr.xn);
        ok &= fxv_gemm(w[1], fxr.xn, zi, Ni, fxr.h, (size_t)Nt * 3 * D);
        ok &= fxv_prep(fxr.h, 3 * D, N, Nt, (size_t)b * 512);
        ok &= fxv_att(fxr.h, fxr.cat, N, 3 * D, D);
        ok &= fxv_gemm(w[3], fxr.cat, zi, Ni, fxr.o, 0);  ok &= fxv_axg(fxr.o, fxr.z, zi, Ni, MI + 2 * DD);
        ok &= fxv_gemm(w[2], fxr.cat, 0, Nt, fxr.o, 0);   ok &= fxv_axg(fxr.o, fxr.z, 0, Nt, MT + 2 * DD);
        ok &= fxv_ln(fxr.z, zi, zi, Ni, MI + 3 * DD, MI + 4 * DD, fxr.xn);
        ok &= fxv_gemm(w[6], fxr.xn, zi, Ni, fxr.h, 0);  ok &= fxv_swi(fxr.h, 0, M2, fxr.cat, 0, FXD_MLP, Ni, FXD_MLP);
        ok &= fxv_gemm(w[7], fxr.cat, 0, Ni, fxr.o, 0);  ok &= fxv_axg(fxr.o, fxr.z, zi, Ni, MI + 5 * DD);
        ok &= fxv_ln(fxr.z, 0, 0, Nt, MT + 3 * DD, MT + 4 * DD, fxr.xn);
        ok &= fxv_gemm(w[4], fxr.xn, 0, Nt, fxr.h, 0);   ok &= fxv_swi(fxr.h, 0, M2, fxr.cat, 0, FXD_MLP, Nt, FXD_MLP);
        ok &= fxv_gemm(w[5], fxr.cat, 0, Nt, fxr.o, 0);  ok &= fxv_axg(fxr.o, fxr.z, 0, Nt, MT + 5 * DD);
        if (pr && ok && pr->dimg[b]) {
            if (!kvk_buf_read(fxr.z, 0, z, (size_t)N * D * 4)) return 0;
            fxd_probe_copy(pr->dtxt[b], z, (size_t)Nt * D); fxd_probe_copy(pr->dimg[b], z + (size_t)Nt * D, (size_t)Ni * D);
        }
    }
    const int L1 = 3 * D + 2 * FXD_MLP, C2 = D + FXD_MLP;
    for (int b = 0; b < FXD_NSGL && ok; b++) {
        ok &= fxv_ln(fxr.z, 0, 0, N, MS, MS + DD, fxr.xn);
        ok &= fxv_gemm(sw[b][0], fxr.xn, 0, N, fxr.h, 0);
        ok &= fxv_prep(fxr.h, L1, N, N, (size_t)(5 + b) * 512);
        ok &= fxv_att(fxr.h, fxr.cat, N, L1, C2);
        ok &= fxv_swi(fxr.h, (size_t)3 * D, L1, fxr.cat, (size_t)D, C2, N, FXD_MLP);
        ok &= fxv_gemm(sw[b][1], fxr.cat, 0, N, fxr.o, 0);
        ok &= fxv_axg(fxr.o, fxr.z, 0, N, MS + 2 * DD);
        if (pr && ok && pr->sgl[b]) { if (!kvk_buf_read(fxr.z, 0, pr->sgl[b], (size_t)N * D * 4)) return 0; }
    }
    if (!ok || !kvk_buf_read(fxr.z, 0, z, (size_t)N * D * 4)) { fprintf(stderr, "flux: falha na GPU (%s), CPU\n", kvk_error()); fxv.on = 0; return 0; }
    fxr.t += sdt_now() - t0;
    return 1;
}

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
    /* 5. blocos duplos (a GPU faz os 25 blocos de uma vez, se puder) */
    float *z = xalloc((size_t)N * D, 4);
    const int on_gpu = fxd_gpu_blocks(F, x, c, Ni, Nt, mi, mt, ms, cs, sn, z, pr);
    for (int b = 0; b < FXD_NDBL && !on_gpu; b++) {
        if (!fxd_double(F, b, x, Ni, c, Nt, mi, mt, cs, sn)) return 0;
        if (pr) { fxd_probe_copy(pr->dimg[b], x, (size_t)Ni * D); fxd_probe_copy(pr->dtxt[b], c, (size_t)Nt * D); }
    }
    /* 6. blocos simples sobre [texto; imagem] */
    if (!on_gpu) { memcpy(z, c, (size_t)Nt * D * 4); memcpy(z + (size_t)Nt * D, x, (size_t)Ni * D * 4); }
    for (int b = 0; b < FXD_NSGL && !on_gpu; b++) {
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

/* bench: forward com entradas sinteticas, tamanho Ni (imagem) x Nt (texto); o primeiro forward e descartado (monta caches), o segundo e medido */
static int fxd_bench(int Ni, int Nt) {
    char gp[1100]; snprintf(gp, sizeof gp, "%s/flux2klein/gguf/flux-2-klein-4b-Q4_K_M.gguf", models_dir());
    FxDit F; if (!fxd_open(&F, gp)) return 1;
    float *img = xalloc((size_t)Ni * FXD_INCH, 4), *txt = xalloc((size_t)Nt * FXD_TXTCH, 4), *ii = xalloc((size_t)Ni * 4, 4), *it = xalloc((size_t)Nt * 4, 4), *out = xalloc((size_t)Ni * FXD_INCH, 4);
    uint64_t r = 12345; for (size_t i = 0; i < (size_t)Ni * FXD_INCH; i++) { r = r * 6364136223846793005ULL + 1442695040888963407ULL; img[i] = ((int)(r >> 40) % 2000 - 1000) / 1000.0f; }
    for (size_t i = 0; i < (size_t)Nt * FXD_TXTCH; i++) { r = r * 6364136223846793005ULL + 1442695040888963407ULL; txt[i] = ((int)(r >> 40) % 2000 - 1000) / 400.0f; }
    int W = 1; while (W * W < Ni) W++;
    for (int i = 0; i < Ni; i++) { ii[i * 4] = 0; ii[i * 4 + 1] = (float)(i / W); ii[i * 4 + 2] = (float)(i % W); ii[i * 4 + 3] = 0; }
    for (int i = 0; i < Nt; i++) { it[i * 4] = 0; it[i * 4 + 1] = 0; it[i * 4 + 2] = 0; it[i * 4 + 3] = (float)i; }
    g_fxd_prof = 0;
    double t0 = sdt_now(); if (!fxd_forward(&F, img, ii, Ni, txt, it, Nt, 0.5f, out, NULL)) return 1; const double first = sdt_now() - t0;
    g_fxd_prof = 1; memset(g_fxd_tm, 0, sizeof g_fxd_tm);
    t0 = sdt_now(); if (!fxd_forward(&F, img, ii, Ni, txt, it, Nt, 0.5f, out, NULL)) return 1; const double tot = sdt_now() - t0;
    double h = 0; for (size_t i = 0; i < (size_t)Ni * FXD_INCH; i++) h += (double)out[i] * (double)((i % 97) + 1);
    printf("bench Ni=%d Nt=%d  1o forward %.2f s  2o forward %.2f s  | desquantizar/empacotar %.2f  produtos %.2f  atencao %.2f  resto %.2f | soma %.6e\n", Ni, Nt, first, tot,
           g_fxd_tm[FT_PACK], g_fxd_tm[FT_GEMM], g_fxd_tm[FT_ATT], tot - g_fxd_tm[FT_PACK] - g_fxd_tm[FT_GEMM] - g_fxd_tm[FT_ATT], h);
    if (fxv.on) printf("  GPU: %ld produtos, pesos %.0f MB na VRAM (%d tensores na CPU por falta de espaco), envio %.2f s  produto %.2f s  leitura %.2f s (soma dos 2 forwards)\n  GPU residente: blocos %.2f s (soma dos 2 forwards, inclui envio e leitura de z)\n", fxv.ncalls, fxv.wbytes / 1048576.0, fxv.nfall, fxv.t_up, fxv.t_gemm, fxv.t_dl, fxr.t);
    return 0;
}

/* attn-test [n]: atencao por produtos contra a versao simples, Q K V aleatorios, n tokens */
static int fxd_attn_test(int n) {
    const size_t sz = (size_t)n * FXD_DIM;
    float *Q = xalloc(sz, 4), *K = xalloc(sz, 4), *V = xalloc(sz, 4), *O1 = xalloc(sz, 4), *O2 = xalloc(sz, 4);
    uint64_t r = 99; for (size_t i = 0; i < sz; i++) { r = r * 6364136223846793005ULL + 1442695040888963407ULL; Q[i] = ((int)(r >> 40) % 2000 - 1000) / 250.0f; r = r * 6364136223846793005ULL + 1442695040888963407ULL; K[i] = ((int)(r >> 40) % 2000 - 1000) / 250.0f; r = r * 6364136223846793005ULL + 1442695040888963407ULL; V[i] = ((int)(r >> 40) % 2000 - 1000) / 1000.0f; }
    double t0 = sdt_now(); fxd_attention_simple(O1, Q, K, V, n); const double ts = sdt_now() - t0;
    t0 = sdt_now(); fxd_attention_gemm(O2, Q, K, V, n); const double tg = sdt_now() - t0;
    int bad = fxd_cmp("atencao por produtos x simples", O2, O1, sz);
    printf("  n=%d tokens  razao de tempo simples/produtos %.1f  (produtos %.1f ms)\n", n, ts / tg, tg * 1e3);
    fxv_init();
    if (fxv.on && fxv.shared_ok) {       /* atencao na GPU contra a CPU: q, k, v lado a lado [n][9216] */
        float *qkv = xalloc((size_t)n * 3 * FXD_DIM, 4), *Og = xalloc(sz, 4);
        for (int i = 0; i < n; i++) { memcpy(qkv + (size_t)i * 3 * FXD_DIM, Q + (size_t)i * FXD_DIM, FXD_DIM * 4); memcpy(qkv + (size_t)i * 3 * FXD_DIM + FXD_DIM, K + (size_t)i * FXD_DIM, FXD_DIM * 4); memcpy(qkv + (size_t)i * 3 * FXD_DIM + 2 * FXD_DIM, V + (size_t)i * FXD_DIM, FXD_DIM * 4); }
        KvkBuf bq = kvk_buf_create(sz * 12, KVK_USAGE_RW), bo = kvk_buf_create(sz * 4, KVK_USAGE_RW);
        kvk_buf_write(bq, 0, qkv, sz * 12); fxv_att(bq, bo, n, 3 * FXD_DIM, FXD_DIM); kvk_wait();
        t0 = sdt_now(); for (int i = 0; i < 5; i++) fxv_att(bq, bo, n, 3 * FXD_DIM, FXD_DIM); kvk_wait(); const double tv = (sdt_now() - t0) / 5;
        kvk_buf_read(bo, 0, Og, sz * 4);
        bad += fxd_cmp("atencao GPU x CPU", Og, O1, sz);
        printf("  GPU %.1f ms por atencao (%.0f GFLOP/s)\n", tv * 1e3, 4.0 * n * n * FXD_DIM / tv / 1e9);
    }
    return bad;
}

/* gemm-test [n] [tensor]: um produto na GPU contra o da CPU (mesmo tensor, X aleatorio) */
static int fxd_gemm_test(int n, const char *name) {
    char gp[1100]; snprintf(gp, sizeof gp, "%s/flux2klein/gguf/flux-2-klein-4b-Q4_K_M.gguf", models_dir());
    FxDit F; if (!fxd_open(&F, gp)) return 1;
    const SdgInfo *t = sdg_find(&F.g, name); if (!t) { fprintf(stderr, "tensor %s nao existe\n", name); return 1; }
    const int cols = (int)t->dim[0], rows = (int)t->dim[1];
    float *X = xalloc((size_t)n * cols, 4), *Yc = xalloc((size_t)n * rows, 4), *Yg = xalloc((size_t)n * rows, 4);
    uint64_t r = 7; for (size_t i = 0; i < (size_t)n * cols; i++) { r = r * 6364136223846793005ULL + 1442695040888963407ULL; X[i] = ((int)(r >> 40) % 2000 - 1000) / 500.0f; }
    const char *sv = getenv("KATANA_FLUX_GPU"); setenv("KATANA_FLUX_GPU", "0", 1);
    double t0 = sdt_now(); fxd_lin(&F, Yc, X, n, t); fxd_lin(&F, Yc, X, n, t); const double tc = (sdt_now() - t0) / 2;
    if (sv) setenv("KATANA_FLUX_GPU", sv, 1); else unsetenv("KATANA_FLUX_GPU");
    fxv.tried = 0; fxv_init();
    if (!fxv.on) { fprintf(stderr, "sem GPU\n"); return 1; }
    fxv_lin(&F.g, Yg, X, n, t);      /* sobe o peso */
    t0 = sdt_now(); for (int i = 0; i < 5; i++) fxv_lin(&F.g, Yg, X, n, t); const double tg = (sdt_now() - t0) / 5;
    const int bad = fxd_cmp(name, Yg, Yc, (size_t)n * rows);
    printf("  n=%d  %dx%d  CPU %.1f ms  GPU %.1f ms (envio %.1f, produto %.1f, leitura %.1f)  %.1f GFLOP/s\n", n, rows, cols, tc * 1e3, tg * 1e3,
           fxv.t_up / 6e-3, fxv.t_gemm / 6e-3, fxv.t_dl / 6e-3, 2.0 * n * rows * cols / tg / 1e9);
    return bad;
}

static int fxd_main(int argc, char **argv) {
    if (argc >= 1 && !strcmp(argv[0], "gemm-test")) return fxd_gemm_test(argc > 1 ? atoi(argv[1]) : 800, argc > 2 ? argv[2] : "single_blocks.0.linear1.weight");
    if (argc >= 1 && !strcmp(argv[0], "attn-test")) return fxd_attn_test(argc > 1 ? atoi(argv[1]) : 2700);
    if (argc >= 1 && !strcmp(argv[0], "bench")) return fxd_bench(argc > 1 ? atoi(argv[1]) : 240, argc > 2 ? atoi(argv[2]) : 64);
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
