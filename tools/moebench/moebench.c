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
    Model m; memset(&m, 0, sizeof m);
    load_model(&m, argv[1]);
    const Config *c = &m.cfg;
    const int d = c->dim, H = c->hidden, E = c->n_expert, K = c->n_used;
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
