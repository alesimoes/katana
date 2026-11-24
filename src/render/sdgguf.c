/* ============================ render: leitor de GGUF ===============================
 * Abre um GGUF gravado por `katana render convert` (docs/render.md): mapeia o arquivo, le a tabela de tensores e acha tensores e
 * chaves por nome. Os dados ficam no arquivo mapeado (nada e copiado). Dimensoes na ordem do GGUF (ne0 = a mais interna). */
typedef struct { char name[160]; int nd; uint64_t dim[4]; uint32_t type; uint64_t off; } SdgInfo;
typedef struct { const uint8_t *base; int64_t size; const uint8_t *kv; uint64_t nkv; uint64_t data0; int n; SdgInfo *t; } SdgFile;

static uint32_t sdg_u32(const uint8_t **p) { uint32_t v; memcpy(&v, *p, 4); *p += 4; return v; }
static uint64_t sdg_u64(const uint8_t **p) { uint64_t v; memcpy(&v, *p, 8); *p += 8; return v; }
static void sdg_str(const uint8_t **p, char *out, size_t cap) {
    const uint64_t n = sdg_u64(p);
    const size_t c = n < cap - 1 ? (size_t)n : cap - 1;
    if (out) { memcpy(out, *p, c); out[c] = 0; }
    *p += n;
}
/* pula um valor do tipo t (tipos do GGUF: 0-1 8 bits, 2-3 16, 4-6 32, 7 bool, 8 texto, 9 vetor, 10-12 64) */
static void sdg_skip(const uint8_t **p, uint32_t t) {
    static const int sz[13] = { 1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8 };
    if (t == 8) { sdg_str(p, NULL, 1); return; }
    if (t == 9) { const uint32_t et = sdg_u32(p); const uint64_t n = sdg_u64(p); for (uint64_t i = 0; i < n; i++) sdg_skip(p, et); return; }
    if (t < 13) *p += sz[t];
}
static int sdg_cmp(const void *a, const void *b) { return strcmp(((const SdgInfo *)a)->name, ((const SdgInfo *)b)->name); }

static int sdg_open(SdgFile *g, const char *path) {
    memset(g, 0, sizeof *g);
    const uint8_t *b = map_file(path, &g->size);
    if (!b || g->size < 24 || memcmp(b, "GGUF", 4) != 0) { fprintf(stderr, "render: %s nao e um GGUF\n", path); return 0; }
    g->base = b;
    const uint8_t *p = b + 4;
    if (sdg_u32(&p) != 3) { fprintf(stderr, "render: %s: so a versao 3 do GGUF\n", path); return 0; }
    const uint64_t nt = sdg_u64(&p); g->nkv = sdg_u64(&p);
    g->kv = p;
    char key[160];
    for (uint64_t i = 0; i < g->nkv; i++) { sdg_str(&p, key, sizeof key); sdg_skip(&p, sdg_u32(&p)); }
    g->t = xalloc((size_t)nt, sizeof(SdgInfo));
    for (uint64_t i = 0; i < nt; i++) {
        SdgInfo *t = &g->t[i];
        sdg_str(&p, t->name, sizeof t->name);
        t->nd = (int)sdg_u32(&p);
        if (t->nd < 1 || t->nd > 4) { fprintf(stderr, "render: tensor %s com %d dimensoes\n", t->name, t->nd); return 0; }
        for (int d = 0; d < t->nd; d++) t->dim[d] = sdg_u64(&p);
        t->type = sdg_u32(&p); t->off = sdg_u64(&p);
    }
    g->n = (int)nt;
    g->data0 = ((uint64_t)(p - b) + 31) / 32 * 32;
    qsort(g->t, (size_t)g->n, sizeof(SdgInfo), sdg_cmp);
    return 1;
}
static const SdgInfo *sdg_find(const SdgFile *g, const char *name) {
    SdgInfo key; snprintf(key.name, sizeof key.name, "%s", name);
    return bsearch(&key, g->t, (size_t)g->n, sizeof(SdgInfo), sdg_cmp);
}
static const uint8_t *sdg_data(const SdgFile *g, const SdgInfo *t) { return g->base + g->data0 + t->off; }
static uint64_t sdg_elems(const SdgInfo *t) { uint64_t n = 1; for (int d = 0; d < t->nd; d++) n *= t->dim[d]; return n; }

/* um tensor inteiro como float (memoria nova; o chamador libera). NULL se nao existe ou se o tipo nao e F32/F16 */
static float *sdg_floats(const SdgFile *g, const char *name, uint64_t *n_out) {
    const SdgInfo *t = sdg_find(g, name);
    if (!t) { fprintf(stderr, "render: tensor %s nao existe\n", name); return NULL; }
    const uint64_t n = sdg_elems(t);
    float *f = xalloc((size_t)n, sizeof(float));
    const uint8_t *d = sdg_data(g, t);
    if (t->type == 0) memcpy(f, d, (size_t)n * 4);
    else if (t->type == 1) { for (uint64_t i = 0; i < n; i++) { uint16_t h; memcpy(&h, d + 2 * i, 2); f[i] = f16_to_f32(h); } }
    else { fprintf(stderr, "render: tensor %s com tipo %u (so F32 e F16)\n", name, t->type); free(f); return NULL; }
    if (n_out) *n_out = n;
    return f;
}
