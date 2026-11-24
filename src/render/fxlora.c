/* ============================ render: adaptadores LoRA do transformador do FLUX.2 klein ===============================
 * Um adaptador de posto r em cada matriz de atencao e de MLP de todos os blocos (blocos duplos: qkv, proj, mlp.0 e mlp.2 dos fluxos de imagem e
 * de texto; blocos simples: linear1 e linear2): 80 matrizes. O peso base fica quantizado e congelado; o adaptador e um caminho a parte,
 * Y += escala * s * (X A^T) B^T, somado a saida do produto (fxd_lin em fxdit.c chama fxl_apply). Sem adaptador ativo (g_fxl_on = 0) o custo
 * e um teste de inteiro. Convencoes do .kspec: A [r][entrada] sorteada em +-1/sqrt(entrada), B [saida][r] zero (comeca igual ao base),
 * s = alpha / r, AdamW com correcao de vies.
 *
 * Arquivo .fxlora: "FXLORA01", tamanho do GGUF base (u64), impressao do GGUF base (u64: FNV por palavras de 8 bytes do arquivo inteiro),
 * n modulos (u32); cada um: tamanho do nome (u32), nome do tensor, linhas (u64), colunas (u64), posto (i32), s (f32), A em f32, B em f32;
 * por fim "KSXF" + marcas (u32). O estado do otimizador fica ao lado (<arquivo>.kopt: "FXOPT001", passo, n, momentos de A e B por modulo). */

typedef struct {
    char name[160]; const SdgInfo *t;
    int rows, cols, r; float s;
    float *A, *B;                       /* A [r][cols], B [rows][r] */
    float *gA, *gB, *mA1, *mA2, *mB1, *mB2;   /* so no treino */
} FxlMod;
typedef struct { FxlMod *m; int n; float scale; uint64_t base_size, base_hash; } FxLora;
static FxLora g_fxl_store;

/* produto generico C[n][m] = A[n][k] * B (B [k][m] com passo ldb, ou transposto: B(kk, j) = B[j*ldb + kk]); empacota B em paineis e roda
 * nas tarefas em duas dimensoes do produto de fxdit.c. Serve ao caminho do adaptador e a toda a volta do treino. */
typedef struct { const float *B; float *P; int k, m, NR, ldb, tr; } FxbPack;
static void fxb_pack_job(void *cx, int p0, int p1) {
    const FxbPack *j = cx;
    for (int p = p0; p < p1; p++) {
        float *o = j->P + (size_t)p * j->k * j->NR;
        for (int kk = 0; kk < j->k; kk++, o += j->NR)
            for (int r = 0; r < j->NR; r++) {
                const int c = p * j->NR + r;
                o[r] = c >= j->m ? 0.0f : j->tr ? j->B[(size_t)c * j->ldb + kk] : j->B[(size_t)kk * j->ldb + c];
            }
    }
}
static float *fxb_pack(const float *B, int ldb, int k, int m, int tr) {
    const int NR = vt_nr(), np = (m + NR - 1) / NR;
    float *P = vt_alloc((size_t)np * NR * k * 4);
    FxbPack j = { B, P, k, m, NR, ldb, tr };
    pool_for(fxb_pack_job, &j, np, 1);
    return P;
}
/* C = A * P (P ja em paineis, k x m) */
static void fxb_run(float *C, size_t ldc, const float *A, size_t lda, const float *P, int n, int k, int m) {
    const int NR = vt_nr(), np = (m + NR - 1) / NR;
    FxdWE we = { NULL, (float *)P, NULL };
    int G = (int)((2u << 20) / ((size_t)k * NR * 4)); if (G < 1) G = 1;
    const int nrb = (n + FXD_RB - 1) / FXD_RB, npg = (np + G - 1) / G;
    FxdGj j = { C, A, &we, n, k, m, NR, G, np, nrb };
    if (ldc != (size_t)m || lda != (size_t)k) die("fxb_run: so matrizes contiguas");
    pool_for(fxd_gj_job, &j, npg * nrb, 1);
}
static void fxb_mm(float *C, size_t ldc, const float *A, size_t lda, int n, int k, int m, const float *B, int ldb, int tr) {
    float *P = fxb_pack(B, ldb, k, m, tr);
    fxb_run(C, ldc, A, lda, P, n, k, m);
    vt_free(P);
}

typedef struct { float *Y; const float *T; float s; size_t m; } FxlAdd;
static void fxl_add_job(void *cx, int r0, int r1) {
    const FxlAdd *j = cx;
    for (int r = r0; r < r1; r++) { float *y = j->Y + (size_t)r * j->m; const float *t = j->T + (size_t)r * j->m; for (size_t i = 0; i < j->m; i++) y[i] += j->s * t[i]; }
}

static const FxlMod *fxl_find(const SdgInfo *t) {
    for (int i = 0; i < g_fxl_store.n; i++) if (g_fxl_store.m[i].t == t) return &g_fxl_store.m[i];
    return NULL;
}
/* U = X A^T [n][r] (devolvido pelo chamador se u != NULL) e Y += escala * s * U B^T */
static void fxl_apply_mod(float *Y, const float *X, int n, const FxlMod *m, float scale, float *u_out) {
    const int r = m->r, rows = m->rows;
    float *U = u_out ? u_out : vt_alloc((size_t)n * r * 4);
    fxb_mm(U, (size_t)r, X, (size_t)m->cols, n, m->cols, r, m->A, m->cols, 1);
    const int CH = 512;
    float *P = fxb_pack(m->B, r, r, rows, 1);                 /* B(kk, j) = B[j][kk] */
    float *T = vt_alloc((size_t)(n < CH ? n : CH) * rows * 4);
    for (int r0 = 0; r0 < n; r0 += CH) {
        const int nn = n - r0 < CH ? n - r0 : CH;
        fxb_run(T, (size_t)rows, U + (size_t)r0 * r, (size_t)r, P, nn, r, rows);
        FxlAdd a = { Y + (size_t)r0 * rows, T, scale * m->s, (size_t)rows };
        pool_for(fxl_add_job, &a, nn, 8);
    }
    vt_free(T); vt_free(P);
    if (!u_out) vt_free(U);
}
static void fxl_apply(float *Y, const float *X, int n, const SdgInfo *t) {
    const FxlMod *m = fxl_find(t);
    if (m) fxl_apply_mod(Y, X, n, m, g_fxl_store.scale, NULL);
}

/* impressao do GGUF base: tamanho e FNV por palavras de 8 bytes */
static uint64_t fxl_base_hash(const SdgFile *g) {
    uint64_t h = 1469598103934665603ULL; uint64_t w;
    const size_t nw = (size_t)g->size / 8;
    for (size_t i = 0; i < nw; i++) { memcpy(&w, g->base + i * 8, 8); h = (h ^ w) * 1099511628211ULL; }
    return h ^ (uint64_t)g->size;
}

/* os 80 pesos que ganham adaptador */
static int fxl_names(char names[][160]) {
    int n = 0;
    static const char *dbl[8] = { "img_attn.qkv", "img_attn.proj", "img_mlp.0", "img_mlp.2", "txt_attn.qkv", "txt_attn.proj", "txt_mlp.0", "txt_mlp.2" };
    for (int b = 0; b < FXD_NDBL; b++) for (int k = 0; k < 8; k++) snprintf(names[n++], 160, "double_blocks.%d.%s.weight", b, dbl[k]);
    for (int b = 0; b < FXD_NSGL; b++) { snprintf(names[n++], 160, "single_blocks.%d.linear1.weight", b); snprintf(names[n++], 160, "single_blocks.%d.linear2.weight", b); }
    return n;
}
static uint64_t fxl_rng(uint64_t *s) { uint64_t z = (*s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }

/* adaptador novo (A aleatoria, B zero) sobre o modelo aberto F */
static int fxl_create(const FxDit *F, int rank, float alpha, uint64_t seed) {
    static char names[FXD_NDBL * 8 + FXD_NSGL * 2][160];
    const int n = fxl_names(names);
    FxLora *L = &g_fxl_store; memset(L, 0, sizeof *L);
    L->m = xalloc((size_t)n, sizeof(FxlMod)); L->n = n; L->scale = 1.0f;
    L->base_size = (uint64_t)F->g.size; L->base_hash = fxl_base_hash(&F->g);
    for (int i = 0; i < n; i++) {
        FxlMod *m = &L->m[i];
        snprintf(m->name, sizeof m->name, "%s", names[i]);
        m->t = sdg_find(&F->g, m->name);
        if (!m->t) { fprintf(stderr, "flux-lora: tensor %s nao existe\n", m->name); return 0; }
        m->cols = (int)m->t->dim[0]; m->rows = (int)m->t->dim[1]; m->r = rank; m->s = alpha / (float)rank;
        m->A = xalloc((size_t)rank * m->cols, 4); m->B = xalloc((size_t)m->rows * rank, 4);
        const float sc = 1.0f / sqrtf((float)m->cols);
        for (size_t j = 0; j < (size_t)rank * m->cols; j++) m->A[j] = ((float)((fxl_rng(&seed) >> 40) & 0xFFFFFF) / 16777215.0f * 2.0f - 1.0f) * sc;
    }
    return 1;
}

static int fxl_save(const char *path) {
    const FxLora *L = &g_fxl_store;
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    fwrite("FXLORA01", 1, 8, f); fwrite(&L->base_size, 8, 1, f); fwrite(&L->base_hash, 8, 1, f); const uint32_t n = (uint32_t)L->n; fwrite(&n, 4, 1, f);
    for (int i = 0; i < L->n; i++) {
        const FxlMod *m = &L->m[i];
        const uint32_t nl = (uint32_t)strlen(m->name); const uint64_t r = (uint64_t)m->rows, c = (uint64_t)m->cols; const int32_t rk = m->r;
        fwrite(&nl, 4, 1, f); fwrite(m->name, 1, nl, f); fwrite(&r, 8, 1, f); fwrite(&c, 8, 1, f); fwrite(&rk, 4, 1, f); fwrite(&m->s, 4, 1, f);
        fwrite(m->A, 4, (size_t)m->r * m->cols, f); fwrite(m->B, 4, (size_t)m->rows * m->r, f);
    }
    { const uint32_t fl = 0; fwrite("KSXF", 1, 4, f); fwrite(&fl, 4, 1, f); }
    const int ok = !ferror(f);
    return fclose(f) == 0 && ok && rename(tmp, path) == 0;
}

/* carrega, confere o GGUF base e ativa (g_fxl_on). 0 = falhou (mensagem em stderr) */
static int fxl_load(const FxDit *F, const char *path, float scale) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "flux-lora: nao abriu %s\n", path); return 0; }
    char mg[8]; uint64_t bs = 0, bh = 0; uint32_t n = 0;
    if (fread(mg, 1, 8, f) != 8 || memcmp(mg, "FXLORA01", 8) || fread(&bs, 8, 1, f) != 1 || fread(&bh, 8, 1, f) != 1 || fread(&n, 4, 1, f) != 1 || n > 4096) {
        fprintf(stderr, "flux-lora: %s nao e um arquivo .fxlora valido\n", path); fclose(f); return 0;
    }
    if (bs != (uint64_t)F->g.size || bh != fxl_base_hash(&F->g)) { fprintf(stderr, "flux-lora: %s foi treinado sobre outro GGUF base\n", path); fclose(f); return 0; }
    FxLora *L = &g_fxl_store; memset(L, 0, sizeof *L);
    L->m = xalloc((size_t)n, sizeof(FxlMod)); L->n = (int)n; L->scale = scale; L->base_size = bs; L->base_hash = bh;
    for (uint32_t i = 0; i < n; i++) {
        FxlMod *m = &L->m[i];
        uint32_t nl = 0; uint64_t r = 0, c = 0; int32_t rk = 0;
        if (fread(&nl, 4, 1, f) != 1 || nl >= sizeof m->name || fread(m->name, 1, nl, f) != nl) goto bad;
        m->name[nl] = 0;
        if (fread(&r, 8, 1, f) != 1 || fread(&c, 8, 1, f) != 1 || fread(&rk, 4, 1, f) != 1 || fread(&m->s, 4, 1, f) != 1 || rk < 1 || rk > 1024) goto bad;
        m->t = sdg_find(&F->g, m->name);
        if (!m->t || (uint64_t)m->t->dim[0] != c || (uint64_t)m->t->dim[1] != r) { fprintf(stderr, "flux-lora: %s nao casa com o modelo\n", m->name); goto bad; }
        m->rows = (int)r; m->cols = (int)c; m->r = rk;
        m->A = xalloc((size_t)rk * c, 4); m->B = xalloc((size_t)r * rk, 4);
        if (fread(m->A, 4, (size_t)rk * c, f) != (size_t)rk * c || fread(m->B, 4, (size_t)r * rk, f) != (size_t)r * rk) goto bad;
    }
    fclose(f);
    g_fxl_on = 1;
    return 1;
bad:
    fprintf(stderr, "flux-lora: arquivo %s truncado ou invalido\n", path); fclose(f); memset(&g_fxl_store, 0, sizeof g_fxl_store); return 0;
}

/* estado do otimizador */
static int fxl_opt_save(const char *path, int64_t step) {
    char p[1250], tmp[1260]; snprintf(p, sizeof p, "%s.kopt", path); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    const int32_t n = g_fxl_store.n;
    fwrite("FXOPT001", 1, 8, f); fwrite(&step, 8, 1, f); fwrite(&n, 4, 1, f);
    for (int i = 0; i < n; i++) {
        const FxlMod *m = &g_fxl_store.m[i]; const size_t na = (size_t)m->r * m->cols, nb = (size_t)m->rows * m->r;
        fwrite(m->mA1, 4, na, f); fwrite(m->mA2, 4, na, f); fwrite(m->mB1, 4, nb, f); fwrite(m->mB2, 4, nb, f);
    }
    const int ok = !ferror(f);
    return fclose(f) == 0 && ok && rename(tmp, p) == 0;
}
static int64_t fxl_opt_load(const char *path) {
    char p[1250]; snprintf(p, sizeof p, "%s.kopt", path);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    char mg[8]; int64_t step = 0; int32_t n = 0;
    if (fread(mg, 1, 8, f) != 8 || memcmp(mg, "FXOPT001", 8) || fread(&step, 8, 1, f) != 1 || fread(&n, 4, 1, f) != 1 || n != g_fxl_store.n) { fclose(f); return -1; }
    for (int i = 0; i < n; i++) {
        const FxlMod *m = &g_fxl_store.m[i]; const size_t na = (size_t)m->r * m->cols, nb = (size_t)m->rows * m->r;
        if (fread(m->mA1, 4, na, f) != na || fread(m->mA2, 4, na, f) != na || fread(m->mB1, 4, nb, f) != nb || fread(m->mB2, 4, nb, f) != nb) { fclose(f); return -1; }
    }
    fclose(f);
    return step;
}
