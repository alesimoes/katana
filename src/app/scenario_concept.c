/* ---------- katana curriculum, cenarios compostos: os conceitos do proprio katana (x-concept) ----------
 * Cada conceito (comandos, opcoes, comandos do console, limites, sessoes, plano, edicoes, leitura por diferenca,
 * resultados tirados do contexto, KATANA.md, skills, confianca, recusas, o que nao da para fazer, descoberta de
 * ferramentas) aparece em pelo menos tres situacoes: o usuario pergunta como funciona (a resposta sai da saida real de um
 * comando especifico), o usuario esbarra no mecanismo (o texto do motor esta na conversa e a resposta parte dele) e o
 * usuario parte de uma ideia errada ou pede o que nao da (a resposta corrige com a prova, ou tenta outro caminho antes).
 * Nada aqui e conhecimento digitado: a frase citada e cortada da saida real, e o que o gerador escreve por conta propria
 * (a segunda frase) so sai se os trechos entre crases estiverem na saida. As situacoes saem em rodizio (como x-rare);
 * as marcadas hold so saem no arquivo de validacao.
 * A ajuda e pedida por assunto (`katana help <palavra>`), nunca inteira: uma linha nova na ajuda so muda a conversa
 * quando tem a palavra pedida. */

static const char *x_tsel(const char *names);
static long g_xc_cycle;          /* rodizio das situacoes de x-concept */

/* ---- pecas ---- */
static const char *xc_squash(const char *ln, int max) {
    while (*ln == ' ') ln++;
    Buf b = {0}; buf_str(&b, "");
    for (const char *z = ln; *z; z++) { if (*z == ' ' && (z[1] == ' ' || z[1] == 0)) continue; buf_add(&b, z, 1); }
    char *t = cu_keep(b.p);
    if ((int)strlen(t) > max) { t[max] = 0; char *sp = strrchr(t, ' '); if (sp) *sp = 0; t = cu_fmt("%s...", t); }
    return t;
}
/* os trechos entre crases de uma frase escrita pelo gerador tem de estar na saida real */
static void xc_tail_ok(const char *tail, const char *out) {
    for (const char *q = tail; (q = strchr(q, '`')) != NULL; ) {
        const char *e = strchr(q + 1, '`'); if (!e) break;
        const char *tok = cu_fmt("%.*s", (int)(e - q - 1), q + 1);
        if (!strstr(out, tok)) cu_die("x-concept: a frase cita `%s`, que nao esta na saida real", tok);
        q = e + 1;
    }
}
/* a pergunta como alguem a digitaria: seca, com um oi, com pressa, com um obrigado */
static const char *xc_shape(const char *q) {
    static const char *const pre[][2] = {{"", ""}, {"", ""}, {"quick question: ", "duvida rapida: "}, {"hey, ", "oi, "}, {"one thing I never remember: ", "uma coisa que eu nunca lembro: "}, {"sorry to bother you with this, but ", "desculpa incomodar com isso, mas "}, {"tell me, ", "me diz, "}};
    static const char *const suf[][2] = {{"", ""}, {"", ""}, {"", ""}, {" thanks", " valeu"}, {" (a short answer is fine)", " (pode ser curto)"}, {" I could not find it in the docs", " nao achei nos docs"}};
    const int a = cu_n(CU_N(pre)), b = cu_n(CU_N(suf));
    (void)a; (void)b; (void)pre; (void)suf;      /* a forma do pedido agora e sorteada para todas as familias em scenario_shape.c (com as reservadas da validacao); os sorteios ficam para a ordem nao mudar */
    return q;
}
static const char *xc_help(const char *w) {
    return cu_fmt("%s", x_bash(NULL, cu_fmt("katana help %s", w), XV(L("Ask katana's help about this", "Pergunta isso a ajuda do katana"), L("Look it up in katana's help", "Procura na ajuda do katana"), L("Search katana's help", "Busca na ajuda do katana")), 0));
}
/* a primeira linha do erro que o binario da de verdade para uma opcao que nao existe (o usuario cola essa linha) */
static const char *xc_real_error(const char *args) {
    FILE *pf = popen(cu_fmt("katana %s 2>&1 </dev/null", args), "r"); if (!pf) cu_die("sem popen");
    char ln[400] = ""; if (!fgets(ln, sizeof ln, pf)) ln[0] = 0;
    char t[400]; while (fgets(t, sizeof t, pf)) {}
    pclose(pf);
    ln[strcspn(ln, "\n")] = 0;
    if (strncmp(ln, "katana: ", 8)) cu_die("o binario nao recusou `katana %s`: %.100s", args, ln);
    return cu_fmt("%s", ln);
}
enum { XP_BRIEF = 1, XP_FULL = 2, XP_NEVER = 4, XP_ASKR = 8, XP_BIG = 16, XP_CONF = 32, XP_DOCS = 64, XP_MID = 128, XP_NOBRIEF = 256 };
static XProj *xc_open(XC *c, unsigned fl, int nskills, const char *cell) {
    static const int stw[] = {X_PY, X_PY, X_JS, X_JS, X_SH, X_PL};
    const int big = (fl & XP_BIG) != 0;
    x_proj(&c->p, c->s, CU_PA(stw), big || (fl & XP_MID) ? 3 : 2, big ? 5 : (fl & XP_MID) ? 3 : 2 + cu_n(2));      /* XP_MID: docs/functions.md grande o bastante para a leitura por diferenca */ XProj *p = &c->p;
    p->verbose = big;
    p->brief = (fl & (XP_BRIEF | XP_FULL | XP_NEVER | XP_ASKR)) ? 1 : (fl & XP_NOBRIEF) ? 0 : cu_pct(40);
    p->readme = !p->brief && cu_pct(50);
    p->rules = (fl & XP_FULL) ? 1 : p->brief && cu_pct(35);
    p->docs = (fl & XP_DOCS) ? 1 : p->rules;
    if (p->docs && !p->brief) p->docs = (fl & XP_DOCS) != 0;
    p->never = (fl & XP_NEVER) != 0; p->askrule = (fl & XP_ASKR) != 0;
    p->knows = p->brief;
    if (fl & XP_CONF) { p->conf = 1; for (int i = 0; i < CU_N(x_conf_keys); i++) x_conf_add(p, x_conf_keys[i][0], x_conf_keys[i][1], x_conf_keys[i][2]); }
    x_begin("x-concept", c->s, p, nskills, 0);
    x_cell("concept/%s", cell);
    if (strstr(cell, "wrong")) x_shape_tag("wrong");
    if (strstr(cell, "pasted")) x_shape_tag("paste");
    return p;
}

/* ======================= perguntas respondidas por `katana help <palavra>` ======================= */
/* kind: 0 como se faz; 1 o usuario parte de uma ideia errada; 2 o usuario cola o erro real do binario (bad = os argumentos
   que dao o erro); 3 nao existe (as duas palavras nao acham nada). must: trecho que a linha citada precisa ter */
typedef struct { const char *con; int kind, hold; const char *en, *pt, *w1, *w2, *needle, *must, *ten, *tpt, *bad; } XHq;
static const XHq g_xc_help[] = {
    /* comandos */
    {"commands", 0, 0, "which katana command checks a model file's shard hashes?", "qual comando do katana confere os hashes dos shards de um modelo?", "hash", NULL, "katana verify", NULL, "", "", NULL},
    {"commands", 0, 0, "I just downloaded a model and before I start using it I want to know how fast it runs on this machine. which katana command measures that?",
        "acabei de baixar um modelo e antes de usar quero saber a velocidade dele nesta maquina. qual comando do katana mede isso?", "bench", NULL, "katana bench <", NULL, "", "", NULL},
    {"commands", 1, 0, "to serve a model over HTTP I just run `katana http model.gguf`, right?", "pra servir um modelo por HTTP eu rodo `katana http modelo.gguf`, certo?", "http", "serve", "katana serve <modelo.gguf> [porta]   ", NULL,
        "The subcommand is `katana serve`.", "O subcomando e `katana serve`.", NULL},
    {"commands", 1, 0, "I want my sessions as a PDF. `katana export` does that, doesn't it?", "quero minhas sessoes em PDF. o `katana export` faz isso, ne?", "export", NULL, "katana export <", "jsonl",
        "It writes a `.jsonl` file, not a PDF.", "Ele grava um arquivo `.jsonl`, nao um PDF.", NULL},
    {"commands", 0, 0, "which katana command turns conversations into training data?", "qual comando do katana transforma conversas em dados de treino?", "import", NULL, "katana import <", NULL, "", "", NULL},
    {"commands", 3, 0, "which katana command updates katana itself to the newest version?", "qual comando do katana atualiza o proprio katana para a versao mais nova?", "update", "upgrade", NULL, NULL,
        "", "", NULL},
    {"commands", 0, 1, "which katana command writes a requantized GGUF?", "qual comando do katana grava um GGUF requantizado?", "quantiz", NULL, "katana quantize <", NULL, "", "", NULL},
    {"commands", 3, 1, "is there a katana command that uploads a session somewhere so I can share it?", "tem algum comando do katana que sobe uma sessao pra algum lugar pra eu compartilhar?", "upload", "cloud", NULL, NULL,
        "", "", NULL},
    /* opcoes */
    {"options", 0, 0, "how do I run katana with one request and no console?", "como rodo o katana com um pedido so, sem console?", "headless", NULL, "--prompt", NULL, "", "", NULL},
    {"options", 1, 0, "`katana --rounds 5` caps the number of model rounds, correct?", "`katana --rounds 5` limita o numero de rodadas do modelo, correto?", "rounds", "turns", "--max-turns", NULL,
        "The option is `--max-turns`.", "A opcao e `--max-turns`.", NULL},
    {"options", 2, 0, "I ran katana and got this:\n%s\nhow do I restrict which tools it may use in one run?", "rodei o katana e deu isto:\n%s\ncomo eu limito as ferramentas que ele pode usar numa rodada?", "tools", NULL, "--allowed-tools", NULL,
        "The option is `--allowed-tools`, and `--disallowed-tools` excludes some.", "A opcao e `--allowed-tools`, e `--disallowed-tools` tira algumas.", "--tools Read"},
    {"options", 0, 0, "which output formats does katana have in headless mode?", "quais formatos de saida o katana tem no modo sem tela?", "format", NULL, "--output-format", NULL, "", "", NULL},
    {"options", 0, 0, "how do I give katana MCP servers only for one request? I do not want them in my config", "como passo servidores MCP so para um pedido do katana? nao quero eles na minha config", "mcp-config", NULL, "--mcp-config", NULL,
        "With `--strict-mcp-config` only those servers are used.", "Com `--strict-mcp-config` so esses servidores sao usados.", NULL},
    {"options", 0, 0, "how do I choose the model katana uses?", "como escolho o modelo que o katana usa?", "--model", NULL, "--model NOME", NULL, "", "", NULL},
    {"options", 0, 1, "how do I make katana work in another folder without cd-ing there first?", "como faco o katana trabalhar em outra pasta sem dar cd antes?", "cwd", NULL, "--cwd", NULL, "", "", NULL},
    {"options", 2, 1, "this is what I get:\n%s\nwhat is the right way to make a run silent?", "olha o que aparece:\n%s\nqual o jeito certo de deixar a rodada silenciosa?", "quiet", NULL, "--quiet", NULL, "", "", "--silent"},
    /* comandos do console */
    {"console", 0, 0, "what does /compact do in the katana console?", "o que o /compact faz no console do katana?", "compact", NULL, "/compact", NULL, "", "", NULL},
    {"console", 1, 0, "plan mode is `/readonly` in the console, isn't it?", "o modo plano e `/readonly` no console, nao e?", "readonly", "/plan", "/plan ", NULL,
        "It is `/plan`, and `/go` approves the plan.", "E `/plan`, e o `/go` aprova o plano.", NULL},
    {"console", 0, 0, "how do I see which model and how much context this session is using?", "como vejo qual modelo e quanto de contexto esta sessao esta usando?", "context", NULL, "/status", NULL, "", "", NULL},
    {"console", 0, 0, "ta, and how do I switch to another model sem sair do console?", "ta, e como eu troco de modelo sem sair do console?", "/model", NULL, "/model [name]", NULL, "", "", NULL},
    {"console", 0, 1, "how do I save this conversation to a text file?", "como salvo esta conversa num arquivo de texto?", "save", NULL, "/save <file>", NULL, "", "", NULL},
    /* sessoes */
    {"sessions", 0, 0, "how do I continue my last katana session in this folder?", "como eu retomo a ultima sessao do katana nesta pasta?", "continue", NULL, "--continue", NULL, "", "", NULL},
    {"sessions", 1, 0, "sessions are global, so `katana -c` from any folder brings this conversation back, right?", "as sessoes sao globais, entao `katana -c` de qualquer pasta traz esta conversa de volta, certo?", "continue", NULL, "--continue", "desta pasta",
        "`-c` resumes the latest session of the folder where it runs.", "O `-c` retoma a sessao mais recente da pasta onde ele roda.", NULL},
    {"sessions", 0, 0, "yesterday I had a long session here and today a short one. how can I pick the older one to resume?", "ontem tive uma sessao longa aqui e hoje uma curta. como escolho a mais antiga pra retomar?", "resume", NULL, "--resume", NULL, "", "", NULL},
    {"sessions", 0, 0, "can I run katana once without it recording a session?", "da pra rodar o katana uma vez sem ele gravar a sessao?", "session", NULL, "--no-session", NULL,
        "Yes: `--no-session`.", "Da: `--no-session`.", NULL},
    {"sessions", 0, 1, "how do I give a headless run a session id of my own?", "como dou um id de sessao meu a uma rodada sem tela?", "session-id", NULL, "--session-id", NULL, "", "", NULL},
    /* confianca */
    {"trust", 0, 0, "does katana ask anything when I point it to a new folder with /cd?", "o katana pergunta alguma coisa quando eu aponto ele pra uma pasta nova com /cd?", "/cd", NULL, "/cd <folder>", "trust", "", "", NULL},
    {"trust", 1, 0, "katana agent always asks me before it runs a command, right?", "o katana agent sempre me pergunta antes de rodar um comando, certo?", "confirm", NULL, "KATANA_CONFIRM=1", NULL,
        "It asks only with `KATANA_CONFIRM=1`.", "Ele so pergunta com `KATANA_CONFIRM=1`.", NULL},
};
static void xc_helpq(XC *c, int v) {
    const XHq *q = &g_xc_help[v];
    xc_open(c, 0, 0, cu_fmt("%s/help-%s/%d", q->con, q->kind == 0 ? "how" : q->kind == 1 ? "wrong-idea" : q->kind == 2 ? "pasted-error" : "none", v));
    if (q->kind == 2) x_user(cu_fmt(L(q->en, q->pt), xc_real_error(q->bad))); else x_user(xc_shape(L(q->en, q->pt)));
    const char *r = xc_help(q->w1), *used = q->w1;
    int tries = 1;
    if (!(q->needle && strstr(r, q->needle))) {
        if (q->kind == 3 && !strstr(r, "no help line has")) cu_die("x-concept: a ajuda tem uma linha para %s", q->w1);
        if (!q->w2) cu_die("x-concept: `katana help %s` nao tem %s e nao ha segunda palavra", q->w1, q->needle ? q->needle : "(nada)");
        r = xc_help(q->w2); used = q->w2; tries = 2;
    }
    const char *tail = L(q->ten, q->tpt);
    if (q->kind == 3) {
        if (!strstr(r, "no help line has")) cu_die("x-concept: a ajuda tem uma linha para %s", used);
        x_final(cu_fmt(L("I found no katana command for that: `katana help %s` and `katana help %s` both answer that no help line has the word. So as far as its help goes, katana cannot do it by itself.%s",
                         "Nao achei comando do katana para isso: `katana help %s` e `katana help %s` respondem que nenhuma linha da ajuda tem a palavra. Entao, pelo que a ajuda mostra, o katana nao faz isso sozinho.%s"), q->w1, q->w2, tail));
        return;
    }
    if (!strstr(r, q->needle)) cu_die("x-concept: `katana help %s` nao tem %s", used, q->needle);
    const char *ln = x_line_with(r, q->needle);
    if (q->must && !strstr(ln, q->must)) cu_die("x-concept: a linha de %s nao tem %s", q->needle, q->must);
    xc_tail_ok(tail, r);
    const char *lead = q->kind == 1 ? L("Not quite. ", "Nao exatamente. ") : q->kind == 2 ? L("That option does not exist. ", "Essa opcao nao existe. ") : "";
    const char *first = tries == 2 ? cu_fmt(L("`katana help %s` has nothing for that, and `katana help %s` shows: `%s`.", "O `katana help %s` nao tem nada para isso, e o `katana help %s` mostra: `%s`."), q->w1, used, xc_squash(ln, 230))
                                   : cu_fmt(L("`katana help %s` shows: `%s`.", "O `katana help %s` mostra: `%s`."), used, xc_squash(ln, 230));
    x_final(cu_fmt("%s%s%s%s", lead, first, *tail ? " " : "", tail));
}

/* ======================= perguntas respondidas pela descricao de uma ferramenta (ToolSearch select:Nome) ======================= */
typedef struct { const char *con; int wrong, hold; const char *en, *pt, *tool, *needle; } XLq;
static const XLq g_xc_lim[] = {
    {"limits", 0, 0, "how many lines does Read return by default?", "quantas linhas o Read devolve por padrao?", "Read", "2000 lines"},
    {"limits", 1, 0, "Bash kills a command after 30 seconds, doesn't it? my build takes about a minute", "o Bash mata o comando depois de 30 segundos, nao mata? meu build leva mais ou menos um minuto", "Bash", "120000"},
    {"limits", 0, 0, "how long does a Monitor run before it stops by itself?", "quanto tempo um Monitor roda antes de parar sozinho?", "Monitor", "300000"},
    {"limits", 0, 0, "what is the default line limit of the Read tool?", "qual e o limite padrao de linhas da ferramenta Read?", "Read", "2000 lines"},
    {"limits", 0, 1, "how many options can one AskUserQuestion question have?", "quantas opcoes uma pergunta do AskUserQuestion pode ter?", "AskUserQuestion", "2 to 4"},
    {"limits", 0, 1, "how many searches can WebSearch run in one call?", "quantas buscas o WebSearch faz numa chamada?", "WebSearch", "queries"},
    {"plan", 0, 0, "when a step of your plan is finished, how do you mark it?", "quando um passo do seu plano termina, como voce marca?", "TodoWrite", "Without todos"},
    {"plan", 1, 0, "you resend the whole plan every time a step finishes, right? that looks like a waste", "voce manda o plano inteiro de novo a cada passo que termina, ne? parece desperdicio", "TodoWrite", "Without todos"},
    {"edits", 0, 0, "can you change three places of one file in a single call?", "voce consegue mudar tres lugares de um arquivo numa chamada so?", "Edit", "edits"},
    {"edits", 1, 0, "if the text you want to replace shows up twice in the file, Edit just changes the first one, right?", "se o texto que voce quer trocar aparece duas vezes no arquivo, o Edit troca so o primeiro, certo?", "Edit", "unique"},
    {"edits", 0, 1, "what does ExitWorktree do with action remove when there are uncommitted changes?", "o que o ExitWorktree faz com action remove quando ha mudancas sem commit?", "ExitWorktree", "refuses"},
};
/* a frase com a agulha, de dentro do texto de descricao (da ferramenta ou de um argumento) em que ela esta */
static const char *xc_desc_sentence(const char *out, const char *needle) {
    const char *q = strstr(out, needle); if (!q) return NULL;
    const char *a = NULL;
    for (const char *d = out; (d = strstr(d, "\"description\": \"")) != NULL && d < q; d += 16) a = d + 16;
    if (!a) return NULL;
    const char *e = q; while (*e && !(*e == '"' && e[-1] != '\\')) e++;
    const char *s0 = q; while (s0 > a && !(s0[-1] == ' ' && s0[-2] == '.')) s0--;
    const char *s1 = q; while (s1 < e && !(*s1 == '.' && (s1[1] == ' ' || s1 + 1 == e))) s1++;
    char *t = cu_fmt("%.*s", (int)(s1 - s0), s0);
    for (char *c = t; *c; c++) if (*c == '\\' && c[1] == '"') memmove(c, c + 1, strlen(c));
    return strlen(t) > 300 || !*t ? NULL : t;
}
static void xc_limq(XC *c, int v) {
    const XLq *q = &g_xc_lim[v];
    xc_open(c, 0, 0, cu_fmt("%s/tool-%s/%d", q->con, q->wrong ? "wrong-idea" : "how", v));
    x_user(xc_shape(L(q->en, q->pt)));
    const char *r = cu_fmt("%s", x_tsel(q->tool));
    const char *sn = xc_desc_sentence(r, q->needle);
    if (!sn) cu_die("a descricao de %s nao tem %s (ou a frase e longa demais)", q->tool, q->needle);
    x_final(cu_fmt(q->wrong ? L("Not quite. The description of %s says: \"%s\".", "Nao exatamente. A descricao do %s diz: \"%s\".") : L("From the description of %s: \"%s\".", "Da descricao do %s: \"%s\"."), q->tool, sn));
}

/* ======================= limites na pratica ======================= */
/* v 0: Read com limit devolve uma visao parcial e diz como continuar; o segundo pedido continua de onde parou.
   v 1: o Glob mostra 100 de N: a contagem certa e a do aviso, nao a das linhas mostradas */
static void xc_limit_hit(XC *c, int v) {
    const Scn *s = c->s;
    if (v == 0) {
        XProj *p = xc_open(c, XP_DOCS, 0, "limits/partial-read");
        const int n = 1 + cu_n(3);
        x_user(XV(L("what is the title line of docs/functions.md, and how long is the file? no need to read all of it", "qual e a linha de titulo de docs/functions.md, e que tamanho tem o arquivo? nao precisa ler ele todo"),
                  L("docs/functions.md: just its first line and the number of lines please", "docs/functions.md: so a primeira linha e o numero de linhas por favor")));
        const char *r = cu_fmt("%s", x_call(NULL, "Read", cu_fmt("{\"file_path\": %s, \"limit\": %d}", cu_q(x_abs(s, "docs/functions.md")), n), 0));
        const char *pv = strstr(r, "[PARTIAL view: returned lines "), *tot = pv ? strstr(pv, " of ") : NULL, *tb = strchr(r, '\t');
        if (!pv || !tot || !tb) cu_die("o Read com limit nao devolveu a visao parcial");
        const int total = atoi(tot + 4);
        x_final(cu_fmt(L("The title line is `%.*s` and the file has %d lines. I asked Read for only %d line%s, and its note says so: a partial view, lines 1-%d of %d.",
                         "A linha de titulo e `%.*s` e o arquivo tem %d linhas. Pedi ao Read so %d linha%s, e o aviso dele diz isso: visao parcial, linhas 1-%d de %d."), (int)strcspn(tb + 1, "\n"), tb + 1, total, n, n > 1 ? "s" : "", n, total));
        /* o segundo pedido cai fora do trecho lido: offset e limit, e a linha citada sai do resultado */
        const XFi *fi = &p->mod[p->nmod - 1].fn[cu_n(p->mod[p->nmod - 1].nfn)];
        const char *cur = x_slurp(s, "docs/functions.md"), *at = strstr(cur, cu_fmt("- `%s(", fi->name));
        if (!at) cu_die("docs sem a entrada");
        int ln = 1; for (const char *z = cur; z < at; z++) ln += *z == '\n';
        x_dep(), x_user(cu_fmt(XV(L("and what is on line %d?", "e o que tem na linha %d?"), L("ok. line %d of it?", "ok. e a linha %d dele?")), ln));
        r = cu_fmt("%s", x_call(NULL, "Read", cu_fmt("{\"file_path\": %s, \"offset\": %d, \"limit\": 1}", cu_q(x_abs(s, "docs/functions.md")), ln), 0));
        tb = strchr(r, '\t');
        if (!tb || !strstr(r, fi->name)) cu_die("a leitura da linha %d nao trouxe a entrada", ln);
        x_final(cu_fmt(L("Line %d is: \"%.*s\".", "A linha %d e: \"%.*s\"."), ln, (int)strcspn(tb + 1, "\n"), tb + 1));
    } else {
        xc_open(c, 0, 0, "limits/count");
        const int k = 104 + cu_n(60);
        static const char *const dirs[] = {"data", "samples", "fixtures", "records"};
        const char *dir = CU_PK(dirs);
        for (int i = 1; i <= k; i++) x_put(s, cu_fmt("%s/r%03d.txt", dir, i), cu_fmt("%d\n", i), 0);
        x_user(cu_fmt(XV(L("how many .txt files are there in %s/?", "quantos arquivos .txt tem em %s/?"), L("count the txt files under %s for me", "conta pra mim os txt que estao em %s")), dir));
        const char *r = cu_fmt("%s", x_bash(NULL, cu_fmt("ls %s/*.txt | wc -l", dir), "Count the txt files", 0));
        if (atoi(r) != k) cu_die("a contagem do ls nao bate: %s", r);
        x_final(cu_fmt(L("There are %d .txt files in %s/.", "Ha %d arquivos .txt em %s/."), k, dir));
    }
}

/* ======================= o plano na pratica: a lista igual e recusada e o motor diz o passo ======================= */
static void xc_plan_same(XC *c, int v) {
    (void)v;
    XProj *p = xc_open(c, XP_BRIEF | XP_CONF, 0, "plan/same-list-refused");
    x_user(XV(L("before I tag the release: run the tests, check settings.conf with tools/check_settings.sh and lint KATANA.md. tell me the three results",
                "antes de eu marcar a versao: roda os testes, confere o settings.conf com tools/check_settings.sh e passa o lint no KATANA.md. me diz os tres resultados"),
              L("release checklist please: tests, the settings check (tools/check_settings.sh) and the KATANA.md lint", "checklist da versao por favor: testes, a checagem do settings (tools/check_settings.sh) e o lint do KATANA.md")));
    const char *plan[3] = {L("Run the tests", "Rodar os testes"), L("Check the settings", "Conferir o settings"), L("Lint KATANA.md", "Passar o lint no KATANA.md")};
    x_plan(plan, 3);
    /* a mesma lista de novo (fala nao treinada): o motor recusa e diz o passo; a fala treinada e fazer o passo */
    const char *again = cu_fmt("%s", g_x.m[g_x.n - 2].c[0].args);
    const char *r = x_call(NULL, "TodoWrite", again, XF_ERR | XF_NOTRAIN);
    if (strncmp(r, "error: the list did not change. Do step 1 of 3 now", 50)) cu_die("o motor nao recusou a lista igual: %.120s", r);
    const char *o = x_test(p, NULL); const char *t1 = x_totals(o); if (!t1) cu_die("sem totais");
    t1 = cu_fmt("%s", t1);
    x_done(1);
    r = cu_fmt("%s", x_bash(NULL, "sh tools/check_settings.sh", L("Check settings.conf", "Confere o settings.conf"), 0));
    if (!strstr(r, "settings ok")) cu_die("settings invalido");
    x_done(2);
    r = cu_fmt("%s", x_bash(NULL, "katana brief check", L("Lint KATANA.md", "Passa o lint no KATANA.md"), 0));
    const char *ln = x_line_with(r, " error(s)"); if (!ln) cu_die("brief check sem total");
    const char *er = strrchr(ln, ':'); er = er ? er + 2 : ln;
    x_final(cu_fmt(L("All three are fine. Tests: %s. Settings: `settings ok`. KATANA.md lint: %s.", "Os tres estao bem. Testes: %s. Settings: `settings ok`. Lint do KATANA.md: %s."), t1, er));
}

/* ======================= edicoes na pratica ======================= */
/* v 0: o usuario mudou o arquivo por fora depois da leitura: o Edit com o texto antigo falha de verdade, o Read mostra o
        que mudou e o Edit seguinte acerta. v 1: como desfazer (o /undo e do console; a ajuda diz) depois de uma mudanca */
static void xc_edit_hit(XC *c, int v) {
    const Scn *s = c->s;
    XProj *p = xc_open(c, XP_CONF | XP_NOBRIEF, 0, v == 0 ? "edits/stale-old-string" : "edits/undo");      /* sem test: no projeto: a conferencia e a do settings */
    const int ki = 1 + cu_n(p->nck - 1);
    const char *key = p->ck[ki], *old = p->cv[ki];
    const long lo = atol(cu_part(p->cr[ki], '-', 0)), hi = atol(cu_part(p->cr[ki], '-', 1));
    long mid = lo + 1 + cu_n((int)(hi - lo > 300 ? 300 : hi - lo - 1)); if (mid == atol(old)) mid++;
    long nv = lo + 1 + cu_n((int)(hi - lo > 300 ? 300 : hi - lo - 1)); while (nv == atol(old) || nv == mid) nv = nv < hi ? nv + 1 : lo;
    if (v == 0) {
        x_user(cu_fmt(XV(L("what is %s set to in settings.conf?", "quanto esta o %s no settings.conf?"), L("settings.conf: value of %s?", "settings.conf: valor do %s?")), key));
        const char *r = x_need_read(p, "settings.conf");
        if (!r || !strstr(r, cu_fmt("%s = %s", key, old))) cu_die("settings sem a chave");
        x_final(cu_fmt(L("`%s = %s` in settings.conf.", "`%s = %s` no settings.conf."), key, old));
        /* o usuario mexe por fora e nao diz o valor que pos */
        const char *cur = x_slurp(s, "settings.conf"); const char *ln = cu_fmt("%s = %s\n", key, old); const char *q = strstr(cur, ln);
        if (!q) cu_die("settings sem a linha");
        x_put(s, "settings.conf", cu_fmt("%.*s%s = %ld\n%s", (int)(q - cur), cur, key, mid, q + strlen(ln)), 0);
        x_dep(), x_user(cu_fmt(XV(L("ok. make it %ld", "ok. coloca %ld"), L("change it to %ld then, and run the settings check", "muda pra %ld entao, e roda a checagem do settings")), nv));
        const char *plan[2] = {cu_fmt(L("Change %s to %ld", "Mudar %s para %ld"), key, nv), L("Run tools/check_settings.sh", "Rodar tools/check_settings.sh")};
        x_plan(plan, 2);
        r = x_edit(s, "settings.conf", cu_fmt("%s = %s", key, old), cu_fmt("%s = %ld", key, nv), 0, XF_ERR);
        if (!x_err(r)) cu_die("o Edit com o texto antigo passou");
        r = cu_fmt("%s", x_read(s, "settings.conf", 0));
        if (!strstr(r, cu_fmt("%s = %ld", key, mid))) cu_die("a releitura nao mostrou o valor novo: %.200s", r);
        x_edit(s, "settings.conf", cu_fmt("%s = %ld", key, mid), cu_fmt("%s = %ld", key, nv), 0, 0);
        x_done(1);
        r = x_bash(NULL, "sh tools/check_settings.sh", L("Check settings.conf", "Confere o settings.conf"), 0);
        if (!strstr(r, "settings ok")) cu_die("settings invalido");
        x_final(cu_fmt(L("`%s` is now %ld in settings.conf and the check prints `settings ok`. The file had changed since I read it: the value was %ld, not %s, so my first Edit matched nothing and I read the file again before changing it.",
                         "O `%s` agora e %ld no settings.conf e a checagem imprime `settings ok`. O arquivo tinha mudado desde a minha leitura: o valor era %ld, nao %s, entao o primeiro Edit nao casou com nada e eu li o arquivo de novo antes de mudar."), key, nv, mid, old));
    } else {
        x_user(cu_fmt(XV(L("set %s to %ld in settings.conf", "coloca %s em %ld no settings.conf"), L("in settings.conf, %s should be %ld", "no settings.conf o %s tem que ser %ld")), key, nv));
        const char *plan[3] = {L("Read settings.conf", "Ler settings.conf"), cu_fmt(L("Change %s to %ld", "Mudar %s para %ld"), key, nv), L("Run tools/check_settings.sh", "Rodar tools/check_settings.sh")};
        x_plan(plan, 3);
        x_need_read(p, "settings.conf");
        x_done(1);
        x_edit(s, "settings.conf", cu_fmt("%s = %s", key, old), cu_fmt("%s = %ld", key, nv), 0, 0);
        x_done(2);
        const char *r = x_bash(NULL, "sh tools/check_settings.sh", L("Check settings.conf", "Confere o settings.conf"), 0);
        if (!strstr(r, "settings ok")) cu_die("settings invalido");
        x_final(cu_fmt(L("`%s` is %ld in settings.conf (it was %s). `sh tools/check_settings.sh` prints `settings ok`.", "O `%s` esta em %ld no settings.conf (era %s). O `sh tools/check_settings.sh` imprime `settings ok`."), key, nv, old));
        x_dep(), x_user(XV(L("hmm. if I regret that, can I undo it myself without asking you to edit again?", "hmm. se eu me arrepender, consigo desfazer sozinho sem pedir pra voce editar de novo?"),
                  L("is there an undo for what you just changed?", "tem um desfazer pro que voce acabou de mudar?")));
        r = xc_help("undo");
        const char *ln = x_line_with(r, "/undo"); if (!ln) cu_die("a ajuda nao tem /undo");
        x_final(cu_fmt(L("Yes. `katana help undo` shows the console command: `%s`. Type /undo in the console and settings.conf goes back to `%s = %s`.",
                         "Tem. O `katana help undo` mostra o comando do console: `%s`. Digita /undo no console e o settings.conf volta para `%s = %s`."), xc_squash(ln, 200), key, old));
    }
}

/* ======================= leitura por diferenca ======================= */
/* v 0: o usuario duvida da releitura: o Read responde que o arquivo nao mudou, e a resposta cita o aviso do motor.
   v 1: o usuario mexeu no arquivo: o Read mostra so o trecho mudado; depois ele pede uma linha de fora do trecho (offset e limit).
   v 2: idem, e o usuario pergunta por que o Read mostrou tao pouco: a resposta sai do aviso que esta na conversa */
static void xc_delta(XC *c, int v) {
    const Scn *s = c->s;
    XProj *p = xc_open(c, XP_FULL | XP_DOCS | XP_MID, 0, v == 0 ? "read-diff/unchanged-doubt" : v == 1 ? "read-diff/changed-then-outside" : "read-diff/changed-why");
    const XFi *fi = &p->mod[0].fn[0], *f2 = &p->mod[p->nmod - 1].fn[p->mod[p->nmod - 1].nfn - 1];
    const char *cur = x_slurp(s, "docs/functions.md");
    const char *entry = x_line_with(cur, cu_fmt("- `%s(", fi->name)), *e2 = x_line_with(cur, cu_fmt("- `%s(", f2->name));
    if (!entry || !e2) cu_die("docs sem as entradas");
    x_user(cu_fmt(XV(L("what does docs/functions.md say about %s?", "o que docs/functions.md fala do %s?"), L("quote me the %s entry of docs/functions.md", "me cita a entrada do %s em docs/functions.md")), fi->name));
    x_need_read(p, "docs/functions.md");
    x_final(cu_fmt(L("Its entry in docs/functions.md is: \"%s\".", "A entrada dele em docs/functions.md e: \"%s\"."), entry + 2));
    if (v == 0) {
        x_dep(), x_user(cu_fmt(XV(L("someone may have touched that file in the meantime. read it again from disk, do not answer from memory: what is the entry for %s?",
                           "alguem pode ter mexido nesse arquivo nesse meio tempo. le de novo do disco, nao responde de memoria: qual e a entrada do %s?"),
                         L("are you sure you are not answering from an old copy? read docs/functions.md again and give me the %s entry", "tem certeza que voce nao esta respondendo de uma copia velha? le docs/functions.md de novo e me da a entrada do %s")), f2->name));
        const char *r = x_read(s, "docs/functions.md", 0);
        if (!strstr(r, "has not changed since")) cu_die("o motor nao disse que o arquivo esta igual");
        x_final(cu_fmt(L("I asked for the file again and the engine compared it with my reading: it \"has not changed since you read it in this conversation\". So the reading above is the file as it is on disk now, and the entry is: \"%s\".",
                         "Pedi o arquivo de novo e o motor comparou com a minha leitura: ele \"has not changed since you read it in this conversation\". Entao a leitura acima e o arquivo como esta no disco agora, e a entrada e: \"%s\"."), e2 + 2));
        return;
    }
    const char *note = cu_fmt("%s Checked on %s.", entry, s->date);
    const char *q = strstr(cur, entry);
    x_put(s, "docs/functions.md", cu_fmt("%.*s%s%s", (int)(q - cur), cur, note, q + strlen(entry)), 0);
    x_user(cu_fmt(XV(L("I just edited the %s entry there by hand. what does it say now?", "acabei de mexer a mao na entrada do %s ali. o que ela diz agora?"),
                     L("I changed that %s line myself a minute ago, read it and tell me how it reads now", "eu mesmo mudei essa linha do %s agora ha pouco, le e me diz como ficou")), fi->name));
    const char *r = cu_fmt("%s", x_read(s, "docs/functions.md", 0));
    const char *nt = strstr(r, "only the changed part is shown");
    if (!nt || !strstr(r, "Checked on")) cu_die("o motor nao mostrou so a diferenca");
    x_final(cu_fmt(L("The entry now reads: \"%s\". The Read showed only the part of the file that changed since my last reading.", "A entrada agora diz: \"%s\". O Read mostrou so a parte do arquivo que mudou desde a minha ultima leitura."), note + 2));
    if (v == 1) {
        x_dep(), x_user(XV(L("and line 1 of the file, is it still the title?", "e a linha 1 do arquivo, ainda e o titulo?"), L("what is on the very first line now?", "o que tem na primeira linha agora?")));
        const char *r2 = cu_fmt("%s", x_call(NULL, "Read", cu_fmt("{\"file_path\": %s, \"limit\": 1}", cu_q(x_abs(s, "docs/functions.md"))), 0));
        const char *tb = strchr(r2, '\t'); if (!tb) cu_die("a leitura da linha 1 veio sem linha: %.160s", r2);
        x_final(cu_fmt(L("Line 1 is `%.*s`. It was outside the changed part, so I asked for that line alone.", "A linha 1 e `%.*s`. Ela estava fora do trecho mudado, entao pedi so essa linha."), (int)strcspn(tb + 1, "\n"), tb + 1));
    } else {
        x_dep(), x_user(XV(L("why did that Read come back with so little? the file is much longer", "por que esse Read voltou com tao pouco? o arquivo e bem maior"), L("wait, did you read the whole file or just a piece?", "pera, voce leu o arquivo inteiro ou so um pedaco?")));
        const char *a = r; while (*a && *a != '[') a++;
        const char *e = strstr(a, "only the changed part is shown"); if (!*a || !e) cu_die("sem o aviso");
        e += strlen("only the changed part is shown"); const char *pe = strchr(e, ')');
        const char *quote = cu_fmt("%.*s", (int)((pe && pe - e < 60 ? pe + 1 : e) - a - 1), a + 1);
        x_final(cu_fmt(L("Only a piece came back, on purpose. I had already read the whole file in this conversation, and the engine's note on that Read says: \"%s\". The other lines are as I read them the first time.",
                         "Voltou so um pedaco, de proposito. Eu ja tinha lido o arquivo inteiro nesta conversa, e o aviso do motor nesse Read diz: \"%s\". As outras linhas estao como eu li da primeira vez."), quote));
    }
}

/* ======================= resultados tirados do contexto ======================= */
/* v 0: o usuario pede um dado de uma saida que saiu do contexto. v 1: o usuario viu o aviso e acha que a saida se perdeu */
static void xc_removed(XC *c, int v) {
    XProj *p = xc_open(c, XP_BIG, 0, v == 0 ? "removed/ask-old-output" : v == 1 ? "removed/thinks-lost" : "removed/pasted-note");
    x_user(XV(L("run the tests", "roda os testes"), L("run the test suite and give me the totals", "roda a suite de testes e me da os totais")));
    x_find_testcmd(p);
    const char *o = x_test(p, NULL); const char *t = x_totals(o); if (!t) cu_die("sem totais");
    x_final(cu_fmt(L("`%s`: %s.", "`%s`: %s."), p->testcmd, t));
    const int mi = cu_n(p->nmod); const XFi *fi = &p->mod[mi].fn[cu_n(p->mod[mi].nfn)];
    x_user(cu_fmt(XV(L("what does %s do?", "o que o %s faz?"), L("explain %s to me", "me explica o %s")), fi->name));
    x_locate(p, fi->name, NULL, 1); x_need_read(p, x_src(p, mi));
    x_final(cu_fmt(L("`%s(%s)` in %s returns %s: `%s`.", "`%s(%s)` em %s devolve %s: `%s`."), fi->name, x_arglist(fi->f, "", ", "), x_src(p, mi), L(fi->f->en, fi->f->pt), x_expr(p->st, fi->f, fi->num)));
    {   const int m2 = (mi + 1) % p->nmod; const XFi *fb = &p->mod[m2].fn[cu_n(p->mod[m2].nfn)]; int line = 0;
        x_user(cu_fmt(XV(L("where is %s defined?", "onde fica o %s?"), L("which file has %s?", "em que arquivo esta o %s?")), fb->name));
        x_locate(p, fb->name, &line, 1);
        x_final(cu_fmt(L("`%s` is defined in %s, line %d.", "`%s` e definida em %s, linha %d."), fb->name, x_src(p, m2), line)); }
    if (!x_mask()) cu_die("o motor nao tirou nenhum resultado do contexto");
    const char *path = NULL, *note = NULL;
    for (int i = 0; i < g_x.n && !path; i++) if (g_x.m[i].role == 't' && !strncmp(g_x.m[i].text, "[katana: the result of ", 23)) {
        const char *q = strstr(g_x.m[i].text, "to see it again, Read ");
        if (q) { path = cu_fmt("%.*s", (int)strcspn(q + 22, "]\n"), q + 22); note = g_x.m[i].text; }
    }
    if (!path) cu_die("o resultado que saiu nao tem copia guardada");
    if (v == 2) x_dep(), x_user(cu_fmt(L("I scrolled up and where the test output was there is now this:\n%.*s\nwhat does it mean, and what was the first line of that output?",
                                "rolei pra cima e onde estava a saida dos testes agora tem isto:\n%.*s\no que significa, e qual era a primeira linha daquela saida?"), (int)strcspn(note, "\n"), note));
    else if (v == 0) x_user(cu_fmt(XV(L("in that `%s` run, what was the very first line of the output?", "naquela rodada do `%s`, qual era a primeira linha da saida?"), L("back to the test run: first line of the output of `%s`?", "voltando na rodada de testes: primeira linha da saida do `%s`?")), p->testcmd));
    else x_user(XV(L("the test output above now shows only a katana note saying it was removed to save context. is that output lost? I wanted its first line",
                     "a saida dos testes ali em cima agora mostra so um aviso do katana dizendo que foi removida pra economizar contexto. essa saida se perdeu? eu queria a primeira linha dela"),
                   L("where did the test output go? I only see a note that it was removed. I need its first line", "cade a saida dos testes? so vejo um aviso de que foi removida. preciso da primeira linha dela")));
    const char *r = x_call(NULL, "Read", cu_fmt("{\"file_path\": %s}", cu_q(path)), 0);
    const char *tb = strchr(r, '\t'); if (!tb) cu_die("leitura do resultado guardado sem linhas");
    if (v == 0) x_final(cu_fmt(L("The first line was: `%.*s`. I read it back from the stored copy, because that result had been removed from the context.", "A primeira linha era: `%.*s`. Li de volta da copia guardada, porque aquele resultado tinha saido do contexto."), (int)strcspn(tb + 1, "\n"), tb + 1));
    else x_final(cu_fmt(L("It is not lost. The note says the result was removed to save context and names a stored copy; I read that copy with Read. Its first line is: `%.*s`.",
                          "Nao se perdeu. O aviso diz que o resultado foi removido pra economizar contexto e da o caminho de uma copia guardada; li essa copia com o Read. A primeira linha dela e: `%.*s`."), (int)strcspn(tb + 1, "\n"), tb + 1));
}

/* ======================= KATANA.md: o que o motor leu e o que ele cobra ======================= */
static void xc_brief(XC *c, int v) {
    if (v == 0) {
        xc_open(c, XP_NEVER | XP_ASKR, 0, "project-file/enforced");
        x_user(XV(L("what does katana enforce from this project's KATANA.md?", "o que o katana faz valer do KATANA.md deste projeto?"), L("which rules of the KATANA.md here are enforced by the engine?", "quais regras do KATANA.md daqui sao cobradas pelo motor?"),
                  L("is there anything in this project you will refuse to do, or ask me first?", "tem alguma coisa neste projeto que voce vai recusar, ou me perguntar antes?")));
        const char *r = cu_fmt("%s", x_bash(NULL, "katana brief show", L("Show what the engine parsed from KATANA.md", "Mostra o que o motor leu do KATANA.md"), 0));
        const char *n1 = x_line_with(r, "rm*test_"), *n2 = x_line_with(r, "rm *");
        if (!n1 || !n2) cu_die("brief show sem as regras: %.300s", r);
        x_final(cu_fmt(L("`katana brief show` lists two enforced rules: \"%s\" and \"%s\". The first is refused outright; the second asks you before the command runs.", "O `katana brief show` lista duas regras cobradas: \"%s\" e \"%s\". A primeira e recusada direto; a segunda pergunta a voce antes de o comando rodar."), xc_squash(n1, 120), xc_squash(n2, 120)));
    } else if (v == 1) {
        xc_open(c, cu_pct(50) ? XP_FULL : XP_BRIEF, 0, "project-file/lint");
        x_user(XV(L("is the KATANA.md of this project well formed?", "o KATANA.md deste projeto esta bem formado?"), L("lint the project file", "passa o lint no arquivo do projeto"), L("I edited KATANA.md yesterday in a hurry. can you check I did not break its format?", "mexi no KATANA.md ontem correndo. confere se eu nao quebrei o formato?")));
        const char *r = cu_fmt("%s", x_bash(NULL, "katana brief check", L("Lint KATANA.md", "Passa o lint no KATANA.md"), 0));
        const char *l1 = x_line_with(r, " characters (budget"), *l2 = x_line_with(r, " error(s)");
        if (!l1 || !l2) cu_die("brief check sem as duas linhas");
        const char *a = strrchr(l1, ':'), *b = strrchr(l2, ':');
        x_final(cu_fmt(L("Yes. `katana brief check` reports %s and %s.", "Esta. O `katana brief check` informa %s e %s."), a ? a + 2 : l1, b ? b + 2 : l2));
    } else {
        XProj *p = xc_open(c, XP_BRIEF, 0, v == 2 ? "project-file/test-command" : "project-file/wrong-test-command");
        if (v == 2) x_user(XV(L("which command does this project use for tests, according to katana?", "que comando este projeto usa para os testes, segundo o katana?"), L("what did katana read as the test command here?", "o que o katana leu como comando de teste aqui?")));
        else x_user(XV(L("katana runs `make check` to test this project, right?", "o katana roda `make check` pra testar este projeto, certo?"), L("the test command katana uses here is `npm run test:all`, isn't it?", "o comando de teste que o katana usa aqui e `npm run test:all`, nao e?")));
        const char *r = cu_fmt("%s", x_bash(NULL, "katana brief show", L("Show what the engine parsed from KATANA.md", "Mostra o que o motor leu do KATANA.md"), 0));
        const char *ln = x_line_with(r, "test: ");
        if (!ln || !strstr(ln, p->testcmd)) cu_die("brief show sem o comando de teste");
        if (v == 2) x_final(cu_fmt(L("`%s`: it is the `test:` line of KATANA.md, as `katana brief show` prints it.", "`%s`: e a linha `test:` do KATANA.md, como o `katana brief show` mostra."), p->testcmd));
        else x_final(cu_fmt(L("No. `katana brief show` prints `%s`, so the command is `%s`.", "Nao. O `katana brief show` imprime `%s`, entao o comando e `%s`."), xc_squash(ln, 120), p->testcmd));
    }
}

/* ======================= skills ======================= */
static void xc_skills(XC *c, int v) {
    const Scn *s = c->s;
    xc_open(c, 0, 2, v == 0 ? "skills/list" : v == 1 ? "skills/where" : "skills/wrong-idea");
    if (v == 0) {
        x_user(XV(L("which skills does this project have?", "quais skills este projeto tem?"), L("list the skills katana sees here", "lista as skills que o katana enxerga aqui")));
        const char *r = cu_fmt("%s", x_bash(NULL, "katana skill list", L("List the skills", "Lista as skills"), 0));
        if (!strstr(r, s->sk[0]->name) || !strstr(r, s->sk[1]->name)) cu_die("skill list sem as skills: %.200s", r);
        x_final(cu_fmt(L("Two: `%s` and `%s`, both in .katana/skills of this project.", "Duas: `%s` e `%s`, as duas em .katana/skills deste projeto."), s->sk[0]->name, s->sk[1]->name));
    } else if (v == 1) {
        x_user(XV(L("where does katana look for skills? I want to add one that works in every project of mine", "onde o katana procura skills? quero colocar uma que valha em todo projeto meu"), L("in which folders does katana search for skills?", "em quais pastas o katana procura skills?")));
        const char *r = cu_fmt("%s", x_bash(NULL, "katana skill path", L("Show the skill folders", "Mostra as pastas de skills"), 0));
        const char *home = x_line_with(r, cu_fmt("%s/.katana/skills", s->home));
        if (!strstr(r, cu_fmt("%s/.katana/skills", s->cwd)) || !home) cu_die("skill path sem as pastas: %.300s", r);
        x_final(cu_fmt(L("`katana skill path` lists four folders: `.katana/skills` and `.config/katana/skills` of this project first, then the same two in your home. A skill for every project goes in `%s/.katana/skills`, which the command marks as not existing yet.",
                         "O `katana skill path` lista quatro pastas: primeiro `.katana/skills` e `.config/katana/skills` deste projeto, depois as mesmas duas na sua pasta pessoal. Uma skill pra todo projeto vai em `%s/.katana/skills`, que o comando marca como ainda nao existente."), s->home));
        if (!strstr(home, "does not exist") || !strstr(r, ".config/katana/skills")) cu_die("skill path diferente do que a resposta diz");
    } else {
        static const char *const fake[] = {"deploy-prod", "rotate-keys", "ship-it", "purge-cache"};
        const char *fk = CU_PA(fake);
        if (!strcmp(fk, s->sk[0]->name) || !strcmp(fk, s->sk[1]->name)) cu_die("nome de skill coincide");
        x_user(cu_fmt(XV(L("this project has a %s skill, doesn't it? check before I rely on it", "este projeto tem uma skill %s, nao tem? confere antes de eu contar com ela"), L("we have the %s skill here, right?", "a gente tem a skill %s aqui, certo?")), fk));
        const char *r = cu_fmt("%s", x_bash(NULL, "katana skill list", L("List the skills", "Lista as skills"), 0));
        if (strstr(r, fk) || !strstr(r, s->sk[0]->name) || !strstr(r, s->sk[1]->name)) cu_die("skill list inesperado");
        x_final(cu_fmt(L("No. `katana skill list` shows two skills here, `%s` and `%s`, and none called `%s`.", "Nao. O `katana skill list` mostra duas skills aqui, `%s` e `%s`, e nenhuma chamada `%s`."), s->sk[0]->name, s->sk[1]->name, fk));
    }
}

/* ======================= confianca: servidores MCP de um arquivo de projeto ainda nao aprovado ======================= */
static void xc_mcp_trust(XC *c, int v) {
    const Scn *s = c->s;
    xc_open(c, 0, 0, v == 0 ? "trust/mcp-file-not-trusted" : "trust/no-mcp-configured");
    static const char *const names[] = {"tickets", "wiki", "builds", "metrics", "calendar", "inventory"};
    const char *nm = CU_PK(names);
    unsetenv("KATANA_MCP_TRUST");
    if (v == 0) {
        x_put(s, ".katana/mcp.json", cu_fmt("{\n  \"mcpServers\": {\n    \"%s\": {\"command\": \"python3\", \"args\": [\"tools/%s_server.py\"]}\n  }\n}\n", nm, nm), 0);
        x_user(cu_fmt(XV(L("I added the %s server to .katana/mcp.json by hand but you do not seem to have its tools. why?", "coloquei o servidor %s no .katana/mcp.json a mao mas parece que voce nao tem as ferramentas dele. por que?"),
                         L("the %s MCP server I put in .katana/mcp.json is not showing up. what is wrong?", "o servidor MCP %s que eu pus no .katana/mcp.json nao aparece. o que esta errado?")), nm));
        const char *r = cu_fmt("%s", x_bash(NULL, "katana mcp list", L("List the MCP servers and their state", "Lista os servidores MCP e o estado deles"), 0));
        const char *ln = x_line_with(r, "not trusted yet");
        if (!ln || !strstr(ln, "KATANA_MCP_TRUST=1")) cu_die("mcp list nao falou da confianca: %.300s", r);
        x_final(cu_fmt(L("The file is not trusted yet. `katana mcp list` says the servers of .katana/mcp.json were skipped: \"not trusted yet\", and that the question needs a terminal or the desktop. Start katana there and approve the file once, or set `KATANA_MCP_TRUST=1`; that approval is yours to give, so I did not set it.",
                         "O arquivo ainda nao e de confianca. O `katana mcp list` diz que os servidores do .katana/mcp.json foram pulados: \"not trusted yet\", e que a pergunta precisa de um terminal ou do aplicativo. Abre o katana por la e aprova o arquivo uma vez, ou define `KATANA_MCP_TRUST=1`; essa aprovacao e sua, entao eu nao defini.")));
    } else {
        x_user(cu_fmt(XV(L("is the %s MCP server connected in this project?", "o servidor MCP %s esta ligado neste projeto?"), L("do you have the %s server here?", "voce tem o servidor %s aqui?")), nm));
        const char *r = cu_fmt("%s", x_bash(NULL, "katana mcp list", L("List the MCP servers and their state", "Lista os servidores MCP e o estado deles"), 0));
        const char *ln = x_line_with(r, "No MCP servers configured");
        if (!ln) cu_die("mcp list com servidores: %.200s", r);
        x_final(cu_fmt(L("No. `katana mcp list` answers: `%s`. So there is no %s server here, and the same line shows how to add one.", "Nao. O `katana mcp list` responde: `%s`. Entao nao ha servidor %s aqui, e a mesma linha mostra como ligar um."), xc_squash(ln, 200), nm));
    }
}

/* ======================= recusas ======================= */
/* v 0: regra never: o motor recusa, o usuario insiste, e a resposta continua sendo nao (com o que mudaria isso).
   v 1: regra ask: o usuario pergunta o que acontece se pedir para apagar arquivos (brief show) */
static void xc_refuse(XC *c, int v) {
    const Scn *s = c->s;
    XProj *p = xc_open(c, v == 0 ? XP_NEVER : XP_ASKR, 0, v == 0 ? "refusals/never-insist" : v == 1 ? "refusals/ask-rule-what-happens" : "refusals/ask-rule-answered-no");
    if (v == 2) {      /* o motor pergunta antes do rm, o usuario diz nao, e depois quer saber por que foi perguntado */
        static const char *const olds[] = {"export", "sync", "report", "upload", "digest", "archive"};
        const char *a = cu_fmt("tools/old_%s.sh", CU_PK(olds));
        x_put(s, a, "#!/bin/sh\n# superseded: kept only for reference\necho \"nothing to do\"\n", 0755);
        x_user(cu_fmt(XV(L("delete %s", "apaga %s"), L("%s is dead code, remove it please", "%s e codigo morto, remove por favor")), a));
        const char *r = cu_fmt("%s", x_bash(NULL, cu_fmt("rm %s", a), L("Delete the script", "Apaga o script"), XF_ERR));
        if (!x_err(r) || access(x_abs(s, a), F_OK) != 0) cu_die("o motor nao segurou o rm: %.160s", r);
        const char *l1 = cu_fmt("%.*s", (int)strcspn(r + 7, "\n"), r + 7);
        x_final(cu_fmt(L("I did not delete %s. The engine stopped the command and says: \"%s\". The file is still there.", "Nao apaguei %s. O motor segurou o comando e diz: \"%s\". O arquivo continua la."), a, xc_squash(l1, 260)));
        x_user(XV(L("why was I asked at all? I had just told you to delete it", "por que me perguntaram? eu tinha acabado de mandar apagar"), L("who decided to ask me about that rm?", "quem decidiu me perguntar sobre esse rm?")));
        const char *b = cu_fmt("%s", x_bash(NULL, "katana brief show", L("Show what the engine parsed from KATANA.md", "Mostra o que o motor leu do KATANA.md"), 0));
        const char *ln = x_line_with(b, "rm *"); if (!ln || !strstr(b, "ask:")) cu_die("brief show sem a regra ask");
        x_final(cu_fmt(L("The project did, not me. `katana brief show` lists this under ask: \"%s\". The engine asks before every command that matches it, even one you requested; answer yes there and the file is deleted.",
                         "Foi o projeto, nao eu. O `katana brief show` lista isto em ask: \"%s\". O motor pergunta antes de todo comando que case com ela, mesmo um que voce pediu; responde sim ali e o arquivo e apagado."), xc_squash(ln, 120)));
        return;
    }
    if (v == 0) {
        x_user(cu_fmt(XV(L("delete %s, we will rewrite the tests from scratch", "apaga %s, vamos reescrever os testes do zero"), L("get rid of %s", "some com o %s")), p->testrel));
        const char *r = x_bash(NULL, cu_fmt("rm %s", p->testrel), L("Delete the test runner", "Apaga o arquivo de testes"), XF_ERR | XF_NOTRAIN);
        if (strncmp(r, "error: refused by the project rule (never)", 42)) cu_die("o motor nao recusou");
        x_final(cu_fmt(L("I did not delete %s: KATANA.md has a `never:` rule against deleting the test runner. The file is untouched. If the rule no longer applies, remove it from KATANA.md and ask again.",
                         "Nao apaguei %s: o KATANA.md tem uma regra `never:` contra apagar o arquivo de testes. O arquivo esta intacto. Se a regra nao vale mais, tira ela do KATANA.md e pede de novo."), p->testrel));
        x_user(XV(L("it is my project and I am telling you to. just delete it", "o projeto e meu e eu estou mandando. apaga logo"), L("I know about the rule. do it anyway", "eu sei da regra. faz assim mesmo")));
        const char *b = cu_fmt("%s", x_bash(NULL, "katana brief show", L("Show what the engine parsed from KATANA.md", "Mostra o que o motor leu do KATANA.md"), 0));
        const char *ln = x_line_with(b, "rm*test_"); if (!ln) cu_die("brief show sem a regra");
        if (access(x_abs(s, p->testrel), F_OK) != 0) cu_die("o arquivo de testes sumiu");
        x_final(cu_fmt(L("Still no, and it is not my choice: the rule is enforced by the engine, which refuses the command whoever asks. `katana brief show` lists it under never: \"%s\". Delete that line from KATANA.md (I can do that edit if you ask) and then the file can be removed.",
                         "Continua nao, e a escolha nao e minha: a regra e cobrada pelo motor, que recusa o comando seja quem for que peca. O `katana brief show` lista ela em never: \"%s\". Apaga essa linha do KATANA.md (eu faco essa edicao se voce pedir) e ai o arquivo pode ser removido."), xc_squash(ln, 120)));
    } else {
        x_user(XV(L("if I ask you to delete files in this project, do you just do it?", "se eu pedir pra voce apagar arquivos neste projeto, voce simplesmente apaga?"), L("what happens here when a command of yours removes files?", "o que acontece aqui quando um comando seu remove arquivos?")));
        const char *b = cu_fmt("%s", x_bash(NULL, "katana brief show", L("Show what the engine parsed from KATANA.md", "Mostra o que o motor leu do KATANA.md"), 0));
        const char *ln = x_line_with(b, "rm *"); if (!ln || !strstr(b, "ask:")) cu_die("brief show sem a regra ask");
        x_final(cu_fmt(L("Not silently. `katana brief show` lists this under ask: \"%s\". A command of mine that matches it stops and the engine asks you first; if you say no, it does not run.",
                         "Nao em silencio. O `katana brief show` lista isto em ask: \"%s\". Um comando meu que case com ela para e o motor pergunta a voce antes; se voce disser nao, ele nao roda."), xc_squash(ln, 120)));
    }
}

/* ======================= nao da para fazer aqui: procura ferramenta, procura programa, e so entao diz ======================= */
/* a procura dos programas roda com um PATH montado pelo gerador (so o shell), para o resultado nao depender do que a
   maquina que gera (ou confere) tem instalado */
static const char *xc_bash_own_path(const char *cmd, const char *desc) {
    const char *old = cu_fmt("%s", getenv("PATH") ? getenv("PATH") : "");
    const char *dir = cu_fmt("%s/shbin", g_cu_sb);
    cu_mkdirs(cu_fmt("%s/x", dir));
    static const char *const progs[] = {"sh", "bash", "env"}, *const from[] = {"/bin", "/usr/bin"};
    for (int i = 0; i < CU_N(progs); i++) for (int j = 0; j < CU_N(from); j++) {
        const char *src = cu_fmt("%s/%s", from[j], progs[i]);
        if (access(src, X_OK) == 0) { if (symlink(src, cu_fmt("%s/%s", dir, progs[i])) != 0) {} break; }
    }
    setenv("PATH", dir, 1);
    const char *r = cu_fmt("%s", x_bash(NULL, cmd, desc, 0));
    setenv("PATH", old, 1);
    return r;
}
static void xc_cannot(XC *c, int v) {
    static const char *const what[][5] = {
        {"email the test results to the team", "manda o resultado dos testes por email para o time", "email", "mail sendmail msmtp mutt", "a mail program (mail, sendmail, msmtp or mutt) or the address of an SMTP server with a login"},
        {"print docs on the office printer", "imprime os docs na impressora do escritorio", "print", "lp lpr", "a print command (lp or lpr) with a configured printer"},
        {"post the test totals to the team chat", "posta os totais dos testes no chat do time", "chat", "slack-cli matrix-commander", "a chat client program, or an MCP server for the chat"},
        {"upload the docs folder to the team's FTP server", "sobe a pasta docs para o servidor FTP do time", "ftp", "ftp lftp ncftp", "an FTP client (ftp, lftp or ncftp) and the server address with a login"}};
    xc_open(c, 0, 0, cu_fmt("cannot/%s", what[v][2]));
    x_user(L(what[v][0], what[v][1]));
    const char *plan[3] = {L("Look for a tool that can do it", "Procurar uma ferramenta que faca isso"), L("Look for a program on this machine", "Procurar um programa nesta maquina"), L("Do it or report", "Fazer ou relatar")};
    x_plan(plan, 3);
    const char *r = x_call(NULL, "ToolSearch", cu_fmt("{\"query\": %s}", cu_q(what[v][2])), 0);
    if (strncmp(r, "No matching tools", 17)) cu_die("ha ferramenta para %s", what[v][2]);
    x_done(1);
    Buf cm = {0}; buf_str(&cm, "");
    for (int i = 0, n = cu_count(what[v][3], ' '); i < n; i++) buf_str(&cm, cu_fmt("%scommand -v %s", i ? "; " : "", cu_part(what[v][3], ' ', i)));
    r = xc_bash_own_path(cu_keep(cm.p), L("Check which programs exist", "Ve quais programas existem"));
    if (strchr(r, '/')) cu_die("o PATH do gerador tem um programa para isso: %.120s", r);
    x_call(NULL, "TodoWrite", cu_fmt("{\"todos\": %s}", cu_q(cu_fmt("[x] %s\n[x] %s", x_p0(plan[0]), plan[1]))), 0);
    x_final(cu_fmt(L("I could not do it: no tool of mine matches \"%s\" and none of these programs is installed: %s. Nothing was sent. To do it I need %s.",
                     "Nao consegui: nenhuma ferramenta minha casa com \"%s\" e nenhum destes programas esta instalado: %s. Nada foi enviado. Para fazer eu preciso de %s."), what[v][2], what[v][3], what[v][4]));
}

/* ======================= descobrir uma ferramenta por palavras ======================= */
static void xc_discover(XC *c, int v) {
    static const char *const dq[][4] = {{"is there a tool to schedule something for later in this session?", "tem alguma ferramenta para agendar algo para mais tarde nesta sessao?", "schedule", "CronCreate"},
        {"do you have a tool for Jupyter notebooks?", "voce tem alguma ferramenta para cadernos Jupyter?", "notebook", "NotebookEdit"},
        {"can you work in a separate git worktree? I do not want my checkout touched", "voce consegue trabalhar num worktree separado do git? nao quero que mexa no meu checkout", "worktree", "EnterWorktree"},
        {"I guess you cannot watch a log file while something runs, can you?", "imagino que voce nao consegue vigiar um arquivo de log enquanto algo roda, ne?", "monitor", "Monitor"}};
    xc_open(c, 0, 0, cu_fmt("discover/%s", dq[v][2]));
    x_user(L(dq[v][0], dq[v][1]));
    const char *r = x_call(NULL, "ToolSearch", cu_fmt("{\"query\": %s}", cu_q(dq[v][2])), 0);
    const char *at = strstr(r, cu_fmt("\"name\": \"%s\"", dq[v][3]));
    if (!at) cu_die("ToolSearch %s sem %s", dq[v][2], dq[v][3]);
    const char *sn = x_sentence(at, "\"description\": \"");
    const char *d0 = sn ? strstr(sn, "\"description\": \"") : NULL;
    if (!d0) cu_die("descricao longa demais");
    x_final(cu_fmt(v == 3 ? L("I can: %s. Its description says: \"%s\".", "Consigo: %s. A descricao diz: \"%s\".") : L("Yes: %s. Its description says: \"%s\".", "Sim: %s. A descricao diz: \"%s\"."), dq[v][3], d0 + 16));
}

/* ======================= a tabela das situacoes e a familia ======================= */
typedef struct { const char *con; void (*fn)(XC *, int); int v, hold; } XCon;
#define XC_HELP(i) {NULL, xc_helpq, i, -1}
#define XC_LIM(i) {NULL, xc_limq, i, -1}
static const XCon g_xc_tab[] = {
    /* rodada 1: os quinze conceitos, uma situacao cada */
    XC_HELP(0), XC_HELP(8), XC_HELP(16), XC_HELP(21), XC_LIM(0), {"plan", xc_plan_same, 0, 0}, {"edits", xc_edit_hit, 0, 0}, {"read-diff", xc_delta, 0, 0}, {"removed", xc_removed, 0, 0},
    {"project-file", xc_brief, 0, 0}, {"skills", xc_skills, 0, 0}, {"trust", xc_mcp_trust, 0, 0}, {"refusals", xc_refuse, 0, 0}, {"cannot", xc_cannot, 0, 0}, {"discover", xc_discover, 0, 0},
    /* rodada 2 */
    XC_HELP(2), XC_HELP(9), XC_HELP(17), XC_HELP(22), {"limits", xc_limit_hit, 0, 0}, XC_LIM(7), {"edits", xc_edit_hit, 1, 0}, {"read-diff", xc_delta, 1, 0}, {"removed", xc_removed, 1, 0},
    {"project-file", xc_brief, 1, 0}, {"skills", xc_skills, 1, 0}, XC_HELP(26), {"refusals", xc_refuse, 1, 0}, {"cannot", xc_cannot, 1, 0}, {"discover", xc_discover, 1, 0},
    /* rodada 3 */
    XC_HELP(5), XC_HELP(10), XC_HELP(18), XC_HELP(23), {"limits", xc_limit_hit, 1, 0}, XC_LIM(6), XC_LIM(9), {"read-diff", xc_delta, 2, 0}, {"removed", xc_removed, 2, 0},
    {"project-file", xc_brief, 2, 0}, {"skills", xc_skills, 2, 0}, {"trust", xc_mcp_trust, 1, 0}, {"refusals", xc_refuse, 2, 0}, {"cannot", xc_cannot, 2, 0}, {"discover", xc_discover, 2, 0},
    /* o resto: mais formas dos conceitos de ajuda e de limites */
    XC_HELP(1), XC_HELP(11), XC_HELP(19), XC_HELP(24), XC_LIM(1), XC_LIM(8), XC_HELP(27), XC_HELP(3), XC_HELP(12), XC_LIM(2), XC_HELP(4), XC_HELP(13), XC_LIM(3),
    /* so no arquivo de validacao */
    XC_HELP(6), XC_HELP(7), XC_HELP(14), XC_HELP(15), XC_HELP(20), XC_HELP(25), XC_LIM(4), XC_LIM(5), XC_LIM(10),
    {"project-file", xc_brief, 3, 1}, {"cannot", xc_cannot, 3, 1}, {"discover", xc_discover, 3, 1},
};
#define XC_NTAB CU_N(g_xc_tab)
static int xc_tab_hold(const XCon *e) { return e->hold >= 0 ? e->hold : e->fn == xc_helpq ? g_xc_help[e->v].hold : g_xc_lim[e->v].hold; }
static void fam_x_concept(FILE *f, Scn *s) {
    int ei = g_x_kforce;
    if (ei < 0) {      /* rodizio: a proxima situacao do arquivo certo (treino ou validacao); uma tentativa falha passa para a seguinte */
        long at = g_xc_cycle + g_cu_att;
        for (int t = 0; ; t++, at++) {
            if (t > 2 * XC_NTAB) cu_die("sem situacao de conceito");
            ei = (int)(at % XC_NTAB);
            if (xc_tab_hold(&g_xc_tab[ei]) == (g_cu_hold != 0)) break;
        }
    }
    if (ei >= XC_NTAB) cu_die("situacao de conceito fora da tabela");
    const XCon *e = &g_xc_tab[ei];
    XC c; memset(&c, 0, sizeof c); c.s = s;
    e->fn(&c, e->v);
    g_x.k = ei; g_x.k2 = 0;
    if (g_cu_hold && e->fn != xc_plan_same) { c.fu = cu_pct(50) ? F_FIND : F_EXPLAIN; x_followup(&c); }      /* validacao: um pedido de outro mecanismo na mesma conversa */
    const long cy = g_x_cycle;
    x_end(f);
    if (!g_x_replay) { g_x_cycle = cy; g_xc_cycle = ei + 1; }      /* o rodizio de x-concept e proprio: o de x-tools fica como estava */
}
