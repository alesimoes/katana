/* ---------- ferramentas Bash, TaskStop, Monitor (ver docs/tools.md) ----------
 * Bash: cada comando roda num shell novo (bash, ou sh se nao houver), em novo grupo de processos, stdin /dev/null, com
 * stdout+stderr num ARQUIVO (~/.katana/jobs/bash-<pid>-<n>.log). O katana espera ate o timeout lendo o arquivo (a
 * interface ve a saida ao vivo); se o comando nao acabou, ele nao morre: entra na tabela de trabalhos de jobs.c (g_jobs)
 * e vira tarefa em segundo plano, com numero e arquivo. Assim nao e preciso "adotar" um processo que escreve num cano.
 * A pasta do shell persiste entre chamadas so para o Bash (g_ctb_cwd): o roteiro grava `pwd -P` ao sair (trap EXIT);
 * se a pasta final ficar fora da pasta de trabalho do agente (e das pastas liberadas), volta para a pasta de trabalho
 * e o resultado avisa. Variaveis de ambiente nao persistem.
 * TaskStop encerra uma tarefa (grupo inteiro). Monitor inicia um comando em segundo plano com prazo (padrao 5 min). */

#ifndef _WIN32
static char g_ctb_cwd[4200];       /* pasta do shell do Bash entre chamadas ("" = pasta de trabalho do agente) */
static int g_ctb_seq;

/* pasta de trabalho do agente (realpath) */
static void ctb_project(char *o, size_t cap) {
    char c[4200];
    if (!getcwd(c, sizeof c) || !realpath(c, o)) snprintf(o, cap, "%s", c);
    (void)cap;
}
/* 1 se d esta dentro da pasta de trabalho ou de uma pasta liberada pelo usuario */
static int ctb_allowed(const char *d) {
    char p[4200];
    ctb_project(p, sizeof p);
    if (path_under(p, d)) return 1;
    for (int i = 0; i < g_nroots; i++) if (path_under(g_roots[i], d)) return 1;
    for (int i = 0; i < g_nwroots; i++) if (path_under(g_wroots[i], d)) return 1;
    return 0;
}
/* pasta onde o proximo comando comeca */
static void ctb_start_dir(char *o, size_t cap) {
    struct stat st;
    if (g_ctb_cwd[0] && stat(g_ctb_cwd, &st) == 0 && S_ISDIR(st.st_mode) && ctb_allowed(g_ctb_cwd)) { snprintf(o, cap, "%s", g_ctb_cwd); return; }
    g_ctb_cwd[0] = 0;
    ctb_project(o, cap);
}
/* acrescenta s entre aspas simples (seguro para o shell) */
static size_t ctb_q(char *w, const char *s) {
    size_t n = 0;
    w[n++] = '\'';
    for (; *s; s++) { if (*s == '\'') { memcpy(w + n, "'\\''", 4); n += 4; } else w[n++] = *s; }
    w[n++] = '\'';
    w[n] = 0;
    return n;
}
static const char *ctb_shell(void) { return access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh"; }

/* inicia o roteiro em novo grupo de processos com a saida em log; devolve o pid ou -1 */
static pid_t ctb_spawn(const char *script, const char *log) {
    static char *envp[4096];                                 /* ambiente herdado + variaveis que evitam prompts (como run_cmd_ex) */
    int ne = 0;
    for (char **e = environ; e && *e && ne < 4090; e++) if (strncmp(*e, "GIT_TERMINAL_PROMPT=", 20) && strncmp(*e, "PAGER=", 6)) envp[ne++] = *e;
    envp[ne++] = (char *)"GIT_TERMINAL_PROMPT=0"; envp[ne++] = (char *)"PAGER=cat";
    static char pwenv[300], apenv[560];
    if (g_sudo_pw[0] && g_askpass_path[0]) {
        snprintf(pwenv, sizeof pwenv, "KATANA_PW=%s", g_sudo_pw); snprintf(apenv, sizeof apenv, "SUDO_ASKPASS=%s", g_askpass_path);
        envp[ne++] = pwenv; envp[ne++] = apenv;
    }
    envp[ne] = NULL;
    mkdir_parents(log);
    const int lf = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600), dn = open("/dev/null", O_RDONLY);
    if (lf < 0) { if (dn >= 0) close(dn); return -1; }
    const char *sh = ctb_shell();
    fflush(stdout); fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (dn >= 0) dup2(dn, 0);
        dup2(lf, 1); dup2(lf, 2);
        if (lf > 2) close(lf);
        if (dn > 2) close(dn);
        execve(sh, (char *const[]){ (char *)sh, (char *)"-c", (char *)script, NULL }, envp);
        _exit(127);
    }
    close(lf); if (dn >= 0) close(dn);
    return pid;
}
/* roteiro: entra na pasta, (opcional) grava a pasta final ao sair, funcao sudo -A, e o comando */
static char *ctb_script(const char *cmd, const char *dir, const char *cwdfile, int sudo) {
    char *s = (char *)malloc(strlen(cmd) + strlen(dir) * 4 + (cwdfile ? strlen(cwdfile) * 4 : 0) + 256);
    if (!s) return NULL;
    size_t n = 0;
    if (cwdfile) { n += (size_t)sprintf(s + n, "trap 'pwd -P > \"$__KT_CWD\" 2>/dev/null' EXIT\n__KT_CWD="); n += ctb_q(s + n, cwdfile); s[n++] = '\n'; }
    n += (size_t)sprintf(s + n, "cd -- "); n += ctb_q(s + n, dir); n += (size_t)sprintf(s + n, " || exit 1\n");
    if (sudo) n += (size_t)sprintf(s + n, "sudo() { command sudo -A \"$@\"; }\n");
    sprintf(s + n, "%s\n", cmd);
    return s;
}
/* um lugar na tabela de trabalhos (jobs.c); NULL se os JOB_N estao rodando */
static Job *ctb_slot(void) {
    Job *j = NULL;
    for (int i = 0; i < JOB_N; i++) job_poll(&g_jobs[i]);
    for (int i = 0; i < JOB_N && !j; i++) if (!g_jobs[i].id) j = &g_jobs[i];
    if (!j) for (int i = 0; i < JOB_N; i++) if (g_jobs[i].done && (!j || g_jobs[i].id < j->id)) j = &g_jobs[i];
    if (j && j->id) remove(j->log);
    return j;
}
static Job *ctb_adopt(pid_t pid, const char *log, const char *cmd, time_t t0) {
    Job *j = ctb_slot();
    if (!j) return NULL;
    memset(j, 0, sizeof *j);
    j->id = ++g_job_seq; j->pid = pid; j->t0 = t0;
    snprintf(j->log, sizeof j->log, "%s", log);
    snprintf(j->cmd, sizeof j->cmd, "%.190s", cmd);
    if (!g_job_exit) { atexit(job_all_kill); g_job_exit = 1; }
    return j;
}
static void ctb_logpath(char *o, size_t cap) {
    char h[1024];
    agent_home(h, sizeof h);
    snprintf(o, cap, "%s/.katana/jobs/bash-%ld-%d.log", h, (long)getpid(), ++g_ctb_seq);
}
/* le de f a partir de off ate n bytes, NUL vira espaco; devolve bytes lidos */
static size_t ctb_read(const char *path, long off, char *o, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) { o[0] = 0; return 0; }
    fseek(f, off, SEEK_SET);
    size_t k = fread(o, 1, n, f);
    fclose(f);
    for (size_t i = 0; i < k; i++) if (!o[i] || o[i] == 1) o[i] = ' ';
    o[k] = 0;
    return k;
}
static long ctb_size(const char *path) { struct stat st; return stat(path, &st) == 0 ? (long)st.st_size : 0; }
/* nao corta um caractere UTF-8 no meio: recua n ate um inicio de caractere */
static size_t ctb_utf8(const char *s, size_t n) { while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--; return n; }

/* palavra de comando do ultimo trecho (depois de |, ;, &&, ||), pulando VAR=valor */
static int ctb_exit1_ok(const char *cmd) {
    const char *seg = cmd;
    for (const char *q = cmd; *q; q++) if (*q == '|' || *q == ';' || *q == '&' || *q == '\n') seg = q + 1;
    for (;;) {
        while (*seg == ' ' || *seg == '\t' || *seg == '(' || *seg == '{') seg++;
        const size_t l = strcspn(seg, " \t");
        const char *eq = memchr(seg, '=', l);
        if (eq && eq > seg && l) { seg += l; continue; }
        break;
    }
    char w[32] = "", w2[32] = "";
    const size_t l = strcspn(seg, " \t\n)}");
    snprintf(w, sizeof w, "%.*s", (int)(l < 31 ? l : 31), seg);
    const char *s2 = seg + l; while (*s2 == ' ' || *s2 == '\t') s2++;
    const size_t l2 = strcspn(s2, " \t\n)}");
    snprintf(w2, sizeof w2, "%.*s", (int)(l2 < 31 ? l2 : 31), s2);
    const char *b = strrchr(w, '/');
    const char *name = b ? b + 1 : w;
    static const char *ok[] = { "grep", "rg", "egrep", "fgrep", "find", "diff", "test", "[", NULL };
    for (int i = 0; ok[i]; i++) if (strcmp(name, ok[i]) == 0) return 1;
    return strcmp(name, "git") == 0 && (strcmp(w2, "diff") == 0 || strcmp(w2, "grep") == 0);
}
static int ctb_is_sleep(const char *cmd) {
    while (*cmd == ' ' || *cmd == '\t' || *cmd == '\n') cmd++;
    return strncmp(cmd, "sleep", 5) == 0 && (cmd[5] == 0 || cmd[5] == ' ' || cmd[5] == '\t' || cmd[5] == ';' || cmd[5] == '\n');
}

/* inicia em segundo plano (run_in_background ou Monitor); prazo_s > 0 encerra o grupo depois desse tempo */
static void ctb_background(const char *cmd, int prazo_s, const char *tool, char *out, size_t cap) {
    if (sudo_word(cmd)) { snprintf(out, cap, "error: a background command cannot use sudo (there is no way to ask for the password later); run it in the foreground"); return; }
    for (int i = 0; i < JOB_N; i++) job_poll(&g_jobs[i]);
    int busy = 0;
    for (int i = 0; i < JOB_N; i++) if (g_jobs[i].id && !g_jobs[i].done) busy++;
    if (busy >= JOB_N) { snprintf(out, cap, "error: %d background tasks are already running; stop one with TaskStop(task_id)", JOB_N); return; }
    char dir[4200], log[1300];
    ctb_start_dir(dir, sizeof dir);
    ctb_logpath(log, sizeof log);
    char *script = ctb_script(cmd, dir, NULL, 0);
    if (!script) { snprintf(out, cap, "error: out of memory"); return; }
    if (prazo_s > 0) {                                       /* prazo: o proprio shell arma um vigia que encerra o grupo */
        char *s2 = (char *)malloc(strlen(script) + 200);
        if (!s2) { free(script); snprintf(out, cap, "error: out of memory"); return; }
        sprintf(s2, "( sleep %d; kill -TERM -$$ 2>/dev/null; sleep 3; kill -KILL -$$ 2>/dev/null ) >/dev/null 2>&1 &\n%s", prazo_s, script);
        free(script); script = s2;
    }
    const pid_t pid = ctb_spawn(script, log);
    free(script);
    if (pid < 0) { snprintf(out, cap, "error: cannot start the command (output file %s)", log); return; }
    Job *j = ctb_adopt(pid, log, cmd, time(NULL));
    if (!j) { kill(-pid, SIGKILL); waitpid(pid, NULL, 0); remove(log); snprintf(out, cap, "error: %d background tasks are already running; stop one with TaskStop(task_id)", JOB_N); return; }
    cmdlog_add(cmd, 0, "(started in the background)");
    if (prazo_s > 0) snprintf(out, cap, "%s started with ID: %d (stops after %d s). Output is being written to: %s\nRead it with the Read tool; stop it early with TaskStop(task_id=\"%d\").", tool, j->id, prazo_s, log, j->id);
    else snprintf(out, cap, "Command running in background with ID: %d. Output is being written to: %s\nRead it with the Read tool; stop it with TaskStop(task_id=\"%d\").", j->id, log, j->id);
}

static void ct_bash_run(const char *cmd, long long tmo_ms, char *out, size_t cap) {
    char dir[4200], log[1300], cwdf[1320];
    ctb_start_dir(dir, sizeof dir);
    ctb_logpath(log, sizeof log);
    snprintf(cwdf, sizeof cwdf, "%s.cwd", log);
    remove(cwdf);
    const int sudo = sudo_word(cmd) && sudo_prepare(cmd, 0);      /* modal/pergunta de senha, como no run_command */
    char *script = ctb_script(cmd, dir, cwdf, sudo);
    if (!script) { snprintf(out, cap, "error: out of memory"); return; }
    cmd_term_hook();
    const pid_t pid = ctb_spawn(script, log);
    free(script);
    if (pid < 0) { snprintf(out, cap, "error: cannot start the command (output file %s)", log); return; }
    const time_t t0w = time(NULL);
    g_fg_pid = pid;
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa); sa.sa_handler = cmd_on_sigint; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old);
    g_cmd_int = 0;
    g_ui_live = 1; g_live_total = 0;
    const double t0 = cm_now();
    int st = 0, exited = 0, timedout = 0, intr = 0;
    long seen = 0;
    char buf[8192];
    for (;;) {
        if (waitpid(pid, &st, WNOHANG) == pid) exited = 1;
        const long sz = ctb_size(log);                       /* saida nova para a interface ao vivo */
        while (seen < sz) { const size_t k = ctb_read(log, seen, buf, sizeof buf - 1); if (!k) break; ui_live(buf, k); if (g_cmd_tap) g_cmd_tap(buf, k); seen += (long)k; }
        if (exited) break;
        if (g_cmd_int) { intr = 1; break; }
        if ((cm_now() - t0) * 1000.0 >= (double)tmo_ms) { timedout = 1; break; }
        usleep(20000);
    }
    g_ui_live = 0;
    g_fg_pid = 0;
    sigaction(SIGINT, &old, NULL);
    char secs[32];
    snprintf(secs, sizeof secs, "%g", (double)tmo_ms / 1000.0);

    if (timedout && !ctb_is_sleep(cmd)) {                    /* passa para segundo plano em vez de matar */
        Job *j = ctb_adopt(pid, log, cmd, t0w);
        if (j) {
            remove(cwdf);
            size_t pv = cap > 900 ? cap - 700 : 200; if (pv > 2000) pv = 2000;
            char *tl = (char *)malloc(pv + 1);
            const size_t n = tl ? job_tail(j, tl, pv) : 0;
            snprintf(out, cap, "Command did not complete within its %ss timeout and was moved to the background with ID: %d. Output is being written to: %s\nRead it with the Read tool; stop it with TaskStop(task_id=\"%d\").\nOutput so far:\n%s",
                     secs, j->id, log, j->id, n ? tl : "(none yet)");
            free(tl);
            cmdlog_add(cmd, -1, "(moved to the background at the timeout)");
            return;
        }
    }
    if (!exited) {                                           /* sleep, Ctrl+C ou tabela cheia: encerra o grupo */
        kill(-pid, SIGTERM);
        for (int i = 0; i < 10 && !exited; i++) { usleep(30000); if (waitpid(pid, &st, WNOHANG) == pid) exited = 1; }
        kill(-pid, SIGKILL);
        if (!exited) waitpid(pid, &st, 0);
    }
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    if (timedout) rc = 124;

    /* pasta final do shell */
    char note[4400] = "";
    {
        char nd[4200] = "";
        size_t k = ctb_read(cwdf, 0, nd, sizeof nd - 1);
        while (k && (nd[k - 1] == '\n' || nd[k - 1] == '\r')) nd[--k] = 0;
        remove(cwdf);
        if (k && nd[0] == '/') {
            char pj[4200]; ctb_project(pj, sizeof pj);
            if (ctb_allowed(nd)) snprintf(g_ctb_cwd, sizeof g_ctb_cwd, "%s", strcmp(nd, pj) == 0 ? "" : nd);
            else { g_ctb_cwd[0] = 0; snprintf(note, sizeof note, "\nShell cwd was reset to %s", pj); }
        }
    }

    /* limites: sucesso ate 30000, falha ate 10000, sempre dentro de cap */
    const int fail = (rc != 0 && !(rc == 1 && ctb_exit1_ok(cmd))) || timedout || intr;
    const size_t reserve = 1200 + strlen(note) + strlen(log);
    const size_t room = cap > reserve + 400 ? cap - reserve : 400;
    size_t lim = fail ? 10000 : 30000;
    if (lim > room) lim = room;
    const long sz = ctb_size(log);
    char *body = (char *)malloc(lim + 64);
    if (!body) { snprintf(out, cap, "error: out of memory"); return; }
    int keep = 0;
    size_t o = 0;
    if ((size_t)sz <= lim) {
        size_t n = ctb_read(log, 0, body, (size_t)sz);
        while (n && (body[n - 1] == '\n' || body[n - 1] == '\r')) body[--n] = 0;
        o += (size_t)snprintf(out + o, cap - o, "%s", n ? body : "(no output)");
    } else if (!fail) {                                      /* sucesso grande: caminho do arquivo + inicio */
        keep = 1;
        size_t pv = lim < 2000 ? lim : 2000;
        size_t n = ctb_read(log, 0, body, pv);
        n = ctb_utf8(body, n); body[n] = 0;
        o += (size_t)snprintf(out + o, cap - o, "Output too large (%ld bytes). Full output saved to: %s\nPreview (first %zu bytes):\n%s\n...", sz, log, n, body);
    } else {                                                 /* falha grande: inicio e fim */
        keep = 1;
        const size_t half = lim / 2;
        size_t n = ctb_read(log, 0, body, half);
        n = ctb_utf8(body, n); body[n] = 0;
        o += (size_t)snprintf(out + o, cap - o, "%s\n[... %ld bytes omitted; full output saved to: %s ...]\n", body, sz - (long)(2 * half), log);
        n = ctb_read(log, sz - (long)half, body, half);
        size_t s = 0; while (s < n && ((unsigned char)body[s] & 0xC0) == 0x80) s++;
        while (n && (body[n - 1] == '\n' || body[n - 1] == '\r')) body[--n] = 0;
        if (o < cap) o += (size_t)snprintf(out + o, cap - o, "%s", body + s);
    }
    if (o >= cap) o = cap - 1;
    if (timedout) o += (size_t)snprintf(out + o, cap - o, "\nCommand timed out after %ss and was killed (%s)", secs, ctb_is_sleep(cmd) ? "commands starting with sleep are not moved to the background" : "no free background slot; stop a task with TaskStop");
    if (intr && o < cap) o += (size_t)snprintf(out + o, cap - o, "\n[interrupted by the user]");
    if (rc != 0 && o < cap) o += (size_t)snprintf(out + o, cap - o, "\nExit code %d", rc);
    if (o < cap) o += (size_t)snprintf(out + o, cap - o, "%s", note);
    /* dica (diagnose.c) e anel de comandos usam o fim da saida */
    {
        size_t n = ctb_read(log, sz > 4000 ? sz - 4000 : 0, body, lim < 4000 ? lim : 4000);
        body[n] = 0;
        if (fail && o < cap) cmd_hint(rc, timedout, body, cmd, out + o, cap - o);
        cmdlog_add(cmd, rc, body);
    }
    free(body);
    if (!keep) remove(log);
}

static const char CT_BASH_DESC[] =
    "Run a shell command (bash) and return its output (stdout and stderr together). Each call starts a new shell: "
    "environment variables do not persist, but the working directory does (a `cd` carries over to the next call while it "
    "stays inside the project; otherwise it is reset). stdin is closed, so interactive programs fail instead of waiting. "
    "timeout is in milliseconds (default 120000, max 600000); a command still running at the timeout is moved to the "
    "background and you get its task ID and output file. Use run_in_background for servers and long jobs; read the output "
    "file with Read and stop it with TaskStop. Large output is saved to a file and you get its path and a preview. "
    "Prefer Read, Edit, Glob and Grep over cat, sed, find and grep for files.";
static const char CT_BASH_PROPS[] =
    "\"command\": {\"type\": \"string\", \"description\": \"The command to execute\"}, "
    "\"timeout\": {\"type\": \"number\", \"description\": \"Optional timeout in milliseconds (max 600000)\"}, "
    "\"description\": {\"type\": \"string\", \"description\": \"Clear, concise description of what this command does, in 5-10 words\"}, "
    "\"run_in_background\": {\"type\": \"boolean\", \"description\": \"Set to true to run this command in the background; read its output file later with Read\"}";
static const char CT_BASH_REQ[] = "\"command\"";
static void ct_bash(const char *js, const char *end, char *out, size_t cap) {
    char *cmd = ct_str(js, end, "command");
    if (!cmd || !cmd[strspn(cmd, " \t\r\n")]) { free(cmd); snprintf(out, cap, "error: missing command (the shell command to run)"); return; }
    long long tmo = ct_int(js, end, "timeout", -1);
    if (tmo <= 0) { const char *e = getenv("KATANA_CMD_TIMEOUT"); tmo = e && *e && atoi(e) > 0 ? (long long)atoi(e) * 1000 : 120000; }
    if (tmo > 600000) tmo = 600000;
    char *desc = ct_str(js, end, "description");
    char what[300];
    if (desc && *desc) snprintf(what, sizeof what, "executar o comando (%.200s)", desc);
    else snprintf(what, sizeof what, "executar o comando");
    free(desc);
    if (!agent_act(what, cmd)) { free(cmd); snprintf(out, cap, "error: the user denied this action"); return; }
    if (ct_bool(js, end, "run_in_background")) ctb_background(cmd, 0, "Command", out, cap);
    else ct_bash_run(cmd, tmo, out, cap);
    free(cmd);
}

static const char CT_TASKSTOP_DESC[] =
    "Stop a background task started by Bash (run_in_background, or moved to the background at its timeout) or by Monitor. "
    "The whole process group is terminated. Its output file stays readable with Read.";
static const char CT_TASKSTOP_PROPS[] = "\"task_id\": {\"type\": \"string\", \"description\": \"The ID of the background task to stop\"}";
static const char CT_TASKSTOP_REQ[] = "\"task_id\"";
static void ct_taskstop(const char *js, const char *end, char *out, size_t cap) {
    char *id = ct_str(js, end, "task_id");
    if (!id) id = ct_str(js, end, "shell_id");               /* nome antigo do argumento */
    const char *d = id; while (d && *d && !isdigit((unsigned char)*d)) d++;
    const int n = d && *d ? atoi(d) : 0;
    free(id);
    for (int i = 0; i < JOB_N; i++) job_poll(&g_jobs[i]);
    Job *j = NULL;
    for (int i = 0; i < JOB_N; i++) if (n && g_jobs[i].id == n) j = &g_jobs[i];
    if (!j) {
        size_t o = (size_t)snprintf(out, cap, n ? "error: there is no task %d." : "error: missing task_id.", n);
        int k = 0;
        for (int i = 0; i < JOB_N && o + 300 < cap; i++) if (g_jobs[i].id && !g_jobs[i].done) o += (size_t)snprintf(out + o, cap - o, "%s %d (%s)", k++ ? "," : " Running tasks:", g_jobs[i].id, g_jobs[i].cmd);
        if (!k && o < cap) snprintf(out + o, cap - o, " No background task is running.");
        return;
    }
    if (j->done) { snprintf(out, cap, "error: task %d is not running (it exited with code %d). Its output is in %s", j->id, j->rc, j->log); return; }
    job_kill(j);
    snprintf(out, cap, "Successfully stopped task: %d (%s). Output file: %s", j->id, j->cmd, j->log);
}

static const char CT_MONITOR_DESC[] =
    "Start a long-running command in the background to watch its output (logs, a test watcher, a server). It stops by itself "
    "after timeout_ms (default 300000, max 1800000). Returns a task ID and the output file: read new lines with Read on that "
    "file, stop it early with TaskStop. Use grep --line-buffered in pipes so lines are not held back.";
static const char CT_MONITOR_PROPS[] =
    "\"command\": {\"type\": \"string\", \"description\": \"The command whose output lines you want to watch\"}, "
    "\"description\": {\"type\": \"string\", \"description\": \"Short description of what is being watched\"}, "
    "\"timeout_ms\": {\"type\": \"number\", \"description\": \"Stop after this many milliseconds (default 300000, max 1800000)\"}";
static const char CT_MONITOR_REQ[] = "\"command\", \"description\"";
static void ct_monitor(const char *js, const char *end, char *out, size_t cap) {
    char *cmd = ct_str(js, end, "command");
    if (!cmd || !cmd[strspn(cmd, " \t\r\n")]) { free(cmd); snprintf(out, cap, "error: missing command (the command whose output to watch)"); return; }
    long long ms = ct_int(js, end, "timeout_ms", 300000);
    if (ms <= 0) ms = 300000;
    if (ms > 1800000) ms = 1800000;
    char *desc = ct_str(js, end, "description");
    char what[300];
    snprintf(what, sizeof what, "monitorar o comando%s%.200s%s", desc && *desc ? " (" : "", desc ? desc : "", desc && *desc ? ")" : "");
    free(desc);
    if (!agent_act(what, cmd)) { free(cmd); snprintf(out, cap, "error: the user denied this action"); return; }
    ctb_background(cmd, (int)((ms + 999) / 1000), "Monitor", out, cap);
    free(cmd);
}
#else
static const char CT_BASH_DESC[] = "Run a shell command (not available on Windows yet)";
static const char CT_BASH_PROPS[] = "\"command\": {\"type\": \"string\"}, \"timeout\": {\"type\": \"number\"}, \"description\": {\"type\": \"string\"}, \"run_in_background\": {\"type\": \"boolean\"}";
static const char CT_BASH_REQ[] = "\"command\"";
static void ct_bash(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: Bash is not available on Windows yet; use run_command"); }
static const char CT_TASKSTOP_DESC[] = "Stop a background task (not available on Windows yet)";
static const char CT_TASKSTOP_PROPS[] = "\"task_id\": {\"type\": \"string\"}";
static const char CT_TASKSTOP_REQ[] = "\"task_id\"";
static void ct_taskstop(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: TaskStop is not available on Windows yet"); }
static const char CT_MONITOR_DESC[] = "Watch a command's output (not available on Windows yet)";
static const char CT_MONITOR_PROPS[] = "\"command\": {\"type\": \"string\"}, \"description\": {\"type\": \"string\"}, \"timeout_ms\": {\"type\": \"number\"}";
static const char CT_MONITOR_REQ[] = "\"command\", \"description\"";
static void ct_monitor(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: Monitor is not available on Windows yet"); }
#endif
