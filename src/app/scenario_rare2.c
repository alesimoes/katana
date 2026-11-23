/* ---------- katana curriculum, cenarios compostos: preparos de verdade para as ferramentas raras ----------
 * O que as situacoes de x-rare precisam ter em volta para a ferramenta rodar de verdade, sem modelo e sem sair da pasta
 * do cenario:
 *   - um servidor MCP pequeno (stdio, escrito em tools/<nome>_server.py do projeto) ligado ao cliente do motor antes do
 *     prompt ser montado: o cartao aparece no prompt e ListMcpResourcesTool / ReadMcpResourceTool / WaitForMcpServers
 *     falam com um processo de verdade; um segundo servidor cujo comando nao existe da o caminho "failed";
 *   - outra sessao do katana registrada na caixa de correio da pasta HOME do cenario (um processo parado faz o papel dela):
 *     ListAgents mostra, SendMessage entrega;
 *   - o subagente: com KATANA_AGENT_FAKE=1 o motor chama o gancho g_ct_sub_run; aqui o gancho devolve o que uma busca de
 *     verdade (Grep/Glob do motor, rodados na hora) acha no projeto, no formato da resposta de um subagente;
 *   - um servidor de linguagem minimo (pylsp de mentira na pasta bin do cenario: acha `def nome` nos .py do projeto);
 *   - arquivos: log, caderno, script que fica rodando, roteiro de workflow, configuracoes. */

enum { P_NONE, P_MCP, P_MCPDOWN, P_MAIL, P_BG, P_NB, P_GIT, P_GITMOD, P_CONF, P_WWW, P_FLOW, P_AGENT, P_LSP };

/* ---- servidor MCP de verdade ---- */
static const char *g_r2_srvname;       /* nome do servidor desta conversa */
static int g_r2_down;                  /* registra tambem um servidor cujo comando nao existe */
static pid_t g_r2_srvpid; static int g_r2_srvfin = -1, g_r2_srvfout = -1;
static const char *x_r2_server_py(const char *srv, const char *proj) {
    return cu_fmt(
        "import json, sys\n"
        "# small MCP server over stdio: the team notes of %s as resources, plus one search tool\n"
        "TEXT = {\n"
        "    'notes://today': 'Today: finish the review of %s, then tag the release.\\nBlocked: nothing.',\n"
        "    'notes://release-checklist': '1. run the tests\\n2. update docs/functions.md\\n3. tag the release\\n4. tell the team',\n"
        "    'notes://decisions': 'Whole numbers only. Division rounds down. One file per module.',\n"
        "}\n"
        "RES = [\n"
        "    {'uri': 'notes://today', 'name': 'today', 'mimeType': 'text/plain', 'description': 'what the team does today'},\n"
        "    {'uri': 'notes://release-checklist', 'name': 'release checklist', 'mimeType': 'text/plain', 'description': 'steps before a release'},\n"
        "    {'uri': 'notes://decisions', 'name': 'decisions', 'mimeType': 'text/plain', 'description': 'decisions already taken'},\n"
        "    {'uri': 'notes://logo', 'name': 'logo', 'mimeType': 'image/png', 'description': 'the project logo'},\n"
        "]\n"
        "TOOLS = [{'name': 'search', 'description': 'Search the notes for a word', 'inputSchema': {'type': 'object', 'properties': {'query': {'type': 'string', 'description': 'word to look for'}}, 'required': ['query']}}]\n"
        "for line in sys.stdin:\n"
        "    line = line.strip()\n"
        "    if not line:\n        continue\n"
        "    m = json.loads(line)\n"
        "    if 'id' not in m:\n        continue\n"
        "    meth, p, err = m.get('method'), m.get('params') or {}, None\n"
        "    if meth == 'initialize':\n        r = {'protocolVersion': p.get('protocolVersion', '2024-11-05'), 'capabilities': {'tools': {}, 'resources': {}}, 'serverInfo': {'name': '%s', 'version': '1.2'}}\n"
        "    elif meth == 'tools/list':\n        r = {'tools': TOOLS}\n"
        "    elif meth == 'resources/list':\n        r = {'resources': RES}\n"
        "    elif meth == 'resources/read':\n"
        "        u = p.get('uri')\n"
        "        if u in TEXT:\n            r = {'contents': [{'uri': u, 'mimeType': 'text/plain', 'text': TEXT[u]}]}\n"
        "        elif u == 'notes://logo':\n            r = {'contents': [{'uri': u, 'mimeType': 'image/png', 'blob': 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGBgAAAABQABh6FO1AAAAABJRU5ErkJggg=='}]}\n"
        "        else:\n            err = {'code': -32002, 'message': 'resource not found: %%s' %% u}\n"
        "    elif meth == 'tools/call':\n"
        "        q = str((p.get('arguments') or {}).get('query', ''))\n"
        "        hits = [u for u, t in sorted(TEXT.items()) if q and q.lower() in t.lower()]\n"
        "        r = {'content': [{'type': 'text', 'text': '\\n'.join(hits) if hits else 'no note mentions %%s' %% q}]}\n"
        "    else:\n        err = {'code': -32601, 'message': 'method not found'}\n"
        "    print(json.dumps({'jsonrpc': '2.0', 'id': m['id'], 'error': err} if err else {'jsonrpc': '2.0', 'id': m['id'], 'result': r}), flush=True)\n",
        proj, proj, srv);
}
static void x_r2_srv_stop(void) {
    if (g_r2_srvpid <= 0) return;
    /* se o registro do cliente ainda tem este processo, quem fecha e o proprio cliente (nao fechar os canais duas vezes) */
    for (int i = 0; i < g_nmcp; i++) if (g_mcp[i].pid == g_r2_srvpid && g_mcp[i].cmd && !strcmp(g_mcp[i].cmd, "python3")) {
        if (g_mcp[i].spawned) mcp_kill(&g_mcp[i]);
        g_r2_srvpid = 0; g_r2_srvfin = g_r2_srvfout = -1; return;
    }
    if (g_r2_srvfin >= 0) close(g_r2_srvfin);
    int st = 0, done = 0;
    for (int i = 0; i < 30 && !done; i++) { if (waitpid(g_r2_srvpid, &st, WNOHANG) != 0) done = 1; else usleep(10000); }
    if (!done) { kill(g_r2_srvpid, SIGTERM); waitpid(g_r2_srvpid, &st, 0); }
    if (g_r2_srvfout >= 0) close(g_r2_srvfout);
    g_r2_srvpid = 0; g_r2_srvfin = g_r2_srvfout = -1;
}
/* chamado por x_begin depois de registrar os servidores do cenario e antes de montar o prompt */
static void x_r2_mcp_hook(const Scn *s) {
    const char *srv = g_r2_srvname, *rel = cu_fmt("tools/%s_server.py", srv);
    cu_put(cu_fmt("%s/%s", s->cwd, rel), x_r2_server_py(srv, s->name), 0);
    cu_put(cu_fmt("%s/.katana/mcp.json", s->cwd), cu_fmt("{\"mcpServers\": {\"%s\": {\"command\": \"python3\", \"args\": [\"%s\"]}%s}}\n", srv, rel,
           g_r2_down ? ", \"archive\": {\"command\": \"archive-mcp\", \"args\": [\"--stdio\"]}" : ""), 0);
    cu_put(cu_fmt("%s/.katana/mcp/%s.md", s->cwd, srv), cu_fmt("what: the team notes of %s (resources) and a search over them\nfirst: \norder: \navoid: \n", s->name), 0);
    McpSrv *v = &g_mcp[g_nmcp++]; memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", srv); v->project = 1;
    snprintf(v->source, sizeof v->source, "%s/.katana/mcp.json", s->cwd);
    v->cmd = strdup("python3"); v->args = malloc(sizeof(char *)); v->args[0] = strdup(cu_fmt("%s/%s", s->cwd, rel)); v->nargs = 1;
    mcp_start(v);
    if (!v->alive) cu_die("o servidor MCP do cenario nao subiu: %s", v->err);
    g_r2_srvpid = v->pid; g_r2_srvfin = v->fin; g_r2_srvfout = v->fout;
    if (g_r2_down) {
        McpSrv *d = &g_mcp[g_nmcp++]; memset(d, 0, sizeof *d);
        snprintf(d->name, sizeof d->name, "archive"); d->project = 1;
        snprintf(d->source, sizeof d->source, "%s/.katana/mcp.json", s->cwd);
        d->cmd = strdup("archive-mcp");
        mcp_start(d);
        if (d->alive) cu_die("o comando archive-mcp existe nesta maquina");
    }
}

/* ---- outra sessao do katana na caixa de correio ---- */
static pid_t g_r2_peer;
static void x_r2_peer_stop(void) { if (g_r2_peer > 0) { kill(g_r2_peer, SIGTERM); waitpid(g_r2_peer, NULL, 0); g_r2_peer = 0; } }
static long x_r2_peer_start(const Scn *s, const char *name) {
    const pid_t pid = fork();
    if (pid < 0) cu_die("sem processo para a outra sessao");
    if (pid == 0) {      /* sem os canais do gerador: quem espera a saida dele nao fica preso a este processo */
        const int dn = open("/dev/null", O_RDWR); if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        execlp("sleep", "sleep", "120", (char *)NULL); _exit(127);
    }
    g_r2_peer = pid;
    cu_mkdirs(cu_fmt("%s/.katana/mail/agents", s->home));
    cu_put(cu_fmt("%s/.katana/mail/agents/%ld", s->home, (long)pid), cu_fmt("%s\n%s/%s-ci\n", name, s->home, s->name), 0);
    return (long)pid;
}

/* ---- subagente: o gancho devolve o que a busca de verdade acha ---- */
static char g_r2_sub[2][2600]; static int g_r2_subn;
static void x_r2_oracle(const char *prompt, char *out, size_t cap) {
    (void)prompt;
    snprintf(out, cap, "[sub-agent answer; 1 tool calls]\n%s", g_r2_sub[g_r2_subn < 1 ? g_r2_subn : 1]); g_r2_subn++;
}
static int x_r2_lines(const char *t) { int n = 0; for (const char *l = t; *l; l++) n += *l == '\n'; return n + (t[0] && t[strlen(t) - 1] != '\n'); }
/* kind: 1 linhas que citam a funcao; 2 arquivos de teste; 3 marcas TODO; 4 arquivos de codigo. Devolve tambem o numero que a resposta final cita */
static int x_r2_subans(int kind, const char *fn, const char *srcglob, char *dst, size_t cap) {
    static char r[6000];
    const char *a = kind == 1 ? cu_fmt("{\"pattern\": %s, \"output_mode\": \"content\"}", cu_q(fn)) : kind == 3 ? "{\"pattern\": \"TODO\", \"output_mode\": \"content\"}" : kind == 2 ? "{\"pattern\": \"**/test_*\"}" : cu_fmt("{\"pattern\": %s}", cu_q(srcglob));
    ct_dispatch(kind == 1 || kind == 3 ? "Grep" : "Glob", a, a + strlen(a), r, sizeof r);
    const int none = !strncmp(r, "No ", 3) || !*r;
    const int n = none ? 0 : x_r2_lines(r);
    if (kind == 1) snprintf(dst, cap, "%s is mentioned on %d lines:\n%.1800s", fn, n, r);
    else if (kind == 3) snprintf(dst, cap, none ? "No line of the project has a TODO mark." : "%d lines have a TODO mark:\n%.1800s", n, r);
    else if (kind == 2) snprintf(dst, cap, "%d test file%s:\n%.1800s", n, n == 1 ? "" : "s", r);
    else snprintf(dst, cap, "%d source file%s:\n%.1800s", n, n == 1 ? "" : "s", r);
    return n;
}

/* ---- servidor de linguagem minimo: so o que o LSP do motor pede (initialize, didOpen, workspace/symbol, documentSymbol, definition, references) ---- */
static const char g_r2_pylsp[] =
    "#!/usr/bin/env python3\n"
    "# minimal language server for the Python files of one project: it knows the top-level `def` lines and nothing else\n"
    "import json, os, re, sys\n"
    "root = '.'\n"
    "def send(o):\n"
    "    b = json.dumps(o).encode()\n"
    "    sys.stdout.buffer.write(b'Content-Length: %d\\r\\n\\r\\n' % len(b) + b); sys.stdout.buffer.flush()\n"
    "def files():\n"
    "    out = []\n"
    "    for d, dn, fn in os.walk(root):\n"
    "        dn[:] = sorted(x for x in dn if not x.startswith('.'))\n"
    "        out += [os.path.join(d, f) for f in sorted(fn) if f.endswith('.py')]\n"
    "    return out\n"
    "def defs(path):\n"
    "    for i, l in enumerate(open(path, encoding='utf-8', errors='replace').read().split('\\n')):\n"
    "        m = re.match(r'def (\\w+)\\(', l)\n"
    "        if m: yield m.group(1), i, l\n"
    "def loc(path, i, a, b): return {'uri': 'file://' + path, 'range': {'start': {'line': i, 'character': a}, 'end': {'line': i, 'character': b}}}\n"
    "def word(p):\n"
    "    path = p['textDocument']['uri'][7:]; pos = p['position']\n"
    "    l = open(path, encoding='utf-8', errors='replace').read().split('\\n')[pos['line']]\n"
    "    for m in re.finditer(r'\\w+', l):\n"
    "        if m.start() <= pos['character'] <= m.end(): return m.group(0)\n"
    "    return ''\n"
    "while True:\n"
    "    n = 0\n"
    "    while True:\n"
    "        h = sys.stdin.buffer.readline()\n"
    "        if not h: sys.exit(0)\n"
    "        h = h.strip()\n"
    "        if not h: break\n"
    "        if h.lower().startswith(b'content-length:'): n = int(h.split(b':')[1])\n"
    "    m = json.loads(sys.stdin.buffer.read(n)); meth = m.get('method'); p = m.get('params') or {}\n"
    "    if meth == 'exit': sys.exit(0)\n"
    "    if 'id' not in m: continue\n"
    "    r = None\n"
    "    if meth == 'initialize':\n"
    "        root = p.get('rootPath') or '.'\n"
    "        r = {'capabilities': {'textDocumentSync': 1, 'definitionProvider': True, 'referencesProvider': True, 'documentSymbolProvider': True, 'workspaceSymbolProvider': True}, 'serverInfo': {'name': 'mini-pylsp'}}\n"
    "    elif meth == 'workspace/symbol':\n"
    "        q = p.get('query', '')\n"
    "        r = [{'name': nm, 'kind': 12, 'location': loc(f, i, 4, 4 + len(nm))} for f in files() for nm, i, l in defs(f) if q in nm]\n"
    "    elif meth == 'textDocument/documentSymbol':\n"
    "        f = p['textDocument']['uri'][7:]\n"
    "        r = [{'name': nm, 'kind': 12, 'location': loc(f, i, 4, 4 + len(nm))} for nm, i, l in defs(f)]\n"
    "    elif meth == 'textDocument/definition':\n"
    "        w = word(p)\n"
    "        r = [loc(f, i, 4, 4 + len(nm)) for f in files() for nm, i, l in defs(f) if nm == w]\n"
    "    elif meth == 'textDocument/references':\n"
    "        w = word(p); r = []\n"
    "        for f in files():\n"
    "            for i, l in enumerate(open(f, encoding='utf-8', errors='replace').read().split('\\n')):\n"
    "                for mm in re.finditer(r'\\b%s\\b' % re.escape(w), l) if w else []: r.append(loc(f, i, mm.start(), mm.end()))\n"
    "    send({'jsonrpc': '2.0', 'id': m['id'], 'result': r})\n";

/* limpa o que uma conversa anterior (ou uma tentativa descartada) deixou rodando */
static void x_r2_cleanup(void);
static void x_r2_atexit(void) { x_r2_cleanup(); }
static void x_r2_cleanup(void) {
    static int reg; if (!reg) { reg = 1; atexit(x_r2_atexit); }
    x_r2_srv_stop(); x_r2_peer_stop();
    lsp_shutdown(); g_nlsp = 0;
    g_r2_subn = 0; g_r2_down = 0; g_r2_srvname = NULL;
}
