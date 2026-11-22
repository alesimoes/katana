/* ---- produtos da CPU do treino (W x, W^T dy, gradiente de W): kernels em blocos ----
   Incluido por train.c (usa TPar, Mat, pool_for, dequant). KATANA_TRAIN_KERNEL=0 volta aos lacos simples.
   Ideias: cada linha de W e desquantizada uma vez (SIMD) e usada por todos os tokens do lote; W^T dy divide as
   LINHAS entre as threads (cada uma com seu acumulador de dx, somados no fim) em vez das colunas (que faziam
   todas as threads desquantizarem todas as linhas); micro-kernels com varios tokens e varias linhas por vez. */

static int tk_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("KATANA_TRAIN_KERNEL"); v = e ? atoi(e) != 0 : 1; }
    return v;
}

/* memoria de trabalho por thread (cresce, nunca encolhe) */
static __thread float *tk_tls;
static __thread size_t tk_tls_n;
static float *tk_scratch(size_t n) {
    if (n > tk_tls_n) { free(tk_tls); tk_tls = (float *)xalloc(n, sizeof(float)); tk_tls_n = n; }
    return tk_tls;
}
/* memoria do chamador (acumuladores por pedaco), compartilhada: os produtos nao se aninham */
static float *tk_acc; static size_t tk_acc_n;
static float *tk_accbuf(size_t n) {
    if (n > tk_acc_n) { free(tk_acc); tk_acc = (float *)xalloc(n, sizeof(float)); tk_acc_n = n; }
    return tk_acc;
}

/* ---- desquantizacao SIMD (AVX2) dos formatos que os modelos usam; o resto cai em dequant() ---- */
static inline __m256 tk_u8f(__m128i b) { return _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(b)); }

static void tk_dq_q8_0(const uint8_t *src, float *out, size_t nb) {
    for (size_t b = 0; b < nb; b++, src += 34, out += 32) {
        const __m256 d = _mm256_set1_ps(h2f(src));
        for (int j = 0; j < 32; j += 8) {
            const __m256i v = _mm256_cvtepi8_epi32(_mm_loadl_epi64((const __m128i *)(src + 2 + j)));
            _mm256_storeu_ps(out + j, _mm256_mul_ps(_mm256_cvtepi32_ps(v), d));
        }
    }
}

static void tk_dq_q4k(const uint8_t *src, float *out, size_t nb) {
    const __m128i m15 = _mm_set1_epi8(15);
    for (size_t b = 0; b < nb; b++, src += 144) {
        const float d = h2f(src), dmin = h2f(src + 2);
        const uint8_t *sc = src + 4, *q = src + 16;
        for (int j = 0; j < 8; j += 2, q += 32, out += 64) {
            int s1, m1, s2, m2;
            k4_scale_min(j, sc, &s1, &m1); k4_scale_min(j + 1, sc, &s2, &m2);
            const __m256 d1 = _mm256_set1_ps(d * s1), n1 = _mm256_set1_ps(dmin * m1);
            const __m256 d2 = _mm256_set1_ps(d * s2), n2 = _mm256_set1_ps(dmin * m2);
            for (int l = 0; l < 32; l += 8) {
                const __m128i x = _mm_loadl_epi64((const __m128i *)(q + l));
                _mm256_storeu_ps(out + l, _mm256_fmsub_ps(tk_u8f(_mm_and_si128(x, m15)), d1, n1));
                _mm256_storeu_ps(out + 32 + l, _mm256_fmsub_ps(tk_u8f(_mm_and_si128(_mm_srli_epi16(x, 4), m15)), d2, n2));
            }
        }
    }
}

static void tk_dq_q5k(const uint8_t *src, float *out, size_t nb) {
    const __m128i m15 = _mm_set1_epi8(15), m1b = _mm_set1_epi8(1);
    for (size_t b = 0; b < nb; b++, src += 176) {
        const float d = h2f(src), dmin = h2f(src + 2);
        const uint8_t *sc = src + 4, *qh = src + 16, *q = src + 48;
        for (int j = 0; j < 8; j += 2, q += 32, out += 64) {
            int s1, m1, s2, m2;
            k4_scale_min(j, sc, &s1, &m1); k4_scale_min(j + 1, sc, &s2, &m2);
            const __m256 d1 = _mm256_set1_ps(d * s1), n1 = _mm256_set1_ps(dmin * m1);
            const __m256 d2 = _mm256_set1_ps(d * s2), n2 = _mm256_set1_ps(dmin * m2);
            for (int l = 0; l < 32; l += 8) {
                const __m128i x = _mm_loadl_epi64((const __m128i *)(q + l));
                const __m128i h = _mm_loadl_epi64((const __m128i *)(qh + l));
                const __m128i h1 = _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(h, j), m1b), 4);
                const __m128i h2 = _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(h, j + 1), m1b), 4);
                _mm256_storeu_ps(out + l, _mm256_fmsub_ps(tk_u8f(_mm_or_si128(_mm_and_si128(x, m15), h1)), d1, n1));
                _mm256_storeu_ps(out + 32 + l, _mm256_fmsub_ps(tk_u8f(_mm_or_si128(_mm_and_si128(_mm_srli_epi16(x, 4), m15), h2)), d2, n2));
            }
        }
    }
}

static void tk_dq_q6k(const uint8_t *src, float *out, size_t nb) {
    const __m128i m15 = _mm_set1_epi8(15), m3 = _mm_set1_epi8(3);
    const __m256i c32 = _mm256_set1_epi32(32);
    for (size_t b = 0; b < nb; b++, src += 210) {
        const uint8_t *ql = src, *qh = src + 128;
        const int8_t *sc = (const int8_t *)(src + 192);
        const float d = h2f(src + 208);
        for (int c = 0; c < 2; c++, out += 128, ql += 64, qh += 32, sc += 8) {
            for (int l = 0; l < 32; l += 8) {
                const int is = l / 16;
                const __m128i a = _mm_loadl_epi64((const __m128i *)(ql + l));
                const __m128i a2 = _mm_loadl_epi64((const __m128i *)(ql + 32 + l));
                const __m128i h = _mm_loadl_epi64((const __m128i *)(qh + l));
                const __m128i q1 = _mm_or_si128(_mm_and_si128(a, m15), _mm_slli_epi16(_mm_and_si128(h, m3), 4));
                const __m128i q2 = _mm_or_si128(_mm_and_si128(a2, m15), _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(h, 2), m3), 4));
                const __m128i q3 = _mm_or_si128(_mm_and_si128(_mm_srli_epi16(a, 4), m15), _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(h, 4), m3), 4));
                const __m128i q4 = _mm_or_si128(_mm_and_si128(_mm_srli_epi16(a2, 4), m15), _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(h, 6), m3), 4));
                _mm256_storeu_ps(out + l, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_cvtepu8_epi32(q1), c32)), _mm256_set1_ps(d * sc[is])));
                _mm256_storeu_ps(out + l + 32, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_cvtepu8_epi32(q2), c32)), _mm256_set1_ps(d * sc[is + 2])));
                _mm256_storeu_ps(out + l + 64, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_cvtepu8_epi32(q3), c32)), _mm256_set1_ps(d * sc[is + 4])));
                _mm256_storeu_ps(out + l + 96, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_cvtepu8_epi32(q4), c32)), _mm256_set1_ps(d * sc[is + 6])));
            }
        }
    }
}

static void tk_dq_q5_1(const uint8_t *src, float *out, size_t nb) {
    const __m128i m15 = _mm_set1_epi8(15);
    const __m256i sh = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7), one = _mm256_set1_epi32(1);
    for (size_t b = 0; b < nb; b++, src += 24, out += 32) {
        const __m256 d = _mm256_set1_ps(h2f(src)), m = _mm256_set1_ps(h2f(src + 2));
        uint32_t qh; memcpy(&qh, src + 4, 4);
        const uint8_t *qs = src + 8;
        for (int j = 0; j < 16; j += 8) {
            const __m128i x = _mm_loadl_epi64((const __m128i *)(qs + j));
            const __m256i hl = _mm256_slli_epi32(_mm256_and_si256(_mm256_srlv_epi32(_mm256_set1_epi32((int)(qh >> j)), sh), one), 4);
            const __m256i hh = _mm256_slli_epi32(_mm256_and_si256(_mm256_srlv_epi32(_mm256_set1_epi32((int)(qh >> (j + 16))), sh), one), 4);
            const __m256i lo = _mm256_or_si256(_mm256_cvtepu8_epi32(_mm_and_si128(x, m15)), hl);
            const __m256i hi = _mm256_or_si256(_mm256_cvtepu8_epi32(_mm_and_si128(_mm_srli_epi16(x, 4), m15)), hh);
            _mm256_storeu_ps(out + j, _mm256_fmadd_ps(_mm256_cvtepi32_ps(lo), d, m));
            _mm256_storeu_ps(out + j + 16, _mm256_fmadd_ps(_mm256_cvtepi32_ps(hi), d, m));
        }
    }
}

/* n valores de uma linha quantizada -> float */
static void tk_dequant(uint32_t type, const uint8_t *src, float *out, size_t n) {
    switch (type) {
    case T_Q8_0: if (n % 32 == 0) { tk_dq_q8_0(src, out, n / 32); return; } break;
    case T_Q4_K: if (n % 256 == 0) { tk_dq_q4k(src, out, n / 256); return; } break;
    case T_Q5_K: if (n % 256 == 0) { tk_dq_q5k(src, out, n / 256); return; } break;
    case T_Q6_K: if (n % 256 == 0) { tk_dq_q6k(src, out, n / 256); return; } break;
    case T_Q5_1: if (n % 32 == 0) { tk_dq_q5_1(src, out, n / 32); return; } break;
    default: break;
    }
    dequant(type, src, out, n);
}

/* ponteiros para as linhas [r0, r0+n) em float: direto se o parametro ja e float, senao desquantiza em buf (n*C) */
static void tk_rows(const TPar *p, size_t r0, int n, float *buf, const float **ptr) {
    const size_t C = p->cols;
    for (int i = 0; i < n; i++) {
        const size_t r = r0 + i;
        if (p->w) ptr[i] = p->w + r * C;
        else if (p->vec) ptr[i] = p->vec;
        else if (p->mat->f) ptr[i] = p->mat->f + r * C;
        else { tk_dequant(p->mat->type, p->mat->q + r * p->mat->stride, buf + (size_t)i * C, C); ptr[i] = buf + (size_t)i * C; }
    }
}

static inline float tk_hsum(__m256 v) {
    __m128 lo = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    lo = _mm_hadd_ps(lo, lo); lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
}

/* ---- W x: out[b][r] = W[r] . x[b] ---- */
/* 4 linhas x NB tokens: NB constante para o compilador manter os acumuladores em registradores */
static inline __attribute__((always_inline)) void tk_mv_tile(const float *const *w, const float *x, size_t C, size_t R, float *out, size_t r, const int NB) {
    __m256 acc[4][3];
    for (int i = 0; i < 4; i++) for (int t = 0; t < NB; t++) acc[i][t] = _mm256_setzero_ps();
    size_t c = 0;
    for (; c + 8 <= C; c += 8) {
        __m256 xv[3];
        for (int t = 0; t < NB; t++) xv[t] = _mm256_loadu_ps(x + (size_t)t * C + c);
        for (int i = 0; i < 4; i++) {
            const __m256 wv = _mm256_loadu_ps(w[i] + c);
            for (int t = 0; t < NB; t++) acc[i][t] = _mm256_fmadd_ps(wv, xv[t], acc[i][t]);
        }
    }
    for (int i = 0; i < 4; i++) for (int t = 0; t < NB; t++) {
        float s = tk_hsum(acc[i][t]);
        for (size_t k = c; k < C; k++) s += w[i][k] * x[(size_t)t * C + k];
        out[(size_t)t * R + r + i] = s;
    }
}

typedef struct { const TPar *p; float *out; const float *x; int B; } TkMvJob;
static void tk_mv_job(void *ctx, int r0, int r1) {
    const TkMvJob *j = (const TkMvJob *)ctx;
    const size_t C = j->p->cols, R = j->p->rows;
    float *buf = (j->p->w || j->p->vec || j->p->mat->f) ? NULL : tk_scratch(4 * C);
    const float *w[4];
    int r = r0;
    for (; r + 4 <= r1; r += 4) {
        tk_rows(j->p, (size_t)r, 4, buf, w);
        int b = 0;
        for (; b + 3 <= j->B; b += 3) tk_mv_tile(w, j->x + (size_t)b * C, C, R, j->out + (size_t)b * R, (size_t)r, 3);
        if (j->B - b == 2) tk_mv_tile(w, j->x + (size_t)b * C, C, R, j->out + (size_t)b * R, (size_t)r, 2);
        else if (j->B - b == 1) tk_mv_tile(w, j->x + (size_t)b * C, C, R, j->out + (size_t)b * R, (size_t)r, 1);
    }
    for (; r < r1; r++) {
        tk_rows(j->p, (size_t)r, 1, buf, w);
        for (int b = 0; b < j->B; b++) j->out[(size_t)b * R + r] = dot(w[0], j->x + (size_t)b * C, (int)C);
    }
}
static void tk_mv(const TPar *p, float *out, const float *x, int B) {
    TkMvJob j = { p, out, x, B };
    const int R = (int)p->rows;
    int chunk = (R + pool.n * 4 - 1) / (pool.n * 4);
    chunk = (chunk + 3) & ~3;
    pool_for(tk_mv_job, &j, R, chunk < 4 ? 4 : chunk);
}

/* ---- W^T dy: dx[b][c] += sum_r dy[b][r] W[r][c] ----
   as linhas se dividem em pedacos fixos; cada pedaco soma num acumulador proprio, depois os pedacos sao somados em dx */
typedef struct { const TPar *p; const float *dy; float *acc; int B, chunk; } TkMtJob;

/* NB tokens x 16 colunas, sobre nr linhas desquantizadas (wt[r] aponta a linha, com 16 colunas a partir de c) */
static inline __attribute__((always_inline)) void tk_mt_tile(const float *const *wt, int nr, size_t c, const float *dy, size_t R, size_t r0,
                                                            float *acc, size_t C, const int NB) {
    __m256 a0[4], a1[4];
    for (int t = 0; t < NB; t++) { a0[t] = _mm256_loadu_ps(acc + (size_t)t * C + c); a1[t] = _mm256_loadu_ps(acc + (size_t)t * C + c + 8); }
    for (int i = 0; i < nr; i++) {
        const __m256 w0 = _mm256_loadu_ps(wt[i] + c), w1 = _mm256_loadu_ps(wt[i] + c + 8);
        for (int t = 0; t < NB; t++) {
            const __m256 a = _mm256_broadcast_ss(dy + (size_t)t * R + r0 + i);
            a0[t] = _mm256_fmadd_ps(a, w0, a0[t]); a1[t] = _mm256_fmadd_ps(a, w1, a1[t]);
        }
    }
    for (int t = 0; t < NB; t++) { _mm256_storeu_ps(acc + (size_t)t * C + c, a0[t]); _mm256_storeu_ps(acc + (size_t)t * C + c + 8, a1[t]); }
}

static void tk_mt_job(void *ctx, int r0, int r1) {
    const TkMtJob *j = (const TkMtJob *)ctx;
    const size_t C = j->p->cols, R = j->p->rows;
    const int B = j->B;
    float *acc = j->acc + (size_t)(r0 / j->chunk) * B * C;
    memset(acc, 0, (size_t)B * C * sizeof(float));
    enum { RB = 8 };
    float *buf = (j->p->w || j->p->vec || j->p->mat->f) ? NULL : tk_scratch((size_t)RB * C);
    const float *w[RB];
    const size_t C16 = C & ~(size_t)15;
    for (int r = r0; r < r1; r += RB) {
        const int nr = r1 - r < RB ? r1 - r : RB;
        tk_rows(j->p, (size_t)r, nr, buf, w);
        for (size_t c = 0; c < C16; c += 16) {
            int b = 0;
            for (; b + 4 <= B; b += 4) tk_mt_tile(w, nr, c, j->dy + (size_t)b * R, R, (size_t)r, acc + (size_t)b * C, C, 4);
            const int rem = B - b;
            if (rem == 3) tk_mt_tile(w, nr, c, j->dy + (size_t)b * R, R, (size_t)r, acc + (size_t)b * C, C, 3);
            else if (rem == 2) tk_mt_tile(w, nr, c, j->dy + (size_t)b * R, R, (size_t)r, acc + (size_t)b * C, C, 2);
            else if (rem == 1) tk_mt_tile(w, nr, c, j->dy + (size_t)b * R, R, (size_t)r, acc + (size_t)b * C, C, 1);
        }
        for (size_t c = C16; c < C; c++)
            for (int b = 0; b < B; b++) { float s = 0; for (int i = 0; i < nr; i++) s += j->dy[(size_t)b * R + r + i] * w[i][c]; acc[(size_t)b * C + c] += s; }
    }
}

typedef struct { float *dx; const float *acc; size_t n; int nch; } TkRedJob;
static void tk_red_job(void *ctx, int i0, int i1) {
    const TkRedJob *j = (const TkRedJob *)ctx;
    for (int i = i0; i < i1; i++) {
        float s = 0;
        for (int k = 0; k < j->nch; k++) s += j->acc[(size_t)k * j->n + i];
        j->dx[i] += s;
    }
}

static void tk_mvT(const TPar *p, float *dx, const float *dy, int B) {
    const int R = (int)p->rows;
    const size_t C = p->cols;
    int nch = pool.n > 1 ? pool.n : 1;
    if (nch > (R + 7) / 8) nch = (R + 7) / 8;
    int chunk = (R + nch - 1) / nch;
    chunk = (chunk + 7) & ~7;
    nch = (R + chunk - 1) / chunk;
    float *acc = tk_accbuf((size_t)nch * B * C);
    TkMtJob j = { p, dy, acc, B, chunk };
    pool_for(tk_mt_job, &j, R, chunk);
    TkRedJob rj = { dx, acc, (size_t)B * C, nch };
    const int n = B * (int)C;
    pool_for(tk_red_job, &rj, n, n / (pool.n * 2) + 64);
}

/* ---- gradiente de W: g[r][c] += sum_b dy[b][r] x[b][c]; 4 tokens por passada sobre g ---- */
typedef struct { TPar *p; const float *dy, *x; int B; } TkGJob;
static void tk_g_job(void *ctx, int r0, int r1) {
    const TkGJob *j = (const TkGJob *)ctx;
    const size_t C = j->p->cols, R = j->p->rows;
    for (int r = r0; r < r1; r++) {
        float *g = j->p->g + (size_t)r * C;
        int b = 0;
        for (; b + 4 <= j->B; b += 4) {
            const float a0 = j->dy[(size_t)b * R + r], a1 = j->dy[(size_t)(b + 1) * R + r], a2 = j->dy[(size_t)(b + 2) * R + r], a3 = j->dy[(size_t)(b + 3) * R + r];
            if (a0 == 0.0f && a1 == 0.0f && a2 == 0.0f && a3 == 0.0f) continue;
            const float *x0 = j->x + (size_t)b * C, *x1 = x0 + C, *x2 = x1 + C, *x3 = x2 + C;
            const __m256 v0 = _mm256_set1_ps(a0), v1 = _mm256_set1_ps(a1), v2 = _mm256_set1_ps(a2), v3 = _mm256_set1_ps(a3);
            size_t c = 0;
            for (; c + 8 <= C; c += 8) {
                __m256 s = _mm256_loadu_ps(g + c);
                s = _mm256_fmadd_ps(v0, _mm256_loadu_ps(x0 + c), s); s = _mm256_fmadd_ps(v1, _mm256_loadu_ps(x1 + c), s);
                s = _mm256_fmadd_ps(v2, _mm256_loadu_ps(x2 + c), s); s = _mm256_fmadd_ps(v3, _mm256_loadu_ps(x3 + c), s);
                _mm256_storeu_ps(g + c, s);
            }
            for (; c < C; c++) g[c] += a0 * x0[c] + a1 * x1[c] + a2 * x2[c] + a3 * x3[c];
        }
        for (; b < j->B; b++) {
            const float a = j->dy[(size_t)b * R + r];
            if (a == 0.0f) continue;
            const float *x = j->x + (size_t)b * C;
            for (size_t c = 0; c < C; c++) g[c] += a * x[c];
        }
    }
}
static void tk_grad(TPar *p, const float *dy, const float *x, int B) {
    TkGJob j = { p, dy, x, B };
    pool_for(tk_g_job, &j, (int)p->rows, 16);
}
