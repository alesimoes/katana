/* ---------- uioe/ctxb.c: o Construtor de Contexto (docs/uioe.md 3.9 e o formato normativo de 4.6, fase U4) ----------
 * Recebe o resultado da recuperacao (grafo de evidencias, caminho, lacunas, limites, confianca) e devolve UM bloco de texto para o alvo (Qwen):
 *   [katana: context for this request. ...]
 *   Question understood as: ...        o que o programa entendeu (em ingles, de um modelo de frase por tipo de tarefa; nunca as palavras do usuario de volta)
 *   Answer path: | Findings:           passos numerados `N. [C] arquivo:linhas texto`; [C] so quando a linha foi RELIDA; senao `(guess)`
 *   Evidence:                          o que apoia e nao e passo (no maximo 6 linhas); o que o pedido ja mostrou vira UMA linha "already shown"
 *   Links:                             ligacoes por chave irmas das do caminho e pontas abertas (`-> nobody handles it in the indexed set`)
 *   Not found / limits:                SEMPRE presente; cada clausula diz o que nao se achou ou nao se olhou e o MOTIVO
 *   Next:                              imperativo com criterio de parada (Read FILE lines a-b, then answer. Do not search again ...)
 *   [end of context]
 * Os sete passos de 3.9.1: B1 selecionar (caminho, depois apoio, depois ligacoes), B2 deduplicar (contra o pedido, contra o estado do pedido e contra si
 * mesmo), B3 ordenar (ordem fixa; o mais relevante primeiro), B4 comprimir por modelos de frase (verbos fixos; citacao literal so no passo e nas linhas de
 * documento, cortada em `quote_cols`), B5 orcamento (4 bytes por token; corte na ordem de 4.6.3), B6 renderizar, B7 registrar (o que entrou e o que saiu,
 * para o traco, e o que passa a contar como "mostrado" no estado do pedido).
 * Decisoes do implementador:
 *   - O texto do bloco e SEMPRE em ingles (rotulos, frases e a linha de entendimento), qualquer que seja o idioma do pedido: o formato e fixo para ser
 *     treinavel (4.6.1 regra 3); o idioma do pedido so entra nas citacoes literais do codigo e nos nomes.
 *   - Sem caminho (impacto, uso): o "Answer path" e a definicao seguida dos chamadores por profundidade; sem definicao verificada nao ha passo 1 e nao ha bloco.
 *     Tipos sem cadeia (localizar, configuracao, erro, como fazer): `Findings:` com os melhores itens.
 *   - ESCAPE: todo texto que veio de arquivo, documento ou nome passa por cxb_esc: `[katana` e `[end of context` ganham um espaco depois do colchete, os
 *     marcadores de formato de conversa (`<|`, tool_call, tool_response) sao neutralizados como em defang, formas de segredo sao mascaradas (kn_mask),
 *     quebras de linha viram espaco e a citacao e cortada em 110 colunas. Sempre, em todo campo de origem externa.
 *   - O bloco nunca leva valor de arquivo marcado como segredo (n->secret): so o nome e o lugar. */
static void kn_mask(const char *s, Buf *out);      /* app/knowledge.c, mais adiante na unidade */

typedef struct { const char *name; int tokens_max, bpt10, quote_cols, steps_max; } UioeTarget;
static const UioeTarget g_uioe_target_qwen = {"qwen-chatml", 1500, 40, 110, 5};

typedef struct {
    int tokens, bytes, nsteps, nevid, nlinks, nlimits, ded_req, ded_state, ded_self, cut, findings;
    int tok_head, tok_path, tok_evid, tok_links, tok_lim, tok_next;
    int shown[80], nshown;              /* nos que o bloco mostrou (B7) */
    char why[200];                      /* por que nao saiu bloco (vazio quando saiu) */
    char log[1800];                     /* o que entrou e o que saiu, para o traco */
} UioePackStats;

/* ---- escape e limpeza de texto de origem externa ---- */
static void cxb_esc2(char *dst, size_t cap, const char *src, int cols, int mask) {
    char t[1400]; size_t o = 0; int sp = 1;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p && o + 1 < sizeof t; p++) {
        unsigned char c = *p; if (c < 0x20 || c == 0x7f) c = ' ';
        if (c == ' ') { if (sp) continue; sp = 1; } else sp = 0;
        t[o++] = (char)c;
    }
    while (o && t[o - 1] == ' ') o--;
    t[o] = 0; int cut = 0;
    if (cols > 0 && (int)o > cols) { size_t k = (size_t)cols; while (k && ((unsigned char)t[k] & 0xC0) == 0x80) k--; t[k] = 0; cut = 1; }
    Buf b = {0}; if (mask) kn_mask(t, &b); const char *m = b.p ? b.p : t;      /* caminhos nao passam pela mascara: um UUID numa pasta e so uma pasta */
    size_t d = 0;
    for (const char *p = m; *p && d + 6 < cap; p++) {
        if (*p == '[' && !strncasecmp(p + 1, "katana", 6)) { dst[d++] = '['; dst[d++] = ' '; continue; }
        if (*p == '[' && !strncasecmp(p + 1, "end of context", 14)) { dst[d++] = '['; dst[d++] = ' '; continue; }
        if (*p == '<' && (p[1] == '|' || !strncmp(p + 1, "tool_call", 9) || !strncmp(p + 1, "/tool_call", 10) || !strncmp(p + 1, "tool_response", 13) || !strncmp(p + 1, "/tool_response", 14))) { dst[d++] = '<'; dst[d++] = ' '; continue; }
        dst[d++] = *p;
    }
    if (cut && d + 4 < cap) { dst[d++] = '.'; dst[d++] = '.'; dst[d++] = '.'; }
    dst[d] = 0; free(b.p);
}
static void cxb_esc(char *dst, size_t cap, const char *src, int cols) { cxb_esc2(dst, cap, src, cols, 1); }
static void cxb_escp(char *dst, size_t cap, const char *src) { cxb_esc2(dst, cap, src, 0, 0); }
static int cxb_ok(const UioeNode *n) { return n->ver == UV_OK || n->ver == UV_MOVED; }
static void cxb_mark(char *d, size_t cap, const UioeNode *n) { if (cxb_ok(n)) snprintf(d, cap, "[%d]", n->conf > 99 ? 99 : n->conf < 1 ? 1 : n->conf); else snprintf(d, cap, "(guess)"); }
static void cxb_loc(char *d, size_t cap, const UioeNode *n) {
    char p[420]; cxb_escp(p, sizeof p, n->path);
    if (n->kind == UN_FILE || n->kind == UN_FOLDER || n->line0 <= 0) snprintf(d, cap, "%s", p);
    else if (n->line1 > n->line0) snprintf(d, cap, "%s:%d-%d", p, n->line0, n->line1);
    else snprintf(d, cap, "%s:%d", p, n->line0);
}
static int cxb_depth(const UioeNode *n) { const char *p = strstr(n->how, "depth "); return p ? atoi(p + 6) : 1; }
static const char *cxb_clsword(const char *c) {
    return !strcmp(c, "msg") ? "message" : !strcmp(c, "event") ? "event" : !strcmp(c, "route") ? "route" : !strcmp(c, "env") ? "environment variable" : !strcmp(c, "config") ? "config key" : !strcmp(c, "cmd") ? "command" : "key";
}

/* ---- B4: uma frase por no, de um modelo fixo ----
 * role: 1 = o no envia a chave, 2 = o no trata a chave, 0 = nao se sabe (decide o `how` do no). quote: 1 = com a linha citada. */
static void cxb_node_text(char *out, size_t cap, const UioeNode *n, int role, int quote, int cols) {
    char nm[130], sg[220], q[320], t[760];
    cxb_esc(nm, sizeof nm, n->name, 100); cxb_esc(sg, sizeof sg, n->sig, 150);
    q[0] = 0; if (quote && n->text[0] && !n->secret && strcmp(n->cls, "caller") && strcmp(n->cls, "test")) cxb_esc(q, sizeof q, n->text, cols);      /* a linha de um chamador e a de sua definicao, nao a da chamada: nao e decisiva */
    if (n->kind == UN_KEY) {
        if (!role) role = strstr(n->how, "writes, sets or sends it") ? 1 : strstr(n->how, "reads or handles it") ? 2 : 0;
        const int insig = !strncmp(sg, "in ", 3);
        const int cfg = !strcmp(n->cls, "env") || !strcmp(n->cls, "config") || !strcmp(n->cls, "cmd");      /* variavel e chave de configuracao: define e le; mensagem, evento e rota: envia e trata */
        snprintf(t, sizeof t, "%s %s \"%s\"%s%s", role == 1 ? (cfg ? "sets" : "sends") : role == 2 ? (cfg ? "reads" : "handles") : "mentions", cxb_clsword(n->cls), nm, insig ? " " : "", insig ? sg : "");
    } else if (n->kind == UN_SYMBOL && (!strcmp(n->cls, "caller") || !strcmp(n->cls, "test"))) {
        snprintf(t, sizeof t, "%s() %s", nm, sg[0] ? sg : "calls the target");
        if (!strcmp(n->cls, "caller")) { const int d = cxb_depth(n); if (d > 1) { size_t l = strlen(t); snprintf(t + l, sizeof t - l, " (depth %d)", d); } }
    } else if (n->kind == UN_SYMBOL) {
        snprintf(t, sizeof t, "defines %s %s%s%s", n->cls[0] ? n->cls : "symbol", nm, sg[0] && !q[0] ? " " : "", q[0] ? "" : sg);
    } else if (n->kind == UN_DOCLINE) {
        snprintf(t, sizeof t, "document text");
        if (!q[0] && n->text[0]) cxb_esc(q, sizeof q, n->text, cols);
    } else if (n->kind == UN_FILE) {
        snprintf(t, sizeof t, "%s%s%s", nm[0] && strcmp(nm, "file") ? nm : "file", sg[0] ? " " : "", sg);
    } else {      /* faixa: o nome e a assinatura dizem o que e (import, literal...) */
        snprintf(t, sizeof t, "%s%s%s", nm, sg[0] ? " " : "", sg);
    }
    if (q[0]) snprintf(out, cap, "%s: %s", t, q); else snprintf(out, cap, "%s", t);
}

/* ---- as linhas ---- */
#define CB_MAXLINE 14
typedef struct { char mark[12]; char loc[430]; char txt[900]; char qless[420]; int id; int hasq; } CbLine;       /* qless: o texto sem a citacao (corte de 4.6.3) */
typedef struct { CbLine step[8]; int nstep; CbLine ev[CB_MAXLINE]; int nev; char link[8][400]; int nlink; char lim[14][320]; int nlim; char leftout[400]; char already[420]; char tests[420]; CbLine mem[4]; int nmem; } CbDoc;      /* mem: lembrancas do dono (U8), sempre depois do resto de Evidence */

static void cxb_line_from(CbLine *L, const UioeGraph *g, int id, int role, int quote, int cols) {
    const UioeNode *n = &g->n[id]; L->id = id;
    cxb_mark(L->mark, sizeof L->mark, n); cxb_loc(L->loc, sizeof L->loc, n);
    cxb_node_text(L->txt, sizeof L->txt, n, role, quote, cols); cxb_node_text(L->qless, sizeof L->qless, n, role, 0, cols);
    L->hasq = quote && n->text[0] && !n->secret && strcmp(L->txt, L->qless) != 0;
    if (n->secret) { size_t l = strlen(L->txt); snprintf(L->txt + l, sizeof L->txt - l, " (a secret file: the value is never printed)"); snprintf(L->qless, sizeof L->qless, "%s", L->txt); }
    if (!cxb_ok(n) && n->vnote[0]) { char v[120]; cxb_esc(v, sizeof v, n->vnote, 80); size_t l = strlen(L->txt); snprintf(L->txt + l, sizeof L->txt - l, " (%s)", v); snprintf(L->qless + strlen(L->qless), sizeof L->qless - strlen(L->qless), " (%s)", v); }
}
static int cxb_in(const int *a, int n, int v) { for (int i = 0; i < n; i++) if (a[i] == v) return 1; return 0; }
static int cxb_typed_path(const UioeAsk *a, const UioeNode *n) {      /* o usuario digitou este arquivo: nao se explica de volta */
    for (int i = 0; i < a->nent; i++) { const UioeEntity *e = &a->ent[i]; if (e->kind != UK_PATH || !e->resolved[0]) continue; const size_t L = strlen(n->path); const size_t R = strlen(e->resolved);
        if (L && R >= L && !strcmp(e->resolved + R - L, n->path)) return 1; }
    return 0;
}

/* B1 e B2: escolhe os passos, o apoio e as ligacoes */
static void cxb_select(const UioeCtx *cx, const UioeRequest *r, const UioeTarget *t, CbDoc *D, UioePackStats *st) {
    const UioeRecall *rc = &r->rc; const UioeGraph *g = rc->g; const UioeAsk *a = &r->ask; const UioeClass *k = &r->cls;
    int used[96]; int nused = 0; int pathid[8], role[8], np = 0;
    /* --- o caminho --- */
    if (rc->npath >= 2) {
        for (int i = 0; i < rc->npath && np < t->steps_max; i++) {
            const UioeNode *n = &g->n[rc->path[i]]; int ro = 0;
            if (i > 0 && rc->pathedge[i - 1] >= 0 && g->e[rc->pathedge[i - 1]].kind == UE_LINKS && !strcmp(g->e[rc->pathedge[i - 1]].key, n->name)) ro = 2;
            else if (rc->pathedge[i] >= 0 && g->e[rc->pathedge[i]].kind == UE_LINKS && !strcmp(g->e[rc->pathedge[i]].key, n->name)) ro = 1;
            pathid[np] = rc->path[i]; role[np++] = ro; (void)n;
        }
    } else if (k->kind == UI_IMPACT || k->kind == UI_USAGE) {
        int def = -1;
        for (int i = 0; i < rc->nitems; i++) { const UioeNode *n = &g->n[rc->items[i]]; if (n->kind == UN_SYMBOL && strcmp(n->cls, "caller") && strcmp(n->cls, "test") && n->tier == 1 && cxb_ok(n)) { def = rc->items[i]; break; } }
        if (def >= 0) {
            pathid[np] = def; role[np++] = 0;
            for (int depth = 1; depth <= 3 && np < t->steps_max; depth++)
                for (int i = 0; i < rc->nitems && np < t->steps_max; i++) { const UioeNode *n = &g->n[rc->items[i]]; if (n->kind == UN_SYMBOL && !strcmp(n->cls, "caller") && cxb_depth(n) == depth && cxb_ok(n)) { pathid[np] = rc->items[i]; role[np++] = 0; } }
        }
    }
    for (int i = 0; i < np; i++) { cxb_line_from(&D->step[i], g, pathid[i], role[i], 1, t->quote_cols); used[nused++] = pathid[i];
        const UioeNode *n = &g->n[pathid[i]]; if (n->shown || n->read) { D->step[i].hasq = 0; snprintf(D->step[i].txt, sizeof D->step[i].txt, "%s (already %s to you: not quoted again)", D->step[i].qless, n->read ? "read" : "shown"); st->ded_state++; } }
    D->nstep = np;
    /* sem caminho: `Findings` com os melhores itens verificados */
    if (!np) {
        st->findings = 1;
        for (int i = 0; i < rc->nitems && D->nstep < t->steps_max; i++) { const UioeNode *n = &g->n[rc->items[i]];
            if (n->kind == UN_FILE || n->kind == UN_FOLDER || n->dropped || !cxb_ok(n) || n->conf < 55 || n->tier > 3 || n->shown || n->read || !strcmp(n->cls, "test")) continue;
            if (k->kind == UI_LOCATE && n->tier > 2 && D->nstep >= 3) continue;
            cxb_line_from(&D->step[D->nstep++], g, rc->items[i], 0, 1, t->quote_cols); used[nused++] = rc->items[i]; }
    }
    /* --- ligacoes: as irmas das do caminho (mesmo par de arquivos) e as pontas abertas --- */
    typedef struct { int e, conf; } LK; LK lk[40]; int nlk = 0;
    for (int i = 0; i < g->ne && nlk < 40; i++) {
        const UioeEdge *e = &g->e[i]; if (e->kind != UE_LINKS || e->ver != UV_OK || e->conf < 50) continue;
        int on = 0; for (int j = 0; j + 1 < rc->npath; j++) if (rc->pathedge[j] == i) on = 1; if (on) continue;
        const UioeNode *A = &g->n[e->from], *B = &g->n[e->to]; int rel = 0;
        for (int j = 0; j + 1 < rc->npath && !rel; j++) { const UioeNode *P = &g->n[rc->path[j]], *C = &g->n[rc->path[j + 1]]; if (!strcmp(A->path, P->path) && !strcmp(B->path, C->path)) rel = 1; }
        if (!rel && !rc->npath) for (int j = 0; j < D->nstep; j++) if (D->step[j].id == e->from || D->step[j].id == e->to) rel = 1;
        if (!rel) continue;
        { char hz[900]; snprintf(hz, sizeof hz, "%s %s %s", e->key, A->text, B->text); lk[nlk].conf = e->conf + 15 * ug_terms_in(a, hz); lk[nlk].e = i; nlk++; }      /* as que casam com o pedido primeiro */
    }
    for (int a2 = 0; a2 < nlk; a2++) for (int b2 = a2 + 1; b2 < nlk; b2++) if (lk[b2].conf > lk[a2].conf) { const LK x = lk[a2]; lk[a2] = lk[b2]; lk[b2] = x; }
    for (int i = 0; i < nlk && D->nlink < (rc->npath >= 2 ? 6 : 3); i++) {
        const UioeEdge *e = &g->e[lk[i].e]; const UioeNode *A = &g->n[e->from], *B = &g->n[e->to]; char key[90], pa[300], pb[300];
        cxb_esc(key, sizeof key, e->key, 60); cxb_escp(pa, sizeof pa, A->path); cxb_escp(pb, sizeof pb, B->path);
        int dup = 0; for (int j = 0; j < D->nlink; j++) { char probe[200]; snprintf(probe, sizeof probe, "\"%s\" %s:%d -> %s:%d", key, pa, A->line0, pb, B->line0); if (strstr(D->link[j], probe)) dup = 1; }
        if (dup) { st->ded_self++; continue; }
        snprintf(D->link[D->nlink++], sizeof D->link[0], "\"%s\" %s:%d -> %s:%d [%d]", key, pa, A->line0, pb, B->line0, e->conf);
        if (!cxb_in(used, nused, e->from)) { used[nused++] = e->from; }
        if (!cxb_in(used, nused, e->to)) { used[nused++] = e->to; }      /* o que a ligacao ja diz nao se repete em Evidence */
    }
    for (int i = 0; i < g->nn && D->nlink < 6; i++) { const UioeNode *n = &g->n[i];
        if (n->kind == UN_KEY && n->conf == 0 && strstr(n->how, "nobody") && (n->onpath || n->relevance >= 36)) { char key[90], pa[300], w[90]; cxb_esc(key, sizeof key, n->name, 60); cxb_escp(pa, sizeof pa, n->path); cxb_esc(w, sizeof w, strstr(n->how, "nobody"), 80);
            snprintf(D->link[D->nlink++], sizeof D->link[0], "\"%s\" %s:%d -> %s", key, pa, n->line0, w); } }
    /* --- Evidence: o resto dos itens, sem repetir o que ja foi dito --- */
    int ntests = 0; D->tests[0] = 0; D->already[0] = 0; int nal = 0;
    for (int i = 0; i < rc->nitems; i++) {
        const int id = rc->items[i]; const UioeNode *n = &g->n[id];
        if (n->kind == UN_FACT) continue;      /* U8: a memoria tem o seu bloco logo abaixo */
        if (cxb_in(used, nused, id) || n->dropped || n->kind == UN_FOLDER) continue;
        if (n->kind == UN_FILE && k->kind != UI_OVERVIEW) continue;
        if (n->ver == UV_STALE || n->ver == UV_GONE || n->ver == UV_UNREADABLE) continue;
        if (cxb_typed_path(a, n)) { st->ded_req++; continue; }       /* o pedido ja traz este arquivo */
        char loc[430]; cxb_loc(loc, sizeof loc, n);
        if (n->shown || n->read) { if (nal < 6) { size_t l = strlen(D->already); snprintf(D->already + l, sizeof D->already - l, "%s%s", nal ? ", " : "", loc); nal++; } st->ded_state++; continue; }
        if (!strcmp(n->cls, "test")) { if (ntests < 2 && cxb_ok(n)) { char nm[130], mk[12]; cxb_esc(nm, sizeof nm, n->name, 80); cxb_mark(mk, sizeof mk, n); size_t l = strlen(D->tests); snprintf(D->tests + l, sizeof D->tests - l, "%s%s %s %s()", ntests ? "; " : "", mk, loc, nm); ntests++; } continue; }
        if (n->conf < 45 || n->tier > 3) continue;
        if ((k->kind == UI_FLOW || k->kind == UI_LOCATE) && (n->kind == UN_KEY || n->kind == UN_RANGE)) { char hz[700]; snprintf(hz, sizeof hz, "%s %s %s", n->name, n->sig, n->text); if (ug_terms_in(a, hz) < 2) continue; }      /* chave sem relacao com as palavras do pedido: ruido */
        if (D->nev < CB_MAXLINE && D->nev < (k->kind == UI_FLOW ? 3 : 6)) {      /* o que nao e passo nem ligacao: poucas linhas, so as que casam com o pedido */
             cxb_line_from(&D->ev[D->nev++], g, id, 0, n->kind == UN_DOCLINE || n->kind == UN_RANGE, t->quote_cols); used[nused++] = id; }
    }
    /* U8 (decisao do dono, 2026-11-23): a memoria entra como evidencia DATADA e de BAIXA confianca, rotulada como lembranca do dono: `(guess) memory DATA (id X): "texto" (...)`.
       Fica depois de todo o resto de Evidence, e e a primeira coisa cortada por espaco. Nunca ha valor de segredo: memory_query omite o fato, uioe_call e cxb_esc mascaram. */
    { const long today = (long)(time(NULL) / 86400);
      for (int i = 0; i < rc->nitems && D->nmem < 3; i++) {
        const UioeNode *n = &g->n[rc->items[i]]; if (n->kind != UN_FACT || n->dropped || n->secret) continue;
        CbLine *L = &D->mem[D->nmem++]; char dt[48], idn[24], q[470], note[96] = "";
        L->id = rc->items[i]; L->hasq = 0; snprintf(L->mark, sizeof L->mark, "(guess)");
        if (!strcmp(n->cls, "page") || !strcmp(n->cls, "web")) {      /* U9: pagina ou resultado da web: a URL e a hora da busca (pagina) ou a data do resultado, nunca verificado */
            const int pg = !strcmp(n->cls, "page"); char u[260], src[110];
            if (pg) { cxb_escp(dt, sizeof dt, n->name); cxb_escp(u, sizeof u, n->sig); cxb_esc(q, sizeof q, n->text, 190);
                snprintf(L->loc, sizeof L->loc, "web page %s (read %s):", u, dt); snprintf(L->txt, sizeof L->txt, "\"%s\" (outside content, start of the passages; not checked)", q); }
            else { const char *w = strstr(n->how, "web result "); cxb_escp(src, sizeof src, n->name); cxb_escp(dt, sizeof dt, n->sig[0] ? n->sig : "undated"); cxb_escp(u, sizeof u, w ? w + 11 : "");
                cxb_esc(q, sizeof q, n->text, t->quote_cols * 2); snprintf(L->loc, sizeof L->loc, "web result %s (%s, %s):", u, src, dt); snprintf(L->txt, sizeof L->txt, "\"%s\" (outside content; not checked)", q); }
            snprintf(L->qless, sizeof L->qless, "%s", L->txt); continue; }
        cxb_escp(dt, sizeof dt, n->sig); cxb_escp(idn, sizeof idn, n->name); cxb_esc(q, sizeof q, n->text, t->quote_cols);
        snprintf(L->loc, sizeof L->loc, "memory %s (id %s):", dt, idn);
        const long dd = mem_days(n->sig); const long age = dd >= 0 && today > dd ? today - dd : 0;
        if (age >= 180) snprintf(note, sizeof note, "; saved %ld days ago, the code may have changed since", age);
        snprintf(L->txt, sizeof L->txt, "\"%s\" (a saved note of the owner; not checked against the code%s)", q, note);
        snprintf(L->qless, sizeof L->qless, "%s", L->txt);
      } }
    for (int i = 0; i < D->nstep && st->nshown < 80; i++) st->shown[st->nshown++] = D->step[i].id;
    (void)cx;
}

/* Not found / limits: sempre presente, cada clausula com o motivo */
static void cxb_limits(const UioeCtx *cx, const UioeRequest *r, const UioeTarget *t, CbDoc *D) {
    const UioeRecall *rc = &r->rc; const UioeRoute *rt = &r->route; const UioeClass *k = &r->cls; char c[800];
#define CL(...) do { if (D->nlim < 14) { snprintf(c, sizeof c, __VA_ARGS__); cxb_esc(D->lim[D->nlim], sizeof D->lim[0], c, 300); D->nlim++; } } while (0)
    char sc[260]; cxb_esc(sc, sizeof sc, rc->scope, 200);
    if (k->kind == UI_WEB) CL("web content comes from outside the machine and is shown as read at the time given; it is not checked against the code or the files and may be wrong or out of date");
    else if (k->kind == UI_HISTORY) CL("only the owner's saved memory was searched, not the code or the files; a note is the owner's own dated recollection");
    else if (rc->idx_mode == 2) CL("only the indexed set was searched (%s); another project, an ignored folder or a file outside it is not seen", sc);
    else if (rc->idx_mode == 1) CL("the index is partial (%s): what is listed is what exists so far, and a match may be missing", sc);
    else CL("no index covers this folder (%s): files were scanned by text only", sc);
    for (int i = 0; i < rc->nhyp; i++) if (rc->hyp[i][0] == '"' && strstr(rc->hyp[i], "nobody")) { CL("no counterpart in the indexed set: %s", rc->hyp[i]); break; }
    { int nb = 0; for (int i = 0; i < rc->nhyp; i++) if (!strncmp(rc->hyp[i], "not followed:", 13)) { if (!nb) CL("other connections were not followed (weaker or alternative): %s", rc->hyp[i] + 14); nb++; } if (nb > 1) { D->nlim--; CL("%d other connections were not followed (weaker or alternative); the first: %s", nb, strstr(rc->hyp[0], "not followed:") ? rc->hyp[0] + 14 : ""); } }
    for (int i = 0; i < rc->nlim; i++) {
        const char *l = rc->limits[i];
        if (!strncmp(l, "code.search:", 12)) { if (k->kind == UI_FLOW || k->kind == UI_LOCATE) CL("the starting files were chosen by the request's words: a file that does not use them is not found"); continue; }
        if (!strncmp(l, "code.flow:", 10) || !strncmp(l, "code.symbols", 12) || !strncmp(l, "docs.search:", 12) || !strncmp(l, "scope:", 6) || !strncmp(l, "not consulted:", 14)) continue;
        CL("%s", l);
    }
    if (cx->s && cx->s->todo_open > 0) CL("an earlier todo list still has %d unfinished step(s); this message is newer and comes first: if it changes the plan, follow it and rewrite or drop the old steps", cx->s->todo_open);
    if (rt->notrouted[0]) { char nr[300]; snprintf(nr, sizeof nr, "%s", rt->notrouted); for (char *p = nr; *p; p++) if (*p == ';') *p = ','; size_t L = strlen(nr); while (L && (nr[L - 1] == ' ' || nr[L - 1] == ',')) nr[--L] = 0; CL("not consulted: %s", nr); }
    if (rc->nnotreread) CL("%d item(s) are shown without a reread (verification budget)", rc->nnotreread);
    if (rc->nstale) CL("%d item(s) failed the reread because the file changed since it was indexed, and were left out", rc->nstale);
    if (rc->nitems_all > rc->nitems) CL("%d more candidate(s) qualified and were not shown or reread", rc->nitems_all - rc->nitems);
    if (D->leftout[0]) CL("left out for space: %s", D->leftout);
    (void)cx; (void)t;
#undef CL
}

/* Question understood as: um modelo de frase por tipo de tarefa */
static void cxb_question(const UioeCtx *cx, const UioeRequest *r, char *out, size_t cap) {
    const UioeAsk *a = &r->ask; const UioeClass *k = &r->cls; const UioeRecall *rc = &r->rc; char terms[400] = ""; int nt = 0; char seen[8][48];
    /* os termos do pedido que aparecem no caminho (na forma que casou, em ingles quando foi o conceito); sem caminho, os primeiros */
    char hay[3000]; hay[0] = 0;
    for (int i = 0; i < rc->npath; i++) { const UioeNode *n = &rc->g->n[rc->path[i]]; size_t l = strlen(hay); snprintf(hay + l, sizeof hay - l, " %s %s %s %s", n->name, n->sig, n->text, n->path); }
    for (int pass = 0; pass < 2 && nt < 5; pass++) for (int i = 0; i < a->nterms && nt < 5; i++) {
        const char *w = NULL; const char *base = strrchr(a->root, '/'); base = base ? base + 1 : a->root;
        if (!strcasecmp(a->terms[i], base) || !strcasecmp(a->norm[i], base)) continue;       /* o nome do projeto ja esta em "project" */
        if (pass == 0) { if (!hay[0]) continue; if (ug_has_word(hay, a->terms[i])) w = a->terms[i]; else if (ug_has_word(hay, a->norm[i])) w = a->norm[i];
            for (int j = 0; j < a->nxw && !w; j++) if (a->xw_of[j] == i && ug_has_word(hay, a->xw[j])) w = a->xw[j]; }
        else { if (nt) break; w = a->norm[i][0] ? a->norm[i] : a->terms[i]; for (int j = 0; j < a->nxw; j++) if (a->xw_of[j] == i && a->xw[j][0]) { w = a->xw[j]; break; } }
        if (!w || strlen(w) < 3) continue;
        int dup = 0; for (int j = 0; j < nt; j++) if (!strcasecmp(seen[j], w)) dup = 1; if (dup) continue;
        snprintf(seen[nt], sizeof seen[0], "%.47s", w); size_t l = strlen(terms); char e[80]; cxb_esc(e, sizeof e, w, 40); snprintf(terms + l, sizeof terms - l, "%s%s", nt ? ", " : "", e); nt++;
    }
    char sym[130] = ""; for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == UK_SYMBOL || a->ent[i].kind == UK_ENV || a->ent[i].kind == UK_KEY) { cxb_esc(sym, sizeof sym, a->ent[i].text, 100); break; }
    char what[520];
    switch (k->kind) {
    case UI_FLOW: snprintf(what, sizeof what, "trace how %s%s%s is %s and where it is handled", nt ? "(" : "the data in question", terms, nt ? ")" : "", !strcmp(a->dir, "recv") ? "received" : "sent"); break;
    case UI_IMPACT: snprintf(what, sizeof what, "find who calls %s and what a change to it can break", sym[0] ? sym : "the symbol"); break;
    case UI_USAGE: snprintf(what, sizeof what, "find where %s is defined and who uses it", sym[0] ? sym : "the symbol"); break;
    case UI_CONFIG: snprintf(what, sizeof what, "find where %s is set or defined and where it is read", sym[0] ? sym : nt ? terms : "the setting"); break;
    case UI_ERROR: snprintf(what, sizeof what, "find where this error comes from%s%s", nt ? " (" : "", nt ? terms : ""); if (nt) { size_t l = strlen(what); snprintf(what + l, sizeof what - l, ")"); } break;
    case UI_HOWTO: snprintf(what, sizeof what, "find what the project documents say about %s", nt ? terms : "this"); break;
    case UI_OVERVIEW: snprintf(what, sizeof what, "describe the structure and purpose of the project"); break;
    case UI_WEB: snprintf(what, sizeof what, "find out from the web%s%s%s", nt ? " (" : "", nt ? terms : "", nt ? ")" : ""); break;
    case UI_HISTORY: snprintf(what, sizeof what, "recall what the owner saved in memory about %s (earlier agreements, preferences or decisions)", nt ? terms : "this"); break;
    default: snprintf(what, sizeof what, "locate %s", sym[0] ? sym : nt ? terms : "what was asked"); break;
    }
    char root[300] = ""; if (a->root[0]) { char e[300]; cxb_escp(e, sizeof e, a->root); snprintf(root, sizeof root, " (project %s%s)", e, a->place_note ? "; the folder you named does not exist from the working folder" : ""); }
    snprintf(out, cap, "%s%s.", what, root); (void)cx;
}

/* Next: imperativo, no maximo duas acoes, com criterio de parada */
static void cxb_next(const UioeCtx *cx, const UioeRequest *r, const CbDoc *D, int limits_form, char *out, size_t cap) {
    const UioeRecall *rc = &r->rc; const UioeAsk *a = &r->ask; const UioeGraph *g = rc->g; char act[420] = "";
    char root[300]; cxb_escp(root, sizeof root, a->root);
    const int foreign = a->root[0] && strcmp(a->root, cx->cwd) != 0;
    if (r->cls.kind == UI_WEB) { char e[420]; cxb_esc(e, sizeof e, rc->next, 400); snprintf(out, cap, "%s", e); return; }      /* U9: o Next da web e decidido na recuperacao (orch_next) */
    if (limits_form) {
        if (a->place_note || strstr(rc->decision_note, "cited place")) snprintf(act, sizeof act, "Pass path=%s to Find, Symbols and Grep, then answer.", root);
        else if (strstr(rc->decision_note, "does not exist")) snprintf(act, sizeof act, "Tell the user the name does not exist in this project, or ask for the exact name.");
        else snprintf(act, sizeof act, "Use Grep and Bash ls%s%s%s; the index does not cover everything yet.", foreign ? " with path=" : "", foreign ? root : "", "");
        snprintf(out, cap, "%s", act); return;
    }
    /* o ultimo passo ainda nao lido; se todos foram lidos, responder */
    int target = -1;
    if (rc->npath >= 2) { for (int i = D->nstep - 1; i >= 0 && target < 0; i--) { const UioeNode *n = &g->n[D->step[i].id]; if (n->path[0] && n->kind != UN_FILE && !n->read && cxb_ok(n)) target = D->step[i].id; } }      /* cadeia: o ultimo elo ainda nao lido */
    else for (int i = 0; i < D->nstep && target < 0; i++) { const UioeNode *n = &g->n[D->step[i].id]; if (n->path[0] && n->kind != UN_FILE && !n->read && cxb_ok(n)) target = D->step[i].id; }      /* sem cadeia: o primeiro achado (a definicao) */
    if (D->nstep > 0 && target >= 0) {
        const UioeNode *n = &g->n[target]; int x = n->line0 > 0 ? n->line0 : 1, y = n->line1 > x ? n->line1 : x + 12; if (y - x > 60) y = x + 60;
        char p[420]; cxb_escp(p, sizeof p, n->path); snprintf(act, sizeof act, "Read %s lines %d-%d, then answer.", p, x, y);
    } else if (D->nstep > 0) snprintf(act, sizeof act, "Answer from the path above.");
    else snprintf(act, sizeof act, "Answer from the evidence above.");
    if (foreign && (a->place_note)) { size_t l = strlen(act); snprintf(act + l, sizeof act - l, " If you must search, pass path=%s to Find, Symbols and Grep.", root); }
    snprintf(out, cap, "%s Do not search again for what is listed above.", act);
}

/* B6: escreve o bloco a partir do documento e dos cortes pedidos; devolve os bytes */
static size_t cxb_render(const UioeCtx *cx, const UioeRequest *r, const CbDoc *D, int limits_form, int findings, int nev_max, int nlink_max, int quote_upto, const char *question, const char *next, char *out, size_t cap, UioePackStats *st) {
    (void)r; size_t o = 0; int mark[7] = {0};
#define W(...) do { if (o + 2 < cap) o += (size_t)snprintf(out + o, cap - o, __VA_ARGS__); } while (0)
    if (!cx->tool_form) W("[katana: context for this request. Gathered by the program before you started. Items marked with a number were checked in the files; (guess) means not checked.]\n");
    if (!cx->tool_form || question[0]) W("Question understood as: %s\n", question);      /* facade: question vazia = a consulta e um identificador (4.6.6) */
    mark[0] = (int)o;
    if (!limits_form) {
        if (D->nstep) { W("%s\n", findings ? "Findings:" : "Answer path:"); for (int i = 0; i < D->nstep; i++) W("%d. %s %s %s\n", i + 1, D->step[i].mark, D->step[i].loc, i >= quote_upto && D->step[i].hasq ? D->step[i].qless : D->step[i].txt); }
        mark[1] = (int)o;
        const int ne = D->nev < nev_max ? D->nev : nev_max;
        if (ne || D->nmem || D->tests[0] || D->already[0]) { W("Evidence:\n"); for (int i = 0; i < ne; i++) W("- %s %s %s\n", D->ev[i].mark, D->ev[i].loc, D->ev[i].txt);
            if (D->tests[0]) { W("- tests that call it: %s\n", D->tests); }
            if (D->already[0]) W("- already shown to you, not repeated: %s\n", D->already);
            for (int i = 0; i < D->nmem; i++) W("- %s %s %s\n", D->mem[i].mark, D->mem[i].loc, D->mem[i].txt); }
        mark[2] = (int)o;
        const int nl = D->nlink < nlink_max ? D->nlink : nlink_max;
        if (nl) { if (nl == 1) W("Links: %s\n", D->link[0]); else { W("Links: %s", D->link[0]); for (int i = 1; i < nl; i++) W("; %s", D->link[i]); W("\n"); } }
    }
    mark[3] = (int)o;
    if (D->nlim <= 3) { W("Not found / limits: "); for (int i = 0; i < D->nlim; i++) W("%s%s", i ? "; " : "", D->lim[i]); W("\n"); }
    else { W("Not found / limits:\n"); for (int i = 0; i < D->nlim; i++) W("- %s\n", D->lim[i]); }
    mark[4] = (int)o;
    if (cx->tool_form) W("Next: %s", next); else { W("Next: %s\n", next); W("[end of context]"); }
    mark[5] = (int)o;
#undef W
    if (st) { st->tok_head = (mark[0] * 10) / g_uioe_target_qwen.bpt10; st->tok_path = ((mark[1] - mark[0]) * 10) / g_uioe_target_qwen.bpt10; st->tok_evid = ((mark[2] - mark[1]) * 10) / g_uioe_target_qwen.bpt10; st->tok_links = ((mark[3] - mark[2]) * 10) / g_uioe_target_qwen.bpt10; st->tok_lim = ((mark[4] - mark[3]) * 10) / g_uioe_target_qwen.bpt10; st->tok_next = ((mark[5] - mark[4]) * 10) / g_uioe_target_qwen.bpt10; }
    return o;
}

/* o ponto de entrada (3.9). limits_form: so Question, Not found / limits e Next (G5). Devolve os bytes escritos, ou 0 com st->why */
static size_t ctxb_build(const UioeCtx *cx, const UioeRequest *r, const UioeTarget *t, int limits_form, char *out, size_t cap, UioePackStats *st) {
    CbDoc *D = (CbDoc *)calloc(1, sizeof *D); if (!D) { snprintf(st->why, sizeof st->why, "out of memory"); return 0; }
    size_t n = 0; char question[700], next[700]; int ceiling = r->route.token_ceiling > 0 ? r->route.token_ceiling : t->tokens_max;
    if (ceiling > t->tokens_max) { ceiling = t->tokens_max; }
    if (cx->token_ceiling < ceiling) ceiling = cx->token_ceiling;
    if (limits_form) ceiling = ceiling < 300 ? ceiling : 300;      /* o formulario de limites e curto (4.6.4: cerca de 120 tokens; aqui ate 300) */
    const size_t maxb = (size_t)ceiling * (size_t)t->bpt10 / 10;
    if (!limits_form) cxb_select(cx, r, t, D, st);
    if (!limits_form && !D->nstep && !D->nev && !D->nmem) { snprintf(st->why, sizeof st->why, "nothing verified to say: no path, no finding, no evidence"); free(D); return 0; }
    if (!limits_form && D->nstep && !strncmp(D->step[0].mark, "(guess)", 7)) { snprintf(st->why, sizeof st->why, "the first step is a guess (never step 1)"); free(D); return 0; }
    cxb_question(cx, r, question, sizeof question);
    if (cx->tool_form && r->ask.nterms <= 1 && r->ask.nent == 1 && r->ask.ent[0].kind == UK_SYMBOL) question[0] = 0;      /* 4.6.6: identificador, sem a linha de entendimento */
    cxb_limits(cx, r, t, D); cxb_next(cx, r, D, limits_form, next, sizeof next);
    if (limits_form && D->nlim > 3) D->nlim = 3;      /* escopo primeiro; o resto fica no traco */
    /* U8: a memoria usa ate 10% do teto (no minimo um bom trecho) quando ha codigo; num pedido de historico ela e a resposta e nao tem esse limite */
    const int memonly = !D->nstep && !D->nev;
    if (!memonly) { size_t share = maxb / 10; if (share < 260) share = 260; size_t used = 0; int keep = 0;
        for (int i = 0; i < D->nmem; i++) { used += strlen(D->mem[i].loc) + strlen(D->mem[i].txt) + 6; if (used > share) break; keep++; }
        for (int i = keep; i < D->nmem; i++) { size_t l = strlen(D->leftout); if (l + strlen(D->mem[i].loc) + 4 < sizeof D->leftout) snprintf(D->leftout + l, sizeof D->leftout - l, "%s%.60s", l ? ", " : "", D->mem[i].loc); }
        D->nmem = keep; }
    if (!limits_form && D->leftout[0]) { D->nlim = 0; cxb_limits(cx, r, t, D); }
    int nev = D->nev, nlink = D->nlink, quote_upto = 8, cut_ev = 0; char log[1800] = "";
    n = cxb_render(cx, r, D, limits_form, st->findings, nev, nlink, quote_upto, question, next, out, cap, st);
    /* B5: corte na ordem de 4.6.3 (evidencia de baixo para cima, ligacoes alem das tres primeiras, citacoes dos passos 3 a 5); nunca um passo, uma clausula ou uma marca */
    while (n > maxb && !limits_form) {
        if (D->nmem > (memonly ? 1 : 0)) { D->nmem--; const CbLine *M = &D->mem[D->nmem]; size_t l = strlen(D->leftout); if (l + strlen(M->loc) + 4 < sizeof D->leftout) snprintf(D->leftout + l, sizeof D->leftout - l, "%s%.60s", l ? ", " : "", M->loc); }
        else if (nev > 0) { nev--; cut_ev++; const CbLine *L = &D->ev[nev]; size_t l = strlen(D->leftout); if (l + strlen(L->loc) + 4 < sizeof D->leftout) snprintf(D->leftout + l, sizeof D->leftout - l, "%s%s", l ? ", " : "", L->loc); }
        else if (D->tests[0]) D->tests[0] = 0;
        else if (D->already[0]) D->already[0] = 0;
        else if (nlink > 3) nlink--;
        else if (quote_upto > 2) quote_upto = 2;
        else if (nlink > 0) nlink--;
        else break;
        D->nlim = 0; cxb_limits(cx, r, t, D);
        n = cxb_render(cx, r, D, limits_form, st->findings, nev, nlink, quote_upto, question, next, out, cap, st);
    }
    if (n > maxb) { snprintf(st->why, sizeof st->why, "does not fit in %d tokens even after trimming (%d)", ceiling, (int)(n * 10 / (size_t)t->bpt10)); free(D); out[0] = 0; return 0; }
    st->bytes = (int)n; st->tokens = (int)(n * 10 / (size_t)t->bpt10); st->nsteps = D->nstep; st->nevid = nev + D->nmem + (D->tests[0] ? 1 : 0); st->nlinks = nlink; st->nlimits = D->nlim; st->cut = cut_ev;
    for (int i = 0; i < nev && st->nshown < 80; i++) st->shown[st->nshown++] = D->ev[i].id;
    { size_t l = 0; l += (size_t)snprintf(log + l, sizeof log - l, "in: %d path/finding step(s), %d evidence line(s), %d link(s), %d limit clause(s)", D->nstep, nev, nlink, D->nlim);
      if (st->ded_req) l += (size_t)snprintf(log + l, sizeof log - l, "; out: %d item(s) the request already names", st->ded_req);
      if (st->ded_state) l += (size_t)snprintf(log + l, sizeof log - l, "; %d item(s) already shown or read in this request (not quoted again)", st->ded_state);
      if (st->ded_self) l += (size_t)snprintf(log + l, sizeof log - l, "; %d duplicate link(s) merged", st->ded_self);
      if (cut_ev || quote_upto < 8 || nlink < D->nlink) l += (size_t)snprintf(log + l, sizeof log - l, "; cut for the %d-token ceiling: %d evidence line(s)%s%s", ceiling, cut_ev, nlink < D->nlink ? ", links beyond the first" : "", quote_upto < 8 ? ", quotes of steps 3 to 5" : "");
      snprintf(st->log, sizeof st->log, "%s", log); }
    free(D); return n;
}
