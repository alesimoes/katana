/* ---------- tabela de comandos em segundo plano ----------
 * O Bash com run_in_background=true inicia o comando (servidor, build longo) em seu proprio grupo de processos, com a
 * saida num arquivo, e devolve na hora o numero do trabalho (tools/bash.c); TaskStop o encerra. Tudo que ainda roda
 * e encerrado quando o agente sai. */
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

#endif
