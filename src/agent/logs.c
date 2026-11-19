#ifndef _WIN32
/* ---- read_log ---- */
static int ci_has(const char *h, const char *n) {
    const size_t nl = strlen(n);
    if (!nl) return 1;
    for (; *h; h++) if (ci_ncmp(h, n, nl) == 0) return 1;
    return 0;
}
/* ultimas `lines` linhas de data que casam com filter (regex estendida sem maiusculas, ou texto); cabecalho `head` */
static void log_emit(const char *data, size_t n, int lines, const char *filter, const char *head, char *out, size_t cap) {
    if (lines < 1) lines = 50;
    if (lines > 500) lines = 500;
    regex_t re; int use_re = 0;
    const int hasf = filter && filter[0];
    if (hasf && regcomp(&re, filter, REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0) use_re = 1;
    size_t *st = (size_t *)malloc((size_t)lines * 2 * sizeof(size_t));
    if (!st) { snprintf(out, cap, "error: out of memory"); if (use_re) regfree(&re); return; }
    long cnt = 0, total = 0;
    char lb[4200];
    for (size_t i = 0; i < n; ) {
        const char *e = memchr(data + i, '\n', n - i);
        size_t l = e ? (size_t)(e - (data + i)) : n - i;
        const size_t next = i + l + (e ? 1 : 0);
        if (l > 0 && data[i + l - 1] == '\r') l--;
        int ok = 1;
        if (hasf) {
            const size_t cl = l < sizeof lb - 1 ? l : sizeof lb - 1;
            memcpy(lb, data + i, cl); lb[cl] = 0;
            ok = use_re ? regexec(&re, lb, 0, NULL, 0) == 0 : ci_has(lb, filter);
        }
        if (ok) { const long k = cnt % lines; st[2 * k] = i; st[2 * k + 1] = l; cnt++; }
        total++;
        i = next;
    }
    if (use_re) regfree(&re);
    size_t L = g_budget < 8000 ? g_budget : 8000;
    if (L < 1500) L = 1500;
    if (L + 400 > cap) L = cap > 800 ? cap - 400 : 400;
    const long have = cnt < lines ? cnt : lines;
    long first = 0;
    size_t sum = 0;                                          /* descarta as linhas mais antigas ate caber (cada linha cortada em 300 bytes) */
    for (long k = have - 1; k >= 0; k--) {
        const long idx = (cnt - have + k) % lines;
        size_t l = st[2 * idx + 1]; if (l > 300) l = 303;
        if (sum + l + 1 > L) { first = k + 1; break; }
        sum += l + 1;
    }
    size_t o = (size_t)snprintf(out, cap, "%s (%ld of %ld lines%s%s)\n", head, have - first, total, hasf ? ", filter: " : "", hasf ? filter : "");
    if (have == 0) o += (size_t)snprintf(out + o, cap - o, "(no matching lines)");
    for (long k = first; k < have && o + 8 < cap; k++) {
        const long idx = (cnt - have + k) % lines;
        size_t l = st[2 * idx + 1]; const char *p = data + st[2 * idx];
        const int cut = l > 300;
        if (cut) { l = 300; while (l > 0 && ((unsigned char)p[l] & 0xC0) == 0x80) l--; }
        o += (size_t)snprintf(out + o, cap - o, "%.*s%s\n", (int)l, p, cut ? "..." : "");
    }
    free(st);
}

static char *read_tail_file(const char *path, size_t maxb, size_t *n, int *err) {
    FILE *f = fopen(path, "rb");
    *err = 0; *n = 0;
    if (!f) { *err = errno; return NULL; }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    const long from = sz > (long)maxb ? sz - (long)maxb : 0;
    fseek(f, from, SEEK_SET);
    char *b = (char *)malloc((size_t)(sz - from) + 1);
    if (!b) { fclose(f); *err = ENOMEM; return NULL; }
    const size_t r = fread(b, 1, (size_t)(sz - from), f);
    fclose(f);
    b[r] = 0;
    size_t skip = 0;
    if (from > 0) { const char *nl = memchr(b, '\n', r); skip = nl ? (size_t)(nl - b) + 1 : 0; }     /* primeira linha pode estar partida */
    memmove(b, b + skip, r - skip + 1);
    *n = r - skip;
    return b;
}

static void tool_read_log(const char *js, const char *end, char *out, size_t cap) {
    static char src[1024], filt[256], unit[96];
    src[0] = filt[0] = unit[0] = 0;
    if (!json_str(js, end, "source", src, sizeof src) || !src[0]) { snprintf(out, cap, "error: missing source (agent, commands, journal, dmesg, syslog or a file path)"); return; }
    json_str(js, end, "filter", filt, sizeof filt);
    json_str(js, end, "unit", unit, sizeof unit);
    int lines = json_int(js, end, "lines", 50);
    if (lines < 1) lines = 50;
    if (lines > 500) lines = 500;
    for (const char *q = unit; *q; q++) if (!isalnum((unsigned char)*q) && !strchr("._@-:", *q)) { snprintf(out, cap, "error: invalid unit name"); return; }
    if (strcmp(src, "commands") == 0) {
        const int have = g_cmdlog_n < CMDLOG_N ? g_cmdlog_n : CMDLOG_N;
        if (!have) { snprintf(out, cap, "(no commands run yet in this session)"); return; }
        Buf b = {0};
        char tb[40], one[1000];
        for (int k = 0; k < have; k++) {
            const int i = (g_cmdlog_n - have + k) % CMDLOG_N;
            strftime(tb, sizeof tb, "%H:%M:%S", localtime(&g_cmdlog[i].t));
            char sm[380], cm[240]; size_t o = 0;
            for (const char *q = g_cmdlog[i].sum; *q && o < sizeof sm - 1; q++) sm[o++] = *q == '\n' ? '|' : *q;
            sm[o] = 0; o = 0;
            for (const char *q = g_cmdlog[i].cmd; *q && o < sizeof cm - 1; q++) cm[o++] = *q == '\n' ? ' ' : *q;
            cm[o] = 0;
            if (g_cmdlog[i].rc < 0) snprintf(one, sizeof one, "%s $ %s  [terminal] -> %s\n", tb, cm, sm);
            else snprintf(one, sizeof one, "%s $ %s  [exit %d] -> %s\n", tb, cm, g_cmdlog[i].rc, sm);
            buf_str(&b, one);
        }
        log_emit(b.p ? b.p : "", b.n, lines, filt, "[commands run by the agent in this session]", out, cap);
        free(b.p);
        return;
    }
    char path[4200]; path[0] = 0;
    char *data = NULL; size_t dn = 0; char head[300];
    int rc = 0;
    if (strcmp(src, "journal") == 0 || strcmp(src, "dmesg") == 0) {
        char cmd[400], uq[200];
        const int fetch = filt[0] ? 3000 : lines;
        const int jd = strcmp(src, "journal") == 0;
        if (jd && unit[0]) { shq(unit, uq, sizeof uq); snprintf(cmd, sizeof cmd, "journalctl -n %d --no-pager -o short-iso -u %s", fetch, uq); }
        else if (jd) snprintf(cmd, sizeof cmd, "journalctl -n %d --no-pager -o short-iso", fetch);
        else snprintf(cmd, sizeof cmd, "dmesg || journalctl -k -n %d --no-pager", fetch);
        data = run_collect(cmd, 20, 512 * 1024, &rc);
        if (!data) { snprintf(out, cap, "error: cannot run %s", jd ? "journalctl" : "dmesg"); return; }
        dn = strlen(data);
        if (rc == 127) { snprintf(out, cap, "error: %s is not available on this machine", jd ? "journalctl" : "dmesg"); free(data); return; }
        const int denied = strstr(data, "insufficient permissions") || strstr(data, "Operation not permitted") || strstr(data, "Permission denied");
        if (rc != 0 && denied && dn < 1500) {
            snprintf(out, cap, "error: permission denied reading %s. Try again after the user joins the group %s (usermod -aG GROUP USER, then log in again), or ask the user to run in their terminal: %s | tail -n %d, and paste the result",
                src, jd ? "systemd-journal or adm" : "adm (or sysctl kernel.dmesg_restrict=0)", jd ? "journalctl --no-pager" : "sudo dmesg", lines);
            free(data); return;
        }
        snprintf(head, sizeof head, "[%s%s%s]", src, unit[0] ? " unit " : "", unit);
    } else {
        if (strcmp(src, "agent") == 0) {
            const char *e = getenv("KATANA_LOG");
            if (e && *e) snprintf(path, sizeof path, "%s", e);
            else { char h[1024]; agent_home(h, sizeof h); snprintf(path, sizeof path, "%s/.katana/agente.log", h); }
        } else if (strcmp(src, "syslog") == 0) {
            snprintf(path, sizeof path, "%s", access("/var/log/syslog", F_OK) == 0 ? "/var/log/syslog" : "/var/log/messages");
        } else {
            char res[4200], er[1300];
            if (!agent_read_access(src, 0, res, sizeof res, er, sizeof er)) { snprintf(out, cap, "%s", er); return; }
            snprintf(path, sizeof path, "%s", res);
        }
        struct stat sb;
        if (stat(path, &sb) != 0) { snprintf(out, cap, "error: %s does not exist", path); return; }
        if (!S_ISREG(sb.st_mode)) { snprintf(out, cap, "error: %s is not a regular file", path); return; }
        int er2;
        data = read_tail_file(path, 1024 * 1024, &dn, &er2);
        if (!data) {
            if (er2 == EACCES) snprintf(out, cap, "error: permission denied reading %s. Try the group adm (usermod -aG adm USER, then log in again), or ask the user to run in their terminal: sudo tail -n %d %s, and paste the result", path, lines, path);
            else snprintf(out, cap, "error: cannot read %s (%s)", path, strerror(er2));
            return;
        }
        if (memchr(data, 0, dn < 4096 ? dn : 4096)) { snprintf(out, cap, "error: %s looks like a binary file; read_log only reads text logs", path); free(data); return; }
        snprintf(head, sizeof head, "[%s]", path);
    }
    log_emit(data, dn, lines, filt, head, out, cap);
    free(data);
}

/* ---- terminal_screen: texto do painel tmux do usuario ---- */
static void tool_terminal_screen(const char *js, const char *end, char *out, size_t cap) {
    static char pane[80];
    pane[0] = 0;
    json_str(js, end, "pane", pane, sizeof pane);
    for (const char *q = pane; *q; q++) if (!isalnum((unsigned char)*q) && !strchr("_.:%@$-", *q)) { snprintf(out, cap, "error: invalid pane"); return; }
    int lines = json_int(js, end, "lines", 60);
    if (lines < 1) lines = 60;
    if (lines > 500) lines = 500;
    const char *tm = getenv("TMUX"), *own = getenv("TMUX_PANE");
    int rc;
    free(run_collect("command -v tmux", 5, 1000, &rc));
    if (rc != 0) { snprintf(out, cap, "error: tmux is not installed. Reading the user's real terminal is only possible inside tmux; ask the user to copy and paste the error here"); return; }
    if (!(tm && *tm) && !pane[0]) { snprintf(out, cap, "error: katana is not running inside tmux, so the user's terminal cannot be read. Ask the user to paste the error here (or to start katana inside tmux)"); return; }
    char cmd[600], qp[200];
    if (!pane[0] && own && *own) {      /* sem pane: outro painel da mesma janela (o proprio painel e o do chat) */
        char q1[200], lc[700];
        shq(own, q1, sizeof q1);
        snprintf(lc, sizeof lc, "tmux list-panes -t %s -F '#{pane_id}'", q1);
        char *lp = run_collect(lc, 5, 4000, &rc);
        if (lp && rc == 0) for (char *t = strtok(lp, "\n"); t; t = strtok(NULL, "\n")) if (strcmp(t, own) != 0) { snprintf(pane, sizeof pane, "%s", t); break; }
        free(lp);
        if (!pane[0]) snprintf(pane, sizeof pane, "%s", own);
    }
    if (pane[0]) { shq(pane, qp, sizeof qp); snprintf(cmd, sizeof cmd, "tmux capture-pane -p -t %s -S -%d", qp, lines); }
    else snprintf(cmd, sizeof cmd, "tmux capture-pane -p -S -%d", lines);
    char *d = run_collect(cmd, 10, 128 * 1024, &rc);
    if (!d) { snprintf(out, cap, "error: cannot run tmux"); return; }
    if (rc != 0) { snprintf(out, cap, "error: tmux capture-pane failed: %.300s", d); free(d); return; }
    size_t dn = strlen(d);
    while (dn && (d[dn - 1] == '\n' || d[dn - 1] == ' ')) d[--dn] = 0;       /* o painel vem preenchido com linhas vazias */
    const char *t = tail_lines(d, dn, lines, 6000);
    snprintf(out, cap, "[tmux pane %s, last lines]\n%s", pane[0] ? pane : "current", t);
    free(d);
}
#endif
