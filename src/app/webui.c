/* ---------- katana web: a pagina do agente, no navegador (docs/web.md) ----------
 * katana serve --agent [porta] (ou katana web). A pagina (web/: HTML, CSS, JS, fontes e icones) entra no binario no build
 * (src/app/web.inc, gerado por tools/genweb.sh a partir de web/ e dos .js do desktop) e e entregue por este arquivo.
 * O agente e o MESMO processo filho que o desktop usa (katana agent <modelo>, KATANA_UI=1, NO_COLOR=1): o que ele escreve
 * (texto e linhas de marcadores \x01...) vira uma sequencia de eventos numerados (anel em memoria) que a pagina le por
 * Server-Sent Events e traduz em cartoes, como o backend do desktop faz. A pagina escreve linhas no agente por POST /web/line.
 * Seguranca: so conexoes do loopback; Host e Origin conferidos (anti DNS rebinding); uma chave aleatoria por execucao vai na
 * URL impressa (/?token=...), troca por um cookie HttpOnly SameSite=Strict e sai da barra de enderecos; todo POST exige o
 * cabecalho X-Katana (o navegador nao o manda de outro site sem pre-verificacao CORS, que o servidor nao responde).
 * Terminal embutido: um bash (via script, como o desktop) por pty. So Linux. */

#ifndef _WIN32
#include <dirent.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
extern char **environ;

typedef struct { const char *path, *mime; size_t len; const char *b64; } WebFile;
#if defined(__has_include)
#if __has_include("app/web.inc")
#include "app/web.inc"
#else
static const WebFile WEB_FILES[] = { { NULL, NULL, 0, NULL } };
#endif
#else
static const WebFile WEB_FILES[] = { { NULL, NULL, 0, NULL } };
#endif

static const char *models_dir(void);
static int web_teacher_has_key(void);

/* ---- arquivos embutidos (base64 decodificado uma vez, quando pedidos) ---- */
static unsigned char *g_web_cache[256];
static pthread_mutex_t g_web_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static const unsigned char *web_file_data(int i) {
    pthread_mutex_lock(&g_web_cache_mu);
    if (!g_web_cache[i]) {
        const WebFile *f = &WEB_FILES[i];
        unsigned char *o = xalloc(f->len + 4, 1);
        int val[256];
        for (int k = 0; k < 256; k++) val[k] = -1;
        for (int k = 0; k < 26; k++) { val['A' + k] = k; val['a' + k] = 26 + k; }
        for (int k = 0; k < 10; k++) val['0' + k] = 52 + k;
        val['+'] = 62; val['/'] = 63;
        size_t n = 0; unsigned acc = 0; int bits = 0;
        for (const unsigned char *p = (const unsigned char *)f->b64; *p; p++) {
            if (val[*p] < 0) continue;
            acc = (acc << 6) | (unsigned)val[*p]; bits += 6;
            if (bits >= 8) { bits -= 8; if (n < f->len) o[n++] = (unsigned char)((acc >> bits) & 255); }
        }
        g_web_cache[i] = o;
    }
    pthread_mutex_unlock(&g_web_cache_mu);
    return g_web_cache[i];
}

/* ---- resposta HTTP com cabecalhos extras ---- */
static void web_reply(sock_t s, int code, const char *ctype, const char *extra, const void *body, size_t n, int csp) {
    char h[1400];
    const char *st = code == 200 ? "OK" : code == 302 ? "Found" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" :
                     code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" :
                     code == 409 ? "Conflict" : code == 413 ? "Payload Too Large" : code == 500 ? "Internal Server Error" : "Error";
    const int k = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                           "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n%s%sConnection: close\r\n\r\n",
                           code, st, ctype, n,
                           csp ? "Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
                                 "script-src 'self'; connect-src 'self'; font-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n" : "",
                           extra ? extra : "");
    sock_send(s, h, (size_t)k);
    if (n) sock_send(s, (const char *)body, n);
}
static void web_json(sock_t s, int code, const Buf *b) { web_reply(s, code, "application/json; charset=utf-8", NULL, b->p ? b->p : "", b->n, 0); }
static void web_err(sock_t s, int code, const char *msg) {
    Buf b = {0}; buf_str(&b, "{\"error\":\""); js_escape(&b, msg, strlen(msg)); buf_str(&b, "\"}");
    web_json(s, code, &b); free(b.p);
}
static void web_ok(sock_t s) { web_reply(s, 200, "application/json; charset=utf-8", NULL, "{\"ok\":true}", 11, 0); }

/* ---- fluxo de eventos numerados (anel) ---- */
#define WEB_RING 4096
#define WEB_RING_BYTES (24u << 20)
typedef struct { char *s; size_t n; } WSlot;
typedef struct {
    pthread_mutex_t mu; pthread_cond_t cv;
    WSlot slot[WEB_RING];
    unsigned long next, first;                   /* numero do proximo evento; do mais antigo guardado */
    size_t bytes;
} WStream;
static WStream g_wev = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, {{0}}, 0, 0, 0 };
static WStream g_wterm = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, {{0}}, 0, 0, 0 };

static void ws_push(WStream *w, const char *type, const char *json, size_t n) {
    char h[96];
    pthread_mutex_lock(&w->mu);
    const int k = snprintf(h, sizeof h, "id: %lu\nevent: %s\ndata: ", w->next, type);
    const size_t tot = (size_t)k + n + 2;
    char *e = xalloc(tot + 1, 1);
    memcpy(e, h, (size_t)k); memcpy(e + k, json, n); memcpy(e + k + n, "\n\n", 2);
    WSlot *sl = &w->slot[w->next % WEB_RING];
    if (sl->s) { w->bytes -= sl->n; free(sl->s); w->first = w->next - WEB_RING + 1; }
    sl->s = e; sl->n = tot; w->bytes += tot; w->next++;
    while (w->first + 1 < w->next && w->bytes > WEB_RING_BYTES) {     /* orcamento de memoria: descarta os mais antigos */
        WSlot *o = &w->slot[w->first % WEB_RING];
        if (o->s) { w->bytes -= o->n; free(o->s); o->s = NULL; o->n = 0; }
        w->first++;
    }
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
}
static void ws_clear(WStream *w) {               /* nova sessao do processo: o numero continua, o conteudo velho sai */
    pthread_mutex_lock(&w->mu);
    for (unsigned long i = w->first; i < w->next; i++) { WSlot *o = &w->slot[i % WEB_RING]; free(o->s); o->s = NULL; o->n = 0; }
    w->first = w->next; w->bytes = 0;
    pthread_mutex_unlock(&w->mu);
}
static void ws_push_str(WStream *w, const char *type, const char *key, const char *txt, size_t n) {
    Buf b = {0};
    buf_str(&b, "{\""); buf_str(&b, key); buf_str(&b, "\":\""); js_escape(&b, txt, n); buf_str(&b, "\"}");
    ws_push(w, type, b.p, b.n); free(b.p);
}

/* GET /web/events, /web/term/events: SSE a partir do numero pedido (Last-Event-ID dos reconectados, ou ?from=) */
static void web_sse(sock_t s, WStream *w, unsigned long from) {
    const char *hd = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-store\r\n"
                     "X-Accel-Buffering: no\r\nConnection: close\r\n\r\n";
    if (!sock_send(s, hd, strlen(hd))) return;
    unsigned long cur = from;
    Buf out = {0};
    for (;;) {
        buf_clear(&out);
        pthread_mutex_lock(&w->mu);
        if (cur > w->next) { buf_str(&out, "event: reset\ndata: {}\n\n"); cur = w->first; }      /* o servidor reiniciou: recomeca */
        if (cur >= w->next) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 15;
            pthread_cond_timedwait(&w->cv, &w->mu, &ts);
        }
        if (cur < w->first) { buf_str(&out, "event: gap\ndata: {}\n\n"); cur = w->first; }
        while (cur < w->next && out.n < (1u << 20)) {
            const WSlot *sl = &w->slot[cur % WEB_RING];
            if (sl->s) buf_add(&out, sl->s, sl->n);
            cur++;
        }
        pthread_mutex_unlock(&w->mu);
        if (!out.n) buf_str(&out, ": ping\n\n");
        if (!sock_send(s, out.p, out.n)) break;
    }
    free(out.p);
}

/* ---- processos filhos: o agente e o terminal ---- */
typedef struct {
    pid_t pid; int to, out, err;
    int active, busy, gen;
    char model[512], cwd[4096];
} WProc;
static WProc g_wag, g_wtm;
static pthread_mutex_t g_wag_mu2 = PTHREAD_MUTEX_INITIALIZER;
static int g_wgen;

static const char *web_home(void) { const char *h = getenv("HOME"); return h && *h ? h : "/tmp"; }
static void web_desktop_home(char *o, size_t cap) {
    const char *e = getenv("KATANA_DESKTOP_HOME");
    if (e && *e) snprintf(o, cap, "%s", e); else snprintf(o, cap, "%s/.katana/desktop", web_home());
}
static void web_mkdirs(const char *p) {
    char t[4200]; snprintf(t, sizeof t, "%s", p);
    for (char *q = t + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(t, 0700); *q = '/'; }
    mkdir(t, 0700);
}

static void web_kill_proc(WProc *p) {            /* pede /exit (agente) e depois mata o grupo, sem bloquear: uma thread espera */
    pid_t pid = p->pid;
    if (pid > 0) { if (p->to >= 0) { ssize_t r = write(p->to, "/exit\n", 6); (void)r; } }
    if (p->to >= 0) { close(p->to); p->to = -1; }
    if (pid > 0) {
        if (fork() == 0) {                       /* guardiao: espera 3 s e mata o grupo se ainda existir */
            setsid();
            for (int i = 0; i < 30; i++) { usleep(100000); if (kill(pid, 0) != 0) _exit(0); }
            kill(-pid, SIGKILL); kill(pid, SIGKILL); _exit(0);
        }
    }
}

typedef struct { int fd_out, fd_err, gen; pid_t pid; } WRead;
static void *web_agent_reader(void *vp) {
    WRead r = *(WRead *)vp; free(vp);
    Buf in = {0}, tx = {0}, el = {0};
    char rb[8192];
    int at_line = 1;
    struct pollfd pf[2] = { { r.fd_out, POLLIN, 0 }, { r.fd_err, POLLIN, 0 } };
    int open_out = 1, open_err = 1;
    while (open_out || open_err) {
        pf[0].fd = open_out ? r.fd_out : -1; pf[1].fd = open_err ? r.fd_err : -1;
        const int pr = poll(pf, 2, 1000);
        if (pr < 0 && errno != EINTR) break;
        if (pr <= 0) continue;
        if (pf[1].revents & (POLLIN | POLLHUP)) {
            const ssize_t k = read(r.fd_err, rb, sizeof rb);
            if (k <= 0) open_err = 0;
            else {
                buf_add(&el, rb, (size_t)k);
                char *nl;
                while (el.n && (nl = memchr(el.p, '\n', el.n))) {
                    size_t ll = (size_t)(nl - el.p);
                    while (ll && el.p[ll - 1] == '\r') ll--;
                    if (ll) ws_push_str(&g_wev, "e", "l", el.p, ll);
                    const size_t used = (size_t)(nl - el.p) + 1;
                    memmove(el.p, el.p + used, el.n - used); el.n -= used;
                }
                if (el.n > (1u << 20)) el.n = 0;
            }
        }
        if (pf[0].revents & (POLLIN | POLLHUP)) {
            const ssize_t k = read(r.fd_out, rb, sizeof rb);
            if (k <= 0) { open_out = 0; continue; }
            buf_add(&in, rb, (size_t)k);
            buf_clear(&tx);
            for (;;) {
                if (!in.n) break;
                if (at_line && in.p[0] == 1) {                                   /* marcador: a linha inteira */
                    char *nl = memchr(in.p, '\n', in.n);
                    if (!nl) break;
                    if (tx.n) { ws_push_str(&g_wev, "t", "t", tx.p, tx.n); buf_clear(&tx); }
                    size_t ll = (size_t)(nl - in.p) - 1;
                    while (ll && in.p[ll] == '\r') ll--;
                    ws_push_str(&g_wev, "m", "l", in.p + 1, ll);
                    if (ll >= 5 && !strncmp(in.p + 1, "READY", 5)) { pthread_mutex_lock(&g_wag_mu2); if (g_wag.gen == r.gen) g_wag.busy = 0; pthread_mutex_unlock(&g_wag_mu2); }
                    const size_t used = (size_t)(nl - in.p) + 1;
                    memmove(in.p, in.p + used, in.n - used); in.n -= used;
                    at_line = 1;
                    continue;
                }
                char *nl = memchr(in.p, '\n', in.n);
                size_t take = nl ? (size_t)(nl - in.p) + 1 : in.n;
                if (!nl) take = utf8_complete(in.p, take);
                if (!nl) for (size_t i = take > 8 ? take - 8 : 0; i < take; i++) if (in.p[i] == 27) { take = i; break; }   /* sequencia de cor cortada */
                if (!take) break;
                for (size_t i = 0; i < take; i++) {
                    if (in.p[i] == 27 && i + 1 < take && in.p[i + 1] == '[') {      /* cores do terminal saem */
                        i += 2;
                        while (i < take && !((in.p[i] >= 'A' && in.p[i] <= 'Z') || (in.p[i] >= 'a' && in.p[i] <= 'z'))) i++;
                        continue;
                    }
                    if (in.p[i] == '\r') continue;
                    buf_add(&tx, in.p + i, 1);
                }
                at_line = in.p[take - 1] == '\n';
                memmove(in.p, in.p + take, in.n - take); in.n -= take;
            }
            if (tx.n) { ws_push_str(&g_wev, "t", "t", tx.p, tx.n); buf_clear(&tx); }
        }
    }
    close(r.fd_out); close(r.fd_err);
    int st = 0;
    waitpid(r.pid, &st, 0);
    pthread_mutex_lock(&g_wag_mu2);
    const int mine = g_wag.gen == r.gen;
    if (mine) { g_wag.active = 0; g_wag.busy = 0; g_wag.pid = 0; if (g_wag.to >= 0) { close(g_wag.to); g_wag.to = -1; } }
    pthread_mutex_unlock(&g_wag_mu2);
    if (mine) {
        char e[160]; const int k = snprintf(e, sizeof e, "{\"code\":%d,\"signal\":%d}", WIFEXITED(st) ? WEXITSTATUS(st) : -1, WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        ws_push(&g_wev, "x", e, (size_t)k);
    }
    free(in.p); free(tx.p); free(el.p);
    return NULL;
}

/* ambiente do agente como o do desktop: variaveis KATANA_* de desempenho saem, so as de comportamento ficam */
static char **web_agent_env(void) {
    static const char *keep[] = { "KATANA_UI", "KATANA_THINK", "KATANA_CONFIRM", "KATANA_WEB", "KATANA_SEARX", "KATANA_DL_MAX_MB", "KATANA_GIT_UNSAFE",
        "KATANA_SYSTEM", "KATANA_DATE", "KATANA_CACHE_DIR", "KATANA_LOG", "KATANA_TEMP", "KATANA_MAXTOK", "KATANA_COMPACT", "KATANA_SESS_DIAS",
        "KATANA_SESS_MB", "KATANA_SESS_RESUMO", "KATANA_RETOMAR_TOK", "KATANA_DESKTOP_HOME", "KATANA_DESKTOP_ENV", "KATANA_TRUST", "KATANA_SPEC",
        "KATANA_TOOL_DECL", "KATANA_TEACHER", "KATANA_TEACHER_MODEL", "KATANA_TEACHER_URL", "KATANA_TEACHER_EFFORT", "KATANA_TEACHER_FALLBACK",
        "KATANA_TEACHER_CMD", "KATANA_TEACHER_KEY", "KATANA_TEACHER_VIA", "KATANA_TEACHER_CLI", "KATANA_TEACHER_MAX_TOKENS", NULL };
    const char *de = getenv("KATANA_DESKTOP_ENV");
    const int all = de && !strcmp(de, "1");
    int ne = 0; while (environ[ne]) ne++;
    char **env = xalloc((size_t)ne + 8, sizeof(char *)); int k = 0;
    int has_think = 0, has_spec = 0;
    for (int i = 0; i < ne; i++) {
        const char *e = environ[i];
        if (!strncmp(e, "KATANA_API_KEY=", 15) || !strncmp(e, "KATANA_HOST=", 12) || !strncmp(e, "KATANA_AGENT_EXE=", 17) ||
            !strncmp(e, "KATANA_UI=", 10) || !strncmp(e, "NO_COLOR=", 9)) continue;
        if (!strncmp(e, "KATANA_", 7) && strncmp(e, "KATANA_MCP_", 11) && !all) {
            int ok = 0;
            for (int j = 0; keep[j]; j++) { const size_t l = strlen(keep[j]); if (!strncmp(e, keep[j], l) && e[l] == '=') { ok = 1; break; } }
            if (!ok) continue;
        }
        if (!strncmp(e, "KATANA_THINK=", 13)) has_think = 1;
        if (!strncmp(e, "KATANA_SPEC=", 12)) has_spec = 1;
        env[k++] = environ[i];
    }
    env[k++] = "KATANA_UI=1"; env[k++] = "NO_COLOR=1";
    if (!has_think) env[k++] = "KATANA_THINK=0";
    if (!has_spec) {                                  /* especializacoes: os .kspec de ~/.katana/specs entram na carga */
        static char sp[4400];
        snprintf(sp, sizeof sp, "%s/.katana/specs", web_home());
        DIR *d = opendir(sp);
        int any = 0;
        if (d) { struct dirent *de2; while ((de2 = readdir(d))) { const size_t l = strlen(de2->d_name); if (l > 6 && !strcmp(de2->d_name + l - 6, ".kspec")) { any = 1; break; } } closedir(d); }
        if (any) { static char kv[4500]; snprintf(kv, sizeof kv, "KATANA_SPEC=%s", sp); env[k++] = kv; }
    }
    env[k] = NULL;
    return env;
}

static const char *web_exe(char *buf, size_t cap) {
    const char *e = getenv("KATANA_AGENT_EXE");        /* so para teste: outro executavel que fale o protocolo (desktop-qt/tools/standin-engine.sh) */
    if (e && *e) { snprintf(buf, cap, "%s", e); return buf; }
    const ssize_t el = readlink("/proc/self/exe", buf, cap - 1);
    if (el <= 0) return NULL;
    buf[el] = 0;
    return buf;
}

static int web_start_agent(const char *model, const char *cwd, char *err, size_t ecap) {
    char exe[4096], path[4700];
    if (!web_exe(exe, sizeof exe)) { snprintf(err, ecap, "nao achei o executavel do katana"); return 0; }
    const char *base = strrchr(model, '/'); base = base ? base + 1 : model;
    snprintf(path, sizeof path, "%s/%s", models_dir(), base);
    if (g_web_pre[0] && !strcmp(base, g_web_pre) && g_srv_model_path[0]) snprintf(path, sizeof path, "%s", g_srv_model_path);   /* o modelo da linha de comando */
    struct stat sb;
    if (!base[0] || stat(path, &sb) != 0) { snprintf(err, ecap, "modelo nao encontrado: %s", base); return 0; }
    char rc[4096];
    if (!cwd || !*cwd || !realpath(cwd, rc) || stat(rc, &sb) != 0 || !S_ISDIR(sb.st_mode)) { if (!getcwd(rc, sizeof rc)) snprintf(rc, sizeof rc, "/"); }
    int in[2], out[2], er[2];
    if (pipe(in) != 0) { snprintf(err, ecap, "pipe falhou"); return 0; }
    if (pipe(out) != 0) { close(in[0]); close(in[1]); snprintf(err, ecap, "pipe falhou"); return 0; }
    if (pipe(er) != 0) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); snprintf(err, ecap, "pipe falhou"); return 0; }
    char **env = web_agent_env();
    char *av[] = { exe, "agent", path, NULL };
    pthread_mutex_lock(&g_wag_mu2);
    if (g_wag.active) { web_kill_proc(&g_wag); g_wag.active = 0; }   /* o antigo sai em silencio (a thread de leitura confere o gen) */
    const int gen = ++g_wgen;
    ws_clear(&g_wev);
    const pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        dup2(in[0], 0); dup2(out[1], 1); dup2(er[1], 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        signal(SIGPIPE, SIG_DFL);
        if (chdir(rc) != 0) _exit(126);
        execve(exe, av, env);
        _exit(127);
    }
    free(env);
    close(in[0]); close(out[1]); close(er[1]);
    if (pid < 0) { close(in[1]); close(out[0]); close(er[0]); pthread_mutex_unlock(&g_wag_mu2); snprintf(err, ecap, "fork falhou"); return 0; }
    memset(&g_wag, 0, sizeof g_wag);
    g_wag.pid = pid; g_wag.to = in[1]; g_wag.out = out[0]; g_wag.err = er[0]; g_wag.active = 1; g_wag.busy = 1; g_wag.gen = gen;
    snprintf(g_wag.model, sizeof g_wag.model, "%s", base);
    snprintf(g_wag.cwd, sizeof g_wag.cwd, "%s", rc);
    pthread_mutex_unlock(&g_wag_mu2);
    char sj[5200]; Buf b = {0};
    buf_str(&b, "{\"model\":\""); js_escape(&b, base, strlen(base)); buf_str(&b, "\",\"cwd\":\""); js_escape(&b, rc, strlen(rc));
    snprintf(sj, sizeof sj, "\",\"pid\":%d}", (int)pid); buf_str(&b, sj);
    ws_push(&g_wev, "s", b.p, b.n); free(b.p);
    WRead *wr = xalloc(1, sizeof *wr);
    wr->fd_out = out[0]; wr->fd_err = er[0]; wr->gen = gen; wr->pid = pid;
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, web_agent_reader, wr) != 0) { free(wr); kill(-pid, SIGKILL); snprintf(err, ecap, "thread falhou"); pthread_attr_destroy(&at); return 0; }
    pthread_attr_destroy(&at);
    return 1;
}

/* uma linha para o agente: sem quebras nem \x01 (o agente le uma linha por mensagem, como o desktop manda) */
static int web_write_line(const char *line, int raw_marker, const char *marker) {
    pthread_mutex_lock(&g_wag_mu2);
    if (!g_wag.active || g_wag.to < 0) { pthread_mutex_unlock(&g_wag_mu2); return 0; }
    Buf b = {0};
    if (raw_marker) { buf_str(&b, "\x01"); buf_str(&b, marker); }
    for (const char *p = line; *p; p++) {
        const unsigned char ch = (unsigned char)*p;
        buf_add(&b, (ch == '\n' || ch == '\r' || ch == 1) ? " " : p, 1);
    }
    buf_str(&b, "\n");
    if (!raw_marker && strncmp(line, "/session", 8) != 0) g_wag.busy = 1;
    const int to = g_wag.to;
    pthread_mutex_unlock(&g_wag_mu2);
    size_t off = 0;
    while (off < b.n) { const ssize_t k = write(to, b.p + off, b.n - off); if (k <= 0) break; off += (size_t)k; }
    free(b.p);
    return 1;
}

/* ---- terminal embutido ---- */
static void *web_term_reader(void *vp) {
    WRead r = *(WRead *)vp; free(vp);
    Buf in = {0};
    char rb[8192];
    for (;;) {
        const ssize_t k = read(r.fd_out, rb, sizeof rb);
        if (k <= 0) break;
        buf_add(&in, rb, (size_t)k);
        const size_t take = utf8_complete(in.p, in.n);
        if (take) { ws_push_str(&g_wterm, "d", "d", in.p, take); memmove(in.p, in.p + take, in.n - take); in.n -= take; }
    }
    close(r.fd_out);
    int st = 0; waitpid(r.pid, &st, 0);
    pthread_mutex_lock(&g_wag_mu2);
    const int mine = g_wtm.gen == r.gen;
    if (mine) { g_wtm.active = 0; g_wtm.pid = 0; if (g_wtm.to >= 0) { close(g_wtm.to); g_wtm.to = -1; } }
    pthread_mutex_unlock(&g_wag_mu2);
    if (mine) ws_push(&g_wterm, "x", "{}", 2);
    free(in.p);
    return NULL;
}

static int web_start_term(int cols, int rows) {
    if (cols < 20 || cols > 400) cols = 100;
    if (rows < 5 || rows > 200) rows = 24;
    char dir[4200], rc[4400], cmd[200];
    web_desktop_home(dir, sizeof dir);
    web_mkdirs(dir);
    snprintf(rc, sizeof rc, "%s/inputrc", dir);
    FILE *f = fopen(rc, "w");
    if (f) { fputs("$include /etc/inputrc\nset enable-bracketed-paste off\n", f); fclose(f); }
    snprintf(cmd, sizeof cmd, "stty cols %d rows %d; exec bash -i", cols, rows);
    int in[2], out[2];
    if (pipe(in) != 0) return 0;
    if (pipe(out) != 0) { close(in[0]); close(in[1]); return 0; }
    int ne = 0; while (environ[ne]) ne++;
    char **env = xalloc((size_t)ne + 6, sizeof(char *)); int k = 0;
    for (int i = 0; i < ne; i++) {
        const char *e = environ[i];
        if (!strncmp(e, "KATANA_API_KEY=", 15) || !strncmp(e, "KATANA_HOST=", 12) || !strncmp(e, "TERM=", 5) || !strncmp(e, "INPUTRC=", 8) || !strncmp(e, "COLORTERM=", 10)) continue;
        env[k++] = environ[i];
    }
    char ir[4500]; snprintf(ir, sizeof ir, "INPUTRC=%s", rc);
    env[k++] = "TERM=xterm-256color"; env[k++] = ir; env[k++] = "COLORTERM=truecolor"; env[k] = NULL;
    char *av[] = { "script", "-qfec", cmd, "/dev/null", NULL };
    pthread_mutex_lock(&g_wag_mu2);
    if (g_wtm.active) { if (g_wtm.pid > 0) kill(-g_wtm.pid, SIGHUP); if (g_wtm.to >= 0) { close(g_wtm.to); g_wtm.to = -1; } g_wtm.active = 0; }
    const int gen = ++g_wgen;
    ws_clear(&g_wterm);
    const pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        dup2(in[0], 0); dup2(out[1], 1); dup2(out[1], 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        signal(SIGPIPE, SIG_DFL);
        if (chdir(web_home()) != 0) _exit(126);
        environ = env;
        execvp("script", av);
        _exit(127);
    }
    free(env);
    close(in[0]); close(out[1]);
    if (pid < 0) { close(in[1]); close(out[0]); pthread_mutex_unlock(&g_wag_mu2); return 0; }
    memset(&g_wtm, 0, sizeof g_wtm);
    g_wtm.pid = pid; g_wtm.to = in[1]; g_wtm.out = out[0]; g_wtm.active = 1; g_wtm.gen = gen;
    pthread_mutex_unlock(&g_wag_mu2);
    WRead *wr = xalloc(1, sizeof *wr);
    wr->fd_out = out[0]; wr->gen = gen; wr->pid = pid;
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, web_term_reader, wr) != 0) { free(wr); kill(-pid, SIGKILL); pthread_attr_destroy(&at); return 0; }
    pthread_attr_destroy(&at);
    return 1;
}

/* ---- preferencias (ui.ini do desktop, o mesmo arquivo: tema, fonte, pasta de trabalho, ultimo modelo) ---- */
static int web_ui_key_ok(const char *k) {
    static const char *ok[] = { "tema", "fonte", "workspace", "lateralRecolhida", "ultimoModelo", "professor", "professorModelo", "manterAcordado", NULL };
    for (int i = 0; ok[i]; i++) if (!strcmp(k, ok[i])) return 1;
    return 0;
}
static void web_ui_path(char *o, size_t cap) { char d[4200]; web_desktop_home(d, sizeof d); snprintf(o, cap, "%s/ui.ini", d); }
static char *web_slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); const long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (64l << 20)) { fclose(f); return NULL; }
    char *b = xalloc((size_t)sz + 1, 1);
    const size_t r = fread(b, 1, (size_t)sz, f); fclose(f);
    b[r] = 0; if (n) *n = r;
    return b;
}
static void web_ini_unq(const char *v, size_t n, Buf *o) {
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') { v++; n -= 2; }
    for (size_t i = 0; i < n; i++) { if (v[i] == '\\' && i + 1 < n) i++; buf_add(o, v + i, 1); }
}
static int web_ui_get(const char *key, char *out, size_t cap) {
    char p[4400]; web_ui_path(p, sizeof p);
    char *t = web_slurp(p, NULL);
    out[0] = 0;
    if (!t) return 0;
    const size_t kl = strlen(key);
    int found = 0;
    for (char *l = t; l && *l; ) {
        char *e = strchr(l, '\n'); const size_t ll = e ? (size_t)(e - l) : strlen(l);
        if (ll > kl && !strncmp(l, key, kl) && l[kl] == '=') {
            Buf b = {0}; web_ini_unq(l + kl + 1, ll - kl - 1, &b);
            while (b.n && (b.p[b.n - 1] == '\r' || b.p[b.n - 1] == ' ')) b.n--;
            snprintf(out, cap, "%.*s", (int)b.n, b.p ? b.p : ""); free(b.p); found = 1; break;
        }
        l = e ? e + 1 : NULL;
    }
    free(t);
    return found;
}
static void web_ui_set(const char *key, const char *val) {
    char p[4400], tp[4500]; web_ui_path(p, sizeof p);
    char d[4200]; web_desktop_home(d, sizeof d); web_mkdirs(d);
    char *t = web_slurp(p, NULL);
    Buf o = {0}, q = {0};
    const int plain = !strpbrk(val, ",;\"\\=\n\r") && val[0] != ' ';
    if (!plain) { buf_str(&q, "\""); for (const char *c = val; *c; c++) { if (*c == '"' || *c == '\\') buf_str(&q, "\\"); if (*c == '\n' || *c == '\r') continue; buf_add(&q, c, 1); } buf_str(&q, "\""); }
    const size_t kl = strlen(key);
    int done = 0, header = 0;
    for (char *l = t; l && *l; ) {
        char *e = strchr(l, '\n'); const size_t ll = e ? (size_t)(e - l) : strlen(l);
        if (ll >= 9 && !strncmp(l, "[General]", 9)) header = 1;
        if (ll > kl && !strncmp(l, key, kl) && l[kl] == '=' && !done) {
            buf_add(&o, key, kl); buf_str(&o, "="); if (plain) buf_str(&o, val); else buf_add(&o, q.p, q.n); buf_str(&o, "\n"); done = 1;
        } else { buf_add(&o, l, ll); buf_str(&o, "\n"); }
        l = e ? e + 1 : NULL;
    }
    if (!header && !o.n) buf_str(&o, "[General]\n");
    if (!done) { buf_add(&o, key, kl); buf_str(&o, "="); if (plain) buf_str(&o, val); else buf_add(&o, q.p, q.n); buf_str(&o, "\n"); }
    snprintf(tp, sizeof tp, "%s.web%d", p, (int)getpid());
    FILE *f = fopen(tp, "wb");
    if (f) { fwrite(o.p, 1, o.n, f); fclose(f); rename(tp, p); }
    free(o.p); free(q.p); free(t);
}

/* ---- modelos instalados (mesma regra do desktop) ---- */
typedef struct { char file[300]; double gb; int usable, def; } WModel;
static int web_model_usable(const char *name) {
    char l[300]; size_t i = 0;
    for (; name[i] && i < sizeof l - 1; i++) l[i] = (char)tolower((unsigned char)name[i]);
    l[i] = 0;
    return strstr(l, "qwen3") || strstr(l, "gemma-4") || strstr(l, "gemma4") || strstr(l, "gpt-oss") || strstr(l, "ravenx") || strstr(l, "qwen-agentworld");
}
static int web_model_cmp(const void *a, const void *b) {
    const WModel *x = a, *y = b;
    if (x->usable != y->usable) return y->usable - x->usable;
    if (x->def != y->def) return y->def - x->def;
    return strcmp(x->file, y->file);
}
static void web_models_json(Buf *b) {
    WModel *v = xalloc(512, sizeof *v); int n = 0;
    DIR *d = opendir(models_dir());
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < 512) {
            const size_t l = strlen(e->d_name);
            if (l < 6 || strcmp(e->d_name + l - 5, ".gguf") != 0 || l >= sizeof v[0].file) continue;
            char p[4700]; struct stat sb;
            snprintf(p, sizeof p, "%s/%s", models_dir(), e->d_name);
            if (stat(p, &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
            snprintf(v[n].file, sizeof v[n].file, "%s", e->d_name);
            v[n].gb = sb.st_size / 1073741824.0;
            v[n].usable = web_model_usable(e->d_name);
            char lo[300]; size_t i = 0; for (; e->d_name[i]; i++) lo[i] = (char)tolower((unsigned char)e->d_name[i]); lo[i] = 0;
            v[n].def = strstr(lo, "qwen3.6") != NULL;
            n++;
        }
        closedir(d);
    }
    qsort(v, (size_t)n, sizeof *v, web_model_cmp);
    buf_str(b, "[");
    for (int i = 0; i < n; i++) {
        char t[160]; const size_t fl = strlen(v[i].file);
        if (i) buf_str(b, ",");
        buf_str(b, "{\"file\":\""); js_escape(b, v[i].file, fl);
        buf_str(b, "\",\"name\":\""); js_escape(b, v[i].file, fl - 5);
        snprintf(t, sizeof t, "\",\"sizeGb\":%.3f,\"sizeText\":\"%.1f GB\",\"usable\":%s,\"isDefault\":%s,\"note\":\"%s\"}", v[i].gb, v[i].gb,
                 v[i].usable ? "true" : "false", v[i].def ? "true" : "false", v[i].usable ? "" : "ainda nao esta utilizavel no agente");
        buf_str(b, t);
    }
    buf_str(b, "]");
    free(v);
}

/* ---- conversas salvas (o mesmo formato e a mesma pasta do desktop: <home do desktop>/sessoes/<id>.json) ---- */
static int web_id_ok(const char *s) {
    const size_t n = strlen(s);
    if (!n || n > 64) return 0;
    for (size_t i = 0; i < n; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '-')) return 0;
    return 1;
}
static void web_sess_dir(char *o, size_t cap) { char d[4200]; web_desktop_home(d, sizeof d); snprintf(o, cap, "%s/sessoes", d); }
typedef struct { char id[72], titulo[400], modelo[300], criada[48], atualizada[48]; int ksess; } WSess;
static int web_sess_cmp(const void *a, const void *b) {
    const WSess *x = a, *y = b;
    const int c = strcmp(y->atualizada, x->atualizada);
    return c ? c : strcmp(y->id, x->id);
}
static void web_sessions_json(Buf *b) {
    char dir[4300]; web_sess_dir(dir, sizeof dir);
    DIR *d = opendir(dir);
    WSess *v = xalloc(2048, sizeof *v); int n = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < 2048) {
            const size_t l = strlen(e->d_name);
            if (l < 6 || strcmp(e->d_name + l - 5, ".json") != 0 || l - 5 > 64) continue;
            char id[72]; snprintf(id, sizeof id, "%.*s", (int)(l - 5), e->d_name);
            if (!web_id_ok(id)) continue;
            char p[4700]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            char *t = web_slurp(p, NULL);
            if (!t) continue;
            if (*js_ws(t) == '{') {
                WSess *s = &v[n++];
                memset(s, 0, sizeof *s);
                snprintf(s->id, sizeof s->id, "%s", id);
                oa_str(g4_js_get(t, "titulo"), s->titulo, sizeof s->titulo);
                oa_str(g4_js_get(t, "modelo"), s->modelo, sizeof s->modelo);
                oa_str(g4_js_get(t, "criada"), s->criada, sizeof s->criada);
                oa_str(g4_js_get(t, "atualizada"), s->atualizada, sizeof s->atualizada);
                snprintf(p, sizeof p, "%s/%s.ksess", dir, id);
                s->ksess = access(p, F_OK) == 0;
            }
            free(t);
        }
        closedir(d);
    }
    qsort(v, (size_t)n, sizeof *v, web_sess_cmp);
    buf_str(b, "[");
    for (int i = 0; i < n; i++) {
        if (i) buf_str(b, ",");
        buf_str(b, "{\"id\":\""); js_escape(b, v[i].id, strlen(v[i].id));
        buf_str(b, "\",\"titulo\":\""); js_escape(b, v[i].titulo, strlen(v[i].titulo));
        buf_str(b, "\",\"modelo\":\""); js_escape(b, v[i].modelo, strlen(v[i].modelo));
        buf_str(b, "\",\"criada\":\""); js_escape(b, v[i].criada, strlen(v[i].criada));
        buf_str(b, "\",\"atualizada\":\""); js_escape(b, v[i].atualizada, strlen(v[i].atualizada));
        buf_str(b, v[i].ksess ? "\",\"ksess\":true}" : "\",\"ksess\":false}");
    }
    buf_str(b, "]");
    free(v);
}

/* ---- estado inicial ---- */
static void web_state_json(Buf *b) {
    char t[4600], v[4096], dir[4300], hd[4200];
    web_desktop_home(hd, sizeof hd);
    web_sess_dir(dir, sizeof dir);
    char exe[4096]; const char *ex = web_exe(exe, sizeof exe);
    buf_str(b, "{\"engine\":\"katana "); js_escape(b, KATANA_BUILD, strlen(KATANA_BUILD));
    buf_str(b, " \\u00b7 "); js_escape(b, ex ? ex : "?", ex ? strlen(ex) : 1);
    buf_str(b, "\",\"changes\":\""); js_escape(b, KATANA_CHANGES, strlen(KATANA_CHANGES));
    buf_str(b, "\",\"models\":"); web_models_json(b);
    buf_str(b, ",\"modelsDir\":\""); js_escape(b, models_dir(), strlen(models_dir()));
    buf_str(b, "\",\"home\":\""); js_escape(b, web_home(), strlen(web_home()));
    buf_str(b, "\",\"desktopHome\":\""); js_escape(b, hd, strlen(hd));
    buf_str(b, "\",\"sessDir\":\""); js_escape(b, dir, strlen(dir));
    snprintf(t, sizeof t, "%s/.katana/specs", web_home());
    buf_str(b, "\",\"specsDir\":\""); js_escape(b, t, strlen(t));
    if (!getcwd(v, sizeof v)) snprintf(v, sizeof v, "/");
    buf_str(b, "\",\"cwd\":\""); js_escape(b, v, strlen(v));
    buf_str(b, web_teacher_has_key() ? "\",\"hasTeacherKey\":true,\"preselect\":\"" : "\",\"hasTeacherKey\":false,\"preselect\":\""); js_escape(b, g_web_pre, strlen(g_web_pre));
    buf_str(b, "\",\"ui\":{");
    static const char *keys[] = { "tema", "fonte", "workspace", "lateralRecolhida", "ultimoModelo", "professor", "professorModelo", "manterAcordado", NULL };
    int first = 1;
    for (int i = 0; keys[i]; i++) {
        if (!web_ui_get(keys[i], v, sizeof v)) continue;
        if (!first) buf_str(b, ",");
        first = 0;
        buf_str(b, "\""); buf_str(b, keys[i]); buf_str(b, "\":\""); js_escape(b, v, strlen(v)); buf_str(b, "\"");
    }
    pthread_mutex_lock(&g_wag_mu2);
    snprintf(t, sizeof t, "},\"agent\":{\"running\":%s,\"busy\":%s,\"model\":\"", g_wag.active ? "true" : "false", g_wag.busy ? "true" : "false");
    buf_str(b, t); js_escape(b, g_wag.model, strlen(g_wag.model));
    buf_str(b, "\",\"cwd\":\""); js_escape(b, g_wag.cwd, strlen(g_wag.cwd));
    pthread_mutex_unlock(&g_wag_mu2);
    pthread_mutex_lock(&g_wev.mu);
    snprintf(t, sizeof t, "\"},\"next\":%lu,\"first\":%lu,\"pid\":%d}", g_wev.next, g_wev.first, (int)getpid());
    pthread_mutex_unlock(&g_wev.mu);
    buf_str(b, t);
}

/* ---- navegador de pastas (o navegador nao tem seletor de pasta da maquina) ---- */
static int web_str_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void web_dirs(sock_t s, const char *pathq) {
    char want[4096], rp[4096];
    snprintf(want, sizeof want, "%s", pathq && *pathq ? pathq : web_home());
    struct stat sb;
    if (!realpath(want, rp) || stat(rp, &sb) != 0 || !S_ISDIR(sb.st_mode)) { web_err(s, 404, "pasta nao encontrada"); return; }
    DIR *d = opendir(rp);
    if (!d) { web_err(s, 403, "sem permissao para listar a pasta"); return; }
    char **names = xalloc(4096, sizeof(char *)); int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 4096) {
        if (e->d_name[0] == '.') continue;
        char p[8300]; snprintf(p, sizeof p, "%s/%s", strcmp(rp, "/") ? rp : "", e->d_name);
        if (stat(p, &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(char *), web_str_cmp);
    Buf b = {0};
    buf_str(&b, "{\"path\":\""); js_escape(&b, rp, strlen(rp));
    char par[4096]; snprintf(par, sizeof par, "%s", rp);
    char *sl = strrchr(par, '/'); if (sl && sl != par) *sl = 0; else if (sl) par[1] = 0;
    buf_str(&b, "\",\"parent\":\""); js_escape(&b, par, strlen(par));
    buf_str(&b, "\",\"dirs\":[");
    for (int i = 0; i < n; i++) { if (i) buf_str(&b, ","); buf_str(&b, "\""); js_escape(&b, names[i], strlen(names[i])); buf_str(&b, "\""); free(names[i]); }
    buf_str(&b, "]}");
    free(names);
    web_json(s, 200, &b); free(b.p);
}

static void web_open_path(const char *path, int folder) {
    char rp[4096];
    char full[8300];
    if (path[0] == '/') snprintf(full, sizeof full, "%s", path);
    else { pthread_mutex_lock(&g_wag_mu2); snprintf(full, sizeof full, "%s/%s", g_wag.cwd[0] ? g_wag.cwd : web_home(), path); pthread_mutex_unlock(&g_wag_mu2); }
    if (!realpath(full, rp)) return;
    if (folder) { char *sl = strrchr(rp, '/'); if (sl && sl != rp) *sl = 0; }
    if (fork() == 0) {                           /* neto: sem zumbi */
        if (fork() == 0) {
            setsid();
            const int nul = open("/dev/null", O_RDWR);
            if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
            for (int fd = 3; fd < 1024; fd++) close(fd);
            execlp("xdg-open", "xdg-open", rp, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    int st; wait(&st);
}

/* ---- uso da maquina e atividade do agente (a barra de baixo; mesma conta do desktop) ---- */
static long long web_ticks(long pid) {                   /* utime + stime do processo (campos 14 e 15 de /proc/<pid>/stat) */
    char p[64], b[1024];
    snprintf(p, sizeof p, "/proc/%ld/stat", pid);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    const size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    const char *c = strrchr(b, ')');
    if (!c) return -1;
    long long ut = 0, st = 0;
    if (sscanf(c + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lld %lld", &ut, &st) != 2) return -1;
    return ut + st;
}
static void web_sys_json(Buf *b) {
    char t[512], l[256];
    long long tot = 0, idle = 0, memt = 0, mema = 0, ag = 0;
    int children = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        if (fgets(l, sizeof l, f)) {
            long long v[8] = {0};
            if (sscanf(l, "cpu %lld %lld %lld %lld %lld %lld %lld %lld", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
                for (int i = 0; i < 8; i++) tot += v[i];
                idle = v[3] + v[4];
            }
        }
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(l, sizeof l, f)) {
            if (!strncmp(l, "MemTotal:", 9)) memt = atoll(l + 9);
            else if (!strncmp(l, "MemAvailable:", 13)) mema = atoll(l + 13);
        }
        fclose(f);
    }
    pthread_mutex_lock(&g_wag_mu2);
    const long pid = g_wag.active ? (long)g_wag.pid : 0;
    pthread_mutex_unlock(&g_wag_mu2);
    if (pid > 0) {
        long long k = web_ticks(pid);
        if (k > 0) ag += k;
        snprintf(l, sizeof l, "/proc/%ld/task/%ld/children", pid, pid);
        f = fopen(l, "r");
        if (f) {
            char cb[1024]; const size_t n = fread(cb, 1, sizeof cb - 1, f); fclose(f); cb[n] = 0;
            for (char *tok = strtok(cb, " \n"); tok; tok = strtok(NULL, " \n")) { children = 1; const long long q = web_ticks(atol(tok)); if (q > 0) ag += q; }
        }
    }
    snprintf(t, sizeof t, "{\"cpuTot\":%lld,\"cpuIdle\":%lld,\"memTotal\":%lld,\"memAvail\":%lld,\"agentTicks\":%lld,\"children\":%s,\"gpu\":", tot, idle, memt, mema, ag, children ? "true" : "false");
    buf_str(b, t);
    static time_t gpu_t; static char gpu_s[160]; static int gpu_state;           /* nvidia-smi a cada 4 s; sem ele, nunca mais */
    pthread_mutex_lock(&g_wag_mu2);
    if (gpu_state >= 0 && time(NULL) - gpu_t >= 4) {
        gpu_t = time(NULL);
        FILE *p = popen("nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits 2>/dev/null", "r");
        gpu_s[0] = 0;
        if (p) { if (fgets(gpu_s, sizeof gpu_s, p)) { gpu_state = 1; gpu_s[strcspn(gpu_s, "\r\n")] = 0; } if (pclose(p) != 0 || !gpu_s[0]) { gpu_s[0] = 0; if (gpu_state == 0) gpu_state = -1; } }
        else gpu_state = -1;
    }
    char g[160]; snprintf(g, sizeof g, "%s", gpu_s);
    pthread_mutex_unlock(&g_wag_mu2);
    int a1 = 0, a2 = 0, a3 = 0;
    if (g[0] && sscanf(g, "%d , %d , %d", &a1, &a2, &a3) == 3) { snprintf(t, sizeof t, "[\"%d\",\"%d\",\"%d\"]", a1, a2, a3); buf_str(b, t); }
    else buf_str(b, "null");
    buf_str(b, "}");
}

/* `katana coder status <pasta>`: o estado do indice de codigo (a pagina le "arquivos vigentes", os niveis e o rastreador) */
static void web_index(sock_t s, const char *path) {
    char exe[4096], rp[4096];
    struct stat sb;
    if (!web_exe(exe, sizeof exe) || !path[0] || !realpath(path, rp) || stat(rp, &sb) != 0 || !S_ISDIR(sb.st_mode)) { web_err(s, 404, "pasta nao encontrada"); return; }
    Buf cmd = {0};
    buf_str(&cmd, "timeout 10 '");
    for (const char *c = exe; *c; c++) { if (*c == '\'') buf_str(&cmd, "'\\''"); else buf_add(&cmd, c, 1); }
    buf_str(&cmd, "' coder status '");
    for (const char *c = rp; *c; c++) { if (*c == '\'') buf_str(&cmd, "'\\''"); else buf_add(&cmd, c, 1); }
    buf_str(&cmd, "' 2>/dev/null");
    Buf out = {0};
    FILE *p = popen(cmd.p, "r");
    if (p) { char rb[4096]; size_t n; while ((n = fread(rb, 1, sizeof rb, p)) > 0 && out.n < 65536) buf_add(&out, rb, n); pclose(p); }
    Buf j = {0};
    buf_str(&j, "{\"out\":\""); js_escape(&j, out.p ? out.p : "", out.n); buf_str(&j, "\"}");
    web_json(s, 200, &j);
    free(j.p); free(out.p); free(cmd.p);
}

/* modo professor: a chave fica em ~/.katana/teacher.key (so do dono), nunca no ui.ini */
static void web_teacher_path(char *o, size_t cap) { snprintf(o, cap, "%s/.katana/teacher.key", web_home()); }
static int web_teacher_has_key(void) {
    const char *e = getenv("KATANA_TEACHER_KEY");
    if (e && *e) return 1;
    char p[4400]; web_teacher_path(p, sizeof p);
    char *t = web_slurp(p, NULL);
    int ok = 0;
    if (t) { for (const char *c = t; *c; c++) if (!isspace((unsigned char)*c)) { ok = 1; break; } free(t); }
    return ok;
}
static void web_teacher_set(const char *key, const char *model) {
    if (key[0]) {
        char d[4400], p[4400]; snprintf(d, sizeof d, "%s/.katana", web_home()); mkdir(d, 0700);
        web_teacher_path(p, sizeof p);
        remove(p);
        const int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) { ssize_t r = write(fd, key, strlen(key)); r = write(fd, "\n", 1); (void)r; close(fd); }
    }
    if (model[0]) web_ui_set("professorModelo", model);
}

/* ---- autenticacao e roteamento ---- */
static int web_ct_eq(const char *a, const char *b) {
    const size_t la = strlen(a), lb = strlen(b);
    unsigned d = la != lb;
    for (size_t i = 0; i < lb; i++) d |= (unsigned char)(i < la ? a[i] : 0) ^ (unsigned char)b[i];
    return d == 0;
}
static int web_cookie_ok(const char *head) {
    char c[1024];
    if (!hdr_get(head, "Cookie", c, sizeof c)) return 0;
    const char *p = strstr(c, "katana_web=");
    while (p && p != c && p[-1] != ' ' && p[-1] != ';') p = strstr(p + 1, "katana_web=");
    if (!p) return 0;
    p += 11;
    char t[80]; size_t i = 0;
    while (p[i] && p[i] != ';' && p[i] != ' ' && i < sizeof t - 1) { t[i] = p[i]; i++; }
    t[i] = 0;
    return web_ct_eq(t, g_web_token);
}
static int web_host_ok(const char *h) {          /* 127.0.0.1:PORTA ou localhost:PORTA, a porta deste servidor */
    char a[64], b[64];
    snprintf(a, sizeof a, "127.0.0.1:%d", g_web_port); snprintf(b, sizeof b, "localhost:%d", g_web_port);
    return !strcmp(h, a) || !strcmp(h, b);
}

static int web_qget(const char *q, const char *key, char *out, size_t cap) {
    out[0] = 0;
    const size_t kl = strlen(key);
    for (const char *p = q; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1; size_t i = 0;
            while (*v && *v != '&' && i < cap - 1) {
                if (*v == '%' && isxdigit((unsigned char)v[1]) && isxdigit((unsigned char)v[2])) { char h[3] = { v[1], v[2], 0 }; out[i++] = (char)strtol(h, NULL, 16); v += 3; }
                else if (*v == '+') { out[i++] = ' '; v++; }
                else out[i++] = *v++;
            }
            out[i] = 0;
            return 1;
        }
        p = strchr(p, '&'); if (p) p++;
    }
    return 0;
}

/* corpo JSON: chave de texto, ou "" */
static void web_jstr(const char *body, const char *key, char *out, size_t cap) { out[0] = 0; const char *p = js_ws(body ? body : ""); if (*p == '{') oa_str(g4_js_get(p, key), out, cap); }

/* 1 = tratou (resposta enviada); 0 = nao e da pagina */
static int web_handle(sock_t s, const char *method, const char *path, const char *query, const char *head, size_t hn, size_t hend, int peer_local) {
    if (!(path[0] == '/' && (path[1] == 0 || strncmp(path, "/web/", 5) == 0 || strncmp(path, "/css/", 5) == 0 || strncmp(path, "/js/", 4) == 0 ||
          strncmp(path, "/fonts/", 7) == 0 || strncmp(path, "/img/", 5) == 0 || !strcmp(path, "/index.html") || !strcmp(path, "/favicon.ico"))))
        return 0;
    char val[600];
    if (!peer_local) { web_err(s, 403, "a pagina so atende a propria maquina"); return 1; }
    if (!hdr_get(head, "Host", val, sizeof val) || !web_host_ok(val)) { web_err(s, 403, "host nao permitido"); return 1; }
    char host[600]; snprintf(host, sizeof host, "%s", val);
    if (hdr_get(head, "Origin", val, sizeof val)) {
        const char *o = strstr(val, "://");
        if (!o || strcmp(o + 3, host) != 0) { web_err(s, 403, "origem nao permitida"); return 1; }
    }
    const int get = !strcmp(method, "GET");
    if (!get && !hdr_get(head, "X-Katana", val, sizeof val)) { web_err(s, 403, "falta o cabecalho X-Katana"); return 1; }
    char tk[80];
    if (get && path[1] == 0 && web_qget(query, "token", tk, sizeof tk)) {      /* a chave da URL vira cookie e sai da barra */
        if (!web_ct_eq(tk, g_web_token)) { web_err(s, 403, "chave invalida"); return 1; }
        char ex[200]; snprintf(ex, sizeof ex, "Set-Cookie: katana_web=%s; Path=/; HttpOnly; SameSite=Strict\r\nLocation: /\r\n", g_web_token);
        web_reply(s, 302, "text/plain", ex, "", 0, 0);
        return 1;
    }
    if (!web_cookie_ok(head)) {
        const char *m = "<!doctype html><meta charset=utf-8><title>katana</title><body style=\"font-family:sans-serif;margin:3em\">"
                        "<h3>katana web</h3><p>Abra o endereco completo que o <code>katana serve --agent</code> imprimiu no terminal "
                        "(termina com <code>/?token=...</code>).</p>";
        web_reply(s, 401, "text/html; charset=utf-8", NULL, m, strlen(m), 1);
        return 1;
    }
    if (strncmp(path, "/web/", 5) != 0) {                                     /* arquivos da pagina */
        if (!get) { web_err(s, 405, "use GET"); return 1; }
        const char *want = path[1] == 0 ? "index.html" : path + 1;
        for (int i = 0; WEB_FILES[i].path; i++)
            if (!strcmp(WEB_FILES[i].path, want)) {
                web_reply(s, 200, WEB_FILES[i].mime, "Cache-Control: no-cache\r\n", web_file_data(i), WEB_FILES[i].len, !strncmp(WEB_FILES[i].mime, "text/html", 9));
                return 1;
            }
        if (path[1] == 0) {
            const char *m = "<!doctype html><meta charset=utf-8><title>katana</title><body style=\"font-family:sans-serif;margin:3em\">"
                            "<p>Esta compilacao nao tem a pagina embutida: rode <code>./build.sh</code> (ele gera src/app/web.inc a partir de web/).</p>";
            web_reply(s, 200, "text/html; charset=utf-8", NULL, m, strlen(m), 1);
        } else web_err(s, 404, "nao encontrado");
        return 1;
    }
    const char *r = path + 4;                                                  /* "/..." */
    char *body = NULL;
    if (!strcmp(method, "POST") || !strcmp(method, "PUT")) { body = srv_body(s, head, hn, hend); if (!body) return 1; }
    Buf b = {0};
    if (get && !strcmp(r, "/state")) { web_state_json(&b); web_json(s, 200, &b); }
    else if (get && !strcmp(r, "/sessions")) { web_sessions_json(&b); web_json(s, 200, &b); }
    else if (get && !strcmp(r, "/events")) {
        char f[40], le[40]; unsigned long from = 0;
        if (hdr_get(head, "Last-Event-ID", le, sizeof le)) from = strtoul(le, NULL, 10) + 1;
        else if (web_qget(query, "from", f, sizeof f)) from = strtoul(f, NULL, 10);
        web_sse(s, &g_wev, from);
    }
    else if (get && !strcmp(r, "/term/events")) {
        char f[40], le[40]; unsigned long from = 0;
        if (hdr_get(head, "Last-Event-ID", le, sizeof le)) from = strtoul(le, NULL, 10) + 1;
        else if (web_qget(query, "from", f, sizeof f)) from = strtoul(f, NULL, 10);
        web_sse(s, &g_wterm, from);
    }
    else if (get && !strcmp(r, "/dirs")) { char p[4096]; web_qget(query, "path", p, sizeof p); web_dirs(s, p); }
    else if (get && !strncmp(r, "/sessions/", 10)) {
        char dir[4300], p[4500]; web_sess_dir(dir, sizeof dir);
        if (!web_id_ok(r + 10)) web_err(s, 400, "id invalido");
        else {
            snprintf(p, sizeof p, "%s/%s.json", dir, r + 10);
            size_t n = 0; char *t = web_slurp(p, &n);
            if (!t) web_err(s, 404, "conversa nao encontrada"); else { web_reply(s, 200, "application/json; charset=utf-8", NULL, t, n, 0); free(t); }
        }
    }
    else if (!strcmp(method, "PUT") && !strncmp(r, "/sessions/", 10)) {
        char dir[4300], p[4600], tp[4700]; web_sess_dir(dir, sizeof dir);
        if (!web_id_ok(r + 10) || *js_ws(body) != '{') web_err(s, 400, "id ou corpo invalido");
        else {
            web_mkdirs(dir);
            snprintf(p, sizeof p, "%s/%s.json", dir, r + 10); snprintf(tp, sizeof tp, "%s.web%d", p, (int)getpid());
            FILE *f = fopen(tp, "wb");
            if (!f) web_err(s, 500, "nao consegui gravar");
            else { fwrite(body, 1, strlen(body), f); fclose(f); if (rename(tp, p) != 0) web_err(s, 500, "nao consegui gravar"); else web_ok(s); }
        }
    }
    else if (!strcmp(method, "DELETE") && !strncmp(r, "/sessions/", 10)) {
        char dir[4300], p[4600]; web_sess_dir(dir, sizeof dir);
        if (!web_id_ok(r + 10)) web_err(s, 400, "id invalido");
        else { snprintf(p, sizeof p, "%s/%s.json", dir, r + 10); remove(p); snprintf(p, sizeof p, "%s/%s.ksess", dir, r + 10); remove(p); web_ok(s); }
    }
    else if (!strcmp(method, "POST") && !strcmp(r, "/start")) {
        char model[600], cwd[4096], err[300];
        web_jstr(body, "model", model, sizeof model); web_jstr(body, "cwd", cwd, sizeof cwd);
        if (web_start_agent(model, cwd, err, sizeof err)) { web_ui_set("ultimoModelo", model); web_ok(s); } else web_err(s, 400, err);
    }
    else if (!strcmp(method, "POST") && !strcmp(r, "/line")) {
        char line[8200]; web_jstr(body, "line", line, sizeof line);
        if (!line[0]) web_err(s, 400, "line vazia");
        else if (!web_write_line(line, 0, NULL)) web_err(s, 409, "o agente nao esta rodando");
        else web_ok(s);
    }
    else if (!strcmp(method, "POST") && !strcmp(r, "/ans")) {          /* resposta a CHOICE (texto) ou ASK (yes: true/false) */
        char t[4200]; web_jstr(body, "text", t, sizeof t);
        const char *y = body ? g4_js_get(js_ws(body), "yes") : NULL;
        int ok;
        if (y) ok = web_write_line("", 1, !strncmp(y, "true", 4) ? "ANS 1" : "ANS 0");
        else ok = web_write_line(t, 1, "ANS ");
        if (ok) web_ok(s); else web_err(s, 409, "o agente nao esta rodando");
    }
    else if (!strcmp(method, "POST") && !strcmp(r, "/pass")) {         /* senha do sudo: direto ao agente, sem log */
        char t[600]; web_jstr(body, "password", t, sizeof t);
        if (web_write_line(t, 1, "PASS ")) web_ok(s); else web_err(s, 409, "o agente nao esta rodando");
    }
    else if (!strcmp(method, "POST") && !strcmp(r, "/stop")) {
        pthread_mutex_lock(&g_wag_mu2);
        const pid_t pid = g_wag.active ? g_wag.pid : 0;
        if (pid > 0) kill(-pid, SIGINT);                               /* como o Ctrl+C do desktop */
        pthread_mutex_unlock(&g_wag_mu2);
        if (pid > 0) web_ok(s); else web_err(s, 409, "o agente nao esta rodando");
    }
    else if (!strcmp(method, "POST") && !strcmp(r, "/ui")) {
        char k[64], v[4096]; web_jstr(body, "key", k, sizeof k); web_jstr(body, "value", v, sizeof v);
        if (!web_ui_key_ok(k)) web_err(s, 400, "chave desconhecida"); else { web_ui_set(k, v); web_ok(s); }
    }
    else if (!strcmp(method, "POST") && !strcmp(r, "/open")) {
        char p[4096]; web_jstr(body, "path", p, sizeof p);
        const char *fv = body ? g4_js_get(js_ws(body), "folder") : NULL, *mv = body ? g4_js_get(js_ws(body), "mkdir") : NULL;
        if (!p[0]) web_err(s, 400, "path vazio");
        else { if (mv && !strncmp(mv, "true", 4)) web_mkdirs(p); web_open_path(p, fv && !strncmp(fv, "true", 4)); web_ok(s); }
    }
    else if (get && !strcmp(r, "/sys")) { web_sys_json(&b); web_json(s, 200, &b); }
    else if (get && !strcmp(r, "/index")) { char p[4096]; web_qget(query, "path", p, sizeof p); web_index(s, p); }
    else if (!strcmp(method, "POST") && !strcmp(r, "/teacher")) {
        char k[600], m[300]; web_jstr(body, "key", k, sizeof k); web_jstr(body, "model", m, sizeof m);
        web_teacher_set(k, m); web_ok(s);
    }
    else if (!strcmp(method, "POST") && !strcmp(r, "/term/start")) {
        const char *c = body ? g4_js_get(js_ws(body), "cols") : NULL, *w = body ? g4_js_get(js_ws(body), "rows") : NULL;
        if (web_start_term(c ? atoi(c) : 100, w ? atoi(w) : 24)) web_ok(s); else web_err(s, 500, "nao consegui abrir o terminal (falta o comando script?)");
    }
    else if (!strcmp(method, "POST") && !strcmp(r, "/term/key")) {
        char t[8200]; web_jstr(body, "data", t, sizeof t);
        pthread_mutex_lock(&g_wag_mu2);
        if (g_wtm.active && g_wtm.to >= 0) { const size_t l = strlen(t); size_t off = 0; while (off < l) { const ssize_t k = write(g_wtm.to, t + off, l - off); if (k <= 0) break; off += (size_t)k; } }
        pthread_mutex_unlock(&g_wag_mu2);
        web_ok(s);
    }
    else if (!strcmp(method, "POST") && !strcmp(r, "/term/stop")) {
        pthread_mutex_lock(&g_wag_mu2);
        if (g_wtm.active) { if (g_wtm.pid > 0) kill(-g_wtm.pid, SIGHUP); if (g_wtm.to >= 0) { close(g_wtm.to); g_wtm.to = -1; } }
        pthread_mutex_unlock(&g_wag_mu2);
        web_ok(s);
    }
    else web_err(s, 404, "nao encontrado");
    free(b.p); free(body);
    return 1;
}

static void web_init(void) {
    unsigned char r[16] = {0};
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(r, 1, sizeof r, f) != sizeof r) die("katana web: sem /dev/urandom para gerar a chave da pagina");
    fclose(f);
    for (int i = 0; i < 16; i++) snprintf(g_web_token + i * 2, 3, "%02x", r[i]);
}
#else
static int web_handle(sock_t s, const char *method, const char *path, const char *query, const char *head, size_t hn, size_t hend, int peer_local) {
    (void)s; (void)method; (void)path; (void)query; (void)head; (void)hn; (void)hend; (void)peer_local;
    return 0;
}
static void web_init(void) {}
#endif
