/* ---------- lista de tarefas do proprio modelo ----------
 * todo(steps): o modelo grava os passos do trabalho (um por linha) e chama de novo marcando com [x] os feitos. A lista
 * fica no motor: volta para o contexto depois de uma compactacao e, se o modelo encerra a resposta com passos em
 * aberto que ele mesmo listou neste pedido, o laco lembra uma vez. */
#define TODO_MAX 12
static char g_todo[3000];
static int g_todo_touched;      /* a lista foi gravada durante o pedido atual */

static int todo_count(int *done) {
    int n = 0, d = 0;
    for (const char *p = g_todo; *p; ) {
        if (strncmp(p, "[x]", 3) == 0) d++;
        n++;
        p = strchr(p, '\n'); if (!p) break; p++;
    }
    if (done) *done = d;
    return n;
}
static void todo_ui(void) {
    if (!getenv("KATANA_UI")) return;
    fputs("\x01" "TODO ", stdout);
    for (const char *p = g_todo; *p; p++) { if (*p == '\n') fputs("\\n", stdout); else if (*p == '\\') fputs("\\\\", stdout); else if (*p != 1 && *p != '\r') putchar(*p); }
    putchar('\n'); fflush(stdout);
}
static void todo_clear(void) { g_todo[0] = 0; g_todo_touched = 0; todo_ui(); }

static void tool_todo(const char *js, const char *end, char *out, size_t cap) {
    static char raw[6000];
    if (!json_str(js, end, "steps", raw, sizeof raw) || !raw[0]) {
        if (g_todo[0]) snprintf(out, cap, "current list:\n%s", g_todo); else snprintf(out, cap, "error: missing steps (one step per line; put [x] in front of the finished ones)");
        return;
    }
    if (raw[0] == '[' && raw[1] == '"') {       /* o modelo mandou uma lista JSON dentro do texto: ["a", "b"] vira uma linha por item */
        size_t w = 0;
        for (size_t i = 2; raw[i]; i++) {
            if (raw[i] == '"' && (raw[i + 1] == ',' || raw[i + 1] == ']')) {
                i++;
                while (raw[i + 1] == ' ' || raw[i + 1] == '\n') i++;
                if (raw[i + 1] == '"') i++;
                raw[w++] = '\n';
            } else raw[w++] = raw[i];
        }
        raw[w] = 0;
    }
    size_t o = 0; int n = 0, done = 0;
    char next[200] = "";
    for (char *p = raw; *p && n < TODO_MAX; ) {
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char *nx = e ? e + 1 : p + l;
        while (l && (*p == ' ' || *p == '\t' || *p == '-' || *p == '*')) { p++; l--; }
        while (l && isdigit((unsigned char)*p)) { p++; l--; }                      /* "1. passo" ou "1) passo" */
        while (l && (*p == '.' || *p == ')' || *p == ' ')) { p++; l--; }
        int dn = 0;
        if (l >= 3 && p[0] == '[' && p[2] == ']') { dn = p[1] == 'x' || p[1] == 'X'; p += 3; l -= 3; }
        else if (l >= 2 && (p[0] == 'x' || p[0] == 'X') && p[1] == ' ') { dn = 1; p += 2; l -= 2; }
        while (l && *p == ' ') { p++; l--; }
        while (l && (p[l - 1] == ' ' || p[l - 1] == '\r')) l--;
        if (l) {
            if (l > 180) l = 180;
            if (o + l + 8 >= sizeof g_todo) break;
            o += (size_t)snprintf(g_todo + o, sizeof g_todo - o, "%s %.*s\n", dn ? "[x]" : "[ ]", (int)l, p);
            if (!dn && !next[0]) snprintf(next, sizeof next, "%.*s", (int)l, p);
            n++; done += dn;
        }
        p = nx;
    }
    g_todo[o] = 0;
    if (!n) { snprintf(out, cap, "error: no steps found (one step per line)"); return; }
    g_todo_touched = 1;
    todo_ui();
    if (done == n) snprintf(out, cap, "list saved: all %d steps done", n);
    else snprintf(out, cap, "list saved: %d of %d done. Next: %s", done, n, next);
}
