/* ---------- terminal e logs: run_command robusto, terminal (pty), read_log, terminal_screen ----------
 * O agente precisa VER erros: stderr junto da saida, stdin fechado (sudo/ssh nao travam), timeout, codigo de
 * saida, inicio+fim da saida, anel dos ultimos comandos (read_log commands) e sessao de shell persistente. */

/* valor inteiro da chave (numero ou texto numerico); def se nao houver */
static int json_int(const char *js, const char *end, const char *key, int def) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *q = strstr(js, pat);
    if (!q || q >= end) return def;
    q += strlen(pat);
    while (q < end && (*q == ' ' || *q == ':' || *q == '\n' || *q == '\t' || *q == '\r' || *q == '"')) q++;
    if (q >= end || !(isdigit((unsigned char)*q) || *q == '-')) return def;
    return atoi(q);
}
static int json_has(const char *js, const char *end, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *q = strstr(js, pat);
    return q && q < end;
}

/* anel dos ultimos comandos do agente nesta sessao */
#define CMDLOG_N 32
static struct { char cmd[240]; int rc; char sum[360]; time_t t; } g_cmdlog[CMDLOG_N];
static int g_cmdlog_n;
static void cmdlog_add(const char *cmd, int rc, const char *out) {
    const int i = g_cmdlog_n % CMDLOG_N;
    g_cmdlog_n++;
    snprintf(g_cmdlog[i].cmd, sizeof g_cmdlog[i].cmd, "%s", cmd);
    g_cmdlog[i].rc = rc;
    g_cmdlog[i].t = time(NULL);
    const size_t n = strlen(out);
    const char *s = out;
    if (n > 340) { s = out + n - 340; while (((unsigned char)*s & 0xC0) == 0x80) s++; }
    snprintf(g_cmdlog[i].sum, sizeof g_cmdlog[i].sum, "%s", s);
}

#ifdef _WIN32
static void tool_terminal(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: terminal is not available on Windows yet"); }
static void tool_terminal_screen(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: terminal_screen is not available on Windows yet"); }
static void tool_read_log(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: read_log is not available on Windows yet"); }
#else
#include <regex.h>
#include <sys/ioctl.h>
#include <termios.h>
extern int posix_openpt(int);
extern int grantpt(int);
extern int unlockpt(int);
extern char *ptsname(int);
extern char **environ;

static double cm_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

/* guarda o INICIO (H bytes) e o FIM (T bytes, anel) de um fluxo de tamanho qualquer */
typedef struct { char *head, *tail; size_t H, T, hn, tn; } HT;
static void ht_init(HT *h, size_t H, size_t T) { h->H = H; h->T = T ? T : 1; h->hn = h->tn = 0; h->head = (char *)malloc(H + 1); h->tail = (char *)malloc(h->T + 1); }
static void ht_free(HT *h) { free(h->head); free(h->tail); }
static void ht_add(HT *h, const char *p, size_t n) {
    size_t i = 0;
    if (h->hn < h->H) { size_t k = h->H - h->hn < n ? h->H - h->hn : n; memcpy(h->head + h->hn, p, k); h->hn += k; i = k; }
    for (; i < n; i++) h->tail[h->tn++ % h->T] = p[i];
}
/* texto final: tudo se coube; senao inicio + aviso + fim; devolve bytes escritos */
static size_t ht_emit(const HT *h, char *out, size_t cap) {
    size_t o = 0, hn = h->hn;
    if (h->tn <= h->T) {
        o = hn < cap ? hn : cap - 1; memcpy(out, h->head, o);
        for (size_t i = 0; i < h->tn && o + 1 < cap; i++) out[o++] = h->tail[i];
        out[o] = 0; return o;
    }
    size_t cut = hn;                                         /* corta o inicio numa quebra de linha, se houver perto */
    for (size_t i = hn; i > 0 && hn - i < 400; i--) if (h->head[i - 1] == '\n') { cut = i; break; }
    const size_t from = h->tn - h->T;
    size_t skip = 0;                                         /* o fim comeca numa linha inteira, se houver perto */
    for (size_t i = 0; i < h->T && i < 400; i++) if (h->tail[(from + i) % h->T] == '\n') { skip = i + 1; break; }
    const size_t omitted = h->tn - h->T + (hn - cut) + skip;
    if (cut >= cap) cut = cap - 1;
    memcpy(out, h->head, cut); o = cut;
    o += (size_t)snprintf(out + o, cap - o, "\n[... %zu bytes omitted ...]\n", omitted);
    for (size_t i = skip; i < h->T && o + 1 < cap; i++) out[o++] = h->tail[(from + i) % h->T];
    out[o] = 0; return o;
}

static volatile sig_atomic_t g_cmd_int;
static void cmd_on_sigint(int s) { (void)s; g_cmd_int = 1; }

/* roda `sh -c cmd` em novo grupo de processos (setsid), stdin /dev/null, stdout+stderr no mesmo cano.
 * tmo = segundos (0 = sem limite). Devolve 0, ou -1 se nao conseguiu iniciar. */
static char g_sudo_pw[256], g_askpass_path[512];   /* senha do sudo desta sessao (so em memoria) */
static int g_ui_live;   /* 1 so durante o run_command do agente: repassa a saida ao vivo para a interface (marcador TOOLLIVE) */
static size_t g_live_total;
static void ui_live(const char *b, size_t n) {
    if (!n || !g_ui_live || !getenv("KATANA_UI")) return;
    if (g_live_total > ((size_t)256 << 10)) return;          /* limite por comando; o resultado final vem em TOOLOUT */
    g_live_total += n;
    fputs("\x01" "TOOLLIVE ", stdout);
    for (size_t i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char)b[i];
        if (ch == '\n') fputs("\\n", stdout);
        else if (ch == '\\') fputs("\\\\", stdout);
        else if (ch == '\r' || ch == 1) continue;
        else putchar(ch);
    }
    putchar('\n'); fflush(stdout);
}
/* o agente foi encerrado (o dono fechou o aplicativo) com um comando em curso: o grupo do comando morre junto.
   Sem isto o comando ficava orfao (visto com a chamada do professor, que seguia rodando depois do aplicativo fechar).
   Depois o sinal segue o caminho padrao. */
static volatile pid_t g_fg_pid;
static void (*g_cmd_tap)(const char *, size_t);     /* quem chamou quer ver a saida do comando enquanto ela chega */
static void cmd_on_term(int sig) {
    const pid_t p = g_fg_pid;
    if (p > 0) kill(-p, SIGTERM);
    signal(sig, SIG_DFL);
    raise(sig);
}
static void cmd_term_hook(void) {
    static int done;
    if (done) return;
    done = 1;
    const int sigs[2] = { SIGTERM, SIGHUP };
    for (int i = 0; i < 2; i++) {
        struct sigaction cur;
        if (sigaction(sigs[i], NULL, &cur) != 0 || cur.sa_handler != SIG_DFL) continue;      /* ja tratado ou ignorado: nao mexe */
        struct sigaction sa;
        memset(&sa, 0, sizeof sa); sa.sa_handler = cmd_on_term; sigemptyset(&sa.sa_mask);
        sigaction(sigs[i], &sa, NULL);
    }
}
static int run_cmd_ex(const char *cmd, int tmo, HT *ht, int *rc, int *timedout, int *interrupted) {
    int pf[2];
    cmd_term_hook();
    *rc = -1; *timedout = 0; *interrupted = 0;
    if (pipe(pf) != 0) return -1;
    static char *envp[4096];                                 /* ambiente herdado + variaveis que evitam prompts */
    int ne = 0;
    for (char **e = environ; e && *e && ne < 4090; e++) if (strncmp(*e, "GIT_TERMINAL_PROMPT=", 20) && strncmp(*e, "PAGER=", 6)) envp[ne++] = *e;
    envp[ne++] = (char *)"GIT_TERMINAL_PROMPT=0"; envp[ne++] = (char *)"PAGER=cat";
    static char pwenv[300], apenv[560];
    if (g_sudo_pw[0] && g_askpass_path[0] && ne < 4092) {     /* senha do sudo so no ambiente do filho; o sudo -A a le pelo askpass */
        snprintf(pwenv, sizeof pwenv, "KATANA_PW=%s", g_sudo_pw); snprintf(apenv, sizeof apenv, "SUDO_ASKPASS=%s", g_askpass_path);
        envp[ne++] = pwenv; envp[ne++] = apenv;
    }
    envp[ne] = NULL;
    fflush(stdout); fflush(stderr);
    const int dn = open("/dev/null", O_RDWR);
    const pid_t pid = vfork();
    if (pid < 0) { close(pf[0]); close(pf[1]); if (dn >= 0) close(dn); return -1; }
    if (pid == 0) {
        setsid();
        if (dn >= 0) dup2(dn, 0);
        dup2(pf[1], 1); dup2(pf[1], 2);
        close(pf[0]); if (pf[1] > 2) close(pf[1]); if (dn > 2) close(dn);
        execve("/bin/sh", (char *const[]){ (char *)"sh", (char *)"-c", (char *)cmd, NULL }, envp);
        _exit(127);
    }
    close(pf[1]); if (dn >= 0) close(dn);
    g_fg_pid = pid;
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa); sa.sa_handler = cmd_on_sigint; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old);
    g_cmd_int = 0;
    const double t0 = cm_now();
    int exited = 0, st = 0, eof = 0;
    char buf[8192];
    while (!eof) {
        if (g_cmd_int) { *interrupted = 1; break; }
        if (tmo > 0 && cm_now() - t0 >= tmo) { *timedout = 1; break; }
        if (!exited && waitpid(pid, &st, WNOHANG) == pid) exited = 1;
        struct pollfd p = { pf[0], POLLIN, 0 };
        const int w = poll(&p, 1, exited ? 100 : 200);
        if (w > 0) {
            const ssize_t n = read(pf[0], buf, sizeof buf);
            if (n > 0) { ht_add(ht, buf, (size_t)n); ui_live(buf, (size_t)n); if (g_cmd_tap) g_cmd_tap(buf, (size_t)n); }
            else if (n == 0 || (errno != EINTR && errno != EAGAIN)) eof = 1;
        } else if (w == 0 && exited) break;                  /* o shell acabou e nada mais chega (processo em segundo plano segurando o cano) */
    }
    close(pf[0]);
    if (*timedout || *interrupted) {
        kill(-pid, SIGTERM);
        for (int i = 0; i < 10 && !exited; i++) { usleep(30000); if (waitpid(pid, &st, WNOHANG) == pid) exited = 1; }
        kill(-pid, SIGKILL);
    }
    if (!exited) waitpid(pid, &st, 0);
    g_fg_pid = 0;
    sigaction(SIGINT, &old, NULL);
    *rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return 0;
}

/* comando curto com saida completa (ate maxb, mantendo o fim); malloc, *rc codigo */
static char *run_collect(const char *cmd, int tmo, size_t maxb, int *rc) {
    HT h; int to, ir;
    ht_init(&h, 0, maxb);
    char *o = (char *)malloc(maxb + 128);
    if (!o || run_cmd_ex(cmd, tmo, &h, rc, &to, &ir) != 0) { ht_free(&h); free(o); *rc = -1; return NULL; }
    ht_emit(&h, o, maxb + 128);
    ht_free(&h);
    if (to) *rc = 124;
    return o;
}

#endif
