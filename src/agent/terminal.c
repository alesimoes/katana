/* ---------- execucao de comandos do Bash e o shell root persistente (pty) ---------- */
#ifndef _WIN32
static void tool_run_command(const char *cmd, const char *js, const char *end, char *out, size_t cap) {
    int tmo = json_int(js, end, "timeout_s", -1);
    if (tmo < 0) { const char *e = getenv("KATANA_CMD_TIMEOUT"); tmo = e && *e ? atoi(e) : 120; }
    if (tmo < 1) tmo = 120;
    if (tmo > 3600) tmo = 3600;
    size_t L = g_budget < 6000 ? g_budget : 6000;
    if (L < 1500) L = 1500;
    if (L + 700 > cap) L = cap > 1500 ? cap - 700 : 800;
    HT h; int rc, to, ir;
    const char *shown = cmd;
    char *wrapped = NULL;
    const int byword = sudo_word(cmd);
    if (sudo_prepare(cmd, sudo_default())) {
        /* padrao: o comando inteiro roda como root (sudo -A sh -c); se ele ja usa sudo, so troca o sudo por sudo -A */
        wrapped = (char *)malloc(strlen(cmd) * 4 + 128);
        if (wrapped && byword) sprintf(wrapped, "sudo() { command sudo -A \"$@\"; }; %s", cmd);
        else if (wrapped) {
            char *w = wrapped + sprintf(wrapped, "exec sudo -A sh -c '");
            for (const char *q = cmd; *q; q++) { if (*q == '\'') { memcpy(w, "'\\''", 4); w += 4; } else *w++ = *q; }
            memcpy(w, "'", 2);
        }
        if (wrapped) cmd = wrapped;
    }
    ht_init(&h, L / 3, L - L / 3);
    g_ui_live = 1; g_live_total = 0;
    int ran = -1;
    if (wrapped && sudo_default()) {       /* sessao root persistente: mantem cwd e variaveis e nao pede a senha de novo */
        ran = root_exec(shown, tmo, &h, &rc, &to, &ir);
        if (ran != 0) { ht_free(&h); ht_init(&h, L / 3, L - L / 3); }
    }
    if (ran != 0 && run_cmd_ex(cmd, tmo, &h, &rc, &to, &ir) != 0) { ht_free(&h); free(wrapped); snprintf(out, cap, "error: cannot run the command"); return; }
    g_ui_live = 0;
    cmd = shown; free(wrapped);
    char *body = (char *)malloc(L + 200);
    if (!body) { ht_free(&h); snprintf(out, cap, "error: out of memory"); return; }
    size_t n = ht_emit(&h, body, L + 200);
    ht_free(&h);
    while (n && (body[n - 1] == '\n' || body[n - 1] == '\r')) body[--n] = 0;
    size_t o = 0;
    if (n == 0) o += (size_t)snprintf(out + o, cap - o, "(no output)");
    else o += (size_t)snprintf(out + o, cap - o, "%s", body);
    if (to) o += (size_t)snprintf(out + o, cap - o, "\n[timeout apos %d s] the command was killed with its whole process group; use a larger timeout_s, or run it again with background=true and read it with job", tmo);
    if (ir) o += (size_t)snprintf(out + o, cap - o, "\n[interrupted by the user]");
    o += (size_t)snprintf(out + o, cap - o, "\n[exit code %d]", rc);
    cmd_hint(rc, to, body, cmd, out + o, cap - o);
    cmdlog_add(cmd, rc, body);
    free(body);
}

/* ---- sessao de shell em pty (so a do root, para o sudo) ---- */
#define TERM_BUF (256 * 1024)
typedef struct { int used, fd, dead, status; pid_t pid; char name[24]; char *buf; unsigned long long tot, rd; } TermSess;

static void term_kill(TermSess *s) {
    if (!s->used) return;
    if (!s->dead) {
        kill(-s->pid, SIGHUP);
        close(s->fd); s->fd = -1;
        int st = 0, ok = 0;
        for (int i = 0; i < 10 && !ok; i++) { if (waitpid(s->pid, &st, WNOHANG) == s->pid) ok = 1; else usleep(20000); }
        if (!ok) { kill(-s->pid, SIGKILL); waitpid(s->pid, &st, 0); }
    } else if (s->fd >= 0) close(s->fd);
    free(s->buf); memset(s, 0, sizeof *s);
}
static TermSess g_rootS;     /* sessao root persistente do Bash com sudo */
static int g_root_exit_set;
static void term_all_kill(void) { term_kill(&g_rootS); }

static void term_pump(TermSess *s) {
    char b[4096];
    while (s->fd >= 0 && !s->dead) {
        const ssize_t n = read(s->fd, b, sizeof b);
        if (n > 0) { for (ssize_t i = 0; i < n; i++) s->buf[s->tot++ % TERM_BUF] = b[i]; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        s->dead = 1;                                         /* EOF/EIO: o shell saiu */
        int st = 0;
        if (waitpid(s->pid, &st, 0) == s->pid) s->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    }
}
/* espera saida nova: ate wait_ms; volta quando chegou algo e ficou 150 ms em silencio */
static void term_wait(TermSess *s, int wait_ms) {
    const double t0 = cm_now();
    int got = 0;
    for (;;) {
        const double rem = wait_ms - (cm_now() - t0) * 1000.0;
        if (rem <= 0 || s->dead) break;
        struct pollfd p = { s->fd, POLLIN, 0 };
        const int w = poll(&p, 1, (int)(got ? (rem < 150 ? rem : 150) : rem));
        if (w <= 0) break;
        const unsigned long long before = s->tot;
        term_pump(s);
        if (s->tot > before) got = 1;
    }
    term_pump(s);
}
/* remove sequencias ANSI, \r e controles; \r sem \n volta ao inicio da linha (barras de progresso) */
static size_t term_clean(const char *src, size_t n, char *dst) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)src[i];
        if (c == 0x1b) {
            if (i + 1 >= n) break;
            const unsigned char d = (unsigned char)src[i + 1];
            if (d == '[') { i += 2; while (i < n && !((unsigned char)src[i] >= 0x40 && (unsigned char)src[i] <= 0x7e)) i++; }
            else if (d == ']') { i += 2; while (i < n && src[i] != 7 && !(src[i] == 0x1b && i + 1 < n && src[i + 1] == '\\')) i++; if (i < n && src[i] == 0x1b) i++; }
            else if (strchr("()*+", d)) i += 2;
            else i += 1;
        } else if (c == '\r') {
            if (i + 1 < n && src[i + 1] == '\n') continue;
            while (o > 0 && dst[o - 1] != '\n') o--;
        } else if (c == '\b') { if (o > 0 && dst[o - 1] != '\n') o--; }
        else if (c == '\n' || c == '\t' || c >= 0x20) dst[o++] = (char)c;
    }
    dst[o] = 0;
    return o;
}

static int root_open(void) {
    if (g_rootS.used && !g_rootS.dead) return 0;
    if (g_rootS.used) term_kill(&g_rootS);
    const int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) { if (m >= 0) close(m); return -1; }
    const char *sn = ptsname(m);
    const int sl = sn ? open(sn, O_RDWR | O_NOCTTY) : -1;
    if (sl < 0) { close(m); return -1; }
    struct winsize ws = { 50, 200, 0, 0 };
    ioctl(m, TIOCSWINSZ, &ws);
    static char *envp[4096], pwenv[300], apenv[560];
    int ne = 0;
    for (char **e = environ; e && *e && ne < 4080; e++)
        if (strncmp(*e, "TERM=", 5) && strncmp(*e, "PS1=", 4) && strncmp(*e, "PS2=", 4) && strncmp(*e, "PAGER=", 6) && strncmp(*e, "GIT_PAGER=", 10) &&
            strncmp(*e, "GIT_TERMINAL_PROMPT=", 20) && strncmp(*e, "DEBIAN_FRONTEND=", 16)) envp[ne++] = *e;
    envp[ne++] = (char *)"TERM=dumb"; envp[ne++] = (char *)"PAGER=cat"; envp[ne++] = (char *)"GIT_PAGER=cat";
    envp[ne++] = (char *)"GIT_TERMINAL_PROMPT=0"; envp[ne++] = (char *)"DEBIAN_FRONTEND=noninteractive";
    if (g_sudo_pw[0] && g_askpass_path[0]) {
        snprintf(pwenv, sizeof pwenv, "KATANA_PW=%s", g_sudo_pw); snprintf(apenv, sizeof apenv, "SUDO_ASKPASS=%s", g_askpass_path);
        envp[ne++] = pwenv; envp[ne++] = apenv;
    }
    envp[ne] = NULL;
    static char sudop[1100];                                  /* caminho do sudo pelo PATH (resolvido antes do vfork) */
    sudop[0] = 0;
    { const char *pa = getenv("PATH"); char dir[1000]; if (!pa) pa = "/usr/bin:/bin";
      for (const char *q = pa; *q && !sudop[0]; ) { const size_t l = strcspn(q, ":"); if (l && l < sizeof dir) { memcpy(dir, q, l); dir[l] = 0; char t[1100]; snprintf(t, sizeof t, "%s/sudo", dir); if (access(t, X_OK) == 0) snprintf(sudop, sizeof sudop, "%s", t); } q += l; if (*q == ':') q++; } }
    if (!sudop[0]) { close(m); close(sl); return -1; }
    fflush(stdout); fflush(stderr);
    const pid_t pid = vfork();
    if (pid < 0) { close(m); close(sl); return -1; }
    if (pid == 0) {
        setsid();
        ioctl(sl, TIOCSCTTY, 0);
        dup2(sl, 0); dup2(sl, 1); dup2(sl, 2);
        if (sl > 2) close(sl);
        close(m);
        execve(sudop, (char *const[]){ (char *)"sudo", (char *)"-A", (char *)"/bin/bash", (char *)"--norc", (char *)"--noprofile", (char *)"--noediting", (char *)"-i", NULL }, envp);
        _exit(127);
    }
    reap_add(pid);
    close(sl);
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    fcntl(m, F_SETFD, FD_CLOEXEC);
    memset(&g_rootS, 0, sizeof g_rootS);
    g_rootS.used = 1; g_rootS.fd = m; g_rootS.pid = pid; snprintf(g_rootS.name, sizeof g_rootS.name, "root");
    g_rootS.buf = (char *)malloc(TERM_BUF);
    if (!g_root_exit_set) { atexit(term_all_kill); g_root_exit_set = 1; }
    const char *init = "stty -echo; PS1=; PS2=; printf '__KRE%s\\n' ADY\n";
    for (size_t i = 0, n = strlen(init); i < n; ) { const ssize_t w = write(m, init + i, n - i); if (w > 0) i += (size_t)w; else if (errno == EAGAIN) usleep(2000); else if (errno != EINTR) break; }
    const double t0 = cm_now();
    while (cm_now() - t0 < 8.0 && !g_rootS.dead) {
        term_wait(&g_rootS, 300);
        char *raw = (char *)malloc(g_rootS.tot > TERM_BUF ? TERM_BUF : g_rootS.tot + 1);
        if (!raw) break;
        const size_t n = g_rootS.tot > TERM_BUF ? TERM_BUF : (size_t)g_rootS.tot;
        for (size_t i = 0; i < n; i++) raw[i] = g_rootS.buf[(g_rootS.tot - n + i) % TERM_BUF];
        raw[n] = 0;
        const int ok = strstr(raw, "__KREADY") != NULL;
        free(raw);
        if (ok) { g_rootS.rd = g_rootS.tot; return 0; }
    }
    term_kill(&g_rootS);                                     /* senha recusada ou sudo indisponivel */
    return -1;
}

static int root_exec(const char *cmd, int tmo, HT *ht, int *rc, int *timedout, int *interrupted) {
    *rc = -1; *timedout = 0; *interrupted = 0;
    static int seq;
    if (root_open() != 0) return -1;
    char f[256];
    const char *tp = getenv("TMPDIR");
    snprintf(f, sizeof f, "%s/katana-root-%d-%d.sh", tp && *tp ? tp : "/tmp", (int)getpid(), ++seq);
    const int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    const size_t cl = strlen(cmd);
    if (write(fd, cmd, cl) != (ssize_t)cl || write(fd, "\n", 1) != 1) { close(fd); unlink(f); return -1; }
    close(fd);
    char id[40], line[600];
    snprintf(id, sizeof id, "%d_%d", (int)getpid(), seq);
    snprintf(line, sizeof line, ". '%s' </dev/null; __rc=$?; rm -f '%s'; printf '\\n__KEND_%s_%%d__\\n' $__rc\n", f, f, id);
    if (g_rootS.dead) { unlink(f); return -1; }
    const unsigned long long from = g_rootS.tot;
    for (size_t i = 0, n = strlen(line); i < n; ) { const ssize_t w = write(g_rootS.fd, line + i, n - i); if (w > 0) i += (size_t)w; else if (errno == EAGAIN) usleep(2000); else if (errno != EINTR) { unlink(f); return -1; } }
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa); sa.sa_handler = cmd_on_sigint; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old);
    g_cmd_int = 0;
    char mk[80];
    snprintf(mk, sizeof mk, "\n__KEND_%s_", id);
    const double t0 = cm_now();
    size_t cap = 65536, n = 0;
    char *acc = (char *)malloc(cap);
    unsigned long long last = from;
    int done = 0, sent_int = 0;
    double int_at = 0;
    char *hit = NULL;
    while (acc && !done) {
        term_wait(&g_rootS, 200);
        if (g_rootS.tot > last) {
            unsigned long long st = last;
            if (g_rootS.tot - st > TERM_BUF) st = g_rootS.tot - TERM_BUF;
            const size_t add = (size_t)(g_rootS.tot - st);
            if (n + add + 1 > cap) { while (n + add + 1 > cap) cap *= 2; if (cap > (size_t)32 << 20) { /* muito grande: guarda so o fim */ size_t keep = (size_t)1 << 20; if (n > keep) { memmove(acc, acc + n - keep, keep); n = keep; } cap = (size_t)8 << 20; } char *na = (char *)realloc(acc, cap); if (!na) break; acc = na; }
            { const size_t n0 = n; for (size_t i = 0; i < add; i++) acc[n++] = g_rootS.buf[(st + i) % TERM_BUF];
              if (g_ui_live && !strstr(acc + (n0 > 80 ? n0 - 80 : 0), "__KEND_")) { char *cl3 = (char *)malloc(add + 1); if (cl3) { const size_t m3 = term_clean(acc + n0, add, cl3); ui_live(cl3, m3); free(cl3); } } }
            acc[n] = 0;
            last = g_rootS.tot;
            hit = strstr(acc, mk);
            if (hit) done = 1;
        }
        if (g_rootS.dead) break;
        if (!done && !sent_int && (g_cmd_int || (tmo > 0 && cm_now() - t0 >= tmo))) {
            if (g_cmd_int) *interrupted = 1; else *timedout = 1;
            const char c3 = 3;
            if (write(g_rootS.fd, &c3, 1) < 0) {}
            usleep(300000);                                  /* o Ctrl+C aborta a linha inteira (inclusive o marcador): manda outro */
            char l2[200];
            snprintf(l2, sizeof l2, "printf '\\n__KEND_%s_%%d__\\n' 130\n", id);
            if (write(g_rootS.fd, l2, strlen(l2)) < 0) {}
            sent_int = 1; int_at = cm_now();
        }
        if (sent_int && !done && cm_now() - int_at > 3.0) break;     /* nao respondeu ao Ctrl+C */
    }
    sigaction(SIGINT, &old, NULL);
    int ret = 0;
    if (hit) {
        const char *q = hit + strlen(mk);
        *rc = atoi(q);
        *hit = 0;
        n = (size_t)(hit - acc);
    } else if (g_rootS.dead) { ret = -1; }
    else { *rc = 130; term_kill(&g_rootS); }                  /* sem marcador: o shell root foi descartado (reabre no proximo comando) */
    if (acc && ret == 0) {
        char *cl2 = (char *)malloc(n + 1);
        if (cl2) { const size_t m = term_clean(acc, n, cl2); size_t b = 0; while (b < m && (cl2[b] == '\n' || cl2[b] == ' ')) b++; ht_add(ht, cl2 + b, m - b); free(cl2); }
    }
    free(acc);
    unlink(f);
    return ret;
}



#endif
