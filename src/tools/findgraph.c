/* ---------- grafo de arquivos do Find e ativacao esparsa (ver docs/tools.md e docs/investigation-engine.md) ----------
 * Nos = arquivos do indice do Find. Quatro tipos de aresta, nao dirigidas, guardadas num CSR com peso normalizado por linha:
 *   sym   identificador usado num arquivo e definido (regra de simbolos do indice) em outro; peso idf; nomes usados em mais
 *         de max(30, 2%) arquivos ou definidos em mais de 3 ficam de fora (nao criam hubs)
 *   path  um arquivo cita o caminho (nome do arquivo com extensao conhecida, import relativo, import de python) do outro
 *   rare  identificador raro (em 2 a 4 arquivos, 6+ letras, com _ ou camelCase) compartilhado; peso idf/(df-1)
 *   co    mudaram juntos no git (ultimos FG_GITLOG commits, sem os de mais de FG_GITMAX arquivos); peso 1/(n-1), teto 3; beta 0.5
 * Consulta: palavras de pedido saem da pergunta; BM25 por campos (find.c); ativacao esparsa em 2 rodadas
 * (decaimento 0.5, k vencedores); cada candidato que veio por aresta leva a evidencia. Nota de vizinhos no laco: fg_neighbor_note. */
#ifndef _WIN32
/* O grafo nao e construido aqui. Vem da secao STS_GRAPH do armazenamento do coder (coder/graph.c e coder/findstore.c),
 * derivada dos niveis N2 e N3, com nos chaveados pelo hash do caminho; este arquivo so o consulta (ativacao esparsa, evidencia, nota de vizinhos). */
typedef struct { uint32_t nn, nnz, nfiles; uint64_t sig; const uint64_t *hash; const uint32_t *rp, *dst, *poff; const float *w; const uint8_t *ty; const char *pool; uint64_t poolsz; void *mem; } FgsGraph;
static FgsGraph *fs_graph(struct FsCtx *F);                                         /* findstore.c: o grafo do contexto (abre, ou deriva se faltar) */
static uint64_t fs_hash_rel(struct FsCtx *F, const char *rel);
static const char *fs_strip(struct FsCtx *F, const char *full);                    /* caminho do armazenamento -> relativo a pasta do Find, NULL se fora */
static struct FsCtx *fs_loop(void);                                                 /* contexto leve do laco do agente: so le o que existe, nunca cria nem deriva */
static FgsGraph *g_fgs;                                                             /* grafo da consulta em curso (fg_mask, fg_evidence) */
static int fgs_find(const FgsGraph *G, uint64_t h) {
    int lo = 0, hi = (int)G->nn - 1;
    while (lo <= hi) { const int mid = (lo + hi) / 2; if (G->hash[mid] == h) return mid; if (G->hash[mid] < h) lo = mid + 1; else hi = mid - 1; }
    return -1;
}
#define FG_MAGIC 0x46524752u        /* "RGRF" */
#define FG_GITLOG 2000
#define FG_GITMAX 40
#define FG_K_FILE 40
#define FG_K_GRAPH 20
#define FG_ROUNDS 2
#define FG_DECAY 0.5
#define FG_THETA 0.15
#define FG_NEIGH_MIN 0.06
#define FG_NOTES_MAX 8
enum { FG_SYM, FG_PATH, FG_RARE, FG_CO };
static const double fg_beta[4] = {1.0, 1.0, 1.0, 0.5};


/* ---- arestas brutas ---- */
typedef struct { uint32_t a, b; float w; uint8_t t; } FgE;
typedef struct { FgE *v; size_t n, cap; } FgEv;
static void fg_ev_push(FgEv *e, uint32_t a, uint32_t b, float w, uint8_t t) {
    if (a == b) return;
    if (a > b) { const uint32_t x = a; a = b; b = x; }
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 1 << 16; e->v = (FgE *)realloc(e->v, sizeof(FgE) * e->cap); }
    e->v[e->n].a = a; e->v[e->n].b = b; e->v[e->n].w = w; e->v[e->n].t = t; e->n++;
}
static int fg_cmp_e(const void *x, const void *y) {
    const FgE *p = (const FgE *)x, *q = (const FgE *)y;
    if (p->a != q->a) return p->a < q->a ? -1 : 1;
    if (p->b != q->b) return p->b < q->b ? -1 : 1;
    return (int)p->t - (int)q->t;
}
typedef struct { uint32_t s, d; float w; uint8_t m; } FgD;
static int fg_cmp_d(const void *x, const void *y) {
    const FgD *p = (const FgD *)x, *q = (const FgD *)y;
    if (p->s != q->s) return p->s < q->s ? -1 : 1;
    return p->d < q->d ? -1 : p->d > q->d;
}

/* ---- texto: identificadores, caminhos citados ---- */
static int fg_code(int kind) { return kind != FK_MD && kind != FK_OTHER; }
static int fg_kw(const char *w, int l) {
    static const char *const k[] = {"self", "this", "that", "true", "false", "null", "none", "return", "static", "const", "void", "char", "else", "elif",
        "while", "break", "continue", "struct", "class", "import", "export", "from", "function", "async", "await", "default", "public", "private",
        "protected", "final", "override", "string", "bool", "size_t", "uint", "int32", "int64", "uint8", "uint32", "uint64", "float", "double", "main",
        "init", "data", "name", "type", "value", "text", "path", "file", "line", "list", "item", "args", "result", "error", "test", NULL};
    char b[16]; if (l >= 16) return 0;
    for (int i = 0; i < l; i++) b[i] = (char)tolower((unsigned char)w[i]);
    b[l] = 0;
    for (int i = 0; k[i]; i++) if (!strcmp(k[i], b)) return 1;
    return 0;
}
static int fg_identlike(const char *t, int l) {
    int up = 0, lo = 0, us = 0, camel = 0;
    for (int i = 0; i < l; i++) {
        if (isupper((unsigned char)t[i])) up++; else if (islower((unsigned char)t[i])) lo++;
        if (t[i] == '_' && i > 0 && i < l - 1) us = 1;
        if (i > 0 && islower((unsigned char)t[i - 1]) && isupper((unsigned char)t[i])) camel = 1;
    }
    return us || camel || (up > 0 && lo == 0 && l >= 6);
}
static int fg_u32cmp(const void *a, const void *b) { const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
/* ids unicos dos identificadores [A-Za-z_][A-Za-z0-9_]{3,} do texto, em T (raw); ordenados */
static void fg_idents(FdTerms *T, const char *s, size_t n, FdIv *out, int add) {
    const int base = out->n;
    for (size_t i = 0; i < n; ) {
        if (!(isalpha((unsigned char)s[i]) || s[i] == '_')) { i++; continue; }
        size_t j = i + 1;
        while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
        if (j - i >= 4 && j - i <= 100) { const int id = fd_term(T, s + i, (int)(j - i), add); if (id >= 0) fd_iv_push(out, (uint32_t)id); }
        i = j;
    }
    if (out->n > base) {
        qsort(out->v + base, (size_t)(out->n - base), 4, fg_u32cmp);
        int w = base;
        for (int r = base; r < out->n; r++) if (r == base || out->v[r] != out->v[r - 1]) out->v[w++] = out->v[r];
        out->n = w;
    }
}
static int fg_known_ext(const char *e, int l) {
    static const char *const x[] = {"mjs", "cjs", "js", "ts", "tsx", "py", "sh", "c", "h", "cc", "cpp", "hpp", "json", "kt", "java", "qml", "md", "toml",
        "yml", "yaml", "go", "rs", "bat", "ps1", "cmake", "glsl", "comp", "swift", "m", "txt", "xml", "gradle", NULL};
    for (int i = 0; x[i]; i++) if ((int)strlen(x[i]) == l && !memcmp(x[i], e, (size_t)l)) return 1;
    return 0;
}

static void fg_shq(const char *s, char *o, size_t cap) {      /* entre aspas simples para o sh */
    size_t k = 0; o[k++] = '\'';
    for (; *s && k + 6 < cap; s++) { if (*s == '\'') { memcpy(o + k, "'\\''", 4); k += 4; } else o[k++] = *s; }
    o[k++] = '\''; o[k] = 0;
}
/* o grafo do armazenamento existe para este indice? */
static int fg_ensure(FdIndex *X) { return fs_graph(X->fs) != NULL; }

/* ---- palavras de pedido (nao dizem onde olhar): fora da pergunta antes do ranking ---- */
static int fg_is_request(const char *term) {
    static char st[96][24]; static int nst;
    if (!nst) {
        static const char *const w[] = {"descubra", "descobrir", "diga", "dizer", "explique", "explicar", "altere", "alterar", "mude", "mudar", "nada",
            "precisa", "precisar", "feito", "fazer", "faz", "funciona", "funcionar", "funcionando", "aparece", "aparecer", "acontece", "porque", "motivo",
            "causa", "onde", "quando", "qual", "quais", "projeto", "codigo", "arquivo", "problema", "erro", "corrija", "corrigir", "veja", "investigue",
            "find", "explain", "tell", "why", "what", "where", "which", "change", "fix", "project", "code", "file", "problem", "issue", "happen", "happens",
            "work", "works", "working", "does", "should", "need", "needs", NULL};
        for (int i = 0; w[i] && nst < 96; i++) { char b[24]; snprintf(b, sizeof b, "%s", w[i]); const int l = fd_stem(b, (int)strlen(b)); memcpy(st[nst], b, (size_t)l); st[nst][l] = 0; nst++; }
    }
    for (int i = 0; i < nst; i++) if (!strcmp(st[i], term)) return 1;
    return 0;
}
/* tira de qid/qw os termos de pedido; se sobrar nada, mantem tudo */
static int fg_drop_request(FdIndex *X, uint32_t *qid, double *qw, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) if (!fg_is_request(fd_tstr(&X->T, (int)qid[i]))) { qid[w] = qid[i]; qw[w] = qw[i]; w++; }
    return w ? w : n;
}

/* ---- evidencia: por que o arquivo dst esta ligado ao arquivo src ---- */
typedef struct { int kind; char name[100]; int line; } FgWhy;    /* 1 simbolo, 2 dst cita o caminho de src, 3 src cita o caminho de dst, 4 identificador comum, 5 co-mudanca */
static uint8_t fg_mask(const FdIndex *X, int src, int dst) {
    const FgsGraph *G = g_fgs; const int a = X->f[src].node, b = X->f[dst].node;
    if (!G || a < 0 || b < 0) return 0;
    for (uint32_t e = G->rp[a]; e < G->rp[a + 1]; e++) if ((int)G->dst[e] == b) return G->ty[e];
    return 0;
}
static int fg_word_line(const FdText *t, const char *w, int wl) {
    for (int ln = 1; ln <= t->ls.n; ln++) { int ll = 0; const char *s = fd_line(t, ln, &ll); if (fd_has_word(s, ll, w, wl, 0)) return ln; }
    return 0;
}
/* simbolo definido em def e citado no texto de use (o de menor df) */
static int fg_sym_between(FdIndex *X, int def, const FdText *tu, char *name, size_t cap, int *dline) {
    int best = -1; long bdf = 1L << 40;
    for (int s = X->fs0[def]; s < X->fs0[def] + X->fsn[def] && s < X->fs0[def] + 400; s++) {
        const char *nm = fd_tstr(&X->T, X->sn[s]); const int l = (int)strlen(nm);
        if (l < 4 || fg_kw(nm, l) || !fd_has_word(tu->s, (int)tu->n, nm, l, 0)) continue;
        const long df = fs_name_refs(X->fs, nm, l);        /* quantas referencias o indice de simbolos tem; so os candidatos nao dizem se o nome e raro */
        if (df < bdf) { bdf = df; best = s; }
    }
    if (best < 0) return 0;
    snprintf(name, cap, "%s", fd_tstr(&X->T, X->sn[best])); *dline = X->sl[best];
    return 1;
}
static void fg_why(FdIndex *X, int src, int dst, FgWhy *w) {
    memset(w, 0, sizeof *w); w->kind = 5;
    const uint8_t m = fg_mask(X, src, dst);
    FdText ts = {0}, td = {0};
    const int hs = (m & (1 << FG_SYM | 1 << FG_PATH | 1 << FG_RARE)) ? fd_text(X, src, &ts) : 0;
    const int hd = hs ? fd_text(X, dst, &td) : 0;
    if (hs && hd) {
        if ((m & (1 << FG_SYM))) {
            int dl = 0;
            if (fg_sym_between(X, dst, &ts, w->name, sizeof w->name, &dl)) { w->kind = 1; w->line = dl; goto done; }
            if (fg_sym_between(X, src, &td, w->name, sizeof w->name, &dl)) { w->kind = 1; w->line = fg_word_line(&td, w->name, (int)strlen(w->name)); goto done; }
        }
        if ((m & (1 << FG_PATH))) {
            const char *bs = strrchr(X->f[src].rel, '/'); bs = bs ? bs + 1 : X->f[src].rel;
            const char *bd = strrchr(X->f[dst].rel, '/'); bd = bd ? bd + 1 : X->f[dst].rel;
            if (memmem(td.s, td.n, bs, strlen(bs))) { w->kind = 2; goto done; }
            if (memmem(ts.s, ts.n, bd, strlen(bd))) { w->kind = 3; goto done; }
            w->kind = 2; goto done;        /* import relativo sem o nome com extensao: vale para o lado que citou */
        }
        if ((m & (1 << FG_RARE))) {
            FdTerms G; memset(&G, 0, sizeof G); FdIv a = {0}, b = {0};
            fg_idents(&G, ts.s, ts.n, &a, 1); fg_idents(&G, td.s, td.n, &b, 1);
            int best = -1, bl = 0;
            for (int i = 0, j = 0; i < a.n && j < b.n; ) {
                if (a.v[i] < b.v[j]) i++; else if (a.v[i] > b.v[j]) j++;
                else { const char *s = fd_tstr(&G, (int)a.v[i]); const int l = (int)strlen(s); if (l >= 6 && fg_identlike(s, l) && l > bl) { bl = l; best = (int)a.v[i]; } i++; j++; }
            }
            if (best >= 0) { snprintf(w->name, sizeof w->name, "%s", fd_tstr(&G, best)); w->kind = 4; }
            free(a.v); free(b.v); free(G.pool); free(G.off); free(G.len); free(G.ht);
        }
    }
done:
    if (hs) fd_text_free(&ts);
    if (hd) fd_text_free(&td);
}
/* frase curta; src = arquivo de onde se chegou */
static void fg_why_str(const FgWhy *w, const char *src, char *o, size_t cap) {
    switch (w->kind) {
        case 1: snprintf(o, cap, "via %s (symbol %s)", src, w->name); break;
        case 2: snprintf(o, cap, "cites the path of %s", src); break;
        case 3: snprintf(o, cap, "its path is cited by %s", src); break;
        case 4: snprintf(o, cap, "shares identifier %s with %s", w->name, src); break;
        default: snprintf(o, cap, "changes together with %s", src); break;
    }
}

/* ---- ativacao esparsa: de all[] (BM25 por arquivo, decrescente) para hits[]; esrc[i] = arquivo de origem se veio por aresta, senao -1 ---- */
typedef struct { int i; double v; } FgV;
static int fg_cmp_v(const void *a, const void *b) { const double x = ((const FgV *)a)->v, y = ((const FgV *)b)->v; return x < y ? 1 : x > y ? -1 : ((const FgV *)a)->i - ((const FgV *)b)->i; }
/* ---- ativacao no grafo do armazenamento (modo store). Os nos sao do grafo inteiro; os arquivos do indice de trechos (X) sao so os candidatos.
 * Os vizinhos que entram na saida (e os arquivos de onde vieram) sao lidos e acrescentados a X, para ter trecho, simbolos e evidencia ---- */
typedef struct { int node, file; double v; int srcnode; } FgsOut;
static int fgs_cmp_out(const void *a, const void *b) {
    const FgsOut *x = (const FgsOut *)a, *y = (const FgsOut *)b;
    if (x->v != y->v) return x->v < y->v ? 1 : -1;
    const int fx = x->file >= 0 ? x->file : 1 << 30, fy = y->file >= 0 ? y->file : 1 << 30;
    return fx != fy ? fx - fy : x->node - y->node;
}
static void fgs_bind(FdIndex *X, FgsGraph *G) {
    for (int i = 0; i < X->nf; i++) X->f[i].node = fgs_find(G, fs_hash_rel(X->fs, X->f[i].rel));
}
static int fgs_file_of(const FdIndex *X, const char *rel) { for (int i = 0; i < X->nf; i++) if (!strcmp(X->f[i].rel, rel)) return i; return -1; }
static int fgs_activate(FdIndex *X, const FdHit *all, int na, const char *pre, int k, FdHit *hits, int *esrc) {
    FgsGraph *G = fs_graph(X->fs);
    if (!G) { const int r = na < k ? na : k; memcpy(hits, all, sizeof(FdHit) * (size_t)r); return r; }
    g_fgs = G; fgs_bind(X, G);
    const int nf = X->nf; const uint32_t nn = G->nn;
    double *act = (double *)calloc((size_t)nn + 1, sizeof(double)), *nx = (double *)calloc((size_t)nn + 1, sizeof(double)), *nb = (double *)calloc((size_t)nn + 1, sizeof(double));
    int *src = (int *)malloc(sizeof(int) * ((size_t)nn + 1)), *nsrc = (int *)malloc(sizeof(int) * ((size_t)nn + 1)), *n2f = (int *)malloc(sizeof(int) * ((size_t)nn + 1));
    int *touched = (int *)malloc(sizeof(int) * ((size_t)nn + 1)), *alist = (int *)malloc(sizeof(int) * ((size_t)nn + 1)); int nal = 0;
    for (uint32_t i = 0; i < nn; i++) { src[i] = -1; n2f[i] = -1; }
    for (int i = 0; i < nf; i++) if (X->f[i].node >= 0) n2f[X->f[i].node] = i;
    double *xact = (double *)calloc((size_t)nf + 1, sizeof(double)); int *chunk = (int *)malloc(sizeof(int) * ((size_t)nf + 1));
    for (int i = 0; i < nf; i++) chunk[i] = -1;
    FgV *cur = (FgV *)malloc(sizeof(FgV) * (size_t)(FG_K_FILE + 1)); int ncur = 0;
    const int top = na < FG_K_FILE ? na : FG_K_FILE;
    const double mx = na ? all[0].score : 1;
    for (int i = 0; i < top; i++) {
        const double x = all[i].score / mx;
        if (x < FG_THETA) continue;
        const int fi = all[i].file, nd = X->f[fi].node;
        chunk[fi] = all[i].chunk;
        if (nd >= 0) { act[nd] = x; alist[nal++] = nd; cur[ncur].i = nd; cur[ncur].v = x; ncur++; } else xact[fi] = x;
    }
    for (int r = 0; r < FG_ROUNDS && ncur; r++) {
        qsort(cur, (size_t)ncur, sizeof(FgV), fg_cmp_v);
        const int use = ncur < FG_K_GRAPH ? ncur : FG_K_GRAPH; int nt = 0;
        for (int c = 0; c < use; c++) {
            const int i = cur[c].i;
            for (uint32_t e = G->rp[i]; e < G->rp[i + 1]; e++) {
                const int j = (int)G->dst[e]; const double y = FG_DECAY * cur[c].v * G->w[e];
                if (nx[j] == 0 && nb[j] == 0) touched[nt++] = j;
                if (y > nb[j]) { nb[j] = y; nsrc[j] = i; }
                nx[j] += y;
            }
        }
        FgV *tv = (FgV *)malloc(sizeof(FgV) * (size_t)(nt + 1));
        for (int c = 0; c < nt; c++) { tv[c].i = touched[c]; tv[c].v = nx[touched[c]]; }
        qsort(tv, (size_t)nt, sizeof(FgV), fg_cmp_v);
        ncur = 0;
        for (int c = 0; c < nt && c < FG_K_GRAPH; c++) {
            const int j = tv[c].i; const double y = tv[c].v;
            if (act[j] == 0) { cur[ncur].i = j; cur[ncur].v = y; ncur++; src[j] = nsrc[j]; alist[nal++] = j; }
            act[j] += y;
        }
        for (int c = 0; c < nt; c++) { nx[touched[c]] = 0; nb[touched[c]] = 0; }
        free(tv);
    }
    double amax = 0;
    for (int c = 0; c < nal; c++) if (act[alist[c]] > amax) amax = act[alist[c]];
    for (int i = 0; i < nf; i++) if (xact[i] > amax) amax = xact[i];
    FgsOut *out = (FgsOut *)malloc(sizeof(FgsOut) * ((size_t)nal + (size_t)nf + 1)); int nout = 0;
    if (amax > 0) {
        for (int c = 0; c < nal; c++) {
            const int j = alist[c]; if (act[j] < FG_THETA * amax) continue;
            const char *rel = fs_strip(X->fs, G->pool + G->poff[j]);
            if (!rel || !fd_in_path(rel, pre)) continue;
            out[nout].node = j; out[nout].file = n2f[j]; out[nout].v = act[j]; out[nout].srcnode = n2f[j] >= 0 && chunk[n2f[j]] >= 0 ? -1 : src[j]; nout++;
        }
        for (int i = 0; i < nf; i++) if (xact[i] >= FG_THETA * amax && fd_in_path(X->f[i].rel, pre)) { out[nout].node = -1; out[nout].file = i; out[nout].v = xact[i]; out[nout].srcnode = -1; nout++; }
    }
    qsort(out, (size_t)nout, sizeof(FgsOut), fgs_cmp_out);
    if (getenv("KATANA_FIND_DEBUG")) for (int c = 0; c < nout && c < 10; c++) { const char *rel = out[c].file >= 0 ? X->f[out[c].file].rel : fs_strip(X->fs, G->pool + G->poff[out[c].node]); fprintf(stderr, "find-debug: activation %d %-50.50s %.3f%s\n", c + 1, rel ? rel : "?", out[c].v, out[c].srcnode >= 0 ? " (by edge)" : ""); }
    const int want = nout < k ? nout : k;
    /* arquivos que ainda nao estao em X: os vizinhos escolhidos e as origens deles */
    static char add[2 * FD_HITS_MAX + 4][300]; int nadd = 0;
    for (int c = 0; c < want; c++) {
        const int cand[2] = {out[c].file < 0 ? out[c].node : -1, out[c].srcnode};
        for (int z = 0; z < 2; z++) {
            const int nd = cand[z]; if (nd < 0 || n2f[nd] >= 0 || nadd >= (int)(sizeof add / sizeof add[0])) continue;
            const char *rel = fs_strip(X->fs, G->pool + G->poff[nd]); if (!rel) continue;
            int dup = 0; for (int q = 0; q < nadd; q++) if (!strcmp(add[q], rel)) dup = 1;
            if (!dup) snprintf(add[nadd++], sizeof add[0], "%s", rel);
        }
    }
    if (nadd) { fs_add_files(X->fs, X, add, nadd); fgs_bind(X, G); }
    int r = 0;
    for (int c = 0; c < want; c++) {
        int fi = out[c].file;
        if (fi < 0) { const char *rel = fs_strip(X->fs, G->pool + G->poff[out[c].node]); fi = rel ? fgs_file_of(X, rel) : -1; }
        if (fi < 0) continue;                                   /* sumiu do disco ou ilegivel */
        int es = -1;
        if (out[c].srcnode >= 0 && out[c].file < 0) { const char *rel = fs_strip(X->fs, G->pool + G->poff[out[c].srcnode]); es = rel ? fgs_file_of(X, rel) : -1; }
        else if (out[c].srcnode >= 0 && out[c].file >= 0 && chunk[out[c].file] < 0) { const char *rel = fs_strip(X->fs, G->pool + G->poff[out[c].srcnode]); es = rel ? fgs_file_of(X, rel) : -1; }
        hits[r].file = fi; hits[r].score = out[c].v;
        hits[r].chunk = fi < nf && chunk[fi] >= 0 ? chunk[fi] : X->fc0[fi];
        esrc[r] = fi < nf && chunk[fi] >= 0 ? -1 : es;
        r++;
    }
    free(act); free(nx); free(nb); free(src); free(nsrc); free(n2f); free(touched); free(alist); free(xact); free(chunk); free(cur); free(out);
    return r;
}
static int fg_activate(FdIndex *X, const FdHit *all, int na, const char *pre, int k, FdHit *hits, int *esrc) {
    return fgs_activate(X, all, na, pre, k, hits, esrc);
}
static int fg_chunk_at(FdIndex *X, int fi, int ln, int cur) {
    if (ln <= 0) return cur;
    for (int c = X->fc0[fi]; c < X->fc0[fi] + X->fcn[fi]; c++) if (X->ca[c] <= ln && ln <= X->cb[c]) return c;
    return cur;
}
/* evidencia de h (que entrou por aresta vinda de src); ajusta o trecho mostrado para o da linha do simbolo */
static void fg_evidence(FdIndex *X, int src, FdHit *h, char *why, size_t cap) {
    FgWhy w; fg_why(X, src, h->file, &w);
    fg_why_str(&w, X->f[src].rel, why, cap);
    h->chunk = fg_chunk_at(X, h->file, w.line, h->chunk);
}
/* trecho de fi que contem a linha ln (senao o atual) */

/* ---- nota de vizinhos ainda nao lidos (laco do agente) ---- */
static int g_fg_notes;
static void fd_state_reset(void);
static uint64_t g_fgs_read[1024], g_fgs_cited[1024]; static int g_fgs_nread, g_fgs_ncited;       /* arquivos lidos e ja citados no pedido, por hash do caminho */
static void fg_reset(void) { fd_state_reset(); g_fg_notes = 0; g_fgs_nread = g_fgs_ncited = 0; }
/* ---- nota de vizinhos: le o grafo do armazenamento (nunca cria nem deriva); o porque de cada ligacao vem dos dois arquivos, lidos agora ---- */
static int fgs_in(const uint64_t *a, int n, uint64_t h) { for (int i = 0; i < n; i++) if (a[i] == h) return 1; return 0; }
static void fgs_put(uint64_t *a, int *n, int cap, uint64_t h) { if (!fgs_in(a, *n, h) && *n < cap) a[(*n)++] = h; }
static void fgs_why_pair(struct FsCtx *F, FgsGraph *G, const char *root, int n0, int n1, FgWhy *w) {
    memset(w, 0, sizeof *w); w->kind = 5;
    const char *rel[2] = {fs_strip(F, G->pool + G->poff[n0]), fs_strip(F, G->pool + G->poff[n1])};
    if (!rel[0] || !rel[1]) return;
    FdIndex P; memset(&P, 0, sizeof P); P.fs = F; snprintf(P.root, sizeof P.root, "%s", root);
    P.f = (FdFile *)calloc(2, sizeof(FdFile)); P.capf = 2;
    int ok = 1;
    for (int z = 0; z < 2 && ok; z++) {
        char abs[8400]; struct stat st; snprintf(abs, sizeof abs, "%s/%s", root, rel[z]);
        if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode)) { ok = 0; break; }
        FdFile *f = &P.f[z]; f->rel = strdup(rel[z]); f->mt = (long long)st.st_mtime; f->size = (long long)st.st_size; f->kind = fd_kind(rel[z]); f->node = z ? n1 : n0;
        P.nf = z + 1; fd_build_file(&P, f);
    }
    if (ok) { fd_invert(&P); g_fgs = G; fg_why(&P, 0, 1, w); }
    fd_store_reset(&P);
    free(P.T.pool); free(P.T.off); free(P.T.len); free(P.T.ht);
}
typedef struct { int node; double sc, bw; int from; } FgsCand;
static void fgs_neighbor_note(const char *name, const char *js, const char *end, char *result, size_t cap) {
    const int isread = !strcmp(name, "Read"), isgrep = !strcmp(name, "Grep"), isfind = !strcmp(name, "Find");
    if (!isread && !isgrep && !isfind) return;
    struct FsCtx *F = fs_loop(); if (!F) return;
    FgsGraph *G = fs_graph(F); if (!G || !G->nn) return;
    char cwd[4096]; if (!getcwd(cwd, sizeof cwd)) return;
    const size_t cl = strlen(cwd);
    int srcs[4], ns = 0; uint64_t shown[16]; int nshown = 0;
    if (isread) {
        char *fp = ct_str(js, end, "file_path");
        const long long lim = ct_int(js, end, "limit", 0);
        const char *r = fp ? fp : "";
        if (r[0] == '/') { if (strncmp(r, cwd, cl) != 0 || r[cl] != '/') { free(fp); return; } r += cl + 1; }
        while (r[0] == '.' && r[1] == '/') r += 2;
        const uint64_t h = *r ? fs_hash_rel(F, r) : 0; const int nd = *r ? fgs_find(G, h) : -1;
        free(fp);
        if (!*r) return;
        const int seen = fgs_in(g_fgs_read, g_fgs_nread, h);
        fgs_put(g_fgs_read, &g_fgs_nread, 1024, h);
        if (nd < 0 || (seen && lim > 0 && lim <= 80)) return;
        srcs[ns++] = nd; fgs_put(shown, &nshown, 16, h);
    } else {
        int nl = 0;
        for (const char *p = result; *p && nl < 300; nl++) {
            const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
            const char *s0 = p;
            while (s0 < e && isdigit((unsigned char)*s0)) s0++;
            if (s0 > p && s0 + 1 < e && s0[0] == '.' && s0[1] == ' ') s0 += 2; else s0 = p;
            const char *t = s0; while (t < e && *t != ':' && *t != ' ' && *t != '\t') t++;
            if (t > s0 && (size_t)(t - s0) < 4000) {
                char b[4100]; memcpy(b, s0, (size_t)(t - s0)); b[t - s0] = 0;
                const char *r = b;
                if (r[0] == '/') { if (strncmp(r, cwd, cl) == 0 && r[cl] == '/') r += cl + 1; else r = NULL; }
                if (r) {
                    while (r[0] == '.' && r[1] == '/') r += 2;
                    const uint64_t h = fs_hash_rel(F, r); const int nd = fgs_find(G, h);
                    if (nd >= 0 || strchr(r, '.')) fgs_put(shown, &nshown, 16, h);
                    if (nd >= 0) { int dup = 0; for (int q = 0; q < ns; q++) if (srcs[q] == nd) dup = 1; if (!dup && ns < 3) srcs[ns++] = nd; }
                }
            }
            p = *e ? e + 1 : e;
        }
    }
    if (!ns) return;
    FgsCand cand[256]; int nc = 0;
    for (int q = 0; q < ns; q++) {
        const int i = srcs[q];
        for (uint32_t e = G->rp[i]; e < G->rp[i + 1]; e++) {
            const int j = (int)G->dst[e];
            if (fgs_in(shown, nshown, G->hash[j]) || fgs_in(g_fgs_read, g_fgs_nread, G->hash[j]) || fgs_in(g_fgs_cited, g_fgs_ncited, G->hash[j])) continue;
            const char *rel = fs_strip(F, G->pool + G->poff[j]); if (!rel) continue;
            if (isfind && strstr(result, rel)) continue;
            int at = -1; for (int c = 0; c < nc; c++) if (cand[c].node == j) at = c;
            if (at < 0) { if (nc >= 256) continue; at = nc++; cand[at].node = j; cand[at].sc = 0; cand[at].bw = 0; cand[at].from = i; }
            if (G->w[e] > cand[at].bw) { cand[at].bw = G->w[e]; cand[at].from = i; }
            cand[at].sc += G->w[e];
        }
    }
    FgV pick[256]; int np = 0;
    for (int c = 0; c < nc; c++) if (cand[c].sc >= FG_NEIGH_MIN) { pick[np].i = c; pick[np].v = cand[c].sc; np++; }
    qsort(pick, (size_t)np, sizeof(FgV), fg_cmp_v);
    char li[1100 - sizeof KN_LINKS_HEAD + 1]; size_t nl = 0;      /* so a lista; o cabecalho e o ] saem da tabela (agent/knote.c) com o mesmo tamanho total de antes */
    int put = 0;
    for (int c = 0; c < np && put < 3; c++) {
        const FgsCand *K = &cand[pick[c].i]; const int j = K->node, s0 = K->from;
        FgWhy w; fgs_why_pair(F, G, cwd, s0, j, &w);
        const char *rs = fs_strip(F, G->pool + G->poff[s0]), *rj = fs_strip(F, G->pool + G->poff[j]);
        const char *bs = strrchr(rs, '/'); bs = bs ? bs + 1 : rs;
        char ph[300]; fg_why_str(&w, bs, ph, sizeof ph);
        const int add = snprintf(li + nl, sizeof li - nl - 3, "%s%s (%s)", put ? ", " : "", rj, ph);
        if (add <= 0 || nl + (size_t)add + 3 >= sizeof li) break;
        nl += (size_t)add; put++; fgs_put(g_fgs_cited, &g_fgs_ncited, 1024, G->hash[j]);
    }
    if (put) {
        char note[1100]; li[nl] = 0;
        nl = KNOTE(LINKS, note, sizeof note, li);      /* 0 = recusada pelo orcamento (agent/knote.c) */
        const size_t rl = strlen(result);
        if (nl && rl + nl + 1 < cap) { memcpy(result + rl, note, nl + 1); g_fg_notes++; }
    }
}
static void fg_neighbor_note(const char *name, const char *js, const char *end, char *result, size_t cap) {
    if (g_fg_notes >= FG_NOTES_MAX) return;
    fgs_neighbor_note(name, js, end, result, cap);
}
#else
static void fg_reset(void) {}
static void fg_neighbor_note(const char *name, const char *js, const char *end, char *result, size_t cap) { (void)name; (void)js; (void)end; (void)result; (void)cap; }
#endif
