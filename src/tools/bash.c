/* ---------- ferramentas Bash, TaskStop, Monitor, SendInput (ver docs/tools.md) ----------
 * Bash: cada comando roda num shell novo (bash, ou sh se nao houver), em nova sessao, num PSEUDO-TERMINAL (stdin, stdout e
 * stderr). Um processo copiador (ctb_pump) le o terminal e grava a saida, sem sequencias de escape, num ARQUIVO
 * (~/.katana/jobs/bash-<pid>-<n>.log). O katana espera ate o timeout lendo o arquivo (a interface ve a saida ao vivo); se o
 * comando nao acabou, ele nao morre: entra na tabela de trabalhos de jobs.c (g_jobs) e vira tarefa em segundo plano.
 * Pergunta: se o comando fica parado lendo o terminal (ou, sem /proc, a saida acaba numa linha com cara de pergunta) depois
 * de um silencio (KATANA_PROMPT_WAIT ms, padrao 2000), o Bash nao espera o timeout: a tarefa passa para segundo plano e o
 * modelo recebe "[waiting for input: ...]" com o numero dela; SendInput escreve no terminal dela. Pergunta de SEGREDO (eco
 * desligado no terminal, ou senha/frase secreta/PIN/codigo/token no texto): o katana pede ao usuario (secret_ask, agent/sudo.c:
 * marcador SECRET no desktop, /dev/tty no terminal) e escreve direto no terminal; o modelo so ve a marca.
 * A pasta do shell persiste entre chamadas so para o Bash (g_ctb_cwd): o roteiro grava `pwd -P` ao sair (trap EXIT);
 * se a pasta final ficar fora da pasta de trabalho do agente (e das pastas liberadas), volta para a pasta de trabalho
 * e o resultado avisa. Variaveis de ambiente nao persistem. O PATH ganha as pastas usuais de programas do usuario (ctb_path).
 * TaskStop encerra uma tarefa (grupo inteiro). Monitor inicia um comando em segundo plano com prazo (padrao 5 min). */

#ifndef _WIN32
#include <glob.h>
#include <dirent.h>
#include <poll.h>
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

/* terminal de um comando: ponta mestre (fd+1, 0 = sem terminal), copiador e nome do terminal */
typedef struct { int mfd; pid_t pump; char slave[64]; } CtbPty;

/* versao principal de uma pasta .../node/vNN.x/bin (0 se nao tiver) */
static long ctb_major(const char *p) { const char *v = strstr(p, "/node/v"); return v ? strtol(v + 7, NULL, 10) : 0; }
/* "PATH=..." do comando: o do katana mais as pastas usuais de programas do usuario que existirem e ainda nao estiverem nele */
static void ctb_path(char *o, size_t cap) {
    const char *p = getenv("PATH");
    if (!p || !*p) p = "/usr/local/bin:/usr/bin:/bin";
    size_t n = (size_t)snprintf(o, cap, "PATH=%s", p);
    if (n >= cap) { o[cap - 1] = 0; return; }
    static char c[12][1100];
    int nc = 0;
    const char *h = getenv("HOME");
    if (h && *h) {
        static const char *const rel[] = { ".local/bin", ".cargo/bin", ".local/share/swiftly/bin", "go/bin", ".bun/bin", ".deno/bin", ".dotnet/tools", NULL };
        for (int i = 0; rel[i]; i++) snprintf(c[nc++], sizeof c[0], "%s/%s", h, rel[i]);
        char pat[1100];
        snprintf(pat, sizeof pat, "%s/.nvm/versions/node/*/bin", h);
        glob_t g;
        if (glob(pat, 0, NULL, &g) == 0) {                   /* node: a versao mais nova instalada */
            size_t best = 0;
            for (size_t i = 1; i < g.gl_pathc; i++) if (ctb_major(g.gl_pathv[i]) > ctb_major(g.gl_pathv[best])) best = i;
            if (g.gl_pathc) snprintf(c[nc++], sizeof c[0], "%s", g.gl_pathv[best]);
            globfree(&g);
        }
    }
    snprintf(c[nc++], sizeof c[0], "/usr/local/go/bin");
    for (int i = 0; i < nc; i++) {
        struct stat st;
        if (stat(c[i], &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        const size_t l = strlen(c[i]);
        int have = 0;
        for (const char *q = p; *q; ) { const size_t k = strcspn(q, ":"); if (k == l && !strncmp(q, c[i], l)) { have = 1; break; } q += k; if (*q) q++; }
        if (!have && n + l + 2 < cap) n += (size_t)snprintf(o + n, cap - n, ":%s", c[i]);
    }
}

/* copiador: le o terminal e grava no arquivo sem as sequencias de escape (cor, cursor, titulo); \r\n vira \n e \r sozinho
 * (barra de progresso) vira quebra de linha; outros caracteres de controle saem. Sai quando ninguem mais segura o terminal. */
static void ctb_pump(int m, int lf) {
    static char in[8192], ob[2 * 8192 + 8];
    int st = 0, cr = 0;
    for (;;) {
        const ssize_t k = read(m, in, sizeof in);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && errno == EAGAIN) { usleep(20000); continue; }
        if (k <= 0) break;                                   /* EIO: o ultimo processo soltou o terminal */
        size_t o = 0;
        for (ssize_t i = 0; i < k; i++) {
            const unsigned char c = (unsigned char)in[i];
            if (st == 1) { st = c == '[' ? 2 : (c == ']' || c == 'P' || c == '_' || c == '^') ? 3 : (c == '(' || c == ')' || c == '*' || c == '+' || c == '#' || c == '%') ? 4 : 0; continue; }
            if (st == 2) { if (c >= 0x40 && c <= 0x7e) st = 0; continue; }       /* CSI: ate o byte final */
            if (st == 3) { if (c == 7) st = 0; else if (c == 0x1b) st = 5; continue; }      /* OSC/DCS: ate BEL ou ESC \ */
            if (st == 5) { st = c == '\\' ? 0 : 3; continue; }
            if (st == 4) { st = 0; continue; }
            if (c == 0x1b) { st = 1; continue; }
            if (cr) { cr = 0; ob[o++] = '\n'; if (c == '\n') continue; }
            if (c == '\r') { cr = 1; continue; }
            if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f) continue;
            ob[o++] = (char)c;
        }
        for (size_t w = 0; w < o; ) {
            const ssize_t r = write(lf, ob + w, o - w);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            w += (size_t)r;
        }
    }
    if (cr && write(lf, "\n", 1) < 0) {}
}

/* inicia o roteiro em nova sessao, com um pseudo-terminal como stdin/stdout/stderr e o copiador gravando a saida em log;
 * devolve o pid ou -1. Sem pseudo-terminal (falhou ao abrir): stdin /dev/null e a saida direto no arquivo (t->mfd = 0). */
static pid_t ctb_spawn(const char *script, const char *log, CtbPty *t) {
    static char *envp[4096];                                 /* ambiente herdado + variaveis que evitam prompts e cores */
    int ne = 0;
    for (char **e = environ; e && *e && ne < 4080; e++)
        if (strncmp(*e, "GIT_TERMINAL_PROMPT=", 20) && strncmp(*e, "PAGER=", 6) && strncmp(*e, "PATH=", 5) && strncmp(*e, "TERM=", 5) &&
            strncmp(*e, "NO_COLOR=", 9) && strncmp(*e, "COLUMNS=", 8) && strncmp(*e, "LINES=", 6)) envp[ne++] = *e;
    static char pathenv[16384];
    ctb_path(pathenv, sizeof pathenv);
    envp[ne++] = pathenv;
    envp[ne++] = (char *)"GIT_TERMINAL_PROMPT=0"; envp[ne++] = (char *)"PAGER=cat";
    envp[ne++] = (char *)"TERM=dumb"; envp[ne++] = (char *)"NO_COLOR=1";
    static char pwenv[300], apenv[560];
    if (g_sudo_pw[0] && g_askpass_path[0]) {
        snprintf(pwenv, sizeof pwenv, "KATANA_PW=%s", g_sudo_pw); snprintf(apenv, sizeof apenv, "SUDO_ASKPASS=%s", g_askpass_path);
        envp[ne++] = pwenv; envp[ne++] = apenv;
    }
    envp[ne] = NULL;
    mkdir_parents(log);
    t->mfd = 0; t->pump = 0; t->slave[0] = 0;
    const int lf = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600), dn = open("/dev/null", O_RDWR);
    if (lf < 0) { if (dn >= 0) close(dn); return -1; }
    int m = posix_openpt(O_RDWR | O_NOCTTY), s = -1;
    if (m >= 0) {
        fcntl(m, F_SETFD, FD_CLOEXEC);
        const char *sn = grantpt(m) == 0 && unlockpt(m) == 0 ? ptsname(m) : NULL;
        if (sn) { snprintf(t->slave, sizeof t->slave, "%s", sn); s = open(t->slave, O_RDWR | O_NOCTTY); }
        if (s < 0) { close(m); m = -1; t->slave[0] = 0; }
        else {
            struct termios tio;
            if (tcgetattr(s, &tio) == 0) { tio.c_oflag &= ~(tcflag_t)ONLCR; tcsetattr(s, TCSANOW, &tio); }    /* \n sem \r */
            struct winsize ws;
            memset(&ws, 0, sizeof ws); ws.ws_row = 50; ws.ws_col = 200;
            ioctl(s, TIOCSWINSZ, &ws);
        }
    }
    const char *sh = ctb_shell();
    fflush(stdout); fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        signal(SIGPIPE, SIG_DFL);
        if (s >= 0) { ioctl(s, TIOCSCTTY, 0); dup2(s, 0); dup2(s, 1); dup2(s, 2); if (s > 2) close(s); }
        else { if (dn >= 0) dup2(dn, 0); dup2(lf, 1); dup2(lf, 2); }
        if (lf > 2) close(lf);
        if (dn > 2) close(dn);
        execve(sh, (char *const[]){ (char *)sh, (char *)"-c", (char *)script, NULL }, envp);
        _exit(127);
    }
    if (s >= 0) close(s);
    if (pid < 0) { if (m >= 0) close(m); close(lf); if (dn >= 0) close(dn); return -1; }
    reap_add(pid);
    if (m >= 0) {
        const pid_t pp = fork();
        if (pp == 0) {                                       /* copiador: sessao propria, sem os descritores do katana */
            setsid();
            signal(SIGINT, SIG_IGN); signal(SIGHUP, SIG_IGN); signal(SIGTERM, SIG_DFL); signal(SIGPIPE, SIG_IGN);
            if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
            for (int fd = 3; fd < 4096; fd++) if (fd != m && fd != lf) close(fd);
            ctb_pump(m, lf);
            _exit(0);
        }
        if (pp < 0) { kill(-pid, SIGKILL); waitpid(pid, NULL, 0); close(m); close(lf); if (dn >= 0) close(dn); return -1; }
        reap_add(pp);
        t->pump = pp; t->mfd = m + 1;
    }
    close(lf); if (dn >= 0) close(dn);
    return pid;
}
/* o comando acabou (ou foi encerrado): espera o copiador gravar o resto (ate wait_s) e solta a ponta mestre */
static void ctb_pty_close(CtbPty *t, double wait_s) {
    if (t->pump > 0) {
        const double t0 = cm_now();
        int gone = 0;
        for (;;) { if (waitpid(t->pump, NULL, WNOHANG) != 0) { gone = 1; break; } if (cm_now() - t0 >= wait_s) break; usleep(10000); }
        if (!gone) pump_later(t->pump);
        t->pump = 0;
    }
    if (t->mfd > 0) { close(t->mfd - 1); t->mfd = 0; }
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
static Job *ctb_adopt(pid_t pid, const char *log, const char *cmd, time_t t0, CtbPty *t) {
    Job *j = ctb_slot();
    if (!j) return NULL;
    memset(j, 0, sizeof *j);
    j->id = ++g_job_seq; j->pid = pid; j->t0 = t0;
    if (t) { j->mfd = t->mfd; j->pump = t->pump; snprintf(j->slave, sizeof j->slave, "%s", t->slave); t->mfd = 0; t->pump = 0; }     /* a tarefa fica com o terminal */
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

/* ---- perguntas do comando (terminal parado esperando resposta) ---- */
/* 1 = algum processo da sessao sid esta parado lendo o terminal (slave ou /dev/tty); 0 = nenhum; -1 = nao da para saber
 * (espera em poll/select, /proc fechado, outra arquitetura) */
static int ctb_reading(pid_t sid, const char *slave) {
#if defined(__x86_64__) || defined(__aarch64__)
#if defined(__x86_64__)
    static const long rd[] = { 0, 19, 275, -9 };      /* read, readv, splice */
#else
    static const long rd[] = { 63, 65, 76, -9 };
#endif
    DIR *d = opendir("/proc");
    if (!d) return -1;
    int res = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char p[300], b[1024];
        snprintf(p, sizeof p, "/proc/%s/stat", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        size_t k = fread(b, 1, sizeof b - 1, f);
        fclose(f);
        b[k] = 0;
        const char *rp = strrchr(b, ')');
        char state = 0;
        int ppid = 0, pgrp = 0, sess = 0;
        if (!rp || sscanf(rp + 1, " %c %d %d %d", &state, &ppid, &pgrp, &sess) != 4 || sess != (int)sid || state == 'Z') continue;
        if (state == 'R') { res = res < 0 ? res : 0; continue; }
        /* todas as threads do processo (um programa com varias threads le o terminal numa delas; a principal pode estar em sigsuspend,
           futex ou epoll): leitura do terminal = 1; dormindo em outra chamada = nao da para saber (-1), o texto da ultima linha decide */
        char td[300];
        snprintf(td, sizeof td, "/proc/%s/task", e->d_name);
        DIR *dt = opendir(td);
        if (!dt) { res = -1; continue; }
        struct dirent *et;
        while ((et = readdir(dt))) {
            if (!isdigit((unsigned char)et->d_name[0])) continue;
            snprintf(p, sizeof p, "/proc/%s/task/%s/syscall", e->d_name, et->d_name);
            f = fopen(p, "r");
            if (!f) { res = -1; continue; }
            k = fread(b, 1, sizeof b - 1, f);
            fclose(f);
            b[k] = 0;
            long nr = -1;
            unsigned long a0 = 0;
            if (sscanf(b, "%ld 0x%lx", &nr, &a0) != 2) { if (strncmp(b, "running", 7)) res = -1; continue; }
            int isrd = 0;
            for (int i = 0; rd[i] >= 0; i++) if (nr == rd[i]) isrd = 1;
            if (isrd) {
                char l[256];
                snprintf(p, sizeof p, "/proc/%s/fd/%lu", e->d_name, a0);
                const ssize_t ll = readlink(p, l, sizeof l - 1);
                if (ll > 0) { l[ll] = 0; if (!strcmp(l, slave) || !strcmp(l, "/dev/tty")) { closedir(dt); closedir(d); return 1; } }
            } else res = -1;
        }
        closedir(dt);
    }
    closedir(d);
    return res;
#else
    (void)sid; (void)slave;
    return -1;
#endif
}
/* ultima linha da saida (ou a anterior nao vazia, se a saida acaba em quebra de linha), sem espacos nas pontas e com " virando ';
 * *nl = 1 se a saida acaba em quebra de linha */
static void ctb_last_line(const char *log, char *o, size_t cap, int *nl) {
    char b[704];
    const long sz = ctb_size(log);
    size_t k = ctb_read(log, sz > 600 ? sz - 600 : 0, b, 600);
    *nl = k && b[k - 1] == '\n';
    while (k && (b[k - 1] == '\n' || b[k - 1] == ' ' || b[k - 1] == '\t')) k--;
    b[k] = 0;
    size_t i = k;
    while (i && b[i - 1] != '\n') i--;
    const char *ln = b + i;
    while (*ln == ' ' || *ln == '\t') ln++;
    size_t n = 0;
    for (; ln[n] && n + 1 < cap && n < 200; n++) o[n] = ln[n] == '"' ? '\'' : ln[n];
    if (ln[n]) n = ctb_utf8(o, n);
    o[n] = 0;
}
/* w aparece em s como palavra inteira (s ja em minusculas) */
static int ctb_word(const char *s, const char *w) {
    const size_t l = strlen(w);
    for (const char *q = s; (q = strstr(q, w)); q++)
        if ((q == s || !isalnum((unsigned char)q[-1])) && !isalnum((unsigned char)q[l])) return 1;
    return 0;
}
static void ctb_lower(const char *s, char *o, size_t cap) { size_t n = 0; for (; s[n] && n + 1 < cap; n++) o[n] = (char)tolower((unsigned char)s[n]); o[n] = 0; }
/* a linha tem cara de pergunta (usado quando nao da para ver se o processo le o terminal) */
static int ctb_prompt_like(const char *l) {
    char lo[256];
    ctb_lower(l, lo, sizeof lo);
    size_t n = strlen(lo);
    if (!n) return 0;
    const char c = lo[n - 1];
    if (c == ':' || c == '?' || c == ']' || c == ')' || c == '>') return 1;
    static const char *const w[] = { "password", "passphrase", "[sudo]", "pin", "code", "token", "login", "username", "(y/n)", "[y/n]", "(yes/no)", "select", "path to", "enter", "choose", NULL };
    for (int i = 0; w[i]; i++) if (w[i][0] == '[' || w[i][0] == '(' || strchr(w[i], ' ') ? strstr(lo, w[i]) != NULL : ctb_word(lo, w[i])) return 1;
    return 0;
}
/* a pergunta e de segredo: o terminal esta com o eco desligado em modo de linha (como faz quem le senha), ou o texto fala de
 * senha, frase secreta, PIN, codigo de verificacao ou token */
static int ctb_is_secret(int m, const char *l) {
    struct termios t;
    if (tcgetattr(m, &t) == 0 && !(t.c_lflag & ECHO) && (t.c_lflag & ICANON)) return 1;
    char lo[256];
    ctb_lower(l, lo, sizeof lo);
    if (strstr(lo, "password") || strstr(lo, "passphrase") || strstr(lo, "passwd") || strstr(lo, "senha") || strstr(lo, "one-time") || strstr(lo, "two-factor") || strstr(lo, "verification")) return 1;
    static const char *const w[] = { "pin", "otp", "2fa", "code", "token", "secret", NULL };
    for (int i = 0; w[i]; i++) if (ctb_word(lo, w[i])) return 1;
    return 0;
}
static double ctb_wait_s(void) {
    const char *e = getenv("KATANA_PROMPT_WAIT");
    int ms = e && atoi(e) > 0 ? atoi(e) : 2000;
    if (ms < 300) ms = 300;
    if (ms > 30000) ms = 30000;
    return ms / 1000.0;
}
/* escreve n bytes no terminal (espera ate 2 s se ele estiver cheio); devolve os bytes escritos, *err = errno se parou antes */
static size_t ctb_write(int m, const char *s, size_t n, int *err) {
    size_t put = 0;
    *err = 0;
    const double t0 = cm_now();
    while (put < n) {
        struct pollfd pf = { m, POLLOUT, 0 };
        const int pr = poll(&pf, 1, 100);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0 || !(pf.revents & POLLOUT)) { if (cm_now() - t0 < 2.0 && pr == 0) continue; *err = pr < 0 ? errno : EIO; break; }
        const size_t c = n - put > 512 ? 512 : n - put;
        const ssize_t k = write(m, s + put, c);
        if (k > 0) { put += (size_t)k; continue; }
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        *err = k < 0 ? errno : EIO;
        break;
    }
    return put;
}
static int g_ctb_secret_seq;
/* pergunta de segredo: o usuario responde (secret_ask) e o texto vai direto ao terminal, com o eco desligado durante a escrita;
 * na saida (e para o modelo) fica so a marca. Cancelado (ou cancel = 1): Ctrl+C no terminal. */
static void ctb_secret_answer(int m, const char *log, const char *label, int cancel) {
    static char pw[1100];
    const int ok = !cancel && secret_ask(++g_ctb_secret_seq, label, pw, sizeof pw - 2);
    const int lf = open(log, O_WRONLY | O_APPEND);
    int err = 0;
    if (ok) {
        struct termios t;
        const int echo = tcgetattr(m, &t) == 0 && (t.c_lflag & ECHO);
        if (echo) { struct termios t2 = t; t2.c_lflag &= ~(tcflag_t)(ECHO | ECHONL); tcsetattr(m, TCSANOW, &t2); }
        size_t n = strlen(pw);
        pw[n++] = '\n';
        ctb_write(m, pw, n, &err);
        if (echo) { usleep(300000); if (tcgetattr(m, &t) == 0) { t.c_lflag |= ECHO; tcsetattr(m, TCSANOW, &t); } }      /* a linha ja foi lida pelo terminal sem eco */
        if (lf >= 0 && write(lf, "[secret entered by the user]\n", 29) < 0) {}
    } else {
        ctb_write(m, "\x03", 1, &err);
        if (lf >= 0 && write(lf, "[secret cancelled]\n", 19) < 0) {}
    }
    memset(pw, 0, sizeof pw);
    if (lf >= 0) close(lf);
}
enum { CW_EXIT, CW_PROMPT, CW_TIME, CW_INTR };
/* espera o comando: CW_EXIT quando ele acaba (*st), CW_PROMPT quando para numa pergunta que o modelo pode responder (label
 * recebe o texto), CW_TIME no prazo, CW_INTR no Ctrl+C (so com live). Perguntas de segredo sao respondidas aqui pelo usuario
 * (ate 3; depois cancela) e o tempo que o usuario leva nao conta no prazo. live: a saida nova vai para a interface ao vivo
 * (*seen = bytes ja mostrados). */
static int ctb_watch(pid_t pid, const CtbPty *t, const char *log, double max_s, int live, long *seen, int *st, char *label, size_t lcap) {
    double t0 = cm_now(), still = t0, next = 0;
    const double ws = ctb_wait_s();
    long last = ctb_size(log);
    int nsec = 0;
    char buf[8192];
    label[0] = 0;
    for (;;) {
        if (waitpid(pid, st, WNOHANG) == pid) return CW_EXIT;
        const long sz = ctb_size(log);
        if (live) while (*seen < sz) { const size_t k = ctb_read(log, *seen, buf, sizeof buf - 1); if (!k) break; ui_live(buf, k); if (g_cmd_tap) g_cmd_tap(buf, k); *seen += (long)k; }
        const double now = cm_now();
        if (sz != last) { last = sz; still = now; }
        if (live && g_cmd_int) return CW_INTR;
        if (now - t0 >= max_s) return CW_TIME;
        if (t->mfd > 0 && now - still >= ws && now >= next) {
            next = now + 1.0;
            const int r = ctb_reading(pid, t->slave);
            int nl = 0;
            ctb_last_line(log, label, lcap, &nl);
            if (r == 1 || (r < 0 && !nl && ctb_prompt_like(label))) {
                if (!ctb_is_secret(t->mfd - 1, label)) return CW_PROMPT;
                const double a0 = cm_now();
                ctb_secret_answer(t->mfd - 1, log, label, nsec++ >= 3);
                t0 += cm_now() - a0;
                last = ctb_size(log); still = cm_now(); next = 0;
            }
            label[0] = 0;
        }
        usleep(20000);
    }
}

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
    CtbPty t;
    const pid_t pid = ctb_spawn(script, log, &t);
    free(script);
    if (pid < 0) { snprintf(out, cap, "error: cannot start the command (output file %s)", log); return; }
    Job *j = ctb_adopt(pid, log, cmd, time(NULL), &t);
    if (!j) { kill(-pid, SIGKILL); waitpid(pid, NULL, 0); ctb_pty_close(&t, 0.5); remove(log); snprintf(out, cap, "error: %d background tasks are already running; stop one with TaskStop(task_id)", JOB_N); return; }
    cmdlog_add(cmd, 0, "(started in the background)");
    const char *ask = j->mfd ? " If it stops at a question, answer it with SendInput (passwords and codes are asked from the user, not from you)." : "";
    if (prazo_s > 0) snprintf(out, cap, "%s started with ID: %d (stops after %d s). Output is being written to: %s\nRead it with the Read tool; stop it early with TaskStop(task_id=\"%d\").%s", tool, j->id, prazo_s, log, j->id, ask);
    else snprintf(out, cap, "Command running in background with ID: %d. Output is being written to: %s\nRead it with the Read tool; stop it with TaskStop(task_id=\"%d\").%s", j->id, log, j->id, ask);
}

static void ct_bash_run(const char *cmd, long long tmo_ms, char *out, size_t cap) {
    char dir[4200], log[1300], cwdf[1320];
    ctb_start_dir(dir, sizeof dir);
    ctb_logpath(log, sizeof log);
    snprintf(cwdf, sizeof cwdf, "%s.cwd", log);
    remove(cwdf);
    const int sudo = sudo_word(cmd) && sudo_prepare(cmd, 0);      /* modal/pergunta de senha, como no Bash */
    char *script = ctb_script(cmd, dir, cwdf, sudo);
    if (!script) { snprintf(out, cap, "error: out of memory"); return; }
    cmd_term_hook();
    CtbPty t;
    const pid_t pid = ctb_spawn(script, log, &t);
    free(script);
    if (pid < 0) { snprintf(out, cap, "error: cannot start the command (output file %s)", log); return; }
    const time_t t0w = time(NULL);
    g_fg_pid = pid;
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa); sa.sa_handler = cmd_on_sigint; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old);
    g_cmd_int = 0;
    g_ui_live = 1; g_live_total = 0;
    int st = 0;
    long seen = 0;
    char label[240];
    const int why = ctb_watch(pid, &t, log, (double)tmo_ms / 1000.0, 1, &seen, &st, label, sizeof label);
    int exited = why == CW_EXIT, timedout = why == CW_TIME, intr = why == CW_INTR;
    if (exited) {                                            /* o copiador grava o resto; a interface ve o fim */
        ctb_pty_close(&t, 1.0);
        char buf[8192];
        const long sz = ctb_size(log);
        while (seen < sz) { const size_t k = ctb_read(log, seen, buf, sizeof buf - 1); if (!k) break; ui_live(buf, k); if (g_cmd_tap) g_cmd_tap(buf, k); seen += (long)k; }
    }
    g_ui_live = 0;
    g_fg_pid = 0;
    sigaction(SIGINT, &old, NULL);
    char secs[32];
    snprintf(secs, sizeof secs, "%g", (double)tmo_ms / 1000.0);

    if (why == CW_PROMPT || (timedout && !ctb_is_sleep(cmd))) {      /* pergunta ou timeout: passa para segundo plano em vez de matar */
        Job *j = ctb_adopt(pid, log, cmd, t0w, &t);
        if (j) {
            remove(cwdf);
            size_t pv = cap > 900 ? cap - 700 : 200; if (pv > 2000) pv = 2000;
            char *tl = (char *)malloc(pv + 1);
            const size_t n = tl ? job_tail(j, tl, pv) : 0;
            if (why == CW_PROMPT) {
                j->asked = time(NULL); j->asked_sz = ctb_size(log);
                snprintf(out, cap, "%s\n[waiting for input: \"%s\", task %d; answer with SendInput(task_id=\"%d\", text=...)]\n"
                         "The command is still running as background task %d (output file: %s); stop it with TaskStop(task_id=\"%d\").",
                         n ? tl : "(no output yet)", label, j->id, j->id, j->id, log, j->id);
                cmdlog_add(cmd, -1, "(waiting for input)");
            } else {
                snprintf(out, cap, "Command did not complete within its %ss timeout and was moved to the background with ID: %d. Output is being written to: %s\nRead it with the Read tool; stop it with TaskStop(task_id=\"%d\").\nOutput so far:\n%s",
                         secs, j->id, log, j->id, n ? tl : "(none yet)");
                cmdlog_add(cmd, -1, "(moved to the background at the timeout)");
            }
            free(tl);
            return;
        }
        if (why == CW_PROMPT) timedout = 1;                  /* tabela cheia: nao ha como esperar a resposta */
    }
    if (!exited) {                                           /* sleep, Ctrl+C ou tabela cheia: encerra o grupo */
        kill(-pid, SIGTERM);
        for (int i = 0; i < 10 && !exited; i++) { usleep(30000); if (waitpid(pid, &st, WNOHANG) == pid) exited = 1; }
        kill(-pid, SIGKILL);
        if (!exited) waitpid(pid, &st, 0);
        ctb_pty_close(&t, 0.5);
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
        /* o meio omitido, por sentido (servico de embeddings): ate 2 janelas do meio, as mais proximas do que o modelo procura, entre o
         * inicio e o fim; o fim encolhe o que for preciso para o total nao crescer. Sem modelo, sem pedido ou saida acima de 1 MB: como antes. */
        char sem[2600]; size_t semn = 0, semc = 0;
#ifndef _WIN32
        if (sz <= (long)HT_ALL_MAX && sz > (long)(2 * half) + 600 && es_available() && *intent_get()) {
            char *all = (char *)malloc((size_t)sz + 1);
            if (all) { const size_t an = ctb_read(log, 0, all, (size_t)sz); if (an == (size_t)sz) semn = ht_sem_mid(all + half, (size_t)sz - 2 * half, sem, sizeof sem, &semc); free(all); }
        }
#endif
        size_t tailn = half;
        if (semn && half > semn + 400) tailn = half - semn; else semn = 0;
        o += (size_t)snprintf(out + o, cap - o, "%s\n[... %ld bytes omitted; full output saved to: %s ...]\n", body, sz - (long)half - (long)tailn - (long)(semn ? semc : 0), log);
        if (semn && o + semn + 80 < cap) { memcpy(out + o, sem, semn); o += semn; o += (size_t)snprintf(out + o, cap - o, "[... end of the selected parts; the end of the output follows ...]\n"); }
        n = ctb_read(log, sz - (long)tailn, body, tailn);
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
    "stays inside the project; otherwise it is reset). Commands run in a terminal. If a command stops at a question (a "
    "username, a path, yes/no), you get [waiting for input: \"<prompt>\", task <id>] and the command keeps running: answer "
    "with SendInput(task_id, text). Passwords, passphrases, PINs and verification codes are asked from the user directly; "
    "you only see [secret entered by the user]: never type a secret yourself. Common user program folders (~/.local/bin, "
    "~/.cargo/bin, ~/go/bin and similar) are already in PATH. "
    "timeout is in milliseconds (default 120000, max 600000); a command still running at the timeout is moved to the "
    "background and you get its task ID and output file. Use run_in_background for servers and long jobs; read the output "
    "file with Read and stop it with TaskStop. Large output is saved to a file and you get its path and a preview. "
    "Prefer Read, Edit, Find and Grep over cat, sed, find and grep for files.";
static const char CT_BASH_PROPS[] =
    "\"command\": {\"type\": \"string\", \"description\": \"The command to execute\"}, "
    "\"timeout\": {\"type\": \"number\", \"description\": \"Optional timeout in milliseconds (max 600000)\"}, "
    "\"description\": {\"type\": \"string\", \"description\": \"Clear, concise description of what this command does, in 5-10 words\"}, "
    "\"run_in_background\": {\"type\": \"boolean\", \"description\": \"Set to true to run this command in the background; read its output file later with Read\"}";
static const char CT_BASH_REQ[] = "\"command\"";
static void index_tree_changed(void);                       /* src/coder/indexctl.c */
static int index_bash_may_change(const char *cmd);
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
    if (index_bash_may_change(cmd)) index_tree_changed();      /* o indice do coder revalida a arvore na proxima consulta (comandos de leitura simples nao contam) */
    free(cmd);
}

static const char CT_TASKSTOP_DESC[] =
    "Stop a background task started by Bash (run_in_background, or moved to the background at its timeout) or by Monitor. "
    "The whole process group is terminated. Its output file stays readable with Read.";
static const char CT_TASKSTOP_PROPS[] = "\"task_id\": {\"type\": \"string\", \"description\": \"The ID of the background task to stop\"}";
static const char CT_TASKSTOP_REQ[] = "\"task_id\"";
static void ct_taskstop(const char *js, const char *end, char *out, size_t cap) {
    char *id = ct_str(js, end, "task_id");
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

/* SendInput: escreve no terminal de uma tarefa deste agente e devolve a saida nova. O texto nao aparece no resultado, no log de
 * comandos nem nos marcadores (job_redact, agent/jobs.c). Se a tarefa esta parada numa pergunta de segredo, o texto do modelo
 * e descartado e o katana pede o segredo ao usuario (ctb_secret_answer). Depois de escrever, espera como o Bash: ate a tarefa
 * acabar, parar noutra pergunta ou 20 s. */
#define CT_SENDINPUT_MAX 4096
static const char CT_SENDINPUT_DESC[] =
    "Type text into a running task (a Bash command that stopped at a question, or one started with run_in_background or "
    "Monitor), as if typed at its prompt: a username, a path, a yes/no answer, a menu choice. A newline is added unless "
    "newline is false; eof=true sends end of input (Ctrl+D) after the text. Returns the new output: it waits until the task "
    "ends, stops at its next question ([waiting for input: ...]) or 20 s pass. Never type passwords, passphrases, PINs or "
    "verification codes: when the task asks for a secret, katana asks the user directly and your text is discarded. At most "
    "4096 bytes per call.";
static const char CT_SENDINPUT_PROPS[] =
    "\"task_id\": {\"type\": \"string\", \"description\": \"The ID of the task\"}, "
    "\"text\": {\"type\": \"string\", \"description\": \"What to type (not echoed in logs)\"}, "
    "\"newline\": {\"type\": \"boolean\", \"description\": \"Add a newline after text (default true)\"}, "
    "\"eof\": {\"type\": \"boolean\", \"description\": \"Send end of input (Ctrl+D) after the text (default false)\"}";
static const char CT_SENDINPUT_REQ[] = "\"task_id\"";
static void ct_sendinput(const char *js, const char *end, char *out, size_t cap) {
    char *id = ct_str(js, end, "task_id");
    const char *d = id; while (d && *d && !isdigit((unsigned char)*d)) d++;
    const int n = d && *d ? atoi(d) : 0;
    free(id);
    const char *a, *b;
    size_t tl = 0;
    const int eof = ct_find(js, end, "eof", &a, &b) && ct_bool(js, end, "eof");
    char *text = ct_find(js, end, "text", &a, &b) && *a == '"' ? ct_unquote(a, b, &tl) : NULL;
    if (!text && !eof) { snprintf(out, cap, "error: missing text (what to type into the task)"); return; }
    if (!text) { text = (char *)calloc(1, 1); tl = 0; if (!text) { snprintf(out, cap, "error: out of memory"); return; } }
    int nl = tl > 0 || !eof;
    if (ct_find(js, end, "newline", &a, &b)) nl = ct_bool(js, end, "newline");
    if (tl + (size_t)nl > CT_SENDINPUT_MAX) { memset(text, 0, tl); free(text); snprintf(out, cap, "error: text is too long (%zu bytes; at most %d)", tl, CT_SENDINPUT_MAX); return; }
    for (int i = 0; i < JOB_N; i++) job_poll(&g_jobs[i]);
    Job *j = NULL;
    for (int i = 0; i < JOB_N; i++) if (n && g_jobs[i].id == n) j = &g_jobs[i];
    if (!j || j->done || !j->mfd) {
        memset(text, 0, tl); free(text);
        if (!j) {
            size_t o = (size_t)snprintf(out, cap, n ? "error: there is no task %d." : "error: missing task_id.", n);
            int k = 0;
            for (int i = 0; i < JOB_N && o + 300 < cap; i++) if (g_jobs[i].id && !g_jobs[i].done) o += (size_t)snprintf(out + o, cap - o, "%s %d (%s)", k++ ? "," : " Running tasks:", g_jobs[i].id, g_jobs[i].cmd);
            if (!k && o < cap) snprintf(out + o, cap - o, " No background task is running.");
        } else if (j->done) snprintf(out, cap, "error: task %d is not running (it exited with code %d); nothing was typed. Its output is in %s", j->id, j->rc, j->log);
        else snprintf(out, cap, "error: task %d has no terminal and cannot take input; stop it with TaskStop and run the command again", j->id);
        return;
    }
    if (!agent_act("responder ao comando em segundo plano", j->cmd)) { memset(text, 0, tl); free(text); snprintf(out, cap, "error: the user denied this action"); return; }
    const int m = j->mfd - 1;
    const long from = ctb_size(j->log);
    char label[240];
    int lnl = 0, secret = 0;
    {                                                        /* parada numa pergunta de segredo: o usuario responde, nao o modelo */
        const int r = ctb_reading(j->pid, j->slave);
        ctb_last_line(j->log, label, sizeof label, &lnl);
        if ((r == 1 || (r < 0 && !lnl && ctb_prompt_like(label))) && ctb_is_secret(m, label)) secret = 1;
    }
    size_t put = 0;
    int err = 0;
    if (secret) {
        memset(text, 0, tl); free(text);
        ctb_secret_answer(m, j->log, label, 0);
    } else {
        char *w = (char *)malloc(tl + 3);
        if (!w) { memset(text, 0, tl); free(text); snprintf(out, cap, "error: out of memory"); return; }
        memcpy(w, text, tl);
        memset(text, 0, tl); free(text);
        size_t wl = tl;
        if (nl) w[wl++] = '\n';
        if (eof) w[wl++] = 4;                                /* VEOF: fim da entrada para quem le o terminal */
        put = ctb_write(m, w, wl, &err);
        memset(w, 0, wl); free(w);
        if (err) { snprintf(out, cap, "error: could not type into task %d (%s; %zu of %zu bytes written): the program is not reading its input. Read its output in %s", j->id, strerror(err), put, wl, j->log); return; }
    }
    j->asked = 0;
    CtbPty t;
    t.mfd = j->mfd; t.pump = 0;
    snprintf(t.slave, sizeof t.slave, "%s", j->slave);
    int st = 0;
    long seen = 0;
    const int why = ctb_watch(j->pid, &t, j->log, 20.0, 0, &seen, &st, label, sizeof label);
    if (why == CW_EXIT) {
        j->done = 1; j->rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        CtbPty jt; jt.mfd = j->mfd; jt.pump = j->pump; jt.slave[0] = 0;
        ctb_pty_close(&jt, 1.0);                             /* o copiador grava o resto antes da leitura */
        j->mfd = 0; j->pump = 0;
    }
    const long sz = ctb_size(j->log);
    size_t o;
    if (secret) o = (size_t)snprintf(out, cap, "Task %d was asking for a secret; it was asked from the user and your text was discarded.", j->id);
    else o = (size_t)snprintf(out, cap, "Typed %zu bytes into task %d%s%s.", tl, j->id, nl ? " followed by a newline" : "", eof ? " and end of input" : "");
    if (o >= cap) o = cap - 1;
    if (why == CW_EXIT) o += (size_t)snprintf(out + o, cap - o, " The task has exited with code %d.", j->rc);
    else if (why == CW_PROMPT) {
        j->asked = time(NULL); j->asked_sz = sz;
        o += (size_t)snprintf(out + o, cap - o, " [waiting for input: \"%s\", task %d; answer with SendInput(task_id=\"%d\", text=...)]", label, j->id, j->id);
    } else o += (size_t)snprintf(out + o, cap - o, " The task is still running; read %s later or stop it with TaskStop.", j->log);
    if (o >= cap) return;
    if (sz <= from) { snprintf(out + o, cap - o, " No new output."); return; }
    size_t lim = cap > o + 800 ? cap - 600 - o : 200; if (lim > 3000) lim = 3000;
    const long start = sz - from > (long)lim ? sz - (long)lim : from;
    char *body = (char *)malloc(lim + 1);
    if (!body) return;
    size_t k = ctb_read(j->log, start, body, (size_t)(sz - start));
    size_t s = 0;
    if (start > from) { char *nlp = memchr(body, '\n', k); s = nlp ? (size_t)(nlp + 1 - body) : 0; }
    while (k > s && (body[k - 1] == '\n' || body[k - 1] == '\r')) body[--k] = 0;
    snprintf(out + o, cap - o, "\nNew output%s:\n%s", start > from ? " (last lines)" : "", body + s);
    free(body);
}
#else
static const char CT_BASH_DESC[] = "Run a shell command (not available on Windows yet)";
static const char CT_BASH_PROPS[] = "\"command\": {\"type\": \"string\"}, \"timeout\": {\"type\": \"number\"}, \"description\": {\"type\": \"string\"}, \"run_in_background\": {\"type\": \"boolean\"}";
static const char CT_BASH_REQ[] = "\"command\"";
static void ct_bash(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: Bash is not available on Windows yet"); }
static const char CT_TASKSTOP_DESC[] = "Stop a background task (not available on Windows yet)";
static const char CT_TASKSTOP_PROPS[] = "\"task_id\": {\"type\": \"string\"}";
static const char CT_TASKSTOP_REQ[] = "\"task_id\"";
static void ct_taskstop(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: TaskStop is not available on Windows yet"); }
static const char CT_MONITOR_DESC[] = "Watch a command's output (not available on Windows yet)";
static const char CT_MONITOR_PROPS[] = "\"command\": {\"type\": \"string\"}, \"description\": {\"type\": \"string\"}, \"timeout_ms\": {\"type\": \"number\"}";
static const char CT_MONITOR_REQ[] = "\"command\", \"description\"";
static void ct_monitor(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: Monitor is not available on Windows yet"); }
static const char CT_SENDINPUT_DESC[] = "Type text into a background task (not available on Windows yet)";
static const char CT_SENDINPUT_PROPS[] = "\"task_id\": {\"type\": \"string\"}, \"text\": {\"type\": \"string\"}, \"newline\": {\"type\": \"boolean\"}, \"eof\": {\"type\": \"boolean\"}";
static const char CT_SENDINPUT_REQ[] = "\"task_id\"";
static void ct_sendinput(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: SendInput is not available on Windows yet"); }
#endif
