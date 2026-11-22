/* ---------- canal MCP: mensagens que um servidor ligado empurra para o console (docs/channels.md) ----------
 * Um servidor que o dono autorizou ("channel": true no mcp.json dele, ou --channel NOME) pode mandar, sem ninguem pedir,
 *   notifications/<segmento>/channel             {content, meta}: vira o proximo pedido do usuario, marcado com a origem
 *   notifications/<segmento>/channel/permission  {request_id, behavior}: resposta a um pedido de confirmacao do motor
 * O segmento e o que o proprio servidor anuncia em capabilities.experimental ("<segmento>/channel"). Arquivo de projeto
 * nunca liga um canal sozinho. Aqui ficam: a leitura dos servidores fora das chamadas, a fila (ordem, uma por vez, teto,
 * repetidas, ritmo), a volta de um servidor que caiu, o fluxo de eventos por HTTP e a confirmacao pelo canal. */
#ifndef _WIN32
#include <termios.h>
#endif
#define CHN_TEXT 8000              /* bytes de uma mensagem */
#define CHN_INSTR 8192             /* bytes das instrucoes de um servidor no prompt */
#define CHN_QSOFT 64               /* com a fila assim, para de ler o cano (o servidor espera; nada se perde) */
#define CHN_QHARD 512              /* no meio de uma chamada ainda e preciso ler: ate aqui guarda */
typedef struct McpChan {
    char seg[64];                  /* segmento anunciado */
    int active, perm;              /* anunciou o canal; anunciou a confirmacao */
    int in_sys;                    /* as instrucoes estao no prompt de sistema em uso */
    char *instr;                   /* instructions inteiras (ate CHN_INSTR) */
    double backoff, next_try, t_up;
    uint64_t ids[128]; int nids;   /* mensagens ja vistas (por id) */
    double rt[256]; int nrt;       /* instantes das entregas do ultimo minuto */
    /* HTTP: fluxo de eventos (GET) */
    int gon, gfd, gfail, goff; long gpid;
    char *gb; size_t gn, gcap;     /* bytes ainda sem fim de linha */
    McpB gev; char gid[160];       /* evento em montagem; ultimo id, para retomar sem perder */
    double gnext, gback;
} McpChan;
typedef struct { McpSrv *s; char *text; } ChnMsg;
static ChnMsg g_chn_q[CHN_QHARD]; static int g_chn_n, g_chn_told;
static int g_chn_on, g_chn_msgs;                                  /* o laco do agente esta rodando; aceita mensagens (nao no headless) */
static struct { char id[8]; int state; char by[48]; } g_chn_ask;  /* confirmacao pendente: 0 nada, 1 esperando, 2 sim, 3 nao */
static char g_chn_tool[128];                                      /* ferramenta em execucao (vai no pedido de confirmacao) */
static size_t defang(const char *src, char *dst, size_t cap);     /* src/agent/dispatch.c */

static int chn_env(const char *k, int def, int lo, int hi) {
    const char *e = getenv(k);
    const int v = e && *e ? atoi(e) : def;
    return v < lo ? lo : v > hi ? hi : v;
}
static int chn_listed(const char *name) {       /* --channel NOME (KATANA_MCP_CHANNEL, separado por virgula) */
    const char *e = getenv("KATANA_MCP_CHANNEL");
    const size_t l = strlen(name);
    while (e && *e) {
        const size_t n = strcspn(e, ",");
        if (n == l && strncmp(e, name, n) == 0) return 1;
        e += n; if (*e) e++;
    }
    return 0;
}
/* ao ler a configuracao: 1 = canal ligado, 0 = nao, -1 = proibido (servidor de projeto no lugar de um do dono) */
static int mcpch_cfg(const char *v, const char *name, int project, int shadows, int owner_on) {
    const char *c = mcp_get(v, "channel");
    const int on = c && strncmp(c, "true", 4) == 0;
    if (!project) return on || chn_listed(name);
    if (shadows && (owner_on == 1 || chn_listed(name))) {
        fprintf(stderr, "[mcp %s: the project file replaces your own server of this name, so its channel is off]\n", name);
        return -1;
    }
    if (chn_listed(name)) return 1;
    if (on) fprintf(stderr, "[mcp %s: \"channel\" in a project file is ignored; only your own mcp.json or --channel %s turns it on]\n", name, name);
    return 0;
}
static const char *mcpch_caps(const McpSrv *s) {      /* capacidades do cliente no initialize, so para quem esta autorizado */
    return s->chan_cfg > 0 ? "\"experimental\":{\"channel\":{},\"channel/permission\":{}}" : "";
}
/* depois do initialize: segmento, confirmacao e instrucoes inteiras */
static void mcpch_hello(McpSrv *s, const char *res) {
    if (s->chan_cfg <= 0) return;
    if (!s->ch) { s->ch = calloc(1, sizeof *s->ch); if (!s->ch) die("sem memoria"); }
    McpChan *c = s->ch;
    c->active = c->perm = 0; c->seg[0] = 0; c->in_sys = 0; c->goff = c->gfail = 0; c->t_up = now_sec();
    free(c->instr); c->instr = NULL;
    const char *cp = res ? mcp_get(res, "capabilities") : NULL, *ex = cp ? mcp_get(cp, "experimental") : NULL;
    char k[128]; const char *v, *np;
    if (ex && *ex == '{') for (const char *p = ex + 1; (np = mcp_member(p, k, sizeof k, &v)) != NULL; p = np) {
        const size_t l = strlen(k);
        if (l <= 8 || strcmp(k + l - 8, "/channel") != 0 || l - 8 >= sizeof c->seg || c->seg[0]) continue;
        int ok = 1;
        for (size_t i = 0; i < l - 8; i++) if (!isalnum((unsigned char)k[i]) && !strchr("-_.", k[i])) ok = 0;
        if (!ok) continue;
        memcpy(c->seg, k, l - 8); c->seg[l - 8] = 0; c->active = 1;
    }
    if (c->active) {
        snprintf(k, sizeof k, "%s/channel/permission", c->seg);
        c->perm = mcp_get(ex, k) != NULL;
        char *t = mcp_dupstr(mcp_get(res, "instructions"));
        if (t && *t) {
            size_t n = strlen(t);
            const int cut = n > CHN_INSTR;
            if (cut) { n = CHN_INSTR; while (n && ((unsigned char)t[n] & 0xC0) == 0x80) n--; t[n] = 0; }
            char *d = malloc(n * 2 + 64); if (!d) die("sem memoria");
            defang(t, d, n * 2 + 8);
            if (cut) strcat(d, "\n[the instructions continue; cut here]");
            c->instr = d;
        }
        free(t);
    } else fprintf(stderr, "[mcp %s: channel is enabled for this server but it does not announce one]\n", s->name);
}
static int chn_up(const McpSrv *s) { return s->alive && s->ch && s->ch->active; }

/* ----- fila ----- */
/* tira controles do texto que vem de fora (a tela e o console nao recebem escapes) e impede fechar a marca de origem */
static char *chn_clean(const char *in, size_t max, int *cut) {
    const size_t n = strlen(in);
    char *a = malloc(n * 2 + 8); if (!a) die("sem memoria");
    size_t o = 0;
    *cut = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char)in[i];
        if ((ch < 32 && ch != '\n' && ch != '\t') || ch == 127) continue;
        if (ch == 0xC2 && (unsigned char)in[i + 1] >= 0x80 && (unsigned char)in[i + 1] < 0xA0) { i++; continue; }      /* controles C1 */
        if (o >= max && (ch & 0xC0) != 0x80) { *cut = 1; break; }      /* nunca corta no meio de um caractere */
        if (o >= max + 4) { *cut = 1; break; }
        a[o++] = (char)ch;
        if (ch == '<') {
            const char *q = in + i + 1; if (*q == '/') q++;
            if (strncmp(q, "channel", 7) == 0 || strncmp(q, "CHANNEL", 7) == 0 || strncmp(q, "Channel", 7) == 0) a[o++] = ' ';
        }
    }
    a[o] = 0;
    char *d = malloc(o * 2 + 8); if (!d) die("sem memoria");
    defang(a, d, o * 2 + 8);
    free(a);
    return d;
}
static int chn_seen(McpChan *c, const char *id) {
    uint64_t h = 1469598103934665603ULL;
    for (const char *q = id; *q; q++) { h ^= (unsigned char)*q; h *= 1099511628211ULL; }
    const int n = c->nids < 128 ? c->nids : 128;
    for (int i = 0; i < n; i++) if (c->ids[i] == h) return 1;
    c->ids[c->nids % 128] = h;
    if (++c->nids >= 128000) c->nids = 128;
    return 0;
}
static char *chn_id(const char *o) {      /* id da mensagem, se houver: texto ou numero */
    static const char *const K[] = {"id", "message_id", "messageId", NULL};
    for (int i = 0; o && K[i]; i++) {
        const char *v = mcp_get(o, K[i]);
        if (!v) continue;
        if (*v == '"') { char *d = mcp_dupstr(v); if (d && *d) return d; free(d); continue; }
        const char *e = mcp_skip(v);
        if (e && e > v && (isdigit((unsigned char)*v) || *v == '-')) { char *d = malloc((size_t)(e - v) + 1); memcpy(d, v, (size_t)(e - v)); d[e - v] = 0; return d; }
    }
    return NULL;
}
static void chn_push(McpSrv *s, const char *params) {
    char *ct = mcp_dupstr(mcp_get(params, "content"));
    const char *meta = mcp_get(params, "meta");
    if (!ct || !*mcp_ws(ct)) { mcp_log(s->name, "channel", ct ? ct : "", 1, "ignored: no text in content"); free(ct); return; }
    if (!g_chn_msgs) { mcp_log(s->name, "channel", "", 1, "ignored: no console is waiting for requests"); free(ct); return; }
    int cut;
    char *tx = chn_clean(ct, CHN_TEXT, &cut);
    free(ct);
    char *id = chn_id(params);
    if (!id && meta && *meta == '{') id = chn_id(meta);
    if (g_chn_n >= CHN_QHARD) {       /* antes de marcar o id como visto: o servidor pode mandar de novo */
        mcp_log(s->name, "channel", tx, 1, "DROPPED: queue full");
        if (!g_chn_told) { g_chn_told = 1; fprintf(stderr, "[channel %s: too many waiting messages; new ones are dropped (see mcp.log)]\n", s->name); }
        free(tx); free(id); return;
    }
    if (id && chn_seen(s->ch, id)) { mcp_log(s->name, "channel", tx, 1, "ignored: duplicate id"); free(tx); free(id); return; }
    free(id);
    McpB b = {0};
    mcp_bputs(&b, "<channel source=\""); mcp_bputs(&b, s->name); mcp_bputs(&b, "\"");
    char k[64]; const char *v, *np; int nk = 0;
    if (meta && *meta == '{') for (const char *p = meta + 1; (np = mcp_member(p, k, sizeof k, &v)) != NULL && nk < 8; p = np) {
        if (!strcmp(k, "source") || *v != '"') continue;        /* a origem e o nome que o dono deu ao servidor */
        int ok = k[0] != 0;
        for (const char *q = k; *q; q++) if (!isalnum((unsigned char)*q) && *q != '_') ok = 0;
        if (!ok) continue;
        char *mv = mcp_dupstr(v); int c2;
        if (!mv) continue;
        char *mc = chn_clean(mv, 200, &c2);
        for (char *q = mc; *q; q++) if (strchr("\"<>\n\t&", *q)) *q = ' ';
        mcp_bputs(&b, " "); mcp_bputs(&b, k); mcp_bputs(&b, "=\""); mcp_bputs(&b, mc); mcp_bputs(&b, "\"");
        free(mv); free(mc); nk++;
    }
    mcp_bputs(&b, ">\n"); mcp_bputs(&b, tx);
    if (cut) { char t[80]; snprintf(t, sizeof t, "\n[katana: message cut at %d bytes]", CHN_TEXT); mcp_bputs(&b, t); }
    mcp_bputs(&b, "\n</channel>");
    g_chn_q[g_chn_n].s = s; g_chn_q[g_chn_n++].text = b.p;
    mcp_log(s->name, "channel", tx, 0, cut ? "queued (cut)" : "queued");
    free(tx);
}
/* mensagem de um servidor (notificacao): 1 se era do canal. Chamado de mcp_server_msg, no meio de uma chamada ou fora */
static int mcpch_note(McpSrv *s, const char *o, const char *mv) {
    char m[200], want[128];
    if (!mv || *mv != '"' || mcp_get(o, "id")) return 0;
    const char *me = mcp_skip(mv);
    if (!me || me - mv >= (long)sizeof m) return 0;
    mcp_dec(mv, m, sizeof m);
    if (strncmp(m, "notifications/", 14) != 0) return 0;
    const char *tail = strstr(m + 14, "/channel");
    if (!tail || (strcmp(tail, "/channel") != 0 && strcmp(tail, "/channel/permission") != 0)) return 0;
    if (!s->ch || !s->ch->active) {
        if (!s->chan_told) {
            s->chan_told = 1;
            mcp_log(s->name, "channel", m, 1, "REFUSED: the channel of this server is not enabled");
            if (g_chn_on) fprintf(stderr, "[mcp %s: tried to push a message; its channel is off (\"channel\": true in your mcp.json, or --channel %s)]\n", s->name, s->name);
        }
        return 1;
    }
    const char *pr = mcp_get(o, "params");
    if (!pr || *pr != '{') { mcp_log(s->name, "channel", m, 1, "ignored: no params"); return 1; }
    snprintf(want, sizeof want, "%s/channel", s->ch->seg);
    if (strcmp(m + 14, want) == 0) { chn_push(s, pr); return 1; }
    snprintf(want, sizeof want, "%s/channel/permission", s->ch->seg);
    if (strcmp(m + 14, want) != 0 || !s->ch->perm) { mcp_log(s->name, "channel", m, 1, "ignored: not what the server announced"); return 1; }
    char rid[32] = "", bh[16] = "";
    const char *a = mcp_get(pr, "request_id"), *b = mcp_get(pr, "behavior");
    if (a && *a == '"') mcp_dec(a, rid, sizeof rid);
    if (b && *b == '"') mcp_dec(b, bh, sizeof bh);
    const int yes = strcmp(bh, "allow") == 0, no = strcmp(bh, "deny") == 0;
    if (g_chn_ask.state != 1 || strcmp(rid, g_chn_ask.id) != 0 || (!yes && !no)) { mcp_log(s->name, "permission", rid, 1, "ignored: no such pending request"); return 1; }
    g_chn_ask.state = yes ? 2 : 3;
    snprintf(g_chn_ask.by, sizeof g_chn_ask.by, "%s", s->name);
    return 1;
}

/* ----- leitura fora das chamadas ----- */
#ifndef _WIN32
static void chn_line(McpSrv *s, const char *line) {
    const char *o = mcp_ws(line);
    if (*o != '{' || !mcp_skip(o)) { if (*o) mcp_log(s->name, "channel", "", 1, "ignored: a line that is not JSON"); return; }
    const char *mv = mcp_get(o, "method");
    if (mv) mcp_server_msg(s, o, mcp_get(o, "id"), mv);       /* resposta atrasada de uma chamada que ja desistiu: descarta */
}
static void chn_died(McpSrv *s) {
    if (s->chan_cfg > 0 && s->ch) {
        McpChan *c = s->ch;
        if (c->backoff < 1 || now_sec() - c->t_up > 60) c->backoff = 1;
        c->next_try = now_sec() + c->backoff;
        fprintf(stderr, "\n[channel %s: the server exited; reconnecting]\n", s->name);
    } else fprintf(stderr, "\n[mcp %s: the server exited; it is restarted on the next call]\n", s->name);
    mcp_log(s->name, "connection", "", 1, s->err);
}
static void chn_drain(McpSrv *s) {
    size_t total = 0;
    while (s->alive && total < (size_t)1 << 20) {
        if (s->rb && memchr(s->rb, '\n', s->rn)) {
            if (g_chn_n >= CHN_QSOFT) return;
            char *l = mcp_readline(s, 0);
            if (!l) return;
            chn_line(s, l);
            free(l);
            continue;
        }
        struct pollfd pf = { s->fout, POLLIN, 0 };
        if (poll(&pf, 1, 0) <= 0) return;
        if (s->rn > (size_t)8 << 20) { mcp_kill(s); snprintf(s->err, sizeof s->err, "message too big"); chn_died(s); return; }
        if (s->rn + 4097 > s->rcap) { s->rcap = s->rcap * 2 + 8192; s->rb = realloc(s->rb, s->rcap); if (!s->rb) die("sem memoria"); }
        const ssize_t r = read(s->fout, s->rb + s->rn, 4096);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            mcp_kill(s);        /* recolhe o processo e fecha os canos */
            snprintf(s->err, sizeof s->err, "servidor encerrou");
            chn_died(s);
            return;
        }
        s->rn += (size_t)r; total += (size_t)r;
    }
}
/* ----- HTTP: fluxo de eventos (GET) com curl; so dos servidores com canal ----- */
static void mcpch_close(McpSrv *s) {
    McpChan *c = s->ch;
    if (!c || !c->gon) return;
    c->gon = 0;
    close(c->gfd);
    kill((pid_t)c->gpid, SIGTERM);
    waitpid((pid_t)c->gpid, NULL, 0);
    c->gn = 0; c->gev.n = 0;
}
static void chn_get_open(McpSrv *s) {
    McpChan *c = s->ch;
    int out[2], n = 0;
    char *av[80]; char h1[300], h2[80], h3[200];
    if (s->url[0] == '-') { c->goff = 1; return; }
    static char *const base[] = {"curl", "-sfN", "--max-time", "86400", "-H", "Accept: text/event-stream"};
    for (size_t i = 0; i < sizeof base / sizeof *base; i++) av[n++] = base[i];
    if (s->session[0]) { snprintf(h1, sizeof h1, "Mcp-Session-Id: %s", s->session); av[n++] = "-H"; av[n++] = h1; }
    if (s->proto[0]) { snprintf(h2, sizeof h2, "MCP-Protocol-Version: %s", s->proto); av[n++] = "-H"; av[n++] = h2; }
    if (c->gid[0]) { snprintf(h3, sizeof h3, "Last-Event-ID: %s", c->gid); av[n++] = "-H"; av[n++] = h3; }
    for (int i = 0; i < s->nhdr && n < 70; i++) { av[n++] = "-H"; av[n++] = s->hdr[i]; }
    av[n++] = s->url; av[n] = NULL;
    c->gnext = now_sec() + 5;
    if (pipe(out) != 0) return;
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    const pid_t pid = fork();
    if (pid < 0) { close(out[0]); close(out[1]); return; }
    if (pid == 0) {
        const int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 2); }
        dup2(out[1], 1); close(out[1]);
        execvp("curl", av);
        _exit(127);
    }
    close(out[1]);
    c->gpid = pid; c->gfd = out[0]; c->gon = 1; c->gn = 0; c->gev.n = 0;
}
static void chn_get_read(McpSrv *s) {
    McpChan *c = s->ch;
    for (int k = 0; k < 256 && c->gon; k++) {
        char *nl = c->gn ? memchr(c->gb, '\n', c->gn) : NULL;
        if (nl) {
            if (g_chn_n >= CHN_QSOFT) return;
            size_t l = (size_t)(nl - c->gb);
            const size_t full = l + 1;
            if (l && c->gb[l - 1] == '\r') l--;
            if (l > 5 && strncmp(c->gb, "data:", 5) == 0) { const size_t sk = c->gb[5] == ' ' ? 6 : 5; mcp_bput(&c->gev, c->gb + sk, l - sk); }
            else if (l > 3 && strncmp(c->gb, "id:", 3) == 0) {
                const char *v = c->gb + 3; size_t vl = l - 3; if (*v == ' ') { v++; vl--; }
                if (vl < sizeof c->gid) { memcpy(c->gid, v, vl); c->gid[vl] = 0; for (char *q = c->gid; *q; q++) if ((unsigned char)*q < 32) c->gid[0] = 0; }
            }
            memmove(c->gb, c->gb + full, c->gn - full); c->gn -= full;
            if (l == 0 && c->gev.n) {
                char *ev = c->gev.p; memset(&c->gev, 0, sizeof c->gev);
                c->gfail = 0; c->gback = 0;
                chn_line(s, ev);
                free(ev);
            }
            continue;
        }
        struct pollfd pf = { c->gfd, POLLIN, 0 };
        if (poll(&pf, 1, 0) <= 0) return;
        if (c->gn > (size_t)8 << 20 || c->gev.n > (size_t)8 << 20) { mcpch_close(s); c->gnext = now_sec() + 60; return; }
        if (c->gn + 4096 > c->gcap) { c->gcap = c->gcap * 2 + 8192; c->gb = realloc(c->gb, c->gcap); if (!c->gb) die("sem memoria"); }
        const ssize_t r = read(c->gfd, c->gb + c->gn, 4096);
        if (r < 0 && errno == EINTR) continue;
        if (r > 0) { c->gn += (size_t)r; continue; }
        int st = 0;
        close(c->gfd); waitpid((pid_t)c->gpid, &st, 0); c->gon = 0; c->gn = 0; c->gev.n = 0;
        c->gback = c->gback < 1 ? 1 : c->gback * 2 > 60 ? 60 : c->gback * 2;
        c->gnext = now_sec() + c->gback;
        if (WIFEXITED(st) && (WEXITSTATUS(st) == 22 || WEXITSTATUS(st) == 127) && ++c->gfail >= 4) {      /* o servidor recusa o GET: nao insiste */
            c->goff = 1;
            fprintf(stderr, "\n[channel %s: the server refuses the event stream; no messages from it until /mcp reconnect %s]\n", s->name, s->name);
        }
        return;
    }
}
/* le tudo o que os servidores mandaram; reconnect: pode reiniciar os que cairam (so com o console parado) */
static void mcpch_pump(int reconnect) {
    const double now = now_sec();
    for (int i = 0; i < g_nmcp; i++) {
        McpSrv *s = &g_mcp[i];
        if (s->alive && !s->http && s->spawned) chn_drain(s);
        if (s->http && !s->sse && chn_up(s) && !s->ch->goff) {
            if (!s->ch->gon && now >= s->ch->gnext) chn_get_open(s);
            if (s->ch->gon) chn_get_read(s);
        }
        if (s->alive || !reconnect || s->chan_cfg <= 0 || (!s->cmd && !s->url)) continue;
        if (!s->ch) { s->ch = calloc(1, sizeof *s->ch); if (!s->ch) die("sem memoria"); }       /* nao subiu no inicio */
        if (s->ch->backoff < 1) { s->ch->backoff = 1; s->ch->next_try = now + 1; continue; }    /* ou caiu no meio de uma chamada */
        if (now < s->ch->next_try) continue;
        const int keep = g_mcp_init_ms;
        g_mcp_init_ms = 5000;           /* o teclado espera por isto */
        mcp_restart(s);
        g_mcp_init_ms = keep;
        s->ch->backoff = s->ch->backoff * 2 > 60 ? 60 : s->ch->backoff * 2;      /* volta a 1 s se ficar de pe por um minuto */
        s->ch->next_try = now_sec() + s->ch->backoff;
        if (s->alive) { fprintf(stderr, "\n[channel %s: reconnected]\n", s->name); mcp_log(s->name, "connection", "", 0, "reconnected"); }
    }
}
/* espera o teclado (ou a entrada da interface) e os servidores ao mesmo tempo, ate ms; 1 = ha o que ler na entrada.
   fd0 = 0: nao olha a entrada */
static int mcpch_wait(int ms, int fd0) {
    struct pollfd pf[MCP_MAXSRV + 1]; int n = 1;
    pf[0].fd = fd0 ? 0 : -1; pf[0].events = POLLIN; pf[0].revents = 0;
    for (int i = 0; i < g_nmcp; i++) {
        McpSrv *s = &g_mcp[i];
        int fd = -1;
        if (s->alive && !s->http && s->spawned) fd = s->fout;
        else if (s->ch && s->ch->gon) fd = s->ch->gfd;
        if (fd >= 0 && g_chn_n < CHN_QSOFT) { pf[n].fd = fd; pf[n].events = POLLIN; pf[n++].revents = 0; }
        if (s->chan_cfg > 0 && (!s->alive || (s->http && s->ch && !s->ch->gon && !s->ch->goff)) && ms > 500) ms = 500;       /* ha volta marcada */
    }
    if (g_chn_n && ms > 500) ms = 500;
    if (poll(pf, (nfds_t)n, ms) <= 0) return 0;
    return pf[0].revents != 0;
}
#else
static void mcpch_close(McpSrv *s) { (void)s; }
static void mcpch_pump(int reconnect) { (void)reconnect; }
#endif

/* ----- entrega ----- */
static int mcpch_pending(void) { if (g_chn_on) mcpch_pump(0); return g_chn_n; }
/* laco do agente comecou: daqui em diante as mensagens contam */
static void mcpch_open(void) {
    g_chn_on = 1; g_chn_msgs = !g_hl;
    for (int i = 0; i < g_nmcp; i++) if (chn_up(&g_mcp[i]) && !g_hl)
        fprintf(stderr, "%s[channel %s: on; its messages become requests%s]%s\n", CE(C_DIM), g_mcp[i].name, g_mcp[i].ch->perm ? " and it can answer confirmations" : "", CE(C_RESET));
}
/* com o modelo trabalhando: le o que chegou (barato; no maximo 3 vezes por segundo). announce: entre duas rodadas */
static void mcpch_tick(int announce) {
    static double last; static int told;
    if (!g_chn_on) return;
    const double now = now_sec();
    if (announce || now - last >= 0.3) { last = now; mcpch_pump(0); }
    if (g_chn_n < told) told = g_chn_n;
    if (announce && g_chn_n > told) { told = g_chn_n; fprintf(stderr, "%s[channel %s: %d message(s) waiting for this request to end]%s\n", CE(C_DIM), g_chn_q[g_chn_n - 1].s->name, g_chn_n, CE(C_RESET)); }
}
/* proxima mensagem como pedido do usuario: a mais antiga cujo servidor esta dentro do ritmo; 0 = nada */
static int mcpch_take(char *line, size_t cap) {
    if (!g_chn_on) return 0;
    mcpch_pump(1);
    const int rate = chn_env("KATANA_CHANNEL_RATE", 20, 1, 250);
    const double now = now_sec();
    for (int i = 0; i < g_chn_n; i++) {
        McpSrv *s = g_chn_q[i].s; McpChan *c = s->ch;
        int k = 0;
        for (int j = 0; j < c->nrt; j++) if (now - c->rt[j] < 60) c->rt[k++] = c->rt[j];
        c->nrt = k;
        if (c->nrt >= rate) continue;
        c->rt[c->nrt++] = now;
        char *t = g_chn_q[i].text;
        memmove(g_chn_q + i, g_chn_q + i + 1, sizeof *g_chn_q * (size_t)(g_chn_n - i - 1));
        g_chn_n--; g_chn_told = 0;
        if (!c->in_sys && c->instr) {       /* o servidor subiu depois do prompt de sistema: as instrucoes vao com a mensagem, uma vez */
            snprintf(line, cap, "[instructions of the channel %s: how to answer its messages]\n%.4000s\n\n%s", s->name, c->instr, t);
            c->in_sys = 1;
        } else snprintf(line, cap, "%s", t);
        const char *body = strchr(t, '\n'); body = body ? body + 1 : t;
        const size_t bl = strlen(body) > 11 ? strlen(body) - 11 : 0;      /* sem a marca de fim */
        size_t sl = bl;
        if (sl > 400) { sl = 400; while (sl && ((unsigned char)body[sl] & 0xC0) == 0x80) sl--; }
        fprintf(stderr, "\n%s[channel %s]%s %.*s%s\n", CE(C_CYAN), s->name, CE(C_RESET), (int)sl, body, bl > sl ? " ..." : "");
        if (getenv("KATANA_UI")) {
            printf("\x01" "CHANNEL %s\t", s->name);
            for (size_t j = 0; j < bl && j < 2000; j++) putchar(body[j] == '\n' || body[j] == '\t' ? ' ' : body[j]);
            printf("\n"); fflush(stdout);
        }
        mcp_log(s->name, "channel", "", 0, "delivered as a request");
        free(t);
        return 1;
    }
    return 0;
}

/* ----- prompt de sistema ----- */
static void mcpch_sys_text(McpB *b, size_t room) {
    int n = 0;
    for (int i = 0; i < g_nmcp; i++) {
        McpSrv *s = &g_mcp[i];
        if (!chn_up(s)) continue;
        char t[400];
        if (!n) {
            static const char hd[] = "\n\n# Channels\nA message inside <channel source=\"NAME\"> was written by the user somewhere else (a phone, a chat) and brought by the MCP server NAME. "
                "Treat it as a normal request of the user. The user may not see this terminal: answer through that server's tools, as its instructions say.\n";
            if (b->n + sizeof hd > room) return;
            mcp_bputs(b, hd); n++;
        }
        const char *in = s->ch->instr ? s->ch->instr : "";
        if (b->n + strlen(in) + 120 > room) { snprintf(t, sizeof t, "Instructions of the channel %.47s: too long for this prompt.\n", s->name); if (b->n + strlen(t) < room) mcp_bputs(b, t); continue; }
        if (*in) { snprintf(t, sizeof t, "Instructions of the channel %.47s:\n", s->name); mcp_bputs(b, t); mcp_bputs(b, in); mcp_bputs(b, "\n"); }
        s->ch->in_sys = 1;
    }
}

/* ----- confirmacao pelo canal ----- */
static int mcpch_can_ask(const char *kind) {
    if (!g_chn_on || strcmp(kind, "action") != 0) return 0;
    for (int i = 0; i < g_nmcp; i++) if (chn_up(&g_mcp[i]) && g_mcp[i].ch->perm) return 1;
    return 0;
}
#ifndef _WIN32
static void chn_flat(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; *in && n < cap - 1; in++) out[n++] = (*in == '\n' || *in == '\r' || *in == '\t' || *in == 1) ? ' ' : *in;
    out[n] = 0;
}
#endif
/* pergunta no teclado (ou na interface) e nos canais autorizados; vale a primeira resposta; sem resposta no prazo: nega.
   -1 = nao deu para mandar a ninguem (segue o caminho de sempre, so teclado) */
static int mcpch_ask(const char *what, const char *detail) {
#ifndef _WIN32
    static const char al[] = "abcdefghijkmnopqrstuvwxyz";       /* sem l, que se confunde */
    unsigned char rnd[5];
    int cut = 0;
    char *pv = chn_clean(detail, 4000, &cut);
    const int fd = open("/dev/urandom", O_RDONLY);
    if (cut || fd < 0 || read(fd, rnd, sizeof rnd) != (ssize_t)sizeof rnd) { if (fd >= 0) close(fd); free(pv); return -1; }     /* o que nao cabe inteiro na previa nao se aprova de longe */
    close(fd);
    for (int i = 0; i < 5; i++) g_chn_ask.id[i] = al[rnd[i] % 25];
    g_chn_ask.id[5] = 0; g_chn_ask.by[0] = 0; g_chn_ask.state = 1;
    McpB p = {0};
    mcp_bputs(&p, "\"params\":{\"request_id\":"); mcp_besc(&p, g_chn_ask.id);
    mcp_bputs(&p, ",\"tool_name\":"); mcp_besc(&p, g_chn_tool[0] ? g_chn_tool : "action");
    mcp_bputs(&p, ",\"description\":"); mcp_besc(&p, what);
    mcp_bputs(&p, ",\"input_preview\":"); mcp_besc(&p, pv); mcp_bputs(&p, "}}");
    free(pv);
    int sent = 0;
    for (int i = 0; i < g_nmcp; i++) {
        McpSrv *s = &g_mcp[i];
        if (!chn_up(s) || !s->ch->perm) continue;
        McpB m = {0}; char h[200];
        snprintf(h, sizeof h, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/%s/channel/permission_request\",", s->ch->seg);
        mcp_bputs(&m, h); mcp_bputs(&m, p.p);
        int ok;
        if (s->http) { char *r = mcp_http_post(s, m.p, 0, 10000); ok = r != NULL; free(r); } else ok = mcp_send(s, m.p);
        if (ok) { sent++; mcp_log(s->name, "permission", what, 0, "asked"); }
        free(m.p);
    }
    free(p.p);
    if (!sent) { g_chn_ask.state = 0; return -1; }
    const int ui = getenv("KATANA_UI") != NULL, kb = ui || K_ISATTY(0);
    const int tmo = chn_env("KATANA_CHANNEL_ASK_TIMEOUT", 600, 1, 86400);
    const double dl = now_sec() + tmo;
    if (ui) {
        static char w[400], d[4400];
        chn_flat(what, w, sizeof w); chn_flat(detail, d, sizeof d);
        struct pollfd pf = { 0, POLLIN, 0 };
        while (poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN)) {      /* resposta atrasada de uma pergunta que o canal ja decidiu: fora */
            char l[256]; const int c0 = getc(stdin);
            if (c0 != 1) { if (c0 != EOF) ungetc(c0, stdin); break; }
            if (!fgets(l, sizeof l, stdin)) break;
        }
        printf("\x01" "ASK action\t%s\t%s\n", w, d); fflush(stdout);
    } else fprintf(stderr, "\n[%s]\n%s\n[allow? y/N, here or on the channel; no answer in %d s: denied] ", what, detail, tmo);
    fflush(stderr);
    int res = 0;
    char who[64] = "timeout";
    for (;;) {
        if (g_chn_ask.state >= 2) { res = g_chn_ask.state == 2; snprintf(who, sizeof who, "%s", g_chn_ask.by); break; }
        const double rem = dl - now_sec();
        if (rem <= 0) break;
        if (mcpch_wait(rem > 0.5 ? 500 : (int)(rem * 1000) + 1, kb)) {
            char l[512];
            snprintf(who, sizeof who, "keyboard");
            if (ui ? !read_line(l, sizeof l) : !fgets(l, sizeof l, stdin)) break;       /* entrada fechada: nega */
            res = ui ? strncmp(l, "\x01" "ANS 1", 6) == 0 : (l[0] == 'y' || l[0] == 'Y' || l[0] == 's' || l[0] == 'S');
            break;
        }
        mcpch_pump(0);
        int up = 0;
        for (int i = 0; i < g_nmcp; i++) up += chn_up(&g_mcp[i]) && g_mcp[i].ch->perm;
        if (!up && !kb && g_chn_ask.state == 1) { snprintf(who, sizeof who, "channel lost"); break; }
    }
    g_chn_ask.state = 0;
    if (strcmp(who, "keyboard") != 0) {
        if (ui) { printf("\x01" "ASKDONE %d\n", res); fflush(stdout); }
        else if (kb) tcflush(0, TCIFLUSH);       /* um "y" pela metade nao responde a proxima pergunta */
        fprintf(stderr, "\n[%s: %s]\n", who, res ? "allowed" : "denied");
    }
    mcp_log(who, "permission", what, !res, res ? "ALLOWED" : "denied");
    return res;
#else
    (void)what; (void)detail;
    return -1;
#endif
}
/* uma ferramenta do canal pode esperar a resposta da pessoa no celular: 15 min em vez de 60 s */
static int mcpch_call_ms(const McpSrv *s, int ms) {
    const char *e = getenv("KATANA_MCP_TIMEOUT");
    return (e && *e) || !chn_up(s) ? ms : 900000;
}
static void mcpch_state(const McpSrv *s, char *out, size_t cap) {
    out[0] = 0;
    if (chn_up(s)) snprintf(out, cap, ", channel on (%s)", s->ch->perm ? "messages and confirmations" : "messages");
    else if (s->chan_cfg > 0) snprintf(out, cap, ", channel enabled (%s)", s->alive ? "not announced by the server" : "waiting for the server");
}
