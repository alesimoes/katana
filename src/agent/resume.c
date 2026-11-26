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
    int probe_pending;                                                        /* KATANA_RESUME_PROBE: a primeira chamada depois do bloco recebe uma sonda */
    int sc_calls, dead_hit; uint64_t subj_noted[8]; int nsubj_noted;                    /* nota passiva: chamadas contadas; a chamada casou tentativa morta; (epoca, assunto) ja avisados */
    int task_ended;                                                           /* a reconciliacao achou tarefa terminada que o modelo nao soube */
    int enter_reason;                                                         /* a ultima reentrada (RZR_*): o texto do desvio de tentativa morta diz de quando ela e */
    long long t_saved; int grace_age;                                         /* quando o estado foi gravado pela ultima vez; pedidos desde a carga */
    char old_cwd[1024];                                                       /* a pasta gravada antes de este processo se anotar */
    int more_delta;                                                           /* mudancas alem das seis frases */
    int plan_done;                                                            /* passos do plano marcados na ultima conta */
    char req_text[400]; int goal_fid;                                         /* o pedido em curso (limpo) e o fluxo de baixo da pilha quando o objetivo foi gravado */
} g_rz;
#define RZ_UNUSED __attribute__((unused))
static int g_rz_keep;      /* 1 quando o estado desta sessao esta em arquivo: os arquivos de saida das tarefas ficam (jobs.c) para o texto do estado poder apontar para eles */
static int rz_keep_logs(void) { return g_rz_keep; }

static int rz_on(void) { const char *e = getenv("KATANA_RESUME"); return !(e && !strcmp(e, "0")); }
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
static void rz_clean(char *d, size_t cap, const char *s, int multi) {
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

/* g_fl guarda o que a conversa ja viu (seen) e as chamadas recentes (ring): vao ao estado so no ponto de salvamento, os mais novos */
static void rz_pull_ring(void) {
    RzState *s = &g_rz.s;
    const int sn = g_fl.seen_n < 256 ? g_fl.seen_n : 256, ss = g_fl.seen_n < 256 ? 0 : (g_fl.seen_w & 255);
    const int st = sn < RZ_SEEN ? sn : RZ_SEEN;
    s->nseen = 0;
    for (int i = sn - st; i < sn; i++) s->seen[s->nseen++] = g_fl.seen[(ss + i) % 256];
    const int rn = g_fl.ring_n, rs = g_fl.ring_n < FL_RING ? 0 : (g_fl.ring_w % FL_RING);
    const int rt = rn < RZ_RING ? rn : RZ_RING;
    s->nring = 0;
    for (int i = rn - rt; i < rn; i++) { const int k = (rs + i) % FL_RING; s->ring[s->nring].sig = g_fl.ring[k].sig; s->ring[s->nring].res = g_fl.ring[k].res; s->ring[s->nring].n = g_fl.ring[k].n; s->ring[s->nring].req = g_fl.ring[k].req; s->nring++; }
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
    if (g_ct_depth <= 1) rz_pull_ring();
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
    rz_cpy(g_rz.old_cwd, sizeof g_rz.old_cwd, s->cwd);
    g_rz.t_saved = s->t_event;
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
static void rz_close(void) { rz_release(); memset(&g_rz, 0, sizeof g_rz); }
static void rz_restore_flow(void);
static void rz_restore_misc(void);
static void rz_reconcile(void);
/* abre o estado da sessao: trava, le, reescreve como ponto de salvamento e anota o processo atual. adopt: o estado em memoria
 * (de antes de a sessao ter arquivo) passa para o arquivo novo, se ele ainda nao existe. Devolve 1 se havia estado gravado. */
static int rz_open(const char *sess_path, int adopt) {
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
    if (!g_rz.readonly) {
        rz_checkpoint();      /* reescreve o arquivo limpo (apara a cauda rasgada) e abre para anexar */
        rz_header(sess_path);
    }
    if (g_rz.file[0] && !g_rz.readonly) g_rz_keep = 1;
    if (g_rz.loaded) { g_rz.grace = 1; g_rz.grace_age = 0; rz_ev('N', "session", "0", NULL); g_rz.enter_reason = RZR_SESSION; rz_restore_flow(); rz_restore_misc(); rz_reconcile(); }      /* graca: o primeiro pedido depois da carga nao encerra o trabalho aberto so por nao ser uma palavra de "continue" */
    return g_rz.loaded;
}
/* a sessao foi apagada (sess_purge): o estado dela vai junto */
static void rz_remove_files(const char *sess_path) {
    char p[1100], q[1130]; rz_path(sess_path, p, sizeof p);
    if (!p[0]) return;
    remove(p); snprintf(q, sizeof q, "%s.lock", p); remove(q); snprintf(q, sizeof q, "%s.bad", p); remove(q); snprintf(q, sizeof q, "%s.tmp", p); remove(q);
}

/* ---------- espelho do fluxo, objetivo e pedido ---------- */
static void rz_sync_all(void);
/* g_fl -> estado: so o que mudou vira evento (F, X, G) */
static void rz_sync_flow(void) {
    if (!rz_on() || g_ct_depth > 1) return;
    RzState *s = &g_rz.s;
    int diff = s->depth != g_fl.depth || s->task != g_fl.task || s->cycles != g_fl.cycles || s->webcalls != g_fl.webcalls || s->badfetch != g_fl.badfetch;
    for (int i = 0; !diff && i < g_fl.depth; i++) diff = s->stk[i].id != g_fl.stk[i].id || s->stk[i].st != g_fl.stk[i].st;
    if (diff) {
        const char *f[RZ_MAXF]; char b[FL_DEPTH][48]; int k = 0;
        f[k++] = rz_i(g_fl.depth); k = rz_stack_fields(g_fl.stk, g_fl.depth, b, f, k);
        f[k++] = rz_i(g_fl.task); f[k++] = rz_i(g_fl.cycles); f[k++] = rz_i(g_fl.webcalls); f[k++] = rz_i(g_fl.badfetch);
        rz_emit('F', k, f);
    }
    char b1[400];
    rz_clean(b1, sizeof s->path, g_fl.path, 0); if (strcmp(b1, s->path)) rz_ev('X', "path", b1, NULL);
    rz_clean(b1, sizeof s->err, g_fl.err, 0); if (strcmp(b1, s->err)) rz_ev('X', "err", b1, NULL);
    rz_clean(b1, sizeof s->errfile, g_fl.errfile, 0); if (strcmp(b1, s->errfile)) rz_ev('X', "errfile", b1, NULL);
    /* objetivo: o pedido que abriu o fluxo de baixo da pilha; troca so quando o pedido cai em OUTRO fluxo */
    const int bot = g_fl.depth ? g_fl.stk[0].id : 0;
    if (!bot) g_rz.goal_fid = 0;
    else if (bot != g_rz.goal_fid && g_rz.req_text[0]) { g_rz.goal_fid = bot; rz_ev('G', g_rz.req_text, NULL); }
}
/* estado -> g_fl, pelos nomes (a pilha ja veio validada de rz_apply) */
static void rz_restore_flow(void) {
    const RzState *s = &g_rz.s;
    g_fl.depth = s->depth;
    for (int i = 0; i < s->depth; i++) g_fl.stk[i] = s->stk[i];
    g_fl.task = s->task; g_fl.cycles = s->cycles; g_fl.webcalls = s->webcalls; g_fl.badfetch = s->badfetch;
    rz_cpy(g_fl.path, sizeof g_fl.path, s->path); rz_cpy(g_fl.err, sizeof g_fl.err, s->err); rz_cpy(g_fl.errfile, sizeof g_fl.errfile, s->errfile);
    g_fl.req = s->req;
    g_fl.seen_n = s->nseen; g_fl.seen_w = 0;
    for (int i = 0; i < s->nseen; i++) g_fl.seen[i] = s->seen[i];
    g_fl.ring_n = s->nring; g_fl.ring_w = 0;
    for (int i = 0; i < s->nring; i++) { g_fl.ring[i].sig = s->ring[i].sig; g_fl.ring[i].res = s->ring[i].res; g_fl.ring[i].n = s->ring[i].n; g_fl.ring[i].req = s->ring[i].req; }
    g_fl.emitted_st = -1;
    g_rz.goal_fid = s->depth ? s->stk[0].id : 0;
    if (fl_id() == FL_CODE) fl_find_verify();
    flow_grammar_mask();
}
static void rz_sync_all(void);
static int rz_has_work(void);
static int rz_idle_min(void);
static void rz_score_flush(int answered);
/* chamado antes de flow_request: anota o pedido (U) */
static void rz_request_begin(const char *ureq) {
    if (!rz_on() || g_ct_depth > 1) return;
    if (!g_rz.s.agent_pid) rz_header(g_sess_path);
    rz_score_flush(1);      /* o pedido anterior acabou antes da terceira chamada: respondeu */
    if (g_rz.grace && g_rz.grace_age++ >= 1) g_rz.grace = 0;      /* a graca vale para um pedido so */
    rz_clean(g_rz.req_text, sizeof g_rz.req_text, ureq, 0);
    char b[240]; rz_clean(b, sizeof b, ureq, 0);
    rz_ev('U', rz_i(g_fl.req + 1), b, NULL);
    rz_reconcile();
    rz_sync_all();
    if (!g_rz.reason) {      /* reentrada dentro do processo: tarefa que acabou, silencio longo, ou algo que mudou no mundo */
        const long long now = (long long)time(NULL);
        if (g_rz.task_ended) g_rz.reason = RZR_TASK;
        else if (g_rz.s.t_clean && now - g_rz.s.t_clean > (long long)rz_idle_min() * 60 && rz_has_work()) g_rz.reason = RZR_IDLE;
        else if (g_rz.ndelta > 0) g_rz.reason = RZR_IDLE;
    }
    g_rz.task_ended = 0;
}
/* a nota passiva da orientacao: as tres chamadas depois do bloco, lidas do comportamento do modelo (sem juiz e sem sonda) */
static void rz_score_flush(int answered) {
    if (g_rz.score_left <= 0) return;
    fl_log("resume\tscore\t%s\tcalls=%d\ton_flow=%d\tdead=%d\tdup=%d\tanswered=%d", RZ_RNAME[g_rz.sc_reason], g_rz.sc_calls, g_rz.sc_on, g_rz.sc_dead, g_rz.sc_dup, answered);
    g_rz.score_left = 0; g_rz.sc_calls = g_rz.sc_on = g_rz.sc_dead = g_rz.sc_dup = 0;
}
/* o pedido terminou numa interrupcao do usuario: o proximo pedido recebe o bloco curto */
static void rz_interrupted(void) { if (rz_on() && g_ct_depth <= 1 && !g_rz.reason) g_rz.reason = RZR_INTERRUPT; }

/* ---------- as chamadas: B antes, E depois; tentativas falhas, evidencia ganha, arquivos, decisoes (docs/flow-resume.md secao 5) ---------- */
/* valor de uma chave em JSON (texto simples, sem entrar em objetos aninhados com a mesma chave; o primeiro valor vale) */
static void rz_jget(const char *js, const char *end, const char *key, char *o, size_t cap) {
    o[0] = 0;
    char k[48]; snprintf(k, sizeof k, "\"%s\"", key);
    const size_t kl = strlen(k);
    for (const char *p = js; p + kl < end; p++) {
        if (strncmp(p, k, kl)) continue;
        const char *q = p + kl; while (q < end && (*q == ' ' || *q == ':')) q++;
        if (q >= end || *q != '"') continue;
        q++;
        char tmp[600]; size_t n = 0;
        while (q < end && *q != '"' && n + 2 < sizeof tmp) { if (*q == '\\' && q + 1 < end) { q++; tmp[n++] = *q == 'n' ? ' ' : *q; } else tmp[n++] = *q; q++; }
        tmp[n] = 0;
        rz_clean(o, cap, tmp, 0);
        return;
    }
}
/* o assunto da chamada para o estado: o comando, o arquivo, o endereco, a consulta; SendInput nunca guarda o texto (pode ser senha) */
static void rz_subject(const char *name, const char *js, const char *end, char *o, size_t cap) {
    o[0] = 0;
    if (!strcmp(name, "SendInput")) { char id[16]; rz_jget(js, end, "task_id", id, sizeof id); snprintf(o, cap, "task %s", id[0] ? id : "?"); return; }
    static const char *const keys[] = {"command", "file_path", "notebook_path", "path", "url", "pattern", "query", "prompt", "description", "subject", NULL};
    for (int i = 0; keys[i] && !o[0]; i++) rz_jget(js, end, keys[i], o, cap);
}
static void rz_abs(const char *p, char *o, size_t cap) {      /* o pode ser o mesmo buffer de p */
    char tmp[1600], cwd[1024];
    if (p[0] == '/' || !p[0] || !getcwd(cwd, sizeof cwd)) snprintf(tmp, sizeof tmp, "%s", p);
    else snprintf(tmp, sizeof tmp, "%s/%s", cwd, p);
    rz_cpy(o, cap, tmp);
}
/* hash do conteudo de um arquivo (ate 8 MB; acima disso so tamanho e data); 0 se nao existe */
static int rz_file_info(const char *path, uint64_t *hash, long long *size, long long *mtime) {
    struct stat st; *hash = 0; *size = 0; *mtime = 0;
    if (stat(path, &st) != 0) return 0;
    *size = (long long)st.st_size; *mtime = (long long)st.st_mtime;
    if (st.st_size > 8 * 1024 * 1024 || !S_ISREG(st.st_mode)) return 1;
    FILE *f = fopen(path, "rb"); if (!f) return 1;
    char b[8192]; size_t n; uint64_t h = RZ_FNV;
    while ((n = fread(b, 1, sizeof b, f)) > 0) h = fl_hash(b, n, h);
    fclose(f);
    *hash = h;
    return 1;
}
/* arquivo tocado ou produzido: P com o estado de agora */
static void rz_file_note(const char *path, int op, int shown) {
    char ap[400]; rz_clean(ap, sizeof ap, path, 0); rz_abs(ap, ap, sizeof ap);
    if (!ap[0]) return;
    uint64_t h; long long sz, mt; rz_file_info(ap, &h, &sz, &mt);
    rz_ev('P', ap, rz_i(op), rz_x(h), rz_i(sz), rz_i(mt), rz_i(shown), rz_i(g_rz.s.req), NULL);
}
static void rz_evidence(int kind, const char *text, const char *path) {
    char t[160]; rz_clean(t, sizeof t, text, 0);
    char p[400] = ""; uint64_t h = 0; long long sz, mt;
    if (path && path[0]) { rz_clean(p, sizeof p, path, 0); rz_abs(p, p, sizeof p); rz_file_info(p, &h, &sz, &mt); }
    rz_ev('W', RZ_ENAME[kind], rz_x(fl_hash(t, strlen(t), RZ_FNV)), t, p, rz_x(h), NULL);
}
static int rz_secret_q(const char *q) {
    static const char *const w[] = {"password", "senha", "token", "secret", "passphrase", "key", "chave", "pin", NULL};
    char lo[200]; size_t n = 0; for (; q[n] && n < sizeof lo - 1; n++) lo[n] = (char)tolower((unsigned char)q[n]); lo[n] = 0;
    for (int i = 0; w[i]; i++) if (strstr(lo, w[i])) return 1;
    return 0;
}
/* antes de rodar: a chamada abre (B). Escrita na frente do efeito */
static void rz_before(const char *name, const char *js, const char *end) {
    if (!rz_on() || g_ct_depth > 1) return;
    char subj[120]; rz_subject(name, js, end, subj, sizeof subj);
    rz_ev('B', name, subj, rz_x(g_fl.cur_sig), NULL);
    if (g_rz.score_left > 0) {      /* nota passiva: esta chamada esta no fluxo? repete tentativa morta? duplica tarefa que roda? */
        int on = 1; const FlowPos *t = fl_top();
        if (t && strcmp(name, "ToolSearch")) { const char *ex = FL_BOOK[t->id].st[t->st].expect; if (ex) on = ex[0] && fl_list_has(ex, name); }
        g_rz.sc_calls++; g_rz.sc_on += on; g_rz.sc_dead += g_rz.dead_hit; g_rz.sc_dup += strstr(g_fl.dev, "ALREADY running") != NULL;
        if (--g_rz.score_left == 0) { g_rz.score_left = 1; rz_score_flush(0); }
    }
}
/* depois de rodar: E, tentativa falha ou corrigida, evidencia, arquivo, decisao */
static void rz_after_body(const char *name, const char *js, const char *end, const char *result, int failed, int ev, int gain) {
    if (!rz_on() || g_ct_depth > 1) return;
    RzState *s = &g_rz.s;
    rz_ev('E', name, failed ? "0" : "1", rz_x(g_fl.cur_sig), rz_x(fl_hash(result, strlen(result), RZ_FNV)), FE_NAME[ev], gain ? "1" : "0", NULL);
    if (ev == FE_NONE) return;
    char subj[80]; fm_subject(name, js, end, subj, sizeof subj);
    if (!strcmp(name, "SendInput")) rz_subject(name, js, end, subj, sizeof subj);
    rz_clean(subj, sizeof subj, subj, 0);
    if (failed) {
        char sig[120]; fm_errsig(result, sig, sizeof sig); rz_clean(sig, sizeof sig, sig, 0);
        uint64_t key = fl_hash(name, strlen(name), RZ_FNV); key = fl_hash(subj, strlen(subj), key); key = fl_hash(sig, strlen(sig), key);
        int n = 1; const char *fix = "";
        for (int i = 0; i < s->natt; i++) if (s->att[i].key == key) { n = s->att[i].n + 1; fix = s->att[i].fix; }
        char fx[120]; rz_cpy(fx, sizeof fx, fix);
        rz_ev('A', rz_x(key), rz_x(g_fl.cur_sig), name, subj, sig, rz_i(n), rz_i(n >= 2), fx, rz_i(s->epoch), NULL);
    } else {
        for (int i = 0; i < s->natt; i++) {
            const RzAttempt a = s->att[i];
            if (strcmp(a.tool, name) || strcmp(a.subj, subj) || !(a.dead || !a.fix[0])) continue;
            char fx[120];
            if (!strcmp(name, "Bash") || !strcmp(name, "PowerShell")) rz_jget(js, end, "command", fx, sizeof fx);
            else { char sh[100]; fm_shape(js, sh, sizeof sh); snprintf(fx, sizeof fx, "%.12s(%.100s)", name, sh); }
            rz_ev('A', rz_x(a.key), rz_x(a.call), a.tool, a.subj, a.cause, rz_i(a.n), "0", fx, rz_i(a.epoch), NULL);
        }
    }
    if (failed) return;
    char t[200], v[400];
    if (ev == FE_ASKED) {
        rz_jget(js, end, "question", v, sizeof v);
        const char *a = strstr(result, "the user answered: ");
        char ans[160]; rz_clean(ans, sizeof ans, a ? a + 19 : "", 0);
        if (rz_secret_q(v)) rz_cpy(ans, sizeof ans, "(given)");
        rz_ev('D', v, ans, NULL);
    }
    if (!strcmp(name, "TodoWrite")) {      /* passo do plano fechado: a evidencia e o texto do passo */
        int td = 0; todo_count(&td);
        if (td > g_rz.plan_done) {
            int k = 0; v[0] = 0;
            for (const char *l = g_todo; *l; ) {
                if (!strncmp(l, "[x]", 3) && ++k == td) { const char *e = strchr(l, '\n'); snprintf(v, sizeof v, "%.*s", (int)(e ? e - l - 3 : (int)strlen(l) - 3), l + 3); break; }
                l = strchr(l, '\n'); if (!l) break; l++;
            }
            char tp[120]; rz_clean(tp, sizeof tp, v, 0);
            snprintf(t, sizeof t, "plan step %d done: %s", td, tp); rz_evidence(RZE_PLAN, t, NULL);
        }
        g_rz.plan_done = td;
    }
    if (ev == FE_WRITE) { rz_jget(js, end, "file_path", v, sizeof v); if (!v[0]) rz_jget(js, end, "notebook_path", v, sizeof v); if (v[0]) rz_file_note(v, RZF_WRITE, 0); }
    else if (ev == FE_RENDER_OK) {
        const char *p = strstr(result, ": "); const char *e = p ? strstr(p, " (") : NULL;
        if (p && e && e > p + 2) { snprintf(v, sizeof v, "%.*s", (int)(e - p - 2), p + 2); rz_file_note(v, RZF_MADE, 1); }
    } else if (ev == FE_IMG_CMD) { if (g_fl.path[0]) rz_file_note(g_fl.path, RZF_MADE, 0); }
    else if (ev == FE_SENT_FILE) {
        const char *a = strstr(js, "\"files\""); const char *b = a ? strchr(a, '[') : NULL;
        for (const char *q = b ? b + 1 : NULL; q && q < end && *q != ']'; ) {
            if (*q != '"') { q++; continue; }
            const char *e = ++q; while (e < end && *e != '"') e++;
            snprintf(v, sizeof v, "%.*s", (int)(e - q < 399 ? e - q : 399), q); rz_file_note(v, RZF_MADE, 1);
            q = e < end ? e + 1 : end;
        }
    }
    if (!gain) return;
    switch (ev) {
    case FE_READ:
        rz_jget(js, end, "file_path", v, sizeof v);
        if (v[0]) { snprintf(t, sizeof t, "read %s", v); rz_evidence(RZE_READ, t, v); break; }
        rz_jget(js, end, "pattern", v, sizeof v); if (!v[0]) rz_jget(js, end, "query", v, sizeof v);
        snprintf(t, sizeof t, v[0] ? "searched %s" : "looked up with %s", v[0] ? v : name); rz_evidence(RZE_READ, t, NULL);
        break;
    case FE_CMD_OK: rz_jget(js, end, "command", v, sizeof v); snprintf(t, sizeof t, "`%.120s` passed", v); rz_evidence(RZE_CMD, t, NULL); break;
    case FE_FETCH_OK: rz_jget(js, end, "url", v, sizeof v); snprintf(t, sizeof t, "read %s", v); rz_evidence(RZE_WEB, t, NULL); break;
    case FE_SEARCH_OK: rz_jget(js, end, "query", v, sizeof v); snprintf(t, sizeof t, "searched %s", v); rz_evidence(RZE_WEB, t, NULL); break;
    case FE_SUB_OK: rz_jget(js, end, "prompt", v, sizeof v); snprintf(t, sizeof t, "sub-agent answered: %s", v); rz_evidence(RZE_SUB, t, NULL); break;
    default: break;
    }
}

static void rz_after(const char *name, const char *js, const char *end, const char *result, int failed, int ev, int gain) {
    rz_after_body(name, js, end, result, failed, ev, gain);
    rz_sync_all();
}
/* ---------- espelhos das tarefas, do plano e dos modos: comparados depois de cada resultado, sem gancho dentro de bash.c, jobs.c ou tasks.c ---------- */
/* inicio de um processo (campo 22 de /proc/PID/stat): com o pid, identifica o processo mesmo que o numero seja reusado */
static long long rz_proc_start(long pid) {
#ifdef __linux__
    char p[64], b[1024]; snprintf(p, sizeof p, "/proc/%ld/stat", pid);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    const size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    char *c = strrchr(b, ')'); if (!c || !c[1]) return 0;
    c += 2;
    int field = 3;
    while (*c && field < 22) { c = strchr(c, ' '); if (!c) return 0; c++; field++; }
    return c ? atoll(c) : 0;
#else
    (void)pid; return 0;
#endif
}
static void rz_sync_jobs(void) {
#ifndef _WIN32
    RzState *s = &g_rz.s;
    const char *waiting = NULL; int wid = 0;
    for (int i = 0; i < JOB_N; i++) {
        Job *j = &g_jobs[i]; if (!j->id) continue;
        job_poll(j);
        int st = j->done ? ((j->rc == 137 || j->rc == 143) ? RZJ_STOPPED : RZJ_DONE) : (j->asked && !j->typed ? RZJ_WAIT : RZJ_RUN);
        if (st == RZJ_WAIT && !waiting) { waiting = j->asked_label; wid = j->id; }
        RzJob *e = NULL;
        for (int k = 0; k < RZ_NJOB; k++) if (s->job[k].id == j->id && s->job[k].agent_pid == (long)getpid()) e = &s->job[k];
        if (e && e->status == RZJ_DETACHED && st == RZJ_RUN) st = RZJ_DETACHED;      /* tarefa readotada depois de recarregar: vive, sem terminal */
        struct stat sb; const long sz = stat(j->log, &sb) == 0 ? (long)sb.st_size : 0;
        char asked[240]; rz_clean(asked, sizeof asked, st == RZJ_WAIT ? j->asked_label : "", 0);
        const int rc = j->done ? j->rc : 0;
        int told = e ? e->told : 0;
        if (j->done && g_fl.task == j->id) { const int ti = fl_in(FL_TERMINAL); if (!ti || g_fl.stk[ti - 1].st == 2) told = 1; }      /* a tarefa acompanhada que acabou: o fluxo avisa pela linha next quando ja a notou (FE_TASK_ENDED) */
        if (e && e->status == st && e->rc == rc && !strcmp(e->asked, asked) && e->told == told && labs(sz - e->log_sz) < 2048) continue;
        char cmd[200], lg[1300]; rz_clean(cmd, sizeof cmd, j->cmd, 0); rz_clean(lg, sizeof lg, j->log, 0);
        const long long start = e && e->start ? e->start : rz_proc_start((long)j->pid);
        rz_ev('J', rz_i(j->id), rz_i((long long)j->pid), rz_i(start), rz_i((long long)getpid()), rz_i((long long)j->t0), RZ_JNAME[st], rz_i(rc), rz_i(sz), rz_i(told), cmd, lg, asked, NULL);
    }
    char q[240]; rz_clean(q, sizeof q, waiting ? waiting : "", 0);
    if (strcmp(q, s->ask) || (waiting && s->ask_task != wid)) rz_ev('Q', rz_i(waiting ? wid : 0), q, NULL);
#endif
}
static void rz_sync_all(void) {
    if (!rz_on() || g_ct_depth > 1) return;
    rz_sync_jobs();
    char *t = (char *)malloc(sizeof g_rz.s.todo);
    if (t) { rz_clean(t, sizeof g_rz.s.todo, g_todo, 1); if (strcmp(t, g_rz.s.todo)) rz_ev('T', t, NULL); free(t); }
    if (g_rz.s.plan_mode != (g_plan_mode != 0) || g_rz.s.worktree != (g_wt_active != 0)) rz_ev('M', rz_i(g_plan_mode != 0), rz_i(g_wt_active != 0), NULL);
}
/* plano e modo plano voltam com a sessao (o estado e a verdade da sessao: o plano de outra conversa nao fica) */
static void rz_restore_misc(void) {
    const RzState *s = &g_rz.s;
    rz_cpy(g_todo, sizeof g_todo, s->todo);
    g_todo_touched = 0; g_rz.plan_done = 0; { int td = 0; todo_count(&td); g_rz.plan_done = td; }
    ct_task_sync(); todo_ui();
    if (g_plan_mode != s->plan_mode) { g_plan_mode = s->plan_mode; plan_ui(); }
}
/* arquivos de saida de tarefas mais velhos do que o limite das sessoes saem junto (sess_purge) */
static void rz_purge_jobs(double days) {
#ifndef _WIN32
    char h[1024], d[1100]; agent_home(h, sizeof h); snprintf(d, sizeof d, "%s/.katana/jobs", h);
    DIR *dir = opendir(d); if (!dir) return;
    const time_t now = time(NULL);
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (de->d_name[0] == '.') continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", d, de->d_name);
        struct stat st; if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        int live = 0; for (int i = 0; i < JOB_N; i++) if (g_jobs[i].id && !strcmp(g_jobs[i].log, p)) live = 1;
        if (!live && (double)(now - st.st_mtime) > days * 86400.0) remove(p);
    }
    closedir(dir);
#else
    (void)days;
#endif
}

/* ---------- reconciliacao: o que se leu de volta e hipotese; confere com o mundo, sem o modelo (docs/flow-resume.md secao 6) ---------- */
#define RZ_ND 32
typedef struct { int pr; char t[400]; } RzDelta;
static void rz_dpush(RzDelta *d, int *nd, int pr, const char *fmt, ...) {
    if (*nd >= RZ_ND) return;
    va_list ap; va_start(ap, fmt); vsnprintf(d[*nd].t, sizeof d[*nd].t, fmt, ap); va_end(ap);
    d[*nd].pr = pr; (*nd)++;
}
/* uma tarefa do estado esta viva? outro processo: so com o inicio do processo conferido (sem /proc, nunca) */
static int rz_alive(long pid, long long start, int strict) {
#ifndef _WIN32
    if (pid <= 1) return 0;
    if (kill((pid_t)pid, 0) != 0 && errno != EPERM) return 0;
    if (!strict) return 1;
    const long long now = rz_proc_start(pid);
    return start && now && start == now;
#else
    (void)pid; (void)start; (void)strict; return 0;
#endif
}
static void rz_job_put(const RzJob *e, int status, int rc, int told, long sz) {
    rz_ev('J', rz_i(e->id), rz_i(e->pid), rz_i(e->start), rz_i(e->agent_pid), rz_i(e->t0), RZ_JNAME[status], rz_i(rc), rz_i(sz), rz_i(told), e->cmd, e->log, e->asked, NULL);
}
/* tira um fluxo da pilha onde ele estiver (o estado e o espelho sao ajustados por rz_sync_flow) */
static void rz_flow_drop(int id) {
    const int at = fl_in(id); if (!at) return;
    fl_log("end\t%s\t-\tresume", FL_BOOK[id].name);
    memmove(g_fl.stk + at - 1, g_fl.stk + at, sizeof g_fl.stk[0] * (size_t)(g_fl.depth - at));
    g_fl.depth--; g_fl.emitted_st = -1;
}
static void rz_reconcile(void) {
    if (!rz_on() || g_ct_depth > 1) return;
    RzState *s = &g_rz.s;
    RzDelta d[RZ_ND]; int nd = 0;
    char cwd[1024]; if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    const int foreign = g_rz.foreign;
    if (g_rz.moved && !foreign) rz_dpush(d, &nd, 4, "the working folder was %.100s when this work stopped; it is %.100s now", g_rz.old_cwd, cwd);
    if (foreign) rz_dpush(d, &nd, 0, "this work was saved on another machine or folder: file and task state below is not verified");
    /* chamada iniciada e sem resultado: o katana parou no meio dela */
    if (s->open_tool[0]) {
        if (g_rz.new_proc) {
            rz_dpush(d, &nd, 0, "the call %.24s %.100s was started and never returned: katana stopped during it. Its effect is unknown; check before you repeat it", s->open_tool, s->open_subj);
            if (ct_readonly(s->open_tool) == 0 && s->open_subj[0] && !foreign && (s->open_subj[0] == '/' || access(s->open_subj, F_OK) == 0)) {
                int known = 0; char ap[400]; rz_abs(s->open_subj, ap, sizeof ap);
                for (int i = 0; i < s->nfile; i++) if (!strcmp(s->file[i].path, ap)) known = 1;
                if (!known) rz_file_note(s->open_subj, RZF_WRITE, 0);      /* o arquivo passa a ser conferido como os outros */
            }
        }
        rz_ev('E', s->open_tool, "0", "0", "0", "none", "0", NULL);
    }
#ifndef _WIN32
    /* tarefas */
    if (!foreign) rz_sync_jobs();      /* o estado e a tabela viva dizem a mesma coisa antes da conferencia */
    for (int k = 0; k < RZ_NJOB; k++) {
        RzJob e = s->job[k];
        if (!e.id) continue;
        const int mine = e.agent_pid == (long)getpid();
        Job *live = NULL; for (int i = 0; i < JOB_N; i++) if (g_jobs[i].id == e.id && mine) live = &g_jobs[i];
        if (mine && e.id > g_job_seq) g_job_seq = e.id;      /* um numero que o modelo tem no contexto nao volta a ser usado */
        else if (!mine && e.id > g_job_seq) g_job_seq = e.id;
        const int running = e.status == RZJ_RUN || e.status == RZJ_WAIT || e.status == RZJ_DETACHED;
        struct stat sb; const int has_log = e.log[0] && stat(e.log, &sb) == 0;
        const long now_sz = has_log ? (long)sb.st_size : 0;
        if (running && foreign) { rz_job_put(&e, RZJ_LOST, 0, 1, e.log_sz); continue; }
        if (running && !live) {
            int st = 0, rc = 0;
            if (mine) {      /* o mesmo processo depois de recarregar (execv): os comandos continuam filhos deste processo */
                int ws = 0; const pid_t w = waitpid((pid_t)e.pid, &ws, WNOHANG);
                if (w == (pid_t)e.pid) { st = RZJ_DONE; rc = WIFEXITED(ws) ? WEXITSTATUS(ws) : 128 + (WIFSIGNALED(ws) ? WTERMSIG(ws) : 0); }
                else if (rz_alive(e.pid, e.start, 0)) {
                    Job *j = NULL; for (int i = 0; i < JOB_N && !j; i++) if (!g_jobs[i].id) j = &g_jobs[i];
                    if (j) {      /* readotada: Read, Monitor e TaskStop voltam a funcionar; a resposta ao terminal nao */
                        memset(j, 0, sizeof *j); j->id = e.id; j->pid = (pid_t)e.pid; j->t0 = (time_t)e.t0;
                        snprintf(j->log, sizeof j->log, "%s", e.log); snprintf(j->cmd, sizeof j->cmd, "%s", e.cmd);
                        if (!g_job_exit) { atexit(job_all_kill); g_job_exit = 1; }
                        rz_dpush(d, &nd, 1, "task %d (`%.60s`) is still running but its terminal was lost: it can be read and stopped, not answered", e.id, e.cmd);
                        rz_job_put(&e, RZJ_DETACHED, 0, 1, now_sz);
                        continue;
                    }
                    st = RZJ_LOST;
                } else st = RZJ_LOST;
            } else if (rz_alive(e.pid, e.start, 1)) {
                rz_dpush(d, &nd, 1, "task %d (`%.60s`) kept running when katana restarted (process %ld); its output is in %.100s", e.id, e.cmd, e.pid, e.log);
                rz_job_put(&e, RZJ_DETACHED, 0, 1, now_sz);
                continue;
            } else st = RZJ_LOST;
            if (st == RZJ_LOST) {
                if (has_log) rz_dpush(d, &nd, 1, "task %d (`%.60s`) ended when katana restarted; exit code unknown; its output (%ld new bytes) is in %.100s", e.id, e.cmd, now_sz > e.log_sz ? now_sz - e.log_sz : 0L, e.log);
                else rz_dpush(d, &nd, 1, "task %d (`%.60s`) ended when katana restarted; exit code unknown; its output file is gone", e.id, e.cmd);
            } else {
                if (has_log) rz_dpush(d, &nd, 1, "task %d (`%.60s`) ended while katana was reloading with exit code %d; its output (%ld new bytes) is in %.100s", e.id, e.cmd, rc, now_sz > e.log_sz ? now_sz - e.log_sz : 0L, e.log);
                else rz_dpush(d, &nd, 1, "task %d (`%.60s`) ended while katana was reloading with exit code %d; its output file is gone", e.id, e.cmd, rc);
            }
            rz_job_put(&e, st, rc, 1, now_sz);
        } else if (!running && !e.told && e.status != RZJ_DETACHED) {      /* acabou e o modelo nao soube: diz uma vez */
            if (e.status == RZJ_LOST) rz_dpush(d, &nd, 1, "task %d (`%.60s`) ended; exit code unknown%s%.100s", e.id, e.cmd, has_log ? "; its output is in " : "", has_log ? e.log : "");
            else rz_dpush(d, &nd, 1, "task %d (`%.60s`) ended with exit code %d%s%.100s", e.id, e.cmd, e.rc, has_log ? "; its output is in " : "", has_log ? e.log : "");
            g_rz.task_ended = 1;
            rz_job_put(&e, e.status, e.rc, 1, now_sz);
        }
    }
#endif
    /* o fluxo que acompanha uma tarefa */
    if (fl_in(FL_TERMINAL) && !fl_job_alive(g_fl.task)) { g_fl.depth = fl_in(FL_TERMINAL); fl_pop("task gone"); g_fl.task = 0; }
    if (fl_in(FL_IMAGE) && !foreign) {
        FlowPos *ip = &g_fl.stk[fl_in(FL_IMAGE) - 1];
        if (ip->st == 3 && !fl_job_alive(g_fl.task)) {
            if (g_fl.path[0] && access(g_fl.path, R_OK) == 0) ip->st = 2;
            else { ip->st = 5; rz_dpush(d, &nd, 2, "the image command ended but %.200s was not written", g_fl.path[0] ? g_fl.path : "its output"); }
            g_fl.emitted_st = -1;
        } else if ((ip->st == 2 || ip->st == 1) && g_fl.path[0] && access(g_fl.path, R_OK) != 0) {
            ip->st = 5; g_fl.emitted_st = -1; rz_dpush(d, &nd, 2, "the image command ended but %.200s was not written", g_fl.path);
        }
    }
    if (g_rz.new_proc) {
        if (fl_in(FL_PLANMODE) && g_fl.stk[fl_in(FL_PLANMODE) - 1].st == 0 && !g_plan_mode) { g_plan_mode = 1; plan_ui(); rz_dpush(d, &nd, 5, "plan mode is on again: read only until the user approves the plan"); }
        if (fl_in(FL_WORKTREE)) { rz_flow_drop(FL_WORKTREE); rz_dpush(d, &nd, 5, "the worktree session did not survive the restart"); }
    }
    /* arquivos */
    for (int i = 0; i < s->nfile; i++) {
        RzFile f = s->file[i];
        if (foreign) { s->file[i].changed = 1; continue; }
        uint64_t h; long long sz, mt;
        const int ex = rz_file_info(f.path, &h, &sz, &mt);
        if (f.op == RZF_WRITE) {
            if (!ex) { rz_dpush(d, &nd, 3, "%.200s, which you edited, no longer exists", f.path); rz_ev('P', f.path, "0", "0", "0", "0", "0", "0", NULL); i--; continue; }
            if (sz != f.size || mt != f.mtime) {
                const int differs = (f.hash && h) ? h != f.hash : (sz != f.size || mt != f.mtime);
                if (differs) rz_dpush(d, &nd, 3, "%.200s changed outside this session after your edit: read it again before editing", f.path);
                rz_ev('P', f.path, rz_i(f.op), rz_x(h), rz_i(sz), rz_i(mt), rz_i(f.shown), rz_i(f.req), NULL);      /* o estado de agora e o novo ponto de comparacao */
            }
        } else if (f.op == RZF_MADE) {
            if (!ex) { rz_dpush(d, &nd, 2, "%.200s, which was made, no longer exists", f.path); rz_ev('P', f.path, "0", "0", "0", "0", "0", "0", NULL); i--; continue; }
        }
    }
    /* evidencia ligada a um arquivo: se o arquivo mudou, o item envelheceu */
    for (int i = 0; i < s->nev; i++) {
        RzEvid *e = &s->ev[i]; e->stale = 0;
        if (!e->path[0] || !e->hash) continue;
        uint64_t h; long long sz, mt;
        if (foreign || !rz_file_info(e->path, &h, &sz, &mt) || (h && h != e->hash)) e->stale = 1;
    }
    /* tentativas: com o tempo, a pasta trocada ou outra maquina, "nao repita" vira "falhou antes" */
    const long long now = (long long)time(NULL);
    for (int i = 0; i < s->natt; i++) {
        const RzAttempt a = s->att[i];
        if (a.dead && (now - a.t > 24 * 3600 || g_rz.moved || foreign))
            rz_ev('A', rz_x(a.key), rz_x(a.call), a.tool, a.subj, a.cause, rz_i(a.n), "0", a.fix, rz_i(a.epoch), NULL);
    }
    /* as frases: acumulam enquanto a reentrada da sessao espera o bloco; senao recomecam */
    if (g_rz.reason != RZR_SESSION) { g_rz.ndelta = 0; g_rz.more_delta = 0; }
    for (int pr = 0; pr < 8; pr++)
        for (int i = 0; i < nd; i++) {
            if (d[i].pr != pr) continue;
            int dup = 0; for (int k = 0; k < g_rz.ndelta; k++) if (!strncmp(g_rz.delta[k], d[i].t, 199)) dup = 1;
            if (dup) continue;
            if (g_rz.ndelta < 6) rz_cpy(g_rz.delta[g_rz.ndelta++], sizeof g_rz.delta[0], d[i].t); else g_rz.more_delta++;
        }
    g_rz.new_proc = g_rz.moved = g_rz.foreign = 0;
    rz_sync_flow();
}
/* o usuario diz que algo ja esta feito: nenhuma tentativa continua "nao repita" */
static void rz_forgive_attempts(void) {
    if (!rz_on() || g_ct_depth > 1) return;
    const RzState *s = &g_rz.s;
    for (int i = 0; i < s->natt; i++) { const RzAttempt a = s->att[i]; if (a.dead) rz_ev('A', rz_x(a.key), rz_x(a.call), a.tool, a.subj, a.cause, rz_i(a.n), "0", a.fix, rz_i(a.epoch), NULL); }
}

/* tarefa de segundo plano terminada e que o modelo nao soube (nao e a que o fluxo acompanha): uma clausula por tarefa, uma vez */
static size_t rz_job_clause(char *out, size_t cap) {
    out[0] = 0;
#ifndef _WIN32
    if (!rz_on() || g_ct_depth > 1 || cap < 400) return 0;
    rz_sync_jobs();
    size_t n = 0;
    for (int k = 0; k < RZ_NJOB; k++) {
        const RzJob e = g_rz.s.job[k];
        if (!e.id || e.told || e.agent_pid != (long)getpid() || e.id == g_fl.task) continue;
        if (e.status != RZJ_DONE && e.status != RZJ_STOPPED) continue;
        if (n + 260 >= cap) break;
        n += (size_t)snprintf(out + n, cap - n, "task %d (`%.60s`) ended with exit code %d; its output is in %.100s. ", e.id, e.cmd, e.rc, e.log);
        rz_job_put(&e, e.status, e.rc, 1, e.log_sz);
    }
    return n;
#else
    (void)cap; return 0;
#endif
}

/* tentativa morta de antes da reentrada: a chamada igual ganha um desvio (so conselho, nunca recusa) */
static void rz_dead_check(const char *name, const char *js, const char *end) {
    g_rz.dead_hit = 0;
    if (!rz_on() || g_ct_depth > 1 || g_fl.dev[0]) return;
    RzState *s = &g_rz.s;
    const char *when = g_rz.enter_reason == RZR_SUMMARY ? "the context was summarised" : "the session was reopened";
    for (int i = 0; i < s->natt; i++) {
        const RzAttempt *a = &s->att[i];
        if (!a->dead || a->call != g_fl.cur_sig || a->epoch >= s->epoch) continue;
        snprintf(g_fl.dev, sizeof g_fl.dev, "you made this call before %s and it failed %d times: %.100s. It will fail the same way; use what you have or tell the user what blocks you. ", when, a->n, a->cause);
        g_fl.dev_soft = 1; g_rz.dead_hit = 1;
        return;
    }
    char subj[80]; fm_subject(name, js, end, subj, sizeof subj);
    if (!strcmp(name, "SendInput")) rz_subject(name, js, end, subj, sizeof subj);
    rz_clean(subj, sizeof subj, subj, 0);
    int n = 0; const char *c1 = "", *c2 = "";
    for (int i = 0; i < s->natt; i++) {
        const RzAttempt *a = &s->att[i];
        if (!a->dead || a->epoch >= s->epoch || strcmp(a->tool, name) || strcmp(a->subj, subj)) continue;
        if (n == 0) c1 = a->cause; else if (n == 1) c2 = a->cause;
        n++;
    }
    if (n < 3) return;
    uint64_t key = fl_hash(name, strlen(name), (uint64_t)s->epoch + RZ_FNV); key = fl_hash(subj, strlen(subj), key);
    for (int i = 0; i < g_rz.nsubj_noted; i++) if (g_rz.subj_noted[i] == key) return;
    g_rz.subj_noted[g_rz.nsubj_noted++ & 7] = key; if (g_rz.nsubj_noted > 8) g_rz.nsubj_noted = 8;
    snprintf(g_fl.dev, sizeof g_fl.dev, "%d different calls around %.60s failed before: %.80s; %.80s. ", n, subj, c1, c2);
    g_fl.dev_soft = 1;
    return;
}
