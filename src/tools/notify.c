/* ---------- PushNotification, SendUserFile, EndConversation (docs/claude-tools.md) ----------
 * Versoes locais: nada sai da maquina. Para a interface (KATANA_UI) vao marcadores de linha:
 *   \x01NOTIFY titulo<TAB>mensagem        \x01FILE modo<TAB>caminho<TAB>legenda */

/* troca controles (tab, quebra de linha, \x01) por espaco, no lugar */
static void ct_nt_clean(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r' || *s == 1) *s = ' '; }
/* corta em no maximo n caracteres (UTF-8); 1 se cortou */
static int ct_nt_cut(char *s, size_t n) {
    size_t c = 0;
    for (char *p = s; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) { if (c++ == n) { *p = 0; return 1; } }
    return 0;
}

static const char CT_PUSHNOTIFICATION_DESC[] =
    "Shows a local desktop notification to the user (notify-send, and the desktop application when it is running). "
    "Use it to say that a long task finished or that something needs the user's attention. 'message' is limited to "
    "200 characters; 'title' is optional. Nothing leaves this machine.";
static const char CT_PUSHNOTIFICATION_PROPS[] =
    "\"message\": {\"type\": \"string\", \"description\": \"Notification text, at most 200 characters.\"}, "
    "\"title\": {\"type\": \"string\", \"description\": \"Short title (default: katana).\"}";
static const char CT_PUSHNOTIFICATION_REQ[] = "\"message\"";
static void ct_pushnotification(const char *js, const char *end, char *out, size_t cap) {
    char *msg = ct_str(js, end, "message"), *title = ct_str(js, end, "title");
    out[0] = 0;
    if (!msg || !msg[0]) { snprintf(out, cap, "error: missing message"); goto done; }
    if (!title || !title[0]) { free(title); title = strdup("katana"); }
    ct_nt_clean(msg); ct_nt_clean(title);
    const int truncated = ct_nt_cut(msg, 200);
    ct_nt_cut(title, 80);
    int ui = 0, sent = 0;
    if (getenv("KATANA_UI")) { printf("\x01" "NOTIFY %s\t%s\n", title, msg); fflush(stdout); ui = 1; }
#ifndef _WIN32
    {
        const char *path = getenv("PATH"); int have = 0; char bin[4400];
        for (const char *p = path ? path : ""; *p; ) {
            size_t l = strcspn(p, ":");
            if (l && l + 14 < sizeof bin) { snprintf(bin, sizeof bin, "%.*s/notify-send", (int)l, p); if (access(bin, X_OK) == 0) { have = 1; break; } }
            p += l; if (*p == ':') p++;
        }
        if (have) {
            const pid_t pid = fork();
            if (pid == 0) {
                const int dn = open("/dev/null", O_RDWR);
                if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
                execl(bin, "notify-send", "-a", "katana", "--", title, msg, (char *)NULL);
                _exit(127);
            }
            if (pid > 0) { int st = 0; waitpid(pid, &st, 0); sent = WIFEXITED(st) && WEXITSTATUS(st) == 0; }
        }
    }
#endif
    if (sent || ui) snprintf(out, cap, "notification shown (%s%s%s)%s", sent ? "notify-send" : "", sent && ui ? " and " : "", ui ? "desktop application" : "", truncated ? "; the message was cut to 200 characters" : "");
    else snprintf(out, cap, "no notification channel is available (notify-send is not installed or failed, and the desktop application is not running); nothing was shown");
done:
    free(msg); free(title);
}

static const char CT_SENDUSERFILE_DESC[] =
    "Hands files to the user: the desktop application shows them for opening or saving, and the result lists their "
    "absolute paths and sizes so a console user knows where they are. 'files' is an array of paths that must exist "
    "and be readable. Nothing leaves this machine.";
static const char CT_SENDUSERFILE_PROPS[] =
    "\"files\": {\"type\": \"array\", \"items\": {\"type\": \"string\"}, \"description\": \"Paths of the files to hand to the user.\"}, "
    "\"caption\": {\"type\": \"string\", \"description\": \"Short text shown with the files.\"}, "
    "\"display\": {\"type\": \"string\", \"description\": \"How to show them (for example inline or attachment); default auto.\"}";
static const char CT_SENDUSERFILE_REQ[] = "\"files\"";
static void ct_senduserfile(const char *js, const char *end, char *out, size_t cap) {
    const char *vs, *ve;
    char *caption = ct_str(js, end, "caption"), *display = ct_str(js, end, "display");
    char *paths[64]; int n = 0;
    char (*res)[4300] = NULL;
    out[0] = 0;
    if (!ct_find(js, end, "files", &vs, &ve) || *vs != '[') { snprintf(out, cap, "error: files must be an array of paths"); goto done; }
    for (const char *p = vs + 1; p < ve && n < 64; ) {
        while (p < ve && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= ve || *p == ']') break;
        const char *e = ct_skip(p, ve);
        if (!e) break;
        if (*p == '"') paths[n++] = ct_unquote(p, e, NULL);
        p = e;
    }
    if (n == 0) { snprintf(out, cap, "error: files is empty; pass at least one path"); goto done; }
    res = (char (*)[4300])malloc((size_t)n * 4300);
    long long sizes[64]; int bad = 0;
    for (int i = 0; i < n && !bad; i++) {
        char err[400]; struct stat st;
        if (!agent_read_access(paths[i], 0, res[i], 4300, err, sizeof err)) { snprintf(out, cap, "%s", err); bad = 1; break; }
        if (stat(res[i], &st) != 0) { snprintf(out, cap, "error: %s does not exist; nothing was sent", paths[i]); bad = 1; break; }
        if (!S_ISREG(st.st_mode)) { snprintf(out, cap, "error: %s is not a regular file; nothing was sent", res[i]); bad = 1; break; }
        FILE *f = fopen(res[i], "rb");
        if (!f) { snprintf(out, cap, "error: %s is not readable (%s); nothing was sent", res[i], strerror(errno)); bad = 1; break; }
        fclose(f);
        sizes[i] = (long long)st.st_size;
    }
    if (!bad) {
        if (!display || !display[0]) { free(display); display = strdup("auto"); }
        if (!caption) caption = strdup("");
        ct_nt_clean(display);
        const int ui = getenv("KATANA_UI") != NULL;
        size_t o = (size_t)snprintf(out, cap, "%d file%s handed to the user:", n, n == 1 ? "" : "s");
        char cap1[1200]; snprintf(cap1, sizeof cap1, "%s", caption); ct_nt_clean(cap1);
        for (int i = 0; i < n; i++) {
            if (ui) { printf("\x01" "FILE %s\t%s\t%s\n", display, res[i], cap1); fflush(stdout); }
            if (o < cap) o += (size_t)snprintf(out + o, cap - o, "\n%s (%lld bytes)", res[i], sizes[i]);
        }
        if (caption[0] && o < cap) snprintf(out + o, cap - o, "\ncaption: %s", cap1);
    }
done:
    free(res);
    for (int i = 0; i < n; i++) free(paths[i]);
    free(caption); free(display);
}

/* EndConversation: o laco do agente (src/agent/loop.c) deve testar g_ct_end_conversation logo depois que agent_tool()
 * volta, em cada chamada de ferramenta do turno: se for 1, nao gera mais nada, mostra g_ct_end_reason ao usuario e
 * encerra a conversa (como /exit ou /clear, e zera a flag). */
static int g_ct_end_conversation;
static char g_ct_end_reason[400];
static const char CT_ENDCONVERSATION_DESC[] =
    "Ends the conversation. Use it ONLY for sustained abuse after you have already warned the user once and the abuse "
    "continued, or when the user explicitly asks to end the conversation and has confirmed it. Never use it for "
    "ordinary disagreement, difficult tasks or errors. After it returns, write nothing more.";
static const char CT_ENDCONVERSATION_PROPS[] =
    "\"reason\": {\"type\": \"string\", \"description\": \"Why the conversation is being ended (one short sentence).\"}";
static const char CT_ENDCONVERSATION_REQ[] = "\"reason\"";
static void ct_endconversation(const char *js, const char *end, char *out, size_t cap) {
    char *r = ct_str(js, end, "reason");
    if (!r || !r[0]) { snprintf(out, cap, "error: missing reason; say in one sentence why the conversation is ending (only for sustained abuse after a warning, or a confirmed user request)"); free(r); return; }
    snprintf(g_ct_end_reason, sizeof g_ct_end_reason, "%s", r);
    g_ct_end_conversation = 1;
    snprintf(out, cap, "ok: the conversation will end after this call; reason recorded: %.300s", g_ct_end_reason);
    free(r);
}
