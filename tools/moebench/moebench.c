/* microbancada do caminho de CPU dos especialistas MoE na geracao (um token, K especialistas).
   Sem GPU: carrega o modelo (mapeado), sorteia K especialistas distintos por camada e chama
   moe_cpu_multi em todas as camadas MoE, como num token. Tambem mede a banda de leitura da RAM.
   Compilar: tools/moebench/build.sh   Uso: tools/moebench/moebench <modelo.gguf> [tokens] [reps]
   MOEB_DUMP=arquivo grava a saida de uma camada fixa (para comparar versoes). */
#define main katana_main_
#include "../../src/katana.c"
#undef main

static volatile long long bw_sink;
typedef struct { const uint8_t *p; size_t n; } BwJob;
static void bw_job(void *c, int i0, int i1) {
    const BwJob *b = c;
    __m256i s = _mm256_setzero_si256();
    for (int i = i0; i < i1; i++) {
        const __m256i *v = (const __m256i *)(b->p + (size_t)i * 4096);
        for (int k = 0; k < 128; k++) s = _mm256_add_epi64(s, _mm256_sad_epu8(_mm256_load_si256(v + k), _mm256_setzero_si256()));
    }
    bw_sink += _mm256_extract_epi64(s, 0);
}

/* MOEB_SYNTH=1: corretude sem modelo. Especialistas Q4_K/Q6_K sinteticos (bytes aleatorios, escalas validas) e
   compara o caminho novo (KATANA_MOE_*: prefetch, 4 linhas, SiLU vetorizado, fatias alinhadas) com o anterior,
   por moe_cpu_multi (1 token e lote agrupado) e por moe_rows com contagens que nao sao multiplo de 4. */
static void synth_mat(Mat *w, int rows, int cols, uint32_t type) {
    const int bb = type == T_Q4_K ? 144 : 210, nb = cols / 256;
    w->type = type; w->stride = (size_t)nb * bb;
    uint8_t *q = xalloc((size_t)rows * w->stride, 1);
    for (size_t i = 0; i < (size_t)rows * w->stride; i++) q[i] = (uint8_t)rand();
    for (int r = 0; r < rows; r++) for (int b = 0; b < nb; b++) {
        uint8_t *blk = q + (size_t)r * w->stride + (size_t)b * bb;
        const uint16_t d = 0x2800 + (rand() & 0x1ff), dm = 0x2400 + (rand() & 0x1ff);     /* f16 pequenos e positivos */
        if (type == T_Q4_K) { memcpy(blk, &d, 2); memcpy(blk + 2, &dm, 2); }
        else memcpy(blk + 208, &d, 2);
    }
    w->q = q;
}
static int synth_check(void) {
    int bad = 0;
    srand(5);
    moe_opt_init();
    const int d = 512, H = 256, E = 16, K = 8, B = 4;
    Model m; memset(&m, 0, sizeof m);
    m.cfg.dim = d; m.cfg.hidden = H; m.cfg.n_expert = E; m.cfg.n_used = K;
    for (int ty = 0; ty < 2; ty++) {
        Layer L; memset(&L, 0, sizeof L);
        L.eg = xalloc(E, sizeof(Mat)); L.eu = xalloc(E, sizeof(Mat)); L.ed = xalloc(E, sizeof(Mat));
        memset(L.eg, 0, E * sizeof(Mat)); memset(L.eu, 0, E * sizeof(Mat)); memset(L.ed, 0, E * sizeof(Mat));
        for (int e = 0; e < E; e++) {
            synth_mat(&L.eg[e], H, d, T_Q4_K); synth_mat(&L.eu[e], H, d, T_Q4_K); synth_mat(&L.ed[e], d, H, ty ? T_Q4_K : T_Q6_K);
        }
        float *xs = xalloc((size_t)B * d, sizeof(float)), *o0 = xalloc((size_t)B * K * d, sizeof(float)), *o1 = xalloc((size_t)B * K * d, sizeof(float));
        int ex[64], xi[64];
        for (int mode = 0; mode < 2; mode++) for (int rep = 0; rep < 3; rep++) {
            for (int i = 0; i < B * d; i++) xs[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f + (i % 11 == 0 ? 3.0f : 0.0f);
            const int n = mode ? B * K : K, nx = mode ? B : 1;
            for (int j = 0; j < n; j++) { xi[j] = mode ? j / K : 0; ex[j] = mode ? rand() % 5 : j * 2 % E; }
            moe_opt_pf = moe_opt_rows = moe_opt_act = moe_opt_align = 0;
            moe_cpu_multi(&m, &L, ex, xi, n, nx, xs, o0);
            moe_opt_pf = moe_opt_rows = moe_opt_act = moe_opt_align = 1;
            moe_cpu_multi(&m, &L, ex, xi, n, nx, xs, o1);
            double mx = 0, md = 0;
            for (int i = 0; i < n * d; i++) { mx = fmax(mx, fabs(o0[i])); md = fmax(md, fabs((double)o0[i] - o1[i])); }
            const double rel = md / (mx > 0 ? mx : 1);
            fprintf(stderr, "synth down=%s modo %d: max|dif|=%.3g max|saida|=%.3g rel=%.3g\n", ty ? "Q4_K" : "Q6_K", mode, md, mx, rel);
            if (!(rel < 1e-4)) bad = 1;
        }
        /* moe_rows direto: tamanhos 1..9 e inicios nao alinhados, contra moe_dot linha a linha */
        for (int wi = 0; wi < 2; wi++) {
            const Mat *W = wi ? &L.ed[3] : &L.eg[3];
            const int cols = wi ? H : d, rows = wi ? d : H;
            xq_tls = &xq0; xq_prepare(xs, cols);
            moe_opt_pf = moe_opt_rows = 1;
            for (int n = 1; n <= 9; n++) {
                float a[16], b2[16];
                const size_t r0 = (size_t)(n * 5) % (rows - 9);
                moe_rows(W, r0, n, cols, (size_t)rows, a);
                moe_opt_rows = 0;
                moe_rows(W, r0, n, cols, (size_t)rows, b2);
                moe_opt_rows = 1;
                for (int i = 0; i < n; i++) if (fabsf(a[i] - b2[i]) > 1e-4f * (fabsf(b2[i]) + 1.0f)) { bad = 1; fprintf(stderr, "moe_rows n=%d i=%d: %g x %g\n", n, i, a[i], b2[i]); }
            }
            xq_tls = NULL;
        }
        /* SiLU vetorizado contra a formula escalar */
        float g[1003], u[1003], g0[1003]; double w = 0;
        for (int i = 0; i < 1003; i++) { g[i] = ((float)rand() / RAND_MAX - 0.5f) * 40.0f; u[i] = (float)rand() / RAND_MAX; g0[i] = g[i] / (1.0f + expf(-g[i])) * u[i]; }
        silu_mul_v(g, u, 1003);
        for (int i = 0; i < 1003; i++) w = fmax(w, fabs(g[i] - g0[i]) / (fabs(g0[i]) + 1e-6));
        fprintf(stderr, "silu: pior erro relativo %.3g\n", w);
        if (w > 1e-5) bad = 1;
    }
    fprintf(stderr, "moebench synth: %s\n", bad ? "FALHOU" : "OK");
    return bad;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: moebench <modelo.gguf> [tokens] [reps]\n"); return 1; }
    setenv("KATANA_CPU", "1", 1);
    pool_init(); vnni_init();
    const int T = argc > 2 ? atoi(argv[2]) : 32, R = argc > 3 ? atoi(argv[3]) : 3;
    if (!getenv("MOEB_NOBW")) {     /* banda: 1 GiB lido em blocos de 4 KiB por todas as threads */
        const size_t N = (size_t)1 << 30;
        uint8_t *p = aligned_alloc(4096, N); memset(p, 1, N);
        BwJob bj = { p, N };
        for (int r = 0; r < 3; r++) {
            const double t0 = now_sec();
            pool_for(bw_job, &bj, (int)(N / 4096), 64);
            const double dt = now_sec() - t0;
            fprintf(stderr, "banda RAM (%d threads): %.1f GB/s\n", pool.n, N / dt / 1e9);
        }
        free(p);
    }
    if (getenv("MOEB_SYNTH")) return synth_check();
    Model m; memset(&m, 0, sizeof m);
    load_model(&m, argv[1]);
    const Config *c = &m.cfg;
    const int d = c->dim, H = c->hidden, E = c->n_expert, K = c->n_used;
    if (getenv("MOEB_CHECK")) {     /* corretude: caminho novo (KATANA_MOE_*) contra o anterior, mesmas entradas; sem tempos */
        moe_opt_init();
        const int B = 4, NP = B * K;
        float *xs = xalloc((size_t)B * d, sizeof(float)), *o0 = xalloc((size_t)NP * d, sizeof(float)), *o1 = xalloc((size_t)NP * d, sizeof(float));
        int ex[MOE_NMAX], xi[MOE_NMAX], cnt = 0, bad = 0;
        double worst = 0;
        srand(77);
        for (int l = 0; l < c->n_layers && cnt < 6; l++) {
            const Layer *L = &m.layers[l];
            if (!L->eg) continue;
            cnt++;
            for (int i = 0; i < B * d; i++) xs[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * (1 + (i % 7 == 0) * 4);
            for (int mode = 0; mode < 2; mode++) {     /* 0: 1 token, K pares; 1: B tokens com especialistas repetidos (caminho agrupado) */
                const int n = mode ? NP : K, nx = mode ? B : 1;
                for (int j = 0; j < n; j++) { xi[j] = mode ? j / K : 0; ex[j] = mode ? (j % K) * 3 % E : 0; }
                if (!mode) for (int j = 0; j < K; j++) { int e, dup; do { e = rand() % E; dup = 0; for (int q = 0; q < j; q++) if (ex[q] == e) dup = 1; } while (dup); ex[j] = e; }
                if (mode) for (int j = 0; j < n; j++) ex[j] = (rand() % 5) * 7 % E;     /* poucos especialistas distintos */
                moe_opt_pf = moe_opt_rows = moe_opt_act = moe_opt_align = 0;
                moe_cpu_multi(&m, L, ex, xi, n, nx, xs, o0);
                moe_opt_pf = moe_opt_rows = moe_opt_act = moe_opt_align = 1;
                moe_cpu_multi(&m, L, ex, xi, n, nx, xs, o1);
                double mx = 0, md = 0;
                for (int i = 0; i < n * d; i++) { mx = fmax(mx, fabs(o0[i])); md = fmax(md, fabs((double)o0[i] - o1[i])); }
                const double rel = md / (mx > 0 ? mx : 1);
                if (rel > worst) worst = rel;
                if (rel > 1e-4) bad = 1;
                fprintf(stderr, "check camada %d modo %d: max|dif|=%.3g max|saida|=%.3g rel=%.3g\n", l, mode, md, mx, rel);
            }
        }
        fprintf(stderr, "moebench check: %s (pior rel %.3g)\n", bad ? "FALHOU" : "OK", worst);
        return bad;
    }
    float *x = xalloc(d, sizeof(float)), *outs = xalloc((size_t)K * d, sizeof(float));
    srand(1234);
    for (int i = 0; i < d; i++) x[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    int nl = 0, lastl = 0; size_t bytes = 0;
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m.layers[l];
        if (!L->eg) continue;
        nl++; lastl = l;
        bytes += (size_t)K * ((size_t)H * L->eg[0].stride + (size_t)H * L->eu[0].stride + (size_t)d * L->ed[0].stride);
    }
    fprintf(stderr, "d=%d H=%d E=%d K=%d camadas MoE=%d tipos g/u/d=%d/%d/%d; bytes/token=%.1f MB\n",
            d, H, E, K, nl, m.layers[lastl].eg[0].type, m.layers[lastl].eu[0].type, m.layers[lastl].ed[0].type, bytes / 1e6);
    int *sel = xalloc((size_t)T * c->n_layers * K, sizeof(int));
    for (int t = 0; t < T; t++)
        for (int l = 0; l < c->n_layers; l++)
            for (int k = 0; k < K; k++) {
                int e, dup;
                do { e = rand() % E; dup = 0; for (int q = 0; q < k; q++) if (sel[((size_t)t * c->n_layers + l) * K + q] == e) dup = 1; } while (dup);
                sel[((size_t)t * c->n_layers + l) * K + k] = e;
            }
    static const int zero[64];
    double chk = 0;
    for (int r = 0; r < R; r++) {
        moe_prof_on = getenv("MOEB_PROF") != NULL;
        memset(moe_prof, 0, sizeof moe_prof);
        const double t0 = now_sec();
        for (int t = 0; t < T; t++)
            for (int l = 0; l < c->n_layers; l++) {
                const Layer *L = &m.layers[l];
                if (!L->eg) continue;
                moe_cpu_multi(&m, L, sel + ((size_t)t * c->n_layers + l) * K, zero, K, 1, x, outs);
                chk += outs[l % d];
            }
        const double dt = (now_sec() - t0) / T;
        fprintf(stderr, "rep %d: %.2f ms/token  %.1f GB/s efetivos\n", r, dt * 1e3, bytes / dt / 1e9);
        if (moe_prof_on) fprintf(stderr, "  fases ms/token: xq %.2f  gate/up %.2f  ativ+xq %.2f  down %.2f\n",
                                 moe_prof[0] * 1e3 / T, moe_prof[1] * 1e3 / T, moe_prof[2] * 1e3 / T, moe_prof[3] * 1e3 / T);
    }
    moe_prof_on = 0;
    {   /* checagem numerica: saida da ultima camada MoE */
        const Layer *L = &m.layers[lastl];
        moe_cpu_multi(&m, L, sel, zero, K, 1, x, outs);
        double s = 0, s2 = 0; for (int i = 0; i < K * d; i++) { s += outs[i]; s2 += (double)outs[i] * outs[i]; }
        fprintf(stderr, "chk soma=%.6f norma2=%.6f\n", s, s2);
        FILE *f = getenv("MOEB_DUMP") ? fopen(getenv("MOEB_DUMP"), "wb") : NULL;
        if (f) { fwrite(outs, sizeof(float), (size_t)K * d, f); fclose(f); }
    }
    return chk == 12345.0;
}
