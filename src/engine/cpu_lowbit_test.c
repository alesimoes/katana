/* ---------------- katana lbtest: conferencia e medidas dos kernels de poucos bits (incluido por cpu_lowbit.c) ----------------
   katana lbtest [arquivo.gguf] [bench]
   Sem arquivo: blocos aleatorios de cada tipo. Com arquivo (pode ser um .part em download): os primeiros tensores de
   especialistas de cada tipo cujos bytes ja chegaram. Confere contra o produto escalar dequantizado em double
   (erro relativo a soma dos modulos < 1e-4, o mesmo criterio do qtest); com "bench" mede G pesos/s. */
#if HAVE_LOWBIT
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct { char name[96]; uint32_t type; uint64_t dims[4]; int nd; uint64_t off; } LbRT;

static const char *lb_tname(uint32_t t) {
    switch (t) {
    case T_IQ2_XXS: return "IQ2_XXS"; case T_IQ2_XS: return "IQ2_XS"; case T_IQ3_XXS: return "IQ3_XXS"; case T_IQ3_S: return "IQ3_S";
    case T_IQ2_S: return "IQ2_S"; case T_IQ4_XS: return "IQ4_XS"; case T_IQ4_NL: return "IQ4_NL"; case T_Q2_0: return "Q2_0";
    default: return "?";
    }
}

static int lb_kvskip(const uint8_t **p, uint32_t t) {
    static const int sz[13] = { 1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8 };
    if (t == 8) { uint64_t l; memcpy(&l, *p, 8); *p += 8 + l; return 1; }
    if (t == 9) {
        uint32_t et; uint64_t n; memcpy(&et, *p, 4); memcpy(&n, *p + 4, 8); *p += 12;
        for (uint64_t i = 0; i < n; i++) if (!lb_kvskip(p, et)) return 0;
        return 1;
    }
    if (t > 12) return 0;
    *p += sz[t];
    return 1;
}

/* le o cabecalho do GGUF mapeado; devolve os tensores e o inicio dos dados */
static LbRT *lb_gguf(const uint8_t *m, size_t fsize, int *nt, const uint8_t **data) {
    const uint8_t *p = m;
    uint64_t nten, nkv;
    if (fsize < 24 || memcmp(p, "GGUF", 4)) return NULL;
    memcpy(&nten, p + 8, 8); memcpy(&nkv, p + 16, 8); p += 24;
    for (uint64_t i = 0; i < nkv; i++) {
        uint64_t l; uint32_t t;
        memcpy(&l, p, 8); p += 8 + l; memcpy(&t, p, 4); p += 4;
        if (!lb_kvskip(&p, t)) return NULL;
    }
    LbRT *r = xalloc(nten, sizeof *r);
    for (uint64_t i = 0; i < nten; i++) {
        uint64_t l; uint32_t nd;
        memcpy(&l, p, 8); p += 8;
        memset(r[i].name, 0, sizeof r[i].name);
        memcpy(r[i].name, p, l < 95 ? l : 95); p += l;
        memcpy(&nd, p, 4); p += 4; r[i].nd = (int)nd;
        for (uint32_t d = 0; d < nd; d++) { uint64_t v; memcpy(&v, p, 8); p += 8; if (d < 4) r[i].dims[d] = v; }
        memcpy(&r[i].type, p, 4); p += 4; memcpy(&r[i].off, p, 8); p += 8;
    }
    *nt = (int)nten;
    *data = m + (((size_t)(p - m) + 31) & ~(size_t)31);
    return r;
}

static double lb_ref(uint32_t type, const uint8_t *row, const float *x, int cols, float *tmp, double *mag) {
    dequant(type, row, tmp, cols);
    double s = 0, a = 0;
    for (int j = 0; j < cols; j++) { s += (double)tmp[j] * x[j]; a += fabs((double)tmp[j] * x[j]); }
    *mag = a;
    return s;
}

/* confere uma matriz (rows linhas de cols) em todos os modos da CPU: linha unica e lote de 8 */
static int lb_check(const char *label, uint32_t type, const uint8_t *buf, size_t stride, int rows, int cols) {
    float *x = xalloc((size_t)LB_MAXB * cols, sizeof(float)), *tmp = xalloc(cols, sizeof(float));
    XQ xqs[LB_MAXB]; memset(xqs, 0, sizeof xqs);
    for (int i = 0; i < LB_MAXB * cols; i++) x[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    for (int t = 0; t < LB_MAXB; t++) { xq_tls = &xqs[t]; xq_prepare(x + (size_t)t * cols, cols); }
    xq_tls = NULL;
    Mat W; memset(&W, 0, sizeof W); W.q = buf; W.type = type; W.stride = stride;
    const int top = lb_mode_v;
    int bad = 0;
    printf("  %-8s %-28s", lb_tname(type), label);
    for (int mode = 1; mode <= 3; mode++) {
        if (mode > top) { printf("  m%d: -", mode); continue; }
        lb_mode_v = mode;
        double e1 = 0, e8 = 0;
        for (int r = 0; r < rows; r++) {
            const uint8_t *row = buf + (size_t)r * stride;
            float out[LB_MAXB];
            xq_tls = &xqs[0];
            double mag, ref = lb_ref(type, row, x, cols, tmp, &mag);
            double e = fabs(lowbit_dot(&W, row, cols) - ref) / (mag + 1e-30);
            if (e > e1) e1 = e;
            xq_tls = NULL;
            lowbit_dot_batch(&W, row, xqs, 0, LB_MAXB, cols, out);
            for (int t = 0; t < LB_MAXB; t++) {
                ref = lb_ref(type, row, x + (size_t)t * cols, cols, tmp, &mag);
                e = fabs(out[t] - ref) / (mag + 1e-30);
                if (e > e8) e8 = e;
            }
            lowbit_dot_batch(&W, row, xqs, 3, 10, cols, out);          /* 7 tokens: um bloco de 4 e a sobra de 3 */
            for (int t = 3; t < 10; t++) {
                ref = lb_ref(type, row, x + (size_t)t * cols, cols, tmp, &mag);
                e = fabs(out[t - 3] - ref) / (mag + 1e-30);
                if (e > e8) e8 = e;
            }
        }
        printf("  m%d: %.1e/%.1e", mode, e1, e8);
        if (e1 > 1e-4 || e8 > 1e-4) bad = 1;
    }
    lb_mode_v = top;
    printf("  %s\n", bad ? "FALHOU" : "OK");
    free(x); free(tmp);
    return bad;
}

static volatile float lb_sink;
LB_256 static float lb_dotonly_v256(const int16_t *w, const int16_t *x, int n, int reps) {
    __m256i p0 = _mm256_setzero_si256(), p1 = _mm256_setzero_si256();
    for (int r = 0; r < reps; r++)
        for (int g = 0; g < n; g += 64) { p0 = LB_DOT_V256(p0, w + g, x + g); p1 = LB_DOT_V256(p1, w + g + 32, x + g + 32); }
    return (float)_mm256_extract_epi32(_mm256_add_epi32(p0, p1), 0);
}
LB_512 static float lb_dotonly_v512(const int16_t *w, const int16_t *x, int n, int reps) {
    __m512i p0 = _mm512_setzero_si512(), p1 = _mm512_setzero_si512();
    for (int r = 0; r < reps; r++)
        for (int g = 0; g < n; g += 64) { p0 = LB_DOT_V512(p0, w + g, x + g); p1 = LB_DOT_V512(p1, w + g + 32, x + g + 32); }
    return (float)_mm512_reduce_add_epi32(_mm512_add_epi32(p0, p1));
}
typedef struct { const Mat *W; const float *x; int cols; } LbFloatJob;
static void lb_bench_float(void *c, int i0, int i1) {
    const LbFloatJob *j = c;
    float a = 0;
    for (int i = i0; i < i1; i++) a += dot_row(j->W, (size_t)i, j->x, NULL, j->cols);
    lb_sink += a;
}

/* caminho 0 = float (antes), 1..3 = modos novos; G pesos/s no cache, varrendo a matriz (1 thread e todas) e em lote */
static void lb_bench(const char *label, uint32_t type, const uint8_t *buf, size_t stride, int rows, int cols) {
    const LbT *T = lb_by_type(type);
    float *x = xalloc((size_t)LB_MAXB * cols, sizeof(float));
    for (int i = 0; i < LB_MAXB * cols; i++) x[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    XQ xqs[LB_MAXB]; memset(xqs, 0, sizeof xqs);
    for (int t = 0; t < LB_MAXB; t++) { xq_tls = &xqs[t]; xq_prepare(x + (size_t)t * cols, cols); }
    xq_tls = NULL;
    xq0 = xqs[0];
    Mat W; memset(&W, 0, sizeof W); W.q = buf; W.type = type; W.stride = stride;
    const int top = lb_mode_v;
    const double wr = (double)cols;
    printf("  %s %s (cols=%d, %d linhas, %.0f MB, %d threads)\n", lb_tname(type), label, cols, rows, (double)rows * stride / 1e6, pool.n);
    {
        int16_t w[256] __attribute__((aligned(64))); float f[8];
        const int reps = (int)(3e8 / cols) + 1;
        double t0 = now_sec();
        for (int r = 0; r < reps; r++)
            for (int c = 0; c < cols; c += 256) T->dec(buf + (size_t)(r & 7) * stride + (size_t)(c / T->unit) * T->ub, w, f, cols - c < 256 ? cols - c : 256);
        lb_sink += w[3];
        const double g2 = (double)reps * cols / (now_sec() - t0) / 1e9;
        t0 = now_sec();
        for (int r = 0; r < reps; r++)
            for (int c = 0; c < cols; c += 256) T->dec5(buf + (size_t)(r & 7) * stride + (size_t)(c / T->unit) * T->ub, w, f, cols - c < 256 ? cols - c : 256);
        lb_sink += w[3];
        printf("    so decodificar (1 thread, no cache): AVX2 %6.2f | AVX-512 %6.2f G pesos/s\n", g2, (double)reps * cols / (now_sec() - t0) / 1e9);
    }
    {   /* so o produto, com os pesos de um trecho de 256 ja decodificados (o que um repack em inteiros pouparia) */
        int16_t w[256] __attribute__((aligned(64))); float f[8];
        T->dec(buf, w, f, cols < 256 ? cols : 256);
        const int n = cols < 256 ? cols : 256;
        const int reps = (int)(6e8 / n) + 1;
        for (int path = 2; path <= top; path++) {
            double t0 = now_sec();
            const int16_t *xq_q = xqs[0].q;
            float r = path == 2 ? lb_dotonly_v256(w, xq_q, n, reps) : lb_dotonly_v512(w, xq_q, n, reps);
            lb_sink += r;
            printf("    so produto, pesos ja em int16 (1T, modo %d): %6.2f G pesos/s\n", path, (double)reps * n / (now_sec() - t0) / 1e9);
        }
    }
    for (int path = 0; path <= top; path++) {
        cpu_exact = path == 0; lb_mode_v = path ? path : top;
        const int reps = (int)(3e8 / cols) + 1;
        double t0 = now_sec();
        for (int r = 0; r < reps; r++) lb_sink += dot_row(&W, (size_t)(r & 3) % rows, x, NULL, cols);
        const double g_cache = (double)reps * wr / (now_sec() - t0) / 1e9;
        const int passes1 = (int)(4e8 / ((double)rows * cols)) + 1;
        t0 = now_sec();
        for (int ps = 0; ps < passes1; ps++) for (int i = 0; i < rows; i++) lb_sink += dot_row(&W, (size_t)i, x, NULL, cols);
        const double g_1t = (double)passes1 * rows * wr / (now_sec() - t0) / 1e9;
        LbFloatJob job = { &W, x, cols };
        const int passesn = (int)(2e9 / ((double)rows * cols)) + 1;
        t0 = now_sec();
        for (int ps = 0; ps < passesn; ps++) pool_for(lb_bench_float, &job, rows, rows / (pool.n * 8) + 1);
        const double g_nt = (double)passesn * rows * wr / (now_sec() - t0) / 1e9;
        printf("    %-16s no cache 1T: %6.2f | varrendo 1T: %6.2f | varrendo %dT: %6.2f G pesos/s (%.1f GB/s)\n",
               path == 0 ? "float (antes)" : path == 1 ? "AVX2 madd" : path == 2 ? "VNNI 256" : "AVX-512",
               g_cache, g_1t, pool.n, g_nt, g_nt * (double)stride / cols);
        cpu_exact = 0;
    }
    for (int pv = 1; pv <= top + (top == 3); pv++) {
        const int path = pv > 3 ? 3 : pv;
        lb_mode_v = path;
        lb_tile = pv <= 3;
        float out[LB_MAXB];
        printf("    lote, modo %d%s (1T), G pesos*token/s:", path, path == 3 ? (lb_tile ? " bloco de 4" : " sem bloco") : "");
        for (int nb = 8; nb <= LB_MAXB; nb *= 4) {
            const int n = rows < 4096 ? rows : 4096;
            const int passes = (int)(1e9 / ((double)n * cols * nb)) + 1;
            double t0 = now_sec();
            for (int ps = 0; ps < passes; ps++)
                for (int i = 0; i < n; i++) { lowbit_dot_batch(&W, buf + (size_t)i * stride, xqs, 0, nb, cols, out); lb_sink += out[0]; }
            const double g_b = (double)passes * n * wr * nb / (now_sec() - t0) / 1e9;
            t0 = now_sec();
            for (int ps = 0; ps < passes; ps++)
                for (int i = 0; i < n; i++)
                    for (int t = 0; t < nb; t++) { xq_tls = &xqs[t]; lb_sink += lowbit_dot(&W, buf + (size_t)i * stride, cols); xq_tls = NULL; }
            const double g_p = (double)passes * n * wr * nb / (now_sec() - t0) / 1e9;
            printf("  %d tokens: decodifica uma vez %6.2f, por token %6.2f |", nb, g_b, g_p);
        }
        printf("\n");
    }
    lb_tile = 1;
    lb_mode_v = top;
    free(x);
}

static int lowbit_test(int argc, char **argv) {
    static const uint32_t types[] = { T_IQ2_XXS, T_IQ2_XS, T_IQ3_XXS, T_IQ3_S, T_IQ2_S, T_IQ4_XS, T_IQ4_NL, T_Q2_0 };
    const char *path = argc > 0 && strcmp(argv[0], "bench") ? argv[0] : NULL;
    int bench = 0;
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], "bench")) bench = 1;
    pool_init();
    lb_mode();
    const int def = lb_mode_v;
    printf("lbtest: modo padrao %d (1 AVX2, 2 VNNI 256, 3 AVX-512; KATANA_LOWBIT)\n", def);
    srand(777);
    int fails = 0;
    if (def > 0 && def < 3 && __builtin_cpu_supports("avx512vnni")) lb_mode_v = 3;      /* o teste cobre todos os modos da CPU */
    printf("blocos aleatorios (erro maximo contra o produto dequantizado: linha unica / lote de 8):\n");
    for (size_t k = 0; k < sizeof types / sizeof types[0]; k++) {
        const uint32_t t = types[k];
        const LbT *T = lb_by_type(t);
        for (int ci = 0; ci < 2; ci++) {
            const int cols = ci ? 640 : 2560;
            if (cols % T->unit) continue;
            const size_t stride = qbytes(t, cols);
            const int rows = 96;
            uint8_t *buf = xalloc((size_t)rows * stride, 1);
            for (size_t i = 0; i < (size_t)rows * stride; i++) buf[i] = (uint8_t)(rand() >> 7);
            for (size_t b = 0; b < (size_t)rows * stride / T->ub; b++) {
                const uint16_t h = (uint16_t)(0x1C00 + rand() % 0x400);
                memcpy(buf + b * T->ub, &h, 2);
            }
            if (__builtin_cpu_supports("avx512vnni")) {     /* decodificador AVX-512 igual ao AVX2, peso a peso */
                int16_t wa[256] __attribute__((aligned(64))), wb[256] __attribute__((aligned(64))); float fa[8], fb[8];
                int diff = 0;
                for (int r = 0; r < rows; r++)
                    for (int c = 0; c < cols; c += 256) {
                        const int nn = cols - c < 256 ? cols - c : 256;
                        const uint8_t *src = buf + (size_t)r * stride + (size_t)(c / T->unit) * T->ub;
                        memset(fa, 0, sizeof fa); memset(fb, 0, sizeof fb);
                        T->dec(src, wa, fa, nn); T->dec5(src, wb, fb, nn);
                        if (memcmp(wa, wb, nn * 2) || memcmp(fa, fb, sizeof fa)) diff++;
                    }
                printf("  %-8s decodificador AVX-512 = AVX2 (cols=%d): %s\n", lb_tname(t), cols, diff ? "DIFERENTE" : "igual");
                fails += diff != 0;
            }
            char lab[32]; snprintf(lab, sizeof lab, "aleatorio cols=%d", cols);
            fails += lb_check(lab, t, buf, stride, rows, cols);
            free(buf);
        }
    }
    {   /* BF16: expoentes entre 2^-6 e 2^3, ativacao em float */
        const int cols = 2560, rows = 64;
        uint8_t *buf = xalloc((size_t)rows * cols * 2, 1);
        float *x = xalloc(cols, sizeof(float)), *tmp = xalloc(cols, sizeof(float));
        for (int i = 0; i < cols; i++) x[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
        for (size_t i = 0; i < (size_t)rows * cols; i++) {
            const uint16_t h = (uint16_t)(((rand() & 1) << 15) | ((121 + rand() % 10) << 7) | (rand() & 127));
            memcpy(buf + 2 * i, &h, 2);
        }
        double e = 0;
        for (int r = 0; r < rows; r++) {
            double mag, ref = lb_ref(T_BF16, buf + (size_t)r * cols * 2, x, cols, tmp, &mag);
            const double d = fabs(lowbit_bf16_dot(buf + (size_t)r * cols * 2, x, cols) - ref) / (mag + 1e-30);
            if (d > e) e = d;
        }
        printf("  BF16     aleatorio cols=%d                  erro %.1e  %s\n", cols, e, e > 1e-5 ? "FALHOU" : "OK");
        fails += e > 1e-5;
        if (bench) {
            Mat W; memset(&W, 0, sizeof W); W.q = buf; W.type = T_BF16; W.stride = (size_t)cols * 2;
            const int reps = 400000;
            double t0 = now_sec();
            for (int r = 0; r < reps; r++) lb_sink += dot_row(&W, (size_t)(r & 7), x, NULL, cols);
            printf("  BF16 no cache 1T: %.2f G pesos/s\n", (double)reps * cols / (now_sec() - t0) / 1e9);
        }
        free(buf); free(x); free(tmp);
    }
    if (path) {
        int fd = open(path, O_RDONLY);
        struct stat st;
        if (fd < 0 || fstat(fd, &st)) { printf("nao abriu %s\n", path); return 1; }
        const uint8_t *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        int nt = 0; const uint8_t *data = NULL;
        LbRT *rt = m == (const uint8_t *)MAP_FAILED ? NULL : lb_gguf(m, (size_t)st.st_size, &nt, &data);
        if (!rt) { printf("cabecalho invalido em %s\n", path); return 1; }
        printf("tensores reais de %s (%d tensores; so os bytes ja baixados valem):\n", path, nt);
        for (size_t k = 0; k < sizeof types / sizeof types[0]; k++) {
            const uint32_t t = types[k];
            int shown = 0;
            for (int i = 0; i < nt && shown < 2; i++) {
                if (rt[i].type != t || !strstr(rt[i].name, "exps") || rt[i].nd < 2) continue;
                const int cols = (int)rt[i].dims[0], rows = (int)rt[i].dims[1];
                const size_t stride = qbytes(t, cols), total = stride * rows * (rt[i].nd > 2 ? rt[i].dims[2] : 1);
                if ((size_t)(data - m) + rt[i].off + total > (size_t)st.st_size) continue;      /* ainda nao baixado */
                fails += lb_check(rt[i].name, t, data + rt[i].off, stride, rows < 128 ? rows : 128, cols);
                shown++;
            }
            if (!shown) printf("  %-8s (nenhum tensor de especialistas completo no arquivo ainda)\n", lb_tname(t));
        }
        lb_mode_v = def > 0 && __builtin_cpu_supports("avx512vnni") ? 3 : def;
        if (bench) {
            printf("medidas em tensores reais (G pesos/s):\n");
            for (size_t k = 0; k < sizeof types / sizeof types[0]; k++) {
                const uint32_t t = types[k];
                for (int i = 0; i < nt; i++) {
                    if (rt[i].type != t || !strstr(rt[i].name, "exps") || rt[i].nd < 3) continue;
                    const int cols = (int)rt[i].dims[0];
                    const size_t stride = qbytes(t, cols), total = stride * rt[i].dims[1] * rt[i].dims[2];
                    if ((size_t)(data - m) + rt[i].off + total > (size_t)st.st_size) continue;
                    size_t rows = rt[i].dims[1] * rt[i].dims[2];
                    if (rows * stride > 400000000u) rows = 400000000u / stride;
                    volatile uint8_t s = 0;
                    for (size_t o = 0; o < rows * stride; o += 4096) s += data[rt[i].off + o];     /* traz para a memoria */
                    lb_bench(rt[i].name, t, data + rt[i].off, stride, (int)rows, cols);
                    break;
                }
            }
        }
    } else if (bench) {
        lb_mode_v = def > 0 && __builtin_cpu_supports("avx512vnni") ? 3 : def;
        printf("medidas em blocos aleatorios (G pesos/s):\n");
        for (size_t k = 0; k < sizeof types / sizeof types[0]; k++) {
            const uint32_t t = types[k];
            const int cols = 2560;
            const size_t stride = qbytes(t, cols);
            const int rows = (int)(400000000u / stride);
            uint8_t *buf = xalloc((size_t)rows * stride, 1);
            for (size_t i = 0; i < (size_t)rows * stride; i++) buf[i] = (uint8_t)(rand() >> 7);
            const LbT *T = lb_by_type(t);
            for (size_t b = 0; b < (size_t)rows * stride / T->ub; b++) { const uint16_t h = (uint16_t)(0x1C00 + rand() % 0x400); memcpy(buf + b * T->ub, &h, 2); }
            lb_bench("aleatorio", t, buf, stride, rows, cols);
            free(buf);
        }
    }
    lb_mode_v = def;
    printf("lbtest: %s\n", fails ? "FALHOU" : "OK");
    return fails ? 1 : 0;
}
#else
static int lowbit_test(int argc, char **argv) { (void)argc; (void)argv; printf("lbtest: sem AVX2 nesta compilacao\n"); return 0; }
#endif
