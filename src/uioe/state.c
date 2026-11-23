/* ---------- uioe/state.c: o estado do pedido visto pelo UioeRequest (docs/uioe.md 3.6.2, fase U0) ----------
 * As globais do pedido continuam onde estao (g_rs, g_rc, g_intent_* em tools/common.c; g_place_dirs em agent/locate.c; g_fd_* em tools/find.c;
 * rr_*, rp_*, bk_* em agent/loop.c). U0 nao move dado nenhum: uioe_req_sync() tira um INSTANTANEO delas para dentro do UioeRequest, e e so
 * por ele que o UIOE as le. Quem escreve nelas continua sendo o codigo de antes: o comportamento do laco e das ferramentas e identico.
 * As globais de loop.c vem depois deste arquivo; uioe_loop_state() e definida la (um ganho de poucas linhas, ao lado de rr_reset). */
static void uioe_loop_state(UioeLoopState *o);      /* agent/loop.c */

static void uioe_req_sync(UioeRequest *r) {
    r->nshown = r->nread = 0;
    for (int i = 0; i < g_rs_n; i++) {
        if ((g_rs[i].how & 1) && r->nshown < 256) { UioeRange *x = &r->shown[r->nshown++]; snprintf(x->path, sizeof x->path, "%.1020s", g_rs[i].path); x->a = g_rs[i].a; x->b = g_rs[i].b; x->how = g_rs[i].how; }
        if ((g_rs[i].how & 2) && r->nread < 256) { UioeRange *x = &r->read[r->nread++]; snprintf(x->path, sizeof x->path, "%.1020s", g_rs[i].path); x->a = g_rs[i].a; x->b = g_rs[i].b; x->how = g_rs[i].how; }
    }
    r->ncur = 0;
    for (int i = 0; i < g_rc_n && r->ncur < 64; i++) {
        UioeCursor *c = &r->cur[r->ncur++]; snprintf(c->path, sizeof c->path, "%.1020s", g_rc[i].path);
        c->cov_end = g_rc[i].cov_end; c->last_off = g_rc[i].last_off; c->total = g_rc[i].total;
    }
    snprintf(r->intent_user, sizeof r->intent_user, "%s", g_intent_user);
    snprintf(r->intent_find, sizeof r->intent_find, "%s", g_intent_find);
#ifndef _WIN32
    r->nplace = g_place_nd < 4 ? g_place_nd : 4;
    for (int i = 0; i < r->nplace; i++) snprintf(r->place_dirs[i], sizeof r->place_dirs[0], "%.1090s", g_place_dirs[i]);
#else
    r->nplace = 0;
#endif
    snprintf(r->last_query, sizeof r->last_query, "%s", g_fd_lastq);
    r->nlast = g_fd_lqn;
    uioe_loop_state(&r->loop);
}

/* `katana uioe state` e /uioe: imprime o instantaneo */
static void uioe_state_print(FILE *o, const UioeRequest *r) {
    fprintf(o, "mode        %s (KATANA_UIOE; pack is the default)\n", uioe_mode() == UIOE_OFF ? "off" : uioe_mode() == UIOE_PACK ? "pack" : "full");
    fprintf(o, "intent      user: \"%.120s\"\n", r->intent_user);
    fprintf(o, "            find: \"%.120s\"\n", r->intent_find);
    fprintf(o, "shown       %d range(s) pointed at by Find\n", r->nshown);
    for (int i = 0; i < r->nshown && i < 20; i++) fprintf(o, "            %s:%d-%d\n", r->shown[i].path, r->shown[i].a, r->shown[i].b);
    fprintf(o, "read        %d range(s) delivered by Read\n", r->nread);
    for (int i = 0; i < r->nread && i < 20; i++) fprintf(o, "            %s:%d-%d%s\n", r->read[i].path, r->read[i].a, r->read[i].b, (r->read[i].how & 1) ? " (also pointed at)" : "");
    fprintf(o, "cursors     %d file(s)\n", r->ncur);
    for (int i = 0; i < r->ncur && i < 20; i++) fprintf(o, "            %s covered to line %lld of %lld (last read from %lld)\n", r->cur[i].path, r->cur[i].cov_end, r->cur[i].total, r->cur[i].last_off);
    fprintf(o, "places      %d folder(s) cited and located\n", r->nplace);
    for (int i = 0; i < r->nplace; i++) fprintf(o, "            %s\n", r->place_dirs[i]);
    fprintf(o, "find        last query \"%.100s\" (%d term(s) kept for paging)\n", r->last_query, r->nlast);
    fprintf(o, "loop        repeat table %d entr%s, %d call(s) this request; failures: %d (refused %d, stuck %d); obstacle notes %d\n", r->loop.rr_n, r->loop.rr_n == 1 ? "y" : "ies",
            r->loop.rr_total, r->loop.rp_fail, r->loop.rp_refused, r->loop.rp_stuck, r->loop.bk_n);
}
/* /uioe dentro do agente: o estado do pedido em andamento */
static void uioe_slash(const char *arg) {
    while (*arg == ' ') arg++;
    if (*arg && strcmp(arg, "state")) { fprintf(stderr, "[/uioe: only `state` exists in this build]\n"); return; }
    UioeRequest *r = (UioeRequest *)calloc(1, sizeof *r); if (!r) return;
    uioe_req_sync(r); uioe_state_print(stderr, r); free(r);
}
