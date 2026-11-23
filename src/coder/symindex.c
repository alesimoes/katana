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
 * Chaves de texto (keys.c), quatro secoes proprias (todas presentes ou todas ausentes; segmentos antigos so tem as sete acima):
 *   SY_T_KNAMES (0x207) tabela de strings ordenada por bytes: as chaves do segmento; id = posicao (busca binaria)
 *   SY_T_KDOCS  (0x208) tabela largura 8, um registro por documento: u32 occ0, u32 nocc | 0x80000000 (bit alto = o documento foi analisado pelo
 *                       extrator de chaves; sem o bit = indexado antes de existirem chaves, ou nao analisavel). As ocorrencias de um documento sao contiguas.
 *   SY_T_KOCC   (0x209) tabela largura 16: u32 chave, role | cls<<8 | meth<<16, line, documento. Em ordem de linha dentro do documento.
 *   SY_T_KBYKEY (0x20A) pares (id da chave, linha de KOCC) ordenados: "todas as ocorrencias desta chave" e uma busca binaria
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
#define SY_T_KNAMES (STS_SYM_BASE + 7)
#define SY_T_KDOCS (STS_SYM_BASE + 8)
#define SY_T_KOCC (STS_SYM_BASE + 9)
#define SY_T_KBYKEY (STS_SYM_BASE + 10)
#define SY_KOCCW 16
#define SY_KFLAG 0x80000000u
#define SY_NONE 0xFFFFFFFFu
#define SY_DOCW 24
#define SY_SYMW 48
#define SY_REFW 28

/* ---- entrada do construtor (nomes apontam para quem os possui; nada e copiado) ---- */
typedef struct { const char *name, *sig; uint32_t nl, sl, kind, line0, line1, byte0, byte1, body0, pack, parent; } SySIn;
typedef struct { const char *name; uint32_t nl, kind, line, col, flags, from; } SyRIn;
typedef struct { const char *name; uint32_t nl, pack, line; } SyKIn;
typedef struct { SySIn *s; uint32_t ns; SyRIn *r; uint32_t nr; uint32_t lang; CoderFile cf; SyKIn *k; uint32_t nk; int kflag; double secs; } SyDIn;
typedef struct { StoreBuf names, docs, occ, bykey; uint32_t nnames, nocc; } KyOut;
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
/* monta as quatro secoes de chaves de nd documentos */
static void ky_build(const SyDIn *d, uint32_t nd, KyOut *o) {
    memset(o, 0, sizeof *o);
    uint64_t tn = 0; for (uint32_t i = 0; i < nd; i++) tn += d[i].nk;
    SyNm *all = (SyNm *)malloc(sizeof *all * (size_t)(tn + 1)); uint64_t k = 0;
    for (uint32_t i = 0; i < nd; i++) for (uint32_t q = 0; q < d[i].nk; q++) { all[k].p = d[i].k[q].name; all[k++].l = d[i].k[q].nl; }
    qsort(all, (size_t)k, sizeof *all, sy_nm_cmp);
    uint64_t u = 0; for (uint64_t i = 0; i < k; i++) if (!u || sy_nm_cmp(&all[u - 1], &all[i]) != 0) all[u++] = all[i];
    const char **ps = (const char **)malloc(sizeof *ps * (size_t)(u + 1)); uint32_t *ls = (uint32_t *)malloc(sizeof *ls * (size_t)(u + 1));
    for (uint64_t i = 0; i < u; i++) { ps[i] = all[i].p; ls[i] = all[i].l; }
    store_strs_build(&o->names, ps, ls, (uint32_t)u); o->nnames = (uint32_t)u; free(ps); free(ls);
    uint32_t no = (uint32_t)tn; o->nocc = no;
    store_table_begin(&o->docs, nd, 8); store_table_begin(&o->occ, no, SY_KOCCW);
    uint64_t *pk = (uint64_t *)malloc(sizeof *pk * (size_t)(no + 1)); uint32_t row = 0;
    for (uint32_t i = 0; i < nd; i++) {
        const SyDIn *x = &d[i];
        sb_u32(&o->docs, x->nk ? row : 0); sb_u32(&o->docs, x->nk | (x->kflag ? SY_KFLAG : 0));
        for (uint32_t q = 0; q < x->nk; q++) {
            const SyKIn *y = &x->k[q]; uint32_t id = sy_nm_id(all, (uint32_t)u, y->name, y->nl);
            sb_u32(&o->occ, id); sb_u32(&o->occ, y->pack); sb_u32(&o->occ, y->line); sb_u32(&o->occ, i);
            pk[row] = (uint64_t)id << 32 | row; row++;
        }
    }
    qsort(pk, no, sizeof *pk, sy_cmp_u64);
    store_table_begin(&o->bykey, no, 8); for (uint32_t i = 0; i < no; i++) { sb_u32(&o->bykey, (uint32_t)(pk[i] >> 32)); sb_u32(&o->bykey, (uint32_t)pk[i]); }
    free(pk); free(all);
}
static void ky_out_free(KyOut *o) { sb_free(&o->names); sb_free(&o->docs); sb_free(&o->occ); sb_free(&o->bykey); }
static void sy_out_free(SyOut *o) { sb_free(&o->names); sb_free(&o->docs); sb_free(&o->syms); sb_free(&o->refs); sb_free(&o->bydef); sb_free(&o->byref); sb_free(&o->ival); }

/* ---- gancho do `index`: encadeia com o da busca ----
 * Estado de UMA indexacao num SyHook proprio (alocado por coder_sym_hook, solto por coder_sym_hook_done), sem globais. O gancho se poe na frente do
 * anterior: hook_ctx passa a ser o SyHook e o ctx anterior fica em prev_ctx; coder_sym_hook_done devolve tudo como estava (chame-o ANTES do done da busca).
 * sy_analyze e reentrante (so escreve no SyDIn recebido): o rastreador a roda em varias threads e entrega os resultados a sy_ingest, uma thread so, na
 * ordem dos documentos. */
typedef struct {
    void (*prev_doc)(void *, uint32_t, const StoreDoc *, const uint8_t *, size_t);
    void (*prev_commit)(void *, StoreWriter *, uint32_t);
    void *prev_ctx;
    SyDIn *d; uint32_t nd, cap; char root[4096]; uint32_t nfiles, nsyms, nrefs, nparsed, nkeys; double secs; uint64_t bytes[7];
} SyHook;
/* analisa um documento: x deve estar zerado; reentrante */
static void sy_analyze(const char *root, const StoreDoc *d, const uint8_t *data, size_t n, SyDIn *x) {
    char rel[4100]; uint32_t pl = d->plen < 4000 ? d->plen : 4000; memcpy(rel, d->path, pl); rel[pl] = 0;
    char full[8300]; snprintf(full, sizeof full, "%s/%s", root, rel);
    double t0 = now_sec();
    char *txt = (char *)malloc(n + 1); memcpy(txt, data, n); txt[n] = 0;
    coder_parse(full, txt, n, &x->cf); free(txt);
    CoderFile *f = &x->cf; x->lang = (uint32_t)f->lang;
    if (f->lang != CL_UNKNOWN) {          /* chaves de texto: o documento foi analisado (mesmo sem nenhuma chave) */
        x->kflag = 1;
        if (f->nkey) { x->k = (SyKIn *)calloc((size_t)f->nkey, sizeof(SyKIn)); x->nk = (uint32_t)f->nkey;
            for (int i = 0; i < f->nkey; i++) { const CoderKey *y = &f->key[i]; SyKIn *z = &x->k[i]; z->name = y->key; z->nl = (uint32_t)strlen(y->key); z->line = (uint32_t)y->line; z->pack = (uint32_t)y->role | (uint32_t)y->cls << 8 | (uint32_t)y->meth << 16; } }
    }
    if (f->nsym || f->nref) {
        x->s = (SySIn *)calloc((size_t)f->nsym + 1, sizeof(SySIn)); x->r = (SyRIn *)calloc((size_t)f->nref + 1, sizeof(SyRIn)); x->ns = (uint32_t)f->nsym; x->nr = (uint32_t)f->nref;
        for (int i = 0; i < f->nsym; i++) { const CoderSym *y = &f->sym[i]; SySIn *z = &x->s[i];
            z->name = y->name; z->nl = (uint32_t)strlen(y->name); z->sig = y->sig ? y->sig : ""; z->sl = (uint32_t)strlen(z->sig); z->kind = (uint32_t)y->kind;
            z->line0 = (uint32_t)y->line0; z->line1 = (uint32_t)y->line1; z->byte0 = (uint32_t)y->byte0; z->byte1 = (uint32_t)y->byte1; z->body0 = (uint32_t)y->body0;
            z->pack = (uint32_t)(y->vis & 0xFF) | (uint32_t)(y->conf & 0xFF) << 8 | (uint32_t)(y->flags & 0xFFFF) << 16; z->parent = y->parent >= 0 ? (uint32_t)y->parent : SY_NONE; }
        for (int i = 0; i < f->nref; i++) { const CoderRef *y = &f->ref[i]; SyRIn *z = &x->r[i];
            z->name = y->name; z->nl = (uint32_t)strlen(y->name); z->kind = (uint32_t)y->kind; z->line = (uint32_t)y->line; z->col = (uint32_t)y->col; z->flags = (uint32_t)y->flags; z->from = y->from >= 0 ? (uint32_t)y->from : SY_NONE; }
    }
    x->secs = now_sec() - t0;
}
/* guarda o resultado do documento idx (x = NULL: documento sem analise) e soma os contadores; uma thread so, idx crescente */
static void sy_ingest(SyHook *h, uint32_t idx, SyDIn *x) {
    if (idx >= h->cap) { uint32_t nc = idx + 256; h->d = (SyDIn *)realloc(h->d, sizeof(SyDIn) * nc); memset(h->d + h->cap, 0, sizeof(SyDIn) * (nc - h->cap)); h->cap = nc; }
    if (idx >= h->nd) h->nd = idx + 1;
    if (!x) return;
    h->d[idx] = *x;
    if (x->kflag) h->nparsed++;
    h->nkeys += x->nk;
    if (x->ns || x->nr) { h->nfiles++; h->nsyms += x->ns; h->nrefs += x->nr; }
    h->secs += x->secs;
}
static void sy_doc_hook(void *ctx, uint32_t idx, const StoreDoc *d, const uint8_t *data, size_t n) {
    SyHook *h = (SyHook *)ctx;
    if (h->prev_doc) h->prev_doc(h->prev_ctx, idx, d, data, n);
    if (!data || !n) { sy_ingest(h, idx, NULL); return; }
    SyDIn x; memset(&x, 0, sizeof x);
    sy_analyze(h->root, d, data, n, &x);
    sy_ingest(h, idx, &x);
}
static void sy_commit_hook(void *ctx, StoreWriter *w, uint32_t ndocs) {
    SyHook *h = (SyHook *)ctx;
    if (h->prev_commit) h->prev_commit(h->prev_ctx, w, ndocs);
    if (!h->nfiles && !h->nparsed) return;
    SyDIn *all = (SyDIn *)calloc(ndocs + 1, sizeof *all);
    for (uint32_t i = 0; i < ndocs && i < h->nd; i++) all[i] = h->d[i];
    { KyOut ko; ky_build(all, ndocs, &ko);
      store_writer_add_section_buf(w, SY_T_KNAMES, 1, ko.nnames, &ko.names); store_writer_add_section_buf(w, SY_T_KDOCS, 1, ndocs, &ko.docs);
      store_writer_add_section_buf(w, SY_T_KOCC, 1, ko.nocc, &ko.occ); store_writer_add_section_buf(w, SY_T_KBYKEY, 1, ko.nocc, &ko.bykey); }
    if (!h->nfiles) { free(all); return; }
    SyOut o; sy_build(all, ndocs, &o); free(all);
    h->bytes[0] = o.names.n; h->bytes[1] = o.docs.n; h->bytes[2] = o.syms.n; h->bytes[3] = o.refs.n; h->bytes[4] = o.bydef.n; h->bytes[5] = o.byref.n; h->bytes[6] = o.ival.n;
    store_writer_add_section_buf(w, SY_T_NAMES, 1, o.nnames, &o.names); store_writer_add_section_buf(w, SY_T_DOCS, 1, ndocs, &o.docs);
    store_writer_add_section_buf(w, SY_T_SYMS, 1, o.nsyms, &o.syms); store_writer_add_section_buf(w, SY_T_REFS, 1, o.nrefs, &o.refs);
    store_writer_add_section_buf(w, SY_T_BYDEF, 1, o.nsyms, &o.bydef); store_writer_add_section_buf(w, SY_T_BYREF, 1, o.nrefs, &o.byref);
    store_writer_add_section_buf(w, SY_T_IVAL, 1, o.nsyms, &o.ival);
}
static void coder_sym_hook(StoreScanOpts *o, const char *proj) {
    SyHook *h = (SyHook *)calloc(1, sizeof *h);
    if (!realpath(proj, h->root)) snprintf(h->root, sizeof h->root, "%s", proj);
    h->prev_doc = o->doc_fn; h->prev_commit = o->commit_fn; h->prev_ctx = o->hook_ctx;
    o->hook_ctx = h; o->doc_fn = sy_doc_hook; o->commit_fn = sy_commit_hook;
}
static void coder_sym_hook_done(StoreScanOpts *o, char *out, size_t cap) {
    SyHook *h = (SyHook *)o->hook_ctx; out[0] = 0;
    if (!h) return;
    if (h->nfiles) snprintf(out, cap, "%u arquivos com simbolos, %u simbolos, %u referencias, analise %.3f s; secoes novas: nomes %llu B, documentos %llu B, simbolos %llu B, referencias %llu B, def %llu B, ref %llu B, intervalos %llu B",
        h->nfiles, h->nsyms, h->nrefs, h->secs, (unsigned long long)h->bytes[0], (unsigned long long)h->bytes[1], (unsigned long long)h->bytes[2], (unsigned long long)h->bytes[3], (unsigned long long)h->bytes[4], (unsigned long long)h->bytes[5], (unsigned long long)h->bytes[6]);
    for (uint32_t i = 0; i < h->cap; i++) { coder_file_free(&h->d[i].cf); free(h->d[i].s); free(h->d[i].r); free(h->d[i].k); }
    free(h->d);
    o->hook_ctx = h->prev_ctx; o->doc_fn = h->prev_doc; o->commit_fn = h->prev_commit; free(h);
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
/* visao das secoes de chaves de um segmento */
typedef struct { StoreStrs names; StoreTable docs, occ, bykey; int has; } KySec;
static int ky_sec_open(const StoreSeg *s, KySec *x) {
    memset(x, 0, sizeof *x);
    const StoreSection *a = store_seg_section(s, SY_T_KNAMES), *b = store_seg_section(s, SY_T_KDOCS), *c = store_seg_section(s, SY_T_KOCC), *d = store_seg_section(s, SY_T_KBYKEY);
    if (!a || !b || !c || !d) return 0;
    if (!store_strs_open(a, &x->names) || !store_table_open(b, &x->docs) || x->docs.w != 8 || !store_table_open(c, &x->occ) || x->occ.w != SY_KOCCW
        || !store_table_open(d, &x->bykey) || x->bykey.w != 8) return 0;
    x->has = 1; return 1;
}
static void coder_sym_seg_info(const StoreSeg *s, char *out, size_t cap) {
    out[0] = 0; SySec x; KySec k;
    if (sy_sec_open(s, &x)) snprintf(out, cap, "%u simbolos, %u referencias, %u nomes", x.syms.n, x.refs.n, x.names.n);
    if (ky_sec_open(s, &k)) { size_t l = strlen(out); snprintf(out + l, cap - l, "%s%u chaves de texto (%u ocorrencias)", l ? ", " : "", k.names.n, k.occ.n); }
}
static int coder_key_verify_seg(const StoreSeg *s, char *err, size_t ecap) {
    int have = 0; for (uint32_t t = SY_T_KNAMES; t <= SY_T_KBYKEY; t++) if (store_seg_section(s, t)) have++;
    if (!have) return 0;
    KySec x; const StoreSection *sd = store_seg_section(s, STS_DOCS);
    if (have != 4 || !sd || !ky_sec_open(s, &x)) { snprintf(err, ecap, "chaves: secoes incompletas ou malformadas"); return -1; }
    if (x.docs.n != sd->nrec) { snprintf(err, ecap, "chaves: tabela de documentos tem %u registros, o catalogo %u", x.docs.n, sd->nrec); return -1; }
    for (uint32_t i = 1; i < x.names.n; i++) { uint32_t l1, l2; const char *a = store_strs_get(&x.names, i - 1, &l1), *b = store_strs_get(&x.names, i, &l2); if (st_cmp_bytes(a, l1, b, l2) >= 0) { snprintf(err, ecap, "chaves: nomes fora de ordem em %u", i); return -1; } }
    uint32_t eo = 0;
    for (uint32_t i = 0; i < x.docs.n; i++) {
        const uint8_t *r = store_table_at(&x.docs, i); uint32_t o0 = st_get_u32(r), no = st_get_u32(r + 4) & ~SY_KFLAG;
        if (no) { if (o0 != eo) { snprintf(err, ecap, "chaves: documento %u fora da sequencia", i); return -1; } }
        if ((uint64_t)o0 + no > x.occ.n) { snprintf(err, ecap, "chaves: documento %u fora dos limites", i); return -1; }
        for (uint32_t k = 0; k < no; k++) { const uint8_t *y = store_table_at(&x.occ, o0 + k); uint32_t pk = st_get_u32(y + 4);
            if (st_get_u32(y) >= x.names.n || st_get_u32(y + 12) != i || (pk & 0xFF) < 1 || (pk & 0xFF) > 2 || ((pk >> 8) & 0xFF) >= CKC_N) { snprintf(err, ecap, "chaves: ocorrencia %u do documento %u invalida", k, i); return -1; } }
        eo += no;
    }
    if (eo != x.occ.n) { snprintf(err, ecap, "chaves: ocorrencias sobrando"); return -1; }
    if (x.bykey.n != x.occ.n) { snprintf(err, ecap, "chaves: tabela por chave com %u pares para %u ocorrencias", x.bykey.n, x.occ.n); return -1; }
    uint32_t pid = 0, prow = 0;
    for (uint32_t i = 0; i < x.bykey.n; i++) {
        const uint8_t *y = store_table_at(&x.bykey, i); uint32_t id = st_get_u32(y), row = st_get_u32(y + 4);
        if (id >= x.names.n || row >= x.occ.n || (i && (id < pid || (id == pid && row <= prow)))) { snprintf(err, ecap, "chaves: par %u da tabela por chave invalido", i); return -1; }
        if (st_get_u32(store_table_at(&x.occ, row)) != id) { snprintf(err, ecap, "chaves: par %u aponta para a ocorrencia de outra chave", i); return -1; }
        pid = id; prow = row;
    }
    return 0;
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
static KyOut g_ky_m; static int g_ky_mvalid;
static int ky_merge_names(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) {
    if (g_ky_mvalid) { ky_out_free(&g_ky_m); g_ky_mvalid = 0; }
    uint32_t nn = 0;
    for (int s = 0; s < c->nseg; s++) { const StoreSection *sd = store_seg_section(c->seg[s], STS_DOCS); if (!sd) continue; for (uint32_t i = 0; i < sd->nrec; i++) if (c->docmap[s][i] != UINT32_MAX && c->docmap[s][i] + 1 > nn) nn = c->docmap[s][i] + 1; }
    SyDIn *nd = (SyDIn *)calloc((size_t)nn + 1, sizeof *nd);
    for (int s = 0; s < c->nseg; s++) {
        KySec x; if (!ky_sec_open(c->seg[s], &x)) continue;
        for (uint32_t i = 0; i < x.docs.n; i++) {
            uint32_t g = c->docmap[s][i]; if (g == UINT32_MAX) continue;
            const uint8_t *r = store_table_at(&x.docs, i); uint32_t o0 = st_get_u32(r), w1 = st_get_u32(r + 4), no = w1 & ~SY_KFLAG;
            SyDIn *y = &nd[g]; y->kflag = (w1 & SY_KFLAG) != 0;
            if (!no) continue;
            y->k = (SyKIn *)calloc((size_t)no, sizeof(SyKIn)); y->nk = no;
            for (uint32_t k = 0; k < no; k++) { const uint8_t *q = store_table_at(&x.occ, o0 + k); SyKIn *z = &y->k[k]; z->name = store_strs_get(&x.names, st_get_u32(q), &z->nl); z->pack = st_get_u32(q + 4); z->line = st_get_u32(q + 8); }
        }
    }
    ky_build(nd, nn, &g_ky_m); g_ky_mvalid = 1;
    for (uint32_t i = 0; i < nn; i++) free(nd[i].k);
    free(nd);
    *out = g_ky_m.names; memset(&g_ky_m.names, 0, sizeof(StoreBuf)); *nrec = g_ky_m.nnames; return 0;
}
#define KY_TAKE(field, count) do { *out = g_ky_m.field; memset(&g_ky_m.field, 0, sizeof(StoreBuf)); *nrec = (count); (void)c; return 0; } while (0)
static int ky_merge_docs(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) { KY_TAKE(docs, g_ky_m.docs.n >= 8 ? st_get_u32(g_ky_m.docs.p) : 0); }
static int ky_merge_occ(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) { KY_TAKE(occ, g_ky_m.nocc); }
static int ky_merge_bykey(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) { KY_TAKE(bykey, g_ky_m.nocc); }
static void coder_sym_register(void) {
    store_register_merger(SY_T_KNAMES, 1, ky_merge_names); store_register_merger(SY_T_KDOCS, 1, ky_merge_docs); store_register_merger(SY_T_KOCC, 1, ky_merge_occ); store_register_merger(SY_T_KBYKEY, 1, ky_merge_bykey);
    store_register_merger(SY_T_NAMES, 1, sy_merge_names); store_register_merger(SY_T_DOCS, 1, sy_merge_docs); store_register_merger(SY_T_SYMS, 1, sy_merge_syms);
    store_register_merger(SY_T_REFS, 1, sy_merge_refs); store_register_merger(SY_T_BYDEF, 1, sy_merge_bydef); store_register_merger(SY_T_BYREF, 1, sy_merge_byref);
    store_register_merger(SY_T_IVAL, 1, sy_merge_ival);
}
