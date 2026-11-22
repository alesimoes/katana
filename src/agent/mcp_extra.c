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
