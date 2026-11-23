/* ---------- uioe/budget.c: prazos, disjuntor, escada de degradacao e a chamada de um conector (docs/uioe.md 3.5.2, 3.7, fase U3) ----------
 * `uioe_call` e o UNICO lugar que executa um conector: confere se ele existe e esta pronto neste escopo, o disjuntor da sessao, o prazo do
 * pedido, roda, aplica a tentativa unica (so quando o resultado e UR_TIMEOUT COM dados e sobra tempo, com metade do que resta), conta falha
 * ou sucesso no disjuntor, anota a chamada no registro do pedido e funde os nos e arestas no grafo. Um conector que falha nunca derruba o
 * pedido: o resultado diz o que faltou e o pipeline segue com o que tem (3.5.2).
 * Decisoes do implementador:
 *   - os conectores de dentro do processo nao podem ser interrompidos; o prazo de uma chamada e conferido DEPOIS dela: passou do limite
 *     (4x o `default_ms` para os de indice e instantaneos, 1x para os demais) = UR_TIMEOUT que conta no disjuntor, e os dados que ela trouxe
 *     ficam. O prazo do PEDIDO (KATANA_UIOE_MS) e conferido ANTES de cada chamada: estourou = nao chama e o pipeline salta para as lacunas.
 *   - disjuntor: 2 falhas seguidas (erro ou timeout) abrem por 5 pedidos da sessao; depois fica meio-aberto (uma tentativa com metade do prazo).
 *   - KATANA_UIOE_FAULT="conector=timeout|error|partial,..." e um gancho de TESTE da escada de degradacao: timeout/error devolvem esse
 *     resultado sem rodar o conector; partial roda o conector, devolve os dados como UR_TIMEOUT e a tentativa unica roda de verdade.
 *   - KATANA_UIOE_DET=1: sem corte por tempo (para repeticao e curriculo). */
static int uioe_find_conn(const UioeSession *s, const char *id);
static double uioe_elapsed_ms(const UioeCtx *cx) { return (now_sec() - cx->t0) * 1000.0; }
static int uioe_over(const UioeCtx *cx) { return !cx->det && uioe_elapsed_ms(cx) > cx->deadline_ms; }
static int uioe_left_ms(const UioeCtx *cx) { const int l = cx->deadline_ms - (int)uioe_elapsed_ms(cx); return l < 0 ? 0 : l; }

static void rc_vadd(char (*arr)[230], int *n, int max, const char *fmt, va_list ap) {
    char t[230]; vsnprintf(t, sizeof t, fmt, ap);
    for (int i = 0; i < *n; i++) if (!strcmp(arr[i], t)) return;
    if (*n < max) snprintf(arr[(*n)++], 230, "%s", t);
}
static void rc_limit(UioeRecall *rc, const char *fmt, ...) { va_list ap; va_start(ap, fmt); rc_vadd(rc->limits, &rc->nlim, 20, fmt, ap); va_end(ap); }
static void rc_hyp(UioeRecall *rc, const char *fmt, ...) {
    char t[200]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    for (int i = 0; i < rc->nhyp; i++) if (!strcmp(rc->hyp[i], t)) return;
    if (rc->nhyp < 8) snprintf(rc->hyp[rc->nhyp++], 200, "%s", t);
}
static void rc_degrade(UioeRecall *rc, const char *fmt, ...) {
    char t[170]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    for (int i = 0; i < rc->ndeg; i++) if (!strcmp(rc->degrade[i], t)) return;
    if (rc->ndeg < 8) snprintf(rc->degrade[rc->ndeg++], 170, "%s", t);
}
static void rc_state(UioeRecall *rc, const char *fmt, ...) {
    if (rc->nstates >= 16) return; va_list ap; va_start(ap, fmt); vsnprintf(rc->states[rc->nstates++], 120, fmt, ap); va_end(ap);
}

/* novo pedido na sessao: os disjuntores abertos contam um pedido a menos; o que chega a zero fica meio-aberto */
static void uioe_breaker_begin(UioeSession *s) {
    for (int i = 0; i < s->nreg; i++) { UioeBreaker *b = &s->brk[i]; if (b->open && --b->skip_left <= 0) { b->open = 0; b->fails = 1; b->skip_left = 0; } }
}
static void uioe_breaker_note(UioeSession *s, int ix, int status) {
    UioeBreaker *b = &s->brk[ix];
    if (status == UR_TIMEOUT || status == UR_ERROR) { if (++b->fails >= 2) { b->open = 1; b->skip_left = 5; } }
    else if (status == UR_OK || status == UR_PARTIAL || status == UR_EMPTY) b->fails = 0;
}
static const char *uioe_fault_of(const UioeCtx *cx, const char *id) {
    static char out[16]; const char *p = cx->fault; const size_t il = strlen(id);
    while (p && *p) {
        if (!strncmp(p, id, il) && p[il] == '=') { const char *v = p + il + 1; size_t l = strcspn(v, ","); if (l >= sizeof out) l = sizeof out - 1; memcpy(out, v, l); out[l] = 0; return out; }
        p = strchr(p, ','); if (p) p++;
    }
    return NULL;
}
/* o conector pode rodar neste escopo? 1 sim; 0 nao, com o motivo */
static int uioe_runnable(UioeCtx *cx, int ix, char *why, size_t cap) {
    const UioeConnector *c = &cx->s->reg[ix];
    if (!c->run) { snprintf(why, cap, "%s exists in the registry but is not built in this phase (earlier sessions and the web arrive in U8 and U9)", c->id); return 0; }
    if (cx->s->brk[ix].open) { snprintf(why, cap, "circuit breaker open: failed twice, skipped for %d more request(s)", cx->s->brk[ix].skip_left); return 0; }
    char w[200]; const int av = c->available(c, cx->root, w, sizeof w);
    if (!av && strcmp(c->id, "docs.select") != 0) { snprintf(why, cap, "%s", w); return 0; }
    if (!strcmp(c->id, "docs.select") && !cx->tk) { snprintf(why, cap, "needs the model's tokenizer: only inside an agent session"); return 0; }
    if (!strncmp(c->id, "code.", 5)) {
        if (!cx->has_store) { snprintf(why, cap, "no index store for this folder"); return 0; }
        if (c->need_level >= 2 && cx->view_state == 1 && !cx->have_sym) { snprintf(why, cap, "the index has no symbol sections (partial index)"); return 0; }
        if (c->need_level >= 3 && cx->view_state == 1 && !cx->have_keys) { snprintf(why, cap, "the index has no text-key sections (partial index)"); return 0; }
        if (cx->view_state < 0 && c->need_level >= 2) { snprintf(why, cap, "%s", cx->view_note); return 0; }
    }
    return 1;
}
static const char *g_ur_name[] = {"ok", "partial", "empty", "unavailable", "timeout", "error"};
/* uma chamada de conector. res recebe o resultado (o chamador chama ur_free depois de ler os nos); map recebe o no do grafo de cada no do resultado. */
static int uioe_call(UioeCtx *cx, UioeRecall *rc, const char *state, const char *label, const char *id, const UioeCall *qin, UioeResult *res, int *map, int mapcap) {
    memset(res, 0, sizeof *res);
    UioeCall q = *qin;
    UioeExec *ex = rc->nex < 80 ? &rc->ex[rc->nex++] : NULL;
    if (ex) { memset(ex, 0, sizeof *ex); ex->n = rc->nex; snprintf(ex->state, sizeof ex->state, "%s", state); snprintf(ex->label, sizeof ex->label, "%s", label); snprintf(ex->conn, sizeof ex->conn, "%s", id);
              snprintf(ex->input, sizeof ex->input, "%.130s%s%.20s%s", q.arg, q.arg2[0] ? " [" : "", q.arg2, q.arg2[0] ? "]" : ""); if (q.line) { const size_t l = strlen(ex->input); snprintf(ex->input + l, sizeof ex->input - l, ":%d", q.line); } }
    for (int i = 0; i < UG_MAXNODE && i < mapcap; i++) map[i] = -1;
    const int ix = uioe_find_conn(cx->s, id);
    char why[220];
    if (ix < 0) { res->status = UR_UNAVAILABLE; snprintf(res->note, sizeof res->note, "unknown connector"); goto done; }
    if (!uioe_runnable(cx, ix, why, sizeof why)) { res->status = UR_UNAVAILABLE; snprintf(res->note, sizeof res->note, "%s", why); goto done; }
    if (uioe_over(cx)) { res->status = UR_TIMEOUT; snprintf(res->note, sizeof res->note, "the request deadline (%d ms) passed before this call", cx->deadline_ms); goto done; }
    const UioeConnector *c = &cx->s->reg[ix];
    q.deadline_ms = c->default_ms; if (!cx->det && uioe_left_ms(cx) < q.deadline_ms) q.deadline_ms = uioe_left_ms(cx);
    if (cx->s->brk[ix].fails == 1) q.deadline_ms /= 2;      /* meio-aberto: uma tentativa com metade do prazo */
    const char *fault = uioe_fault_of(cx, id);
    const double t0 = now_sec(); int attempt = 0, soft = 0;
    if (fault && !attempt && (!strcmp(fault, "timeout") || !strcmp(fault, "error"))) { res->status = !strcmp(fault, "error") ? UR_ERROR : UR_TIMEOUT; snprintf(res->note, sizeof res->note, "injected fault (KATANA_UIOE_FAULT): %s", fault); }
    else {
        c->run(cx, &q, res);
        if (fault && !attempt && !strcmp(fault, "partial")) { res->status = UR_TIMEOUT; snprintf(res->note, sizeof res->note, "injected fault (KATANA_UIOE_FAULT): partial data then timeout"); }
        const double softms = (c->cost == UC_INDEX || c->cost == UC_INSTANT ? 4.0 : 1.0) * c->default_ms;
        if (!cx->det && !fault && res->status != UR_UNAVAILABLE && (now_sec() - t0) * 1000.0 > softms) { res->status = UR_TIMEOUT; soft = 1; snprintf(res->note, sizeof res->note, "took %.0f ms, over its %.0f ms deadline (the result is complete)", (now_sec() - t0) * 1000.0, softms); }
    }
    if (res->status == UR_TIMEOUT && res->nnode > 0 && !attempt && !soft && uioe_left_ms(cx) > 20 && ix >= 0) {      /* tentativa unica, com metade do que resta: so quando o resultado e PARCIAL (um resultado completo que demorou nao se repete) */
        attempt = 1; if (ex) ex->retried = 1;
        UioeResult r2; memset(&r2, 0, sizeof r2); q.deadline_ms = uioe_left_ms(cx) / 2;
        const int st1 = res->status; (void)st1;
        c->run(cx, &q, &r2);
        if (r2.nnode >= res->nnode) { ur_free(res); *res = r2; snprintf(res->note, sizeof res->note, "retried once after a timeout with partial data; the second run %s", g_ur_name[r2.status < 6 ? r2.status : 5]); }
        else ur_free(&r2);
    }
    res->ms = (now_sec() - t0) * 1000.0;
    uioe_breaker_note(cx->s, ix, res->status);
done:
    for (int i = 0; i < res->nnode; i++) { UioeNode *x = &res->node[i]; if (uioe_secret_path(x->path)) { x->secret = 1; x->text[0] = 0; x->sig[0] = 0; } else uioe_mask_line(x->text); }       /* um arquivo de segredo mostra o lugar, nunca o texto */
    if (res->status == UR_OK || res->status == UR_PARTIAL || res->status == UR_TIMEOUT || res->status == UR_EMPTY) {
        if (ix >= 0 && res->nnode) { const int e0 = rc->g->ne; (void)e0; ug_merge(rc->g, res, ix, id, q.anchor, map, mapcap); }
    }
    if (ex) { ex->status = res->status; ex->ms = res->ms; ex->nnode = res->nnode; ex->nedge = res->nedge; snprintf(ex->note, sizeof ex->note, "%.120s%s%.70s", res->note, res->note[0] && res->notfound[0] && res->status == UR_EMPTY ? "; " : "", res->status == UR_EMPTY ? res->notfound : ""); }
    /* o passo do plano que esta chamada cumpre */
    return res->status;
}
