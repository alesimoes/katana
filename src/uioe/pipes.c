/* ---------- uioe/pipes.c: os pipelines por tipo de tarefa (docs/uioe.md 3.7.2, fase U3) ----------
 * Cada pipeline executa os passos do plano de rota (route.c) sobre os conectores: ancoras, expansao, cadeia. Aqui nada verifica nem decide a
 * porta: isso e da maquina de estados (orch.c). Um pipeline so le e acrescenta ao grafo; o que ele nao conseguiu vira lacuna (`rc_limit`,
 * `rc_hyp`), nunca um item inventado.
 *   locate   uma entidade (definicao, arquivo, chave, literal) ou uma busca por palavras, e quem chama as duas primeiras
 *   usage    definicao; chamadores, usos ou chamados conforme o bit de relacao; importadores se o alvo e um arquivo; sem definicao, o nome
 *            como literal e como chave de texto (a chave que o `refs` do Find nao enxerga)
 *   impact   definicao; fecho de chamadores (profundidade 3, 25 nos); as chaves que o arquivo envia/trata e o outro lado; importadores;
 *            testes que chamam, a parte
 *   flow     a cadeia do caso medido: ancoras -> ligacoes por chave dos arquivos ancora -> pontuacao para a pergunta -> cadeia de ate 4 saltos
 *            (chave, importador, chamado) -> documento que cita dois arquivos da cadeia
 *   config   onde a variavel/chave e definida e lida (classes env, config, cmd), valores de arquivos secretos nunca; a linha que a documenta
 *   error    o texto do erro como literal (exato, depois sem numeros e caminhos), o simbolo que o contem, as linhas de guarda acima dele,
 *            os chamadores em 2 niveis
 *   howto    documentos do projeto (docs.select dentro do agente, docs.search fora); receitas e skills ainda nao existem (U8)
 *   overview a listagem da raiz e os titulos dos documentos
 * Decisoes do implementador:
 *   - a busca por palavras usa os termos normalizados e ate 2 palavras em ingles/codigo de cada conceito (a base de conhecimento), sem os
 *     nomes que o pedido deu a pasta ou ao projeto (eles ocorrem em tudo); o pedido original nunca e a consulta.
 *   - sem indice, o degrau 5 da escada: K = 3, varredura de arquivos pelos 3 termos mais especificos; o limite diz que foi varredura.
 *   - a relevancia de uma ligacao so ordena (direcao, termos do pedido nas linhas e nos simbolos, os dois lados entre os candidatos);
 *     a confianca continua sendo a do coder.
 *   - o salto por importador (UE_VIA) tem confianca 55 por natureza: o arquivo importa o outro e um simbolo seu casa com os termos. */
typedef struct {
    UioeCtx *cx; UioeRecall *rc; UioeGraph *g; const UioeAsk *a; const UioeClass *k; UioeRoute *rt;
    const char *st;                           /* estado da maquina: "P3", "P4" */
    char terms[640];                          /* a consulta por palavras: os termos do pedido, normalizados */
    char terms_b[640];                        /* a segunda consulta: o equivalente em ingles/codigo (conceitos da base de conhecimento) e a palavra da direcao */
    int afile[16]; int naf;                   /* nos de arquivo ancora (da busca) */
    int anch[24]; int nanch;                  /* nos ancora: simbolos, chaves, faixas */
    int entity_hit, entity_miss, structural;
    int req_missing;
    int bad[8]; int nbad;                     /* arestas derrubadas pela verificacao */
    int erel[UG_MAXEDGE];
    uint64_t limseen;
    int qflags;
} UioeOrch;

/* ---- utilitarios ---- */
static const char *po_base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static int po_is_doc(const char *path) {
    const char *dot = strrchr(po_base(path), '.'); if (!dot) return 0;
    return !strcasecmp(dot, ".md") || !strcasecmp(dot, ".txt") || !strcasecmp(dot, ".rst") || !strcasecmp(dot, ".adoc") || !strcasecmp(dot, ".markdown");
}
static int po_is_test(const UioeOrch *o, const char *path) { return !(o->qflags & 2) && fd_is_test(path); }
static int po_bad_edge(const UioeOrch *o, int e) { for (int i = 0; i < o->nbad; i++) if (o->bad[i] == e) return 1; return 0; }
static void po_anchor(UioeOrch *o, int id) {
    if (id < 0) return;
    for (int i = 0; i < o->nanch; i++) if (o->anch[i] == id) return;
    if (o->nanch < 24) { o->anch[o->nanch++] = id; o->g->n[id].anchor = 1; }
}
static int po_step_find(UioeRoute *rt, const char *label) { for (int i = 0; i < rt->nstep; i++) if (!strcmp(rt->step[i].label, label)) return i; return -1; }

/* uma chamada de conector no estado e passo atuais; marca o passo do plano e junta os limites do conector (uma vez por conector) */
static int po_run(UioeOrch *o, const char *label, const char *conn, const UioeCall *q, UioeResult *res, int *map, int required) {
    const int st = uioe_call(o->cx, o->rc, o->st, label, conn, q, res, map, 256);
    int pi = po_step_find(o->rt, label);
    if (pi < 0) for (int i = 0; i < o->rt->nstep; i++) if (!o->rt->step[i].ran && !strcmp(o->rt->step[i].conn, conn)) { pi = i; break; }       /* o rotulo do pipeline nao e o do plano: o primeiro passo do plano que usa este conector */
    if (pi >= 0) { UioeStep *s = &o->rt->step[pi]; s->ran = 1; s->rms += res->ms; s->rnodes += res->nnode; s->rcalls++; s->rstatus = st; if (res->note[0]) snprintf(s->rnote, sizeof s->rnote, "%.159s", res->note); }
    const int ix = uioe_find_conn(o->cx->s, conn);
    if (pi >= 0) { UioeStep *s = &o->rt->step[pi]; if (!strstr(s->rconn, conn)) { const size_t l = strlen(s->rconn); if (l + strlen(conn) + 3 < sizeof s->rconn) snprintf(s->rconn + l, sizeof s->rconn - l, "%s%s", l ? "," : "", conn); } }
    const int grp = (!strncmp(conn, "code.", 5) && strcmp(conn, "code.search") && strcmp(conn, "code.flow")) ? 60 : ix;
    if (st == UR_UNAVAILABLE || st == UR_TIMEOUT || st == UR_ERROR) {
        if (required) o->req_missing++;
        rc_limit(o->rc, "%s: %s%.160s", conn, st == UR_UNAVAILABLE ? "not available: " : st == UR_TIMEOUT ? "timeout: " : "error: ", res->note);
    } else if (res->limits[0] && grp >= 0 && grp < 64 && !(o->limseen >> grp & 1)) { o->limseen |= 1ull << grp; rc_limit(o->rc, "%s: %.190s", grp == 60 ? "code.symbols (def, callers, callees, outline, deps, impact)" : conn, res->limits); }
    return st;
}
static int po_usable(int st) { return st == UR_OK || st == UR_PARTIAL || st == UR_TIMEOUT; }

/* ---- a consulta por palavras ---- */
static int po_is_place_word(const UioeAsk *a, const char *w) {
    for (int i = 0; i < a->nent; i++) { if (a->ent[i].kind != UK_FOLDER && a->ent[i].kind != UK_NAME) continue; char t[200]; snprintf(t, sizeof t, "%s", a->ent[i].text); for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (strstr(t, w) && strlen(w) >= 3) return 1; }
    return 0;
}
static void po_terms(UioeOrch *o) {
    const UioeAsk *a = o->a; size_t n = 0, m = 0; o->terms[0] = 0; o->terms_b[0] = 0; int count = 0;
    for (int i = 0; i < a->nterms && count < 12; i++) {
        if (po_is_place_word(a, a->terms[i]) || po_is_place_word(a, a->norm[i])) continue;
        n += (size_t)snprintf(o->terms + n, sizeof o->terms - n, "%s%s", n ? " " : "", a->norm[i]); count++;
        int added = 0;                                       /* B: a palavra nao portuguesa de maior peso do conceito; sem conceito, o proprio termo */
        for (int j = 0; j < a->nxw && !added; j++) if (a->xw_of[j] == i && !strstr(o->terms_b, a->xw[j]) && strncmp(a->concept[i], "op:", 3)) { m += (size_t)snprintf(o->terms_b + m, sizeof o->terms_b - m, "%s%s", m ? " " : "", a->xw[j]); added++; }
        if (!added && strncmp(a->concept[i], "op:", 3) && !strstr(o->terms_b, a->norm[i])) m += (size_t)snprintf(o->terms_b + m, sizeof o->terms_b - m, "%s%s", m ? " " : "", a->norm[i]);
        if (n + 60 > sizeof o->terms || m + 60 > sizeof o->terms_b) break;
    }
    if (!strcmp(a->dir, "send")) snprintf(o->terms_b + strlen(o->terms_b), sizeof o->terms_b - strlen(o->terms_b), "%ssend", o->terms_b[0] ? " " : "");
    else if (!strcmp(a->dir, "recv")) snprintf(o->terms_b + strlen(o->terms_b), sizeof o->terms_b - strlen(o->terms_b), "%sreceive", o->terms_b[0] ? " " : "");
}

/* ---- nos ancora ---- */
/* o simbolo que contem a linha (o mais interno); devolve o no do grafo ou -1 */
static int po_at(UioeOrch *o, const char *path, int line, const char *label) {
    UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", path); q.line = line; q.tier = 2; UioeResult r; int map[256];
    const int st = po_run(o, label, "code.outline", &q, &r, map, 0); int id = -1;
    if (po_usable(st) && r.nnode > 0) id = map[r.nnode - 1];
    ur_free(&r); return id;
}
/* resolve um caminho citado para o caminho relativo da raiz: exato, ou por sufixo no indice, ou por varredura */
static int po_resolve_path(UioeOrch *o, const char *text, char *rel, size_t cap) {
    struct stat st; char full[2600]; const char *t = text; while (t[0] == '.' && t[1] == '/') t += 2;
    snprintf(full, sizeof full, "%s/%s", o->cx->root, t);
    if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) { snprintf(rel, cap, "%s", t); return 1; }
    if (uioe_open_view(o->cx)) { char rr[4400]; int seg, ns; uint32_t idx; if (sy_find_doc(&o->cx->view, t, rr, sizeof rr, &seg, &idx, &ns)) { snprintf(rel, cap, "%s", rr); return 1; } }
    return 0;
}
/* degrau 4: sem indice de simbolos ou de chaves, o nome como literal (busca do indice se houver, senao varredura de arquivos) */
static void po_literal_fallback(UioeOrch *o, const char *label, const char *text, int tier) {
    UioeCall g = {0}; g.tier = tier; g.limit = 8; snprintf(g.arg, sizeof g.arg, "%.419s", text); UioeResult r2; int m2[256];
    const int s2 = po_run(o, label, "code.search", &g, &r2, m2, 0);
    if (!po_usable(s2)) { ur_free(&r2); const int s3 = po_run(o, label, "file.grep", &g, &r2, m2, 0); (void)s3; }
    for (int j = 0; j < r2.nnode; j++) if (r2.node[j].kind == UN_RANGE) po_anchor(o, m2[j]);
    ur_free(&r2);
}
/* as entidades nomeadas do pedido viram nos (faixa 1 no passo exato da cascata) */
static void po_entities(UioeOrch *o) {
    const UioeAsk *a = o->a; UioeResult r; int map[256]; int nsym = 0;
    for (int i = 0; i < a->nent; i++) {
        const UioeEntity *e = &a->ent[i]; UioeCall q = {0}; q.tier = 1; q.limit = 8; snprintf(q.arg, sizeof q.arg, "%.419s", e->text);
        if (e->kind == UK_SYMBOL && nsym < 3) {
            nsym++;
            const int st = po_run(o, "def", "code.def", &q, &r, map, 1);
            if (po_usable(st) && r.nnode > 0) { if (r.step <= 2) o->entity_hit = 1; for (int j = 0; j < r.nnode; j++) po_anchor(o, map[j]); }
            else if (st == UR_UNAVAILABLE || st == UR_ERROR || st == UR_TIMEOUT) {                      /* degrau 4: sem simbolos, o nome como literal por varredura de arquivos */
                rc_degrade(o->rc, "no symbol index: \"%.60s\" searched as a literal in the files (degradation 4; confidence at most derived)", e->text);
                UioeCall g = {0}; g.tier = 3; g.limit = 8; snprintf(g.arg, sizeof g.arg, "%.419s", e->text); UioeResult r2; int m2[256];
                const int s2 = po_run(o, "def", "code.search", &g, &r2, m2, 0);
                if (!po_usable(s2)) { ur_free(&r2); UioeCall gg = g; const int s3 = po_run(o, "def", "file.grep", &gg, &r2, m2, 0); (void)s3; }
                for (int j = 0; j < r2.nnode; j++) if (r2.node[j].kind == UN_RANGE) po_anchor(o, m2[j]);
                ur_free(&r2);
            } else if (st == UR_EMPTY && o->rc->idx_mode == 2) { o->entity_miss++; rc_limit(o->rc, "no definition of \"%.80s\" in the indexed set (%u files, complete)", e->text, o->cx->view_state == 1 ? o->cx->view.nlive : 0u); }
            ur_free(&r);
        } else if (e->kind == UK_ENV || e->kind == UK_KEY) {
            q.arg2[0] = 0;
            const int st = po_run(o, "key", "code.flow", &q, &r, map, 1);
            if (po_usable(st) && r.nnode > 0) { o->entity_hit = 1; for (int j = 0; j < r.nnode; j++) if (r.node[j].kind == UN_KEY) po_anchor(o, map[j]); }
            else if (st == UR_UNAVAILABLE || st == UR_ERROR || st == UR_TIMEOUT) { rc_degrade(o->rc, "no text-key index or the connector failed: \"%.60s\" searched as a literal in the files (degradation 3 and 4; the other end of a link is not looked for)", e->text); po_literal_fallback(o, "key", e->text, 3); }
            else if (st == UR_EMPTY && o->rc->idx_mode >= 1) { o->entity_miss++; rc_limit(o->rc, "no text key \"%.80s\" in the indexed set (nobody sends, handles or reads it by that literal name)", e->text); }
            ur_free(&r);
        } else if (e->kind == UK_PATH) {
            char rel[420];
            if (po_resolve_path(o, e->text, rel, sizeof rel)) {
                UioeNode n; memset(&n, 0, sizeof n); n.kind = UN_FILE; snprintf(n.path, sizeof n.path, "%s", rel); n.line0 = n.line1 = 1; snprintf(n.name, sizeof n.name, "file"); n.conf = 90; n.step = 1; n.tier = 1;
                snprintf(n.how, sizeof n.how, "the file the request names (%s)", e->how);
                const int id = ug_add(o->g, &n, uioe_find_conn(o->cx->s, "file.glob"), 1, "file.glob"); po_anchor(o, id); o->entity_hit = 1;
            } else { o->entity_miss++; rc_limit(o->rc, "the file \"%.100s\" does not exist under the scope and is not in the index", e->text); }
        } else if (e->kind == UK_LITERAL || e->kind == UK_ERRTEXT) {
            /* tratado pelo pipeline de erro/localizar, que sabe tentar o texto exato e o mascarado */
        }
    }
}
static int po_has_kind_ent(const UioeAsk *a, int kind) { for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == kind) return 1; return 0; }

/* ---- busca por palavras: indice, ou varredura de arquivos (degrau 5) ---- */
static int po_where(UioeOrch *o, const char *label, int k, int docs_ok) {
    UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", o->terms); q.limit = k < 12 ? k : 12; q.tier = 4; UioeResult r; int map[256];
    int st = po_run(o, label, "code.search", &q, &r, map, 0); int n = 0;
    if (po_usable(st)) {
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {                                   /* a segunda consulta (equivalente em ingles): a fusao junta as duas listas */
                ur_free(&r); if (!o->terms_b[0] || !strcmp(o->terms_b, o->terms)) break;
                snprintf(q.arg, sizeof q.arg, "%s", o->terms_b); st = po_run(o, label, "code.search", &q, &r, map, 0); if (!po_usable(st)) break;
            }
            for (int i = 0; i < r.nnode && o->naf < 16; i++) {
                if (r.node[i].kind != UN_FILE || map[i] < 0) continue;
                const char *p = r.node[i].path;
                if ((!docs_ok && po_is_doc(p)) || po_is_test(o, p)) continue;
                int dup = 0; for (int z = 0; z < o->naf; z++) dup |= o->afile[z] == map[i]; if (dup) continue;
                o->afile[o->naf++] = map[i]; n++;
            }
        }
    } else if (st == UR_UNAVAILABLE) {
        ur_free(&r); rc_degrade(o->rc, "no index: the search scanned the files for the 3 most specific terms (degradation 5, K = 3); a file that does not contain those words is not found");
        char w[3][48]; int nw = 0;
        for (int i = 0; i < o->a->nterms && nw < 3; i++) { const char *t = o->a->terms[i]; if (strlen(t) < 4 || po_is_place_word(o->a, t)) continue; int dup = 0; for (int j = 0; j < nw; j++) dup |= !strcmp(w[j], t); if (!dup) snprintf(w[nw++], sizeof w[0], "%s", t); }
        for (int j = 0; j < nw; j++) {
            UioeCall g = {0}; snprintf(g.arg, sizeof g.arg, "%s", w[j]); g.limit = 4; g.tier = 4; UioeResult r2; int m2[256];
            const int s2 = po_run(o, label, "file.grep", &g, &r2, m2, 0);
            if (po_usable(s2)) for (int i = 0; i < r2.nnode; i++) {
                const char *p = r2.node[i].path; if ((!docs_ok && po_is_doc(p)) || po_is_test(o, p)) continue;
                int fi = -1; for (int z = 0; z < o->naf; z++) if (!strcmp(o->g->n[o->afile[z]].path, p)) fi = o->afile[z];
                if (fi < 0 && o->naf < 3) { UioeNode f; memset(&f, 0, sizeof f); f.kind = UN_FILE; snprintf(f.path, sizeof f.path, "%s", p); f.line0 = f.line1 = 1; f.conf = 50; f.step = 1; f.tier = 4; snprintf(f.how, sizeof f.how, "contains \"%.60s\" (file scan)", w[j]);
                    fi = ug_add(o->g, &f, uioe_find_conn(o->cx->s, "file.grep"), 1, "file.grep"); if (fi >= 0) { o->afile[o->naf++] = fi; n++; } }
            }
            ur_free(&r2);
        }
        return n;
    }
    ur_free(&r); return n;
}
/* o simbolo que casa melhor com os termos do pedido numa lista de nos (por nome e assinatura); devolve o indice no resultado ou -1 */
static int po_best_sym(const UioeAsk *a, const UioeResult *r, const int *map, int need) {
    int best = -1, bs = 0;
    for (int i = 0; i < r->nnode; i++) {
        const UioeNode *n = &r->node[i]; if (n->kind != UN_SYMBOL || map[i] < 0) continue;
        char hay[300]; snprintf(hay, sizeof hay, "%s %s", n->name, n->sig); int sc = ug_terms_w(a, hay);
        if (sc < need) continue;
        sc = sc * 10 - (n->line1 - n->line0) / 200;
        if (!strncmp(n->cls, "func", 4) || !strncmp(n->cls, "meth", 4)) sc += 2;
        if (sc > bs) { bs = sc; best = i; }
    }
    return best;
}

/* ---- ligacoes por chave: pontuacao para a pergunta ---- */
/* candidato = arquivo que a busca por palavras devolveu (nao os arquivos que so apareceram como lado de um import) */
static int po_is_cand(const UioeOrch *o, const char *path) { for (int i = 0; i < o->g->nn; i++) if (o->g->n[i].kind == UN_FILE && !strcmp(o->g->n[i].path, path) && strstr(o->g->n[i].how, "search channels")) return 1; return 0; }
static int po_afile_has(const UioeOrch *o, const char *path) { for (int i = 0; i < o->naf; i++) if (!strcmp(o->g->n[o->afile[i]].path, path)) return 1; return 0; }
static int po_link_rel(const UioeOrch *o, const UioeEdge *e, char *why, size_t wcap) {
    const UioeGraph *g = o->g; const UioeNode *P = &g->n[e->from], *C = &g->n[e->to]; const UioeAsk *a = o->a;
    int rel = 20; char hay[1100];
    char base[300]; snprintf(base, sizeof base, "%s %s", po_base(P->path), po_base(C->path)); size_t w = 0; if (why) why[0] = 0;
#define WHY(...) do { if (why && w < wcap) w += (size_t)snprintf(why + w, wcap - w, __VA_ARGS__); } while (0)
    snprintf(hay, sizeof hay, "%s %s %s %s %s", e->key, P->text, P->sig, C->sig, C->text);
    { const int nt = ug_rel_terms(a, hay, base); rel += nt > 60 ? 60 : nt; WHY("terms %+d", nt > 60 ? 60 : nt); }
    if (!strcmp(a->dir, "send") && po_afile_has(o, P->path)) { rel += 20; WHY(", direction +20"); }
    if (!strcmp(a->dir, "recv") && po_afile_has(o, C->path)) { rel += 20; WHY(", direction +20"); }
    if (po_is_cand(o, P->path) && po_is_cand(o, C->path)) { rel += 12; WHY(", both ends among the candidates +12"); }
    { static const char *const gen[] = {"text", "type", "json", "error", "message", "ok", "data", "id", "name", "value", "result", "status", "close", "open", "log", "info", "start", "stop", "ready", NULL};
      int plain = 1; for (const char *p = e->key; *p; p++) if (!isalpha((unsigned char)*p)) plain = 0;
      int isgen = plain && strlen(e->key) <= 4; for (int i = 0; gen[i]; i++) if (!strcasecmp(e->key, gen[i])) isgen = 1;
      if (isgen) { rel -= 15; WHY(", generic key -15"); } }
    if (po_is_test(o, P->path) || po_is_test(o, C->path)) { rel -= 25; WHY(", test file -25"); }
    if (!strcmp(P->cls, "other")) { rel -= 8; WHY(", class other -8"); } else if (!strcmp(P->cls, "msg") || !strcmp(P->cls, "event") || !strcmp(P->cls, "route")) rel += 4;
#undef WHY
    return rel < 0 ? 0 : rel > 100 ? 100 : rel;
}
/* as ligacoes de um arquivo (ou de uma faixa dele): acrescenta ao grafo; e0..e1 sao as arestas novas ou ja existentes desta chamada */
static int po_file_links(UioeOrch *o, const char *label, const char *path, int line, int tier, int *e0, int *e1) {
    UioeCall q = {0}; q.depth = line > 0 ? 0 : 1; snprintf(q.arg, sizeof q.arg, "%s", path); snprintf(q.arg2, sizeof q.arg2, "file"); q.line = line; q.limit = 30; q.tier = tier; UioeResult r; int map[256];
    const int before = o->g->ne; const int st = po_run(o, label, "code.flow", &q, &r, map, 1);
    if (e0) *e0 = before;
    if (e1) *e1 = o->g->ne;
    if (po_usable(st) && r.nnode > 0 && r.nedge == 0) { /* ligacoes ja existentes (fundidas) nao criam aresta nova */ }
    ur_free(&r); return st;
}
static void po_score_links(UioeOrch *o) {
    for (int i = 0; i < o->g->ne; i++) {
        o->erel[i] = o->g->e[i].kind == UE_LINKS ? po_link_rel(o, &o->g->e[i], NULL, 0) : 0;
        if (o->erel[i]) { UioeNode *p = &o->g->n[o->g->e[i].from], *c = &o->g->n[o->g->e[i].to]; if (o->erel[i] > p->relevance) p->relevance = o->erel[i]; if (o->erel[i] > c->relevance) c->relevance = o->erel[i]; }
    }
}
/* ---- a cadeia ----
 * O caminho de resposta e CONSTRUIDO, nao ordenado: de cada uma das melhores ligacoes iniciais (a melhor de cada par de arquivos, ate 6) a cadeia
 * segue para frente ate 4 saltos, e a cadeia de maior soma (relevancia da ligacao inicial mais a pontuacao dos saltos) vira o caminho. Assim uma
 * ligacao boa que leva a um simbolo do pedido vence uma ligacao um pouco melhor que nao leva a lugar nenhum. Os saltos candidatos nao seguidos
 * da cadeia vencedora ficam como ramos (hipoteses abertas). */
typedef struct { int path[8], pedge[8], npath, score; int br[4], brsc[4], nbr; } PoChain;
static int po_chain_has_file(const UioeGraph *g, const PoChain *c, const char *path) { for (int i = 0; i < c->npath; i++) if (!strcmp(g->n[c->path[i]].path, path)) return 1; return 0; }
static void po_chain_add(PoChain *c, int node, int edge) { if (c->npath >= 8 || node < 0) return; c->path[c->npath] = node; c->pedge[c->npath] = -1; if (c->npath > 0) c->pedge[c->npath - 1] = edge; c->npath++; }
/* "in nome (a-b)" -> faixa do simbolo que contem a linha */
static int po_encl_range(const UioeNode *n, char *name, size_t cap, int *a, int *b) {
    if (n->kind == UN_SYMBOL) { snprintf(name, cap, "%s", n->name); *a = n->line0; *b = n->line1; return 1; }
    if (cc_where(n->sig, name, cap, a, b)) return 1;
    name[0] = 0; *a = n->line0 > 12 ? n->line0 - 12 : 1; *b = n->line0 + 12; return 0;
}
typedef struct { int kind, edge, to, score; char how[100]; } PoCand;
/* arg2 "match=a b c": o conector so devolve simbolos cujo nome ou assinatura contem uma dessas palavras (os termos do pedido e as palavras dos conceitos), para o grafo nao encher */
static void po_match_arg(const UioeOrch *o, char *out, size_t cap) {
    size_t n = (size_t)snprintf(out, cap, "match=");
    for (int i = 0; i < o->a->nterms && n + 30 < cap; i++) {               /* por termo: o proprio termo e as 3 primeiras palavras do conceito */
        if (po_is_place_word(o->a, o->a->terms[i]) || strlen(o->a->norm[i]) < 4) continue;
        if (!strstr(out, o->a->norm[i])) n += (size_t)snprintf(out + n, cap - n, "%s%s", n > 6 ? " " : "", o->a->norm[i]);
        int rank = 0; for (int j = 0; j < o->a->nxw && rank < 3 && n + 30 < cap; j++) if (o->a->xw_of[j] == i) { if (!strstr(out, o->a->xw[j]) && strlen(o->a->xw[j]) >= 4) n += (size_t)snprintf(out + n, cap - n, " %s", o->a->xw[j]); rank++; }
    }
}
/* segue a cadeia a partir da ligacao start, ate 4 saltos; devolve a soma */
static void po_build_chain(UioeOrch *o, int start, PoChain *ch) {
    UioeGraph *g = o->g; memset(ch, 0, sizeof *ch);
    const UioeEdge *be = &g->e[start]; po_chain_add(ch, be->from, -1); po_chain_add(ch, be->to, start); ch->score = o->erel[start];
    int hops = 1; PoCand cands[24];
    while (hops < 4 && ch->npath < 7 && !uioe_over(o->cx)) {
        const int cur = ch->path[ch->npath - 1]; const UioeNode *cn = &g->n[cur]; int nc = 0;
        char ename[100]; int ra, rb; po_encl_range(cn, ename, sizeof ename, &ra, &rb);
        /* (a) as chaves que o simbolo da posicao atual envia e o outro lado */
        { int e0, e1; po_file_links(o, "chain", cn->path, cn->line0, 2, &e0, &e1); po_score_links(o);
          for (int i = 0; i < g->ne && nc < 20; i++) {
              const UioeEdge *e = &g->e[i]; if (e->kind != UE_LINKS || po_bad_edge(o, i)) continue;
              const UioeNode *P = &g->n[e->from], *C = &g->n[e->to];
              if (strcmp(P->path, cn->path) || P->line0 < ra || P->line0 > rb || !strcmp(C->path, cn->path) || po_chain_has_file(g, ch, C->path) || po_is_test(o, C->path)) continue;
              PoCand *c = &cands[nc++]; c->kind = 1; c->edge = i; c->to = e->to; c->score = o->erel[i] + 10 + (!strcmp(o->a->dir, "send") ? 8 : 0); snprintf(c->how, sizeof c->how, "sends %.60s", e->key);
          } }
        /* (b) arquivos que importam o arquivo atual, com o simbolo que casa com os termos */
        { UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", cn->path); snprintf(q.arg2, sizeof q.arg2, "importers"); q.limit = 12; q.tier = 2; UioeResult r; int map[256];
          const int st = po_run(o, "chain", "code.deps", &q, &r, map, 0);
          if (po_usable(st)) { char seen[6][400]; int ns = 0;
              for (int i = 0; i < r.nnode && ns < 5; i++) {
                  if (r.node[i].kind != UN_RANGE || strcmp(r.node[i].cls, "imported-by") || po_is_test(o, r.node[i].path) || !strcmp(r.node[i].path, cn->path) || po_chain_has_file(g, ch, r.node[i].path)) continue;
                  int dup = 0; for (int z = 0; z < ns; z++) dup |= !strcmp(seen[z], r.node[i].path); if (dup) continue; snprintf(seen[ns++], 400, "%s", r.node[i].path);
                  if (!po_is_cand(o, r.node[i].path)) continue;                    /* o importador tambem e candidato da busca: so assim o salto por arquivo vale */
                  UioeCall q2 = {0}; snprintf(q2.arg, sizeof q2.arg, "%s", r.node[i].path); q2.limit = 80; q2.tier = 2; UioeResult r2; int m2[256]; po_match_arg(o, q2.arg2, sizeof q2.arg2);
                  const int s2 = po_run(o, "chain", "code.outline", &q2, &r2, m2, 0);
                  if (po_usable(s2)) { const int bi = po_best_sym(o->a, &r2, m2, 1);
                      if (bi >= 0 && nc < 22) { char hay[300]; snprintf(hay, sizeof hay, "%s %s", r2.node[bi].name, r2.node[bi].sig);
                          const int eid = ug_edge(g, cur, m2[bi], UE_VIA, 0, 55, 0, uioe_find_conn(o->cx->s, "code.deps"), "the importer of this file has a symbol that matches the request's terms; the connection through the imported object is not shown by a call", "");
                          PoCand *c = &cands[nc++]; c->kind = 2; c->edge = eid; c->to = m2[bi]; c->score = 15 + 4 * ug_terms_w(o->a, hay); snprintf(c->how, sizeof c->how, "importer %.70s", r.node[i].path); } }
                  ur_free(&r2);
              } }
          ur_free(&r); }
        /* (c) o que o simbolo atual chama, em outros arquivos */
        if (ename[0]) { UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", ename); q.limit = 14; q.tier = 2; UioeResult r; int map[256]; po_match_arg(o, q.arg2, sizeof q.arg2);
          const int st = po_run(o, "chain", "code.callees", &q, &r, map, 0);
          if (po_usable(st)) for (int i = 0; i < r.nnode && nc < 24; i++) {
              if (r.node[i].kind != UN_SYMBOL || map[i] < 0 || !strcmp(r.node[i].path, cn->path) || po_chain_has_file(g, ch, r.node[i].path) || po_is_test(o, r.node[i].path)) continue;
              char hay[300]; snprintf(hay, sizeof hay, "%s %s", r.node[i].name, r.node[i].sig); const int nt = ug_terms_w(o->a, hay); if (!nt) continue;
              int eid = -1; for (int z = 0; z < g->ne; z++) if (g->e[z].kind == UE_CALLS && g->e[z].to == map[i]) { eid = z; break; }
              PoCand *c = &cands[nc++]; c->kind = 3; c->edge = eid; c->to = map[i]; c->score = 8 + 4 * nt; snprintf(c->how, sizeof c->how, "calls %.70s", r.node[i].name);
          }
          ur_free(&r); }
        int pick = -1, ps = 21; for (int i = 0; i < nc; i++) if (cands[i].score > ps) { ps = cands[i].score; pick = i; }
        for (int i = 0; i < nc; i++) if (i != pick && cands[i].score >= 22 && cands[i].edge >= 0) {            /* os melhores nao seguidos */
            int dup = 0; for (int z = 0; z < ch->nbr; z++) dup |= ch->br[z] == cands[i].edge; if (dup) continue;
            if (ch->nbr < 4) { ch->br[ch->nbr] = cands[i].edge; ch->brsc[ch->nbr++] = cands[i].score; }
            else { int w = 0; for (int z = 1; z < 4; z++) if (ch->brsc[z] < ch->brsc[w]) w = z; if (cands[i].score > ch->brsc[w]) { ch->br[w] = cands[i].edge; ch->brsc[w] = cands[i].score; } } }
        if (pick < 0) break;
        const PoCand *c = &cands[pick];
        if (c->kind == 1) { const UioeEdge *e = &g->e[c->edge]; if (e->from != cur) po_chain_add(ch, e->from, -1); po_chain_add(ch, e->to, c->edge); }
        else po_chain_add(ch, c->to, c->edge);
        ch->score += c->score; hops++;
    }
}
static void po_chain(UioeOrch *o) {
    UioeGraph *g = o->g; UioeRecall *rc = o->rc; rc->npath = 0; rc->nbranch = 0;
    po_score_links(o);
    /* as 5 ligacoes mais bem pontuadas, com a conta, para o traco */
    { int used[24]; for (int z = 0; z < 24; z++) used[z] = -1; rc->nscore = 0; const int maxsc = uioe_env_int("KATANA_UIOE_SCORES", 5, 1, 24);
      for (int r = 0; r < maxsc; r++) { int bi = -1, bs = -1; for (int i = 0; i < g->ne; i++) { const UioeEdge *e = &g->e[i]; if (e->kind != UE_LINKS || !strcmp(g->n[e->from].path, g->n[e->to].path)) continue; int u = 0; for (int z = 0; z < r; z++) u |= used[z] == i; if (u) continue; const int sc = o->erel[i] * 1000 + e->conf; if (sc > bs) { bs = sc; bi = i; } }
          if (bi < 0) break;
          used[r] = bi; const UioeEdge *e = &g->e[bi]; char why[160]; po_link_rel(o, e, why, sizeof why);
          snprintf(rc->score[rc->nscore++], sizeof rc->score[0], "L%d %s:%d -> %s:%d \"%.40s\" conf=%d rel=%d (%s)", r + 1, g->n[e->from].path, g->n[e->from].line0, g->n[e->to].path, g->n[e->to].line0, e->key, e->conf, o->erel[bi], why); } }
    /* as ligacoes iniciais: a melhor de cada par (arquivo produtor, arquivo consumidor), as 6 maiores */
    int starts[6], nst = 0;
    for (int r = 0; r < 6; r++) {
        int bi = -1, bs = -1;
        for (int i = 0; i < g->ne; i++) {
            const UioeEdge *e = &g->e[i]; if (e->kind != UE_LINKS || po_bad_edge(o, i) || o->erel[i] < 20 || !strcmp(g->n[e->from].path, g->n[e->to].path)) continue;
            int u = 0; for (int z = 0; z < nst && !u; z++) { const UioeEdge *s = &g->e[starts[z]]; u = !strcmp(g->n[s->from].path, g->n[e->from].path) && !strcmp(g->n[s->to].path, g->n[e->to].path); }
            if (u) continue;
            const int sc = o->erel[i] * 1000 + e->conf; if (sc > bs) { bs = sc; bi = i; }
        }
        if (bi < 0) break;
        starts[nst++] = bi;
    }
    if (!nst) return;
    PoChain best, cur; int have = 0;
    for (int i = 0; i < nst && !uioe_over(o->cx); i++) { po_build_chain(o, starts[i], &cur); if (!have || cur.score > best.score) { best = cur; have = 1; } }
    if (!have) return;
    for (int i = 0; i < best.npath; i++) { rc->path[i] = best.path[i]; rc->pathedge[i] = best.pedge[i]; g->n[best.path[i]].onpath = 1; }
    rc->npath = best.npath; o->structural = 1;
    for (int i = 0; i < best.nbr && rc->nbranch < 4; i++) { int onp = 0; for (int z = 0; z < rc->npath; z++) onp |= rc->path[z] == g->e[best.br[i]].to; if (!onp) rc->branch[rc->nbranch++] = best.br[i]; }
}

/* ---- pipelines ---- */
static void pipe_locate(UioeOrch *o) {
    o->st = "P3"; po_entities(o);
    if (!o->nanch) {
        po_terms(o); const int n = po_where(o, "where", 5, 0);
        for (int i = 0; i < n && i < 3; i++) {
            const UioeNode *f = &o->g->n[o->afile[i]]; int line = 0;
            for (int j = 0; j < o->g->ne; j++) { const UioeEdge *e = &o->g->e[j]; if (e->kind == UE_CONTAINS && e->from == o->afile[i]) { line = o->g->n[e->to].line0; break; } }
            if (line) { const int s = po_at(o, f->path, line, "where"); po_anchor(o, s >= 0 ? s : -1); }
        }
        if (!n) rc_limit(o->rc, "nothing in the index matches the request's words (%s)", o->terms);
    }
    int done = 0; o->st = "P4";
    for (int i = 0; i < o->nanch && done < 2; i++) {
        const UioeNode *s = &o->g->n[o->anch[i]]; if (s->kind != UN_SYMBOL) continue;
        UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", s->name); q.limit = 3; q.tier = 2; UioeResult r; int map[256]; po_run(o, "callers", "code.callers", &q, &r, map, 0);
        ur_free(&r); done++;
    }
}
static void pipe_usage(UioeOrch *o) {
    o->st = "P3"; po_entities(o); o->st = "P4"; const UioeAsk *a = o->a; int ns = 0; char seen[3][100];
    for (int i = 0; i < o->nanch; i++) {
        const UioeNode *s = &o->g->n[o->anch[i]]; if (s->kind != UN_SYMBOL) continue;
        int dup = 0; for (int z = 0; z < ns; z++) dup |= !strcmp(seen[z], s->name); if (dup || ns >= 3) continue; snprintf(seen[ns++], 100, "%s", s->name);
        UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", s->name); q.limit = 8; q.tier = 2; UioeResult r; int map[256];
        if (a->rel & UREL_CALLEES) po_run(o, "callees", "code.callees", &q, &r, map, 1);
        else if (a->rel & UREL_USES) po_run(o, "uses", "code.refs", &q, &r, map, 1);
        else po_run(o, "callers", "code.callers", &q, &r, map, 1);
        ur_free(&r);
    }
    if (po_has_kind_ent(a, UK_PATH)) for (int i = 0; i < o->nanch; i++) { const UioeNode *s = &o->g->n[o->anch[i]]; if (s->kind != UN_FILE) continue;
        UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", s->path); q.limit = 20; q.tier = 2; UioeResult r; int map[256]; po_run(o, "imports", "code.deps", &q, &r, map, 0); ur_free(&r); }
    if (!ns && !o->entity_hit) {                                   /* sem definicao: o nome como literal e como chave de texto */
        for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == UK_SYMBOL) {
            UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", a->ent[i].text); snprintf(q.arg2, sizeof q.arg2, "literal"); q.limit = 6; q.tier = 3; UioeResult r; int map[256];
            po_run(o, "literal", "code.search", &q, &r, map, 0); ur_free(&r);
            UioeCall k = {0}; snprintf(k.arg, sizeof k.arg, "%s", a->ent[i].text); k.limit = 6; k.tier = 1; UioeResult r2;
            const int st = po_run(o, "literal", "code.flow", &k, &r2, map, 0);
            if (po_usable(st) && r2.nnode) { o->entity_hit = 1; o->entity_miss = 0; rc_hyp(o->rc, "\"%s\" is a text key (message, event, route or environment variable), not a symbol: see where it is sent and handled", a->ent[i].text); }
            ur_free(&r2);
        }
    }
}
static void pipe_impact(UioeOrch *o) {
    o->st = "P3"; po_entities(o); o->st = "P4"; int ns = 0;
    for (int i = 0; i < o->nanch && ns < 2; i++) {
        const UioeNode *s = &o->g->n[o->anch[i]]; if (s->kind != UN_SYMBOL) continue; ns++;
        /* os passos baratos primeiro (chaves do arquivo e importadores); o fecho de chamadores, que e o caro, usa o tempo que sobra */
        int map[256];
        UioeCall l = {0}; snprintf(l.arg, sizeof l.arg, "%s", s->path); snprintf(l.arg2, sizeof l.arg2, "file"); l.line = s->line0; l.limit = 16; l.tier = 2; UioeResult r2;
        po_run(o, "links", "code.flow", &l, &r2, map, 0); ur_free(&r2);
        UioeCall d = {0}; snprintf(d.arg, sizeof d.arg, "%s", s->path); d.limit = 20; d.tier = 2; UioeResult r3;
        po_run(o, "imports", "code.deps", &d, &r3, map, 0); ur_free(&r3);
        /* profundidade 3 (3.7.2) quando sobra mais de 600 ms no prazo; senao 2: o fecho em profundidade 3 levou 0,5 s na arvore do katana, mais que o prazo inteiro */
        UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", s->name); q.limit = 25; q.depth = (o->cx->det || uioe_left_ms(o->cx) > 600) ? 3 : 2; q.tier = 2; UioeResult r;
        const int st = po_run(o, "impact", "code.impact", &q, &r, map, 1);
        if (q.depth < 3) rc_limit(o->rc, "the closure of callers was followed to depth 2, not 3: depth 3 does not fit in the %d ms request deadline (KATANA_UIOE_MS)", o->cx->deadline_ms);
        int ntest = 0; for (int j = 0; j < r.nnode; j++) if (map[j] >= 0 && fd_is_test(r.node[j].path)) { snprintf(o->g->n[map[j]].cls, sizeof o->g->n[0].cls, "test"); ntest++; }
        { const int ti = po_step_find(o->rt, "tests"); if (ti >= 0 && !o->rt->step[ti].ran) { UioeStep *ts = &o->rt->step[ti]; ts->ran = 1; ts->rcalls = 1; ts->rnodes = ntest; ts->rstatus = UR_OK; snprintf(ts->rconn, sizeof ts->rconn, "code.impact"); snprintf(ts->rnote, sizeof ts->rnote, "the callers found by the impact step that live in test files, listed apart"); } }
        if (po_usable(st)) { o->structural = 1; if (!r.nnode) rc_limit(o->rc, "no caller of %s in the indexed set: nobody in the set depends on it by name", s->name); }
        ur_free(&r);
    }
    rc_limit(o->rc, "calls through pointers, macro-generated names, dynamic dispatch, reflection and other repositories are not seen; callers are matched by name");
}
static void pipe_flow(UioeOrch *o) {
    o->st = "P3"; po_entities(o); po_terms(o);
    const int named = o->nanch > 0;
    if (!named || !po_has_kind_ent(o->a, UK_SYMBOL)) {
        const int n = po_where(o, "where", 12, 0);
        if (!n && !named) rc_limit(o->rc, "no file matches the request's words (%s): nothing to start from", o->terms);
    }
    for (int i = 0; i < o->nanch; i++) { const UioeNode *s = &o->g->n[o->anch[i]]; if (s->kind == UN_FILE || s->kind == UN_SYMBOL) { int dup = 0; for (int z = 0; z < o->naf; z++) dup |= !strcmp(o->g->n[o->afile[z]].path, s->path); if (!dup && o->naf < 16) o->afile[o->naf++] = o->anch[i]; } }
    /* ligacoes por chave dos arquivos ancora: todos os candidatos da busca (ate 14), porque a palavra so acerta o primeiro lugar poucas vezes e cada consulta custa menos de 1 ms */
    o->st = "P4"; int nlinkfiles = 0, skipped_keys = 0;
    for (int i = 0; i < o->naf && nlinkfiles < 16 && !uioe_over(o->cx); i++) {
        const UioeNode *f = &o->g->n[o->afile[i]]; int e0, e1;
        const int st = po_file_links(o, "links", f->path, 0, 2, &e0, &e1);
        if (st == UR_UNAVAILABLE) { skipped_keys = 1; break; }
        nlinkfiles++;
    }
    if (skipped_keys) { rc_degrade(o->rc, "no text-key index: the connections through strings (events, routes, message types) are not available (degradation 3); only calls can be followed"); }
    /* simbolos ancora e suas chamadas (ate 3, profundidade 1 chamados e chamadores) */
    int nsym = 0;
    for (int i = 0; i < o->naf && nsym < 3 && !uioe_over(o->cx) && o->cx->have_sym; i++) {
        const UioeNode *f = &o->g->n[o->afile[i]]; int line = 0;
        for (int j = 0; j < o->g->ne; j++) { const UioeEdge *e = &o->g->e[j]; if (e->kind == UE_CONTAINS && e->from == o->afile[i]) { line = o->g->n[e->to].line0; break; } }
        if (!line) continue;
        const int s = po_at(o, f->path, line, "callees"); if (s < 0) continue; nsym++;
        po_anchor(o, s);
        UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", o->g->n[s].name); q.limit = 8; q.tier = 2; UioeResult r; int map[256];
        po_run(o, "callees", "code.callees", &q, &r, map, 0); ur_free(&r);
    }
    po_chain(o);
    /* documento que cita dois ou mais arquivos da cadeia: corrobora (sobe a relevancia, nao a confianca dos fatos) */
    if (o->rc->npath >= 2 && !uioe_over(o->cx)) {
        char names[600]; size_t n = 0; names[0] = 0;
        for (int i = 0; i < o->rc->npath; i++) { const char *b = po_base(o->g->n[o->rc->path[i]].path); if (strstr(names, b)) continue; n += (size_t)snprintf(names + n, sizeof names - n, "%s%s", n ? " " : "", b); }
        UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", names); q.limit = 6; q.tier = 3; UioeResult r; int map[256];
        const int st = po_run(o, "doc", "docs.search", &q, &r, map, 0);
        if (po_usable(st)) for (int i = 0; i < r.nnode; i++) {
            int hit = 0; for (int z = 0; z < o->rc->npath; z++) { const char *b = po_base(o->g->n[o->rc->path[z]].path); int dup = 0; for (int y = 0; y < z; y++) dup |= !strcmp(po_base(o->g->n[o->rc->path[y]].path), b); if (!dup && strstr(r.node[i].text, b)) hit++; }
            if (hit >= 2 && map[i] >= 0) { o->g->n[map[i]].relevance += 20; snprintf(o->g->n[map[i]].how + strlen(o->g->n[map[i]].how), sizeof o->g->n[0].how - strlen(o->g->n[map[i]].how), "; names %d files of the chain", hit); o->g->n[map[i]].tier = 3; }
            else if (map[i] >= 0) o->g->n[map[i]].dropped = 1;
        }
        ur_free(&r);
    }
    if (!o->rc->npath) rc_limit(o->rc, "no connection found between the candidates in the indexed set");
}
static int po_is_config_class(const char *c) { return !strcmp(c, "env") || !strcmp(c, "config") || !strcmp(c, "cmd"); }
static void pipe_config(UioeOrch *o) {
    o->st = "P3"; po_entities(o); po_terms(o);
    int have_key = 0; for (int i = 0; i < o->nanch; i++) if (o->g->n[o->anch[i]].kind == UN_KEY) have_key = 1;
    o->st = "P4";
    if (!have_key && !po_has_kind_ent(o->a, UK_ENV) && !po_has_kind_ent(o->a, UK_KEY)) {
        char save[640]; snprintf(save, sizeof save, "%s", o->terms); snprintf(o->terms + strlen(o->terms), sizeof o->terms - strlen(o->terms), " config setting option env default");
        po_where(o, "where", 8, 1); snprintf(o->terms, sizeof o->terms, "%s", save);
        for (int i = 0; i < o->naf && i < 6; i++) { const UioeNode *f = &o->g->n[o->afile[i]]; if (po_is_doc(f->path)) continue; int e0, e1; po_file_links(o, "key", f->path, 0, 3, &e0, &e1); }
    }
    for (int i = 0; i < o->g->nn; i++) { UioeNode *n = &o->g->n[i]; if (n->kind == UN_KEY && !po_is_config_class(n->cls) && have_key) n->relevance -= 30; if (uioe_secret_path(n->path)) { n->secret = 1; n->text[0] = 0; } }
    /* o que documenta a opcao */
    { UioeCall q = {0}; q.limit = 5; q.tier = 3; const UioeAsk *a = o->a;
      for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == UK_ENV || a->ent[i].kind == UK_KEY || a->ent[i].kind == UK_SYMBOL) { snprintf(q.arg, sizeof q.arg, "%.400s", a->ent[i].text); break; }
      if (!q.arg[0]) snprintf(q.arg, sizeof q.arg, "%s", o->terms);
      UioeResult r; int map[256]; const int st = po_run(o, "doc", "docs.search", &q, &r, map, 0);
      if (po_usable(st) && !r.nnode) rc_limit(o->rc, "no document line mentions it");
      for (int i = 0; i < r.nnode; i++) if (map[i] >= 0 && q.arg[0] && !strcasestr(r.node[i].text, q.arg) && !strchr(q.arg, ' ')) o->g->n[map[i]].dropped = 1;     /* so as linhas que citam a opcao pelo nome */
      ur_free(&r); }
    if (have_key) o->structural = 1;
}
/* texto de erro sem numeros, caminhos e aspas: o trecho fixo que o programa imprime */
static void po_mask_err(const char *in, char *out, size_t cap) {
    size_t o = 0; int last_space = 1;
    for (const char *p = in; *p && o + 2 < cap; p++) {
        if (*p == '"' || *p == '\'' || *p == '`') { while (*(p + 1) && *(p + 1) != *p) p++; if (*(p + 1)) p++; if (!last_space) out[o++] = ' '; last_space = 1; continue; }
        if (isdigit((unsigned char)*p) || *p == '/' || *p == '\\') { while (p[1] && !isspace((unsigned char)p[1])) p++; continue; }
        out[o++] = *p; last_space = isspace((unsigned char)*p);
    }
    out[o] = 0; while (o && isspace((unsigned char)out[o - 1])) out[--o] = 0;
}
/* as linhas de guarda acima de uma linha dentro do mesmo corpo: condicoes que decidem se a mensagem sai */
static int po_guards(UioeOrch *o, int node) {
    UioeGraph *g = o->g; const UioeNode *n = &g->n[node]; int count = 0;
    if (uv_load(o->cx, n->path) != 1) return 0;
    char t[400];
    if (uv_line(o->cx, n->line0, t, sizeof t) && (strstr(t, "if (") || strstr(t, "if(") || !strncmp(t, "case ", 5) || strstr(t, "else"))) return 0;      /* a condicao esta na propria linha da mensagem: ja aparece nela */
    for (int l = n->line0 - 1; l >= n->line0 - 8 && l >= 1 && count < 1; l--) {
        if (!uv_line(o->cx, l, t, sizeof t)) continue;
        const char *kw[] = {"if (", "if(", "else", "case ", "switch", "unless ", " when ", "while (", "assert", "guard ", NULL}; int hit = 0;
        for (int k = 0; kw[k]; k++) if (strstr(t, kw[k])) hit = 1;
        if (t[0] == 'i' && t[1] == 'f' && (t[2] == ' ' || t[2] == '(')) hit = 1;
        if (!hit) continue;
        UioeNode gnode; memset(&gnode, 0, sizeof gnode); gnode.kind = UN_RANGE; snprintf(gnode.path, sizeof gnode.path, "%s", n->path); gnode.line0 = gnode.line1 = l; snprintf(gnode.name, sizeof gnode.name, "guard");
        snprintf(gnode.cls, sizeof gnode.cls, "guard"); gnode.conf = 70; gnode.step = 1; gnode.tier = 2; ur_set_text(&gnode, t); snprintf(gnode.vkey, sizeof gnode.vkey, "%.40s", gnode.text);
        snprintf(gnode.how, sizeof gnode.how, "a condition above the message in the same body (line %d)", l);
        const int id = ug_add(g, &gnode, uioe_find_conn(o->cx->s, "file.windows"), count + 1, "file.windows");
        if (id >= 0) { ug_edge(g, id, node, UE_CONTAINS, l, 70, 0, 0, "guards the message", ""); count++; }
        if (l != n->line0 && strstr(t, "if")) break;
    }
    return count;
}
static void pipe_error(UioeOrch *o) {
    const UioeAsk *a = o->a; const UioeEntity *lit = NULL;
    for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == UK_ERRTEXT || a->ent[i].kind == UK_LITERAL) { lit = &a->ent[i]; break; }
    o->st = "P3"; po_entities(o); po_terms(o);
    int hits[4], nh = 0;
    if (lit) {
        char t[420]; snprintf(t, sizeof t, "%s", lit->text); for (char *p = t; *p; p++) if (*p == '"') *p = ' ';
        UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", t); snprintf(q.arg2, sizeof q.arg2, "literal"); q.limit = 6; q.tier = 1; UioeResult r; int map[256];
        int st = po_run(o, "literal", "code.search", &q, &r, map, 1);
        int nlit = 0; for (int i = 0; i < r.nnode; i++) if (r.node[i].kind == UN_RANGE && !strcmp(r.node[i].cls, "literal")) nlit++;
        if (po_usable(st) && nlit) o->entity_hit = 1;
        if (st == UR_UNAVAILABLE) {
            ur_free(&r); rc_degrade(o->rc, "no index: the error text is searched by scanning the files (degradation 5)");
            UioeCall g = q; g.arg2[0] = 0; st = po_run(o, "literal", "file.grep", &g, &r, map, 0); if (po_usable(st) && r.nnode) o->entity_hit = 1;
        } else if (!nlit) {                       /* o texto exato nao esta em arquivo nenhum: tenta o trecho fixo (sem numeros, caminhos e partes entre aspas) */
            char m[420]; po_mask_err(lit->text, m, sizeof m);
            for (int i = 0; i < r.nnode; i++) if (map[i] >= 0) o->g->n[map[i]].dropped = 1;           /* a ordem por palavras do primeiro intento nao e prova de nada */
            if (strlen(m) >= 8 && strcmp(m, t)) { ur_free(&r); UioeCall q2 = q; snprintf(q2.arg, sizeof q2.arg, "%s", m); q2.tier = 3;
                st = po_run(o, "literal", "code.search", &q2, &r, map, 0); rc_hyp(o->rc, "the exact text was not found; searched the fixed part of the message (numbers, paths and quoted parts removed): \"%.80s\"", m);
                nlit = 0; for (int i = 0; i < r.nnode; i++) if (r.node[i].kind == UN_RANGE && !strcmp(r.node[i].cls, "literal")) nlit++;
                if (po_usable(st) && nlit) o->entity_hit = 1; }
        }
        char needle[420]; po_mask_err(lit->text, needle, sizeof needle); if (strlen(needle) < 8) snprintf(needle, sizeof needle, "%s", t);
        for (int i = 0; i < r.nnode; i++) {
            if (r.node[i].kind == UN_FILE && map[i] >= 0 && strcmp(r.node[i].cls, "literal")) o->g->n[map[i]].dropped = 1;
            if (r.node[i].kind != UN_RANGE || map[i] < 0) continue;
            if (strcmp(r.node[i].cls, "literal") || !strcasestr(r.node[i].text, needle)) { o->g->n[map[i]].dropped = 1; continue; }           /* so as linhas onde o texto esta de fato */
            const char *p = r.node[i].path; if (po_is_doc(p) && !(o->qflags & 1)) o->g->n[map[i]].relevance -= 10;
            if (nh < 4) hits[nh++] = map[i]; }
        if (!nlit && o->rc->idx_mode >= 1) { o->entity_miss = 1; rc_limit(o->rc, "the error text (or its fixed part) does not occur in the indexed set: it may come from another program, a library, a format built at run time or a run-time message"); }
        ur_free(&r);
    } else rc_limit(o->rc, "the request has no error text to search for; only its words were used");
    /* o simbolo que contem cada ocorrencia (os 2 primeiros fora de testes), as guardas e os chamadores */
    int done = 0; o->st = "P4";
    for (int i = 0; i < nh && done < 2; i++) {
        const UioeNode *h = &o->g->n[hits[i]]; if (fd_is_test(h->path) && !(o->qflags & 2)) continue; done++;
        const int s = po_at(o, h->path, h->line0, "at");
        po_guards(o, hits[i]);
        if (s >= 0) { o->g->n[s].tier = 2; ug_edge(o->g, s, hits[i], UE_CONTAINS, h->line0, 80, 0, 0, "the message is inside this symbol", ""); o->structural = 1;
            UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", o->g->n[s].name); q.limit = 5; q.tier = 2; UioeResult r; int map[256];
            const int st = po_run(o, "at", "code.callers", &q, &r, map, 0);
            if (po_usable(st)) for (int j = 0; j < r.nnode && j < 2; j++) if (r.node[j].kind == UN_RANGE && map[j] >= 0) {
                const char *nm = r.node[j].sig; char en[100]; int ra, rb; if (cc_where(nm, en, sizeof en, &ra, &rb) && en[0]) {          /* callers de profundidade 2 */
                    UioeCall q2 = {0}; snprintf(q2.arg, sizeof q2.arg, "%s", en); q2.limit = 3; q2.tier = 2; UioeResult r2; int m2[256]; po_run(o, "at", "code.callers", &q2, &r2, m2, 0); ur_free(&r2); } }
            ur_free(&r); }
    }
    rc_limit(o->rc, "the last failing call and the files changed in this session need an agent session (session.facts); memory and earlier sessions are not built in this phase (U8)");
}
static void pipe_howto(UioeOrch *o) {
    o->st = "P3"; po_terms(o);
    UioeResult r; int map[256]; UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", o->a->text); q.limit = 6; q.tier = 3;
    po_run(o, "docs", "docs.select", &q, &r, map, 0); ur_free(&r);
    char dq[640]; size_t n = 0; dq[0] = 0;
    for (int i = 0; i < o->a->nterms; i++) { if (po_is_place_word(o->a, o->a->terms[i])) continue; n += (size_t)snprintf(dq + n, sizeof dq - n, "%s%s", n ? " " : "", o->a->terms[i]);
        int added = 0; for (int j = 0; j < o->a->nxw && added < 2; j++) if (o->a->xw_of[j] == i && !strstr(dq, o->a->xw[j])) { n += (size_t)snprintf(dq + n, sizeof dq - n, " %s", o->a->xw[j]); added++; } }
    if (!dq[0]) snprintf(dq, sizeof dq, "%s", o->terms);
    UioeCall q2 = {0}; snprintf(q2.arg, sizeof q2.arg, "%s", dq); q2.limit = 6; q2.tier = 3; UioeResult r2;
    po_run(o, "docsearch", "docs.search", &q2, &r2, map, 0);
    if (!r2.nnode) rc_limit(o->rc, "the project's documents (README, docs/, KATANA.md) have no line that matches: \"%.100s\"", dq);
    ur_free(&r2);
}
static void pipe_overview(UioeOrch *o) {
    o->st = "P3"; UioeResult r; int map[256]; UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "*"); q.limit = 20; q.tier = 3;
    po_run(o, "tree", "file.glob", &q, &r, map, 0); ur_free(&r);
    int nd = 0;
#ifndef _WIN32
    DIR *d = opendir(o->cx->root);
    if (d) { struct dirent *e; while ((e = readdir(d)) != NULL && nd < 24) { if (e->d_name[0] == '.' || sf_skip_dir(e->d_name)) continue; char full[2600]; snprintf(full, sizeof full, "%s/%s", o->cx->root, e->d_name); struct stat st; if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        UioeNode f; memset(&f, 0, sizeof f); f.kind = UN_FOLDER; snprintf(f.path, sizeof f.path, "%s", e->d_name); f.line0 = f.line1 = 1; snprintf(f.name, sizeof f.name, "%s/", e->d_name); f.conf = 90; f.step = 1; f.tier = 3; snprintf(f.how, sizeof f.how, "top-level folder");
        ug_add(o->g, &f, uioe_find_conn(o->cx->s, "file.glob"), nd + 1, "file.glob"); nd++; }
      closedir(d); }
#endif
    /* os titulos dos documentos de entrada (o seletor docsel precisa do tokenizador): a primeira linha de titulo e a primeira linha de texto depois dela */
    { static const char *const nm[] = {"README.md", "KATANA.md", "AGENTS.md", "CLAUDE.md", NULL}; const int pi = po_step_find(o->rt, "docs"); int nn = 0;
      for (int k = 0; nm[k]; k++) { if (uv_load(o->cx, nm[k]) != 1) continue; char t[300]; int title = 0;
          for (int l = 1; l <= o->cx->vnl && l < 80; l++) { if (!uv_line(o->cx, l, t, sizeof t) || !t[0]) continue;
              if (!title && t[0] == '#') { title = l; }
              else if (title && t[0] != '#' && t[0] != '|' && strncmp(t, "```", 3)) { title = -l; }
              if (title < 0 || (!title && l > 12)) break; }
          const int lt = title < 0 ? -title : title; if (!lt) continue;
          UioeNode d; memset(&d, 0, sizeof d); d.kind = UN_DOCLINE; snprintf(d.path, sizeof d.path, "%s", nm[k]); d.line0 = d.line1 = lt; snprintf(d.name, sizeof d.name, "opening of %s", nm[k]);
          if (uv_line(o->cx, lt, t, sizeof t)) { ur_set_text(&d, t); snprintf(d.vkey, sizeof d.vkey, "%.40s", d.text); uioe_mask_line(d.text); }
          d.conf = 70; d.step = 1; d.tier = 3; snprintf(d.how, sizeof d.how, "first heading or text line of %s (the project's own description)", nm[k]);
          ug_add(o->g, &d, uioe_find_conn(o->cx->s, "docs.search"), 1 + nn, "docs.search"); nn++; }
      if (pi >= 0) { UioeStep *s = &o->rt->step[pi]; s->ran = 1; s->rcalls = 1; s->rnodes = nn; s->rstatus = nn ? UR_OK : UR_EMPTY; snprintf(s->rconn, sizeof s->rconn, "direct read"); snprintf(s->rnote, sizeof s->rnote, "the opening lines of README.md, KATANA.md, AGENTS.md and CLAUDE.md were read (docs.select needs the tokenizer)"); } }
    rc_limit(o->rc, "the most imported files per folder were not computed in this phase; use the folder listing and the document titles");
}
/* U8: a memoria do dono como evidencia datada e de baixa confianca (conn_memory.c). Roda no tipo history e, nos outros, so quando o roteador pos o passo "memory"
   (pista de historico ou de preferencia). Os termos sao os normalizados do pedido, sem o nome da pasta ou do projeto. */
static void pipe_memory(UioeOrch *o) {
    if (po_step_find(o->rt, "memory") < 0) return;
    o->st = "P3"; po_terms(o);
    UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%.419s", o->terms); q.limit = 4; q.tier = 4; UioeResult r; int map[256];
    const int st = po_run(o, "memory", "memory.recall", &q, &r, map, 0);
    { const int mi = po_step_find(o->rt, "memory"); if (mi >= 0) { UioeStep *ms = &o->rt->step[mi]; char old[160]; snprintf(old, sizeof old, "%.100s", ms->rnote); snprintf(ms->rnote, sizeof ms->rnote, "%s%s%.90s", old, old[0] ? "; " : "", o->k->why_memory); } }      /* o motivo da rota aparece em `uioe ask --explain` */
    if (st == UR_EMPTY) rc_limit(o->rc, "memory: nothing saved matches the request");
    ur_free(&r);
}
/* U9: a web (docs/uioe.md 3.8.6). Ordem: (1) os documentos do projeto, se ha indice; (2) cada endereco que o USUARIO digitou (ate 2), lido antes do modelo mesmo com
   KATANA_UIOE_WEB=0; (3) so com KATANA_UIOE_WEB=1 e sem pagina lida: a busca com os termos interpretados e, nos resultados, a pagina do melhor. Cada chamada grava em
   `sent` o que saiu; o passo fora da politica fica "not routed" e o Next propoe `Search the web for "..."` (orch_next). */
static void pipe_web(UioeOrch *o) {
    o->st = "P3"; po_terms(o);
    UioeResult r; int map[256]; UioeCall q = {0};
    if (o->cx->has_store) { snprintf(q.arg, sizeof q.arg, "%.419s", o->terms); q.limit = 4; q.tier = 3; po_run(o, "docs", "docs.search", &q, &r, map, 0); ur_free(&r); }
    int got = 0, ntyped = 0;
    for (int i = 0; i < o->a->nent && ntyped < 2; i++) {
        if (o->a->ent[i].kind != UK_URL) continue;
        char u[420]; snprintf(u, sizeof u, "%s%.410s", !strncmp(o->a->ent[i].text, "www.", 4) ? "https://" : "", o->a->ent[i].text);
        for (size_t l = strlen(u); l > 0 && strchr(".,;:)]}>\"'!?", u[l - 1]); ) u[--l] = 0;
        memset(&q, 0, sizeof q); snprintf(q.arg, sizeof q.arg, "%s", u); snprintf(q.arg2, sizeof q.arg2, "%.200s", o->terms); q.depth = 1; q.tier = 1; q.limit = 1;
        o->st = "P4"; const int st = po_run(o, "page", "web.fetch", &q, &r, map, 0); ntyped++;
        if (po_usable(st) && r.nnode) got++;
        else if (st == UR_EMPTY) rc_limit(o->rc, "web.fetch: %.200s", r.notfound[0] ? r.notfound : "the page returned nothing");
        ur_free(&r);
    }
    const int wi = po_step_find(o->rt, "web");
    if (got || wi < 0 || o->rt->step[wi].status) {
        if (!got && !ntyped) rc_limit(o->rc, "the web was not searched before the model: KATANA_UIOE_WEB is not 1 (policy 3.8.6), so no search terms left the machine");
        return;
    }
    memset(&q, 0, sizeof q); snprintf(q.arg, sizeof q.arg, "%.419s", o->terms); q.limit = 4; q.tier = 3; o->st = "P3";
    const int st = po_run(o, "web", "web.search", &q, &r, map, 0);
    char best[420] = ""; int bc = -1;
    if (po_usable(st)) for (int i = 0; i < r.nnode; i++) {
        const char *w = strstr(r.node[i].how, "web result "); if (!w || strncmp(w + 11, "http", 4)) continue;
        if (r.node[i].conf > bc) { bc = r.node[i].conf; snprintf(best, sizeof best, "%.*s", (int)strcspn(w + 11, " "), w + 11); } }
    ur_free(&r);
    if (best[0] && !uioe_over(o->cx)) {
        memset(&q, 0, sizeof q); snprintf(q.arg, sizeof q.arg, "%s", best); snprintf(q.arg2, sizeof q.arg2, "%.200s", o->terms); q.depth = 0; q.tier = 3; q.limit = 1; o->st = "P4";
        const int s2 = po_run(o, "best", "web.fetch", &q, &r, map, 0);
        if (s2 == UR_EMPTY) rc_limit(o->rc, "web.fetch: %.200s", r.notfound[0] ? r.notfound : "the page returned nothing");
        ur_free(&r);
    } else if (!best[0] && po_usable(st)) rc_limit(o->rc, "web.search returned no result with an address to read");
}
static void pipe_place(UioeOrch *o) {
    o->st = "P3"; UioeCall q = {0}; snprintf(q.arg, sizeof q.arg, "%s", o->a->nplaces ? o->a->places[0] : o->a->root); q.tier = 1; UioeResult r; int map[256];
    po_run(o, "place", "place.resolve", &q, &r, map, 1); ur_free(&r);
}
static void po_pipeline(UioeOrch *o) {
    if (o->k->retrieve == 2) { pipe_place(o); return; }
    switch (o->k->kind) {
    case UI_LOCATE: pipe_locate(o); break;
    case UI_USAGE: pipe_usage(o); break;
    case UI_IMPACT: pipe_impact(o); break;
    case UI_FLOW: pipe_flow(o); break;
    case UI_CONFIG: pipe_config(o); break;
    case UI_ERROR: pipe_error(o); break;
    case UI_HOWTO: pipe_howto(o); break;
    case UI_OVERVIEW: pipe_overview(o); break;
    case UI_HISTORY: pipe_memory(o); break;
    case UI_WEB: pipe_web(o); break;
    default: rc_limit(o->rc, "this kind of request (%s) needs sources that are not built (external connectors: U10 to U12)", g_uioe_kindname[o->k->kind]); break;
    }
    if (o->k->kind != UI_HISTORY && o->k->kind != UI_NONE) pipe_memory(o);      /* so roda se o roteador pos o passo (pista de memoria) */
}
