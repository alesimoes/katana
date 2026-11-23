/* ---------- coder/indexctl.c: quando o indice do coder precisa ser revalidado, e as visoes de leitura reaproveitadas ----------
 * Fase 1 de docs/code-index-lifecycle.md ("consultas baratas"). Antes: toda chamada de Find ou Symbols varria a arvore inteira (o Find
 * duas vezes: git ls-files + stat, depois lstat do catalogo do coder) e remontava a visao de leitura. Agora:
 *   - index_ensure(): o unico ponto que decide se o catalogo do coder e revalidado (varredura + escrita incremental) ou se a consulta
 *     segue direto. Revalida na primeira consulta de cada raiz no processo, quando uma ferramenta do agente gravou ou apagou algo
 *     debaixo da raiz (index_touch, chamado por Edit, Write, MultiEdit, NotebookEdit), quando o Bash pode ter mudado a arvore
 *     (index_tree_changed) e quando passou KATANA_INDEX_SWEEP_S segundos (padrao 10; 0 = a cada chamada, como antes) desde a ultima
 *     varredura: esse prazo e a rede de seguranca enquanto nao existe vigia de arquivos (fase 4); mudancas feitas por outros programas
 *     aparecem no maximo depois dele, e todo arquivo citado numa resposta continua sendo relido do disco para a evidencia.
 *   - index_due()/index_validated(): o mesmo controle para um segundo consumidor (o indice proprio do Find, ate a fase 7 o remover).
 *   - IdxCache: ate 4 visoes abertas (CsView, SyView) por processo, reaproveitadas enquanto o MANIFEST nao muda (um stat).
 *   - contadores (index_stats_line) para KATANA_FIND_TIME: quantas varreduras ocorreram e quantas foram dispensadas.
 * Decisoes do implementador (o documento deixava abertas):
 *   - index_ensure atualiza no proprio processo (store_catalog_update), nao dispara `katana coder index` em outro processo como o
 *     documento descreve para a primeira versao: um filho que estoura o orcamento deixaria a consulta sem indice nenhum (o segmento so
 *     e publicado no fim), pior que bloquear como hoje; o filho so compensa quando existir o rastreador de niveis (fase 3). Os
 *     parametros level, prefix e budget_ms ja estao na assinatura para essa fase e por ora nao mudam nada.
 *   - o "pode ter mudado" do Bash e filtrado: comandos simples de leitura (ls, cat, grep, git status...) sem redirecionamento,
 *     pipe, ; ou && nao marcam a arvore; qualquer outro marca.
 *   - o estado fica por raiz (ate 8) e por consumidor (0 = coder, 1 = Find); a chave e o realpath. */
#ifndef _WIN32
#include <pthread.h>
enum { IDX_CODER = 0, IDX_FIND = 1, IDX_NCONS = 2, IDX_ROOTS = 8, IDX_SLOTS = 4 };
typedef struct { unsigned seen; double t; int ok; } IdxCons;
typedef struct { char root[4096]; unsigned epoch; IdxCons c[IDX_NCONS]; unsigned long tick; } IdxRoot;
static IdxRoot g_idx_root[IDX_ROOTS];
static unsigned g_idx_all_epoch;                      /* sobe quando o Bash pode ter mudado a arvore (vale para todas as raizes) */
static unsigned long g_idx_tick;
static struct { int walks, skips, touches, view_open, view_hit, kb_load, kb_hit; } g_idx_n;
static pthread_mutex_t g_idx_mx = PTHREAD_MUTEX_INITIALIZER;

static double idx_ttl(void) { const char *e = getenv("KATANA_INDEX_SWEEP_S"); return e && *e ? atof(e) : 10.0; }
/* caminho absoluto e sem links; arquivo que ainda nao existe (ou acabou de ser apagado) resolve pela pasta */
static int idx_abs(const char *p, char *out, size_t cap) {
    char t[4200];
    if (realpath(p, t)) { snprintf(out, cap, "%s", t); return 1; }
    char d[4200]; snprintf(d, sizeof d, "%s", p);
    char *sl = strrchr(d, '/');
    if (!sl) { if (!getcwd(t, sizeof t)) return 0; snprintf(out, cap, "%s/%s", t, p); return 1; }
    const char *base = sl + 1; if (sl == d) { snprintf(out, cap, "/%s", base); return 1; }
    *sl = 0; if (!realpath(d, t)) return 0;
    snprintf(out, cap, "%s/%s", t, base); return 1;
}
static IdxRoot *idx_state(const char *root, int create) {       /* chamar com g_idx_mx tomado */
    char rp[4200]; if (!realpath(root, rp)) snprintf(rp, sizeof rp, "%s", root);
    IdxRoot *old = &g_idx_root[0];
    for (int i = 0; i < IDX_ROOTS; i++) {
        if (g_idx_root[i].root[0] && !strcmp(g_idx_root[i].root, rp)) { g_idx_root[i].tick = ++g_idx_tick; return &g_idx_root[i]; }
        if (!g_idx_root[i].root[0] || g_idx_root[i].tick < old->tick) old = &g_idx_root[i];
    }
    if (!create) return NULL;
    memset(old, 0, sizeof *old); snprintf(old->root, sizeof old->root, "%s", rp); old->tick = ++g_idx_tick; return old;
}
/* 1 se o consumidor precisa varrer a arvore antes da proxima consulta */
static int index_due(const char *root, int cons) {
    pthread_mutex_lock(&g_idx_mx);
    IdxRoot *s = idx_state(root, 1); IdxCons *c = &s->c[cons];
    const double ttl = idx_ttl();
    const int due = !c->ok || c->seen != s->epoch + g_idx_all_epoch || ttl <= 0 || now_sec() - c->t > ttl;
    pthread_mutex_unlock(&g_idx_mx);
    return due;
}
static void index_validated(const char *root, int cons) {
    pthread_mutex_lock(&g_idx_mx);
    IdxRoot *s = idx_state(root, 1); s->c[cons].ok = 1; s->c[cons].seen = s->epoch + g_idx_all_epoch; s->c[cons].t = now_sec();
    pthread_mutex_unlock(&g_idx_mx);
}
/* uma ferramenta do agente gravou, criou, apagou ou renomeou este caminho: as raizes que o contem precisam ser revalidadas */
static void index_touch(const char *path) {
    if (!path || !*path) return;
    char ap[4200]; if (!idx_abs(path, ap, sizeof ap)) return;
    pthread_mutex_lock(&g_idx_mx);
    for (int i = 0; i < IDX_ROOTS; i++) {
        const size_t rl = strlen(g_idx_root[i].root);
        if (rl && !strncmp(ap, g_idx_root[i].root, rl) && (ap[rl] == '/' || !ap[rl] || rl == 1)) { g_idx_root[i].epoch++; g_idx_n.touches++; }
    }
    pthread_mutex_unlock(&g_idx_mx);
}
static void index_tree_changed(void) { pthread_mutex_lock(&g_idx_mx); g_idx_all_epoch++; g_idx_n.touches++; pthread_mutex_unlock(&g_idx_mx); }
/* o comando do Bash pode ter mudado arquivos? so comandos simples e conhecidos de leitura dizem que nao */
static int index_bash_may_change(const char *cmd) {
    static const char *const ro[] = {"ls", "cat", "head", "tail", "grep", "egrep", "rg", "wc", "pwd", "echo", "which", "file", "stat", "du", "df", "diff", "sort", "uniq", "cut", "nl", "ps", "whoami", "date", "uname", "env", "printenv", "readlink", "realpath", "basename", "dirname", "tree", "type", "id", "hostname", "sleep", "true", "false", NULL};
    if (!cmd) return 1;
    while (*cmd == ' ' || *cmd == '\t' || *cmd == '\n') cmd++;
    for (const char *p = cmd; *p; p++) if (strchr("><;&|`$(\n", *p)) return 1;
    char w[40]; size_t n = 0; while (cmd[n] && cmd[n] != ' ' && cmd[n] != '\t' && n < sizeof w - 1) { w[n] = cmd[n]; n++; } w[n] = 0;
    if (!strcmp(w, "git")) {                                 /* git de leitura */
        const char *a = cmd + n; while (*a == ' ') a++;
        return strncmp(a, "status", 6) && strncmp(a, "log", 3) && strncmp(a, "diff", 4) && strncmp(a, "show", 4) && strncmp(a, "branch", 6) && strncmp(a, "remote", 6) && strncmp(a, "rev-parse", 9);
    }
    for (int i = 0; ro[i]; i++) if (!strcmp(w, ro[i])) return 0;
    return 1;
}

/* ---- index_ensure: o catalogo do coder da raiz esta pronto para consultar? ----
 * res: created = o indice nao existia; walked = esta chamada varreu e gravou; busy = outro processo grava agora (le o que existe);
 * ms = tempo gasto aqui. Devolve 0 (pode consultar) ou -1 (nao foi possivel criar nem ler). level/prefix/budget_ms: reservados (fase 3). */
typedef struct { int created, walked, busy; double ms; } IndexRes;
static int index_ensure(const char *root, int level, const char *prefix, int budget_ms, IndexRes *res) {
    (void)level; (void)prefix; (void)budget_ms;
    IndexRes z; if (!res) res = &z; memset(res, 0, sizeof *res);
    char dir[4400], man[4500], err[300] = ""; struct stat sv; const double t0 = now_sec();
    store_locate(root, "coder", dir, sizeof dir, 0);
    snprintf(man, sizeof man, "%s/MANIFEST", dir);
    const int exists = stat(man, &sv) == 0;
    res->created = !exists;
    if (exists && !index_due(root, IDX_CODER)) { g_idx_n.skips++; return 0; }
    store_locate(root, "coder", dir, sizeof dir, 1);
    StoreScanOpts o; memset(&o, 0, sizeof o); coder_search_hook(&o); coder_sym_hook(&o, root);      /* busca e simbolos juntos: o indice nunca fica com arquivos sem simbolos */
    StoreUpdStats S; const int rc = store_catalog_update(dir, root, &o, &S, err, sizeof err);
    char si[400], yi[500]; coder_search_hook_done(&o, si, sizeof si); coder_sym_hook_done(yi, sizeof yi);
    g_idx_n.walks++; res->walked = 1; res->ms = (now_sec() - t0) * 1000;
    if (rc == 0) index_validated(root, IDX_CODER);
    else if (rc == 1) res->busy = 1;                       /* outro processo grava agora: le o que existe e tenta de novo na proxima */
    return (rc == 0 || rc == 1) ? 0 : -1;
}
static void index_stats_line(char *o, size_t cap) {
    snprintf(o, cap, "index: tree walks %d, skipped %d, tool changes %d; views opened %d, reused %d; knowledge base loads %d, reused %d",
             g_idx_n.walks, g_idx_n.skips, g_idx_n.touches, g_idx_n.view_open, g_idx_n.view_hit, g_idx_n.kb_load, g_idx_n.kb_hit);
}

/* ---- cache de visoes de leitura (CsView, SyView): ate IDX_SLOTS por processo, vale enquanto o MANIFEST nao muda ----
 * A visao do cache e alocada e e dona do StoreReader; quem consulta recebe uma copia rasa marcada `borrowed` e a devolve em close
 * (refs). Uma visao velha com consulta em andamento so e fechada na ultima devolucao. Sem vaga (todas em uso), a visao fica com quem abriu. */
typedef struct { char dir[4400]; void *view; StoreReader *rd; int refs, dead; unsigned long tick; } IdxSlot;
typedef struct { IdxSlot s[IDX_SLOTS]; void (*destroy)(void *view); } IdxCache;
static void idx_slot_free(IdxCache *c, IdxSlot *s) { if (s->view) c->destroy(s->view); memset(s, 0, sizeof *s); }
static void *idx_cache_get(IdxCache *c, const char *dir) {
    void *r = NULL;
    pthread_mutex_lock(&g_idx_mx);
    for (int i = 0; i < IDX_SLOTS && !r; i++) {
        IdxSlot *s = &c->s[i]; if (!s->view || s->dead || strcmp(s->dir, dir) != 0) continue;
        if (store_stale(s->rd)) { if (s->refs) s->dead = 1; else idx_slot_free(c, s); continue; }
        s->refs++; s->tick = ++g_idx_tick; r = s->view; g_idx_n.view_hit++;
    }
    pthread_mutex_unlock(&g_idx_mx);
    return r;
}
static int idx_cache_put(IdxCache *c, const char *dir, void *view, StoreReader *rd) {       /* 1 = o cache ficou com a visao (refs 1) */
    int ok = 0;
    pthread_mutex_lock(&g_idx_mx);
    IdxSlot *pick = NULL;
    for (int i = 0; i < IDX_SLOTS; i++) { IdxSlot *s = &c->s[i];
        if (!s->view) { pick = s; break; }
        if (!s->refs && (!pick || s->tick < pick->tick)) pick = s; }
    if (pick) { idx_slot_free(c, pick); snprintf(pick->dir, sizeof pick->dir, "%s", dir); pick->view = view; pick->rd = rd; pick->refs = 1; pick->tick = ++g_idx_tick; ok = 1; }
    if (ok) g_idx_n.view_open++;
    pthread_mutex_unlock(&g_idx_mx);
    return ok;
}
static void idx_cache_release(IdxCache *c, void *view) {
    pthread_mutex_lock(&g_idx_mx);
    for (int i = 0; i < IDX_SLOTS; i++) { IdxSlot *s = &c->s[i];
        if (s->view != view) continue;
        if (s->refs > 0) s->refs--;
        if (s->dead && !s->refs) idx_slot_free(c, s);
        break; }
    pthread_mutex_unlock(&g_idx_mx);
}
#else
static void index_touch(const char *path) { (void)path; }
static void index_tree_changed(void) {}
static int index_bash_may_change(const char *cmd) { (void)cmd; return 1; }
static int index_due(const char *root, int cons) { (void)root; (void)cons; return 1; }
static void index_validated(const char *root, int cons) { (void)root; (void)cons; }
static void index_stats_line(char *o, size_t cap) { snprintf(o, cap, "index: not available on this platform"); }
#endif
