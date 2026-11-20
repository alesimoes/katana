/* ---------- especializacao: treino do proprio katana (katana train, katana gradcheck, KATANA_SPEC) ----------
 * Passo para tras escrito a mao para os modelos densos com atencao comum (llama, qwen2: RMSNorm, GQA, RoPE em
 * pares ou em metades, SwiGLU, vies opcional em q/k/v/o), conferido contra o forward do motor e contra
 * diferencas finitas (katana gradcheck).
 *
 * O que fica congelado continua QUANTIZADO: os produtos com pesos congelados (e o produto transposto do passo
 * para tras) leem cada linha direto do formato do GGUF (Q4_K, Q6_K, ...), sem copia em float do modelo. So o que
 * se treina ganha copia em float (mestre), gradiente e momentos do AdamW. Memoria do treino = modelo quantizado +
 * 16 bytes por peso treinado + ativacoes da sequencia.
 *
 * A perda e entropia cruzada so nos tokens marcados (train_spans do tools/dataset/ksess2jsonl.py: as falas do
 * modelo e o token que encerra cada uma). O resultado vai para um arquivo de especializacao (.kspec) com so os
 * tensores treinados, que o motor aplica por cima do GGUF na carga (KATANA_SPEC=arquivo), sem fundir nem
 * reescrever o modelo base. */

typedef struct {
    char name[96];
    const Mat *mat;             /* peso do modelo (matriz) ou NULL */
    float *vec;                 /* vetor do modelo (normas) ou NULL */
    Mat *mat_w;                 /* matriz para gravar o resultado (carga da especializacao) */
    float **vec_w;
    size_t rows, cols;          /* vetor: rows = 1 */
    float *w, *g, *m1, *m2;     /* treinavel inteiro: mestre em float, gradiente, momentos */
    /* adaptador de posto r: W' = W + s * B A (A [r][cols], B [rows][r]); W continua congelado e quantizado */
    int r; float s;
    float *a, *b, *ga, *gb, *ma1, *ma2, *mb1, *mb2;
} TPar;

typedef struct {
    TPar *p; int n;
    int tok, out, onorm;        /* indices de token_embd, output (-1 = amarrado ao embedding), output_norm */
    /* por camada (-1 = nao existe nesta camada) */
    int *an, *fn, *q, *k, *v, *o, *gt, *up, *dn;    /* atencao e feed-forward denso */
    int *qn, *kn;               /* norma de q e k (olmoe: vetor inteiro; qwen3: por cabeca) */
    int *rt, *ex0;              /* MoE: roteador; primeiro especialista (gate, up, down de e em ex0 + 3e ...) */
    int *sg, *su, *sd, *sgv;    /* MoE: especialistas compartilhados e o portao sigmoide deles */
    int *dqkv, *dz, *dal, *dbe, *dcv, *dsa, *ddt, *dsn, *dout;    /* Gated DeltaNet (camadas lineares do qwen35) */
    int moe;                    /* o modelo tem camadas MoE */
} TTab;

static void tt_add(TTab *t, const char *name, const Mat *m, Mat *mw, float *v, float **vw, size_t rows, size_t cols) {
    TPar *p = &t->p[t->n++];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->mat = m; p->mat_w = mw; p->vec = v; p->vec_w = vw; p->rows = rows; p->cols = cols;
}

static int tt_is_moe(const Model *m, int l) { return m->cfg.n_expert && l >= m->cfg.n_dense && m->layers[l].eg; }

/* tabela dos tensores treinaveis (nomes do GGUF; especialista e de um tensor empilhado: nome + ".e<e>") */
static const char *tt_build(Model *m, TTab *t) {
    const Config *c = &m->cfg;
    if (c->g4 || c->go || c->mla || c->glm || c->glmoe)
        return "this architecture is not supported by the trainer yet (llama, qwen2, qwen3, olmoe, qwen3moe, qwen35, qwen35moe)";
    if (c->ssm_inner && c->ssm_inner / c->ssm_rank > 512) return "DeltaNet: v head too wide";
    if ((c->emb_scale != 0.0f && c->emb_scale != 1.0f) || c->softcap != 0.0f) return "embedding scale or logit softcap not supported yet";
    if (c->n_expert && (c->gate_sig || c->mla)) return "sigmoid routers (DeepSeek-style) are not supported by the trainer yet";
    const int L = c->n_layers, d = c->dim, qd = c->n_heads * c->hd, kvd = c->n_kv * c->hd, E = c->n_expert;
    for (int l = 0; l < L; l++) {
        const Layer *Ly = &m->layers[l];
        if (Ly->exp_bias || Ly->router_b || Ly->exp_w || Ly->eg_b) return "router or expert bias is not supported by the trainer yet";
    }
    size_t cap = (size_t)L * (26 + 3 * (size_t)E) + 4;
    t->p = xalloc(cap, sizeof(TPar)); t->n = 0;
    const int NA = 26;
    int *all = xalloc((size_t)L * NA, sizeof(int));
    for (int i = 0; i < L * NA; i++) all[i] = -1;
    int **fields[] = { &t->an, &t->fn, &t->q, &t->k, &t->v, &t->o, &t->gt, &t->up, &t->dn, &t->qn, &t->kn, &t->rt, &t->ex0, &t->sg, &t->su, &t->sd, &t->sgv,
                       &t->dqkv, &t->dz, &t->dal, &t->dbe, &t->dcv, &t->dsa, &t->ddt, &t->dsn, &t->dout };
    for (int i = 0; i < NA; i++) *fields[i] = all + (size_t)i * L;
    t->tok = t->n; tt_add(t, "token_embd.weight", &m->tok_emb, &m->tok_emb, NULL, NULL, c->vocab, d);
    const int tied = m->lm_head.f == m->tok_emb.f && m->lm_head.q == m->tok_emb.q;
    t->out = -1;
    if (!tied) { t->out = t->n; tt_add(t, "output.weight", &m->lm_head, &m->lm_head, NULL, NULL, c->vocab, d); }
    t->onorm = t->n; tt_add(t, "output_norm.weight", NULL, NULL, m->out_norm, &m->out_norm, 1, d);
    char nm[96];
    for (int l = 0; l < L; l++) {
        Layer *Ly = &m->layers[l];
#define TT_M(idx, suf, field, r, cc) snprintf(nm, sizeof nm, "blk.%d." suf, l); t->idx[l] = t->n; tt_add(t, nm, &Ly->field, &Ly->field, NULL, NULL, r, cc)
#define TT_V(idx, suf, field, nn) snprintf(nm, sizeof nm, "blk.%d." suf, l); t->idx[l] = t->n; tt_add(t, nm, NULL, NULL, Ly->field, &Ly->field, 1, nn)
        TT_V(an, "attn_norm.weight", attn_norm, d);
        if (Ly->lin) {                       /* Gated DeltaNet */
            const int C = (int)dn_channels(c), nv = c->ssm_rank;
            TT_M(dqkv, "attn_qkv.weight", wqkv, C, d);
            TT_M(dz, "attn_gate.weight", wz, c->ssm_inner, d);
            TT_M(dal, "ssm_alpha.weight", walpha, nv, d);
            TT_M(dbe, "ssm_beta.weight", wbeta, nv, d);
            TT_V(dcv, "ssm_conv1d.weight", conv_w, C * c->ssm_conv);
            snprintf(nm, sizeof nm, "blk.%d.ssm_a", l); t->dsa[l] = t->n; tt_add(t, nm, NULL, NULL, Ly->ssm_a, &Ly->ssm_a, 1, nv);
            TT_V(ddt, "ssm_dt.bias", dt_bias, nv);
            TT_V(dsn, "ssm_norm.weight", ssm_norm, c->ssm_inner / nv);
            TT_M(dout, "ssm_out.weight", wssm_out, d, c->ssm_inner);
        } else {
        TT_M(q, "attn_q.weight", wq, c->attn_gated ? 2 * qd : qd, d);
        TT_M(k, "attn_k.weight", wk, kvd, d);
        TT_M(v, "attn_v.weight", wv, kvd, d);
        TT_M(o, "attn_output.weight", wo, d, qd);
        }
        if (c->qk_norm && Ly->q_norm) {
            TT_V(qn, "attn_q_norm.weight", q_norm, c->qk_norm == 1 ? qd : c->hd);
            TT_V(kn, "attn_k_norm.weight", k_norm, c->qk_norm == 1 ? kvd : c->hd);
        }
        if (c->ssm_inner) { TT_V(fn, "post_attention_norm.weight", ffn_norm, d); }
        else { TT_V(fn, "ffn_norm.weight", ffn_norm, d); }
        if (tt_is_moe(m, l)) {
            const int H = c->hidden;
            t->moe = 1;
            TT_M(rt, "ffn_gate_inp.weight", router, E, d);
            t->ex0[l] = t->n;
            for (int e = 0; e < E; e++) {
                snprintf(nm, sizeof nm, "blk.%d.ffn_gate_exps.weight.e%d", l, e); tt_add(t, nm, &Ly->eg[e], &Ly->eg[e], NULL, NULL, H, d);
                snprintf(nm, sizeof nm, "blk.%d.ffn_up_exps.weight.e%d", l, e);   tt_add(t, nm, &Ly->eu[e], &Ly->eu[e], NULL, NULL, H, d);
                snprintf(nm, sizeof nm, "blk.%d.ffn_down_exps.weight.e%d", l, e); tt_add(t, nm, &Ly->ed[e], &Ly->ed[e], NULL, NULL, d, H);
            }
            if (c->n_shared && (Ly->sg.q || Ly->sg.f)) {
                const int Hs = c->n_shared * H;
                TT_M(sg, "ffn_gate_shexp.weight", sg, Hs, d);
                TT_M(su, "ffn_up_shexp.weight", su, Hs, d);
                TT_M(sd, "ffn_down_shexp.weight", sd, d, Hs);
                if (Ly->shexp_gate) { TT_V(sgv, "ffn_gate_inp_shexp.weight", shexp_gate, d); }
            }
        } else {
            const int Hd = c->n_expert ? c->hidden_dense : c->hidden;
            TT_M(gt, "ffn_gate.weight", w_gate, Hd, d);
            TT_M(up, "ffn_up.weight", w_up, Hd, d);
            TT_M(dn, "ffn_down.weight", w_down, d, Hd);
        }
#undef TT_M
#undef TT_V
    }
    return NULL;
}

/* casa nome com padrao com '*' (qualquer sequencia) */
static int tt_glob(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') { for (; ; s++) { if (tt_glob(p + 1, s)) return 1; if (!*s) return 0; } }
    return *s && *p == *s && tt_glob(p + 1, s + 1);
}

/* liga o treino nos tensores que casam com algum padrao da lista (virgulas); devolve quantos pesos treinaveis.
   rank > 0: matrizes ganham adaptador de posto rank (so A e B treinam); normas e embedding treinam inteiros. */
static size_t tt_select_r(TTab *t, const char *pats, int rank, float alpha);
static size_t tt_select(TTab *t, const char *pats) { return tt_select_r(t, pats, 0, 0); }
/* liga o treino de um parametro (inteiro, ou adaptador de posto rank nas matrizes); devolve quantos pesos */
static size_t tp_enable(TTab *t, int i, int rank, float alpha) {
    TPar *p = &t->p[i];
    if (p->w || p->r) return 0;
    if (rank > 0 && p->mat && i != t->tok) {
        const size_t R = p->rows, C = p->cols, rr = (size_t)rank;
        p->r = rank; p->s = alpha / rank;
        p->a = xalloc(rr * C, 4); p->b = xalloc(R * rr, 4);
        p->ga = xalloc(rr * C, 4); p->gb = xalloc(R * rr, 4);
        p->ma1 = xalloc(rr * C, 4); p->ma2 = xalloc(rr * C, 4); p->mb1 = xalloc(R * rr, 4); p->mb2 = xalloc(R * rr, 4);
        const float sc = 1.0f / sqrtf((float)C);          /* A aleatoria, B zero: comeca igual ao modelo base */
        for (size_t j = 0; j < rr * C; j++) p->a[j] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * sc;
        return rr * (R + C);
    }
    const size_t n = p->rows * p->cols;
    p->w = xalloc(n, sizeof(float)); p->g = xalloc(n, sizeof(float));
    p->m1 = xalloc(n, sizeof(float)); p->m2 = xalloc(n, sizeof(float));
    if (p->vec) memcpy(p->w, p->vec, n * sizeof(float));
    else for (size_t r = 0; r < p->rows; r++) mat_row(p->w + r * p->cols, p->mat, (int)r, (int)p->cols);
    return n;
}
static size_t tt_select_r(TTab *t, const char *pats, int rank, float alpha) {
    size_t tot = 0;
    char buf[1024];
    for (int i = 0; i < t->n; i++) {
        snprintf(buf, sizeof buf, "%s", pats);
        int hit = 0;
        for (char *s = strtok(buf, ","); s && !hit; s = strtok(NULL, ",")) {
            while (*s == ' ') s++;
            hit = !strcmp(s, "all") || tt_glob(s, t->p[i].name);
        }
        if (hit) tot += tp_enable(t, i, rank, alpha);
    }
    return tot;
}

/* ---- produtos com um parametro (treinavel em float, ou congelado direto do formato do GGUF) ---- */
typedef struct { const TPar *p; float *out; const float *x; float *tmp; int B; } TMvJob;

/* linha r do parametro em float (mestre, ou desquantizada em tmp) */
static const float *tp_row(const TPar *p, size_t r, float *tmp) {
    if (p->w) return p->w + r * p->cols;
    if (p->vec) return p->vec;
    if (p->mat->f) return p->mat->f + r * p->cols;
    dequant(p->mat->type, p->mat->q + r * p->mat->stride, tmp, p->cols);
    return tmp;
}

/* out[b][r] = W[r] . x[b] para B vetores de entrada (cada linha e lida/desquantizada uma vez para o lote) */
static void tmv_job(void *ctx, int r0, int r1) {
    const TMvJob *j = (const TMvJob *)ctx;
    const size_t C = j->p->cols, R = j->p->rows;
    float *tmp = xalloc(C, sizeof(float));
    for (int r = r0; r < r1; r++) {
        const float *w = tp_row(j->p, (size_t)r, tmp);
        for (int b = 0; b < j->B; b++) j->out[(size_t)b * R + r] = dot(w, j->x + (size_t)b * C, (int)C);
    }
    free(tmp);
}
/* z[b][k] = A[k] . x[b] */
static float *tp_az(const TPar *p, const float *x, int B) {
    const size_t C = p->cols;
    float *z = xalloc((size_t)B * p->r, 4);
    for (int b = 0; b < B; b++) for (int k = 0; k < p->r; k++) z[(size_t)b * p->r + k] = dot(p->a + (size_t)k * C, x + (size_t)b * C, (int)C);
    return z;
}
static void tp_mv(const TPar *p, float *out, const float *x, int B) {
    TMvJob j = { p, out, x, NULL, B };
    pool_for(tmv_job, &j, (int)p->rows, 16);
    if (!p->r) return;
    float *z = tp_az(p, x, B);
    const size_t R = p->rows;
    for (int b = 0; b < B; b++) {
        const float *zb = z + (size_t)b * p->r;
        float *o = out + (size_t)b * R;
        for (size_t i = 0; i < R; i++) o[i] += p->s * dot(p->b + i * p->r, zb, p->r);
    }
    free(z);
}

/* dx[b][c] += sum_r dy[b][r] W[r][c] (transposto): cada thread soma num pedaco das colunas */
typedef struct { const TPar *p; float *dx; const float *dy; int B; } TMtJob;
static void tmt_job(void *ctx, int c0, int c1) {
    const TMtJob *j = (const TMtJob *)ctx;
    const size_t C = j->p->cols, R = j->p->rows;
    float *tmp = xalloc(C, sizeof(float));
    for (size_t r = 0; r < R; r++) {
        const float *w = tp_row(j->p, r, tmp);
        for (int b = 0; b < j->B; b++) {
            const float a = j->dy[(size_t)b * R + r];
            if (a == 0.0f) continue;
            float *o = j->dx + (size_t)b * C;
            for (int c = c0; c < c1; c++) o[c] += a * w[c];
        }
    }
    free(tmp);
}
static void tp_mvT(const TPar *p, float *dx, const float *dy, int B) {
    TMtJob j = { p, dx, dy, B };
    const int C = (int)p->cols;
    pool_for(tmt_job, &j, C, C / (pool.n > 1 ? pool.n : 1) + 1);
    if (!p->r) return;
    const size_t R = p->rows;
    float *t = xalloc(p->r, 4);
    for (int b = 0; b < B; b++) {                      /* dx += s * A^T (B^T dy) */
        memset(t, 0, sizeof(float) * p->r);
        const float *d = dy + (size_t)b * R;
        for (size_t i = 0; i < R; i++) { const float a = d[i]; if (a == 0.0f) continue; const float *bi = p->b + i * p->r; for (int k = 0; k < p->r; k++) t[k] += a * bi[k]; }
        float *o = dx + (size_t)b * C;
        for (int k = 0; k < p->r; k++) { const float tk = p->s * t[k]; const float *ak = p->a + (size_t)k * C; for (int c = 0; c < C; c++) o[c] += tk * ak[c]; }
    }
    free(t);
}

/* gradiente de W: g[r][c] += sum_b dy[b][r] x[b][c] (so se treinavel) */
typedef struct { TPar *p; const float *dy, *x; int B; } TGJob;
static void tg_job(void *ctx, int r0, int r1) {
    const TGJob *j = (const TGJob *)ctx;
    const size_t C = j->p->cols, R = j->p->rows;
    for (int r = r0; r < r1; r++) {
        float *g = j->p->g + (size_t)r * C;
        for (int b = 0; b < j->B; b++) {
            const float a = j->dy[(size_t)b * R + r];
            if (a == 0.0f) continue;
            const float *x = j->x + (size_t)b * C;
            for (size_t c = 0; c < C; c++) g[c] += a * x[c];
        }
    }
}
static void tp_grad(TPar *p, const float *dy, const float *x, int B) {
    if (p->r) {                                        /* gB += s dy (A x)^T ; gA += s (B^T dy) x^T */
        const size_t R = p->rows, C = p->cols;
        float *z = tp_az(p, x, B), *t = xalloc(p->r, 4);
        for (int b = 0; b < B; b++) {
            const float *d = dy + (size_t)b * R, *zb = z + (size_t)b * p->r, *xb = x + (size_t)b * C;
            memset(t, 0, sizeof(float) * p->r);
            for (size_t i = 0; i < R; i++) {
                const float a = d[i]; if (a == 0.0f) continue;
                float *gb = p->gb + i * p->r; const float *bi = p->b + i * p->r;
                for (int k = 0; k < p->r; k++) { gb[k] += p->s * a * zb[k]; t[k] += a * bi[k]; }
            }
            for (int k = 0; k < p->r; k++) { const float tk = p->s * t[k]; if (tk == 0.0f) continue; float *ga = p->ga + (size_t)k * C; for (size_t c = 0; c < C; c++) ga[c] += tk * xb[c]; }
        }
        free(z); free(t);
        return;
    }
    if (!p->g) return;
    TGJob j = { p, dy, x, B };
    pool_for(tg_job, &j, (int)p->rows, 16);
}

static const float *tp_vec(const TPar *p) { return p->w ? p->w : p->vec; }

/* RMSNorm de B vetores: y = g * x * inv, inv guardado para o passo para tras */
static void t_rms(const TPar *gp, const float *x, float *y, float *inv, int B, int d, float eps) {
    const float *g = tp_vec(gp);
    for (int b = 0; b < B; b++) {
        const float *xi = x + (size_t)b * d; float *yi = y + (size_t)b * d;
        double ss = 0; for (int i = 0; i < d; i++) ss += (double)xi[i] * xi[i];
        const float iv = 1.0f / sqrtf((float)(ss / d) + eps);
        inv[b] = iv;
        for (int i = 0; i < d; i++) yi[i] = g[i] * (xi[i] * iv);
    }
}
/* passo para tras da RMSNorm: dx += ..., dg += dy * x * inv */
static void t_rms_back(TPar *gp, const float *x, const float *inv, const float *dy, float *dx, int B, int d) {
    const float *g = tp_vec(gp);
    for (int b = 0; b < B; b++) {
        const float *xi = x + (size_t)b * d, *dyi = dy + (size_t)b * d; float *dxi = dx + (size_t)b * d;
        const float iv = inv[b];
        double s = 0; for (int i = 0; i < d; i++) s += (double)g[i] * dyi[i] * xi[i];
        const float k = (float)(s / d) * iv * iv * iv;
        for (int i = 0; i < d; i++) dxi[i] += iv * g[i] * dyi[i] - xi[i] * k;
        if (gp->g) for (int i = 0; i < d; i++) gp->g[i] += dyi[i] * xi[i] * iv;
    }
}

static int t_dn_ck(void);
/* ---- ativacoes de uma sequencia ---- */
typedef struct {
    int T, Tmax, H, Hs, K, E;   /* T: tokens desta sequencia (<= Tmax, o alocado); H: largura do feed-forward denso; Hs: compartilhados */
    int fixsel;                 /* 1: o MoE reusa os escolhidos ja guardados (gradcheck: a escolha dos K e degrau, sem derivada) */
    float *x, *xn1, *inv1, *qr, *kr, *invq, *invk, *q, *k, *v, *P, *ao, *xm, *xn2, *inv2, *a, *u, *h;    /* [L][...] */
    /* MoE, por camada: probabilidades do roteador, escolhidos, pesos, intermediarios e saidas de cada escolhido */
    float *pr, *ew, *ea, *eu, *eh, *eo; int *sel;
    float *sa, *su, *sh, *so, *sgt;     /* compartilhados: intermediarios, saida e portao */
    float *qg, *apre;                   /* atencao com portao: q e portao intercalados; saida antes do portao */
    /* Gated DeltaNet: entrada da convolucao, convolucao antes e depois da SiLU, q/k normalizados e seus fatores, z,
       alpha, beta, decaimento, beta sigmoide, estados S (T+1 por camada), saida da recorrencia, fator da norma,
       saida normalizada e saida com portao */
    float *draw, *dcy, *dcs, *dqk, *diqk, *dzz, *dal, *dbe, *ddec, *dbet, *dS, *dor, *dio, *don, *dog;
    float *xf, *xnf, *invf;
} TAct;

static size_t g_ta_bytes;      /* bytes das ativacoes da ultima tact_alloc */
static void *ta_alloc(size_t n, size_t sz) { g_ta_bytes += n * sz; return xalloc(n, sz); }
static void tact_alloc(TAct *A, const Config *c, int T) {
    g_ta_bytes = 0;
    const size_t L = c->n_layers, d = c->dim, qd = (size_t)c->n_heads * c->hd, kvd = (size_t)c->n_kv * c->hd;
    memset(A, 0, sizeof *A);
    A->T = T; A->Tmax = T;
    A->H = c->n_expert ? (c->hidden_dense > 0 ? c->hidden_dense : 1) : c->hidden;
    A->E = c->n_expert; A->K = c->n_expert ? c->n_used : 0; A->Hs = c->n_shared * c->hidden;
    const size_t H = A->H;
    A->x = ta_alloc(L * T * d, 4); A->xn1 = ta_alloc(L * T * d, 4); A->inv1 = ta_alloc(L * T, 4);
    A->qr = ta_alloc(L * T * qd, 4); A->kr = ta_alloc(L * T * kvd, 4);
    A->invq = ta_alloc(L * T * c->n_heads, 4); A->invk = ta_alloc(L * T * c->n_kv, 4);
    A->q = ta_alloc(L * T * qd, 4); A->k = ta_alloc(L * T * kvd, 4); A->v = ta_alloc(L * T * kvd, 4);
    A->P = ta_alloc((size_t)T, 4); A->ao = ta_alloc(L * T * qd, 4);      /* P: uma linha de rascunho (a atencao e refeita na volta) */
    A->xm = ta_alloc(L * T * d, 4); A->xn2 = ta_alloc(L * T * d, 4); A->inv2 = ta_alloc(L * T, 4);
    A->a = ta_alloc(L * T * H, 4); A->u = ta_alloc(L * T * H, 4); A->h = ta_alloc(L * T * H, 4);
    if (A->E) {
        const size_t K = A->K, He = c->hidden;
        A->pr = ta_alloc(L * T * A->E, 4); A->ew = ta_alloc(L * T * K, 4); A->sel = ta_alloc(L * T * K, sizeof(int));
        A->ea = ta_alloc(L * T * K * He, 4); A->eu = ta_alloc(L * T * K * He, 4); A->eh = ta_alloc(L * T * K * He, 4); A->eo = ta_alloc(L * T * K * d, 4);
        if (A->Hs) { A->sa = ta_alloc(L * T * A->Hs, 4); A->su = ta_alloc(L * T * A->Hs, 4); A->sh = ta_alloc(L * T * A->Hs, 4); }
        A->so = ta_alloc(L * T * d, 4); A->sgt = ta_alloc(L * T, 4);
    }
    if (c->attn_gated) { A->qg = ta_alloc(L * T * 2 * qd, 4); A->apre = ta_alloc(L * T * qd, 4); }
    if (c->ssm_inner) {
        const size_t C = dn_channels(c), kd2 = 2 * (size_t)c->ssm_groups * c->ssm_state, nv = c->ssm_rank, in = c->ssm_inner;
        A->draw = ta_alloc(L * T * C, 4); A->dcy = ta_alloc(L * T * C, 4); A->dcs = ta_alloc(L * T * C, 4);
        A->dqk = ta_alloc(L * T * kd2, 4); A->diqk = ta_alloc(L * T * 2 * c->ssm_groups, 4);
        A->dzz = ta_alloc(L * T * in, 4); A->dal = ta_alloc(L * T * nv, 4); A->dbe = ta_alloc(L * T * nv, 4);
        A->ddec = ta_alloc(L * T * nv, 4); A->dbet = ta_alloc(L * T * nv, 4);
        A->dS = ta_alloc(L * ((size_t)T / t_dn_ck() + 1) * (size_t)c->ssm_state * in, 4);
        A->dor = ta_alloc(L * T * in, 4); A->dio = ta_alloc(L * T * nv, 4); A->don = ta_alloc(L * T * in, 4); A->dog = ta_alloc(L * T * in, 4);
    }
    A->xf = ta_alloc((size_t)T * d, 4); A->xnf = ta_alloc((size_t)T * d, 4); A->invf = ta_alloc(T, 4);
}
static void tact_free(TAct *A) {
    float *fs[] = { A->x, A->xn1, A->inv1, A->qr, A->kr, A->invq, A->invk, A->q, A->k, A->v, A->P, A->ao, A->xm, A->xn2, A->inv2,
                    A->a, A->u, A->h, A->pr, A->ew, A->ea, A->eu, A->eh, A->eo, A->sa, A->su, A->sh, A->so, A->sgt, A->xf, A->xnf, A->invf,
                    A->qg, A->apre, A->draw, A->dcy, A->dcs, A->dqk, A->diqk, A->dzz, A->dal, A->dbe, A->ddec, A->dbet, A->dS, A->dor, A->dio, A->don, A->dog };
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++) free(fs[i]);
    free(A->sel);
    memset(A, 0, sizeof *A);
}

static void t_rope(const Model *m, float *v, int nh, int pos, int inverse) {
    const Config *c = &m->cfg;
    const int hd = c->hd, rhalf = c->attn_gated && c->rope_dim > 0 && c->rope_dim < hd ? c->rope_dim / 2 : hd / 2;
    const int step = c->rope_neox ? 1 : 2, gap = c->rope_neox ? rhalf : 1;
    for (int i = 0; i < rhalf; i++) {
        const float ang = pos * m->rope_freq[i], cs = cosf(ang), sn = inverse ? -sinf(ang) : sinf(ang);
        for (int h = 0; h < nh; h++) {
            float *p = v + h * hd + step * i, a = p[0], b = p[gap];
            p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs;
        }
    }
}

static void t_bias(const float *b, float *y, int B, int n) { if (b) for (int t = 0; t < B; t++) for (int i = 0; i < n; i++) y[(size_t)t * n + i] += b[i]; }

static float t_silu(float g) { return g / (1.0f + expf(-g)); }

/* SwiGLU com tres parametros: out = down(silu(gate x) * up x); guarda a, u, h */
static void t_glu_fwd(TTab *tt, int ig, int iu, int idn, const float *x, float *a, float *u, float *h, float *out, int B, int H) {
    tp_mv(&tt->p[ig], a, x, B);
    tp_mv(&tt->p[iu], u, x, B);
    for (size_t i = 0; i < (size_t)B * H; i++) h[i] = t_silu(a[i]) * u[i];
    tp_mv(&tt->p[idn], out, h, B);
}
/* volta do SwiGLU: dy na saida; acumula em dx e nos gradientes */
static void t_glu_back(TTab *tt, int ig, int iu, int idn, const float *x, const float *a, const float *u, const float *h,
                       const float *dy, float *dx, int B, int H) {
    float *dh = xalloc((size_t)B * H, 4), *da = xalloc((size_t)B * H, 4), *du = xalloc((size_t)B * H, 4);
    tp_grad(&tt->p[idn], dy, h, B);
    tp_mvT(&tt->p[idn], dh, dy, B);
    for (size_t i = 0; i < (size_t)B * H; i++) {
        const float g = a[i], sg = 1.0f / (1.0f + expf(-g));
        du[i] = dh[i] * g * sg;
        da[i] = dh[i] * u[i] * sg * (1.0f + g * (1.0f - sg));
    }
    tp_grad(&tt->p[ig], da, x, B); tp_mvT(&tt->p[ig], dx, da, B);
    tp_grad(&tt->p[iu], du, x, B); tp_mvT(&tt->p[iu], dx, du, B);
    free(dh); free(da); free(du);
}

/* MoE da camada l (x = entrada normalizada [T][d]): roteador softmax, K melhores, pesos (renormalizados se o modelo
   pede), especialistas e compartilhados. out = soma. Guarda tudo para voltar. Mesma conta do motor (moe_route). */
static void t_moe_fwd(Model *m, TTab *tt, TAct *A, int l, const float *x, float *out) {
    const Config *c = &m->cfg;
    const int T = A->T, d = c->dim, E = A->E, K = A->K, He = c->hidden;
    float *pr = A->pr + (size_t)l * T * E, *ew = A->ew + (size_t)l * T * K;
    int *sel = A->sel + (size_t)l * T * K;
    float *ea = A->ea + (size_t)l * T * K * He, *eu = A->eu + (size_t)l * T * K * He, *eh = A->eh + (size_t)l * T * K * He;
    float *eo = A->eo + (size_t)l * T * K * d;
    tp_mv(&tt->p[tt->rt[l]], pr, x, T);
    float *ch = xalloc(E, 4);
    for (int t = 0; t < T; t++) {
        float *p = pr + (size_t)t * E;
        softmax(p, E);
        memcpy(ch, p, (size_t)E * 4);
        float wsum = 0;
        for (int k = 0; k < K; k++) {
            int best = 0;
            if (A->fixsel) best = sel[(size_t)t * K + k];
            else { for (int e = 1; e < E; e++) if (ch[e] > ch[best]) best = e; }
            sel[(size_t)t * K + k] = best; ew[(size_t)t * K + k] = p[best]; wsum += p[best];
            ch[best] = -INFINITY;
        }
        const float eps = 6.103515625e-5f;
        for (int k = 0; k < K; k++) {
            float w = ew[(size_t)t * K + k];
            if (c->moe_norm) w /= (wsum > eps ? wsum : eps);
            if (c->exp_scale != 1.0f) w *= c->exp_scale;
            ew[(size_t)t * K + k] = w;
        }
    }
    free(ch);
    memset(out, 0, (size_t)T * d * 4);
    /* agrupa os pares (token, k) por especialista: um lote por especialista */
    int *ix = xalloc((size_t)T * K, sizeof(int));
    float *xb = xalloc((size_t)T * K * d, 4), *ab = xalloc((size_t)T * K * He, 4), *ub = xalloc((size_t)T * K * He, 4);
    float *hb = xalloc((size_t)T * K * He, 4), *ob = xalloc((size_t)T * K * d, 4);
    for (int e = 0; e < E; e++) {
        int n = 0;
        for (int j = 0; j < T * K; j++) if (sel[j] == e) ix[n++] = j;
        if (!n) continue;
        const int base = tt->ex0[l] + 3 * e;
        for (int i = 0; i < n; i++) memcpy(xb + (size_t)i * d, x + (size_t)(ix[i] / K) * d, d * 4);
        t_glu_fwd(tt, base, base + 1, base + 2, xb, ab, ub, hb, ob, n, He);
        for (int i = 0; i < n; i++) {
            const size_t j = (size_t)ix[i];
            memcpy(ea + j * He, ab + (size_t)i * He, He * 4); memcpy(eu + j * He, ub + (size_t)i * He, He * 4);
            memcpy(eh + j * He, hb + (size_t)i * He, He * 4); memcpy(eo + j * d, ob + (size_t)i * d, d * 4);
            const float w = ew[j];
            float *ot = out + (j / K) * d;
            for (int q = 0; q < d; q++) ot[q] += w * ob[(size_t)i * d + q];
        }
    }
    free(ix); free(xb); free(ab); free(ub); free(hb); free(ob);
    if (tt->sg[l] >= 0) {
        const int Hs = A->Hs;
        float *sa = A->sa + (size_t)l * T * Hs, *su = A->su + (size_t)l * T * Hs, *sh = A->sh + (size_t)l * T * Hs;
        float *so = A->so + (size_t)l * T * d, *sgt = A->sgt + (size_t)l * T;
        t_glu_fwd(tt, tt->sg[l], tt->su[l], tt->sd[l], x, sa, su, sh, so, T, Hs);
        for (int t = 0; t < T; t++) {
            sgt[t] = tt->sgv[l] >= 0 ? 1.0f / (1.0f + expf(-dot(tp_vec(&tt->p[tt->sgv[l]]), x + (size_t)t * d, d))) : 1.0f;
            for (int i = 0; i < d; i++) out[(size_t)t * d + i] += sgt[t] * so[(size_t)t * d + i];
        }
    }
}

static void t_moe_back(Model *m, TTab *tt, TAct *A, int l, const float *x, const float *dy, float *dx) {
    const Config *c = &m->cfg;
    const int T = A->T, d = c->dim, E = A->E, K = A->K, He = c->hidden;
    const float *pr = A->pr + (size_t)l * T * E, *ew = A->ew + (size_t)l * T * K;
    const int *sel = A->sel + (size_t)l * T * K;
    const float *ea = A->ea + (size_t)l * T * K * He, *eu = A->eu + (size_t)l * T * K * He, *eh = A->eh + (size_t)l * T * K * He;
    const float *eo = A->eo + (size_t)l * T * K * d;
    float *dz = xalloc((size_t)T * E, 4), *dp = xalloc(E, 4), *dwa = xalloc((size_t)T * K, 4);
    /* especialistas, agrupados como no forward */
    int *ix = xalloc((size_t)T * K, sizeof(int));
    float *xb = xalloc((size_t)T * K * d, 4), *ab = xalloc((size_t)T * K * He, 4), *ub = xalloc((size_t)T * K * He, 4);
    float *hb = xalloc((size_t)T * K * He, 4), *db = xalloc((size_t)T * K * d, 4), *dxb = xalloc((size_t)T * K * d, 4);
    for (int j = 0; j < T * K; j++) dwa[j] = dot(eo + (size_t)j * d, dy + (size_t)(j / K) * d, d);     /* d peso do escolhido */
    for (int e = 0; e < E; e++) {
        int n = 0;
        for (int j = 0; j < T * K; j++) if (sel[j] == e) ix[n++] = j;
        if (!n) continue;
        const int base = tt->ex0[l] + 3 * e;
        for (int i = 0; i < n; i++) {
            const size_t j = (size_t)ix[i];
            memcpy(xb + (size_t)i * d, x + (j / K) * d, d * 4);
            memcpy(ab + (size_t)i * He, ea + j * He, He * 4); memcpy(ub + (size_t)i * He, eu + j * He, He * 4); memcpy(hb + (size_t)i * He, eh + j * He, He * 4);
            const float *dyt = dy + (j / K) * d;
            for (int q = 0; q < d; q++) db[(size_t)i * d + q] = ew[j] * dyt[q];
        }
        memset(dxb, 0, (size_t)n * d * 4);
        t_glu_back(tt, base, base + 1, base + 2, xb, ab, ub, hb, db, dxb, n, He);
        for (int i = 0; i < n; i++) { float *dt = dx + (size_t)(ix[i] / K) * d; for (int q = 0; q < d; q++) dt[q] += dxb[(size_t)i * d + q]; }
    }
    free(ix); free(xb); free(ab); free(ub); free(hb); free(db); free(dxb);
    for (int t = 0; t < T; t++) {
        const float *p = pr + (size_t)t * E;
        const float *dw = dwa + (size_t)t * K;
        double S = 0;
        for (int k = 0; k < K; k++) S += p[sel[(size_t)t * K + k]];
        /* pesos -> probabilidades dos escolhidos (renormalizacao e escala), depois softmax -> logits do roteador */
        memset(dp, 0, (size_t)E * 4);
        const float es = c->exp_scale, eps = 6.103515625e-5f;
        if (c->moe_norm) {
            const double Sx = S > eps ? S : eps;
            double sdw = 0;
            for (int k = 0; k < K; k++) sdw += dw[k] * p[sel[(size_t)t * K + k]];
            for (int k = 0; k < K; k++) {
                const int e = sel[(size_t)t * K + k];
                dp[e] += (float)(es * (dw[k] / Sx - (S > eps ? sdw / (Sx * Sx) : 0.0)));
            }
        } else for (int k = 0; k < K; k++) dp[sel[(size_t)t * K + k]] += es * dw[k];
        double s = 0;
        for (int e = 0; e < E; e++) s += (double)p[e] * dp[e];
        for (int e = 0; e < E; e++) dz[(size_t)t * E + e] = p[e] * (dp[e] - (float)s);
    }
    tp_grad(&tt->p[tt->rt[l]], dz, x, T);
    tp_mvT(&tt->p[tt->rt[l]], dx, dz, T);
    if (tt->sg[l] >= 0) {
        const int Hs = A->Hs;
        const float *sa = A->sa + (size_t)l * T * Hs, *su = A->su + (size_t)l * T * Hs, *sh = A->sh + (size_t)l * T * Hs;
        const float *so = A->so + (size_t)l * T * d, *sgt = A->sgt + (size_t)l * T;
        float *dso = xalloc((size_t)T * d, 4);
        TPar *gv = tt->sgv[l] >= 0 ? &tt->p[tt->sgv[l]] : NULL;
        for (int t = 0; t < T; t++) {
            const float *dyt = dy + (size_t)t * d;
            for (int i = 0; i < d; i++) dso[(size_t)t * d + i] = sgt[t] * dyt[i];
            if (gv) {                                         /* portao sigmoide escalar */
                const float dl = dot(so + (size_t)t * d, dyt, d) * sgt[t] * (1.0f - sgt[t]);
                const float *gw = tp_vec(gv), *xt = x + (size_t)t * d;
                for (int i = 0; i < d; i++) dx[(size_t)t * d + i] += dl * gw[i];
                if (gv->g) for (int i = 0; i < d; i++) gv->g[i] += dl * xt[i];
            }
        }
        t_glu_back(tt, tt->sg[l], tt->su[l], tt->sd[l], x, sa, su, sh, dso, dx, T, Hs);
        free(dso);
    }
    free(dz); free(dp); free(dwa);
}

/* ---- Gated DeltaNet (camada linear do qwen35): mesma conta do motor (dn_proj, dn_conv, dn_recur, dn_gate) ----
   proj: qkv, z, alpha, beta;  conv: causal por canal (janela K, zeros antes do inicio) + SiLU;  q e k: norma L2 por
   grupo (q tambem / sqrt(dk));  por cabeca v: S = exp(g) S, delta = beta (v - S^T k), S += k delta^T, o = S^T q, com
   g = ssm_a * softplus(alpha + dt_bias) e beta = sigmoide;  saida: norma RMS por cabeca vezes SiLU(z), depois ssm_out. */
static int t_dn_group(const Config *c, int h) {
    if (dn_tile < 0) dn_tile = getenv("KATANA_DN_TILE") ? atoi(getenv("KATANA_DN_TILE")) : 1;
    return dn_tile ? h % c->ssm_groups : h / (c->ssm_rank / c->ssm_groups);
}
static float t_softplus(float a) { return a > 20.0f ? a : log1pf(expf(a)); }

/* Estados do DeltaNet guardados so a cada DN_CK tokens (pontos de controle); o passo para tras refaz os estados de um
   bloco por vez a partir do ponto de controle. Memoria por camada: (T / DN_CK + 1) estados em vez de T + 1.
   KATANA_TRAIN_DNCK muda o bloco (1 = guarda todos). */
static int t_dn_ck(void) { static int v; if (!v) { v = getenv("KATANA_TRAIN_DNCK") ? atoi(getenv("KATANA_TRAIN_DNCK")) : 64; if (v < 1) v = 1; } return v; }

/* um passo da recorrencia para todas as cabecas: Sp -> Sn (podem ser buffers diferentes), o opcional */
static void t_dn_step(const Config *c, const float *qkt, const float *cst, const float *dect, const float *bett, const float *Sp, float *Sn, float *ot) {
    const int dk = c->ssm_state, ng = c->ssm_groups, nv = c->ssm_rank, dv = c->ssm_inner / nv, kd = ng * dk;
    float kvv[512];
    for (int h = 0; h < nv; h++) {
        const int kg = t_dn_group(c, h);
        const float *q = qkt + (size_t)kg * dk, *k = qkt + kd + (size_t)kg * dk, *v = cst + 2 * kd + (size_t)h * dv;
        const float *S0 = Sp + (size_t)h * dk * dv;
        float *S1 = Sn + (size_t)h * dk * dv;
        const float dd = dect[h], bb = bett[h];
        for (int j = 0; j < dv; j++) kvv[j] = 0;
        for (int i = 0; i < dk; i++) for (int j = 0; j < dv; j++) { S1[(size_t)i * dv + j] = dd * S0[(size_t)i * dv + j]; kvv[j] += S1[(size_t)i * dv + j] * k[i]; }
        for (int j = 0; j < dv; j++) kvv[j] = (v[j] - kvv[j]) * bb;
        if (ot) for (int j = 0; j < dv; j++) ot[(size_t)h * dv + j] = 0;
        for (int i = 0; i < dk; i++) for (int j = 0; j < dv; j++) {
            S1[(size_t)i * dv + j] += k[i] * kvv[j];
            if (ot) ot[(size_t)h * dv + j] += S1[(size_t)i * dv + j] * q[i];
        }
    }
}

static void t_dn_fwd(Model *m, TTab *tt, TAct *A, int l, const float *x, float *out) {
    const Config *c = &m->cfg;
    const int T = A->T, C = (int)dn_channels(c), K = c->ssm_conv, dk = c->ssm_state, ng = c->ssm_groups, nv = c->ssm_rank;
    const int in = c->ssm_inner, dv = in / nv, kd = ng * dk;
    const size_t NS = (size_t)dk * in;
    float *raw = A->draw + (size_t)l * T * C, *cy = A->dcy + (size_t)l * T * C, *cs = A->dcs + (size_t)l * T * C;
    float *qk = A->dqk + (size_t)l * T * 2 * kd, *iqk = A->diqk + (size_t)l * T * 2 * ng, *z = A->dzz + (size_t)l * T * in;
    float *al = A->dal + (size_t)l * T * nv, *be = A->dbe + (size_t)l * T * nv, *dec = A->ddec + (size_t)l * T * nv, *bet = A->dbet + (size_t)l * T * nv;
    const int CK = t_dn_ck(), NC = A->Tmax / CK + 1;
    float *S = A->dS + (size_t)l * NC * NS, *o = A->dor + (size_t)l * T * in, *io = A->dio + (size_t)l * T * nv;
    float *on = A->don + (size_t)l * T * in, *og = A->dog + (size_t)l * T * in;
    tp_mv(&tt->p[tt->dqkv[l]], raw, x, T);
    tp_mv(&tt->p[tt->dz[l]], z, x, T);
    tp_mv(&tt->p[tt->dal[l]], al, x, T);
    tp_mv(&tt->p[tt->dbe[l]], be, x, T);
    const float *cw = tp_vec(&tt->p[tt->dcv[l]]), *sa = tp_vec(&tt->p[tt->dsa[l]]), *dtb = tp_vec(&tt->p[tt->ddt[l]]);
    for (int t = 0; t < T; t++) for (int ch = 0; ch < C; ch++) {
        float y = 0;
        for (int j = 0; j < K; j++) { const int ts = t - (K - 1) + j; if (ts >= 0) y += cw[(size_t)ch * K + j] * raw[(size_t)ts * C + ch]; }
        cy[(size_t)t * C + ch] = y; cs[(size_t)t * C + ch] = t_silu(y);
    }
    for (int t = 0; t < T; t++) for (int g = 0; g < 2 * ng; g++) {
        const float *p = cs + (size_t)t * C + (size_t)g * dk;
        const float inv = 1.0f / sqrtf(dot(p, p, dk) + c->eps) * (g < ng ? 1.0f / sqrtf((float)dk) : 1.0f);
        iqk[(size_t)t * 2 * ng + g] = inv;
        for (int i = 0; i < dk; i++) qk[(size_t)t * 2 * kd + (size_t)g * dk + i] = p[i] * inv;
    }
    for (int t = 0; t < T; t++) for (int h = 0; h < nv; h++) {
        const float a = al[(size_t)t * nv + h] + dtb[h];
        dec[(size_t)t * nv + h] = expf(sa[h] * t_softplus(a));
        bet[(size_t)t * nv + h] = 1.0f / (1.0f + expf(-be[(size_t)t * nv + h]));
    }
    float *cur = xalloc(NS, 4), *nxt = xalloc(NS, 4);     /* estado zero no inicio da sequencia */
    for (int t = 0; t < T; t++) {
        if (t % CK == 0) memcpy(S + (size_t)(t / CK) * NS, cur, NS * 4);     /* ponto de controle: estado antes do passo t */
        t_dn_step(c, qk + (size_t)t * 2 * kd, cs + (size_t)t * C, dec + (size_t)t * nv, bet + (size_t)t * nv, cur, nxt, o + (size_t)t * in);
        float *sw = cur; cur = nxt; nxt = sw;
    }
    free(cur); free(nxt);
    t_rms(&tt->p[tt->dsn[l]], o, on, io, T * nv, dv, c->eps);
    for (size_t i = 0; i < (size_t)T * in; i++) og[i] = on[i] * t_silu(z[i]);
    tp_mv(&tt->p[tt->dout[l]], out, og, T);
}

static void t_dn_back(Model *m, TTab *tt, TAct *A, int l, const float *x, const float *dy, float *dx) {
    const Config *c = &m->cfg;
    const int T = A->T, C = (int)dn_channels(c), K = c->ssm_conv, dk = c->ssm_state, ng = c->ssm_groups, nv = c->ssm_rank;
    const int in = c->ssm_inner, dv = in / nv, kd = ng * dk;
    const size_t NS = (size_t)dk * in;
    const float *raw = A->draw + (size_t)l * T * C, *cy = A->dcy + (size_t)l * T * C, *cs = A->dcs + (size_t)l * T * C;
    const float *qk = A->dqk + (size_t)l * T * 2 * kd, *iqk = A->diqk + (size_t)l * T * 2 * ng, *z = A->dzz + (size_t)l * T * in;
    const float *al = A->dal + (size_t)l * T * nv, *dec = A->ddec + (size_t)l * T * nv, *bet = A->dbet + (size_t)l * T * nv;
    const int CK = t_dn_ck(), NC = A->Tmax / CK + 1;
    const float *Sck = A->dS + (size_t)l * NC * NS, *o = A->dor + (size_t)l * T * in, *io = A->dio + (size_t)l * T * nv;
    const float *on = A->don + (size_t)l * T * in, *og = A->dog + (size_t)l * T * in;
    TPar *pcv = &tt->p[tt->dcv[l]], *psa = &tt->p[tt->dsa[l]], *pdt = &tt->p[tt->ddt[l]];
    const float *cw = tp_vec(pcv), *sa = tp_vec(psa), *dtb = tp_vec(pdt);
    /* saida e portao */
    float *dog = xalloc((size_t)T * in, 4), *don = xalloc((size_t)T * in, 4), *dz = xalloc((size_t)T * in, 4), *dor = xalloc((size_t)T * in, 4);
    tp_grad(&tt->p[tt->dout[l]], dy, og, T);
    tp_mvT(&tt->p[tt->dout[l]], dog, dy, T);
    for (size_t i = 0; i < (size_t)T * in; i++) {
        const float g = z[i], sg = 1.0f / (1.0f + expf(-g));
        don[i] = dog[i] * g * sg;
        dz[i] = dog[i] * on[i] * sg * (1.0f + g * (1.0f - sg));
    }
    t_rms_back(&tt->p[tt->dsn[l]], o, io, don, dor, T * nv, dv);
    /* recorrencia, de tras para frente; dS e o gradiente do estado depois do passo */
    float *dqk = xalloc((size_t)T * 2 * kd, 4), *dcs = xalloc((size_t)T * C, 4), *ddec = xalloc((size_t)T * nv, 4), *dbet = xalloc((size_t)T * nv, 4);
    float *dS = xalloc(NS, 4), *Sp = xalloc((size_t)dk * dv, 4);
    float *Sl = xalloc((size_t)(CK + 1) * NS, 4);           /* estados do bloco, refeitos a partir do ponto de controle */
    float kvv[512], dlt[512], ddl[512], dkv[512];
    int blk = -1;
    for (int t = T - 1; t >= 0; t--) {
        const int t0 = t / CK * CK;
        if (blk != t0) {
            blk = t0;
            memcpy(Sl, Sck + (size_t)(t0 / CK) * NS, NS * 4);
            const int t1 = t0 + CK < T ? t0 + CK : T;
            for (int u = t0; u < t1; u++)
                t_dn_step(c, qk + (size_t)u * 2 * kd, cs + (size_t)u * C, dec + (size_t)u * nv, bet + (size_t)u * nv,
                          Sl + (size_t)(u - t0) * NS, Sl + (size_t)(u - t0 + 1) * NS, NULL);
        }
        const float *S = Sl - (size_t)t0 * NS;              /* S + t*NS = estado antes do passo t */
    for (int h = 0; h < nv; h++) {
        const int kg = t_dn_group(c, h);
        const float *q = qk + (size_t)t * 2 * kd + (size_t)kg * dk, *k = qk + (size_t)t * 2 * kd + kd + (size_t)kg * dk;
        const float *v = cs + (size_t)t * C + 2 * kd + (size_t)h * dv;
        const float *S0 = S + (size_t)t * NS + (size_t)h * dk * dv, *Sn = S + (size_t)(t + 1) * NS + (size_t)h * dk * dv;
        const float *doh = dor + (size_t)t * in + (size_t)h * dv;
        float *dSh = dS + (size_t)h * dk * dv;
        float *dq = dqk + (size_t)t * 2 * kd + (size_t)kg * dk, *dk_ = dqk + (size_t)t * 2 * kd + kd + (size_t)kg * dk;
        float *dvv = dcs + (size_t)t * C + 2 * kd + (size_t)h * dv;
        const float dd = dec[(size_t)t * nv + h], bb = bet[(size_t)t * nv + h];
        /* refaz o passo: Sp = dd S0, kvv = Sp^T k, delta = bb (v - kvv) */
        for (int j = 0; j < dv; j++) kvv[j] = 0;
        for (int i = 0; i < dk; i++) for (int j = 0; j < dv; j++) { Sp[(size_t)i * dv + j] = dd * S0[(size_t)i * dv + j]; kvv[j] += Sp[(size_t)i * dv + j] * k[i]; }
        for (int j = 0; j < dv; j++) dlt[j] = bb * (v[j] - kvv[j]);
        /* o = Sn^T q */
        for (int i = 0; i < dk; i++) {
            float s = 0;
            for (int j = 0; j < dv; j++) { dSh[(size_t)i * dv + j] += q[i] * doh[j]; s += Sn[(size_t)i * dv + j] * doh[j]; }
            dq[i] += s;
        }
        /* Sn = Sp + k delta^T */
        for (int j = 0; j < dv; j++) ddl[j] = 0;
        for (int i = 0; i < dk; i++) {
            float s = 0;
            for (int j = 0; j < dv; j++) { ddl[j] += dSh[(size_t)i * dv + j] * k[i]; s += dSh[(size_t)i * dv + j] * dlt[j]; }
            dk_[i] += s;
        }
        /* delta = bb (v - kvv) */
        double db = 0;
        for (int j = 0; j < dv; j++) { dvv[j] += bb * ddl[j]; dkv[j] = -bb * ddl[j]; db += (double)ddl[j] * (v[j] - kvv[j]); }
        dbet[(size_t)t * nv + h] += (float)db;
        /* kvv = Sp^T k: dSp = dSn + k dkv^T */
        for (int i = 0; i < dk; i++) {
            float s = 0;
            for (int j = 0; j < dv; j++) { s += Sp[(size_t)i * dv + j] * dkv[j]; dSh[(size_t)i * dv + j] += k[i] * dkv[j]; }
            dk_[i] += s;
        }
        /* Sp = dd S0 */
        double sd = 0;
        for (size_t i = 0; i < (size_t)dk * dv; i++) { sd += (double)dSh[i] * S0[i]; dSh[i] *= dd; }
        ddec[(size_t)t * nv + h] += (float)sd;
    }
    }
    free(Sl);
    /* decaimento e beta -> alpha, dt_bias, ssm_a, beta */
    float *dal = xalloc((size_t)T * nv, 4), *dbe = xalloc((size_t)T * nv, 4);
    for (int t = 0; t < T; t++) for (int h = 0; h < nv; h++) {
        const size_t j = (size_t)t * nv + h;
        const float a = al[j] + dtb[h], sp = t_softplus(a), dd = dec[j];
        const float g = ddec[j] * dd;                    /* d(sa * sp) */
        if (psa->g) psa->g[h] += g * sp;
        const float da = g * sa[h] * (a > 20.0f ? 1.0f : 1.0f / (1.0f + expf(-a)));
        dal[j] = da;
        if (pdt->g) pdt->g[h] += da;
        dbe[j] = dbet[j] * bet[j] * (1.0f - bet[j]);
    }
    /* normas L2 de q e k: y = x * inv, inv = s / sqrt(x.x + eps) */
    for (int t = 0; t < T; t++) for (int g = 0; g < 2 * ng; g++) {
        const float *xg = cs + (size_t)t * C + (size_t)g * dk, *dyg = dqk + (size_t)t * 2 * kd + (size_t)g * dk;
        const float inv = iqk[(size_t)t * 2 * ng + g], s = g < ng ? 1.0f / sqrtf((float)dk) : 1.0f, r = inv / s;
        const float xd = dot(xg, dyg, dk);
        float *dxg = dcs + (size_t)t * C + (size_t)g * dk;
        for (int i = 0; i < dk; i++) dxg[i] += inv * dyg[i] - s * r * r * r * xg[i] * xd;
    }
    /* SiLU e convolucao */
    float *dcy = xalloc((size_t)T * C, 4), *draw = xalloc((size_t)T * C, 4);
    for (size_t i = 0; i < (size_t)T * C; i++) { const float g = cy[i], sg = 1.0f / (1.0f + expf(-g)); dcy[i] = dcs[i] * sg * (1.0f + g * (1.0f - sg)); }
    for (int t = 0; t < T; t++) for (int ch = 0; ch < C; ch++) {
        const float gy = dcy[(size_t)t * C + ch];
        if (gy == 0.0f) continue;
        for (int j = 0; j < K; j++) {
            const int ts = t - (K - 1) + j;
            if (ts < 0) continue;
            draw[(size_t)ts * C + ch] += cw[(size_t)ch * K + j] * gy;
            if (pcv->g) pcv->g[(size_t)ch * K + j] += gy * raw[(size_t)ts * C + ch];
        }
    }
    /* projecoes */
    tp_grad(&tt->p[tt->dqkv[l]], draw, x, T); tp_mvT(&tt->p[tt->dqkv[l]], dx, draw, T);
    tp_grad(&tt->p[tt->dz[l]], dz, x, T);     tp_mvT(&tt->p[tt->dz[l]], dx, dz, T);
    tp_grad(&tt->p[tt->dal[l]], dal, x, T);   tp_mvT(&tt->p[tt->dal[l]], dx, dal, T);
    tp_grad(&tt->p[tt->dbe[l]], dbe, x, T);   tp_mvT(&tt->p[tt->dbe[l]], dx, dbe, T);
    free(dog); free(don); free(dz); free(dor); free(dqk); free(dcs); free(ddec); free(dbet); free(dS); free(Sp);
    free(dal); free(dbe); free(dcy); free(draw);
}

/* forward da sequencia inteira guardando o necessario para voltar */
static void t_forward(Model *m, TTab *tt, const int *toks, TAct *A) {
    const Config *c = &m->cfg;
    const int T = A->T, d = c->dim, hd = c->hd, nh = c->n_heads, nkv = c->n_kv, qd = nh * hd, kvd = nkv * hd, H = A->H;
    const int group = nh / nkv;
    const float scale = 1.0f / sqrtf((float)hd) * (c->attn_gated ? c->kq_mul : 1.0f);
    float *x = A->x;                                     /* entrada da camada 0 */
    float *tmp = xalloc((size_t)T * (qd > d ? qd : d), 4);
    for (int t = 0; t < T; t++) {
        const TPar *e = &tt->p[tt->tok];
        if (e->w) memcpy(x + (size_t)t * d, e->w + (size_t)toks[t] * d, d * 4);
        else mat_row(x + (size_t)t * d, e->mat, toks[t], d);
    }
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *Ly = &m->layers[l];
        float *xl = A->x + (size_t)l * T * d, *xn1 = A->xn1 + (size_t)l * T * d, *q = A->q + (size_t)l * T * qd;
        float *k = A->k + (size_t)l * T * kvd, *v = A->v + (size_t)l * T * kvd, *P = A->P;
        float *qr = A->qr + (size_t)l * T * qd, *kr = A->kr + (size_t)l * T * kvd;
        float *ao = A->ao + (size_t)l * T * qd, *xm = A->xm + (size_t)l * T * d, *xn2 = A->xn2 + (size_t)l * T * d;
        t_rms(&tt->p[tt->an[l]], xl, xn1, A->inv1 + (size_t)l * T, T, d, c->eps);
        if (Ly->lin) {
            t_dn_fwd(m, tt, A, l, xn1, tmp);
            for (size_t i = 0; i < (size_t)T * d; i++) xm[i] = xl[i] + tmp[i];
            goto ffn;
        }
        if (c->attn_gated) {                                /* q e portao intercalados por cabeca */
            float *qg = A->qg + (size_t)l * T * 2 * qd;
            tp_mv(&tt->p[tt->q[l]], qg, xn1, T);
            for (int t = 0; t < T; t++) for (int hh = 0; hh < nh; hh++) memcpy(qr + (size_t)t * qd + hh * hd, qg + (size_t)t * 2 * qd + (size_t)hh * 2 * hd, hd * 4);
        } else { tp_mv(&tt->p[tt->q[l]], qr, xn1, T); t_bias(Ly->bq, qr, T, qd); }
        tp_mv(&tt->p[tt->k[l]], kr, xn1, T); t_bias(Ly->bk, kr, T, kvd);
        tp_mv(&tt->p[tt->v[l]], v, xn1, T); t_bias(Ly->bv, v, T, kvd);
        if (tt->qn[l] >= 0 && c->qk_norm == 2) {            /* norma por cabeca */
            t_rms(&tt->p[tt->qn[l]], qr, q, A->invq + (size_t)l * T * nh, T * nh, hd, c->eps);
            t_rms(&tt->p[tt->kn[l]], kr, k, A->invk + (size_t)l * T * nkv, T * nkv, hd, c->eps);
        } else if (tt->qn[l] >= 0) {                          /* norma no vetor inteiro */
            t_rms(&tt->p[tt->qn[l]], qr, q, A->invq + (size_t)l * T * nh, T, qd, c->eps);
            t_rms(&tt->p[tt->kn[l]], kr, k, A->invk + (size_t)l * T * nkv, T, kvd, c->eps);
        } else { memcpy(q, qr, (size_t)T * qd * 4); memcpy(k, kr, (size_t)T * kvd * 4); }
        for (int t = 0; t < T; t++) { t_rope(m, q + (size_t)t * qd, nh, t, 0); t_rope(m, k + (size_t)t * kvd, nkv, t, 0); }
        for (int hh = 0; hh < nh; hh++) {
            const int kvh = (hh / group) * hd;
            for (int t = 0; t < T; t++) {
                float *pr = P;
                const float *qh = q + (size_t)t * qd + hh * hd;
                for (int s = 0; s <= t; s++) pr[s] = dot(qh, k + (size_t)s * kvd + kvh, hd) * scale;
                softmax(pr, t + 1);
                float *o = ao + (size_t)t * qd + hh * hd;
                memset(o, 0, hd * 4);
                for (int s = 0; s <= t; s++) { const float w = pr[s]; const float *vh = v + (size_t)s * kvd + kvh; for (int i = 0; i < hd; i++) o[i] += w * vh[i]; }
            }
        }
        if (c->attn_gated) {                                /* saida de cada cabeca vezes sigmoide(portao) */
            const float *qg = A->qg + (size_t)l * T * 2 * qd;
            float *ap = A->apre + (size_t)l * T * qd;
            memcpy(ap, ao, (size_t)T * qd * 4);
            for (int t = 0; t < T; t++) for (int hh = 0; hh < nh; hh++) for (int i = 0; i < hd; i++) {
                const float g = qg[(size_t)t * 2 * qd + (size_t)hh * 2 * hd + hd + i];
                ao[(size_t)t * qd + hh * hd + i] = ap[(size_t)t * qd + hh * hd + i] / (1.0f + expf(-g));
            }
        }
        tp_mv(&tt->p[tt->o[l]], tmp, ao, T); t_bias(Ly->bo, tmp, T, d);
        for (size_t i = 0; i < (size_t)T * d; i++) xm[i] = xl[i] + tmp[i];
    ffn:
        t_rms(&tt->p[tt->fn[l]], xm, xn2, A->inv2 + (size_t)l * T, T, d, c->eps);
        if (tt->rt[l] >= 0) t_moe_fwd(m, tt, A, l, xn2, tmp);
        else t_glu_fwd(tt, tt->gt[l], tt->up[l], tt->dn[l], xn2, A->a + (size_t)l * T * H, A->u + (size_t)l * T * H, A->h + (size_t)l * T * H, tmp, T,
                       (int)tt->p[tt->gt[l]].rows);
        float *xnext = l + 1 < c->n_layers ? A->x + (size_t)(l + 1) * T * d : A->xf;
        for (size_t i = 0; i < (size_t)T * d; i++) xnext[i] = xm[i] + tmp[i];
    }
    t_rms(&tt->p[tt->onorm], A->xf, A->xnf, A->invf, T, d, c->eps);
    free(tmp);
}

static const TPar *t_head(const TTab *tt) { return tt->out >= 0 ? &tt->p[tt->out] : &tt->p[tt->tok]; }

/* perda media nos alvos marcados (mask[t] = 1: o token t+1 conta) e, se back, o passo para tras inteiro */
static double t_step(Model *m, TTab *tt, const int *toks, const uint8_t *mask, TAct *A, int back, int *ncount) {
    const Config *c = &m->cfg;
    const int T = A->T, d = c->dim, hd = c->hd, nh = c->n_heads, nkv = c->n_kv, qd = nh * hd, kvd = nkv * hd, H = A->H, V = c->vocab;
    const int group = nh / nkv;
    const float scale = 1.0f / sqrtf((float)hd) * (c->attn_gated ? c->kq_mul : 1.0f);
    t_forward(m, tt, toks, A);
    int N = 0;
    for (int t = 0; t + 1 < T; t++) N += mask[t] != 0;
    if (ncount) *ncount = N;
    if (!N) return 0.0;
    TPar *head = (TPar *)t_head(tt);
    float *lg = xalloc((size_t)T * V, 4);
    tp_mv(head, lg, A->xnf, T);
    double loss = 0;
    float *dl = back ? xalloc((size_t)T * V, 4) : NULL;
    for (int t = 0; t + 1 < T; t++) {
        if (!mask[t]) continue;
        float *z = lg + (size_t)t * V;
        float mx = z[0]; for (int i = 1; i < V; i++) if (z[i] > mx) mx = z[i];
        double se = 0; for (int i = 0; i < V; i++) se += exp((double)z[i] - mx);
        const int y = toks[t + 1];
        loss += -((double)z[y] - mx - log(se));
        if (back) {
            float *g = dl + (size_t)t * V;
            for (int i = 0; i < V; i++) g[i] = (float)(exp((double)z[i] - mx) / se) / N;
            g[y] -= 1.0f / N;
        }
    }
    free(lg);
    loss /= N;
    if (!back) return loss;

    /* ---- passo para tras ---- */
    float *dxn = xalloc((size_t)T * (qd > d ? qd : d), 4), *dx = xalloc((size_t)T * d, 4);
    tp_grad(head, dl, A->xnf, T);
    tp_mvT(head, dxn, dl, T);
    free(dl);
    t_rms_back(&tt->p[tt->onorm], A->xf, A->invf, dxn, dx, T, d);       /* dx = gradiente na saida da ultima camada */
    float *dxm = xalloc((size_t)T * d, 4), *dao = xalloc((size_t)T * qd, 4);
    float *dq = xalloc((size_t)T * qd, 4), *dk = xalloc((size_t)T * kvd, 4), *dv = xalloc((size_t)T * kvd, 4);
    float *dqr = xalloc((size_t)T * qd, 4), *dkr = xalloc((size_t)T * kvd, 4);
    float *dP = xalloc(T, 4);
    for (int l = c->n_layers - 1; l >= 0; l--) {
        float *xl = A->x + (size_t)l * T * d, *xn1 = A->xn1 + (size_t)l * T * d, *q = A->q + (size_t)l * T * qd;
        float *k = A->k + (size_t)l * T * kvd, *v = A->v + (size_t)l * T * kvd, *P = A->P;
        float *qr = A->qr + (size_t)l * T * qd, *kr = A->kr + (size_t)l * T * kvd;
        float *ao = A->ao + (size_t)l * T * qd, *xm = A->xm + (size_t)l * T * d, *xn2 = A->xn2 + (size_t)l * T * d;
        /* feed-forward: x_out = xm + ffn(norma(xm)) */
        memset(dxn, 0, (size_t)T * d * 4);
        if (tt->rt[l] >= 0) t_moe_back(m, tt, A, l, xn2, dx, dxn);
        else t_glu_back(tt, tt->gt[l], tt->up[l], tt->dn[l], xn2, A->a + (size_t)l * T * H, A->u + (size_t)l * T * H, A->h + (size_t)l * T * H,
                        dx, dxn, T, (int)tt->p[tt->gt[l]].rows);
        memcpy(dxm, dx, (size_t)T * d * 4);
        t_rms_back(&tt->p[tt->fn[l]], xm, A->inv2 + (size_t)l * T, dxn, dxm, T, d);
        if (m->layers[l].lin) {                              /* Gated DeltaNet */
            memset(dxn, 0, (size_t)T * d * 4);
            t_dn_back(m, tt, A, l, xn1, dxm, dxn);
            memcpy(dx, dxm, (size_t)T * d * 4);
            t_rms_back(&tt->p[tt->an[l]], xl, A->inv1 + (size_t)l * T, dxn, dx, T, d);
            continue;
        }
        /* atencao: xm = x + wo(ao) */
        memset(dao, 0, (size_t)T * qd * 4);
        tp_grad(&tt->p[tt->o[l]], dxm, ao, T);
        tp_mvT(&tt->p[tt->o[l]], dao, dxm, T);
        float *dgt = NULL;
        if (c->attn_gated) {                                /* portao: dao vira o gradiente da saida antes do portao */
            const float *qg = A->qg + (size_t)l * T * 2 * qd, *ap = A->apre + (size_t)l * T * qd;
            dgt = xalloc((size_t)T * qd, 4);
            for (int t = 0; t < T; t++) for (int hh = 0; hh < nh; hh++) for (int i = 0; i < hd; i++) {
                const size_t j = (size_t)t * qd + hh * hd + i;
                const float g = qg[(size_t)t * 2 * qd + (size_t)hh * 2 * hd + hd + i], sg = 1.0f / (1.0f + expf(-g));
                dgt[j] = dao[j] * ap[j] * sg * (1.0f - sg);
                dao[j] *= sg;
            }
        }
        memset(dq, 0, (size_t)T * qd * 4); memset(dk, 0, (size_t)T * kvd * 4); memset(dv, 0, (size_t)T * kvd * 4);
        for (int hh = 0; hh < nh; hh++) {
            const int kvh = (hh / group) * hd;
            for (int t = 0; t < T; t++) {
                const float *qh0 = q + (size_t)t * qd + hh * hd, *dout = dao + (size_t)t * qd + hh * hd;
                for (int s = 0; s <= t; s++) P[s] = dot(qh0, k + (size_t)s * kvd + kvh, hd) * scale;     /* refaz a linha */
                softmax(P, t + 1);
                const float *pr = P;
                double sum = 0;
                for (int s = 0; s <= t; s++) {
                    const float *vh = v + (size_t)s * kvd + kvh;
                    dP[s] = dot(dout, vh, hd);
                    sum += (double)pr[s] * dP[s];
                    float *dvh = dv + (size_t)s * kvd + kvh;
                    for (int i = 0; i < hd; i++) dvh[i] += pr[s] * dout[i];
                }
                const float *qh = q + (size_t)t * qd + hh * hd;
                float *dqh = dq + (size_t)t * qd + hh * hd;
                for (int s = 0; s <= t; s++) {
                    const float ds = pr[s] * (dP[s] - (float)sum) * scale;
                    if (ds == 0.0f) continue;
                    const float *kh = k + (size_t)s * kvd + kvh;
                    float *dkh = dk + (size_t)s * kvd + kvh;
                    for (int i = 0; i < hd; i++) { dqh[i] += ds * kh[i]; dkh[i] += ds * qh[i]; }
                }
            }
        }
        for (int t = 0; t < T; t++) { t_rope(m, dq + (size_t)t * qd, nh, t, 1); t_rope(m, dk + (size_t)t * kvd, nkv, t, 1); }
        if (tt->qn[l] >= 0) {                                /* volta da norma de q e k */
            memset(dqr, 0, (size_t)T * qd * 4); memset(dkr, 0, (size_t)T * kvd * 4);
            if (c->qk_norm == 2) {
                t_rms_back(&tt->p[tt->qn[l]], qr, A->invq + (size_t)l * T * nh, dq, dqr, T * nh, hd);
                t_rms_back(&tt->p[tt->kn[l]], kr, A->invk + (size_t)l * T * nkv, dk, dkr, T * nkv, hd);
            } else {
                t_rms_back(&tt->p[tt->qn[l]], qr, A->invq + (size_t)l * T * nh, dq, dqr, T, qd);
                t_rms_back(&tt->p[tt->kn[l]], kr, A->invk + (size_t)l * T * nkv, dk, dkr, T, kvd);
            }
        } else { memcpy(dqr, dq, (size_t)T * qd * 4); memcpy(dkr, dk, (size_t)T * kvd * 4); }
        memset(dxn, 0, (size_t)T * d * 4);
        if (c->attn_gated) {                                /* volta a intercalar q e portao */
            float *dqg = xalloc((size_t)T * 2 * qd, 4);
            for (int t = 0; t < T; t++) for (int hh = 0; hh < nh; hh++) {
                memcpy(dqg + (size_t)t * 2 * qd + (size_t)hh * 2 * hd, dqr + (size_t)t * qd + hh * hd, hd * 4);
                memcpy(dqg + (size_t)t * 2 * qd + (size_t)hh * 2 * hd + hd, dgt + (size_t)t * qd + hh * hd, hd * 4);
            }
            tp_grad(&tt->p[tt->q[l]], dqg, xn1, T); tp_mvT(&tt->p[tt->q[l]], dxn, dqg, T);
            free(dqg); free(dgt);
        } else { tp_grad(&tt->p[tt->q[l]], dqr, xn1, T); tp_mvT(&tt->p[tt->q[l]], dxn, dqr, T); }
        tp_grad(&tt->p[tt->k[l]], dkr, xn1, T); tp_mvT(&tt->p[tt->k[l]], dxn, dkr, T);
        tp_grad(&tt->p[tt->v[l]], dv, xn1, T); tp_mvT(&tt->p[tt->v[l]], dxn, dv, T);
        memcpy(dx, dxm, (size_t)T * d * 4);
        t_rms_back(&tt->p[tt->an[l]], xl, A->inv1 + (size_t)l * T, dxn, dx, T, d);
    }
    TPar *e = &tt->p[tt->tok];
    if (e->g) for (int t = 0; t < T; t++) { float *g = e->g + (size_t)toks[t] * d; const float *s = dx + (size_t)t * d; for (int i = 0; i < d; i++) g[i] += s[i]; }
    free(dxn); free(dx); free(dxm); free(dao); free(dq); free(dk); free(dv); free(dqr); free(dkr); free(dP);
    return loss;
}

/* ---- arquivo de especializacao (.kspec) ----
 * "KSPEC1\0\0", impressao do modelo base (tamanho do arquivo, parametros), quantidade de tensores; cada um: nome,
 * linhas, colunas e os pesos em float32. Carregado com KATANA_SPEC: troca so esses tensores do modelo base. */
#define KSPEC_MAGIC "KSPEC2\0\0"
static int spec_save(const Model *m, const TTab *tt, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int n = 0;
    for (int i = 0; i < tt->n; i++) n += tt->p[i].w != NULL || tt->p[i].r;
    const int64_t fs = m->fsize; const uint64_t np = m->n_params;
    fwrite(KSPEC_MAGIC, 1, 8, f); fwrite(&fs, 8, 1, f); fwrite(&np, 8, 1, f); fwrite(&n, 4, 1, f);
    for (int i = 0; i < tt->n; i++) {
        const TPar *p = &tt->p[i];
        if (!p->w && !p->r) continue;
        const uint32_t nl = (uint32_t)strlen(p->name), kind = p->r ? 1 : 0; const uint64_t r = p->rows, cc = p->cols;
        fwrite(&nl, 4, 1, f); fwrite(p->name, 1, nl, f); fwrite(&kind, 4, 1, f); fwrite(&r, 8, 1, f); fwrite(&cc, 8, 1, f);
        if (p->r) {                                    /* adaptador: posto, escala, A e B */
            const int32_t rk = p->r; fwrite(&rk, 4, 1, f); fwrite(&p->s, 4, 1, f);
            fwrite(p->a, 4, (size_t)p->r * cc, f); fwrite(p->b, 4, (size_t)(r * p->r), f);
        } else fwrite(p->w, 4, (size_t)(r * cc), f);
    }
    const int ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

/* aplica a especializacao de KATANA_SPEC sobre o modelo carregado (antes de preparar a GPU) */
static void spec_apply(Model *m) {
    const char *path = getenv("KATANA_SPEC");
    if (!path || !*path) return;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "KATANA_SPEC: nao abriu %s\n", path); exit(1); }
    char mg[8]; int64_t fs = 0; uint64_t np = 0; int n = 0;
    if (fread(mg, 1, 8, f) != 8 || memcmp(mg, KSPEC_MAGIC, 8) || fread(&fs, 8, 1, f) != 1 || fread(&np, 8, 1, f) != 1 || fread(&n, 4, 1, f) != 1 || n < 0)
        die("KATANA_SPEC: arquivo de especializacao invalido");
    if (fs != m->fsize || np != m->n_params) die("KATANA_SPEC: a especializacao foi treinada sobre outro modelo base");
    TTab tt = {0};
    const char *err = tt_build(m, &tt);
    if (err) { fprintf(stderr, "KATANA_SPEC: %s\n", err); exit(1); }
    for (int k = 0; k < n; k++) {
        uint32_t nl = 0, kind = 0; uint64_t r = 0, cc = 0; char name[128];
        if (fread(&nl, 4, 1, f) != 1 || nl == 0 || nl >= sizeof name || fread(name, 1, nl, f) != nl || fread(&kind, 4, 1, f) != 1
            || kind > 1 || fread(&r, 8, 1, f) != 1 || fread(&cc, 8, 1, f) != 1)
            die("KATANA_SPEC: arquivo cortado");
        name[nl] = 0;
        TPar *p = NULL;
        for (int i = 0; i < tt.n && !p; i++) if (!strcmp(tt.p[i].name, name)) p = &tt.p[i];
        if (!p || p->rows != r || p->cols != cc) { fprintf(stderr, "KATANA_SPEC: tensor %s nao confere com o modelo\n", name); exit(1); }
        float *w = xalloc((size_t)(r * cc), sizeof(float));
        if (kind == 1) {        /* adaptador: W = W_base + s * B A, desquantizando so esta matriz */
            int32_t rk = 0; float sc = 0;
            if (!p->mat || fread(&rk, 4, 1, f) != 1 || fread(&sc, 4, 1, f) != 1 || rk < 1 || rk > 4096) die("KATANA_SPEC: adaptador invalido");
            float *A = xalloc((size_t)rk * cc, 4), *Bm = xalloc((size_t)r * rk, 4);
            if (fread(A, 4, (size_t)rk * cc, f) != (size_t)rk * cc || fread(Bm, 4, (size_t)(r * rk), f) != (size_t)(r * rk)) die("KATANA_SPEC: arquivo cortado");
            for (uint64_t i = 0; i < r; i++) {
                float *wi = w + i * cc;
                mat_row(wi, p->mat, (int)i, (int)cc);
                for (int kk = 0; kk < rk; kk++) { const float bk = sc * Bm[i * rk + kk]; if (bk == 0.0f) continue; const float *ak = A + (size_t)kk * cc; for (uint64_t c2 = 0; c2 < cc; c2++) wi[c2] += bk * ak[c2]; }
            }
            free(A); free(Bm);
        } else if (fread(w, 4, (size_t)(r * cc), f) != (size_t)(r * cc)) die("KATANA_SPEC: arquivo cortado");
        if (p->vec_w) { *p->vec_w = w; continue; }
        if (strstr(name, "_exps.weight.e") && !getenv("KATANA_CPU")) {      /* o cache de especialistas da GPU supoe um formato so */
            putenv_dup("KATANA_CPU=1");
            fprintf(stderr, "especializacao com especialistas treinados: rodando na CPU (o cache da GPU ainda nao mistura formatos)\n");
        }
        const int was_head = p->mat_w == &m->tok_emb && m->lm_head.f == m->tok_emb.f && m->lm_head.q == m->tok_emb.q;
        memset(p->mat_w, 0, sizeof(Mat));
        p->mat_w->f = w;
        if (was_head) m->lm_head = m->tok_emb;       /* saida amarrada ao embedding: continua amarrada */
    }
    fclose(f);
    fprintf(stderr, "especializacao: %s (%d tensores)\n", path, n);
    free(tt.p); free(tt.an);
}

/* ---- dados: JSONL com "text" e "train_spans" (posicoes em caracteres), como o ksess2jsonl.py grava ---- */
typedef struct { Ids ids; uint8_t *mask; } TEx;

static int t_load_line(const Model *m, const char *line, TEx *ex) {
    char k[64]; const char *v, *q = js_ws(line), *e;
    if (*q != '{') return 0;
    q++;
    Buf text = {0};
    int *sp = NULL, nsp = 0, have_spans = 0;
    while ((e = gs_member(q, k, sizeof k, &v))) {
        if (!strcmp(k, "text") && *v == '"') { const char *p = v; buf_clear(&text); if (!js_string(&p, &text)) { free(text.p); free(sp); return 0; } }
        else if (!strcmp(k, "train_spans") && *v == '[') {
            have_spans = 1;
            const char *q2 = v + 1, *el, *e2;
            while ((e2 = gs_elem(q2, &el))) {
                if (*el == '[') {
                    const char *p = el + 1; char *end;
                    const long a = strtol(p, &end, 10); p = end; while (*p == ' ' || *p == ',') p++;
                    const long b = strtol(p, &end, 10);
                    sp = realloc(sp, sizeof(int) * (size_t)(nsp + 1) * 2); sp[2 * nsp] = (int)a; sp[2 * nsp + 1] = (int)b; nsp++;
                }
                q2 = e2;
            }
        }
        q = e;
    }
    if (!text.n) { free(text.p); free(sp); return 0; }
    /* posicao em caracteres -> byte (o JSON conta caracteres Unicode) */
    int *c2b = xalloc(text.n + 2, sizeof(int)); int nc = 0;
    for (size_t i = 0; i < text.n; ) {
        c2b[nc++] = (int)i;
        const unsigned char ch = (unsigned char)text.p[i];
        i += ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4;
    }
    c2b[nc] = (int)text.n;
    memset(ex, 0, sizeof *ex);
    if (m->cfg.add_bos && m->cfg.bos >= 0) ids_push(&ex->ids, m->cfg.bos);
    const int nb = ex->ids.n;
    encode(&m->tk, text.p, &ex->ids);
    /* bytes de cada token, para saber quais caem nos trechos treinados */
    int *tb = xalloc((size_t)ex->ids.n + 1, sizeof(int));
    size_t pos = 0; char tmp[256];
    for (int i = 0; i < ex->ids.n; i++) { tb[i] = (int)pos; if (i >= nb) pos += tok_bytes(&m->tk, ex->ids.ids[i], tmp, sizeof tmp); }
    tb[ex->ids.n] = (int)pos;
    if (pos != text.n) {                        /* tokens nao refazem o texto: os trechos nao dariam certo */
        fprintf(stderr, "aviso: exemplo ignorado (os tokens nao reproduzem o texto: %zu de %zu bytes)\n", pos, text.n);
        free(c2b); free(tb); free(sp); free(text.p); free(ex->ids.ids); memset(ex, 0, sizeof *ex); return 0;
    }
    ex->mask = xalloc((size_t)ex->ids.n, 1);
    for (int t = 0; t + 1 < ex->ids.n; t++) {     /* mask[t]: o alvo e o token t+1 */
        const int j = t + 1;
        if (j < nb) continue;
        if (!have_spans) { ex->mask[t] = 1; continue; }
        for (int s = 0; s < nsp; s++) {
            const int a = sp[2 * s] < 0 ? 0 : sp[2 * s] > nc ? (int)text.n : c2b[sp[2 * s]];
            const int b = sp[2 * s + 1] < 0 ? 0 : sp[2 * s + 1] > nc ? (int)text.n : c2b[sp[2 * s + 1]];
            if (tb[j] < b && tb[j + 1] > a) { ex->mask[t] = 1; break; }
        }
    }
    free(c2b); free(tb); free(sp); free(text.p);
    return 1;
}

static void t_clear_grads(TTab *tt) {
    for (int i = 0; i < tt->n; i++) {
        TPar *p = &tt->p[i];
        if (p->g) memset(p->g, 0, p->rows * p->cols * 4);
        if (p->r) { memset(p->ga, 0, (size_t)p->r * p->cols * 4); memset(p->gb, 0, p->rows * (size_t)p->r * 4); }
    }
}
static void adam_upd(float *w, const float *gr, float *m1, float *m2, size_t n, float sc, float lr, float wd, float c1, float c2) {
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    for (size_t j = 0; j < n; j++) {
        const float g = gr[j] * sc;
        m1[j] = b1 * m1[j] + (1 - b1) * g;
        m2[j] = b2 * m2[j] + (1 - b2) * g * g;
        w[j] -= lr * ((m1[j] / c1) / (sqrtf(m2[j] / c2) + eps) + wd * w[j]);
    }
}

/* AdamW com corte do gradiente pela norma global; devolve a norma antes do corte */
static double t_adamw(TTab *tt, int step, float lr, float wd, float clip) {
    double ss = 0;
    for (int i = 0; i < tt->n; i++) {
        const TPar *p = &tt->p[i];
        if (p->g) for (size_t j = 0; j < p->rows * p->cols; j++) ss += (double)p->g[j] * p->g[j];
        if (p->r) {
            for (size_t j = 0; j < (size_t)p->r * p->cols; j++) ss += (double)p->ga[j] * p->ga[j];
            for (size_t j = 0; j < p->rows * (size_t)p->r; j++) ss += (double)p->gb[j] * p->gb[j];
        }
    }
    const double norm = sqrt(ss);
    const float sc = clip > 0 && norm > clip ? (float)(clip / norm) : 1.0f;
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    const float c1 = 1.0f - powf(b1, (float)step), c2 = 1.0f - powf(b2, (float)step);
    for (int i = 0; i < tt->n; i++) {
        TPar *p = &tt->p[i];
        if (p->r) {
            adam_upd(p->a, p->ga, p->ma1, p->ma2, (size_t)p->r * p->cols, sc, lr, wd, c1, c2);
            adam_upd(p->b, p->gb, p->mb1, p->mb2, p->rows * (size_t)p->r, sc, lr, wd, c1, c2);
        }
        if (!p->g) continue;
        const int is_vec = p->vec != NULL;
        for (size_t j = 0; j < p->rows * p->cols; j++) {
            const float g = p->g[j] * sc;
            p->m1[j] = b1 * p->m1[j] + (1 - b1) * g;
            p->m2[j] = b2 * p->m2[j] + (1 - b2) * g * g;
            const float upd = (p->m1[j] / c1) / (sqrtf(p->m2[j] / c2) + eps);
            p->w[j] -= lr * (upd + (is_vec ? 0.0f : wd * p->w[j]));
        }
    }
    return norm;
}

/* katana gradcheck <modelo>: forward de treino x forward do motor, e gradientes x diferencas finitas */
static int train_gradcheck(Model *m, int T) {
    const Config *c = &m->cfg;
    TTab tt = {0};
    const char *err = tt_build(m, &tt);
    if (err) { fprintf(stderr, "%s\n", err); return 1; }
    if (T > c->ctx) T = c->ctx;
    srand(7);
    int *toks = xalloc(T, sizeof(int));
    for (int t = 0; t < T; t++) toks[t] = rand() % c->vocab;
    uint8_t *mask = xalloc(T, 1);
    for (int t = 0; t < T; t++) mask[t] = (t % 3) != 0;
    TAct A; tact_alloc(&A, c, T);
    /* 1. o forward de treino tem que dar os mesmos logits que o motor, em todas as posicoes */
    t_forward(m, &tt, toks, &A);
    float *lg = xalloc((size_t)T * c->vocab, 4);
    tp_mv(t_head(&tt), lg, A.xnf, T);
    double worst = 0;
    for (int t = 0; t < T; t++) {
        const float *ref = forward(m, toks[t], t, 1);
        for (int i = 0; i < c->vocab; i++) { const double dd = fabs((double)ref[i] - lg[(size_t)t * c->vocab + i]); if (dd > worst) worst = dd; }
    }
    printf("forward de treino x motor: maior diferenca nos logits %.2e (%d posicoes)\n", worst, T);
    int fails = worst > 1e-3;
    /* 2. gradiente analitico x diferenca central na direcao de um vetor aleatorio, tensor por tensor
          (a direcao inteira da um sinal muito maior que um peso so, entao o arredondamento do float pesa pouco) */
    tt_select(&tt, "all");
    double maxrel = 0;
    int checked = 0;
    for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) {        /* segunda volta: adaptadores de posto 3 nas matrizes, com B aleatorio */
        for (int i = 0; i < tt.n; i++) { TPar *p = &tt.p[i]; if (p->mat && i != tt.tok) { free(p->w); free(p->g); free(p->m1); free(p->m2); p->w = p->g = p->m1 = p->m2 = NULL; } }
        /* normas e embedding seguem inteiros */
        for (int i = 0; i < tt.n; i++) {
            TPar *p = &tt.p[i];
            if (!p->mat || i == tt.tok) continue;
            p->r = 3; p->s = 2.0f / 3;
            p->a = xalloc(3 * p->cols, 4); p->b = xalloc(p->rows * 3, 4); p->ga = xalloc(3 * p->cols, 4); p->gb = xalloc(p->rows * 3, 4);
            for (size_t j = 0; j < 3 * p->cols; j++) p->a[j] = ((float)rand() / RAND_MAX - 0.5f) * 0.2f;
            for (size_t j = 0; j < p->rows * 3; j++) p->b[j] = ((float)rand() / RAND_MAX - 0.5f) * 0.2f;
        }
    }
    t_clear_grads(&tt);
    A.fixsel = 0;
    t_step(m, &tt, toks, mask, &A, 1, NULL);
    A.fixsel = 1;          /* MoE: as diferencas finitas usam os mesmos escolhidos (a derivada so existe com a escolha fixa) */
    for (int i = 0; i < tt.n; i++) for (int part = 0; part < 3; part++) {
        TPar *p = &tt.p[i];
        float *W = part == 0 ? p->w : part == 1 ? p->a : p->b;
        const float *G = part == 0 ? p->g : part == 1 ? p->ga : p->gb;
        if (!W) continue;
        const size_t n = part == 0 ? p->rows * p->cols : part == 1 ? (size_t)p->r * p->cols : p->rows * (size_t)p->r;
        float *u = xalloc(n, 4), *w0 = xalloc(n, 4);
        memcpy(w0, W, n * 4);
        double wn = 0, un = 0, gu = 0;
        for (size_t j = 0; j < n; j++) wn += (double)w0[j] * w0[j];
        for (size_t j = 0; j < n; j++) {
            const int used = part || i != tt.tok || (int)(j / p->cols) < c->vocab;
            u[j] = used ? (float)rand() / RAND_MAX - 0.5f : 0.0f;
            un += (double)u[j] * u[j];
        }
        const double hs = getenv("KATANA_GC_H") ? atof(getenv("KATANA_GC_H")) : 1.0;   /* escala do passo (para conferir a convergencia) */
        const double wl = 1e-3 * sqrt(wn), h = hs * (wl > 2e-2 ? wl : 2e-2) / sqrt(un);    /* passo com piso: tensor pequeno da sinal pequeno */
        for (size_t j = 0; j < n; j++) gu += (double)G[j] * u[j];
        double nd[2];
        for (int r = 0; r < 2; r++) {                     /* diferenca central com h e h/2 */
            const double hh = r ? h / 2 : h;
            for (size_t j = 0; j < n; j++) W[j] = w0[j] + (float)(hh * u[j]);
            const double lp = t_step(m, &tt, toks, mask, &A, 0, NULL);
            for (size_t j = 0; j < n; j++) W[j] = w0[j] - (float)(hh * u[j]);
            const double lm = t_step(m, &tt, toks, mask, &A, 0, NULL);
            nd[r] = (lp - lm) / (2.0 * hh);
        }
        memcpy(W, w0, n * 4);
        /* duas estimativas: diferenca central simples (menos ruido do float) e Richardson (sem o erro de curvatura de
           ordem h^2); vale a que chega mais perto. Um gradiente errado erra nas duas. */
        const double nr = (4.0 * nd[1] - nd[0]) / 3.0;
        const double num = fabs(nr - gu) < fabs(nd[0] - gu) ? nr : nd[0];
        const double rel = fabs(num - gu) / (fabs(num) + fabs(gu) + 1e-8);
        /* tolerancia: 1% relativo mais um piso absoluto de 1e-4 (o arredondamento do float domina nos gradientes pequenos) */
        if (getenv("KATANA_GC_VERBOSE") && rel > 1e-2) printf("  (dentro da tolerancia) %s part %d: %.3g x %.3g\n", p->name, part, gu, num);
        if (rel > maxrel) maxrel = rel;
        if (fabs(num - gu) > 1e-2 * (fabs(num) + fabs(gu)) + 1e-4) { printf("  %s%s: analitico %.6g numerico %.6g (rel %.3g)\n", p->name, part == 1 ? " (A)" : part == 2 ? " (B)" : "", gu, num, rel); fails++; }
        checked++;
        free(u); free(w0);
    }
    }
    printf("gradientes: %d conferidos (tensores inteiros e adaptadores A/B, derivada direcional), maior erro relativo %.2e\n", checked, maxrel);
    printf(fails ? "gradcheck: FALHOU\n" : "gradcheck: ok\n");
    tact_free(&A); free(lg); free(toks); free(mask);
    return fails != 0;
}

/* katana train <modelo> <dados.jsonl> [--out f.kspec] [--train padroes] [--steps N] [--lr X] [--seq T] [--wd X] [--seed N] */
static int train_main(Model *m, const char *model_path, int argc, char **argv) {
    const Config *c = &m->cfg;
    const char *data = argv[0], *pats = "all";
    char out[1100]; snprintf(out, sizeof out, "%s.kspec", model_path);
    int steps = 200, seq = 256, every = 10, rank = 0, hot = 0, pats_set = 0, plan = 0; float lr = 1e-3f, wd = 0.0f, clip = 1.0f, alpha = 0; unsigned seed = 1;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *k = argv[i], *v = argv[i + 1];
        if (!strcmp(k, "--out")) snprintf(out, sizeof out, "%s", v);
        else if (!strcmp(k, "--train")) { pats = v; pats_set = 1; }
        else if (!strcmp(k, "--experts")) hot = atoi(v);
        else if (!strcmp(k, "--plan")) plan = atoi(v);
        else if (!strcmp(k, "--steps")) steps = atoi(v);
        else if (!strcmp(k, "--lr")) lr = (float)atof(v);
        else if (!strcmp(k, "--seq")) seq = atoi(v);
        else if (!strcmp(k, "--wd")) wd = (float)atof(v);
        else if (!strcmp(k, "--clip")) clip = (float)atof(v);
        else if (!strcmp(k, "--seed")) seed = (unsigned)atoi(v);
        else if (!strcmp(k, "--every")) every = atoi(v);
        else if (!strcmp(k, "--rank")) rank = atoi(v);
        else if (!strcmp(k, "--alpha")) alpha = (float)atof(v);
        else { fprintf(stderr, "opcao desconhecida: %s\n", k); return 1; }
    }
    if (seq > c->ctx) seq = c->ctx;
    if (seq < 2) seq = 2;
    TTab tt = {0};
    const char *err = tt_build(m, &tt);
    if (err) { fprintf(stderr, "%s\n", err); return 1; }
    /* dados: cada exemplo vira janelas de ate seq tokens (as que tem algum alvo marcado) */
    FILE *f = fopen(data, "rb");
    if (!f) { fprintf(stderr, "nao abriu %s\n", data); return 1; }
    TEx *ex = NULL; int nex = 0, cap = 0;
    size_t lc = 1 << 20; char *line = xalloc(lc, 1);
    for (;;) {
        size_t n = 0; int ch;
        while ((ch = fgetc(f)) != EOF && ch != '\n') { if (n + 2 >= lc) { lc *= 2; line = realloc(line, lc); } line[n++] = (char)ch; }
        line[n] = 0;
        if (n) {
            TEx e;
            if (t_load_line(m, line, &e)) {
                for (int s0 = 0; s0 < e.ids.n - 1; s0 += seq - 1) {      /* janelas com um token de sobreposicao */
                    const int len = e.ids.n - s0 < seq ? e.ids.n - s0 : seq;
                    int any = 0; for (int t = 0; t + 1 < len; t++) any |= e.mask[s0 + t];
                    if (!any || len < 2) continue;
                    if (nex == cap) { cap = cap * 2 + 16; ex = realloc(ex, sizeof(TEx) * (size_t)cap); }
                    TEx *w = &ex[nex++];
                    memset(w, 0, sizeof *w);
                    for (int t = 0; t < len; t++) ids_push(&w->ids, e.ids.ids[s0 + t]);
                    w->mask = xalloc((size_t)len, 1); memcpy(w->mask, e.mask + s0, (size_t)len);
                    if (len > 1) w->mask[len - 1] = 0;
                }
                free(e.ids.ids); free(e.mask);
            }
        }
        if (ch == EOF) break;
    }
    fclose(f); free(line);
    if (!nex) { fprintf(stderr, "nenhum exemplo com alvo marcado em %s\n", data); return 1; }
    srand(seed);
    size_t ntr = 0;
    if (hot > 0) {
        if (!tt.moe) { fprintf(stderr, "--experts: o modelo nao tem camadas MoE\n"); return 1; }
        /* especializacao guiada pelo calor: em cada camada, os N especialistas que o roteador do modelo base mais
           escolhe nas posicoes que contam para a perda destes dados */
        const int L = c->n_layers, E = c->n_expert, K = c->n_used;
        if (hot > E) hot = E;
        long *cnt = xalloc((size_t)L * E, sizeof(long));
        TAct H0; tact_alloc(&H0, c, seq);
        for (int i = 0; i < nex; i++) {
            H0.T = ex[i].ids.n;
            t_forward(m, &tt, ex[i].ids.ids, &H0);
            for (int l = 0; l < L; l++) {
                if (tt.rt[l] < 0) continue;
                for (int t = 0; t + 1 < H0.T; t++) if (ex[i].mask[t]) for (int k = 0; k < K; k++) cnt[(size_t)l * E + H0.sel[((size_t)l * H0.T + t) * K + k]]++;
            }
        }
        H0.T = seq; tact_free(&H0);
        double cov = 0; int nl = 0;
        for (int l = 0; l < L; l++) {
            if (tt.rt[l] < 0) continue;
            long tot = 0, got = 0;
            for (int e = 0; e < E; e++) tot += cnt[(size_t)l * E + e];
            for (int j = 0; j < hot; j++) {
                int best = -1;
                for (int e = 0; e < E; e++) if (cnt[(size_t)l * E + e] >= 0 && (best < 0 || cnt[(size_t)l * E + e] > cnt[(size_t)l * E + best])) best = e;
                if (best < 0) break;
                got += cnt[(size_t)l * E + best];
                cnt[(size_t)l * E + best] = -1;               /* ja escolhido */
                for (int part = 0; part < 3; part++) ntr += tp_enable(&tt, tt.ex0[l] + 3 * best + part, rank, alpha > 0 ? alpha : (float)rank);
            }
            cov += tot ? (double)got / tot : 0; nl++;
        }
        printf("especialistas quentes: %d por camada; cobrem em media %.0f%% das escolhas do roteador nestes dados\n", hot, nl ? 100.0 * cov / nl : 0.0);
        free(cnt);
        if (!pats_set) pats = "blk.*.ffn_gate_inp.weight";    /* padrao com --experts: os quentes e o roteador */
    }
    ntr += tt_select_r(&tt, pats, rank, alpha > 0 ? alpha : (float)rank);
    if (!ntr) { fprintf(stderr, "nenhum tensor casa com --train %s\n", pats); return 1; }
    int nt = 0, na = 0; for (int i = 0; i < tt.n; i++) { nt += tt.p[i].w != NULL; na += tt.p[i].r != 0; }
    long tgt = 0; for (int i = 0; i < nex; i++) for (int t = 0; t < ex[i].ids.n; t++) tgt += ex[i].mask[t];
    printf("treino: %d janelas (%ld alvos), %d tensores inteiros + %d adaptadores (posto %d), %.3fM pesos treinaveis (%.1f MB de estado), passos %d, lr %g\n",
           nex, tgt, nt, na, rank, ntr / 1e6, ntr * 16.0 / 1048576.0, steps, lr);
    srand(seed);
    TAct A; tact_alloc(&A, c, seq);
    printf("memoria: ativacoes de uma janela de %d tokens %.0f MB; estado do treino %.0f MB; modelo base mapeado do arquivo (%.1f GB)\n",
           seq, g_ta_bytes / 1048576.0, ntr * 16.0 / 1048576.0, m->fsize / 1073741824.0);
    if (plan) { printf("--plan 1: so o plano, sem treinar\n"); tact_free(&A); return 0; }
    double ema = -1;
    const double t0 = now_sec();
    for (int st = 1; st <= steps; st++) {
        const TEx *e = &ex[rand() % nex];
        A.T = e->ids.n;
        t_clear_grads(&tt);
        int N = 0;
        const double loss = t_step(m, &tt, e->ids.ids, e->mask, &A, 1, &N);
        const float lr_t = lr * (st <= 10 ? (float)st / 10.0f : 1.0f);      /* aquecimento curto */
        const double gn = t_adamw(&tt, st, lr_t, wd, clip);
        ema = ema < 0 ? loss : 0.9 * ema + 0.1 * loss;
        if (st == 1 || st % every == 0 || st == steps)
            printf("passo %4d  perda %.4f  media %.4f  |grad| %.3f  (%d alvos, %.1f s)\n", st, loss, ema, gn, N, now_sec() - t0);
        fflush(stdout);
    }
    A.T = seq;
    /* perda final em todas as janelas */
    double sum = 0; int cnt = 0;
    for (int i = 0; i < nex; i++) { A.T = ex[i].ids.n; int N = 0; const double l = t_step(m, &tt, ex[i].ids.ids, ex[i].mask, &A, 0, &N); if (N) { sum += l * N; cnt += N; } }
    printf("perda media final: %.4f em %d alvos\n", cnt ? sum / cnt : 0.0, cnt);
    if (!spec_save(m, &tt, out)) { fprintf(stderr, "nao consegui gravar %s\n", out); return 1; }
    printf("especializacao gravada: %s (use KATANA_SPEC=%s)\n", out, out);
    A.T = seq; tact_free(&A);
    return 0;
}
