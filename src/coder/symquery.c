/* ---------- coder/symquery.c: consultas estruturais sobre o indice de simbolos (symindex.c) ----------
 * katana coder def|refs|callers|callees|uses|outline|at|deps|impact <alvo> [pasta] [--json] [--limit N] [--lines N] [--depth N] [--nodes N] [--body]
 *
 * CASCATA E NIVEL DE CONFIANCA (substitui os tres graus do desenho original; nao ha rotulo "exato"): a consulta tenta primeiro o NOME EXATO
 * (passo 1); se o conjunto indexado nao tem o que foi pedido, tenta por DERIVACAO (passo 2: mesmo nome ignorando maiusculas e sublinhados, ou o
 * nome como parte de um identificador maior); se ainda nao achou, tenta o PROVAVEL (passo 3: nomes a distancia de edicao <= 2). So depois de
 * percorrer a cascata inteira o resultado vazio vira "NOT FOUND" com o limite do conjunto indexado. Cada resultado traz `confidence` (0 a 100),
 * o nivel (high >= 75, medium >= 45, low) e `how` (em que passo e por que foi achado). A confianca vem de:
 *   - o passo da cascata que achou (92, 62, 32);
 *   - ambiguidade: varias definicoes do mesmo nome baixam (5 por definicao a mais, ate 25);
 *   - verificacao: o texto do nome foi reencontrado relendo a linha do arquivo (+4); a linha nao confere ou o arquivo mudou desde o `index` (-25);
 *   - relacao por nome: mesmo arquivo da definicao ou definicao unica no conjunto nao penalizam; nome sem definicao no conjunto (externo) -25.
 * Caminhos no grafo (impact) tem a confianca da aresta mais fraca. Nada aqui resolve nomes de verdade: parse.c so da o nome final de uma chamada. */
typedef struct { const StoreSeg *seg; SySec x; KySec ky; StoreDocs sd; uint8_t *live; } SyVSeg;
/* mapa modulo->arquivo e modulos go.mod montados sob demanda por sy_mods_build; pertencem a visao (nao ha estado global): liberados em sy_view_close */
typedef struct { char *k; StoreLiveDoc *d; } SyNsE;
typedef struct { SyNsE *e; int n, cap; int built; char **gomod; char **godir; int ngo; } SyMods;
typedef struct {
    StoreReader *r; SyVSeg *s; int ns; StoreLive L; char dir[4400]; int any;
    uint32_t nlive, nwith, nbin, nbig, nunread, nunparsed, nkeyflag, nkeyold; int anykeys;
    char fpath[400]; char *fbuf; size_t fn; size_t *fln; int fnl, fok, fstale;
    uint32_t strip;                     /* consulta restrita a um prefixo da raiz: bytes do prefixo (e da barra) tirados dos caminhos */
    SyMods *mods;                       /* sob demanda (deps, imports); da visao, nao do processo */
    int borrowed; void *slot;           /* visao reaproveitada de indexctl.c: copia rasa do cache; fbuf, fln e os contadores fN sao desta consulta */
} SyView;
typedef struct { const char *path; uint32_t plen; const char *name, *sig; uint32_t nl, sl, kind, line0, line1, byte0, byte1, body0, pack, parent, doc, lang; int seg; uint32_t row, rel; } SySym;
typedef struct { const char *path; uint32_t plen; const char *name; uint32_t nl, kind, line, col, flags, from, doc, lang; int seg; uint32_t row; } SyRef;

static void sy_mods_free(SyView *v);
static int sy_view_open_raw(const char *proj, SyView *v, char *err, size_t ecap) {
    memset(v, 0, sizeof *v);
    IndexLoc loc; index_resolve(proj, 0, &loc); snprintf(v->dir, sizeof v->dir, "%s", loc.store);
    if (store_open(v->dir, &v->r, err, ecap) != 0) return -1;
    StoreReader *r = v->r;
    if (r->nseg == 0) { snprintf(err, ecap, "no index in %s (run: katana coder index %s)", v->dir, proj); store_close(r); v->r = NULL; return -1; }
    const StoreSeg **segs = (const StoreSeg **)malloc(sizeof *segs * (size_t)r->nseg);
    for (int i = 0; i < r->nseg; i++) segs[i] = &r->seg[i];
    store_live_build(&v->L, segs, r->nseg); free(segs);
    ign_hide_for_view(&loc, &v->L);               /* pastas de inclusao restrita que esta consulta nao pediu (maint.c) */
    if (loc.prefix[0]) { store_live_rebase(&v->L, loc.prefix, strlen(loc.prefix)); v->strip = (uint32_t)strlen(loc.prefix) + 1; snprintf(r->man.root, sizeof r->man.root, "%s/%s", loc.root, loc.prefix); }      /* ver search.c cs_view_open_raw */
    else snprintf(r->man.root, sizeof r->man.root, "%s", loc.root);
    v->ns = r->nseg; v->s = (SyVSeg *)calloc((size_t)v->ns, sizeof(SyVSeg));
    for (int s = 0; s < v->ns; s++) {
        SyVSeg *c = &v->s[s]; c->seg = &r->seg[s]; store_docs_open(store_seg_section(c->seg, STS_DOCS), &c->sd);
        c->live = (uint8_t *)calloc((size_t)c->sd.t.n + 1, 1);
        if (sy_sec_open(c->seg, &c->x)) v->any = 1;
        if (ky_sec_open(c->seg, &c->ky)) v->anykeys = 1;
    }
    for (int i = 0; i < v->L.n; i++) {
        StoreLiveDoc *e = &v->L.e[i]; if (e->dead) continue;
        v->s[e->seg].live[e->idx] = 1; v->nlive++;
        if (e->d.flags & SDF_SKIP_BINARY) v->nbin++; else if (e->d.flags & SDF_SKIP_BIG) v->nbig++; else if (e->d.flags & SDF_SKIP_UNREADABLE) v->nunread++;
        else { const SyVSeg *c = &v->s[e->seg]; int w = 0;
            if (c->x.has && e->idx < c->x.docs.n) { const uint8_t *q = store_table_at(&c->x.docs, e->idx); w = st_get_u32(q + 4) || st_get_u32(q + 12); }
            if (w) v->nwith++; else v->nunparsed++;
            if (c->ky.has && e->idx < c->ky.docs.n && (st_get_u32(store_table_at(&c->ky.docs, e->idx) + 4) & SY_KFLAG)) v->nkeyflag++; else if (w) v->nkeyold++; }
    }
    return 0;
}
static void sy_view_free_real(SyView *v) {
    sy_mods_free(v);                     /* o mapa de modulos nasce na consulta de imports; quem fecha direto (grafo do Find) tambem o libera */
    for (int s = 0; s < v->ns; s++) free(v->s[s].live);
    free(v->s); store_live_free(&v->L); free(v->fbuf); free(v->fln); if (v->r) store_close(v->r); memset(v, 0, sizeof *v);
}
static void sy_cache_destroy(void *p) { sy_view_free_real((SyView *)p); free(p); }
static IdxCache g_sy_cache = {.destroy = sy_cache_destroy};
static void sy_view_close(SyView *v) {
    sy_mods_free(v);
    if (v->borrowed) { void *h = v->slot; free(v->fbuf); free(v->fln); memset(v, 0, sizeof *v); idx_cache_release(&g_sy_cache, h); return; }
    sy_view_free_real(v);
}
static int sy_view_open(const char *proj, SyView *v, char *err, size_t ecap) {
    char dir[4400]; idx_view_key(proj, dir, sizeof dir);
    void *h = idx_cache_get(&g_sy_cache, dir);
    if (h) { *v = *(SyView *)h; v->fpath[0] = 0; v->fbuf = NULL; v->fn = 0; v->fln = NULL; v->fnl = v->fok = v->fstale = 0; v->mods = NULL; v->borrowed = 1; v->slot = h; return 0; }
    SyView t; if (sy_view_open_raw(proj, &t, err, ecap) != 0) return -1;
    SyView *hp = (SyView *)malloc(sizeof *hp);
    if (hp) { *hp = t; if (idx_cache_put(&g_sy_cache, dir, hp, t.r)) { *v = *hp; v->borrowed = 1; v->slot = hp; return 0; } free(hp); }
    *v = t; return 0;
}
static const char *sy_root(const SyView *v) { return v->r->man.root; }
static void sy_limits(const SyView *v, char *o, size_t cap) {
    snprintf(o, cap, "Indexed set: %u files (%u with symbols); not analysed: %u binary, %u over the size cap, %u unreadable; %u text/config/unknown-language files have no symbol extraction. "
             "Macro-generated names, dynamic calls and calls through pointers or reflection are not seen; a call is recorded by its final name only.", v->nlive, v->nwith, v->nbin, v->nbig, v->nunread, v->nunparsed);
}

/* ---- acesso a registros ---- */
static void sy_doc_path(const SyView *v, int seg, uint32_t doc, const char **p, uint32_t *l, uint32_t *lang) {
    StoreDoc d; *p = ""; *l = 0; if (lang) *lang = 0;
    if (store_docs_get(&v->s[seg].sd, doc, &d)) { *p = d.path; *l = d.plen; if (v->strip && d.plen > v->strip) { *p += v->strip; *l -= v->strip; } }
    if (lang && v->s[seg].x.has && doc < v->s[seg].x.docs.n) *lang = st_get_u32(store_table_at(&v->s[seg].x.docs, doc) + 16);
}
static void sy_sym_at(const SyView *v, int seg, uint32_t row, SySym *o) {
    const SySec *x = &v->s[seg].x; const uint8_t *q = store_table_at(&x->syms, row);
    o->seg = seg; o->row = row; o->name = store_strs_get(&x->names, st_get_u32(q), &o->nl); o->kind = st_get_u32(q + 4); o->parent = st_get_u32(q + 8);
    o->line0 = st_get_u32(q + 12); o->line1 = st_get_u32(q + 16); o->byte0 = st_get_u32(q + 20); o->byte1 = st_get_u32(q + 24); o->body0 = st_get_u32(q + 28);
    o->sig = (const char *)x->sigpool + st_get_u32(q + 32); o->sl = st_get_u32(q + 36); o->pack = st_get_u32(q + 40); o->doc = st_get_u32(q + 44);
    sy_doc_path(v, seg, o->doc, &o->path, &o->plen, &o->lang);
    o->rel = row - st_get_u32(store_table_at(&x->docs, o->doc));
}
static void sy_ref_at(const SyView *v, int seg, uint32_t row, SyRef *o) {
    const SySec *x = &v->s[seg].x; const uint8_t *q = store_table_at(&x->refs, row);
    o->seg = seg; o->row = row; o->name = store_strs_get(&x->names, st_get_u32(q), &o->nl); o->kind = st_get_u32(q + 4); o->from = st_get_u32(q + 8);
    o->line = st_get_u32(q + 12); o->col = st_get_u32(q + 16); o->flags = st_get_u32(q + 20); o->doc = st_get_u32(q + 24);
    sy_doc_path(v, seg, o->doc, &o->path, &o->plen, &o->lang);
}
static int sy_pair_range(const StoreTable *t, uint32_t id, uint32_t *lo, uint32_t *hi) {
    uint32_t a = 0, b = t->n;
    while (a < b) { uint32_t m = a + (b - a) / 2; if (st_get_u32(store_table_at(t, m)) < id) a = m + 1; else b = m; }
    *lo = a; b = t->n;
    uint32_t c = a; while (c < b) { uint32_t m = c + (b - c) / 2; if (st_get_u32(store_table_at(t, m)) <= id) c = m + 1; else b = m; }
    *hi = c; return c > a;
}
static int sy_cmp_path_line(const char *p1, uint32_t l1, int ln1, const char *p2, uint32_t l2, int ln2) {
    int c = st_cmp_bytes(p1, l1, p2, l2); if (c) return c; return ln1 < ln2 ? -1 : ln1 > ln2;
}
static int sy_cmp_sym(const void *a, const void *b) {
    const SySym *x = (const SySym *)a, *y = (const SySym *)b;
    int dx = (x->pack >> 16) & CSF_DECL, dy = (y->pack >> 16) & CSF_DECL; if (dx != dy) return dx - dy;
    return sy_cmp_path_line(x->path, x->plen, (int)x->line0, y->path, y->plen, (int)y->line0);
}
static int sy_cmp_ref(const void *a, const void *b) {
    const SyRef *x = (const SyRef *)a, *y = (const SyRef *)b; if (x->kind != y->kind) return x->kind < y->kind ? -1 : 1;
    int c = sy_cmp_path_line(x->path, x->plen, (int)x->line, y->path, y->plen, (int)y->line); return c ? c : x->col < y->col ? -1 : x->col > y->col;
}
typedef struct { SySym *s; int n, cap; } SySyms;
typedef struct { SyRef *r; int n, cap; } SyRefs;
static void sy_defs_of(const SyView *v, const char *name, size_t nl, SySyms *o) {
    for (int s = 0; s < v->ns; s++) { const SyVSeg *c = &v->s[s]; if (!c->x.has) continue;
        int64_t id = store_strs_find(&c->x.names, name, nl); uint32_t lo, hi; if (id < 0 || !sy_pair_range(&c->x.bydef, (uint32_t)id, &lo, &hi)) continue;
        for (uint32_t i = lo; i < hi; i++) { uint32_t row = st_get_u32(store_table_at(&c->x.bydef, i) + 4); if (!c->live[st_get_u32(store_table_at(&c->x.syms, row) + 44)]) continue;
            if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 16; o->s = (SySym *)realloc(o->s, sizeof(SySym) * (size_t)o->cap); }
            sy_sym_at(v, s, row, &o->s[o->n++]); } }
    if (o->n > 1) qsort(o->s, (size_t)o->n, sizeof(SySym), sy_cmp_sym);
}
static void sy_refs_of(const SyView *v, const char *name, size_t nl, uint32_t kindmask, SyRefs *o) {
    for (int s = 0; s < v->ns; s++) { const SyVSeg *c = &v->s[s]; if (!c->x.has) continue;
        int64_t id = store_strs_find(&c->x.names, name, nl); uint32_t lo, hi; if (id < 0 || !sy_pair_range(&c->x.byref, (uint32_t)id, &lo, &hi)) continue;
        for (uint32_t i = lo; i < hi; i++) { uint32_t row = st_get_u32(store_table_at(&c->x.byref, i) + 4); const uint8_t *q = store_table_at(&c->x.refs, row);
            if (!c->live[st_get_u32(q + 24)] || !((kindmask >> st_get_u32(q + 4)) & 1)) continue;
            if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 64; o->r = (SyRef *)realloc(o->r, sizeof(SyRef) * (size_t)o->cap); }
            sy_ref_at(v, s, row, &o->r[o->n++]); } }
    if (o->n > 1) qsort(o->r, (size_t)o->n, sizeof(SyRef), sy_cmp_ref);
}
static int sy_ndefs(const SyView *v, const char *name, size_t nl, int *ndecl) {
    SySyms d = {0}; sy_defs_of(v, name, nl, &d); int n = 0; if (ndecl) *ndecl = 0;
    for (int i = 0; i < d.n; i++) { if ((d.s[i].pack >> 16) & CSF_DECL) { if (ndecl) (*ndecl)++; } else n++; }
    free(d.s); return n;
}
static int sy_nrefs(const SyView *v, const char *name, size_t nl) { SyRefs r = {0}; sy_refs_of(v, name, nl, ~0u, &r); int n = r.n; free(r.r); return n; }

/* ---- arquivo lido de volta (verificacao e trechos) ---- */
static int sy_file_load(SyView *v, const char *path, uint32_t plen) {
    char key[400]; snprintf(key, sizeof key, "%.*s", plen < 399 ? (int)plen : 399, path);
    if (v->fbuf && !strcmp(key, v->fpath)) return v->fok;
    free(v->fbuf); free(v->fln); v->fbuf = NULL; v->fln = NULL; v->fn = 0; v->fnl = 0; v->fok = 0; v->fstale = 0;
    snprintf(v->fpath, sizeof v->fpath, "%s", key);
    char full[8400]; snprintf(full, sizeof full, "%s/%s", sy_root(v), key);
    struct stat st; if (stat(full, &st) != 0 || st.st_size > (8 << 20)) { v->fbuf = (char *)calloc(1, 1); v->fstale = 1; fresh_check(sy_root(v), key, 0, 0); return 0; }
    v->fbuf = (char *)malloc((size_t)st.st_size + 1); int64_t got = st_slurp(full, (uint8_t *)v->fbuf, (size_t)st.st_size);
    if (got < 0) { v->fbuf[0] = 0; v->fstale = 1; return 0; }
    v->fn = (size_t)got; v->fbuf[got] = 0;
    StoreLiveDoc *e = store_live_find(&v->L, path, plen);
    int64_t mt;
#ifdef _WIN32
    mt = (int64_t)st.st_mtime * 1000000000ll;
#else
    mt = (int64_t)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
#endif
    if (!e || e->d.size != (uint64_t)got || e->d.mtime != mt) { v->fstale = 1; fresh_check(sy_root(v), key, e ? e->d.size : 0, e ? e->d.mtime : 0); }      /* conta, avisa o vigia e mostra a linha como "NOT verified" (fresh.c) */
    int nl = 1; for (size_t i = 0; i < v->fn; i++) if (v->fbuf[i] == '\n') nl++;
    v->fln = (size_t *)malloc(sizeof(size_t) * (size_t)nl); int q = 0; v->fln[q++] = 0;
    for (size_t i = 0; i < v->fn; i++) if (v->fbuf[i] == '\n') v->fln[q++] = i + 1;
    v->fnl = nl; v->fok = 1; return 1;
}
/* texto da linha (1-based) sem espacos nas pontas, truncado */
static int sy_line_text(const SyView *v, int line, char *o, size_t cap) {
    o[0] = 0; if (!v->fok || line < 1 || line > v->fnl) return 0;
    size_t a = v->fln[line - 1], b = a; while (b < v->fn && v->fbuf[b] != '\n' && v->fbuf[b] != '\r') b++;
    while (a < b && (v->fbuf[a] == ' ' || v->fbuf[a] == '\t')) a++;
    while (b > a && (v->fbuf[b - 1] == ' ' || v->fbuf[b - 1] == '\t')) b--;
    size_t l = b - a; if (l + 1 > cap) { l = cap - 1; while (l > 0 && ((unsigned char)v->fbuf[a + l] & 0xC0) == 0x80) l--; }
    memcpy(o, v->fbuf + a, l); o[l] = 0; return 1;
}
static int sy_has_word(const char *t, const char *w, size_t wl) {
    for (const char *p = t; (p = strstr(p, w)); p += 1) {
        int pre = p == t || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' || ((unsigned char)p[-1] & 0x80)), post = !(isalnum((unsigned char)p[wl]) || p[wl] == '_' || ((unsigned char)p[wl] & 0x80));
        if (pre && post) return 1;
    }
    return 0;
}
/* verifica relendo o arquivo: o nome ocorre nas linhas [l0, l0+span]. Devolve 1 confere, -1 nao confere ou arquivo mudou/ilegivel */
static int sy_verify(SyView *v, const char *path, uint32_t plen, int l0, int span, const char *name, size_t nl) {
    if (!sy_file_load(v, path, plen) || v->fstale) return -1;
    char t[600], nm[200]; snprintf(nm, sizeof nm, "%.*s", nl < 199 ? (int)nl : 199, name);
    for (int l = l0; l <= l0 + span; l++) { if (sy_line_text(v, l, t, sizeof t) && sy_has_word(t, nm, strlen(nm))) return 1; }
    return -1;
}

/* ---- confianca ---- */
static int sy_conf(int step, int ndefs, int ver, int adj) {
    static const int base[4] = {0, 92, 62, 32}; int c = base[step < 1 ? 1 : step > 3 ? 3 : step];
    if (ndefs > 1) { int p = 5 * (ndefs - 1); c -= p > 25 ? 25 : p; }
    c += ver > 0 ? 4 : ver < 0 ? -25 : 0; c += adj;
    return c < 1 ? 1 : c > 99 ? 99 : c;
}
static const char *sy_level(int c) { return c >= 75 ? "high" : c >= 45 ? "medium" : "low"; }

/* ---- cascata de nomes ---- */
typedef struct { char name[128]; int step; char how[110]; } SyCand;
static int sy_tokens(const char *s, size_t l, char t[][32], int max) {
    int n = 0, k = 0;
    for (size_t i = 0; i < l; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c)) { if (k) { t[n++][k] = 0; k = 0; if (n >= max) return n; } continue; }
        int cut = 0;
        if (k && isupper(c) && islower((unsigned char)s[i - 1])) cut = 1;
        if (k && isupper(c) && isupper((unsigned char)s[i - 1]) && i + 1 < l && islower((unsigned char)s[i + 1])) cut = 1;
        if (cut) { t[n++][k] = 0; k = 0; if (n >= max) return n; }
        if (k < 31) t[n][k++] = (char)tolower(c);
    }
    if (k && n < max) t[n++][k] = 0;
    return n;
}
static void sy_fold(const char *s, size_t l, char *o, size_t cap) { size_t k = 0; for (size_t i = 0; i < l && k + 1 < cap; i++) if (isalnum((unsigned char)s[i]) || ((unsigned char)s[i] & 0x80)) o[k++] = (char)tolower((unsigned char)s[i]); o[k] = 0; }
static int sy_lev(const char *a, int al, const char *b, int bl) {
    int d[2][130]; if (al > 128 || bl > 128) return 99;
    for (int j = 0; j <= bl; j++) d[0][j] = j;
    for (int i = 1; i <= al; i++) { d[i & 1][0] = i; for (int j = 1; j <= bl; j++) { int c = d[(i - 1) & 1][j - 1] + (a[i - 1] != b[j - 1]), x = d[(i - 1) & 1][j] + 1, y = d[i & 1][j - 1] + 1; if (x < c) c = x; if (y < c) c = y; d[i & 1][j] = c; } }
    return d[al & 1][bl];
}
typedef struct { char name[128]; int score; } SyNameHit;
static int sy_hit_cmp(const void *a, const void *b) { const SyNameHit *x = (const SyNameHit *)a, *y = (const SyNameHit *)b; if (x->score != y->score) return x->score - y->score; return (int)strlen(x->name) - (int)strlen(y->name); }
/* want 1: precisa de definicao; 2: basta definicao ou referencia. Devolve o numero de candidatos; *stepused = passo que achou (0 = nenhum) */
static int sy_cascade(const SyView *v, const char *q, int want, SyCand *out, int max, int *stepused) {
    size_t ql = strlen(q); int n = 0; *stepused = 0;
    int nd = sy_ndefs(v, q, ql, NULL), nr = want == 2 ? sy_nrefs(v, q, ql) : 0;
    if (nd > 0 || nr > 0) { snprintf(out[0].name, sizeof out[0].name, "%s", q); out[0].step = 1; snprintf(out[0].how, sizeof out[0].how, "exact name match"); *stepused = 1; return 1; }
    char qf[128]; sy_fold(q, ql, qf, sizeof qf); char qt[16][32]; int qn = sy_tokens(q, ql, qt, 16);
    SyNameHit *hits = NULL; int nh = 0, hc = 0;
    for (int step = 2; step <= 3 && !n; step++) {
        nh = 0;
        for (int s = 0; s < v->ns; s++) { const SyVSeg *c = &v->s[s]; if (!c->x.has) continue;
            for (uint32_t i = 0; i < c->x.names.n; i++) {
                uint32_t l; const char *p = store_strs_get(&c->x.names, i, &l); if (l == 0 || l > 120 || (l == ql && !memcmp(p, q, l))) continue;
                int score = -1;
                if (step == 2) {
                    char nf[128]; sy_fold(p, l, nf, sizeof nf);
                    if (!strcmp(nf, qf) && qf[0]) score = 0;
                    else if (qn) { char nt[16][32]; int nn = sy_tokens(p, l, nt, 16);
                        for (int a = 0; a + qn <= nn && score < 0; a++) { int ok = 1; for (int b = 0; b < qn; b++) if (strcmp(nt[a + b], qt[b])) { ok = 0; break; } if (ok) score = 1 + (nn - qn); } }
                } else if (ql >= 4 && (int)l >= (int)ql - 2 && (int)l <= (int)ql + 2) {
                    char lo[128], lq[128]; for (uint32_t k = 0; k < l; k++) lo[k] = (char)tolower((unsigned char)p[k]); for (size_t k = 0; k < ql; k++) lq[k] = (char)tolower((unsigned char)q[k]);
                    int d = sy_lev(lo, (int)l, lq, (int)ql); if (d <= 2) score = d;
                }
                if (score < 0) continue;
                int dup = 0; for (int k = 0; k < nh; k++) if (!strncmp(hits[k].name, p, l) && hits[k].name[l] == 0) { dup = 1; break; }
                if (dup) continue;
                if (nh == hc) { hc = hc ? hc * 2 : 64; hits = (SyNameHit *)realloc(hits, sizeof *hits * (size_t)hc); }
                memcpy(hits[nh].name, p, l); hits[nh].name[l] = 0; hits[nh++].score = score;
            } }
        if (nh > 1) qsort(hits, (size_t)nh, sizeof *hits, sy_hit_cmp);
        for (int k = 0; k < nh && n < max && n < 8; k++) {
            size_t l = strlen(hits[k].name); int d = sy_ndefs(v, hits[k].name, l, NULL), r = want == 2 ? sy_nrefs(v, hits[k].name, l) : 0; if (d <= 0 && r <= 0) continue;
            snprintf(out[n].name, sizeof out[n].name, "%s", hits[k].name); out[n].step = step;
            if (step == 2) snprintf(out[n].how, sizeof out[n].how, hits[k].score == 0 ? "no exact match; same name ignoring case and underscores" : "no exact match; '%s' is part of the identifier", q);
            else snprintf(out[n].how, sizeof out[n].how, "no exact or derived match; name within edit distance %d", hits[k].score);
            n++;
        }
        if (n) *stepused = step;
    }
    free(hits); return n;
}

/* ---- itens de resultado e saida (texto ou JSON) ---- */
enum { SYI_BRIEF = 1, SYI_NOCONF = 2 };
typedef struct { char path[400]; int line0, line1, col, conf, ver, depth, flags; char kind[16], name[100], encl[220], sig[200], vis[12], how[170]; char *snip;
    char peer_path[400]; int peer_line; char peer_where[160]; } SyItem;      /* peer_*: o outro lado de uma ligacao por chave (flow): arquivo, linha e "in simbolo (l0-l1)"; vazio nas demais consultas */
typedef struct { SyItem *v; int n, cap; } SyItems;
static SyItem *sy_item_new(SyItems *a) {
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 32; a->v = (SyItem *)realloc(a->v, sizeof(SyItem) * (size_t)a->cap); }
    SyItem *it = &a->v[a->n++]; memset(it, 0, sizeof *it); return it;
}
static void sy_items_free(SyItems *a) { for (int i = 0; i < a->n; i++) free(a->v[i].snip); free(a->v); memset(a, 0, sizeof *a); }
static const char *const SY_VIS[] = {"", "public", "exported", "file", "private", "protected"};
static void sy_cpy(char *d, size_t cap, const char *s, size_t l) { if (l + 1 > cap) l = cap - 1; memcpy(d, s, l); d[l] = 0; }
static void sy_set_path(SyItem *it, const char *p, uint32_t l) { sy_cpy(it->path, sizeof it->path, p, l); }
static void sy_set_snip(SyView *v, SyItem *it, const char *path, uint32_t plen, int l0, int l1, int maxl) {
    if (!sy_file_load(v, path, plen)) return;
    size_t cap = (size_t)maxl * 260 + 104; char *b = (char *)malloc(cap); size_t o = 0; char t[240];
    for (int l = l0; l <= l1 && l < l0 + maxl; l++) { if (!sy_line_text(v, l, t, sizeof t)) break; o += (size_t)snprintf(b + o, cap - o, "%s%d: %s", o ? "\n" : "", l, t); if (o >= cap) o = cap - 1; }      /* snprintf devolve o tamanho que caberia: sem o limite o fica alem do buffer */
    if (l1 >= l0 + maxl) { o += (size_t)snprintf(b + o, cap - o, "\n... (+%d lines; Read the file with offset=%d)", l1 - (l0 + maxl) + 1, l0 + maxl); if (o >= cap) o = cap - 1; }
    b[o] = 0; it->snip = b;
}
/* ---- API estruturada das consultas (fase U2 do UIOE) ----
 * Cada consulta e `sy_q_*` (aqui) ou `fl_q_*` (flow.c): recebe uma SyView ABERTA (sy_view_open; o chamador a fecha e pode reaproveitar a mesma visao em
 * varias consultas: ela vem do cache de indexctl.c e o estado derivado, como o mapa de modulos de `deps`, e dela), preenche `SyItems *out` (zerado
 * pelo chamador; liberar com sy_items_free) e o descritor `SyQ *meta`, e devolve o total de resultados (pode ser maior que out->n: o limite so corta
 * os itens). NAO imprime nada, nao toca stdout/stderr, nao usa estado global: pode ser chamada de qualquer funcao, em qualquer thread que tenha a
 * sua propria visao. `meta` e quem carrega tudo que a saida precisa (cascata, NOT FOUND, limites do conjunto indexado, dica, cabecalho), de modo
 * que os renderizadores nao precisam da visao:
 *   sy_render_text(sb, meta, out)  o texto de `katana coder <consulta>` (byte a byte), sy_render_json(sb, meta, out)  o de --json.
 * Itens: arquivo (path), linhas (line0..line1, col), kind, name, scope (encl), assinatura (sig), visibilidade, trecho (snip), confidence 0-100 (conf),
 * level (sy_level(conf)), how (por que), ver (linha reconferida no arquivo), depth. Os itens de flow que ligam dois lados trazem tambem o outro lado
 * (peer_path, peer_line, peer_where). A cadeia da cascata esta em meta->cand[0..ncand) (nome, passo 1 exato, 2 derivado, 3 provavel, e o motivo).
 * Consultas (vistas de fora so pelos wrappers coder_*_main e pela ferramenta Symbols em symtool.c):
 *   sy_q_def(v, nome, limit, lines, out, meta)          sy_q_refs / sy_q_callers / sy_q_uses(v, nome, limit, out, meta)    sy_q_callees(v, nome, limit, ...)
 *   sy_q_outline(v, arquivo, limit, ...)                sy_q_at(v, arquivo, linha, ...)                                    sy_q_deps(v, arquivo, limit, ...)
 *   sy_q_impact(v, nome, depth, nodes, limit, ...)      fl_q_key(v, chave, limit, ...)  fl_q_file(v, arquivo, linha, limit, ...)  fl_q_links(v, limit, ...)  fl_q_arg(v, arg, limit, ...) */
typedef struct {
    const char *query, *arg;          /* nome da consulta (literal) e o argumento como o chamador o deu (o texto/JSON o repete); sy_q_at e fl_q_*: argbuf, o chamador pode trocar */
    int total, step;                  /* total de resultados; passo da cascata que achou (0 = nao precisou, 1 exato, 2 derivado, 3 provavel) */
    char resolved[200], cascade[160]; /* nomes resolvidos pela cascata (ate 4) e o motivo */
    SyCand cand[8]; int ncand;        /* a cadeia da cascata: cada nome candidato com o passo e o motivo */
    char external[1500];              /* nomes chamados que nao tem definicao no conjunto (callees) ou imports sem arquivo (deps) */
    char hint[240];                   /* "Next: ..." da saida de texto */
    char head[4700];                  /* linha de cabecalho da saida de texto (outline, at), so quando ha resultado */
    char notfound[704];               /* o que nao foi achado ("no definition of 'x' in the indexed set"), para o NOT FOUND */
    char limits[700];                 /* o limite do conjunto indexado (quantos arquivos, o que nao e visto) */
    char partial[300], ignote[420];   /* fase 6: indice ainda parcial (o NOT FOUND diz "so far") e pastas ignoradas que poderiam ter o nome; vazios = saida de sempre */
    uint32_t nlive, nwith;            /* arquivos do conjunto e arquivos com simbolos */
    char argbuf[4500];
} SyQ;
static void sy_q_begin(SyQ *o, const char *query, const char *arg) { memset(o, 0, sizeof *o); o->query = query; o->arg = arg; }
static int sy_q_end(const SyView *v, SyQ *o, int total, const char *notfound) {
    o->total = total; snprintf(o->notfound, sizeof o->notfound, "%s", notfound); sy_limits(v, o->limits, sizeof o->limits); o->nlive = v->nlive; o->nwith = v->nwith; return total;
}
/* ---- escrita em CtSb (nenhuma saida vai para stdout daqui) ---- */
static void sy_sbf(CtSb *b, const char *fmt, ...) {
    char t[2048]; va_list ap; va_start(ap, fmt); int n = vsnprintf(t, sizeof t, fmt, ap); va_end(ap); if (n < 0) return;
    if ((size_t)n < sizeof t) { ct_sb_add(b, t, (size_t)n); return; }
    char *h = (char *)malloc((size_t)n + 1); va_start(ap, fmt); vsnprintf(h, (size_t)n + 1, fmt, ap); va_end(ap); ct_sb_add(b, h, (size_t)n); free(h);
}
static void sy_sb_jstr(CtSb *b, const char *s) {
    ct_sb_add(b, "\"", 1);
    for (; *s; s++) { unsigned char ch = (unsigned char)*s; if (ch == '"' || ch == '\\') { char t[2] = {'\\', (char)ch}; ct_sb_add(b, t, 2); } else if (ch < 0x20) sy_sbf(b, "\\u%04x", ch); else ct_sb_add(b, (const char *)&ch, 1); }
    ct_sb_add(b, "\"", 1);
}
static void sy_json_item(CtSb *b, const SyItem *it) {
    sy_sbf(b, "{\"path\":"); sy_sb_jstr(b, it->path); sy_sbf(b, ",\"line0\":%d,\"line1\":%d", it->line0, it->line1); if (it->col) sy_sbf(b, ",\"col\":%d", it->col);
    sy_sbf(b, ",\"kind\":"); sy_sb_jstr(b, it->kind); sy_sbf(b, ",\"name\":"); sy_sb_jstr(b, it->name);
    if (it->encl[0]) { sy_sbf(b, ",\"scope\":"); sy_sb_jstr(b, it->encl); } if (it->sig[0]) { sy_sbf(b, ",\"sig\":"); sy_sb_jstr(b, it->sig); } if (it->vis[0]) { sy_sbf(b, ",\"visibility\":"); sy_sb_jstr(b, it->vis); }
    if (!(it->flags & SYI_NOCONF)) { sy_sbf(b, ",\"confidence\":%d,\"level\":\"%s\",\"verified\":%s,\"how\":", it->conf, sy_level(it->conf), it->ver > 0 ? "true" : "false"); sy_sb_jstr(b, it->how); }
    if (it->depth) sy_sbf(b, ",\"depth\":%d", it->depth);
    if (it->snip) { sy_sbf(b, ",\"snippet\":"); sy_sb_jstr(b, it->snip); }
    sy_sbf(b, "}");
}
static void sy_render_json(CtSb *b, const SyQ *o, const SyItems *a) {
    sy_sbf(b, "{\"query\":"); sy_sb_jstr(b, o->query); sy_sbf(b, ",\"arg\":"); sy_sb_jstr(b, o->arg);
    if (o->resolved[0]) { sy_sbf(b, ",\"resolved\":"); sy_sb_jstr(b, o->resolved); }
    if (o->step) { sy_sbf(b, ",\"cascade_step\":%d,\"cascade\":", o->step); sy_sb_jstr(b, o->cascade); }
    sy_sbf(b, ",\"count\":%d,\"shown\":%d,\"items\":[", o->total, a->n);
    for (int i = 0; i < a->n; i++) { if (i) sy_sbf(b, ","); sy_json_item(b, &a->v[i]); }
    sy_sbf(b, "]");
    if (o->external[0]) { sy_sbf(b, ",\"external\":"); sy_sb_jstr(b, o->external); }
    if (!o->total) { char nf[1700]; snprintf(nf, sizeof nf, "NOT FOUND%s: %s. Cascade tried: exact name, same name ignoring case or as an identifier part, edit distance <= 2. %s%s%s%s%s", o->partial[0] ? " so far" : "", o->notfound, o->limits, o->partial[0] ? " " : "", o->partial, o->ignote[0] ? " " : "", o->ignote); sy_sbf(b, ",\"not_found\":"); sy_sb_jstr(b, nf); }
    sy_sbf(b, ",\"index\":{\"files\":%u,\"with_symbols\":%u}}\n", o->nlive, o->nwith);
}
static void sy_render_text(CtSb *b, const SyQ *o, const SyItems *a) {
    if (!o->total) {
        sy_sbf(b, "NOT FOUND%s: %s.\nCascade tried: exact name, same name ignoring case or as an identifier part, edit distance <= 2.\n%s\n", o->partial[0] ? " so far" : "", o->notfound, o->limits);
        if (o->partial[0]) sy_sbf(b, "%s\n", o->partial);
        if (o->ignote[0]) sy_sbf(b, "%s\n", o->ignote);
        return; }
    if (o->head[0]) sy_sbf(b, "%s\n", o->head);
    if (o->step > 1) sy_sbf(b, "No exact match for '%s'; %s\n", o->arg, o->cascade);
    for (int i = 0; i < a->n; i++) {
        const SyItem *it = &a->v[i]; int ind = it->depth * 2;
        if (it->flags & SYI_BRIEF) {
            sy_sbf(b, "%*s%d-%d %s %s", ind + 2, "", it->line0, it->line1, it->kind, it->name); if (it->sig[0]) sy_sbf(b, "  %s", it->sig); sy_sbf(b, "\n"); continue;
        }
        sy_sbf(b, "%*s%s:%d", ind, "", it->path, it->line0); if (it->line1 > it->line0) sy_sbf(b, "-%d", it->line1); if (it->col) sy_sbf(b, ":%d", it->col);
        sy_sbf(b, "  %s %s", it->kind, it->name); if (it->vis[0]) sy_sbf(b, " [%s]", it->vis); if (it->encl[0]) sy_sbf(b, "  %s", it->encl);
        if (!(it->flags & SYI_NOCONF)) sy_sbf(b, "   [confidence %d %s%s%s]", it->conf, sy_level(it->conf), it->how[0] ? "; " : "", it->how);
        sy_sbf(b, "\n");
        if (it->sig[0] && it->snip == NULL) sy_sbf(b, "%*s    %s\n", ind, "", it->sig);
        if (it->snip) { for (const char *p = it->snip; *p; ) { const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p); sy_sbf(b, "%*s    %.*s\n", ind, "", (int)l, p); if (!e) break; p = e + 1; } }
    }
    if (o->external[0]) sy_sbf(b, "+ external (no definition in the indexed set): %s\n", o->external);
    if (o->total > a->n) sy_sbf(b, "(%d more not shown; raise --limit)\n", o->total - a->n);
    if (o->hint[0]) sy_sbf(b, "%s\n", o->hint);
}
/* escreve o texto no stdout (wrappers `katana coder ...`) e libera o buffer */
static void sy_put(CtSb *b) { if (b->n) fwrite(b->p, 1, b->n, stdout); free(b->p); memset(b, 0, sizeof *b); }

/* ---- argumentos ---- */
typedef struct { const char *pos[3]; int npos, json, limit, lines, depth, nodes; } SyArgs;
static int sy_args(int argc, char **argv, SyArgs *a, int deflim) {
    memset(a, 0, sizeof *a); a->limit = deflim; a->lines = 3; a->depth = 0; a->nodes = 100;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) a->json = 1; else if (!strcmp(argv[i], "--body")) a->lines = 200;
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) a->limit = atoi(argv[++i]); else if (!strcmp(argv[i], "--lines") && i + 1 < argc) a->lines = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) a->depth = atoi(argv[++i]); else if (!strcmp(argv[i], "--nodes") && i + 1 < argc) a->nodes = atoi(argv[++i]);
        else if (a->npos < 3) a->pos[a->npos++] = argv[i];
    }
    if (a->limit < 1) a->limit = 1; if (a->lines < 1) a->lines = 1;
    return a->npos >= 1 ? 0 : -1;
}
static int sy_start(const char *cmd, const char *use, int argc, char **argv, SyArgs *a, SyView *v, int deflim) {
    if (sy_args(argc, argv, a, deflim) != 0) { fprintf(stderr, "uso: katana coder %s\n", use); return 2; }
    char err[300] = ""; if (sy_view_open(a->npos >= 2 ? a->pos[1] : ".", v, err, sizeof err) != 0) { fprintf(stderr, "%s: %s\n", cmd, err); return 1; }
    if (!v->any) { fprintf(stderr, "%s: the index has no symbol sections (built before they existed); delete %s and run `katana coder index` again\n", cmd, v->dir); sy_view_close(v); return 1; }
    return 0;
}
static void sy_cascade_note(SyQ *o, const SyCand *c, int nc, int step) {
    o->step = step; o->resolved[0] = 0; o->ncand = nc < 8 ? nc : 8; memcpy(o->cand, c, sizeof(SyCand) * (size_t)o->ncand);
    for (int i = 0; i < nc && i < 4; i++) { size_t l = strlen(o->resolved); snprintf(o->resolved + l, sizeof o->resolved - l, "%s%s", i ? ", " : "", c[i].name); }
    snprintf(o->cascade, sizeof o->cascade, "showing %s (%s)", o->resolved, c[0].how);
}
static void sy_fill_def(SyView *v, SyItem *it, const SySym *s, int lines) {
    sy_set_path(it, s->path, s->plen); it->line0 = (int)s->line0; it->line1 = (int)s->line1;
    snprintf(it->kind, sizeof it->kind, "%s", s->kind < CK_N ? CP_KINDN[s->kind] : "?"); sy_cpy(it->name, sizeof it->name, s->name, s->nl); sy_cpy(it->sig, sizeof it->sig, s->sig, s->sl);
    snprintf(it->vis, sizeof it->vis, "%s", SY_VIS[(s->pack & 0xFF) < 6 ? (s->pack & 0xFF) : 0]);
    if (lines > 0) sy_set_snip(v, it, s->path, s->plen, (int)s->line0, (int)s->line1, lines);
}

/* ---- def ---- */
static int sy_q_def(SyView *v, const char *name, int limit, int lines, SyItems *out, SyQ *o) {
    sy_q_begin(o, "def", name);
    SyCand cand[8]; int step, nc = sy_cascade(v, name, 1, cand, 8, &step); int total = 0;
    if (nc) sy_cascade_note(o, cand, nc, step);
    for (int c = 0; c < nc; c++) {
        SySyms d = {0}; sy_defs_of(v, cand[c].name, strlen(cand[c].name), &d); int ndef = 0; for (int i = 0; i < d.n; i++) if (!((d.s[i].pack >> 16) & CSF_DECL)) ndef++;
        for (int i = 0; i < d.n; i++) {
            const SySym *s = &d.s[i]; total++; if (out->n >= limit) continue;
            SyItem *it = sy_item_new(out); sy_fill_def(v, it, s, lines);
            int decl = (s->pack >> 16) & CSF_DECL; if (decl) snprintf(it->kind + strlen(it->kind), sizeof it->kind - strlen(it->kind), " (decl)");
            it->ver = sy_verify(v, s->path, s->plen, (int)s->line0, 3, s->name, s->nl);
            int nd = ndef ? ndef : d.n; it->conf = sy_conf(cand[c].step, nd, it->ver, decl ? -5 : 0);
            if (cand[c].step == 1) snprintf(it->how, sizeof it->how, "%s; %d definition%s%s", decl ? "declaration only (prototype)" : "name match", nd, nd == 1 ? "" : "s", it->ver > 0 ? "; line verified" : "; line NOT verified (file changed or unreadable)");
            else snprintf(it->how, sizeof it->how, "%s; %d definition%s", cand[c].how, nd, nd == 1 ? "" : "s");
        }
        free(d.s);
    }
    if (total) snprintf(o->hint, sizeof o->hint, "%d definition%s. Next: refs/callers/callees %s, or Read the file range above.", total, total == 1 ? "" : "s", cand[0].name);
    char nf[300]; snprintf(nf, sizeof nf, "no definition of '%s' in the indexed set", name);
    return sy_q_end(v, o, total, nf);
}

/* ---- deps ---- */
static const char *const SY_EXT[] = {"", ".h", ".c", ".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx", ".py", ".kt", ".java", ".go", ".rs", ".qml", ".sh", ".md", "/index.js", "/index.ts", "/index.tsx", "/index.jsx", "/index.mjs", "/__init__.py", "/mod.rs", NULL};
/* ---- resolucao de imports por linguagem (modulo -> arquivo) ----
 * Cada regra devolve os arquivos do conjunto indexado e a confianca; o mapa modulo->arquivo (namespaces/pacotes declarados pelos simbolos) e
 * montado aqui, na consulta, a partir das secoes do indice (o formato do armazenamento nao muda). Sem regra que sirva, cai no casamento por
 * caminho (sy_resolve_path). */
static void sy_mods_free(SyView *v) { SyMods *m = v->mods; if (!m) return; for (int i = 0; i < m->n; i++) free(m->e[i].k); free(m->e); for (int i = 0; i < m->ngo; i++) { free(m->gomod[i]); free(m->godir[i]); } free(m->gomod); free(m->godir); free(m); v->mods = NULL; }
static SyMods *sy_mods(const SyView *v) { SyView *w = (SyView *)v; if (!w->mods) w->mods = (SyMods *)calloc(1, sizeof(SyMods)); return w->mods; }
static uint32_t sy_dlang(const SyView *v, const StoreLiveDoc *d) { const SySec *x = &v->s[d->seg].x; return x->has && d->idx < x->docs.n ? st_get_u32(store_table_at(&x->docs, d->idx) + 16) : 0; }
static StoreLiveDoc *sy_lookup(const SyView *v, const char *p, size_t l) { if (!l || l > 1500) return NULL; StoreLiveDoc *d = store_live_find(&v->L, p, l); return d && !d->dead ? d : NULL; }
/* familia de linguagem: imports so resolvem para arquivos da mesma familia */
static int sy_lfam(uint32_t l) { switch (l) { case CL_JAVA: case CL_KT: case CL_SCALA: case CL_GROOVY: return 1; case CL_C: case CL_CPP: case CL_OBJC: return 2; case CL_JS: case CL_TS: case CL_VUE: return 3; default: return 100 + (int)l; } }
static void sy_pnorm(const char *in, char *out, size_t cap) {
    char tmp[1500]; snprintf(tmp, sizeof tmp, "%s", in); char *seg[100]; int ns = 0;
    for (char *p = strtok(tmp, "/"); p && ns < 100; p = strtok(NULL, "/")) { if (!strcmp(p, ".")) continue; if (!strcmp(p, "..")) { if (ns) ns--; continue; } seg[ns++] = p; }
    out[0] = 0; for (int i = 0; i < ns; i++) { size_t l = strlen(out); snprintf(out + l, cap - l, "%s%s", i ? "/" : "", seg[i]); }
}
/* lookup de <dir>/<rel><sufixo>; dir termina em '/' ou e vazio */
static StoreLiveDoc *sy_try(const SyView *v, const char *dir, const char *rel, const char *suf) {
    char b[1500], n[1500]; snprintf(b, sizeof b, "%s%s%s", dir, rel, suf); sy_pnorm(b, n, sizeof n); return sy_lookup(v, n, strlen(n));
}
static int sy_ns_cmp(const void *a, const void *b) { return strcmp(((const SyNsE *)a)->k, ((const SyNsE *)b)->k); }
static void sy_ns_add(SyMods *M, const char *nm, uint32_t nl, StoreLiveDoc *d) {
    if (!nl || nl > 300) return;
    if (M->n == M->cap) { M->cap = M->cap ? M->cap * 2 : 256; M->e = (SyNsE *)realloc(M->e, sizeof(SyNsE) * (size_t)M->cap); }
    char *k = (char *)malloc(nl + 1); size_t w = 0;
    for (uint32_t i = 0; i < nl; i++) { char c = nm[i]; if (c == '\\' || c == '/') c = '.'; else if (c == ':' && i + 1 < nl && nm[i + 1] == ':') { c = '.'; i++; } k[w++] = c; }
    k[w] = 0; M->e[M->n].k = k; M->e[M->n].d = d; M->n++;
}
/* le "module x" de um go.mod do projeto */
static void sy_go_mods(const SyView *v) {
    SyMods *M = sy_mods(v);
    for (int i = 0; i < v->L.n; i++) {
        const StoreLiveDoc *d = &v->L.e[i]; if (d->dead || d->d.plen < 6) continue;
        const char *p = d->d.path; uint32_t pl = d->d.plen; if (!(pl == 6 || p[pl - 7] == '/') || memcmp(p + pl - 6, "go.mod", 6)) continue;
        char fp[4800]; snprintf(fp, sizeof fp, "%s/%.*s", sy_root(v), (int)pl, p); FILE *f = fopen(fp, "r"); if (!f) continue;
        char ln[600]; char mod[400] = "";
        while (fgets(ln, sizeof ln, f)) { char *q = ln; while (*q == ' ' || *q == '\t') q++; if (!strncmp(q, "module", 6) && (q[6] == ' ' || q[6] == '\t')) { q += 6; while (*q == ' ' || *q == '\t' || *q == '"') q++; size_t l = strcspn(q, " \t\r\n\""); if (l && l < sizeof mod) { memcpy(mod, q, l); mod[l] = 0; } break; } }
        fclose(f); if (!mod[0]) continue;
        M->gomod = (char **)realloc(M->gomod, sizeof(char *) * (size_t)(M->ngo + 1)); M->godir = (char **)realloc(M->godir, sizeof(char *) * (size_t)(M->ngo + 1));
        M->gomod[M->ngo] = strdup(mod); char dd[1500]; snprintf(dd, sizeof dd, "%.*s", pl > 6 ? (int)pl - 6 : 0, p); M->godir[M->ngo] = strdup(dd); M->ngo++;
    }
}
static void sy_mods_build(const SyView *v) {
    SyMods *M = sy_mods(v);
    if (M->built) return; M->built = 1; sy_go_mods(v);
    for (int s = 0; s < v->ns; s++) {
        const SySec *x = &v->s[s].x; if (!x->has) continue;
        for (uint32_t r = 0; r < x->syms.n; r++) {
            const uint8_t *q = store_table_at(&x->syms, r); uint32_t kind = st_get_u32(q + 4), doc = st_get_u32(q + 44); if (doc >= v->s[s].sd.t.n || !v->s[s].live[doc]) continue;
            uint32_t lang = doc < x->docs.n ? st_get_u32(store_table_at(&x->docs, doc) + 16) : 0;
            int ok = (kind == CK_NAMESPACE && lang != CL_RS && st_get_u32(q + 8) == SY_NONE) || (lang == CL_EX && kind == CK_CLASS);
            if (!ok) continue;
            StoreDoc sd; if (!store_docs_get(&v->s[s].sd, doc, &sd)) continue; if (v->strip && sd.plen > v->strip) { sd.path += v->strip; sd.plen -= v->strip; } StoreLiveDoc *ld = sy_lookup(v, sd.path, sd.plen); if (!ld) continue;
            uint32_t nl; const char *nm = store_strs_get(&x->names, st_get_u32(q), &nl); sy_ns_add(M, nm, nl, ld);
        }
    }
    if (M->n) qsort(M->e, (size_t)M->n, sizeof(SyNsE), sy_ns_cmp);
}
/* documentos com o namespace exato; devolve quantos (ate cap) */
static int sy_ns_find(const SyView *v, const char *key, StoreLiveDoc **out, int cap, uint32_t fam) {
    SyMods *M = sy_mods(v);
    int lo = 0, hi = M->n; while (lo < hi) { int m = (lo + hi) / 2; if (strcmp(M->e[m].k, key) < 0) lo = m + 1; else hi = m; }
    int n = 0; for (int i = lo; i < M->n && !strcmp(M->e[i].k, key) && n < cap; i++) { int dup = 0; for (int j = 0; j < n; j++) if (out[j] == M->e[i].d) dup = 1; if (!dup) out[n++] = M->e[i].d; }
    (void)fam; return n;
}
static int sy_doc_has_sym(const SyView *v, const StoreLiveDoc *d, const char *nm, size_t nl) {
    const SySec *x = &v->s[d->seg].x; if (!x->has || d->idx >= x->docs.n) return 0;
    const uint8_t *dr = store_table_at(&x->docs, d->idx); uint32_t s0 = st_get_u32(dr), n = st_get_u32(dr + 4);
    for (uint32_t k = 0; k < n; k++) { uint32_t l; const char *s = store_strs_get(&x->names, st_get_u32(store_table_at(&x->syms, s0 + k)), &l); if (l == nl && !memcmp(s, nm, nl)) return 1; }
    return 0;
}
static const char *sy_stem(const StoreLiveDoc *d, char *o, size_t cap) {
    const char *p = d->d.path; uint32_t l = d->d.plen; uint32_t b = l; while (b && p[b - 1] != '/') b--;
    uint32_t e = l; for (uint32_t i = b; i < l; i++) if (p[i] == '.') e = i; if (e == b) e = l;
    snprintf(o, cap, "%.*s", (int)(e - b), p + b); return o;
}
static int sy_ext_is(const StoreLiveDoc *d, const char *ext) { size_t el = strlen(ext); return d->d.plen >= el && !memcmp(d->d.path + d->d.plen - el, ext, el); }
static int sy_dir_is(const StoreLiveDoc *d, const char *dir, size_t dl) {      /* dir sem '/' final; arquivo diretamente nele */
    if (d->d.plen <= dl + 1) return 0; if (dl && (memcmp(d->d.path, dir, dl) || d->d.path[dl] != '/')) return 0; return memchr(d->d.path + dl + (dl ? 1 : 0), '/', d->d.plen - dl - (dl ? 1 : 0)) == NULL;
}
/* sufixo de caminho em fronteira de '/': candidatos "<sufixo><ext>" para uma lista de extensoes; so arquivos da familia */
static int sy_suffix_find(const SyView *v, const char *suf, const char *const *exts, uint32_t fam, StoreLiveDoc **out, int cap) {
    size_t sl = strlen(suf); int n = 0; if (!sl) return 0;
    for (int i = 0; i < v->L.n && n < cap; i++) { StoreLiveDoc *d = &v->L.e[i]; if (d->dead) continue;
        for (int e = 0; exts[e]; e++) { size_t el = strlen(exts[e]), pl = d->d.plen; if (pl < sl + el) continue; if (memcmp(d->d.path + pl - el, exts[e], el)) continue;
            const char *s = d->d.path + pl - el - sl; if (memcmp(s, suf, sl)) continue; if (s != d->d.path && s[-1] != '/') continue;
            if (fam && sy_dlang(v, d) && (uint32_t)sy_lfam(sy_dlang(v, d)) != fam) continue; out[n++] = d; break; } }
    return n;
}
static int sy_set_how(char *why, size_t cap, const char *fmt, int n) { snprintf(why, cap, fmt, n, n == 1 ? "" : "s"); return 1; }

/* Python: ".b", "..", ".sub.c", "pkg.sub.c" */
static int sy_res_py(const SyView *v, const char *from, size_t fl, const char *t, StoreLiveDoc **out, int cap, int *conf, char *why, size_t wc) {
    char dir[1500]; size_t dl = fl; while (dl && from[dl - 1] != '/') dl--; snprintf(dir, sizeof dir, "%.*s", (int)dl, from);
    int dots = 0; while (t[dots] == '.') dots++;
    char rest[600]; snprintf(rest, sizeof rest, "%s", t + dots); for (char *p = rest; *p; p++) if (*p == '.') *p = '/';
    StoreLiveDoc *d = NULL;
    if (dots) {
        char base[1500]; snprintf(base, sizeof base, "%s", dir); for (int k = 1; k < dots; k++) { strcat(base, "../"); }
        if (rest[0]) { if ((d = sy_try(v, base, rest, ".py")) || (d = sy_try(v, base, rest, "/__init__.py"))) { out[0] = d; *conf = 90; snprintf(why, wc, "relative Python import, module found next to the importing file or in a parent package"); return 1; } }
        if (!rest[0] || strchr(t + dots, '.') == NULL) { if ((d = sy_try(v, base, "__init__", ".py"))) { out[0] = d; *conf = rest[0] ? 62 : 85; snprintf(why, wc, rest[0] ? "relative import of a name from the package: resolved to the package __init__.py (the name may be a submodule or an attribute)" : "relative import of the package itself (__init__.py)"); return 1; } }
        return 0;
    }
    if (!rest[0]) return 0;
    if ((d = sy_try(v, "", rest, ".py")) || (d = sy_try(v, "", rest, "/__init__.py"))) { out[0] = d; *conf = 82; snprintf(why, wc, "absolute Python import found from the project root"); return 1; }
    if (!strchr(rest, '/')) return 0;      /* modulo de um segmento so vale na raiz (senao 'import pathlib' casaria com um pathlib.py do projeto)
    */
    static const char *const ex[] = {".py", "/__init__.py", NULL}; char suf[700]; int n = 0;
    snprintf(suf, sizeof suf, "%s", rest); n = sy_suffix_find(v, suf, ex, sy_lfam(CL_PY), out, cap);
    if (n) { *conf = n == 1 ? (strchr(rest, '/') ? 74 : 60) : 44; sy_set_how(why, wc, "Python module path found under a source folder; %d file%s match", n); return n; }
    return 0;
}

/* Rust: crate::a::b, super::x, self::x, a::b, com chaves a::{b, c} */
static StoreLiveDoc *sy_rs_modfile(const SyView *v, const char *d) {     /* arquivo do modulo cuja pasta e d (sem '/' final) */
    StoreLiveDoc *x; char b[1500];
    if ((x = sy_try(v, d, "/", "mod.rs"))) return x;
    snprintf(b, sizeof b, "%s.rs", d); if ((x = sy_lookup(v, b, strlen(b)))) return x;
    if ((x = sy_try(v, d, "/", "lib.rs"))) return x; if ((x = sy_try(v, d, "/", "main.rs"))) return x; return NULL;
}
static int sy_res_rs(const SyView *v, const char *from, size_t fl, const char *text, StoreLiveDoc **out, int cap, int *conf, char *why, size_t wc) {
    char t[700]; snprintf(t, sizeof t, "%s", text); char *brace = strchr(t, '{'); char inner[500] = ""; if (brace) { snprintf(inner, sizeof inner, "%s", brace + 1); *brace = 0; char *cb = strchr(inner, '}'); if (cb) *cb = 0; }
    { size_t l = strlen(t); while (l && (t[l - 1] == ':' || t[l - 1] == '*')) t[--l] = 0; }
    char *as = strstr(t, " as "); if (as) *as = 0;
    char *seg[40]; int ns = 0; for (char *p = t; *p && ns < 40;) { seg[ns++] = p; char *q = strstr(p, "::"); if (!q) break; *q = 0; p = q + 2; }
    if (!ns) return 0;
    if (!strcmp(seg[0], "std") || !strcmp(seg[0], "core") || !strcmp(seg[0], "alloc")) return 0;
    char fdir[1500]; size_t dl = fl; while (dl && from[dl - 1] != '/') dl--; snprintf(fdir, sizeof fdir, "%.*s", dl ? (int)dl - 1 : 0, from);      /* pasta do arquivo, sem '/' final */
    const char *fb = from + dl; char mdir[1500];                      /* pasta do modulo deste arquivo */
    if (!strcmp(fb, "mod.rs") || !strcmp(fb, "main.rs") || !strcmp(fb, "lib.rs")) snprintf(mdir, sizeof mdir, "%s", fdir);
    else { char st[300]; snprintf(st, sizeof st, "%.*s", (int)(fl - dl > 3 ? fl - dl - 3 : 0), fb); snprintf(mdir, sizeof mdir, "%s%s%s", fdir, fdir[0] ? "/" : "", st); }
    char croot[1500]; croot[0] = 0; int haveroot = 0;
    { char cur[1500]; snprintf(cur, sizeof cur, "%s", fdir); for (int g = 0; g < 30; g++) { char b[1500]; snprintf(b, sizeof b, "%s%slib.rs", cur, cur[0] ? "/" : ""); StoreLiveDoc *a = sy_lookup(v, b, strlen(b)); snprintf(b, sizeof b, "%s%smain.rs", cur, cur[0] ? "/" : ""); StoreLiveDoc *m = sy_lookup(v, b, strlen(b));
        if (a || m) { snprintf(croot, sizeof croot, "%s", cur); haveroot = 1; break; } char *sl = strrchr(cur, '/'); if (!sl) { if (!cur[0]) break; cur[0] = 0; continue; } *sl = 0; } }
    char D[1500]; int k = 0, how = 0;
    if (!strcmp(seg[0], "crate")) { if (!haveroot) return 0; snprintf(D, sizeof D, "%s", croot); k = 1; how = 1; }
    else if (!strcmp(seg[0], "self")) { snprintf(D, sizeof D, "%s", mdir); k = 1; how = 2; }
    else if (!strcmp(seg[0], "super")) { snprintf(D, sizeof D, "%s", mdir); while (k < ns && !strcmp(seg[k], "super")) { char *sl = strrchr(D, '/'); if (sl) *sl = 0; else D[0] = 0; k++; } how = 3; }
    else { char b1[1500]; snprintf(b1, sizeof b1, "%s%s%s", mdir, mdir[0] ? "/" : "", seg[0]); char b2[1500]; snprintf(b2, sizeof b2, "%s%s%s.rs", mdir, mdir[0] ? "/" : "", seg[0]);
        char b3[1500]; snprintf(b3, sizeof b3, "%s%s%s/mod.rs", mdir, mdir[0] ? "/" : "", seg[0]);
        if (sy_lookup(v, b2, strlen(b2)) || sy_lookup(v, b3, strlen(b3))) { snprintf(D, sizeof D, "%s", mdir); k = 0; how = 4; }
        else if (haveroot) { snprintf(b2, sizeof b2, "%s%s%s.rs", croot, croot[0] ? "/" : "", seg[0]); snprintf(b3, sizeof b3, "%s%s%s/mod.rs", croot, croot[0] ? "/" : "", seg[0]); if (sy_lookup(v, b2, strlen(b2)) || sy_lookup(v, b3, strlen(b3))) { snprintf(D, sizeof D, "%s", croot); k = 0; how = 5; } else return 0; }
        else return 0; }
    StoreLiveDoc *last = NULL; int resolved = 0;
    for (; k < ns; k++) {
        char b[1500]; StoreLiveDoc *x; snprintf(b, sizeof b, "%s%s%s.rs", D, D[0] ? "/" : "", seg[k]);
        if ((x = sy_lookup(v, b, strlen(b)))) { last = x; resolved++; snprintf(b, sizeof b, "%s%s%s", D, D[0] ? "/" : "", seg[k]); snprintf(D, sizeof D, "%s", b); continue; }
        snprintf(b, sizeof b, "%s%s%s/mod.rs", D, D[0] ? "/" : "", seg[k]);
        if ((x = sy_lookup(v, b, strlen(b)))) { last = x; resolved++; snprintf(b, sizeof b, "%s%s%s", D, D[0] ? "/" : "", seg[k]); snprintf(D, sizeof D, "%s", b); continue; }
        break;
    }
    int n = 0, all = k >= ns;
    if (inner[0] && last != NULL) {      /* a::{b, c}: cada nome pode ser submodulo */
        for (char *p = strtok(inner, ","); p && n < cap; p = strtok(NULL, ",")) { while (*p == ' ') p++; char nm[200]; size_t l = strcspn(p, ": "); if (!l || l >= sizeof nm) continue; memcpy(nm, p, l); nm[l] = 0; char b[1500]; StoreLiveDoc *x;
            snprintf(b, sizeof b, "%s/%s.rs", D, nm); if ((x = sy_lookup(v, b, strlen(b)))) { out[n++] = x; continue; } snprintf(b, sizeof b, "%s/%s/mod.rs", D, nm); if ((x = sy_lookup(v, b, strlen(b)))) out[n++] = x; }
        if (n) { *conf = 88; snprintf(why, wc, "Rust use path resolved through the module tree (crate/self/super and mod files)"); return n; }
    }
    if (!last) {     /* so crate/self/super: o proprio arquivo do modulo */
        StoreLiveDoc *m = k >= ns || (how != 4 && how != 5) ? sy_rs_modfile(v, D) : NULL; if (!m) return 0; out[0] = m; *conf = 78; snprintf(why, wc, "Rust path names the module itself or an item of it; resolved to the module file"); return 1;
    }
    out[0] = last; *conf = all || resolved ? (all ? 90 : 78) : 70;
    snprintf(why, wc, all ? "Rust use/mod path resolved through the module tree (crate/self/super and mod files)" : "Rust use path resolved through the module tree up to the module that holds the item (the item itself is a name in that file)");
    return 1;
}

/* Go: "modulo/pkg": pasta do pacote dentro do projeto (go.mod) */
static int sy_res_go(const SyView *v, const char *t, StoreLiveDoc **out, int cap, int *conf, char *why, size_t wc) {
    SyMods *M = sy_mods(v);
    char dir[1500]; int have = 0, bymod = 0;
    for (int i = 0; i < M->ngo && !have; i++) { size_t ml = strlen(M->gomod[i]);
        if (!strncmp(t, M->gomod[i], ml) && (t[ml] == 0 || t[ml] == '/')) { const char *gd = M->godir[i]; size_t gl = strlen(gd); if (gl && gd[gl - 1] == '/') gl--; snprintf(dir, sizeof dir, "%.*s%s%s", (int)gl, gd, gl && t[ml] ? "/" : "", t[ml] ? t + ml + 1 : ""); have = 1; bymod = 1; } }
    int n = 0, nt = 0;
    if (have) {
        size_t dl = strlen(dir); for (int i = 0; i < v->L.n && n < cap; i++) { StoreLiveDoc *d = &v->L.e[i]; if (d->dead || !sy_ext_is(d, ".go") || !sy_dir_is(d, dir, dl)) continue; if (sy_ext_is(d, "_test.go")) { nt++; continue; } out[n++] = d; }
        if (n) { *conf = n == 1 ? 90 : 82; snprintf(why, wc, "Go import path under the module declared in go.mod; package folder with %d file%s", n, n == 1 ? "" : "s"); return n; }
        return 0;
    }
    (void)bymod;
    /* sem go.mod que sirva: o ultimo trecho do caminho (2 ou mais componentes) como pasta dentro do projeto */
    int comps = 1; for (const char *p = t; *p; p++) if (*p == '/') comps++;
    if (comps < 2) return 0;
    for (int take = comps; take >= 2 && !n; take--) {
        const char *s = t; for (int k = 0; k < comps - take; k++) s = strchr(s, '/') + 1; size_t sl = strlen(s);
        for (int i = 0; i < v->L.n && n < cap; i++) { StoreLiveDoc *d = &v->L.e[i]; if (d->dead || !sy_ext_is(d, ".go") || sy_ext_is(d, "_test.go")) continue;
            uint32_t b = d->d.plen; while (b && d->d.path[b - 1] != '/') b--; if (b < sl + 1 || memcmp(d->d.path + b - 1 - sl, s, sl) || (b - 1 - sl > 0 && d->d.path[b - 2 - sl] != '/')) continue; out[n++] = d; }
    }
    if (n) { *conf = n == 1 ? 55 : 48; snprintf(why, wc, "Go import path matched to a package folder by its trailing components; no go.mod names this module; %d file%s", n, n == 1 ? "" : "s"); }
    return n;
}

/* linguagens de nome pontilhado: Java, Kotlin, Scala, Groovy, C#, Elixir, Clojure, PHP */
static int sy_res_dotted(const SyView *v, uint32_t lang, const char *text, StoreLiveDoc **out, int cap, int *conf, char *why, size_t wc) {
    char t[600]; size_t w = 0; for (const char *p = text; *p && w < sizeof t - 1; p++) { char c = *p; if (c == '\\') c = '.'; else if (c == ':' && p[1] == ':') { c = '.'; p++; } t[w++] = c; } t[w] = 0;
    while (w && (t[w - 1] == '.' || t[w - 1] == '*' || t[w - 1] == ';')) t[--w] = 0;
    if (!strncmp(t, "static.", 7)) memmove(t, t + 7, strlen(t + 6));
    if (!w || w > 400 || strchr(t, '/') || strchr(t, ' ') || strchr(t, '{') || strchr(t, ',')) return 0;
    if (lang == CL_PHP && !strchr(t, '.') && 0) return 0;
    uint32_t fam = (uint32_t)sy_lfam(lang);
    sy_mods_build(v);
    int n = 0;
    /* 1. o texto inteiro e um namespace/modulo declarado (using A.B; alias My.Foo; (ns my.util); use App\Svc) */
    if ((n = sy_ns_find(v, t, out, cap, fam))) { int ok = 0; for (int i = 0; i < n; i++) if ((uint32_t)sy_lfam(sy_dlang(v, out[i])) == fam) out[ok++] = out[i]; n = ok; if (n) { *conf = n == 1 ? 82 : 62; snprintf(why, wc, "name matches a namespace/module declared by %d indexed file%s", n, n == 1 ? "" : "s"); return n; } }
    /* 2. prefixo e namespace e o ultimo trecho e um simbolo ou o nome do arquivo (import x.y.S; use App\Svc\Foo) */
    char *dot = strrchr(t, '.');
    if (dot) {
        char pre[600]; snprintf(pre, sizeof pre, "%.*s", (int)(dot - t), t); const char *last = dot + 1;
        StoreLiveDoc *cd[24]; int nc = sy_ns_find(v, pre, cd, 24, fam), best = 0; StoreLiveDoc *pick[24];
        for (int i = 0; i < nc; i++) { if ((uint32_t)sy_lfam(sy_dlang(v, cd[i])) != fam) continue; char st[300]; if (sy_doc_has_sym(v, cd[i], last, strlen(last)) || !strcmp(sy_stem(cd[i], st, sizeof st), last)) pick[best++] = cd[i]; }
        if (best) { n = best < cap ? best : cap; for (int i = 0; i < n; i++) out[i] = pick[i]; *conf = n == 1 ? 84 : 58; snprintf(why, wc, "namespace '%s' is declared by an indexed file that defines '%s'", pre, last); return n; }
    }
    /* 3. caminho derivado do nome (pacote = pasta): com/foo/Bar.java, a/b/C.kt */
    static const char *const jx[] = {".java", ".kt", ".kts", ".scala", ".groovy", NULL}, *const cx[] = {".cs", NULL}, *const ex[] = {".ex", ".exs", NULL}, *const clx[] = {".clj", ".cljs", ".cljc", NULL}, *const px[] = {".php", NULL};
    const char *const *exts = lang == CL_CS ? cx : lang == CL_EX ? ex : lang == CL_CLJ ? clx : lang == CL_PHP ? px : jx;
    if (lang == CL_EX || lang == CL_CLJ) { /* Elixir: My.Foo -> my/foo.ex (minusculas); Clojure: my-util -> my_util */
        for (char *p = t; *p; p++) { if (lang == CL_CLJ && *p == '-') *p = '_'; if (lang == CL_EX) *p = (char)tolower((unsigned char)*p); } }
    char path[600]; snprintf(path, sizeof path, "%s", t); for (char *p = path; *p; p++) if (*p == '.') *p = '/';
    for (int drop = 0; drop < 3 && !n; drop++) {
        if (drop) { char *sl = strrchr(path, '/'); if (!sl) break; *sl = 0; }
        n = sy_suffix_find(v, path, exts, fam, out, cap);
        if (n) { *conf = (n == 1 ? 76 : 46) - drop * 6; snprintf(why, wc, "name mapped to a source path (package folders); %d file%s match", n, n == 1 ? "" : "s"); return n; }
        if (lang != CL_KT && lang != CL_JAVA && lang != CL_SCALA && lang != CL_GROOVY) break;
    }
    return n;
}

/* uma linguagem por vez: devolve quantos arquivos resolveu (0 = sem regra ou sem acerto: segue o casamento por caminho) */
static int sy_res_lang(const SyView *v, const char *from, size_t fl, uint32_t lang, const char *t, StoreLiveDoc **out, int cap, int *conf, char *why, size_t wc) {
    char dir[1500]; size_t dl = fl; while (dl && from[dl - 1] != '/') dl--; snprintf(dir, sizeof dir, "%.*s", (int)dl, from);
    StoreLiveDoc *d;
    switch (lang) {
    case CL_PY: return sy_res_py(v, from, fl, t, out, cap, conf, why, wc);
    case CL_RS: return sy_res_rs(v, from, fl, t, out, cap, conf, why, wc);
    case CL_GO: sy_mods_build(v); return sy_res_go(v, t, out, cap, conf, why, wc);
    case CL_JAVA: case CL_KT: case CL_SCALA: case CL_GROOVY: case CL_CS: case CL_EX: case CL_CLJ: return sy_res_dotted(v, lang, t, out, cap, conf, why, wc);
    case CL_PHP:
        if (strchr(t, '\\') || (!strchr(t, '/') && !strstr(t, ".php"))) return sy_res_dotted(v, lang, t, out, cap, conf, why, wc);
        return 0;
    case CL_RB:
        if (t[0] != '.' && !strchr(t, ':')) { if ((d = sy_try(v, dir, t, ".rb")) || (d = sy_try(v, dir, t, ""))) { out[0] = d; *conf = 85; snprintf(why, wc, "require_relative/require text resolved relative to the importing file (.rb)"); return 1; }
            static const char *const rx[] = {".rb", NULL}; char suf[400]; snprintf(suf, sizeof suf, "%s", t); int n = sy_suffix_find(v, suf, rx, sy_lfam(CL_RB), out, cap); if (n) { *conf = n == 1 ? 60 : 38; sy_set_how(why, wc, "require name matched as a .rb path suffix; %d file%s match", n); return n; } }
        return 0;
    case CL_DART:
        if (!strncmp(t, "dart:", 5)) return 0;
        if (!strncmp(t, "package:", 8)) { const char *p = strchr(t + 8, '/'); if (!p) return 0; char suf[600]; snprintf(suf, sizeof suf, "lib/%s", p + 1); static const char *const dx[] = {"", NULL}; int n = sy_suffix_find(v, suf, dx, sy_lfam(CL_DART), out, cap); if (!n) return 0; *conf = n == 1 ? 84 : 52; sy_set_how(why, wc, "package: import mapped to lib/ of an indexed package; %d file%s match", n); return n; }
        if ((d = sy_try(v, dir, t, ""))) { out[0] = d; *conf = 90; snprintf(why, wc, "Dart import resolved relative to the importing file"); return 1; }
        return 0;
    case CL_SWIFT: {
        char fam[300]; if (strlen(t) > 100 || strchr(t, '.')) return 0; int n = 0; (void)fam;
        for (int i = 0; i < v->L.n && n < cap; i++) { StoreLiveDoc *x = &v->L.e[i]; if (x->dead || !sy_ext_is(x, ".swift")) continue; char st[300]; if (!strcmp(sy_stem(x, st, sizeof st), t)) out[n++] = x; }
        if (n) { *conf = n == 1 ? 66 : 50; sy_set_how(why, wc, "Swift module name matched to a file of that name; %d file%s", n); return n; }
        size_t tl = strlen(t);
        for (int i = 0; i < v->L.n && n < cap; i++) { StoreLiveDoc *x = &v->L.e[i]; if (x->dead || !sy_ext_is(x, ".swift")) continue; uint32_t b = x->d.plen; while (b && x->d.path[b - 1] != '/') b--; if (b < tl + 2) continue; if (memcmp(x->d.path + b - 1 - tl, t, tl) || (b - 1 - tl > 0 && x->d.path[b - 2 - tl] != '/')) continue; out[n++] = x; }
        if (n) { *conf = 60; sy_set_how(why, wc, "Swift module name matched to a folder of source files; %d file%s", n); }
        return n; }
    case CL_SH: {
        if (t[0] != '$' && t[0] != '~') return 0;
        const char *r = t; if (r[0] == '~') r++; else if (r[1] == '{') { r = strchr(r, '}'); if (!r) return 0; r++; } else if (r[1] == '(') return 0; else { r++; while (isalnum((unsigned char)*r) || *r == '_') r++; }
        if (*r != '/') return 0; r++; while (r[0] == '.' && r[1] == '/') r += 2; if (!*r) return 0;
        if ((d = sy_try(v, dir, r, ""))) { out[0] = d; *conf = 80; snprintf(why, wc, "script path with a variable prefix; resolved relative to the calling script's folder"); return 1; }
        if ((d = sy_try(v, "", r, ""))) { out[0] = d; *conf = 76; snprintf(why, wc, "script path with a variable prefix; resolved from the project root"); return 1; }
        static const char *const sx[] = {"", NULL}; int n = sy_suffix_find(v, r, sx, 0, out, cap); if (n) { *conf = n == 1 ? 58 : 36; sy_set_how(why, wc, "script path with a variable prefix; path suffix match; %d file%s match", n); }
        return n; }
    default: return 0;
    }
}

/* casamento por caminho (js/ts/c e o que nao tem regra propria): devolve quantos e em *how 1 relativo ao arquivo, 2 sufixo de caminho */
static int sy_resolve_path(const SyView *v, const char *from, uint32_t flen, uint32_t lang, const char *text, uint32_t tl, StoreLiveDoc **out, int cap, int *how) {
    char t[600]; if (tl > 500) return 0; memcpy(t, text, tl); t[tl] = 0; *how = 0;
    if (lang == CL_PY && t[0] != '.' && !strchr(t, '/')) for (char *p = t; *p; p++) if (*p == '.') *p = '/';
    int relative = t[0] == '.' || lang == CL_C || lang == CL_CPP;
    if (!strchr(t, '/') && !strchr(t, '.') && lang != CL_PY && lang != CL_GO && lang != CL_JAVA && lang != CL_KT) return 0;
    char dir[600]; uint32_t dl = flen; while (dl && from[dl - 1] != '/') dl--;
    snprintf(dir, sizeof dir, "%.*s", dl > 500 ? 500 : (int)dl, from);
    char base[1200], norm[1200]; int n = 0;
    for (int pass = 0; pass < 2 && !n; pass++) {
        if (pass == 0) { if (!relative) continue; snprintf(base, sizeof base, "%s%s", dir, t); } else snprintf(base, sizeof base, "%s", t);
        /* normaliza ./ e ../ */
        char *seg[80]; int ns = 0; char tmp[1200]; snprintf(tmp, sizeof tmp, "%s", base);
        for (char *p = strtok(tmp, "/"); p && ns < 80; p = strtok(NULL, "/")) { if (!strcmp(p, ".")) continue; if (!strcmp(p, "..")) { if (ns) ns--; continue; } seg[ns++] = p; }
        norm[0] = 0; for (int i = 0; i < ns; i++) { size_t l = strlen(norm); snprintf(norm + l, sizeof norm - l, "%s%s", i ? "/" : "", seg[i]); }
        for (int e = 0; SY_EXT[e] && !n; e++) { char cand[1300]; snprintf(cand, sizeof cand, "%s%s", norm, SY_EXT[e]); StoreLiveDoc *d = store_live_find(&v->L, cand, strlen(cand)); if (d && !d->dead) { out[n++] = d; *how = 1; } }
    }
    if (n) return n;
    const char *suffix = norm[0] ? norm : t; size_t sl = strlen(suffix); if (!sl) return 0;
    for (int i = 0; i < v->L.n && n < cap; i++) { StoreLiveDoc *d = &v->L.e[i]; if (d->dead) continue;
        for (int e = 0; SY_EXT[e]; e++) { size_t el = strlen(SY_EXT[e]), pl = d->d.plen; if (pl < sl + el) continue; if (el && memcmp(d->d.path + pl - el, SY_EXT[e], el)) continue; if (!el && 0) continue;
            const char *s = d->d.path + pl - el - sl; if (memcmp(s, suffix, sl)) continue; if (s != d->d.path && s[-1] != '/') continue; out[n++] = d; *how = 2; break; } }
    return n;
}
#define SY_IMPCAP 64
/* resolve o texto de um import para arquivos do conjunto indexado: primeiro a regra da linguagem (sy_res_lang: pacotes, modulos, namespaces, go.mod),
   depois o casamento por caminho. *how: 1 relativo ao arquivo, 2 sufixo de caminho, 3 regra da linguagem (confianca e motivo em *conf e why) */
static int sy_resolve_import(const SyView *v, const char *from, uint32_t flen, uint32_t lang, const char *text, uint32_t tl, StoreLiveDoc **out, int cap, int *how, int *conf, char *why, size_t wc) {
    char t[600]; if (tl > 500) return 0; memcpy(t, text, tl); t[tl] = 0; *how = 0; *conf = 0; why[0] = 0;
    int n = sy_res_lang(v, from, flen, lang, t, out, cap, conf, why, wc);
    if (n) { *how = 3; return n; }
    n = sy_resolve_path(v, from, flen, lang, text, tl, out, cap, how);
    int own = lang == CL_PY || lang == CL_RS || lang == CL_GO || lang == CL_JAVA || lang == CL_KT || lang == CL_SCALA || lang == CL_GROOVY || lang == CL_CS || lang == CL_EX || lang == CL_CLJ || lang == CL_PHP || lang == CL_RB || lang == CL_DART || lang == CL_SWIFT || lang == CL_SH;
    if (n && own) { int w = 0; for (int i = 0; i < n; i++) if (!sy_dlang(v, out[i]) || sy_lfam(sy_dlang(v, out[i])) == sy_lfam(lang)) out[w++] = out[i]; n = w; }
    if (n && lang == CL_PY && *how == 2) n = 0;      /* os casos de Python por sufixo ja foram tentados com regra propria; o resto e modulo da biblioteca padrao ou de fora */
    if (!n) { *how = 0; return 0; }
    if (*how == 1) { *conf = n > 1 ? 60 : 85; snprintf(why, wc, "import text resolved relative to the importing file%s", n > 1 ? " (several extensions)" : ""); }
    else { *conf = n == 1 ? 60 : 30; snprintf(why, wc, "path suffix match; %d file%s match", n, n == 1 ? "" : "s"); }
    return n;
}
/* resolve um import (texto inteiro, depois cada pedaco separado por espaco) */
static int sy_resolve_imp_text(const SyView *v, const char *from, uint32_t flen, uint32_t lang, const char *name, uint32_t nl, StoreLiveDoc **m, int cap, int *how, int *conf, char *why, size_t wc) {
    char buf[600]; snprintf(buf, sizeof buf, "%.*s", nl < 590 ? (int)nl : 590, name);
    int nm = sy_resolve_import(v, from, flen, lang, buf, (uint32_t)strlen(buf), m, cap, how, conf, why, wc);
    if (!nm) { for (char *p = strtok(buf, " "); p && !nm; p = strtok(NULL, " ")) nm = sy_resolve_import(v, from, flen, lang, p, (uint32_t)strlen(p), m, cap, how, conf, why, wc); }
    return nm;
}
/* quem importa o arquivo tg: varre as referencias de import dos documentos vigentes; pre-filtro barato pelas chaves do alvo (nome do arquivo, da pasta, namespaces) */
typedef struct { SyRef f; int conf, nm; char why[100]; } SyImp;
static void sy_key_norm(const char *s, size_t l, char *o, size_t cap) { size_t w = 0; for (size_t i = 0; i < l && w + 1 < cap; i++) { char c = (char)tolower((unsigned char)s[i]); if (c == '-') c = '_'; else if (c == '\\') c = '.'; o[w++] = c; } o[w] = 0; }
static int sy_importers(SyView *v, const StoreLiveDoc *tg, SyImp **res) {
    int ncap = 32, nres = 0; SyImp *out = (SyImp *)malloc(sizeof(SyImp) * (size_t)ncap);
    const char *rel = tg->d.path; size_t rl = tg->d.plen;
    char keys[12][200]; int nk = 0; char stem[200];
    sy_stem(tg, stem, sizeof stem); sy_key_norm(stem, strlen(stem), keys[nk++], sizeof keys[0]);
    if (!strcmp(stem, "lib") || !strcmp(stem, "main") || !strcmp(stem, "mod") || !strcmp(stem, "__init__") || !strcmp(stem, "index") || sy_dlang(v, tg) == CL_GO || sy_dlang(v, tg) == CL_SWIFT) { const char *bs = rel + rl; while (bs > rel && bs[-1] != '/') bs--; if (bs > rel + 1) { const char *de = bs - 1, *ds = de; while (ds > rel && ds[-1] != '/') ds--; if (de > ds) sy_key_norm(ds, (size_t)(de - ds), keys[nk++], sizeof keys[0]); } }
    sy_mods_build(v); SyMods *M = sy_mods(v);
    for (int i = 0; i < M->n && nk < 12; i++) if (M->e[i].d == tg) sy_key_norm(M->e[i].k, strlen(M->e[i].k), keys[nk++], sizeof keys[0]);
    int rootmod = !strcmp(stem, "lib") || !strcmp(stem, "main") || !strcmp(stem, "mod") || !strcmp(stem, "__init__") || !strcmp(stem, "index");
    for (int s = 0; s < v->ns; s++) { const SyVSeg *c = &v->s[s]; if (!c->x.has) continue;
        for (uint32_t r = 0; r < c->x.refs.n; r++) {
            const uint8_t *q = store_table_at(&c->x.refs, r); if (st_get_u32(q + 4) != CR_IMPORT || !c->live[st_get_u32(q + 24)]) continue;
            uint32_t tl; const char *tx = store_strs_get(&c->x.names, st_get_u32(q), &tl);
            uint32_t dl2 = st_get_u32(store_table_at(&c->x.docs, st_get_u32(q + 24)) + 16);
            int hit = rootmod && (dl2 == CL_RS || dl2 == CL_PY);
            if (!hit) { char ln[620]; sy_key_norm(tx, tl < 600 ? tl : 600, ln, sizeof ln); for (int k = 0; k < nk && !hit; k++) if (keys[k][0] && strstr(ln, keys[k])) hit = 1; }
            if (!hit) continue;
            SyRef f; sy_ref_at(v, s, r, &f); if (f.plen == rl && !memcmp(f.path, rel, rl)) continue;
            StoreLiveDoc *m[SY_IMPCAP]; int how = 0, conf = 0; char why[100];
            int nm = sy_resolve_imp_text(v, f.path, f.plen, f.lang, tx, tl, m, SY_IMPCAP, &how, &conf, why, sizeof why);
            int ok = 0; for (int i = 0; i < nm; i++) if (m[i] == tg) ok = 1; if (!ok) continue;
            if (nres == ncap) { ncap *= 2; out = (SyImp *)realloc(out, sizeof(SyImp) * (size_t)ncap); }
            out[nres].f = f; out[nres].nm = nm; out[nres].conf = (how == 3 && nm > 1) ? (conf > 60 ? 60 : conf) : conf; snprintf(out[nres].why, sizeof out[nres].why, "%s", why); nres++;
        }
    }
    *res = out; return nres;
}
/* arquivos que importam algum dos arquivos que definem o nome (ate 4 arquivos): um chamador nesses arquivos tem ligacao por import com a definicao */
static int sy_link_build(SyView *v, const SySyms *d, SyImp **lk) {
    int n = 0; *lk = NULL; const StoreLiveDoc *seen[4]; int ns = 0;
    for (int i = 0; i < d->n && ns < 4; i++) {
        if ((d->s[i].pack >> 16) & CSF_DECL) continue; StoreLiveDoc *t = sy_lookup(v, d->s[i].path, d->s[i].plen); if (!t) continue;
        int dup = 0; for (int k = 0; k < ns; k++) if (seen[k] == t) dup = 1; if (dup) continue; seen[ns++] = t;
        SyImp *im = NULL; int m = sy_importers(v, t, &im); if (!m) { free(im); continue; }
        *lk = (SyImp *)realloc(*lk, sizeof(SyImp) * (size_t)(n + m)); memcpy(*lk + n, im, sizeof(SyImp) * (size_t)m); n += m; free(im);
    }
    return n;
}
static int sy_linked(const SyImp *lk, int n, const char *path, uint32_t pl) { for (int i = 0; i < n; i++) if (lk[i].f.plen == pl && !memcmp(lk[i].f.path, path, pl)) return 1; return 0; }
/* ---- refs, callers, uses ---- */
static int sy_q_refs_as(SyView *v, const char *cmd, uint32_t mask, const char *name, int limit, SyItems *out, SyQ *o) {
    sy_q_begin(o, cmd, name);
    SyCand cand[8]; int step, nc = sy_cascade(v, name, 2, cand, 8, &step); int total = 0;
    if (nc) sy_cascade_note(o, cand, nc, step);
    for (int c = 0; c < nc; c++) {
        size_t cl = strlen(cand[c].name); SySyms d = {0}; sy_defs_of(v, cand[c].name, cl, &d); int ndef = 0; for (int i = 0; i < d.n; i++) if (!((d.s[i].pack >> 16) & CSF_DECL)) ndef++;
        SyRefs r = {0}; sy_refs_of(v, cand[c].name, cl, mask, &r); SyImp *lk = NULL; int nlk = r.n ? sy_link_build(v, &d, &lk) : 0;
        for (int i = 0; i < r.n; i++) {
            const SyRef *f = &r.r[i]; total++; if (out->n >= limit) continue;
            SyItem *it = sy_item_new(out); sy_set_path(it, f->path, f->plen); it->line0 = it->line1 = (int)f->line; it->col = (int)f->col;
            snprintf(it->kind, sizeof it->kind, "%s", f->kind < CR_N ? CP_REFN[f->kind] : "?"); sy_cpy(it->name, sizeof it->name, f->name, f->nl);
            if (f->from != SY_NONE) { SySym e; sy_sym_at(v, f->seg, st_get_u32(store_table_at(&v->s[f->seg].x.docs, f->doc)) + f->from, &e); snprintf(it->encl, sizeof it->encl, "in %.*s (%u-%u)", (int)(e.nl < 80 ? e.nl : 80), e.name, e.line0, e.line1); }
            else snprintf(it->encl, sizeof it->encl, "at file level");
            int same = 0; for (int k = 0; k < d.n; k++) if (d.s[k].plen == f->plen && !memcmp(d.s[k].path, f->path, f->plen)) same = 1;
            it->ver = sy_verify(v, f->path, f->plen, (int)f->line, 0, f->name, f->nl);
            if (f->kind == CR_IMPORT) { it->ver = 0; it->conf = 70; snprintf(it->how, sizeof it->how, "import statement text"); }
            else {
                int linked = !same && ndef > 1 && sy_linked(lk, nlk, f->path, f->plen);      /* o arquivo do chamador importa o arquivo da definicao */
                int adj = ndef == 0 ? -25 : 0, nd = same || linked ? 1 : ndef;
                it->conf = sy_conf(cand[c].step, nd, it->ver, adj);
                const char *ver = it->ver > 0 ? "line verified" : "line NOT verified";
                if (ndef == 0) snprintf(it->how, sizeof it->how, "name only; no definition in the indexed set; %s", ver);
                else if (same) snprintf(it->how, sizeof it->how, "name match in same file as a definition; %s", ver);
                else if (linked) snprintf(it->how, sizeof it->how, "same name, %d definitions; this file imports the defining file; %s", ndef, ver);
                else snprintf(it->how, sizeof it->how, "same name, %d definition%s; %s", ndef, ndef == 1 ? "" : "s", ver);
                if (cand[c].step > 1) { size_t l = strlen(it->how); snprintf(it->how + l, sizeof it->how - l, "; step %d", cand[c].step); }
            }
            char t[240]; if (sy_file_load(v, f->path, f->plen) && sy_line_text(v, (int)f->line, t, sizeof t)) { it->snip = (char *)malloc(strlen(t) + 16); sprintf(it->snip, "%u: %s", f->line, t); }
        }
        free(r.r); free(d.s); free(lk);
    }
    if (total) snprintf(o->hint, sizeof o->hint, "%d occurrence%s. Next: def %s for the definition, impact %s for the closure of callers.", total, total == 1 ? "" : "s", cand[0].name, cand[0].name);
    char nf[300]; snprintf(nf, sizeof nf, "no %s of '%s' in the indexed set", mask == (1u << CR_CALL) ? "caller" : "reference", name);
    return sy_q_end(v, o, total, nf);
}

/* ---- callees ---- */
static int sy_q_callees(SyView *v, const char *name, int limit, SyItems *out, SyQ *o) {
    sy_q_begin(o, "callees", name);
    SyCand cand[8]; int step, nc = sy_cascade(v, name, 1, cand, 8, &step); int total = 0;
    if (nc) sy_cascade_note(o, cand, nc, step);
    for (int c = 0; c < nc; c++) {
        SySyms d = {0}; sy_defs_of(v, cand[c].name, strlen(cand[c].name), &d); int ndef = 0; for (int i = 0; i < d.n; i++) if (!((d.s[i].pack >> 16) & CSF_DECL)) ndef++;
        for (int di = 0; di < d.n; di++) {
            const SySym *D = &d.s[di]; if ((D->pack >> 16) & CSF_DECL) continue;
            const SySec *x = &v->s[D->seg].x; const uint8_t *dr = store_table_at(&x->docs, D->doc); uint32_t s0 = st_get_u32(dr), r0 = st_get_u32(dr + 8), nr = st_get_u32(dr + 12);
            char seen[400][64]; int nseen = 0;
            for (uint32_t k = 0; k < nr; k++) {
                const SyRef *f; SyRef fr; sy_ref_at(v, D->seg, r0 + k, &fr); f = &fr;
                if (f->kind != CR_CALL || f->from == SY_NONE) continue;
                int inside = 0; for (uint32_t p = f->from; p != SY_NONE && !inside; ) { if (p == D->rel) inside = 1; else p = st_get_u32(store_table_at(&x->syms, s0 + p) + 8); }
                if (!inside) continue;
                int dup = 0; for (int q = 0; q < nseen; q++) if (!strncmp(seen[q], f->name, 63) && strlen(f->name) == f->nl && f->nl < 64) dup = 1; if (dup) continue;
                if (nseen < 400 && f->nl < 64) { memcpy(seen[nseen], f->name, f->nl); seen[nseen++][f->nl] = 0; }
                SySyms td = {0}; sy_defs_of(v, f->name, f->nl, &td); int tn = 0, pick = -1;
                for (int q = 0; q < td.n; q++) if (!((td.s[q].pack >> 16) & CSF_DECL)) { tn++; if (pick < 0 || (td.s[q].plen == D->plen && !memcmp(td.s[q].path, D->path, D->plen))) pick = q; }
                total++;
                if (tn == 0) { size_t l = strlen(o->external); if (l < 1300) snprintf(o->external + l, sizeof o->external - l, "%s%.*s", l ? ", " : "", (int)f->nl, f->name); free(td.s); total--; continue; }
                if (out->n < limit) {
                    SyItem *it = sy_item_new(out); sy_set_path(it, D->path, D->plen); it->line0 = it->line1 = (int)f->line; it->col = (int)f->col; snprintf(it->kind, sizeof it->kind, "call"); sy_cpy(it->name, sizeof it->name, f->name, f->nl);
                    int same = td.s[pick].plen == D->plen && !memcmp(td.s[pick].path, D->path, D->plen);
                    snprintf(it->encl, sizeof it->encl, "-> %.*s:%u-%u (%s)", (int)td.s[pick].plen, td.s[pick].path, td.s[pick].line0, td.s[pick].line1, CP_KINDN[td.s[pick].kind < CK_N ? td.s[pick].kind : 0]);
                    it->ver = sy_verify(v, D->path, D->plen, (int)f->line, 0, f->name, f->nl); it->conf = sy_conf(cand[c].step, same ? 1 : tn, it->ver, 0);
                    if (same) snprintf(it->how, sizeof it->how, "name match in same file; called from %.*s (%u-%u); %s", (int)D->nl, D->name, D->line0, D->line1, it->ver > 0 ? "line verified" : "line NOT verified");
                    else snprintf(it->how, sizeof it->how, "same name, %d definition%s; called from %.*s (%u-%u); %s", tn, tn == 1 ? "" : "s", (int)D->nl, D->name, D->line0, D->line1, it->ver > 0 ? "line verified" : "line NOT verified");
                    char t[240]; if (sy_file_load(v, D->path, D->plen) && sy_line_text(v, (int)f->line, t, sizeof t)) { it->snip = (char *)malloc(strlen(t) + 16); sprintf(it->snip, "%u: %s", f->line, t); }
                }
                free(td.s);
            }
        }
        (void)ndef; free(d.s);
    }
    if (total) snprintf(o->hint, sizeof o->hint, "%d callee%s in the indexed set (calls in source order within each definition). Next: def <callee>, or impact %s.", total, total == 1 ? "" : "s", cand[0].name);
    char nf[300]; snprintf(nf, sizeof nf, "'%s' has no definition with calls to indexed definitions in the indexed set%s", name, o->external[0] ? " (it only calls external names, listed in the JSON field external)" : "");
    return sy_q_end(v, o, total, nf);
}

/* ---- arquivo do indice ---- */
static int sy_find_doc(const SyView *v, const char *arg, char *rel, size_t cap, int *seg, uint32_t *idx, int *nsuffix) {
    char rp[4400]; const char *p = arg; *nsuffix = 0;
    if (realpath(arg, rp)) { const char *root = sy_root(v); size_t rl = strlen(root); if (!strncmp(rp, root, rl) && rp[rl] == '/') p = rp + rl + 1; }
    while (p[0] == '.' && p[1] == '/') p += 2;
    snprintf(rel, cap, "%s", p);
    StoreLiveDoc *e = store_live_find(&v->L, rel, strlen(rel));
    if (e && !e->dead) { *seg = e->seg; *idx = e->idx; return 1; }
    size_t pl = strlen(rel); StoreLiveDoc *best = NULL; int n = 0;
    for (int i = 0; i < v->L.n; i++) { StoreLiveDoc *q = &v->L.e[i]; if (q->dead || q->d.plen <= pl || memcmp(q->d.path + q->d.plen - pl, rel, pl) || q->d.path[q->d.plen - pl - 1] != '/') continue; n++; if (!best) best = q; }
    if (best) { snprintf(rel, cap, "%.*s", (int)best->d.plen, best->d.path); *seg = best->seg; *idx = best->idx; *nsuffix = n; return 1; }
    return 0;
}
static const char *sy_langname(uint32_t l) { return l < CL_N ? CP_LANGN[l] : "?"; }

/* ---- outline ---- */
static int sy_q_outline(SyView *v, const char *file, int limit, SyItems *out, SyQ *o) {
    sy_q_begin(o, "outline", file);
    char rel[4400]; int seg, ns; uint32_t idx; int total = 0, okc = 0;
    char nf[600];
    if (!sy_find_doc(v, file, rel, sizeof rel, &seg, &idx, &ns)) { snprintf(nf, sizeof nf, "'%s' is not in the indexed set (not indexed, deleted, or outside the project folder)", file); return sy_q_end(v, o, 0, nf); }
    const SySec *x = &v->s[seg].x; const uint8_t *dr = x->has ? store_table_at(&x->docs, idx) : NULL; uint32_t s0 = dr ? st_get_u32(dr) : 0, n = dr ? st_get_u32(dr + 4) : 0, lang = dr ? st_get_u32(dr + 16) : 0;
    for (uint32_t k = 0; k < n; k++) {
        SySym s; sy_sym_at(v, seg, s0 + k, &s); total++; if (out->n >= limit) continue;
        SyItem *it = sy_item_new(out); sy_fill_def(v, it, &s, 0); it->flags = SYI_BRIEF;
        int d = 0; for (uint32_t p = s.parent; p != SY_NONE && d < 40; p = st_get_u32(store_table_at(&x->syms, s0 + p) + 8)) d++;
        it->depth = d; it->ver = sy_verify(v, s.path, s.plen, (int)s.line0, 3, s.name, s.nl); it->conf = sy_conf(1, 1, it->ver, 0); if (it->ver > 0) okc++;
        snprintf(it->how, sizeof it->how, "indexed symbol; line %s", it->ver > 0 ? "verified" : "NOT verified");
    }
    if (total) { snprintf(o->head, sizeof o->head, "%s  [%s]  %d symbols; %d of %d declaration lines verified by rereading the file (confidence %d %s)", rel, sy_langname(lang), total, okc, out->n, sy_conf(1, 1, okc * 10 >= out->n * 9 ? 1 : -1, 0), sy_level(sy_conf(1, 1, okc * 10 >= out->n * 9 ? 1 : -1, 0))); }
    snprintf(nf, sizeof nf, "'%s' is in the indexed set but has no symbols (%s: text, configuration, unknown language, or nothing declared)", rel, sy_langname(lang));
    return sy_q_end(v, o, total, nf);
}

/* ---- at <arquivo>:<linha> ---- */
static int sy_q_at(SyView *v, const char *spec, int line, SyItems *out, SyQ *o) {
    sy_q_begin(o, "at", NULL); snprintf(o->argbuf, sizeof o->argbuf, "%s:%d", spec, line); o->arg = o->argbuf;
    char rel[4400]; int seg, ns; uint32_t idx; char nf[700];
    if (!sy_find_doc(v, spec, rel, sizeof rel, &seg, &idx, &ns)) { snprintf(nf, sizeof nf, "'%s' is not in the indexed set", spec); return sy_q_end(v, o, 0, nf); }
    const SySec *x = &v->s[seg].x; const uint8_t *dr = x->has ? store_table_at(&x->docs, idx) : NULL; uint32_t s0 = dr ? st_get_u32(dr) : 0, n = dr ? st_get_u32(dr + 4) : 0;
    int64_t k = -1;     /* ultimo simbolo com line0 <= linha (busca binaria), depois volta enquanto o maximo acumulado de line1 alcanca a linha */
    { uint32_t lo = 0, hi = n; while (lo < hi) { uint32_t m = lo + (hi - lo) / 2; if ((int)st_get_u32(store_table_at(&x->syms, s0 + m) + 12) <= line) lo = m + 1; else hi = m; } k = (int64_t)lo - 1; }
    int64_t hit = -1; while (k >= 0 && (int)st_get_u32(store_table_at(&x->ival, s0 + (uint32_t)k)) >= line) { if ((int)st_get_u32(store_table_at(&x->syms, s0 + (uint32_t)k) + 16) >= line) { hit = k; break; } k--; }
    int total = 0;
    if (hit >= 0) {
        uint32_t chain[64]; int nc = 0; for (uint32_t p = (uint32_t)hit; p != SY_NONE && nc < 64; p = st_get_u32(store_table_at(&x->syms, s0 + p) + 8)) chain[nc++] = p;
        for (int c = nc - 1; c >= 0; c--) {
            SySym s; sy_sym_at(v, seg, s0 + chain[c], &s); SyItem *it = sy_item_new(out); sy_fill_def(v, it, &s, 0); it->depth = nc - 1 - c; total++;
            it->ver = sy_verify(v, s.path, s.plen, (int)s.line0, 3, s.name, s.nl); it->conf = sy_conf(1, 1, it->ver, 0);
            snprintf(it->how, sizeof it->how, "%s; line range from the index, declaration line %s", c == 0 ? "innermost symbol containing the line" : "enclosing symbol", it->ver > 0 ? "verified" : "NOT verified");
        }
        char t[240]; if (sy_file_load(v, rel, (uint32_t)strlen(rel)) && sy_line_text(v, line, t, sizeof t)) snprintf(o->head, sizeof o->head, "%s:%d  %s", rel, line, t);
    }
    snprintf(nf, sizeof nf, "line %d of '%s' is not inside any indexed symbol (file level code, or the file has no symbols)", line, rel);
    return sy_q_end(v, o, total, nf);
}

static int sy_q_deps(SyView *v, const char *file, int limit, SyItems *out, SyQ *o) {
    sy_q_begin(o, "deps", file);
    char rel[4400]; int seg, ns; uint32_t idx; char nf[700]; int total = 0;
    if (!sy_find_doc(v, file, rel, sizeof rel, &seg, &idx, &ns)) { snprintf(nf, sizeof nf, "'%s' is not in the indexed set", file); return sy_q_end(v, o, 0, nf); }
    const SySec *x = &v->s[seg].x; const uint8_t *dr = x->has ? store_table_at(&x->docs, idx) : NULL; uint32_t r0 = dr ? st_get_u32(dr + 8) : 0, nr = dr ? st_get_u32(dr + 12) : 0, lang = dr ? st_get_u32(dr + 16) : 0;
    size_t rl = strlen(rel);
    for (uint32_t k = 0; k < nr; k++) {
        SyRef f; sy_ref_at(v, seg, r0 + k, &f); if (f.kind != CR_IMPORT) continue;
        /* o texto do import pode ter varios nomes separados por espaco (import { a, b } from): tenta o texto inteiro, depois cada pedaco */
        StoreLiveDoc *m[SY_IMPCAP]; int how = 0, conf = 0; char why[100];
        int nm = sy_resolve_imp_text(v, rel, (uint32_t)rl, lang, f.name, f.nl, m, SY_IMPCAP, &how, &conf, why, sizeof why);
        if (!nm) { size_t l = strlen(o->external); if (l < 1300) snprintf(o->external + l, sizeof o->external - l, "%s%.*s", l ? ", " : "", (int)(f.nl < 80 ? f.nl : 80), f.name); continue; }
        for (int q = 0; q < nm; q++) {
            total++; if (out->n >= limit) continue; SyItem *it = sy_item_new(out); sy_set_path(it, rel, (uint32_t)rl); it->line0 = it->line1 = (int)f.line; snprintf(it->kind, sizeof it->kind, "imports"); sy_cpy(it->name, sizeof it->name, f.name, f.nl);
            snprintf(it->encl, sizeof it->encl, "-> %.*s", (int)m[q]->d.plen, m[q]->d.path);
            it->conf = conf; if (nm > 1 && how != 3 && how == 1) it->conf = 60; if (nm > 1 && how == 3 && it->conf > 60) it->conf = 60;
            snprintf(it->how, sizeof it->how, "%s", why);
        }
    }
    /* quem importa este arquivo */
    StoreLiveDoc *self = sy_lookup(v, rel, rl); SyImp *imp = NULL; int nimp = self ? sy_importers(v, self, &imp) : 0;
    for (int i = 0; i < nimp; i++) {
        const SyRef *f = &imp[i].f; total++; if (out->n >= limit) continue;
        SyItem *it = sy_item_new(out); sy_set_path(it, f->path, f->plen); it->line0 = it->line1 = (int)f->line; snprintf(it->kind, sizeof it->kind, "imported-by"); sy_cpy(it->name, sizeof it->name, f->name, f->nl);
        snprintf(it->encl, sizeof it->encl, "<- %.*s", (int)f->plen, f->path); it->conf = imp[i].conf; snprintf(it->how, sizeof it->how, "%s", imp[i].why);
    }
    free(imp);
    if (total) snprintf(o->hint, sizeof o->hint, "%d relation%s (imports out and imported-by). Import texts are resolved to files by language rules (packages, modules, namespaces, go.mod) or by path.", total, total == 1 ? "" : "s");
    snprintf(nf, sizeof nf, "'%s' has no import relation to another file of the indexed set (it imports nothing indexed and no indexed file imports it)", rel);
    return sy_q_end(v, o, total, nf);
}

/* ---- impact: fecho de chamadores ate a profundidade N, com limite de nos ---- */
typedef struct { int seg; uint32_t row; char path[400]; char name[100]; uint32_t l0, l1; int depth, conf, parent; uint32_t cline; char calls[100]; } SyNode;
static int sy_q_impact(SyView *v, const char *name, int depth, int nodes, int limit, SyItems *out, SyQ *o) {
    sy_q_begin(o, "impact", name);
    int maxd = depth > 0 ? depth : 3, maxn = nodes > 0 ? nodes : 100;
    SyCand cand[8]; int step, nc = sy_cascade(v, name, 2, cand, 8, &step); 
    SyNode *nd = (SyNode *)calloc((size_t)maxn + 1, sizeof *nd); int nn = 0, head = 0, truncated = 0, total = 0;
    if (nc) sy_cascade_note(o, cand, nc, step);
    /* raizes: a(s) definicao(oes) do nome; os chamadores buscam-se pelo nome */
    typedef struct { char name[128]; int depth, conf; int node; } Q; Q *q = (Q *)calloc((size_t)maxn + 8, sizeof(Q)); int qh = 0, qt = 0;
    if (nc) { snprintf(q[qt].name, sizeof q[qt].name, "%s", cand[0].name); q[qt].depth = 0; q[qt].conf = sy_conf(cand[0].step, 1, 0, 0) + 4; q[qt].node = -1; qt++; }
    while (qh < qt) {
        Q cur = q[qh++]; if (cur.depth >= maxd) continue;
        size_t cl = strlen(cur.name); SyRefs r = {0}; sy_refs_of(v, cur.name, cl, 1u << CR_CALL, &r); SySyms d = {0}; sy_defs_of(v, cur.name, cl, &d);
        int ndef = 0; for (int i = 0; i < d.n; i++) if (!((d.s[i].pack >> 16) & CSF_DECL)) ndef++;
        SyImp *lk = NULL; int nlk = r.n && ndef > 1 ? sy_link_build(v, &d, &lk) : 0;
        for (int i = 0; i < r.n; i++) {
            const SyRef *f = &r.r[i]; if (f->from == SY_NONE) continue;
            uint32_t row = st_get_u32(store_table_at(&v->s[f->seg].x.docs, f->doc)) + f->from; int dup = 0;
            for (int k = 0; k < nn; k++) if (nd[k].seg == f->seg && nd[k].row == row) { dup = 1; break; } if (dup) continue;
            if (nn >= maxn) { truncated = 1; break; }
            SySym s; sy_sym_at(v, f->seg, row, &s); SyNode *N = &nd[nn]; N->seg = f->seg; N->row = row; sy_cpy(N->path, sizeof N->path, s.path, s.plen); sy_cpy(N->name, sizeof N->name, s.name, s.nl);
            N->l0 = s.line0; N->l1 = s.line1; N->depth = cur.depth + 1; N->parent = cur.node; N->cline = f->line; snprintf(N->calls, sizeof N->calls, "%s", cur.name);
            int same = 0; for (int k = 0; k < d.n; k++) if (d.s[k].plen == f->plen && !memcmp(d.s[k].path, f->path, f->plen)) same = 1;
            int ver = sy_verify(v, f->path, f->plen, (int)f->line, 0, f->name, f->nl);
            int linked = !same && sy_linked(lk, nlk, f->path, f->plen); int edge = sy_conf(cand[0].step, same || linked ? 1 : ndef, ver, ndef == 0 ? -25 : 0); N->conf = edge < cur.conf ? edge : cur.conf;
            if (qt < maxn + 7) { snprintf(q[qt].name, sizeof q[qt].name, "%s", N->name); q[qt].depth = N->depth; q[qt].conf = N->conf; q[qt].node = nn; qt++; }
            nn++;
        }
        free(r.r); free(d.s); free(lk);
        if (truncated) break;
    }
    /* ordem de saida: arvore (em profundidade) a partir dos filhos da raiz */
    int *order = (int *)malloc(sizeof(int) * (size_t)(nn + 1)), no = 0; int *stack = (int *)malloc(sizeof(int) * (size_t)(nn + 2)); int sp = 0;
    for (int i = nn - 1; i >= 0; i--) if (nd[i].parent < 0) stack[sp++] = i;
    while (sp) { int i = stack[--sp]; order[no++] = i; for (int k = nn - 1; k >= 0; k--) if (nd[k].parent == i) stack[sp++] = k; }
    for (int oi = 0; oi < no; oi++) {
        const SyNode *N = &nd[order[oi]]; total++; if (out->n >= limit) continue;
        SyItem *it = sy_item_new(out); sy_cpy(it->path, sizeof it->path, N->path, strlen(N->path)); it->line0 = (int)N->l0; it->line1 = (int)N->l1; it->depth = N->depth - 1;
        snprintf(it->kind, sizeof it->kind, "caller"); snprintf(it->name, sizeof it->name, "%s", N->name); snprintf(it->encl, sizeof it->encl, "calls %s at line %u", N->calls, N->cline);
        it->conf = N->conf; it->ver = 0; snprintf(it->how, sizeof it->how, "caller at depth %d; confidence is that of the weakest edge on the path (call sites matched by name)", N->depth);
    }
    if (total) snprintf(o->hint, sizeof o->hint, "%d node%s at depth <= %d%s. Callers are matched by name only; a path has the confidence of its weakest edge.", total, total == 1 ? "" : "s", maxd, truncated ? " (node limit reached; raise --nodes)" : "");
    char nf[300]; snprintf(nf, sizeof nf, "no caller of '%s' in the indexed set", name);
    free(nd); free(q); free(order); free(stack);
    return sy_q_end(v, o, total, nf);
}

/* ---- as consultas por tipo de referencia: refs (todas), callers (so chamadas), uses (tudo que nao e chamada) ---- */
static int sy_q_refs(SyView *v, const char *name, uint32_t kindmask, int limit, SyItems *out, SyQ *o) {
    return sy_q_refs_as(v, kindmask == (1u << CR_CALL) ? "callers" : kindmask == (~0u & ~(1u << CR_CALL)) ? "uses" : "refs", kindmask, name, limit, out, o);
}
static int sy_q_callers(SyView *v, const char *name, int limit, SyItems *out, SyQ *o) { return sy_q_refs(v, name, 1u << CR_CALL, limit, out, o); }
static int sy_q_uses(SyView *v, const char *name, int limit, SyItems *out, SyQ *o) { return sy_q_refs(v, name, ~0u & ~(1u << CR_CALL), limit, out, o); }

/* "arquivo:linha" -> arquivo e linha; 0 se nao ha linha */
static int sy_split_at(const char *in, char *spec, size_t cap, int *line) {
    snprintf(spec, cap, "%s", in); *line = 0; char *colon = strrchr(spec, ':');
    if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)) { *line = atoi(colon + 1); *colon = 0; }
    return *line >= 1;
}

/* ---- wrappers `katana coder def|refs|callers|callees|uses|outline|at|deps|impact`: argumentos + sy_q_* + renderizador (texto ou --json) ---- */
enum { SYK_DEF, SYK_REFS, SYK_CALLEES, SYK_OUTLINE, SYK_AT, SYK_DEPS, SYK_IMPACT };
static int sy_cli(int kind, const char *cmd, const char *use, uint32_t mask, int deflim, int argc, char **argv) {
    SyArgs a; SyView v; int rc = sy_start(cmd, use, argc, argv, &a, &v, deflim); if (rc) return rc;
    SyItems items = {0}; SyQ q; CtSb sb = {0};
    switch (kind) {
    case SYK_DEF: sy_q_def(&v, a.pos[0], a.limit, a.lines, &items, &q); break;
    case SYK_REFS: sy_q_refs_as(&v, cmd, mask, a.pos[0], a.limit, &items, &q); break;
    case SYK_CALLEES: sy_q_callees(&v, a.pos[0], a.limit, &items, &q); break;
    case SYK_OUTLINE: sy_q_outline(&v, a.pos[0], a.limit, &items, &q); break;
    case SYK_AT: {
        char spec[4400]; int line;
        if (!sy_split_at(a.pos[0], spec, sizeof spec, &line)) { fprintf(stderr, "uso: katana coder at <arquivo>:<linha> [pasta]\n"); sy_view_close(&v); return 2; }
        sy_q_at(&v, spec, line, &items, &q); q.arg = a.pos[0]; break; }
    case SYK_DEPS: sy_q_deps(&v, a.pos[0], a.limit, &items, &q); break;
    default: sy_q_impact(&v, a.pos[0], a.depth, a.nodes, a.limit, &items, &q); break;
    }
    if (a.json) sy_render_json(&sb, &q, &items); else sy_render_text(&sb, &q, &items);
    sy_put(&sb); sy_items_free(&items); sy_view_close(&v); return q.total ? 0 : 1;
}
static int coder_def_main(int argc, char **argv) { return sy_cli(SYK_DEF, "def", "def <name> [pasta] [--json] [--lines N|--body] [--limit N]", 0, 50, argc, argv); }
static int coder_refs_main(int argc, char **argv) { return sy_cli(SYK_REFS, "refs", "refs <name> [pasta] [--json] [--limit N]", ~0u, 100, argc, argv); }
static int coder_callers_main(int argc, char **argv) { return sy_cli(SYK_REFS, "callers", "callers <name> [pasta] [--json] [--limit N]", 1u << CR_CALL, 100, argc, argv); }
static int coder_uses_main(int argc, char **argv) { return sy_cli(SYK_REFS, "uses", "uses <name> [pasta] [--json] [--limit N]  (every occurrence that is not a call)", ~0u & ~(1u << CR_CALL), 100, argc, argv); }
static int coder_callees_main(int argc, char **argv) { return sy_cli(SYK_CALLEES, "callees", "callees <name> [pasta] [--json] [--limit N]", 0, 100, argc, argv); }
static int coder_outline_main(int argc, char **argv) { return sy_cli(SYK_OUTLINE, "outline", "outline <arquivo> [pasta] [--json]", 0, 100000, argc, argv); }
static int coder_at_main(int argc, char **argv) { return sy_cli(SYK_AT, "at", "at <arquivo>:<linha> [pasta] [--json]", 0, 100, argc, argv); }
static int coder_deps_main(int argc, char **argv) { return sy_cli(SYK_DEPS, "deps", "deps <arquivo> [pasta] [--json]", 0, 200, argc, argv); }
static int coder_impact_main(int argc, char **argv) { return sy_cli(SYK_IMPACT, "impact", "impact <name> [pasta] [--depth N] [--nodes N] [--json]", 0, 1000, argc, argv); }

#ifndef _WIN32
/* ---- estado do indice para o modelo (fase 6, J.2): o escopo consultado esta completo? ----
 * Uma pasta so pode ser dita "pronta" se todos os arquivos dela que esperam nivel chegaram ao N2/N3. O mapa de estado vem dos documentos vigentes da visao
 * (statemap.c) e so e consultado quando o rastreador diz que o indice ainda nao esta completo (STS_CRAWL): num indice completo, ou feito pelo caminho sincrono
 * (sem STS_CRAWL), nao ha linha nenhuma e a saida das ferramentas e a de antes. */
typedef struct { int partial; uint32_t n_names, n_text, n_full; char label[400]; } IdxScope;       /* partial: 0 completo, 1 so nomes em parte do escopo, 2 texto sem simbolos */
static void idx_scope(const char *root, const char *pre, IdxScope *S) {
    memset(S, 0, sizeof *S);
    SyView v; char err[200] = ""; if (sy_view_open(root, &v, err, sizeof err) != 0) return;
    int complete = 1; const int known = sm_crawl_state(v.r, &complete, NULL, NULL, NULL);
    char pf[2100]; snprintf(pf, sizeof pf, "%s", pre ? pre : ""); { const char *q = pf; while (q[0] == '.' && q[1] == '/') q += 2; if (q != pf) memmove(pf, q, strlen(q) + 1); }
    size_t pl = strlen(pf); while (pl > 0 && pf[pl - 1] == '/') pf[--pl] = 0;
    if (known && !complete) {
        for (int i = 0; i < v.L.n; i++) {
            const StoreLiveDoc *e = &v.L.e[i]; if (e->dead || (e->d.flags & (SDF_SKIPMASK | SDF_GENERATED))) continue;
            if (pl && !(e->d.plen > pl && !memcmp(e->d.path, pf, pl) && e->d.path[pl] == '/')) continue;
            const uint32_t lv = sdf_lv(e->d.flags); if (lv == 1) S->n_names++; else if (lv == 2) S->n_text++; else S->n_full++;
        }
        S->partial = S->n_names ? 1 : S->n_text ? 2 : 0;
    }
    if (S->partial) {
        char ab[4400], sh[4400]; snprintf(ab, sizeof ab, "%s%s%s", root, pl ? "/" : "", pf); ign_disp(ab, sh, sizeof sh);
        size_t l = strlen(sh); while (l > 1 && sh[l - 1] == '/') sh[--l] = 0;
        if (!strcmp(sh, ".")) snprintf(S->label, sizeof S->label, "the project"); else snprintf(S->label, sizeof S->label, "%.300s/", sh);
    }
    sy_view_close(&v);
}
static int idx_scope_partial(const char *root, const char *pre) { IdxScope S; idx_scope(root, pre, &S); return S.partial; }
/* as linhas de estado: no maximo UMA (a mais importante): indice reconstruido por dano, inclusao restrita desta chamada, escopo ainda incompleto, arquivos
 * que mudaram desde a indexacao (fresh.c). Esvazia as notas pendentes de qualquer jeito. Devolve 1 se escreveu a linha (com a quebra de linha final) */
static int idx_status_line(const char *root, const char *pre, char *out, size_t cap) {
    char fr[200], sn[500]; const int fresh = fresh_note_take(fr, sizeof fr), scoped = ign_scope_note_take(sn, sizeof sn);
    if (cap) out[0] = 0;
    if (g_maint_rebuilt == 3) { g_maint_rebuilt = 0; snprintf(out, cap, "[index was damaged and has been rebuilt]\n"); return 1; }
    if (scoped) { snprintf(out, cap, "%s", sn); return 1; }
    IdxScope S; idx_scope(root, pre, &S);
    if (S.partial == 1) {
        const uint32_t all = S.n_names + S.n_text + S.n_full, text = S.n_text + S.n_full;
        if (!text) snprintf(out, cap, "[index: %s is still at level names only; text and symbols are not ready yet]\n", S.label);
        else snprintf(out, cap, "[index: %s is still being built: text ready for %u of %u files, symbols for %u; results are partial]\n", S.label, text, all, S.n_full);
        return 1; }
    if (S.partial == 2) { snprintf(out, cap, "[index: %s is still at level text; symbols not ready yet]\n", S.label); return 1; }
    if (fresh) { snprintf(out, cap, "%s", fr); return 1; }
    return 0;
}
/* as duas frases do NOT FOUND (J.3) para a consulta de simbolos em `root`: indice parcial (diz "so far" e a ressalva) e pastas ignoradas que poderiam ter o nome */
static void sy_nf_notes(const char *root, SyQ *q) {
    IdxScope S; idx_scope(root, "", &S);
    if (S.partial) { const uint32_t all = S.n_names + S.n_text + S.n_full;
        snprintf(q->partial, sizeof q->partial, "The index of %s is still being built (symbols ready for %u of %u files): this is not proof that it does not exist; try Grep, or ask again shortly.", S.label, S.n_full, all); }
    ign_notfound_note(root, "", q->ignote, sizeof q->ignote);
}

/* ---- Symbols action=tree (J.5): as pastas, a profundidade do indice de cada uma e o que as define; so N0, nenhum arquivo e lido ---- */
typedef struct { int kind; int di; const SmRec *rec; const IgnRec *ig; char *path; int depth; int64_t files; int parent; int sel; char rule[200]; int state; char main[4][100]; } TrNode;       /* kind: 0 pasta, 1 ignorada, 2 raiz aninhada, 3 copia separada */
static int sy_tree_lang(const char *p, size_t l) {
    char t[512]; if (l >= sizeof t) l = sizeof t - 1; memcpy(t, p, l); t[l] = 0;
    const char *base = strrchr(t, '/'); base = base ? base + 1 : t; const char *dot = strrchr(base, '.');
    return dot && dot > base ? coder_lang_of(t) : c2_lang_of(base, NULL);
}
static int sy_tree_cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
/* a pasta pedida (`file`) em relacao a raiz da visao: sem "./", sem barras sobrando; -1 se sai da raiz da visao */
static int sy_tree_base(const SyView *v, const char *folder, char *base, size_t cap) {
    char t[4400]; snprintf(t, sizeof t, "%s", folder ? folder : ""); const char *p = t;
    if (p[0] == '/') { const char *rt = sy_root(v); const size_t rl = strlen(rt); if (strncmp(p, rt, rl) != 0 || (p[rl] != '/' && p[rl])) return -1; p += rl; }
    size_t o = 0; base[0] = 0;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const size_t n = strcspn(p, "/");
        if (n == 1 && p[0] == '.') { p += n; continue; }
        if (n == 2 && p[0] == '.' && p[1] == '.') return -1;
        if (o + n + 2 >= cap) return -1;
        if (o) base[o++] = '/';
        memcpy(base + o, p, n); o += n; base[o] = 0; p += n;
    }
    return 0;
}
static int sy_tree_cmp(const void *a, const void *b) {
    const TrNode *x = *(TrNode *const *)a, *y = *(TrNode *const *)b;
    if (x->kind != y->kind) return x->kind - y->kind;
    if (x->kind == 0 && x->depth != y->depth) return x->depth - y->depth;
    if (x->files != y->files) return x->files < y->files ? 1 : -1;
    return strcmp(x->path, y->path);
}
/* ate 4 definicoes de `dir` (relativo a visao; "" = tudo) mais referenciadas de fora da pasta: topo do arquivo, nao privadas, sem declaracoes. So le as secoes do indice */
static int sy_tree_main(const SyView *v, const char *dir, size_t dl, char out[4][100], double t_end) {
    int nm = 0; int sc[4] = {0, 0, 0, 0}; int budget = 6000;
    for (int i = 0; i < v->L.n && budget > 0; i++) {
        const StoreLiveDoc *e = &v->L.e[i]; if (e->dead) continue;
        if (dl && !(e->d.plen > dl && !memcmp(e->d.path, dir, dl) && e->d.path[dl] == '/')) continue;
        const SySec *x = &v->s[e->seg].x; if (!x->has || e->idx >= x->docs.n) continue;
        const uint8_t *dr = store_table_at(&x->docs, e->idx); const uint32_t s0 = st_get_u32(dr), n = st_get_u32(dr + 4);
        for (uint32_t k = 0; k < n && budget > 0; k++, budget--) {
            const uint8_t *q = store_table_at(&x->syms, s0 + k); const uint32_t kind = st_get_u32(q + 4), pack = st_get_u32(q + 40);
            if (st_get_u32(q + 8) != SY_NONE || ((pack >> 16) & CSF_DECL)) continue;
            if (!(kind == CK_FUNC || kind == CK_CLASS || kind == CK_STRUCT || kind == CK_ENUM || kind == CK_IFACE || kind == CK_TYPEDEF || kind == CK_UNION)) continue;
            const uint32_t vis = pack & 0xFF; if (vis == CV_PRIVATE || vis == CV_FILE) continue;
            uint32_t nl; const char *nmv = store_strs_get(&x->names, st_get_u32(q), &nl); if (!nl || nl > 90) continue;
            int dup = 0; for (int j = 0; j < nm; j++) if (strlen(out[j]) == nl && !memcmp(out[j], nmv, nl)) dup = 1;
            if (dup) continue;
            int score = 0;      /* referencias vivas de fora da pasta (de qualquer lugar quando a pasta e a base), contadas nos segmentos do indice */
            for (int s = 0; s < v->ns; s++) { const SyVSeg *c = &v->s[s]; if (!c->x.has) continue;
                const int64_t id = store_strs_find(&c->x.names, nmv, nl); uint32_t lo, hi; if (id < 0 || !sy_pair_range(&c->x.byref, (uint32_t)id, &lo, &hi)) continue;
                for (uint32_t r = lo; r < hi && r < lo + 80; r++) {
                    const uint8_t *rq = store_table_at(&c->x.refs, st_get_u32(store_table_at(&c->x.byref, r) + 4)); const uint32_t rd = st_get_u32(rq + 24);
                    if (!c->live[rd]) continue;
                    if (dl) { StoreDoc d; if (!store_docs_get(&c->sd, rd, &d)) continue; const char *pp = d.path + v->strip; const uint32_t pl = d.plen - v->strip;
                        if (pl > dl && !memcmp(pp, dir, dl) && pp[dl] == '/') continue; }
                    score++; } }
            if (score <= 0) continue;
            if (nm == 4 && score <= sc[3]) continue;
            int at = nm < 4 ? nm : 3;
            while (at > 0 && sc[at - 1] < score) { sc[at] = sc[at - 1]; memcpy(out[at], out[at - 1], 100); at--; }
            sc[at] = score; memcpy(out[at], nmv, nl); out[at][nl] = 0; if (nm < 4) nm++;
        }
        if (now_sec() > t_end) break;
    }
    return nm;
}
/* "C 210, shell 40, markdown 23, other 12": as tres linguagens com mais arquivos e o resto */
static void sy_tree_langs(const SmDir *d, char *o, size_t cap) {
    uint32_t c[SM_LANG]; memcpy(c, d->lc, sizeof c); size_t l = 0; o[0] = 0; uint32_t rest = d->files;
    for (int t = 0; t < 3; t++) {
        int b = -1; for (int k = 1; k < SM_LANG - 1; k++) if (c[k] && k < CL_N && (b < 0 || c[k] > c[b])) b = k;
        if (b < 0) break;
        rest -= c[b]; l += (size_t)snprintf(o + l, cap - l, "%s%s %u", t ? ", " : "", CP_LANGN[b], c[b]); c[b] = 0;
    }
    if (rest) snprintf(o + l, cap - l, "%sother %u", l ? ", " : "", rest);
}
/* a profundidade do indice de uma pasta em palavras: o menor nivel que todos os arquivos dela que esperam nivel ja alcancaram */
static const char *sy_tree_level(const SmDir *d, int any, int anykeys, const char **note) {
    *note = "";
    if (d->n_names) return "names only";
    if (d->n_text) { *note = "symbols not ready"; return "text"; }
    if (!d->n_full) { *note = d->files ? "no text files" : ""; return "names only"; }
    return !any ? "text" : anykeys ? "symbols and links" : "symbols";
}
/* o texto (ou o JSON) da arvore de `folder` (relativa a `proj`); 0 = respondeu, 1 = nao ha o que mostrar (a mensagem esta no buffer), -1 = uso errado */
static int sy_tree(const char *proj, const char *folder, int depth, int limit, int json, CtSb *sb) {
    if (depth < 1) depth = 2;
    if (depth > 4) depth = 4;
    if (limit < 1) limit = 40;
    if (limit > 100) limit = 100;
    char pd[4400]; ign_disp(proj, pd, sizeof pd); const char *pdn = strcmp(pd, ".") ? pd : "this folder";       /* como o modelo chama a pasta nas frases */
    IndexLoc loc; index_resolve(proj, 0, &loc);
    { char man[4500]; struct stat sm; snprintf(man, sizeof man, "%s/MANIFEST", loc.store);
      if (stat(man, &sm) != 0) { sy_sbf(sb, "No index for %s yet. `tree` only reads an existing index and never builds one: run `katana coder index %s` (or call another Symbols action, which builds it on first use), then ask again.\n", pdn, pd); return 1; } }
    { int skip = 0; SyView v0; char e0[200] = "";      /* com o rastreador no meio do trabalho (STS_CRAWL incompleto) nao se espera nem se completa nada aqui: a arvore (N0) ja esta */
      if (sy_view_open(proj, &v0, e0, sizeof e0) == 0) { int c = 1; skip = sm_crawl_state(v0.r, &c, NULL, NULL, NULL) && !c; sy_view_close(&v0); }
      if (!skip) { IndexRes ir; index_ensure(proj, 0, "", 0, &ir); } }           /* senao mantem o indice em dia como as outras acoes; so le arquivo o que mudou */
    SyView v; char err[300] = "";
    if (sy_view_open(proj, &v, err, sizeof err) != 0) { sy_sbf(sb, "No index for %s: %s\n", pdn, err); return 1; }
    char base[4400]; if (sy_tree_base(&v, folder, base, sizeof base) != 0) { sy_sbf(sb, "error: `file` must be a folder inside the project (%s)\n", pdn); sy_view_close(&v); return -1; }
    const size_t bl = strlen(base);
    char fb[4400]; if (loc.prefix[0]) snprintf(fb, sizeof fb, "%s%s%s", loc.prefix, bl ? "/" : "", base); else snprintf(fb, sizeof fb, "%s", base);      /* a base no espaco da raiz do indice */
    const size_t fbl = strlen(fb);
    StateMap M; const uint32_t total = statemap_build(&v.L, base, depth, sy_tree_lang, &M);
    SmRecs R; sm_recs_load(v.r, &R);
    IgnReg G; ign_reg_get(&loc, &G);
    if (!total) {
        char sh[4400]; snprintf(sh, sizeof sh, "%s%s%s", bl && !strcmp(pd, ".") ? "" : pd, bl && strcmp(pd, ".") ? "/" : "", base);
        int ig = -1;
        if (G.has && fbl) { char sub[4400]; snprintf(sub, sizeof sub, "%s", fb);
            for (;;) { ig = ign_reg_find_dir(&G, sub); if (ig >= 0) break; char *sl = strrchr(sub, '/'); if (!sl) break; *sl = 0; } }
        if (ig >= 0) sy_sbf(sb, "%s/ is not in the index: it is an ignored folder (%s). To look inside it, call a Symbols or Find action with `path` set to it (that indexes just that folder), or run `katana coder include`.\n", sh, G.r[ig].rule);
        else if (bl) sy_sbf(sb, "%s/ has no files in the index (no such folder, or nothing indexable in it). Call tree on its parent to see the folders.\n", sh);
        else sy_sbf(sb, "The index of %s has no files yet.\n", pdn);
        statemap_free(&M); sm_recs_free(&R); ign_reg_free(&G); sy_view_close(&v); return 1;
    }
    /* os nos: pastas do mapa (profundidade >= 1), pastas descobertas e ainda nao listadas, e as ignoradas */
    TrNode *N = (TrNode *)calloc((size_t)M.n + (size_t)R.n + (size_t)G.n + 4, sizeof *N); int nn = 0;
    char **own = (char **)calloc((size_t)R.n + 4, sizeof(char *)); int nown = 0;           /* caminhos montados aqui (para liberar) */
    for (int i = 1; i < M.n; i++) { TrNode *t = &N[nn++]; t->kind = 0; t->di = i; t->path = M.d[i].path; t->depth = M.d[i].depth; t->files = M.d[i].files; t->parent = -1;
        char key[4400]; snprintf(key, sizeof key, "%s%s%s", fb, fbl ? "/" : "", M.d[i].path); const int ri = sm_recs_find(&R, key, strlen(key)); t->rec = ri >= 0 ? &R.r[ri] : NULL; }
    for (int i = 0; i < R.n; i++) {
        const SmRec *x = &R.r[i]; if (!x->plen || (fbl && !(x->plen > fbl && !memcmp(x->path, fb, fbl) && x->path[fbl] == '/'))) continue;
        const char *rel = x->path + (fbl ? fbl + 1 : 0); const size_t rl = x->plen - (fbl ? fbl + 1 : 0); int dd = 1; for (size_t k = 0; k < rl; k++) if (rel[k] == '/') dd++;
        if (dd > depth) continue;
        if (x->state == SM_ST_DISC) {          /* descoberta e ainda nao listada (o rastreador esta no meio): aparece com 0 arquivos */
            if (sm_dir_find(&M, rel, rl) >= 0) continue;
            char *rc = (char *)malloc(rl + 1); memcpy(rc, rel, rl); rc[rl] = 0; own[nown++] = rc;
            TrNode *t = &N[nn++]; t->kind = 0; t->di = -1; t->rec = x; t->path = rc; t->depth = dd; t->files = 0; t->parent = -1; continue; }
        if (x->state != SM_ST_IGNORED && x->state != SM_ST_NESTED && x->state != SM_ST_CHECKOUT) continue;
        if (G.has && ign_reg_find_dir(&G, x->path) >= 0) continue;          /* o registro de ignorados (com contagem) cobre esta */
        if (x->state == SM_ST_IGNORED && x->reason == 14) continue;         /* pasta de segredo: nao se nomeia */
        char *rc = (char *)malloc(rl + 1); memcpy(rc, rel, rl); rc[rl] = 0; own[nown++] = rc;
        TrNode *t = &N[nn++]; t->kind = x->state == SM_ST_NESTED ? 2 : x->state == SM_ST_CHECKOUT ? 3 : 1; t->path = rc; t->depth = dd; t->files = -1; t->parent = -1; t->state = 0;
        snprintf(t->rule, sizeof t->rule, "%.*s", (int)(x->rlen < 150 ? x->rlen : 150), x->rule);
    }
    int64_t ign_files = 0; int ign_dirs = 0, ign_uncounted = 0;
    for (int i = 0; i < G.n; i++) {
        const IgnRec *x = &G.r[i]; if (!x->isdir || x->reason == IGR_SECRET) continue;
        if (fbl && !(strlen(x->rel) > fbl && !memcmp(x->rel, fb, fbl) && x->rel[fbl] == '/')) continue;
        const char *rel = x->rel + (fbl ? fbl + 1 : 0); int dd = 1; for (const char *c = rel; *c; c++) if (*c == '/') dd++;
        ign_dirs++; if (x->state) ign_files += x->files; else ign_uncounted++;
        if (dd > depth) continue;
        TrNode *t = &N[nn++]; t->kind = x->reason == IGR_NESTED ? 2 : x->reason == IGR_CHECKOUT ? 3 : 1; t->ig = x; t->path = (char *)rel; t->depth = dd; t->files = x->state ? x->files : -1; t->state = x->state; t->parent = -1;
        snprintf(t->rule, sizeof t->rule, "%.190s", x->rule);
    }
    /* escolha em largura (profundidade, depois as maiores): `limit` pastas; as ignoradas sempre entram (ate 15) e nao gastam o limite */
    TrNode **ord = (TrNode **)malloc(sizeof *ord * (size_t)(nn + 1)); for (int i = 0; i < nn; i++) ord[i] = &N[i];
    qsort(ord, (size_t)nn, sizeof *ord, sy_tree_cmp);
    int nreal = 0, nrealtotal = 0, nigsel = 0; for (int i = 0; i < nn; i++) if (N[i].kind == 0) nrealtotal++;
    for (int i = 0; i < nn; i++) { TrNode *t = ord[i];
        if (t->kind == 0) { if (nreal < limit) { t->sel = 1; nreal++; } }
        else if (nigsel < 15) { t->sel = 1; nigsel++; } }
    /* pais: o caminho sem o ultimo componente, entre as pastas escolhidas; sem achar, a base */
    for (int i = 0; i < nn; i++) { TrNode *t = &N[i]; if (!t->sel) continue; const char *sl = strrchr(t->path, '/'); if (!sl) continue;
        const size_t pl = (size_t)(sl - t->path);
        for (int j = 0; j < nn; j++) if (N[j].sel && N[j].kind == 0 && strlen(N[j].path) == pl && !memcmp(N[j].path, t->path, pl)) { t->parent = j; break; } }
    /* ordem de saida em arvore: cada pasta seguida das suas, as maiores primeiro, as ignoradas depois (ord ja esta por tipo, profundidade e tamanho) */
    int *emit = (int *)malloc(sizeof(int) * (size_t)(nn + 1)); int ne = 0;
    { int *stack = (int *)malloc(sizeof(int) * (size_t)(nn + 2)); int sp = 0; int *kids = (int *)malloc(sizeof(int) * (size_t)(nn + 1)); int kc = 0;
      for (int i = 0; i < nn; i++) if (ord[i]->sel && ord[i]->parent < 0) kids[kc++] = (int)(ord[i] - N);
      for (int i = kc - 1; i >= 0; i--) stack[sp++] = kids[i];
      while (sp > 0) { const int cur = stack[--sp]; emit[ne++] = cur; kc = 0;
          for (int i = 0; i < nn; i++) if (ord[i]->sel && ord[i]->parent == cur) kids[kc++] = (int)(ord[i] - N);
          for (int i = kc - 1; i >= 0; i--) stack[sp++] = kids[i]; }
      free(kids); free(stack); }
    const double t_end = now_sec() + 0.5; char nb[4400]; { const char *bn = base; if (!bl) { bn = strrchr(proj, '/'); bn = bn && bn[1] ? bn + 1 : proj; } snprintf(nb, sizeof nb, "%s", bn); }
    char hn[40]; mt_commas(total, hn, sizeof hn);
    char langs[200]; sy_tree_langs(&M.d[0], langs, sizeof langs); const char *hnote; const char *hlev = sy_tree_level(&M.d[0], v.any, v.anykeys, &hnote);
    if (json) {
        sy_sbf(sb, "{\"folder\":"); sy_sb_jstr(sb, bl ? base : "."); sy_sbf(sb, ",\"files\":%u,\"index\":", total); sy_sb_jstr(sb, hlev); sy_sbf(sb, ",\"folders\":[");
        for (int k = 0; k < ne; k++) { const TrNode *t = &N[emit[k]]; if (k) sy_sbf(sb, ",");
            sy_sbf(sb, "{\"path\":"); { char pp[4400]; snprintf(pp, sizeof pp, "%s%s%s", base, bl ? "/" : "", t->path); sy_sb_jstr(sb, pp); }
            if (t->kind == 0) { const char *nt; const SmDir *d = t->di >= 0 ? &M.d[t->di] : NULL; const char *lv = d ? sy_tree_level(d, v.any, v.anykeys, &nt) : "names only";
                sy_sbf(sb, ",\"kind\":\"folder\",\"files\":%lld,\"index\":", (long long)t->files); sy_sb_jstr(sb, lv);
                if (d) { char lg[200]; sy_tree_langs(d, lg, sizeof lg); sy_sbf(sb, ",\"languages\":"); sy_sb_jstr(sb, lg); }
                if (t->rec && t->rec->dlen) { char dd[200]; snprintf(dd, sizeof dd, "%.*s", (int)(t->rec->dlen < 190 ? t->rec->dlen : 190), t->rec->doc); sy_sbf(sb, ",\"doc\":"); sy_sb_jstr(sb, dd); } }
            else { sy_sbf(sb, ",\"kind\":\"%s\",\"rule\":", t->kind == 1 ? "ignored" : t->kind == 2 ? "own-index" : "separate-checkout"); sy_sb_jstr(sb, t->rule); sy_sbf(sb, ",\"files\":%lld,\"counted\":%s", (long long)t->files, t->state ? "true" : "false"); }
            sy_sbf(sb, "}"); }
        sy_sbf(sb, "],\"more_folders\":%d,\"ignored\":{\"folders\":%d,\"files\":%lld}}\n", nrealtotal > nreal ? nrealtotal - nreal : 0, ign_dirs, (long long)ign_files);
    } else {
        char lgs[240] = ""; if (langs[0]) snprintf(lgs, sizeof lgs, " (%s)", langs);
        sy_sbf(sb, "%s/  %s %s%s  [indexed: %s]%s%s\n", nb, hn, total == 1 ? "file" : "files", lgs, hlev, *hnote ? "; " : "", hnote);
        { const int ri = sm_recs_find(&R, fb, fbl); if (ri >= 0 && R.r[ri].dlen) sy_sbf(sb, "  doc: %.*s\n", (int)(R.r[ri].dlen < 150 ? R.r[ri].dlen : 150), R.r[ri].doc); }
        if (M.nhere) { int cnt = 0; qsort(M.here, (size_t)M.nhere, sizeof(char *), sy_tree_cmp_str); sy_sbf(sb, "  files here: ");
            for (int i = 0; i < M.nhere && cnt < 6; i++, cnt++) sy_sbf(sb, "%s%.60s", cnt ? ", " : "", M.here[i]);
            if (M.nhere > cnt) sy_sbf(sb, " (+%d)", M.nhere - cnt);
            sy_sbf(sb, "\n"); }
        int cutf = 0;                      /* pastas que o teto de tamanho da saida deixou de fora (a saida do Symbols e cortada em ~6500 caracteres; as linhas finais precisam caber) */
        for (int k = 0; k < ne; k++) {
            TrNode *t = &N[emit[k]]; char pp[4400]; snprintf(pp, sizeof pp, "%s%s%s", base, bl ? "/" : "", t->path); const int ind = 2 * t->depth;
            if (t->kind == 0 && sb->n > 4800) { cutf++; continue; }
            if (t->kind != 0 && sb->n > 5900) continue;
            if (t->kind == 0) {
                const SmDir *d = t->di >= 0 ? &M.d[t->di] : NULL; const char *nt = "", *lv = d ? sy_tree_level(d, v.any, v.anykeys, &nt) : "names only"; char fn[40], lg[200] = "";
                mt_commas((long long)t->files, fn, sizeof fn); if (d) sy_tree_langs(d, lg, sizeof lg);
                sy_sbf(sb, "%*s%s/  %s %s", ind, "", pp, fn, t->files == 1 ? "file" : "files"); if (lg[0]) sy_sbf(sb, " (%s)", lg); sy_sbf(sb, "  [%s]", lv);
                if (!d) sy_sbf(sb, "  not listed yet"); else if (*nt) sy_sbf(sb, "  %s", nt);
                if (t->rec && t->rec->state == SM_ST_TOOMANY) sy_sbf(sb, "  limit: more than 20000 entries");
                if (t->rec && t->rec->state == SM_ST_TOODEEP) sy_sbf(sb, "  limit: deeper than the depth limit");
                if (t->rec && t->rec->dlen) sy_sbf(sb, "  doc: %.*s", (int)(t->rec->dlen < 110 ? t->rec->dlen : 110), t->rec->doc);
                sy_sbf(sb, "\n");
                if (d && !*nt && !d->n_names && !d->n_text && d->n_full && v.any && now_sec() < t_end) {
                    const int nm = sy_tree_main(&v, pp, strlen(pp), t->main, t_end);
                    if (nm) { sy_sbf(sb, "%*s  main: ", ind, ""); for (int j = 0; j < nm; j++) sy_sbf(sb, "%s%s", j ? ", " : "", t->main[j]); sy_sbf(sb, "\n"); } }
            } else {
                char ab[4400], sh[4400]; snprintf(ab, sizeof ab, "%s/%s%s%s", strcmp(loc.root, "/") ? loc.root : "", loc.prefix, loc.prefix[0] ? "/" : "", pp); ign_disp(ab, sh, sizeof sh);
                if (t->kind == 2) sy_sbf(sb, "%*s%s/  (has its own index; use path=%s)\n", ind, "", pp, sh);
                else if (t->kind == 3) sy_sbf(sb, "%*s%s/  (separate git checkout; use path=%s)\n", ind, "", pp, sh);
                else { char fn[60]; if (t->files < 0) snprintf(fn, sizeof fn, "not counted yet"); else { char c[40]; mt_commas((long long)t->files, c, sizeof c); snprintf(fn, sizeof fn, "%s%s %s", t->state == 2 ? "at least " : "", c, t->files == 1 ? "file" : "files"); }
                    sy_sbf(sb, "%*s%s/  (ignored: %s, %s)\n", ind, "", pp, t->rule, fn); }
            }
        }
        if (nrealtotal - nreal + cutf > 0) sy_sbf(sb, "(%d more folders not shown; raise `limit` or call tree on a subfolder)\n", nrealtotal - nreal + cutf);
        if (ign_dirs > nigsel) sy_sbf(sb, "(%d more ignored folder%s not listed here; `katana coder ignored` lists them all)\n", ign_dirs - nigsel, ign_dirs - nigsel == 1 ? "" : "s");
        if (ign_dirs) { char fn[40]; mt_commas((long long)ign_files, fn, sizeof fn); sy_sbf(sb, "ignored: %d folder%s, %s%s files not in the index%s. To search inside one, set `path` to it.\n", ign_dirs, ign_dirs == 1 ? "" : "s", ign_uncounted ? "at least " : "", fn, ign_uncounted ? " (some not counted yet)" : ""); }
    }
    for (int i = 0; i < nown; i++) free(own[i]);
    free(own); free(emit); free(ord); free(N);
    statemap_free(&M); sm_recs_free(&R); ign_reg_free(&G); sy_view_close(&v);
    return 0;
}
/* katana coder tree [pasta] [--depth N] [--limit N] [--json] */
static int coder_tree_main(int argc, char **argv) {
    const char *dir = "."; int depth = 0, limit = 0, json = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1; else if (!strcmp(argv[i], "--depth") && i + 1 < argc) depth = atoi(argv[++i]); else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (argv[i][0] != '-') dir = argv[i];
        else { fprintf(stderr, "uso: katana coder tree [pasta] [--depth N] [--limit N] [--json]\n"); return 2; }
    }
    struct stat st; if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) { fprintf(stderr, "tree: %s is not a folder\n", dir); return 2; }
    char rp[4200]; if (!realpath(dir, rp)) { fprintf(stderr, "tree: cannot resolve %s\n", dir); return 2; }
    ign_scope_enter(rp, 0);
    CtSb sb = {0}; const int rc = sy_tree(rp, "", depth, limit, json, &sb); sy_put(&sb); ign_scope_leave();
    return rc < 0 ? 2 : rc;
}
#else
static int idx_status_line(const char *root, const char *pre, char *out, size_t cap) { (void)root; (void)pre; if (cap) out[0] = 0; return 0; }
static int idx_scope_partial(const char *root, const char *pre) { (void)root; (void)pre; return 0; }
static void sy_nf_notes(const char *root, SyQ *q) { (void)root; (void)q; }
static int sy_tree(const char *proj, const char *folder, int depth, int limit, int json, CtSb *sb) { (void)proj; (void)folder; (void)depth; (void)limit; (void)json; sy_sbf(sb, "tree: not available on this platform\n"); return 1; }
static int coder_tree_main(int argc, char **argv) { (void)argc; (void)argv; fprintf(stderr, "tree: not available on this platform\n"); return 2; }
#endif
