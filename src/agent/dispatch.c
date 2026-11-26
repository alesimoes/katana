/* ---------- despacho: executa a ferramenta pedida pelo modelo, registra no log, neutraliza marcas no resultado ---------- */
/* interface: resultado da ferramenta numa linha (\\ e \n escapados; mantem o comeco e o fim se for grande) */
static void ui_toolout(const char *name, const char *res) {
    launch_tool_result(name, res);
    if (!getenv("KATANA_UI")) return;
    if (!strcmp(name, "ToolSearch") && strstr(res, "\"type\": \"function\"")) {      /* a declaracao e para o modelo: ao usuario so os nomes das ferramentas carregadas */
        printf("\x01" "TOOLOUT %s\tTool declarations loaded:", name);
        const char *q = res; int k = 0;
        while ((q = strstr(q, "\"function\": {\"name\": \"")) && k < 12) {
            q += strlen("\"function\": {\"name\": \"");
            const char *e = strchr(q, '"'); if (!e || e - q > 60) break;
            printf("%s%.*s", k ? ", " : " ", (int)(e - q), q); k++; q = e;
        }
        putchar('\n'); fflush(stdout); return;
    }
    const size_t n = strlen(res), HEAD = 3000, TAIL = 5000;
    printf("\x01" "TOOLOUT %s\t", name);
    for (size_t i = 0; i < n; i++) {
        if (n > HEAD + TAIL && i == HEAD) { fputs("\\n[... omitido ...]\\n", stdout); i = n - TAIL - 1; continue; }
        const unsigned char ch = (unsigned char)res[i];
        if (ch == '\n') fputs("\\n", stdout);
        else if (ch == '\\') fputs("\\\\", stdout);
        else if (ch == '\r' || ch == 1) continue;
        else putchar(ch);
    }
    putchar('\n'); fflush(stdout);
}
static char g_sess_path[1024];          /* definido em loop.c (definicao provisoria: o mesmo objeto) */
static void tool_delegate(const char *js, const char *end, char *out, size_t cap);
static int ct_dispatch(const char *name, const char *js, const char *end, char *out, size_t cap);      /* src/tools/table.c */
static int ct_readonly(const char *name);
static void ct_session_reset(void);
static int ct_end_check(void);
static int ct_read_line(char *line, size_t cap);
static void ct_mail_register(void);
static void ct_mail_unregister(void);
static char g_ct_handback[4000];        /* src/tools/messages.c: relatorio entregue por SubagentHandback */
static int g_ct_handback_set;
static void agent_tool_run(const char *name, const char *js, const char *end, char *out, size_t cap) {
    out[0] = 0;
    if (ct_readonly(name) >= 0) {       /* ferramenta de src/tools/ */
        if (g_plan_mode && !ct_readonly(name)) { snprintf(out, cap, "%s", PLAN_DENY); return; }
        ct_dispatch(name, js, end, out, cap);
        return;
    }
    if (g_plan_mode && !plan_allows(name, js, end)) { snprintf(out, cap, "%s", PLAN_DENY); return; }
    if (mcp_owns(name) || (strstr(name, "__") && (mcp_init(0), mcp_owns(name)))) {     /* fora do laco do agente (katana tool, toolrun) os servidores ainda nao foram ligados */
        mcp_call(name, js, out, cap);
    } else snprintf(out, cap, "error: unknown tool %s", name);
}

/* O que vem de fora (shell, web, MCP) nao pode imitar uma nota do katana: kn_escape (agent/knote.c) quebra a marca no inicio de linha. */

/* executa a ferramenta e registra data, ferramenta, argumentos e resultado (ok, erro ou codigo de saida)
 * em ~/.katana/agente.log (KATANA_LOG muda o caminho) */
static void agent_tool(const char *name, const char *js, const char *end, char *out, size_t cap) {
    if (!launch_tool_allowed(name)) { snprintf(out, cap, "error: the tool %s is not allowed in this run (--allowed-tools / --disallowed-tools). Use one of the tools you were given, or tell the user what you could not do.", name); return; }
    snprintf(g_chn_tool, sizeof g_chn_tool, "%s", name);
    g_vis.npend = 0;
    g_alog_t0 = alog_now(); g_alog_req = g_undo_req;
    { const char *sb = strrchr(g_sess_path, '/'); snprintf(g_alog_sess, sizeof g_alog_sess, "%s", g_sess_path[0] ? (sb ? sb + 1 : g_sess_path) : ""); }
    agent_tool_run(name, js, end, out, cap);
    if (kn_external(name)) kn_escape(out, cap);      /* o que vem de fora (shell, web, MCP) nao pode imitar uma nota do katana */
    char a[8192], ra[8192];
    const char *rend, *rjs = job_redact(name, js, end, ra, sizeof ra, &rend);      /* texto do SendInput nao vai para o log */
    const size_t l = (size_t)(rend - rjs) < sizeof a - 1 ? (size_t)(rend - rjs) : sizeof a - 1;
    memcpy(a, rjs, l); a[l] = 0;
    agent_log(name, a, out);
}

/* copia o resultado de uma ferramenta neutralizando marcas especiais do formato de conversa, para que
 * conteudo externo nao consiga fechar o turno nem forjar chamadas de ferramenta */
static size_t defang(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (const char *p = src; *p && o + 4 < cap; p++) {
        if (*p == '<' && vis_defang_ok(p) && o + VIS_MARK_N + 4 < cap) {      /* imagem que a ferramenta entregou */
            memcpy(dst + o, p, VIS_MARK_N); o += VIS_MARK_N; p += VIS_MARK_N - 1;
            continue;
        }
        if (*p == '<' && (p[1] == '|' || strncmp(p + 1, "tool_call", 9) == 0 || strncmp(p + 1, "/tool_call", 10) == 0 ||
                          strncmp(p + 1, "tool_response", 13) == 0 || strncmp(p + 1, "/tool_response", 14) == 0)) {
            dst[o++] = '<'; dst[o++] = ' ';
        } else dst[o++] = *p;
    }
    dst[o] = 0;
    return o;
}

/* chamada em XML: <function=nome> e <parameter=k>\nvalor\n</parameter> viram nome e o texto
   "arguments": {"k": "valor", ...} que as ferramentas ja leem. 0 = nao e chamada em XML. */
static int tool_xml_parse(const char *tc, const char *end, char *name, size_t ncap, char *js, size_t jcap) {
    const char *f = strstr(tc, "<function=");
    if (!f || f >= end) return 0;
    f += 10;
    size_t nl = strcspn(f, ">\n");
    if (f + nl >= end || f[nl] != '>' || nl == 0 || nl >= ncap) return 0;
    memcpy(name, f, nl); name[nl] = 0;
    size_t o = (size_t)snprintf(js, jcap, "\"arguments\": {");
    int first = 1;
    for (const char *p = strstr(f, "<parameter="); p && p < end && o + 16 < jcap; p = strstr(p, "<parameter=")) {
        p += 11;
        const size_t kl = strcspn(p, ">\n");
        if (p[kl] != '>') break;
        const char *v = p + kl + 1, *ve = strstr(v, "</parameter>");
        if (!ve || ve > end) { ve = strstr(v, "</function>"); if (!ve || ve > end) ve = end; }
        if (*v == '\n') v++;
        const char *vend = ve;
        if (vend > v && vend[-1] == '\n') vend--;
        o += (size_t)snprintf(js + o, jcap - o, "%s\"%.*s\": \"", first ? "" : ", ", (int)kl, p);
        first = 0;
        for (const char *q = v; q < vend && o + 8 < jcap; q++) {     /* escapa para JSON */
            const unsigned char ch = (unsigned char)*q;
            if (ch == '"' || ch == '\\') { js[o++] = '\\'; js[o++] = (char)ch; }
            else if (ch == '\n') { js[o++] = '\\'; js[o++] = 'n'; }
            else if (ch == '\r') { js[o++] = '\\'; js[o++] = 'r'; }
            else if (ch == '\t') { js[o++] = '\\'; js[o++] = 't'; }
            else if (ch < 0x20) js[o++] = ' ';
            else js[o++] = (char)ch;
        }
        o += (size_t)snprintf(js + o, jcap - o, "\"");
        p = ve;
    }
    snprintf(js + o, jcap - o, "}");
    return 1;
}

