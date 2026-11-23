/* ---------- coder/findstore.c: o Find unificado sobre o armazenamento do coder (fase 7, docs/code-index-lifecycle.md E) ----------
 * O Find (tools/find.c) nao tem indice proprio em disco:
 *   - a lista de arquivos e os CANDIDATOS vem do indice do coder: o lexico e os postings (busca por campos, os mesmos termos da pergunta) dao os
 *     cerca de 100 arquivos mais provaveis (KATANA_FIND_CAND), mais os que o motor de nomes e texto sugeriu (CoderHits) e os que definem um nome da pergunta;
 *   - o indice de trechos do Find (trechos de 40 linhas, BM25 por campo, simbolos por regra) e montado a cada consulta SO sobre esses arquivos, lidos do
 *     disco (eles sao lidos de qualquer jeito para mostrar linhas); o idf usa estimativas do corpus inteiro (document frequency do lexico do store);
 *   - o grafo de arquivos vem da secao STS_GRAPH (graph.c), derivada dos niveis N2/N3 (simbolos, referencias, imports, chaves de texto) e do lexico do N1
 *     (onde cada nome definido e cada nome de arquivo com extensao aparece) em vez de reler todos os arquivos.
 * Nada aqui varre a arvore: a atualizacao do catalogo e do coder (index_ensure, que cria o indice na primeira chamada). O que o ranking perde em
 * relacao ao indice proprio: so os candidatos entram no BM25 de trechos (a nota de um arquivo fora deles nunca e calculada); o df por campo e estimado. */
#ifndef _WIN32
struct FsCtx {
    char root[4096], pre[2048]; IndexLoc loc;
    CsView cv; int cv_ok; SyView sv; int sv_ok;
    FgsGraph g; int gstate, gsrc; int loop;           /* gstate 0 = nao tentou, 1 = tem, -1 = nao tem; gsrc 1 = lido, 2 = derivado e gravado, 3 = derivado so em memoria, 4 = velho (assinatura) */
    double total_len;
    int nq; uint32_t qid[64]; double qw[64]; char qsrc[64][64]; double dfbody[64], dfpath[64];
    int ncand; int cg[512];                           /* documento (numero global da vista) de cada arquivo de X, -1 se nao se sabe */
    double ens_ms, cand_ms, build_ms, graph_ms; int created, walked;
};

static int fs_cand_max(void) { const char *e = getenv("KATANA_FIND_CAND"); const int v = e ? atoi(e) : 0; return v >= 10 && v <= 300 ? v : 100; }
static int fs_graph_max(void) { const char *e = getenv("KATANA_FIND_GRAPH_MAX"); const int v = e ? atoi(e) : 0; return v > 0 ? v : 30000; }

static struct FsCtx *fs_begin(const char *root, const char *pre, struct FdIndexS *X, double *ms_ensure, char *err, size_t ecap) {
    IndexRes ir; const double t0 = fd_ms();
    if (index_ensure(root, 2, "", 0, &ir) != 0) { snprintf(err, ecap, "the index could not be created or read"); return NULL; }
    struct FsCtx *F = (struct FsCtx *)calloc(1, sizeof *F);
    snprintf(F->root, sizeof F->root, "%s", root); snprintf(F->pre, sizeof F->pre, "%s", pre ? pre : "");
    index_resolve(root, 0, &F->loc);
    F->created = ir.created; F->walked = ir.walked;
    if (cs_view_open(root, &F->cv, err, ecap) != 0) { free(F); return NULL; }
    F->cv_ok = 1;
    char e2[300]; if (sy_view_open(root, &F->sv, e2, sizeof e2) == 0) F->sv_ok = 1;
    for (uint32_t g = 0; g < F->cv.N; g++) F->total_len += F->cv.lens[(size_t)g * 3 + 2];
    fd_store_reset(X);
    X->fs = F; X->ready = 0;
    snprintf(X->root, sizeof X->root, "%s", root);
    F->ens_ms = (fd_ms() - t0);
    if (ms_ensure) *ms_ensure = F->ens_ms;
    return F;
}
static void fs_end(struct FsCtx *F) {
    if (!F) return;
    if (F->loop) return;                                               /* o contexto do laco e reaproveitado */
    if (F->cv_ok) cs_view_close(&F->cv);
    if (F->sv_ok) sy_view_close(&F->sv);
    fgs_free(&F->g);
    if (g_fd.fs == F) g_fd.fs = NULL;
    free(F);
}
static uint32_t fs_nfiles(struct FsCtx *F) { return F->cv_ok ? F->cv.N : 0; }

/* ---- caminhos ---- */
static uint64_t fs_hash_rel(struct FsCtx *F, const char *rel) {
    char b[4400]; const size_t pl = strlen(F->loc.prefix);
    const int n = pl ? snprintf(b, sizeof b, "%s/%s", F->loc.prefix, rel) : snprintf(b, sizeof b, "%s", rel);
    return fgs_hash(b, n > 0 && (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}
static const char *fs_strip(struct FsCtx *F, const char *full) {
    const size_t pl = strlen(F->loc.prefix);
    if (!pl) return full;
    return !strncmp(full, F->loc.prefix, pl) && full[pl] == '/' ? full + pl + 1 : NULL;
}
static int fs_rel_copy(const char *p, uint32_t l, char *o, size_t cap) { if (l + 1 > cap) return 0; memcpy(o, p, l); o[l] = 0; return 1; }

static int fs_listed(struct FsCtx *F, const char *rel) {
    if (fd_kind(rel) < 0 || fd_skip_rel(rel)) return 0;
    if (!F->sv_ok) return 1;
    return sy_lookup(&F->sv, rel, strlen(rel)) != NULL;
}

/* ---- consultas ao indice de simbolos ---- */
static int fs_cmp_pp(const void *a, const void *b) {
    const SyRef *x = *(const SyRef *const *)a, *y = *(const SyRef *const *)b;
    return st_cmp_bytes(x->path, x->plen, y->path, y->plen);
}
/* arquivos que citam o nome (referencias do indice de simbolos), por caminho, sem repetir; 0 se o indice nao tem simbolos ou o nome e comum demais (> 2000) */
static int fs_ref_files(struct FsCtx *F, const char *name, int l, char (*out)[300], int *cnt, int max) {
    if (!F->sv_ok || !F->sv.any) return 0;
    SyRefs r = {0}; sy_refs_of(&F->sv, name, (size_t)l, ~0u, &r);
    int n = 0;
    if (r.n > 0 && r.n <= 2000) {
        const SyRef **pp = (const SyRef **)malloc(sizeof *pp * (size_t)r.n);
        for (int i = 0; i < r.n; i++) pp[i] = &r.r[i];
        qsort(pp, (size_t)r.n, sizeof *pp, fs_cmp_pp);
        for (int i = 0; i < r.n && n < max; i++) {
            if (i && pp[i]->plen == pp[i - 1]->plen && !memcmp(pp[i]->path, pp[i - 1]->path, pp[i]->plen)) continue;
            if (!fs_rel_copy(pp[i]->path, pp[i]->plen, out[n], 300)) continue;
            if (cnt) cnt[n] = 1;
            n++;
        }
        free(pp);
    }
    free(r.r);
    return n;
}
/* definicoes do nome (fora de documentos de texto e de declaracoes); total = quantas ha */
static int fs_defs(struct FsCtx *F, const char *name, int l, FsDef *out, int max, int *total) {
    *total = 0;
    if (!F->sv_ok || !F->sv.any) return 0;
    SySyms d = {0}; sy_defs_of(&F->sv, name, (size_t)l, &d);
    int n = 0;
    for (int i = 0; i < d.n; i++) {
        if (((d.s[i].pack >> 16) & CSF_DECL)) continue;
        char b[4200]; if (!fs_rel_copy(d.s[i].path, d.s[i].plen, b, sizeof b) || fd_kind(b) == FK_MD) continue;
        (*total)++;
        if (n < max && fs_rel_copy(d.s[i].path, d.s[i].plen, out[n].path, sizeof out[n].path)) { out[n].line = (int)d.s[i].line0; n++; }
    }
    free(d.s);
    return n;
}
static long fs_name_refs(struct FsCtx *F, const char *name, int l) {
    if (!F || !F->sv_ok || !F->sv.any) return 1L << 20;
    const int n = sy_nrefs(&F->sv, name, (size_t)l);
    return n > 0 ? n : 1L << 20;
}

/* ---- candidatos ---- */
static void fs_key_add(char keys[][100], int *nk, char kind, const char *text) {
    if (*nk >= 12 || !text[0] || strlen(text) > 90) return;
    char k[100]; k[0] = kind; size_t i = 0; for (; text[i] && i < 95; i++) k[1 + i] = (char)tolower((unsigned char)text[i]); k[1 + i] = 0;
    for (int q = 0; q < *nk; q++) if (!strcmp(keys[q], k)) return;
    snprintf(keys[(*nk)++], 100, "%s", k);
}
static void fs_keys_of(char keys[][100], int *nk, const char *text) {
    CoderTok tk[48];
    fs_key_add(keys, nk, 'n', text); fs_key_add(keys, nk, 'o', text);
    const int n = coder_tokens_ex(text, tk, 48, CF_WORD, CT_AUTO);
    for (int i = 0; i < n; i++) { if (tk[i].kind == 2) continue; fs_key_add(keys, nk, 'n', tk[i].norm); fs_key_add(keys, nk, 'o', tk[i].orig); }
}
/* documentos que tem o termo do Find (ou a palavra da pergunta que o gerou, quando era uma palavra so): uniao das chaves normalizada e original do lexico */
static CsHit *fs_term_hits(struct FsCtx *F, const char *t, const char *src, int single, uint32_t *n) {
    char keys[12][100]; int nk = 0;
    fs_keys_of(keys, &nk, t);
    if (single && src && src[0] && strcmp(src, t) != 0) fs_keys_of(keys, &nk, src);
    CsHit *H = NULL; uint32_t nh = 0;
    for (int i = 0; i < nk; i++) { uint32_t m = 0; CsHit *h = cs_key_hits(&F->cv, keys[i], strlen(keys[i]), &m); H = cs_hits_union(H, nh, h, m, &nh); }
    *n = nh;
    return H;
}
typedef struct { char rel[300]; int g; double sc; } FsCand;
static int fs_cmp_cand_rel(const void *a, const void *b) { return strcmp(((const FsCand *)a)->rel, ((const FsCand *)b)->rel); }
static int fs_cmp_cand_sc(const void *a, const void *b) { const double x = ((const FsCand *)a)->sc, y = ((const FsCand *)b)->sc; return x < y ? 1 : x > y ? -1 : strcmp(((const FsCand *)a)->rel, ((const FsCand *)b)->rel); }
static void fs_cand_add(FsCand **v, int *n, int *cap, const char *rel, int g, double sc) {
    if (strlen(rel) >= 300) return;
    for (int i = 0; i < *n; i++) if (!strcmp((*v)[i].rel, rel)) { if (g >= 0) (*v)[i].g = g; return; }
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 128; *v = (FsCand *)realloc(*v, sizeof(FsCand) * (size_t)*cap); }
    snprintf((*v)[*n].rel, 300, "%s", rel); (*v)[*n].g = g; (*v)[*n].sc = sc; (*n)++;
}
/* monta X com os arquivos de cand (em ordem de caminho: o desempate do ranking e o de sempre); os ilegiveis ou grandes demais ficam de fora */
static void fs_build_files(struct FsCtx *F, struct FdIndexS *X, FsCand *c, int n) {
    if (n > 1) qsort(c, (size_t)n, sizeof(FsCand), fs_cmp_cand_rel);
    X->f = (FdFile *)calloc((size_t)n + 1, sizeof(FdFile)); X->capf = n + 1; X->nf = 0; F->ncand = 0;
    for (int i = 0; i < n; i++) {
        char abs[8400]; struct stat st; snprintf(abs, sizeof abs, "%s/%s", X->root, c[i].rel);
        const int k = fd_kind(c[i].rel);
        if (k < 0 || stat(abs, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > FD_MAXBYTES) continue;
        FdFile *f = &X->f[X->nf];
        f->rel = strdup(c[i].rel); f->mt = (long long)st.st_mtime; f->size = (long long)st.st_size; f->kind = k; f->node = -1;
        X->nf++;
        if (F->ncand < (int)(sizeof F->cg / sizeof F->cg[0])) F->cg[F->ncand++] = c[i].g; else F->ncand++;
        fd_store_file(X, f);
    }
    fd_invert(X);
}

static int fs_prepare(struct FsCtx *F, struct FdIndexS *X, const char *query, const void *CHv) {
    const CoderHits *CH = (const CoderHits *)CHv;
    const double t0 = fd_ms();
    fd_store_reset(X); X->fs = F;
    const FdTune tu = fd_tune();
    uint32_t qid[64]; double qw[64]; char qsrc[64][64];
    int nq = fd_query_terms(X, query, 1, tu.idw, qid, qw, qsrc);
    F->nq = 0;
    for (int i = 0; i < nq; i++) {                       /* palavras de pedido ("onde", "explique") nao dizem onde olhar: fora, como no ranking */
        if (fg_is_request(fd_tstr(&X->T, (int)qid[i]))) continue;
        F->qid[F->nq] = qid[i]; F->qw[F->nq] = qw[i]; memcpy(F->qsrc[F->nq], qsrc[i], 64); F->nq++;
    }
    if (!F->nq) { F->nq = nq; for (int i = 0; i < nq; i++) { F->qid[i] = qid[i]; F->qw[i] = qw[i]; memcpy(F->qsrc[i], qsrc[i], 64); } }
    const uint32_t N = F->cv.N;
    FsCand *cand = NULL; int nc = 0, ccap = 0;
    /* 1) texto: BM25 por arquivo sobre os termos da pergunta */
    double *sc = (double *)calloc((size_t)N + 1, sizeof(double)), *cov = (double *)calloc((size_t)N + 1, sizeof(double));
    uint32_t *tl = (uint32_t *)malloc(sizeof(uint32_t) * ((size_t)N + 1)); uint8_t *seen = (uint8_t *)calloc((size_t)N + 1, 1); uint32_t nt = 0;
    double idfsum = 0;
    for (int q = 0; q < F->nq; q++) {
        const char *t = fd_tstr(&X->T, (int)F->qid[q]);
        int same = 0; for (int z = 0; z < F->nq; z++) if (!strcmp(F->qsrc[z], F->qsrc[q])) same++;
        uint32_t nh = 0; CsHit *H = fs_term_hits(F, t, F->qsrc[q], same == 1, &nh);       /* so uma palavra que deu um termo so empresta as chaves dela */
        F->dfbody[q] = F->dfpath[q] = 0;
        if (!nh) { free(H); continue; }
        const double idf = log(1 + (N - nh + 0.5) / (nh + 0.5));
        if (idf < 0.05) { free(H); continue; }              /* quase todo arquivo tem: nao distingue */
        idfsum += idf;
        for (uint32_t i = 0; i < nh; i++) {
            const uint32_t g = H[i].doc; const CsHit *h = &H[i];
            const double fp = (double)h->tf[CF_PATH] + h->tf[CF_NAME], fb = (double)h->tf[CF_WORD] + 0.5 * h->tf[CF_PART];
            if (h->tf[CF_WORD] + h->tf[CF_PART]) F->dfbody[q]++;
            if (fp > 0) F->dfpath[q]++;
            const double f = 3.0 * fp + fb; if (f <= 0) continue;
            const double len = F->cv.lens[(size_t)g * 3 + 2], norm = 0.5 + 0.5 * len / F->cv.avg[2];
            sc[g] += F->qw[q] * idf * f * 2.2 / (f + 1.2 * norm); cov[g] += idf;
            if (!seen[g]) { seen[g] = 1; tl[nt++] = g; }
        }
        free(H);
    }
    const int qf = fd_qflags(query);
    const int K = fs_cand_max();
    FsCand *tc = (FsCand *)malloc(sizeof(FsCand) * ((size_t)nt + 1)); int ntc = 0;
    for (uint32_t i = 0; i < nt; i++) {
        const uint32_t g = tl[i]; char rel[4200];
        if (!fs_rel_copy(F->cv.path[g], F->cv.plen[g], rel, sizeof rel) || strlen(rel) >= 300) continue;
        const int k = fd_kind(rel);
        if (k < 0 || fd_skip_rel(rel) || !fd_in_path(rel, F->pre)) continue;
        double s = sc[g] * (1 + FD_W_COVER * (idfsum > 0 ? cov[g] / idfsum : 0));
        if (!(qf & 1)) { if (k == FK_MD) s *= tu.docpen; else if (k == FK_OTHER) s *= FD_W_CFG; }
        if (!(qf & 2) && fd_is_test(rel)) s *= FD_W_TEST;
        snprintf(tc[ntc].rel, 300, "%s", rel); tc[ntc].g = (int)g; tc[ntc].sc = s; ntc++;
    }
    qsort(tc, (size_t)ntc, sizeof(FsCand), fs_cmp_cand_sc);
    for (int i = 0; i < ntc && i < K; i++) fs_cand_add(&cand, &nc, &ccap, tc[i].rel, tc[i].g, tc[i].sc);
    if (getenv("KATANA_FIND_DEBUG")) { fprintf(stderr, "find-debug: candidates: %u files have a query term, %d eligible, %d taken; terms:", nt, ntc, nc < K ? nc : K);
        for (int q = 0; q < F->nq; q++) fprintf(stderr, " %s(%.0f files)", fd_tstr(&X->T, (int)F->qid[q]), F->dfbody[q]); fprintf(stderr, "\n");
        for (int i = 0; i < ntc && i < K + 5; i++) fprintf(stderr, "find-debug: cand %d %-60.60s %.2f%s\n", i + 1, tc[i].rel, tc[i].sc, i >= K ? " (cut)" : ""); }
    free(tc); free(sc); free(cov); free(tl); free(seen);
    /* 2) o que o motor de nomes e texto do coder sugeriu */
    if (CH) for (int i = 0; i < CH->n; i++) if (fs_listed(F, CH->r[i].path) && fd_in_path(CH->r[i].path, F->pre)) fs_cand_add(&cand, &nc, &ccap, CH->r[i].path, -1, 0);
    /* 3) quem DEFINE um nome da pergunta (identificador escrito como esta) */
    if (F->sv_ok && F->sv.any) {
        int nd = 0;
        for (int q = 0; q < F->nq && nd < 8; q++) {
            const char *w = F->qsrc[q]; const int wl = (int)strlen(w);
            if (wl < 4) continue;
            FsDef d[3]; int total = 0; const int m = fs_defs(F, w, wl, d, 3, &total);
            if (total > 3) continue;
            for (int z = 0; z < m && nd < 8; z++) if (fd_in_path(d[z].path, F->pre)) { fs_cand_add(&cand, &nc, &ccap, d[z].path, -1, 0); nd++; }
        }
        /* e quem define um nome IGUAL a um termo da pergunta (o campo de simbolos do ranking de trechos), mesmo que o termo apareca em centenas de arquivos: uma
           pergunta com `list` acha o arquivo que define `list`, nao so os que mais repetem a palavra */
        int nds = 0;
        for (int q = 0; q < F->nq && nds < 40; q++) {
            const char *t = fd_tstr(&X->T, (int)F->qid[q]); const int tl = (int)strlen(t);
            if (tl < 3 || strchr(t, '_')) continue;
            char vr[3][100]; int nv = 0;
            snprintf(vr[nv++], 100, "%s", t);
            { char c[100]; snprintf(c, sizeof c, "%s", t); c[0] = (char)toupper((unsigned char)c[0]); snprintf(vr[nv++], 100, "%s", c); }
            if (strcmp(F->qsrc[q], t) != 0 && strlen(F->qsrc[q]) >= 3) snprintf(vr[nv++], 100, "%s", F->qsrc[q]);
            for (int v = 0; v < nv && nds < 40; v++) {
                FsDef d[40]; int total = 0; const int m = fs_defs(F, vr[v], (int)strlen(vr[v]), d, 40, &total);
                if (total > 40) continue;
                for (int z = 0; z < m && nds < 40; z++) if (fd_in_path(d[z].path, F->pre) && fd_kind(d[z].path) != FK_MD) { fs_cand_add(&cand, &nc, &ccap, d[z].path, -1, 0); nds++; }
            }
        }
    }
    F->cand_ms = fd_ms() - t0;
    const double t1 = fd_ms();
    fs_build_files(F, X, cand, nc);
    free(cand);
    /* estimativas do corpus para o idf: document frequency do lexico (arquivos) vezes quantos trechos por arquivo os candidatos tem com o termo */
    const int T = X->T.n; X->gdfn = T;
    for (int f = 0; f < 3; f++) X->gdf[f] = (double *)calloc((size_t)T + 1, sizeof(double));
    double chunks = 0, lens = 0;
    for (int i = 0; i < X->nf && i < F->ncand && i < (int)(sizeof F->cg / sizeof F->cg[0]); i++) if (F->cg[i] >= 0) { chunks += X->fcn[i]; lens += F->cv.lens[(size_t)F->cg[i] * 3 + 2]; }
    if (lens > 0 && F->total_len > 0) X->gnc = chunks / lens * F->total_len;
    const double rho = X->nf ? (double)X->nc / X->nf : 1;
    for (int q = 0; q < F->nq; q++) {
        const int t = (int)F->qid[q]; if (t >= T) continue;
        const int c2 = X->poff[2][t + 1] - X->poff[2][t], c1 = X->poff[1][t + 1] - X->poff[1][t];
        int files2 = 0, last = -1;
        for (int i = X->poff[2][t]; i < X->poff[2][t + 1]; i++) { const int fi = X->cf[X->pch[2][i]]; if (fi != last) { files2++; last = fi; } }
        const double r = files2 ? (double)c2 / files2 : 1.5;
        if (F->dfbody[q] > 0) { X->gdf[2][t] = F->dfbody[q] * r; if (c2 > 0) X->gdf[1][t] = c1 * (X->gdf[2][t] / c2); }
        if (F->dfpath[q] > 0) X->gdf[0][t] = F->dfpath[q] * rho;
    }
    X->ready = 1;
    F->build_ms = fd_ms() - t1;
    return X->nf;
}
/* mode=refs sem resposta do indice de simbolos: X com os arquivos que tem o nome (e os que o definem), para o caminho de texto de fd_refs */
static int fs_prepare_refs(struct FsCtx *F, struct FdIndexS *X, const char *query) {
    const double t1 = fd_ms();
    fd_store_reset(X); X->fs = F;
    const char *p = query; while (*p && !(isalpha((unsigned char)*p) || *p == '_' || *p == '$')) p++;
    int l = fd_idlen(p, p + strlen(p), FK_PS); while (l > 0 && p[l - 1] == '-') l--;
    FsCand *cand = NULL; int nc = 0, ccap = 0;
    if (l > 0 && l < 200) {
        char name[256]; memcpy(name, p, (size_t)l); name[l] = 0;
        uint32_t nh = 0; CsHit *H = fs_term_hits(F, name, name, 1, &nh);
        FsCand *tc = (FsCand *)malloc(sizeof(FsCand) * ((size_t)nh + 1)); int ntc = 0;
        for (uint32_t i = 0; i < nh; i++) {
            const uint32_t g = H[i].doc; char rel[4200];
            if (!H[i].tf[CF_WORD] && !H[i].tf[CF_PART] && !H[i].tf[CF_NAME]) continue;
            if (!fs_rel_copy(F->cv.path[g], F->cv.plen[g], rel, sizeof rel) || strlen(rel) >= 300 || fd_kind(rel) < 0 || fd_skip_rel(rel) || !fd_in_path(rel, F->pre)) continue;
            snprintf(tc[ntc].rel, 300, "%s", rel); tc[ntc].g = (int)g; tc[ntc].sc = (double)H[i].tf[CF_WORD] + H[i].tf[CF_PART]; ntc++;
        }
        qsort(tc, (size_t)ntc, sizeof(FsCand), fs_cmp_cand_sc);
        for (int i = 0; i < ntc && i < 300; i++) fs_cand_add(&cand, &nc, &ccap, tc[i].rel, tc[i].g, tc[i].sc);
        free(tc); free(H);
        FsDef d[6]; int total = 0; const int m = fs_defs(F, name, l, d, 6, &total);
        for (int z = 0; z < m; z++) if (fd_in_path(d[z].path, F->pre)) fs_cand_add(&cand, &nc, &ccap, d[z].path, -1, 0);
    }
    fs_build_files(F, X, cand, nc);
    free(cand);
    X->ready = 1; F->build_ms = fd_ms() - t1;
    return X->nf;
}
/* acrescenta arquivos a X (vizinhos do grafo e as origens deles) e refaz as listas invertidas; os numeros de trecho dos arquivos que ja estavam nao mudam */
static int fs_add_files(struct FsCtx *F, struct FdIndexS *X, char (*rels)[300], int n) {
    int added = 0;
    for (int i = 0; i < n; i++) {
        int has = 0; for (int q = 0; q < X->nf; q++) if (!strcmp(X->f[q].rel, rels[i])) has = 1;
        if (has) continue;
        char abs[8400]; struct stat st; snprintf(abs, sizeof abs, "%s/%s", X->root, rels[i]);
        const int k = fd_kind(rels[i]);
        if (k < 0 || stat(abs, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > FD_MAXBYTES) continue;
        if (X->nf + 1 >= X->capf) { X->capf = X->capf ? X->capf * 2 : 64; X->f = (FdFile *)realloc(X->f, sizeof(FdFile) * (size_t)X->capf); }
        FdFile *f = &X->f[X->nf]; memset(f, 0, sizeof *f);
        f->rel = strdup(rels[i]); f->mt = (long long)st.st_mtime; f->size = (long long)st.st_size; f->kind = k; f->node = -1;
        X->nf++; fd_store_file(X, f); added++;
    }
    if (added) { fd_invert(X); X->ready = 1; }
    (void)F;
    return added;
}

/* ---- o grafo: abrir, ou derivar do que o indice ja tem ---- */
static int fs_graph_derive(const char *root, uint64_t sig, StoreBuf *out, uint32_t *nnout);
static FgsGraph *fs_graph(struct FsCtx *F) {
    if (F->gstate) return F->gstate > 0 ? &F->g : NULL;
    const double t0 = fd_ms();
    const StoreReader *r = F->sv_ok ? F->sv.r : F->cv_ok ? F->cv.r : NULL;
    F->gstate = -1;
    if (!r) return NULL;
    FgsGraph g; const int have = graph_open(r, &g);
    const uint64_t sig = graph_doc_sig(r);
    if (have && g.sig == sig) { F->g = g; F->gstate = 1; F->gsrc = 1; F->graph_ms = fd_ms() - t0; return &F->g; }
    if (!F->loop && F->cv.N <= (uint32_t)fs_graph_max() && F->sv_ok && F->sv.any) {
        StoreBuf b = {0}; uint32_t nn = 0;
        if (fs_graph_derive(F->loc.root, sig, &b, &nn)) {
            FgsGraph m; uint8_t *mem = (uint8_t *)malloc(b.n + 8);       /* malloc devolve alinhado a 16 */
            memcpy(mem, b.p, b.n);
            if (fgs_parse(mem, b.n, &m)) {
                m.mem = mem; F->g = m; F->gstate = 1; F->gsrc = 3;
                StoreWriter *w; char err[300] = "";           /* grava para as proximas consultas e para os outros processos; se outro escritor esta ativo, fica so em memoria */
                if (store_writer_begin(F->loc.store, &w, err, sizeof err) == 0) {
                    store_writer_add_section(w, STS_GRAPH, 1, nn, b.p, b.n);
                    if (store_writer_commit(w, 0, err, sizeof err) >= 0) F->gsrc = 2;
                    store_writer_end(w);
                }
            } else free(mem);
            sb_free(&b);
            if (F->gstate > 0) { F->graph_ms = fd_ms() - t0; return &F->g; }
        } else sb_free(&b);
    }
    if (have) { F->g = g; F->gstate = 1; F->gsrc = 4; F->graph_ms = fd_ms() - t0; return &F->g; }
    return NULL;
}
static void fs_time_line(struct FsCtx *F, char *o, size_t cap) {
    static const char *const gs[] = {"none", "read from the index", "derived and saved in the index", "derived in memory only", "read (older than the files)"};
    snprintf(o, cap, "unified store: index %s%.1f ms; candidates %.1f ms; mini-index %d files %.1f ms; graph %s%s%.1f ms (no tree walk of its own)",
             F->created ? "created, " : F->walked ? "updated, " : "ready, ", F->ens_ms, F->cand_ms, g_fd.nf, F->build_ms, gs[F->gstate > 0 ? F->gsrc : 0], F->gstate > 0 ? " " : " ", F->graph_ms);
}
/* o grafo do laco do agente: so le o que o armazenamento da pasta de trabalho ja tem (nunca cria indice nem deriva grafo) */
static struct FsCtx *fs_loop(void) {
    static struct FsCtx *L; static char key[4096];
    char cwd[4096], man[4700]; struct stat sv;
    if (!getcwd(cwd, sizeof cwd) || !strcmp(cwd, "/")) return NULL;
    if (L && !strcmp(key, cwd) && L->sv_ok && !store_stale(L->sv.r)) return L;
    if (L) { sy_view_close(&L->sv); fgs_free(&L->g); free(L); L = NULL; key[0] = 0; }
    IndexLoc loc; index_resolve(cwd, 0, &loc);
    snprintf(man, sizeof man, "%s/MANIFEST", loc.store);
    if (stat(man, &sv) != 0) return NULL;
    struct FsCtx *F = (struct FsCtx *)calloc(1, sizeof *F);
    char err[300] = "";
    snprintf(F->root, sizeof F->root, "%s", cwd); F->loc = loc; F->loop = 1;
    if (sy_view_open(cwd, &F->sv, err, sizeof err) != 0) { free(F); return NULL; }
    F->sv_ok = 1; L = F; snprintf(key, sizeof key, "%s", cwd);
    return L;
}

/* ---- derivacao do grafo (N2/N3 -> STS_GRAPH) ---- */
static int fs_cmp_u64(const void *a, const void *b) { const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static int fs_cmp_u64pair(const void *a, const void *b) { const uint64_t *x = (const uint64_t *)a, *y = (const uint64_t *)b; return x[0] < y[0] ? -1 : x[0] > y[0]; }
static int fs_graph_derive(const char *root, uint64_t sig, StoreBuf *out, uint32_t *nnout) {
    const double t0 = fd_ms();
    SyView sv; char err[300] = "";
    if (sy_view_open_raw(root, &sv, err, sizeof err) != 0) return 0;
    if (!sv.any) { sy_view_free_real(&sv); return 0; }
    StoreLive *L = &sv.L;
    int *nid = (int *)malloc(sizeof(int) * ((size_t)L->n + 1)); int n = 0;
    int *eidx = (int *)malloc(sizeof(int) * ((size_t)L->n + 1)); int *kindv = (int *)malloc(sizeof(int) * ((size_t)L->n + 1));
    for (int i = 0; i < L->n; i++) {
        const StoreLiveDoc *e = &L->e[i]; nid[i] = -1;
        if (e->dead || (e->d.flags & SDF_SKIPMASK) || e->d.size > FD_MAXBYTES) continue;
        char rel[4200]; if (!fs_rel_copy(e->d.path, e->d.plen, rel, sizeof rel)) continue;
        const int k = fd_kind(rel); if (k < 0 || fd_skip_rel(rel)) continue;
        nid[i] = n; eidx[n] = i; kindv[n] = k; n++;
    }
    if (n < 2) { free(nid); free(eidx); free(kindv); sy_view_free_real(&sv); return 0; }
    FdTerms G; memset(&G, 0, sizeof G);
    FdIv ids = {0}; uint32_t *fo = (uint32_t *)calloc((size_t)n + 1, 4);
    int (*defs)[3] = NULL; uint8_t *defc = NULL; int ndefcap = 0;
    typedef struct { int file, nm; } Dn; Dn *dl = NULL; int ndl = 0, dlcap = 0;       /* (arquivo, id do nome) das definicoes de codigo */
    for (int i = 0; i < n; i++) {
        const StoreLiveDoc *e = &L->e[eidx[i]]; const SySec *x = &sv.s[e->seg].x;
        fo[i] = (uint32_t)ids.n;
        if (!x->has || e->idx >= x->docs.n) continue;
        const uint8_t *dr = store_table_at(&x->docs, e->idx);
        const uint32_t s0 = st_get_u32(dr), ns = st_get_u32(dr + 4), r0 = st_get_u32(dr + 8), nr = st_get_u32(dr + 12);
        const int code = fg_code(kindv[i]);
        for (uint32_t k = 0; k < nr; k++) {
            const uint8_t *q = store_table_at(&x->refs, r0 + k); const uint32_t rk = st_get_u32(q + 4);
            if (rk == CR_IMPORT || rk == CR_LITERAL) continue;
            uint32_t nl; const char *nm = store_strs_get(&x->names, st_get_u32(q), &nl);
            if (nl < 4 || nl > 100) continue;
            const int id = fd_term(&G, nm, (int)nl, 1); if (id >= 0) fd_iv_push(&ids, (uint32_t)id);
        }
        for (uint32_t k = 0; k < ns; k++) {
            const uint8_t *q = store_table_at(&x->syms, s0 + k);
            if ((st_get_u32(q + 40) >> 16) & CSF_DECL) continue;
            uint32_t nl; const char *nm = store_strs_get(&x->names, st_get_u32(q), &nl);
            if (nl < 4 || nl > 100) continue;
            const int id = fd_term(&G, nm, (int)nl, 1); if (id < 0) continue;
            fd_iv_push(&ids, (uint32_t)id);
            if (code && !fg_kw(nm, (int)nl)) { if (ndl == dlcap) { dlcap = dlcap ? dlcap * 2 : 1024; dl = (Dn *)realloc(dl, sizeof(Dn) * (size_t)dlcap); } dl[ndl].file = i; dl[ndl].nm = id; ndl++; }
        }
        if (ids.n > (int)fo[i]) {
            qsort(ids.v + fo[i], (size_t)(ids.n - (int)fo[i]), 4, fg_u32cmp);
            int w = (int)fo[i];
            for (int r = (int)fo[i]; r < ids.n; r++) if (r == (int)fo[i] || ids.v[r] != ids.v[r - 1]) ids.v[w++] = ids.v[r];
            ids.n = w;
        }
    }
    fo[n] = (uint32_t)ids.n;
    const int T = G.n; (void)ndefcap;
    defs = (int (*)[3])malloc(sizeof(int[3]) * ((size_t)T + 1)); defc = (uint8_t *)calloc((size_t)T + 1, 1);
    for (int d = 0; d < ndl; d++) {
        const int g = dl[d].nm, fi = dl[d].file;
        int has = 0; for (int q = 0; q < defc[g] && q < 3; q++) if (defs[g][q] == fi) has = 1;
        if (has) continue;
        if (defc[g] < 3) defs[g][defc[g]] = fi;
        if (defc[g] < 4) defc[g]++;
    }
    /* uso por texto (nivel N1, o lexico): o nome definido aparece no texto de outro arquivo (comentario, texto entre aspas, propriedade, tudo o que a analise de
       referencias nao ve e o grafo antigo via por ler o arquivo inteiro). Entram como usos a mais; a uniao (arquivo, nome) refaz as listas ids/fo */
    long nlex = 0;
    {
        CsView cv; char e3[300] = "";
        if (cs_view_open_raw(root, &cv, e3, sizeof e3) == 0) {
            int *gnode = (int *)malloc(sizeof(int) * ((size_t)cv.N + 1));
            for (uint32_t g = 0; g < cv.N; g++) { StoreLiveDoc *d = store_live_find(L, cv.path[g], cv.plen[g]); gnode[g] = d && !d->dead ? nid[d - L->e] : -1; }
            uint64_t *pr = (uint64_t *)malloc(sizeof(uint64_t) * ((size_t)ids.n + 1)); size_t npr = 0, prcap = (size_t)ids.n + 1;
            for (int i = 0; i < n; i++) for (uint32_t k = fo[i]; k < fo[i + 1]; k++) pr[npr++] = (uint64_t)i << 32 | ids.v[k];
            for (int t = 0; t < T; t++) {
                if (defc[t] < 1 || defc[t] > 3) continue;
                const char *nm = fd_tstr(&G, t); CoderTok tk[48]; char keys[6][100]; int nk = 0;
                const int ntk = coder_tokens_ex(nm, tk, 48, CF_WORD, CT_AUTO); int nw = 0;
                for (int q = 0; q < ntk; q++) if (tk[q].kind == 1) nw++;
                for (int q = 0; q < ntk; q++) if (tk[q].kind == 1 || (!nw && ntk == 1 && tk[q].kind == 0)) { fs_key_add(keys, &nk, 'n', tk[q].norm); fs_key_add(keys, &nk, 'o', tk[q].orig); }
                for (int z = 0; z < nk; z++) {
                    uint32_t m = 0; CsHit *h = cs_key_hits(&cv, keys[z], strlen(keys[z]), &m);
                    for (uint32_t q = 0; q < m; q++) {
                        const int nd = gnode[h[q].doc]; if (nd < 0 || !(h[q].tf[CF_WORD] + h[q].tf[CF_PART])) continue;
                        if (npr == prcap) { prcap *= 2; pr = (uint64_t *)realloc(pr, sizeof(uint64_t) * prcap); }
                        pr[npr++] = (uint64_t)nd << 32 | (uint32_t)t; nlex++;
                    }
                    free(h);
                }
            }
            qsort(pr, npr, sizeof(uint64_t), fs_cmp_u64);
            ids.n = 0; size_t w = 0; int cur = 0;
            for (size_t r = 0; r < npr; r++) { if (w && pr[w - 1] == pr[r]) continue; pr[w++] = pr[r]; }
            for (int i = 0; i < n; i++) fo[i] = 0;
            { size_t r = 0; for (int i = 0; i < n; i++) { fo[i] = (uint32_t)ids.n; while (r < w && (int)(pr[r] >> 32) == i) { fd_iv_push(&ids, (uint32_t)pr[r]); r++; } } (void)cur; }
            fo[n] = (uint32_t)ids.n;
            free(pr); free(gnode); cs_view_free_real(&cv);
        }
    }
    uint32_t *udf = (uint32_t *)calloc((size_t)T + 1, 4);
    for (int k = 0; k < ids.n; k++) udf[ids.v[k]]++;
    FgEv E = {0};
    const int lim = n / 50 > 30 ? n / 50 : 30;
    if (g_fd_verbose) { int nd1 = 0, nu = 0; for (int t = 0; t < T; t++) { if (defc[t] >= 1 && defc[t] <= 3) nd1++; if (defc[t] >= 1 && defc[t] <= 3 && (int)udf[t] <= lim) nu++; }
        fprintf(stderr, "find: graph: %d names (%d name-in-file pairs, %ld of them from the text index), %d with 1 to 3 definitions, %d of them usable (used in at most %d files)\n", T, ids.n, nlex, nd1, nu, lim); }
    for (int i = 0; i < n; i++) {                               /* sym: nome usado aqui e definido (em ate 3 arquivos) em outro */
        if (!fg_code(kindv[i])) continue;
        for (uint32_t k = fo[i]; k < fo[i + 1]; k++) {
            const uint32_t t = ids.v[k];
            if (defc[t] < 1 || defc[t] > 3 || (int)udf[t] > lim) continue;
            const float w = (float)log(1.0 + (double)n / udf[t]);
            for (int q = 0; q < defc[t]; q++) fg_ev_push(&E, (uint32_t)i, (uint32_t)defs[t][q], w, FG_SYM);
        }
    }
    {                                                            /* rare: nome raro (2 a 4 arquivos), com cara de identificador, que nao e definido em lugar nenhum */
        int *first = (int *)malloc(sizeof(int) * ((size_t)T + 1)); uint8_t *rare = (uint8_t *)calloc((size_t)T + 1, 1); int nr = 0;
        for (int t = 0; t < T; t++) {
            first[t] = -1;
            const char *s = fd_tstr(&G, t); const int l = (int)strlen(s);
            if (udf[t] >= 2 && udf[t] <= 4 && l >= 6 && !defc[t] && fg_identlike(s, l)) { rare[t] = 1; first[t] = nr; nr += (int)udf[t]; }
        }
        int *fill = (int *)calloc((size_t)T + 1, sizeof(int)), *fl = (int *)malloc(sizeof(int) * ((size_t)nr + 1));
        for (int i = 0; i < n; i++) for (uint32_t k = fo[i]; k < fo[i + 1]; k++) { const uint32_t t = ids.v[k]; if (rare[t]) fl[first[t] + fill[t]++] = i; }
        for (int t = 0; t < T; t++) {
            if (!rare[t]) continue;
            const int df = (int)udf[t]; const float w = (float)(log(1.0 + (double)n / df) / (df - 1));
            for (int a = 0; a < df; a++) for (int b = a + 1; b < df; b++) fg_ev_push(&E, (uint32_t)fl[first[t] + a], (uint32_t)fl[first[t] + b], w, FG_RARE);
        }
        free(first); free(rare); free(fill); free(fl);
    }
    const double tph1 = fd_ms();
    {                                                            /* path: imports resolvidos a arquivos (N3), inclusive os links de documentos */
        int *mark = (int *)calloc((size_t)n + 1, sizeof(int)); long nimp = 0, nmiss = 0;
        /* o mesmo texto de import na mesma pasta e linguagem resolve igual (kotlin e java repetem centenas): memoria de (linguagem, pasta, texto) -> arquivos */
        typedef struct { uint64_t h; int n; int doc[8]; } Imo;
        uint32_t tcap = 1024; Imo *tab = (Imo *)calloc(tcap, sizeof(Imo)); uint32_t tn = 0;
        /* conjunto dos nomes de arquivo sem extensao: um import cujo ultimo componente nao e nome de nenhum arquivo do projeto e de biblioteca ou de fora (a busca
           por sufixo no catalogo inteiro e o que custa) */
        uint32_t scap = 64; while (scap < (uint32_t)n * 4) scap *= 2; uint64_t *stems = (uint64_t *)calloc(scap, sizeof(uint64_t));
        for (int i = 0; i < n; i++) {
            const StoreLiveDoc *e = &L->e[eidx[i]]; uint32_t b = e->d.plen; while (b && e->d.path[b - 1] != '/') b--;
            uint32_t en = e->d.plen; for (uint32_t q = b; q < e->d.plen; q++) if (e->d.path[q] == '.') en = q;
            if (en == b) en = e->d.plen;
            const uint64_t h = store_hash64(e->d.path + b, en - b, 5) | 1; uint32_t z = (uint32_t)h & (scap - 1);
            while (stems[z] && stems[z] != h) z = (z + 1) & (scap - 1);
            stems[z] = h;
        }
        for (int i = 0; i < n; i++) {
            const StoreLiveDoc *e = &L->e[eidx[i]]; const SySec *x = &sv.s[e->seg].x;
            if (!x->has || e->idx >= x->docs.n) continue;
            const uint8_t *dr = store_table_at(&x->docs, e->idx);
            const uint32_t r0 = st_get_u32(dr + 8), nr = st_get_u32(dr + 12), lang = st_get_u32(dr + 16);
            uint32_t dl = e->d.plen; while (dl && e->d.path[dl - 1] != '/') dl--;
            for (uint32_t k = 0; k < nr; k++) {
                SyRef f; sy_ref_at(&sv, e->seg, r0 + k, &f); if (f.kind != CR_IMPORT) continue;
                nimp++;
                {   /* ultimo componente do texto (depois de / . :), sem * nem espacos; vazio ou com espaco = nao filtra */
                    uint32_t en = f.nl; while (en && (f.name[en - 1] == '*' || f.name[en - 1] == ';')) en--;
                    uint32_t b = en; int sp = 0; while (b && !strchr("/.:", f.name[b - 1])) { if (f.name[b - 1] == ' ') sp = 1; b--; }
                    if (!sp && en > b) {
                        const uint64_t hs = store_hash64(f.name + b, en - b, 5) | 1; uint32_t z = (uint32_t)hs & (scap - 1); int found = 0;
                        while (stems[z]) { if (stems[z] == hs) { found = 1; break; } z = (z + 1) & (scap - 1); }
                        if (!found) continue;
                    }
                }
                uint64_t h = store_hash64(e->d.path, dl, lang + 1); h = store_hash64(f.name, f.nl, h) | 1;
                if (tn * 2 >= tcap) {
                    const uint32_t nc = tcap * 2; Imo *nt = (Imo *)calloc(nc, sizeof(Imo));
                    for (uint32_t q = 0; q < tcap; q++) if (tab[q].h) { uint32_t z = (uint32_t)tab[q].h & (nc - 1); while (nt[z].h) z = (z + 1) & (nc - 1); nt[z] = tab[q]; }
                    free(tab); tab = nt; tcap = nc;
                }
                uint32_t z = (uint32_t)h & (tcap - 1);
                while (tab[z].h && tab[z].h != h) z = (z + 1) & (tcap - 1);
                if (!tab[z].h) {
                    StoreLiveDoc *m[SY_IMPCAP]; int how = 0, conf = 0; char why[100]; nmiss++;
                    const int nm = sy_resolve_imp_text(&sv, e->d.path, e->d.plen, lang, f.name, f.nl, m, SY_IMPCAP, &how, &conf, why, sizeof why);
                    tab[z].h = h; tn++; tab[z].n = 0;
                    for (int q = 0; q < nm && q < 8; q++) tab[z].doc[tab[z].n++] = (int)(m[q] - L->e);
                }
                for (int q = 0; q < tab[z].n; q++) {
                    const int j = tab[z].doc[q]; if (j < 0 || j >= L->n || nid[j] < 0 || nid[j] == i || mark[nid[j]] == i + 1) continue;
                    mark[nid[j]] = i + 1; fg_ev_push(&E, (uint32_t)i, (uint32_t)nid[j], 1.0f, FG_PATH);
                }
            }
        }
        free(tab); free(stems);
        free(mark);
        if (g_fd_verbose) fprintf(stderr, "find: graph: %ld import references, %ld resolved against the catalog (the others repeat a text already resolved in the same folder, or name no file of the project)\n", nimp, nmiss);
    }
    {                                                            /* rare, por chave de texto (N3): o mesmo texto de ligacao (evento, rota, variavel de ambiente, chave de configuracao) em 2 a 4 arquivos */
        FdTerms K; memset(&K, 0, sizeof K); uint64_t *kp = NULL; size_t nkp = 0, kcap = 0;
        for (int sgi = 0; sgi < sv.ns; sgi++) {
            const KySec *ky = &sv.s[sgi].ky; if (!ky->has) continue;
            for (uint32_t o = 0; o < ky->occ.n; o++) {
                const uint8_t *q = store_table_at(&ky->occ, o); const uint32_t doc = st_get_u32(q + 12);
                if (doc >= sv.s[sgi].sd.t.n || !sv.s[sgi].live[doc]) continue;
                StoreDoc sd; if (!store_docs_get(&sv.s[sgi].sd, doc, &sd)) continue;
                StoreLiveDoc *ld = store_live_find(L, sd.path, sd.plen); const int nd = ld && !ld->dead ? nid[ld - L->e] : -1; if (nd < 0) continue;
                uint32_t kl; const char *kn = store_strs_get(&ky->names, st_get_u32(q), &kl); if (kl < 4 || kl > 100) continue;
                const int kid = fd_term(&K, kn, (int)kl, 1); if (kid < 0) continue;
                if (nkp == kcap) { kcap = kcap ? kcap * 2 : 4096; kp = (uint64_t *)realloc(kp, sizeof(uint64_t) * kcap); }
                kp[nkp++] = (uint64_t)kid << 32 | (uint32_t)nd;
            }
        }
        qsort(kp, nkp, sizeof(uint64_t), fs_cmp_u64);
        size_t w = 0; for (size_t r = 0; r < nkp; r++) if (!w || kp[w - 1] != kp[r]) kp[w++] = kp[r];
        for (size_t a = 0; a < w; ) {
            size_t b = a; while (b < w && (kp[b] >> 32) == (kp[a] >> 32)) b++;
            const size_t df = b - a;
            if (df >= 2 && df <= 4) { const float wt = (float)(log(1.0 + (double)n / (double)df) / (double)(df - 1)); for (size_t x = a; x < b; x++) for (size_t y = x + 1; y < b; y++) fg_ev_push(&E, (uint32_t)kp[x], (uint32_t)kp[y], wt, FG_RARE); }
            a = b;
        }
        free(kp); free(K.pool); free(K.off); free(K.len); free(K.ht);
    }
    {                                                            /* path, por nome de arquivo citado com extensao no texto (o lexico guarda "grey_channel_mjs" como uma palavra so) */
        CsView cv; char e3[300] = "";
        if (cs_view_open_raw(root, &cv, e3, sizeof e3) == 0) {
            int *gnode = (int *)malloc(sizeof(int) * ((size_t)cv.N + 1));
            for (uint32_t g = 0; g < cv.N; g++) { StoreLiveDoc *d = store_live_find(L, cv.path[g], cv.plen[g]); gnode[g] = d && !d->dead ? nid[d - L->e] : -1; }
            for (int i = 0; i < n; i++) {
                const StoreLiveDoc *e = &L->e[eidx[i]]; uint32_t b = e->d.plen; while (b && e->d.path[b - 1] != '/') b--;
                const char *bn = e->d.path + b; const uint32_t bl = e->d.plen - b; const char *dot = (const char *)memchr(bn, '.', bl);
                if (!dot || dot == bn || bl > 90 || !fg_known_ext(dot + 1, (int)(bl - (uint32_t)(dot - bn) - 1))) continue;
                char t[100]; memcpy(t, bn, bl); t[bl] = 0; CoderTok tk[8]; const int ntk = coder_tokens_ex(t, tk, 8, CF_WORD, CT_AUTO);
                char key[100]; key[0] = 0; for (int q = 0; q < ntk; q++) if (tk[q].kind == 1) { key[0] = 'n'; snprintf(key + 1, sizeof key - 1, "%s", tk[q].norm); break; }
                if (!key[0]) continue;
                uint32_t m = 0; CsHit *h = cs_key_hits(&cv, key, strlen(key), &m);
                if (m <= 400) for (uint32_t q = 0; q < m; q++) { const int d = gnode[h[q].doc]; if (d >= 0 && d != i) fg_ev_push(&E, (uint32_t)d, (uint32_t)i, 1.0f, FG_PATH); }
                free(h);
            }
            free(gnode); cs_view_free_real(&cv);
        }
    }
    const double tph2 = fd_ms();
    long ncommits = 0;
    {                                                          /* co: arquivos que mudaram juntos no git (some em silencio sem git ou fora de repositorio) */
        char q[8800], cmd[9000]; fg_shq(root, q, sizeof q);
        snprintf(cmd, sizeof cmd, "git -C %s -c core.quotepath=false log --no-merges --relative --name-only --pretty=format:@@ -n %d 2>/dev/null", q, FG_GITLOG);
        FILE *gp = popen(cmd, "r");
        if (gp) {
            char *ln = (char *)malloc(8200); int cur[FG_GITMAX + 1], nc = 0, raw = 0;
            for (;;) {
                const char *got = fgets(ln, 8200, gp);
                if (got) { size_t l = strlen(ln); while (l && (ln[l - 1] == '\n' || ln[l - 1] == '\r')) ln[--l] = 0; if (!l) continue; }
                if (!got || (ln[0] == '@' && ln[1] == '@')) {
                    if (nc >= 2 && raw <= FG_GITMAX) {
                        const float w = 1.0f / (float)(nc - 1);
                        for (int a = 0; a < nc; a++) for (int b = a + 1; b < nc; b++) fg_ev_push(&E, (uint32_t)cur[a], (uint32_t)cur[b], w, FG_CO);
                        ncommits++;
                    }
                    nc = 0; raw = 0;
                    if (!got) break;
                    continue;
                }
                raw++;
                if (nc < FG_GITMAX) { StoreLiveDoc *d = store_live_find(L, ln, strlen(ln)); const int j = d ? (int)(d - L->e) : -1; if (j >= 0 && nid[j] >= 0) cur[nc++] = nid[j]; }
            }
            free(ln); pclose(gp);
        }
    }
    /* junta arestas iguais; normaliza por linha (como o grafo antigo) */
    qsort(E.v, E.n, sizeof(FgE), fg_cmp_e);
    size_t nm = 0;
    for (size_t k = 0; k < E.n; k++) {
        if (nm && E.v[nm - 1].a == E.v[k].a && E.v[nm - 1].b == E.v[k].b && E.v[nm - 1].t == E.v[k].t) E.v[nm - 1].w += E.v[k].w;
        else E.v[nm++] = E.v[k];
    }
    E.n = nm;
    float *rs = (float *)calloc(4 * (size_t)n, sizeof(float));
    long cnt_t[4] = {0, 0, 0, 0};
    for (size_t k = 0; k < nm; k++) {
        if (E.v[k].t == FG_CO && E.v[k].w > 3.0f) E.v[k].w = 3.0f;
        rs[(size_t)E.v[k].t * n + E.v[k].a] += E.v[k].w; rs[(size_t)E.v[k].t * n + E.v[k].b] += E.v[k].w; cnt_t[E.v[k].t]++;
    }
    FgD *D = (FgD *)malloc(sizeof(FgD) * (2 * nm + 1)); size_t nd = 0;
    for (size_t k = 0; k < nm; k++) {
        const FgE *e = &E.v[k];
        D[nd].s = e->a; D[nd].d = e->b; D[nd].w = (float)(fg_beta[e->t] * e->w / rs[(size_t)e->t * n + e->a]); D[nd].m = (uint8_t)(1 << e->t); nd++;
        D[nd].s = e->b; D[nd].d = e->a; D[nd].w = (float)(fg_beta[e->t] * e->w / rs[(size_t)e->t * n + e->b]); D[nd].m = (uint8_t)(1 << e->t); nd++;
    }
    qsort(D, nd, sizeof(FgD), fg_cmp_d);
    size_t nu = 0;
    for (size_t k = 0; k < nd; k++) {
        if (nu && D[nu - 1].s == D[k].s && D[nu - 1].d == D[k].d) { D[nu - 1].w += D[k].w; D[nu - 1].m |= D[k].m; }
        else D[nu++] = D[k];
    }
    double *tot = (double *)calloc((size_t)n + 1, sizeof(double));
    uint32_t *rowa = (uint32_t *)calloc((size_t)n + 1, 4), *rowb = (uint32_t *)calloc((size_t)n + 1, 4);
    for (size_t k = 0; k < nu; k++) { if (!tot[D[k].s]) rowa[D[k].s] = (uint32_t)k; tot[D[k].s] += D[k].w; rowb[D[k].s] = (uint32_t)k + 1; }
    /* nos = arquivos que tem aresta, em ordem de hash do caminho completo */
    uint64_t (*hn)[2] = (uint64_t (*)[2])malloc(sizeof(uint64_t[2]) * ((size_t)n + 1)); int nn = 0;
    for (int i = 0; i < n; i++) if (tot[i] > 0) { const StoreLiveDoc *e = &L->e[eidx[i]]; hn[nn][0] = fgs_hash(e->d.path, e->d.plen); hn[nn][1] = (uint64_t)i; nn++; }
    qsort(hn, (size_t)nn, sizeof hn[0], fs_cmp_u64pair);
    int *newidx = (int *)malloc(sizeof(int) * ((size_t)n + 1)); for (int i = 0; i < n; i++) newidx[i] = -1;
    for (int k = 0; k < nn; k++) newidx[hn[k][1]] = k;
    uint32_t nnz = 0; for (int k = 0; k < nn; k++) nnz += rowb[hn[k][1]] - rowa[hn[k][1]];
    StoreBuf *b = out; sb_u32(b, FGS_MAGIC); sb_u32(b, 1); sb_u32(b, (uint32_t)nn); sb_u32(b, nnz); sb_u32(b, (uint32_t)n); sb_u32(b, 0); sb_u64(b, sig); sb_u64(b, 0);
    for (int k = 0; k < nn; k++) sb_u64(b, hn[k][0]);
    uint32_t acc = 0; for (int k = 0; k < nn; k++) { sb_u32(b, acc); acc += rowb[hn[k][1]] - rowa[hn[k][1]]; } sb_u32(b, acc);
    for (int k = 0; k < nn; k++) for (uint32_t z = rowa[hn[k][1]]; z < rowb[hn[k][1]]; z++) sb_u32(b, (uint32_t)newidx[D[z].d]);
    for (int k = 0; k < nn; k++) for (uint32_t z = rowa[hn[k][1]]; z < rowb[hn[k][1]]; z++) { const float w = (float)(D[z].w / tot[D[z].s]); uint32_t u; memcpy(&u, &w, 4); sb_u32(b, u); }
    for (int k = 0; k < nn; k++) for (uint32_t z = rowa[hn[k][1]]; z < rowb[hn[k][1]]; z++) sb_u8(b, D[z].m);
    sb_pad8(b);
    uint32_t po = 0;
    for (int k = 0; k < nn; k++) { sb_u32(b, po); po += sv.L.e[eidx[hn[k][1]]].d.plen + 1; } sb_u32(b, po);
    for (int k = 0; k < nn; k++) { const StoreLiveDoc *e = &L->e[eidx[hn[k][1]]]; sb_bytes(b, e->d.path, e->d.plen); sb_u8(b, 0); }
    sb_pad8(b);
    *nnout = (uint32_t)nn;
    if (g_fd_verbose) fprintf(stderr, "find: graph phases: names, definitions, sym and rare edges %.0f ms; imports %.0f ms; co-change and compaction %.0f ms\n", tph1 - t0, tph2 - tph1, fd_ms() - tph2);
    if (g_fd_verbose) fprintf(stderr, "find: graph derived from symbols and references in %.0f ms: %d files, %d with edges; edges sym %ld path %ld rare %ld co %ld (%ld commits), %u directed entries, %.2f MB\n",
                              fd_ms() - t0, n, nn, cnt_t[0], cnt_t[1], cnt_t[2], cnt_t[3], ncommits, nnz, (double)b->n / 1048576.0);
    free(newidx); free(hn); free(rowa); free(rowb); free(tot); free(D); free(rs); free(E.v); free(ids.v); free(fo); free(udf); free(defs); free(defc); free(dl);
    free(G.pool); free(G.off); free(G.len); free(G.ht); free(nid); free(eidx); free(kindv);
    sy_view_free_real(&sv);
    return 1;
}
#else
#endif
