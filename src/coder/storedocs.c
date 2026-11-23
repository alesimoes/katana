/* ---------- catalogo generico de documentos sobre o armazenamento (src/coder/store.c) e os subcomandos de manutencao ----------
 * Generico: "documento" e qualquer arquivo da pasta; o tipo/idioma e um inteiro opaco (kind) que quem chama atribui; nada aqui conhece codigo.
 * Secao STS_DOCS (versao 1): u32 n, u32 largura (40), n registros ordenados por caminho (bytes), depois o pool de caminhos. Registro:
 *   0 u32 offset do caminho no pool  4 u32 tamanho do caminho  8 u64 tamanho do arquivo  16 i64 mtime (ns)  24 u64 hash do conteudo
 *   32 u32 kind  36 u32 flags
 * Secao STS_TOMBS (versao 1): tabela de strings ordenada com os caminhos removidos.
 * Regra de vigencia: o segmento mais novo que cita um caminho (documento ou tombstone) decide; um documento novo substitui o antigo sem
 * precisar de tombstone. Subcomandos: katana coder index | status | verify | compact [pasta]. */
#ifdef _WIN32
#define lstat stat
#endif
#define SDF_SKIP_BINARY 1u      /* flags do documento: catalogado mas nao lido (binario) */
#define SDF_SKIP_BIG 2u         /* maior que o limite configuravel */
#define SDF_SKIP_UNREADABLE 4u
#define SDF_SKIP_SECRET 32u     /* arquivo de segredo (coder/ignore.c): catalogado so pelo nome, conteudo nunca lido */
#define SDF_GENERATED 16u       /* gerado/minificado: so caminho e nome entram na busca, sem simbolos (coder/crawl.c, KATANA_INDEX_GENERATED=1) */
/* bits 8..11 de flags = nivel de informacao que o rastreador (crawl.c) ja deu ao documento: 0 = completo (caminho sincrono e indices antigos: busca e simbolos
 * de uma vez), 1 = N0 (so catalogo: nome, tamanho, data), 2 = N1 (texto), 4 = N2 e N3 (simbolos, referencias e chaves, saem da mesma analise) */
#define SDF_LVMASK 0xF00u
#define SDF_SKIPMASK (SDF_SKIP_BINARY | SDF_SKIP_BIG | SDF_SKIP_UNREADABLE | SDF_SKIP_SECRET)
static uint32_t sdf_lv(uint32_t f) { return (f & SDF_LVMASK) >> 8; }
static int sdf_partial(uint32_t f) { return sdf_lv(f) != 0 && sdf_lv(f) < 4; }       /* ainda falta nivel (ate o nome de binario e grande entra na busca no N1): a atualizacao sincrona refaz o documento */
#define STORE_DOC_W 40

typedef struct { const char *path; uint32_t plen; uint64_t size; int64_t mtime; uint64_t hash; uint32_t kind, flags; } StoreDoc;

/* ---- secao de documentos ---- */
static void store_docs_build(StoreBuf *b, const StoreDoc *d, uint32_t n) {     /* d ja ordenado por caminho */
    store_table_begin(b, n, STORE_DOC_W);
    uint32_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        sb_u32(b, off); sb_u32(b, d[i].plen); sb_u64(b, d[i].size); sb_u64(b, (uint64_t)d[i].mtime); sb_u64(b, d[i].hash); sb_u32(b, d[i].kind); sb_u32(b, d[i].flags);
        off += d[i].plen;
    }
    for (uint32_t i = 0; i < n; i++) sb_bytes(b, d[i].path, d[i].plen);
}
typedef struct { StoreTable t; const uint8_t *pool; uint64_t poolsize; } StoreDocs;
static int store_docs_open(const StoreSection *s, StoreDocs *d) {
    memset(d, 0, sizeof *d);
    if (!s || !store_table_open(s, &d->t) || d->t.w != STORE_DOC_W) { d->t.n = 0; return 0; }
    uint64_t head = 8 + (uint64_t)d->t.n * d->t.w;
    d->pool = s->data + head; d->poolsize = s->size - head;
    return 1;
}
static int store_docs_get(const StoreDocs *d, uint32_t i, StoreDoc *o) {
    const uint8_t *r = store_table_at(&d->t, i);
    uint32_t off = st_get_u32(r), len = st_get_u32(r + 4);
    if ((uint64_t)off + len > d->poolsize) return 0;
    o->path = (const char *)d->pool + off; o->plen = len; o->size = st_get_u64(r + 8); o->mtime = (int64_t)st_get_u64(r + 16);
    o->hash = st_get_u64(r + 24); o->kind = st_get_u32(r + 32); o->flags = st_get_u32(r + 36);
    return 1;
}
static int64_t store_docs_find(const StoreDocs *d, const char *path, size_t len) {
    uint32_t lo = 0, hi = d->t.n;
    while (lo < hi) {
        uint32_t m = lo + (hi - lo) / 2; StoreDoc x;
        if (!store_docs_get(d, m, &x)) return -1;
        int c = st_cmp_bytes(x.path, x.plen, path, len);
        if (c == 0) return m;
        if (c < 0) lo = m + 1; else hi = m;
    }
    return -1;
}

/* ---- vista vigente: junta os segmentos do mais antigo ao mais novo (documentos sobrescrevem, tombstones removem) ---- */
typedef struct { StoreDoc d; int seg; uint32_t idx; int dead, seen; } StoreLiveDoc;
typedef struct { StoreLiveDoc *e; int n; int *tab; uint32_t tabn; } StoreLive;
static StoreLiveDoc *store_live_find(const StoreLive *L, const char *path, size_t len) {
    uint32_t h = (uint32_t)store_hash64(path, len, 7) & (L->tabn - 1);
    while (L->tab[h] >= 0) {
        StoreLiveDoc *e = &L->e[L->tab[h]];
        if (e->d.plen == len && !memcmp(e->d.path, path, len)) return e;
        h = (h + 1) & (L->tabn - 1);
    }
    return NULL;
}
static StoreLiveDoc *store_live_put(StoreLive *L, const char *path, size_t len) {
    StoreLiveDoc *f = store_live_find(L, path, len);
    if (f) return f;
    uint32_t h = (uint32_t)store_hash64(path, len, 7) & (L->tabn - 1);
    while (L->tab[h] >= 0) h = (h + 1) & (L->tabn - 1);
    L->tab[h] = L->n;
    StoreLiveDoc *e = &L->e[L->n++]; memset(e, 0, sizeof *e);
    e->d.path = path; e->d.plen = (uint32_t)len;
    return e;
}
static void store_live_build(StoreLive *L, const StoreSeg *const *seg, int nseg) {
    uint64_t total = 1;
    for (int s = 0; s < nseg; s++) for (uint32_t i = 0; i < seg[s]->nsec; i++) if (seg[s]->sec[i].type == STS_DOCS || seg[s]->sec[i].type == STS_TOMBS) total += seg[s]->sec[i].nrec;
    memset(L, 0, sizeof *L);
    L->e = (StoreLiveDoc *)calloc((size_t)total, sizeof *L->e);
    L->tabn = 16; while (L->tabn < total * 2) L->tabn *= 2;
    L->tab = (int *)malloc(sizeof(int) * L->tabn);
    for (uint32_t i = 0; i < L->tabn; i++) L->tab[i] = -1;
    for (int s = 0; s < nseg; s++) {
        StoreDocs dc; StoreStrs ts;
        if (store_docs_open(store_seg_section(seg[s], STS_DOCS), &dc))
            for (uint32_t i = 0; i < dc.t.n; i++) {
                StoreDoc d; if (!store_docs_get(&dc, i, &d)) continue;
                StoreLiveDoc *e = store_live_put(L, d.path, d.plen);
                e->d = d; e->seg = s; e->idx = i; e->dead = 0;
            }
        if (store_strs_open(store_seg_section(seg[s], STS_TOMBS), &ts))
            for (uint32_t i = 0; i < ts.n; i++) {
                uint32_t l; const char *p = store_strs_get(&ts, i, &l);
                StoreLiveDoc *e = store_live_put(L, p, l);
                e->dead = 1; e->seg = s;
            }
    }
}
static void store_live_free(StoreLive *L) { free(L->e); free(L->tab); memset(L, 0, sizeof *L); }
/* consulta restrita ao prefixo `pfx` (sem barra final) de uma raiz que o absorve: fica so o que esta debaixo dele, com o caminho relativo ao prefixo
 * (as saidas mostram o mesmo que quando a pasta era a propria raiz); a tabela de busca e refeita para os caminhos novos */
static void store_live_rebase(StoreLive *L, const char *pfx, size_t pl) {
    int n = 0;
    for (int i = 0; i < L->n; i++) {
        StoreLiveDoc *e = &L->e[i];
        if (e->dead || e->d.plen <= pl + 1 || memcmp(e->d.path, pfx, pl) != 0 || e->d.path[pl] != '/') continue;
        L->e[n] = *e; L->e[n].d.path += pl + 1; L->e[n].d.plen -= (uint32_t)(pl + 1); n++;
    }
    L->n = n;
    for (uint32_t i = 0; i < L->tabn; i++) L->tab[i] = -1;
    for (int i = 0; i < n; i++) {
        uint32_t h = (uint32_t)store_hash64(L->e[i].d.path, L->e[i].d.plen, 7) & (L->tabn - 1);
        while (L->tab[h] >= 0) h = (h + 1) & (L->tabn - 1);
        L->tab[h] = i;
    }
}
/* inclusao restrita por `path` (D.10): os documentos debaixo de uma linha `scoped <pasta>` do arquivo overrides so aparecem para consultas cujo prefixo cai
 * dentro dela; as demais (busca normal do projeto) nao os veem. Chamar ANTES de store_live_rebase, com os caminhos ainda relativos a raiz.
 * qprefix = prefixo da consulta relativo a raiz ("" = projeto inteiro). Devolve quantos documentos escondeu. Quem monta a visao (fase 6) liga isto. */
static int store_live_hide_scoped(StoreLive *L, const IgnOvr *O, const char *qprefix) {
    int hidden = 0; const size_t ql = strlen(qprefix);
    if (!O) return 0;
    for (int k = 0; k < O->n; k++) {
        const IgnOvrEnt *x = &O->e[k]; if (!x->scoped) continue;
        if (ql >= x->pl && !memcmp(qprefix, x->p, x->pl) && (ql == x->pl || qprefix[x->pl] == '/')) continue;       /* a consulta esta dentro da pasta: ve tudo */
        for (int i = 0; i < L->n; i++) { StoreLiveDoc *e = &L->e[i];
            if (!e->dead && e->d.plen > x->pl && !memcmp(e->d.path, x->p, x->pl) && e->d.path[x->pl] == '/') { e->dead = 1; hidden++; } }
    }
    return hidden;
}
static int st_cmp_live(const void *a, const void *b) {
    const StoreLiveDoc *x = *(const StoreLiveDoc *const *)a, *y = *(const StoreLiveDoc *const *)b;
    return st_cmp_bytes(x->d.path, x->d.plen, y->d.path, y->d.plen);
}
/* ponteiros para as entradas (vivas ou mortas), ordenados por caminho */
static StoreLiveDoc **store_live_sorted(const StoreLive *L, int want_dead, int *n) {
    StoreLiveDoc **v = (StoreLiveDoc **)malloc(sizeof *v * (size_t)(L->n + 1)); int k = 0;
    for (int i = 0; i < L->n; i++) if (L->e[i].dead == want_dead) v[k++] = &L->e[i];
    qsort(v, (size_t)k, sizeof *v, st_cmp_live);
    *n = k; return v;
}

/* ---- fusores da compactacao ---- */
static int store_merge_docs(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) {
    StoreLive L; store_live_build(&L, c->seg, c->nseg);
    int n; StoreLiveDoc **v = store_live_sorted(&L, 0, &n);
    for (int s = 0; s < c->nseg; s++) {
        const StoreSection *sc = store_seg_section(c->seg[s], STS_DOCS);
        uint32_t cnt = 0; StoreDocs dc;
        if (store_docs_open(sc, &dc)) cnt = dc.t.n;
        c->docmap[s] = (uint32_t *)malloc(sizeof(uint32_t) * (cnt + 1));
        for (uint32_t i = 0; i < cnt; i++) c->docmap[s][i] = UINT32_MAX;
    }
    StoreDoc *d = (StoreDoc *)malloc(sizeof *d * (size_t)(n + 1));
    for (int i = 0; i < n; i++) { d[i] = v[i]->d; c->docmap[v[i]->seg][v[i]->idx] = (uint32_t)i; }
    store_docs_build(out, d, (uint32_t)n);
    *nrec = (uint32_t)n;
    free(d); free(v); store_live_free(&L);
    return 0;
}
static int store_merge_tombs(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) {
    *nrec = 0;
    if (!c->keep_tombs) return 0;
    StoreLive L; store_live_build(&L, c->seg, c->nseg);
    int n; StoreLiveDoc **v = store_live_sorted(&L, 1, &n);
    if (n) {
        const char **s = (const char **)malloc(sizeof *s * (size_t)n); uint32_t *l = (uint32_t *)malloc(sizeof *l * (size_t)n);
        for (int i = 0; i < n; i++) { s[i] = v[i]->d.path; l[i] = v[i]->d.plen; }
        store_strs_build(out, s, l, (uint32_t)n); *nrec = (uint32_t)n;
        free(s); free(l);
    }
    free(v); store_live_free(&L);
    return 0;
}
static void coder_search_register(void);       /* search.c: fusores das secoes de busca */
static void coder_sym_register(void);          /* symindex.c: fusores das secoes de simbolos */
static void coder_crawl_register(void);        /* crawl.c: fusores de STS_DIRS e STS_CRAWL */
static void store_catalog_init(void) {
    store_register_merger(STS_DOCS, 1, store_merge_docs);
    store_register_merger(STS_TOMBS, 1, store_merge_tombs);
    coder_search_register();
    coder_sym_register();
    coder_crawl_register();
}

/* ---- varredura da pasta ---- */
typedef struct { char *path; uint32_t plen; uint64_t size; int64_t mtime; } StoreScanEnt;
/* regras de ignorar (.gitignore e .katanaignore). Suportado: linhas em branco e # ignoradas, ! de negacao, / final (so pastas), / inicial ou
 * no meio (ancorado ao diretorio do arquivo de regras), * ? ** [abc] [a-z] [!abc], \ de escape, .gitignore em subpastas (o mais fundo vence;
 * dentro de um arquivo a ultima regra vence). NAO suportado: .git/info/exclude, ~/.config/git/ignore, core.excludesFile, classes [[:alpha:]],
 * re-incluir arquivo dentro de pasta ja ignorada (igual ao git), espacos finais protegidos por barra, casamento insensivel a maiusculas. */
typedef struct { char *pat; int neg, dironly, anchored; int src, line; char *orig; } StoreRule;      /* src: 1 = .gitignore, 2 = .katanaignore, 0 = lista interna; orig = linha original (para dizer arquivo:linha) */
typedef struct { StoreRule *r; int n, cap; size_t base; } StoreRuleSet;
#define ST_MAXRS 64
/* destino opcional dos registros de ignorados (passada de contagem, maint.c): chamado para cada pasta/arquivo deixado de fora, com o motivo */
typedef struct { void *ctx; void (*fn)(void *ctx, const char *rel, int isdir, int reason, const char *rule, uint64_t size, const char *abs); uint64_t maxsz; } IgnSink;
typedef struct {
    StoreScanEnt *e; int n, cap; StoreRuleSet rs[ST_MAXRS]; int nrs; int ignored, ignored_heur, has_gitignore;
    const IgnOvr *ovr; IgnSink *sink; int nolist;           /* overrides; destino dos ignorados; nolist = nao listar os arquivos (so contar o que fica de fora) */
    int mode, pass_reason; char pass_why[300];               /* mode: 0 normal, 1 passagem (pasta ignorada com um include dentro), 2 forcado (dentro de um include) */
} StoreScan;
typedef struct {
    uint64_t max_size;                                          /* arquivos maiores sao catalogados sem leitura (SDF_SKIP_BIG); 0 = 2 MiB */
    uint32_t (*kind_fn)(const char *path, size_t len);          /* tipo/idioma opaco por caminho; NULL = 0 */
    int max_segs;                                               /* compactacao automatica acima disso; 0 = 8 */
    /* ganchos de quem indexa conteudo (busca): doc_fn e chamado para CADA documento novo/alterado, na ordem do segmento (idx = posicao na secao
     * de documentos), com data/n = conteudo lido (NULL se binario, grande ou ilegivel); commit_fn roda antes de gravar o segmento e acrescenta
     * as secoes proprias ao escritor. Arquivos iguais nao sao relidos nem reindexados. */
    void *hook_ctx;
    void (*doc_fn)(void *ctx, uint32_t idx, const StoreDoc *d, const uint8_t *data, size_t n);
    void (*commit_fn)(void *ctx, StoreWriter *w, uint32_t ndocs);
} StoreScanOpts;
typedef struct { int scanned, unchanged, added, changed, touched, removed, skip_bin, skip_big, skip_unread, skip_secret, live, compacted, ignored, ignored_heur; int64_t gen; double t_scan, t_total; } StoreUpdStats;
static void st_rules_add(StoreRuleSet *R, const char *line, size_t len) {
    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ' || line[len - 1] == '\t')) len--;
    if (!len || line[0] == '#') return;
    StoreRule x; memset(&x, 0, sizeof x);
    x.orig = (char *)malloc(len + 1); memcpy(x.orig, line, len); x.orig[len] = 0;
    if (line[0] == '!') { x.neg = 1; line++; len--; }
    else if (line[0] == '\\' && len > 1 && (line[1] == '#' || line[1] == '!')) { line++; len--; }
    if (len && line[len - 1] == '/') { x.dironly = 1; len--; }
    if (!len) { free(x.orig); return; }
    size_t k = 0; for (size_t i = 0; i < len; i++) if (line[i] == '/') k++;
    x.anchored = k > 0;
    if (line[0] == '/') { line++; len--; if (!len) { free(x.orig); return; } }
    x.pat = (char *)malloc(len + 1); memcpy(x.pat, line, len); x.pat[len] = 0;
    if (R->n == R->cap) { R->cap = R->cap ? R->cap * 2 : 16; R->r = (StoreRule *)realloc(R->r, sizeof *R->r * (size_t)R->cap); }
    R->r[R->n++] = x;
}
static void st_rules_load(StoreRuleSet *R, const char *file) {
    FILE *f = fopen(file, "rb");
    if (!f) return;
    char ln[2048]; int lineno = 0; const int src = strstr(file, ".katanaignore") ? 2 : 1;
    while (fgets(ln, sizeof ln, f)) { const int before = R->n; lineno++; st_rules_add(R, ln, strlen(ln)); if (R->n > before) { R->r[R->n - 1].src = src; R->r[R->n - 1].line = lineno; } }
    fclose(f);
}
static int st_glob(const char *p, const char *s) {
    while (*p) {
        if (*p == '*') {
            int dbl = p[1] == '*';
            while (*p == '*') p++;
            if (dbl) {
                if (*p == '/' && st_glob(p + 1, s)) return 1;               /* "**" + "/" casa zero pastas */
                for (const char *t = s; ; t++) { if (st_glob(p, t)) return 1; if (!*t) return 0; }
            }
            if (!*p) return strchr(s, '/') == NULL;
            for (const char *t = s; ; t++) { if (st_glob(p, t)) return 1; if (!*t || *t == '/') return 0; }
        }
        if (!*s) return 0;
        if (*p == '?') { if (*s == '/') return 0; p++; s++; continue; }
        if (*p == '[') {
            const char *q = p + 1; int neg = 0, ok = 0;
            if (*q == '!' || *q == '^') { neg = 1; q++; }
            const char *first = q;
            while (*q && (*q != ']' || q == first)) {
                if (q[1] == '-' && q[2] && q[2] != ']') { if ((unsigned char)*s >= (unsigned char)q[0] && (unsigned char)*s <= (unsigned char)q[2]) ok = 1; q += 3; }
                else { if (*q == *s) ok = 1; q++; }
            }
            if (*q != ']') { if (*s != '[') return 0; p++; s++; continue; }       /* colchete sem fim: literal */
            if (*s == '/' || ok == neg) return 0;
            p = q + 1; s++; continue;
        }
        if (*p == '\\' && p[1]) p++;
        if (*p != *s) return 0;
        p++; s++;
    }
    return *s == 0;
}
/* 1 = ignorado, 0 = reincluido (!), -1 = nenhuma regra casou; rel = caminho relativo ao diretorio das regras, nm = ultimo componente */
static int st_rules_match(const StoreRuleSet *R, const char *rel, const char *nm, int isdir) {
    for (int i = R->n - 1; i >= 0; i--) {
        const StoreRule *x = &R->r[i];
        if (x->dironly && !isdir) continue;
        if (st_glob(x->pat, x->anchored ? rel : nm)) return x->neg ? 0 : 1;
    }
    return -1;
}
static int st_scan_ignored(const StoreScan *S, const char *path, const char *nm, int isdir) {
    for (int i = S->nrs - 1; i >= 0; i--) {
        int m = st_rules_match(&S->rs[i], path + S->rs[i].base + 1, nm, isdir);
        if (m >= 0) return m;
    }
    return 0;
}
static int st_count_files_capped(char *path, size_t plen, int cap) {                 /* conta arquivos (sem .git) ate passar de cap */
    DIR *d = opendir(path); if (!d) return 0;
    struct dirent *de; int n = 0;
    while (n <= cap && (de = readdir(d))) {
        const char *nm = de->d_name; if (!strcmp(nm, ".") || !strcmp(nm, "..") || !strcmp(nm, ".git")) continue;
        size_t nl = strlen(nm); if (plen + nl + 2 >= 4000) continue;
        path[plen] = '/'; memcpy(path + plen + 1, nm, nl + 1);
        struct stat st;
        if (lstat(path, &st) == 0) { if (S_ISDIR(st.st_mode)) n += st_count_files_capped(path, plen + 1 + nl, cap - n); else n++; }
        path[plen] = 0;
    }
    closedir(d); return n;
}
/* a lista interna unica de pastas pesadas esta em ignore.c; aqui so a consulta por nome (usada tambem pelo rastreador) */
static int st_ignored_dir(const char *n) { return ign_heavy(n) != NULL; }
static int st_binary_ext(const char *n) {       /* binario por extensao; arquivo de segredo conta como "nao ler" (o motivo "secret" vem de ign_secret_name) */
    static const char *const ex[] = {"png", "jpg", "jpeg", "gif", "webp", "ico", "bmp", "pdf", "zip", "gz", "tgz", "xz", "bz2", "7z", "rar", "o", "a", "so", "dll", "exe", "obj", "lib", "class", "jar", "bin", "gguf", "safetensors", "pt", "mp3", "mp4", "wav", "ogg", "mov", "avi", "ttf", "otf", "woff", "woff2", "pyc", "dylib", "dat", "sqlite", "db", NULL};
    if (ign_secret_name(n)) return 1;
    const char *dot = strrchr(n, '.'), *sl = strrchr(n, '/');
    if (!dot || (sl && dot < sl)) return 0;
    for (int i = 0; ex[i]; i++) if (!strcasecmp(dot + 1, ex[i])) return 1;
    return 0;
}
/* 1 = ignorada por regra de arquivo, 0 = reincluida por "!", -1 = nenhuma regra casou; devolve a regra e o conjunto que decidiram */
static int st_scan_rule(const StoreScan *S, const char *path, const char *nm, int isdir, const StoreRule **rr, const StoreRuleSet **rs) {
    for (int i = S->nrs - 1; i >= 0; i--) {
        const StoreRuleSet *R = &S->rs[i]; const char *rel = path + R->base + 1;
        for (int k = R->n - 1; k >= 0; k--) {
            const StoreRule *x = &R->r[k];
            if (x->dironly && !isdir) continue;
            if (st_glob(x->pat, x->anchored ? rel : nm)) { *rr = x; *rs = R; return x->neg ? 0 : 1; }
        }
    }
    return -1;
}
/* "sub/.gitignore:4 \"build/\"": arquivo (relativo a raiz), linha e a linha original da regra */
static void st_rule_text(const StoreRule *x, const StoreRuleSet *R, const char *path, size_t rel0, char *out, size_t cap) {
    if (!x->src) { snprintf(out, cap, "built-in rule \"%s\"", x->orig ? x->orig : x->pat); return; }
    char d[300] = "";
    if (R->base > rel0) { size_t n = R->base - rel0; if (n >= sizeof d) n = sizeof d - 1; memcpy(d, path + rel0, n); d[n] = 0; }
    snprintf(out, cap, "%s%s%s:%d \"%s\"", d, d[0] ? "/" : "", x->src == 2 ? ".katanaignore" : ".gitignore", x->line, x->orig ? x->orig : x->pat);
}
static void st_sink(StoreScan *S, const char *path, size_t rel0, int isd, int reason, const char *rule, uint64_t size) {
    if (S->sink && S->sink->fn) S->sink->fn(S->sink->ctx, path + rel0, isd, reason, rule, size, path);
}
/* varredura da raiz. Precedencia (ignore.c): overrides > .katanaignore > .gitignore > lista interna. Cada coisa deixada de fora e dita ao S->sink com o motivo */
static void st_scan_walk(StoreScan *S, char *path, size_t plen, size_t rel0) {
    DIR *d = opendir(path);
    if (!d) return;
    int pushed = 0, isroot = (S->nrs == 0);
    if (S->nrs < ST_MAXRS) {
        StoreRuleSet *R = &S->rs[S->nrs]; memset(R, 0, sizeof *R); R->base = plen;
        char f[4200];
        snprintf(f, sizeof f, "%s/.gitignore", path);
        st_rules_load(R, f);
        if (isroot) S->has_gitignore = R->n > 0;
        snprintf(f, sizeof f, "%s/.katanaignore", path); st_rules_load(R, f);
        if (R->n) { S->nrs++; pushed = 1; } else free(R->r);
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        size_t nl = strlen(nm);
        if (plen + 1 + nl + 1 >= 4000) continue;
        size_t at = plen;
        path[at++] = '/';
        memcpy(path + at, nm, nl + 1);
        struct stat st;
        if (lstat(path, &st) == 0) {
            const int isd = S_ISDIR(st.st_mode);
            const IgnHeavy *hv = isd ? ign_heavy(nm) : NULL;
            if ((!isd && !S_ISREG(st.st_mode)) || (hv && hv->group == IGG_VCS)) { path[plen] = 0; continue; }      /* links, soquetes; .git e afins nunca entram nem sao contados */
            const char *rel = path + rel0;
            const int ov = S->ovr ? ign_ovr_state(S->ovr, rel, isd) : IGO_NONE;
            int ign = 0, reason = 0, nmode = S->mode; char rule[420]; rule[0] = 0;
            if (hv && !ign_group_includable(hv->group)) { ign = 1; reason = IGR_SECRET; snprintf(rule, sizeof rule, "secret folder \"%s\"", nm); }
            else if (S->mode == 1) {                 /* dentro de pasta ignorada que tem um include: so entra o que leva ate ele */
                if (ov == IGO_INCLUDE) nmode = 2; else if (ov == IGO_PASS) nmode = 1;
                else { ign = 1; reason = S->pass_reason; snprintf(rule, sizeof rule, "%s", S->pass_why); }
            } else if (S->mode == 0) {
                if (ov == IGO_EXCLUDE) { ign = 1; reason = IGR_EXCLUDED; snprintf(rule, sizeof rule, "excluded with katana coder exclude"); }
                else if (ov == IGO_INCLUDE) nmode = 2;
                else {
                    const StoreRule *mr = NULL; const StoreRuleSet *ms = NULL;
                    const int m = S->nrs ? st_scan_rule(S, path, nm, isd, &mr, &ms) : -1;
                    if (m == 1) { ign = 1; reason = mr->src == 2 ? IGR_KATANAIGNORE : IGR_GITIGNORE; st_rule_text(mr, ms, path, rel0, rule, sizeof rule); }
                    else if (m < 0 && hv) { ign = 1; reason = IGR_HEAVY; snprintf(rule, sizeof rule, "built-in heavy folder \"%s\"", nm); }
                    if (!ign && isd && rt_skip_subdir(path)) {       /* init aninhado (indice proprio) ou copia de trabalho do git (roots.c) */
                        ign = 1; if (rt_has_marker(path)) { reason = IGR_NESTED; snprintf(rule, sizeof rule, "has its own index (katana init)"); } else { reason = IGR_CHECKOUT; snprintf(rule, sizeof rule, "linked git worktree"); } }
                    else if (ign && isd && ov == IGO_PASS) { ign = 0; nmode = 1; S->pass_reason = reason; snprintf(S->pass_why, sizeof S->pass_why, "%s", rule); }
                }
            } else if (S->mode == 2 && ov == IGO_EXCLUDE) { ign = 1; reason = IGR_EXCLUDED; snprintf(rule, sizeof rule, "excluded with katana coder exclude"); }
            if (ign) { S->ignored++; st_sink(S, path, rel0, isd, reason, rule, isd ? 0 : (uint64_t)st.st_size); path[plen] = 0; continue; }
            if (isd) {
                const int smode = S->mode, sreason = S->pass_reason; char swhy[300]; if (smode == 1 || nmode == 1) memcpy(swhy, S->pass_why, sizeof swhy);
                S->mode = nmode;
                st_scan_walk(S, path, at + nl, rel0);
                S->mode = smode; S->pass_reason = sreason; if (smode == 1 || nmode == 1) memcpy(S->pass_why, swhy, sizeof swhy);
            } else {
                if (S->sink) {                       /* arquivo que fica no catalogo mas sem leitura: so a passada de contagem os registra */
                    const char *sr = ign_secret_name(rel);
                    if (sr) { snprintf(rule, sizeof rule, "secret file rule \"%s\"", sr); st_sink(S, path, rel0, 0, IGR_SECRET, rule, (uint64_t)st.st_size); }
                    else if (st_binary_ext(nm)) { const char *dot = strrchr(nm, '.'); snprintf(rule, sizeof rule, "binary (%s)", dot ? dot : "?"); st_sink(S, path, rel0, 0, IGR_BINARY, rule, (uint64_t)st.st_size); }
                    else if (S->sink->maxsz && (uint64_t)st.st_size > S->sink->maxsz) { snprintf(rule, sizeof rule, "larger than %llu MiB", (unsigned long long)(S->sink->maxsz >> 20)); st_sink(S, path, rel0, 0, IGR_TOOBIG, rule, (uint64_t)st.st_size); }
                }
                if (S->nolist) { path[plen] = 0; continue; }
                if (S->n == S->cap) { S->cap = S->cap ? S->cap * 2 : 1024; S->e = (StoreScanEnt *)realloc(S->e, sizeof *S->e * (size_t)S->cap); }
                StoreScanEnt *e = &S->e[S->n++];
                e->plen = (uint32_t)(at + nl - rel0); e->path = (char *)malloc(e->plen + 1); memcpy(e->path, path + rel0, e->plen + 1);
                e->size = (uint64_t)st.st_size;
#ifdef _WIN32
                e->mtime = (int64_t)st.st_mtime * 1000000000ll;
#else
                e->mtime = (int64_t)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
#endif
            }
        }
        path[plen] = 0;
    }
    closedir(d);
    if (pushed) { StoreRuleSet *R = &S->rs[--S->nrs]; for (int i = 0; i < R->n; i++) { free(R->r[i].pat); free(R->r[i].orig); } free(R->r); }
}
static int st_cmp_scan(const void *a, const void *b) { return strcmp(((const StoreScanEnt *)a)->path, ((const StoreScanEnt *)b)->path); }
static int st_cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* le o arquivo inteiro para buf (capacidade >= size+1); devolve bytes lidos ou -1 */
static int64_t st_slurp(const char *path, uint8_t *buf, size_t cap) {
#ifndef _WIN32
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    for (;;) { ssize_t r = read(fd, buf + got, cap - got); if (r < 0) { close(fd); return -1; } if (r == 0) break; got += (size_t)r; if (got == cap) break; }
    close(fd); return (int64_t)got;
#else
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    size_t got = fread(buf, 1, cap, f); fclose(f); return (int64_t)got;
#endif
}

static int maint_store_prepare(const char *dir, char *err, size_t ecap);       /* maint.c */
static int g_maint_rebuilt, g_maint_rebuilds;
static int maint_disk_ok(const char *dir, uint64_t need, char *err, size_t ecap);
/* atualizacao incremental: varre `root`, compara com o catalogo por (tamanho, mtime) e, se mtime mudou, por hash; escreve UM segmento novo
 * com os documentos novos/alterados e um tombstone por removido; compacta se passou de max_segs. Devolve 0, 1 (outro escritor) ou -1 (err). */
static int store_catalog_update(const char *dir, const char *root, const StoreScanOpts *o, StoreUpdStats *S, char *err, size_t ecap) {
    memset(S, 0, sizeof *S);
    double t0 = now_sec();
    store_catalog_init();
    uint64_t maxsz = o && o->max_size ? o->max_size : (2u << 20);
    int maxseg = o && o->max_segs ? o->max_segs : 8;
    char path[4096], rr[4096];
    StoreScan sc = {0};
    if (!realpath(root, rr)) { snprintf(err, ecap, "pasta invalida: %s", root); return -1; }
    IgnOvr ovr; ign_ovr_load(&ovr, dir); sc.ovr = &ovr;
    snprintf(path, sizeof path, "%s", rr);
    size_t rl = strlen(rr);
    st_scan_walk(&sc, path, rl, rl + (rl > 1 ? 1 : 0));
    ign_ovr_free(&ovr);
    qsort(sc.e, (size_t)sc.n, sizeof *sc.e, st_cmp_scan);
    S->scanned = sc.n; S->ignored = sc.ignored; S->ignored_heur = sc.ignored_heur; S->t_scan = now_sec() - t0;
    if (!maint_disk_ok(dir, (uint64_t)sc.n * 512, err, ecap)) { for (int i = 0; i < sc.n; i++) free(sc.e[i].path); free(sc.e); return -1; }      /* disco cheio: pausa, a camada anterior continua valendo */
    StoreWriter *w; int rc = store_writer_begin(dir, &w, err, ecap);
    if (rc == -1) {            /* nao abre: formato antigo ou dano = reconstroi do zero (F.6, G); formato mais novo = deixa quieto e diz */
        const int before = g_maint_rebuilds; const int pr = maint_store_prepare(dir, err, ecap);
        if (pr != 0) rc = pr; else if (g_maint_rebuilds != before) rc = store_writer_begin(dir, &w, err, ecap);
    }
    if (rc != 0) { for (int i = 0; i < sc.n; i++) free(sc.e[i].path); free(sc.e); return rc; }
    store_writer_set_root(w, rr);
    StoreLive L; const StoreSeg **segs = (const StoreSeg **)malloc(sizeof *segs * (size_t)(w->base->nseg + 1));
    for (int i = 0; i < w->base->nseg; i++) segs[i] = &w->base->seg[i];
    store_live_build(&L, segs, w->base->nseg); free(segs);
    uint8_t *buf = (uint8_t *)malloc((size_t)maxsz + 1);
    StoreDoc *nd = (StoreDoc *)malloc(sizeof *nd * (size_t)(sc.n + 1)); int nn = 0;
    for (int i = 0; i < sc.n; i++) {
        StoreScanEnt *e = &sc.e[i];
        StoreLiveDoc *l = store_live_find(&L, e->path, e->plen);
        int exists = l && !l->dead;
        if (exists) l->seen = 1;
        if (exists && l->d.size == e->size && l->d.mtime == e->mtime && !sdf_partial(l->d.flags)) { S->unchanged++; continue; }
        StoreDoc d; memset(&d, 0, sizeof d);
        const uint8_t *body = NULL; size_t blen = 0;
        d.path = e->path; d.plen = e->plen; d.size = e->size; d.mtime = e->mtime;
        d.kind = o && o->kind_fn ? o->kind_fn(e->path, e->plen) : 0;
        if (ign_secret_name(e->path)) d.flags = SDF_SKIP_SECRET;           /* segredo: so o nome entra no indice */
        else if (st_binary_ext(e->path)) d.flags = SDF_SKIP_BINARY;
        else if (e->size > maxsz) d.flags = SDF_SKIP_BIG;
        else {
            char full[8300]; snprintf(full, sizeof full, "%s/%s", rr, e->path);
            int64_t got = st_slurp(full, buf, (size_t)maxsz);
            if (got < 0) d.flags = SDF_SKIP_UNREADABLE;
            else {
                size_t chk = (size_t)got < 8000 ? (size_t)got : 8000;
                if (ign_secret_sniff(buf, (size_t)got)) d.flags = SDF_SKIP_SECRET;      /* chave privada reconhecida pelo conteudo (camada 2) */
                else if (memchr(buf, 0, chk)) d.flags = SDF_SKIP_BINARY;
                else { d.hash = store_hash64(buf, (size_t)got, 1); d.size = (uint64_t)got; body = buf; blen = (size_t)got; }
            }
        }
        if (o && o->doc_fn) o->doc_fn(o->hook_ctx, (uint32_t)nn, &d, body, blen);
        if (!exists) S->added++;
        else if (l->d.hash == d.hash && (l->d.flags & ~SDF_LVMASK) == d.flags && d.hash != 0) S->touched++;      /* so o mtime mudou: grava o registro novo, nao e alteracao */
        else S->changed++;
        nd[nn++] = d;
    }
    free(buf);
    const char **tomb = (const char **)malloc(sizeof *tomb * (size_t)(L.n + 1)); uint32_t *tl = (uint32_t *)malloc(sizeof *tl * (size_t)(L.n + 1)); int nt = 0;
    int nlive = 0;
    { int nv; StoreLiveDoc **v = store_live_sorted(&L, 0, &nv);
      for (int i = 0; i < nv; i++) { if (!v[i]->seen) { tomb[nt] = v[i]->d.path; tl[nt++] = v[i]->d.plen; } else nlive++; }
      free(v); }
    S->removed = nt;
    for (int i = 0; i < nn; i++) { if (nd[i].flags & SDF_SKIP_SECRET) S->skip_secret++; else if (nd[i].flags & SDF_SKIP_BINARY) S->skip_bin++; else if (nd[i].flags & SDF_SKIP_BIG) S->skip_big++; else if (nd[i].flags & SDF_SKIP_UNREADABLE) S->skip_unread++; }
    int64_t gen = 0;
    if (nn || nt) {
        StoreBuf db = {0}; store_docs_build(&db, nd, (uint32_t)nn);
        if (nn) store_writer_add_section_buf(w, STS_DOCS, 1, (uint32_t)nn, &db); else sb_free(&db);
        if (nn && o && o->commit_fn) o->commit_fn(o->hook_ctx, w, (uint32_t)nn);
        if (nt) { StoreBuf tb = {0}; store_strs_build(&tb, tomb, tl, (uint32_t)nt); store_writer_add_section_buf(w, STS_TOMBS, 1, (uint32_t)nt, &tb); }
        gen = store_writer_commit(w, (uint32_t)nn, err, ecap);
        if (gen < 0) rc = -1;
    }
    S->live = sc.n;
    if (rc == 0 && w->base->nseg > maxseg) {
        /* tiers: funde os segmentos mais novos enquanto somados forem pelo menos metade do anterior (pelo menos dois) */
        int from = w->base->nseg - 1; uint64_t sum = w->base->seg[from].size;
        while (from > 0 && (sum * 2 >= w->base->seg[from - 1].size || w->base->nseg - from < 2)) { from--; sum += w->base->seg[from].size; }
        int64_t g2 = store_writer_compact(w, from, err, ecap);
        if (g2 < 0) rc = -1; else if (g2 > 0) { gen = g2; S->compacted = 1; }
    }
    S->gen = gen ? gen : (int64_t)w->base->gen;
    free(nd); free(tomb); free(tl); store_live_free(&L);
    store_writer_end(w);
    for (int i = 0; i < sc.n; i++) free(sc.e[i].path);
    free(sc.e);
    S->t_total = now_sec() - t0;
    return rc;
}

/* ---- subcomandos ---- */
static void cstore_args(int argc, char **argv, char *proj, size_t cap, uint64_t *maxsz, int *force_all) {
    snprintf(proj, cap, ".");
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--max-size") && i + 1 < argc) *maxsz = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--all")) *force_all = 1;
        else snprintf(proj, cap, "%s", argv[i]);
    }
}
static const char *cstore_human(uint64_t b, char *o, size_t cap) {
    if (b >= (1u << 20)) snprintf(o, cap, "%.2f MiB", b / 1048576.0); else if (b >= 1024) snprintf(o, cap, "%.1f KiB", b / 1024.0); else snprintf(o, cap, "%llu B", (unsigned long long)b);
    return o;
}
static void coder_search_hook(StoreScanOpts *o);                       /* search.c: liga o indice de busca ao `index` */
static void coder_search_hook_done(StoreScanOpts *o, char *out, size_t cap);
static void coder_sym_hook(StoreScanOpts *o, const char *proj);        /* symindex.c: indice de simbolos e referencias, encadeado ao da busca */
static void coder_sym_hook_done(char *out, size_t cap);
static int coder_sym_verify_seg(const StoreSeg *s, char *err, size_t ecap);
static int coder_key_verify_seg(const StoreSeg *s, char *err, size_t ecap);
static void coder_sym_seg_info(const StoreSeg *s, char *out, size_t cap);
static int coder_search_verify_seg(const StoreSeg *s, char *err, size_t ecap);
static void coder_search_seg_info(const StoreSeg *s, char *out, size_t cap);
static int coder_crawl_verify_seg(const StoreSeg *s, char *err, size_t ecap);       /* crawl.c */
static void crawl_status_print(StoreReader *r, const IndexLoc *loc, const char *dir);       /* crawl.c */
static void ign_status_print(const char *storedir);                                         /* maint.c */
static void maint_after_index(const char *root, const char *dir);
static char g_maint_quarantine[4600];
static int coder_cmd_index(int argc, char **argv) {
    char proj[4096], dir[4400], err[300], h[40]; uint64_t maxsz = 0; int fa = 0;
    cstore_args(argc, argv, proj, sizeof proj, &maxsz, &fa);
    struct stat st; if (stat(proj, &st) != 0 || !S_ISDIR(st.st_mode)) { fprintf(stderr, "index: %s nao e uma pasta\n", proj); return 2; }
    IndexLoc loc; index_resolve(proj, 1, &loc);      /* a raiz que cobre a pasta (init, ancestral ou home); o indice e da raiz inteira */
    snprintf(proj, sizeof proj, "%s", loc.root);
    store_locate(proj, "coder", dir, sizeof dir, 1);
    StoreScanOpts o; memset(&o, 0, sizeof o); o.max_size = maxsz;
    coder_search_hook(&o);
    coder_sym_hook(&o, proj);
    StoreUpdStats S; err[0] = 0;
    int rc = store_catalog_update(dir, proj, &o, &S, err, sizeof err);
    char sinfo[400], yinfo[500]; coder_search_hook_done(&o, sinfo, sizeof sinfo); coder_sym_hook_done(yinfo, sizeof yinfo);
    if (rc == 1) { fprintf(stderr, "index: %s; use a leitura (status)\n", err); return 3; }
    if (rc != 0) { fprintf(stderr, "index: erro: %s\n", err); return 1; }
    if (loc.kind == IL_HOME) index_retire_below(proj);
    maint_after_index(proj, dir);                    /* passada de contagem dos ignorados (prioridade mais baixa: depois do indice) */
    StoreReader *r; uint64_t disk = 0; int nseg = 0;
    if (store_open(dir, &r, err, sizeof err) == 0) { disk = store_disk_bytes(r); nseg = r->nseg; store_close(r); }
    printf("index %s: %d arquivos (%d novos, %d alterados, %d so mtime, %d removidos, %d iguais); ignorados por tipo: %d binarios, %d grandes, %d ilegiveis, %d segredos; ignorados por regra (.gitignore/.katanaignore/lista interna/overrides): %d entradas (detalhe: katana coder ignored)\n",
           proj, S.scanned, S.added, S.changed, S.touched, S.removed, S.unchanged, S.skip_bin, S.skip_big, S.skip_unread, S.skip_secret, S.ignored);
    printf("  geracao %lld, %d segmento(s)%s, %s em disco, varredura %.3f s, total %.3f s\n  armazenamento: %s\n", (long long)S.gen, nseg, S.compacted ? " (compactado)" : "", cstore_human(disk, h, sizeof h), S.t_scan, S.t_total, dir);
    if (sinfo[0]) printf("  busca: %s\n", sinfo);
    if (yinfo[0]) printf("  simbolos: %s\n", yinfo);
    return 0;
}
static int coder_cmd_status(int argc, char **argv) {
    char proj[4096], dir[4400], err[300], h[40]; uint64_t maxsz = 0; int fa = 0;
    cstore_args(argc, argv, proj, sizeof proj, &maxsz, &fa);
    IndexLoc loc; index_resolve(proj, 0, &loc); snprintf(dir, sizeof dir, "%s", loc.store);
    { char vis[4200]; rt_real(proj, vis, sizeof vis);
      printf("pasta consultada: %s\nraiz resolvida: %s\n", vis, loc.root);
      if (loc.kind == IL_LOCAL) printf("origem do indice: local (init nesta pasta)\ninit: sim (marcador %s/.katana/coder/ROOT)\n", loc.root);
      else if (loc.kind == IL_ANCESTOR) printf("origem do indice: ancestral (init em %s); a consulta fica restrita ao prefixo %s\ninit: sim, na raiz ancestral\n", loc.root, loc.prefix);
      else if (loc.prefix[0]) printf("origem do indice: home (raiz da home %s absorve esta pasta; consulta restrita ao prefixo %s)\ninit: nao (nenhum ancestral tem init)\n", loc.root, loc.prefix);
      else printf("origem do indice: home (pasta visitada, sem init; nada e criado dentro dela)\ninit: nao\n");
      if (index_old_local_store(loc.root)) printf("aviso: ha um indice local antigo sem marcador em %s/.katana/coder/store (nao e lido; `katana init` na pasta o adota)\n", loc.root); }
    StoreReader *r;
    if (store_open(dir, &r, err, sizeof err) != 0) { fprintf(stderr, "status: %s\n", err); return 1; }
    printf("armazenamento: %s\nformato %d, geracao %llu, raiz %s, %d segmento(s), %s em segmentos\n", dir, STORE_FORMAT, (unsigned long long)r->gen, r->man.root[0] ? r->man.root : "(vazio)", r->nseg, cstore_human(store_disk_bytes(r), h, sizeof h));
    for (int i = 0; i < r->nseg; i++) {
        uint32_t nd = 0, nt = 0;
        const StoreSection *sd = store_seg_section(&r->seg[i], STS_DOCS), *stt = store_seg_section(&r->seg[i], STS_TOMBS);
        if (sd) nd = sd->nrec;
        if (stt) nt = stt->nrec;
        char si[200], yi[200]; coder_search_seg_info(&r->seg[i], si, sizeof si); coder_sym_seg_info(&r->seg[i], yi, sizeof yi);
        printf("  seg %06llu  %10s  %u secoes  %u documentos  %u tombstones%s%s%s%s\n", (unsigned long long)r->seg[i].id, cstore_human(r->seg[i].size, h, sizeof h), r->seg[i].nsec, nd, nt, si[0] ? "  | busca: " : "", si, yi[0] ? "  | simbolos: " : "", yi);
    }
    StoreLive L; const StoreSeg **segs = (const StoreSeg **)malloc(sizeof *segs * (size_t)(r->nseg + 1));
    for (int i = 0; i < r->nseg; i++) segs[i] = &r->seg[i];
    store_live_build(&L, segs, r->nseg); free(segs);
    int live = 0, bin = 0, big = 0, unr = 0; uint64_t bytes = 0;
    for (int i = 0; i < L.n; i++) if (!L.e[i].dead) { live++; bytes += L.e[i].d.size; if (L.e[i].d.flags & SDF_SKIP_BINARY) bin++; else if (L.e[i].d.flags & SDF_SKIP_BIG) big++; else if (L.e[i].d.flags & SDF_SKIP_UNREADABLE) unr++; }
    printf("arquivos vigentes: %d (%s de conteudo; %d binarios, %d grandes, %d ilegiveis sem leitura)\n", live, cstore_human(bytes, h, sizeof h), bin, big, unr);
    if (loc.prefix[0]) { const size_t pl = strlen(loc.prefix); int np = 0;
        for (int i = 0; i < L.n; i++) if (!L.e[i].dead && L.e[i].d.plen > pl && !memcmp(L.e[i].d.path, loc.prefix, pl) && L.e[i].d.path[pl] == '/') np++;
        printf("arquivos sob o prefixo %s: %d\n", loc.prefix, np); }
    { int sec = 0; for (int i = 0; i < L.n; i++) if (!L.e[i].dead && (L.e[i].d.flags & SDF_SKIP_SECRET)) sec++;
      if (sec) printf("arquivos de segredo catalogados so pelo nome (conteudo nunca lido): %d\n", sec); }
    store_live_free(&L);
    crawl_status_print(r, &loc, dir);
    ign_status_print(dir);
    store_close(r);
    return 0;
}
static int cstore_check_docs(const StoreSection *s, char *err, size_t ecap) {
    StoreDocs d; if (!store_docs_open(s, &d)) { snprintf(err, ecap, "secao de documentos malformada"); return -1; }
    StoreDoc prev; memset(&prev, 0, sizeof prev);
    for (uint32_t i = 0; i < d.t.n; i++) {
        StoreDoc x;
        if (!store_docs_get(&d, i, &x)) { snprintf(err, ecap, "documento %u: caminho fora do pool", i); return -1; }
        if (i && st_cmp_bytes(prev.path, prev.plen, x.path, x.plen) >= 0) { snprintf(err, ecap, "documentos fora de ordem no indice %u", i); return -1; }
        prev = x;
    }
    return 0;
}
static int coder_cmd_verify(int argc, char **argv) {
    char proj[4096], dir[4400], err[300]; uint64_t maxsz = 0; int fa = 0, bad = 0;
    cstore_args(argc, argv, proj, sizeof proj, &maxsz, &fa);
    store_locate(proj, "coder", dir, sizeof dir, 0);
    StoreReader *r;
    if (store_open(dir, &r, err, sizeof err) != 0) { printf("verify: FALHA: %s\n", err); return 1; }
    int secs = 0;
    for (int i = 0; i < r->nseg; i++) {
        char e2[300]; e2[0] = 0;
        int b = store_seg_verify(&r->seg[i], e2, sizeof e2);
        if (b) { printf("FALHA: %s (%d secao(oes))\n", e2, b); bad++; }
        const StoreSection *sd = store_seg_section(&r->seg[i], STS_DOCS);
        if (sd && !b && cstore_check_docs(sd, e2, sizeof e2) != 0) { printf("FALHA: segmento %llu: %s\n", (unsigned long long)r->seg[i].id, e2); bad++; }
        if (!b && coder_search_verify_seg(&r->seg[i], e2, sizeof e2) != 0) { printf("FALHA: segmento %llu: %s\n", (unsigned long long)r->seg[i].id, e2); bad++; }
        if (!b && coder_sym_verify_seg(&r->seg[i], e2, sizeof e2) != 0) { printf("FALHA: segmento %llu: %s\n", (unsigned long long)r->seg[i].id, e2); bad++; }
        if (!b && coder_crawl_verify_seg(&r->seg[i], e2, sizeof e2) != 0) { printf("FALHA: segmento %llu: %s\n", (unsigned long long)r->seg[i].id, e2); bad++; }
        if (!b && coder_key_verify_seg(&r->seg[i], e2, sizeof e2) != 0) { printf("FALHA: segmento %llu: %s\n", (unsigned long long)r->seg[i].id, e2); bad++; }
        secs += (int)r->seg[i].nsec;
    }
    /* arquivos em seg/ e tmp/ que o manifesto nao cita: apenas informativo (a proxima escrita limpa) */
    int orph = 0; char p[4600]; DIR *d; struct dirent *e;
    snprintf(p, sizeof p, "%s/seg", dir);
    if ((d = opendir(p))) {
        while ((e = readdir(d))) { unsigned long long id; char ext[8]; if (e->d_name[0] == '.') continue; int live = 0;
            if (sscanf(e->d_name, "%llu.%7s", &id, ext) == 2) for (int i = 0; i < r->nseg; i++) if (r->seg[i].id == id) live = 1;
            if (!live) orph++; }
        closedir(d);
    }
    snprintf(p, sizeof p, "%s/tmp", dir);
    if ((d = opendir(p))) { while ((e = readdir(d))) if (e->d_name[0] != '.') orph++; closedir(d); }
    printf("verify %s: geracao %llu, %d segmento(s), %d secao(oes) conferidas: %s%s\n", dir, (unsigned long long)r->gen, r->nseg, secs, bad ? "FALHOU" : "ok", "");
    if (orph) printf("  %d arquivo(s) orfao(s) ou temporario(s) (nao citados pelo manifesto; a proxima escrita os apaga)\n", orph);
    store_close(r);
    return bad ? 1 : 0;
}
static int coder_cmd_compact(int argc, char **argv) {
    char proj[4096], dir[4400], err[300]; uint64_t maxsz = 0; int fa = 0;
    cstore_args(argc, argv, proj, sizeof proj, &maxsz, &fa);
    store_locate(proj, "coder", dir, sizeof dir, 0);
    store_catalog_init();
    StoreWriter *w; int rc = store_writer_begin(dir, &w, err, sizeof err);
    if (rc == 1) { fprintf(stderr, "compact: %s\n", err); return 3; }
    if (rc != 0) { fprintf(stderr, "compact: erro: %s\n", err); return 1; }
    int before = w->base->nseg;
    int64_t g = store_writer_compact(w, 0, err, sizeof err);
    int after = w->base ? w->base->nseg : before;
    store_writer_end(w);
    if (g < 0) { fprintf(stderr, "compact: erro: %s\n", err); return 1; }
    printf("compact: %d -> %d segmento(s)%s\n", before, after, g == 0 ? " (nada a fundir)" : "");
    return 0;
}
