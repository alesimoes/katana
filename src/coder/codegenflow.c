/* ---------- coder/codegenflow.c: Codegen, UIR-G parte 2: analises de fluxo de dados, SCCP, chamadas e relatorio (docs/codegen.md, secao 6.4 a 6.6) ----------
 * Um unico resolvedor de ponto fixo com lista de trabalho para reticulados de conjuntos de bits (direcao, uniao ou intersecao, funcao de
 * transferencia gen/kill ou escrita a mao) e as instancias: vivacidade (para tras), definicoes alcancaveis (para frente, uniao), expressoes
 * disponiveis (para frente, intersecao) e atribuicao definitiva (para frente, intersecao). A propagacao esparsa condicional de constantes
 * (Wegman e Zadeck) roda sobre a SSA com um reticulado proprio (topo, constante, fundo) e acha tambem os blocos inalcancaveis. Em cima
 * disso: codigo morto por marcacao a partir das instrucoes com efeito, variaveis nao usadas, retornos faltando, lacos sem saida, e o
 * relatorio com certeza por achado ([structural] fato da especificacao, [derived] vem de uma analise), nunca "exato". */

/* ================= 1. o resolvedor generico ================= */
typedef struct CgxDfa CgxDfa;
struct CgxDfa {
    const CgxDg *g; const CgxDom *dom;
    int fwd, must;                  /* fwd: para frente; must: meet = intersecao (senao uniao) */
    int nbits, nw;
    const CgxW *gen, *kill;         /* por bloco (nb * nw): out = gen | (in & ~kill) */
    void (*xfer)(void *ctx, int b, const CgxW *in, CgxW *out);      /* se dado, substitui gen/kill */
    void *ctx;
    const CgxW *boundary;           /* valor na entrada (para frente) ou nas saidas (para tras); NULL = vazio */
    CgxW *in, *out;                 /* resultado: nb * nw */
    long visits;                    /* quantas vezes um bloco foi processado */
};
static void cgx_dfa_solve(CgxDfa *d) {
    const CgxDg *g = d->g; const int n = g->n, nw = d->nw;
    d->in = cgx_bit_new(d->nbits ? d->nbits : 1, n); d->out = cgx_bit_new(d->nbits ? d->nbits : 1, n); d->visits = 0;
    CgxW *tmp = (CgxW *)cgx_xcalloc((size_t)nw + 1, sizeof(CgxW)), *acc = (CgxW *)cgx_xcalloc((size_t)nw + 1, sizeof(CgxW));
    int *q = (int *)cgx_xcalloc((size_t)n + 1, sizeof(int)); char *inq = (char *)cgx_xcalloc((size_t)n + 1, 1); int qh = 0, qt = 0, qn = 0;
    const int np = d->dom->nrpo;
    /* o "alto" do reticulado: vazio (uniao) ou cheio (intersecao); blocos ainda nao visitados valem alto e nao entram no meet */
    char *vis = (char *)cgx_xcalloc((size_t)n + 1, 1);
    for (int i = 0; i < np; i++) { const int b = d->fwd ? d->dom->rpo[i] : d->dom->rpo[np - 1 - i]; q[qt++] = b; inq[b] = 1; qn++; }
    const int *sp_ = d->fwd ? g->pp : g->sp, *s_ = d->fwd ? g->p : g->s;                     /* "predecessores" na direcao da analise */
    const int *np_ = d->fwd ? g->sp : g->pp, *n_ = d->fwd ? g->s : g->p;                      /* "sucessores" na direcao da analise */
    while (qn) {
        const int b = q[qh]; qh = (qh + 1) % (n + 1); qn--; inq[b] = 0; d->visits++;
        CgxW *bin = d->fwd ? d->in + (size_t)b * (size_t)nw : d->out + (size_t)b * (size_t)nw;      /* o lado "de entrada" da direcao */
        CgxW *bout = d->fwd ? d->out + (size_t)b * (size_t)nw : d->in + (size_t)b * (size_t)nw;
        int any = 0; memset(acc, 0, sizeof(CgxW) * (size_t)nw);
        for (int k = sp_[b]; k < sp_[b + 1]; k++) {
            const int p = s_[k]; if (d->dom->rponum[p] < 0) continue;
            const CgxW *po = d->fwd ? d->out + (size_t)p * (size_t)nw : d->in + (size_t)p * (size_t)nw;
            if (!vis[p]) continue;
            if (!any) { memcpy(acc, po, sizeof(CgxW) * (size_t)nw); any = 1; }
            else if (d->must) for (int w = 0; w < nw; w++) acc[w] &= po[w]; else for (int w = 0; w < nw; w++) acc[w] |= po[w];
        }
        const int boundary_blk = d->fwd ? b == d->dom->entry : (sp_[b + 1] == sp_[b]);       /* entrada (frente) ou saida sem sucessor (tras) */
        if (boundary_blk) { if (d->boundary) memcpy(acc, d->boundary, sizeof(CgxW) * (size_t)nw); else memset(acc, 0, sizeof(CgxW) * (size_t)nw); }
        else if (!any) { if (d->must) memset(acc, 0xff, sizeof(CgxW) * (size_t)nw); }             /* sem informacao ainda: o alto do reticulado (so blocos sem caminho ate a fronteira) */
        memcpy(bin, acc, sizeof(CgxW) * (size_t)nw);
        if (d->xfer) d->xfer(d->ctx, b, bin, tmp);
        else for (int w = 0; w < nw; w++) tmp[w] = d->gen[(size_t)b * (size_t)nw + (size_t)w] | (bin[w] & ~d->kill[(size_t)b * (size_t)nw + (size_t)w]);
        const int changed = !vis[b] || memcmp(tmp, bout, sizeof(CgxW) * (size_t)nw) != 0;
        vis[b] = 1;
        if (changed) {
            memcpy(bout, tmp, sizeof(CgxW) * (size_t)nw);
            for (int k = np_[b]; k < np_[b + 1]; k++) { const int s = n_[k]; if (d->dom->rponum[s] >= 0 && !inq[s]) { inq[s] = 1; q[qt] = s; qt = (qt + 1) % (n + 1); qn++; } }
        }
    }
    free(tmp); free(acc); free(q); free(inq); free(vis);
}
static void cgx_dfa_free(CgxDfa *d) { free(d->in); free(d->out); d->in = d->out = NULL; }

/* ================= 2. instancias sobre o CFG de uma funcao (valem antes e depois da SSA: phi e ignorado, so variaveis contam) ================= */
/* gen/kill de vivacidade: gen = variaveis lidas antes de escritas no bloco; kill = escritas */
static void cgx_live_genkill(const CgxFnIr *f, int nw, CgxW *gen, CgxW *kill) {
    for (int b = 0; b < f->nbb; b++) {
        if (!f->bb[b].reach) continue;
        CgxW *ge = gen + (size_t)b * (size_t)nw, *ki = kill + (size_t)b * (size_t)nw;
        for (int k = 0; k < f->bb[b].ins.n; k++) {
            const CgxIns *x = &f->ins[f->bb[b].ins.p[k]]; if (x->op == IO_PHI) continue;
            for (int o = 0; o < cgx_ins_nop(x); o++) { const int v = cgx_opvar(f, cgx_ins_op(x, o)); if (v >= 0 && !cgx_bit_get(ki, v)) cgx_bit_set(ge, v); }
            const int d1 = cgx_opvar(f, x->dst), d2 = cgx_opvar(f, x->d2);
            if (d1 >= 0) cgx_bit_set(ki, d1);
            if (d2 >= 0) cgx_bit_set(ki, d2);
        }
    }
}
static void cgx_live_in(const CgxFnIr *f, CgxW **in) {
    const int nw = cgx_nw(f->nvar ? f->nvar : 1);
    CgxW *gen = cgx_bit_new(f->nvar ? f->nvar : 1, f->nbb), *kill = cgx_bit_new(f->nvar ? f->nvar : 1, f->nbb);
    cgx_live_genkill(f, nw, gen, kill);
    CgxDfa d; memset(&d, 0, sizeof d); d.g = &f->g; d.dom = &f->dom; d.fwd = 0; d.must = 0; d.nbits = f->nvar; d.nw = nw; d.gen = gen; d.kill = kill;
    cgx_dfa_solve(&d);
    *in = d.in; free(d.out); free(gen); free(kill);
}

/* definicoes alcancaveis: cada instrucao que escreve uma variavel (fora phi) e uma definicao; gen = a ultima de cada variavel no bloco */
typedef struct { int ndef; int *ins, *var; CgxW *in, *out; int nw; int *defno; CgxVi *vdefs; long visits; } CgxReach;
static void cgx_reach_free(CgxReach *r, int nvar) { free(r->ins); free(r->var); free(r->in); free(r->out); free(r->defno); for (int v = 0; v < nvar; v++) cgx_vi_free(&r->vdefs[v]); free(r->vdefs); memset(r, 0, sizeof *r); }
static void cgx_reach_run(const CgxFnIr *f, CgxReach *r) {
    memset(r, 0, sizeof *r);
    r->defno = (int *)cgx_xcalloc((size_t)f->nins + 1, sizeof(int)); r->vdefs = (CgxVi *)cgx_xcalloc((size_t)(f->nvar ? f->nvar : 1), sizeof(CgxVi));
    r->ins = (int *)cgx_xcalloc((size_t)f->nins * 2 + 2, sizeof(int)); r->var = (int *)cgx_xcalloc((size_t)f->nins * 2 + 2, sizeof(int));
    for (int i = 0; i < f->nins; i++) r->defno[i] = -1;
    int *def2 = (int *)cgx_xcalloc((size_t)f->nins + 1, sizeof(int)); for (int i = 0; i <= f->nins; i++) def2[i] = -1;
    for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach) for (int k = 0; k < f->bb[b].ins.n; k++) {
        const int ix = f->bb[b].ins.p[k]; const CgxIns *x = &f->ins[ix]; if (x->op == IO_PHI) continue;
        const int d1 = cgx_opvar(f, x->dst), d2 = cgx_opvar(f, x->d2);
        if (d1 >= 0) { r->defno[ix] = r->ndef; r->ins[r->ndef] = ix; r->var[r->ndef] = d1; cgx_vi_push(&r->vdefs[d1], r->ndef); r->ndef++; }
        if (d2 >= 0) { def2[ix] = r->ndef; r->ins[r->ndef] = ix; r->var[r->ndef] = d2; cgx_vi_push(&r->vdefs[d2], r->ndef); r->ndef++; }
    }
    const int nw = cgx_nw(r->ndef ? r->ndef : 1); r->nw = nw;
    CgxW *gen = cgx_bit_new(r->ndef ? r->ndef : 1, f->nbb), *kill = cgx_bit_new(r->ndef ? r->ndef : 1, f->nbb);
    for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach) {
        CgxW *ge = gen + (size_t)b * (size_t)nw, *ki = kill + (size_t)b * (size_t)nw;
        for (int k = 0; k < f->bb[b].ins.n; k++) {
            const int ix = f->bb[b].ins.p[k];
            for (int w = 0; w < 2; w++) {
                const int dn = w ? def2[ix] : r->defno[ix]; if (dn < 0) continue;
                const int v = r->var[dn];
                for (int j = 0; j < r->vdefs[v].n; j++) { const int o = r->vdefs[v].p[j]; if (o != dn) { cgx_bit_set(ki, o); cgx_bit_clr(ge, o); } }
                cgx_bit_set(ge, dn);
            }
        }
    }
    CgxDfa d; memset(&d, 0, sizeof d); d.g = &f->g; d.dom = &f->dom; d.fwd = 1; d.must = 0; d.nbits = r->ndef; d.nw = nw; d.gen = gen; d.kill = kill;
    cgx_dfa_solve(&d); r->in = d.in; r->out = d.out; r->visits = d.visits; free(gen); free(kill); free(def2);
}

/* atribuicao definitiva (para frente, intersecao): variaveis certamente escritas ao chegar em cada bloco */
static void cgx_defassign_run(const CgxFnIr *f, CgxW **in, int *nwo, long *visits) {
    const int nw = cgx_nw(f->nvar ? f->nvar : 1);
    CgxW *gen = cgx_bit_new(f->nvar ? f->nvar : 1, f->nbb), *kill = cgx_bit_new(f->nvar ? f->nvar : 1, f->nbb);
    for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach) for (int k = 0; k < f->bb[b].ins.n; k++) {
        const CgxIns *x = &f->ins[f->bb[b].ins.p[k]]; if (x->op == IO_PHI) continue;
        const int d1 = cgx_opvar(f, x->dst), d2 = cgx_opvar(f, x->d2);
        if (d1 >= 0) cgx_bit_set(gen + (size_t)b * (size_t)nw, d1);
        if (d2 >= 0) cgx_bit_set(gen + (size_t)b * (size_t)nw, d2);
    }
    CgxDfa d; memset(&d, 0, sizeof d); d.g = &f->g; d.dom = &f->dom; d.fwd = 1; d.must = 1; d.nbits = f->nvar; d.nw = nw; d.gen = gen; d.kill = kill;
    cgx_dfa_solve(&d); *in = d.in; free(d.out); free(gen); free(kill); *nwo = nw; if (visits) *visits = d.visits;
}

/* expressoes disponiveis: BIN e UN sobre variaveis e constantes (temporarios que nao sao constantes contam como distintos). Chave = (op, operando, operando) */
typedef struct { int op, sub, ka, va, kb, vb; } CgxExprKey;       /* k: 0 nada, 1 variavel, 2 temporario, 3 constante (va = indice na tabela de constantes) */
typedef struct { CgxExprKey *key; int n, cap; CgxW *in, *out; int nw; long visits; int nredundant; CgxVi red; int *ht, htn; CgxVi *byvar; int nbyvar; } CgxAvail;
static void cgx_avail_free(CgxAvail *a) { free(a->key); free(a->in); free(a->out); cgx_vi_free(&a->red); free(a->ht); for (int i = 0; i < a->nbyvar; i++) cgx_vi_free(&a->byvar[i]); free(a->byvar); memset(a, 0, sizeof *a); }
static void cgx_avail_opkey(const CgxFnIr *f, int x, int *k, int *v) {
    if (x == CGX_NIL) { *k = 0; *v = 0; return; }
    if (x < 0) { *k = 1; *v = CGX_VARIDX(x); return; }
    if (f->val[x].var >= 0) { *k = 1; *v = f->val[x].var; return; }
    const int d = f->val[x].def;
    if (d >= 0 && f->ins[d].op == IO_CONST && f->ins[d].node) {            /* constante: o valor e a identidade */
        const CgxNode *n = f->ins[d].node; *k = 3;
        *v = n->k == CGN_INT ? (int)(n->iv * 31 + 1) : n->k == CGN_BOOL ? (int)n->iv + 7 : n->k == CGN_STR ? (int)cgx_hash(n->s ? n->s : "") : n->k == CGN_FLT ? (int)(n->dv * 1000) : 5;
        *v = *v * 8 + n->k; return;
    }
    *k = 2; *v = x;
}
static unsigned cgx_avail_hash(const CgxExprKey *k) { unsigned h = 2166136261u; const unsigned char *p = (const unsigned char *)k; for (size_t i = 0; i < sizeof *k; i++) h = (h ^ p[i]) * 16777619u; return h; }
static int cgx_avail_find(CgxAvail *a, const CgxExprKey *k, int add) {          /* tabela hash de enderecamento aberto sobre a chave inteira */
    if (a->n * 2 >= a->htn) {
        const int nn = a->htn ? a->htn * 2 : 256; int *t = (int *)cgx_xcalloc((size_t)nn, sizeof(int));
        for (int i = 0; i < a->n; i++) { unsigned h = cgx_avail_hash(&a->key[i]) & (unsigned)(nn - 1); while (t[h]) h = (h + 1) & (unsigned)(nn - 1); t[h] = i + 1; }
        free(a->ht); a->ht = t; a->htn = nn;
    }
    unsigned h = cgx_avail_hash(k) & (unsigned)(a->htn - 1);
    while (a->ht[h]) { if (!memcmp(&a->key[a->ht[h] - 1], k, sizeof *k)) return a->ht[h] - 1; h = (h + 1) & (unsigned)(a->htn - 1); }
    if (!add) return -1;
    a->key = (CgxExprKey *)cgx_xgrow(a->key, &a->cap, a->n + 1, sizeof(CgxExprKey)); a->key[a->n] = *k; a->ht[h] = a->n + 1; return a->n++;
}
static int cgx_avail_expr(const CgxFnIr *f, CgxAvail *a, const CgxIns *x, int add) {
    if (x->op != IO_BIN && x->op != IO_UN) return -1;
    CgxExprKey k; memset(&k, 0, sizeof k); k.op = x->op; k.sub = x->sub;
    cgx_avail_opkey(f, x->a, &k.ka, &k.va); cgx_avail_opkey(f, x->b, &k.kb, &k.vb);
    if (x->node && x->node->k != CGN_BIN && x->node->k != CGN_UN && x->node->k != CGN_ASSIGN) return -1;       /* so expressoes do usuario (nao o contador escondido dos lacos) */
    return cgx_avail_find(a, &k, add);
}
static void cgx_avail_run(const CgxFnIr *f, CgxAvail *a) {
    memset(a, 0, sizeof *a);
    for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach) for (int k = 0; k < f->bb[b].ins.n; k++) cgx_avail_expr(f, a, &f->ins[f->bb[b].ins.p[k]], 1);
    const int nw = cgx_nw(a->n ? a->n : 1); a->nw = nw;
    a->nbyvar = f->nvar; a->byvar = (CgxVi *)cgx_xcalloc((size_t)f->nvar + 1, sizeof(CgxVi));         /* expressoes que mencionam cada variavel */
    for (int j = 0; j < a->n; j++) { if (a->key[j].ka == 1) cgx_vi_push(&a->byvar[a->key[j].va], j); if (a->key[j].kb == 1 && !(a->key[j].ka == 1 && a->key[j].va == a->key[j].vb)) cgx_vi_push(&a->byvar[a->key[j].vb], j); }
    CgxW *gen = cgx_bit_new(a->n ? a->n : 1, f->nbb), *kill = cgx_bit_new(a->n ? a->n : 1, f->nbb);
    for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach) {
        CgxW *ge = gen + (size_t)b * (size_t)nw, *ki = kill + (size_t)b * (size_t)nw;
        for (int k = 0; k < f->bb[b].ins.n; k++) {
            const CgxIns *x = &f->ins[f->bb[b].ins.p[k]]; if (x->op == IO_PHI) continue;
            const int e = cgx_avail_expr(f, a, x, 0); if (e >= 0) cgx_bit_set(ge, e);
            for (int w = 0; w < 2; w++) {
                const int dv = cgx_opvar(f, w ? x->d2 : x->dst); if (dv < 0) continue;
                for (int q = 0; q < a->byvar[dv].n; q++) { const int j = a->byvar[dv].p[q]; cgx_bit_set(ki, j); cgx_bit_clr(ge, j); }
            }
        }
    }
    CgxDfa d; memset(&d, 0, sizeof d); d.g = &f->g; d.dom = &f->dom; d.fwd = 1; d.must = 1; d.nbits = a->n; d.nw = nw; d.gen = gen; d.kill = kill;
    cgx_dfa_solve(&d); a->in = d.in; a->out = d.out; a->visits = d.visits; free(gen); free(kill);
    /* computacoes redundantes: a chave ja disponivel quando a instrucao roda */
    CgxW *cur = (CgxW *)cgx_xcalloc((size_t)nw + 1, sizeof(CgxW));
    for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach) {
        memcpy(cur, a->in + (size_t)b * (size_t)nw, sizeof(CgxW) * (size_t)nw);
        for (int k = 0; k < f->bb[b].ins.n; k++) {
            const int ix = f->bb[b].ins.p[k]; const CgxIns *x = &f->ins[ix]; if (x->op == IO_PHI) continue;
            const int e = cgx_avail_expr(f, a, x, 0);
            if (e >= 0) { if (cgx_bit_get(cur, e)) { a->nredundant++; cgx_vi_push(&a->red, ix); } cgx_bit_set(cur, e); }
            for (int w = 0; w < 2; w++) {
                const int dv = cgx_opvar(f, w ? x->d2 : x->dst); if (dv < 0) continue;
                for (int q = 0; q < a->byvar[dv].n; q++) cgx_bit_clr(cur, a->byvar[dv].p[q]);
            }
        }
    }
    free(cur);
}

/* ================= 3. valores constantes e dobra (a semantica do Python, bit a bit; o que nao se garante nao se dobra) ================= */
enum { CV_TOP, CV_BOT, CV_INT, CV_FLT, CV_STR, CV_BOOL, CV_NULL };
typedef struct { int k; long long iv; double dv; const char *s; } CgxCv;
#define CGX_2P53 9007199254740992LL
static int cgx_cv_isnum(const CgxCv *c) { return c->k == CV_INT || c->k == CV_FLT; }
static int cgx_cv_ascii(const char *s) { for (; *s; s++) if ((unsigned char)*s >= 0x80) return 0; return 1; }
static int cgx_cv_same(const CgxCv *a, const CgxCv *b) {
    if (a->k != b->k) return 0;
    switch (a->k) { case CV_INT: case CV_BOOL: return a->iv == b->iv; case CV_FLT: return memcmp(&a->dv, &b->dv, sizeof(double)) == 0; case CV_STR: return !strcmp(a->s, b->s); default: return 1; }
}
static CgxCv cgx_cv_of_node(const CgxNode *n) {
    CgxCv c; memset(&c, 0, sizeof c); c.k = CV_BOT;
    if (!n) return c;
    switch (n->k) {
        case CGN_INT: c.k = CV_INT; c.iv = n->iv; break;
        case CGN_FLT: if (isfinite(n->dv)) { c.k = CV_FLT; c.dv = n->dv; } break;
        case CGN_STR: c.k = CV_STR; c.s = n->s ? n->s : ""; break;
        case CGN_BOOL: c.k = CV_BOOL; c.iv = n->iv ? 1 : 0; break;
        case CGN_NULL: c.k = CV_NULL; break;
    }
    return c;
}
static int cgx_cv_todouble(const CgxCv *c, double *d) {                       /* inteiro exatamente representavel ou flutuante */
    if (c->k == CV_FLT) { *d = c->dv; return 1; }
    if (c->k == CV_INT && c->iv >= -CGX_2P53 && c->iv <= CGX_2P53) { *d = (double)c->iv; return 1; }
    return 0;
}
static int cgx_add_ovf(long long a, long long b, long long *r) { if ((b > 0 && a > LLONG_MAX - b) || (b < 0 && a < LLONG_MIN - b)) return 1; *r = a + b; return 0; }
static int cgx_sub_ovf(long long a, long long b, long long *r) { if ((b < 0 && a > LLONG_MAX + b) || (b > 0 && a < LLONG_MIN + b)) return 1; *r = a - b; return 0; }
static int cgx_mul_ovf(long long a, long long b, long long *r) {
    if (a == 0 || b == 0) { *r = 0; return 0; }
    if ((a == -1 && b == LLONG_MIN) || (b == -1 && a == LLONG_MIN)) return 1;
    const long long p = (long long)((unsigned long long)a * (unsigned long long)b);
    if (p / b != a) { return 1; }
    *r = p; return 0;
}
/* ancora do arena: texto novo vive o tempo do Cgx (cgx_alloc); sem g, nao se dobra string nova */
static const char *cgx_cv_strdup(Cgx *g, const char *s, size_t n) { return g ? cgx_dupn(g, s, n) : NULL; }
static int cgx_cv_bin(Cgx *g, int op, const CgxCv *a, const CgxCv *b, CgxCv *r) {
    memset(r, 0, sizeof *r);
    const int num = cgx_cv_isnum(a) && cgx_cv_isnum(b);
    switch (op) {
        case CGO_ADD:
            if (a->k == CV_STR && b->k == CV_STR) {
                const size_t la = strlen(a->s), lb = strlen(b->s); if (la + lb > 4096) return 0;
                char *t = (char *)malloc(la + lb + 1); memcpy(t, a->s, la); memcpy(t + la, b->s, lb); t[la + lb] = 0;
                r->s = cgx_cv_strdup(g, t, la + lb); free(t); if (!r->s) return 0; r->k = CV_STR; return 1;
            }
            /* fallthrough */
        case CGO_SUB: case CGO_MUL:
            if (!num) return 0;
            if (a->k == CV_INT && b->k == CV_INT) {
                long long v; const int o = op == CGO_ADD ? cgx_add_ovf(a->iv, b->iv, &v) : op == CGO_SUB ? cgx_sub_ovf(a->iv, b->iv, &v) : cgx_mul_ovf(a->iv, b->iv, &v);
                if (o) { return 0; }
                r->k = CV_INT; r->iv = v; return 1;
            } else {
                double x, y; if (!cgx_cv_todouble(a, &x) || !cgx_cv_todouble(b, &y)) return 0;
                const double v = op == CGO_ADD ? x + y : op == CGO_SUB ? x - y : x * y;
                if (!isfinite(v)) { return 0; }
                r->k = CV_FLT; r->dv = v; return 1;
            }
        case CGO_DIV: {
            double x, y; if (!num || !cgx_cv_todouble(a, &x) || !cgx_cv_todouble(b, &y) || y == 0.0) return 0;
            const double v = x / y; if (!isfinite(v)) return 0; r->k = CV_FLT; r->dv = v; return 1;
        }
        case CGO_IDIV: case CGO_MOD: {
            if (a->k != CV_INT || b->k != CV_INT || b->iv == 0 || (a->iv == LLONG_MIN && b->iv == -1)) return 0;
            long long q = a->iv / b->iv, m = a->iv % b->iv;
            if (m != 0 && ((m < 0) != (b->iv < 0))) { q--; m += b->iv; }
            r->k = CV_INT; r->iv = op == CGO_IDIV ? q : m; return 1;
        }
        case CGO_EQ: case CGO_NE: {
            int eq;
            if (num) { double x, y; if (a->k == CV_INT && b->k == CV_INT) eq = a->iv == b->iv; else if (cgx_cv_todouble(a, &x) && cgx_cv_todouble(b, &y)) eq = x == y; else return 0; }
            else if (a->k == CV_STR && b->k == CV_STR) eq = !strcmp(a->s, b->s);
            else if (a->k == CV_BOOL && b->k == CV_BOOL) eq = a->iv == b->iv;
            else if (a->k == CV_NULL && b->k == CV_NULL) eq = 1;
            else if ((a->k == CV_NULL) != (b->k == CV_NULL)) eq = 0;                 /* None so e igual a None */
            else return 0;                                                            /* bool com numero, str com numero: o Python tem regras proprias */
            r->k = CV_BOOL; r->iv = op == CGO_EQ ? eq : !eq; return 1;
        }
        case CGO_LT: case CGO_LE: case CGO_GT: case CGO_GE: {
            int c;
            if (num) { double x, y; if (a->k == CV_INT && b->k == CV_INT) c = a->iv < b->iv ? -1 : a->iv > b->iv; else if (cgx_cv_todouble(a, &x) && cgx_cv_todouble(b, &y)) c = x < y ? -1 : x > y; else return 0; }
            else if (a->k == CV_STR && b->k == CV_STR && cgx_cv_ascii(a->s) && cgx_cv_ascii(b->s)) { c = strcmp(a->s, b->s); c = c < 0 ? -1 : c > 0; }
            else return 0;
            r->k = CV_BOOL; r->iv = op == CGO_LT ? c < 0 : op == CGO_LE ? c <= 0 : op == CGO_GT ? c > 0 : c >= 0; return 1;
        }
        case CGO_AND: case CGO_OR:
            if (a->k != CV_BOOL || b->k != CV_BOOL) return 0;
            r->k = CV_BOOL; r->iv = op == CGO_AND ? (a->iv && b->iv) : (a->iv || b->iv); return 1;
        case CGO_IN:
            if (a->k == CV_STR && b->k == CV_STR) { r->k = CV_BOOL; r->iv = strstr(b->s, a->s) != NULL; return 1; }
            return 0;
    }
    return 0;
}
static int cgx_cv_un(int op, const CgxCv *a, CgxCv *r) {
    memset(r, 0, sizeof *r);
    if (op == CGO_NOT) { if (a->k != CV_BOOL) return 0; r->k = CV_BOOL; r->iv = !a->iv; return 1; }
    if (op == CGO_NEG) {
        if (a->k == CV_INT) { if (a->iv == LLONG_MIN) return 0; r->k = CV_INT; r->iv = -a->iv; return 1; }
        if (a->k == CV_FLT) { r->k = CV_FLT; r->dv = -a->dv; return 1; }
    }
    return 0;
}
static size_t cgx_cv_utf8len(const char *s) { size_t n = 0; for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++; return n; }
static int cgx_cv_pyspace(char c) { return c == ' ' || (c >= 9 && c <= 13) || (c >= 28 && c <= 31); }
/* funcoes embutidas sobre constantes. Cada caso so dobra o que e identico no Python do backend; o resto devolve 0 */
static int cgx_cv_builtin(Cgx *g, int bi, const CgxCv *a, int n, CgxCv *r) {
    memset(r, 0, sizeof *r);
    double x, y;
    switch (bi) {
        case CGB_LEN: if (n == 1 && a[0].k == CV_STR) { r->k = CV_INT; r->iv = (long long)cgx_cv_utf8len(a[0].s); return 1; } return 0;
        case CGB_STR: if (n == 1 && a[0].k == CV_INT) { char t[32]; snprintf(t, sizeof t, "%lld", a[0].iv); r->s = cgx_cv_strdup(g, t, strlen(t)); if (!r->s) return 0; r->k = CV_STR; return 1; } if (n == 1 && a[0].k == CV_STR) { *r = a[0]; return 1; } return 0;
        case CGB_INT:
            if (n != 1) return 0;
            if (a[0].k == CV_INT) { *r = a[0]; return 1; }
            if (a[0].k == CV_FLT && fabs(a[0].dv) < 4.0e18) { r->k = CV_INT; r->iv = (long long)a[0].dv; return 1; }   /* trunc, como o int() do Python */
            return 0;
        case CGB_FLOAT: if (n == 1 && cgx_cv_isnum(&a[0]) && cgx_cv_todouble(&a[0], &x)) { r->k = CV_FLT; r->dv = x; return 1; } return 0;
        case CGB_UPPER: case CGB_LOWER: {
            if (n != 1 || a[0].k != CV_STR || !cgx_cv_ascii(a[0].s)) return 0;
            const size_t l = strlen(a[0].s); char *t = (char *)malloc(l + 1);
            for (size_t i = 0; i <= l; i++) t[i] = bi == CGB_UPPER ? (char)toupper((unsigned char)a[0].s[i]) : (char)tolower((unsigned char)a[0].s[i]);
            r->s = cgx_cv_strdup(g, t, l); free(t); if (!r->s) return 0; r->k = CV_STR; return 1;
        }
        case CGB_TRIM: {
            if (n != 1 || a[0].k != CV_STR || !cgx_cv_ascii(a[0].s)) return 0;
            const char *s = a[0].s; size_t l = strlen(s); while (l && cgx_cv_pyspace(s[0])) { s++; l--; } while (l && cgx_cv_pyspace(s[l - 1])) l--;
            r->s = cgx_cv_strdup(g, s, l); if (!r->s) return 0; r->k = CV_STR; return 1;
        }
        case CGB_ABS:
            if (n != 1) return 0;
            if (a[0].k == CV_INT && a[0].iv != LLONG_MIN) { r->k = CV_INT; r->iv = a[0].iv < 0 ? -a[0].iv : a[0].iv; return 1; }
            if (a[0].k == CV_FLT) { r->k = CV_FLT; r->dv = fabs(a[0].dv); return 1; }
            return 0;
        case CGB_MIN: case CGB_MAX:
            if (n != 2 || !cgx_cv_isnum(&a[0]) || !cgx_cv_isnum(&a[1])) return 0;
            if (a[0].k == CV_INT && a[1].k == CV_INT) { const int pick1 = bi == CGB_MAX ? a[1].iv > a[0].iv : a[1].iv < a[0].iv; *r = a[pick1]; return 1; }
            if (!cgx_cv_todouble(&a[0], &x) || !cgx_cv_todouble(&a[1], &y)) return 0;
            { const int pick1 = bi == CGB_MAX ? y > x : y < x; *r = a[pick1]; return 1; }        /* o Python devolve o objeto: o tipo do vencedor (o primeiro em empate) */
        case CGB_SQRT: if (n == 1 && cgx_cv_isnum(&a[0]) && cgx_cv_todouble(&a[0], &x) && x >= 0) { r->k = CV_FLT; r->dv = sqrt(x); return isfinite(r->dv); } return 0;
        case CGB_FLOOR:
            if (n != 1) return 0;
            if (a[0].k == CV_INT) { *r = a[0]; return 1; }
            if (a[0].k == CV_FLT && fabs(a[0].dv) < 4.0e18) { r->k = CV_INT; r->iv = (long long)floor(a[0].dv); return 1; }
            return 0;
        case CGB_STARTS: case CGB_ENDS:
            if (n != 2 || a[0].k != CV_STR || a[1].k != CV_STR) return 0;
            { const size_t ls = strlen(a[0].s), lp = strlen(a[1].s); r->k = CV_BOOL; r->iv = lp <= ls && (bi == CGB_STARTS ? !strncmp(a[0].s, a[1].s, lp) : !strcmp(a[0].s + ls - lp, a[1].s)); return 1; }
        case CGB_CONTAINS: if (n == 2 && a[0].k == CV_STR && a[1].k == CV_STR) { r->k = CV_BOOL; r->iv = strstr(a[0].s, a[1].s) != NULL; return 1; } return 0;
        case CGB_INDEXOF:
            if (n == 2 && a[0].k == CV_STR && a[1].k == CV_STR && cgx_cv_ascii(a[0].s) && cgx_cv_ascii(a[1].s)) { const char *p = strstr(a[0].s, a[1].s); r->k = CV_INT; r->iv = p ? (long long)(p - a[0].s) : -1; return 1; }
            return 0;
        case CGB_REVERSE:
            if (n == 1 && a[0].k == CV_STR && cgx_cv_ascii(a[0].s)) { const size_t l = strlen(a[0].s); char *t = (char *)malloc(l + 1); for (size_t i = 0; i < l; i++) t[i] = a[0].s[l - 1 - i]; t[l] = 0; r->s = cgx_cv_strdup(g, t, l); free(t); if (!r->s) return 0; r->k = CV_STR; return 1; }
            return 0;
        case CGB_REPLACE:
            if (n == 3 && a[0].k == CV_STR && a[1].k == CV_STR && a[2].k == CV_STR && a[1].s[0]) {
                CgxB b = {0}; const char *p = a[0].s, *q; const size_t lo = strlen(a[1].s);
                while ((q = strstr(p, a[1].s))) { cgx_bn(&b, p, (size_t)(q - p)); cgx_bs(&b, a[2].s); p = q + lo; }
                cgx_bs(&b, p); if (b.n > 4096) { free(b.p); return 0; }
                r->s = cgx_cv_strdup(g, b.p ? b.p : "", b.n); free(b.p); if (!r->s) return 0; r->k = CV_STR; return 1;
            }
            return 0;
        case CGB_SLICE:
            if (n >= 2 && a[0].k == CV_STR && cgx_cv_ascii(a[0].s) && a[1].k == CV_INT && (n < 3 || a[2].k == CV_INT)) {
                const long long l = (long long)strlen(a[0].s); long long i = a[1].iv, j = n < 3 ? l : a[2].iv;
                if (i < 0) { i += l; if (i < 0) i = 0; } if (i > l) i = l;
                if (j < 0) { j += l; if (j < 0) j = 0; } if (j > l) j = l;
                r->s = cgx_cv_strdup(g, a[0].s + i, j > i ? (size_t)(j - i) : 0); if (!r->s) return 0; r->k = CV_STR; return 1;
            }
            return 0;
    }
    return 0;
}
/* um no literal novo com o valor c (tipo anotado, para os backends) */
static CgxNode *cgx_cv_node(Cgx *g, const CgxCv *c, int line) {
    CgxNode *n = NULL;
    switch (c->k) {
        case CV_INT: n = cgx_node(g, CGN_INT, line); n->iv = c->iv; n->ty = cgx_t(CGT_INT); break;
        case CV_FLT: n = cgx_node(g, CGN_FLT, line); n->dv = c->dv; n->ty = cgx_t(CGT_FLOAT); break;
        case CV_STR: n = cgx_node(g, CGN_STR, line); n->s = cgx_dup(g, c->s); n->ty = cgx_t(CGT_STR); break;
        case CV_BOOL: n = cgx_node(g, CGN_BOOL, line); n->iv = c->iv; n->ty = cgx_t(CGT_BOOL); break;
        case CV_NULL: n = cgx_node(g, CGN_NULL, line); n->ty = cgx_t(CGT_NULL); break;
    }
    return n;
}

/* ================= 4. SCCP: propagacao esparsa condicional de constantes (Wegman e Zadeck) ================= */
typedef struct { CgxCv *lat; char *bex, *eex; int *upp, *up; int nupd; long evals; } CgxSccp;     /* lat por valor, bloco executavel, aresta executavel (indexada como g.s), usos por valor (CSR) */
static void cgx_sccp_free(CgxSccp *s) { free(s->lat); free(s->bex); free(s->eex); free(s->upp); free(s->up); memset(s, 0, sizeof *s); }
static CgxCv cgx_cv_meet(const CgxCv *a, const CgxCv *b) {
    if (a->k == CV_TOP) { return *b; }
    if (b->k == CV_TOP) { return *a; }
    if (a->k == CV_BOT || b->k == CV_BOT || !cgx_cv_same(a, b)) { CgxCv r; memset(&r, 0, sizeof r); r.k = CV_BOT; return r; }
    return *a;
}
static void cgx_sccp_run(const CgxIr *ir, const CgxFnIr *f, CgxSccp *S) {
    memset(S, 0, sizeof *S);
    const int nv = f->nval, nb = f->nbb; const CgxDg *g = &f->g;
    S->lat = (CgxCv *)cgx_xcalloc((size_t)nv + 1, sizeof(CgxCv)); S->bex = (char *)cgx_xcalloc((size_t)nb + 1, 1); S->eex = (char *)cgx_xcalloc((size_t)g->ne + 1, 1);
    for (int v = 0; v < nv; v++) S->lat[v].k = CV_TOP;
    S->lat[0].k = CV_BOT;
    /* listas de usos por valor (instrucoes) */
    S->upp = (int *)cgx_xcalloc((size_t)nv + 2, sizeof(int));
    for (int b = 0; b < nb; b++) if (f->bb[b].reach) for (int k = 0; k < f->bb[b].ins.n; k++) { const CgxIns *x = &f->ins[f->bb[b].ins.p[k]]; for (int o = 0; o < cgx_ins_nop(x); o++) { const int u = cgx_ins_op(x, o); if (u > 0 && u != CGX_NIL) S->upp[u + 1]++; } }
    for (int v = 0; v < nv; v++) S->upp[v + 1] += S->upp[v];
    S->up = (int *)cgx_xcalloc((size_t)S->upp[nv] + 1, sizeof(int)); int *fill = (int *)cgx_xcalloc((size_t)nv + 1, sizeof(int));
    for (int b = 0; b < nb; b++) if (f->bb[b].reach) for (int k = 0; k < f->bb[b].ins.n; k++) { const int ix = f->bb[b].ins.p[k]; const CgxIns *x = &f->ins[ix]; for (int o = 0; o < cgx_ins_nop(x); o++) { const int u = cgx_ins_op(x, o); if (u > 0 && u != CGX_NIL) S->up[S->upp[u] + fill[u]++] = ix; } }
    free(fill);
    CgxVi eq = {0}, vq = {0}; int eh = 0;                                           /* listas de trabalho: arestas do CFG e instrucoes a reavaliar */
    /* bloco `b` torna-se executavel: avalia tudo nele */
    #define EVAL_BLOCK(b) do { for (int kk = 0; kk < f->bb[b].ins.n; kk++) cgx_vi_push(&vq, f->bb[b].ins.p[kk]); } while (0)
    S->bex[f->entry] = 1; EVAL_BLOCK(f->entry);
    int vh = 0;
    while (eh < eq.n || vh < vq.n) {
        while (eh < eq.n) {                                                         /* aresta nova: torna executavel o destino */
            const int e = eq.p[eh++], t = g->s[e];
            if (S->eex[e]) continue;
            S->eex[e] = 1;
            if (!S->bex[t]) { S->bex[t] = 1; EVAL_BLOCK(t); }
            else for (int k = 0; k < f->bb[t].ins.n; k++) { const int ix = f->bb[t].ins.p[k]; if (f->ins[ix].op != IO_PHI) break; cgx_vi_push(&vq, ix); }   /* nova aresta: so os phis mudam */
        }
        if (vh < vq.n) {
            const int ix = vq.p[vh++]; const CgxIns *x = &f->ins[ix]; S->evals++;
            if (!S->bex[x->blk]) continue;
            CgxCv nvl; int has = 0; memset(&nvl, 0, sizeof nvl); nvl.k = CV_BOT;
            #define LAT(o) (((o) == CGX_NIL || (o) < 0) ? (CgxCv){.k = CV_BOT} : S->lat[(o)])
            switch (x->op) {
                case IO_CONST: nvl = cgx_cv_of_node(x->node); has = 1; break;
                case IO_COPY: nvl = LAT(x->a); has = 1; break;
                case IO_PHI: {
                    nvl.k = CV_TOP; has = 1;
                    for (int j = 0; j < x->nargs; j++) { const int e = f->g.pe[f->g.pp[x->blk] + j]; if (!S->eex[e]) continue; const CgxCv a = x->args[j] == 0 ? (CgxCv){.k = CV_BOT} : S->lat[x->args[j]]; nvl = cgx_cv_meet(&nvl, &a); }
                    break;
                }
                case IO_BIN: {
                    const CgxCv a = LAT(x->a), b = LAT(x->b); has = 1;
                    if (a.k == CV_TOP || b.k == CV_TOP) nvl.k = CV_TOP;
                    else if (a.k != CV_BOT && b.k != CV_BOT) { CgxCv r; if (cgx_cv_bin(ir->g, x->sub, &a, &b, &r)) nvl = r; else nvl.k = CV_BOT; }
                    break;
                }
                case IO_UN: {
                    const CgxCv a = LAT(x->a); has = 1;
                    if (a.k == CV_TOP) nvl.k = CV_TOP; else if (a.k != CV_BOT) { CgxCv r; if (cgx_cv_un(x->sub, &a, &r)) nvl = r; else nvl.k = CV_BOT; }
                    break;
                }
                case IO_CALL:
                    has = 1;
                    if (x->sub && !x->callee && x->nargs <= 4) {
                        CgxCv av[4]; int top = 0, bot = 0;
                        for (int j = 0; j < x->nargs; j++) { av[j] = LAT(x->args[j]); if (av[j].k == CV_TOP) top = 1; else if (av[j].k == CV_BOT) bot = 1; }
                        if (top && !bot) nvl.k = CV_TOP;
                        else if (!bot) { CgxCv r; if (cgx_cv_builtin(ir->g, x->sub, av, x->nargs, &r)) nvl = r; else nvl.k = CV_BOT; }
                    }
                    break;
                case IO_GLOBAL:
                    has = 1;
                    if (ir->globals && x->callee && x->callee->a && x->callee->a->k >= CGN_INT && x->callee->a->k <= CGN_NULL) nvl = cgx_cv_of_node(x->callee->a);
                    break;
                default: has = x->dst != CGX_NIL && x->dst >= 0; break;             /* qualquer outra definicao: desconhecida */
            }
            if (has && x->dst != CGX_NIL && x->dst > 0) {
                CgxCv *cur = &S->lat[x->dst];
                const CgxCv m = cgx_cv_meet(cur, &nvl);                          /* o valor so desce no reticulado: topo, constante, fundo */
                if (m.k != cur->k || !cgx_cv_same(&m, cur)) { *cur = m; S->nupd++; for (int j = S->upp[x->dst]; j < S->upp[x->dst + 1]; j++) cgx_vi_push(&vq, S->up[j]); }
            }
            if (x->d2 != CGX_NIL && x->d2 > 0 && S->lat[x->d2].k != CV_BOT) { S->lat[x->d2].k = CV_BOT; for (int j = S->upp[x->d2]; j < S->upp[x->d2 + 1]; j++) cgx_vi_push(&vq, S->up[j]); }
            #undef LAT
            /* terminadores: que arestas ficam executaveis */
            if (x->op == IO_BR) {
                const CgxCv c = x->a > 0 ? S->lat[x->a] : (CgxCv){.k = CV_BOT};
                for (int e = g->sp[x->blk]; e < g->sp[x->blk + 1]; e++) {
                    int on;
                    if (c.k == CV_TOP) on = 0;
                    else if (c.k == CV_BOOL) on = (f->ekind[e] == EK_TRUE) == (c.iv != 0);
                    else on = 1;
                    if (on && !S->eex[e]) cgx_vi_push(&eq, e);
                }
            } else if (x->op == IO_JMP || x->op == IO_THROW) {
                for (int e = g->sp[x->blk]; e < g->sp[x->blk + 1]; e++) if (!S->eex[e]) cgx_vi_push(&eq, e);
            }
        }
    }
    #undef EVAL_BLOCK
    /* um BR reavaliado depois (condicao passou de topo a constante) ja empurra as arestas pelo caminho acima; o resto de TOP vira indefinido (nunca executado) */
    cgx_vi_free(&eq); cgx_vi_free(&vq);
}

/* ================= 5. efeitos por instrucao, vivacidade de valores (codigo morto) ================= */
/* efeito de UMA instrucao (os operandos ja sao instrucoes proprias) */
static int cgx_ins_fx(const CgxIr *ir, const CgxFnIr *f, const CgxIns *x) {
    (void)f;
    switch (x->op) {
        case IO_BIN: case IO_UN:
            if (!x->node) return CGX_FX_THROW;
            if (x->node->k == CGN_BIN) return cgx_fx_op(x->node->op, x->node->a, x->node->b);
            if (x->node->k == CGN_UN) return cgx_fx_op(x->node->op, x->node->a, NULL);
            if (x->node->k == CGN_ASSIGN) return cgx_fx_op(x->sub, x->node->a, x->node->b);
            return 0;                                                                   /* contador escondido dos lacos for: inteiros */
        case IO_INDEX: return CGX_FX_READ | CGX_FX_THROW;
        case IO_FIELD: return CGX_FX_READ | (x->node && x->node->a && x->node->a->ty.k == CGT_CLASS ? 0 : CGX_FX_THROW);
        case IO_SETIDX: return CGX_FX_WRITE | CGX_FX_THROW;
        case IO_SETFLD: return CGX_FX_WRITE | (x->node && x->node->k == CGN_ASSIGN && x->node->a->a && x->node->a->a->ty.k == CGT_CLASS ? 0 : CGX_FX_THROW);
        case IO_CALL: case IO_MCALL: {
            if (x->op == IO_CALL && x->sub && !x->callee) { CgxNode tmp = *x->node; tmp.bi = x->sub; return cgx_fx_builtin(&tmp); }
            if (!x->callee) return CGX_BADFX;
            if (x->callee->k == CGN_CLASS) {
                const CgxNode *init = NULL; for (int i = 0; i < x->callee->b1.n; i++) if (x->callee->b1.p[i]->k == CGN_FN && !strcmp(x->callee->b1.p[i]->s, "init")) init = x->callee->b1.p[i];
                if (!init) return 0;
                return init->id > 0 && init->id < ir->nid && ir->fnid[init->id] >= 0 ? ir->fx[ir->fnid[init->id]] : CGX_BADFX;
            }
            return x->callee->id > 0 && x->callee->id < ir->nid && ir->fnid[x->callee->id] >= 0 ? ir->fx[ir->fnid[x->callee->id]] : CGX_BADFX;
        }
        case IO_RAWUSE: return CGX_BADFX;
        case IO_UNDEF: return CGX_FX_THROW;
        case IO_ITER: return CGX_FX_THROW;
    }
    return 0;
}
/* a instrucao nao pode sumir mesmo com o resultado sem uso */
static int cgx_ins_root(const CgxIr *ir, const CgxFnIr *f, const CgxIns *x) {
    switch (x->op) {
        case IO_BR: case IO_JMP: case IO_RET: case IO_THROW: case IO_SETIDX: case IO_SETFLD: case IO_RAWUSE: case IO_ITER: case IO_HASNEXT: case IO_NEXT: return 1;
        default: break;
    }
    /* uma definicao cujo lado direito nao pode sumir (pode lancar ou ter efeito) fica inteira: seus operandos continuam necessarios */
    if ((x->op == IO_COPY || x->op == IO_BIN) && x->node && (x->node->k == CGN_LET || x->node->k == CGN_ASSIGN)) {
        const CgxNode *rhs = x->node->k == CGN_LET ? x->node->a : x->node->b;
        if (!cgx_fx_removable(cgx_fx_expr(ir, rhs))) return 1;
    }
    if ((x->flags & IF_STMTROOT) && x->node && !cgx_fx_removable(cgx_fx_expr(ir, x->node))) return 1;     /* comando-chamada mantido inteiro */
    return !cgx_fx_removable(cgx_ins_fx(ir, f, x));
}
/* marcacao: instrucoes e valores vivos, a partir das raizes, so nos blocos executaveis. live_ins[i] / live_val[v] */
static void cgx_live_values(const CgxIr *ir, const CgxFnIr *f, const CgxSccp *S, char *live_val, char *live_ins) {
    CgxVi wl = {0};
    for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach && S->bex[b]) for (int k = 0; k < f->bb[b].ins.n; k++) {
        const int ix = f->bb[b].ins.p[k];
        if (cgx_ins_root(ir, f, &f->ins[ix])) { live_ins[ix] = 1; cgx_vi_push(&wl, ix); }
    }
    while (wl.n) {
        const int ix = wl.p[--wl.n]; const CgxIns *x = &f->ins[ix];
        for (int o = 0; o < cgx_ins_nop(x); o++) {
            const int u = cgx_ins_op(x, o); if (u <= 0 || u == CGX_NIL || live_val[u]) continue;
            live_val[u] = 1;
            const int d = f->val[u].def; if (d >= 0 && !live_ins[d] && S->bex[f->ins[d].blk]) { live_ins[d] = 1; cgx_vi_push(&wl, d); }
        }
    }
    cgx_vi_free(&wl);
}

/* ================= 6. relatorio ================= */
enum { FK_UNREACH, FK_DEADBRANCH, FK_UNUSEDVAR, FK_DEADSTORE, FK_USEBEFOREDEF, FK_MISSINGRET, FK_INFLOOP, FK_UNUSEDFN, FK_RECURSION, FK_REDUNDANT, FK_UNUSEDPARAM, FK_UNDEFNAME, FK_N };
static const char *const CGX_FK_NAME[FK_N] = {"unreachable", "dead-branch", "unused-var", "dead-store", "use-before-def", "missing-return", "infinite-loop", "unused-function", "recursion", "redundant-expr", "unused-param", "undefined-name"};
enum { CERT_STRUCT = 0, CERT_DERIVED = 1 };
typedef struct { int kind, line, cert, warn; char msg[240]; } CgxFind;
typedef struct {
    CgxFind *f; int n, cap; int count[FK_N];
    int nfn, nblk, nins, nphi, nloop, nunreach, npure, ntotal, nrec, nredundant;
    long visits_live, visits_reach, visits_avail, visits_da, sccp_evals;
    int nfold;                                            /* valores constantes achados pelo SCCP (temporarios e versoes) */
} CgxFlowReport;
static void cgx_report_free(CgxFlowReport *r) { free(r->f); memset(r, 0, sizeof *r); }
static void cgx_find(CgxFlowReport *r, int kind, int line, int cert, int warn, const char *fmt, ...) {
    r->f = (CgxFind *)cgx_xgrow(r->f, &r->cap, r->n + 1, sizeof(CgxFind));
    CgxFind *x = &r->f[r->n++]; x->kind = kind; x->line = line; x->cert = cert; x->warn = warn;
    va_list ap; va_start(ap, fmt); vsnprintf(x->msg, sizeof x->msg, fmt, ap); va_end(ap);
    r->count[kind]++;
}
static int cgx_find_cmp(const void *a, const void *b) {
    const CgxFind *x = (const CgxFind *)a, *y = (const CgxFind *)b;
    if (x->line != y->line) return x->line < y->line ? -1 : 1;
    if (x->kind != y->kind) return x->kind - y->kind;
    return strcmp(x->msg, y->msg);
}
/* o texto do relatorio: `[derived] line 12: mensagem` (certeza antes), uma por linha; info no fim de cada linha quando nao e aviso */
static void cgx_report_str(const CgxFlowReport *r, CgxB *o) {
    for (int i = 0; i < r->n; i++) cgx_bf(o, "[%s] line %d: %s%s\n", r->f[i].cert == CERT_STRUCT ? "structural" : "derived", r->f[i].line, r->f[i].msg, r->f[i].warn ? "" : " (info)");
}
static int cgx_report_warnings(const CgxFlowReport *r) { int c = 0; for (int i = 0; i < r->n; i++) c += r->f[i].warn; return c; }

/* ---- estado de uma funcao durante o relatorio ---- */
static int cgx_node_exec(const CgxFnIr *f, const CgxSccp *S, const CgxNode *s) {
    int seen = 0;
    for (int e = cgx_nm_first(f, s->id); e >= 0; e = cgx_nm_next(f, e)) if (f->nm[e].kind == NM_BLK) { seen = 1; if (S->bex[f->nm[e].x]) return 1; }
    return !seen;
}
/* o valor booleano constante de uma condicao (todas as ocorrencias concordam), ou -1 */
static int cgx_cond_const(const CgxFnIr *f, const CgxSccp *S, const CgxNode *c) {
    int res = -2;
    for (int e = cgx_nm_first(f, c->id); e >= 0; e = cgx_nm_next(f, e)) {
        const int v = cgx_nm_value(f, e); if (v < 0) continue;
        if (!S->bex[f->val[v].blk >= 0 ? f->val[v].blk : f->entry] && v != 0) continue;     /* ocorrencia inalcancavel: nao conta */
        const CgxCv *l = &S->lat[v]; const int c1 = l->k == CV_BOOL ? (int)l->iv : -1;
        if (c1 < 0) { return -1; }
        if (res == -2) res = c1; else if (res != c1) { return -1; }
    }
    return res == -2 ? -1 : res;
}
typedef struct { const CgxIr *ir; const CgxFnIr *f; const CgxSccp *S; CgxFlowReport *r; const char *fname; } CgxWalk;
static void cgx_walk_blk(CgxWalk *w, const CgxBlk *b);
static void cgx_walk_stmt(CgxWalk *w, const CgxNode *s) {
    switch (s->k) {
        case CGN_IF: {
            const int c = cgx_cond_const(w->f, w->S, s->a);
            if (c >= 0) {
                const int elif = s->flag;
                cgx_find(w->r, FK_DEADBRANCH, s->line, CERT_DERIVED, 1, "`%s` condition is always %s: the %s branch never runs (in %s)", elif ? "elif" : "if", c ? "true" : "false", c ? (s->b2.n ? "else" : "implicit else") : "then", w->fname);
                cgx_walk_blk(w, c ? &s->b1 : &s->b2);
            } else { cgx_walk_blk(w, &s->b1); cgx_walk_blk(w, &s->b2); }
            break;
        }
        case CGN_WHILE: {
            const int c = cgx_cond_const(w->f, w->S, s->a);
            if (c == 0) cgx_find(w->r, FK_DEADBRANCH, s->line, CERT_DERIVED, 1, "`while` condition is always false: the body never runs (in %s)", w->fname);
            else cgx_walk_blk(w, &s->b1);
            break;
        }
        case CGN_FOR: case CGN_EACH: cgx_walk_blk(w, &s->b1); break;
        case CGN_TRY: cgx_walk_blk(w, &s->b1); cgx_walk_blk(w, &s->b2); cgx_walk_blk(w, &s->b3); break;
        default: break;
    }
}
static void cgx_walk_blk(CgxWalk *w, const CgxBlk *b) {
    int reported = 0, prev = -1;
    for (int i = 0; i < b->n; i++) {
        const CgxNode *s = b->p[i];
        if (s->k == CGN_COMMENT || s->k == CGN_DOC) continue;
        if (!cgx_node_exec(w->f, w->S, s)) {
            if (!reported) {
                const int syn = prev == CGN_RET || prev == CGN_THROW || prev == CGN_BREAK || prev == CGN_CONT;
                cgx_find(w->r, FK_UNREACH, s->line, syn ? CERT_STRUCT : CERT_DERIVED, 1, "unreachable code%s (in %s)", syn ? " after return, throw, break or continue" : ": no path from the function entry reaches it", w->fname);
                reported = 1;
            }
            continue;
        }
        cgx_walk_stmt(w, s); prev = s->k;
    }
}

/* analise completa de uma funcao: SCCP, vivacidade, definicoes, atribuicao definitiva, expressoes disponiveis, e os achados */
typedef struct { CgxSccp S; char *live_val, *live_ins; } CgxFnFacts;
static void cgx_flow_fn(const CgxIr *ir, const CgxFnIr *f, CgxFlowReport *r, CgxFnFacts *keep) {
    CgxSccp S; cgx_sccp_run(ir, f, &S); r->sccp_evals += S.evals;
    char *lv = (char *)cgx_xcalloc((size_t)f->nval + 1, 1), *li = (char *)cgx_xcalloc((size_t)f->nins + 1, 1);
    cgx_live_values(ir, f, &S, lv, li);
    r->nblk += f->nbb; r->nins += f->nins; r->nphi += f->nphi; r->nloop += f->loops.nloop;
    for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach && !S.bex[b]) r->nunreach++;
    for (int v = 1; v < f->nval; v++) if (S.lat[v].k >= CV_INT) r->nfold++;
    char fname[96]; snprintf(fname, sizeof fname, "%s%s%s", f->cls ? f->cls->s : "", f->cls ? "." : "", f->name);
    CgxWalk w = {ir, f, &S, r, fname};
    cgx_walk_blk(&w, &f->fn->b1);
    /* atribuicao definitiva: usos antes de definir */
    { CgxW *in; int nw; long vis; cgx_defassign_run(f, &in, &nw, &vis); r->visits_da += vis;
      for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach && S.bex[b]) {
          CgxW *cur = (CgxW *)cgx_xcalloc((size_t)nw + 1, sizeof(CgxW)); memcpy(cur, in + (size_t)b * (size_t)nw, sizeof(CgxW) * (size_t)nw);
          for (int k = 0; k < f->bb[b].ins.n; k++) {
              const CgxIns *x = &f->ins[f->bb[b].ins.p[k]]; if (x->op == IO_PHI) continue;
              if (x->op == IO_UNDEF && (x->flags & IF_UNDEFNAME)) { cgx_find(r, FK_UNDEFNAME, x->line, CERT_STRUCT, 1, "name `%s` is not defined in this function (in %s)", x->node && x->node->s ? x->node->s : "?", fname); continue; }
              for (int o = 0; o < cgx_ins_nop(x); o++) {
                  const int v = cgx_opvar(f, cgx_ins_op(x, o));
                  if (v >= 0 && !cgx_bit_get(cur, v) && f->var[v].kind != VAR_HIDDEN) { cgx_find(r, FK_USEBEFOREDEF, x->line, CERT_DERIVED, 1, "variable `%s` may be used before it is assigned (in %s)", f->var[v].name, fname); cgx_bit_set(cur, v); }
              }
              const int d1 = cgx_opvar(f, x->dst), d2 = cgx_opvar(f, x->d2); if (d1 >= 0) cgx_bit_set(cur, d1); if (d2 >= 0) cgx_bit_set(cur, d2);
          }
          free(cur);
      }
      free(in); }
    { CgxReach rc; cgx_reach_run(f, &rc); r->visits_reach += rc.visits; cgx_reach_free(&rc, f->nvar); }
    { CgxAvail av; cgx_avail_run(f, &av); r->visits_avail += av.visits; r->nredundant += av.nredundant;
      for (int i = 0; i < av.red.n; i++) { const CgxIns *x = &f->ins[av.red.p[i]]; if (!S.bex[x->blk] || !x->node) continue;
          cgx_find(r, FK_REDUNDANT, x->line, CERT_DERIVED, 0, "`%s %s ...` was already computed on every path to here and its operands did not change; keep the first result in a variable (in %s)", x->op == IO_BIN ? "operation" : "operation", CGO_SYM[x->sub], fname); }
      cgx_avail_free(&av); }
    /* variaveis: nunca usadas, e atribuicoes mortas (uma passada pelos valores e outra pelas instrucoes, nao uma por variavel) */
    {
        char *anylive = (char *)cgx_xcalloc((size_t)f->nvar + 1, 1), *anyread = (char *)cgx_xcalloc((size_t)f->nvar + 1, 1); int *ndef = (int *)cgx_xcalloc((size_t)f->nvar + 1, sizeof(int));
        for (int i = 1; i < f->nval; i++) { const int v = f->val[i].var; const int d = f->val[i].def; if (v < 0 || d < 0 || !S.bex[f->ins[d].blk]) continue; ndef[v]++; if (lv[i]) anylive[v] = 1; }
        for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach && S.bex[b]) for (int k = 0; k < f->bb[b].ins.n; k++) {
            const CgxIns *x = &f->ins[f->bb[b].ins.p[k]]; if (x->op == IO_PHI) continue;
            for (int o = 0; o < cgx_ins_nop(x); o++) { const int u = cgx_ins_op(x, o); if (u > 0 && u != CGX_NIL && f->val[u].var >= 0) anyread[f->val[u].var] = 1; }
        }
        for (int v = 0; v < f->nvar; v++) {
            const CgxVar *vr = &f->var[v];
            if (vr->kind != VAR_LET && vr->kind != VAR_PARAM) continue;
            int grouped_dup = 0;
            /* a mesma declaracao copiada (finally): so uma vez */
            for (int u = 0; u < v; u++) if (f->var[u].decl == vr->decl && f->var[u].kind == vr->kind && f->var[u].sid == vr->sid && vr->decl && vr->kind == VAR_LET) grouped_dup = 1;
            if (vr->kind == VAR_PARAM) {
                if (!anylive[v] && ndef[v] && f->fn->b1.n && !f->has_raw && !grouped_dup) cgx_find(r, FK_UNUSEDPARAM, vr->line, CERT_STRUCT, 0, "parameter `%s` is never used (in %s)", vr->name, fname);
                continue;
            }
            if (grouped_dup) continue;
            if (!anylive[v] && !ndef[v]) continue;                                  /* so existe em codigo inalcancavel: o achado de inalcancavel ja cobre */
            if (!anylive[v]) { if (anyread[v]) cgx_find(r, FK_UNUSEDVAR, vr->line, CERT_DERIVED, 1, "variable `%s` is assigned but its value is never used (in %s)", vr->name, fname); else cgx_find(r, FK_UNUSEDVAR, vr->line, CERT_STRUCT, 1, "variable `%s` is never read (in %s)", vr->name, fname); continue; }
        }
        for (int i = 1; i < f->nval; i++) {                                           /* variavel usada, mas esta atribuicao nao */
            const int v = f->val[i].var; if (v < 0 || lv[i] || !anylive[v] || f->var[v].kind != VAR_LET) continue;
            const int d = f->val[i].def; if (d < 0 || !S.bex[f->ins[d].blk]) continue;
            const CgxIns *x = &f->ins[d]; if (!x->node || (x->node->k != CGN_LET && x->node->k != CGN_ASSIGN)) continue;
            cgx_find(r, FK_DEADSTORE, x->line, CERT_DERIVED, 1, "the value assigned to `%s` here is never read (it is overwritten, or the paths that read it do not run) (in %s)", f->var[v].name, fname);
        }
        free(anylive); free(anyread); free(ndef);
    }
    /* retorno que falta, e lacos sem saida */
    for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach && S.bex[b]) for (int k = 0; k < f->bb[b].ins.n; k++) {
        const CgxIns *x = &f->ins[f->bb[b].ins.p[k]];
        if (x->op == IO_RET && (x->flags & IF_IMPLICIT) && f->fn->ty.k != CGT_VOID && !(f->cls && !strcmp(f->fn->s, "init")) && f->fn->has_ty)
            cgx_find(r, FK_MISSINGRET, f->fn->line, CERT_DERIVED, 1, "`%s` can reach its end without `return` on some path (declared `-> %s`)", fname, f->fn->ty.k == CGT_INT ? "int" : f->fn->ty.k == CGT_FLOAT ? "float" : f->fn->ty.k == CGT_STR ? "str" : f->fn->ty.k == CGT_BOOL ? "bool" : "a value");
    }
    if (f->loops.nloop) {
        const CgxDg *g = &f->g; char *canexit = (char *)cgx_xcalloc((size_t)f->nbb + 1, 1); CgxVi wl = {0};
        for (int b = 0; b < f->nbb; b++) if (f->bb[b].reach && S.bex[b]) { const CgxIns *t = &f->ins[f->bb[b].ins.p[f->bb[b].ins.n - 1]]; if (t->op == IO_RET || (t->op == IO_THROW && g->sp[b + 1] == g->sp[b])) { canexit[b] = 1; cgx_vi_push(&wl, b); } }
        while (wl.n) { const int b = wl.p[--wl.n]; for (int k = g->pp[b]; k < g->pp[b + 1]; k++) { const int p = g->p[k]; const int e = g->pe[k]; if (!canexit[p] && S.bex[p] && S.eex[e]) { canexit[p] = 1; cgx_vi_push(&wl, p); } } }
        char *bad = (char *)cgx_xcalloc((size_t)f->loops.nloop + 1, 1);
        for (int i = 0; i < f->loops.nloop; i++) { const int h = f->loops.head[i]; bad[i] = S.bex[h] && !canexit[h]; }
        for (int i = 0; i < f->loops.nloop; i++) if (bad[i]) {
            int inner = 0; for (int j = 0; j < f->loops.nloop; j++) if (j != i && bad[j] && cgx_bit_get(f->loops.body[i], f->loops.head[j]) && !cgx_bit_get(f->loops.body[j], f->loops.head[i])) inner = 1;
            if (inner) continue;
            const int h = f->loops.head[i]; int lit = 0, line = f->bb[h].line;
            for (int k = 0; k < f->bb[h].ins.n; k++) { const CgxIns *x = &f->ins[f->bb[h].ins.p[k]]; if (x->op == IO_BR && (x->flags & IF_LOOPCOND) && x->node && x->node->a) { lit = x->node->a->k == CGN_BOOL && x->node->a->iv; line = x->node->line; } }
            cgx_find(r, FK_INFLOOP, line, lit ? CERT_STRUCT : CERT_DERIVED, 1, "this loop has no way out (no reachable `break`, `return` or `throw`) (in %s)", fname);
        }
        free(canexit); free(bad); cgx_vi_free(&wl);
    }
    if (keep) { keep->S = S; keep->live_val = lv; keep->live_ins = li; }
    else { cgx_sccp_free(&S); free(lv); free(li); }
}
static void cgx_mark_called(const CgxIr *ir, const CgxNode *n, char *called) {
    if (!n) return;
    cgx_mark_called(ir, n->a, called); cgx_mark_called(ir, n->b, called); cgx_mark_called(ir, n->c, called);
    for (int i = 0; i < n->b1.n; i++) cgx_mark_called(ir, n->b1.p[i], called);
    if (n->k == CGN_CALL || n->k == CGN_MCALL) { const CgxNode *c = cgx_callee_of(ir, n); if (c && c->id > 0 && c->id < ir->nid && ir->fnid[c->id] >= 0) called[ir->fnid[c->id]] = 1; }
}
/* ordem de geracao: quem e chamado antes de quem chama (ordem topologica reversa das componentes de Tarjan); desempate pela ordem do fonte */
static int cgx_cg_order(const CgxIr *ir, int *order) {
    int n = 0;
    for (int c = 0; c < ir->ncomp; c++) for (int i = 0; i < ir->nfn; i++) if (ir->cgcomp[i] == c) order[n++] = i;
    return n;
}
/* roda todas as analises e junta o relatorio (ordenado por linha) */
static int cgx_flow_run(CgxIr *ir, CgxFlowReport *r) {
    memset(r, 0, sizeof *r);
    for (int i = 0; i < ir->nfn; i++) { cgx_flow_fn(ir, &ir->fn[i], r, NULL); r->nfn++; if (cgx_fx_removable(ir->fx[i])) r->npure++; if (!ir->fx[i]) r->ntotal++; }
    char *called = (char *)cgx_xcalloc((size_t)ir->nfn + 1, 1);
    for (int i = 0; i < ir->nfn; i++) for (int k = ir->cg.sp[i]; k < ir->cg.sp[i + 1]; k++) if (ir->cg.s[k] != i) called[ir->cg.s[k]] = 1;
    if (ir->g->prog) for (int i = 0; i < ir->g->prog->b1.n; i++) { const CgxNode *d = ir->g->prog->b1.p[i]; if (d->k == CGN_TEST) { cgx_mark_called(ir, d->a, called); cgx_mark_called(ir, d->b, called); } else if (d->k == CGN_LET) cgx_mark_called(ir, d->a, called); }
    for (int i = 0; i < ir->nfn; i++) if (!ir->fn[i].cls && !called[i]) cgx_find(r, FK_UNUSEDFN, ir->fn[i].fn->line, CERT_DERIVED, 0, "function `%s` is never called in this spec (fine if it is an entry point)", ir->fn[i].name);
    int *csz = (int *)cgx_xcalloc((size_t)ir->ncomp + 1, sizeof(int)); for (int i = 0; i < ir->nfn; i++) csz[ir->cgcomp[i]]++;
    for (int c = 0; c < ir->ncomp; c++) {
        int first = -1; for (int i = 0; i < ir->nfn; i++) if (ir->cgcomp[i] == c) { first = i; break; }
        if (first < 0) continue;
        if (csz[c] > 1) {
            char names[200]; size_t w = 0; names[0] = 0;
            for (int i = 0; i < ir->nfn && w + 40 < sizeof names; i++) if (ir->cgcomp[i] == c) w += (size_t)snprintf(names + w, sizeof names - w, "%s%s", w ? ", " : "", ir->fn[i].name);
            cgx_find(r, FK_RECURSION, ir->fn[first].fn->line, CERT_STRUCT, 0, "mutual recursion between %s", names); r->nrec++;
        } else if (ir->selfrec[first]) { cgx_find(r, FK_RECURSION, ir->fn[first].fn->line, CERT_STRUCT, 0, "`%s` calls itself (direct recursion)", ir->fn[first].name); r->nrec++; }
    }
    free(csz); free(called);
    if (r->n > 1) qsort(r->f, (size_t)r->n, sizeof(CgxFind), cgx_find_cmp);
    return r->n;
}
