/* ---------- coder/symindex.c: indice de simbolos e referencias sobre o armazenamento (secoes da faixa 0x200) ----------
 * Escrito por segmento no gancho do `index` (storedocs.c), junto com a busca por arquivo: so arquivos novos/alterados sao analisados (coder_parse).
 * Tudo little-endian, campos lidos e escritos um a um (nunca struct), documento = indice LOCAL ao segmento (posicao em STS_DOCS). Sete secoes,
 * todas presentes ou todas ausentes no segmento:
 *   SY_T_NAMES (0x200) tabela de strings ordenada por bytes: nomes de simbolos e de referencias do segmento; id de nome = posicao (busca binaria)
 *   SY_T_DOCS  (0x201) tabela largura 24, um registro por documento do segmento: u32 sym0, nsym, ref0, nref, lang, reservado
 *                      (zeros = documento sem simbolos nem referencias; as linhas de SYMS/REFS de um documento sao contiguas)
 *   SY_T_SYMS  (0x202) tabela largura 48 + pool de assinaturas: u32 nome, tipo, pai (relativo ao documento, ou 0xFFFFFFFF), line0, line1,
 *                      byte0, byte1, body0, sig_off, sig_len, vis|conf<<8|flags<<16, documento. Em ordem de arquivo dentro do documento.
 *   SY_T_REFS  (0x203) tabela largura 28: u32 nome, tipo, origem (simbolo relativo ao documento, ou 0xFFFFFFFF), line, col, flags, documento
 *   SY_T_BYDEF (0x204) pares (id do nome, linha de SYMS) ordenados: "todas as definicoes deste nome" e uma busca binaria
 *   SY_T_BYREF (0x205) pares (id do nome, linha de REFS) ordenados: "todas as referencias a este nome"
 *   SY_T_IVAL  (0x206) indice por intervalo de linhas, alinhado a SYMS: u32 maximo de line1 desde o inicio do documento ate o simbolo. Como os
 *                      simbolos de um documento vem ordenados por line0, "qual simbolo contem a linha X" e uma busca binaria no ultimo
 *                      simbolo com line0 <= X e uma volta para tras enquanto o maximo acumulado alcancar X.
 * Fusores: reconstroem tudo (um por tipo de secao, o primeiro reconstroi e entrega os demais) remapeando documentos pelo docmap; documentos
 * mortos ou substituidos somem; os nomes sao unidos e reordenados.
 * Nada aqui resolve nomes: as relacoes entre simbolos sao por NOME IGUAL; ver symquery.c para a cascata de consulta e o nivel de confianca. */
#define SY_T_NAMES (STS_SYM_BASE + 0)
#define SY_T_DOCS (STS_SYM_BASE + 1)
#define SY_T_SYMS (STS_SYM_BASE + 2)
#define SY_T_REFS (STS_SYM_BASE + 3)
#define SY_T_BYDEF (STS_SYM_BASE + 4)
#define SY_T_BYREF (STS_SYM_BASE + 5)
#define SY_T_IVAL (STS_SYM_BASE + 6)
#define SY_NONE 0xFFFFFFFFu
#define SY_DOCW 24
#define SY_SYMW 48
#define SY_REFW 28

/* ---- entrada do construtor (nomes apontam para quem os possui; nada e copiado) ---- */
typedef struct { const char *name, *sig; uint32_t nl, sl, kind, line0, line1, byte0, byte1, body0, pack, parent; } SySIn;
typedef struct { const char *name; uint32_t nl, kind, line, col, flags, from; } SyRIn;
typedef struct { SySIn *s; uint32_t ns; SyRIn *r; uint32_t nr; uint32_t lang; CoderFile cf; } SyDIn;
typedef struct { StoreBuf names, docs, syms, refs, bydef, byref, ival; uint32_t nnames, nsyms, nrefs; } SyOut;
typedef struct { const char *p; uint32_t l; } SyNm;
static int sy_nm_cmp(const void *a, const void *b) { const SyNm *x = (const SyNm *)a, *y = (const SyNm *)b; return st_cmp_bytes(x->p, x->l, y->p, y->l); }
static int sy_cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static uint32_t sy_nm_id(const SyNm *u, uint32_t n, const char *p, uint32_t l) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) { uint32_t m = lo + (hi - lo) / 2; int c = st_cmp_bytes(u[m].p, u[m].l, p, l); if (c == 0) return m; if (c < 0) lo = m + 1; else hi = m; }
    return 0;
}
/* monta as sete secoes de nd documentos (alguns podem estar vazios) */
static void sy_build(const SyDIn *d, uint32_t nd, SyOut *o) {
    memset(o, 0, sizeof *o);
    uint64_t tn = 0; for (uint32_t i = 0; i < nd; i++) tn += (uint64_t)d[i].ns + d[i].nr;
    SyNm *all = (SyNm *)malloc(sizeof *all * (size_t)(tn + 1)); uint64_t k = 0;
    for (uint32_t i = 0; i < nd; i++) { for (uint32_t s = 0; s < d[i].ns; s++) { all[k].p = d[i].s[s].name; all[k++].l = d[i].s[s].nl; } for (uint32_t r = 0; r < d[i].nr; r++) { all[k].p = d[i].r[r].name; all[k++].l = d[i].r[r].nl; } }
    qsort(all, (size_t)k, sizeof *all, sy_nm_cmp);
    uint64_t u = 0; for (uint64_t i = 0; i < k; i++) if (!u || sy_nm_cmp(&all[u - 1], &all[i]) != 0) all[u++] = all[i];
    const char **ps = (const char **)malloc(sizeof *ps * (size_t)(u + 1)); uint32_t *ls = (uint32_t *)malloc(sizeof *ls * (size_t)(u + 1));
    for (uint64_t i = 0; i < u; i++) { ps[i] = all[i].p; ls[i] = all[i].l; }
    store_strs_build(&o->names, ps, ls, (uint32_t)u); o->nnames = (uint32_t)u; free(ps); free(ls);
    uint32_t ns = 0, nr = 0; for (uint32_t i = 0; i < nd; i++) { ns += d[i].ns; nr += d[i].nr; }
    o->nsyms = ns; o->nrefs = nr;
    store_table_begin(&o->docs, nd, SY_DOCW); store_table_begin(&o->syms, ns, SY_SYMW); store_table_begin(&o->refs, nr, SY_REFW); store_table_begin(&o->ival, ns, 4);
    uint64_t *pd = (uint64_t *)malloc(sizeof *pd * (size_t)(ns + 1)), *pr = (uint64_t *)malloc(sizeof *pr * (size_t)(nr + 1));
    StoreBuf sig = {0}; uint32_t sr = 0, rr = 0;
    for (uint32_t i = 0; i < nd; i++) {
        const SyDIn *x = &d[i];
        sb_u32(&o->docs, x->ns || x->nr ? sr : 0); sb_u32(&o->docs, x->ns); sb_u32(&o->docs, x->ns || x->nr ? rr : 0); sb_u32(&o->docs, x->nr); sb_u32(&o->docs, x->lang); sb_u32(&o->docs, 0);
        uint32_t mx = 0;
        for (uint32_t s = 0; s < x->ns; s++) {
            const SySIn *y = &x->s[s]; uint32_t id = sy_nm_id(all, (uint32_t)u, y->name, y->nl);
            sb_u32(&o->syms, id); sb_u32(&o->syms, y->kind); sb_u32(&o->syms, y->parent); sb_u32(&o->syms, y->line0); sb_u32(&o->syms, y->line1);
            sb_u32(&o->syms, y->byte0); sb_u32(&o->syms, y->byte1); sb_u32(&o->syms, y->body0); sb_u32(&o->syms, (uint32_t)sig.n); sb_u32(&o->syms, y->sl);
            sb_u32(&o->syms, y->pack); sb_u32(&o->syms, i);
            sb_bytes(&sig, y->sig, y->sl);
            if (y->line1 > mx) mx = y->line1;
            sb_u32(&o->ival, mx);
            pd[sr] = (uint64_t)id << 32 | sr; sr++;
        }
        for (uint32_t r = 0; r < x->nr; r++) {
            const SyRIn *y = &x->r[r]; uint32_t id = sy_nm_id(all, (uint32_t)u, y->name, y->nl);
            sb_u32(&o->refs, id); sb_u32(&o->refs, y->kind); sb_u32(&o->refs, y->from); sb_u32(&o->refs, y->line); sb_u32(&o->refs, y->col); sb_u32(&o->refs, y->flags); sb_u32(&o->refs, i);
            pr[rr] = (uint64_t)id << 32 | rr; rr++;
        }
    }
    sb_bytes(&o->syms, sig.p, sig.n); sb_free(&sig);
    qsort(pd, ns, sizeof *pd, sy_cmp_u64); qsort(pr, nr, sizeof *pr, sy_cmp_u64);
    store_table_begin(&o->bydef, ns, 8); for (uint32_t i = 0; i < ns; i++) { sb_u32(&o->bydef, (uint32_t)(pd[i] >> 32)); sb_u32(&o->bydef, (uint32_t)pd[i]); }
    store_table_begin(&o->byref, nr, 8); for (uint32_t i = 0; i < nr; i++) { sb_u32(&o->byref, (uint32_t)(pr[i] >> 32)); sb_u32(&o->byref, (uint32_t)pr[i]); }
    free(pd); free(pr); free(all);
}
static void sy_out_free(SyOut *o) { sb_free(&o->names); sb_free(&o->docs); sb_free(&o->syms); sb_free(&o->refs); sb_free(&o->bydef); sb_free(&o->byref); sb_free(&o->ival); }

/* ---- gancho do `index`: encadeia com o da busca (mesmo ctx, que continua sendo o da busca) ---- */
static struct {
    void (*prev_doc)(void *, uint32_t, const StoreDoc *, const uint8_t *, size_t);
    void (*prev_commit)(void *, StoreWriter *, uint32_t);
    SyDIn *d; uint32_t nd, cap; char root[4096]; uint32_t nfiles, nsyms, nrefs; double secs; uint64_t bytes[7];
} g_sy;
static void sy_doc_hook(void *ctx, uint32_t idx, const StoreDoc *d, const uint8_t *data, size_t n) {
    if (g_sy.prev_doc) g_sy.prev_doc(ctx, idx, d, data, n);
    if (idx >= g_sy.cap) { uint32_t nc = idx + 256; g_sy.d = (SyDIn *)realloc(g_sy.d, sizeof(SyDIn) * nc); memset(g_sy.d + g_sy.cap, 0, sizeof(SyDIn) * (nc - g_sy.cap)); g_sy.cap = nc; }
    if (idx >= g_sy.nd) g_sy.nd = idx + 1;
    if (!data || !n) return;
    char rel[4100]; uint32_t pl = d->plen < 4000 ? d->plen : 4000; memcpy(rel, d->path, pl); rel[pl] = 0;
    char full[8300]; snprintf(full, sizeof full, "%s/%s", g_sy.root, rel);
    double t0 = now_sec();
    char *txt = (char *)malloc(n + 1); memcpy(txt, data, n); txt[n] = 0;
    SyDIn *x = &g_sy.d[idx];
    coder_parse(full, txt, n, &x->cf); free(txt);
    CoderFile *f = &x->cf; x->lang = (uint32_t)f->lang;
    if (f->nsym || f->nref) {
        x->s = (SySIn *)calloc((size_t)f->nsym + 1, sizeof(SySIn)); x->r = (SyRIn *)calloc((size_t)f->nref + 1, sizeof(SyRIn)); x->ns = (uint32_t)f->nsym; x->nr = (uint32_t)f->nref;
        for (int i = 0; i < f->nsym; i++) { const CoderSym *y = &f->sym[i]; SySIn *z = &x->s[i];
            z->name = y->name; z->nl = (uint32_t)strlen(y->name); z->sig = y->sig ? y->sig : ""; z->sl = (uint32_t)strlen(z->sig); z->kind = (uint32_t)y->kind;
            z->line0 = (uint32_t)y->line0; z->line1 = (uint32_t)y->line1; z->byte0 = (uint32_t)y->byte0; z->byte1 = (uint32_t)y->byte1; z->body0 = (uint32_t)y->body0;
            z->pack = (uint32_t)(y->vis & 0xFF) | (uint32_t)(y->conf & 0xFF) << 8 | (uint32_t)(y->flags & 0xFFFF) << 16; z->parent = y->parent >= 0 ? (uint32_t)y->parent : SY_NONE; }
        for (int i = 0; i < f->nref; i++) { const CoderRef *y = &f->ref[i]; SyRIn *z = &x->r[i];
            z->name = y->name; z->nl = (uint32_t)strlen(y->name); z->kind = (uint32_t)y->kind; z->line = (uint32_t)y->line; z->col = (uint32_t)y->col; z->flags = (uint32_t)y->flags; z->from = y->from >= 0 ? (uint32_t)y->from : SY_NONE; }
        g_sy.nfiles++; g_sy.nsyms += x->ns; g_sy.nrefs += x->nr;
    }
    g_sy.secs += now_sec() - t0;
}
static void sy_commit_hook(void *ctx, StoreWriter *w, uint32_t ndocs) {
    if (g_sy.prev_commit) g_sy.prev_commit(ctx, w, ndocs);
    if (!g_sy.nfiles) return;
    SyDIn *all = (SyDIn *)calloc(ndocs + 1, sizeof *all);
    for (uint32_t i = 0; i < ndocs && i < g_sy.nd; i++) all[i] = g_sy.d[i];
    SyOut o; sy_build(all, ndocs, &o); free(all);
    g_sy.bytes[0] = o.names.n; g_sy.bytes[1] = o.docs.n; g_sy.bytes[2] = o.syms.n; g_sy.bytes[3] = o.refs.n; g_sy.bytes[4] = o.bydef.n; g_sy.bytes[5] = o.byref.n; g_sy.bytes[6] = o.ival.n;
    store_writer_add_section_buf(w, SY_T_NAMES, 1, o.nnames, &o.names); store_writer_add_section_buf(w, SY_T_DOCS, 1, ndocs, &o.docs);
    store_writer_add_section_buf(w, SY_T_SYMS, 1, o.nsyms, &o.syms); store_writer_add_section_buf(w, SY_T_REFS, 1, o.nrefs, &o.refs);
    store_writer_add_section_buf(w, SY_T_BYDEF, 1, o.nsyms, &o.bydef); store_writer_add_section_buf(w, SY_T_BYREF, 1, o.nrefs, &o.byref);
    store_writer_add_section_buf(w, SY_T_IVAL, 1, o.nsyms, &o.ival);
}
static void coder_sym_hook(StoreScanOpts *o, const char *proj) {
    memset(&g_sy, 0, sizeof g_sy);
    if (!realpath(proj, g_sy.root)) snprintf(g_sy.root, sizeof g_sy.root, "%s", proj);
    g_sy.prev_doc = o->doc_fn; g_sy.prev_commit = o->commit_fn; o->doc_fn = sy_doc_hook; o->commit_fn = sy_commit_hook;
}
static void coder_sym_hook_done(char *out, size_t cap) {
    out[0] = 0;
    if (g_sy.nfiles) snprintf(out, cap, "%u arquivos com simbolos, %u simbolos, %u referencias, analise %.3f s; secoes novas: nomes %llu B, documentos %llu B, simbolos %llu B, referencias %llu B, def %llu B, ref %llu B, intervalos %llu B",
        g_sy.nfiles, g_sy.nsyms, g_sy.nrefs, g_sy.secs, (unsigned long long)g_sy.bytes[0], (unsigned long long)g_sy.bytes[1], (unsigned long long)g_sy.bytes[2], (unsigned long long)g_sy.bytes[3], (unsigned long long)g_sy.bytes[4], (unsigned long long)g_sy.bytes[5], (unsigned long long)g_sy.bytes[6]);
    for (uint32_t i = 0; i < g_sy.cap; i++) { coder_file_free(&g_sy.d[i].cf); free(g_sy.d[i].s); free(g_sy.d[i].r); }
    free(g_sy.d); g_sy.d = NULL; g_sy.cap = g_sy.nd = 0;
}

/* ---- visao de uma secao de simbolos de um segmento ---- */
typedef struct { StoreStrs names; StoreTable docs, syms, refs, bydef, byref, ival; const uint8_t *sigpool; uint64_t sigsz; int has; } SySec;
static int sy_sec_open(const StoreSeg *s, SySec *x) {
    memset(x, 0, sizeof *x);
    const StoreSection *a = store_seg_section(s, SY_T_NAMES), *b = store_seg_section(s, SY_T_DOCS), *c = store_seg_section(s, SY_T_SYMS), *d = store_seg_section(s, SY_T_REFS),
                       *e = store_seg_section(s, SY_T_BYDEF), *f = store_seg_section(s, SY_T_BYREF), *g = store_seg_section(s, SY_T_IVAL);
    if (!a || !b || !c || !d || !e || !f || !g) return 0;
    if (!store_strs_open(a, &x->names) || !store_table_open(b, &x->docs) || x->docs.w != SY_DOCW || !store_table_open(c, &x->syms) || x->syms.w != SY_SYMW
        || !store_table_open(d, &x->refs) || x->refs.w != SY_REFW || !store_table_open(e, &x->bydef) || x->bydef.w != 8 || !store_table_open(f, &x->byref) || x->byref.w != 8
        || !store_table_open(g, &x->ival) || x->ival.w != 4) return 0;
    uint64_t head = 8 + (uint64_t)x->syms.n * SY_SYMW; x->sigpool = c->data + head; x->sigsz = c->size - head;
    x->has = 1; return 1;
}
static void coder_sym_seg_info(const StoreSeg *s, char *out, size_t cap) {
    out[0] = 0; SySec x;
    if (sy_sec_open(s, &x)) snprintf(out, cap, "%u simbolos, %u referencias, %u nomes", x.syms.n, x.refs.n, x.names.n);
}

/* ---- verificacao ---- */
static int coder_sym_verify_seg(const StoreSeg *s, char *err, size_t ecap) {
    int have = 0; for (uint32_t t = SY_T_NAMES; t <= SY_T_IVAL; t++) if (store_seg_section(s, t)) have++;
    if (!have) return 0;
    SySec x; const StoreSection *sd = store_seg_section(s, STS_DOCS);
    if (have != 7 || !sd || !sy_sec_open(s, &x)) { snprintf(err, ecap, "simbolos: secoes incompletas ou malformadas"); return -1; }
    if (x.docs.n != sd->nrec) { snprintf(err, ecap, "simbolos: tabela de documentos tem %u registros, o catalogo %u", x.docs.n, sd->nrec); return -1; }
    for (uint32_t i = 1; i < x.names.n; i++) { uint32_t l1, l2; const char *a = store_strs_get(&x.names, i - 1, &l1), *b = store_strs_get(&x.names, i, &l2); if (st_cmp_bytes(a, l1, b, l2) >= 0) { snprintf(err, ecap, "simbolos: nomes fora de ordem em %u", i); return -1; } }
    uint32_t es = 0, er = 0;
    for (uint32_t i = 0; i < x.docs.n; i++) {
        const uint8_t *r = store_table_at(&x.docs, i); uint32_t s0 = st_get_u32(r), ns = st_get_u32(r + 4), r0 = st_get_u32(r + 8), nr = st_get_u32(r + 12);
        if (ns || nr) { if (s0 != es || r0 != er) { snprintf(err, ecap, "simbolos: documento %u fora da sequencia", i); return -1; } }
        if ((uint64_t)s0 + ns > x.syms.n || (uint64_t)r0 + nr > x.refs.n) { snprintf(err, ecap, "simbolos: documento %u fora dos limites", i); return -1; }
        uint32_t mx = 0, pl0 = 0;
        for (uint32_t k = 0; k < ns; k++) {
            const uint8_t *y = store_table_at(&x.syms, s0 + k); uint32_t l0 = st_get_u32(y + 12), l1 = st_get_u32(y + 16), par = st_get_u32(y + 8), so = st_get_u32(y + 32), sl = st_get_u32(y + 36);
            if (st_get_u32(y) >= x.names.n || l1 < l0 || (k && l0 < pl0) || (par != SY_NONE && par >= ns) || (uint64_t)so + sl > x.sigsz || st_get_u32(y + 44) != i) { snprintf(err, ecap, "simbolos: registro %u do documento %u invalido", k, i); return -1; }
            if (l1 > mx) mx = l1; pl0 = l0;
            if (st_get_u32(store_table_at(&x.ival, s0 + k)) != mx) { snprintf(err, ecap, "simbolos: indice de intervalos errado no simbolo %u do documento %u", k, i); return -1; }
        }
        for (uint32_t k = 0; k < nr; k++) {
            const uint8_t *y = store_table_at(&x.refs, r0 + k); uint32_t fr = st_get_u32(y + 8);
            if (st_get_u32(y) >= x.names.n || (fr != SY_NONE && fr >= ns) || st_get_u32(y + 24) != i) { snprintf(err, ecap, "simbolos: referencia %u do documento %u invalida", k, i); return -1; }
        }
        es += ns; er += nr;
    }
    if (es != x.syms.n || er != x.refs.n) { snprintf(err, ecap, "simbolos: linhas sobrando"); return -1; }
    for (int w = 0; w < 2; w++) {
        const StoreTable *t = w ? &x.byref : &x.bydef; uint32_t lim = w ? x.refs.n : x.syms.n, pid = 0, prow = 0;
        if (t->n != lim) { snprintf(err, ecap, "simbolos: tabela por nome com %u pares para %u linhas", t->n, lim); return -1; }
        for (uint32_t i = 0; i < t->n; i++) {
            const uint8_t *y = store_table_at(t, i); uint32_t id = st_get_u32(y), row = st_get_u32(y + 4);
            if (id >= x.names.n || row >= lim || (i && (id < pid || (id == pid && row <= prow)))) { snprintf(err, ecap, "simbolos: par %u da tabela por nome invalido", i); return -1; }
            const uint8_t *z = store_table_at(w ? &x.refs : &x.syms, row); if (st_get_u32(z) != id) { snprintf(err, ecap, "simbolos: par %u aponta para a linha de outro nome", i); return -1; }
            pid = id; prow = row;
        }
    }
    return 0;
}

/* ---- fusores da compactacao ---- */
static SyOut g_sy_m; static int g_sy_mvalid;
static int sy_merge_names(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) {
    if (g_sy_mvalid) { sy_out_free(&g_sy_m); g_sy_mvalid = 0; }
    uint32_t nn = 0;
    for (int s = 0; s < c->nseg; s++) { const StoreSection *sd = store_seg_section(c->seg[s], STS_DOCS); if (!sd) continue; for (uint32_t i = 0; i < sd->nrec; i++) if (c->docmap[s][i] != UINT32_MAX && c->docmap[s][i] + 1 > nn) nn = c->docmap[s][i] + 1; }
    SyDIn *nd = (SyDIn *)calloc((size_t)nn + 1, sizeof *nd);
    for (int s = 0; s < c->nseg; s++) {
        SySec x; if (!sy_sec_open(c->seg[s], &x)) continue;
        for (uint32_t i = 0; i < x.docs.n; i++) {
            uint32_t g = c->docmap[s][i]; if (g == UINT32_MAX) continue;
            const uint8_t *r = store_table_at(&x.docs, i); uint32_t s0 = st_get_u32(r), ns = st_get_u32(r + 4), r0 = st_get_u32(r + 8), nr = st_get_u32(r + 12);
            SyDIn *y = &nd[g]; y->lang = st_get_u32(r + 16);
            if (!ns && !nr) continue;
            y->s = (SySIn *)calloc((size_t)ns + 1, sizeof(SySIn)); y->r = (SyRIn *)calloc((size_t)nr + 1, sizeof(SyRIn)); y->ns = ns; y->nr = nr;
            for (uint32_t k = 0; k < ns; k++) { const uint8_t *q = store_table_at(&x.syms, s0 + k); SySIn *z = &y->s[k];
                z->name = store_strs_get(&x.names, st_get_u32(q), &z->nl); z->kind = st_get_u32(q + 4); z->parent = st_get_u32(q + 8); z->line0 = st_get_u32(q + 12); z->line1 = st_get_u32(q + 16);
                z->byte0 = st_get_u32(q + 20); z->byte1 = st_get_u32(q + 24); z->body0 = st_get_u32(q + 28); z->sig = (const char *)x.sigpool + st_get_u32(q + 32); z->sl = st_get_u32(q + 36); z->pack = st_get_u32(q + 40); }
            for (uint32_t k = 0; k < nr; k++) { const uint8_t *q = store_table_at(&x.refs, r0 + k); SyRIn *z = &y->r[k];
                z->name = store_strs_get(&x.names, st_get_u32(q), &z->nl); z->kind = st_get_u32(q + 4); z->from = st_get_u32(q + 8); z->line = st_get_u32(q + 12); z->col = st_get_u32(q + 16); z->flags = st_get_u32(q + 20); }
        }
    }
    sy_build(nd, nn, &g_sy_m); g_sy_mvalid = 1;
    for (uint32_t i = 0; i < nn; i++) { free(nd[i].s); free(nd[i].r); }
    free(nd);
    *out = g_sy_m.names; memset(&g_sy_m.names, 0, sizeof(StoreBuf)); *nrec = g_sy_m.nnames; return 0;
}
#define SY_TAKE(field, count) do { *out = g_sy_m.field; memset(&g_sy_m.field, 0, sizeof(StoreBuf)); *nrec = (count); (void)c; return 0; } while (0)
static int sy_merge_docs(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) { SY_TAKE(docs, g_sy_m.docs.n >= 8 ? st_get_u32(g_sy_m.docs.p) : 0); }
static int sy_merge_syms(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) { SY_TAKE(syms, g_sy_m.nsyms); }
static int sy_merge_refs(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) { SY_TAKE(refs, g_sy_m.nrefs); }
static int sy_merge_bydef(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) { SY_TAKE(bydef, g_sy_m.nsyms); }
static int sy_merge_byref(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) { SY_TAKE(byref, g_sy_m.nrefs); }
static int sy_merge_ival(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) { SY_TAKE(ival, g_sy_m.nsyms); }
static void coder_sym_register(void) {
    store_register_merger(SY_T_NAMES, 1, sy_merge_names); store_register_merger(SY_T_DOCS, 1, sy_merge_docs); store_register_merger(SY_T_SYMS, 1, sy_merge_syms);
    store_register_merger(SY_T_REFS, 1, sy_merge_refs); store_register_merger(SY_T_BYDEF, 1, sy_merge_bydef); store_register_merger(SY_T_BYREF, 1, sy_merge_byref);
    store_register_merger(SY_T_IVAL, 1, sy_merge_ival);
}
