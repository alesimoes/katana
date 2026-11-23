/* ---------- uioe/orch.c: Recuperar e Orquestrar (docs/uioe.md 3.6, 3.7.1 e 3.5.3, fase U3) ----------
 * A maquina de estados de um pedido, so para frente (com duas voltas limitadas, abaixo):
 *   P0 UNDERSTAND  interpretar, classificar e roteirizar (U1, ja feito pelo chamador); G1 a G3 encerram sem recuperar
 *   P1 SCOPE       a raiz, o armazenamento do indice (index_ensure so se ja existe), UMA SyView para o pedido, o nivel do indice
 *   P2 RECALL      o estado do pedido (o que o Find e o Read ja mostraram), tirado das globais pelo instantaneo de U0
 *   P3 ANCHOR      as entidades viram nos (faixa 1); sem entidade, a busca por palavras
 *   P4 EXPAND      o pipeline do tipo: ligacoes, chamadores, impacto, cadeia
 *   P5 CONFIRM     reler as linhas que vao aparecer (verify.c); uma volta a P4 se a verificacao derrubou o caminho inteiro
 *   P6 GAPS        o que se queria e nao se achou, o que nao se olhou e por que
 *   P7 SELECT      ordem final (faixa, RRF, relevancia), caminho final, confianca depois de recuperar e a porta G4 a G6
 *   P8 EMIT        registro no traco e (so com --record, depois do Construtor de Contexto de U4) o estado do pedido
 * Voltas: P4 -> P3 UMA vez quando a expansao nao achou aresta a partir de nenhuma ancora (o fluxo pega os proximos candidatos da busca);
 * P5 -> P4 UMA vez quando a verificacao derrubou todos os passos do caminho (a ligacao e marcada e a cadeia refeita sem ela).
 * Prazos: o do PEDIDO (KATANA_UIOE_MS, 400 ms) conta so a recuperacao, nao o index_ensure (3.5.2); estourou = salta para P6 com o que tem.
 * Decisoes do implementador:
 *   - a confianca depois de recuperar e RECALCULADA do zero com os sinais de 3.5.3 (nao reaproveita a de U1, que contava a pasta citada como
 *     entidade reconhecida): so entidade resolvida pelo indice (cascata exata ou derivada) soma +20. O "gap" (termos informativos ausentes
 *     do indice) continua sem ser calculado: a API do coder nao expoe a frequencia de documento do termo.
 *   - sem indice (degrau 5) nao se constroi um indice: o UIOE nao tem efeitos colaterais no disco. `--index` constroi antes (index_ensure).
 *   - item "mostrado/lido" vem de rs_cover (as faixas que o Find e o Read ja entregaram neste processo); a CLI de um pedido so ve o que um
 *     pedido anterior da MESMA sessao (`katana uioe session`) marcou com --record. */
static void uioe_req_sync(UioeRequest *r);
/* ---- P1: escopo ---- */
static void orch_scope(UioeCtx *cx, UioeRecall *rc, const UioeAsk *a) {
    char why[240] = ""; const double t0 = now_sec();
    cx->has_store = uioe_index_present(cx->root, why, sizeof why);
    if (!cx->has_store && cx->build_index) { IndexRes ir; if (index_ensure(cx->root, 2, "", 0, &ir) == 0) { cx->has_store = uioe_index_present(cx->root, why, sizeof why); rc_degrade(rc, "the index was built for this request (--index), %.0f ms", ir.ms); } }
    if (cx->has_store && cx->refresh) { IndexRes ir; index_ensure(cx->root, 2, "", 0, &ir); if (ir.walked && !ir.created) rc_degrade(rc, "the index was refreshed before the search (%.0f ms): files had changed since it was built", ir.ms); }
    rc->ms_index = (now_sec() - t0) * 1000.0;
    cx->idx_mode = 0;
    if (!cx->has_store) { rc->idx_mode = 0; snprintf(rc->scope, sizeof rc->scope, "no index store for %s: the request is answered by scanning files (degradation 5)", cx->root); rc_degrade(rc, "no index store: scanning files with K = 3 (degradation 5); nothing was indexed by this request"); return; }
    if (!uioe_open_view(cx)) { rc->idx_mode = 0; snprintf(rc->scope, sizeof rc->scope, "the index of %s cannot be read (%.150s): scanning files (degradation 5)", cx->root, cx->view_note); rc_degrade(rc, "the index could not be read: scanning files (degradation 5)"); return; }
    const SyView *v = &cx->view;
    if (cx->have_sym && cx->have_keys && v->nkeyold == 0) { rc->idx_mode = cx->idx_mode = 2; snprintf(rc->scope, sizeof rc->scope, "index complete: %u files (%u with symbols, %u analysed for text keys)", v->nlive, v->nwith, v->nkeyflag); }
    else {
        rc->idx_mode = cx->idx_mode = 1; size_t n = (size_t)snprintf(rc->scope, sizeof rc->scope, "index partial: %u files", v->nlive);
        if (!cx->have_sym) { n += (size_t)snprintf(rc->scope + n, sizeof rc->scope - n, "; no symbol sections"); rc_degrade(rc, "partial index: no symbols, so definitions and callers fall back to the search channel and file scans (degradation 4)"); }
        if (!cx->have_keys) { n += (size_t)snprintf(rc->scope + n, sizeof rc->scope - n, "; no text-key sections"); rc_degrade(rc, "partial index: no text keys, so connections through strings are skipped (degradation 3)"); }
        if (v->nkeyold) { n += (size_t)snprintf(rc->scope + n, sizeof rc->scope - n, "; %u files were indexed before text keys existed", v->nkeyold); rc_limit(rc, "%u indexed files have no text-key data (indexed before keys existed): links through strings may be missing", v->nkeyold); }
    }
    (void)a;
}
/* ---- relevancia dos nos que nao sao de ligacao ---- */
static void orch_relevance(UioeOrch *o) {
    for (int i = 0; i < o->g->nn; i++) {
        UioeNode *n = &o->g->n[i]; char hay[700]; snprintf(hay, sizeof hay, "%s %s %s %s", n->name, n->sig, n->text, po_base(n->path));
        int rel = n->relevance + 10 * ug_terms_in(o->a, hay); if (n->anchor) rel += 10;
        if (po_is_test(o, n->path)) rel -= 15; if (po_is_doc(n->path) && !(o->qflags & 1) && o->k->kind != UI_HOWTO && o->k->kind != UI_CONFIG) rel -= 8;
        n->relevance = rel < 0 ? 0 : rel > 100 ? 100 : rel;
    }
}
static void orch_mark_seen(UioeOrch *o) {
    UioeRequest *r = o->cx->s->cur; uioe_req_sync(r);
    for (int i = 0; i < o->g->nn; i++) {
        UioeNode *n = &o->g->n[i]; if (!n->path[0] || n->kind == UN_FOLDER) continue;
        char abs[2700]; snprintf(abs, sizeof abs, "%s/%s", o->cx->root, n->path); int how = 0;
        const int a = n->line0 > 0 ? n->line0 : 1, b = n->line1 >= a ? n->line1 : a; const int cov = rs_cover(abs, a, b, &how);
        if (cov > 0 && (b - a + 1) <= cov * 2) { if (how & 1) n->shown = 1; if (how & 2) n->read = 1; }
    }
}
/* quais nos vao ser mostrados e (por isso) verificados: o caminho primeiro, depois os itens por ordem provisoria */
static int orch_select_items(UioeOrch *o, int *ids, int max) {
    UioeGraph *g = o->g; int n = 0;
    for (int i = 0; i < g->nn; i++) {
        const UioeNode *x = &g->n[i]; if (x->dropped || x->onpath) continue;
        if (x->kind == UN_FILE) { int has = 0; for (int j = 0; j < g->ne; j++) if (g->e[j].kind == UE_CONTAINS && g->e[j].from == i && !g->n[g->e[j].to].dropped) { has = 1; break; } if (has) continue; }
        if (x->kind == UN_FILE && !strcmp(x->name, "file") && x->tier >= 2 && o->k->kind != UI_OVERVIEW && !x->anchor) continue;     /* o lado "arquivo" de um import, so contexto */
        if (x->kind == UN_KEY && o->k->kind == UI_FLOW && x->relevance < 36 && !x->anchor) continue;       /* chave que nao liga nada ao que foi pedido */
        if (x->kind == UN_RANGE && x->tier >= 4 && x->relevance < 10 && o->k->kind != UI_ERROR && o->k->kind != UI_USAGE && o->k->kind != UI_LOCATE) continue;
        if (n < 200) ids[n++] = i;
    }
    ug_sort(g, ids, n); o->rc->nitems_all = n; return n < max ? n : max;
}
/* aresta de ligacao verificada quando os dois lados foram */
static void orch_verify_edges(UioeOrch *o) {
    UioeGraph *g = o->g;
    for (int i = 0; i < g->ne; i++) {
        UioeEdge *e = &g->e[i]; const UioeNode *A = &g->n[e->from], *B = &g->n[e->to];
        if (e->kind == UE_LINKS || e->kind == UE_VIA || e->kind == UE_IMPORTS) {
            const int va = A->ver == UV_OK || A->ver == UV_MOVED, vb = B->ver == UV_OK || B->ver == UV_MOVED;
            if (A->ver == UV_NONE || B->ver == UV_NONE) { e->ver = UV_NONE; continue; }
            if (va && vb) { e->ver = UV_OK; if (e->kind == UE_LINKS && !strstr(e->how, "verified")) { e->conf += 4; if (e->conf > 99) e->conf = 99; } }
            else if (A->ver == UV_SKIPPED || B->ver == UV_SKIPPED) e->ver = UV_SKIPPED;
            else { e->ver = UV_STALE; if (e->kind == UE_LINKS) { e->conf -= 25; if (e->conf < 1) e->conf = 1; } }
        }
    }
}
/* o caminho final: corta no primeiro salto cujos dois lados nao foram relidos com sucesso */
static void orch_finalize_path(UioeOrch *o) {
    UioeRecall *rc = o->rc; UioeGraph *g = o->g; int keep = rc->npath;
    for (int i = 0; i < rc->npath; i++) { const UioeNode *n = &g->n[rc->path[i]]; if (n->ver == UV_STALE || n->ver == UV_GONE || n->ver == UV_UNREADABLE) { keep = i; break; } }
    if (!o->cx->verify) keep = rc->npath;
    for (int i = keep; i < rc->npath; i++) g->n[rc->path[i]].onpath = 0;
    rc->npath = keep < 2 ? 0 : keep;
    if (!rc->npath) for (int i = 0; i < 8; i++) { /* nada no caminho */ }
}
/* ---- a confianca depois de recuperar e a porta G4 a G6 (3.5.3) ---- */
static void orch_gate(UioeOrch *o) {
    UioeRecall *rc = o->rc; UioeGraph *g = o->g; UioeRoute *rt = o->rt; const UioeClass *k = o->k;
    int conf = 50; rc->nsig = 0;
#define GS(...) do { if (rc->nsig < 10) snprintf(rc->sig[rc->nsig++], sizeof rc->sig[0], __VA_ARGS__); } while (0)
    GS("start 50");
    if (k->sure) { conf += 15; GS("+15 the task kind matched a strong cue (R%d)", k->rule_kind); }
    if (o->entity_hit) { conf += 20; GS("+20 a named entity was resolved by the index at the exact or derived step"); }
    if ((k->domain[0] == UD_CODE || k->domain[0] == UD_FILES) && rc->idx_mode == 2) { conf += 10; GS("+10 the scope is indexed completely (symbols and text keys)"); }
    else if (rc->idx_mode == 1) GS("+0 the index is partial: %s", rc->scope);
    else if (rc->idx_mode == 0) GS("+0 no index for this scope (files were scanned)");
    if (o->req_missing) { const int d = o->req_missing > 2 ? 20 : o->req_missing * 10; conf -= d; GS("-%d %d connector(s) the pipeline needs were unavailable or timed out", d, o->req_missing); }
    /* depois de recuperar */
    int strong = 0, any45 = 0, onlyword = 1, agree = 0, nfact = 0;
    for (int i = 0; i < g->nn; i++) { const UioeNode *n = &g->n[i]; if (n->dropped || n->kind == UN_FILE) continue;
        if (n->kind == UN_FACT) { nfact++; continue; }      /* U8: a memoria e lembranca nao verificada: nunca conta como achado de codigo, nem a favor nem contra */
        const int ok = n->ver == UV_OK || n->ver == UV_MOVED;
        if (n->conf >= 45 && n->ver != UV_STALE) any45 = 1;
        if (k->kind != UI_FLOW && n->tier <= 2 && ok && n->conf >= 75 && (n->anchor || n->tier == 1)) strong = 1;
        if (n->tier <= 3) onlyword = 0;
        if (n->nsrc >= 2 && ok) agree = 1; }
    if (k->kind == UI_FLOW) for (int i = 0; i + 1 < rc->npath; i++) if (rc->pathedge[i] >= 0) { const UioeEdge *e = &g->e[rc->pathedge[i]]; if ((e->kind == UE_LINKS || e->kind == UE_CALLS) && e->ver == UV_OK && e->conf >= 75) strong = 1; }
    for (int i = 0; i < g->ne; i++) { const UioeEdge *e = &g->e[i]; if (k->kind != UI_FLOW && e->kind == UE_CALLS && e->ver == UV_OK && e->conf >= 75) strong = 1; if (e->kind == UE_MENTIONS) agree = 1; }
    for (int i = 0; i < g->nn; i++) if (g->n[i].kind == UN_DOCLINE && !g->n[i].dropped && g->n[i].relevance >= 20 && rc->npath >= 2) agree = 1;
    int docfinding = 0;       /* como instalar, visao geral: a resposta esta em documentos; uma linha de documento relida, com dois termos ou mais, e o achado (nao ha estrutura de codigo a seguir) */
    if ((k->kind == UI_HOWTO || k->kind == UI_OVERVIEW) && o->cx->verify) for (int i = 0; i < g->nn; i++) { const UioeNode *n = &g->n[i]; if (n->kind == UN_DOCLINE && !n->dropped && (n->ver == UV_OK || n->ver == UV_MOVED) && n->conf >= 55 && n->tier <= 3) docfinding = 1; }
    const int memfinding = k->kind == UI_HISTORY && nfact > 0;      /* U8: o achado de um pedido de historico e uma nota salva, datada e de baixa confianca */
    if (memfinding) { conf += 10; GS("+10 a saved note matched the request (the owner's memory: dated, low confidence, not verified against the code)"); }
    if (!o->cx->verify) strong = 0;
    if (strong && o->structural) { conf += 20; GS("+20 a verified structural finding with confidence >= 75 (reread in the files)"); }
    else if (docfinding) GS("+0 a document line matching two or more terms was reread in the file: the finding of a how-to request is a document, not a code structure");
    if (agree) { conf += 10; GS("+10 independent sources agree (a document names the files of the chain, or two connectors returned the same item)"); }
    if ((!any45 || onlyword) && !docfinding && !memfinding) { conf -= 25; GS("-25 %s", !any45 ? "nothing above confidence 45 was found" : "only candidates ranked by words were found, none reached by structure"); }
    const int gate = uioe_env_int("KATANA_UIOE_GATE", 70, 1, 100);
    if (conf >= gate && !(strong && o->structural) && !o->entity_hit && !docfinding && !memfinding) { conf = gate - 1; GS("cap at %d: no verified structural finding with confidence >= 75 and no entity resolved by the index, so a packet would rest on word ranking alone (it never passes G4)", conf); }
    if (conf < 0) conf = 0; if (conf > 100) conf = 100;
#undef GS
    rc->conf_after = conf; rt->confidence = conf;
    int nres = 0; for (int i = 0; i < g->nn; i++) if (!g->n[i].dropped) nres++;
    int authoritative = 0; const char *why = "";
    if (k->retrieve == 2) { authoritative = 1; why = "a cited place was located"; }
    else if (o->entity_miss && rc->idx_mode == 2) { authoritative = 1; why = "a named thing does not exist in a complete scope"; }
    else if (rc->idx_mode < 2 && (k->domain[0] == UD_CODE || k->domain[0] == UD_FILES)) { authoritative = 1; why = "the index is partial or absent, and the model should know what to use meanwhile"; }
    else if (o->a->place_note) { authoritative = 1; why = "a cited place was located"; }
    else if (k->kind == UI_HISTORY) { authoritative = 1; why = "nothing saved in memory matches the request"; }
    else if (k->kind == UI_WEB) { authoritative = 1; why = "the source this request needs is not installed in this build"; }
    if (k->retrieve == 2) { rt->decision = UG_LIMITS; rt->rule = 5; snprintf(rt->decision_note, sizeof rt->decision_note, "G5: confidence %d; %s: only Question understood as, limits and Next would be told", conf, why); }
    else if (conf >= gate) { rt->decision = UG_PACKET; rt->rule = 4; snprintf(rt->decision_note, sizeof rt->decision_note, "G4: confidence %d >= %d after retrieval (%d node(s), path of %d step(s)): the full block", conf, gate, nres, rc->npath); }
    else if (conf >= 40 && authoritative) { rt->decision = UG_LIMITS; rt->rule = 5; snprintf(rt->decision_note, sizeof rt->decision_note, "G5: confidence %d (40 to %d) and there is something authoritative to say (%s): the limits form only", conf, gate - 1, why); }
    else { rt->decision = UG_NONE; rt->rule = 6;
        if (authoritative) snprintf(rt->decision_note, sizeof rt->decision_note, "G6: confidence %d is below 40, so even the limits form (G5: %.90s) is not told: no packet; the model works as today", conf, why);
        else snprintf(rt->decision_note, sizeof rt->decision_note, "G6: confidence %d < %d and nothing authoritative to say: no packet; the model works as today", conf, gate); }
    rc->decision = rt->decision; rc->rule = rt->rule; snprintf(rc->decision_note, sizeof rc->decision_note, "%s", rt->decision_note);
}
/* "Next": a sugestao concreta com criterio de parada (U4 a coloca no bloco; aqui so aparece no resultado) */
static void orch_next(UioeOrch *o) {
    UioeRecall *rc = o->rc; UioeGraph *g = o->g; rc->next[0] = 0;
    if (rc->npath >= 2) { const UioeNode *n = &g->n[rc->path[rc->npath - 1]]; int a = n->line0, b = n->line1 > n->line0 ? n->line1 : n->line0 + 12; if (b - a > 60) b = a + 60;
        snprintf(rc->next, sizeof rc->next, "Read %.100s lines %d-%d, then answer. Do not search again for what is listed above.", n->path, a, b); return; }
    for (int i = 0; i < rc->nitems; i++) { const UioeNode *n = &g->n[rc->items[i]]; if (n->kind == UN_SYMBOL || n->kind == UN_RANGE || n->kind == UN_KEY || n->kind == UN_DOCLINE) {
        int a = n->line0, b = n->line1 > n->line0 ? n->line1 : n->line0 + 12; if (b - a > 60) b = a + 60;
        snprintf(rc->next, sizeof rc->next, "Read %.100s lines %d-%d, then answer. Do not search again for what is listed above.", n->path, a, b); return; } }
    for (int i = 0; i < rc->nitems; i++) if (g->n[rc->items[i]].kind == UN_FACT) { snprintf(rc->next, sizeof rc->next, "Answer from the saved notes, saying they are old notes of the owner, not checked against the code."); return; }
    snprintf(rc->next, sizeof rc->next, "Answer without retrieved context, or ask the user for a name, a file or a folder to look at.");
}
/* ---- P6: o que nao se achou e o que nao se olhou ---- */
static void orch_gaps(UioeOrch *o) {
    UioeRecall *rc = o->rc; UioeGraph *g = o->g;
    if (o->cx->view_state == 1 && rc->idx_mode >= 1 && rc->idx_mode == 2 && o->cx->view.nlive) rc_limit(rc, "scope: %.100s (the search covers only that set; another project or an ignored folder is not seen)", rc->scope);
    else if (rc->idx_mode < 2) rc_limit(rc, "scope: %.180s", rc->scope);
    if (o->rt->notrouted[0]) { char t[300]; snprintf(t, sizeof t, "%s", o->rt->notrouted); size_t l = strlen(t); while (l && (t[l - 1] == ' ' || t[l - 1] == ';')) t[--l] = 0; rc_limit(rc, "not consulted: %.200s", t); }
    /* pontas abertas das ligacoes por chave: ninguem trata / ninguem envia */
    int nopen = 0;
    for (int i = 0; i < g->nn && nopen < 3; i++) { const UioeNode *n = &g->n[i]; if (n->kind == UN_KEY && n->conf == 0 && strstr(n->how, "nobody") && (n->onpath || n->relevance >= 36)) { rc_hyp(rc, "\"%s\" at %s:%d: %s", n->name, n->path, n->line0, strstr(n->how, "nobody") ? strstr(n->how, "nobody") : ""); nopen++; } }
    for (int i = 0; i < rc->nbranch; i++) { const UioeEdge *e = &g->e[rc->branch[i]]; const UioeNode *t = &g->n[e->to]; rc_hyp(rc, "not followed: %s -> %s:%d %s (conf %d)", e->kind == UE_VIA ? "importer" : e->kind == UE_CALLS ? "call" : e->key, t->path, t->line0, t->name, e->conf); }
    if (g->lost_n || g->lost_e) rc_limit(rc, "the evidence graph reached its size limit: %d node(s) and %d edge(s) were left out", g->lost_n, g->lost_e);
    if (rc->nnotreread) rc_limit(rc, "%d item(s) were not reread (verification budget: 40 ranges, 150 ms%s): shown as unverified", rc->nnotreread, o->cx->verify ? "" : "; --no-verify");
    if (rc->nstale) rc_limit(rc, "%d item(s) failed the reread: the file changed since it was indexed; they are shown with the confidence lowered by 25", rc->nstale);
    int gone = 0; for (int i = 0; i < g->nn; i++) if (g->n[i].ver == UV_GONE || g->n[i].ver == UV_UNREADABLE) gone++;
    if (gone) rc_limit(rc, "%d item(s) were dropped: the file no longer exists or cannot be read", gone);
    if (rc->cut) rc_limit(rc, "%s", rc->cut_note);
}

/* ---- a maquina ---- */
static void uioe_orchestrate(UioeCtx *cx, const UioeAsk *a, const UioeClass *k, UioeRoute *rt, UioeRecall *rc) {
    UioeGraph *keepg = rc->g; if (!keepg) keepg = (UioeGraph *)calloc(1, sizeof *keepg); else memset(keepg, 0, sizeof *keepg);
    memset(rc, 0, sizeof *rc); rc->g = keepg; if (!keepg) return;
    rc->ran = 1;
    for (int i = 0; i < rt->nstep; i++) { UioeStep *s = &rt->step[i]; s->ran = 0; s->rms = 0; s->rnodes = 0; s->rcalls = 0; s->rstatus = 0; s->rnote[0] = 0; }
    rc_state(rc, "P0 UNDERSTAND  kind %s (R%d), %s, retrieve %d (C%d), route confidence %d, gate before: %s", g_uioe_kindname[k->kind], k->rule_kind, g_uioe_domname[k->domain[0]], k->retrieve, k->rule_retrieve, rt->confidence, rt->rule ? rt->decision_note : "passes G1 to G3");
    if (rt->rule && rt->rule <= 3) { rc->decision = rt->decision; rc->rule = rt->rule; snprintf(rc->decision_note, sizeof rc->decision_note, "%s", rt->decision_note); rc->ran = 0; rc_state(rc, "P8 EMIT        no retrieval: %s", rt->decision_note); return; }
    UioeOrch *o = (UioeOrch *)calloc(1, sizeof *o); if (!o) return;
    o->cx = cx; o->rc = rc; o->g = keepg; o->a = a; o->k = k; o->rt = rt; o->st = "P1"; o->qflags = fd_qflags(a->text);
    uioe_breaker_begin(cx->s);
    /* P1 SCOPE */
    if (k->kind == UI_HISTORY) { cx->has_store = 0; cx->idx_mode = rc->idx_mode = 0; rc->ms_index = 0; snprintf(rc->scope, sizeof rc->scope, "the owner's saved memory only: no code index and no files were consulted"); }      /* U8: pergunta de historico nao toca o indice */
    else orch_scope(cx, rc, a);
    cx->t0 = now_sec();
    rc_state(rc, "P1 SCOPE        %s; index step %.1f ms (outside the request clock)", rc->scope, rc->ms_index);
    /* P2 RECALL */
    { UioeRequest *r = cx->s->cur; uioe_req_sync(r); rc_state(rc, "P2 RECALL       the request state shows %d range(s) pointed at by Find and %d read by Read", r->nshown, r->nread); }
    /* P3 + P4 */
    o->st = "P3"; po_pipeline(o);
    rc_state(rc, "P3 ANCHOR       %d anchor node(s), %d anchor file(s); entity resolved by the index: %s", o->nanch, o->naf, o->entity_hit ? "yes" : "no");
    rc_state(rc, "P4 EXPAND       %d connector call(s), %d node(s), %d edge(s), path of %d step(s)", rc->nex, keepg->nn, keepg->ne, rc->npath);
    /* volta P4 -> P3: nenhuma ancora levou a uma ligacao (so o fluxo, uma vez) */
    if (k->kind == UI_FLOW && !rc->npath && !uioe_over(cx) && o->naf > 0 && k->retrieve == 1 && rc->idx_mode >= 1) {
        rc_state(rc, "P4 -> P3        no edge from any anchor: taking the next candidates of the search (once)");
        o->st = "P3"; const int before = o->naf; const int n = po_where(o, "where", 12, 0); (void)n;
        o->st = "P4"; for (int i = before > 14 ? before : 14; i < o->naf && i < 16 && !uioe_over(cx); i++) { int e0, e1; po_file_links(o, "links", keepg->n[o->afile[i]].path, 0, 2, &e0, &e1); }
        po_chain(o); rc_state(rc, "P3 ANCHOR       second pass: %d anchor file(s); path of %d step(s)", o->naf, rc->npath);
    }
    if (uioe_over(cx)) { rc->cut = 1; snprintf(rc->cut_note, sizeof rc->cut_note, "the request deadline (%d ms) passed: the stages after it ran with what was already retrieved", cx->deadline_ms); rc_state(rc, "P4 -> P6        deadline %d ms expired after %.1f ms", cx->deadline_ms, uioe_elapsed_ms(cx)); }
    /* P5 CONFIRM */
    orch_relevance(o); orch_mark_seen(o);
    int ids[160]; int nids = 0, tries = 0;
    for (;;) {
        nids = 0; for (int i = 0; i < rc->npath; i++) ids[nids++] = rc->path[i];
        for (int i = 0; i < rc->nbranch; i++) ids[nids++] = keepg->e[rc->branch[i]].to;                 /* os ramos e as ligacoes irmas tambem sao citados: relidos tambem */
        if (rc->npath >= 2) { const UioeNode *P = &keepg->n[rc->path[0]], *C = &keepg->n[rc->path[1]]; int sib = 0;
            for (int i = 0; i < keepg->ne && sib < 3; i++) { const UioeEdge *e = &keepg->e[i]; if (e->kind != UE_LINKS || i == rc->pathedge[0]) continue;
                if (strcmp(keepg->n[e->from].path, P->path) || strcmp(keepg->n[e->to].path, C->path)) continue; ids[nids++] = e->from; ids[nids++] = e->to; sib++; } }
        int items[200]; const int ni = orch_select_items(o, items, 24); for (int i = 0; i < ni && nids < 150; i++) ids[nids++] = items[i];
        if (cx->verify) { const double tv = now_sec(); uioe_verify_list(cx, keepg, ids, nids, rc); rc->ms_verify += (now_sec() - tv) * 1000.0; }
        else for (int i = 0; i < nids; i++) rc->nnotreread++;
        orch_verify_edges(o);
        int stale = 0; for (int i = 0; i < rc->npath; i++) { const UioeNode *n = &keepg->n[rc->path[i]]; if (n->ver == UV_STALE || n->ver == UV_GONE || n->ver == UV_UNREADABLE) stale = 1; }
        if (!stale || tries++ || k->kind != UI_FLOW || !cx->verify) break;
        /* volta P5 -> P4: a verificacao derrubou o caminho; marca as ligacoes e refaz a cadeia sem elas */
        int dropped_all = 1; for (int i = 0; i + 1 < rc->npath; i++) { const UioeNode *A = &keepg->n[rc->path[i]], *B = &keepg->n[rc->path[i + 1]]; if ((A->ver == UV_OK || A->ver == UV_MOVED) && (B->ver == UV_OK || B->ver == UV_MOVED)) dropped_all = 0; }
        if (!dropped_all) break;
        rc_state(rc, "P5 -> P4        verification dropped every step of the path: marking the link and rebuilding the chain (once)");
        for (int i = 0; i < rc->npath; i++) if (rc->pathedge[i] >= 0 && o->nbad < 8) o->bad[o->nbad++] = rc->pathedge[i];
        for (int i = 0; i < rc->npath; i++) keepg->n[rc->path[i]].onpath = 0;
        rc->npath = 0; o->st = "P4"; po_chain(o); orch_relevance(o);
    }
    rc_state(rc, "P5 CONFIRM      %d range(s) reread: %d verified (%d moved), %d stale, %d not reread", nids - rc->nnotreread, rc->nverified, rc->nmoved, rc->nstale, rc->nnotreread);
    orch_finalize_path(o);
    { const int vi = po_step_find(rt, "verify"); if (vi >= 0 && !rt->step[vi].ran) { UioeStep *s = &rt->step[vi]; s->ran = 1; s->rcalls = 1; s->rnodes = nids - rc->nnotreread; s->rms = rc->ms_verify; s->rstatus = cx->verify ? UR_OK : UR_EMPTY; snprintf(s->rconn, sizeof s->rconn, "verify.c"); snprintf(s->rnote, sizeof s->rnote, "%s", cx->verify ? "the lines were reread in the files by verify.c (file.windows is its primitive)" : "--no-verify: nothing was reread"); } }
    /* P6 GAPS e P7 SELECT */
    orch_gaps(o);
    rc_state(rc, "P6 GAPS         %d limit clause(s), %d open hypothesis(es)", rc->nlim, rc->nhyp);
    { int items[200]; rc->nitems = orch_select_items(o, items, 24); memcpy(rc->items, items, sizeof(int) * (size_t)rc->nitems); }
    orch_gate(o); orch_next(o);
    rc_state(rc, "P7 SELECT       %d item(s), path of %d step(s); confidence after retrieval %d", rc->nitems, rc->npath, rc->conf_after);
    /* P8 EMIT: so o registro; o bloco de contexto e de U4 */
    if (cx->record) { for (int i = 0; i < rc->npath; i++) { const UioeNode *n = &keepg->n[rc->path[i]]; char abs[2700]; snprintf(abs, sizeof abs, "%s/%s", cx->root, n->path); rs_note(abs, n->line0, n->line1 > n->line0 ? n->line1 : n->line0, 1); }
        for (int i = 0; i < rc->nitems && i < 12; i++) { const UioeNode *n = &keepg->n[rc->items[i]]; if (!n->path[0] || n->kind == UN_FOLDER) continue; char abs[2700]; snprintf(abs, sizeof abs, "%s/%s", cx->root, n->path); rs_note(abs, n->line0, n->line1 > n->line0 ? n->line1 : n->line0, 1); } }
    rc->ms = uioe_elapsed_ms(cx);
    rc_state(rc, "P8 EMIT         decision %s%s", rc->decision_note, cx->record ? "; the shown ranges were recorded in the request state" : "");
    free(o);
    uv_free(cx);
}
