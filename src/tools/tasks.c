/* ---------- TodoWrite, TaskCreate, TaskUpdate, TaskList, TaskGet ----------
 * Uma lista so, por baixo: a de agent/todo.c (g_todo, linhas "[ ] texto" e "[x] texto"), que o desktop desenha como
 * cartao pelo marcador de todo_ui(). Aqui a lista tem mais detalhe (id, estado de tres valores, descricao, forma
 * "em andamento") e a cada mudanca o texto de g_todo e refeito e todo_ui() emitido. Se o `todo` antigo (ou uma conversa
 * nova) mexer em g_todo, a diferenca e percebida e a lista estruturada e refeita a partir dele. */
#define CT_TASK_MAX 64
typedef struct { int id; int st; char subject[300]; char desc[1000]; char active[300]; } CtTask;     /* st: 0 pendente, 1 andando, 2 feita */
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
static int ct_task_idx(int id) { for (int i = 0; i < g_ct_ntasks; i++) if (g_ct_tasks[i].id == id) return i; return -1; }
static void ct_cpy(char *dst, size_t cap, const char *s) { snprintf(dst, cap, "%s", s ? s : ""); for (char *p = dst; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' '; }
static void ct_task_ids(char *buf, size_t cap) {
    size_t o = 0;
    buf[0] = 0;
    for (int i = 0; i < g_ct_ntasks && o + 12 < cap; i++) o += (size_t)snprintf(buf + o, cap - o, "%s%d", i ? ", " : "", g_ct_tasks[i].id);
    if (!g_ct_ntasks) snprintf(buf, cap, "none, the list is empty");
}
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
        if (same) { snprintf(out, cap, "error: the list did not change. Do step %d of %d now: %.300s. When it is finished send done=%d.", cur + 1, g_ct_ntasks, g_ct_tasks[cur].subject, cur + 1); return; }
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

static const char CT_TASKCREATE_DESC[] =
    "Create a task in the task list (status pending). Returns its id. Use TaskUpdate to start it (in_progress) and to finish it (completed).";
static const char CT_TASKCREATE_PROPS[] =
    "\"subject\": {\"type\": \"string\", \"description\": \"Short title in the imperative (Fix the login bug)\"}, "
    "\"description\": {\"type\": \"string\", \"description\": \"What needs to be done\"}, "
    "\"activeForm\": {\"type\": \"string\", \"description\": \"Text shown while the task runs (Fixing the login bug)\"}";
static const char CT_TASKCREATE_REQ[] = "\"subject\", \"description\"";
static void ct_taskcreate(const char *js, const char *end, char *out, size_t cap) {
    char *s = ct_str(js, end, "subject"), *d = ct_str(js, end, "description"), *f = ct_str(js, end, "activeForm");
    if (!s || !s[0]) { snprintf(out, cap, "error: subject is required (a short title for the task)"); goto done; }
    ct_task_sync();
    if (g_ct_ntasks == CT_TASK_MAX) { snprintf(out, cap, "error: the list is full (%d tasks); delete finished tasks with TaskUpdate status deleted", CT_TASK_MAX); goto done; }
    CtTask *t = &g_ct_tasks[g_ct_ntasks++];
    memset(t, 0, sizeof *t);
    t->id = g_ct_nextid++;
    ct_cpy(t->subject, sizeof t->subject, s);
    ct_cpy(t->desc, sizeof t->desc, d);
    ct_cpy(t->active, sizeof t->active, f && f[0] ? f : s);
    ct_task_render();
    snprintf(out, cap, "Task #%d created: %s", t->id, t->subject);
done:
    free(s); free(d); free(f);
}

static const char CT_TASKUPDATE_DESC[] =
    "Update a task: change its status (pending, in_progress, completed, or deleted to remove it) and/or its subject, description or activeForm. "
    "Mark a task in_progress before working on it and completed as soon as it is done.";
static const char CT_TASKUPDATE_PROPS[] =
    "\"taskId\": {\"type\": \"string\", \"description\": \"Id of the task (from TaskCreate or TaskList)\"}, "
    "\"status\": {\"type\": \"string\", \"enum\": [\"pending\", \"in_progress\", \"completed\", \"deleted\"]}, "
    "\"subject\": {\"type\": \"string\", \"description\": \"New title\"}, "
    "\"description\": {\"type\": \"string\", \"description\": \"New description\"}, "
    "\"activeForm\": {\"type\": \"string\", \"description\": \"New text shown while the task runs\"}";
static const char CT_TASKUPDATE_REQ[] = "\"taskId\"";
/* id da tarefa: aceita "3", 3 ou "#3"; -1 se nao for numero */
static int ct_task_id_arg(const char *js, const char *end, char *out, size_t cap, char **keep) {
    char *id = ct_str(js, end, "taskId");
    *keep = id;
    if (!id || !id[0]) { snprintf(out, cap, "error: taskId is required (use TaskList to see the ids)"); return -1; }
    const char *q = id + (id[0] == '#');
    if (!isdigit((unsigned char)*q)) { char ids[200]; ct_task_sync(); ct_task_ids(ids, sizeof ids); snprintf(out, cap, "error: taskId \"%.40s\" is not a number. Valid ids: %s", id, ids); return -1; }
    return atoi(q);
}
static void ct_taskupdate(const char *js, const char *end, char *out, size_t cap) {
    char *idtxt, *st = ct_str(js, end, "status"), *s = ct_str(js, end, "subject"), *d = ct_str(js, end, "description"), *f = ct_str(js, end, "activeForm");
    const int id = ct_task_id_arg(js, end, out, cap, &idtxt);
    if (id < 0) goto done;
    ct_task_sync();
    const int i = ct_task_idx(id);
    if (i < 0) { char ids[200]; ct_task_ids(ids, sizeof ids); snprintf(out, cap, "error: no task with id %d. Valid ids: %s", id, ids); goto done; }
    int del = 0, ns = -1;
    if (st && st[0]) {
        if (strcmp(st, "deleted") == 0) del = 1;
        else if ((ns = ct_st_parse(st)) < 0) { snprintf(out, cap, "error: status \"%.40s\" is invalid; use pending, in_progress, completed or deleted", st); goto done; }
    }
    if (s && !s[0]) { snprintf(out, cap, "error: subject cannot be empty"); goto done; }
    if (del) {
        memmove(&g_ct_tasks[i], &g_ct_tasks[i + 1], sizeof g_ct_tasks[0] * (size_t)(g_ct_ntasks - i - 1));
        g_ct_ntasks--;
        ct_task_render();
        snprintf(out, cap, "Task #%d deleted. %d tasks left.", id, g_ct_ntasks);
        goto done;
    }
    CtTask *t = &g_ct_tasks[i];
    if (ns >= 0) t->st = ns;
    if (s) ct_cpy(t->subject, sizeof t->subject, s);
    if (d) ct_cpy(t->desc, sizeof t->desc, d);
    if (f) ct_cpy(t->active, sizeof t->active, f);
    ct_task_render();
    snprintf(out, cap, "Task #%d updated: %s [%s]%s", t->id, t->subject, CT_ST_NAME[t->st],
             t->st == 1 && ct_task_count(1) > 1 ? ". Warning: more than one task is in_progress; keep exactly one in progress at a time." : "");
done:
    free(idtxt); free(st); free(s); free(d); free(f);
}

static const char CT_TASKLIST_DESC[] = "List all tasks, one line each: #id [status] subject.";
static const char CT_TASKLIST_PROPS[] = "";
static const char CT_TASKLIST_REQ[] = "";
static void ct_tasklist(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    ct_task_sync();
    if (!g_ct_ntasks) { snprintf(out, cap, "No tasks."); return; }
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < g_ct_ntasks; i++) {
        const CtTask *t = &g_ct_tasks[i];
        o += (size_t)snprintf(out + o, cap - o, "#%d [%s] %s\n", t->id, CT_ST_NAME[t->st], t->subject);
        if (o + 1 >= cap) { out[cap - 1] = 0; return; }
    }
    out[o - 1] = 0;
}

static const char CT_TASKGET_DESC[] = "Get all the fields of one task by its id: subject, status, description, activeForm.";
static const char CT_TASKGET_PROPS[] = "\"taskId\": {\"type\": \"string\", \"description\": \"Id of the task\"}";
static const char CT_TASKGET_REQ[] = "\"taskId\"";
static void ct_taskget(const char *js, const char *end, char *out, size_t cap) {
    char *idtxt;
    const int id = ct_task_id_arg(js, end, out, cap, &idtxt);
    if (id >= 0) {
        ct_task_sync();
        const int i = ct_task_idx(id);
        if (i < 0) { char ids[200]; ct_task_ids(ids, sizeof ids); snprintf(out, cap, "error: no task with id %d. Valid ids: %s", id, ids); }
        else {
            const CtTask *t = &g_ct_tasks[i];
            snprintf(out, cap, "id: %d\nsubject: %s\nstatus: %s\nactiveForm: %s\ndescription: %s", t->id, t->subject, CT_ST_NAME[t->st], t->active, t->desc);
        }
    }
    free(idtxt);
}
