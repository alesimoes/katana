/* graphtest: testes do modulo de grafos, analises e otimizador do UCGE (docs/ucge.md, "UIR-G"), sem modelo e sem o katana.
 *   gcc -O1 -g -Wall -I. tools/ucge/graphtest.c -o /tmp/graphtest -lm && /tmp/graphtest tools/ucge/tasks [pasta de .csp extra]
 * Referencias independentes: dominadores, fronteira, componentes e lacos contra algoritmos ingenuos (conjuntos e fecho transitivo) em grafos
 * aleatorios; fluxo de dados contra iteracao em rodadas; grafos de livros (CFG do livro do dragao, figura 2 de Cooper-Harvey-Kennedy);
 * invariantes da SSA (cada uso dominado pela definicao) em todas as tarefas; relatorio exato em 20 programas semeados; dobra e simplificacao
 * com os casos de borda do Python (piso, divisao por zero, estouro de 64 bits, NaN, -0.0, texto com acento). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include "src/coder/ucge.c"
#include "src/coder/ucgereg.c"
#include "src/coder/ucgecheck.c"
#include "src/coder/ucgetype.c"
#include "src/coder/ucgedoc.c"
#include "src/coder/ucgepy.c"
#include "src/coder/ucgecfg.c"
#include "src/coder/ucgeflow.c"
#include "src/coder/ucgeopt.c"

static int NT = 0, NBAD = 0;
#define CHECK(c, ...) do { NT++; if (!(c)) { NBAD++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)
static unsigned long long RS = 88172645463325252ull;
static unsigned rnd(void) { RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return (unsigned)(RS >> 11); }

/* ---------- referencias ingenuas ---------- */
static void naive_reach(const CgxDg *g, int src, char *seen) {
    memset(seen, 0, (size_t)g->n); int *st = (int *)malloc(sizeof(int) * (size_t)(g->n + 1)); int sp = 0; seen[src] = 1; st[sp++] = src;
    while (sp) { const int v = st[--sp]; for (int e = g->sp[v]; e < g->sp[v + 1]; e++) if (!seen[g->s[e]]) { seen[g->s[e]] = 1; st[sp++] = g->s[e]; } }
    free(st);
}
/* dom[v] como conjunto (bytes): iteracao sobre conjuntos ate nao mudar */
static void naive_dom(const CgxDg *g, int entry, char *reach, char *dom /* n*n */) {
    const int n = g->n; naive_reach(g, entry, reach);
    for (int v = 0; v < n; v++) for (int u = 0; u < n; u++) dom[v * n + u] = reach[v];
    memset(dom + entry * n, 0, (size_t)n); dom[entry * n + entry] = 1;
    for (int ch = 1; ch;) {
        ch = 0;
        for (int v = 0; v < n; v++) {
            if (!reach[v] || v == entry) continue;
            char *nw = (char *)malloc((size_t)n); memset(nw, 1, (size_t)n); int any = 0;
            for (int k = g->pp[v]; k < g->pp[v + 1]; k++) { const int p = g->p[k]; if (!reach[p]) continue; any = 1; for (int u = 0; u < n; u++) nw[u] &= dom[p * n + u]; }
            if (!any) memset(nw, 0, (size_t)n);
            nw[v] = 1;
            if (memcmp(nw, dom + v * n, (size_t)n)) { memcpy(dom + v * n, nw, (size_t)n); ch = 1; }
            free(nw);
        }
    }
}
static int naive_idom(const CgxDg *g, const char *dom, int v) {                      /* o dominador estrito com mais dominadores */
    const int n = g->n; int best = -1, bc = -1;
    for (int d = 0; d < n; d++) if (d != v && dom[v * n + d]) { int c = 0; for (int u = 0; u < n; u++) c += dom[d * n + u]; if (c > bc) { bc = c; best = d; } }
    return best;
}
static void rand_graph(int n, int ne, int **from, int **to) {
    *from = (int *)malloc(sizeof(int) * (size_t)(ne + 1)); *to = (int *)malloc(sizeof(int) * (size_t)(ne + 1));
    for (int i = 0; i < ne; i++) { (*from)[i] = (int)(rnd() % (unsigned)n); (*to)[i] = (int)(rnd() % (unsigned)n); }
}
static void test_dom_graph(const CgxDg *g, int entry, const char *label) {
    const int n = g->n; CgxDom d; cgx_dom_build(&d, g, entry);
    char *reach = (char *)malloc((size_t)n), *dom = (char *)malloc((size_t)n * (size_t)n);
    naive_dom(g, entry, reach, dom);
    int bad = 0;
    for (int v = 0; v < n && !bad; v++) {
        if (!reach[v]) { if (d.idom[v] != -1) bad = 1; continue; }
        const int ni = v == entry ? entry : naive_idom(g, dom, v);
        if (d.idom[v] != ni) bad = 1;
        for (int u = 0; u < n && !bad; u++) if (reach[u] && cgx_dom_dominates(&d, u, v) != dom[v * n + u]) bad = 1;
    }
    CHECK(!bad, "%s: idom or dominates differs from the naive algorithm", label);
    /* fronteira: y esta em DF(x) sse x domina um predecessor de y e nao domina y estritamente */
    int badf = 0;
    for (int x = 0; x < n && !badf; x++) {
        if (!reach[x]) continue;
        for (int y = 0; y < n && !badf; y++) {
            if (!reach[y]) continue;
            int want = 0;
            for (int k = g->pp[y]; k < g->pp[y + 1]; k++) { const int p = g->p[k]; if (reach[p] && dom[p * n + x]) want = 1; }
            if (want && x != y && dom[y * n + x]) want = 0;
            int have = 0; for (int k = d.dfp[x]; k < d.dfp[x + 1]; k++) if (d.df[k] == y) have++;
            if (have > 1 || have != want) badf = 1;
        }
    }
    CHECK(!badf, "%s: dominance frontier differs from the definition", label);
    /* ordem pos-ordem reversa: todo no alcancavel aparece uma vez, a entrada primeiro, e idom[v] vem antes de v */
    int nr = 0; for (int v = 0; v < n; v++) nr += reach[v];
    int badr = d.nrpo != nr || (nr && d.rpo[0] != entry);
    for (int i = 1; i < d.nrpo && !badr; i++) if (d.rponum[d.idom[d.rpo[i]]] >= i) badr = 1;
    CHECK(!badr, "%s: reverse post-order", label);
    free(reach); free(dom); cgx_dom_free(&d);
}
static void test_scc_graph(const CgxDg *g, const char *label) {
    const int n = g->n; int *comp = (int *)malloc(sizeof(int) * (size_t)(n + 1)); const int nc = cgx_scc(g, comp);
    char *R = (char *)malloc((size_t)n * (size_t)n), *row = (char *)malloc((size_t)n);
    for (int v = 0; v < n; v++) { naive_reach(g, v, row); memcpy(R + v * n, row, (size_t)n); }
    int bad = 0, mx = -1;
    for (int u = 0; u < n && !bad; u++) { if (comp[u] < 0 || comp[u] >= nc) bad = 1; if (comp[u] > mx) mx = comp[u];
        for (int v = 0; v < n && !bad; v++) { const int same = R[u * n + v] && R[v * n + u]; if (same != (comp[u] == comp[v])) bad = 1; } }
    CHECK(!bad && mx == nc - 1, "%s: components differ from mutual reachability", label);
    int badt = 0;                                                                   /* aresta entre componentes: do numero maior para o menor (sumidouros primeiro) */
    for (int u = 0; u < n; u++) for (int e = g->sp[u]; e < g->sp[u + 1]; e++) if (comp[u] != comp[g->s[e]] && comp[u] < comp[g->s[e]]) badt = 1;
    CHECK(!badt, "%s: component order is not reverse topological", label);
    free(comp); free(R); free(row);
}
static void test_loops_graph(const CgxDg *g, int entry, const char *label) {
    const int n = g->n; CgxDom d; cgx_dom_build(&d, g, entry); CgxLoops L; cgx_loops_build(&L, g, &d);
    char *reach = (char *)malloc((size_t)n), *dom = (char *)malloc((size_t)n * (size_t)n); naive_dom(g, entry, reach, dom);
    int bad = 0, nl = 0;
    for (int h = 0; h < n && !bad; h++) {
        if (!reach[h]) continue;
        char *body = (char *)calloc((size_t)n, 1); int back = 0;
        for (int k = g->pp[h]; k < g->pp[h + 1]; k++) { const int u = g->p[k]; if (!reach[u] || !dom[u * n + h]) continue; back = 1;
            int *st = (int *)malloc(sizeof(int) * (size_t)(n + 1)); int sp = 0; body[h] = 1; if (!body[u]) { body[u] = 1; st[sp++] = u; }
            while (sp) { const int x = st[--sp]; for (int j = g->pp[x]; j < g->pp[x + 1]; j++) { const int y = g->p[j]; if (reach[y] && !body[y]) { body[y] = 1; st[sp++] = y; } } }
            free(st); }
        if (back) { nl++; int li = -1; for (int i = 0; i < L.nloop; i++) if (L.head[i] == h) li = i; if (li < 0) bad = 1; else for (int v = 0; v < n; v++) if ((int)cgx_bit_get(L.body[li], v) != body[v]) bad = 1; }
        free(body);
    }
    CHECK(!bad && nl == L.nloop, "%s: natural loops (%d vs %d)", label, nl, L.nloop);
    free(reach); free(dom); cgx_loops_free(&L); cgx_dom_free(&d);
}

/* ---------- grafos conhecidos ---------- */
static void build_edges(CgxDg *g, int n, const int (*e)[2], int ne) {
    int *f = (int *)malloc(sizeof(int) * (size_t)(ne + 1)), *t = (int *)malloc(sizeof(int) * (size_t)(ne + 1));
    for (int i = 0; i < ne; i++) { f[i] = e[i][0]; t[i] = e[i][1]; }
    cgx_dg_build(g, n, f, t, ne); free(f); free(t);
}
static void test_known_graphs(void) {
    /* livro do dragao, figura 9.38: B1..B10 (aqui 0..9) */
    static const int dragon[][2] = {{1,2},{1,3},{2,3},{3,4},{4,3},{4,5},{4,6},{5,7},{6,7},{7,4},{7,8},{8,3},{8,9},{8,10},{9,1},{10,7}};
    int e[40][2]; int ne = 0; for (int i = 0; i < 16; i++) { e[ne][0] = dragon[i][0] - 1; e[ne][1] = dragon[i][1] - 1; ne++; }
    CgxDg g; build_edges(&g, 10, (const int (*)[2])e, ne); CgxDom d; cgx_dom_build(&d, &g, 0);
    static const int want[10] = {1, 1, 1, 3, 4, 4, 4, 7, 8, 8};      /* idom de B1..B10 (1-based); B1 e a entrada */
    int ok = d.idom[0] == 0; for (int v = 1; v < 10; v++) if (d.idom[v] != want[v] - 1) ok = 0;
    CHECK(ok, "dragon book fig 9.38 immediate dominators");
    CgxLoops L; cgx_loops_build(&L, &g, &d);
    int hdr[10] = {0}; for (int i = 0; i < L.nloop; i++) hdr[L.head[i]] = 1;
    CHECK(L.nloop == 4 && hdr[0] && hdr[2] && hdr[3] && hdr[6], "dragon book loops: headers B1, B3, B4, B7 (got %d loops)", L.nloop);
    CHECK(L.depth[0] == 1 && L.depth[2] == 2 && L.depth[3] == 3 && L.depth[6] == 4 && L.depth[7] == 4 && L.depth[9] == 4, "dragon book loop depth");
    /* o laco do B1 (aresta 9->1) contem todos; o de B3 tem B3..B8, B10 */
    cgx_loops_free(&L); cgx_dom_free(&d); test_dom_graph(&g, 0, "dragon"); test_scc_graph(&g, "dragon"); test_loops_graph(&g, 0, "dragon"); cgx_dg_free(&g);
    /* Cooper, Harvey, Kennedy, figura 2 (irredutivel): 6 -> 5,4; 5 -> 1; 4 -> 2; 1 -> 2; 2 -> 1,3; 3 -> 2 */
    static const int chk[][2] = {{5,4},{5,3},{4,0},{3,1},{0,1},{1,0},{1,2},{2,1}};   /* nos 0..5 = 1..6 do artigo menos 1 */
    build_edges(&g, 6, chk, 8); cgx_dom_build(&d, &g, 5);
    ok = d.idom[5] == 5 && d.idom[4] == 5 && d.idom[3] == 5 && d.idom[0] == 5 && d.idom[1] == 5 && d.idom[2] == 1;
    CHECK(ok, "CHK paper fig 2 (irreducible) idoms");
    cgx_dom_free(&d); test_dom_graph(&g, 5, "chk-fig2"); test_loops_graph(&g, 5, "chk-fig2"); cgx_dg_free(&g);
    /* losango com laco */
    static const int dia[][2] = {{0,1},{0,2},{1,3},{2,3},{3,1},{3,4}};
    build_edges(&g, 5, dia, 6); cgx_dom_build(&d, &g, 0);
    CHECK(d.idom[3] == 0 && d.idom[1] == 0 && d.idom[4] == 3, "diamond with back edge idoms");
    int has = 0; for (int k = d.dfp[1]; k < d.dfp[2]; k++) if (d.df[k] == 3) has = 1;
    CHECK(has, "DF(1) contains the merge block 3");
    cgx_dom_free(&d); cgx_dg_free(&g);
}
static void test_random_graphs(void) {
    for (int it = 0; it < 500; it++) {
        const int n = 1 + (int)(rnd() % 40), ne = (int)(rnd() % (unsigned)(n * 3 + 2));
        int *f, *t; rand_graph(n, ne, &f, &t); CgxDg g; cgx_dg_build(&g, n, f, t, ne);
        char lab[48]; snprintf(lab, sizeof lab, "random graph %d (n=%d, e=%d)", it, n, ne);
        const int entry = (int)(rnd() % (unsigned)n);
        test_dom_graph(&g, entry, lab); test_scc_graph(&g, lab); test_loops_graph(&g, entry, lab);
        cgx_dg_free(&g); free(f); free(t);
    }
    /* grafo grande: a pilha explicita aguenta uma cadeia de 200 mil nos */
    { const int n = 200000; int *f = (int *)malloc(sizeof(int) * (size_t)n), *t = (int *)malloc(sizeof(int) * (size_t)n);
      for (int i = 0; i + 1 < n; i++) { f[i] = i; t[i] = i + 1; } f[n - 1] = n - 1; t[n - 1] = 0;
      CgxDg g; cgx_dg_build(&g, n, f, t, n); int *comp = (int *)malloc(sizeof(int) * (size_t)n); const int nc = cgx_scc(&g, comp);
      CgxDom d; cgx_dom_build(&d, &g, 0);
      CHECK(nc == 1 && d.idom[n - 1] == n - 2 && d.nrpo == n, "chain of %d nodes (iterative Tarjan and dominators, no recursion)", n);
      cgx_dom_free(&d); cgx_dg_free(&g); free(comp); free(f); free(t); }
}

/* ---------- fluxo de dados ---------- */
/* referencia: iteracao em rodadas ate nao mudar (maior ponto fixo) */
static void ref_solve(const CgxDg *g, const CgxDom *dom, int fwd, int must, int nb, const CgxW *gen, const CgxW *kill, const CgxW *bnd, CgxW *in, CgxW *out) {
    const int n = g->n, nw = cgx_nw(nb);
    for (int b = 0; b < n; b++) for (int w = 0; w < nw; w++) { in[b * nw + w] = must ? ~0ull : 0; out[b * nw + w] = must ? ~0ull : 0; }
    for (int ch = 1; ch;) {
        ch = 0;
        for (int i = 0; i < dom->nrpo; i++) {
            const int b = dom->rpo[i]; const int *sp = fwd ? g->pp : g->sp, *s = fwd ? g->p : g->s; const int none = sp[b + 1] == sp[b];
            CgxW *ib = fwd ? in + b * nw : out + b * nw, *ob = fwd ? out + b * nw : in + b * nw;
            CgxW acc[8]; int first = 1; for (int w = 0; w < nw; w++) acc[w] = 0;
            for (int k = sp[b]; k < sp[b + 1]; k++) { const int p = s[k]; if (dom->rponum[p] < 0) continue; const CgxW *po = fwd ? out + p * nw : in + p * nw;
                for (int w = 0; w < nw; w++) { if (first) acc[w] = po[w]; else acc[w] = must ? acc[w] & po[w] : acc[w] | po[w]; } first = 0; }
            const int boundary = fwd ? b == dom->entry : none;
            if (boundary) for (int w = 0; w < nw; w++) acc[w] = bnd ? bnd[w] : 0;
            else if (first) for (int w = 0; w < nw; w++) acc[w] = must ? ~0ull : 0;
            CgxW no[8]; for (int w = 0; w < nw; w++) no[w] = gen[b * nw + w] | (acc[w] & ~kill[b * nw + w]);
            for (int w = 0; w < nw; w++) { if (ib[w] != acc[w] || ob[w] != no[w]) ch = 1; ib[w] = acc[w]; ob[w] = no[w]; }
        }
    }
    (void)nb;
}
static void test_dfa_random(void) {
    int bad = 0, total = 0, badm[4] = {0, 0, 0, 0};
    for (int it = 0; it < 400; it++) {
        const int n = 2 + (int)(rnd() % 14), nb = 1 + (int)(rnd() % 100), nw = cgx_nw(nb), ne = n + (int)(rnd() % (unsigned)(n * 2));
        int *f, *t; rand_graph(n, ne, &f, &t); CgxDg g; cgx_dg_build(&g, n, f, t, ne); CgxDom dom; cgx_dom_build(&dom, &g, 0);
        CgxW *gen = cgx_bit_new(nb, n), *kill = cgx_bit_new(nb, n);
        for (int b = 0; b < n; b++) for (int i = 0; i < nb; i++) { const unsigned r = rnd() % 8; if (r == 0) cgx_bit_set(gen + b * nw, i); else if (r == 1) cgx_bit_set(kill + b * nw, i); }
        CgxW *bnd = cgx_bit_new(nb, 1); for (int i = 0; i < nb; i++) if (rnd() % 4 == 0) cgx_bit_set(bnd, i);
        for (int mode = 0; mode < 4; mode++) {
            const int fwd = mode & 1, must = (mode >> 1) & 1;
            CgxDfa d; memset(&d, 0, sizeof d); d.g = &g; d.dom = &dom; d.fwd = fwd; d.must = must; d.nbits = nb; d.nw = nw; d.gen = gen; d.kill = kill; d.boundary = bnd;
            cgx_dfa_solve(&d);
            CgxW *ri = cgx_bit_new(nb, n), *ro = cgx_bit_new(nb, n); ref_solve(&g, &dom, fwd, must, nb, gen, kill, bnd, ri, ro);
            for (int i = 0; i < dom.nrpo; i++) { const int b = dom.rpo[i]; for (int w = 0; w < nw; w++) if (d.in[b * nw + w] != ri[b * nw + w] || d.out[b * nw + w] != ro[b * nw + w]) { bad++; badm[mode]++; } }
            total++; cgx_dfa_free(&d); free(d.out); free(ri); free(ro);
        }
        free(gen); free(kill); free(bnd); cgx_dom_free(&dom); cgx_dg_free(&g); free(f); free(t);
    }
    CHECK(bad == 0, "worklist solver vs round-robin reference: %d differences in %d runs (by mode bwd-may %d, fwd-may %d, bwd-must %d, fwd-must %d)", bad, total, badm[0], badm[1], badm[2], badm[3]);
}
static void test_dfa_known(void) {
    /* definicoes alcancaveis, livro do dragao fig 9.12: B1 {d1,d2,d3}, B2 {d4,d5}, B3 {d6}, B4 {d7}; B1->B2, B2->B3, B2->B4, B3->B4, B4->B2, B4->saida */
    static const int e[][2] = {{0,1},{1,2},{1,3},{2,3},{3,1},{3,4}};
    CgxDg g; build_edges(&g, 5, e, 6); CgxDom dom; cgx_dom_build(&dom, &g, 0);
    const int nb = 7, nw = 1; CgxW *gen = cgx_bit_new(nb, 5), *kill = cgx_bit_new(nb, 5);
    #define D(i) (1ull << ((i) - 1))
    gen[0] = D(1) | D(2) | D(3); kill[0] = D(4) | D(5) | D(6) | D(7);
    gen[1] = D(4) | D(5);        kill[1] = D(1) | D(2) | D(7);
    gen[2] = D(6);               kill[2] = D(3);
    gen[3] = D(7);               kill[3] = D(1) | D(4);
    CgxDfa d; memset(&d, 0, sizeof d); d.g = &g; d.dom = &dom; d.fwd = 1; d.must = 0; d.nbits = nb; d.nw = nw; d.gen = gen; d.kill = kill; cgx_dfa_solve(&d);
    CHECK(d.in[0] == 0 && d.out[0] == (D(1) | D(2) | D(3)), "reaching defs B1");
    CHECK(d.in[1] == (D(1) | D(2) | D(3) | D(5) | D(6) | D(7)), "reaching defs IN[B2] = {d1,d2,d3,d5,d6,d7} (book)");
    CHECK(d.out[1] == (D(3) | D(4) | D(5) | D(6)), "reaching defs OUT[B2]");
    CHECK(d.in[2] == d.out[1] && d.out[2] == (D(4) | D(5) | D(6)), "reaching defs B3");
    CHECK(d.in[3] == (D(3) | D(4) | D(5) | D(6)) && d.out[3] == (D(3) | D(5) | D(6) | D(7)), "reaching defs B4");
    cgx_dfa_free(&d); free(d.out); free(gen); free(kill); cgx_dom_free(&dom); cgx_dg_free(&g);
    /* vivacidade (para tras, uniao): variaveis a=0,b=1,c=2; B0: a=1;b=2  B1: c=a+b  B2: a=a+1  B3: ret c ; 0->1, 1->2, 1->3, 2->1 */
    static const int e2[][2] = {{0,1},{1,2},{1,3},{2,1}};
    build_edges(&g, 4, e2, 4); cgx_dom_build(&dom, &g, 0); gen = cgx_bit_new(3, 4); kill = cgx_bit_new(3, 4);
    #define V(i) (1ull << (i))
    kill[0] = V(0) | V(1); gen[1] = V(0) | V(1); kill[1] = V(2); gen[2] = V(0); kill[2] = V(0); gen[3] = V(2);
    memset(&d, 0, sizeof d); d.g = &g; d.dom = &dom; d.fwd = 0; d.must = 0; d.nbits = 3; d.nw = 1; d.gen = gen; d.kill = kill; cgx_dfa_solve(&d);
    CHECK(d.in[0] == 0 && d.in[1] == (V(0) | V(1)) && d.in[2] == (V(0) | V(1)) && d.in[3] == V(2), "liveness IN sets");
    CHECK(d.out[1] == (V(0) | V(1) | V(2)) && d.out[2] == (V(0) | V(1)) && d.out[3] == 0, "liveness OUT sets (c is live out of B1 because of B3)");
    cgx_dfa_free(&d); free(d.out); free(gen); free(kill); cgx_dom_free(&dom); cgx_dg_free(&g);
    /* expressoes disponiveis (para frente, intersecao): e0 gerada em B0, morta em B2; B0->B1, B0->B2, B1->B3, B2->B3 */
    static const int e3[][2] = {{0,1},{0,2},{1,3},{2,3}};
    build_edges(&g, 4, e3, 4); cgx_dom_build(&dom, &g, 0); gen = cgx_bit_new(2, 4); kill = cgx_bit_new(2, 4);
    gen[0] = V(0) | V(1); kill[2] = V(0);
    memset(&d, 0, sizeof d); d.g = &g; d.dom = &dom; d.fwd = 1; d.must = 1; d.nbits = 2; d.nw = 1; d.gen = gen; d.kill = kill; cgx_dfa_solve(&d);
    CHECK(d.in[1] == (V(0) | V(1)) && d.in[2] == (V(0) | V(1)) && d.in[3] == V(1) && d.out[2] == V(1), "available expressions: killed on one path means not available at the merge");
    cgx_dfa_free(&d); free(d.out); free(gen); free(kill); cgx_dom_free(&dom); cgx_dg_free(&g);
    #undef D
    #undef V
}

/* ---------- programas ---------- */
static int ld(Cgx *g, const char *src) { const int ok = cgx_parse(g, src) && cgx_check(g); if (!ok) { for (int i = 0; i < g->nerr; i++) printf("  spec error line %d: %s\n", g->err[i].line, g->err[i].msg); } return ok; }
static char *flow_str(const char *src, int globals, int *nwarn) {
    Cgx g; if (!ld(&g, src)) { cgx_free(&g); return strdup("SPEC ERROR"); }
    CgxIr *ir = cgx_ir_build_ex(&g, 1, globals); CgxFlowReport r; cgx_flow_run(ir, &r);
    CgxB b = {0}; for (int i = 0; i < r.n; i++) if (r.f[i].warn) cgx_bf(&b, "%s%s@%d", b.n ? " " : "", CGX_FK_NAME[r.f[i].kind], r.f[i].line);
    if (nwarn) *nwarn = cgx_report_warnings(&r);
    char *res = b.p ? b.p : strdup(""); cgx_report_free(&r); cgx_ir_free(ir); cgx_free(&g); return res;
}
static void expect_flow(const char *name, const char *src, const char *want) {
    char *got = flow_str(src, 0, NULL);
    CHECK(!strcmp(got, want), "seeded program `%s`: expected [%s], got [%s]", name, want, got);
    free(got);
}
/* otimiza e gera o Python (sem comentarios) */
static char *opt_py(const char *src, int passes, CgxOptReport *rep) {
    Cgx g; if (!ld(&g, src)) { cgx_free(&g); return strdup("SPEC ERROR"); }
    CgxOptOpts o = {passes, 0, 0}; cgx_opt_run(&g, &o, rep);
    CgxGenOpts go = {200, 0, 4}; CgxB code = {0}; const int ok = cgx_gen_python(&g, &go, &code, NULL);
    char *res = ok && code.p ? code.p : strdup("GEN ERROR"); cgx_free(&g); return res;
}
static void expect_opt(const char *name, const char *src, int passes, const char *has, const char *hasnt) {
    char *py = opt_py(src, passes, NULL);
    CHECK(strstr(py, has) != NULL, "%s: expected `%s` in\n%s", name, has, py);
    if (hasnt) CHECK(strstr(py, hasnt) == NULL, "%s: did not expect `%s` in\n%s", name, hasnt, py);
    free(py);
}

static void test_seeded_flow(void) {
    expect_flow("clean", "fn f(a: int) -> int\n  let b = a + 1\n  return b * 2\n", "");
    expect_flow("unused variable", "fn f(a: int) -> int\n  let b = 5\n  return a\n", "unused-var@2");
    expect_flow("unreachable after return", "fn f(a: int) -> int\n  return a\n  let b = 5\n  return b\n", "unreachable@3");
    expect_flow("unreachable after throw", "fn f(a: int) -> int\n  throw \"x\"\n  return a\n", "unreachable@3");
    expect_flow("always true if", "fn f(a: int) -> int\n  if 2 > 1\n    return a\n  return 0\n", "dead-branch@2 unreachable@4");
    expect_flow("always false if with else", "fn f(a: int) -> int\n  if 1 > 2\n    return 1\n  else\n    return a\n", "dead-branch@2");
    expect_flow("condition decided through a variable", "fn f(a: int) -> int\n  let k = 3\n  if k == 3\n    return a\n  return 0\n", "dead-branch@3 unreachable@5");
    expect_flow("conditional constant (Wegman and Zadeck)", "fn f() -> int\n  let x = 1\n  let y = 0\n  if x == 1\n    y = 2\n  else\n    y = 3\n  return y\n", "dead-store@3 dead-branch@4");
    expect_flow("dead store", "fn f(a: int) -> int\n  let x = 1\n  x = a\n  return x\n", "dead-store@2");
    expect_flow("assigned but never used", "fn f(a: int) -> int\n  let s = 0\n  s += a\n  return a\n", "unused-var@2");
    expect_flow("loop variable updates only itself", "fn f(a: int) -> int\n  let n = 0\n  for i in 0..a\n    n += i\n  return a\n", "unused-var@2");
    expect_flow("infinite loop", "fn f() -> int\n  while true\n    print(1)\n  return 0\n", "infinite-loop@2 unreachable@4");
    expect_flow("loop with break is fine", "fn f(a: int) -> int\n  while true\n    if a > 0\n      break\n    a = a + 1\n  return a\n", "");
    expect_flow("loop with return inside is fine", "fn f(a: int) -> int\n  while a < 100\n    if a > 3\n      return a\n    a += 1\n  return 0\n", "");
    expect_flow("while false", "fn f(a: int) -> int\n  while 1 > 2\n    a += 1\n  return a\n", "dead-branch@2");
    expect_flow("code after the loop that never ends", "fn f(a: int) -> int\n  while a > 0\n    a += 1\n    if a > 100\n      return a\n  return 0\n", "");
    expect_flow("try catch finally clean", "fn f(a: int) -> int\n  try\n    return 10 // a\n  catch e\n    print(errmsg(e))\n    return 0\n  finally\n    print(1)\n", "");
    expect_flow("variable used only in finally", "fn f(a: int) -> int\n  let t = 2\n  try\n    return a\n  finally\n    print(t)\n", "");
    expect_flow("unused in catch, parameter unused is only info", "fn f(a: int, b: int) -> int\n  return a\n", "");
    expect_flow("elif chain with constant condition", "fn f(a: int) -> int\n  if a > 0\n    return 1\n  elif 1 > 2\n    return 2\n  else\n    return 3\n", "dead-branch@4");
    expect_flow("short circuit decided", "fn f(a: int) -> int\n  if false and a > 0\n    return 1\n  return 2\n", "dead-branch@2");
    expect_flow("method and field are not locals", "class C\n  field n: int = 0\n  fn inc() -> int\n    self.n += 1\n    return self.n\n", "");
    expect_flow("raw reads and defines", "fn f(a: int) -> int\n  let k = 5\n  raw python defines r\n    r = k + a\n  return r\n", "");
    expect_flow("two copies of finally (return in try)", "fn f(a: int) -> int\n  try\n    if a > 0\n      return 1\n    a += 1\n  finally\n    let z = 4\n    print(z)\n  return a\n", "");
}
static void test_callgraph(void) {
    const char *src =
        "fn leaf(x: int) -> int\n  return x + 1\n"
        "fn isEven(n: int) -> bool\n  if n == 0\n    return true\n  return isOdd(n - 1)\n"
        "fn isOdd(n: int) -> bool\n  if n == 0\n    return false\n  return isEven(n - 1)\n"
        "fn fact(n: int) -> int\n  if n <= 1\n    return 1\n  return n * fact(n - 1)\n"
        "fn shout(x: int) -> int\n  print(x)\n  return leaf(x)\n"
        "fn top(n: int) -> int\n  if isEven(n)\n    return fact(n)\n  return shout(n)\n"
        "test top(4) == 24\n";
    Cgx g; CHECK(ld(&g, src), "callgraph spec"); CgxIr *ir = cgx_ir_build(&g);
    int idx[8]; const char *nm[6] = {"leaf", "isEven", "isOdd", "fact", "shout", "top"};
    for (int k = 0; k < 6; k++) { idx[k] = -1; for (int i = 0; i < ir->nfn; i++) if (!strcmp(ir->fn[i].name, nm[k])) idx[k] = i; }
    CHECK(ir->cgcomp[idx[1]] == ir->cgcomp[idx[2]] && ir->cgcomp[idx[0]] != ir->cgcomp[idx[1]], "mutual recursion isEven/isOdd form one component");
    CHECK(ir->rec[idx[1]] && ir->rec[idx[2]] && ir->rec[idx[3]] && !ir->rec[idx[0]] && !ir->rec[idx[4]] && !ir->rec[idx[5]], "recursion flags");
    CHECK(ir->selfrec[idx[3]] && !ir->selfrec[idx[1]], "direct recursion only for fact");
    int ord[8]; const int n = cgx_cg_order(ir, ord); int pos[8];
    for (int i = 0; i < n; i++) for (int k = 0; k < 6; k++) if (ord[i] == idx[k]) pos[k] = i;
    CHECK(n == 6 && pos[0] < pos[4] && pos[4] < pos[5] && pos[1] < pos[5] && pos[3] < pos[5], "generation order puts callees before callers");
    CHECK(cgx_fx_removable(ir->fx[idx[0]]) && !ir->fx[idx[0]], "leaf is pure and total");
    CHECK(!cgx_fx_removable(ir->fx[idx[4]]) && (ir->fx[idx[4]] & CGX_FX_WRITE), "shout writes (print) and is not removable");
    CHECK((ir->fx[idx[3]] & CGX_FX_LOOP) && !cgx_fx_removable(ir->fx[idx[3]]), "recursive functions may not terminate (not removable)");
    CHECK(!(ir->fx[idx[5]] & ~(CGX_FX_WRITE | CGX_FX_LOOP | CGX_FX_READ | CGX_FX_THROW)), "top effects are the union of its callees");
    CgxFlowReport r; cgx_flow_run(ir, &r); int rec = 0; for (int i = 0; i < r.n; i++) if (r.f[i].kind == FK_RECURSION) rec++;
    CHECK(rec == 2, "recursion findings: one mutual group and one direct (%d)", rec);
    CgxB b = {0}; cgx_report_str(&r, &b); CHECK(b.p && strstr(b.p, "mutual recursion between isEven, isOdd") && !strstr(b.p, "exact"), "recursion message and no claim of exactness");
    free(b.p); cgx_report_free(&r); cgx_ir_free(ir); cgx_free(&g);
}
static void test_redundant(void) {
    int n1, n2; Cgx g;
    ld(&g, "fn f(a: int, b: int, c: bool) -> int\n  let x = a + b\n  if c\n    a = 1\n  let y = a + b\n  return x + y\n"); CgxIr *ir = cgx_ir_build(&g); CgxFlowReport r; cgx_flow_run(ir, &r); n1 = r.nredundant; cgx_report_free(&r); cgx_ir_free(ir); cgx_free(&g);
    ld(&g, "fn f(a: int, b: int, c: bool) -> int\n  let x = a + b\n  if c\n    print(1)\n  let y = a + b\n  return x + y\n"); ir = cgx_ir_build(&g); cgx_flow_run(ir, &r); n2 = r.nredundant; cgx_report_free(&r); cgx_ir_free(ir); cgx_free(&g);
    CHECK(n1 == 0 && n2 >= 1, "available expressions: a+b killed on one path (%d) vs available on all paths (%d)", n1, n2);
}
static void test_use_before_def(void) {
    Cgx g; CHECK(cgx_parse(&g, "fn f(a: int) -> int\n  let b = z + 1\n  return b\n"), "parse without check");
    CgxIr *ir = cgx_ir_build(&g); CgxFlowReport r; cgx_flow_run(ir, &r);
    CHECK(r.count[FK_UNDEFNAME] == 1, "unresolved name is reported on an unchecked tree");
    cgx_report_free(&r); cgx_ir_free(ir); cgx_free(&g);
    /* a variavel que so um raio de execucao define: via try, a definicao dentro do try nao chega ao catch */
    CHECK(cgx_parse(&g, "fn f(a: int) -> int\n  try\n    let b = 10 // a\n    return b\n  catch e\n    return b\n"), "parse");
    ir = cgx_ir_build(&g); cgx_flow_run(ir, &r);
    CHECK(r.count[FK_UNDEFNAME] == 1, "name declared inside try is not visible in catch");
    cgx_report_free(&r); cgx_ir_free(ir); cgx_free(&g);
}

/* a aresta excepcional de um bloco vale para todas as suas instrucoes: nenhuma instrucao de fora do try pode dividir o bloco com o corpo */
static void test_try_split(void) {
    Cgx g; CHECK(ld(&g, "fn f(a: int) -> int\n  let t = 10 // a\n  try\n    return 1 // a\n  catch e\n    return t\n"), "spec");
    CgxIr *ir = cgx_ir_build(&g); const CgxFnIr *f = &ir->fn[0]; int ok = 1, nexc = 0;
    for (int b = 0; b < f->nbb; b++) {
        if (!f->bb[b].reach || f->bb[b].exc < 0) continue;
        nexc++;
        for (int k = 0; k + 2 < f->bb[b].ins.n; k++) { const CgxIns *x = &f->ins[f->bb[b].ins.p[k]]; if (x->op == IO_BIN && x->sub == CGO_IDIV) ok = 0; }   /* a divisao de fora do try nao esta num bloco com aresta excepcional */
    }
    CHECK(nexc == 1 && ok, "exactly one block carries the exceptional edge and it holds no unprotected division (%d)", nexc);
    cgx_ir_free(ir); cgx_free(&g);
}

/* retorno que falta: o CFG ve o que o validador sintatico nao ve (e o contrario) */
static void test_missing_return(void) {
    Cgx g; CgxIr *ir; CgxFlowReport r;
    CHECK(cgx_parse(&g, "fn f(a: int) -> int\n  if a > 0\n    return 1\n"), "parse");
    ir = cgx_ir_build(&g); cgx_flow_run(ir, &r); CHECK(r.count[FK_MISSINGRET] == 1, "a path that falls off the end is found (%d)", r.count[FK_MISSINGRET]); cgx_report_free(&r); cgx_ir_free(ir); cgx_free(&g);
    CHECK(cgx_parse(&g, "fn f(a: int) -> int\n  while true\n    a += 1\n"), "parse");
    ir = cgx_ir_build(&g); cgx_flow_run(ir, &r); CHECK(r.count[FK_MISSINGRET] == 0 && r.count[FK_INFLOOP] == 1, "an endless loop does not fall off the end (%d missing, %d endless)", r.count[FK_MISSINGRET], r.count[FK_INFLOOP]); cgx_report_free(&r); cgx_ir_free(ir); cgx_free(&g);
    CHECK(cgx_parse(&g, "fn f(a: int) -> int\n  if a > 0\n    return 1\n  else\n    return 2\n"), "parse");
    ir = cgx_ir_build(&g); cgx_flow_run(ir, &r); CHECK(r.count[FK_MISSINGRET] == 0, "both branches return"); cgx_report_free(&r); cgx_ir_free(ir); cgx_free(&g);
}

/* ---------- SSA, dominadores sobre CFGs reais ---------- */
static void check_fn_invariants(const CgxFnIr *f, const char *label) {
    char msg[300]; const int bad = cgx_ir_verify_fn(f, msg, sizeof msg);
    CHECK(!bad, "%s::%s verify: %s", label, f->name, msg);
    /* dominadores do CFG real contra o ingenuo */
    const int n = f->g.n; char *reach = (char *)malloc((size_t)n), *dom = (char *)malloc((size_t)n * (size_t)n); naive_dom(&f->g, f->entry, reach, dom);
    int bd = 0; for (int v = 0; v < n && !bd; v++) { if (!reach[v]) continue; const int ni = v == f->entry ? v : naive_idom(&f->g, dom, v); if (f->dom.idom[v] != ni) bd = 1; }
    CHECK(!bd, "%s::%s dominators of the real CFG", label, f->name);
    free(reach); free(dom);
    /* pruned SSA: nenhum phi cujo resultado nunca e usado por nada (pois so se poe phi onde a variavel e viva) */
    int *uses = (int *)calloc((size_t)f->nval + 1, sizeof(int));
    for (int i = 0; i < f->nins; i++) { const CgxIns *x = &f->ins[i]; if (!f->bb[x->blk].reach) continue; for (int o = 0; o < cgx_ins_nop(x); o++) { const int u = cgx_ins_op(x, o); if (u > 0 && u != CGX_NIL) uses[u]++; } }
    int unused_phi = 0; for (int i = 0; i < f->nins; i++) if (f->ins[i].op == IO_PHI && f->bb[f->ins[i].blk].reach && f->ins[i].dst > 0 && !uses[f->ins[i].dst]) unused_phi++;
    CHECK(unused_phi == 0, "%s::%s has %d phi nodes whose result is never used (SSA is not pruned)", label, f->name, unused_phi);
    free(uses);
}
static void test_ssa_on_file(const char *path) {
    FILE *fp = fopen(path, "rb"); if (!fp) return; fseek(fp, 0, SEEK_END); const long n = ftell(fp); fseek(fp, 0, SEEK_SET); char *src = (char *)malloc((size_t)n + 1); if (fread(src, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(src); return; } src[n] = 0; fclose(fp);
    Cgx g; if (!(cgx_parse(&g, src) && cgx_check(&g))) { cgx_free(&g); free(src); return; }
    CgxIr *ir = cgx_ir_build(&g); CgxB d1 = {0}, d2 = {0};
    for (int i = 0; i < ir->nfn; i++) { check_fn_invariants(&ir->fn[i], path); }
    cgx_ir_dump(ir, &d1);
    CgxIr *ir2 = cgx_ir_build(&g); cgx_ir_dump(ir2, &d2);
    CHECK(d1.p && d2.p && !strcmp(d1.p, d2.p), "%s: IR dump is stable across two builds", path);
    for (int i = 0; i < ir->nfn; i++) { cgx_ssa_out(&ir->fn[i]); int phis = 0; for (int k = 0; k < ir->fn[i].nins; k++) if (ir->fn[i].ins[k].op == IO_PHI) phis++;
        char msg[300]; CHECK(phis == 0 && !cgx_ir_verify_fn(&ir->fn[i], msg, sizeof msg), "%s::%s out of SSA leaves no phi (%s)", path, ir->fn[i].name, msg); }
    free(d1.p); free(d2.p); cgx_ir_free(ir); cgx_ir_free(ir2); cgx_free(&g); free(src);
}
static int test_dir(const char *dir, int (*fn)(const char *)) {
    DIR *d = opendir(dir); if (!d) return 0; struct dirent *e; char names[512][300]; int n = 0;
    while ((e = readdir(d)) && n < 512) { const size_t l = strlen(e->d_name); if (l > 4 && !strcmp(e->d_name + l - 4, ".csp")) snprintf(names[n++], sizeof names[0], "%s/%s", dir, e->d_name); }
    closedir(d);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (strcmp(names[i], names[j]) > 0) { char t[300]; memcpy(t, names[i], sizeof t); memcpy(names[i], names[j], sizeof t); memcpy(names[j], t, sizeof t); }
    for (int i = 0; i < n; i++) fn(names[i]);
    return n;
}
static int ssa_file_cb(const char *p) { test_ssa_on_file(p); return 0; }
static char *slurp_file(const char *path) { FILE *fp = fopen(path, "rb"); if (!fp) return NULL; fseek(fp, 0, SEEK_END); const long n = ftell(fp); fseek(fp, 0, SEEK_SET); char *s = (char *)malloc((size_t)n + 1); if (fread(s, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(s); return NULL; } s[n] = 0; fclose(fp); return s; }
static int flow_clean_cb(const char *p) {
    char *src = slurp_file(p); int nw = 0; char *r = flow_str(src, 0, &nw);
    CHECK(nw == 0, "task %s: %d findings that are warnings: %s", p, nw, r);
    free(r); free(src); return 0;
}
static int opt_idem_cb(const char *p) {
    char *src = slurp_file(p); Cgx g; if (!ld(&g, src)) { cgx_free(&g); free(src); return 0; }
    CgxOptReport r1, r2; CgxOptOpts o = {CGX_O_ALL, 0, 0}; cgx_opt_run(&g, &o, &r1); cgx_opt_run(&g, &o, &r2);
    CHECK(r2.rounds == 1 && !(r2.folded + r2.propagated + r2.copies + r2.algebra + r2.dead + r2.unreachable + r2.branches + r2.loops + r2.temps + r2.inlined), "%s: the optimizer is idempotent (second run changed something)", p);
    CgxGenOpts go = {88, 1, 4}; CgxB code = {0}; CHECK(cgx_gen_python(&g, &go, &code, NULL), "%s: optimized AST still generates", p); free(code.p);
    cgx_free(&g); free(src); return 0;
}

/* ---------- otimizador: casos de borda do Python ---------- */
#define RET1(expr) "fn f() -> int\n  return " expr "\n"
static void test_opt_semantics(void) {
    const int P = CGX_O_DEFAULT;
    expect_opt("fold arithmetic", RET1("2 * 3 + 4"), P, "return 10", NULL);
    expect_opt("floor division negative", RET1("(0 - 7) // 2"), P, "return -4", NULL);
    expect_opt("floor division by negative", RET1("7 // (0 - 2)"), P, "return -4", NULL);
    expect_opt("floored modulo negative dividend", RET1("(0 - 7) % 3"), P, "return 2", NULL);
    expect_opt("floored modulo negative divisor", RET1("7 % (0 - 2)"), P, "return -1", NULL);
    expect_opt("exact negative floor division", RET1("(0 - 6) // 3"), P, "return -2", NULL);
    expect_opt("division by zero is not folded (//)", RET1("1 // 0"), P, "1 // 0", NULL);
    expect_opt("division by zero is not folded (%)", RET1("1 % 0"), P, "1 % 0", NULL);
    expect_opt("division by zero is not folded (/)", "fn f() -> float\n  return 1 / 0\n", P, "1 / 0", NULL);
    expect_opt("true division int/int", "fn f() -> float\n  return 10 / 4\n", P, "return 2.5", NULL);
    expect_opt("float addition keeps the Python value", "fn f() -> float\n  return 0.1 + 0.2\n", P, "0.30000000000000004", NULL);
    expect_opt("float overflow to inf is not folded", "fn f() -> float\n  return 1e308 * 10.0\n", P, "1e+308 * 10.0", NULL);
    expect_opt("int overflow of 64 bits is not folded (add)", RET1("9223372036854775807 + 1"), P, "9223372036854775807 + 1", NULL);
    expect_opt("int overflow of 64 bits is not folded (mul)", RET1("4611686018427387904 * 2"), P, "4611686018427387904 * 2", NULL);
    expect_opt("int overflow just above 2^63 (mul)", RET1("3037000500 * 3037000500"), P, "3037000500 * 3037000500", NULL);
    expect_opt("big product inside 64 bits is folded", RET1("3037000499 * 3037000499"), P, "return 9223372030926249001", NULL);
    expect_opt("int and float beyond 2^53 are not mixed", "fn f() -> float\n  return 9007199254740993 + 1.0\n", P, "9007199254740993 + 1.0", NULL);
    expect_opt("string concatenation", "fn f() -> str\n  return \"ab\" + \"cd\"\n", P, "return \"abcd\"", NULL);
    expect_opt("upper of ASCII", "fn f() -> str\n  return upper(\"abc\")\n", P, "return \"ABC\"", NULL);
    expect_opt("upper of non-ASCII is left to Python", "fn f() -> str\n  return upper(\"caf\xc3\xa9\")\n", P, ".upper()", NULL);
    expect_opt("len counts code points", RET1("len(\"caf\xc3\xa9\")"), P, "return 4", NULL);
    expect_opt("compare strings", "fn f() -> bool\n  return \"a\" < \"b\"\n", P, "return True", NULL);
    expect_opt("max keeps the winner's type", "fn f() -> float\n  return max(2, 1.5)\n", P, "return 2", "1.5");
    expect_opt("min of equal int and float returns the first", "fn f() -> float\n  return min(1, 1.0)\n", P, "return 1\n", "1.0");
    expect_opt("negative zero survives", "fn f() -> float\n  return 0.0 * (0.0 - 1.0)\n", P, "return -0.0", NULL);
    expect_opt("int(float) truncates", RET1("int(0.0 - 2.7)"), P, "return -2", NULL);
    expect_opt("floor of float", RET1("floor(0.0 - 2.5)"), P, "return -3", NULL);
    expect_opt("sqrt of a negative is not folded", "fn f() -> float\n  return sqrt(0.0 - 4.0)\n", P, "math.sqrt", NULL);
    /* algebra: so onde a identidade vale */
    expect_opt("x*1 and x+0 on int", "fn f(x: int) -> int\n  return x * 1 + 0\n", P, "return x\n", "* 1");
    expect_opt("x*0 on int with a pure operand", "fn f(x: int) -> int\n  return x * 0\n", P, "return 0", NULL);
    expect_opt("x*0 keeps an operand that can raise", "fn f(x: int, y: int) -> int\n  return (x // y) * 0\n", P, "x // y * 0", NULL);
    expect_opt("x-x on int", "fn f(x: int) -> int\n  return x - x\n", P, "return 0", NULL);
    expect_opt("x==x on int", "fn f(x: int) -> bool\n  return x == x\n", P, "return True", NULL);
    expect_opt("x==x on float is NaN-safe", "fn f(x: float) -> bool\n  return x == x\n", P, "x == x", NULL);
    expect_opt("x-x on float is NaN/inf-safe", "fn f(x: float) -> float\n  return x - x\n", P, "x - x", NULL);
    expect_opt("x+0.0 on float keeps -0.0", "fn f(x: float) -> float\n  return x + 0.0\n", P, "x + 0.0", NULL);
    expect_opt("x*1.0 on float", "fn f(x: float) -> float\n  return x * 1.0\n", P, "return x\n", NULL);
    expect_opt("x//1 and x%1", "fn f(x: int) -> int\n  return x // 1 + x % 1\n", P, "return x\n", NULL);
    expect_opt("not not", "fn f(x: bool) -> bool\n  return not not x\n", P, "return x\n", NULL);
    expect_opt("not ==", "fn f(x: int, y: int) -> bool\n  return not (x == y)\n", P, "x != y", NULL);
    expect_opt("not < is NOT flipped (NaN)", "fn f(x: float, y: float) -> bool\n  return not (x < y)\n", P, "not x < y", NULL);
    expect_opt("and false keeps a raising operand", "fn f(x: int) -> bool\n  return (10 // x > 0) and false\n", P, "10 // x", NULL);
    expect_opt("and false drops a pure operand", "fn f(x: int) -> bool\n  return (x > 0) and false\n", P, "return False", NULL);
    expect_opt("false and anything never evaluates the right side", "fn f(x: int) -> bool\n  return false and (10 // x > 0)\n", P, "return False", NULL);
    /* fluxo */
    expect_opt("dead branch removed", "fn f(a: int) -> int\n  if 2 > 1\n    return a\n  else\n    return 0\n", P, "return a", "else");
    expect_opt("code after return removed", "fn f(a: int) -> int\n  return a\n  print(1)\n  return 0\n", P, "return a", "print");
    expect_opt("constant propagation through variables", "fn f(a: int) -> int\n  let x = 4\n  let y = x * 2\n  return y + a\n", P, "return 8 + a", "x = ");
    expect_opt("copy propagation then dead copy removed", "fn f(a: int) -> int\n  let b = a\n  let c = b + 1\n  return c * c\n", P, "a + 1", "b = ");
    expect_opt("copy not propagated after the source changes", "fn f(a: int) -> int\n  let b = a\n  a = a + 1\n  return b * 10 + a\n", P, "b * 10", NULL);
    expect_opt("dead store removed when pure", "fn f(a: int) -> int\n  let x = 1\n  x = a\n  return x\n", P, "return a", "x = 1");
    expect_opt("dead store with a raising value stays", "fn f(a: int) -> int\n  let x = 10 // a\n  x = 2\n  return x\n", P, "10 // a", NULL);
    expect_opt("unused call result keeps the call", "fn g(a: int) -> int\n  print(a)\n  return a\nfn f(a: int) -> int\n  let u = g(a)\n  return 1\n", P, "g(a)", "u = ");
    expect_opt("unused pure call is removed", "fn g(a: int) -> int\n  return a + 1\nfn f(a: int) -> int\n  let u = g(a)\n  return 1\n", P, "return 1", "g(a)\n");
    expect_opt("temporary removed", "fn g(a: int) -> int\n  print(a)\n  return a\nfn f(a: int) -> int\n  let r = g(a)\n  return r\n", P, "return g(a)", "r = ");
    expect_opt("temporary kept when the use is not the only one", "fn f(a: int) -> int\n  let r = a + 1\n  return r * r\n", P, "r = a + 1", NULL);
    expect_opt("while false removed", "fn f(a: int) -> int\n  while 1 > 2\n    a += 1\n  return a\n", P, "return a", "while");
    expect_opt("try finally kept", "fn f(a: int) -> int\n  try\n    return a\n  finally\n    print(1)\n", P, "finally:", NULL);
    expect_opt("catch handler kept when the body can raise", "fn f(a: int) -> int\n  try\n    return 10 // a\n  catch e\n    return 0\n", P, "except Exception", NULL);
    expect_opt("variable assigned in try is visible in the handler", "fn f(a: int) -> int\n  let x = 1\n  try\n    x = 2\n    let y = 10 // a\n    x = 3\n  catch e\n    return x\n  return x\n", P, "return 2", NULL);
    expect_opt("the value after the try is the one assigned last", "fn f(a: int) -> int\n  let x = 1\n  try\n    x = 2\n    let y = 10 // a\n    x = 3\n  catch e\n    return x\n  return x\n", P, "return 3", NULL);
    expect_opt("loop-carried value is not constant", "fn f() -> int\n  let s = 0\n  for i in 0..4\n    s += i\n  return s\n", P, "s += i", NULL);
    expect_opt("module constant kept by default", "const K = 7\nfn f() -> int\n  return K * 2\n", P, "K * 2", NULL);
    expect_opt("module constant propagated with GLOBALS", "const K = 7\nfn f() -> int\n  return K * 2\n", P | CGX_O_GLOBALS, "return 14", NULL);
    expect_opt("inline of a one-line function", "fn sq(x: int) -> int\n  return x * x\nfn f(a: int) -> int\n  return sq(a + 1)\n", P | CGX_O_INLINE, "(a + 1) * (a + 1)", "sq(a");
    expect_opt("no inline of a recursive function", "fn fact(n: int) -> int\n  if n <= 1\n    return 1\n  return n * fact(n - 1)\nfn f(a: int) -> int\n  return fact(a)\n", P | CGX_O_INLINE, "fact(a)", NULL);
    expect_opt("inline keeps an effectful argument once", "fn g(x: int) -> int\n  print(x)\n  return x\nfn h(x: int) -> int\n  return x + 1\nfn f(a: int) -> int\n  return h(g(a))\n", P | CGX_O_INLINE, "g(a) + 1", NULL);
    expect_opt("inline refuses to duplicate an effectful argument", "fn g(x: int) -> int\n  print(x)\n  return x\nfn h(x: int) -> int\n  return x * x\nfn f(a: int) -> int\n  return h(g(a))\n", P | CGX_O_INLINE, "h(g(a))", NULL);
    expect_opt("passes are independent: no folding when FOLD is off", RET1("2 * 3"), CGX_O_DCE, "2 * 3", NULL);
    expect_opt("readable names survive", "fn total(price: int, qty: int) -> int\n  let subtotal = price * qty\n  let tax = subtotal // 10\n  return subtotal + tax\n", P, "subtotal = price * qty", NULL);
}
static void test_opt_report(void) {
    CgxOptReport r; char *py = opt_py("fn f(a: int) -> int\n  let x = 5\n  let y = x + 1\n  if y > 3\n    return a\n  return 0\n", CGX_O_DEFAULT, &r);
    CHECK(r.rounds >= 2 && r.branches == 1 && r.dead >= 2, "optimizer report counts (rounds %d, branches %d, dead %d)", r.rounds, r.branches, r.dead);
    CgxB b = {0}; cgx_opt_report_str(&r, &b); CHECK(strstr(b.p, "branch") != NULL, "report text"); free(b.p); free(py);
}

int main(int argc, char **argv) {
    test_known_graphs(); test_random_graphs(); test_dfa_random(); test_dfa_known();
    test_try_split(); test_missing_return(); test_seeded_flow(); test_callgraph(); test_redundant(); test_use_before_def();
    test_opt_semantics(); test_opt_report();
    if (argc > 1) { const int n = test_dir(argv[1], ssa_file_cb); CHECK(n >= 15, "tasks directory has %d specs", n); test_dir(argv[1], flow_clean_cb); test_dir(argv[1], opt_idem_cb); }
    for (int i = 2; i < argc; i++) { test_dir(argv[i], ssa_file_cb); test_dir(argv[i], opt_idem_cb); }
    printf("%s: %d checks, %d failed\n", NBAD ? "FAILED" : "OK", NT, NBAD);
    return NBAD != 0;
}
