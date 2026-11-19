/* ---------- despacho: executa a ferramenta pedida pelo modelo, registra no log, neutraliza marcas no resultado ---------- */
/* interface: resultado da ferramenta numa linha (\\ e \n escapados; mantem o comeco e o fim se for grande) */
static void ui_toolout(const char *name, const char *res) {
    if (!getenv("KATANA_UI")) return;
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
static void agent_tool_run(const char *name, const char *js, const char *end, char *out, size_t cap) {
    static char arg[8192], content[65536];
    out[0] = 0;
    if (strcmp(name, "read_file") == 0) {
        tool_read_file(js, end, out, cap);
    } else if (strcmp(name, "list_dir") == 0) {
        if (!json_str(js, end, "path", arg, sizeof arg)) strcpy(arg, ".");
        {
            char res[4200], err[1300];
            if (!agent_read_access(arg, 1, res, sizeof res, err, sizeof err)) { snprintf(out, cap, "%s", err); return; }
            snprintf(arg, sizeof arg, "%s", res);
        }
        size_t n = 0;
#ifdef _WIN32
        char pat[8300]; WIN32_FIND_DATAA fd;
        snprintf(pat, sizeof pat, "%s\\*", arg);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) { snprintf(out, cap, "error: cannot list %s", arg); return; }
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            if (n + strlen(fd.cFileName) + 4 >= TOOL_OUT) { n += snprintf(out + n, cap - n, "[more files omitted]\n"); break; }
            n += snprintf(out + n, cap - n, "%s%s\n", fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "/" : "");
        } while (FindNextFileA(h, &fd));
        FindClose(h);
#else
        (void)n;
        tool_list_dir(arg, out, cap);
        return;
#endif
        if (n == 0 && !out[0]) snprintf(out, cap, "(empty)");
    } else if (strcmp(name, "write_file") == 0) {
        static char wp[4300], werr[400];
        if (!json_str(js, end, "path", arg, sizeof arg)) { snprintf(out, cap, "error: missing path"); return; }
        if (!agent_write_path(arg, wp, sizeof wp, werr, sizeof werr)) { snprintf(out, cap, "%s", werr); return; }
        if (!json_str(js, end, "content", content, sizeof content)) { snprintf(out, cap, "error: missing content"); return; }
        if (strlen(content) + 8 >= sizeof content) { snprintf(out, cap, "error: content too long (limit %d bytes); write it in parts: first call normally, the next ones with append=true", (int)sizeof content - 8); return; }
        const int app = json_has(js, end, "append") && json_bool(js, end, "append");
        const int wsudo = json_has(js, end, "sudo") && json_bool(js, end, "sudo");
        char what[8300];
        snprintf(what, sizeof what, "%s the file %s (%zu bytes)", app ? "append to" : "write", wp, strlen(content));
        if (!agent_act(what, content)) { snprintf(out, cap, "error: the user denied this action"); return; }
        mkdir_parents(wp);
        undo_note(wp);
        FILE *f = fopen(wp, app ? "ab" : "wb");
        if (!f && (errno == EACCES || errno == EPERM || errno == EROFS || errno == ENOENT) && wsudo) {     /* sudo=true: grava como root (modal de senha) */
            char rerr[400];
            if (!root_write_file(wp, content, strlen(content), app, rerr, sizeof rerr)) { snprintf(out, cap, "%s", rerr); return; }
        } else {
            if (!f && (errno == EACCES || errno == EPERM || errno == EROFS || errno == ENOENT)) { snprintf(out, cap, "error: permission denied writing %s (%s). To write it as root call write_file again with sudo=true (the user types the password in a dialog)", wp, strerror(errno)); return; }
            if (!f) { snprintf(out, cap, "error: cannot create %s (%s)", wp, strerror(errno)); return; }
            fwrite(content, 1, strlen(content), f);
            fclose(f);
        }
        snprintf(out, cap, "ok: %s %zu bytes to %s", app ? "appended" : "wrote", strlen(content), wp);
        code_check(wp, app, out, cap);
    } else if (strcmp(name, "run_command") == 0) {
        if (!json_str(js, end, "command", arg, sizeof arg)) { snprintf(out, cap, "error: missing command"); return; }
        if (!agent_act("executar o comando", arg)) { snprintf(out, cap, "error: the user denied this action"); return; }
#ifdef _WIN32
        char wcmd[8300];
        snprintf(wcmd, sizeof wcmd, "%s 2>&1", arg);
        FILE *f = _popen(wcmd, "r");
        if (!f) { snprintf(out, cap, "error: cannot run the command"); return; }
        size_t n = fread(out, 1, TOOL_OUT, f);
        out[n] = 0;
        char sink[512];
        int more = 0;
        while (fread(sink, 1, sizeof sink, f) > 0) more = 1;
        const int rc = _pclose(f);
        snprintf(out + n, cap - n, "%s\n[exit code %d]", more ? "\n[output truncated]" : "", rc);
        cmdlog_add(arg, rc, out);
#else
        tool_run_command(arg, js, end, out, cap);
#endif
    } else if (strcmp(name, "search_files") == 0) {
        static char pat[512];
        if (!json_str(js, end, "pattern", pat, sizeof pat)) { snprintf(out, cap, "error: missing pattern"); return; }
        if (!json_str(js, end, "path", arg, sizeof arg) || !arg[0]) strcpy(arg, ".");
        tool_search_files(pat, arg, out, cap);
    } else if (strcmp(name, "web_search") == 0) {
        if (!g_web) { snprintf(out, cap, "error: internet tools are disabled (KATANA_WEB=0)"); return; }
        if (!json_str(js, end, "query", arg, sizeof arg) || !arg[0]) { snprintf(out, cap, "error: missing query"); return; }
        tool_web_search(arg, out, cap);
    } else if (strcmp(name, "web_fetch") == 0) {
        if (!g_web) { snprintf(out, cap, "error: internet tools are disabled (KATANA_WEB=0)"); return; }
        tool_web_fetch(js, end, out, cap);
    } else if (strcmp(name, "remember") == 0) {
        if (!json_str(js, end, "fact", arg, sizeof arg)) { snprintf(out, cap, "error: missing fact"); return; }
        tool_remember(arg, out, cap);
    } else if (strcmp(name, "recall") == 0) {
        if (!json_str(js, end, "query", arg, sizeof arg)) arg[0] = 0;
        tool_recall(arg, out, cap);
    } else if (strcmp(name, "howto") == 0) {
        if (!json_str(js, end, "topic", arg, sizeof arg)) arg[0] = 0;
        tool_howto(arg, out, cap);
    } else if (strcmp(name, "edit_file") == 0) {
        tool_edit_file(js, end, out, cap);
    } else if (strcmp(name, "glob") == 0) {
        static char pat[512];
        if (!json_str(js, end, "pattern", pat, sizeof pat) || !pat[0]) { snprintf(out, cap, "error: missing pattern"); return; }
        if (!json_str(js, end, "root", arg, sizeof arg) || !arg[0]) strcpy(arg, ".");
        tool_glob(pat, arg, out, cap);
    } else if (strcmp(name, "download_file") == 0) {
        static char dst[4096];
        if (!g_web) { snprintf(out, cap, "error: internet tools are disabled (KATANA_WEB=0)"); return; }
        if (!json_str(js, end, "url", arg, sizeof arg) || !arg[0]) { snprintf(out, cap, "error: missing url"); return; }
        if (!json_str(js, end, "path", dst, sizeof dst) || !dst[0]) { snprintf(out, cap, "error: missing path"); return; }
        tool_download_file(arg, dst, out, cap);
    } else if (strcmp(name, "install_package") == 0) {
        static char pk[512];
        if (!g_web) { snprintf(out, cap, "error: internet tools are disabled (KATANA_WEB=0)"); return; }
        if (!json_str(js, end, "manager", arg, sizeof arg) || !arg[0]) { snprintf(out, cap, "error: missing manager (pip, npm, apt, cargo)"); return; }
        if (!json_str(js, end, "name", pk, sizeof pk) || !pk[0]) { snprintf(out, cap, "error: missing name"); return; }
        tool_install_package(arg, pk, out, cap);
    } else if (strcmp(name, "git") == 0) {
        if (!json_str(js, end, "args", arg, sizeof arg) || !arg[0]) { snprintf(out, cap, "error: missing args (for example: status)"); return; }
        tool_git(arg, out, cap);
    } else if (strcmp(name, "read_document") == 0) {
        if (!json_str(js, end, "path", arg, sizeof arg) || !arg[0]) { snprintf(out, cap, "error: missing path"); return; }
        tool_read_document(arg, out, cap);
#ifndef _WIN32
    } else if (strcmp(name, "find_file") == 0) {
        tool_find_file(js, end, out, cap);
#endif
    } else if (strcmp(name, "todo") == 0) {
        tool_todo(js, end, out, cap);
    } else if (strcmp(name, "ask_user") == 0) {
        tool_ask_user(js, end, out, cap);
    } else if (strcmp(name, "terminal") == 0) {
        tool_terminal(js, end, out, cap);
    } else if (strcmp(name, "read_log") == 0) {
        tool_read_log(js, end, out, cap);
    } else if (strcmp(name, "terminal_screen") == 0) {
        tool_terminal_screen(js, end, out, cap);
    } else if (mcp_owns(name)) {
        mcp_call(name, js, out, cap);
    } else snprintf(out, cap, "error: unknown tool %s", name);
}

/* executa a ferramenta e registra data, ferramenta, argumentos e resultado (ok, erro ou codigo de saida)
 * em ~/.katana/agente.log (KATANA_LOG muda o caminho) */
static void agent_tool(const char *name, const char *js, const char *end, char *out, size_t cap) {
    agent_tool_run(name, js, end, out, cap);
    char a[8192];
    const size_t l = (size_t)(end - js) < sizeof a - 1 ? (size_t)(end - js) : sizeof a - 1;
    memcpy(a, js, l); a[l] = 0;
    agent_log(name, a, out);
}

/* copia o resultado de uma ferramenta neutralizando marcas especiais do formato de conversa, para que
 * conteudo externo nao consiga fechar o turno nem forjar chamadas de ferramenta */
static size_t defang(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (const char *p = src; *p && o + 4 < cap; p++) {
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

