/* Prefetch dos especialistas frios para a cache da CPU, durante a espera pela GPU (geracao do qwen35moe no grafo Vulkan).
 *
 * A CPU espera a GPU terminar o DeltaNet/atencao de cada camada (leitura bloqueante). Nesse tempo uma thread de apoio
 * (propria, nao e a do pool) aplica o roteador adiantado da camada seguinte ao que a CPU ja conhece (a entrada
 * normalizada do feed-forward da camada l, da qual se recupera o residual a menos de escala: x = xn / g; a norma e
 * invariante a escala), escolhe os N especialistas mais provaveis que NAO estao nas vagas da GPU e le os tres blocos
 * de cada um (gate, up, down) linha a linha: a leitura faz a falta de pagina do arquivo mapeado acontecer aqui e traz
 * as linhas para L2/L3. O volume fica limitado a metade do L3 (sysconf/sysfs; padrao 8 MB) e a thread para na hora em
 * que a camada real chega (pf_before). So le: nenhuma saida numerica muda.
 *
 *   KATANA_MOE_PF=0             desliga (padrao ligado)
 *   KATANA_MOE_PF_N=<n>         quantos prever por camada (padrao 12; recall 79% com 8 e 90% com 12 no previsor)
 *   KATANA_MOE_PF_MB=<mb>       limite de volume por camada em MB (padrao metade do L3)
 *   KATANA_MOE_PF_STATS=1       imprime previsoes certas/erradas no fim (tambem com KATANA_DEBUG) */
#if defined(__linux__)
#include <unistd.h>

#define PF_MAXN 32
static struct {
    int init, on, stats, N, E, d, nl; size_t budget;
    pthread_mutex_t mu; pthread_cond_t cv;
    unsigned gen;                                       /* sobe a cada trabalho novo e a cada cancelamento */
    Model *m;
    /* trabalho pendente */
    int pend, jl; unsigned jgen; float *jx;
    /* resultado do trabalho publicado */
    int pl_layer, pl_n, pl[PF_MAXN]; volatile int pl_started, pl_full;
    /* residencia na VRAM antes do MoE da camada, para a conta */
    unsigned char *res;
    /* contagens */
    uint64_t jobs, cancelled, sel_total, sel_cold, hit_full, hit_any, no_pred, listed, bytes;
    volatile unsigned sink;
} pf;

static size_t pf_l3_bytes(void) {
    long v = 0;
#ifdef _SC_LEVEL3_CACHE_SIZE
    v = sysconf(_SC_LEVEL3_CACHE_SIZE);
#endif
    if (v <= 0) {
        FILE *f = fopen("/sys/devices/system/cpu/cpu0/cache/index3/size", "r");
        if (f) { char b[32] = ""; if (fgets(b, sizeof b, f)) { v = atol(b); if (strchr(b, 'K')) v *= 1024; else if (strchr(b, 'M')) v *= 1048576; } fclose(f); }
    }
    return v > 0 ? (size_t)v : (size_t)8 << 20;     /* conservador */
}

/* le [p, p+len) de 64 em 64 bytes, com prefetch adiante; cancela (por pagina) se a geracao mudou. Devolve os bytes lidos */
static size_t pf_touch(const uint8_t *p, size_t len, unsigned gen) {
    size_t done = 0;
    unsigned s = 0;
    for (size_t o = 0; o < len; o += 4096) {
        if (__atomic_load_n(&pf.gen, __ATOMIC_RELAXED) != gen) break;
        const size_t e = o + 4096 < len ? o + 4096 : len;
        for (size_t i = o; i < e; i += 64) { s += p[i]; __builtin_prefetch(p + i + 512, 0, 2); }
        done = e;
    }
    pf.sink = s;
    return done;
}

static void pf_predict(const float *xn, int l, int lt, int *out, int *n) {
    const Model *m = pf.m; const Config *c = &m->cfg; const int d = pf.d, E = pf.E;
    const Layer *A = &m->layers[l], *B = &m->layers[lt];
    float x[8192], xr[8192], row[8192], lg[4096];
    *n = 0;
    if (!A->ffn_norm || !B->ffn_norm || !(B->router.f || B->router.q)) return;
    for (int i = 0; i < d; i++) { const float g = A->ffn_norm[i]; x[i] = (g > 1e-8f || g < -1e-8f) ? xn[i] / g : xn[i]; }
    rmsnorm(xr, x, B->ffn_norm, d, c->eps);
    for (int e = 0; e < E; e++) {
        if (B->router.f) lg[e] = dot(B->router.f + (size_t)e * d, xr, d);
        else { dequant(B->router.type, B->router.q + (size_t)e * B->router.stride, row, (size_t)d); lg[e] = dot(row, xr, d); }
    }
    const int N = pf.N < PF_MAXN ? pf.N : PF_MAXN;
    for (int k = 0; k < N; k++) {                                   /* os mais provaveis fora das vagas da GPU */
        int b = -1;
        for (int e = 0; e < E; e++) {
            if (gx.slot[(size_t)lt * E + e].slot >= 0) continue;
            int used = 0;
            for (int i = 0; i < k; i++) if (out[i] == e) { used = 1; break; }
            if (!used && (b < 0 || lg[e] > lg[b])) b = e;
        }
        if (b < 0) break;
        out[k] = b; (*n)++;
    }
}

static void *pf_thread(void *a) {
    (void)a;
    float *xn = xalloc((size_t)pf.d, 4);
    for (;;) {
        int l, n; unsigned gen;
        pthread_mutex_lock(&pf.mu);
        while (!pf.pend) pthread_cond_wait(&pf.cv, &pf.mu);
        pf.pend = 0; l = pf.jl; gen = pf.jgen; memcpy(xn, pf.jx, (size_t)pf.d * 4);
        pthread_mutex_unlock(&pf.mu);
        const int lt = l + 1;
        int list[PF_MAXN];
        pf_predict(xn, l, lt, list, &n);
        pthread_mutex_lock(&pf.mu);
        if (__atomic_load_n(&pf.gen, __ATOMIC_RELAXED) != gen) { pf.cancelled++; pthread_mutex_unlock(&pf.mu); continue; }
        pf.pl_layer = lt; pf.pl_n = n; memcpy(pf.pl, list, sizeof(int) * (size_t)n); pf.pl_started = 0; pf.pl_full = 0; pf.listed += (uint64_t)n;
        pthread_mutex_unlock(&pf.mu);
        const Layer *L = &pf.m->layers[lt];
        const int H = pf.m->cfg.hidden, d = pf.d;
        size_t spent = 0; int canceled = 0;
        for (int i = 0; i < n && !canceled; i++) {
            const int e = list[i];
            const Mat *ms[3] = { &L->eg[e], &L->eu[e], &L->ed[e] };
            const size_t rows[3] = { (size_t)H, (size_t)H, (size_t)d };
            size_t tot = 0;
            for (int r = 0; r < 3; r++) tot += ms[r]->q ? rows[r] * ms[r]->stride : 0;
            if (tot == 0 || spent + tot > pf.budget) break;          /* limite de volume (L3) */
            pf.pl_started = i + 1;
            for (int r = 0; r < 3 && !canceled; r++) {
                if (!ms[r]->q) continue;
                const size_t len = rows[r] * ms[r]->stride, got = pf_touch(ms[r]->q, len, gen);
                spent += got; pf.bytes += got;
                if (got < len) canceled = 1;
            }
            if (!canceled) pf.pl_full = i + 1;
        }
        if (canceled) { pthread_mutex_lock(&pf.mu); pf.cancelled++; pthread_mutex_unlock(&pf.mu); }
    }
    return NULL;
}

static void pf_report(void) {
    if (!pf.on || !(pf.stats || gpu.debug)) return;
    fprintf(stderr, "[moe_pf] %llu trabalhos (%llu cancelados antes de terminar), %llu previstos, %.1f MB lidos; "
            "dos %llu escolhidos reais, %llu estavam fora da VRAM: %llu (%.1f%%) ja previstos e lidos por inteiro, %llu (%.1f%%) ao menos iniciados; "
            "%llu sem previsao pronta\n",
            (unsigned long long)pf.jobs, (unsigned long long)pf.cancelled, (unsigned long long)pf.listed, pf.bytes / 1048576.0,
            (unsigned long long)pf.sel_total, (unsigned long long)pf.sel_cold,
            (unsigned long long)pf.hit_full, pf.sel_cold ? 100.0 * (double)pf.hit_full / (double)pf.sel_cold : 0.0,
            (unsigned long long)pf.hit_any, pf.sel_cold ? 100.0 * (double)pf.hit_any / (double)pf.sel_cold : 0.0,
            (unsigned long long)pf.no_pred);
}

static void pf_start(Model *m) {
    pf.init = 1;
    const char *s = getenv("KATANA_MOE_PF");
    const Config *c = &m->cfg;
    if ((s && atoi(s) == 0) || et_on || !c->n_expert || !gx.slot || c->dim > 8192 || c->n_expert > 4096) return;
    pf.m = m; pf.E = c->n_expert; pf.d = c->dim; pf.nl = c->n_layers;
    pf.N = (s = getenv("KATANA_MOE_PF_N")) ? atoi(s) : 12;
    if (pf.N < 1) return;
    pf.budget = (s = getenv("KATANA_MOE_PF_MB")) && atof(s) > 0 ? (size_t)(atof(s) * 1048576.0) : pf_l3_bytes() / 2;
    pf.stats = (s = getenv("KATANA_MOE_PF_STATS")) && atoi(s) != 0;
    pf.jx = xalloc((size_t)pf.d, 4); pf.res = xalloc((size_t)pf.E, 1);
    pf.pl_layer = -1;
    pthread_mutex_init(&pf.mu, NULL); pthread_cond_init(&pf.cv, NULL);
    pthread_t t;
    if (pthread_create(&t, NULL, pf_thread, NULL) != 0) return;
    pthread_detach(t);
    pf.on = 1;
    atexit(pf_report);
    fprintf(stderr, "[moe_pf] ligado: ate %d especialistas por camada, limite %.1f MB (L3 %.1f MB)\n", pf.N, pf.budget / 1048576.0, pf_l3_bytes() / 1048576.0);
}

/* a leitura da camada l voltou: cancela o trabalho em curso e guarda quem esta na VRAM (antes do MoE da camada) */
static void pf_before(Model *m, int l) {
    if (!pf.init) pf_start(m);
    if (!pf.on) return;
    __atomic_add_fetch(&pf.gen, 1u, __ATOMIC_RELAXED);
    const int E = pf.E;
    for (int e = 0; e < E; e++) pf.res[e] = gx.slot[(size_t)l * E + e].slot >= 0;
}

/* o MoE da camada l acabou (moe.sel valido): conta acertos e manda prever a camada l + 1 enquanto a GPU trabalha nela */
static void pf_after(Model *m, int l) {
    if (!pf.on) return;
    const int K = m->cfg.n_used;
    pthread_mutex_lock(&pf.mu);
    if (pf.stats || gpu.debug) {
        const int have = pf.pl_layer == l;
        for (int k = 0; k < K; k++) {
            const int e = moe.sel[k];
            pf.sel_total++;
            if (pf.res[e]) continue;
            pf.sel_cold++;
            if (!have) { pf.no_pred++; continue; }
            for (int i = 0; i < pf.pl_n; i++) if (pf.pl[i] == e) {
                if (i < pf.pl_started) pf.hit_any++;
                if (i < pf.pl_full) pf.hit_full++;
                break;
            }
        }
    }
    pf.pl_layer = -1;
    if (l + 1 < pf.nl && m->layers[l + 1].eg) {
        memcpy(pf.jx, gx.xn, (size_t)pf.d * 4);
        pf.jl = l; pf.jgen = __atomic_add_fetch(&pf.gen, 1u, __ATOMIC_RELAXED); pf.pend = 1; pf.jobs++;
        pthread_cond_signal(&pf.cv);
    }
    pthread_mutex_unlock(&pf.mu);
}
#else
static void pf_before(Model *m, int l) { (void)m; (void)l; }
static void pf_after(Model *m, int l) { (void)m; (void)l; }
#endif
