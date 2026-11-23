/* ---------- coder/symtool.c: o indice de simbolos como ferramenta do agente (Symbols) e como fonte do Find mode=refs ----------
 * Fica depois de symquery.c: reutiliza os `katana coder def|refs|callers|callees|uses|outline|at|deps|impact` (mesmo codigo, mesma
 * cascata e mesmo nivel de confianca), capturando o que eles imprimem, e o indice do coder da pasta (o mesmo que o Find atualiza).
 * Chamado de src/tools/symbols.c (ferramenta) e de src/tools/find.c (mode=refs), que so declaram as duas funcoes abaixo. */
#ifndef _WIN32
/* atualiza (cria na primeira vez) o indice do coder da pasta, com busca e simbolos encadeados, como `katana coder index`.
   Devolve 0 se o indice pode ser lido; *ms = tempo gasto; *created = 1 se nao existia */
static int sym_ensure_index(const char *root, double *ms, int *created) {
    char dir[4400], man[4500], err[300] = ""; struct stat sv; const double t0 = now_sec();
    store_locate(root, "coder", dir, sizeof dir, 1);
    snprintf(man, sizeof man, "%s/MANIFEST", dir); *created = stat(man, &sv) != 0;
    StoreScanOpts o; memset(&o, 0, sizeof o); coder_search_hook(&o); coder_sym_hook(&o, root);
    StoreUpdStats S; const int rc = store_catalog_update(dir, root, &o, &S, err, sizeof err);
    char si[400], yi[500]; coder_search_hook_done(&o, si, sizeof si); coder_sym_hook_done(yi, sizeof yi);
    *ms = (now_sec() - t0) * 1000;
    return (rc == 0 || rc == 1) ? 0 : -1;       /* 1: outro processo grava agora; le o que existe */
}
/* roda uma consulta do coder capturando stdout e stderr (as consultas imprimem direto) */
static int sym_capture(int (*fn)(int, char **), int argc, char **argv, char **out, char **err) {
    fflush(stdout); fflush(stderr);
    FILE *fo = tmpfile(), *fe = tmpfile(); *out = *err = NULL;
    if (!fo || !fe) { if (fo) fclose(fo); if (fe) fclose(fe); return -1; }
    const int so = dup(1), se = dup(2);
    dup2(fileno(fo), 1); dup2(fileno(fe), 2);
    const int rc = fn(argc, argv);
    fflush(stdout); fflush(stderr);
    dup2(so, 1); dup2(se, 2); close(so); close(se);
    FILE *fs[2] = {fo, fe}; char **dst[2] = {out, err};
    for (int i = 0; i < 2; i++) {
        long n = ftell(fs[i]); if (n < 0) n = 0;
        char *b = (char *)malloc((size_t)n + 1); rewind(fs[i]); size_t g = fread(b, 1, (size_t)n, fs[i]); b[g] = 0; *dst[i] = b; fclose(fs[i]);
    }
    return rc;
}
/* action -> funcao e limite padrao. Devolve 0 se a acao nao existe */
static int sym_action(const char *a, int (**fn)(int, char **), int *deflim) {
    static const struct { const char *n; int (*f)(int, char **); int lim; } T[] = {
        {"def", coder_def_main, 8}, {"refs", coder_refs_main, 15}, {"callers", coder_callers_main, 15}, {"callees", coder_callees_main, 15},
        {"uses", coder_uses_main, 15}, {"outline", coder_outline_main, 100}, {"at", coder_at_main, 8}, {"deps", coder_deps_main, 30}, {"impact", coder_impact_main, 25}};
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(T[i].n, a)) { *fn = T[i].f; *deflim = T[i].lim; return 1; }
    return 0;
}
/* Symbols: devolve 0 e o texto em out, ou -1 com a mensagem de erro em out. */
static int sym_tool_run(const char *action, const char *name, const char *file, long long line, long long limit, long long depth, const char *root, char *out, size_t cap) {
    int (*fn)(int, char **) = NULL; int lim = 0;
    if (!action || !sym_action(action, &fn, &lim)) { snprintf(out, cap, "error: unknown action \"%s\". Use def, refs, callers, callees, uses, outline, at, deps or impact.", action ? action : ""); return -1; }
    const int isfile = !strcmp(action, "outline") || !strcmp(action, "deps") || !strcmp(action, "at");
    char target[4400];
    const char *t = isfile ? (file && *file ? file : name) : (name && *name ? name : NULL);
    if (!t || !*t) { snprintf(out, cap, "error: action=%s needs `%s`. Example: %s", action, isfile ? "file" : "name",
        isfile ? (!strcmp(action, "at") ? "{\"action\": \"at\", \"file\": \"src/main.c\", \"line\": 120}" : "{\"action\": \"outline\", \"file\": \"src/main.c\"}") : "{\"action\": \"def\", \"name\": \"parseConfig\"}"); return -1; }
    if (isfile && t[0] == '/' && !strncmp(t, root, strlen(root)) && t[strlen(root)] == '/') t += strlen(root) + 1;
    if (!strcmp(action, "at")) {
        if (line < 1 && !strrchr(t, ':')) { snprintf(out, cap, "error: action=at needs `line`. Example: {\"action\": \"at\", \"file\": \"src/main.c\", \"line\": 120}"); return -1; }
        if (line >= 1) snprintf(target, sizeof target, "%s:%lld", t, line); else snprintf(target, sizeof target, "%s", t);
    } else snprintf(target, sizeof target, "%s", t);
    if (limit < 1) limit = lim;
    if (limit > 100) limit = 100;
    double ms = 0; int created = 0;
    if (sym_ensure_index(root, &ms, &created) != 0) { snprintf(out, cap, "error: cannot create or read the symbol index of %s (is the folder writable?)", root); return -1; }
    char lb[16], db[16], nb[16]; snprintf(lb, sizeof lb, "%lld", limit);
    char *av[16]; int ac = 0; av[ac++] = target; av[ac++] = (char *)root; av[ac++] = (char *)"--limit"; av[ac++] = lb;
    if (!strcmp(action, "def")) { av[ac++] = (char *)"--lines"; av[ac++] = (char *)"1"; }
    if (!strcmp(action, "impact")) {
        if (depth > 0) { snprintf(db, sizeof db, "%lld", depth > 6 ? 6 : depth); av[ac++] = (char *)"--depth"; av[ac++] = db; }
        snprintf(nb, sizeof nb, "%lld", limit); av[ac++] = (char *)"--nodes"; av[ac++] = nb;
    }
    char *so, *se; const int rc = sym_capture(fn, ac, av, &so, &se);
    size_t o = 0; out[0] = 0; (void)rc;
    if (created || ms > 700) o += (size_t)snprintf(out + o, cap - o, "[symbol index %s in %.1f s]\n", created ? "created" : "updated", ms / 1000);
    if (!*so && *se) snprintf(out + o, cap - o, "error: %s", se);
    else {
        const size_t lc = cap < 7000 ? cap - o - 100 : 6500; size_t n = strlen(so);
        if (n > lc) { n = lc; while (n > 0 && so[n] != '\n') n--; snprintf(out + o, cap - o, "%.*s\n(output cut; lower `limit` or be more specific)", (int)n, so); }
        else snprintf(out + o, cap - o, "%s", so);
    }
    free(so); free(se);
    size_t l = strlen(out); while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == ' ')) out[--l] = 0;
    return 0;
}
/* o nome nao e simbolo mas e o nome (ou o nome sem extensao) de um unico arquivo do indice: lista quem o importa/le por source/require/include, no mesmo formato */
static int sy_imp_cmp(const void *a, const void *b) { const SyImp *x = (const SyImp *)a, *y = (const SyImp *)b; int c = x->f.plen < y->f.plen ? memcmp(x->f.path, y->f.path, x->f.plen) : memcmp(x->f.path, y->f.path, y->f.plen); if (!c && x->f.plen != y->f.plen) c = x->f.plen < y->f.plen ? -1 : 1; return c ? c : (int)x->f.line - (int)y->f.line; }
static int sym_find_file_users(SyView *v, const char *name, size_t nl, const char *pre, size_t pl, int limit, CtSb *sb) {
    StoreLiveDoc *hit = NULL; int nh = 0;
    for (int i = 0; i < v->L.n; i++) { StoreLiveDoc *d = &v->L.e[i]; if (d->dead) continue;
        const char *p = d->d.path; uint32_t l = d->d.plen, b = l; while (b && p[b - 1] != '/') b--;
        size_t bl = l - b; if (bl == nl && !memcmp(p + b, name, nl)) { hit = d; nh++; } }
    if (nh != 1 || !hit) return 0;
    SyImp *imp = NULL; int n = sy_importers(v, hit, &imp); int m = 0;
    for (int i = 0; i < n; i++) { const SyRef *f = &imp[i].f; if (pl && !(f->plen >= pl && !memcmp(f->path, pre, pl) && (f->plen == pl || f->path[pl] == '/'))) continue; imp[m++] = imp[i]; }
    n = m; if (!n) { free(imp); return 0; }
    qsort(imp, (size_t)n, sizeof *imp, sy_imp_cmp);
    char hd[600], tx[240]; snprintf(hd, sizeof hd, "Defined (1):\n  %.*s:1  (file)\n", (int)hit->d.plen, hit->d.path); ct_sb_puts(sb, hd);
    int ng = 0; for (int i = 0; i < n; i++) if (!i || imp[i].f.plen != imp[i - 1].f.plen || memcmp(imp[i].f.path, imp[i - 1].f.path, imp[i].f.plen)) ng++;
    snprintf(hd, sizeof hd, "Used %d times in %d files%s:\n", n, ng, ng > limit ? " (top files shown)" : ""); ct_sb_puts(sb, hd);
    int shownf = 0;
    for (int i = 0; i < n && shownf < limit;) {
        int j = i; while (j < n && imp[j].f.plen == imp[i].f.plen && !memcmp(imp[j].f.path, imp[i].f.path, imp[i].f.plen)) j++;
        snprintf(hd, sizeof hd, "%.*s (%d):\n", (int)imp[i].f.plen, imp[i].f.path, j - i); ct_sb_puts(sb, hd); sy_file_load(v, imp[i].f.path, imp[i].f.plen);
        for (int q = i; q < j && q < i + 3; q++) { snprintf(hd, sizeof hd, "  %u: ", imp[q].f.line); ct_sb_puts(sb, hd); if (sy_line_text(v, (int)imp[q].f.line, tx, 101)) ct_sb_puts(sb, tx); ct_sb_puts(sb, "\n"); }
        shownf++; i = j;
    }
    free(imp); return 1;
}
/* Find mode=refs servido pelo indice de simbolos, no mesmo formato do caminho antigo. 1 = respondeu; 0 = sem indice, sem
   definicao do nome exato ou erro: o Find segue pelo caminho antigo. O indice so e atualizado se ja existe (o Find nao o cria). */
static int sym_find_refs(const char *root, const char *pre, const char *name, int limit, CtSb *sb) {
    char dir[4400], man[4500]; struct stat sv; double ms; int created;
    store_locate(root, "coder", dir, sizeof dir, 0); snprintf(man, sizeof man, "%s/MANIFEST", dir);
    if (stat(man, &sv) != 0 || sym_ensure_index(root, &ms, &created) != 0) return 0;
    SyView v; char err[300] = "";
    if (sy_view_open(root, &v, err, sizeof err) != 0) return 0;
    if (!v.any) { sy_view_close(&v); return 0; }
    const size_t nl = strlen(name), pl = pre ? strlen(pre) : 0;
#define SYF_IN(p, l) (!pl || ((l) >= pl && !memcmp((p), pre, pl) && ((l) == pl || (p)[pl] == '/')))
    SySyms d = {0}; sy_defs_of(&v, name, nl, &d);
    int dn = 0; SySym *D = d.s;
    for (int i = 0; i < d.n; i++) if (!((d.s[i].pack >> 16) & CSF_DECL) && SYF_IN(d.s[i].path, d.s[i].plen)) D[dn++] = d.s[i];
    if (!dn) { free(d.s); int fr = sym_find_file_users(&v, name, nl, pre, pl, limit, sb); sy_view_close(&v); return fr; }
    char hd[600], tx[240];
    snprintf(hd, sizeof hd, "Defined (%d):\n", dn); ct_sb_puts(sb, hd);
    for (int i = 0; i < dn && i < 6; i++) {
        snprintf(hd, sizeof hd, "  %.*s:%u  ", (int)D[i].plen, D[i].path, D[i].line0); ct_sb_puts(sb, hd);
        if (sy_file_load(&v, D[i].path, D[i].plen) && sy_line_text(&v, (int)D[i].line0, tx, 111)) ct_sb_puts(sb, tx);
        ct_sb_puts(sb, "\n");
    }
    SyRefs r = {0}; sy_refs_of(&v, name, nl, ~0u, &r);
    typedef struct { int first, n, isdef; } Grp;           /* r esta ordenado por arquivo e linha: cada arquivo e um trecho continuo */
    Grp *g = (Grp *)calloc((size_t)r.n + 1, sizeof *g); int ng = 0, total = 0, prev = -1;
    for (int i = 0; i < r.n; i++) {
        const SyRef *f = &r.r[i]; if (!SYF_IN(f->path, f->plen)) continue;
        int onDef = 0; for (int k = 0; k < dn; k++) if (D[k].plen == f->plen && !memcmp(D[k].path, f->path, f->plen) && D[k].line0 == f->line) onDef = 1;
        if (onDef) continue;
        total++;
        if (prev >= 0 && r.r[prev].plen == f->plen && !memcmp(r.r[prev].path, f->path, f->plen)) { g[ng - 1].n++; prev = i; continue; }
        g[ng].first = i; g[ng].n = 1; ng++; prev = i;
        for (int k = 0; k < dn; k++) if (D[k].plen == f->plen && !memcmp(D[k].path, f->path, f->plen)) g[ng - 1].isdef = 1;
    }
    if (!ng) ct_sb_puts(sb, "No uses found outside the definition.\n");
    else {
        for (int i = 0; i < ng; i++) for (int j = i + 1; j < ng; j++) if (g[j].isdef != g[i].isdef ? g[j].isdef : g[j].n > g[i].n) { Grp t = g[i]; g[i] = g[j]; g[j] = t; }
        snprintf(hd, sizeof hd, "Used %d times in %d files%s:\n", total, ng, ng > limit ? " (top files shown)" : ""); ct_sb_puts(sb, hd);
        for (int i = 0; i < ng && i < limit; i++) {
            const SyRef *f0 = &r.r[g[i].first];
            snprintf(hd, sizeof hd, "%.*s (%d):\n", (int)f0->plen, f0->path, g[i].n); ct_sb_puts(sb, hd);
            sy_file_load(&v, f0->path, f0->plen);
            const int show = sb->n > 1500 ? 1 : g[i].n < 3 ? g[i].n : 3; int shown = 0, lastl = -1;
            for (int q = 0; q < g[i].n && shown < show; q++) {
                const SyRef *f = &r.r[g[i].first + q]; if ((int)f->line == lastl) continue;
                lastl = (int)f->line;
                if (f->from != SY_NONE) { SySym e; sy_sym_at(&v, f->seg, st_get_u32(store_table_at(&v.s[f->seg].x.docs, f->doc)) + f->from, &e); snprintf(hd, sizeof hd, "  %u (in %.*s): ", f->line, (int)(e.nl < 80 ? e.nl : 80), e.name); }
                else snprintf(hd, sizeof hd, "  %u: ", f->line);
                ct_sb_puts(sb, hd); if (sy_line_text(&v, (int)f->line, tx, 101)) ct_sb_puts(sb, tx);
                ct_sb_puts(sb, "\n"); shown++;
            }
            if (g[i].n > shown) { snprintf(hd, sizeof hd, "  (+%d more lines)\n", g[i].n - shown); ct_sb_puts(sb, hd); }
        }
        if (ng > limit) {
            ct_sb_puts(sb, "Also in: ");
            for (int i = limit; i < ng && i < limit + 6; i++) { if (i > limit) ct_sb_puts(sb, ", "); const SyRef *f0 = &r.r[g[i].first]; snprintf(hd, sizeof hd, "%.*s", (int)f0->plen, f0->path); ct_sb_puts(sb, hd); }
            if (ng > limit + 6) { snprintf(hd, sizeof hd, " (+%d more)", ng - limit - 6); ct_sb_puts(sb, hd); }
            ct_sb_puts(sb, "\n");
        }
    }
#undef SYF_IN
    free(g); free(r.r); free(d.s); sy_view_close(&v);
    return 1;
}
#endif
