/* ---------- uioe/registry.c: o registro de conectores (docs/uioe.md 3.8.2, fase U1) ----------
 * Em U1 o registro so DESCREVE o que existe e responde "esta disponivel nesta pasta e por que" (nada roda: run e nulo, entra em U3).
 * Os conectores do documento estao todos aqui, os de dentro do processo e os que ainda nao existem (mcp:<servidor>, model.expand,
 * lsp.confirm), para o plano de rota poder dizer "nao roteado" com o motivo. Nao escreve em disco e nao abre o indice: so olha se os
 * arquivos existem. A disponibilidade da politica (web) segue 3.8.6: desligada ate KATANA_UIOE_WEB=1. */
static int uioe_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
/* ha um armazenamento de indice do coder para a raiz? (so existencia do MANIFEST; nao diz o nivel nem se esta fresco) */
static int uioe_index_present(const char *root, char *why, size_t cap) {
#ifndef _WIN32
    IndexLoc L; index_resolve(root, 0, &L);
    char m[4600]; snprintf(m, sizeof m, "%s/MANIFEST", L.store);
    if (uioe_exists(m)) { if (why) snprintf(why, cap, "index store exists (%s root); level and freshness not checked", L.init ? "init" : "home"); return 1; }
    if (why) snprintf(why, cap, "no index store yet (the first Find or Symbols builds it)");
    return 0;
#else
    (void)root; if (why) snprintf(why, cap, "the code index is not built on this platform"); return 0;
#endif
}
static int uioe_av_index(const UioeConnector *s, const char *root, char *why, size_t cap) { (void)s; return uioe_index_present(root, why, cap); }
static int uioe_av_always(const UioeConnector *s, const char *root, char *why, size_t cap) { (void)s; (void)root; snprintf(why, cap, "always (reads files on demand)"); return 1; }
static int uioe_av_docs(const UioeConnector *s, const char *root, char *why, size_t cap) {
    (void)s; char p[1300];
    snprintf(p, sizeof p, "%s/KATANA.md", root); if (uioe_exists(p)) { snprintf(why, cap, "KATANA.md found"); return 1; }
    snprintf(p, sizeof p, "%s/AGENTS.md", root); if (uioe_exists(p)) { snprintf(why, cap, "AGENTS.md found"); return 1; }
    snprintf(why, cap, "no KATANA.md or AGENTS.md in %s", root); return 0;
}
static int uioe_av_memory(const UioeConnector *s, const char *root, char *why, size_t cap) {
    (void)s; (void)root; const char *e = getenv("KATANA_MEM"); char p[1300];
    if (e && *e) snprintf(p, sizeof p, "%s", e); else { char h[1024]; agent_home(h, sizeof h); snprintf(p, sizeof p, "%s/.katana/memoria.txt", h); }
    if (uioe_exists(p)) { snprintf(why, cap, "memory file exists"); return 1; }
    snprintf(why, cap, "no memory file yet"); return 0;
}
static int uioe_av_history(const UioeConnector *s, const char *root, char *why, size_t cap) {
    (void)s; (void)root; char h[1024], p[1300]; agent_home(h, sizeof h); snprintf(p, sizeof p, "%s/.katana/knowledge", h);
    if (uioe_exists(p)) { snprintf(why, cap, "session notes exist"); return 1; }
    snprintf(why, cap, "no session notes (katana knowledge build)"); return 0;
}
static int uioe_av_session(const UioeConnector *s, const char *root, char *why, size_t cap) { (void)s; (void)root; snprintf(why, cap, "only inside an agent session"); return 0; }
static int uioe_av_skill(const UioeConnector *s, const char *root, char *why, size_t cap) {
    (void)s; char h[1024], p[1300]; agent_home(h, sizeof h);
    snprintf(p, sizeof p, "%s/.katana/skills", root); if (uioe_exists(p)) { snprintf(why, cap, "project skills folder exists"); return 1; }
    snprintf(p, sizeof p, "%s/.katana/skills", h); if (uioe_exists(p)) { snprintf(why, cap, "user skills folder exists"); return 1; }
    snprintf(why, cap, "built-in recipes only"); return 1;
}
static int uioe_av_web(const UioeConnector *s, const char *root, char *why, size_t cap) {
    (void)s; (void)root; const char *w = getenv("KATANA_WEB"), *u = getenv("KATANA_UIOE_WEB");
    if (w && !strcmp(w, "0")) { snprintf(why, cap, "KATANA_WEB=0"); return 0; }
    if (!(u && !strcmp(u, "1"))) { snprintf(why, cap, "not routed pre-LLM: KATANA_UIOE_WEB is not 1 (policy 3.8.6); the model may still call it"); return 0; }
    snprintf(why, cap, "KATANA_UIOE_WEB=1"); return 1;
}
static int uioe_av_none(const UioeConnector *s, const char *root, char *why, size_t cap) {
    (void)root;
    if (!strncmp(s->id, "mcp:", 4)) snprintf(why, cap, "no connector card installed (stage 4)");
    else if (!strcmp(s->id, "model.expand")) snprintf(why, cap, "off by default (KATANA_UIOE_EXPAND=0)");
    else snprintf(why, cap, "off by default");
    return 0;
}

/* registra um conector (a tabela e da sessao, nao global); devolve o indice ou -1 */
static int uioe_register(UioeSession *s, const UioeConnector *c) {
    if (s->nreg >= UIOE_MAXCONN || !c->id || !c->available) return -1;
    s->reg[s->nreg] = *c; return s->nreg++;
}
static void uioe_registry_init(UioeSession *s) {
    s->nreg = 0;
#define C(id, ans, cost, conf, lvl, fl, ms, dom, av) { id, ans, cost, conf, lvl, fl, ms, "1", dom, av, NULL }
    static const UioeConnector tab[] = {
        C("code.search",  UQ_WHERE | UQ_LITERAL,                       UC_INDEX,   60, 1, UCF_READONLY, 50,  "code",   uioe_av_index),
        C("code.rank",    UQ_WHERE,                                    UC_INDEX,   35, 1, UCF_READONLY, 50,  "code",   uioe_av_index),
        C("code.def",     UQ_DEF,                                      UC_INDEX,   92, 2, UCF_READONLY, 50,  "code",   uioe_av_index),
        C("code.refs",    UQ_USES,                                     UC_INDEX,   62, 2, UCF_READONLY, 50,  "code",   uioe_av_index),
        C("code.callers", UQ_CALLERS,                                  UC_INDEX,   62, 2, UCF_READONLY, 50,  "code",   uioe_av_index),
        C("code.callees", UQ_CALLEES,                                  UC_INDEX,   62, 2, UCF_READONLY, 50,  "code",   uioe_av_index),
        C("code.outline", UQ_STRUCTURE,                                UC_INDEX,   92, 2, UCF_READONLY, 50,  "code",   uioe_av_index),
        C("code.deps",    UQ_IMPORTS,                                  UC_INDEX,   75, 2, UCF_READONLY, 50,  "code",   uioe_av_index),
        C("code.impact",  UQ_IMPACT,                                   UC_INDEX,   55, 2, UCF_READONLY, 50,  "code",   uioe_av_index),
        C("code.flow",    UQ_LINKS | UQ_CONFIG,                        UC_INDEX,   80, 3, UCF_READONLY, 50,  "code",   uioe_av_index),
        C("docs.select",  UQ_DOC,                                      UC_FILES,   70, -1, UCF_READONLY, 300, "files", uioe_av_docs),
        C("docs.search",  UQ_DOC,                                      UC_INDEX,   55, 1, UCF_READONLY, 50,  "files",  uioe_av_index),
        C("file.windows", UQ_VERIFY,                                   UC_FILES,   80, -1, UCF_READONLY, 300, "files", uioe_av_always),
        C("file.grep",    UQ_LITERAL,                                  UC_FILES,   90, -1, UCF_READONLY, 300, "files", uioe_av_always),
        C("file.glob",    UQ_STRUCTURE,                                UC_FILES,   90, -1, UCF_READONLY, 300, "files", uioe_av_always),
        C("place.resolve", UQ_PLACE,                                   UC_FILES,   80, -1, UCF_READONLY, 300, "files", uioe_av_always),
        C("memory.recall", UQ_FACT,                                    UC_INSTANT, 60, -1, UCF_READONLY, 5,   "memory", uioe_av_memory),
        C("history.search", UQ_HISTORY,                                UC_FILES,   45, -1, UCF_READONLY, 300, "memory", uioe_av_history),
        C("session.facts", UQ_FACT,                                    UC_INSTANT, 85, -1, UCF_READONLY, 5,   "memory", uioe_av_session),
        C("howto.recipes", UQ_RECIPE,                                     UC_INSTANT, 60, -1, UCF_READONLY, 5,   "system", uioe_av_always),
        C("skill.match",  UQ_RECIPE,                                      UC_INSTANT, 50, -1, UCF_READONLY, 5,   "system", uioe_av_skill),
        C("web.search",   UQ_EXTERNAL,                                 UC_NETWORK, 40, -1, UCF_READONLY | UCF_LEAVES_MACHINE | UCF_UNTRUSTED, 8000, "research", uioe_av_web),
        C("web.fetch",    UQ_EXTERNAL,                                 UC_NETWORK, 40, -1, UCF_READONLY | UCF_LEAVES_MACHINE | UCF_UNTRUSTED, 8000, "research", uioe_av_web),
        C("mcp:<server>", UQ_EXTERNAL,                                 UC_PROCESS, 70, -1, UCF_READONLY | UCF_UNTRUSTED, 3000, "data",     uioe_av_none),
        C("model.expand", UQ_WHERE,                                    UC_MODEL,    0, -1, UCF_NEEDS_MODEL, 6000, "code",     uioe_av_none),
        C("lsp.confirm",  UQ_VERIFY,                                   UC_PROCESS, 95, -1, UCF_READONLY, 3000, "code",     uioe_av_none),
    };
#undef C
    for (size_t i = 0; i < sizeof tab / sizeof *tab; i++) uioe_register(s, &tab[i]);
    memset(s->brk, 0, sizeof s->brk);
}
/* o conector mais barato e disponivel que responde a capacidade; -1 se nenhum; *any = existe algum que a declara (mesmo indisponivel) */
static int uioe_pick(UioeSession *s, uint32_t cap, const char *root, char *why, size_t wcap, int *any) {
    int best = -1; if (any) *any = -1;
    for (int i = 0; i < s->nreg; i++) {
        const UioeConnector *c = &s->reg[i];
        if (!(c->answers & cap)) continue;
        char w[200]; const int av = c->available(c, root, w, sizeof w);
        if (any && *any < 0) { *any = i; if (why) snprintf(why, wcap, "%s", w); }
        if (av && !s->brk[i].open && (best < 0 || c->cost < s->reg[best].cost)) { best = i; if (why) snprintf(why, wcap, "%s", w); }
    }
    return best;
}
