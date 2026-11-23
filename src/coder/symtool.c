/* ---------- coder/symtool.c: o indice de simbolos como ferramenta do agente (Symbols) e como fonte do Find mode=refs ----------
 * Fica depois de symquery.c e flow.c: chama as consultas estruturadas `sy_q_*` e `fl_q_*` (mesmo codigo, mesma cascata e mesmo nivel de confianca de
 * `katana coder def|refs|callers|callees|uses|outline|at|deps|impact|flow`) e as renderiza em texto num buffer (sym_query_text): nao captura stdout nem
 * stderr, entao pode rodar com a interface usando stdout como protocolo, e usa o indice do coder da pasta (o mesmo que o Find atualiza).
 * Chamado de src/tools/symbols.c (ferramenta) e de src/tools/find.c (mode=refs), que so declaram as duas funcoes abaixo. */
#ifndef _WIN32
/* atualiza (cria na primeira vez) o indice do coder da pasta, com busca e simbolos encadeados, como `katana coder index`, mas so quando
   index_ensure acha que precisa (primeira consulta da raiz, ferramenta que gravou, Bash, prazo KATANA_INDEX_SWEEP_S).
   Devolve 0 se o indice pode ser lido; *ms = tempo gasto; *created = 1 se nao existia */
static int sym_ensure_index(const char *root, double *ms, int *created) {
    IndexRes ir; const int rc = index_ensure(root, 2, "", 0, &ir);      /* indexctl.c: varre e grava so se preciso */
    *created = ir.created; *ms = ir.ms;         /* ms = 0 quando a consulta seguiu sem varrer: o aviso "[symbol index ...]" so aparece quando houve trabalho */
    return rc;
}
/* a consulta `action` do coder sobre `root`, pela API estruturada (sy_q_* / fl_q_*), renderizada em texto: sem capturar stdout nem stderr e sem estado global.
   Devolve em *so o texto que `katana coder <action> ...` imprimiria, ou, se nao ha como consultar (visao nao abre, indice sem as secoes, uso errado), o texto de erro
   (com a quebra de linha final) em *se e *so vazio. A visao vem do cache de indexctl.c e e fechada aqui; `limit` ja vem dentro dos limites; `depth` so vale em impact. */
static void sym_query_text(const char *action, const char *target, const char *root, int limit, int depth, char **so, char **se) {
    const int isflow = !strcmp(action, "flow");
    { char drop[200]; fresh_note_take(drop, sizeof drop); }          /* so os arquivos citados nesta consulta contam para a linha de aviso (fresh.c) */
    char em[900]; SyView v; char err[300] = ""; SyItems items = {0}; CtSb sb = {0}; *so = *se = NULL;
    if (sy_view_open(root, &v, err, sizeof err) != 0) { snprintf(em, sizeof em, "%s: %s\n", action, err); goto fail; }
    if (isflow && !v.anykeys) { snprintf(em, sizeof em, "flow: the index has no text-key sections (it was built before they existed); delete %s and run `katana coder index` again\n", v.dir); sy_view_close(&v); goto fail; }
    if (!isflow && !v.any) { int cmp = 1; if (sm_crawl_state(v.r, &cmp, NULL, NULL, NULL) && !cmp) { snprintf(em, sizeof em, "%s: the symbol index is still being built (no symbols are ready yet); ask again shortly, or use Find or Grep meanwhile\n", action); sy_view_close(&v); goto fail; } }
    if (!isflow && !v.any) { snprintf(em, sizeof em, "%s: the index has no symbol sections (built before they existed); delete %s and run `katana coder index` again\n", action, v.dir); sy_view_close(&v); goto fail; }
    if (isflow) {
        FlQ q;
        if (!strcmp(target, "--links")) fl_q_links(&v, limit, &items, &q); else fl_q_arg(&v, target, limit, &items, &q);
        q.brief = 1; fl_render_text(&sb, &q, &items);
    } else {
        SyQ q;
        if (!strcmp(action, "def")) sy_q_def(&v, target, limit, 1, &items, &q);
        else if (!strcmp(action, "refs")) sy_q_refs(&v, target, ~0u, limit, &items, &q);
        else if (!strcmp(action, "callers")) sy_q_callers(&v, target, limit, &items, &q);
        else if (!strcmp(action, "uses")) sy_q_uses(&v, target, limit, &items, &q);
        else if (!strcmp(action, "callees")) sy_q_callees(&v, target, limit, &items, &q);
        else if (!strcmp(action, "outline")) sy_q_outline(&v, target, limit, &items, &q);
        else if (!strcmp(action, "deps")) sy_q_deps(&v, target, limit, &items, &q);
        else if (!strcmp(action, "impact")) sy_q_impact(&v, target, depth, limit, limit, &items, &q);
        else {                                               /* at */
            char spec[4400]; int line;
            if (!sy_split_at(target, spec, sizeof spec, &line)) { snprintf(em, sizeof em, "uso: katana coder at <arquivo>:<linha> [pasta]\n"); sy_view_close(&v); goto fail; }
            sy_q_at(&v, spec, line, &items, &q); q.arg = target;
        }
        if (!q.total) sy_nf_notes(root, &q);               /* NOT FOUND: "so far" com indice parcial, e as pastas ignoradas que poderiam ter o nome (J.3) */
        sy_render_text(&sb, &q, &items);
    }
    sy_items_free(&items); sy_view_close(&v);
    *so = sb.p ? sb.p : strdup(""); *se = strdup(""); return;
fail:
    *so = strdup(""); *se = strdup(em);
}
/* action -> limite padrao. Devolve 0 se a acao nao existe */
static int sym_action(const char *a, int *deflim) {
    static const struct { const char *n; int lim; } T[] = {
        {"def", 8}, {"refs", 15}, {"callers", 15}, {"callees", 15}, {"uses", 15}, {"outline", 100}, {"at", 8}, {"deps", 30}, {"impact", 25}};
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(T[i].n, a)) { *deflim = T[i].lim; return 1; }
    return 0;
}
/* Symbols: devolve 0 e o texto em out, ou -1 com a mensagem de erro em out. */
static int sym_tool_run2(const char *action, const char *name, const char *file, long long line, long long limit, long long depth, const char *root, char *out, size_t cap);
/* Symbols: a pasta consultada fica registrada durante a chamada (inclusao restrita de `path` dentro de pasta ignorada e visibilidade, maint.c) */
static int sym_tool_run(const char *action, const char *name, const char *file, long long line, long long limit, long long depth, const char *root, char *out, size_t cap) {
    ign_scope_enter(root, 1);
    const int rc = sym_tool_run2(action, name, file, line, limit, depth, root, out, cap);
    ign_scope_leave(); return rc;
}
/* a saida cortada no teto de caracteres das acoes do Symbols, numa quebra de linha */
static void sym_put_capped(char *out, size_t o, size_t cap, const char *so) {
    const size_t lc = cap < 7000 ? cap - o - 100 : 6500; size_t n = strlen(so);
    if (n > lc) { n = lc; while (n > 0 && so[n] != '\n') n--; snprintf(out + o, cap - o, "%.*s\n(output cut; lower `limit` or be more specific)", (int)n, so); }
    else snprintf(out + o, cap - o, "%s", so);
}
static int sym_tool_run2(const char *action, const char *name, const char *file, long long line, long long limit, long long depth, const char *root, char *out, size_t cap) {
    int lim = 0;
    if (action && !strcmp(action, "tree")) {          /* so le o indice que ja existe (N0): nao cria, nao espera nivel mais fundo */
        CtSb sb = {0}; const int rc = sy_tree(root, file && *file ? file : "", (int)depth, (int)limit, 0, &sb);
        size_t o = 0; out[0] = 0; char stl[500]; if (rc >= 0 && idx_status_line(root, "", stl, sizeof stl)) o += (size_t)snprintf(out + o, cap - o, "%s", stl);
        sym_put_capped(out, o, cap, sb.p ? sb.p : ""); free(sb.p);
        size_t l = strlen(out); while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == ' ')) out[--l] = 0;
        return rc < 0 ? -1 : 0;
    }
    const int isflow = action && !strcmp(action, "flow");
    if (isflow) lim = 5;
    else if (!action || !sym_action(action, &lim)) { snprintf(out, cap, "error: unknown action \"%s\". Use def, refs, callers, callees, uses, outline, at, deps, impact, flow or tree.", action ? action : ""); return -1; }
    const int isfile = !strcmp(action, "outline") || !strcmp(action, "deps") || !strcmp(action, "at");
    char target[4400];
    const char *t = isflow ? (name && *name ? name : (file && *file ? file : NULL)) : isfile ? (file && *file ? file : name) : (name && *name ? name : NULL);
    if (isflow && (!t || !*t)) t = "--links";          /* flow sem alvo: o resumo das ligacoes por chave do projeto */
    if (!t || !*t) { snprintf(out, cap, "error: action=%s needs `%s`. Example: %s", action, isfile ? "file" : "name",
        isfile ? (!strcmp(action, "at") ? "{\"action\": \"at\", \"file\": \"src/main.c\", \"line\": 120}" : "{\"action\": \"outline\", \"file\": \"src/main.c\"}") : "{\"action\": \"def\", \"name\": \"parseConfig\"}"); return -1; }
    if ((isfile || (isflow && file && *file && t == file)) && t[0] == '/' && !strncmp(t, root, strlen(root)) && t[strlen(root)] == '/') t += strlen(root) + 1;
    if (isflow && file && *file && t == file && line >= 1 && !strrchr(t, ':')) { snprintf(target, sizeof target, "%s:%lld", t, line); t = target; }
    if (!strcmp(action, "at")) {
        if (line < 1 && !strrchr(t, ':')) { snprintf(out, cap, "error: action=at needs `line`. Example: {\"action\": \"at\", \"file\": \"src/main.c\", \"line\": 120}"); return -1; }
        if (line >= 1) snprintf(target, sizeof target, "%s:%lld", t, line); else snprintf(target, sizeof target, "%s", t);
    } else if (t != target) snprintf(target, sizeof target, "%s", t);
    if (limit < 1) limit = lim;
    if (limit > 100) limit = 100;
    double ms = 0; int created = 0;
    if (sym_ensure_index(root, &ms, &created) != 0) { snprintf(out, cap, "error: cannot create or read the symbol index of %s (is the folder writable?)", root); return -1; }
    char *so, *se; sym_query_text(action, target, root, (int)limit, depth > 0 ? (int)(depth > 6 ? 6 : depth) : 0, &so, &se);
    size_t o = 0; out[0] = 0;
    if (created || ms > 700) o += (size_t)snprintf(out + o, cap - o, "[symbol index %s in %.1f s]\n", created ? "created" : "updated", ms / 1000);
    { char stl[500]; if (idx_status_line(root, "", stl, sizeof stl)) o += (size_t)snprintf(out + o, cap - o, "%s", stl); }       /* no maximo uma linha de estado (J.1) */
    if (!*so && *se) snprintf(out + o, cap - o, "error: %s", se);
    else sym_put_capped(out, o, cap, so);
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
static const char *sy_rchr(const char *s, int c, size_t n) { while (n--) if (s[n] == c) return s + n; return NULL; }
/* ---- o que esta na frente e o que esta atras de um resultado do Find ----
 * Para o simbolo que contem a melhor linha do trecho: `called by:` (quem chama, mesmo arquivo ou outro), `calls:` (o que chama) e `links:` (ligacoes por
 * chave de texto: quem envia/trata a mesma chave em outro arquivo), cada linha com ate ~150 caracteres, ate 3 itens em ordem de confianca, (+N) se houver mais.
 * Liga-se a sym_ensure_index: so existe quando o indice da pasta existe (o Find nao o cria). Devolve 1 se escreveu algo; 0 = o Find usa o `calls:` antigo. */
typedef struct { char name[64]; char path[300]; uint32_t l0, l1; int conf, ord; } SyFn;
static int sy_fn_cmp(const void *a, const void *b) { const SyFn *x = (const SyFn *)a, *y = (const SyFn *)b; return x->conf != y->conf ? y->conf - x->conf : x->ord - y->ord; }
static int sy_is_testpath(const char *p, uint32_t l) { for (uint32_t i = 0; i + 4 <= l; i++) if (!strncasecmp(p + i, "test", 4)) return 1; return 0; }
static void sy_fn_ref(char *o, size_t cap, const char *cur, size_t cl, const SyFn *f) {
    const char *p = f->path; size_t pl = strlen(p);
    const char *cs = cl ? sy_rchr(cur, '/', cl) : NULL; size_t cd = cs ? (size_t)(cs - cur) + 1 : 0;      /* mesma pasta: so o nome do arquivo */
    const char *shown = p; if (cd && pl > cd && !memcmp(p, cur, cd) && !memchr(p + cd, '/', pl - cd)) shown = p + cd; else if (!cd && !strchr(p, '/')) shown = p;
    if (f->l1 > f->l0) snprintf(o, cap, "%s (%s:%u-%u)", f->name, shown, f->l0, f->l1); else snprintf(o, cap, "%s (%s:%u)", f->name, shown, f->l0);
}
static void sy_fn_line(char *out, size_t cap, const char *label, const char *cur, size_t cl, SyFn *f, int n) {
    if (!n) return;
    qsort(f, (size_t)n, sizeof *f, sy_fn_cmp);
    int top = f[0].conf, m = 0; while (m < n && !(f[m].conf < 45 && top >= 45)) m++;
    size_t o = strlen(out); const size_t o0 = o; o += (size_t)snprintf(out + o, cap - o, "   %s: ", label);
    int shown = 0;
    for (int i = 0; i < m && shown < 3; i++) { char t[420]; sy_fn_ref(t, sizeof t, cur, cl, &f[i]); if (o - o0 + strlen(t) + 12 > 150 && shown) break; o += (size_t)snprintf(out + o, cap - o, "%s%s", shown ? ", " : "", t); shown++; }
    if (m > shown) o += (size_t)snprintf(out + o, cap - o, " (+%d)", m - shown);
    snprintf(out + o, cap - o, "\n");
}
static int sym_find_note(const char *root, const char *file, int a, int b, int top, char *out, size_t cap) {
    char dir[4400], man[4500]; struct stat sv; double ms; int created; out[0] = 0;
    store_locate(root, "coder", dir, sizeof dir, 0); snprintf(man, sizeof man, "%s/MANIFEST", dir);
    if (cap < 400 || stat(man, &sv) != 0 || sym_ensure_index(root, &ms, &created) != 0) return 0;
    SyView v; char err[300] = "";
    if (sy_view_open(root, &v, err, sizeof err) != 0) return 0;
    int wrote = 0; const size_t fl = strlen(file);
    StoreLiveDoc *ld = v.any ? sy_lookup(&v, file, fl) : NULL;
    if (ld && v.s[ld->seg].x.has && ld->idx < v.s[ld->seg].x.docs.n) {
        const int seg = ld->seg; const SySec *x = &v.s[seg].x; const uint8_t *dr = store_table_at(&x->docs, ld->idx);
        const uint32_t s0 = st_get_u32(dr), ns = st_get_u32(dr + 4), r0 = st_get_u32(dr + 8), nr = st_get_u32(dr + 12);
        int64_t k = -1; char nm[100]; uint32_t d0 = 0, d1 = 0;
        if (top > 0) k = fl_encl_k(&v, seg, ld->idx, (uint32_t)top, nm, sizeof nm, &d0, &d1);
        if (k < 0) for (uint32_t i = 0; i < ns && k < 0; i++) {            /* sem simbolo na melhor linha: o primeiro funcao/metodo do trecho */
            const uint8_t *y = store_table_at(&x->syms, s0 + i); uint32_t kind = st_get_u32(y + 4), l0 = st_get_u32(y + 12), l1 = st_get_u32(y + 16);
            if (((st_get_u32(y + 40) >> 16) & CSF_DECL) || (kind != CK_FUNC && kind != CK_METHOD && kind != CK_CTOR) || (int)l1 < a || (int)l0 > b) continue;
            uint32_t nl; const char *p = store_strs_get(&x->names, st_get_u32(y), &nl); sy_cpy(nm, sizeof nm, p, nl); d0 = l0; d1 = l1; k = i; }
        SySym D; int haveD = 0; if (k >= 0) { sy_sym_at(&v, seg, s0 + (uint32_t)k, &D); haveD = 1; }
        SyFn *fr = (SyFn *)calloc(64, sizeof *fr); int nfr = 0, totfr = 0;
        if (haveD && D.nl >= 2 && D.nl < 60 && (D.kind == CK_FUNC || D.kind == CK_METHOD || D.kind == CK_CTOR || D.kind == CK_MACRO || D.kind == CK_VAR || D.kind == CK_CONST || D.kind == CK_PROPERTY)) {
            /* quem chama */
            const int nd = sy_ndefs(&v, D.name, D.nl, NULL); SyRefs r = {0}; sy_refs_of(&v, D.name, D.nl, 1u << CR_CALL, &r);
            for (int i = 0; i < r.n; i++) {
                const SyRef *f = &r.r[i]; SyFn e; memset(&e, 0, sizeof e); const SySec *fx = &v.s[f->seg].x; uint32_t fs0 = st_get_u32(store_table_at(&fx->docs, f->doc));
                if (f->from != SY_NONE) { SySym es; sy_sym_at(&v, f->seg, fs0 + f->from, &es); if (es.plen == D.plen && !memcmp(es.path, D.path, D.plen) && es.line0 == D.line0) continue;
                    snprintf(e.name, sizeof e.name, "%.*s", (int)(es.nl < 60 ? es.nl : 60), es.name); e.l0 = es.line0; e.l1 = es.line1; }
                else { snprintf(e.name, sizeof e.name, "(file level)"); e.l0 = f->line; e.l1 = f->line; }
                snprintf(e.path, sizeof e.path, "%.*s", (int)(f->plen < 299 ? f->plen : 299), f->path);
                int dup = 0; for (int q = 0; q < nfr && !dup; q++) if (!strcmp(fr[q].path, e.path) && fr[q].l0 == e.l0) dup = 1; if (dup) continue;
                totfr++; { const int samef = f->plen == D.plen && !memcmp(f->path, D.path, D.plen); e.conf = sy_conf(1, nd, 0, (samef ? 4 : 0) - (D.nl <= 3 ? 20 : 0) - (sy_is_testpath(f->path, f->plen) ? 12 : 0) - (!samef && (D.pack & 0xFF) == CV_FILE ? 35 : 0) - (sy_lfam(f->lang) != sy_lfam(D.lang) ? 35 : 0)); } e.ord = i;
                if (nfr < 64) fr[nfr++] = e; }
            free(r.r);
            char cl[1200] = ""; if (nfr) sy_fn_line(cl, sizeof cl, "called by", file, fl, fr, nfr);
            else if (D.kind == CK_FUNC || D.kind == CK_METHOD || D.kind == CK_CTOR) snprintf(cl, sizeof cl, "   called by: nothing indexed (entry point or called by name from outside)\n");
            if (cl[0]) { snprintf(out + strlen(out), cap - strlen(out), "%s", cl); wrote = 1; }
            /* o que chama */
            nfr = 0; char seen[200][48]; int nseen = 0;
            for (uint32_t q = 0; q < nr && nfr < 64; q++) {
                SyRef f; sy_ref_at(&v, seg, r0 + q, &f); if (f.kind != CR_CALL || f.from == SY_NONE || f.nl < 3 || f.nl > 46) continue;
                int inside = 0; for (uint32_t p = f.from; p != SY_NONE && !inside; ) { if (p == (uint32_t)k) inside = 1; else p = st_get_u32(store_table_at(&x->syms, s0 + p) + 8); }
                if (!inside) continue;
                int dup = 0; for (int z = 0; z < nseen && !dup; z++) if (!strncmp(seen[z], f.name, f.nl) && seen[z][f.nl] == 0) dup = 1; if (dup) continue;
                if (nseen < 200) { memcpy(seen[nseen], f.name, f.nl); seen[nseen++][f.nl] = 0; }
                SySyms td = {0}; sy_defs_of(&v, f.name, f.nl, &td); int tn = 0, pick = -1;
                for (int z = 0; z < td.n; z++) if (!((td.s[z].pack >> 16) & CSF_DECL)) { tn++; if (pick < 0 || (td.s[z].plen == fl && !memcmp(td.s[z].path, file, fl))) pick = z; }
                if (tn && !(td.s[pick].plen == D.plen && !memcmp(td.s[pick].path, D.path, D.plen) && td.s[pick].line0 == D.line0)) {
                    SyFn e; memset(&e, 0, sizeof e); const SySym *T = &td.s[pick]; snprintf(e.name, sizeof e.name, "%.*s", (int)f.nl, f.name); snprintf(e.path, sizeof e.path, "%.*s", (int)(T->plen < 299 ? T->plen : 299), T->path);
                    { const int samef = T->plen == fl && !memcmp(T->path, file, fl); e.l0 = T->line0; e.l1 = T->line1; e.conf = sy_conf(1, tn, 0, (samef ? 4 : 0) - (f.nl <= 4 ? 15 : 0) - (!samef && (T->pack & 0xFF) == CV_FILE ? 35 : 0) - (sy_lfam(T->lang) != sy_lfam(D.lang) ? 35 : 0)); } e.ord = (int)q; fr[nfr++] = e; }
                free(td.s); }
            if (nfr) { sy_fn_line(out, cap, "calls", file, fl, fr, nfr); }
        }
        free(fr);
        /* ligacoes por chave de texto das linhas do simbolo (ou do trecho) */
        const KySec *ky = &v.s[seg].ky;
        if (v.anykeys && ky->has && ld->idx < ky->docs.n) {
            const uint8_t *kd = store_table_at(&ky->docs, ld->idx); uint32_t o0 = st_get_u32(kd), no = st_get_u32(kd + 4) & ~SY_KFLAG;
            uint32_t lo = haveD ? d0 : (uint32_t)a, hi = haveD ? d1 : (uint32_t)b;
            typedef struct { char txt[200]; int conf, sc; } Lk; Lk lk[6]; int nlk = 0;
            for (uint32_t q = 0; q < no && nlk < 6; q++) {
                KyOcc x0; ky_occ_at(&v, seg, o0 + q, &x0); if (x0.line < lo || x0.line > hi) continue;
                int dupk = 0; for (int z = 0; z < nlk; z++) { size_t tl = strlen(lk[z].txt); (void)tl; }
                KyOccs oc = {0}; ky_occs_of(&v, x0.key, x0.kl, &oc);
                const int nfiles = ky_count_files(oc.o, oc.n, 0), npf = ky_count_files(oc.o, oc.n, CKR_PRODUCE), ncf = ky_count_files(oc.o, oc.n, CKR_CONSUME);
                int bi = -1, bc = 0;
                for (int z = 0; z < oc.n; z++) { const KyOcc *p = &oc.o[z]; if (p->role == x0.role || ky_same_file(p, &x0)) continue; FlLink L; if (x0.role == CKR_PRODUCE) fl_pair(&x0, p, nfiles, npf, ncf, ky_file_has(oc.o, oc.n, CKR_PRODUCE, p), &L); else fl_pair(p, &x0, nfiles, npf, ncf, ky_file_has(oc.o, oc.n, CKR_PRODUCE, &x0), &L); if (L.conf > bc) { bc = L.conf; bi = z; } }
                if (bi >= 0 && bc >= 45) {
                    const KyOcc *p = &oc.o[bi]; char lbl[100], pr[300]; fl_key_label(&x0, lbl, sizeof lbl);
                    const char *cs = sy_rchr(file, '/', fl); size_t cd = cs ? (size_t)(cs - file) + 1 : 0; const char *pp = p->path; uint32_t pplen = p->plen;
                    if (cd && pplen > cd && !memcmp(p->path, file, cd) && !memchr(p->path + cd, '/', pplen - cd)) { pp += cd; pplen -= (uint32_t)cd; }
                    snprintf(pr, sizeof pr, "%.*s:%u", (int)(pplen < 200 ? pplen : 200), pp, p->line);
                    const int q2 = x0.cls != CKC_ROUTE && x0.cls != CKC_ENV;
                    for (int z = 0; z < nlk; z++) if (strstr(lk[z].txt, lbl) && strstr(lk[z].txt, pr)) dupk = 1;
                    if (!dupk) { if (x0.role == CKR_PRODUCE) snprintf(lk[nlk].txt, sizeof lk[0].txt, "%s %s%s%s -> %s %s", FL_PV[x0.cls], q2 ? "\"" : "", lbl, q2 ? "\"" : "", FL_PL[p->cls], pr);
                        else snprintf(lk[nlk].txt, sizeof lk[0].txt, "%s %s%s%s <- %s %s", FL_CV[x0.cls], q2 ? "\"" : "", lbl, q2 ? "\"" : "", FL_CL[p->cls], pr);
                        lk[nlk].conf = bc; { int dd = top > 0 ? ((int)x0.line > top ? (int)x0.line - top : top - (int)x0.line) : 0; lk[nlk].sc = bc - (dd * 3 > 30 ? 30 : dd * 3); } nlk++; }
                }
                free(oc.o);
            }
            for (int i = 0; i < nlk; i++) for (int j = i + 1; j < nlk; j++) if (lk[j].sc > lk[i].sc) { Lk t = lk[i]; lk[i] = lk[j]; lk[j] = t; }
            for (int i = 0; i < nlk && i < 2; i++) { size_t l = strlen(out); if (l + 170 >= cap) break; snprintf(out + l, cap - l, "   links: %.140s\n", lk[i].txt); wrote = 1; }
            if (nlk > 2) { size_t l = strlen(out); if (l > 0 && out[l - 1] == '\n') { out[l - 1] = 0; l--; } snprintf(out + l, cap - l, " (+%d more: Symbols flow)\n", nlk - 2); }
        }
    }
    sy_view_close(&v);
    return wrote;
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
