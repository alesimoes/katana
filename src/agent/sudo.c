/* ---------- sudo: deteccao de pedido de senha, pergunta ao usuario (terminal ou modal do desktop), arquivos como root ---------- */
#ifndef _WIN32
/* 0 = nada; 1 = pede senha/privilegio (stdin fechado); 2 = "permission denied" generico */
static int pw_detect(const char *o) {
    static const char *strong[] = { "a password is required", "[sudo] password", "sudo: a terminal is required", "no tty present",
        "Enter passphrase", "Permission denied (publickey", "Permission denied, please try again", "password for ", "Password:", NULL };
    for (int i = 0; strong[i]; i++) if (strstr(o, strong[i])) return 1;
    return strstr(o, "Permission denied") ? 2 : 0;
}

/* run_command: stdin fechado, stderr+stdout, timeout, codigo de saida, inicio+fim da saida, dica de senha */
/* sudo: se o comando usa sudo e ele pede senha, pergunta ao usuario uma vez (terminal ou, no desktop, marcador NEEDPASS) e
 * guarda so em memoria; os comandos seguintes usam sudo -A com um askpass que le a senha do ambiente do filho.
 * Devolve 1 se o comando deve rodar com o sudo -A. */
static int sudo_word(const char *cmd) {
    for (const char *q = cmd; (q = strstr(q, "sudo")); q += 4)
        if ((q == cmd || !isalnum((unsigned char)q[-1]) && q[-1] != '_' && q[-1] != '-') && (q[4] == 0 || q[4] == ' ' || q[4] == '\t')) return 1;
    return 0;
}
/* senha para as sessoes de terminal: arquivo 0600 em tmpfs (XDG_RUNTIME_DIR), lido pelo askpass; apagado ao sair */
static char g_pwfile[512];
static const char *pwfile_path(void) {
    if (!g_pwfile[0]) {
        const char *b = getenv("XDG_RUNTIME_DIR");
        struct stat st;
        if (!b || !*b || stat(b, &st) != 0 || !S_ISDIR(st.st_mode)) b = "/tmp";
        snprintf(g_pwfile, sizeof g_pwfile, "%s/katana-pw-%d", b, (int)getpid());
    }
    return g_pwfile;
}
static void pwfile_remove(void) { if (g_pwfile[0]) unlink(g_pwfile); }
static void pwfile_write(void) {
    static int reg;
    const int fd = open(pwfile_path(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    if (fchmod(fd, 0600) != 0) {}
    if (write(fd, g_sudo_pw, strlen(g_sudo_pw)) < 0) {}
    if (write(fd, "\n", 1) < 0) {}
    close(fd);
    if (!reg) { atexit(pwfile_remove); reg = 1; }
}
static int sudo_ask(char *pw, size_t cap, int retry) {
    pw[0] = 0;
    if (getenv("KATANA_UI")) {
        printf("\x01" "NEEDPASS%s\n", retry ? " retry" : ""); fflush(stdout);
        char l[512];
        if (!read_line(l, sizeof l)) return 0;
        if (strncmp(l, "\x01PASS ", 6) != 0) return 0;
        snprintf(pw, cap, "%s", l + 6);
        return pw[0] != 0;
    }
    if (!isatty(0)) return 0;
    struct termios t0, t1;
    fprintf(stderr, "%s[sudo] password for the agent (kept only in memory for this session): ", retry ? "wrong password, try again. " : ""); fflush(stderr);
    const int hv = tcgetattr(0, &t0) == 0;
    if (hv) { t1 = t0; t1.c_lflag &= ~(tcflag_t)ECHO; tcsetattr(0, TCSANOW, &t1); }
    const int ok = read_line(pw, cap);
    if (hv) tcsetattr(0, TCSANOW, &t0);
    fprintf(stderr, "\n");
    return ok && pw[0];
}
static int root_exec(const char *cmd, int tmo, HT *ht, int *rc, int *timedout, int *interrupted);   /* shell root persistente (pty) */
static int g_sudo_agent;   /* 1 so dentro do agente interativo (nos testes de ferramenta nao pede senha sozinho) */
static int sudo_default(void) { const char *e = getenv("KATANA_SUDO"); return (g_sudo_agent || getenv("KATANA_SUDO_FORCE")) && e && strcmp(e, "1") == 0; }   /* padrao: usuario normal; root so quando o comando pede sudo (KATANA_SUDO=1 roda tudo como root) */
static int sudo_prepare(const char *cmd, int force) {
    if (!force && !sudo_word(cmd)) return 0;
    if (!g_sudo_pw[0]) {
        int rc = 1; char *o = run_collect("sudo -n true 2>&1", 5, 400, &rc); free(o);
        if (rc == 0) return 0;                                /* sem senha (cache do sudo ou NOPASSWD) */
        const char *home = getenv("HOME");
        if (!home) return 0;
        for (int attempt = 0; attempt < 3; attempt++) {      /* senha errada: pergunta de novo (ate 3 vezes) */
            char pw[256];
            if (!sudo_ask(pw, sizeof pw, attempt > 0)) return 0;                 /* cancelou */
            char dir[560]; snprintf(dir, sizeof dir, "%s/.katana", home); mkdir(dir, 0700);
            snprintf(g_askpass_path, sizeof g_askpass_path, "%s/.katana/askpass.sh", home);
            FILE *f = fopen(g_askpass_path, "w");
            if (!f) { g_askpass_path[0] = 0; return 0; }
            fputs("#!/bin/sh\nif [ -n \"$KATANA_PWFILE\" ] && [ -r \"$KATANA_PWFILE\" ]; then cat \"$KATANA_PWFILE\"; else printf '%s\\n' \"$KATANA_PW\"; fi\n", f); fclose(f);
            chmod(g_askpass_path, 0700);
            snprintf(g_sudo_pw, sizeof g_sudo_pw, "%s", pw);
            memset(pw, 0, sizeof pw);
            o = run_collect("sudo -A -k -v 2>&1", 10, 400, &rc); free(o);
            if (rc == 0) { pwfile_write(); break; }
            memset(g_sudo_pw, 0, sizeof g_sudo_pw);
            if (attempt == 2) return 0;
        }
    }
    return 1;
}

/* sudo utilizavel agora? (senha ja guardada, ou o sudo nao pede senha). Abre o modal de senha se for preciso. */
static int sudo_ready(void) {
    sudo_prepare("", 1);
    if (g_sudo_pw[0]) return 1;
    int rc = 1; char *o = run_collect("sudo -n true 2>&1", 5, 400, &rc); free(o);
    return rc == 0;
}

static const char *sudo_prefix(void) { return g_sudo_pw[0] ? "sudo -A" : "sudo -n"; }

/* gravar como root quando o sistema nega (permissao): dados num arquivo temporario do usuario e um script que o sudo executa */
static int root_write_file(const char *dest, const char *data, size_t n, int append, char *err, size_t ecap) {
    if (!sudo_ready()) { snprintf(err, ecap, "error: permission denied writing %s and sudo is not available (the password was not given)", dest); return 0; }
    char tmpd[] = "/tmp/katana-wr-XXXXXX", fd_[300], fs_[300];
    if (!mkdtemp(tmpd)) { snprintf(err, ecap, "error: cannot create a temporary folder"); return 0; }
    snprintf(fd_, sizeof fd_, "%s/data", tmpd);
    snprintf(fs_, sizeof fs_, "%s/run.sh", tmpd);
    FILE *f = fopen(fd_, "wb");
    if (!f || fwrite(data, 1, n, f) != n) { if (f) fclose(f); snprintf(err, ecap, "error: cannot write the temporary file"); return 0; }
    fclose(f);
    char qd[4700], qt[700];
    if (!shq(dest, qd, sizeof qd) || !shq(fd_, qt, sizeof qt)) { snprintf(err, ecap, "error: path too long"); return 0; }
    f = fopen(fs_, "wb");
    if (!f) { snprintf(err, ecap, "error: cannot write the temporary script"); return 0; }
    fprintf(f, "set -e\nD=%s\nT=%s\ndir=$(dirname \"$D\")\nmkdir -p \"$dir\"\nnew=0; [ -e \"$D\" ] || new=1\ncat \"$T\" %s \"$D\"\n"
               "if [ $new = 1 ]; then chown --reference=\"$dir\" \"$D\" 2>/dev/null || true; fi\n", qd, qt, append ? ">>" : ">");
    fclose(f);
    char cmd[900];
    snprintf(cmd, sizeof cmd, "%s sh %s 2>&1", g_sudo_pw[0] ? "sudo -A" : "sudo -n", fs_);
    int rc = 1; char *o = run_collect(cmd, 30, 1500, &rc);
    unlink(fd_); unlink(fs_); rmdir(tmpd);
    if (rc != 0) { snprintf(err, ecap, "error: could not write %s as root: %.300s", dest, o ? o : ""); free(o); return 0; }
    free(o);
    return 1;
}

/* ler como root (arquivo sem permissao de leitura); malloc, NULL se falhar */
static char *root_read_file(const char *path, size_t *n) {
    if (!sudo_ready()) return NULL;
    char q[4700], cmd[5000];
    if (!shq(path, q, sizeof q)) return NULL;
    snprintf(cmd, sizeof cmd, "%s cat -- %s 2>&1", g_sudo_pw[0] ? "sudo -A" : "sudo -n", q);
    int rc = 1; char *o = run_collect(cmd, 30, 8000000, &rc);
    if (!o || rc != 0) { free(o); return NULL; }
    *n = strlen(o);
    return o;
}
#endif
