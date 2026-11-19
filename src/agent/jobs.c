/* ---------- comandos em segundo plano ----------
 * run_command com background=true inicia o comando (servidor, build longo) em seu proprio grupo de processos, com a
 * saida num arquivo, e devolve na hora o numero do trabalho. job(id, action) le o fim da saida, lista ou encerra.
 * Tudo que ainda roda e encerrado quando o agente sai. */
#ifndef _WIN32
#define JOB_N 8
typedef struct { int id; pid_t pid; int done, rc; char log[1300], cmd[200]; time_t t0; } Job;
static Job g_jobs[JOB_N];
static int g_job_seq, g_job_exit;

static void job_poll(Job *j) {
    int st;
    if (!j->id || j->done) return;
    if (waitpid(j->pid, &st, WNOHANG) == j->pid) { j->done = 1; j->rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0); }
}
static void job_kill(Job *j) {
    if (!j->id || j->done) return;
    kill(-j->pid, SIGTERM);
    for (int i = 0; i < 20 && !j->done; i++) { usleep(50000); job_poll(j); }
    if (!j->done) { kill(-j->pid, SIGKILL); int st; waitpid(j->pid, &st, 0); j->done = 1; j->rc = 137; }
}
static void job_all_kill(void) { for (int i = 0; i < JOB_N; i++) { job_kill(&g_jobs[i]); if (g_jobs[i].id) remove(g_jobs[i].log); } }
/* o fim do arquivo de saida (ate max bytes, a partir de um inicio de linha) */
static size_t job_tail(const Job *j, char *out, size_t max) {
    out[0] = 0;
    FILE *f = fopen(j->log, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f), from = sz > (long)max ? sz - (long)max : 0;
    fseek(f, from, SEEK_SET);
    size_t n = fread(out, 1, max, f);
    fclose(f);
    out[n] = 0;
    for (size_t i = 0; i < n; i++) if (!out[i] || out[i] == 1) out[i] = ' ';
    if (from) { char *nl = strchr(out, '\n'); if (nl && nl[1]) { n -= (size_t)(nl + 1 - out); memmove(out, nl + 1, n + 1); } }
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return n;
}
static void job_status(const Job *j, char *out, size_t cap) {
    if (j->done) snprintf(out, cap, "exited with code %d", j->rc);
    else snprintf(out, cap, "running for %ld s, pid %ld", (long)(time(NULL) - j->t0), (long)j->pid);
}

static void tool_job_start(const char *cmd, char *out, size_t cap) {
    if (sudo_word(cmd)) { snprintf(out, cap, "error: a background command cannot use sudo (there is no way to ask for the password later); run it without background=true"); return; }
    Job *j = NULL;
    for (int i = 0; i < JOB_N; i++) job_poll(&g_jobs[i]);
    for (int i = 0; i < JOB_N && !j; i++) if (!g_jobs[i].id) j = &g_jobs[i];
    if (!j) for (int i = 0; i < JOB_N; i++) if (g_jobs[i].done && (!j || g_jobs[i].id < j->id)) j = &g_jobs[i];      /* reaproveita o mais antigo que ja acabou */
    if (!j) { snprintf(out, cap, "error: %d background commands are already running; stop one with job(id, action=\"stop\")", JOB_N); return; }
    if (j->id) remove(j->log);
    char h[1024];
    agent_home(h, sizeof h);
    memset(j, 0, sizeof *j);
    snprintf(j->log, sizeof j->log, "%s/.katana/jobs/%ld-%d.log", h, (long)getpid(), g_job_seq + 1);
    mkdir_parents(j->log);
    const int lf = open(j->log, O_WRONLY | O_CREAT | O_TRUNC, 0600), dn = open("/dev/null", O_RDONLY);
    if (lf < 0) { if (dn >= 0) close(dn); snprintf(out, cap, "error: cannot create the output file %s", j->log); return; }
    fflush(stdout); fflush(stderr);
    const pid_t pid = vfork();
    if (pid < 0) { close(lf); if (dn >= 0) close(dn); snprintf(out, cap, "error: cannot start the command"); return; }
    if (pid == 0) {
        setsid();
        if (dn >= 0) dup2(dn, 0);
        dup2(lf, 1); dup2(lf, 2);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(lf); if (dn >= 0) close(dn);
    j->id = ++g_job_seq; j->pid = pid; j->t0 = time(NULL);
    snprintf(j->cmd, sizeof j->cmd, "%.190s", cmd);
    if (!g_job_exit) { atexit(job_all_kill); g_job_exit = 1; }
    for (int i = 0; i < 15 && !j->done; i++) { usleep(100000); job_poll(j); }      /* 1,5 s: pega a falha imediata */
    char st[120]; static char tl[2200];
    job_status(j, st, sizeof st);
    const size_t n = job_tail(j, tl, 2000);
    if (j->done) snprintf(out, cap, "the command already ended (%s); it is not running in the background.\n%s", st, n ? tl : "(no output)");
    else snprintf(out, cap, "started in the background as job %d (%s). Read its output later with job(id=%d); stop it with job(id=%d, action=\"stop\").\nOutput so far:\n%s", j->id, st, j->id, j->id, n ? tl : "(none yet)");
}

static void tool_job(const char *js, const char *end, char *out, size_t cap) {
    char act[32] = "";
    const int id = json_int(js, end, "id", 0);
    if (!json_str(js, end, "action", act, sizeof act) || !act[0]) snprintf(act, sizeof act, "%s", id ? "read" : "list");
    for (int i = 0; i < JOB_N; i++) job_poll(&g_jobs[i]);
    Job *j = NULL;
    for (int i = 0; i < JOB_N; i++) if (id && g_jobs[i].id == id) j = &g_jobs[i];
    if (strcmp(act, "list") == 0 || !j) {
        size_t o = 0; int n = 0;
        if (id && !j) o += (size_t)snprintf(out + o, cap - o, "error: there is no job %d. ", id);
        for (int i = 0; i < JOB_N; i++) if (g_jobs[i].id) { char st[120]; job_status(&g_jobs[i], st, sizeof st); o += (size_t)snprintf(out + o, cap - o, "%sjob %d: %s: %s", n++ ? "\n" : "Background commands:\n", g_jobs[i].id, st, g_jobs[i].cmd); if (o + 400 > cap) break; }
        if (!n) snprintf(out + o, cap - o, "no background commands (start one with run_command and background=true)");
        return;
    }
    if (strcmp(act, "stop") == 0) job_kill(j);
    else if (strcmp(act, "read") != 0) { snprintf(out, cap, "error: unknown action %s (read, stop, list)", act); return; }
    size_t L = g_budget < 4000 ? g_budget : 4000;
    if (L + 400 > cap) L = cap > 800 ? cap - 400 : 200;
    char st[120];
    job_status(j, st, sizeof st);
    size_t o = (size_t)snprintf(out, cap, "job %d (%s): %s\n", j->id, j->cmd, st);
    if (!job_tail(j, out + o, L)) snprintf(out + o, cap - o, "(no output)");
}
#else
static void tool_job_start(const char *cmd, char *out, size_t cap) { (void)cmd; snprintf(out, cap, "error: background commands are not available on Windows yet"); }
static void tool_job(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: background commands are not available on Windows yet"); }
#endif
