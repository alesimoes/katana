/* train_methods.c: metodos de ajuste fino publicados + diagnostico honesto do treino (incluido por train.c, depois de
 * tset_loss). Tudo aqui e opcao, desligada por padrao; sem opcao o treino e o de antes.
 *
 * Seguranca e diagnostico (sempre ligados quando ha validacao):
 *   - perda de validacao do modelo BASE antes do passo 1 (tm_off desliga adaptadores, mestres e magnitudes) e a
 *     conferencia de que adaptadores zerados dao exatamente o modelo base;
 *   - o arquivo so e gravado se a validacao vence a base (--allow-worse grava mesmo assim); perda por fonte;
 *   - guardas: perda ou gradiente nao finitos, ou perda muito acima da mediana corrente, pulam o passo; varios
 *     seguidos param o treino;
 *   - katana compare: perda em janelas separadas por fonte e deriva (KL medio em relacao a base) num segundo arquivo.
 * Metodos (cada um com a referencia no docs/specialization.md):
 *   --ema D              media movel dos pesos (Polyak), gravada se a validacao dela for melhor
 *   --neft A             ruido uniforme nos embeddings de entrada, escala A / sqrt(L d)  (NEFTune, Jain et al. 2023)
 *   --adapter-dropout P  dropout na entrada dos adaptadores (LoRA dropout), mascara deterministica pela semente
 *   --dora               decomposicao magnitude/direcao (Liu et al. 2024), norma fora do gradiente como no artigo
 *   --anchor L           KL(base || modelo) nas posicoes de contexto (fora dos alvos), peso L
 *   --label-smoothing E  suavizacao de rotulos
 *   --tool-weight W      peso W para os tokens dentro de chamadas de ferramenta (nome e argumentos)
 *   --optimizer muon     Muon (Jordan et al. 2024) nas matrizes dos adaptadores, escala de Liu et al. 2025
 *   --pissa              A e B iniciados nas componentes principais de W (Meng et al. 2024), custo medido
 */

typedef struct {
    int allow_worse;
    double ema;                 /* decaimento da media movel (0 = desligada) */
    float neft, drop, anchor, smooth, tool_w;
    int dora, muon, pissa;
    double spike; int max_skips;
    uint64_t key;               /* chave da janela atual: ruido e mascaras sao funcao dela (deterministico) */
    int force_w;                /* gradcheck: pesos por posicao pelo padrao fixo, sem olhar o texto */
} TmCfg;
static TmCfg tm = { 0, 0, 0, 0, 0, 0, 1.0f, 0, 0, 0, 5.0, 5, 0, 0 };

struct TmPar {
    float *mag, *gmag, *mm1, *mm2;          /* DoRA: magnitude por linha, gradiente e momentos */
    float *d, *nrm, *nfz, *wn2; int frozen;  /* fator m/norma, norma, norma congelada (gradcheck), |w_i|^2 da base */
    float *ew, *ea, *eb, *emag;             /* media movel (sombra) */
    int rt;                                 /* PiSSA: posto treinavel; as componentes de rt ate r sao a copia congelada */
    float *fa, *fb;                         /* PiSSA: copia da parte congelada (restaurada depois do otimizador) */
};
static struct TmPar *tm_get(TPar *p) { if (!p->tm) p->tm = xalloc(1, sizeof(struct TmPar)); return p->tm; }
static int tm_has_dora(const TPar *p) { return p->tm && p->tm->mag; }

static uint64_t tm_mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull; x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
static float tm_unif(uint64_t k) { return (float)((tm_mix(k) >> 40) * (1.0 / 16777216.0)); }       /* [0, 1) */
static int tm_finite(double x) { union { double d; uint64_t u; } v; v.d = x; return ((v.u >> 52) & 0x7ff) != 0x7ff; }   /* a prova de -ffast-math */
static int tm_finitef(float x) { union { float f; uint32_t u; } v; v.f = x; return ((v.u >> 23) & 0xff) != 0xff; }
static void tm_set_key(unsigned seed, long step, int bi) { tm.key = tm_mix((uint64_t)seed * 0x100000001b3ull + (uint64_t)step * 1315423911u + (uint64_t)bi + 1); }

/* ---- ruido nos embeddings (NEFTune): x += alfa / sqrt(L d) * U(-1, 1) ---- */
static void tm_neft_add(float *x, int T, int d) {
    if (!tm_on || tm.neft <= 0 || tm_off) return;
    const float a = tm.neft / sqrtf((float)T * (float)d);
    for (int t = 0; t < T; t++) for (int i = 0; i < d; i++)
        x[(size_t)t * d + i] += a * (2.0f * tm_unif(tm.key * 0x2545f4914f6cdd1dull + ((uint64_t)t << 24) + (uint64_t)i + 77) - 1.0f);
}

/* ---- dropout do adaptador: mascara sobre a entrada x do produto A x, mesma na ida e na volta ---- */
static int tm_drop_on(const TPar *p) { return tm_on && !tm_off && tm.drop > 0 && p->r; }
static float tm_drop_m(const TPar *p, int b, int c) {
    return tm_unif(tm.key ^ ((uint64_t)(p->tmid + 1) << 48) ^ ((uint64_t)b << 28) ^ (uint64_t)c) < tm.drop ? 0.0f : 1.0f / (1.0f - tm.drop);
}
static float *tm_drop_x(const TPar *p, const float *x, int B) {
    if (!tm_drop_on(p)) return NULL;
    const size_t C = p->cols;
    float *o = xalloc((size_t)B * C, 4);
    for (int b = 0; b < B; b++) for (size_t c = 0; c < C; c++) o[(size_t)b * C + c] = x[(size_t)b * C + c] * tm_drop_m(p, b, (int)c);
    return o;
}
static void tm_drop_row(const TPar *p, int b, float *row, int C) {
    if (!tm_drop_on(p)) return;
    for (int c = 0; c < C; c++) row[c] *= tm_drop_m(p, b, c);
}

/* ---- DoRA: y_i = (m_i / n_i) * (W x + s B A x)_i, n_i = |w_i + s B_i A| (norma da linha; fora do gradiente) ---- */
typedef struct { const TPar *p; float *nrm; const float *G; } TmNJob;
static void tm_nrm_job(void *ctx, int r0, int r1) {
    const TmNJob *j = (const TmNJob *)ctx;
    const TPar *p = j->p; const struct TmPar *q = p->tm;
    const int r = p->r; const size_t C = p->cols;
    float *tmp = xalloc(C, 4), *ck = xalloc((size_t)r, 4);
    for (int i = r0; i < r1; i++) {
        const float *w = tp_row(p, (size_t)i, tmp);
        for (int k = 0; k < r; k++) ck[k] = dot(w, p->a + (size_t)k * C, (int)C);
        const float *bi = p->b + (size_t)i * r;
        double n2 = q->wn2[i], c1 = 0, c2 = 0;
        for (int k = 0; k < r; k++) { c1 += (double)bi[k] * ck[k]; for (int l = 0; l < r; l++) c2 += (double)bi[k] * bi[l] * j->G[k * r + l]; }
        n2 += 2.0 * p->s * c1 + (double)p->s * p->s * c2;
        j->nrm[i] = (float)sqrt(n2 > 1e-12 ? n2 : 1e-12);
    }
    free(tmp); free(ck);
}
static void tm_dora_prep(const TPar *p) {
    struct TmPar *q = p->tm;
    if (!q->frozen) {
        const int r = p->r;
        float *G = xalloc((size_t)r * r, 4);
        for (int k = 0; k < r; k++) for (int l = 0; l < r; l++) G[k * r + l] = dot(p->a + (size_t)k * p->cols, p->a + (size_t)l * p->cols, (int)p->cols);
        TmNJob j = { p, q->nrm, G };
        pool_for(tm_nrm_job, &j, (int)p->rows, 8);
        free(G);
    } else memcpy(q->nrm, q->nfz, p->rows * 4);
    for (size_t i = 0; i < p->rows; i++) q->d[i] = q->mag[i] / q->nrm[i];
}
static void tm_dora_scale(const TPar *p, float *out, int B) {
    const float *d = p->tm->d;
    for (int b = 0; b < B; b++) for (size_t i = 0; i < p->rows; i++) out[(size_t)b * p->rows + i] *= d[i];
}
static float *tm_dora_dy(const TPar *p, const float *dy, int B) {
    if (!tm_has_dora(p)) return NULL;
    float *o = xalloc((size_t)B * p->rows, 4);
    const float *d = p->tm->d;
    for (int b = 0; b < B; b++) for (size_t i = 0; i < p->rows; i++) o[(size_t)b * p->rows + i] = dy[(size_t)b * p->rows + i] * d[i];
    return o;
}
static float *tm_dora_back(TPar *p, const float *dy, const float *x, int B) {
    if (!tm_has_dora(p)) return NULL;
    struct TmPar *q = p->tm;
    const size_t R = p->rows;
    float *u = xalloc((size_t)B * R, 4);
    tm_nodora = 1; tp_mv(p, u, x, B); tm_nodora = 0;          /* V x sem a magnitude */
    for (int b = 0; b < B; b++) for (size_t i = 0; i < R; i++) q->gmag[i] += dy[(size_t)b * R + i] * u[(size_t)b * R + i] / q->nrm[i];
    free(u);
    return tm_dora_dy(p, dy, B);
}
/* liga a magnitude num adaptador: m_i = |w_i| da base (com B zero o resultado continua igual a base) */
static void tm_dora_enable(TPar *p) {
    if (!p->r || !p->mat || tm_has_dora(p)) return;
    struct TmPar *q = tm_get(p);
    const size_t R = p->rows;
    q->mag = xalloc(R, 4); q->gmag = xalloc(R, 4); q->mm1 = xalloc(R, 4); q->mm2 = xalloc(R, 4);
    q->d = xalloc(R, 4); q->nrm = xalloc(R, 4); q->nfz = xalloc(R, 4); q->wn2 = xalloc(R, 4);
    float *tmp = xalloc(p->cols, 4);
    const int off = tm_off; tm_off = 1;
    for (size_t i = 0; i < R; i++) { const float *w = tp_row(p, i, tmp); q->wn2[i] = dot(w, w, (int)p->cols); }
    tm_off = off;
    free(tmp);
    tm_dora_prep(p);                                           /* norma com A e B atuais */
    for (size_t i = 0; i < R; i++) q->mag[i] = q->nrm[i];     /* d = 1: a saida e a do LoRA sem magnitude */
}
static void tm_dora_freeze(TTab *tt, int on) {
    for (int i = 0; i < tt->n; i++) {
        TPar *p = &tt->p[i];
        if (!tm_has_dora(p)) continue;
        p->tm->frozen = 0; tm_dora_prep(p);
        memcpy(p->tm->nfz, p->tm->nrm, p->rows * 4);
        p->tm->frozen = on;
    }
}

/* ---- perda moldada: pesos por token, suavizacao, ancora KL (so no treino) ---- */
static double tm_kl_row(const float *zb, const float *zs, int V);
static int tm_shape(void) { return tm.smooth > 0 || tm.tool_w != 1.0f || tm.anchor > 0 || tm.force_w; }
static int tm_anchor_on(void) { return tm.anchor > 0; }

/* t_tool_flags: fl[j] = 1 se o token j esta dentro de uma chamada de ferramenta (<tool_call>...</tool_call> ou o par do Gemma 4) */
static void tm_tool_flags(const Model *m, const int *toks, int T, uint8_t *fl) {
    memset(fl, 0, (size_t)T);
    size_t cap = (size_t)T * 12 + 64, n = 0;
    char *tx = xalloc(cap, 1), tmp[320];
    size_t *st = xalloc((size_t)T + 1, sizeof(size_t));
    for (int j = 0; j < T; j++) {
        st[j] = n;
        const size_t l = tok_bytes(&m->tk, toks[j], tmp, sizeof tmp);
        if (n + l + 1 >= cap) { cap = (cap + l) * 2; tx = realloc(tx, cap); }
        memcpy(tx + n, tmp, l); n += l;
    }
    st[T] = n; tx[n] = 0;
    uint8_t *bf = xalloc(n + 1, 1);
    static const char *pr[2][2] = { { "<tool_call>", "</tool_call>" }, { "<|tool_call>", "<tool_call|>" } };
    for (int k = 0; k < 2; k++) {
        const char *s = tx;
        while ((s = strstr(s, pr[k][0]))) {
            const char *e = strstr(s + strlen(pr[k][0]), pr[k][1]);
            const size_t a = (size_t)(s - tx), b = e ? (size_t)(e - tx) + strlen(pr[k][1]) : n;
            memset(bf + a, 1, b - a);
            s = e ? e : tx + n;
            if (!e) break;
        }
    }
    for (int j = 0; j < T; j++) for (size_t q = st[j]; q < st[j + 1]; q++) if (bf[q]) { fl[j] = 1; break; }
    free(tx); free(st); free(bf);
}

/* soma da perda comum (sem peso nem suavizacao) nos alvos e, em dl, o gradiente da perda moldada nos logits.
   Perda moldada = sum_t w_t [(1-e)(-log p_y) + e (-media log p)] / sum w  +  L * media_contexto KL(base || modelo).
   Com tm.ret_shaped devolve o valor dela vezes N (o gradcheck diferencia a perda moldada, nao a comum) */
static int tm_ret_shaped;
static double tm_loss(const Model *m, const int *toks, const uint8_t *mask, const float *lg, const float *blg, float *dl, int T, int N) {
    const int V = m->cfg.vocab;
    const float cap = m->cfg.softcap;
    float *w = xalloc((size_t)T, 4);
    for (int t = 0; t < T; t++) w[t] = 1.0f;
    if (tm.force_w) { for (int t = 0; t + 1 < T; t++) w[t] = (t % 4 == 0) ? tm.tool_w : 1.0f; }
    else if (tm.tool_w != 1.0f) {
        uint8_t *fl = xalloc((size_t)T, 1);
        tm_tool_flags(m, toks, T, fl);
        for (int t = 0; t + 1 < T; t++) if (fl[t + 1]) w[t] = tm.tool_w;
        free(fl);
    }
    double ws = 0; int nc = 0;
    for (int t = 0; t + 1 < T; t++) { if (mask[t]) ws += w[t]; else nc++; }
    if (ws <= 0) ws = N;
    const double eps = tm.smooth;
    double *pm = xalloc((size_t)V, sizeof(double));
    double sum = 0, shaped = 0;
    for (int t = 0; t + 1 < T; t++) {
        const float *z = lg + (size_t)t * V;
        float *g = dl ? dl + (size_t)t * V : NULL;
        if (mask[t]) {
            float mx = z[0]; for (int i = 1; i < V; i++) if (z[i] > mx) mx = z[i];
            double se = 0, sz = 0; for (int i = 0; i < V; i++) { pm[i] = exp((double)z[i] - mx); se += pm[i]; sz += z[i]; }
            const double lse = log(se) + mx;
            const int y = toks[t + 1];
            sum += lse - z[y];
            shaped += w[t] / ws * ((1.0 - eps) * (lse - z[y]) + eps * (lse - sz / V));
            if (g) {
                const double sc = (double)t_gscale * w[t] / ws;
                for (int i = 0; i < V; i++) g[i] = (float)(sc * (pm[i] / se - eps / V));
                g[y] -= (float)(sc * (1.0 - eps));
            }
        } else if (blg && tm.anchor > 0 && nc > 0) {          /* contexto: KL(base || modelo), gradiente p_m - p_b */
            const float *zb = blg + (size_t)t * V;
            shaped += tm.anchor / nc * tm_kl_row(zb, z, V);
            if (g) {
                float mx = z[0], mb = zb[0];
                for (int i = 1; i < V; i++) { if (z[i] > mx) mx = z[i]; if (zb[i] > mb) mb = zb[i]; }
                double se = 0, sb = 0;
                for (int i = 0; i < V; i++) { se += exp((double)z[i] - mx); sb += exp((double)zb[i] - mb); }
                double sc = (double)t_gscale * tm.anchor / nc;
                for (int i = 0; i < V; i++) g[i] = (float)(sc * (exp((double)z[i] - mx) / se - exp((double)zb[i] - mb) / sb));
            }
        } else continue;
        if (g && cap > 0.0f) for (int i = 0; i < V; i++) { const float th = z[i] / cap; g[i] *= 1.0f - th * th; }
    }
    free(pm); free(w);
    return tm_ret_shaped ? shaped * N : sum;
}

/* logits do modelo base (tudo desligado) para a ancora; preserva a escolha do MoE guardada em A */
static float *tm_base_lg(Model *m, TTab *tt, const int *toks, void *Av) {
    TAct *A = (TAct *)Av;
    const int T = A->T, V = m->cfg.vocab;
    int *sel = NULL; const int fx = A->fixsel;
    if (A->sel) { sel = xalloc((size_t)A->Tmax * A->K * m->cfg.n_layers + 1, sizeof(int)); memcpy(sel, A->sel, (size_t)T * A->K * m->cfg.n_layers * sizeof(int)); }
    A->fixsel = 0; tm_off = 1;
    const int on = tm_on; tm_on = 0;
    t_forward(m, tt, toks, A);
    float *lg = xalloc((size_t)T * V, 4);
    t_logits(m, tt, A, lg);
    tm_on = on; tm_off = 0; A->fixsel = fx;
    if (sel) { memcpy(A->sel, sel, (size_t)T * A->K * m->cfg.n_layers * sizeof(int)); free(sel); }
    return lg;
}

/* ---- avaliacao: perda por fonte (e nos tokens de chamada de ferramenta, se pedido) ---- */
typedef struct { double all; long alln; double src[16]; long srcn[16]; double tool; long tooln; } TmEv;
static TmEv tm_ev, tm_ev_best, tm_ev_base;
static void tm_eval(Model *m, TTab *tt, TAct *A, const TSet *S, int seq, int tools, TmEv *ev) {
    memset(ev, 0, sizeof *ev);
    const int on = tm_on; tm_on = 0;
    for (int i = 0; i < S->n; i++) {
        const TEx *w = &S->w[i];
        A->T = w->ids.n;
        int N = 0;
        const double l = t_step(m, tt, w->ids.ids, w->mask, A, 0, &N);
        if (!N) continue;
        ev->all += l * N; ev->alln += N;
        ev->src[w->src & 15] += l * N; ev->srcn[w->src & 15] += N;
        if (tools) {
            uint8_t *fl = xalloc((size_t)w->ids.n, 1), *m2 = xalloc((size_t)w->ids.n, 1);
            tm_tool_flags(m, w->ids.ids, w->ids.n, fl);
            int any = 0;
            for (int t = 0; t + 1 < w->ids.n; t++) { m2[t] = w->mask[t] && fl[t + 1]; any |= m2[t]; }
            if (any) { int n2 = 0; const double l2 = t_step(m, tt, w->ids.ids, m2, A, 0, &n2); ev->tool += l2 * n2; ev->tooln += n2; }
            free(fl); free(m2);
        }
    }
    A->T = seq;
    tm_on = on;
}
static void tm_print_src(const char *tag, const TmEv *ev) {
    printf("%s", tag);
    for (int s = 0; s < g_ntsrc && s < 16; s++) if (ev->srcn[s]) printf("  %s %.4f (%ld)", g_tsrc[s], ev->src[s] / ev->srcn[s], ev->srcn[s]);
    printf("\n");
}

/* ---- media movel dos pesos ---- */
static void tm_arrays(TPar *p, float **v, size_t *n, float **sh, int *k) {      /* matrizes treinaveis do parametro e sombras */
    *k = 0;
    struct TmPar *q = p->tm;
    if (p->w) { v[*k] = p->w; n[*k] = p->rows * p->cols; sh[*k] = q ? q->ew : NULL; (*k)++; }
    if (p->r) { v[*k] = p->a; n[*k] = (size_t)p->r * p->cols; sh[*k] = q ? q->ea : NULL; (*k)++;
                v[*k] = p->b; n[*k] = p->rows * (size_t)p->r; sh[*k] = q ? q->eb : NULL; (*k)++; }
    if (q && q->mag) { v[*k] = q->mag; n[*k] = p->rows; sh[*k] = q->emag; (*k)++; }
}
static void tm_ema_init(TTab *tt) {
    for (int i = 0; i < tt->n; i++) {
        TPar *p = &tt->p[i];
        if (!p->w && !p->r) continue;
        struct TmPar *q = tm_get(p);
        if (p->w) q->ew = xalloc(p->rows * p->cols, 4);
        if (p->r) { q->ea = xalloc((size_t)p->r * p->cols, 4); q->eb = xalloc(p->rows * (size_t)p->r, 4); }
        if (q->mag) q->emag = xalloc(p->rows, 4);
        float *v[4], *sh[4]; size_t n[4]; int k;
        tm_arrays(p, v, n, sh, &k);
        for (int a = 0; a < k; a++) memcpy(sh[a], v[a], n[a] * 4);
    }
}
static void tm_ema_update(TTab *tt, long step) {
    double d = tm.ema, ramp = (1.0 + step) / (10.0 + step);          /* aquecimento do decaimento (como na media movel de pesos de uso comum) */
    if (ramp < d) d = ramp;
    for (int i = 0; i < tt->n; i++) {
        TPar *p = &tt->p[i];
        if (!p->tm || (!p->tm->ea && !p->tm->ew)) continue;
        float *v[4], *sh[4]; size_t n[4]; int k;
        tm_arrays(p, v, n, sh, &k);
        for (int a = 0; a < k; a++) for (size_t j = 0; j < n[a]; j++) sh[a][j] = (float)(d * sh[a][j] + (1.0 - d) * v[a][j]);
    }
}
static void tm_ema_swap(TTab *tt) {
    for (int i = 0; i < tt->n; i++) {
        TPar *p = &tt->p[i];
        if (!p->tm || (!p->tm->ea && !p->tm->ew)) continue;
        float *v[4], *sh[4]; size_t n[4]; int k;
        tm_arrays(p, v, n, sh, &k);
        for (int a = 0; a < k; a++) for (size_t j = 0; j < n[a]; j++) { const float t = v[a][j]; v[a][j] = sh[a][j]; sh[a][j] = t; }
    }
    tg.ver++;
}
/* avaliacao num ponto de validacao: o estado atual e, com --ema, a media movel; deixa em tt o estado que valeu (o
   chamador grava e chama tm_eval_done para devolver o outro) */
static double tm_eval_point(Model *m, TTab *tt, TAct *A, const TSet *S, int seq, int *nt, int *used_ema) {
    *used_ema = 0;
    tm_eval(m, tt, A, S, seq, 0, &tm_ev);
    double best = tm_ev.alln ? tm_ev.all / tm_ev.alln : 0.0;
    if (nt) *nt = (int)tm_ev.alln;
    if (tm.ema > 0) {
        tm_ema_swap(tt);
        TmEv e2; tm_eval(m, tt, A, S, seq, 0, &e2);
        const double l2 = e2.alln ? e2.all / e2.alln : 0.0;
        printf("validacao com a media movel: perda %.4f (estado atual %.4f)\n", l2, best);
        if (l2 < best) { best = l2; tm_ev = e2; *used_ema = 1; }
        else tm_ema_swap(tt);
    }
    return best;
}
static void tm_eval_done(TTab *tt, int used_ema) { if (used_ema) tm_ema_swap(tt); }

/* ---- PiSSA: componentes principais de W (subespacos aleatorios com iteracao de potencia; Halko et al. 2011) ---- */
static void tm_orth(float *Q, int k, int n) {      /* Gram-Schmidt nas k linhas de Q (cada uma com n floats), duas passadas */
    for (int j = 0; j < k; j++) {
        float *qj = Q + (size_t)j * n;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < j; i++) { const float c = dot(qj, Q + (size_t)i * n, n); const float *qi = Q + (size_t)i * n; for (int c2 = 0; c2 < n; c2++) qj[c2] -= c * qi[c2]; }
        const float nr = sqrtf(dot(qj, qj, n));
        const float iv = nr > 1e-20f ? 1.0f / nr : 0.0f;
        for (int c2 = 0; c2 < n; c2++) qj[c2] *= iv;
    }
}
typedef struct { const float *W; float *Y; const float *X; int C, k; } TmMJob;
static void tm_wx_job(void *ctx, int r0, int r1) {       /* Y[i][j] = W_i . X_j  (X: k linhas de C) */
    const TmMJob *j = (const TmMJob *)ctx;
    for (int i = r0; i < r1; i++) for (int q = 0; q < j->k; q++) j->Y[(size_t)i * j->k + q] = dot(j->W + (size_t)i * j->C, j->X + (size_t)q * j->C, j->C);
}
/* Qt (k x R, linhas ortonormais) -> Bm = Q^T W (k x C) */
static void tm_qtw(const float *W, int R, int C, const float *Qt, int k, float *Bm) {
    memset(Bm, 0, (size_t)k * C * 4);
    for (int i = 0; i < R; i++) { const float *wi = W + (size_t)i * C; for (int q = 0; q < k; q++) { const float a = Qt[(size_t)q * R + i]; float *b = Bm + (size_t)q * C; for (int c = 0; c < C; c++) b[c] += a * wi[c]; } }
}
static void tm_topsvd(const float *W, int R, int C, int k, float *U /* k x R */, float *S, float *Vt /* k x C */) {
    int ko = k + 4; if (ko > R) ko = R; if (ko > C) ko = C;
    if (k > ko) k = ko;
    float *X = xalloc((size_t)ko * C, 4), *Y = xalloc((size_t)R * ko, 4), *Qt = xalloc((size_t)ko * R, 4), *Bm = xalloc((size_t)ko * C, 4);
    uint64_t sd = 12345;
    for (size_t i = 0; i < (size_t)ko * C; i++) X[i] = tm_unif(sd++) - 0.5f;
    for (int it = 0; it < 4; it++) {                         /* potencia: Q <- orth(W X^T); X <- orth(W^T Q) */
        TmMJob j = { W, Y, X, C, ko };
        pool_for(tm_wx_job, &j, R, 16);
        for (int i = 0; i < R; i++) for (int q = 0; q < ko; q++) Qt[(size_t)q * R + i] = Y[(size_t)i * ko + q];
        tm_orth(Qt, ko, R);
        if (it == 3) break;
        tm_qtw(W, R, C, Qt, ko, X);
        tm_orth(X, ko, C);
    }
    tm_qtw(W, R, C, Qt, ko, Bm);
    double *G = xalloc((size_t)ko * ko, 8), *Ev = xalloc((size_t)ko * ko, 8);
    for (int a = 0; a < ko; a++) for (int b = 0; b < ko; b++) G[a * ko + b] = dot(Bm + (size_t)a * C, Bm + (size_t)b * C, C);
    for (int a = 0; a < ko; a++) Ev[a * ko + a] = 1;
    for (int sweep = 0; sweep < 60; sweep++) {              /* Jacobi: autovalores de G */
        double off = 0; for (int a = 0; a < ko; a++) for (int b = a + 1; b < ko; b++) off += G[a * ko + b] * G[a * ko + b];
        if (off < 1e-24) break;
        for (int a = 0; a < ko; a++) for (int b = a + 1; b < ko; b++) {
            const double g = G[a * ko + b];
            if (fabs(g) < 1e-300) continue;
            const double th = (G[b * ko + b] - G[a * ko + a]) / (2 * g);
            const double t = (th >= 0 ? 1 : -1) / (fabs(th) + sqrt(th * th + 1)), c = 1 / sqrt(t * t + 1), s = t * c;
            for (int i = 0; i < ko; i++) { const double x = G[i * ko + a], y = G[i * ko + b]; G[i * ko + a] = c * x - s * y; G[i * ko + b] = s * x + c * y; }
            for (int i = 0; i < ko; i++) { const double x = G[a * ko + i], y = G[b * ko + i]; G[a * ko + i] = c * x - s * y; G[b * ko + i] = s * x + c * y; }
            for (int i = 0; i < ko; i++) { const double x = Ev[i * ko + a], y = Ev[i * ko + b]; Ev[i * ko + a] = c * x - s * y; Ev[i * ko + b] = s * x + c * y; }
        }
    }
    int *ord = xalloc((size_t)ko, sizeof(int));
    for (int i = 0; i < ko; i++) ord[i] = i;
    for (int i = 0; i < ko; i++) for (int j = i + 1; j < ko; j++) if (G[ord[j] * ko + ord[j]] > G[ord[i] * ko + ord[i]]) { const int t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
    for (int q = 0; q < k; q++) {
        const int e = ord[q];
        const double lam = G[e * ko + e] > 0 ? G[e * ko + e] : 0, sg = sqrt(lam);
        S[q] = (float)sg;
        float *u = U + (size_t)q * R, *v = Vt + (size_t)q * C;
        memset(u, 0, (size_t)R * 4); memset(v, 0, (size_t)C * 4);
        for (int a = 0; a < ko; a++) {
            const float ca = (float)Ev[a * ko + e];
            const float *qa = Qt + (size_t)a * R, *ba = Bm + (size_t)a * C;
            for (int i = 0; i < R; i++) u[i] += ca * qa[i];
            if (sg > 1e-20) for (int c = 0; c < C; c++) v[c] += ca * ba[c];
        }
        if (sg > 1e-20) for (int c = 0; c < C; c++) v[c] /= (float)sg;
    }
    free(X); free(Y); free(Qt); free(Bm); free(G); free(Ev); free(ord);
}
/* A e B passam a ser as componentes principais (s B A = U S V^T) e o que sai de W fica como parte congelada do
   adaptador, de posto 2r: [A; A0], [B | -B0]. O arquivo grava o posto 2r e funciona em leitores antigos. */
static double tm_pissa_init(TTab *tt, long *nmat) {
    const double t0 = now_sec();
    *nmat = 0;
    for (int i = 0; i < tt->n; i++) {
        TPar *p = &tt->p[i];
        if (!p->r || !p->mat || i == tt->tok) continue;
        const int r = p->r, R = (int)p->rows, C = (int)p->cols;
        if (r * 2 >= R || r * 2 >= C) continue;
        float *W = xalloc((size_t)R * C, 4);
        const int off = tm_off; tm_off = 1;
        for (int q = 0; q < R; q++) mat_row(W + (size_t)q * C, p->mat, q, C);
        tm_off = off;
        float *U = xalloc((size_t)r * R, 4), *S = xalloc((size_t)r, 4), *Vt = xalloc((size_t)r * C, 4);
        tm_topsvd(W, R, C, r, U, S, Vt);
        free(W);
        const int r2 = 2 * r;
        float *a = xalloc((size_t)r2 * C, 4), *b = xalloc((size_t)R * r2, 4);
        for (int q = 0; q < r; q++) {
            const float f = sqrtf(S[q] / p->s);
            for (int c = 0; c < C; c++) { a[(size_t)q * C + c] = f * Vt[(size_t)q * C + c]; a[(size_t)(r + q) * C + c] = f * Vt[(size_t)q * C + c]; }
            for (int row = 0; row < R; row++) { b[(size_t)row * r2 + q] = f * U[(size_t)q * R + row]; b[(size_t)row * r2 + r + q] = -f * U[(size_t)q * R + row]; }
        }
        free(U); free(S); free(Vt);
        free(p->a); free(p->b); free(p->ga); free(p->gb); free(p->ma1); free(p->ma2); free(p->mb1); free(p->mb2);
        p->r = r2; p->a = a; p->b = b;
        p->ga = xalloc((size_t)r2 * C, 4); p->gb = xalloc((size_t)R * r2, 4);
        p->ma1 = xalloc((size_t)r2 * C, 4); p->ma2 = xalloc((size_t)r2 * C, 4); p->mb1 = xalloc((size_t)R * r2, 4); p->mb2 = xalloc((size_t)R * r2, 4);
        struct TmPar *q = tm_get(p);
        q->rt = r;
        q->fa = xalloc((size_t)r * C, 4); q->fb = xalloc((size_t)R * r, 4);
        memcpy(q->fa, a + (size_t)r * C, (size_t)r * C * 4);
        for (int row = 0; row < R; row++) memcpy(q->fb + (size_t)row * r, b + (size_t)row * r2 + r, (size_t)r * 4);
        (*nmat)++;
    }
    return now_sec() - t0;
}
/* antes do otimizador: gradiente zero na parte congelada */
static void tm_pre_opt(TTab *tt) {
    for (int i = 0; i < tt->n; i++) {
        TPar *p = &tt->p[i];
        if (!p->tm || !p->tm->fa) continue;
        const int r = p->r, rt = p->tm->rt;
        memset(p->ga + (size_t)rt * p->cols, 0, (size_t)(r - rt) * p->cols * 4);
        for (size_t row = 0; row < p->rows; row++) memset(p->gb + row * r + rt, 0, (size_t)(r - rt) * 4);
    }
}
static double tm_gradsq(const TPar *p) {
    double s = 0;
    if (p->tm && p->tm->mag) for (size_t i = 0; i < p->rows; i++) s += (double)p->tm->gmag[i] * p->tm->gmag[i];
    return s;
}

/* ---- Muon: momento + Newton-Schulz (quintico) para ortogonalizar a atualizacao de uma matriz ---- */
static void tm_ns(float *U, int n, int m) {         /* U: n x m, no lugar */
    const int tr = n > m;
    const int a = tr ? m : n, b = tr ? n : m;           /* a <= b */
    float *X = xalloc((size_t)a * b, 4), *Aa = xalloc((size_t)a * a, 4), *Bb = xalloc((size_t)a * a, 4), *Y = xalloc((size_t)a * b, 4);
    for (int i = 0; i < n; i++) for (int j = 0; j < m; j++) { if (tr) X[(size_t)j * b + i] = U[(size_t)i * m + j]; else X[(size_t)i * b + j] = U[(size_t)i * m + j]; }
    double fn = 0; for (size_t i = 0; i < (size_t)a * b; i++) fn += (double)X[i] * X[i];
    const float inv = 1.0f / ((float)sqrt(fn) + 1e-7f);
    for (size_t i = 0; i < (size_t)a * b; i++) X[i] *= inv;
    for (int it = 0; it < 5; it++) {
        for (int i = 0; i < a; i++) for (int j = 0; j < a; j++) Aa[i * a + j] = dot(X + (size_t)i * b, X + (size_t)j * b, b);
        for (int i = 0; i < a; i++) for (int j = 0; j < a; j++) { float s = 0; for (int k = 0; k < a; k++) s += Aa[i * a + k] * Aa[k * a + j]; Bb[i * a + j] = -4.7750f * Aa[i * a + j] + 2.0315f * s; }
        for (int i = 0; i < a; i++) { float *y = Y + (size_t)i * b; for (int c = 0; c < b; c++) y[c] = 3.4445f * X[(size_t)i * b + c];
            for (int k = 0; k < a; k++) { const float f = Bb[i * a + k]; const float *xk = X + (size_t)k * b; for (int c = 0; c < b; c++) y[c] += f * xk[c]; } }
        memcpy(X, Y, (size_t)a * b * 4);
    }
    for (int i = 0; i < n; i++) for (int j = 0; j < m; j++) U[(size_t)i * m + j] = tr ? X[(size_t)j * b + i] : X[(size_t)i * b + j];
    free(X); free(Aa); free(Bb); free(Y);
}
static void tm_muon_upd(float *w, const float *gr, float *mom, int n, int m, float sc, float lr, float wd) {
    const size_t N = (size_t)n * m;
    float *u = xalloc(N, 4);
    const float mu = 0.95f;
    for (size_t j = 0; j < N; j++) { const float g = gr[j] * sc; mom[j] = mu * mom[j] + g; u[j] = g + mu * mom[j]; }      /* Nesterov */
    tm_ns(u, n, m);
    const float scl = 0.2f * sqrtf((float)(n > m ? n : m));        /* mesma RMS que o AdamW (Liu et al. 2025) */
    for (size_t j = 0; j < N; j++) w[j] -= lr * (scl * u[j] + wd * w[j]);
    free(u);
}
/* atualizacao das matrizes de um adaptador (e da magnitude do DoRA); devolve 1 se tratou */
static void tm_adapter_update(TPar *p, float sc, float lr, float wd, float c1, float c2) {
    const int r = p->r;
    if (tm.muon) {
        tm_muon_upd(p->a, p->ga, p->ma1, r, (int)p->cols, sc, lr, wd);
        tm_muon_upd(p->b, p->gb, p->mb1, (int)p->rows, r, sc, lr, wd);
    } else {
        adam_upd(p->a, p->ga, p->ma1, p->ma2, (size_t)r * p->cols, sc, lr, wd, c1, c2);
        adam_upd(p->b, p->gb, p->mb1, p->mb2, p->rows * (size_t)r, sc, lr, wd, c1, c2);
    }
    struct TmPar *q = p->tm;
    if (q && q->mag) adam_upd(q->mag, q->gmag, q->mm1, q->mm2, p->rows, sc, lr, 0.0f, c1, c2);
    if (q && q->fa) {                                   /* PiSSA: a parte congelada volta ao que era (o decaimento de pesos a mexeria) */
        const int rt = q->rt;
        memcpy(p->a + (size_t)rt * p->cols, q->fa, (size_t)(r - rt) * p->cols * 4);
        for (size_t row = 0; row < p->rows; row++) memcpy(p->b + row * r + rt, q->fb + row * (r - rt), (size_t)(r - rt) * 4);
    }
}
static void tm_clear(TPar *p) { if (p->tm && p->tm->gmag) memset(p->tm->gmag, 0, p->rows * 4); }

/* ---- DoRA no .kspec: secao "KSDM" depois dos tensores e antes da marca final; 8 bytes antes da marca guardam onde
   ela comeca. Leitores antigos param nos tensores e so veem a marca final (que traz a flag 2). ---- */
#define KSPEC_DORA 2u
static uint32_t tm_save_dora(FILE *f, const TTab *tt) {
    int n = 0;
    for (int i = 0; i < tt->n; i++) n += tm_has_dora(&tt->p[i]);
    if (!n) return 0;
    const uint64_t off = (uint64_t)ftell(f);
    fwrite("KSDM", 1, 4, f); fwrite(&n, 4, 1, f);
    for (int i = 0; i < tt->n; i++) {
        const TPar *p = &tt->p[i];
        if (!tm_has_dora(p)) continue;
        const uint32_t nl = (uint32_t)strlen(p->name); const uint64_t r = p->rows;
        fwrite(&nl, 4, 1, f); fwrite(p->name, 1, nl, f); fwrite(&r, 8, 1, f); fwrite(p->tm->mag, 4, p->rows, f);
    }
    fwrite(&off, 8, 1, f);
    return KSPEC_DORA;
}

static TmDsec tm_dora_read(const char *path) {
    TmDsec s = {0};
    if (!(spec_flags(path) & KSPEC_DORA)) return s;
    FILE *f = fopen(path, "rb");
    if (!f) return s;
    uint64_t off = 0; char mg[4]; int n = 0;
    if (fseek(f, -16, SEEK_END) || fread(&off, 8, 1, f) != 1 || fseek(f, (long)off, SEEK_SET) || fread(mg, 1, 4, f) != 4 || memcmp(mg, "KSDM", 4) || fread(&n, 4, 1, f) != 1 || n < 0 || n > 100000) { fclose(f); return s; }
    s.name = xalloc((size_t)n, 128); s.rows = xalloc((size_t)n, 8); s.mag = xalloc((size_t)n, sizeof(float *));
    for (int i = 0; i < n; i++) {
        uint32_t nl = 0; uint64_t r = 0;
        if (fread(&nl, 4, 1, f) != 1 || nl == 0 || nl >= 128 || fread(s.name[i], 1, nl, f) != nl || fread(&r, 8, 1, f) != 1 || r > (1u << 28)) break;
        s.name[i][nl] = 0; s.rows[i] = r; s.mag[i] = xalloc(r, 4);
        if (fread(s.mag[i], 4, r, f) != r) break;
        s.n = i + 1;
    }
    fclose(f);
    return s;
}
static void tm_dsec_free(TmDsec *s) { for (int i = 0; i < s->n; i++) free(s->mag[i]); free(s->name); free(s->rows); free(s->mag); memset(s, 0, sizeof *s); }
static const float *tm_dsec_find(const TmDsec *s, const char *name, uint64_t rows) {
    for (int i = 0; i < s->n; i++) if (!strcmp(s->name[i], name) && s->rows[i] == rows) return s->mag[i];
    return NULL;
}
/* na carga (KATANA_SPEC): W (ja com s B A) com as linhas escaladas por m_i / |w_i| */
static void tm_dora_apply(const TmDsec *s, const char *name, float *w, uint64_t r, uint64_t cc) {
    const float *mg = tm_dsec_find(s, name, r);
    if (!mg) return;
    for (uint64_t i = 0; i < r; i++) {
        double n2 = 0; for (uint64_t c = 0; c < cc; c++) n2 += (double)w[i * cc + c] * w[i * cc + c];
        const float f = n2 > 1e-24 ? mg[i] / (float)sqrt(n2) : 1.0f;
        for (uint64_t c = 0; c < cc; c++) w[i * cc + c] *= f;
    }
}
/* na retomada: as magnitudes do arquivo entram nos parametros que tem DoRA ligado; -1 se o arquivo tem DoRA e a opcao nao */
static int tm_dora_resume(const char *path, TTab *tt) {
    TmDsec s = tm_dora_read(path);
    int got = 0;
    for (int i = 0; i < tt->n; i++) {
        TPar *p = &tt->p[i];
        if (!p->r) continue;
        const float *mg = tm_dsec_find(&s, p->name, p->rows);
        if (!mg) { if (tm_has_dora(p)) { tm_dora_prep(p); memcpy(p->tm->mag, p->tm->nrm, p->rows * 4); } continue; }   /* sem magnitude no arquivo: d = 1 */
        if (!tm_has_dora(p)) { tm_dsec_free(&s); fprintf(stderr, "--resume: %s tem DoRA; use --dora\n", path); return -1; }
        memcpy(p->tm->mag, mg, p->rows * 4); got++;
    }
    tm_dsec_free(&s);
    return got;
}

/* ---- opcoes de linha de comando ---- */
static int tm_flag(const char *k) {                   /* opcoes sem valor */
    if (!strcmp(k, "--allow-worse")) { tm.allow_worse = 1; return 1; }
    if (!strcmp(k, "--dora")) { tm.dora = 1; return 1; }
    if (!strcmp(k, "--pissa")) { tm.pissa = 1; return 1; }
    return 0;
}
static int tm_opt(const char *k, const char *v) {     /* opcoes com valor; 1 = reconhecida */
    if (!strcmp(k, "--ema")) tm.ema = atof(v);
    else if (!strcmp(k, "--neft")) tm.neft = (float)atof(v);
    else if (!strcmp(k, "--adapter-dropout")) tm.drop = (float)atof(v);
    else if (!strcmp(k, "--anchor")) tm.anchor = (float)atof(v);
    else if (!strcmp(k, "--label-smoothing")) tm.smooth = (float)atof(v);
    else if (!strcmp(k, "--tool-weight")) tm.tool_w = (float)atof(v);
    else if (!strcmp(k, "--spike")) tm.spike = atof(v);
    else if (!strcmp(k, "--max-skips")) tm.max_skips = atoi(v);
    else if (!strcmp(k, "--optimizer")) { if (!strcmp(v, "muon")) tm.muon = 1; else if (!strcmp(v, "adamw")) tm.muon = 0; else { fprintf(stderr, "--optimizer: adamw ou muon\n"); exit(1); } }
    else return 0;
    return 1;
}
static int tm_validate(int rank, int resume) {
    if (tm.ema < 0 || tm.ema >= 1) { fprintf(stderr, "--ema: decaimento em [0, 1)\n"); return 1; }
    if (tm.drop < 0 || tm.drop >= 1) { fprintf(stderr, "--adapter-dropout: em [0, 1)\n"); return 1; }
    if (tm.smooth < 0 || tm.smooth >= 1) { fprintf(stderr, "--label-smoothing: em [0, 1)\n"); return 1; }
    if (tm.tool_w <= 0 || tm.anchor < 0 || tm.neft < 0) { fprintf(stderr, "opcoes de metodo com valor invalido\n"); return 1; }
    if ((tm.dora || tm.pissa || tm.drop > 0 || tm.muon) && rank <= 0) { fprintf(stderr, "--dora, --pissa, --adapter-dropout e --optimizer muon pedem adaptadores (--rank R)\n"); return 1; }
    if (tm.pissa && resume) { fprintf(stderr, "--pissa nao combina com --resume (o posto muda para 2R)\n"); return 1; }
    if (tm.pissa && tm.muon) { fprintf(stderr, "--pissa e --optimizer muon nao combinam (a parte congelada entraria na ortogonalizacao)\n"); return 1; }
    return 0;
}
static void tm_describe(void) {
    printf("metodos:");
    int any = 0;
    if (tm.ema > 0) { printf(" ema %.4g", tm.ema); any = 1; }
    if (tm.neft > 0) { printf(" neftune %.3g", tm.neft); any = 1; }
    if (tm.drop > 0) { printf(" adapter-dropout %.3g", tm.drop); any = 1; }
    if (tm.dora) { printf(" dora"); any = 1; }
    if (tm.anchor > 0) { printf(" anchor-kl %.3g", tm.anchor); any = 1; }
    if (tm.smooth > 0) { printf(" label-smoothing %.3g", tm.smooth); any = 1; }
    if (tm.tool_w != 1.0f) { printf(" tool-weight %.3g", tm.tool_w); any = 1; }
    if (tm.muon) { printf(" muon"); any = 1; }
    if (tm.pissa) { printf(" pissa"); any = 1; }
    printf(any ? "\n" : " nenhum\n");
}
/* depois da selecao e da retomada: PiSSA, DoRA e media movel */
static void tm_setup_ema(TTab *tt) { if (tm.ema > 0) tm_ema_init(tt); }
static int tm_setup(TTab *tt) {
    if (tm.pissa) {
        long nm = 0;
        const double sec = tm_pissa_init(tt, &nm);
        printf("PiSSA: %ld matrizes iniciadas nas componentes principais em %.2f s (%.3f s por matriz; posto %s)\n", nm, sec, nm ? sec / nm : 0.0, "2R gravado");
    }
    if (tm.dora) for (int i = 0; i < tt->n; i++) tm_dora_enable(&tt->p[i]);
    return 0;
}

/* ---- guardas contra divergencia ---- */
static double g_loss_hist[20]; static int g_nloss, g_skips, g_consec, g_skip_total;
static int tm_cmpd(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
/* 1 = pular o passo (perda ou gradiente nao finitos, ou perda acima de spike vezes a mediana dos ultimos 20 aceitos e
   pelo menos 2 nats acima dela: perda muito baixa oscila em proporcao sem estar divergindo) */
static int tm_guard_step(TTab *tt, double loss, int step) {
    double ss = 0;
    for (int i = 0; i < tt->n; i++) {
        const TPar *p = &tt->p[i];
        if (p->g) for (size_t j = 0; j < p->rows * p->cols; j++) ss += (double)p->g[j] * p->g[j];
        if (p->r) {
            for (size_t j = 0; j < (size_t)p->r * p->cols; j++) ss += (double)p->ga[j] * p->ga[j];
            for (size_t j = 0; j < p->rows * (size_t)p->r; j++) ss += (double)p->gb[j] * p->gb[j];
        }
        ss += tm_gradsq(p);
    }
    const char *why = NULL; double med = 0;
    if (!tm_finite(loss)) why = "perda nao finita";
    else if (!tm_finite(ss)) why = "gradiente nao finito";
    else if (tm.spike > 0 && g_nloss >= 10) {
        double h[20]; const int n = g_nloss < 20 ? g_nloss : 20;
        memcpy(h, g_loss_hist, sizeof(double) * (size_t)n);
        qsort(h, (size_t)n, sizeof(double), tm_cmpd);
        med = h[n / 2];
        if (loss > tm.spike * med && loss > med + 2.0) why = "pico de perda";
    }
    if (why) {
        g_skips++; g_consec++;
        printf("passo %d PULADO (%s: perda %.4g%s%.4g); %d seguidos\n", step, why, loss, med > 0 ? ", mediana " : "", med, g_consec);
        return 1;
    }
    g_consec = 0;
    g_loss_hist[g_nloss % 20] = loss; g_nloss++;
    return 0;
}
static int tm_guard_stop(void) { return tm.max_skips > 0 && g_consec >= tm.max_skips; }

/* ---- baseline e a conferencia de que adaptadores zerados dao o modelo base ---- */
/* perda de validacao do modelo base (tudo desligado); sem retomada nem PiSSA confere tambem que o estado atual de
   treino (adaptadores zerados, mestres iguais ao GGUF) da exatamente a mesma saida. Devolve a perda ou -1 se a conferencia falhou */
static double tm_baseline(Model *m, TTab *tt, TAct *A, const TSet *S, int seq, int fresh) {
    const double t0 = now_sec();
    tm_off = 1; tm_eval(m, tt, A, S, seq, 0, &tm_ev_base); tm_off = 0;
    const double base = tm_ev_base.alln ? tm_ev_base.all / tm_ev_base.alln : 0.0;
    printf("validacao BASE (antes do passo 1, modelo sem especializacao): perda %.4f em %ld alvos (%.1f s)\n", base, tm_ev_base.alln, now_sec() - t0);
    tm_print_src("  por fonte:", &tm_ev_base);
    if (fresh && S->n) {
        const TEx *w = &S->w[0];
        const int T = w->ids.n, V = m->cfg.vocab;
        A->T = T;
        float *l0 = xalloc((size_t)T * V, 4), *l1 = xalloc((size_t)T * V, 4);
        tm_off = 1; t_forward(m, tt, w->ids.ids, A); t_logits(m, tt, A, l0);
        tm_off = 0; t_forward(m, tt, w->ids.ids, A); t_logits(m, tt, A, l1);
        double worst = 0; for (size_t i = 0; i < (size_t)T * V; i++) { const double d = fabs((double)l0[i] - l1[i]); if (d > worst) worst = d; }
        A->T = seq;
        free(l0); free(l1);
        double tol = 0; for (int i = 0; i < tt->n; i++) if (tt->p[i].w) tol = 1e-3;     /* mestres inteiros: a GPU soma em outra ordem */
        if (worst > tol) { fprintf(stderr, "ERRO: com os adaptadores em zero a saida difere da do modelo base (maior diferenca nos logits %.3e); treino recusado\n", worst); return -1; }
        printf("conferencia: adaptadores em zero dao exatamente o modelo base (diferenca maxima nos logits 0 em %d posicoes)\n", T);
    }
    return base;
}

/* ---- katana compare: perda por fonte e deriva ---- */
typedef struct { TmEv ev; double drift, kl; long driftn, kln; } TmRes;
static float *tm_win_logits(Model *m, TTab *tt, TAct *A, const TEx *w) {
    A->T = w->ids.n;
    t_forward(m, tt, w->ids.ids, A);
    float *lg = xalloc((size_t)A->T * m->cfg.vocab, 4);
    t_logits(m, tt, A, lg);
    return lg;
}
static double tm_kl_row(const float *zb, const float *zs, int V) {         /* KL(base || spec) de uma posicao */
    float mb = zb[0], ms = zs[0];
    for (int i = 1; i < V; i++) { if (zb[i] > mb) mb = zb[i]; if (zs[i] > ms) ms = zs[i]; }
    double sb = 0, ss = 0;
    for (int i = 0; i < V; i++) { sb += exp((double)zb[i] - mb); ss += exp((double)zs[i] - ms); }
    const double lb = log(sb) + mb, ls = log(ss) + ms;
    double kl = 0;
    for (int i = 0; i < V; i++) { const double pb = exp((double)zb[i] - lb); kl += pb * ((zb[i] - lb) - (zs[i] - ls)); }
    return kl;
}
static void tm_measure(Model *m, TTab *tt, TAct *A, const TSet *Vs, const TSet *Ds, float **blg, int seq, TmRes *r) {
    memset(r, 0, sizeof *r);
    tm_eval(m, tt, A, Vs, seq, 1, &r->ev);
    for (int i = 0; i < Ds->n; i++) {
        const TEx *w = &Ds->w[i];
        float *lg = tm_win_logits(m, tt, A, w);
        const int T = w->ids.n, V = m->cfg.vocab;
        for (int t = 0; t + 1 < T; t++) {
            if (w->mask[t]) {
                const float *z = lg + (size_t)t * V;
                float mx = z[0]; for (int j = 1; j < V; j++) if (z[j] > mx) mx = z[j];
                double se = 0; for (int j = 0; j < V; j++) se += exp((double)z[j] - mx);
                r->drift += log(se) + mx - z[w->ids.ids[t + 1]]; r->driftn++;
            }
            if (blg) { r->kl += tm_kl_row(blg[i] + (size_t)t * V, lg + (size_t)t * V, V); r->kln++; }
        }
        free(lg);
    }
    A->T = seq;
}
/* le a lista de tensores de um .kspec e liga o treino deles na tabela (para o spec_resume encher), e as magnitudes do DoRA */
static int tm_prescan(TTab *tt, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "compare: nao abriu %s\n", path); return -1; }
    char mg[8]; int64_t fs = 0; uint64_t np = 0; int n = 0;
    if (fread(mg, 1, 8, f) != 8 || memcmp(mg, KSPEC_MAGIC, 8) || fread(&fs, 8, 1, f) != 1 || fread(&np, 8, 1, f) != 1 || fread(&n, 4, 1, f) != 1 || n < 0) { fclose(f); fprintf(stderr, "compare: %s nao e uma especializacao\n", path); return -1; }
    for (int k = 0; k < n; k++) {
        uint32_t nl = 0, kind = 0; uint64_t r = 0, cc = 0; char name[128];
        if (fread(&nl, 4, 1, f) != 1 || nl == 0 || nl >= sizeof name || fread(name, 1, nl, f) != nl || fread(&kind, 4, 1, f) != 1 || fread(&r, 8, 1, f) != 1 || fread(&cc, 8, 1, f) != 1) { fclose(f); return -1; }
        name[nl] = 0;
        int idx = -1;
        for (int i = 0; i < tt->n && idx < 0; i++) if (!strcmp(tt->p[i].name, name)) idx = i;
        int32_t rk = 0; float sc = 0;
        if (kind == 1) { if (fread(&rk, 4, 1, f) != 1 || fread(&sc, 4, 1, f) != 1) { fclose(f); return -1; } }
        if (idx >= 0) tp_enable(tt, idx, kind == 1 ? rk : 0, kind == 1 ? (float)rk : 0);
        fseek(f, (long)(kind == 1 ? ((uint64_t)rk * cc + r * (uint64_t)rk) * 4 : r * cc * 4), SEEK_CUR);
    }
    fclose(f);
    if (spec_flags(path) & KSPEC_DORA) { tm.dora = 1; for (int i = 0; i < tt->n; i++) tm_dora_enable(&tt->p[i]); }
    return n;
}
static void tm_free_tab(TTab *tt) {
    for (int i = 0; i < tt->n; i++) {
        TPar *p = &tt->p[i];
        free(p->w); free(p->g); free(p->m1); free(p->m2); free(p->a); free(p->b); free(p->ga); free(p->gb); free(p->ma1); free(p->ma2); free(p->mb1); free(p->mb2);
        if (p->tm) { struct TmPar *q = p->tm; free(q->mag); free(q->gmag); free(q->mm1); free(q->mm2); free(q->d); free(q->nrm); free(q->nfz); free(q->wn2); free(q->ew); free(q->ea); free(q->eb); free(q->emag); free(q->fa); free(q->fb); free(q); }
    }
    free(tt->p);
}
/* katana compare <modelo> [--val-data F | --data F --val X --seed N] [--drift-data F] [--seq T] [--max-windows N] [--drift-windows N] a.kspec [b.kspec ...] */
static int tm_compare_main(Model *m, int argc, char **argv) {
    const char *vdata = NULL, *data = NULL, *ddata = NULL; double vf = 0.1; unsigned seed = 1; int seq = 256, maxw = 400, maxd = 16;
    const char *specs[32]; int ns = 0;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && !strcmp(argv[i], "--val-data")) vdata = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--data")) data = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--val")) vf = atof(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--seed")) seed = (unsigned)atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--drift-data")) ddata = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--seq")) seq = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--max-windows")) maxw = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--drift-windows")) maxd = atoi(argv[++i]);
        else if (argv[i][0] != '-' && ns < 32) specs[ns++] = argv[i];
        else { fprintf(stderr, "compare: opcao desconhecida %s\n", argv[i]); return 1; }
    }
    if (!ns || (!vdata && !data)) {
        fprintf(stderr, "uso: katana compare <modelo> (--val-data F | --data F [--val X] [--seed N]) [--drift-data F] [--seq T] [--max-windows N] [--drift-windows N] a.kspec [b.kspec ...]\n");
        return 1;
    }
    const Config *c = &m->cfg;
    if (seq > c->ctx) seq = c->ctx;
    TSet Vs = {0}, Ds = {0}, Tr = {0};
    if (vdata) { if (!tset_load(m, vdata, seq, &Vs, NULL, 0, seed, NULL, NULL)) return 1; }
    else if (!tset_load(m, data, seq, &Tr, &Vs, vf, seed, NULL, NULL)) return 1;
    if (ddata && !tset_load(m, ddata, seq, &Ds, NULL, 0, seed, NULL, NULL)) return 1;
    if (Vs.n > maxw) Vs.n = maxw;
    if (Ds.n > maxd) Ds.n = maxd;
    if (!Vs.n) { fprintf(stderr, "compare: nenhuma janela de validacao\n"); return 1; }
    printf("compare: %d janelas de validacao%s%s; %d janelas de deriva%s%s\n", Vs.n, vdata ? " de " : "", vdata ? vdata : " (separadas de --data pela semente)", Ds.n, ddata ? " de " : "", ddata ? ddata : "");
    TAct A; tact_alloc(&A, c, seq);
    TmRes res[33];
    float **blg = Ds.n ? xalloc((size_t)Ds.n, sizeof(float *)) : NULL;
    {
        TTab tt = {0};
        const char *err = tt_build(m, &tt);
        if (err) { fprintf(stderr, "%s\n", err); return 1; }
        for (int i = 0; i < Ds.n; i++) blg[i] = tm_win_logits(m, &tt, &A, &Ds.w[i]);
        printf("  (logits da base guardados para a deriva: %.1f MB)\n", Ds.n * (double)seq * c->vocab * 4 / 1048576.0);
        tm_measure(m, &tt, &A, &Vs, &Ds, blg, seq, &res[0]);
        tm_free_tab(&tt);
    }
    int ok[32] = {0};
    for (int s = 0; s < ns; s++) {
        TTab tt = {0};
        const char *err = tt_build(m, &tt);
        if (err) { fprintf(stderr, "%s\n", err); return 1; }
        tm.dora = 0;
        if (tm_prescan(&tt, specs[s]) < 0 || spec_resume(m, &tt, specs[s]) < 0) { fprintf(stderr, "compare: %s nao pode ser carregada\n", specs[s]); tm_free_tab(&tt); continue; }
        tg.ver++;
        tm_measure(m, &tt, &A, &Vs, &Ds, blg, seq, &res[s + 1]);
        ok[s] = 1;
        tm_free_tab(&tt);
    }
    /* tabela */
    printf("\n%-30s %12s", "", "base");
    for (int s = 0; s < ns; s++) { const char *b = strrchr(specs[s], '/'); printf(" %14.14s", b ? b + 1 : specs[s]); }
    printf("\n");
    #define TMROW(label, expr_base, expr) do { printf("%-30s %12.4f", label, (expr_base)); for (int s = 0; s < ns; s++) { if (ok[s]) printf(" %14.4f", (expr)); else printf(" %14s", "-"); } printf("\n"); } while (0)
    { const TmRes *b = &res[0];
      TMROW("held-out loss (all)", b->ev.all / (b->ev.alln ? b->ev.alln : 1), res[s + 1].ev.all / (res[s + 1].ev.alln ? res[s + 1].ev.alln : 1));
      for (int q = 0; q < g_ntsrc && q < 16; q++) if (b->ev.srcn[q]) {
          char lab[64]; snprintf(lab, sizeof lab, "  source %s (%ld)", g_tsrc[q], b->ev.srcn[q]);
          TMROW(lab, b->ev.src[q] / b->ev.srcn[q], res[s + 1].ev.srcn[q] ? res[s + 1].ev.src[q] / res[s + 1].ev.srcn[q] : 0.0);
      }
      if (b->ev.tooln) { char lab[64]; snprintf(lab, sizeof lab, "  tool-call tokens (%ld)", b->ev.tooln); TMROW(lab, b->ev.tool / b->ev.tooln, res[s + 1].ev.tooln ? res[s + 1].ev.tool / res[s + 1].ev.tooln : 0.0); }
      if (b->driftn) {
          TMROW("drift file loss", b->drift / b->driftn, res[s + 1].driftn ? res[s + 1].drift / res[s + 1].driftn : 0.0);
          printf("%-30s %12s", "drift KL(base||spec), mean", "0");
          for (int s = 0; s < ns; s++) { if (ok[s]) printf(" %14.5f", res[s + 1].kln ? res[s + 1].kl / res[s + 1].kln : 0.0); else printf(" %14s", "-"); }
          printf("\n");
      }
    }
    for (int s = 0; s < ns + 1; s++) {                 /* uma linha por coluna para scripts: base primeiro */
        if (s && !ok[s - 1]) continue;
        const TmRes *q = &res[s];
        printf("compare-json: {\"spec\": \"%s\", \"val\": %.6f, \"tool\": %.6f, \"drift\": %.6f, \"kl\": %.6f}\n", s ? specs[s - 1] : "base",
               q->ev.alln ? q->ev.all / q->ev.alln : 0.0, q->ev.tooln ? q->ev.tool / q->ev.tooln : 0.0, q->driftn ? q->drift / q->driftn : 0.0, q->kln ? q->kl / q->kln : 0.0);
    }
    printf("\nverdict per specialization (held-out loss vs base; drift loss vs base):\n");
    for (int s = 0; s < ns; s++) {
        if (!ok[s]) continue;
        const double vb = res[0].ev.all / (res[0].ev.alln ? res[0].ev.alln : 1), vs = res[s + 1].ev.all / (res[s + 1].ev.alln ? res[s + 1].ev.alln : 1);
        printf("  %s: held-out %+.4f (%s)", specs[s], vs - vb, vs < vb ? "better" : "WORSE");
        if (res[0].driftn) { const double db = res[0].drift / res[0].driftn, ds = res[s + 1].driftn ? res[s + 1].drift / res[s + 1].driftn : 0; printf("; drift loss %+.4f (%s)", ds - db, ds <= db + 1e-9 ? "no forgetting seen" : "forgetting"); }
        printf("\n");
    }
    tact_free(&A);
    return 0;
}

/* ---- gradcheck dos metodos que mudam o gradiente: ruido, dropout, perda moldada (suavizacao, pesos, ancora) e DoRA ----
   A perda diferenciada e a moldada (tm_ret_shaped); a chave do ruido e das mascaras fica fixa, entao e uma funcao
   determinista dos pesos. No DoRA a norma das linhas fica congelada (o artigo tira a norma do gradiente). */
static void tm_reset(void) { memset(&tm, 0, sizeof tm); tm.tool_w = 1.0f; tm.spike = 5.0; tm.max_skips = 5; }
static int tm_gradcheck(Model *m, int T) {
    const Config *c = &m->cfg;
    TTab tt = {0};
    if (tt_build(m, &tt)) return 0;
    if (T > c->ctx) T = c->ctx;
    srand(11);
    int *toks = xalloc(T, sizeof(int));
    for (int t = 0; t < T; t++) toks[t] = rand() % c->vocab;
    uint8_t *mask = xalloc(T, 1);
    for (int t = 0; t < T; t++) mask[t] = (t % 3) != 0;
    TAct A; tact_alloc(&A, c, T);
    tt_select(&tt, "all");
    for (int i = 0; i < tt.n; i++) { TPar *p = &tt.p[i]; if (p->mat && i != tt.tok) { free(p->w); free(p->g); free(p->m1); free(p->m2); p->w = p->g = p->m1 = p->m2 = NULL; } }
    for (int i = 0; i < tt.n; i++) {
        TPar *p = &tt.p[i];
        if (!p->mat || i == tt.tok) continue;
        p->r = 3; p->s = 2.0f / 3;
        p->a = xalloc(3 * p->cols, 4); p->b = xalloc(p->rows * 3, 4); p->ga = xalloc(3 * p->cols, 4); p->gb = xalloc(p->rows * 3, 4);
        for (size_t j = 0; j < 3 * p->cols; j++) p->a[j] = ((float)rand() / RAND_MAX - 0.5f) * 0.2f;
        for (size_t j = 0; j < p->rows * 3; j++) p->b[j] = ((float)rand() / RAND_MAX - 0.5f) * 0.2f;
    }
    static const char *names[] = { "neftune", "adapter-dropout", "label-smoothing + tool-weight", "kl-anchor", "dora", "todos juntos" };
    int fails = 0, checked = 0; double maxrel = 0, hdr = 0;
    (void)hdr;
    for (int cfg = 0; cfg < 6; cfg++) {
        tm_reset();
        const int all = cfg == 5;
        if (cfg == 0 || all) tm.neft = 6.0f;
        if (cfg == 1 || all) tm.drop = 0.3f;
        if (cfg == 2 || all) { tm.smooth = 0.15f; tm.tool_w = 3.0f; tm.force_w = 1; }
        if (cfg == 3 || all) tm.anchor = 0.7f;
        if (cfg == 4) {
            for (int i = 0; i < tt.n; i++) {
                TPar *p = &tt.p[i];
                tm_dora_enable(p);
                if (tm_has_dora(p)) for (size_t j = 0; j < p->rows; j++) p->tm->mag[j] *= 0.7f + 0.6f * (float)rand() / RAND_MAX;
            }
            tm_dora_freeze(&tt, 1);
        }
        tm.key = 424242; tm_on = 1; tm_ret_shaped = 1;
        t_clear_grads(&tt);
        A.fixsel = 0;
        t_step(m, &tt, toks, mask, &A, 1, NULL);
        A.fixsel = 1;
        for (int i = 0; i < tt.n; i++) for (int part = 0; part < 4; part++) {
            TPar *p = &tt.p[i];
            float *W = part == 0 ? p->w : part == 1 ? p->a : part == 2 ? p->b : (p->tm ? p->tm->mag : NULL);
            const float *G = part == 0 ? p->g : part == 1 ? p->ga : part == 2 ? p->gb : (p->tm ? p->tm->gmag : NULL);
            if (!W || !G) continue;
            const size_t n = part == 0 ? p->rows * p->cols : part == 1 ? (size_t)p->r * p->cols : part == 2 ? p->rows * (size_t)p->r : p->rows;
            float *u = xalloc(n, 4), *w0 = xalloc(n, 4);
            memcpy(w0, W, n * 4);
            double wn = 0, un = 0, gu = 0;
            for (size_t j = 0; j < n; j++) wn += (double)w0[j] * w0[j];
            for (size_t j = 0; j < n; j++) {
                const int used = part || i != tt.tok || (int)(j / p->cols) < c->vocab;
                u[j] = used ? (float)rand() / RAND_MAX - 0.5f : 0.0f;
                un += (double)u[j] * u[j];
            }
            const double hs0 = getenv("KATANA_GC_H") ? atof(getenv("KATANA_GC_H")) : 2.0;
            const double wl = 1e-3 * sqrt(wn);
            for (size_t j = 0; j < n; j++) gu += (double)G[j] * u[j];
            double nd[2] = {0, 0}, num = 0, rel = 1;
            /* dois tamanhos de passo: o pequeno sofre com o ruido do float, o grande com a curvatura; um gradiente errado erra nos dois */
            for (int tries = 0; tries < 2; tries++) {
                const double hs = hs0 * (tries ? 4.0 : 1.0), h = hs * (wl > 2e-2 ? wl : 2e-2) / sqrt(un);
                for (int r = 0; r < 2; r++) {
                    const double hh = r ? h / 2 : h;
                    for (size_t j = 0; j < n; j++) W[j] = w0[j] + (float)(hh * u[j]);
                    tg.ver++;
                    const double lp = t_step(m, &tt, toks, mask, &A, 0, NULL);
                    for (size_t j = 0; j < n; j++) W[j] = w0[j] - (float)(hh * u[j]);
                    tg.ver++;
                    const double lm = t_step(m, &tt, toks, mask, &A, 0, NULL);
                    nd[r] = (lp - lm) / (2.0 * hh);
                }
                const double nr = (4.0 * nd[1] - nd[0]) / 3.0;
                num = fabs(nr - gu) < fabs(nd[0] - gu) ? nr : nd[0];
                rel = fabs(num - gu) / (fabs(num) + fabs(gu) + 1e-8);
                if (fabs(num - gu) <= 1e-2 * (fabs(num) + fabs(gu)) + 1e-4) break;
            }
            memcpy(W, w0, n * 4);
            tg.ver++;
            if (rel > maxrel) maxrel = rel;
            if (fabs(num - gu) > 1e-2 * (fabs(num) + fabs(gu)) + 1e-4) {
                printf("  [%s] %s%s: analitico %.6g numerico %.6g (rel %.3g)\n", names[cfg], p->name, part == 1 ? " (A)" : part == 2 ? " (B)" : part == 3 ? " (magnitude)" : "", gu, num, rel);
                fails++;
            }
            checked++;
            free(u); free(w0);
        }
    }
    tm_reset();
    tm_on = 0; tm_ret_shaped = 0;
    printf("gradientes dos metodos (neftune, dropout, suavizacao + pesos, ancora KL, DoRA, todos juntos): %d conferidos, maior erro relativo %.2e%s\n", checked, maxrel, fails ? "  FALHOU" : "");
    tm_free_tab(&tt);
    tact_free(&A); free(toks); free(mask);
    return fails;
}
