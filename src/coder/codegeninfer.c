/* ---------- coder/ucgeinfer.c: UCGE, inferencia de tipos (docs/ucge.md, "Type inference") ----------
 * Hindley-Milner (algoritmo J: unificacao destrutiva por union-find) com generalizacao por niveis (Remy), let-polimorfismo para funcoes e
 * metodos, e uma extensao gradual: `any` e consistente com tudo (nunca e erro) e cada ponto onde um valor `any` entra num lugar de tipo
 * concreto recebe uma MARCA na AST (n->chk), nao um teste de execucao. Em cima disso:
 *   - classes de variaveis de tipo (mascara de tipos permitidos por variavel: `a + b` pede numero ou str, `len(x)` pede lista, str ou mapa),
 *     que entram nos esquemas e na instanciacao: `fn add(a, b) return a + b` e forall a{int|float|str}. (a, a) -> a;
 *   - bidirecional: o tipo esperado (parametro anotado, retorno, let anotado) desce para listas, mapas, ternarios e null;
 *   - int <: float e T <: T? (opcional) sao os unicos subtipos; uniao so nasce na juncao de ramos (ternario, elementos de lista, retornos);
 *   - tipagem por fluxo: `x != null`, `x == null`, `not`, `and`/`or`, saida antecipada (return/throw/break/continue) estreitam o tipo;
 *   - grafo de chamadas com SCCs de Tarjan: uma funcao so e generalizada depois de inferido o seu grupo de recursao mutua;
 *   - pendencias (sobrecarga resolvida quando o operando se resolve: indice, `in`, iteracao, contains) com padrao no fim do grupo.
 * Cada no da AST recebe n->tid (TYK_GEN = parametro de tipo da funcao), n->cert (1 derivado, 2 declarado, 3 gradual: nunca "exato") e,
 * onde ha fronteira gradual, n->chk. Erros: linha, o que se esperava e uma sugestao. Mesma API de erros do validador (g->err). */

enum { CGC_NONE, CGC_DERIVED, CGC_DECLARED, CGC_ANY };
enum { IP_INDEX, IP_ITER, IP_IN, IP_CONTAINS };
enum { ST_LET, ST_ASSIGN, ST_ARG, ST_RET, ST_PUSH, ST_ELEM, ST_FIELD, ST_DEFAULT, ST_BRANCH };
#define INF_KM(k) (1u << (k))
#define INF_NUM (INF_KM(TYK_INT) | INF_KM(TYK_FLOAT))
#define INF_ORD (INF_NUM | INF_KM(TYK_STR))
#define INF_MAXQ 24

typedef struct { CgxTid t; int nq; unsigned m[INF_MAXQ]; CgxTid qv[INF_MAXQ]; } InfSch;
typedef struct { int sid; CgxTid ty, nty; CgxNode *decl; int depth, glob, declared, path; } InfSym;    /* path: nome sintetico `obj.campo` (estreitamento de campo) */
typedef struct { int sym; CgxTid old; } InfTrail;
typedef struct { int nsym, ntr; } InfMark;
typedef struct { int sym; CgxTid ty; } InfFact;
typedef struct { int kind, done; CgxTid a, b, r; CgxNode *n; } InfPend;
typedef struct { int line; CgxTid want; CgxNode *n; } InfChk;
typedef struct { int kind, idx; const char *a, *b; } InfSite;
typedef struct {
    CgxNode *d, *cls; int kind;                        /* 0 funcao, 1 metodo, 2 constante de modulo */
    int *e, ne, cape; int idx, low, onst, chain;
    int sigdone, has_sch, np, retfix, done;
    CgxTid *pt, ret, mono; InfSch sch;
} InfUnit;
typedef struct { CgxNode *d; int sid; CgxTid tid; } InfCls;
typedef struct {
    Cgx *g; CgxTypes *T;
    CgxTid I, F, S, B, V, A, E;
    unsigned *mask; int nmask;
    InfSym *sym; int nsym, capsym; int *bind; int nbind, depth;
    InfTrail *tr; int ntr, captr;
    InfUnit *u; int nu;
    InfCls *cls; int ncls;
    int *hfn, *hmeth, *hconst, *hcls, *bi_of, nh, sidErr, sidSelf;
    InfPend *pd; int npd, cappd;
    InfChk *ck; int nck, capck;
    InfUnit *cu; CgxNode *cconst;
    CgxTid *rets; CgxNode **retn; int nret, capret, nretv, nretb;
    int loops, nerr0, nerr1, ncert[4], nexpr, npath; CgxNode *assign_target;
    int *tstack, tsp, tcount, *scc_order, nscc;
} CgxInf;

/* ---------------- basicos ---------------- */
static CgxTid inf_f(CgxInf *c, CgxTid t) { return cgx_ty_find(c->T, t); }
static int inf_k(CgxInf *c, CgxTid t) { return c->T->t[cgx_ty_find(c->T, t)].k; }
static CgxTid inf_a(CgxInf *c, CgxTid t, int i) { return c->T->t[cgx_ty_find(c->T, t)].a[i]; }
static CgxTid inf_var(CgxInf *c) { return cgx_ty_var(c->T); }
static unsigned inf_mget(CgxInf *c, CgxTid t) { return t < c->nmask ? c->mask[t] : 0; }
static void inf_mset(CgxInf *c, CgxTid t, unsigned m) {
    if (t >= c->nmask) { const int nn = c->T->n + 64; c->mask = (unsigned *)realloc(c->mask, sizeof(unsigned) * (size_t)nn); memset(c->mask + c->nmask, 0, sizeof(unsigned) * (size_t)(nn - c->nmask)); c->nmask = nn; }
    c->mask[t] = m;
}
static void inf_err(CgxInf *c, int line, const char *fmt, ...) {
    char m[300]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    for (int i = c->g->nerr > 8 ? c->g->nerr - 8 : 0; i < c->g->nerr; i++) if (c->g->err[i].line == line && !strcmp(c->g->err[i].msg, m)) return;
    cgx_err(c->g, line, "%s", m);
}
static const char *inf_kname(int k) {
    static const char *const nm[] = {"?", "any", "int", "float", "str", "bool", "void", "never", "list", "map", "tuple", "function", "optional", "union", "class", "type parameter"};
    return nm[k];
}
/* texto de um tipo (variavel livre vira `?`; com mascara, a lista dos tipos que aceita) */
static void inf_pr(CgxInf *c, CgxTid t, CgxB *o, const InfSch *s) {
    t = inf_f(c, t); const CgxTyNode x = c->T->t[t];
    switch (x.k) {
        case TYK_VAR: {
            const unsigned m = inf_mget(c, t);
            if (!m) { cgx_bs(o, "?"); break; }
            if (m == INF_NUM) { cgx_bs(o, "number"); break; }
            int first = 1; for (int k = 0; k < 16; k++) if (m & INF_KM(k)) { if (!first) cgx_bs(o, "|"); cgx_bs(o, inf_kname(k)); first = 0; }
            break;
        }
        case TYK_GEN: cgx_bf(o, "'%c", 'a' + x.sym % 26); (void)s; break;
        case TYK_LIST: cgx_bs(o, "list["); inf_pr(c, x.a[0], o, s); cgx_bs(o, "]"); break;
        case TYK_MAP: cgx_bs(o, "map[str,"); inf_pr(c, x.a[0], o, s); cgx_bs(o, "]"); break;
        case TYK_OPT: inf_pr(c, x.a[0], o, s); cgx_bs(o, "?"); break;
        case TYK_CLASS: cgx_bs(o, cgx_symname(c->g, x.sym)); break;
        case TYK_TUPLE: cgx_bs(o, "("); for (int i = 0; i < x.n; i++) { if (i) cgx_bs(o, ", "); inf_pr(c, c->T->t[t].a[i], o, s); } cgx_bs(o, ")"); break;
        case TYK_UNION: for (int i = 0; i < x.n; i++) { if (i) cgx_bs(o, " | "); inf_pr(c, c->T->t[t].a[i], o, s); } break;
        case TYK_FN: cgx_bs(o, "("); for (int i = 0; i + 1 < x.n; i++) { if (i) cgx_bs(o, ", "); inf_pr(c, c->T->t[t].a[i], o, s); } cgx_bs(o, ") -> "); inf_pr(c, c->T->t[t].a[x.n - 1], o, s); break;
        default: cgx_bs(o, inf_kname(x.k));
    }
}
static char *inf_tn(CgxInf *c, CgxTid t, CgxB *b) { memset(b, 0, sizeof *b); inf_pr(c, t, b, NULL); if (!b->p) cgx_bs(b, ""); return b->p; }
#define INF_TN(name, t) CgxB name##_b; const char *name = inf_tn(c, (t), &name##_b)
#define INF_FREE(name) free(name##_b.p)

static int inf_hasvar(CgxInf *c, CgxTid t) {
    t = inf_f(c, t); if (c->T->t[t].k == TYK_VAR) return 1;
    for (int i = 0; i < c->T->t[t].n; i++) if (inf_hasvar(c, c->T->t[t].a[i])) return 1;
    return 0;
}
static int inf_hasany(CgxInf *c, CgxTid t) {
    t = inf_f(c, t); if (c->T->t[t].k == TYK_ANY) return 1;
    for (int i = 0; i < c->T->t[t].n; i++) if (inf_hasany(c, c->T->t[t].a[i])) return 1;
    return 0;
}
/* baixa o nivel de todas as variaveis de t ao menos a lv (variaveis que nao podem ser generalizadas) */
static void inf_lower(CgxInf *c, CgxTid t, int lv) {
    t = inf_f(c, t);
    if (c->T->t[t].k == TYK_VAR) { if (c->T->t[t].level > lv) c->T->t[t].level = lv; return; }
    for (int i = 0; i < c->T->t[t].n; i++) inf_lower(c, c->T->t[t].a[i], lv);
}

/* ---------------- unificacao com classes de variaveis e any gradual ---------------- */
static int inf_bind(CgxInf *c, CgxTid v, CgxTid t) {          /* v: variavel livre; t: representante */
    if (v == t) return 1;
    const unsigned m = inf_mget(c, v); const int k = c->T->t[t].k;
    if (k == TYK_VAR) {
        const unsigned m2 = inf_mget(c, t);
        unsigned nm = m && m2 ? (m & m2) : (m | m2);
        if (m && m2 && !nm) return 0;
        if (cgx_ty_occurs(c->T, v, t)) return 0;
        inf_mset(c, t, nm); c->T->t[v].link = t; return 1;
    }
    if (m && k != TYK_ANY && !(m & INF_KM(k))) return 0;
    if (cgx_ty_occurs(c->T, v, t)) return 0;
    c->T->t[v].link = t; return 1;
}
static int inf_unify(CgxInf *c, CgxTid a, CgxTid b) {
    a = inf_f(c, a); b = inf_f(c, b);
    if (a == b) return 1;
    int ka = c->T->t[a].k, kb = c->T->t[b].k;
    if (ka == TYK_VAR) return inf_bind(c, a, b);
    if (kb == TYK_VAR) return inf_bind(c, b, a);
    if (ka == TYK_ANY || kb == TYK_ANY) return 1;
    const CgxTyNode x = c->T->t[a], y = c->T->t[b];
    if (x.k != y.k || x.n != y.n || x.sym != y.sym) return 0;
    for (int i = 0; i < x.n; i++) if (!inf_unify(c, c->T->t[a].a[i], c->T->t[b].a[i])) return 0;
    return 1;
}
/* as duas formas sao compativeis, sem ligar nada (variaveis livres e any contam como compativeis) */
static int inf_same(CgxInf *c, CgxTid a, CgxTid b) {
    a = inf_f(c, a); b = inf_f(c, b);
    if (a == b) return 1;
    const CgxTyNode x = c->T->t[a], y = c->T->t[b];
    if (x.k == TYK_VAR || y.k == TYK_VAR || x.k == TYK_ANY || y.k == TYK_ANY) return 1;
    if (x.k != y.k || x.n != y.n || x.sym != y.sym) return 0;
    for (int i = 0; i < x.n; i++) if (!inf_same(c, c->T->t[a].a[i], c->T->t[b].a[i])) return 0;
    return 1;
}
static int inf_can(CgxInf *c, CgxTid d, CgxTid s) {
    d = inf_f(c, d); s = inf_f(c, s);
    if (d == s) return 1;
    const CgxTyNode x = c->T->t[d], y = c->T->t[s];
    if (x.k == TYK_VAR || y.k == TYK_VAR || x.k == TYK_ANY || y.k == TYK_ANY) return 1;
    if (x.k == TYK_FLOAT && y.k == TYK_INT) return 1;
    if (x.k == TYK_OPT) return inf_can(c, x.a[0], y.k == TYK_OPT ? y.a[0] : s);
    if (y.k == TYK_OPT) return 0;
    if (y.k == TYK_UNION) { for (int i = 0; i < y.n; i++) if (!inf_can(c, d, c->T->t[s].a[i])) return 0; return 1; }
    if (x.k == TYK_UNION) { for (int i = 0; i < x.n; i++) if (inf_can(c, c->T->t[d].a[i], s)) return 1; return 0; }
    return inf_same(c, d, s);
}
static void inf_mark(CgxInf *c, CgxNode *n, CgxTid want) {
    if (!n || n->chk) return;
    n->chk = want;
    if (c->nck == c->capck) { c->capck = c->capck ? c->capck * 2 : 32; c->ck = (InfChk *)realloc(c->ck, sizeof(InfChk) * (size_t)c->capck); }
    c->ck[c->nck++] = (InfChk){n->line, want, n};
}
/* o valor de tipo s pode entrar num lugar de tipo d? liga variaveis; any e consistente (marca a verificacao na AST do no sn) */
static int inf_sub(CgxInf *c, CgxTid d, CgxTid s, CgxNode *sn) {
    d = inf_f(c, d); s = inf_f(c, s);
    if (d == s) return 1;
    const int kd = c->T->t[d].k, ks = c->T->t[s].k;
    if (ks == TYK_ANY) { if (kd == TYK_VAR) return inf_bind(c, d, s); if (kd != TYK_ANY) inf_mark(c, sn, d); return 1; }
    if (kd == TYK_ANY) return 1;
    if (kd == TYK_VAR) return inf_bind(c, d, s);
    if (ks == TYK_VAR) {
        if (kd == TYK_FLOAT) { const unsigned m = inf_mget(c, s); inf_mset(c, s, m ? (m & INF_NUM) : INF_NUM); return inf_mget(c, s) != 0; }
        if (kd == TYK_OPT) return inf_sub(c, c->T->t[d].a[0], s, sn);
        return inf_bind(c, s, d);
    }
    if (kd == TYK_FLOAT && ks == TYK_INT) return 1;
    if (kd == TYK_OPT) { const CgxTid di = c->T->t[d].a[0]; return inf_sub(c, di, ks == TYK_OPT ? c->T->t[s].a[0] : s, sn); }
    if (ks == TYK_OPT) return 0;
    if (ks == TYK_UNION) { for (int i = 0; i < c->T->t[s].n; i++) if (!inf_sub(c, d, c->T->t[s].a[i], NULL)) return 0; return 1; }
    if (kd == TYK_UNION) {
        if (!inf_can(c, d, s)) return 0;
        for (int i = 0; i < c->T->t[d].n; i++) { const CgxTid m = inf_f(c, c->T->t[d].a[i]); if (c->T->t[m].k == ks && inf_hasvar(c, s)) { inf_unify(c, m, s); break; } }
        return 1;
    }
    if (kd != ks) return 0;
    return inf_unify(c, d, s);                                         /* listas e mapas sao invariantes */
}
/* juncao de dois ramos: numero com numero e float; opcional absorve; estrutura igual unifica; senao uniao */
static CgxTid inf_join(CgxInf *c, CgxTid a, CgxTid b) {
    a = inf_f(c, a); b = inf_f(c, b);
    if (a == b) return a;
    int ka = c->T->t[a].k, kb = c->T->t[b].k;
    if (ka == TYK_ANY) return a;
    if (kb == TYK_ANY) return b;
    if (ka == TYK_VAR || kb == TYK_VAR) { if (inf_unify(c, a, b)) return inf_f(c, a); return c->A; }
    if ((ka == TYK_INT && kb == TYK_FLOAT) || (ka == TYK_FLOAT && kb == TYK_INT)) return c->F;
    if (ka == TYK_OPT || kb == TYK_OPT) {
        const CgxTid ia = ka == TYK_OPT ? c->T->t[a].a[0] : a, ib = kb == TYK_OPT ? c->T->t[b].a[0] : b;
        return cgx_ty_opt(c->T, inf_join(c, ia, ib));
    }
    if (ka == kb && (ka == TYK_LIST || ka == TYK_MAP || ka == TYK_CLASS || ka == TYK_TUPLE) && inf_same(c, a, b)) { inf_unify(c, a, b); return inf_f(c, a); }
    CgxTid m[2] = {a, b}; return cgx_ty_union(c->T, m, 2);
}

/* ---------------- esquemas ---------------- */
static CgxTid inf_gen_walk(CgxInf *c, CgxTid t, InfSch *s) {
    t = inf_f(c, t); const CgxTyNode x = c->T->t[t];
    if (x.k == TYK_VAR) {
        if (x.level <= c->T->level) return t;
        for (int i = 0; i < s->nq; i++) if (s->qv[i] == t) return cgx_ty_gen(c->T, i);
        if (s->nq >= INF_MAXQ) return t;
        s->qv[s->nq] = t; s->m[s->nq] = inf_mget(c, t); return cgx_ty_gen(c->T, s->nq++);
    }
    if (!x.n) return t;
    CgxTid a[64]; const int n = x.n > 64 ? 64 : x.n;
    for (int i = 0; i < n; i++) a[i] = inf_gen_walk(c, c->T->t[t].a[i], s);
    return x.k == TYK_UNION ? cgx_ty_union(c->T, a, n) : cgx_ty_cons(c->T, x.k, a, n, x.sym);
}
static CgxTid inf_inst_walk(CgxInf *c, CgxTid t, const CgxTid *fresh, int nq) {
    t = inf_f(c, t); const CgxTyNode x = c->T->t[t];
    if (x.k == TYK_GEN) return x.sym < nq ? fresh[x.sym] : t;
    if (!x.n) return t;
    CgxTid a[64]; const int n = x.n > 64 ? 64 : x.n;
    for (int i = 0; i < n; i++) a[i] = inf_inst_walk(c, c->T->t[t].a[i], fresh, nq);
    return x.k == TYK_UNION ? cgx_ty_union(c->T, a, n) : cgx_ty_cons(c->T, x.k, a, n, x.sym);
}
static CgxTid inf_instantiate(CgxInf *c, const InfSch *s) {
    CgxTid fresh[INF_MAXQ];
    for (int i = 0; i < s->nq; i++) { fresh[i] = inf_var(c); if (s->m[i]) inf_mset(c, fresh[i], s->m[i]); }
    return inf_inst_walk(c, s->t, fresh, s->nq);
}
/* tipo final de um no: variaveis do esquema viram TYK_GEN, qualquer outra sobra vira any */
static CgxTid inf_zonk(CgxInf *c, CgxTid t, const InfSch *s) {
    t = inf_f(c, t); const CgxTyNode x = c->T->t[t];
    if (x.k == TYK_VAR) { if (s) for (int i = 0; i < s->nq; i++) if (s->qv[i] == t) return cgx_ty_gen(c->T, i); return c->A; }
    if (!x.n) return t;
    CgxTid a[64]; const int n = x.n > 64 ? 64 : x.n;
    for (int i = 0; i < n; i++) a[i] = inf_zonk(c, c->T->t[t].a[i], s);
    return x.k == TYK_UNION ? cgx_ty_union(c->T, a, n) : cgx_ty_cons(c->T, x.k, a, n, x.sym);
}

/* ---------------- escopo e estreitamento ---------------- */
static int inf_is_ref(const CgxNode *n) { return n->k == CGN_VAR || (n->k == CGN_FIELD && n->a->k == CGN_VAR); }
static int inf_path_sid(Cgx *g, const CgxNode *fld) { char nm[140]; snprintf(nm, sizeof nm, "%.60s.%.60s", fld->a->s, fld->s); return cgx_intern(g, nm); }
static int inf_lookup(CgxInf *c, int sid) { return sid > 0 && sid < c->nbind ? c->bind[sid] - 1 : -1; }
static void inf_bindgrow(CgxInf *c, int sid) {
    if (sid < c->nbind) return;
    const int nn = sid + 64; c->bind = (int *)realloc(c->bind, sizeof(int) * (size_t)nn); memset(c->bind + c->nbind, 0, sizeof(int) * (size_t)(nn - c->nbind)); c->nbind = nn;
}
static int inf_edit(const char *a, const char *b) {          /* distancia de edicao (nomes curtos) */
    const int la = (int)strlen(a), lb = (int)strlen(b); if (la > 40 || lb > 40) return 99;
    int d[2][42]; for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++) {
        d[i & 1][0] = i;
        for (int j = 1; j <= lb; j++) { int v = d[(i - 1) & 1][j - 1] + (a[i - 1] != b[j - 1]); if (d[(i - 1) & 1][j] + 1 < v) v = d[(i - 1) & 1][j] + 1; if (d[i & 1][j - 1] + 1 < v) v = d[i & 1][j - 1] + 1; d[i & 1][j] = v; }
    }
    return d[la & 1][lb];
}
static const char *inf_suggest(CgxInf *c, const char *name) {
    const char *best = NULL; int bd = 3;
    for (int i = 0; i < c->nsym; i++) { const char *s = cgx_symname(c->g, c->sym[i].sid); const int d = inf_edit(name, s); if (d < bd && strcmp(name, s)) { bd = d; best = s; } }
    return best;
}
static int inf_declare(CgxInf *c, int line, const char *name, CgxTid ty, CgxNode *decl, int glob, int declared) {
    const int sid = cgx_intern(c->g, name); inf_bindgrow(c, sid);
    const int o = inf_lookup(c, sid);
    if (o >= 0) {
        inf_err(c, line, "`%s` is already declared%s (line %d): a name cannot be declared twice or shadow a visible one, because Python and JavaScript scope it differently; use another name or assign without `let`",
                name, c->sym[o].glob ? " at module level" : c->sym[o].depth == c->depth ? " in this block" : " in an outer block", c->sym[o].decl ? c->sym[o].decl->line : 0);
        return o;
    }
    if (c->nsym == c->capsym) { c->capsym = c->capsym ? c->capsym * 2 : 64; c->sym = (InfSym *)realloc(c->sym, sizeof(InfSym) * (size_t)c->capsym); }
    c->sym[c->nsym] = (InfSym){sid, ty, 0, decl, c->depth, glob, declared};
    c->bind[sid] = c->nsym + 1;
    return c->nsym++;
}
static void inf_push(CgxInf *c, InfMark *m) { m->nsym = c->nsym; m->ntr = c->ntr; c->depth++; }
static void inf_pop(CgxInf *c, const InfMark *m) {
    while (c->ntr > m->ntr) { c->ntr--; if (c->tr[c->ntr].sym < c->nsym) c->sym[c->tr[c->ntr].sym].nty = c->tr[c->ntr].old; }
    while (c->nsym > m->nsym) { c->nsym--; c->bind[c->sym[c->nsym].sid] = 0; }
    c->depth--;
}
static void inf_narrow(CgxInf *c, int sym, CgxTid ty) {
    if (c->ntr == c->captr) { c->captr = c->captr ? c->captr * 2 : 32; c->tr = (InfTrail *)realloc(c->tr, sizeof(InfTrail) * (size_t)c->captr); }
    c->tr[c->ntr++] = (InfTrail){sym, c->sym[sym].nty}; c->sym[sym].nty = ty;
}
static int inf_path_sym(CgxInf *c, CgxNode *fld, int create) {      /* simbolo sintetico de `obj.campo` (so um nivel) */
    const int sid = inf_path_sid(c->g, fld); inf_bindgrow(c, sid);
    int y = inf_lookup(c, sid);
    if (y < 0 && create) { y = inf_declare(c, fld->line, cgx_symname(c->g, sid), fld->tid, NULL, 0, 0); c->sym[y].path = 1; c->npath++; }
    return y;
}
static void inf_kill(CgxInf *c, int sym) {                      /* atribuicao: o estreitamento acaba, tambem para o que vem de escopos de fora */
    c->sym[sym].nty = 0;
    for (int i = 0; i < c->ntr; i++) if (c->tr[i].sym == sym) c->tr[i].old = 0;
}
static void inf_collect(CgxBlk *b, int *out, int *n, int cap, Cgx *g);
static void inf_collect_node(CgxNode *s, int *out, int *n, int cap, Cgx *g) {
    if (!s) return;
    if (s->k == CGN_ASSIGN && s->a && inf_is_ref(s->a) && *n < cap) out[(*n)++] = s->a->k == CGN_VAR ? cgx_intern(g, s->a->s) : inf_path_sid(g, s->a);
    inf_collect(&s->b1, out, n, cap, g); inf_collect(&s->b2, out, n, cap, g); inf_collect(&s->b3, out, n, cap, g);
}
static void inf_collect(CgxBlk *b, int *out, int *n, int cap, Cgx *g) { for (int i = 0; i < b->n; i++) inf_collect_node(b->p[i], out, n, cap, g); }
static void inf_kill_blk(CgxInf *c, CgxBlk *b) {
    int out[256], n = 0; inf_collect(b, out, &n, 256, c->g);
    for (int i = 0; i < n; i++) { const int y = inf_lookup(c, out[i]); if (y >= 0) inf_kill(c, y); }
}
static void inf_kill_paths(CgxInf *c) { if (c->npath) for (int i = 0; i < c->nsym; i++) if (c->sym[i].path && c->sym[i].nty) inf_kill(c, i); }
static int inf_blk_assigns(CgxInf *c, CgxBlk *b, int sid) { int out[256], n = 0; inf_collect(b, out, &n, 256, c->g); for (int i = 0; i < n; i++) if (out[i] == sid) return 1; return 0; }
static int inf_term(CgxBlk *b) {                                 /* o bloco nunca chega ao fim (return, throw, break, continue) */
    for (int i = b->n - 1; i >= 0; i--) {
        CgxNode *s = b->p[i];
        if (s->k == CGN_COMMENT || s->k == CGN_DOC) continue;
        if (s->k == CGN_RET || s->k == CGN_THROW || s->k == CGN_BREAK || s->k == CGN_CONT) return 1;
        if (s->k == CGN_IF) return s->b2.n && inf_term(&s->b1) && inf_term(&s->b2);
        return 0;
    }
    return 0;
}
/* fatos que a condicao e (pos=1) ou nao e (pos=0) verdadeira garante sobre variaveis */
static int inf_facts(CgxInf *c, CgxNode *e, int pos, InfFact *f, int nf, int cap) {
    if (!e || nf >= cap) return nf;
    if (e->k == CGN_UN && e->op == CGO_NOT) return inf_facts(c, e->a, !pos, f, nf, cap);
    if (e->k == CGN_BIN && ((e->op == CGO_AND && pos) || (e->op == CGO_OR && !pos))) { nf = inf_facts(c, e->a, pos, f, nf, cap); return inf_facts(c, e->b, pos, f, nf, cap); }
    if (e->k == CGN_BIN && (e->op == CGO_EQ || e->op == CGO_NE)) {
        CgxNode *v = inf_is_ref(e->a) ? e->a : inf_is_ref(e->b) ? e->b : NULL, *o = v == e->a ? e->b : e->a;
        if (!v || inf_is_ref(o)) return nf;
        const int y = v->k == CGN_VAR ? inf_lookup(c, cgx_intern(c->g, v->s)) : inf_path_sym(c, v, 1); if (y < 0) return nf;
        const int equal = (e->op == CGO_EQ) == (pos != 0);           /* o fato e "v == o" */
        CgxTid cur = c->sym[y].nty ? c->sym[y].nty : c->sym[y].ty; cur = inf_f(c, cur); const int k = c->T->t[cur].k;
        if (o->k == CGN_NULL) { if (!equal && k == TYK_OPT) { f[nf].sym = y; f[nf].ty = inf_f(c, c->T->t[cur].a[0]); nf++; } return nf; }
        if (!equal) return nf;
        CgxTid lit = o->k == CGN_INT ? c->I : o->k == CGN_FLT ? c->F : o->k == CGN_STR ? c->S : o->k == CGN_BOOL ? c->B : 0;
        if (!lit) return nf;
        if (k == TYK_OPT) { f[nf].sym = y; f[nf].ty = inf_f(c, c->T->t[cur].a[0]); nf++; }
        else if (k == TYK_ANY) { f[nf].sym = y; f[nf].ty = lit; nf++; }
        else if (k == TYK_UNION) { for (int i = 0; i < c->T->t[cur].n; i++) { const CgxTid m = inf_f(c, c->T->t[cur].a[i]); if (m == lit) { f[nf].sym = y; f[nf].ty = lit; nf++; break; } } }
    }
    return nf;
}
static void inf_apply(CgxInf *c, const InfFact *f, int n) { for (int i = 0; i < n; i++) inf_narrow(c, f[i].sym, f[i].ty); }

/* ---------------- tipos escritos ---------------- */
static CgxTid inf_prim_of(CgxInf *c, int cgt) {
    switch (cgt) { case CGT_INT: return c->I; case CGT_FLOAT: return c->F; case CGT_STR: return c->S; case CGT_BOOL: return c->B; case CGT_VOID: return c->V; default: return 0; }
}
static CgxTid inf_from_ty(CgxInf *c, const CgxTy *t) {
    switch (t->k) {
        case CGT_ANY: return c->A;
        case CGT_LIST: { const CgxTid e = inf_prim_of(c, t->e); return cgx_ty_list(c->T, e ? e : inf_var(c)); }
        case CGT_MAP: { const CgxTid e = inf_prim_of(c, t->e); return cgx_ty_map(c->T, e ? e : inf_var(c)); }
        case CGT_CLASS: return cgx_ty_class(c->T, cgx_intern(c->g, t->cls));
        case CGT_NULL: return cgx_ty_opt(c->T, inf_var(c));
        default: { const CgxTid p = inf_prim_of(c, t->k); return p ? p : c->A; }
    }
}
static InfCls *inf_cls_by_sid(CgxInf *c, int sid) { const int i = sid > 0 && sid < c->nh ? c->hcls[sid] - 1 : -1; return i >= 0 ? &c->cls[i] : NULL; }
static InfCls *inf_cls_of(CgxInf *c, CgxTid t) { t = inf_f(c, t); return c->T->t[t].k == TYK_CLASS ? inf_cls_by_sid(c, c->T->t[t].sym) : NULL; }
static CgxNode *inf_field(InfCls *k, const char *n) { for (int i = 0; i < k->d->b1.n; i++) if (k->d->b1.p[i]->k == CGN_FIELDDECL && !strcmp(k->d->b1.p[i]->s, n)) return k->d->b1.p[i]; return NULL; }
static InfUnit *inf_method(CgxInf *c, InfCls *k, const char *n) {
    const int sid = cgx_intern(c->g, n);
    for (int i = sid < c->nh ? c->hmeth[sid] - 1 : -1; i >= 0; i = c->u[i].chain - 1) if (c->u[i].cls == k->d) return &c->u[i];
    return NULL;
}

/* ---------------- mensagens de erro de tipo ---------------- */
static void inf_hint(CgxInf *c, CgxTid d, CgxTid s, CgxB *h) {
    d = inf_f(c, d); s = inf_f(c, s); const int kd = c->T->t[d].k, ks = c->T->t[s].k;
    if (ks == TYK_OPT && kd != TYK_OPT) cgx_bs(h, "the value may be null; test `x != null` first");
    else if (kd == TYK_INT && ks == TYK_FLOAT) cgx_bs(h, "convert with int(x) or floor(x), or declare the target float");
    else if ((kd == TYK_INT || kd == TYK_FLOAT) && (ks == TYK_STR || ks == TYK_BOOL)) cgx_bf(h, "convert with %s(x)", kd == TYK_INT ? "int" : "float");
    else if (kd == TYK_STR && (ks == TYK_INT || ks == TYK_FLOAT || ks == TYK_BOOL)) cgx_bs(h, "convert with str(x)");
    else if (kd == TYK_BOOL) cgx_bs(h, "write the comparison, e.g. `x != 0` or `len(xs) > 0`");
    else if (kd == ks && (kd == TYK_LIST || kd == TYK_MAP)) cgx_bs(h, "lists and maps are invariant: the element types must match exactly");
    else cgx_bs(h, "annotate the source with the type you expect, or convert the value");
}
static int inf_contains(CgxInf *c, CgxTid v, CgxTid t) {              /* a variavel v ocorre em t? (sem mexer nos niveis) */
    t = inf_f(c, t); if (t == v) return 1;
    for (int i = 0; i < c->T->t[t].n; i++) if (inf_contains(c, v, c->T->t[t].a[i])) return 1;
    return 0;
}
static int inf_sub_at(CgxInf *c, CgxTid d, CgxTid s, CgxNode *sn, int line, const InfSite *st) {
    if (inf_sub(c, d, s, sn)) return 1;
    { const CgxTid fd = inf_f(c, d), fs = inf_f(c, s);
      if (c->T->t[fd].k == TYK_VAR && fd != fs && inf_contains(c, fd, fs)) { inf_err(c, line, "infinite type: this would make a value contain itself (a list or map that holds the very variable it is stored in); use a different variable for the nested value"); return 0; } }
    CgxB h = {0}; inf_hint(c, d, s, &h);
    INF_TN(td, d); INF_TN(ts, s);
    switch (st->kind) {
        case ST_LET: inf_err(c, line, "`%s` is declared %s but the value is %s; %s", st->a, td, ts, h.p); break;
        case ST_ASSIGN: inf_err(c, line, "cannot assign %s to a target of type %s; %s", ts, td, h.p); break;
        case ST_ARG: inf_err(c, line, "`%s` argument %d (`%s`) expects %s, found %s; %s", st->a, st->idx, st->b, td, ts, h.p); break;
        case ST_RET: inf_err(c, line, "`%s` declares `-> %s` but returns %s; %s", st->a, td, ts, h.p); break;
        case ST_PUSH: inf_err(c, line, "push: the list holds %s but the value is %s; %s", td, ts, h.p); break;
        case ST_ELEM: inf_err(c, line, "the elements of this %s must have one type: expected %s, found %s; %s", st->a, td, ts, h.p); break;
        case ST_BRANCH: inf_err(c, line, "a branch of `?:` is %s but %s was expected; %s", ts, td, h.p); break;
        case ST_FIELD: inf_err(c, line, "field `%s` is %s but the value is %s; %s", st->a, td, ts, h.p); break;
        case ST_DEFAULT: inf_err(c, line, "the default of `%s` is %s but the parameter is %s", st->a, ts, td); break;
        default: inf_err(c, line, "expected %s, found %s; %s", td, ts, h.p);
    }
    free(h.p); INF_FREE(td); INF_FREE(ts);
    return 0;
}
/* o operando e opcional: erro "pode ser nulo" e segue com o tipo de dentro */
static CgxTid inf_nn(CgxInf *c, CgxNode *n, CgxTid t) {
    t = inf_f(c, t);
    if (c->T->t[t].k != TYK_OPT) return t;
    INF_TN(tn, t);
    inf_err(c, n->line, "%s%s%s may be null (its type is %s) here; test `x != null` first, or return early when it is null", n->k == CGN_VAR ? "`" : "this value", n->k == CGN_VAR ? n->s : "", n->k == CGN_VAR ? "`" : "", tn);
    INF_FREE(tn);
    return inf_f(c, c->T->t[t].a[0]);
}
/* t deve ser do tipo k (variavel livre e ligada; any fica marcado) */
static int inf_want(CgxInf *c, CgxNode *n, CgxTid t, CgxTid want) {
    t = inf_f(c, t); const int k = c->T->t[t].k;
    if (k == TYK_ANY) { inf_mark(c, n, want); return 1; }
    if (k == TYK_VAR) return inf_bind(c, t, inf_f(c, want));
    return t == inf_f(c, want);
}
static void inf_pend_add(CgxInf *c, int kind, CgxTid a, CgxTid b, CgxTid r, CgxNode *n) {
    if (c->npd == c->cappd) { c->cappd = c->cappd ? c->cappd * 2 : 16; c->pd = (InfPend *)realloc(c->pd, sizeof(InfPend) * (size_t)c->cappd); }
    c->pd[c->npd++] = (InfPend){kind, 0, a, b, r, n};
}

/* ---------------- pendencias (sobrecarga resolvida quando o operando aparece) ---------------- */
static int inf_pend_try(CgxInf *c, InfPend *p, int force) {
    CgxTid a = inf_f(c, p->kind == IP_IN ? p->b : p->a); int k = c->T->t[a].k; CgxNode *n = p->n;
    if (k == TYK_VAR) {
        if (!force) return 0;
        if (p->kind == IP_CONTAINS) { const unsigned m = inf_mget(c, a); inf_mset(c, a, (m ? m : (unsigned)-1) & (INF_KM(TYK_STR) | INF_KM(TYK_LIST) | INF_KM(TYK_MAP))); return 1; }
        inf_bind(c, a, cgx_ty_list(c->T, p->kind == IP_IN ? p->a : inf_var(c))); a = inf_f(c, a); k = c->T->t[a].k;
    }
    switch (p->kind) {
        case IP_INDEX: {
            const CgxTid ix = inf_f(c, p->b);
            if (k == TYK_LIST) { if (!inf_want(c, n->b, ix, c->I)) { INF_TN(ti, ix); inf_err(c, n->line, "a list index must be int, found %s", ti); INF_FREE(ti); } inf_unify(c, p->r, c->T->t[a].a[0]); }
            else if (k == TYK_MAP) { if (!inf_want(c, n->b, ix, c->S)) { INF_TN(ti, ix); inf_err(c, n->line, "a map index must be str, found %s", ti); INF_FREE(ti); } inf_unify(c, p->r, c->T->t[a].a[0]); }
            else if (k == TYK_STR) { if (!inf_want(c, n->b, ix, c->I)) inf_err(c, n->line, "a str index must be int"); inf_unify(c, p->r, c->S); }
            else if (k == TYK_ANY) inf_unify(c, p->r, c->A);
            else { INF_TN(tn, a); inf_err(c, n->line, "cannot index a %s (expected a list, map or str)", tn); INF_FREE(tn); inf_unify(c, p->r, c->A); }
            break;
        }
        case IP_ITER:
            if (k == TYK_LIST) inf_unify(c, p->r, c->T->t[a].a[0]);
            else if (k == TYK_STR || k == TYK_MAP) inf_unify(c, p->r, c->S);
            else if (k == TYK_ANY) inf_unify(c, p->r, c->A);
            else { INF_TN(tn, a); inf_err(c, n->line, "cannot iterate over %s (expected a list, str or map)", tn); INF_FREE(tn); }
            break;
        case IP_IN: case IP_CONTAINS: {
            const CgxTid x = inf_f(c, p->kind == IP_IN ? p->a : p->b); CgxNode *xn = p->kind == IP_IN ? n->a : n->b1.p[1];
            if (k == TYK_LIST) { const CgxTid e = c->T->t[a].a[0]; if (!inf_can(c, e, x) && !inf_can(c, x, e)) { INF_TN(te, e); INF_TN(tx, x); inf_err(c, n->line, "looking for %s in a list of %s can never succeed", tx, te); INF_FREE(te); INF_FREE(tx); } else inf_unify(c, e, x); }
            else if (k == TYK_STR || k == TYK_MAP) { if (!inf_want(c, xn, x, c->S)) { INF_TN(tx, x); inf_err(c, n->line, "`x in %s` needs a str on the left, found %s", k == TYK_STR ? "str" : "map", tx); INF_FREE(tx); } }
            else if (k != TYK_ANY) { INF_TN(tn, a); inf_err(c, n->line, "`in` needs a list, str or map on the right, found %s", tn); INF_FREE(tn); }
            break;
        }
    }
    return 1;
}
static void inf_pend_run(CgxInf *c, int force) {
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < c->npd; i++) if (!c->pd[i].done && inf_pend_try(c, &c->pd[i], force)) { c->pd[i].done = 1; progress = 1; }
        if (force && !progress) break;
    }
    if (force) c->npd = 0; else { int w = 0; for (int i = 0; i < c->npd; i++) if (!c->pd[i].done) c->pd[w++] = c->pd[i]; c->npd = w; }
}

/* ---------------- operadores ---------------- */
static unsigned inf_restrict(CgxInf *c, CgxTid v, unsigned m) {     /* v: variavel livre; devolve a mascara resultante (0 = vazia) */
    const unsigned o = inf_mget(c, v), nm = o ? (o & m) : m; inf_mset(c, v, nm); return nm;
}
static CgxTid inf_arith(CgxInf *c, CgxNode *n, int op, CgxTid a, CgxTid b, CgxNode *na, CgxNode *nb) {
    a = inf_f(c, a); b = inf_f(c, b);
    int ka = c->T->t[a].k, kb = c->T->t[b].k;
    const int isnum_a = ka == TYK_INT || ka == TYK_FLOAT, isnum_b = kb == TYK_INT || kb == TYK_FLOAT;
    const unsigned nummask = INF_NUM, addmask = INF_ORD;
    if (ka == TYK_ANY || kb == TYK_ANY) {
        CgxTid o = ka == TYK_ANY ? b : a; CgxNode *an = ka == TYK_ANY ? na : nb;
        const int ko = c->T->t[inf_f(c, o)].k;
        if (ko == TYK_INT || ko == TYK_FLOAT || ko == TYK_STR) inf_mark(c, an, inf_f(c, o));
        if (op == CGO_ADD && ko == TYK_STR) return c->S;
        return op == CGO_DIV ? c->F : c->A;
    }
    const int ok_op = op == CGO_ADD || op == CGO_SUB || op == CGO_MUL || op == CGO_DIV || op == CGO_IDIV || op == CGO_MOD;
    if (ok_op && (ka == TYK_VAR || kb == TYK_VAR)) {
        unsigned want = op == CGO_ADD ? addmask : nummask;
        if (op == CGO_IDIV || op == CGO_MOD) {
            if (ka == TYK_VAR && !inf_bind(c, a, c->I)) goto bad;
            if (kb == TYK_VAR && !inf_bind(c, inf_f(c, b), c->I)) goto bad;
            a = inf_f(c, a); b = inf_f(c, b); if (c->T->t[a].k != TYK_INT || c->T->t[b].k != TYK_INT) goto bad;
            return c->I;
        }
        if (ka == TYK_VAR && kb == TYK_VAR) {
            if (op == CGO_DIV) { if (!inf_restrict(c, a, want) || !inf_restrict(c, b, want)) goto bad; return c->F; }
            if (!inf_unify(c, a, b)) goto bad;
            a = inf_f(c, a); if (!inf_restrict(c, a, want)) goto bad; return a;
        }
        const CgxTid v = ka == TYK_VAR ? a : b, o = ka == TYK_VAR ? b : a; const int ko = c->T->t[o].k;
        if (ko == TYK_STR && op == CGO_ADD) { if (!inf_bind(c, v, c->S)) goto bad; return c->S; }
        if (ko == TYK_INT || ko == TYK_FLOAT) {
            want = nummask;
            if (!inf_restrict(c, v, want)) goto bad;
            if (op == CGO_DIV) return c->F;
            return ko == TYK_FLOAT ? c->F : v;
        }
        goto bad;
    }
    if (op == CGO_ADD && ka == TYK_STR && kb == TYK_STR) return c->S;
    if (isnum_a && isnum_b) {
        if ((op == CGO_IDIV || op == CGO_MOD) && (ka != TYK_INT || kb != TYK_INT)) goto bad;
        return op == CGO_DIV ? c->F : (ka == TYK_FLOAT || kb == TYK_FLOAT) ? c->F : c->I;
    }
bad: {
        INF_TN(t1, a); INF_TN(t2, b);
        if (op == CGO_ADD) inf_err(c, n->line, "ADD of %s and %s (expected two numbers or two str; convert with str(x))", t1, t2);
        else if (op == CGO_IDIV || op == CGO_MOD) inf_err(c, n->line, "%s needs two int operands, found %s and %s", CGO_NAME[op], t1, t2);
        else inf_err(c, n->line, "%s of %s and %s (expected two numbers)", CGO_NAME[op], t1, t2);
        INF_FREE(t1); INF_FREE(t2);
    }
    return op == CGO_DIV ? c->F : c->A;
}
static void inf_order(CgxInf *c, CgxNode *n, CgxTid a, CgxTid b) {
    a = inf_f(c, a); b = inf_f(c, b); const int ka = c->T->t[a].k, kb = c->T->t[b].k;
    if (ka == TYK_ANY || kb == TYK_ANY) { CgxNode *an = ka == TYK_ANY ? n->a : n->b; const CgxTid o = ka == TYK_ANY ? b : a; const int ko = c->T->t[o].k; if (ko == TYK_INT || ko == TYK_FLOAT || ko == TYK_STR) inf_mark(c, an, o); return; }
    int ok = 1;
    if (ka == TYK_VAR && kb == TYK_VAR) { ok = inf_unify(c, a, b) && inf_restrict(c, inf_f(c, a), INF_ORD); }
    else if (ka == TYK_VAR || kb == TYK_VAR) {
        const CgxTid v = ka == TYK_VAR ? a : b, o = ka == TYK_VAR ? b : a; const int ko = c->T->t[o].k;
        if (ko == TYK_STR) ok = inf_bind(c, v, c->S); else if (ko == TYK_INT || ko == TYK_FLOAT) ok = inf_restrict(c, v, INF_NUM) != 0; else ok = 0;
    } else ok = ((ka == TYK_INT || ka == TYK_FLOAT) && (kb == TYK_INT || kb == TYK_FLOAT)) || (ka == TYK_STR && kb == TYK_STR);
    if (!ok) { INF_TN(t1, a); INF_TN(t2, b); inf_err(c, n->line, "COMPARE %s of %s and %s (expected two numbers or two str)", CGO_SYM[n->op], t1, t2); INF_FREE(t1); INF_FREE(t2); }
}

/* ---------------- expressoes ---------------- */
static CgxTid inf_ex(CgxInf *c, CgxNode *n, CgxTid want);
static CgxTid inf_bool(CgxInf *c, CgxNode *e, const char *what) {
    CgxTid t = inf_ex(c, e, c->B); t = inf_f(c, t); const int k = c->T->t[t].k;
    if (k == TYK_VAR) { inf_bind(c, t, c->B); return c->B; }
    if (k == TYK_ANY) { inf_mark(c, e, c->B); return c->B; }
    if (k != TYK_BOOL) { INF_TN(tn, t); inf_err(c, e->line, "%s must be bool, found %s (truthiness of numbers, strings and lists differs between languages; write the comparison, e.g. `x != 0` or `len(xs) > 0`)", what, tn); INF_FREE(tn); }
    return c->B;
}
/* lista (ou string/mapa, conforme a mascara) como container: devolve o tipo do elemento ou 0 */
static CgxTid inf_as_list(CgxInf *c, CgxTid t) {
    t = inf_f(c, t); const int k = c->T->t[t].k;
    if (k == TYK_LIST) return c->T->t[t].a[0];
    if (k == TYK_ANY) return c->A;
    if (k == TYK_VAR) { const CgxTid e = inf_var(c); if (!inf_bind(c, t, cgx_ty_list(c->T, e))) return 0; return e; }
    return 0;
}
static CgxTid inf_as_map(CgxInf *c, CgxTid t) {
    t = inf_f(c, t); const int k = c->T->t[t].k;
    if (k == TYK_MAP) return c->T->t[t].a[0];
    if (k == TYK_ANY) return c->A;
    if (k == TYK_VAR) { const CgxTid e = inf_var(c); if (!inf_bind(c, t, cgx_ty_map(c->T, e))) return 0; return e; }
    return 0;
}
/* argumento `i` da embutida: tipo permitido pela mascara (var ganha a mascara; any e aceito; opcional exige teste) */
static int inf_single(unsigned m) { for (int k = 0; k < 16; k++) if (m == INF_KM(k)) return k; return -1; }
static int inf_need(CgxInf *c, CgxNode *call, int i, CgxTid t, unsigned mask, const char *what) {
    CgxNode *an = call->b1.p[i];
    t = inf_nn(c, an, t); const int k = c->T->t[t].k;
    const int mk = inf_single(mask), prim = mk == TYK_INT || mk == TYK_FLOAT || mk == TYK_STR || mk == TYK_BOOL;
    if (k == TYK_ANY) { if (prim) inf_mark(c, an, cgx_ty_prim(c->T, mk)); return 1; }
    if (k == TYK_VAR) { if (prim ? inf_bind(c, t, cgx_ty_prim(c->T, mk)) : inf_restrict(c, t, mask) != 0) return 1; }
    else if (mask & INF_KM(k)) return 1;
    INF_TN(tn, t); inf_err(c, call->line, "%s: argument %d expected %s, found %s", cgx_op_by_id(call->bi)->use, i + 1, what, tn); INF_FREE(tn);
    return 0;
}
static CgxTid inf_builtin(CgxInf *c, CgxNode *n, CgxTid *a, int na) {
    const int id = n->bi; (void)na;
    CgxNode **an = n->b1.p;
    switch (id) {
        case CGB_LEN: inf_need(c, n, 0, a[0], INF_KM(TYK_LIST) | INF_KM(TYK_STR) | INF_KM(TYK_MAP), "a list, str or map"); return c->I;
        case CGB_PUSH: {
            const CgxTid xs = inf_nn(c, an[0], a[0]); const CgxTid e = inf_as_list(c, xs);
            if (!e) { INF_TN(tn, xs); inf_err(c, n->line, "%s: argument 1 expected a list, found %s", cgx_op_by_id(id)->use, tn); INF_FREE(tn); return c->V; }
            const InfSite st = {ST_PUSH, 0, NULL, NULL}; inf_sub_at(c, e, a[1], an[1], n->line, &st);
            return c->V;
        }
        case CGB_POP: { const CgxTid e = inf_as_list(c, inf_nn(c, an[0], a[0])); if (!e) { INF_TN(tn, a[0]); inf_err(c, n->line, "%s: argument 1 expected a list, found %s", cgx_op_by_id(id)->use, tn); INF_FREE(tn); return c->A; } return e; }
        case CGB_STR: if (inf_k(c, a[0]) == TYK_VOID) inf_err(c, n->line, "%s: argument 1 expected a value, found void", cgx_op_by_id(id)->use); return c->S;
        case CGB_INT: inf_need(c, n, 0, a[0], INF_NUM | INF_KM(TYK_STR) | INF_KM(TYK_BOOL), "a number or str"); return c->I;
        case CGB_FLOAT: inf_need(c, n, 0, a[0], INF_NUM | INF_KM(TYK_STR), "a number or str"); return c->F;
        case CGB_UPPER: case CGB_LOWER: case CGB_TRIM: inf_need(c, n, 0, a[0], INF_KM(TYK_STR), "a str"); return c->S;
        case CGB_SPLIT: inf_need(c, n, 0, a[0], INF_KM(TYK_STR), "a str"); inf_need(c, n, 1, a[1], INF_KM(TYK_STR), "a str separator"); return cgx_ty_list(c->T, c->S);
        case CGB_WORDS: inf_need(c, n, 0, a[0], INF_KM(TYK_STR), "a str"); return cgx_ty_list(c->T, c->S);
        case CGB_JOIN: {
            const CgxTid e = inf_as_list(c, inf_nn(c, an[0], a[0]));
            if (!e) { INF_TN(tn, a[0]); inf_err(c, n->line, "%s: argument 1 expected a list of str, found %s", cgx_op_by_id(id)->use, tn); INF_FREE(tn); }
            else if (!inf_unify(c, e, c->S)) inf_err(c, n->line, "join: the list must hold str (convert with str(x) first)");
            inf_need(c, n, 1, a[1], INF_KM(TYK_STR), "a str separator"); return c->S;
        }
        case CGB_CONTAINS: {
            const CgxTid x = inf_f(c, inf_nn(c, an[0], a[0])); const int k = c->T->t[x].k;
            if (k == TYK_VAR || k == TYK_LIST || k == TYK_STR || k == TYK_MAP || k == TYK_ANY) inf_pend_add(c, IP_CONTAINS, x, a[1], 0, n);
            else { INF_TN(tn, x); inf_err(c, n->line, "%s: argument 1 expected a str, list or map, found %s", cgx_op_by_id(id)->use, tn); INF_FREE(tn); }
            return c->B;
        }
        case CGB_INDEXOF: inf_need(c, n, 0, a[0], INF_KM(TYK_STR), "a str"); inf_need(c, n, 1, a[1], INF_KM(TYK_STR), "a str"); return c->I;
        case CGB_REPLACE: for (int i = 0; i < 3; i++) inf_need(c, n, i, a[i], INF_KM(TYK_STR), "a str"); return c->S;
        case CGB_STARTS: case CGB_ENDS: inf_need(c, n, 0, a[0], INF_KM(TYK_STR), "a str"); inf_need(c, n, 1, a[1], INF_KM(TYK_STR), "a str"); return c->B;
        case CGB_SLICE: {
            inf_need(c, n, 0, a[0], INF_KM(TYK_STR) | INF_KM(TYK_LIST), "a str or list");
            for (int i = 1; i < na; i++) inf_need(c, n, i, a[i], INF_KM(TYK_INT), "an int");
            return inf_nn(c, an[0], a[0]);
        }
        case CGB_KEYS: { const CgxTid e = inf_as_map(c, inf_nn(c, an[0], a[0])); if (!e) { INF_TN(tn, a[0]); inf_err(c, n->line, "%s: argument 1 expected a map, found %s", cgx_op_by_id(id)->use, tn); INF_FREE(tn); } return cgx_ty_list(c->T, c->S); }
        case CGB_PRINT: if (inf_k(c, a[0]) == TYK_VOID) inf_err(c, n->line, "%s: argument 1 expected a value, found void", cgx_op_by_id(id)->use); return c->V;
        case CGB_ABS: inf_need(c, n, 0, a[0], INF_NUM, "a number"); return inf_nn(c, an[0], a[0]);
        case CGB_MIN: case CGB_MAX: {
            inf_need(c, n, 0, a[0], INF_NUM, "a number"); inf_need(c, n, 1, a[1], INF_NUM, "a number");
            const CgxTid x = inf_f(c, a[0]), y = inf_f(c, a[1]); const int kx = c->T->t[x].k, ky = c->T->t[y].k;
            if (kx == TYK_ANY || ky == TYK_ANY) return c->A;
            if (kx == TYK_VAR && ky == TYK_VAR) { inf_unify(c, x, y); return inf_f(c, x); }
            if (kx == TYK_VAR) return ky == TYK_FLOAT ? c->F : x;
            if (ky == TYK_VAR) return kx == TYK_FLOAT ? c->F : y;
            return kx == TYK_FLOAT || ky == TYK_FLOAT ? c->F : c->I;
        }
        case CGB_SQRT: inf_need(c, n, 0, a[0], INF_NUM, "a number"); return c->F;
        case CGB_FLOOR: inf_need(c, n, 0, a[0], INF_NUM, "a number"); return c->I;
        case CGB_POW: inf_need(c, n, 0, a[0], INF_NUM, "a number"); inf_need(c, n, 1, a[1], INF_NUM, "a number"); return c->F;
        case CGB_SUM: {
            const CgxTid e = inf_as_list(c, inf_nn(c, an[0], a[0]));
            if (!e) { INF_TN(tn, a[0]); inf_err(c, n->line, "%s: argument 1 expected a list of numbers, found %s", cgx_op_by_id(id)->use, tn); INF_FREE(tn); return c->A; }
            const CgxTid ef = inf_f(c, e); const int k = c->T->t[ef].k;
            if (k == TYK_VAR) inf_restrict(c, ef, INF_NUM);
            else if (k != TYK_INT && k != TYK_FLOAT && k != TYK_ANY) { INF_TN(tn, e); inf_err(c, n->line, "%s: argument 1 expected a list of numbers, found list[%s]", cgx_op_by_id(id)->use, tn); INF_FREE(tn); return c->A; }
            return ef;
        }
        case CGB_SORT: {
            const CgxTid e = inf_as_list(c, inf_nn(c, an[0], a[0]));
            if (!e) { INF_TN(tn, a[0]); inf_err(c, n->line, "%s: argument 1 expected a list, found %s", cgx_op_by_id(id)->use, tn); INF_FREE(tn); return c->A; }
            const CgxTid ef = inf_f(c, e); const int k = c->T->t[ef].k;
            if (k == TYK_VAR) inf_restrict(c, ef, INF_ORD);
            else if (k != TYK_INT && k != TYK_FLOAT && k != TYK_STR && k != TYK_ANY) { INF_TN(tn, e); inf_err(c, n->line, "sort: the elements must be numbers or str (they are ordered with `<`), found %s", tn); INF_FREE(tn); }
            return inf_nn(c, an[0], a[0]);
        }
        case CGB_REVERSE: inf_need(c, n, 0, a[0], INF_KM(TYK_LIST) | INF_KM(TYK_STR), "a list or str"); return inf_nn(c, an[0], a[0]);
        case CGB_MATCHES: inf_need(c, n, 0, a[0], INF_KM(TYK_STR), "a str"); inf_need(c, n, 1, a[1], INF_KM(TYK_STR), "a regex str"); return c->B;
        case CGB_ERRMSG: {
            const CgxTid t = inf_f(c, a[0]);
            if (c->T->t[t].k == TYK_VAR) inf_bind(c, t, c->E);
            else if (c->T->t[t].k != TYK_ANY && t != c->E) inf_err(c, n->line, "%s: argument 1 expected the error caught by `catch e`, found %s", cgx_op_by_id(id)->use, inf_kname(c->T->t[t].k));
            return c->S;
        }
    }
    return c->A;
}

static InfUnit *inf_unit_of(CgxInf *c, CgxNode *d) { for (int i = 0; i < c->nu; i++) if (c->u[i].d == d) return &c->u[i]; return NULL; }
static void inf_unit_sig(CgxInf *c, InfUnit *u);
static CgxTid inf_unit_type(CgxInf *c, InfUnit *u) {
    if (!u->sigdone) { const int lv = c->T->level; c->T->level = 1; inf_unit_sig(c, u); c->T->level = lv; }
    return u->has_sch ? inf_instantiate(c, &u->sch) : u->mono;
}
/* chamada de funcao, metodo ou construtor da especificacao: aridade, argumentos contra os parametros (bidirecional), retorno */
static CgxTid inf_call_unit(CgxInf *c, CgxNode *n, InfUnit *u, const char *what) {
    CgxNode *fn = u->d; const CgxTid ft = inf_unit_type(c, u); const int np = c->T->t[ft].n - 1, na = n->b1.n;
    int req = 0; for (int i = 0; i < fn->npar; i++) if (!fn->par[i].def) req = i + 1;
    if (na < req || na > fn->npar) {
        char sig[400]; size_t w = (size_t)snprintf(sig, sizeof sig, "%s(", what);
        for (int i = 0; i < fn->npar && w + 60 < sizeof sig; i++) w += (size_t)snprintf(sig + w, sizeof sig - w, "%s%s%s", i ? ", " : "", fn->par[i].name, fn->par[i].def ? "=..." : "");
        snprintf(sig + w, sizeof sig - w, ")");
        inf_err(c, n->line, "`%s` called with %d argument%s, expected %s%d: %s", what, na, na == 1 ? "" : "s", req == fn->npar ? "" : "from ", req, sig);
    }
    for (int i = 0; i < na; i++) {
        const CgxTid p = i < np ? c->T->t[inf_f(c, ft)].a[i] : 0;
        const CgxTid t = inf_ex(c, n->b1.p[i], p && inf_k(c, p) != TYK_VAR ? p : 0);
        if (p) { const InfSite st = {ST_ARG, i + 1, what, i < fn->npar ? fn->par[i].name : "?"}; inf_sub_at(c, p, t, n->b1.p[i], n->b1.p[i]->line, &st); }
    }
    inf_kill_paths(c);                                                 /* o chamado pode mudar campos de qualquer objeto */
    return c->T->t[inf_f(c, ft)].a[np];
}
static const char *inf_cls_list(CgxInf *c, const char *name, int methods, char *buf, size_t cap) {
    size_t w = 0; buf[0] = 0;
    for (int i = 0; i < c->ncls; i++) {
        CgxNode *d = c->cls[i].d;
        for (int j = 0; j < d->b1.n; j++) { CgxNode *m = d->b1.p[j]; if ((methods ? m->k == CGN_FN : m->k == CGN_FIELDDECL) && !strcmp(m->s, name)) { w += (size_t)snprintf(buf + w, cap - w, "%s%s", w ? ", " : "", d->s); break; } }
        if (w + 40 > cap) break;
    }
    return buf;
}
/* o receptor de .campo / .metodo(): classe conhecida, ou a unica classe que tem o nome, ou erro pedindo anotacao */
static InfCls *inf_recv(CgxInf *c, CgxNode *n, CgxTid rt, int methods) {
    rt = inf_f(c, rt); const int k = c->T->t[rt].k;
    if (rt == c->E) { inf_err(c, n->line, methods ? "an error has no methods: use errmsg(e) for its text" : "an error has no fields: use errmsg(e) for its text"); return NULL; }
    if (k == TYK_CLASS) { InfCls *kl = inf_cls_of(c, rt); if (!kl) inf_err(c, n->line, "unknown class `%s`", cgx_symname(c->g, c->T->t[rt].sym)); return kl; }
    if (k == TYK_VAR) {
        InfCls *found = NULL; int cnt = 0;
        for (int i = 0; i < c->ncls; i++) {
            CgxNode *d = c->cls[i].d;
            for (int j = 0; j < d->b1.n; j++) { CgxNode *m = d->b1.p[j]; if ((methods ? m->k == CGN_FN : m->k == CGN_FIELDDECL) && !strcmp(m->s, n->s)) { found = &c->cls[i]; cnt++; break; } }
        }
        if (cnt == 1) { inf_unify(c, rt, found->tid); return found; }
        char l[300]; inf_cls_list(c, n->s, methods, l, sizeof l);
        if (cnt == 0) inf_err(c, n->line, "no class has a %s named `%s`", methods ? "method" : "field", n->s);
        else inf_err(c, n->line, "cannot tell which class has `.%s` (%s); annotate the parameter with the class name", n->s, l);
        return NULL;
    }
    if (k == TYK_ANY) return NULL;
    if (k == TYK_LIST || k == TYK_STR || k == TYK_MAP) {
        if (methods) inf_err(c, n->line, "`.%s(...)` on a %s: lists, strings and maps have no methods in the UIR; write the function form, e.g. `push(xs, v)`, `upper(s)`, `len(x)`", n->s, inf_kname(k));
        else inf_err(c, n->line, "`.%s` on a %s: lists, strings and maps have no fields; use the functions (len(x), keys(m), ...)", n->s, inf_kname(k));
        return NULL;
    }
    INF_TN(tn, rt); inf_err(c, n->line, methods ? "cannot call a method on %s" : "cannot read a field of %s", tn); INF_FREE(tn);
    return NULL;
}

static CgxTid inf_ex(CgxInf *c, CgxNode *n, CgxTid want) {
    CgxTid r = c->A; int cert = CGC_DERIVED;
    if (want) want = inf_f(c, want);
    switch (n->k) {
        case CGN_INT: r = c->I; break;
        case CGN_FLT: r = c->F; break;
        case CGN_STR: r = c->S; break;
        case CGN_BOOL: r = c->B; break;
        case CGN_NULL: r = want && c->T->t[want].k == TYK_OPT ? want : cgx_ty_opt(c->T, inf_var(c)); break;
        case CGN_VAR: {
            const int sid = cgx_intern(c->g, n->s), y = inf_lookup(c, sid);
            if (y >= 0 && c->cconst && c->sym[y].glob && c->sym[y].decl && c->sym[y].decl != c->cconst && c->sym[y].decl->line > c->cconst->line) inf_err(c, n->line, "constant `%s` is used before its declaration (line %d)", n->s, c->sym[y].decl->line);
            if (y >= 0) { r = c->sym[y].nty ? c->sym[y].nty : c->sym[y].ty; n->ref = c->sym[y].decl; cert = c->sym[y].declared ? CGC_DECLARED : CGC_DERIVED; break; }
            if (sid < c->nh && (c->hfn[sid] || c->hcls[sid])) inf_err(c, n->line, "`%s` is a %s, not a variable: call it as `%s(...)`", n->s, c->hfn[sid] ? "function" : "class", n->s);
            else { const char *sg = inf_suggest(c, n->s); inf_err(c, n->line, "variable `%s` is not declared (declare it with `let %s = ...` first)%s%s%s", n->s, n->s, sg ? "; did you mean `" : "", sg ? sg : "", sg ? "`?" : ""); }
            break;
        }
        case CGN_LIST: {
            const CgxTid ew = want && c->T->t[want].k == TYK_LIST ? c->T->t[want].a[0] : 0; CgxTid acc = 0;
            for (int i = 0; i < n->b1.n; i++) {
                const CgxTid t = inf_ex(c, n->b1.p[i], ew);
                if (ew) { const InfSite st = {ST_ELEM, 0, "list", NULL}; inf_sub_at(c, ew, t, n->b1.p[i], n->b1.p[i]->line, &st); }
                else if (inf_k(c, t) == TYK_VOID) inf_err(c, n->line, "a list element is the result of a call that returns nothing (void)");
                else acc = acc ? inf_join(c, acc, t) : t;
            }
            r = cgx_ty_list(c->T, ew ? ew : acc ? acc : inf_var(c)); break;
        }
        case CGN_MAP: {
            const CgxTid ew = want && c->T->t[want].k == TYK_MAP ? c->T->t[want].a[0] : 0; CgxTid acc = 0;
            for (int i = 0; i + 1 < n->b1.n; i += 2) {
                const CgxTid kt = inf_ex(c, n->b1.p[i], c->S);
                if (!inf_want(c, n->b1.p[i], kt, c->S)) { INF_TN(tn, kt); inf_err(c, n->line, "map keys must be str, found %s (JavaScript objects turn every key into a string)", tn); INF_FREE(tn); }
                const CgxTid t = inf_ex(c, n->b1.p[i + 1], ew);
                if (ew) { const InfSite st = {ST_ELEM, 0, "map", NULL}; inf_sub_at(c, ew, t, n->b1.p[i + 1], n->b1.p[i + 1]->line, &st); }
                else acc = acc ? inf_join(c, acc, t) : t;
            }
            r = cgx_ty_map(c->T, ew ? ew : acc ? acc : inf_var(c)); break;
        }
        case CGN_INDEX: {
            CgxTid a = inf_ex(c, n->a, 0), ix = inf_ex(c, n->b, 0);
            a = inf_nn(c, n->a, a); const int k = inf_k(c, a); const int ki = inf_k(c, ix);
            r = inf_var(c);
            if (k == TYK_VAR) { if (ki == TYK_STR) { inf_bind(c, inf_f(c, a), cgx_ty_map(c->T, r)); inf_pend_add(c, IP_INDEX, a, ix, r, n); inf_pend_run(c, 0); } else inf_pend_add(c, IP_INDEX, a, ix, r, n); }
            else { inf_pend_add(c, IP_INDEX, a, ix, r, n); inf_pend_run(c, 0); }
            break;
        }
        case CGN_FIELD: {
            const CgxTid o = inf_nn(c, n->a, inf_ex(c, n->a, 0));
            InfCls *kl = inf_recv(c, n, o, 0);
            if (!kl) break;
            CgxNode *f = inf_field(kl, n->s);
            if (!f) {
                char fl[300] = ""; size_t w = 0;
                for (int i = 0; i < kl->d->b1.n && w + 40 < sizeof fl; i++) if (kl->d->b1.p[i]->k == CGN_FIELDDECL) w += (size_t)snprintf(fl + w, sizeof fl - w, "%s%s", w ? ", " : "", kl->d->b1.p[i]->s);
                inf_err(c, n->line, "class `%s` has no field `%s` (fields: %s)", kl->d->s, n->s, fl[0] ? fl : "none"); break;
            }
            r = f->tid; n->ref = f; cert = f->has_ty && !(f->flag & 2) ? CGC_DECLARED : CGC_DERIVED;
            if (c->npath && n->a->k == CGN_VAR && c->assign_target != n) { const int y = inf_path_sym(c, n, 0); if (y >= 0 && c->sym[y].nty) r = c->sym[y].nty; }
            break;
        }
        case CGN_UN: {
            if (n->op == CGO_NOT) { inf_bool(c, n->a, "`not` operand"); r = c->B; break; }
            const CgxTid a = inf_nn(c, n->a, inf_ex(c, n->a, 0)); const int k = inf_k(c, a);
            if (k == TYK_VAR) inf_restrict(c, inf_f(c, a), INF_NUM);
            else if (k == TYK_ANY) inf_mark(c, n->a, c->F);
            else if (k != TYK_INT && k != TYK_FLOAT) { INF_TN(tn, a); inf_err(c, n->line, "unary `-` expects a number, found %s", tn); INF_FREE(tn); }
            r = inf_f(c, a); break;
        }
        case CGN_BIN: {
            const int op = n->op;
            if (op == CGO_AND || op == CGO_OR) {
                const CgxTid ta = inf_ex(c, n->a, c->B); (void)ta; InfFact f[8]; const int nf = inf_facts(c, n->a, op == CGO_AND, f, 0, 8);
                InfMark m; inf_push(c, &m); inf_apply(c, f, nf);
                const CgxTid tb = inf_ex(c, n->b, c->B); (void)tb;
                inf_pop(c, &m);
                for (int s = 0; s < 2; s++) {                           /* cada lado: bool, var vira bool, any marcado */
                    CgxNode *x = s ? n->b : n->a; const CgxTid t = inf_f(c, x->tid); const int k = c->T->t[t].k;
                    if (k == TYK_VAR) inf_bind(c, t, c->B); else if (k == TYK_ANY) inf_mark(c, x, c->B);
                    else if (k != TYK_BOOL) { INF_TN(t1, n->a->tid); INF_TN(t2, n->b->tid); inf_err(c, n->line, "`%s` expects bool on both sides, found %s and %s (write the comparison)", CGO_SYM[op], t1, t2); INF_FREE(t1); INF_FREE(t2); break; }
                }
                r = c->B; break;
            }
            const CgxTid a = inf_ex(c, n->a, 0), b = inf_ex(c, n->b, 0);
            if (op == CGO_EQ || op == CGO_NE) {
                const CgxTid fa = inf_f(c, a), fb = inf_f(c, b); const int ka = c->T->t[fa].k, kb = c->T->t[fb].k;
                if (ka == TYK_VAR || kb == TYK_VAR) { if (!inf_unify(c, fa, fb)) { INF_TN(t1, fa); INF_TN(t2, fb); inf_err(c, n->line, "comparing %s with %s is always false (expected the same type on both sides)", t1, t2); INF_FREE(t1); INF_FREE(t2); } }
                else if (!inf_can(c, fa, fb) && !inf_can(c, fb, fa)) { INF_TN(t1, fa); INF_TN(t2, fb); inf_err(c, n->line, "comparing %s with %s is always false (expected the same type on both sides)", t1, t2); INF_FREE(t1); INF_FREE(t2); }
                r = c->B; break;
            }
            if (op == CGO_LT || op == CGO_LE || op == CGO_GT || op == CGO_GE) { inf_order(c, n, inf_nn(c, n->a, a), inf_nn(c, n->b, b)); r = c->B; break; }
            if (op == CGO_IN) {
                const CgxTid xs = inf_nn(c, n->b, b); const int k = inf_k(c, xs);
                if (k == TYK_VAR) inf_pend_add(c, IP_IN, a, xs, 0, n); else { inf_pend_add(c, IP_IN, a, xs, 0, n); inf_pend_run(c, 0); }
                r = c->B; break;
            }
            if (op == CGO_APPROX) { inf_err(c, n->line, "`~=` is only for `test` lines"); r = c->B; break; }
            r = inf_arith(c, n, op, inf_nn(c, n->a, a), inf_nn(c, n->b, b), n->a, n->b); break;
        }
        case CGN_TERN: {
            inf_bool(c, n->a, "the condition of `?:`");
            InfFact fp[8], fnn[8]; const int np = inf_facts(c, n->a, 1, fp, 0, 8), nn = inf_facts(c, n->a, 0, fnn, 0, 8);
            InfMark m; inf_push(c, &m); inf_apply(c, fp, np); const CgxTid a = inf_ex(c, n->b, want); inf_pop(c, &m);
            inf_push(c, &m); inf_apply(c, fnn, nn); const CgxTid b = inf_ex(c, n->c, want); inf_pop(c, &m);
            r = want && c->T->t[want].k != TYK_VAR ? want : inf_join(c, a, b);
            if (want && c->T->t[want].k != TYK_VAR) { const InfSite st = {ST_BRANCH, 0, NULL, NULL}; inf_sub_at(c, want, a, n->b, n->line, &st); inf_sub_at(c, want, b, n->c, n->line, &st); }
            break;
        }
        case CGN_MCALL: {
            const CgxTid o = inf_nn(c, n->a, inf_ex(c, n->a, 0));
            InfCls *kl = inf_recv(c, n, o, 1);
            if (!kl) { for (int i = 0; i < n->b1.n; i++) inf_ex(c, n->b1.p[i], 0); break; }
            InfUnit *m = inf_method(c, kl, n->s);
            if (!m) {
                char ml[300] = ""; size_t w = 0;
                for (int i = 0; i < kl->d->b1.n && w + 40 < sizeof ml; i++) if (kl->d->b1.p[i]->k == CGN_FN && strcmp(kl->d->b1.p[i]->s, "init")) w += (size_t)snprintf(ml + w, sizeof ml - w, "%s%s", w ? ", " : "", kl->d->b1.p[i]->s);
                inf_err(c, n->line, "class `%s` has no method `%s` (methods: %s)", kl->d->s, n->s, ml[0] ? ml : "none");
                for (int i = 0; i < n->b1.n; i++) inf_ex(c, n->b1.p[i], 0);
                break;
            }
            if (!strcmp(m->d->s, "init")) { inf_err(c, n->line, "the constructor is called by writing `%s(...)`, not `.init(...)`", kl->d->s); break; }
            n->ref = m->d; r = inf_call_unit(c, n, m, n->s); cert = m->d->has_ty ? CGC_DECLARED : CGC_DERIVED; break;
        }
        case CGN_CALL: {
            const int sid = cgx_intern(c->g, n->s);
            InfUnit *fu = sid < c->nh && c->hfn[sid] ? &c->u[c->hfn[sid] - 1] : NULL; InfCls *kl = !fu ? inf_cls_by_sid(c, sid) : NULL;
            const int bi = !fu && !kl && sid < c->nh ? c->bi_of[sid] : 0;
            if (fu) { n->ref = fu->d; r = inf_call_unit(c, n, fu, n->s); cert = fu->d->has_ty ? CGC_DECLARED : CGC_DERIVED; }
            else if (kl) {
                InfUnit *init = inf_method(c, kl, "init"); n->ref = kl->d;
                if (init) inf_call_unit(c, n, init, n->s);
                else { for (int i = 0; i < n->b1.n; i++) inf_ex(c, n->b1.p[i], 0); if (n->b1.n) inf_err(c, n->line, "class `%s` has no `init`, so `%s(...)` takes no arguments (found %d)", n->s, n->s, n->b1.n); }
                r = kl->tid;
            } else if (bi) {
                CgxTid a[16]; const int na = n->b1.n > 16 ? 16 : n->b1.n; n->bi = bi;
                for (int i = 0; i < n->b1.n; i++) { const CgxTid t = inf_ex(c, n->b1.p[i], 0); if (i < 16) a[i] = t; }
                if (na < cgx_op_by_id(bi)->lo || na > cgx_op_by_id(bi)->hi) inf_err(c, n->line, "%s called with %d argument%s (expected %s)", n->s, na, na == 1 ? "" : "s", cgx_op_by_id(bi)->use);
                else r = inf_builtin(c, n, a, na);
            } else {
                for (int i = 0; i < n->b1.n; i++) inf_ex(c, n->b1.p[i], 0);
                if (inf_lookup(c, sid) >= 0) inf_err(c, n->line, "`%s` is a variable, not a function", n->s);
                else inf_err(c, n->line, "unknown function `%s` (not defined in the spec and not a built-in; built-ins: %s)", n->s, cgx_reg_names_all());
            }
            break;
        }
        default: inf_err(c, n->line, "internal: unexpected node kind %d in an expression", n->k);
    }
    n->tid = r; n->cert = (unsigned char)cert; c->nexpr++;
    return r;
}

/* ---------------- instrucoes ---------------- */
static void inf_blk(CgxInf *c, CgxBlk *b);
static void inf_end_stmt(CgxInf *c) { if (c->npd) inf_pend_run(c, 0); }
static void inf_stmt(CgxInf *c, CgxNode *s) {
    switch (s->k) {
        case CGN_COMMENT: case CGN_DOC: break;
        case CGN_RAW:
            if (s->defs) { char *d = cgx_dup(c->g, s->defs); for (char *tok = strtok(d, " "); tok; tok = strtok(NULL, " ")) { const int sid = cgx_intern(c->g, tok); inf_bindgrow(c, sid); if (inf_lookup(c, sid) < 0) inf_declare(c, s->line, tok, c->A, s, 0, 0); } }
            s->tid = c->A; s->cert = CGC_ANY; break;
        case CGN_LET: {
            const int annotated = s->has_ty && !(s->flag & 2);
            const CgxTid d = annotated ? inf_from_ty(c, &s->ty) : 0;
            CgxTid t = inf_ex(c, s->a, d), ty;
            if (inf_k(c, t) == TYK_VOID) { inf_err(c, s->line, "`%s` is assigned the result of a call that returns nothing (void)", s->s); t = c->A; }
            if (annotated) {
                const int kd = inf_k(c, d);
                if (s->a->k == CGN_NULL && (kd == TYK_STR || kd == TYK_LIST || kd == TYK_MAP || kd == TYK_CLASS)) ty = cgx_ty_opt(c->T, d);   /* `let x: T = null` declara um T opcional */
                else { const InfSite st = {ST_LET, 0, s->s, NULL}; inf_sub_at(c, d, t, s->a, s->line, &st); ty = d; }
            } else ty = t;
            inf_declare(c, s->line, s->s, ty, s, s->flag & 1, annotated);
            s->tid = ty; s->cert = annotated ? CGC_DECLARED : CGC_DERIVED; break;
        }
        case CGN_ASSIGN: {
            CgxNode *tg = s->a; CgxTid tt; int y = -1;
            if (tg->k == CGN_VAR) {
                y = inf_lookup(c, cgx_intern(c->g, tg->s));
                if (y >= 0) { tt = c->sym[y].ty; tg->tid = tt; tg->ref = c->sym[y].decl; tg->cert = c->sym[y].declared ? CGC_DECLARED : CGC_DERIVED; if (c->sym[y].glob) inf_err(c, s->line, "`%s` is a module-level constant and cannot be reassigned inside a function", tg->s); }
                else tt = inf_ex(c, tg, 0);
            } else { c->assign_target = tg; tt = inf_ex(c, tg, 0); c->assign_target = NULL; }
            const CgxTid vt = inf_ex(c, s->b, tt);
            if (inf_k(c, vt) == TYK_VOID) inf_err(c, s->line, "the value assigned is the result of a call that returns nothing (void)");
            if (s->op < 0) { const InfSite st = {ST_ASSIGN, 0, NULL, NULL}; inf_sub_at(c, tt, vt, s->b, s->line, &st); }
            else {
                const CgxTid cur = tg->k == CGN_VAR ? tg->tid : tt;
                const CgxTid cu = y >= 0 && c->sym[y].nty ? c->sym[y].nty : cur;
                const CgxTid res = inf_arith(c, s, s->op, inf_nn(c, tg, cu), inf_nn(c, s->b, vt), tg, s->b);
                if (inf_k(c, tt) == TYK_INT && inf_k(c, res) == TYK_FLOAT) inf_err(c, s->line, "`%s=` would store a float in an int variable", CGO_SYM[s->op]);
                else { const InfSite st = {ST_ASSIGN, 0, NULL, NULL}; inf_sub_at(c, tt, res, NULL, s->line, &st); }
            }
            if (tg->k == CGN_FIELD && tg->a->k == CGN_VAR) {
                const CgxTid ty = inf_f(c, tt); const CgxTid v = inf_f(c, vt);
                int py = inf_path_sym(c, tg, c->T->t[ty].k == TYK_OPT && s->op < 0);
                if (py >= 0) { inf_kill(c, py); const int kv = c->T->t[v].k; if (c->T->t[ty].k == TYK_OPT && s->op < 0 && kv != TYK_OPT && kv != TYK_VAR && kv != TYK_ANY) inf_narrow(c, py, inf_f(c, c->T->t[ty].a[0])); }
            }
            if (y >= 0) {
                inf_kill(c, y);
                const CgxTid ty = inf_f(c, c->sym[y].ty);
                if (c->T->t[ty].k == TYK_OPT && s->op < 0) { const CgxTid v = inf_f(c, vt); const int kv = c->T->t[v].k; if (kv != TYK_OPT && kv != TYK_VAR && kv != TYK_ANY) inf_narrow(c, y, inf_f(c, c->T->t[ty].a[0])); }
            }
            break;
        }
        case CGN_RET: {
            InfUnit *u = c->cu;
            if (!u) { inf_err(c, s->line, "`return` outside a function"); break; }
            CgxNode *fn = u->d; const CgxTid t = s->a ? inf_ex(c, s->a, u->retfix ? u->ret : 0) : c->V;
            if (u->retfix) {
                const int kr = inf_k(c, u->ret);
                if (kr == TYK_VOID && s->a) inf_err(c, s->line, "`%s` is declared `-> void` but returns a value", fn->s);
                else if (kr != TYK_VOID && !s->a) { INF_TN(tn, u->ret); inf_err(c, s->line, "`return` without a value in `%s`, which declares `-> %s`", fn->s, tn); INF_FREE(tn); }
                else if (s->a) {
                    if (inf_k(c, t) == TYK_VOID) inf_err(c, s->line, "returning the result of a call that returns nothing (void)");
                    else { const InfSite st = {ST_RET, 0, fn->s, NULL}; inf_sub_at(c, u->ret, t, s->a, s->line, &st); }
                }
            } else if (s->a) {
                if (inf_k(c, t) == TYK_VOID) inf_err(c, s->line, "returning the result of a call that returns nothing (void)");
                else { if (c->nret == c->capret) { c->capret = c->capret ? c->capret * 2 : 16; c->rets = (CgxTid *)realloc(c->rets, sizeof(CgxTid) * (size_t)c->capret); c->retn = (CgxNode **)realloc(c->retn, sizeof(CgxNode *) * (size_t)c->capret); } c->rets[c->nret] = t; c->retn[c->nret++] = s; c->nretv++; }
            } else c->nretb++;
            break;
        }
        case CGN_THROW: {
            const CgxTid t = inf_f(c, inf_ex(c, s->a, c->S)); const int k = c->T->t[t].k;
            if (k == TYK_VAR) inf_bind(c, t, c->S);
            else if (k == TYK_ANY) inf_mark(c, s->a, c->S);
            else if (t != c->S && t != c->E) { INF_TN(tn, t); inf_err(c, s->line, "`throw` takes a str (the message) or the error caught by `catch e`, found %s", tn); INF_FREE(tn); }
            break;
        }
        case CGN_BREAK: case CGN_CONT: if (!c->loops) inf_err(c, s->line, "`%s` outside a loop", s->k == CGN_BREAK ? "break" : "continue"); break;
        case CGN_EXPR: inf_ex(c, s->a, 0); break;
        case CGN_IF: {
            inf_bool(c, s->a, "the `if` condition");
            InfFact fp[8], fnn[8]; const int np = inf_facts(c, s->a, 1, fp, 0, 8), nn = inf_facts(c, s->a, 0, fnn, 0, 8);
            InfMark m;
            inf_push(c, &m); inf_apply(c, fp, np); inf_blk(c, &s->b1); inf_pop(c, &m);
            inf_push(c, &m); inf_apply(c, fnn, nn); inf_blk(c, &s->b2); inf_pop(c, &m);
            inf_kill_blk(c, &s->b1); inf_kill_blk(c, &s->b2);
            const int t1 = inf_term(&s->b1), t2 = s->b2.n && inf_term(&s->b2);
            if (t1 != t2) {                                              /* saida antecipada: o outro lado vale para o resto do bloco */
                const InfFact *f = t1 ? fnn : fp; const int nf = t1 ? nn : np;
                for (int i = 0; i < nf; i++) if (!inf_blk_assigns(c, &s->b1, c->sym[f[i].sym].sid) && !inf_blk_assigns(c, &s->b2, c->sym[f[i].sym].sid)) inf_narrow(c, f[i].sym, f[i].ty);
            }
            break;
        }
        case CGN_WHILE: {
            inf_kill_blk(c, &s->b1); inf_kill_paths(c);
            inf_bool(c, s->a, "the `while` condition");
            InfFact fp[8]; const int np = inf_facts(c, s->a, 1, fp, 0, 8);
            InfMark m; inf_push(c, &m); inf_apply(c, fp, np); c->loops++; inf_blk(c, &s->b1); c->loops--; inf_pop(c, &m);
            inf_kill_blk(c, &s->b1); break;
        }
        case CGN_FOR: {
            const CgxTid a = inf_ex(c, s->a, c->I), b = inf_ex(c, s->b, c->I);
            if (!inf_want(c, s->a, a, c->I) || !inf_want(c, s->b, b, c->I)) { INF_TN(t1, a); INF_TN(t2, b); inf_err(c, s->line, "a range loop needs int bounds, found %s..%s", t1, t2); INF_FREE(t1); INF_FREE(t2); }
            if (s->c) inf_ex(c, s->c, c->I);
            inf_kill_blk(c, &s->b1); inf_kill_paths(c);
            InfMark m; inf_push(c, &m); const int y = inf_declare(c, s->line, s->s, c->I, s, 0, 1); (void)y;
            c->loops++; inf_blk(c, &s->b1); c->loops--; inf_pop(c, &m);
            inf_kill_blk(c, &s->b1); break;
        }
        case CGN_EACH: {
            const CgxTid col = inf_nn(c, s->a, inf_ex(c, s->a, 0)); const int k = inf_k(c, col);
            CgxTid kt = c->S, vt = inf_var(c);
            if (s->s2) {
                if (k == TYK_LIST || k == TYK_STR) inf_err(c, s->line, k == TYK_LIST ? "iterating a list gives one variable (`for x in xs`); two variables are for maps" : "iterating a str gives one variable");
                else { const CgxTid e = inf_as_map(c, col); if (!e) { INF_TN(tn, col); inf_err(c, s->line, "cannot iterate over %s (expected a list, str or map)", tn); INF_FREE(tn); } else vt = e; }
            } else { inf_pend_add(c, IP_ITER, col, 0, vt, s); inf_pend_run(c, 0); }
            inf_kill_blk(c, &s->b1); inf_kill_paths(c);
            InfMark m; inf_push(c, &m);
            if (s->s2) { inf_declare(c, s->line, s->s, kt, s, 0, 0); inf_declare(c, s->line, s->s2, vt, s, 0, 0); } else inf_declare(c, s->line, s->s, vt, s, 0, 0);
            c->loops++; inf_blk(c, &s->b1); c->loops--; inf_pop(c, &m);
            inf_kill_blk(c, &s->b1); break;
        }
        case CGN_TRY: {
            InfMark m; inf_kill_blk(c, &s->b1); inf_kill_paths(c);
            inf_push(c, &m); inf_blk(c, &s->b1); inf_pop(c, &m);
            if (s->flag) { inf_push(c, &m); if (s->s2) inf_declare(c, s->line, s->s2, c->E, s, 0, 1); inf_blk(c, &s->b2); inf_pop(c, &m); }
            if (s->iv) { inf_kill_blk(c, &s->b2); inf_push(c, &m); inf_blk(c, &s->b3); inf_pop(c, &m); }
            inf_kill_blk(c, &s->b1); inf_kill_blk(c, &s->b2); break;
        }
        default: inf_err(c, s->line, "internal: unexpected statement kind %d", s->k);
    }
    inf_end_stmt(c);
}
static void inf_blk(CgxInf *c, CgxBlk *b) { for (int i = 0; i < b->n; i++) inf_stmt(c, b->p[i]); }

/* ---------------- unidades: assinatura, corpo, grafo de chamadas ---------------- */
static void inf_unit_sig(CgxInf *c, InfUnit *u) {
    if (u->sigdone) return;
    u->sigdone = 1;
    CgxNode *fn = u->d; if (u->kind == 2) return;
    u->np = fn->npar > 39 ? 39 : fn->npar;
    if (fn->npar > 39) inf_err(c, fn->line, "too many parameters (39 at most for type inference)");
    u->pt = (CgxTid *)cgx_alloc(c->g, sizeof(CgxTid) * (size_t)(u->np + 1));
    for (int i = 0; i < u->np; i++) {
        CgxParam *p = &fn->par[i];
        CgxTid pt = p->has_ty ? inf_from_ty(c, &p->ty) : inf_var(c);
        if (p->def) {
            CgxTid dt = inf_ex(c, p->def, p->has_ty ? pt : 0);
            if (p->def->k == CGN_NULL) { if (inf_k(c, pt) != TYK_OPT && inf_k(c, pt) != TYK_ANY) pt = cgx_ty_opt(c->T, pt); }
            else if (p->has_ty) { const InfSite st = {ST_DEFAULT, 0, p->name, NULL}; inf_sub_at(c, pt, dt, p->def, p->line, &st); }
            else if (!inf_unify(c, pt, dt)) inf_err(c, p->line, "the default of `%s` conflicts with its other uses", p->name);
        }
        u->pt[i] = pt;
    }
    const int init = u->cls && !strcmp(fn->s, "init");
    if (fn->has_ty || init) { CgxTy vt = fn->ty; if (init) vt = cgx_t(CGT_VOID); u->ret = inf_from_ty(c, &vt); u->retfix = 1; }
    else u->ret = inf_var(c);
    u->mono = cgx_ty_fn(c->T, u->pt, u->np, u->ret);
    fn->tid = u->mono;
}
static void inf_unit_body(CgxInf *c, InfUnit *u) {
    CgxNode *fn = u->d; InfMark m; c->cu = u; c->nret = c->nretv = c->nretb = 0; c->loops = 0;
    inf_push(c, &m);
    if (u->cls) { CgxTid st = cgx_ty_class(c->T, cgx_intern(c->g, u->cls->s)); inf_declare(c, fn->line, "self", st, fn, 0, 1); }
    for (int i = 0; i < u->np; i++) {
        int dup = 0; for (int j = 0; j < i; j++) if (!strcmp(fn->par[j].name, fn->par[i].name)) dup = 1;
        if (dup) { inf_err(c, fn->par[i].line, "duplicate parameter `%s` in `%s`", fn->par[i].name, fn->s); continue; }
        inf_declare(c, fn->par[i].line, fn->par[i].name, u->pt[i], fn, 0, fn->par[i].has_ty);
    }
    inf_blk(c, &fn->b1);
    if (!u->retfix) {                                                  /* retorno inferido: juncao de todos os retornos */
        if (c->nretv && c->nretb) inf_err(c, fn->line, "`%s` returns a value on some paths and nothing on others (declare `-> type`, and return a value everywhere)", fn->s);
        if (!c->nretv) inf_unify(c, u->ret, c->V);
        else {
            CgxTid acc = 0;
            for (int i = 0; i < c->nret; i++) { const CgxTid t = inf_f(c, c->rets[i]); if (t == inf_f(c, u->ret) || c->T->t[t].k == TYK_VAR) continue; acc = acc ? inf_join(c, acc, t) : t; }
            if (acc && !inf_unify(c, u->ret, acc)) {
                INF_TN(t1, acc); INF_TN(t2, u->ret);
                inf_err(c, fn->line, "`%s` returns %s here but %s elsewhere (declare `-> type` to fix the contract)", fn->s, t1, t2); INF_FREE(t1); INF_FREE(t2);
            }
            for (int i = 0; i < c->nret; i++) if (c->T->t[inf_f(c, c->rets[i])].k == TYK_VAR) inf_unify(c, c->rets[i], u->ret);
        }
    }
    inf_pop(c, &m);
    c->cu = NULL;
}
static void inf_edges_node(CgxInf *c, InfUnit *u, CgxNode *n);
static void inf_edge(InfUnit *u, int to) { if (u->ne == u->cape) { u->cape = u->cape ? u->cape * 2 : 8; u->e = (int *)realloc(u->e, sizeof(int) * (size_t)u->cape); } u->e[u->ne++] = to; }
static void inf_edges_blk(CgxInf *c, InfUnit *u, CgxBlk *b) { for (int i = 0; i < b->n; i++) inf_edges_node(c, u, b->p[i]); }
static void inf_edges_node(CgxInf *c, InfUnit *u, CgxNode *n) {
    if (!n) return;
    const int sid = n->s ? cgx_intern(c->g, n->s) : 0;
    if (n->k == CGN_CALL && sid < c->nh) {
        if (c->hfn[sid]) inf_edge(u, c->hfn[sid] - 1);
        else if (c->hcls[sid]) { for (int i = 0; i < c->nu; i++) if (c->u[i].kind == 1 && c->u[i].cls == c->cls[c->hcls[sid] - 1].d && !strcmp(c->u[i].d->s, "init")) inf_edge(u, i); }
    } else if (n->k == CGN_MCALL && sid < c->nh) { for (int i = c->hmeth[sid] - 1; i >= 0; i = c->u[i].chain - 1) inf_edge(u, i); }
    else if (n->k == CGN_VAR && sid < c->nh && c->hconst[sid]) inf_edge(u, c->hconst[sid] - 1);
    inf_edges_node(c, u, n->a); inf_edges_node(c, u, n->b); inf_edges_node(c, u, n->c);
    inf_edges_blk(c, u, &n->b1); inf_edges_blk(c, u, &n->b2); inf_edges_blk(c, u, &n->b3);
}
static void inf_tarjan(CgxInf *c, int v) {
    InfUnit *u = &c->u[v]; u->idx = u->low = ++c->tcount; c->tstack[c->tsp++] = v; u->onst = 1;
    for (int i = 0; i < u->ne; i++) {
        const int w = u->e[i];
        if (!c->u[w].idx) { inf_tarjan(c, w); if (c->u[w].low < c->u[v].low) c->u[v].low = c->u[w].low; }
        else if (c->u[w].onst && c->u[w].idx < c->u[v].low) c->u[v].low = c->u[w].idx;
    }
    if (c->u[v].low == c->u[v].idx) {
        const int start = c->nscc; int w;
        do { w = c->tstack[--c->tsp]; c->u[w].onst = 0; c->scc_order[c->nscc++] = w; } while (w != v);
        c->scc_order[c->nscc++] = -1 - start;                          /* marcador do fim do grupo (so para saber o tamanho) */
    }
}

/* passada final: tipos resolvidos, graus de certeza, marcas graduais */
static void inf_fin(CgxInf *c, CgxNode *n, const InfSch *s) {
    if (!n) return;
    if (n->tid) {
        n->tid = inf_zonk(c, n->tid, s);
        if (inf_hasany(c, n->tid)) n->cert = CGC_ANY; else if (!n->cert) n->cert = CGC_DERIVED;
        c->ncert[n->cert]++;
    }
    if (n->chk) n->chk = inf_zonk(c, n->chk, s);
    inf_fin(c, n->a, s); inf_fin(c, n->b, s); inf_fin(c, n->c, s);
    for (int i = 0; i < n->npar; i++) inf_fin(c, n->par[i].def, s);
    CgxBlk *bl[3] = {&n->b1, &n->b2, &n->b3};
    for (int k = 0; k < 3; k++) for (int i = 0; i < bl[k]->n; i++) inf_fin(c, bl[k]->p[i], s);
}

/* ---------------- relatorio ---------------- */
static const char *inf_cname(int cert) { return cert == CGC_ANY ? "[any]" : cert == CGC_DECLARED ? "[declared]" : "[derived]"; }
static void inf_sig_text(CgxInf *c, InfUnit *u, CgxB *o) {
    inf_pr(c, u->d->tid, o, &u->sch);
    for (int i = 0; i < u->sch.nq; i++) if (u->sch.m[i]) {
        cgx_bf(o, "%s'%c in ", i && u->sch.m[i - 1] ? ", " : " where ", 'a' + i % 26);
        int first = 1; for (int k = 0; k < 16; k++) if (u->sch.m[i] & INF_KM(k)) { if (!first) cgx_bs(o, "|"); cgx_bs(o, inf_kname(k)); first = 0; }
    }
}
static void inf_report_node(CgxInf *c, CgxNode *n, const char *fname, int flags, CgxB *o) {
    if (!n) return;
    if (n->k == CGN_LET && n->tid && (flags & 1)) { cgx_bs(o, "let "); if (fname) cgx_bf(o, "%s.", fname); cgx_bf(o, "%s: ", n->s); inf_pr(c, n->tid, o, NULL); cgx_bf(o, " %s\n", inf_cname(n->cert)); }
    if (n->k == CGN_VAR && n->tid && (flags & 2)) { cgx_bs(o, "use "); if (fname) cgx_bf(o, "%s.", fname); cgx_bf(o, "%s@%d: ", n->s, n->line); inf_pr(c, n->tid, o, NULL); cgx_bs(o, "\n"); }
    inf_report_node(c, n->a, fname, flags, o); inf_report_node(c, n->b, fname, flags, o); inf_report_node(c, n->c, fname, flags, o);
    CgxBlk *bl[3] = {&n->b1, &n->b2, &n->b3};
    for (int k = 0; k < 3; k++) for (int i = 0; i < bl[k]->n; i++) inf_report_node(c, bl[k]->p[i], fname, flags, o);
}
static void inf_report(CgxInf *c, int flags, CgxB *o) {
    cgx_bs(o, "-- types (certainty: [declared] written in the spec, [derived] inferred by unification, [any] gradual; never \"exact\") --\n");
    for (int i = 0; i < c->nu; i++) {
        InfUnit *u = &c->u[i];
        if (u->kind == 2) { cgx_bf(o, "const %s: ", u->d->s); inf_pr(c, u->d->tid, o, NULL); cgx_bf(o, " %s\n", inf_cname(u->d->cert)); continue; }
        CgxNode *fn = u->d; int cert = CGC_DECLARED; const CgxTid ft = fn->tid;
        for (int j = 0; j < fn->npar; j++) if (!fn->par[j].has_ty) cert = CGC_DERIVED;
        if (!fn->has_ty && !(u->cls && !strcmp(fn->s, "init"))) cert = CGC_DERIVED;
        if (inf_hasany(c, ft)) cert = CGC_ANY;
        cgx_bf(o, "%s %s%s%s: ", u->cls ? "method" : "fn", u->cls ? u->cls->s : "", u->cls ? "." : "", fn->s); inf_sig_text(c, u, o); cgx_bf(o, " %s\n", inf_cname(cert));
    }
    for (int i = 0; i < c->ncls; i++) {
        CgxNode *d = c->cls[i].d;
        for (int j = 0; j < d->b1.n; j++) if (d->b1.p[j]->k == CGN_FIELDDECL) { CgxNode *f = d->b1.p[j]; cgx_bf(o, "field %s.%s: ", d->s, f->s); inf_pr(c, f->tid, o, NULL); cgx_bf(o, " %s\n", inf_cname(f->cert)); }
    }
    if (flags & 3) for (int i = 0; i < c->nu; i++) { InfUnit *u = &c->u[i]; char nm[160]; snprintf(nm, sizeof nm, "%s%s%s", u->cls ? u->cls->s : "", u->cls ? "." : "", u->d->s); inf_report_node(c, u->kind == 2 ? NULL : u->d, nm, flags, o); }
    for (int i = 0; i < c->nck; i++) { cgx_bf(o, "check line %d: a gradual value must be ", c->ck[i].line); inf_pr(c, c->ck[i].n->chk, o, NULL); cgx_bs(o, " here (marked on the AST, not a runtime test)\n"); }
    cgx_bf(o, "nodes typed: %d  [declared] %d  [derived] %d  [any] %d; gradual checks marked: %d\n", c->ncert[1] + c->ncert[2] + c->ncert[3], c->ncert[CGC_DECLARED], c->ncert[CGC_DERIVED], c->ncert[CGC_ANY], c->nck);
}

/* ---------------- entrada ---------------- */
static int cgx_infer_ex(Cgx *g, CgxTypes *T, int flags, CgxB *rep) {
    CgxInf *c = (CgxInf *)calloc(1, sizeof *c); c->g = g; c->T = T; c->nerr0 = g->nerr_all; c->nerr1 = g->nerr;
    c->I = cgx_ty_prim(T, TYK_INT); c->F = cgx_ty_prim(T, TYK_FLOAT); c->S = cgx_ty_prim(T, TYK_STR); c->B = cgx_ty_prim(T, TYK_BOOL);
    c->V = cgx_ty_prim(T, TYK_VOID); c->A = cgx_ty_prim(T, TYK_ANY);
    c->sidErr = cgx_intern(g, "Error"); c->E = cgx_ty_class(T, c->sidErr);
    CgxNode *pr = g->prog; T->level = 0;
    /* tabelas por simbolo: funcoes, metodos, constantes, classes e embutidas */
    cgx_reg_init();
    for (int b = 1; b <= cgx_nops; b++) cgx_intern(g, cgx_op_by_id(b)->name);
    for (int i = 0; i < pr->b1.n; i++) { CgxNode *d = pr->b1.p[i]; if (d->k == CGN_FN || d->k == CGN_CLASS || d->k == CGN_LET) cgx_intern(g, d->s); if (d->k == CGN_CLASS) for (int j = 0; j < d->b1.n; j++) if (d->b1.p[j]->s) cgx_intern(g, d->b1.p[j]->s); }
    c->nh = g->nsyms + 64;
    c->hfn = (int *)calloc((size_t)c->nh, sizeof(int)); c->hmeth = (int *)calloc((size_t)c->nh, sizeof(int)); c->hconst = (int *)calloc((size_t)c->nh, sizeof(int));
    c->hcls = (int *)calloc((size_t)c->nh, sizeof(int)); c->bi_of = (int *)calloc((size_t)c->nh, sizeof(int));
    for (int b = 1; b <= cgx_nops; b++) c->bi_of[cgx_intern(g, cgx_op_by_id(b)->name)] = b;
    inf_bindgrow(c, c->nh);
    int ncu = 0; for (int i = 0; i < pr->b1.n; i++) { CgxNode *d = pr->b1.p[i]; ncu += d->k == CGN_FN || (d->k == CGN_LET && (d->flag & 1)) || d->k == CGN_LET; if (d->k == CGN_CLASS) for (int j = 0; j < d->b1.n; j++) ncu += d->b1.p[j]->k == CGN_FN; }
    c->u = (InfUnit *)calloc((size_t)ncu + 1, sizeof(InfUnit));
    c->cls = (InfCls *)calloc((size_t)pr->b1.n + 1, sizeof(InfCls));
    for (int i = 0; i < pr->b1.n; i++) {
        CgxNode *d = pr->b1.p[i];
        if (d->k == CGN_FN) {
            const int sid = cgx_intern(g, d->s); InfUnit *u = &c->u[c->nu++]; u->d = d; u->kind = 0;
            if (c->hfn[sid]) inf_err(c, d->line, "`%s` is defined twice", d->s); else c->hfn[sid] = c->nu;
            for (int b = 1; b <= cgx_nops; b++) if (!strcmp(cgx_op_by_id(b)->name, d->s)) inf_err(c, d->line, "function `%s` has the name of a built-in (rename it; built-ins: %s)", d->s, cgx_reg_names_all());
        } else if (d->k == CGN_LET) { const int sid = cgx_intern(g, d->s); InfUnit *u = &c->u[c->nu++]; u->d = d; u->kind = 2; if (!c->hconst[sid]) c->hconst[sid] = c->nu; }
        else if (d->k == CGN_CLASS) {
            const int sid = cgx_intern(g, d->s);
            if (c->hcls[sid] || c->hfn[sid]) { inf_err(c, d->line, "`%s` is defined twice", d->s); continue; }
            c->cls[c->ncls] = (InfCls){d, sid, cgx_ty_class(T, sid)}; c->hcls[sid] = ++c->ncls;
            d->tid = c->cls[c->ncls - 1].tid; d->cert = CGC_DECLARED;
            for (int j = 0; j < d->b1.n; j++) if (d->b1.p[j]->k == CGN_FN) {
                CgxNode *m = d->b1.p[j]; if (!strcmp(m->s, "constructor") || !strcmp(m->s, "__init__")) m->s = cgx_dup(g, "init");
                const int ms = cgx_intern(g, m->s); InfUnit *u = &c->u[c->nu++]; u->d = m; u->kind = 1; u->cls = d; u->chain = c->hmeth[ms]; c->hmeth[ms] = c->nu;
            }
        }
    }
    /* campos: tipo escrito, ou o do padrao, ou variavel fraca (nivel 0) fixada pelos usos */
    for (int i = 0; i < c->ncls; i++) {
        CgxNode *d = c->cls[i].d;
        for (int j = 0; j < d->b1.n; j++) {
            CgxNode *f = d->b1.p[j]; if (f->k != CGN_FIELDDECL) continue;
            const int annotated = f->has_ty && !(f->flag & 2);
            const CgxTid want = annotated ? inf_from_ty(c, &f->ty) : 0;
            if (f->a) {
                const CgxTid t = inf_ex(c, f->a, want);
                const int kd = annotated ? inf_k(c, want) : 0;
                if (annotated && f->a->k == CGN_NULL && (kd == TYK_STR || kd == TYK_LIST || kd == TYK_MAP || kd == TYK_CLASS)) f->tid = cgx_ty_opt(c->T, want);   /* `field next: Node = null` e um Node opcional */
                else if (annotated) { const InfSite st = {ST_FIELD, 0, f->s, NULL}; inf_sub_at(c, want, t, f->a, f->line, &st); f->tid = want; } else f->tid = t;
            } else f->tid = annotated ? want : inf_var(c);
            f->cert = annotated ? CGC_DECLARED : CGC_DERIVED;
        }
    }
    /* grafo de chamadas e SCCs */
    c->tstack = (int *)malloc(sizeof(int) * (size_t)(c->nu + 1)); c->scc_order = (int *)malloc(sizeof(int) * (size_t)(2 * c->nu + 2));
    for (int i = 0; i < c->nu; i++) { InfUnit *u = &c->u[i]; if (u->kind == 2) inf_edges_node(c, u, u->d->a); else { inf_edges_blk(c, u, &u->d->b1); for (int j = 0; j < u->d->npar; j++) inf_edges_node(c, u, u->d->par[j].def); } }
    for (int i = 0; i < c->nu; i++) if (!c->u[i].idx) inf_tarjan(c, i);
    /* cada grupo: assinaturas mono, corpos, generalizacao */
    int gs = 0;
    for (int p = 0; p < c->nscc; p++) {
        if (c->scc_order[p] >= 0) continue;
        const int first = -1 - c->scc_order[p], cnt = p - first;   /* membros: scc_order[first .. p-1] */
        qsort(c->scc_order + first, (size_t)cnt, sizeof(int), cgx_ty_cmp);
        T->level = 1;
        for (int q = first; q < p; q++) { InfUnit *u = &c->u[c->scc_order[q]]; if (u->kind != 2) inf_unit_sig(c, u); }
        for (int q = first; q < p; q++) {                               /* ordem de declaracao dentro do grupo */
            InfUnit *u = &c->u[c->scc_order[q]];
            if (u->kind == 2) {
                if (cnt > 1) inf_err(c, u->d->line, "constant `%s` depends on a function that depends on it", u->d->s);
                c->cu = NULL; c->cconst = u->d; inf_stmt(c, u->d); c->cconst = NULL; inf_lower(c, u->d->tid, 0);
            } else inf_unit_body(c, u);
        }
        inf_pend_run(c, 1);
        T->level = 0;
        for (int q = first; q < p; q++) {
            InfUnit *u = &c->u[c->scc_order[q]]; if (u->kind == 2) continue;
            u->sch.t = inf_gen_walk(c, u->mono, &u->sch); u->has_sch = 1; u->done = 1;
        }
        gs++;
    }
    (void)gs;
    /* testes */
    for (int i = 0; i < pr->b1.n; i++) {
        CgxNode *d = pr->b1.p[i]; if (d->k != CGN_TEST) continue;
        const CgxTid a = inf_ex(c, d->a, 0);
        if (d->iv == 2 && d->a->k != CGN_CALL && d->a->k != CGN_MCALL) inf_err(c, d->line, "`test ... raises` needs a call (expected `test f(x) raises`)");
        if (d->b) {
            const CgxTid b = inf_ex(c, d->b, a);
            if (!inf_can(c, a, b) && !inf_can(c, b, a)) { INF_TN(t1, a); INF_TN(t2, b); inf_err(c, d->line, "the test compares %s with %s", t1, t2); INF_FREE(t1); INF_FREE(t2); }
            else inf_unify(c, a, b);
        } else if (d->iv == 3) { const CgxTid t = inf_f(c, a); if (c->T->t[t].k == TYK_VAR) inf_bind(c, t, c->B); else if (c->T->t[t].k != TYK_ANY && c->T->t[t].k != TYK_BOOL) inf_err(c, d->line, "a `test` without `==` must be a bool expression"); }
        inf_end_stmt(c);
    }
    inf_pend_run(c, 1);
    /* anotacao final da AST */
    for (int i = 0; i < c->nu; i++) {
        InfUnit *u = &c->u[i];
        if (u->kind == 2) { inf_fin(c, u->d, NULL); continue; }
        { int cert = CGC_DECLARED; for (int j = 0; j < u->d->npar; j++) if (!u->d->par[j].has_ty) cert = CGC_DERIVED; if (!u->d->has_ty) cert = CGC_DERIVED; u->d->cert = (unsigned char)cert; }
        inf_fin(c, u->d, &u->sch);
        u->d->tid = u->sch.t = inf_zonk(c, u->sch.t, &u->sch);
    }
    for (int i = 0; i < c->ncls; i++) { CgxNode *d = c->cls[i].d; for (int j = 0; j < d->b1.n; j++) if (d->b1.p[j]->k == CGN_FIELDDECL) { CgxNode *f = d->b1.p[j]; f->tid = inf_zonk(c, f->tid, NULL); if (inf_hasany(c, f->tid)) f->cert = CGC_ANY; inf_fin(c, f->a, NULL); } }
    for (int i = 0; i < pr->b1.n; i++) if (pr->b1.p[i]->k == CGN_TEST || pr->b1.p[i]->k == CGN_RAW) inf_fin(c, pr->b1.p[i], NULL);
    if (rep) inf_report(c, flags, rep);
    for (int i = c->nerr1 + 1; i < g->nerr; i++) { CgxErr t = g->err[i]; int j = i - 1; while (j >= c->nerr1 && g->err[j].line > t.line) { g->err[j + 1] = g->err[j]; j--; } g->err[j + 1] = t; }
    const int ok = g->nerr_all == c->nerr0;
    for (int i = 0; i < c->nu; i++) free(c->u[i].e);
    free(c->u); free(c->cls); free(c->sym); free(c->bind); free(c->tr); free(c->pd); free(c->ck); free(c->rets); free(c->retn); free(c->mask);
    free(c->hfn); free(c->hmeth); free(c->hconst); free(c->hcls); free(c->bi_of); free(c->tstack); free(c->scc_order);
    free(c);
    return ok;
}
static int cgx_infer(Cgx *g, CgxTypes *T) { return cgx_infer_ex(g, T, 0, NULL); }
/* o validador chama a inferencia quando pedido: primeiro as regras da fase 1, depois os tipos (mesmos erros, tipos mais precisos) */
static int cgx_check_ex(Cgx *g, CgxTypes *T, int infer) {
    int ok = cgx_check(g);
    if (ok && infer && T) ok = cgx_infer(g, T);
    return ok;
}
