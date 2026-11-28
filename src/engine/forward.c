/* granitehybrid: definida em engine/granite.c (incluido depois deste arquivo, pois reusa matmat/AttJob/att_job daqui) */
static void grh_layers_n(Model *m, int l0, int l1, int pos0, int B, float *X);

/* ---------- camadas na CPU em lote (prompt) ----------
   B tokens de uma vez: cada produto le a matriz uma vez para os B tokens (em vez de uma vez por token),
   o que tira o gargalo de memoria do prompt. As ativacoes sao quantizadas uma vez por token e cada
   linha de pesos e multiplicada pelas colunas do bloco enquanto esta na cache. Atencao causal por
   token (o cache de K/V de todo o lote e gravado antes). X: B x dim, token a token. */
typedef struct { float *out; const Mat *W; XQ *xqs; int B, rows, cols; } MmJob;
static void mm_job(void *c, int i0, int i1) {
    const MmJob *j = c;
    if (lowbit_ok(j->W->type)) {    /* tipos de poucos bits: a linha e decodificada uma vez para um grupo de ate LB_MAXB tokens */
        float tmp[LB_MAXB];
        for (int t0 = 0; t0 < j->B; t0 += LB_MAXB) {
            const int t1 = t0 + LB_MAXB < j->B ? t0 + LB_MAXB : j->B;
            for (int i = i0; i < i1; i++) {
                lowbit_dot_batch(j->W, j->W->q + (size_t)i * j->W->stride, j->xqs, t0, t1, j->cols, tmp);
                for (int t = t0; t < t1; t++) j->out[(size_t)t * j->rows + i] = tmp[t - t0];
            }
        }
        return;
    }
    for (int t0 = 0; t0 < j->B; t0 += 8) {
        const int t1 = t0 + 8 < j->B ? t0 + 8 : j->B;
        for (int i = i0; i < i1; i++) {
            for (int t = t0; t < t1; t++) {
                xq_tls = &j->xqs[t];
                j->out[(size_t)t * j->rows + i] = dot_row(j->W, (size_t)i, NULL, NULL, j->cols);
            }
        }
    }
    xq_tls = NULL;
}
typedef struct { XQ *xqs; const float *X; int cols; } MqJob;
static void mq_job(void *c, int i0, int i1) {
    const MqJob *j = c;
    for (int t = i0; t < i1; t++) { xq_tls = &j->xqs[t]; xq_prepare(j->X + (size_t)t * j->cols, j->cols); }
    xq_tls = NULL;
}
/* quantiza as B linhas de X (B x cols) para o produto inteiro */
static void mm_prepare(XQ *xqs, const float *X, int B, int cols) {
    MqJob job = { xqs, X, cols };
    pool_for(mq_job, &job, B, B / (pool.n * 2) + 1);
}
/* out[t*rows + i] = W[i] . X[t], com X ja quantizado em xqs (so para tipos com produto inteiro) */
static void mm_run(float *out, const Mat *W, XQ *xqs, int B, int rows, int cols) {
    MmJob job = { out, W, xqs, B, rows, cols };
    const size_t rb = W->q ? W->stride : (size_t)cols * 4;
    int chunk = (int)(65536 / rb);
    if (chunk > rows / (pool.n * 4)) chunk = rows / (pool.n * 4);
    if (chunk < 1) chunk = 1;
    pool_for(mm_job, &job, rows, chunk);
}
/* produto com B colunas; sem produto inteiro (f32, Q8_0, modo exato) cai no matvec por token */
static void matmat(float *out, const Mat *W, const float *X, XQ *xqs, int *prepared, int B, int rows, int cols) {
    if (!mat_int(W)) {
        for (int t = 0; t < B; t++) matvec(out + (size_t)t * rows, W, X + (size_t)t * cols, rows, cols);
        return;
    }
    if (!*prepared) { mm_prepare(xqs, X, B, cols); *prepared = 1; }
    mm_run(out, W, xqs, B, rows, cols);
}

typedef struct {
    const float *Q; float *O; const float *kb, *vb;
    int pos0, B, nh, hd, kvd, group, qd; float scale; int ctx;
} AttJob;
static void att_job(void *c, int i0, int i1) {
    const AttJob *j = c;
    static __thread float *att; static __thread int cap;
    if (cap < j->ctx) { free(att); att = xalloc((size_t)j->ctx, sizeof(float)); cap = j->ctx; }
    for (int u = i0; u < i1; u++) {
        const int t = u / j->nh, h = u % j->nh, pos = j->pos0 + t;
        const float *qh = j->Q + (size_t)t * j->qd + h * j->hd;
        const int kvh = (h / j->group) * j->hd;
        for (int s = 0; s <= pos; s++) att[s] = dot(qh, j->kb + (size_t)s * j->kvd + kvh, j->hd) * j->scale;
        softmax(att, pos + 1);
        float *out = j->O + (size_t)t * j->qd + h * j->hd;
        memset(out, 0, j->hd * sizeof(float));
        for (int s = 0; s <= pos; s++) {
            const float *vh = j->vb + (size_t)s * j->kvd + kvh, a = att[s];
            for (int i = 0; i < j->hd; i++) out[i] += a * vh[i];
        }
    }
}

#define CPU_NB 64
static void cpu_layers_nc(Model *m, int l0, int l1, int pos0, int B, float *X) {
    const Config *c = &m->cfg;
    const int d = c->dim, hd = c->hd, H = c->hidden, ctx = c->ctx;
    const int kvd = c->n_kv * hd, group = c->n_heads / c->n_kv, qd = c->n_heads * hd;
    const int rhalf = (c->glm || c->glmoe) && c->rope_dim > 0 && c->rope_dim < hd ? c->rope_dim / 2 : hd / 2;
    const int step = c->rope_neox ? 1 : 2, gap = c->rope_neox ? rhalf : 1;
    static float *Xb, *Q, *K, *V, *O, *X2, *HB, *HB2; static XQ xqs[CPU_NB];
    static size_t cap_d, cap_q, cap_k, cap_h;
    if ((size_t)d * CPU_NB > cap_d) {
        free(Xb); free(X2); Xb = xalloc((size_t)d * CPU_NB, sizeof(float)); X2 = xalloc((size_t)d * CPU_NB, sizeof(float));
        cap_d = (size_t)d * CPU_NB;
    }
    if ((size_t)qd * CPU_NB > cap_q) {
        free(Q); free(O); Q = xalloc((size_t)qd * CPU_NB, sizeof(float)); O = xalloc((size_t)qd * CPU_NB, sizeof(float));
        cap_q = (size_t)qd * CPU_NB;
    }
    if ((size_t)kvd * CPU_NB > cap_k) {
        free(K); free(V); K = xalloc((size_t)kvd * CPU_NB, sizeof(float)); V = xalloc((size_t)kvd * CPU_NB, sizeof(float));
        cap_k = (size_t)kvd * CPU_NB;
    }
    if ((size_t)H * CPU_NB > cap_h) {
        free(HB); free(HB2); HB = xalloc((size_t)H * CPU_NB, sizeof(float)); HB2 = xalloc((size_t)H * CPU_NB, sizeof(float));
        cap_h = (size_t)H * CPU_NB;
    }
    for (int l = l0; l < l1; l++) {
        const Layer *L = &m->layers[l];
        if (l + 1 < l1) layer_prefetch(m, l + 1);
        int prep = 0;
        for (int t = 0; t < B; t++) rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->attn_norm, d, c->eps);
        matmat(Q, &L->wq, Xb, xqs, &prep, B, qd, d);
        matmat(K, &L->wk, Xb, xqs, &prep, B, kvd, d);
        matmat(V, &L->wv, Xb, xqs, &prep, B, kvd, d);
        float *kbase = m->kcache + (size_t)l * ctx * kvd;
        float *vbase = m->vcache + (size_t)l * ctx * kvd;
        for (int t = 0; t < B; t++) {
            float *q = Q + (size_t)t * qd, *k = K + (size_t)t * kvd, *v = V + (size_t)t * kvd;
            const int pos = pos0 + t;
            if (L->bq) for (int i = 0; i < qd; i++) q[i] += L->bq[i];
            if (L->bk) for (int i = 0; i < kvd; i++) k[i] += L->bk[i];
            if (L->bv) for (int i = 0; i < kvd; i++) v[i] += L->bv[i];
            if (c->qk_norm == 1) {
                rmsnorm(q, q, L->q_norm, qd, c->eps);
                rmsnorm(k, k, L->k_norm, kvd, c->eps);
            } else if (c->qk_norm == 2) {
                for (int h = 0; h < c->n_heads; h++) rmsnorm(q + h * hd, q + h * hd, L->q_norm, hd, c->eps);
                for (int h = 0; h < c->n_kv; h++) rmsnorm(k + h * hd, k + h * hd, L->k_norm, hd, c->eps);
            }
            for (int i = 0; i < rhalf; i++) {
                float ang = pos * m->rope_freq[i], cs = cosf(ang), sn = sinf(ang);
                for (int h = 0; h < c->n_heads; h++) {
                    float *p = q + h * hd + step * i, a = p[0], b = p[gap];
                    p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs;
                }
                for (int h = 0; h < c->n_kv; h++) {
                    float *p = k + h * hd + step * i, a = p[0], b = p[gap];
                    p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs;
                }
            }
            memcpy(kbase + (size_t)pos * kvd, k, kvd * sizeof(float));
            memcpy(vbase + (size_t)pos * kvd, v, kvd * sizeof(float));
        }
        {
            AttJob aj = { Q, O, kbase, vbase, pos0, B, c->n_heads, hd, kvd, group, qd, 1.0f / sqrtf((float)hd), ctx };
            pool_for(att_job, &aj, B * c->n_heads, 1);
        }
        prep = 0;
        matmat(X2, &L->wo, O, xqs, &prep, B, d, qd);
        if (c->glm) for (int t = 0; t < B; t++) rmsnorm(X2 + (size_t)t * d, X2 + (size_t)t * d, L->post_attn_norm, d, c->eps);
        for (int t = 0; t < B; t++) {
            float *x = X + (size_t)t * d; const float *y = X2 + (size_t)t * d;
            for (int i = 0; i < d; i++) x[i] += y[i] + (L->bo ? L->bo[i] : 0.0f);
        }
        /* --- feed-forward SwiGLU --- */
        for (int t = 0; t < B; t++) rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->ffn_norm, d, c->eps);
        prep = 0;
        matmat(HB, &L->w_gate, Xb, xqs, &prep, B, H, d);
        matmat(HB2, &L->w_up, Xb, xqs, &prep, B, H, d);
        for (size_t i = 0; i < (size_t)B * H; i++) {
            float g = HB[i];
            HB[i] = g / (1.0f + expf(-g)) * HB2[i];
        }
        prep = 0;
        matmat(X2, &L->w_down, HB, xqs, &prep, B, d, H);
        if (c->glm) for (int t = 0; t < B; t++) rmsnorm(X2 + (size_t)t * d, X2 + (size_t)t * d, L->post_ffn_norm, d, c->eps);
        for (size_t i = 0; i < (size_t)B * d; i++) X[i] += X2[i];
    }
}

/* camadas [l0, l1) para B tokens a partir de pos0; X: B x dim (token a token), atualizado no lugar */
static void cpu_layers_n(Model *m, int l0, int l1, int pos0, int B, float *X) {
    const int d = m->cfg.dim;
    if (B < 1) return;
    if (B == 1 || m->cfg.n_expert || sp.collect || sp.oracle_keep || sp.mask) {    /* sem caminho em lote */
        for (int t = 0; t < B; t++) {
            memcpy(m->x, X + (size_t)t * d, d * sizeof(float));
            cpu_layers(m, l0, l1, pos0 + t);
            memcpy(X + (size_t)t * d, m->x, d * sizeof(float));
        }
        return;
    }
    for (int b = 0; b < B; b += CPU_NB) cpu_layers_nc(m, l0, l1, pos0 + b, B - b < CPU_NB ? B - b : CPU_NB, X + (size_t)b * d);
}

/* qwen35moe, B tokens (B <= GPU_BMAX) a partir de pos0 na CPU: projecoes, saidas e feed-forward em
   lote (cada matriz lida uma vez para os B tokens; os pares (token, especialista) da camada saem num
   despacho so); convolucao, recorrencia do DeltaNet e atencao seguem token a token. X: B x dim. */
static float *qn_ckpt; static size_t qn_ckpt_n;      /* decodificacao especulativa: onde guardar o estado recorrente depois de cada posicao do lote (qn_ckpt_n floats por posicao) */
static double qn_prof[6]; static int qn_prof_on;      /* spectime: projecoes+DeltaNet/atencao, roteador, especialistas, compartilhado */
static void qn_layers_n(Model *m, int l0, int l1, int pos0, int B, float *X) {
    const Config *c = &m->cfg;
    double pt0 = 0, pt1 = 0;
    const int d = c->dim, nh = c->n_heads, hd = c->hd, qd = nh * hd, kvd = c->n_kv * hd, K = c->n_used;
    const int C = (int)dn_channels(c), inner = c->ssm_inner, nv = c->ssm_rank, Hs = c->n_shared * c->hidden;
    static float *Xb, *X2, *P1, *Z, *O, *KK, *VV, *AL, *BE, *HB, *HB2, *OUTS;
    static XQ xqs[GPU_BMAX];
    if (B > GPU_BMAX) die("qn_layers_n: lote grande demais");
    if (!Xb) {
        const size_t wide = (size_t)(C > 2 * qd ? C : 2 * qd), o = (size_t)(inner > qd ? inner : qd);
        Xb = xalloc((size_t)GPU_BMAX * d, 4); X2 = xalloc((size_t)GPU_BMAX * d, 4);
        P1 = xalloc(GPU_BMAX * wide, 4); Z = xalloc((size_t)GPU_BMAX * inner, 4); O = xalloc(GPU_BMAX * o, 4);
        KK = xalloc((size_t)GPU_BMAX * kvd, 4); VV = xalloc((size_t)GPU_BMAX * kvd, 4);
        AL = xalloc((size_t)GPU_BMAX * nv, 4); BE = xalloc((size_t)GPU_BMAX * nv, 4);
        HB = xalloc((size_t)GPU_BMAX * (Hs + 1), 4); HB2 = xalloc((size_t)GPU_BMAX * (Hs + 1), 4);
        OUTS = xalloc((size_t)GPU_BMAX * K * d, 4);
    }
    for (int l = l0; l < l1; l++) {
        const Layer *L = &m->layers[l];
        int prep = 0;
        if (l + 1 < l1) layer_prefetch(m, l + 1);
        if (qn_prof_on) pt0 = now_sec();
        for (int t = 0; t < B; t++) rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->attn_norm, d, c->eps);
        if (L->lin) {
            if (pos0 == 0) dn_reset(m, L);
            matmat(P1, &L->wqkv, Xb, xqs, &prep, B, C, d);
            matmat(Z, &L->wz, Xb, xqs, &prep, B, inner, d);
            matmat(AL, &L->walpha, Xb, xqs, &prep, B, nv, d);
            matmat(BE, &L->wbeta, Xb, xqs, &prep, B, nv, d);
            if (qn_prof_on) pt1 = now_sec();
            for (int t = 0; t < B; t++) {
                float *qkv = P1 + (size_t)t * C, *o = O + (size_t)t * inner;
                dn_conv(c, L, dn_CV(m, L), qkv);
                dn_recur(m, L, qkv, AL + (size_t)t * nv, BE + (size_t)t * nv, o);
                dn_gate(c, L, o, Z + (size_t)t * inner);
                if (qn_ckpt && t < B - 1) memcpy(qn_ckpt + (size_t)t * qn_ckpt_n + (size_t)L->lin_i * dn_state_floats(c), dn_S(m, L), dn_state_floats(c) * sizeof(float));
            }
            if (qn_prof_on) qn_prof[5] += now_sec() - pt1;     /* [5] conv + recorrencia + portao */
            if (qn_prof_on) { pt1 = now_sec(); qn_prof[4] += pt1 - pt0; pt0 = pt1; }     /* [4] projecoes de entrada + conv/recorrencia */
            prep = 0;
            matmat(X2, &L->wssm_out, O, xqs, &prep, B, d, inner);
        } else {
            matmat(P1, &L->wq, Xb, xqs, &prep, B, 2 * qd, d);
            matmat(KK, &L->wk, Xb, xqs, &prep, B, kvd, d);
            matmat(VV, &L->wv, Xb, xqs, &prep, B, kvd, d);
            static int attb = -1;      /* KATANA_QN_ATTB=0: atencao do lote token a token (conferencia) */
            if (attb < 0) attb = getenv("KATANA_QN_ATTB") ? atoi(getenv("KATANA_QN_ATTB")) != 0 : 1;
            if (attb && B > 1) qn_core_n(m, L, l, pos0, B, P1, KK, VV, O);
            else
                for (int t = 0; t < B; t++)
                    qn_core(m, L, l, pos0 + t, P1 + (size_t)t * 2 * qd, KK + (size_t)t * kvd, VV + (size_t)t * kvd, O + (size_t)t * qd);
            prep = 0;
            matmat(X2, &L->wo, O, xqs, &prep, B, d, qd);
        }
        for (size_t i = 0; i < (size_t)B * d; i++) X[i] += X2[i];
        if (qn_prof_on) { pt1 = now_sec(); qn_prof[0] += pt1 - pt0; pt0 = pt1; }     /* [0] camada de atencao/DeltaNet inteira */
        /* feed-forward: roteamento por token, todos os pares do lote de uma vez, compartilhado em lote */
        for (int t = 0; t < B; t++) rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->ffn_norm, d, c->eps);
        if (layer_tap) for (int t = 0; t < B; t++) layer_tap(l, pos0 + t, Xb + (size_t)t * d);
        static int ex[GPU_BMAX * 64], xi[GPU_BMAX * 64]; static float ew[GPU_BMAX * 64];
        for (int t = 0; t < B; t++) {
            memcpy(m->xb, Xb + (size_t)t * d, d * sizeof(float));
            moe_route(m, L, l);
            for (int j = 0; j < K; j++) { ex[t * K + j] = moe.sel[j]; xi[t * K + j] = t; ew[t * K + j] = moe.w[j]; }
        }
        if (qn_prof_on) { pt1 = now_sec(); qn_prof[1] += pt1 - pt0; pt0 = pt1; }     /* [1] normas + roteador */
        moe_cpu_multi(m, L, ex, xi, B * K, B, Xb, OUTS);
        if (qn_prof_on) { pt1 = now_sec(); qn_prof[2] += pt1 - pt0; pt0 = pt1; }     /* [2] especialistas */
        for (int t = 0; t < B; t++) {
            float *x = X + (size_t)t * d;
            for (int j = 0; j < K; j++) {
                const float w = ew[t * K + j], *o = OUTS + (size_t)(t * K + j) * d;
                for (int i = 0; i < d; i++) x[i] += w * o[i];
            }
        }
        if (Hs > 0) {
            prep = 0;
            matmat(HB, &L->sg, Xb, xqs, &prep, B, Hs, d);
            matmat(HB2, &L->su, Xb, xqs, &prep, B, Hs, d);
            for (size_t i = 0; i < (size_t)B * Hs; i++) { const float g = HB[i]; HB[i] = g / (1.0f + expf(-g)) * HB2[i]; }
            prep = 0;
            matmat(X2, &L->sd, HB, xqs, &prep, B, d, Hs);
            for (int t = 0; t < B; t++) {
                const float *xb = Xb + (size_t)t * d, *y = X2 + (size_t)t * d;
                const float sw = L->shexp_gate ? 1.0f / (1.0f + expf(-dot(L->shexp_gate, xb, d))) : 1.0f;
                float *x = X + (size_t)t * d;
                for (int i = 0; i < d; i++) x[i] += sw * y[i];
            }
        }
        if (qn_prof_on) { pt1 = now_sec(); qn_prof[3] += pt1 - pt0; pt0 = pt1; }     /* [3] soma dos especialistas + compartilhado */
    }
}

/* gemma4: logits finais limitados por softcap * tanh(logit / softcap) */
static void logit_softcap(const Model *m, float *lg) {
    const float s = m->cfg.softcap;
    if (lg && m->cfg.logit_scale > 0.0f) for (int i = 0; i < m->cfg.vocab; i++) lg[i] /= m->cfg.logit_scale;     /* granitehybrid */
    if (!lg || s <= 0.0f) return;
    for (int i = 0; i < m->cfg.vocab; i++) lg[i] = s * tanhf(lg[i] / s);
}

/* norma final + projecao para o vocabulario, na CPU */
static float *cpu_head(Model *m) {
    const Config *c = &m->cfg;
    if (!c->q4e) rmsnorm(m->x, m->x, m->out_norm, c->dim, c->eps);      /* qwen4exp: o mixer final ja normaliza */
    matvec(m->logits, &m->lm_head, m->x, c->vocab, c->dim);
    logit_softcap(m, m->logits);
    return m->logits;
}

/* decodificacao especulativa por consulta ao contexto (engine/lookup.c): -1 ainda nao lido, 0 desligada */
static int lk_on_ = -1, lk_pend_;
static void lk_hook(Model *m, const int *toks, int n, int pos0);
static void lk_init(Model *m);
static void lk_finish(Model *m);
static float *lk_forward(Model *m, int token, int pos);
static void lk_report(void);
/* as camadas da GPU so sao usadas pelo grafo da GPU; na CPU os produtos sao sempre locais */
/* o que esta no cache KV (ids por linha) e ate onde o estado recorrente foi: so com as fotos do estado ligadas (kvt.on,
   agent/snapshots.c). Todo token processado passa por forward ou forward_seq */
static struct { int on, n, cap, bad, cpu, top, dry; int *ids; void (*cut)(int pos0); } kvt;
static void kvt_note(const int *toks, int n, int pos0) {
    const int cpu = gpu.force_cpu != 0;
    if (pos0 == 0) { kvt.bad = 0; kvt.cpu = cpu; }
    else if (pos0 != kvt.n || cpu != kvt.cpu) kvt.bad = 1;      /* o estado nao e o desta posicao: sem fotos daqui em diante */
    if (pos0 < kvt.top) kvt.cut(pos0);
    for (int i = 0; i < n && pos0 + i < kvt.cap; i++) kvt.ids[pos0 + i] = toks[i];
    kvt.n = pos0 + n;
}
/* linhas carregadas de fora (disco): ids NULL = desconhecidas */
static void kvt_load(const int *ids, int n) {
    if (!kvt.on) return;
    if (kvt.top) kvt.cut(0);
    kvt.n = 0; kvt.bad = 1;
    if (ids && n <= kvt.cap) { memcpy(kvt.ids, ids, sizeof(int) * (size_t)n); kvt.n = n; kvt.bad = 0; kvt.cpu = gpu.force_cpu != 0; }
}
static float *forward(Model *m, int token, int pos, int want_logits) {
    if (lk_on_) lk_hook(m, &token, 1, pos);
    if (kvt.on) { kvt_note(&token, 1, pos); if (kvt.dry) { m->logits[m->cfg.eos] = kvt.dry++ % 390 ? -5.0f : 5.0f; return m->logits; } }
    const Config *c = &m->cfg;
    if (vis_rp || token >= VIS_BASE) vis_seq(m, &token, 1, pos);
    if (c->q4e) return q4_forward(m, &token, 1, pos, want_logits);
    if (c->gr) { embed_row(m, m->x, token); grh_layers_n(m, 0, c->n_layers, pos, 1, m->x); return want_logits ? cpu_head(m) : NULL; }
    if (vkg.on && !gpu.force_cpu) return vk_forward_1(m, token, pos, want_logits);
    if (gpu.n_gl > 0 && !gpu.force_cpu) return gpu_forward_n(m, &gpu.set[gpu.n_sets - 1], &token, pos, want_logits);
    /* gemma4 (modo hibrido): sem grafo gravado proprio; o lote da GPU (vk_g4_n) aceita B=1, mas o produto
       cooperativo dele e feito para lotes maiores - medido no E2B, token a token fica mais lento assim
       (~17 tok/s) do que deixando cpu_layers/g4_layer, que ja faz os mesmos produtos com vk_matvecs (ida e
       volta mais leve, pensado para B=1): ~36 tok/s. Por isso o padrao e NAO usar vk_g4_n na decodificacao;
       KATANA_VK_G4DEC=1 liga (so o prompt, em lote, sempre usa vk_g4_n). */
    if (vkg4.ok && !gpu.force_cpu && getenv("KATANA_VK_G4DEC") && atoi(getenv("KATANA_VK_G4DEC")) != 0)
        return vk_g4_n(m, &token, 1, pos, want_logits);
    embed_row(m, m->x, token);
    if (c->g4 && c->ple_dim) g4_ple_prep(m, token, m->x, m->g4_ple);
    cpu_layers(m, 0, c->n_layers, pos);
    return want_logits ? cpu_head(m) : NULL;
}

/* Vulkan: menor lote que compensa e maior lote por passada (KATANA_VK_MINB, KATANA_VK_CAP ou KATANA_VK_PASS) */
static int vk_batch_min(void) { return getenv("KATANA_VK_MINB") ? atoi(getenv("KATANA_VK_MINB")) : 2; }
static int vk_pass_cap(void) {
    const int cap = getenv("KATANA_VK_CAP") ? atoi(getenv("KATANA_VK_CAP")) : vkb.P;
    return cap >= 1 && cap < vkb.P ? cap : vkb.P;
}

/* qwen35moe: prompt em lote na CPU (KATANA_QN_BATCH=0 desliga; com a GPU, o padrao e token a token) */
static int qn_batch(const Model *m) {
    static int v = -2;
    if (vkg.on && !gpu.force_cpu) return 0;     /* grafo Vulkan: o estado (DeltaNet, cache KV) vive na VRAM */
    if (v == -2) v = getenv("KATANA_QN_BATCH") ? atoi(getenv("KATANA_QN_BATCH")) : -1;
    if (v >= 0) return v;
    return !(gpu.mv_host && !gpu.force_cpu);
}

/* gemma4: prompt em lote na CPU (KATANA_G4_BATCH=0 desliga, 1 forca) */
static int g4_batch(const Model *m) {
    static int v = -2;
    (void)m;
    if (v == -2) v = getenv("KATANA_G4_BATCH") ? atoi(getenv("KATANA_G4_BATCH")) : -1;
    if (v >= 0) return v;
    return 1;
}

/* processa n tokens seguidos a partir de pos0 (em lotes, na GPU); logits do ultimo se pedido */
static float *forward_seq_in(Model *m, const int *toks, int n, int pos0, int want_logits);
static float *forward_seq(Model *m, const int *toks, int n, int pos0, int want_logits) {
    if (!kvt.on) return forward_seq_in(m, toks, n, pos0, want_logits);
    kvt_note(toks, n, pos0);
    if (kvt.dry) { m->logits[m->cfg.eos] = -5.0f; kvt.dry = 1; return m->logits; }      /* katana snapreplay: so as contas, sem modelo */
    kvt.on = 0;         /* ja anotado: os forward de dentro nao repetem */
    float *lg = forward_seq_in(m, toks, n, pos0, want_logits);
    kvt.on = 1;
    return lg;
}
static float *forward_seq_in(Model *m, const int *toks, int n, int pos0, int want_logits) {
    float *lg = NULL;
    int i = 0;
    if (lk_on_) lk_hook(m, toks, n, pos0);
    vis_seq(m, toks, n, pos0);      /* sem imagem nenhuma, nao faz nada */
    if (m->cfg.q4e) return q4_forward(m, toks, n, pos0, want_logits);
    if (m->cfg.gr) {
        const int d = m->cfg.dim;
        static float *X;
        if (!X) X = xalloc((size_t)GPU_BMAX * d, sizeof(float));
        while (i < n) {
            const int B = n - i < GPU_BMAX ? n - i : GPU_BMAX;
            for (int t = 0; t < B; t++) embed_row(m, X + (size_t)t * d, toks[i + t]);
            grh_layers_n(m, 0, m->cfg.n_layers, pos0 + i, B, X);
            i += B;
        }
        memcpy(m->x, X + (size_t)((n - 1) % GPU_BMAX) * d, d * sizeof(float));
        return want_logits ? cpu_head(m) : NULL;
    }
    if (vkml.ok && !gpu.force_cpu && vk_batch_min() <= n) {      /* MLA em Vulkan (modo hibrido): lote proprio */
        while (n - i >= vk_batch_min()) {
            const int nb = n - i < vkml.P ? n - i : vkml.P;
            lg = vk_mla_n(m, toks + i, nb, pos0 + i, want_logits && i + nb == n);
            i += nb;
        }
        for (; i < n; i++) lg = forward(m, toks[i], pos0 + i, want_logits && i == n - 1);
        return lg;
    }
    if (vkg.on && vkb.ok && !gpu.force_cpu && vk_batch_min() <= n) {
        /* lotes de ate vkb.P tokens; sobra menor que o minimo vai um token por vez */
        while (n - i >= vk_batch_min()) {
            const int nb = n - i < vk_pass_cap() ? n - i : vk_pass_cap();
            lg = vk_forward_n(m, toks + i, nb, pos0 + i, want_logits && i + nb == n);
            i += nb;
        }
        for (; i < n; i++) lg = forward(m, toks[i], pos0 + i, want_logits && i == n - 1);
        return lg;
    }
    if (gpu.n_gl > 0 && !gpu.force_cpu) {
        while (i < n) {
            const KSet *ks = &gpu.set[gpu.n_sets - 1];
            if (!gx.on || !getenv("KATANA_MOE_NOBATCH"))
                for (int s = 0; s < gpu.n_sets; s++) if (gpu.set[s].nb <= n - i) { ks = &gpu.set[s]; break; }
            lg = gpu_forward_n(m, ks, toks + i, pos0 + i, want_logits && i + ks->nb == n);
            i += ks->nb;
        }
        return lg;
    }
    if (vkg4.ok && !gpu.force_cpu && vk_batch_min() <= n) {       /* gemma4 em Vulkan (modo hibrido): lote proprio */
        while (n - i >= vk_batch_min()) {
            const int nb = n - i < vkg4.P ? n - i : vkg4.P;
            lg = vk_g4_n(m, toks + i, nb, pos0 + i, want_logits && i + nb == n);
            i += nb;
        }
        for (; i < n; i++) lg = forward(m, toks[i], pos0 + i, want_logits && i == n - 1);
        return lg;
    }
    if (m->cfg.g4 && n > 1 && g4_batch(m)) {
        const int d = m->cfg.dim;
        static float *X;
        if (!X) X = xalloc((size_t)G4_NB * d, sizeof(float));
        int last = 0;
        while (i < n) {
            const int B = n - i < G4_NB ? n - i : G4_NB;
            for (int t = 0; t < B; t++) {
                embed_row(m, X + (size_t)t * d, toks[i + t]);
                if (m->cfg.ple_dim) g4_ple_prep(m, toks[i + t], X + (size_t)t * d, m->g4_ple + (size_t)t * m->cfg.n_layers * m->cfg.ple_dim);
            }
            g4_layers_n(m, pos0 + i, B, X);
            last = B - 1; i += B;
        }
        memcpy(m->x, X + (size_t)last * d, d * sizeof(float));
        return want_logits ? cpu_head(m) : NULL;
    }
    if (m->cfg.ssm_inner && m->cfg.n_expert && n > 1 && qn_batch(m)) {
        const int d = m->cfg.dim;
        static float *X;
        if (!X) X = xalloc((size_t)GPU_BMAX * d, sizeof(float));
        while (i < n) {
            const int B = n - i < GPU_BMAX ? n - i : GPU_BMAX;
            for (int t = 0; t < B; t++) embed_row(m, X + (size_t)t * d, toks[i + t]);
            qn_layers_n(m, 0, m->cfg.n_layers, pos0 + i, B, X);
            i += B;
        }
        memcpy(m->x, X + (size_t)((n - 1) % GPU_BMAX) * d, d * sizeof(float));
        return want_logits ? cpu_head(m) : NULL;
    }
    if (vkqd.ok && gpu.mv_host && !gpu.force_cpu && n > 1) {      /* qwen35 denso hibrido: produtos do lote na GPU */
        while (i < n) {
            const int nb = n - i < vkqd.P ? n - i : vkqd.P;
            lg = vk_qd_n(m, toks + i, nb, pos0 + i, want_logits && i + nb == n);
            i += nb;
        }
        return lg;
    }
    for (; i < n; i++) lg = forward(m, toks[i], pos0 + i, want_logits && i == n - 1);
    return lg;
}

/* reuso de prefixo: o cache guarda n tokens, o pedido novo (nnew tokens) repete os primeiros `common`; devolve quantas
   posicoes do cache valem (0 = comecar do zero). gemma4: as camadas locais guardam um anel de kv_cap posicoes; voltar
   so vale se nenhuma posicao que a janela do proximo token precisa foi sobrescrita (a posicao p ainda esta no anel
   se p >= n - kv_cap) */
static int cache_rewind(const Model *m, int n, int common, int nnew) {
    if (common >= nnew) common = nnew - 1;              /* precisa de pelo menos um token para obter os logits */
    if (m->cfg.ssm_inner && common != n) return 0;      /* DeltaNet: o estado so continua do fim */
    if (m->cfg.g4) for (int l = 0; l < m->cfg.n_layers; l++) {
        const Layer *L = &m->layers[l];
        const int low = L->swa && common - m->cfg.swa_win + 1 > 0 ? common - m->cfg.swa_win + 1 : 0;
        if (n > L->kv_cap && low < n - L->kv_cap) return 0;
    }
    return common;
}

/* ================= amostragem (temperatura + top-k + top-p) =================
 * g_top_k: quantos candidatos entram no sorteio; g_top_p: fica so o menor grupo de candidatos cuja probabilidade
 * somada chega a g_top_p (1 = desligado). sample_preset escolhe os dois pela familia do modelo, com os valores
 * que quem treinou o modelo recomenda. KATANA_TOP_K e KATANA_TOP_P trocam (para medir). */
static int g_top_k = 40;
static float g_top_p = 1.0f;

static void sample_preset(const char *arch) {
    if (strncmp(arch, "gemma", 5) == 0) { g_top_k = 64; g_top_p = 0.95f; }
    else if (strncmp(arch, "qwen4", 5) == 0) { g_top_k = 20; g_top_p = 0.95f; }
    else if (strncmp(arch, "qwen", 4) == 0) { g_top_k = 20; g_top_p = 0.8f; }
    else { g_top_k = 40; g_top_p = 0.95f; }
    if (getenv("KATANA_TOP_K")) g_top_k = atoi(getenv("KATANA_TOP_K"));
    if (getenv("KATANA_TOP_P")) g_top_p = (float)atof(getenv("KATANA_TOP_P"));
    if (g_top_k < 1) g_top_k = 1;
    if (g_top_k > TOP_K) g_top_k = TOP_K;
    if (g_top_p <= 0.0f || g_top_p > 1.0f) g_top_p = 1.0f;
}

static int sample(const float *logits, int n, float temp) {
    int idx[TOP_K]; float val[TOP_K]; int k = 0;
    const int TK = smp_bypass(temp) ? TOP_K : g_top_k;
    if (temp <= 0.0f) {
        int best = 0;
        for (int i = 1; i < n; i++) if (logits[i] > logits[best]) best = i;
        return best;
    }
    for (int i = 0; i < n; i++) {   /* mantem os TOP_K maiores, em ordem decrescente */
        if (k == TK && logits[i] <= val[k - 1]) continue;
        int j = k < TK ? k++ : k - 1;
        while (j > 0 && val[j - 1] < logits[i]) { val[j] = val[j - 1]; idx[j] = idx[j - 1]; j--; }
        val[j] = logits[i]; idx[j] = i;
    }
    for (int i = 0; i < k; i++) val[i] /= temp;
    softmax(val, k);
    if (g_top_p < 1.0f && !smp_bypass(temp)) {       /* nucleo: corta a cauda e renormaliza */
        float cum = 0.0f; int kk = 0;
        while (kk < k) { cum += val[kk++]; if (cum >= g_top_p) break; }
        for (int i = 0; i < kk; i++) val[i] /= cum;
        k = kk;
    }
    float r = smp_rand01(), acc = 0.0f;
    for (int i = 0; i < k; i++) { acc += val[i]; if (r < acc) return idx[i]; }
    return idx[k - 1];
}

/* ================= saida estruturada: gramatica JSON =================
 * Automato de pilha que le o texto gerado byte a byte e diz se ainda pode virar um JSON valido
 * (opcionalmente conforme um JSON Schema simples: object/properties/required/string/number/integer/
 * boolean/array/items/enum/const). O amostrador testa os candidatos em ordem de logit e fica com o
 * primeiro cujo texto o automato aceita; so cai na varredura do vocabulario inteiro se nenhum dos
 * melhores servir. Dentro do agente, a gramatica de chamada de ferramenta liga quando o modelo abre
 * <tool_call> (JSON: nome da ferramenta e argumentos conforme o schema dela; XML: nome da funcao). */
enum { GN_ANY, GN_OBJ, GN_ARR, GN_STR, GN_NUM, GN_INT, GN_BOOL, GN_NULL, GN_ENUM, GN_DISP };
enum { GF_VAL, GF_OBJ, GF_ARR, GF_STR, GF_NUM, GF_LIT, GF_ENUM, GF_KEY };
#define GN_MAXN 512
#define GN_MAXP 48
#define GN_MAXE 128
#define GN_DEPTH 48
#define GN_WS 24
#define GN_ARENA (1 << 19)

typedef struct {
    int type, np, ne, items, ordered;
    char **pk; int *pn;             /* propriedades: nome e no do valor */
    uint64_t req;                   /* mascara das obrigatorias */
    char **ev; int *dn;             /* enum: texto JSON de cada valor; despacho: no por opcao */
} GNode;
typedef struct { uint8_t t, ph, x, aux; int16_t node, pos, k; uint64_t used, m[2]; } GFrame;
typedef struct { int sp, ws; GFrame f[GN_DEPTH]; } GState;

static struct {
    int on, err, nn, root, tool, stop2, V, left;
    GNode n[GN_MAXN];
    char *arena; size_t au;
    GState s;
    char *tt; uint32_t *toff; int *tlen;     /* texto de cada token do vocabulario */
    int tt_ok;
} g_gr;

static void *gr_alloc(size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (!g_gr.arena) g_gr.arena = xalloc(GN_ARENA, 1);
    if (g_gr.au + n > GN_ARENA) { g_gr.err = 1; return NULL; }
    void *p = g_gr.arena + g_gr.au; g_gr.au += n;
    memset(p, 0, n);
    return p;
}
static char *gr_dup(const char *s, size_t n) {
    char *p = gr_alloc(n + 1);
    if (p) memcpy(p, s, n);
    return p;
}
static int gn_new(int type) {
    if (g_gr.nn >= GN_MAXN) { g_gr.err = 1; return 0; }
    memset(&g_gr.n[g_gr.nn], 0, sizeof(GNode));
    g_gr.n[g_gr.nn].type = type;
    return g_gr.nn++;
}
static void gr_reset(void) {
    g_gr.err = 0; g_gr.au = 0; g_gr.nn = 1; g_gr.tool = 0; g_gr.on = 0;
    memset(&g_gr.n[0], 0, sizeof(GNode));
    g_gr.n[0].type = GN_ANY;
}

static const char *gs_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++; return p; }
/* fim do valor JSON que comeca em p; NULL se malformado */
static const char *gs_skipv(const char *p) {
    p = gs_ws(p);
    if (*p == '"') { for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++; return *p ? p + 1 : NULL; }
    if (*p == '{' || *p == '[') {
        int d = 0;
        for (; *p; p++) {
            if (*p == '"') { p = gs_skipv(p); if (!p) return NULL; p--; continue; }
            if (*p == '{' || *p == '[') d++;
            else if ((*p == '}' || *p == ']') && --d == 0) return p + 1;
        }
        return NULL;
    }
    const char *q = p;
    while (*q && !strchr(",}] \n\t\r", *q)) q++;
    return q > p ? q : NULL;
}
/* proximo membro de um objeto: p vem logo apos '{' ou apos o valor anterior; devolve o fim do valor */
static const char *gs_member(const char *p, char *key, size_t kc, const char **val) {
    p = gs_ws(p);
    if (*p == ',') p = gs_ws(p + 1);
    if (*p != '"') return NULL;
    size_t n = 0;
    for (p++; *p && *p != '"'; p++) { if (*p == '\\' && p[1]) p++; if (n + 1 < kc) key[n++] = *p; }
    key[n] = 0;
    if (*p != '"') return NULL;
    p = gs_ws(p + 1);
    if (*p != ':') return NULL;
    *val = gs_ws(p + 1);
    return gs_skipv(*val);
}
static const char *gs_elem(const char *p, const char **val) {
    p = gs_ws(p);
    if (*p == ',') p = gs_ws(p + 1);
    if (!*p || *p == ']' || *p == '}') return NULL;
    *val = p;
    return gs_skipv(p);
}
static int gs_typename(const char *q) {
    static const char *nm[] = { "object", "array", "string", "number", "integer", "boolean", "null" };
    static const int ty[] = { GN_OBJ, GN_ARR, GN_STR, GN_NUM, GN_INT, GN_BOOL, GN_NULL };
    if (*q == '"') q++;
    for (int i = 0; i < 7; i++) if (strncmp(q, nm[i], strlen(nm[i])) == 0) return ty[i];
    return -1;
}

/* constroi o no do schema que comeca em v; 0 (= qualquer valor) se nao entendido */
static int gn_build(const char *v, int depth) {
    if (g_gr.err || depth > 10 || !v || *v != '{') return 0;
    char key[64];
    const char *val, *p = v + 1, *e, *props = NULL, *req = NULL, *items = NULL, *en = NULL, *cst = NULL;
    int type = -1;
    while ((e = gs_member(p, key, sizeof key, &val))) {
        if (!strcmp(key, "type")) {
            if (*val == '"') type = gs_typename(val);
            else if (*val == '[') { const char *ev; if (gs_elem(val + 1, &ev) && strncmp(ev, "\"null\"", 6) != 0) type = gs_typename(ev); }
        }
        else if (!strcmp(key, "properties")) props = val;
        else if (!strcmp(key, "required")) req = val;
        else if (!strcmp(key, "items")) items = val;
        else if (!strcmp(key, "enum")) en = val;
        else if (!strcmp(key, "const")) cst = val;
        p = e;
    }
    if (en || cst) {
        char *tmp[GN_MAXE]; int ne = 0;
        if (cst) { const char *ce = gs_skipv(cst); if (ce) tmp[ne++] = gr_dup(cst, (size_t)(ce - cst)); }
        else if (*en == '[') {
            const char *ev, *q = en + 1;
            while (ne < GN_MAXE && (q = gs_elem(q, &ev))) tmp[ne++] = gr_dup(ev, (size_t)(q - ev));
        }
        if (g_gr.err || ne == 0) return 0;
        const int id = gn_new(GN_ENUM);
        g_gr.n[id].ev = gr_alloc(sizeof(char *) * (size_t)ne);
        if (!g_gr.n[id].ev) return 0;
        memcpy(g_gr.n[id].ev, tmp, sizeof(char *) * (size_t)ne);
        g_gr.n[id].ne = ne;
        return id;
    }
    if (type == GN_OBJ || (type < 0 && props)) {
        const int id = gn_new(GN_OBJ);
        if (props && *props == '{') {
            char *names[GN_MAXP]; int nodes[GN_MAXP], np = 0;
            const char *q = props + 1, *pv, *pe;
            char k2[96];
            while (np < GN_MAXP && (pe = gs_member(q, k2, sizeof k2, &pv))) {
                names[np] = gr_dup(k2, strlen(k2));
                nodes[np] = gn_build(pv, depth + 1);
                np++; q = pe;
            }
            if (g_gr.err) return 0;
            g_gr.n[id].pk = gr_alloc(sizeof(char *) * (size_t)(np + 1));
            g_gr.n[id].pn = gr_alloc(sizeof(int) * (size_t)(np + 1));
            if (!g_gr.n[id].pk || !g_gr.n[id].pn) return 0;
            memcpy(g_gr.n[id].pk, names, sizeof(char *) * (size_t)np);
            memcpy(g_gr.n[id].pn, nodes, sizeof(int) * (size_t)np);
            g_gr.n[id].np = np;
            if (req && *req == '[') {
                const char *ev, *r2 = req + 1;
                while ((r2 = gs_elem(r2, &ev))) {
                    if (*ev != '"') continue;
                    for (int i = 0; i < np; i++)
                        if (strncmp(ev + 1, names[i], strlen(names[i])) == 0 && ev[1 + strlen(names[i])] == '"') g_gr.n[id].req |= 1ULL << i;
                }
            }
        }
        return id;
    }
    if (type == GN_ARR) {
        const int id = gn_new(GN_ARR);
        const int it = items ? gn_build(items, depth + 1) : 0;
        g_gr.n[id].items = it;
        return id;
    }
    if (type == GN_STR || type == GN_NUM || type == GN_INT || type == GN_BOOL || type == GN_NULL) return gn_new(type);
    return 0;
}

/* ---- passo do automato ---- */
static int gn_isws(unsigned c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }
static GFrame *gn_push(GState *s, int t, int node) {
    if (s->sp >= GN_DEPTH) return NULL;
    GFrame *f = &s->f[s->sp++];
    memset(f, 0, sizeof *f);
    f->t = (uint8_t)t; f->node = (int16_t)node; f->k = -1;
    return f;
}
static int gn_popcnt(uint64_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

/* abre uma chave de objeto (apos o '"'): com propriedades declaradas casa so as permitidas */
static int gn_open_key(const GNode *nd, GState *s, int fi) {
    GFrame *f = &s->f[fi];
    f->ph = 1;
    if (nd->np > 0) {
        uint64_t cand;
        if (nd->ordered) { const int u = gn_popcnt(f->used); cand = u < nd->np ? 1ULL << u : 0; }
        else cand = (nd->np >= 64 ? ~0ULL : (1ULL << nd->np) - 1) & ~f->used;
        if (!cand) return 0;
        const int node = f->node;
        GFrame *k = gn_push(s, GF_KEY, node);
        if (!k) return 0;
        k->m[0] = cand;
        return 1;
    }
    return gn_push(s, GF_STR, 0) != NULL;
}

/* consome o byte c; 0 = o texto deixaria de poder virar JSON valido (o estado fica indefinido) */
static int gn_step(GState *s, unsigned char c) {
    const int ws = gn_isws(c);
    for (;;) {
        if (s->sp == 0) return 0;
        GFrame *f = &s->f[s->sp - 1];
        const GNode *nd = &g_gr.n[f->node];
        switch (f->t) {
        case GF_VAL: {
            if (ws) { if (++s->ws > GN_WS) return 0; return 1; }
            s->ws = 0;
            const int T = nd->type;
            if (T == GN_ENUM) {
                f->t = GF_ENUM; f->pos = 0;
                f->m[0] = nd->ne >= 64 ? ~0ULL : (1ULL << nd->ne) - 1;
                f->m[1] = nd->ne > 64 ? (nd->ne >= 128 ? ~0ULL : (1ULL << (nd->ne - 64)) - 1) : 0;
                continue;
            }
            if (c == '{' && (T == GN_OBJ || T == GN_ANY)) { f->t = GF_OBJ; f->ph = 0; return 1; }
            if (c == '[' && (T == GN_ARR || T == GN_ANY)) { f->t = GF_ARR; f->ph = 0; return 1; }
            if (c == '"' && (T == GN_STR || T == GN_ANY || (T == GN_ARR && nd->ordered))) { f->t = GF_STR; f->aux = 0; return 1; }   /* array com ordered: tambem aceita texto (TodoWrite em linhas) */
            if ((c == '-' || (c >= '0' && c <= '9')) && (T == GN_NUM || T == GN_INT || T == GN_ANY)) {
                f->t = GF_NUM; f->ph = 0; f->x = T == GN_INT; continue;
            }
            if ((c == 't' || c == 'f') && (T == GN_BOOL || T == GN_ANY)) { f->t = GF_LIT; f->node = c == 't' ? 0 : 1; f->pos = 1; return 1; }
            if (c == 'n' && (T == GN_NULL || T == GN_ANY)) { f->t = GF_LIT; f->node = 2; f->pos = 1; return 1; }
            return 0;
        }
        case GF_OBJ: {
            if (ws) { if (++s->ws > GN_WS) return 0; return 1; }
            s->ws = 0;
            if (f->ph == 1) {                          /* apos a chave: ':' */
                if (c != ':') return 0;
                f->ph = 2;
                int vn = 0;
                if (nd->np > 0 && f->k >= 0 && f->k < nd->np) {
                    vn = nd->pn[f->k];
                    if (g_gr.n[vn].type == GN_DISP) { const int xi = f->x < g_gr.n[vn].ne ? f->x : 0; vn = g_gr.n[vn].dn[xi]; }
                }
                return gn_push(s, GF_VAL, vn) != NULL;
            }
            if ((f->ph == 0 || f->ph == 2) && c == '}') {
                if ((nd->type == GN_OBJ ? nd->req : 0) & ~f->used) return 0;
                s->sp--; return 1;
            }
            if (f->ph == 2 && c == ',') {              /* so vale virgula se ainda cabe uma chave */
                if (nd->np > 0 && !(nd->ordered ? gn_popcnt(f->used) < nd->np : (f->used & ((nd->np >= 64 ? ~0ULL : (1ULL << nd->np) - 1))) != (nd->np >= 64 ? ~0ULL : (1ULL << nd->np) - 1))) return 0;
                f->ph = 3; return 1;
            }
            if ((f->ph == 0 || f->ph == 3) && c == '"') return gn_open_key(nd, s, s->sp - 1);
            return 0;
        }
        case GF_ARR: {
            if (ws) { if (++s->ws > GN_WS) return 0; return 1; }
            s->ws = 0;
            if ((f->ph == 0 || f->ph == 2) && c == ']') { s->sp--; return 1; }
            if (f->ph == 2 && c == ',') { f->ph = 3; return 1; }
            if (f->ph == 0 || f->ph == 3) {
                const int it = nd->type == GN_ARR ? nd->items : 0;
                f->ph = 2;
                if (!gn_push(s, GF_VAL, it)) return 0;
                continue;
            }
            return 0;
        }
        case GF_STR: {
            s->ws = 0;
            if (f->aux == 0) {
                if (c < 0x20) return 0;
                if (c == '"') { s->sp--; return 1; }
                if (c == '\\') f->aux = 1;
                return 1;
            }
            if (f->aux == 1) {
                if (c == 'u') { f->aux = 14; return 1; }
                if (c && strchr("\"\\/bfnrt", c)) { f->aux = 0; return 1; }
                return 0;
            }
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
            if (--f->aux == 10) f->aux = 0;
            return 1;
        }
        case GF_NUM: {
            s->ws = 0;
            const int dg = c >= '0' && c <= '9', iv = f->x;
            switch (f->ph) {
            case 0: if (c == '-') { f->ph = 1; return 1; } if (c == '0') { f->ph = 2; return 1; } if (dg) { f->ph = 3; return 1; } return 0;
            case 1: if (c == '0') { f->ph = 2; return 1; } if (dg) { f->ph = 3; return 1; } return 0;
            case 2: case 3: case 5:
                if ((f->ph == 3 || f->ph == 5) && dg) return 1;
                if (!iv && c == '.' && f->ph != 5) { f->ph = 4; return 1; }
                if (!iv && (c == 'e' || c == 'E')) { f->ph = 6; return 1; }
                s->sp--; continue;
            case 4: if (dg) { f->ph = 5; return 1; } return 0;
            case 6: if (c == '+' || c == '-') { f->ph = 7; return 1; } if (dg) { f->ph = 8; return 1; } return 0;
            case 7: if (dg) { f->ph = 8; return 1; } return 0;
            default: if (dg) return 1; s->sp--; continue;   /* 8 */
            }
        }
        case GF_LIT: {
            s->ws = 0;
            static const char *lit[] = { "true", "false", "null" };
            const char *t = lit[f->node];
            if (c != (unsigned char)t[f->pos]) return 0;
            if (t[++f->pos] == 0) s->sp--;
            return 1;
        }
        case GF_ENUM: {
            s->ws = 0;
            uint64_t keep[2] = { 0, 0 };
            int longer = 0, full = -1;
            for (int i = 0; i < nd->ne; i++) {
                if (!((f->m[i >> 6] >> (i & 63)) & 1)) continue;
                const char *t = nd->ev[i];
                if ((int)strlen(t) > f->pos && (unsigned char)t[f->pos] == c) keep[i >> 6] |= 1ULL << (i & 63);
            }
            if (!keep[0] && !keep[1]) return 0;
            f->m[0] = keep[0]; f->m[1] = keep[1]; f->pos++;
            for (int i = 0; i < nd->ne; i++) {
                if (!((keep[i >> 6] >> (i & 63)) & 1)) continue;
                if ((int)strlen(nd->ev[i]) == f->pos) full = i; else longer = 1;
            }
            if (full >= 0 && !longer) {
                s->sp--;
                if (s->sp > 0 && s->f[s->sp - 1].t == GF_OBJ) s->f[s->sp - 1].x = (uint8_t)full;
            }
            return 1;
        }
        default: {                                     /* GF_KEY */
            s->ws = 0;
            uint64_t keep = 0;
            for (int i = 0; i < nd->np && i < 64; i++) {
                if (!((f->m[0] >> i) & 1)) continue;
                const int L = (int)strlen(nd->pk[i]);
                const unsigned char e = f->pos < L ? (unsigned char)nd->pk[i][f->pos] : '"';
                if (f->pos <= L && e == c) keep |= 1ULL << i;
            }
            if (!keep) return 0;
            f->m[0] = keep; f->pos++;
            if (c == '"') {
                int idx = 0;
                while (!((keep >> idx) & 1)) idx++;
                s->sp--;
                GFrame *o = &s->f[s->sp - 1];
                o->k = (int16_t)idx; o->used |= 1ULL << idx;
            }
            return 1;
        }
        }
    }
}

/* ---- construcao das gramaticas ---- */
static void gr_start(void) {
    memset(&g_gr.s, 0, sizeof g_gr.s);
    gn_push(&g_gr.s, GF_VAL, g_gr.root);
    g_gr.on = 1;
}
static void gr_off(void) { g_gr.on = 0; g_gr.tool = 0; g_gr.left = 1 << 30; }

/* objeto JSON qualquer */
static void gr_json_any(void) {
    gr_reset();
    g_gr.root = gn_new(GN_OBJ);
    gr_start();
}
/* objeto conforme o JSON Schema em texto; 0 se o schema nao e um objeto JSON */
static int gr_json_schema(const char *schema) {
    gr_reset();
    schema = gs_ws(schema);
    if (*schema != '{' || !gs_skipv(schema)) return 0;
    g_gr.root = gn_build(schema, 0);
    if (g_gr.err) return 0;
    gr_start();
    return 1;
}

/* gramatica de chamada de ferramenta a partir da lista <tools> do prompt de sistema.
 * xml=0: {"name": <ferramenta>, "arguments": <objeto conforme parameters>}; xml=1: so o nome + '>' */
static void gr_tools_scan(const char *sys, const char *pv[], char names[][64], int *nt) {
    const char *t0 = strstr(sys, "<tools>"), *t1 = strstr(sys, "</tools>");
    *nt = 0;
    if (!t0 || !t1) return;
    const char *p = t0 + 7;
    while (p < t1 && *nt < GN_MAXE) {
        p = gs_ws(p);
        if (*p != '{') { while (p < t1 && *p != '\n') p++; if (p >= t1) break; continue; }
        const char *end = gs_skipv(p);
        if (!end) break;
        char key[64]; const char *val, *q = p + 1, *e;
        while ((e = gs_member(q, key, sizeof key, &val))) {
            if (!strcmp(key, "function") && *val == '{') {
                char k2[64]; const char *v2, *q2 = val + 1, *e2;
                names[*nt][0] = 0; pv[*nt] = NULL;
                while ((e2 = gs_member(q2, k2, sizeof k2, &v2))) {
                    if (!strcmp(k2, "name") && *v2 == '"') {
                        size_t n = 0;
                        for (const char *r = v2 + 1; *r && *r != '"' && n < 63; r++) names[*nt][n++] = *r;
                        names[*nt][n] = 0;
                    } else if (!strcmp(k2, "parameters")) pv[*nt] = v2;
                    q2 = e2;
                }
                if (names[*nt][0]) (*nt)++;
            }
            q = e;
        }
        p = end;
    }
}

static int gr_tools(const char *sys, int xml) {
    static const char *pv[GN_MAXE]; static char names[GN_MAXE][64];
    int nt;
    gr_tools_scan(sys, pv, names, &nt);
    gr_reset();
    if (nt == 0) return 0;
    char *ev[GN_MAXE];
    if (xml) {
        for (int i = 0; i < nt; i++) {
            char b[80]; const int n = snprintf(b, sizeof b, "%s>", names[i]);
            ev[i] = gr_dup(b, (size_t)n);
        }
        const int id = gn_new(GN_ENUM);
        g_gr.n[id].ev = gr_alloc(sizeof(char *) * (size_t)nt);
        if (g_gr.err) return 0;
        memcpy(g_gr.n[id].ev, ev, sizeof(char *) * (size_t)nt);
        g_gr.n[id].ne = nt;
        g_gr.root = id;
    } else {
        for (int i = 0; i < nt; i++) {
            char b[80]; const int n = snprintf(b, sizeof b, "\"%s\"", names[i]);
            ev[i] = gr_dup(b, (size_t)n);
        }
        const int root = gn_new(GN_OBJ), en = gn_new(GN_ENUM), dp = gn_new(GN_DISP);
        GNode *r = &g_gr.n[root];
        r->ordered = 1; r->np = 2; r->req = 3;
        r->pk = gr_alloc(sizeof(char *) * 2); r->pn = gr_alloc(sizeof(int) * 2);
        g_gr.n[en].ev = gr_alloc(sizeof(char *) * (size_t)nt); g_gr.n[en].ne = nt;
        g_gr.n[dp].dn = gr_alloc(sizeof(int) * (size_t)nt); g_gr.n[dp].ne = nt;
        if (g_gr.err) return 0;
        r->pk[0] = gr_dup("name", 4); r->pk[1] = gr_dup("arguments", 9);
        r->pn[0] = en; r->pn[1] = dp;
        memcpy(g_gr.n[en].ev, ev, sizeof(char *) * (size_t)nt);
        for (int i = 0; i < nt; i++) {
            int a = pv[i] ? gn_build(pv[i], 0) : 0;
            if (g_gr.n[a].type != GN_OBJ) a = gn_new(GN_OBJ);
            g_gr.n[dp].dn[i] = a;
        }
        if (g_gr.err) return 0;
        g_gr.root = root;
    }
    g_gr.tool = 1;
    return 1;
}

/* ---- amostragem restrita ---- */
static size_t tok_bytes(const Tokenizer *tk, int id, char *out, size_t cap);

static void gr_prepare(const Tokenizer *tk, int V) {
    if (g_gr.tt_ok && g_gr.V == V) return;
    free(g_gr.tt); free(g_gr.toff); free(g_gr.tlen);
    g_gr.toff = xalloc((size_t)V, sizeof(uint32_t));
    g_gr.tlen = xalloc((size_t)V, sizeof(int));
    size_t cap = (size_t)V * 8 + 4096, used = 0;
    g_gr.tt = xalloc(cap, 1);
    char b[320];
    for (int t = 0; t < V; t++) {
        int n = 0;
        if (!(tk->toktype && tk->toktype[t] == 3) && tk->tok[t]) {
            n = (int)tok_bytes(tk, t, b, sizeof b);
            if (n >= 250) n = 0;
        }
        if (used + (size_t)n + 1 > cap) { cap = cap * 2 + (size_t)n; g_gr.tt = realloc(g_gr.tt, cap); if (!g_gr.tt) die("sem memoria"); }
        memcpy(g_gr.tt + used, b, (size_t)n);
        g_gr.toff[t] = (uint32_t)used; g_gr.tlen[t] = n;
        used += (size_t)n;
    }
    g_gr.V = V; g_gr.tt_ok = 1;
}

static int gr_done(void) { return g_gr.s.sp == 0; }

/* o token t (texto ja pre-calculado) mantem o JSON valido a partir do estado s? */
static int gr_tok_ok(const GState *s, int t, int *sp_after) {
    const int n = g_gr.tlen[t];
    if (n <= 0) return 0;
    GState tmp;
    memcpy(&tmp, s, offsetof(GState, f) + sizeof(GFrame) * (size_t)s->sp);
    const unsigned char *b = (const unsigned char *)g_gr.tt + g_gr.toff[t];
    for (int i = 0; i < n; i++) if (!gn_step(&tmp, b[i])) return 0;
    if (sp_after) *sp_after = tmp.sp;
    return 1;
}

static void gr_accept(int t) {
    if (!g_gr.on || t < 0 || t >= g_gr.V) return;
    const unsigned char *b = (const unsigned char *)g_gr.tt + g_gr.toff[t];
    for (int i = 0; i < g_gr.tlen[t]; i++) if (!gn_step(&g_gr.s, b[i])) { g_gr.s.sp = 0; break; }
}

#define GR_NC 256
/* token formado so por aspas e fechadores */
static int gr_closer(const GState *s, int t) {
    int sp = s->sp;
    return gr_tok_ok(s, t, &sp) && sp < s->sp;
}
/* primeiro byte do menor valor possivel para o no */
static int gc_valchar(const GNode *nd) {
    switch (nd->type) {
    case GN_OBJ: return '{';
    case GN_ARR: return '[';
    case GN_STR: return '"';
    case GN_NUM: case GN_INT: return '0';
    case GN_BOOL: return 'f';
    case GN_ENUM: return (unsigned char)nd->ev[0][0];
    default: return 'n';
    }
}
/* menor texto que completa o JSON a partir do estado s (fecha a string aberta, completa chave/valor pendente
 * com o minimo e fecha as estruturas na ordem da pilha); devolve o tamanho, ou -1 se nao conseguir */
static int gr_completion(const GState *s, char *out, int cap) {
    GState t;
    memcpy(&t, s, offsetof(GState, f) + sizeof(GFrame) * (size_t)s->sp);
    static const char *lit[] = { "true", "false", "null" };
    int n = 0;
    while (t.sp > 0) {
        if (n + 2 >= cap) return -1;
        GFrame *f = &t.f[t.sp - 1];
        const GNode *nd = &g_gr.n[f->node];
        int c = -1;
        switch (f->t) {
        case GF_VAL: c = gc_valchar(nd); break;
        case GF_OBJ: {
            const uint64_t miss = nd->type == GN_OBJ ? nd->req & ~f->used : 0;
            if (f->ph == 1) c = ':';
            else if (f->ph == 2) c = miss ? ',' : '}';
            else if (f->ph == 0) c = miss ? '"' : '}';
            else c = '"';
            break;
        }
        case GF_ARR: c = (f->ph == 0 || f->ph == 2) ? ']' : gc_valchar(&g_gr.n[nd->type == GN_ARR ? nd->items : 0]); break;
        case GF_STR: c = f->aux == 0 ? '"' : f->aux == 1 ? 'n' : '0'; break;
        case GF_NUM:
            if (f->ph == 2 || f->ph == 3 || f->ph == 5 || f->ph == 8) { t.sp--; continue; }
            c = '0'; break;
        case GF_LIT: c = lit[f->node][f->pos]; break;
        case GF_ENUM:
            for (int i = 0; i < nd->ne && c < 0; i++) if ((f->m[i >> 6] >> (i & 63)) & 1) c = (unsigned char)nd->ev[i][f->pos];
            break;
        default: {                          /* GF_KEY: casa o nome de uma propriedade (as obrigatorias primeiro) */
            const GNode *on = &g_gr.n[t.f[t.sp - 2].node];
            uint64_t cand = f->m[0] & on->req;
            if (!cand) cand = f->m[0];
            int i = 0;
            while (i < 64 && !((cand >> i) & 1)) i++;
            if (i < 64 && i < nd->np) { const int L = (int)strlen(nd->pk[i]); c = f->pos < L ? (unsigned char)nd->pk[i][f->pos] : '"'; }
        }
        }
        if (c < 0 || !gn_step(&t, (unsigned char)c)) return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

/* elementos pendentes (virgula sem item ainda, chave aberta): fechar isso depois custa lixo semantico */
static int gr_pend(const GState *s) {
    int n = 0;
    for (int i = 0; i < s->sp; i++) {
        const GFrame *f = &s->f[i];
        if ((f->t == GF_OBJ || f->t == GF_ARR) && f->ph == 3) n++;
        else if (f->t == GF_OBJ && f->ph == 1) n++;
    }
    return n;
}
static int gr_newpend(const GState *s, int t) {
    GState tmp;
    memcpy(&tmp, s, offsetof(GState, f) + sizeof(GFrame) * (size_t)s->sp);
    const unsigned char *b = (const unsigned char *)g_gr.tt + g_gr.toff[t];
    for (int i = 0; i < g_gr.tlen[t]; i++) if (!gn_step(&tmp, b[i])) return 0;
    return gr_pend(&tmp) > gr_pend(s);
}

static int gr_sample(const float *lg, int n, float temp, int eos) {
    GState *s = &g_gr.s;
    if (gr_done()) {                       /* gramatica completa: so falta terminar */
        if (g_gr.stop2 >= 0 && g_gr.stop2 < n && lg[g_gr.stop2] > lg[eos]) return g_gr.stop2;
        return eos;
    }
    unsigned char allow[256];
    for (int b = 0; b < 256; b++) {        /* primeiros bytes que o estado aceita */
        GState tmp;
        memcpy(&tmp, s, offsetof(GState, f) + sizeof(GFrame) * (size_t)s->sp);
        allow[b] = (unsigned char)gn_step(&tmp, (unsigned char)b);
    }
    int near = 0;
    {   /* orcamento acabando: segue o menor texto que completa o JSON (so tokens que sao prefixo dele) */
        char comp[600];
        const int cl = gr_completion(s, comp, sizeof comp);
        near = cl > 0 && g_gr.left <= cl + 20;     /* perto do fim: nao abre virgula/chave nova */
        if (cl > 0 && g_gr.left <= cl + 2) {
            float best = -3.4e38f; int bi = -1;
            for (int t = 0; t < n; t++) {
                const int L = g_gr.tlen[t];
                if (L <= 0 || L > cl || lg[t] <= best || g_gr.tt[g_gr.toff[t]] != comp[0]) continue;
                if (memcmp(g_gr.tt + g_gr.toff[t], comp, (size_t)L) == 0) { best = lg[t]; bi = t; }
            }
            if (bi >= 0) return bi;
        }
    }
    int idx[GR_NC]; float val[GR_NC]; int k = 0;
    for (int i = 0; i < n; i++) {
        if (k == GR_NC && lg[i] <= val[k - 1]) continue;
        int j = k < GR_NC ? k++ : k - 1;
        while (j > 0 && val[j - 1] < lg[i]) { val[j] = val[j - 1]; idx[j] = idx[j - 1]; j--; }
        val[j] = lg[i]; idx[j] = i;
    }
    int cand[TOP_K]; float cv[TOP_K]; int nc = 0;
    const int want = temp <= 0.0f ? 1 : smp_bypass(temp) ? TOP_K : g_top_k;
    /* orcamento acabando: so tokens que fecham as estruturas abertas (para o JSON terminar valido) */
    for (int cl = 0; cl >= 0 && nc == 0; cl--) {
        for (int i = 0; i < k && nc < want; i++) {
            const int t = idx[i];
            if (g_gr.tlen[t] <= 0 || !allow[(unsigned char)g_gr.tt[g_gr.toff[t]]]) continue;
            if (near && gr_newpend(s, t)) continue;
            if (gr_tok_ok(s, t, NULL)) { cand[nc] = t; cv[nc] = val[i]; nc++; }
        }
        if (nc == 0 && cl) {               /* tambem varre o vocabulario atras de um fechador */
            float best = -3.4e38f; int bi = -1;
            for (int t = 0; t < n; t++) {
                if (lg[t] <= best || g_gr.tlen[t] <= 0 || !allow[(unsigned char)g_gr.tt[g_gr.toff[t]]] || !gr_closer(s, t)) continue;
                if (gr_tok_ok(s, t, NULL)) { best = lg[t]; bi = t; }
            }
            if (bi >= 0) return bi;
        }
    }
    if (nc == 0) {                         /* nenhum dos melhores serve: varre o vocabulario */
        for (int pass = near; pass >= 0; pass--) {      /* perto do fim, primeiro sem abrir elemento novo */
            float best = -3.4e38f; int bi = -1;
            for (int t = 0; t < n; t++) {
                if (lg[t] <= best || g_gr.tlen[t] <= 0 || !allow[(unsigned char)g_gr.tt[g_gr.toff[t]]]) continue;
                if (gr_tok_ok(s, t, NULL) && !(pass && gr_newpend(s, t))) { best = lg[t]; bi = t; }
            }
            if (bi >= 0) return bi;
        }
        return eos;
    }
    if (temp <= 0.0f || nc == 1) return cand[0];
    for (int i = 0; i < nc; i++) cv[i] /= temp;
    softmax(cv, nc);
    float r = smp_rand01(), acc = 0.0f;
    for (int i = 0; i < nc; i++) { acc += cv[i]; if (r < acc) return cand[i]; }
    return cand[nc - 1];
}

/* ---- teste de unidade (katana jsontest) ---- */
static int gr_text(const char *txt, int *complete) {
    for (const char *p = txt; *p; p++) if (!gn_step(&g_gr.s, (unsigned char)*p)) return 0;
    if (complete) *complete = gr_done();
    return 1;
}
static int jsontest(void) {
    int fail = 0, n = 0;
#define EXPECT(cond, what) do { n++; if (!(cond)) { fail++; printf("FALHOU: %s\n", what); } } while (0)
#define ACC(txt) do { int cm = 0; gr_json_any(); EXPECT(gr_text(txt, &cm) && cm, "aceita " txt); } while (0)
#define REJ(txt) do { int cm = 0; gr_json_any(); EXPECT(!(gr_text(txt, &cm) && cm), "recusa " txt); } while (0)
    ACC("{}"); ACC("{\"a\":1}"); ACC(" {\"a\": [1, 2.5e-3, true, false, null, \"x\\n\\u00e9\"], \"b\": {\"c\": {}}}");
    ACC("{\"a\":-0.5}"); ACC("{\"a\":[[],[[]]]}");
    REJ("[]"); REJ("{"); REJ("{\"a\":}"); REJ("{\"a\":1,}"); REJ("{a:1}"); REJ("{\"a\":01}"); REJ("{\"a\":tru}");
    REJ("{\"a\":\"x\ny\"}"); REJ("{\"a\":1 \"b\":2}"); REJ("{\"a\":[1,]}"); REJ("{\"a\":1}}"); REJ("{\"a\":\"\\q\"}"); REJ("{\"a\":1.}");
    {   /* prefixos validos continuam possiveis */
        gr_json_any(); EXPECT(gr_text("{\"a\": [1, {\"b\"", NULL) && !gr_done(), "prefixo valido");
    }
    {
        const char *sc = "{\"type\":\"object\",\"properties\":{\"nome\":{\"type\":\"string\"},\"idade\":{\"type\":\"integer\"},"
                         "\"cor\":{\"enum\":[\"azul\",\"verde\"]},\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
                         "\"ok\":{\"type\":\"boolean\"}},\"required\":[\"nome\",\"idade\"]}";
        int cm;
#define SACC(txt) do { cm = 0; EXPECT(gr_json_schema(sc) && gr_text(txt, &cm) && cm, "schema aceita " txt); } while (0)
#define SREJ(txt) do { cm = 0; EXPECT(gr_json_schema(sc) && !(gr_text(txt, &cm) && cm), "schema recusa " txt); } while (0)
        SACC("{\"nome\":\"ana\",\"idade\":30}");
        SACC("{\"idade\":3,\"nome\":\"b\",\"cor\":\"verde\",\"tags\":[\"x\",\"y\"],\"ok\":true}");
        SREJ("{\"nome\":\"ana\"}");                       /* falta idade */
        SREJ("{\"nome\":\"ana\",\"idade\":3.5}");         /* nao inteiro */
        SREJ("{\"nome\":\"ana\",\"idade\":3,\"cor\":\"rosa\"}");
        SREJ("{\"nome\":\"ana\",\"idade\":3,\"extra\":1}");
        SREJ("{\"nome\":\"ana\",\"nome\":\"b\",\"idade\":3}");   /* chave repetida */
        SREJ("{\"nome\":5,\"idade\":3}");
        SREJ("{\"nome\":\"a\",\"idade\":3,\"tags\":[1]}");
        SREJ("{\"nome\":\"a\",\"idade\":3,\"ok\":\"sim\"}");
    }
    {
        const char *sys = "x\n<tools>\n{\"type\": \"function\", \"function\": {\"name\": \"WebSearch\", \"description\": \"d\", \"parameters\": {\"type\": \"object\", \"properties\": {\"query\": {\"type\": \"string\"}}, \"required\": [\"query\"]}}}\n"
                          "{\"type\": \"function\", \"function\": {\"name\": \"Read\", \"description\": \"d\", \"parameters\": {\"type\": \"object\", \"properties\": {\"path\": {\"type\": \"string\"}}, \"required\": [\"path\"]}}}\n</tools>";
        int cm;
        EXPECT(gr_tools(sys, 0), "gramatica de ferramentas");
        gr_start(); cm = 0;
        EXPECT(gr_text("\n{\"name\": \"WebSearch\", \"arguments\": {\"query\": \"x\"}}", &cm) && cm, "chamada valida");
        gr_tools(sys, 0); gr_start(); cm = 0;
        EXPECT(!(gr_text("{\"name\": \"WebSearch\", \"arguments\": {\"path\": \"x\"}}", &cm) && cm), "argumento de outra ferramenta");
        gr_tools(sys, 0); gr_start(); cm = 0;
        EXPECT(gr_text("{\"name\": \"Read\", \"arguments\": {\"path\": \".\"}}", &cm) && cm, "segunda ferramenta");
        gr_tools(sys, 0); gr_start();
        EXPECT(!gr_text("{\"name\": \"nope\"", NULL), "ferramenta inexistente");
        gr_tools(sys, 0); gr_start();
        EXPECT(!gr_text("{\"name\": \"WebSearch\", \"arguments\": {}}", &cm) || !cm, "falta argumento obrigatorio");
        gr_tools(sys, 1); gr_start(); cm = 0;
        EXPECT(gr_text("Read>", &cm) && cm, "xml nome valido");
        gr_tools(sys, 1); gr_start();
        EXPECT(!gr_text("Reed", NULL), "xml nome invalido");
    }
    printf("jsontest: %d testes, %d falhas\n", n, fail);
    return fail != 0;
}

/* amostra com a gramatica ligada (se houver) ou normalmente */
static int sample_c(const float *lg, int n, float temp, int eos) {
    const float *pl = smp_apply(lg, n, temp);        /* sampler (min-p, penalidades...) sobre os logits que a gramatica ja filtra depois */
    const int t = g_gr.on ? gr_sample(pl, n, temp, eos) : sample(pl, n, temp);
    smp_accept(t);
    return t;
}
/* o mesmo sem gramatica (verificacao do rascunho na decodificacao especulativa) */
static int sample_s(const float *lg, int n, float temp) {
    const int t = sample(smp_apply(lg, n, temp), n, temp);
    smp_accept(t);
    return t;
}
/* KATANA_JSON=1: saida JSON; KATANA_JSON='{...schema...}': saida conforme o schema */
static int gr_env_begin(const Tokenizer *tk, int V, int stop2) {
    const char *e = getenv("KATANA_JSON");
    if (!e || !*e || strcmp(e, "0") == 0) return 0;
    gr_prepare(tk, V);
    g_gr.stop2 = stop2;
    if (*e == '{') { if (!gr_json_schema(e)) { fprintf(stderr, "[KATANA_JSON: schema invalido, usando objeto JSON qualquer]\n"); gr_json_any(); } }
    else gr_json_any();
    return 1;
}
static const char *g_agent_sys;          /* agente: lista de ferramentas do prompt de sistema (restringe as chamadas) */

/* ================= comandos ================= */


static void print_info(const Model *m) {
    const Config *c = &m->cfg;
    fprintf(stderr, "arquitetura=%s  parametros=%.1fM  camadas=%d  dim=%d  cabecas=%d (kv=%d)  ffn=%d  vocab=%d  ctx=%d\n",
            c->arch, m->n_params / 1e6, c->n_layers, c->dim, c->n_heads, c->n_kv, c->hidden, c->vocab, c->ctx);
    if (c->n_expert) fprintf(stderr, "MoE: %d especialistas por camada, %d ativos por token\n", c->n_expert, c->n_used);
    if (c->yarn_ext) fprintf(stderr, "aviso: ctx=%d acima do treinado (%d); RoPE estendido por yarn x%.2f, qualidade pode cair\n",
                             c->ctx, c->ctx_train, c->yarn_factor);
    if (m->kq8) {
        size_t na = 0;
        for (int l = 0; l < c->n_layers; l++) na += !m->layers[l].lin;
        fprintf(stderr, "cache KV da CPU: %zu camadas, Q8_0, %.2f GB\n", na, 2.0 * na * c->ctx * c->n_kv * c->hd / 32 * 34 / 1e9);
    }
    if (m->kh || m->kcache) {
        size_t na = 0;
        for (int l = 0; l < c->n_layers; l++) na += !(c->ssm_inner && m->layers[l].lin);
        double gb = 2.0 * na * c->ctx * c->n_kv * c->hd * (m->kh ? 2 : 4) / 1e9;
        if (c->g4) {        /* por camada: locais guardam a janela, globais o contexto */
            gb = 0.0;
            for (int l = 0; l < c->n_layers; l++) gb += 2.0 * m->layers[l].kv_cap * m->layers[l].nkv_l * m->layers[l].hd_l * (m->kh ? 2 : 4) / 1e9;
        }
        fprintf(stderr, "cache KV da CPU: %zu camadas, %s, %.2f GB\n", na, m->kh ? "fp16" : "fp32", gb);
    }
}

/* ---------- decodificacao especulativa por repeticao ----------
 * Rascunho gratis: se os ultimos tokens ja apareceram antes no contexto, aposta que a
 * continuacao se repete e propoe os 3 tokens que vieram depois. O modelo confere os 4
 * (o token atual + 3 rascunhos) numa unica passada em lote; cada rascunho so vale se for
 * exatamente o token que o modelo escolheria naquela posicao, entao a saida e a mesma de
 * gerar um por um. Compensa em texto repetitivo (codigo, citacoes, listas); um controle
 * de acerto desliga a aposta quando ela nao esta pagando. KATANA_SPEC=0 desliga. */
static struct { long steps, drafted, accepted; } spec_stat;

/* procura a ocorrencia anterior mais recente dos ultimos K tokens; devolve os 3 seguintes */
static int find_draft(const int *h, int n, int *draft) {
    const int K = 3;
    if (n < K + 4) return 0;
    for (int j = n - K - 1; j >= 0; j--) {
        if (j + K + 3 > n) continue;
        if (h[j] == h[n - K] && h[j + 1] == h[n - K + 1] && h[j + 2] == h[n - K + 2]) {
            draft[0] = h[j + K]; draft[1] = h[j + K + 1]; draft[2] = h[j + K + 2];
            return 1;
        }
    }
    return 0;
}

static const KSet *spec_set(void) {
    if (!gpu.full || gpu.force_cpu || !gpu.blog4 || gx.on) return NULL;
    for (int i = 0; i < gpu.n_sets; i++) if (gpu.set[i].nb == 4) return &gpu.set[i];
    return NULL;
}

static void moe_report(void) {
    if (!gx.on) return;
    if (getenv("KATANA_VK_GENPROF") && gprof.lay)
        fprintf(stderr, "[geracao MoE: %ld camadas, %ld (%.1f%%) com os %d escolhidos todos nas vagas | leitura bloqueante por camada %.3f ms, roteador na CPU %.3f ms, frios na CPU %.3f ms (medias por camada)]\n",
                gprof.lay, gprof.allhit, 100.0 * gprof.allhit / gprof.lay, m_nused, 1e3 * gprof.t_rd / gprof.lay,
                1e3 * gprof.t_route / gprof.lay, 1e3 * gprof.t_cold / gprof.lay);
    const double uses = (double)(gx.hits + gx.miss);
    fprintf(stderr, "[especialistas: %.1f%% dos usos na VRAM, %.1f%% na CPU (%.2f s) | %llu subidas (%llu conversoes, %llu trocas) em %.2f s, leitura do disco %.2f s]\n",
            100.0 * gx.hits / (uses + 1e-9), 100.0 * gx.cpu_runs / (uses + 1e-9), gx.t_cpu,
            (unsigned long long)gx.loads, (unsigned long long)gx.conv, (unsigned long long)gx.evict, gx.t_up, gx.t_read);
}

static int generate(Model *m, const char *text, int max_tokens, float temp, int echo) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    const int V = c->vocab;
    Ids ids = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    encode(tk, text, &ids);
    if (ids.n == 0) ids_push(&ids, c->bos >= 0 ? c->bos : 0);
    /* medicao de contexto longo: KATANA_BENCH_POS=P comeca o prompt na posicao P sobre um cache KV zerado
       (todo tocado, para a RAM aparecer); a saida nao tem sentido, so o tempo e a memoria contam */
    const int bpos = getenv("KATANA_BENCH_POS") ? atoi(getenv("KATANA_BENCH_POS")) : 0;
    if (bpos > 0) {
        const size_t kvd = (size_t)c->n_kv * c->hd;
        size_t na = 0;
        for (int l = 0; l < c->n_layers; l++) na += !(c->ssm_inner && m->layers[l].lin);
        if (m->kq8) { memset(m->kq8, 0, na * c->ctx * kvd / 32 * 34); memset(m->vq8, 0, na * c->ctx * kvd / 32 * 34); }
        else if (m->kh && !c->g4) { memset(m->kh, 0, na * c->ctx * kvd * 2); memset(m->vh, 0, na * c->ctx * kvd * 2); }
        else if (m->kcache && !c->g4) { memset(m->kcache, 0, na * c->ctx * kvd * 4); memset(m->vcache, 0, na * c->ctx * kvd * 4); }
        fprintf(stderr, "[bench: prompt comeca na posicao %d]\n", bpos);
    }
    if (bpos + ids.n >= c->ctx) die("prompt maior que o contexto");

    srand((unsigned)time(NULL));
    float *lg = NULL, *col = xalloc(V, sizeof(float));
    int pos = 0, gen = 0, n_prompt = ids.n, stop = 0;
    const KSet *ks4 = getenv("KATANA_SPEC") && !strcmp(getenv("KATANA_SPEC"), "0") ? NULL : spec_set();
    double ema = 1.5;       /* media movel de rascunhos aceitos por aposta */
    long probe = 0;
    const int json = gr_env_begin(tk, V, -1);
    if (json) ks4 = NULL;             /* com gramatica nao ha aposta de rascunho */
    if (lk_on_ < 0) lk_init(m);
    if (lk_on_ > 0) ks4 = NULL;       /* KATANA_LOOKUP: o rascunho por consulta ao contexto substitui este */
    double t0 = now_sec();
    lg = forward_seq(m, ids.ids, ids.n, bpos, 1);
    pos = bpos + ids.n;
    double t1 = now_sec();
    if (getenv("KATANA_DUMP_LOGITS")) {     /* grava os logits do fim do prompt (float32) para comparar modos */
        FILE *fl = fopen(getenv("KATANA_DUMP_LOGITS"), "wb");
        if (fl) { fwrite(lg, sizeof(float), V, fl); fclose(fl); }
    }
    if (echo && !json) { fputs(text, stdout); fflush(stdout); }

#define EMIT_TOKEN(t) { if ((t) == c->eos || (tk->toktype && tk->toktype[t] == 3)) { stop = 1; } \
    else { print_token(tk, (t), stdout); fflush(stdout); gen++; ids_push(&ids, (t)); gr_accept(t); } }

    while (!stop && gen < max_tokens && pos < c->ctx) {
        g_gr.left = max_tokens - gen;
        int next = sample_c(lg, V, temp, c->eos), draft[3];
        EMIT_TOKEN(next)
        if (stop) break;
        /* aposta so com rascunho disponivel e taxa de acerto que pague a passada em lote */
        int try_spec = ks4 && pos + 4 < c->ctx && gen + 3 < max_tokens && find_draft(ids.ids, ids.n, draft);
        if (try_spec && ema < 1.0 && (++probe % 8) != 0) try_spec = 0;
        if (!try_spec) { lg = lk_forward(m, next, pos++); continue; }

        int toks[4] = { next, draft[0], draft[1], draft[2] }, ok = 0;
        gpu.want_all = 1;
        const float *all = gpu_forward_n(m, ks4, toks, pos, 1);
        gpu.want_all = 0;
        spec_stat.steps++; spec_stat.drafted += 3;
        int i;
        for (i = 0; i < 3 && !stop && gen < max_tokens; i++) {
            for (int v = 0; v < V; v++) col[v] = all[(size_t)v * 4 + i];
            const int t = sample_s(col, V, temp);
            EMIT_TOKEN(t)
            if (stop) break;
            if (t != toks[i + 1]) {         /* rascunho errado: o modelo segue a partir do token certo */
                pos += i + 1;
                lg = forward(m, t, pos++, 1);
                break;
            }
            ok++;
        }
        spec_stat.accepted += ok;
        ema = 0.8 * ema + 0.2 * ok;
        if (i == 3 && !stop) {              /* os 3 valeram: os logits da ultima posicao seguem o jogo */
            pos += 4;
            for (int v = 0; v < V; v++) m->logits[v] = all[(size_t)v * 4 + 3];
            lg = m->logits;
        }
    }
#undef EMIT_TOKEN
    lk_finish(m);
    gr_off();
    double t2 = now_sec();
    printf("\n");
    fflush(stdout);
    prof_report();
    gen = ids.n - n_prompt;
    fprintf(stderr, "[prompt: %d tokens, %.1f tok/s | geracao: %d tokens, %.1f tok/s]\n",
            n_prompt, n_prompt / (t1 - t0 + 1e-9), gen, gen / (t2 - t1 + 1e-9));
    moe_report();
    heat_save(m);
    if (spec_stat.steps)
        fprintf(stderr, "[especulacao: %ld apostas, %ld de %ld rascunhos aceitos]\n",
                spec_stat.steps, spec_stat.accepted, spec_stat.drafted);
    lk_report();
    free(ids.ids); free(col);
    return 0;
}

/* confere o produto quantizado nativo contra dequantizar + produto em float */
static double worst_int;    /* pior erro do produto inteiro da CPU (quantizacao de 8 bits das ativacoes) */

static void selftest_mat(const char *name, const Mat *W, int rows, int cols, double *worst) {
    if (!W->q) { printf("  %-10s float32 (sem caminho quantizado)\n", name); return; }
    float *x = xalloc(cols, sizeof(float)), *row = xalloc(cols, sizeof(float)), *sx = xalloc(cols / 32 + 1, sizeof(float));
    double maxerr = 0.0, interr = 0.0;
    for (int i = 0; i < cols; i++) x[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    block_sums(sx, x, cols);
    int n = rows < 64 ? rows : 64;
    for (int r = 0; r < n; r++) {
        size_t i = (size_t)r * (rows / n);
        mat_row(row, W, (int)i, cols);
        double ref = 0.0, mag = 0.0;
        for (int j = 0; j < cols; j++) { ref += (double)row[j] * x[j]; mag += fabs((double)row[j] * x[j]); }
        cpu_exact = 1;
        double err = fabs(dot_row(W, i, x, sx, cols) - ref) / (mag + 1e-30);
        cpu_exact = 0;
        if (err > maxerr) maxerr = err;
        if (moe_int(W)) {     /* tipos K; Q8_0/Q5_1 so no caminho dos especialistas */
            xq_prepare(x, cols);
            double ei = fabs(moe_dot(W, i, cols) - ref) / (mag + 1e-30);
            if (ei > interr) interr = ei;
        }
    }
    printf("  %-10s tipo=%-2u linhas=%-6d colunas=%-5d erro relativo max=%.2e", name, W->type, rows, cols, maxerr);
    if (maxerr > *worst) *worst = maxerr;
    if (moe_int(W)) { printf("  | CPU inteiro=%.2e", interr); if (interr > worst_int) worst_int = interr; }
    if (W->gbuf) {   /* resultado inteiro da GPU contra a CPU, linha a linha */
        float *og = xalloc(rows, sizeof(float));
        double gerr = 0.0;
        gpu.test_mv = 1; matvec(og, W, x, rows, cols); gpu.test_mv = 0;
        for (int r = 0; r < rows; r++) {
            mat_row(row, W, r, cols);
            double ref = 0.0, mag = 0.0;
            for (int j = 0; j < cols; j++) { ref += (double)row[j] * x[j]; mag += fabs((double)row[j] * x[j]); }
            double err = fabs(og[r] - ref) / (mag + 1e-30);
            if (err > gerr) gerr = err;
        }
        printf("  | GPU=%.2e", gerr);
        if (gerr > *worst) *worst = gerr;
        free(og);
    }
    printf("\n");
    free(x); free(row); free(sx);
}

static void setenv_(const char *k, const char *v) {
#ifdef _WIN32
    char b[128]; snprintf(b, sizeof b, "%s=%s", k, v ? v : ""); _putenv(b);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

