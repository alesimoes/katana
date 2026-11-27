/* ---------- sudo: deteccao de pedido de senha, pergunta ao usuario (terminal ou modal do desktop), arquivos como root ---------- */
#ifndef _WIN32
/* 0 = nada; 1 = pede senha/privilegio (stdin fechado); 2 = "permission denied" generico */
static int pw_detect(const char *o) {
    static const char *strong[] = { "a password is required", "[sudo] password", "sudo: a terminal is required", "no tty present",
        "Enter passphrase", "Permission denied (publickey", "Permission denied, please try again", "password for ", "Password:", NULL };
    for (int i = 0; strong[i]; i++) if (strstr(o, strong[i])) return 1;
    return strstr(o, "Permission denied") ? 2 : 0;
}

/* Bash: stdin fechado, stderr+stdout, timeout, codigo de saida, inicio+fim da saida, dica de senha */
/* sudo: se o comando usa sudo e ele pede senha, pergunta ao usuario uma vez (terminal ou, no desktop, marcador NEEDPASS) e
 * guarda so em memoria; os comandos seguintes usam sudo -A com um askpass que le a senha do ambiente do filho.
 * Devolve 1 se o comando deve rodar com o sudo -A. */
static int sudo_word(const char *cmd) {
    for (const char *q = cmd; (q = strstr(q, "sudo")); q += 4)
        if ((q == cmd || (!isalnum((unsigned char)q[-1]) && q[-1] != '_' && q[-1] != '-')) && (q[4] == 0 || q[4] == ' ' || q[4] == '\t')) return 1;
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
static void launch_spin_pause(void);                 /* src/app/launch.c: o spinner sai da frente da pergunta */
static int sudo_ask(char *pw, size_t cap, int retry) {
    pw[0] = 0;
    launch_spin_pause();
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
/* resposta do usuario a uma pergunta de um comando do Bash (src/tools/bash.c). masked = 1: segredo (senha, frase secreta, PIN,
 * codigo, token), vai direto do usuario para o terminal do comando, sem passar pelo modelo, pelo log nem pelo historico;
 * masked = 0: pergunta comum que o modelo nao soube responder (ou cuja resposta foi recusada), o texto volta ao modelo.
 * Na interface (KATANA_UI): marcador "\x01ASKINPUT <id> <masked> <pergunta do terminal>\t<pergunta do modelo>\t<contexto>\t<dicas>"
 * (contexto com \n e \\ escapados; dicas separadas por \x1f, a primeira e a melhor) e a resposta chega na linha
 * "\x01ASKINPUT <id>\t<texto>" (ou "\x01ASKINPUTCANCEL <id>"). No terminal: le de /dev/tty (eco desligado se masked; Enter
 * vazio aceita a primeira dica), com limite de KATANA_PROMPT_MAX segundos. Sem interface nem terminal: cancela.
 * 1 = out tem a resposta; 0 = cancelado. */
static void ia_clean(const char *s, char *o, size_t cap, int esc) {
    size_t n = 0;
    for (const char *q = s ? s : ""; *q && n + 3 < cap; q++) {
        const unsigned char c = (unsigned char)*q;
        if (esc && c == '\\') { o[n++] = '\\'; o[n++] = '\\'; }
        else if (esc && c == '\n') { o[n++] = '\\'; o[n++] = 'n'; }
        else o[n++] = c < 0x20 && c != 0x1f ? ' ' : (char)c;
    }
    while (n && o[n - 1] == ' ') n--;
    o[n] = 0;
}
static int input_ask(int id, int masked, const char *label, const char *question, const char *ctx, const char *hints, char *out, size_t cap) {
    out[0] = 0;
    launch_spin_pause();
    static char lb[240], qs[600], cx[2000], hs[3000];
    ia_clean(label, lb, sizeof lb, 0);
    ia_clean(question, qs, sizeof qs, 0);
    ia_clean(ctx, cx, sizeof cx, 1);
    ia_clean(masked ? "" : hints, hs, sizeof hs, 0);
    if (getenv("KATANA_UI")) {
        printf("\x01" "ASKINPUT %d %d %s\t%s\t%s\t%s\n", id, masked ? 1 : 0, lb, qs, cx, hs); fflush(stdout);
        static char l[4200];
        char pre[48];
        const int pl = snprintf(pre, sizeof pre, "\x01" "ASKINPUT %d\t", id);
        int ok = 0;
        if (read_line(l, sizeof l) && strncmp(l, pre, (size_t)pl) == 0) { snprintf(out, cap, "%s", l + pl); ok = 1; }
        memset(l, 0, sizeof l);                              /* ASKINPUTCANCEL ou qualquer outra linha: cancela */
        return ok;
    }
    const int fd = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return 0;
    static char msg[6000];
    int ml = 0;
    if (masked) ml = snprintf(msg, sizeof msg, "\n[katana] the command asks for a secret; type it here (it is not shown to the model). %s ", lb[0] ? lb : "Secret:");
    else {
        ml = snprintf(msg, sizeof msg, "\n[katana] the command is waiting for input and needs your answer%s%s\n%.1900s\n", qs[0] ? ": " : ".", qs, ctx ? ctx : "");
        int nh = 0;
        for (const char *q = hs; *q && ml < (int)sizeof msg - 600; nh++) {
            const size_t l = strcspn(q, "\x1f");
            ml += snprintf(msg + ml, sizeof msg - (size_t)ml, "%s  %.*s\n", nh ? "" : "suggestions (an empty line accepts the first):\n", (int)(l < 500 ? l : 500), q);
            q += l + (q[l] ? 1 : 0);
        }
        if (ml < (int)sizeof msg - 260) ml += snprintf(msg + ml, sizeof msg - (size_t)ml, "%s ", lb[0] ? lb : "Answer:");
    }
    if (ml > (int)sizeof msg - 1) ml = (int)sizeof msg - 1;
    if (write(fd, msg, (size_t)ml) < 0) {}
    struct termios t0, t1;
    const int hv = tcgetattr(fd, &t0) == 0;
    if (hv) { t1 = t0; if (masked) t1.c_lflag &= ~(tcflag_t)(ECHO | ECHONL); else t1.c_lflag |= ECHO; t1.c_lflag |= ICANON | ISIG; tcsetattr(fd, TCSANOW, &t1); }
    size_t o = 0; int ok = 0;
    const char *e = getenv("KATANA_PROMPT_MAX");
    const int wait_ms = (e && atoi(e) > 0 ? atoi(e) : 900) * 1000;
    for (;;) {
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, wait_ms) <= 0) break;               /* sem resposta no prazo (ou Ctrl+C): cancela */
        char ch;
        const ssize_t k = read(fd, &ch, 1);
        if (k <= 0) break;
        if (ch == '\n' || ch == '\r') { ok = 1; break; }
        if (o + 1 < cap) out[o++] = ch;
    }
    out[o] = 0;
    if (ok && !o && !masked && hs[0]) { const size_t l = strcspn(hs, "\x1f"); snprintf(out, cap, "%.*s", (int)l, hs); }
    if (hv) tcsetattr(fd, TCSANOW, &t0);
    if (masked && write(fd, "\n", 1) < 0) {}
    close(fd);
    if (!ok) memset(out, 0, cap);
    return ok;
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




#endif
