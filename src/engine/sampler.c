/* Amostrador: tudo o que decide o proximo token alem de temperatura, top-k e top-p (esses ficam em forward.c).
 * Pipeline sobre uma COPIA dos logits (smp_apply), na ordem: logit_bias, penalidade de repeticao (janela),
 * presence/frequency (semantica da API compativel com OpenAI), DRY, typical-p, min-p, mirostat v2. Os filtros
 * nao removem token: botam -1e30 no logit (SMP_OFF). A gramatica de chamadas e o vies de fluxo conferem o token
 * escolhido (e reamostram sobre estes mesmos logits, smp_view), entao nada aqui devolve um token proibido: nenhum
 * passo SOBE um logit, exceto o logit_bias, que e pedido explicito do chamador, e a gramatica ainda confere o resultado.
 * Valores padrao neutros: sem nenhum parametro o sampler fica desligado (smp_active() = 0) e nada muda.
 * Variaveis de ambiente (console/agente) e campos do corpo (servidor/API): ver docs/api.md. Teste sem modelo: katana samplertest. */
#define SMP_OFF (-1.0e30f)
#define SMP_MAXB 64
#define SMP_MAXK 8
#define SMP_HIST 8192

typedef struct {
    float min_p, typ_p, rep_pen, pres, freq, mir_tau, mir_eta, dry_mult, dry_base;
    int rep_win, dry_alen, dry_range, mir, top_k, has_seed;
    unsigned long long seed;
    int nbias, bias_tok[SMP_MAXB]; float bias_val[SMP_MAXB];
    int nbrk; char brk[SMP_MAXK][32];          /* nbrk < 0: separadores padrao; 0: nenhum */
} SmpCfg;

static SmpCfg g_smp;
static struct {
    int *h, n, cap, nprompt;                  /* historico: prompt (so repeticao e DRY) + gerados */
    unsigned long long rng; int rng_on;
    float mu, lastz; int mu_on;
    float *proc, *pr; int pn;                 /* copia processada dos logits e probabilidades antes do corte (mirostat) */
    const float *view;                        /* logits que o ultimo smp_apply entregou (copia ou original) */
    uint8_t *brk; int brk_V, brk_ok; unsigned brk_key;
    const Tokenizer *tk; int V;
    int *cnt; uint8_t *mark; float *pen; int sn;      /* rascunhos de tamanho do vocabulario */
} g_smps;

static size_t tok_bytes(const Tokenizer *tk, int id, char *out, size_t cap);
static const char *gs_member(const char *p, char *key, size_t kc, const char **val);
static const char *gs_elem(const char *p, const char **val);
static int sample(const float *logits, int n, float temp);

static void smp_defaults(SmpCfg *c) {
    memset(c, 0, sizeof *c);
    c->typ_p = 1.0f; c->rep_pen = 1.0f; c->rep_win = 64;
    c->mir_tau = 5.0f; c->mir_eta = 0.1f; c->dry_base = 1.75f; c->dry_alen = 2; c->nbrk = -1;
}
static int smp_active_cfg(const SmpCfg *c) {
    return c->min_p > 0 || c->typ_p < 1 || c->rep_pen != 1.0f || c->pres != 0 || c->freq != 0 || c->mir == 2 ||
           c->dry_mult > 0 || c->nbias > 0;
}
static int smp_active(void) { return smp_active_cfg(&g_smp); }

/* logit_bias de ambiente: "id:valor,id:valor" */
static void smp_parse_bias(SmpCfg *c, const char *s) {
    c->nbias = 0;
    while (s && *s && c->nbias < SMP_MAXB) {
        char *e; const long id = strtol(s, &e, 10);
        if (e == s || *e != ':') break;
        const float v = (float)strtod(e + 1, &e);
        if (id >= 0) { c->bias_tok[c->nbias] = (int)id; c->bias_val[c->nbias] = v < -100 ? -100 : v > 100 ? 100 : v; c->nbias++; }
        if (*e != ',') break;
        s = e + 1;
    }
}
static void smp_parse_brk(SmpCfg *c, const char *s) {       /* "a|b|\n" (a sequencia \n vira nova linha) */
    c->nbrk = 0;
    while (*s && c->nbrk < SMP_MAXK) {
        char *d = c->brk[c->nbrk]; int n = 0;
        for (; *s && *s != '|' && n < 31; s++) {
            if (*s == '\\' && s[1] == 'n') { d[n++] = '\n'; s++; }
            else if (*s == '\\' && s[1] == 't') { d[n++] = '\t'; s++; }
            else d[n++] = *s;
        }
        d[n] = 0;
        if (n) c->nbrk++;
        if (*s == '|') s++;
    }
}
static void smp_clamp(SmpCfg *c) {
    if (c->typ_p <= 0 || c->typ_p > 1) c->typ_p = 1.0f;
    if (c->min_p < 0 || c->min_p >= 1) c->min_p = 0;
    if (c->rep_pen <= 0) c->rep_pen = 1.0f;
    if (c->mir != 2) c->mir = 0;
    if (c->dry_alen < 1) c->dry_alen = 1;
}
/* variaveis de ambiente -> g_smp (chamado ao carregar o modelo) */
static void smp_env_load(void) {
    SmpCfg *c = &g_smp; const char *e;
    smp_defaults(c);
    if ((e = getenv("KATANA_MIN_P"))) c->min_p = (float)atof(e);
    if ((e = getenv("KATANA_TYPICAL_P"))) c->typ_p = (float)atof(e);
    if ((e = getenv("KATANA_REPEAT_PENALTY"))) c->rep_pen = (float)atof(e);
    if ((e = getenv("KATANA_REPEAT_LAST_N"))) c->rep_win = atoi(e);
    if ((e = getenv("KATANA_PRESENCE_PENALTY"))) c->pres = (float)atof(e);
    if ((e = getenv("KATANA_FREQUENCY_PENALTY"))) c->freq = (float)atof(e);
    if ((e = getenv("KATANA_MIROSTAT"))) c->mir = atoi(e);
    if ((e = getenv("KATANA_MIROSTAT_TAU"))) c->mir_tau = (float)atof(e);
    if ((e = getenv("KATANA_MIROSTAT_ETA"))) c->mir_eta = (float)atof(e);
    if ((e = getenv("KATANA_DRY_MULTIPLIER"))) c->dry_mult = (float)atof(e);
    if ((e = getenv("KATANA_DRY_BASE"))) c->dry_base = (float)atof(e);
    if ((e = getenv("KATANA_DRY_ALLOWED_LENGTH"))) c->dry_alen = atoi(e);
    if ((e = getenv("KATANA_DRY_RANGE"))) c->dry_range = atoi(e);
    if ((e = getenv("KATANA_DRY_BREAKERS"))) smp_parse_brk(c, e);
    if ((e = getenv("KATANA_LOGIT_BIAS"))) smp_parse_bias(c, e);
    if ((e = getenv("KATANA_SEED")) && *e) { c->seed = strtoull(e, NULL, 10); c->has_seed = 1; }
    smp_clamp(c);
    if (c->has_seed) { g_smps.rng = c->seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull; g_smps.rng_on = 1; }
}
static void smp_bind(const Tokenizer *tk, int V) { g_smps.tk = tk; g_smps.V = V; g_smps.brk_ok = 0; smp_env_load(); }

/* sorteio: com seed, o gerador proprio (mesma seed => mesma sequencia); sem seed, rand() como sempre */
static void smp_seed(unsigned long long s) {
    g_smps.rng = s * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
    g_smps.rng_on = 1;
}
static float smp_rand01(void) {
    if (!g_smps.rng_on) return (float)rand() / ((float)RAND_MAX + 1.0f);
    unsigned long long z = (g_smps.rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
    return (float)((z >> 40) & 0xFFFFFF) / 16777216.0f;
}

/* comeco de uma geracao: zera historico e mirostat; prime = tokens do prompt (contam so para repeticao e DRY) */
static void smp_hist_alloc(void) { if (!g_smps.h) { g_smps.cap = SMP_HIST + 64; g_smps.h = xalloc((size_t)g_smps.cap, sizeof(int)); } }
static void smp_begin(const int *prime, int n) {
    g_smps.n = 0; g_smps.nprompt = 0; g_smps.mu_on = 0;
    if (prime && n > 0) {
        if (n > SMP_HIST / 2) { prime += n - SMP_HIST / 2; n = SMP_HIST / 2; }
        smp_hist_alloc();
        memcpy(g_smps.h, prime, (size_t)n * sizeof(int)); g_smps.n = g_smps.nprompt = n;
    }
}
static void smp_push(int t) {
    smp_hist_alloc();
    if (g_smps.n >= SMP_HIST) {
        const int drop = SMP_HIST / 4;
        memmove(g_smps.h, g_smps.h + drop, (size_t)(g_smps.n - drop) * sizeof(int));
        g_smps.n -= drop; g_smps.nprompt = g_smps.nprompt > drop ? g_smps.nprompt - drop : 0;
    }
    g_smps.h[g_smps.n++] = t;
}

/* separadores do DRY: todo token cujo texto contem algum separador */
static void smp_brk_build(void) {
    const SmpCfg *c = &g_smp;
    unsigned key = 2166136261u;
    for (int i = 0; i < c->nbrk; i++) for (const char *s = c->brk[i]; *s; s++) key = (key ^ (unsigned char)*s) * 16777619u;
    key ^= (unsigned)c->nbrk;
    if (g_smps.brk_ok && g_smps.brk_key == key) return;
    g_smps.brk_ok = 1; g_smps.brk_key = key;
    if (!g_smps.tk || g_smps.V <= 0) return;
    static const char *def[] = { "\n", ":", "\"", "*" };
    free(g_smps.brk); g_smps.brk = xalloc((size_t)g_smps.V, 1); g_smps.brk_V = g_smps.V;
    char tmp[256];
    for (int t = 0; t < g_smps.V; t++) {
        const size_t L = tok_bytes(g_smps.tk, t, tmp, sizeof tmp - 1);
        if (L == 0) continue;
        tmp[L < sizeof tmp ? L : sizeof tmp - 1] = 0;
        if (c->nbrk < 0) { for (int i = 0; i < 4; i++) if (strstr(tmp, def[i])) { g_smps.brk[t] = 1; break; } }
        else for (int i = 0; i < c->nbrk; i++) if (strstr(tmp, c->brk[i])) { g_smps.brk[t] = 1; break; }
    }
}
static int smp_is_brk(int t) { return g_smps.brk && t >= 0 && t < g_smps.brk_V && g_smps.brk[t]; }

static float *g_smp_key;
static int smp_cmp_dev(const void *a, const void *b) {
    const float x = g_smp_key[*(const int *)a], y = g_smp_key[*(const int *)b];
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Processa os logits; devolve o ponteiro a usar (os mesmos logits se nada esta ligado). temp <= 0 (guloso): so as
 * penalidades e o vies valem; os cortes por probabilidade nao mudam o maior. */
static const float *smp_apply(const float *lg, int n, float temp) {
    const SmpCfg *c = &g_smp;
    g_smps.view = lg; g_smps.lastz = 0;
    if (!smp_active_cfg(c) || n <= 0) return lg;
    if (g_smps.pn < n) {
        free(g_smps.proc); free(g_smps.pr); free(g_smps.cnt); free(g_smps.mark); free(g_smps.pen);
        g_smps.proc = xalloc((size_t)n, sizeof(float)); g_smps.pr = xalloc((size_t)n, sizeof(float));
        g_smps.cnt = xalloc((size_t)n, sizeof(int)); g_smps.mark = xalloc((size_t)n, 1); g_smps.pen = xalloc((size_t)n, sizeof(float));
        g_smps.pn = n;
    }
    float *p = g_smps.proc;
    memcpy(p, lg, (size_t)n * sizeof(float));
    g_smps.view = p;
    const int H = g_smps.n;
    for (int i = 0; i < c->nbias; i++) if (c->bias_tok[i] < n) p[c->bias_tok[i]] += c->bias_val[i];
    if (c->rep_pen != 1.0f && c->rep_win != 0) {          /* janela: ultimos rep_win tokens (prompt incluso); -1 = tudo */
        const int w = c->rep_win < 0 || c->rep_win > H ? H : c->rep_win;
        for (int i = H - w; i < H; i++) {
            const int t = g_smps.h[i];
            if (t < 0 || t >= n || g_smps.mark[t]) continue;      /* cada token so uma vez */
            g_smps.mark[t] = 1;
            p[t] = p[t] > 0 ? p[t] / c->rep_pen : p[t] * c->rep_pen;
        }
        for (int i = H - w; i < H; i++) { const int t = g_smps.h[i]; if (t >= 0 && t < n) g_smps.mark[t] = 0; }
    }
    if ((c->pres != 0 || c->freq != 0) && H > g_smps.nprompt) {    /* so os gerados: logit -= cont * freq + (cont > 0) * pres */
        for (int i = g_smps.nprompt; i < H; i++) { const int t = g_smps.h[i]; if (t >= 0 && t < n) g_smps.cnt[t]++; }
        for (int i = g_smps.nprompt; i < H; i++) {
            const int t = g_smps.h[i];
            if (t < 0 || t >= n || g_smps.cnt[t] == 0) continue;
            p[t] -= (float)g_smps.cnt[t] * c->freq + c->pres;
            g_smps.cnt[t] = 0;
        }
    }
    if (c->dry_mult > 0 && H >= 2) {
        smp_brk_build();
        const int range = c->dry_range > 0 && c->dry_range < H ? c->dry_range : H > 4096 ? 4096 : H;
        const int last = g_smps.h[H - 1];
        float *pen = g_smps.pen;
        int *touched = xalloc((size_t)range + 1, sizeof(int)); int nt = 0;
        if (!smp_is_brk(last)) for (int i = H - range > 1 ? H - range : 1; i < H; i++) {
            const int tok = g_smps.h[i];
            if (tok < 0 || tok >= n || smp_is_brk(tok)) continue;
            int L = 0;     /* quantos tokens antes de i repetem os ultimos tokens do historico, sem cruzar um separador */
            while (L < 64 && i - 1 - L >= 0 && g_smps.h[i - 1 - L] == g_smps.h[H - 1 - L] && !smp_is_brk(g_smps.h[H - 1 - L])) L++;
            if (L < c->dry_alen) continue;
            const float q = c->dry_mult * powf(c->dry_base, (float)(L - c->dry_alen));
            if (pen[tok] == 0) touched[nt++] = tok;
            if (q > pen[tok]) pen[tok] = q;
        }
        for (int i = 0; i < nt; i++) { p[touched[i]] -= pen[touched[i]]; pen[touched[i]] = 0; }
        free(touched);
    }
    if (temp > 0.0f && (c->typ_p < 1 || c->min_p > 0 || c->mir == 2)) {
        float mx = -3.4e38f;
        for (int i = 0; i < n; i++) if (p[i] > mx) mx = p[i];
        float *pr = g_smps.pr; double z = 0;
        for (int i = 0; i < n; i++) { pr[i] = p[i] <= SMP_OFF * 0.5f ? 0.0f : expf((p[i] - mx) / temp); z += pr[i]; }
        for (int i = 0; i < n; i++) pr[i] = (float)(pr[i] / z);
        if (c->typ_p < 1) {                               /* tipico: fica o conjunto de menor |surpresa - entropia| que soma typ_p */
            double Hn = 0; int m = 0;
            int *cand = xalloc((size_t)n, sizeof(int)); float *key = xalloc((size_t)n, sizeof(float));
            for (int i = 0; i < n; i++) if (pr[i] > 1e-9f) { Hn -= (double)pr[i] * logf(pr[i]); cand[m++] = i; }
            for (int i = 0; i < m; i++) key[cand[i]] = fabsf(-logf(pr[cand[i]]) - (float)Hn);
            g_smp_key = key; qsort(cand, (size_t)m, sizeof(int), smp_cmp_dev);
            double cum = 0; int kept = 0;
            while (kept < m) { cum += pr[cand[kept++]]; if (cum >= c->typ_p) break; }
            uint8_t *keep = xalloc((size_t)n, 1);
            for (int i = 0; i < kept; i++) keep[cand[i]] = 1;
            for (int i = 0; i < n; i++) if (!keep[i]) { p[i] = SMP_OFF; pr[i] = 0; }
            free(keep); free(cand); free(key);
        }
        if (c->min_p > 0) {                               /* min-p: probabilidade >= min_p * maior */
            float pm = 0; for (int i = 0; i < n; i++) if (pr[i] > pm) pm = pr[i];
            for (int i = 0; i < n; i++) if (pr[i] < c->min_p * pm) { p[i] = SMP_OFF; pr[i] = 0; }
        }
        if (c->mir == 2) {                                /* mirostat v2: corta o que surpreende mais que mu bits (o melhor sempre fica) */
            if (!g_smps.mu_on) { g_smps.mu = 2.0f * c->mir_tau; g_smps.mu_on = 1; }
            float pm = 0; int bi = 0;
            for (int i = 0; i < n; i++) if (pr[i] > pm) { pm = pr[i]; bi = i; }
            double zk = 0;
            for (int i = 0; i < n; i++) {
                if (pr[i] <= 0) { p[i] = SMP_OFF; continue; }
                if (i != bi && -log2f(pr[i]) > g_smps.mu) { p[i] = SMP_OFF; pr[i] = 0; } else zk += pr[i];
            }
            g_smps.lastz = (float)zk;
        }
    }
    return p;
}
/* mirostat v2 ignora top-k e top-p (o corte dele e o proprio) */
static int smp_bypass(float temp) { return g_smp.mir == 2 && temp > 0.0f; }
/* logits que o ultimo smp_apply entregou, para quem reamostra (cg_check): se nada processou, os originais */
static const float *smp_view(const float *lg) { return g_smps.proc && g_smps.view == g_smps.proc ? g_smps.proc : lg; }

static void smp_mu_step(int t, int sign) {
    if (g_smp.mir == 2 && g_smps.mu_on && g_smps.lastz > 0 && t >= 0 && t < g_smps.pn && g_smps.pr[t] > 0)
        g_smps.mu -= (float)sign * g_smp.mir_eta * (-log2f(g_smps.pr[t] / g_smps.lastz) - g_smp.mir_tau);
}
/* o token escolhido entra no historico (e atualiza o mu) */
static void smp_accept(int t) {
    if (!smp_active_cfg(&g_smp)) return;
    smp_mu_step(t, 1); smp_push(t);
}
/* a camada de chamadas trocou o token escolhido: o historico fica com o definitivo */
static void smp_replace(int from, int to) {
    if (!smp_active_cfg(&g_smp) || g_smps.n == 0 || g_smps.h[g_smps.n - 1] != from) return;
    smp_mu_step(from, -1);
    g_smps.h[g_smps.n - 1] = to; smp_mu_step(to, 1);
}

/* ---- campos do corpo JSON (servidor e API): devolve 1 se k e um parametro de amostragem (v e o valor) ---- */
static int smp_jstr(const char *v, char *out, size_t cap) {
    if (*v != '"') return 0;
    size_t n = 0;
    for (v++; *v && *v != '"'; v++) {
        char ch = *v;
        if (ch == '\\' && v[1]) { v++; ch = *v == 'n' ? '\n' : *v == 't' ? '\t' : *v == 'r' ? '\r' : *v; }
        if (n + 1 < cap) out[n++] = ch;
    }
    out[n] = 0; return 1;
}
static int smp_json_field(SmpCfg *c, const char *k, const char *v) {
    const int isnum = *v == '-' || *v == '.' || (*v >= '0' && *v <= '9');
    const double d = isnum ? strtod(v, NULL) : 0;
    if (!strcmp(k, "min_p")) { if (isnum) c->min_p = d > 0 && d < 1 ? (float)d : 0; }
    else if (!strcmp(k, "typical_p")) { if (isnum) c->typ_p = d > 0 && d <= 1 ? (float)d : 1.0f; }
    else if (!strcmp(k, "repeat_penalty")) { if (isnum && d > 0) c->rep_pen = (float)d; }
    else if (!strcmp(k, "repeat_last_n")) { if (isnum) c->rep_win = (int)d; }
    else if (!strcmp(k, "presence_penalty")) { if (isnum) c->pres = (float)(d < -2 ? -2 : d > 2 ? 2 : d); }
    else if (!strcmp(k, "frequency_penalty")) { if (isnum) c->freq = (float)(d < -2 ? -2 : d > 2 ? 2 : d); }
    else if (!strcmp(k, "mirostat")) { if (isnum) c->mir = (int)d == 2 ? 2 : 0; }
    else if (!strcmp(k, "mirostat_tau")) { if (isnum && d > 0) c->mir_tau = (float)d; }
    else if (!strcmp(k, "mirostat_eta")) { if (isnum && d > 0) c->mir_eta = (float)d; }
    else if (!strcmp(k, "dry_multiplier")) { if (isnum) c->dry_mult = d > 0 ? (float)d : 0; }
    else if (!strcmp(k, "dry_base")) { if (isnum && d >= 1) c->dry_base = (float)d; }
    else if (!strcmp(k, "dry_allowed_length")) { if (isnum) c->dry_alen = d < 1 ? 1 : (int)d; }
    else if (!strcmp(k, "dry_range")) { if (isnum) c->dry_range = d < 0 ? 0 : (int)d; }
    else if (!strcmp(k, "top_k")) { if (isnum && d >= 1) c->top_k = (int)d; }
    else if (!strcmp(k, "dry_sequence_breakers") && *v == '[') {
        c->nbrk = 0; const char *e, *q = v + 1, *el;
        while ((e = gs_elem(q, &el)) && c->nbrk < SMP_MAXK) { if (smp_jstr(el, c->brk[c->nbrk], 32) && c->brk[c->nbrk][0]) c->nbrk++; q = e; }
    } else if (!strcmp(k, "logit_bias") && *v == '{') {
        c->nbias = 0; char key[32]; const char *val, *e, *q = v + 1;
        while ((e = gs_member(q, key, sizeof key, &val)) && c->nbias < SMP_MAXB) {
            const long id = strtol(key, NULL, 10);
            if (id >= 0 && (*val == '-' || (*val >= '0' && *val <= '9'))) {
                const double b = strtod(val, NULL);
                c->bias_tok[c->nbias] = (int)id; c->bias_val[c->nbias] = (float)(b < -100 ? -100 : b > 100 ? 100 : b); c->nbias++;
            }
            q = e;
        }
    } else return 0;
    smp_clamp(c);
    return 1;
}

/* escopo de uma requisicao: aplica os parametros dela (e top_k, seed) e devolve o estado anterior em smp_scope_end */
static int g_top_k;
typedef struct { SmpCfg cfg; int top_k, rng_on; unsigned long long rng; } SmpScope;
static void smp_scope_begin(SmpScope *sv, const SmpCfg *rq, int has_seed, unsigned seed, const int *prime, int n) {
    sv->cfg = g_smp; sv->top_k = g_top_k; sv->rng_on = g_smps.rng_on; sv->rng = g_smps.rng;
    g_smp = *rq;
    if (rq->top_k > 0) g_top_k = rq->top_k > TOP_K ? TOP_K : rq->top_k;
    if (has_seed) { srand(seed); smp_seed(seed); }
    smp_begin(prime, n);
}
static void smp_scope_end(const SmpScope *sv) {
    g_smp = sv->cfg; g_top_k = sv->top_k; g_smps.rng_on = sv->rng_on; g_smps.rng = sv->rng;
    smp_begin(NULL, 0);
}

/* ---- teste sem modelo: katana samplertest (distribuicoes sinteticas) ---- */
static int st_n, st_fail;
#define ST_EXPECT(cond, what) do { st_n++; if (!(cond)) { st_fail++; printf("FAILED: %s\n", what); } } while (0)
/* logits = log das probabilidades dadas */
static void st_logits(float *lg, const float *pr, int n) { for (int i = 0; i < n; i++) lg[i] = logf(pr[i]); }
/* conjunto que sobrou depois de smp_apply (mascara de bits dos tokens com logit > SMP_OFF/2) */
static unsigned st_set(const float *p, int n) { unsigned m = 0; for (int i = 0; i < n; i++) if (p[i] > SMP_OFF * 0.5f) m |= 1u << i; return m; }
static double st_sum(const float *p, int n, float temp) {
    float tmp[64]; memcpy(tmp, p, (size_t)n * sizeof(float));
    for (int i = 0; i < n; i++) tmp[i] /= temp;
    softmax(tmp, n);
    double s = 0; for (int i = 0; i < n; i++) s += tmp[i];
    return s;
}
static int samplertest(void) {
    const SmpCfg keep = g_smp; const int keep_n = g_smps.n, keep_np = g_smps.nprompt;
    const float P5[5] = { 0.5f, 0.25f, 0.15f, 0.06f, 0.04f };
    float lg[64]; const float *p;
    st_logits(lg, P5, 5);
    /* neutro: nada ligado, mesmos logits */
    smp_defaults(&g_smp); smp_begin(NULL, 0);
    ST_EXPECT(!smp_active(), "neutro: desligado");
    ST_EXPECT(smp_apply(lg, 5, 1.0f) == lg, "neutro: devolve os mesmos logits");
    /* min-p 0.2: limite 0.1 -> ficam 0.5, 0.25, 0.15 */
    smp_defaults(&g_smp); g_smp.min_p = 0.2f;
    p = smp_apply(lg, 5, 1.0f);
    ST_EXPECT(st_set(p, 5) == 0x7u, "min-p 0.2 deixa {0,1,2}");
    ST_EXPECT(fabs(st_sum(p, 5, 1.0f) - 1.0) < 1e-5, "min-p: soma 1");
    g_smp.min_p = 0.9f; p = smp_apply(lg, 5, 1.0f);
    ST_EXPECT(st_set(p, 5) == 0x1u, "min-p 0.9 deixa so o maior");
    /* typical-p 0.6 em {.4,.3,.2,.05,.05} -> {0,1,2} */
    { const float T[5] = { 0.4f, 0.3f, 0.2f, 0.05f, 0.05f }; float l2[5]; st_logits(l2, T, 5);
      smp_defaults(&g_smp); g_smp.typ_p = 0.6f; p = smp_apply(l2, 5, 1.0f);
      ST_EXPECT(st_set(p, 5) == 0x7u, "typical-p 0.6 deixa {0,1,2}");
      ST_EXPECT(fabs(st_sum(p, 5, 1.0f) - 1.0) < 1e-5, "typical-p: soma 1");
      g_smp.typ_p = 0.25f; p = smp_apply(l2, 5, 1.0f);
      ST_EXPECT(st_set(p, 5) == 0x2u, "typical-p 0.25 deixa so o token 1 (o mais tipico)"); }
    /* mirostat v2, tau=1 -> mu=2 bits: -log2(p) <= 2 fica ({0,1}) */
    smp_defaults(&g_smp); g_smp.mir = 2; g_smp.mir_tau = 1.0f; smp_begin(NULL, 0);
    p = smp_apply(lg, 5, 1.0f);
    ST_EXPECT(st_set(p, 5) == 0x3u, "mirostat v2 deixa {0,1}");
    { const float mu0 = g_smps.mu; smp_accept(1); ST_EXPECT(fabsf(g_smps.mu - (mu0 - 0.1f * (-log2f(0.25f / 0.75f) - 1.0f))) < 1e-4f, "mirostat: mu desce pela surpresa observada"); }
    for (int i = 0; i < 200; i++) { p = smp_apply(lg, 5, 1.0f); smp_accept(sample(p, 5, 1.0f)); }
    ST_EXPECT(g_smps.mu > 0 && g_smps.mu < 20 && isfinite(g_smps.mu), "mirostat: mu continua finito apos 200 passos");
    /* repeticao: positivo divide, negativo multiplica; so quem esta na janela */
    smp_defaults(&g_smp); g_smp.rep_pen = 2.0f; g_smp.rep_win = 2;
    { int pm[3] = { 4, 3, 2 }; smp_begin(pm, 3); float l3[5] = { 2, 2, 2, -2, 2 };
      p = smp_apply(l3, 5, 1.0f);
      ST_EXPECT(p[2] == 1.0f && p[3] == -4.0f && p[4] == 2.0f && p[0] == 2.0f, "repeticao: so a janela (2 ultimos), positivo/2 e negativo*2"); }
    smp_defaults(&g_smp); g_smp.rep_pen = 2.0f; g_smp.rep_win = -1;
    { int pm[3] = { 4, 3, 2 }; smp_begin(pm, 3); float l3[5] = { 2, 2, 2, -2, 2 }; p = smp_apply(l3, 5, 1.0f);
      ST_EXPECT(p[4] == 1.0f, "repeticao com janela -1: historico inteiro"); }
    /* presence/frequency: so gerados; massa do token repetido cai */
    smp_defaults(&g_smp); g_smp.pres = 0.5f; g_smp.freq = 0.25f;
    { int pm[1] = { 4 }; smp_begin(pm, 1); smp_accept(1); smp_accept(1); smp_accept(1);
      p = smp_apply(lg, 5, 1.0f);
      ST_EXPECT(fabsf(p[1] - (lg[1] - (3 * 0.25f + 0.5f))) < 1e-5f, "freq/pres: logit -= 3*0.25 + 0.5");
      ST_EXPECT(p[4] == lg[4], "freq/pres: token do prompt nao conta");
      float a[64], b[64]; memcpy(a, lg, sizeof(float) * 5); memcpy(b, p, sizeof(float) * 5);
      softmax(a, 5); softmax(b, 5);
      ST_EXPECT(b[1] < a[1] && b[0] > a[0], "freq/pres: massa do token repetido cai, a dos outros sobe"); }
    /* DRY: [5 6 7 5 6] -> 7 continuaria a sequencia repetida (comprimento 2) */
    smp_defaults(&g_smp); g_smp.dry_mult = 1.0f; g_smp.dry_base = 2.0f; g_smp.dry_alen = 2;
    g_smps.brk_ok = 1; g_smps.brk_key = 0; free(g_smps.brk); g_smps.brk = NULL; g_smps.tk = NULL;
    { int hs[5] = { 5, 6, 7, 5, 6 }; float l9[10] = { 0 }; smp_begin(hs, 5); p = smp_apply(l9, 10, 1.0f);
      ST_EXPECT(p[7] == -1.0f && p[5] == 0 && p[6] == 0 && p[0] == 0, "DRY: so o token que continua a repeticao e penalizado (mult*base^0)");
      int hs2[7] = { 5, 6, 7, 9, 5, 6, 7 }; smp_begin(hs2, 7); p = smp_apply(l9, 10, 1.0f);
      ST_EXPECT(p[9] == -2.0f, "DRY: repeticao de 3 -> mult*base^(3-2)");
      smp_begin(hs, 5); g_smps.brk = xalloc(16, 1); g_smps.brk_V = 16; g_smps.brk[6] = 1; g_smps.brk_ok = 1; g_smps.brk_key = 0;
      p = smp_apply(l9, 10, 1.0f);
      ST_EXPECT(p[7] == 0, "DRY: separador interrompe a sequencia"); }
    /* logit_bias */
    smp_defaults(&g_smp); g_smp.nbias = 2; g_smp.bias_tok[0] = 0; g_smp.bias_val[0] = -100; g_smp.bias_tok[1] = 4; g_smp.bias_val[1] = 5;
    smp_begin(NULL, 0); p = smp_apply(lg, 5, 1.0f);
    ST_EXPECT(p[0] == lg[0] - 100 && p[4] == lg[4] + 5 && p[2] == lg[2], "logit_bias soma o vies");
    { float a[8]; memcpy(a, p, sizeof(float) * 5); softmax(a, 5); ST_EXPECT(a[0] < 1e-6f && a[4] > 0.5f, "logit_bias -100 remove, +5 domina"); }
    /* nunca resgata: sem logit_bias nenhum passo sobe um logit, e o que foi cortado fica em SMP_OFF */
    smp_defaults(&g_smp); g_smp.min_p = 0.1f; g_smp.typ_p = 0.9f; g_smp.rep_pen = 1.3f; g_smp.pres = 0.2f; g_smp.freq = 0.1f; g_smp.dry_mult = 0.5f;
    { int pm[4] = { 1, 2, 1, 2 }; smp_begin(pm, 4); smp_accept(0); smp_accept(1);
      float lq[6] = { 1.5f, 0.2f, 0.9f, -3.0f, SMP_OFF, 0.4f };    /* o token 4 ja chega proibido (como a gramatica o deixa) */
      p = smp_apply(lq, 6, 0.8f); int up = 0; for (int i = 0; i < 6; i++) if (p[i] > lq[i]) up = 1;
      ST_EXPECT(!up, "nenhum logit sobe (token proibido continua proibido)");
      ST_EXPECT(p[4] <= SMP_OFF * 0.5f, "token ja proibido continua fora do conjunto"); }
    /* greedy: cortes por probabilidade nao mudam o maior; penalidade pode */
    smp_defaults(&g_smp); g_smp.min_p = 0.5f; smp_begin(NULL, 0);
    p = smp_apply(lg, 5, 0.0f); ST_EXPECT(p[0] == lg[0] && p[4] == lg[4], "temperatura 0: min-p nao mexe");
    /* seed: mesma seed => mesma sequencia (sorteio das amostras sobre distribuicao sintetica) */
    smp_defaults(&g_smp); g_smp.min_p = 0.05f; g_smp.rep_pen = 1.1f;
    { int a[60], b[60], c2[60], diff = 0;
      for (int run = 0; run < 3; run++) {
          int *o = run == 0 ? a : run == 1 ? b : c2;
          smp_seed(run == 2 ? 99 : 1234); smp_begin(NULL, 0);
          for (int i = 0; i < 60; i++) { p = smp_apply(lg, 5, 1.0f); o[i] = sample(p, 5, 1.0f); smp_accept(o[i]); }
      }
      ST_EXPECT(memcmp(a, b, sizeof a) == 0, "mesma seed => mesma sequencia");
      for (int i = 0; i < 60; i++) if (a[i] != c2[i]) diff = 1;
      ST_EXPECT(diff, "outra seed => outra sequencia"); }
    /* amostras seguem a distribuicao renormalizada do min-p 0.2 ({.5,.25,.15} -> .556,.278,.167) */
    smp_defaults(&g_smp); g_smp.min_p = 0.2f; smp_seed(7);
    { int cnt[5] = { 0 }; const int N = 20000;
      for (int i = 0; i < N; i++) { smp_begin(NULL, 0); p = smp_apply(lg, 5, 1.0f); const int t = sample(p, 5, 1.0f); cnt[t]++; smp_accept(t); }
      ST_EXPECT(cnt[3] == 0 && cnt[4] == 0, "amostras nunca saem do conjunto filtrado");
      ST_EXPECT(fabs(cnt[0] / (double)N - 0.5 / 0.9) < 0.02 && fabs(cnt[1] / (double)N - 0.25 / 0.9) < 0.02, "frequencias seguem a distribuicao renormalizada"); }
    /* JSON da API */
    smp_defaults(&g_smp);
    { SmpCfg c2; smp_defaults(&c2);
      ST_EXPECT(smp_json_field(&c2, "min_p", "0.1") && c2.min_p == 0.1f, "json: min_p");
      ST_EXPECT(smp_json_field(&c2, "presence_penalty", "9") && c2.pres == 2.0f, "json: presence_penalty limitado a 2");
      ST_EXPECT(smp_json_field(&c2, "logit_bias", "{\"12\": -100, \"7\": 3.5}") && c2.nbias == 2 && c2.bias_tok[0] == 12 && c2.bias_val[0] == -100 && c2.bias_val[1] == 3.5f, "json: logit_bias");
      ST_EXPECT(smp_json_field(&c2, "dry_sequence_breakers", "[\"\\n\", \":\"]") && c2.nbrk == 2 && c2.brk[0][0] == '\n' && c2.brk[1][0] == ':', "json: dry_sequence_breakers");
      ST_EXPECT(!smp_json_field(&c2, "temperature", "0.5"), "json: campo alheio nao e consumido");
      ST_EXPECT(smp_active_cfg(&c2), "json: configuracao ativa"); }
    g_smp = keep; g_smps.n = keep_n; g_smps.nprompt = keep_np; g_smps.rng_on = 0;
    printf("samplertest: %d tests, %d failures\n", st_n, st_fail);
    return st_fail != 0;
}
