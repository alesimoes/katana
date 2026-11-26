/* ---------- tabela de comandos em segundo plano ----------
 * O Bash com run_in_background=true inicia o comando (servidor, build longo) em seu proprio grupo de processos, com a
 * saida num arquivo, e devolve na hora o numero do trabalho (tools/bash.c); TaskStop o encerra. Tudo que ainda roda
 * e encerrado quando o agente sai. Todo comando roda num pseudo-terminal: um processo copiador (pump) passa o que o
 * terminal escreve para o arquivo de saida, sem as sequencias de escape; o katana guarda a ponta mestre para o SendInput. */
#ifndef _WIN32
#define JOB_N 8
/* mfd: ponta mestre do pseudo-terminal da tarefa, guardada como fd+1 (0 = sem terminal). Fecha quando a tarefa acaba ou e
 * encerrada. pump: o copiador do terminal para o arquivo. slave: nome do terminal (/dev/pts/N). asked: quando o katana
 * devolveu "waiting for input" ao modelo (0 = nao esta esperando); asked_sz: tamanho da saida naquele momento. Uma pergunta
 * sem resposta e sem saida nova por KATANA_PROMPT_MAX segundos (padrao 900) encerra a tarefa (job_poll). asked_label: o texto da
 * ultima pergunta mostrada ao modelo; typed: o modelo escreveu uma resposta depois dela; asked_rep: quantas vezes a MESMA
 * pergunta voltou depois de uma resposta do modelo (na segunda o katana pergunta ao usuario). */
typedef struct { int id; pid_t pid, pump; int done, rc, mfd; char log[1300], cmd[200], slave[64]; time_t t0, asked; long asked_sz; char asked_label[240]; int typed, asked_rep; } Job;
static Job g_jobs[JOB_N];
static int g_job_seq, g_job_exit;

/* copiadores cujo comando ja acabou mas que ainda nao sairam (um filho em segundo plano segura o terminal): recolhidos depois */
static pid_t g_pumps[32];
static void pump_reap(void) { for (int i = 0; i < 32; i++) if (g_pumps[i] > 0 && waitpid(g_pumps[i], NULL, WNOHANG) != 0) g_pumps[i] = 0; }
static void pump_later(pid_t p) {
    if (p <= 0) return;
    pump_reap();
    for (int i = 0; i < 32; i++) if (!g_pumps[i]) { g_pumps[i] = p; return; }
}
static int job_prompt_max(void) { const char *e = getenv("KATANA_PROMPT_MAX"); return e && atoi(e) > 0 ? atoi(e) : 900; }
static void job_close_in(Job *j) { if (j->mfd > 0) { close(j->mfd - 1); j->mfd = 0; } if (j->pump > 0) { pump_later(j->pump); j->pump = 0; } }
static void job_kill(Job *j);
static void job_poll(Job *j) {
    int st;
    if (!j->id || j->done) { if (j->id) job_close_in(j); return; }
    if (waitpid(j->pid, &st, WNOHANG) == j->pid) { j->done = 1; j->rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0); job_close_in(j); return; }
    if (j->asked) {                                          /* pergunta sem resposta: limite de espera */
        struct stat sb;
        const long sz = stat(j->log, &sb) == 0 ? (long)sb.st_size : 0;
        if (sz != j->asked_sz) j->asked = 0;
        else if (time(NULL) - j->asked > job_prompt_max()) { j->asked = 0; job_kill(j); }
    }
}
static void job_kill(Job *j) {
    if (!j->id) return;
    job_close_in(j);                                         /* solta a ponta mestre do terminal antes do sinal */
    if (j->done) return;
    kill(-j->pid, SIGTERM);
    for (int i = 0; i < 20 && !j->done; i++) { usleep(50000); job_poll(j); }
    if (!j->done) { kill(-j->pid, SIGKILL); int st; waitpid(j->pid, &st, 0); j->done = 1; j->rc = 137; }
}
static int rz_keep_logs(void);      /* resume.c: com o estado da sessao em arquivo, a saida das tarefas fica (o texto do estado aponta para ela) */
static void job_all_kill(void) { for (int i = 0; i < JOB_N; i++) { job_kill(&g_jobs[i]); if (g_jobs[i].id && !rz_keep_logs()) remove(g_jobs[i].log); } }
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
