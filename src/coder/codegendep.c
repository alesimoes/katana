/* ---------- coder/codegendep.c: Codegen, grafo de dependencia e code cache (docs/codegen.md, "Dependencia e cache") ----------
 * 1) FORMA CANONICA: texto determinista da UIR (sem linhas, sem espacos, sem JSON-ou-code spec, sem comentarios quando eles nao sairao no codigo);
 *    o hash FNV-1a de 64 bits dela identifica a unidade (funcao, classe, constante, bloco raw) independente de como foi escrita.
 * 2) GRAFO: nos = unidades de um ou mais modulos; arestas = chamada, uso de tipo, leitura de constante, tipo de campo, import. Tarjan iterativo
 *    (SCCs), ordem topologica ESTAVEL (dependencias primeiro; empate pela ordem de declaracao), ciclos com mensagem que diz o caminho e como
 *    quebrar: constantes e campos que se contem sao erro; recursao entre funcoes e um grupo permitido (ou erro com allow_rec=0).
 * 3) CODE CACHE em disco: uma entrada por unidade; chave = hash(forma canonica da unidade + chaves das dependencias, transitivas porque cada
 *    chave ja contem as das suas + alvo + versao do gerador + resumo do registro de operacoes + opcoes). Mudou uma funcao: a chave dela e a
 *    dos dependentes mudam, o resto continua valendo. Escrita atomica (arquivo temporario + rename), checksum no cabecalho (corrompido = erro
 *    de leitura = regenera), recencia = mtime (LRU), limite de tamanho com despejo dos menos recentes. */
#include <stdint.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#endif

#ifndef CGX_GEN_VERSION
#define CGX_GEN_VERSION 1
#endif
#define CGX_GEN_VERSION_DOC 1                                   /* sobe sempre que um backend ou o pretty-printer muda */

/* ---- hash ---- */
static uint64_t cgx_fnv(uint64_t h, const void *p, size_t n) { const unsigned char *s = (const unsigned char *)p; for (size_t i = 0; i < n; i++) h = (h ^ s[i]) * 1099511628211ull; return h; }
#define CGX_FNV0 14695981039346656037ull
static uint64_t cgx_fnv_s(uint64_t h, const char *s) { return cgx_fnv(h, s, strlen(s)); }
static uint64_t cgx_fnv_u(uint64_t h, uint64_t v) { return cgx_fnv(h, &v, 8); }
static uint64_t cgx_mix64(uint64_t x) { x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull; x ^= x >> 27; x *= 0x94d049bb133111ebull; x ^= x >> 31; return x; }
static double cgx_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6; }

/* ---- forma canonica ---- */
static void cgx_canon_ty(const CgxTy *t, CgxB *o) { cgx_bf(o, "<%d,%d,%s>", t->k, t->e, t->k == CGT_CLASS ? t->cls : ""); }
static void cgx_canon_node(const CgxNode *n, CgxB *o, int comments) {
    if (!n) { cgx_bs(o, "_"); return; }
    if (!comments && (n->k == CGN_COMMENT || n->k == CGN_DOC)) { cgx_bs(o, "_"); return; }
    cgx_bf(o, "(%d %d %d", n->k, n->op, n->flag);
    if (n->s) cgx_bf(o, " s%zu:%s", strlen(n->s), n->s);
    if (n->s2) cgx_bf(o, " t%zu:%s", strlen(n->s2), n->s2);
    if (n->k == CGN_INT || n->k == CGN_BOOL || n->k == CGN_TEST || n->k == CGN_TRY) cgx_bf(o, " i%lld", n->iv);
    if (n->k == CGN_FLT) cgx_bf(o, " d%.17g", n->dv);
    if (n->has_ty) cgx_canon_ty(&n->ty, o);
    if (n->k == CGN_LET || n->k == CGN_FN || n->k == CGN_FIELDDECL || n->k == CGN_VAR || n->k == CGN_CALL) { cgx_bf(o, " y"); cgx_canon_ty(&n->ty, o); }
    if (n->defs) cgx_bf(o, " f%zu:%s", strlen(n->defs), n->defs);
    for (int i = 0; i < 4; i++) if (n->raw[i]) cgx_bf(o, " r%d:%zu:%s", i, strlen(n->raw[i]), n->raw[i]);
    for (int i = 0; i < n->npar; i++) { cgx_bf(o, " p%s", n->par[i].name); if (n->par[i].has_ty) cgx_canon_ty(&n->par[i].ty, o); if (n->par[i].def) { cgx_bs(o, "="); cgx_canon_node(n->par[i].def, o, comments); } }
    const CgxNode *kid[3] = {n->a, n->b, n->c};
    for (int i = 0; i < 3; i++) if (kid[i]) { cgx_bf(o, " %c", 'a' + i); cgx_canon_node(kid[i], o, comments); }
    const CgxBlk *bl[3] = {&n->b1, &n->b2, &n->b3};
    for (int k = 0; k < 3; k++) if (bl[k]->n) { cgx_bf(o, " [%d", k + 1); for (int i = 0; i < bl[k]->n; i++) { if (!comments && (bl[k]->p[i]->k == CGN_COMMENT || bl[k]->p[i]->k == CGN_DOC)) continue; cgx_bs(o, " "); cgx_canon_node(bl[k]->p[i], o, comments); } cgx_bs(o, "]"); }
    cgx_bs(o, ")");
}
static uint64_t cgx_canon_hash(const CgxNode *n, int comments) { CgxB b = {0}; cgx_canon_node(n, &b, comments); const uint64_t h = cgx_fnv(CGX_FNV0, b.p ? b.p : "", b.n); free(b.p); return h; }
/* dump da UIR inteira (declaracoes ordenadas pelo nome: a ordem de declaracao nao muda a forma) */
static void cgx_dump(const Cgx *g, int comments, CgxB *o) {
    const CgxNode *pr = g->prog; int n = pr->b1.n; const CgxNode **v = (const CgxNode **)malloc(sizeof(void *) * (size_t)(n ? n : 1)); int m = 0;
    for (int i = 0; i < n; i++) if (pr->b1.p[i]->k != CGN_TEST) v[m++] = pr->b1.p[i];
    for (int i = 1; i < m; i++) { const CgxNode *x = v[i]; int j = i - 1; while (j >= 0 && strcmp(v[j]->s ? v[j]->s : "", x->s ? x->s : "") > 0) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    for (int i = 0; i < m; i++) { cgx_canon_node(v[i], o, comments); cgx_bs(o, "\n"); }
    free(v);
}

/* ---- grafo ---- */
enum { CDK_CALL, CDK_TYPE, CDK_USE, CDK_FIELD, CDK_IMPORT };              /* USE (constante lida) e FIELD (tipo de campo) sao as arestas fortes */
static const char *const CDK_NAME[] = {"call", "type", "use", "field", "import"};
enum { CDN_MODULE = 100 };
typedef struct { char *name; int kind, decl, line, mod, scc; const CgxNode *node; uint64_t canon, key; } CgxDepNode;
typedef struct { int from, to, kind, line; } CgxDepEdge;
typedef struct { int from; char *name; int kind, line, mod; } CgxDepRef;
typedef struct {
    CgxDepNode *n; int nn, cn;
    CgxDepEdge *e; int ne, ce;
    CgxDepRef *r; int nr, cr;
    char **mods; int nmod;
    int *scc, nscc; int *adj, *adjo;                                   /* adjacencia de saida (CSR) apos finish */
    int finished, comments;
} CgxDep;
static void cgx_dep_init(CgxDep *d, int comments) { memset(d, 0, sizeof *d); d->comments = comments; }
static void cgx_dep_free(CgxDep *d) {
    for (int i = 0; i < d->nn; i++) free(d->n[i].name);
    for (int i = 0; i < d->nr; i++) free(d->r[i].name);
    for (int i = 0; i < d->nmod; i++) free(d->mods[i]);
    free(d->n); free(d->e); free(d->r); free(d->mods); free(d->scc); free(d->adj); free(d->adjo); memset(d, 0, sizeof *d);
}
static int cgx_dep_node_add(CgxDep *d, const char *name, int kind, int decl, int line, int mod, const CgxNode *nd, uint64_t canon) {
    if (d->nn == d->cn) { d->cn = d->cn ? d->cn * 2 : 32; d->n = (CgxDepNode *)realloc(d->n, sizeof(CgxDepNode) * (size_t)d->cn); }
    CgxDepNode *x = &d->n[d->nn]; memset(x, 0, sizeof *x); x->name = strdup(name); x->kind = kind; x->decl = decl; x->line = line; x->mod = mod; x->node = nd; x->canon = canon;
    return d->nn++;
}
static void cgx_dep_edge_add(CgxDep *d, int from, int to, int kind, int line) {
    if (d->ne == d->ce) { d->ce = d->ce ? d->ce * 2 : 64; d->e = (CgxDepEdge *)realloc(d->e, sizeof(CgxDepEdge) * (size_t)d->ce); }
    d->e[d->ne].from = from; d->e[d->ne].to = to; d->e[d->ne].kind = kind; d->e[d->ne].line = line; d->ne++;
}
static void cgx_dep_ref_add(CgxDep *d, int from, const char *name, int kind, int line, int mod) {
    for (int i = d->nr - 1; i >= 0 && d->r[i].from == from; i--) if (d->r[i].kind == kind && !strcmp(d->r[i].name, name)) return;   /* mesma referencia da mesma unidade */
    if (d->nr == d->cr) { d->cr = d->cr ? d->cr * 2 : 64; d->r = (CgxDepRef *)realloc(d->r, sizeof(CgxDepRef) * (size_t)d->cr); }
    d->r[d->nr].from = from; d->r[d->nr].name = strdup(name); d->r[d->nr].kind = kind; d->r[d->nr].line = line; d->r[d->nr].mod = mod; d->nr++;
}

/* nomes locais de uma unidade (parametros, let, laco, catch): uma variavel com esse nome nao e leitura de constante do modulo */
typedef struct { const char **p; int n, cap; } CgxNameSet;
static void cgx_ns_add(CgxNameSet *s, const char *n) { if (!n) return; for (int i = 0; i < s->n; i++) if (!strcmp(s->p[i], n)) return; if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 16; s->p = (const char **)realloc(s->p, sizeof(char *) * (size_t)s->cap); } s->p[s->n++] = n; }
static int cgx_ns_has(const CgxNameSet *s, const char *n) { for (int i = 0; i < s->n; i++) if (!strcmp(s->p[i], n)) return 1; return 0; }
static void cgx_dep_locals(const CgxNode *n, CgxNameSet *s) {
    if (!n) return;
    if (n->k == CGN_LET || n->k == CGN_FOR || n->k == CGN_EACH) cgx_ns_add(s, n->s);
    if (n->k == CGN_EACH || n->k == CGN_TRY) cgx_ns_add(s, n->s2);
    for (int i = 0; i < n->npar; i++) cgx_ns_add(s, n->par[i].name);
    cgx_dep_locals(n->a, s); cgx_dep_locals(n->b, s); cgx_dep_locals(n->c, s);
    const CgxBlk *bl[3] = {&n->b1, &n->b2, &n->b3};
    for (int k = 0; k < 3; k++) for (int i = 0; i < bl[k]->n; i++) cgx_dep_locals(bl[k]->p[i], s);
}
typedef struct { CgxDep *d; int from, mod; const CgxNameSet *loc; int is_const; } CgxDepW;
static void cgx_dep_ty(CgxDepW *w, const CgxTy *t, int line, int kind) { if (t->k == CGT_CLASS && t->cls[0] && strcmp(t->cls, "Error")) cgx_dep_ref_add(w->d, w->from, t->cls, kind, line, w->mod); }
static void cgx_dep_walk(CgxDepW *w, const CgxNode *n, int top) {
    if (!n) return;
    switch (n->k) {
        case CGN_CALL: if (!n->bi) cgx_dep_ref_add(w->d, w->from, n->s, CDK_CALL, n->line, w->mod); break;
        case CGN_VAR: if (!cgx_ns_has(w->loc, n->s)) cgx_dep_ref_add(w->d, w->from, n->s, CDK_USE, n->line, w->mod); break;
        case CGN_MCALL: if (n->a && n->a->ty.k == CGT_CLASS) cgx_dep_ty(w, &n->a->ty, n->line, CDK_TYPE); break;
        case CGN_FIELDDECL: if (n->has_ty) cgx_dep_ty(w, &n->ty, n->line, CDK_FIELD); break;
        case CGN_LET: if (n->has_ty) cgx_dep_ty(w, &n->ty, n->line, CDK_TYPE); break;
        case CGN_FN: if (n->has_ty) cgx_dep_ty(w, &n->ty, n->line, CDK_TYPE); break;
        default: break;
    }
    for (int i = 0; i < n->npar; i++) { if (n->par[i].has_ty) cgx_dep_ty(w, &n->par[i].ty, n->par[i].line, CDK_TYPE); cgx_dep_walk(w, n->par[i].def, 0); }
    cgx_dep_walk(w, n->a, 0); cgx_dep_walk(w, n->b, 0); cgx_dep_walk(w, n->c, 0);
    const CgxBlk *bl[3] = {&n->b1, &n->b2, &n->b3};
    for (int k = 0; k < 3; k++) for (int i = 0; i < bl[k]->n; i++) cgx_dep_walk(w, bl[k]->p[i], 0);
    (void)top;
}
/* acrescenta as unidades de uma especificacao como um modulo; devolve o indice do modulo */
static int cgx_dep_add_spec(CgxDep *d, Cgx *g, const char *modname) {
    const int mod = d->nmod++; d->mods = (char **)realloc(d->mods, sizeof(char *) * (size_t)d->nmod); d->mods[mod] = strdup(modname ? modname : "main");
    const CgxNode *pr = g->prog; const int first = d->nn;
    for (int i = 0; i < pr->b1.n; i++) {
        const CgxNode *u = pr->b1.p[i]; char nm[160];
        if (u->k == CGN_TEST) continue;
        if (u->k == CGN_FN || u->k == CGN_CLASS || u->k == CGN_LET) snprintf(nm, sizeof nm, "%s", u->s);
        else if (u->k == CGN_RAW) snprintf(nm, sizeof nm, "raw@%d", u->line);
        else snprintf(nm, sizeof nm, "comment@%d", u->line);
        cgx_dep_node_add(d, nm, u->k, i, u->line, mod, u, cgx_canon_hash(u, d->comments));
    }
    for (int x = first; x < d->nn; x++) {
        const CgxNode *u = d->n[x].node; CgxNameSet loc = {0};
        cgx_dep_locals(u, &loc);
        if (u->k == CGN_LET) cgx_ns_add(&loc, NULL);
        CgxDepW w = {d, x, mod, &loc, u->k == CGN_LET};
        if (u->k == CGN_LET) { /* a propria constante nao e local dela */ }
        cgx_dep_walk(&w, u, 1);
        free(loc.p);
    }
    return mod;
}
/* import explicito entre modulos (a linguagem ainda nao tem `import`; o chamador diz quem importa quem) */
static void cgx_dep_add_import(CgxDep *d, int frommod, int tomod) {
    int a = -1, b = -1;
    for (int i = 0; i < d->nn; i++) if (d->n[i].kind == CDN_MODULE) { if (d->n[i].mod == frommod) a = i; if (d->n[i].mod == tomod) b = i; }
    if (a < 0) a = cgx_dep_node_add(d, d->mods[frommod], CDN_MODULE, -1, 0, frommod, NULL, 0);
    if (b < 0) b = cgx_dep_node_add(d, d->mods[tomod], CDN_MODULE, -1, 0, tomod, NULL, 0);
    cgx_dep_edge_add(d, a, b, CDK_IMPORT, 0);
}
/* resolve as referencias por nome (mesmo modulo primeiro) e monta a adjacencia */
static void cgx_dep_finish(CgxDep *d) {
    int hs = 1; while (hs < d->nn * 2 + 8) hs <<= 1;
    int *tab = (int *)malloc(sizeof(int) * (size_t)hs), *nxt = (int *)malloc(sizeof(int) * (size_t)(d->nn ? d->nn : 1)); memset(tab, -1, sizeof(int) * (size_t)hs);
    for (int i = d->nn - 1; i >= 0; i--) {                                 /* encadeia por nome; defs de raw tambem sao nomes */
        if (d->n[i].kind == CDN_MODULE || d->n[i].kind == CGN_COMMENT) { nxt[i] = -1; continue; }
        const char *nm = d->n[i].kind == CGN_RAW ? NULL : d->n[i].name;
        if (nm) { const unsigned h = cgx_hash(nm) & (unsigned)(hs - 1); nxt[i] = tab[h]; tab[h] = i; } else nxt[i] = -1;
    }
    /* nomes definidos por blocos raw (`defines a, b`) */
    int *rawn = NULL, nraw = 0;
    for (int i = 0; i < d->nn; i++) if (d->n[i].kind == CGN_RAW && d->n[i].node->defs && d->n[i].node->defs[0]) nraw++;
    (void)rawn;
    for (int r = 0; r < d->nr; r++) {
        const CgxDepRef *rf = &d->r[r]; int best = -1;
        for (int j = tab[cgx_hash(rf->name) & (unsigned)(hs - 1)]; j >= 0; j = nxt[j]) {
            if (strcmp(d->n[j].name, rf->name)) continue;
            if (rf->kind == CDK_USE && d->n[j].kind != CGN_LET) continue;               /* ler um nome so conta se for constante do modulo */
            if ((rf->kind == CDK_CALL) && d->n[j].kind == CGN_LET) continue;
            if (d->n[j].mod == rf->mod) { best = j; break; }
            if (best < 0) best = j;
        }
        if (best < 0 && nraw && rf->kind == CDK_USE) {                                  /* constante definida por raw */
            for (int j = 0; j < d->nn; j++) if (d->n[j].kind == CGN_RAW && d->n[j].mod == rf->mod && d->n[j].node->defs) {
                char t[300]; snprintf(t, sizeof t, "%s", d->n[j].node->defs);
                for (char *tok = strtok(t, ", "); tok; tok = strtok(NULL, ", ")) if (!strcmp(tok, rf->name)) best = j;
            }
        }
        if (best >= 0) cgx_dep_edge_add(d, rf->from, best, rf->kind, rf->line);
        if (best >= 0 && d->n[best].mod != d->n[rf->from].mod) {                        /* aresta de modulo derivada */
            int a = -1, b = -1;
            for (int i = 0; i < d->nn; i++) if (d->n[i].kind == CDN_MODULE) { if (d->n[i].mod == d->n[rf->from].mod) a = i; if (d->n[i].mod == d->n[best].mod) b = i; }
            if (a < 0) a = cgx_dep_node_add(d, d->mods[d->n[rf->from].mod], CDN_MODULE, -1, 0, d->n[rf->from].mod, NULL, 0);
            if (b < 0) b = cgx_dep_node_add(d, d->mods[d->n[best].mod], CDN_MODULE, -1, 0, d->n[best].mod, NULL, 0);
            cgx_dep_edge_add(d, a, b, CDK_IMPORT, rf->line);
        }
    }
    free(tab); free(nxt);
    d->adjo = (int *)calloc((size_t)d->nn + 2, sizeof(int)); d->adj = (int *)malloc(sizeof(int) * (size_t)(d->ne ? d->ne : 1));
    for (int i = 0; i < d->ne; i++) d->adjo[d->e[i].from + 1]++;
    for (int i = 0; i < d->nn; i++) d->adjo[i + 1] += d->adjo[i];
    int *pos = (int *)malloc(sizeof(int) * (size_t)(d->nn + 1)); memcpy(pos, d->adjo, sizeof(int) * (size_t)(d->nn + 1));
    for (int i = 0; i < d->ne; i++) d->adj[pos[d->e[i].from]++] = d->e[i].to;
    free(pos); d->finished = 1;
}
/* Tarjan iterativo; sub: so as arestas fortes (USE, FIELD). Preenche scc[] (ids em ordem de emissao: dependencias primeiro) e devolve o numero */
static int cgx_dep_tarjan(const CgxDep *d, int strong_only, int *scc) {
    const int n = d->nn; int *idx = (int *)malloc(sizeof(int) * (size_t)(n + 1)), *low = (int *)malloc(sizeof(int) * (size_t)(n + 1)), *stk = (int *)malloc(sizeof(int) * (size_t)(n + 1));
    int *cs = (int *)malloc(sizeof(int) * (size_t)(n + 1)), *cp = (int *)malloc(sizeof(int) * (size_t)(n + 1)); unsigned char *on = (unsigned char *)calloc((size_t)n + 1, 1);
    /* arestas fortes: lista propria quando strong_only */
    int *so = NULL, *sa = NULL;
    if (strong_only) {
        so = (int *)calloc((size_t)n + 2, sizeof(int)); sa = (int *)malloc(sizeof(int) * (size_t)(d->ne ? d->ne : 1));
        for (int i = 0; i < d->ne; i++) if (d->e[i].kind == CDK_USE || d->e[i].kind == CDK_FIELD) so[d->e[i].from + 1]++;
        for (int i = 0; i < n; i++) so[i + 1] += so[i];
        int *pos = (int *)malloc(sizeof(int) * (size_t)(n + 1)); memcpy(pos, so, sizeof(int) * (size_t)(n + 1));
        for (int i = 0; i < d->ne; i++) if (d->e[i].kind == CDK_USE || d->e[i].kind == CDK_FIELD) sa[pos[d->e[i].from]++] = d->e[i].to;
        free(pos);
    }
    const int *ao = strong_only ? so : d->adjo, *aa = strong_only ? sa : d->adj;
    for (int i = 0; i < n; i++) { idx[i] = -1; scc[i] = -1; }
    int counter = 0, sp = 0, ns = 0;
    for (int s = 0; s < n; s++) {
        if (idx[s] >= 0) continue;
        int top = 0; cs[0] = s; cp[0] = ao[s]; idx[s] = low[s] = counter++; stk[sp++] = s; on[s] = 1;
        while (top >= 0) {
            const int v = cs[top];
            if (cp[top] < ao[v + 1]) {
                const int w = aa[cp[top]++];
                if (idx[w] < 0) { idx[w] = low[w] = counter++; stk[sp++] = w; on[w] = 1; cs[++top] = w; cp[top] = ao[w]; }
                else if (on[w] && idx[w] < low[v]) low[v] = idx[w];
            } else {
                if (low[v] == idx[v]) { int w; do { w = stk[--sp]; on[w] = 0; scc[w] = ns; } while (w != v); ns++; }
                top--;
                if (top >= 0 && low[v] < low[cs[top]]) low[cs[top]] = low[v];
            }
        }
    }
    free(idx); free(low); free(stk); free(cs); free(cp); free(on); free(so); free(sa);
    return ns;
}
static int cgx_dep_selfloop(const CgxDep *d, int v) { for (int i = d->adjo[v]; i < d->adjo[v + 1]; i++) if (d->adj[i] == v) return 1; return 0; }

/* ordem topologica estavel das unidades (dependencias primeiro; empate: menor indice = ordem de declaracao). Devolve quantos nos escreveu em out. */
typedef struct { int *h; int n; const int *pri; } CgxHeap;
static void cgx_hp_push(CgxHeap *p, int v) { int i = p->n++; p->h[i] = v; while (i > 0) { int up = (i - 1) / 2; if (p->pri[p->h[up]] <= p->pri[p->h[i]]) break; int t = p->h[up]; p->h[up] = p->h[i]; p->h[i] = t; i = up; } }
static int cgx_hp_pop(CgxHeap *p) {
    int r = p->h[0]; p->h[0] = p->h[--p->n]; int i = 0;
    for (;;) { int l = 2 * i + 1, rr = l + 1, m = i; if (l < p->n && p->pri[p->h[l]] < p->pri[p->h[m]]) m = l; if (rr < p->n && p->pri[p->h[rr]] < p->pri[p->h[m]]) m = rr; if (m == i) break; int t = p->h[m]; p->h[m] = p->h[i]; p->h[i] = t; i = m; }
    return r;
}
static int cgx_dep_order(CgxDep *d, int *out) {
    if (!d->finished) cgx_dep_finish(d);
    free(d->scc); d->scc = (int *)malloc(sizeof(int) * (size_t)(d->nn + 1));
    d->nscc = cgx_dep_tarjan(d, 0, d->scc);
    const int ns = d->nscc, n = d->nn;
    int *pri = (int *)malloc(sizeof(int) * (size_t)(ns + 1)), *deg = (int *)calloc((size_t)ns + 1, sizeof(int)), *mo = (int *)calloc((size_t)ns + 2, sizeof(int)), *mem = (int *)malloc(sizeof(int) * (size_t)(n + 1));
    for (int i = 0; i < ns; i++) pri[i] = n + 1;
    for (int v = 0; v < n; v++) { d->n[v].scc = d->scc[v]; if (v < pri[d->scc[v]]) pri[d->scc[v]] = v; mo[d->scc[v] + 1]++; }
    for (int i = 0; i < ns; i++) mo[i + 1] += mo[i];
    { int *pos = (int *)malloc(sizeof(int) * (size_t)(ns + 1)); memcpy(pos, mo, sizeof(int) * (size_t)(ns + 1)); for (int v = 0; v < n; v++) mem[pos[d->scc[v]]++] = v; free(pos); }
    /* dependentes por SCC: aresta u -> w (u depende de w) conta uma dependencia de scc(u) */
    int *ro = (int *)calloc((size_t)ns + 2, sizeof(int)), *ra = (int *)malloc(sizeof(int) * (size_t)(d->ne ? d->ne : 1));
    for (int i = 0; i < d->ne; i++) { const int a = d->scc[d->e[i].from], b = d->scc[d->e[i].to]; if (a != b) { deg[a]++; ro[b + 1]++; } }
    for (int i = 0; i < ns; i++) ro[i + 1] += ro[i];
    { int *pos = (int *)malloc(sizeof(int) * (size_t)(ns + 1)); memcpy(pos, ro, sizeof(int) * (size_t)(ns + 1)); for (int i = 0; i < d->ne; i++) { const int a = d->scc[d->e[i].from], b = d->scc[d->e[i].to]; if (a != b) ra[pos[b]++] = a; } free(pos); }
    CgxHeap hp = {(int *)malloc(sizeof(int) * (size_t)(ns + 1)), 0, pri};
    for (int i = 0; i < ns; i++) if (!deg[i]) cgx_hp_push(&hp, i);
    int w = 0;
    while (hp.n) {
        const int c = cgx_hp_pop(&hp);
        for (int i = mo[c]; i < mo[c + 1]; i++) out[w++] = mem[i];                       /* membros em ordem de indice */
        for (int i = ro[c]; i < ro[c + 1]; i++) if (--deg[ra[i]] == 0) cgx_hp_push(&hp, ra[i]);
    }
    free(pri); free(deg); free(mo); free(mem); free(ro); free(ra); free(hp.h);
    return w;
}

/* caminho concreto dentro de um conjunto de nos que se alcancam: from -> ... -> from (BFS limitada ao SCC) */
static int cgx_dep_path(const CgxDep *d, const int *scc, int comp, int strong, int from, int *path, int cap) {
    int *prev = (int *)malloc(sizeof(int) * (size_t)(d->nn + 1)), *q = (int *)malloc(sizeof(int) * (size_t)(d->nn + 1)), *pe = (int *)malloc(sizeof(int) * (size_t)(d->nn + 1));
    for (int i = 0; i < d->nn; i++) prev[i] = -2;
    int h = 0, t = 0, found = -1; q[t++] = from;
    while (h < t && found < 0) {
        const int v = q[h++];
        for (int e = 0; e < d->ne; e++) {
            if (d->e[e].from != v) continue;
            if (strong && d->e[e].kind != CDK_USE && d->e[e].kind != CDK_FIELD) continue;
            const int w = d->e[e].to; if (scc[w] != comp) continue;
            if (w == from) { found = v; pe[from] = e; break; }
            if (prev[w] == -2) { prev[w] = v; pe[w] = e; q[t++] = w; }
        }
    }
    int len = 0;
    if (found >= 0) { int tmp[64], k = 0; for (int v = found; v != from && k < 62; v = prev[v]) tmp[k++] = v; path[len++] = from; for (int i = k - 1; i >= 0 && len < cap; i--) path[len++] = tmp[i]; }
    free(prev); free(q); free(pe);
    return len;
}
static int cgx_dep_edge_line(const CgxDep *d, int a, int b, int strong) { for (int e = 0; e < d->ne; e++) if (d->e[e].from == a && d->e[e].to == b && (!strong || d->e[e].kind == CDK_USE || d->e[e].kind == CDK_FIELD)) return d->e[e].line; return 0; }
/* acha os ciclos e escreve uma mensagem acionavel por ciclo. Devolve o numero de ERROS (ciclos de constantes/campos; funcoes so se allow_rec == 0);
 * os grupos de recursao permitidos vao como linhas `note:` em msg e a contagem em *nrec. */
static int cgx_dep_cycles(CgxDep *d, int allow_rec, CgxB *msg, int *nrec) {
    if (!d->finished) cgx_dep_finish(d);
    const int n = d->nn; int errs = 0; if (nrec) *nrec = 0;
    int *full = (int *)malloc(sizeof(int) * (size_t)(n + 1)), *strong = (int *)malloc(sizeof(int) * (size_t)(n + 1));
    const int nf = cgx_dep_tarjan(d, 0, full), nst = cgx_dep_tarjan(d, 1, strong);
    int *cnt = (int *)calloc((size_t)(nst > nf ? nst : nf) + 1, sizeof(int));
    unsigned char *reported = (unsigned char *)calloc((size_t)n + 1, 1);
    /* erros: ciclos entre arestas fortes (constante lendo constante, campo de tipo que contem a si) */
    for (int v = 0; v < n; v++) cnt[strong[v]]++;
    for (int v = 0; v < n; v++) {
        int self = 0; for (int e = 0; e < d->ne; e++) if (d->e[e].from == v && d->e[e].to == v && (d->e[e].kind == CDK_USE || d->e[e].kind == CDK_FIELD)) self = 1;
        if (reported[v] || (cnt[strong[v]] < 2 && !self)) continue;
        int path[64]; const int len = cgx_dep_path(d, strong, strong[v], 1, v, path, 64);
        for (int i = 0; i < len; i++) reported[path[i]] = 1;
        for (int u = 0; u < n; u++) if (strong[u] == strong[v]) reported[u] = 1;
        const int isfield = d->n[v].kind == CGN_CLASS;
        cgx_bf(msg, "line %d: %s cycle: ", d->n[v].line, isfield ? "type" : "constant");
        for (int i = 0; i < len; i++) cgx_bf(msg, "%s -> ", d->n[path[i]].name);
        cgx_bf(msg, "%s", d->n[v].name);
        for (int i = 0; i < len; i++) { const int a = path[i], b = path[(i + 1) % len]; cgx_bf(msg, "%s%s (line %d) needs %s", i ? "; " : " (", d->n[a].name, cgx_dep_edge_line(d, a, b, 1), d->n[b].name); }
        cgx_bs(msg, len ? ")" : "");
        cgx_bf(msg, ". %s\n", isfield ? "A class cannot contain itself: store the other one by a key or an id, or make the field optional in a `raw` block." : "A constant cannot depend on itself: compute the shared value in one function or in a third constant that neither of these reads.");
        errs++;
    }
    /* o resto: recursao (so chamadas e tipos de assinatura) */
    memset(cnt, 0, sizeof(int) * (size_t)((nst > nf ? nst : nf) + 1));
    for (int v = 0; v < n; v++) cnt[full[v]]++;
    unsigned char *seen = (unsigned char *)calloc((size_t)nf + 1, 1);
    for (int v = 0; v < n; v++) {
        if (seen[full[v]] || d->n[v].kind == CDN_MODULE) continue;
        if (cnt[full[v]] < 2 && !cgx_dep_selfloop(d, v)) continue;
        if (reported[v]) { seen[full[v]] = 1; continue; }
        /* uma constante no ciclo pelo grafo cheio (ex.: const a = f(); f le a) e erro de inicializacao */
        int hasconst = 0; for (int u = 0; u < n; u++) if (full[u] == full[v] && d->n[u].kind == CGN_LET) hasconst = 1;
        int path[64]; const int len = cgx_dep_path(d, full, full[v], 0, v, path, 64);
        seen[full[v]] = 1;
        if (hasconst) {
            cgx_bf(msg, "line %d: initialization cycle: ", d->n[v].line);
            for (int i = 0; i < len; i++) cgx_bf(msg, "%s -> ", d->n[path[i]].name);
            cgx_bf(msg, "%s. A constant is computed once at load, before the functions it calls can read it: pass the value as an argument instead of reading the constant inside that function.\n", d->n[v].name);
            errs++; continue;
        }
        if (len == 1 || cnt[full[v]] < 2) { /* auto-recursao */
            if (allow_rec) { cgx_bf(msg, "note: `%s` is recursive (line %d)\n", d->n[v].name, d->n[v].line); if (nrec) (*nrec)++; }
            else { cgx_bf(msg, "line %d: `%s` calls itself, and recursion is not allowed here. Rewrite it as a loop.\n", d->n[v].line, d->n[v].name); errs++; }
            continue;
        }
        if (allow_rec) {
            cgx_bf(msg, "note: mutual recursion group (%d units): ", cnt[full[v]]);
            for (int i = 0; i < len; i++) cgx_bf(msg, "%s -> ", d->n[path[i]].name);
            cgx_bf(msg, "%s\n", d->n[v].name); if (nrec) (*nrec)++;
        } else {
            cgx_bf(msg, "line %d: `%s` and its callees call each other: ", d->n[v].line, d->n[v].name);
            for (int i = 0; i < len; i++) cgx_bf(msg, "%s (line %d) -> ", d->n[path[i]].name, cgx_dep_edge_line(d, path[i], path[(i + 1) % len], 0));
            cgx_bf(msg, "%s. Mutual recursion is not allowed here: merge them into one function with an extra parameter, or break the cycle with a loop.\n", d->n[v].name);
            errs++;
        }
    }
    free(full); free(strong); free(cnt); free(reported); free(seen);
    return errs;
}
/* quem depende (direta ou indiretamente) de u: marca em mark[] (1 = u e dependentes) */
static int cgx_dep_dependents(const CgxDep *d, int u, unsigned char *mark) {
    int *ro = (int *)calloc((size_t)d->nn + 2, sizeof(int)), *ra = (int *)malloc(sizeof(int) * (size_t)(d->ne ? d->ne : 1)), *q = (int *)malloc(sizeof(int) * (size_t)(d->nn + 1));
    for (int i = 0; i < d->ne; i++) ro[d->e[i].to + 1]++;
    for (int i = 0; i < d->nn; i++) ro[i + 1] += ro[i];
    { int *pos = (int *)malloc(sizeof(int) * (size_t)(d->nn + 1)); memcpy(pos, ro, sizeof(int) * (size_t)(d->nn + 1)); for (int i = 0; i < d->ne; i++) ra[pos[d->e[i].to]++] = d->e[i].from; free(pos); }
    memset(mark, 0, (size_t)d->nn); int h = 0, t = 0, c = 1; q[t++] = u; mark[u] = 1;
    while (h < t) { const int v = q[h++]; for (int i = ro[v]; i < ro[v + 1]; i++) if (!mark[ra[i]]) { mark[ra[i]] = 1; q[t++] = ra[i]; c++; } }
    free(ro); free(ra); free(q);
    return c;
}
/* chaves de cache: ordem topologica, SCC como bloco. ctx: hash de alvo + versao + registro + opcoes */
static void cgx_dep_keys(CgxDep *d, uint64_t ctx) {
    int *ord = (int *)malloc(sizeof(int) * (size_t)(d->nn + 1)); const int n = cgx_dep_order(d, ord);
    uint64_t *sk = (uint64_t *)calloc((size_t)d->nscc + 1, sizeof(uint64_t));
    for (int p = 0; p < n;) {
        const int c = d->scc[ord[p]]; int q = p; while (q < n && d->scc[ord[q]] == c) q++;
        /* hash do bloco: canonicos dos membros, ordenados (independe da ordem de declaracao) */
        uint64_t hs[256]; int m = 0; for (int i = p; i < q && m < 256; i++) hs[m++] = d->n[ord[i]].canon;
        for (int i = 1; i < m; i++) { uint64_t x = hs[i]; int j = i - 1; while (j >= 0 && hs[j] > x) { hs[j + 1] = hs[j]; j--; } hs[j + 1] = x; }
        uint64_t blk = cgx_fnv_u(CGX_FNV0, (uint64_t)m); for (int i = 0; i < m; i++) blk = cgx_fnv_u(blk, hs[i]);
        /* chaves das dependencias de fora do bloco */
        uint64_t dk[512]; int nd = 0;
        for (int i = p; i < q; i++) for (int e = d->adjo[ord[i]]; e < d->adjo[ord[i] + 1]; e++) { const int w = d->adj[e]; if (d->scc[w] == c || nd >= 512) continue; dk[nd++] = sk[d->scc[w]]; }
        for (int i = 1; i < nd; i++) { uint64_t x = dk[i]; int j = i - 1; while (j >= 0 && dk[j] > x) { dk[j + 1] = dk[j]; j--; } dk[j + 1] = x; }
        uint64_t depk = CGX_FNV0; int last = 0; uint64_t prev = 0; for (int i = 0; i < nd; i++) { if (last && dk[i] == prev) continue; depk = cgx_fnv_u(depk, dk[i]); prev = dk[i]; last = 1; }
        uint64_t h = cgx_fnv_u(cgx_fnv_u(cgx_fnv_u(CGX_FNV0, ctx), blk), depk);
        sk[c] = cgx_mix64(h);
        for (int i = p; i < q; i++) d->n[ord[i]].key = cgx_mix64(cgx_fnv_u(sk[c], d->n[ord[i]].canon));
        p = q;
    }
    free(ord); free(sk);
}
/* resumo do relatorio do grafo */
static void cgx_dep_report(CgxDep *d, CgxB *o) {
    int *ord = (int *)malloc(sizeof(int) * (size_t)(d->nn + 1)); const int n = cgx_dep_order(d, ord);
    cgx_bf(o, "units %d edges %d\norder:", d->nn, d->ne);
    for (int i = 0; i < n; i++) if (d->n[ord[i]].kind != CDN_MODULE) cgx_bf(o, " %s", d->n[ord[i]].name);
    cgx_bs(o, "\n");
    for (int e = 0; e < d->ne; e++) cgx_bf(o, "edge %s -> %s (%s%s%d)\n", d->n[d->e[e].from].name, d->n[d->e[e].to].name, CDK_NAME[d->e[e].kind], d->e[e].line ? ", line " : "", d->e[e].line);
    free(ord);
}

/* ---- code cache em disco ---- */
typedef struct { char dir[1024]; long long max_bytes; int hits, misses, corrupt, stored, evicted; long long added; int enabled; } CgxCache;
static int cgx_cache_open(CgxCache *c, const char *dir) {
    memset(c, 0, sizeof *c); c->max_bytes = 64ll << 20;
    const char *e = getenv("KATANA_KATANA_CODEGEN_CACHE_MAX"); if (e && atoll(e) > 0) c->max_bytes = atoll(e);
    if (!dir || !*dir) dir = getenv("KATANA_KATANA_CODEGEN_CACHE");
    if (dir && *dir) snprintf(c->dir, sizeof c->dir, "%s", dir);
    else { const char *h = getenv("HOME"); if (!h || !*h) return 0; snprintf(c->dir, sizeof c->dir, "%s/.katana/codegen-cache", h); }
#ifdef _WIN32
    return 0;
#else
    char t[1100]; snprintf(t, sizeof t, "%s", c->dir);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    mkdir(t, 0755);
    struct stat st; c->enabled = stat(c->dir, &st) == 0 && S_ISDIR(st.st_mode);
    return c->enabled;
#endif
}
#ifndef _WIN32
static void cgx_cache_path(const CgxCache *c, uint64_t key, char *p, size_t cap) { snprintf(p, cap, "%s/%016llx.uc", c->dir, (unsigned long long)key); }
/* valor: texto + meta (inteiro livre do chamador). 1 = acerto. Entrada corrompida e apagada e conta como erro. */
static int cgx_cache_get(CgxCache *c, uint64_t key, char **out, size_t *len, int *meta) {
    if (!c->enabled) { c->misses++; return 0; }
    char path[1400]; cgx_cache_path(c, key, path, sizeof path);
    FILE *f = fopen(path, "rb"); if (!f) { c->misses++; return 0; }
    char hdr[160]; unsigned long long k = 0, cs = 0; size_t n = 0; int m = 0; int ok = 0; char *b = NULL;
    if (fgets(hdr, sizeof hdr, f) && sscanf(hdr, "UCGC1 %llx %zu %llx %d", &k, &n, &cs, &m) == 4 && k == key && n < (64u << 20)) {
        b = (char *)malloc(n + 1);
        if (b && fread(b, 1, n, f) == n && fgetc(f) == EOF && cgx_fnv(CGX_FNV0, b, n) == cs) ok = 1;
    }
    fclose(f);
    if (!ok) { free(b); unlink(path); c->corrupt++; c->misses++; return 0; }
    b[n] = 0; *out = b; *len = n; *meta = m; c->hits++;
    utimes(path, NULL);                                                                   /* recencia para o LRU */
    return 1;
}
typedef struct { char name[256]; long long size; time_t mt; } CgxCEnt;
static int cgx_cent_cmp(const void *a, const void *b) { const CgxCEnt *x = (const CgxCEnt *)a, *y = (const CgxCEnt *)b; if (x->mt != y->mt) return x->mt < y->mt ? -1 : 1; return strcmp(x->name, y->name); }
/* despeja os menos recentes ate caber em 90% do limite; devolve o tamanho final */
static long long cgx_cache_trim(CgxCache *c) {
    if (!c->enabled) return 0;
    DIR *d = opendir(c->dir); if (!d) return 0;
    CgxCEnt *v = NULL; int n = 0, cap = 0; long long tot = 0; struct dirent *de;
    while ((de = readdir(d))) {
        const size_t l = strlen(de->d_name); if (l < 4 || strcmp(de->d_name + l - 3, ".uc") || l >= 256) continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", c->dir, de->d_name); struct stat st; if (stat(p, &st) != 0) continue;
        if (n == cap) { cap = cap ? cap * 2 : 256; v = (CgxCEnt *)realloc(v, sizeof(CgxCEnt) * (size_t)cap); }
        snprintf(v[n].name, sizeof v[n].name, "%s", de->d_name); v[n].size = st.st_size; v[n].mt = st.st_mtime; tot += st.st_size; n++;
    }
    closedir(d);
    if (tot > c->max_bytes) {
        qsort(v, (size_t)n, sizeof *v, cgx_cent_cmp);
        const long long goal = c->max_bytes / 10 * 9;
        for (int i = 0; i < n && tot > goal; i++) { char p[1400]; snprintf(p, sizeof p, "%s/%s", c->dir, v[i].name); if (unlink(p) == 0) { tot -= v[i].size; c->evicted++; } }
    }
    free(v); c->added = 0;
    return tot;
}
static int cgx_cache_put(CgxCache *c, uint64_t key, const char *val, size_t len, int meta) {
    if (!c->enabled) return 0;
    static int seq; char tmp[1400], path[1400]; cgx_cache_path(c, key, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s/.tmp.%ld.%d", c->dir, (long)getpid(), seq++);
    FILE *f = fopen(tmp, "wb"); if (!f) return 0;
    const unsigned long long cs = cgx_fnv(CGX_FNV0, val, len);
    int ok = fprintf(f, "UCGC1 %016llx %zu %016llx %d\n", (unsigned long long)key, len, cs, meta) > 0 && fwrite(val, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return 0; }
    c->stored++; c->added += (long long)len + 64;
    if (c->added > c->max_bytes / 8) cgx_cache_trim(c);                                   /* confere o tamanho a cada 1/8 do limite escrito */
    return 1;
}
static int cgx_cache_clear(CgxCache *c) {
    if (!c->enabled) return 0;
    DIR *d = opendir(c->dir); if (!d) return 0; int n = 0; struct dirent *de;
    while ((de = readdir(d))) { const size_t l = strlen(de->d_name); if (l > 3 && (!strcmp(de->d_name + l - 3, ".uc") || !strncmp(de->d_name, ".tmp.", 5))) { char p[1400]; snprintf(p, sizeof p, "%s/%s", c->dir, de->d_name); if (unlink(p) == 0) n++; } }
    closedir(d); return n;
}
#else
static int cgx_cache_get(CgxCache *c, uint64_t key, char **out, size_t *len, int *meta) { (void)key; (void)out; (void)len; (void)meta; c->misses++; return 0; }
static long long cgx_cache_trim(CgxCache *c) { (void)c; return 0; }
static int cgx_cache_put(CgxCache *c, uint64_t key, const char *val, size_t len, int meta) { (void)c; (void)key; (void)val; (void)len; (void)meta; return 0; }
static int cgx_cache_clear(CgxCache *c) { (void)c; return 0; }
#endif

/* ---- geracao com cache ---- */
typedef struct { int units, hits, misses, corrupt, whole; double ms_dep, ms_gen, ms_total; } CgxCacheRep;
static uint64_t cgx_reg_digest(void) {
    static uint64_t h; static int n = -1;
    if (n == cgx_nops && h) return h;
    CgxB b = {0}; cgx_reg_dump(&b); h = cgx_fnv(CGX_FNV0, b.p ? b.p : "", b.n); free(b.p); n = cgx_nops; return h;
}
static uint64_t cgx_cache_ctx(const CgxBackend *be, const CgxGenOpts *o, const char *flags) {
    uint64_t h = cgx_fnv_s(CGX_FNV0, be->name);
    h = cgx_fnv_u(h, CGX_GEN_VERSION); h = cgx_fnv_u(h, cgx_reg_digest());
    h = cgx_fnv_u(h, (uint64_t)o->width); h = cgx_fnv_u(h, (uint64_t)o->comments); h = cgx_fnv_u(h, (uint64_t)o->indent);
    return cgx_fnv_s(h, flags ? flags : "");
}
/* gera `g` (ja validado) para o alvo be, pelo cache. Python: por unidade (montagem identica a de cgx_gen_python, na ordem de declaracao,
 * ou na ordem topologica se topo != 0); outros alvos: uma entrada para o programa inteiro (a ordem de declaracao entra na chave). Testes nunca
 * vao ao cache. Devolve 1 se gerou sem erro. */
static int cgx_gen_cached(Cgx *g, const CgxBackend *be, const CgxGenOpts *o, CgxCache *c, const char *flags, int topo, CgxB *code, CgxB *tests, CgxCacheRep *rep) {
    const double t0 = cgx_now_ms(); CgxCacheRep R; memset(&R, 0, sizeof R);
    CgxDep d; cgx_dep_init(&d, o->comments); cgx_dep_add_spec(&d, g, "main");
    const uint64_t ctx = cgx_cache_ctx(be, o, flags);
    cgx_dep_keys(&d, ctx);
    R.ms_dep = cgx_now_ms() - t0;
    const int err0 = g->nerr_all; int ok = 1; const int h0 = c->hits, m0 = c->misses, c0 = c->corrupt;
    if (!strcmp(be->name, "python")) {
        const int nu = d.nn; char **txt = (char **)calloc((size_t)nu + 1, sizeof(char *)); int *isfn = (int *)calloc((size_t)nu + 1, sizeof(int)), *imp = (int *)calloc((size_t)nu + 1, sizeof(int)), *use = (int *)calloc((size_t)nu + 1, sizeof(int)); int m = 0;
        int *seq = (int *)malloc(sizeof(int) * (size_t)(nu + 1));
        if (topo) { const int k = cgx_dep_order(&d, seq); (void)k; } else for (int i = 0; i < nu; i++) seq[i] = i;
        /* entrada do programa inteiro (uma leitura so quando nada mudou); a chave cobre a ordem de montagem */
        uint64_t wk = cgx_fnv_u(cgx_fnv_u(ctx, 0x70797768ull), (uint64_t)topo);
        for (int s = 0; s < nu; s++) wk = cgx_fnv_u(wk, d.n[seq[s]].key);
        wk = cgx_mix64(wk); char *wv = NULL; size_t wl = 0; int wm = 0;
        { const int sm = c->misses; const int got = cgx_cache_get(c, wk, &wv, &wl, &wm); if (!got) c->misses = sm; if (got) { cgx_bn(code, wv, wl); free(wv); R.whole = 1; c->hits += nu - 1; } }
        if (!R.whole) {
        const double tg = cgx_now_ms();
        for (int s = 0; s < nu && ok; s++) {
            const CgxDepNode *u = &d.n[seq[s]]; char *v = NULL; size_t vl = 0; int meta = 0;
            if (cgx_cache_get(c, u->key, &v, &vl, &meta)) { txt[m] = v; isfn[m] = meta & 1; imp[m] = meta >> 1; m++; continue; }
            CgxB b = {0}; int f = 0, im = 0;
            if (!cgx_py_unit(g, o, u->node, &b, &f, &im)) { if (g->nerr_all != err0) ok = 0; free(b.p); continue; }   /* unidade que nao gera nada (comentario sem texto) */
            cgx_cache_put(c, u->key, b.p, b.n, f | (im << 1));
            txt[m] = b.p; isfn[m] = f; imp[m] = im; m++;
        }
        R.ms_gen = cgx_now_ms() - tg;
        if (ok) { const size_t c0 = code->n; cgx_py_assemble((const char *const *)txt, isfn, imp, m, code); cgx_cache_put(c, wk, code->p + c0, code->n - c0, 0); }
        for (int i = 0; i < m; i++) free(txt[i]);
        }
        free(txt); free(isfn); free(imp); free(use); free(seq);
        if (ok && tests) cgx_py_tests_text(g, o, tests);
        R.units = nu;
    } else {
        uint64_t wk = cgx_fnv_u(ctx, tests ? 1 : 2);
        for (int i = 0; i < d.nn; i++) wk = cgx_fnv_u(cgx_fnv_u(wk, d.n[i].key), (uint64_t)d.n[i].decl);
        { uint64_t th = CGX_FNV0; for (int i = 0; i < g->prog->b1.n; i++) if (g->prog->b1.p[i]->k == CGN_TEST) th = cgx_fnv_u(th, cgx_canon_hash(g->prog->b1.p[i], 0)); wk = cgx_fnv_u(wk, th); }
        wk = cgx_mix64(wk); char *v = NULL; size_t vl = 0; int meta = 0; R.units = 1; R.whole = 1;
        if (cgx_cache_get(c, wk, &v, &vl, &meta)) { const char *sep = memchr(v, 1, vl); size_t cl = sep ? (size_t)(sep - v) : vl; cgx_bn(code, v, cl); if (tests && sep) cgx_bs(tests, sep + 1); free(v); }
        else {
            const double tg = cgx_now_ms();
            ok = be->gen(g, o, code, tests); R.ms_gen = cgx_now_ms() - tg;
            if (ok) { CgxB pv = {0}; cgx_bs(&pv, code->p ? code->p : ""); if (tests) { cgx_bn(&pv, "\1", 1); cgx_bs(&pv, tests->p ? tests->p : ""); } cgx_cache_put(c, wk, pv.p ? pv.p : "", pv.n, 0); free(pv.p); }
        }
    }
    R.hits = c->hits - h0; R.misses = c->misses - m0; R.corrupt = c->corrupt - c0;
    cgx_dep_free(&d); R.ms_total = cgx_now_ms() - t0;
    if (rep) *rep = R;
    return ok;
}
