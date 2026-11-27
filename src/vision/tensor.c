/* ---------- visao: algebra em float32 na CPU (produto de matrizes, camada linear, norma, GELU, atencao) ----------
 * Pecas sem estado e sem nada de imagem dentro: servem ao codificador de imagem (vit.c) e a qualquer outro modulo com
 * tensores densos em float (convolucoes e atencao de um gerador de imagens, por exemplo). Peso F16 ou F32 no arquivo
 * mapeado; o produto abre o peso em paineis de NR linhas (uma vez por chamada) e roda em blocos com AVX-512 ou AVX2. */
enum { VT_F32 = 0, VT_F16 = 1 };
typedef struct { const void *p; int type, rows, cols; } VtMat;      /* rows saidas x cols entradas, linha a linha */

static int vt_wide(void) {
    static int v = -1;
    if (v < 0) {
        v = qn_512();
        if (getenv("KATANA_VISION_AVX2")) v = 0;      /* conferencia: so AVX2 */
    }
    return v;
}
static int vt_nr(void) { return vt_wide() ? 32 : 16; }
static void *vt_alloc(size_t bytes) {
    void *p = NULL;
#ifdef _WIN32
    p = _aligned_malloc(bytes + 64, 64);
#else
    if (posix_memalign(&p, 64, bytes + 64)) p = NULL;
#endif
    if (!p) die("visao: sem memoria");
    return p;
}
static void vt_free(void *p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

/* ---- nucleo: C[mr][NR] (+)= A[mr][k] * painel[k][NR] ---- */
#define VT_INL static inline __attribute__((always_inline))
#define VT_512 __attribute__((target("avx512f,avx512bw,avx512vl,avx512dq,fma")))
#ifdef __AVX2__
VT_512 VT_INL void vt_k512(const int mr, const float *a, size_t lda, const float *b, int k, float *c, size_t ldc, int add) {
    __m512 x[12], y[12];
#pragma GCC unroll 12
    for (int i = 0; i < mr; i++) { x[i] = _mm512_setzero_ps(); y[i] = _mm512_setzero_ps(); }
    for (int p = 0; p < k; p++, b += 32) {
        const __m512 b0 = _mm512_load_ps(b), b1 = _mm512_load_ps(b + 16);
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
VT_512 static void vt_kern512(int mr, const float *a, size_t lda, const float *b, int k, float *c, size_t ldc, int add) {
    switch (mr) {
#define K(n) case n: vt_k512(n, a, lda, b, k, c, ldc, add); break;
    K(1) K(2) K(3) K(4) K(5) K(6) K(7) K(8) K(9) K(10) K(11) K(12)
#undef K
    }
}
VT_INL void vt_k256(const int mr, const float *a, size_t lda, const float *b, int k, float *c, size_t ldc, int add) {
    __m256 x[6], y[6];
#pragma GCC unroll 6
    for (int i = 0; i < mr; i++) { x[i] = _mm256_setzero_ps(); y[i] = _mm256_setzero_ps(); }
    for (int p = 0; p < k; p++, b += 16) {
        const __m256 b0 = _mm256_load_ps(b), b1 = _mm256_load_ps(b + 8);
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
static void vt_kern256(int mr, const float *a, size_t lda, const float *b, int k, float *c, size_t ldc, int add) {
    switch (mr) {
#define K(n) case n: vt_k256(n, a, lda, b, k, c, ldc, add); break;
    K(1) K(2) K(3) K(4) K(5) K(6)
#undef K
    }
}
#else
/* porte ARM, 2026-11-27: sem SIMD. Mesma conta de vt_k512/vt_kern512 (painel de 32 colunas), um escalar por vez;
   nao precisa do helper vt_k512 (so existia para desenrolar os registros do AVX-512). Mais lento, nunca testado
   num celular; serve so para compilar e conferir a saida contra a versao AVX2/AVX-512 (katana selftest) antes de
   ganhar uma versao NEON. */
static void vt_kern512(int mr, const float *a, size_t lda, const float *b, int k, float *c, size_t ldc, int add) {
    for (int i = 0; i < mr; i++) {
        float *o = c + (size_t)i * ldc;
        for (int j = 0; j < 32; j++) {
            float s = add ? o[j] : 0.0f;
            for (int p = 0; p < k; p++) s += a[(size_t)i * lda + p] * b[(size_t)p * 32 + j];
            o[j] = s;
        }
    }
}
/* porte ARM, 2026-11-27: sem SIMD. Mesma conta de vt_k256/vt_kern256 (painel de 16 colunas). */
static void vt_kern256(int mr, const float *a, size_t lda, const float *b, int k, float *c, size_t ldc, int add) {
    for (int i = 0; i < mr; i++) {
        float *o = c + (size_t)i * ldc;
        for (int j = 0; j < 16; j++) {
            float s = add ? o[j] : 0.0f;
            for (int p = 0; p < k; p++) s += a[(size_t)i * lda + p] * b[(size_t)p * 16 + j];
            o[j] = s;
        }
    }
}
#endif
/* C[n][m] = A[n][k] * B, com B ja em paineis de NR colunas (B[painel][k][NR], completado com zeros) */
static void vt_gemm_packed(float *C, size_t ldc, const float *A, size_t lda, const float *B, int n, int k, int m) {
    const int wide = vt_wide(), NR = wide ? 32 : 16, MR = wide ? 12 : 6, KC = 512;
    for (int p0 = 0; p0 < k; p0 += KC) {
        const int kc = k - p0 < KC ? k - p0 : KC, add = p0 > 0;
        for (int j = 0; j < m; j += NR) {
            const float *b = B + (size_t)(j / NR) * k * NR + (size_t)p0 * NR;
            const int w = m - j < NR ? m - j : NR;
            for (int i = 0; i < n; i += MR) {
                const int mr = n - i < MR ? n - i : MR;
                const float *a = A + (size_t)i * lda + p0;
                float *c = C + (size_t)i * ldc + j;
                if (w == NR) {
                    if (wide) vt_kern512(mr, a, lda, b, kc, c, ldc, add); else vt_kern256(mr, a, lda, b, kc, c, ldc, add);
                    continue;
                }
                float t[12 * 32];      /* ultimo painel incompleto: sai por um rascunho */
                if (add) for (int r = 0; r < mr; r++) { memset(t + r * NR, 0, (size_t)NR * 4); memcpy(t + r * NR, c + (size_t)r * ldc, (size_t)w * 4); }
                if (wide) vt_kern512(mr, a, lda, b, kc, t, (size_t)NR, add); else vt_kern256(mr, a, lda, b, kc, t, (size_t)NR, add);
                for (int r = 0; r < mr; r++) memcpy(c + (size_t)r * ldc, t + r * NR, (size_t)w * 4);
            }
        }
    }
}
/* linha de um peso em float */
static void vt_row(const VtMat *W, int r, float *out) {
    const int c = W->cols;
    if (W->type == VT_F32) { memcpy(out, (const char *)W->p + (size_t)r * c * 4, (size_t)c * 4); return; }
    const uint16_t *h = (const uint16_t *)W->p + (size_t)r * c;
    int i = 0;
#ifdef __AVX2__
    for (; i + 8 <= c; i += 8) _mm256_storeu_ps(out + i, _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(h + i))));
    for (; i < c; i++) out[i] = _cvtsh_ss(h[i]);
#else
    /* porte ARM, 2026-11-27: sem SIMD; f16_to_f32 ja existe em src/engine/model.c (mesma conta, sem F16C) */
    for (; i < c; i++) out[i] = f16_to_f32(h[i]);
#endif
}
/* paineis de um peso: NR linhas de W viram um painel [cols][NR] */
typedef struct { const VtMat *W; float *B; int NR; } VtPackJob;
static void vt_pack_job(void *cx, int j0, int j1) {
    const VtPackJob *j = cx;
    const int c = j->W->cols, NR = j->NR;
    float *row = vt_alloc((size_t)c * 4);
    for (int p = j0; p < j1; p++) {
        float *o = j->B + (size_t)p * c * NR;
        for (int r = 0; r < NR; r++) {
            const int rr = p * NR + r;
            if (rr >= j->W->rows) { for (int i = 0; i < c; i++) o[(size_t)i * NR + r] = 0.0f; continue; }
            vt_row(j->W, rr, row);
            for (int i = 0; i < c; i++) o[(size_t)i * NR + r] = row[i];
        }
    }
    vt_free(row);
}
static float *vt_pack(const VtMat *W) {
    const int NR = vt_nr(), np = (W->rows + NR - 1) / NR;
    float *B = vt_alloc((size_t)np * NR * W->cols * 4);
    VtPackJob j = { W, B, NR };
    pool_for(vt_pack_job, &j, np, 1);
    return B;
}
typedef struct { float *Y; const float *X, *B, *bias; int n, k, m; size_t ldy, ldx; } VtLinJob;
#define VT_ROWS 48
static void vt_lin_job(void *cx, int j0, int j1) {
    const VtLinJob *j = cx;
    for (int b = j0; b < j1; b++) {
        const int r0 = b * VT_ROWS, n = j->n - r0 < VT_ROWS ? j->n - r0 : VT_ROWS;
        float *y = j->Y + (size_t)r0 * j->ldy;
        vt_gemm_packed(y, j->ldy, j->X + (size_t)r0 * j->ldx, j->ldx, j->B, n, j->k, j->m);
        if (j->bias) for (int r = 0; r < n; r++) for (int i = 0; i < j->m; i++) y[(size_t)r * j->ldy + i] += j->bias[i];
    }
}
/* camada linear: Y[n][rows] = X[n][cols] * W^T + bias */
static void vt_linear(float *Y, const float *X, int n, const VtMat *W, const float *bias) {
    float *B = vt_pack(W);
    VtLinJob j = { Y, X, B, bias, n, W->cols, W->rows, (size_t)W->rows, (size_t)W->cols };
    pool_for(vt_lin_job, &j, (n + VT_ROWS - 1) / VT_ROWS, 1);
    vt_free(B);
}

/* ---- norma de camada (media e variancia da linha, ganho e deslocamento treinados) ---- */
typedef struct { float *Y; const float *X, *g, *b; int d; float eps; } VtNormJob;
static void vt_norm_job(void *cx, int r0, int r1) {
    const VtNormJob *j = cx;
    for (int r = r0; r < r1; r++) {
        const float *x = j->X + (size_t)r * j->d; float *y = j->Y + (size_t)r * j->d;
        float s = 0.0f, v = 0.0f;
        for (int i = 0; i < j->d; i++) s += x[i];
        const float mu = s / j->d;
        for (int i = 0; i < j->d; i++) { const float t = x[i] - mu; v += t * t; }
        const float inv = 1.0f / sqrtf(v / j->d + j->eps);
        for (int i = 0; i < j->d; i++) y[i] = (x[i] - mu) * inv * j->g[i] + j->b[i];
    }
}
static void vt_layernorm(float *Y, const float *X, int n, int d, const float *g, const float *b, float eps) {
    VtNormJob j = { Y, X, g, b, d, eps };
    pool_for(vt_norm_job, &j, n, 32);
}

/* ---- exponencial vetorial (reducao por ln2 e polinomio de grau 6; erro relativo ~1e-7) ----
   So chamada dentro de trechos ja protegidos por __AVX2__ (vt_softmax, vt_gelu_job); sem AVX2 esses
   trechos usam expf/tanhf direto, entao vt_exp8 nem precisa existir fora do x86 (porte ARM, 2026-11-27). */
#ifdef __AVX2__
static inline __m256 vt_exp8(__m256 x) {
    x = _mm256_min_ps(_mm256_max_ps(x, _mm256_set1_ps(-87.0f)), _mm256_set1_ps(88.0f));
    const __m256 k = _mm256_round_ps(_mm256_mul_ps(x, _mm256_set1_ps(1.44269504f)), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m256 r = _mm256_fnmadd_ps(k, _mm256_set1_ps(0.693145752f), x);
    r = _mm256_fnmadd_ps(k, _mm256_set1_ps(1.42860677e-6f), r);
    __m256 p = _mm256_set1_ps(1.0f / 720);
    p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.0f / 120)); p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.0f / 24));
    p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.0f / 6)); p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(0.5f));
    p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.0f)); p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.0f));
    return _mm256_mul_ps(p, _mm256_castsi256_ps(_mm256_slli_epi32(_mm256_add_epi32(_mm256_cvtps_epi32(k), _mm256_set1_epi32(127)), 23)));
}
#endif
#ifdef __AVX2__
VT_512 static void vt_softmax512(float *x, int n) {
    __m512 mx = _mm512_set1_ps(-3.0e38f), sum = _mm512_setzero_ps();
    int i = 0;
    for (; i + 16 <= n; i += 16) mx = _mm512_max_ps(mx, _mm512_loadu_ps(x + i));
    const __mmask16 tail = (__mmask16)((1u << (n - i)) - 1);
    if (tail) mx = _mm512_mask_max_ps(mx, tail, mx, _mm512_maskz_loadu_ps(tail, x + i));
    const __m512 top = _mm512_set1_ps(_mm512_reduce_max_ps(mx));
    for (i = 0; i < n; i += 16) {
        const __mmask16 k16 = n - i >= 16 ? 0xFFFF : tail;
        const __m512 v = _mm512_max_ps(_mm512_sub_ps(_mm512_maskz_loadu_ps(k16, x + i), top), _mm512_set1_ps(-87.0f));
        const __m512 k = _mm512_roundscale_ps(_mm512_mul_ps(v, _mm512_set1_ps(1.44269504f)), 0);
        __m512 r = _mm512_fnmadd_ps(k, _mm512_set1_ps(0.693145752f), v);
        r = _mm512_fnmadd_ps(k, _mm512_set1_ps(1.42860677e-6f), r);
        __m512 p = _mm512_set1_ps(1.0f / 720);
        p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.0f / 120)); p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.0f / 24));
        p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.0f / 6)); p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(0.5f));
        p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.0f)); p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.0f));
        p = _mm512_scalef_ps(p, k);
        sum = _mm512_mask_add_ps(sum, k16, sum, p);
        _mm512_mask_storeu_ps(x + i, k16, p);
    }
    const __m512 inv = _mm512_set1_ps(1.0f / _mm512_reduce_add_ps(sum));
    for (i = 0; i < n; i += 16) {
        const __mmask16 k16 = n - i >= 16 ? 0xFFFF : tail;
        _mm512_mask_storeu_ps(x + i, k16, _mm512_mul_ps(_mm512_maskz_loadu_ps(k16, x + i), inv));
    }
}
#else
/* porte ARM, 2026-11-27: sem SIMD. Mesmo softmax (maximo, exp deslocado, soma, normaliza) de vt_softmax512;
   nunca chamado de fato aqui porque vt_wide() depende de qn_512(), que e sempre falso sem AVX-512. */
static void vt_softmax512(float *x, int n) {
    float mx = x[0];
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) { x[i] = expf(x[i] - mx); sum += x[i]; }
    const float inv = 1.0f / sum;
    for (int i = 0; i < n; i++) x[i] *= inv;
}
#endif
static void vt_softmax(float *x, int n) {
    if (vt_wide()) { vt_softmax512(x, n); return; }
    float mx = x[0], sum = 0.0f; int i;
    for (i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
#ifdef __AVX2__
    __m256 s = _mm256_setzero_ps(); const __m256 top = _mm256_set1_ps(mx);
    for (i = 0; i + 8 <= n; i += 8) { const __m256 e = vt_exp8(_mm256_sub_ps(_mm256_loadu_ps(x + i), top)); s = _mm256_add_ps(s, e); _mm256_storeu_ps(x + i, e); }
    float t[8]; _mm256_storeu_ps(t, s);
    for (int k = 0; k < 8; k++) sum += t[k];
#else
    /* porte ARM, 2026-11-27: sem SIMD; o laco escalar abaixo (que ja existia para a sobra) cobre o intervalo inteiro */
#endif
    for (; i < n; i++) { x[i] = expf(x[i] - mx); sum += x[i]; }
    const float inv = 1.0f / sum;
    for (i = 0; i < n; i++) x[i] *= inv;
}

/* ---- GELU: pela tangente hiperbolica (aproximada, como nos blocos) ou pela funcao de erro (exata) ---- */
typedef struct { float *X; size_t per; int exact; } VtGeluJob;
static void vt_gelu_job(void *cx, int r0, int r1) {
    const VtGeluJob *j = cx;
    float *x = j->X + (size_t)r0 * j->per;
    const size_t n = (size_t)(r1 - r0) * j->per;
    if (j->exact) { for (size_t i = 0; i < n; i++) x[i] = 0.5f * x[i] * (1.0f + erff(x[i] * 0.70710678118654752f)); return; }
    size_t i = 0;
#ifdef __AVX2__
    const __m256 a = _mm256_set1_ps(0.79788456080286535588f), c3 = _mm256_set1_ps(0.044715f), one = _mm256_set1_ps(1.0f);
    for (; i + 8 <= n; i += 8) {      /* 0,5 x (1 + tanh u) = x / (1 + e^(-2u)) */
        const __m256 v = _mm256_loadu_ps(x + i);
        const __m256 u = _mm256_mul_ps(a, _mm256_fmadd_ps(_mm256_mul_ps(_mm256_mul_ps(v, v), v), c3, v));
        _mm256_storeu_ps(x + i, _mm256_div_ps(v, _mm256_add_ps(one, vt_exp8(_mm256_mul_ps(u, _mm256_set1_ps(-2.0f))))));
    }
#else
    /* porte ARM, 2026-11-27: sem SIMD; o laco escalar abaixo (que ja existia para a sobra) cobre o intervalo inteiro */
#endif
    for (; i < n; i++) x[i] = 0.5f * x[i] * (1.0f + tanhf(0.79788456080286535588f * (x[i] + 0.044715f * x[i] * x[i] * x[i])));
}
static void vt_gelu(float *X, int n, size_t per, int exact) { VtGeluJob j = { X, per, exact }; pool_for(vt_gelu_job, &j, n, 8); }

/* ---- atencao sem mascara, varias cabecas: qkv[n][3*d] intercalado por linha (q, k, v), q e k ja girados ----
   O[n][d]; a mesma conta serve a um codificador de imagem e a um transformador de difusao */
typedef struct { const float *qkv; float *O, *KP, *VP; int n, d, hd, h, NR; float scale; } VtAttJob;
static void vt_att_pack(void *cx, int p0, int p1) {      /* chaves viram paineis [painel][hd][NR] */
    const VtAttJob *j = cx;
    const int NR = j->NR, hd = j->hd;
    for (int p = p0; p < p1; p++) for (int r = 0; r < NR; r++) {
        const int t = p * NR + r;
        const float *k = t < j->n ? j->qkv + (size_t)t * 3 * j->d + j->d + (size_t)j->h * hd : NULL;
        float *o = j->KP + (size_t)p * hd * NR + r;
        for (int i = 0; i < hd; i++) o[(size_t)i * NR] = k ? k[i] * j->scale : 0.0f;
    }
}
static void vt_att_packv(void *cx, int p0, int p1) {     /* valores: [painel de colunas][n][NR] */
    const VtAttJob *j = cx;
    const int NR = j->NR, hd = j->hd;
    for (int p = p0; p < p1; p++) {
        const int w = hd - p * NR < NR ? hd - p * NR : NR;
        for (int t = 0; t < j->n; t++) {
            float *o = j->VP + (size_t)p * j->n * NR + (size_t)t * NR;
            memset(o, 0, (size_t)NR * 4);
            memcpy(o, j->qkv + (size_t)t * 3 * j->d + 2 * (size_t)j->d + (size_t)j->h * hd + (size_t)p * NR, (size_t)w * 4);
        }
    }
}
#define VT_AQ 24
static void vt_att_job(void *cx, int j0, int j1) {
    const VtAttJob *j = cx;
    float *sc = vt_alloc((size_t)VT_AQ * j->n * 4);
    for (int b = j0; b < j1; b++) {
        const int t0 = b * VT_AQ, nq = j->n - t0 < VT_AQ ? j->n - t0 : VT_AQ;
        vt_gemm_packed(sc, (size_t)j->n, j->qkv + (size_t)t0 * 3 * j->d + (size_t)j->h * j->hd, (size_t)3 * j->d, j->KP, nq, j->hd, j->n);
        for (int t = 0; t < nq; t++) vt_softmax(sc + (size_t)t * j->n, j->n);
        vt_gemm_packed(j->O + (size_t)t0 * j->d + (size_t)j->h * j->hd, (size_t)j->d, sc, (size_t)j->n, j->VP, nq, j->n, j->hd);
    }
    vt_free(sc);
}
static void vt_attention(float *O, const float *qkv, int n, int d, int heads) {
    const int hd = d / heads, NR = vt_nr(), np = (n + NR - 1) / NR, nv = (hd + NR - 1) / NR;
    VtAttJob j = { qkv, O, vt_alloc((size_t)np * NR * hd * 4), vt_alloc((size_t)nv * NR * n * 4), n, d, hd, 0, NR, 1.0f / sqrtf((float)hd) };
    for (int h = 0; h < heads; h++) {
        j.h = h;
        pool_for(vt_att_pack, &j, np, 4);
        pool_for(vt_att_packv, &j, nv, 1);
        pool_for(vt_att_job, &j, (n + VT_AQ - 1) / VT_AQ, 1);
    }
    vt_free(j.KP); vt_free(j.VP);
}
