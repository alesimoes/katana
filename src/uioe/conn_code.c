/* ---------- uioe/conn_code.c: os conectores de codigo (docs/uioe.md 3.8.2 e 3.8.4, fase U3) ----------
 * Cada conector chama a API estruturada do coder (sy_q_*, fl_q_*: docs/code-intelligence.md 4.1) sobre UMA SyView aberta por pedido em
 * UioeCtx, ou a busca por palavras (coder_lookup), e converte o resultado em nos e arestas. Nada imprime, nada captura stdout.
 *   code.search   onde esta, por palavras, nomes ou texto entre aspas (coder_lookup -> cs_search_core)
 *   code.def      definicoes de um nome pela cascata (exato, derivado, provavel)       code.callers  quem chama     code.refs  quem usa (nao chamadas)
 *   code.callees  o que um simbolo chama                                               code.outline  simbolos de um arquivo, ou o que contem uma linha
 *   code.deps     imports e importadores de um arquivo                                 code.impact   fecho de chamadores (profundidade, nos)
 *   code.flow     ligacoes por chave de texto: uma chave (onde e enviada e tratada) ou um arquivo (as chaves dele e o outro lado)
 * Precondicao por nivel do indice (3.8.2): code.search precisa do armazenamento; code.def e os estruturais precisam das secoes de simbolos;
 * code.flow precisa das secoes de chaves. O que falta devolve UR_UNAVAILABLE com o motivo, e o pipeline desce a escada (3.5.2).
 * Decisoes do implementador: a confianca de um no e a do item do coder (cascata, ambiguidade, verificacao do proprio coder); a ligacao por
 * chave vira UMA aresta UE_LINKS do produtor ao consumidor, com os dois lados como nos de chave; o tamanho de cada resultado e limitado pelo
 * `limit` da chamada e o resto aparece em `total` (nunca cortado em silencio: o pipeline diz "N more not shown"). */
/* ---- visao do indice: uma por pedido ---- */
static int uioe_open_view(UioeCtx *cx) {
    if (cx->view_state == 1) return 1;
    if (cx->view_state < 0 || !cx->has_store) return 0;
    char err[300] = ""; SyView v;
    if (sy_view_open(cx->root, &v, err, sizeof err) != 0) { cx->view_state = -1; snprintf(cx->view_note, sizeof cx->view_note, "index not readable: %.200s", err); return 0; }
    cx->view = v; cx->view_state = 1; cx->have_sym = v.any; cx->have_keys = v.anykeys;
    return 1;
}
static void uioe_close_view(UioeCtx *cx) { if (cx->view_state == 1) sy_view_close(&cx->view); cx->view_state = 0; }
static int cc_view(UioeCtx *cx, UioeResult *r) {
    if (uioe_open_view(cx)) return 1;
    r->status = UR_UNAVAILABLE; snprintf(r->note, sizeof r->note, "%s", cx->view_note[0] ? cx->view_note : "no index store for this folder"); return 0;
}
static int cc_need_sym(UioeCtx *cx, UioeResult *r) {
    if (!cc_view(cx, r)) return 0;
    if (!cx->have_sym) { r->status = UR_UNAVAILABLE; snprintf(r->note, sizeof r->note, "the index has no symbol sections (partial index)"); snprintf(r->limits, sizeof r->limits, "symbols not indexed here"); return 0; }
    return 1;
}
static int cc_need_keys(UioeCtx *cx, UioeResult *r) {
    if (!cc_view(cx, r)) return 0;
    if (!cx->have_keys) { r->status = UR_UNAVAILABLE; snprintf(r->note, sizeof r->note, "the index has no text-key sections (partial index)"); snprintf(r->limits, sizeof r->limits, "text-key links are not indexed here"); return 0; }
    return 1;
}
/* "in nome (a-b)" -> nome e faixa */
static int cc_where(const char *s, char *name, size_t cap, int *a, int *b) {
    name[0] = 0; *a = *b = 0; if (!s) return 0;
    if (!strncmp(s, "in ", 3)) s += 3;
    const char *p = strrchr(s, '('); if (!p) return 0;
    size_t l = (size_t)(p - s); while (l && s[l - 1] == ' ') l--; if (l >= cap) l = cap - 1;
    memcpy(name, s, l); name[l] = 0;
    return sscanf(p, "(%d-%d)", a, b) >= 1;
}
static void cc_snip_text(const SyItem *it, char *o, size_t cap) {
    o[0] = 0; if (!it->snip) return;
    const char *p = it->snip; while (isdigit((unsigned char)*p)) p++;
    if (*p == ':') p++;
    while (*p == ' ') p++;
    snprintf(o, cap, "%.*s", (int)(strcspn(p, "\n") < cap - 1 ? strcspn(p, "\n") : cap - 1), p);
}
/* o item do coder preenche um no base (caminho, faixa, nome, confianca, verificacao propria, como) */
static UioeNode *cc_base(UioeResult *r, const SyItem *it, int kind, int step, int tier) {
    UioeNode *n = ur_node(r); if (!n) return NULL;
    n->kind = (uint8_t)kind; snprintf(n->path, sizeof n->path, "%s", it->path); n->line0 = it->line0; n->line1 = it->line1 > it->line0 ? it->line1 : it->line0;
    snprintf(n->name, sizeof n->name, "%s", it->name); snprintf(n->cls, sizeof n->cls, "%s", it->kind); snprintf(n->sig, sizeof n->sig, "%s", it->sig[0] ? it->sig : it->encl);
    n->conf = it->conf; n->step = step ? step : 1; n->tier = tier; n->cver = it->ver > 0 ? 1 : it->ver < 0 ? -1 : 0;
    snprintf(n->how, sizeof n->how, "%s", it->how); snprintf(n->vkey, sizeof n->vkey, "%s", it->name);
    char t[240]; cc_snip_text(it, t, sizeof t); ur_set_text(n, t);
    return n;
}
static int cc_tier(const UioeCall *q, int step) { if (step == 2) return 3; if (step >= 3) return 4; return q->tier ? q->tier : 2; }       /* derivado e provavel nunca sobem de faixa por dica do pipeline */
static void cc_meta(UioeResult *r, const SyItems *it, int total, const char *notfound, const char *limits, int step) {
    r->total = total; r->step = step; snprintf(r->limits, sizeof r->limits, "%.399s", limits); snprintf(r->notfound, sizeof r->notfound, "%.299s", notfound);
    r->status = it->n ? UR_OK : UR_EMPTY;
}

/* arg2 "match=a b c": palavras que o nome ou a assinatura do simbolo devem conter (alguma) para o simbolo entrar no resultado */
static int cc_match(const char *arg2, const char *name, const char *sig) {
    if (strncmp(arg2, "match=", 6)) return 1;
    char hay[300]; snprintf(hay, sizeof hay, "%s %s", name, sig);
    char w[220]; snprintf(w, sizeof w, "%s", arg2 + 6);
    for (char *p = strtok(w, " "); p; p = strtok(NULL, " ")) if (ug_has_word(hay, p)) return 1;
    return 0;
}
/* ---- code.search ---- */
static int cc_search(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    snprintf(r->source, sizeof r->source, "code.search");
#ifdef _WIN32
    (void)cx; (void)q; r->status = UR_UNAVAILABLE; snprintf(r->note, sizeof r->note, "the code search is not built on this platform"); return r->status;
#else
    if (!cx->has_store) { r->status = UR_UNAVAILABLE; snprintf(r->note, sizeof r->note, "no index store for this folder"); return r->status; }
    if (!cx->refresh && cx->idx_mode == 1) { r->status = UR_UNAVAILABLE; snprintf(r->note, sizeof r->note, "the index is partial and --no-refresh is set: the search would have to finish building it (coder_lookup updates the index)"); return r->status; }
    CoderHits *ch = (CoderHits *)calloc(1, sizeof *ch); if (!ch) { r->status = UR_ERROR; return r->status; }
    char qs[460]; if (!strcmp(q->arg2, "literal")) snprintf(qs, sizeof qs, "\"%s\"", q->arg); else snprintf(qs, sizeof qs, "%s", q->arg);
    if (coder_lookup(cx->root, qs, 1, q->limit > 0 ? q->limit : 8, ch) != 0) { free(ch); r->status = UR_UNAVAILABLE; snprintf(r->note, sizeof r->note, "the coder index could not be read"); return r->status; }
    for (int i = 0; i < ch->n; i++) {
        const CoderHit *h = &ch->r[i];
        const int islit = !strcmp(q->arg2, "literal");           /* consulta de texto exato: so o canal literal prova que o texto esta no arquivo; o resto e ordem por palavras */
        const int proven = islit ? h->literal : (h->exact || h->literal);
        const int conf = proven ? 85 : h->tier <= 1 ? 70 : h->tier == 2 ? 55 : 40;
        const int tier = proven ? (q->tier ? q->tier : 3) : 4;
        UioeNode *f = ur_node(r); if (!f) break;
        f->kind = UN_FILE; snprintf(f->path, sizeof f->path, "%s", h->path); f->line0 = f->line1 = 1; f->conf = islit && !proven ? 40 : conf; f->step = 1; f->tier = tier; f->cver = 0; if (islit && h->literal) snprintf(f->cls, sizeof f->cls, "literal");
        snprintf(f->how, sizeof f->how, "search channels: %.140s", h->channels);
        const int fi = r->nnode - 1;
        for (int j = 0; j < h->nev; j++) {
            UioeNode *e = ur_node(r); if (!e) break;
            e->kind = UN_RANGE; snprintf(e->path, sizeof e->path, "%s", h->path); e->line0 = e->line1 = h->line[j]; e->conf = islit && !proven ? 40 : conf > 60 ? 60 : conf; e->step = 1; e->tier = tier; if (islit && h->literal) snprintf(e->cls, sizeof e->cls, "literal");
            const char *tx = h->text[j]; const char *c = strstr(tx, ": "); if (!strncmp(tx, "line ", 5) && c) tx = c + 2;
            ur_set_text(e, tx); snprintf(e->vkey, sizeof e->vkey, "%.40s", e->text); snprintf(e->how, sizeof e->how, "evidence line of %.100s", h->path);
            ur_edge(r, fi, r->nnode - 1, UE_CONTAINS, h->line[j], conf, "evidence line", "");
        }
    }
    r->total = ch->total; r->step = 1; r->status = ch->n ? UR_OK : UR_EMPTY;
    snprintf(r->limits, sizeof r->limits, "search over %u indexed files; at most %d files returned; ranked by words, so a file that does not use the request's words is not found", ch->files, CODER_HITS_MAX);
    snprintf(r->notfound, sizeof r->notfound, "%.299s", ch->notfound[0] ? ch->notfound : "no file matches the terms");
    free(ch); return r->status;
#endif
}

/* ---- code.def ---- */
static int cc_def(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    snprintf(r->source, sizeof r->source, "code.def");
    if (!cc_need_sym(cx, r)) return r->status;
    SyItems it = {0}; SyQ m; sy_q_def(&cx->view, q->arg, q->limit > 0 ? q->limit : 8, 1, &it, &m);
    for (int i = 0; i < it.n; i++) {
        UioeNode *n = cc_base(r, &it.v[i], UN_SYMBOL, m.step, cc_tier(q, m.step)); if (!n) break;
        n->cls[0] = 0; snprintf(n->cls, sizeof n->cls, "%.11s", it.v[i].kind);
        n->anchor = 1;
    }
    cc_meta(r, &it, m.total, m.notfound, m.limits, m.step);
    if (m.step > 1 && it.n) snprintf(r->note, sizeof r->note, "no exact match for '%.60s'; %.120s", q->arg, m.cascade);
    sy_items_free(&it); return r->status;
}

/* ---- code.callers / code.refs ---- */
static int cc_refs_gen(UioeCtx *cx, const UioeCall *q, UioeResult *r, int calls) {
    snprintf(r->source, sizeof r->source, calls ? "code.callers" : "code.refs");
    if (!cc_need_sym(cx, r)) return r->status;
    SyItems it = {0}; SyQ m; const int lim = q->limit > 0 ? q->limit : 8;
    if (calls) sy_q_callers(&cx->view, q->arg, lim, &it, &m); else sy_q_uses(&cx->view, q->arg, lim, &it, &m);
    for (int i = 0; i < it.n; i++) {
        UioeNode *n = cc_base(r, &it.v[i], UN_RANGE, m.step, cc_tier(q, m.step)); if (!n) break;
        n->line1 = n->line0; snprintf(n->name, sizeof n->name, "%.99s", it.v[i].name); snprintf(n->sig, sizeof n->sig, "%.159s", it.v[i].encl);
        snprintf(n->cls, sizeof n->cls, "%.11s", it.v[i].kind);
        ur_edge(r, r->nnode - 1, -1, calls ? UE_CALLS : UE_USES, it.v[i].line0, it.v[i].conf, it.v[i].how, it.v[i].name);
    }
    cc_meta(r, &it, m.total, m.notfound, m.limits, m.step);
    sy_items_free(&it); return r->status;
}
static int cc_callers(UioeCtx *cx, const UioeCall *q, UioeResult *r) { return cc_refs_gen(cx, q, r, 1); }
static int cc_refs(UioeCtx *cx, const UioeCall *q, UioeResult *r) { return cc_refs_gen(cx, q, r, 0); }

/* ---- code.callees ---- */
static int cc_callees(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    snprintf(r->source, sizeof r->source, "code.callees");
    if (!cc_need_sym(cx, r)) return r->status;
    SyItems it = {0}; SyQ m; sy_q_callees(&cx->view, q->arg, q->limit > 0 ? q->limit : 12, &it, &m);
    for (int i = 0; i < it.n; i++) {
        const SyItem *x = &it.v[i]; char dp[400] = ""; int a = 0, b = 0; char kd[16] = "";
        if (sscanf(x->encl, "-> %399[^:]:%d-%d (%15[^)])", dp, &a, &b, kd) < 3) continue;
        if (!cc_match(q->arg2, x->name, "")) continue;
        UioeNode *n = ur_node(r); if (!n) break;
        n->kind = UN_SYMBOL; snprintf(n->path, sizeof n->path, "%s", dp); n->line0 = a; n->line1 = b; snprintf(n->name, sizeof n->name, "%.99s", x->name); snprintf(n->cls, sizeof n->cls, "%.11s", kd);
        n->conf = x->conf; n->step = m.step ? m.step : 1; n->tier = cc_tier(q, m.step); n->cver = 0; snprintf(n->vkey, sizeof n->vkey, "%.99s", x->name);
        snprintf(n->how, sizeof n->how, "callee of %.60s: %.100s", q->arg, x->how);
        ur_edge(r, -1, r->nnode - 1, UE_CALLS, x->line0, x->conf, x->how, x->name);
        /* o ponto da chamada tambem e uma faixa citavel: a linha dentro do chamador */
        UioeNode *s = ur_node(r); if (!s) break;
        s->kind = UN_RANGE; snprintf(s->path, sizeof s->path, "%s", x->path); s->line0 = s->line1 = x->line0; snprintf(s->name, sizeof s->name, "call %.90s", x->name); snprintf(s->vkey, sizeof s->vkey, "%.99s", x->name);
        s->conf = x->conf; s->step = n->step; s->tier = n->tier; s->cver = x->ver > 0 ? 1 : x->ver < 0 ? -1 : 0; snprintf(s->how, sizeof s->how, "%s", x->how);
        char t[240]; cc_snip_text(x, t, sizeof t); ur_set_text(s, t);
        ur_edge(r, r->nnode - 1, r->nnode - 2, UE_CONTAINS, x->line0, x->conf, "call site", "");
    }
    cc_meta(r, &it, m.total, m.notfound, m.limits, m.step);
    if (m.external[0]) snprintf(r->note, sizeof r->note, "external (no definition in the set): %.160s", m.external);
    sy_items_free(&it); return r->status;
}

/* ---- code.outline (arquivo) e at (arquivo + linha) ---- */
static int cc_outline(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    snprintf(r->source, sizeof r->source, "code.outline");
    if (!cc_need_sym(cx, r)) return r->status;
    SyItems it = {0}; SyQ m;
    if (q->line > 0) sy_q_at(&cx->view, q->arg, q->line, &it, &m); else sy_q_outline(&cx->view, q->arg, q->limit > 0 ? q->limit : 60, &it, &m);
    for (int i = 0; i < it.n; i++) {
        if (q->line == 0 && !cc_match(q->arg2, it.v[i].name, it.v[i].sig)) continue;
        UioeNode *n = cc_base(r, &it.v[i], UN_SYMBOL, 1, q->tier ? q->tier : 2); if (!n) break;
        n->conf = it.v[i].conf;
    }
    cc_meta(r, &it, m.total, m.notfound, m.limits, 1);
    sy_items_free(&it); return r->status;
}

/* ---- code.deps ---- */
static int cc_deps(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    snprintf(r->source, sizeof r->source, "code.deps");
    if (!cc_need_sym(cx, r)) return r->status;
    SyItems it = {0}; SyQ m; sy_q_deps(&cx->view, q->arg, q->limit > 0 ? q->limit : 20, &it, &m);
    /* o no do proprio arquivo e o 0 do resultado */
    UioeNode *self = ur_node(r); if (self) { self->kind = UN_FILE; snprintf(self->path, sizeof self->path, "%s", q->arg); self->line0 = self->line1 = 1; snprintf(self->name, sizeof self->name, "file"); self->conf = 90; self->tier = q->tier ? q->tier : 2; snprintf(self->how, sizeof self->how, "the file asked about"); }
    for (int i = 0; i < it.n && self; i++) {
        const SyItem *x = &it.v[i]; const int imported_by = !strcmp(x->kind, "imported-by");
        if (!strcmp(q->arg2, "importers") && !imported_by) continue;
        const char *other = x->encl; if (!strncmp(other, "-> ", 3) || !strncmp(other, "<- ", 3)) other += 3;
        UioeNode *n = ur_node(r); if (!n) break;
        if (imported_by) { n->kind = UN_RANGE; snprintf(n->path, sizeof n->path, "%s", x->path); n->line0 = n->line1 = x->line0; snprintf(n->name, sizeof n->name, "import %.90s", x->name); snprintf(n->vkey, sizeof n->vkey, "%.99s", x->name); }
        else { n->kind = UN_RANGE; snprintf(n->path, sizeof n->path, "%s", x->path); n->line0 = n->line1 = x->line0; snprintf(n->name, sizeof n->name, "import %.90s", x->name); snprintf(n->vkey, sizeof n->vkey, "%.99s", x->name); }
        snprintf(n->cls, sizeof n->cls, "%.11s", x->kind); snprintf(n->sig, sizeof n->sig, "%s %.150s", imported_by ? "imported by" : "imports", other);
        n->conf = x->conf; n->step = 1; n->tier = q->tier ? q->tier : 2; snprintf(n->how, sizeof n->how, "%s", x->how);
        UioeNode *o = ur_node(r); if (!o) break;
        o->kind = UN_FILE; snprintf(o->path, sizeof o->path, "%s", imported_by ? x->path : other); o->line0 = o->line1 = 1; snprintf(o->name, sizeof o->name, "file"); o->conf = x->conf; o->tier = n->tier; snprintf(o->how, sizeof o->how, "%s", x->how);
        const int ni = r->nnode - 2, oi = r->nnode - 1;
        ur_edge(r, imported_by ? oi : 0, imported_by ? 0 : oi, UE_IMPORTS, x->line0, x->conf, x->how, x->name);
        ur_edge(r, oi, ni, UE_CONTAINS, x->line0, x->conf, "import statement", "");
    }
    cc_meta(r, &it, m.total, m.notfound, m.limits, 1);
    if (m.external[0]) snprintf(r->note, sizeof r->note, "external: %.170s", m.external);
    sy_items_free(&it); return r->status;
}

/* ---- code.impact ---- */
static int cc_impact(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    snprintf(r->source, sizeof r->source, "code.impact");
    if (!cc_need_sym(cx, r)) return r->status;
    SyItems it = {0}; SyQ m; const int nodes = q->limit > 0 ? q->limit : 25; sy_q_impact(&cx->view, q->arg, q->depth > 0 ? q->depth : 3, nodes, nodes, &it, &m);
    int stack[16]; for (int i = 0; i < 16; i++) stack[i] = -1;
    for (int i = 0; i < it.n; i++) {
        const SyItem *x = &it.v[i];
        UioeNode *n = cc_base(r, x, UN_SYMBOL, m.step, q->tier ? q->tier : 2); if (!n) break;
        n->cver = 0; snprintf(n->cls, sizeof n->cls, "caller"); snprintf(n->how, sizeof n->how, "caller at depth %d; confidence of the weakest edge on the path", x->depth + 1);
        char callee[100] = ""; int cl = 0; { const char *p = strstr(x->encl, "calls "); if (p) { p += 6; const char *e = strstr(p, " at line "); size_t l = e ? (size_t)(e - p) : 0; if (l && l < sizeof callee) { memcpy(callee, p, l); callee[l] = 0; cl = atoi(e + 9); } } }
        const int d = x->depth < 15 ? x->depth : 15; const int me = r->nnode - 1;
        const int parent = d == 0 ? -1 : stack[d - 1];
        ur_edge(r, me, parent, UE_CALLS, cl, x->conf, x->how, callee);
        stack[d] = me; for (int z = d + 1; z < 16; z++) stack[z] = -1;
    }
    cc_meta(r, &it, m.total, m.notfound, m.limits, m.step);
    sy_items_free(&it); return r->status;
}

/* ---- code.flow ---- */
/* a chave sem o metodo de rota ("POST /x" -> "/x"), para a verificacao */
static void cc_key_text(const char *label, char *o, size_t cap) {
    const char *sp = strchr(label, ' ');
    if (sp && isupper((unsigned char)label[0]) && sp - label <= 8 && sp[1] == '/') snprintf(o, cap, "%s", sp + 1); else snprintf(o, cap, "%s", label);
}
static int cc_flow_node(UioeCtx *cx, UioeResult *r, const char *path, int line, const char *label, const char *cls, const char *where, int conf, int tier, const char *how, const char *text) {
    UioeNode *n = ur_node(r); if (!n) return -1;
    n->kind = UN_KEY; snprintf(n->path, sizeof n->path, "%s", path); n->line0 = n->line1 = line; snprintf(n->name, sizeof n->name, "%.99s", label); snprintf(n->cls, sizeof n->cls, "%.11s", cls);
    snprintf(n->sig, sizeof n->sig, "%.159s", where); n->conf = conf; n->step = 1; n->tier = tier; n->cver = 0;
    cc_key_text(label, n->vkey, sizeof n->vkey); snprintf(n->how, sizeof n->how, "%.169s", how);
    if (text && text[0]) ur_set_text(n, text);
    else if (uv_load(cx, path) == 1) { char t[240]; if (uv_line(cx, line, t, sizeof t)) ur_set_text(n, t); }       /* o outro lado da ligacao: a linha lida agora, para a pontuacao e a citacao */
    return r->nnode - 1;
}
static int cc_flow(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    snprintf(r->source, sizeof r->source, "code.flow");
    if (!cc_need_keys(cx, r)) return r->status;
    SyItems it = {0}; FlQ m; const int lim = q->limit > 0 ? q->limit : 20; const int tier = q->tier ? q->tier : 2;
    const int filemode = !strcmp(q->arg2, "file");
    if (filemode) fl_q_file(&cx->view, q->arg, q->line, lim, &it, &m); else fl_q_key(&cx->view, q->arg, lim, &it, &m);
    char first[100]; snprintf(first, sizeof first, "%s", q->arg); { char *c = strchr(m.resolved, ','); if (!filemode && m.resolved[0]) snprintf(first, sizeof first, "%.*s", c ? (int)(c - m.resolved) : 99, m.resolved); }
    for (int i = 0; i < it.n; i++) {
        const SyItem *x = &it.v[i]; char txt[240]; cc_snip_text(x, txt, sizeof txt);
        const int isprod = !strcmp(x->kind, "produce"), iscons = !strcmp(x->kind, "consume"), islink = !strcmp(x->kind, "link");
        if (islink) {                                           /* modo chave: produtor em path:line, consumidor em peer_* */
            const int a = cc_flow_node(cx, r, x->path, x->line0, first, "", "", x->conf, tier, x->how, txt);
            char pw[200]; snprintf(pw, sizeof pw, "%s", x->peer_where);
            const int b = cc_flow_node(cx, r, x->peer_path, x->peer_line, first, "", pw, x->conf, tier, x->how, NULL);
            if (a >= 0 && b >= 0) { ur_edge(r, a, b, UE_LINKS, x->line0, x->conf, x->how, first); r->node[a].cver = x->ver > 0 ? 1 : x->ver < 0 ? -1 : 0; r->node[b].cver = r->node[a].cver; }
            continue;
        }
        if (!isprod && !iscons) continue;
        if (q->depth == 1 && (!x->peer_path[0] || (x->flags & SYI_NOCONF))) continue;        /* depth 1: so as chaves que ligam a outro arquivo (as ancoras de um fluxo) */
        char hw[200]; snprintf(hw, sizeof hw, "%s; %.150s", isprod ? "writes, sets or sends it" : "reads or handles it", x->how);
        const int self = cc_flow_node(cx, r, x->path, x->line0, x->name, x->vis, x->encl, x->conf, tier, hw, txt);
        if (self < 0) break;
        if (x->peer_path[0] && !(x->flags & SYI_NOCONF)) {
            const int peer = cc_flow_node(cx, r, x->peer_path, x->peer_line, x->name, x->vis, x->peer_where, x->conf, tier, x->how, NULL);
            if (peer >= 0) { if (isprod) ur_edge(r, self, peer, UE_LINKS, x->line0, x->conf, x->how, x->name); else ur_edge(r, peer, self, UE_LINKS, x->peer_line, x->conf, x->how, x->name); }
        } else if (x->peer_path[0] == 0 && (x->flags & SYI_NOCONF)) {
            snprintf(r->node[self].how, sizeof r->node[self].how, "%s; %.100s", x->how, x->sig);       /* "nobody handles it in the indexed set" / "only in this same file" */
            r->node[self].conf = 0; r->node[self].cver = 0;
        } else if (x->peer_path[0] == 0 && !(x->flags & SYI_NOCONF)) r->node[self].cver = x->ver > 0 ? 1 : x->ver < 0 ? -1 : 0;
    }
    r->total = m.total; r->step = m.step; snprintf(r->limits, sizeof r->limits, "%.399s", m.limits); snprintf(r->notfound, sizeof r->notfound, "%.299s", m.notfound);
    r->status = it.n ? UR_OK : UR_EMPTY;
    if (m.hint[0]) snprintf(r->note, sizeof r->note, "%.199s", m.hint);
    sy_items_free(&it); return r->status;
}

/* ---- ligacao dos conectores aos ids do registro ---- */
static void uioe_bind_runs(UioeSession *s) {
    static const struct { const char *id; int (*run)(UioeCtx *, const UioeCall *, UioeResult *); } T[] = {
        {"code.search", cc_search}, {"code.def", cc_def}, {"code.callers", cc_callers}, {"code.refs", cc_refs}, {"code.callees", cc_callees},
        {"code.outline", cc_outline}, {"code.deps", cc_deps}, {"code.impact", cc_impact}, {"code.flow", cc_flow},
        {"file.grep", cf_grep}, {"file.glob", cf_glob}, {"place.resolve", cf_place}, {"docs.search", cf_docs}, {"docs.select", cf_docsel}, {"memory.recall", cm_recall}, {"web.search", cw_search}};
    for (int i = 0; i < s->nreg; i++) for (size_t k = 0; k < sizeof T / sizeof *T; k++) if (!strcmp(s->reg[i].id, T[k].id)) s->reg[i].run = T[k].run;
}
