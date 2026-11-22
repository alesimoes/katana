/* ---------- katana curriculum, cenarios compostos: o proprio katana, ferramentas listadas so pelo nome, web e skills ----------
 * x-concept: perguntas sobre o katana respondidas com o que o binario diz (katana --help, katana help, brief show, skill
 *            list, descricao de uma ferramenta pelo ToolSearch) e o que fazer quando nao ha como.
 * x-tools:   ferramentas que o prompt so cita pelo nome: primeiro ToolSearch select:Nome, depois a chamada, tudo rodado.
 * x-web:     WebSearch e WebFetch do motor contra paginas locais do cenario (resultados de busca fixos, servidor local).
 * x-skill:   uma skill de verdade do projeto, com passos que rodam. */

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <signal.h>

/* ---- servidor local de paginas: um processo python3 -m http.server sobre <pasta do gerador>/www, encerrado pelo pid ---- */
static void x_web_down(void) { if (g_x_srv > 0) { kill(g_x_srv, SIGTERM); waitpid(g_x_srv, NULL, 0); g_x_srv = 0; } }
static int x_web_up(void) {
    if (g_x_port) return 1;
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return 0;
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    socklen_t al = sizeof a;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || getsockname(s, (struct sockaddr *)&a, &al) != 0) { close(s); return 0; }
    const int port = ntohs(a.sin_port); close(s);
    char dir[700], ps[16]; snprintf(dir, sizeof dir, "%s/www", g_cu_sbroot); cu_mkdirs(dir); snprintf(ps, sizeof ps, "%d", port);
    const pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        const int dn = open("/dev/null", O_RDWR); dup2(dn, 0); dup2(dn, 1); dup2(dn, 2);
        setenv("PATH", g_cu_path0, 1);
        execlp("python3", "python3", "-I", "-m", "http.server", ps, "--bind", "127.0.0.1", "--directory", dir, (char *)NULL);
        _exit(127);
    }
    g_x_srv = pid; atexit(x_web_down);
    for (int t = 0; t < 100; t++) {      /* espera o servidor aceitar conexao */
        int c = socket(AF_INET, SOCK_STREAM, 0);
        a.sin_port = htons((uint16_t)port);
        const int ok = connect(c, (struct sockaddr *)&a, sizeof a) == 0; close(c);
        if (ok) { g_x_port = port; setenv("KATANA_WEB_ALLOW_LOCAL", "1", 1); return 1; }
        usleep(100000);
    }
    x_web_down();
    return 0;
}
/* tira do resultado os tempos de rede, que mudam a cada execucao */
static const char *x_web_fix(const char *r) {
    char *t = cu_fmt("%s", r), *w = strstr(t, "[web: ");
    if (!w) return t;
    Buf b = {0}; buf_add(&b, t, (size_t)(w - t));
    for (const char *p = w; *p; ) {
        if (isdigit((unsigned char)*p) && p[1] == '.' && isdigit((unsigned char)p[2]) && !strncmp(p + 3, " s", 2)) { buf_str(&b, "0.2"); p += 3; continue; }
        buf_add(&b, p, 1); p++;
    }
    return cu_keep(b.p);
}
static const char *x_webcall(const char *name, const char *args) {
    const char *r = x_call(NULL, name, args, XF_ERR);
    g_x.m[g_x.n - 1].text = x_web_fix(r);
    return g_x.m[g_x.n - 1].text;
}

/* ======================= web ======================= */
static const char *const g_x_libs[] = {"kestrel", "lumen", "tarn", "quill", "ember", "harbor", "sorrel", "pylon", "marlin", "cobalt", "fennel", "osier"};
static const char *const g_x_opts[][2] = {{"--fast", "--quick"}, {"--verbose", "--trace"}, {"--out", "--output-dir"}, {"--workers", "--jobs"}, {"--no-cache", "--fresh"}, {"--strict", "--checked"}, {"--limit", "--max-rows"}, {"--dry", "--plan-only"}};
static void fam_x_web(FILE *f, Scn *s) {
    if (!x_web_up()) cu_die("o servidor local de paginas nao subiu");
    const char *lib = CU_PK(g_x_libs); const int oi = cu_ix(CU_N(g_x_opts)), v1 = 1 + cu_n(4), v2 = 1 + cu_n(9), keep = v1 + 1;
    const int kind = cu_n(4), sdef = 10 * (2 + cu_n(8)), sdef2 = sdef * 2;      /* 0 busca; 1 busca + pagina; 2 URL dada; 3 URL errada, depois busca */
    for (int t = 0; x_held(40 + kind, oi % 3, 0, 0, 0) != (g_cu_hold != 0); t++) { if (t > 50) cu_die("sem celula de web"); g_cu_rng += 0x9E37; (void)cu_n(7); return fam_x_web(f, s); }
    char site[80]; snprintf(site, sizeof site, "%s-%04x", lib, (unsigned)(g_x_rng0 & 0xffff));
    const char *base = cu_fmt("http://127.0.0.1:%d/%s", g_x_port, site), *www = cu_fmt("%s/www/%s", g_cu_sbroot, site);
    const char *ver = cu_fmt("%d.%d", v1, v2);
    const char *fact1 = cu_fmt("%s %s renames the option %s to %s.", lib, ver, g_x_opts[oi][0], g_x_opts[oi][1]);
    const char *fact2 = cu_fmt("The old name %s keeps working until %s %d.0 and prints a warning.", g_x_opts[oi][0], lib, keep);
    const char *fact3 = cu_fmt("The default batch size of %s %s changed from %d to %d rows.", lib, ver, sdef, sdef2);
    const int conn = 4 * (1 + cu_n(8));
    const char *fact4 = cu_fmt("At most %d connections are opened at once.", conn);
    Buf fill = {0}; buf_str(&fill, "");
    for (int i = 0; i < 14; i++) buf_str(&fill, cu_fmt("<p>Section %d lists contributors, build notes and packaging details of the project.</p>\n", i + 1));
    cu_put(cu_fmt("%s/release-%s.html", www, ver), cu_fmt("<html><head><title>%s %s release notes</title></head><body><main>\n<p>%s</p>\n<p>%s</p>\n%s<p>%s</p>\n<p>%s</p>\n</main></body></html>\n", lib, ver, fact1, fact4, fill.p, fact2, fact3), 0);
    cu_put(cu_fmt("%s/install.html", www), cu_fmt("<html><head><title>Installing %s</title></head><body><main>\n<p>Download the archive of %s and unpack it into a folder on your PATH.</p>\n%s</main></body></html>\n", lib, lib, fill.p), 0);
    free(fill.p);
    setenv("KATANA_WEBSEARCH_FIXTURE", cu_fmt("1. %s %s release notes\n   %s/release-%s.html\n   What changed in %s %s\n   (sources: bing)\n2. Installing %s\n   %s/install.html\n   How to install %s\n   (sources: bing)", lib, ver, base, ver, lib, ver, lib, base, lib), 1);
    x_begin("x-web", s, NULL, 0, 0);
    x_cell("web/kind%d/opt%d", kind, oi % 3);
    const char *url = cu_fmt("%s/release-%s.html", base, ver), *shown = cu_fmt("https://docs.example/%s/release-%s.html", site, ver);
    const int q = kind == 1 ? 3 : kind == 0 ? cu_n(3) : 0;      /* o que se pergunta: 0 a opcao renomeada; 1 ate quando vale o nome antigo; 2 o tamanho do lote; 3 o limite de conexoes (a frase da pagina nao tem as palavras da pergunta) */
    const char *ask = q == 0 ? cu_fmt(XV(L("which option did %s %s rename?", "que opcao o %s %s renomeou?"), L("%s %s renamed a command line option. which one, and to what?", "o %s %s renomeou uma opcao de linha de comando. qual, e para que?")), lib, ver)
                    : q == 1 ? cu_fmt(XV(L("until which %s version does the old %s option keep working?", "ate que versao do %s a opcao antiga %s continua funcionando?")), lib, g_x_opts[oi][0])
                    : q == 2 ? cu_fmt(XV(L("what is the default batch size in %s %s?", "qual e o tamanho padrao do lote no %s %s?")), lib, ver)
                             : cu_fmt(XV(L("what is the concurrency limit of %s %s?", "qual e o limite de concorrencia do %s %s?")), lib, ver);
    const char *ans0 = cu_fmt(L("%s %s renames `%s` to `%s`. Source: %s", "O %s %s renomeia `%s` para `%s`. Fonte: %s"), lib, ver, g_x_opts[oi][0], g_x_opts[oi][1], shown);
    if (kind == 2 || kind == 3) {
        const char *given = kind == 3 ? cu_fmt("%s/releases-%s.html", base, ver) : url;
        x_user(cu_fmt(L("%s %s", "%s %s"), ask, cu_fmt(L("the notes are at %s", "as notas estao em %s"), given)));
        const char *r = x_webcall("WebFetch", cu_fmt("{\"url\": %s, \"prompt\": %s}", cu_q(given), cu_q(cu_fmt("option renamed in %s %s", lib, ver))));
        if (kind == 3) {      /* a pagina nao existe: a busca acha a certa */
            if (!strstr(r, "404")) cu_die("a pagina errada existe");
            r = x_webcall("WebSearch", cu_fmt("{\"query\": %s}", cu_q(cu_fmt("%s %s release notes option renamed", lib, ver))));
        }
        if (!strstr(r, fact1)) cu_die("a pagina nao trouxe o fato: %.200s", r);
        x_final(kind == 3 ? cu_fmt(L("%s (the address you gave returns 404; the search found the right page).", "%s (o endereco que voce passou devolve 404; a busca achou a pagina certa)."), ans0) : ans0);
    } else {
        x_user(ask);
        const char *r = cu_fmt("%s", x_webcall("WebSearch", cu_fmt("{\"query\": %s}", cu_q(q == 0 ? cu_fmt("%s %s option renamed", lib, ver) : q == 1 ? cu_fmt("%s old option %s removed version", lib, g_x_opts[oi][0]) : q == 2 ? cu_fmt("%s %s default batch size", lib, ver) : cu_fmt("%s %s concurrency limit", lib, ver)))));
        if (!strstr(r, cu_fmt("/release-%s.html", ver))) cu_die("a busca nao listou a pagina");
        const char *need = q == 0 ? fact1 : q == 1 ? fact2 : q == 2 ? fact3 : fact4;
        if (!strstr(r, need)) {      /* o trecho da busca nao traz a resposta: abre a pagina */
            r = x_webcall("WebFetch", cu_fmt("{\"url\": %s, \"prompt\": %s}", cu_q(url), cu_q(q == 1 ? cu_fmt("until which version the old option %s works", g_x_opts[oi][0]) : q == 2 ? "default batch size" : q == 3 ? "concurrency limit" : "option renamed")));
            if (!strstr(r, need)) cu_die("o WebFetch nao trouxe o fato: %.200s", r);
        }
        if (q == 3) x_final(cu_fmt(L("%s %s opens at most %d connections at once. The search passages did not say it, so I opened the page. Source: %s", "O %s %s abre no maximo %d conexoes ao mesmo tempo. Os trechos da busca nao diziam, entao abri a pagina. Fonte: %s"), lib, ver, conn, shown));
        else x_final(q == 0 ? ans0 : q == 1 ? cu_fmt(L("`%s` keeps working until %s %d.0, with a warning. Source: %s", "O `%s` continua funcionando ate o %s %d.0, com um aviso. Fonte: %s"), g_x_opts[oi][0], lib, keep, shown)
                                       : cu_fmt(L("The default batch size in %s %s is %d rows (it was %d). Source: %s", "O tamanho padrao do lote no %s %s e %d linhas (era %d). Fonte: %s"), lib, ver, sdef2, sdef, shown));
    }
    if (g_cu_hold) {      /* holdout: uma segunda pergunta sobre a mesma pagina */
        x_user(cu_fmt(L("and what is the default batch size in %s %s?", "e qual e o tamanho padrao do lote no %s %s?"), lib, ver));
        const char *r2 = x_webcall("WebFetch", cu_fmt("{\"url\": %s, \"prompt\": \"default batch size\"}", cu_q(url)));
        if (!strstr(r2, fact3)) cu_die("o WebFetch nao trouxe o lote: %.200s", r2);
        x_final(cu_fmt(L("%d rows; it was %d before %s. Source: %s", "%d linhas; era %d antes do %s. Fonte: %s"), sdef2, sdef, ver, shown));
    }
    unsetenv("KATANA_WEBSEARCH_FIXTURE");
    x_end(f);
}

/* ======================= o proprio katana ======================= */
/* a frase (ou a linha) da saida real que contem a agulha */
static const char *x_sentence(const char *out, const char *needle) {
    const char *q = strstr(out, needle); if (!q) return NULL;
    const char *a = q, *e = q;
    while (a > out && a[-1] != '\n' && !(a[-1] == ' ' && a - 2 >= out && a[-2] == '.')) a--;
    while (*e && *e != '\n' && !(*e == '.' && (e[1] == ' ' || e[1] == '"' || e[1] == 0))) e++;
    char *t = cu_fmt("%.*s", (int)(e - a), a);
    for (char *c = t; *c; c++) if (*c == '\\' && c[1] == '"') memmove(c, c + 1, strlen(c));
    while (*t == ' ') t++;
    size_t l = strlen(t); while (l && t[l - 1] == ' ') t[--l] = 0;
    return l > 300 ? NULL : t;
}
#include "app/scenario_concept.c"    /* x-concept: os conceitos do katana, tres situacoes cada, em rodizio */

/* ======================= ferramentas listadas so pelo nome ======================= */
enum { K_BG, K_MON, K_NB, K_SEND, K_PLANMODE, K_WT, K_GIT, K_MEM, K_DL, K_LSP, K_MCPRES, K_AGENT, K_CRON, K_TERM, K_N };
static const char *const g_x_kname[K_N] = {"background", "monitor", "notebook", "send-file", "plan-mode", "worktree", "git", "memory", "download", "lsp", "mcp-resources", "agent", "cron", "terminal"};
static char g_x_subans[2000];
static void x_sub_oracle(const char *prompt, char *out, size_t cap) { (void)prompt; snprintf(out, cap, "%s", g_x_subans); }
static const char *x_tsel(const char *names) { return x_call(NULL, "ToolSearch", cu_fmt("{\"query\": \"select:%s\"}", names), 0); }
static void fam_x_tools(FILE *f, Scn *s) {
    /* as ferramentas saem em rodizio (sem gastar sorteio; a escolha vai na linha gravada). Ficam de fora: Agent (so dentro do laco do agente, com modelo) e terminal (a saida do pty depende do tempo) */
    static const int order[] = {K_BG, K_MON, K_NB, K_SEND, K_PLANMODE, K_WT, K_GIT, K_MEM, K_DL, K_LSP, K_MCPRES, K_CRON};
    const int kind = g_x_kforce >= 0 ? g_x_kforce : order[(g_x_cycle + g_cu_att) % CU_N(order)];      /* terminal fica de fora: a saida do pty depende do tempo */
    XC c; memset(&c, 0, sizeof c); c.s = s;
    const int st = cu_n(X_NST);
    x_proj(&c.p, s, st == X_C ? X_SH : st, 2, 2); XProj *p = &c.p;
    p->brief = cu_pct(40) && kind != K_NB && kind != K_PLANMODE && kind != K_WT; p->readme = !p->brief && cu_pct(50); p->knows = p->brief;
    if (p->brief || kind == K_WT) p->knows = p->brief;
    const char *nb = cu_fmt("notebooks/%s.ipynb", XV("analysis", "report", "explore")); const int rate = 3 + cu_n(6), nrate = rate + 1 + cu_n(5);
    if (kind == K_BG || kind == K_MON) x_extra(p, "tools/watch.sh", cu_fmt("#!/bin/sh\n# prints a line when it starts watching, then waits\necho \"watching %s\"\nexec sleep 300\n", p->name), 0755);
    if (kind == K_NB) x_extra(p, nb, cu_fmt("{\"cells\": [{\"cell_type\": \"code\", \"id\": \"setup\", \"metadata\": {}, \"outputs\": [], \"execution_count\": null, \"source\": [\"rate = %d\\n\"]}, "
                                            "{\"cell_type\": \"code\", \"id\": \"calc\", \"metadata\": {}, \"outputs\": [], \"execution_count\": null, \"source\": [\"total = rate * 12\\n\", \"total\\n\"]}], \"metadata\": {}, \"nbformat\": 4, \"nbformat_minor\": 5}\n", rate), 0);
    if (kind == K_PLANMODE) { p->conf = 1; for (int i = 0; i < CU_N(x_conf_keys); i++) x_conf_add(p, x_conf_keys[i][0], x_conf_keys[i][1], x_conf_keys[i][2]); }
    g_x_realgit = kind == K_WT || kind == K_GIT;
    if (kind == K_DL || kind == K_WT) p->brief = 0;
    char site[80]; snprintf(site, sizeof site, "files-%04x", (unsigned)(g_x_rng0 & 0xffff));
    const char *dlname = cu_fmt("%s-notes.txt", XV("release", "api", "upgrade"));
    if (kind == K_DL) { if (!x_web_up()) cu_die("o servidor local de paginas nao subiu"); cu_put(cu_fmt("%s/www/%s/%s", g_cu_sbroot, site, dlname), cu_fmt("notes for %s\nline two\nline three\n", p->name), 0); }
    x_begin("x-tools", s, p, 0, 0);
    g_x_realgit = 0; g_x.k = kind;
    x_cell("tools/%s/%s/conv%d", g_x_kname[kind], g_x_stid[p->st], p->brief ? 2 : p->readme);
    if (kind == K_BG || kind == K_MON) {
        const int mon = kind == K_MON;
        x_user(mon ? XV(L("watch the output of tools/watch.sh for me, tell me its first line and then stop watching", "monitora a saida do tools/watch.sh pra mim, me diz a primeira linha e depois para de monitorar"))
                   : XV(L("start tools/watch.sh in the background, tell me what it prints and then stop it", "sobe o tools/watch.sh em segundo plano, me diz o que ele imprime e depois para ele"),
                        L("run tools/watch.sh in the background and show me its first line; stop it after that", "roda o tools/watch.sh em segundo plano e me mostra a primeira linha; depois para ele")));
        const char *plan[3] = {mon ? L("Start watching the script", "Comecar a monitorar o script") : L("Start the script in the background", "Subir o script em segundo plano"), L("Read its output", "Ler a saida"), L("Stop it", "Parar")};
        x_plan(plan, 3);
        if (mon) x_tsel("Monitor,TaskStop");
        const char *r = cu_fmt("%s", mon ? x_call(NULL, "Monitor", cu_fmt("{\"command\": \"sh tools/watch.sh\", \"description\": %s}", cu_q(cu_fmt(L("Watch the script of %s", "Monitora o script do %s"), p->name))), 0)
                                         : x_call(NULL, "Bash", cu_fmt("{\"command\": \"sh tools/watch.sh\", \"run_in_background\": true, \"description\": %s}", cu_q(cu_fmt(L("Start the watcher of %s in the background", "Sobe o monitor do %s em segundo plano"), p->name))), 0));
        const char *idp = strstr(r, "ID: "), *fp = strstr(r, "written to: ");
        if (!idp || !fp) cu_die("resultado sem id ou arquivo: %.200s", r);
        const char *id = cu_fmt("%d", atoi(idp + 4)), *log = cu_fmt("%.*s", (int)strcspn(fp + 12, "\n"), fp + 12);
        usleep(500000);
        x_done(1);
        const char *o = x_call(NULL, "Read", cu_fmt("{\"file_path\": %s}", cu_q(log)), 0);
        if (!strstr(o, cu_fmt("watching %s", p->name))) { ct_dispatch("TaskStop", cu_fmt("{\"task_id\": \"%s\"}", id), "", g_x_res, 60000); cu_die("a saida ainda nao tinha a linha"); }
        x_done(2);
        if (!mon) x_tsel("TaskStop");
        r = x_call(NULL, "TaskStop", cu_fmt("{\"task_id\": \"%s\"}", id), 0);
        if (!strstr(r, "Successfully stopped")) cu_die("TaskStop nao parou: %.200s", r);
        x_final(cu_fmt(L("The script printed `watching %s`. I stopped it (task %s); its output file is still there if you want to read it.", "O script imprimiu `watching %s`. Parei ele (tarefa %s); o arquivo de saida continua la se voce quiser ler."), p->name, id));
    } else if (kind == K_NB) {
        x_user(cu_fmt(XV(L("in %s, change the rate to %d", "em %s, muda o rate para %d"), L("the notebook %s should use rate = %d", "o caderno %s tem que usar rate = %d")), nb, nrate));
        const char *plan[3] = {L("Read the notebook", "Ler o caderno"), L("Get the notebook tool", "Pegar a ferramenta de cadernos"), L("Change the cell", "Mudar a celula")};
        x_plan(plan, 3);
        const char *r = x_read(s, nb, 0);
        if (!strstr(r, "setup") || !strstr(r, cu_fmt("rate = %d", rate))) cu_die("o Read do caderno nao mostrou a celula: %.300s", r);
        x_done(1); x_tsel("NotebookEdit"); x_done(2);
        r = x_call(NULL, "NotebookEdit", cu_fmt("{\"notebook_path\": %s, \"cell_id\": \"setup\", \"new_source\": \"rate = %d\"}", cu_q(x_abs(s, nb)), nrate), 0);
        if (!strstr(r, "Replaced the source of cell setup")) cu_die("NotebookEdit: %.200s", r);
        x_final(cu_fmt(L("The cell `setup` of %s now has `rate = %d` (it was %d). Only that cell changed; the other cell was not run again.", "A celula `setup` de %s agora tem `rate = %d` (era %d). So essa celula mudou; a outra celula nao foi rodada de novo."), nb, nrate, rate));
    } else if (kind == K_SEND) {
        const char *rel = cu_pct(50) ? p->testrel : x_src(p, 0);
        x_user(cu_fmt(XV(L("send me the file %s", "me manda o arquivo %s"), L("hand me %s so I can open it", "me entrega o %s pra eu abrir")), rel));
        x_tsel("SendUserFile");
        const char *r = x_call(NULL, "SendUserFile", cu_fmt("{\"files\": [%s]}", cu_q(x_abs(s, rel))), 0);
        const char *ln = x_line_with(r, rel);
        if (!ln) cu_die("SendUserFile sem o arquivo: %.200s", r);
        x_final(cu_fmt(L("Sent: %s.", "Enviado: %s."), ln));
    } else if (kind == K_PLANMODE) {
        const int ki = cu_n(p->nck); const long lo = atol(cu_part(p->cr[ki], '-', 0)), hi = atol(cu_part(p->cr[ki], '-', 1)); long v = lo + 1 + cu_n((int)(hi - lo > 300 ? 300 : hi - lo - 1)); if (v == atol(p->cv[ki])) v++;
        x_user(cu_fmt(XV(L("I want to approve the plan before anything changes: set %s to %ld", "quero aprovar o plano antes de qualquer mudanca: coloca %s em %ld"), L("plan first and wait for my ok: %s should become %ld", "planeja primeiro e espera o meu ok: o %s tem que virar %ld")), p->ck[ki], v));
        const char *plan[3] = {L("Enter plan mode and read the settings", "Entrar no modo plano e ler as configuracoes"), L("Present the plan for approval", "Apresentar o plano para aprovacao"), L("Apply it and run the check", "Aplicar e rodar a verificacao")};
        x_plan(plan, 3);
        x_tsel("EnterPlanMode,ExitPlanMode");
        x_call(NULL, "EnterPlanMode", "{}", 0);
        x_need_read(p, "settings.conf");
        x_done(1);
        x_feed("approve");
        const char *r = x_call(NULL, "ExitPlanMode", cu_fmt("{\"plan\": %s}", cu_q(cu_fmt("1. In settings.conf change `%s = %s` to `%s = %ld`\n2. Run `sh tools/check_settings.sh`", p->ck[ki], p->cv[ki], p->ck[ki], v))), XF_ERR);
        x_unfeed();
        if (x_err(r) || g_plan_mode) cu_die("o plano nao foi aprovado: %.200s", r);
        x_done(2);
        x_edit(s, "settings.conf", cu_fmt("%s = %s", p->ck[ki], p->cv[ki]), cu_fmt("%s = %ld", p->ck[ki], v), 0, 0);
        r = x_bash(NULL, "sh tools/check_settings.sh", L("Check the settings", "Confere as configuracoes"), 0);
        if (!strstr(r, "settings ok")) cu_die("verificacao falhou");
        x_final(cu_fmt(L("You approved the plan, so I applied it: settings.conf now has `%s = %ld`, and `sh tools/check_settings.sh` ends with \"settings ok\".", "Voce aprovou o plano, entao apliquei: o settings.conf agora tem `%s = %ld`, e o `sh tools/check_settings.sh` termina com \"settings ok\"."), p->ck[ki], v));
    } else if (kind == K_WT) {
        const char *w = XV("trial", "spike", "check");
        x_user(cu_fmt(XV(L("make an isolated worktree called %s, run the tests there, then remove it", "cria um worktree isolado chamado %s, roda os testes la e depois remove"), L("run the tests in a separate worktree named %s and clean it up afterwards", "roda os testes num worktree separado chamado %s e limpa depois")), w));
        const char *plan[3] = {L("Create the worktree", "Criar o worktree"), L("Run the tests in it", "Rodar os testes nele"), L("Remove the worktree", "Remover o worktree")};
        x_plan(plan, 3);
        x_tsel("EnterWorktree,ExitWorktree");
        const char *r = x_call(NULL, "EnterWorktree", cu_fmt("{\"name\": %s}", cu_q(w)), 0);
        if (!strstr(r, "created worktree")) cu_die("EnterWorktree: %.200s", r);
        x_done(1);
        x_find_testcmd(p);
        const char *o = x_test(p, NULL), *tot = x_totals(o);
        x_done(2);
        r = cu_fmt("%s", x_call(NULL, "ExitWorktree", "{\"action\": \"remove\"}", XF_ERR));
        int asked = 0;
        if (tot && x_err(r) && strstr(r, "uncommitted changes:")) {      /* rodar os testes deixou arquivos novos: descartar o que nao foi pedido pede a confirmacao do usuario */
            const char *un = strstr(r, "?? "); const char *what = un ? cu_fmt("%.*s", (int)strcspn(un + 3, "\n"), un + 3) : "files";
            const char *lab[2] = {L("Remove it", "Remover"), L("Keep it", "Manter")}, *ds[2] = {cu_fmt(L("discard %s and delete the worktree", "descartar %s e apagar o worktree"), what), L("leave the worktree on disk", "deixar o worktree no disco")};
            x_askproof("delete", 1, "uncommitted changes:", NULL);
            x_ask(cu_fmt(L("The worktree has files that are not committed (%s). Remove it anyway?", "O worktree tem arquivos sem commit (%s). Removo mesmo assim?"), what), lab, ds, 2, lab[0]);
            r = cu_fmt("%s", x_call(NULL, "ExitWorktree", "{\"action\": \"remove\", \"discard_changes\": true}", XF_ERR));
            asked = 1;
        }
        if (!tot || !strstr(r, "removed the worktree")) { ct_dispatch("ExitWorktree", "{\"action\": \"remove\", \"discard_changes\": true}", "", g_x_res, 60000); cu_die("worktree: %.200s", r); }
        if (asked) { x_final(cu_fmt(L("In the worktree `%s` the tests gave: %s. The run left uncommitted files there, so I asked before discarding them; the worktree and its branch are now removed and your checkout was not touched.",
                                      "No worktree `%s` os testes deram: %s. A rodada deixou arquivos sem commit la, entao perguntei antes de descartar; o worktree e a branch dele foram removidos e o seu checkout nao foi tocado."), w, tot)); x_end(f); return; }
        x_final(cu_fmt(L("In the worktree `%s` the tests gave: %s. The worktree and its branch are removed; your checkout was not touched.", "No worktree `%s` os testes deram: %s. O worktree e a branch dele foram removidos; o seu checkout nao foi tocado."), w, tot));
    } else if (kind == K_GIT) {
        XFi *fi = &p->mod[0].fn[0]; const char *rel = x_src(p, 0);
        const char *cur = x_slurp(s, rel), *oldl = x_snip(p->st, fi->name, fi->f, fi->num), *newl = x_snip(p->st, fi->name, fi->f, fi->f->bad);
        const char *q = strstr(cur, oldl); if (!q) cu_die("trecho nao achado");
        cu_put(x_abs(s, rel), cu_fmt("%.*s%s%s", (int)(q - cur), cur, newl, q + strlen(oldl)), 0);
        x_user(XV(L("what did I change since the last commit?", "o que eu mudei desde o ultimo commit?"), L("show me my uncommitted changes", "me mostra as minhas mudancas sem commit")));
        const char *r = cu_fmt("%s", x_call(NULL, "git", "{\"args\": \"status --short\"}", XF_OLD));
        if (!strstr(r, rel)) cu_die("git status sem o arquivo: %.200s", r);
        r = cu_fmt("%s", x_call(NULL, "git", "{\"args\": \"diff\"}", XF_OLD));
        const char *plus = strstr(r, "\n+ "), *minus = strstr(r, "\n- ");
        if (!plus || !minus) cu_die("git diff sem as linhas: %.300s", r);
        x_final(cu_fmt(L("One file changed: %s. In `%s` the line `%.*s` became `%.*s`. Nothing is staged.", "Um arquivo mudou: %s. Em `%s` a linha `%.*s` virou `%.*s`. Nada esta no stage."), rel, fi->name,
                       (int)strcspn(minus + 2, "\n") - (int)strspn(minus + 2, " "), minus + 2 + strspn(minus + 2, " "), (int)strcspn(plus + 2, "\n") - (int)strspn(plus + 2, " "), plus + 2 + strspn(plus + 2, " ")));
    } else if (kind == K_MEM) {
        static const char *const facts[][4] = {{"I prefer tabs over spaces in shell scripts", "prefiro tabs a espacos em scripts de shell", "tabs", "indentation"}, {"my release day is Thursday", "meu dia de release e quinta-feira", "Thursday", "release"},
            {"I review code in the morning, before ten", "eu reviso codigo de manha, antes das dez", "morning", "review"}};
        const int fi = cu_n(3);
        x_user(cu_fmt(L("remember this: %s", "guarda isso: %s"), L(facts[fi][0], facts[fi][1])));
        const char *r = x_call(NULL, "remember", cu_fmt("{\"fact\": %s}", cu_q(cu_fmt("The user said: %s", facts[fi][0]))), XF_OLD);
        if (x_err(r)) cu_die("remember: %.200s", r);
        x_final(L("Saved. I will have it in later conversations too.", "Guardado. Vou ter isso nas proximas conversas tambem."));
        x_user(cu_fmt(XV(L("what do you have saved about my %s?", "o que voce tem guardado sobre %s?")), g_cu_lang ? (fi == 0 ? "indentacao" : fi == 1 ? "o meu release" : "a minha revisao de codigo") : facts[fi][3]));
        r = x_call(NULL, "recall", cu_fmt("{\"query\": %s}", cu_q(facts[fi][2])), XF_OLD);
        if (!strstr(r, facts[fi][2])) cu_die("recall nao achou: %.200s", r);
        x_final(cu_fmt(L("I have this saved: \"%s\".", "Tenho isto guardado: \"%s\"."), cu_fmt("The user said: %s", facts[fi][0])));
    } else if (kind == K_DL) {
        const char *url = cu_fmt("http://127.0.0.1:%d/%s/%s", g_x_port, site, dlname), *dst = cu_fmt("docs/%s", dlname);
        x_user(cu_fmt(XV(L("download %s into docs/", "baixa %s para dentro de docs/"), L("save %s under docs/ of this project", "salva %s em docs/ deste projeto")), url));
        const char *r = x_call(NULL, "download_file", cu_fmt("{\"url\": %s, \"path\": %s}", cu_q(url), cu_q(dst)), XF_OLD);
        if (x_err(r) || access(x_abs(s, dst), F_OK) != 0) cu_die("download_file: %.200s", r);
        r = x_bash(NULL, cu_fmt("wc -l %s", dst), L("Count the lines of the downloaded file", "Conta as linhas do arquivo baixado"), 0);
        x_final(cu_fmt(L("Saved as %s (%d lines).", "Salvo como %s (%d linhas)."), dst, atoi(r)));
    } else if (kind == K_LSP) {
        XFi *fi = &p->mod[1].fn[0];
        x_user(cu_fmt(XV(L("use the language server to find where %s is defined", "usa o servidor de linguagem pra achar onde o %s e definido"), L("ask the language server for the definition of %s", "pede ao servidor de linguagem a definicao do %s")), fi->name));
        const char *plan[2] = {L("Get the language server tool", "Pegar a ferramenta do servidor de linguagem"), cu_fmt(L("Ask for the definition of %s and report it", "Pedir a definicao de %s e dizer onde fica"), fi->name)};
        x_plan(plan, 2);
        x_tsel("LSP");
        x_done(1);
        const char *r = x_call(NULL, "LSP", cu_fmt("{\"operation\": \"workspaceSymbol\", \"filePath\": %s, \"query\": %s}", cu_q(x_abs(s, x_src(p, 0))), cu_q(fi->name)), XF_ERR);
        if (!x_err(r)) cu_die("ha servidor de linguagem nesta maquina");
        const char *why = cu_fmt("%.*s", (int)strcspn(r + 7, "."), r + 7);
        int line = 0; x_locate(p, fi->name, &line, 1);
        x_final(cu_fmt(L("The language server could not answer (%s), so I searched the files instead: `%s` is defined in %s, line %d.", "O servidor de linguagem nao respondeu (%s), entao procurei nos arquivos: `%s` e definida em %s, linha %d."), why, fi->name, x_src(p, 1), line));
    } else if (kind == K_MCPRES) {
        x_user(XV(L("which resources do the connected MCP servers offer?", "que recursos os servidores MCP ligados oferecem?"), L("list the MCP resources available here", "lista os recursos MCP disponiveis aqui")));
        x_tsel("ListMcpResourcesTool");
        const char *r = x_call(NULL, "ListMcpResourcesTool", "{}", XF_ERR);
        if (strstr(r, "://")) cu_die("ha recursos MCP");
        x_final(cu_fmt(L("None: the tool answers \"%.*s\".", "Nenhum: a ferramenta responde \"%.*s\"."), (int)strcspn(x_err(r) ? r + 7 : r, "\n"), x_err(r) ? r + 7 : r));
    } else if (kind == K_AGENT) {
        XFi *fi = &p->mod[cu_n(p->nmod)].fn[0];
        static char gr[8000]; const char *ga = cu_fmt("{\"pattern\": %s, \"output_mode\": \"content\"}", cu_q(fi->name));
        ct_dispatch("Grep", ga, ga + strlen(ga), gr, sizeof gr);
        int n = 0; for (const char *l = gr; *l; l++) n += *l == '\n'; n += gr[0] && gr[strlen(gr) - 1] != '\n';
        snprintf(g_x_subans, sizeof g_x_subans, "%s is mentioned on %d lines:\n%.1500s", fi->name, n, gr);
        g_ct_sub_run = x_sub_oracle;
        x_user(cu_fmt(XV(L("have a sub-agent list every line that mentions %s, and give me the count", "manda um subagente listar toda linha que cita %s, e me da a contagem"), L("delegate this to a sub-agent: how many lines mention %s?", "delega isso a um subagente: quantas linhas citam %s?")), fi->name));
        x_tsel("Agent");
        const char *r = x_call(NULL, "Agent", cu_fmt("{\"description\": %s, \"prompt\": %s}", cu_q(cu_fmt("Find mentions of %s", fi->name)),
                               cu_q(cu_fmt("In the project at %s, search every file for the text %s. Report how many lines mention it and list them as path:line:text.", s->cwd, fi->name))), XF_ERR);
        g_ct_sub_run = NULL;
        if (x_err(r) || !strstr(r, cu_fmt("on %d lines", n))) cu_die("Agent: %.200s", r);
        x_final(cu_fmt(L("The sub-agent found %d lines that mention `%s`; its list is above.", "O subagente achou %d linhas que citam `%s`; a lista dele esta acima."), n, fi->name));
    } else if (kind == K_CRON) {
        setenv("KATANA_SCHED_NOW", "1770000000", 1);
        const int mins = 10 * (1 + cu_n(5));
        x_user(cu_fmt(XV(L("every %d minutes in this session, remind yourself to run the tests; then show me what is scheduled", "a cada %d minutos nesta sessao, se lembra de rodar os testes; depois me mostra o que esta agendado")), mins));
        const char *plan[3] = {L("Get the scheduling tools", "Pegar as ferramentas de agendamento"), cu_fmt(L("Schedule the reminder every %d minutes", "Agendar o lembrete a cada %d minutos"), mins), L("List what is scheduled", "Listar o que esta agendado")};
        x_plan(plan, 3);
        x_tsel("CronCreate,CronList");
        x_done(1);
        const char *r = x_call(NULL, "CronCreate", cu_fmt("{\"cron\": \"*/%d * * * *\", \"prompt\": %s, \"recurring\": true}", mins, cu_q(cu_fmt("Run the tests: %s", p->testcmd))), XF_ERR);
        if (x_err(r)) { unsetenv("KATANA_SCHED_NOW"); cu_die("CronCreate: %.200s", r); }
        { const char *tk = strstr(r, "task "); if (tk && g_x.nsub < 4) { g_x.sfrom[g_x.nsub] = cu_fmt("%.*s", (int)strcspn(tk + 5, " "), tk + 5); g_x.sto[g_x.nsub++] = "5c0de001"; } }
        x_done(2);
        r = cu_fmt("%s", x_call(NULL, "CronList", "{}", 0));
        unsetenv("KATANA_SCHED_NOW");
        const char *ln = x_line_with(r, cu_fmt("*/%d * * * *", mins));
        if (!ln) cu_die("CronList sem a tarefa: %.200s", r);
        ct_sched_reset();
        x_final(cu_fmt(L("Scheduled for this session only: `%s`. It ends when the session ends.", "Agendado so para esta sessao: `%s`. Acaba quando a sessao acabar."), ln));
    } else {
        const char *val = XV("staging", "canary", "nightly");
        x_user(cu_fmt(XV(L("open a shell session called build, set STAGE=%s in it and show me that the value is kept", "abre uma sessao de shell chamada build, define STAGE=%s nela e me mostra que o valor fica guardado")), val));
        const char *r = x_call(NULL, "terminal", "{\"action\": \"open\", \"name\": \"build\"}", XF_OLD);
        if (x_err(r)) cu_die("terminal open: %.200s", r);
        x_call(NULL, "terminal", cu_fmt("{\"action\": \"send\", \"name\": \"build\", \"text\": \"export STAGE=%s\"}", val), XF_OLD);
        r = cu_fmt("%s", x_call(NULL, "terminal", "{\"action\": \"send\", \"name\": \"build\", \"text\": \"echo stage=$STAGE\"}", XF_OLD));
        static char cl[2000]; agent_tool_run("terminal", "{\"action\": \"close\", \"name\": \"build\"}", "", cl, sizeof cl);
        if (!strstr(r, cu_fmt("stage=%s", val))) cu_die("terminal sem o valor: %.300s", r);
        cu_die("terminal: saida depende do tempo; fora do arquivo (%.60s)", r);
    }
    if (g_cu_hold) { c.fu = cu_pct(50) ? F_FIND : F_EXPLAIN; x_followup(&c); }      /* holdout: um pedido de outro mecanismo na mesma conversa */
    x_end(f);
}

/* ======================= uma skill de verdade ======================= */
static void fam_x_skill(FILE *f, Scn *s) {
    XC c; memset(&c, 0, sizeof c); c.s = s;
    int kind, fail;
    int stk;
    for (int t = 0; ; t++) { if (t > 200) cu_die("sem celula de skill"); kind = cu_n(3); fail = cu_pct(30); stk = CU_PA(((const int[]){X_PY, X_JS, X_SH, X_PL})); if (x_held(70 + kind, fail, stk, 0, 0) == (g_cu_hold != 0)) break; }
    x_proj(&c.p, s, stk, 2, 2); XProj *p = &c.p;
    p->brief = cu_pct(50); p->readme = !p->brief; p->knows = 1; p->conf = kind == 1;
    if (p->conf) for (int i = 0; i < CU_N(x_conf_keys); i++) x_conf_add(p, x_conf_keys[i][0], x_conf_keys[i][1], x_conf_keys[i][2]);
    if (fail) { XFi *b = &p->mod[0].fn[0]; b->num = b->f->bad; }
    const char *ver = cu_fmt("%d.%d.%d", 1 + cu_n(3), cu_n(10), cu_n(10)), *label = XV("baseline", "before-upgrade", "friday", "rc1");
    static const char *const nm[3] = {"cut-version", "preflight", "snapshot"};
    const char *md = kind == 0 ? cu_fmt("---\nname: cut-version\nwhen: Record a new version number of the project\nneeds: version\nargs: version\n---\nsteps:\n1. Run the tests: `%s` => it ends with 0 failed\n"
                                        "2. Write the file VERSION with the single line $version => VERSION contains $version\n3. Add the line `$version` at the top of CHANGELOG.md => the first line of CHANGELOG.md is $version\n"
                                        "done: VERSION and CHANGELOG.md carry $version\nreport: the version, the two files and the test totals\nfail: stop at the failing step and say which check failed\n", p->testcmd)
                   : kind == 1 ? cu_fmt("---\nname: preflight\nwhen: Check that the project is ready before a deploy\nneeds: sh\nargs:\n---\nsteps:\n1. Run the tests: `%s` => it ends with 0 failed\n"
                                        "2. Check the settings: `sh tools/check_settings.sh` => it ends with settings ok\n3. List the source files: `ls %s` => at least one file is listed\n"
                                        "done: tests and settings pass\nreport: the test totals and the last line of the settings check\nfail: stop at the failing step and say which check failed\n", p->testcmd, x_srcdir(p))
                               : cu_fmt("---\nname: snapshot\nwhen: Keep a copy of the source files under a label\nneeds: cp, label\nargs: label\n---\nsteps:\n1. Run the tests: `%s` => it ends with 0 failed\n"
                                        "2. Create the folder: `mkdir -p snapshots/$label` => the folder exists\n3. Copy the sources: `cp -r %s snapshots/$label/` => `ls snapshots/$label` lists them\n"
                                        "done: snapshots/$label holds a copy of the sources\nreport: the label, the folder and the test totals\nfail: stop at the failing step and say which check failed\n", p->testcmd, x_srcdir(p));
    x_extra(p, cu_fmt(".katana/skills/%s/SKILL.md", nm[kind]), md, 0);
    if (kind == 0) x_extra(p, "CHANGELOG.md", cu_fmt("0.9.0\n0.8.2\n"), 0);
    c.pre = !g_cu_hold && cu_pct(30) ? 1 + cu_n(2) : 0;
    x_begin("x-skill", s, p, 0, 0);
    x_cell("skill/%s/%s/%s/pre%d", nm[kind], fail ? "step-fails" : "ok", g_x_stid[p->st], c.pre);
    x_pre(&c);
    x_user(kind == 0 ? cu_fmt(XV(L("cut version %s", "fecha a versao %s"), L("record version %s for this project", "registra a versao %s deste projeto")), ver)
         : kind == 1 ? XV(L("is the project ready to deploy?", "o projeto ta pronto pra deploy?"), L("run the preflight before I deploy", "roda o preflight antes de eu fazer o deploy"))
                     : cu_fmt(XV(L("keep a copy of the sources, call it %s", "guarda uma copia dos fontes, chama de %s"), L("take a snapshot of the code labelled %s", "tira um snapshot do codigo com o rotulo %s")), label));
    const char *r = x_call(NULL, "Skill", kind == 1 ? "{\"skill\": \"preflight\"}" : cu_fmt("{\"skill\": %s, \"args\": %s}", cu_q(nm[kind]), cu_q(kind == 0 ? cu_fmt("version=%s", ver) : cu_fmt("label=%s", label))), 0);
    if (!strstr(r, "Plan written from the skill: do step 1.")) cu_die("a skill nao escreveu o plano: %.300s", r);
    const char *o = cu_fmt("%s", x_test(p, NULL)), *tot = x_totals(o); char fn[4][48];
    if (!tot) cu_die("sem totais");
    if (x_fails(o, fn, 4)) {
        if (!fail) cu_die("testes falharam sem defeito");
        x_call(NULL, "TodoWrite", cu_fmt("{\"todos\": %s}", cu_q(L("[x] Run the tests (failed: stopped here, as the skill says)", "[x] Rodar os testes (falhou: parei aqui, como a skill manda)"))), 0);
        x_final(cu_fmt(L("I stopped at step 1 of `%s`, as its `fail:` line says: the check \"it ends with 0 failed\" did not hold (`%s` ends with %s; first failure: `%s`). The other steps were not run and nothing was written.",
                         "Parei no passo 1 do `%s`, como a linha `fail:` manda: a verificacao \"it ends with 0 failed\" nao valeu (o `%s` termina com %s; primeira falha: `%s`). Os outros passos nao rodaram e nada foi gravado."), nm[kind], p->testcmd, tot, x_line_with(o, "FAIL ") + 5));
        x_end(f); return;
    }
    if (fail) cu_die("o defeito nao apareceu");
    x_done(1);
    if (kind == 0) {
        x_write(s, NULL, "VERSION", cu_fmt("%s\n", ver));
        x_done(2);
        x_need_read(p, "CHANGELOG.md");
        x_edit(s, "CHANGELOG.md", "0.9.0", cu_fmt("%s\n0.9.0", ver), 0, 0);
        if (p->brief) x_test(p, NULL);
        x_final(cu_fmt(L("Version %s is recorded: VERSION holds it and it is the first line of CHANGELOG.md. The tests: %s.", "A versao %s esta registrada: o VERSION tem ela e e a primeira linha do CHANGELOG.md. Os testes: %s."), ver, tot));
    } else if (kind == 1) {
        const char *ck = cu_fmt("%s", x_bash(NULL, "sh tools/check_settings.sh", L("Check the settings", "Confere as configuracoes"), 0));
        if (!strstr(ck, "settings ok")) cu_die("settings");
        x_done(2);
        x_bash(NULL, cu_fmt("ls %s", x_srcdir(p)), L("List the source files", "Lista os arquivos fonte"), 0);
        x_final(cu_fmt(L("Ready: the tests end with %s and the settings check ends with \"settings ok\".", "Pronto: os testes terminam com %s e a verificacao das configuracoes termina com \"settings ok\"."), tot));
    } else {
        x_bash(NULL, cu_fmt("mkdir -p snapshots/%s", label), L("Create the snapshot folder", "Cria a pasta do snapshot"), 0);
        x_done(2);
        x_bash(NULL, cu_fmt("cp -r %s snapshots/%s/", x_srcdir(p), label), L("Copy the sources", "Copia os fontes"), 0);
        const char *ls = cu_fmt("%s", x_bash(NULL, cu_fmt("ls snapshots/%s", label), L("List the snapshot", "Lista o snapshot"), 0));
        if (x_err(ls) || strstr(ls, "No such file")) cu_die("snapshot vazio");
        x_final(cu_fmt(L("Snapshot `%s` is in snapshots/%s, with a copy of %s. The tests before the copy: %s.", "O snapshot `%s` esta em snapshots/%s, com uma copia de %s. Os testes antes da copia: %s."), label, label, x_srcdir(p), tot));
    }
    if (g_cu_hold) { c.fu = cu_pct(50) ? F_FIND : F_EXPLAIN; x_followup(&c); }      /* holdout: um pedido de outro mecanismo na mesma conversa */
    x_end(f);
}

/* ======================= check: refaz a conversa inteira e compara ======================= */
static char *cu_jstr(const char *v);
static const struct { const char *name; void (*fn)(FILE *, Scn *); } g_x_fams[] = {
    {"x-fix", fam_x_fix}, {"x-add", fam_x_add}, {"x-rename", fam_x_rename}, {"x-find", fam_x_find}, {"x-explain", fam_x_explain}, {"x-run", fam_x_run}, {"x-config", fam_x_config}, {"x-clean", fam_x_clean},
    {"x-investigate", fam_x_investigate}, {"x-web", fam_x_web}, {"x-concept", fam_x_concept}, {"x-tools", fam_x_tools}, {"x-skill", fam_x_skill}, {"x-rare", fam_x_rare}, {"x-ps1", fam_x_ps1},
    {"x-mcp", fam_x_mcp}, {"x-ability", fam_x_ability}, {"x-make", fam_x_make}};
static int cu_is_old_tool(const char *n) { return !strcmp(n, "git") || !strcmp(n, "terminal") || !strcmp(n, "recall") || !strcmp(n, "remember") || !strcmp(n, "download_file") || !strcmp(n, "install_package") || !strcmp(n, "read_log") || !strcmp(n, "howto"); }
/* 1 = a linha foi refeita igual; 0 = diferente (why diz onde) */
static int x_replay(const char *line, const char *fam, long nl, char *why, size_t cap) {
    const char *sc = cu_jget(line, "scn");
    if (!sc) return 1;
    int fi = -1; for (int i = 0; i < CU_N(g_x_fams); i++) if (!strcmp(g_x_fams[i].name, fam)) fi = i;
    if (fi < 0) { snprintf(why, cap, "linha com scn de uma familia que nao e do motor de cenarios"); return 0; }
    const char *rs = cu_jstr(cu_jget(sc, "rng")), *lg = cu_jget(sc, "lang"), *hd = cu_jget(sc, "hold"), *md = cu_jget(sc, "mode"), *dc = cu_jget(sc, "decl"), *kk = cu_jget(sc, "k");
    if (!rs || !lg || !hd || !md || !dc) { snprintf(why, cap, "scn incompleto"); return 0; }
    char *mem = NULL; size_t ml = 0; FILE *mf = open_memstream(&mem, &ml);
    static Scn s;
    const int sm = g_cu_sysmode, sd = g_cu_decl;
    g_x_rng0 = g_cu_rng = strtoull(rs, NULL, 10); g_cu_lang = atoi(lg); g_cu_hold = atoi(hd); g_x_rmode = atoi(md); g_x_rdecl = atoi(dc); g_x_replay = 1; g_cu_att = 0;
    snprintf(g_cu_sb, sizeof g_cu_sb, "%s/r%ld", g_cu_sbroot, nl);
    g_cu_missing = NULL; g_cu_extra = NULL; g_cu_askno = 0;
    g_x_kforce = kk ? atoi(kk) : -1;
    g_cu_injmp = 1;
    const int failed = setjmp(g_cu_jmp);
    if (!failed) { cu_scn(&s); g_x_fams[fi].fn(mf, &s); }
    g_cu_injmp = 0; g_x_replay = 0; g_x_rmode = -1; g_x_kforce = -1; g_cu_sysmode = sm; g_cu_decl = sd; g_cu_hold = 0;
    x_unfeed();
    fclose(mf);
    if (chdir("/") != 0) {}
    cu_rmrf(g_cu_sb);
    int ok = 1;
    if (failed) { snprintf(why, cap, "ao refazer, o motor nao confirmou a conversa: %.300s", g_cu_why); ok = 0; }
    else {
        const size_t ll = strlen(line);
        if (ml != ll + 1 || memcmp(mem, line, ll)) {
            size_t d = 0; while (d < ll && d < ml && mem[d] == line[d]) d++;
            const size_t a = d > 60 ? d - 60 : 0;
            snprintf(why, cap, "refeita, a conversa sai diferente a partir de: ...%.120s  |  refeita: ...%.120s", line + a, mem + (a < ml ? a : 0));
            for (char *q = why; *q; q++) if (*q == '\n') *q = ' ';
            ok = 0;
        }
    }
    free(mem);
    return ok;
}
/* provas das perguntas: o que tinha de estar (ou de faltar) no texto anterior a pergunta ou a primeira mudanca */
static int x_askcheck(const char *line, char *why, size_t cap) {
    const char *ak = cu_jget(line, "ask"), *msgs = cu_jget(line, "messages");
    if (!ak || *ak != '[' || !msgs) return 1;
    Buf pre = {0}; buf_str(&pre, ""); int asked_call = 0, stop = 0;
    const char *el, *e;
    for (const char *p = msgs + 1; (e = gs_elem(p, &el)); p = e) {
        const char *role = cu_jstr(cu_jget(el, "role")), *content = cu_jstr(cu_jget(el, "content")), *tc = cu_jget(el, "tool_calls");
        if (role && !strcmp(role, "assistant") && tc) {
            const char *c1, *ce;
            for (const char *q = tc + 1; (ce = gs_elem(q, &c1)); q = ce) {
                const char *fnv = cu_jget(c1, "function"), *name = fnv ? cu_jstr(cu_jget(fnv, "name")) : NULL, *args = fnv ? cu_jget(fnv, "arguments") : NULL;
                if (!name) continue;
                if (!strcmp(name, "AskUserQuestion")) { asked_call = 1; stop = 1; }
                const char *cm = !strcmp(name, "Bash") && args ? cu_jstr(cu_jget(args, "command")) : NULL;
                if (!strcmp(name, "Edit") || !strcmp(name, "Write") || (cm && !strncmp(cm, "rm ", 3))) stop = 1;
            }
        }
        if (!stop && role && strcmp(role, "assistant") && content) { buf_str(&pre, content); buf_str(&pre, "\n"); }
    }
    int ok = 1;
    for (const char *p = ak + 1; ok && (e = gs_elem(p, &el)); p = e) {
        const char *as = cu_jget(el, "asked"), *need = cu_jstr(cu_jget(el, "need")), *absent = cu_jstr(cu_jget(el, "absent")), *kind = cu_jstr(cu_jget(el, "kind"));
        const int asked = as && *as == 't';
        if (asked != asked_call) { snprintf(why, cap, "prova de pergunta (%s): a conversa %s AskUserQuestion", kind ? kind : "?", asked_call ? "chama" : "nao chama"); ok = 0; break; }
        for (int i = 0, n = need && *need ? cu_count(need, '|') : 0; i < n; i++) if (!strstr(pre.p, cu_part(need, '|', i))) { snprintf(why, cap, "prova de pergunta (%s): \"%s\" devia estar no texto anterior e nao esta", kind, cu_part(need, '|', i)); ok = 0; }
        for (int i = 0, n = absent && *absent ? cu_count(absent, '|') : 0; i < n; i++) if (strstr(pre.p, cu_part(absent, '|', i))) { snprintf(why, cap, "prova de pergunta (%s): \"%s\" esta no texto anterior (o fato nao faltava)", kind, cu_part(absent, '|', i)); ok = 0; }
    }
    free(pre.p);
    return ok;
}
/* katana curriculum windows <modelo.gguf> <dados importados.jsonl> [--seq N] [--window dense] [--win-ctx N] [--win-min N]:
   as janelas que o treino cortaria, contadas com o tokenizador do modelo (so o vocabulario e carregado) */
static int cu_windows_main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: katana curriculum windows <modelo.gguf> <dados.jsonl> [--seq N] [--window fixed|dense] [--win-ctx N] [--win-min N]\n"); return 1; }
    int seq = 512;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--seq")) seq = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--window")) g_win_dense = !strcmp(argv[i + 1], "dense");
        else if (!strcmp(argv[i], "--win-ctx")) g_win_ctx = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--win-min")) g_win_min = atoi(argv[i + 1]);
        else { fprintf(stderr, "curriculum windows: opcao desconhecida %s\n", argv[i]); return 1; }
    }
    g_vocab_only = 1;
    static Model m; memset(&m, 0, sizeof m);
    load_model(&m, argv[0]);
    TSet trs = {0}; int nlines = 0;
    g_ws_for = &trs;
    if (!tset_load(&m, argv[1], seq, &trs, NULL, 0, 1, &nlines, NULL)) return 1;
    g_ws_for = NULL;
    printf("seq %d, %d linhas\n", seq, nlines);
    tset_stats_print();
    return 0;
}

