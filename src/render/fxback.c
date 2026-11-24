/* ============================ render: passada de volta do transformador do FLUX.2 klein (treino de LoRA) ===============================
 * Gradientes de cada peca do fxdit.c em float32 na CPU, com os pesos base quantizados e congelados: dos produtos com peso congelado so se
 * calcula o gradiente da entrada (dX = dY W, o peso e desquantizado por linhas direto para paineis transpostos, sem copia em cache); dos
 * adaptadores calculam-se tambem gA e gB. As modulacoes (shift, scale, gate) vem so do passo de tempo e sao constantes: os blocos nao
 * devolvem gradiente delas (a LayerNorm modulada sabe calcula-los, usado nos testes).
 *
 * Atencao sem materializar a matriz n x n: a volta recalcula as pontuacoes em blocos (estilo "recomputar"), em duas passadas paralelas sem
 * escrita concorrente: (A) por bloco de consultas: P = exp(S - L), dP = dO V^T, dS = P (dP - D), dQ = dS K; (B) por bloco de chaves, com o
 * mesmo L: dV = P^T dO, dK = dS^T Q. L (logsumexp por linha) sai da passada A; D = soma(dO * O) por cabeca e consulta.
 *
 * Cada bloco da volta recebe as entradas do bloco (o ponto de verificacao guardado na ida), refaz a ida guardando o que precisa
 * (fxr_dbl_fwd / fxr_sgl_fwd), calcula a volta e libera: a memoria de ativacoes e a de um bloco mais as entradas dos 25 blocos. */

static int g_fxr_train;          /* 1: a volta acumula gA e gB dos adaptadores (gradientes alocados por fxr_alloc_grads) */

static void fxr_alloc_grads(void) {
    FxLora *L = &g_fxl_store;
    for (int i = 0; i < L->n; i++) {
        FxlMod *m = &L->m[i]; const size_t na = (size_t)m->r * m->cols, nb = (size_t)m->rows * m->r;
        if (!m->gA) { m->gA = xalloc(na, 4); m->gB = xalloc(nb, 4); m->mA1 = xalloc(na, 4); m->mA2 = xalloc(na, 4); m->mB1 = xalloc(nb, 4); m->mB2 = xalloc(nb, 4); }
        memset(m->gA, 0, na * 4); memset(m->gB, 0, nb * 4);
    }
}
static void fxr_zero_grads(void) {
    FxLora *L = &g_fxl_store;
    for (int i = 0; i < L->n; i++) { FxlMod *m = &L->m[i]; if (m->gA) { memset(m->gA, 0, (size_t)m->r * m->cols * 4); memset(m->gB, 0, (size_t)m->rows * m->r * 4); } }
}

/* ---- LayerNorm modulada: Y = LN(X) (1 + scale) + shift ---- */
typedef struct { float *dX; const float *dY, *X, *scale; int d; } FxrLnb;
static void fxr_lnb_job(void *cx, int r0, int r1) {
    const FxrLnb *j = cx; const int d = j->d;
    for (int r = r0; r < r1; r++) {
        const float *x = j->X + (size_t)r * d, *dy = j->dY + (size_t)r * d; float *dx = j->dX + (size_t)r * d;
        double s = 0, s2 = 0; for (int i = 0; i < d; i++) { s += x[i]; s2 += (double)x[i] * x[i]; }
        const double mean = s / d, var = s2 / d - mean * mean, inv = 1.0 / sqrt((var > 0 ? var : 0) + 1e-6);
        double a = 0, b = 0;
        for (int i = 0; i < d; i++) { const double g = (double)dy[i] * (1.0 + j->scale[i]), xh = (x[i] - mean) * inv; a += g; b += g * xh; }
        a /= d; b /= d;
        for (int i = 0; i < d; i++) { const double g = (double)dy[i] * (1.0 + j->scale[i]), xh = (x[i] - mean) * inv; dx[i] += (float)(inv * (g - a - xh * b)); }
    }
}
/* dX += dLN; dshift e dscale (opcionais) += somas por coluna */
static void fxr_ln_mod_bwd(float *dX, float *dshift, float *dscale, const float *dY, const float *X, int n, int d, const float *scale) {
    FxrLnb j = { dX, dY, X, scale, d };
    pool_for(fxr_lnb_job, &j, n, 8);
    if (dshift || dscale)
        for (int r = 0; r < n; r++) {
            const float *x = X + (size_t)r * d, *dy = dY + (size_t)r * d;
            double s = 0, s2 = 0; for (int i = 0; i < d; i++) { s += x[i]; s2 += (double)x[i] * x[i]; }
            const double mean = s / d, var = s2 / d - mean * mean, inv = 1.0 / sqrt((var > 0 ? var : 0) + 1e-6);
            for (int i = 0; i < d; i++) { if (dshift) dshift[i] += dy[i]; if (dscale) dscale[i] += (float)(dy[i] * (x[i] - mean) * inv); }
        }
}

/* ---- RMSNorm por cabeca: X0 = entrada da norma, dY = gradiente da saida; dX no lugar de dY ---- */
typedef struct { float *dY; const float *X0, *w; int stride; } FxrRb;
static void fxr_rmsb_job(void *cx, int i0, int i1) {
    const FxrRb *j = cx;
    for (int i = i0; i < i1; i++) {
        const size_t off = (size_t)(i / FXD_HEADS) * j->stride + (size_t)(i % FXD_HEADS) * FXD_HD;
        const float *x = j->X0 + off; float *dy = j->dY + off;
        double s = 0; for (int k = 0; k < FXD_HD; k++) s += (double)x[k] * x[k];
        const double inv = 1.0 / sqrt(s / FXD_HD + 1e-6);
        double dot = 0; for (int k = 0; k < FXD_HD; k++) dot += (double)dy[k] * j->w[k] * x[k];
        const double c = inv * inv * inv * dot / FXD_HD;
        for (int k = 0; k < FXD_HD; k++) dy[k] = (float)(inv * dy[k] * j->w[k] - x[k] * c);
    }
}
static void fxr_rms_bwd(float *dY, const float *X0, int n, int stride, const float *w) { FxrRb j = { dY, X0, w, stride }; pool_for(fxr_rmsb_job, &j, n * FXD_HEADS, 16); }

/* ---- RoPE: a volta e a rotacao inversa (no lugar) ---- */
typedef struct { float *X; const float *cs, *sn; } FxrRp;
static void fxr_rope_job(void *cx, int i0, int i1) {
    const FxrRp *j = cx;
    for (int i = i0; i < i1; i++) {
        const int tok = i / FXD_HEADS, h = i % FXD_HEADS;
        float *x = j->X + (size_t)tok * FXD_DIM + (size_t)h * FXD_HD;
        const float *c = j->cs + (size_t)tok * 128, *s = j->sn + (size_t)tok * 128;
        for (int k = 0; k < FXD_HD; k += 2) {
            const float a = x[k], b = x[k + 1];
            x[k] = a * c[k] + b * s[k + 1];
            x[k + 1] = -a * s[k] + b * c[k + 1];
        }
    }
}
static void fxr_rope_bwd(float *dX, int n, const float *cs, const float *sn) { FxrRp j = { dX, cs, sn }; pool_for(fxr_rope_job, &j, n * FXD_HEADS, 16); }

/* ---- SwiGLU: y = silu(a) b com a = H[:, :m], b = H[:, m:]; dH recebe (da, db) com passo proprio ---- */
typedef struct { float *dH; const float *dY, *H; int m; size_t lddh, lddy, ldh; } FxrSwb;
static void fxr_swb_job(void *cx, int r0, int r1) {
    const FxrSwb *j = cx;
    for (int r = r0; r < r1; r++) {
        const float *h = j->H + (size_t)r * j->ldh, *dy = j->dY + (size_t)r * j->lddy; float *dh = j->dH + (size_t)r * j->lddh;
        for (int i = 0; i < j->m; i++) {
            const float a = h[i], b = h[j->m + i], sg = 1.0f / (1.0f + expf(-a)), sa = a * sg;
            dh[i] = dy[i] * b * (sg * (1.0f + a * (1.0f - sg)));
            dh[j->m + i] = dy[i] * sa;
        }
    }
}
static void fxr_swiglu_bwd(float *dH, size_t lddh, const float *dY, size_t lddy, const float *H, size_t ldh, int n, int m) {
    FxrSwb j = { dH, dY, H, m, lddh, lddy, ldh }; pool_for(fxr_swb_job, &j, n, 4);
}

/* Y[r] = G * X[r] (ganho por coluna) */
typedef struct { float *Y; const float *G, *X; int d; } FxrGl;
static void fxr_gl_job(void *cx, int r0, int r1) { const FxrGl *j = cx; for (int r = r0; r < r1; r++) for (int i = 0; i < j->d; i++) j->Y[(size_t)r * j->d + i] = j->G[i] * j->X[(size_t)r * j->d + i]; }
static void fxr_gate_mul(float *Y, const float *G, const float *X, int n, int d) { FxrGl j = { Y, G, X, d }; pool_for(fxr_gl_job, &j, n, 8); }

typedef struct { float *Y; const float *X; size_t n; } FxrAd;
static void fxr_ad_job(void *cx, int a, int b) { const FxrAd *j = cx; const size_t c = (size_t)(j->n + 63) / 64; const size_t s = (size_t)a * c, e = (size_t)b * c < j->n ? (size_t)b * c : j->n; for (size_t i = s; i < e; i++) j->Y[i] += j->X[i]; }
static void fxr_add(float *Y, const float *X, size_t n) { FxrAd j = { Y, X, n }; pool_for(fxr_ad_job, &j, 64, 1); }

/* ---- produto com peso congelado: dX[n][cols] = dY[n][rows] W[rows][cols] ---- */
typedef struct { const SdgInfo *t; const uint8_t *data; float *P; int rows, cols, NR; } FxrTj;
static void fxr_tj_job(void *cx, int o0, int o1) {
    const FxrTj *j = cx; const int np = (j->cols + j->NR - 1) / j->NR;
    float *row = vt_alloc((size_t)j->cols * 4);
    for (int o = o0; o < o1; o++) {
        fxd_row(j->t, j->data, o, j->cols, row);
        for (int p = 0; p < np; p++) {
            float *dst = j->P + ((size_t)p * j->rows + o) * j->NR; const int w = j->cols - p * j->NR < j->NR ? j->cols - p * j->NR : j->NR;
            memcpy(dst, row + (size_t)p * j->NR, (size_t)w * 4);
            for (int r = w; r < j->NR; r++) dst[r] = 0.0f;
        }
    }
    vt_free(row);
}
static void fxr_dx(const FxDit *F, float *dX, const float *dY, int n, const SdgInfo *t) {
    const int cols = (int)t->dim[0], rows = (int)t->dim[1], NR = vt_nr(), np = (cols + NR - 1) / NR;
    float *P = vt_alloc((size_t)np * NR * rows * 4);
    FxrTj j = { t, sdg_data(&F->g, t), P, rows, cols, NR };
    pool_for(fxr_tj_job, &j, rows, 16);
    fxb_run(dX, (size_t)cols, dY, (size_t)rows, P, n, rows, cols);
    vt_free(P);
}

/* ---- adaptador: volta de Y += scale s (X A^T) B^T. dX += ...; gA, gB += ... (se g_fxr_train) ---- */
static void fxr_lora_bwd(float *dX, const float *dY, const float *X, int n, const FxlMod *m, float scale) {
    const int r = m->r, rows = m->rows, cols = m->cols; const float se = scale * m->s;
    float *U = vt_alloc((size_t)n * r * 4), *dU = vt_alloc((size_t)n * r * 4);
    fxb_mm(U, (size_t)r, X, (size_t)cols, n, cols, r, m->A, cols, 1);             /* U = X A^T */
    fxb_mm(dU, (size_t)r, dY, (size_t)rows, n, rows, r, m->B, r, 0);               /* dU = dY B */
    for (size_t i = 0; i < (size_t)n * r; i++) dU[i] *= se;
    float *T = vt_alloc((size_t)n * cols * 4);
    fxb_mm(T, (size_t)cols, dU, (size_t)r, n, r, cols, m->A, cols, 0);             /* dX += dU A */
    fxr_add(dX, T, (size_t)n * cols);
    vt_free(T);
    if (g_fxr_train && m->gA) {
        float *UT = vt_alloc((size_t)r * n * 4), *dUT = vt_alloc((size_t)r * n * 4);
        for (int i = 0; i < n; i++) for (int k = 0; k < r; k++) { UT[(size_t)k * n + i] = U[(size_t)i * r + k]; dUT[(size_t)k * n + i] = dU[(size_t)i * r + k]; }
        float *G = vt_alloc((size_t)r * (rows > cols ? rows : cols) * 4);
        fxb_mm(G, (size_t)rows, UT, (size_t)n, r, n, rows, dY, rows, 0);           /* G[k][o] = sum_i U[i][k] dY[i][o] */
        for (int o = 0; o < rows; o++) for (int k = 0; k < r; k++) m->gB[(size_t)o * r + k] += se * G[(size_t)k * rows + o];
        fxb_mm(G, (size_t)cols, dUT, (size_t)n, r, n, cols, X, cols, 0);           /* gA[k][c] = sum_i dU[i][k] X[i][c] */
        for (size_t i = 0; i < (size_t)r * cols; i++) m->gA[i] += G[i];
        vt_free(UT); vt_free(dUT); vt_free(G);
    }
    vt_free(U); vt_free(dU);
}
/* dX = dY W (+ caminho do adaptador); X e a entrada que o produto teve na ida */
static void fxr_lin_bwd(const FxDit *F, float *dX, const float *dY, const float *X, int n, const SdgInfo *t) {
    fxr_dx(F, dX, dY, n, t);
    if (g_fxl_on) { const FxlMod *m = fxl_find(t); if (m) fxr_lora_bwd(dX, dY, X, n, m, g_fxl_store.scale); }
}

/* ---- atencao: volta ---- */
#define FXR_AQ 48
typedef struct { const float *X; float *P; int n, NR, np, nv, kt; } FxrPk;
static void fxr_pk_job(void *cx, int a, int b) {
    const FxrPk *j = cx;
    for (int idx = a; idx < b; idx++) {
        if (j->kt) {                                   /* K-dim = d (128), colunas = tokens: P[h][p][d][NR] */
            const int h = idx / j->np, p = idx % j->np;
            for (int r = 0; r < j->NR; r++) {
                const int t = p * j->NR + r; const float *x = t < j->n ? j->X + (size_t)t * FXD_DIM + (size_t)h * FXD_HD : NULL;
                float *o = j->P + ((size_t)h * j->np + p) * FXD_HD * j->NR + r;
                for (int d = 0; d < FXD_HD; d++) o[(size_t)d * j->NR] = x ? x[d] : 0.0f;
            }
        } else {                                       /* K-dim = tokens, colunas = d: P[h][pv][t][NR] */
            const int h = idx / j->nv, pv = idx % j->nv;
            for (int t = 0; t < j->n; t++) {
                float *o = j->P + ((size_t)h * j->nv + pv) * j->n * j->NR + (size_t)t * j->NR;
                const int w = FXD_HD - pv * j->NR < j->NR ? FXD_HD - pv * j->NR : j->NR;
                memset(o, 0, (size_t)j->NR * 4);
                memcpy(o, j->X + (size_t)t * FXD_DIM + (size_t)h * FXD_HD + (size_t)pv * j->NR, (size_t)w * 4);
            }
        }
    }
}
static float *fxr_pk(const float *X, int n, int kt) {
    const int NR = vt_nr(), np = (n + NR - 1) / NR, nv = (FXD_HD + NR - 1) / NR;
    float *P = vt_alloc((size_t)FXD_HEADS * (kt ? (size_t)np * NR * FXD_HD : (size_t)nv * NR * n) * 4);
    FxrPk j = { X, P, n, NR, np, nv, kt };
    pool_for(fxr_pk_job, &j, FXD_HEADS * (kt ? np : nv), 1);
    return P;
}
typedef struct {
    const float *Q, *K, *V, *dO; float *dQ, *dK, *dV, *L, *Dv;
    const float *Kkt, *Vkt, *Kvt, *Qkt, *Qvt, *dOkt, *dOvt;
    int n, NR, np, nv, nqb; float scale;
} FxrAb;
static void fxr_ab_d(void *cx, int a, int b) {                 /* D[h][q] = soma_d dO O */
    const FxrAb *j = cx;
    for (int idx = a; idx < b; idx++) {
        const int h = idx / j->n, q = idx % j->n;
        const float *o = j->dQ /* O guardado em dQ na entrada: ver fxr_attention_bwd */ + (size_t)q * FXD_DIM + (size_t)h * FXD_HD, *d = j->dO + (size_t)q * FXD_DIM + (size_t)h * FXD_HD;
        double s = 0; for (int k = 0; k < FXD_HD; k++) s += (double)o[k] * d[k];
        j->Dv[(size_t)h * j->n + q] = (float)s;
    }
}
static void fxr_ab_a(void *cx, int a, int b) {
    const FxrAb *j = cx; const int n = j->n;
    float *S = vt_alloc((size_t)FXR_AQ * n * 4), *dP = vt_alloc((size_t)FXR_AQ * n * 4);
    for (int idx = a; idx < b; idx++) {
        const int h = idx / j->nqb, bq = idx % j->nqb, t0 = bq * FXR_AQ, nq = n - t0 < FXR_AQ ? n - t0 : FXR_AQ;
        const size_t ho = (size_t)h * FXD_HD;
        vt_gemm_packed(S, (size_t)n, j->Q + (size_t)t0 * FXD_DIM + ho, (size_t)FXD_DIM, j->Kkt + (size_t)h * j->np * FXD_HD * j->NR, nq, FXD_HD, n);
        vt_gemm_packed(dP, (size_t)n, j->dO + (size_t)t0 * FXD_DIM + ho, (size_t)FXD_DIM, j->Vkt + (size_t)h * j->np * FXD_HD * j->NR, nq, FXD_HD, n);
        for (int q = 0; q < nq; q++) {
            float *s = S + (size_t)q * n, *dp = dP + (size_t)q * n; float mx = -1e30f;
            for (int t = 0; t < n; t++) { s[t] *= j->scale; if (s[t] > mx) mx = s[t]; }
            double den = 0; for (int t = 0; t < n; t++) den += expf(s[t] - mx);
            const float L = mx + (float)log(den), Dq = j->Dv[(size_t)h * n + t0 + q];
            j->L[(size_t)h * n + t0 + q] = L;
            for (int t = 0; t < n; t++) { const float p = expf(s[t] - L); dp[t] = p * (dp[t] - Dq); }     /* dp = dS */
        }
        vt_gemm_packed(j->dQ + (size_t)t0 * FXD_DIM + ho, (size_t)FXD_DIM, dP, (size_t)n, j->Kvt + (size_t)h * j->nv * n * j->NR, nq, n, FXD_HD);
        for (int q = 0; q < nq; q++) { float *o = j->dQ + (size_t)(t0 + q) * FXD_DIM + ho; for (int d = 0; d < FXD_HD; d++) o[d] *= j->scale; }
    }
    vt_free(S); vt_free(dP);
}
static void fxr_ab_b(void *cx, int a, int b) {
    const FxrAb *j = cx; const int n = j->n;
    float *S = vt_alloc((size_t)FXR_AQ * n * 4), *dP = vt_alloc((size_t)FXR_AQ * n * 4);
    for (int idx = a; idx < b; idx++) {
        const int h = idx / j->nqb, bk = idx % j->nqb, t0 = bk * FXR_AQ, nk = n - t0 < FXR_AQ ? n - t0 : FXR_AQ;
        const size_t ho = (size_t)h * FXD_HD;
        vt_gemm_packed(S, (size_t)n, j->K + (size_t)t0 * FXD_DIM + ho, (size_t)FXD_DIM, j->Qkt + (size_t)h * j->np * FXD_HD * j->NR, nk, FXD_HD, n);
        vt_gemm_packed(dP, (size_t)n, j->V + (size_t)t0 * FXD_DIM + ho, (size_t)FXD_DIM, j->dOkt + (size_t)h * j->np * FXD_HD * j->NR, nk, FXD_HD, n);
        for (int k = 0; k < nk; k++) {
            float *s = S + (size_t)k * n, *dp = dP + (size_t)k * n;
            for (int q = 0; q < n; q++) { const float p = expf(s[q] * j->scale - j->L[(size_t)h * n + q]); s[q] = p; dp[q] = p * (dp[q] - j->Dv[(size_t)h * n + q]); }
        }
        vt_gemm_packed(j->dV + (size_t)t0 * FXD_DIM + ho, (size_t)FXD_DIM, S, (size_t)n, j->dOvt + (size_t)h * j->nv * n * j->NR, nk, n, FXD_HD);
        vt_gemm_packed(j->dK + (size_t)t0 * FXD_DIM + ho, (size_t)FXD_DIM, dP, (size_t)n, j->Qvt + (size_t)h * j->nv * n * j->NR, nk, n, FXD_HD);
        for (int k = 0; k < nk; k++) { float *o = j->dK + (size_t)(t0 + k) * FXD_DIM + ho; for (int d = 0; d < FXD_HD; d++) o[d] *= j->scale; }
    }
    vt_free(S); vt_free(dP);
}
/* O e a saida da ida; dO o gradiente; devolve dQ, dK, dV [n][3072] (memoria do chamador). dQ serve de area de O durante a primeira passada */
static void fxr_attention_bwd(float *dQ, float *dK, float *dV, const float *Q, const float *K, const float *V, const float *O, const float *dO, int n) {
    const int NR = vt_nr(), np = (n + NR - 1) / NR, nv = (FXD_HD + NR - 1) / NR, nqb = (n + FXR_AQ - 1) / FXR_AQ;
    float *L = xalloc((size_t)FXD_HEADS * n, 4), *Dv = xalloc((size_t)FXD_HEADS * n, 4);
    float *Kkt = fxr_pk(K, n, 1), *Vkt = fxr_pk(V, n, 1), *Kvt = fxr_pk(K, n, 0), *Qkt = fxr_pk(Q, n, 1), *Qvt = fxr_pk(Q, n, 0), *dOkt = fxr_pk(dO, n, 1), *dOvt = fxr_pk(dO, n, 0);
    FxrAb j = { Q, K, V, dO, dQ, dK, dV, L, Dv, Kkt, Vkt, Kvt, Qkt, Qvt, dOkt, dOvt, n, NR, np, nv, nqb, 1.0f / sqrtf((float)FXD_HD) };
    memcpy(dQ, O, (size_t)n * FXD_DIM * 4);                    /* a passada D le O de dQ; depois a passada A sobrescreve */
    pool_for(fxr_ab_d, &j, FXD_HEADS * n, 64);
    pool_for(fxr_ab_a, &j, FXD_HEADS * nqb, 1);
    pool_for(fxr_ab_b, &j, FXD_HEADS * nqb, 1);
    free(L); free(Dv);
    vt_free(Kkt); vt_free(Vkt); vt_free(Kvt); vt_free(Qkt); vt_free(Qvt); vt_free(dOkt); vt_free(dOvt);
}
