/* ---------- execucao de comandos: stdin fechado, timeout, inicio+fim da saida, anel dos ultimos comandos ----------
 * O agente precisa VER erros: stderr junto da saida, stdin fechado (sudo/ssh nao travam), timeout, codigo de
 * saida e inicio+fim da saida. */

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

#ifndef _WIN32
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
#define HT_ALL_MAX ((size_t)1 << 20)      /* a saida inteira fica guardada ate 1 MB: o meio omitido pode ser consultado por sentido */
typedef struct { char *head, *tail; size_t H, T, hn, tn; char *all; size_t an, acap; int aover; } HT;
static void ht_init(HT *h, size_t H, size_t T) { h->H = H; h->T = T ? T : 1; h->hn = h->tn = 0; h->all = NULL; h->an = h->acap = 0; h->aover = 0; h->head = (char *)malloc(H + 1); h->tail = (char *)malloc(h->T + 1); }
static void ht_free(HT *h) { free(h->head); free(h->tail); free(h->all); h->all = NULL; }
static void ht_add(HT *h, const char *p, size_t n) {
    size_t i = 0;
    if (!h->aover) {
        if (h->an + n > HT_ALL_MAX) { h->aover = 1; free(h->all); h->all = NULL; h->an = h->acap = 0; }
        else {
            if (h->an + n > h->acap) { size_t nc = h->acap ? h->acap * 2 : 8192; while (nc < h->an + n) nc *= 2; h->all = (char *)realloc(h->all, nc); h->acap = nc; }
            memcpy(h->all + h->an, p, n); h->an += n;
        }
    }
    if (h->hn < h->H) { size_t k = h->H - h->hn < n ? h->H - h->hn : n; memcpy(h->head + h->hn, p, k); h->hn += k; i = k; }
    for (; i < n; i++) h->tail[h->tn++ % h->T] = p[i];
}
/* Do trecho omitido (mid, mn bytes) escolhe ate 2 janelas de 6 linhas, as mais proximas em sentido do que o modelo procura (servico de
 * embeddings, app/embsvc.c). Candidatas: janelas com palavras de falha (error, fail, fatal, warning, exception, panic, undefined,
 * denied, cannot, not found, traceback, assert), ate 16, e janelas espalhadas ate completar 24; os vetores so sao calculados dentro de
 * KATANA_EMBED_BASH_MS (padrao 1500 ms). Nota = cosseno + 0,1 se tem palavra de falha; entra com cosseno >= 0,60 ou palavra de falha
 * (limiar por analogia com a memoria, onde o texto sem relacao ficou em 0,5 a 0,68). Escreve em buf o texto das janelas com numero relativo de linha e devolve quantos bytes
 * de conteudo copiou do omitido em *content (0 = nada escolhido). */
static const char *intent_get(void);
static size_t ht_sem_mid(const char *mid, size_t mn, char *buf, size_t bcap, size_t *content) {
    *content = 0; buf[0] = 0;
    const char *intent = intent_get();
    if (!*intent || mn < 600 || !es_available()) return 0;
    enum { WL = 6, MAXW = 24 };
    size_t ls[4096]; size_t nl = 0;
    for (size_t i = 0; i < mn && nl < 4095; ) { ls[nl++] = i; const char *e = (const char *)memchr(mid + i, '\n', mn - i); i = e ? (size_t)(e - mid) + 1 : mn; }
    ls[nl] = mn;
    if (nl < (size_t)(2 * WL)) return 0;
    static const char *const bad[] = { "error", "fail", "fatal", "warning", "exception", "panic", "undefined", "denied", "cannot", "not found", "traceback", "assert", NULL };
    size_t wa[MAXW]; int prior[MAXW], nw = 0;
    for (size_t a = 0; a + WL <= nl + 0 && nw < 16; a += WL) {
        int hit = 0;
        for (size_t i = a; i < a + WL && !hit; i++) {
            const size_t l = ls[i + 1] - ls[i];
            for (int k = 0; bad[k] && !hit; k++) { const size_t bl = strlen(bad[k]); for (size_t j = 0; j + bl <= l && !hit; j++) if (strncasecmp(mid + ls[i] + j, bad[k], bl) == 0) hit = 1; }
        }
        if (hit) { wa[nw] = a; prior[nw] = 1; nw++; }
    }
    const size_t nwin = nl / WL, step = nwin > 8 ? nwin / 8 : 1;
    for (size_t w = 0; w < nwin && nw < MAXW; w += step) {
        int dup = 0; for (int k = 0; k < nw; k++) if (wa[k] == w * WL) dup = 1;
        if (!dup) { wa[nw] = w * WL; prior[nw] = 0; nw++; }
    }
    char *wt[MAXW]; const char *tp[MAXW]; float cs[MAXW];
    for (int k = 0; k < nw; k++) {
        const size_t a = wa[k], z = a + WL < nl ? a + WL : nl;
        wt[k] = (char *)malloc(WL * 200 + 8); size_t tl = 0;
        for (size_t i = a; i < z; i++) { size_t l = ls[i + 1] - ls[i]; if (l && mid[ls[i] + l - 1] == '\n') l--; if (l > 200) l = 200; memcpy(wt[k] + tl, mid + ls[i], l); tl += l; wt[k][tl++] = '\n'; }
        wt[k][tl] = 0; tp[k] = wt[k];
    }
    const char *bm = getenv("KATANA_EMBED_BASH_MS");
    const int got = es_rank(intent, EMB_SEARCH, tp, NULL, nw, bm && atoi(bm) > 0 ? atof(bm) : 1500.0, cs);
    size_t o = 0; int pick[2] = {-1, -1};
    if (got > 0) {
        double bs[2] = {-9, -9};
        for (int k = 0; k < nw; k++) {
            if (cs[k] < -1.5f) continue;
            if (cs[k] < 0.60f && !prior[k]) continue;
            const double f = cs[k] + (prior[k] ? 0.1 : 0.0);
            if (pick[0] >= 0 && wa[k] < wa[pick[0]] + WL && wa[pick[0]] < wa[k] + WL) { if (f > bs[0]) { bs[0] = f; pick[0] = k; } continue; }
            if (pick[0] < 0 || f > bs[0]) { pick[1] = pick[0]; bs[1] = bs[0]; pick[0] = k; bs[0] = f; }
            else if (pick[1] < 0 || f > bs[1]) { if (!(wa[k] < wa[pick[0]] + WL && wa[pick[0]] < wa[k] + WL)) { pick[1] = k; bs[1] = f; } }
        }
        for (int r = 0; r < 2; r++) {
            if (pick[r] < 0) continue;
            const size_t a = wa[pick[r]], z = a + WL < nl ? a + WL : nl;
            if (o + 400 > bcap) break;
            o += (size_t)snprintf(buf + o, bcap - o, "[part of the omitted output, closest in meaning to what you are looking for (omitted lines %zu-%zu)]\n", a + 1, z);
            for (size_t i = a; i < z; i++) {
                size_t l = ls[i + 1] - ls[i]; if (l && mid[ls[i] + l - 1] == '\n') l--; if (l > 200) l = 200;
                if (o + l + 2 > bcap) break;
                memcpy(buf + o, mid + ls[i], l); o += l; buf[o++] = '\n'; *content += l + 1;
            }
        }
        buf[o] = 0;
    }
    for (int k = 0; k < nw; k++) free(wt[k]);
    return o;
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
    size_t omitted = h->tn - h->T + (hn - cut) + skip;
    if (cut >= cap) cut = cap - 1;
    memcpy(out, h->head, cut); o = cut;
    /* o meio omitido, escolhido por sentido: o fim da saida nao perde nada, so comeca mais tarde (o espaco vem do inicio do fim) */
    char sem[2600]; size_t semn = 0, semc = 0;
    if (!h->aover && h->all && h->an == hn + h->tn && hn + from + skip > cut) {
        semn = ht_sem_mid(h->all + cut, hn + from + skip - cut, sem, sizeof sem, &semc);
        if (semn) {
            size_t skip2 = skip + semn;
            for (size_t i = skip2; i < h->T && i < skip2 + 400; i++) if (h->tail[(from + i) % h->T] == '\n') { skip2 = i + 1; break; }
            if (skip2 + 200 < h->T && o + semn + 80 < cap) { omitted = omitted + (skip2 - skip) - semc; skip = skip2; }
            else semn = 0;
        }
    }
    o += (size_t)snprintf(out + o, cap - o, "\n[... %zu bytes omitted ...]\n", omitted);
    if (semn) { memcpy(out + o, sem, semn); o += semn; o += (size_t)snprintf(out + o, cap - o, "[... end of the selected parts; the end of the output follows ...]\n"); }
    for (size_t i = skip; i < h->T && o + 1 < cap; i++) out[o++] = h->tail[(from + i) % h->T];
    out[o] = 0; return o;
}

static volatile sig_atomic_t g_cmd_int;
static void cmd_on_sigint(int s) { (void)s; g_cmd_int = 1; }

/* roda `sh -c cmd` em novo grupo de processos (setsid), stdin /dev/null, stdout+stderr no mesmo cano.
 * tmo = segundos (0 = sem limite). Devolve 0, ou -1 se nao conseguiu iniciar. */
static char g_sudo_pw[256], g_askpass_path[512];   /* senha do sudo desta sessao (so em memoria) */
static int g_ui_live;   /* 1 so durante o Bash do agente: repassa a saida ao vivo para a interface (marcador TOOLLIVE) */
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
    reap_add(pid);
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
