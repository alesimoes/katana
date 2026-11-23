/* ---------- coder/watch.c: vigia de arquivos da sessao (fase 4 de docs/code-index-lifecycle.md, C.1 e C.3) ----------
 * O rastreador sai quando termina (nao fica vivo so para vigiar), entao quem observa e a SESSAO: um descritor inotify por processo, lido SEM
 * BLOQUEAR em tres momentos: antes de uma consulta (index_ensure), depois de uma ferramenta que muda arquivo (index_touch, Bash) e com a sessao
 * ociosa no prompt (watch_idle, chamado de ct_idle_line a cada ~3 s). Sem thread.
 *   - uma fila de mudancas por raiz (caminhos relativos, sem repeticao). Entrega ao rastreador: TOUCH pelo socket se ele esta vivo, senao um
 *     processo curto `katana coder crawl <raiz> --changed <arquivos>` (ele sai 2 s depois de assentar). Mais de 200 mudancas sem rastreador vivo,
 *     estouro da fila do kernel (IN_Q_OVERFLOW) ou mais de 2000 eventos sob uma pasta entre duas leituras viram SWEEP (varredura por stat).
 *   - a primeira visita de uma sessao a uma raiz que ja tem indice pede um SWEEP: mudancas feitas sem sessao aberta ninguem viu.
 *   - uma watch por pasta da arvore indexada (as mesmas regras da varredura: .gitignore, .katanaignore, pastas pesadas, raizes aninhadas;
 *     pastas ignoradas nao entram), mais rasas primeiro. Orcamento: um quarto de fs.inotify.max_user_watches por sessao (KATANA_INDEX_WATCH_MAX
 *     o substitui). O que passa do orcamento, ou falta de watches (ENOSPC), vira degradacao: a raiz fica com cobertura parcial e a sessao
 *     pede SWEEP periodico (max(2 s, 20 x o custo da ultima varredura)) e confere o mtime das pastas sem watch.
 *   - sem inotify (outro sistema, KATANA_INDEX_WATCH=poll, sistema de arquivos de rede ou FUSE): so varredura periodica pelo mesmo periodo.
 *   - mudanca de mtime de .git/HEAD ou .git/index pede varredura na hora (troca de ramo muda muitos arquivos juntos); o git e so uma dica.
 *   - KATANA_INDEX_SWEEP_S (fase 1) vira reserva: com cobertura total por inotify e o prazo (padrao 600 s) que forca um SWEEP mesmo sem evento.
 *   - o vigia so liga dentro de uma sessao do agente (watch_enable, chamado de autoindex_start): comandos avulsos `katana coder ...` seguem como antes.
 *   - estado de cada raiz vigiada em ~/.katana/run/watch-<pid>-<chave>.state, lido por `katana coder status` (watch_status_print).
 * KATANA_INDEX_WATCH=off desliga tudo (volta ao prazo da fase 1); =poll forca so varredura periodica. */
#ifndef _WIN32
#include <pthread.h>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#ifdef __linux__
#include <sys/vfs.h>
#include <sys/inotify.h>
#define WT_HAVE_INOTIFY 1
#endif
extern char **environ;
static int cw_client(const char *key, const char *line, char *reply, size_t cap, int timeout_ms);       /* crawl.c */
static void index_note_changed(const char *root);                                                       /* indexctl.c */
static void agent_home(char *out, size_t cap);

enum { WT_OFF = 0, WT_IDLE = 1, WT_PENDING = 2, WT_FIRST = 3 };       /* resultado de watch_poll */
enum { WT_ROOTS = 8, WT_QMAX = 4000, WT_SPAWN_MAX = 200, WT_EVT_DIR_MAX = 2000 };
enum { WTM_INOTIFY = 0, WTM_PARTIAL = 1, WTM_POLL = 2 };
typedef struct { char *rel; int root; uint32_t cnt, stamp; } WtWd;
typedef struct {
    char root[4096], key[24]; int used, mode; char why[120];
    int nwatch, nunwatched; char **ud; int64_t *udm; int capud;        /* pastas sem watch e o mtime visto */
    char **q; int nq, capq; uint64_t *qh; int qhn;                      /* fila: caminhos relativos, e o conjunto de hashes para nao repetir */
    int overflow, sweep_req, unsynced, first_pending;
    double t_change, t_sweep, t_deliver, t_poll, sweep_cost, t_attach;
    int64_t git_head, git_index;
    unsigned long events, delivered, sweeps, spawns, touches; double t_state;
    char sync_prefix[600];
} WtRoot;
static struct { int enabled, fd, forced, init_done, drain_id; long budget; WtRoot r[WT_ROOTS]; WtWd *wd; int nwd; unsigned long drains; } g_wt;
static pthread_mutex_t g_wt_mx = PTHREAD_MUTEX_INITIALIZER;

/* KATANA_INDEX_TRACE=1: uma linha em stderr por decisao do vigia (para conferir a mao) */
static void wt_trace(const char *fmt, ...) {
    static int on = -1; if (on < 0) { const char *e = getenv("KATANA_INDEX_TRACE"); on = e && *e && *e != '0'; }
    if (!on) return;
    char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fprintf(stderr, "[watch %.3f] %s\n", fmod(now_sec(), 1000.0), b);
}
static int wt_env_ms(const char *n, int d) { const char *e = getenv(n); return e && *e ? atoi(e) : d; }
static int wt_mode_env(void) { const char *e = getenv("KATANA_INDEX_WATCH"); if (!e || !*e) return 1; if (!strcmp(e, "off") || !strcmp(e, "0")) return 0; if (!strcmp(e, "poll")) return 2; return 1; }
static double wt_reserve_s(void) { const char *e = getenv("KATANA_INDEX_SWEEP_S"); return e && *e ? atof(e) : 600.0; }
static void wt_key(const char *root, char *key, size_t cap) { snprintf(key, cap, "%016llx", (unsigned long long)store_hash64(root, strlen(root), 0)); }
static int64_t wt_mt(const char *p) { struct stat st; if (stat(p, &st) != 0) return 0; return (int64_t)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec; }
/* o sistema de arquivos entrega eventos? rede e FUSE nao (o servidor ou o programa de espaco de usuario muda sem o kernel local saber) */
#ifndef __linux__
struct statfs { unsigned long f_type; };
static int statfs(const char *p, struct statfs *s) { (void)p; (void)s; return -1; }
#endif
static const char *wt_fs_nowatch(const char *path) {
    struct statfs s; if (statfs(path, &s) != 0) return NULL;
    switch ((unsigned long)s.f_type) {
        case 0x6969ul: return "network filesystem (NFS)";
        case 0x517Bul: case 0xFE534D42ul: case 0xFF534D42ul: return "network filesystem (SMB/CIFS)";
        case 0x65735546ul: return "FUSE filesystem";
        case 0x00C36400ul: return "network filesystem (Ceph)";
        case 0x73757245ul: return "network filesystem (Coda)";
        case 0x5346414Ful: return "network filesystem (AFS)";
        default: return NULL;
    }
}
static long wt_budget(void) {
    const char *e = getenv("KATANA_INDEX_WATCH_MAX"); if (e && *e) return atol(e);
    long mx = 8192; FILE *f = fopen("/proc/sys/fs/inotify/max_user_watches", "r"); if (f) { if (fscanf(f, "%ld", &mx) != 1) mx = 8192; fclose(f); }
    long b = mx / 4; return b < 100 ? 100 : b;
}
static void watch_enable(void) { if (!g_wt.enabled && wt_mode_env() != 0) g_wt.enabled = 1; }
/* ligado pela sessao do agente (watch_enable) ou, para quem roda fora dela (`katana toolrun`, testes), por KATANA_INDEX_WATCH=on */
static int wt_on(void) { if (!g_wt.enabled) { const char *e = getenv("KATANA_INDEX_WATCH"); if (e && (!strcmp(e, "on") || !strcmp(e, "poll"))) g_wt.enabled = 1; } return g_wt.enabled; }

/* ---------- arvore das pastas vigiadas: as mesmas regras de st_scan_walk, so para pastas ---------- */
typedef struct { char **rel; int *depth; int n, cap; } WtDirs;
static void wt_dirs_add(WtDirs *D, const char *rel, int depth) {
    if (D->n == D->cap) { D->cap = D->cap ? D->cap * 2 : 256; D->rel = (char **)realloc(D->rel, sizeof(char *) * (size_t)D->cap); D->depth = (int *)realloc(D->depth, sizeof(int) * (size_t)D->cap); }
    D->rel[D->n] = strdup(rel); D->depth[D->n++] = depth;
}
static int wt_push_rules(StoreScan *S, const char *path, size_t plen, int isroot) {
    if (S->nrs >= ST_MAXRS) return 0;
    StoreRuleSet *R = &S->rs[S->nrs]; memset(R, 0, sizeof *R); R->base = plen;
    char f[4200]; snprintf(f, sizeof f, "%s/.gitignore", path); st_rules_load(R, f);
    if (isroot) {
        S->has_gitignore = R->n > 0;
        if (!S->has_gitignore) { static const char *const hv[] = {"ref", "models", "modelos", ".claude", "node_modules", "build", "dist", "target", "__pycache__", ".venv", "venv", ".cache", NULL};
            for (int i = 0; hv[i]; i++) { char l[64]; snprintf(l, sizeof l, "/%s/", hv[i]); st_rules_add(R, l, strlen(l)); } }
    }
    snprintf(f, sizeof f, "%s/.katanaignore", path); st_rules_load(R, f);
    if (R->n) { S->nrs++; return 1; }
    free(R->r); return 0;
}
static void wt_pop_rules(StoreScan *S) { StoreRuleSet *R = &S->rs[--S->nrs]; for (int i = 0; i < R->n; i++) free(R->r[i].pat); free(R->r); }
static void wt_walk(StoreScan *S, char *path, size_t plen, size_t rel0, int depth, int isroot, WtDirs *D) {
    DIR *d = opendir(path); if (!d) return;
    const int pushed = wt_push_rules(S, path, plen, isroot);
    wt_dirs_add(D, plen > rel0 ? path + rel0 + 1 : "", depth);
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name; if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        const size_t nl = strlen(nm); if (plen + 1 + nl + 1 >= 4000) continue;
        int isd = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN) { struct stat st; char t[4200]; snprintf(t, sizeof t, "%s/%s", path, nm); isd = lstat(t, &st) == 0 && S_ISDIR(st.st_mode); }
        if (!isd) continue;
        size_t at = plen; path[at++] = '/'; memcpy(path + at, nm, nl + 1);
        int skip = st_ignored_dir(nm) || (S->nrs && st_scan_ignored(S, path, nm, 1)) || rt_skip_subdir(path);
        if (!skip && isroot && S->has_gitignore && (!strcmp(nm, "ref") || !strcmp(nm, "models")) && st_count_files_capped(path, at + nl, 5000) > 5000) skip = 1;
        if (!skip) wt_walk(S, path, at + nl, rel0, depth + 1, 0, D);
        path[plen] = 0;
    }
    closedir(d);
    if (pushed) wt_pop_rules(S);
}
static void wt_dirs_free(WtDirs *D) { for (int i = 0; i < D->n; i++) free(D->rel[i]); free(D->rel); free(D->depth); memset(D, 0, sizeof *D); }
/* regras das pastas acima de `rel` (da raiz ate a pasta pai), para percorrer uma pasta nova sem refazer a raiz toda */
static void wt_prime(StoreScan *S, const char *root, const char *rel) {
    char path[4200]; snprintf(path, sizeof path, "%s", root); size_t pl = strlen(path); wt_push_rules(S, path, pl, 1);
    const char *p = rel;
    while (*p) {
        const char *sl = strchr(p, '/'); size_t cl = sl ? (size_t)(sl - p) : strlen(p); if (!sl) break;      /* so os ancestrais, nao a pasta nova */
        path[pl] = '/'; memcpy(path + pl + 1, p, cl); pl += 1 + cl; path[pl] = 0; wt_push_rules(S, path, pl, 0); p = sl + 1;
    }
}

/* ---------- raizes e watches ---------- */
static WtRoot *wt_find(const char *root) { for (int i = 0; i < WT_ROOTS; i++) if (g_wt.r[i].used && !strcmp(g_wt.r[i].root, root)) return &g_wt.r[i]; return NULL; }
/* raiz vigiada que contem o caminho absoluto */
static WtRoot *wt_covering(const char *abs) {
    WtRoot *best = NULL; size_t bl = 0;
    for (int i = 0; i < WT_ROOTS; i++) { WtRoot *r = &g_wt.r[i]; if (!r->used) continue; const size_t rl = strlen(r->root);
        if (!strncmp(abs, r->root, rl) && (abs[rl] == '/' || !abs[rl]) && rl >= bl) { best = r; bl = rl; } }
    return best;
}
static int wt_set_wd(int wd, int root, const char *rel) {
    if (wd < 0 || wd > (1 << 22)) return 0;
    if (wd >= g_wt.nwd) { int n = g_wt.nwd ? g_wt.nwd : 256; while (n <= wd) n *= 2; g_wt.wd = (WtWd *)realloc(g_wt.wd, sizeof(WtWd) * (size_t)n); memset(g_wt.wd + g_wt.nwd, 0, sizeof(WtWd) * (size_t)(n - g_wt.nwd)); g_wt.nwd = n; }
    free(g_wt.wd[wd].rel); g_wt.wd[wd].rel = strdup(rel); g_wt.wd[wd].root = root; g_wt.wd[wd].cnt = 0; g_wt.wd[wd].stamp = 0; return 1;
}
static void wt_ud_add(WtRoot *r, const char *rel) {
    if (r->nunwatched == r->capud) { r->capud = r->capud ? r->capud * 2 : 64; r->ud = (char **)realloc(r->ud, sizeof(char *) * (size_t)r->capud); r->udm = (int64_t *)realloc(r->udm, sizeof(int64_t) * (size_t)r->capud); }
    char a[4400]; snprintf(a, sizeof a, "%s%s%s", r->root, *rel ? "/" : "", rel);
    r->ud[r->nunwatched] = strdup(rel); r->udm[r->nunwatched++] = wt_mt(a);
}
static long wt_total_watches(void) { long n = 0; for (int i = 0; i < WT_ROOTS; i++) if (g_wt.r[i].used) n += g_wt.r[i].nwatch; return n; }
/* uma watch para a pasta `rel` da raiz; 1 = colocada */
static int wt_add_dir(WtRoot *r, int ri, const char *rel, int *enospc) {
#ifdef WT_HAVE_INOTIFY
    if (g_wt.fd < 0 || wt_total_watches() >= g_wt.budget) return 0;
    char a[4400]; snprintf(a, sizeof a, "%s%s%s", r->root, *rel ? "/" : "", rel);
    const int wd = inotify_add_watch(g_wt.fd, a, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK);
    if (wd < 0) { if (errno == ENOSPC && enospc) *enospc = 1; return 0; }
    if (!wt_set_wd(wd, ri, rel)) return 0;
    r->nwatch++; return 1;
#else
    (void)r; (void)ri; (void)rel; (void)enospc; return 0;
#endif
}
static int wt_cmp_depth(const void *a, const void *b) { return ((const int *)a)[1] - ((const int *)b)[1]; }
static void wt_watch_tree(WtRoot *r, int ri, const char *subrel) {        /* watches da arvore sob subrel ("" = a raiz toda), mais rasas primeiro */
    StoreScan S; memset(&S, 0, sizeof S); WtDirs D; memset(&D, 0, sizeof D);
    char path[4200]; snprintf(path, sizeof path, "%s%s%s", r->root, *subrel ? "/" : "", subrel);
    const size_t rel0 = strlen(r->root);
    if (*subrel) wt_prime(&S, r->root, subrel);
    wt_walk(&S, path, strlen(path), rel0, 0, *subrel ? 0 : 1, &D);
    while (S.nrs > 0) wt_pop_rules(&S);
    free(S.e);
    int *ord = (int *)malloc(sizeof(int) * 2 * (size_t)(D.n ? D.n : 1));
    for (int i = 0; i < D.n; i++) { ord[2 * i] = i; ord[2 * i + 1] = D.depth[i]; }
    qsort(ord, (size_t)D.n, sizeof(int) * 2, wt_cmp_depth);
    int enospc = 0;
    for (int k = 0; k < D.n; k++) { const char *rel = D.rel[ord[2 * k]];
        if (enospc || !wt_add_dir(r, ri, rel, &enospc)) wt_ud_add(r, rel); }
    if (enospc) snprintf(r->why, sizeof r->why, "inotify watch limit reached (fs.inotify.max_user_watches)");
    else if (r->nunwatched && !r->why[0]) snprintf(r->why, sizeof r->why, "watch budget of %ld used", g_wt.budget);
    free(ord); wt_dirs_free(&D);
}
static void wt_state_write(WtRoot *r, int force);
static WtRoot *wt_attach(const char *root, int swept) {
    int ri = -1; for (int i = 0; i < WT_ROOTS; i++) if (!g_wt.r[i].used) { ri = i; break; }
    if (ri < 0) return NULL;
    WtRoot *r = &g_wt.r[ri]; memset(r, 0, sizeof *r); snprintf(r->root, sizeof r->root, "%s", root); wt_key(root, r->key, sizeof r->key);
    r->used = 1; r->t_attach = now_sec(); r->t_sweep = swept ? r->t_attach : 0; r->first_pending = !swept;
    char gp[4300]; snprintf(gp, sizeof gp, "%s/.git/HEAD", root); r->git_head = wt_mt(gp); snprintf(gp, sizeof gp, "%s/.git/index", root); r->git_index = wt_mt(gp);
    if (!g_wt.init_done) {
        g_wt.init_done = 1; g_wt.fd = -1; g_wt.budget = wt_budget();
#ifdef WT_HAVE_INOTIFY
        if (wt_mode_env() == 1) g_wt.fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#endif
    }
    const char *nw = wt_fs_nowatch(root);
    if (wt_mode_env() == 2) { r->mode = WTM_POLL; snprintf(r->why, sizeof r->why, "KATANA_INDEX_WATCH=poll"); }
    else if (nw) { r->mode = WTM_POLL; snprintf(r->why, sizeof r->why, "%s: no change events", nw); }
    else if (g_wt.fd < 0) { r->mode = WTM_POLL; snprintf(r->why, sizeof r->why, "inotify not available"); }
    else {
        wt_watch_tree(r, ri, "");
        r->mode = r->nunwatched ? (r->nwatch ? WTM_PARTIAL : WTM_POLL) : WTM_INOTIFY;
    }
    wt_trace("attach %s: %s, %d watches, %d unwatched (budget %ld)%s%s", r->root, r->mode == WTM_INOTIFY ? "inotify" : r->mode == WTM_PARTIAL ? "inotify+poll" : "poll", r->nwatch, r->nunwatched, g_wt.budget, r->why[0] ? "; " : "", r->why);
    wt_state_write(r, 1);
    return r;
}

/* ---------- fila de mudancas ---------- */
static int wt_q_add(WtRoot *r, const char *rel) {
    if (r->nq >= WT_QMAX) { r->overflow = 1; return 0; }
    const uint64_t h = store_hash64(rel, strlen(rel), 5) | 1;
    if (r->qhn < 8192) { r->qh = r->qh ? r->qh : (uint64_t *)calloc(16384, sizeof(uint64_t)); }
    uint32_t i = (uint32_t)h & 16383;
    while (r->qh[i]) { if (r->qh[i] == h) return 0; i = (i + 1) & 16383; }
    if (r->qhn >= 8192) { r->overflow = 1; return 0; }
    r->qh[i] = h; r->qhn++;
    if (r->nq == r->capq) { r->capq = r->capq ? r->capq * 2 : 64; r->q = (char **)realloc(r->q, sizeof(char *) * (size_t)r->capq); }
    r->q[r->nq++] = strdup(rel); return 1;
}
static void wt_q_clear(WtRoot *r) { for (int i = 0; i < r->nq; i++) free(r->q[i]); r->nq = 0; r->qhn = 0; if (r->qh) memset(r->qh, 0, 16384 * sizeof(uint64_t)); r->overflow = 0; r->sweep_req = 0; }

/* ---------- leitura dos eventos (sem bloquear) ---------- */
static void wt_drain(void) {
#ifdef WT_HAVE_INOTIFY
    if (g_wt.fd < 0) return;
    static char buf[1 << 16] __attribute__((aligned(8)));
    g_wt.drain_id++; g_wt.drains++;
    for (;;) {
        const ssize_t n = read(g_wt.fd, buf, sizeof buf);
        if (n <= 0) break;
        for (char *p = buf; p < buf + n; ) {
            const struct inotify_event *e = (const struct inotify_event *)p; p += sizeof *e + e->len;
            if (e->mask & IN_Q_OVERFLOW) { for (int i = 0; i < WT_ROOTS; i++) if (g_wt.r[i].used && g_wt.r[i].mode != WTM_POLL) { g_wt.r[i].overflow = 1; g_wt.r[i].t_change = now_sec(); } continue; }
            if (e->wd < 0 || e->wd >= g_wt.nwd) continue;
            WtWd *w = &g_wt.wd[e->wd]; if (!w->rel) continue;
            WtRoot *r = &g_wt.r[w->root];
            if (e->mask & IN_IGNORED) { free(w->rel); w->rel = NULL; if (r->nwatch > 0) r->nwatch--; continue; }
            if (w->stamp != (uint32_t)g_wt.drain_id) { w->stamp = (uint32_t)g_wt.drain_id; w->cnt = 0; }
            if (++w->cnt > WT_EVT_DIR_MAX) { r->overflow = 1; continue; }
            char rel[4400];
            if (e->len && e->name[0]) snprintf(rel, sizeof rel, "%s%s%s", w->rel, *w->rel ? "/" : "", e->name); else snprintf(rel, sizeof rel, "%s", w->rel);
            r->events++; r->t_change = now_sec();
            if (!rel[0]) { r->sweep_req = 1; continue; }                          /* a propria raiz sumiu ou mudou de nome */
            wt_q_add(r, rel);
            if ((e->mask & IN_ISDIR) && (e->mask & (IN_CREATE | IN_MOVED_TO))) wt_watch_tree(r, w->root, rel);      /* pasta nova: vigiada ja, e o rastreador a percorre como ramo */
            if (r->mode == WTM_INOTIFY && r->nunwatched) r->mode = WTM_PARTIAL;
        }
    }
#endif
}

/* ---------- entrega ao rastreador ---------- */
static int wt_spawn(const char *root, int sweep, char **rels, int nr) {
    char exe[1100]; const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1); if (n <= 0) return -1; exe[n] = 0;
    static char abs[WT_SPAWN_MAX][4500]; char *argv[WT_SPAWN_MAX + 16]; int k = 0;
    argv[k++] = "sh"; argv[k++] = "-c"; argv[k++] = "\"$0\" \"$@\" >/dev/null 2>&1 &"; argv[k++] = exe; argv[k++] = "coder"; argv[k++] = "crawl"; argv[k++] = (char *)root; argv[k++] = "--quiet";
    if (sweep || nr > WT_SPAWN_MAX || nr == 0) argv[k++] = "--sweep";
    else { argv[k++] = "--changed"; for (int i = 0; i < nr; i++) { snprintf(abs[i], sizeof abs[i], "%s%s%s", root, *rels[i] ? "/" : "", rels[i]); argv[k++] = abs[i]; } }
    argv[k] = NULL;
    pid_t pid; if (posix_spawn(&pid, "/bin/sh", NULL, NULL, argv, environ) != 0) return -1;
    int st; waitpid(pid, &st, 0); return 0;
}
static void wt_common_prefix(WtRoot *r) {       /* pasta comum das mudancas: a espera da consulta olha so ela */
    r->sync_prefix[0] = 0; if (r->nq == 0 || r->overflow || r->sweep_req) return;
    char pre[600]; snprintf(pre, sizeof pre, "%s", r->q[0]); char *sl = strrchr(pre, '/'); if (sl) *sl = 0; else pre[0] = 0;     /* pasta do primeiro */
    for (int i = 1; i < r->nq && pre[0]; i++) { size_t pl = strlen(pre); const char *q = r->q[i]; while (pre[0] && !(!strncmp(q, pre, pl) && (q[pl] == '/' || !q[pl]))) { char *s2 = strrchr(pre, '/'); if (s2) *s2 = 0; else pre[0] = 0; pl = strlen(pre); } }
    if (!strchr(pre, ' ')) snprintf(r->sync_prefix, sizeof r->sync_prefix, "%s", pre);
}
/* 0 = entregue, -1 = nao deu (sem como iniciar o rastreador) */
static int wt_deliver(WtRoot *r) {
    const int want_sweep = r->overflow || r->sweep_req || r->nq > WT_QMAX / 2;
    if (!want_sweep && r->nq == 0) return 0;
    wt_common_prefix(r);
    char rep[300]; int sent = 0;
    if (cw_client(r->key, "PING", rep, sizeof rep, 400) == 0) {
        int fail = 0;
        if (want_sweep) fail = cw_client(r->key, "SWEEP", rep, sizeof rep, 1000) != 0;
        else for (int i = 0; i < r->nq && !fail; i++) { char l[4500]; snprintf(l, sizeof l, "TOUCH %s", r->q[i]); fail = cw_client(r->key, l, rep, sizeof rep, 1000) != 0; }
        sent = !fail;
    }
    wt_trace("deliver %s: %d queued%s%s via %s", r->root, r->nq, r->overflow ? ", overflow" : "", r->sweep_req ? ", sweep requested" : "", sent ? "socket" : "new crawler process");
    if (!sent) { if (wt_spawn(r->root, want_sweep, r->q, r->nq) != 0) return -1; r->spawns++; }
    if (want_sweep) { r->sweeps++; r->t_sweep = now_sec(); }
    r->delivered += (unsigned long)(r->nq ? r->nq : 1); r->t_deliver = now_sec(); r->unsynced = 1;
    index_note_changed(r->root);
    wt_q_clear(r); wt_state_write(r, 0);
    return 0;
}
/* espera o rastreador drenar o que foi entregue (SYNC), no maximo ms; 1 = completo */
static int wt_wait(WtRoot *r, int ms, const char *prefix) {
    const double t0 = now_sec(), dl = t0 + ms / 1000.0; char rep[300]; int done = 0;
    for (;;) {
        int left = (int)((dl - now_sec()) * 1000); if (left < 20) left = 20;
        char l[700]; snprintf(l, sizeof l, "SYNC 3 %s %d", prefix && *prefix ? prefix : "-", left);
        if (cw_client(r->key, l, rep, sizeof rep, left + 300) == 0) { done = strstr(rep, "complete") != NULL; break; }
        if (now_sec() >= dl) break;
        usleep(15000);
    }
    r->unsynced = !done; if (done) r->sweep_cost = now_sec() - t0;
    wt_trace("wait %s: %s after %.0f ms (budget %d ms)", prefix && *prefix ? prefix : "whole root", done ? "drained" : "NOT drained", (now_sec() - t0) * 1000, ms);
    return done;
}

/* ---------- varredura periodica, dica do git ---------- */
static double wt_period(const WtRoot *r) { const double p = 20.0 * r->sweep_cost; return p < 2.0 ? 2.0 : p; }
static void wt_checks(WtRoot *r) {
    const double now = now_sec();
    char gp[4300]; snprintf(gp, sizeof gp, "%s/.git/HEAD", r->root); const int64_t h = wt_mt(gp); snprintf(gp, sizeof gp, "%s/.git/index", r->root); const int64_t ix = wt_mt(gp);
    if (h != r->git_head || ix != r->git_index) { r->git_head = h; r->git_index = ix; if (r->t_sweep) r->sweep_req = 1; }       /* troca de ramo: muitos arquivos juntos */
    if (r->mode != WTM_INOTIFY && now - r->t_poll >= wt_period(r)) {
        r->t_poll = now;
        for (int i = 0; i < r->nunwatched; i++) { char a[4400]; snprintf(a, sizeof a, "%s%s%s", r->root, *r->ud[i] ? "/" : "", r->ud[i]); const int64_t m = wt_mt(a);
            if (m != r->udm[i]) { r->udm[i] = m; wt_q_add(r, r->ud[i]); r->t_change = now; } }
        if (now - r->t_sweep >= wt_period(r) && r->t_sweep) r->sweep_req = 1;           /* reescrita no lugar nao muda o mtime da pasta: so o stat dos arquivos acha */
    }
    if (r->mode == WTM_INOTIFY && r->t_sweep && now - r->t_sweep >= wt_reserve_s()) r->sweep_req = 1;       /* reserva (KATANA_INDEX_SWEEP_S) */
}

/* ---------- estado em disco, para `katana coder status` ---------- */
static void wt_state_path(const WtRoot *r, char *out, size_t cap) { char h[1000]; agent_home(h, sizeof h); snprintf(out, cap, "%s/.katana/run/watch-%ld-%s.state", h, (long)getpid(), r->key); }
static void wt_state_write(WtRoot *r, int force) {
    const double now = now_sec(); if (!force && now - r->t_state < 0.5) return; r->t_state = now;
    char p[1300], rd[1100]; { char h[1000]; agent_home(h, sizeof h); snprintf(rd, sizeof rd, "%s/.katana/run", h); } if (st_mkdirs(rd) != 0) return;
    wt_state_path(r, p, sizeof p); FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "pid %ld\nroot %s\nmode %s\nwhy %s\nwatches %d\nunwatched %d\nbudget %ld\nqueue %d\nevents %lu\ndelivered %lu\nspawns %lu\nsweeps %lu\nsince_change %.1f\nsince_sweep %.1f\nperiod %.1f\n",
            (long)getpid(), r->root, r->mode == WTM_INOTIFY ? "inotify" : r->mode == WTM_PARTIAL ? "inotify+poll" : "poll", r->why, r->nwatch, r->nunwatched, g_wt.budget, r->nq + (r->overflow || r->sweep_req ? 1 : 0),
            r->events, r->delivered, r->spawns, r->sweeps, r->t_change ? now - r->t_change : -1.0, r->t_sweep ? now - r->t_sweep : -1.0, wt_period(r));
    fclose(f); chmod(p, 0600);
}
static void wt_atexit(void) { for (int i = 0; i < WT_ROOTS; i++) if (g_wt.r[i].used) { char p[1300]; wt_state_path(&g_wt.r[i], p, sizeof p); unlink(p); } }
static void wt_register_exit(void) { static int done; if (!done) { done = 1; atexit(wt_atexit); } }
/* `katana coder status`: o que as sessoes abertas vigiam nesta raiz */
static void watch_status_print(const char *root) {
    char h[1000]; agent_home(h, sizeof h); char rd[1100]; snprintf(rd, sizeof rd, "%s/.katana/run", h);
    char key[24]; wt_key(root, key, sizeof key); DIR *d = opendir(rd); int shown = 0;
    if (d) { struct dirent *de; char suf[40]; snprintf(suf, sizeof suf, "-%s.state", key);
        while ((de = readdir(d))) {
            if (strncmp(de->d_name, "watch-", 6) != 0) continue;
            const size_t nl = strlen(de->d_name), sl = strlen(suf); if (nl < sl || strcmp(de->d_name + nl - sl, suf) != 0) continue;
            char p[1400]; snprintf(p, sizeof p, "%s/%s", rd, de->d_name); FILE *f = fopen(p, "r"); if (!f) continue;
            long pid = 0; char mode[40] = "", why[600] = ""; int w = 0, u = 0, q = 0; long budget = 0; unsigned long ev = 0, dl = 0, sp = 0, sw = 0; double sc = -1, ss = -1, per = 0; char ln[600];
            while (fgets(ln, sizeof ln, f)) { ln[strcspn(ln, "\n")] = 0;
                if (!strncmp(ln, "pid ", 4)) pid = atol(ln + 4); else if (!strncmp(ln, "mode ", 5)) snprintf(mode, sizeof mode, "%s", ln + 5); else if (!strncmp(ln, "why ", 4)) snprintf(why, sizeof why, "%s", ln + 4);
                else if (!strncmp(ln, "watches ", 8)) w = atoi(ln + 8); else if (!strncmp(ln, "unwatched ", 10)) u = atoi(ln + 10); else if (!strncmp(ln, "budget ", 7)) budget = atol(ln + 7);
                else if (!strncmp(ln, "queue ", 6)) q = atoi(ln + 6); else if (!strncmp(ln, "events ", 7)) ev = strtoul(ln + 7, NULL, 10); else if (!strncmp(ln, "delivered ", 10)) dl = strtoul(ln + 10, NULL, 10);
                else if (!strncmp(ln, "spawns ", 7)) sp = strtoul(ln + 7, NULL, 10); else if (!strncmp(ln, "sweeps ", 7)) sw = strtoul(ln + 7, NULL, 10);
                else if (!strncmp(ln, "since_change ", 13)) sc = atof(ln + 13); else if (!strncmp(ln, "since_sweep ", 12)) ss = atof(ln + 12); else if (!strncmp(ln, "period ", 7)) per = atof(ln + 7); }
            fclose(f);
            if (pid <= 0 || (kill((pid_t)pid, 0) != 0 && errno == ESRCH)) { unlink(p); continue; }
            if (!shown) printf("vigia de arquivos (watcher; uma por sessao aberta):\n");
            shown++;
            printf("  sessao pid %ld: %s%s%s; %d watches (orcamento %ld), %d pastas sem watch; fila %d; eventos lidos %lu, mudancas entregues %lu (%lu varreduras, %lu rastreadores iniciados)\n", pid, strcmp(mode, "inotify") == 0 ? "ativo, inotify" : strcmp(mode, "inotify+poll") == 0 ? "ativo, inotify parcial + varredura periodica" : "varredura periodica (sem inotify)", why[0] ? " [" : "", why, w, budget, u, q, ev, dl, sw, sp);
            if (why[0]) printf("    motivo: %s\n", why);
            if (sc >= 0) printf("    ultima mudanca ha %.1f s", sc);
            else printf("    nenhuma mudanca vista ainda");
            if (ss >= 0) printf("; ultima varredura pedida ha %.1f s", ss);
            if (strcmp(mode, "inotify") != 0) printf("; periodo %.1f s", per);
            printf("\n");
        }
        closedir(d); }
    if (!shown) printf("vigia de arquivos: nenhuma sessao aberta vigia esta raiz (o vigia vive dentro de cada sessao do agente; a primeira visita de uma sessao pede uma varredura por stat)\n");
    { const char *nw = wt_fs_nowatch(root); struct statfs s; if (nw) printf("  aviso: %s; sessoes usam so varredura periodica aqui\n", nw); else if (statfs(root, &s) == 0 && (unsigned long)s.f_type == 0x7366746eul) printf("  nota: sistema de arquivos ntfs3; inotify entrega eventos das mudancas feitas por este kernel (verificado na instalacao); mudancas feitas fora dele (outro sistema operacional) so a varredura acha\n"); }
}

/* ---------- pontos de chamada ---------- */
/* uma mudanca conhecida por dica (ferramenta do agente gravou, ou arquivo citado que mudou): entra na fila da raiz vigiada que a contem */
static void watch_note_path(const char *abs) {
    if (!wt_on()) return;
    pthread_mutex_lock(&g_wt_mx);
    WtRoot *r = wt_covering(abs);
    if (r) { wt_drain(); const size_t rl = strlen(r->root); const char *rel = abs[rl] ? abs + rl + 1 : ""; if (*rel) wt_q_add(r, rel); else r->sweep_req = 1; r->t_change = now_sec(); wt_register_exit(); wt_deliver(r); }
    pthread_mutex_unlock(&g_wt_mx);
}
/* o Bash pode ter mudado coisas: le o que o kernel ja enfileirou e entrega */
static void watch_after_tool(void) {
    if (!wt_on()) return;
    pthread_mutex_lock(&g_wt_mx);
    wt_drain();
    for (int i = 0; i < WT_ROOTS; i++) if (g_wt.r[i].used) { wt_checks(&g_wt.r[i]); if (g_wt.r[i].nq || g_wt.r[i].overflow || g_wt.r[i].sweep_req) wt_deliver(&g_wt.r[i]); }
    pthread_mutex_unlock(&g_wt_mx);
}
/* com a sessao ociosa no prompt (ct_idle_line, a cada ~3 s) */
static void watch_idle(void) { watch_after_tool(); }
/* a raiz tem cobertura total por inotify? (entao KATANA_INDEX_SWEEP_S e so reserva) */
static int watch_full(const char *abs) { if (!wt_on()) return 0; pthread_mutex_lock(&g_wt_mx); WtRoot *r = wt_covering(abs); const int f = r && r->mode == WTM_INOTIFY; pthread_mutex_unlock(&g_wt_mx); return f; }
static double watch_reserve(void) { return wt_reserve_s(); }
/* so le os eventos (para quem decide por outro caminho, como o Find com indice proprio) */
static void watch_drain_quiet(void) { if (!wt_on()) return; pthread_mutex_lock(&g_wt_mx); wt_drain(); pthread_mutex_unlock(&g_wt_mx); }
/* chamado por index_ensure com a raiz resolvida e o indice ja existente. have_index = 0: raiz sem indice ainda (index_ensure o cria no proprio processo e
 * chama watch_adopt depois). Devolve WT_OFF (o vigia nao cobre: vale o prazo da fase 1), WT_IDLE (nada pendente), WT_PENDING (mudancas entregues; a consulta
 * espera ate KATANA_INDEX_FRESH_MS) ou WT_FIRST (primeira visita: varredura pedida; espera ate KATANA_INDEX_WAIT_MS). *wait_ms diz quanto esperar. */
static int watch_poll(const char *root, int have_index, int *wait_ms, char *prefix, size_t pcap) {
    if (wait_ms) *wait_ms = 0;
    if (prefix && pcap) prefix[0] = 0;
    if (!wt_on() || !have_index) return WT_OFF;
    pthread_mutex_lock(&g_wt_mx); wt_register_exit();
    int rc = WT_IDLE; WtRoot *r = wt_find(root);
    if (!r) { r = wt_attach(root, 0); if (!r) { pthread_mutex_unlock(&g_wt_mx); return WT_OFF; } }
    wt_drain();
    for (int i = 0; i < WT_ROOTS; i++) if (g_wt.r[i].used && &g_wt.r[i] != r) wt_checks(&g_wt.r[i]);       /* as outras raizes tambem avancam */
    wt_checks(r);
    int first = 0;
    if (r->first_pending) { r->first_pending = 0; r->sweep_req = 1; first = 1; }
    int delivered = 0;
    if (r->nq || r->overflow || r->sweep_req) { if (wt_deliver(r) != 0) { rc = WT_OFF; goto out; } delivered = 1; }
    if (first) { rc = WT_FIRST; if (wait_ms) *wait_ms = wt_env_ms("KATANA_INDEX_WAIT_MS", 2000); }
    else if (delivered || r->unsynced) { rc = WT_PENDING; if (wait_ms) *wait_ms = wt_env_ms("KATANA_INDEX_FRESH_MS", 300); if (prefix && pcap && !first) snprintf(prefix, pcap, "%s", r->sync_prefix); }
out:
    pthread_mutex_unlock(&g_wt_mx);
    return rc;
}
static int watch_wait(const char *root, int ms, const char *prefix) {
    pthread_mutex_lock(&g_wt_mx); WtRoot *r = wt_find(root); int ok = 0; if (r) ok = wt_wait(r, ms, prefix); pthread_mutex_unlock(&g_wt_mx); return ok;
}
/* a raiz acabou de ser indexada em processo (index_ensure sem indice previo): ja esta varrida, o vigia entra sem pedir varredura */
static void watch_adopt(const char *root) {
    if (!wt_on()) return;
    pthread_mutex_lock(&g_wt_mx); wt_register_exit(); if (!wt_find(root)) wt_attach(root, 1); pthread_mutex_unlock(&g_wt_mx);
}
#else
static void watch_enable(void) {}
static void watch_note_path(const char *abs) { (void)abs; }
static void watch_after_tool(void) {}
static void watch_idle(void) {}
static int watch_full(const char *abs) { (void)abs; return 0; }
static double watch_reserve(void) { return 600.0; }
static void watch_drain_quiet(void) {}
static int watch_poll(const char *root, int have_index, int *wait_ms, char *prefix, size_t pcap) { (void)root; (void)have_index; (void)prefix; (void)pcap; if (wait_ms) *wait_ms = 0; return 0; }
static int watch_wait(const char *root, int ms, const char *prefix) { (void)root; (void)ms; (void)prefix; return 0; }
static void watch_adopt(const char *root) { (void)root; }
static void watch_status_print(const char *root) { (void)root; }
#endif
