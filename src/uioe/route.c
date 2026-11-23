/* ---------- uioe/route.c: Roteirizar (docs/uioe.md 3.5 e 3.7.2, fase U1) ----------
 * (pedido interpretado, classificacao) -> plano de rota: o pipeline pela tabela (dominio, tipo), os passos com a capacidade e o conector
 * escolhido (o mais barato disponivel; sem ele o passo cai na escada de degradacao ou fica "nao rodado" com o motivo), o orcamento, a
 * confianca de rota ANTES da recuperacao (3.5.3) e as regras da porta G1 a G3 (G4 a G6 dependem do que foi achado: so em U3).
 * NADA aqui executa um conector: o plano so descreve o que rodaria. Os numeros dos pesos sao os provisorios do documento.
 * Decisoes do implementador:
 *   - "entidade resolvida" (+20) em U1 quer dizer reconhecida pela forma ou achada no disco (confianca >= 70), porque o indice nao e
 *     consultado; o sinal diz "by shape/disk, not index-confirmed" para o dono nao ler como confirmacao do indice.
 *   - sem o termo "gap" (precisa do indice), o sinal de -20 por vocabulario ausente nao existe aqui; o plano diz "gap n/a".
 *   - G3 (ambiguo) decide UG_NONE; C5 (so um lugar a localizar) decide UG_LIMITS pela regra G5, como diz 3.10.2.
 *   - o teto de tokens do bloco e o do tipo (3.7.2) limitado por KATANA_UIOE_TOKENS (padrao 1500, maximo 3000). */
static int ui_ceiling(UioeKind k, int change) {
    int c = k == UI_LOCATE ? 500 : k == UI_USAGE ? 600 : k == UI_IMPACT ? 1200 : k == UI_FLOW ? 1500 : k == UI_CONFIG ? 600 : k == UI_ERROR ? 1500 :
            k == UI_HOWTO ? 700 : k == UI_HISTORY ? 400 : k == UI_WEB ? 300 : k == UI_OVERVIEW ? 400 : k == UI_FINDINGS ? 1000 : 0;
    if (change && c) c = 1500;
    return c;
}
static int ui_step_ix(const UioeRoute *rt, const char *label) { for (int i = 0; i < rt->nstep; i++) if (!strcmp(rt->step[i].label, label)) return i; return -1; }
static int uioe_find_conn(const UioeSession *s, const char *id) { for (int i = 0; i < s->nreg; i++) if (!strcmp(s->reg[i].id, id)) return i; return -1; }

/* acrescenta um passo e escolhe o conector: o preferido se disponivel, senao o mais barato com a capacidade, senao a degradacao */
static UioeStep *ui_step(UioeCtx *cx, UioeRoute *rt, const char *label, uint32_t cap, const char *pref, const char *input, int dep) {
    if (rt->nstep >= 24) return NULL;
    UioeStep *st = &rt->step[rt->nstep]; memset(st, 0, sizeof *st);
    st->n = ++rt->nstep; snprintf(st->label, sizeof st->label, "%s", label); st->cap = cap; snprintf(st->input, sizeof st->input, "%s", input); st->dep = dep;
    UioeSession *s = cx->s; char why[200]; why[0] = 0;
    if (!cap) { snprintf(st->conn, sizeof st->conn, "(orchestrate)"); snprintf(st->note, sizeof st->note, "local, no connector"); return st; }
    int ix = -1;
    if (pref) { const int p = uioe_find_conn(s, pref); if (p >= 0) { char w[200]; if (s->reg[p].available(&s->reg[p], cx->root, w, sizeof w) && !s->brk[p].open) { ix = p; snprintf(why, sizeof why, "%s", w); } else snprintf(why, sizeof why, "%.60s: %.130s", pref, w); } }
    if (ix < 0) { int any; char w[200]; ix = uioe_pick(s, cap, cx->root, w, sizeof w, &any);
        if (ix >= 0) { if (pref && why[0]) snprintf(st->note, sizeof st->note, "%.60s; fell back", why); else snprintf(why, sizeof why, "%s", w); }
        else if (cap & (UQ_WHERE | UQ_LITERAL | UQ_DOC)) { ix = uioe_find_conn(s, "file.grep"); snprintf(st->note, sizeof st->note, "no index: scanning files (degradation 5)"); }
        else if (any >= 0) { snprintf(st->note, sizeof st->note, "%s: %.100s", s->reg[any].id, w); snprintf(st->conn, sizeof st->conn, "%s", s->reg[any].id); st->status = (s->reg[any].flags & UCF_LEAVES_MACHINE) ? 2 : 1; return st; } }
    if (ix >= 0) snprintf(st->conn, sizeof st->conn, "%s", s->reg[ix].id);
    else { st->status = 1; snprintf(st->note, sizeof st->note, "no connector available"); }
    return st;
}
/* "a, b, c" com os textos das entidades de certos tipos (ou de todas se mask == 0) */
static void ui_ents_str(const UioeAsk *a, unsigned mask, char *out, size_t cap) {
    out[0] = 0; size_t o = 0;
    for (int i = 0; i < a->nent; i++) { if (mask && !(mask & (1u << a->ent[i].kind))) continue; o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? ", " : "", a->ent[i].text); if (o + 60 > cap) break; }
}
static void ui_terms_str(const UioeAsk *a, char *out, size_t cap) {
    out[0] = 0; size_t o = 0;
    for (int i = 0; i < a->nterms; i++) { o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? " " : "", a->norm[i]); if (o + 50 > cap) break; }
}

static void uioe_route(UioeCtx *cx, const UioeAsk *a, const UioeClass *k, UioeRoute *rt) {
    memset(rt, 0, sizeof *rt);
    rt->deadline_ms = uioe_env_int("KATANA_UIOE_MS", 400, 20, 20000);
    const int tmax = uioe_env_int("KATANA_UIOE_TOKENS", 1500, 200, 3000);
    const int ceil_kind = ui_ceiling(k->kind, k->change); rt->token_ceiling = ceil_kind < tmax ? ceil_kind : tmax;
    snprintf(rt->pipeline, sizeof rt->pipeline, "%s/%s", g_uioe_domname[k->domain[0]], g_uioe_kindname[k->kind]);
    char es[400], ts[400], sym[300]; ui_ents_str(a, (1u << UK_SYMBOL) | (1u << UK_PATH) | (1u << UK_KEY) | (1u << UK_ENV) | (1u << UK_LITERAL) | (1u << UK_ERRTEXT), es, sizeof es); ui_terms_str(a, ts, sizeof ts);
    ui_ents_str(a, (1u << UK_SYMBOL) | (1u << UK_KEY) | (1u << UK_ENV), sym, sizeof sym);
    const int hascode = sym[0] != 0 || ui_has_ent(a, UK_PATH) || ui_has_ent(a, UK_LITERAL) || ui_has_ent(a, UK_ERRTEXT), hasent = hascode;
    char buf[300];
    if (k->retrieve == 0) goto gate;
    if (k->retrieve == 2) { ui_step(cx, rt, "place", UQ_PLACE, "place.resolve", a->nplaces ? a->places[0] : "(the cited folder)", 0); goto gate; }
    switch (k->kind) {
    case UI_LOCATE:
        if (hasent) ui_step(cx, rt, "anchor", UQ_DEF, "code.def", es, 0); else { snprintf(buf, sizeof buf, "%s k=5", ts); ui_step(cx, rt, "where", UQ_WHERE, "code.search", buf, 0); }
        ui_step(cx, rt, "callers", UQ_CALLERS, "code.callers", "count and first caller of the top 2 anchors", 1);
        break;
    case UI_USAGE:
        ui_step(cx, rt, "def", UQ_DEF, "code.def", hascode ? sym : es, 0);
        if (a->rel & UREL_CALLEES) ui_step(cx, rt, "callees", UQ_CALLEES, "code.callees", sym, 1);
        else if (a->rel & UREL_USES) ui_step(cx, rt, "uses", UQ_USES, "code.refs", sym, 1);
        else ui_step(cx, rt, "callers", UQ_CALLERS, "code.callers", sym, 1);
        if (ui_has_ent(a, UK_PATH)) ui_step(cx, rt, "imports", UQ_IMPORTS, "code.deps", "who imports the file", 0);
        ui_step(cx, rt, "literal", UQ_LITERAL, "code.search", "if there is no definition: the name as a literal, then key links", 1);
        break;
    case UI_IMPACT:
        ui_step(cx, rt, "def", UQ_DEF, "code.def", hascode ? sym : es, 0);
        ui_step(cx, rt, "impact", UQ_IMPACT, "code.impact", "depth 3, at most 25 nodes", 1);
        ui_step(cx, rt, "links", UQ_LINKS, "code.flow", "keys sent or handled by the definition and their other ends", 1);
        ui_step(cx, rt, "imports", UQ_IMPORTS, "code.deps", "importers of the file when the symbol is file-level", 1);
        ui_step(cx, rt, "tests", UQ_CALLERS, "code.callers", "callers in test files, listed apart", 1);
        break;
    case UI_FLOW:
        if (hasent) ui_step(cx, rt, "anchors", UQ_DEF, "code.def", es, 0);
        else { snprintf(buf, sizeof buf, "%s k=8, code only", ts); ui_step(cx, rt, "where", UQ_WHERE, "code.search", buf, 0); }
        ui_step(cx, rt, "links", UQ_LINKS, "code.flow", "keys of each anchor file (top 5), links to other files", 1);
        ui_step(cx, rt, "callees", UQ_CALLEES, "code.callees", "anchor symbols (top 3) depth 2; callers depth 1", 1);
        ui_step(cx, rt, "chain", 0, NULL, a->dir[0] ? (strcmp(a->dir, "send") == 0 ? "follow the best link forward up to 4 hops (direction: send)" : "follow the best link up to 4 hops (direction: receive)") : "follow the best link up to 4 hops", 2);
        ui_step(cx, rt, "doc", UQ_DOC, "docs.select", "lines that name two or more files of the chain", 4);
        break;
    case UI_CONFIG:
        if (ui_has_ent(a, UK_ENV)) ui_step(cx, rt, "key", UQ_CONFIG, "code.flow", sym, 0);
        else { snprintf(buf, sizeof buf, "%s + config vocabulary, config files only", ts); ui_step(cx, rt, "where", UQ_WHERE, "code.search", buf, 0); ui_step(cx, rt, "key", UQ_CONFIG, "code.flow", "keys of the config files found", 1); }
        ui_step(cx, rt, "doc", UQ_DOC, "docs.search", "the line that documents the option", 0);
        break;
    case UI_ERROR: {
        const UioeEntity *lit = NULL; for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == UK_ERRTEXT || a->ent[i].kind == UK_LITERAL) { lit = &a->ent[i]; break; }
        ui_step(cx, rt, "literal", UQ_LITERAL, "code.search", lit ? lit->text : ts, 0);
        ui_step(cx, rt, "at", UQ_CALLERS, "code.callers", "enclosing symbol, its guard lines, callers depth 2", 1);
        ui_step(cx, rt, "facts", UQ_FACT, "session.facts", "last failing call and files changed in this session", 0);
        ui_step(cx, rt, "history", UQ_HISTORY, "history.search", "what solved it in earlier sessions", 0);
        break; }
    case UI_HOWTO:
        ui_step(cx, rt, "docs", UQ_DOC, "docs.select", ts, 0);
        ui_step(cx, rt, "docsearch", UQ_DOC, "docs.search", ts, 0);
        ui_step(cx, rt, "recipes", UQ_RECIPE, "howto.recipes", ts, 0);
        ui_step(cx, rt, "skill", UQ_RECIPE, "skill.match", ts, 0);
        break;
    case UI_HISTORY:
        ui_step(cx, rt, "facts", UQ_FACT, "session.facts", ts, 0);
        ui_step(cx, rt, "memory", UQ_FACT, "memory.recall", ts, 0);
        ui_step(cx, rt, "history", UQ_HISTORY, "history.search", ts, 0);
        break;
    case UI_WEB:
        ui_step(cx, rt, "docs", UQ_DOC, "docs.search", ts, 0);
        ui_step(cx, rt, "web", UQ_EXTERNAL, "web.search", ts, 0);
        break;
    case UI_OVERVIEW:
        ui_step(cx, rt, "tree", UQ_STRUCTURE, "file.glob", "the folder listing", 0);
        ui_step(cx, rt, "docs", UQ_DOC, "docs.select", "title lines of README or KATANA.md", 0);
        ui_step(cx, rt, "deps", UQ_IMPORTS, "code.deps", "most imported files per top folder", 0);
        break;
    default: break;
    }
    /* U8: a memoria so entra com pista de historico, preferencia ou decisao anterior (o classificador marca k->memcue); pergunta so de codigo nunca a consulta.
       No tipo history o passo ja existe acima; nos outros tipos e um passo a mais, com o motivo da rota na nota (aparece em `uioe ask --explain`). */
    if (k->retrieve == 1 && k->memcue && ui_step_ix(rt, "memory") < 0) { UioeStep *ms = ui_step(cx, rt, "memory", UQ_FACT, "memory.recall", ts, 0); if (ms) snprintf(ms->note, sizeof ms->note, "routed: %.120s", k->why_memory); }
    else if (k->retrieve == 1 && k->kind == UI_HISTORY) { const int mi = ui_step_ix(rt, "memory"); if (mi >= 0) snprintf(rt->step[mi].note, sizeof rt->step[mi].note, "routed: %.120s", k->why_memory); }
    if (rt->nstep && k->retrieve == 1 && (k->domain[0] == UD_CODE || k->domain[0] == UD_FILES) && k->kind != UI_HOWTO && k->kind != UI_HISTORY && k->kind != UI_WEB)
        ui_step(cx, rt, "verify", UQ_VERIFY, "file.windows", "reread every line that will be shown (40 ranges, 150 ms)", 0);
gate:
    /* ---- fontes que nao entram ---- */
    { size_t o = 0; rt->notrouted[0] = 0;
      int web = 0, mem = 0, mcp = 1; for (int i = 0; i < rt->nstep; i++) { if (!strncmp(rt->step[i].conn, "web.", 4)) web = 1; if (!strncmp(rt->step[i].conn, "memory.", 7) || !strncmp(rt->step[i].conn, "history.", 8)) mem = 1; }
      if (!web) o += (size_t)snprintf(rt->notrouted + o, sizeof rt->notrouted - o, "web (%s); ", k->domain[0] == UD_RESEARCH ? "policy: KATANA_UIOE_WEB=0" : "not needed for this kind of request");
      if (!mem) o += (size_t)snprintf(rt->notrouted + o, sizeof rt->notrouted - o, "memory and earlier sessions (no memory cue); ");
      if (mcp) o += (size_t)snprintf(rt->notrouted + o, sizeof rt->notrouted - o, "mcp (no connector card installed)"); }
    /* ---- confianca antes da recuperacao (3.5.3) ---- */
    int conf = 50; rt->nsig = 0;
#define SIG(...) do { if (rt->nsig < 8) snprintf(rt->signals[rt->nsig++], sizeof rt->signals[0], __VA_ARGS__); } while (0)
    SIG("start 50");
    if (k->sure) { conf += 15; SIG("+15 the task kind matched a strong cue (R%d)", k->rule_kind); }
    { const UioeEntity *best = NULL; for (int i = 0; i < a->nent; i++) { const int kd = a->ent[i].kind; if (kd == UK_NAME || kd == UK_URL || kd == UK_DATE || kd == UK_AMOUNT || kd == UK_CONCEPT) continue; if (a->ent[i].conf >= 70 && (!best || a->ent[i].conf > best->conf)) best = &a->ent[i]; }
      if (best) { conf += 20; SIG("+20 entity \"%.60s\" recognized (%s; by shape or on disk, not confirmed by the index)", best->text, g_uioe_entname[(int)best->kind]); } }
    if ((k->domain[0] == UD_CODE || k->domain[0] == UD_FILES) && k->retrieve) {
        if (a->index_ready) { conf += 10; SIG("+10 the scope has an index store (level not checked)"); } else SIG("+0 no index store for the scope: scanning fallback");
    }
    if (a->ambiguous) { conf -= 25; SIG("-25 ambiguous: %.70s", a->amb); }
    { int miss = 0; for (int i = 0; i < rt->nstep; i++) if (rt->step[i].status == 1 && strcmp(rt->step[i].label, "verify") != 0 && rt->step[i].cap != UQ_FACT && rt->step[i].cap != UQ_HISTORY) miss++;
      if (miss) { const int d = miss > 2 ? 20 : miss * 10; conf -= d; SIG("-%d %d step(s) have no available connector", d, miss); } }
    SIG("gap n/a (the indexes are not consulted before U3)");
#undef SIG
    if (conf < 0) conf = 0;
    if (conf > 100) conf = 100;
    rt->confidence = conf;
    if (k->retrieve == 0) { rt->confidence = -1; rt->nsig = 0; }      /* nada a recuperar: a confianca nao se aplica */
    /* ---- porta: G1 a G3 aqui; G4 a G6 so depois de recuperar ---- */
    const char *tgt = rt->nstep ? "" : "";
    (void)tgt;
    if (k->retrieve == 0) { rt->decision = UG_NONE; rt->rule = 1; snprintf(rt->decision_note, sizeof rt->decision_note, "G1: retrieve = 0 (C%d): no retrieval, no packet, state untouched", k->rule_retrieve); }
    else if (conf < 35) { rt->decision = UG_NONE; rt->rule = 2; snprintf(rt->decision_note, sizeof rt->decision_note, "G2: confidence %d < 35: no retrieval, no packet", conf); }
    else if (a->ambiguous) { rt->decision = UG_NONE; rt->rule = 3; snprintf(rt->decision_note, sizeof rt->decision_note, "G3: ambiguous request: no packet (the model may ask: %.80s)", a->amb); }
    else if (k->retrieve == 2) { rt->decision = UG_LIMITS; rt->rule = 5; snprintf(rt->decision_note, sizeof rt->decision_note, "G5: only the cited place is told (Question understood as, limits, Next); no search"); }
    else { rt->decision = UG_NONE; rt->rule = 0; snprintf(rt->decision_note, sizeof rt->decision_note, "passes G1 to G3; G4 (>= %d after retrieval), G5 and G6 are decided after retrieval (run `katana uioe ask`; `plan` does not execute the route)", uioe_env_int("KATANA_UIOE_GATE", 70, 1, 100)); }
    /* ---- o que sairia da maquina ---- */
    rt->sends_out[0] = 0;
    for (int i = 0; i < rt->nstep; i++) if (rt->step[i].conn[0] && !rt->step[i].status) { const int ix = uioe_find_conn(cx->s, rt->step[i].conn); if (ix >= 0 && (cx->s->reg[ix].flags & UCF_LEAVES_MACHINE)) snprintf(rt->sends_out, sizeof rt->sends_out, "terms to %s: %.60s", rt->step[i].conn, rt->step[i].input); }
}
