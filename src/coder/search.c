/* ---------- coder/search.c: busca por nome e texto sobre o armazenamento (nivel de ARQUIVO; a unidade passara a ser o simbolo) ----------
 * Indice de busca em secoes do armazenamento (faixa 0x100, store.c), escrito por segmento junto com o catalogo (gancho do `index` em storedocs.c):
 *   CS_T_TERMS (0x100)  lexico: tabela de registros de 24 bytes ordenada pelos bytes da chave + pool de chaves.
 *                       registro: u32 off, u32 len (chave), u32 df, u32 ctf (soma das frequencias), u32 poff, u32 plen (no pool de postings)
 *                       chave = 1 byte de tipo + termo: 'n' forma normalizada (raiz canonica; identificador inteiro = partes minusculas unidas por _),
 *                       'o' forma original em minusculas (so quando difere da normalizada)
 *   CS_T_POSTS (0x101)  postings: por termo, df entradas ordenadas por documento: varint delta do documento, u8 mascara de campos, e um varint
 *                       de frequencia por campo presente. Documento = indice LOCAL ao segmento (posicao na secao STS_DOCS).
 *   CS_T_LENS  (0x102)  tabela de n documentos x 12 bytes: u32 tokens de caminho, de nome de arquivo e de conteudo (para BM25 por campos)
 *   CS_T_TRI   (0x103)  trigramas DO LEXICO (nao do texto): tabela de 12 bytes {u32 trigrama (3 bytes), u32 off, u32 n} + pool de listas de
 *                       indices de termos (delta varint). Derivada do lexico: a compactacao a reconstroi do lexico fundido.
 * Campos: 0 caminho (diretorios), 1 nome do arquivo, 2 conteudo/palavra (inclui o identificador inteiro), 3 conteudo/subpalavra (parte de identificador).
 * Fusores (compactacao): remapeiam documentos pelo docmap (documentos mortos/substituidos somem) e fundem os lexicos por fusao k-vias.
 * Limites: so os primeiros KATANA_CODER_INDEX_KB (128) KB de cada arquivo de texto entram no conteudo; hashes, numeros longos e termos de
 * uma letra ficam fora; palavras de funcao/pedido (flag stop) nao entram no conteudo.
 * Consulta: ver a segunda metade deste arquivo (canais, fusao, evidencia). */
#define CS_T_TERMS (STS_LEX_BASE + 0)
#define CS_T_POSTS (STS_LEX_BASE + 1)
#define CS_T_LENS (STS_LEX_BASE + 2)
#define CS_T_TRI (STS_LEX_BASE + 3)
#define CS_LEXW 24
#define CS_LENW 12
#define CS_TRIW 12
enum { CF_PATH, CF_NAME, CF_WORD, CF_PART, CF_N };

typedef struct { uint32_t doc; uint16_t tf[CF_N]; } CsHit;

/* ---- visao de um lexico (secao CS_T_TERMS) ---- */
typedef struct { StoreTable t; const uint8_t *pool; uint64_t poolsz; } CsLex;
static int cs_lex_open(const StoreSection *s, CsLex *x) {
    memset(x, 0, sizeof *x);
    if (!s || !store_table_open(s, &x->t) || x->t.w != CS_LEXW) { x->t.n = 0; return 0; }
    uint64_t head = 8 + (uint64_t)x->t.n * x->t.w;
    if (head > s->size) { x->t.n = 0; return 0; }
    x->pool = s->data + head; x->poolsz = s->size - head;
    return 1;
}
static const char *cs_lex_key(const CsLex *x, uint32_t i, uint32_t *len) {
    const uint8_t *r = store_table_at(&x->t, i); uint32_t off = st_get_u32(r), l = st_get_u32(r + 4);
    if ((uint64_t)off + l > x->poolsz) { *len = 0; return ""; }
    *len = l; return (const char *)x->pool + off;
}
static uint32_t cs_lex_df(const CsLex *x, uint32_t i) { return st_get_u32(store_table_at(&x->t, i) + 8); }
static int64_t cs_lex_find(const CsLex *x, const char *key, size_t kl) {
    uint32_t lo = 0, hi = x->t.n;
    while (lo < hi) {
        uint32_t m = lo + (hi - lo) / 2, l; const char *s = cs_lex_key(x, m, &l);
        int c = st_cmp_bytes(s, l, key, kl);
        if (c == 0) return m;
        if (c < 0) lo = m + 1; else hi = m;
    }
    return -1;
}
/* decodifica ate df entradas; devolve quantas ou -1 se truncado/inconsistente */
static int64_t cs_decode_post(const uint8_t *p, const uint8_t *end, uint32_t df, CsHit *out) {
    uint32_t doc = 0;
    for (uint32_t i = 0; i < df; i++) {
        uint64_t d, f; p = st_get_varint(p, end, &d); if (!p || p >= end) return -1;
        doc += (uint32_t)d; uint8_t mask = *p++;
        if (mask == 0 || mask > 15) return -1;
        out[i].doc = doc; memset(out[i].tf, 0, sizeof out[i].tf);
        for (int k = 0; k < CF_N; k++) if (mask >> k & 1) { p = st_get_varint(p, end, &f); if (!p) return -1; out[i].tf[k] = f > 65535 ? 65535 : (uint16_t)f; }
    }
    return df;
}
static void cs_post_put(StoreBuf *b, uint32_t *last, const CsHit *h) {
    uint8_t mask = 0; for (int k = 0; k < CF_N; k++) if (h->tf[k]) mask |= (uint8_t)(1u << k);
    sb_varint(b, h->doc - *last); *last = h->doc; sb_u8(b, mask);
    for (int k = 0; k < CF_N; k++) if (h->tf[k]) sb_varint(b, h->tf[k]);
}

/* ---- trigramas do lexico ---- */
static int cs_cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static uint32_t cs_tri_of(const char *s) { return (uint32_t)(uint8_t)s[0] << 16 | (uint32_t)(uint8_t)s[1] << 8 | (uint32_t)(uint8_t)s[2]; }
static void cs_tri_build(StoreBuf *out, const CsLex *x, uint32_t *nrec) {
    uint64_t *v = NULL; size_t n = 0, cap = 0;
    for (uint32_t i = 0; i < x->t.n; i++) {
        uint32_t kl; const char *k = cs_lex_key(x, i, &kl);
        if (kl < 4) continue;                                        /* tipo + >= 3 bytes */
        const char *s = k + 1; uint32_t l = kl - 1; if (l > 64) l = 64;
        uint32_t seen[64]; int ns = 0;
        for (uint32_t j = 0; j + 3 <= l; j++) {
            uint32_t t = cs_tri_of(s + j); int dup = 0;
            for (int q = 0; q < ns; q++) if (seen[q] == t) { dup = 1; break; }
            if (dup) continue;
            seen[ns++] = t;
            if (n == cap) { cap = cap ? cap * 2 : 4096; v = (uint64_t *)realloc(v, cap * sizeof *v); }
            v[n++] = (uint64_t)t << 32 | i;
        }
    }
    qsort(v, n, sizeof *v, cs_cmp_u64);
    uint32_t ng = 0; for (size_t i = 0; i < n; i++) if (!i || (v[i] >> 32) != (v[i - 1] >> 32)) ng++;
    StoreBuf pool = {0}; StoreBuf rec = {0};
    for (size_t i = 0; i < n;) {
        size_t j = i; while (j < n && (v[j] >> 32) == (v[i] >> 32)) j++;
        sb_u32(&rec, (uint32_t)(v[i] >> 32)); sb_u32(&rec, (uint32_t)pool.n); sb_u32(&rec, (uint32_t)(j - i));
        uint32_t prev = 0; for (size_t q = i; q < j; q++) { uint32_t id = (uint32_t)v[q]; sb_varint(&pool, id - prev); prev = id; }
        i = j;
    }
    store_table_begin(out, ng, CS_TRIW); sb_bytes(out, rec.p, rec.n); sb_bytes(out, pool.p, pool.n);
    *nrec = ng; sb_free(&pool); sb_free(&rec); free(v);
}

/* ---- construcao do indice de um segmento (durante o `index`) ---- */
typedef struct { char *key; uint32_t klen, df, lastdoc; uint64_t ctf; StoreBuf post; } CsT;
/* Analise de UM documento (N1), separada da fusao no indice do segmento para poder rodar em varias threads (fase 5): CsAn e o contexto de uma
 * thread (buffer de tokens e tabela local), CsDocRes o resultado de um documento (termos distintos com a contagem por campo, tamanhos, tokens).
 * A analise nao toca em nada compartilhado; a fusao (cs_ix_merge) e feita por UMA thread, na ordem dos documentos, e por isso o indice e igual
 * ao do caminho de uma thread so, byte a byte. */
typedef struct { uint32_t off, klen; uint16_t tf[CF_N]; } CsDT;
typedef struct { CsDT *t; uint32_t n, cap; char *pool; size_t pn, pcap; uint32_t len[3]; uint64_t ntok; int has_text; } CsDocRes;
typedef struct { CoderTok *tk; uint32_t kb_limit; uint32_t *tab; uint32_t tabn, tabcap; CsDocRes *r; } CsAn;
typedef struct {
    CsT **tab; uint32_t cap, n;
    uint32_t *lens; uint32_t nlens;                       /* 3 por documento do segmento */
    CsAn *an; CsDocRes scratch; int ndoc_text; uint64_t ntok;      /* caminho de uma thread: analisa e funde no mesmo gancho */
    uint32_t nterms; uint64_t posts_bytes; uint32_t terms_total; uint64_t sec_bytes[4];
} CsIx;
static void cs_res_free(CsDocRes *r) { free(r->t); free(r->pool); memset(r, 0, sizeof *r); }
static CsAn *cs_an_new(uint32_t kb_limit) {
    CsAn *a = (CsAn *)calloc(1, sizeof *a); a->kb_limit = kb_limit ? kb_limit : 128;
    a->tk = (CoderTok *)malloc(4096 * sizeof(CoderTok)); ct_init();         /* ct_init antes de qualquer thread (tambem e seguro depois: ver text.c) */
    return a;
}
static void cs_an_free(CsAn *a) { if (!a) return; free(a->tk); free(a->tab); free(a); }
static uint32_t cs_kb_limit(void) { const char *e = getenv("KATANA_CODER_INDEX_KB"); return e && atoi(e) > 0 ? (uint32_t)atoi(e) : 128; }

static CsT *cs_ix_term(CsIx *ix, const char *key, uint32_t kl) {
    if (ix->n * 2 >= ix->cap) {
        uint32_t nc = ix->cap ? ix->cap * 2 : 4096; CsT **nt = (CsT **)calloc(nc, sizeof *nt);
        for (uint32_t i = 0; i < ix->cap; i++) if (ix->tab[i]) { uint32_t h = (uint32_t)store_hash64(ix->tab[i]->key, ix->tab[i]->klen, 3) & (nc - 1); while (nt[h]) h = (h + 1) & (nc - 1); nt[h] = ix->tab[i]; }
        free(ix->tab); ix->tab = nt; ix->cap = nc;
    }
    uint32_t h = (uint32_t)store_hash64(key, kl, 3) & (ix->cap - 1);
    while (ix->tab[h]) { CsT *t = ix->tab[h]; if (t->klen == kl && !memcmp(t->key, key, kl)) return t; h = (h + 1) & (ix->cap - 1); }
    CsT *t = (CsT *)calloc(1, sizeof *t); t->key = (char *)malloc(kl); memcpy(t->key, key, kl); t->klen = kl;
    ix->tab[h] = t; ix->n++;
    return t;
}
/* conta um termo no resultado do documento (tabela de enderecamento aberto local; nada compartilhado) */
static void cs_an_add(CsAn *a, char kind, const char *sv, uint32_t l, int field) {
    char key[200]; if (l > 190) return;
    key[0] = kind; memcpy(key + 1, sv, l); const uint32_t kl = l + 1;
    CsDocRes *r = a->r;
    if ((r->n + 1) * 2 > a->tabn) {
        uint32_t nn = a->tabn * 2;
        if (nn > a->tabcap) { a->tab = (uint32_t *)realloc(a->tab, sizeof(uint32_t) * nn); a->tabcap = nn; }
        a->tabn = nn; memset(a->tab, 0, sizeof(uint32_t) * nn);
        for (uint32_t i = 0; i < r->n; i++) { uint32_t h = (uint32_t)store_hash64(r->pool + r->t[i].off, r->t[i].klen, 3) & (nn - 1); while (a->tab[h]) h = (h + 1) & (nn - 1); a->tab[h] = i + 1; }
    }
    uint32_t h = (uint32_t)store_hash64(key, kl, 3) & (a->tabn - 1);
    CsDT *t = NULL;
    while (a->tab[h]) { CsDT *x = &r->t[a->tab[h] - 1]; if (x->klen == kl && !memcmp(r->pool + x->off, key, kl)) { t = x; break; } h = (h + 1) & (a->tabn - 1); }
    if (!t) {
        if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 256; r->t = (CsDT *)realloc(r->t, sizeof(CsDT) * r->cap); }
        if (r->pn + kl > r->pcap) { r->pcap = (r->pcap + kl) * 2; r->pool = (char *)realloc(r->pool, r->pcap); }
        t = &r->t[r->n]; memset(t, 0, sizeof *t); t->off = (uint32_t)r->pn; t->klen = kl; memcpy(r->pool + r->pn, key, kl); r->pn += kl;
        a->tab[h] = ++r->n;
    }
    if (t->tf[field] < 65535) t->tf[field]++;
}
/* termo sem valor de busca: hash, numero longo, muito comprido */
static int cs_junk(const char *s, int l) {
    if (l > 40) return 1;
    int dig = 0, hex = 0, alpha = 0;
    for (int i = 0; i < l; i++) { unsigned char c = (unsigned char)s[i]; if (isdigit(c)) { dig++; hex++; } else if (c >= 'a' && c <= 'f') { hex++; alpha++; } else if (isalpha(c)) alpha++; }
    if (dig == l && l > 6) return 1;
    if (l >= 12 && hex == l && dig > 0) return 1;                   /* hash hexadecimal */
    if (l >= 20 && dig * 3 > l) return 1;
    return 0;
}
static void cs_lower(char *o, const char *s, size_t cap) { size_t i = 0; for (; s[i] && i + 1 < cap; i++) o[i] = (char)tolower((unsigned char)s[i]); o[i] = 0; }
/* a parte esta colada a outra em um identificador? (caractere vizinho alfanumerico, ou _ . - seguido de alfanumerico) */
static int cs_is_part(const char *line, int pos, int len, int linelen) {
    if (pos > 0) { unsigned char c = (unsigned char)line[pos - 1]; if (isalnum(c) || c == '_') return 1;
        if ((c == '.' || c == '-') && pos > 1 && isalnum((unsigned char)line[pos - 2])) return 1; }
    if (pos + len < linelen) { unsigned char c = (unsigned char)line[pos + len]; if (isalnum(c) || c == '_') return 1;
        if ((c == '.' || c == '-') && pos + len + 1 < linelen && isalnum((unsigned char)line[pos + len + 1])) return 1; }
    return 0;
}
/* tokeniza um trecho NUL-terminado no campo informado; body=1 aplica as regras do conteudo (sem stop, kind 2 fora, campos 2/3) */
static void cs_an_text(CsAn *a, const char *text, int field, int body, uint32_t *len) {
    int n = coder_tokens_ex(text, a->tk, 4096, field, body ? CT_AUTO : CT_L_CODE);
    int tl = (int)strlen(text);
    for (int i = 0; i < n; i++) {
        const CoderTok *t = &a->tk[i];
        if (t->kind == 2) continue;
        if (body && t->stop) continue;
        int nl = (int)strlen(t->norm); if (nl < 2 && !isdigit((unsigned char)t->norm[0])) continue;
        if (cs_junk(t->norm, nl)) continue;
        int f = field;
        if (body) f = (t->kind == 0 && cs_is_part(text, t->pos, (int)strlen(t->orig), tl)) ? CF_PART : CF_WORD;
        cs_an_add(a, 'n', t->norm, (uint32_t)nl, f);
        char lo[64]; cs_lower(lo, t->orig, sizeof lo);
        if (strcmp(lo, t->norm) && !cs_junk(lo, (int)strlen(lo))) cs_an_add(a, 'o', lo, (uint32_t)strlen(lo), f);
        if (t->kind == 0) { (*len)++; a->r->ntok++; }
    }
}
/* analisa um documento (reentrante: so mexe em `a` e em `r`); data = conteudo (NULL se binario, grande ou ilegivel) */
static void cs_an_doc(CsAn *a, const StoreDoc *d, const uint8_t *data, size_t n, CsDocRes *r) {
    a->r = r; r->n = 0; r->pn = 0; r->len[0] = r->len[1] = r->len[2] = 0; r->ntok = 0; r->has_text = 0;
    a->tabn = 128; if (a->tabcap < a->tabn) { a->tab = (uint32_t *)realloc(a->tab, sizeof(uint32_t) * a->tabn); a->tabcap = a->tabn; }
    memset(a->tab, 0, sizeof(uint32_t) * a->tabn);
    char p[4200]; uint32_t pl = d->plen < 4000 ? d->plen : 4000; memcpy(p, d->path, pl); p[pl] = 0;
    char *sl = strrchr(p, '/');
    if (sl) { *sl = 0; cs_an_text(a, p, CF_PATH, 0, &r->len[0]); cs_an_text(a, sl + 1, CF_NAME, 0, &r->len[1]); }
    else cs_an_text(a, p, CF_NAME, 0, &r->len[1]);
    if (data) {
        size_t lim = (size_t)a->kb_limit * 1024; if (n > lim) n = lim;
        char line[4100];
        size_t i = 0; r->has_text = 1;
        while (i < n) {
            size_t j = i; while (j < n && data[j] != '\n' && j - i < 4000) j++;
            size_t l = j - i; memcpy(line, data + i, l); line[l] = 0;
            for (size_t q = 0; q < l; q++) if (line[q] == 0) line[q] = ' ';
            if (l >= 2) cs_an_text(a, line, CF_WORD, 1, &r->len[2]);
            i = j < n && data[j] == '\n' ? j + 1 : j;
        }
    }
}
/* funde o resultado de um documento no indice do segmento: UMA thread, documentos na ordem do segmento (idx crescente) */
static void cs_ix_merge(CsIx *ix, uint32_t idx, const CsDocRes *r) {
    if ((idx + 1) * 3 > ix->nlens) { uint32_t nl = (idx + 1) * 3 + 3000; ix->lens = (uint32_t *)realloc(ix->lens, nl * sizeof *ix->lens); memset(ix->lens + ix->nlens, 0, (nl - ix->nlens) * sizeof *ix->lens); ix->nlens = nl; }
    memcpy(ix->lens + (size_t)idx * 3, r->len, sizeof r->len);
    ix->ntok += r->ntok; ix->ndoc_text += r->has_text;
    for (uint32_t i = 0; i < r->n; i++) {
        const CsDT *d = &r->t[i]; CsT *t = cs_ix_term(ix, r->pool + d->off, d->klen);
        CsHit h; h.doc = idx; memcpy(h.tf, d->tf, sizeof h.tf);
        cs_post_put(&t->post, &t->lastdoc, &h); t->df++;
        for (int k = 0; k < CF_N; k++) t->ctf += d->tf[k];
    }
}
static void cs_hook_doc(void *ctx, uint32_t idx, const StoreDoc *d, const uint8_t *data, size_t n) {
    CsIx *ix = (CsIx *)ctx;
    cs_an_doc(ix->an, d, data, n, &ix->scratch);
    cs_ix_merge(ix, idx, &ix->scratch);
}
static int cs_cmp_t(const void *a, const void *b) { const CsT *x = *(CsT *const *)a, *y = *(CsT *const *)b; return st_cmp_bytes(x->key, x->klen, y->key, y->klen); }
static void cs_hook_commit(void *ctx, StoreWriter *w, uint32_t ndocs) {
    CsIx *ix = (CsIx *)ctx;
    CsT **v = (CsT **)malloc(sizeof *v * (size_t)(ix->n + 1)); uint32_t k = 0;
    for (uint32_t i = 0; i < ix->cap; i++) if (ix->tab[i]) v[k++] = ix->tab[i];
    qsort(v, k, sizeof *v, cs_cmp_t);
    StoreBuf rec = {0}, pool = {0}, posts = {0};
    for (uint32_t i = 0; i < k; i++) {
        CsT *t = v[i];
        sb_u32(&rec, (uint32_t)pool.n); sb_u32(&rec, t->klen); sb_u32(&rec, t->df); sb_u32(&rec, t->ctf > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)t->ctf);
        sb_u32(&rec, (uint32_t)posts.n); sb_u32(&rec, (uint32_t)t->post.n);
        sb_bytes(&pool, t->key, t->klen); sb_bytes(&posts, t->post.p, t->post.n);
    }
    StoreBuf lex = {0}; store_table_begin(&lex, k, CS_LEXW); sb_bytes(&lex, rec.p, rec.n); sb_bytes(&lex, pool.p, pool.n);
    sb_free(&rec); sb_free(&pool);
    StoreBuf lens = {0}; store_table_begin(&lens, ndocs, CS_LENW);
    for (uint32_t i = 0; i < ndocs; i++) for (int f = 0; f < 3; f++) sb_u32(&lens, (size_t)i * 3 + f < ix->nlens ? ix->lens[(size_t)i * 3 + f] : 0);
    StoreBuf tri = {0}; uint32_t ntri = 0;
    if (k) { CsLex lx; StoreSection s; memset(&s, 0, sizeof s); s.data = lex.p; s.size = lex.n; cs_lex_open(&s, &lx); cs_tri_build(&tri, &lx, &ntri); }
    ix->sec_bytes[0] = lex.n; ix->sec_bytes[1] = posts.n; ix->sec_bytes[2] = lens.n; ix->sec_bytes[3] = tri.n; ix->nterms = k;
    if (k) {
        store_writer_add_section_buf(w, CS_T_TERMS, 1, k, &lex);
        store_writer_add_section_buf(w, CS_T_POSTS, 1, k, &posts);
        store_writer_add_section_buf(w, CS_T_LENS, 1, ndocs, &lens);
        store_writer_add_section_buf(w, CS_T_TRI, 1, ntri, &tri);
    } else { sb_free(&lex); sb_free(&posts); sb_free(&lens); sb_free(&tri); }
    for (uint32_t i = 0; i < k; i++) { sb_free(&v[i]->post); free(v[i]->key); free(v[i]); }
    free(v); free(ix->tab); ix->tab = NULL; ix->cap = ix->n = 0;
}
static void coder_search_hook(StoreScanOpts *o) {
    CsIx *ix = (CsIx *)calloc(1, sizeof *ix);          /* contexto proprio de cada indexacao: nada global */
    ix->an = cs_an_new(cs_kb_limit());
    o->hook_ctx = ix; o->doc_fn = cs_hook_doc; o->commit_fn = cs_hook_commit;
}
static void coder_search_hook_done(StoreScanOpts *o, char *out, size_t cap) {
    CsIx *ix = (CsIx *)o->hook_ctx; out[0] = 0;
    if (!ix) return;
    if (ix->nterms) snprintf(out, cap, "%u termos, %d arquivos de texto lidos, %llu tokens; secoes novas: lexico %llu B, postings %llu B, tamanhos %llu B, trigramas %llu B",
        ix->nterms, ix->ndoc_text, (unsigned long long)ix->ntok, (unsigned long long)ix->sec_bytes[0], (unsigned long long)ix->sec_bytes[1], (unsigned long long)ix->sec_bytes[2], (unsigned long long)ix->sec_bytes[3]);
    else if (ix->ndoc_text == 0) snprintf(out, cap, "nada novo para indexar");
    cs_an_free(ix->an); cs_res_free(&ix->scratch); free(ix->lens); free(ix->tab); free(ix);
    o->hook_ctx = NULL; o->doc_fn = NULL; o->commit_fn = NULL;
}
static void coder_search_seg_info(const StoreSeg *s, char *out, size_t cap) {
    out[0] = 0;
    const StoreSection *a = store_seg_section(s, CS_T_TERMS), *b = store_seg_section(s, CS_T_POSTS), *c = store_seg_section(s, CS_T_TRI);
    if (a) snprintf(out, cap, "%u termos, lexico %llu B, postings %llu B, trigramas %llu B", a->nrec, (unsigned long long)a->size, b ? (unsigned long long)b->size : 0ull, c ? (unsigned long long)c->size : 0ull);
}

/* ---- verificacao ---- */
static int coder_search_verify_seg(const StoreSeg *s, char *err, size_t ecap) {
    const StoreSection *sl = store_seg_section(s, CS_T_TERMS), *sp = store_seg_section(s, CS_T_POSTS), *sn = store_seg_section(s, CS_T_LENS), *st = store_seg_section(s, CS_T_TRI), *sd = store_seg_section(s, STS_DOCS);
    if (!sl && !sp && !sn && !st) return 0;
    if (!sl || !sp || !sn || !st || !sd) { snprintf(err, ecap, "busca: secoes incompletas (lexico, postings, tamanhos, trigramas e documentos devem andar juntos)"); return -1; }
    CsLex x; if (!cs_lex_open(sl, &x)) { snprintf(err, ecap, "busca: lexico malformado"); return -1; }
    StoreTable lt; if (!store_table_open(sn, &lt) || lt.w != CS_LENW || lt.n != sd->nrec) { snprintf(err, ecap, "busca: tabela de tamanhos nao confere com os documentos (%u x %u)", lt.n, sd->nrec); return -1; }
    uint32_t ndocs = sd->nrec; CsHit *tmp = NULL; uint32_t tcap = 0; uint32_t pkl = 0; const char *pk = NULL; uint64_t expect_off = 0;
    for (uint32_t i = 0; i < x.t.n; i++) {
        uint32_t kl; const char *k = cs_lex_key(&x, i, &kl);
        if (kl < 2 || (k[0] != 'n' && k[0] != 'o')) { snprintf(err, ecap, "busca: chave invalida no termo %u", i); return -1; }
        if (i && st_cmp_bytes(pk, pkl, k, kl) >= 0) { snprintf(err, ecap, "busca: lexico fora de ordem no termo %u", i); free(tmp); return -1; }
        pk = k; pkl = kl;
        const uint8_t *r = store_table_at(&x.t, i); uint32_t df = st_get_u32(r + 8), poff = st_get_u32(r + 16), plen = st_get_u32(r + 20);
        if (poff != expect_off || (uint64_t)poff + plen > sp->size) { snprintf(err, ecap, "busca: postings do termo %u fora dos limites", i); free(tmp); return -1; }
        expect_off = (uint64_t)poff + plen;
        if (df > tcap) { tcap = df + 64; tmp = (CsHit *)realloc(tmp, tcap * sizeof *tmp); }
        if (cs_decode_post(sp->data + poff, sp->data + poff + plen, df, tmp) != (int64_t)df) { snprintf(err, ecap, "busca: postings do termo %u truncados", i); free(tmp); return -1; }
        for (uint32_t q = 0; q < df; q++) if (tmp[q].doc >= ndocs || (q && tmp[q].doc <= tmp[q - 1].doc)) { snprintf(err, ecap, "busca: documento invalido ou fora de ordem nos postings do termo %u", i); free(tmp); return -1; }
    }
    free(tmp);
    if (expect_off != sp->size) { snprintf(err, ecap, "busca: sobram bytes nos postings"); return -1; }
    StoreTable tt; if (!store_table_open(st, &tt) || tt.w != CS_TRIW) { snprintf(err, ecap, "busca: tabela de trigramas malformada"); return -1; }
    uint64_t head = 8 + (uint64_t)tt.n * tt.w; if (head > st->size) { snprintf(err, ecap, "busca: trigramas fora dos limites"); return -1; }
    const uint8_t *tp = st->data + head, *te = st->data + st->size; uint32_t prev = 0;
    for (uint32_t i = 0; i < tt.n; i++) {
        const uint8_t *r = store_table_at(&tt, i); uint32_t tri = st_get_u32(r), off = st_get_u32(r + 4), cnt = st_get_u32(r + 8);
        if (i && tri <= prev) { snprintf(err, ecap, "busca: trigramas fora de ordem"); return -1; }
        prev = tri;
        if (off > (uint64_t)(te - tp)) { snprintf(err, ecap, "busca: lista de trigrama fora dos limites"); return -1; }
        const uint8_t *p = tp + off; uint32_t id = 0;
        for (uint32_t q = 0; q < cnt; q++) { uint64_t d; p = st_get_varint(p, te, &d); if (!p) { snprintf(err, ecap, "busca: lista de trigrama truncada"); return -1; } id += (uint32_t)d; if (id >= x.t.n) { snprintf(err, ecap, "busca: trigrama aponta para termo inexistente"); return -1; } }
    }
    return 0;
}

/* ---- fusores da compactacao ---- */
static StoreBuf g_cs_mp, g_cs_ml; static uint32_t g_cs_mn;     /* postings e lexico fundidos, entregues aos fusores seguintes (a ordem dos tipos e crescente) */
typedef struct { CsLex x; const StoreSection *sp; uint32_t i; } CsCur;
static int cs_cmp_hit(const void *a, const void *b) { uint32_t x = ((const CsHit *)a)->doc, y = ((const CsHit *)b)->doc; return x < y ? -1 : x > y; }
static int cs_merge_terms(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) {
    sb_free(&g_cs_mp); sb_free(&g_cs_ml); g_cs_mn = 0; *nrec = 0;
    CsCur *cu = (CsCur *)calloc((size_t)c->nseg, sizeof *cu);
    uint32_t ndocs_seg[256]; if (c->nseg > 256) { free(cu); return -1; }
    for (int s = 0; s < c->nseg; s++) {
        cs_lex_open(store_seg_section(c->seg[s], CS_T_TERMS), &cu[s].x); cu[s].sp = store_seg_section(c->seg[s], CS_T_POSTS);
        if (!cu[s].sp) cu[s].x.t.n = 0;
        const StoreSection *sd = store_seg_section(c->seg[s], STS_DOCS); ndocs_seg[s] = sd ? sd->nrec : 0;
    }
    StoreBuf rec = {0}, pool = {0}, posts = {0}; uint32_t nt = 0; CsHit *ent = NULL; size_t ecap = 0;
    for (;;) {
        const char *mk = NULL; uint32_t ml = 0;
        for (int s = 0; s < c->nseg; s++) if (cu[s].i < cu[s].x.t.n) {
            uint32_t l; const char *k = cs_lex_key(&cu[s].x, cu[s].i, &l);
            if (!mk || st_cmp_bytes(k, l, mk, ml) < 0) { mk = k; ml = l; }
        }
        if (!mk) break;
        size_t ne = 0; uint32_t ctf = 0;
        for (int s = 0; s < c->nseg; s++) if (cu[s].i < cu[s].x.t.n) {
            uint32_t l; const char *k = cs_lex_key(&cu[s].x, cu[s].i, &l);
            if (st_cmp_bytes(k, l, mk, ml)) continue;
            const uint8_t *r = store_table_at(&cu[s].x.t, cu[s].i); uint32_t df = st_get_u32(r + 8), poff = st_get_u32(r + 16), plen = st_get_u32(r + 20);
            cu[s].i++;
            if ((uint64_t)poff + plen > cu[s].sp->size) continue;
            if (ne + df > ecap) { ecap = (ne + df) * 2 + 16; ent = (CsHit *)realloc(ent, ecap * sizeof *ent); }
            int64_t got = cs_decode_post(cu[s].sp->data + poff, cu[s].sp->data + poff + plen, df, ent + ne);
            if (got < 0) continue;
            size_t base = ne;                                          /* lidos de base+q, gravados em ne (ne <= base+q) */
            for (int64_t q = 0; q < got; q++) {
                uint32_t od = ent[base + q].doc, nd = (c->docmap[s] && od < ndocs_seg[s]) ? c->docmap[s][od] : UINT32_MAX;
                if (nd == UINT32_MAX) continue;
                ent[ne] = ent[base + q]; ent[ne].doc = nd; ne++;
            }
        }
        if (!ne) continue;
        qsort(ent, ne, sizeof *ent, cs_cmp_hit);
        uint32_t last = 0; size_t p0 = posts.n;
        for (size_t q = 0; q < ne; q++) { cs_post_put(&posts, &last, &ent[q]); for (int f = 0; f < CF_N; f++) ctf += ent[q].tf[f]; }
        sb_u32(&rec, (uint32_t)pool.n); sb_u32(&rec, ml); sb_u32(&rec, (uint32_t)ne); sb_u32(&rec, ctf); sb_u32(&rec, (uint32_t)p0); sb_u32(&rec, (uint32_t)(posts.n - p0));
        sb_bytes(&pool, mk, ml); nt++;
    }
    free(ent); free(cu);
    if (nt) { store_table_begin(out, nt, CS_LEXW); sb_bytes(out, rec.p, rec.n); sb_bytes(out, pool.p, pool.n); sb_bytes(&g_cs_ml, out->p, out->n); g_cs_mp = posts; memset(&posts, 0, sizeof posts); g_cs_mn = nt; }
    sb_free(&rec); sb_free(&pool); sb_free(&posts);
    *nrec = nt; return 0;
}
static int cs_merge_posts(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) {
    (void)c; *nrec = 0; if (!g_cs_mp.n) return 0;
    *out = g_cs_mp; memset(&g_cs_mp, 0, sizeof g_cs_mp); *nrec = g_cs_mn; return 0;
}
static int cs_merge_lens(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) {
    uint32_t n = 0;
    for (int s = 0; s < c->nseg; s++) { const StoreSection *sd = store_seg_section(c->seg[s], STS_DOCS); uint32_t cnt = sd ? sd->nrec : 0;
        for (uint32_t i = 0; i < cnt; i++) if (c->docmap[s] && c->docmap[s][i] != UINT32_MAX) n++; }
    *nrec = 0; if (!g_cs_mn || !n) return 0;
    uint32_t *v = (uint32_t *)calloc((size_t)n * 3, sizeof *v);
    for (int s = 0; s < c->nseg; s++) {
        const StoreSection *sd = store_seg_section(c->seg[s], STS_DOCS), *sn = store_seg_section(c->seg[s], CS_T_LENS); StoreTable lt;
        if (!sd || !sn || !store_table_open(sn, &lt) || lt.w != CS_LENW) continue;
        for (uint32_t i = 0; i < sd->nrec && i < lt.n; i++) { uint32_t nd = c->docmap[s][i]; if (nd == UINT32_MAX || nd >= n) continue;
            for (int f = 0; f < 3; f++) v[(size_t)nd * 3 + f] = st_get_u32(store_table_at(&lt, i) + 4 * f); }
    }
    store_table_begin(out, n, CS_LENW); for (size_t i = 0; i < (size_t)n * 3; i++) sb_u32(out, v[i]);
    free(v); *nrec = n; return 0;
}
static int cs_merge_tri(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) {
    (void)c; *nrec = 0; if (!g_cs_ml.n) return 0;
    CsLex x; StoreSection s; memset(&s, 0, sizeof s); s.data = g_cs_ml.p; s.size = g_cs_ml.n;
    if (!cs_lex_open(&s, &x)) return -1;
    cs_tri_build(out, &x, nrec); sb_free(&g_cs_ml); return 0;
}
static void coder_search_register(void) {
    store_register_merger(CS_T_TERMS, 1, cs_merge_terms); store_register_merger(CS_T_POSTS, 1, cs_merge_posts);
    store_register_merger(CS_T_LENS, 1, cs_merge_lens); store_register_merger(CS_T_TRI, 1, cs_merge_tri);
}

/* =====================================================================================================================================
 * CONSULTA: katana coder search "pergunta" [pasta] [--limit N] [--json] [--why]   e   katana coder find-name <nome> [pasta]
 * Canais (cada um produz uma lista ordenada de arquivos, com a evidencia que o fez entrar):
 *   exact       nome de arquivo ou identificador inteiro citado na pergunta (indice: chave 'n' do identificador inteiro; tambem o texto do nome do arquivo)
 *   subword     o termo aparece como PARTE de um identificador (campo 3) ou do nome do arquivo (listSessions casa com sessions e list)
 *   bm25        BM25 por campos (caminho, nome do arquivo, conteudo) sobre os termos da pergunta
 *   trigram     termos do lexico parecidos com termo da pergunta que nao existe no indice (typos, nomes parciais), via trigramas do lexico
 *   literal     frase entre aspas, verificada relendo os arquivos candidatos
 *   concept     termos relacionados pela base de conhecimento (outro idioma/estilo, verbos de operacao, abreviacoes), com peso menor
 * Fusao: camadas (1 ancoras exatas e literais verificados; 2 par conceito + operacao no nome/caminho; 3 o resto); dentro de cada camada,
 * fusao de postos (RRF, constante 60).
 * Parametros fixos: BM25 k1=1.2, b=0.75, pesos caminho/nome/corpo 2.0/4.0/1.0; pesos dos canais 1,1,1,1,1,0.6; escala dos termos conceituais 0.35;
 * KATANA_CODER_INDEX_KB (128). */
enum { CH_EXACT, CH_SUB, CH_BM25, CH_TRI, CH_LIT, CH_CON, CH_N };
static const char *const g_cs_chname[CH_N] = {"exact", "subword", "bm25", "trigram", "literal", "conceptual"};
enum { QC_ORIG, QC_WHOLE, QC_ABBR, QC_FUZZY, QC_CONCEPT, QC_OP, QC_ABBRREV };
static const char *const g_cs_clsname[] = {"original", "identifier", "abbreviation", "fuzzy", "concept", "operation", "abbreviated-as"};
static double g_cs_k1 = 1.2, g_cs_b = 0.75, g_cs_wf[3] = {2.0, 4.0, 1.0};
static double g_cs_rrfk = 60, g_cs_chw[CH_N] = {1, 1, 1, 1, 1, 0.6}, g_cs_cscale = 0.35;

/* ---- visao de leitura: segmentos, documentos vigentes (numeracao global), tamanhos ---- */
typedef struct { const StoreSeg *seg; CsLex lex; const StoreSection *sp; StoreTable lens, tri; const uint8_t *tpool, *tend; StoreDocs docs; uint32_t ndocs; uint32_t *gmap; int has; } CsSeg;
typedef struct { StoreReader *r; CsSeg *s; int ns; uint32_t N; const char **path; uint32_t *plen; uint32_t *lens; double avg[3]; char dir[4400]; int noindex; int borrowed; void *slot; uint32_t strip; } CsView;
static int cs_view_open_raw(const char *proj, CsView *v, char *err, size_t ecap) {
    memset(v, 0, sizeof *v);
    IndexLoc loc; index_resolve(proj, 0, &loc); snprintf(v->dir, sizeof v->dir, "%s", loc.store);
    if (store_open(v->dir, &v->r, err, ecap) != 0) return -1;
    StoreReader *r = v->r;
    if (r->nseg == 0) { snprintf(err, ecap, "sem indice em %s (rode: katana coder index %s)", v->dir, proj); return -1; }
    StoreLive L; const StoreSeg **segs = (const StoreSeg **)malloc(sizeof *segs * (size_t)r->nseg);
    for (int i = 0; i < r->nseg; i++) segs[i] = &r->seg[i];
    store_live_build(&L, segs, r->nseg); free(segs);
    ign_hide_for_view(&loc, &L);                  /* pastas de inclusao restrita que esta consulta nao pediu (maint.c) */
    /* a raiz usada para ler arquivos e a localizada agora (nao a gravada no manifesto: a pasta pode ter sido movida); com prefixo, a consulta
       fica restrita a ele e os caminhos saem relativos a ele */
    if (loc.prefix[0]) { store_live_rebase(&L, loc.prefix, strlen(loc.prefix)); v->strip = (uint32_t)strlen(loc.prefix) + 1; snprintf(r->man.root, sizeof r->man.root, "%s/%s", loc.root, loc.prefix); }
    else snprintf(r->man.root, sizeof r->man.root, "%s", loc.root);
    v->ns = r->nseg; v->s = (CsSeg *)calloc((size_t)v->ns, sizeof(CsSeg));
    for (int s = 0; s < v->ns; s++) {
        CsSeg *c = &v->s[s]; c->seg = &r->seg[s];
        store_docs_open(store_seg_section(c->seg, STS_DOCS), &c->docs); c->ndocs = c->docs.t.n;
        c->gmap = (uint32_t *)malloc(sizeof(uint32_t) * (c->ndocs + 1)); for (uint32_t i = 0; i < c->ndocs; i++) c->gmap[i] = UINT32_MAX;
        const StoreSection *sl = store_seg_section(c->seg, CS_T_TERMS), *sn = store_seg_section(c->seg, CS_T_LENS), *st = store_seg_section(c->seg, CS_T_TRI);
        c->sp = store_seg_section(c->seg, CS_T_POSTS);
        if (sl && c->sp && sn && st && cs_lex_open(sl, &c->lex) && store_table_open(sn, &c->lens) && c->lens.w == CS_LENW && store_table_open(st, &c->tri) && c->tri.w == CS_TRIW) {
            c->has = 1; uint64_t head = 8 + (uint64_t)c->tri.n * c->tri.w; c->tpool = st->data + head; c->tend = st->data + st->size;
        } else if (c->ndocs && sdf_lv(st_get_u32(store_table_at(&c->docs.t, 0) + 36)) != 1) v->noindex++;      /* segmento so de catalogo (N0 do rastreador) nao e defeito */
    }
    for (int i = 0; i < L.n; i++) if (!L.e[i].dead) v->s[L.e[i].seg].gmap[L.e[i].idx] = UINT32_MAX - 1;
    uint32_t N = 0;
    for (int s = 0; s < v->ns; s++) for (uint32_t i = 0; i < v->s[s].ndocs; i++) if (v->s[s].gmap[i] == UINT32_MAX - 1) v->s[s].gmap[i] = N++;
    v->N = N;
    v->path = (const char **)malloc(sizeof(char *) * (N + 1)); v->plen = (uint32_t *)malloc(sizeof(uint32_t) * (N + 1)); v->lens = (uint32_t *)calloc((size_t)N * 3 + 3, sizeof(uint32_t));
    double sum[3] = {0, 0, 0};
    for (int s = 0; s < v->ns; s++) { CsSeg *c = &v->s[s];
        for (uint32_t i = 0; i < c->ndocs; i++) { uint32_t g = c->gmap[i]; if (g == UINT32_MAX) continue;
            StoreDoc d; store_docs_get(&c->docs, i, &d); v->path[g] = d.path + v->strip; v->plen[g] = d.plen - v->strip;
            if (c->has && i < c->lens.n) for (int f = 0; f < 3; f++) { uint32_t x = st_get_u32(store_table_at(&c->lens, i) + 4 * f); v->lens[(size_t)g * 3 + f] = x; sum[f] += x; } } }
    for (int f = 0; f < 3; f++) { v->avg[f] = N ? sum[f] / N : 1; if (v->avg[f] < 1) v->avg[f] = 1; }
    store_live_free(&L);
    statemap_get(v, loc.prefix);
    return 0;
}
static void cs_view_free_real(CsView *v) {
    for (int s = 0; s < v->ns; s++) free(v->s[s].gmap);
    free(v->s); free(v->path); free(v->plen); free(v->lens); if (v->r) store_close(v->r); memset(v, 0, sizeof *v);
}
/* visao reaproveitada (indexctl.c): o cache guarda a visao inteira e entrega copias rasas `borrowed`; vale ate o MANIFEST mudar */
static void cs_cache_destroy(void *p) { cs_view_free_real((CsView *)p); free(p); }
static IdxCache g_cs_cache = {.destroy = cs_cache_destroy};
static void cs_view_close(CsView *v) {
    if (v->borrowed) { idx_cache_release(&g_cs_cache, v->slot); memset(v, 0, sizeof *v); return; }
    cs_view_free_real(v);
}
static int cs_view_open(const char *proj, CsView *v, char *err, size_t ecap) {
    char dir[4400]; idx_view_key(proj, dir, sizeof dir);
    void *h = idx_cache_get(&g_cs_cache, dir);
    if (h) { *v = *(CsView *)h; v->borrowed = 1; v->slot = h; return 0; }
    CsView t; if (cs_view_open_raw(proj, &t, err, ecap) != 0) return -1;
    CsView *hp = (CsView *)malloc(sizeof *hp);
    if (hp) { *hp = t; if (idx_cache_put(&g_cs_cache, dir, hp, t.r)) { *v = *hp; v->borrowed = 1; v->slot = hp; return 0; } free(hp); }
    *v = t; return 0;
}
/* postings vigentes de uma chave, com o documento ja em numeracao global (ordenados) */
static CsHit *cs_key_hits(const CsView *v, const char *key, size_t kl, uint32_t *n) {
    CsHit *out = NULL; uint32_t no = 0, cap = 0; *n = 0;
    for (int s = 0; s < v->ns; s++) { const CsSeg *c = &v->s[s]; if (!c->has) continue;
        int64_t ix = cs_lex_find(&c->lex, key, kl); if (ix < 0) continue;
        const uint8_t *r = store_table_at(&c->lex.t, (uint32_t)ix); uint32_t df = st_get_u32(r + 8), poff = st_get_u32(r + 16), plen = st_get_u32(r + 20);
        if ((uint64_t)poff + plen > c->sp->size || !df) continue;
        CsHit *tmp = (CsHit *)malloc(sizeof *tmp * df);
        if (cs_decode_post(c->sp->data + poff, c->sp->data + poff + plen, df, tmp) == (int64_t)df)
            for (uint32_t i = 0; i < df; i++) { if (tmp[i].doc >= c->ndocs) continue; uint32_t g = c->gmap[tmp[i].doc]; if (g >= UINT32_MAX - 1) continue;
                if (no == cap) { cap = cap ? cap * 2 : 64; out = (CsHit *)realloc(out, sizeof *out * cap); }
                out[no] = tmp[i]; out[no++].doc = g; }
        free(tmp);
    }
    *n = no; return out;
}
static CsHit *cs_hits_union(CsHit *a, uint32_t na, CsHit *b, uint32_t nb, uint32_t *n) {       /* consome a e b; tf = maximo por campo */
    if (!nb) { free(b); *n = na; return a; }
    if (!na) { free(a); *n = nb; return b; }
    CsHit *o = (CsHit *)malloc(sizeof *o * (na + nb)); uint32_t i = 0, j = 0, k = 0;
    while (i < na || j < nb) {
        if (j >= nb || (i < na && a[i].doc < b[j].doc)) o[k++] = a[i++];
        else if (i >= na || b[j].doc < a[i].doc) o[k++] = b[j++];
        else { o[k] = a[i]; for (int f = 0; f < CF_N; f++) if (b[j].tf[f] > o[k].tf[f]) o[k].tf[f] = b[j].tf[f]; k++; i++; j++; }
    }
    free(a); free(b); *n = k; return o;
}
static CsHit *cs_hits_and(CsHit *a, uint32_t na, CsHit *b, uint32_t nb, uint32_t *n) {          /* consome a e b; tf = soma */
    CsHit *o = (CsHit *)malloc(sizeof *o * ((na < nb ? na : nb) + 1)); uint32_t i = 0, j = 0, k = 0;
    while (i < na && j < nb) {
        if (a[i].doc < b[j].doc) i++; else if (b[j].doc < a[i].doc) j++;
        else { o[k] = a[i]; for (int f = 0; f < CF_N; f++) { uint32_t s = (uint32_t)o[k].tf[f] + b[j].tf[f]; o[k].tf[f] = s > 65535 ? 65535 : (uint16_t)s; } k++; i++; j++; }
    }
    free(a); free(b); *n = k; return o;
}

/* ---- termos da consulta ---- */
typedef struct { char k[3][96]; int nk; } QMem;           /* chaves alternativas: normalizada, original minuscula, normalizada no outro idioma */
typedef struct { char disp[64]; int cls, src, side; double w; int nm; QMem m[3]; CsHit *hit; uint32_t nhit; double idf; char why[220]; } QTerm;     /* side: 1 conceito, 2 operacao */
typedef struct { QTerm *t; int n, cap; } QSet;
static void cs_qmem(QMem *m, const char *norm, const char *orig) {
    m->nk = 0; if (!norm || !*norm) return;
    snprintf(m->k[0], sizeof m->k[0], "n%.90s", norm); m->nk = 1;
    if (orig && *orig && strcmp(orig, norm)) { snprintf(m->k[1], sizeof m->k[1], "o%.90s", orig); m->nk = 2; }
}
static void cs_qmem_alt(QMem *m, const char *alt) {
    if (!alt || !*alt || m->nk >= 3) return;
    char k[96]; snprintf(k, sizeof k, "n%.90s", alt);
    for (int i = 0; i < m->nk; i++) if (!strcmp(m->k[i], k)) return;
    snprintf(m->k[m->nk++], sizeof m->k[0], "%s", k);
}
static int cs_qt_same(const QTerm *a, const QTerm *b) {
    if (a->nm != b->nm) return 0;
    for (int i = 0; i < a->nm; i++) if (strcmp(a->m[i].k[0], b->m[i].k[0])) return 0;
    return 1;
}
static QTerm *cs_qt_add(QSet *q, const QTerm *t) {
    for (int i = 0; i < q->n; i++) if (cs_qt_same(&q->t[i], t)) { if (t->side) q->t[i].side |= t->side; return NULL; }
    if (q->n == q->cap) { q->cap = q->cap ? q->cap * 2 : 128; q->t = (QTerm *)realloc(q->t, sizeof(QTerm) * (size_t)q->cap); }
    q->t[q->n] = *t; return &q->t[q->n++];
}
static void cs_qt_load(const CsView *v, QTerm *t) {
    CsHit *acc = NULL; uint32_t na = 0;
    for (int m = 0; m < t->nm; m++) {
        CsHit *u = NULL; uint32_t nu = 0;
        for (int a = 0; a < t->m[m].nk; a++) { uint32_t n; CsHit *h = cs_key_hits(v, t->m[m].k[a], strlen(t->m[m].k[a]), &n); u = cs_hits_union(u, nu, h, n, &nu); }
        if (m == 0) { acc = u; na = nu; } else acc = cs_hits_and(acc, na, u, nu, &na);
    }
    t->hit = acc; t->nhit = na;
    t->idf = log(1.0 + ((double)v->N - na + 0.5) / (na + 0.5));
}
static const CsHit *cs_qt_find(const QTerm *t, uint32_t doc) {
    uint32_t lo = 0, hi = t->nhit;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (t->hit[m].doc < doc) lo = m + 1; else hi = m; }
    return lo < t->nhit && t->hit[lo].doc == doc ? &t->hit[lo] : NULL;
}

/* ---- candidatos por trigramas do lexico: parciais (substring) e typos (distancia de edicao) ---- */
typedef struct { char key[96]; int kl; double sim; uint32_t df; int substr; } CsCand;
static int cs_lev(const char *a, int al, const char *b, int bl) {
    if (al > 60) al = 60;
    if (bl > 60) bl = 60;
    int p[62], c[62]; for (int j = 0; j <= bl; j++) p[j] = j;
    for (int i = 1; i <= al; i++) { c[0] = i; for (int j = 1; j <= bl; j++) { int x = p[j - 1] + (a[i - 1] != b[j - 1]), y = p[j] + 1, z = c[j - 1] + 1; c[j] = x < y ? (x < z ? x : z) : (y < z ? y : z); } memcpy(p, c, sizeof(int) * (size_t)(bl + 1)); }
    return p[bl];
}
static int cs_cand_cmp(const void *a, const void *b) { const CsCand *x = (const CsCand *)a, *y = (const CsCand *)b; if (x->sim != y->sim) return x->sim < y->sim ? 1 : -1; return x->df < y->df ? 1 : x->df > y->df ? -1 : 0; }
static int cs_fuzzy(const CsView *v, const char *q, int ql, CsCand *out, int max) {
    if (ql < 3) return 0;
    uint32_t tri[64]; int nt = 0; int qq = ql > 62 ? 62 : ql;
    for (int j = 0; j + 3 <= qq; j++) { uint32_t t = cs_tri_of(q + j); int dup = 0; for (int k = 0; k < nt; k++) if (tri[k] == t) { dup = 1; break; } if (!dup) tri[nt++] = t; }
    int maxd = ql <= 6 ? 1 : ql <= 10 ? 2 : 3, need = (int)ceil(0.5 * nt); if (need < 2) need = nt < 2 ? nt : 2;
    CsCand *all = NULL; int na = 0, cap = 0;
    for (int s = 0; s < v->ns; s++) { const CsSeg *c = &v->s[s]; if (!c->has || !c->lex.t.n) continue;
        uint16_t *cnt = (uint16_t *)calloc(c->lex.t.n, sizeof *cnt); uint32_t *touched = (uint32_t *)malloc(sizeof(uint32_t) * (c->lex.t.n + 1)); uint32_t ntch = 0;
        for (int k = 0; k < nt; k++) {
            uint32_t lo = 0, hi = c->tri.n;
            while (lo < hi) { uint32_t m = (lo + hi) / 2; if (st_get_u32(store_table_at(&c->tri, m)) < tri[k]) lo = m + 1; else hi = m; }
            if (lo >= c->tri.n || st_get_u32(store_table_at(&c->tri, lo)) != tri[k]) continue;
            const uint8_t *r = store_table_at(&c->tri, lo), *p = c->tpool + st_get_u32(r + 4); uint32_t n = st_get_u32(r + 8), id = 0;
            for (uint32_t i = 0; i < n; i++) { uint64_t d; p = st_get_varint(p, c->tend, &d); if (!p) break; id += (uint32_t)d; if (id >= c->lex.t.n) break;
                if (cnt[id]++ == 0) touched[ntch++] = id; }
        }
        for (uint32_t i = 0; i < ntch; i++) {
            uint32_t id = touched[i]; if (cnt[id] < need) continue;
            uint32_t kl; const char *k = cs_lex_key(&c->lex, id, &kl); const char *w = k + 1; int wl = (int)kl - 1; if (wl < 3 || wl > 90) continue;
            if (wl == ql && !memcmp(w, q, (size_t)ql)) continue;
            CsCand cd; memset(&cd, 0, sizeof cd); memcpy(cd.key, k, kl); cd.kl = (int)kl; cd.df = cs_lex_df(&c->lex, id);
            if ((int)cnt[id] == nt && wl > ql && memmem(w, (size_t)wl, q, (size_t)ql)) { cd.substr = 1; cd.sim = 0.8 * ql / wl; }
            else { if (w[0] != q[0]) continue; int d = cs_lev(q, ql, w, wl); if (d > maxd || d == 0) continue;     /* typo: mesma letra inicial */ cd.sim = 1.0 - (double)d / (ql > wl ? ql : wl); }
            int dup = 0; for (int a = 0; a < na; a++) if (all[a].kl == cd.kl && !memcmp(all[a].key, cd.key, (size_t)cd.kl)) { dup = 1; if (all[a].sim < cd.sim) all[a] = cd; break; }
            if (dup) continue;
            if (na == cap) { cap = cap ? cap * 2 : 64; all = (CsCand *)realloc(all, sizeof *all * (size_t)cap); }
            all[na++] = cd;
        }
        free(cnt); free(touched);
    }
    qsort(all, (size_t)na, sizeof *all, cs_cand_cmp);
    int n = na < max ? na : max; if (n) memcpy(out, all, sizeof *out * (size_t)n);
    free(all); return n;
}

/* ---- arquivos: leitura limitada e utilitarios de texto ---- */
static uint32_t cs_read_limit(void) { const char *e = getenv("KATANA_CODER_INDEX_KB"); return (e && atoi(e) > 0 ? (uint32_t)atoi(e) : 128u) * 1024u; }
static char *cs_read_doc(const CsView *v, uint32_t g, size_t *len) {
    char full[8400]; snprintf(full, sizeof full, "%s/%.*s", v->r->man.root, (int)v->plen[g], v->path[g]);
    { char rl[4200]; snprintf(rl, sizeof rl, "%.*s", (int)v->plen[g], v->path[g]); fresh_check(v->r->man.root, rl, 0, 0); }
    size_t cap = cs_read_limit(); uint8_t *b = (uint8_t *)malloc(cap + 1); if (!b) return NULL;
    int64_t got = st_slurp(full, b, cap); if (got < 0) { free(b); return NULL; }
    b[got] = 0; *len = (size_t)got; return (char *)b;
}
static const char *cs_stristr(const char *h, const char *n) {
    size_t nl = strlen(n); if (!nl) return h;
    for (; *h; h++) { size_t i = 0; while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)n[i])) i++; if (i == nl) return h; }
    return NULL;
}
static void cs_compact(const char *s, size_t l, char *o, size_t cap) { size_t k = 0; for (size_t i = 0; i < l && k + 1 < cap; i++) if (isalnum((unsigned char)s[i])) o[k++] = (char)tolower((unsigned char)s[i]); o[k] = 0; }
static void cs_jstr(const char *s, size_t n) {
    putchar('"');
    for (size_t i = 0; i < n && s[i]; i++) { const unsigned char c = (unsigned char)s[i]; if (c == '"' || c == '\\') printf("\\%c", c); else if (c < 0x20) printf("\\u%04x", c); else putchar(c); }
    putchar('"');
}

/* ---- evidencia: linhas (calculadas ao imprimir, relendo o arquivo) ---- */
typedef struct { int line; char text[150]; uint32_t mask; } CsEvLine;
#define CS_EVBITS 24
/* mascara de termos que casam em um trecho de texto (mesma regra por token das linhas de evidencia) */
static uint32_t cs_text_mask(const char *text, const QSet *q, const int *ti, int nti, CoderTok *tk) {
    uint32_t m = 0; int n = coder_tokens_ex(text, tk, 4096, 2, CT_AUTO);
    for (int a = 0; a < n; a++) { char lo[64]; cs_lower(lo, tk[a].orig, sizeof lo);
        for (int j2 = 0; j2 < nti; j2++) { if (m >> j2 & 1) continue; const QTerm *t = &q->t[ti[j2]]; int hit = 0; if (t->nm > 1) continue;     /* termos de varias partes (E) nao marcam linhas */
            for (int mm = 0; mm < t->nm && !hit; mm++) for (int k = 0; k < t->m[mm].nk; k++) { const char *key = t->m[mm].k[k];
                if ((key[0] == 'n' && !strcmp(tk[a].norm, key + 1)) || (key[0] == 'o' && !strcmp(lo, key + 1))) { hit = 1; break; } }
            if (hit) m |= 1u << j2; } }
    return m;
}
/* le o arquivo e devolve a mascara de termos por linha (1-based em lm[linha-1]); *buf/*len = texto lido (o chamador libera); first[b] = primeira linha do bit b */
static uint32_t *cs_linemasks(const CsView *v, uint32_t doc, const QSet *q, const int *ti, int nti, char phr[][256], uint32_t phrmask, CoderTok *tk, char **bufout, size_t *lenout, int *nlines, int *first) {
    size_t len; char *buf = cs_read_doc(v, doc, &len); *bufout = buf; *lenout = 0; *nlines = 0; if (!buf) return NULL;
    if (nti > CS_EVBITS) nti = CS_EVBITS;
    uint32_t *lm = NULL; int cap = 0, lineno = 1; for (int i = 0; i < 32; i++) first[i] = 0;
    static char line[4100];
    for (size_t i = 0; i <= len;) {
        size_t j = i; while (j < len && buf[j] != '\n' && j - i < 4000) j++;
        size_t l = j - i; memcpy(line, buf + i, l); line[l] = 0;
        for (size_t z = 0; z < l; z++) if (line[z] == 0) line[z] = ' ';
        while (lineno > cap) { int oc = cap; cap = cap ? cap * 2 : 256; lm = (uint32_t *)realloc(lm, sizeof *lm * (size_t)cap); for (int z = oc; z < cap; z++) lm[z] = 0; }
        uint32_t m = 0;
        if (l >= 2 && nti) m = cs_text_mask(line, q, ti, nti, tk);
        for (int p = 0; p < 8; p++) if ((phrmask >> p & 1) && cs_stristr(line, phr[p])) m |= 1u << (CS_EVBITS + p);
        lm[lineno - 1] |= m;
        for (int b = 0; b < 32; b++) if ((m >> b & 1) && !first[b]) first[b] = lineno;
        if (j < len && buf[j] == '\n') { lineno++; j++; } else if (j >= len) break;
        i = j;
    }
    *lenout = len; *nlines = lineno; return lm;
}
static void cs_line_text(const char *buf, size_t len, int ln, char *out, size_t cap) {
    size_t i = 0; int cur = 1; out[0] = 0; while (cur < ln && i < len) { if (buf[i] == '\n') cur++; i++; }
    while (i < len && (buf[i] == ' ' || buf[i] == '\t')) i++;
    size_t k2 = 0; while (i < len && buf[i] != '\n' && buf[i] != '\r' && k2 + 1 < cap) { out[k2++] = buf[i] == '\t' ? ' ' : buf[i]; i++; }
    out[k2] = 0;
}
static int cs_evidence(const CsView *v, uint32_t doc, const QSet *q, const int *ti, int nti, char phr[][256], uint32_t phrmask, CsEvLine *out, int maxout, CoderTok *tk) {
    char *buf; size_t len; int nl, first[32]; uint32_t *lm = cs_linemasks(v, doc, q, ti, nti, phr, phrmask, tk, &buf, &len, &nl, first); if (!buf) return 0;
    int chosen[40], nc = 0;
    for (int b = 0; b < 32 && nc < maxout; b++) { if (!first[b]) continue; int dup = 0; for (int k = 0; k < nc; k++) if (chosen[k] == first[b]) dup = 1; if (!dup) chosen[nc++] = first[b]; }
    for (int a = 1; a < nc; a++) { int x = chosen[a], b = a - 1; while (b >= 0 && chosen[b] > x) { chosen[b + 1] = chosen[b]; b--; } chosen[b + 1] = x; }
    int no = 0;
    for (int k = 0; k < nc; k++) { int ln = chosen[k]; CsEvLine *e = &out[no++]; e->line = ln; e->mask = lm[ln - 1]; cs_line_text(buf, len, ln, e->text, sizeof e->text); }
    free(lm); free(buf); return no;
}

/* ---- analise da pergunta: termos originais, identificadores, abreviacoes, frases e ancoras ---- */
typedef struct { char w[128]; int codey; } CsAnchor;
typedef struct { CsAnchor a[32]; int na; char phr[8][256]; int nphr; } CsQuery;
static int cs_is_codey(const char *w) {
    for (const char *p = w; *p; p++) { if (strchr("._-/", *p) || isdigit((unsigned char)*p)) return 1; if (p > w && islower((unsigned char)p[-1]) && isupper((unsigned char)*p)) return 1; }
    return 0;
}
static void cs_parse_raw(const char *qs, CsQuery *Q, char *rest, size_t cap) {
    memset(Q, 0, sizeof *Q);
    size_t k = 0; int inq = 0; char cur[256]; size_t cl = 0;
    for (const char *p = qs; *p && k + 2 < cap; p++) {
        if (*p == '"') { if (inq) { cur[cl] = 0; if (cl >= 2 && Q->nphr < 8) snprintf(Q->phr[Q->nphr++], sizeof Q->phr[0], "%s", cur); inq = 0; cl = 0; } else inq = 1; rest[k++] = ' '; continue; }
        if (inq && cl + 1 < sizeof cur) cur[cl++] = *p;
        rest[k++] = *p;
    }
    rest[k] = 0;
    char buf[1024]; snprintf(buf, sizeof buf, "%s", rest);
    for (char *w = strtok(buf, " \t\r\n"); w; w = strtok(NULL, " \t\r\n")) {
        int bt = 0; while (*w && strchr("\",;:()[]{}!?'`<>", *w)) { if (*w == '`') bt = 1; w++; }
        size_t l = strlen(w); while (l && (strchr("\",;:()[]{}!?'`<>.", w[l - 1]))) { if (w[l - 1] == '`') bt = 1; w[--l] = 0; }
        if (l < 3 || Q->na >= 32) continue;
        CsAnchor *a = &Q->a[Q->na++]; cs_lower(a->w, w, sizeof a->w); a->codey = bt || cs_is_codey(w);
    }
}
static int cs_kb_find(const CoderKb *kb, const char *w, const char *nm, int *cid, int *vid, int nmax, char *used) {
    char cand[10][64]; int nc = 0; snprintf(cand[nc++], 64, "%s", w);
    if (strcmp(nm, w)) snprintf(cand[nc++], 64, "%s", nm);
    for (int lg = KB_PT; lg <= KB_EN; lg++) { char st[4][64]; int n = coder_kb_stems(kb, lg, w, st, 4); for (int i = 0; i < n && nc < 10; i++) snprintf(cand[nc++], 64, "%s", st[i]); }
    for (int i = 0; i < nc; i++) {
        int a = coder_kb_term_concepts(kb, cand[i], cid, nmax), b = coder_kb_verb_ops(kb, cand[i], vid, nmax);
        if (a || b) { snprintf(used, 64, "%s", cand[i]); return (a << 8) | b; }
    }
    return 0;
}
/* acrescenta termos a partir de um texto da base (palavra, frase ou identificador) */
static void cs_add_kbterm(QSet *q, const char *text, int lang, int cls, int side, int src, double w, const char *why) {
    if (w <= 0.01) return;
    CoderTok tk[24]; int n = coder_tokens_ex(text, tk, 24, 0, lang == KB_PT ? CT_L_PT : CT_L_EN);
    char parts[4][64], porig[4][64]; int np = 0; int has_ws = strpbrk(text, " _.-") != NULL;
    for (int i = 0; i < n; i++) { if (tk[i].kind != 0 || tk[i].stop) continue; if (np < 3) { snprintf(parts[np], 64, "%s", tk[i].norm); cs_lower(porig[np], tk[i].orig, 64); np++; } }
    if (!np) return;
    QTerm t; memset(&t, 0, sizeof t); t.cls = cls; t.side = side; t.src = src; t.w = w;
    snprintf(t.why, sizeof t.why, "%s", why);
    if (np == 1 && !has_ws) { t.nm = 1; cs_qmem(&t.m[0], parts[0], porig[0]); snprintf(t.disp, sizeof t.disp, "%s", text); cs_qt_add(q, &t); return; }
    char sn[96]; size_t sl = 0;                                   /* identificador inteiro: partes minusculas com _ (como o indice grava) */
    CoderTok all[24]; int na = coder_tokens_ex(text, all, 24, 0, CT_L_EN);
    for (int i = 0; i < na; i++) if (all[i].kind == 0 && !all[i].stop) { if (sl) sn[sl++] = '_'; char lo[48]; cs_lower(lo, all[i].orig, sizeof lo); for (char *c = lo; *c && sl < 90; c++) sn[sl++] = *c; }
    sn[sl] = 0;
    if (np > 1) {
        t.nm = 1; cs_qmem(&t.m[0], sn, NULL); snprintf(t.disp, sizeof t.disp, "%s", sn); if (sl) cs_qt_add(q, &t);
        QTerm u; memset(&u, 0, sizeof u); u.cls = cls; u.side = side; u.src = src; u.w = w * 0.6; u.nm = np; snprintf(u.why, sizeof u.why, "%s (all parts)", why);
        for (int i = 0; i < np; i++) cs_qmem(&u.m[i], parts[i], porig[i]);
        snprintf(u.disp, sizeof u.disp, "%s+%s%s", parts[0], parts[1], np > 2 ? "+.." : ""); cs_qt_add(q, &u);
    } else { t.nm = 1; cs_qmem(&t.m[0], parts[0], porig[0]); snprintf(t.disp, sizeof t.disp, "%s", parts[0]); cs_qt_add(q, &t); }
}
typedef struct { int idx; double w; } CsWI;
static int cs_wi_cmp(const void *a, const void *b) { double x = ((const CsWI *)a)->w, y = ((const CsWI *)b)->w; return x < y ? 1 : x > y ? -1 : 0; }
/* expansao conceitual de uma palavra da consulta (termo original ja inserido em q->t[oi]) */
static void cs_expand_word(const CoderKb *kb, QSet *q, int oi, const CoderTok *tk, int src) {
    char w[64], used[64]; coder_kb_norm(tk->orig, w, sizeof w);
    int cid[8], vid[8]; int r = cs_kb_find(kb, w, tk->norm, cid, vid, 8, used);
    int nc = r >> 8, nv = r & 255;
    if (nc) q->t[oi].side |= 1;
    if (nv) q->t[oi].side |= 2;
    for (int i = 0; i < nc; i++) {                                        /* conceitos que contem a palavra: os outros termos deles */
        const KbTerm *ct = &kb->term[cid[i]]; const KbConcept *c = &kb->con[ct->con]; if (c->dead) continue;
        CsWI order[64]; int no = 0;
        for (int k = 0; k < c->nt && no < 64; k++) { const KbTerm *t2 = &kb->term[c->t0 + k]; if (t2 == ct) continue; char a[128]; coder_kb_norm(t2->term, a, sizeof a); if (!strcmp(a, w) || !strcmp(a, used)) continue; order[no].idx = c->t0 + k; order[no++].w = ct->w * t2->w; }
        qsort(order, (size_t)no, sizeof *order, cs_wi_cmp);
        for (int k = 0; k < no && k < 6; k++) { const KbTerm *t2 = &kb->term[order[k].idx]; char why[220];
            snprintf(why, sizeof why, "word \"%s\" -> concept %s (%s %.2f) -> %s \"%s\" (%.2f)", tk->orig, c->id, g_kb_langname[ct->lang], ct->w, g_kb_langname[t2->lang], t2->term, t2->w);
            cs_add_kbterm(q, t2->term, t2->lang, QC_CONCEPT, 1, src, g_cs_cscale * order[k].w, why); }
    }
    for (int i = 0; i < nv; i++) {                                        /* operacoes que tem a palavra como verbo: os outros verbos delas */
        const KbVerb *vb = &kb->verb[vid[i]]; const KbOp *op = &kb->op[vb->op]; if (op->dead) continue; int added = 0;
        for (int pass = 0; pass < 2; pass++) for (int k = 0; k < op->nv && added < 5; k++) { const KbVerb *v2 = &kb->verb[op->v0 + k]; char a[128]; coder_kb_norm(v2->w, a, sizeof a);
            if (!strcmp(a, w) || !strcmp(a, used) || strchr(a, ' ')) continue; if ((v2->lang == KB_PT) != (pass == 1)) continue;
            char why[220]; snprintf(why, sizeof why, "word \"%s\" -> operation %s (verb %s) -> %s verb \"%s\"", tk->orig, op->name, g_kb_langname[vb->lang], g_kb_langname[v2->lang], v2->w);
            cs_add_kbterm(q, v2->w, v2->lang, QC_OP, 2, src, g_cs_cscale * 0.3, why); added++; }
    }
    int ab[4]; int na = coder_kb_abbr(kb, w, ab, 4);                       /* abreviacao -> expansoes */
    for (int i = 0; i < na; i++) { const KbAbbr *a = &kb->abbr[ab[i]]; for (int j = 0; j < a->ne && j < 3; j++) { char why[220]; snprintf(why, sizeof why, "abbreviation \"%s\" expands to \"%s\"", a->abbr, kb->exp[a->e0 + j]); cs_add_kbterm(q, kb->exp[a->e0 + j], KB_EN, QC_ABBR, 0, src, g_cs_cscale * 1.2, why); } }
    na = coder_kb_abbr_rev(kb, w, ab, 4);                                 /* palavra -> abreviacao usada no codigo */
    for (int i = 0; i < na; i++) { char why[220]; snprintf(why, sizeof why, "\"%s\" is abbreviated in code as \"%s\"", w, kb->abbr[ab[i]].abbr); cs_add_kbterm(q, kb->abbr[ab[i]].abbr, KB_CODE, QC_ABBRREV, 0, src, g_cs_cscale * 1.2, why); }
}

/* ---- pontuacao por campos (BM25F): cada campo normaliza o seu tf pelo tamanho do campo no documento ---- */
static double cs_bm25(const CsView *v, const QTerm *t, const CsHit *h) {
    double nr[3]; const uint32_t *L = v->lens + (size_t)h->doc * 3;
    for (int f = 0; f < 3; f++) nr[f] = 1.0 - g_cs_b + g_cs_b * L[f] / v->avg[f];
    double tfw = g_cs_wf[0] * h->tf[CF_PATH] / nr[0] + g_cs_wf[1] * h->tf[CF_NAME] / nr[1] + g_cs_wf[2] * (h->tf[CF_WORD] + h->tf[CF_PART]) / nr[2];
    return t->w * t->idf * tfw * (g_cs_k1 + 1.0) / (g_cs_k1 + tfw);
}
static const double *g_cs_sortsc;
static int cs_cmp_sc(const void *a, const void *b) { double x = g_cs_sortsc[*(const uint32_t *)a], y = g_cs_sortsc[*(const uint32_t *)b]; if (x != y) return x < y ? 1 : -1; uint32_t p = *(const uint32_t *)a, q = *(const uint32_t *)b; return p < q ? -1 : p > q; }
/* lista ordenada (maximo top) dos documentos com pontuacao > 0; rk[doc] = posto (1 = melhor), 0 = fora */
static int cs_rank(const double *sc, uint32_t N, uint16_t *rk, int top) {
    uint32_t *d = (uint32_t *)malloc(sizeof *d * (N + 1)); uint32_t n = 0;
    for (uint32_t i = 0; i < N; i++) if (sc[i] > 0) d[n++] = i;
    g_cs_sortsc = sc; qsort(d, n, sizeof *d, cs_cmp_sc);
    int k = (int)n < top ? (int)n : top;
    for (int i = 0; i < k; i++) rk[d[i]] = (uint16_t)(i + 1);
    free(d); return (int)n;
}
typedef struct { uint32_t doc; int tier; double fused; } CsRes;
static int cs_cmp_res(const void *a, const void *b) {
    const CsRes *x = (const CsRes *)a, *y = (const CsRes *)b;
    if (x->tier != y->tier) return x->tier < y->tier ? -1 : 1;
    if (x->fused != y->fused) return x->fused < y->fused ? 1 : -1;
    return x->doc < y->doc ? -1 : x->doc > y->doc;
}
static const char *cs_basename(const char *p, uint32_t l, uint32_t *bl) { uint32_t i = l; while (i && p[i - 1] != '/') i--; *bl = l - i; return p + i; }
static void cs_fields_str(const CsHit *h, char *o, size_t cap) {
    o[0] = 0; static const char *const fn[CF_N] = {"path", "name", "word", "identifier-part"};
    for (int f = 0; f < CF_N; f++) if (h->tf[f]) { size_t l = strlen(o); snprintf(o + l, cap - l, "%s%s x%u", l ? ", " : "", fn[f], h->tf[f]); }
}
typedef struct { int i; double w; } CsTW;
static int cs_tw_cmp(const void *a, const void *b) { double x = ((const CsTW *)a)->w, y = ((const CsTW *)b)->w; return x < y ? 1 : x > y ? -1 : 0; }

/* imprime o bloco de evidencia de um resultado (texto); devolve nada */
static void cs_print_evidence_text(const CsView *v, uint32_t doc, const QSet *q, const CsQuery *Q, uint32_t litmask, CoderTok *tk, int why, const char *const *chans, int nch) {
    CsTW tw[256]; int nm = 0;
    for (int i = 0; i < q->n && nm < 256; i++) if (cs_qt_find(&q->t[i], doc)) { tw[nm].i = i; tw[nm++].w = q->t[i].w * q->t[i].idf; }
    qsort(tw, (size_t)nm, sizeof *tw, cs_tw_cmp);
    int ti[CS_EVBITS]; int nti = nm < CS_EVBITS ? nm : CS_EVBITS; for (int i = 0; i < nti; i++) ti[i] = tw[i].i;
    CsEvLine ev[40]; int ne = cs_evidence(v, doc, q, ti, nti, (char (*)[256])Q->phr, litmask, ev, 5, tk);
    for (int i = 0; i < ne; i++) {
        printf("      line %d: %s", ev[i].line, ev[i].text);
        char tl[200]; tl[0] = 0;
        for (int b = 0; b < CS_EVBITS + 8; b++) if (ev[i].mask >> b & 1) { size_t l = strlen(tl); if (l > 150) break; if (b < CS_EVBITS) snprintf(tl + l, sizeof tl - l, "%s%s", l ? ", " : "", q->t[ti[b]].disp); else snprintf(tl + l, sizeof tl - l, "%s\"%s\"", l ? ", " : "", Q->phr[b - CS_EVBITS]); }
        printf("   <- %s\n", tl);
    }
    char lst[512]; lst[0] = 0;
    for (int k = 0; k < nm && k < 8; k++) { const QTerm *t = &q->t[tw[k].i]; const CsHit *h = cs_qt_find(t, doc); char fl[160]; cs_fields_str(h, fl, sizeof fl);
        size_t l = strlen(lst); if (l > 420) break; snprintf(lst + l, sizeof lst - l, "%s%s [%s: %s]", l ? "; " : "", t->disp, g_cs_clsname[t->cls], fl); }
    printf("      terms: %s%s\n", lst, nm > 8 ? " ..." : "");
    (void)why; (void)chans; (void)nch;
}
/* ---- UNIDADE POR SIMBOLO: o resultado de um arquivo com simbolos indexados vira uma lista de simbolos (symindex.c) ----
 * O termo casa no NOME do simbolo (peso 4), na ASSINATURA (2) ou no CORPO, pelo intervalo de linhas do simbolo (1 + log da contagem de linhas);
 * cada linha com casamento e atribuida ao simbolo mais interno que a contem; linhas fora de qualquer simbolo ficam num resultado de nivel de arquivo.
 * Arquivos sem simbolos (texto, docs, config) continuam como resultado de arquivo. A confianca vem da camada do arquivo (tier 1: 90, 2: 65, 3: 40),
 * +6 se o termo esta no nome do simbolo, +3 na assinatura, -8 se so no corpo. */
typedef struct { int sym, l0, l1, kind, nlines; uint32_t nmask, smask, bmask; double score; int ev[3]; char name[100], sig[160]; } CsUnit;
static int cs_unit_cmp(const void *a, const void *b) { double x = ((const CsUnit *)a)->score, y = ((const CsUnit *)b)->score; return x < y ? 1 : x > y ? -1 : ((const CsUnit *)a)->l0 - ((const CsUnit *)b)->l0; }
static int cs_tier_conf(int tier) { return tier == 1 ? 90 : tier == 2 ? 65 : 40; }
/* devolve o numero de unidades (ate max) ou -1 se o arquivo nao tem simbolos indexados; *flines = linhas com casamento fora de simbolos (ate 3) */
static int cs_symbol_units(SyView *sv, const char *path, uint32_t plen, const uint32_t *lm, int nlm, const char *buf, size_t blen, const QSet *q, const int *ti, int nti, CoderTok *tk, CsUnit *out, int max, int *flines, int *nflines) {
    *nflines = 0; if (!sv->any) return -1;
    StoreLiveDoc *e = store_live_find(&sv->L, path, plen); if (!e || e->dead) return -1;
    const SySec *x = &sv->s[e->seg].x; if (!x->has || e->idx >= x->docs.n) return -1;
    const uint8_t *dr = store_table_at(&x->docs, e->idx); uint32_t s0 = st_get_u32(dr), n = st_get_u32(dr + 4); if (!n) return -1;
    CsUnit *u = (CsUnit *)calloc(n, sizeof *u);
    uint32_t allow = 0; for (int b = 0; b < nti && b < CS_EVBITS; b++) { int cl = q->t[ti[b]].cls; if (cl == QC_ORIG || cl == QC_WHOLE || cl == QC_ABBR) allow |= 1u << b; }     /* so os termos do usuario (e abreviacoes), nao a expansao conceitual */
    for (uint32_t k = 0; k < n; k++) {
        SySym y; sy_sym_at(sv, e->seg, s0 + k, &y); CsUnit *c = &u[k]; c->sym = (int)k; c->l0 = (int)y.line0; c->l1 = (int)y.line1; c->kind = (int)y.kind;
        sy_cpy(c->name, sizeof c->name, y.name, y.nl); sy_cpy(c->sig, sizeof c->sig, y.sig, y.sl);
        c->nmask = cs_text_mask(c->name, q, ti, nti, tk); c->smask = c->sig[0] ? cs_text_mask(c->sig, q, ti, nti, tk) : 0; c->nmask &= allow; c->smask &= allow;
    }
    for (int ln = 1; ln <= nlm; ln++) {                         /* linha com casamento -> simbolo mais interno que a contem */
        uint32_t m = lm[ln - 1] & allow; if (!m) continue;
        int best = -1; for (uint32_t k = 0; k < n; k++) if (u[k].l0 <= ln && u[k].l1 >= ln && (best < 0 || u[k].l0 >= u[best].l0)) best = (int)k;
        if (best < 0) { if (*nflines < 3) flines[(*nflines)++] = ln; continue; }
        if (ln == u[best].l0) continue;                         /* a linha da declaracao ja vale como nome/assinatura */
        u[best].bmask |= m; u[best].nlines++; if (u[best].ev[2] == 0) { int z = 0; while (z < 3 && u[best].ev[z]) z++; if (z < 3) u[best].ev[z] = ln; }
    }
    int nu = 0;
    for (uint32_t k = 0; k < n; k++) {
        CsUnit *c = &u[k]; uint32_t all = c->nmask | c->smask | c->bmask; if (!all) continue;
        double sc = 0; for (int b = 0; b < nti && b < CS_EVBITS; b++) { if (!(all >> b & 1)) continue; double w = q->t[ti[b]].w * q->t[ti[b]].idf; sc += w * ((c->nmask >> b & 1) * 4.0 + (c->smask >> b & 1) * 2.0 + ((c->bmask >> b & 1) ? 1.0 + log(1.0 + c->nlines) : 0)); }
        c->score = sc; u[nu++] = *c;
    }
    qsort(u, (size_t)nu, sizeof *u, cs_unit_cmp);
    if (nu > max) nu = max;
    memcpy(out, u, sizeof *out * (size_t)nu); free(u); (void)buf; (void)blen; return nu;
}
static void cs_unit_conf(const CsUnit *c, int tier, int *conf, char *how, size_t cap) {
    int cf = cs_tier_conf(tier); const char *w = (c->nmask ? "name" : c->smask ? "signature" : "body");
    cf += c->nmask ? 6 : c->smask ? 3 : -8; *conf = cf < 1 ? 1 : cf > 99 ? 99 : cf;
    snprintf(how, cap, "tier %d; term in the symbol %s%s%s%s", tier, w, c->nmask && c->bmask ? " and body" : "", c->nlines ? "" : "", "");
}
static void cs_json_terms(const QSet *q) {
    printf("\"terms\":[");
    for (int i = 0; i < q->n; i++) { const QTerm *t = &q->t[i]; printf("%s{\"term\":", i ? "," : ""); cs_jstr(t->disp, 64); printf(",\"class\":\"%s\",\"weight\":%.3f,\"docs\":%u", g_cs_clsname[t->cls], t->w, t->nhit); if (t->why[0]) { printf(",\"why\":"); cs_jstr(t->why, 220); } printf("}"); }
    printf("]");
}

static int cs_search_core(const char *query, const char *proj, int limit, int json, int why, CoderHits *ho);
static int coder_search_main(int argc, char **argv) {
    const char *query = NULL, *proj = "."; int limit = 10, json = 0, why = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--why")) why = 1;
        else if (!query) query = argv[i]; else proj = argv[i];
    }
    if (!query || !*query) { fprintf(stderr, "uso: katana coder search \"pergunta\" [pasta] [--limit N] [--json] [--why]\n"); return 2; }
    if (limit < 1) limit = 10;
    return cs_search_core(query, proj, limit, json, why, NULL);
}
/* nucleo da busca. ho == NULL: imprime (katana coder search). ho != NULL: nao imprime, preenche os resultados estruturados (usado pelo Find);
 * ho->concept == 0 desliga a expansao pela base de conhecimento (canal conceitual) */
/* base de conhecimento: lida do disco uma vez por processo e reaproveitada (antes cada consulta relia todos os .kb: 10 a 14 ms de 11 a 15 ms).
 * So relida se a pasta de conhecimento do projeto ou a padrao mudarem (outra raiz, KATANA_KB_DIR). Nao liberada: vive ate o fim do processo. */
static const CoderKb *cs_kb_get(const char *kbproj) {
    static CoderKb kb; static char key[9000], proj[4300]; static int have;
    char k[9000]; snprintf(k, sizeof k, "%s|%s", coder_kb_default_dir(), kbproj ? kbproj : "");
    if (have && !strcmp(k, key)) { g_idx_n.kb_hit++; return kb.nfile > 0 ? &kb : NULL; }
    memset(&kb, 0, sizeof kb); kb.quiet = 1; kb.project = NULL;
    if (kbproj) { snprintf(proj, sizeof proj, "%s", kbproj); kb.project = proj; }
    coder_kb_load(coder_kb_default_dir(), &kb); g_idx_n.kb_load++;
    snprintf(key, sizeof key, "%s", k); have = 1;
    return kb.nfile > 0 ? &kb : NULL;
}
static int cs_search_core(const char *query, const char *proj, int limit, int json, int why, CoderHits *ho) {
    double t0 = now_sec(), t_open, t_kb, t_load, t_score;
    CsView v; char err[400];
    if (cs_view_open(proj, &v, err, sizeof err) != 0) { if (!ho) fprintf(stderr, "search: %s\n", err); return 1; }
    t_open = now_sec() - t0;
    IndexLoc kloc; index_resolve(proj, 0, &kloc);
    char kbproj[4300]; snprintf(kbproj, sizeof kbproj, "%s/.katana/coder", kloc.root);
    const CoderKb *kbc = cs_kb_get(kb_isdir(kbproj) ? kbproj : NULL); int kbok = kbc != NULL;
    t_kb = now_sec() - t0 - t_open;
    CsQuery Q; char rest[1100]; cs_parse_raw(query, &Q, rest, sizeof rest);
    CoderTok *tk = (CoderTok *)malloc(4096 * sizeof(CoderTok));
    int ntk = coder_tokens_ex(rest, tk, 4096, 0, CT_AUTO);
    QSet q = {0}; int norig = 0, nsrc = 0; int rest_len = (int)strlen(rest);
    for (int i = 0; i < ntk; i++) {
        const CoderTok *t = &tk[i]; if (t->stop || t->kind == 2) continue;
        QTerm x; memset(&x, 0, sizeof x); x.w = 1.0; x.nm = 1; char lo[64]; cs_lower(lo, t->orig, sizeof lo);
        if (t->kind == 1) { x.cls = QC_WHOLE; x.src = -1; cs_qmem(&x.m[0], t->norm, lo); snprintf(x.disp, sizeof x.disp, "%s", t->norm); cs_qt_add(&q, &x); continue; }
        x.cls = QC_ORIG; x.src = nsrc < 31 ? nsrc : 31; cs_qmem(&x.m[0], t->norm, lo);
        if (t->lang == CT_L_UNKNOWN) for (int lg = CT_L_PT; lg <= CT_L_EN; lg++) { char alt[64]; coder_norm_word(lo, lg, alt, sizeof alt); if (strcmp(alt, t->norm)) cs_qmem_alt(&x.m[0], alt); }   /* idioma incerto: tenta tambem a raiz no outro idioma */ snprintf(x.disp, sizeof x.disp, "%s", lo);
        int part = cs_is_part(rest, t->pos, (int)strlen(t->orig), rest_len);
        int before = q.n; QTerm *p = cs_qt_add(&q, &x);
        if (!p) continue;
        norig++; nsrc++;
        if (kbok && !part && (!ho || ho->concept)) cs_expand_word(kbc, &q, before, t, x.src);
    }
    int nuser = 0; for (int i = 0; i < q.n; i++) if (q.t[i].cls == QC_ORIG || q.t[i].cls == QC_WHOLE) nuser++;
    for (int i = 0; i < q.n; i++) cs_qt_load(&v, &q.t[i]);
    /* trigramas: so para o que a pergunta cita e nao existe no indice (typos, nomes parciais) */
    int nfz = 0;
    for (int i = 0; i < nuser; i++) {
        QTerm *t = &q.t[i]; if (t->nhit > 0 || (t->cls != QC_ORIG && t->cls != QC_WHOLE)) continue;
        CsCand cd[6]; const char *key = t->m[0].k[0] + 1; int n = cs_fuzzy(&v, key, (int)strlen(key), cd, 4);
        if (!n && t->m[0].nk > 1) { key = t->m[0].k[1] + 1; n = cs_fuzzy(&v, key, (int)strlen(key), cd, 4); }
        for (int k = 0; k < n; k++) {
            QTerm f; memset(&f, 0, sizeof f); f.cls = QC_FUZZY; f.src = t->src; f.nm = 1; f.w = 0.6 * cd[k].sim; snprintf(f.m[0].k[0], sizeof f.m[0].k[0], "%s", cd[k].key); f.m[0].nk = 1;
            snprintf(f.disp, sizeof f.disp, "%s", cd[k].key + 1);
            snprintf(f.why, sizeof f.why, "\"%s\" has no occurrence; lexicon term \"%s\" is %s (similarity %.2f, df %u)", t->disp, cd[k].key + 1, cd[k].substr ? "a partial match" : "a near spelling", cd[k].sim, cd[k].df);
            int idx = q.n; if (cs_qt_add(&q, &f)) { cs_qt_load(&v, &q.t[idx]); nfz++; }
        }
    }
    t_load = now_sec() - t0 - t_open - t_kb;
    uint32_t N = v.N;
    double *sc[CH_N]; uint16_t *rk[CH_N];
    for (int c = 0; c < CH_N; c++) { sc[c] = (double *)calloc(N + 1, sizeof(double)); rk[c] = (uint16_t *)calloc(N + 1, sizeof(uint16_t)); }
    uint8_t *anchor = (uint8_t *)calloc(N + 1, 1), *litmask = (uint8_t *)calloc(N + 1, 1);
    uint32_t *cmask = (uint32_t *)calloc(N + 1, 4), *omask = (uint32_t *)calloc(N + 1, 4); double *scop = (double *)calloc(N + 1, sizeof(double));
    for (int i = 0; i < q.n; i++) {
        const QTerm *t = &q.t[i];
        for (uint32_t k = 0; k < t->nhit; k++) {
            const CsHit *h = &t->hit[k]; uint32_t d = h->doc; double b = cs_bm25(&v, t, h);
            if (t->cls == QC_ORIG || t->cls == QC_WHOLE) sc[CH_BM25][d] += b;
            if (t->cls == QC_ORIG && (h->tf[CF_PART] || h->tf[CF_NAME])) sc[CH_SUB][d] += t->idf * (1.0 + log(1.0 + h->tf[CF_PART]) + (h->tf[CF_NAME] ? 1.0 : 0.0));
            if (t->cls == QC_WHOLE) sc[CH_EXACT][d] += 1.0 + (h->tf[CF_NAME] || h->tf[CF_PATH] ? 5.0 : 0.0) + log(1.0 + h->tf[CF_WORD] + h->tf[CF_PART]);
            if (t->cls == QC_FUZZY) sc[CH_TRI][d] += b;
            if (t->cls == QC_OP) scop[d] += b; else if (t->cls >= QC_ABBR && t->cls != QC_FUZZY) sc[CH_CON][d] += b;
            if ((t->side || t->cls == QC_ORIG) && t->src >= 0 && (h->tf[CF_PATH] || h->tf[CF_NAME])) { if (t->side & 1) cmask[d] |= 1u << t->src; if (t->side & 2) omask[d] |= 1u << t->src; }
        }
    }
    for (uint32_t d = 0; d < N; d++) if (sc[CH_CON][d] > 0) sc[CH_CON][d] += scop[d];       /* verbos de operacao so reforcam quem ja casou por conceito/abreviacao (sozinhos sao ruido: run, get, read) */
    /* ancoras: nome de arquivo (ou fim do caminho) citado; palavra simples so conta se a pergunta tem ate 2 termos de conteudo */
    for (int a = 0; a < Q.na; a++) {
        const CsAnchor *an = &Q.a[a]; if (!(an->codey || norig <= 2)) continue;
        size_t al = strlen(an->w); int has_slash = strchr(an->w, '/') != NULL;
        for (uint32_t g = 0; g < N; g++) {
            uint32_t bl; const char *bn = cs_basename(v.path[g], v.plen[g], &bl); int hit = 0;
            if (has_slash) { if (v.plen[g] >= al && !strncasecmp(v.path[g] + v.plen[g] - al, an->w, al) && (v.plen[g] == al || v.path[g][v.plen[g] - al - 1] == '/')) hit = 1; }
            else if (bl == al && !strncasecmp(bn, an->w, al)) hit = 1;
            else { uint32_t dot = bl; while (dot && bn[dot - 1] != '.') dot--; if (dot > 1 && dot - 1 == al && !strncasecmp(bn, an->w, al)) hit = 1; }
            if (hit) { sc[CH_EXACT][g] += 10.0; anchor[g] = (uint8_t)(a + 1); }
        }
    }
    /* literal: frases entre aspas, candidatos pelos postings das palavras da frase e verificacao relendo o arquivo */
    int phr_ok[8] = {0}, phr_idx_ok[8] = {0};
    for (int p = 0; p < Q.nphr; p++) {
        CoderTok pt[64]; int n = coder_tokens_ex(Q.phr[p], pt, 64, 0, CT_AUTO); CsHit *acc = NULL; uint32_t na = 0; int first = 1;
        for (int i = 0; i < n; i++) { if (pt[i].stop || pt[i].kind != 0) continue;
            char key[100]; snprintf(key, sizeof key, "n%s", pt[i].norm); uint32_t nh; CsHit *h = cs_key_hits(&v, key, strlen(key), &nh);
            if (first) { acc = h; na = nh; first = 0; } else acc = cs_hits_and(acc, na, h, nh, &na); phr_idx_ok[p] = 1; }
        if (!phr_idx_ok[p]) continue;
        uint32_t *cand = (uint32_t *)malloc(sizeof *cand * (na + 1)); double *cs = (double *)calloc((size_t)N + 1, sizeof(double));
        for (uint32_t k = 0; k < na; k++) { cand[k] = acc[k].doc; cs[acc[k].doc] = 1.0 + acc[k].tf[0] * 3 + acc[k].tf[1] * 3 + acc[k].tf[2] + acc[k].tf[3]; }
        g_cs_sortsc = cs; qsort(cand, na, sizeof *cand, cs_cmp_sc);
        for (uint32_t k = 0; k < na && k < 400; k++) {
            size_t len; char *b = cs_read_doc(&v, cand[k], &len); if (!b) continue;
            int cnt = 0; for (const char *s = b; (s = cs_stristr(s, Q.phr[p])) != NULL && cnt < 50; s++) cnt++;
            if (cnt) { sc[CH_LIT][cand[k]] += 1.0 + log(1.0 + cnt); litmask[cand[k]] |= (uint8_t)(1u << p); phr_ok[p] = 1; }
            free(b);
        }
        free(cand); free(cs); free(acc);
    }
    for (int c = 0; c < CH_N; c++) cs_rank(sc[c], N, rk[c], 500);
    /* fusao: camada, depois RRF (ou soma ponderada) */
    double mx[CH_N]; for (int c = 0; c < CH_N; c++) { mx[c] = 0; for (uint32_t i = 0; i < N; i++) if (sc[c][i] > mx[c]) mx[c] = sc[c][i]; }
    CsRes *res = (CsRes *)malloc(sizeof *res * ((size_t)N + 1)); uint32_t nres = 0;
    for (uint32_t d = 0; d < N; d++) {
        int any = 0; double f = 0;
        for (int c = 0; c < CH_N; c++) { if (!rk[c][d]) continue; any = 1;
            f += g_cs_chw[c] / (g_cs_rrfk + rk[c][d]); }
        if (!any) continue;
        int pair = cmask[d] && omask[d] && ((cmask[d] & (cmask[d] - 1)) || (omask[d] & (omask[d] - 1)) || cmask[d] != omask[d]);
        res[nres].doc = d; res[nres].fused = f; res[nres].tier = (sc[CH_EXACT][d] > 0 || litmask[d]) ? 1 : pair ? 2 : 3; nres++;
    }
    qsort(res, nres, sizeof *res, cs_cmp_res);
    t_score = now_sec() - t0 - t_open - t_kb - t_load;
    double t_total = now_sec() - t0;
    /* o que NAO foi encontrado */
    char nf[600]; nf[0] = 0; int nnf = 0;
    for (int i = 0; i < q.n; i++) { const QTerm *t = &q.t[i]; if ((t->cls == QC_ORIG || t->cls == QC_WHOLE) && t->nhit == 0) { size_t l = strlen(nf); if (l < 500) snprintf(nf + l, sizeof nf - l, "%s%s", nnf ? ", " : "", t->disp); nnf++; } }
    for (int p = 0; p < Q.nphr; p++) if (!phr_ok[p]) { size_t l = strlen(nf); if (l < 500) snprintf(nf + l, sizeof nf - l, "%s\"%s\" (literal phrase%s)", nnf ? ", " : "", Q.phr[p], phr_idx_ok[p] ? "" : ", no indexable words"); nnf++; }
    if (ho) {
        snprintf(ho->notfound, sizeof ho->notfound, "%s", nf); ho->files = N; ho->ms = t_total * 1000; ho->total = (int)nres;
    } else if (json) {
        printf("{\"query\":"); cs_jstr(query, 1024); printf(",\"files_indexed\":%u,\"results_total\":%u,\"ms\":%.2f,", N, nres, t_total * 1000);
        cs_json_terms(&q); printf(",\"not_found\":[");
        { int first = 1; for (int i = 0; i < q.n; i++) { const QTerm *t = &q.t[i]; if ((t->cls == QC_ORIG || t->cls == QC_WHOLE) && t->nhit == 0) { printf("%s", first ? "" : ","); cs_jstr(t->disp, 64); first = 0; } }
          for (int p = 0; p < Q.nphr; p++) if (!phr_ok[p]) { printf("%s", first ? "" : ","); cs_jstr(Q.phr[p], 256); first = 0; } }
        printf("],\"results\":[");
    } else {
        printf("search \"%s\" in %s: %u files, %d segment(s), %u term(s) used; %.1f ms (open %.1f, kb %.1f, terms %.1f, score+fuse %.1f)\n", query, v.r->man.root, N, v.ns, (unsigned)q.n, t_total * 1000, t_open * 1000, t_kb * 1000, t_load * 1000, t_score * 1000);
        if (v.noindex) printf("warning: %d segment(s) have no search index (older store); run `katana coder compact` after deleting the store, or re-index from scratch\n", v.noindex);
        printf("terms:");
        for (int i = 0; i < q.n; i++) { const QTerm *t = &q.t[i]; if (t->cls == QC_ORIG || t->cls == QC_WHOLE) printf(" %s(%u)", t->disp, t->nhit); }
        printf("\n");
        int shown = 0; for (int i = 0; i < q.n; i++) { const QTerm *t = &q.t[i]; if (t->cls == QC_ORIG || t->cls == QC_WHOLE) continue; if (!why && shown >= 14) continue; shown++;
            if (why) printf("  [%s w=%.2f docs=%u] %s%s%s\n", g_cs_clsname[t->cls], t->w, t->nhit, t->disp, t->why[0] ? "  :: " : "", t->why); }
        if (!why && shown) { printf("expanded (conceptual/fuzzy, lower weight, never replacing yours):"); int k = 0; for (int i = 0; i < q.n && k < 14; i++) { const QTerm *t = &q.t[i]; if (t->cls == QC_ORIG || t->cls == QC_WHOLE) continue; printf(" %s[%s %u]", t->disp, g_cs_clsname[t->cls][0] == 'c' ? "con" : g_cs_clsname[t->cls][0] == 'o' ? "op" : g_cs_clsname[t->cls][0] == 'f' ? "fz" : "ab", t->nhit); k++; } printf("\n"); }
        if (nnf) printf("NOT FOUND in the index (no occurrence): %s\n", nf);
        if (!nres) printf("(no results)\n");
    }
    SyView sv; memset(&sv, 0, sizeof sv); int sv_tried = 0, sv_ok = 0, shown_units = 0;
    for (uint32_t r = 0; r < nres && (int)r < limit; r++) {
        uint32_t d = res[r].doc; int exactg = sc[CH_EXACT][d] > 0 || litmask[d];
        char chl[200]; chl[0] = 0;
        for (int c = 0; c < CH_N; c++) if (rk[c][d]) { size_t l = strlen(chl); snprintf(chl + l, sizeof chl - l, "%s%s#%d", l ? " " : "", g_cs_chname[c], rk[c][d]); }
        if (ho) {
            if (ho->n >= CODER_HITS_MAX) continue;
            CoderHit *H = &ho->r[ho->n++]; memset(H, 0, sizeof *H);
            snprintf(H->path, sizeof H->path, "%.*s", (int)v.plen[d], v.path[d]); H->exact = exactg; H->tier = res[r].tier; H->literal = litmask[d] != 0; H->anchor = anchor[d] != 0;
            snprintf(H->channels, sizeof H->channels, "%s", chl);
            CsTW tw[256]; int nm = 0; for (int i = 0; i < q.n && nm < 256; i++) if (cs_qt_find(&q.t[i], d)) { tw[nm].i = i; tw[nm++].w = q.t[i].w * q.t[i].idf; }
            qsort(tw, (size_t)nm, sizeof *tw, cs_tw_cmp);
            int ti[CS_EVBITS]; int nti = nm < CS_EVBITS ? nm : CS_EVBITS; for (int i = 0; i < nti; i++) ti[i] = tw[i].i;
            CsEvLine ev[40]; int ne = cs_evidence(&v, d, &q, ti, nti, Q.phr, litmask[d], ev, CODER_EV_MAX, tk);
            for (int i = 0; i < ne && i < CODER_EV_MAX; i++) { H->line[i] = ev[i].line; snprintf(H->text[i], sizeof H->text[i], "%s", ev[i].text); H->nev++; }
            continue;
        }
        CsUnit un[3]; int nu = -1, fl[3], nfl = 0; char *ubuf = NULL; size_t ulen = 0; uint32_t *ulm = NULL;
        if (!sv_tried) { sv_tried = 1; char e2[200]; if (sy_view_open(proj, &sv, e2, sizeof e2) == 0) sv_ok = 1; }
        if (sv_ok) {
            CsTW tw2[256]; int nm2 = 0; for (int i = 0; i < q.n && nm2 < 256; i++) if (cs_qt_find(&q.t[i], d)) { tw2[nm2].i = i; tw2[nm2++].w = q.t[i].w * q.t[i].idf; }
            qsort(tw2, (size_t)nm2, sizeof *tw2, cs_tw_cmp);
            int ti2[CS_EVBITS]; int nti2 = nm2 < CS_EVBITS ? nm2 : CS_EVBITS; for (int i = 0; i < nti2; i++) ti2[i] = tw2[i].i;
            int first2[32], nlm = 0; ulm = cs_linemasks(&v, d, &q, ti2, nti2, Q.phr, litmask[d], tk, &ubuf, &ulen, &nlm, first2);
            if (ulm) nu = cs_symbol_units(&sv, v.path[d], v.plen[d], ulm, nlm, ubuf, ulen, &q, ti2, nti2, tk, un, 3, fl, &nfl);
            if (nu <= 0) nu = -1;
        }
        shown_units += nu > 0 ? nu + (nfl ? 1 : 0) : 1;
        if (json) {
            printf("%s{\"path\":", r ? "," : ""); cs_jstr(v.path[d], v.plen[d]);
            { int fconf = cs_tier_conf(res[r].tier) + (exactg ? 2 : 0); printf(",\"tier\":%d,\"confidence\":%d,\"level\":\"%s\",\"how\":\"%s\",\"unit\":\"%s\",\"score\":%.5f,\"channels\":{", res[r].tier, fconf, sy_level(fconf), exactg ? "exact name or literal anchor in the query (tier 1 layer)" : "ranked match (lower layer)", nu > 0 ? "symbol" : "file", res[r].fused); }
            { int first = 1; for (int c = 0; c < CH_N; c++) if (rk[c][d]) { printf("%s\"%s\":%d", first ? "" : ",", g_cs_chname[c], rk[c][d]); first = 0; } }
            printf("},\"evidence\":[");
            CsTW tw[256]; int nm = 0; for (int i = 0; i < q.n && nm < 256; i++) if (cs_qt_find(&q.t[i], d)) { tw[nm].i = i; tw[nm++].w = q.t[i].w * q.t[i].idf; }
            qsort(tw, (size_t)nm, sizeof *tw, cs_tw_cmp);
            int ti[CS_EVBITS]; int nti = nm < CS_EVBITS ? nm : CS_EVBITS; for (int i = 0; i < nti; i++) ti[i] = tw[i].i;
            CsEvLine ev[40]; int ne = cs_evidence(&v, d, &q, ti, nti, Q.phr, litmask[d], ev, 5, tk);
            for (int i = 0; i < ne; i++) { printf("%s{\"line\":%d,\"text\":", i ? "," : "", ev[i].line); cs_jstr(ev[i].text, 150); printf(",\"terms\":[");
                int f2 = 1; for (int b = 0; b < CS_EVBITS + 8; b++) if (ev[i].mask >> b & 1) { printf("%s", f2 ? "" : ","); if (b < CS_EVBITS) cs_jstr(q.t[ti[b]].disp, 64); else cs_jstr(Q.phr[b - CS_EVBITS], 256); f2 = 0; } printf("]}"); }
            printf("],\"matched\":[");
            for (int k = 0; k < nm && k < 16; k++) { const QTerm *t = &q.t[tw[k].i]; const CsHit *h = cs_qt_find(t, d); char fl[160]; cs_fields_str(h, fl, sizeof fl); printf("%s{\"term\":", k ? "," : ""); cs_jstr(t->disp, 64); printf(",\"class\":\"%s\",\"fields\":", g_cs_clsname[t->cls]); cs_jstr(fl, 160); printf("}"); }
            printf("]");
            if (nu > 0) { printf(",\"symbols\":[");
                for (int k = 0; k < nu; k++) { int cf; char how[160]; cs_unit_conf(&un[k], res[r].tier, &cf, how, sizeof how);
                    printf("%s{\"line0\":%d,\"line1\":%d,\"kind\":\"%s\",\"name\":", k ? "," : "", un[k].l0, un[k].l1, un[k].kind < CK_N ? CP_KINDN[un[k].kind] : "?"); cs_jstr(un[k].name, 100);
                    printf(",\"sig\":"); cs_jstr(un[k].sig, 160); printf(",\"confidence\":%d,\"level\":\"%s\",\"how\":", cf, sy_level(cf)); cs_jstr(how, 160);
                    printf(",\"matched\":[%s%s%s],\"lines\":[", un[k].nmask ? "\"name\"" : "", un[k].smask ? (un[k].nmask ? ",\"signature\"" : "\"signature\"") : "", un[k].bmask ? (un[k].nmask || un[k].smask ? ",\"body\"" : "\"body\"") : "");
                    for (int z = 0; z < 3 && un[k].ev[z]; z++) { char lt[200]; cs_line_text(ubuf, ulen, un[k].ev[z], lt, sizeof lt); printf("%s{\"line\":%d,\"text\":", z ? "," : "", un[k].ev[z]); cs_jstr(lt, 150); printf("}"); }
                    printf("]}"); }
                printf("]"); }
            printf("}"); free(ulm); free(ubuf);
        } else {
            int fconf = cs_tier_conf(res[r].tier) + (exactg ? 2 : 0); if (nu > 0) {
                printf("%2u. %.*s   [file confidence %d %s] tier %d  score %.4f  (%s)\n", r + 1, (int)v.plen[d], v.path[d], fconf, sy_level(fconf), res[r].tier, res[r].fused, chl);
                if (anchor[d]) printf("      named in the query: \"%s\" matches the file name\n", Q.a[anchor[d] - 1].w);
                if (why && res[r].tier == 2) printf("      tier 2: a concept term and an operation term from different query words both appear in the path/file name\n");
                for (int k = 0; k < nu; k++) { int cf; char how[160]; cs_unit_conf(&un[k], res[r].tier, &cf, how, sizeof how);
                    printf("      %.*s:%d-%d  %s %s   [confidence %d %s; %s]\n", (int)v.plen[d], v.path[d], un[k].l0, un[k].l1, un[k].kind < CK_N ? CP_KINDN[un[k].kind] : "?", un[k].name, cf, sy_level(cf), how);
                    if (un[k].sig[0]) printf("          %s\n", un[k].sig);
                    for (int z = 0; z < 3 && un[k].ev[z]; z++) { char lt[200]; cs_line_text(ubuf, ulen, un[k].ev[z], lt, sizeof lt); printf("          line %d: %.150s\n", un[k].ev[z], lt); } }
            } else {
                printf("%2u. %.*s   [confidence %d %s] tier %d  score %.4f  (%s)\n", r + 1, (int)v.plen[d], v.path[d], fconf, sy_level(fconf), res[r].tier, res[r].fused, chl);
                if (anchor[d]) printf("      named in the query: \"%s\" matches the file name\n", Q.a[anchor[d] - 1].w);
                if (why && res[r].tier == 2) printf("      tier 2: a concept term and an operation term from different query words both appear in the path/file name\n");
                cs_print_evidence_text(&v, d, &q, &Q, litmask[d], tk, why, NULL, 0);
            }
            free(ulm); free(ubuf);
        }
    }
    if (ho) {}
    else if (json) printf("]}\n");
    else if (nres > (uint32_t)limit) printf("... %u more (use --limit)\n", nres - (uint32_t)limit);
    if (sv_ok) sy_view_close(&sv);
    for (int c = 0; c < CH_N; c++) { free(sc[c]); free(rk[c]); }
    for (int i = 0; i < q.n; i++) free(q.t[i].hit);
    free(q.t); free(anchor); free(scop); free(litmask); free(cmask); free(omask); free(res); free(tk); cs_view_close(&v);
    return 0;
}

/* ---- katana coder find-name <nome>: arquivos com esse nome (exato, parcial, parecido) e arquivos que citam o identificador ---- */
typedef struct { uint32_t doc; int kind; double sc; } CsNameRes;
static int cs_cmp_nameres(const void *a, const void *b) { const CsNameRes *x = (const CsNameRes *)a, *y = (const CsNameRes *)b; if (x->kind != y->kind) return x->kind < y->kind ? -1 : 1; if (x->sc != y->sc) return x->sc < y->sc ? 1 : -1; return x->doc < y->doc ? -1 : x->doc > y->doc; }
static int coder_find_name_main(int argc, char **argv) {
    const char *name = NULL, *proj = "."; int limit = 15, json = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--json")) json = 1;
        else if (!name) name = argv[i]; else proj = argv[i];
    }
    if (!name || !*name) { fprintf(stderr, "uso: katana coder find-name <nome> [pasta] [--limit N] [--json]\n"); return 2; }
    double t0 = now_sec(); CsView v; char err[400];
    if (cs_view_open(proj, &v, err, sizeof err) != 0) { fprintf(stderr, "find-name: %s\n", err); return 1; }
    CoderTok *tk = (CoderTok *)malloc(4096 * sizeof(CoderTok));
    int ntk = coder_tokens_ex(name, tk, 64, 0, CT_L_CODE);
    QSet q = {0}; char compact[128]; cs_compact(name, strlen(name), compact, sizeof compact);
    int whole_tok = 0; for (int j = 0; j < ntk; j++) if (tk[j].kind == 1) whole_tok = 1;
    for (int i = 0; i < ntk; i++) { if (tk[i].kind == 2 || (whole_tok && tk[i].kind != 1)) continue;
        QTerm x; memset(&x, 0, sizeof x); x.w = 1; x.nm = 1; x.cls = QC_WHOLE; x.src = -1; char lo[64]; cs_lower(lo, tk[i].orig, sizeof lo); cs_qmem(&x.m[0], tk[i].norm, lo); snprintf(x.disp, sizeof x.disp, "%s", tk[i].norm); cs_qt_add(&q, &x); }
    int nuser = q.n;
    for (int i = 0; i < nuser; i++) cs_qt_load(&v, &q.t[i]);
    /* parecidos pelo lexico: a forma com _ e a forma compacta */
    const char *forms[2] = {nuser ? q.t[0].m[0].k[0] + 1 : compact, compact};
    for (int f = 0; f < 2; f++) { CsCand cd[8]; int n = cs_fuzzy(&v, forms[f], (int)strlen(forms[f]), cd, 6);
        for (int k = 0; k < n; k++) { QTerm x; memset(&x, 0, sizeof x); x.cls = QC_FUZZY; x.src = -1; x.nm = 1; x.w = 0.6 * cd[k].sim; snprintf(x.m[0].k[0], sizeof x.m[0].k[0], "%s", cd[k].key); x.m[0].nk = 1; snprintf(x.disp, sizeof x.disp, "%s", cd[k].key + 1);
            snprintf(x.why, sizeof x.why, "lexicon term \"%s\" is %s of \"%s\"", cd[k].key + 1, cd[k].substr ? "a superstring" : "a near spelling", forms[f]);
            int idx = q.n; if (cs_qt_add(&q, &x)) cs_qt_load(&v, &q.t[idx]); } }
    uint32_t N = v.N; CsNameRes *res = (CsNameRes *)calloc((size_t)N + 1, sizeof *res); uint32_t nres = 0; int8_t *kind = (int8_t *)malloc(N + 1); memset(kind, 127, N + 1); double *sc = (double *)calloc((size_t)N + 1, sizeof(double));
    size_t cl = strlen(compact);
    for (uint32_t g = 0; g < N; g++) {                                      /* pelo nome do arquivo: 0 exato, 2 contem, 3 parecido */
        uint32_t bl; const char *bn = cs_basename(v.path[g], v.plen[g], &bl); char bc[128], bs[128]; cs_compact(bn, bl, bc, sizeof bc);
        uint32_t dot = bl; while (dot && bn[dot - 1] != '.') dot--; cs_compact(bn, dot > 1 ? dot - 1 : bl, bs, sizeof bs);
        int k = 127; size_t bsl = strlen(bs);
        if (!strcmp(bc, compact) || !strcmp(bs, compact)) k = 0;
        else if (cl >= 3 && strstr(bs, compact)) { k = 2; sc[g] = (double)cl / (double)(bsl ? bsl : 1); }
        else if (cl >= 5 && bsl >= 4 && cs_lev(bs, (int)bsl, compact, (int)cl) <= (int)(cl / 5 > 0 ? cl / 5 : 1)) { k = 3; sc[g] = 0.5; }
        if (k < kind[g]) kind[g] = (int8_t)k;
    }
    for (int i = 0; i < q.n; i++) { const QTerm *t = &q.t[i];                 /* pelo que cita: 1 identificador exato, 4 parecido */
        for (uint32_t k = 0; k < t->nhit; k++) { const CsHit *h = &t->hit[k]; int kk = t->cls == QC_WHOLE ? 1 : 4; if (kk < kind[h->doc]) { kind[h->doc] = (int8_t)kk; sc[h->doc] = 0; } if (kk == kind[h->doc]) sc[h->doc] += t->w * (1.0 + log(1.0 + h->tf[CF_WORD] + h->tf[CF_PART]) + (h->tf[CF_NAME] ? 3 : 0)); } }
    for (uint32_t g = 0; g < N; g++) if (kind[g] != 127) { res[nres].doc = g; res[nres].kind = kind[g]; res[nres].sc = sc[g]; nres++; }
    qsort(res, nres, sizeof *res, cs_cmp_nameres);
    static const char *const kn[] = {"file name is exactly this", "file mentions this identifier", "file name contains this", "file name is similar", "file mentions a similar identifier"};
    double ms = (now_sec() - t0) * 1000;
    if (json) printf("{\"name\":"); else printf("find-name \"%s\" in %s: %u files, %u candidate(s), %.1f ms\n", name, v.r->man.root, N, nres, ms);
    if (json) { cs_jstr(name, 256); printf(",\"ms\":%.2f,\"results\":[", ms); }
    CsQuery Q; memset(&Q, 0, sizeof Q);
    if (!nres && !json) printf("NOT FOUND: no file is named like \"%s\" and no indexed file contains it or a similar identifier\n", name);
    for (uint32_t r = 0; r < nres && (int)r < limit; r++) {
        uint32_t d = res[r].doc; int k = res[r].kind; static const int kc[] = {92, 85, 55, 40, 35}; int cf = kc[k]; const char *lv = cf >= 75 ? "high" : cf >= 45 ? "medium" : "low";
        if (json) { printf("%s{\"path\":", r ? "," : ""); cs_jstr(v.path[d], v.plen[d]); printf(",\"kind\":\"%s\",\"confidence\":%d,\"level\":\"%s\"}", kn[k], cf, lv); continue; }
        printf("%2u. %.*s   [confidence %d %s] %s\n", r + 1, (int)v.plen[d], v.path[d], cf, lv, kn[k]);
        if (k == 1 || k == 4) {
            CsTW tw[64]; int nm = 0; for (int i = 0; i < q.n && nm < 64; i++) if (cs_qt_find(&q.t[i], d)) { tw[nm].i = i; tw[nm++].w = q.t[i].w; }
            qsort(tw, (size_t)nm, sizeof *tw, cs_tw_cmp);
            int ti[CS_EVBITS]; int nti = nm < CS_EVBITS ? nm : CS_EVBITS; for (int i = 0; i < nti; i++) ti[i] = tw[i].i;
            CsEvLine ev[40]; int ne = cs_evidence(&v, d, &q, ti, nti, Q.phr, 0, ev, 3, tk);
            for (int i = 0; i < ne; i++) { printf("      line %d: %s   <- ", ev[i].line, ev[i].text); int f2 = 1; for (int b = 0; b < CS_EVBITS; b++) if (ev[i].mask >> b & 1) { printf("%s%s", f2 ? "" : ", ", q.t[ti[b]].disp); f2 = 0; } printf("\n"); }
        }
    }
    if (json) printf("]}\n");
    for (int i = 0; i < q.n; i++) free(q.t[i].hit);
    free(q.t); free(res); free(kind); free(sc); free(tk); cs_view_close(&v);
    return 0;
}

/* ---- porta da biblioteca: consulta estruturada, sem imprimir (usada pelo Find, src/tools/find.c) ----
 * Atualiza o indice do coder da pasta de forma incremental (cria na primeira consulta) e roda o mesmo nucleo do `katana coder search`.
 * concept=0: sem o canal conceitual (base de conhecimento). Devolve 0, ou -1 se o indice nao pode ser criado ou lido. */
static int coder_lookup(const char *root, const char *query, int concept, int limit, CoderHits *ho) {
    memset(ho, 0, sizeof *ho); ho->concept = concept;
    double t0 = now_sec();
    IndexRes ir; const int rc = index_ensure(root, 2, "", 0, &ir);      /* varre e grava so se preciso (indexctl.c); simbolos junto: o indice nunca fica com arquivos sem simbolos */
    ho->created = ir.created;
    ho->index_ms = (now_sec() - t0) * 1000;
    if (rc != 0) return -1;
    if (limit < 1) limit = 1;
    if (limit > CODER_HITS_MAX) limit = CODER_HITS_MAX;
    return cs_search_core(query, root, limit, 0, 0, ho) == 0 ? 0 : -1;
}
