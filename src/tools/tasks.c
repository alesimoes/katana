/* ---------- TodoWrite (TaskCreate/Update/List/Get foram removidas, ver docs/tools-review.md) ----------
 * Uma lista so, por baixo: a de agent/todo.c (g_todo, linhas "[ ] texto" e "[x] texto"), que o desktop desenha como
 * cartao pelo marcador de todo_ui(). Aqui a lista tem mais detalhe (id, estado de tres valores, descricao, forma
 * "em andamento") e a cada mudanca o texto de g_todo e refeito e todo_ui() emitido. Se algo de fora (uma conversa
 * nova, a interface) mexer em g_todo, a diferenca e percebida e a lista estruturada e refeita a partir dele. */
#define CT_TASK_MAX 64
typedef struct { int id; int st; char subject[300]; char desc[1000]; char active[300]; int rec; } CtTask;     /* st: 0 pendente, 1 andando, 2 feita; rec: so anotada (TaskCreate), ninguem comecou */
static CtTask g_ct_tasks[CT_TASK_MAX];
static int g_ct_ntasks, g_ct_nextid = 1;
static char g_ct_rendered[3000];                 /* o que escrevemos em g_todo por ultimo */
static const char *const CT_ST_NAME[3] = { "pending", "in_progress", "completed" };

static void ct_task_render(void) {
    size_t o = 0;
    {   /* mais passos prontos que antes (por done=N ou por reescrever a lista): fronteira de etapa (context.c) */
        static int was; int d = 0;
        for (int i = 0; i < g_ct_ntasks; i++) d += g_ct_tasks[i].st == 2;
        if (d > was) g_cp_step = 1;
        was = d;
    }
    for (int i = 0; i < g_ct_ntasks; i++) {
        const CtTask *t = &g_ct_tasks[i];
        if (o + 200 >= sizeof g_todo) break;
        o += (size_t)snprintf(g_todo + o, sizeof g_todo - o, "%s %.180s\n", t->st == 2 ? "[x]" : "[ ]", t->subject);
    }
    g_todo[o] = 0;
    snprintf(g_ct_rendered, sizeof g_ct_rendered, "%s", g_todo);
    g_todo_touched = 1;
    todo_ui();
}
/* antes de qualquer operacao: se g_todo mudou por fora, refaz a lista a partir dele */
static void ct_task_sync(void) {
    if (strcmp(g_todo, g_ct_rendered) == 0) return;
    g_ct_ntasks = 0;
    if (!g_todo[0]) g_ct_nextid = 1;
    for (const char *p = g_todo; *p && g_ct_ntasks < CT_TASK_MAX; ) {
        const char *e = strchr(p, '\n');
        const size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l >= 4) {
            CtTask *t = &g_ct_tasks[g_ct_ntasks++];
            memset(t, 0, sizeof *t);
            t->id = g_ct_nextid++;
            t->st = p[1] == 'x' ? 2 : 0;
            snprintf(t->subject, sizeof t->subject, "%.*s", (int)(l - 4), p + 4);
        }
        if (!e) break;
        p = e + 1;
    }
    snprintf(g_ct_rendered, sizeof g_ct_rendered, "%s", g_todo);
}
static int ct_st_parse(const char *s) { for (int i = 0; i < 3; i++) if (s && strcmp(s, CT_ST_NAME[i]) == 0) return i; return -1; }
static void ct_cpy(char *dst, size_t cap, const char *s) { snprintf(dst, cap, "%s", s ? s : ""); for (char *p = dst; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' '; }
static int ct_task_count(int st) { int n = 0; for (int i = 0; i < g_ct_ntasks; i++) n += g_ct_tasks[i].st == st; return n; }

static const char CT_TODOWRITE_DESC[] =
    "Create and update the task list for the work in progress. Send the WHOLE list every time (it replaces the previous one). "
    "Use it for work with 3 or more steps; keep exactly one task in_progress while you work on it, and mark each task completed as soon as it is done.";
static const char CT_TODOWRITE_PROPS[] =
    "\"todos\": {\"type\": \"array\", \"description\": \"The complete task list\", \"items\": {\"type\": \"object\", \"properties\": {"
    "\"content\": {\"type\": \"string\", \"description\": \"What to do, in the imperative (Run the tests)\"}, "
    "\"status\": {\"type\": \"string\", \"enum\": [\"pending\", \"in_progress\", \"completed\"]}, "
    "\"activeForm\": {\"type\": \"string\", \"description\": \"The same task while it runs (Running the tests)\"}}, "
    "\"required\": [\"content\", \"status\", \"activeForm\"]}}, "
    "\"done\": {\"type\": \"integer\", \"description\": \"Without todos: how many steps of the current list are finished; the next one becomes the step in progress\"}";
static const char CT_TODOWRITE_REQ[] = "";
/* plano em linhas de texto: um passo por linha; [x] feito, [>] ou [~] em andamento, [ ] ou nada pendente
   (aceita "- ", "* ", "1. " na frente). Mais barato que o array JSON (docs/call-formats.md). Devolve quantos passos. */
static int ct_todo_lines(const char *txt, CtTask *nt, char *out, size_t cap) {
    int n = 0;
    for (const char *p = txt; *p; ) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        const char *nx = e ? e + 1 : p + l;
        while (l && (*p == ' ' || *p == '\t' || *p == '-' || *p == '*')) { p++; l--; }
        while (l && isdigit((unsigned char)*p)) { p++; l--; }
        while (l && (*p == '.' || *p == ')' || *p == ' ')) { p++; l--; }
        int st = 0;
        if (l >= 3 && p[0] == '[' && p[2] == ']') {
            st = (p[1] == 'x' || p[1] == 'X') ? 2 : (p[1] == '>' || p[1] == '~') ? 1 : 0;
            p += 3; l -= 3;
        }
        while (l && *p == ' ') { p++; l--; }
        while (l && (p[l - 1] == ' ' || p[l - 1] == '\r')) l--;
        {   /* o modelo marca o passo feito escrevendo no fim da linha (" - DONE", "(done)", "- feito"): vale como [x] e sai do texto.
               A sessao de 25/11 reenviou a lista 30 vezes assim e recebeu "the list did not change" em todas */
            static const char *const sfx[] = {"done", "DONE", "Done", "feito", "FEITO", "Feito", "ok", "OK", "concluido", "completed", "COMPLETED", NULL};
            for (int si = 0; sfx[si]; si++) {
                const size_t sl = strlen(sfx[si]); size_t k = l;
                if (k && p[k - 1] == ')') k--;
                if (k <= sl + 2 || strncmp(p + k - sl, sfx[si], sl)) continue;
                k -= sl;
                if (p[k - 1] == '(') k--; else { while (k && p[k - 1] == ' ') k--; if (!k || (p[k - 1] != '-' && p[k - 1] != ':')) continue; k--; }
                while (k && p[k - 1] == ' ') k--;
                if (!k) continue;
                l = k; st = 2;
                break;
            }
        }
        if (l) {
            if (n == CT_TASK_MAX) { snprintf(out, cap, "error: too many tasks (at most %d); merge or drop some", CT_TASK_MAX); return -1; }
            char line[512]; snprintf(line, sizeof line, "%.*s", (int)(l < 500 ? l : 500), p);
            nt[n].st = st;
            ct_cpy(nt[n].subject, sizeof nt[n].subject, line);
            ct_cpy(nt[n].active, sizeof nt[n].active, line);
            n++;
        }
        p = nx;
    }
    if (!n) snprintf(out, cap, "error: todos has no steps (one step per line: [x] done, [>] in progress, [ ] pending)");
    return n;
}
static void ct_todo_commit(CtTask *nt, int n, char *out, size_t cap);
/* o pedido terminou com so o ultimo passo em aberto: o motor o fecha, sem gastar uma rodada do modelo com isso */
static int ct_todo_autofinish(void) {
    int open = 0, last = -1;
    for (int i = 0; i < g_ct_ntasks; i++) if (g_ct_tasks[i].st != 2) { open++; last = i; }
    if (open != 1 || last != g_ct_ntasks - 1) return 0;
    if (g_ct_tasks[last].rec) return 0;      /* tarefa so anotada a pedido do usuario: responder nao a conclui */
    g_ct_tasks[last].st = 2;
    ct_task_render();
    return 1;
}
static void ct_todowrite(const char *js, const char *end, char *out, size_t cap) {
    const char *a, *b;
    if (!ct_find(js, end, "todos", &a, &b) && ct_find(js, end, "done", &a, &b)) {
        /* progresso barato: o plano e escrito uma vez; depois basta dizer quantos passos acabaram (docs/design/10-agent-loop.md) */
        ct_task_sync();
        const int n = g_ct_ntasks;
        long d = (long)ct_int(js, end, "done", 0);
        if (!n) { snprintf(out, cap, "error: there is no task list yet; send todos (one step per line) first"); return; }
        if (d < 0) d = 0;
        if (d > n) d = n;
        for (int i = 0; i < n; i++) g_ct_tasks[i].st = i < d ? 2 : i == d ? 1 : 0;
        ct_task_render();
        if (d < n) snprintf(out, cap, "Step %ld of %d: %.300s.", d + 1, n, g_ct_tasks[d].subject);
        else snprintf(out, cap, "All %d steps are completed: tell the user what was done.", n);
        return;
    }
    if (ct_find(js, end, "todos", &a, &b) && *a == '"') {          /* plano em linhas */
        char *txt = ct_str(js, end, "todos");
        ct_task_sync();
        CtTask *nt = (CtTask *)calloc(CT_TASK_MAX, sizeof *nt);
        const int n = txt ? ct_todo_lines(txt, nt, out, cap) : 0;
        if (!txt) snprintf(out, cap, "error: todos is empty");
        free(txt);
        if (n > 0 && ct_find(js, end, "done", &a, &b)) {      /* todos e done na mesma chamada: done diz quantos passos estao feitos (antes era ignorado) */
            long d = (long)ct_int(js, end, "done", 0); if (d < 0) d = 0; if (d > n) d = n;
            for (int i = 0; i < n; i++) if (i < d) nt[i].st = 2; else if (i == d && nt[i].st == 0) nt[i].st = 1;
        }
        if (n > 0) ct_todo_commit(nt, n, out, cap);
        free(nt);
        return;
    }
    if (!ct_find(js, end, "todos", &a, &b) || *a != '[') { snprintf(out, cap, "error: todos must be an array of {content, status, activeForm}, or text with one step per line ([x] done, [>] in progress, [ ] pending)"); return; }
    ct_task_sync();
    CtTask *nt = (CtTask *)calloc(CT_TASK_MAX, sizeof *nt);
    int n = 0;
    const char *p = a + 1;
    for (;;) {
        while (p < b && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= b || *p == ']') break;
        const char *e = ct_skip(p, b);
        if (!e || *p != '{') { snprintf(out, cap, "error: todos[%d] must be an object {content, status, activeForm}", n); free(nt); return; }
        if (n == CT_TASK_MAX) { snprintf(out, cap, "error: too many tasks (at most %d); merge or drop some", CT_TASK_MAX); free(nt); return; }
        char *obj = (char *)malloc((size_t)(e - p) + 1);
        memcpy(obj, p, (size_t)(e - p)); obj[e - p] = 0;
        char *c = ct_str(obj, obj + (e - p), "content"), *s = ct_str(obj, obj + (e - p), "status"), *f = ct_str(obj, obj + (e - p), "activeForm");
        free(obj);
        int bad = 0;
        if (!c || !c[0]) { snprintf(out, cap, "error: todos[%d].content is missing or empty", n); bad = 1; }
        else if (!s) { snprintf(out, cap, "error: todos[%d].status is missing (pending, in_progress or completed)", n); bad = 1; }
        else if (ct_st_parse(s) < 0) { snprintf(out, cap, "error: todos[%d].status is \"%.40s\"; it must be pending, in_progress or completed", n, s); bad = 1; }
        if (!bad) {
            CtTask *t = &nt[n++];
            t->st = ct_st_parse(s);
            ct_cpy(t->subject, sizeof t->subject, c);
            ct_cpy(t->active, sizeof t->active, f && f[0] ? f : c);
        }
        free(c); free(s); free(f);
        if (bad) { free(nt); return; }
        p = e;
    }
    ct_todo_commit(nt, n, out, cap);
    free(nt);
}
/* grava a lista nova no lugar da antiga e escreve o resultado */
static void ct_todo_commit(CtTask *nt, int n, char *out, size_t cap) {
    /* plano vazio ou igual nao e progresso: o modelo ficava em laco so reescrevendo a lista (prova de 21/11/2026) */
    if (g_ct_ntasks && ct_task_count(2) < g_ct_ntasks) {
        int same = n == g_ct_ntasks, cur = 0;
        for (int i = 0; i < n && same; i++) same = nt[i].st == g_ct_tasks[i].st && strcmp(nt[i].subject, g_ct_tasks[i].subject) == 0;
        while (cur < g_ct_ntasks - 1 && g_ct_tasks[cur].st == 2) cur++;
        if (!n) { snprintf(out, cap, "error: that list is empty and the current one still has open steps. Do step %d of %d: %.300s. When it is finished send done=%d.", cur + 1, g_ct_ntasks, g_ct_tasks[cur].subject, cur + 1); return; }
        if (same) { snprintf(out, cap, "error: the list did not change. Do step %d of %d now: %.300s. When it is finished send done=%d. If you cannot do that step yourself (it needs the user's login, a password, an install or a decision), do NOT send the list again: answer the user in words saying what you need.", cur + 1, g_ct_ntasks, g_ct_tasks[cur].subject, cur + 1); return; }
    }
    /* mantem a descricao de quem ja existia com o mesmo texto; ids novos em sequencia */
    for (int i = 0; i < n; i++) {
        nt[i].id = g_ct_nextid++;
        for (int j = 0; j < g_ct_ntasks; j++) if (strcmp(g_ct_tasks[j].subject, nt[i].subject) == 0) { snprintf(nt[i].desc, sizeof nt[i].desc, "%s", g_ct_tasks[j].desc); break; }
    }
    memcpy(g_ct_tasks, nt, sizeof g_ct_tasks[0] * (size_t)n);
    g_ct_ntasks = n;
    ct_task_render();
    const int ip = ct_task_count(1);
    int cur = -1;      /* passo atual: o primeiro em andamento, senao o primeiro pendente */
    for (int i = 0; i < n && cur < 0; i++) if (g_ct_tasks[i].st == 1) cur = i;
    for (int i = 0; i < n && cur < 0; i++) if (g_ct_tasks[i].st == 0) cur = i;
    char nx[400] = "";
    if (cur >= 0) snprintf(nx, sizeof nx, " Now step %d of %d: %.300s. Do it, then mark it completed.", cur + 1, n, g_ct_tasks[cur].subject);
    else if (n) snprintf(nx, sizeof nx, " All steps are completed: tell the user what was done.");
    snprintf(out, cap, "Todos updated: %d tasks (%d completed, %d in progress, %d pending).%s%s", n, ct_task_count(2), ip, ct_task_count(0),
             ip > 1 ? " Warning: more than one task is in_progress; keep exactly one in progress at a time." : "", nx);
}

