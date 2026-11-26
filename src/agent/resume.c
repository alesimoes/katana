/* ---------- estado durável do trabalho: registro de eventos, aplicacao, carga, ponto de salvamento (docs/flow-resume.md) ----------
 * O estado do trabalho (o que e, onde esta, o que foi ganho, o que falhou, o que roda) e o tronco; a conversa e um cache dele.
 * Uma estrutura, RzState, guarda tudo. Cada mudanca e UMA linha anexada a um arquivo antes de valer na memoria (escrita
 * antecipada): a operacao ao vivo e a recuperacao rodam a mesma funcao, rz_apply. O ponto de salvamento e o mesmo arquivo
 * reescrito como a lista de eventos mais curta que reproduz o estado: um formato, um leitor, um caminho de codigo.
 * O que se le de volta e hipotese: a conferencia com o mundo (reconciliacao) vem depois da carga. Resultados de ferramenta
 * nunca sao gravados, so o hash; todo texto passa por rz_clean (mascara de segredos). KATANA_RESUME=0 desliga tudo. */
#ifndef _WIN32
#include <sys/file.h>
#endif
#define RZ_VERSION 1
#define RZ_ATT 24            /* tentativas falhas */
#define RZ_EVID 16           /* evidencias ganhas (anel) */
#define RZ_FILE 24           /* arquivos tocados ou produzidos */
#define RZ_DEC 8             /* decisoes do usuario (anel) */
#define RZ_WORK 3            /* trabalhos suspensos */
#define RZ_NJOB 8            /* tamanho da tabela de tarefas espelhada (o mesmo JOB_N de jobs.c) */
#define RZ_SEEN 32
#define RZ_RING 16
#define RZ_MAXF 48           /* campos por linha */
#define RZ_LINE 16384
enum { RZJ_RUN = 1, RZJ_WAIT, RZJ_DONE, RZJ_STOPPED, RZJ_LOST, RZJ_DETACHED };
enum { RZF_WRITE = 1, RZF_MADE };                     /* editado pelo agente; artefato produzido (imagem, arquivo a entregar) */
enum { RZR_NONE, RZR_SESSION, RZR_SUMMARY, RZR_INTERRUPT, RZR_IDLE, RZR_TASK, RZR_RECALL };
enum { RZE_READ = 1, RZE_CMD, RZE_WEB, RZE_PLAN, RZE_SUB, RZE_USER };
static const char *const RZ_JNAME[] = {"", "run", "wait", "done", "stopped", "lost", "detached"};
static const char *const RZ_ENAME[] = {"", "read", "cmd", "web", "plan", "sub", "user"};
static const char *const RZ_RNAME[] = {"none", "session", "summary", "interrupt", "idle", "task", "recall"};

typedef struct { int id; long pid, agent_pid; long long start, t0; unsigned char status, told; int rc; long log_sz;
                 char cmd[200], log[1300], asked[240]; } RzJob;
typedef struct { char path[400]; uint64_t hash; long long size, mtime; unsigned char op, shown, gone, changed; int req; } RzFile;
typedef struct { uint64_t key, call; char tool[24], subj[80], cause[120], fix[120]; int n, req, epoch; long long t; unsigned char dead; } RzAttempt;
typedef struct { unsigned char kind; uint64_t key, hash; char text[160], path[400]; int req; unsigned char stale; } RzEvid;
typedef struct { char q[160], a[160]; int req; } RzDecision;
typedef struct { char goal[400]; FlowPos stk[FL_DEPTH]; int depth; long long t; } RzWork;
typedef struct {
    char sess[64], host[64], cwd[1024]; long agent_pid; long long t_event, t_clean;
    char goal[400], last_req[240]; int req;
    FlowPos stk[FL_DEPTH]; int depth, task, cycles, webcalls, badfetch;       /* espelho de g_fl */
    char path[400], err[170], errfile[330];
    char todo[3000]; int plan_mode, worktree;
    char open_tool[24], open_subj[120]; uint64_t open_sig;                    /* chamada iniciada e sem resultado */
    char ask[240]; int ask_task;                                              /* pergunta pendente ao usuario */
    RzJob job[RZ_NJOB]; RzFile file[RZ_FILE]; int nfile;
    RzAttempt att[RZ_ATT]; int natt; RzEvid ev[RZ_EVID]; int nev, ev_w; RzDecision dec[RZ_DEC]; int ndec, dec_w;
    RzWork work[RZ_WORK]; int nwork;
    uint64_t seen[RZ_SEEN]; int nseen;                                        /* g_fl.seen (so no ponto de salvamento) */
    struct { uint64_t sig, res; int n, req; } ring[RZ_RING]; int nring;       /* g_fl.ring (so no ponto de salvamento) */
    int epoch;                                                                /* sobe a cada reentrada */
} RzState;
static struct {
    RzState s; FILE *f; int lockfd, readonly; long seq, since_ckpt; char file[1100];
    int reason;                                                               /* reentrada pendente (RZR_*) */
    int grace, score_left, sc_on, sc_dead, sc_dup, sc_reason; char delta[6][200]; int ndelta;
    int loaded, new_proc, moved, foreign, noted;                              /* carga: houve estado; o que mudou no ambiente; aviso de erro dado */
} g_rz;
#define RZ_UNUSED __attribute__((unused))

static int rz_on(void) { const char *e = getenv("KATANA_RESUME"); return !(e && !strcmp(e, "0")); }
RZ_UNUSED static int rz_strict(void) { const char *e = getenv("KATANA_RESUME"); return e && !strcmp(e, "strict"); }
/* o caminho do arquivo de estado: o da sessao sem .ksess e com .kstate; sem sessao, so KATANA_RESUME_FILE */
static void rz_path(const char *sess, char *o, size_t cap) {
    o[0] = 0;
    if (sess && sess[0]) {
        const size_t n = strlen(sess);
        if (n > 6 && !strcmp(sess + n - 6, ".ksess")) snprintf(o, cap, "%.*s.kstate", (int)(n - 6), sess);
        else snprintf(o, cap, "%s.kstate", sess);
        return;
    }
    const char *e = getenv("KATANA_RESUME_FILE");
    if (e && *e) snprintf(o, cap, "%s", e);
}
static void rz_cpy(char *d, size_t cap, const char *s) { size_t n = s ? strlen(s) : 0; if (n >= cap) n = cap - 1; if (n) memcpy(d, s, n); d[n] = 0; }
/* texto para dentro do estado: no maximo cap-1 bytes, corte seguro em UTF-8, segredos mascarados; multi: mantem as quebras de linha (so a lista de tarefas) */
RZ_UNUSED static void rz_clean(char *d, size_t cap, const char *s, int multi) {
    if (!s) s = "";
    if (!multi) fc_cut(d, cap, s);
    else {
        size_t n = strlen(s); if (n >= cap) { n = cap - 1; while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--; }
        memcpy(d, s, n); d[n] = 0;
    }
    secret_mask(d);
}
/* numero em texto, para os campos (16 casas rotativas: uma linha usa varias) */
static const char *rz_i(long long v) { static char b[16][28]; static int k; char *o = b[k++ & 15]; snprintf(o, 28, "%lld", v); return o; }
static const char *rz_x(uint64_t v) { static char b[16][20]; static int k; char *o = b[k++ & 15]; snprintf(o, 20, "%llx", (unsigned long long)v); return o; }

/* ---- escape de campo: \ vira \\, TAB vira \t, nova linha vira \n, o resto abaixo de 0x20 vira espaco ---- */
static size_t rz_esc(char *o, size_t cap, const char *s) {
    size_t n = 0;
    for (; *s && n + 3 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '\\') { o[n++] = '\\'; o[n++] = '\\'; }
        else if (c == '\t') { o[n++] = '\\'; o[n++] = 't'; }
        else if (c == '\n') { o[n++] = '\\'; o[n++] = 'n'; }
        else if (c < 0x20) o[n++] = ' ';
        else o[n++] = (char)c;
    }
    o[n] = 0;
    return *s ? 0 : n;      /* 0: nao coube */
}
static void rz_unesc(char *s) {
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r == '\\' && r[1]) { r++; *w++ = *r == 't' ? '\t' : *r == 'n' ? '\n' : *r; }
        else *w++ = *r;
    }
    *w = 0;
}
#define RZ_FNV 1469598103934665603ULL
/* a linha: <seq> TAB <epoch> TAB <tipo> TAB campos... TAB <crc>; devolve o tamanho (0: nao coube) */
static size_t rz_line(char *o, size_t cap, long seq, long long ts, char kind, int n, const char *const *f) {
    size_t l = (size_t)snprintf(o, cap, "%ld\t%lld\t%c", seq, ts, kind);
    for (int i = 0; i < n; i++) {
        if (l + 16 >= cap) return 0;
        o[l++] = '\t';
        const size_t e = rz_esc(o + l, cap - l - 12, f[i] ? f[i] : "");
        if (!e && f[i] && f[i][0]) return 0;
        l += e;
    }
    if (l + 14 >= cap) return 0;
    const unsigned crc = (unsigned)(fl_hash(o, l, RZ_FNV) & 0xffffffffu);
    l += (size_t)snprintf(o + l, cap - l, "\t%08x\n", crc);
    return l;
}
/* a linha lida: confere o crc, separa os campos (ja sem escape). -1: linha ruim; senao o numero de campos */
static int rz_parse(char *ln, long long *ts, char *kind, char **f, int maxf, long *seq) {
    size_t l = strlen(ln);
    while (l && (ln[l - 1] == '\n' || ln[l - 1] == '\r')) ln[--l] = 0;
    char *lt = strrchr(ln, '\t'); if (!lt) return -1;
    char *ce = NULL; const unsigned long crc = strtoul(lt + 1, &ce, 16);
    if (!ce || ce == lt + 1 || *ce || (unsigned)crc != (unsigned)(fl_hash(ln, (size_t)(lt - ln), RZ_FNV) & 0xffffffffu)) return -1;
    *lt = 0;
    char *p = ln;
    char *t1 = strchr(p, '\t'); if (!t1) return -1; *t1 = 0; *seq = atol(p); p = t1 + 1;
    char *t2 = strchr(p, '\t'); if (!t2) return -1; *t2 = 0; *ts = atoll(p); p = t2 + 1;
    if (!*p) return -1;
    *kind = *p;
    char *t3 = strchr(p, '\t');
    int n = 0;
    while (t3 && n < maxf) { p = t3 + 1; f[n++] = p; t3 = strchr(p, '\t'); if (t3) *t3 = 0; rz_unesc(p); }
    return n;
}

/* ---- fluxo e estado por NOME: uma tabela reordenada nao corrompe um estado gravado ---- */
static void rz_fl_enc(const FlowPos *p, char *o, size_t cap) { snprintf(o, cap, "%s:%s", FL_BOOK[p->id].name, FL_BOOK[p->id].st[p->st].name); }
static int rz_fl_dec(const char *s, FlowPos *p) {
    const char *c = strchr(s, ':'); if (!c) return 0;
    const size_t fl = (size_t)(c - s);
    for (int id = 1; id < FL_COUNT; id++) {
        if (strlen(FL_BOOK[id].name) != fl || strncmp(FL_BOOK[id].name, s, fl)) continue;
        for (int i = 0; i < FL_BOOK[id].nst; i++) if (!strcmp(FL_BOOK[id].st[i].name, c + 1)) { p->id = (unsigned char)id; p->st = (unsigned char)i; return 1; }
        return 0;
    }
    return 0;
}
/* d entradas "fluxo:estado" a partir de f; uma entrada que nao existe mais descarta ela e as de cima */
static int rz_fl_stack(FlowPos *stk, char **f, int n, int d) {
    int k = 0;
    for (int i = 0; i < d && i < FL_DEPTH && i < n; i++) { if (!rz_fl_dec(f[i], &stk[k])) break; k++; }
    return k;
}

/* ---- aplicar um evento ao estado (a unica funcao que muda RzState) ---- */
static int rz_name_idx(const char *const *tab, int n, const char *s) { for (int i = 1; i < n; i++) if (!strcmp(tab[i], s)) return i; return 0; }
static void rz_apply(char kind, char **f, int n, long long ts) {
    RzState *s = &g_rz.s;
#define FLD(i) ((i) < n ? f[i] : "")
    s->t_event = ts;
    switch (kind) {
    case 'H': rz_cpy(s->sess, sizeof s->sess, FLD(0)); s->agent_pid = atol(FLD(1)); rz_cpy(s->host, sizeof s->host, FLD(2)); rz_cpy(s->cwd, sizeof s->cwd, FLD(3)); if (n > 4) s->epoch = atoi(f[4]); break;
    case 'U': s->req = atoi(FLD(0)); rz_cpy(s->last_req, sizeof s->last_req, FLD(1)); break;
    case 'G': rz_cpy(s->goal, sizeof s->goal, FLD(0)); break;
    case 'F': {
        int d = atoi(FLD(0)); if (d < 0) d = 0; if (d > FL_DEPTH) d = FL_DEPTH;
        s->depth = rz_fl_stack(s->stk, f + 1, n > 1 ? n - 1 : 0, d);
        s->task = atoi(FLD(1 + d)); s->cycles = atoi(FLD(2 + d)); s->webcalls = atoi(FLD(3 + d)); s->badfetch = atoi(FLD(4 + d));
        break; }
    case 'X':
        if (!strcmp(FLD(0), "path")) rz_cpy(s->path, sizeof s->path, FLD(1));
        else if (!strcmp(FLD(0), "err")) rz_cpy(s->err, sizeof s->err, FLD(1));
        else if (!strcmp(FLD(0), "errfile")) rz_cpy(s->errfile, sizeof s->errfile, FLD(1));
        break;
    case 'B': rz_cpy(s->open_tool, sizeof s->open_tool, FLD(0)); rz_cpy(s->open_subj, sizeof s->open_subj, FLD(1)); s->open_sig = strtoull(FLD(2), NULL, 16); break;
    case 'E': s->open_tool[0] = 0; s->open_subj[0] = 0; s->open_sig = 0; break;
    case 'A': {
        const uint64_t key = strtoull(FLD(0), NULL, 16);
        int at = -1;
        for (int i = 0; i < s->natt; i++) if (s->att[i].key == key) at = i;
        if (at < 0) {
            if (s->natt < RZ_ATT) at = s->natt++;
            else {      /* cheio: sai a mais antiga que nao esta morta; senao a mais antiga */
                for (int i = 0; i < s->natt; i++) if (!s->att[i].dead && (at < 0 || s->att[i].t < s->att[at].t)) at = i;
                if (at < 0) { at = 0; for (int i = 1; i < s->natt; i++) if (s->att[i].t < s->att[at].t) at = i; }
            }
            memset(&s->att[at], 0, sizeof s->att[at]);
        }
        RzAttempt *a = &s->att[at];
        a->key = key; a->call = strtoull(FLD(1), NULL, 16);
        rz_cpy(a->tool, sizeof a->tool, FLD(2)); rz_cpy(a->subj, sizeof a->subj, FLD(3)); rz_cpy(a->cause, sizeof a->cause, FLD(4));
        a->n = atoi(FLD(5)); a->dead = (unsigned char)(atoi(FLD(6)) != 0); rz_cpy(a->fix, sizeof a->fix, FLD(7));
        a->epoch = n > 8 ? atoi(f[8]) : s->epoch; a->t = ts; a->req = s->req;
        break; }
    case 'W': {
        RzEvid e; memset(&e, 0, sizeof e);
        e.kind = (unsigned char)rz_name_idx(RZ_ENAME, 7, FLD(0)); if (!e.kind) break;
        e.key = strtoull(FLD(1), NULL, 16); rz_cpy(e.text, sizeof e.text, FLD(2)); rz_cpy(e.path, sizeof e.path, FLD(3)); e.hash = strtoull(FLD(4), NULL, 16); e.req = s->req;
        if (s->nev < RZ_EVID) s->ev[s->nev++] = e; else { s->ev[s->ev_w] = e; s->ev_w = (s->ev_w + 1) % RZ_EVID; }
        break; }
    case 'J': {
        RzJob j; memset(&j, 0, sizeof j);
        j.id = atoi(FLD(0)); j.pid = atol(FLD(1)); j.start = atoll(FLD(2)); j.agent_pid = atol(FLD(3)); j.t0 = atoll(FLD(4));
        j.status = (unsigned char)rz_name_idx(RZ_JNAME, 7, FLD(5)); j.rc = atoi(FLD(6)); j.log_sz = atol(FLD(7)); j.told = (unsigned char)(atoi(FLD(8)) != 0);
        rz_cpy(j.cmd, sizeof j.cmd, FLD(9)); rz_cpy(j.log, sizeof j.log, FLD(10)); rz_cpy(j.asked, sizeof j.asked, FLD(11));
        if (!j.id || !j.status) break;
        int at = -1;
        for (int i = 0; i < RZ_NJOB; i++) if (s->job[i].id == j.id && s->job[i].agent_pid == j.agent_pid) at = i;
        if (at < 0) for (int i = 0; i < RZ_NJOB && at < 0; i++) if (!s->job[i].id) at = i;
        if (at < 0) { at = 0; for (int i = 1; i < RZ_NJOB; i++) if (s->job[i].t0 < s->job[at].t0) at = i; }
        s->job[at] = j;
        break; }
    case 'P': {
        RzFile p; memset(&p, 0, sizeof p);
        rz_cpy(p.path, sizeof p.path, FLD(0)); p.op = (unsigned char)atoi(FLD(1)); p.hash = strtoull(FLD(2), NULL, 16); p.size = atoll(FLD(3)); p.mtime = atoll(FLD(4));
        p.shown = (unsigned char)(atoi(FLD(5)) != 0); p.req = atoi(FLD(6));
        if (!p.path[0]) break;
        int at = -1;
        for (int i = 0; i < s->nfile; i++) if (!strcmp(s->file[i].path, p.path)) at = i;
        if (!p.op) { if (at >= 0) s->file[at] = s->file[--s->nfile]; break; }      /* op 0: o arquivo saiu do estado */
        if (at < 0) {
            if (s->nfile < RZ_FILE) at = s->nfile++;
            else { at = 0; for (int i = 1; i < s->nfile; i++) if (s->file[i].req < s->file[at].req) at = i; }
        }
        s->file[at] = p;
        break; }
    case 'D': {
        RzDecision d; memset(&d, 0, sizeof d); rz_cpy(d.q, sizeof d.q, FLD(0)); rz_cpy(d.a, sizeof d.a, FLD(1)); d.req = s->req;
        if (s->ndec < RZ_DEC) s->dec[s->ndec++] = d; else { s->dec[s->dec_w] = d; s->dec_w = (s->dec_w + 1) % RZ_DEC; }
        break; }
    case 'Q': s->ask_task = atoi(FLD(0)); rz_cpy(s->ask, sizeof s->ask, FLD(1)); break;
    case 'T': rz_cpy(s->todo, sizeof s->todo, FLD(0)); break;
    case 'M': s->plan_mode = atoi(FLD(0)) != 0; s->worktree = atoi(FLD(1)) != 0; break;
    case 'S': s->nseen = 0; for (int i = 0; i < n && s->nseen < RZ_SEEN; i++) s->seen[s->nseen++] = strtoull(f[i], NULL, 16); break;
    case 'R': s->nring = 0;
        for (int i = 0; i < n && s->nring < RZ_RING; i++) {
            char *e1 = NULL, *e2 = NULL; const uint64_t sg = strtoull(f[i], &e1, 16); if (!e1 || *e1 != ':') continue;
            const uint64_t rs = strtoull(e1 + 1, &e2, 16); if (!e2 || *e2 != ':') continue;
            s->ring[s->nring].sig = sg; s->ring[s->nring].res = rs; s->ring[s->nring].n = atoi(e2 + 1);
            const char *c3 = strchr(e2 + 1, ':'); s->ring[s->nring].req = c3 ? atoi(c3 + 1) : 0; s->nring++;
        }
        break;
    case 'O': {
        const int slot = atoi(FLD(0)); if (slot < 0 || slot >= RZ_WORK) break;
        int d = atoi(FLD(2)); if (d < 0) d = 0; if (d > FL_DEPTH) d = FL_DEPTH;
        RzWork *w = &s->work[slot]; memset(w, 0, sizeof *w);
        rz_cpy(w->goal, sizeof w->goal, FLD(1)); w->depth = rz_fl_stack(w->stk, f + 3, n > 3 ? n - 3 : 0, d); w->t = atoll(FLD(3 + d));
        if (slot >= s->nwork) s->nwork = slot + 1;
        break; }
    case 'C': s->t_clean = ts; break;
    case 'N': s->epoch++; break;
    default: break;      /* tipo desconhecido dentro da versao 1: ignorado */
    }
#undef FLD
}

/* ---- escrever: a linha vai ao arquivo (fflush) ANTES de valer na memoria ---- */
static void rz_fail(void) {
    if (g_rz.f) { fclose(g_rz.f); g_rz.f = NULL; }
    g_rz.readonly = 1;
    if (!g_rz.noted) { g_rz.noted = 1; fprintf(stderr, "%s[work state: could not write %s; the agent goes on without saving it]%s\n", CE(C_DIM), g_rz.file, CE(C_RESET)); }
}
static void rz_checkpoint(void);
static void rz_emit(char kind, int n, const char *const *f) {
    static char buf[RZ_LINE];
    const long long ts = (long long)time(NULL);
    if (g_rz.f && !g_rz.readonly) {
        const size_t l = rz_line(buf, sizeof buf, g_rz.seq + 1, ts, kind, n, f);
        if (!l) return;      /* nao coube numa linha: nao vale nem na memoria (o arquivo e a memoria nunca divergem) */
        g_rz.seq++; g_rz.since_ckpt++;
        if (fwrite(buf, 1, l, g_rz.f) != l || fflush(g_rz.f) != 0) rz_fail();
#ifndef _WIN32
        else if (kind == 'B' && n > 0 && ct_readonly(f[0]) == 0) fsync(fileno(g_rz.f));      /* a chamada que muda coisas e anotada em disco de verdade antes de rodar */
#endif
    }
    char *cp[RZ_MAXF]; char tmp[RZ_LINE]; size_t off = 0; int k = 0;      /* a memoria aplica uma copia: rz_apply pode ler e cortar os campos */
    for (; k < n && k < RZ_MAXF; k++) { const size_t l = strlen(f[k] ? f[k] : "") + 1; if (off + l > sizeof tmp) break; memcpy(tmp + off, f[k] ? f[k] : "", l); cp[k] = tmp + off; off += l; }
    rz_apply(kind, cp, k, ts);
    if (g_rz.f && !g_rz.readonly && ((kind == 'C' && g_rz.since_ckpt >= 50) || g_rz.since_ckpt >= 200)) rz_checkpoint();
}
/* um evento: o tipo e os campos como texto, terminados em NULL */
static void rz_ev(char kind, ...) {
    const char *f[RZ_MAXF]; int n = 0; va_list ap; va_start(ap, kind);
    for (const char *s; n < RZ_MAXF && (s = va_arg(ap, const char *)); ) f[n++] = s;
    va_end(ap);
    rz_emit(kind, n, f);
}

/* ---- ponto de salvamento: o estado como a menor lista de eventos que o reproduz ---- */
static void rz_wla(FILE *fp, long *seq, long long ts, char kind, int n, const char *const *f) {
    static char buf[RZ_LINE];
    const size_t l = rz_line(buf, sizeof buf, ++*seq, ts, kind, n, f);
    if (l) fwrite(buf, 1, l, fp);
}
static void rz_wl(FILE *fp, long *seq, long long ts, char kind, ...) {
    const char *f[RZ_MAXF]; int n = 0; va_list ap; va_start(ap, kind);
    for (const char *s; n < RZ_MAXF && (s = va_arg(ap, const char *)); ) f[n++] = s;
    va_end(ap);
    rz_wla(fp, seq, ts, kind, n, f);
}
/* "fluxo:estado" x d na frente de campos extras: devolve o numero de campos escritos em f a partir de k */
static int rz_stack_fields(const FlowPos *stk, int d, char (*b)[48], const char **f, int k) {
    for (int i = 0; i < d; i++) { rz_fl_enc(&stk[i], b[i], 48); f[k++] = b[i]; }
    return k;
}
static long rz_dump(FILE *fp) {
    const RzState *s = &g_rz.s; long seq = 0;
    const long long ts = s->t_event ? s->t_event : (long long)time(NULL);
    rz_wl(fp, &seq, ts, 'H', s->sess, rz_i(s->agent_pid), s->host, s->cwd, rz_i(s->epoch), NULL);
    if (s->goal[0]) rz_wl(fp, &seq, ts, 'G', s->goal, NULL);
    rz_wl(fp, &seq, ts, 'U', rz_i(s->req), s->last_req, NULL);
    {   const char *f[RZ_MAXF]; char b[FL_DEPTH][48]; int k = 0;
        f[k++] = rz_i(s->depth); k = rz_stack_fields(s->stk, s->depth, b, f, k);
        f[k++] = rz_i(s->task); f[k++] = rz_i(s->cycles); f[k++] = rz_i(s->webcalls); f[k++] = rz_i(s->badfetch);
        rz_wla(fp, &seq, ts, 'F', k, f); }
    if (s->path[0]) rz_wl(fp, &seq, ts, 'X', "path", s->path, NULL);
    if (s->err[0]) rz_wl(fp, &seq, ts, 'X', "err", s->err, NULL);
    if (s->errfile[0]) rz_wl(fp, &seq, ts, 'X', "errfile", s->errfile, NULL);
    if (s->todo[0]) rz_wl(fp, &seq, ts, 'T', s->todo, NULL);
    rz_wl(fp, &seq, ts, 'M', rz_i(s->plan_mode), rz_i(s->worktree), NULL);
    if (s->ask[0]) rz_wl(fp, &seq, ts, 'Q', rz_i(s->ask_task), s->ask, NULL);
    for (int i = 0; i < RZ_NJOB; i++) {
        const RzJob *j = &s->job[i]; if (!j->id) continue;
        rz_wl(fp, &seq, ts, 'J', rz_i(j->id), rz_i(j->pid), rz_i(j->start), rz_i(j->agent_pid), rz_i(j->t0), RZ_JNAME[j->status], rz_i(j->rc), rz_i(j->log_sz), rz_i(j->told), j->cmd, j->log, j->asked, NULL);
    }
    for (int i = 0; i < s->nfile; i++) { const RzFile *p = &s->file[i]; rz_wl(fp, &seq, ts, 'P', p->path, rz_i(p->op), rz_x(p->hash), rz_i(p->size), rz_i(p->mtime), rz_i(p->shown), rz_i(p->req), NULL); }
    for (int i = 0; i < s->natt; i++) { const RzAttempt *a = &s->att[i]; rz_wl(fp, &seq, a->t ? a->t : ts, 'A', rz_x(a->key), rz_x(a->call), a->tool, a->subj, a->cause, rz_i(a->n), rz_i(a->dead), a->fix, rz_i(a->epoch), NULL); }
    for (int i = 0; i < s->nev; i++) { const RzEvid *e = &s->ev[(s->nev == RZ_EVID ? s->ev_w + i : i) % RZ_EVID]; rz_wl(fp, &seq, ts, 'W', RZ_ENAME[e->kind], rz_x(e->key), e->text, e->path, rz_x(e->hash), NULL); }
    for (int i = 0; i < s->ndec; i++) { const RzDecision *d = &s->dec[(s->ndec == RZ_DEC ? s->dec_w + i : i) % RZ_DEC]; rz_wl(fp, &seq, ts, 'D', d->q, d->a, NULL); }
    for (int i = 0; i < s->nwork; i++) {
        const RzWork *w = &s->work[i]; if (!w->goal[0] && !w->depth) continue;
        const char *f[RZ_MAXF]; char b[FL_DEPTH][48]; int k = 0;
        f[k++] = rz_i(i); f[k++] = w->goal; f[k++] = rz_i(w->depth); k = rz_stack_fields(w->stk, w->depth, b, f, k); f[k++] = rz_i(w->t);
        rz_wla(fp, &seq, ts, 'O', k, f);
    }
    if (s->nseen) { const char *f[RZ_SEEN]; for (int i = 0; i < s->nseen; i++) f[i] = rz_x(s->seen[i]); rz_wla(fp, &seq, ts, 'S', s->nseen, f); }
    if (s->nring) {
        const char *f[RZ_RING]; static char rb[RZ_RING][80];
        for (int i = 0; i < s->nring; i++) { snprintf(rb[i], sizeof rb[i], "%llx:%llx:%d:%d", (unsigned long long)s->ring[i].sig, (unsigned long long)s->ring[i].res, s->ring[i].n, s->ring[i].req); f[i] = rb[i]; }
        rz_wla(fp, &seq, ts, 'R', s->nring, f);
    }
    if (s->open_tool[0]) rz_wl(fp, &seq, ts, 'B', s->open_tool, s->open_subj, rz_x(s->open_sig), NULL);
    if (s->t_clean) rz_wl(fp, &seq, s->t_clean, 'C', "0", NULL);
    return seq;
}
static FILE *rz_fopen_append(const char *path) {
#ifndef _WIN32
    const int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    return fd < 0 ? NULL : fdopen(fd, "a");
#else
    return fopen(path, "ab");
#endif
}
static void rz_checkpoint(void) {
    if (!g_rz.file[0] || g_rz.readonly) return;
    char tmp[1120]; snprintf(tmp, sizeof tmp, "%s.tmp", g_rz.file);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) { rz_fail(); return; }
#ifndef _WIN32
    fchmod(fileno(fp), 0600);
#endif
    fprintf(fp, "katana-state\t%d\n", RZ_VERSION);
    const long seq = rz_dump(fp);
    int ok = fflush(fp) == 0 && !ferror(fp);
#ifndef _WIN32
    if (ok) fsync(fileno(fp));
#endif
    ok = fclose(fp) == 0 && ok;
    if (!ok) { remove(tmp); rz_fail(); return; }
    if (g_rz.f) { fclose(g_rz.f); g_rz.f = NULL; }
    if (rename(tmp, g_rz.file) != 0) { remove(tmp); rz_fail(); return; }
    g_rz.f = rz_fopen_append(g_rz.file);
    if (!g_rz.f) { rz_fail(); return; }
    g_rz.seq = seq; g_rz.since_ckpt = 0;
}

/* ---- ler o arquivo: cabecalho, crc, repeticao. Devolve 1 se havia estado. Linha com crc errado encerra a leitura (cauda rasgada) ---- */
static int rz_replay(const char *path) {
    FILE *fp = fopen(path, "rb"); if (!fp) return 0;
    static char ln[RZ_LINE + 64];
    char want[40]; snprintf(want, sizeof want, "katana-state\t%d", RZ_VERSION);
    if (!fgets(ln, sizeof ln, fp) || (ln[strcspn(ln, "\r\n")] = 0, strcmp(ln, want) != 0)) {      /* cabecalho de outra versao: sem migracao, o estado recomeca vazio */
        fclose(fp);
        char bad[1120]; snprintf(bad, sizeof bad, "%s.bad", path); remove(bad);
        if (rename(path, bad) == 0) fprintf(stderr, "%s[work state: %s has another format; set aside as .bad, starting empty]%s\n", CE(C_DIM), path, CE(C_RESET));
        return 0;
    }
    int events = 0;
    while (fgets(ln, sizeof ln, fp)) {
        if (!strchr(ln, '\n') && !feof(fp)) break;      /* linha maior do que o limite: fim da leitura */
        char *f[RZ_MAXF]; long long ts = 0; char kind = 0; long seq = 0;
        const int n = rz_parse(ln, &ts, &kind, f, RZ_MAXF, &seq);
        if (n < 0) break;
        rz_apply(kind, f, n, ts);
        if (seq > g_rz.seq) g_rz.seq = seq;
        events++;
    }
    fclose(fp);
    return events > 0;
}
static void rz_host(char *o, size_t cap) { o[0] = 0;
#ifndef _WIN32
    if (gethostname(o, cap) != 0) o[0] = 0; else o[cap - 1] = 0;
#endif
}
/* cabecalho deste processo: o que mudou em relacao ao que foi gravado fica em g_rz.new_proc, moved e foreign */
static void rz_env_check(void) {
    RzState *s = &g_rz.s;
    char host[64], cwd[1024]; rz_host(host, sizeof host);
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    g_rz.new_proc = s->agent_pid != (long)getpid() || strcmp(s->host, host) != 0;
    g_rz.moved = s->cwd[0] && strcmp(s->cwd, cwd) != 0;
    struct stat st;
    g_rz.foreign = strcmp(s->host, host) != 0 || (s->cwd[0] && stat(s->cwd, &st) != 0);
}
static void rz_header(const char *sess_path) {
    char host[64], cwd[1024]; rz_host(host, sizeof host);
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    const char *b = sess_path ? strrchr(sess_path, '/') : NULL;
    rz_ev('H', b ? b + 1 : (sess_path ? sess_path : ""), rz_i((long long)getpid()), host, cwd, rz_i(g_rz.s.epoch), NULL);
}
/* fecha o arquivo e solta a trava; guarda o estado em memoria (rz_close tambem o zera) */
static void rz_release(void) {
    if (g_rz.file[0] && !g_rz.readonly) rz_checkpoint();
    if (g_rz.f) { fclose(g_rz.f); g_rz.f = NULL; }
#ifndef _WIN32
    if (g_rz.lockfd > 0) { flock(g_rz.lockfd, LOCK_UN); close(g_rz.lockfd); }
#endif
    g_rz.lockfd = 0; g_rz.file[0] = 0; g_rz.readonly = 0; g_rz.seq = 0; g_rz.since_ckpt = 0;
}
RZ_UNUSED static void rz_close(void) { rz_release(); memset(&g_rz, 0, sizeof g_rz); }
/* abre o estado da sessao: trava, le, reescreve como ponto de salvamento e anota o processo atual. adopt: o estado em memoria
 * (de antes de a sessao ter arquivo) passa para o arquivo novo, se ele ainda nao existe. Devolve 1 se havia estado gravado. */
RZ_UNUSED static int rz_open(const char *sess_path, int adopt) {
    if (!rz_on()) return 0;
    char path[1100]; rz_path(sess_path, path, sizeof path);
    RzState keep; if (adopt) keep = g_rz.s;
    rz_release();
    memset(&g_rz, 0, sizeof g_rz);
    if (adopt) g_rz.s = keep;
    if (!path[0]) return 0;      /* sem arquivo: o estado fica so na memoria */
    rz_cpy(g_rz.file, sizeof g_rz.file, path);
#ifndef _WIN32
    char lk[1120]; snprintf(lk, sizeof lk, "%s.lock", path);
    g_rz.lockfd = open(lk, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (g_rz.lockfd > 0 && flock(g_rz.lockfd, LOCK_EX | LOCK_NB) != 0) {
        close(g_rz.lockfd); g_rz.lockfd = 0; g_rz.readonly = 1;
        fprintf(stderr, "%s[work state: this session is open in another window; this one reads the state and writes nothing]%s\n", CE(C_DIM), CE(C_RESET));
    }
#endif
    struct stat st;
    const int exists = stat(path, &st) == 0;
    if (!adopt || exists) { memset(&g_rz.s, 0, sizeof g_rz.s); g_rz.loaded = rz_replay(path); }
    if (g_rz.loaded) rz_env_check();
    if (g_rz.readonly) return g_rz.loaded;
    rz_checkpoint();      /* reescreve o arquivo limpo (apara a cauda rasgada) e abre para anexar */
    rz_header(sess_path);
    return g_rz.loaded;
}
