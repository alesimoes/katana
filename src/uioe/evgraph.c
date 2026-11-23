/* ---------- uioe/evgraph.c: o grafo de evidencias, a fusao e a ordem (docs/uioe.md 3.7.3 e 3.7.4, fase U3) ----------
 * Um grafo por pedido (UioeGraph, na pilha de UioeRecall): nos (arquivo, faixa, simbolo, chave, linha de documento) e arestas com
 * proveniencia, confianca 0-100 e verificacao. Sem estado de arquivo: tudo pendura do grafo que o chamador passa.
 *   - identidade de um no: (tipo, caminho, linha inicial, nome). Dois conectores que devolvem o mesmo item viram UM no que lembra as duas
 *     proveniencias; a concordancia entre conectores independentes entra na ordem (faixa 3) e no sinal "fontes concordam" da porta.
 *   - faixas (3.7.4): 1 nomeado no pedido e achado no passo exato da cascata; 2 alcancado de um no de faixa 1 ou 2 por aresta estrutural
 *     (chama, liga, importa); 3 passo derivado, ou confirmado por dois conectores; 4 provavel (so palavras). Faixa menor nunca perde para maior.
 *   - dentro da faixa: RRF sobre as listas de ordem dos conectores (k = 60, o mesmo da busca do coder), mais ajustes de relevancia limitados.
 *   - estouro (400 nos, 800 arestas): o item novo e descartado e contado em `lost_*`, que vai para "Not found / limits".
 * Decisoes do implementador: a relevancia (0-100) so ordena e nunca vira confianca; o peso 1 vale para todo conector (o 0,6 de fontes
 * conceituais e de historico so existe quando U8 trouxer essas fontes). */
#define UG_RRFK 60.0

static int ug_find(const UioeGraph *g, int kind, const char *path, int line0, const char *name) {
    for (int i = 0; i < g->nn; i++) { const UioeNode *n = &g->n[i]; if (n->kind == kind && n->line0 == line0 && !strcmp(n->path, path) && !strcmp(n->name, name)) return i; }
    return -1;
}
/* acrescenta (ou funde) um no; rank = posicao 1-based na lista do conector; devolve o indice ou -1 se o grafo esta cheio */
static int ug_add(UioeGraph *g, const UioeNode *in, int conn, int rank, const char *conn_id) {
    int i = ug_find(g, in->kind, in->path, in->line0, in->name);
    const double r = 1.0 / (UG_RRFK + (rank < 1 ? 1 : rank));
    if (i >= 0) {
        UioeNode *n = &g->n[i];
        if (n->conn != conn) {
            n->nsrc++;
            if (conn_id && !strstr(n->how, conn_id)) { size_t l = strlen(n->how); if (l + strlen(conn_id) + 12 < sizeof n->how) snprintf(n->how + l, sizeof n->how - l, "; also by %s", conn_id); }
        }
        n->rrf += r;
        if (in->conf > n->conf) n->conf = in->conf;
        if (in->tier && (n->tier == 0 || in->tier < n->tier)) n->tier = in->tier;
        if (in->line1 > n->line1) n->line1 = in->line1;
        if (!n->text[0] && in->text[0]) snprintf(n->text, sizeof n->text, "%s", in->text);
        if (!n->sig[0] && in->sig[0]) snprintf(n->sig, sizeof n->sig, "%s", in->sig);
        if (!n->vkey[0] && in->vkey[0]) snprintf(n->vkey, sizeof n->vkey, "%s", in->vkey);
        if (in->cver > n->cver) n->cver = in->cver;
        return i;
    }
    if (g->nn >= UG_MAXNODE) { g->lost_n++; return -1; }
    UioeNode *n = &g->n[g->nn]; *n = *in; n->id = (uint32_t)g->nn; n->conn = conn; n->nsrc = 1; n->rrf = r;
    if (!n->tier) n->tier = 4;
    return g->nn++;
}
static int ug_edge(UioeGraph *g, int from, int to, int kind, int line, int conf, int ver, int conn, const char *how, const char *key) {
    if (from < 0 || to < 0 || from == to) return -1;
    for (int i = 0; i < g->ne; i++) { UioeEdge *e = &g->e[i]; if (e->from == from && e->to == to && e->kind == kind && e->line == line) { if (conf > e->conf) e->conf = conf; return i; } }
    if (g->ne >= UG_MAXEDGE) { g->lost_e++; return -1; }
    UioeEdge *e = &g->e[g->ne]; memset(e, 0, sizeof *e);
    e->from = from; e->to = to; e->kind = (uint8_t)kind; e->line = line; e->conf = conf; e->ver = (uint8_t)ver; e->conn = conn;
    snprintf(e->how, sizeof e->how, "%s", how ? how : ""); snprintf(e->key, sizeof e->key, "%s", key ? key : "");
    return g->ne++;
}

/* ---- resultado de conector: acrescenta nos e arestas ---- */
static UioeNode *ur_node(UioeResult *r) {
    if (r->nnode == r->capn) { r->capn = r->capn ? r->capn * 2 : 32; r->node = (UioeNode *)realloc(r->node, sizeof(UioeNode) * (size_t)r->capn); if (!r->node) { r->nnode = r->capn = 0; return NULL; } }
    UioeNode *n = &r->node[r->nnode++]; memset(n, 0, sizeof *n); return n;
}
static int ur_edge(UioeResult *r, int from, int to, int kind, int line, int conf, const char *how, const char *key) {
    if (r->nedge == r->cape) { r->cape = r->cape ? r->cape * 2 : 32; r->edge = (UioeEdge *)realloc(r->edge, sizeof(UioeEdge) * (size_t)r->cape); if (!r->edge) { r->nedge = r->cape = 0; return -1; } }
    UioeEdge *e = &r->edge[r->nedge]; memset(e, 0, sizeof *e);
    e->from = from; e->to = to; e->kind = (uint8_t)kind; e->line = line; e->conf = conf;
    snprintf(e->how, sizeof e->how, "%s", how ? how : ""); snprintf(e->key, sizeof e->key, "%s", key ? key : "");
    return r->nedge++;
}
static void ur_free(UioeResult *r) { free(r->node); free(r->edge); memset(r, 0, sizeof *r); }
static void ur_set_text(UioeNode *n, const char *t) {
    while (*t == ' ' || *t == '\t') t++;
    snprintf(n->text, sizeof n->text, "%.190s", t);
}
/* funde o resultado de um conector no grafo; map recebe o indice no grafo de cada no do resultado; devolve quantos nos entraram (novos ou fundidos) */
static int ug_merge(UioeGraph *g, const UioeResult *r, int conn, const char *conn_id, int anchor, int *map, int mapcap) {
    int ok = 0;
    for (int i = 0; i < r->nnode; i++) { const int id = ug_add(g, &r->node[i], conn, i + 1, conn_id); if (i < mapcap) map[i] = id; if (id >= 0) ok++; }
    for (int i = 0; i < r->nedge; i++) {
        const UioeEdge *e = &r->edge[i]; int a = e->from < 0 ? anchor : e->from < mapcap ? map[e->from] : -1, b = e->to < 0 ? anchor : e->to < mapcap ? map[e->to] : -1;
        if (a >= 0 && b >= 0) ug_edge(g, a, b, e->kind, e->line, e->conf, 0, conn, e->how, e->key);
    }
    return ok;
}

/* ---- relevancia: quanto o no casa com o pedido (so ordena) ---- */
/* o texto contem a palavra (sem caixa, so a partir de 4 letras; menores so como palavra inteira) */
static int ug_has_word(const char *hay, const char *w) {
    const size_t wl = strlen(w); if (wl < 3) return 0;
    for (const char *p = hay; (p = strcasestr(p, w)); p++) {
        if (wl >= 4) return 1;
        const int pre = p == hay || !isalnum((unsigned char)p[-1]), post = !isalnum((unsigned char)p[wl]);
        if (pre && post) return 1;
    }
    return 0;
}
/* quantos termos distintos do pedido (e as palavras dos seus conceitos) ocorrem no texto */
static int ug_terms_in(const UioeAsk *a, const char *hay) {
    int n = 0;
    for (int i = 0; i < a->nterms; i++) {
        int hit = ug_has_word(hay, a->terms[i]) || ug_has_word(hay, a->norm[i]);
        for (int j = 0; j < a->nxw && !hit; j++) if (a->xw_of[j] == i && ug_has_word(hay, a->xw[j])) hit = 1;
        n += hit;
    }
    return n;
}
/* termos ponderados: o termo escrito no pedido vale 4; uma palavra do conceito vale 3 (a primeira, de maior peso), 2 (a segunda e a terceira) ou 1 */
static int ug_terms_w(const UioeAsk *a, const char *hay) {
    int sc = 0;
    for (int i = 0; i < a->nterms; i++) {
        if (ug_has_word(hay, a->terms[i]) || ug_has_word(hay, a->norm[i])) { sc += 4; continue; }
        int rank = 0;
        for (int j = 0; j < a->nxw; j++) if (a->xw_of[j] == i) { if (ug_has_word(hay, a->xw[j])) { sc += rank == 0 ? 3 : rank <= 2 ? 2 : 1; break; } rank++; }
    }
    return sc;
}
/* termos do pedido no texto "central" (chave, linha, simbolo) valem 12; so no nome do arquivo, 4: o nome do arquivo e igual para todas as ligacoes dele */
static int ug_rel_terms(const UioeAsk *a, const char *core, const char *base) {
    int sc = 0;
    for (int i = 0; i < a->nterms; i++) {
        int hit = ug_has_word(core, a->terms[i]) || ug_has_word(core, a->norm[i]);
        for (int j = 0; j < a->nxw && !hit; j++) if (a->xw_of[j] == i && ug_has_word(core, a->xw[j])) hit = 1;
        if (hit) { sc += 12; continue; }
        hit = ug_has_word(base, a->terms[i]) || ug_has_word(base, a->norm[i]);
        for (int j = 0; j < a->nxw && !hit; j++) if (a->xw_of[j] == i && ug_has_word(base, a->xw[j])) hit = 1;
        if (hit) sc += 4;
    }
    return sc;
}
/* ordem de exibicao (3.7.4): faixa, depois RRF + relevancia, verificado antes de nao verificado, mesmo arquivo de uma ancora, nao visto, faixa curta, caminho */
static int ug_cmp(const UioeGraph *g, int ia, int ib) {
    const UioeNode *a = &g->n[ia], *b = &g->n[ib];
    if (a->tier != b->tier) return a->tier - b->tier;
    const double sa = a->rrf * 1000.0 + a->relevance * 10.0, sb = b->rrf * 1000.0 + b->relevance * 10.0;
    if (sa != sb) return sa > sb ? -1 : 1;
    const int va = a->ver == UV_OK || a->ver == UV_MOVED, vb = b->ver == UV_OK || b->ver == UV_MOVED;
    if (va != vb) return vb - va;
    if (a->anchor != b->anchor) return b->anchor - a->anchor;
    if (a->shown + a->read != b->shown + b->read) return (a->shown + a->read) - (b->shown + b->read);
    const int la = a->line1 - a->line0, lb = b->line1 - b->line0; if (la != lb) return la - lb;
    const int c = strcmp(a->path, b->path); if (c) return c;
    return a->line0 - b->line0;
}
/* ordena ids[0..n) com a regra acima (insercao: n e pequeno, e a ordem e estavel e sem estado global) */
static void ug_sort(const UioeGraph *g, int *ids, int n) {
    for (int i = 1; i < n; i++) { const int x = ids[i]; int j = i; while (j > 0 && ug_cmp(g, ids[j - 1], x) > 0) { ids[j] = ids[j - 1]; j--; } ids[j] = x; }
}
