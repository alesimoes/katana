/* ---------- coder/ucgecfg.c: UCGE, UIR-G parte 1: grafos, dominadores, SSA e CFG por funcao (docs/ucge.md, "UIR-G") ----------
 * A UIR-H (AST validada) vira, por funcao, um GRAFO DE FLUXO DE CONTROLE de blocos basicos com instrucoes de tres enderecos; o grafo e a
 * base das analises (ucgeflow.c) e do otimizador (ucgeopt.c). Os backends NAO leem este grafo: ele existe para analisar e para guiar
 * reescritas que voltam para a AST, entao o codigo gerado continua saindo da AST e com os nomes do autor.
 * Conteudo, na ordem:
 *   1. vetores, conjuntos de bits e o grafo dirigido em CSR (sucessores e predecessores), geral: serve a qualquer grafo (CFG, chamadas);
 *   2. Tarjan (componentes fortemente conexas, iterativo), ordem pos-ordem reversa, DOMINADORES por Cooper-Harvey-Kennedy, arvore de
 *      dominancia, FRONTEIRA DE DOMINANCIA, lacos naturais;
 *   3. efeitos das expressoes (puro, pode lancar, escreve, pode nao terminar) e GRAFO DE CHAMADAS com resumos por funcao;
 *   4. a IR: valores, variaveis, instrucoes, blocos; construcao do CFG a partir da AST (try/catch/finally com arestas excepcionais);
 *   5. SSA de Cytron (phi podada por vivacidade, renomeacao pela arvore de dominancia), saida de SSA, verificador, dump.
 * C puro, so a biblioteca padrao; compila junto de ucge.c e ucgecheck.c (usa Cgx, CgxNode, cgx_alloc... de la). */
#include <limits.h>

/* ================= 1. vetores, bits e grafo em CSR ================= */
typedef struct { int *p; int n, cap; } CgxVi;
static void cgx_vi_push(CgxVi *v, int x) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 8; v->p = (int *)realloc(v->p, sizeof(int) * (size_t)v->cap); if (!v->p) { fprintf(stderr, "ucge: sem memoria\n"); exit(1); } }
    v->p[v->n++] = x;
}
static void cgx_vi_free(CgxVi *v) { free(v->p); v->p = NULL; v->n = v->cap = 0; }
static void *cgx_xcalloc(size_t n, size_t sz) { void *p = calloc(n ? n : 1, sz ? sz : 1); if (!p) { fprintf(stderr, "ucge: sem memoria\n"); exit(1); } return p; }
static void *cgx_xgrow(void *p, int *cap, int need, size_t sz) {          /* cresce um vetor generico ate caber `need` elementos (zerando o novo) */
    if (need <= *cap) return p;
    int nc = *cap ? *cap : 8; while (nc < need) nc *= 2;
    p = realloc(p, sz * (size_t)nc); if (!p) { fprintf(stderr, "ucge: sem memoria\n"); exit(1); }
    memset((char *)p + sz * (size_t)*cap, 0, sz * (size_t)(nc - *cap)); *cap = nc; return p;
}

typedef unsigned long long CgxW;                                           /* palavra de um conjunto de bits */
#define CGX_WB 64
static int cgx_nw(int nbits) { return (nbits + CGX_WB - 1) / CGX_WB; }
static CgxW *cgx_bit_new(int nbits, int n) { return (CgxW *)cgx_xcalloc((size_t)cgx_nw(nbits) * (size_t)(n ? n : 1), sizeof(CgxW)); }   /* n conjuntos de nbits, contiguos */
static int cgx_bit_get(const CgxW *s, int i) { return (int)((s[i / CGX_WB] >> (i % CGX_WB)) & 1u); }
static void cgx_bit_set(CgxW *s, int i) { s[i / CGX_WB] |= 1ull << (i % CGX_WB); }
static void cgx_bit_clr(CgxW *s, int i) { s[i / CGX_WB] &= ~(1ull << (i % CGX_WB)); }
static int cgx_bit_count(const CgxW *s, int nw) { int c = 0; for (int i = 0; i < nw; i++) { CgxW x = s[i]; while (x) { x &= x - 1; c++; } } return c; }

/* grafo dirigido imutavel: sp[v]..sp[v+1] indexa s[] (sucessores), pp/p os predecessores; pe[k] = indice em s[] da aresta que originou o predecessor k
 * (assim uma aresta e identificada por um inteiro e os argumentos de um phi seguem a ordem dos predecessores). Arestas repetidas sao fundidas. */
typedef struct { int n, ne; int *sp, *s, *pp, *p, *pe; } CgxDg;
static void cgx_dg_build(CgxDg *g, int n, const int *from, const int *to, int ne) {
    memset(g, 0, sizeof *g); g->n = n;
    int *cnt = (int *)cgx_xcalloc((size_t)n + 1, sizeof(int)); g->sp = (int *)cgx_xcalloc((size_t)n + 2, sizeof(int));
    char *keep = (char *)cgx_xcalloc((size_t)ne, 1);
    /* fusao de repetidas: por origem, uma lista encadeada dos destinos ja aceitos (a primeira ocorrencia fica) */
    int *head = (int *)cgx_xcalloc((size_t)n + 1, sizeof(int)), *nxt = (int *)cgx_xcalloc((size_t)ne + 1, sizeof(int));
    for (int e = 0; e < ne; e++) {
        int dup = 0;
        for (int k = head[from[e]] - 1; k >= 0 && !dup; k = nxt[k] - 1) if (to[k] == to[e]) dup = 1;
        keep[e] = !dup; if (!dup) { nxt[e] = head[from[e]]; head[from[e]] = e + 1; cnt[from[e]]++; }
    }
    free(head); free(nxt);
    for (int v = 0; v < n; v++) g->sp[v + 1] = g->sp[v] + cnt[v];
    g->ne = g->sp[n]; g->s = (int *)cgx_xcalloc((size_t)g->ne + 1, sizeof(int));
    memset(cnt, 0, sizeof(int) * ((size_t)n + 1));
    int *eorig = (int *)cgx_xcalloc((size_t)g->ne + 1, sizeof(int));           /* indice da aresta de entrada que virou a i-esima de s[] */
    for (int e = 0; e < ne; e++) if (keep[e]) { const int i = g->sp[from[e]] + cnt[from[e]]++; g->s[i] = to[e]; eorig[i] = e; }
    (void)eorig;
    g->pp = (int *)cgx_xcalloc((size_t)n + 2, sizeof(int));
    for (int i = 0; i < g->ne; i++) g->pp[g->s[i] + 1]++;
    for (int v = 0; v < n; v++) g->pp[v + 1] += g->pp[v];
    g->p = (int *)cgx_xcalloc((size_t)g->ne + 1, sizeof(int)); g->pe = (int *)cgx_xcalloc((size_t)g->ne + 1, sizeof(int));
    memset(cnt, 0, sizeof(int) * ((size_t)n + 1));
    for (int v = 0; v < n; v++) for (int i = g->sp[v]; i < g->sp[v + 1]; i++) { const int t = g->s[i]; const int k = g->pp[t] + cnt[t]++; g->p[k] = v; g->pe[k] = i; }
    free(cnt); free(keep); free(eorig);
}
static void cgx_dg_free(CgxDg *g) { free(g->sp); free(g->s); free(g->pp); free(g->p); free(g->pe); memset(g, 0, sizeof *g); }
/* posicao de `from` entre os predecessores de `to` (ou -1) */
static int cgx_dg_pslot(const CgxDg *g, int from, int to) { for (int k = g->pp[to]; k < g->pp[to + 1]; k++) if (g->p[k] == from) return k - g->pp[to]; return -1; }
/* indice (em s[]) da aresta from->to, ou -1 */
static int cgx_dg_edge(const CgxDg *g, int from, int to) { for (int i = g->sp[from]; i < g->sp[from + 1]; i++) if (g->s[i] == to) return i; return -1; }

/* ================= 2. Tarjan, ordem, dominadores, fronteira, lacos ================= */
/* componentes fortemente conexas (Tarjan, iterativo: sem recursao, grafos grandes nao estouram a pilha). comp[v] = numero da componente; as
 * componentes saem na ORDEM TOPOLOGICA REVERSA do grafo condensado (sumidouros primeiro): para um grafo de chamadas, quem e chamado vem antes de quem chama. */
static int cgx_scc(const CgxDg *g, int *comp) {
    const int n = g->n; int ncomp = 0, idx = 0, sp = 0, cp = 0;
    int *low = (int *)cgx_xcalloc((size_t)n, sizeof(int)), *num = (int *)cgx_xcalloc((size_t)n, sizeof(int)), *st = (int *)cgx_xcalloc((size_t)n, sizeof(int));
    int *cs = (int *)cgx_xcalloc((size_t)n, sizeof(int)), *ci = (int *)cgx_xcalloc((size_t)n, sizeof(int)); char *on = (char *)cgx_xcalloc((size_t)n, 1);
    for (int v = 0; v < n; v++) comp[v] = -1;
    for (int r = 0; r < n; r++) {
        if (num[r]) continue;
        cs[cp] = r; ci[cp] = g->sp[r]; cp++; num[r] = low[r] = ++idx; st[sp++] = r; on[r] = 1;
        while (cp) {
            const int v = cs[cp - 1];
            if (ci[cp - 1] < g->sp[v + 1]) {
                const int w = g->s[ci[cp - 1]++];
                if (!num[w]) { num[w] = low[w] = ++idx; st[sp++] = w; on[w] = 1; cs[cp] = w; ci[cp] = g->sp[w]; cp++; }
                else if (on[w] && num[w] < low[v]) low[v] = num[w];
            } else {
                if (low[v] == num[v]) { int w; do { w = st[--sp]; on[w] = 0; comp[w] = ncomp; } while (w != v); ncomp++; }
                cp--;
                if (cp) { const int u = cs[cp - 1]; if (low[v] < low[u]) low[u] = low[v]; }
            }
        }
    }
    free(low); free(num); free(st); free(cs); free(ci); free(on);
    return ncomp;
}

typedef struct {
    int n, entry, nrpo;
    int *idom;            /* dominador imediato (entry: ele mesmo; inalcancavel: -1) */
    int *rpo;             /* rpo[i] = bloco na i-esima posicao da pos-ordem reversa (so os alcancaveis) */
    int *rponum;          /* bloco -> posicao em rpo, ou -1 */
    int *cp, *c;          /* filhos na arvore de dominancia (CSR) */
    int *tin, *tout;      /* entrada e saida de uma DFS na arvore: a domina b sse tin[a] <= tin[b] e tout[b] <= tout[a] */
    int *dfp, *df;        /* fronteira de dominancia (CSR) */
} CgxDom;
static void cgx_dom_free(CgxDom *d) { free(d->idom); free(d->rpo); free(d->rponum); free(d->cp); free(d->c); free(d->tin); free(d->tout); free(d->dfp); free(d->df); memset(d, 0, sizeof *d); }
static int cgx_dom_dominates(const CgxDom *d, int a, int b) { return d->idom[a] >= 0 && d->idom[b] >= 0 && d->tin[a] <= d->tin[b] && d->tout[b] <= d->tout[a]; }
/* Cooper, Harvey, Kennedy, "A Simple, Fast Dominance Algorithm": iteracao sobre a pos-ordem reversa com `intersect` nos dominadores imediatos.
 * Custo O(n^2) no pior caso, na pratica quase linear (grafos estruturados convergem em 2 passadas). Depois: arvore de dominancia (CSR),
 * tin/tout por DFS iterativa, e a fronteira de dominancia pelo metodo dos predecessores (so blocos com 2+ predecessores). */
static void cgx_dom_build(CgxDom *d, const CgxDg *g, int entry) {
    const int n = g->n; memset(d, 0, sizeof *d); d->n = n; d->entry = entry;
    d->idom = (int *)cgx_xcalloc((size_t)n, sizeof(int)); d->rponum = (int *)cgx_xcalloc((size_t)n, sizeof(int)); d->rpo = (int *)cgx_xcalloc((size_t)n, sizeof(int));
    for (int i = 0; i < n; i++) { d->idom[i] = -1; d->rponum[i] = -1; }
    /* pos-ordem por DFS iterativa */
    int *post = (int *)cgx_xcalloc((size_t)n, sizeof(int)), *stk = (int *)cgx_xcalloc((size_t)n, sizeof(int)), *it = (int *)cgx_xcalloc((size_t)n, sizeof(int)); char *seen = (char *)cgx_xcalloc((size_t)n, 1);
    int np = 0, sp = 0;
    if (n > 0) { stk[sp] = entry; it[sp] = g->sp[entry]; sp++; seen[entry] = 1; }
    while (sp) {
        const int v = stk[sp - 1];
        if (it[sp - 1] < g->sp[v + 1]) { const int w = g->s[it[sp - 1]++]; if (!seen[w]) { seen[w] = 1; stk[sp] = w; it[sp] = g->sp[w]; sp++; } }
        else { post[np++] = v; sp--; }
    }
    d->nrpo = np;
    for (int i = 0; i < np; i++) { d->rpo[i] = post[np - 1 - i]; d->rponum[d->rpo[i]] = i; }
    /* idom: usa o numero na pos-ordem reversa (menor = mais perto da entrada) */
    if (n > 0) d->idom[entry] = entry;
    for (int changed = 1; changed;) {
        changed = 0;
        for (int i = 1; i < np; i++) {
            const int b = d->rpo[i]; int ni = -1;
            for (int k = g->pp[b]; k < g->pp[b + 1]; k++) {
                const int p = g->p[k];
                if (d->rponum[p] < 0 || d->idom[p] < 0) continue;
                if (ni < 0) { ni = p; continue; }
                int a = p, c = ni;                                              /* intersect */
                while (a != c) { while (d->rponum[a] > d->rponum[c]) a = d->idom[a]; while (d->rponum[c] > d->rponum[a]) c = d->idom[c]; }
                ni = a;
            }
            if (ni >= 0 && d->idom[b] != ni) { d->idom[b] = ni; changed = 1; }
        }
    }
    /* arvore de dominancia em CSR */
    d->cp = (int *)cgx_xcalloc((size_t)n + 2, sizeof(int)); d->c = (int *)cgx_xcalloc((size_t)n + 1, sizeof(int));
    for (int i = 1; i < np; i++) d->cp[d->idom[d->rpo[i]] + 1]++;
    for (int v = 0; v < n; v++) d->cp[v + 1] += d->cp[v];
    { int *fill = (int *)cgx_xcalloc((size_t)n + 1, sizeof(int)); for (int i = 1; i < np; i++) { const int b = d->rpo[i], pa = d->idom[b]; d->c[d->cp[pa] + fill[pa]++] = b; } free(fill); }
    d->tin = (int *)cgx_xcalloc((size_t)n, sizeof(int)); d->tout = (int *)cgx_xcalloc((size_t)n, sizeof(int));
    { int t = 0; sp = 0;
      if (np) { stk[0] = entry; it[0] = d->cp[entry]; d->tin[entry] = t++; sp = 1; }
      while (sp) { const int v = stk[sp - 1]; if (it[sp - 1] < d->cp[v + 1]) { const int w = d->c[it[sp - 1]++]; d->tin[w] = t++; stk[sp] = w; it[sp] = d->cp[w]; sp++; } else { d->tout[v] = t++; sp--; } } }
    /* fronteira: CHK, "runner" subindo pelos idoms a partir de cada predecessor ate o idom do bloco */
    CgxVi *dv = (CgxVi *)cgx_xcalloc((size_t)n, sizeof(CgxVi));
    for (int i = 0; i < np; i++) {
        const int b = d->rpo[i]; int np_ = 0;
        for (int k = g->pp[b]; k < g->pp[b + 1]; k++) if (d->rponum[g->p[k]] >= 0) np_++;
        if (np_ < 2 && b != entry) continue;                                      /* a entrada com um retorno tem uma predecessora virtual a mais */
        for (int k = g->pp[b]; k < g->pp[b + 1]; k++) {
            int r = g->p[k]; if (d->rponum[r] < 0) continue;
            if (b == entry) { for (;;) { if (!(dv[r].n && dv[r].p[dv[r].n - 1] == b)) cgx_vi_push(&dv[r], b); if (r == entry) break; r = d->idom[r]; } continue; }
            while (r != d->idom[b]) { if (!(dv[r].n && dv[r].p[dv[r].n - 1] == b)) cgx_vi_push(&dv[r], b); r = d->idom[r]; }
        }
    }
    d->dfp = (int *)cgx_xcalloc((size_t)n + 2, sizeof(int)); { int tot = 0; for (int v = 0; v < n; v++) { d->dfp[v] = tot; tot += dv[v].n; } d->dfp[n] = tot; d->df = (int *)cgx_xcalloc((size_t)tot + 1, sizeof(int)); }
    for (int v = 0; v < n; v++) { if (dv[v].n) memcpy(d->df + d->dfp[v], dv[v].p, sizeof(int) * (size_t)dv[v].n); cgx_vi_free(&dv[v]); }
    free(dv); free(post); free(stk); free(it); free(seen);
}

/* lacos naturais: aresta u->h com h dominando u e uma aresta de retorno; o corpo e o que alcanca u andando para tras sem passar por h.
 * Lacos de mesmo cabecalho se fundem. depth[b] = quantos lacos contem b. */
typedef struct { int nloop; int *head; CgxW **body; int nw; int *depth; int n; } CgxLoops;
static void cgx_loops_free(CgxLoops *L) { for (int i = 0; i < L->nloop; i++) free(L->body[i]); free(L->head); free(L->body); free(L->depth); memset(L, 0, sizeof *L); }
static void cgx_loops_build(CgxLoops *L, const CgxDg *g, const CgxDom *d) {
    memset(L, 0, sizeof *L); L->n = g->n; L->nw = cgx_nw(g->n ? g->n : 1);
    L->depth = (int *)cgx_xcalloc((size_t)g->n, sizeof(int));
    L->head = (int *)cgx_xcalloc((size_t)g->n + 1, sizeof(int)); L->body = (CgxW **)cgx_xcalloc((size_t)g->n + 1, sizeof(CgxW *));
    int *stk = (int *)cgx_xcalloc((size_t)g->n + 1, sizeof(int));
    for (int h = 0; h < g->n; h++) {
        int li = -1;
        for (int k = g->pp[h]; k < g->pp[h + 1]; k++) {
            const int u = g->p[k];
            if (d->idom[u] < 0 || d->idom[h] < 0 || !cgx_dom_dominates(d, h, u)) continue;
            if (li < 0) { li = L->nloop++; L->head[li] = h; L->body[li] = cgx_bit_new(g->n ? g->n : 1, 1); cgx_bit_set(L->body[li], h); }
            int sp = 0; if (!cgx_bit_get(L->body[li], u)) { cgx_bit_set(L->body[li], u); stk[sp++] = u; }
            while (sp) { const int x = stk[--sp]; for (int j = g->pp[x]; j < g->pp[x + 1]; j++) { const int y = g->p[j]; if (!cgx_bit_get(L->body[li], y) && d->idom[y] >= 0) { cgx_bit_set(L->body[li], y); stk[sp++] = y; } } }
        }
    }
    free(stk);
    for (int i = 0; i < L->nloop; i++) for (int b = 0; b < g->n; b++) if (cgx_bit_get(L->body[i], b)) L->depth[b]++;
}

/* ================= 3. a IR: tipos ================= */
/* operandos: x >= 0 e um valor (0 = indefinido); x < 0 e x != CGX_NIL e a VARIAVEL -(x+1) (so antes da SSA); CGX_NIL = sem operando */
#define CGX_NIL INT_MIN
#define CGX_OPV(v) (-(v) - 1)
#define CGX_ISVAR(x) ((x) < 0 && (x) != CGX_NIL)
#define CGX_VARIDX(x) (-(x) - 1)
enum { IO_NOP, IO_PARAM, IO_CONST, IO_COPY, IO_BIN, IO_UN, IO_CALL, IO_MCALL, IO_LIST, IO_MAP, IO_INDEX, IO_FIELD, IO_GLOBAL, IO_SETIDX, IO_SETFLD,
       IO_ITER, IO_HASNEXT, IO_NEXT, IO_CATCH, IO_RAWUSE, IO_RAWDEF, IO_PHI, IO_BR, IO_JMP, IO_RET, IO_THROW, IO_UNDEF, IO_N };
static const char *const CGX_IO_NAME[IO_N] = {"nop", "param", "const", "copy", "bin", "un", "call", "mcall", "list", "map", "index", "field", "global", "setidx", "setfld",
    "iter", "hasnext", "next", "catch", "rawuse", "rawdef", "phi", "br", "jmp", "ret", "throw", "undef"};
enum { IF_IMPLICIT = 1, IF_RETHROW = 2, IF_LOOPCOND = 4, IF_UNDEFNAME = 8, IF_FORCOND = 16, IF_EACHCOND = 32, IF_STMTROOT = 64 };
typedef struct {
    int op, sub, dst, d2, a, b, c;       /* sub: operador (CGO_*) em BIN/UN, id da funcao embutida em CALL, simbolo do campo em FIELD/SETFLD */
    int *args, nargs;                     /* chamadas, listas e mapas; em PHI, um por predecessor */
    int blk, line, flags;
    CgxNode *node, *callee;               /* origem na AST; funcao ou classe chamada */
} CgxIns;
enum { VK_UNDEF, VK_TEMP, VK_VER };
typedef struct { int kind, var, ver, def, blk; } CgxVal;
enum { VAR_PARAM, VAR_SELF, VAR_LET, VAR_LOOP, VAR_EACH, VAR_CATCH, VAR_RAW, VAR_HIDDEN };
typedef struct { const char *name; int sid, kind, nver, line; CgxNode *decl; } CgxVar;
typedef struct { CgxVi ins; int line, reach, exc, flags; } CgxBB;          /* exc: alvo da aresta excepcional do fim do bloco, ou -1 */
enum { EK_NORMAL, EK_TRUE, EK_FALSE, EK_EXC };
enum { NM_OP, NM_VAL, NM_DEF, NM_BLK };
typedef struct { int node, kind, x, y, next; } CgxNm;

typedef struct CgxIr CgxIr;
typedef struct {
    CgxNode *fn, *cls; int idx; const char *name;
    CgxVar *var; int nvar, capvar; CgxVal *val; int nval, capval; CgxIns *ins; int nins, capins; CgxBB *bb; int nbb, capbb;
    int entry;
    CgxVi ef, et, ek;                     /* arestas na ordem de criacao (origem, destino, tipo) */
    CgxDg g; int *ekind;                  /* grafo congelado e o tipo de cada aresta (indexado como g.s) */
    CgxDom dom; CgxLoops loops; int has_dom;
    CgxNm *nm; int nnm, capnm; int *nmhead; int nmhead_n;
    int ssa, nundef, nphi; CgxVi undef_ins;
    int has_raw, has_try;
} CgxFnIr;
enum { CGX_FX_THROW = 1, CGX_FX_WRITE = 2, CGX_FX_READ = 4, CGX_FX_LOOP = 8 };
struct CgxIr {
    Cgx *g; CgxFnIr *fn; int nfn;
    int *fnid, nid;                       /* id do no da AST -> indice da funcao (ou -1) */
    CgxDg cg; int *cgcomp, ncomp;         /* grafo de chamadas e suas componentes (Tarjan) */
    int *fx; char *rec, *selfrec;         /* resumo de efeitos e recursividade por funcao */
    int globals;                          /* 1: constantes de modulo com literal entram na propagacao como constantes */
    int has_modraw;
};
#define CGX_BADFX (CGX_FX_THROW | CGX_FX_WRITE | CGX_FX_READ | CGX_FX_LOOP)

/* ================= 3b. efeitos das expressoes e resumos por funcao ================= */
static const CgxNode *cgx_callee_of(const CgxIr *ir, const CgxNode *n);
static int cgx_fx_blk(const CgxIr *ir, const CgxBlk *b);
static int cgx_isnumk(int k) { return k == CGT_INT || k == CGT_FLOAT; }
static int cgx_lit_nonzero(const CgxNode *n) { return (n->k == CGN_INT && n->iv != 0) || (n->k == CGN_FLT && n->dv != 0.0); }
/* efeito de uma chamada de funcao embutida; os tipos dos argumentos vem da AST (n->ty): `any` ou desconhecido = pode lancar */
static int cgx_fx_builtin(const CgxNode *c) {
    const CgxBlk *a = &c->b1; const int bi = c->bi;
    #define AK(i) ((i) < a->n ? a->p[i]->ty.k : CGT_ANY)
    switch (bi) {
        case CGB_LEN: return CGX_FX_READ | (AK(0) == CGT_LIST || AK(0) == CGT_STR || AK(0) == CGT_MAP ? 0 : CGX_FX_THROW);
        case CGB_PUSH: return CGX_FX_WRITE | CGX_FX_READ;
        case CGB_POP: return CGX_FX_WRITE | CGX_FX_READ | CGX_FX_THROW;
        case CGB_STR: return 0;
        case CGB_INT: return AK(0) == CGT_INT || AK(0) == CGT_BOOL ? 0 : CGX_FX_THROW;
        case CGB_FLOAT: return a->n && a->p[0]->k == CGN_INT ? 0 : (AK(0) == CGT_FLOAT ? 0 : CGX_FX_THROW);
        case CGB_UPPER: case CGB_LOWER: case CGB_TRIM: case CGB_WORDS: return AK(0) == CGT_STR ? 0 : CGX_FX_THROW;
        case CGB_SPLIT: return AK(0) == CGT_STR && a->n > 1 && a->p[1]->k == CGN_STR && a->p[1]->s[0] ? 0 : CGX_FX_THROW;
        case CGB_JOIN: return a->n && a->p[0]->ty.k == CGT_LIST && a->p[0]->ty.e == CGT_STR && AK(1) == CGT_STR ? CGX_FX_READ : CGX_FX_THROW | CGX_FX_READ;
        case CGB_CONTAINS: return CGX_FX_READ | (AK(0) == CGT_STR || AK(0) == CGT_LIST || AK(0) == CGT_MAP ? 0 : CGX_FX_THROW);
        case CGB_INDEXOF: case CGB_STARTS: case CGB_ENDS: return AK(0) == CGT_STR && AK(1) == CGT_STR ? 0 : CGX_FX_THROW;
        case CGB_REPLACE: return AK(0) == CGT_STR && AK(1) == CGT_STR && AK(2) == CGT_STR ? 0 : CGX_FX_THROW;
        case CGB_SLICE: return CGX_FX_READ | ((AK(0) == CGT_STR || AK(0) == CGT_LIST) && AK(1) == CGT_INT && (a->n < 3 || AK(2) == CGT_INT) ? 0 : CGX_FX_THROW);
        case CGB_KEYS: return CGX_FX_READ | (AK(0) == CGT_MAP ? 0 : CGX_FX_THROW);
        case CGB_PRINT: return CGX_FX_WRITE;
        case CGB_ABS: return cgx_isnumk(AK(0)) ? 0 : CGX_FX_THROW;
        case CGB_MIN: case CGB_MAX: return cgx_isnumk(AK(0)) && cgx_isnumk(AK(1)) ? 0 : CGX_FX_THROW;
        case CGB_SQRT: case CGB_FLOOR: case CGB_POW: case CGB_MATCHES: return CGX_FX_THROW;
        case CGB_SUM: return CGX_FX_READ | (a->n && a->p[0]->ty.k == CGT_LIST && cgx_isnumk(a->p[0]->ty.e) ? 0 : CGX_FX_THROW);
        case CGB_SORT: return CGX_FX_READ | (a->n && a->p[0]->ty.k == CGT_LIST && (cgx_isnumk(a->p[0]->ty.e) || a->p[0]->ty.e == CGT_STR) ? 0 : CGX_FX_THROW);
        case CGB_REVERSE: return CGX_FX_READ | (AK(0) == CGT_LIST || AK(0) == CGT_STR ? 0 : CGX_FX_THROW);
        case CGB_ERRMSG: return 0;
    }
    #undef AK
    return CGX_BADFX;
}
/* efeito do operador (sem os operandos) a partir dos tipos escritos pelo validador */
static int cgx_fx_op(int op, const CgxNode *l, const CgxNode *r) {
    const int ak = l ? l->ty.k : CGT_ANY, bk = r ? r->ty.k : CGT_ANY;
    switch (op) {
        case CGO_ADD: return (ak == CGT_STR && bk == CGT_STR) || (ak == bk && cgx_isnumk(ak)) ? 0 : CGX_FX_THROW;   /* int com float mistura: um inteiro enorme estoura na conversao */
        case CGO_SUB: case CGO_MUL: return ak == bk && cgx_isnumk(ak) ? 0 : CGX_FX_THROW;
        case CGO_DIV: return ak == CGT_FLOAT && bk == CGT_FLOAT && r && cgx_lit_nonzero(r) ? 0 : CGX_FX_THROW;
        case CGO_IDIV: case CGO_MOD: return ak == CGT_INT && bk == CGT_INT && r && r->k == CGN_INT && r->iv != 0 ? 0 : CGX_FX_THROW;
        case CGO_EQ: case CGO_NE: return 0;
        case CGO_LT: case CGO_LE: case CGO_GT: case CGO_GE: return (cgx_isnumk(ak) && cgx_isnumk(bk)) || (ak == CGT_STR && bk == CGT_STR) ? 0 : CGX_FX_THROW;
        case CGO_AND: case CGO_OR: case CGO_NOT: return 0;
        case CGO_NEG: return cgx_isnumk(ak) ? 0 : CGX_FX_THROW;
        case CGO_IN: return CGX_FX_READ | ((bk == CGT_LIST || (bk == CGT_STR && ak == CGT_STR) || (bk == CGT_MAP && ak == CGT_STR)) ? 0 : CGX_FX_THROW);
    }
    return CGX_BADFX;
}
static int cgx_fx_expr(const CgxIr *ir, const CgxNode *n) {
    if (!n) return 0;
    int m = 0;
    switch (n->k) {
        case CGN_INT: case CGN_FLT: case CGN_STR: case CGN_BOOL: case CGN_NULL: case CGN_VAR: return 0;
        case CGN_BIN: m = cgx_fx_expr(ir, n->a) | cgx_fx_expr(ir, n->b) | cgx_fx_op(n->op, n->a, n->b); break;
        case CGN_UN: m = cgx_fx_expr(ir, n->a) | cgx_fx_op(n->op, n->a, NULL); break;
        case CGN_TERN: m = cgx_fx_expr(ir, n->a) | cgx_fx_expr(ir, n->b) | cgx_fx_expr(ir, n->c); break;
        case CGN_LIST: case CGN_MAP: for (int i = 0; i < n->b1.n; i++) m |= cgx_fx_expr(ir, n->b1.p[i]); break;
        case CGN_INDEX: m = cgx_fx_expr(ir, n->a) | cgx_fx_expr(ir, n->b) | CGX_FX_READ | CGX_FX_THROW; break;
        case CGN_FIELD: m = cgx_fx_expr(ir, n->a) | CGX_FX_READ | (n->a->ty.k == CGT_CLASS ? 0 : CGX_FX_THROW); break;
        case CGN_CALL: case CGN_MCALL: {
            for (int i = 0; i < n->b1.n; i++) m |= cgx_fx_expr(ir, n->b1.p[i]);
            if (n->k == CGN_MCALL) m |= cgx_fx_expr(ir, n->a);
            int bi = n->k == CGN_CALL ? n->bi : 0;
            if (n->k == CGN_CALL && !bi && !cgx_callee_of(ir, n)) { const CgxOp *bo = cgx_op_find(n->s); if (bo) bi = bo->id; }
            if (bi) { CgxNode tmp = *n; tmp.bi = bi; m |= cgx_fx_builtin(&tmp); break; }
            const CgxNode *c = cgx_callee_of(ir, n);
            if (!c) { m |= CGX_BADFX; break; }
            if (c->k == CGN_CLASS) {                                           /* construtor: o `init` (se houver) mais a criacao */
                const CgxNode *init = NULL; for (int i = 0; i < c->b1.n; i++) if (c->b1.p[i]->k == CGN_FN && !strcmp(c->b1.p[i]->s, "init")) init = c->b1.p[i];
                if (init && init->id > 0 && init->id < ir->nid && ir->fnid[init->id] >= 0) m |= ir->fx[ir->fnid[init->id]]; else if (init) m |= CGX_BADFX;
                break;
            }
            if (c->id > 0 && c->id < ir->nid && ir->fnid[c->id] >= 0) m |= ir->fx[ir->fnid[c->id]]; else m |= CGX_BADFX;
            break;
        }
        default: m = CGX_BADFX;
    }
    return m;
}
static int cgx_fx_stmt(const CgxIr *ir, const CgxNode *s) {
    int m = 0;
    switch (s->k) {
        case CGN_COMMENT: case CGN_DOC: case CGN_BREAK: case CGN_CONT: return 0;
        case CGN_RAW: return CGX_BADFX;
        case CGN_LET: return cgx_fx_expr(ir, s->a);
        case CGN_ASSIGN:
            m = cgx_fx_expr(ir, s->b);
            if (s->a->k == CGN_INDEX) m |= cgx_fx_expr(ir, s->a->a) | cgx_fx_expr(ir, s->a->b) | CGX_FX_WRITE | CGX_FX_THROW;
            else if (s->a->k == CGN_FIELD) m |= cgx_fx_expr(ir, s->a->a) | CGX_FX_WRITE | (s->a->a->ty.k == CGT_CLASS ? 0 : CGX_FX_THROW);
            if (s->op >= 0) { m |= cgx_fx_op(s->op, s->a, s->b); if (s->a->k != CGN_VAR) m |= CGX_FX_READ; }
            return m;
        case CGN_RET: return cgx_fx_expr(ir, s->a);
        case CGN_THROW: return cgx_fx_expr(ir, s->a) | CGX_FX_THROW;
        case CGN_EXPR: return cgx_fx_expr(ir, s->a);
        case CGN_IF: return cgx_fx_expr(ir, s->a) | cgx_fx_blk(ir, &s->b1) | cgx_fx_blk(ir, &s->b2);
        case CGN_WHILE: return cgx_fx_expr(ir, s->a) | cgx_fx_blk(ir, &s->b1) | CGX_FX_LOOP;
        case CGN_FOR: return cgx_fx_expr(ir, s->a) | cgx_fx_expr(ir, s->b) | cgx_fx_expr(ir, s->c) | cgx_fx_blk(ir, &s->b1);
        case CGN_EACH: return cgx_fx_expr(ir, s->a) | cgx_fx_blk(ir, &s->b1) | CGX_FX_READ | (s->a->ty.k == CGT_LIST || s->a->ty.k == CGT_STR || s->a->ty.k == CGT_MAP ? 0 : CGX_FX_THROW);
        case CGN_TRY: {
            int b = cgx_fx_blk(ir, &s->b1); if (s->flag) b &= ~CGX_FX_THROW;      /* o catch pega qualquer erro lancado no corpo */
            return b | cgx_fx_blk(ir, &s->b2) | cgx_fx_blk(ir, &s->b3);
        }
    }
    return CGX_BADFX;
}
static int cgx_fx_blk(const CgxIr *ir, const CgxBlk *b) { int m = 0; for (int i = 0; i < b->n; i++) m |= cgx_fx_stmt(ir, b->p[i]); return m; }
/* a chamada pode ser removida (valor nao usado) sem mudar o comportamento? nao escreve, nao lanca, termina */
static int cgx_fx_removable(int fx) { return (fx & (CGX_FX_THROW | CGX_FX_WRITE | CGX_FX_LOOP)) == 0; }

/* ================= 3c. funcoes, grafo de chamadas ================= */
static void cgx_ir_collect(CgxIr *ir) {
    Cgx *g = ir->g; int cap = 0; ir->fn = NULL; ir->nfn = 0;
    CgxNode *pr = g->prog;
    for (int i = 0; pr && i < pr->b1.n; i++) {
        CgxNode *d = pr->b1.p[i];
        if (d->k == CGN_FN) { ir->fn = (CgxFnIr *)cgx_xgrow(ir->fn, &cap, ir->nfn + 1, sizeof(CgxFnIr)); ir->fn[ir->nfn].fn = d; ir->fn[ir->nfn].idx = ir->nfn; ir->fn[ir->nfn].name = d->s; ir->nfn++; }
        else if (d->k == CGN_CLASS) { for (int j = 0; j < d->b1.n; j++) if (d->b1.p[j]->k == CGN_FN) {
            ir->fn = (CgxFnIr *)cgx_xgrow(ir->fn, &cap, ir->nfn + 1, sizeof(CgxFnIr)); ir->fn[ir->nfn].fn = d->b1.p[j]; ir->fn[ir->nfn].cls = d; ir->fn[ir->nfn].idx = ir->nfn; ir->fn[ir->nfn].name = d->b1.p[j]->s; ir->nfn++;
        } }
        else if (d->k == CGN_RAW) ir->has_modraw = 1;
    }
    ir->nid = g->nnodes + 2; ir->fnid = (int *)cgx_xcalloc((size_t)ir->nid, sizeof(int));
    for (int i = 0; i < ir->nid; i++) ir->fnid[i] = -1;
    for (int i = 0; i < ir->nfn; i++) if (ir->fn[i].fn->id > 0 && ir->fn[i].fn->id < ir->nid) ir->fnid[ir->fn[i].fn->id] = i;
}
static CgxNode *cgx_find_top(const Cgx *g, const char *name, int cls) {
    if (!g->prog || !name) return NULL;
    for (int i = 0; i < g->prog->b1.n; i++) { CgxNode *d = g->prog->b1.p[i]; if ((d->k == (cls ? CGN_CLASS : CGN_FN)) && !strcmp(d->s, name)) return d; }
    return NULL;
}
/* funcao ou classe chamada: pelo ref que o validador gravou, ou (AST sem validar) pelo nome */
static const CgxNode *cgx_callee_of(const CgxIr *ir, const CgxNode *n) {
    if (n->k == CGN_CALL) {
        if (n->bi) return NULL;
        if (n->ref && (n->ref->k == CGN_FN || n->ref->k == CGN_CLASS)) return n->ref;
        const CgxNode *f = cgx_find_top(ir->g, n->s, 0); if (f) return f;
        return cgx_find_top(ir->g, n->s, 1);
    }
    if (n->k == CGN_MCALL && n->ref && n->ref->k == CGN_FN) return n->ref;
    return NULL;
}
static void cgx_cg_collect_expr(CgxIr *ir, int from, const CgxNode *n, CgxVi *ef, CgxVi *et) {
    if (!n) return;
    cgx_cg_collect_expr(ir, from, n->a, ef, et); cgx_cg_collect_expr(ir, from, n->b, ef, et); cgx_cg_collect_expr(ir, from, n->c, ef, et);
    for (int i = 0; i < n->b1.n; i++) cgx_cg_collect_expr(ir, from, n->b1.p[i], ef, et);
    if (n->k == CGN_CALL || n->k == CGN_MCALL) {
        const CgxNode *c = cgx_callee_of(ir, n);
        if (n->k == CGN_CALL && c && c->k == CGN_CLASS) { for (int i = 0; i < c->b1.n; i++) if (c->b1.p[i]->k == CGN_FN && !strcmp(c->b1.p[i]->s, "init")) c = c->b1.p[i]; if (c->k == CGN_CLASS) c = NULL; }
        if (c && c->k == CGN_FN && c->id > 0 && c->id < ir->nid && ir->fnid[c->id] >= 0) { cgx_vi_push(ef, from); cgx_vi_push(et, ir->fnid[c->id]); }
        else if (n->k == CGN_MCALL && !c) {                                    /* receptor sem tipo conhecido: todo metodo de mesmo nome (conservador) */
            for (int j = 0; j < ir->nfn; j++) if (ir->fn[j].cls && !strcmp(ir->fn[j].fn->s, n->s)) { cgx_vi_push(ef, from); cgx_vi_push(et, j); }
        }
    }
}
static void cgx_cg_collect_blk(CgxIr *ir, int from, const CgxBlk *b, CgxVi *ef, CgxVi *et) {
    for (int i = 0; i < b->n; i++) {
        const CgxNode *s = b->p[i];
        cgx_cg_collect_expr(ir, from, s->a, ef, et); cgx_cg_collect_expr(ir, from, s->b, ef, et); cgx_cg_collect_expr(ir, from, s->c, ef, et);
        cgx_cg_collect_blk(ir, from, &s->b1, ef, et); cgx_cg_collect_blk(ir, from, &s->b2, ef, et); cgx_cg_collect_blk(ir, from, &s->b3, ef, et);
    }
}
/* grafo de chamadas (nos = funcoes e metodos, na ordem do fonte), componentes de Tarjan e resumos de efeitos por ponto fixo dentro de cada
 * componente, de quem e chamado para quem chama */
static void cgx_cg_build(CgxIr *ir) {
    CgxVi ef = {0}, et = {0};
    for (int i = 0; i < ir->nfn; i++) cgx_cg_collect_blk(ir, i, &ir->fn[i].fn->b1, &ef, &et);
    cgx_dg_build(&ir->cg, ir->nfn, ef.p ? ef.p : (int[]){0}, et.p ? et.p : (int[]){0}, ef.n);
    cgx_vi_free(&ef); cgx_vi_free(&et);
    ir->cgcomp = (int *)cgx_xcalloc((size_t)ir->nfn + 1, sizeof(int));
    ir->ncomp = cgx_scc(&ir->cg, ir->cgcomp);
    ir->fx = (int *)cgx_xcalloc((size_t)ir->nfn + 1, sizeof(int)); ir->rec = (char *)cgx_xcalloc((size_t)ir->nfn + 1, 1); ir->selfrec = (char *)cgx_xcalloc((size_t)ir->nfn + 1, 1);
    int *csz = (int *)cgx_xcalloc((size_t)ir->ncomp + 1, sizeof(int));
    for (int i = 0; i < ir->nfn; i++) csz[ir->cgcomp[i]]++;
    for (int i = 0; i < ir->nfn; i++) {
        for (int k = ir->cg.sp[i]; k < ir->cg.sp[i + 1]; k++) if (ir->cg.s[k] == i) ir->selfrec[i] = 1;
        ir->rec[i] = ir->selfrec[i] || csz[ir->cgcomp[i]] > 1;
        if (ir->rec[i]) ir->fx[i] = CGX_FX_LOOP;                                /* recursao pode nao terminar */
    }
    /* componentes saem callees-first: itera por numero de componente crescente */
    for (int c = 0; c < ir->ncomp; c++) {
        for (int again = 1; again;) {
            again = 0;
            for (int i = 0; i < ir->nfn; i++) if (ir->cgcomp[i] == c) {
                const int m = ir->fx[i] | cgx_fx_blk(ir, &ir->fn[i].fn->b1);
                if (m != ir->fx[i]) { ir->fx[i] = m; again = 1; }
            }
        }
    }
    free(csz);
}

/* ================= 4. construcao do CFG a partir da AST ================= */
typedef struct { int kind, handler, land; CgxNode *fin; } LwCtx;            /* kind 0: regiao de um `catch`; 1: `finally` pendente */
typedef struct { int head, exit, ctxdepth; } LwLoop;
typedef struct {
    CgxIr *ir; CgxFnIr *f; int cur;
    LwCtx *cx; int ncx, capcx; LwLoop *lp; int nlp, caplp; int *scs, *scv; int nsc, capsc, capsv; int nhid;
} Lw;
static int lw_bb(Lw *L) { CgxFnIr *f = L->f; f->bb = (CgxBB *)cgx_xgrow(f->bb, &f->capbb, f->nbb + 1, sizeof(CgxBB)); f->bb[f->nbb].exc = -1; return f->nbb++; }
static int lw_val(Lw *L, int kind) { CgxFnIr *f = L->f; f->val = (CgxVal *)cgx_xgrow(f->val, &f->capval, f->nval + 1, sizeof(CgxVal)); f->val[f->nval].kind = kind; f->val[f->nval].var = -1; f->val[f->nval].def = -1; f->val[f->nval].blk = -1; return f->nval++; }
static int lw_var(Lw *L, const char *name, int kind, CgxNode *decl, int line) {
    CgxFnIr *f = L->f; f->var = (CgxVar *)cgx_xgrow(f->var, &f->capvar, f->nvar + 1, sizeof(CgxVar));
    CgxVar *v = &f->var[f->nvar]; v->name = name; v->kind = kind; v->decl = decl; v->line = line; v->sid = kind == VAR_HIDDEN ? 0 : cgx_intern(L->ir->g, name);
    if (kind != VAR_HIDDEN) { L->scs = (int *)cgx_xgrow(L->scs, &L->capsc, L->nsc + 1, sizeof(int)); L->scv = (int *)cgx_xgrow(L->scv, &L->capsv, L->nsc + 1, sizeof(int)); L->scs[L->nsc] = v->sid; L->scv[L->nsc] = f->nvar; L->nsc++; }
    return f->nvar++;
}
static int lw_hidden(Lw *L) { char *nm = (char *)cgx_alloc(L->ir->g, 16); snprintf(nm, 16, "$t%d", ++L->nhid); return lw_var(L, nm, VAR_HIDDEN, NULL, 0); }
static int lw_lookup(Lw *L, int sid) { for (int i = L->nsc - 1; i >= 0; i--) if (L->scs[i] == sid) return L->scv[i]; return -1; }
static void lw_nm(Lw *L, int node, int kind, int x, int y) {
    CgxFnIr *f = L->f; if (node <= 0) return;
    if (node >= f->nmhead_n) { const int nn = node + 256 + (f->nmhead_n >> 1); f->nmhead = (int *)realloc(f->nmhead, sizeof(int) * (size_t)nn); for (int i = f->nmhead_n; i < nn; i++) f->nmhead[i] = -1; f->nmhead_n = nn; }
    f->nm = (CgxNm *)cgx_xgrow(f->nm, &f->capnm, f->nnm + 1, sizeof(CgxNm));
    CgxNm *e = &f->nm[f->nnm]; e->node = node; e->kind = kind; e->x = x; e->y = y; e->next = f->nmhead[node]; f->nmhead[node] = f->nnm++;
}
static int lw_cur(Lw *L) { if (L->cur < 0) L->cur = lw_bb(L); return L->cur; }          /* apos um terminador, o que vier e inalcancavel: bloco novo sem predecessores */
/* acrescenta uma instrucao ao bloco corrente; dst: CGX_NIL, um valor (cria o par) ou uma variavel */
static int lw_ins(Lw *L, int op, int sub, int dst, int a, int b, int c, CgxNode *node) {
    CgxFnIr *f = L->f; const int blk = lw_cur(L);
    f->ins = (CgxIns *)cgx_xgrow(f->ins, &f->capins, f->nins + 1, sizeof(CgxIns));
    CgxIns *x = &f->ins[f->nins]; x->op = op; x->sub = sub; x->dst = dst; x->d2 = CGX_NIL; x->a = a; x->b = b; x->c = c; x->blk = blk; x->node = node; x->line = node ? node->line : 0;
    if (dst >= 0) { f->val[dst].def = f->nins; f->val[dst].blk = blk; }
    cgx_vi_push(&f->bb[blk].ins, f->nins);
    return f->nins++;
}
static int lw_tmp(Lw *L) { return lw_val(L, VK_TEMP); }
static void lw_edge(Lw *L, int from, int to, int kind) { cgx_vi_push(&L->f->ef, from); cgx_vi_push(&L->f->et, to); cgx_vi_push(&L->f->ek, kind); }
static void lw_jmp(Lw *L, int to) { if (L->cur < 0) return; const int b = L->cur; lw_ins(L, IO_JMP, 0, CGX_NIL, CGX_NIL, CGX_NIL, CGX_NIL, NULL); lw_edge(L, b, to, EK_NORMAL); L->cur = -1; }
static void lw_ctx_push(Lw *L, int kind, int handler, CgxNode *fin) { L->cx = (LwCtx *)cgx_xgrow(L->cx, &L->capcx, L->ncx + 1, sizeof(LwCtx)); L->cx[L->ncx].kind = kind; L->cx[L->ncx].handler = handler; L->cx[L->ncx].fin = fin; L->cx[L->ncx].land = -1; L->ncx++; }
/* alvo de uma excecao lancada agora: o `catch` da regiao mais interna, ou o bloco de pouso do `finally` pendente; -1 se sai da funcao */
static int lw_exc_target(Lw *L) {
    for (int i = L->ncx - 1; i >= 0; i--) {
        if (L->cx[i].kind == 0) return L->cx[i].handler;
        if (L->cx[i].land < 0) L->cx[i].land = lw_bb(L);
        return L->cx[i].land;
    }
    return -1;
}
/* a instrucao `ix` pode lancar: dentro de um try o bloco termina aqui, com a aresta excepcional para o tratador */
static void lw_may_throw(Lw *L, int ix) {
    CgxFnIr *f = L->f; const CgxIns *x = &f->ins[ix]; int m = 0;
    switch (x->op) {
        case IO_BIN: case IO_UN: { CgxNode *n = x->node; if (!n) m = CGX_FX_THROW; else if (n->k == CGN_BIN) m = cgx_fx_op(n->op, n->a, n->b); else if (n->k == CGN_UN) m = cgx_fx_op(n->op, n->a, NULL); else if (n->k == CGN_ASSIGN) m = cgx_fx_op(x->sub, n->a, n->b); else m = 0; break; }
        case IO_CALL: case IO_MCALL: m = x->node ? cgx_fx_expr(L->ir, x->node) : CGX_BADFX; break;   /* inclui os argumentos: aresta a mais e sempre segura */
        case IO_INDEX: case IO_SETIDX: case IO_RAWUSE: case IO_ITER: m = CGX_FX_THROW; break;
        case IO_SETFLD: m = x->node && x->node->k == CGN_ASSIGN && x->node->a->a && x->node->a->a->ty.k == CGT_CLASS ? 0 : CGX_FX_THROW; break;
        default: break;
    }
    if (!(m & CGX_FX_THROW)) return;
    const int tgt = lw_exc_target(L); if (tgt < 0) return;
    const int b = L->cur, nb = lw_bb(L);
    lw_ins(L, IO_JMP, 0, CGX_NIL, CGX_NIL, CGX_NIL, CGX_NIL, NULL);
    lw_edge(L, b, nb, EK_NORMAL); lw_edge(L, b, tgt, EK_EXC); L->f->bb[b].exc = tgt;
    L->cur = nb;
}
static CgxNode *lw_lit(Lw *L, int k, long long iv) { CgxNode *n = cgx_node(L->ir->g, k, 0); n->iv = iv; n->ty = cgx_t(k == CGN_BOOL ? CGT_BOOL : CGT_INT); return n; }

static int lw_expr(Lw *L, CgxNode *e);
/* operando de uma instrucao: calcula o filho e devolve o operando; o chamador registra (instrucao, posicao) com lw_rec depois de emitir */
static void lw_rec(Lw *L, int ix, int slot, CgxNode *n) { lw_nm(L, n->id, NM_OP, ix, slot); }
static int lw_args(Lw *L, const CgxBlk *b, int **out) {
    int *v = (int *)cgx_alloc(L->ir->g, sizeof(int) * (size_t)(b->n ? b->n : 1));
    for (int i = 0; i < b->n; i++) v[i] = lw_expr(L, b->p[i]);
    *out = v; return b->n;
}
static void lw_rec_args(Lw *L, int ix, const CgxBlk *b) { for (int i = 0; i < b->n; i++) lw_rec(L, ix, 3 + i, b->p[i]); }
static int lw_const(Lw *L, CgxNode *e) { const int t = lw_tmp(L); const int ix = lw_ins(L, IO_CONST, 0, t, CGX_NIL, CGX_NIL, CGX_NIL, e); (void)ix; lw_nm(L, e->id, NM_VAL, t, 0); return t; }
static int lw_expr(Lw *L, CgxNode *e) {
    CgxFnIr *f = L->f; (void)f;
    switch (e->k) {
        case CGN_INT: case CGN_FLT: case CGN_STR: case CGN_BOOL: case CGN_NULL: return lw_const(L, e);
        case CGN_VAR: {
            const int v = lw_lookup(L, e->sid ? e->sid : cgx_intern(L->ir->g, e->s));
            if (v >= 0) return CGX_OPV(v);
            const CgxNode *gc = NULL;
            if (L->ir->g->prog) for (int i = 0; i < L->ir->g->prog->b1.n; i++) { const CgxNode *d = L->ir->g->prog->b1.p[i]; if (d->k == CGN_LET && d->s && !strcmp(d->s, e->s)) gc = d; }
            const int t = lw_tmp(L);
            if (gc || L->ir->has_modraw) { const int ix = lw_ins(L, IO_GLOBAL, 0, t, CGX_NIL, CGX_NIL, CGX_NIL, e); L->f->ins[ix].callee = (CgxNode *)gc; }
            else { const int ix = lw_ins(L, IO_UNDEF, 0, t, CGX_NIL, CGX_NIL, CGX_NIL, e); L->f->ins[ix].flags |= IF_UNDEFNAME; }
            lw_nm(L, e->id, NM_VAL, t, 0); return t;
        }
        case CGN_BIN:
            if (e->op == CGO_AND || e->op == CGO_OR) {
                const int h = lw_hidden(L), a = lw_expr(L, e->a);
                const int c1 = lw_ins(L, IO_COPY, 0, CGX_OPV(h), a, CGX_NIL, CGX_NIL, e->a); lw_rec(L, c1, 0, e->a);
                const int rhs = lw_bb(L), join = lw_bb(L), b0 = L->cur;
                const int br = lw_ins(L, IO_BR, 0, CGX_NIL, a, CGX_NIL, CGX_NIL, e); lw_rec(L, br, 0, e->a);
                if (e->op == CGO_AND) { lw_edge(L, b0, rhs, EK_TRUE); lw_edge(L, b0, join, EK_FALSE); } else { lw_edge(L, b0, join, EK_TRUE); lw_edge(L, b0, rhs, EK_FALSE); }
                L->cur = -1; L->cur = rhs;
                const int b = lw_expr(L, e->b); const int c2 = lw_ins(L, IO_COPY, 0, CGX_OPV(h), b, CGX_NIL, CGX_NIL, e->b); lw_rec(L, c2, 0, e->b);
                lw_jmp(L, join); L->cur = join;
                return CGX_OPV(h);
            } else {
                const int a = lw_expr(L, e->a), b = lw_expr(L, e->b), t = lw_tmp(L);
                const int ix = lw_ins(L, IO_BIN, e->op, t, a, b, CGX_NIL, e); lw_rec(L, ix, 0, e->a); lw_rec(L, ix, 1, e->b); lw_nm(L, e->id, NM_VAL, t, 0);
                lw_may_throw(L, ix); return t;
            }
        case CGN_UN: {
            const int a = lw_expr(L, e->a), t = lw_tmp(L);
            const int ix = lw_ins(L, IO_UN, e->op, t, a, CGX_NIL, CGX_NIL, e); lw_rec(L, ix, 0, e->a); lw_nm(L, e->id, NM_VAL, t, 0); lw_may_throw(L, ix); return t;
        }
        case CGN_TERN: {
            const int h = lw_hidden(L), c = lw_expr(L, e->a);
            const int th = lw_bb(L), el = lw_bb(L), join = lw_bb(L), b0 = L->cur;
            const int br = lw_ins(L, IO_BR, 0, CGX_NIL, c, CGX_NIL, CGX_NIL, e); lw_rec(L, br, 0, e->a);
            lw_edge(L, b0, th, EK_TRUE); lw_edge(L, b0, el, EK_FALSE); L->cur = th;
            const int x = lw_expr(L, e->b); const int c1 = lw_ins(L, IO_COPY, 0, CGX_OPV(h), x, CGX_NIL, CGX_NIL, e->b); lw_rec(L, c1, 0, e->b); lw_jmp(L, join);
            L->cur = el;
            const int y = lw_expr(L, e->c); const int c2 = lw_ins(L, IO_COPY, 0, CGX_OPV(h), y, CGX_NIL, CGX_NIL, e->c); lw_rec(L, c2, 0, e->c); lw_jmp(L, join);
            L->cur = join; return CGX_OPV(h);
        }
        case CGN_CALL: {
            int *args; const int n = lw_args(L, &e->b1, &args), t = lw_tmp(L);
            const int ix = lw_ins(L, IO_CALL, e->bi, t, CGX_NIL, CGX_NIL, CGX_NIL, e);
            if (!e->bi) { const CgxOp *bo = cgx_op_find(e->s); if (bo && !cgx_callee_of(L->ir, e)) L->f->ins[ix].sub = bo->id; }
            L->f->ins[ix].args = args; L->f->ins[ix].nargs = n; L->f->ins[ix].callee = (CgxNode *)cgx_callee_of(L->ir, e);
            lw_rec_args(L, ix, &e->b1); lw_nm(L, e->id, NM_VAL, t, 0); lw_may_throw(L, ix); return t;
        }
        case CGN_MCALL: {
            const int r = lw_expr(L, e->a); int *args; const int n = lw_args(L, &e->b1, &args), t = lw_tmp(L);
            const int ix = lw_ins(L, IO_MCALL, 0, t, r, CGX_NIL, CGX_NIL, e);
            L->f->ins[ix].args = args; L->f->ins[ix].nargs = n; L->f->ins[ix].callee = (CgxNode *)cgx_callee_of(L->ir, e); L->f->ins[ix].sub = e->sid;
            lw_rec(L, ix, 0, e->a); lw_rec_args(L, ix, &e->b1); lw_nm(L, e->id, NM_VAL, t, 0); lw_may_throw(L, ix); return t;
        }
        case CGN_FIELD: {
            const int a = lw_expr(L, e->a), t = lw_tmp(L);
            const int ix = lw_ins(L, IO_FIELD, e->sid, t, a, CGX_NIL, CGX_NIL, e); lw_rec(L, ix, 0, e->a); lw_nm(L, e->id, NM_VAL, t, 0); return t;
        }
        case CGN_INDEX: {
            const int a = lw_expr(L, e->a), b = lw_expr(L, e->b), t = lw_tmp(L);
            const int ix = lw_ins(L, IO_INDEX, 0, t, a, b, CGX_NIL, e); lw_rec(L, ix, 0, e->a); lw_rec(L, ix, 1, e->b); lw_nm(L, e->id, NM_VAL, t, 0); lw_may_throw(L, ix); return t;
        }
        case CGN_LIST: case CGN_MAP: {
            int *args; const int n = lw_args(L, &e->b1, &args), t = lw_tmp(L);
            const int ix = lw_ins(L, e->k == CGN_LIST ? IO_LIST : IO_MAP, 0, t, CGX_NIL, CGX_NIL, CGX_NIL, e);
            L->f->ins[ix].args = args; L->f->ins[ix].nargs = n; lw_rec_args(L, ix, &e->b1); lw_nm(L, e->id, NM_VAL, t, 0); return t;
        }
    }
    const int t = lw_tmp(L); lw_ins(L, IO_UNDEF, 0, t, CGX_NIL, CGX_NIL, CGX_NIL, e); return t;
}

static void lw_blk(Lw *L, CgxBlk *b);
/* roda os `finally` pendentes (de dentro para fora) ate a profundidade `downto`, copiando o bloco em linha; um `return` dentro do finally encerra */
static void lw_unwind(Lw *L, int downto) {
    const int saved = L->ncx;
    for (int i = saved - 1; i >= downto && L->cur >= 0; i--) {
        if (L->cx[i].kind != 1) continue;
        L->ncx = i;                                                              /* o finally roda no contexto de fora do seu try */
        CgxNode *fin = L->cx[i].fin; const int ns = L->nsc;
        lw_blk(L, &fin->b3); L->nsc = ns;
    }
    L->ncx = saved;
}
static void lw_stmt(Lw *L, CgxNode *s);
static void lw_blk(Lw *L, CgxBlk *b) { const int ns = L->nsc; for (int i = 0; i < b->n; i++) lw_stmt(L, b->p[i]); L->nsc = ns; }
static void lw_stmts_scoped(Lw *L, CgxBlk *b) { lw_blk(L, b); }

static void lw_stmt(Lw *L, CgxNode *s) {
    CgxFnIr *f = L->f; (void)f;
    if (s->k == CGN_COMMENT || s->k == CGN_DOC) return;
    lw_nm(L, s->id, NM_BLK, lw_cur(L), 0);
    switch (s->k) {
        case CGN_LET: {
            const int v = lw_expr(L, s->a); const int var = lw_var(L, s->s, VAR_LET, s, s->line);
            const int ix = lw_ins(L, IO_COPY, 0, CGX_OPV(var), v, CGX_NIL, CGX_NIL, s); lw_rec(L, ix, 0, s->a); lw_nm(L, s->id, NM_DEF, ix, 0);
            break;
        }
        case CGN_ASSIGN: {
            CgxNode *t = s->a;
            if (t->k == CGN_VAR) {
                const int var = lw_lookup(L, t->sid ? t->sid : cgx_intern(L->ir->g, t->s));
                const int v = lw_expr(L, s->b);
                if (var < 0) { const int u = lw_tmp(L); const int ux = lw_ins(L, IO_UNDEF, 0, u, CGX_NIL, CGX_NIL, CGX_NIL, t); L->f->ins[ux].flags |= IF_UNDEFNAME; break; }
                int ix;
                if (s->op < 0) { ix = lw_ins(L, IO_COPY, 0, CGX_OPV(var), v, CGX_NIL, CGX_NIL, s); lw_rec(L, ix, 0, s->b); }
                else {                                            /* x op= e: o calculo (que pode lancar) num temporario, depois a copia: se lancar, x nao mudou */
                    const int tt = lw_tmp(L); const int bx = lw_ins(L, IO_BIN, s->op, tt, CGX_OPV(var), v, CGX_NIL, s); lw_rec(L, bx, 0, t); lw_rec(L, bx, 1, s->b); lw_may_throw(L, bx);
                    ix = lw_ins(L, IO_COPY, 0, CGX_OPV(var), tt, CGX_NIL, CGX_NIL, s);
                }
                lw_nm(L, s->id, NM_DEF, ix, 0);
            } else if (t->k == CGN_INDEX) {
                if (s->op < 0) {
                    const int v = lw_expr(L, s->b), a = lw_expr(L, t->a), i = lw_expr(L, t->b);
                    const int ix = lw_ins(L, IO_SETIDX, 0, CGX_NIL, a, i, v, s); lw_rec(L, ix, 0, t->a); lw_rec(L, ix, 1, t->b); lw_rec(L, ix, 2, s->b); lw_may_throw(L, ix);
                } else {
                    const int a = lw_expr(L, t->a), i = lw_expr(L, t->b), c = lw_tmp(L);
                    const int lx = lw_ins(L, IO_INDEX, 0, c, a, i, CGX_NIL, t); lw_rec(L, lx, 0, t->a); lw_rec(L, lx, 1, t->b); lw_may_throw(L, lx);
                    const int v = lw_expr(L, s->b), r = lw_tmp(L);
                    const int bx = lw_ins(L, IO_BIN, s->op, r, c, v, CGX_NIL, s); lw_rec(L, bx, 1, s->b); lw_may_throw(L, bx);
                    const int ix = lw_ins(L, IO_SETIDX, 0, CGX_NIL, a, i, r, s); lw_may_throw(L, ix);
                }
            } else {                                                              /* FIELD */
                if (s->op < 0) {
                    const int v = lw_expr(L, s->b), a = lw_expr(L, t->a);
                    const int ix = lw_ins(L, IO_SETFLD, t->sid, CGX_NIL, a, v, CGX_NIL, s); lw_rec(L, ix, 0, t->a); lw_rec(L, ix, 1, s->b); lw_may_throw(L, ix);
                } else {
                    const int a = lw_expr(L, t->a), c = lw_tmp(L);
                    const int lx = lw_ins(L, IO_FIELD, t->sid, c, a, CGX_NIL, CGX_NIL, t); lw_rec(L, lx, 0, t->a);
                    const int v = lw_expr(L, s->b), r = lw_tmp(L);
                    const int bx = lw_ins(L, IO_BIN, s->op, r, c, v, CGX_NIL, s); lw_rec(L, bx, 1, s->b); lw_may_throw(L, bx);
                    const int ix = lw_ins(L, IO_SETFLD, t->sid, CGX_NIL, a, r, CGX_NIL, s); lw_may_throw(L, ix);
                }
            }
            break;
        }
        case CGN_EXPR: { const int t = lw_expr(L, s->a); const int d = t >= 0 ? L->f->val[t].def : -1; if (d >= 0) L->f->ins[d].flags |= IF_STMTROOT; break; }       /* a chamada que e o comando inteiro */
        case CGN_RET: {
            int v = CGX_NIL;
            if (s->a) { v = lw_expr(L, s->a); if (L->ncx > 0 && v != CGX_NIL && (CGX_ISVAR(v))) { const int t = lw_tmp(L); lw_ins(L, IO_COPY, 0, t, v, CGX_NIL, CGX_NIL, s->a); v = t; } }
            lw_unwind(L, 0);
            if (L->cur < 0) break;
            const int ix = lw_ins(L, IO_RET, 0, CGX_NIL, v, CGX_NIL, CGX_NIL, s); if (s->a) lw_rec(L, ix, 0, s->a);
            L->cur = -1; break;
        }
        case CGN_THROW: {
            const int v = lw_expr(L, s->a); const int b = L->cur, tgt = lw_exc_target(L);
            const int ix = lw_ins(L, IO_THROW, 0, CGX_NIL, v, CGX_NIL, CGX_NIL, s); lw_rec(L, ix, 0, s->a);
            if (tgt >= 0) { lw_edge(L, b, tgt, EK_EXC); L->f->bb[b].exc = tgt; }
            L->cur = -1; break;
        }
        case CGN_BREAK: case CGN_CONT: {
            if (L->nlp == 0) break;
            const LwLoop lp = L->lp[L->nlp - 1];
            lw_unwind(L, lp.ctxdepth);
            if (L->cur >= 0) lw_jmp(L, s->k == CGN_BREAK ? lp.exit : lp.head);
            break;
        }
        case CGN_IF: {
            const int c = lw_expr(L, s->a), b0 = L->cur;
            const int th = lw_bb(L), el = lw_bb(L), join = lw_bb(L);
            const int br = lw_ins(L, IO_BR, 0, CGX_NIL, c, CGX_NIL, CGX_NIL, s); lw_rec(L, br, 0, s->a);
            lw_edge(L, b0, th, EK_TRUE); lw_edge(L, b0, el, EK_FALSE); L->cur = th;
            lw_stmts_scoped(L, &s->b1); lw_jmp(L, join);
            L->cur = el; lw_stmts_scoped(L, &s->b2); lw_jmp(L, join);
            L->cur = join; break;
        }
        case CGN_WHILE: {
            const int head = lw_bb(L), body = lw_bb(L), exit = lw_bb(L);
            lw_jmp(L, head); L->cur = head;
            const int c = lw_expr(L, s->a), b0 = L->cur;
            const int br = lw_ins(L, IO_BR, 0, CGX_NIL, c, CGX_NIL, CGX_NIL, s); L->f->ins[br].flags |= IF_LOOPCOND; lw_rec(L, br, 0, s->a);
            lw_edge(L, b0, body, EK_TRUE); lw_edge(L, b0, exit, EK_FALSE);
            L->lp = (LwLoop *)cgx_xgrow(L->lp, &L->caplp, L->nlp + 1, sizeof(LwLoop)); L->lp[L->nlp].head = head; L->lp[L->nlp].exit = exit; L->lp[L->nlp].ctxdepth = L->ncx; L->nlp++;
            L->cur = body; lw_stmts_scoped(L, &s->b1); lw_jmp(L, head);
            L->nlp--; L->cur = exit; break;
        }
        case CGN_FOR: {
            /* for i in a..b step k: o contador escondido $idx anda sozinho (o corpo pode mexer em i sem mudar as iteracoes, como o range do Python) */
            const int lo = lw_expr(L, s->a), idx = lw_hidden(L), hi = lw_hidden(L);
            const int c1 = lw_ins(L, IO_COPY, 0, CGX_OPV(idx), lo, CGX_NIL, CGX_NIL, s->a); lw_rec(L, c1, 0, s->a);
            const int hv = lw_expr(L, s->b); const int c2 = lw_ins(L, IO_COPY, 0, CGX_OPV(hi), hv, CGX_NIL, CGX_NIL, s->b); lw_rec(L, c2, 0, s->b);
            const long long step = s->c && s->c->k == CGN_INT ? s->c->iv : 1;
            const int head = lw_bb(L), body = lw_bb(L), latch = lw_bb(L), exit = lw_bb(L);
            lw_jmp(L, head); L->cur = head;
            const int cond = lw_tmp(L);
            lw_ins(L, IO_BIN, step < 0 ? CGO_GT : CGO_LT, cond, CGX_OPV(idx), CGX_OPV(hi), CGX_NIL, s);
            { const int hb = L->cur; const int br = lw_ins(L, IO_BR, 0, CGX_NIL, cond, CGX_NIL, CGX_NIL, s); L->f->ins[br].flags |= IF_FORCOND; lw_edge(L, hb, body, EK_TRUE); lw_edge(L, hb, exit, EK_FALSE); }
            L->cur = body; const int ns = L->nsc;
            const int var = lw_var(L, s->s, VAR_LOOP, s, s->line);
            { const int ix = lw_ins(L, IO_COPY, 0, CGX_OPV(var), CGX_OPV(idx), CGX_NIL, CGX_NIL, s); lw_nm(L, s->id, NM_DEF, ix, 0); }
            L->lp = (LwLoop *)cgx_xgrow(L->lp, &L->caplp, L->nlp + 1, sizeof(LwLoop)); L->lp[L->nlp].head = latch; L->lp[L->nlp].exit = exit; L->lp[L->nlp].ctxdepth = L->ncx; L->nlp++;
            lw_blk(L, &s->b1); lw_jmp(L, latch); L->nlp--; L->nsc = ns;
            L->cur = latch;
            { CgxNode *sl = lw_lit(L, CGN_INT, step); const int sv = lw_tmp(L); lw_ins(L, IO_CONST, 0, sv, CGX_NIL, CGX_NIL, CGX_NIL, sl);
              lw_ins(L, IO_BIN, CGO_ADD, CGX_OPV(idx), CGX_OPV(idx), sv, CGX_NIL, s); lw_jmp(L, head); }
            L->cur = exit; break;
        }
        case CGN_EACH: {
            const int coll = lw_expr(L, s->a), it = lw_hidden(L);
            const int ix0 = lw_ins(L, IO_ITER, 0, CGX_OPV(it), coll, CGX_NIL, CGX_NIL, s); lw_rec(L, ix0, 0, s->a); lw_may_throw(L, ix0);
            const int head = lw_bb(L), body = lw_bb(L), exit = lw_bb(L);
            lw_jmp(L, head); L->cur = head;
            const int hn = lw_tmp(L); lw_ins(L, IO_HASNEXT, 0, hn, CGX_OPV(it), CGX_NIL, CGX_NIL, s);
            { const int hb = L->cur; const int br = lw_ins(L, IO_BR, 0, CGX_NIL, hn, CGX_NIL, CGX_NIL, s); L->f->ins[br].flags |= IF_EACHCOND; lw_edge(L, hb, body, EK_TRUE); lw_edge(L, hb, exit, EK_FALSE); }
            L->cur = body; const int ns = L->nsc;
            const int v1 = lw_var(L, s->s, VAR_EACH, s, s->line); const int v2 = s->s2 ? lw_var(L, s->s2, VAR_EACH, s, s->line) : -1;
            { const int ix = lw_ins(L, IO_NEXT, v2 >= 0, CGX_OPV(v1), CGX_OPV(it), CGX_NIL, CGX_NIL, s); L->f->ins[ix].d2 = v2 >= 0 ? CGX_OPV(v2) : CGX_NIL; lw_nm(L, s->id, NM_DEF, ix, 0); }
            L->lp = (LwLoop *)cgx_xgrow(L->lp, &L->caplp, L->nlp + 1, sizeof(LwLoop)); L->lp[L->nlp].head = head; L->lp[L->nlp].exit = exit; L->lp[L->nlp].ctxdepth = L->ncx; L->nlp++;
            lw_blk(L, &s->b1); lw_jmp(L, head); L->nlp--; L->nsc = ns;
            L->cur = exit; break;
        }
        case CGN_TRY: {
            L->f->has_try = 1;
            const int join = lw_bb(L), mid = lw_bb(L);
            const int h = s->flag ? lw_bb(L) : -1;
            { const int tb = lw_bb(L); lw_jmp(L, tb); L->cur = tb; }               /* o corpo comeca num bloco novo: a aresta excepcional de um bloco vale para todas as suas instrucoes, entao nenhuma de fora do try pode estar nele */
            if (s->iv) lw_ctx_push(L, 1, -1, s);
            const int fin_idx = s->iv ? L->ncx - 1 : -1;
            if (s->flag) lw_ctx_push(L, 0, h, NULL);
            lw_blk(L, &s->b1);
            if (s->flag) L->ncx--;
            lw_jmp(L, mid);
            if (s->flag) {
                L->cur = h; const int ns = L->nsc;
                if (s->s2) { const int ev = lw_var(L, s->s2, VAR_CATCH, s, s->line); const int ix = lw_ins(L, IO_CATCH, 0, CGX_OPV(ev), CGX_NIL, CGX_NIL, CGX_NIL, s); (void)ix; }
                lw_blk(L, &s->b2); L->nsc = ns; lw_jmp(L, mid);
            }
            const int land = fin_idx >= 0 ? L->cx[fin_idx].land : -1;
            if (s->iv) L->ncx--;
            L->cur = mid;
            if (s->iv) { const int ns = L->nsc; lw_blk(L, &s->b3); L->nsc = ns; }
            lw_jmp(L, join);
            if (land >= 0) {                                                      /* pouso excepcional do finally: roda o finally e relanca */
                L->cur = land; const int ns = L->nsc; lw_blk(L, &s->b3); L->nsc = ns;
                if (L->cur >= 0) {
                    const int b = L->cur, tgt = lw_exc_target(L);
                    const int ix = lw_ins(L, IO_THROW, 0, CGX_NIL, CGX_NIL, CGX_NIL, CGX_NIL, s); L->f->ins[ix].flags |= IF_RETHROW;
                    if (tgt >= 0) { lw_edge(L, b, tgt, EK_EXC); L->f->bb[b].exc = tgt; }
                    L->cur = -1;
                }
            }
            L->cur = join; break;
        }
        case CGN_RAW: {
            L->f->has_raw = 1;
            int *args = (int *)cgx_alloc(L->ir->g, sizeof(int) * (size_t)(L->nsc + 1)); int na = 0;
            for (int i = 0; i < L->nsc; i++) args[na++] = CGX_OPV(L->scv[i]);
            const int ix = lw_ins(L, IO_RAWUSE, 0, CGX_NIL, CGX_NIL, CGX_NIL, CGX_NIL, s); L->f->ins[ix].args = args; L->f->ins[ix].nargs = na;
            lw_may_throw(L, ix);
            const int nvis = L->nsc;
            if (s->defs) { char *d = cgx_dup(L->ir->g, s->defs); for (char *tok = strtok(d, " "); tok; tok = strtok(NULL, " ")) { if (lw_lookup(L, cgx_intern(L->ir->g, tok)) < 0) lw_var(L, tok, VAR_RAW, s, s->line); } }
            for (int i = 0; i < L->nsc; i++) { const int k = lw_ins(L, IO_RAWDEF, 0, CGX_OPV(L->scv[i]), CGX_NIL, CGX_NIL, CGX_NIL, s); (void)k; }
            (void)nvis; break;
        }
        default: break;
    }
}

/* ================= 4b. construcao e congelamento por funcao ================= */
static void cgx_fn_freeze(CgxFnIr *f) {
    /* alcancabilidade pelas arestas brutas, a partir da entrada; arestas que saem de blocos inalcancaveis nao entram no grafo */
    const int n = f->nbb; char *seen = (char *)cgx_xcalloc((size_t)n + 1, 1); int *stk = (int *)cgx_xcalloc((size_t)n + 1, sizeof(int)); int sp = 0;
    int *head = (int *)cgx_xcalloc((size_t)n + 1, sizeof(int)), *nxt = (int *)cgx_xcalloc((size_t)f->ef.n + 1, sizeof(int));
    for (int e = 0; e < f->ef.n; e++) { nxt[e] = head[f->ef.p[e]]; head[f->ef.p[e]] = e + 1; }
    seen[f->entry] = 1; stk[sp++] = f->entry;
    while (sp) { const int v = stk[--sp]; for (int e = head[v] - 1; e >= 0; e = nxt[e] - 1) { const int w = f->et.p[e]; if (!seen[w]) { seen[w] = 1; stk[sp++] = w; } } }
    CgxVi ef = {0}, et = {0}, ek = {0};
    for (int e = 0; e < f->ef.n; e++) if (seen[f->ef.p[e]]) { cgx_vi_push(&ef, f->ef.p[e]); cgx_vi_push(&et, f->et.p[e]); cgx_vi_push(&ek, f->ek.p[e]); }
    cgx_dg_build(&f->g, n, ef.p ? ef.p : (int[]){0}, et.p ? et.p : (int[]){0}, ef.n);
    f->ekind = (int *)cgx_xcalloc((size_t)f->g.ne + 1, sizeof(int));
    for (int i = ef.n - 1; i >= 0; i--) { const int e = cgx_dg_edge(&f->g, ef.p[i], et.p[i]); if (e >= 0) f->ekind[e] = ek.p[i]; }
    for (int b = 0; b < n; b++) f->bb[b].reach = seen[b];
    cgx_vi_free(&ef); cgx_vi_free(&et); cgx_vi_free(&ek); free(seen); free(stk); free(head); free(nxt);
    cgx_dom_build(&f->dom, &f->g, f->entry); f->has_dom = 1;
    cgx_loops_build(&f->loops, &f->g, &f->dom);
}
static void cgx_fn_build(CgxIr *ir, CgxFnIr *f) {
    Lw L; memset(&L, 0, sizeof L); L.ir = ir; L.f = f;
    lw_val(&L, VK_UNDEF);                                                         /* valor 0: indefinido */
    f->entry = lw_bb(&L); L.cur = f->entry;
    if (f->cls) { const int v = lw_var(&L, "self", VAR_SELF, f->fn, f->fn->line); lw_ins(&L, IO_PARAM, -1, CGX_OPV(v), CGX_NIL, CGX_NIL, CGX_NIL, f->fn); }
    for (int i = 0; i < f->fn->npar; i++) { const int v = lw_var(&L, f->fn->par[i].name, VAR_PARAM, f->fn, f->fn->par[i].line); lw_ins(&L, IO_PARAM, i, CGX_OPV(v), CGX_NIL, CGX_NIL, CGX_NIL, f->fn); }
    lw_blk(&L, &f->fn->b1);
    if (L.cur >= 0) { const int ix = lw_ins(&L, IO_RET, 0, CGX_NIL, CGX_NIL, CGX_NIL, CGX_NIL, f->fn); f->ins[ix].flags |= IF_IMPLICIT; L.cur = -1; }
    for (int i = 0; i < f->nbb; i++) if (f->bb[i].ins.n) f->bb[i].line = f->ins[f->bb[i].ins.p[0]].line;
    free(L.cx); free(L.lp); free(L.scs); free(L.scv);
    cgx_fn_freeze(f);
}
static void cgx_fn_free_ir(CgxFnIr *f) {
    for (int i = 0; i < f->nins; i++) if (f->ins[i].op == IO_PHI) free(f->ins[i].args);          /* os args de chamadas e listas vivem na arena do Cgx; os dos phi sao proprios */
    for (int i = 0; i < f->nbb; i++) cgx_vi_free(&f->bb[i].ins);
    free(f->bb); free(f->ins); free(f->val); free(f->var); free(f->nm); free(f->nmhead); cgx_vi_free(&f->ef); cgx_vi_free(&f->et); cgx_vi_free(&f->ek); cgx_vi_free(&f->undef_ins);
    cgx_dg_free(&f->g); free(f->ekind); cgx_dom_free(&f->dom); cgx_loops_free(&f->loops);
    memset(f, 0, sizeof *f);
}

/* ================= 5. SSA (Cytron et al.), saida de SSA, verificador, dump ================= */
/* acesso uniforme as instrucoes, antes ou depois da SSA: o var de um operando (ou -1) */
static int cgx_opvar(const CgxFnIr *f, int x) { if (x == CGX_NIL) return -1; if (x < 0) return CGX_VARIDX(x); return f->val[x].var; }
static int cgx_ins_nop(const CgxIns *x) { return 3 + x->nargs; }
static int cgx_ins_op(const CgxIns *x, int i) { return i == 0 ? x->a : i == 1 ? x->b : i == 2 ? x->c : x->args[i - 3]; }
static void cgx_ins_setop(CgxIns *x, int i, int v) { if (i == 0) x->a = v; else if (i == 1) x->b = v; else if (i == 2) x->c = v; else x->args[i - 3] = v; }
static int cgx_is_term(int op) { return op == IO_BR || op == IO_JMP || op == IO_RET || op == IO_THROW; }
/* a instrucao e um uso ordinario de seus operandos (phi nao: seus argumentos sao usos no fim dos predecessores) */
static int cgx_uses_ops(const CgxIns *x) { return x->op != IO_PHI; }

static void cgx_live_in(const CgxFnIr *f, CgxW **in);                    /* ucgeflow.c: vivacidade de variaveis, para a SSA podada */
static void cgx_ssa_build(CgxFnIr *f) {
    const int nv = f->nvar, nb = f->nbb, nw = cgx_nw(nv ? nv : 1);
    const CgxDg *g = &f->g; const CgxDom *d = &f->dom;
    CgxW *live = NULL; cgx_live_in(f, &live);
    CgxW *hasphi = cgx_bit_new(nv ? nv : 1, nb);                          /* hasphi[b] : conjunto de variaveis com phi em b */
    CgxVi *defb = (CgxVi *)cgx_xcalloc((size_t)(nv ? nv : 1), sizeof(CgxVi)); CgxVi *phis = (CgxVi *)cgx_xcalloc((size_t)nb, sizeof(CgxVi));
    for (int b = 0; b < nb; b++) { if (!f->bb[b].reach) continue; for (int k = 0; k < f->bb[b].ins.n; k++) { const CgxIns *x = &f->ins[f->bb[b].ins.p[k]]; const int dv = cgx_opvar(f, x->dst), d2 = cgx_opvar(f, x->d2);
        if (dv >= 0 && !(defb[dv].n && defb[dv].p[defb[dv].n - 1] == b)) cgx_vi_push(&defb[dv], b);
        if (d2 >= 0 && !(defb[d2].n && defb[d2].p[defb[d2].n - 1] == b)) cgx_vi_push(&defb[d2], b); } }
    CgxVi work = {0}; char *inw = (char *)cgx_xcalloc((size_t)nb + 1, 1);
    for (int v = 0; v < nv; v++) {
        work.n = 0; memset(inw, 0, (size_t)nb);
        for (int i = 0; i < defb[v].n; i++) { cgx_vi_push(&work, defb[v].p[i]); inw[defb[v].p[i]] = 1; }
        while (work.n) {
            const int b = work.p[--work.n];
            for (int k = d->dfp[b]; k < d->dfp[b + 1]; k++) {
                const int y = d->df[k];
                if (cgx_bit_get(hasphi + (size_t)y * (size_t)nw, v) || !cgx_bit_get(live + (size_t)y * (size_t)nw, v)) continue;   /* podada: so onde a variavel e usada adiante */
                cgx_bit_set(hasphi + (size_t)y * (size_t)nw, v);
                f->ins = (CgxIns *)cgx_xgrow(f->ins, &f->capins, f->nins + 1, sizeof(CgxIns));
                CgxIns *x = &f->ins[f->nins]; memset(x, 0, sizeof *x); x->op = IO_PHI; x->dst = CGX_OPV(v); x->a = x->b = x->c = x->d2 = CGX_NIL; x->blk = y; x->line = f->var[v].line;
                x->nargs = g->pp[y + 1] - g->pp[y]; x->args = (int *)cgx_xcalloc((size_t)x->nargs + 1, sizeof(int)); for (int j = 0; j < x->nargs; j++) x->args[j] = CGX_OPV(v);
                cgx_vi_push(&phis[y], f->nins++); f->nphi++;
                if (!inw[y]) { inw[y] = 1; cgx_vi_push(&work, y); }
            }
        }
    }
    for (int b = 0; b < nb; b++) if (phis[b].n) {                         /* phis na frente do bloco */
        CgxVi nv_ = {0}; for (int i = 0; i < phis[b].n; i++) cgx_vi_push(&nv_, phis[b].p[i]); for (int i = 0; i < f->bb[b].ins.n; i++) cgx_vi_push(&nv_, f->bb[b].ins.p[i]);
        cgx_vi_free(&f->bb[b].ins); f->bb[b].ins = nv_;
    }
    /* renomeacao: DFS iterativa na arvore de dominancia, uma pilha de versoes por variavel */
    CgxVi *st = (CgxVi *)cgx_xcalloc((size_t)(nv ? nv : 1), sizeof(CgxVi)); CgxVi trail = {0};
    int *fb = (int *)cgx_xcalloc((size_t)nb + 1, sizeof(int)), *fi = (int *)cgx_xcalloc((size_t)nb + 1, sizeof(int)), *fm = (int *)cgx_xcalloc((size_t)nb + 1, sizeof(int)); int fsp = 0;
#define NEWVER(vr, ixd) do { f->val = (CgxVal *)cgx_xgrow(f->val, &f->capval, f->nval + 1, sizeof(CgxVal)); CgxVal *nvl = &f->val[f->nval]; nvl->kind = VK_VER; nvl->var = (vr); nvl->ver = ++f->var[vr].nver; nvl->def = (ixd); nvl->blk = f->ins[ixd].blk; cgx_vi_push(&st[vr], f->nval); cgx_vi_push(&trail, vr); f->nval++; } while (0)
    if (nb) { fb[0] = f->entry; fi[0] = d->cp[f->entry]; fm[0] = -1; fsp = 1; }
    while (fsp) {
        const int top = fsp - 1, b = fb[top];
        if (fm[top] < 0) {                                                 /* entrada no bloco */
            fm[top] = trail.n;
            for (int k = 0; k < f->bb[b].ins.n; k++) {
                const int ixd = f->bb[b].ins.p[k]; CgxIns *x = &f->ins[ixd];
                if (x->op != IO_PHI) for (int o = 0; o < cgx_ins_nop(x); o++) {
                    const int u = cgx_ins_op(x, o); if (!CGX_ISVAR(u)) continue;
                    const int v = CGX_VARIDX(u); int nvu = 0;
                    if (st[v].n) nvu = st[v].p[st[v].n - 1]; else { f->nundef++; cgx_vi_push(&f->undef_ins, ixd); }
                    cgx_ins_setop(x, o, nvu);
                }
                x = &f->ins[ixd];
                if (CGX_ISVAR(x->dst)) { const int v = CGX_VARIDX(x->dst); NEWVER(v, ixd); x = &f->ins[ixd]; x->dst = f->nval - 1; }
                if (CGX_ISVAR(x->d2)) { const int v = CGX_VARIDX(x->d2); NEWVER(v, ixd); x = &f->ins[ixd]; x->d2 = f->nval - 1; }
            }
            for (int e = g->sp[b]; e < g->sp[b + 1]; e++) {                /* argumentos dos phis dos sucessores */
                const int t = g->s[e], slot = cgx_dg_pslot(g, b, t);
                for (int k = 0; k < f->bb[t].ins.n; k++) {
                    CgxIns *x = &f->ins[f->bb[t].ins.p[k]]; if (x->op != IO_PHI) break;
                    const int v = CGX_VARIDX(x->dst >= 0 ? -(f->val[x->dst].var) - 1 : x->dst);
                    x->args[slot] = st[v].n ? st[v].p[st[v].n - 1] : 0;
                }
            }
        }
        if (fi[top] < d->cp[b + 1]) { const int c = d->c[fi[top]++]; fb[fsp] = c; fi[fsp] = d->cp[c]; fm[fsp] = -1; fsp++; }
        else { while (trail.n > fm[top]) { const int v = trail.p[--trail.n]; st[v].n--; } fsp--; }
    }
#undef NEWVER
    /* blocos inalcancaveis: sem valores vivos; cada definicao ganha uma versao, cada uso vira indefinido */
    for (int b = 0; b < nb; b++) if (!f->bb[b].reach) for (int k = 0; k < f->bb[b].ins.n; k++) {
        const int ixd = f->bb[b].ins.p[k]; CgxIns *x = &f->ins[ixd];
        for (int o = 0; o < cgx_ins_nop(x); o++) if (CGX_ISVAR(cgx_ins_op(x, o))) cgx_ins_setop(x, o, 0);
        for (int w = 0; w < 2; w++) { int *dp = w ? &f->ins[ixd].d2 : &f->ins[ixd].dst; if (CGX_ISVAR(*dp)) { const int v = CGX_VARIDX(*dp); f->val = (CgxVal *)cgx_xgrow(f->val, &f->capval, f->nval + 1, sizeof(CgxVal)); CgxVal *nvl = &f->val[f->nval]; nvl->kind = VK_VER; nvl->var = v; nvl->ver = ++f->var[v].nver; nvl->def = ixd; nvl->blk = b; f->ins[ixd].dst = w ? f->ins[ixd].dst : f->nval; if (w) f->ins[ixd].d2 = f->nval; f->nval++; } }
    }
    f->ssa = 1;
    /* os registros NM_OP guardam (instrucao, posicao): continuam validos, pois os operandos foram trocados no lugar */
    for (int v = 0; v < nv; v++) { cgx_vi_free(&defb[v]); cgx_vi_free(&st[v]); }
    for (int b = 0; b < nb; b++) cgx_vi_free(&phis[b]);
    free(defb); free(phis); free(st); cgx_vi_free(&trail); cgx_vi_free(&work); free(inw); free(fb); free(fi); free(fm); free(hasphi); free(live);
}

/* valor de um no da AST depois da SSA: o operando que o consumidor usou (NM_OP) ou o temporario (NM_VAL). Varias ocorrencias (finally copiado,
 * corpo de laco) tem varios valores: nm_next percorre. Devolve -1 quando nao ha mais. */
static int cgx_nm_first(const CgxFnIr *f, int node) { return node > 0 && node < f->nmhead_n ? f->nmhead[node] : -1; }
static int cgx_nm_next(const CgxFnIr *f, int e) { return f->nm[e].next; }
static int cgx_nm_value(const CgxFnIr *f, int e) {                          /* valor (>= 0) ou -1 se a entrada nao e de valor */
    const CgxNm *m = &f->nm[e];
    if (m->kind == NM_VAL) return m->x;
    if (m->kind == NM_OP) { const CgxIns *x = &f->ins[m->x]; if (m->y >= cgx_ins_nop(x)) return -1; const int v = cgx_ins_op(x, m->y); return v >= 0 && v != CGX_NIL ? v : -1; }
    return -1;
}

/* saida de SSA: cada phi vira copias por uma variavel escondida nova (copia no fim de cada predecessor, antes do terminador, e uma copia no lugar do
 * phi): nao cai nos problemas da copia perdida nem da troca, porque os alvos das copias paralelas nunca sao fontes. Depois cada versao volta a ser a
 * sua variavel. So vale para uma IR que nao foi transformada depois de construida (as versoes de uma variavel nao se sobrepoem). */
static void cgx_ssa_out(CgxFnIr *f) {
    if (!f->ssa) return;
    const int nb = f->nbb;
    for (int b = 0; b < nb; b++) {
        if (!f->bb[b].reach) continue;
        for (int k = 0; k < f->bb[b].ins.n; k++) {
            const int ixp = f->bb[b].ins.p[k]; if (f->ins[ixp].op != IO_PHI) break;
            const int tv = f->nvar; f->var = (CgxVar *)cgx_xgrow(f->var, &f->capvar, f->nvar + 1, sizeof(CgxVar)); f->var[tv].name = "$phi"; f->var[tv].kind = VAR_HIDDEN; f->nvar++;
            const int dst = f->ins[ixp].dst;
            for (int j = 0; j < f->ins[ixp].nargs; j++) {
                const int pb = f->g.p[f->g.pp[b] + j];
                f->ins = (CgxIns *)cgx_xgrow(f->ins, &f->capins, f->nins + 1, sizeof(CgxIns));
                CgxIns *c = &f->ins[f->nins]; memset(c, 0, sizeof *c); c->op = IO_COPY; c->dst = CGX_OPV(tv); c->a = f->ins[ixp].args[j]; c->b = c->c = c->d2 = CGX_NIL; c->blk = pb;
                CgxVi *pv = &f->bb[pb].ins; cgx_vi_push(pv, 0); memmove(pv->p + pv->n - 1, pv->p + pv->n - 2, sizeof(int)); pv->p[pv->n - 2] = f->nins; f->nins++;   /* antes do terminador */
            }
            CgxIns *p = &f->ins[ixp]; p->op = IO_COPY; p->a = CGX_OPV(tv); p->dst = dst; free(p->args); p->args = NULL; p->nargs = 0;
        }
    }
    /* versoes -> variaveis */
    for (int i = 0; i < f->nins; i++) {
        CgxIns *x = &f->ins[i];
        for (int o = 0; o < cgx_ins_nop(x); o++) { const int u = cgx_ins_op(x, o); if (u > 0 && u != CGX_NIL && f->val[u].var >= 0) cgx_ins_setop(x, o, CGX_OPV(f->val[u].var)); }
        if (x->dst >= 0 && x->dst != CGX_NIL && f->val[x->dst].var >= 0) x->dst = CGX_OPV(f->val[x->dst].var);
        if (x->d2 >= 0 && x->d2 != CGX_NIL && f->val[x->d2].var >= 0) x->d2 = CGX_OPV(f->val[x->d2].var);
    }
    f->ssa = 0; f->nphi = 0;
}

/* verificador: invariantes do CFG e da SSA. Devolve 0 e escreve a primeira violacao em msg */
static int cgx_ir_verify_fn(const CgxFnIr *f, char *msg, size_t cap) {
#define BAD(...) do { snprintf(msg, cap, __VA_ARGS__); return 1; } while (0)
    const CgxDg *g = &f->g; msg[0] = 0;
    for (int b = 0; b < f->nbb; b++) {
        if (!f->bb[b].reach) continue;
        const CgxBB *bb = &f->bb[b];
        if (!bb->ins.n) BAD("block B%d is empty", b);
        const CgxIns *last = &f->ins[bb->ins.p[bb->ins.n - 1]];
        if (!cgx_is_term(last->op)) BAD("block B%d does not end in a terminator", b);
        int seen_nonphi = 0;
        for (int k = 0; k < bb->ins.n; k++) {
            const CgxIns *x = &f->ins[bb->ins.p[k]];
            if (x->blk != b) BAD("instruction %d lists block B%d but lives in B%d", bb->ins.p[k], x->blk, b);
            if (cgx_is_term(x->op) && k != bb->ins.n - 1) BAD("terminator in the middle of B%d", b);
            if (x->op == IO_PHI) { if (seen_nonphi) BAD("phi after a non-phi in B%d", b); if (x->nargs != g->pp[b + 1] - g->pp[b]) BAD("phi in B%d has %d arguments for %d predecessors", b, x->nargs, g->pp[b + 1] - g->pp[b]); }
            else seen_nonphi = 1;
        }
        if (last->op == IO_JMP && g->sp[b + 1] - g->sp[b] < 1) BAD("jmp in B%d without successor", b);
        if (last->op == IO_BR && g->sp[b + 1] - g->sp[b] < 1) BAD("br in B%d without successor", b);
        if ((last->op == IO_RET) && g->sp[b + 1] - g->sp[b] != 0) BAD("ret in B%d has successors", b);
    }
    for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach) for (int e = g->sp[b]; e < g->sp[b + 1]; e++) if (!f->bb[g->s[e]].reach) BAD("edge B%d -> B%d reaches an unreachable block", b, g->s[e]);
    if (!f->ssa) return 0;
    int *defcnt = (int *)cgx_xcalloc((size_t)f->nval + 1, sizeof(int)); int *pos = (int *)cgx_xcalloc((size_t)f->nins + 1, sizeof(int));
    for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach) for (int k = 0; k < f->bb[b].ins.n; k++) pos[f->bb[b].ins.p[k]] = k;
    int rc = 0;
    for (int b = 0; b < f->nbb && !rc; b++) {
        if (!f->bb[b].reach) continue;
        for (int k = 0; k < f->bb[b].ins.n && !rc; k++) {
            const int ix = f->bb[b].ins.p[k]; const CgxIns *x = &f->ins[ix];
            if (x->dst != CGX_NIL && x->dst >= 0) { defcnt[x->dst]++; if (f->val[x->dst].def != ix && x->dst != 0) { snprintf(msg, cap, "value v%d: recorded def %d but defined by %d", x->dst, f->val[x->dst].def, ix); rc = 1; } }
            if (x->d2 != CGX_NIL && x->d2 >= 0) defcnt[x->d2]++;
            if (CGX_ISVAR(x->dst)) { snprintf(msg, cap, "variable destination left in SSA form (instruction %d)", ix); rc = 1; }
            for (int o = 0; o < cgx_ins_nop(x) && !rc; o++) {
                const int u = cgx_ins_op(x, o); if (u == CGX_NIL) continue;
                if (CGX_ISVAR(u)) { snprintf(msg, cap, "variable operand left in SSA form (instruction %d, B%d)", ix, b); rc = 1; break; }
                if (u == 0) continue;
                const int db = f->val[u].blk; if (db < 0) { snprintf(msg, cap, "value v%d used in B%d has no definition", u, b); rc = 1; break; }
                if (x->op == IO_PHI) { const int pb = g->p[g->pp[b] + o - 3]; if (!cgx_dom_dominates(&f->dom, db, pb)) { snprintf(msg, cap, "phi argument v%d (def B%d) does not dominate predecessor B%d of B%d", u, db, pb, b); rc = 1; } }
                else if (db == b) { if (pos[f->val[u].def] >= k) { snprintf(msg, cap, "v%d used before its definition in B%d", u, b); rc = 1; } }
                else if (!cgx_dom_dominates(&f->dom, db, b)) { snprintf(msg, cap, "use of v%d in B%d is not dominated by its definition in B%d", u, b, db); rc = 1; }
            }
        }
    }
    if (!rc) for (int v = 1; v < f->nval; v++) if (defcnt[v] > 1) { snprintf(msg, cap, "value v%d is defined %d times", v, defcnt[v]); rc = 1; break; }
    free(defcnt); free(pos);
    return rc;
#undef BAD
}

/* ---- dump estavel (mesmos blocos, mesma ordem, sem enderecos) ---- */
static void cgx_val_str(const CgxFnIr *f, int x, char *o, size_t cap) {
    if (x == CGX_NIL) { snprintf(o, cap, "_"); return; }
    if (x < 0) { snprintf(o, cap, "%s", f->var[CGX_VARIDX(x)].name); return; }
    if (x == 0) { snprintf(o, cap, "undef"); return; }
    if (f->val[x].kind == VK_VER) snprintf(o, cap, "%s_%d", f->var[f->val[x].var].name, f->val[x].ver); else snprintf(o, cap, "t%d", x);
}
static void cgx_const_str(const CgxNode *n, char *o, size_t cap) {
    if (!n) { snprintf(o, cap, "?"); return; }
    switch (n->k) {
        case CGN_INT: snprintf(o, cap, "%lld", n->iv); break;
        case CGN_FLT: snprintf(o, cap, "%.17g", n->dv); break;
        case CGN_STR: snprintf(o, cap, "\"%.40s\"", n->s ? n->s : ""); break;
        case CGN_BOOL: snprintf(o, cap, n->iv ? "true" : "false"); break;
        case CGN_NULL: snprintf(o, cap, "null"); break;
        default: snprintf(o, cap, "?"); break;
    }
}
static void cgx_ins_dump(const CgxFnIr *f, const CgxIns *x, CgxB *o) {
    char t1[96], t2[96], t3[96];
    cgx_val_str(f, x->dst, t1, sizeof t1);
    if (x->dst != CGX_NIL) cgx_bf(o, "    %s%s%s = ", t1, x->d2 != CGX_NIL ? ", " : "", x->d2 != CGX_NIL ? (cgx_val_str(f, x->d2, t3, sizeof t3), t3) : ""); else cgx_bs(o, "    ");
    cgx_bs(o, CGX_IO_NAME[x->op]);
    switch (x->op) {
        case IO_CONST: cgx_const_str(x->node, t2, sizeof t2); cgx_bf(o, " %s", t2); break;
        case IO_BIN: cgx_val_str(f, x->a, t2, sizeof t2); cgx_val_str(f, x->b, t3, sizeof t3); cgx_bf(o, " %s %s %s", t2, CGO_SYM[x->sub], t3); break;
        case IO_UN: cgx_val_str(f, x->a, t2, sizeof t2); cgx_bf(o, " %s %s", CGO_SYM[x->sub], t2); break;
        case IO_PARAM: cgx_bf(o, " #%d", x->sub); break;
        case IO_GLOBAL: case IO_UNDEF: if (x->node && x->node->s) cgx_bf(o, " %s", x->node->s); break;
        case IO_FIELD: cgx_val_str(f, x->a, t2, sizeof t2); cgx_bf(o, " %s .%s", t2, x->node && x->node->s ? x->node->s : "?"); break;
        case IO_JMP: case IO_NOP: break;
        default: {
            if (x->op == IO_CALL) cgx_bf(o, " %s", x->callee ? x->callee->s : x->node && x->node->s ? x->node->s : "?");
            if (x->op == IO_MCALL) cgx_bf(o, " .%s", x->node && x->node->s ? x->node->s : "?");
            for (int i = 0; i < cgx_ins_nop(x); i++) { const int u = cgx_ins_op(x, i); if (u == CGX_NIL) continue; cgx_val_str(f, u, t2, sizeof t2); cgx_bf(o, " %s", t2); }
        }
    }
    if (x->flags & IF_IMPLICIT) cgx_bs(o, " (implicit)");
    if (x->flags & IF_RETHROW) cgx_bs(o, " (rethrow)");
    cgx_bs(o, "\n");
}
static void cgx_fn_dump(const CgxFnIr *f, CgxB *o) {
    cgx_bf(o, "fn %s%s%s entry=B%d blocks=%d%s\n", f->cls ? f->cls->s : "", f->cls ? "." : "", f->name, f->entry, f->nbb, f->ssa ? " ssa" : "");
    for (int b = 0; b < f->nbb; b++) {
        if (!f->bb[b].reach) { cgx_bf(o, "  B%d: unreachable\n", b); continue; }
        cgx_bf(o, "  B%d: idom=B%d preds=[", b, f->dom.idom[b]);
        for (int k = f->g.pp[b]; k < f->g.pp[b + 1]; k++) cgx_bf(o, "%sB%d", k > f->g.pp[b] ? "," : "", f->g.p[k]);
        cgx_bs(o, "] succs=[");
        for (int e = f->g.sp[b]; e < f->g.sp[b + 1]; e++) cgx_bf(o, "%sB%d%s", e > f->g.sp[b] ? "," : "", f->g.s[e], f->ekind[e] == EK_EXC ? "!" : f->ekind[e] == EK_TRUE ? "T" : f->ekind[e] == EK_FALSE ? "F" : "");
        cgx_bs(o, "]\n");
        for (int k = 0; k < f->bb[b].ins.n; k++) cgx_ins_dump(f, &f->ins[f->bb[b].ins.p[k]], o);
    }
}

/* ================= 6. entrada e saida da IR ================= */
static void cgx_ir_free(CgxIr *ir) {
    if (!ir) return;
    for (int i = 0; i < ir->nfn; i++) cgx_fn_free_ir(&ir->fn[i]);
    free(ir->fn); free(ir->fnid); cgx_dg_free(&ir->cg); free(ir->cgcomp); free(ir->fx); free(ir->rec); free(ir->selfrec); free(ir);
}
/* monta o CFG e a SSA (do_ssa = 1; 0 so o CFG; -1 so o grafo de chamadas) de todas as funcoes e metodos; a AST deve estar validada (cgx_check). Os ids dos nos sao renumerados (estaveis) antes. */
static CgxIr *cgx_ir_build_ex(Cgx *g, int do_ssa, int globals) {
    CgxIr *ir = (CgxIr *)cgx_xcalloc(1, sizeof *ir); ir->g = g; ir->globals = globals;
    cgx_number(g);
    cgx_ir_collect(ir); cgx_cg_build(ir);
    if (do_ssa >= 0) for (int i = 0; i < ir->nfn; i++) { cgx_fn_build(ir, &ir->fn[i]); if (do_ssa) cgx_ssa_build(&ir->fn[i]); }       /* do_ssa < 0: so o grafo de chamadas e os efeitos */
    return ir;
}
static CgxIr *cgx_ir_build(Cgx *g) { return cgx_ir_build_ex(g, 1, 0); }
static void cgx_ir_dump(const CgxIr *ir, CgxB *o) { for (int i = 0; i < ir->nfn; i++) cgx_fn_dump(&ir->fn[i], o); }
