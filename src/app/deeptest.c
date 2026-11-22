/* ---------- katana selftest <modelo> --deep ----------
 * Conferencias que o selftest comum nao faz, sobre texto de verdade:
 *   arquivo    tensores do GGUF que ninguem le
 *   caminhos   cada caminho (inteiro/exato, token a token/lote, KV fp16/Q8_0, GPU) contra a CPU em fp32 token a token:
 *              KL da distribuicao, trocas de argmax com a margem da referencia, erro por camada, escolha de especialistas
 *   estado     gravar, estragar e restaurar o estado (cache KV + DeltaNet) e voltar atras no cache
 *   longo      contexto longo: o fim em lote contra o fim token a token a partir do mesmo estado
 *   valores    nao finitos nos estados e logits, pesos do roteador que nao somam 1
 * Opcoes: --tokens N, --text arquivo, --long N, --full-long, --long-cpu, --save-ref arquivo, --ref arquivo, --no-gpu, --only nomes */

/* sob -ffast-math isnan() e sempre falso: olha os bits */
static int dt_bad(const float *x, size_t n) {
    for (size_t i = 0; i < n; i++) { uint32_t u; memcpy(&u, &x[i], 4); if ((u & 0x7f800000u) == 0x7f800000u) return 1; }
    return 0;
}

static const char DT_TEXT[] =
    "O motor processa o texto em lotes e depois gera um token por vez. A pergunta e simples: o resultado muda quando o caminho muda? "
    "For every layer the hidden state is compared with the plain floating point path, token by token, and the largest relative error is printed.\n\n"
    "```c\nstatic int sum(const int *v, int n) {\n    int s = 0;\n    for (int i = 0; i < n; i++) s += v[i];\n    return s;\n}\n```\n\n"
    "The quick brown fox jumps over the lazy dog. Em 1969 o homem chegou a Lua; a missao Apollo 11 levou tres astronautas. "
    "{\"name\": \"Read\", \"arguments\": {\"file_path\": \"/tmp/notes.txt\", \"offset\": 120, \"limit\": 40}}\n"
    "Edit the file, run the tests and report what changed. Se o teste falhar, leia o erro, corrija a causa e rode de novo. "
    "2 + 2 = 4, 17 * 23 = 391, and the square root of 144 is 12. Paris is the capital of France and Brasilia is the capital of Brazil.\n"
    "def fib(n):\n    a, b = 0, 1\n    for _ in range(n):\n        a, b = b, a + b\n    return a\n";

typedef struct { int n, flips, bigflips, bad; double kl, klmax, dmax; int tie, tie_l; double tie_m; int ndiv; } DtStat;
static struct {
    int L, d, N, K;
    float *ref; uint8_t *have;          /* estado por (token, camada) da referencia */
    int rec, cmp, bad, pos0;
    double *err; long *cnt;             /* pior erro relativo por camada */
    uint64_t *sel; float *mar; long nsel, kr;   /* 1 a 1: assinatura dos escolhidos e margem (K-esimo - seguinte, relativa) por chamada do roteador */
    int s1, ndiv; long div0;            /* comparando; tokens com escolha diferente; primeira chamada que divergiu */
    double wsum;                        /* pior |soma dos pesos - 1| do roteador */
    int dup, klpos;                     /* escolhidos repetidos; posicao do maior KL */
    int calls; double secs, disp, subm, hit, miss, load;     /* por chamada do caminho: despachos, envios a placa, usos de especialista na VRAM e fora, subidas */
} dt;
static void dt_tap(int l, int pos, const float *x) {
    if (dt_bad(x, dt.d)) dt.bad++;
    const int t = pos - dt.pos0;
    if (t < 0 || t >= dt.N || !dt.ref) return;
    float *r = dt.ref + ((size_t)t * dt.L + l) * dt.d;
    if (dt.rec) { memcpy(r, x, (size_t)dt.d * 4); dt.have[(size_t)t * dt.L + l] = 1; return; }
    if (!dt.cmp || !dt.have[(size_t)t * dt.L + l]) return;
    double e = 0, n = 0;
    for (int i = 0; i < dt.d; i++) { const double f = (double)x[i] - r[i]; e += f * f; n += (double)r[i] * r[i]; }
    e = sqrt(e / (n + 1e-30));
    if (e > dt.err[l]) dt.err[l] = e;
    dt.cnt[l]++;
}
static int moe_E;
static void dt_route(int l, const int *sel, const float *w, int K, int unit) {
    double s = 0;
    for (int k = 0; k < K; k++) { s += w[k]; for (int j = 0; j < k; j++) if (sel[j] == sel[k]) dt.dup++; }
    if (unit && fabs(s - 1.0) > dt.wsum) dt.wsum = fabs(s - 1.0);
    (void)l;
    if (!dt.s1 || dt.kr >= dt.nsel) return;
    uint64_t sg = 0;
    for (int k = 0; k < K; k++) sg += (uint64_t)(sel[k] + 1) * 0x9E3779B97F4A7C15ULL ^ ((uint64_t)(sel[k] + 1) << 40);
    if (dt.rec) {
        float lo = 1e30f, up = 0;
        for (int k = 0; k < K; k++) if (moe.lg[sel[k]] < lo) lo = moe.lg[sel[k]];
        for (int e = 0; e < moe_E; e++) { int in = 0; for (int k = 0; k < K; k++) in |= sel[k] == e; if (!in && moe.lg[e] > up) up = moe.lg[e]; }
        dt.sel[dt.kr] = sg; dt.mar[dt.kr] = (lo - up) / (lo + 1e-30f);
    } else if (dt.sel[dt.kr] != sg) { dt.ndiv++; if (dt.div0 < 0) dt.div0 = dt.kr; }
    dt.kr++;
}
static void dt_clear_err(void) { memset(dt.err, 0, (size_t)dt.L * sizeof(double)); memset(dt.cnt, 0, (size_t)dt.L * sizeof(long)); }

static void dt_add(DtStat *s, const float *ref, const float *o, int V) {
    if (dt_bad(o, V)) { s->bad++; s->n++; return; }
    int a = 0, b = 0; double md = 0;
    for (int i = 0; i < V; i++) {
        if (ref[i] > ref[a]) a = i;
        if (o[i] > o[b]) b = i;
        const double f = fabs((double)ref[i] - o[i]);
        if (f > md) md = f;
    }
    double zr = 0, zo = 0, kl = 0;
    for (int i = 0; i < V; i++) { zr += exp((double)ref[i] - ref[a]); zo += exp((double)o[i] - o[b]); }
    const double lzr = log(zr) + ref[a], lzo = log(zo) + o[b];
    for (int i = 0; i < V; i++) { const double lp = ref[i] - lzr, p = exp(lp); if (p > 1e-12) kl += p * (lp - (o[i] - lzo)); }
    if (kl < 0) kl = 0;
    s->n++; s->kl += kl;
    if (kl > s->klmax) { s->klmax = kl; dt.klpos = s->n - 1; }
    if (md > s->dmax) s->dmax = md;
    if (a != b) { s->flips++; if (ref[a] - ref[b] > 2 * 0.25) s->bigflips++; }
}

static size_t dt_kv_floats(const Model *m) {
    const Config *c = &m->cfg;
    size_t tot = 0;
    if (c->g4) { for (int l = 0; l < c->n_layers; l++) tot += (size_t)m->layers[l].kv_cap * m->layers[l].nkv_l * m->layers[l].hd_l; return tot; }
    size_t na = 0;
    for (int l = 0; l < c->n_layers; l++) na += !m->layers[l].lin;
    return (na ? na : 1) * c->ctx * (size_t)c->n_kv * c->hd;
}

/* roda os N tokens e compara; chunk 1 = token a token; senao forward_seq em pedacos e compara no fim de cada um */
static void dt_run(Model *m, const int *toks, int N, int chunk, const float *ref, DtStat *s) {
    const int V = m->cfg.vocab;
    memset(s, 0, sizeof *s);
    dt_clear_err();
    dt.bad = 0; dt.cmp = 1;
    dt.s1 = chunk == 1 && dt.sel && dt.nsel; dt.kr = 0; dt.ndiv = 0; dt.div0 = -1;
    KvkStats k0; memset(&k0, 0, sizeof k0);
    if (gpu.vk) kvk_stats(&k0);
    const uint64_t h0 = gx.hits, m0 = gx.miss, l0 = gx.loads;
    const double t0 = now_sec();
    for (int i = 0; i < N; ) {
        const int nb = chunk > 1 ? (N - i < chunk ? N - i : chunk) : 1;
        const float *o = chunk > 1 ? forward_seq(m, toks + i, nb, i, 1) : forward(m, toks[i], i, 1);
        i += nb;
        dt_add(s, ref + (size_t)(i - 1) * V, o, V);
    }
    dt.cmp = 0;
    s->bad += dt.bad;
    s->ndiv = dt.s1 ? dt.ndiv : -1;
    if (dt.s1 && dt.div0 >= 0) { s->tie = (int)(dt.div0 / dt.L); s->tie_l = (int)(dt.div0 % dt.L); s->tie_m = dt.mar[dt.div0]; }
    dt.s1 = 0;
    dt.calls = chunk > 1 ? (N + chunk - 1) / chunk : N;
    dt.secs = now_sec() - t0;
    dt.disp = dt.subm = 0;
    if (gpu.vk) { KvkStats k1; kvk_stats(&k1); dt.disp = (double)(k1.dispatches - k0.dispatches) / dt.calls; dt.subm = (double)(k1.submits - k0.submits) / dt.calls; }
    dt.hit = (double)(gx.hits - h0) / dt.calls; dt.miss = (double)(gx.miss - m0) / dt.calls; dt.load = (double)(gx.loads - l0) / dt.calls;
}

static int dt_print(const char *name, const DtStat *s) {
    double w = 0, mean = 0; int wl = -1, nl = 0;
    for (int l = 0; l < dt.L; l++) if (dt.cnt[l]) { mean += dt.err[l]; nl++; if (dt.err[l] > w) { w = dt.err[l]; wl = l; } }
    const int ok = !s->bad && s->kl / (s->n ? s->n : 1) < 5e-3 && !s->bigflips;
    char lay[64];
    if (nl) snprintf(lay, sizeof lay, "%.1e (L%d), media %.1e", w, wl, mean / nl); else snprintf(lay, sizeof lay, "-");
    printf("  %-26s %4d  %9.2e %9.2e  %3d (%d)  %8.4f  %-28s %s%s\n", name, s->n, s->kl / (s->n ? s->n : 1), s->klmax, s->flips, s->bigflips, s->dmax, lay,
           ok ? "OK" : "FALHOU", s->bad ? " (valores nao finitos)" : "");
    if (s->n && s->ndiv > 0) printf("    roteador: %d escolhas diferentes das da referencia; a primeira no token %d, camada %d, onde a margem relativa da referencia era %.1e; KL maximo na posicao %d\n", s->ndiv, s->tie, s->tie_l, s->tie_m, s->bad ? -1 : dt.klpos);
    if (getenv("KATANA_DEEP_LAYERS")) { printf("    por camada:"); for (int l = 0; l < dt.L; l++) if (dt.cnt[l]) printf(" %d:%.1e", l, dt.err[l]); printf("\n"); }
    fflush(stdout);
    return ok;
}

static int dt_state(Model *m, const int *toks, int N, const char *what, int batch) {
    const Config *c = &m->cfg;
    const int V = c->vocab, n0 = N / 2, n1 = N - n0 < 8 ? N - n0 : 8;
    int ngl, esz, backend, fails = 0;
    if (!pfx_geom(m, &ngl, &esz, &backend)) { printf("  %-26s nao coberto por este motor (pfx_geom = 0)\n", what); return 0; }
    float *a = xalloc((size_t)V * n1, 4), *b = xalloc(V, 4);
    if (batch) forward_seq(m, toks, n0, 0, 0); else for (int i = 0; i < n0; i++) forward(m, toks[i], i, 0);
    const uint64_t key = 0x4445455000000000ULL ^ (uint64_t)getpid();
    const int sv = pfx_save(m, key, n0, ngl, esz, backend);
    for (int i = 0; i < n1; i++) memcpy(a + (size_t)i * V, forward(m, toks[n0 + i], n0 + i, 1), (size_t)V * 4);
    for (int i = 0; i < n0 + 5 && i + 1 < c->ctx; i++) forward(m, toks[(i * 7 + 3) % N], i, 0);      /* estraga: outra conversa desde a posicao 0 */
    const int ld = sv && pfx_load(m, key, n0, ngl, esz, backend);
    double md = 0; int flips = 0, bits = 1;
    for (int i = 0; i < n1 && ld; i++) {
        memcpy(b, forward(m, toks[n0 + i], n0 + i, 1), (size_t)V * 4);
        const float *r = a + (size_t)i * V;
        if (memcmp(b, r, (size_t)V * 4)) bits = 0;
        int x = 0, y = 0;
        for (int v = 0; v < V; v++) { if (r[v] > r[x]) x = v; if (b[v] > b[y]) y = v; if (fabs((double)r[v] - b[v]) > md) md = fabs((double)r[v] - b[v]); }
        flips += x != y;
    }
    char p[1000]; pfx_path(p, sizeof p, key, 0); remove(p);
    const int ok = sv && ld && (!gpu.force_cpu ? md < 0.05 && !flips : bits);
    printf("  %-26s %s gravado %s, restaurado %s; %d tokens depois: maior diferenca %.2e, trocas %d%s: %s\n", what, backend == 0 ? "CPU" : backend == 3 ? "VRAM+RAM" : "VRAM",
           sv ? "sim" : "NAO", ld ? "sim" : "NAO", n1, md, flips, bits ? ", bit a bit" : "", ok ? "OK" : "FALHOU");
    if (!ok) fails++;
    free(a); free(b);
    return fails;
}

/* voltar atras no cache: conversa de n tokens, o pedido novo repete os primeiros `common`; o que cache_rewind permite tem de dar igual ao calculo do zero */
static int dt_rewind(Model *m, const int *toks, int N) {
    const Config *c = &m->cfg;
    const int V = c->vocab;
    int fails = 0;
    float *a = xalloc(V, 4);
    const int cut[4] = { N / 8 > 1 ? N / 8 : 1, N / 3, N - 8 > 1 ? N - 8 : 1, N - 1 };
    for (int k = 0; k < 4; k++) {
        const int cm = cut[k], nn = cm + 4;
        int nt[4];
        for (int i = 0; i < 4; i++) nt[i] = toks[(i * 11 + 5) % N];
        forward_seq(m, toks, cm, 0, 0);
        memcpy(a, forward_seq(m, nt, 4, cm, 1), (size_t)V * 4);
        forward_seq(m, toks, N, 0, 0);
        const int use = cache_rewind(m, N, cm, nn);
        const float *o;
        if (use) o = forward_seq(m, nt, 4, use, 1);
        else { forward_seq(m, toks, cm, 0, 0); o = forward_seq(m, nt, 4, cm, 1); }
        double md = 0;
        for (int v = 0; v < V; v++) if (fabs((double)a[v] - o[v]) > md) md = fabs((double)a[v] - o[v]);
        const int ok = md < (gpu.force_cpu ? 2e-3 : 0.05);
        printf("  volta de %d para %d tokens: aproveita %d, maior diferenca %.2e: %s\n", N, cm, use, md, ok ? "OK" : "FALHOU");
        fails += !ok;
    }
    free(a);
    return fails;
}

static int dt_long(Model *m, const int *toks, int N, int len, int full, int cpu) {
    const Config *c = &m->cfg;
    const int V = c->vocab, tail = 24;
    if (len + tail + 1 >= c->ctx) { printf("  contexto longo: KATANA_CTX=%d nao cabe %d tokens\n", c->ctx, len + tail); return 1; }
    int ngl, esz, backend, fails = 0;
    const int geo = pfx_geom(m, &ngl, &esz, &backend);
    int *lt = xalloc((size_t)len + tail, sizeof(int));
    for (int i = 0; i < len + tail; i++) lt[i] = toks[(i + i / N * 13) % N];
    float *a = xalloc((size_t)V * 2, 4), *b = a + V;
    double t0 = now_sec();
    forward_seq(m, lt, len, 0, 0);
    printf("  %d tokens em lote: %.1f s (maquina carregada)\n", len, now_sec() - t0);
    const uint64_t key = 0x4c4f4e4700000000ULL ^ (uint64_t)getpid();
    const int sv = geo && pfx_save(m, key, len, ngl, esz, backend);
    memcpy(a, forward_seq(m, lt + len, tail, len, 1), (size_t)V * 4);
    if (sv && pfx_load(m, key, len, ngl, esz, backend)) {
        const float *o = NULL;
        for (int i = 0; i < tail; i++) o = forward(m, lt[len + i], len + i, i == tail - 1);
        memcpy(b, o, (size_t)V * 4);
        DtStat s = {0};
        dt_clear_err();
        dt_add(&s, a, b, V);
        fails += !dt_print("longo: fim lote x 1 a 1", &s);
    } else printf("  longo: estado nao pode ser guardado neste motor (fim token a token nao comparado)\n");
    char p[1000]; pfx_path(p, sizeof p, key, 0); remove(p);
    if (full) {     /* tudo token a token (modelo pequeno) */
        const float *o = NULL;
        for (int i = 0; i < len + tail; i++) o = forward(m, lt[i], i, i == len + tail - 1);
        memcpy(b, o, (size_t)V * 4);
        DtStat s = {0};
        dt_clear_err();
        dt_add(&s, a, b, V);
        fails += !dt_print("longo: tudo lote x 1 a 1", &s);
    }
    if (cpu && !gpu.force_cpu) {     /* a mesma conversa inteira na CPU: deriva acumulada do estado na GPU */
        gpu.force_cpu = 1;
        t0 = now_sec();
        forward_seq(m, lt, len, 0, 0);
        memcpy(b, forward_seq(m, lt + len, tail, len, 1), (size_t)V * 4);
        gpu.force_cpu = 0;
        printf("  %d tokens em lote na CPU: %.1f s (maquina carregada)\n", len + tail, now_sec() - t0);
        DtStat s = {0};
        dt_clear_err();
        dt_add(&s, b, a, V);
        fails += !dt_print("longo: CPU x GPU, lote", &s);
    }
    free(lt); free(a);
    return fails;
}

static int dt_want(const char *only, const char *name) {
    if (!only) return 1;
    const size_t l = strlen(name);
    for (const char *p = only; (p = strstr(p, name)); p += l) if ((p == only || p[-1] == ',') && (p[l] == 0 || p[l] == ',')) return 1;
    return 0;
}

static int selftest_deep(Model *m, int argc, char **argv) {
    const Config *c = &m->cfg;
    const int V = c->vocab;
    int N = 48, llen = 0, full = 0, lcpu = 0, nogpu = 0, fails = 0;
    const char *text = NULL, *sref = NULL, *lref = NULL, *only = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--tokens") && i + 1 < argc) N = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--text") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(argv[i], "--long") && i + 1 < argc) llen = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--full-long")) full = 1;
        else if (!strcmp(argv[i], "--long-cpu")) lcpu = 1;
        else if (!strcmp(argv[i], "--save-ref") && i + 1 < argc) sref = argv[++i];
        else if (!strcmp(argv[i], "--ref") && i + 1 < argc) lref = argv[++i];
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--no-gpu")) nogpu = 1;
        else { fprintf(stderr, "selftest --deep: opcao desconhecida: %s\n", argv[i]); return 2; }
    }
    printf("== arquivo\n");
    long un = 0; long long ub = 0;
    for (int i = 0; i < g_unread.n; i++) { un++; ub += (long long)g_unread.bytes[i]; if (i < 40) printf("  tensor sem leitor: %s (%llu bytes)\n", g_unread.name[i], (unsigned long long)g_unread.bytes[i]); }
    printf("  %ld tensores sem leitor, %lld bytes de %lld\n", un, ub, (long long)m->fsize);
    Ids ids = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    encode(&m->tk, text ? read_text(text) : DT_TEXT, &ids);
    if (N > ids.n) N = ids.n;
    if (N + 16 >= c->ctx) N = c->ctx - 17;
    if (N < 16) { printf("texto curto demais\n"); return 2; }
    char tdir[] = "/tmp/katana-deep-XXXXXX";
    if (!getenv("KATANA_CACHE_DIR") && mkdtemp(tdir)) setenv_("KATANA_CACHE_DIR", tdir);
    const int gpu_on = !nogpu && (vkg.on || gpu.mv_host || gpu.n_gl > 0);
    dt.L = c->n_layers; dt.d = c->dim; dt.N = N; dt.K = c->n_used;
    dt.err = xalloc(dt.L, sizeof(double)); dt.cnt = xalloc(dt.L, sizeof(long));
    float *ref = xalloc((size_t)N * V, 4);
    layer_tap = dt_tap; if (c->n_expert) route_tap = dt_route;
    const int pool_only_route = only && !strcmp(only, "none");
    if (!pool_only_route) {
        /* referencia: CPU, produtos em float, token a token, cache fp32 */
        gpu.force_cpu = 1;
        const double t0 = now_sec();
        if (lref) {
            FILE *f = fopen(lref, "rb");
            int h[2] = {0};
            if (!f || fread(h, 4, 2, f) != 2 || h[0] != V || h[1] < N || fread(ref, 4, (size_t)N * V, f) != (size_t)N * V) { printf("referencia %s nao serve\n", lref); return 2; }
            fclose(f);
        } else {
            uint16_t *kh = m->kh, *vh = m->vh; uint8_t *kq = m->kq8, *vq = m->vq8; float *kf = m->kcache, *vf = m->vcache;
            const size_t nk = dt_kv_floats(m);
            float *k2 = NULL, *v2 = NULL;
            if (!kf) { k2 = xalloc(nk, 4); v2 = xalloc(nk, 4); m->kcache = k2; m->vcache = v2; m->kh = m->vh = NULL; m->kq8 = m->vq8 = NULL; }
            dt.ref = xalloc((size_t)N * dt.L * dt.d, 4); dt.have = xalloc((size_t)N * dt.L, 1);
            if (c->n_expert) { dt.nsel = (long)N * dt.L; dt.sel = xalloc(dt.nsel, sizeof(uint64_t)); dt.mar = xalloc(dt.nsel, 4); moe_E = c->n_expert; dt.s1 = 1; dt.kr = 0; }
            cpu_exact = 1; dt.rec = 1;
            for (int i = 0; i < N; i++) { memcpy(ref + (size_t)i * V, forward(m, ids.ids[i], i, 1), (size_t)V * 4); }
            cpu_exact = 0; dt.rec = 0; dt.s1 = 0;
            if (dt.nsel) {      /* quao perto de empate o roteador decide */
                long n4 = 0, n3 = 0; float mn = 1e30f;
                for (long i = 0; i < dt.kr; i++) { n4 += dt.mar[i] < 1e-4f; n3 += dt.mar[i] < 1e-3f; if (dt.mar[i] < mn) mn = dt.mar[i]; }
                printf("  roteador da referencia: %ld escolhas, %ld com margem < 1e-3 e %ld < 1e-4 (menor %.1e)\n", dt.kr, n3, n4, mn);
            }
            if (!kf) { free(k2); free(v2); m->kcache = NULL; m->vcache = NULL; m->kh = kh; m->vh = vh; m->kq8 = kq; m->vq8 = vq; }
        }
        printf("== caminhos: %d tokens de texto contra a CPU em fp32 token a token (%.1f s, maquina carregada)\n", N, now_sec() - t0);
        if (dt_bad(ref, (size_t)N * V) || dt.bad) { printf("  referencia com valores nao finitos: FALHOU\n"); fails++; }
        if (sref) { FILE *f = fopen(sref, "wb"); const int h[2] = { V, N }; if (f) { fwrite(h, 4, 2, f); fwrite(ref, 4, (size_t)N * V, f); fclose(f); } }
        printf("  %-26s pos.  KL medio  KL max     trocas    max|dl|   erro por camada (pior)\n", "caminho");
        DtStat s;
#define DT_CASE(nm, label, prep, chunk, undo) if (dt_want(only, nm)) { prep; dt_run(m, ids.ids, N, chunk, ref, &s); undo; fails += !dt_print(label, &s); }
#define DT_CASE_GPU(nm, label, prep, chunk, undo) if (dt_want(only, nm)) { prep; dt_run(m, ids.ids, N, chunk, ref, &s); undo; fails += !dt_print(label, &s); \
            printf("    por chamada (%d): %.0f despachos, %.0f envios, especialistas %.0f na VRAM e %.0f fora, %.1f subidas, %.1f ms (maquina carregada)\n", \
                   dt.calls, dt.disp, dt.subm, dt.hit, dt.miss, dt.load, 1e3 * dt.secs / dt.calls); }
        DT_CASE("exact16", "CPU fp32, lote 16", cpu_exact = 1, 16, cpu_exact = 0)
        DT_CASE("int1", "CPU inteira, 1 a 1", , 1, )
        DT_CASE("int16", "CPU inteira, lote 16", , 16, )
        DT_CASE("int5", "CPU inteira, lote 5", , 5, )
        if (!m->kcache || c->g4 || c->ssm_inner) {
            uint16_t *kh = m->kh, *vh = m->vh; uint8_t *kq = m->kq8, *vq = m->vq8; float *kf = m->kcache, *vf = m->vcache;
            const size_t nk = dt_kv_floats(m);
            if ((c->g4 || c->ssm_inner) && dt_want(only, "kv16")) {
                m->kcache = m->vcache = NULL; m->kq8 = m->vq8 = NULL;
                m->kh = xalloc(nk, 2); m->vh = xalloc(nk, 2);
                dt_run(m, ids.ids, N, 1, ref, &s); fails += !dt_print("CPU inteira, KV fp16", &s);
                free(m->kh); free(m->vh);
            }
            if (c->ssm_inner && c->hd % 32 == 0 && dt_want(only, "kv8")) {
                m->kcache = m->vcache = NULL; m->kh = m->vh = NULL;
                m->kq8 = xalloc(nk / 32, 34); m->vq8 = xalloc(nk / 32, 34);
                dt_run(m, ids.ids, N, 1, ref, &s); fails += !dt_print("CPU inteira, KV Q8_0", &s);
                free(m->kq8); free(m->vq8);
            }
            m->kh = kh; m->vh = vh; m->kq8 = kq; m->vq8 = vq; m->kcache = kf; m->vcache = vf;
        }
        gpu.force_cpu = 0;
        if (gpu_on) {
            DT_CASE_GPU("gpu1", "GPU, 1 a 1", , 1, )
            DT_CASE_GPU("gpu1b", "GPU, 1 a 1 (de novo)", , 1, )
            DT_CASE_GPU("gpu16", "GPU, lote 16", , 16, )
            DT_CASE_GPU("gpu5", "GPU, lote 5", , 5, )
            DT_CASE_GPU("gpuN", "GPU, lote inteiro", , N, )
            DT_CASE_GPU("gpu1e", "GPU 1 a 1, frios em fp32", cpu_exact = 1, 1, cpu_exact = 0)
        } else printf("  (sem GPU nesta execucao)\n");
#undef DT_CASE
    }
    free(dt.ref); dt.ref = NULL;
    const int SN = ids.n < 64 ? ids.n : 64;
    if (dt_want(only, "state")) {
        printf("== estado\n");
        gpu.force_cpu = 1; fails += dt_state(m, ids.ids, SN, "CPU 1 a 1", 0); fails += dt_state(m, ids.ids, SN, "CPU lote", 1); gpu.force_cpu = 0;
        if (gpu_on) fails += dt_state(m, ids.ids, SN, "GPU lote", 1);
    }
    if (dt_want(only, "rewind")) {
        printf("== volta no cache (reuso de prefixo do servidor)\n");
        gpu.force_cpu = 1;      /* na GPU a mistura de especialistas quentes e frios muda entre duas passadas */
        fails += dt_rewind(m, ids.ids, ids.n < 400 ? ids.n : 400);
        gpu.force_cpu = 0;
    }
    if (llen) {
        printf("== contexto longo\n");
        gpu.force_cpu = !gpu_on;
        fails += dt_long(m, ids.ids, ids.n, llen, full, lcpu);
        gpu.force_cpu = 0;
    }
    printf("== valores\n");
    if (c->n_expert) printf("  roteador: pior |soma dos pesos - 1| %.2e, escolhidos repetidos %d: %s\n", dt.wsum, dt.dup, dt.wsum < 1e-4 && !dt.dup ? "OK" : "FALHOU");
    if (c->n_expert && (dt.wsum >= 1e-4 || dt.dup)) fails++;
    printf("  nao finitos depois das camadas: %d\n", dt.bad);
    layer_tap = NULL; route_tap = NULL;
    printf("selftest --deep: %s\n", fails ? "FALHOU" : "OK");
    if (!strcmp(getenv("KATANA_CACHE_DIR") ? getenv("KATANA_CACHE_DIR") : "", tdir)) rmdir(tdir);
    free(ref); free(ids.ids);
    return fails ? 1 : 0;
}
