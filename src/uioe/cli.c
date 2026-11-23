/* ---------- uioe/cli.c: `katana uioe` (docs/uioe.md 3.11.1, fases U0, U1 e U3) ----------
 *   katana uioe ask "<pedido>" [--cwd DIR] [--json] [--no-trace] [--explain] [--no-verify] [--index] [--record]
 *        interpreta, classifica, roteiriza e EXECUTA a rota (U3): imprime o pedido interpretado, o plano e o resultado da recuperacao (grafo de
 *        evidencias resumido, o que nao foi achado e os limites). Sem modelo. --explain: modo deterministico (sem corte por tempo) e imprime
 *        o registro de decisao inteiro (o mesmo do traco). --no-verify: nao rele as linhas. --index: constroi o indice antes se nao houver. --no-refresh: nao chama index_ensure (usa o indice como esta, mesmo parcial).
 *        --record: marca o que mostrou no estado do pedido (o que o Find e o Read veriam depois).
 *   katana uioe plan "<pedido>"                                      so o pedido interpretado e o plano (U1): nada e executado
 *   katana uioe explain [last|N|ID]                                  reproduz um pedido do traco na arvore de agora e marca o que mudou
 *   katana uioe session [--cwd DIR] [--json]                         um pedido por linha de stdin, na MESMA sessao (disjuntores e estado do pedido)
 *   katana uioe connectors [--cwd DIR]                               o registro: capacidades, custo, disponibilidade nesta pasta, disjuntor
 *   katana uioe state                                                 o estado do pedido (num processo novo nao ha pedido: use /uioe no agente)
 * Cada `ask` grava um registro no traco (~/.katana/uioe.log; --no-trace ou KATANA_UIOE_LOG=0 desligam). O laco do agente nao chama o UIOE. */
static void ui_flags_str(unsigned v, const char *const *names, int n, char *out, size_t cap) {
    out[0] = 0; size_t o = 0;
    for (int i = 0; i < n; i++) if (v & (1u << i)) o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? "," : "", names[i]);
    if (!o) snprintf(out, cap, "none");
}
static const char *const g_ui_relnames[] = {"callers", "defined", "uses", "send", "receive", "work/fail", "negation", "callees"};
static const char *const g_ui_risknames[] = {"changes-files", "runs-commands", "leaves-machine", "sensitive", "destructive"};

static void uioe_render_text(UioeBuf *b, const UioeAsk *a, const UioeClass *k, const UioeRoute *rt) {
    char rel[120], risk[120];
    ui_flags_str(a->rel, g_ui_relnames, 8, rel, sizeof rel); ui_flags_str(k->risk, g_ui_risknames, 5, risk, sizeof risk);
    uioe_bprintf(b, "ask       \"%.600s\"\n", a->text);
    uioe_bprintf(b, "lang      %s\n", a->lang);
    uioe_bprintf(b, "root      %s   (%s)\n", a->root, a->index_note);
    if (a->nent) { uioe_bprintf(b, "entities\n"); uioe_fmt_ents(b, a, "          "); } else uioe_bprintf(b, "entities  none\n");
    if (a->nterms) {
        uioe_bprintf(b, "terms     ");
        for (int i = 0; i < a->nterms; i++) uioe_bprintf(b, "%s%s>%s%s%s%s", i ? "   " : "", a->terms[i], a->norm[i], a->concept[i][0] ? " [" : "", a->concept[i], a->concept[i][0] ? "]" : "");
        uioe_bprintf(b, "\n");
    } else uioe_bprintf(b, "terms     none\n");
    if (a->nxw) { uioe_bprintf(b, "expand    "); for (int i = 0; i < a->nxw; i++) uioe_bprintf(b, "%s%s:%s", i ? "   " : "", a->terms[a->xw_of[i]], a->xw[i]); uioe_bprintf(b, "   (words of each term's concepts in the knowledge base, used to match the code)\n"); }
    if (a->ndropped) { uioe_bprintf(b, "dropped   "); for (int i = 0; i < a->ndropped; i++) uioe_bprintf(b, "%s%s", i ? ", " : "", a->dropped[i]); uioe_bprintf(b, "\n"); }
    uioe_bprintf(b, "relation  %s%s%s\n", rel, a->dir[0] ? "   direction: " : "", a->dir);
    for (int i = 0; i < a->ncon; i++) uioe_bprintf(b, "constraint %s: %s\n", a->con[i].what, a->con[i].text);
    uioe_bprintf(b, "gap       n/a (the indexes are not consulted before U3)      ambiguous %s%s%s\n", a->ambiguous ? "yes: " : "no", a->amb, "");
    uioe_bprintf(b, "class     domain %s", g_uioe_domname[k->domain[0]]);
    for (int i = 1; i < k->ndomain; i++) uioe_bprintf(b, "+%s", g_uioe_domname[k->domain[i]]);
    uioe_bprintf(b, " (rule D%d: %s)   kind %s (R%d%s: %s)   complexity %s\n", k->rule_domain, k->why_domain, g_uioe_kindname[k->kind], k->rule_kind, k->sure ? ", sure" : ", not sure", k->why_kind, g_uioe_cxname[k->complexity]);
    uioe_bprintf(b, "          retrieve %d (C%d: %s)   change %d   risk: %s\n", k->retrieve, k->rule_retrieve, k->why_retrieve, k->change, risk);
    if (!rt->nstep) uioe_bprintf(b, "route     no pipeline (%s)\n", k->retrieve == 0 ? "nothing to retrieve" : "retrieve = 2: no steps");
    else uioe_bprintf(b, "route     pipeline %s   deadline %d ms   ceiling %d tokens   confidence before retrieval %d\n", rt->pipeline, rt->deadline_ms, rt->token_ceiling, rt->confidence);
    if (!rt->nstep) uioe_bprintf(b, "          confidence n/a\n");
    for (int i = 0; i < rt->nstep; i++) {
        const UioeStep *s = &rt->step[i]; char dep[24] = "";
        if (s->dep) snprintf(dep, sizeof dep, "   depends on %d", s->dep);
        uioe_bprintf(b, "  %-2d %-9s %-14s %.100s%s%s%s%s%s\n", s->n, s->label, s->conn[0] ? s->conn : "(none)", s->input, dep, s->status == 1 ? "   NOT RUN" : s->status == 2 ? "   NOT ROUTED" : "", s->note[0] ? "  (" : "", s->note, s->note[0] ? ")" : "");
    }
    for (int i = 0; i < rt->nsig; i++) uioe_bprintf(b, "          %s\n", rt->signals[i]);
    if (rt->notrouted[0]) uioe_bprintf(b, "  not routed: %s\n", rt->notrouted);
    uioe_bprintf(b, "  sends out: %s\n", rt->sends_out[0] ? rt->sends_out : "nothing");
    uioe_bprintf(b, "decision  %s\n", rt->decision_note);
}
static void uioe_render_json(UioeBuf *b, const UioeAsk *a, const UioeClass *k, const UioeRoute *rt) {
    uioe_bprintf(b, "{\"ask\":"); ui_json_str(b, a->text);
    uioe_bprintf(b, ",\"lang\":\"%s\",\"root\":", a->lang); ui_json_str(b, a->root);
    uioe_bprintf(b, ",\"entities\":[");
    for (int i = 0; i < a->nent; i++) {
        const UioeEntity *e = &a->ent[i];
        uioe_bprintf(b, "%s{\"kind\":\"%s\",\"text\":", i ? "," : "", g_uioe_entname[(int)e->kind]); ui_json_str(b, e->text);
        uioe_bprintf(b, ",\"resolved\":"); ui_json_str(b, e->resolved); uioe_bprintf(b, ",\"how\":"); ui_json_str(b, e->how);
        uioe_bprintf(b, ",\"confidence\":%d,\"guess\":%s}", e->conf, e->guess ? "true" : "false");
    }
    uioe_bprintf(b, "],\"terms\":[");
    for (int i = 0; i < a->nterms; i++) { uioe_bprintf(b, "%s{\"term\":", i ? "," : ""); ui_json_str(b, a->terms[i]); uioe_bprintf(b, ",\"norm\":"); ui_json_str(b, a->norm[i]); uioe_bprintf(b, ",\"concept\":"); ui_json_str(b, a->concept[i]); uioe_bprintf(b, "}"); }
    uioe_bprintf(b, "],\"dropped\":[");
    for (int i = 0; i < a->ndropped; i++) { if (i) uioe_bprintf(b, ","); ui_json_str(b, a->dropped[i]); }
    char rel[120], risk[120]; ui_flags_str(a->rel, g_ui_relnames, 8, rel, sizeof rel); ui_flags_str(k->risk, g_ui_risknames, 5, risk, sizeof risk);
    uioe_bprintf(b, "],\"relation\":\"%s\",\"direction\":\"%s\",\"ambiguous\":%s,\"ambiguity\":", rel, a->dir, a->ambiguous ? "true" : "false"); ui_json_str(b, a->amb);
    uioe_bprintf(b, ",\"class\":{\"domains\":[");
    for (int i = 0; i < k->ndomain; i++) uioe_bprintf(b, "%s\"%s\"", i ? "," : "", g_uioe_domname[k->domain[i]]);
    uioe_bprintf(b, "],\"kind\":\"%s\",\"rule\":\"R%d\",\"sure\":%s,\"complexity\":\"%s\",\"retrieve\":%d,\"retrieve_rule\":\"C%d\",\"change\":%s,\"risk\":\"%s\"}",
                 g_uioe_kindname[k->kind], k->rule_kind, k->sure ? "true" : "false", g_uioe_cxname[k->complexity], k->retrieve, k->rule_retrieve, k->change ? "true" : "false", risk);
    uioe_bprintf(b, ",\"route\":{\"pipeline\":\"%s\",\"deadline_ms\":%d,\"token_ceiling\":%d,\"confidence\":%d,\"gate_rule\":%d,\"decision\":\"%s\",\"steps\":[",
                 rt->pipeline, rt->deadline_ms, rt->token_ceiling, rt->confidence, rt->rule, g_ui_decname[rt->decision]);
    for (int i = 0; i < rt->nstep; i++) {
        const UioeStep *s = &rt->step[i];
        uioe_bprintf(b, "%s{\"n\":%d,\"label\":\"%s\",\"connector\":\"%s\",\"depends_on\":%d,\"run\":%s,\"input\":", i ? "," : "", s->n, s->label, s->conn, s->dep, s->status ? "false" : "true");
        ui_json_str(b, s->input); uioe_bprintf(b, "}");
    }
    uioe_bprintf(b, "],\"signals\":["); for (int i = 0; i < rt->nsig; i++) { if (i) uioe_bprintf(b, ","); ui_json_str(b, rt->signals[i]); }
    uioe_bprintf(b, "],\"note\":"); ui_json_str(b, rt->decision_note); uioe_bprintf(b, "}");
}

static void uioe_ctx_init(UioeCtx *c, UioeSession *s, const char *cwd_override) {
    memset(c, 0, sizeof *c); c->s = s; c->now = time(NULL); c->mode = uioe_mode();
    if (cwd_override && *cwd_override) { char rp[4200]; if (realpath(cwd_override, rp)) snprintf(c->cwd, sizeof c->cwd, "%.1090s", rp); else snprintf(c->cwd, sizeof c->cwd, "%.1090s", cwd_override); }
    else if (!getcwd(c->cwd, sizeof c->cwd)) snprintf(c->cwd, sizeof c->cwd, ".");
    snprintf(c->root, sizeof c->root, "%.1090s", c->cwd);
    c->deadline_ms = uioe_env_int("KATANA_UIOE_MS", 400, 20, 20000); c->token_ceiling = uioe_env_int("KATANA_UIOE_TOKENS", 1500, 200, 3000);
    c->trace = 1; c->verify = 1; c->run = 1; c->refresh = 1;
    { const char *d = getenv("KATANA_UIOE_DET"); c->det = d && !strcmp(d, "1"); }
    { const char *f = getenv("KATANA_UIOE_FAULT"); if (f) snprintf(c->fault, sizeof c->fault, "%s", f); }
}
static void uioe_ctx_close(UioeCtx *c) { uv_free(c); uioe_close_view(c); }
static UioeSession *uioe_session_new(void) {
    UioeSession *s = (UioeSession *)calloc(1, sizeof *s); if (!s) return NULL;
    s->cur = (UioeRequest *)calloc(1, sizeof *s->cur);
    if (!s->cur) { free(s); return NULL; }
    uioe_registry_init(s); uioe_bind_runs(s);
    return s;
}
static void uioe_session_free(UioeSession *s) { if (!s) return; free(s->cur->rc.g); free(s->cur); free(s); }

/* o pipeline inteiro sobre um texto: U1 (interpretar, classificar, roteirizar) e U3 (recuperar e orquestrar) */
static void uioe_run_ask(UioeCtx *cx, const char *text) {
    UioeRequest *r = cx->s->cur;
    r->id = uioe_new_id(cx->s);
    uioe_interpret(cx, text, &r->ask);
    uioe_classify(cx, &r->ask, &r->cls);
    snprintf(cx->root, sizeof cx->root, "%s", r->ask.root);       /* a rota enxerga a pasta achada, nao so a de trabalho */
    uioe_route(cx, &r->ask, &r->cls, &r->route);
    if (cx->run) uioe_orchestrate(cx, &r->ask, &r->cls, &r->route, &r->rc);
    else { UioeGraph *g = r->rc.g; memset(&r->rc, 0, sizeof r->rc); r->rc.g = g; }
}
/* um pedido: roda, imprime (texto ou JSON) e grava o traco */
static void uioe_one_request(UioeCtx *cx, const char *text, int trace, int explain) {
    uioe_run_ask(cx, text);
    UioeRequest *r = cx->s->cur; UioeBuf b = {0};
    if (cx->json) { uioe_render_json(&b, &r->ask, &r->cls, &r->route); if (cx->run) uioe_render_recall_json(&b, cx, r); uioe_bprintf(&b, "}\n"); }
    else { uioe_render_text(&b, &r->ask, &r->cls, &r->route); if (cx->run) uioe_render_recall_text(&b, cx, r); }
    if (b.p) fputs(b.p, stdout);
    uioe_bfree(&b);
    UioeBuf t = {0}; uioe_fmt_trace(&t, cx, r); if (cx->run) uioe_fmt_recall_trace(&t, cx, r); uioe_bprintf(&t, "\n");
    if (explain && t.p) printf("\n== explain: the decision record of this request (the same text goes to the trace); replay later with `katana uioe explain last`\n%s", t.p);
    if (trace && t.p) uioe_trace_write(t.p);
    uioe_bfree(&t);
    uioe_ctx_close(cx);
}
/* `ask --pack`: o que o laco injetaria para este pedido, sem modelo: o bloco exato em stdout (ou "no block" e a regra da porta), o resto em stderr */
static void uioe_pack_cli(UioeCtx *cx, const char *text, int stats) {
    char *block = (char *)malloc(16384); if (!block) return; UioePrep pp;
    if (!getenv("KATANA_UIOE") || !*getenv("KATANA_UIOE")) { /* o padrao e pack */ }
    uioe_pack_core(cx, text, block, 16384, &pp);
    if (pp.injected) { fputs(block, stdout); fputc('\n', stdout); }
    else {
        printf("[no block] %s\n", pp.why);
        UioeRequest *r = cx->s->cur;
        if (pp.ran && r->rc.decision == UG_LIMITS && r->rc.g) { UioePackStats st; memset(&st, 0, sizeof st); char *lf = (char *)malloc(4096);
            if (lf && ctxb_build(cx, r, &g_uioe_target_qwen, 1, lf, 4096, &st)) { printf("[the limits form, G5, that KATANA_UIOE_LIMITS=1 would inject]\n%s\n", lf); }
            free(lf); }
    }
    if (stats) {
        fprintf(stderr, "[pack] %s | %s | confidence %d after retrieval, rule G%d | total %.1f ms%s%s\n", uioe_mode() == UIOE_OFF ? "KATANA_UIOE=off: the agent would inject nothing" : "mode pack",
                pp.injected ? "INJECTED" : "not injected", pp.conf, pp.rule, pp.ms, pp.inherited ? " | inherited the open investigation" : "", pp.overtime ? " | over the total time" : "");
        if (pp.injected) fprintf(stderr, "[pack] %d bytes, about %d tokens (bytes/4; ceiling %d) | %s\n", pp.st.bytes, pp.st.tokens, cx->s->cur->route.token_ceiling, pp.st.log);
    }
    free(block); uioe_ctx_close(cx);
}
static int uioe_cmd_ask(int argc, char **argv, int run) {
    const char *text = NULL, *cwd = NULL; int json = 0, trace = 1, verify = 1, index = 0, explain = 0, record = 0, refresh = 1, pack = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--pack")) pack = 1;
        else if (!strcmp(argv[i], "--no-trace")) trace = 0;
        else if (!strcmp(argv[i], "--no-verify")) verify = 0;
        else if (!strcmp(argv[i], "--index")) index = 1;
        else if (!strcmp(argv[i], "--no-refresh")) refresh = 0;
        else if (!strcmp(argv[i], "--explain")) explain = 1;
        else if (!strcmp(argv[i], "--record")) record = 1;
        else if (!strcmp(argv[i], "--cwd") && i + 1 < argc) cwd = argv[++i];
        else if (!text) text = argv[i];
        else { fprintf(stderr, "uioe ask: unexpected argument \"%s\"\n", argv[i]); return 2; }
    }
    if (!text || !*text) { fprintf(stderr, "usage: katana uioe %s \"<request>\" [--cwd DIR] [--json] [--no-trace]%s\n", run ? "ask" : "plan", run ? " [--explain] [--no-verify] [--index] [--no-refresh] [--record] [--pack]" : ""); return 2; }
    UioeSession *s = uioe_session_new(); if (!s) return 1;
    UioeCtx cx; uioe_ctx_init(&cx, s, cwd); cx.json = json; cx.run = run; cx.verify = verify; cx.build_index = index; cx.record = record; cx.refresh = refresh;
    if (explain) cx.det = 1;
    cx.trace = trace;
    if (pack && run) { uioe_pack_cli(&cx, text, 1); uioe_session_free(s); return 0; }
    uioe_one_request(&cx, text, trace, explain);
    uioe_session_free(s);
    return 0;
}
/* `katana uioe session`: varios pedidos na mesma sessao (disjuntores e estado do pedido valem entre eles); cada um marca o que mostrou */
static int uioe_cmd_session(int argc, char **argv) {
    const char *cwd = NULL; int json = 0, trace = 1, pack = 0;
    for (int i = 0; i < argc; i++) { if (!strcmp(argv[i], "--json")) json = 1; else if (!strcmp(argv[i], "--pack")) pack = 1; else if (!strcmp(argv[i], "--no-trace")) trace = 0; else if (!strcmp(argv[i], "--cwd") && i + 1 < argc) cwd = argv[++i]; else { fprintf(stderr, "usage: katana uioe session [--cwd DIR] [--json] [--no-trace] [--pack]  (one request per line on stdin)\n"); return 2; } }
    UioeSession *s = uioe_session_new(); if (!s) return 1;
    char line[2200]; int n = 0;
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0; if (!line[0]) continue;
        UioeCtx cx; uioe_ctx_init(&cx, s, cwd); cx.json = json; cx.record = 1; cx.run = 1;
        printf("--- request %d: %.100s\n", ++n, line);
        if (pack) { cx.trace = trace; cx.record = 0; uioe_pack_cli(&cx, line, 1); continue; }
        uioe_one_request(&cx, line, trace, 0);
    }
    uioe_session_free(s);
    return 0;
}
/* ---- explain: o registro do traco, repetido na arvore de agora, com o que mudou ---- */
static int ex_cmp_line(const char *l) {         /* linhas comparaveis: as decisoes, sem os tempos (depois de " | ") */
    static const char *const pre[] = {"ask ", "route ", "step ", "signal ", "gate ", "scope ", "degrade ", "state ", "call ", "path ", "link ", "item ", "limit ", "hyp ", NULL};
    for (int i = 0; pre[i]; i++) if (!strncmp(l, pre[i], strlen(pre[i]))) return 1;
    return 0;
}
static char *ex_strip(const char *l) { char *c = strdup(l); char *bar = strstr(c, " | "); if (bar) *bar = 0; size_t n = strlen(c); while (n && (c[n - 1] == ' ' || c[n - 1] == '\n')) c[--n] = 0; return c; }
static int uioe_cmd_explain(int argc, char **argv) {
    const char *which = argc > 0 ? argv[0] : "last"; char p[1300]; uioe_trace_path(p, sizeof p);
    if (!p[0]) { fprintf(stderr, "uioe explain: the trace is off (KATANA_UIOE_LOG=0)\n"); return 1; }
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "uioe explain: no trace at %s (run `katana uioe ask` first)\n", p); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); char *all = (char *)malloc((size_t)sz + 1); if (!all) { fclose(f); return 1; }
    const size_t got = fread(all, 1, (size_t)sz, f); all[got] = 0; fclose(f);
    /* os registros comecam em "== "; escolhe o ultimo, o de numero N a partir do fim, ou o de id */
    char *rec[400]; int nr = 0; for (char *q = all; q && *q && nr < 400; ) { if (!strncmp(q, "== ", 3)) rec[nr++] = q; char *nl = strstr(q, "\n== "); q = nl ? nl + 1 : NULL; }
    if (!nr) { fprintf(stderr, "uioe explain: the trace has no record\n"); free(all); return 1; }
    int pick = nr - 1;
    if (strcmp(which, "last")) {
        if (strlen(which) <= 3 && strspn(which, "0123456789") == strlen(which) && atoi(which) > 0) { const int num = atoi(which); pick = nr - num >= 0 ? nr - num : 0; }
        else { pick = -1; char key[40]; snprintf(key, sizeof key, " req %s ", which);
            for (int i = nr - 1; i >= 0; i--) { const char *e = strchr(rec[i], '\n'); const char *h = strstr(rec[i], key); if (h && (!e || h < e)) { pick = i; break; } }
            if (pick < 0) { fprintf(stderr, "uioe explain: no record with id %s\n", which); free(all); return 1; } } }
    char *old = strndup(rec[pick], pick + 1 < nr ? (size_t)(rec[pick + 1] - rec[pick]) : strlen(rec[pick]));
    char text[2200] = "", root[1100] = "";
    { const char *t = strstr(old, "text: "); if (t) { t += 6; size_t l = strcspn(t, "\n"); if (l >= sizeof text) l = sizeof text - 1; memcpy(text, t, l); text[l] = 0; }
      const char *r = strstr(old, " root="); if (r) { r += 6; size_t l = strcspn(r, "\n"); if (l >= sizeof root) l = sizeof root - 1; memcpy(root, r, l); root[l] = 0; } }
    if (!text[0]) { fprintf(stderr, "uioe explain: the record has no request text\n"); free(all); free(old); return 1; }
    printf("== replaying the request of this record on the tree as it is now (deterministic mode: no time cuts)\n%.*s\n", (int)strcspn(old, "\n"), old);
    UioeSession *s = uioe_session_new(); if (!s) return 1;
    UioeCtx cx; uioe_ctx_init(&cx, s, root[0] ? root : NULL); cx.det = 1; cx.run = 1; cx.record = 0;
    uioe_run_ask(&cx, text);
    UioeBuf t = {0}; uioe_fmt_trace(&t, &cx, s->cur); uioe_fmt_recall_trace(&t, &cx, s->cur);
    char *ol[600], *nl2[600]; int no = 0, nn = 0;
    for (char *q = old; q && *q && no < 600; ) { char *e = strchr(q, '\n'); size_t l = e ? (size_t)(e - q) : strlen(q); char *c = strndup(q, l); const char *sp = c; while (*sp == ' ') sp++; if (ex_cmp_line(sp)) ol[no++] = ex_strip(sp); free(c); q = e ? e + 1 : NULL; }
    if (t.p) for (char *q = t.p; q && *q && nn < 600; ) { char *e = strchr(q, '\n'); size_t l = e ? (size_t)(e - q) : strlen(q); char *c = strndup(q, l); const char *sp = c; while (*sp == ' ') sp++; if (ex_cmp_line(sp)) nl2[nn++] = ex_strip(sp); free(c); q = e ? e + 1 : NULL; }
    int same = 0, diff = 0;
    for (int i = 0; i < no; i++) { int f2 = 0; for (int j = 0; j < nn && !f2; j++) if (!strcmp(ol[i], nl2[j])) f2 = 1; if (f2) same++; else { diff++; printf("  - was:  %.200s\n", ol[i]); } }
    for (int j = 0; j < nn; j++) { int f2 = 0; for (int i = 0; i < no && !f2; i++) if (!strcmp(ol[i], nl2[j])) f2 = 1; if (!f2) { diff++; printf("  + now:  %.200s\n", nl2[j]); } }
    printf("== %d decision line(s) are the same, %d differ%s\n", same, diff, diff ? " (a different rule, a connector now unavailable, a file that changed or an index that was refreshed)" : ": the decision is reproduced");
    printf("== the record as it comes out now:\n%s", t.p ? t.p : "");
    for (int i = 0; i < no; i++) free(ol[i]);
    for (int j = 0; j < nn; j++) free(nl2[j]);
    uioe_bfree(&t); uioe_ctx_close(&cx); uioe_session_free(s); free(all); free(old);
    return 0;
}
static int uioe_cmd_connectors(int argc, char **argv) {
    const char *cwd = NULL;
    for (int i = 0; i < argc; i++) { if (!strcmp(argv[i], "--cwd") && i + 1 < argc) cwd = argv[++i]; else { fprintf(stderr, "usage: katana uioe connectors [--cwd DIR]\n"); return 2; } }
    UioeSession *s = uioe_session_new(); if (!s) return 1;
    UioeCtx cx; uioe_ctx_init(&cx, s, cwd);
    printf("mode %s   scope %s\n", cx.mode == UIOE_OFF ? "off" : cx.mode == UIOE_PACK ? "pack" : "full", cx.root);
    printf("%-16s %-9s %-8s %-4s %-3s %-6s %-5s %-9s %s\n", "connector", "domain", "cost", "conf", "lvl", "ms", "built", "available", "why");
    for (int i = 0; i < s->nreg; i++) {
        const UioeConnector *c = &s->reg[i]; char w[240]; const int av = c->available(c, cx.root, w, sizeof w);
        printf("%-16s %-9s %-8s %-4d %-3d %-6d %-5s %-9s %s%s\n", c->id, c->domain, g_uioe_costname[c->cost], c->conf_typical, c->need_level, c->default_ms, c->run ? "yes" : "no",
               av == 1 ? "yes" : av == 2 ? "partial" : "no", w, s->brk[i].open ? "   [breaker OPEN]" : "");
    }
    printf("breakers: all closed (a fresh process); inside `katana uioe session` they count failures across requests\n");
    uioe_session_free(s);
    return 0;
}
static int uioe_cmd_state(void) {
    UioeSession *s = uioe_session_new(); if (!s) return 1;
    uioe_req_sync(s->cur);
    printf("# state of THIS process (a new one: no agent request is running). Inside the agent use /uioe.\n");
    uioe_state_print(stdout, s->cur);
    uioe_session_free(s);
    return 0;
}
static int uioe_main(int argc, char **argv) {
    if (argc < 1 || !strcmp(argv[0], "help") || !strcmp(argv[0], "--help")) {
        printf("katana uioe: the front door before the model (docs/uioe.md). U0, U1, U3 and U4 are built; nothing here runs a model.\n"
               "  ask \"<request>\" [--cwd DIR] [--json] [--no-trace] [--explain] [--no-verify] [--index] [--no-refresh] [--record] [--pack]\n"
               "                                                       --pack: print ONLY what the agent would inject before the model (the context block, or \"[no block]\" and the rule of the door)\n"
               "                                                       interpret, classify, route AND run the route: prints the understood request, the plan and the\n"
               "                                                       retrieved evidence (items with file:lines, confidence, verified or not, what was not found, the limits)\n"
               "  plan \"<request>\" [--cwd DIR] [--json]                only the understood request and the plan: nothing is executed\n"
               "  explain [last|N|ID]                                  replay a request of the trace on the tree as it is now and mark what changed\n"
               "  session [--cwd DIR] [--json]                         one request per stdin line, in the same session (breakers, request state)\n"
               "  connectors [--cwd DIR]                               the registry: what each source answers and whether it is available here\n"
               "  state                                                the per-request state (inside the agent: /uioe)\n"
               "KATANA_UIOE=off|pack|full (default pack: the agent calls the door before the model; off is the escape)   trace: KATANA_UIOE_LOG (default ~/.katana/uioe.log, 0 = off)\n"
               "KATANA_UIOE_MS (request deadline, 400)   KATANA_UIOE_GATE (G4 threshold, 70)   KATANA_UIOE_DET=1 (no time cuts)   KATANA_UIOE_FAULT=connector=timeout|error|partial,... (test hook)\n");
        return argc < 1 ? 2 : 0;
    }
    if (!strcmp(argv[0], "ask")) return uioe_cmd_ask(argc - 1, argv + 1, 1);
    if (!strcmp(argv[0], "plan")) return uioe_cmd_ask(argc - 1, argv + 1, 0);
    if (!strcmp(argv[0], "explain")) return uioe_cmd_explain(argc - 1, argv + 1);
    if (!strcmp(argv[0], "session")) return uioe_cmd_session(argc - 1, argv + 1);
    if (!strcmp(argv[0], "connectors")) return uioe_cmd_connectors(argc - 1, argv + 1);
    if (!strcmp(argv[0], "state")) return uioe_cmd_state();
    fprintf(stderr, "uioe: unknown subcommand \"%s\" (ask, plan, explain, session, connectors, state)\n", argv[0]);
    return 2;
}
