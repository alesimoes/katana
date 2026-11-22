/* ---------- qwen35moe: etapas de uma camada ----------
   Cada etapa le e escreve buffers, na mesma sequencia que um grafo na GPU vai seguir:
     dn_proj  : xb -> qkv [C], z [inner], alpha e beta [nv]                (projecoes; em lote no prompt)
     dn_conv  : qkv -> qkv  convolucao causal + SiLU, normas L2 de q e k   (estado da convolucao)
     dn_recur : q, k, v, alpha, beta -> o [inner]                          (recorrencia; estado S)
     dn_gate  : o, z -> o   norma RMS por cabeca vezes SiLU(z)
     qn_core  : q+portao, k, v -> saida da atencao com portao [nh*hd]       (cache KV)
     ffn_moe  : m->xb -> saida do feed-forward (roteados + compartilhado com portao sigmoide)
   Estado do DeltaNet (m->ssm, contiguo, pronto para ir inteiro a VRAM): por camada linear,
   S [nv][dk][dv] seguido das K-1 entradas anteriores da convolucao [K-1][C]. */
static size_t dn_channels(const Config *c) { return 2 * (size_t)c->ssm_groups * c->ssm_state + c->ssm_inner; }
static size_t dn_state_floats(const Config *c) { return (size_t)c->ssm_state * c->ssm_inner + (size_t)(c->ssm_conv - 1) * dn_channels(c); }
static float *dn_S(Model *m, const Layer *L) { return m->ssm + (size_t)L->lin_i * dn_state_floats(&m->cfg); }
static float *dn_CV(Model *m, const Layer *L) { return dn_S(m, L) + (size_t)m->cfg.ssm_state * m->cfg.ssm_inner; }
static void dn_reset(Model *m, const Layer *L) { memset(dn_S(m, L), 0, dn_state_floats(&m->cfg) * sizeof(float)); }

/* projecoes de um token (xb ja normalizado) */
static void dn_proj(Model *m, const Layer *L, const float *xb, float *qkv, float *z, float *al, float *be) {
    const Config *c = &m->cfg;
    const int d = c->dim;
    matvec(qkv, &L->wqkv, xb, (int)dn_channels(c), d);
    matvec(z, &L->wz, xb, c->ssm_inner, d);
    matvec(al, &L->walpha, xb, c->ssm_rank, d);
    matvec(be, &L->wbeta, xb, c->ssm_rank, d);
}

/* convolucao causal por canal (janela K; as K-1 entradas anteriores ficam em cv) e SiLU; depois
   q e k normalizados (L2) por grupo e q escalado por 1/sqrt(dk) */
static void dn_conv(const Config *c, const Layer *L, float *cv, float *qkv) {
    const int K = c->ssm_conv, C = (int)dn_channels(c), dk = c->ssm_state, ng = c->ssm_groups;
    for (int ch = 0; ch < C; ch++) {
        const float *w = L->conv_w + (size_t)ch * K, x = qkv[ch];
        float y = w[K - 1] * x;
        for (int t = 0; t < K - 1; t++) y += w[t] * cv[(size_t)t * C + ch];
        for (int t = 0; t < K - 2; t++) cv[(size_t)t * C + ch] = cv[(size_t)(t + 1) * C + ch];
        cv[(size_t)(K - 2) * C + ch] = x;
        qkv[ch] = y / (1.0f + expf(-y));
    }
    for (int g = 0; g < 2 * ng; g++) {
        float *p = qkv + (size_t)g * dk;
        const float inv = 1.0f / sqrtf(dot(p, p, dk) + c->eps) * (g < ng ? 1.0f / sqrtf((float)dk) : 1.0f);
        for (int i = 0; i < dk; i++) p[i] *= inv;
    }
}

/* Recorrencia do Gated DeltaNet (um token). Cada cabeca de v tem S [dk][dv]:
     S = exp(g) S;  delta = beta (v - S^T k);  S += k delta^T;  o = S^T q
   com g = ssm_a * softplus(alpha + dt_bias) (ssm_a ja vem negativo) e beta = sigmoide. A cabeca
   v h usa o grupo q/k h % grupos (ordem das cabecas no arquivo; KATANA_DN_TILE=0 usa h / (nv/grupos)).
   Este laco e o ponto a trocar pela forma em blocos (varios tokens por vez) no prompt. */
typedef struct { const Config *c; const Layer *L; float *S; const float *q, *k, *v, *al, *be; float *o; } DnJob;
static int dn_tile = -1;
static void dn_job(void *cx, int h0, int h1) {
    const DnJob *j = cx;
    const Config *c = j->c;
    const int dk = c->ssm_state, nv = c->ssm_rank, dv = c->ssm_inner / nv, ng = c->ssm_groups;
    float kv[512];
    for (int h = h0; h < h1; h++) {
        const int kg = dn_tile ? h % ng : h / (nv / ng);
        const float *q = j->q + (size_t)kg * dk, *k = j->k + (size_t)kg * dk, *v = j->v + (size_t)h * dv;
        float *S = j->S + (size_t)h * dk * dv, *o = j->o + (size_t)h * dv;
        const float a = j->al[h] + j->L->dt_bias[h];
        const float sp = a > 20.0f ? a : log1pf(expf(a));
        const float decay = expf(j->L->ssm_a[h] * sp), beta = 1.0f / (1.0f + expf(-j->be[h]));
        for (int jj = 0; jj < dv; jj++) kv[jj] = 0.0f;
        for (int i = 0; i < dk; i++) {
            float *Si = S + (size_t)i * dv;
            const float ki = k[i];
            for (int jj = 0; jj < dv; jj++) { Si[jj] *= decay; kv[jj] += Si[jj] * ki; }
        }
        for (int jj = 0; jj < dv; jj++) { kv[jj] = (v[jj] - kv[jj]) * beta; o[jj] = 0.0f; }
        for (int i = 0; i < dk; i++) {
            float *Si = S + (size_t)i * dv;
            const float ki = k[i], qi = q[i];
            for (int jj = 0; jj < dv; jj++) { Si[jj] += ki * kv[jj]; o[jj] += Si[jj] * qi; }
        }
    }
}
static void dn_recur(Model *m, const Layer *L, const float *qkv, const float *al, const float *be, float *o) {
    const Config *c = &m->cfg;
    const int kd = c->ssm_groups * c->ssm_state;
    if (dn_tile < 0) dn_tile = getenv("KATANA_DN_TILE") ? atoi(getenv("KATANA_DN_TILE")) : 1;
    if (c->ssm_inner / c->ssm_rank > 512) die("DeltaNet: cabeca v larga demais");
    DnJob job = { c, L, dn_S(m, L), qkv, qkv + kd, qkv + 2 * kd, al, be, o };
    pool_for(dn_job, &job, c->ssm_rank, 1);
}

/* norma RMS de cada cabeca (ssm_norm) com portao SiLU(z) */
static void dn_gate(const Config *c, const Layer *L, float *o, const float *z) {
    const int dv = c->ssm_inner / c->ssm_rank;
    for (int h = 0; h < c->ssm_rank; h++) {
        float *p = o + (size_t)h * dv;
        const float *zh = z + (size_t)h * dv;
        rmsnorm(p, p, L->ssm_norm, dv, c->eps);
        for (int i = 0; i < dv; i++) p[i] *= zh[i] / (1.0f + expf(-zh[i]));
    }
}

/* atencao com portao de um token: qg traz, por cabeca, q e o portao; norma RMS por cabeca em q e k,
   RoPE parcial (primeiras rope_dim dimensoes, em metades), k e v no cache e saida de cada cabeca
   vezes sigmoide(portao). out [nh*hd] */
/* atencao com portao de um token, em pedacos de posicoes (flash-decoding): cada trabalho pega uma cabeca kv e
   um pedaco de posicoes e atende as `group` cabecas q dela juntas (cada linha de K/V sai da RAM uma vez so).
   Softmax online em blocos de 64; os parciais (maximo, soma, saida) de cada pedaco se juntam no fim. A memoria
   de trabalho tem tamanho fixo (QN_MAXCH pedacos), nao cresce com o contexto. Cache fp32, fp16 (m->kh/m->vh) ou Q8_0 (m->kq8/m->vq8). */
#define QN_MAXCH 256
#define QN_MAXG 16
typedef struct { Model *m; const Layer *L; const float *q; float *part; int pos, hd, kvd, group, nch, chl; float scale; } QnAttJob;
static float dot_h(const float *a, const uint16_t *b, int n) {
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    int j = 0;
    for (; j + 16 <= n; j += 16) {
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + j), _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(b + j))), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + j + 8), _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(b + j + 8))), acc1);
    }
    acc0 = _mm256_add_ps(acc0, acc1);
    __m128 lo = _mm_add_ps(_mm256_castps256_ps128(acc0), _mm256_extractf128_ps(acc0, 1));
    lo = _mm_hadd_ps(lo, lo); lo = _mm_hadd_ps(lo, lo);
    float s = _mm_cvtss_f32(lo);
    for (; j < n; j++) s += a[j] * _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(b[j])));
    return s;
}
static void qn_att_job(void *cx, int j0, int j1) {
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
                    continue;
                }
                for (int k = 0; k < hd; k += 8) {
                    const __m256 vv = m->vh ? _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(m->vh + r + k))) : _mm256_loadu_ps(m->vcache + r + k);
                    for (int u = 0; u < G; u++)
                        _mm256_storeu_ps(o[u] + k, _mm256_fmadd_ps(_mm256_set1_ps(sc[u][i]), vv, _mm256_loadu_ps(o[u] + k)));
                }
            }
        }
        for (int u = 0; u < G; u++) {   /* parcial: [maximo, soma, saida] por (cabeca q, pedaco) */
            float *pp = j->part + ((size_t)(g * G + u) * j->nch + ch) * (hd + 2);
            pp[0] = mx[u]; pp[1] = sum[u];
            memcpy(pp + 2, o[u], hd * sizeof(float));
        }
    }
}

static void qn_core(Model *m, const Layer *L, int l, int pos, const float *qg, float *k, const float *v, float *out) {
    const Config *c = &m->cfg;
    const int nh = c->n_heads, hd = c->hd, kvd = c->n_kv * hd, ctx = c->ctx;
    const int group = nh / c->n_kv, nr = c->rope_dim > 0 && c->rope_dim < hd ? c->rope_dim : hd, half = nr / 2;
    const float scale = 1.0f / sqrtf((float)hd);
    float *q = m->q;
    for (int h = 0; h < nh; h++) rmsnorm(q + h * hd, qg + (size_t)h * 2 * hd, L->q_norm, hd, c->eps);
    for (int h = 0; h < c->n_kv; h++) rmsnorm(k + h * hd, k + h * hd, L->k_norm, hd, c->eps);
    for (int i = 0; i < half; i++) {
        const float ang = pos * m->rope_freq[i], cs = cosf(ang), sn = sinf(ang);
        for (int h = 0; h < nh; h++) {
            float *p = q + h * hd + i, a = p[0], b = p[half];
            p[0] = a * cs - b * sn; p[half] = a * sn + b * cs;
        }
        for (int h = 0; h < c->n_kv; h++) {
            float *p = k + h * hd + i, a = p[0], b = p[half];
            p[0] = a * cs - b * sn; p[half] = a * sn + b * cs;
        }
    }
    const size_t koff = L->kv_off + (size_t)pos * kvd;
    if (m->kq8) { q8_put(m->kq8 + koff / 32 * 34, k, kvd); q8_put(m->vq8 + koff / 32 * 34, v, kvd); }
    else if (m->kh) {
        for (int i = 0; i < kvd; i += 8) {
            _mm_storeu_si128((__m128i *)(m->kh + koff + i), _mm256_cvtps_ph(_mm256_loadu_ps(k + i), 0));
            _mm_storeu_si128((__m128i *)(m->vh + koff + i), _mm256_cvtps_ph(_mm256_loadu_ps(v + i), 0));
        }
    } else {
        memcpy(m->kcache + koff, k, kvd * sizeof(float));
        memcpy(m->vcache + koff, v, kvd * sizeof(float));
    }
    if (hd > 512 || hd % 8 || group > QN_MAXG) die("qn_core: hd ou grupo fora do suportado");
    static float *part;
    if (!part) part = xalloc((size_t)nh * QN_MAXCH * (hd + 2), sizeof(float));
    const int n = pos + 1;
    int chl = 512;      /* pedaco minimo; cresce para caber em QN_MAXCH pedacos */
    while ((n + chl - 1) / chl > QN_MAXCH) chl *= 2;
    const int nch = (n + chl - 1) / chl;
    QnAttJob job = { m, L, q, part, pos, hd, kvd, group, nch, chl, scale * c->kq_mul };
    pool_for(qn_att_job, &job, c->n_kv * nch, 1);
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

/* atencao com portao de B tokens seguidos (prompt na CPU): todas as consultas do lote contra cada pedaco de K/V.
   Trabalho = (cabeca kv, pedaco de posicoes, faixa de tokens): cada linha de K/V sai da RAM uma vez por faixa e serve
   as group x tokens consultas dela. Mesmos pedacos, blocos de 64 e ordem de soma do qn_core por consulta: o resultado
   e igual bit a bit ao token a token (enquanto o tamanho de pedaco for o mesmo para todo o lote). */
typedef struct { Model *m; const Layer *L; const float *q; float *part; int pos0, B, hd, kvd, group, nch, chl, nsp, tps, qd; float scale; } QnAttNJob;
static void qn_att_n_job(void *cx, int j0, int j1) {
    const QnAttNJob *j = cx;
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
                    for (int k = 0; k < hd; k += 8) {
                        const __m256 vv = m->vq8 ? _mm256_loadu_ps(kr + k) : m->vh ? _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(m->vh + r + k))) : _mm256_loadu_ps(m->vcache + r + k);
                        for (int u = 0; u < G; u++) {
                            const int qi = (t - t0q) * G + u;
                            float *o = ob + (size_t)qi * 512;
                            _mm256_storeu_ps(o + k, _mm256_fmadd_ps(_mm256_set1_ps(scb[(size_t)qi * 64 + i]), vv, _mm256_loadu_ps(o + k)));
                        }
                    }
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

/* B tokens a partir de pos0: P1 [B][2*qd] (q+portao), KK/VV [B][kvd] (mudam: norma e RoPE), O [B][qd] */
static void qn_core_n(Model *m, const Layer *L, int l, int pos0, int B, const float *P1, float *KK, const float *VV, float *O) {
    const Config *c = &m->cfg;
    const int nh = c->n_heads, hd = c->hd, kvd = c->n_kv * hd, qd = nh * hd;
    const int group = nh / c->n_kv, nr = c->rope_dim > 0 && c->rope_dim < hd ? c->rope_dim : hd, half = nr / 2;
    const float scale = 1.0f / sqrtf((float)hd);
    (void)l;
    if (hd > 512 || hd % 8 || group > QN_MAXG || B > GPU_BMAX) die("qn_core_n: hd, grupo ou lote fora do suportado");
    static float *Q, *part;
    if (!Q) { Q = xalloc((size_t)GPU_BMAX * qd, sizeof(float)); part = xalloc((size_t)GPU_BMAX * nh * QN_MAXCH * (hd + 2), sizeof(float)); }
    for (int t = 0; t < B; t++) {       /* norma, RoPE e cache, como no qn_core */
        const int pos = pos0 + t;
        float *q = Q + (size_t)t * qd, *k = KK + (size_t)t * kvd;
        const float *qg = P1 + (size_t)t * 2 * qd, *v = VV + (size_t)t * kvd;
        for (int h = 0; h < nh; h++) rmsnorm(q + h * hd, qg + (size_t)h * 2 * hd, L->q_norm, hd, c->eps);
        for (int h = 0; h < c->n_kv; h++) rmsnorm(k + h * hd, k + h * hd, L->k_norm, hd, c->eps);
        for (int i = 0; i < half; i++) {
            const float ang = pos * m->rope_freq[i], cs = cosf(ang), sn = sinf(ang);
            for (int h = 0; h < nh; h++) {
                float *p = q + h * hd + i, a = p[0], b = p[half];
                p[0] = a * cs - b * sn; p[half] = a * sn + b * cs;
            }
            for (int h = 0; h < c->n_kv; h++) {
                float *p = k + h * hd + i, a = p[0], b = p[half];
                p[0] = a * cs - b * sn; p[half] = a * sn + b * cs;
            }
        }
        const size_t koff = L->kv_off + (size_t)pos * kvd;
        if (m->kq8) { q8_put(m->kq8 + koff / 32 * 34, k, kvd); q8_put(m->vq8 + koff / 32 * 34, v, kvd); }
        else if (m->kh) {
            for (int i = 0; i < kvd; i += 8) {
                _mm_storeu_si128((__m128i *)(m->kh + koff + i), _mm256_cvtps_ph(_mm256_loadu_ps(k + i), 0));
                _mm_storeu_si128((__m128i *)(m->vh + koff + i), _mm256_cvtps_ph(_mm256_loadu_ps(v + i), 0));
            }
        } else {
            memcpy(m->kcache + koff, k, kvd * sizeof(float));
            memcpy(m->vcache + koff, v, kvd * sizeof(float));
        }
    }
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
    QnAttNJob job = { m, L, Q, part, pos0, B, hd, kvd, group, nch, chl, nsp, tps, qd, scale * c->kq_mul };
    pool_for(qn_att_n_job, &job, c->n_kv * nch * nsp, 1);
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

/* feed-forward MoE de um token sobre m->xb (ja normalizado): out = roteados + portao * compartilhado */
static void ffn_moe(Model *m, const Layer *L, int l, float *out) {
    const Config *c = &m->cfg;
    const int d = c->dim, K = c->n_used, Hs = c->n_shared * c->hidden;
    static float *outs;
    if (!outs) outs = xalloc((size_t)K * d, sizeof(float));
    if (gx.on && gpu.mv_host && !gpu.force_cpu && gx_host_layer(l)) {
        memset(out, 0, d * sizeof(float));
        host_moe_ffn(m, L, l, out);
        return;
    }
    {
        moe_route(m, L, l);
        moe_cpu_experts(m, L, moe.sel, K, outs);
        memset(out, 0, d * sizeof(float));
        for (int k = 0; k < K; k++) {
            const float w = moe.w[k], *o = outs + (size_t)k * d;
            for (int i = 0; i < d; i++) out[i] += w * o[i];
        }
    }
    if (Hs > 0) {
        matvec(m->hb, &L->sg, m->xb, Hs, d);
        matvec(m->hb2, &L->su, m->xb, Hs, d);
        for (int i = 0; i < Hs; i++) { const float g = m->hb[i]; m->hb[i] = g / (1.0f + expf(-g)) * m->hb2[i]; }
        matvec(m->xb2, &L->sd, m->hb, d, Hs);
        const float sw = shexp_gate_w(m, L);
        for (int i = 0; i < d; i++) out[i] += sw * m->xb2[i];
    }
}

/* uma camada do qwen35moe para o token em pos, sobre m->x */
static void qn_layer(Model *m, const Layer *L, int l, int pos) {
    const Config *c = &m->cfg;
    const int d = c->dim, nh = c->n_heads, hd = c->hd, qd = nh * hd, kvd = c->n_kv * hd;
    static float *qkv, *z, *o, *qg, al[1024], be[1024];
    if (!qkv) {
        qkv = xalloc(dn_channels(c), sizeof(float)); z = xalloc(c->ssm_inner, sizeof(float));
        o = xalloc(c->ssm_inner > qd ? c->ssm_inner : qd, sizeof(float)); qg = xalloc(2 * (size_t)qd, sizeof(float));
        if (c->ssm_rank > 1024) die("DeltaNet: cabecas demais");
    }
    rmsnorm(m->xb, m->x, L->attn_norm, d, c->eps);
    if (L->lin) {
        if (pos == 0) dn_reset(m, L);      /* conversa nova */
        dn_proj(m, L, m->xb, qkv, z, al, be);
        dn_conv(c, L, dn_CV(m, L), qkv);
        dn_recur(m, L, qkv, al, be, o);
        dn_gate(c, L, o, z);
        matvec(m->xb2, &L->wssm_out, o, d, c->ssm_inner);
    } else {
        matvec(qg, &L->wq, m->xb, 2 * qd, d);
        matvec(m->k, &L->wk, m->xb, kvd, d);
        matvec(m->v, &L->wv, m->xb, kvd, d);
        qn_core(m, L, l, pos, qg, m->k, m->v, o);
        matvec(m->xb2, &L->wo, o, d, qd);
    }
    for (int i = 0; i < d; i++) m->x[i] += m->xb2[i];
    if (dbg_on && cpu_exact && pos < 64) { memcpy(dbg_x + ((size_t)pos * c->n_layers + l) * d, m->x, d * 4); dbg_pos = pos; }
    rmsnorm(m->xb, m->x, L->ffn_norm, d, c->eps);
    if (layer_tap) layer_tap(l, pos, m->xb);
    if (!c->n_expert) {        /* qwen35 denso: feed-forward SwiGLU comum */
        const int H = c->hidden;
        matvec(m->hb, &L->w_gate, m->xb, H, d);
        matvec(m->hb2, &L->w_up, m->xb, H, d);
        for (int i = 0; i < H; i++) { const float g = m->hb[i]; m->hb[i] = g / (1.0f + expf(-g)) * m->hb2[i]; }
        matvec(m->xb2, &L->w_down, m->hb, d, H);
        for (int i = 0; i < d; i++) m->x[i] += m->xb2[i];
        return;
    }
    ffn_moe(m, L, l, o);       /* o: saida do feed-forward (m->xb2 e rascunho do compartilhado) */
    for (int i = 0; i < d; i++) m->x[i] += o[i];
}

/* ---------- qwen35moe em Vulkan: grafo completo (DeltaNet, atencao com portao, MoE) ----------
   Pesos densos sobem pelo mla_gpu_upload (os mesmos do modo hibrido); alpha e beta de cada camada DeltaNet viram
   uma matriz float so [a (NV) | b (NV)]. Estado do DeltaNet na VRAM: S [NV][128][128] e anel da convolucao [4][CH]
   por camada linear; a posicao 0 dispensa zerar (os kernels tratam estado e anel como zero).
   Um token: norma, projecoes, dn_step (ou att1f com q/portao intercalados, RoPE parcial e portao na saida), saida
   com residual, norma, e o MoE como no vk_forward_moe (roteador na CPU, especialistas quentes nas vagas, frios na
   CPU, compartilhado com portao na GPU).
   Lote: produtos em mm.comp, DeltaNet em blocos de 64 (dn_prep, dn_cstate, dn_chunk, dn_state, dn_norm), atencao
   com qkv_post + att_flash + qn_gate. MoE do lote: o residual desce para a CPU (norma e roteamento exatos); na GPU
   o compartilhado e cada especialista quente rodam sobre as colunas dos seus tokens (moe_gath, mm glu e down com
   a vaga por deslocamento, moe_scat soma pesada); os frios rodam na CPU enquanto isso e sobem somados. */
static struct {
    int on, NK, NV, CH, nrot, kmap, dim, P, dnstep, blk, cpuatt, pos1, r1, moedn;   /* r1: roteador de um token na GPU */
    float *hq, *hk, *hv, *ho, *hT;          /* atencao na CPU (cpuatt): q/portao, k, v e saida por token */
    Mat *wab;                               /* por camada linear: [alpha | beta] float */
    KvkBuf qkv, z, ab, o, qg;               /* um token */
    KvkBuf *cw, *pr, *ring, *st;            /* por camada linear */
    KvkPipe step, prep, cst, chunk, state, norm, gate, gath, scat, rms32, seq, gath32, gate32, flash32;
    KvkBuf xf, bqkv, bz, bab, qh, kh, vh, bg, ub, wb, qm, qgh, kd, gl, bo, bqg, xg, hg, ys, yc, meta;   /* lote */
    float *xn, *xc, *outs, *col, *sw; uint32_t *mt;
    int *cnt, *ofs, *pv, *pb, *ce, *cb; float *pw, *cw_;
    double t_rd, t_route, t_gpu, t_cpu, t_up; long n_hot, n_cold;    /* perfil do lote (KATANA_QN_PROF=1) */
} vkq;

/* MoE do lote do qwen35moe pelo vk_moe_n comum (norma e roteador na GPU, frios usados por varios tokens do lote em
   vagas temporarias): aloca o vkbm. Se faltar memoria, fica o vkq_moe_n (residual desce, frios todos na CPU). */
static void vkq_moen_build(Model *m, size_t P) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, E = c->n_expert, K = c->n_used, H = c->hidden, NL = c->n_layers;
    const size_t Hs = (size_t)c->n_shared * H, Hm = H > Hs ? H : Hs;
    int fail = 0;
    const size_t used0 = gpu.used;
    memset(&vkbm, 0, sizeof vkbm);
    vkb.hb = vk_wk(Hm * P * 4, &fail);
    vkbm.xf = vk_wk(d * P * 4, &fail); vkbm.xg = vk_wk(d * P * 4, &fail);
    vkbm.ys = vk_wk(d * P * 4, &fail); vkbm.yc = vk_wk(d * P * 4, &fail);
    vkbm.lg = vk_wk((E * P + 128) * 4, &fail); vkbm.meta = vk_wk(2 * P * (K + 2) * 4, &fail);
    vkbm.rms32 = kvk_pipe_create(rms_wg32_spv, sizeof rms_wg32_spv, 3, 12, NULL, NULL, 0);
    vkbm.gath32 = kvk_pipe_create(moe_gath32_spv, sizeof moe_gath32_spv, 3, 16, NULL, NULL, 0);
    vkbm.scat = kvk_pipe_create(moe_scat_spv, sizeof moe_scat_spv, 3, 16, NULL, NULL, 0);
    if (!vkbm.rms32.p || !vkbm.gath32.p || !vkbm.scat.p) fail = 1;
    /* roteador na GPU (float32, grupos de 32 linhas) */
    if (!fail && !vkm.rw && !(getenv("KATANA_VK_ROUTER") && atoi(getenv("KATANA_VK_ROUTER")) == 0)) {
        float *wr = xalloc(E * d, sizeof(float)), *row = xalloc(d, sizeof(float));
        vkm.rw = xalloc(NL, sizeof(KvkBuf));
        for (size_t l = 0; l < NL && !fail; l++) {
            if (!m->layers[l].eg) continue;
            for (size_t e = 0; e < E; e++) {
                mat_row(row, &m->layers[l].router, (int)e, (int)d);
                for (size_t i = 0; i < d; i++) wr[(e / 32) * d * 32 + i * 32 + e % 32] = row[i];
            }
            vkm.rw[l] = vk_vec(wr, E * d, &fail);
        }
        free(row); free(wr);
        if (!fail) vkm.rt = 1;
    }
    vkbm.T = getenv("KATANA_VK_COLDT") ? atoi(getenv("KATANA_VK_COLDT")) : 16;
    vkbm.cmin = getenv("KATANA_VK_COLDMIN") ? atoi(getenv("KATANA_VK_COLDMIN")) : 2;
    if (vkbm.T < 0) vkbm.T = 0;
    if (vkbm.T > 64) vkbm.T = 64;
    if (vkbm.T && !fail) {
        for (size_t l = 0; l < NL; l++) {
            const Layer *L = &m->layers[l];
            if (!L->eg) continue;
            const size_t bg = 2 * gpu_mat_bytes(&L->eg[0], H, d), bd = gpu_mat_bytes(&L->ed[0], d, H);
            if (bg > vkbm.tgb) vkbm.tgb = bg;
            if (bd > vkbm.tdb) vkbm.tdb = bd;
        }
        vkbm.tg = vk_wk(vkbm.T * vkbm.tgb, &fail); vkbm.td = vk_wk(vkbm.T * vkbm.tdb, &fail);
        vkbm.stage = xalloc(vkbm.tgb + vkbm.tdb, 1);
        vkbm.pf = xalloc(NL, sizeof(FILE *));
        if (!getenv("KATANA_VK_TMPTHR") || atoi(getenv("KATANA_VK_TMPTHR"))) vkbm.tstage = xalloc(vkbm.T * (vkbm.tgb + vkbm.tdb), 1);
    }
    if (fail || !kvk_wait()) {
        fprintf(stderr, "Vulkan: sem memoria para o MoE do lote do qwen35moe (%s): frios do lote na CPU\n", kvk_error());
        vkm.rt = 0; vkbm.on = 0;
        gpu.used = used0;
        return;
    }
    vkbm.xn = xalloc(d * P, sizeof(float)); vkbm.lgT = xalloc(E * P, sizeof(float)); vkbm.xc = xalloc(d * P, sizeof(float));
    vkbm.outs = xalloc((size_t)GPU_BMAX * K * d, sizeof(float)); vkbm.sw = xalloc(P, sizeof(float));
    vkbm.mt = xalloc(2 * P * (K + 2), sizeof(uint32_t));
    vkbm.ccnt = xalloc(E, sizeof(int)); vkbm.tmpe = xalloc(64, sizeof(int));
    vkbm.cnt = xalloc(E + 66, sizeof(int)); vkbm.ofs = xalloc(E + 66, sizeof(int));
    vkbm.pv = xalloc(P * K, sizeof(int)); vkbm.pb = xalloc(P * K, sizeof(int)); vkbm.pw = xalloc(P * K, sizeof(float));
    vkbm.ce = xalloc(P * K, sizeof(int)); vkbm.cb = xalloc(P * K, sizeof(int)); vkbm.cw = xalloc(P * K, sizeof(float));
    vkbm.prec = 1;
    vkbm.on = 1;
}

static int vkq_build(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, kvd = (size_t)c->n_kv * c->hd, qd = (size_t)c->n_heads * c->hd, NL = c->n_layers;
    const int NK = c->ssm_groups, NV = c->ssm_rank, D = c->ssm_state, K = c->n_used;
    const size_t CH = 2 * (size_t)NK * D + (size_t)NV * D, inner = c->ssm_inner, H = c->hidden, Hs = (size_t)c->n_shared * H;
    if (getenv("KATANA_VK_QN") && atoi(getenv("KATANA_VK_QN")) == 0) return 0;
    if (D != 128 || (int)inner != NV * 128 || c->ssm_conv != 4 || c->hd > 256 || c->hd % 32 || !c->attn_gated
        || c->n_heads % c->n_kv || c->n_heads / c->n_kv > 8 || !vkg.coop || !vkg.sg32 || c->n_used > 16 || d % 64) {
        fprintf(stderr, "Vulkan: qwen35moe com dimensoes fora do grafo (ou sem matriz cooperativa): modo hibrido\n");
        return 0;
    }
    for (size_t l = 0; l < NL; l++) {
        const Layer *L = &m->layers[l];
        const int ok = L->lin ? L->wqkv.gbuf && L->wz.gbuf && L->wssm_out.gbuf : L->wq.gbuf && L->wk.gbuf && L->wv.gbuf && L->wo.gbuf;
        if (!ok || !L->eg) { fprintf(stderr, "Vulkan: camada %d fora da VRAM: modo hibrido\n", (int)l); return 0; }
        const Mat *all[6] = { &L->wqkv, &L->wz, &L->wssm_out, &L->wq, &L->wk, &L->wo };
        for (int i = L->lin ? 0 : 3; i < (L->lin ? 3 : 6); i++) if (gpu_kind(all[i]) == T_Q2_K || all[i]->gs != 1) { fprintf(stderr, "Vulkan: formato sem produto em lote: modo hibrido\n"); return 0; }
    }
    if (!m->lm_head.gbuf) { fprintf(stderr, "Vulkan: vocabulario fora da VRAM: modo hibrido\n"); return 0; }
    vkq.NK = NK; vkq.NV = NV; vkq.CH = (int)CH; vkq.dim = (int)d;
    vkq.nrot = c->rope_dim > 0 && c->rope_dim < c->hd ? c->rope_dim : c->hd;
    vkq.kmap = getenv("KATANA_DN_TILE") ? atoi(getenv("KATANA_DN_TILE")) != 0 : 1;     /* mesmo mapa grupo -> cabeca da CPU */
    int fail = 0;
    /* alpha|beta de cada camada linear */
    vkq.wab = xalloc(m->n_lin, sizeof(Mat));
    for (size_t l = 0; l < NL && !fail; l++) {
        const Layer *L = &m->layers[l];
        if (!L->lin) continue;
        Mat *W = &vkq.wab[L->lin_i];
        W->f = xalloc((size_t)2 * NV * d, sizeof(float)); W->type = T_F32;
        for (int r = 0; r < NV; r++) { mat_row(W->f + (size_t)r * d, &L->walpha, r, (int)d); mat_row(W->f + (size_t)(NV + r) * d, &L->wbeta, r, (int)d); }
        char nm[32]; snprintf(nm, sizeof nm, "L%03d.ab", (int)l);
        gpu_upload(W, 2 * (size_t)NV, d, nm);
        if (!W->gbuf) fail = 1;
    }
    if (fail) { fprintf(stderr, "Vulkan: sem memoria para alpha/beta: modo hibrido\n"); return 0; }
    /* cache KV: so as camadas de atencao */
    int n_att = 0;
    for (size_t l = 0; l < NL; l++) n_att += !m->layers[l].lin;
    const size_t kv32 = (size_t)n_att * 2 * c->ctx * kvd * 4;
    vkg.kv16 = getenv("KATANA_KV16") ? atoi(getenv("KATANA_KV16")) != 0 : kv32 > ((size_t)256 << 20);
    gpu.kv16 = vkg.kv16;
    /* atencao completa na CPU (cache KV na RAM, qn_core/qn_core_n): o grafo deixa de depender do contexto e cabe
       com contextos longos; padrao quando o cache em 16 bits passaria de 512 MB. KATANA_QN_CPUATT=0/1 forca. */
    vkq.cpuatt = getenv("KATANA_QN_CPUATT") ? atoi(getenv("KATANA_QN_CPUATT")) != 0 : kv32 / 2 > ((size_t)512 << 20);
    if (vkq.cpuatt && !m->kcache && !m->kh && !m->kq8) vkq.cpuatt = 0;
    vkg.nl = c->n_layers;
    vkg.x = vk_wk(d * 4, &fail); vkg.xn = vk_wk((d + (size_t)c->n_expert) * 4, &fail);     /* logits do roteador depois da entrada */
    vkg.k = vk_wk(kvd * 4, &fail); vkg.v = vk_wk(kvd * 4, &fail);
    vkg.lg = vk_wk((c->vocab + 128) * 4, &fail);
    const int actx = vkq.cpuatt ? 128 : c->ctx;      /* com atencao na CPU os buffers da atencao na GPU ficam minimos */
    vkg.att = vk_wk((size_t)c->n_heads * actx * 4, &fail); vkg.meta = vk_wk(16, &fail); vkg.dummy = vk_wk(256, &fail);
    vkg.chunk = 128 * ((actx + 128 * 32 - 1) / (128 * 32));
    vkg.nchunk = (actx + vkg.chunk - 1) / vkg.chunk;
    vkg.part = vk_wk((size_t)c->n_heads * vkg.nchunk * (c->hd + 2) * 4, &fail);
    vkg.freq = vk_vec(m->rope_freq, c->hd / 2, &fail); vkg.onorm = vk_vec(m->out_norm, d, &fail);
    vkg.an = xalloc(NL, sizeof(KvkBuf)); vkg.fn = xalloc(NL, sizeof(KvkBuf));
    vkg.qn = xalloc(NL, sizeof(KvkBuf)); vkg.kn = xalloc(NL, sizeof(KvkBuf));
    vkg.kc = xalloc(NL, sizeof(KvkBuf)); vkg.vc = xalloc(NL, sizeof(KvkBuf));
    vkq.cw = xalloc(m->n_lin, sizeof(KvkBuf)); vkq.pr = xalloc(m->n_lin, sizeof(KvkBuf));
    vkq.ring = xalloc(m->n_lin, sizeof(KvkBuf)); vkq.st = xalloc(m->n_lin, sizeof(KvkBuf));
    const size_t kvb = (size_t)actx * kvd * (vkg.kv16 ? 2 : 4);
    float *prv = xalloc(2 * (size_t)NV + D, sizeof(float));
    for (size_t l = 0; l < NL && !fail; l++) {
        const Layer *L = &m->layers[l];
        vkg.an[l] = vk_vec(L->attn_norm, d, &fail); vkg.fn[l] = vk_vec(L->ffn_norm, d, &fail);
        if (L->lin) {
            const int li = L->lin_i;
            memcpy(prv, L->ssm_a, NV * sizeof(float)); memcpy(prv + NV, L->dt_bias, NV * sizeof(float));
            memcpy(prv + 2 * NV, L->ssm_norm, D * sizeof(float));
            vkq.cw[li] = vk_vec(L->conv_w, CH * 4, &fail); vkq.pr[li] = vk_vec(prv, 2 * (size_t)NV + D, &fail);
            vkq.ring[li] = vk_wk(4 * CH * 4, &fail); vkq.st[li] = vk_wk((size_t)NV * D * D * 4, &fail);
        } else {
            vkg.qn[l] = vk_vec(L->q_norm, c->hd, &fail); vkg.kn[l] = vk_vec(L->k_norm, c->hd, &fail);
            vkg.kc[l] = vk_wk(kvb, &fail); vkg.vc[l] = vk_wk(kvb, &fail);
        }
    }
    free(prv);
    vkq.qkv = vk_wk(CH * 4, &fail); vkq.z = vk_wk(inner * 4, &fail); vkq.ab = vk_wk((2 * (size_t)NV + 128) * 4, &fail);
    vkq.o = vk_wk((inner > qd ? inner : qd) * 4, &fail); vkq.qg = vk_wk(2 * qd * 4, &fail);
    /* lote */
    int P = getenv("KATANA_VK_PASS") ? atoi(getenv("KATANA_VK_PASS")) : VK_PASS_DEF;
    if (P > 512) P = 512;
    if (P < 64) P = 64;
    P = P / 64 * 64;
    const size_t Pp = (size_t)P, nch = Pp / 64, mx = d > qd ? (d > inner ? d : inner) : (qd > inner ? qd : inner);
    const size_t Hm = H > Hs ? H : Hs;
    vkb.x = vk_wk(d * Pp * 4, &fail); vkb.xh = vk_wk(mx * Pp * 2, &fail);
    vkb.q2 = vk_wk(qd * Pp * 4, &fail); vkb.k = vk_wk(kvd * Pp * 4, &fail); vkb.v = vk_wk(kvd * Pp * 4, &fail);
    vkq.bqkv = vk_wk(CH * Pp * 4, &fail); vkq.bz = vk_wk(inner * Pp * 4, &fail); vkq.bab = vk_wk(2 * NV * Pp * 4, &fail);
    vkq.qh = vk_wk((size_t)NK * D * Pp * 2, &fail); vkq.kh = vk_wk((size_t)NK * D * Pp * 2, &fail);
    vkq.vh = vk_wk((size_t)NV * D * Pp * 2, &fail); vkq.bg = vk_wk(2 * (size_t)NV * Pp * 4, &fail);
    vkq.ub = vk_wk((size_t)NV * D * Pp * 4, &fail); vkq.wb = vk_wk((size_t)NV * D * Pp * 2, &fail);
    vkq.qm = vk_wk((size_t)NV * nch * 64 * 64 * 2, &fail); vkq.qgh = vk_wk((size_t)NV * D * Pp * 2, &fail);
    vkq.kd = vk_wk((size_t)NV * D * Pp * 2, &fail); vkq.gl = vk_wk((size_t)NV * nch * 4, &fail);
    vkq.bo = vk_wk((size_t)NV * D * Pp * 4, &fail); vkq.bqg = vk_wk(2 * qd * Pp * 4, &fail);
    vkq.xg = vk_wk(d * Pp * 4, &fail); vkq.hg = vk_wk(Hm * Pp * 4, &fail);     /* fp32: produtos precisos */
    vkq.ys = vk_wk(d * Pp * 4, &fail); vkq.yc = vk_wk(d * Pp * 4, &fail);
    vkq.meta = vk_wk(2 * Pp * (K + 2) * 4, &fail);
    vkq.xf = vk_wk(d * Pp * 4, &fail);
    if (fail) { fprintf(stderr, "Vulkan: sem memoria para o grafo do qwen35moe (%s): modo hibrido\n", kvk_error()); return 0; }
    kvk_wait();
    const uint32_t id0 = 0, hdv = (uint32_t)c->hd, sid[3] = { 0, 1, 2 }, sv[3] = { (uint32_t)NK, (uint32_t)NV, 1 };
    const uint32_t fv[2] = { (uint32_t)c->hd, (uint32_t)(c->n_heads / c->n_kv) };
    vkg.rms = kvk_pipe_create(rms_wg_spv, sizeof rms_wg_spv, 3, 12, NULL, NULL, 0);
    vkg.att1f = vkg.kv16 ? kvk_pipe_create(att1f_kv16_spv, sizeof att1f_kv16_spv, 15, 64, &id0, &hdv, 1)
                         : kvk_pipe_create(att1f_kv32_spv, sizeof att1f_kv32_spv, 15, 64, &id0, &hdv, 1);
    vkg.att1r = kvk_pipe_create(att1r_spv, sizeof att1r_spv, 2, 8, &id0, &hdv, 1);
    vkb.rms16 = kvk_pipe_create(rms_wg16_spv, sizeof rms_wg16_spv, 3, 12, NULL, NULL, 0);
    vkb.qkv = vkg.kv16 ? kvk_pipe_create(qkv_post_kv16_spv, sizeof qkv_post_kv16_spv, 12, 48, NULL, NULL, 0)
                       : kvk_pipe_create(qkv_post_kv32_spv, sizeof qkv_post_kv32_spv, 12, 48, NULL, NULL, 0);
    vkb.gat = kvk_pipe_create(gather_spv, sizeof gather_spv, 2, 12, NULL, NULL, 0);
    vkb.flash_p = vkg.kv16 ? kvk_pipe_create(att_flash_kv16_spv, sizeof att_flash_kv16_spv, 4, 24, sid, fv, 2)
                           : kvk_pipe_create(att_flash_kv32_spv, sizeof att_flash_kv32_spv, 4, 24, sid, fv, 2);
    vkq.step = kvk_pipe_create(dn_step_spv, sizeof dn_step_spv, 9, 16, sid, sv, 2);
    vkq.prep = kvk_pipe_create(dn_prep_spv, sizeof dn_prep_spv, 9, 16, sid, sv, 2);
    vkq.cst = kvk_pipe_create(dn_cstate_spv, sizeof dn_cstate_spv, 2, 12, NULL, NULL, 0);
    vkq.chunk = kvk_pipe_create(dn_chunk_spv, sizeof dn_chunk_spv, 10, 8, sid, sv, 2);
    vkq.state = kvk_pipe_create(dn_state_spv, sizeof dn_state_spv, 8, 12, sid, sv, 3);
    vkq.norm = kvk_pipe_create(dn_norm_spv, sizeof dn_norm_spv, 4, 12, sid, sv, 2);
    vkq.rms32 = kvk_pipe_create(rms_wg32_spv, sizeof rms_wg32_spv, 3, 12, NULL, NULL, 0);
    vkq.gate32 = kvk_pipe_create(qn_gate32_spv, sizeof qn_gate32_spv, 2, 12, NULL, NULL, 0);
    vkq.flash32 = vkg.kv16 ? kvk_pipe_create(att_flash32_kv16_spv, sizeof att_flash32_kv16_spv, 4, 24, sid, fv, 2)
                           : kvk_pipe_create(att_flash32_kv32_spv, sizeof att_flash32_kv32_spv, 4, 24, sid, fv, 2);
    if (qd > inner) { fprintf(stderr, "qwen35moe: saida da atencao maior que a do DeltaNet\n"); return 0; }
    vkq.gath32 = kvk_pipe_create(moe_gath32_spv, sizeof moe_gath32_spv, 3, 16, NULL, NULL, 0);
    vkq.seq = kvk_pipe_create(dn_seq_spv, sizeof dn_seq_spv, 8, 20, sid, sv, 2);
    vkq.gate = kvk_pipe_create(qn_gate_spv, sizeof qn_gate_spv, 2, 12, NULL, NULL, 0);
    vkq.gath = kvk_pipe_create(moe_gath_spv, sizeof moe_gath_spv, 3, 16, NULL, NULL, 0);
    vkq.scat = kvk_pipe_create(moe_scat_spv, sizeof moe_scat_spv, 3, 16, NULL, NULL, 0);
    if (!vkg.rms.p || !vkg.att1f.p || !vkg.att1r.p || !vkb.rms16.p || !vkb.qkv.p || !vkb.gat.p || !vkb.flash_p.p || !vkq.step.p
        || !vkq.prep.p || !vkq.cst.p || !vkq.chunk.p || !vkq.state.p || !vkq.norm.p || !vkq.rms32.p || !vkq.seq.p || !vkq.gath32.p || !vkq.gate32.p || !vkq.flash32.p || !vkq.gate.p || !vkq.gath.p || !vkq.scat.p) {
        fprintf(stderr, "Vulkan: pipeline do qwen35moe falhou (%s): modo hibrido\n", kvk_error()); return 0;
    }
    /* host do lote */
    vkb.P = P; vkg.P = P; vkb.flash = 1; vkq.P = P;
    vkq.dnstep = getenv("KATANA_QN_DNSTEP") && atoi(getenv("KATANA_QN_DNSTEP")) != 0;
    vkq.moedn = !(getenv("KATANA_QN_MOEDN") && atoi(getenv("KATANA_QN_MOEDN")) == 0);      /* 0: MoE das camadas DeltaNet pelo vkq_moe_n (conferencia) */
    vkq.blk = getenv("KATANA_QN_CHUNK") && atoi(getenv("KATANA_QN_CHUNK")) != 0;
    vkb.xT = xalloc(d * Pp, sizeof(float));
    if (!(getenv("KATANA_QN_MOEN") && atoi(getenv("KATANA_QN_MOEN")) == 0)) vkq_moen_build(m, Pp);
    vkq.xn = xalloc(d * Pp, sizeof(float)); vkq.xc = xalloc(d * Pp, sizeof(float)); vkq.col = xalloc(d, sizeof(float));
    vkq.outs = xalloc((size_t)GPU_BMAX * K * d, sizeof(float)); vkq.sw = xalloc(Pp, sizeof(float));
    vkq.mt = xalloc(2 * Pp * (K + 2), sizeof(uint32_t));
    vkq.cnt = xalloc(c->n_expert + 1, sizeof(int)); vkq.ofs = xalloc(c->n_expert + 1, sizeof(int));
    vkq.pv = xalloc(Pp * K, sizeof(int)); vkq.pb = xalloc(Pp * K, sizeof(int)); vkq.pw = xalloc(Pp * K, sizeof(float));
    vkq.ce = xalloc(Pp * K, sizeof(int)); vkq.cb = xalloc(Pp * K, sizeof(int)); vkq.cw_ = xalloc(Pp * K, sizeof(float));
    if (vkq.cpuatt) {
        vkq.hq = xalloc(2 * qd * Pp, sizeof(float)); vkq.hk = xalloc(kvd * Pp, sizeof(float)); vkq.hv = xalloc(kvd * Pp, sizeof(float));
        vkq.ho = xalloc(qd * Pp, sizeof(float)); vkq.hT = xalloc(2 * qd * Pp, sizeof(float));
    }
    return 1;
}

static void vkq_setup(Model *m) {
    const Config *c = &m->cfg;
    if (!gpu.vk) return;
    if (!vkq_build(m)) return;
    if (!vk_moe_init(m)) { fprintf(stderr, "Vulkan: cache de especialistas indisponivel: modo hibrido\n"); return; }
    if (vkbm.on && vkm.rw) vkm.rt = 1;     /* roteador do lote na GPU */
    /* roteador de um token na GPU (KATANA_VK_GPUROUTER=1; padrao desligado: ganho medido ~1%, dentro do ruido): os
       logits saem junto com a entrada do feed-forward na mesma leitura por camada, sem o produto do roteador na CPU.
       Nao tira a leitura bloqueante: so 0,3% das camadas tem os 8 escolhidos todos nas vagas (17 de 256), entao a
       CPU quase sempre precisa da entrada para os frios */
    vkq.r1 = 0;
    if (vkm.rt && c->n_expert % 32 == 0 && getenv("KATANA_VK_GPUROUTER") && atoi(getenv("KATANA_VK_GPUROUTER")) != 0) {
        if (!vkm.rpipe.p) vkm.rpipe = kvk_pipe_create(moe_router_spv, sizeof moe_router_spv, 3, 8, NULL, NULL, 0);
        vkq.r1 = vkm.rpipe.p != 0;
    }
    gpu.mv_host = 0; vkg.on = 1; vkg.rec = 0; vkb.ok = 1; vkq.on = 1;
    fprintf(stderr, "GPU: Vulkan - grafo completo do qwen35moe (DeltaNet + atencao com portao + MoE, lote de ate %d), %.0f MB usados%s\n",
            vkb.P, gpu.used / 1048576.0, vkq.cpuatt ? ", atencao completa na CPU (cache KV na RAM)" : vkg.kv16 ? ", cache KV em 16 bits" : "");
    (void)c;
}

/* um token (vkg.x e meta ja escritos) */
static float *vk_qn_forward_1(Model *m, int want_logits) {
    const Config *c = &m->cfg;
    const int d = c->dim, hd = c->hd, nh = c->n_heads, kvd = c->n_kv * hd, qd = nh * hd, NV = vkq.NV, inner = c->ssm_inner;
    const struct { int n; float eps; int B; } rp = { d, c->eps, 1 };
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
        int ok = 1;
        const KvkBuf rb[3] = { vkg.x, vkg.an[l], vkg.xn };
        ok &= kvk_dispatch(vkg.rms, rb, 3, &rp, sizeof rp, 1, 1, 1);
        if (L->lin) {
            const int li = L->lin_i;
            ok &= vk_gmv_ex(&L->wqkv, vkg.xn, vkq.qkv, vkg.dummy, vkq.CH, d, 0, 0);
            ok &= vk_gmv_ex(&L->wz, vkg.xn, vkq.z, vkg.dummy, inner, d, 0, 0);
            ok &= vk_gmv_ex(&vkq.wab[li], vkg.xn, vkq.ab, vkg.dummy, 2 * NV, d, 0, 0);
            const KvkBuf sb[9] = { vkq.qkv, vkq.z, vkq.ab, vkq.cw[li], vkq.pr[li], vkq.ring[li], vkq.st[li], vkq.o, vkg.meta };
            const struct { int pos, kmap; float e1, e2; } stp = { 0, vkq.kmap, c->eps, c->eps };
            ok &= kvk_dispatch(vkq.step, sb, 9, &stp, sizeof stp, (uint32_t)NV, 1, 1);
            ok &= vk_gmv_ex(&L->wssm_out, vkq.o, vkg.x, vkg.dummy, d, inner, 1, 0);
        } else {
            ok &= vk_gmv_ex(&L->wq, vkg.xn, vkq.qg, vkg.dummy, 2 * qd, d, 0, 0);
            ok &= vk_gmv_ex(&L->wk, vkg.xn, vkg.k, vkg.dummy, kvd, d, 0, 0);
            ok &= vk_gmv_ex(&L->wv, vkg.xn, vkg.v, vkg.dummy, kvd, d, 0, 0);
            if (vkq.cpuatt) {     /* atencao na CPU: q/portao, k, v descem; a saida sobe para o produto de saida */
                ok &= kvk_buf_read(vkq.qg, 0, vkq.hq, (size_t)2 * qd * 4) && kvk_buf_read(vkg.k, 0, vkq.hk, (size_t)kvd * 4)
                      && kvk_buf_read(vkg.v, 0, vkq.hv, (size_t)kvd * 4);
                if (!ok) die("falha na execucao em Vulkan (qwen35moe, leitura da atencao)");
                qn_core(m, L, l, vkq_pos1, vkq.hq, vkq.hk, vkq.hv, vkq.ho);
                ok &= kvk_buf_write_q(vkq.o, 0, vkq.ho, (size_t)qd * 4);
                ok &= vk_gmv_ex(&L->wo, vkq.o, vkg.x, vkg.dummy, d, qd, 1, 0);
                goto qn1_ffn;
            }
            const KvkBuf ab[15] = { vkq.qg, vkg.k, vkg.v, vkg.dummy, vkg.dummy, vkg.dummy, vkg.kc[l], vkg.vc[l], vkg.att, vkq.o,
                                    vkg.freq, vkg.qn[l], vkg.kn[l], vkg.meta, vkg.part };
            const struct { int base, ctx, hd, kvd, group, neox, has_bias, norm, d; float scale, eps; int chunk, nchunk, nrot, qst, gate; } ap =
                { 0, c->ctx, hd, kvd, nh / c->n_kv, 1, 0, 2, qd, 1.0f / sqrtf((float)hd), c->eps, vkg.chunk, vkg.nchunk, vkq.nrot, 2 * hd, 1 };
            ok &= kvk_dispatch(vkg.att1f, ab, 15, &ap, sizeof ap, (uint32_t)nh, (uint32_t)vkg.nchunk, 1);
            if (vkg.nchunk > 1) {
                const KvkBuf pb[2] = { vkg.part, vkq.o };
                const struct { int hd, nchunk; } rp2 = { hd, vkg.nchunk };
                ok &= kvk_dispatch(vkg.att1r, pb, 2, &rp2, sizeof rp2, (uint32_t)nh, 1, 1);
            }
            ok &= vk_gmv_ex(&L->wo, vkq.o, vkg.x, vkg.dummy, d, qd, 1, 0);
        }
    qn1_ffn:;
        const KvkBuf rb2[3] = { vkg.x, vkg.fn[l], vkg.xn };
        ok &= kvk_dispatch(vkg.rms, rb2, 3, &rp, sizeof rp, 1, 1, 1);
        if (vkq.r1) {
            const KvkBuf rr[3] = { vkm.rw[l], vkg.xn, vkg.xn };
            const int rpc[2] = { d, d };
            ok &= kvk_dispatch(vkm.rpipe, rr, 3, rpc, sizeof rpc, (uint32_t)(c->n_expert / 32), 1, 1);
        }
        if (!ok) die("falha na execucao em Vulkan (qwen35moe)");
        const double tr0 = now_sec();
        if (!kvk_buf_read(vkg.xn, 0, gx.xn, (size_t)(d + (vkq.r1 ? c->n_expert : 0)) * sizeof(float))) die("falha na execucao em Vulkan (leitura)");
        gprof.t_rd += now_sec() - tr0;
        if (layer_tap) layer_tap(l, vkq_pos1, gx.xn);
        moe_lg_ext = vkq.r1 ? gx.xn + d : NULL;
        vk_moe_ffn(m, L, l);
        moe_lg_ext = NULL;
    }
    if (!want_logits) { if (!kvk_flush()) die("falha na execucao em Vulkan"); return NULL; }
    if (!vk_emit_head(m) || !kvk_buf_read(vkg.lg, 0, m->logits, (size_t)c->vocab * sizeof(float))) die("falha na execucao em Vulkan (cabeca)");
    return m->logits;
}

static int vkq_scat(KvkBuf y, int B, int n, int moff) {
    const KvkBuf b[3] = { vkb.x, y, vkq.meta };
    const int pc[4] = { vkq.dim, B, n, moff };
    return kvk_dispatch(vkq.scat, b, 3, pc, sizeof pc, (uint32_t)(((size_t)vkq.dim * n + 255) / 256), 1, 1);
}

/* MoE do lote na camada l (residual em vkb.x, B tokens) */
static void vkq_moe_n(Model *m, const Layer *L, int l, int B) {
    const Config *c = &m->cfg;
    const int d = c->dim, K = c->n_used, E = c->n_expert, H = c->hidden, Hs = c->n_shared * H;
    float *heat = gx.heat + (size_t)l * E;
    int *own = gx.owner + (size_t)l * gx.S;
    uint64_t *st = gx.stamp + (size_t)l * gx.S;
    /* residual para a CPU: norma e roteamento exatos */
    const double ta = now_sec();
    if (!kvk_buf_read(vkb.x, 0, vkb.xT, (size_t)d * B * sizeof(float))) die("falha na execucao em Vulkan (leitura do lote)");
    const double tb = now_sec();
    vkq.t_rd += tb - ta;
    if (dbg_on) {
        double wr = 0; int nd = 0, nt = B < 64 ? B : 64;
        for (int b = 0; b < nt; b++) {
            const float *r = dbg_x + ((size_t)b * c->n_layers + l) * d;
            double e2 = 0, n2 = 0;
            for (int i = 0; i < d; i++) { const double df = vkb.xT[(size_t)i * B + b] - r[i]; e2 += df * df; n2 += (double)r[i] * r[i]; }
            if (sqrt(e2 / n2) > wr) wr = sqrt(e2 / n2);
            for (int i = 0; i < d; i++) vkq.col[i] = vkb.xT[(size_t)i * B + b];
            rmsnorm(m->xb, vkq.col, L->ffn_norm, d, c->eps);
            const int sv = cpu_exact; cpu_exact = 1; dbg_on = 0; moe_route(m, L, l); dbg_on = 1; cpu_exact = sv;
            int same = 0;
            for (int j = 0; j < K; j++) for (int k = 0; k < K; k++) same += moe.sel[j] == dbg_sel[((size_t)b * c->n_layers + l) * K + k];
            nd += same != K;
            if (same != K) {
                float p8 = 1e30f, p9 = -1e30f;     /* 8o escolhido e melhor nao escolhido (lote) */
                for (int j = 0; j < K; j++) if (moe.lg[moe.sel[j]] < p8) p8 = moe.lg[moe.sel[j]];
                for (int e = 0; e < c->n_expert; e++) { int in = 0; for (int j = 0; j < K; j++) in |= moe.sel[j] == e; if (!in && moe.lg[e] > p9) p9 = moe.lg[e]; }
                fprintf(stderr, "dbg   token %d: 8o %.6f 9o %.6f margem relativa %.2e\n", b, p8, p9, (p8 - p9) / p8);
            }
        }
        fprintf(stderr, "dbg camada %2d %s: erro relativo maximo %.2e, tokens com top-%d diferente %d/%d\n", l, L->lin ? "dn " : "att", wr, K, nd, nt);
    }
    if (gx.loader) loader_install(m);       /* fila vazia: as vagas podem ser reescritas */
    const uint64_t tick = ++gx.clock;
    int np = 0, nc = 0;
    for (int v = 0; v <= gx.S; v++) vkq.cnt[v] = 0;
    for (int b = 0; b < B; b++) {
        float *xn = vkq.xn + (size_t)b * d;
        for (int i = 0; i < d; i++) vkq.col[i] = vkb.xT[(size_t)i * B + b];
        rmsnorm(xn, vkq.col, L->ffn_norm, d, c->eps);
        if (layer_tap) layer_tap(l, vkq_posn + b, xn);
        memcpy(m->xb, xn, (size_t)d * sizeof(float));
        moe_route(m, L, l);
        vkq.sw[b] = vkm.sh ? shexp_gate_w(m, L) : 0.0f;
        for (int e = 0; e < E; e++) heat[e] *= gx.decay;
        for (int j = 0; j < K; j++) heat[moe.sel[j]] += 1.0f;
        for (int j = 0; j < K; j++) {
            const int e = moe.sel[j];
            int v = gx.slot[(size_t)l * E + e].slot;
            if (v >= 0) gx.hits++;
            else {
                gx.miss++;
                int cand = -1;
                for (int i = 0; i < gx.S; i++) {
                    if (own[i] < 0) { cand = i; break; }
                    if (st[i] == tick) continue;
                    if (cand < 0 || heat[own[i]] < heat[own[cand]]) cand = i;
                }
                if (gx.loader) { if (cand >= 0 && own[cand] < 0) loader_request(l, e, E); }   /* lote: so vaga livre */
                else if (cand >= 0 && (own[cand] < 0 || !gx.cpu_cold || heat[e] > heat[own[cand]] + 0.5f)) { expert_load(m, l, e, cand); v = cand; }
            }
            if (v >= 0) { st[v] = tick; vkq.pv[np] = v; vkq.pb[np] = b; vkq.pw[np++] = moe.w[j]; vkq.cnt[v]++; }
            else { vkq.ce[nc] = e; vkq.cb[nc] = b; vkq.cw_[nc++] = moe.w[j]; }
        }
    }
    const double tc = now_sec();
    vkq.t_route += tc - tb; vkq.n_hot += np; vkq.n_cold += nc;
    /* meta: [compartilhado: (b, sw)] [por vaga: (b, w)...] [frios somados: (b, 1)] */
    uint32_t *mt = vkq.mt;
    const int off_sh = 0, off_v = 2 * B;
    for (int b = 0; b < B; b++) { mt[2 * b] = (uint32_t)b; memcpy(&mt[2 * b + 1], &vkq.sw[b], 4); }
    int acc = 0;
    for (int v = 0; v < gx.S; v++) { vkq.ofs[v] = acc; acc += vkq.cnt[v]; }
    for (int v = 0; v < gx.S; v++) vkq.cnt[v] = 0;
    for (int p = 0; p < np; p++) {
        const int v = vkq.pv[p], k = vkq.ofs[v] + vkq.cnt[v]++;
        mt[off_v + 2 * k] = (uint32_t)vkq.pb[p]; memcpy(&mt[off_v + 2 * k + 1], &vkq.pw[p], 4);
    }
    const int off_c = off_v + 2 * np;
    const float one = 1.0f;
    for (int b = 0; b < B; b++) { mt[off_c + 2 * b] = (uint32_t)b; memcpy(&mt[off_c + 2 * b + 1], &one, 4); }
    int ok = kvk_buf_write_q(vkq.meta, 0, mt, (size_t)(off_c + 2 * B) * 4);
    const struct { int n; float eps; int B; } rp = { d, c->eps, B };
    const KvkBuf rb[3] = { vkb.x, vkg.fn[l], vkq.xf };
    ok &= kvk_dispatch(vkq.rms32, rb, 3, &rp, sizeof rp, (uint32_t)((B + 7) / 8), 1, 1);
    if (vkm.sh) {
        ok &= vk_mm_p(&L->sg, vkq.xf, vkq.hg, Hs, d, B, 0, 2);
        ok &= vk_mm_p(&L->sd, vkq.hg, vkq.ys, d, Hs, B, 0, 1);
        ok &= vkq_scat(vkq.ys, B, B, off_sh);
    }
    const uint32_t kg = gpu_kind(&L->eg[0]), kd = gpu_kind(&L->ed[0]);
    const size_t wg = gpu_wpr(&L->eg[0], d), wd = gpu_wpr(&L->ed[0], H);
    for (int v = 0; v < gx.S && ok; v++) {
        const int n = vkq.cnt[v], mo = off_v + 2 * vkq.ofs[v];
        if (!n) continue;
        const KvkBuf gb[3] = { vkq.xf, vkq.xg, vkq.meta };
        const int gp[4] = { d, B, n, mo };
        ok &= kvk_dispatch(vkq.gath32, gb, 3, gp, sizeof gp, (uint32_t)(((size_t)d * n + 255) / 256), 1, 1);
        ok &= vk_mm_pw(kg, vk_sg[l], (uint32_t)((size_t)v * (gx.bg[l] / 4)), wg, vkq.xg, vkq.hg, H, d, n, 0, 2);
        ok &= vk_mm_pw(kd, vk_sd[l], (uint32_t)((size_t)v * (gx.bd[l] / 4)), wd, vkq.hg, vkq.ys, d, H, n, 0, 1);
        ok &= vkq_scat(vkq.ys, B, n, mo);
    }
    if (!ok) die("falha na execucao em Vulkan (MoE do lote)");
    if (nc > 0) {       /* frios na CPU (blocos de ate GPU_BMAX tokens) enquanto a GPU trabalha */
        const double t0 = now_sec();
        kvk_flush();
        memset(vkq.xc, 0, (size_t)d * B * sizeof(float));
        int p = 0;
        while (p < nc) {
            const int b0 = vkq.cb[p];
            int q = p;
            while (q < nc && vkq.cb[q] < b0 + GPU_BMAX) q++;
            int xi[GPU_BMAX * 64];
            for (int i = p; i < q; i++) xi[i - p] = vkq.cb[i] - b0;
            const int nb = vkq.cb[q - 1] - b0 + 1;
            moe_cpu_multi(m, L, vkq.ce + p, xi, q - p, nb, vkq.xn + (size_t)b0 * d, vkq.outs);
            for (int i = p; i < q; i++) {
                const float w = vkq.cw_[i], *o = vkq.outs + (size_t)(i - p) * d;
                const int b = vkq.cb[i];
                for (int r = 0; r < d; r++) vkq.xc[(size_t)r * B + b] += w * o[r];
            }
            p = q;
        }
        gx.cpu_runs += nc;
        gx.t_cpu += now_sec() - t0; vkq.t_cpu += now_sec() - t0;
        if (!kvk_buf_write_q(vkq.yc, 0, vkq.xc, (size_t)d * B * sizeof(float)) || !vkq_scat(vkq.yc, B, B, off_c))
            die("falha na execucao em Vulkan (MoE do lote, CPU)");
    }
}

/* B tokens (embeddings ja em vkb.x) a partir de pos0 */
static float *vk_qn_forward_n(Model *m, int B, int pos0, int want_logits) {
    const Config *c = &m->cfg;
    const int d = c->dim, hd = c->hd, nh = c->n_heads, kvd = c->n_kv * hd, qd = nh * hd, NV = vkq.NV, NK = vkq.NK, inner = c->ssm_inner;
    const int Bp = (B + 63) / 64 * 64, nch = Bp / 64;
    vkq_posn = pos0;
    const double t0 = now_sec();
    const struct { int n; float eps; int B; } rp = { d, c->eps, B };
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
        int ok = 1;
        const KvkBuf rb[3] = { vkb.x, vkg.an[l], vkq.xf };      /* entrada das projecoes em fp32 (produtos precisos) */
        ok &= kvk_dispatch(vkq.rms32, rb, 3, &rp, sizeof rp, (uint32_t)((B + 7) / 8), 1, 1);
        if (L->lin) {
            const int li = L->lin_i;
            ok &= vk_mm_p(&L->wqkv, vkq.xf, vkq.bqkv, vkq.CH, d, B, 0, 1);
            ok &= vk_mm_p(&L->wz, vkq.xf, vkq.bz, inner, d, B, 0, 1);
            ok &= vk_mm_p(&vkq.wab[li], vkq.xf, vkq.bab, 2 * NV, d, B, 0, 1);
            if (vkq.dnstep) {   /* conferencia: recorrencia token a token (dn_step, fp32) dentro do lote */
                for (int b = 0; b < B && ok; b++) {
                    const int mp[4] = { pos0 + b, 0, 0, 0 };
                    uint32_t mt[2] = { (uint32_t)b, 0 };
                    const float one = 1.0f; memcpy(&mt[1], &one, 4);
                    ok &= kvk_buf_write_q(vkg.meta, 0, mp, sizeof mp);
                    ok &= kvk_buf_write_q(vkq.meta, (size_t)(2 * vkq.P * (c->n_used + 2) - 2) * 4, mt, sizeof mt);
                    const KvkBuf g1[2] = { vkq.bqkv, vkq.qkv }, g2[2] = { vkq.bz, vkq.z }, g3[2] = { vkq.bab, vkq.ab };
                    const int p1[3] = { vkq.CH, B, b }, p2[3] = { inner, B, b }, p3[3] = { 2 * NV, B, b };
                    ok &= kvk_dispatch(vkb.gat, g1, 2, p1, sizeof p1, (uint32_t)((vkq.CH + 127) / 128), 1, 1);
                    ok &= kvk_dispatch(vkb.gat, g2, 2, p2, sizeof p2, (uint32_t)((inner + 127) / 128), 1, 1);
                    ok &= kvk_dispatch(vkb.gat, g3, 2, p3, sizeof p3, 1, 1, 1);
                    const KvkBuf sb[9] = { vkq.qkv, vkq.z, vkq.ab, vkq.cw[li], vkq.pr[li], vkq.ring[li], vkq.st[li], vkq.o, vkg.meta };
                    const struct { int pos, kmap; float e1, e2; } stp = { 0, vkq.kmap, c->eps, c->eps };
                    ok &= kvk_dispatch(vkq.step, sb, 9, &stp, sizeof stp, (uint32_t)NV, 1, 1);
                    ok &= vk_gmv_ex(&L->wssm_out, vkq.o, vkq.ys, vkg.dummy, d, inner, 0, 0);
                    ok &= vkq_scat(vkq.ys, B, 1, 2 * vkq.P * (c->n_used + 2) - 2);
                }
                if (!ok) die("falha na execucao em Vulkan (lote qwen35moe, conferencia)");
                vkq_moe_n(m, L, l, B);
                continue;
            }
            if (!vkq.blk) {   /* recorrencia exata em fp32, um lancamento por camada; depois o anel da convolucao */
                const KvkBuf io[8] = { vkq.bqkv, vkq.bz, vkq.bab, vkq.cw[li], vkq.pr[li], vkq.ring[li], vkq.st[li], vkq.ub };
                const struct { int pos0, B, kmap; float e1, e2; } pc = { pos0, B, vkq.kmap, c->eps, c->eps };
                ok &= kvk_dispatch(vkq.seq, io, 8, &pc, sizeof pc, (uint32_t)NV, 1, 1);
                const KvkBuf io2[2] = { vkq.bqkv, vkq.ring[li] };
                const struct { int pos0, B, ch; } pc2 = { pos0, B, vkq.CH };
                ok &= kvk_dispatch(vkq.cst, io2, 2, &pc2, sizeof pc2, (uint32_t)((vkq.CH + 255) / 256), 1, 1);
                ok &= vk_mm_p(&L->wssm_out, vkq.ub, vkb.x, d, inner, B, 1, 1);
                if (!ok) die("falha na execucao em Vulkan (lote qwen35moe)");
                if (vkbm.on && vkq.moedn && !dbg_on) vk_moe_n(m, L, l, B);      /* frios de varios tokens em vagas temporarias, como na camada de atencao */
                else vkq_moe_n(m, L, l, B);
                continue;
            }
            {   const KvkBuf io[9] = { vkq.bqkv, vkq.bab, vkq.cw[li], vkq.pr[li], vkq.ring[li], vkq.qh, vkq.kh, vkq.vh, vkq.bg };
                const struct { int pos0, B, Bp; float eps; } pc = { pos0, B, Bp, c->eps };
                ok &= kvk_dispatch(vkq.prep, io, 9, &pc, sizeof pc, (uint32_t)(2 * NK + NV), (uint32_t)(Bp / 32), 1); }
            {   const KvkBuf io[2] = { vkq.bqkv, vkq.ring[li] };
                const struct { int pos0, B, ch; } pc = { pos0, B, vkq.CH };
                ok &= kvk_dispatch(vkq.cst, io, 2, &pc, sizeof pc, (uint32_t)((vkq.CH + 255) / 256), 1, 1); }
            {   const KvkBuf io[10] = { vkq.qh, vkq.kh, vkq.vh, vkq.bg, vkq.ub, vkq.wb, vkq.qm, vkq.qgh, vkq.kd, vkq.gl };
                const struct { int Bp, kmap; } pc = { Bp, vkq.kmap };
                ok &= kvk_dispatch(vkq.chunk, io, 10, &pc, sizeof pc, (uint32_t)nch, (uint32_t)NV, 1); }
            {   const KvkBuf io[8] = { vkq.ub, vkq.wb, vkq.qm, vkq.qgh, vkq.kd, vkq.gl, vkq.st[li], vkq.bo };
                const struct { int Bp, nch, pos0; } pc = { Bp, nch, pos0 };
                ok &= kvk_dispatch(vkq.state, io, 8, &pc, sizeof pc, (uint32_t)NV, 128 / 16, 1); }
            {   const KvkBuf io[4] = { vkq.bo, vkq.bz, vkq.pr[li], vkq.ub };     /* ub (livre aqui) recebe a saida em fp32 */
                const struct { int B, Bp; float eps; } pc = { B, Bp, c->eps };
                ok &= kvk_dispatch(vkq.norm, io, 4, &pc, sizeof pc, (uint32_t)NV, (uint32_t)((B + 31) / 32), 1); }
            ok &= vk_mm_p(&L->wssm_out, vkq.ub, vkb.x, d, inner, B, 1, 1);
        } else {
            ok &= vk_mm_p(&L->wq, vkq.xf, vkq.bqg, 2 * qd, d, B, 0, 1);
            ok &= vk_mm_p(&L->wk, vkq.xf, vkb.k, kvd, d, B, 0, 1);
            ok &= vk_mm_p(&L->wv, vkq.xf, vkb.v, kvd, d, B, 0, 1);
            if (vkq.cpuatt) {     /* atencao do lote na CPU (qn_core_n, cache KV na RAM); layouts da GPU sao [linha][B] */
                ok &= kvk_buf_read(vkq.bqg, 0, vkq.hT, (size_t)2 * qd * B * 4);
                if (!ok) die("falha na execucao em Vulkan (lote qwen35moe, leitura da atencao)");
                for (int b = 0; b < B; b++) for (int i = 0; i < 2 * qd; i++) vkq.hq[(size_t)b * 2 * qd + i] = vkq.hT[(size_t)i * B + b];
                ok &= kvk_buf_read(vkb.k, 0, vkq.hT, (size_t)kvd * B * 4);
                for (int b = 0; b < B; b++) for (int i = 0; i < kvd; i++) vkq.hk[(size_t)b * kvd + i] = vkq.hT[(size_t)i * B + b];
                ok &= kvk_buf_read(vkb.v, 0, vkq.hT, (size_t)kvd * B * 4);
                for (int b = 0; b < B; b++) for (int i = 0; i < kvd; i++) vkq.hv[(size_t)b * kvd + i] = vkq.hT[(size_t)i * B + b];
                if (!ok) die("falha na execucao em Vulkan (lote qwen35moe, leitura da atencao)");
                for (int b0 = 0; b0 < B; b0 += GPU_BMAX) {
                    const int nb = B - b0 < GPU_BMAX ? B - b0 : GPU_BMAX;
                    qn_core_n(m, L, l, pos0 + b0, nb, vkq.hq + (size_t)b0 * 2 * qd, vkq.hk + (size_t)b0 * kvd, vkq.hv + (size_t)b0 * kvd,
                              vkq.ho + (size_t)b0 * qd);
                }
                for (int i = 0; i < qd; i++) for (int b = 0; b < B; b++) vkq.hT[(size_t)i * B + b] = vkq.ho[(size_t)b * qd + i];
                ok &= kvk_buf_write_q(vkq.ub, 0, vkq.hT, (size_t)qd * B * 4);
                ok &= vk_mm_p(&L->wo, vkq.ub, vkb.x, d, qd, B, 1, 1);
                if (!ok) die("falha na execucao em Vulkan (lote qwen35moe)");
                if (vkbm.on && !dbg_on) vk_moe_n(m, L, l, B);
                else vkq_moe_n(m, L, l, B);
                continue;
            }
            const KvkBuf qb[12] = { vkb.q2, vkb.k, vkb.v, vkg.dummy, vkg.dummy, vkg.dummy, vkg.kc[l], vkg.vc[l],
                                    vkg.freq, vkq.bqg, vkg.qn[l], vkg.kn[l] };
            const struct { int base, pos0, hd, neox, has_bias, d, kvd, norm, B; float eps; int nrot, qst; } qp =
                { 0, pos0, hd, 1, 0, qd, kvd, 2, B, c->eps, vkq.nrot, 2 * hd };
            ok &= kvk_dispatch(vkb.qkv, qb, 12, &qp, sizeof qp, (uint32_t)((B * (qd / 2 + kvd / 2 + kvd) + 127) / 128), 1, 1);
            const KvkBuf ab[4] = { vkb.q2, vkg.kc[l], vkg.vc[l], vkq.ub };     /* saida em fp32 (ub livre aqui) */
            const struct { int base, pos0, kvd, nkv, B; float scale; } ap = { 0, pos0, kvd, c->n_kv, B, 1.0f / sqrtf((float)hd) };
            ok &= kvk_dispatch(vkq.flash32, ab, 4, &ap, sizeof ap, (uint32_t)(B * c->n_kv), 1, 1);
            const KvkBuf gb[2] = { vkq.ub, vkq.bqg };
            const int gp[3] = { hd, nh, B };
            ok &= kvk_dispatch(vkq.gate32, gb, 2, gp, sizeof gp, (uint32_t)(((size_t)qd * B + 255) / 256), 1, 1);
            ok &= vk_mm_p(&L->wo, vkq.ub, vkb.x, d, qd, B, 1, 1);
        }
        if (!ok) die("falha na execucao em Vulkan (lote qwen35moe)");
        if (vkbm.on && !dbg_on) vk_moe_n(m, L, l, B);
        else vkq_moe_n(m, L, l, B);
    }
    vkb.t_emit += now_sec() - t0;
    if (getenv("KATANA_QN_PROF")) {
        fprintf(stderr, "[lote qwen35moe] %d tokens em %.3f s: leitura/espera %.3f, norma+roteamento %.3f, frios na CPU %.3f (pares GPU %ld, CPU %ld)\n",
                B, now_sec() - t0, vkq.t_rd, vkq.t_route, vkq.t_cpu, vkq.n_hot, vkq.n_cold);
        vkq.t_rd = vkq.t_route = vkq.t_cpu = 0; vkq.n_hot = vkq.n_cold = 0;
    }
    if (!want_logits) { if (!kvk_flush()) die("falha na execucao em Vulkan"); return NULL; }
    const KvkBuf gb[2] = { vkb.x, vkg.x };
    const int gp[3] = { d, B, B - 1 };
    if (!kvk_dispatch(vkb.gat, gb, 2, gp, sizeof gp, (uint32_t)((d + 127) / 128), 1, 1) || !vk_emit_head(m)
        || !kvk_buf_read(vkg.lg, 0, m->logits, (size_t)c->vocab * sizeof(float))) die("falha na execucao em Vulkan (cabeca do lote)");
    return m->logits;
}

/* camadas [l0, l1) na CPU sobre o estado m->x, para o token na posicao pos */
/* gemma4: atencao de um token, cabecas divididas entre as threads; posicoes t0..t0+n-1 no anel de cap */
typedef struct { const float *q; float *o; const float *kb, *vb; const uint16_t *kh, *vh; int hd, kvd, group, t0, n, cap; } G4AttJob;
/* gemma4: grava k e v de uma posicao no cache (fp32 ou fp16) */
static void g4_kv_put(float *kb, float *vb, uint16_t *kh, uint16_t *vh, size_t r, const float *k, const float *v, int kvd) {
    if (kh) {
        for (int i = 0; i < kvd; i += 8) {
            _mm_storeu_si128((__m128i *)(kh + r + i), _mm256_cvtps_ph(_mm256_loadu_ps(k + i), 0));
            _mm_storeu_si128((__m128i *)(vh + r + i), _mm256_cvtps_ph(_mm256_loadu_ps(v + i), 0));
        }
    } else { memcpy(kb + r, k, kvd * sizeof(float)); memcpy(vb + r, v, kvd * sizeof(float)); }
}
/* out += a * v (v em fp16, n multiplo de 8) */
static inline void axpy_h(float *out, float a, const uint16_t *v, int n) {
    const __m256 av = _mm256_set1_ps(a);
    for (int k = 0; k < n; k += 8)
        _mm256_storeu_ps(out + k, _mm256_fmadd_ps(av, _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(v + k))), _mm256_loadu_ps(out + k)));
}
static void g4_att_job(void *c, int h0, int h1) {
    const G4AttJob *j = c;
    static __thread float *att; static __thread int acap;
    if (acap < j->n) { free(att); att = xalloc((size_t)j->n, sizeof(float)); acap = j->n; }
    for (int h = h0; h < h1; h++) {
        const float *qh = j->q + (size_t)h * j->hd;
        const int kvh = (h / j->group) * j->hd;
        for (int i = 0; i < j->n; i++) {
            const size_t r = (size_t)((j->t0 + i) % j->cap) * j->kvd + kvh;
            att[i] = j->kh ? dot_h(qh, j->kh + r, j->hd) : dot(qh, j->kb + r, j->hd);
        }
        softmax(att, j->n);
        float *out = j->o + (size_t)h * j->hd;
        memset(out, 0, j->hd * sizeof(float));
        for (int i = 0; i < j->n; i++) {
            const size_t r = (size_t)((j->t0 + i) % j->cap) * j->kvd + kvh;
            if (j->vh) { axpy_h(out, att[i], j->vh + r, j->hd); continue; }
            const float *vh = j->vb + r, a = att[i];
            for (int k = 0; k < j->hd; k++) out[k] += a * vh[k];
        }
    }
}

/* norma RMS sem peso (no lugar) */
static void rms_plain(float *x, int n, float eps) {
    const float inv = 1.0f / sqrtf(dot(x, x, n) / n + eps);
    for (int i = 0; i < n; i++) x[i] *= inv;
}

/* gemma4, uma camada, um token (m->x). Atencao com escala 1 (q e k normalizados por cabeca, v pela norma
   sem peso); camada local so ve as ultimas swa_win posicoes (anel no cache); sem attn_v, v = k antes da
   norma. Feed-forward: denso (GELU) e MoE lado a lado, cada um com sua norma de saida, somados e
   normalizados de novo antes do residuo; o roteador le o residuo normalizado sem peso, vezes a escala
   do arquivo e 1/sqrt(dim). No fim a camada inteira e multiplicada por out_scale. */
/* ================= gpt-oss (arquitetura OpenAI MoE) =================
   atencao com "sink" por cabeca (logit extra no denominador do softmax) e janela deslizante nas
   camadas pares; MoE com SwiGLU OAI (alpha 1.702, limite 7) e vieses em gate/up/down; roteador com
   vies, top-K sobre os logits e softmax sobre os escolhidos (moe_route com moe_norm cuida disso). */
static inline float go_swiglu(float gate, float up) {
    const float a = 1.702f, lim = 7.0f;
    const float x = gate < lim ? gate : lim;
    const float y = up < -lim ? -lim : up > lim ? lim : up;
    return (x / (1.0f + expf(-a * x))) * (y + 1.0f);
}

/* especialistas do token na CPU: entrada normalizada em m->xb; outs[k*d..] recebe a saida do k-esimo */
static void go_moe_experts(Model *m, const Layer *L, const int *sel, int K, float *outs) {
    const Config *c = &m->cfg;
    const int d = c->dim, H = c->hidden;
    static float *g, *u, *h;
    if (!g) { g = xalloc(H, 4); u = xalloc(H, 4); h = xalloc(H, 4); }
    for (int k = 0; k < K; k++) {
        const int e = sel[k];
        matvec(g, &L->eg[e], m->xb, H, d);
        matvec(u, &L->eu[e], m->xb, H, d);
        const float *gb = L->eg_b + (size_t)e * H, *ub = L->eu_b + (size_t)e * H;
        for (int i = 0; i < H; i++) h[i] = go_swiglu(g[i] + gb[i], u[i] + ub[i]);
        float *o = outs + (size_t)k * d;
        matvec(o, &L->ed[e], h, d, H);
        const float *db = L->ed_b + (size_t)e * d;
        for (int i = 0; i < d; i++) o[i] += db[i];
    }
}

static void go_layer(Model *m, const Layer *L, int l, int pos) {
    const Config *c = &m->cfg;
    const int d = c->dim, nh = c->n_heads, hd = c->hd, nkv = c->n_kv;
    const int qd = nh * hd, kvd = nkv * hd, half = hd / 2, group = nh / nkv, K = c->n_used;
    const float scale = c->kq_mul / sqrtf((float)hd);
    static float *outs;
    if (!outs) outs = xalloc((size_t)MOE_KMAX * d, 4);
    float *x = m->x;
    rmsnorm(m->xb, x, L->attn_norm, d, c->eps);
    matvec(m->q, &L->wq, m->xb, qd, d);
    matvec(m->k, &L->wk, m->xb, kvd, d);
    matvec(m->v, &L->wv, m->xb, kvd, d);
    for (int i = 0; i < qd; i++) m->q[i] += L->bq[i];
    for (int i = 0; i < kvd; i++) { m->k[i] += L->bk[i]; m->v[i] += L->bv[i]; }
    for (int i = 0; i < half; i++) {        /* RoPE em metades (yarn ja embutido em rope_freq) */
        const float ang = pos * m->rope_freq[i], cs = cosf(ang), sn = sinf(ang);
        for (int h = 0; h < nh; h++) { float *q = m->q + h * hd + i, a = q[0], b = q[half]; q[0] = a * cs - b * sn; q[half] = a * sn + b * cs; }
        for (int h = 0; h < nkv; h++) { float *k = m->k + h * hd + i, a = k[0], b = k[half]; k[0] = a * cs - b * sn; k[half] = a * sn + b * cs; }
    }
    float *kb = m->kcache + (size_t)l * c->ctx * kvd, *vb = m->vcache + (size_t)l * c->ctx * kvd;
    memcpy(kb + (size_t)pos * kvd, m->k, kvd * sizeof(float));
    memcpy(vb + (size_t)pos * kvd, m->v, kvd * sizeof(float));
    const int win = L->swa && c->swa_win < c->ctx ? c->swa_win : c->ctx;
    const int t0 = pos - win + 1 > 0 ? pos - win + 1 : 0, n = pos - t0 + 1;
    for (int h = 0; h < nh; h++) {
        const float *qh = m->q + h * hd;
        const int kvh = (h / group) * hd;
        float *att = m->att;
        for (int i = 0; i < n; i++) att[i] = dot(qh, kb + (size_t)(t0 + i) * kvd + kvh, hd) * scale;
        float mx = L->sinks[h];     /* o sink e um logit extra (valor nulo) no denominador */
        for (int i = 0; i < n; i++) if (att[i] > mx) mx = att[i];
        float sum = expf(L->sinks[h] - mx);
        for (int i = 0; i < n; i++) { att[i] = expf(att[i] - mx); sum += att[i]; }
        for (int i = 0; i < n; i++) att[i] /= sum;
        float *out = m->xb + h * hd;
        memset(out, 0, hd * sizeof(float));
        for (int i = 0; i < n; i++) { const float *vh = vb + (size_t)(t0 + i) * kvd + kvh, a = att[i]; for (int j = 0; j < hd; j++) out[j] += a * vh[j]; }
    }
    matvec(m->xb2, &L->wo, m->xb, d, qd);
    for (int i = 0; i < d; i++) m->xb2[i] += L->bo[i];
    for (int i = 0; i < d; i++) x[i] += m->xb2[i];
    /* MoE (a norma post_attention esta em ffn_norm) */
    rmsnorm(m->xb, x, L->ffn_norm, d, c->eps);
    moe_route(m, L, l);
    go_moe_experts(m, L, moe.sel, K, outs);
    for (int i = 0; i < d; i++) { float acc = 0.0f; for (int k = 0; k < K; k++) acc += moe.w[k] * outs[(size_t)k * d + i]; x[i] += acc; }
}

static void g4_layer(Model *m, const Layer *L, int l, int pos) {
    const Config *c = &m->cfg;
    const int d = c->dim, nh = c->n_heads, hd = L->hd_l, nkv = L->nkv_l, qd = nh * hd, kvd = nkv * hd;
    const int group = nh / nkv, half = hd / 2, Hd = c->hidden_dense, K = c->n_used;
    const float *fr = L->swa ? m->rope_freq_swa : m->rope_freq;
    static float *ya, *yb, *outs;
    if (!ya) { ya = xalloc(d, sizeof(float)); yb = xalloc(d, sizeof(float)); outs = xalloc((size_t)MOE_KMAX * d, sizeof(float)); }
    float *x = m->x;
    rmsnorm(m->xb, x, L->attn_norm, d, c->eps);
    matvec(m->q, &L->wq, m->xb, qd, d);
    matvec(m->k, &L->wk, m->xb, kvd, d);
    if (L->wv.q || L->wv.f) matvec(m->v, &L->wv, m->xb, kvd, d);
    else memcpy(m->v, m->k, kvd * sizeof(float));
    for (int h = 0; h < nh; h++) rmsnorm(m->q + h * hd, m->q + h * hd, L->q_norm, hd, c->eps);
    for (int h = 0; h < nkv; h++) { rmsnorm(m->k + h * hd, m->k + h * hd, L->k_norm, hd, c->eps); rms_plain(m->v + h * hd, hd, c->eps); }
    for (int i = 0; i < half; i++) {        /* RoPE em metades */
        const float ang = pos * fr[i], cs = cosf(ang), sn = sinf(ang);
        for (int h = 0; h < nh; h++) { float *p = m->q + h * hd + i, a = p[0], b = p[half]; p[0] = a * cs - b * sn; p[half] = a * sn + b * cs; }
        for (int h = 0; h < nkv; h++) { float *p = m->k + h * hd + i, a = p[0], b = p[half]; p[0] = a * cs - b * sn; p[half] = a * sn + b * cs; }
    }
    float *kb = m->kcache ? m->kcache + L->kv_off : NULL, *vb = m->vcache ? m->vcache + L->kv_off : NULL;
    uint16_t *kh = m->kh ? m->kh + L->kv_off : NULL, *vh = m->vh ? m->vh + L->kv_off : NULL;
    const int cap = L->kv_cap;
    g4_kv_put(kb, vb, kh, vh, (size_t)(pos % cap) * kvd, m->k, m->v, kvd);
    const int win = L->swa && c->swa_win < cap ? c->swa_win : cap;
    const int t0 = pos - win + 1 > 0 ? pos - win + 1 : 0, n = pos - t0 + 1;
    {
        G4AttJob aj = { m->q, m->xb, kb, vb, kh, vh, hd, kvd, group, t0, n, cap };
        if (n < 64) g4_att_job(&aj, 0, nh);
        else pool_for(g4_att_job, &aj, nh, 1);
    }
    matvec(m->xb2, &L->wo, m->xb, d, qd);
    rmsnorm(m->xb2, m->xb2, L->post_attn_norm, d, c->eps);
    for (int i = 0; i < d; i++) x[i] += m->xb2[i];
    /* feed-forward denso */
    rmsnorm(m->xb, x, L->ffn_norm, d, c->eps);
    matvec(m->hb, &L->w_gate, m->xb, Hd, d);
    matvec(m->hb2, &L->w_up, m->xb, Hd, d);
    for (int i = 0; i < Hd; i++) m->hb[i] = gelu_t(m->hb[i]) * m->hb2[i];
    matvec(ya, &L->w_down, m->hb, d, Hd);
    if (L->eg) {
        rmsnorm(ya, ya, L->post_ffn1, d, c->eps);
        const float rs = 1.0f / sqrtf((float)d);
        memcpy(m->xb, x, d * sizeof(float));
        rms_plain(m->xb, d, c->eps);
        for (int i = 0; i < d; i++) m->xb[i] *= rs * L->router_scale[i];
        memset(yb, 0, d * sizeof(float));
        if (gx.on && gpu.mv_host && !gpu.force_cpu && gx_host_layer(l)) {
            /* cache de especialistas na GPU: o roteador ja pontuou (a entrada dele e outra), os especialistas leem xb */
            static float *lg;
            if (!lg) lg = xalloc(c->n_expert, sizeof(float));
            const double trt0 = now_sec();
            matvec(lg, &L->router, m->xb, c->n_expert, d);
            gprof.t_route += now_sec() - trt0;
            rmsnorm(m->xb, x, L->pre_ffn2, d, c->eps);
            moe_lg_ext = lg;
            host_moe_ffn(m, L, l, yb);
            moe_lg_ext = NULL;
        } else {
            moe_route(m, L, l);
            rmsnorm(m->xb, x, L->pre_ffn2, d, c->eps);
            moe_cpu_experts(m, L, moe.sel, K, outs);
            for (int k = 0; k < K; k++) { const float w = moe.w[k], *o = outs + (size_t)k * d; for (int i = 0; i < d; i++) yb[i] += w * o[i]; }
        }
        rmsnorm(yb, yb, L->post_ffn2, d, c->eps);
        for (int i = 0; i < d; i++) ya[i] += yb[i];
    }
    rmsnorm(ya, ya, L->post_ffn_norm, d, c->eps);
    for (int i = 0; i < d; i++) x[i] = (x[i] + ya[i]) * L->out_scale;
    if (layer_tap) layer_tap(l, pos, x);
}

/* gemma4, atencao de B tokens (pos0..): par (token, cabeca) por item; janela win; cache em anel de cap */
typedef struct { const float *Q; float *O; const float *kb, *vb; const uint16_t *kh, *vh; int pos0, nh, hd, kvd, group, win, cap; } G4AttN;
static void g4_attn_job(void *c, int i0, int i1) {
    const G4AttN *j = c;
    static __thread float *att; static __thread int acap;
    const int qd = j->nh * j->hd;
    for (int u = i0; u < i1; u++) {
        const int t = u / j->nh, h = u % j->nh, pos = j->pos0 + t;
        const int t0 = pos - j->win + 1 > 0 ? pos - j->win + 1 : 0, n = pos - t0 + 1;
        if (acap < n) { free(att); att = xalloc((size_t)n, sizeof(float)); acap = n; }
        const float *qh = j->Q + (size_t)t * qd + (size_t)h * j->hd;
        const int kvh = (h / j->group) * j->hd;
        for (int i = 0; i < n; i++) {
            const size_t r = (size_t)((t0 + i) % j->cap) * j->kvd + kvh;
            att[i] = j->kh ? dot_h(qh, j->kh + r, j->hd) : dot(qh, j->kb + r, j->hd);
        }
        softmax(att, n);
        float *out = j->O + (size_t)t * qd + (size_t)h * j->hd;
        memset(out, 0, j->hd * sizeof(float));
        for (int i = 0; i < n; i++) {
            const size_t r = (size_t)((t0 + i) % j->cap) * j->kvd + kvh;
            if (j->vh) { axpy_h(out, att[i], j->vh + r, j->hd); continue; }
            const float *vh = j->vb + r, a = att[i];
            for (int k = 0; k < j->hd; k++) out[k] += a * vh[k];
        }
    }
}

static void matmat(float *out, const Mat *W, const float *X, XQ *xqs, int *prepared, int B, int rows, int cols);
/* gemma4, B tokens (B <= G4_NB) a partir de pos0, todas as camadas na CPU: cada matriz e lida uma vez para o
   lote; os pares (token, especialista) da camada saem num despacho so. X: B x dim, atualizado no lugar. */
static void g4_layers_n(Model *m, int pos0, int B, float *X) {
    const Config *c = &m->cfg;
    const int d = c->dim, nh = c->n_heads, K = c->n_used, E = c->n_expert, Hd = c->hidden_dense;
    static float *Xb, *X2, *Q, *KK, *VV, *O, *HB, *HB2, *YA, *YB, *OUTS, *LG;
    static XQ xqs[G4_NB];
    static int ex[G4_NB * MOE_KMAX], xi[G4_NB * MOE_KMAX]; static float ew[G4_NB * MOE_KMAX];
    if (B > G4_NB) die("g4_layers_n: lote grande demais");
    if (!Xb) {
        const size_t qd = (size_t)nh * c->hd, kvd = (size_t)c->n_kv * c->hd;
        Xb = xalloc((size_t)G4_NB * d, 4); X2 = xalloc((size_t)G4_NB * d, 4); YA = xalloc((size_t)G4_NB * d, 4); YB = xalloc((size_t)G4_NB * d, 4);
        Q = xalloc(G4_NB * qd, 4); O = xalloc(G4_NB * qd, 4); KK = xalloc(G4_NB * kvd, 4); VV = xalloc(G4_NB * kvd, 4);
        HB = xalloc((size_t)G4_NB * Hd, 4); HB2 = xalloc((size_t)G4_NB * Hd, 4);
        OUTS = xalloc((size_t)G4_NB * K * d, 4); LG = xalloc((size_t)(E > 1 ? E : 1), 4);
    }
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
        const int hd = L->hd_l, nkv = L->nkv_l, qd = nh * hd, kvd = nkv * hd, half = hd / 2, cap = L->kv_cap;
        const float *fr = L->swa ? m->rope_freq_swa : m->rope_freq;
        int prep = 0;
        for (int t = 0; t < B; t++) rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->attn_norm, d, c->eps);
        matmat(Q, &L->wq, Xb, xqs, &prep, B, qd, d);
        matmat(KK, &L->wk, Xb, xqs, &prep, B, kvd, d);
        if (L->wv.q || L->wv.f) matmat(VV, &L->wv, Xb, xqs, &prep, B, kvd, d);
        else memcpy(VV, KK, (size_t)B * kvd * sizeof(float));
        float *kb = m->kcache ? m->kcache + L->kv_off : NULL, *vb = m->vcache ? m->vcache + L->kv_off : NULL;
        uint16_t *kh = m->kh ? m->kh + L->kv_off : NULL, *vh = m->vh ? m->vh + L->kv_off : NULL;
        for (int t = 0; t < B; t++) {
            float *q = Q + (size_t)t * qd, *k = KK + (size_t)t * kvd, *v = VV + (size_t)t * kvd;
            const int pos = pos0 + t;
            for (int h = 0; h < nh; h++) rmsnorm(q + h * hd, q + h * hd, L->q_norm, hd, c->eps);
            for (int h = 0; h < nkv; h++) { rmsnorm(k + h * hd, k + h * hd, L->k_norm, hd, c->eps); rms_plain(v + h * hd, hd, c->eps); }
            for (int i = 0; i < half; i++) {
                const float ang = pos * fr[i], cs = cosf(ang), sn = sinf(ang);
                for (int h = 0; h < nh; h++) { float *p = q + h * hd + i, a = p[0], b = p[half]; p[0] = a * cs - b * sn; p[half] = a * sn + b * cs; }
                for (int h = 0; h < nkv; h++) { float *p = k + h * hd + i, a = p[0], b = p[half]; p[0] = a * cs - b * sn; p[half] = a * sn + b * cs; }
            }
            g4_kv_put(kb, vb, kh, vh, (size_t)(pos % cap) * kvd, k, v, kvd);
        }
        {
            G4AttN aj = { Q, O, kb, vb, kh, vh, pos0, nh, hd, kvd, nh / nkv, L->swa && c->swa_win < cap ? c->swa_win : cap, cap };
            pool_for(g4_attn_job, &aj, B * nh, 1);
        }
        prep = 0;
        matmat(X2, &L->wo, O, xqs, &prep, B, d, qd);
        for (int t = 0; t < B; t++) {
            float *x = X + (size_t)t * d, *y = X2 + (size_t)t * d;
            rmsnorm(y, y, L->post_attn_norm, d, c->eps);
            for (int i = 0; i < d; i++) x[i] += y[i];
        }
        /* feed-forward denso */
        for (int t = 0; t < B; t++) rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->ffn_norm, d, c->eps);
        prep = 0;
        matmat(HB, &L->w_gate, Xb, xqs, &prep, B, Hd, d);
        matmat(HB2, &L->w_up, Xb, xqs, &prep, B, Hd, d);
        for (size_t i = 0; i < (size_t)B * Hd; i++) HB[i] = gelu_t(HB[i]) * HB2[i];
        prep = 0;
        matmat(YA, &L->w_down, HB, xqs, &prep, B, d, Hd);
        if (L->eg) {
            const float rs = 1.0f / sqrtf((float)d);
            for (int t = 0; t < B; t++) {
                rmsnorm(YA + (size_t)t * d, YA + (size_t)t * d, L->post_ffn1, d, c->eps);
                memcpy(m->xb, X + (size_t)t * d, d * sizeof(float));
                rms_plain(m->xb, d, c->eps);
                for (int i = 0; i < d; i++) m->xb[i] *= rs * L->router_scale[i];
                moe_route(m, L, l);
                for (int j = 0; j < K; j++) { ex[t * K + j] = moe.sel[j]; xi[t * K + j] = t; ew[t * K + j] = moe.w[j]; }
                rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->pre_ffn2, d, c->eps);
            }
            moe_cpu_multi(m, L, ex, xi, B * K, B, Xb, OUTS);
            for (int t = 0; t < B; t++) {
                float *yb = YB + (size_t)t * d, *ya = YA + (size_t)t * d;
                memset(yb, 0, d * sizeof(float));
                for (int j = 0; j < K; j++) { const float w = ew[t * K + j], *o = OUTS + (size_t)(t * K + j) * d; for (int i = 0; i < d; i++) yb[i] += w * o[i]; }
                rmsnorm(yb, yb, L->post_ffn2, d, c->eps);
                for (int i = 0; i < d; i++) ya[i] += yb[i];
            }
        }
        for (int t = 0; t < B; t++) {
            float *x = X + (size_t)t * d, *ya = YA + (size_t)t * d;
            rmsnorm(ya, ya, L->post_ffn_norm, d, c->eps);
            for (int i = 0; i < d; i++) x[i] = (x[i] + ya[i]) * L->out_scale;
            if (layer_tap) layer_tap(l, pos0 + t, x);
        }
    }
}

