/* ---------- katana mcp serve: o katana como servidor MCP (stdio) ----------
 * Expoe as ferramentas de arquivos e busca do proprio katana (Read, Write, Edit, Glob, Grep, NotebookEdit) e, so com
 * --allow-shell, Bash. Quem chama e o cliente MCP de outro programa; o katana nao pergunta nada a ninguem aqui:
 *  - as regras never e ask do KATANA.md da pasta valem (never recusa; ask recusa, porque nao ha a quem perguntar);
 *  - os caminhos ficam presos a pasta de trabalho (cwd, --cwd, --root) ou as roots que o cliente declarar (roots/list);
 *  - Bash nao e preso a pasta (um shell faz qualquer coisa): por isso so liga com --allow-shell.
 * O protocolo anda num descritor proprio (copia do stdout original); o stdout das ferramentas vai para o stderr e o stdin
 * das ferramentas e /dev/null, assim nada que uma ferramenta imprima estraga o protocolo nem consome mensagens.
 * Revisao do protocolo: aceita 2025-11-25, 2025-06-18 e 2025-03-26 (a mesma negociacao do cliente). */
#ifndef _WIN32
#define MCPS_MAXROOT 8
typedef struct { const char *name, *title; int ro, destr, idem, open, shell; } McpsTool;
static const McpsTool g_mcps_tools[] = {
    {"Read", "Read a file", 1, 0, 1, 0, 0},
    {"Glob", "Find files by name", 1, 0, 1, 0, 0},
    {"Grep", "Search file contents", 1, 0, 1, 0, 0},
    {"Write", "Write a file", 0, 1, 1, 0, 0},
    {"Edit", "Edit a file", 0, 0, 0, 0, 0},
    {"NotebookEdit", "Edit a notebook cell", 0, 0, 0, 0, 0},
    {"Bash", "Run a shell command", 0, 1, 0, 1, 1},
    {NULL, NULL, 0, 0, 0, 0, 0}
};
static char g_mcps_root[MCPS_MAXROOT][4200];
static int g_mcps_nroot, g_mcps_shell, g_mcps_readonly, g_mcps_out = 1;

static void mcps_send(const char *msg) {
    const size_t n = strlen(msg);
    size_t o = 0;
    while (o < n) { const ssize_t w = write(g_mcps_out, msg + o, n - o); if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; } o += (size_t)w; }
    if (write(g_mcps_out, "\n", 1) < 0) { /* sem leitor: o proximo read acaba */ }
}
static void mcps_error(const char *idraw, size_t il, int code, const char *msg) {
    McpB r = {0};
    mcp_bputs(&r, "{\"jsonrpc\":\"2.0\",\"id\":"); mcp_bput(&r, idraw, il);
    char t[40]; snprintf(t, sizeof t, ",\"error\":{\"code\":%d,\"message\":", code); mcp_bputs(&r, t); mcp_besc(&r, msg); mcp_bputs(&r, "}}");
    mcps_send(r.p); free(r.p);
}
static void mcps_result(const char *idraw, size_t il, const char *result) {
    McpB r = {0};
    mcp_bputs(&r, "{\"jsonrpc\":\"2.0\",\"id\":"); mcp_bput(&r, idraw, il); mcp_bputs(&r, ",\"result\":"); mcp_bputs(&r, result); mcp_bputs(&r, "}");
    mcps_send(r.p); free(r.p);
}
/* caminho normalizado sem "." e ".."; links resolvidos no que ja existe. 0 se nao deu */
static int mcps_resolve(const char *path, char *out, size_t cap) {
    char abs[4600], norm[4600];
    if (path[0] == '/') snprintf(abs, sizeof abs, "%s", path);
    else { char cwd[4200]; if (!getcwd(cwd, sizeof cwd)) return 0; snprintf(abs, sizeof abs, "%s/%s", cwd, path); }
    size_t n = 0;
    norm[0] = 0;
    for (char *save = NULL, *t = strtok_r(abs, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".") || !*t) continue;
        if (!strcmp(t, "..")) { while (n && norm[n - 1] != '/') n--; if (n) n--; norm[n] = 0; continue; }
        n += (size_t)snprintf(norm + n, sizeof norm - n, "/%s", t);
    }
    if (!n) snprintf(norm, sizeof norm, "/");
    /* realpath do maior prefixo que existe, depois o resto */
    char head[4600], rest[4600] = "", real[4600];
    snprintf(head, sizeof head, "%s", norm);
    for (;;) {
        if (realpath(head, real)) break;
        char *sl = strrchr(head, '/');
        if (!sl) return 0;
        snprintf(rest + 0, 1, "%s", "");
        char tmp[4600]; snprintf(tmp, sizeof tmp, "%s%s", sl, rest); snprintf(rest, sizeof rest, "%s", tmp);
        if (sl == head) { head[1] = 0; if (!realpath("/", real)) return 0; break; }
        *sl = 0;
    }
    snprintf(out, cap, "%s%s", real, strcmp(real, "/") ? rest : (rest[0] ? rest : ""));
    if (!out[0]) snprintf(out, cap, "/");
    return 1;
}
static int mcps_inside(const char *path) {
    char r[4600];
    if (!mcps_resolve(path, r, sizeof r)) return 0;
    for (int i = 0; i < g_mcps_nroot; i++) {
        const size_t l = strlen(g_mcps_root[i]);
        if (!strncmp(r, g_mcps_root[i], l) && (r[l] == 0 || r[l] == '/' || l == 1)) return 1;
    }
    return 0;
}
static void mcps_add_root(const char *p) {
    char r[4600];
    if (g_mcps_nroot >= MCPS_MAXROOT || !realpath(p, r) || strlen(r) >= sizeof g_mcps_root[0]) return;
    snprintf(g_mcps_root[g_mcps_nroot++], sizeof g_mcps_root[0], "%s", r);
}
/* roots/list do cliente: troca as raizes; devolve quantas valeram */
static void mcps_roots_from(const char *res) {
    const char *arr = res ? mcp_get(res, "roots") : NULL;
    if (!arr || *arr != '[') return;
    char old[MCPS_MAXROOT][4200]; const int on = g_mcps_nroot;
    memcpy(old, g_mcps_root, sizeof old);
    g_mcps_nroot = 0;
    const char *v, *np;
    for (const char *p = arr + 1; (np = mcp_elem(p, &v)) != NULL; p = np) {
        char *u = mcp_dupstr(mcp_get(v, "uri"));
        if (u && !strncmp(u, "file://", 7)) {
            char dec[4200]; size_t k = 0;
            for (const char *q = u + 7; *q && k + 1 < sizeof dec; q++) {
                if (*q == '%' && isxdigit((unsigned char)q[1]) && isxdigit((unsigned char)q[2])) { char h[3] = {q[1], q[2], 0}; dec[k++] = (char)strtol(h, NULL, 16); q += 2; }
                else dec[k++] = *q;
            }
            dec[k] = 0;
            mcps_add_root(dec);
        }
        free(u);
    }
    if (!g_mcps_nroot) { memcpy(g_mcps_root, old, sizeof old); g_mcps_nroot = on; return; }
    if (!mcps_inside(".") && chdir(g_mcps_root[0]) != 0) { /* fica onde esta */ }
}
/* confere os caminhos dos argumentos; 1 = ok, senao out recebe o motivo */
static int mcps_args_ok(const char *js, const char *end, char *out, size_t cap) {
    static const char *const keys[] = {"file_path", "path", "notebook_path", "pattern", "glob", NULL};
    for (int i = 0; keys[i]; i++) {
        char *v = ct_str(js, end, keys[i]);
        if (!v) continue;
        const int pat = !strcmp(keys[i], "pattern") || !strcmp(keys[i], "glob");
        int ok = 1;
        if (pat) { if (v[0] == '/' || strstr(v, "../") || !strcmp(v, "..")) ok = v[0] == '/' ? mcps_inside(v) && !strstr(v, "..") : 0; }
        else if (v[0]) ok = mcps_inside(v);
        if (!ok) { snprintf(out, cap, "error: %.200s is outside the folders this katana server may use (%s); use a path inside the working folder", v, g_mcps_root[0]); free(v); return 0; }
        free(v);
    }
    return 1;
}
static const McpsTool *mcps_find(const char *name) {
    for (int i = 0; g_mcps_tools[i].name; i++) {
        const McpsTool *t = &g_mcps_tools[i];
        if (strcmp(t->name, name)) continue;
        if (t->shell && !g_mcps_shell) return NULL;
        if (g_mcps_readonly && !t->ro) return NULL;
        return t;
    }
    return NULL;
}
static void mcps_tools_list(McpB *o) {
    mcp_bputs(o, "{\"tools\":[");
    int n = 0;
    for (int i = 0; g_mcps_tools[i].name; i++) {
        const McpsTool *t = &g_mcps_tools[i];
        if (!mcps_find(t->name)) continue;
        const char *name, *desc, *props, *req;
        int found = 0;
        for (int k = 0; ct_tool_info(k, &name, &desc, &props, &req); k++) if (!strcmp(name, t->name)) { found = 1; break; }
        if (!found) continue;
        if (n++) mcp_bputs(o, ",");
        mcp_bputs(o, "{\"name\":"); mcp_besc(o, t->name);
        mcp_bputs(o, ",\"title\":"); mcp_besc(o, t->title);
        mcp_bputs(o, ",\"description\":"); mcp_besc(o, desc);
        mcp_bputs(o, ",\"inputSchema\":{\"type\":\"object\",\"properties\":{"); mcp_bputs(o, props); mcp_bputs(o, "}");
        if (req && req[0]) { mcp_bputs(o, ",\"required\":["); mcp_bputs(o, req); mcp_bputs(o, "]"); }
        mcp_bputs(o, "},\"annotations\":{");
        char a[200];
        snprintf(a, sizeof a, "\"readOnlyHint\":%s,\"destructiveHint\":%s,\"idempotentHint\":%s,\"openWorldHint\":%s", t->ro ? "true" : "false", t->destr ? "true" : "false", t->idem ? "true" : "false", t->open ? "true" : "false");
        mcp_bputs(o, a); mcp_bputs(o, "}}");
    }
    mcp_bputs(o, "]}");
}
static int mcps_serve_cli(int argc, char **argv) {
    const char *cwd = NULL;
    const char *roots[MCPS_MAXROOT]; int nr = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--allow-shell")) g_mcps_shell = 1;
        else if (!strcmp(argv[i], "--read-only")) g_mcps_readonly = 1;
        else if (!strcmp(argv[i], "--cwd") && i + 1 < argc) cwd = argv[++i];
        else if (!strcmp(argv[i], "--root") && i + 1 < argc && nr < MCPS_MAXROOT) roots[nr++] = argv[++i];
        else { fprintf(stderr, "usage: katana mcp serve [--cwd DIR] [--root DIR]... [--read-only] [--allow-shell]\n"); return 2; }
    }
    if (cwd && chdir(cwd) != 0) { fprintf(stderr, "katana mcp serve: cannot enter %s\n", cwd); return 1; }
    { char c[4200]; if (!getcwd(c, sizeof c)) return 1; if (!nr) mcps_add_root(c); for (int i = 0; i < nr; i++) mcps_add_root(roots[i]); }
    if (!g_mcps_nroot) { fprintf(stderr, "katana mcp serve: no usable folder\n"); return 1; }
    /* protocolo em descritores proprios; as ferramentas ficam com stdout=stderr e stdin=/dev/null */
    const int pin = dup(0);
    g_mcps_out = dup(1);
    if (pin < 0 || g_mcps_out < 0) return 1;
    fcntl(pin, F_SETFD, FD_CLOEXEC); fcntl(g_mcps_out, F_SETFD, FD_CLOEXEC);
    dup2(2, 1);
    { int dn = open("/dev/null", O_RDONLY); if (dn >= 0) { dup2(dn, 0); close(dn); } }
    unsetenv("KATANA_UI");
    signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "katana mcp serve: folder %s%s%s (MCP 2025-11-25)\n", g_mcps_root[0], g_mcps_shell ? ", shell ON" : ", shell off", g_mcps_readonly ? ", read-only" : "");
    FILE *in = fdopen(pin, "r");
    if (!in) return 1;
    static char line[1 << 20], res[TOOL_WEB + 512];
    int client_roots = 0, initialized = 0;
    while (fgets(line, sizeof line, in)) {
        const char *o = strchr(line, '{');
        if (!o) continue;
        const char *iv = mcp_get(o, "id"), *mv = mcp_get(o, "method");
        char m[100] = "";
        if (mv && *mv == '"') mcp_dec(mv, m, sizeof m);
        if (!mv) {                                          /* resposta ao nosso roots/list */
            const char *rs = mcp_get(o, "result");
            if (iv && rs && !strncmp(iv, "\"katana-roots\"", 14)) mcps_roots_from(rs);
            continue;
        }
        const char *ie = iv ? mcp_skip(iv) : NULL;
        const size_t il = ie ? (size_t)(ie - iv) : 0;
        if (!iv) {
            if (!strcmp(m, "notifications/initialized") && client_roots) mcps_send("{\"jsonrpc\":\"2.0\",\"id\":\"katana-roots\",\"method\":\"roots/list\"}");
            else if (!strcmp(m, "notifications/roots/list_changed") && client_roots) mcps_send("{\"jsonrpc\":\"2.0\",\"id\":\"katana-roots\",\"method\":\"roots/list\"}");
            continue;
        }
        if (!strcmp(m, "initialize")) {
            const char *pr = mcp_get(o, "params"), *pv = pr ? mcp_get(pr, "protocolVersion") : NULL, *cp = pr ? mcp_get(pr, "capabilities") : NULL;
            char ver[40] = "2025-11-25";
            if (pv && *pv == '"') { char c[40]; mcp_dec(pv, c, sizeof c); if (!strcmp(c, "2025-06-18") || !strcmp(c, "2025-03-26") || !strcmp(c, "2025-11-25")) snprintf(ver, sizeof ver, "%s", c); }
            client_roots = cp && mcp_get(cp, "roots") != NULL;
            McpB r = {0};
            char t[300];
            snprintf(t, sizeof t, "{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"katana\",\"title\":\"katana file and search tools\",\"version\":\"0.1\"},", ver);
            mcp_bputs(&r, t);
            mcp_bputs(&r, "\"instructions\":");
            mcp_besc(&r, g_mcps_shell ? "Files, search and a shell, limited to the working folder by path (the shell is not limited). The KATANA.md rules of the folder apply." : "Files and search, limited to the working folder. The KATANA.md rules of the folder apply.");
            mcp_bputs(&r, "}");
            mcps_result(iv, il, r.p); free(r.p);
            initialized = 1;
            continue;
        }
        if (!strcmp(m, "ping")) { mcps_result(iv, il, "{}"); continue; }
        if (!initialized) { mcps_error(iv, il, -32002, "not initialized"); continue; }
        if (!strcmp(m, "tools/list")) { McpB r = {0}; mcp_bput(&r, "", 0); mcps_tools_list(&r); mcps_result(iv, il, r.p); free(r.p); continue; }
        if (!strcmp(m, "tools/call")) {
            const char *pr = mcp_get(o, "params");
            char *tn = pr ? mcp_dupstr(mcp_get(pr, "name")) : NULL;
            const McpsTool *t = tn ? mcps_find(tn) : NULL;
            if (!t) { mcps_error(iv, il, -32602, "unknown tool"); free(tn); continue; }
            const char *av = mcp_get(pr, "arguments");
            char *args = NULL;
            if (av && *av == '{') { const char *e = mcp_skip(av); if (e) { args = malloc((size_t)(e - av) + 1); memcpy(args, av, (size_t)(e - av)); args[e - av] = 0; } }
            if (!args) args = strdup("{}");
            res[0] = 0;
            if (mcps_args_ok(args, args + strlen(args), res, sizeof res)) agent_tool(tn, args, args + strlen(args), res, sizeof res);
            McpB r = {0};
            mcp_bputs(&r, "{\"content\":[{\"type\":\"text\",\"text\":"); mcp_besc(&r, res); mcp_bputs(&r, "}],\"isError\":");
            mcp_bputs(&r, strncmp(res, "error", 5) == 0 ? "true}" : "false}");
            mcps_result(iv, il, r.p);
            free(r.p); free(args); free(tn);
            continue;
        }
        mcps_error(iv, il, -32601, "method not found");
    }
    return 0;
}
#else
static int mcps_serve_cli(int argc, char **argv) { (void)argc; (void)argv; fprintf(stderr, "katana mcp serve needs a POSIX system\n"); return 1; }
#endif
