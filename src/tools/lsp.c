/* ---------- ferramenta LSP: inteligencia de codigo de um servidor de linguagem (Language Server Protocol) ----------
 * O servidor roda como processo filho e fala JSON-RPC pelo stdin/stdout com cabecalho Content-Length. Um servidor por
 * (comando, raiz do projeto), mantido vivo na sessao; morto no fim (atexit) e reiniciado se morreu. Toda leitura e
 * escrita nos canos tem prazo (poll): servidor que trava, morre ou manda lixo vira erro, nunca espera sem fim.
 * Linhas e colunas para o modelo sao 1-based; a coluna e em bytes da linha UTF-8 e vira unidades UTF-16 no protocolo.
 * Reusa o JSON minimo do cliente MCP (mcp_get, mcp_skip, mcp_elem, mcp_dupstr, McpB). */
static const char CT_LSP_DESC[] =
    "Code intelligence from a language server (LSP). operation: goToDefinition, findReferences, hover, documentSymbol, "
    "workspaceSymbol, goToImplementation, prepareCallHierarchy, incomingCalls, outgoingCalls, or diagnostics (errors and "
    "warnings of the file). filePath picks the server by extension (clangd, pyright/pylsp, typescript-language-server, "
    "rust-analyzer, gopls, bash-language-server, qmlls; override with env KATANA_LSP_<EXT>). line and character are "
    "1-based (character = byte column in the line) and are needed by the position operations. query is the search "
    "text for workspaceSymbol. Locations come back as path:line:col with the source line.";
static const char CT_LSP_PROPS[] =
    "\"operation\": {\"type\": \"string\", \"enum\": [\"goToDefinition\", \"findReferences\", \"hover\", \"documentSymbol\", "
    "\"workspaceSymbol\", \"goToImplementation\", \"prepareCallHierarchy\", \"incomingCalls\", \"outgoingCalls\", \"diagnostics\"], "
    "\"description\": \"The LSP operation to perform\"}, "
    "\"filePath\": {\"type\": \"string\", \"description\": \"The file to operate on (absolute or relative)\"}, "
    "\"line\": {\"type\": \"integer\", \"description\": \"Line number, 1-based as shown in editors\"}, "
    "\"character\": {\"type\": \"integer\", \"description\": \"Column, 1-based as shown in editors\"}, "
    "\"query\": {\"type\": \"string\", \"description\": \"Search text for workspaceSymbol\"}";
static const char CT_LSP_REQ[] = "\"operation\", \"filePath\"";

#ifndef _WIN32
#define LSP_MAXSRV 8
#define LSP_FILE_MAX (8 << 20)

typedef struct { char *path, *uri; long long mt, sz; int ver, open, diag_fresh; char *diag; } LspDoc;
typedef struct {
    char key[2400], root[1024], name[128];
    int alive, fin, fout, nreq;
    pid_t pid;
    char *rb; size_t rn, rcap;
    long nextid;
    LspDoc *docs; int ndocs;
    char err[300];
} LspSrv;
static LspSrv g_lsp[LSP_MAXSRV];
static int g_nlsp, g_lsp_atexit;

/* ----- texto: URI, colunas UTF-16 ----- */
static void lsp_uri(McpB *b, const char *path) {
    mcp_bputs(b, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (isalnum(*p) || strchr("/-._~", *p)) { char c = (char)*p; mcp_bput(b, &c, 1); }
        else { char t[4]; snprintf(t, sizeof t, "%%%02X", *p); mcp_bputs(b, t); }
    }
}
static char *lsp_unuri(const char *uri) {                  /* file:///a%20b -> /a b (memoria nova) */
    const char *p = strncmp(uri, "file://", 7) == 0 ? uri + 7 : uri;
    char *o = malloc(strlen(p) + 1); size_t n = 0;
    for (; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) { char h[3] = { p[1], p[2], 0 }; o[n++] = (char)strtol(h, NULL, 16); p += 2; }
        else o[n++] = *p;
    }
    o[n] = 0;
    return o;
}
static int lsp_cplen(unsigned char c) { return c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1; }
/* coluna em bytes (0-based) -> unidades UTF-16; arredonda para o inicio do caractere */
static long lsp_b2u(const char *s, size_t len, size_t bcol) {
    long u = 0; size_t i = 0;
    while (i < len) {
        const int k = lsp_cplen((unsigned char)s[i]);
        if (i + (size_t)k > bcol) break;
        u += k == 4 ? 2 : 1; i += (size_t)k;
    }
    return u;
}
static size_t lsp_u2b(const char *s, size_t len, long ucol) {      /* o inverso */
    long u = 0; size_t i = 0;
    while (i < len && u < ucol) { const int k = lsp_cplen((unsigned char)s[i]); u += k == 4 ? 2 : 1; i += (size_t)k; }
    return i > len ? len : i;
}
/* linha ln (0-based) do texto: inicio e tamanho sem \r\n; 0 se nao existe */
static int lsp_line(const char *txt, size_t n, long ln, const char **ls, size_t *ll) {
    const char *p = txt, *e = txt + n;
    for (long i = 0; i < ln; i++) { const char *q = memchr(p, '\n', (size_t)(e - p)); if (!q) return 0; p = q + 1; }
    if (p > e || (p == e && ln > 0 && txt[n - 1] == '\n')) return 0;
    const char *q = memchr(p, '\n', (size_t)(e - p)); if (!q) q = e;
    if (q > p && q[-1] == '\r') q--;
    *ls = p; *ll = (size_t)(q - p);
    return 1;
}
static char *lsp_slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > LSP_FILE_MAX) { fclose(f); return NULL; }
    char *b = malloc((size_t)sz + 1);
    *n = fread(b, 1, (size_t)sz, f); b[*n] = 0;
    fclose(f);
    return b;
}
/* cache do ultimo arquivo lido para mostrar as linhas dos resultados */
static char g_lsp_cpath[1200]; static char *g_lsp_ctxt; static size_t g_lsp_cn;
static const char *lsp_cached(const char *path, size_t *n) {
    if (strcmp(path, g_lsp_cpath) != 0) {
        free(g_lsp_ctxt); g_lsp_ctxt = lsp_slurp(path, &g_lsp_cn);
        snprintf(g_lsp_cpath, sizeof g_lsp_cpath, "%s", path);
    }
    *n = g_lsp_cn;
    return g_lsp_ctxt;
}

/* ----- escolha do servidor ----- */
static int lsp_onpath(const char *prog) {
    if (strchr(prog, '/')) return access(prog, X_OK) == 0;
    const char *pa = getenv("PATH");
    if (!pa) return 0;
    char d[1400];
    while (*pa) {
        const char *c = strchr(pa, ':'); const size_t l = c ? (size_t)(c - pa) : strlen(pa);
        snprintf(d, sizeof d, "%.*s/%s", (int)(l ? l : 1), l ? pa : ".", prog);
        if (access(d, X_OK) == 0) return 1;
        pa += l; if (*pa) pa++;
    }
    return 0;
}
static const char *lsp_langid(const char *ext) {
    static const char *t[][2] = {
        {"c","c"},{"h","c"},{"cc","cpp"},{"cpp","cpp"},{"cxx","cpp"},{"hpp","cpp"},{"hh","cpp"},{"hxx","cpp"},
        {"py","python"},{"pyi","python"},{"js","javascript"},{"mjs","javascript"},{"cjs","javascript"},{"jsx","javascriptreact"},
        {"ts","typescript"},{"mts","typescript"},{"cts","typescript"},{"tsx","typescriptreact"},{"rs","rust"},{"go","go"},
        {"sh","shellscript"},{"bash","shellscript"},{"qml","qml"},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (strcmp(t[i][0], ext) == 0) return t[i][1];
    return ext;
}
/* comando do servidor para a extensao (minuscula) em cmd; 0 com a mensagem de erro em err */
static int lsp_pick(const char *ext, char *cmd, size_t cap, char *err, size_t ecap) {
    char env[64]; size_t k = snprintf(env, sizeof env, "KATANA_LSP_");
    for (const char *p = ext; *p && k + 1 < sizeof env; p++) if (isalnum((unsigned char)*p)) env[k++] = (char)toupper((unsigned char)*p);
    env[k] = 0;
    const char *ov = getenv(env);
    if (ov && *ov) { snprintf(cmd, cap, "%s", ov); return 1; }
    static const struct { const char *exts, *cmds, *hint; } t[] = {
        {" c h cc cpp cxx hpp hh hxx ", "clangd", "clangd (e.g. apt install clangd)"},
        {" py pyi ", "pyright-langserver --stdio|pylsp", "pyright (npm install -g pyright) or pylsp (pip install python-lsp-server)"},
        {" js mjs cjs jsx ts mts cts tsx ", "typescript-language-server --stdio", "typescript-language-server (npm install -g typescript-language-server typescript)"},
        {" rs ", "rust-analyzer", "rust-analyzer (rustup component add rust-analyzer)"},
        {" go ", "gopls", "gopls (go install golang.org/x/tools/gopls@latest)"},
        {" sh bash ", "bash-language-server start", "bash-language-server (npm install -g bash-language-server)"},
        {" qml ", "qmlls", "qmlls (part of Qt 6: qt6-declarative-dev-tools or the Qt installer)"},
    };
    char pat[40]; snprintf(pat, sizeof pat, " %s ", ext);
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        if (!strstr(t[i].exts, pat)) continue;
        const char *c = t[i].cmds;
        while (*c) {
            const char *bar = strchr(c, '|'); const size_t l = bar ? (size_t)(bar - c) : strlen(c);
            char one[200]; snprintf(one, sizeof one, "%.*s", (int)l, c);
            char prog[200]; sscanf(one, "%199s", prog);
            if (lsp_onpath(prog)) { snprintf(cmd, cap, "%s", one); return 1; }
            c += l; if (*c) c++;
        }
        snprintf(err, ecap, "error: no language server for .%s files is installed. Install %s, or set %s to the server's "
                 "command line. Meanwhile use Grep/Read to navigate the code.", ext, t[i].hint, env);
        return 0;
    }
    snprintf(err, ecap, "error: no language server is known for .%s files; set %s to a server command line, or use Grep/Read.", ext, env);
    return 0;
}
static char **lsp_argv(const char *cmd) {                  /* divide a linha de comando (aspas simples e duplas) */
    char **av = calloc(64, sizeof *av); int n = 0;
    const char *p = cmd;
    while (*p && n < 63) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        McpB w = {0}; mcp_bputs(&w, "");
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '"' || *p == '\'') { const char q = *p++; while (*p && *p != q) mcp_bput(&w, p++, 1); if (*p) p++; }
            else mcp_bput(&w, p++, 1);
        }
        av[n++] = w.p;
    }
    return av;
}

/* ----- processo e canos ----- */
static void lsp_kill(LspSrv *s) {
    if (!s->alive) return;
    s->alive = 0;
    close(s->fin);
    int st = 0, done = 0;
    kill(s->pid, SIGTERM);
    for (int i = 0; i < 20 && !done; i++) { if (waitpid(s->pid, &st, WNOHANG) != 0) done = 1; else usleep(5000); }
    if (!done) { kill(s->pid, SIGKILL); waitpid(s->pid, &st, 0); }
    close(s->fout);
    for (int i = 0; i < s->ndocs; i++) { free(s->docs[i].path); free(s->docs[i].uri); free(s->docs[i].diag); }
    free(s->docs); s->docs = NULL; s->ndocs = 0;
    s->rn = 0;
}
static void lsp_shutdown(void) { for (int i = 0; i < g_nlsp; i++) lsp_kill(&g_lsp[i]); }
static int lsp_spawn(LspSrv *s, const char *cmd) {
    int in[2], out[2];
    signal(SIGPIPE, SIG_IGN);
    if (pipe(in) != 0) return 0;
    if (pipe(out) != 0) { close(in[0]); close(in[1]); return 0; }
    fcntl(in[1], F_SETFD, FD_CLOEXEC); fcntl(out[0], F_SETFD, FD_CLOEXEC);
    char **av = lsp_argv(cmd);
    if (!av[0]) { free(av); close(in[0]); close(in[1]); close(out[0]); close(out[1]); return 0; }
    const pid_t pid = fork();
    if (pid < 0) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); return 0; }
    if (pid == 0) {
        dup2(in[0], 0); dup2(out[1], 1);
        if (!getenv("KATANA_LSP_DEBUG")) { int dn = open("/dev/null", O_WRONLY); if (dn >= 0) dup2(dn, 2); }
        close(in[0]); close(out[1]);
        if (chdir(s->root) != 0) {}
        signal(SIGPIPE, SIG_DFL);
        execvp(av[0], av);
        _exit(127);
    }
    for (int i = 0; av[i]; i++) free(av[i]);
    free(av);
    close(in[0]); close(out[1]);
    fcntl(in[1], F_SETFL, fcntl(in[1], F_GETFL) | O_NONBLOCK);
    s->fin = in[1]; s->fout = out[0]; s->pid = pid; s->alive = 1; s->nextid = 1; s->nreq = 0; s->rn = 0;
    return 1;
}
static int lsp_send(LspSrv *s, const char *body, double deadline) {
    char hdr[64]; const size_t bl = strlen(body);
    const int hl = snprintf(hdr, sizeof hdr, "Content-Length: %zu\r\n\r\n", bl);
    for (int part = 0; part < 2; part++) {
        const char *p = part ? body : hdr; size_t n = part ? bl : (size_t)hl, off = 0;
        while (off < n) {
            const ssize_t w = write(s->fin, p + off, n - off);
            if (w > 0) { off += (size_t)w; continue; }
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                const double rem = deadline - now_sec();
                if (rem <= 0) { snprintf(s->err, sizeof s->err, "the server stopped reading its input"); lsp_kill(s); return 0; }
                struct pollfd pf = { s->fin, POLLOUT, 0 };
                poll(&pf, 1, rem > 0.2 ? 200 : (int)(rem * 1000) + 1);
                continue;
            }
            snprintf(s->err, sizeof s->err, "the server exited"); lsp_kill(s); return 0;
        }
    }
    return 1;
}
/* proxima mensagem (corpo JSON, memoria nova) ate o prazo; NULL com s->err */
static char *lsp_recv(LspSrv *s, double deadline) {
    for (;;) {
        if (s->rn) {
            char *he = NULL;
            for (size_t i = 0; i + 3 < s->rn; i++) if (memcmp(s->rb + i, "\r\n\r\n", 4) == 0) { he = s->rb + i; break; }
            if (he) {
                long long cl = -1;
                for (char *p = s->rb; p < he; ) {
                    char *e = p; while (e < he && *e != '\r') e++;
                    if ((size_t)(e - p) > 15 && strncasecmp(p, "Content-Length:", 15) == 0) cl = atoll(p + 15);
                    else if (!memchr(p, ':', (size_t)(e - p))) cl = -2;
                    p = e + 2;
                    if (cl == -2) break;
                }
                if (cl < 0 || cl > (64LL << 20)) { snprintf(s->err, sizeof s->err, "the server wrote something that is not LSP framing"); lsp_kill(s); return NULL; }
                const size_t hlen = (size_t)(he + 4 - s->rb);
                if (s->rn >= hlen + (size_t)cl) {
                    char *m = malloc((size_t)cl + 1);
                    memcpy(m, s->rb + hlen, (size_t)cl); m[cl] = 0;
                    memmove(s->rb, s->rb + hlen + cl, s->rn - hlen - (size_t)cl); s->rn -= hlen + (size_t)cl;
                    const char *v = mcp_ws(m), *e = *v == '{' ? mcp_skip(v) : NULL;
                    if (!e || *mcp_ws(e)) { free(m); snprintf(s->err, sizeof s->err, "the server sent invalid JSON"); lsp_kill(s); return NULL; }
                    return m;
                }
            } else if (s->rn > 8192) { snprintf(s->err, sizeof s->err, "the server wrote something that is not LSP framing"); lsp_kill(s); return NULL; }
        }
        const double rem = deadline - now_sec();
        if (rem <= 0) { snprintf(s->err, sizeof s->err, "timeout"); return NULL; }
        if (s->rn + 65537 > s->rcap) { s->rcap = s->rcap * 2 + 65536; s->rb = realloc(s->rb, s->rcap); if (!s->rb) die("sem memoria"); }
        struct pollfd pf = { s->fout, POLLIN, 0 };
        const int pr = poll(&pf, 1, rem > 0.2 ? 200 : (int)(rem * 1000) + 1);
        if (pr == 0) continue;
        if (pr < 0) { if (errno == EINTR) continue; snprintf(s->err, sizeof s->err, "the server exited"); lsp_kill(s); return NULL; }
        const ssize_t r = read(s->fout, s->rb + s->rn, 65536);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { snprintf(s->err, sizeof s->err, "the server exited"); lsp_kill(s); return NULL; }
        s->rn += (size_t)r;
    }
}
static LspDoc *lsp_doc(LspSrv *s, const char *uri) {
    for (int i = 0; i < s->ndocs; i++) if (strcmp(s->docs[i].uri, uri) == 0) return &s->docs[i];
    s->docs = realloc(s->docs, sizeof *s->docs * (size_t)(s->ndocs + 1));
    LspDoc *d = &s->docs[s->ndocs++];
    memset(d, 0, sizeof *d);
    d->uri = strdup(uri); d->path = lsp_unuri(uri);
    return d;
}
/* trata pedido ou aviso do servidor; 1 se a mensagem e uma resposta (sem "method") */
static int lsp_handle(LspSrv *s, const char *m) {
    const char *meth = mcp_get(m, "method"), *id = mcp_get(m, "id");
    if (!meth) return 1;
    char mn[96]; mcp_dec(meth, mn, sizeof mn);
    if (id) {                                              /* pedido do servidor: responde para ele nao esperar */
        const char *ide = mcp_skip(id);
        McpB b = {0};
        mcp_bputs(&b, "{\"jsonrpc\":\"2.0\",\"id\":"); mcp_bput(&b, id, ide ? (size_t)(ide - id) : 1);
        if (strcmp(mn, "workspace/configuration") == 0) {
            const char *items = mcp_get(mcp_get(m, "params"), "items"), *v, *p;
            mcp_bputs(&b, ",\"result\":[");
            int n = 0;
            if (items && *items == '[') for (p = items + 1; (p = mcp_elem(p, &v)) != NULL; ) mcp_bputs(&b, n++ ? ",null" : "null");
            mcp_bputs(&b, "]}");
        } else if (strcmp(mn, "workspace/workspaceFolders") == 0) {
            mcp_bputs(&b, ",\"result\":[{\"uri\":"); McpB u = {0}; lsp_uri(&u, s->root); mcp_besc(&b, u.p); free(u.p);
            mcp_bputs(&b, ",\"name\":\"root\"}]}");
        } else mcp_bputs(&b, ",\"result\":null}");
        lsp_send(s, b.p, now_sec() + 5);
        free(b.p);
        return 0;
    }
    if (strcmp(mn, "textDocument/publishDiagnostics") == 0) {
        const char *pa = mcp_get(m, "params");
        char *uri = mcp_dupstr(mcp_get(pa, "uri"));
        const char *dg = mcp_get(pa, "diagnostics"), *de = dg ? mcp_skip(dg) : NULL;
        if (uri && dg && de) {
            LspDoc *d = lsp_doc(s, uri);
            free(d->diag); d->diag = malloc((size_t)(de - dg) + 1); memcpy(d->diag, dg, (size_t)(de - dg)); d->diag[de - dg] = 0;
            d->diag_fresh = 1;
        }
        free(uri);
    }
    return 0;
}
/* pedido e espera da resposta com o mesmo id; devolve a mensagem inteira (memoria nova) ou NULL com err */
static char *lsp_request(LspSrv *s, const char *method, const char *params, double secs) {
    const long id = s->nextid++;
    McpB b = {0};
    char h[96]; snprintf(h, sizeof h, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"method\":", id);
    mcp_bputs(&b, h); mcp_besc(&b, method); mcp_bputs(&b, ",\"params\":"); mcp_bputs(&b, params); mcp_bputs(&b, "}");
    const double deadline = now_sec() + secs;
    const int ok = lsp_send(s, b.p, deadline);
    free(b.p);
    if (!ok) return NULL;
    s->nreq++;
    for (;;) {
        char *m = lsp_recv(s, deadline);
        if (!m) {
            if (strcmp(s->err, "timeout") == 0) {
                snprintf(s->err, sizeof s->err, "the server did not answer %s within %.0f s (it was stopped; the next call starts it again)", method, secs);
                lsp_kill(s);
            }
            return NULL;
        }
        if (lsp_handle(s, m)) {
            const char *iv = mcp_get(m, "id");
            if (iv && *iv != '"' && atol(iv) == id) return m;
            if (iv && *iv == '"' && atol(iv + 1) == id) return m;
        }
        free(m);
    }
}
static int lsp_notify(LspSrv *s, const char *method, const char *params) {
    McpB b = {0};
    mcp_bputs(&b, "{\"jsonrpc\":\"2.0\",\"method\":"); mcp_besc(&b, method); mcp_bputs(&b, ",\"params\":"); mcp_bputs(&b, params); mcp_bputs(&b, "}");
    const int ok = lsp_send(s, b.p, now_sec() + 5);
    free(b.p);
    return ok;
}
static double lsp_secs(const char *env, double def) { const char *v = getenv(env); return v && atof(v) > 0 ? atof(v) : def; }

/* servidor vivo para (cmd, root): reusa ou inicia (com initialize/initialized); NULL com a mensagem em err */
static LspSrv *lsp_get(const char *cmd, const char *root, char *err, size_t ecap) {
    char key[2400]; snprintf(key, sizeof key, "%s\n%s", cmd, root);
    LspSrv *s = NULL;
    for (int i = 0; i < g_nlsp; i++) if (strcmp(g_lsp[i].key, key) == 0) { s = &g_lsp[i]; break; }
    if (s && s->alive) {
        int st;
        if (waitpid(s->pid, &st, WNOHANG) == s->pid) { s->alive = 1; lsp_kill(s); s->alive = 0; }
        else return s;
    }
    if (!s) {
        if (g_nlsp == LSP_MAXSRV) { lsp_kill(&g_lsp[0]); free(g_lsp[0].rb); memmove(g_lsp, g_lsp + 1, sizeof g_lsp[0] * (LSP_MAXSRV - 1)); g_nlsp--; }
        s = &g_lsp[g_nlsp++];
        memset(s, 0, sizeof *s);
        snprintf(s->key, sizeof s->key, "%s", key);
        snprintf(s->root, sizeof s->root, "%s", root);
        char **av = lsp_argv(cmd);
        snprintf(s->name, sizeof s->name, "%.127s", av[0] ? av[0] : cmd);
        for (int i = 0; av[i]; i++) free(av[i]);
        free(av);
    }
    if (!g_lsp_atexit) { atexit(lsp_shutdown); g_lsp_atexit = 1; }
    char **av = lsp_argv(cmd);
    const int found = av[0] && lsp_onpath(av[0]);
    for (int i = 0; av[i]; i++) free(av[i]);
    free(av);
    if (!found) { snprintf(err, ecap, "error: the language server program '%s' was not found on PATH; install it or fix the command line.", s->name); return NULL; }
    if (!lsp_spawn(s, cmd)) { snprintf(err, ecap, "error: could not start the language server '%s'", s->name); return NULL; }
    McpB p = {0}, u = {0};
    lsp_uri(&u, root);
    char h[64]; snprintf(h, sizeof h, "{\"processId\":%d,\"rootUri\":", (int)getpid());
    mcp_bputs(&p, h); mcp_besc(&p, u.p);
    mcp_bputs(&p, ",\"rootPath\":"); mcp_besc(&p, root);
    mcp_bputs(&p, ",\"workspaceFolders\":[{\"uri\":"); mcp_besc(&p, u.p); mcp_bputs(&p, ",\"name\":\"root\"}]");
    mcp_bputs(&p, ",\"clientInfo\":{\"name\":\"katana\"},\"capabilities\":{\"general\":{\"positionEncodings\":[\"utf-16\"]},"
        "\"workspace\":{\"configuration\":true,\"workspaceFolders\":true,\"symbol\":{}},"
        "\"textDocument\":{\"synchronization\":{\"didSave\":false},\"hover\":{\"contentFormat\":[\"plaintext\",\"markdown\"]},"
        "\"definition\":{\"linkSupport\":true},\"implementation\":{\"linkSupport\":true},\"references\":{},"
        "\"documentSymbol\":{\"hierarchicalDocumentSymbolSupport\":true},\"callHierarchy\":{},"
        "\"publishDiagnostics\":{\"relatedInformation\":false}}}}");
    free(u.p);
    char *r = lsp_request(s, "initialize", p.p, lsp_secs("KATANA_LSP_FIRST_TIMEOUT", 60));
    free(p.p);
    if (!r) { snprintf(err, ecap, "error: language server '%s' failed to start: %s", s->name, s->err); lsp_kill(s); return NULL; }
    const int bad = mcp_get(r, "error") != NULL;
    free(r);
    if (bad) { snprintf(err, ecap, "error: language server '%s' refused initialize", s->name); lsp_kill(s); return NULL; }
    if (!lsp_notify(s, "initialized", "{}")) { snprintf(err, ecap, "error: language server '%s' failed after initialize: %s", s->name, s->err); return NULL; }
    s->nreq = 0;
    return s;
}
/* abre o arquivo no servidor ou manda o texto novo se mudou desde a ultima vez; 0 com err */
static int lsp_sync(LspSrv *s, const char *abs, const char *ext, char *err, size_t ecap) {
    struct stat st;
    if (stat(abs, &st) != 0) { snprintf(err, ecap, "error: cannot read %.500s", abs); return 0; }
    McpB u = {0}; lsp_uri(&u, abs);
    LspDoc *d = lsp_doc(s, u.p);
    if (d->open && d->mt == (long long)st.st_mtime && d->sz == (long long)st.st_size) { free(u.p); return 1; }
    size_t n = 0; char *txt = lsp_slurp(abs, &n);
    if (!txt) { free(u.p); snprintf(err, ecap, "error: cannot read %.500s (or it is larger than 8 MB)", abs); return 0; }
    McpB p = {0}; char h[64];
    mcp_bputs(&p, "{\"textDocument\":{\"uri\":"); mcp_besc(&p, u.p);
    int ok;
    if (!d->open) {
        mcp_bputs(&p, ",\"languageId\":"); mcp_besc(&p, lsp_langid(ext));
        mcp_bputs(&p, ",\"version\":1,\"text\":"); mcp_besc(&p, txt); mcp_bputs(&p, "}}");
        d->ver = 1;
        ok = lsp_notify(s, "textDocument/didOpen", p.p);
    } else {
        snprintf(h, sizeof h, ",\"version\":%d},\"contentChanges\":[{\"text\":", ++d->ver);
        mcp_bputs(&p, h); mcp_besc(&p, txt); mcp_bputs(&p, "}]}");
        ok = lsp_notify(s, "textDocument/didChange", p.p);
    }
    free(p.p); free(txt); free(u.p);
    if (!ok) { snprintf(err, ecap, "error: language server '%s' failed: %s", s->name, s->err); return 0; }
    d->open = 1; d->mt = (long long)st.st_mtime; d->sz = (long long)st.st_size; d->diag_fresh = 0;
    return 1;
}

/* ----- saida com limite ----- */
typedef struct { char *o; size_t cap, n; int omitted; } LspOut;
static void lsp_put(LspOut *w, const char *s) {           /* texto fixo (cabecalhos): corta se nao couber */
    const size_t l = strlen(s);
    if (w->n + l + 1 > w->cap) return;
    memcpy(w->o + w->n, s, l); w->n += l; w->o[w->n] = 0;
}
static void lsp_entry(LspOut *w, const char *s) {         /* uma entrada; guarda 100 bytes para a nota de omitidos */
    const size_t l = strlen(s);
    if (w->omitted || w->n + l + 2 + 100 > w->cap) { w->omitted++; return; }
    memcpy(w->o + w->n, s, l); w->n += l; w->o[w->n++] = '\n'; w->o[w->n] = 0;
}
static void lsp_end(LspOut *w) {
    if (w->omitted) { char t[100]; snprintf(t, sizeof t, "(%d more entries omitted)\n", w->omitted); w->omitted = 0; lsp_put(w, t); }
    while (w->n && w->o[w->n - 1] == '\n') w->o[--w->n] = 0;
}
static const char *lsp_kind(long k) {
    static const char *t[] = { "Symbol", "File", "Module", "Namespace", "Package", "Class", "Method", "Property", "Field",
        "Constructor", "Enum", "Interface", "Function", "Variable", "Constant", "String", "Number", "Boolean", "Array",
        "Object", "Key", "Null", "EnumMember", "Struct", "Event", "Operator", "TypeParameter" };
    return k >= 1 && k <= 26 ? t[k] : t[0];
}
static long lsp_num(const char *v) { return v ? atol(v) : 0; }
/* "path:line:col" de uri + posicao (0-based, UTF-16) usando o texto no disco; line_txt recebe a linha (ou "") */
static void lsp_loc(const char *root, const char *uri, const char *pos, char *o, size_t cap, char *ltxt, size_t lcap) {
    char *path = lsp_unuri(uri);
    const long ln = lsp_num(mcp_get(pos, "line")), uc = lsp_num(mcp_get(pos, "character"));
    size_t n = 0; const char *txt = lsp_cached(path, &n), *ls = NULL; size_t ll = 0;
    long col = uc + 1;
    if (ltxt) ltxt[0] = 0;
    if (txt && lsp_line(txt, n, ln, &ls, &ll)) {
        col = (long)lsp_u2b(ls, ll, uc) + 1;
        if (ltxt) {
            while (ll && (*ls == ' ' || *ls == '\t')) { ls++; ll--; }
            snprintf(ltxt, lcap, "%.*s", (int)(ll > 200 ? 200 : ll), ls);
        }
    }
    const size_t rl = strlen(root);
    const char *shown = (strncmp(path, root, rl) == 0 && path[rl] == '/') ? path + rl + 1 : path;
    snprintf(o, cap, "%s:%ld:%ld", shown, ln + 1, col);
    free(path);
}
/* uma localizacao (Location ou LocationLink) como entrada */
static void lsp_put_loc(LspOut *w, const char *root, const char *v) {
    char *uri = mcp_dupstr(mcp_get(v, "uri"));
    const char *rg = mcp_get(v, "range");
    if (!uri) { uri = mcp_dupstr(mcp_get(v, "targetUri")); rg = mcp_get(v, "targetSelectionRange"); if (!rg) rg = mcp_get(v, "targetRange"); }
    if (!uri || !rg) { free(uri); return; }
    char loc[1400], lt[220], e[1700];
    lsp_loc(root, uri, mcp_get(rg, "start"), loc, sizeof loc, lt, sizeof lt);
    snprintf(e, sizeof e, "%s: %s", loc, lt);
    lsp_entry(w, e);
    free(uri);
}
static int lsp_locations(LspOut *w, const char *root, const char *res) {
    const char *v, *p; int n = 0;
    if (*res == '{') { lsp_put_loc(w, root, res); return 1; }
    if (*res != '[') return 0;
    for (p = res + 1; (p = mcp_elem(p, &v)) != NULL; n++) lsp_put_loc(w, root, v);
    return n;
}
static void lsp_markup(McpB *b, const char *v) {          /* conteudo de hover em texto simples */
    if (!v) return;
    if (*v == '"') { char *s = mcp_dupstr(v); mcp_bputs(b, s); mcp_bputs(b, "\n"); free(s); return; }
    if (*v == '[') { const char *e, *p; for (p = v + 1; (p = mcp_elem(p, &e)) != NULL; ) lsp_markup(b, e); return; }
    if (*v == '{') { lsp_markup(b, mcp_get(v, "value")); }
}
static void lsp_symbols(LspOut *w, const char *root, const char *arr, int depth, int with_loc) {
    const char *v, *p;
    if (!arr || *arr != '[') return;
    for (p = arr + 1; (p = mcp_elem(p, &v)) != NULL; ) {
        char *name = mcp_dupstr(mcp_get(v, "name")), *det = mcp_dupstr(mcp_get(v, "detail")), *cont = mcp_dupstr(mcp_get(v, "containerName"));
        const char *kind = lsp_kind(lsp_num(mcp_get(v, "kind")));
        const char *rg = mcp_get(v, "selectionRange"), *lo = mcp_get(v, "location");
        char where[1500] = "", e[2400];
        if (lo) {
            char *uri = mcp_dupstr(mcp_get(lo, "uri")); const char *r2 = mcp_get(lo, "range");
            if (uri && r2) lsp_loc(root, uri, mcp_get(r2, "start"), where, sizeof where, NULL, 0);
            if (uri && !r2) { char *pa = lsp_unuri(uri); snprintf(where, sizeof where, "%s", pa); free(pa); }
            free(uri);
        }
        if (!with_loc) {
            if (!rg) rg = mcp_get(v, "range");
            if (!rg && lo) rg = mcp_get(lo, "range");
            snprintf(e, sizeof e, "%*s%s %s%s%s (line %ld)", depth * 2, "", kind, name ? name : "?", det && *det ? " - " : "", det && *det ? det : "",
                     rg ? lsp_num(mcp_get(mcp_get(rg, "start"), "line")) + 1 : 0);
        } else snprintf(e, sizeof e, "%s %s%s%s - %s", kind, name ? name : "?", cont && *cont ? " in " : "", cont && *cont ? cont : "", where);
        lsp_entry(w, e);
        free(name); free(det); free(cont);
        lsp_symbols(w, root, mcp_get(v, "children"), depth + 1, with_loc);
    }
}
static void lsp_item(LspOut *w, const char *root, const char *it, const char *pre) {    /* CallHierarchyItem */
    char *name = mcp_dupstr(mcp_get(it, "name")), *uri = mcp_dupstr(mcp_get(it, "uri"));
    const char *rg = mcp_get(it, "selectionRange"); if (!rg) rg = mcp_get(it, "range");
    char loc[1400] = "?", e[1800];
    if (uri && rg) lsp_loc(root, uri, mcp_get(rg, "start"), loc, sizeof loc, NULL, 0);
    snprintf(e, sizeof e, "%s%s %s - %s", pre, lsp_kind(lsp_num(mcp_get(it, "kind"))), name ? name : "?", loc);
    lsp_entry(w, e);
    free(name); free(uri);
}
static int lsp_diag_cmp(const void *a, const void *b) { const long *x = a, *y = b; return x[0] != y[0] ? (x[0] < y[0] ? -1 : 1) : (x[1] < y[1] ? -1 : x[1] > y[1]); }

static void ct_lsp(const char *js, const char *end, char *out, size_t cap) {
    char *op = ct_str(js, end, "operation"), *fp = ct_str(js, end, "filePath"), *query = ct_str(js, end, "query");
    const long line = (long)ct_int(js, end, "line", 0), chr = (long)ct_int(js, end, "character", 0);
    char abs[4200], err[600], cmd[1200], root[1024];
    LspSrv *s = NULL; char *resp = NULL;
    static const char *ops[] = { "goToDefinition", "findReferences", "hover", "documentSymbol", "workspaceSymbol",
        "goToImplementation", "prepareCallHierarchy", "incomingCalls", "outgoingCalls", "diagnostics" };
    int oi = -1;
    for (int i = 0; op && i < 10; i++) if (strcmp(op, ops[i]) == 0) oi = i;
    if (oi < 0) { snprintf(out, cap, "error: operation must be one of goToDefinition, findReferences, hover, documentSymbol, workspaceSymbol, goToImplementation, prepareCallHierarchy, incomingCalls, outgoingCalls, diagnostics"); goto fim; }
    if (!fp || !*fp) { snprintf(out, cap, "error: filePath is required"); goto fim; }
    if (!agent_read_access(fp, 0, abs, sizeof abs, err, sizeof err)) { snprintf(out, cap, "%s", err); goto fim; }
    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode)) { snprintf(out, cap, "error: %s does not exist or is not a file", fp); goto fim; }
    const int needpos = oi != 3 && oi != 4 && oi != 9;
    if (needpos && (line < 1 || chr < 1)) { snprintf(out, cap, "error: %s needs line and character (both 1-based)", op); goto fim; }
    /* extensao e servidor */
    const char *base = strrchr(abs, '/'); base = base ? base + 1 : abs;
    const char *dot = strrchr(base, '.');
    char ext[32] = "";
    if (dot && dot != base) for (size_t i = 0; dot[1 + i] && i + 1 < sizeof ext; i++) { ext[i] = (char)tolower((unsigned char)dot[1 + i]); ext[i + 1] = 0; }
    if (!ext[0]) { snprintf(out, cap, "error: %s has no extension, so no language server can be chosen", fp); goto fim; }
    if (!lsp_pick(ext, cmd, sizeof cmd, err, sizeof err)) { snprintf(out, cap, "%s", err); goto fim; }
    /* raiz: o pai mais proximo com um marcador de projeto */
    {
        static const char *mk[] = { ".git", "compile_commands.json", "pyproject.toml", "package.json", "Cargo.toml", "go.mod" };
        char d[1024]; snprintf(d, sizeof d, "%.1023s", abs);
        root[0] = 0;
        for (char *sl; !root[0] && (sl = strrchr(d, '/')) != NULL; ) {
            *sl = 0;
            const char *dir = d[0] ? d : "/";
            for (int i = 0; i < 6 && !root[0]; i++) { char t[1100]; struct stat s2; snprintf(t, sizeof t, "%s/%s", dir, mk[i]); if (stat(t, &s2) == 0) snprintf(root, sizeof root, "%s", dir); }
            if (!d[0]) break;
        }
        if (!root[0] && !getcwd(root, sizeof root)) snprintf(root, sizeof root, "/");
    }
    s = lsp_get(cmd, root, err, sizeof err);
    if (!s) { snprintf(out, cap, "%s", err); goto fim; }
    if (!lsp_sync(s, abs, ext, err, sizeof err)) { snprintf(out, cap, "%s", err); goto fim; }
    const double secs = s->nreq == 0 ? lsp_secs("KATANA_LSP_FIRST_TIMEOUT", 60) : lsp_secs("KATANA_LSP_TIMEOUT", 20);
    LspOut w = { out, cap, 0, 0 }; out[0] = 0;
    McpB u = {0}; lsp_uri(&u, abs);
    const char *rel = (strncmp(abs, root, strlen(root)) == 0 && abs[strlen(root)] == '/') ? abs + strlen(root) + 1 : abs;

    if (oi == 9) {                                         /* diagnostics: espera um pouco pelo aviso do servidor */
        LspDoc *d = lsp_doc(s, u.p);
        const double dl = now_sec() + lsp_secs("KATANA_LSP_DIAG_WAIT", 3);
        while (!d->diag_fresh && s->alive) {
            char *m = lsp_recv(s, dl);
            if (!m) break;
            lsp_handle(s, m); free(m);
            d = lsp_doc(s, u.p);
        }
        if (!s->alive) { snprintf(out, cap, "error: language server '%s' failed: %s", s->name, s->err); free(u.p); goto fim; }
        if (!d->diag) { snprintf(out, cap, "No diagnostics reported for %s (the server sent none within %.0f s).", rel, lsp_secs("KATANA_LSP_DIAG_WAIT", 3)); free(u.p); goto fim; }
        const char *v, *p; int n = 0;
        long (*ix)[3] = NULL;
        for (p = d->diag + 1; *d->diag == '[' && (p = mcp_elem(p, &v)) != NULL; n++) {
            ix = realloc(ix, sizeof *ix * (size_t)(n + 1));
            const char *stt = mcp_get(mcp_get(v, "range"), "start");
            ix[n][0] = lsp_num(mcp_get(stt, "line")); ix[n][1] = lsp_num(mcp_get(stt, "character")); ix[n][2] = (long)(v - d->diag);
        }
        if (!n) { snprintf(out, cap, "No diagnostics: the server reports no errors or warnings in %s.", rel); free(u.p); goto fim; }
        qsort(ix, (size_t)n, sizeof *ix, lsp_diag_cmp);
        static const char *sev[] = { "error", "error", "warning", "info", "hint" };
        for (int i = 0; i < n; i++) {
            v = d->diag + ix[i][2];
            char loc[1400], e[2400], msg[900] = "";
            const char *mv = mcp_get(v, "message"); if (mv) mcp_dec(mv, msg, sizeof msg);
            for (char *q = msg; *q; q++) if (*q == '\n') *q = ' ';
            lsp_loc(root, u.p, mcp_get(mcp_get(v, "range"), "start"), loc, sizeof loc, NULL, 0);
            const long sv = lsp_num(mcp_get(v, "severity"));
            snprintf(e, sizeof e, "%s: %s: %s", loc, sev[sv >= 1 && sv <= 4 ? sv : 0], msg);
            lsp_entry(&w, e);
        }
        free(ix);
        lsp_end(&w); free(u.p); goto fim;
    }

    /* parametros: posicao convertida de bytes para UTF-16 */
    McpB p = {0};
    long uch = chr - 1;
    if (needpos) {
        size_t n = 0; char *txt = lsp_slurp(abs, &n); const char *ls; size_t ll;
        if (!txt || !lsp_line(txt, n, line - 1, &ls, &ll)) { free(txt); snprintf(out, cap, "error: %s has no line %ld", fp, line); free(u.p); goto fim; }
        uch = lsp_b2u(ls, ll, (size_t)(chr - 1) > ll ? ll : (size_t)(chr - 1));
        free(txt);
        char h[96];
        mcp_bputs(&p, "{\"textDocument\":{\"uri\":"); mcp_besc(&p, u.p);
        snprintf(h, sizeof h, "},\"position\":{\"line\":%ld,\"character\":%ld}", line - 1, uch);
        mcp_bputs(&p, h);
        if (oi == 1) mcp_bputs(&p, ",\"context\":{\"includeDeclaration\":true}");
        mcp_bputs(&p, "}");
    } else if (oi == 3) { mcp_bputs(&p, "{\"textDocument\":{\"uri\":"); mcp_besc(&p, u.p); mcp_bputs(&p, "}}"); }
    else { mcp_bputs(&p, "{\"query\":"); mcp_besc(&p, query ? query : ""); mcp_bputs(&p, "}"); }
    static const char *meth[] = { "textDocument/definition", "textDocument/references", "textDocument/hover",
        "textDocument/documentSymbol", "workspace/symbol", "textDocument/implementation", "textDocument/prepareCallHierarchy",
        "textDocument/prepareCallHierarchy", "textDocument/prepareCallHierarchy" };
    resp = lsp_request(s, meth[oi], p.p, secs);
    free(p.p); free(u.p);
    if (!resp) { snprintf(out, cap, "error: language server '%s' failed: %s", s->name, s->err); goto fim; }
    const char *er = mcp_get(resp, "error");
    if (er) { char m[400] = "?"; const char *mv = mcp_get(er, "message"); if (mv) mcp_dec(mv, m, sizeof m); snprintf(out, cap, "error: language server '%s' returned an error for %s: %s", s->name, op, m); goto fim; }
    const char *res = mcp_get(resp, "result");
    const int empty = !res || strncmp(res, "null", 4) == 0 || (*res == '[' && *mcp_ws(res + 1) == ']');
    char hd[1400];
    switch (oi) {
    case 0: case 1: case 5: {
        static const char *none[] = { "No definition found", "No references found", "", "", "", "No implementation found" };
        if (empty) { snprintf(out, cap, "%s at %s:%ld:%ld.", none[oi], rel, line, chr); goto fim; }
        int n = 0; const char *v, *q;
        if (*res == '[') for (q = res + 1; (q = mcp_elem(q, &v)) != NULL; ) n++;
        else n = 1;
        if (oi == 1) { snprintf(hd, sizeof hd, "%d reference%s:\n", n, n == 1 ? "" : "s"); lsp_put(&w, hd); }
        lsp_locations(&w, root, res);
        break;
    }
    case 2: {
        McpB t = {0}; mcp_bputs(&t, "");
        if (!empty) lsp_markup(&t, mcp_get(res, "contents"));
        /* tira as cercas de codigo do markdown e linhas vazias sobrando */
        char *q = t.p; size_t o = 0;
        for (char *ln = q; ln && *ln; ) {
            char *nl = strchr(ln, '\n'); const size_t l = nl ? (size_t)(nl - ln) : strlen(ln);
            if (!(l >= 3 && strncmp(ln, "```", 3) == 0)) { memmove(q + o, ln, l); o += l; q[o++] = '\n'; }
            ln = nl ? nl + 1 : NULL;
        }
        q[o] = 0;
        while (o && (q[o - 1] == '\n' || q[o - 1] == ' ')) q[--o] = 0;
        if (!o) snprintf(out, cap, "No hover information at %s:%ld:%ld.", rel, line, chr);
        else if (o + 1 > cap) snprintf(out, cap, "%.*s\n(truncated)", (int)(cap > 20 ? cap - 20 : 0), q);
        else snprintf(out, cap, "%s", q);
        free(t.p);
        goto fim;
    }
    case 3: case 4:
        if (empty) { snprintf(out, cap, oi == 3 ? "No symbols found in %s." : "No symbols match '%s'.", oi == 3 ? rel : (query ? query : "")); goto fim; }
        lsp_symbols(&w, root, res, 0, oi == 4);
        break;
    default: {                                             /* hierarquia de chamadas */
        if (empty) { snprintf(out, cap, "No call hierarchy item at %s:%ld:%ld.", rel, line, chr); goto fim; }
        if (oi == 6) { const char *v, *q; for (q = res + 1; (q = mcp_elem(q, &v)) != NULL; ) lsp_item(&w, root, v, ""); break; }
        const char *it = NULL; mcp_elem(res + 1, &it);
        const char *ie = mcp_skip(it);
        char *nm = mcp_dupstr(mcp_get(it, "name"));
        McpB q = {0}; mcp_bputs(&q, "{\"item\":"); mcp_bput(&q, it, (size_t)(ie - it)); mcp_bputs(&q, "}");
        char *r2 = lsp_request(s, oi == 7 ? "callHierarchy/incomingCalls" : "callHierarchy/outgoingCalls", q.p, lsp_secs("KATANA_LSP_TIMEOUT", 20));
        free(q.p);
        if (!r2) { snprintf(out, cap, "error: language server '%s' failed: %s", s->name, s->err); free(nm); goto fim; }
        const char *e2 = mcp_get(r2, "error"), *res2 = mcp_get(r2, "result");
        if (e2) { char m[400] = "?"; const char *mv = mcp_get(e2, "message"); if (mv) mcp_dec(mv, m, sizeof m); snprintf(out, cap, "error: language server '%s' returned an error for %s: %s", s->name, op, m); free(nm); free(r2); goto fim; }
        if (!res2 || *res2 != '[' || *mcp_ws(res2 + 1) == ']') {
            snprintf(out, cap, oi == 7 ? "No callers of %s found." : "%s calls nothing that the server knows.", nm ? nm : "?");
            free(nm); free(r2); goto fim;
        }
        snprintf(hd, sizeof hd, oi == 7 ? "Callers of %s:\n" : "Calls made by %s:\n", nm ? nm : "?");
        lsp_put(&w, hd);
        const char *v, *qq;
        for (qq = res2 + 1; (qq = mcp_elem(qq, &v)) != NULL; ) lsp_item(&w, root, mcp_get(v, oi == 7 ? "from" : "to"), "  ");
        free(nm); free(r2);
        break;
    }
    }
    lsp_end(&w);
fim:
    free(op); free(fp); free(query); free(resp);
}
#else
static void ct_lsp(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: LSP is not available on Windows yet");
}
#endif
