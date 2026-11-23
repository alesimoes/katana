/* ---------- uioe/api.c: a porta antes do modelo, `uioe_prepare` (docs/uioe.md 3.2, 3.10.1 e 6.1 fase U4) ----------
 * `uioe_prepare(sessao, pedido)`: interpretar, classificar, roteirizar (U1), recuperar e orquestrar (U3), a PORTA conservadora e o Construtor de Contexto
 * (ctxb.c), com um prazo total curto. Devolve 1 e o bloco, ou 0 e nada: o pedido segue ao modelo exatamente como o usuario o escreveu e o laco faz o que
 * fazia (nota de pasta, selecao de documentos). Erro interno, prazo estourado e duvida viram 0 e uma linha no traco; nunca falha para o laco.
 *
 * A PORTA (decisao do dono, 2026-11-23: o modelo ainda nao foi treinado para o bloco, entao um bloco ruim e pior que nenhum). Todas as condicoes
 * valem ao mesmo tempo, e a PRIMEIRA que falha e o motivo gravado:
 *   1. a recuperacao rodou (nada de conversa, "bom dia", pedido curto sem investigacao herdada, pedido ambiguo: a classificacao ja manda `retrieve 0`);
 *   2. dominio codigo ou arquivos, ou o tipo history (U8: servido pela memoria do dono, memory.recall); os outros dominios ainda nao tem conector;
 *   3. a decisao de U3 foi G4 (confianca depois de recuperar >= KATANA_UIOE_GATE, padrao 70) E a confianca e >= UIOE_PACK_MIN_CONF;
 *   4. ha apoio verificado: um caminho de dois passos relidos, ou um item de faixa 1 ou 2 relido com confianca >= 75;
 *   5. o pedido nao traz so o caminho exato de um arquivo (nada a procurar);
 *   6. nenhum conector estourou o prazo nem falhou e o prazo do pedido nao cortou a recuperacao;
 *   7. o tempo total (interpretar a construir) coube em KATANA_UIOE_TOTAL_MS (padrao 1500; o alvo e 400);
 *   8. o Construtor montou um bloco que cabe no teto de tokens, com um primeiro passo verificado.
 * O formulario de limites (G5: so `Question understood as`, `Not found / limits`, `Next`) existe no Construtor e e mostrado por `katana uioe ask --pack`,
 * mas o laco so o injeta com KATANA_UIOE_LIMITS=1: a nota de pasta citada ja cobre o caso mais comum e um aviso de "indice parcial" ensina pouco.
 * Os valores abaixo sao PROVISORIOS e nao calibrados. */
static void uioe_ctx_init(UioeCtx *c, UioeSession *s, const char *cwd_override);      /* cli.c, mais adiante */
static void uioe_ctx_close(UioeCtx *c);
#define UIOE_PACK_MIN_CONF 80           /* confianca depois de recuperar: acima do G4 (70) de U3 de proposito; comecar alto e baixar com o uso */
#define UIOE_INHERIT_TURNS 4            /* quantos pedidos sem bloco novo a investigacao aberta sobrevive */
#define UIOE_TOTAL_MS_DEFAULT 1500      /* teto do tempo total de uioe_prepare; o prazo da recuperacao (400 ms) e menor e esta em UioeCtx */

typedef struct {
    int injected;                       /* 1 = ha bloco */
    int limits_form;                    /* o bloco e o formulario de limites */
    int tokens, bytes;
    int ran;                            /* a recuperacao rodou */
    int rule, conf;                     /* regra G e confianca depois de recuperar */
    int inherited;                      /* o pedido herdou a investigacao aberta */
    int overtime;                       /* o prazo total estourou */
    double ms;                          /* tempo total */
    char why[520];                      /* por que (nao) injetou, a regra da porta */
    uint64_t id;
    UioePackStats st;
} UioePrep;

/* o pedido so se refere ao anterior? ("e onde isso chega?", "e o outro?", "and where does that go?"): sem entidade propria e com uma palavra de referencia */
static int up_followup(const UioeAsk *a, const char *text) {
    if (strlen(text) > 160) return 0;
    for (int i = 0; i < a->nent; i++) { const int k = a->ent[i].kind; if (k == UK_SYMBOL || k == UK_PATH || k == UK_FOLDER || k == UK_LITERAL || k == UK_ERRTEXT || k == UK_KEY || k == UK_ENV || k == UK_URL) return 0; }
    static const char *const ref[] = {"isso", "esse", "essa", "esses", "essas", "disso", "nisso", "desse", "dessa", "aquele", "aquela", "outro", "outra", "outros", "outras", "ele", "ela", "la", "entao", "tambem",
                                      "that", "this", "it", "those", "them", "other", "another", "there", "also", "then", NULL};
    char w[40]; int wl = 0;
    for (const char *p = text;; p++) {
        const int c = (unsigned char)*p;
        if (isalpha(c)) { if (wl < 39) w[wl++] = (char)tolower(c); continue; }
        if (wl) { w[wl] = 0; for (int i = 0; ref[i]; i++) if (!strcmp(w, ref[i])) return 1; wl = 0; }
        if (!c) break;
    }
    return 0;
}
static void up_note_shown(UioeSession *s, const UioeCtx *cx, const UioeGraph *g, const UioePackStats *st) {      /* o que o bloco mostrou: o estado do pedido (depois do rr_reset) e a investigacao aberta */
    s->npend = 0; s->inv.nsh = 0;
    for (int i = 0; i < st->nshown; i++) {
        const UioeNode *n = &g->n[st->shown[i]]; if (!n->path[0] || n->kind == UN_FOLDER) continue;
        const int a = n->line0 > 0 ? n->line0 : 1, b = n->line1 >= a ? n->line1 : a;
        if (s->npend < 64) { snprintf(s->pend[s->npend].path, sizeof s->pend[0].path, "%s/%s", cx->root, n->path); s->pend[s->npend].a = a; s->pend[s->npend].b = b; s->npend++; }
        if (s->inv.nsh < 48) { snprintf(s->inv.sh[s->inv.nsh].path, sizeof s->inv.sh[0].path, "%s", n->path); s->inv.sh[s->inv.nsh].a = a; s->inv.sh[s->inv.nsh].b = b; s->inv.nsh++; }
    }
}
/* o laco chama depois do rr_reset: o que o bloco mostrou conta como mostrado para o Find e o Read deste pedido */
static void uioe_commit(UioeSession *s) { for (int i = 0; i < s->npend; i++) rs_note(s->pend[i].path, s->pend[i].a, s->pend[i].b, 1); s->npend = 0; }
static void uioe_inv_reset(UioeSession *s) { if (s) { memset(&s->inv, 0, sizeof s->inv); s->npend = 0; } }

/* as linhas que o seletor de documentos do projeto escolheu entram como Evidence (3.10.2: o docsel vira o conector docs.select) */
static int up_add_docsel(UioeCtx *cx, const char *request, UioeRecall *rc, int front) {
    if (!cx->tk) return 0;
    char *out = (char *)malloc(8200); if (!out) return 0;
    const int n = ds_select(cx->tk, request, br_cur(), cx->root, out, 8200);
    int added = 0; UioeGraph *g = rc->g;
    if (n > 0) {
        const int conn = uioe_find_conn(cx->s, "docs.select"); int ids[8]; int ni = 0;
        for (char *p = strstr(out, "\n== "); p && ni < 4; p = strstr(p + 1, "\n== ")) {
            char path[400]; int a = 0, b = 0; if (sscanf(p + 4, "%399[^:]:%d-%d", path, &a, &b) < 3) continue;
            UioeNode nd; memset(&nd, 0, sizeof nd); nd.kind = UN_DOCLINE; snprintf(nd.path, sizeof nd.path, "%s", path); nd.line0 = a; nd.line1 = b; snprintf(nd.name, sizeof nd.name, "selected by docsel");
            const char *t = strchr(p + 4, '\n'); if (t) { t++; char tmp[200]; snprintf(tmp, sizeof tmp, "%.190s", t); char *e = strchr(tmp, '\n'); if (e) *e = 0; snprintf(nd.text, sizeof nd.text, "%s", tmp); snprintf(nd.vkey, sizeof nd.vkey, "%.40s", tmp); }
            nd.conf = 70; nd.step = 1; nd.tier = 3; nd.ver = UV_OK; nd.cver = 1; nd.relevance = 60; snprintf(nd.how, sizeof nd.how, "picked by the docsel selector from the documents KATANA.md points to");
            const int id = ug_add(g, &nd, conn, ni + 1, "docs.select"); if (id >= 0) { g->n[id].ver = UV_OK; ids[ni++] = id; }
        }
        if (ni && rc->nitems + ni < 96) {
            if (front) { memmove(rc->items + ni, rc->items, sizeof(int) * (size_t)rc->nitems); memcpy(rc->items, ids, sizeof(int) * (size_t)ni); } else memcpy(rc->items + rc->nitems, ids, sizeof(int) * (size_t)ni);
            rc->nitems += ni; added = ni;
        }
    }
    free(out); return added;
}

/* a porta: 0 nada, 1 bloco, 2 formulario de limites. `why` recebe a regra */
static int up_gate(const UioeCtx *cx, const UioeRequest *r, char *why, size_t cap) {
    const UioeRecall *rc = &r->rc; const UioeClass *k = &r->cls; const UioeGraph *g = rc->g; const UioeAsk *a = &r->ask;
#define NO(...) do { snprintf(why, cap, __VA_ARGS__); return 0; } while (0)
    if (!rc->ran || !g) NO("door 1: no retrieval ran (%s)", rc->decision_note[0] ? rc->decision_note : r->route.decision_note);
    const int memreq = k->domain[0] == UD_MEMORY && k->kind == UI_HISTORY;      /* U8: o pedido de historico e servido pela memoria do dono (memory.recall) */
    if (k->domain[0] != UD_CODE && k->domain[0] != UD_FILES && !memreq) NO("door 2: domain %s has no connector in this build; only code and files are served", g_uioe_domname[k->domain[0]]);
    if (a->ambiguous) NO("door 1: the request is ambiguous (%s)", a->amb);
    if (k->retrieve != 1) NO("door 1: retrieve = %d (a cited place only: the place note says it)", k->retrieve);
    if (rc->decision == UG_LIMITS) { const char *e = getenv("KATANA_UIOE_LIMITS"); snprintf(why, cap, "door 3: G5, the limits form (%s)%s", rc->decision_note, e && atoi(e) ? "" : "; not injected by default (KATANA_UIOE_LIMITS=1 injects it)"); return e && atoi(e) ? 2 : 0; }
    if (rc->decision != UG_PACKET) NO("door 3: not G4 (%s)", rc->decision_note);
    if (rc->conf_after < UIOE_PACK_MIN_CONF && !memreq) NO("door 3: confidence %d after retrieval is below the pack threshold %d (UIOE_PACK_MIN_CONF)", rc->conf_after, UIOE_PACK_MIN_CONF);
    int sup = 0;
    if (rc->npath >= 2) { const UioeNode *A = &g->n[rc->path[0]], *B = &g->n[rc->path[1]]; if (cxb_ok(A) && cxb_ok(B)) sup = 1; }
    for (int i = 0; i < rc->nitems && !sup; i++) { const UioeNode *n = &g->n[rc->items[i]]; if (cxb_ok(n) && n->tier <= 2 && n->conf >= 75 && n->kind != UN_FILE && n->kind != UN_FOLDER) sup = 1; }
    if (memreq) for (int i = 0; i < rc->nitems && !sup; i++) if (g->n[rc->items[i]].kind == UN_FACT) sup = 1;      /* U8: a memoria nunca e "verificada": basta uma nota salva que casou (rotulada (guess) no bloco) */
    if (!sup) NO("door 4: no verified support (a reread path of two steps, or a reread tier-1/2 item with confidence >= 75)");
    int named = 0, other = 0; for (int i = 0; i < a->nent; i++) { const int e = a->ent[i].kind; if (e == UK_PATH && a->ent[i].resolved[0]) named++; else if (e == UK_SYMBOL || e == UK_KEY || e == UK_ENV || e == UK_LITERAL || e == UK_ERRTEXT) other++; }
    if (named && !other && (k->kind == UI_LOCATE || k->kind == UI_HOWTO || k->kind == UI_OVERVIEW || k->kind == UI_CONFIG)) NO("door 5: the request already names the exact file; there is nothing to look for");
    if (a->inherits) {      /* P2 RECALL (3.7.1): a investigacao herdada so recebe bloco se ha algo NOVO no caminho; o que ja foi mostrado nao se manda de novo */
        int fresh = 0;
        if (rc->npath >= 2) { for (int i = 0; i < rc->npath; i++) { const UioeNode *n = &g->n[rc->path[i]]; if (!n->shown && !n->read) fresh++; } }
        else for (int i = 0; i < rc->nitems; i++) { const UioeNode *n = &g->n[rc->items[i]]; if (n->tier <= 2 && n->conf >= 75 && cxb_ok(n) && !n->shown && !n->read) fresh++; }
        if (!fresh) NO("door 4: inherited the open investigation (trace: continues the previous request), but everything the path holds was already shown: nothing new to inject");
    }
    if (rc->cut) NO("door 6: the request deadline cut the retrieval (%s)", rc->cut_note);
    for (int i = 0; i < rc->nex; i++) if (rc->ex[i].status == UR_TIMEOUT || rc->ex[i].status == UR_ERROR) NO("door 6: connector %s %s (%s): a degraded retrieval is not injected", rc->ex[i].conn, rc->ex[i].status == UR_TIMEOUT ? "timed out" : "failed", rc->ex[i].label);
    (void)cx;
#undef NO
    return 1;
}

/* o traco: o registro do pedido, a decisao da porta e o bloco (linhas `block | ...`) */
static void up_trace(UioeCtx *cx, const UioeRequest *r, const UioePrep *pp, const char *inherit_note, const char *block) {
    if (!cx->trace) return;
    UioeBuf t = {0}; uioe_fmt_trace(&t, cx, r); uioe_fmt_recall_trace(&t, cx, r);
    if (inherit_note[0]) uioe_bprintf(&t, "inherit %s\n", inherit_note);
    uioe_bprintf(&t, "door    %s: %s | total %.1f ms (deadline %d ms, total %d ms)\n", pp->injected ? (pp->limits_form ? "INJECTED (limits form)" : "INJECTED") : "not injected", pp->why, pp->ms, cx->deadline_ms, uioe_env_int("KATANA_UIOE_TOTAL_MS", UIOE_TOTAL_MS_DEFAULT, 100, 20000));
    if (pp->injected) {
        const UioePackStats *st = &pp->st;
        uioe_bprintf(&t, "pack    %d bytes, about %d tokens (4 bytes per token; ceiling %d): head %d, path %d, evidence %d, links %d, limits %d, next %d token(s)\n", st->bytes, st->tokens, r->route.token_ceiling, st->tok_head, st->tok_path, st->tok_evid, st->tok_links, st->tok_lim, st->tok_next);
        uioe_bprintf(&t, "pack    %s\n", st->log);
        for (const char *p = block; p && *p; ) { const char *e = strchr(p, '\n'); const size_t l = e ? (size_t)(e - p) : strlen(p); uioe_bprintf(&t, "block   | %.*s\n", (int)l, p); p = e ? e + 1 : NULL; }
    }
    uioe_bprintf(&t, "\n"); if (t.p) uioe_trace_write(t.p); uioe_bfree(&t);
}

/* o corpo: tudo depois de montar o UioeCtx (a CLI define o cwd; o laco usa o da sessao) */
static int uioe_pack_core(UioeCtx *cx, const char *text, char *block, size_t cap, UioePrep *pp) {
    UioeSession *s = cx->s; UioeRequest *r = s->cur; const double T0 = now_sec(); block[0] = 0; memset(pp, 0, sizeof *pp);
    r->id = uioe_new_id(s); pp->id = r->id;
    uioe_interpret(cx, text, &r->ask);
    char inherit_note[300] = "", comp[2300];
    if (s->inv.open && s->inv.turns <= UIOE_INHERIT_TURNS && up_followup(&r->ask, text)) {      /* 3.6.2: continua a investigacao aberta */
        snprintf(comp, sizeof comp, "%.1900s %.380s", s->inv.text, text);
        uioe_interpret(cx, comp, &r->ask); r->ask.inherits = 1; pp->inherited = 1;
        snprintf(inherit_note, sizeof inherit_note, "inherited from request %04llx (%d request(s) ago): \"%.120s\"", (unsigned long long)s->inv.id, s->inv.turns, s->inv.text);
    }
    uioe_classify(cx, &r->ask, &r->cls);
    snprintf(cx->root, sizeof cx->root, "%s", r->ask.root);
    uioe_route(cx, &r->ask, &r->cls, &r->route);
    uioe_orchestrate(cx, &r->ask, &r->cls, &r->route, &r->rc);
    UioeRecall *rc = &r->rc; pp->ran = rc->ran; pp->rule = rc->rule; pp->conf = rc->conf_after;
    if (pp->inherited && rc->g) for (int i = 0; i < rc->g->nn; i++) { UioeNode *n = &rc->g->n[i]; for (int j = 0; j < s->inv.nsh; j++) if (!strcmp(n->path, s->inv.sh[j].path) && n->line0 >= s->inv.sh[j].a && n->line0 <= s->inv.sh[j].b) n->shown = 1; }      /* o que o bloco anterior ja mostrou */
    int gate = up_gate(cx, r, pp->why, sizeof pp->why);
    int bytes = 0;
    if (gate) {
        if (gate == 1 && r->cls.kind != UI_HISTORY) { const int front = r->cls.kind == UI_HOWTO || r->cls.kind == UI_OVERVIEW || r->cls.kind == UI_CONFIG; if (up_add_docsel(cx, text, rc, front)) snprintf(pp->st.log, sizeof pp->st.log, "docs: the document lines the project documents selected were added"); }
        const size_t n = ctxb_build(cx, r, &g_uioe_target_qwen, gate == 2, block, cap, &pp->st);
        bytes = (int)n;
        if (!n) { snprintf(pp->why, sizeof pp->why, "door 8: the Context Builder made no block: %s", pp->st.why); gate = 0; block[0] = 0; }
        else { pp->bytes = pp->st.bytes; pp->tokens = pp->st.tokens; pp->limits_form = gate == 2; }
    }
    pp->ms = (now_sec() - T0) * 1000.0;
    const int totalms = uioe_env_int("KATANA_UIOE_TOTAL_MS", UIOE_TOTAL_MS_DEFAULT, 100, 20000);
    if (gate && !cx->det && pp->ms > totalms) { snprintf(pp->why, sizeof pp->why, "door 7: the total time %.0f ms passed the limit of %d ms (KATANA_UIOE_TOTAL_MS): nothing injected", pp->ms, totalms); pp->overtime = 1; gate = 0; block[0] = 0; bytes = 0; }
    if (gate) { pp->injected = 1; snprintf(pp->why, sizeof pp->why, "all doors passed: %s (confidence %d after retrieval, threshold %d)", rc->decision_note, rc->conf_after, UIOE_PACK_MIN_CONF); up_note_shown(s, cx, rc->g, &pp->st); }
    /* a investigacao aberta: um bloco abre ou continua; um pedido sem bloco e que muda de assunto fecha; o resto so envelhece */
    if (pp->injected) { s->inv.open = 1; s->inv.turns = 0; s->inv.id = r->id; if (!pp->inherited) { snprintf(s->inv.text, sizeof s->inv.text, "%s", text); snprintf(s->inv.root, sizeof s->inv.root, "%s", r->ask.root); } }
    else if (s->inv.open) { if (!pp->inherited && r->cls.retrieve == 1) s->inv.open = 0; else if (++s->inv.turns > UIOE_INHERIT_TURNS) s->inv.open = 0; }
    pp->st.nshown = pp->st.nshown;
    up_trace(cx, r, pp, inherit_note, block);
    (void)bytes;
    return pp->injected;
}

/* a entrada do laco (3.2): o pedido ORIGINAL, antes de qualquer nota. 0 = nada a injetar. Nunca falha: erro vira 0. tk: o tokenizador do modelo (so o docsel precisa) */
static int uioe_prepare(UioeSession *s, const Tokenizer *tk, const char *request, char *block, size_t cap, UioePrep *pp) {
    if (uioe_mode() == UIOE_OFF || !s || !request || !*request) { if (pp) memset(pp, 0, sizeof *pp); if (block && cap) block[0] = 0; return 0; }
    UioePrep local; if (!pp) pp = &local;
    UioeCtx cx; uioe_ctx_init(&cx, s, NULL); cx.tk = tk; cx.record = 0;
    char text[2000]; snprintf(text, sizeof text, "%.1999s", request);
    const int ok = uioe_pack_core(&cx, text, block, cap, pp);
    uioe_ctx_close(&cx);
    return ok;
}
