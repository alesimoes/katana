/* ---------- curriculo do agente de codigo com a lista enxuta de ferramentas ----------
 * Sem versao no nome: o curriculo e atualizado no lugar e regerado do zero. Tudo na mesma maquina de scenario.c (projeto de verdade, toda
 * chamada passa pelo despacho do motor, `curriculum check` refaz a conversa byte a byte). Seis familias, escritas sobre os projetos de
 * sete linguagens do scenario_v7.c (X7N, x7_*), com as 12 ferramentas declaradas: Bash, Read, Edit, Write, Find, Symbols, Grep, TodoWrite,
 * AskUserQuestion, WebSearch, WebFetch, ToolSearch. Nao ha Glob: arquivo por nome e Find (ou Bash) e listar pasta e Symbols tree.
 *   x-nav         navegar com Symbols e Find: refs, uses, tree, flow (chaves de texto), a regra "sabe o nome exato? Symbols; so palavras? Find;
 *                 texto exato? Grep" em situacoes pareadas, Find seguido de Symbols.
 *   x-ctx         o bloco `[katana: context for this request ...]` depois do texto do usuario (o motor o monta de verdade com uioe_prepare em
 *                 modo deterministico): conferir, seguir o `Next`, completar o que o caminho nao cobre, memoria como evidencia datada de baixa
 *                 confianca; e o mesmo pedido quando o bloco NAO aparece (a conversa continua igual, so com as ferramentas).
 *   x-ctxlimits   `Not found / limits`: nome que nao existe junto de um que existe (nao repetir a busca), pasta ignorada (usar `path`).
 *   x-notes       pasta citada (nota do motor ou bloco), repeticao, arquivo grande lido em partes.
 *   x-plan        plano com TodoWrite: `done` antes da lista (erro e recuperacao), lista e passos com Symbols.
 *   x-multi       conversas de varios pedidos (pt e en): onde esta, quem chama, muda e confere.
 * Cada situacao funciona com e sem bloco: a politica olha o que o motor devolveu. Uma situacao que o motor nao confirma e descartada. */

static void x7c_lang(void) { g_cu_lang = cu_pct(35) ? 1 : 0; }

/* ---- o bloco: a mesma chamada do laco (loop.c), depois do texto do usuario; sem bloco, a nota de pasta citada ---- */
static UioeSession *g_x7c_sess;
static const char *g_x7c_blk = "";          /* o bloco do pedido atual; "" = nao houve */
/* o indice ja existe quando o usuario pergunta: o agente real o monta em segundo plano ao entrar na pasta (autoindex_dir); aqui espera terminar */
static void x7c_index(const char *dir) { if (system(cu_fmt("cd '%s' && KATANA_INDEX_SETTLE_MS=20 katana coder crawl . --quiet >/dev/null 2>&1", dir)) != 0) cu_die("o indice da pasta nao foi montado"); }
static void x7c_begin(void) { if (g_x7c_sess) { uioe_session_free(g_x7c_sess); g_x7c_sess = NULL; } g_x7c_blk = ""; }
static int x7c_request(const char *t) {
    x7_user(t);
    XMsg *m = &g_x.m[g_x.n - 1];
    g_x7c_blk = "";
    setenv("KATANA_UIOE", "pack", 1); setenv("KATANA_UIOE_DET", "1", 1);
    if (!g_x7c_sess) g_x7c_sess = uioe_session_new();
    static char blk[10000]; UioePrep pp; memset(&pp, 0, sizeof pp); blk[0] = 0;
    if (g_x7c_sess && uioe_prepare(g_x7c_sess, NULL, m->text, blk, sizeof blk, &pp) && strstr(blk, "[katana: context for this request") && strstr(blk, "[end of context]")) {
        g_x7c_blk = cu_fmt("%s", blk);
        m->text = cu_fmt("%s\n\n%s", m->text, blk);
        uioe_commit(g_x7c_sess);            /* o que o bloco mostrou conta como mostrado para o Read */
        return 1;
    }
    static char pn[2400];
    if (place_note(m->text, pn, sizeof pn)) m->text = cu_fmt("%s\n\n%s", m->text, pn);
    return 0;
}
/* `Next: Read FILE lines a-b, ...`: faz a leitura que o bloco manda; 1 se leu */
static int x7c_next_read(const Scn *s, const char **out) {
    const char *nx = strstr(g_x7c_blk, "\nNext: Read "); if (!nx) return 0;
    nx += 12;
    char file[300]; const size_t k = strcspn(nx, " "); int a = 0, b = 0;
    if (!k || k >= sizeof file) return 0;
    memcpy(file, nx, k); file[k] = 0;
    if (sscanf(nx + k, " lines %d-%d", &a, &b) != 2 || a < 1 || b < a) return 0;
    *out = x7_read(s, file, a, b - a + 1, 0);
    return 1;
}
static int x7c_has(const char *needle) { return g_x7c_blk[0] && strstr(g_x7c_blk, needle) != NULL; }
/* funcao com pelo menos `minc` chamadores */
static int x7c_pick(const X7N *n, int minc, int needcalls) {
    int cs[16];
    for (int t = 0; t < 300; t++) { const int i = cu_n(n->nf); if (x7n_callers(n, i, cs) >= minc && (!needcalls || n->f[i].nc)) return i; }
    cu_die("sem funcao com %d chamadores", minc); return 0;
}
static const char *x7c_names(const X7N *n, const int *ids, int k) {
    Buf b = {0}; buf_str(&b, "");
    for (int i = 0; i < k; i++) buf_str(&b, cu_fmt("%s`%s` (%s:%d)", i ? (i + 1 == k ? L(" and ", " e ") : ", ") : "", n->f[ids[i]].name, x7n_rel(n, ids[i]), x7n_defline(n, ids[i])));
    return cu_keep(b.p);
}
static const char *x7c_callers_answer(const X7N *n, int ti, int ne, const int *cs) {
    return ne ? cu_fmt(L("`%s` is called by %s.", "`%s` e chamada por %s."), n->f[ti].name, x7c_names(n, cs, ne))
              : cu_fmt(L("Nothing in the indexed files calls `%s`: it is an entry point, or it is called in a way the index does not see.", "Nada nos arquivos indexados chama `%s`: e um ponto de entrada, ou e chamada de um jeito que o indice nao ve."), n->f[ti].name);
}
static const char *x7c_fn_range(const X7N *n, int ti, int *a, int *b) { *a = x7n_defline(n, ti); *b = *a + x7n_nlines(x7n_def(n, ti)) - 1; return x7n_rel(n, ti); }

/* ======================= x-nav ======================= */
enum { N7C_REFS, N7C_USES, N7C_TREE, N7C_FLOW, N7C_WORDS, N7C_TEXT, N7C_NAME, N7C_FINDSYM, N7C_N };
static const char *const g_n7c_names[N7C_N] = {"refs", "uses", "tree", "flow", "words-find", "text-grep", "name-symbols", "find-then-symbols"};
static const int g_n7c_w[N7C_N] = {4, 2, 3, 3, 3, 2, 2, 3};
static const char *const g_x7c_ev[] = {"order_created", "stock_low", "invoice_paid", "route_closed", "ticket_opened"};
static const char *g_x7c_key;
static void x7c_flow_prehook(const Scn *s) {
    const char *k = g_x7c_key;
    x_put(s, "client.js", cu_fmt("'use strict';\nconst socket = connect();\n\nfunction announce(order) {\n  socket.send(JSON.stringify({ type: \"%s\", order }));\n}\n\nmodule.exports = { announce };\n", k), 0);
    x_put(s, "server.js", cu_fmt("'use strict';\nconst handlers = [];\n\nfunction onMessage(msg) {\n  if (msg.type === \"%s\") {\n    return handleOrder(msg.order);\n  }\n  return null;\n}\n\nfunction handleOrder(order) {\n  handlers.push(order);\n  return handlers.length;\n}\n\nmodule.exports = { onMessage };\n", k), 0);
}
static void fam_x_nav(FILE *f, Scn *s) {
    x7c_lang(); x7c_begin();
    int sit, v, lang;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de nav");
        sit = x7_pick(g_n7c_w, N7C_N); v = cu_n(3); lang = cu_n(N7_NL);
        if (sit == N7C_FLOW) lang = N7_JS;
        if (x_held(260 + sit, v, lang, 0, 0) == (g_cu_hold != 0)) break;
    }
    X7N *n = &g_x7n;
    if (sit == N7C_FLOW) { g_x7c_key = CU_PA(g_x7c_ev); n->nm = 0; n->nf = 0; g_x_prehook = x7c_flow_prehook; }
    else { x7n_gen(n, s, lang, 3 + cu_n(2), 4 + cu_n(2), 3 + cu_n(2), 2, sit == N7C_TEXT ? 1 + cu_n(2) : 0, 0); g_x_prehook = x7_prehook; }
    x_begin("x-nav", s, NULL, cu_pct(12) ? 2 : 0, cu_pct(10) ? 1 : 0);
    g_x_prehook = NULL; rr_reset();
    x7c_index(s->cwd);
    x_cell("nav/%s/v%d/%s", g_n7c_names[sit], v, g_n7_ext[lang]);
    int cs[16];
    if (sit == N7C_REFS || sit == N7C_USES) {
        const int ti = x7c_pick(n, 1, 0); const char *tn = n->f[ti].name, *act = sit == N7C_REFS ? "refs" : "uses";
        x7_user(cu_fmt(sit == N7C_REFS ? (v == 0 ? L("where is `%s` used?", "onde `%s` e usada?") : v == 1 ? L("every place `%s` shows up", "todo lugar onde `%s` aparece") : L("what mentions `%s`?", "o que cita `%s`?"))
                                      : (v == 0 ? L("which code uses `%s`?", "qual codigo usa `%s`?") : v == 1 ? L("who uses `%s`?", "quem usa `%s`?") : L("list the uses of `%s`", "lista os usos de `%s`")), tn));
        const char *r = x7_sym(x7_say(), cu_fmt("{\"action\": \"%s\", \"name\": %s}", act, cu_q(tn)));
        if (sit == N7C_USES && !strncmp(r, "NOT FOUND", 9)) { act = "refs"; r = x7_sym(L("No separate uses: the refs list every place the name appears.", "Sem usos separados: o refs lista todo lugar onde o nome aparece."), cu_fmt("{\"action\": \"refs\", \"name\": %s}", cu_q(tn))); }
        X7Hit h[24]; const int nh = x7_hits(r, h, 24);
        if (nh < 1 || !strstr(r, tn) || !strncmp(r, "NOT FOUND", 9)) cu_die("%s de %s nao trouxe lugares (%s): %.200s", act, tn, g_x.cell, r);
        const char *paths[16]; int np = 0;
        for (int i = 0; i < nh; i++) { int dup = 0; for (int j = 0; j < np; j++) dup |= !strcmp(paths[j], h[i].path); if (!dup && np < 16) paths[np++] = cu_fmt("%s", h[i].path); }
        Buf b = {0}; buf_str(&b, ""); for (int i = 0; i < np; i++) buf_str(&b, cu_fmt("%s%s", i ? ", " : "", paths[i]));
        if (cu_pct(50)) {
            int a = h[0].a, bb = h[0].b > h[0].a ? h[0].b : h[0].a + 3;
            const char *rd = x7_read(s, h[0].path, a, bb - a + 1, 0);
            if (!strstr(rd, tn)) cu_die("a faixa do primeiro lugar nao tem %s", tn);
            x_final(cu_fmt(L("The index lists `%s` in %d place%s across %s. I read the first one (%s, lines %d-%d) and it uses the name as shown.", "O indice lista `%s` em %d lugar%s em %s. Li o primeiro (%s, linhas %d-%d) e ele usa o nome como aparece."), tn, nh, nh == 1 ? "" : L("s", "es"), b.p, h[0].path, a, bb));
        } else x_final(cu_fmt(L("The index lists `%s` in %d place%s, in these files: %s.", "O indice lista `%s` em %d lugar%s, nestes arquivos: %s."), tn, nh, nh == 1 ? "" : L("s", "es"), b.p));
        free(b.p);
    } else if (sit == N7C_TREE) {
        x7_user(v == 0 ? L("give me the layout of this project", "me da a estrutura deste projeto") : v == 1 ? L("what is in this project?", "o que tem neste projeto?") : L("I just cloned this repository; where do I start?", "acabei de clonar este repositorio; por onde comeco?"));
        const char *r = x7_sym(x7_say(), "{\"action\": \"tree\"}");
        if (!strstr(r, "src") || !strncmp(r, "error:", 6)) cu_die("tree sem a pasta src (%s): %.200s", g_x.cell, r);
        if (cu_pct(60)) {
            const char *mf = x7n_file(n, 0);
            x_dep(); x7_user(cu_fmt(L("and what is in %s?", "e o que tem em %s?"), mf));
            const char *o = x7_sym(NULL, cu_fmt("{\"action\": \"outline\", \"file\": %s}", cu_q(mf)));
            for (int q = 0; q < n->m[0].nf; q++) if (!strstr(o, n->f[n->m[0].f[q]].name)) cu_die("outline sem %s", n->f[n->m[0].f[q]].name);
            x_final(cu_fmt(L("%s defines %d functions (name and line): %s.", "%s define %d funcoes (nome e linha): %s."), mf, n->m[0].nf, x7_fnlist(n)));
        } else {
            Buf b = {0}; buf_str(&b, ""); for (int m = 0; m < n->nm; m++) buf_str(&b, cu_fmt("%s%s", m ? ", " : "", x7n_file(n, m)));
            const char *pl = n->nm == 1 ? "" : "s";
            x_final(L(cu_fmt("The code is in src: %d %s file%s (%s). The tree shows how deeply each is indexed.", n->nm, g_n7_lname[lang], pl, b.p), cu_fmt("O codigo esta em src: %d arquivo%s %s (%s). A arvore mostra o quanto cada um esta indexado.", n->nm, pl, g_n7_lname[lang], b.p)));
            free(b.p);
        }
    } else if (sit == N7C_FLOW) {
        const char *k = g_x7c_key;
        x7_user(cu_fmt(v == 0 ? L("who sends the message \"%s\" and who handles it? they do not import each other", "quem envia a mensagem \"%s\" e quem trata? um nao importa o outro") : v == 1 ? L("how does \"%s\" get from the client to the server?", "como \"%s\" vai do cliente para o servidor?") : L("trace the key \"%s\" between the two files", "segue a chave \"%s\" entre os dois arquivos"), k));
        const char *r = x7_sym(cu_pct(40) ? L("Two files linked by a text key, not by a call: Symbols flow.", "Dois arquivos ligados por uma chave de texto, nao por chamada: Symbols flow.") : NULL, cu_fmt("{\"action\": \"flow\", \"name\": %s}", cu_q(k)));
        if (!strstr(r, "client.js") || !strstr(r, "server.js") || !strncmp(r, "NOT FOUND", 9)) cu_die("flow nao ligou os dois arquivos (%s): %.300s", g_x.cell, r);
        x_final(cu_fmt(L("client.js sends \"%s\" (in `announce`, through the socket) and server.js handles it (in `onMessage`, which passes the order to `handleOrder`). The two files do not import each other: the link is the text key, as the flow shows.", "client.js envia \"%s\" (em `announce`, pelo socket) e server.js trata (em `onMessage`, que passa o pedido para `handleOrder`). Os dois arquivos nao se importam: a ligacao e a chave de texto, como o flow mostra."), k));
    } else if (sit == N7C_WORDS || sit == N7C_NAME || sit == N7C_TEXT) {
        /* a regra: nome exato -> Symbols; so palavras -> Find; texto exato -> Grep; cada situacao e o par das outras */
        const int ti = x7c_pick(n, 1, 0); const char *tn = n->f[ti].name, *rel = x7n_rel(n, ti); const int dl = x7n_defline(n, ti);
        if (sit == N7C_NAME) {
            x7_user(cu_fmt(L("who calls `%s`?", "quem chama `%s`?"), tn));
            const char *r = x7_sym(L("An exact name: Symbols.", "Um nome exato: Symbols."), cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn)));
            X7Hit h[16]; const int nh = x7_hits(r, h, 16); const int ne = x7n_callers(n, ti, cs);
            if (nh != ne) cu_die("callers de %s", tn);
            x_final(cu_fmt(L("`%s` is called by %s.", "`%s` e chamada por %s."), tn, x7_callers_text(h, nh, tn)));
        } else if (sit == N7C_WORDS) {
            x7_user(cu_fmt(L("I do not remember the name; it is the function in %s that does the arithmetic for the %s step", "nao lembro o nome; e a funcao de %s que faz a conta do passo %s"), rel, n->m[n->f[ti].mod].name));
            const char *r = x7_find(L("Only words, no exact name: Find.", "So palavras, sem nome exato: Find."), cu_fmt("{\"query\": %s, \"path\": %s}", cu_q(cu_fmt("%s functions", n->m[n->f[ti].mod].name)), cu_q(x_abs(s, "src"))));
            if (!strstr(r, rel)) cu_die("o Find por palavras nao chegou em %s (%s): %.200s", rel, g_x.cell, r);
            x_final(cu_fmt(L("The file for that step is %s. It has %d functions; tell me which one you mean or I can list them with the outline.", "O arquivo desse passo e %s. Ele tem %d funcoes; diga qual voce quer ou eu listo com o mapa."), rel, n->m[n->f[ti].mod].nf));
        } else {
            const int msg = n->nmsg > 0; if (!msg) cu_die("sem mensagem para o grep");
            const char *tx = g_n7_msg[n->msgi[0]], *id = g_n7_msgid[n->msgi[0]];
            x7_user(cu_fmt(L("every line with the exact text \"%s\"", "toda linha com o texto exato \"%s\""), tx));
            const char *r = x_call(L("An exact text: Grep.", "Um texto exato: Grep."), "Grep", cu_fmt("{\"pattern\": %s, \"output_mode\": \"content\", \"-n\": true}", cu_q(tx)), 0);
            if (!strstr(r, id)) cu_die("o Grep nao mostrou %s", id);
            x_final(cu_fmt(L("The text \"%s\" is in %s, in the constant `%s`.", "O texto \"%s\" esta em %s, na constante `%s`."), tx, x7n_file(n, 0), id));
        }
        (void)dl;
    } else {      /* find-then-symbols: palavras -> Find acha o nome -> Symbols responde o resto */
        const int ti = x7c_pick(n, 1, 0); const char *tn = n->f[ti].name, *rel = x7n_rel(n, ti); const int dl = x7n_defline(n, ti);
        x7_user(cu_fmt(L("which function is defined at line %d of %s, and who calls it?", "que funcao esta definida na linha %d de %s, e quem chama ela?"), dl, rel));
        const char *r = x7_sym(L("A file and a line: at.", "Um arquivo e uma linha: at."), cu_fmt("{\"action\": \"at\", \"file\": %s, \"line\": %d}", cu_q(rel), dl));
        if (!strstr(r, tn)) cu_die("at nao deu %s (%s)", tn, g_x.cell);
        const char *r2 = x7_sym(L("Now I know the name: its callers.", "Agora sei o nome: os chamadores dela."), cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn)));
        X7Hit h[16]; const int nh = x7_hits(r2, h, 16); const int ne = x7n_callers(n, ti, cs);
        if (nh != ne) cu_die("callers de %s", tn);
        x_final(cu_fmt(L("Line %d of %s is `%s`. %s", "A linha %d de %s e `%s`. %s"), dl, rel, tn, x7c_callers_answer(n, ti, ne, cs)));
    }
    x_end(f);
}
/* FIM x-nav */

/* ======================= x-ctx ======================= */
enum { C7C_CALLERS, C7C_DEF, C7C_MSG, C7C_MEM, C7C_N };
static const char *const g_c7c_names[C7C_N] = {"callers", "def", "message", "memory"};
static const int g_c7c_w[C7C_N] = {4, 4, 2, 2};
static const char *g_x7c_memfact;
static void x7c_mem_prehook(const Scn *s) {
    x7n_write(&g_x7n, s);
    if (g_x7c_memfact) { cu_mkdirs(cu_fmt("%s/.katana", s->home)); cu_put(cu_fmt("%s/.katana/memoria.txt", s->home), cu_fmt("2026-05-20\t%s\n", g_x7c_memfact), 0600); }
}
static void fam_x_ctx(FILE *f, Scn *s) {
    x7c_lang(); x7c_begin();
    int sit, v, lang;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de ctx");
        sit = x7_pick(g_c7c_w, C7C_N); v = cu_n(3); lang = cu_n(N7_NL);
        if (x_held(270 + sit, v, lang, 0, 0) == (g_cu_hold != 0)) break;
    }
    X7N *n = &g_x7n;
    x7n_gen(n, s, lang, 3 + cu_n(2), 4 + cu_n(2), 3 + cu_n(2), 2, sit == C7C_MSG ? 1 + cu_n(2) : 0, 0);
    int ti = x7c_pick(n, sit == C7C_CALLERS || sit == C7C_MEM ? 1 : 0, 0);
    g_x7c_memfact = NULL;
    if (sit == C7C_MEM) g_x7c_memfact = cu_fmt("%s was going to be replaced by a simpler rule; the owner said to drop it after the next release", n->f[ti].name);
    g_x_prehook = x7c_mem_prehook;
    x_begin("x-ctx", s, NULL, cu_pct(12) ? 2 : 0, cu_pct(10) ? 1 : 0);
    g_x_prehook = NULL; rr_reset();
    x7c_index(s->cwd);
    const char *tn = n->f[ti].name; int cs[16]; const int ne = x7n_callers(n, ti, cs);
    int a, b; const char *rel = x7c_fn_range(n, ti, &a, &b);
    const char *pre = NULL; int inj;
    if (sit == C7C_CALLERS) {
        inj = x7c_request(cu_fmt(v == 0 ? L("who calls `%s`?", "quem chama `%s`?") : v == 1 ? L("which functions call `%s`? I want to change its signature", "quais funcoes chamam `%s`? vou mudar a assinatura dela") : L("explain who depends on `%s`", "explica quem depende da `%s`"), tn));
        x_cell("ctx/callers/v%d/%s/blk%d", v, g_n7_ext[lang], inj);
        if (inj && x7c_next_read(s, &pre) && strstr(pre, "error:")) cu_die("a leitura do Next falhou");
        int covered = inj; for (int i = 0; i < ne && covered; i++) covered &= x7c_has(n->f[cs[i]].name);
        if (inj && covered && (!ne || x7c_has(x7n_rel(n, cs[0])))) x_final(x7c_callers_answer(n, ti, ne, cs));
        else {
            const char *r = x7_sym(inj ? L("The context does not list every caller: asking the index.", "O contexto nao lista todos os chamadores: pergunto ao indice.") : x7_say(), cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn))); x7_idx_norm(r);
            X7Hit h[16]; const int nh = x7_hits(r, h, 16);
            if (nh != ne) cu_die("callers de %s: esperava %d, deu %d (%s)", tn, ne, nh, g_x.cell);
            x_final(x7c_callers_answer(n, ti, ne, cs));
        }
    } else if (sit == C7C_DEF) {
        inj = x7c_request(cu_fmt(v == 0 ? L("where is `%s` defined and what does it return?", "onde `%s` e definida e o que ela devolve?") : v == 1 ? L("show me `%s`", "me mostra a `%s`") : L("what is `%s`?", "o que e `%s`?"), tn));
        x_cell("ctx/def/v%d/%s/blk%d", v, g_n7_ext[lang], inj);
        const char *rd = NULL; int did = 0;
        if (inj) { did = x7c_next_read(s, &rd); if (did && strstr(rd, "error:")) cu_die("a leitura do Next falhou"); }
        const char *ex = x7n_expr(n, ti);
        if (!did || !strstr(rd, ex)) {
            if (!inj) { const char *r = x7_sym(x7_say(), cu_fmt("{\"action\": \"def\", \"name\": %s}", cu_q(tn))); x7_idx_norm(r); if (!strstr(r, cu_fmt("%s:%d", rel, a))) cu_die("def de %s nao bate", tn); }
            rd = x7_read(s, rel, a, b - a + 1, 0);
            if (!strstr(rd, ex)) cu_die("a faixa de %s nao tem o retorno (%s)", tn, g_x.cell);
        }
        x_final(cu_fmt(L("`%s` is defined in %s, lines %d-%d, and returns `%s`.", "`%s` esta definida em %s, linhas %d-%d, e devolve `%s`."), tn, rel, a, b, ex));
    } else if (sit == C7C_MSG) {
        const char *tx = g_n7_msg[n->msgi[0]], *id = g_n7_msgid[n->msgi[0]];
        inj = x7c_request(cu_fmt(v == 0 ? L("which file has the message \"%s\"?", "qual arquivo tem a mensagem \"%s\"?") : v == 1 ? L("where does the text \"%s\" come from?", "de onde vem o texto \"%s\"?") : L("find the text \"%s\" in the code", "acha o texto \"%s\" no codigo"), tx));
        x_cell("ctx/message/v%d/%s/blk%d", v, g_n7_ext[lang], inj);
        if (inj && x7c_next_read(s, &pre) && strstr(pre, "error:")) cu_die("a leitura do Next falhou");
        if (!(inj && x7c_has(x7n_file(n, 0)) && x7c_has(id))) {
            const char *r = x7_find(NULL, cu_fmt("{\"query\": %s}", cu_q(cu_fmt("\"%s\"", tx))));
            if (strncmp(r, "Literal text found", 18) || !strstr(r, id)) cu_die("o Find nao achou o texto (%s): %.200s", g_x.cell, r);
        }
        x_final(cu_fmt(L("The text \"%s\" is in %s, in the constant `%s`.", "O texto \"%s\" esta em %s, na constante `%s`."), tx, x7n_file(n, 0), id));
    } else {      /* memory: uma nota do dono, datada; vale como pista, nunca como fato */
        inj = x7c_request(cu_fmt(v == 0 ? L("last time I said `%s` would be replaced; who still calls it?", "da outra vez eu disse que a `%s` ia ser trocada; quem ainda chama ela?") : v == 1 ? L("remind me what we agreed about `%s`, and check who calls it now", "lembra o que combinamos sobre a `%s`, e confere quem chama ela agora") : L("what did I decide about `%s`? is it still used?", "o que eu decidi sobre a `%s`? ainda e usada?"), tn));
        const int mem = inj && strstr(g_x7c_blk, "memory 2026-05-20") != NULL;
        x_cell("ctx/memory/v%d/%s/blk%d/mem%d", v, g_n7_ext[lang], inj, mem);
        const char *r = x7_sym(mem ? L("The memory line is a dated guess: checking the code first.", "A linha de memoria e um palpite datado: confiro no codigo primeiro.") : x7_say(), cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn))); x7_idx_norm(r);
        X7Hit h[16]; const int nh = x7_hits(r, h, 16);
        if (nh != ne) cu_die("callers de %s: esperava %d, deu %d", tn, ne, nh);
        const char *base = x7c_callers_answer(n, ti, ne, cs);
        if (mem) x_final(cu_fmt(L("%s A saved note from 2026-05-20 says it was going to be replaced and dropped after the next release; I did not rely on it, and the index shows it still has callers, so that has not happened yet.", "%s Uma nota salva em 2026-05-20 diz que ela seria trocada e removida depois da proxima versao; nao me apoiei nela, e o indice mostra que ela ainda tem chamadores, entao isso ainda nao aconteceu."), base));
        else x_final(base);
    }
    (void)b;
    x_end(f);
}
/* FIM x-ctx */

/* ======================= x-ctxlimits ======================= */
enum { L7C_TWO, L7C_IGN, L7C_N };
static const char *const g_l7c_names[L7C_N] = {"existing-and-missing", "ignored-folder"};
static const int g_l7c_w[L7C_N] = {4, 4};
static const char *g_x7c_vrel, *g_x7c_vtext;
static void x7c_vendor_prehook(const Scn *s) { x7n_write(&g_x7n, s); x_put(s, ".gitignore", "vendor/\n", 0); x_put(s, g_x7c_vrel, g_x7c_vtext, 0); }
static void fam_x_ctxlimits(FILE *f, Scn *s) {
    x7c_lang(); x7c_begin();
    int sit, v, lang;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de ctxlimits");
        sit = x7_pick(g_l7c_w, L7C_N); v = cu_n(3); lang = sit == L7C_IGN ? CU_PA(((const int[]){N7_PY, N7_JS})) : cu_n(N7_NL);
        if (x_held(280 + sit, v, lang, 0, 0) == (g_cu_hold != 0)) break;
    }
    X7N *n = &g_x7n;
    x7n_gen(n, s, lang, 3 + cu_n(2), 4 + cu_n(2), 3 + cu_n(2), 2, 0, 0);
    const char *vname = NULL, *vlib = CU_PA(((const char *const[]){"acme", "lumen", "tessera", "corvid"}));
    if (sit == L7C_IGN) {
        for (int t = 0; t < 100 && !vname; t++) { const char *c = x7n_style(lang, CU_PA(g_n7_verb), CU_PA(g_n7_noun)); if (x7n_idx(n, c) < 0) vname = c; }
        if (!vname) cu_die("sem nome para a pasta ignorada");
        g_x7c_vrel = cu_fmt("vendor/%s/core.%s", vlib, g_n7_ext[lang]);
        g_x7c_vtext = lang == N7_PY ? cu_fmt("def %s(a, b):\n    return a * 7 + b\n", vname) : cu_fmt("function %s(a, b) {\n  return a * 7 + b;\n}\nmodule.exports = { %s };\n", vname, vname);
        g_x_prehook = x7c_vendor_prehook;
    } else g_x_prehook = x7_prehook;
    x_begin("x-ctxlimits", s, NULL, cu_pct(12) ? 2 : 0, cu_pct(10) ? 1 : 0);
    g_x_prehook = NULL; rr_reset();
    x7c_index(s->cwd);
    int cs[16];
    if (sit == L7C_TWO) {
        const int ti = x7c_pick(n, 1, 0); const char *tn = n->f[ti].name; int a, b; const char *rel = x7c_fn_range(n, ti, &a, &b);
        const char *bad = NULL;
        for (int t = 0; t < 40 && !bad; t++) { const char *c = x7n_style(lang, "recalculate", CU_PA(((const char *const[]){"payroll", "rebate", "forecast", "mileage"}))); if (x7n_idx(n, c) < 0) bad = c; }
        if (!bad) cu_die("sem nome inexistente");
        const int inj = x7c_request(cu_fmt(v == 0 ? L("who calls `%s` and who calls `%s`?", "quem chama `%s` e quem chama `%s`?") : v == 1 ? L("I need the callers of `%s` and of `%s`", "preciso dos chamadores da `%s` e da `%s`") : L("list who calls `%s`, and `%s` too", "lista quem chama a `%s`, e a `%s` tambem"), tn, bad));
        x_cell("ctxlimits/two/v%d/%s/blk%d", v, g_n7_ext[lang], inj);
        const char *pre = NULL; if (inj && x7c_next_read(s, &pre) && strstr(pre, "error:")) cu_die("a leitura do Next falhou");
        const int listed = inj && strstr(g_x7c_blk, "Not found / limits:") && strstr(strstr(g_x7c_blk, "Not found / limits:"), bad);
        const char *r = x7_sym(listed ? L("The limits already say the other name is not there: asking only for the one that exists.", "Os limites ja dizem que o outro nome nao existe: pergunto so o que existe.") : NULL, cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn))); x7_idx_norm(r);
        X7Hit h[16]; const int nh = x7_hits(r, h, 16); const int ne = x7n_callers(n, ti, cs);
        if (nh != ne) cu_die("callers de %s", tn);
        if (!listed) {
            const char *r2 = x7_sym(NULL, cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(bad)));
            if (!strstr(r2, "NOT FOUND")) cu_die("o nome inexistente %s achou algo", bad);
        }
        x_final(cu_fmt(L("%s As for `%s`: the index has no function with that name or a close spelling, so I cannot list its callers. Tell me where you saw it.", "%s Quanto a `%s`: o indice nao tem funcao com esse nome nem grafia parecida, entao nao consigo listar os chamadores. Me diz onde voce viu."), x7c_callers_answer(n, ti, ne, cs), bad));
        (void)rel;
    } else {
        const int inj = x7c_request(cu_fmt(v == 0 ? L("where is `%s` defined?", "onde `%s` e definida?") : v == 1 ? L("find the function `%s`", "acha a funcao `%s`") : L("which file has `%s`?", "qual arquivo tem `%s`?"), vname));
        x_cell("ctxlimits/ignored/v%d/%s/blk%d", v, g_n7_ext[lang], inj);
        const char *dir = x_abs(s, cu_fmt("vendor/%s", vlib));
        const int told = inj && x7c_has("ignored") && x7c_has("vendor");
        if (!told) {
            const char *r1 = x7_sym(x7_say(), cu_fmt("{\"action\": \"def\", \"name\": %s}", cu_q(vname))); x7_idx_norm(r1);
            if (!strstr(r1, "NOT FOUND")) cu_die("o indice achou %s fora da pasta ignorada: %.200s", vname, r1);
            const char *r2 = x7_find(L("Not in the index. Find says which folders it skipped.", "Nao esta no indice. O Find diz quais pastas pulou."), cu_fmt("{\"query\": %s}", cu_q(vname)));
            if (!strstr(r2, "NOT FOUND") || !strstr(r2, "ignored folder")) cu_die("o Find nao falou da pasta ignorada (%s): %.300s", g_x.cell, r2);
        }
        const char *r3 = x7_find(told ? L("The context says the name may be in an ignored folder: searching it with path.", "O contexto diz que o nome pode estar numa pasta ignorada: procuro nela com path.") : L("One ignored folder could hold it: searching it with path.", "Uma pasta ignorada pode ter o nome: procuro nela com path."), cu_fmt("{\"query\": %s, \"path\": %s}", cu_q(vname), cu_q(dir)));
        if (!strstr(r3, "Found by name or text") || !strstr(r3, "core.")) cu_die("o Find com path nao achou %s (%s): %.300s", vname, g_x.cell, r3);
        x_final(cu_fmt(L("`%s` is in %s, line 1. It is in `vendor/%s`, a folder the index ignores, so the first search did not look there; I searched it with `path`.", "`%s` esta em %s, linha 1. Fica em `vendor/%s`, uma pasta que o indice ignora, entao a primeira busca nao olhou ali; procurei nela com `path`."), vname, g_x7c_vrel, vlib));
    }
    x_end(f);
}
/* FIM x-ctxlimits */

/* ======================= x-notes ======================= */
enum { M7C_PLACE, M7C_PART, M7C_REPEAT, M7C_N };
static const char *const g_m7c_names[M7C_N] = {"cited-folder", "read-in-parts", "repeat-refs"};
static const int g_m7c_w[M7C_N] = {4, 3, 3};
static void fam_x_notes(FILE *f, Scn *s) {
    x7c_lang(); x7c_begin();
    int sit, v, lang;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de notes");
        sit = x7_pick(g_m7c_w, M7C_N); v = cu_n(3); lang = sit == M7C_PART ? CU_PA(((const int[]){N7_PY, N7_JS, N7_TS, N7_C, N7_GO, N7_RS, N7_JV})) : cu_n(N7_NL);
        if (x_held(290 + sit, v, lang, 0, 0) == (g_cu_hold != 0)) break;
    }
    X7N *n = &g_x7n; int cs[16];
    if (sit == M7C_PLACE) {
        x7n_gen(n, s, lang, 3, 4, 3, 2, 0, 0);
        const char *d1 = CU_PA(((const char *const[]){"work", "code", "repos", "clients"})), *d2 = CU_PA(((const char *const[]){"greenhouse", "lighthouse", "quarry", "orchard", "harbor", "meadow"}));
        const char *name = s->name, *P = cu_fmt("%s/%s/%s/%s", s->home, d1, d2, name);
        g_x7_other = *s; g_x7_other.cwd = P;
        s->cwd = s->home;
        g_x_prehook = x7_prehook2;
        x_begin("x-notes", s, NULL, cu_pct(12) ? 2 : 0, cu_pct(10) ? 1 : 0);
        g_x_prehook = NULL; rr_reset();
        x7c_index(P);
        const int ti = x7c_pick(n, 1, 0); const char *tn = n->f[ti].name;
        const int inj = x7c_request(cu_fmt(v == 0 ? L("in the project %s, who calls `%s`?", "no projeto %s, quem chama `%s`?") : v == 1 ? L("the project %s: which functions call `%s`?", "o projeto %s: quais funcoes chamam `%s`?") : L("in the project %s, where is `%s` called from?", "no projeto %s, de onde `%s` e chamada?"), name, tn));
        x_cell("notes/place/v%d/%s/blk%d", v, g_n7_ext[lang], inj);
        /* com bloco ou com a nota do motor, a pasta esta no texto: path desde a primeira chamada */
        if (!inj && !strstr(g_x.m[g_x.n - 1].text, "[katana: you mention")) cu_die("o motor nao deu a nota de pasta (%s)", g_x.cell);
        const char *r = x7_sym(cu_pct(40) ? L("The folder is given above: passing it as path.", "A pasta esta acima: passo como path.") : NULL, cu_fmt("{\"action\": \"callers\", \"name\": %s, \"path\": %s}", cu_q(tn), cu_q(P)));
        X7Hit h[16]; const int nh = x7_hits(r, h, 16); const int ne = x7n_callers(n, ti, cs);
        if (nh != ne) cu_die("callers em notes: esperava %d, deu %d (%s)", ne, nh, g_x.cell);
        x_final(cu_fmt(L("In %s, `%s` is called by %s.", "Em %s, `%s` e chamada por %s."), name, tn, x7_callers_text(h, nh, tn)));
    } else if (sit == M7C_PART) {
        x7n_gen(n, s, lang, 1, 5, 5, 5, 0, 1);
        g_x_prehook = x7_prehook;
        x_begin("x-notes", s, NULL, cu_pct(12) ? 2 : 0, cu_pct(10) ? 1 : 0);
        g_x_prehook = NULL; rr_reset();
        x7c_index(s->cwd);
        const char *file = x7n_file(n, 0); const int total = x7n_nlines(n->m[0].text);
        if (total <= 200) cu_die("arquivo pequeno demais (%d linhas)", total);
        x_cell("notes/part/v%d/%s", v, g_n7_ext[lang]);
        const int ti = n->m[0].f[n->m[0].nf - 1]; const char *tn = n->f[ti].name;
        x7_user(cu_fmt(v == 0 ? L("what does `%s` return? it is somewhere in %s", "o que `%s` devolve? ela esta em algum ponto de %s") : v == 1 ? L("read `%s` in %s for me", "le a `%s` em %s pra mim") : L("show me the last function of %s, `%s`", "me mostra a ultima funcao de %s, `%s`"), v == 2 ? file : tn, v == 2 ? tn : file));
        const char *r1 = x7_call(L("A big file: the first part.", "Um arquivo grande: a primeira parte."), "Read", cu_fmt("{\"file_path\": %s, \"offset\": 1, \"limit\": 60}", cu_q(x_abs(s, file))), 0);
        if (!strstr(r1, "[PARTIAL view")) cu_die("o Read parcial nao avisou (%s)", g_x.cell);
        if (strstr(r1, tn)) cu_die("a funcao ja estava na primeira parte");
        int a, b; x7c_fn_range(n, ti, &a, &b);
        const char *r2 = x7_sym(L("It is not in the first part: the index gives its exact range.", "Nao esta na primeira parte: o indice da a faixa exata."), cu_fmt("{\"action\": \"def\", \"name\": %s}", cu_q(tn))); x7_idx_norm(r2);
        if (!strstr(r2, cu_fmt("%s:%d", file, a))) cu_die("def de %s nao bate", tn);
        const char *r3 = x7_read(s, file, a, b - a + 1, 0);
        const char *ex = x7n_expr(n, ti);
        if (!strstr(r3, ex)) cu_die("a faixa de %s nao tem o retorno", tn);
        x_final(cu_fmt(L("`%s` is at %s, lines %d-%d, and returns `%s`. The first read only covered lines 1-60 of %d, so I read just its range.", "`%s` esta em %s, linhas %d-%d, e devolve `%s`. A primeira leitura so cobriu as linhas 1-60 de %d, entao li so a faixa dela."), tn, file, a, b, ex, total));
    } else {
        x7n_gen(n, s, lang, 3 + cu_n(2), 4 + cu_n(2), 3 + cu_n(2), 2, 0, 0);
        g_x_prehook = x7_prehook;
        x_begin("x-notes", s, NULL, cu_pct(12) ? 2 : 0, cu_pct(10) ? 1 : 0);
        g_x_prehook = NULL; rr_reset();
        x7c_index(s->cwd);
        x_cell("notes/repeat/v%d/%s", v, g_n7_ext[lang]);
        const int ti = x7c_pick(n, 1, 0); const char *tn = n->f[ti].name; int a, b; const char *rel = x7c_fn_range(n, ti, &a, &b);
        x7_user(cu_fmt(L("where is `%s` used? I want to see one of the uses", "onde `%s` e usada? quero ver um dos usos"), tn));
        const char *args = cu_fmt("{\"action\": \"refs\", \"name\": %s}", cu_q(tn));
        const char *r1 = x7_sym(NULL, args); x7_idx_norm(r1);
        X7Hit h[24]; const int nh = x7_hits(r1, h, 24); if (nh < 1) cu_die("refs sem lugares");
        const char *r2 = x7_call(NULL, "Symbols", args, XF_NOTRAIN);
        if (!strstr(r2, "you already made this exact call")) cu_die("sem a nota de repeticao (%s): %.200s", g_x.cell, r2);
        const char *rd = x7_read(s, h[0].path, h[0].a, (h[0].b > h[0].a ? h[0].b : h[0].a + 3) - h[0].a + 1, 0);
        if (!strstr(rd, tn)) cu_die("a faixa do primeiro uso nao tem %s", tn);
        x_final(cu_fmt(L("`%s` is used in %d place%s. The first is in %s, lines %d-%d, which I read: it uses the name as the index shows. The definition is in %s, line %d.", "`%s` e usada em %d lugar%s. O primeiro fica em %s, linhas %d-%d, que li: ele usa o nome como o indice mostra. A definicao esta em %s, linha %d."), tn, nh, nh == 1 ? "" : L("s", "es"), h[0].path, h[0].a, h[0].b > h[0].a ? h[0].b : h[0].a + 3, rel, a));
        (void)b;
    }
    x_end(f);
}
/* FIM x-notes */

/* ======================= x-plan ======================= */
enum { P7C_DONEFIRST, P7C_AUDIT, P7C_PICTURE, P7C_N };
static const char *const g_q7c_names[P7C_N] = {"done-before-list", "audit", "module-picture"};
static const int g_q7c_w[P7C_N] = {4, 4, 3};
static void fam_x_plan(FILE *f, Scn *s) {
    x7c_lang(); x7c_begin();
    int sit, v, lang;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de plan");
        sit = x7_pick(g_q7c_w, P7C_N); v = cu_n(3); lang = cu_n(N7_NL);
        if (sit == P7C_PICTURE) lang = CU_PA(((const int[]){N7_JS, N7_TS, N7_C}));
        if (x_held(300 + sit, v, lang, 0, 0) == (g_cu_hold != 0)) break;
    }
    X7N *n = &g_x7n;
    x7n_gen(n, s, lang, 3 + cu_n(2), 4 + cu_n(2), 3 + cu_n(2), 2, 0, 0);
    g_x_prehook = x7_prehook;
    x_begin("x-plan", s, NULL, cu_pct(12) ? 2 : 0, cu_pct(10) ? 1 : 0);
    g_x_prehook = NULL; rr_reset();
    x7c_index(s->cwd);
    x_cell("plan/%s/v%d/%s", g_q7c_names[sit], v, g_n7_ext[lang]);
    int cs[16];
    if (sit == P7C_DONEFIRST || sit == P7C_AUDIT) {
        int ti; do ti = cu_n(n->nf); while (!n->f[ti].nc || x7n_callers(n, ti, cs) < 1);
        const char *tn = n->f[ti].name;
        x7_user(cu_fmt(v == 0 ? L("audit `%s`: who calls it, what it calls, and what a change to it reaches. Answer all three.", "audita a `%s`: quem chama, o que ela chama e ate onde uma mudanca nela chega. Responde as tres.") : v == 1 ? L("I am about to change `%s`. Give me its callers, its callees and the impact", "vou mudar a `%s`. Me da os chamadores, o que ela chama e o impacto") : L("full picture of `%s`: callers, callees, impact", "quadro completo da `%s`: chamadores, o que chama, impacto"), tn));
        if (sit == P7C_DONEFIRST) {
            const char *e = x_call(L("Step one is done; marking it.", "O primeiro passo esta feito; marco."), "TodoWrite", "{\"done\": 1}", XF_ERR);
            if (!strstr(e, "no task list yet")) cu_die("o motor nao recusou o done sem lista: %.200s", e);
        }
        const char *it[3] = {L("Find who calls the function", "Achar quem chama a funcao"), L("List what the function calls", "Listar o que a funcao chama"), L("Check what a change reaches and write the three answers", "Ver ate onde uma mudanca chega e escrever as tres respostas")};
        x_plan(it, 3);
        const char *r1 = x7_sym(NULL, cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn))); x7_idx_norm(r1); X7Hit h1[16]; const int n1 = x7_hits(r1, h1, 16);
        x_done(1);
        const char *r2 = x7_sym(NULL, cu_fmt("{\"action\": \"callees\", \"name\": %s}", cu_q(tn))); X7Hit h2[8]; const int n2 = x7_hits(r2, h2, 8);
        x_done(2);
        const char *r3 = x7_sym(NULL, cu_fmt("{\"action\": \"impact\", \"name\": %s}", cu_q(tn))); X7Hit h3[24]; const int n3 = x7_hits(r3, h3, 24);
        if (n1 != x7n_callers(n, ti, cs) || n2 != n->f[ti].nc) cu_die("auditoria de %s", tn);
        Buf b2 = {0}; buf_str(&b2, ""); for (int i = 0; i < n2; i++) buf_str(&b2, cu_fmt("%s`%s`", i ? ", " : "", x7_after(h2[i].rest, "call ")));
        x_final(cu_fmt(L("`%s` is called by %s. It calls %s. A change to it reaches %d function%s up the call graph.", "`%s` e chamada por %s. Ela chama %s. Uma mudanca nela chega a %d funcao%s no grafo de chamadas."), tn, x7_callers_text(h1, n1, tn), b2.p, n3, n3 == 1 ? "" : L("s", "es")));
        free(b2.p);
    } else {
        const char *mf = x7n_file(n, 0); const int ti = n->m[0].f[0]; const char *tn = n->f[ti].name;
        const char *others[4]; int no = 0;
        for (int om = 1; om < n->nm; om++) { const char *nm[16]; if (x7n_uses(n, 0, om, nm)) others[no++] = n->m[om].name; }
        x7_user(cu_fmt(L("picture of %s: what it defines, what it imports from the other files, and what breaks if I change `%s`", "quadro de %s: o que ela define, o que importa dos outros arquivos, e o que quebra se eu mudar `%s`"), mf, tn));
        const char *it[3] = {L("Outline the file", "Mapear o arquivo"), L("List what it imports", "Listar o que ela importa"), L("Check the impact of the first function and write the answer", "Ver o impacto da primeira funcao e escrever a resposta")};
        x_plan(it, 3);
        const char *o = x7_sym(NULL, cu_fmt("{\"action\": \"outline\", \"file\": %s}", cu_q(mf))); x7_idx_norm(o);
        for (int q = 0; q < n->m[0].nf; q++) if (!strstr(o, n->f[n->m[0].f[q]].name)) cu_die("outline sem %s", n->f[n->m[0].f[q]].name);
        x_done(1);
        const char *d = x7_sym(NULL, cu_fmt("{\"action\": \"deps\", \"file\": %s}", cu_q(mf)));
        for (int i = 0; i < no; i++) if (!strstr(d, others[i])) cu_die("deps sem %s (%s): %.200s", others[i], g_x.cell, d);
        x_done(2);
        const char *r3 = x7_sym(NULL, cu_fmt("{\"action\": \"impact\", \"name\": %s}", cu_q(tn))); X7Hit h3[24]; const int n3 = x7_hits(r3, h3, 24);
        Buf bo = {0}; buf_str(&bo, ""); for (int i = 0; i < no; i++) buf_str(&bo, cu_fmt("%s%s", i ? ", " : "", others[i]));
        x_final(cu_fmt(L("%s defines %d functions (%s). It %s%s. A change to `%s` reaches %d caller%s in the call graph.", "%s define %d funcoes (%s). Ela %s%s. Uma mudanca em `%s` chega a %d chamador%s no grafo de chamadas."),
                       mf, n->m[0].nf, x7_fnlist(n), no ? L("uses code from ", "usa codigo de ") : L("imports nothing from the other project files", "nao importa nada dos outros arquivos do projeto"), bo.p, tn, n3, n3 == 1 ? "" : L("s", "es")));
        free(bo.p);
    }
    x_end(f);
}
/* FIM x-plan */

/* ======================= x-multi ======================= */
/* tres pedidos na mesma conversa: onde esta, quem chama, muda e confere. O segundo e o terceiro so se entendem pelo primeiro */
static void fam_x_multi(FILE *f, Scn *s) {
    x7c_lang(); x7c_begin();
    int v, lang;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de multi");
        v = cu_n(3); lang = CU_PA(((const int[]){N7_PY, N7_JS, N7_C, N7_TS, N7_GO}));
        if (x_held(310, v, lang, 0, 0) == (g_cu_hold != 0)) break;
    }
    X7N *n = &g_x7n;
    x7n_gen(n, s, lang, 3 + cu_n(2), 4 + cu_n(2), 3 + cu_n(2), 2, 0, 0);
    g_x_prehook = x7_prehook;
    x_begin("x-multi", s, NULL, cu_pct(12) ? 2 : 0, cu_pct(10) ? 1 : 0);
    g_x_prehook = NULL; rr_reset();
    x7c_index(s->cwd);
    x_cell("multi/v%d/%s", v, g_n7_ext[lang]);
    const int ti = x7c_pick(n, 1, 0); const char *tn = n->f[ti].name; int a, b; const char *rel = x7c_fn_range(n, ti, &a, &b);
    int cs[16]; const int ne = x7n_callers(n, ti, cs);
    const int inj = x7c_request(cu_fmt(v == 0 ? L("where is `%s` defined and what does it return?", "onde `%s` e definida e o que ela devolve?") : v == 1 ? L("show me `%s`", "me mostra a `%s`") : L("I am new here: what is `%s`?", "sou novo aqui: o que e `%s`?"), tn));
    const char *ex = x7n_expr(n, ti); const char *rd = NULL; int did = 0;
    if (inj) { did = x7c_next_read(s, &rd); if (did && strstr(rd, "error:")) cu_die("a leitura do Next falhou"); }
    if (!did || !strstr(rd, ex)) {
        if (!inj) { const char *r = x7_sym(x7_say(), cu_fmt("{\"action\": \"def\", \"name\": %s}", cu_q(tn))); x7_idx_norm(r); if (!strstr(r, cu_fmt("%s:%d", rel, a))) cu_die("def de %s nao bate", tn); }
        rd = x7_read(s, rel, a, b - a + 1, 0);
        if (!strstr(rd, ex)) cu_die("a faixa de %s nao tem o retorno", tn);
    }
    x_final(cu_fmt(L("`%s` is defined in %s, lines %d-%d, and returns `%s`.", "`%s` esta definida em %s, linhas %d-%d, e devolve `%s`."), tn, rel, a, b, ex));
    x_dep(); x7_user(XV(L("and who calls it?", "e quem chama ela?"), L("which functions use it?", "quais funcoes usam ela?")));
    const char *r2 = x7_sym(NULL, cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn)));
    X7Hit h2[16]; const int n2 = x7_hits(r2, h2, 16); if (n2 != ne) cu_die("callers de %s", tn);
    x_final(x7c_callers_answer(n, ti, ne, cs));
    int cnt = 0; for (const char *q = n->m[n->f[ti].mod].text; (q = strstr(q, ex)); q++) cnt++;
    if (cnt != 1 || (lang != N7_PY && lang != N7_JS && lang != N7_C)) { x_end(f); return; }
    x_dep(); x7_user(cu_fmt(XV(L("add 1 to what `%s` returns, and check the syntax", "soma 1 no que `%s` devolve, e confere a sintaxe"), L("make `%s` return one more, then check it still parses", "faz `%s` devolver um a mais, e confere se ainda compila")), tn));
    const char *nw = lang == N7_PY ? cu_fmt("%s + 1", ex) : cu_fmt("%s + 1", ex);
    x_read(s, rel, 0);
    x_edit(s, rel, ex, nw, 0, 0);
    const char *sc = x_bash(NULL, x7_syntax_cmd(lang, n), L("Syntax check of the changed files", "Checagem de sintaxe dos arquivos mudados"), 0);
    if (strstr(sc, "rror")) cu_die("a checagem de sintaxe falhou: %.300s", sc);
    x_final(cu_fmt(L("Done: `%s` in %s now returns `%s`, and the syntax check passes. The %d caller%s of it were not touched.", "Pronto: `%s` em %s agora devolve `%s`, e a checagem de sintaxe passa. Os %d chamador%s dela nao foram mexidos."), tn, rel, nw, ne, ne == 1 ? "" : L("s", "es")));
    x_end(f);
}
/* FIM x-multi */
