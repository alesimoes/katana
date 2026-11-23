/* ---------- coder/flow.c: ligacoes por CHAVE DE TEXTO (katana coder flow) ----------
 * katana coder flow <chave> [pasta] [--json] [--limit N]          quem produz (envia, emite, escreve, chama a rota) e quem consome (trata, le) a chave
 * katana coder flow <arquivo>[:<linha>] [pasta] [--json]           as chaves que o arquivo (ou o simbolo que contem a linha) produz/consome e para onde vao
 * katana coder flow --links [pasta] [--json] [--limit N]           o grafo de ligacoes por chave do projeto: arquivo -> arquivo, com as chaves
 * As chaves vem de keys.c (secoes SY_T_K* do symindex.c). A ligacao e TEXTO IGUAL em arquivos diferentes; nao ha resolucao.
 * Mesma cascata das outras consultas: nome exato da chave, depois derivado (caixa/sublinhado/parte), depois parecido (distancia de edicao <= 2),
 * com `confidence` 0-100, nivel e `how`. A confianca de uma LIGACAO produtor->consumidor vem de:
 *   base 60 (mesma chave literal em arquivos diferentes), +4 mesma linguagem, +10 se a chave aparece em 2 arquivos (+6 ate 4, -1 por arquivo acima de 8, ate -20:
 *   chave comum), classe (env +14, msg/evento/rota +8, config +4, comando 0, comparacao qualquer -8), rota: +10 se o metodo casa e -25 se discorda,
 *   chave curta e sem separador -6, chave composta (_ . /) +4, muitos produtores x consumidores -3 por par extra (ate -12), mesmo arquivo -20,
 *   a linha foi relida e contem a chave +4, arquivo mudou desde o `index` -25. */
typedef struct { const char *path; uint32_t plen; const char *key; uint32_t kl; int seg; uint32_t doc, line, role, cls, meth, lang; } KyOcc;
typedef struct { KyOcc *o; int n, cap; } KyOccs;
static const char *const FL_CLS[] = {"other", "msg", "event", "route", "env", "config", "cmd"};
static const char *const FL_METH[] = {"", "GET", "POST", "PUT", "DELETE", "PATCH", "OTHER"};
static const char *const FL_PV[] = {"uses", "sends", "emits", "calls", "sets", "defines", "invokes"};              /* verbo do produtor, por classe */
static const char *const FL_PL[] = {"compared in", "handled in", "handled in", "served in", "read in", "read in", "dispatched in"};
static const char *const FL_CV[] = {"compares", "handles", "handles", "serves", "reads", "reads", "dispatches"};   /* verbo do consumidor */
static const char *const FL_CL[] = {"used in", "sent from", "emitted from", "called from", "set in", "defined in", "invoked from"};

static void ky_occ_at(const SyView *v, int seg, uint32_t row, KyOcc *o) {
    const KySec *x = &v->s[seg].ky; const uint8_t *q = store_table_at(&x->occ, row);
    o->seg = seg; o->key = store_strs_get(&x->names, st_get_u32(q), &o->kl); uint32_t pk = st_get_u32(q + 4);
    o->role = pk & 0xFF; o->cls = (pk >> 8) & 0xFF; if (o->cls >= CKC_N) o->cls = 0; o->meth = (pk >> 16) & 0xFF; if (o->meth >= CKM_N) o->meth = 0;
    o->line = st_get_u32(q + 8); o->doc = st_get_u32(q + 12);
    sy_doc_path(v, seg, o->doc, &o->path, &o->plen, &o->lang);
    if (!o->lang) { const char *dot = NULL; for (uint32_t i = 0; i < o->plen; i++) if (o->path[i] == '.') dot = o->path + i; if (dot) { char e[8]; size_t el = (size_t)(o->path + o->plen - dot - 1); if (el < 7) { memcpy(e, dot + 1, el); e[el] = 0; o->lang = (uint32_t)coder_lang_of(o->path); } } }
}
static void ky_push(KyOccs *a, const KyOcc *o) { if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 64; a->o = (KyOcc *)realloc(a->o, sizeof(KyOcc) * (size_t)a->cap); } a->o[a->n++] = *o; }
static int ky_cmp(const void *a, const void *b) { const KyOcc *x = (const KyOcc *)a, *y = (const KyOcc *)b; return sy_cmp_path_line(x->path, x->plen, (int)x->line, y->path, y->plen, (int)y->line); }
static void ky_occs_of(const SyView *v, const char *key, size_t kl, KyOccs *out) {
    for (int s = 0; s < v->ns; s++) { const SyVSeg *c = &v->s[s]; if (!c->ky.has) continue;
        int64_t id = store_strs_find(&c->ky.names, key, kl); uint32_t lo, hi; if (id < 0 || !sy_pair_range(&c->ky.bykey, (uint32_t)id, &lo, &hi)) continue;
        for (uint32_t i = lo; i < hi; i++) { uint32_t row = st_get_u32(store_table_at(&c->ky.bykey, i) + 4); if (!c->live[st_get_u32(store_table_at(&c->ky.occ, row) + 12)]) continue;
            KyOcc o; ky_occ_at(v, s, row, &o); ky_push(out, &o); } }
    qsort(out->o, (size_t)out->n, sizeof(KyOcc), ky_cmp);
}
static void ky_all(const SyView *v, KyOccs *out) {
    for (int s = 0; s < v->ns; s++) { const SyVSeg *c = &v->s[s]; if (!c->ky.has) continue;
        for (uint32_t row = 0; row < c->ky.occ.n; row++) { if (!c->live[st_get_u32(store_table_at(&c->ky.occ, row) + 12)]) continue; KyOcc o; ky_occ_at(v, s, row, &o); ky_push(out, &o); } }
}
static int ky_same_file(const KyOcc *a, const KyOcc *b) { return a->plen == b->plen && !memcmp(a->path, b->path, a->plen); }
static int ky_count_files(const KyOcc *o, int n, int role) {
    int f = 0; for (int i = 0; i < n; i++) { if (role && (int)o[i].role != role) continue; int dup = 0; for (int j = 0; j < i && !dup; j++) if ((!role || (int)o[j].role == role) && ky_same_file(&o[i], &o[j])) dup = 1; if (!dup) f++; }
    return f;
}
/* innermost symbol containing a line: name, range. 0 = none */
static int64_t fl_encl_k(const SyView *v, int seg, uint32_t doc, uint32_t line, char *nm, size_t cap, uint32_t *l0, uint32_t *l1) {
    const SySec *x = &v->s[seg].x; if (!x->has || doc >= x->docs.n) return -1;
    const uint8_t *dr = store_table_at(&x->docs, doc); uint32_t s0 = st_get_u32(dr), n = st_get_u32(dr + 4); if (!n) return -1;
    uint32_t lo = 0, hi = n; while (lo < hi) { uint32_t m = lo + (hi - lo) / 2; if (st_get_u32(store_table_at(&x->syms, s0 + m) + 12) <= line) lo = m + 1; else hi = m; }
    int64_t k = (int64_t)lo - 1;
    while (k >= 0 && st_get_u32(store_table_at(&x->ival, s0 + (uint32_t)k)) >= line) {
        const uint8_t *y = store_table_at(&x->syms, s0 + (uint32_t)k);
        if (st_get_u32(y + 16) >= line && !((st_get_u32(y + 40) >> 16) & CSF_DECL)) { uint32_t nl; const char *p = store_strs_get(&x->names, st_get_u32(y), &nl); sy_cpy(nm, cap, p, nl); *l0 = st_get_u32(y + 12); *l1 = st_get_u32(y + 16); return k; }
        k--;
    }
    return -1;
}
static int fl_encl(const SyView *v, int seg, uint32_t doc, uint32_t line, char *nm, size_t cap, uint32_t *l0, uint32_t *l1) { return fl_encl_k(v, seg, doc, line, nm, cap, l0, l1) >= 0; }

/* ---- confianca de uma ligacao produtor -> consumidor ---- */
typedef struct { int conf; char how[170]; } FlLink;
/* o arquivo de ref tambem tem uma ocorrencia com este papel? (um consumidor que ele mesmo define a chave nao recebe nada de fora) */
static int ky_file_has(const KyOcc *o, int n, int role, const KyOcc *ref) { for (int i = 0; i < n; i++) if ((int)o[i].role == role && ky_same_file(&o[i], ref)) return 1; return 0; }
static void fl_pair(const KyOcc *p, const KyOcc *c, int nfiles, int np, int nc, int localdef, FlLink *L) {
    int conf = 60; char h[170]; size_t o = 0; const int same = ky_same_file(p, c);
    o += (size_t)snprintf(h + o, sizeof h - o, "same literal key");
    if (localdef && !same) { conf -= (c->cls == CKC_ENV || c->cls == CKC_CONFIG) ? 30 : 10; o += (size_t)snprintf(h + o, sizeof h - o, "; the consumer file also defines the key itself"); }
    if (same) { conf -= 20; o += (size_t)snprintf(h + o, sizeof h - o, " in the SAME file (it both sends and handles it)"); } else o += (size_t)snprintf(h + o, sizeof h - o, " in different files");
    const int samelang = p->lang && sy_lfam(p->lang) == sy_lfam(c->lang);
    if (samelang) { conf += 4; o += (size_t)snprintf(h + o, sizeof h - o, "; same language"); } else if (p->lang && c->lang) o += (size_t)snprintf(h + o, sizeof h - o, "; cross-language (%s -> %s)", sy_langname(p->lang), sy_langname(c->lang));
    if (nfiles <= 2) { conf += 10; o += (size_t)snprintf(h + o, sizeof h - o, "; rare key (%d files)", nfiles); } else if (nfiles <= 4) { conf += 6; o += (size_t)snprintf(h + o, sizeof h - o, "; %d files use the key", nfiles); }
    else if (nfiles > 8) { int pen = nfiles - 8; if (pen > 20) pen = 20; conf -= pen; o += (size_t)snprintf(h + o, sizeof h - o, "; common key (%d files)", nfiles); }
    static const int bonus[CKC_N] = {-8, 8, 8, 8, 14, 4, 0};
    int cls = c->cls == p->cls ? (int)c->cls : -1;
    if (cls < 0) { conf -= 8; o += (size_t)snprintf(h + o, sizeof h - o, "; kinds differ (%s vs %s)", FL_CLS[p->cls], FL_CLS[c->cls]); }
    else { conf += bonus[cls]; o += (size_t)snprintf(h + o, sizeof h - o, "; %s key", FL_CLS[cls]); }
    if (p->cls == CKC_ROUTE && c->cls == CKC_ROUTE && p->meth && c->meth) {
        if (p->meth == c->meth) { conf += 10; o += (size_t)snprintf(h + o, sizeof h - o, "; method %s matches", FL_METH[p->meth]); } else { conf -= 25; o += (size_t)snprintf(h + o, sizeof h - o, "; method differs (%s vs %s)", FL_METH[p->meth], FL_METH[c->meth]); } }
    int plain = 1; for (uint32_t i = 0; i < p->kl; i++) if (!isalpha((unsigned char)p->key[i])) plain = 0;
    if (p->kl <= 4 && plain) conf -= 6; else if (!plain && p->cls != CKC_ENV) conf += 4;
    { int extra = np * nc - 1; if (extra > 0) { int pen = extra * 3; conf -= pen > 12 ? 12 : pen; o += (size_t)snprintf(h + o, sizeof h - o, "; %d sender file%s x %d handler file%s", np, np == 1 ? "" : "s", nc, nc == 1 ? "" : "s"); } }
    L->conf = conf < 1 ? 1 : conf > 99 ? 99 : conf; snprintf(L->how, sizeof L->how, "%s", h);
}
static int fl_verify(SyView *v, const KyOcc *o) {
    if (!sy_file_load(v, o->path, o->plen) || v->fstale) return -1;
    char t[600], k[100]; snprintf(k, sizeof k, "%.*s", o->kl < 99 ? (int)o->kl : 99, o->key);
    for (int l = (int)o->line; l <= (int)o->line + 1; l++) if (sy_line_text(v, l, t, sizeof t) && strstr(t, k)) return 1;
    if (o->cls == CKC_ROUTE) { const char *sl = strrchr(k, '/'); if (sl && sl[1]) for (int l = (int)o->line; l <= (int)o->line + 1; l++) if (sy_line_text(v, l, t, sizeof t) && strstr(t, sl)) return 1; }
    return -1;
}
static void fl_limits(const SyView *v, char *o, size_t cap) {
    size_t n = (size_t)snprintf(o, cap, "Indexed set: %u files analysed for text keys. A link is the SAME literal text in two files (event/message names, routes, env variables, config keys, subcommands); keys built at run time, "
        "dispatch tables, reflection and other projects are not seen.", v->nkeyflag);
    if (v->nkeyold && n < cap) snprintf(o + n, cap - n, " %u indexed files were analysed before text keys existed and have none: delete %s and run `katana coder index` again.", v->nkeyold, v->dir);
}
/* "in nome (l0-l1)" do simbolo que contem a linha */
static void fl_where(const SyView *v, const KyOcc *o, char *out, size_t cap) {
    char nm[100]; uint32_t a, b; out[0] = 0;
    if (fl_encl(v, o->seg, o->doc, o->line, nm, sizeof nm, &a, &b)) snprintf(out, cap, "in %s (%u-%u)", nm, a, b);
}
static void fl_set_line(SyView *v, SyItem *it, const KyOcc *o) {
    char t[240]; if (sy_file_load(v, o->path, o->plen) && sy_line_text(v, (int)o->line, t, sizeof t)) { it->snip = (char *)malloc(strlen(t) + 16); sprintf(it->snip, "%u: %s", o->line, t); }
}
static void fl_key_label(const KyOcc *o, char *out, size_t cap) {
    if (o->cls == CKC_ROUTE && o->meth) snprintf(out, cap, "%s %.*s", FL_METH[o->meth], (int)(o->kl < 80 ? o->kl : 80), o->key);
    else snprintf(out, cap, "%.*s", (int)(o->kl < 90 ? o->kl : 90), o->key);
}

/* ---- argumentos e abertura ---- */
static int g_fl_brief;       /* --brief: texto enxuto para o modelo (explicacao da confianca encurtada) */
static void fl_brief_how(const char *how, char *o, size_t cap) {
    snprintf(o, cap, "%s", how); if (!g_fl_brief) return;
    int semi = 0; for (char *p = o; *p; p++) if (*p == ';' && ++semi == 3) { *p = 0; break; }
}
static int fl_start(const char *use, int argc, char **argv, SyArgs *a, SyView *v, int *links) {
    char *av[64]; int ac = 0; *links = 0; g_fl_brief = 0;
    for (int i = 0; i < argc && ac < 62; i++) { if (!strcmp(argv[i], "--links")) *links = 1; else if (!strcmp(argv[i], "--brief")) g_fl_brief = 1; else av[ac++] = argv[i]; }
    SyArgs probe; sy_args(ac, av, &probe, 12);
    if (probe.npos < 1 && *links) av[ac++] = (char *)".";
    if (sy_args(ac, av, a, 12) != 0) { fprintf(stderr, "uso: katana coder %s\n", use); return 2; }
    const char *folder = *links ? a->pos[0] : (a->npos >= 2 ? a->pos[1] : ".");
    char err[300] = ""; if (sy_view_open(folder, v, err, sizeof err) != 0) { fprintf(stderr, "flow: %s\n", err); return 1; }
    if (!v->anykeys) { fprintf(stderr, "flow: the index has no text-key sections (it was built before they existed); delete %s and run `katana coder index` again\n", v->dir); sy_view_close(v); return 1; }
    return 0;
}
typedef struct { const char *query, *arg; int json; char resolved[200], cascade[160], hint[900]; int step; } FlQ;
static void fl_emit(const SyView *v, FlQ *o, SyItems *a, int total, const char *notfound) {
    char lim[900]; fl_limits(v, lim, sizeof lim);
    if (o->json) {
        printf("{\"query\":"); cp_jstr(o->query); printf(",\"arg\":"); cp_jstr(o->arg);
        if (o->resolved[0]) { printf(",\"resolved\":"); cp_jstr(o->resolved); }
        if (o->step) { printf(",\"cascade_step\":%d,\"cascade\":", o->step); cp_jstr(o->cascade); }
        printf(",\"count\":%d,\"shown\":%d,\"items\":[", total, a->n);
        for (int i = 0; i < a->n; i++) { if (i) printf(","); sy_json_item(&a->v[i]); if (a->v[i].sig[0] && !strcmp(a->v[i].kind, "link")) { /* o par ja vai em sig */ } }
        printf("]"); if (o->hint[0]) { printf(",\"note\":"); cp_jstr(o->hint); }
        if (!total) { char nf[1400]; snprintf(nf, sizeof nf, "NOT FOUND: %s. Cascade tried: exact key, same key ignoring case or as a part, edit distance <= 2. %s", notfound, lim); printf(",\"not_found\":"); cp_jstr(nf); }
        printf(",\"index\":{\"files\":%u,\"with_keys\":%u}}\n", v->nlive, v->nkeyflag); return;
    }
    if (!total) { printf("NOT FOUND: %s.\nCascade tried: exact key, same key ignoring case or as a part, edit distance <= 2.\n%s\n", notfound, lim); return; }
    if (o->step > 1) printf("No exact key '%s'; %s\n", o->arg, o->cascade);
}
static void fl_print_item(const SyItem *it) {
    if (!strcmp(it->kind, "link")) { char hb[200]; fl_brief_how(it->how, hb, sizeof hb); printf("  %s:%d  %s  [confidence %d %s; %s]\n", it->path, it->line0, it->name, it->conf, sy_level(it->conf), hb); return; }
    printf("  %s:%d  %s  %s  [%s; confidence %d %s]\n", it->path, it->line0, it->name, it->encl, it->vis, it->conf, sy_level(it->conf));
    if (it->snip) printf("      %s\n", it->snip);
}

/* ---- cascata de nomes de chave ---- */
static int fl_nocc(const SyView *v, const char *k, size_t kl) { KyOccs o = {0}; ky_occs_of(v, k, kl, &o); int n = o.n; free(o.o); return n; }
static int fl_cascade(const SyView *v, const char *q, SyCand *out, int max, int *stepused) {
    size_t ql = strlen(q); int n = 0; *stepused = 0;
    if (fl_nocc(v, q, ql) > 0) { snprintf(out[0].name, sizeof out[0].name, "%s", q); out[0].step = 1; snprintf(out[0].how, sizeof out[0].how, "exact key match"); *stepused = 1; return 1; }
    char qf[128]; sy_fold(q, ql, qf, sizeof qf); char qt[16][32]; int qn = sy_tokens(q, ql, qt, 16);
    SyNameHit *hits = NULL; int nh = 0, hc = 0;
    for (int step = 2; step <= 3 && !n; step++) {
        nh = 0;
        for (int s = 0; s < v->ns; s++) { const SyVSeg *c = &v->s[s]; if (!c->ky.has) continue;
            for (uint32_t i = 0; i < c->ky.names.n; i++) {
                uint32_t l; const char *p = store_strs_get(&c->ky.names, i, &l); if (l == 0 || l > 120 || (l == ql && !memcmp(p, q, l))) continue;
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
        qsort(hits, (size_t)nh, sizeof *hits, sy_hit_cmp);
        for (int k = 0; k < nh && n < max && n < 6; k++) {
            if (fl_nocc(v, hits[k].name, strlen(hits[k].name)) <= 0) continue;
            snprintf(out[n].name, sizeof out[n].name, "%s", hits[k].name); out[n].step = step;
            if (step == 2) snprintf(out[n].how, sizeof out[n].how, hits[k].score == 0 ? "no exact key; same text ignoring case and punctuation" : "no exact key; '%s' is part of the key", q);
            else snprintf(out[n].how, sizeof out[n].how, "no exact or derived key; within edit distance %d", hits[k].score);
            n++;
        }
        if (n) *stepused = step;
    }
    free(hits); return n;
}
/* "7" -> numero; texto -> chave. Devolve 1 se arg e arquivo:linha de um arquivo do indice */
static int fl_is_file(const SyView *v, const char *arg, char *spec, size_t cap, int *line) {
    snprintf(spec, cap, "%s", arg); *line = 0;
    char *colon = strrchr(spec, ':'); int hasline = 0;
    if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)) { *line = atoi(colon + 1); *colon = 0; hasline = 1; }
    if (!hasline && fl_nocc(v, arg, strlen(arg)) > 0) return 0;
    if (!hasline && !strchr(spec, '/') && !strchr(spec, '.')) return 0;
    char rel[4400]; int seg, ns; uint32_t idx; return sy_find_doc(v, spec, rel, sizeof rel, &seg, &idx, &ns);
}

/* ---- flow <chave> ---- */
typedef struct { int p, c; FlLink L; } FlPair;
static int fl_pair_cmp(const void *a, const void *b) { const FlPair *x = (const FlPair *)a, *y = (const FlPair *)b; return y->L.conf - x->L.conf; }
static int fl_key_main(SyView *v, SyArgs *a, const char *q) {
    SyCand cand[8]; int step, nc = fl_cascade(v, q, cand, 8, &step); SyItems items = {0}; FlQ o; memset(&o, 0, sizeof o); o.query = "flow"; o.arg = q; o.json = a->json; int total = 0;
    if (nc) { o.step = step; for (int i = 0; i < nc && i < 4; i++) { size_t l = strlen(o.resolved); snprintf(o.resolved + l, sizeof o.resolved - l, "%s%s", i ? ", " : "", cand[i].name); } snprintf(o.cascade, sizeof o.cascade, "showing %s (%s)", o.resolved, cand[0].how); }
    char nextbuf[400] = ""; int nlink = 0;
    for (int ci = 0; ci < nc; ci++) {
        KyOccs oc = {0}; ky_occs_of(v, cand[ci].name, strlen(cand[ci].name), &oc);
        int np = 0, ncn = 0; for (int i = 0; i < oc.n; i++) { if (oc.o[i].role == CKR_PRODUCE) np++; else ncn++; }
        const int nfiles = ky_count_files(oc.o, oc.n, 0), npf = ky_count_files(oc.o, oc.n, CKR_PRODUCE), ncf = ky_count_files(oc.o, oc.n, CKR_CONSUME);
        int cls = 0, best = -1; { int cnt[CKC_N] = {0}; for (int i = 0; i < oc.n; i++) cnt[oc.o[i].cls]++; for (int k = 0; k < CKC_N; k++) if (cnt[k] > best) { best = cnt[k]; cls = k; } }
        total += oc.n;
        for (int role = CKR_PRODUCE; role <= CKR_CONSUME; role++) {
            int shown = 0;
            for (int i = 0; i < oc.n; i++) { const KyOcc *x = &oc.o[i]; if ((int)x->role != role) continue;
                if (shown >= a->limit) break; shown++;
                SyItem *it = sy_item_new(&items); sy_set_path(it, x->path, x->plen); it->line0 = it->line1 = (int)x->line;
                snprintf(it->kind, sizeof it->kind, "%s", role == CKR_PRODUCE ? "produce" : "consume"); fl_key_label(x, it->name, sizeof it->name); fl_where(v, x, it->encl, sizeof it->encl);
                snprintf(it->vis, sizeof it->vis, "%s", FL_CLS[x->cls]);
                it->ver = fl_verify(v, x); it->conf = sy_conf(cand[ci].step, 1, it->ver, x->cls == CKC_OTHER ? -10 : 0);
                snprintf(it->how, sizeof it->how, "%s; %s", cand[ci].how, it->ver > 0 ? "line verified" : "line NOT verified");
                fl_set_line(v, it, x); }
        }
        /* ligacoes */
        FlPair *pr = (FlPair *)malloc(sizeof *pr * (size_t)(np * ncn + 1)); int npr = 0;
        if (np && ncn && np * ncn <= 4000) for (int i = 0; i < oc.n; i++) { if (oc.o[i].role != CKR_PRODUCE) continue; for (int j = 0; j < oc.n; j++) { if (oc.o[j].role != CKR_CONSUME) continue;
            if (oc.o[i].cls == CKC_ROUTE && oc.o[j].cls == CKC_ROUTE && 0) continue;
            pr[npr].p = i; pr[npr].c = j; fl_pair(&oc.o[i], &oc.o[j], nfiles, npf, ncf, ky_file_has(oc.o, oc.n, CKR_PRODUCE, &oc.o[j]), &pr[npr].L); npr++; } }
        qsort(pr, (size_t)npr, sizeof *pr, fl_pair_cmp);
        int top = npr ? pr[0].L.conf : 0, lshown = 0; (void)cls;
        for (int k = 0; k < npr && lshown < 5; k++) {
            if (pr[k].L.conf < 45 && top >= 45) break;
            if (k > 0 && pr[k].L.conf < top - 30) break;
            const KyOcc *p = &oc.o[pr[k].p], *c = &oc.o[pr[k].c];
            /* so um par por arquivo de cada lado (o primeiro) */
            int dup = 0; for (int z = 0; z < k && !dup; z++) { const KyOcc *p2 = &oc.o[pr[z].p], *c2 = &oc.o[pr[z].c]; if (ky_same_file(p, p2) && ky_same_file(c, c2)) dup = 1; }
            if (dup) continue;
            SyItem *it = sy_item_new(&items); sy_set_path(it, p->path, p->plen); it->line0 = it->line1 = (int)p->line; snprintf(it->kind, sizeof it->kind, "link");
            char pw[160], cw[160], cwl[100], lbl[100]; fl_where(v, p, pw, sizeof pw); fl_where(v, c, cw, sizeof cw); (void)cwl; fl_key_label(p, lbl, sizeof lbl);
            snprintf(it->name, sizeof it->name, "%s %s -> %s", FL_PV[p->cls], lbl, FL_PL[c->cls]);
            snprintf(it->sig, sizeof it->sig, "%.*s:%u", (int)c->plen, c->path, c->line);
            snprintf(it->encl, sizeof it->encl, "producer %.*s%s%s; consumer %s", (int)p->plen, p->path, pw[0] ? " " : "", pw, cw[0] ? cw : "(file level)");
            it->ver = fl_verify(v, p) > 0 && fl_verify(v, c) > 0 ? 1 : -1;
            it->conf = pr[k].L.conf + (it->ver > 0 ? 4 : -25); if (it->conf < 1) it->conf = 1; if (it->conf > 99) it->conf = 99;
            snprintf(it->how, sizeof it->how, "%s; %s", pr[k].L.how, it->ver > 0 ? "both lines verified" : "a line NOT verified");
            if (!nlink) { uint32_t a0, a1; char nm[100]; int hasr = fl_encl(v, c->seg, c->doc, c->line, nm, sizeof nm, &a0, &a1); uint32_t r0 = hasr ? a0 : (c->line > 3 ? c->line - 3 : 1), r1 = hasr ? a1 : c->line + 25; if (r1 - r0 > 60) r1 = r0 + 60;
                snprintf(nextbuf, sizeof nextbuf, "Next: Read %.*s offset=%u limit=%u (the %s side)%s%s", (int)c->plen, c->path, r0, r1 - r0 + 1, FL_CLS[c->cls] == FL_CLS[CKC_ROUTE] ? "serving" : "consuming", hasr ? ", symbol " : "", hasr ? nm : ""); }
            nlink++; lshown++;
        }
        free(pr);
        if (np && !ncn) { size_t l = strlen(o.hint); snprintf(o.hint + l, sizeof o.hint - l, "%sNobody handles '%s' in the indexed set (%u files analysed for text keys): %d place%s send%s it, none consumes it. A handler built at run time, in another project or reached by other means is not seen.", l ? " " : "", cand[ci].name, v->nkeyflag, np, np == 1 ? "" : "s", np == 1 ? "s" : ""); }
        else if (ncn && !np) { size_t l = strlen(o.hint); snprintf(o.hint + l, sizeof o.hint - l, "%sNobody sends '%s' in the indexed set (%u files analysed for text keys): %d place%s read%s it, none produces it. The sender may build the text at run time, live in another project or be external (user input, a protocol).", l ? " " : "", cand[ci].name, v->nkeyflag, ncn, ncn == 1 ? "" : "s", ncn == 1 ? "s" : ""); }
        free(oc.o);
    }
    if (nextbuf[0] && !o.json) { size_t l = strlen(o.hint); snprintf(o.hint + l, sizeof o.hint - l, "%s%s", l ? "\n" : "", nextbuf); }
    char nf[300]; snprintf(nf, sizeof nf, "no text key '%s' in the indexed set (nobody sends, emits, writes, handles or reads it)", q);
    fl_emit(v, &o, &items, total, nf);
    if (!o.json && total) {
        for (int pass = 0; pass < 3; pass++) {
            const char *kind = pass == 0 ? "produce" : pass == 1 ? "consume" : "link"; int cnt = 0; for (int i = 0; i < items.n; i++) if (!strcmp(items.v[i].kind, kind)) cnt++;
            if (!cnt) continue;
            printf("%s (%d):\n", pass == 0 ? "PRODUCE (sends / emits / writes / calls)" : pass == 1 ? "CONSUME (handles / reads / serves)" : "LINKS producer -> consumer", cnt);
            for (int i = 0; i < items.n; i++) if (!strcmp(items.v[i].kind, kind)) {
                const SyItem *it = &items.v[i];
                if (pass == 2) { char hb[200]; fl_brief_how(it->how, hb, sizeof hb); printf("  %s:%d %s %s  [confidence %d %s; %s]\n", it->path, it->line0, it->name, it->sig, it->conf, sy_level(it->conf), hb); if (!g_fl_brief) printf("      %s\n", it->encl); }
                else fl_print_item(it);
            }
        }
        if (o.hint[0]) printf("%s\n", o.hint);
    }
    sy_items_free(&items); return total ? 0 : 1;
}

/* ---- flow <arquivo>[:<linha>] ---- */
typedef struct { KyOcc o; int n; int conf; char peer[200], how[170]; char encl[100]; } FlRow;
static int fl_row_cmp(const void *a, const void *b) { const FlRow *x = (const FlRow *)a, *y = (const FlRow *)b; if (x->o.role != y->o.role) return (int)x->o.role - (int)y->o.role; if ((x->conf > 0) != (y->conf > 0)) return y->conf > 0 ? 1 : -1; if (x->conf != y->conf) return y->conf - x->conf; return (int)x->o.line - (int)y->o.line; }
static int fl_file_main(SyView *v, SyArgs *a, const char *spec, int line) {
    char rel[4400]; int seg, ns; uint32_t idx; FlQ o; memset(&o, 0, sizeof o); o.query = "flow"; o.arg = a->pos[0]; o.json = a->json; SyItems items = {0}; char nf[700];
    if (!sy_find_doc(v, spec, rel, sizeof rel, &seg, &idx, &ns)) { snprintf(nf, sizeof nf, "'%s' is not in the indexed set", spec); fl_emit(v, &o, &items, 0, nf); return 1; }
    const KySec *ky = &v->s[seg].ky;
    if (!ky->has || idx >= ky->docs.n) { snprintf(nf, sizeof nf, "'%s' has no text-key data (indexed before keys existed: delete %s and run `katana coder index` again)", rel, v->dir); fl_emit(v, &o, &items, 0, nf); return 1; }
    const uint8_t *dr = store_table_at(&ky->docs, idx); uint32_t o0 = st_get_u32(dr), no = st_get_u32(dr + 4) & ~SY_KFLAG;
    uint32_t lo = 0, hi = 0xFFFFFFFFu; char rng[200] = "";
    if (line > 0) {
        char nm[100]; uint32_t a0, a1;
        if (fl_encl(v, seg, idx, (uint32_t)line, nm, sizeof nm, &a0, &a1)) { lo = a0; hi = a1; snprintf(rng, sizeof rng, " in %s (%u-%u)", nm, a0, a1); }
        else { lo = line > 10 ? (uint32_t)line - 10 : 1; hi = (uint32_t)line + 10; snprintf(rng, sizeof rng, " around line %d", line); }
    }
    FlRow *rows = (FlRow *)calloc((size_t)no + 1, sizeof *rows); int nr = 0, npro = 0, ncon = 0;
    for (uint32_t k = 0; k < no; k++) {
        KyOcc x; ky_occ_at(v, seg, o0 + k, &x); if (x.line < lo || x.line > hi) continue;
        int dup = -1; for (int i = 0; i < nr; i++) if (rows[i].o.role == x.role && rows[i].o.kl == x.kl && !memcmp(rows[i].o.key, x.key, x.kl) && rows[i].o.meth == x.meth) dup = i;
        if (dup >= 0) { rows[dup].n++; continue; }
        rows[nr].o = x; rows[nr].n = 1; nr++; if (x.role == CKR_PRODUCE) npro++; else ncon++;
    }
    int linked = 0;
    for (int r = 0; r < nr; r++) {
        FlRow *R = &rows[r]; KyOccs oc = {0}; ky_occs_of(v, R->o.key, R->o.kl, &oc);
        const int nfiles = ky_count_files(oc.o, oc.n, 0), npf = ky_count_files(oc.o, oc.n, CKR_PRODUCE), ncf = ky_count_files(oc.o, oc.n, CKR_CONSUME);
        int bi = -1, bc = 0; FlLink bl; memset(&bl, 0, sizeof bl); int peers = 0;
        for (int i = 0; i < oc.n; i++) { const KyOcc *z = &oc.o[i]; if (z->role == R->o.role || ky_same_file(z, &R->o)) continue; peers++;
            FlLink L; if (R->o.role == CKR_PRODUCE) fl_pair(&R->o, z, nfiles, npf, ncf, ky_file_has(oc.o, oc.n, CKR_PRODUCE, z), &L); else fl_pair(z, &R->o, nfiles, npf, ncf, ky_file_has(oc.o, oc.n, CKR_PRODUCE, &R->o), &L);
            if (L.conf > bc) { bc = L.conf; bi = i; bl = L; } }
        if (bi >= 0) { const KyOcc *z = &oc.o[bi]; char w[140]; fl_where(v, z, w, sizeof w); snprintf(R->peer, sizeof R->peer, "%.*s:%u%s%s", (int)z->plen, z->path, z->line, w[0] ? " " : "", w);
            R->conf = bc; snprintf(R->how, sizeof R->how, "%s; %d peer%s elsewhere", bl.how, peers, peers == 1 ? "" : "s"); linked++; }
        else { R->conf = 0; /* nenhum par em outro arquivo */ int same = 0; for (int i = 0; i < oc.n; i++) if (oc.o[i].role != R->o.role && ky_same_file(&oc.o[i], &R->o)) same = 1;
            snprintf(R->peer, sizeof R->peer, "%s", same ? "only in this same file" : R->o.role == CKR_PRODUCE ? "nobody handles it in the indexed set" : "nobody sends it in the indexed set"); R->how[0] = 0; }
        free(oc.o);
    }
    qsort(rows, (size_t)nr, sizeof *rows, fl_row_cmp);
    int total = nr;
    for (int r = 0; r < nr && items.n < a->limit; r++) { const FlRow *R = &rows[r];
        SyItem *it = sy_item_new(&items); sy_set_path(it, R->o.path, R->o.plen); it->line0 = it->line1 = (int)R->o.line;
        snprintf(it->kind, sizeof it->kind, "%s", R->o.role == CKR_PRODUCE ? "produce" : "consume"); fl_key_label(&R->o, it->name, sizeof it->name); snprintf(it->vis, sizeof it->vis, "%s", FL_CLS[R->o.cls]);
        snprintf(it->sig, sizeof it->sig, "%s", R->peer); it->conf = R->conf ? R->conf : 1; if (!R->conf) it->flags |= SYI_NOCONF;
        snprintf(it->how, sizeof it->how, "%s", R->how[0] ? R->how : "no peer in other files");
        if (R->n > 1) snprintf(it->encl, sizeof it->encl, "%d occurrences", R->n);
        else fl_where(v, &R->o, it->encl, sizeof it->encl);
    }
    snprintf(o.hint, sizeof o.hint, "%s: %d key%s sent, %d received; %d with a peer in another file.", rel, npro, npro == 1 ? "" : "s", ncon, linked);
    if (total > items.n) { size_t l = strlen(o.hint); snprintf(o.hint + l, sizeof o.hint - l, " (%d more not shown; raise --limit)", total - items.n); }
    snprintf(nf, sizeof nf, "'%s'%s has no text keys (nothing it sends, emits, writes, handles or reads by literal name)", rel, rng);
    fl_emit(v, &o, &items, total, nf);
    if (!o.json && total) {
        printf("%s%s  [%s]\n", rel, rng, sy_langname(rows[0].o.lang));
        for (int pass = 0; pass < 2; pass++) {
            int cnt = 0; for (int i = 0; i < items.n; i++) if (!strcmp(items.v[i].kind, pass ? "consume" : "produce")) cnt++;
            if (!cnt) continue; printf("%s\n", pass ? "RECEIVES (reads / handles)" : "SENDS (produces / writes / calls)");
            for (int i = 0; i < items.n; i++) { const SyItem *it = &items.v[i]; if (strcmp(it->kind, pass ? "consume" : "produce")) continue;
                int cls = 0; for (int k = 0; k < CKC_N; k++) if (!strcmp(it->vis, FL_CLS[k])) cls = k;
                if (it->flags & SYI_NOCONF) printf("  %d  %s [%s]  -- %s", it->line0, it->name, it->vis, it->sig);
                else printf("  %d  %s [%s]  %s %s   [confidence %d %s]", it->line0, it->name, it->vis, pass ? FL_CL[cls] : FL_PL[cls], it->sig, it->conf, sy_level(it->conf));
                printf("\n"); }
        }
        printf("%s\n", o.hint);
        for (int i = 0; i < items.n; i++) if (!(items.v[i].flags & SYI_NOCONF) && !strcmp(items.v[i].kind, "produce")) { printf("Next: flow %s for the full list of a key; Read the peer range above.\n", items.v[i].name); break; }
    }
    sy_items_free(&items); free(rows); return total ? 0 : 1;
}

/* ---- flow --links ---- */
typedef struct { const char *pp, *cp; uint32_t pl, cl; const char *key; uint32_t kl; int conf, cls; } FlEdge;
static int fl_edge_cmp(const void *a, const void *b) { const FlEdge *x = (const FlEdge *)a, *y = (const FlEdge *)b; int c = st_cmp_bytes(x->pp, x->pl, y->pp, y->pl); if (c) return c; c = st_cmp_bytes(x->cp, x->cl, y->cp, y->cl); if (c) return c; return y->conf - x->conf; }
static int fl_key_run_cmp(const void *a, const void *b) { const KyOcc *x = (const KyOcc *)a, *y = (const KyOcc *)b; int c = st_cmp_bytes(x->key, x->kl, y->key, y->kl); if (c) return c; if (x->cls != y->cls) return (int)x->cls - (int)y->cls; return sy_cmp_path_line(x->path, x->plen, (int)x->line, y->path, y->plen, (int)y->line); }
typedef struct { const char *pp, *cp; uint32_t pl, cl; int nk, best, first; char keys[4][70]; int cls; } FlGroup;
static int fl_group_cmp(const void *a, const void *b) { const FlGroup *x = (const FlGroup *)a, *y = (const FlGroup *)b; if (x->nk != y->nk) return y->nk - x->nk; return y->best - x->best; }
static int fl_links_main(SyView *v, SyArgs *a) {
    KyOccs all = {0}; ky_all(v, &all); qsort(all.o, (size_t)all.n, sizeof(KyOcc), fl_key_run_cmp);
    FlEdge *ed = NULL; int ne = 0, ec = 0, nkeys = 0, nlinked = 0;
    for (int i = 0; i < all.n; ) {
        int j = i; while (j < all.n && all.o[j].kl == all.o[i].kl && !memcmp(all.o[j].key, all.o[i].key, all.o[i].kl) && all.o[j].cls == all.o[i].cls) j++;
        nkeys++;
        const KyOcc *g = all.o + i; int n = j - i, np = 0, nc = 0; for (int k = 0; k < n; k++) { if (g[k].role == CKR_PRODUCE) np++; else nc++; }
        if (np && nc && (long)np * nc <= 3000) {
            const int nfiles = ky_count_files(g, n, 0), npf = ky_count_files(g, n, CKR_PRODUCE), ncf = ky_count_files(g, n, CKR_CONSUME); int any = 0;
            for (int p = 0; p < n; p++) { if (g[p].role != CKR_PRODUCE) continue; for (int c = 0; c < n; c++) { if (g[c].role != CKR_CONSUME || ky_same_file(&g[p], &g[c])) continue;
                FlLink L; fl_pair(&g[p], &g[c], nfiles, npf, ncf, ky_file_has(g, n, CKR_PRODUCE, &g[c]), &L); if (L.conf < 45) continue;
                if (ne == ec) { ec = ec ? ec * 2 : 256; ed = (FlEdge *)realloc(ed, sizeof(FlEdge) * (size_t)ec); }
                ed[ne].pp = g[p].path; ed[ne].pl = g[p].plen; ed[ne].cp = g[c].path; ed[ne].cl = g[c].plen; ed[ne].key = g[p].key; ed[ne].kl = g[p].kl; ed[ne].conf = L.conf; ed[ne].cls = (int)g[p].cls; ne++; any = 1; } }
            nlinked += any;
        }
        i = j;
    }
    qsort(ed, (size_t)ne, sizeof *ed, fl_edge_cmp);
    FlGroup *gr = (FlGroup *)calloc((size_t)ne + 1, sizeof *gr); int ng = 0;
    for (int i = 0; i < ne; i++) {
        FlGroup *G = ng ? &gr[ng - 1] : NULL;
        if (!G || G->pl != ed[i].pl || G->cl != ed[i].cl || memcmp(G->pp, ed[i].pp, ed[i].pl) || memcmp(G->cp, ed[i].cp, ed[i].cl)) { G = &gr[ng++]; G->pp = ed[i].pp; G->pl = ed[i].pl; G->cp = ed[i].cp; G->cl = ed[i].cl; G->first = i; }
        int dup = 0; for (int k = G->first; k < i && !dup; k++) if (ed[k].kl == ed[i].kl && !memcmp(ed[k].key, ed[i].key, ed[i].kl)) dup = 1;      /* a mesma chave em varias linhas conta uma vez */
        if (!dup) { if (G->nk < 4) { snprintf(G->keys[G->nk], sizeof G->keys[0], "%.*s", ed[i].kl < 68 ? (int)ed[i].kl : 68, ed[i].key); } G->nk++; }
        if (ed[i].conf > G->best) { G->best = ed[i].conf; G->cls = ed[i].cls; }
    }
    qsort(gr, (size_t)ng, sizeof *gr, fl_group_cmp);
    FlQ o; memset(&o, 0, sizeof o); o.query = "flow"; o.arg = "--links"; o.json = a->json; SyItems items = {0};
    for (int i = 0; i < ng && items.n < a->limit; i++) { const FlGroup *G = &gr[i];
        SyItem *it = sy_item_new(&items); sy_set_path(it, G->pp, G->pl); it->line0 = it->line1 = 1; snprintf(it->kind, sizeof it->kind, "link");
        char ks[260] = ""; for (int k = 0; k < G->nk && k < 4; k++) { size_t l = strlen(ks); snprintf(ks + l, sizeof ks - l, "%s%s", k ? ", " : "", G->keys[k]); } if (G->nk > 4) { size_t l = strlen(ks); snprintf(ks + l, sizeof ks - l, " (+%d)", G->nk - 4); }
        snprintf(it->name, sizeof it->name, "%.*s", (int)(sizeof it->name - 1), ks); snprintf(it->sig, sizeof it->sig, "%.*s", (int)G->cl, G->cp);
        it->conf = G->best; snprintf(it->how, sizeof it->how, "%d key%s link%s them (best: %s key; line text not reread in this summary)", G->nk, G->nk == 1 ? "" : "s", G->nk == 1 ? "s" : "", FL_CLS[G->cls]); }
    snprintf(o.hint, sizeof o.hint, "%d file pair%s linked by %d shared text key%s (of %d distinct keys with occurrences); only links at confidence >= 45 are counted. Next: flow <key> for the lines, flow <file> for one file.", ng, ng == 1 ? "" : "s", nlinked, nlinked == 1 ? "" : "s", nkeys);
    fl_emit(v, &o, &items, ng, "no text-key links between files in the indexed set (no key is produced in one file and consumed in another)");
    if (!o.json && ng) {
        for (int i = 0; i < items.n; i++) { const SyItem *it = &items.v[i]; printf("%s -> %s   %s   [confidence %d %s]\n", it->path, it->sig, it->name, it->conf, sy_level(it->conf)); }
        if (ng > items.n) printf("(%d more not shown; raise --limit)\n", ng - items.n);
        printf("%s\n", o.hint);
    }
    sy_items_free(&items); free(gr); free(ed); free(all.o); return ng ? 0 : 1;
}

static int coder_flow_main(int argc, char **argv) {
    SyArgs a; SyView v; int links = 0; int rc = fl_start("flow <chave|arquivo[:linha]> [pasta] [--json] [--limit N]  |  flow --links [pasta]", argc, argv, &a, &v, &links); if (rc) return rc;
    int ret;
    if (links) ret = fl_links_main(&v, &a);
    else { char spec[4400]; int line; if (fl_is_file(&v, a.pos[0], spec, sizeof spec, &line)) ret = fl_file_main(&v, &a, spec, line); else ret = fl_key_main(&v, &a, a.pos[0]); }
    sy_view_close(&v); return ret;
}
