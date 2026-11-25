/* ---------- coder/ucgeopt.c: UCGE, otimizador sobre a UIR-H guiado pelas analises da UIR-G (docs/ucge.md, secao 6.7) ----------
 * As analises (SCCP, vivacidade, dominancia) rodam no grafo; as reescritas voltam para a AST, que continua sendo o que os backends leem. Por
 * isso os nomes das variaveis do autor sobrevivem e o codigo gerado fica legivel. Passes (cada um liga e desliga em CgxOptOpts.passes):
 *   FOLD      constantes: dobra expressoes e propaga constantes e desvios decididos pelo SCCP (if/while de condicao constante);
 *   ALGEBRA   simplificacao algebrica so onde e identica no Python (x+0, x*1, x//1, x-x, not not x, ...) e no tipo certo (int; float so x*1);
 *   COPY      propagacao de copias (`let b = a` ... uso de b vira a, se `a` nao mudou no caminho);
 *   DCE       codigo morto: atribuicao cujo valor ninguem le (marcacao a partir das instrucoes com efeito), expressao-instrucao pura;
 *   UNREACH   instrucoes inalcancaveis (depois de return/throw/break/continue, depois de laco sem saida, ramos mortos);
 *   TEMPS     remocao de temporarios: `let t = e` seguido do unico uso de t vira o proprio e;
 *   INLINE    expansao de funcoes pequenas, nao recursivas, de um `return expr` so (opt-in: troca legibilidade por menos chamadas);
 *   GLOBALS   constantes de modulo com literal entram na propagacao (opt-in: o nome da constante some do codigo).
 * Regra de ouro: nada que mude o comportamento observavel do Python gerado, incluindo o erro lancado (divisao por zero, indice fora, tipo),
 * a ordem dos efeitos, os inteiros grandes (a dobra recusa estouro de 64 bits) e NaN/-0.0 (floats so se simplificam onde a identidade vale). */
enum { CGX_O_FOLD = 1, CGX_O_ALGEBRA = 2, CGX_O_COPY = 4, CGX_O_DCE = 8, CGX_O_UNREACH = 16, CGX_O_TEMPS = 32, CGX_O_INLINE = 64, CGX_O_GLOBALS = 128,
       CGX_O_ALL = 255, CGX_O_DEFAULT = CGX_O_FOLD | CGX_O_ALGEBRA | CGX_O_COPY | CGX_O_DCE | CGX_O_UNREACH | CGX_O_TEMPS };
typedef struct { int passes, max_rounds, inline_max; } CgxOptOpts;
typedef struct { int rounds, folded, propagated, copies, algebra, dead, unreachable, branches, loops, temps, inlined; } CgxOptReport;

/* ================= 1. utilitarios de AST ================= */
static CgxNode *cgx_clone(Cgx *g, const CgxNode *n) {
    if (!n) return NULL;
    CgxNode *c = (CgxNode *)cgx_alloc(g, sizeof *c); *c = *n;
    c->a = cgx_clone(g, n->a); c->b = cgx_clone(g, n->b); c->c = cgx_clone(g, n->c);
    CgxBlk *src[3] = {(CgxBlk *)&n->b1, (CgxBlk *)&n->b2, (CgxBlk *)&n->b3}, *dst[3] = {&c->b1, &c->b2, &c->b3};
    for (int k = 0; k < 3; k++) { dst[k]->p = NULL; dst[k]->n = dst[k]->cap = 0; for (int i = 0; i < src[k]->n; i++) cgx_blk_add(g, dst[k], cgx_clone(g, src[k]->p[i])); }
    return c;
}
static int cgx_expr_size(const CgxNode *n) {
    if (!n) return 0;
    int s = 1 + cgx_expr_size(n->a) + cgx_expr_size(n->b) + cgx_expr_size(n->c);
    for (int i = 0; i < n->b1.n; i++) s += cgx_expr_size(n->b1.p[i]);
    return s;
}
static int cgx_count_var(const CgxNode *n, const char *name) {
    if (!n) return 0;
    int c = n->k == CGN_VAR && !strcmp(n->s, name);
    c += cgx_count_var(n->a, name) + cgx_count_var(n->b, name) + cgx_count_var(n->c, name);
    for (int i = 0; i < n->b1.n; i++) c += cgx_count_var(n->b1.p[i], name);
    return c;
}
static int cgx_is_lit(const CgxNode *n) { return n->k == CGN_INT || n->k == CGN_FLT || n->k == CGN_STR || n->k == CGN_BOOL || n->k == CGN_NULL; }
static void cgx_become(CgxNode *dst, const CgxNode *src) { const int line = dst->line; *dst = *src; dst->line = line; }

/* ================= 2. simplificacao algebrica ================= */
static int cgx_lit_is(const CgxNode *n, long long v) { return n->k == CGN_INT && n->iv == v; }
static int cgx_flt_is(const CgxNode *n, double v) { return n->k == CGN_FLT && n->dv == v && !signbit(n->dv) == !signbit(v); }
/* tenta simplificar o no n (filhos ja simplificados); devolve 1 se mudou. fxc: efeitos conhecidos (para saber se um operando pode sumir) */
static int cgx_alg_node(const CgxIr *ir, Cgx *g, CgxNode *n) {
    if (n->k == CGN_BIN) {
        CgxNode *a = n->a, *b = n->b; const int ai = a->ty.k == CGT_INT, bi = b->ty.k == CGT_INT, af = a->ty.k == CGT_FLOAT;
        const int arem = cgx_fx_removable(cgx_fx_expr(ir, a)), brem = cgx_fx_removable(cgx_fx_expr(ir, b));
        switch (n->op) {
            case CGO_ADD:
                if (ai && bi && cgx_lit_is(b, 0)) { cgx_become(n, a); return 1; }
                if (ai && bi && cgx_lit_is(a, 0)) { cgx_become(n, b); return 1; }
                if (a->ty.k == CGT_STR && b->ty.k == CGT_STR && b->k == CGN_STR && !b->s[0]) { cgx_become(n, a); return 1; }
                if (a->ty.k == CGT_STR && b->ty.k == CGT_STR && a->k == CGN_STR && !a->s[0]) { cgx_become(n, b); return 1; }
                break;
            case CGO_SUB:
                if (ai && bi && cgx_lit_is(b, 0)) { cgx_become(n, a); return 1; }
                if (ai && bi && a->k == CGN_VAR && b->k == CGN_VAR && !strcmp(a->s, b->s)) { CgxCv z = {CV_INT, 0, 0, NULL}; CgxNode *l = cgx_cv_node(g, &z, n->line); cgx_become(n, l); return 1; }
                break;
            case CGO_MUL:
                if (ai && bi && cgx_lit_is(b, 1)) { cgx_become(n, a); return 1; }
                if (ai && bi && cgx_lit_is(a, 1)) { cgx_become(n, b); return 1; }
                if (af && b->ty.k == CGT_FLOAT && cgx_flt_is(b, 1.0)) { cgx_become(n, a); return 1; }
                if (ai && bi && cgx_lit_is(b, 0) && arem) { CgxCv z = {CV_INT, 0, 0, NULL}; CgxNode *l = cgx_cv_node(g, &z, n->line); cgx_become(n, l); return 1; }
                if (ai && bi && cgx_lit_is(a, 0) && brem) { CgxCv z = {CV_INT, 0, 0, NULL}; CgxNode *l = cgx_cv_node(g, &z, n->line); cgx_become(n, l); return 1; }
                break;
            case CGO_IDIV: if (ai && bi && cgx_lit_is(b, 1)) { cgx_become(n, a); return 1; } break;
            case CGO_MOD: if (ai && bi && cgx_lit_is(b, 1) && arem) { CgxCv z = {CV_INT, 0, 0, NULL}; CgxNode *l = cgx_cv_node(g, &z, n->line); cgx_become(n, l); return 1; } break;
            case CGO_EQ: case CGO_NE: case CGO_LT: case CGO_LE: case CGO_GT: case CGO_GE:
                if (a->k == CGN_VAR && b->k == CGN_VAR && !strcmp(a->s, b->s) && (a->ty.k == CGT_INT || a->ty.k == CGT_STR || a->ty.k == CGT_BOOL) && (a->ty.k != CGT_BOOL || n->op == CGO_EQ || n->op == CGO_NE)) {
                    CgxCv t = {CV_BOOL, n->op == CGO_EQ || n->op == CGO_LE || n->op == CGO_GE, 0, NULL}; CgxNode *l = cgx_cv_node(g, &t, n->line); cgx_become(n, l); return 1;
                }
                break;
            case CGO_AND:
                if (a->k == CGN_BOOL) { if (a->iv) { if (b->ty.k == CGT_BOOL) { cgx_become(n, b); return 1; } } else { CgxCv t = {CV_BOOL, 0, 0, NULL}; CgxNode *l = cgx_cv_node(g, &t, n->line); cgx_become(n, l); return 1; } }
                if (b->k == CGN_BOOL && b->iv && a->ty.k == CGT_BOOL) { cgx_become(n, a); return 1; }
                if (b->k == CGN_BOOL && !b->iv && arem) { CgxCv t = {CV_BOOL, 0, 0, NULL}; CgxNode *l = cgx_cv_node(g, &t, n->line); cgx_become(n, l); return 1; }
                break;
            case CGO_OR:
                if (a->k == CGN_BOOL) { if (!a->iv) { if (b->ty.k == CGT_BOOL) { cgx_become(n, b); return 1; } } else { CgxCv t = {CV_BOOL, 1, 0, NULL}; CgxNode *l = cgx_cv_node(g, &t, n->line); cgx_become(n, l); return 1; } }
                if (b->k == CGN_BOOL && !b->iv && a->ty.k == CGT_BOOL) { cgx_become(n, a); return 1; }
                if (b->k == CGN_BOOL && b->iv && arem) { CgxCv t = {CV_BOOL, 1, 0, NULL}; CgxNode *l = cgx_cv_node(g, &t, n->line); cgx_become(n, l); return 1; }
                break;
        }
    } else if (n->k == CGN_UN) {
        CgxNode *a = n->a;
        if (n->op == CGO_NOT && a->k == CGN_UN && a->op == CGO_NOT && a->a->ty.k == CGT_BOOL) { cgx_become(n, a->a); return 1; }
        if (n->op == CGO_NOT && a->k == CGN_BIN && (a->op == CGO_EQ || a->op == CGO_NE)) { n->k = CGN_BIN; n->op = a->op == CGO_EQ ? CGO_NE : CGO_EQ; n->b = a->b; n->a = a->a; return 1; }
        if (n->op == CGO_NEG && a->k == CGN_UN && a->op == CGO_NEG && (a->a->ty.k == CGT_INT || a->a->ty.k == CGT_FLOAT)) { cgx_become(n, a->a); return 1; }
    } else if (n->k == CGN_TERN && n->a->k == CGN_BOOL) {
        cgx_become(n, n->a->iv ? n->b : n->c); return 1;
    }
    return 0;
}
static int cgx_alg_expr(const CgxIr *ir, Cgx *g, CgxNode *n) {
    if (!n) return 0;
    int ch = cgx_alg_expr(ir, g, n->a) + cgx_alg_expr(ir, g, n->b) + cgx_alg_expr(ir, g, n->c);
    for (int i = 0; i < n->b1.n; i++) ch += cgx_alg_expr(ir, g, n->b1.p[i]);
    if (n->k == CGN_BIN || n->k == CGN_UN || n->k == CGN_TERN) while (cgx_alg_node(ir, g, n)) ch++;
    return ch;
}
static int cgx_alg_blk(const CgxIr *ir, Cgx *g, CgxBlk *b) {
    int ch = 0;
    for (int i = 0; i < b->n; i++) {
        CgxNode *s = b->p[i];
        switch (s->k) {
            case CGN_LET: case CGN_RET: case CGN_THROW: case CGN_EXPR: ch += cgx_alg_expr(ir, g, s->a); break;
            case CGN_ASSIGN: if (s->a->k != CGN_VAR) { ch += cgx_alg_expr(ir, g, s->a->a); ch += cgx_alg_expr(ir, g, s->a->b); } ch += cgx_alg_expr(ir, g, s->b); break;
            case CGN_IF: case CGN_WHILE: ch += cgx_alg_expr(ir, g, s->a); break;
            case CGN_FOR: ch += cgx_alg_expr(ir, g, s->a) + cgx_alg_expr(ir, g, s->b); break;
            case CGN_EACH: ch += cgx_alg_expr(ir, g, s->a); break;
        }
        ch += cgx_alg_blk(ir, g, &s->b1) + cgx_alg_blk(ir, g, &s->b2) + cgx_alg_blk(ir, g, &s->b3);
    }
    return ch;
}

/* ================= 3. reescrita guiada pela SSA e pelo SCCP ================= */
typedef struct { CgxNode **p; int n, cap; } CgxNv;
static void cgx_nv_push(CgxNv *v, CgxNode *x) { v->p = (CgxNode **)cgx_xgrow(v->p, &v->cap, v->n + 1, sizeof(CgxNode *)); v->p[v->n++] = x; }
typedef struct { Cgx *g; CgxIr *ir; const CgxFnIr *f; CgxSccp S; char *lv, *li; int passes; CgxOptReport *rep; int changed; } CgxRw;

/* o valor de uma ocorrencia (entrada do mapa no -> valor) esta num bloco executavel? */
static int cgx_occ_exec(const CgxRw *R, int e) {
    const CgxNm *m = &R->f->nm[e];
    if (m->kind == NM_OP) return R->S.bex[R->f->ins[m->x].blk];
    if (m->kind == NM_VAL) { const int b = R->f->val[m->x].blk; return b < 0 || R->S.bex[b]; }
    return 1;
}
/* o no vale sempre a mesma constante (todas as ocorrencias lowered concordam)? */
static int rw_const(const CgxRw *R, const CgxNode *n, CgxCv *out) {
    int found = 0;
    for (int e = cgx_nm_first(R->f, n->id); e >= 0; e = cgx_nm_next(R->f, e)) {
        const int v = cgx_nm_value(R->f, e); if (v < 0) continue;
        if (!cgx_occ_exec(R, e)) continue;
        if (v == 0) return 0;
        const CgxCv *l = &R->S.lat[v]; if (l->k < CV_INT) return 0;
        if (!found) { *out = *l; found = 1; } else if (!cgx_cv_same(out, l)) return 0;
    }
    return found;
}
static int cgx_ins_pos(const CgxFnIr *f, int ix) { const CgxBB *b = &f->bb[f->ins[ix].blk]; for (int k = 0; k < b->ins.n; k++) if (b->ins.p[k] == ix) return k; return -1; }
/* a versao de `var` que alcanca o ponto (bloco, posicao): a definicao mais proxima que domina o ponto */
static int cgx_reaching_version(const CgxFnIr *f, int var, int blk, int pos) {
    for (;;) {
        const CgxBB *b = &f->bb[blk];
        for (int k = pos - 1; k >= 0; k--) {
            const CgxIns *x = &f->ins[b->ins.p[k]];
            if (x->dst != CGX_NIL && x->dst > 0 && f->val[x->dst].var == var) return x->dst;
            if (x->d2 != CGX_NIL && x->d2 > 0 && f->val[x->d2].var == var) return x->d2;
        }
        if (f->dom.idom[blk] == blk || f->dom.idom[blk] < 0) return 0;
        blk = f->dom.idom[blk]; pos = f->bb[blk].ins.n;
    }
}
static int cgx_copy_root(const CgxFnIr *f, int v) {
    for (int guard = 0; guard < 64 && v > 0 && f->val[v].var >= 0; guard++) {
        const int d = f->val[v].def; if (d < 0 || f->ins[d].op != IO_COPY) break;
        const int a = f->ins[d].a; if (a <= 0 || a == CGX_NIL || f->val[a].var < 0) break;
        v = a;
    }
    return v;
}
/* VAR -> outra VAR (propagacao de copia) quando todas as ocorrencias concordam e a fonte nao mudou ate o uso */
static int rw_copyprop(CgxRw *R, CgxNode *n) {
    const CgxFnIr *f = R->f; int rvar = -1, any = 0;
    for (int e = cgx_nm_first(f, n->id); e >= 0; e = cgx_nm_next(f, e)) {
        const CgxNm *m = &f->nm[e]; if (m->kind != NM_OP) continue;
        if (!cgx_occ_exec(R, e)) continue;
        const int v = cgx_nm_value(f, e); if (v <= 0 || f->val[v].var < 0) return 0;
        const int root = cgx_copy_root(f, v); if (root == v) return 0;
        const int rv = f->val[root].var; if (rv == f->val[v].var || f->var[rv].kind == VAR_HIDDEN || f->var[rv].kind == VAR_RAW) return 0;
        if (cgx_reaching_version(f, rv, f->ins[m->x].blk, cgx_ins_pos(f, m->x)) != root) return 0;
        if (rvar >= 0 && rvar != rv) return 0;
        rvar = rv; any = 1;
    }
    if (!any) return 0;
    const CgxVar *src = &f->var[rvar];
    n->s = (char *)src->name; n->sid = src->sid; n->ref = src->decl;
    R->rep->copies++; R->changed++; return 1;
}
static void rw_expr(CgxRw *R, CgxNode *n) {
    if (!n || cgx_is_lit(n)) return;
    if ((R->passes & CGX_O_FOLD) && (n->k == CGN_VAR || n->k == CGN_BIN || n->k == CGN_UN || n->k == CGN_TERN || (n->k == CGN_CALL && n->bi))) {
        CgxCv cv;
        if (rw_const(R, n, &cv) && cgx_fx_removable(cgx_fx_expr(R->ir, n))) {
            CgxNode *l = cgx_cv_node(R->g, &cv, n->line);
            if (l) { if (n->k == CGN_VAR) R->rep->propagated++; else R->rep->folded++; R->changed++; cgx_become(n, l); return; }
        }
    }
    if ((R->passes & CGX_O_COPY) && n->k == CGN_VAR && rw_copyprop(R, n)) return;
    rw_expr(R, n->a); rw_expr(R, n->b); rw_expr(R, n->c);
    for (int i = 0; i < n->b1.n; i++) rw_expr(R, n->b1.p[i]);
}

static int cgx_stmts_declare(CgxNode *const *p, int n, const char *name) {
    for (int i = 0; i < n; i++) {
        const CgxNode *s = p[i];
        if ((s->k == CGN_LET || s->k == CGN_FOR || s->k == CGN_EACH) && s->s && !strcmp(s->s, name)) return 1;
        if (s->k == CGN_EACH && s->s2 && !strcmp(s->s2, name)) return 1;
        if (s->k == CGN_TRY && s->s2 && !strcmp(s->s2, name)) return 1;
        if (cgx_stmts_declare(s->b1.p, s->b1.n, name) || cgx_stmts_declare(s->b2.p, s->b2.n, name) || cgx_stmts_declare(s->b3.p, s->b3.n, name)) return 1;
    }
    return 0;
}
static int rw_def_dead(const CgxRw *R, const CgxNode *s) {                  /* todas as copias da instrucao de definicao estao mortas */
    int n = 0;
    for (int e = cgx_nm_first(R->f, s->id); e >= 0; e = cgx_nm_next(R->f, e)) if (R->f->nm[e].kind == NM_DEF) {
        const int ix = R->f->nm[e].x; if (!R->S.bex[R->f->ins[ix].blk]) continue;
        const int d = R->f->ins[ix].dst; if (d > 0 && R->lv[d]) return 0;             /* o valor definido e lido por algo vivo */
        n++;
    }
    return n > 0;
}
/* quantas instrucoes (alcancaveis, fora phi) escrevem a variavel que a instrucao `ix` define: se ha outras, a declaracao `let` nao pode sumir */
static int rw_ndefs(const CgxRw *R, int ix) {
    const int v = R->f->val[R->f->ins[ix].dst].var; int n = 0;
    for (int i = 0; i < R->f->nins; i++) { const CgxIns *x = &R->f->ins[i]; if (x->op == IO_PHI || !R->S.bex[x->blk]) continue;
        if ((x->dst > 0 && R->f->val[x->dst].var == v) || (x->d2 > 0 && x->d2 != CGX_NIL && R->f->val[x->d2].var == v)) n++; }
    return n;
}
static int rw_single_def(const CgxRw *R, const CgxNode *s) {
    for (int e = cgx_nm_first(R->f, s->id); e >= 0; e = cgx_nm_next(R->f, e)) if (R->f->nm[e].kind == NM_DEF && R->S.bex[R->f->ins[R->f->nm[e].x].blk] && rw_ndefs(R, R->f->nm[e].x) > 1) return 0;
    return 1;
}
static void rw_blk(CgxRw *R, CgxBlk *b);
static void rw_collect(CgxRw *R, CgxNode *const *src, int n, CgxNv *out);
static void rw_stmt(CgxRw *R, CgxNode *s, CgxNode *const *rest, int nrest, CgxNv *out) {
    if (s->k == CGN_COMMENT || s->k == CGN_DOC) { cgx_nv_push(out, s); return; }
    if ((R->passes & CGX_O_UNREACH) && !cgx_node_exec(R->f, &R->S, s)) { R->rep->unreachable++; R->changed++; return; }
    switch (s->k) {
        case CGN_LET: {
            rw_expr(R, s->a);
            if ((R->passes & CGX_O_DCE) && rw_def_dead(R, s)) {
                const int single = rw_single_def(R, s);
                if (cgx_fx_removable(cgx_fx_expr(R->ir, s->a))) {
                    if (single) { R->rep->dead++; R->changed++; return; }
                    /* a variavel e escrita depois (sem let): a declaracao fica, com um valor barato no lugar do calculo morto */
                    if (!cgx_is_lit(s->a) && (s->ty.k == CGT_INT || s->ty.k == CGT_FLOAT || s->ty.k == CGT_BOOL || s->ty.k == CGT_STR)) {
                        CgxCv z; memset(&z, 0, sizeof z); z.k = s->ty.k == CGT_INT ? CV_INT : s->ty.k == CGT_FLOAT ? CV_FLT : s->ty.k == CGT_BOOL ? CV_BOOL : CV_STR; z.s = "";
                        CgxNode *l = cgx_cv_node(R->g, &z, s->line); if (l) { s->a = l; R->rep->dead++; R->changed++; }
                    }
                } else if (single && (s->a->k == CGN_CALL || s->a->k == CGN_MCALL)) { s->k = CGN_EXPR; s->s = NULL; s->has_ty = 0; R->rep->dead++; R->changed++; }
            }
            break;
        }
        case CGN_ASSIGN:
            if (s->a->k != CGN_VAR) { rw_expr(R, s->a->a); rw_expr(R, s->a->b); }
            rw_expr(R, s->b);
            if (s->a->k == CGN_VAR && (R->passes & CGX_O_DCE) && rw_def_dead(R, s) && cgx_fx_removable(cgx_fx_expr(R->ir, s->b))) { R->rep->dead++; R->changed++; return; }
            break;
        case CGN_RET: case CGN_THROW: rw_expr(R, s->a); break;
        case CGN_EXPR:
            for (int i = 0; i < s->a->b1.n; i++) rw_expr(R, s->a->b1.p[i]);
            if (s->a->k == CGN_MCALL) rw_expr(R, s->a->a);
            if ((R->passes & CGX_O_DCE) && cgx_fx_removable(cgx_fx_expr(R->ir, s->a))) { R->rep->dead++; R->changed++; return; }
            break;
        case CGN_IF: {
            const int c = (R->passes & CGX_O_FOLD) ? cgx_cond_const(R->f, &R->S, s->a) : -1;
            if (c >= 0 && cgx_fx_removable(cgx_fx_expr(R->ir, s->a))) {
                CgxBlk *taken = c ? &s->b1 : &s->b2; int conflict = 0;
                for (int i = 0; i < taken->n && !conflict; i++) { const CgxNode *t = taken->p[i]; if (t->k == CGN_LET && cgx_stmts_declare(rest, nrest, t->s)) conflict = 1; }
                if (!conflict) {
                    const int n0 = out->n; rw_collect(R, taken->p, taken->n, out);
                    for (int i = n0; i < out->n; i++) if (out->p[i]->k == CGN_IF) out->p[i]->flag = 0;      /* um elif solto vira if */
                    R->rep->branches++; R->changed++; return;
                }
                CgxBlk *dead = c ? &s->b2 : &s->b1; if (dead->n) { dead->n = 0; R->changed++; }       /* mantem o if, sem o ramo morto */
            }
            rw_expr(R, s->a); rw_blk(R, &s->b1); rw_blk(R, &s->b2);
            if ((R->passes & CGX_O_DCE) && !s->b1.n && !s->b2.n && cgx_fx_removable(cgx_fx_expr(R->ir, s->a))) { R->rep->dead++; R->changed++; return; }
            cgx_nv_push(out, s); return;
        }
        case CGN_WHILE: {
            const int c = (R->passes & CGX_O_FOLD) ? cgx_cond_const(R->f, &R->S, s->a) : -1;
            if (c == 0 && cgx_fx_removable(cgx_fx_expr(R->ir, s->a))) { R->rep->loops++; R->changed++; return; }
            rw_expr(R, s->a); rw_blk(R, &s->b1); break;
        }
        case CGN_FOR: rw_expr(R, s->a); rw_expr(R, s->b); rw_blk(R, &s->b1); break;
        case CGN_EACH: rw_expr(R, s->a); rw_blk(R, &s->b1); break;
        case CGN_TRY: rw_blk(R, &s->b1); rw_blk(R, &s->b2); rw_blk(R, &s->b3); break;
        default: break;
    }
    cgx_nv_push(out, s);
}
/* temporarios: `let t = e` + proximo comando que usa t uma vez (e so ali) -> o comando recebe e no lugar de t */
static CgxNode *rw_find_var(CgxNode *n, const char *name) {
    if (!n) return NULL;
    if (n->k == CGN_VAR && !strcmp(n->s, name)) return n;
    CgxNode *r = rw_find_var(n->a, name); if (r) return r;
    r = rw_find_var(n->b, name); if (r) return r;
    r = rw_find_var(n->c, name); if (r) return r;
    for (int i = 0; i < n->b1.n; i++) { r = rw_find_var(n->b1.p[i], name); if (r) return r; }
    return NULL;
}
static int cgx_count_var_blk(const CgxBlk *b, const char *name) {
    int c = 0;
    for (int i = 0; i < b->n; i++) {
        const CgxNode *s = b->p[i];
        c += cgx_count_var(s->a, name) + cgx_count_var(s->b, name) + cgx_count_var(s->c, name);
        c += cgx_count_var_blk(&s->b1, name) + cgx_count_var_blk(&s->b2, name) + cgx_count_var_blk(&s->b3, name);
        if (s->k == CGN_EACH || s->k == CGN_FOR) c += s->s && !strcmp(s->s, name);
    }
    return c;
}
static int rw_temps(CgxRw *R, CgxNv *v) {
    int did = 0;
    for (int i = 0; i + 1 < v->n; i++) {
        CgxNode *s1 = v->p[i]; if (s1->k != CGN_LET || s1->flag & 1) continue;
        int ndef = 0, ix = -1;
        for (int e = cgx_nm_first(R->f, s1->id); e >= 0; e = cgx_nm_next(R->f, e)) if (R->f->nm[e].kind == NM_DEF) { ndef++; ix = R->f->nm[e].x; }
        if (ndef != 1 || !R->S.bex[R->f->ins[ix].blk] || rw_ndefs(R, ix) != 1) continue;
        if (cgx_count_var_blk(&R->f->fn->b1, s1->s) != 1) continue;          /* o nome aparece uma vez em toda a funcao (contagem na AST: as reescritas da rodada ja mudaram os usos) */
        int j = i + 1; while (j < v->n && (v->p[j]->k == CGN_COMMENT || v->p[j]->k == CGN_DOC)) j++;
        if (j >= v->n) continue;
        CgxNode *s2 = v->p[j]; CgxNode *host = NULL;
        switch (s2->k) {
            case CGN_RET: case CGN_THROW: case CGN_LET: case CGN_IF: case CGN_EXPR: host = s2->a; break;
            case CGN_ASSIGN: if (s2->a->k == CGN_VAR && strcmp(s2->a->s, s1->s)) host = s2->b; break;
            default: break;
        }
        if (!host || cgx_count_var(host, s1->s) != 1) continue;
        if (s2->k == CGN_EXPR && host->k != CGN_CALL && host->k != CGN_MCALL) continue;
        const int fx = cgx_fx_expr(R->ir, s1->a);
        const int whole = host->k == CGN_VAR && !strcmp(host->s, s1->s) && s2->k != CGN_EXPR && s2->k != CGN_IF;
        if (!whole && (fx & (CGX_FX_THROW | CGX_FX_WRITE | CGX_FX_LOOP | CGX_FX_READ))) continue;
        CgxNode *hole = rw_find_var(host, s1->s); if (!hole) continue;
        cgx_become(hole, s1->a);
        memmove(v->p + i, v->p + i + 1, sizeof(CgxNode *) * (size_t)(v->n - i - 1)); v->n--; R->rep->temps++; R->changed++; did++; i--;
    }
    return did;
}
static void rw_collect(CgxRw *R, CgxNode *const *src, int n, CgxNv *out) {
    for (int i = 0; i < n; i++) rw_stmt(R, src[i], src + i + 1, n - i - 1, out);
}
static void rw_blk(CgxRw *R, CgxBlk *b) {
    CgxNv out = {0}; CgxNode **copy = (CgxNode **)cgx_xcalloc((size_t)b->n + 1, sizeof(CgxNode *)); if (b->n) memcpy(copy, b->p, sizeof(CgxNode *) * (size_t)b->n);
    const int n0 = b->n; rw_collect(R, copy, n0, &out); free(copy);
    if (R->passes & CGX_O_TEMPS) rw_temps(R, &out);
    CgxNode **np = (CgxNode **)cgx_alloc(R->g, sizeof(CgxNode *) * (size_t)(out.n ? out.n : 1)); if (out.n) memcpy(np, out.p, sizeof(CgxNode *) * (size_t)out.n);
    b->p = np; b->n = b->cap = out.n; free(out.p);
}
/* uma rodada de reescritas numa funcao; devolve se mudou algo */
static int cgx_opt_fn_round(CgxIr *ir, CgxFnIr *f, int passes, CgxOptReport *rep) {
    CgxRw R; memset(&R, 0, sizeof R); R.g = ir->g; R.ir = ir; R.f = f; R.passes = passes; R.rep = rep;
    cgx_sccp_run(ir, f, &R.S);
    R.lv = (char *)cgx_xcalloc((size_t)f->nval + 1, 1); R.li = (char *)cgx_xcalloc((size_t)f->nins + 1, 1);
    cgx_live_values(ir, f, &R.S, R.lv, R.li);
    rw_blk(&R, &f->fn->b1);
    cgx_sccp_free(&R.S); free(R.lv); free(R.li);
    return R.changed;
}

/* ================= 4. expansao em linha ================= */
typedef struct { Cgx *g; CgxIr *ir; const CgxFnIr *caller; int max, count; } CgxInl;
static int cgx_declares_name(const CgxNode *fn, const char *name) {
    for (int i = 0; i < fn->npar; i++) if (!strcmp(fn->par[i].name, name)) return 1;
    return cgx_stmts_declare(fn->b1.p, fn->b1.n, name);
}
static void cgx_call_names(const CgxNode *n, const char **out, int *no, int cap) {
    if (!n) return;
    if ((n->k == CGN_CALL) && n->s && *no < cap) out[(*no)++] = n->s;
    cgx_call_names(n->a, out, no, cap); cgx_call_names(n->b, out, no, cap); cgx_call_names(n->c, out, no, cap);
    for (int i = 0; i < n->b1.n; i++) cgx_call_names(n->b1.p[i], out, no, cap);
}
/* a ocorrencia (primeira) de `name` em e esta sempre avaliada? (nao sob o lado direito de and/or nem num ramo de ?:) */
static int cgx_occ_unconditional(const CgxNode *n, const char *name, int *found) {
    if (!n) return 1;
    if (n->k == CGN_VAR && !strcmp(n->s, name)) { *found = 1; return 1; }
    if (n->k == CGN_BIN && (n->op == CGO_AND || n->op == CGO_OR)) { if (!cgx_occ_unconditional(n->a, name, found)) return 0; return cgx_count_var(n->b, name) == 0; }
    if (n->k == CGN_TERN) { if (!cgx_occ_unconditional(n->a, name, found)) return 0; return cgx_count_var(n->b, name) == 0 && cgx_count_var(n->c, name) == 0; }
    if (!cgx_occ_unconditional(n->a, name, found) || !cgx_occ_unconditional(n->b, name, found) || !cgx_occ_unconditional(n->c, name, found)) return 0;
    for (int i = 0; i < n->b1.n; i++) if (!cgx_occ_unconditional(n->b1.p[i], name, found)) return 0;
    return 1;
}
static CgxNode *cgx_subst(Cgx *g, const CgxNode *e, const CgxNode *fn, CgxNode **args) {
    CgxNode *c = cgx_clone(g, e);
    CgxNode *stack[512]; int sp = 0; stack[sp++] = c;
    /* a raiz pode ser a propria VAR de um parametro */
    if (c->k == CGN_VAR) for (int i = 0; i < fn->npar; i++) if (!strcmp(c->s, fn->par[i].name)) { CgxNode *r = args[i]; return r; }
    while (sp) {
        CgxNode *n = stack[--sp];
        CgxNode **kid[3] = {&n->a, &n->b, &n->c};
        for (int k = 0; k < 3; k++) if (*kid[k]) {
            CgxNode *x = *kid[k];
            if (x->k == CGN_VAR) { for (int i = 0; i < fn->npar; i++) if (!strcmp(x->s, fn->par[i].name)) { *kid[k] = args[i]; args[i] = cgx_clone(g, args[i]); x = NULL; break; } }
            if (x && sp < 500) stack[sp++] = x;
        }
        for (int i = 0; i < n->b1.n; i++) {
            CgxNode *x = n->b1.p[i];
            if (x->k == CGN_VAR) { int hit = 0; for (int j = 0; j < fn->npar; j++) if (!strcmp(x->s, fn->par[j].name)) { n->b1.p[i] = args[j]; args[j] = cgx_clone(g, args[j]); hit = 1; break; } if (hit) continue; }
            if (sp < 500) stack[sp++] = x;
        }
    }
    return c;
}
/* tenta expandir a chamada n; devolve 1 se trocou */
static int inl_call(CgxInl *I, CgxNode *n) {
    if (n->k != CGN_CALL || n->bi || !n->ref || n->ref->k != CGN_FN) return 0;
    const CgxNode *fn = n->ref; const int fi = fn->id > 0 && fn->id < I->ir->nid ? I->ir->fnid[fn->id] : -1;
    if (fi < 0 || I->ir->fn[fi].cls || I->ir->rec[fi]) return 0;
    if (fn == I->caller->fn) return 0;
    const CgxNode *ret = NULL; int nstmt = 0;
    for (int i = 0; i < fn->b1.n; i++) { const CgxNode *s = fn->b1.p[i]; if (s->k == CGN_COMMENT || s->k == CGN_DOC) continue; nstmt++; if (s->k == CGN_RET && s->a) ret = s; }
    if (nstmt != 1 || !ret || cgx_expr_size(ret->a) > I->max) return 0;
    if (n->b1.n > fn->npar) return 0;
    CgxNode *args[40]; const CgxNode *e = ret->a;
    for (int i = 0; i < fn->npar; i++) {
        if (i < n->b1.n) args[i] = n->b1.p[i];
        else { const CgxNode *d = fn->par[i].def; if (!d || !cgx_is_lit(d)) return 0; args[i] = cgx_clone(I->g, d); }
    }
    if (cgx_fx_expr(I->ir, e) & (CGX_FX_THROW | CGX_FX_WRITE | CGX_FX_LOOP)) {
        /* E pode lancar ou ter efeito: so se todos os argumentos forem triviais (a ordem de avaliacao nao muda) */
        for (int i = 0; i < fn->npar; i++) if (args[i]->k != CGN_VAR && !cgx_is_lit(args[i])) return 0;
    }
    int neff = 0;
    for (int i = 0; i < fn->npar; i++) {
        const CgxNode *a = args[i]; const int occ = cgx_count_var(e, fn->par[i].name);
        if (a->k == CGN_VAR || cgx_is_lit(a)) continue;
        const int afx = cgx_fx_expr(I->ir, a);
        if (cgx_fx_removable(afx) && !(afx & CGX_FX_READ) && cgx_expr_size(a) <= 8) continue;
        if (cgx_fx_removable(afx) && occ == 1) continue;
        int found = 0;
        if (occ != 1 || !cgx_occ_unconditional(e, fn->par[i].name, &found) || neff++) return 0;
    }
    const char *names[16]; int nn = 0; cgx_call_names(e, names, &nn, 16);
    for (int i = 0; i < nn; i++) if (cgx_declares_name(I->caller->fn, names[i])) return 0;
    for (int i = 0; i < fn->npar; i++) if (cgx_declares_name(I->caller->fn, fn->par[i].name) && 0) return 0;
    const CgxTy ty = n->ty; const int line = n->line;
    CgxNode *args2[40]; for (int i = 0; i < fn->npar; i++) args2[i] = args[i];
    CgxNode *r = cgx_subst(I->g, e, fn, args2);
    cgx_become(n, r); n->ty = ty; n->line = line; I->count++;
    return 1;
}
static void inl_expr(CgxInl *I, CgxNode *n, int root_ok) {
    if (!n) return;
    inl_expr(I, n->a, 1); inl_expr(I, n->b, 1); inl_expr(I, n->c, 1);
    for (int i = 0; i < n->b1.n; i++) inl_expr(I, n->b1.p[i], 1);
    if (root_ok) while (n->k == CGN_CALL && inl_call(I, n)) { }
}
static void inl_blk(CgxInl *I, CgxBlk *b) {
    for (int i = 0; i < b->n; i++) {
        CgxNode *s = b->p[i];
        switch (s->k) {
            case CGN_LET: case CGN_RET: case CGN_THROW: inl_expr(I, s->a, 1); break;
            case CGN_EXPR: inl_expr(I, s->a, 0); break;
            case CGN_ASSIGN: if (s->a->k != CGN_VAR) { inl_expr(I, s->a->a, 1); inl_expr(I, s->a->b, 1); } inl_expr(I, s->b, 1); break;
            case CGN_IF: case CGN_WHILE: case CGN_EACH: inl_expr(I, s->a, 1); break;
            case CGN_FOR: inl_expr(I, s->a, 1); inl_expr(I, s->b, 1); break;
        }
        inl_blk(I, &s->b1); inl_blk(I, &s->b2); inl_blk(I, &s->b3);
    }
}

/* ================= 5. orquestracao ================= */
static int cgx_opt_run(Cgx *g, const CgxOptOpts *o, CgxOptReport *rep) {
    CgxOptReport dummy; if (!rep) rep = &dummy; memset(rep, 0, sizeof *rep);
    const int passes = o && o->passes ? o->passes : CGX_O_DEFAULT, max = o && o->max_rounds ? o->max_rounds : 10, inl_max = o && o->inline_max ? o->inline_max : 14;
    if (!g->prog || g->nerr) return 0;
    for (int round = 0; round < max; round++) {
        int changed = 0; rep->rounds++;
        if (passes & CGX_O_INLINE) {
            CgxIr *ir = cgx_ir_build_ex(g, -1, 0); CgxInl I = {g, ir, NULL, inl_max, 0};
            for (int i = 0; i < ir->nfn; i++) { I.caller = &ir->fn[i]; inl_blk(&I, &ir->fn[i].fn->b1); }
            rep->inlined += I.count; changed += I.count; cgx_ir_free(ir);
        }
        if (passes & CGX_O_ALGEBRA) {
            CgxIr *ir = cgx_ir_build_ex(g, -1, 0); int ch = 0;
            for (int i = 0; i < ir->nfn; i++) ch += cgx_alg_blk(ir, g, &ir->fn[i].fn->b1);
            rep->algebra += ch; changed += ch; cgx_ir_free(ir);
        }
        if (passes & (CGX_O_FOLD | CGX_O_COPY | CGX_O_DCE | CGX_O_UNREACH | CGX_O_TEMPS)) {
            CgxIr *ir = cgx_ir_build_ex(g, 1, (passes & CGX_O_GLOBALS) != 0);
            for (int i = 0; i < ir->nfn; i++) changed += cgx_opt_fn_round(ir, &ir->fn[i], passes, rep);
            cgx_ir_free(ir);
        }
        if (!changed) break;
    }
    cgx_number(g);
    rep->folded += 0;
    return rep->rounds;
}
/* o que a opcao `fold` do contrato (ucge_api.h) pede: so a dobra de constantes e a simplificacao, sem mexer em variaveis */
static int cgx_opt_fold(Cgx *g) { CgxOptOpts o = {CGX_O_FOLD | CGX_O_ALGEBRA | CGX_O_UNREACH, 0, 0}; return cgx_opt_run(g, &o, NULL); }
static void cgx_opt_report_str(const CgxOptReport *r, CgxB *o) {
    cgx_bf(o, "optimizer: %d round%s; folded %d expression%s, propagated %d constant%s, %d copy propagation%s, %d algebraic, %d dead statement%s removed, %d unreachable removed, %d branch%s decided, %d loop%s removed, %d temporar%s removed, %d call%s inlined\n",
           r->rounds, r->rounds == 1 ? "" : "s", r->folded, r->folded == 1 ? "" : "s", r->propagated, r->propagated == 1 ? "" : "s", r->copies, r->copies == 1 ? "" : "s", r->algebra, r->dead, r->dead == 1 ? "" : "s", r->unreachable,
           r->branches, r->branches == 1 ? "" : "es", r->loops, r->loops == 1 ? "" : "s", r->temps, r->temps == 1 ? "y" : "ies", r->inlined, r->inlined == 1 ? "" : "s");
}
