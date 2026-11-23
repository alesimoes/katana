/* ---------- uioe/cli.c: `katana uioe` (docs/uioe.md 3.11.1, fases U0 e U1) ----------
 *   katana uioe ask "<pedido>" [--cwd DIR] [--json] [--no-trace]   interpreta, classifica e roteiriza; NAO executa nada e NAO usa modelo
 *   katana uioe plan "<pedido>"                                      o mesmo (o bloco de contexto so existe a partir de U3)
 *   katana uioe connectors [--cwd DIR]                               o registro: capacidades, custo, disponibilidade nesta pasta, disjuntor
 *   katana uioe state                                                 o estado do pedido (num processo novo nao ha pedido: use /uioe no agente)
 * Cada `ask` grava um registro no traco (~/.katana/uioe.log; --no-trace ou KATANA_UIOE_LOG=0 desligam). O laco do agente nao chama o UIOE. */
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
static void ui_flags_str(unsigned v, const char *const *names, int n, char *out, size_t cap) {
    out[0] = 0; size_t o = 0;
    for (int i = 0; i < n; i++) if (v & (1u << i)) o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? "," : "", names[i]);
    if (!o) snprintf(out, cap, "none");
}
static const char *const g_ui_relnames[] = {"callers", "defined", "uses", "send", "receive", "work/fail", "negation", "callees"};
static const char *const g_ui_risknames[] = {"changes-files", "runs-commands", "leaves-machine", "sensitive", "destructive"};
static const char *const g_ui_decname[] = {"no packet", "limits only", "packet"};

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
    uioe_bprintf(b, "],\"note\":"); ui_json_str(b, rt->decision_note); uioe_bprintf(b, "}}\n");
}

static void uioe_ctx_init(UioeCtx *c, UioeSession *s, const char *cwd_override) {
    memset(c, 0, sizeof *c); c->s = s; c->now = time(NULL); c->mode = uioe_mode();
    if (cwd_override && *cwd_override) { char rp[4200]; if (realpath(cwd_override, rp)) snprintf(c->cwd, sizeof c->cwd, "%.1090s", rp); else snprintf(c->cwd, sizeof c->cwd, "%.1090s", cwd_override); }
    else if (!getcwd(c->cwd, sizeof c->cwd)) snprintf(c->cwd, sizeof c->cwd, ".");
    snprintf(c->root, sizeof c->root, "%.1090s", c->cwd);
    c->deadline_ms = uioe_env_int("KATANA_UIOE_MS", 400, 20, 20000); c->token_ceiling = uioe_env_int("KATANA_UIOE_TOKENS", 1500, 200, 3000);
    c->trace = 1;
}
static UioeSession *uioe_session_new(void) {
    UioeSession *s = (UioeSession *)calloc(1, sizeof *s); if (!s) return NULL;
    s->cur = (UioeRequest *)calloc(1, sizeof *s->cur);
    if (!s->cur) { free(s); return NULL; }
    uioe_registry_init(s);
    return s;
}
static void uioe_session_free(UioeSession *s) { if (!s) return; free(s->cur); free(s); }

/* o pipeline inteiro (U1) sobre um texto; devolve o UioeRequest da sessao preenchido */
static void uioe_run_ask(UioeCtx *cx, const char *text) {
    UioeRequest *r = cx->s->cur;
    r->id = uioe_new_id(cx->s);
    uioe_interpret(cx, text, &r->ask);
    uioe_classify(cx, &r->ask, &r->cls);
    snprintf(cx->root, sizeof cx->root, "%s", r->ask.root);       /* a rota enxerga a pasta achada, nao so a de trabalho */
    uioe_route(cx, &r->ask, &r->cls, &r->route);
    r->cls.change = r->cls.change;
}
static int uioe_cmd_ask(int argc, char **argv) {
    const char *text = NULL, *cwd = NULL; int json = 0, trace = 1;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--no-trace")) trace = 0;
        else if (!strcmp(argv[i], "--cwd") && i + 1 < argc) cwd = argv[++i];
        else if (!text) text = argv[i];
        else { fprintf(stderr, "uioe ask: unexpected argument \"%s\"\n", argv[i]); return 2; }
    }
    if (!text || !*text) { fprintf(stderr, "usage: katana uioe ask \"<request>\" [--cwd DIR] [--json] [--no-trace]\n"); return 2; }
    UioeSession *s = uioe_session_new(); if (!s) return 1;
    UioeCtx cx; uioe_ctx_init(&cx, s, cwd); cx.json = json;
    uioe_run_ask(&cx, text);
    UioeRequest *r = s->cur; UioeBuf b = {0};
    if (json) uioe_render_json(&b, &r->ask, &r->cls, &r->route); else uioe_render_text(&b, &r->ask, &r->cls, &r->route);
    if (b.p) fputs(b.p, stdout);
    uioe_bfree(&b);
    if (trace) { UioeBuf t = {0}; uioe_fmt_trace(&t, &cx, r); if (t.p) uioe_trace_write(t.p); uioe_bfree(&t); }
    uioe_session_free(s);
    return 0;
}
static int uioe_cmd_connectors(int argc, char **argv) {
    const char *cwd = NULL;
    for (int i = 0; i < argc; i++) { if (!strcmp(argv[i], "--cwd") && i + 1 < argc) cwd = argv[++i]; else { fprintf(stderr, "usage: katana uioe connectors [--cwd DIR]\n"); return 2; } }
    UioeSession *s = uioe_session_new(); if (!s) return 1;
    UioeCtx cx; uioe_ctx_init(&cx, s, cwd);
    printf("mode %s   scope %s\n", cx.mode == UIOE_OFF ? "off" : cx.mode == UIOE_PACK ? "pack" : "full", cx.root);
    printf("%-16s %-9s %-8s %-4s %-3s %-6s %-9s %s\n", "connector", "domain", "cost", "conf", "lvl", "ms", "available", "why");
    for (int i = 0; i < s->nreg; i++) {
        const UioeConnector *c = &s->reg[i]; char w[240]; const int av = c->available(c, cx.root, w, sizeof w);
        printf("%-16s %-9s %-8s %-4d %-3d %-6d %-9s %s%s\n", c->id, c->domain, g_uioe_costname[c->cost], c->conf_typical, c->need_level, c->default_ms,
               av == 1 ? "yes" : av == 2 ? "partial" : "no", w, s->brk[i].open ? "   [breaker OPEN]" : "");
    }
    printf("breakers: all closed (a fresh process; the failure counters arrive with U3)\n");
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
        printf("katana uioe: the front door before the model (docs/uioe.md). U0 and U1 are built; nothing here runs a model or a connector.\n"
               "  ask \"<request>\" [--cwd DIR] [--json] [--no-trace]   interpret, classify and route a request; prints the understood request and the route plan\n"
               "  plan \"<request>\" ...                                same as ask\n"
               "  connectors [--cwd DIR]                              the registry: what each source answers and whether it is available here\n"
               "  state                                               the per-request state (inside the agent: /uioe)\n"
               "KATANA_UIOE=off|pack|full (default off; the agent loop does not call UIOE yet)   trace: KATANA_UIOE_LOG (default ~/.katana/uioe.log, 0 = off)\n");
        return argc < 1 ? 2 : 0;
    }
    if (!strcmp(argv[0], "ask") || !strcmp(argv[0], "plan")) return uioe_cmd_ask(argc - 1, argv + 1);
    if (!strcmp(argv[0], "connectors")) return uioe_cmd_connectors(argc - 1, argv + 1);
    if (!strcmp(argv[0], "state")) return uioe_cmd_state();
    fprintf(stderr, "uioe: unknown subcommand \"%s\" (ask, plan, connectors, state)\n", argv[0]);
    return 2;
}
