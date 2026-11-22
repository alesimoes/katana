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

static int mcpx_elicit(McpSrv *s, const char *params, McpB *res);       /* abaixo (f) */
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
    if (!strcmp(m, "elicitation/create")) {
        McpB r = {0}; mcp_bput(&r, "", 0);
        if (mcpx_elicit(s, mcp_get(o, "params"), &r)) mcpx_reply(s, iv, il, r.p, 0, NULL);
        else mcpx_reply(s, iv, il, NULL, -32602, "unsupported elicitation request (katana handles form mode with simple fields)");
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

/* ----- c: resources/list_changed e prompts/list_changed -----
 * Os prompts sao relidos (o usuario os chama com /servidor:prompt); os recursos sao listados a cada pedido, entao so
 * avisamos o modelo. note pode ser NULL (so rele). */
static void mcpx_refresh(McpSrv *s, McpB *note) {
    if (s->pdirty) {
        s->pdirty = 0;
        if (s->alive) { mcp_load_prompts(s); if (s->alive) s->err[0] = 0; }
        if (note) { if (note->n) mcp_bputs(note, "\n"); mcp_bputs(note, "[the prompt list of this MCP server changed; the user's /server:prompt list is up to date]"); }
    }
    if (s->rdirty) {
        s->rdirty = 0;
        if (note) { if (note->n) mcp_bputs(note, "\n"); mcp_bputs(note, "[the resource list of this MCP server changed: ListMcpResourcesTool shows the current resources]"); }
    }
}

/* ----- d: anotacoes das ferramentas (title, readOnlyHint, destructiveHint, idempotentHint, openWorldHint) -----
 * Sao dicas do servidor e nao se verificam (a propria especificacao diz para tratar como nao confiaveis). Regras:
 *  - mostrar: katana mcp list/test, e no texto da confirmacao;
 *  - so deixam o katana mais cuidadoso, ou dispensam a pergunta/o modo plano quando o servidor e do proprio usuario
 *    (arquivo ~/.katana/mcp.json ou KATANA_MCP_CONFIG); servidor vindo de arquivo de projeto nunca ganha nada. */
static int mcpx_bool(const char *v) { return !v ? -1 : (strncmp(v, "true", 4) == 0 ? 1 : (strncmp(v, "false", 5) == 0 ? 0 : -1)); }
static void mcpx_ann_parse(McpTool *t, const char *v) {
    t->ro = t->destr = t->idem = t->open = -1;
    t->title = mcp_dupstr(mcp_get(v, "title"));
    const char *an = mcp_get(v, "annotations");
    if (!an || *an != '{') return;
    t->ro = (signed char)mcpx_bool(mcp_get(an, "readOnlyHint"));
    t->destr = (signed char)mcpx_bool(mcp_get(an, "destructiveHint"));
    t->idem = (signed char)mcpx_bool(mcp_get(an, "idempotentHint"));
    t->open = (signed char)mcpx_bool(mcp_get(an, "openWorldHint"));
    if (!t->title) t->title = mcp_dupstr(mcp_get(an, "title"));
    if (t->title) { if (strlen(t->title) > 80) t->title[80] = 0; for (char *c = t->title; *c; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' '; }
}
/* "read-only, idempotent": o que o servidor declarou (vazio se nada) */
static void mcpx_ann_text(const McpSrv *s, int tj, char *out, size_t cap) {
    const McpTool *t = &s->tools[tj];
    out[0] = 0;
    size_t o = 0;
    if (t->ro == 1) o += (size_t)snprintf(out + o, cap - o, "%sread-only", o ? ", " : "");
    if (t->destr == 1) o += (size_t)snprintf(out + o, cap - o, "%sdestructive", o ? ", " : "");
    if (t->idem == 1) o += (size_t)snprintf(out + o, cap - o, "%sidempotent", o ? ", " : "");
    if (t->open == 1) o += (size_t)snprintf(out + o, cap - o, "%sopen-world", o ? ", " : "");
}
/* 1 se a dica de somente leitura de uma ferramenta pode ser levada em conta: servidor do usuario, sem dica contraditoria */
static int mcpx_readonly_trusted(const McpSrv *s, int tj) {
    const McpTool *t = &s->tools[tj];
    return !s->project && t->ro == 1 && t->destr != 1;
}
static int mcpx_skip_confirm(const McpSrv *s, int tj) { return mcpx_readonly_trusted(s, tj); }
/* modo plano (src/agent/planmode.c): uma ferramenta MCP so roda se for somente leitura (dica) de um servidor do usuario */
static int mcpx_plan_ok(const char *name) {
    int tj = -1;
    if (!strstr(name, "__")) return 0;
    mcp_init(0);        /* fora do laco do agente (katana tool, toolrun) os servidores ainda nao foram ligados */
    McpSrv *s = mcp_resolve(name, &tj);
    return s && tj >= 0 && s->alive && mcpx_readonly_trusted(s, tj);
}

/* ----- e: .mcp.json na raiz do projeto (mesmo formato; vale a mesma confirmacao de confianca por caminho e conteudo) -----
 * Procura da pasta de trabalho para cima ate a raiz do repositorio (pasta com .git) ou a pasta do usuario.
 * Precedencia, do mais fraco ao mais forte: arquivo do usuario, .mcp.json, .katana/mcp.json. */
static int mcpx_root_file(char *out, size_t cap) {
    { const char *st1 = getenv("KATANA_MCP_STRICT"); if (st1 && *st1 && *st1 != '0') return 0; }
    char d[4200], home[1024], g[4400]; struct stat st;
    if (!getcwd(d, sizeof d)) return 0;
    agent_home(home, sizeof home);
    for (int up = 0; up < 12; up++) {
        snprintf(out, cap, "%s/.mcp.json", d);
        if (proj_isfile(out)) return 1;
        snprintf(g, sizeof g, "%s/.git", d);
        if (stat(g, &st) == 0 || strcmp(d, home) == 0) break;
        char *sl = strrchr(d, '/');
        if (!sl || sl == d) break;
        *sl = 0;
    }
    return 0;
}

/* ----- f: elicitation (o servidor pede dados ao usuario no meio de uma chamada; modo "form" da revisao 2025-11-25) -----
 * Quem pergunta aparece na pergunta. A interface (KATANA_UI) recebe o marcador CHOICE de ask_user (resposta "\x01ANS texto");
 * no terminal a pergunta sai no teclado; sem ninguem para responder (modo sem tela, pipe) a resposta e "decline".
 * Campos que parecem senha ou segredo so seguem se o usuario confirmar. Os valores digitados nunca vao para log. */
typedef struct {
    char key[64], title[120], desc[200], type[16], def[120];
    int req, hasdef, nopts, multi, hasmn, hasmx;
    char *opts[12];
    double mn, mx;
} McpxField;
static void mcpx_flat(char *s) { for (; *s; s++) if (*s == '\n' || *s == '\r' || *s == '\t' || *s == 1) *s = ' '; }
static void mcpx_field_free(McpxField *f) { for (int i = 0; i < f->nopts; i++) free(f->opts[i]); f->nopts = 0; }
static int mcpx_sensitive(const McpxField *f) {
    static const char *const w[] = {"password", "passwd", "passphrase", "secret", "token", "api key", "api_key", "apikey", "api-key", "credential",
        "private key", "private_key", "senha", "cvv", "pin code", "otp", "2fa", NULL};
    char h[500];
    snprintf(h, sizeof h, "%s %s %s", f->key, f->title, f->desc);
    for (char *c = h; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (int i = 0; w[i]; i++) if (strstr(h, w[i])) return 1;
    return 0;
}
/* 1 = campo reconhecido; 0 = tipo que nao tratamos (objeto, lista de objetos) */
static int mcpx_field_parse(const char *key, const char *v, int required, McpxField *f) {
    memset(f, 0, sizeof *f);
    snprintf(f->key, sizeof f->key, "%s", key);
    f->req = required;
    if (*v != '{') return 0;
    char *t = mcp_dupstr(mcp_get(v, "type")), *ti = mcp_dupstr(mcp_get(v, "title")), *de = mcp_dupstr(mcp_get(v, "description"));
    snprintf(f->type, sizeof f->type, "%s", t ? t : "string");
    snprintf(f->title, sizeof f->title, "%s", ti ? ti : "");
    snprintf(f->desc, sizeof f->desc, "%s", de ? de : "");
    free(t); free(ti); free(de);
    mcpx_flat(f->title); mcpx_flat(f->desc);
    const char *en = mcp_get(v, "enum"), *one = mcp_get(v, "oneOf"), *any = NULL;
    if (!strcmp(f->type, "array")) {
        const char *it = mcp_get(v, "items");
        if (!it || *it != '{') return 0;
        en = mcp_get(it, "enum"); one = NULL; any = mcp_get(it, "anyOf");
        if (!en && !any) return 0;
        f->multi = 1;
    } else if (strcmp(f->type, "string") && strcmp(f->type, "number") && strcmp(f->type, "integer") && strcmp(f->type, "boolean")) return 0;
    const char *list = en ? en : (one ? one : any), *e, *np;
    if (list && *list == '[') for (const char *p = list + 1; (np = mcp_elem(p, &e)) != NULL && f->nopts < 12; p = np) {
        char *o = en ? mcp_dupstr(e) : mcp_dupstr(*e == '{' ? mcp_get(e, "const") : NULL);
        if (o) f->opts[f->nopts++] = o;
    }
    const char *d = mcp_get(v, "default");
    if (d && *d) { f->hasdef = 1; if (*d == '"') mcp_dec(d, f->def, sizeof f->def); else { size_t n = (size_t)(mcp_skip(d) - d); if (n >= sizeof f->def) n = sizeof f->def - 1; memcpy(f->def, d, n); f->def[n] = 0; } }
    const char *mn = mcp_get(v, "minimum"), *mx = mcp_get(v, "maximum");
    if (mn) { f->hasmn = 1; f->mn = atof(mn); }
    if (mx) { f->hasmx = 1; f->mx = atof(mx); }
    return 1;
}
/* pergunta ao usuario: 1 = resposta em ans (pode ser vazia); 0 = cancelou (Stop, fim da entrada); -1 = ninguem para responder */
static int mcpx_prompt(const char *q, char **opts, int nopts, char *ans, size_t cap) {
    char line[1200];
    ans[0] = 0;
    if (getenv("KATANA_UI")) {
        char qq[800]; snprintf(qq, sizeof qq, "%s", q); mcpx_flat(qq);
        printf("\x01" "CHOICE %s", qq);
        for (int i = 0; i < nopts; i++) { char o[200]; snprintf(o, sizeof o, "%s", opts[i]); mcpx_flat(o); printf("\t%s", o); }
        printf("\n"); fflush(stdout);
        if (!read_line(line, sizeof line)) return 0;
        if (strncmp(line, "\x01" "ANS ", 5) == 0) snprintf(ans, cap, "%s", line + 5);
        else if (strncmp(line, "\x01" "ANS", 4) == 0) return 0;
        else return 0;
        return 1;
    }
    if (!K_ISATTY(0)) return -1;
    fprintf(stderr, "\n[katana asks] %s\n", q);
    for (int i = 0; i < nopts; i++) fprintf(stderr, "  %d) %s\n", i + 1, opts[i]);
    fprintf(stderr, nopts ? "[number or your own answer] > " : "> ");
    fflush(stderr);
    if (!fgets(line, sizeof line, stdin)) return 0;
    line[strcspn(line, "\r\n")] = 0;
    const int k = atoi(line);
    if (nopts && k >= 1 && k <= nopts && strlen(line) <= 2) snprintf(ans, cap, "%s", opts[k - 1]); else snprintf(ans, cap, "%s", line);
    return 1;
}
static int mcpx_in_opts(const McpxField *f, const char *v) { for (int i = 0; i < f->nopts; i++) if (!strcmp(f->opts[i], v)) return 1; return 0; }
/* valida a resposta e a escreve como valor JSON em out; 0 se invalida */
static int mcpx_value(const McpxField *f, const char *a, McpB *out) {
    while (*a == ' ') a++;
    if (!strcmp(f->type, "boolean")) {
        if (!strcasecmp(a, "yes") || !strcasecmp(a, "true") || !strcasecmp(a, "y") || !strcasecmp(a, "s") || !strcasecmp(a, "sim")) { mcp_bputs(out, "true"); return 1; }
        if (!strcasecmp(a, "no") || !strcasecmp(a, "false") || !strcasecmp(a, "n") || !strcasecmp(a, "nao")) { mcp_bputs(out, "false"); return 1; }
        return 0;
    }
    if (!strcmp(f->type, "number") || !strcmp(f->type, "integer")) {
        char *e = NULL; const double d = strtod(a, &e);
        if (!*a || (e && *e)) return 0;
        if (!strcmp(f->type, "integer") && d != (double)(long long)d) return 0;
        if ((f->hasmn && d < f->mn) || (f->hasmx && d > f->mx)) return 0;
        mcp_bputs(out, a);
        return 1;
    }
    if (f->multi) {
        char buf[1200]; snprintf(buf, sizeof buf, "%s", a);
        mcp_bputs(out, "[");
        int n = 0;
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            size_t l = strlen(t); while (l && t[l - 1] == ' ') t[--l] = 0;
            if (!l) continue;
            if (!mcpx_in_opts(f, t)) return 0;
            if (n++) mcp_bputs(out, ",");
            mcp_besc(out, t);
        }
        mcp_bputs(out, "]");
        return n > 0;
    }
    if (f->nopts && !mcpx_in_opts(f, a)) return 0;
    mcp_besc(out, a);
    return 1;
}
/* devolve 1 com o resultado em res; 0 se o pedido e de um tipo que nao tratamos (o servidor recebe erro) */
static int mcpx_elicit(McpSrv *s, const char *params, McpB *res) {
    char *mode = mcp_dupstr(params ? mcp_get(params, "mode") : NULL);
    const int notform = mode && strcmp(mode, "form") != 0;
    free(mode);
    if (notform) return 0;                                  /* so declaramos o modo form */
    char *msg = mcp_dupstr(params ? mcp_get(params, "message") : NULL);
    const char *sc = params ? mcp_get(params, "requestedSchema") : NULL, *pr = sc ? mcp_get(sc, "properties") : NULL, *rq = sc ? mcp_get(sc, "required") : NULL;
    if (!pr || *pr != '{') { free(msg); return 0; }
    McpxField f[10]; int nf = 0, bad = 0;
    char k[64]; const char *v, *np;
    for (const char *p = pr + 1; (np = mcp_member(p, k, sizeof k, &v)) != NULL; p = np) {
        if (nf >= 10) { bad = 1; break; }
        int req = 0;
        if (rq && *rq == '[') { const char *e, *en; for (const char *z = rq + 1; (en = mcp_elem(z, &e)) != NULL; z = en) { char r[64]; mcp_dec(e, r, sizeof r); if (!strcmp(r, k)) req = 1; } }
        if (!mcpx_field_parse(k, v, req, &f[nf])) { mcpx_field_free(&f[nf]); bad = 1; break; }
        nf++;
    }
    int ok = !bad && nf > 0;
    if (!ok) { for (int i = 0; i < nf; i++) mcpx_field_free(&f[i]); free(msg); return 0; }
    char m[500]; snprintf(m, sizeof m, "%s", msg ? msg : "(no message)"); free(msg); mcpx_flat(m);
    const char *action = "decline";
    McpB content = {0}; mcp_bput(&content, "", 0);
    char names[300] = ""; for (int i = 0; i < nf; i++) { const size_t l = strlen(names); snprintf(names + l, sizeof names - l, "%s%s", i ? ", " : "", f[i].key); }
    char q[1200], ans[1200];
    char *gate[2] = {(char *)"Answer", (char *)"Decline"};
    snprintf(q, sizeof q, "MCP server '%s' asks you for information: %s (fields: %s). It is the server asking, not the model.", s->name, m, names);
    if (!getenv("KATANA_UI") && K_ISATTY(0)) fprintf(stderr, "%s[mcp %s is asking you for information]%s\n", CE(C_DIM), s->name, CE(C_RESET));
    const int g = mcpx_prompt(q, gate, 2, ans, sizeof ans);
    if (g < 0) { fprintf(stderr, "%s[mcp %s asked you for information (%s); declined: nobody to ask in this run]%s\n", CE(C_DIM), s->name, names, CE(C_RESET)); action = "decline"; }
    else if (g == 0) action = "cancel";
    else if (!strncasecmp(ans, "decline", 7) || !strcmp(ans, "2") || !strncasecmp(ans, "no", 2)) action = "decline";
    else {
        int stop = 0;
        for (int i = 0; i < nf && !stop; i++) {            /* campos que parecem segredo: confirmacao explicita */
            if (!mcpx_sensitive(&f[i])) continue;
            char what[300], det[400];
            snprintf(what, sizeof what, "send what looks like a secret ('%s') to the MCP server '%s'", f[i].key, s->name);
            snprintf(det, sizeof det, "the server asks for the field '%.60s' (%.120s); the value would be typed in this window and sent to the server program", f[i].key, f[i].title[0] ? f[i].title : f[i].desc);
            if (!agent_ask_kind("action", what, det)) { action = "decline"; stop = 1; }
        }
        if (!stop) {
            mcp_bputs(&content, "{");
            int n = 0; action = "accept";
            for (int i = 0; i < nf && !stop; i++) {
                McpxField *fl = &f[i];
                char label[600], hint[120] = "";
                if (!strcmp(fl->type, "boolean")) snprintf(hint, sizeof hint, "yes or no");
                else if (fl->multi) snprintf(hint, sizeof hint, "comma-separated list");
                else if (!strcmp(fl->type, "number") || !strcmp(fl->type, "integer")) snprintf(hint, sizeof hint, "%s%s", fl->type, fl->hasmn || fl->hasmx ? " in range" : "");
                snprintf(label, sizeof label, "[mcp %s] %s%s%s%s%s%s%s%s", s->name, fl->title[0] ? fl->title : fl->key, fl->req ? " (required)" : " (optional, empty skips)",
                         hint[0] ? " - " : "", hint, fl->desc[0] ? ": " : "", fl->desc, fl->hasdef ? " [default: " : "", fl->hasdef ? fl->def : "");
                if (fl->hasdef) { const size_t l = strlen(label); snprintf(label + l, sizeof label - l, "]"); }
                char *yn[2] = {(char *)"yes", (char *)"no"};
                char **opts = fl->nopts ? fl->opts : (!strcmp(fl->type, "boolean") ? yn : NULL);
                const int nopts = fl->nopts ? fl->nopts : (!strcmp(fl->type, "boolean") ? 2 : 0);
                int done = 0;
                for (int tries = 0; tries < 3 && !done && !stop; tries++) {
                    const int r = mcpx_prompt(label, opts, nopts, ans, sizeof ans);
                    if (r <= 0) { action = "cancel"; stop = 1; break; }
                    if (!ans[0]) {
                        if (fl->hasdef) snprintf(ans, sizeof ans, "%s", fl->def);
                        else if (!fl->req) { done = 1; break; }
                        else { action = "cancel"; stop = 1; break; }
                    }
                    McpB val = {0}; mcp_bput(&val, "", 0);
                    if (mcpx_value(fl, ans, &val)) {
                        if (n++) mcp_bputs(&content, ",");
                        mcp_besc(&content, fl->key); mcp_bputs(&content, ":"); mcp_bputs(&content, val.p);
                        done = 1;
                    } else if (tries == 2) { action = "cancel"; stop = 1; }
                    else fprintf(stderr, "%s[that value is not valid for '%s'; try again]%s\n", CE(C_DIM), fl->key, CE(C_RESET));
                    free(val.p);
                }
            }
            mcp_bputs(&content, "}");
        }
    }
    for (int i = 0; i < nf; i++) mcpx_field_free(&f[i]);
    mcp_bputs(res, "{\"action\":\"");
    mcp_bputs(res, action);
    mcp_bputs(res, "\"");
    if (!strcmp(action, "accept")) { mcp_bputs(res, ",\"content\":"); mcp_bputs(res, content.p); }
    mcp_bputs(res, "}");
    free(content.p);
    mcp_log(s->name, "elicitation", "", 0, action);       /* so a decisao; nunca os valores */
    return 1;
}
