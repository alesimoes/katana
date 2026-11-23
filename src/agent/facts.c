/* ---------- caderno de fatos: o que o motor sabe da conversa sem perguntar ao modelo ----------
 * Arquivos mudados e lidos, comandos com o codigo de saida e a ultima falha com o texto dela. Entra inteiro quando o contexto e
 * resumido (context.c): o resumo do modelo perde nomes e numeros, o caderno nao. So o agente principal escreve nele. */
static int g_ct_depth;                  /* 1 dentro de uma ferramenta do agente principal; 2 ou mais: dentro de um subagente */
#define FC_FILES 40
#define FC_CMDS 12
static struct { char path[300]; int edits, reads, seq; } g_fc_f[FC_FILES];
static struct { char cmd[200]; int rc, seq; } g_fc_c[FC_CMDS];
static int g_fc_nf, g_fc_nc, g_fc_seq;
static char g_fc_fail[700];
static void facts_reset(void) { g_fc_nf = g_fc_nc = g_fc_seq = 0; g_fc_fail[0] = 0; }
static void fc_cut(char *d, size_t cap, const char *s) {      /* copia numa linha, sem cortar caractere no meio */
    size_t n = 0;
    for (; *s && n + 1 < cap; s++) d[n++] = *s == '\n' || *s == '\r' || *s == '\t' ? ' ' : *s;
    if (*s) { while (n && ((unsigned char)d[n - 1] & 0xC0) == 0x80) n--; if (n && (unsigned char)d[n - 1] >= 0xC0) n--; }
    d[n] = 0;
}
static void fc_file(const char *path, int edit) {
    int at = -1;
    for (int i = 0; i < g_fc_nf; i++) if (strcmp(g_fc_f[i].path, path) == 0) at = i;
    if (at < 0) {
        if (strlen(path) >= sizeof g_fc_f[0].path) return;
        if (g_fc_nf < FC_FILES) at = g_fc_nf++;
        else {                                                  /* cheio: sai o mais antigo que so foi lido; mudado nao sai por leitura */
            for (int i = 0; i < g_fc_nf; i++) if (!g_fc_f[i].edits && (at < 0 || g_fc_f[i].seq < g_fc_f[at].seq)) at = i;
            if (at < 0 && edit) { at = 0; for (int i = 1; i < g_fc_nf; i++) if (g_fc_f[i].seq < g_fc_f[at].seq) at = i; }
            if (at < 0) return;
        }
        snprintf(g_fc_f[at].path, sizeof g_fc_f[at].path, "%s", path); g_fc_f[at].edits = g_fc_f[at].reads = 0;
    }
    if (edit) g_fc_f[at].edits++; else g_fc_f[at].reads++;
    g_fc_f[at].seq = ++g_fc_seq;
}
/* depois de cada ferramenta do agente principal (ct_dispatch) */
static void facts_note(const char *name, const char *file, const char *cmd, const char *res) {
    const int err = strncmp(res, "error:", 6) == 0;
    if (file && *file && !err) {
        if (!strcmp(name, "Write") || !strcmp(name, "Edit") || !strcmp(name, "NotebookEdit")) fc_file(file, 1);
        else if (!strcmp(name, "Read")) fc_file(file, 0);
    }
    if (strcmp(name, "Bash") != 0 || !cmd) return;
    char c[200]; fc_cut(c, sizeof c, cmd);
    int rc = err ? -1 : 0;
    const char *x = NULL;
    for (const char *q = strstr(res, "\nExit code "); q; q = strstr(q + 1, "\nExit code ")) x = q;
    if (!strncmp(res, "Exit code ", 10) && !x) rc = atoi(res + 10);
    else if (x) rc = atoi(x + 11);
    int at = -1;
    for (int i = 0; i < g_fc_nc; i++) if (strcmp(g_fc_c[i].cmd, c) == 0) at = i;
    if (at < 0) { if (g_fc_nc < FC_CMDS) at = g_fc_nc++; else { at = 0; for (int i = 1; i < FC_CMDS; i++) if (g_fc_c[i].seq < g_fc_c[at].seq) at = i; } snprintf(g_fc_c[at].cmd, sizeof g_fc_c[at].cmd, "%s", c); }
    g_fc_c[at].rc = rc; g_fc_c[at].seq = ++g_fc_seq;
    if (rc) {
        const char *e = x ? x : res + strlen(res), *a = e - res > 450 ? e - 450 : res;
        while (a < e && ((unsigned char)*a & 0xC0) == 0x80) a++;
        char t[460]; const size_t n = (size_t)(e - a) < sizeof t - 1 ? (size_t)(e - a) : sizeof t - 1;
        memcpy(t, a, n); t[n] = 0;
        snprintf(g_fc_fail, sizeof g_fc_fail, "`%s` (exit %d):\n%s", c, rc, t);
    } else if (!strncmp(g_fc_fail, "`", 1) && !strncmp(g_fc_fail + 1, c, strlen(c)) && g_fc_fail[1 + strlen(c)] == '`') g_fc_fail[0] = 0;      /* o mesmo comando passou */
}
/* texto do caderno; 0 = vazio */
static int facts_text(Buf *o) {
    if (!g_fc_nf && !g_fc_nc) return 0;
    char t[700];
    buf_str(o, "[Facts kept by katana, exact]\n");
    for (int k = 0; k < 2; k++) {
        int first = 1;
        for (int i = 0; i < g_fc_nf; i++) {
            if ((g_fc_f[i].edits > 0) == (k == 1)) continue;
            buf_str(o, first ? (k ? "files only read: " : "files changed: ") : ", "); first = 0;
            if (k) snprintf(t, sizeof t, "%s", g_fc_f[i].path); else snprintf(t, sizeof t, "%s (%d)", g_fc_f[i].path, g_fc_f[i].edits);
            buf_str(o, t);
        }
        if (!first) buf_str(o, "\n");
    }
    if (g_fc_nc) {
        int done[FC_CMDS] = {0};
        buf_str(o, "commands, last result of each:\n");
        for (int n = 0; n < g_fc_nc; n++) {
            int at = -1;
            for (int i = 0; i < g_fc_nc; i++) if (!done[i] && (at < 0 || g_fc_c[i].seq < g_fc_c[at].seq)) at = i;
            done[at] = 1;
            snprintf(t, sizeof t, "- `%s` -> %s%d\n", g_fc_c[at].cmd, g_fc_c[at].rc < 0 ? "not run, " : "exit ", g_fc_c[at].rc);
            buf_str(o, t);
        }
    }
    if (g_fc_fail[0]) { buf_str(o, "last failure not yet fixed: "); buf_str(o, g_fc_fail); buf_str(o, "\n"); }
    return 1;
}

/* ---- leitura por diferenca: o arquivo relido inteiro, com a leitura anterior ainda no contexto ---- */
#define RD_KEEP 24
static struct { char *path, *txt; size_t n; int epoch, seq, same; } g_rk[RD_KEEP];
static int g_rk_n, g_rk_seq, g_rd_delta_on;      /* ligado pelo laco do agente */
static int g_rd_epoch;
static void rd_delta_reset(void) { for (int i = 0; i < g_rk_n; i++) { free(g_rk[i].path); free(g_rk[i].txt); } g_rk_n = 0; }
static int rd_delta_ok(void) { return g_rd_delta_on && g_ct_depth == 1; }
static size_t rd_line(const char *b, size_t n, size_t s) { const char *q = s < n ? (const char *)memchr(b + s, '\n', n - s) : NULL; return q ? (size_t)(q - b) + 1 : n; }
/* guarda o texto mostrado inteiro */
static void rd_delta_put(const char *path, const char *b, size_t n) {
    if (!rd_delta_ok() || n > 262144) return;
    int at = -1;
    for (int i = 0; i < g_rk_n; i++) if (strcmp(g_rk[i].path, path) == 0) at = i;
    if (at < 0) {
        if (g_rk_n < RD_KEEP) at = g_rk_n++;
        else { at = 0; for (int i = 1; i < RD_KEEP; i++) if (g_rk[i].seq < g_rk[at].seq) at = i; free(g_rk[at].path); free(g_rk[at].txt); }
        g_rk[at].path = strdup(path); g_rk[at].txt = NULL;
    }
    free(g_rk[at].txt);
    g_rk[at].txt = (char *)malloc(n + 1); if (!g_rk[at].txt || !g_rk[at].path) die("sem memoria");
    memcpy(g_rk[at].txt, b, n); g_rk[at].txt[n] = 0;
    g_rk[at].n = n; g_rk[at].epoch = g_rd_epoch; g_rk[at].seq = ++g_rk_seq; g_rk[at].same = 0;
}
/* 1 = out tem a resposta curta (igual, ou so o trecho que mudou); 0 = mostrar inteiro */
static int rd_delta(const char *path, const char *b, size_t n, char *out, size_t cap) {
    if (!rd_delta_ok() || n < 1500 || cap < 3000) return 0;
    int at = -1;
    for (int i = 0; i < g_rk_n; i++) if (strcmp(g_rk[i].path, path) == 0 && g_rk[i].epoch == g_rd_epoch) at = i;
    if (at < 0 || g_rk[at].same) return 0;              /* pediu de novo depois de uma resposta curta: vai inteiro */
    const char *a = g_rk[at].txt; const size_t m = g_rk[at].n;
    if (m == n && !memcmp(a, b, n)) {
        g_rk[at].same = 1;
        snprintf(out, cap, "[%s has not changed since you read it in this conversation; that reading is above and still exact. Read it again only if you need it repeated.]", path);
        return 1;
    }
    size_t p = 0, l1 = 1;                               /* linhas iguais no comeco */
    while (p < m && p < n) { const size_t e1 = rd_line(a, m, p), e2 = rd_line(b, n, p); if (e1 != e2 || memcmp(a + p, b + p, e1 - p)) break; p = e1; l1++; }
    size_t x = m, y = n;                                /* e no fim */
    while (x > p && y > p) {
        size_t s1 = x - 1, s2 = y - 1;
        while (s1 > p && a[s1 - 1] != '\n') s1--;
        while (s2 > p && b[s2 - 1] != '\n') s2--;
        if (x - s1 != y - s2 || memcmp(a + s1, b + s2, x - s1)) break;
        x = s1; y = s2;
    }
    if ((y - p) * 5 > n * 2) return 0;                  /* mudou mais de 40%: inteiro */
    long long old_ln = 0, new_ln = 0, tot = 0;
    for (size_t i = p; i < x; i++) old_ln += a[i] == '\n';
    for (size_t i = p; i < y; i++) new_ln += b[i] == '\n';
    for (size_t i = 0; i < n; i++) tot += b[i] == '\n';
    if (n && b[n - 1] != '\n') tot++;
    long long from = (long long)l1 - 3, to = (long long)l1 + new_ln + 2;
    if (from < 1) from = 1;
    if (to > tot) to = tot;
    int w = snprintf(out, cap, "[%s changed since you read it in this conversation; only the changed part is shown (lines %lld-%lld of %lld). Lines before it are as you read them%s",
                     path, from, to, tot, "");
    if (new_ln != old_ln) w += snprintf(out + w, cap - (size_t)w, "; lines after it are the same text, %lld line%s %s", llabs(new_ln - old_ln), llabs(new_ln - old_ln) == 1 ? "" : "s", new_ln > old_ln ? "further down" : "further up");
    else w += snprintf(out + w, cap - (size_t)w, "; so are the lines after it");
    w += snprintf(out + w, cap - (size_t)w, ". Read it again for the whole file.]\n");
    size_t s = 0; long long ln = 1;
    while (ln < from) { s = rd_line(b, n, s); ln++; }
    for (; ln <= to && s < n; ln++) {
        size_t e = rd_line(b, n, s), z = e;
        while (z > s && (b[z - 1] == '\n' || b[z - 1] == '\r')) z--;
        if (z - s > 2000 || (size_t)w + (z - s) + 64 > cap) return 0;
        w += snprintf(out + w, cap - (size_t)w, "%6lld\t%.*s\n", ln, (int)(z - s), b + s);
        s = e;
    }
    if (w > 0 && out[w - 1] == '\n') out[w - 1] = 0;
    rd_delta_put(path, b, n);
    for (int i = 0; i < g_rk_n; i++) if (strcmp(g_rk[i].path, path) == 0) g_rk[i].same = 1;
    return 1;
}
