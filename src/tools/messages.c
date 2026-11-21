/* ---------- SendMessage, ListAgents, SubagentHandback (nomes e argumentos do Claude Code) ----------
 * Aqui ha um modelo so e um contexto so no processo: o subagente (delegate.c, ferramenta Agent) roda em sequencia,
 * com contexto novo, e so a resposta final volta. Nao existe agente em segundo plano.
 *
 * Registro: cada subagente rodado nesta sessao (Agent, SendMessage, Workflow) fica numa lista (id agent-N, tarefa,
 * ultima resposta). O delegate apaga o cache KV do subagente ao terminar, entao o contexto inteiro dele nao pode ser
 * retomado: SendMessage continua um subagente rodando outro com a tarefa original, a resposta anterior e a mensagem
 * nova ("replay"), e diz isso no resultado.
 *
 * Caixa de correio entre sessoes do katana na mesma maquina: ~/.katana/mail/
 *     agents/<pid>          registro de uma sessao (linha 1: nome; linha 2: pasta de trabalho)
 *     <pid>/<tempo>-<pid de quem mandou>-<n>.msg    uma mensagem por arquivo (linha 1: "from: ..."; depois o texto)
 * O laco da sessao chama ct_mail_register() ao comecar, ct_mail_take() quando esta ocioso e ct_mail_unregister()
 * ao sair. Registros de pid morto sao apagados ao listar ou enviar.
 *
 * Teste sem modelo: todo subagente destes arquivos passa por g_ct_sub_run; KATANA_AGENT_FAKE=1 troca por uma
 * resposta deterministica. */

#ifndef _WIN32
#include <dirent.h>
#include <signal.h>
#include <errno.h>
#endif

/* ---- execucao de um subagente: caminho real (o mesmo da ferramenta Agent) ou falso (testes) ---- */
static char g_ct_handback[4000];        /* relatorio entregue por SubagentHandback dentro do subagente */
static int g_ct_handback_set;
static void ct_subagenthandback(const char *js, const char *end, char *out, size_t cap);

static void ct_sub_real(const char *prompt, char *out, size_t cap) {
    const size_t n = strlen(prompt);
    char *a = (char *)malloc(n * 2 + 32); size_t o = 0;
    o += (size_t)sprintf(a + o, "{\"prompt\": \"");
    for (size_t i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char)prompt[i];
        if (ch == '"' || ch == '\\') { a[o++] = '\\'; a[o++] = (char)ch; }
        else if (ch == '\n') { a[o++] = '\\'; a[o++] = 'n'; }
        else if (ch < 0x20) a[o++] = ' ';
        else a[o++] = (char)ch;
    }
    o += (size_t)sprintf(a + o, "\"}");
    ct_agent(a, a + o, out, cap);
    free(a);
}
/* falso: "fake: <60 primeiros caracteres>"; FAIL no prompt da erro; HANDBACK chama SubagentHandback de dentro */
static void ct_sub_fake(const char *prompt, char *out, size_t cap) {
    if (strstr(prompt, "FAIL")) { snprintf(out, cap, "error: fake sub-agent failure"); return; }
    if (strstr(prompt, "HANDBACK")) {
        char js[600], r[200];
        snprintf(js, sizeof js, "{\"message\": \"handed back: %.60s\"}", prompt);
        for (char *q = js + 13; *q && q < js + strlen(js) - 2; q++) if (*q == '"' || *q == '\\' || (unsigned char)*q < 0x20) *q = ' ';
        g_in_sub = 1;
        ct_subagenthandback(js, js + strlen(js), r, sizeof r);
        g_in_sub = 0;
    }
    if (g_ct_handback_set) { snprintf(out, cap, "[sub-agent answer; 0 tool calls]\n%s", g_ct_handback); return; }
    char b[64]; size_t l = 0;
    for (const char *p = prompt; *p && l < 60; p++) b[l++] = *p == '\n' ? ' ' : *p;
    b[l] = 0;
    snprintf(out, cap, "[sub-agent answer; 0 tool calls]\nfake: %s", b);
}
static void (*g_ct_sub_run)(const char *prompt, char *out, size_t cap);

/* roda um subagente; 1 se respondeu (resposta sem o cabecalho em ans), 0 se falhou (erro em ans) */
static int ct_sub_call(const char *prompt, char *ans, size_t cap) {
    if (!g_ct_sub_run) { const char *f = getenv("KATANA_AGENT_FAKE"); g_ct_sub_run = (f && atoi(f) == 1) ? ct_sub_fake : ct_sub_real; }
    g_ct_handback_set = 0; g_ct_handback[0] = 0;
    char *tmp = (char *)malloc(cap + 1);
    tmp[0] = 0;
    g_ct_sub_run(prompt, tmp, cap);
    const int ok = strncmp(tmp, "error", 5) != 0;
    const char *p = tmp;
    if (ok && strncmp(p, "[sub-agent answer", 17) == 0) { const char *nl = strchr(p, '\n'); p = nl ? nl + 1 : p + strlen(p); }
    /* relatorio entregue por SubagentHandback vale como resposta final */
    if (g_ct_handback_set && ok) p = g_ct_handback;
    snprintf(ans, cap, "%s", p);
    free(tmp);
    g_ct_handback_set = 0;
    return ok;
}

/* ---- registro dos subagentes desta sessao ---- */
#define CT_AG_MAX 64
typedef struct { int id; char *task, *report; int ok, turns; } CtAg;
static CtAg g_ct_ag[CT_AG_MAX];
static int g_ct_ag_n, g_ct_ag_next = 1;
/* anota um subagente terminado; devolve o id. Chamado pela ferramenta Agent (ver o relatorio) e por estes arquivos */
static int ct_agents_note(const char *task, const char *report) {
    if (g_ct_ag_n == CT_AG_MAX) { free(g_ct_ag[0].task); free(g_ct_ag[0].report); memmove(g_ct_ag, g_ct_ag + 1, sizeof(CtAg) * (CT_AG_MAX - 1)); g_ct_ag_n--; }
    const char *r = report ? report : "";
    const int ok = strncmp(r, "error", 5) != 0;
    if (ok && strncmp(r, "[sub-agent answer", 17) == 0) { const char *nl = strchr(r, '\n'); r = nl ? nl + 1 : r + strlen(r); }
    g_ct_ag[g_ct_ag_n] = (CtAg){ g_ct_ag_next++, strdup(task ? task : ""), strdup(r), ok, 1 };
    return g_ct_ag[g_ct_ag_n++].id;
}
static CtAg *ct_agents_find(const char *to) {
    int id = 0;
    if (strncmp(to, "agent-", 6) == 0) id = atoi(to + 6);
    else if (isdigit((unsigned char)to[0]) && strlen(to) < 4) id = atoi(to);
    for (int i = 0; i < g_ct_ag_n && id; i++) if (g_ct_ag[i].id == id) return &g_ct_ag[i];
    return NULL;
}

/* ---- caixa de correio entre sessoes ---- */
#ifndef _WIN32
static int ct_mail_dir(char *o, size_t cap, const char *sub) {
    const char *h = getenv("HOME");
    if (!h || !*h) return 0;
    char b[1024];
    snprintf(b, sizeof b, "%s/.katana", h); mkdir(b, 0700);
    snprintf(b, sizeof b, "%s/.katana/mail", h); mkdir(b, 0700);
    if (sub) { snprintf(o, cap, "%s/%s", b, sub); mkdir(o, 0700); } else snprintf(o, cap, "%s", b);
    return 1;
}
static int ct_pid_alive(long pid) { return pid > 0 && (kill((pid_t)pid, 0) == 0 || errno != ESRCH); }
/* apaga registro e caixa de um pid */
static void ct_mail_drop(long pid) {
    char d[1100], f[1400];
    if (!ct_mail_dir(d, sizeof d, NULL)) return;
    snprintf(f, sizeof f, "%s/agents/%ld", d, pid); remove(f);
    snprintf(f, sizeof f, "%s/%ld", d, pid);
    DIR *dd = opendir(f);
    if (dd) {
        struct dirent *e; char x[1700];
        while ((e = readdir(dd))) if (e->d_name[0] != '.') { snprintf(x, sizeof x, "%s/%s", f, e->d_name); remove(x); }
        closedir(dd);
        rmdir(f);
    }
}
static void ct_mail_register(void) {
    char d[1100], f[1400], cwd[1024] = "";
    if (!ct_mail_dir(d, sizeof d, "agents")) return;
    snprintf(f, sizeof f, "%s/%ld", d, (long)getpid());
    FILE *fp = fopen(f, "w");
    if (!fp) return;
    const char *nm = getenv("KATANA_AGENT_NAME");
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    if (nm && *nm) fprintf(fp, "%s\n%s\n", nm, cwd); else fprintf(fp, "katana-%ld\n%s\n", (long)getpid(), cwd);
    fclose(fp);
    ct_mail_dir(d, sizeof d, NULL);
    snprintf(f, sizeof f, "%s/%ld", d, (long)getpid()); mkdir(f, 0700);
}
static void ct_mail_unregister(void) { ct_mail_drop((long)getpid()); }
/* sessoes registradas e vivas (apaga as de pid morto); chama fn para cada uma */
static int ct_mail_each(void (*fn)(long pid, const char *name, const char *cwd, void *u), void *u) {
    char d[1100];
    if (!ct_mail_dir(d, sizeof d, "agents")) return 0;
    DIR *dd = opendir(d);
    if (!dd) return 0;
    struct dirent *e; int n = 0;
    long dead[64]; int nd = 0;
    while ((e = readdir(dd))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        const long pid = atol(e->d_name);
        if (!ct_pid_alive(pid)) { if (nd < 64) dead[nd++] = pid; continue; }
        char f[1400], nm[256] = "", cw[1024] = "";
        snprintf(f, sizeof f, "%s/%s", d, e->d_name);
        FILE *fp = fopen(f, "r");
        if (fp) {
            if (fgets(nm, sizeof nm, fp)) nm[strcspn(nm, "\n")] = 0;
            if (fgets(cw, sizeof cw, fp)) cw[strcspn(cw, "\n")] = 0;
            fclose(fp);
        }
        if (fn) fn(pid, nm, cw, u);
        n++;
    }
    closedir(dd);
    for (int i = 0; i < nd; i++) ct_mail_drop(dead[i]);
    return n;
}
typedef struct { const char *to; long pid; } CtMailFind;
static void ct_mail_find_cb(long pid, const char *name, const char *cwd, void *u) {
    (void)cwd;
    CtMailFind *f = (CtMailFind *)u;
    if (f->pid) return;
    char ps[32]; snprintf(ps, sizeof ps, "%ld", pid);
    if (strcmp(f->to, ps) == 0 || strcmp(f->to, name) == 0) f->pid = pid;
}
/* grava a mensagem na caixa do destino; 1 se achou o destino */
static int ct_mail_send(const char *to, const char *from, const char *msg, char *err, size_t ecap) {
    CtMailFind f = { to, 0 };
    ct_mail_each(ct_mail_find_cb, &f);
    if (!f.pid) return 0;
    if (f.pid == (long)getpid()) { snprintf(err, ecap, "error: %s is this session; you cannot message yourself", to); return -1; }
    static int seq;
    char d[1100], p[1500], t[1600];
    ct_mail_dir(d, sizeof d, NULL);
    snprintf(p, sizeof p, "%s/%ld", d, f.pid); mkdir(p, 0700);
    struct timeval tv; gettimeofday(&tv, NULL);
    snprintf(t, sizeof t, "%s/.%010ld%06ld-%ld-%d.tmp", p, (long)tv.tv_sec, (long)tv.tv_usec, (long)getpid(), seq);
    FILE *fp = fopen(t, "w");
    if (!fp) { snprintf(err, ecap, "error: could not write to the mailbox of %s", to); return -1; }
    fprintf(fp, "from: %s\n%s", from, msg);
    fclose(fp);
    char fin[1600];
    snprintf(fin, sizeof fin, "%s/%010ld%06ld-%ld-%d.msg", p, (long)tv.tv_sec, (long)tv.tv_usec, (long)getpid(), seq++);
    rename(t, fin);             /* aparece inteira de uma vez */
    return (int)f.pid;
}
/* tira a mensagem mais antiga da caixa desta sessao: 1 se havia (remetente em from, texto em msg) */
static int ct_mail_take(char *from, size_t fcap, char *msg, size_t cap) {
    char d[1100], p[1500], best[256] = "";
    if (!ct_mail_dir(d, sizeof d, NULL)) return 0;
    snprintf(p, sizeof p, "%s/%ld", d, (long)getpid());
    DIR *dd = opendir(p);
    if (!dd) return 0;
    struct dirent *e;
    while ((e = readdir(dd))) {
        const size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 5 || strcmp(e->d_name + l - 4, ".msg") != 0 || l >= sizeof best) continue;
        if (!best[0] || strcmp(e->d_name, best) < 0) snprintf(best, sizeof best, "%s", e->d_name);
    }
    closedir(dd);
    if (!best[0]) return 0;
    char f[1800];
    snprintf(f, sizeof f, "%s/%s", p, best);
    FILE *fp = fopen(f, "r");
    if (!fp) return 0;
    char line[512] = "";
    if (!fgets(line, sizeof line, fp)) line[0] = 0;
    line[strcspn(line, "\n")] = 0;
    { const char *fs = strncmp(line, "from: ", 6) == 0 ? line + 6 : "unknown"; const size_t fl = strlen(fs) < fcap ? strlen(fs) : fcap - 1; memcpy(from, fs, fl); from[fl] = 0; }
    const size_t n = cap ? fread(msg, 1, cap - 1, fp) : 0;
    if (cap) msg[n] = 0;
    fclose(fp);
    remove(f);
    return 1;
}
#else
static void ct_mail_register(void) {}
static void ct_mail_unregister(void) {}
static int ct_mail_take(char *from, size_t fcap, char *msg, size_t cap) { (void)from; (void)fcap; (void)msg; (void)cap; return 0; }
#endif

/* ---- ListAgents ---- */
static const char CT_LISTAGENTS_DESC[] = "List the agents you can message with SendMessage: the sub-agents run in this session (id, task, state) "
    "and other katana sessions on this machine that accept messages (pid, name, folder). Sub-agents here run one at a time and are always finished; "
    "continuing one replays its task and last report, not its full context";
static const char CT_LISTAGENTS_PROPS[] = "";
static const char CT_LISTAGENTS_REQ[] = "";
typedef struct { char *out; size_t cap, o; int n; } CtLaOut;
static void ct_la_cb(long pid, const char *name, const char *cwd, void *u) {
    CtLaOut *w = (CtLaOut *)u;
    if (pid == (long)getpid()) return;
    if (!w->n++ && w->o < w->cap) w->o += (size_t)snprintf(w->out + w->o, w->cap - w->o, "Other katana sessions (send by pid or name):\n");
    if (w->o < w->cap) w->o += (size_t)snprintf(w->out + w->o, w->cap - w->o, "- %ld  name: %s  folder: %s\n", pid, name, cwd);
}
static void ct_listagents(const char *js, const char *end, char *out, size_t cap) {
#ifndef _WIN32
    /* ganchos de teste (so com KATANA_AGENT_FAKE=1): registra, espera uma mensagem, mostra e sai */
    { const char *f = getenv("KATANA_AGENT_FAKE"); char *t = (f && atoi(f) == 1) ? ct_str(js, end, "_test") : NULL;
      if (t) {
          if (strcmp(t, "serve") == 0) {
              const long long w = ct_int(js, end, "_wait", 10);
              ct_mail_register();
              printf("registered %ld\n", (long)getpid()); fflush(stdout);
              char fr[256], m[4000]; int got = 0;
              for (long long i = 0; i < w * 10 && !got; i++) { got = ct_mail_take(fr, sizeof fr, m, sizeof m); if (!got) usleep(100000); }
              if (got) snprintf(out, cap, "[message from %s]\n%s", fr, m); else snprintf(out, cap, "no message");
              if (!ct_bool(js, end, "_keep")) ct_mail_unregister();
          } else if (strcmp(t, "take") == 0) {
              char fr[256], m[4000];
              if (ct_mail_take(fr, sizeof fr, m, sizeof m)) snprintf(out, cap, "[message from %s]\n%s", fr, m); else snprintf(out, cap, "no message");
          } else snprintf(out, cap, "error: unknown test hook");
          free(t);
          return;
      } }
#else
    (void)js; (void)end;
#endif
    size_t o = 0;
    out[0] = 0;
    if (g_ct_ag_n) o += (size_t)snprintf(out + o, cap - o, "Sub-agents of this session (all finished; continue one with SendMessage to its id):\n");
    for (int i = 0; i < g_ct_ag_n && o < cap; i++) {
        char d[64]; size_t l = 0;
        for (const char *p = g_ct_ag[i].task; *p && l < 60; p++) d[l++] = (*p == '\n' || *p == '\t') ? ' ' : *p;
        d[l] = 0;
        o += (size_t)snprintf(out + o, cap - o, "- agent-%d  %s  turns: %d  task: %s%s  context: replay only (task + last report)\n",
                              g_ct_ag[i].id, g_ct_ag[i].ok ? "finished" : "failed", g_ct_ag[i].turns, d, strlen(g_ct_ag[i].task) > 60 ? "..." : "");
    }
#ifndef _WIN32
    if (o < cap) { CtLaOut w = { out, cap, o, 0 }; ct_mail_each(ct_la_cb, &w); o = w.o; }
#endif
    if (!out[0]) snprintf(out, cap, "No agents");
    else if (o && o <= cap && out[o - 1] == '\n') out[o - 1] = 0;
}

/* ---- SendMessage ---- */
static const char CT_SENDMESSAGE_DESC[] = "Send a message to an agent listed by ListAgents. To a sub-agent of this session (to: its id, e.g. agent-1): "
    "it is continued now and its new answer comes back (it is re-run with its original task, its previous report and your message; "
    "its full earlier context is not kept). To another katana session on this machine (to: its pid or name): the message is left in its "
    "mailbox and delivered when that session is idle; no answer comes back here";
static const char CT_SENDMESSAGE_PROPS[] = "\"to\": {\"type\": \"string\", \"description\": \"Sub-agent id (agent-N) or the pid or name of another katana session\"}, "
    "\"message\": {\"type\": \"string\", \"description\": \"The message\"}, "
    "\"summary\": {\"type\": \"string\", \"description\": \"5 to 10 word preview of the message\"}";
static const char CT_SENDMESSAGE_REQ[] = "\"to\", \"message\"";
static void ct_sendmessage(const char *js, const char *end, char *out, size_t cap) {
    char *to = ct_str(js, end, "to"), *msg = ct_str(js, end, "message"), *sum = ct_str(js, end, "summary");
    if (!to || !*to) { snprintf(out, cap, "error: missing to (a sub-agent id like agent-1, or the pid or name of a katana session; call ListAgents to see them)"); goto fim; }
    if (!msg || !*msg) { snprintf(out, cap, "error: missing message"); goto fim; }
    CtAg *a = ct_agents_find(to);
    if (a) {
        const size_t pl = strlen(a->task) + strlen(a->report) + strlen(msg) + 400;
        char *pr = (char *)malloc(pl);
        snprintf(pr, pl, "[You are continuing an earlier sub-agent task. Original task:]\n%s\n[Your previous report:]\n%s\n[New message from the main agent:]\n%s",
                 a->task, a->report, msg);
        const size_t ac = cap > 3400 ? 3400 : cap;
        char *ans = (char *)malloc(ac + 1);
        const int id = a->id;
        const double t0 = now_sec();
        const int ok = ct_sub_call(pr, ans, ac);
        a = ct_agents_find(to);                     /* o registro pode ter mudado durante a chamada */
        if (a && ok) { free(a->report); a->report = strdup(ans); a->turns++; }
        if (a) a->ok = ok;
        if (ok) snprintf(out, cap, "[agent-%d continued from its previous report, not from its full context; %.0f s]\n%s", id, now_sec() - t0, ans);
        else snprintf(out, cap, "%s", ans);
        free(ans); free(pr);
        goto fim;
    }
#ifndef _WIN32
    {
        char from[64], err[300] = "";
        const char *nm = getenv("KATANA_AGENT_NAME");
        if (nm && *nm) snprintf(from, sizeof from, "%.40s (pid %ld)", nm, (long)getpid()); else snprintf(from, sizeof from, "katana-%ld", (long)getpid());
        const int r = ct_mail_send(to, from, msg, err, sizeof err);
        if (r < 0) { snprintf(out, cap, "%s", err); goto fim; }
        if (r > 0) {
            snprintf(out, cap, "Message sent to session %d (%s)%s%s. It is delivered when that session is idle; no answer comes back here.",
                     r, to, sum && *sum ? ": " : "", sum && *sum ? sum : "");
            goto fim;
        }
    }
#endif
    snprintf(out, cap, "error: no agent named %s. Sub-agents are agent-1, agent-2... in the order they ran; other sessions go by pid or name. Call ListAgents", to);
fim:
    free(to); free(msg); free(sum);
}

/* ---- SubagentHandback ---- */
static const char CT_SUBAGENTHANDBACK_DESC[] = "Only inside a sub-agent: deliver your final report to the agent that spawned you. "
    "Put everything it needs in message; after this call write nothing else and call no more tools";
static const char CT_SUBAGENTHANDBACK_PROPS[] = "\"message\": {\"type\": \"string\", \"description\": \"Your full final report\"}";
static const char CT_SUBAGENTHANDBACK_REQ[] = "\"message\"";
static void ct_subagenthandback(const char *js, const char *end, char *out, size_t cap) {
    if (!g_in_sub) { snprintf(out, cap, "error: SubagentHandback is only for sub-agents; you are the main agent, answer the user directly"); return; }
    char *m = ct_str(js, end, "message");
    if (!m || !*m) { free(m); m = ct_str(js, end, "report"); }
    if (!m || !*m) { free(m); snprintf(out, cap, "error: missing message (your full final report)"); return; }
    snprintf(g_ct_handback, sizeof g_ct_handback, "%s", m);
    g_ct_handback_set = 1;
    free(m);
    snprintf(out, cap, "Report recorded as your final answer. Stop now: call no more tools and end your turn by repeating the report as plain text.");
}
