/* ---------- uioe/facade.c: Find e Symbols como FACHADAS do motor do UIOE (docs/uioe.md 3.10.3, 4.6.6 e 6.1 fase U5) ----------
 * Com KATANA_UIOE=full, a consulta que o modelo faz ao Find (modo padrao) passa pelo MESMO codigo do bloco antes do modelo: interpretar, classificar,
 * roteirizar, recuperar e orquestrar (U1 e U3) e o Construtor de Contexto (ctxb.c), com a mesma confianca, os mesmos caminhos, as mesmas marcas e o
 * mesmo formato de ligacoes e de `Not found / limits`. A diferenca e so a forma (4.6.6): sem a linha de abertura nem o fecho e, quando a consulta e um
 * identificador, sem a linha `Question understood as`. Nao ha segunda logica: a unica coisa daqui e a cola (sessao, relogio, estado do pedido, traco).
 *
 * Estado do pedido compartilhado: o que o bloco mostrou ja esta em rs_* (uioe_commit), e o orquestrador marca esses nos como `shown`; o Construtor os
 * tira do bloco ("already shown to you, not repeated"). O que a fachada mostra tambem vai para rs_note: um Find seguinte nao repete.
 *
 * Contrato com find.c (a ligacao fina, duas linhas, feita no merge):
 *   - ct_findtool, no ramo da busca padrao (nem refs nem neighbors), depois de resolvidos `root` e `pre`, antes do indice proprio:
 *         if (uioe_facade_find(q, root, pre, (int)lim, out, cap)) goto done;
 *     devolve 1 = respondeu (out preenchido), 0 = nada a dizer ou modo diferente de `full`: o Find segue como sempre. Nao muda a assinatura.
 *   - ct_symbols, depois de sym_tool_run: uioe_facade_symbols_trace(action, name, file, out). Symbols mantem o seu formato byte a byte (4.6.6).
 * Casos em que devolve 0 de proposito (o Find de hoje responde, com o seu proprio NOT FOUND e limites): modo diferente de full; `pre` nao vazio (o filtro
 * por subpasta ainda nao existe na recuperacao); dominio diferente de codigo e arquivos; recuperacao que nao rodou; decisao diferente de G4 (pacote);
 * nada verificado para dizer; erro interno. Sem porta de confianca: o modelo pediu, entao um pacote com apoio verificado basta. */
static UioeSession *uioe_session_new(void);      /* cli.c, mais adiante */

static UioeSession *uf_session(void) { if (!g_uioe_tool_sess) g_uioe_tool_sess = uioe_session_new(); return g_uioe_tool_sess; }

static int uioe_facade_find(const char *query, const char *root, const char *pre, int lim, char *out, size_t cap) {
    (void)lim;
    if (uioe_mode() != UIOE_FULL || !query || !*query || (pre && *pre) || cap < 400) return 0;
    UioeSession *s = uf_session(); if (!s) return 0;
    UioeCtx cx; uioe_ctx_init(&cx, s, root); cx.record = 0; cx.tool_form = 1;
    cx.deadline_ms = uioe_env_int("KATANA_UIOE_TOOL_MS", 1500, 20, 20000);
    const double T0 = now_sec(); int ok = 0; char why[300] = "";
    char *block = (char *)malloc(16384); UioeRequest *r = s->cur; UioePackStats st; memset(&st, 0, sizeof st);
    if (!block || !r) { free(block); uioe_ctx_close(&cx); return 0; }
    block[0] = 0;
    char text[2000]; snprintf(text, sizeof text, "%.1999s", query);
    r->id = uioe_new_id(s);
    uioe_interpret(&cx, text, &r->ask);
    uioe_classify(&cx, &r->ask, &r->cls);
    snprintf(cx.root, sizeof cx.root, "%s", r->ask.root);
    uioe_route(&cx, &r->ask, &r->cls, &r->route);
    uioe_orchestrate(&cx, &r->ask, &r->cls, &r->route, &r->rc);
    const UioeRecall *rc = &r->rc;
    if (!rc->ran || !rc->g) snprintf(why, sizeof why, "retrieval did not run (%s)", rc->decision_note);
    else if (r->cls.domain[0] != UD_CODE && r->cls.domain[0] != UD_FILES) snprintf(why, sizeof why, "domain %s has no connector in this build", g_uioe_domname[r->cls.domain[0]]);
    else if (rc->decision != UG_PACKET) snprintf(why, sizeof why, "not G4 (%s): the Find of today answers", rc->decision_note);
    else {
        const size_t n = ctxb_build(&cx, r, &g_uioe_target_qwen, 0, block, 16384, &st);
        if (!n) snprintf(why, sizeof why, "the Context Builder made nothing: %s", st.why);
        else if (n + 1 >= cap) snprintf(why, sizeof why, "the result (%d bytes) does not fit the output buffer", (int)n);
        else {
            memcpy(out, block, n + 1); ok = 1;
            const UioeGraph *g = rc->g;      /* o que a fachada mostrou conta como mostrado: o proximo Find e o Read deste pedido nao o repetem */
            for (int i = 0; i < st.nshown; i++) {
                const UioeNode *nd = &g->n[st.shown[i]]; if (!nd->path[0] || nd->kind == UN_FOLDER) continue;
                const int a = nd->line0 > 0 ? nd->line0 : 1, b = nd->line1 >= a ? nd->line1 : a; char ap[1500];
                snprintf(ap, sizeof ap, "%s/%s", cx.root, nd->path); rs_note(ap, a, b, 1);
            }
        }
    }
    if (cx.trace) {
        UioeBuf t = {0}; uioe_fmt_trace(&t, &cx, r); uioe_fmt_recall_trace(&t, &cx, r);
        uioe_bprintf(&t, "facade  Find %s: %s | %d bytes, about %d tokens | %.1f ms\n\n", ok ? "ANSWERED by the engine" : "left to the Find of today", ok ? "same engine as the block" : why, ok ? st.bytes : 0, ok ? st.tokens : 0, (now_sec() - T0) * 1000.0);
        if (t.p) uioe_trace_write(t.p); uioe_bfree(&t);
    }
    free(block); uioe_ctx_close(&cx);
    return ok;
}

/* Symbols: o resultado nao muda; so o traco (uma linha por chamada) para que o `katana uioe` mostre o que o modelo consultou */
static void uioe_facade_symbols_trace(const char *action, const char *name, const char *file, const char *result) {
    if (uioe_mode() != UIOE_FULL || !result) return;
    UioeBuf t = {0};
    uioe_bprintf(&t, "facade  Symbols action=%s name=\"%.80s\" file=\"%.120s\" -> %d bytes%s\n\n", action ? action : "", name ? name : "", file ? file : "", (int)strlen(result), strstr(result, "NOT FOUND") ? " (NOT FOUND)" : "");
    if (t.p) uioe_trace_write(t.p); uioe_bfree(&t);
}
