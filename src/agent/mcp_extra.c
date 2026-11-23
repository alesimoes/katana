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
    if (s->sse) mcpx_legacy_post(s, r.p);
    else if (s->http) { char *x = mcp_http_post(s, r.p, 0, 10000); free(x); }       /* 202 sem corpo */
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
static int mcpx_legacy_post(McpSrv *s, const char *body);                   /* abaixo (g) */
/* ----- mensagem do servidor (pedido ou aviso) chegando por stdio ou pelo fluxo SSE ----- */
static void mcpx_server_msg(McpSrv *s, const char *o) {
    const char *iv = mcp_get(o, "id"), *mv = mcp_get(o, "method");
    char m[120] = "";
    if (mv && *mv == '"') mcp_dec(mv, m, sizeof m);
    if (mv && mcpch_note(s, o, mv)) return;        /* mensagem ou resposta de confirmacao do canal (mcp_channel.c) */
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
 * Quem pergunta aparece na pergunta. A interface (KATANA_UI) recebe o marcador CHOICE de AskUserQuestion (resposta "\x01ANS texto");
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

/* ----- g: fluxos SSE -----
 * 1) o fluxo GET do HTTP "streamable": depois do initialize, um GET com Accept: text/event-stream traz avisos e pedidos
 *    do servidor fora de qualquer chamada. Sessao (Mcp-Session-Id), MCP-Protocol-Version e, ao reconectar,
 *    Last-Event-ID (reenvio do que se perdeu). O servidor pode responder 405 (nao tem fluxo): tudo segue sem ele.
 * 2) o transporte SSE antigo ("type":"sse"): um GET que fica aberto, o primeiro evento "endpoint" diz para onde enviar as
 *    mensagens (POST) e as respostas voltam como eventos "message" no mesmo fluxo.
 * O fluxo e um curl filho com a saida num pipe (nao bloqueante): mcpx_stream_fd() devolve esse descritor para quem
 * quiser esperar com poll() (a escuta ociosa do canal) e mcpx_stream_pump() le e trata o que chegou.
 * Sem Windows: la as funcoes ficam como "sem fluxo". */
/* proximo evento completo de buf (a partir de *done): nome, id e dados; 0 se ainda falta */
static int mcpx_ev_next(McpB *b, size_t *done, int final, char *evname, size_t ncap, char *evid, size_t icap, McpB *data) {
    for (;;) {
        const char *st = b->p + *done, *e = NULL;
        for (const char *q = st; *q;) {
            const size_t ll = strcspn(q, "\n");
            if (!q[ll]) break;
            if (ll == 0 || (ll == 1 && *q == '\r')) { e = q; break; }
            q += ll + 1;
        }
        size_t skip;
        if (e) skip = (size_t)(e + strcspn(e, "\n") + 1 - st);
        else if (final && *st) { e = st + strlen(st); skip = strlen(st); }
        else return 0;
        evname[0] = 0; evid[0] = 0; data->n = 0; mcp_bput(data, "", 0); data->p[0] = 0;
        int any = 0;
        for (const char *q = st; q < e;) {
            size_t ll = strcspn(q, "\n"); if (q + ll > e) ll = (size_t)(e - q);
            size_t cl = ll; if (cl && q[cl - 1] == '\r') cl--;
            const char *v = NULL; size_t vl = 0;
            if (cl >= 5 && !strncmp(q, "data:", 5)) { v = q + 5; vl = cl - 5; if (vl && *v == ' ') { v++; vl--; } if (data->n) mcp_bputs(data, "\n"); mcp_bput(data, v, vl); any = 1; }
            else if (cl >= 6 && !strncmp(q, "event:", 6)) { v = q + 6; vl = cl - 6; if (vl && *v == ' ') { v++; vl--; } snprintf(evname, ncap, "%.*s", (int)vl, v); any = 1; }
            else if (cl >= 3 && !strncmp(q, "id:", 3)) { v = q + 3; vl = cl - 3; if (vl && *v == ' ') { v++; vl--; } snprintf(evid, icap, "%.*s", (int)vl, v); any = 1; }
            q += ll; if (q < e) q++;
        }
        *done += skip;
        if (any) return 1;
    }
}
/* quem leva a resposta do id num texto de evento; dispara pedidos e avisos do servidor */
static void mcpx_ev_dispatch(McpSrv *s, const char *data, long want, char **resp) {
    const char *js = strchr(data, '{');
    if (!js) return;
    if (mcp_get(js, "method")) mcpx_server_msg(s, js);
    else if (want && resp && !*resp) { const char *iv = mcp_get(js, "id"); if (iv && atol(iv) == want && (mcp_get(js, "result") || mcp_get(js, "error"))) *resp = strdup(js); }
}
static void mcpx_sse_scan(McpSrv *s, McpB *o, size_t *done, long id, char **resp, int final) {
    char en[60], ei[128]; McpB d = {0};
    mcp_bput(&d, "", 0);
    while (mcpx_ev_next(o, done, final, en, sizeof en, ei, sizeof ei, &d)) { if (ei[0]) snprintf(s->pleid, sizeof s->pleid, "%s", ei); mcpx_ev_dispatch(s, d.p, id, resp); }
    free(d.p);
}
#ifndef _WIN32
typedef struct McpxStream {
    int fd; pid_t pid; char hdrf[300]; McpB buf; size_t done;
    int kind;                       /* 1 = GET do streamable, 2 = SSE antigo, 3 = reenvio de um POST (temporario) */
    int fails, dead, got_endpoint;
    char leid[128], endpoint[1200];
} McpxStream;
static void mcpx_stream_kill(McpxStream *g) {
    if (g->fd >= 0) close(g->fd);
    g->fd = -1;
    if (g->pid > 0) { int st; kill(g->pid, SIGTERM); waitpid(g->pid, &st, 0); g->pid = 0; }
    if (g->hdrf[0]) { remove(g->hdrf); g->hdrf[0] = 0; }
}
/* abre o GET (curl filho). Devolve o codigo HTTP (200 = fluxo aberto), 0 se nao chegou resposta, -1 se nao deu para iniciar */
static int mcpx_stream_open(McpSrv *s, McpxStream *g, const char *url, const char *leid) {
    int pf[2];
    if (!mcp_tmp(g->hdrf, sizeof g->hdrf)) return -1;
    if (pipe(pf) != 0) return -1;
    char *av[40]; int n = 0; char sh[300], ph[80], lh[200];
    av[n++] = (char *)"curl"; av[n++] = (char *)"-sSN"; av[n++] = (char *)"-D"; av[n++] = g->hdrf;
    av[n++] = (char *)"-H"; av[n++] = (char *)"Accept: text/event-stream";
    if (s->session[0] && g->kind != 2) { snprintf(sh, sizeof sh, "Mcp-Session-Id: %s", s->session); av[n++] = (char *)"-H"; av[n++] = sh; }
    if (s->proto[0] && g->kind != 2) { snprintf(ph, sizeof ph, "MCP-Protocol-Version: %s", s->proto); av[n++] = (char *)"-H"; av[n++] = ph; }
    if (leid && *leid) { snprintf(lh, sizeof lh, "Last-Event-ID: %s", leid); av[n++] = (char *)"-H"; av[n++] = lh; }
    for (int i = 0; i < s->nhdr && n < 34; i++) { av[n++] = (char *)"-H"; av[n++] = s->hdr[i]; }
    av[n++] = (char *)url; av[n] = NULL;
    signal(SIGPIPE, SIG_IGN);
    const pid_t pid = fork();
    if (pid < 0) { close(pf[0]); close(pf[1]); return -1; }
    if (pid == 0) {
        dup2(pf[1], 1); close(pf[0]); close(pf[1]);
        int dn = open("/dev/null", O_WRONLY); if (dn >= 0) dup2(dn, 2);
        execvp("curl", av);
        _exit(127);
    }
    close(pf[1]);
    fcntl(pf[0], F_SETFD, FD_CLOEXEC); fcntl(pf[0], F_SETFL, fcntl(pf[0], F_GETFL) | O_NONBLOCK);
    g->fd = pf[0]; g->pid = pid;
    /* espera os cabecalhos (ate 5 s): o curl os grava assim que chegam, mesmo sem corpo */
    int status = 0, sse = 0;
    for (int i = 0; i < 100 && !status; i++) {
        char *hh = mcp_slurp(g->hdrf, NULL);
        if (hh) { const char *h = strstr(hh, "HTTP/"); if (h && strstr(h, "\r\n\r\n")) { const char *sp = strchr(h, ' '); status = sp ? atoi(sp) : 0; sse = mcp_has_ci(h, "text/event-stream"); } free(hh); }
        if (status) break;
        int st; if (waitpid(pid, &st, WNOHANG) == pid) { g->pid = 0; break; }
        usleep(50000);
    }
    if (status == 200 && sse) return 200;
    mcpx_stream_kill(g);
    return status;
}
/* le o que ha no pipe (nao bloqueante); 0 = fim do fluxo */
static int mcpx_stream_read(McpxStream *g) {
    char t[4096];
    for (;;) {
        const ssize_t r = read(g->fd, t, sizeof t);
        if (r > 0) { mcp_bput(&g->buf, t, (size_t)r); if (g->buf.n > (size_t)8 << 20) return 0; continue; }
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        return 1;                                     /* EAGAIN: por ora acabou o que havia */
    }
}
/* espera ate ms por dados, trata os eventos. want>0: devolve em *resp a resposta desse id (SSE antigo e reenvio). Devolve
   o numero de eventos tratados; -1 quando o fluxo acabou */
static int mcpx_pump(McpSrv *s, McpxStream *g, int ms, long want, char **resp) {
    if (g->fd < 0) return -1;
    const double dl = now_sec() + ms / 1000.0;
    int handled = 0;
    for (;;) {
        char en[60], ei[128]; McpB d = {0};
        mcp_bput(&d, "", 0);
        while (mcpx_ev_next(&g->buf, &g->done, 0, en, sizeof en, ei, sizeof ei, &d)) {
            handled++;
            if (ei[0]) snprintf(g->leid, sizeof g->leid, "%s", ei);
            if (g->kind == 2 && !strcmp(en, "endpoint")) { snprintf(g->endpoint, sizeof g->endpoint, "%.1190s", d.p); g->got_endpoint = 1; continue; }
            mcpx_ev_dispatch(s, d.p, want, resp);
        }
        free(d.p);
        if (g->done > 0 && g->done == g->buf.n) { g->buf.n = 0; g->buf.p[0] = 0; g->done = 0; }
        if ((want && resp && *resp) || (g->kind == 2 && !want && g->got_endpoint && ms > 0 && handled)) return handled;
        const double rem = dl - now_sec();
        struct pollfd pf = { g->fd, POLLIN, 0 };
        const int pr = poll(&pf, 1, rem > 0.2 ? 200 : (rem > 0 ? (int)(rem * 1000) + 1 : 0));
        if (pr < 0 && errno != EINTR) return -1;
        if (pr > 0) {
            if (mcpx_stream_read(g)) continue;              /* chegou mais: trata */
            char en2[60], ei2[128]; McpB d2 = {0};         /* fim do fluxo: trata o que sobrou e avisa */
            mcp_bput(&d2, "", 0);
            while (mcpx_ev_next(&g->buf, &g->done, 1, en2, sizeof en2, ei2, sizeof ei2, &d2)) { if (ei2[0]) snprintf(g->leid, sizeof g->leid, "%s", ei2); mcpx_ev_dispatch(s, d2.p, want, resp); handled++; }
            free(d2.p);
            return (want && resp && *resp) ? handled : -1;
        }
        if (rem <= 0) return handled;
    }
}
/* ---- fluxo GET do streamable HTTP ---- */
static McpxStream *mcpx_gs(McpSrv *s) { return (McpxStream *)s->gs; }
static int mcpx_get_open(McpSrv *s) {
    if (getenv("KATANA_MCP_NO_STREAM")) return 0;
    McpxStream *g = calloc(1, sizeof *g);
    g->fd = -1; g->kind = 1; mcp_bput(&g->buf, "", 0);
    const int gst = mcpx_stream_open(s, g, s->url, NULL);
    if (getenv("KATANA_MCP_DEBUG")) fprintf(stderr, "[mcp %s: GET stream status %d]\n", s->name, gst);
    if (gst != 200) { free(g->buf.p); free(g); return 0; }       /* 405 ou outro: o servidor nao tem fluxo */
    s->gs = g;
    return 1;
}
/* descritor do fluxo, para quem espera com poll(); -1 se nao ha */
static int mcpx_stream_fd(const McpSrv *s) { const McpxStream *g = (const McpxStream *)s->gs; return g && !g->dead ? g->fd : -1; }
/* trata o que chegou no fluxo (espera ate ms); reconecta com Last-Event-ID se caiu. Devolve eventos tratados */
static int mcpx_stream_pump(McpSrv *s, int ms) {
    McpxStream *g = mcpx_gs(s);
    if (!g || g->dead) return 0;
    const int n = mcpx_pump(s, g, ms, 0, NULL);
    if (n >= 0) return n;
    if (g->kind == 2) { g->dead = 1; s->alive = 0; snprintf(s->err, sizeof s->err, "servidor encerrou"); return 0; }     /* SSE antigo nao retoma */
    for (g->fails++; g->fails <= 3; g->fails++) {           /* caiu: retoma de onde parou */
        mcpx_stream_kill(g);
        g->done = 0; g->buf.n = 0; if (g->buf.p) g->buf.p[0] = 0;
        if (mcpx_stream_open(s, g, s->url, g->leid) == 200) { g->fails = 0; return mcpx_pump(s, g, 0, 0, NULL) > 0 ? 1 : 0; }
    }
    g->dead = 1; mcpx_stream_kill(g);
    return 0;
}
/* o POST caiu no meio do fluxo SSE sem a resposta: GET com Last-Event-ID do ultimo evento visto, ate ms */
static int mcpx_resume_post(McpSrv *s, long id, char **resp, int ms) {
    McpxStream g; memset(&g, 0, sizeof g);
    g.fd = -1; g.kind = 3; mcp_bput(&g.buf, "", 0);
    if (mcpx_stream_open(s, &g, s->url, s->pleid) != 200) { free(g.buf.p); return 0; }
    s->pleid[0] = 0;
    mcpx_pump(s, &g, ms, id, resp);
    mcpx_stream_kill(&g); free(g.buf.p);
    return *resp != NULL;
}
static void mcpx_stream_close(McpSrv *s) {
    McpxStream *g = mcpx_gs(s);
    if (!g) return;
    mcpx_stream_kill(g); free(g->buf.p); free(g);
    s->gs = NULL;
}
/* ---- transporte SSE antigo ---- */
static int mcpx_legacy_open(McpSrv *s) {
    McpxStream *g = calloc(1, sizeof *g);
    g->fd = -1; g->kind = 2; mcp_bput(&g->buf, "", 0);
    const int st = mcpx_stream_open(s, g, s->url, NULL);
    if (st != 200) {
        snprintf(s->err, sizeof s->err, st > 0 ? "HTTP %d" : "curl falhou (sem resposta do fluxo SSE)", st);
        free(g->buf.p); free(g); return 0;
    }
    s->gs = g;
    const double dl = now_sec() + 10;
    while (!g->got_endpoint && now_sec() < dl) if (mcpx_pump(s, g, 500, 0, NULL) < 0) break;
    if (!g->got_endpoint) { snprintf(s->err, sizeof s->err, "SSE stream without an endpoint event"); mcpx_stream_close(s); return 0; }
    /* o endpoint tem de ser da mesma origem que a url configurada (nada de mandar mensagens para outro lugar) */
    const char *sc = strstr(s->url, "://"), *pe = sc ? strchr(sc + 3, '/') : NULL;
    const size_t ol = sc ? (pe ? (size_t)(pe - s->url) : strlen(s->url)) : 0;
    char full[1400];
    if (g->endpoint[0] == '/') snprintf(full, sizeof full, "%.*s%s", (int)ol, s->url, g->endpoint);
    else if (!strncmp(g->endpoint, s->url, ol) && ol) snprintf(full, sizeof full, "%s", g->endpoint);
    else { snprintf(s->err, sizeof s->err, "SSE endpoint is not on the same server as the url"); mcpx_stream_close(s); return 0; }
    snprintf(g->endpoint, sizeof g->endpoint, "%.1190s", full);
    return 1;
}
static int mcpx_legacy_post(McpSrv *s, const char *body) {
    McpxStream *g = mcpx_gs(s);
    if (!g || g->dead) { snprintf(s->err, sizeof s->err, "SSE stream is closed"); return 0; }
    char *keep = s->url; s->url = g->endpoint;               /* mcp_http_post usa s->url */
    char *r = mcp_http_post(s, body, 0, 10000);
    s->url = keep;
    if (!r) return 0;
    free(r);
    return 1;
}
static char *mcpx_legacy_request(McpSrv *s, const char *body, long id, int ms) {
    McpxStream *g = mcpx_gs(s);
    if (!mcpx_legacy_post(s, body)) return NULL;
    char *resp = NULL;
    const int n = mcpx_pump(s, g, ms, id, &resp);
    if (!resp) {
        if (n < 0) { g->dead = 1; s->alive = 0; snprintf(s->err, sizeof s->err, "servidor encerrou"); }
        else snprintf(s->err, sizeof s->err, "tempo esgotado");
    }
    return resp;
}
#else
static int mcpx_get_open(McpSrv *s) { (void)s; return 0; }
static int mcpx_stream_fd(const McpSrv *s) { (void)s; return -1; }
static int mcpx_stream_pump(McpSrv *s, int ms) { (void)s; (void)ms; return 0; }
static int mcpx_resume_post(McpSrv *s, long id, char **resp, int ms) { (void)s; (void)id; (void)resp; (void)ms; return 0; }
static void mcpx_stream_close(McpSrv *s) { (void)s; }
static int mcpx_legacy_open(McpSrv *s) { snprintf(s->err, sizeof s->err, "SSE transport needs a POSIX system"); return 0; }
static int mcpx_legacy_post(McpSrv *s, const char *body) { (void)s; (void)body; return 0; }
static char *mcpx_legacy_request(McpSrv *s, const char *body, long id, int ms) { (void)s; (void)body; (void)id; (void)ms; return NULL; }
#endif
static const char *mcpx_kind(const McpSrv *s) { return s->sse ? "sse" : (s->http ? "http" : "stdio"); }
