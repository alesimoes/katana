/* ---------- coder/crawl.c: o rastreador do indice (fase 3 de docs/code-index-lifecycle.md) ----------
 * `katana coder crawl <pasta> [--prio <prefixo>] [--changed <caminho>...] [--sweep] [--stop] [--quiet]`
 * Um processo por raiz de indice, eleito por flock em <store>/CRAWL.LOCK (o nucleo solta o lock quando o processo morre, de qualquer jeito). Sob demanda:
 * quem precisa o inicia (autoindex_dir, em agent/loop.c); ele percorre a arvore em LARGURA, nivel a nivel, e SAI quando todos os niveis estao
 * cobertos e passou o tempo de assentamento (KATANA_INDEX_SETTLE_MS, 2000). Nao espera ocioso, nao e daemon. Um segundo rastreador na mesma raiz
 * entrega o pedido ao primeiro pelo socket (~/.katana/run/coder-<chave>.sock) e sai com 0.
 *
 * Niveis (campo `nivel` em StoreDoc.flags, bits 8 a 11; ver storedocs.c): N0 catalogo (arvore: nome, tamanho, data, pastas com resumo), N1 texto
 * (busca), N2 simbolos e N3 ligacoes (a analise sintatica entrega simbolos, referencias e chaves de texto de uma vez, entao N2 e N3 avancam juntos).
 * A faixa quente (arquivos pedidos por --prio/PRIO, tocados por --changed/TOUCH e arquivos que mudaram durante a rodada) passa por todos de uma vez.
 * Cada lote e um segmento novo, publicado pela troca atomica do manifesto, e leva junto a secao STS_CRAWL (andamento) e, no N0, STS_DIRS (pastas).
 *   N0: segmento so de STS_DOCS (hash 0) + STS_DIRS.     N1: STS_DOCS (hash, nivel 2) + secoes de busca; mascara os registros N0 (o mais novo vence).
 *   N2/N3 por AUMENTO: o segmento N1 inteiro e reescrito com as secoes de busca copiadas byte a byte, mais as secoes de simbolos e chaves, e o
 *   segmento antigo sai do manifesto na mesma troca. Segmento N1 que nao serve para aumento (documento mascarado por versao mais nova, ou fundido
 *   com outros niveis) tem os documentos refeitos por inteiro (busca e simbolos juntos), que e o caminho do `coder index`.
 * Retomada sem arquivo de fila: no inicio as pastas (STS_DIRS) e os documentos vigentes dizem o que falta; matar o processo perde no maximo o lote.
 *
 * Decisoes do implementador (o documento deixava abertas; a mais simples que cumpre o criterio de pronto):
 *   - lote = ate 4096 arquivos e 16 MiB de fonte (KATANA_INDEX_BATCH_MB), escolhidos pelo tamanho do catalogo (sem relogio: a composicao do lote nao depende
 *     da velocidade); o lock de escrita do store fica tomado durante o lote inteiro (nao so no commit) e solto entre lotes; `compact` e `verify` entram
 *     entre lotes (compact recebe "busy").
 *   - FASE 5, analise em paralelo: KATANA_INDEX_THREADS (padrao min(4, cpus-1), 1 com uma cpu so) threads leem e analisam os arquivos de um lote (texto do N1;
 *     simbolos, referencias e chaves do N2/N3), cada uma com o seu contexto (CsAn, buffer de leitura; o analisador nao tem estado global). UMA thread, a do
 *     rastreador, funde os resultados na ordem dos documentos e e a unica que toca o escritor do store; o conteudo do indice e o mesmo do caminho de uma
 *     thread (com 1 nao nasce thread nenhuma). Prioridade baixa (nice + ioprio ocioso) vale tambem para as threads: nascem depois e herdam.
 *   - nivel do documento na faixa 0..4; N2 e N3 gravam o mesmo valor (4). "Completo" = 4 ou 0 (registros do caminho sincrono).
 *   - fronteira, filas e ordem por banda sao recalculadas do store a cada lote (vivos + STS_DIRS); nada em disco alem do proprio store.
 *   - bandas: 0 quente; 1 codigo recente (30 dias), da raiz ou sob src/lib/app/cmd/pkg/include/internal, README e KATANA.md; 2 outro codigo; 3 doc e
 *     configuracao; 4 dados grandes, lock files, *.min.*, *.map e pastas com mais de 20 mil entradas. Dentro da banda, o mais novo primeiro.
 *   - compactacao: so entre etapas (depois do N0, se passou de 24 segmentos; no fim, acima de 8). Funde so a cauda do manifesto.
 *   - limites de D.8 implementados: profundidade (KATANA_INDEX_MAX_DEPTH, 32), pasta com mais de 20 mil entradas, total de arquivos
 *     (KATANA_INDEX_MAX_FILES, 500000) e de arquivos com texto (KATANA_INDEX_MAX_TEXT_FILES, 200000), binario, grande e ilegivel. Deteccao de gerado
 *     e minificado existe mas fica DESLIGADA (KATANA_INDEX_GENERATED=1 liga): muda o que a busca acha em relacao ao caminho sincrono.
 *   - pastas ignoradas sao registradas em STS_DIRS com o motivo e a regra (arquivo da regra misturado: .gitignore e .katanaignore aparecem como
 *     "regra de ignorar"), nao contadas; arquivos ignorados so entram na contagem da pasta. Nao feitos: STS_RULES, deteccao de ponto de montagem (a varredura
 *     sincrona tambem nao tem), N4.
 *   - a decisao de ignorar e a da varredura sincrona, a MESMA funcao (st_scan_decide, storedocs.c): overrides (`katana coder include/exclude`), .katanaignore,
 *     .gitignore, lista interna unica de ignore.c, segredos por nome (SDF_SKIP_SECRET) e por conteudo (ign_secret_sniff), include sob pasta interna (modo
 *     passagem), raizes aninhadas. O rastreador so guarda a pilha de regras por pasta e o modo de cada pasta; ao comecar (e quando o arquivo `overrides` ou
 *     um .gitignore muda) confere de novo as pastas ja conhecidas (cw_revalidate) e a pasta que passou a ser ignorada perde os arquivos catalogados.
 *     Link simbolico para pasta vira `link` no N0 (nao e seguido).
 *   - socket: PING, PRIO, TOUCH, SWEEP, SYNC, STATUS, STOP (uma linha, uma resposta). O arquivo de estado e ~/.katana/run/coder-<chave>.state. */
#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <sys/un.h>
#include <sys/syscall.h>

enum { CW_DISC = 0, CW_LISTED, CW_IGNORED, CW_NESTED, CW_LINK, CW_TOODEEP, CW_TOOMANY, CW_UNREAD, CW_GONE, CW_CHECKOUT, CW_NSTATE };
static const char *const g_cw_state[CW_NSTATE] = {"discovered", "listed", "ignored", "nested-root", "link", "too-deep", "too-many-entries", "unreadable", "gone", "separate-checkout"};
enum { CWR_NONE = 0, CWR_RULE, CWR_HEAVY, CWR_HEAVY_DEFAULT, CWR_NESTED, CWR_CHECKOUT, CWR_BIGDIR, CWR_LINK, CWR_DEPTH, CWR_FANOUT, CWR_UNREAD, CWR_GITIGNORE, CWR_KATANAIGNORE, CWR_EXCLUDED, CWR_SECRET, CWR_NREASON };
static const char *const g_cw_reason[CWR_NREASON] = {"-", "ignore rule", "built-in heavy folder", "default heavy folder (no .gitignore)", "has its own index", "separate git checkout", "huge folder not covered by .gitignore", "symbolic link", "deeper than the depth limit", "more than 20000 entries", "unreadable", "gitignore", "katanaignore", "excluded with katana coder exclude", "secret folder"};
#define CW_N0 1u            /* valores do campo de nivel (SDF_LVMASK >> 8) */
#define CW_N1 2u
#define CW_FULL 4u
#define CW_DIRW 72
#define CW_BATCH_FILES 4096
#define CW_BATCH_BYTES (16u << 20)
#define CW_FANOUT 20000
#define CW_MAXTHR 16

typedef struct {
    char *path, *rule, *doc, *langs; uint32_t plen, depth, state, reason, nfiles, ndirs, nignored, band; int64_t mtime;
    int parent, rs_ok, dirty, prev_listed; unsigned seen; StoreRuleSet rs;
    int mode, pass_reason, mode_ok; char *pass_why;       /* modo da pasta para a decisao de ignorar (0 normal, 1 passagem, 2 forcado): ver st_scan_decide */
} CwDir;
/* um arquivo de um lote: o registro, e o que as threads de analise produzem (so a thread dona do indice liga isso ao escritor) */
typedef struct { StoreDoc d; int nodata; int ok; CsDocRes cs; SyDIn sy; int has_sy; } CwItem;
typedef struct { CsAn *an; uint8_t *buf; } CwWorker;
typedef struct {
    char root[4096], dir[4400], key[40], statepath[1200], sockpath[1200]; size_t rlen; uint32_t maxsz;
    CwDir *d; int nd, cap; int *tab; uint32_t tabn; IgnOvr ovr; int ovr_ok; int64_t ovr_mt, ovr_sz;
    int threads; uint64_t batch_bytes; CwWorker wk[CW_MAXTHR];
    char **hot; int nhot, caphot; char **chg; int nchg, capchg; int sweep, stop, quiet, cmp_needed, bootstrap;
    unsigned listing; uint64_t epoch; double t0, t_state; int lockfd, sockfd;
    uint32_t maxdepth; uint64_t maxfiles, maxtext; int generated; char limit[120];
    uint64_t total, skipped, n1, n2; uint32_t frontier; int nseg; char activity[100]; uint64_t gen;
    int announced[3], work;
    struct { int fd; int level; char prefix[600]; double deadline; } sync[8]; int nsync;
} Cw;
static Cw g_cw;
static void cw_say(const char *fmt, ...) {
    if (g_cw.quiet) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); fflush(stdout);
}

static const void *cw_memrchr(const void *s, int c, size_t n) { const char *p = (const char *)s; while (n--) if ((unsigned char)p[n] == (unsigned char)c) return p + n; return NULL; }
/* ---------- pastas conhecidas: tabela por caminho relativo ("" = a raiz) ---------- */
static char *cw_dup(const char *s, size_t n) { char *p = (char *)malloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }
static int cw_dir_find(const char *p, size_t l) {
    if (!g_cw.tabn) return -1;
    uint32_t h = (uint32_t)store_hash64(p, l, 3) & (g_cw.tabn - 1);
    while (g_cw.tab[h] >= 0) { const CwDir *D = &g_cw.d[g_cw.tab[h]]; if (D->plen == l && !memcmp(D->path, p, l)) return g_cw.tab[h]; h = (h + 1) & (g_cw.tabn - 1); }
    return -1;
}
static void cw_tab_rebuild(void) {
    uint32_t n = 64; while (n < (uint32_t)g_cw.cap * 2) n *= 2;
    free(g_cw.tab); g_cw.tab = (int *)malloc(sizeof(int) * n); g_cw.tabn = n;
    for (uint32_t i = 0; i < n; i++) g_cw.tab[i] = -1;
    for (int i = 0; i < g_cw.nd; i++) { CwDir *D = &g_cw.d[i]; uint32_t h = (uint32_t)store_hash64(D->path, D->plen, 3) & (n - 1); while (g_cw.tab[h] >= 0) h = (h + 1) & (n - 1); g_cw.tab[h] = i; }
}
static int cw_dir_add(const char *p, size_t l, int parent, uint32_t state) {
    int f = cw_dir_find(p, l); if (f >= 0) return f;
    if (g_cw.nd == g_cw.cap) { g_cw.cap = g_cw.cap ? g_cw.cap * 2 : 256; g_cw.d = (CwDir *)realloc(g_cw.d, sizeof(CwDir) * (size_t)g_cw.cap); cw_tab_rebuild(); }
    CwDir *D = &g_cw.d[g_cw.nd]; memset(D, 0, sizeof *D);
    D->path = cw_dup(p, l); D->plen = (uint32_t)l; D->parent = parent; D->state = state; D->band = 1; D->dirty = 1;
    D->depth = 0; for (size_t i = 0; i < l; i++) if (p[i] == '/') D->depth++; if (l) D->depth++;
    uint32_t h = (uint32_t)store_hash64(p, l, 3) & (g_cw.tabn - 1); while (g_cw.tab[h] >= 0) h = (h + 1) & (g_cw.tabn - 1);
    g_cw.tab[h] = g_cw.nd; return g_cw.nd++;
}
static int cw_parent_of(const char *p, size_t l) {      /* indice da pasta pai de um caminho relativo (cria placeholders listados se faltar) */
    if (!l) return -1;
    const char *sl = (const char *)cw_memrchr(p, '/', l); size_t pl = sl ? (size_t)(sl - p) : 0;
    int f = cw_dir_find(p, pl); if (f >= 0) return f;
    return cw_dir_add(p, pl, cw_parent_of(p, pl), CW_LISTED);
}
static void cw_abs(const CwDir *D, char *out, size_t cap) { if (D->plen) snprintf(out, cap, "%s/%s", g_cw.root, D->path); else snprintf(out, cap, "%s", g_cw.root); }
static int cw_under(const char *path, size_t pl, const char *pre, size_t prl) { return pl == prl ? !memcmp(path, pre, pl) : (pl > prl && !memcmp(path, pre, prl) && path[prl] == '/'); }

/* ---------- faixa quente ---------- */
static void cw_hot_add(const char *rel) {
    for (int i = 0; i < g_cw.nhot; i++) if (!strcmp(g_cw.hot[i], rel)) return;
    if (g_cw.nhot == g_cw.caphot) { g_cw.caphot = g_cw.caphot ? g_cw.caphot * 2 : 16; g_cw.hot = (char **)realloc(g_cw.hot, sizeof(char *) * (size_t)g_cw.caphot); }
    g_cw.hot[g_cw.nhot++] = strdup(rel);
}
static int cw_is_hot(const char *p, size_t l) {
    for (int i = 0; i < g_cw.nhot; i++) { size_t hl = strlen(g_cw.hot[i]); if (!hl || cw_under(p, l, g_cw.hot[i], hl)) return 1; }
    return 0;
}
static void cw_chg_add(const char *rel) {
    if (g_cw.nchg == g_cw.capchg) { g_cw.capchg = g_cw.capchg ? g_cw.capchg * 2 : 16; g_cw.chg = (char **)realloc(g_cw.chg, sizeof(char *) * (size_t)g_cw.capchg); }
    g_cw.chg[g_cw.nchg++] = strdup(rel);
}
/* caminho dado pelo usuario (absoluto ou relativo ao diretorio atual) para relativo a raiz; 0 se fica fora */
static int cw_rel(const char *arg, char *out, size_t cap) {
    char a[4400]; if (arg[0] == '/') snprintf(a, sizeof a, "%s", arg); else { char c[4000]; if (!getcwd(c, sizeof c)) return 0; snprintf(a, sizeof a, "%s/%s", c, arg); }
    char r[4400]; if (!realpath(a, r)) { char d[4400]; snprintf(d, sizeof d, "%s", a); char *sl = strrchr(d, '/'); if (!sl || sl == d) return 0; *sl = 0; if (!realpath(d, r)) return 0; size_t n = strlen(r); snprintf(r + n, sizeof r - n, "%s", strrchr(a, '/')); }
    const size_t rl = strlen(g_cw.root);
    if (strncmp(r, g_cw.root, rl) != 0 || (r[rl] != '/' && r[rl] != 0)) return 0;
    snprintf(out, cap, "%s", r[rl] ? r + rl + 1 : ""); return 1;
}

/* ---------- classificacao de arquivos: banda ---------- */
static const char *cw_ext(const char *name) { const char *dot = strrchr(name, '.'), *sl = strrchr(name, '/'); if (!dot || (sl && dot < sl) || dot == name || dot == sl + 1) return ""; return dot + 1; }
static int cw_in(const char *w, const char *const *l) { for (int i = 0; l[i]; i++) if (!strcasecmp(w, l[i])) return 1; return 0; }
static const char *const g_cw_src[] = {"c", "h", "cc", "cpp", "cxx", "hpp", "hh", "cs", "java", "kt", "kts", "go", "rs", "py", "rb", "php", "js", "mjs", "cjs", "ts", "jsx", "tsx", "swift", "scala", "dart", "lua", "sh", "bash", "sql", "m", "mm", "zig", "ex", "exs", "erl", "hs", "ml", "clj", "lisp", "el", "r", "jl", "vue", "svelte", "glsl", "comp", "vert", "frag", "cu", "cuh", "bat", "ps1", "pl", NULL};
static const char *const g_cw_data[] = {"json", "csv", "tsv", "xml", NULL};
static int cw_generated_name(const char *name) {
    const char *e = cw_ext(name); size_t n = strlen(name);
    if (!strcasecmp(e, "lock") || !strcasecmp(e, "map") || strstr(name, ".min.")) return 1;
    return (n >= 9 && (!strcmp(name, "yarn.lock") || !strcmp(name, "Cargo.lock"))) || !strcmp(name, "package-lock.json") || !strcmp(name, "pnpm-lock.yaml");
}
static uint32_t cw_band(const char *path, uint32_t pl, uint64_t size, int64_t mtime_ns) {
    if (cw_is_hot(path, pl)) return 0;
    const char *name = (const char *)cw_memrchr(path, '/', pl); name = name ? name + 1 : path;
    char nm[300]; size_t nl = pl - (size_t)(name - path); if (nl >= sizeof nm) nl = sizeof nm - 1; memcpy(nm, name, nl); nm[nl] = 0;
    const char *e = cw_ext(nm);
    if (!strncasecmp(nm, "README", 6) || !strcmp(nm, "KATANA.md")) return 1;
    if (cw_generated_name(nm) || (cw_in(e, g_cw_data) && size > (256u << 10))) return 4;
    if (cw_in(e, g_cw_src)) {
        static const char *const top[] = {"src", "lib", "app", "cmd", "pkg", "include", "internal", NULL};
        int root = !memchr(path, '/', pl); char first[40]; size_t k = 0; while (k < pl && path[k] != '/' && k < sizeof first - 1) { first[k] = path[k]; k++; } first[k] = 0;
        const int64_t now = (int64_t)time(NULL) * 1000000000ll;
        if (root || (k < pl && cw_in(first, top)) || now - mtime_ns < 30ll * 86400 * 1000000000ll) return 1;
        return 2;
    }
    return 3;
}

/* ---------- regras de ignorar por pasta: a pilha de .gitignore/.katanaignore em cache por pasta e a decisao de storedocs.c (st_scan_decide) ---------- */
static void cw_rules_drop(CwDir *D) {
    if (!D->rs_ok) return;
    for (int i = 0; i < D->rs.n; i++) { free(D->rs.r[i].pat); free(D->rs.r[i].orig); }
    free(D->rs.r); memset(&D->rs, 0, sizeof D->rs); D->rs_ok = 0;
}
static StoreRuleSet *cw_rules(int di) {
    CwDir *D = &g_cw.d[di];
    if (!D->rs_ok) {
        D->rs_ok = 1; memset(&D->rs, 0, sizeof D->rs);
        char a[4300], f[4400]; cw_abs(D, a, sizeof a); D->rs.base = strlen(a);
        snprintf(f, sizeof f, "%s/.gitignore", a); st_rules_load(&D->rs, f);
        snprintf(f, sizeof f, "%s/.katanaignore", a); st_rules_load(&D->rs, f);
    }
    return D->rs.n ? &D->rs : NULL;
}
static void cw_stack(int di, StoreScan *S);
/* modo da pasta (0 normal, 1 passagem, 2 forcado): a listagem do pai o grava; depois de retomar do store recalcula pela decisao do pai */
static void cw_mode_of(int di) {
    CwDir *D = &g_cw.d[di]; if (D->mode_ok) return;
    D->mode_ok = 1; D->mode = 0; D->pass_reason = 0; free(D->pass_why); D->pass_why = NULL;
    if (di == 0 || D->parent < 0) return;
    const int pi = D->parent; cw_mode_of(pi);
    StoreScan S; cw_stack(pi, &S);
    char full[4400]; cw_abs(&g_cw.d[di], full, sizeof full);
    const char *nm = strrchr(g_cw.d[di].path, '/'); nm = nm ? nm + 1 : g_cw.d[di].path;
    int nmode = 0, reason = 0; char rule[420];
    st_scan_decide(&S, full, g_cw.rlen + 1, nm, 1, &nmode, &reason, rule, sizeof rule);
    D = &g_cw.d[di]; D->mode = nmode;
    if (nmode == 1) { D->pass_reason = S.pass_reason; D->pass_why = strdup(S.pass_why); }
}
static void cw_stack(int di, StoreScan *S) {
    int chain[80]; int n = 0;
    cw_mode_of(di);
    for (int x = di; x >= 0 && n < 80; x = g_cw.d[x].parent) chain[n++] = x;
    memset(S, 0, sizeof *S);
    for (int i = n - 1; i >= 0; i--) { StoreRuleSet *R = cw_rules(chain[i]); if (R && S->nrs < ST_MAXRS) S->rs[S->nrs++] = *R; }
    const CwDir *D = &g_cw.d[di];
    S->ovr = g_cw.ovr_ok ? &g_cw.ovr : NULL; S->mode = D->mode; S->pass_reason = D->pass_reason;
    if (D->pass_why) snprintf(S->pass_why, sizeof S->pass_why, "%s", D->pass_why);
}
/* esquece regras e modos em cache (um .gitignore/.katanaignore ou o arquivo overrides mudou) */
static void cw_rules_reset(void) { for (int i = 0; i < g_cw.nd; i++) { cw_rules_drop(&g_cw.d[i]); g_cw.d[i].mode_ok = 0; } }
static uint32_t cw_reason_of(int igr) {
    switch (igr) { case IGR_GITIGNORE: return CWR_GITIGNORE; case IGR_KATANAIGNORE: return CWR_KATANAIGNORE; case IGR_HEAVY: return CWR_HEAVY; case IGR_EXCLUDED: return CWR_EXCLUDED;
        case IGR_SECRET: return CWR_SECRET; case IGR_NESTED: return CWR_NESTED; case IGR_CHECKOUT: return CWR_CHECKOUT; default: return CWR_RULE; }
}
static int cw_blocked(uint32_t st) { return st == CW_IGNORED || st == CW_NESTED || st == CW_CHECKOUT || st == CW_TOODEEP || st == CW_GONE; }
/* le o arquivo de overrides (ao lado do store); devolve 1 se mudou desde a ultima leitura */
static int cw_ovr_check(int force) {
    char f[4600]; ign_ovr_path(g_cw.dir, f, sizeof f); struct stat st; int64_t mt = 0, sz = -1;
    if (stat(f, &st) == 0) { mt = (int64_t)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec; sz = (int64_t)st.st_size; }
    if (!force && g_cw.ovr_ok && mt == g_cw.ovr_mt && sz == g_cw.ovr_sz) return 0;
    if (g_cw.ovr_ok) ign_ovr_free(&g_cw.ovr);
    ign_ovr_load(&g_cw.ovr, g_cw.dir); g_cw.ovr_ok = 1; g_cw.ovr_mt = mt; g_cw.ovr_sz = sz;
    cw_rules_reset(); return 1;
}

/* ---------- listagem de uma pasta ---------- */
typedef struct { StoreDoc *d; int n, cap; char **tomb; int nt, capt; } CwOut;
static void cw_out_doc(CwOut *O, const StoreDoc *d, const char *rel) {
    if (O->n == O->cap) { O->cap = O->cap ? O->cap * 2 : 1024; O->d = (StoreDoc *)realloc(O->d, sizeof(StoreDoc) * (size_t)O->cap); }
    O->d[O->n] = *d; O->d[O->n].path = cw_dup(rel, d->plen); O->n++;
}
static void cw_out_tomb(CwOut *O, const char *p, size_t l) {
    if (O->nt == O->capt) { O->capt = O->capt ? O->capt * 2 : 64; O->tomb = (char **)realloc(O->tomb, sizeof(char *) * (size_t)O->capt); }
    O->tomb[O->nt++] = cw_dup(p, l);
}
static int cw_cmp_doc(const void *a, const void *b) { const StoreDoc *x = (const StoreDoc *)a, *y = (const StoreDoc *)b; return st_cmp_bytes(x->path, x->plen, y->path, y->plen); }
static int cw_cmp_strp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static void cw_out_free(CwOut *O) { for (int i = 0; i < O->n; i++) free((void *)O->d[i].path); free(O->d); for (int i = 0; i < O->nt; i++) free(O->tomb[i]); free(O->tomb); memset(O, 0, sizeof *O); }
static void cw_mark_gone(int di, CwOut *O, StoreLiveDoc **lv, int nlv);
static void cw_drop_below(int di, CwOut *O, StoreLiveDoc **lv, int nlv);
/* primeira linha util de README/KATANA.md/index.md, ate 160 bytes */
static char *cw_doc_line(const char *abs) {
    FILE *f = fopen(abs, "rb"); if (!f) return NULL;
    char b[2048]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    for (char *p = b; *p; ) {
        char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        while (l && (*p == '#' || *p == ' ' || *p == '\t' || *p == '*' || *p == '>' || *p == '-' || *p == '\r')) { p++; l--; }
        while (l && (p[l - 1] == '\r' || p[l - 1] == ' ' || p[l - 1] == '\t')) l--;
        if (l >= 3 && !memchr(p, 0, l) && strncmp(p, "```", 3) != 0 && strncmp(p, "---", 3) != 0 && p[0] != '<') { if (l > 160) l = 160; return cw_dup(p, l); }
        if (!e) break;
        p = e + 1;
    }
    return NULL;
}
static int cw_lower_bound(StoreLiveDoc **lv, int n, const char *pre, size_t pl) {
    int lo = 0, hi = n;
    while (lo < hi) { int m = (lo + hi) / 2; const StoreDoc *d = &lv[m]->d; if (st_cmp_bytes(d->path, d->plen, pre, pl) < 0) lo = m + 1; else hi = m; }
    return lo;
}
static void cw_list_dir(int di, CwOut *O, StoreLive *L, StoreLiveDoc **lv, int nlv) {
    CwDir *D = &g_cw.d[di]; char abs[4300]; cw_abs(D, abs, sizeof abs);
    const int relist = D->prev_listed; D->prev_listed = 0; D->dirty = 1;
    struct stat ds; if (lstat(abs, &ds) == 0) D->mtime = (int64_t)ds.st_mtim.tv_sec * 1000000000ll + ds.st_mtim.tv_nsec;
    DIR *dh = opendir(abs);
    if (!dh) { D->state = CW_UNREAD; D->reason = CWR_UNREAD; free(D->rule); D->rule = strdup(strerror(errno)); return; }
    if (D->state != CW_TOOMANY) D->state = CW_LISTED;
    StoreScan S; cw_stack(di, &S);
    const unsigned lid = ++g_cw.listing;
    char **names = NULL; int nn = 0, capn = 0; uint32_t nfiles = 0, ndirs = 0, nign = 0, nent = 0;
    char langs[16][12]; int lcount[16]; int nl = 0; char *docname = NULL; int docrank = 99; char docabs[4400] = "";
    struct dirent *de; const size_t pl = D->plen;
    while ((de = readdir(dh))) {
        const char *nm = de->d_name; if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        nent++;
        const size_t nlen = strlen(nm); if (pl + 1 + nlen + 1 >= 4000) continue;
        char rel[4100]; if (pl) { memcpy(rel, D->path, pl); rel[pl] = '/'; memcpy(rel + pl + 1, nm, nlen + 1); } else memcpy(rel, nm, nlen + 1);
        const size_t rl = pl ? pl + 1 + nlen : nlen;
        char full[4400]; snprintf(full, sizeof full, "%s/%s", abs, nm);
        struct stat st; if (lstat(full, &st) != 0) continue;
        const int isd = S_ISDIR(st.st_mode);
        if (S_ISLNK(st.st_mode)) {
            struct stat t; if (stat(full, &t) == 0 && S_ISDIR(t.st_mode)) {
                int c = cw_dir_add(rel, rl, di, CW_LINK); D = &g_cw.d[di]; CwDir *C = &g_cw.d[c]; C->state = CW_LINK; C->reason = CWR_LINK; C->seen = lid; C->dirty = 1;
                char tg[600]; ssize_t k = readlink(full, tg, sizeof tg - 1); if (k > 0) { tg[k] = 0; free(C->rule); C->rule = strdup(tg); } ndirs++;
            }
            continue;
        }
        if (!isd && !S_ISREG(st.st_mode)) continue;
        {   /* a decisao de ignorar e a da varredura sincrona (st_scan_decide): overrides, .katanaignore, .gitignore, lista interna, segredos, raiz aninhada */
            int nmode = 0, ir = 0; char rr[420];
            const int ign = st_scan_decide(&S, full, g_cw.rlen + 1, nm, isd, &nmode, &ir, rr, sizeof rr);
            if (ign == 2) continue;                         /* .git e afins: nem entram nem contam */
            if (!isd) { if (ign) { nign++; continue; } goto file_entry; }
            uint32_t state = CW_DISC, reason = CWR_NONE; const char *rule = NULL;
            if (ign) { state = ir == IGR_NESTED ? CW_NESTED : ir == IGR_CHECKOUT ? CW_CHECKOUT : CW_IGNORED; reason = cw_reason_of(ir); rule = rr; }
            else if (D->depth + 1 > g_cw.maxdepth) { state = CW_TOODEEP; reason = CWR_DEPTH; }
            int c = cw_dir_find(rel, rl); const int isnew = c < 0;
            if (isnew) { c = cw_dir_add(rel, rl, di, state); D = &g_cw.d[di]; }
            CwDir *C = &g_cw.d[c]; C->seen = lid; C->parent = di;
            if (!ign) { C->mode = nmode; C->mode_ok = 1; free(C->pass_why); C->pass_why = nmode == 1 ? strdup(S.pass_why) : NULL; C->pass_reason = nmode == 1 ? S.pass_reason : 0; }
            const int was_blocked = cw_blocked(C->state);
            if (isnew || (state != CW_DISC && C->state != state) || (state == CW_DISC && was_blocked)) {
                const int now_blocked = state == CW_IGNORED || state == CW_NESTED || state == CW_CHECKOUT;
                if (!isnew && now_blocked && !was_blocked) cw_drop_below(c, O, lv, nlv);         /* passou a ser ignorada: o que ja estava catalogado dentro dela sai */
                C->state = state; C->reason = reason; free(C->rule); C->rule = rule ? strdup(rule) : NULL; C->dirty = 1; }
            if (state == CW_DISC) ndirs++;
            continue;
        }
        file_entry:;
        nfiles++;
        if (nn == capn) { capn = capn ? capn * 2 : 64; names = (char **)realloc(names, sizeof(char *) * (size_t)capn); }
        names[nn++] = cw_dup(rel, rl);
        const char *ex = cw_ext(nm);
        if (*ex && strlen(ex) < 12) { int k = 0; while (k < nl && strcasecmp(langs[k], ex)) k++; if (k == nl && nl < 16) { snprintf(langs[nl], 12, "%s", ex); lcount[nl++] = 0; } if (k < nl) lcount[k]++; }
        { int rk = !strcmp(nm, "KATANA.md") ? 0 : !strncasecmp(nm, "README", 6) ? 1 : !strcasecmp(nm, "index.md") ? 2 : 99; if (rk < docrank && st.st_size < (256 << 10)) { docrank = rk; snprintf(docabs, sizeof docabs, "%s", full); } }
        const int64_t mt = (int64_t)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
        StoreLiveDoc *l = g_cw.cmp_needed ? store_live_find(L, rel, rl) : NULL;
        if (l && !l->dead && l->d.size == (uint64_t)st.st_size && l->d.mtime == mt) continue;       /* ja catalogado, igual: o nivel dele e outro assunto */
        if (g_cw.total + (uint64_t)O->n >= g_cw.maxfiles) { if (!g_cw.limit[0]) snprintf(g_cw.limit, sizeof g_cw.limit, "index limited to %llu files", (unsigned long long)g_cw.maxfiles); continue; }
        StoreDoc d; memset(&d, 0, sizeof d); d.plen = (uint32_t)rl; d.size = (uint64_t)st.st_size; d.mtime = mt;
        d.flags = CW_N0 << 8;
        if (ign_secret_name(rel)) d.flags |= SDF_SKIP_SECRET; else if (st_binary_ext(nm)) d.flags |= SDF_SKIP_BINARY; else if (d.size > g_cw.maxsz) d.flags |= SDF_SKIP_BIG;
        else if (g_cw.generated && cw_generated_name(nm)) d.flags |= SDF_GENERATED;       /* sem leitura; o N1 ainda indexa o caminho e o nome, como no caminho sincrono */
        cw_out_doc(O, &d, rel);
        if (relist && l && !l->dead) cw_hot_add(rel);              /* mudou depois de catalogado: faixa quente */
        else if (relist && !l) cw_hot_add(rel);
    }
    closedir(dh); D = &g_cw.d[di];
    if (nent > CW_FANOUT) { D->state = CW_TOOMANY; D->reason = CWR_FANOUT; }
    if (docrank < 99) { free(D->doc); D->doc = cw_doc_line(docabs); }
    { char b[80] = ""; int used[16] = {0};
      for (int r = 0; r < 3; r++) { int best = -1; for (int k = 0; k < nl; k++) if (!used[k] && (best < 0 || lcount[k] > lcount[best])) best = k; if (best < 0) break; used[best] = 1; size_t bl = strlen(b); snprintf(b + bl, sizeof b - bl, "%s%s", bl ? "," : "", langs[best]); }
      free(D->langs); D->langs = b[0] ? strdup(b) : NULL; }
    D->nfiles = nfiles; D->ndirs = ndirs; D->nignored = nign; (void)docname;
    /* filhos conhecidos que sumiram, e arquivos catalogados que sumiram ou passaram a ser ignorados */
    for (int i = 0; i < g_cw.nd; i++) { CwDir *C = &g_cw.d[i]; if (C->parent == di && i != di && C->seen != lid && C->state != CW_GONE) cw_mark_gone(i, O, lv, nlv); }
    if (g_cw.cmp_needed && nlv) {
        qsort(names, (size_t)nn, sizeof *names, cw_cmp_strp);
        char pre[4100]; size_t prl = 0; if (pl) { memcpy(pre, D->path, pl); pre[pl] = '/'; prl = pl + 1; }
        for (int i = cw_lower_bound(lv, nlv, pre, prl); i < nlv; i++) {
            const StoreDoc *x = &lv[i]->d; if (x->plen < prl || memcmp(x->path, pre, prl) != 0) break;
            if (memchr(x->path + prl, '/', x->plen - prl)) continue;
            char t[4100]; if (x->plen >= sizeof t) continue; memcpy(t, x->path, x->plen); t[x->plen] = 0;
            if (!bsearch(&(const char *){t}, names, (size_t)nn, sizeof *names, cw_cmp_strp)) cw_out_tomb(O, x->path, x->plen);
        }
    }
    for (int i = 0; i < nn; i++) free(names[i]);
    free(names); (void)L;
}
/* a pasta sumiu: ela e as de dentro ficam `gone` e os arquivos catalogados debaixo dela ganham tombstone */
static void cw_mark_gone(int di, CwOut *O, StoreLiveDoc **lv, int nlv) {
    const size_t pl = g_cw.d[di].plen; char *pre = cw_dup(g_cw.d[di].path, pl);
    for (int i = 0; i < g_cw.nd; i++) { CwDir *C = &g_cw.d[i]; if (C->state != CW_GONE && cw_under(C->path, C->plen, pre, pl)) { C->state = CW_GONE; C->dirty = 1; } }
    if (nlv) { char p2[4200]; snprintf(p2, sizeof p2, "%s/", pre); size_t p2l = strlen(p2);
        for (int i = cw_lower_bound(lv, nlv, p2, p2l); i < nlv; i++) { const StoreDoc *x = &lv[i]->d; if (x->plen < p2l || memcmp(x->path, p2, p2l) != 0) break; cw_out_tomb(O, x->path, x->plen); } }
    free(pre);
}
/* a pasta passou a ser ignorada (override, regra): ela fica com o novo estado, o que esta dentro some (pastas `gone`, arquivos com tombstone) */
static void cw_drop_below(int di, CwOut *O, StoreLiveDoc **lv, int nlv) {
    const size_t pl = g_cw.d[di].plen; char *pre = cw_dup(g_cw.d[di].path, pl);
    for (int i = 0; i < g_cw.nd; i++) { CwDir *C = &g_cw.d[i]; if (i != di && C->state != CW_GONE && C->plen > pl && cw_under(C->path, C->plen, pre, pl)) { C->state = CW_GONE; C->dirty = 1; } }
    if (nlv) { char p2[4200]; snprintf(p2, sizeof p2, "%s/", pre); size_t p2l = strlen(p2);
        for (int i = cw_lower_bound(lv, nlv, p2, p2l); i < nlv; i++) { const StoreDoc *x = &lv[i]->d; if (x->plen < p2l || memcmp(x->path, p2, p2l) != 0) break; cw_out_tomb(O, x->path, x->plen); } }
    free(pre);
}

/* ---------- secoes STS_DIRS e STS_CRAWL ----------
 * STS_DIRS (versao 1): tabela de largura 72 ordenada pelos bytes do caminho (a raiz e o caminho vazio) + pool de textos. Registro: u32 caminho (off, tam),
 * profundidade, estado, motivo, regra (off, tam), resumo (off, tam), linguagens (off, tam), arquivos, pastas, ignorados, banda, reservado, u64 mtime. */
static void cw_dirs_build(StoreBuf *b, CwDir **v, int n) {
    store_table_begin(b, (uint32_t)n, CW_DIRW);
    StoreBuf pool = {0};
    for (int i = 0; i < n; i++) {
        const CwDir *D = v[i]; uint32_t o[4], l[4]; const char *s[4] = {D->path, D->rule ? D->rule : "", D->doc ? D->doc : "", D->langs ? D->langs : ""};
        for (int k = 0; k < 4; k++) { o[k] = (uint32_t)pool.n; l[k] = (uint32_t)strlen(s[k]); sb_bytes(&pool, s[k], l[k]); }
        sb_u32(b, o[0]); sb_u32(b, l[0]); sb_u32(b, D->depth); sb_u32(b, D->state); sb_u32(b, D->reason); sb_u32(b, o[1]); sb_u32(b, l[1]); sb_u32(b, o[2]); sb_u32(b, l[2]);
        sb_u32(b, o[3]); sb_u32(b, l[3]); sb_u32(b, D->nfiles); sb_u32(b, D->ndirs); sb_u32(b, D->nignored); sb_u32(b, D->band); sb_u32(b, 0); sb_u64(b, (uint64_t)D->mtime);
    }
    sb_bytes(b, pool.p, pool.n); sb_free(&pool);
}
typedef struct { StoreTable t; const uint8_t *pool; uint64_t poolsize; } CwDirs;
static int cw_dirs_open(const StoreSection *s, CwDirs *d) {
    memset(d, 0, sizeof *d);
    if (!s || !store_table_open(s, &d->t) || d->t.w != CW_DIRW) { d->t.n = 0; return 0; }
    uint64_t head = 8 + (uint64_t)d->t.n * d->t.w; if (head > s->size) { d->t.n = 0; return 0; }
    d->pool = s->data + head; d->poolsize = s->size - head; return 1;
}
static const char *cw_dirs_str(const CwDirs *d, const uint8_t *r, int k, uint32_t *len) {      /* k: 0 caminho, 1 regra, 2 resumo, 3 linguagens */
    static const int at[4] = {0, 20, 28, 36};
    uint32_t off = st_get_u32(r + at[k]), l = st_get_u32(r + at[k] + 4);
    if ((uint64_t)off + l > d->poolsize) { *len = 0; return ""; }
    *len = l; return (const char *)d->pool + off;
}
static void cw_dirs_fill(CwDir *D, const CwDirs *d, const uint8_t *r) {
    uint32_t l; D->depth = st_get_u32(r + 8); D->state = st_get_u32(r + 12); D->reason = st_get_u32(r + 16); if (D->state >= CW_NSTATE) D->state = CW_DISC;
    free(D->rule); free(D->doc); free(D->langs);
    const char *s = cw_dirs_str(d, r, 1, &l); D->rule = l ? cw_dup(s, l) : NULL;
    s = cw_dirs_str(d, r, 2, &l); D->doc = l ? cw_dup(s, l) : NULL;
    s = cw_dirs_str(d, r, 3, &l); D->langs = l ? cw_dup(s, l) : NULL;
    D->nfiles = st_get_u32(r + 44); D->ndirs = st_get_u32(r + 48); D->nignored = st_get_u32(r + 52); D->band = st_get_u32(r + 56); D->mtime = (int64_t)st_get_u64(r + 64);
}
static int cw_cmp_dirp(const void *a, const void *b) { const CwDir *x = *(CwDir *const *)a, *y = *(CwDir *const *)b; return strcmp(x->path, y->path); }
/* STS_CRAWL (versao 1), 256 bytes: u32 "KCRW", u32 versao, u64 epoca, u64 inicio, u64 agora, u64 total, u64 ignorados-por-tipo, u64 N1, u64 N2/N3,
 * u32 pastas por listar, u32 flags (1 = todos os niveis completos), u64 geracao anterior, texto do limite atingido (120 bytes) */
static void cw_crawl_build(StoreBuf *b, int complete) {
    const size_t at = b->n; const uint64_t el = g_cw.total - g_cw.skipped; complete = !g_cw.frontier && g_cw.total && g_cw.n1 >= el && g_cw.n2 >= el;
    sb_u32(b, 0x5743524Bu); sb_u32(b, 1); sb_u64(b, g_cw.epoch); sb_u64(b, (uint64_t)g_cw.t0); sb_u64(b, (uint64_t)time(NULL)); sb_u64(b, g_cw.total); sb_u64(b, g_cw.skipped); sb_u64(b, g_cw.n1); sb_u64(b, g_cw.n2);
    sb_u32(b, g_cw.frontier); sb_u32(b, complete ? 1u : 0u); sb_u64(b, g_cw.gen);
    char lim[120]; memset(lim, 0, sizeof lim); snprintf(lim, sizeof lim, "%s", g_cw.limit); sb_bytes(b, lim, sizeof lim);
    while (b->n - at < 256) sb_u8(b, 0);
}
static int cw_crawl_parse(const StoreSection *s, uint64_t *epoch, uint64_t *when, uint64_t out[4], uint32_t *frontier, uint32_t *flags, char *lim, size_t lcap) {
    if (!s || s->size < 256 || st_get_u32(s->data) != 0x5743524Bu) return 0;
    *epoch = st_get_u64(s->data + 8); *when = st_get_u64(s->data + 24); for (int i = 0; i < 4; i++) out[i] = st_get_u64(s->data + 32 + 8 * i);
    *frontier = st_get_u32(s->data + 64); *flags = st_get_u32(s->data + 68); snprintf(lim, lcap, "%.119s", (const char *)s->data + 80); return 1;
}
typedef struct { const uint8_t *r; const CwDirs *d; const char *p; uint32_t l; int seg; } CwMEnt;
static int cw_cmp_ment(const void *a, const void *b) {
    const CwMEnt *x = (const CwMEnt *)a, *y = (const CwMEnt *)b; int c = st_cmp_bytes(x->p, x->l, y->p, y->l);
    return c ? c : (x->seg < y->seg ? -1 : x->seg > y->seg);       /* mesmo caminho: do segmento mais antigo ao mais novo */
}
static int cw_merge_dirs(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) {
    CwDirs *ds = (CwDirs *)calloc((size_t)c->nseg, sizeof *ds); size_t tot = 0;
    for (int s = 0; s < c->nseg; s++) { cw_dirs_open(store_seg_section(c->seg[s], STS_DIRS), &ds[s]); tot += ds[s].t.n; }
    CwMEnt *e = (CwMEnt *)malloc(sizeof *e * (tot + 1)); size_t n = 0;
    for (int s = 0; s < c->nseg; s++) for (uint32_t i = 0; i < ds[s].t.n; i++) { CwMEnt *x = &e[n++]; x->r = store_table_at(&ds[s].t, i); x->d = &ds[s]; x->p = cw_dirs_str(&ds[s], x->r, 0, &x->l); x->seg = s; }
    qsort(e, n, sizeof *e, cw_cmp_ment);
    CwDir *tmp = (CwDir *)calloc(n + 1, sizeof *tmp); CwDir **v = (CwDir **)malloc(sizeof *v * (n + 1)); int k = 0;
    for (size_t i = 0; i < n; i++) {
        if (i + 1 < n && e[i + 1].l == e[i].l && !memcmp(e[i + 1].p, e[i].p, e[i].l)) continue;       /* a ultima entrada de cada caminho vale */
        CwDir *D = &tmp[k]; D->path = cw_dup(e[i].p, e[i].l); D->plen = e[i].l; cw_dirs_fill(D, e[i].d, e[i].r); v[k] = D; k++;
    }
    *nrec = 0;
    if (k) { cw_dirs_build(out, v, k); *nrec = (uint32_t)k; }
    for (int i = 0; i < k; i++) { free(tmp[i].path); free(tmp[i].rule); free(tmp[i].doc); free(tmp[i].langs); }
    free(tmp); free(v); free(e); free(ds); return 0;
}
static int cw_merge_crawl(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) {
    *nrec = 0;
    for (int s = c->nseg - 1; s >= 0; s--) { const StoreSection *x = store_seg_section(c->seg[s], STS_CRAWL); if (x && x->size >= 256) { sb_bytes(out, x->data, 256); *nrec = 1; return 0; } }
    return 0;
}
static void coder_crawl_register(void) { store_register_merger(STS_DIRS, 1, cw_merge_dirs); store_register_merger(STS_CRAWL, 1, cw_merge_crawl); }
static int coder_crawl_verify_seg(const StoreSeg *s, char *err, size_t ecap) {
    const StoreSection *sd = store_seg_section(s, STS_DIRS), *sc = store_seg_section(s, STS_CRAWL);
    if (sc && (sc->size != 256 || st_get_u32(sc->data) != 0x5743524Bu)) { snprintf(err, ecap, "crawl: registro de andamento malformado"); return -1; }
    if (!sd) return 0;
    CwDirs d; if (!cw_dirs_open(sd, &d)) { snprintf(err, ecap, "crawl: secao de pastas malformada"); return -1; }
    const char *pp = NULL; uint32_t pl = 0;
    for (uint32_t i = 0; i < d.t.n; i++) {
        const uint8_t *r = store_table_at(&d.t, i); uint32_t l; const char *p = cw_dirs_str(&d, r, 0, &l);
        if (st_get_u32(r + 12) >= CW_NSTATE) { snprintf(err, ecap, "crawl: pasta %u com estado invalido", i); return -1; }
        if (i && st_cmp_bytes(pp, pl, p, l) >= 0) { snprintf(err, ecap, "crawl: pastas fora de ordem no indice %u", i); return -1; }
        pp = p; pl = l;
    }
    return 0;
}

/* ---------- contagens, estado e progresso ---------- */
static void cw_counts(const StoreLive *L) {
    g_cw.total = g_cw.skipped = g_cw.n1 = g_cw.n2 = 0;
    for (int i = 0; i < L->n; i++) { const StoreLiveDoc *e = &L->e[i]; if (e->dead) continue; const uint32_t lv = sdf_lv(e->d.flags);
        g_cw.total++; if (e->d.flags & SDF_SKIPMASK) { g_cw.skipped++; continue; }
        if (lv == 0 || lv >= CW_N1) g_cw.n1++;
        if (lv == 0 || lv >= CW_FULL) g_cw.n2++; }
    uint32_t f = 0; for (int i = 0; i < g_cw.nd; i++) if (g_cw.d[i].state == CW_DISC) f++; g_cw.frontier = f;
}
static void cw_write_state(int finished) {
    if (!g_cw.statepath[0]) return;
    char t[1300]; snprintf(t, sizeof t, "%s.%ld.tmp", g_cw.statepath, (long)getpid());
    FILE *f = fopen(t, "w"); if (!f) return;
    fprintf(f, "pid %ld\nroot %s\nstore %s\nepoch %llu\ngeneration %llu\nsegments %d\nactivity %s\nfiles %llu\nskipped %llu\ntext %llu\nsymbols %llu\nfrontier %u\nlimit %s\nstarted %lld\nupdated %lld\n",
            (long)getpid(), g_cw.root, g_cw.dir, (unsigned long long)g_cw.epoch, (unsigned long long)g_cw.gen, g_cw.nseg, g_cw.activity, (unsigned long long)g_cw.total, (unsigned long long)g_cw.skipped,
            (unsigned long long)g_cw.n1, (unsigned long long)g_cw.n2, g_cw.frontier, g_cw.limit, (long long)g_cw.t0, (long long)time(NULL));
    if (finished) fprintf(f, "finished %lld\n", (long long)time(NULL));
    fclose(f); chmod(t, 0600); rename(t, g_cw.statepath);
}
static void cw_announce(void) {
    if (!g_cw.frontier && !g_cw.announced[0] && g_cw.total) { g_cw.announced[0] = 1; cw_say("[index %s: %llu files found]\n", strrchr(g_cw.root, '/') ? strrchr(g_cw.root, '/') + 1 : g_cw.root, (unsigned long long)g_cw.total); }
    const uint64_t el = g_cw.total - g_cw.skipped;
    if (!g_cw.frontier && el && g_cw.n1 >= el && !g_cw.announced[1]) { g_cw.announced[1] = 1; cw_say("[index %s: text ready]\n", strrchr(g_cw.root, '/') ? strrchr(g_cw.root, '/') + 1 : g_cw.root); }
    if (!g_cw.frontier && g_cw.n2 >= el && el && !g_cw.announced[2]) { g_cw.announced[2] = 1; cw_say("[index %s: symbols and links ready (%.1f s)]\n", strrchr(g_cw.root, '/') ? strrchr(g_cw.root, '/') + 1 : g_cw.root, now_sec() - g_cw.t0); }
}

/* ---------- lotes ---------- */
static void cw_commit_extras(StoreWriter *w, CwOut *O, int dirs, int complete) {
    if (O && O->nt) { qsort(O->tomb, (size_t)O->nt, sizeof *O->tomb, cw_cmp_strp);
        const char **s = (const char **)malloc(sizeof *s * (size_t)O->nt); uint32_t *l = (uint32_t *)malloc(sizeof *l * (size_t)O->nt);
        for (int i = 0; i < O->nt; i++) { s[i] = O->tomb[i]; l[i] = (uint32_t)strlen(O->tomb[i]); }
        StoreBuf tb = {0}; store_strs_build(&tb, s, l, (uint32_t)O->nt); store_writer_add_section_buf(w, STS_TOMBS, 1, (uint32_t)O->nt, &tb); free(s); free(l); }
    if (dirs) {
        CwDir **v = (CwDir **)malloc(sizeof *v * (size_t)(g_cw.nd + 1)); int n = 0;
        for (int i = 0; i < g_cw.nd; i++) if (g_cw.d[i].dirty) v[n++] = &g_cw.d[i];
        if (n) { qsort(v, (size_t)n, sizeof *v, cw_cmp_dirp); StoreBuf db = {0}; cw_dirs_build(&db, v, n); store_writer_add_section_buf(w, STS_DIRS, 1, (uint32_t)n, &db); }
        free(v);
    }
    g_cw.gen = w->base ? w->base->gen + 1 : 0;
    StoreBuf cb = {0}; cw_crawl_build(&cb, complete); store_writer_add_section_buf(w, STS_CRAWL, 1, 1, &cb);
}
static void cw_clear_dirty(void) { for (int i = 0; i < g_cw.nd; i++) g_cw.d[i].dirty = 0; }
static int cw_cmp_cand_dir(const void *a, const void *b) {
    const CwDir *x = &g_cw.d[*(const int *)a], *y = &g_cw.d[*(const int *)b];
    const int hx = cw_is_hot(x->path, x->plen) ? 0 : 1, hy = cw_is_hot(y->path, y->plen) ? 0 : 1;
    if (hx != hy) return hx - hy;
    if (x->depth != y->depth) return x->depth < y->depth ? -1 : 1;
    return *(const int *)a - *(const int *)b;
}
/* N0: lista as pastas por descobrir (largura primeiro, quentes antes) ate o lote encher */
static int cw_batch_n0(StoreWriter *w, StoreLive *L) {
    int *cand = (int *)malloc(sizeof(int) * (size_t)(g_cw.nd + 1)); int nc = 0;
    for (int i = 0; i < g_cw.nd; i++) if (g_cw.d[i].state == CW_DISC) cand[nc++] = i;
    if (!nc) { free(cand); return 0; }
    qsort(cand, (size_t)nc, sizeof *cand, cw_cmp_cand_dir);
    StoreLiveDoc **lv = NULL; int nlv = 0;
    if (g_cw.cmp_needed) lv = store_live_sorted(L, 0, &nlv);
    CwOut O; memset(&O, 0, sizeof O); const double t0 = now_sec(); int done = 0;
    for (int k = 0; k < nc; k++) {
        cw_list_dir(cand[k], &O, L, lv, nlv); done++;
        if (O.n >= CW_BATCH_FILES || now_sec() - t0 > 1.0) break;
    }
    qsort(O.d, (size_t)O.n, sizeof *O.d, cw_cmp_doc);
    if (O.n) { StoreBuf db = {0}; store_docs_build(&db, O.d, (uint32_t)O.n); store_writer_add_section_buf(w, STS_DOCS, 1, (uint32_t)O.n, &db); }
    g_cw.total += (uint64_t)O.n; for (int i = 0; i < O.n; i++) if (O.d[i].flags & SDF_SKIPMASK) g_cw.skipped++;
    { uint32_t f = 0; for (int i = 0; i < g_cw.nd; i++) if (g_cw.d[i].state == CW_DISC) f++; g_cw.frontier = f; }
    cw_commit_extras(w, &O, 1, 0);
    char err[300] = ""; int64_t gen = store_writer_commit(w, (uint32_t)O.n, err, sizeof err);
    cw_out_free(&O); free(lv); free(cand);
    if (gen < 0) { fprintf(stderr, "crawl: %s\n", err); return -1; }
    cw_clear_dirty(); return done ? 1 : 0;
}
typedef struct { StoreLiveDoc *e; uint32_t band; } CwCand;
static int cw_cmp_cand(const void *a, const void *b) {
    const CwCand *x = (const CwCand *)a, *y = (const CwCand *)b;
    if (x->band != y->band) return x->band < y->band ? -1 : 1;
    if (x->e->d.mtime != y->e->d.mtime) return x->e->d.mtime > y->e->d.mtime ? -1 : 1;
    return st_cmp_bytes(x->e->d.path, x->e->d.plen, y->e->d.path, y->e->d.plen);
}
/* arquivos vivos com nivel `lv_from` (campo) que ainda nao tem texto/simbolos; hot = so os da faixa quente */
static CwCand *cw_select(const StoreLive *L, uint32_t lv_from, int hot_only, int *n) {
    CwCand *c = (CwCand *)malloc(sizeof *c * (size_t)(L->n + 1)); int k = 0;
    for (int i = 0; i < L->n; i++) { StoreLiveDoc *e = &L->e[i]; if (e->dead || !sdf_partial(e->d.flags)) continue;
        if (lv_from && sdf_lv(e->d.flags) != lv_from) continue;
        const int hot = cw_is_hot(e->d.path, e->d.plen); if (hot_only && !hot) continue;
        c[k].e = e; c[k].band = cw_band(e->d.path, e->d.plen, e->d.size, e->d.mtime); k++; }
    qsort(c, (size_t)k, sizeof *c, cw_cmp_cand); *n = k; return c;
}
static int cw_generated_body(const uint8_t *b, size_t n) {
    size_t h = n < 1000 ? n : 1000; char t[1001]; memcpy(t, b, h); t[h] = 0;
    if (strstr(t, "DO NOT EDIT") || strstr(t, "@generated") || strstr(t, "Code generated")) return 1;
    size_t lines = 1, mx = 0, cur = 0; for (size_t i = 0; i < n; i++) { if (b[i] == '\n') { lines++; if (cur > mx) mx = cur; cur = 0; } else cur++; } if (cur > mx) mx = cur;
    return mx > 20000 || (n > 4000 && n / lines > 500);
}
/* ---------- pool de analise (fase 5) ---------- */
typedef struct { void (*fn)(void *ctx, int i, CwWorker *wk); void *ctx; int n; int next; } CwJob;
typedef struct { CwJob *j; CwWorker *wk; } CwWArg;
static void *cw_worker_main(void *p) {
    CwWArg *a = (CwWArg *)p;
    for (;;) { const int i = __atomic_fetch_add(&a->j->next, 1, __ATOMIC_RELAXED); if (i >= a->j->n) break; a->j->fn(a->j->ctx, i, a->wk); }
    return NULL;
}
static void cw_worker_init(CwWorker *wk) {
    if (!wk->an) wk->an = cs_an_new(cs_kb_limit());
    if (!wk->buf) wk->buf = (uint8_t *)malloc((size_t)g_cw.maxsz + 1);
}
/* roda fn(ctx, i, worker) para i = 0..n-1 em g_cw.threads threads (a do rastreador entre elas); com 1 thread nao nasce nenhuma. Quem junta os resultados
 * e a thread do rastreador, depois, na ordem de i: a ordem em que as threads terminam nao muda o indice. */
static void cw_parallel(int n, void (*fn)(void *, int, CwWorker *), void *ctx) {
    int t = g_cw.threads; if (t > n) t = n; if (t < 1) t = 1; if (t > CW_MAXTHR) t = CW_MAXTHR;
    ct_init();                                    /* tabelas do tokenizador prontas antes de qualquer thread */
    for (int w = 0; w < t; w++) cw_worker_init(&g_cw.wk[w]);
    CwJob j = {fn, ctx, n, 0};
    CwWArg a[CW_MAXTHR]; pthread_t th[CW_MAXTHR]; int started[CW_MAXTHR] = {0};
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, (size_t)16 << 20);
    for (int w = 1; w < t; w++) { a[w].j = &j; a[w].wk = &g_cw.wk[w]; if (pthread_create(&th[w], &at, cw_worker_main, &a[w]) == 0) started[w] = 1; }
    a[0].j = &j; a[0].wk = &g_cw.wk[0]; cw_worker_main(&a[0]);       /* se alguma thread nao nasceu, as outras (e esta) dao conta: o contador e compartilhado */
    for (int w = 1; w < t; w++) if (started[w]) pthread_join(th[w], NULL);
    pthread_attr_destroy(&at);
}
static int cw_generated_body(const uint8_t *b, size_t n);
typedef struct { CwItem *it; int full; } CwTextJob;
/* uma thread: le o arquivo, decide o nivel (segredo, binario, ilegivel), calcula o hash e analisa (texto do N1; com `full` tambem simbolos e chaves) */
static void cw_work_text(void *ctx, int i, CwWorker *wk) {
    CwTextJob *J = (CwTextJob *)ctx; CwItem *x = &J->it[i]; const StoreDoc *o = &x->d; const uint8_t *data = NULL; size_t dn = 0;
    uint32_t flags = o->flags & ~SDF_LVMASK & ~SDF_GENERATED; x->nodata = 1;
    if (o->flags & (SDF_SKIPMASK | SDF_GENERATED)) { x->d.flags = (o->flags & ~SDF_LVMASK) | (CW_FULL << 8); }       /* sem leitura: so caminho e nome vao para a busca */
    else {
        char full[8400]; snprintf(full, sizeof full, "%s/%.*s", g_cw.root, (int)o->plen, o->path);
        const int64_t got = st_slurp(full, wk->buf, g_cw.maxsz);
        if (got < 0) flags |= SDF_SKIP_UNREADABLE | (CW_FULL << 8);
        else if (ign_secret_sniff(wk->buf, (size_t)got)) flags |= SDF_SKIP_SECRET | (CW_FULL << 8);          /* chave privada reconhecida pelo conteudo: nunca indexada */
        else { const size_t chk = (size_t)got < 8000 ? (size_t)got : 8000;
            if (memchr(wk->buf, 0, chk)) flags |= SDF_SKIP_BINARY | (CW_FULL << 8);
            else { x->d.hash = store_hash64(wk->buf, (size_t)got, 1); x->d.size = (uint64_t)got; data = wk->buf; dn = (size_t)got;
                flags |= (J->full ? CW_FULL : CW_N1) << 8;
                if (g_cw.generated && cw_generated_body(wk->buf, (size_t)got)) { flags |= SDF_GENERATED; flags = (flags & ~SDF_LVMASK) | (CW_FULL << 8); data = NULL; dn = 0; } } }
        x->d.flags = flags;
    }
    x->nodata = data == NULL;
    cs_an_doc(wk->an, &x->d, data, dn, &x->cs);
    if (J->full && data && dn) { sy_analyze(g_cw.root, &x->d, data, dn, &x->sy); x->has_sy = 1; }
}
/* N1 (so texto) ou refazer por inteiro (texto + simbolos + chaves: faixa quente e restos) */
static int cw_batch_text(StoreWriter *w, StoreLive *L, uint32_t from_lv, int hot_only, int full) {
    int nc; CwCand *c = cw_select(L, from_lv, hot_only, &nc);
    if (!nc) { free(c); return 0; }
    if (!hot_only && g_cw.maxtext && g_cw.n1 >= g_cw.maxtext) { if (!g_cw.limit[0]) snprintf(g_cw.limit, sizeof g_cw.limit, "text index limited to %llu files", (unsigned long long)g_cw.maxtext); free(c); return 0; }
    /* o lote: os primeiros da fila de prioridade ate os limites (arquivos, bytes do catalogo); depois por caminho, que e a ordem dos documentos do segmento */
    const int maxfiles = hot_only ? 512 : CW_BATCH_FILES;
    CwItem *it = (CwItem *)calloc((size_t)(nc < maxfiles ? nc : maxfiles) + 1, sizeof *it); int n = 0; uint64_t bytes = 0;
    for (int i = 0; i < nc && n < maxfiles; i++) {
        const StoreDoc *o = &c[i].e->d; it[n].d = *o; it[n].d.path = cw_dup(o->path, o->plen); n++;
        if (!(o->flags & (SDF_SKIPMASK | SDF_GENERATED))) bytes += o->size;       /* so o que sera lido conta para o limite do lote */
        if (bytes >= g_cw.batch_bytes) break;
    }
    free(c);
    qsort(it, (size_t)n, sizeof *it, cw_cmp_doc);       /* StoreDoc e o primeiro campo de CwItem */
    CwTextJob J = {it, full}; cw_parallel(n, cw_work_text, &J);
    StoreScanOpts o; memset(&o, 0, sizeof o); coder_search_hook(&o); void *cs_ctx = o.hook_ctx; void *sy_ctx = NULL;
    if (full) { coder_sym_hook(&o, g_cw.root); sy_ctx = o.hook_ctx; }
    StoreDoc *nd = (StoreDoc *)malloc(sizeof *nd * (size_t)n);
    for (int i = 0; i < n; i++) {         /* a fusao, uma thread, na ordem dos documentos */
        nd[i] = it[i].d;
        cs_ix_merge((CsIx *)cs_ctx, (uint32_t)i, &it[i].cs); cs_res_free(&it[i].cs);
        if (full) sy_ingest((SyHook *)sy_ctx, (uint32_t)i, it[i].has_sy ? &it[i].sy : NULL);
    }
    StoreBuf db = {0}; store_docs_build(&db, nd, (uint32_t)n); store_writer_add_section_buf(w, STS_DOCS, 1, (uint32_t)n, &db);
    o.commit_fn(o.hook_ctx, w, (uint32_t)n);
    for (int i = 0; i < n; i++) { if (!(it[i].d.flags & SDF_SKIPMASK) && !(it[i].d.flags & SDF_GENERATED)) { g_cw.n1++; if (full || sdf_lv(it[i].d.flags) == CW_FULL) g_cw.n2++; } }
    cw_commit_extras(w, NULL, 0, 0);
    char err[300] = "", si[400], yi[500]; int64_t gen = store_writer_commit(w, (uint32_t)n, err, sizeof err);
    if (full) coder_sym_hook_done(&o, yi, sizeof yi);
    coder_search_hook_done(&o, si, sizeof si);
    free(nd); for (int i = 0; i < n; i++) free((void *)it[i].d.path); free(it);
    if (gen < 0) { fprintf(stderr, "crawl: %s\n", err); return -1; }
    return 1;
}
typedef struct { const StoreDoc *nd; CwItem *it; } CwAugJob;
/* uma thread: relê o arquivo; se o hash ainda e o do N1, analisa simbolos e chaves; se mudou, o documento continua no N1 (a faixa dos restos o refaz) */
static void cw_work_aug(void *ctx, int i, CwWorker *wk) {
    CwAugJob *J = (CwAugJob *)ctx; const StoreDoc *d = &J->nd[i]; CwItem *x = &J->it[i];
    if (d->flags & SDF_SKIPMASK) return;
    char full[8400]; snprintf(full, sizeof full, "%s/%.*s", g_cw.root, (int)d->plen, d->path);
    const int64_t got = st_slurp(full, wk->buf, g_cw.maxsz);
    if (got >= 0 && store_hash64(wk->buf, (size_t)got, 1) == d->hash) { x->ok = 1; if (got > 0) { sy_analyze(g_cw.root, d, wk->buf, (size_t)got, &x->sy); x->has_sy = 1; } }
}
/* N2 e N3 por aumento de um segmento N1 limpo: busca copiada byte a byte + simbolos e chaves novos; o segmento antigo sai na mesma troca */
static int cw_batch_aug(StoreWriter *w, StoreLive *L) {
    StoreReader *b = w->base; int pick = -1;
    for (int s = 0; s < b->nseg && pick < 0; s++) {
        const StoreSeg *sg = &b->seg[s]; const StoreSection *sd = store_seg_section(sg, STS_DOCS);
        if (!sd || !sd->nrec || !store_seg_section(sg, CS_T_TERMS) || store_seg_section(sg, SY_T_KDOCS) || store_seg_section(sg, SY_T_DOCS) || store_seg_section(sg, STS_DIRS)) continue;
        StoreDocs dc; if (!store_docs_open(sd, &dc)) continue;
        int ok = 1, any = 0;
        for (uint32_t i = 0; i < dc.t.n && ok; i++) { StoreDoc d; if (!store_docs_get(&dc, i, &d)) { ok = 0; break; }
            StoreLiveDoc *e = store_live_find(L, d.path, d.plen);
            if (!e || e->dead || e->seg != s || e->idx != i) { ok = 0; break; }
            if (d.flags & SDF_SKIPMASK) continue;
            if (sdf_lv(d.flags) != CW_N1) { ok = 0; break; }
            any = 1; }
        if (ok && any) pick = s;
    }
    if (pick < 0) return 0;
    const StoreSeg *sg = &b->seg[pick]; StoreDocs dc; store_docs_open(store_seg_section(sg, STS_DOCS), &dc);
    const uint32_t n = dc.t.n; StoreDoc *nd = (StoreDoc *)malloc(sizeof *nd * (size_t)n);
    for (uint32_t i = 0; i < n; i++) store_docs_get(&dc, i, &nd[i]);
    CwItem *it = (CwItem *)calloc((size_t)n + 1, sizeof *it);
    CwAugJob J = {nd, it}; cw_parallel((int)n, cw_work_aug, &J);
    StoreScanOpts o; memset(&o, 0, sizeof o); coder_sym_hook(&o, g_cw.root);
    for (uint32_t i = 0; i < n; i++) {
        sy_ingest((SyHook *)o.hook_ctx, i, it[i].has_sy ? &it[i].sy : NULL);
        if (it[i].ok) { nd[i].flags = (nd[i].flags & ~SDF_LVMASK) | (CW_FULL << 8); g_cw.n2++; }
    }
    free(it);
    StoreBuf db = {0}; store_docs_build(&db, nd, n); store_writer_add_section_buf(w, STS_DOCS, 1, n, &db);
    for (uint32_t i = 0; i < sg->nsec; i++) { const StoreSection *x = &sg->sec[i]; if (x->type == STS_DOCS || x->type == STS_CRAWL) continue; store_writer_add_section(w, x->type, x->version, x->nrec, x->data, (size_t)x->size); }
    o.commit_fn(o.hook_ctx, w, n);
    cw_commit_extras(w, NULL, 0, 0);
    store_writer_drop(w, sg->id);
    char err[300] = "", yi[500]; int64_t gen = store_writer_commit(w, n, err, sizeof err);
    coder_sym_hook_done(&o, yi, sizeof yi); free(nd);
    if (gen < 0) { fprintf(stderr, "crawl: %s\n", err); return -1; }
    return 1;
}
/* compacta a cauda quando passou de `limit` segmentos (mesma regra por camadas do coder index) */
static void cw_compact(StoreWriter *w, int limit) {
    if (getenv("KATANA_INDEX_TRACE")) fprintf(stderr, "crawl: compactacao: %d segmentos, limite %d\n", w->base->nseg, limit);
    if (w->base->nseg <= limit) return;
    int from = w->base->nseg - 1; uint64_t sum = w->base->seg[from].size;
    while (from > 0 && (sum * 2 >= w->base->seg[from - 1].size || w->base->nseg - from < 2)) { from--; sum += w->base->seg[from].size; }
    char err[300] = ""; if (store_writer_compact(w, from, err, sizeof err) < 0) fprintf(stderr, "crawl: compactacao: %s\n", err);
}

/* ---------- pedidos de mudanca (--changed, TOUCH, SWEEP) ---------- */
static const char *sl_base(const char *p) { const char *s2 = strrchr(p, '/'); return s2 ? s2 + 1 : p; }
static void cw_relist_subtree(int di) {
    const size_t pl = g_cw.d[di].plen; char *pre = cw_dup(g_cw.d[di].path, pl);
    for (int i = 0; i < g_cw.nd; i++) { CwDir *C = &g_cw.d[i]; if (C->state != CW_GONE && (di == 0 || cw_under(C->path, C->plen, pre, pl)) && (C->state == CW_LISTED || C->state == CW_TOOMANY || C->state == CW_UNREAD || C->state == CW_DISC)) { C->state = CW_DISC; C->prev_listed = 1; } }
    free(pre);
}
static void cw_revalidate(void);
static void cw_apply_changes(void) {
    if (g_cw.sweep) { g_cw.sweep = 0; cw_rules_reset(); cw_ovr_check(1); cw_relist_subtree(0); g_cw.cmp_needed = 1; }
    else if (cw_ovr_check(0)) { cw_revalidate(); g_cw.cmp_needed = 1; }          /* `katana coder include/exclude` mexeu no arquivo overrides */
    for (int k = 0; k < g_cw.nchg; k++) {
        const char *rel = g_cw.chg[k]; size_t rl = strlen(rel); char a[4400]; snprintf(a, sizeof a, "%s%s%s", g_cw.root, rl ? "/" : "", rel);
        struct stat st; const int isdir = stat(a, &st) == 0 && S_ISDIR(st.st_mode);
        if (isdir) { int di = cw_dir_find(rel, rl); if (di >= 0) { cw_relist_subtree(di); } else { const char *sl = (const char *)cw_memrchr(rel, '/', rl); int pi = cw_dir_find(rel, sl ? (size_t)(sl - rel) : 0); if (pi >= 0 && g_cw.d[pi].state == CW_LISTED) { g_cw.d[pi].state = CW_DISC; g_cw.d[pi].prev_listed = 1; } } }
        else { const char *sl = (const char *)cw_memrchr(rel, '/', rl); int pi = cw_dir_find(rel, sl ? (size_t)(sl - rel) : 0); if (pi >= 0 && (g_cw.d[pi].state == CW_LISTED || g_cw.d[pi].state == CW_TOOMANY)) { g_cw.d[pi].state = CW_DISC; g_cw.d[pi].prev_listed = 1; } }
        { const char *bn = sl_base(rel); if (!strcmp(bn, ".gitignore") || !strcmp(bn, ".katanaignore")) {       /* as regras valem para baixo: relista a pasta do arquivo e tudo abaixo */
            const char *sl2 = (const char *)cw_memrchr(rel, '/', rl); const size_t dl = sl2 ? (size_t)(sl2 - rel) : 0; int pi2 = cw_dir_find(rel, dl); cw_rules_reset();
            if (pi2 >= 0) { cw_relist_subtree(pi2); if (g_cw.d[pi2].state == CW_LISTED) { g_cw.d[pi2].state = CW_DISC; g_cw.d[pi2].prev_listed = 1; } cw_revalidate(); } } }
        cw_hot_add(rel); free(g_cw.chg[k]);
    }
    if (g_cw.nchg) g_cw.cmp_needed = 1;
    g_cw.nchg = 0;
}

/* Confere as pastas ja conhecidas contra a decisao de hoje (overrides, .gitignore, .katanaignore mudaram, ou o rastreador acabou de retomar do store): a pasta
 * cujo estado nao bate (listada e agora ignorada, ou ignorada e agora reincluida) faz a pasta PAI ser listada de novo; e a listagem do pai que aplica a mudanca
 * (cw_list_dir: novo estado, pastas de dentro `gone`, tombstones). So decide, nao le diretorio. */
static int cw_cmp_depth(const void *a, const void *b) { const CwDir *x = &g_cw.d[*(const int *)a], *y = &g_cw.d[*(const int *)b]; return x->depth != y->depth ? (x->depth < y->depth ? -1 : 1) : *(const int *)a - *(const int *)b; }
static void cw_revalidate(void) {
    int *ord = (int *)malloc(sizeof(int) * (size_t)(g_cw.nd + 1)); int n = 0;
    for (int i = 1; i < g_cw.nd; i++) ord[n++] = i;
    qsort(ord, (size_t)n, sizeof *ord, cw_cmp_depth);
    for (int k = 0; k < n; k++) {
        const int di = ord[k]; CwDir *D = &g_cw.d[di]; const int pi = D->parent;
        if (pi < 0 || D->plen == 0 || D->state == CW_GONE || D->state == CW_LINK || D->state == CW_TOODEEP) continue;
        const CwDir *P = &g_cw.d[pi]; if (cw_blocked(P->state)) continue;
        StoreScan S; cw_stack(pi, &S);
        char full[4400]; cw_abs(D, full, sizeof full); const char *nm = sl_base(D->path);
        int nmode = 0, ir = 0; char rr[420];
        const int ign = st_scan_decide(&S, full, g_cw.rlen + 1, nm, 1, &nmode, &ir, rr, sizeof rr);
        const int want_blocked = ign != 0, is_blocked = D->state == CW_IGNORED || D->state == CW_NESTED || D->state == CW_CHECKOUT;
        if (want_blocked != is_blocked) { CwDir *Q = &g_cw.d[pi]; if (Q->state == CW_LISTED || Q->state == CW_TOOMANY || Q->state == CW_UNREAD) { Q->state = CW_DISC; Q->prev_listed = 1; } g_cw.cmp_needed = 1; }
    }
    free(ord);
}

/* ---------- socket ---------- */
static int cw_run_dir(char *out, size_t cap) { char h[1000]; agent_home(h, sizeof h); snprintf(out, cap, "%s/.katana/run", h); return st_mkdirs(out); }
static int cw_sock_addr(const char *key, int dirfd, struct sockaddr_un *a) {
    memset(a, 0, sizeof *a); a->sun_family = AF_UNIX;
    return snprintf(a->sun_path, sizeof a->sun_path, "/proc/self/fd/%d/coder-%s.sock", dirfd, key) < (int)sizeof a->sun_path ? 0 : -1;
}
/* cliente: uma linha, uma resposta; 0 se falou com um rastreador vivo */
static int cw_client(const char *key, const char *line, char *reply, size_t cap, int timeout_ms) {
    char rd[1100]; if (cw_run_dir(rd, sizeof rd) != 0) return -1;
    int dfd = open(rd, O_RDONLY | O_DIRECTORY); if (dfd < 0) return -1;
    struct sockaddr_un a; int s = socket(AF_UNIX, SOCK_STREAM, 0); int rc = -1;
    if (s >= 0 && cw_sock_addr(key, dfd, &a) == 0 && connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        char l[700]; snprintf(l, sizeof l, "%s\n", line); if (write(s, l, strlen(l)) > 0) {
            struct pollfd p = {s, POLLIN, 0}; if (poll(&p, 1, timeout_ms) > 0) { ssize_t k = read(s, reply, cap - 1); if (k > 0) { reply[k] = 0; reply[strcspn(reply, "\r\n")] = 0; rc = 0; } } } }
    if (s >= 0) close(s);
    close(dfd); return rc;
}
static int cw_listen(void) {
    char rd[1100]; if (cw_run_dir(rd, sizeof rd) != 0) return -1;
    int dfd = open(rd, O_RDONLY | O_DIRECTORY); if (dfd < 0) return -1;
    snprintf(g_cw.sockpath, sizeof g_cw.sockpath, "%s/coder-%s.sock", rd, g_cw.key);
    unlink(g_cw.sockpath);
    struct sockaddr_un a; int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0 || cw_sock_addr(g_cw.key, dfd, &a) != 0 || bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 8) != 0) { if (s >= 0) close(s); close(dfd); g_cw.sockpath[0] = 0; return -1; }
    chmod(g_cw.sockpath, 0600); fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); close(dfd); return s;
}
/* quantos arquivos sob o prefixo ja chegaram a `level` (0 N0, 1 N1, 2 e 3 N2/N3): para SYNC */
static int cw_prefix_done(int level, const char *prefix) {
    for (int i = 0; i < g_cw.nd; i++) if (g_cw.d[i].state == CW_DISC && (!*prefix || cw_under(g_cw.d[i].path, g_cw.d[i].plen, prefix, strlen(prefix)))) return 0;
    if (level == 0) return 1;
    StoreReader *r; char err[200]; if (store_open(g_cw.dir, &r, err, sizeof err) != 0) return 0;
    StoreLive L; const StoreSeg **sg = (const StoreSeg **)malloc(sizeof *sg * (size_t)(r->nseg + 1)); for (int i = 0; i < r->nseg; i++) sg[i] = &r->seg[i];
    store_live_build(&L, sg, r->nseg); free(sg);
    const size_t pl = strlen(prefix); int ok = 1; const uint32_t need = level == 1 ? CW_N1 : CW_FULL;
    for (int i = 0; i < L.n && ok; i++) { const StoreLiveDoc *e = &L.e[i]; if (e->dead || (e->d.flags & SDF_SKIPMASK) || (pl && !cw_under(e->d.path, e->d.plen, prefix, pl))) continue;
        const uint32_t lv = sdf_lv(e->d.flags); if (lv != 0 && lv < need) ok = 0; }
    store_live_free(&L); store_close(r); return ok;
}
static void cw_reply(int fd, const char *s) { size_t n = strlen(s); if (write(fd, s, n) < 0 || write(fd, "\n", 1) < 0) { } }
static void cw_serve(int wait_ms) {
    if (g_cw.sockfd < 0) { if (wait_ms > 0) usleep((useconds_t)wait_ms * 1000); return; }
    struct pollfd p = {g_cw.sockfd, POLLIN, 0};
    int budget = wait_ms;
    for (;;) {
        if (poll(&p, 1, budget) <= 0) break;
        int c = accept(g_cw.sockfd, NULL, NULL); if (c < 0) break;
        struct pollfd q = {c, POLLIN, 0}; char ln[700] = ""; if (poll(&q, 1, 300) > 0) { ssize_t k = read(c, ln, sizeof ln - 1); if (k > 0) ln[k] = 0; }
        ln[strcspn(ln, "\r\n")] = 0; char rep[300];
        if (!strncmp(ln, "PING", 4)) { snprintf(rep, sizeof rep, "OK %ld %llu %s", (long)getpid(), (unsigned long long)g_cw.gen, g_cw.activity); cw_reply(c, rep); }
        else if (!strncmp(ln, "PRIO ", 5)) { cw_hot_add(ln + 5); g_cw.work = 1; cw_reply(c, "OK"); }
        else if (!strncmp(ln, "TOUCH ", 6)) { cw_chg_add(ln + 6); g_cw.work = 1; cw_reply(c, "OK"); }
        else if (!strcmp(ln, "SWEEP")) { g_cw.sweep = 1; g_cw.work = 1; cw_reply(c, "OK"); }
        else if (!strcmp(ln, "STOP")) { g_cw.stop = 1; cw_reply(c, "OK"); }
        else if (!strcmp(ln, "STATUS")) { snprintf(rep, sizeof rep, "OK files %llu skipped %llu text %llu symbols %llu frontier %u generation %llu %s", (unsigned long long)g_cw.total, (unsigned long long)g_cw.skipped, (unsigned long long)g_cw.n1, (unsigned long long)g_cw.n2, g_cw.frontier, (unsigned long long)g_cw.gen, g_cw.activity); cw_reply(c, rep); }
        else if (!strncmp(ln, "SYNC ", 5) && g_cw.nsync < 8) {
            int lv = 3, ms = 2000; char pre[600] = ""; char *p2 = ln + 5; lv = atoi(p2); while (*p2 && *p2 != ' ') p2++; while (*p2 == ' ') p2++;
            char *sp = strchr(p2, ' '); if (sp) { *sp = 0; snprintf(pre, sizeof pre, "%s", p2); ms = atoi(sp + 1); } else ms = atoi(p2);
            if (!strcmp(pre, "-")) pre[0] = 0;
            g_cw.sync[g_cw.nsync].fd = c; g_cw.sync[g_cw.nsync].level = lv; snprintf(g_cw.sync[g_cw.nsync].prefix, sizeof g_cw.sync[0].prefix, "%s", pre); g_cw.sync[g_cw.nsync].deadline = now_sec() + ms / 1000.0; g_cw.nsync++; continue;
        }
        else cw_reply(c, "ERR");
        close(c); budget = 0;
    }
    for (int i = 0; i < g_cw.nsync; ) {
        const int ok = !g_cw.nchg && !g_cw.sweep && cw_prefix_done(g_cw.sync[i].level, g_cw.sync[i].prefix); const int late = now_sec() >= g_cw.sync[i].deadline;
        if (ok || late) { char rep[200]; snprintf(rep, sizeof rep, "OK %llu %s %llu %llu", (unsigned long long)g_cw.gen, ok ? "complete" : "partial", (unsigned long long)g_cw.n1, (unsigned long long)g_cw.total); cw_reply(g_cw.sync[i].fd, rep); close(g_cw.sync[i].fd); g_cw.sync[i] = g_cw.sync[--g_cw.nsync]; }
        else i++;
    }
}

/* ---------- carga do estado a partir do store (retomada) ---------- */
static void cw_load(StoreReader *r) {
    for (int s = 0; s < r->nseg; s++) {
        CwDirs d; if (!cw_dirs_open(store_seg_section(&r->seg[s], STS_DIRS), &d)) continue;
        for (uint32_t i = 0; i < d.t.n; i++) {
            const uint8_t *rec = store_table_at(&d.t, i); uint32_t l; const char *p = cw_dirs_str(&d, rec, 0, &l);
            int di = cw_dir_find(p, l); if (di < 0) { int pa = cw_parent_of(p, l); di = cw_dir_add(p, l, pa, CW_DISC); }
            cw_dirs_fill(&g_cw.d[di], &d, rec); g_cw.d[di].dirty = 0;
        }
    }
    for (int i = 0; i < g_cw.nd; i++) if (g_cw.d[i].plen) { int pa = cw_parent_of(g_cw.d[i].path, g_cw.d[i].plen); g_cw.d[i].parent = pa; }
    for (int s = r->nseg - 1; s >= 0; s--) { const StoreSection *x = store_seg_section(&r->seg[s], STS_CRAWL); uint64_t ep, wh, c[4]; uint32_t fr, fl; char lim[120];
        if (x && cw_crawl_parse(x, &ep, &wh, c, &fr, &fl, lim, sizeof lim)) { g_cw.epoch = ep; break; } }
}

/* ---------- processo ---------- */
static void cw_lower_priority(void) {
    if (nice(10) < 0) { }
#ifdef SYS_ioprio_set
    syscall(SYS_ioprio_set, 1, 0, (3 << 13) | 0);       /* IOPRIO_WHO_PROCESS, este processo, classe ociosa */
#endif
}
static int cw_env_int(const char *n, int d) { const char *e = getenv(n); return e && *e ? atoi(e) : d; }
static void cw_on_term(int sig) { (void)sig; g_cw.stop = 1; }
static int coder_crawl_main(int argc, char **argv) {
    char proj[4096] = "."; int prio_n = 0; char *prio[16]; int stop_req = 0, sweep = 0, foreground = 0; char *chg[256]; int nchg = 0; int mode = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--prio") && i + 1 < argc) { if (prio_n < 16) prio[prio_n++] = argv[++i]; else i++; mode = 1; }
        else if (!strcmp(argv[i], "--changed")) mode = 2;
        else if (!strcmp(argv[i], "--sweep")) sweep = 1;
        else if (!strcmp(argv[i], "--stop")) stop_req = 1;
        else if (!strcmp(argv[i], "--quiet")) g_cw.quiet = 1;
        else if (!strcmp(argv[i], "--foreground")) foreground = 1;
        else if (mode == 2 && nchg < 256) chg[nchg++] = argv[i];
        else if (argv[i][0] != '-') snprintf(proj, sizeof proj, "%s", argv[i]);
    }
    struct stat st0; if (stat(proj, &st0) != 0 || !S_ISDIR(st0.st_mode)) { fprintf(stderr, "crawl: %s nao e uma pasta\n", proj); return 2; }
    IndexLoc loc; index_resolve(proj, 1, &loc);
    { char h[1000]; agent_home(h, sizeof h); if (!strcmp(loc.root, "/") || !strcmp(loc.root, h)) { fprintf(stderr, "crawl: a raiz %s nao e indexada (home ou /)\n", loc.root); return 2; } }
    char key[40]; snprintf(g_cw.root, sizeof g_cw.root, "%s", loc.root); g_cw.rlen = strlen(g_cw.root);
    snprintf(key, sizeof key, "%016llx", (unsigned long long)store_hash64(g_cw.root, g_cw.rlen, 0)); snprintf(g_cw.key, sizeof g_cw.key, "%s", key);
    store_locate(g_cw.root, "coder", g_cw.dir, sizeof g_cw.dir, 1);
    /* prefixos pedidos: relativos a raiz (a pasta visitada dentro de uma raiz que a absorve conta como --prio) */
    if (loc.prefix[0]) cw_hot_add(loc.prefix);
    for (int i = 0; i < prio_n; i++) { char r[4400]; if (cw_rel(prio[i], r, sizeof r)) cw_hot_add(r); else cw_hot_add(prio[i]); }
    for (int i = 0; i < nchg; i++) { char r[4400]; if (cw_rel(chg[i], r, sizeof r)) cw_chg_add(r); }
    g_cw.sweep = sweep;
    /* eleicao */
    char lp[4500]; snprintf(lp, sizeof lp, "%s/CRAWL.LOCK", g_cw.dir);
    int fd = open(lp, O_RDWR | O_CREAT, 0600); if (fd < 0) { fprintf(stderr, "crawl: CRAWL.LOCK: %s\n", strerror(errno)); return 1; }
    int got = flock(fd, LOCK_EX | LOCK_NB) == 0;
    if (stop_req) { char rep[300]; if (cw_client(g_cw.key, "STOP", rep, sizeof rep, 4000) == 0) printf("crawl: stop requested (%s)\n", rep); else printf("crawl: no crawler running for %s\n", g_cw.root); if (got) flock(fd, LOCK_UN); close(fd); return 0; }
    if (!got) {
        char rep[300]; int sent = 0;
        for (int tries = 0; tries < 60 && !sent; tries++) {              /* o vencedor pode ainda estar subindo, ou saindo: 3 s no maximo */
            if (cw_client(g_cw.key, "PING", rep, sizeof rep, 4000) == 0) {
                for (int i = 0; i < g_cw.nhot; i++) { char l[4500]; snprintf(l, sizeof l, "PRIO %s", g_cw.hot[i]); cw_client(g_cw.key, l, rep, sizeof rep, 4000); }
                for (int i = 0; i < g_cw.nchg; i++) { char l[4500]; snprintf(l, sizeof l, "TOUCH %s", g_cw.chg[i]); cw_client(g_cw.key, l, rep, sizeof rep, 4000); }
                if (g_cw.sweep) cw_client(g_cw.key, "SWEEP", rep, sizeof rep, 4000);
                sent = 1; break; }
            usleep(50000);
            if (flock(fd, LOCK_EX | LOCK_NB) == 0) { got = 1; break; }
        }
        if (!got) { if (!sent) fprintf(stderr, "crawl: another crawler holds the lock for %s and did not answer\n", g_cw.root); else cw_say("crawl: a crawler is already running for %s; request handed over\n", g_cw.root); close(fd); return 0; }
    }
    g_cw.lockfd = fd;
    if (!foreground) { signal(SIGHUP, SIG_IGN); signal(SIGPIPE, SIG_IGN); setsid(); }
    cw_lower_priority();
    g_cw.t0 = now_sec(); g_cw.sockfd = cw_listen(); { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = cw_on_term; sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); }
    { char rd[1100]; if (cw_run_dir(rd, sizeof rd) == 0) snprintf(g_cw.statepath, sizeof g_cw.statepath, "%s/coder-%s.state", rd, g_cw.key); }
    { const char *e;
      g_cw.maxsz = 2u << 20; g_cw.maxdepth = (uint32_t)cw_env_int("KATANA_INDEX_MAX_DEPTH", 32);
      g_cw.maxfiles = (e = getenv("KATANA_INDEX_MAX_FILES")) && *e ? strtoull(e, NULL, 10) : 500000ull;
      g_cw.maxtext = (e = getenv("KATANA_INDEX_MAX_TEXT_FILES")) && *e ? strtoull(e, NULL, 10) : 200000ull;
      g_cw.generated = cw_env_int("KATANA_INDEX_GENERATED", 0);
      { const long nc = sysconf(_SC_NPROCESSORS_ONLN); int dflt = nc > 1 ? (int)(nc - 1) : 1; if (dflt > 4) dflt = 4;
        g_cw.threads = cw_env_int("KATANA_INDEX_THREADS", dflt); if (g_cw.threads < 1) g_cw.threads = 1; if (g_cw.threads > CW_MAXTHR) g_cw.threads = CW_MAXTHR; }
      { const int mb = cw_env_int("KATANA_INDEX_BATCH_MB", 16); g_cw.batch_bytes = (uint64_t)(mb > 0 ? mb : 16) << 20; } }
    const int settle = cw_env_int("KATANA_INDEX_SETTLE_MS", 2000);
    store_catalog_init(); maint_on_crawl_start();
    snprintf(g_cw.activity, sizeof g_cw.activity, "starting");
    /* retomada: pastas e documentos vigentes do store */
    cw_tab_rebuild();
    { StoreReader *r; char err[300]; if (store_open(g_cw.dir, &r, err, sizeof err) == 0) { cw_load(r); g_cw.bootstrap = r->nseg > 0; g_cw.nseg = r->nseg; store_close(r); } }
    g_cw.epoch++;
    if (g_cw.nd == 0 || g_cw.d[0].plen != 0) { int z = cw_dir_find("", 0); if (z < 0) { cw_dir_add("", 0, -1, CW_DISC); } }
    g_cw.cmp_needed = g_cw.bootstrap;
    cw_ovr_check(1);
    if (g_cw.bootstrap) cw_revalidate();         /* uma regra pode ter mudado desde a ultima rodada */
    cw_say("[index %s: crawling]\n", g_cw.root);
    int rc = 0, compacted_n0 = 0;
    for (;;) {
        cw_serve(0);
        if (g_cw.stop) break;
        cw_apply_changes(); g_cw.work = 0;
        StoreWriter *w; char err[300] = ""; int br = store_writer_begin_wait(g_cw.dir, &w, err, sizeof err, 5000);
        if (br != 0) { fprintf(stderr, "crawl: %s\n", err); rc = br == 1 ? 3 : 1; break; }
        store_writer_set_root(w, g_cw.root);
        StoreLive L; { const StoreSeg **sg = (const StoreSeg **)malloc(sizeof *sg * (size_t)(w->base->nseg + 1)); for (int i = 0; i < w->base->nseg; i++) sg[i] = &w->base->seg[i]; store_live_build(&L, sg, w->base->nseg); free(sg); }
        g_cw.nseg = w->base->nseg; g_cw.gen = w->base->gen;
        cw_counts(&L);
        int did = 0;
        int hotn = 0; { CwCand *c = cw_select(&L, 0, 1, &hotn); free(c); }
        if (hotn) { snprintf(g_cw.activity, sizeof g_cw.activity, "hot band"); did = cw_batch_text(w, &L, 0, 1, 1); }
        else if (g_cw.frontier) { snprintf(g_cw.activity, sizeof g_cw.activity, "N0 tree"); did = cw_batch_n0(w, &L); }
        else {
            if (!compacted_n0) { compacted_n0 = 1; if (w->base->nseg > 24) { snprintf(g_cw.activity, sizeof g_cw.activity, "compacting"); cw_compact(w, 24); did = 1; } }
            if (!did) { snprintf(g_cw.activity, sizeof g_cw.activity, "N1 text"); did = cw_batch_text(w, &L, CW_N0, 0, 0); }
            if (!did) { snprintf(g_cw.activity, sizeof g_cw.activity, "N2 symbols and N3 links"); did = cw_batch_aug(w, &L); }
            if (!did) { snprintf(g_cw.activity, sizeof g_cw.activity, "N2 symbols and N3 links (rebuild)"); did = cw_batch_text(w, &L, 0, 0, 1); }
        }
        if (getenv("KATANA_INDEX_TRACE")) fprintf(stderr, "crawl: %s did=%d frontier=%u gen=%llu nseg=%d\n", g_cw.activity, did, g_cw.frontier, (unsigned long long)g_cw.gen, w->base->nseg);
        if (did < 0) { store_live_free(&L); store_writer_end(w); rc = 1; break; }
        if (did) {
            store_live_free(&L); g_cw.nseg = w->base->nseg; g_cw.gen = w->base->gen;
            cw_announce(); const double now = now_sec(); if (now - g_cw.t_state > 0.3) { cw_write_state(0); g_cw.t_state = now; }
            store_writer_end(w); usleep(2000); continue;
        }
        /* nada a fazer: fim de rodada. conta, compacta, deriva (grafo), espera o assentamento atendendo pedidos e sai se nada chegou */
        cw_counts(&L); store_live_free(&L); cw_announce();
        snprintf(g_cw.activity, sizeof g_cw.activity, "finishing");
        cw_compact(w, 8); graph_on_quiet(w->base, w);
        g_cw.nseg = w->base->nseg; g_cw.gen = w->base->gen;
        store_writer_end(w);
        snprintf(g_cw.activity, sizeof g_cw.activity, "settling"); cw_write_state(0);
        const double quiet_t = now_sec();
        while (!g_cw.stop && !g_cw.work && !g_cw.nchg && !g_cw.sweep && (now_sec() - quiet_t < settle / 1000.0 || g_cw.nsync)) cw_serve(50);
        if (g_cw.stop || !(g_cw.work || g_cw.nchg || g_cw.sweep)) break;
        compacted_n0 = 1;
    }
    maint_on_crawl_end();
    for (int i = 0; i < g_cw.nsync; i++) { cw_reply(g_cw.sync[i].fd, "OK 0 partial 0 0"); close(g_cw.sync[i].fd); }
    snprintf(g_cw.activity, sizeof g_cw.activity, "finished");
    cw_write_state(1);
    if (g_cw.sockfd >= 0) close(g_cw.sockfd);
    if (g_cw.sockpath[0]) unlink(g_cw.sockpath);                 /* o socket sai antes do lock (G) */
    cw_say("[index %s: done in %.1f s: %llu files, %llu with text, %llu with symbols]\n", g_cw.root, now_sec() - g_cw.t0, (unsigned long long)g_cw.total, (unsigned long long)g_cw.n1, (unsigned long long)g_cw.n2);
    flock(g_cw.lockfd, LOCK_UN); close(g_cw.lockfd);
    return rc;
}

/* ---------- katana coder status: niveis por parte da arvore ---------- */
static int cw_is_running(const char *dir, long *pid) {
    char lp[4500]; snprintf(lp, sizeof lp, "%s/CRAWL.LOCK", dir); int fd = open(lp, O_RDWR); if (fd < 0) return 0;
    int held = flock(fd, LOCK_EX | LOCK_NB) != 0; if (!held) flock(fd, LOCK_UN); close(fd);
    (void)pid; return held;
}
typedef struct { char name[300]; uint32_t total, skip, n1, n2; } CwPart;
static void crawl_status_print(StoreReader *r, const IndexLoc *loc, const char *dir) {
    StoreLive L; const StoreSeg **sg = (const StoreSeg **)malloc(sizeof *sg * (size_t)(r->nseg + 1)); for (int i = 0; i < r->nseg; i++) sg[i] = &r->seg[i];
    store_live_build(&L, sg, r->nseg); free(sg);
    const char *pfx = loc->prefix; const size_t pl = strlen(pfx);
    uint32_t total = 0, skip = 0, n0only = 0, n1 = 0, n2 = 0, gen = 0; CwPart *parts = NULL; int np = 0, capp = 0;
    for (int i = 0; i < L.n; i++) { const StoreLiveDoc *e = &L.e[i]; if (e->dead) continue;
        const char *p = e->d.path; size_t l = e->d.plen; if (pl) { if (!cw_under(p, l, pfx, pl) || l <= pl) continue; p += pl + 1; l -= pl + 1; }
        const uint32_t lv = sdf_lv(e->d.flags); total++;
        const int sk = (e->d.flags & SDF_SKIPMASK) != 0; if (sk) skip++;
        const int d1 = !sk && (lv == 0 || lv >= CW_N1), d2 = !sk && (lv == 0 || lv >= CW_FULL); if (d1) n1++; if (d2) n2++; if (!sk && lv == CW_N0) n0only++;
        const char *sl = (const char *)memchr(p, '/', l); size_t nl = sl ? (size_t)(sl - p) : 0; char nm[300]; if (nl >= sizeof nm) nl = sizeof nm - 1; if (sl) { memcpy(nm, p, nl); nm[nl] = 0; } else snprintf(nm, sizeof nm, "(files at the top)");
        int k = 0; while (k < np && strcmp(parts[k].name, nm)) k++;
        if (k == np) { if (np == capp) { capp = capp ? capp * 2 : 32; parts = (CwPart *)realloc(parts, sizeof *parts * (size_t)capp); } memset(&parts[np], 0, sizeof parts[np]); snprintf(parts[np].name, sizeof parts[np].name, "%s", nm); np++; }
        parts[k].total++; if (sk) parts[k].skip++; if (d1) parts[k].n1++; if (d2) parts[k].n2++;
    }
    (void)gen;
    const uint32_t el = total - skip; char lim[120] = "";
    uint64_t ep = 0, wh = 0, cn[4] = {0}; uint32_t fr = 0, fl = 0; int have = 0;
    for (int s = r->nseg - 1; s >= 0 && !have; s--) have = cw_crawl_parse(store_seg_section(&r->seg[s], STS_CRAWL), &ep, &wh, cn, &fr, &fl, lim, sizeof lim);
    printf("niveis do indice (%s):\n", pl ? pfx : "raiz inteira");
    printf("  N0 arvore    %u arquivos catalogados (%u sem leitura: binarios, grandes, ilegiveis)\n", total, skip);
    printf("  N1 texto     %u de %u elegiveis (%u%%)\n", n1, el, el ? n1 * 100 / el : 100);
    printf("  N2 simbolos  %u de %u elegiveis (%u%%)\n", n2, el, el ? n2 * 100 / el : 100);
    printf("  N3 ligacoes  %u de %u elegiveis (%u%%)   (saem da mesma analise dos simbolos)\n", n2, el, el ? n2 * 100 / el : 100);
    if (n0only) printf("  %u arquivo(s) ainda so no N0\n", n0only);
    { uint32_t cnt[CW_NSTATE] = {0}; CwDirs d;
      memset(&g_cw, 0, sizeof g_cw); cw_tab_rebuild();      /* tabela de pastas so para tirar duplicatas: o registro da secao mais nova vale */
      for (int s = 0; s < r->nseg; s++) if (cw_dirs_open(store_seg_section(&r->seg[s], STS_DIRS), &d))
          for (uint32_t i = 0; i < d.t.n; i++) { const uint8_t *rec = store_table_at(&d.t, i); uint32_t l; const char *p = cw_dirs_str(&d, rec, 0, &l);
              if (pl && !cw_under(p, l, pfx, pl)) continue;
              int k = cw_dir_add(p, l, -1, 0); g_cw.d[k].state = st_get_u32(rec + 12); }
      const int nt = g_cw.nd;
      for (int i = 0; i < nt; i++) { if (g_cw.d[i].state < CW_NSTATE) cnt[g_cw.d[i].state]++; free(g_cw.d[i].path); }
      free(g_cw.d); free(g_cw.tab); memset(&g_cw, 0, sizeof g_cw);
      printf("  pastas:");
      for (int s = 0; s < CW_NSTATE; s++) if (cnt[s]) printf(" %u %s;", cnt[s], g_cw_state[s]);
      if (!nt) printf(" (sem registro de pastas: indice feito pelo `coder index`)");
      printf("\n"); }
    if (np > 1 || (np == 1 && strcmp(parts[0].name, "(files at the top)"))) {
        for (int i = 1; i < np; i++) { CwPart t = parts[i]; int j = i - 1; while (j >= 0 && parts[j].total < t.total) { parts[j + 1] = parts[j]; j--; } parts[j + 1] = t; }
        printf("  por parte da arvore (maiores primeiro):\n");
        for (int i = 0; i < np && i < 14; i++) { uint32_t e2 = parts[i].total - parts[i].skip;
            printf("    %-28s %6u arquivos   N1 %3u%%   N2/N3 %3u%%\n", parts[i].name, parts[i].total, e2 ? parts[i].n1 * 100 / e2 : 100, e2 ? parts[i].n2 * 100 / e2 : 100); }
        if (np > 14) printf("    (+%d partes)\n", np - 14);
    }
    if (have) printf("  ultimo lote do rastreador: epoca %llu, %s%s%s\n", (unsigned long long)ep, (fl & 1) ? "todos os niveis completos" : "em andamento", lim[0] ? "; limite: " : "", lim);
    watch_status_print(loc->root);
    long pid = 0; const int run = cw_is_running(dir, &pid);
    printf("rastreador: %s\n", run ? "em execucao (CRAWL.LOCK tomado)" : "nao esta rodando");
    { char h[1000]; agent_home(h, sizeof h); char sp[1300]; snprintf(sp, sizeof sp, "%s/.katana/run/coder-%016llx.state", h, (unsigned long long)store_hash64(loc->root, strlen(loc->root), 0));
      FILE *f = fopen(sp, "r"); if (f) { char ln[300]; while (fgets(ln, sizeof ln, f)) if (!strncmp(ln, "activity ", 9) || !strncmp(ln, "pid ", 4) || !strncmp(ln, "finished ", 9)) printf("  estado: %s", ln); fclose(f); } }
    free(parts); store_live_free(&L);
}
#else
static void coder_crawl_register(void) {}
static int coder_crawl_verify_seg(const StoreSeg *s, char *err, size_t ecap) { (void)s; (void)err; (void)ecap; return 0; }
static void crawl_status_print(StoreReader *r, const IndexLoc *loc, const char *dir) { (void)r; (void)loc; (void)dir; }
static int coder_crawl_main(int argc, char **argv) { (void)argc; (void)argv; fprintf(stderr, "crawl: not available on this platform\n"); return 2; }
#endif
