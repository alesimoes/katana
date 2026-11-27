/* ---------- katana attbench [posicoes...] ----------
 * Custo da atencao do qwen35moe por posicao, sem modelo e sem prompt: enche o cache KV (10 camadas, 16 cabecas q, 2 kv,
 * 256 por cabeca) com valores aleatorios e mede a geracao (1 token) e o prompt (lote de 64) em cada posicao, com o codigo
 * de antes de 2026-11-22 (copia abaixo, uma conversao por cabeca e token) e o atual, alternados; confere a diferenca. */
typedef struct { Model *m; const Layer *L; const float *q; float *part; int pos, hd, kvd, group, nch, chl; float scale; } QnAttJob;
static void ab_old_job(void *cx, int j0, int j1) {
    const QnAttJob *j = cx;
    const Model *m = j->m;
    const int hd = j->hd, kvd = j->kvd, n = j->pos + 1, G = j->group;
    const size_t off = j->L->kv_off;
    float sc[QN_MAXG][64], o[QN_MAXG][512], mx[QN_MAXG], sum[QN_MAXG], kr[512];
    for (int job = j0; job < j1; job++) {
        const int g = job / j->nch, ch = job % j->nch;
        const int ta = ch * j->chl, tb = ta + j->chl < n ? ta + j->chl : n;
        const size_t kvh = (size_t)g * hd;
        for (int u = 0; u < G; u++) { mx[u] = -INFINITY; sum[u] = 0.0f; memset(o[u], 0, hd * sizeof(float)); }
        for (int t0 = ta; t0 < tb; t0 += 64) {
            const int nb = tb - t0 < 64 ? tb - t0 : 64;
            for (int i = 0; i < nb; i++) {
                const size_t r = off + (size_t)(t0 + i) * kvd + kvh;
                if (m->kq8) {       /* Q8_0: abre a linha uma vez e serve as G cabecas */
                    q8_get(kr, m->kq8 + r / 32 * 34, hd);
                    for (int u = 0; u < G; u++) sc[u][i] = dot(j->q + (size_t)(g * G + u) * hd, kr, hd) * j->scale;
                    continue;
                }
                for (int u = 0; u < G; u++) {
                    const float *qh = j->q + (size_t)(g * G + u) * hd;
                    sc[u][i] = (m->kh ? dot_h(qh, m->kh + r, hd) : dot(qh, m->kcache + r, hd)) * j->scale;
                }
            }
            for (int u = 0; u < G; u++) {
                float bm = mx[u];
                for (int i = 0; i < nb; i++) if (sc[u][i] > bm) bm = sc[u][i];
                if (bm > mx[u]) {       /* novo maximo: reescala o que ja foi somado */
                    const float f = expf(mx[u] - bm);
                    sum[u] *= f;
                    for (int k = 0; k < hd; k++) o[u][k] *= f;
                    mx[u] = bm;
                }
                for (int i = 0; i < nb; i++) { sc[u][i] = expf(sc[u][i] - bm); sum[u] += sc[u][i]; }
            }
            for (int i = 0; i < nb; i++) {
                const size_t r = off + (size_t)(t0 + i) * kvd + kvh;
                if (m->vq8) {       /* Q8_0: soma direto dos inteiros, escala do bloco dobrada no peso */
                    const uint8_t *vb = m->vq8 + r / 32 * 34;
#ifdef __AVX2__
                    for (int k = 0; k < hd; k += 32, vb += 34) {
                        uint16_t h; memcpy(&h, vb, 2);
                        const float dv = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h)));
                        __m256 q4[4];
                        for (int e = 0; e < 4; e++) q4[e] = _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_loadl_epi64((const __m128i *)(vb + 2 + 8 * e))));
                        for (int u = 0; u < G; u++) {
                            const __m256 w = _mm256_set1_ps(sc[u][i] * dv);
                            float *ou = o[u] + k;
                            for (int e = 0; e < 4; e++) _mm256_storeu_ps(ou + 8 * e, _mm256_fmadd_ps(w, q4[e], _mm256_loadu_ps(ou + 8 * e)));
                        }
                    }
#else
                    /* porte ARM, 2026-11-27: sem SIMD */
                    for (int k = 0; k < hd; k += 32, vb += 34) {
                        uint16_t h; memcpy(&h, vb, 2);
                        const float dv = f16_to_f32(h);
                        const int8_t *qv = (const int8_t *)(vb + 2);
                        for (int u = 0; u < G; u++) {
                            const float w = sc[u][i] * dv;
                            float *ou = o[u] + k;
                            for (int e = 0; e < 32; e++) ou[e] += w * qv[e];
                        }
                    }
#endif
                    continue;
                }
#ifdef __AVX2__
                for (int k = 0; k < hd; k += 8) {
                    const __m256 vv = m->vh ? _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(m->vh + r + k))) : _mm256_loadu_ps(m->vcache + r + k);
                    for (int u = 0; u < G; u++)
                        _mm256_storeu_ps(o[u] + k, _mm256_fmadd_ps(_mm256_set1_ps(sc[u][i]), vv, _mm256_loadu_ps(o[u] + k)));
                }
#else
                /* porte ARM, 2026-11-27: sem SIMD */
                for (int k = 0; k < hd; k++) {
                    const float vv = m->vh ? f16_to_f32(m->vh[r + k]) : m->vcache[r + k];
                    for (int u = 0; u < G; u++) o[u][k] += sc[u][i] * vv;
                }
#endif
            }
        }
        for (int u = 0; u < G; u++) {   /* parcial: [maximo, soma, saida] por (cabeca q, pedaco) */
            float *pp = j->part + ((size_t)(g * G + u) * j->nch + ch) * (hd + 2);
            pp[0] = mx[u]; pp[1] = sum[u];
            memcpy(pp + 2, o[u], hd * sizeof(float));
        }
    }
}

static void ab_old_1(Model *m, const Layer *L, int pos, const float *q, const float *qg, float *out) {
    const Config *c = &m->cfg;
    const int nh = c->n_heads, hd = c->hd, kvd = c->n_kv * hd, group = nh / c->n_kv;
    const float scale = 1.0f / sqrtf((float)hd);
    if (hd > 512 || hd % 8 || group > QN_MAXG) die("qn_core: hd ou grupo fora do suportado");
    static float *part;
    if (!part) part = xalloc((size_t)nh * QN_MAXCH * (hd + 2), sizeof(float));
    const int n = pos + 1;
    int chl = 512;      /* pedaco minimo; cresce para caber em QN_MAXCH pedacos */
    while ((n + chl - 1) / chl > QN_MAXCH) chl *= 2;
    const int nch = (n + chl - 1) / chl;
    QnAttJob job = { m, L, q, part, pos, hd, kvd, group, nch, chl, scale * c->kq_mul };
    pool_for(ab_old_job, &job, c->n_kv * nch, 1);
    for (int h = 0; h < nh; h++) {      /* junta os pedacos e aplica o portao */
        const float *pp = part + (size_t)h * nch * (hd + 2);
        float M = -INFINITY, S = 0.0f;
        for (int ch = 0; ch < nch; ch++) if (pp[(size_t)ch * (hd + 2)] > M) M = pp[(size_t)ch * (hd + 2)];
        float *oh = out + (size_t)h * hd;
        memset(oh, 0, hd * sizeof(float));
        for (int ch = 0; ch < nch; ch++) {
            const float *pc = pp + (size_t)ch * (hd + 2), f = expf(pc[0] - M);
            S += pc[1] * f;
            for (int k = 0; k < hd; k++) oh[k] += pc[2 + k] * f;
        }
        const float *gt = qg + (size_t)h * 2 * hd + hd, inv = 1.0f / S;
        for (int k = 0; k < hd; k++) oh[k] *= inv / (1.0f + expf(-gt[k]));
    }
}

typedef struct { Model *m; const Layer *L; const float *q; float *part; int pos0, B, hd, kvd, group, nch, chl, nsp, tps, qd; float scale; } AbOldNJob;
static void ab_old_n_job(void *cx, int j0, int j1) {
    const AbOldNJob *j = cx;
    const Model *m = j->m;
    const int hd = j->hd, kvd = j->kvd, G = j->group;
    const size_t off = j->L->kv_off;
    static __thread float *ob, *scb; static __thread int ocap;
    const int nq = j->tps * G;
    if (ocap < nq) { free(ob); free(scb); ob = xalloc((size_t)nq * 512, sizeof(float)); scb = xalloc((size_t)nq * 64, sizeof(float)); ocap = nq; }
    float mx[GPU_BMAX * QN_MAXG], sum[GPU_BMAX * QN_MAXG], kr[512];
    for (int job = j0; job < j1; job++) {
        const int sp = job % j->nsp, ch = (job / j->nsp) % j->nch, g = job / (j->nsp * j->nch);
        const int t0q = sp * j->tps, t1q = t0q + j->tps < j->B ? t0q + j->tps : j->B;
        if (t0q >= t1q) continue;
        const int ta = ch * j->chl;
        const size_t kvh = (size_t)g * hd;
        int tmax = ta;      /* fim do pedaco para o ultimo token da faixa */
        { const int n = j->pos0 + t1q; tmax = ta + j->chl < n ? ta + j->chl : n; }
        for (int u = 0; u < (t1q - t0q) * G; u++) { mx[u] = -INFINITY; sum[u] = 0.0f; memset(ob + (size_t)u * 512, 0, hd * sizeof(float)); }
        for (int b0 = ta; b0 < tmax; b0 += 64) {
            for (int t = t0q; t < t1q; t++) {
                const int n = j->pos0 + t + 1, tb = ta + j->chl < n ? ta + j->chl : n, nb = tb - b0 < 64 ? tb - b0 : 64;
                if (nb <= 0) continue;
                for (int i = 0; i < nb; i++) {
                    const size_t r = off + (size_t)(b0 + i) * kvd + kvh;
                    if (m->kq8) {   /* Q8_0: abre a linha uma vez para as G cabecas */
                        q8_get(kr, m->kq8 + r / 32 * 34, hd);
                        for (int u = 0; u < G; u++)
                            scb[(size_t)((t - t0q) * G + u) * 64 + i] = dot(j->q + (size_t)t * j->qd + (size_t)(g * G + u) * hd, kr, hd) * j->scale;
                        continue;
                    }
                    for (int u = 0; u < G; u++) {
                        const float *qh = j->q + (size_t)t * j->qd + (size_t)(g * G + u) * hd;
                        scb[(size_t)((t - t0q) * G + u) * 64 + i] = (m->kh ? dot_h(qh, m->kh + r, hd) : dot(qh, m->kcache + r, hd)) * j->scale;
                    }
                }
                for (int u = 0; u < G; u++) {
                    const int qi = (t - t0q) * G + u;
                    float *sc = scb + (size_t)qi * 64, *o = ob + (size_t)qi * 512;
                    float bm = mx[qi];
                    for (int i = 0; i < nb; i++) if (sc[i] > bm) bm = sc[i];
                    if (bm > mx[qi]) {
                        const float f = expf(mx[qi] - bm);
                        sum[qi] *= f;
                        for (int k = 0; k < hd; k++) o[k] *= f;
                        mx[qi] = bm;
                    }
                    for (int i = 0; i < nb; i++) { sc[i] = expf(sc[i] - bm); sum[qi] += sc[i]; }
                }
                for (int i = 0; i < nb; i++) {
                    const size_t r = off + (size_t)(b0 + i) * kvd + kvh;
                    if (m->vq8) q8_get(kr, m->vq8 + r / 32 * 34, hd);
#ifdef __AVX2__
                    for (int k = 0; k < hd; k += 8) {
                        const __m256 vv = m->vq8 ? _mm256_loadu_ps(kr + k) : m->vh ? _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(m->vh + r + k))) : _mm256_loadu_ps(m->vcache + r + k);
                        for (int u = 0; u < G; u++) {
                            const int qi = (t - t0q) * G + u;
                            float *o = ob + (size_t)qi * 512;
                            _mm256_storeu_ps(o + k, _mm256_fmadd_ps(_mm256_set1_ps(scb[(size_t)qi * 64 + i]), vv, _mm256_loadu_ps(o + k)));
                        }
                    }
#else
                    /* porte ARM, 2026-11-27: sem SIMD */
                    for (int k = 0; k < hd; k++) {
                        const float vv = m->vq8 ? kr[k] : m->vh ? f16_to_f32(m->vh[r + k]) : m->vcache[r + k];
                        for (int u = 0; u < G; u++) {
                            const int qi = (t - t0q) * G + u;
                            float *o = ob + (size_t)qi * 512;
                            o[k] += scb[(size_t)qi * 64 + i] * vv;
                        }
                    }
#endif
                }
            }
        }
        for (int t = t0q; t < t1q; t++)
            for (int u = 0; u < G; u++) {
                const int qi = (t - t0q) * G + u;
                float *pp = j->part + (((size_t)t * j->qd / hd + g * G + u) * j->nch + ch) * (hd + 2);
                pp[0] = mx[qi]; pp[1] = sum[qi];
                memcpy(pp + 2, ob + (size_t)qi * 512, hd * sizeof(float));
            }
    }
}

static void ab_old_n(Model *m, const Layer *L, int pos0, int B, const float *Q, const float *P1, float *O) {
    const Config *c = &m->cfg;
    const int nh = c->n_heads, hd = c->hd, kvd = c->n_kv * hd, group = nh / c->n_kv, qd = nh * hd;
    const float scale = 1.0f / sqrtf((float)hd);
    static float *part;
    if (!part) part = xalloc((size_t)GPU_BMAX * nh * QN_MAXCH * (hd + 2), sizeof(float));
    const int nmax = pos0 + B;
    int chl = 512;
    while ((nmax + chl - 1) / chl > QN_MAXCH) chl *= 2;
    const int nch = (nmax + chl - 1) / chl;
    /* faixas de tokens: trabalhos suficientes para as threads quando ha poucos pedacos */
    int nsp = (2 * (pool.n > 0 ? pool.n : 1) + c->n_kv * nch - 1) / (c->n_kv * nch);
    if (nsp > B) nsp = B;
    if (nsp < 1) nsp = 1;
    const int tps = (B + nsp - 1) / nsp;
    nsp = (B + tps - 1) / tps;
    AbOldNJob job = { m, L, Q, part, pos0, B, hd, kvd, group, nch, chl, nsp, tps, qd, scale * c->kq_mul };
    pool_for(ab_old_n_job, &job, c->n_kv * nch * nsp, 1);
    for (int t = 0; t < B; t++) {
        const int n = pos0 + t + 1, ncht = (n + chl - 1) / chl;
        for (int h = 0; h < nh; h++) {      /* junta os pedacos do token e aplica o portao */
            const float *pp = part + ((size_t)t * nh + h) * nch * (hd + 2);
            float M = -INFINITY, S = 0.0f;
            for (int ch = 0; ch < ncht; ch++) if (pp[(size_t)ch * (hd + 2)] > M) M = pp[(size_t)ch * (hd + 2)];
            float *oh = O + (size_t)t * qd + (size_t)h * hd;
            memset(oh, 0, hd * sizeof(float));
            for (int ch = 0; ch < ncht; ch++) {
                const float *pc = pp + (size_t)ch * (hd + 2), f = expf(pc[0] - M);
                S += pc[1] * f;
                for (int k = 0; k < hd; k++) oh[k] += pc[2 + k] * f;
            }
            const float *gt = P1 + (size_t)t * 2 * qd + (size_t)h * 2 * hd + hd, inv = 1.0f / S;
            for (int k = 0; k < hd; k++) oh[k] *= inv / (1.0f + expf(-gt[k]));
        }
    }
}


static double ab_now(void) {      /* uma thread: tempo de processador (a maquina e dividida); varias: relogio */
    if (pool.n > 1) return now_sec();
    struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}
static int attbench(int argc, char **argv) {
    Model *m = xalloc(1, sizeof *m);
    Config *c = &m->cfg;
    const int NL = getenv("KATANA_ATT_LAYERS") ? atoi(getenv("KATANA_ATT_LAYERS")) : 10, q8 = getenv("KATANA_KV8") && atoi(getenv("KATANA_KV8"));
    int P[16], np = 0, mx = 0;
    for (int i = 0; i < argc && np < 16; i++) P[np++] = atoi(argv[i]);
    if (!np) { const int d[5] = { 1024, 4096, 8192, 16384, 32768 }; np = 5; memcpy(P, d, sizeof d); }
    for (int i = 0; i < np; i++) if (P[i] > mx) mx = P[i];
    c->n_heads = 16; c->n_kv = 2; c->hd = 256; c->ctx = mx + 128; c->kq_mul = 1.0f;
    const size_t kvd = 512, n = (size_t)NL * c->ctx * kvd;
    const int qd = 4096;
    if (q8) { m->kq8 = xalloc(n / 32, 34); m->vq8 = xalloc(n / 32, 34); } else { m->kh = xalloc(n, 2); m->vh = xalloc(n, 2); }
    Layer *L = xalloc(NL, sizeof *L);
    uint32_t r = 12345;
    float row[512];
    for (size_t i = 0; i < n; i += 512) for (int w = 0; w < 2; w++) {
        for (int k = 0; k < 512; k++) { r = r * 1664525u + 1013904223u; row[k] = ((int)(r >> 16) - 32768) * (w ? 1e-4f : 2e-5f); }
        if (q8) { q8_put((w ? m->vq8 : m->kq8) + i / 32 * 34, row, 512); continue; }
        uint16_t *d = (w ? m->vh : m->kh) + i;
#ifdef __AVX2__
        for (int k = 0; k < 512; k += 8) _mm_storeu_si128((__m128i *)(d + k), _mm256_cvtps_ph(_mm256_loadu_ps(row + k), 0));
#else
        /* porte ARM, 2026-11-27: sem SIMD */
        for (int k = 0; k < 512; k++) d[k] = f32_to_f16(row[k]);
#endif
    }
    for (int l = 0; l < NL; l++) L[l].kv_off = (size_t)l * c->ctx * kvd;
    float *Q = xalloc((size_t)64 * qd, 4), *G = xalloc((size_t)64 * 2 * qd, 4), *A = xalloc((size_t)64 * qd, 4), *B = xalloc((size_t)64 * qd, 4);
    for (size_t i = 0; i < (size_t)64 * qd; i++) { r = r * 1664525u + 1013904223u; Q[i] = ((int)(r >> 16) - 32768) * 3e-5f; }
    for (size_t i = 0; i < (size_t)128 * qd; i++) { r = r * 1664525u + 1013904223u; G[i] = ((int)(r >> 16) - 32768) * 1e-4f; }
    printf("threads %d, cache %s, 10 camadas; ms por token%s (menor de %d voltas alternadas; em parenteses a mediana)\n", pool.n, q8 ? "Q8_0" : "fp16", pool.n > 1 ? "" : ", tempo de processador", 9);
    printf("%8s | %20s %20s %6s | %20s %20s %6s | %s\n", "posicao", "1 token, antes", "agora", "x", "lote, antes (16)", "agora (64)", "x", "igual");
    int bad = 0;
    for (int ip = 0; ip < np; ip++) {
        const int pos = P[ip] - 1, pn = P[ip] - 64 > 0 ? P[ip] - 64 : 0;
        double t[4][9]; int eq1 = 1, eqn = 1; double dm = 0, am = 0;
        for (int rep = 0; rep < 9; rep++) {
            double t0;
            t0 = ab_now(); for (int x = 0; x < 4; x++) for (int l = 0; l < NL; l++) ab_old_1(m, &L[l], pos, Q + (size_t)x * qd, G + (size_t)x * 2 * qd, A); t[0][rep] = (ab_now() - t0) * 250;
            t0 = ab_now(); for (int x = 0; x < 4; x++) for (int l = 0; l < NL; l++) qn_attend(m, &L[l], pos, 1, Q + (size_t)x * qd, G + (size_t)x * 2 * qd, B); t[1][rep] = (ab_now() - t0) * 250;
            eq1 &= !memcmp(A, B, (size_t)qd * 4); for (int i = 0; i < qd; i++) { const double f = fabs((double)A[i] - B[i]), g = fabs((double)A[i]); if (f > dm) dm = f; if (g > am) am = g; }
            t0 = ab_now(); for (int l = 0; l < NL; l++) for (int b = 0; b < 64; b += 16) ab_old_n(m, &L[l], pn + b, 16, Q + (size_t)b * qd, G + (size_t)b * 2 * qd, A + (size_t)b * qd); t[2][rep] = (ab_now() - t0) * 1e3 / 64;
            t0 = ab_now(); for (int l = 0; l < NL; l++) qn_attend(m, &L[l], pn, 64, Q, G, B); t[3][rep] = (ab_now() - t0) * 1e3 / 64;
            eqn &= !memcmp(A, B, (size_t)64 * qd * 4); for (int i = 0; i < 64 * qd; i++) { const double f = fabs((double)A[i] - B[i]); if (f > dm) dm = f; }
        }
        for (int k = 0; k < 4; k++) for (int i = 1; i < 9; i++) for (int j = i; j && t[k][j] < t[k][j - 1]; j--) { const double s = t[k][j]; t[k][j] = t[k][j - 1]; t[k][j - 1] = s; }
        printf("%8d | %10.3f (%7.3f) %10.3f (%7.3f) %6.1f | %10.3f (%7.3f) %10.3f (%7.3f) %6.1f | %s %s (maior diferenca %.1e de %.1e)\n", P[ip], t[0][0], t[0][4], t[1][0], t[1][4], t[0][0] / t[1][0],
               t[2][0], t[2][4], t[3][0], t[3][4], t[2][0] / t[3][0], eq1 ? "sim" : q8 ? "nao (Q8)" : "nao", eqn ? "sim" : "nao", dm, am);
        fflush(stdout);
        bad += dm > 2e-5 * am;
    }
    return bad != 0;
}
