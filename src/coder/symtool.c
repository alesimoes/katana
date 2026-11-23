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
/* ---- passos seguintes do Find (grafo de simbolos) ----
 * Dado um arquivo e uma faixa de linhas que o Find mostrou: os simbolos definidos ali (os que casam com os termos da pergunta primeiro) e,
 * a partir deles, o que chamam (definido em outro arquivo) e quem os chama (em outro arquivo), cada um com a faixa da funcao.
 * So usa o indice de simbolos que ja existe (o Find nao o cria). Devolve quantas entradas escreveu em out (chamados antes dos chamadores). */
/* SyNext e declarado em tools/find.c */
static int sym_next_steps(const char *root, const char *file, int a, int b, const char *const *terms, int nt, SyNext *out, int max) {
    char dir[4400], man[4500]; struct stat sv; double ms; int created;
    store_locate(root, "coder", dir, sizeof dir, 0); snprintf(man, sizeof man, "%s/MANIFEST", dir);
    if (max <= 0 || stat(man, &sv) != 0 || sym_ensure_index(root, &ms, &created) != 0) return 0;
    SyView v; char err[300] = "";
    if (sy_view_open(root, &v, err, sizeof err) != 0) return 0;
    int n = 0;
    const size_t fl = strlen(file);
    StoreLiveDoc *ld = v.any ? sy_lookup(&v, file, fl) : NULL;
    if (ld && v.s[ld->seg].x.has && ld->idx < v.s[ld->seg].x.docs.n) {
        const int seg = ld->seg; const SySec *x = &v.s[seg].x; const uint8_t *dr = store_table_at(&x->docs, ld->idx);
        const uint32_t s0 = st_get_u32(dr), ns = st_get_u32(dr + 4), r0 = st_get_u32(dr + 8), nr = st_get_u32(dr + 12);
        uint32_t cand[64]; int cm[64], nc = 0, anym = 0;
        for (uint32_t k = 0; k < ns && nc < 64; k++) {
            SySym s; sy_sym_at(&v, seg, s0 + k, &s);
            if (((s.pack >> 16) & CSF_DECL) || (int)s.line1 < a || (int)s.line0 > b || s.nl < 4 || s.nl > 90) continue;
            char lo[100]; for (uint32_t i = 0; i < s.nl; i++) lo[i] = (char)tolower((unsigned char)s.name[i]); lo[s.nl] = 0;
            int m = 0; for (int t = 0; t < nt; t++) if (strlen(terms[t]) >= 4 && strstr(lo, terms[t])) m = 1;
            cand[nc] = k; cm[nc] = m; anym |= m; nc++;
        }
        uint32_t pick[3]; int np = 0;
        for (int i = 0; i < nc && np < 3; i++) if (cm[i]) pick[np++] = cand[i];      /* so simbolos que casam com a pergunta: sem eles o proximo passo seria ruido */
        SyNext cl[24]; int ncl = 0, csc[24];
        for (int p = 0; p < np && ncl < 24; p++) {
            SySym D; sy_sym_at(&v, seg, s0 + pick[p], &D);
            for (uint32_t k = 0; k < nr && ncl < 24; k++) {
                SyRef f; sy_ref_at(&v, seg, r0 + k, &f);
                if (f.kind != CR_CALL || f.from == SY_NONE || f.nl < 5 || f.nl > 60) continue;
                int inside = 0; for (uint32_t q = f.from; q != SY_NONE && !inside; ) { if (q == D.rel) inside = 1; else q = st_get_u32(store_table_at(&x->syms, s0 + q) + 8); }
                if (!inside) continue;
                int dup = 0; for (int q = 0; q < ncl; q++) if (!strncmp(cl[q].name, f.name, f.nl) && strlen(cl[q].name) == f.nl) dup = 1;
                if (dup) continue;
                SySyms td = {0}; sy_defs_of(&v, f.name, f.nl, &td); int tn = 0, best = -1;
                for (int q = 0; q < td.n; q++) if (!((td.s[q].pack >> 16) & CSF_DECL)) { tn++; if (best < 0 || (td.s[best].plen == fl && !memcmp(td.s[best].path, file, fl))) best = q; }
                if (tn >= 1 && tn <= 3) {
                    const SySym *T = &td.s[best]; const int self = T->plen == fl && !memcmp(T->path, file, fl);
                    if (!(self && (int)T->line0 >= a && (int)T->line0 <= b)) {
                        SyNext *e = &cl[ncl]; snprintf(e->path, sizeof e->path, "%.*s", (int)T->plen, T->path); e->l0 = (int)T->line0; e->l1 = (int)T->line1; e->callee = 1;
                        snprintf(e->name, sizeof e->name, "%.*s", (int)f.nl, f.name);
                        char lo[64]; for (uint32_t i = 0; i < f.nl; i++) lo[i] = (char)tolower((unsigned char)f.name[i]); lo[f.nl] = 0;
                        int m = 0; for (int t = 0; t < nt; t++) if (strlen(terms[t]) >= 4 && strstr(lo, terms[t])) m = 1;
                        int multi = 0; for (uint32_t i = 1; i < f.nl; i++) if (lo[i] == '_' || (islower((unsigned char)f.name[i - 1]) && isupper((unsigned char)f.name[i]))) multi = 1;
                        if (m || multi) { csc[ncl] = (m ? 2 : 0) + (self ? 0 : 1) - (T->line1 - T->line0 > 150 ? 1 : 0); ncl++; }      /* nomes soltos (format, request) sao ruido */
                    }
                }
                free(td.s);
            }
        }
        for (int i = 0; i < ncl; i++) for (int j = i + 1; j < ncl; j++) if (csc[j] > csc[i]) { SyNext t = cl[i]; cl[i] = cl[j]; cl[j] = t; int u = csc[i]; csc[i] = csc[j]; csc[j] = u; }
        for (int i = 0; i < ncl && n < max; i++) out[n++] = cl[i];
        /* chamadores (em outros arquivos): a funcao que contem a chamada */
        for (int p = 0; p < np && n < max; p++) {
            SySym D; sy_sym_at(&v, seg, s0 + pick[p], &D);
            SyRefs r = {0}; sy_refs_of(&v, D.name, D.nl, 1u << CR_CALL, &r);
            for (int i = 0; i < r.n && n < max; i++) {
                const SyRef *f = &r.r[i];
                if (f->plen == fl && !memcmp(f->path, file, fl)) continue;
                int l0 = (int)f->line, l1 = (int)f->line + 20;
                if (f->from != SY_NONE) { SySym e; sy_sym_at(&v, f->seg, st_get_u32(store_table_at(&v.s[f->seg].x.docs, f->doc)) + f->from, &e); l0 = (int)e.line0; l1 = (int)e.line1; }
                int dup = 0; for (int q = 0; q < n; q++) if (!out[q].callee && strlen(out[q].path) == f->plen && !memcmp(out[q].path, f->path, f->plen) && out[q].l0 == l0) dup = 1;
                if (dup) continue;
                SyNext *e = &out[n++]; snprintf(e->path, sizeof e->path, "%.*s", (int)f->plen, f->path); e->l0 = l0; e->l1 = l1; e->callee = 0;
                snprintf(e->name, sizeof e->name, "%.*s", (int)(D.nl < 60 ? D.nl : 60), D.name);
            }
            free(r.r);
        }
    }
    sy_view_close(&v);
    return n;
}
/* nomes parecidos com name no indice de simbolos (cascata: caixa e sublinhados, parte de um identificador, distancia de edicao); devolve quantos */
static int sym_similar(const char *root, const char *name, char out[][100], int max) {
    char dir[4400], man[4500]; struct stat sv; double ms; int created;
    store_locate(root, "coder", dir, sizeof dir, 0); snprintf(man, sizeof man, "%s/MANIFEST", dir);
    if (max <= 0 || stat(man, &sv) != 0 || sym_ensure_index(root, &ms, &created) != 0) return 0;
    SyView v; char err[300] = "";
    if (sy_view_open(root, &v, err, sizeof err) != 0) return 0;
    int n = 0;
    if (v.any) { SyCand c[8]; int step = 0; const int nc = sy_cascade(&v, name, 1, c, 8, &step);
        for (int i = 0; i < nc && n < max; i++) if (strcmp(c[i].name, name) != 0) { snprintf(out[n], 100, "%s", c[i].name); n++; } }
    sy_view_close(&v);
    return n;
}
#endif
