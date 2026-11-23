/* ---------- uioe/evrender.c: a saida da recuperacao, em texto e em JSON (docs/uioe.md 3.11.1, fase U3) ----------
 * `katana uioe ask` imprime, depois do pedido interpretado e do plano (cli.c), a estrutura de evidencia que a recuperacao achou:
 *   path        o caminho de resposta (so fluxo): passos com arquivo:linhas, confianca, se foi relido, e a ligacao entre eles
 *   evidence    os demais itens, em ordem de faixa e fusao: arquivo:linhas, tipo, confianca, faixa (T1 a T4), relido ou nao, de onde veio
 *   not found / limits  o que nao se achou e o que nao se olhou, e por que (sempre presente)
 * Isto NAO e o bloco de contexto do modelo (o Construtor de Contexto e de U4): nao ha rotulos fixos nem orcamento de tokens aqui. Cada linha
 * mostra o numero de confianca e a marca de verificacao (ok = linha relida e confere, moved = a linha andou e foi corrigida, STALE = nao
 * confere, - = nao relida). O traco (~/.katana/uioe.log) leva as mesmas decisoes em linhas curtas, sem tempos nas linhas comparaveis
 * (`katana uioe explain` as repete e marca o que mudou). */
static void ui_json_str(UioeBuf *b, const char *s) {
    uioe_bprintf(b, "\"");
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') uioe_bprintf(b, "\\%c", c);
        else if (c == '\n') uioe_bprintf(b, "\\n");
        else if (c < 0x20) uioe_bprintf(b, "\\u%04x", c);
        else uioe_bprintf(b, "%c", c);
    }
    uioe_bprintf(b, "\"");
}
static const char *const g_ui_decname[] = {"no packet", "limits only", "packet"};
static const char *ur_ver_label(int v) { return v == UV_OK ? "ok" : v == UV_MOVED ? "moved" : v == UV_STALE ? "STALE" : v == UV_GONE ? "GONE" : v == UV_UNREADABLE ? "unreadable" : v == UV_SKIPPED ? "n/a" : "not reread"; }
static const char *g_ur_kindname[] = {"file", "range", "symbol", "key", "docline", "fact", "folder"};
static const char *g_ue_kindname[] = {"calls", "defines", "uses", "imports", "sends", "handles", "links", "contains", "mentions", "via"};
static void er_loc(UioeBuf *b, const UioeNode *n) {
    if (n->kind == UN_FOLDER) { uioe_bprintf(b, "%s", n->path); return; }
    if (n->kind == UN_FILE) { uioe_bprintf(b, "%s", n->path); return; }
    if (n->line1 > n->line0) uioe_bprintf(b, "%s:%d-%d", n->path, n->line0, n->line1); else uioe_bprintf(b, "%s:%d", n->path, n->line0);
}
static void er_node(UioeBuf *b, const UioeGraph *g, int id, const char *ind, int withhow) {
    const UioeNode *n = &g->n[id]; if (!withhow && (n->kind == UN_KEY || !strcmp(n->cls, "caller") || !strcmp(n->cls, "test"))) withhow = 2;
    uioe_bprintf(b, "%s[%d %s T%d] ", ind, n->conf, ur_ver_label(n->ver), n->tier); er_loc(b, n);
    uioe_bprintf(b, "  %s", g_ur_kindname[n->kind]);
    if (n->cls[0] && n->kind != UN_RANGE) uioe_bprintf(b, "/%s", n->cls);
    if (n->name[0] && strcmp(n->name, "file")) uioe_bprintf(b, " %s", n->name);
    if (n->sig[0] && (n->kind == UN_KEY || n->kind == UN_RANGE || !strcmp(n->cls, "caller") || !strcmp(n->cls, "test"))) uioe_bprintf(b, "  %s", n->sig);
    if (n->shown || n->read) uioe_bprintf(b, "  (already %s)", n->read ? "read" : "shown");
    uioe_bprintf(b, "\n");
    if (n->text[0] && !n->secret) uioe_bprintf(b, "%s      %.150s\n", ind, n->text);
    if (n->secret) uioe_bprintf(b, "%s      (a secret file: the value is never printed)\n", ind);
    if (n->vnote[0]) uioe_bprintf(b, "%s      note: %s\n", ind, n->vnote);
    if (withhow == 1 && n->how[0]) uioe_bprintf(b, "%s      how: %.160s\n", ind, n->how);
    else if (withhow == 2 && n->how[0]) { char h[90]; snprintf(h, sizeof h, "%.85s", n->how); uioe_bprintf(b, "%s      how: %s\n", ind, h); }
}
static void uioe_render_recall_text(UioeBuf *b, const UioeCtx *cx, const UioeRequest *r) {
    const UioeRecall *rc = &r->rc; const UioeRoute *rt = &r->route; const UioeGraph *g = rc->g;
    if (!rc->ran) { uioe_bprintf(b, "recall    not run: %s\n", rc->decision_note[0] ? rc->decision_note : "nothing to retrieve"); return; }
    uioe_bprintf(b, "recall    pipeline %s   %.1f ms in the request clock (deadline %d ms%s)   index step %.1f ms   %d node(s), %d edge(s)\n", rt->pipeline, rc->ms, cx->deadline_ms, cx->det ? ", deterministic: no time cuts" : "", rc->ms_index, g->nn, g->ne);
    uioe_bprintf(b, "scope     %s\n", rc->scope);
    for (int i = 0; i < rc->ndeg; i++) uioe_bprintf(b, "degraded  %s\n", rc->degrade[i]);
    uioe_bprintf(b, "plan steps\n");
    for (int i = 0; i < rt->nstep; i++) {
        const UioeStep *s = &rt->step[i];
        if (s->ran == 1) uioe_bprintf(b, "  %-2d %-9s %-14s ran via %s: %d call(s), %.1f ms, %d node(s), last status %s%s%.100s%s\n", s->n, s->label, s->conn, s->rconn, s->rcalls, s->rms, s->rnodes, g_ur_name[s->rstatus < 6 ? s->rstatus : 5], s->rnote[0] ? "  (" : "", s->rnote, s->rnote[0] ? ")" : "");
        else { const int ix = s->conn[0] ? uioe_find_conn(cx->s, s->conn) : -1; char dw[100] = ""; if (s->dep > 0 && s->dep <= rt->nstep) { const UioeStep *d = &rt->step[s->dep - 1]; if (d->ran != 1) snprintf(dw, sizeof dw, "nothing to start from: step %d did not run", s->dep); else if (!d->rnodes) snprintf(dw, sizeof dw, "nothing to start from: step %d returned nothing", s->dep); }
            const char *why = s->status == 1 ? "no connector available" : s->status == 2 ? "not routed (policy)" : (ix >= 0 && !cx->s->reg[ix].run) ? "connector not built in this phase (U8 and U9)" : dw[0] ? dw : s->cap == 0 ? "done inside the pipeline (local, no connector)" : "the pipeline did not need it";
            uioe_bprintf(b, "  %-2d %-9s %-14s NOT RUN: %s%s%.100s%s\n", s->n, s->label, s->conn[0] ? s->conn : "(none)", why, s->note[0] ? " (" : "", s->note, s->note[0] ? ")" : ""); }
    }
    uioe_bprintf(b, "calls (%d)\n", rc->nex);
    for (int i = 0; i < rc->nex; i++) { const UioeExec *e = &rc->ex[i];
        uioe_bprintf(b, "  %-2s %-12s %-13s %-40s %6.1f ms  %-11s %d node(s)%s%s%.90s%s\n", e->state, e->label, e->conn, e->input, e->ms, g_ur_name[e->status < 6 ? e->status : 5], e->nnode, e->retried ? "  (retried once)" : "", e->note[0] ? "  (" : "", e->note, e->note[0] ? ")" : ""); }
    if (rc->npath) {
        uioe_bprintf(b, "path      answer path: %d step(s); the confidence of a step is its own, the link to the next is on the arrow\n", rc->npath);
        for (int i = 0; i < rc->npath; i++) {
            uioe_bprintf(b, "  %d. ", i + 1); er_node(b, g, rc->path[i], "", 1);
            if (i + 1 < rc->npath && rc->pathedge[i] >= 0) { const UioeEdge *e = &g->e[rc->pathedge[i]];
                uioe_bprintf(b, "       %s %s%s%s -> next step  [confidence %d, %s]  %.140s\n", g_ue_kindname[e->kind], e->key[0] ? "\"" : "", e->key, e->key[0] ? "\"" : "", e->conf, ur_ver_label(e->ver), e->how); }
            else if (i + 1 < rc->npath) uioe_bprintf(b, "       (same enclosing symbol)\n");
        }
        int minc = 100; for (int i = 0; i + 1 < rc->npath; i++) if (rc->pathedge[i] >= 0 && g->e[rc->pathedge[i]].conf < minc) minc = g->e[rc->pathedge[i]].conf;
        uioe_bprintf(b, "       path confidence (weakest link): %d\n", minc == 100 ? 0 : minc);
        /* as outras ligacoes entre os dois primeiros arquivos */
        if (rc->npath >= 2) { const UioeNode *P = &g->n[rc->path[0]], *C = &g->n[rc->path[1]]; int shown = 0;
            for (int i = 0; i < g->ne && shown < 3; i++) { const UioeEdge *e = &g->e[i]; if (e->kind != UE_LINKS || (rc->pathedge[0] == i)) continue; const UioeNode *p = &g->n[e->from], *c = &g->n[e->to];
                if (strcmp(p->path, P->path) || strcmp(c->path, C->path)) continue;
                if (!shown) uioe_bprintf(b, "       also linking the same two files:\n");
                uioe_bprintf(b, "         \"%s\" %s:%d -> %s:%d  [confidence %d, %s]\n", e->key, p->path, p->line0, c->path, c->line0, e->conf, ur_ver_label(e->ver)); shown++; } }
    }
    for (int i = 0; i < rc->nscore; i++) uioe_bprintf(b, "%s%s\n", i ? "          " : "score     ", rc->score[i]);
    if (rc->nbranch) { uioe_bprintf(b, "branches  not followed (weaker or alternative next steps)\n");
        for (int i = 0; i < rc->nbranch; i++) { const UioeEdge *e = &g->e[rc->branch[i]]; const UioeNode *f = &g->n[e->from], *t = &g->n[e->to];
            uioe_bprintf(b, "  - %s from %s:%d to %s:%d %s  [confidence %d]\n", g_ue_kindname[e->kind], f->path, f->line0, t->path, t->line0, t->name, e->conf); } }
    if (rc->nitems) {
        uioe_bprintf(b, "evidence  %d item(s) shown, ordered by tier (T1 named and exact, T2 reached by structure, T3 derived or corroborated, T4 words only), then fusion rank\n", rc->nitems);
        for (int i = 0; i < rc->nitems; i++) if (strcmp(g->n[rc->items[i]].cls, "test")) er_node(b, g, rc->items[i], "  ", 0);
        { int nt = 0; for (int i = 0; i < rc->nitems; i++) if (!strcmp(g->n[rc->items[i]].cls, "test")) { if (!nt++) uioe_bprintf(b, "  tests that call it (listed apart)\n"); er_node(b, g, rc->items[i], "  ", 0); } }
        if (rc->nitems_all > rc->nitems) uioe_bprintf(b, "  (%d more qualified and were not shown or reread)\n", rc->nitems_all - rc->nitems);
    } else uioe_bprintf(b, "evidence  none\n");
    uioe_bprintf(b, "not found / limits\n");
    if (!rc->nlim) uioe_bprintf(b, "  (none recorded)\n");
    for (int i = 0; i < rc->nlim; i++) uioe_bprintf(b, "  - %s\n", rc->limits[i]);
    if (rc->nhyp) { uioe_bprintf(b, "open hypotheses\n"); for (int i = 0; i < rc->nhyp; i++) uioe_bprintf(b, "  - %s\n", rc->hyp[i]); }
    uioe_bprintf(b, "next      %s\n", rc->next);
    uioe_bprintf(b, "gate      confidence after retrieval %d\n", rc->conf_after);
    for (int i = 0; i < rc->nsig; i++) uioe_bprintf(b, "          %s\n", rc->sig[i]);
    uioe_bprintf(b, "          %s\n", rc->decision_note);
    uioe_bprintf(b, "          (U3 only decides the gate; the context block that the model would receive is built by U4)\n");
}
static void uioe_render_recall_json(UioeBuf *b, const UioeCtx *cx, const UioeRequest *r) {
    const UioeRecall *rc = &r->rc; const UioeRoute *rt = &r->route; const UioeGraph *g = rc->g;
    uioe_bprintf(b, ",\"recall\":{\"ran\":%s", rc->ran ? "true" : "false");
    if (!rc->ran) { uioe_bprintf(b, ",\"note\":"); ui_json_str(b, rc->decision_note); uioe_bprintf(b, "}"); return; }
    uioe_bprintf(b, ",\"ms\":%.1f,\"index_ms\":%.1f,\"deterministic\":%s,\"scope\":", rc->ms, rc->ms_index, cx->det ? "true" : "false"); ui_json_str(b, rc->scope);
    uioe_bprintf(b, ",\"index_mode\":\"%s\",\"degradations\":[", rc->idx_mode == 2 ? "complete" : rc->idx_mode == 1 ? "partial" : "none");
    for (int i = 0; i < rc->ndeg; i++) { if (i) uioe_bprintf(b, ","); ui_json_str(b, rc->degrade[i]); }
    uioe_bprintf(b, "],\"plan_steps\":[");
    for (int i = 0; i < rt->nstep; i++) { const UioeStep *s = &rt->step[i];
        uioe_bprintf(b, "%s{\"n\":%d,\"label\":\"%s\",\"connector\":\"%s\",\"ran\":%s,\"calls\":%d,\"ms\":%.1f,\"nodes\":%d,\"status\":\"%s\"}", i ? "," : "", s->n, s->label, s->conn, s->ran == 1 ? "true" : "false", s->rcalls, s->rms, s->rnodes, s->ran == 1 ? g_ur_name[s->rstatus < 6 ? s->rstatus : 5] : "not run"); }
    uioe_bprintf(b, "],\"calls\":[");
    for (int i = 0; i < rc->nex; i++) { const UioeExec *e = &rc->ex[i];
        uioe_bprintf(b, "%s{\"state\":\"%s\",\"label\":\"%s\",\"connector\":\"%s\",\"input\":", i ? "," : "", e->state, e->label, e->conn); ui_json_str(b, e->input);
        uioe_bprintf(b, ",\"status\":\"%s\",\"ms\":%.1f,\"nodes\":%d,\"retried\":%s,\"note\":", g_ur_name[e->status < 6 ? e->status : 5], e->ms, e->nnode, e->retried ? "true" : "false"); ui_json_str(b, e->note); uioe_bprintf(b, "}"); }
#define NODE_JSON(id, extra) do { const UioeNode *n = &g->n[id]; \
        uioe_bprintf(b, "{\"kind\":\"%s\",\"file\":", g_ur_kindname[n->kind]); ui_json_str(b, n->path); \
        uioe_bprintf(b, ",\"line0\":%d,\"line1\":%d,\"name\":", n->line0, n->line1); ui_json_str(b, n->name); \
        uioe_bprintf(b, ",\"class\":"); ui_json_str(b, n->cls); uioe_bprintf(b, ",\"confidence\":%d,\"tier\":%d,\"relevance\":%d,\"verified\":\"%s\",\"sources\":%d,\"already_shown\":%s,\"text\":", n->conf, n->tier, n->relevance, ur_ver_label(n->ver), n->nsrc, n->shown || n->read ? "true" : "false"); \
        ui_json_str(b, n->secret ? "" : n->text); uioe_bprintf(b, ",\"how\":"); ui_json_str(b, n->how); uioe_bprintf(b, ",\"note\":"); ui_json_str(b, n->vnote); extra; uioe_bprintf(b, "}"); } while (0)
    uioe_bprintf(b, "],\"path\":[");
    for (int i = 0; i < rc->npath; i++) { if (i) uioe_bprintf(b, ","); const UioeEdge *e = (i + 1 < rc->npath && rc->pathedge[i] >= 0) ? &g->e[rc->pathedge[i]] : NULL;
        NODE_JSON(rc->path[i], { if (e) { uioe_bprintf(b, ",\"link_to_next\":{\"kind\":\"%s\",\"key\":", g_ue_kindname[e->kind]); ui_json_str(b, e->key); uioe_bprintf(b, ",\"confidence\":%d,\"verified\":\"%s\",\"how\":", e->conf, ur_ver_label(e->ver)); ui_json_str(b, e->how); uioe_bprintf(b, "}"); } }); }
    uioe_bprintf(b, "],\"branches\":[");
    for (int i = 0; i < rc->nbranch; i++) { const UioeEdge *e = &g->e[rc->branch[i]]; if (i) uioe_bprintf(b, ","); NODE_JSON(e->to, { uioe_bprintf(b, ",\"edge\":\"%s\",\"edge_confidence\":%d", g_ue_kindname[e->kind], e->conf); }); }
    uioe_bprintf(b, "],\"evidence\":[");
    for (int i = 0; i < rc->nitems; i++) { if (i) uioe_bprintf(b, ","); NODE_JSON(rc->items[i], (void)0); }
    uioe_bprintf(b, "],\"not_found_limits\":[");
    for (int i = 0; i < rc->nlim; i++) { if (i) uioe_bprintf(b, ","); ui_json_str(b, rc->limits[i]); }
    uioe_bprintf(b, "],\"hypotheses\":["); for (int i = 0; i < rc->nhyp; i++) { if (i) uioe_bprintf(b, ","); ui_json_str(b, rc->hyp[i]); }
    uioe_bprintf(b, "],\"verification\":{\"verified\":%d,\"moved\":%d,\"stale\":%d,\"not_reread\":%d},\"next\":", rc->nverified, rc->nmoved, rc->nstale, rc->nnotreread); ui_json_str(b, rc->next);
    uioe_bprintf(b, ",\"gate\":{\"confidence_after\":%d,\"rule\":\"G%d\",\"decision\":\"%s\",\"signals\":[", rc->conf_after, rc->rule, g_ui_decname[rc->decision]);
    for (int i = 0; i < rc->nsig; i++) { if (i) uioe_bprintf(b, ","); ui_json_str(b, rc->sig[i]); }
    uioe_bprintf(b, "],\"note\":"); ui_json_str(b, rc->decision_note); uioe_bprintf(b, "}}");
#undef NODE_JSON
}
/* ---- o traco da recuperacao: linhas curtas; os tempos ficam depois de " | " para o explain comparar sem eles ---- */
static void uioe_fmt_recall_trace(UioeBuf *b, const UioeCtx *cx, const UioeRequest *r) {
    const UioeRecall *rc = &r->rc; const UioeGraph *g = rc->g;
    if (!rc->ran) { uioe_bprintf(b, "recall  not run: %s\n", rc->decision_note); return; }
    uioe_bprintf(b, "scope   %s | index step %.1f ms\n", rc->scope, rc->ms_index);
    for (int i = 0; i < rc->ndeg; i++) uioe_bprintf(b, "degrade %s\n", rc->degrade[i]);
    for (int i = 0; i < rc->nstates; i++) { const char *s = rc->states[i]; const char *ms = strstr(s, "; index step"); uioe_bprintf(b, "state   %.*s\n", ms ? (int)(ms - s) : (int)strlen(s), s); }
    for (int i = 0; i < rc->nex; i++) { const UioeExec *e = &rc->ex[i];
        uioe_bprintf(b, "call    %s %s %s %.70s -> %s, %d node(s)%s%s%.80s | %.1f ms\n", e->state, e->label, e->conn, e->input, g_ur_name[e->status < 6 ? e->status : 5], e->nnode, e->retried ? ", retried once" : "", e->note[0] ? "; " : "", e->note, e->ms); }
    for (int i = 0; i < rc->nscore; i++) uioe_bprintf(b, "score   %s\n", rc->score[i]);
    for (int i = 0; i < rc->npath; i++) { const UioeNode *n = &g->n[rc->path[i]]; uioe_bprintf(b, "path    %d. [%d %s T%d] %s:%d-%d %s %s\n", i + 1, n->conf, ur_ver_label(n->ver), n->tier, n->path, n->line0, n->line1, g_ur_kindname[n->kind], n->name);
        if (i + 1 < rc->npath && rc->pathedge[i] >= 0) { const UioeEdge *e = &g->e[rc->pathedge[i]]; uioe_bprintf(b, "link    %s \"%s\" conf %d %s\n", g_ue_kindname[e->kind], e->key, e->conf, ur_ver_label(e->ver)); } }
    for (int i = 0; i < rc->nitems && i < 12; i++) { const UioeNode *n = &g->n[rc->items[i]]; uioe_bprintf(b, "item    [%d %s T%d] %s:%d-%d %s %s\n", n->conf, ur_ver_label(n->ver), n->tier, n->path, n->line0, n->line1, g_ur_kindname[n->kind], n->name); }
    for (int i = 0; i < rc->nlim; i++) uioe_bprintf(b, "limit   %s\n", rc->limits[i]);
    for (int i = 0; i < rc->nhyp; i++) uioe_bprintf(b, "hyp     %s\n", rc->hyp[i]);
    for (int i = 0; i < rc->nsig; i++) uioe_bprintf(b, "signal  %s\n", rc->sig[i]);
    uioe_bprintf(b, "gate    %s | %.1f ms in the request clock%s\n", rc->decision_note, rc->ms, cx->det ? " (deterministic)" : "");
}
