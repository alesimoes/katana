/* ---------- MCP: o que vai alem do basico (revisao 2025-11-25 do protocolo; ver docs/extending.md) ----------
 * Incluido depois de mcp.c. Aqui ficam: expansao de variaveis na configuracao, roots, avisos de lista,
 * anotacoes das ferramentas, elicitation. Os ganchos em mcp.c sao pequenos. */

/* ----- a: ${VAR} e ${VAR:-padrao} em command, args, env, url e headers -----
 * "$${" vale como "${" literal. Variavel ausente (e sem padrao): erro com o NOME da variavel, nunca o valor. */
static int mcpx_expand(const char *in, char **out, char *err, size_t ecap) {
    McpB b = {0};
    mcp_bput(&b, "", 0);
    for (const char *p = in; *p; p++) {
        if (p[0] == '$' && p[1] == '$' && p[2] == '{') { mcp_bputs(&b, "${"); p += 2; continue; }
        if (p[0] != '$' || p[1] != '{') { mcp_bput(&b, p, 1); continue; }
        const char *e = strchr(p + 2, '}');
        if (!e) { snprintf(err, ecap, "unterminated ${ in the configuration"); free(b.p); return 0; }
        char spec[300];
        const size_t sl = (size_t)(e - (p + 2));
        if (sl == 0 || sl >= sizeof spec) { snprintf(err, ecap, "bad ${...} in the configuration"); free(b.p); return 0; }
        memcpy(spec, p + 2, sl); spec[sl] = 0;
        char *def = strstr(spec, ":-");
        if (def) { *def = 0; def += 2; }
        int okname = spec[0] != 0;
        for (const char *c = spec; *c; c++) if (!(isalnum((unsigned char)*c) || *c == '_')) okname = 0;
        if (!okname) { snprintf(err, ecap, "bad variable name in ${...} (letters, digits and _ only)"); free(b.p); return 0; }
        const char *v = getenv(spec);
        if (v && *v) mcp_bputs(&b, v);
        else if (def) mcp_bputs(&b, def);
        else if (v) { /* definida e vazia, sem padrao: vale vazia */ }
        else { snprintf(err, ecap, "environment variable %.80s is not set (use ${%.80s:-default} for a default)", spec, spec); free(b.p); return 0; }
        p = e;
    }
    *out = b.p;
    return 1;
}
/* troca *s pela versao expandida; 0 se faltou variavel (err diz qual) */
static int mcpx_expand_in_place(char **s, char *err, size_t ecap) {
    if (!*s || !strstr(*s, "${")) return 1;
    char *o = NULL;
    if (!mcpx_expand(*s, &o, err, ecap)) return 0;
    free(*s); *s = o;
    return 1;
}
/* aplica a expansao a um servidor lido da configuracao; em caso de erro libera o que o inicia e deixa err */
static int mcpx_expand_srv(McpSrv *t, const char *where) {
    char e[200] = "", f[60] = "";
    int ok = 1;
    if (ok && !(ok = mcpx_expand_in_place(&t->cmd, e, sizeof e))) snprintf(f, sizeof f, "command");
    if (ok && !(ok = mcpx_expand_in_place(&t->url, e, sizeof e))) snprintf(f, sizeof f, "url");
    for (int i = 0; ok && i < t->nargs; i++) if (!(ok = mcpx_expand_in_place(&t->args[i], e, sizeof e))) snprintf(f, sizeof f, "args");
    for (int i = 0; ok && i < t->nenv; i++) if (!(ok = mcpx_expand_in_place(&t->envv[i], e, sizeof e))) snprintf(f, sizeof f, "env");
    for (int i = 0; ok && i < t->nhdr; i++) if (!(ok = mcpx_expand_in_place(&t->hdr[i], e, sizeof e))) snprintf(f, sizeof f, "headers");
    if (ok) return 1;
    free(t->cmd); t->cmd = NULL; free(t->url); t->url = NULL;       /* nao inicia */
    snprintf(t->err, sizeof t->err, "skipped: %.150s (field %s)", e, f);
    fprintf(stderr, "[mcp %s: skipped, %s (field %s of %s)]\n", t->name, e, f, where);
    return 0;
}

/* ----- respostas do cliente a pedidos do servidor ----- */
static void mcpx_reply(McpSrv *s, const char *idraw, size_t idlen, const char *result, int code, const char *msg) {
    McpB r = {0};
    mcp_bputs(&r, "{\"jsonrpc\":\"2.0\",\"id\":");
    mcp_bput(&r, idraw, idlen);
    if (result) { mcp_bputs(&r, ",\"result\":"); mcp_bputs(&r, result); }
    else { char t[40]; snprintf(t, sizeof t, ",\"error\":{\"code\":%d,\"message\":", code); mcp_bputs(&r, t); mcp_besc(&r, msg); mcp_bputs(&r, "}"); }
    mcp_bputs(&r, "}");
    if (s->http) { char *x = mcp_http_post(s, r.p, 0, 10000); free(x); }       /* 202 sem corpo */
    else mcp_send(s, r.p);
    free(r.p);
}

/* ----- b: roots. A raiz e a pasta de trabalho do agente, como URI file:// ----- */
static char g_mcpx_cwd[4200];
static void mcpx_file_uri(const char *path, McpB *b) {
    mcp_bputs(b, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        char t[8];
        if (isalnum(*p) || strchr("-._~/", *p)) mcp_bput(b, (const char *)p, 1);
        else { snprintf(t, sizeof t, "%%%02X", *p); mcp_bputs(b, t); }
    }
}
static void mcpx_roots_result(McpB *b) {
    char cwd[4200];
    if (!getcwd(cwd, sizeof cwd)) { mcp_bputs(b, "{\"roots\":[]}"); return; }
    const char *base = strrchr(cwd, '/');
    base = base && base[1] ? base + 1 : cwd;
    mcp_bputs(b, "{\"roots\":[{\"uri\":\"");
    mcpx_file_uri(cwd, b);
    mcp_bputs(b, "\",\"name\":");
    mcp_besc(b, base);
    mcp_bputs(b, "}]}");
}
/* a pasta de trabalho mudou desde o ultimo aviso? avisa os servidores conectados (notifications/roots/list_changed).
 * Chamado quando o usuario troca de pasta (/cd, worktree) e antes de cada chamada de ferramenta. */
static void mcpx_cwd_sync(void) {
    char cwd[4200];
    if (!g_mcp_ready || !getcwd(cwd, sizeof cwd)) return;
    if (!g_mcpx_cwd[0]) { snprintf(g_mcpx_cwd, sizeof g_mcpx_cwd, "%s", cwd); return; }
    if (strcmp(cwd, g_mcpx_cwd) == 0) return;
    snprintf(g_mcpx_cwd, sizeof g_mcpx_cwd, "%s", cwd);
    for (int i = 0; i < g_nmcp; i++) if (g_mcp[i].alive) mcp_notify(&g_mcp[i], "notifications/roots/list_changed");
}

/* ----- tempo extra que uma chamada HTTP pode esperar quando ha um usuario para responder (elicitation) ----- */
static int mcpx_extra_wait(void) { return getenv("KATANA_UI") || K_ISATTY(0) ? 600 : 0; }

/* ----- mensagem do servidor (pedido ou aviso) chegando por stdio ou pelo fluxo SSE ----- */
static void mcpx_server_msg(McpSrv *s, const char *o) {
    const char *iv = mcp_get(o, "id"), *mv = mcp_get(o, "method");
    char m[120] = "";
    if (mv && *mv == '"') mcp_dec(mv, m, sizeof m);
    if (!iv) {                              /* aviso */
        if (!strcmp(m, "notifications/tools/list_changed")) s->dirty = 1;
        else if (!strcmp(m, "notifications/resources/list_changed")) s->rdirty = 1;
        else if (!strcmp(m, "notifications/prompts/list_changed")) s->pdirty = 1;
        return;
    }
    const char *ie = mcp_skip(iv);
    const size_t il = ie ? (size_t)(ie - iv) : 0;
    if (!il) return;
    if (!strcmp(m, "ping")) { mcpx_reply(s, iv, il, "{}", 0, NULL); return; }
    if (!strcmp(m, "roots/list")) {
        McpB r = {0}; mcp_bput(&r, "", 0);
        mcpx_roots_result(&r);
        mcpx_reply(s, iv, il, r.p, 0, NULL);
        free(r.p);
        return;
    }
    mcpx_reply(s, iv, il, NULL, -32601, "method not found");
}

/* eventos SSE completos em o[*done..]: trata pedidos do servidor, devolve em *resp a resposta do id. final=1: o fluxo acabou */
static void mcpx_sse_scan(McpSrv *s, McpB *o, size_t *done, long id, char **resp, int final) {
    for (;;) {
        const char *b = o->p + *done, *e = NULL;
        for (const char *q = b; *q;) {
            const size_t ll = strcspn(q, "\n");
            if (!q[ll]) break;
            if (ll == 0 || (ll == 1 && *q == '\r')) { e = q; break; }
            q += ll + 1;
        }
        size_t skip = 0;
        if (e) skip = (size_t)(e + strcspn(e, "\n") + 1 - b); else if (final && *b) { e = b + strlen(b); skip = strlen(b); } else return;
        McpB ev = {0};
        mcp_bput(&ev, "", 0);
        for (const char *q = b; q < e;) {
            size_t ll = strcspn(q, "\n"); if (q + ll > e) ll = (size_t)(e - q);
            size_t cl = ll; if (cl && q[cl - 1] == '\r') cl--;
            if (cl >= 5 && !strncmp(q, "data:", 5)) { const char *d = q + 5; if (*d == ' ') d++; if (ev.n) mcp_bputs(&ev, "\n"); mcp_bput(&ev, d, cl - (size_t)(d - q)); }
            q += ll; if (q < e) q++;
        }
        *done += skip;
        const char *js = strchr(ev.p, '{');
        if (js) {
            if (mcp_get(js, "method")) mcpx_server_msg(s, js);
            else if (!*resp) { const char *iv = mcp_get(js, "id"); if (iv && atol(iv) == id && (mcp_get(js, "result") || mcp_get(js, "error"))) *resp = strdup(js); }
        }
        free(ev.p);
    }
}
