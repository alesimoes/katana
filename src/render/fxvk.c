/* ============================ render: FLUX.2 na GPU (Vulkan, camada kvk) ===============================
 * Produtos de matrizes do transformador (Q4_K e Q5_K, ~95% do tempo) nos kernels de matriz cooperativa do motor (mm.comp, o mesmo
 * dos prompts em lote): os pesos sobem uma vez, quantizados, no layout entrelacado de 32 linhas, e sao decodificados dentro do shader.
 * A ativacao entra em fp16 (transposta por um kernel proprio) e a saida volta em fp32. Pesos que nao cabem no orcamento de VRAM, outros
 * tipos (BF16, F32) e produtos de um vetor ficam na CPU, por tensor. KATANA_CPU=1 ou KATANA_FLUX_GPU=0 desliga; se nao ha GPU ou matriz
 * cooperativa o caminho da CPU e o unico. */

typedef struct { const SdgInfo *t; KvkBuf w; size_t wpr, bytes; uint32_t kind; int gpu; } FxvW;
static struct {
    int tried, on;
    FxvW w[320]; int nw;
    KvkBuf x32, xt, y, yt; size_t cx32, cxt, cy, cyt;
    KvkPipe tp32, ln, prep, att, swi, axg;
    uint64_t budget, wbytes; int nfall, shared_ok;      /* prec: 0 = ativacao e peso em fp16; 1 = peso em duas partes fp16; 2 = tambem a ativacao em duas partes */
    double t_up, t_gemm, t_dl; long ncalls;
} fxv;

static void fxv_init(void) {
    if (fxv.tried) return;
    fxv.tried = 1;
    const char *c = getenv("KATANA_CPU"), *g = getenv("KATANA_FLUX_GPU");
    if ((c && atoi(c)) || (g && !atoi(g))) return;
    KvkInfo I;
    if (!kvk_init(&I)) { fprintf(stderr, "flux: Vulkan indisponivel (%s), CPU\n", kvk_error()); return; }
    int coop = 0;
    if (I.coopmat && I.fp16 && I.storage16 && I.max_shared >= 32768)
        for (int i = 0; i < I.n_coop; i++)
            if (I.coop[i].m == 16 && I.coop[i].n == 16 && I.coop[i].k == 16 && I.coop[i].a_type == 0 && I.coop[i].b_type == 0 && I.coop[i].c_type == 1 && I.coop[i].r_type == 1 && I.coop[i].scope == 3) coop = 1;
    if (!coop) { fprintf(stderr, "flux: sem matriz cooperativa fp16 16x16x16, CPU\n"); kvk_shutdown(); return; }
    fxv.tp32 = kvk_pipe_create(fx_tp32_spv, sizeof fx_tp32_spv, 2, 16, NULL, NULL, 0);
    fxv.ln = kvk_pipe_create(fx_ln_spv, sizeof fx_ln_spv, 3, 20, NULL, NULL, 0);
    fxv.prep = kvk_pipe_create(fx_prep_spv, sizeof fx_prep_spv, 4, 20, NULL, NULL, 0);
    fxv.att = kvk_pipe_create(fx_att_spv, sizeof fx_att_spv, 2, 32, NULL, NULL, 0);
    fxv.swi = kvk_pipe_create(fx_swi_spv, sizeof fx_swi_spv, 3, 28, NULL, NULL, 0);
    fxv.axg = kvk_pipe_create(fx_axg_spv, sizeof fx_axg_spv, 3, 28, NULL, NULL, 0);
    if (!fxv.tp32.p || !fxv.ln.p || !fxv.prep.p || !fxv.att.p || !fxv.swi.p || !fxv.axg.p) { fprintf(stderr, "flux: pipelines Vulkan falharam (%s), CPU\n", kvk_error()); kvk_shutdown(); return; }
    uint64_t b = kvk_vram_budget(); if (!b) b = I.vram_bytes;
    fxv.budget = b / 100 * 92;      /* sobra para o sistema, a area de copia e o ladrilho dos kernels */
    fxv.on = 1; fxv.shared_ok = I.max_shared >= 36864;
    fprintf(stderr, "flux: GPU %s, orcamento de VRAM %.0f MB\n", I.name, fxv.budget / 1048576.0);
}

typedef struct { const uint8_t *src; uint32_t *dst; size_t rowbytes, wpr; int rows; } FxvPack;
static void fxv_pack_job(void *cx, int g0, int g1) {      /* g = grupo de 32 linhas: dst[(g*wpr + w)*32 + r] = palavra w da linha 32g + r */
    const FxvPack *j = cx;
    for (int g = g0; g < g1; g++)
        for (int r = 0; r < 32; r++) {
            const int row = g * 32 + r;
            if (row >= j->rows) { for (size_t w = 0; w < j->wpr; w++) j->dst[((size_t)g * j->wpr + w) * 32 + r] = 0; continue; }
            const uint32_t *s = (const uint32_t *)(j->src + (size_t)row * j->rowbytes);
            for (size_t w = 0; w < j->wpr; w++) j->dst[((size_t)g * j->wpr + w) * 32 + r] = s[w];
        }
}

/* peso na GPU (sobe na primeira chamada); NULL = esse tensor fica na CPU */
static const FxvW *fxv_weight(const SdgFile *g, const SdgInfo *t) {
    for (int i = 0; i < fxv.nw; i++) if (fxv.w[i].t == t) return fxv.w[i].gpu ? &fxv.w[i] : NULL;
    if (fxv.nw >= 320) return NULL;
    FxvW *e = &fxv.w[fxv.nw++]; memset(e, 0, sizeof *e); e->t = t;
    const int cols = (int)t->dim[0], rows = (int)t->dim[1];
    if ((t->type != T_Q4_K && t->type != T_Q5_K) || cols % 256) return NULL;
    e->kind = t->type; e->wpr = (size_t)cols / 256 * (t->type == T_Q4_K ? 36 : 44);
    const int G = (rows + 63) / 64 * 2;               /* grupos de 32 linhas, em multiplos de 64 linhas */
    e->bytes = (size_t)G * e->wpr * 32 * 4;
    const size_t act = (size_t)420 << 20;             /* reserva para as ativacoes */
    if (fxv.wbytes + e->bytes + act > fxv.budget) { fxv.nfall++; return NULL; }
    uint32_t *tmp = malloc(e->bytes);
    if (!tmp) return NULL;
    FxvPack pj = { sdg_data(g, t), tmp, e->wpr * 4, e->wpr, rows };
    pool_for(fxv_pack_job, &pj, G, 1);
    e->w = kvk_buf_create(e->bytes, KVK_USAGE_RW);
    if (!e->w.b || !kvk_buf_write(e->w, 0, tmp, e->bytes)) { free(tmp); if (e->w.b) kvk_buf_destroy(&e->w); fxv.nfall++; return NULL; }
    free(tmp);
    fxv.wbytes += e->bytes; e->gpu = 1;
    return e;
}

static int fxv_grow(KvkBuf *b, size_t *cap, size_t need);
static int fxv_grow(KvkBuf *b, size_t *cap, size_t need) {
    if (*cap >= need) return 1;
    if (b->b) kvk_buf_destroy(b);
    *b = kvk_buf_create(need, KVK_USAGE_RW); *cap = b->b ? need : 0;
    return b->b != 0;
}

/* produto GPU -> GPU: out[ooff + r*rows + j] = sum_k in[ioff + r*cols + k] * W[j][k], r < n (so grava na fila; o chamador espera) */
static int fxv_gemm(const FxvW *w, KvkBuf in, size_t ioff, int n, KvkBuf out, size_t ooff) {
    const int cols = (int)w->t->dim[0], rows = (int)w->t->dim[1];
    const size_t nx = (size_t)n * cols, ny = (size_t)n * rows;
    if (!fxv_grow(&fxv.xt, &fxv.cxt, nx * 4) || !fxv_grow(&fxv.y, &fxv.cy, ny * 4)) return 0;
    const uint32_t p1[4] = { (uint32_t)n, (uint32_t)cols, (uint32_t)ioff, 0 }, p2[4] = { (uint32_t)rows, (uint32_t)n, 0, (uint32_t)ooff };
    KvkBuf b1[2] = { in, fxv.xt }, b2[2] = { fxv.y, out };
    int ok = kvk_dispatch(fxv.tp32, b1, 2, p1, 16, (uint32_t)((cols + 15) / 16), (uint32_t)((n + 15) / 16), 1);
    ok &= vk_mm_pw(w->kind, w->w, 0, w->wpr, fxv.xt, fxv.y, rows, cols, n, 0, 1);
    ok &= kvk_dispatch(fxv.tp32, b2, 2, p2, 16, (uint32_t)((n + 15) / 16), (uint32_t)((rows + 15) / 16), 1);
    return ok;
}

/* Y[n][rows] = X[n][cols] * W^T na GPU, com a ida e a volta pelo hospedeiro; 0 = nao fez (o chamador usa a CPU) */
static int fxv_lin(const SdgFile *g, float *Y, const float *X, int n, const SdgInfo *t) {
    fxv_init();
    if (!fxv.on || n < 2) return 0;
    const int cols = (int)t->dim[0], rows = (int)t->dim[1];
    const FxvW *w = fxv_weight(g, t);
    if (!w) return 0;
    const size_t nx = (size_t)n * cols, ny = (size_t)n * rows;
    if (!fxv_grow(&fxv.x32, &fxv.cx32, nx * 4) || !fxv_grow(&fxv.yt, &fxv.cyt, ny * 4)) return 0;
    double t0 = sdt_now();
    if (!kvk_buf_write(fxv.x32, 0, X, nx * 4)) return 0;
    double t1 = sdt_now(); fxv.t_up += t1 - t0;
    int ok = fxv_gemm(w, fxv.x32, 0, n, fxv.yt, 0);
    if (!ok || !kvk_wait()) { fprintf(stderr, "flux: falha na GPU (%s), CPU\n", kvk_error()); fxv.on = 0; return 0; }
    double t2 = sdt_now(); fxv.t_gemm += t2 - t1;
    if (!kvk_buf_read(fxv.yt, 0, Y, ny * 4)) { fxv.on = 0; return 0; }
    fxv.t_dl += sdt_now() - t2; fxv.ncalls++;
    return 1;
}

/* ---- bloco inteiro na GPU: z [texto; imagem] e as ativacoes ficam na VRAM, so a entrada e a saida passam pelo hospedeiro ---- */
static struct { KvkBuf z, xn, h, cat, o, mod, cs, sn, nw; int N; size_t cap; double t; } fxr;
#define FXV_D 3072
static int fxv_res_open(int N) {
    if (!fxv.on || !fxv.shared_ok) return 0;
    if (fxr.N != N || !fxr.z.b) {
        KvkBuf *all[] = { &fxr.z, &fxr.xn, &fxr.h, &fxr.cat, &fxr.o, &fxr.mod, &fxr.cs, &fxr.sn };
        for (int i = 0; i < 8; i++) if ((*all[i]).b) kvk_buf_destroy(all[i]);
        const size_t D = FXV_D, n = (size_t)N;
        const size_t sz[8] = { n * D * 4, n * D * 4, n * 27648 * 4, n * 12288 * 4, n * D * 4, 15 * D * 4, n * 128 * 4, n * 128 * 4 };
        size_t tot = 0; for (int i = 0; i < 8; i++) tot += sz[i];
        if (fxv.wbytes + tot + ((size_t)n * 12288 * 4 + n * 27648 * 4) + ((size_t)64 << 20) > fxv.budget) return 0;
        for (int i = 0; i < 8; i++) { *all[i] = kvk_buf_create(sz[i], KVK_USAGE_RW); if (!all[i]->b) return 0; }
        fxr.N = N;
    }
    if (!fxr.nw.b) { fxr.nw = kvk_buf_create((size_t)25 * 512 * 4, KVK_USAGE_RW); if (!fxr.nw.b) return 0; }
    return 1;
}
static int fxv_ln(KvkBuf in, size_t ioff, size_t ooff, int rows, size_t sh, size_t sc, KvkBuf out) {
    const uint32_t p[5] = { FXV_D, (uint32_t)ioff, (uint32_t)ooff, (uint32_t)sh, (uint32_t)sc };
    KvkBuf b[3] = { in, fxr.mod, out };
    return kvk_dispatch(fxv.ln, b, 3, p, 20, (uint32_t)rows, 1, 1);
}
static int fxv_prep(KvkBuf d, int ld, int N, int nt, size_t wofs) {
    const uint32_t p[5] = { (uint32_t)ld, (uint32_t)nt, 0, FXV_D, (uint32_t)wofs };
    KvkBuf b[4] = { d, fxr.nw, fxr.cs, fxr.sn };
    return kvk_dispatch(fxv.prep, b, 4, p, 20, (uint32_t)N * 48, 1, 1);
}
static int fxv_att(KvkBuf qkv, KvkBuf out, int N, int ld, int ldo) {
    struct { uint32_t n, ld, qo, ko, vo, ldo, oo; float scale; } p = { (uint32_t)N, (uint32_t)ld, 0, FXV_D, 2 * FXV_D, (uint32_t)ldo, 0, 1.0f / sqrtf(128.0f) };
    KvkBuf b[2] = { qkv, out };
    return kvk_dispatch(fxv.att, b, 2, &p, 32, (uint32_t)((N + 63) / 64), 24, 1);
}
static int fxv_swi(KvkBuf h, size_t ho, int lda, KvkBuf y, size_t yo, int ldy, int n, int m) {
    const uint32_t p[7] = { (uint32_t)n, (uint32_t)m, (uint32_t)lda, (uint32_t)ldy, (uint32_t)ho, (uint32_t)yo, 0 };
    KvkBuf b[3] = { h, y, fxr.mod };
    return kvk_dispatch(fxv.swi, b, 3, p, 28, (uint32_t)(((size_t)n * m + 255) / 256), 1, 1);
}
static int fxv_axg(KvkBuf y, KvkBuf x, size_t xo, int n, size_t go) {         /* x[xo + r*D + i] += mod[go + i] * y[r*D + i] */
    const uint32_t p[7] = { (uint32_t)n, FXV_D, FXV_D, FXV_D, 0, (uint32_t)xo, (uint32_t)go };
    KvkBuf b[3] = { y, x, fxr.mod };
    return kvk_dispatch(fxv.axg, b, 3, p, 28, (uint32_t)(((size_t)n * FXV_D + 255) / 256), 1, 1);
}
