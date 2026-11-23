/* ---------- coder/statemap.c: mapa de estado do indice (fase 6 de docs/code-index-lifecycle.md, J) ----------
 * O mapa de estado diz, para uma pasta e suas subpastas, quantos arquivos ha em cada nivel (N0 nomes, N1 texto, N2/N3 simbolos e ligacoes) e em que
 * linguagens: e o que a ferramenta pode afirmar ou nao sobre aquela parte da arvore (linha de estado, `NOT FOUND ... so far`, Symbols action=tree).
 * So le o que o indice ja tem (documentos vigentes e as secoes STS_DIRS e STS_CRAWL): nenhum arquivo do projeto e aberto.
 * Decisoes do implementador:
 *   - o mapa e agregado POR CONSULTA, numa passada sobre os documentos vigentes da visao (a visao ja e guardada por geracao em indexctl.c); nao ha um
 *     segundo cache. O ponto de chamada statemap_get (fim de cs_view_open_raw) continua existindo e nao faz nada.
 *   - so as pastas ate `maxdepth` abaixo da base entram na tabela (a consulta pede ate 4 niveis); a base (profundidade 0) sempre entra.
 *   - o nivel de um arquivo vem dos bits 8..11 de StoreDoc.flags (0 = completo, 1 = N0, 2 = N1, 4 = N2+N3); arquivo binario, grande, segredo ou
 *     gerado nao tem nivel a esperar (conta em `skipped`).
 *   - STS_DIRS tem o formato de crawl.c (largura 72); a leitura abaixo repete o layout porque crawl.c vem depois deste arquivo na unidade de compilacao. */
#ifndef _WIN32
enum { SM_ST_DISC = 0, SM_ST_LISTED, SM_ST_IGNORED, SM_ST_NESTED, SM_ST_LINK, SM_ST_TOODEEP, SM_ST_TOOMANY, SM_ST_UNREAD, SM_ST_GONE, SM_ST_CHECKOUT };     /* espelho de CW_* em crawl.c */
#define SM_LANG 96

/* ---- registros de pasta (STS_DIRS), o mais novo de cada caminho ---- */
typedef struct { const char *path, *rule, *doc, *langs; uint32_t plen, rlen, dlen, llen, state, reason, nfiles, ndirs, nignored; } SmRec;
typedef struct { SmRec *r; int n, cap; int *tab; uint32_t tabn; } SmRecs;
static int sm_recs_find(const SmRecs *R, const char *p, size_t l) {
    if (!R->tabn) return -1;
    uint32_t h = (uint32_t)store_hash64(p, l, 5) & (R->tabn - 1);
    while (R->tab[h] >= 0) { const SmRec *x = &R->r[R->tab[h]]; if (x->plen == l && !memcmp(x->path, p, l)) return R->tab[h]; h = (h + 1) & (R->tabn - 1); }
    return -1;
}
static void sm_recs_free(SmRecs *R) { free(R->r); free(R->tab); memset(R, 0, sizeof *R); }
static void sm_recs_put(SmRecs *R, const SmRec *x) {
    int f = sm_recs_find(R, x->path, x->plen);
    if (f >= 0) { R->r[f] = *x; return; }
    if (R->n == R->cap) {
        R->cap = R->cap ? R->cap * 2 : 128; R->r = (SmRec *)realloc(R->r, sizeof(SmRec) * (size_t)R->cap);
        uint32_t n = 64; while (n < (uint32_t)R->cap * 2) n *= 2;
        free(R->tab); R->tab = (int *)malloc(sizeof(int) * n); R->tabn = n; for (uint32_t i = 0; i < n; i++) R->tab[i] = -1;
        for (int i = 0; i < R->n; i++) { uint32_t h = (uint32_t)store_hash64(R->r[i].path, R->r[i].plen, 5) & (n - 1); while (R->tab[h] >= 0) h = (h + 1) & (n - 1); R->tab[h] = i; }
    }
    R->r[R->n] = *x;
    uint32_t h = (uint32_t)store_hash64(x->path, x->plen, 5) & (R->tabn - 1); while (R->tab[h] >= 0) h = (h + 1) & (R->tabn - 1); R->tab[h] = R->n++;
}
/* le STS_DIRS de todos os segmentos (do mais antigo ao mais novo: o ultimo registro de um caminho vale). Os textos apontam para o mapeamento do leitor. 0 = nao ha registros */
static int sm_recs_load(const StoreReader *r, SmRecs *R) {
    memset(R, 0, sizeof *R);
    for (int s = 0; s < r->nseg; s++) {
        const StoreSection *sec = store_seg_section(&r->seg[s], STS_DIRS); StoreTable t;
        if (!sec || !store_table_open(sec, &t) || t.w != 72) continue;
        const uint64_t head = 8 + (uint64_t)t.n * t.w; if (head > sec->size) continue;
        const uint8_t *pool = sec->data + head; const uint64_t ps = sec->size - head;
#define SM_STR(at, P, L) do { uint32_t o_ = st_get_u32(q + (at)), l_ = st_get_u32(q + (at) + 4); if ((uint64_t)o_ + l_ > ps) { P = ""; L = 0; } else { P = (const char *)pool + o_; L = l_; } } while (0)
        for (uint32_t i = 0; i < t.n; i++) {
            const uint8_t *q = store_table_at(&t, i); SmRec x; memset(&x, 0, sizeof x);
            SM_STR(0, x.path, x.plen); SM_STR(20, x.rule, x.rlen); SM_STR(28, x.doc, x.dlen); SM_STR(36, x.langs, x.llen);
            x.state = st_get_u32(q + 12); x.reason = st_get_u32(q + 16); x.nfiles = st_get_u32(q + 44); x.ndirs = st_get_u32(q + 48); x.nignored = st_get_u32(q + 52);
            sm_recs_put(R, &x);
        }
#undef SM_STR
    }
    return R->n > 0;
}
/* o andamento do rastreador (STS_CRAWL do segmento mais novo que o tem): 1 = ha registro; *complete = todos os niveis completos; total/n1/n2 = contadores */
static int sm_crawl_state(const StoreReader *r, int *complete, uint64_t *total, uint64_t *n1, uint64_t *n2) {
    for (int s = r->nseg - 1; s >= 0; s--) {
        const StoreSection *x = store_seg_section(&r->seg[s], STS_CRAWL); if (!x || x->size < 256 || st_get_u32(x->data) != 0x5743524Bu) continue;
        const uint32_t fr = st_get_u32(x->data + 64), fl = st_get_u32(x->data + 68);
        if (complete) *complete = (fl & 1u) && !fr;
        if (total) *total = st_get_u64(x->data + 32);
        if (n1) *n1 = st_get_u64(x->data + 48);
        if (n2) *n2 = st_get_u64(x->data + 56);
        return 1;
    }
    return 0;
}

/* ---- agregacao por pasta ---- */
typedef struct {
    char *path; uint32_t plen; int depth;                 /* relativo a base ("" = a propria base) */
    uint32_t files, skipped, n_names, n_text, n_full;     /* arquivos debaixo da pasta (recursivo); n_*: os que esperam nivel, por nivel mais baixo ja alcancado */
    uint32_t *lc;                                         /* arquivos por linguagem (id do analisador; SM_LANG-1 = outras) */
} SmDir;
typedef struct { SmDir *d; int n, cap; int *tab; uint32_t tabn; char **here; int nhere, caphere; } StateMap;
static int sm_dir_find(const StateMap *M, const char *p, size_t l) {
    if (!M->tabn) return -1;
    uint32_t h = (uint32_t)store_hash64(p, l, 6) & (M->tabn - 1);
    while (M->tab[h] >= 0) { const SmDir *x = &M->d[M->tab[h]]; if (x->plen == l && !memcmp(x->path, p, l)) return M->tab[h]; h = (h + 1) & (M->tabn - 1); }
    return -1;
}
static SmDir *sm_dir_get(StateMap *M, const char *p, size_t l, int depth) {
    int f = sm_dir_find(M, p, l); if (f >= 0) return &M->d[f];
    if (M->n == M->cap) {
        M->cap = M->cap ? M->cap * 2 : 64; M->d = (SmDir *)realloc(M->d, sizeof(SmDir) * (size_t)M->cap);
        uint32_t n = 64; while (n < (uint32_t)M->cap * 2) n *= 2;
        free(M->tab); M->tab = (int *)malloc(sizeof(int) * n); M->tabn = n; for (uint32_t i = 0; i < n; i++) M->tab[i] = -1;
        for (int i = 0; i < M->n; i++) { uint32_t h = (uint32_t)store_hash64(M->d[i].path, M->d[i].plen, 6) & (n - 1); while (M->tab[h] >= 0) h = (h + 1) & (n - 1); M->tab[h] = i; }
    }
    SmDir *x = &M->d[M->n]; memset(x, 0, sizeof *x); x->path = (char *)malloc(l + 1); memcpy(x->path, p, l); x->path[l] = 0; x->plen = (uint32_t)l; x->depth = depth;
    x->lc = (uint32_t *)calloc(SM_LANG, sizeof(uint32_t));
    uint32_t h = (uint32_t)store_hash64(p, l, 6) & (M->tabn - 1); while (M->tab[h] >= 0) h = (h + 1) & (M->tabn - 1); M->tab[h] = M->n++;
    return x;
}
static void statemap_free(StateMap *M) {
    for (int i = 0; i < M->n; i++) { free(M->d[i].path); free(M->d[i].lc); }
    for (int i = 0; i < M->nhere; i++) free(M->here[i]);
    free(M->d); free(M->tab); free(M->here); memset(M, 0, sizeof *M);
}
static void sm_count(SmDir *x, uint32_t flags, int lang) {
    x->files++;
    if (flags & (SDF_SKIPMASK | SDF_GENERATED)) x->skipped++;
    else { const uint32_t lv = sdf_lv(flags); if (lv == 1) x->n_names++; else if (lv == 2) x->n_text++; else x->n_full++; }
    x->lc[lang > 0 && lang < SM_LANG - 1 ? lang : SM_LANG - 1]++;          /* desconhecida (0) e fora da tabela (>= SM_LANG-1) contam juntas como "other" */
}
/* uma passada sobre os documentos vigentes `L` (caminhos relativos a raiz da visao): a base `base` (sem barra final; "" = tudo) e as pastas ate `maxdepth`
 * niveis abaixo dela. lang_fn(path, len) devolve o id da linguagem do arquivo (so olha o nome; nao abre nada). Devolve quantos arquivos ha debaixo da base. */
static uint32_t statemap_build(const StoreLive *L, const char *base, int maxdepth, int (*lang_fn)(const char *, size_t), StateMap *M) {
    memset(M, 0, sizeof *M);
    const size_t bl = strlen(base); uint32_t total = 0; sm_dir_get(M, "", 0, 0);          /* a base e sempre a entrada 0 (o vetor pode crescer: nao guardar ponteiro) */
    for (int i = 0; i < L->n; i++) {
        const StoreLiveDoc *e = &L->e[i]; if (e->dead) continue;
        const char *p = e->d.path; size_t pl = e->d.plen;
        if (bl) { if (pl <= bl + 1 || memcmp(p, base, bl) != 0 || p[bl] != '/') continue; p += bl + 1; pl -= bl + 1; }
        const int lang = lang_fn ? lang_fn(e->d.path, e->d.plen) : 0; total++;
        sm_count(&M->d[0], e->d.flags, lang);
        int d = 0; size_t pos = 0;
        while (d < maxdepth) {
            const char *sl = (const char *)memchr(p + pos, '/', pl - pos); if (!sl) break;
            pos = (size_t)(sl - p); d++; sm_count(sm_dir_get(M, p, pos, d), e->d.flags, lang); pos++;
        }
        if (!memchr(p, '/', pl) && M->nhere < 2000) {          /* arquivo solto na base: os nomes para `files here:` */
            if (M->nhere == M->caphere) { M->caphere = M->caphere ? M->caphere * 2 : 32; M->here = (char **)realloc(M->here, sizeof(char *) * (size_t)M->caphere); }
            char *nm = (char *)malloc(pl + 1); memcpy(nm, p, pl); nm[pl] = 0; M->here[M->nhere++] = nm;
        }
    }
    return total;
}
/* ponto de chamada do fim de cs_view_open_raw (search.c): o mapa e montado por consulta com statemap_build, entao aqui nao ha o que fazer */
static int statemap_get(const void *view, const char *prefix) { (void)view; (void)prefix; return 0; }
#else
static int statemap_get(const void *view, const char *prefix) { (void)view; (void)prefix; return 0; }
#endif
