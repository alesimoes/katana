/* ---------- o agente do katana pela rede: /v1/agent/sessions ----------
 * katana serve <modelo> [porta] --agent. Um cliente remoto conversa com o agente (o laco que roda as ferramentas do
 * katana NESTA maquina: grava arquivos, executa comandos). O agente roda como processo filho do servidor (o mesmo
 * binario: katana agent <modelo> --sessao ARQ, com KATANA_UI=1) e a linha de marcadores que o desktop le no pipe
 * (\x01TOOL, \x01TODO, \x01CHOICE, \x01READY...) vira eventos SSE. Neste modo o servidor nao carrega o modelo (so o
 * filho carrega): /v1/chat/completions e /api/chat respondem 503. Um modelo na memoria = uma sessao ativa por vez
 * (409 quando ja existe outra ou quando ela esta ocupada).
 * Seguranca: quem tem a chave manda comandos na maquina. Fora do loopback o servidor nem sobe sem KATANA_API_KEY.
 * A senha do sudo nao e pedida pela rede: o pedido e recusado na hora. So Linux (fork/exec). */

#ifndef _WIN32
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
extern char **environ;

typedef struct {
    int active, busy, exited, waiting, resumed;  /* waiting: 1 pergunta (CHOICE), 2 permissao (ASK) */
    pid_t pid; int to_child, from_child;
    int at_line;                                 /* o proximo byte do filho comeca uma linha */
    char id[72], cwd[4096], file[4400];
    char question[4096];                         /* JSON do evento da pergunta pendente */
    Buf todo, in;                                /* lista de tarefas (JSON) e bytes do filho ainda nao tratados */
    int tok_used, tok_ctx, exit_code;
    time_t started;
} AgSess;
static AgSess g_ag;
static pthread_mutex_t g_ag_mu = PTHREAD_MUTEX_INITIALIZER;

static void ag_json(sock_t s, int code, const char *json) { http_reply(s, code, "application/json; charset=utf-8", json, strlen(json)); }

/* texto de um campo do marcador: \\n vira quebra, \\\\ vira barra (TOOLOUT, TOOLLIVE, TODO) */
static void ag_unesc(Buf *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) { i++; buf_add(o, s[i] == 'n' ? "\n" : s + i, 1); }
        else buf_add(o, s + i, 1);
    }
    buf_add(o, "", 0);
}

/* "chave":"valor" (valor escapado) */
static void ag_kv(Buf *b, const char *k, const char *v, size_t n, int comma) {
    if (comma) buf_str(b, ",");
    buf_str(b, "\""); buf_str(b, k); buf_str(b, "\":\""); js_escape(b, v, n); buf_str(b, "\"");
}

/* campo i (separado por tab) de a */
static int ag_field(const char *a, int i, const char **p, size_t *n) {
    const char *s = a;
    for (int k = 0; k < i; k++) { s = strchr(s, '\t'); if (!s) return 0; s++; }
    *p = s; *n = strcspn(s, "\t");
    return 1;
}

static void ag_write(const char *s, size_t n) {         /* para a entrada do filho (com g_ag_mu travado) */
    while (n > 0 && g_ag.to_child >= 0) {
        const ssize_t k = write(g_ag.to_child, s, n);
        if (k <= 0) break;
        s += k; n -= (size_t)k;
    }
}

/* uma linha de texto do cliente: sem quebras nem \x01 (o agente le uma linha por mensagem, como o desktop manda) */
static char *ag_line(const char *t) {
    size_t n = strlen(t), o = 0;
    char *r = xalloc(n + 2, 1);
    int sp = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char)t[i];
        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ' || ch == 1) { sp = o > 0; continue; }
        if (sp) { r[o++] = ' '; sp = 0; }
        r[o++] = (char)ch;
    }
    r[o] = 0;
    return r;
}

/* marcador do agente -> evento (tipo e JSON); atualiza o estado da sessao. 1 = fim do pedido (READY) */
static int ag_marker(const char *line, char *type, size_t tcap, Buf *js) {
    const size_t nl = strcspn(line, " ");
    char name[32]; snprintf(name, sizeof name, "%.*s", (int)(nl < 31 ? nl : 31), line);
    const char *a = line[nl] ? line + nl + 1 : "";
    const char *f; size_t fn;
    buf_clear(js);
    buf_str(js, "{");
    int done = 0;
    if (!strcmp(name, "READY")) { snprintf(type, tcap, "done"); done = 1; }
    else if (!strcmp(name, "TOOL")) {
        snprintf(type, tcap, "tool_call");
        ag_field(a, 0, &f, &fn); ag_kv(js, "name", f, fn, 0);
        if (ag_field(a, 1, &f, &fn)) ag_kv(js, "arguments", f, fn, 1);
        if (ag_field(a, 2, &f, &fn)) ag_kv(js, "description", f, fn, 1);
    } else if (!strcmp(name, "TOOLOUT")) {
        snprintf(type, tcap, "tool_result");
        ag_field(a, 0, &f, &fn); ag_kv(js, "name", f, fn, 0);
        Buf o = {0}; const char *t = strchr(a, '\t'); if (t) ag_unesc(&o, t + 1, strlen(t + 1));
        ag_kv(js, "output", o.p ? o.p : "", o.n, 1); free(o.p);
    } else if (!strcmp(name, "TOOLLIVE")) {
        snprintf(type, tcap, "tool_output");
        Buf o = {0}; ag_unesc(&o, a, strlen(a)); ag_kv(js, "text", o.p, o.n, 0); free(o.p);
    } else if (!strcmp(name, "TODO")) {
        snprintf(type, tcap, "todo");
        Buf o = {0}; ag_unesc(&o, a, strlen(a));
        buf_str(js, "\"items\":[");
        int k = 0;
        for (const char *p = o.p; p && *p; ) {
            const size_t l = strcspn(p, "\n");
            if (l) { if (k++) buf_str(js, ","); buf_str(js, "\""); js_escape(js, p, l); buf_str(js, "\""); }
            p += l; if (*p) p++;
        }
        buf_str(js, "]");
        free(o.p);
        buf_clear(&g_ag.todo); buf_add(&g_ag.todo, js->p + 1, js->n - 1);   /* "items":[...] */
    } else if (!strcmp(name, "CHOICE")) {
        snprintf(type, tcap, "question");
        ag_field(a, 0, &f, &fn); ag_kv(js, "question", f, fn, 0);
        buf_str(js, ",\"options\":[");
        for (int i = 1; ag_field(a, i, &f, &fn); i++) { if (i > 1) buf_str(js, ","); buf_str(js, "\""); js_escape(js, f, fn); buf_str(js, "\""); }
        buf_str(js, "]");
        g_ag.waiting = 1;
    } else if (!strcmp(name, "ASK")) {
        snprintf(type, tcap, "permission");
        ag_field(a, 0, &f, &fn); ag_kv(js, "kind", f, fn, 0);
        if (ag_field(a, 1, &f, &fn)) ag_kv(js, "what", f, fn, 1);
        if (ag_field(a, 2, &f, &fn)) ag_kv(js, "detail", f, fn, 1);
        g_ag.waiting = 2;
    } else if (!strcmp(name, "NEEDPASS")) {        /* senha do sudo nao vem pela rede: recusa ja */
        snprintf(type, tcap, "password_refused");
        buf_str(js, "\"message\":\"a sudo password was requested; passwords are not accepted over the API, so the command was refused\"");
        ag_write("\x01" "NOPASS\n", 8);
    } else if (!strcmp(name, "SECRET")) {          /* segredo pedido por um comando: tambem nao vem pela rede; cancela ja */
        snprintf(type, tcap, "secret_refused");
        buf_str(js, "\"message\":\"a command asked for a secret (password or code); secrets are not accepted over the API, so it was cancelled\"");
        char l[64]; const int n = snprintf(l, sizeof l, "\x01" "SECRETCANCEL %d\n", atoi(a));
        ag_write(l, (size_t)n);
    } else if (!strcmp(name, "MODE")) { snprintf(type, tcap, "mode"); ag_kv(js, "mode", a, strcspn(a, " \n"), 0); }
    else if (!strcmp(name, "CWD")) {
        snprintf(type, tcap, "cwd");
        ag_field(a, 0, &f, &fn); ag_kv(js, "path", f, fn, 0);
        snprintf(g_ag.cwd, sizeof g_ag.cwd, "%.*s", (int)fn, f);
        if (ag_field(a, 1, &f, &fn)) buf_str(js, *f == '1' ? ",\"trusted\":true" : ",\"trusted\":false");
    } else if (!strcmp(name, "TOKENS")) {
        snprintf(type, tcap, "tokens");
        sscanf(a, "%d %d", &g_ag.tok_used, &g_ag.tok_ctx);
        char t[96]; snprintf(t, sizeof t, "\"used\":%d,\"context\":%d", g_ag.tok_used, g_ag.tok_ctx); buf_str(js, t);
    } else if (!strcmp(name, "NOTIFY")) {
        snprintf(type, tcap, "notify");
        ag_field(a, 0, &f, &fn); ag_kv(js, "title", f, fn, 0);
        if (ag_field(a, 1, &f, &fn)) ag_kv(js, "message", f, fn, 1);
    } else if (!strcmp(name, "FILE")) {
        snprintf(type, tcap, "file");
        ag_field(a, 0, &f, &fn); ag_kv(js, "mode", f, fn, 0);
        if (ag_field(a, 1, &f, &fn)) ag_kv(js, "path", f, fn, 1);
        if (ag_field(a, 2, &f, &fn)) ag_kv(js, "caption", f, fn, 1);
    } else {                                       /* PREP, FEED, RESUME, COMPACT, TEACHER... */
        snprintf(type, tcap, "status");
        ag_kv(js, "marker", name, strlen(name), 0);
        ag_kv(js, "args", a, strlen(a), 1);
    }
    buf_str(js, "}");
    if (!strcmp(name, "CHOICE") || !strcmp(name, "ASK")) snprintf(g_ag.question, sizeof g_ag.question, "%s", js->p);   /* para o GET da sessao */
    return done;
}

static void ag_send(sock_t s, int *alive, const char *type, const char *js, size_t n) {
    if (!*alive) return;
    char h[96]; const int k = snprintf(h, sizeof h, "event: %s\ndata: ", type);
    *alive = sock_send(s, h, (size_t)k) && sock_send(s, js, n) && sock_send(s, "\n\n", 2);
}

/* le do filho ate o fim do pedido (READY) ou ate ele sair, mandando os eventos (s < 0: so consome).
   Texto da resposta sai em eventos "text"; as cores do terminal saem fora. 1 = READY, 0 = o filho saiu */
static int ag_pump(sock_t s, int *alive, int timeout_s) {
    Buf js = {0}, tx = {0};
    char type[32], rb[8192];
    const time_t t0 = time(NULL);
    int ret = 0;
    for (;;) {
        /* trata o que ja chegou */
        for (;;) {
            Buf *in = &g_ag.in;
            if (!in->n) break;
            if (g_ag.at_line && in->p[0] == 1) {           /* marcador: a linha inteira */
                char *nlp = memchr(in->p, '\n', in->n);
                if (!nlp) break;
                *nlp = 0;
                char *l = in->p + 1; size_t ll = strlen(l);
                while (ll && l[ll - 1] == '\r') l[--ll] = 0;
                pthread_mutex_lock(&g_ag_mu);
                const int done = ag_marker(l, type, sizeof type, &js);
                pthread_mutex_unlock(&g_ag_mu);
                const size_t used = (size_t)(nlp - in->p) + 1;
                memmove(in->p, in->p + used, in->n - used); in->n -= used; in->p[in->n] = 0;
                g_ag.at_line = 1;
                if (s >= 0) ag_send(s, alive, type, js.p, js.n);
                if (done) { ret = 1; goto out; }
                continue;
            }
            char *nlp = memchr(in->p, '\n', in->n);
            size_t take = nlp ? (size_t)(nlp - in->p) + 1 : in->n;
            if (!nlp) take = utf8_complete(in->p, take);
            /* sequencia de cor cortada no fim: espera o resto */
            for (size_t i = take > 8 ? take - 8 : 0; !nlp && i < take; i++) if (in->p[i] == 27) { int fin = 0; for (size_t j = i + 1; j < take; j++) if ((in->p[j] >= 'A' && in->p[j] <= 'Z') || (in->p[j] >= 'a' && in->p[j] <= 'z')) { fin = 1; break; } if (!fin) { take = i; break; } }
            if (!take) break;
            buf_clear(&tx);
            for (size_t i = 0; i < take; i++) {
                if (in->p[i] == 27 && i + 1 < take && in->p[i + 1] == '[') { i += 2; while (i < take && !((in->p[i] >= 'A' && in->p[i] <= 'Z') || (in->p[i] >= 'a' && in->p[i] <= 'z'))) i++; continue; }
                if (in->p[i] == '\r') continue;
                buf_add(&tx, in->p + i, 1);
            }
            g_ag.at_line = in->p[take - 1] == '\n';
            memmove(in->p, in->p + take, in->n - take); in->n -= take; in->p[in->n] = 0;
            if (tx.n && s >= 0) { buf_clear(&js); buf_str(&js, "{\"text\":\""); js_escape(&js, tx.p, tx.n); buf_str(&js, "\"}"); ag_send(s, alive, "text", js.p, js.n); }
        }
        if (timeout_s > 0 && time(NULL) - t0 > timeout_s) { ret = -1; goto out; }
        struct pollfd pf = { g_ag.from_child, POLLIN, 0 };
        const int pr = poll(&pf, 1, 1000);
        if (pr < 0 && errno != EINTR) goto out;
        if (pr <= 0) continue;
        const ssize_t k = read(g_ag.from_child, rb, sizeof rb);
        if (k <= 0) goto out;                          /* o filho saiu */
        buf_add(&g_ag.in, rb, (size_t)k);
    }
out:
    if (ret == 0) {                                    /* o agente terminou: registra como saiu */
        int st = 0;
        pthread_mutex_lock(&g_ag_mu);
        if (g_ag.pid > 0 && waitpid(g_ag.pid, &st, 0) == g_ag.pid) g_ag.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        g_ag.exited = 1; g_ag.pid = 0;
        pthread_mutex_unlock(&g_ag_mu);
        if (s >= 0) { char e[64]; const int k = snprintf(e, sizeof e, "{\"exit_code\":%d}", g_ag.exit_code); ag_send(s, alive, "exit", e, (size_t)k); }
    }
    free(js.p); free(tx.p);
    return ret;
}

/* fecha a sessao (com g_ag_mu travado): /exit, espera um pouco, depois mata o grupo */
static void ag_close_locked(void) {
    if (g_ag.pid > 0) {
        ag_write("/exit\n", 6);
        int st;
        for (int i = 0; i < 50 && waitpid(g_ag.pid, &st, WNOHANG) == 0; i++) usleep(100000);
        if (waitpid(g_ag.pid, &st, WNOHANG) == 0) { kill(-g_ag.pid, SIGKILL); waitpid(g_ag.pid, &st, 0); }
    }
    if (g_ag.to_child >= 0) close(g_ag.to_child);
    if (g_ag.from_child >= 0) close(g_ag.from_child);
    free(g_ag.todo.p); free(g_ag.in.p);
    memset(&g_ag, 0, sizeof g_ag);
    g_ag.to_child = g_ag.from_child = -1;
}

static void ag_state_json(Buf *b) {
    const char *st = !g_ag.active ? "none" : g_ag.exited ? "exited" : g_ag.waiting ? "waiting_answer" : g_ag.busy ? "busy" : "ready";
    char t[512];
    buf_str(b, "{\"id\":\""); js_escape(b, g_ag.id, strlen(g_ag.id));
    buf_str(b, "\",\"object\":\"agent.session\",\"state\":\""); buf_str(b, st);
    buf_str(b, "\",\"cwd\":\""); js_escape(b, g_ag.cwd, strlen(g_ag.cwd));
    buf_str(b, "\",\"session_file\":\""); js_escape(b, g_ag.file, strlen(g_ag.file));
    snprintf(t, sizeof t, "\",\"resumed\":%s,\"created\":%ld,\"tokens\":{\"used\":%d,\"context\":%d}", g_ag.resumed ? "true" : "false", (long)g_ag.started, g_ag.tok_used, g_ag.tok_ctx);
    buf_str(b, t);
    if (g_ag.exited) { snprintf(t, sizeof t, ",\"exit_code\":%d", g_ag.exit_code); buf_str(b, t); }
    buf_str(b, ",\"question\":"); buf_str(b, g_ag.waiting && g_ag.question[0] ? g_ag.question : "null");
    buf_str(b, ",\"todo\":"); if (g_ag.todo.n > 8) buf_add(b, g_ag.todo.p + 8, g_ag.todo.n - 8); else buf_str(b, "[]");
    buf_str(b, "}");
}

static int ag_id_ok(const char *s) {
    const size_t n = strlen(s);
    if (!n || n > 64) return 0;
    for (size_t i = 0; i < n; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '-')) return 0;
    return 1;
}

/* POST /v1/agent/sessions {"cwd": "...", "session_id": "..."}: sobe o agente e espera ele ficar pronto */
static void ag_create(sock_t s, const char *body) {
    char cwd[4096] = "", sid[128] = "";
    const char *p = js_ws(body ? body : "");
    if (*p == '{') { oa_str(g4_js_get(p, "cwd"), cwd, sizeof cwd); oa_str(g4_js_get(p, "session_id"), sid, sizeof sid); }
    if (!g_srv_model_path[0]) { oa_error(s, 400, "this server was started without a model (katana serve --agent has no model path); use the web page or start it with a model", "invalid_request_error"); return; }
        if (sid[0] && !ag_id_ok(sid)) { oa_error(s, 400, "session_id: 1 to 64 letters, digits, _ or -", "invalid_request_error"); return; }
    if (!cwd[0] && !getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "/");
    char rc[4096];
    struct stat sb;
    if (!realpath(cwd, rc) || stat(rc, &sb) != 0 || !S_ISDIR(sb.st_mode)) { oa_error(s, 400, "cwd: not an existing folder on the server", "invalid_request_error"); return; }
    pthread_mutex_lock(&g_ag_mu);
    if (g_ag.active && g_ag.exited && !g_ag.busy) ag_close_locked();      /* a anterior ja terminou: libera */
    if (g_ag.active) { pthread_mutex_unlock(&g_ag_mu); oa_error(s, 409, "an agent session is already active (one model in memory: one session at a time); DELETE it first", "conflict"); return; }
    memset(&g_ag, 0, sizeof g_ag);
    g_ag.to_child = g_ag.from_child = -1;
    g_ag.active = 1; g_ag.busy = 1;                   /* ocupada ate o primeiro READY */
    pthread_mutex_unlock(&g_ag_mu);
    if (!sid[0]) snprintf(sid, sizeof sid, "as_%08x%08x", (unsigned)rand(), (unsigned)time(NULL));
    snprintf(g_ag.id, sizeof g_ag.id, "%s", sid);
    snprintf(g_ag.cwd, sizeof g_ag.cwd, "%s", rc);
    const char *home = getenv("HOME") && *getenv("HOME") ? getenv("HOME") : "/tmp";
    char dir[4200], logp[4500];
    snprintf(dir, sizeof dir, "%s/.katana", home); mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/.katana/api-sessions", home); mkdir(dir, 0700);
    snprintf(g_ag.file, sizeof g_ag.file, "%s/%s.ksess", dir, sid);
    snprintf(logp, sizeof logp, "%s/%s.log", dir, sid);
    g_ag.resumed = access(g_ag.file, F_OK) == 0;
    /* ambiente do filho montado antes do fork: KATANA_UI=1, sem a chave nem o endereco do servidor */
    int ne = 0; while (environ[ne]) ne++;
    char **env = xalloc((size_t)ne + 2, sizeof(char *)); int k = 0;
    for (int i = 0; i < ne; i++) if (strncmp(environ[i], "KATANA_API_KEY=", 15) && strncmp(environ[i], "KATANA_HOST=", 12) && strncmp(environ[i], "KATANA_UI=", 10)) env[k++] = environ[i];
    env[k++] = "KATANA_UI=1"; env[k] = NULL;
    char exe[4096]; const ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[el > 0 ? el : 0] = 0;
    char *av[] = { exe, "agent", g_srv_model_path, g_ag.resumed ? "--retomar" : "--sessao", g_ag.file, NULL };
    int in[2], out[2];
    const int lf = open(logp, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (el <= 0 || pipe(in) != 0 || pipe(out) != 0) { free(env); if (lf >= 0) close(lf); pthread_mutex_lock(&g_ag_mu); ag_close_locked(); pthread_mutex_unlock(&g_ag_mu); oa_error(s, 500, "could not start the agent", "server_error"); return; }
    const pid_t pid = fork();
    if (pid == 0) {                                    /* filho: grupo proprio (SIGINT so para ele), pipes, pasta */
        setpgid(0, 0);
        dup2(in[0], 0); dup2(out[1], 1); if (lf >= 0) dup2(lf, 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        signal(SIGPIPE, SIG_DFL);
        if (chdir(rc) != 0) _exit(126);
        execve(exe, av, env);
        _exit(127);
    }
    free(env); if (lf >= 0) close(lf);
    close(in[0]); close(out[1]);
    pthread_mutex_lock(&g_ag_mu);
    g_ag.pid = pid; g_ag.to_child = in[1]; g_ag.from_child = out[0]; g_ag.at_line = 1; g_ag.started = time(NULL);
    pthread_mutex_unlock(&g_ag_mu);
    if (pid < 0) { pthread_mutex_lock(&g_ag_mu); g_ag.pid = 0; ag_close_locked(); pthread_mutex_unlock(&g_ag_mu); oa_error(s, 500, "could not start the agent (fork)", "server_error"); return; }
    int alive = 1;
    const int r = ag_pump(-1, &alive, 1800);           /* carga do modelo e aquecimento: ate o primeiro READY */
    pthread_mutex_lock(&g_ag_mu);
    g_ag.busy = 0;
    if (r != 1) {
        char m[600]; snprintf(m, sizeof m, "the agent did not start (%s); see %s", r < 0 ? "timeout" : "it exited", logp);
        ag_close_locked();
        pthread_mutex_unlock(&g_ag_mu);
        oa_error(s, 500, m, "server_error");
        return;
    }
    Buf b = {0}; ag_state_json(&b);
    pthread_mutex_unlock(&g_ag_mu);
    http_reply(s, 200, "application/json; charset=utf-8", b.p, b.n);
    free(b.p);
    fprintf(stderr, "[serve] agente: sessao %s em %s (pid %d%s)\n", g_ag.id, g_ag.cwd, (int)pid, g_ag.resumed ? ", retomada" : "");
}

/* POST .../messages {"content": "..."}: manda a mensagem e transmite os eventos ate o fim do pedido */
static void ag_message(sock_t s, const char *body) {
    Buf c = {0}; const char *why = NULL;
    const char *p = js_ws(body ? body : "");
    const char *cv = *p == '{' ? g4_js_get(p, "content") : NULL;
    if (!cv || !oa_content(cv, &c, &why) || !c.n) { free(c.p); oa_error(s, 400, why ? why : "content (text) is required", "invalid_request_error"); return; }
    char *line = ag_line(c.p); free(c.p);
    if (!line[0]) { free(line); oa_error(s, 400, "content is empty", "invalid_request_error"); return; }
    if (line[0] == '/') { free(line); oa_error(s, 400, "messages that start with / are agent commands and are not accepted over the API", "invalid_request_error"); return; }
    pthread_mutex_lock(&g_ag_mu);
    if (g_ag.exited) { pthread_mutex_unlock(&g_ag_mu); free(line); oa_error(s, 409, "the agent process has exited; DELETE the session and create a new one", "conflict"); return; }
    if (g_ag.busy) { pthread_mutex_unlock(&g_ag_mu); free(line); oa_error(s, 409, "the session is busy with another message (one request at a time)", "conflict"); return; }
    g_ag.busy = 1;
    ag_write(line, strlen(line)); ag_write("\n", 1);
    pthread_mutex_unlock(&g_ag_mu);
    free(line);
    const char *hd = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-store\r\n"
                     "X-Accel-Buffering: no\r\nConnection: close\r\n\r\n";
    int alive = sock_send(s, hd, strlen(hd));
    ag_pump(s, &alive, 0);                             /* cliente que sai: o pedido continua ate o fim (use interrupt) */
    pthread_mutex_lock(&g_ag_mu);
    g_ag.busy = 0; g_ag.waiting = 0; g_ag.question[0] = 0;
    pthread_mutex_unlock(&g_ag_mu);
}

/* POST .../answer {"answer": "..."}: resposta a uma pergunta (CHOICE) ou a um pedido de permissao (ASK) */
static void ag_answer(sock_t s, const char *body) {
    char ans[4096] = "";
    const char *p = js_ws(body ? body : "");
    const char *av = *p == '{' ? g4_js_get(p, "answer") : NULL;
    if (!av) { oa_error(s, 400, "answer is required", "invalid_request_error"); return; }
    if (*av == '"') oa_str(av, ans, sizeof ans);
    else snprintf(ans, sizeof ans, "%s", !strncmp(av, "true", 4) ? "yes" : "no");
    pthread_mutex_lock(&g_ag_mu);
    if (!g_ag.waiting) { pthread_mutex_unlock(&g_ag_mu); oa_error(s, 409, "no question is waiting for an answer", "conflict"); return; }
    if (g_ag.waiting == 2) {
        const int yes = !strcasecmp(ans, "yes") || !strcasecmp(ans, "y") || !strcmp(ans, "1") || !strcasecmp(ans, "true");
        ag_write(yes ? "\x01" "ANS 1\n" : "\x01" "ANS 0\n", 7);
    } else {
        char *l = ag_line(ans);
        ag_write("\x01" "ANS ", 5); ag_write(l, strlen(l)); ag_write("\n", 1);
        free(l);
    }
    g_ag.waiting = 0; g_ag.question[0] = 0;
    pthread_mutex_unlock(&g_ag_mu);
    ag_json(s, 200, "{\"ok\":true}");
}

static void ag_route(sock_t s, const char *method, const char *path, const char *head, size_t hn, size_t hend) {
    if (!g_srv_agent) { oa_error(s, 404, "the agent endpoint is offered only by katana serve <model> [port] --agent", "invalid_request_error"); return; }
    const char *rest = path + strlen("/v1/agent/sessions");
    if (strncmp(path, "/v1/agent/sessions", 18) != 0 || (*rest && *rest != '/')) { oa_error(s, 404, "unknown endpoint", "invalid_request_error"); return; }
    char val[64];
    char *body = NULL;
    if (!strcmp(method, "POST")) {
        if (hdr_get(head, "Content-Length", val, sizeof val)) { body = srv_body(s, head, hn, hend); if (!body) return; }
    }
    if (!*rest || !strcmp(rest, "/")) {                /* /v1/agent/sessions */
        if (!strcmp(method, "POST")) ag_create(s, body);
        else if (!strcmp(method, "GET")) {
            Buf b = {0}; buf_str(&b, "{\"object\":\"list\",\"data\":[");
            pthread_mutex_lock(&g_ag_mu); if (g_ag.active) ag_state_json(&b); pthread_mutex_unlock(&g_ag_mu);
            buf_str(&b, "]}"); http_reply(s, 200, "application/json; charset=utf-8", b.p, b.n); free(b.p);
        } else oa_error(s, 405, "use GET or POST", "invalid_request_error");
        free(body); return;
    }
    char id[80]; const char *sub;
    {   const char *q = rest + 1; const size_t l = strcspn(q, "/");
        snprintf(id, sizeof id, "%.*s", (int)(l < 79 ? l : 79), q); sub = q + l; }
    pthread_mutex_lock(&g_ag_mu);
    const int known = g_ag.active && !strcmp(id, g_ag.id);
    pthread_mutex_unlock(&g_ag_mu);
    if (!known) { oa_error(s, 404, "no such agent session", "invalid_request_error"); free(body); return; }
    if (!*sub || !strcmp(sub, "/")) {
        if (!strcmp(method, "GET")) { Buf b = {0}; pthread_mutex_lock(&g_ag_mu); ag_state_json(&b); pthread_mutex_unlock(&g_ag_mu); http_reply(s, 200, "application/json; charset=utf-8", b.p, b.n); free(b.p); }
        else if (!strcmp(method, "DELETE")) {
            pthread_mutex_lock(&g_ag_mu);
            if (g_ag.busy && !g_ag.exited) { pthread_mutex_unlock(&g_ag_mu); oa_error(s, 409, "the session is busy: interrupt it first", "conflict"); }
            else { ag_close_locked(); pthread_mutex_unlock(&g_ag_mu); ag_json(s, 200, "{\"deleted\":true}"); }
        } else oa_error(s, 405, "use GET or DELETE", "invalid_request_error");
    } else if (!strcmp(sub, "/messages")) {
        if (strcmp(method, "POST") != 0) oa_error(s, 405, "use POST", "invalid_request_error"); else ag_message(s, body);
    } else if (!strcmp(sub, "/answer")) {
        if (strcmp(method, "POST") != 0) oa_error(s, 405, "use POST", "invalid_request_error"); else ag_answer(s, body);
    } else if (!strcmp(sub, "/interrupt")) {
        if (strcmp(method, "POST") != 0) oa_error(s, 405, "use POST", "invalid_request_error");
        else {
            pthread_mutex_lock(&g_ag_mu);
            const int ok = g_ag.busy && g_ag.pid > 0;
            if (ok) kill(-g_ag.pid, SIGINT);           /* como o Ctrl+C do desktop: para a resposta atual */
            pthread_mutex_unlock(&g_ag_mu);
            if (ok) ag_json(s, 200, "{\"interrupted\":true}"); else oa_error(s, 409, "the session is not busy", "conflict");
        }
    } else oa_error(s, 404, "unknown endpoint", "invalid_request_error");
    free(body);
}
#else
static void ag_route(sock_t s, const char *method, const char *path, const char *head, size_t hn, size_t hend) {
    (void)method; (void)path; (void)head; (void)hn; (void)hend;
    oa_error(s, 501, "the agent endpoint is available on Linux only", "invalid_request_error");
}
#endif
