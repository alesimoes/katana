/* ---------- lista de tarefas do proprio modelo ----------
 * TodoWrite (tools/tasks.c): o modelo grava os passos do trabalho (um por linha) e marca com [x] os feitos. A lista
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

