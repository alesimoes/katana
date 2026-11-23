/* ---------- grafo de arquivos do Find e ativacao esparsa (ver docs/tools.md e docs/investigation-engine.md) ----------
 * Nos = arquivos do indice do Find. Quatro tipos de aresta, nao dirigidas, guardadas num CSR com peso normalizado por linha:
 *   sym   identificador usado num arquivo e definido (regra de simbolos do indice) em outro; peso idf; nomes usados em mais
 *         de max(30, 2%) arquivos ou definidos em mais de 3 ficam de fora (nao criam hubs)
 *   path  um arquivo cita o caminho (nome do arquivo com extensao conhecida, import relativo, import de python) do outro
 *   rare  identificador raro (em 2 a 4 arquivos, 6+ letras, com _ ou camelCase) compartilhado; peso idf/(df-1)
 *   co    mudaram juntos no git (ultimos FG_GITLOG commits, sem os de mais de FG_GITMAX arquivos); peso 1/(n-1), teto 3; beta 0.5
 * O grafo e reconstruido inteiro quando o indice muda (uma leitura de todos os arquivos mais um `git log`) e vai no mesmo
 * arquivo do indice. Consulta: palavras de pedido saem da pergunta; BM25 por campos (find.c); ativacao esparsa em 2 rodadas
 * (decaimento 0.5, k vencedores); cada candidato que veio por aresta leva a evidencia. Nota de vizinhos no laco: fg_neighbor_note.
 * KATANA_FIND_ACT=0 volta ao ranking antigo; KATANA_NEIGHBORS=0 desliga a nota; KATANA_FIND_CO=0 nao le o git. */
#ifndef _WIN32
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

typedef struct { int n, nnz; int *rp; int *dst; float *w; uint8_t *ty; int ready; } FgGraph;
static FgGraph g_fg;

static int fg_env_on(const char *name) { const char *e = getenv(name); return !(e && atoi(e) == 0 && *e == '0'); }
static int fg_act_on(void) { return fg_env_on("KATANA_FIND_ACT"); }
static void fg_free(void) { free(g_fg.rp); free(g_fg.dst); free(g_fg.w); free(g_fg.ty); memset(&g_fg, 0, sizeof g_fg); }

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
static int fg_pc(int c) { return isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/'; }
/* dir + "/" + m normalizado (. e ..) */
static int fg_norm(char *out, size_t cap, const char *dir, int dl, const char *m, int ml) {
    char tmp[1100]; int n = 0;
    if (dl + ml + 2 >= (int)sizeof tmp) return 0;
    if (dl) { memcpy(tmp, dir, (size_t)dl); n = dl; tmp[n++] = '/'; }
    memcpy(tmp + n, m, (size_t)ml); n += ml; tmp[n] = 0;
    char *segs[64]; int ns = 0; char *p = tmp;
    while (*p) {
        char *sl = strchr(p, '/'); if (sl) *sl = 0;
        if (!strcmp(p, "..")) { if (ns) ns--; else return 0; }
        else if (*p && strcmp(p, ".")) { if (ns >= 64) return 0; segs[ns++] = p; }
        if (!sl) break;
        p = sl + 1;
    }
    size_t o = 0;
    for (int i = 0; i < ns; i++) { const size_t l = strlen(segs[i]); if (o + l + 2 >= cap) return 0; if (i) out[o++] = '/'; memcpy(out + o, segs[i], l); o += l; }
    out[o] = 0;
    return o > 0;
}
static int fg_stem_cut(const char *p, int l) {      /* comprimento sem a extensao */
    int sl = 0; for (int i = 0; i < l; i++) if (p[i] == '/') sl = i + 1;
    for (int i = l - 1; i > sl; i--) if (p[i] == '.') return (l - i - 1 <= 5) ? i : l;
    return l;
}

typedef struct { FdTerms B, S; int *bh, *bn, *sh, *sn; } FgPaths;
static int fg_chain_add(FdTerms *T, int *head, int *next, const char *s, int l, int file) {
    const int id = fd_term(T, s, l, 1);
    if (id < 0 || id >= T->n) return 0;
    next[file] = head[id]; head[id] = file;
    return 1;
}
static void fg_target(int *tg, int *ntg, int cap, int *mark, int self, int j, int stamp) {
    if (j == self || mark[j] == stamp || *ntg >= cap) return;
    mark[j] = stamp; tg[(*ntg)++] = j;
}
/* arquivos de uma cadeia (no maximo lim; mais que isso = nome comum demais) */
static int fg_chain_files(FdTerms *T, const int *head, const int *next, const char *s, int l, int *out, int lim) {
    const int id = fd_term(T, s, l, 0);
    if (id < 0) return 0;
    int n = 0;
    for (int f = head[id]; f >= 0; f = next[f]) { if (n >= lim) return 0; out[n++] = f; }
    return n;
}

/* ---- construcao ---- */
static int fg_find_rel(FdIndex *X, const char *rel) {
    int lo = 0, hi = X->nf - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2, c = strcmp(X->f[mid].rel, rel);
        if (!c) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
static void fg_shq(const char *s, char *o, size_t cap) {      /* entre aspas simples para o sh */
    size_t k = 0; o[k++] = '\'';
    for (; *s && k + 6 < cap; s++) { if (*s == '\'') { memcpy(o + k, "'\\''", 4); k += 4; } else o[k++] = *s; }
    o[k++] = '\''; o[k] = 0;
}
static int fg_build(FdIndex *X) {
    const double t0 = fd_ms();
    const int n = X->nf;
    fg_free();
    if (n < 2) return 0;
    FdTerms G; memset(&G, 0, sizeof G);
    FdIv ids = {0}; uint32_t *fo = (uint32_t *)calloc((size_t)n + 1, 4);
    FgPaths P; memset(&P, 0, sizeof P);
    P.bh = (int *)malloc(sizeof(int) * (size_t)(n + 1)); P.bn = (int *)malloc(sizeof(int) * (size_t)(n + 1));
    P.sh = (int *)malloc(sizeof(int) * (size_t)(n + 1)); P.sn = (int *)malloc(sizeof(int) * (size_t)(n + 1));
    for (int i = 0; i <= n; i++) { P.bh[i] = P.sh[i] = -1; P.bn[i] = P.sn[i] = -1; }
    for (int i = 0; i < n; i++) {
        const char *r = X->f[i].rel, *b = strrchr(r, '/'); b = b ? b + 1 : r;
        fg_chain_add(&P.B, P.bh, P.bn, b, (int)strlen(b), i);
        fg_chain_add(&P.S, P.sh, P.sn, r, fg_stem_cut(r, (int)strlen(r)), i);
    }
    /* 1) identificadores de cada arquivo */
    char **txt = NULL; (void)txt;
    for (int i = 0; i < n; i++) {
        fo[i] = (uint32_t)ids.n;
        if (!X->f[i].blob || X->f[i].blob[0] == 0) continue;           /* sem trechos: vazio ou binario */
        char abs[8400]; snprintf(abs, sizeof abs, "%s/%s", X->root, X->f[i].rel);
        size_t m = 0; char *s = fd_slurp(abs, &m, FD_MAXBYTES);
        if (!s) continue;
        fg_idents(&G, s, m, &ids, 1);
        free(s);
    }
    fo[n] = (uint32_t)ids.n;
    const int T = G.n;
    uint32_t *udf = (uint32_t *)calloc((size_t)T + 1, 4);
    for (int k = 0; k < ids.n; k++) udf[ids.v[k]]++;
    /* definicoes (so arquivos de codigo; no maximo 3 arquivos por nome) */
    int (*defs)[3] = (int (*)[3])malloc(sizeof(int[3]) * ((size_t)T + 1)); uint8_t *defc = (uint8_t *)calloc((size_t)T + 1, 1);
    for (int s = 0; s < X->ns; s++) {
        const int fi = X->sf[s]; if (!fg_code(X->f[fi].kind)) continue;
        const char *nm = fd_tstr(&X->T, X->sn[s]); const int l = (int)strlen(nm);
        if (l < 4 || fg_kw(nm, l)) continue;
        const int g = fd_term(&G, nm, l, 0); if (g < 0) continue;
        int has = 0; for (int q = 0; q < defc[g] && q < 3; q++) if (defs[g][q] == fi) has = 1;
        if (has) continue;
        if (defc[g] < 3) defs[g][defc[g]] = fi;
        if (defc[g] < 4) defc[g]++;
    }
    FgEv E = {0};
    const int lim = n / 50 > 30 ? n / 50 : 30;
    /* sym */
    for (int i = 0; i < n; i++) {
        if (!fg_code(X->f[i].kind)) continue;
        for (uint32_t k = fo[i]; k < fo[i + 1]; k++) {
            const uint32_t t = ids.v[k];
            if (defc[t] < 1 || defc[t] > 3 || (int)udf[t] > lim) continue;
            const float w = (float)log(1.0 + (double)n / udf[t]);
            for (int q = 0; q < defc[t]; q++) fg_ev_push(&E, (uint32_t)i, (uint32_t)defs[t][q], w, FG_SYM);
        }
    }
    /* rare: lista de arquivos por identificador (so os que podem ser raros) */
    {
        uint32_t *cnt = (uint32_t *)calloc((size_t)T + 2, 4);
        for (int k = 0; k < ids.n; k++) cnt[ids.v[k]]++;
        int *first = (int *)malloc(sizeof(int) * ((size_t)T + 1)), *fl = (int *)malloc(sizeof(int) * (size_t)(T > 0 ? 4 * (size_t)T : 4));
        uint8_t *rare = (uint8_t *)calloc((size_t)T + 1, 1); int nr = 0;
        for (int t = 0; t < T; t++) {
            first[t] = -1;
            const char *s = fd_tstr(&G, t); const int l = (int)strlen(s);
            if (udf[t] >= 2 && udf[t] <= 4 && l >= 6 && !defc[t] && fg_identlike(s, l)) { rare[t] = 1; first[t] = nr; nr += (int)udf[t]; }
        }
        int *fill = (int *)calloc((size_t)T + 1, sizeof(int));
        free(fl); fl = (int *)malloc(sizeof(int) * ((size_t)nr + 1));
        for (int i = 0; i < n; i++) for (uint32_t k = fo[i]; k < fo[i + 1]; k++) { const uint32_t t = ids.v[k]; if (rare[t]) fl[first[t] + fill[t]++] = i; }
        for (int t = 0; t < T; t++) {
            if (!rare[t]) continue;
            const int df = (int)udf[t]; const float w = (float)(log(1.0 + (double)n / df) / (df - 1));
            for (int a = 0; a < df; a++) for (int b = a + 1; b < df; b++) fg_ev_push(&E, (uint32_t)fl[first[t] + a], (uint32_t)fl[first[t] + b], w, FG_RARE);
        }
        free(cnt); free(first); free(fl); free(rare); free(fill);
    }
    /* path */
    {
        int *mark = (int *)calloc((size_t)n + 1, sizeof(int)); int tg[256];
        for (int i = 0; i < n; i++) {
            if (!X->f[i].blob || X->f[i].blob[0] == 0) continue;
            char abs[8400]; snprintf(abs, sizeof abs, "%s/%s", X->root, X->f[i].rel);
            size_t m = 0; char *s = fd_slurp(abs, &m, FD_MAXBYTES);
            if (!s) continue;
            int ntg = 0; const int stamp = i + 1;
            const char *rel = X->f[i].rel, *sl = strrchr(rel, '/'); const int dl = sl ? (int)(sl - rel) : 0;
            for (size_t a = 0; a < m; ) {
                if (!fg_pc((unsigned char)s[a])) { a++; continue; }
                size_t j = a; while (j < m && fg_pc((unsigned char)s[j])) j++;
                for (size_t p = a + 1; p < j; p++) {
                    if (s[p] != '.' || !(isalnum((unsigned char)s[p - 1]) || s[p - 1] == '_' || s[p - 1] == '-')) continue;
                    size_t e = p + 1; while (e < j && (isalnum((unsigned char)s[e]) || s[e] == '_')) e++;
                    if (!fg_known_ext(s + p + 1, (int)(e - p - 1))) continue;
                    size_t b = p; while (b > a && s[b - 1] != '/') b--;
                    if (e - b > 200) continue;
                    int cf[3]; const int nc = fg_chain_files(&P.B, P.bh, P.bn, s + b, (int)(e - b), cf, 3);
                    for (int q = 0; q < nc; q++) fg_target(tg, &ntg, 256, mark, i, cf[q], stamp);
                }
                a = j;
            }
            for (size_t a = 0; a + 3 < m; a++) {       /* import relativo: 'caminho' ou "caminho" com ./ ou ../ */
                if (s[a] != '\'' && s[a] != '"') continue;
                size_t b = a + 1;
                if (!(s[b] == '.' && (s[b + 1] == '/' || (s[b + 1] == '.' && s[b + 2] == '/')))) continue;
                size_t j = b; while (j < m && fg_pc((unsigned char)s[j]) && j - b < 400) j++;
                if (j >= m || s[j] != s[a]) continue;
                char pth[1100];
                if (!fg_norm(pth, sizeof pth, rel, dl, s + b, (int)(j - b))) continue;
                const int pl = (int)strlen(pth), cut = fg_stem_cut(pth, pl);
                int cf[3], nc = fg_chain_files(&P.S, P.sh, P.sn, pth, cut, cf, 3);
                if (!nc && cut != pl) nc = fg_chain_files(&P.S, P.sh, P.sn, pth, pl, cf, 3);
                if (!nc && pl + 6 < (int)sizeof pth) { memcpy(pth + pl, "/index", 7); nc = fg_chain_files(&P.S, P.sh, P.sn, pth, pl + 6, cf, 3); }
                for (int q = 0; q < nc; q++) fg_target(tg, &ntg, 256, mark, i, cf[q], stamp);
            }
            if (X->f[i].kind == FK_PY) {
                for (size_t a = 0; a < m; ) {
                    size_t e = a; while (e < m && s[e] != '\n') e++;
                    size_t q = a; while (q < e && (s[q] == ' ' || s[q] == '\t')) q++;
                    int from = 0;
                    if (e - q > 5 && !memcmp(s + q, "from ", 5)) { from = 1; q += 5; } else if (e - q > 7 && !memcmp(s + q, "import ", 7)) q += 7; else { a = e + 1; continue; }
                    while (q < e && s[q] == ' ') q++;
                    char mod[300]; int ml = 0;
                    while (q < e && (isalnum((unsigned char)s[q]) || s[q] == '_' || s[q] == '.') && ml < 290) mod[ml++] = s[q++] == '.' ? '/' : s[q - 1];
                    mod[ml] = 0;
                    if (ml && (!from || memchr(s + q, 'i', e - q))) {
                        char pth[1100];
                        for (int pass = 0; pass < 2; pass++) {
                            if (!(pass ? fg_norm(pth, sizeof pth, "", 0, mod, ml) : fg_norm(pth, sizeof pth, rel, dl, mod, ml))) continue;
                            int cf[3]; const int nc = fg_chain_files(&P.S, P.sh, P.sn, pth, (int)strlen(pth), cf, 3);
                            for (int z = 0; z < nc; z++) fg_target(tg, &ntg, 256, mark, i, cf[z], stamp);
                        }
                    }
                    a = e + 1;
                }
            }
            for (int q = 0; q < ntg; q++) fg_ev_push(&E, (uint32_t)i, (uint32_t)tg[q], 1.0f, FG_PATH);
            free(s);
        }
        free(mark);
    }
    /* co-mudanca: git log (some sem avisar fora de repositorio ou sem git) */
    long ncommits = 0;
    if (fg_env_on("KATANA_FIND_CO")) {
        char q[8800], cmd[9000];
        fg_shq(X->root, q, sizeof q);
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
                if (nc < FG_GITMAX) { const int f = fg_find_rel(X, ln); if (f >= 0) cur[nc++] = f; }
            }
            free(ln); pclose(gp);
        }
    }
    /* junta arestas iguais (a, b, tipo); co com teto 3 */
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
    g_fg.n = n; g_fg.nnz = (int)nu;
    g_fg.rp = (int *)calloc((size_t)n + 1, sizeof(int)); g_fg.dst = (int *)malloc(sizeof(int) * (nu + 1));
    g_fg.w = (float *)malloc(sizeof(float) * (nu + 1)); g_fg.ty = (uint8_t *)malloc(nu + 1);
    double *tot = (double *)calloc((size_t)n + 1, sizeof(double));
    for (size_t k = 0; k < nu; k++) { tot[D[k].s] += D[k].w; g_fg.rp[D[k].s + 1]++; }
    for (int i = 0; i < n; i++) g_fg.rp[i + 1] += g_fg.rp[i];
    for (size_t k = 0; k < nu; k++) { g_fg.dst[k] = (int)D[k].d; g_fg.w[k] = (float)(D[k].w / tot[D[k].s]); g_fg.ty[k] = D[k].m; }
    g_fg.ready = 1;
    if (g_fd_verbose) fprintf(stderr, "find: graph %.0f ms: %d files, edges sym %ld path %ld rare %ld co %ld (%ld commits), %d directed entries, %.1f MB\n",
                              fd_ms() - t0, n, cnt_t[0], cnt_t[1], cnt_t[2], cnt_t[3], ncommits, g_fg.nnz, (double)g_fg.nnz * 9 / 1048576.0);
    free(tot); free(D); free(rs); free(E.v); free(ids.v); free(fo); free(udf); free(defs); free(defc);
    free(G.pool); free(G.off); free(G.len); free(G.ht);
    free(P.B.pool); free(P.B.off); free(P.B.len); free(P.B.ht); free(P.S.pool); free(P.S.off); free(P.S.len); free(P.S.ht);
    free(P.bh); free(P.bn); free(P.sh); free(P.sn);
    return 1;
}
/* o grafo existe e vale para o indice atual? senao reconstroi e regrava o cache */
static int fg_ensure(FdIndex *X) {
    if (g_fg.ready && g_fg.n == X->nf) return 1;
    if (!X->ready || X->nf < 2) return 0;
    if (!fg_build(X)) return 0;
    fd_save(X);
    return 1;
}

/* ---- disco: depois dos arquivos do indice ---- */
static int fg_save(FILE *f, FdIndex *X) {
    if (!g_fg.ready || g_fg.n != X->nf) return 1;
    uint32_t h[3] = {FG_MAGIC, (uint32_t)g_fg.n, (uint32_t)g_fg.nnz};
    return fwrite(h, sizeof h, 1, f) == 1 && fwrite(g_fg.rp, 4, (size_t)g_fg.n + 1, f) == (size_t)g_fg.n + 1 &&
           fwrite(g_fg.dst, 4, (size_t)g_fg.nnz, f) == (size_t)g_fg.nnz && fwrite(g_fg.w, 4, (size_t)g_fg.nnz, f) == (size_t)g_fg.nnz &&
           fwrite(g_fg.ty, 1, (size_t)g_fg.nnz, f) == (size_t)g_fg.nnz;
}
static void fg_load(const char *p, size_t len, FdIndex *X) {
    fg_free();
    uint32_t h[3];
    if (len < sizeof h) return;
    memcpy(h, p, sizeof h); p += sizeof h; len -= sizeof h;
    if (h[0] != FG_MAGIC || (int)h[1] != X->nf || h[2] > 100000000u) return;
    const size_t need = 4 * ((size_t)h[1] + 1) + 9 * (size_t)h[2];
    if (len < need) return;
    g_fg.n = (int)h[1]; g_fg.nnz = (int)h[2];
    g_fg.rp = (int *)malloc(4 * ((size_t)h[1] + 1)); g_fg.dst = (int *)malloc(4 * (size_t)h[2] + 4);
    g_fg.w = (float *)malloc(4 * (size_t)h[2] + 4); g_fg.ty = (uint8_t *)malloc((size_t)h[2] + 1);
    memcpy(g_fg.rp, p, 4 * ((size_t)h[1] + 1)); p += 4 * ((size_t)h[1] + 1);
    memcpy(g_fg.dst, p, 4 * (size_t)h[2]); p += 4 * (size_t)h[2];
    memcpy(g_fg.w, p, 4 * (size_t)h[2]); p += 4 * (size_t)h[2];
    memcpy(g_fg.ty, p, h[2]);
    for (int i = 0; i < g_fg.n; i++) if (g_fg.rp[i] > g_fg.rp[i + 1] || g_fg.rp[i + 1] > g_fg.nnz) { fg_free(); return; }
    g_fg.ready = 1;
}

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
    if (!fg_act_on()) return n;
    int w = 0;
    for (int i = 0; i < n; i++) if (!fg_is_request(fd_tstr(&X->T, (int)qid[i]))) { qid[w] = qid[i]; qw[w] = qw[i]; w++; }
    return w ? w : n;
}

/* ---- evidencia: por que o arquivo dst esta ligado ao arquivo src ---- */
typedef struct { int kind; char name[100]; int line; } FgWhy;    /* 1 simbolo, 2 dst cita o caminho de src, 3 src cita o caminho de dst, 4 identificador comum, 5 co-mudanca */
static uint8_t fg_mask(int src, int dst) {
    if (!g_fg.ready || src < 0 || src >= g_fg.n) return 0;
    for (int e = g_fg.rp[src]; e < g_fg.rp[src + 1]; e++) if (g_fg.dst[e] == dst) return g_fg.ty[e];
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
        const int key = fd_key(X, nm, l);
        const long df = key >= 0 ? X->poff[2][key + 1] - X->poff[2][key] : 1L << 20;
        if (df < bdf) { bdf = df; best = s; }
    }
    if (best < 0) return 0;
    snprintf(name, cap, "%s", fd_tstr(&X->T, X->sn[best])); *dline = X->sl[best];
    return 1;
}
static void fg_why(FdIndex *X, int src, int dst, FgWhy *w) {
    memset(w, 0, sizeof *w); w->kind = 5;
    const uint8_t m = fg_mask(src, dst);
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
static int fg_activate(FdIndex *X, const FdHit *all, int na, const char *pre, int k, FdHit *hits, int *esrc) {
    const int nf = X->nf;
    double *act = (double *)calloc((size_t)nf, sizeof(double)), *nx = (double *)calloc((size_t)nf, sizeof(double)), *nb = (double *)calloc((size_t)nf, sizeof(double));
    int *chunk = (int *)malloc(sizeof(int) * (size_t)nf), *src = (int *)malloc(sizeof(int) * (size_t)nf), *nsrc = (int *)malloc(sizeof(int) * (size_t)nf);
    int *touched = (int *)malloc(sizeof(int) * (size_t)nf);
    FgV *cur = (FgV *)malloc(sizeof(FgV) * (size_t)(FG_K_FILE + 1)); int ncur = 0;
    for (int i = 0; i < nf; i++) { chunk[i] = -1; src[i] = -1; }
    const int top = na < FG_K_FILE ? na : FG_K_FILE;
    const double mx = na ? all[0].score : 1;
    for (int i = 0; i < top; i++) {
        const double x = all[i].score / mx;
        if (x < FG_THETA) continue;
        act[all[i].file] = x; chunk[all[i].file] = all[i].chunk; cur[ncur].i = all[i].file; cur[ncur].v = x; ncur++;
    }
    const int graph = g_fg.ready && g_fg.n == nf;
    for (int r = 0; graph && r < FG_ROUNDS && ncur; r++) {
        qsort(cur, (size_t)ncur, sizeof(FgV), fg_cmp_v);
        const int use = ncur < FG_K_GRAPH ? ncur : FG_K_GRAPH; int nt = 0;
        for (int c = 0; c < use; c++) {
            const int i = cur[c].i;
            for (int e = g_fg.rp[i]; e < g_fg.rp[i + 1]; e++) {
                const int j = g_fg.dst[e]; const double y = FG_DECAY * cur[c].v * g_fg.w[e];
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
            if (act[j] == 0) { cur[ncur].i = j; cur[ncur].v = y; ncur++; src[j] = nsrc[j]; }
            act[j] += y;
        }
        for (int c = 0; c < nt; c++) { nx[touched[c]] = 0; nb[touched[c]] = 0; }
        free(tv);
    }
    double amax = 0;
    for (int i = 0; i < nf; i++) if (act[i] > amax) amax = act[i];
    int nout = 0; FgV *out = (FgV *)malloc(sizeof(FgV) * (size_t)(nf + 1));
    if (amax > 0) for (int i = 0; i < nf; i++) if (act[i] >= FG_THETA * amax && fd_in_path(X->f[i].rel, pre)) { out[nout].i = i; out[nout].v = act[i]; nout++; }
    qsort(out, (size_t)nout, sizeof(FgV), fg_cmp_v);
    int r = nout < k ? nout : k;
    for (int c = 0; c < r; c++) {
        const int fi = out[c].i;
        hits[c].file = fi; hits[c].score = out[c].v;
        hits[c].chunk = chunk[fi] >= 0 ? chunk[fi] : X->fc0[fi];
        esrc[c] = chunk[fi] >= 0 ? -1 : src[fi];
    }
    free(act); free(nx); free(nb); free(chunk); free(src); free(nsrc); free(touched); free(cur); free(out);
    return r;
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
static uint8_t *g_fg_read, *g_fg_cited; static int g_fg_setn, g_fg_notes;
static void fd_state_reset(void);
static void fg_reset(void) { fd_state_reset(); if (g_fg_read) memset(g_fg_read, 0, (size_t)g_fg_setn); if (g_fg_cited) memset(g_fg_cited, 0, (size_t)g_fg_setn); g_fg_notes = 0; }
static void fg_sets(int n) {
    if (n == g_fg_setn && g_fg_read) return;
    free(g_fg_read); free(g_fg_cited);
    g_fg_read = (uint8_t *)calloc((size_t)n + 1, 1); g_fg_cited = (uint8_t *)calloc((size_t)n + 1, 1); g_fg_setn = n;
}
/* indice ja em memoria (ou o cache da pasta de trabalho, se o Find ja foi usado nela); nunca cria arquivos */
static FdIndex *fg_index_for_loop(void) {
    static char tried[4200];
    if (g_fd.ready && g_fg.ready && g_fg.n == g_fd.nf) return &g_fd;
    if (g_fd.ready && g_fd.nf >= 2) return fg_ensure(&g_fd) ? &g_fd : NULL;
    char cwd[4096], d[4200]; struct stat st;
    if (!getcwd(cwd, sizeof cwd) || !strcmp(tried, cwd)) return NULL;
    snprintf(tried, sizeof tried, "%s", cwd);
    snprintf(d, sizeof d, "%s/.katana/cache", cwd);
    if (stat(d, &st) != 0 || !S_ISDIR(st.st_mode)) return NULL;
    char err[300] = "";
    fd_update(&g_fd, cwd, err, sizeof err);
    return g_fd.ready && fg_ensure(&g_fd) ? &g_fd : NULL;
}
static int fg_lookup_path(FdIndex *X, const char *p, size_t l) {
    char b[4200];
    if (!l || l >= sizeof b) return -1;
    memcpy(b, p, l); b[l] = 0;
    const char *r = b;
    if (r[0] == '/') { const size_t rl = strlen(X->root); if (strncmp(r, X->root, rl) != 0 || r[rl] != '/') return -1; r += rl + 1; }
    while (r[0] == '.' && r[1] == '/') r += 2;
    return fg_find_rel(X, r);
}
static void fg_neighbor_note(const char *name, const char *js, const char *end, char *result, size_t cap) {
    if (!fg_env_on("KATANA_NEIGHBORS") || g_fg_notes >= FG_NOTES_MAX) return;
    const int isread = !strcmp(name, "Read"), isgrep = !strcmp(name, "Grep"), isfind = !strcmp(name, "Find");
    if (!isread && !isgrep && !isfind) return;
    FdIndex *X = fg_index_for_loop();
    if (!X) return;
    fg_sets(X->nf);
    int srcs[4], ns = 0;
    uint8_t *shown = (uint8_t *)calloc((size_t)X->nf + 1, 1);
    if (isread) {
        char *fp = ct_str(js, end, "file_path");
        const int fi = fp ? fg_lookup_path(X, fp, strlen(fp)) : -1;
        const long long lim = ct_int(js, end, "limit", 0);
        free(fp);
        if (fi < 0) { free(shown); return; }
        const int seen = g_fg_read[fi];
        g_fg_read[fi] = 1;
        if (seen && lim > 0 && lim <= 80) { free(shown); return; }       /* releitura curta de um arquivo ja visto */
        srcs[ns++] = fi; shown[fi] = 1;
    } else {
        int nl = 0;
        for (const char *p = result; *p && nl < 300; nl++) {
            const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
            const char *s = p;
            while (s < e && isdigit((unsigned char)*s)) s++;
            if (s > p && s + 1 < e && s[0] == '.' && s[1] == ' ') s += 2; else s = p;      /* "1. caminho:a-b" do Find */
            const char *t = s; while (t < e && *t != ':' && *t != ' ' && *t != '\t') t++;
            const int fi = t > s ? fg_lookup_path(X, s, (size_t)(t - s)) : -1;
            if (fi >= 0) { shown[fi] = 1; int dup = 0; for (int q = 0; q < ns; q++) if (srcs[q] == fi) dup = 1; if (!dup && ns < 3) srcs[ns++] = fi; }
            p = *e ? e + 1 : e;
        }
    }
    if (!ns) { free(shown); return; }
    double *sc = (double *)calloc((size_t)X->nf + 1, sizeof(double)); int *from = (int *)malloc(sizeof(int) * (size_t)(X->nf + 1)); double *bw = (double *)calloc((size_t)X->nf + 1, sizeof(double));
    int cand[64], nc = 0;
    for (int q = 0; q < ns; q++) {
        const int i = srcs[q];
        for (int e = g_fg.rp[i]; e < g_fg.rp[i + 1]; e++) {
            const int j = g_fg.dst[e];
            if (shown[j] || g_fg_read[j] || g_fg_cited[j]) continue;
            if (isfind && strstr(result, X->f[j].rel)) continue;        /* ja esta na saida (next:, also referenced in): nao duplica */
            if (sc[j] == 0 && nc < 64) cand[nc++] = j;
            if (g_fg.w[e] > bw[j]) { bw[j] = g_fg.w[e]; from[j] = i; }
            sc[j] += g_fg.w[e];
        }
    }
    FgV pick[64]; int np = 0;
    for (int c = 0; c < nc; c++) if (sc[cand[c]] >= FG_NEIGH_MIN) { pick[np].i = cand[c]; pick[np].v = sc[cand[c]]; np++; }
    qsort(pick, (size_t)np, sizeof(FgV), fg_cmp_v);
    char note[1100]; size_t nl = (size_t)snprintf(note, sizeof note, "\n[katana: linked files you have not read yet: ");
    int put = 0;
    for (int c = 0; c < np && put < 3; c++) {
        const int j = pick[c].i, s0 = from[j];
        FgWhy w; fg_why(X, s0, j, &w);
        const char *bs = strrchr(X->f[s0].rel, '/'); bs = bs ? bs + 1 : X->f[s0].rel;
        char ph[300]; fg_why_str(&w, bs, ph, sizeof ph);
        const int add = snprintf(note + nl, sizeof note - nl - 3, "%s%s (%s)", put ? ", " : "", X->f[j].rel, ph);
        if (add <= 0 || nl + (size_t)add + 3 >= sizeof note) break;
        nl += (size_t)add; put++; g_fg_cited[j] = 1;
    }
    if (put) {
        note[nl++] = ']'; note[nl] = 0;
        const size_t rl = strlen(result);
        if (rl + nl + 1 < cap) { memcpy(result + rl, note, nl + 1); g_fg_notes++; }
    }
    free(shown); free(sc); free(from); free(bw);
}
#else
static void fg_reset(void) {}
static void fg_neighbor_note(const char *name, const char *js, const char *end, char *result, size_t cap) { (void)name; (void)js; (void)end; (void)result; (void)cap; }
#endif
