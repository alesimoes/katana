/* ---------- tabela de comandos em segundo plano ----------
 * O Bash com run_in_background=true inicia o comando (servidor, build longo) em seu proprio grupo de processos, com a
 * saida num arquivo, e devolve na hora o numero do trabalho (tools/bash.c); TaskStop o encerra. Tudo que ainda roda
 * e encerrado quando o agente sai. */
#ifndef _WIN32
#define JOB_N 8
/* inw: ponta de escrita do cano ligado ao stdin da tarefa, guardada como fd+1 (0 = sem cano: comando que comecou em primeiro
 * plano, com stdin /dev/null, e foi movido para segundo plano no timeout). Fecha quando a tarefa acaba ou e encerrada. */
typedef struct { int id; pid_t pid; int done, rc, inw; char log[1300], cmd[200]; time_t t0; } Job;
static Job g_jobs[JOB_N];
static int g_job_seq, g_job_exit;

static void job_close_in(Job *j) { if (j->inw > 0) { close(j->inw - 1); j->inw = 0; } }
static void job_poll(Job *j) {
    int st;
    if (!j->id || j->done) { if (j->id) job_close_in(j); return; }
    if (waitpid(j->pid, &st, WNOHANG) == j->pid) { j->done = 1; j->rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0); job_close_in(j); }
}
static void job_kill(Job *j) {
    if (!j->id) return;
    job_close_in(j);                                         /* EOF no stdin antes do sinal */
    if (j->done) return;
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

/* argumentos de uma chamada para mostrar ou gravar (marcador TOOL, log de acoes, fluxo do modo sem cabeca): o texto que o
 * SendInput escreve no stdin de uma tarefa pode ser senha ou codigo, entao vira "***". Outras ferramentas: devolve ap como
 * esta. Em JSON ("text": "...") e em XML (<parameter=text>...</parameter>). *pend recebe o fim do texto devolvido. */
static const char *job_redact(const char *name, const char *ap, const char *end, char *buf, size_t cap, const char **pend) {
    *pend = end;
    if (!name || strcmp(name, "SendInput") != 0 || !ap || end <= ap || cap < 32) return ap;
    size_t o = 0;
    const char *p = ap;
    while (p < end && o + 24 < cap) {
        if (*p == '"' && end - p >= 6 && !strncmp(p, "\"text\"", 6)) {
            const char *q = p + 6;
            while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r' || *q == ':')) q++;
            if (q < end && *q == '"') {
                q++;
                while (q < end && *q != '"') { if (*q == '\\' && q + 1 < end) q++; q++; }
                if (q < end) q++;
                o += (size_t)snprintf(buf + o, cap - o, "\"text\": \"***\"");
                p = q;
                continue;
            }
        }
        if (*p == '<' && end - p >= 16 && !strncmp(p, "<parameter=text>", 16)) {
            const char *q = p + 16;
            while (q + 12 <= end && strncmp(q, "</parameter>", 12)) q++;
            o += (size_t)snprintf(buf + o, cap - o, "<parameter=text>***");
            p = q + 12 <= end ? q : end;
            continue;
        }
        buf[o++] = *p++;
    }
    if (o >= cap) o = cap - 1;
    buf[o] = 0;
    *pend = buf + o;
    return buf;
}
