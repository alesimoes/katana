/* ---------- vigia dos comandos: nada do que o agente lancou sobrevive a ele ----------
 * Os comandos do agente rodam em grupos proprios (setsid). atexit e o tratador de SIGTERM nao cobrem tudo: com SIGKILL
 * (tempo esgotado de quem chamou, falta de memoria) nenhum codigo do katana roda, e com SIGTERM so o comando da frente
 * morria; os trabalhos em segundo plano ficavam orfaos por horas. O vigia e um processo pequeno (o proprio binario,
 * "katana __reap") na outra ponta de um canal que so o katana segura: quando o katana morre, por qualquer motivo, o
 * canal fecha e o vigia encerra os grupos registrados que ainda existem (SIGTERM, ate 1 s, SIGKILL).
 * PR_SET_PDEATHSIG nao serve aqui: so alcanca o shell (nao os netos) e dispara quando a THREAD que o criou termina.
 * Quem sai do grupo de proposito (setsid proprio) continua fora do alcance, como um servico iniciado pelo usuario. */
#if defined(__linux__)
#include <sys/socket.h>
#include <poll.h>
static int g_reap_fd = -1, g_reap_tried;

/* processo vigia: canal na entrada padrao, uma mensagem (pid_t) por grupo */
static int reap_alive(pid_t g) { return kill(-g, 0) == 0 || errno == EPERM || kill(g, 0) == 0; }
static int reap_main(void) {
    enum { MAXG = 1024 };
    static pid_t pg[MAXG];
    int n = 0;
    signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN);
    for (;;) {
        struct pollfd p = { 0, POLLIN, 0 };
        const int w = poll(&p, 1, n ? 5000 : -1);
        if (w < 0) { if (errno == EINTR) continue; break; }
        if (w > 0) {
            pid_t g = 0;
            const ssize_t k = recv(0, &g, sizeof g, 0);
            if (k < 0 && errno == EINTR) continue;
            if (k != (ssize_t)sizeof g) break;                /* o katana se foi */
            if (g > 1 && n < MAXG) pg[n++] = g;
        }
        for (int i = 0; i < n; ) if (reap_alive(pg[i])) i++; else pg[i] = pg[--n];      /* grupo vazio: esquece (o numero pode ser reusado) */
    }
    for (int i = 0; i < n; i++) kill(-pg[i], SIGTERM);
    for (int t = 0; t < 20; t++) {
        int left = 0;
        for (int i = 0; i < n; i++) if (kill(-pg[i], 0) == 0) left++;
        if (!left) return 0;
        usleep(50000);
    }
    for (int i = 0; i < n; i++) kill(-pg[i], SIGKILL);
    return 0;
}

/* registra o grupo de um comando recem-criado; o vigia nasce no primeiro registro */
static void reap_add(pid_t pgid) {
    if (pgid <= 1) return;
    if (g_reap_fd < 0) {
        if (g_reap_tried) return;
        g_reap_tried = 1;
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) return;
        const int dn = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (dn < 0) { close(sv[0]); close(sv[1]); return; }
        const pid_t pid = vfork();
        if (pid == 0) {
            setsid();
            dup2(sv[1], 0); dup2(dn, 1); dup2(dn, 2);
            execl("/proc/self/exe", "katana", "__reap", (char *)NULL);
            _exit(127);
        }
        close(sv[1]); close(dn);
        if (pid < 0) { close(sv[0]); return; }
        g_reap_fd = sv[0];
    }
    if (send(g_reap_fd, &pgid, sizeof pgid, MSG_NOSIGNAL) < 0) { close(g_reap_fd); g_reap_fd = -1; }
}
/* o katana vai se recarregar com execv (agent_regrow): o canal atravessa, os comandos continuam */
static void reap_keep_exec(int on) {
    if (g_reap_fd < 0) return;
    fcntl(g_reap_fd, F_SETFD, on ? 0 : FD_CLOEXEC);
    char v[16]; snprintf(v, sizeof v, "%d", g_reap_fd);
    if (on) setenv("KATANA_REAP_FD", v, 1); else unsetenv("KATANA_REAP_FD");
}
/* inicio do processo: vira vigia ("__reap") ou retoma o canal deixado por reap_keep_exec */
static void reap_start(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "__reap") == 0) exit(reap_main());
    const char *e = getenv("KATANA_REAP_FD");
    if (!e) return;
    const int fd = atoi(e);
    struct stat st;
    unsetenv("KATANA_REAP_FD");
    if (fd > 2 && fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode)) { fcntl(fd, F_SETFD, FD_CLOEXEC); g_reap_fd = fd; g_reap_tried = 1; }
}
#else
static void reap_add(long pgid) { (void)pgid; }
static void reap_keep_exec(int on) { (void)on; }
static void reap_start(int argc, char **argv) { (void)argc; (void)argv; }
#endif
