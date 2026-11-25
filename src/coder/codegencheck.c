/* ---------- coder/codegencheck.c: code generation module, parte 2: validador semantico da UIR (docs/codegen.md) ----------
 * Roda sobre a AST ja montada por codegen.c, ANTES de gerar. Cada erro leva a linha e o que se esperava. Regras que valem para qualquer
 * linguagem de destino (e por isso o codigo gerado se comporta igual nelas): variavel declarada antes do uso, nada de redeclarar nem
 * sombrear um nome visivel (Python e JS divergem nisso), condicao so booleana (a verdade de 0, "" e [] diverge), tipos obvios
 * incompativeis, aridade, retorno em todos os caminhos, operacao e funcao conhecidas. Os blocos `raw` sao opacos: so os nomes que
 * declaram em `defines` entram no escopo. */

/* as funcoes embutidas vivem no registro de operacoes (codegenreg.c): aridade, tipos dos argumentos e do resultado, efeito, nivel */
typedef struct { char name[64]; CgxTy ty; CgxNode *decl; int depth, glob; } CgxSym;
typedef struct {
    Cgx *g;
    CgxSym sym[600]; int nsym, depth;
    CgxNode *fns[4096]; int nfn; CgxNode *cls[40]; int ncls;
    CgxNode *fn, *curcls;
    int loops, ret_val, ret_void, quiet, pass;
    CgxTy ret;
    /* o que a especificacao usa (relatorio) */
    int nfunc, nclass, ntest, nraw, ndiv, nstrnum, nops[CGO_N], nloop, ntry, neach, nmap, nlist, nstr, nbi[CGX_OP_MAX + 1], raw_ret;
} CgxV;

static CgxNode *cgx_find_fn(CgxV *v, const char *n) { for (int i = 0; i < v->nfn; i++) if (!strcmp(v->fns[i]->s, n)) return v->fns[i]; return NULL; }
static CgxNode *cgx_find_cls(CgxV *v, const char *n) { for (int i = 0; i < v->ncls; i++) if (!strcmp(v->cls[i]->s, n)) return v->cls[i]; return NULL; }
static CgxNode *cgx_cls_field(CgxNode *c, const char *n) { for (int i = 0; i < c->b1.n; i++) if (c->b1.p[i]->k == CGN_FIELDDECL && !strcmp(c->b1.p[i]->s, n)) return c->b1.p[i]; return NULL; }
static CgxNode *cgx_cls_method(CgxNode *c, const char *n) { for (int i = 0; i < c->b1.n; i++) if (c->b1.p[i]->k == CGN_FN && !strcmp(c->b1.p[i]->s, n)) return c->b1.p[i]; return NULL; }
static void cgx_verr(CgxV *v, int line, const char *fmt, ...) {
    char m[300]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    if (v->quiet) return;
    cgx_err(v->g, line, "%s", m);
}
static CgxSym *cgx_lookup(CgxV *v, const char *n) { for (int i = v->nsym - 1; i >= 0; i--) if (!strcmp(v->sym[i].name, n)) return &v->sym[i]; return NULL; }
static void cgx_known_names(CgxV *v, char *o, size_t cap) {
    size_t w = 0; o[0] = 0; int k = 0;
    for (int i = v->nsym - 1; i >= 0 && k < 8; i--) { w += (size_t)snprintf(o + w, cap - w, "%s%s", k ? ", " : "", v->sym[i].name); k++; if (w + 70 > cap) break; }
}
/* declara um nome: erra se ja visivel (sem redeclarar nem sombrear) */
static CgxSym *cgx_declare(CgxV *v, int line, const char *name, CgxTy ty, CgxNode *decl, int glob) {
    CgxSym *o = cgx_lookup(v, name);
    if (o) {
        cgx_verr(v, line, "`%s` is already declared%s%s (line %d): a name cannot be declared twice or shadow a visible one, because Python and JavaScript scope it differently; use another name or assign without `let`",
                 name, o->glob ? " at module level" : o->depth == v->depth ? " in this block" : " in an outer block", "", o->decl ? o->decl->line : 0);
        return o;
    }
    if (v->nsym >= 600) { cgx_verr(v, line, "too many names in scope (600 at most)"); return &v->sym[v->nsym - 1]; }
    CgxSym *s = &v->sym[v->nsym++]; memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name); s->ty = ty; s->decl = decl; s->depth = v->depth; s->glob = glob;
    return s;
}
static void cgx_push(CgxV *v, int *mark) { *mark = v->nsym; v->depth++; }
static void cgx_pop(CgxV *v, int mark) { v->nsym = mark; v->depth--; }

static CgxTy cgx_ex(CgxV *v, CgxNode *n);
static void cgx_blk(CgxV *v, CgxBlk *b);

static int cgx_cond(CgxV *v, CgxNode *c, const char *what) {
    CgxTy t = cgx_ex(v, c);
    if (t.k != CGT_BOOL && t.k != CGT_ANY) { char tn[60]; cgx_tyname(&t, tn, sizeof tn); cgx_verr(v, c->line, "%s must be bool, found %s (truthiness of numbers, strings and lists differs between languages; write the comparison, e.g. `x != 0` or `len(xs) > 0`)", what, tn); return 0; }
    return 1;
}
static CgxTy cgx_elem(const CgxTy *t) { return cgx_t(t->e); }
static CgxTy cgx_mklist(int e) { CgxTy t = cgx_t(CGT_LIST); t.e = e; return t; }

/* uniao de tipos dos elementos de uma lista literal */
static int cgx_unify(int a, int b) { if (a == b) return a; if ((a == CGT_INT && b == CGT_FLOAT) || (a == CGT_FLOAT && b == CGT_INT)) return CGT_FLOAT; return CGT_ANY; }

static CgxTy cgx_builtin(CgxV *v, CgxNode *c, CgxTy *a, int na) {
    const CgxOp *op = cgx_op_by_id(c->bi); char tn[60];
    const int id = c->bi;
    v->nbi[id]++;
    /* tipos dos argumentos: tabela do registro (qualquer tipo com `any`; `any` passa sempre) */
    for (int i = 0; i < na && i < op->narg && i < 4; i++) {
        const unsigned m = op->arg[i].mask; if (!m || a[i].k == CGT_ANY) continue;
        int bad = !(m & (1u << a[i].k));
        if (id == CGB_ERRMSG) bad = !(a[0].k == CGT_CLASS && !strcmp(a[0].cls, "Error"));
        const char *what = op->arg[i].what;
        if (id == CGB_CONTAINS && i == 1) { bad = (a[0].k == CGT_STR || a[0].k == CGT_MAP) && a[1].k != CGT_STR; what = "a str"; }
        if (bad) { cgx_tyname(&a[i], tn, sizeof tn); cgx_verr(v, c->line, "%s: argument %d expected %s, found %s", op->use, i + 1, what, tn); }
    }
    /* verificacoes que dependem de mais de um argumento (ganchos das embutidas) */
    switch (id) {
        case CGB_PUSH:
            if (a[0].k == CGT_LIST) {
                if (a[0].e == CGT_ANY) {                                        /* a primeira inclusao fixa o tipo do elemento da variavel */
                    if (a[1].k != CGT_ANY && a[1].k != CGT_LIST && a[1].k != CGT_MAP && a[1].k != CGT_CLASS && a[1].k != CGT_NULL && c->b1.p[0]->k == CGN_VAR && c->b1.p[0]->ref) { c->b1.p[0]->ref->ty.e = a[1].k; c->b1.p[0]->ref->ty.k = CGT_LIST; CgxSym *s = cgx_lookup(v, c->b1.p[0]->s); if (s) s->ty.e = a[1].k; }
                } else { CgxTy el = cgx_t(a[0].e); if (!cgx_assignable(&el, &a[1])) { char t2[60]; cgx_tyname(&a[1], tn, sizeof tn); cgx_tyname(&el, t2, sizeof t2); cgx_verr(v, c->line, "push: the list holds %s but the value is %s", t2, tn); } }
            }
            break;
        case CGB_STR: if (a[0].k == CGT_FLOAT || a[0].k == CGT_BOOL) v->nstrnum++; break;
        case CGB_JOIN: if (a[0].k == CGT_LIST && a[0].e != CGT_ANY && a[0].e != CGT_STR) cgx_verr(v, c->line, "join: the list must hold str (convert with str(x) first)"); break;
        default: break;
    }
    /* tipo do resultado */
    switch (op->rk) {
        case CGR_FIXED: { CgxTy r = cgx_t(op->rkind); r.e = op->relem; return r; }
        case CGR_ARG0: return a[0].k == CGT_ANY ? cgx_t(CGT_ANY) : a[0];
        case CGR_ELEM0: return a[0].k == CGT_LIST ? cgx_elem(&a[0]) : cgx_t(CGT_ANY);
        case CGR_NUM0: return a[0].k == CGT_ANY ? cgx_t(CGT_ANY) : a[0];
        case CGR_NUM01: return a[0].k == CGT_ANY || a[1].k == CGT_ANY ? cgx_t(CGT_ANY) : (a[0].k == CGT_FLOAT || a[1].k == CGT_FLOAT) ? cgx_t(CGT_FLOAT) : cgx_t(CGT_INT);
        case CGR_SUM0: return a[0].k == CGT_LIST && (a[0].e == CGT_INT || a[0].e == CGT_FLOAT) ? cgx_t(a[0].e) : cgx_t(CGT_ANY);
    }
    return cgx_t(CGT_ANY);
}

/* confere os argumentos de uma funcao ou metodo da especificacao */
static void cgx_check_args(CgxV *v, CgxNode *c, CgxNode *fn, const char *what, CgxTy *a, int na) {
    int req = 0; for (int i = 0; i < fn->npar; i++) if (!fn->par[i].def) req = i + 1;
    if (na < req || na > fn->npar) {
        char sig[400]; size_t w = 0; w += (size_t)snprintf(sig, sizeof sig, "%s(", what);
        for (int i = 0; i < fn->npar && w + 60 < sizeof sig; i++) w += (size_t)snprintf(sig + w, sizeof sig - w, "%s%s%s", i ? ", " : "", fn->par[i].name, fn->par[i].def ? "=..." : "");
        snprintf(sig + w, sizeof sig - w, ")");
        cgx_verr(v, c->line, "`%s` called with %d argument%s, expected %s%d%s: %s", what, na, na == 1 ? "" : "s", req == fn->npar ? "" : "from ", req, req == fn->npar ? "" : (fn->npar == req + 1 ? "" : ""), sig);
        if (req != fn->npar) { /* ja disse o minimo; completa o maximo */ }
    }
    for (int i = 0; i < na && i < fn->npar; i++) if (fn->par[i].has_ty && !cgx_assignable(&fn->par[i].ty, &a[i])) {
        char t1[60], t2[60]; cgx_tyname(&fn->par[i].ty, t1, sizeof t1); cgx_tyname(&a[i], t2, sizeof t2);
        cgx_verr(v, c->line, "`%s` argument %d (`%s`) expects %s, found %s", what, i + 1, fn->par[i].name, t1, t2);
    }
}

static CgxTy cgx_ex(CgxV *v, CgxNode *n) {
    CgxTy r = cgx_t(CGT_ANY), tn_; (void)tn_;
    char t1[60], t2[60];
    switch (n->k) {
        case CGN_INT: r = cgx_t(CGT_INT); break;
        case CGN_FLT: r = cgx_t(CGT_FLOAT); break;
        case CGN_STR: r = cgx_t(CGT_STR); v->nstr++; break;
        case CGN_BOOL: r = cgx_t(CGT_BOOL); break;
        case CGN_NULL: r = cgx_t(CGT_NULL); break;
        case CGN_VAR: {
            CgxSym *s = cgx_lookup(v, n->s);
            if (s) { r = s->ty; n->ref = s->decl; break; }
            if (cgx_find_fn(v, n->s) || cgx_find_cls(v, n->s)) cgx_verr(v, n->line, "`%s` is a %s, not a variable: call it as `%s(...)`", n->s, cgx_find_fn(v, n->s) ? "function" : "class", n->s);
            else { char kn[400]; cgx_known_names(v, kn, sizeof kn); cgx_verr(v, n->line, "variable `%s` is not declared (declare it with `let %s = ...` first)%s%s", n->s, n->s, kn[0] ? "; names in scope: " : "", kn); }
            break;
        }
        case CGN_LIST: {
            int e = -1; v->nlist++;
            for (int i = 0; i < n->b1.n; i++) { CgxTy t = cgx_ex(v, n->b1.p[i]); const int k = (t.k == CGT_LIST || t.k == CGT_MAP || t.k == CGT_CLASS || t.k == CGT_NULL) ? CGT_ANY : t.k; e = e < 0 ? k : cgx_unify(e, k); }
            r = cgx_mklist(e < 0 ? CGT_ANY : e); break;
        }
        case CGN_MAP: {
            int e = -1; v->nmap++;
            for (int i = 0; i + 1 < n->b1.n; i += 2) {
                CgxTy kt = cgx_ex(v, n->b1.p[i]); CgxTy t = cgx_ex(v, n->b1.p[i + 1]);
                if (kt.k != CGT_STR && kt.k != CGT_ANY) { cgx_tyname(&kt, t1, sizeof t1); cgx_verr(v, n->line, "map keys must be str, found %s (JavaScript objects turn every key into a string)", t1); }
                const int k = (t.k == CGT_LIST || t.k == CGT_MAP || t.k == CGT_CLASS || t.k == CGT_NULL) ? CGT_ANY : t.k; e = e < 0 ? k : cgx_unify(e, k);
            }
            r = cgx_t(CGT_MAP); r.e = e < 0 ? CGT_ANY : e; break;
        }
        case CGN_INDEX: {
            CgxTy a = cgx_ex(v, n->a), i = cgx_ex(v, n->b);
            if (a.k == CGT_LIST) { if (i.k != CGT_INT && i.k != CGT_ANY) { cgx_tyname(&i, t1, sizeof t1); cgx_verr(v, n->line, "a list index must be int, found %s", t1); } r = cgx_elem(&a); }
            else if (a.k == CGT_MAP) { if (i.k != CGT_STR && i.k != CGT_ANY) { cgx_tyname(&i, t1, sizeof t1); cgx_verr(v, n->line, "a map index must be str, found %s", t1); } r = cgx_elem(&a); }
            else if (a.k == CGT_STR) { if (i.k != CGT_INT && i.k != CGT_ANY) cgx_verr(v, n->line, "a str index must be int"); r = cgx_t(CGT_STR); }
            else if (a.k != CGT_ANY) { cgx_tyname(&a, t1, sizeof t1); cgx_verr(v, n->line, "cannot index a %s (expected a list, map or str)", t1); }
            break;
        }
        case CGN_FIELD: {
            CgxTy a = cgx_ex(v, n->a);
            if (a.k == CGT_CLASS) {
                CgxNode *c = cgx_find_cls(v, a.cls);
                if (!c) { if (strcmp(a.cls, "Error")) cgx_verr(v, n->line, "unknown class `%s`", a.cls); else cgx_verr(v, n->line, "an error has no fields: use errmsg(e) for its text"); break; }
                CgxNode *f = cgx_cls_field(c, n->s);
                if (!f) { char fl[300] = ""; size_t w = 0; for (int i = 0; i < c->b1.n && w + 40 < sizeof fl; i++) if (c->b1.p[i]->k == CGN_FIELDDECL) w += (size_t)snprintf(fl + w, sizeof fl - w, "%s%s", w ? ", " : "", c->b1.p[i]->s); cgx_verr(v, n->line, "class `%s` has no field `%s` (fields: %s)", c->s, n->s, fl[0] ? fl : "none"); break; }
                r = f->ty; n->ref = f;
            } else if (a.k == CGT_LIST || a.k == CGT_STR || a.k == CGT_MAP) { cgx_verr(v, n->line, "`.%s` on a %s: lists, strings and maps have no fields; use the functions (len(x), keys(m), ...)", n->s, a.k == CGT_LIST ? "list" : a.k == CGT_STR ? "str" : "map"); }
            else if (a.k != CGT_ANY) { cgx_tyname(&a, t1, sizeof t1); cgx_verr(v, n->line, "cannot read a field of %s", t1); }
            break;
        }
        case CGN_UN: {
            CgxTy a = cgx_ex(v, n->a); v->nops[n->op]++;
            if (n->op == CGO_NOT) { if (a.k != CGT_BOOL && a.k != CGT_ANY) { cgx_tyname(&a, t1, sizeof t1); cgx_verr(v, n->line, "`not` expects bool, found %s (write the comparison)", t1); } r = cgx_t(CGT_BOOL); }
            else { if (!cgx_isnum(&a) && a.k != CGT_ANY) { cgx_tyname(&a, t1, sizeof t1); cgx_verr(v, n->line, "unary `-` expects a number, found %s", t1); } r = a; }
            break;
        }
        case CGN_BIN: {
            CgxTy a = cgx_ex(v, n->a), b = cgx_ex(v, n->b); const int op = n->op; v->nops[op]++;
            const int anyab = a.k == CGT_ANY || b.k == CGT_ANY;
            cgx_tyname(&a, t1, sizeof t1); cgx_tyname(&b, t2, sizeof t2);
            switch (op) {
                case CGO_ADD:
                    if (a.k == CGT_STR && b.k == CGT_STR) r = a;
                    else if (cgx_isnum(&a) && cgx_isnum(&b)) r = cgx_t(a.k == CGT_FLOAT || b.k == CGT_FLOAT ? CGT_FLOAT : CGT_INT);
                    else if (anyab) r = a.k == CGT_STR || b.k == CGT_STR ? cgx_t(CGT_STR) : cgx_t(CGT_ANY);
                    else cgx_verr(v, n->line, "ADD of %s and %s (expected two numbers or two str; convert with str(x))", t1, t2);
                    break;
                case CGO_SUB: case CGO_MUL: case CGO_DIV: case CGO_IDIV: case CGO_MOD:
                    if (op == CGO_DIV || op == CGO_IDIV || op == CGO_MOD) { if (!(n->b->k == CGN_INT || n->b->k == CGN_FLT)) v->ndiv++; }
                    if (cgx_isnum(&a) && cgx_isnum(&b)) {
                        if ((op == CGO_IDIV || op == CGO_MOD) && (a.k != CGT_INT || b.k != CGT_INT)) cgx_verr(v, n->line, "%s needs two int operands, found %s and %s", CGO_NAME[op], t1, t2);
                        r = cgx_t(op == CGO_DIV ? CGT_FLOAT : (a.k == CGT_FLOAT || b.k == CGT_FLOAT) ? CGT_FLOAT : CGT_INT);
                    } else if (anyab) r = cgx_t(op == CGO_DIV ? CGT_FLOAT : CGT_ANY);
                    else cgx_verr(v, n->line, "%s of %s and %s (expected two numbers)", CGO_NAME[op], t1, t2);
                    break;
                case CGO_EQ: case CGO_NE:
                    if (!anyab && !(cgx_isnum(&a) && cgx_isnum(&b)) && !cgx_assignable(&a, &b) && !cgx_assignable(&b, &a)) cgx_verr(v, n->line, "comparing %s with %s is always false (expected the same type on both sides)", t1, t2);
                    r = cgx_t(CGT_BOOL); break;
                case CGO_LT: case CGO_LE: case CGO_GT: case CGO_GE:
                    if (!anyab && !((cgx_isnum(&a) && cgx_isnum(&b)) || (a.k == CGT_STR && b.k == CGT_STR))) cgx_verr(v, n->line, "COMPARE %s of %s and %s (expected two numbers or two str)", CGO_SYM[op], t1, t2);
                    r = cgx_t(CGT_BOOL); break;
                case CGO_AND: case CGO_OR:
                    if ((a.k != CGT_BOOL && a.k != CGT_ANY) || (b.k != CGT_BOOL && b.k != CGT_ANY)) cgx_verr(v, n->line, "`%s` expects bool on both sides, found %s and %s (write the comparison)", CGO_SYM[op], t1, t2);
                    r = cgx_t(CGT_BOOL); break;
                case CGO_IN:
                    if (b.k == CGT_LIST || b.k == CGT_ANY) { /* ok */ }
                    else if (b.k == CGT_STR) { if (a.k != CGT_STR && a.k != CGT_ANY) cgx_verr(v, n->line, "`x in str` needs a str on the left, found %s", t1); }
                    else if (b.k == CGT_MAP) { if (a.k != CGT_STR && a.k != CGT_ANY) cgx_verr(v, n->line, "`k in map` needs a str key, found %s", t1); }
                    else cgx_verr(v, n->line, "`in` needs a list, str or map on the right, found %s", t2);
                    r = cgx_t(CGT_BOOL); break;
                case CGO_APPROX: cgx_verr(v, n->line, "`~=` is only for `test` lines"); r = cgx_t(CGT_BOOL); break;
            }
            break;
        }
        case CGN_TERN: {
            cgx_cond(v, n->a, "the condition of `?:`");
            CgxTy a = cgx_ex(v, n->b), b = cgx_ex(v, n->c);
            if (a.k == b.k || a.k == CGT_ANY || b.k == CGT_ANY) r = a.k == CGT_ANY ? b : a;
            else if (cgx_isnum(&a) && cgx_isnum(&b)) r = cgx_t(CGT_FLOAT);
            else { cgx_tyname(&a, t1, sizeof t1); cgx_tyname(&b, t2, sizeof t2); cgx_verr(v, n->line, "the two branches of `?:` have different types: %s and %s", t1, t2); }
            break;
        }
        case CGN_MCALL: {
            CgxTy o = cgx_ex(v, n->a); CgxTy a[16]; int na = n->b1.n > 16 ? 16 : n->b1.n;
            for (int i = 0; i < n->b1.n; i++) { CgxTy t = cgx_ex(v, n->b1.p[i]); if (i < 16) a[i] = t; }
            if (o.k == CGT_CLASS) {
                CgxNode *c = cgx_find_cls(v, o.cls);
                if (!c) { cgx_verr(v, n->line, "`%s` has no methods", o.cls); break; }
                CgxNode *m = cgx_cls_method(c, n->s);
                if (!m) { char ml[300] = ""; size_t w = 0; for (int i = 0; i < c->b1.n && w + 40 < sizeof ml; i++) if (c->b1.p[i]->k == CGN_FN && strcmp(c->b1.p[i]->s, "init")) w += (size_t)snprintf(ml + w, sizeof ml - w, "%s%s", w ? ", " : "", c->b1.p[i]->s); cgx_verr(v, n->line, "class `%s` has no method `%s` (methods: %s)", c->s, n->s, ml[0] ? ml : "none"); break; }
                if (!strcmp(m->s, "init")) { cgx_verr(v, n->line, "the constructor is called by writing `%s(...)`, not `.init(...)`", c->s); break; }
                cgx_check_args(v, n, m, n->s, a, na);
                n->ref = m; r = m->ty;
            } else if (o.k == CGT_LIST || o.k == CGT_STR || o.k == CGT_MAP) {
                cgx_verr(v, n->line, "`.%s(...)` on a %s: lists, strings and maps have no methods in the UIR; write the function form, e.g. `push(xs, v)`, `upper(s)`, `len(x)`", n->s, o.k == CGT_LIST ? "list" : o.k == CGT_STR ? "str" : "map");
            } else if (o.k != CGT_ANY) { cgx_tyname(&o, t1, sizeof t1); cgx_verr(v, n->line, "cannot call a method on %s", t1); }
            break;
        }
        case CGN_CALL: {
            CgxTy a[16]; int na = n->b1.n > 16 ? 16 : n->b1.n;
            for (int i = 0; i < n->b1.n; i++) { CgxTy t = cgx_ex(v, n->b1.p[i]); if (i < 16) a[i] = t; }
            CgxNode *fn = cgx_find_fn(v, n->s), *cl = fn ? NULL : cgx_find_cls(v, n->s);
            const CgxOp *bop = cgx_op_find(n->s); const int bi = bop ? bop->id : 0;
            if (fn) { cgx_check_args(v, n, fn, n->s, a, na); n->ref = fn; r = fn->ty; if (fn->k == CGN_FN && !fn->has_ty && v->pass == 0 && fn->flag == 0) r = cgx_t(CGT_ANY); }
            else if (cl) {
                CgxNode *init = cgx_cls_method(cl, "init");
                if (init) cgx_check_args(v, n, init, n->s, a, na); else if (na) cgx_verr(v, n->line, "class `%s` has no `init`, so `%s(...)` takes no arguments (found %d)", n->s, n->s, na);
                n->ref = cl; r = cgx_t(CGT_CLASS); snprintf(r.cls, sizeof r.cls, "%s", cl->s);
            } else if (bi) {
                n->bi = bi;
                if (na < bop->lo || na > bop->hi) { cgx_verr(v, n->line, "%s called with %d argument%s (expected %s)", n->s, na, na == 1 ? "" : "s", bop->use); r = cgx_t(CGT_ANY); }
                else r = cgx_builtin(v, n, a, na);
            } else if (cgx_lookup(v, n->s)) cgx_verr(v, n->line, "`%s` is a variable, not a function", n->s);
            else cgx_verr(v, n->line, "unknown function `%s` (not defined in the spec and not a built-in; built-ins: %s)", n->s, cgx_reg_names_all());
            break;
        }
        default: cgx_verr(v, n->line, "internal: unexpected node kind %d in an expression", n->k);
    }
    n->ty = r;
    return r;
}

/* todos os caminhos terminam em return ou throw? (raw conta como terminando: nao da para saber, e o relatorio avisa) */
static int cgx_ends(CgxV *v, CgxBlk *b) {
    if (!b->n) return 0;
    for (int i = b->n - 1; i >= 0; i--) {
        CgxNode *s = b->p[i];
        if (s->k == CGN_COMMENT || s->k == CGN_DOC) continue;
        if (s->k == CGN_RET || s->k == CGN_THROW) return 1;
        if (s->k == CGN_RAW) { v->raw_ret++; return 1; }
        if (s->k == CGN_IF) {
            if (!s->b2.n) return 0;
            if (!cgx_ends(v, &s->b1)) return 0;
            if (s->b2.n == 1 && s->b2.p[0]->k == CGN_IF && s->b2.p[0]->flag) return cgx_ends(v, &s->b2);
            return cgx_ends(v, &s->b2);
        }
        if (s->k == CGN_TRY) {
            if (s->iv && cgx_ends(v, &s->b3)) return 1;
            if (!cgx_ends(v, &s->b1)) return 0;
            return !s->flag || cgx_ends(v, &s->b2);
        }
        return 0;
    }
    return 0;
}

static void cgx_stmt_check(CgxV *v, CgxNode *s) {
    char t1[60], t2[60];
    switch (s->k) {
        case CGN_COMMENT: case CGN_DOC: break;
        case CGN_RAW: {
            v->nraw++;
            if (s->defs) { char *d = cgx_dup(v->g, s->defs); for (char *tok = strtok(d, " "); tok; tok = strtok(NULL, " ")) if (!cgx_lookup(v, tok)) cgx_declare(v, s->line, tok, cgx_t(CGT_ANY), s, 0); }
            break;
        }
        case CGN_LET: {
            CgxTy t = cgx_ex(v, s->a);
            if (t.k == CGT_VOID) { cgx_verr(v, s->line, "`%s` is assigned the result of a call that returns nothing (void)", s->s); t = cgx_t(CGT_ANY); }
            if (s->has_ty) {
                if (!cgx_assignable(&s->ty, &t)) { cgx_tyname(&s->ty, t1, sizeof t1); cgx_tyname(&t, t2, sizeof t2); cgx_verr(v, s->line, "`%s` is declared %s but the value is %s", s->s, t1, t2); }
                if (s->ty.k == CGT_LIST && s->ty.e == CGT_ANY && t.k == CGT_LIST) s->ty.e = t.e;
            } else { s->ty = t; if (t.k == CGT_NULL) s->ty = cgx_t(CGT_ANY); s->has_ty = 1; s->flag |= 2; }     /* flag 2: tipo inferido */
            cgx_declare(v, s->line, s->s, s->ty, s, s->flag & 1);
            break;
        }
        case CGN_ASSIGN: {
            CgxTy tt = cgx_ex(v, s->a), vt = cgx_ex(v, s->b);
            if (s->a->k == CGN_VAR && s->a->ref == NULL && !cgx_lookup(v, s->a->s)) { /* ja erro de nao declarada */ }
            else if (s->a->k == CGN_VAR) {
                CgxSym *y = cgx_lookup(v, s->a->s);
                if (y && y->glob) cgx_verr(v, s->line, "`%s` is a module-level constant and cannot be reassigned inside a function", s->a->s);
            }
            if (vt.k == CGT_VOID) cgx_verr(v, s->line, "the value assigned is the result of a call that returns nothing (void)");
            if (s->op < 0) { if (!cgx_assignable(&tt, &vt)) { cgx_tyname(&tt, t1, sizeof t1); cgx_tyname(&vt, t2, sizeof t2); cgx_verr(v, s->line, "cannot assign %s to a target of type %s", t2, t1); } }
            else {
                v->nops[s->op]++;
                const int ok = (tt.k == CGT_ANY || vt.k == CGT_ANY) || (cgx_isnum(&tt) && cgx_isnum(&vt)) || (s->op == CGO_ADD && tt.k == CGT_STR && vt.k == CGT_STR);
                if (!ok) { cgx_tyname(&tt, t1, sizeof t1); cgx_tyname(&vt, t2, sizeof t2); cgx_verr(v, s->line, "`%s=` on %s and %s (expected two numbers%s)", CGO_SYM[s->op], t1, t2, s->op == CGO_ADD ? " or two str" : ""); }
                else if (tt.k == CGT_INT && vt.k == CGT_FLOAT) cgx_verr(v, s->line, "`%s=` would store a float in an int variable", CGO_SYM[s->op]);
            }
            break;
        }
        case CGN_RET: {
            if (!v->fn) { cgx_verr(v, s->line, "`return` outside a function"); break; }
            CgxTy t = s->a ? cgx_ex(v, s->a) : cgx_t(CGT_VOID);
            if (s->a) v->ret_val++; else v->ret_void++;
            if (v->fn->has_ty) {
                if (v->fn->ty.k == CGT_VOID && s->a) cgx_verr(v, s->line, "`%s` is declared `-> void` but returns a value", v->fn->s);
                else if (v->fn->ty.k != CGT_VOID && !s->a) { cgx_tyname(&v->fn->ty, t1, sizeof t1); cgx_verr(v, s->line, "`return` without a value in `%s`, which declares `-> %s`", v->fn->s, t1); }
                else if (s->a && !cgx_assignable(&v->fn->ty, &t)) { cgx_tyname(&v->fn->ty, t1, sizeof t1); cgx_tyname(&t, t2, sizeof t2); cgx_verr(v, s->line, "`%s` declares `-> %s` but returns %s", v->fn->s, t1, t2); }
            } else if (s->a) {
                if (t.k == CGT_VOID) cgx_verr(v, s->line, "returning the result of a call that returns nothing (void)");
                else if (v->ret.k == CGT_VOID) v->ret = t.k == CGT_NULL ? cgx_t(CGT_ANY) : t;
                else if (v->ret.k != CGT_ANY && t.k != CGT_ANY && t.k != CGT_NULL && !cgx_assignable(&v->ret, &t)) {
                    if (cgx_isnum(&t) && cgx_isnum(&v->ret)) v->ret = cgx_t(CGT_FLOAT);
                    else { cgx_tyname(&v->ret, t1, sizeof t1); cgx_tyname(&t, t2, sizeof t2); cgx_verr(v, s->line, "`%s` returns %s here but %s elsewhere (declare `-> type` to fix the contract)", v->fn->s, t2, t1); }
                }
            }
            break;
        }
        case CGN_THROW: {
            CgxTy t = cgx_ex(v, s->a);
            if (t.k != CGT_STR && t.k != CGT_ANY && !(t.k == CGT_CLASS && !strcmp(t.cls, "Error"))) { cgx_tyname(&t, t1, sizeof t1); cgx_verr(v, s->line, "`throw` takes a str (the message) or the error caught by `catch e`, found %s", t1); }
            break;
        }
        case CGN_BREAK: case CGN_CONT: if (!v->loops) cgx_verr(v, s->line, "`%s` outside a loop", s->k == CGN_BREAK ? "break" : "continue"); break;
        case CGN_EXPR: cgx_ex(v, s->a); break;
        case CGN_IF: {
            cgx_cond(v, s->a, "the `if` condition");
            int m; cgx_push(v, &m); cgx_blk(v, &s->b1); cgx_pop(v, m);
            cgx_push(v, &m); cgx_blk(v, &s->b2); cgx_pop(v, m);
            break;
        }
        case CGN_WHILE: {
            cgx_cond(v, s->a, "the `while` condition"); v->nloop++;
            int m; cgx_push(v, &m); v->loops++; cgx_blk(v, &s->b1); v->loops--; cgx_pop(v, m);
            break;
        }
        case CGN_FOR: {
            CgxTy a = cgx_ex(v, s->a), b = cgx_ex(v, s->b); v->nloop++;
            if ((a.k != CGT_INT && a.k != CGT_ANY) || (b.k != CGT_INT && b.k != CGT_ANY)) { cgx_tyname(&a, t1, sizeof t1); cgx_tyname(&b, t2, sizeof t2); cgx_verr(v, s->line, "a range loop needs int bounds, found %s..%s", t1, t2); }
            if (s->c) { cgx_ex(v, s->c); if (s->c->k != CGN_INT || s->c->iv == 0) cgx_verr(v, s->line, "`step` must be a non-zero integer literal (the direction of the loop depends on its sign)"); }
            int m; cgx_push(v, &m); cgx_declare(v, s->line, s->s, cgx_t(CGT_INT), s, 0);
            v->loops++; cgx_blk(v, &s->b1); v->loops--; cgx_pop(v, m);
            break;
        }
        case CGN_EACH: {
            CgxTy c = cgx_ex(v, s->a), vt = cgx_t(CGT_ANY), kt = cgx_t(CGT_ANY); v->neach++;
            if (c.k == CGT_LIST) { if (s->s2) cgx_verr(v, s->line, "iterating a list gives one variable (`for x in xs`); two variables are for maps"); vt = cgx_elem(&c); }
            else if (c.k == CGT_STR) { if (s->s2) cgx_verr(v, s->line, "iterating a str gives one variable"); vt = cgx_t(CGT_STR); }
            else if (c.k == CGT_MAP) { if (s->s2) { kt = cgx_t(CGT_STR); vt = cgx_elem(&c); } else vt = cgx_t(CGT_STR); }
            else if (c.k != CGT_ANY) { cgx_tyname(&c, t1, sizeof t1); cgx_verr(v, s->line, "cannot iterate over %s (expected a list, str or map)", t1); }
            int m; cgx_push(v, &m);
            if (s->s2) { cgx_declare(v, s->line, s->s, kt.k == CGT_STR ? kt : cgx_t(CGT_STR), s, 0); cgx_declare(v, s->line, s->s2, vt, s, 0); }
            else cgx_declare(v, s->line, s->s, vt, s, 0);
            v->loops++; cgx_blk(v, &s->b1); v->loops--; cgx_pop(v, m);
            break;
        }
        case CGN_TRY: {
            v->ntry++;
            int m; cgx_push(v, &m); cgx_blk(v, &s->b1); cgx_pop(v, m);
            if (s->flag) {
                cgx_push(v, &m);
                if (s->s2) { CgxTy et = cgx_t(CGT_CLASS); snprintf(et.cls, sizeof et.cls, "Error"); cgx_declare(v, s->line, s->s2, et, s, 0); }
                cgx_blk(v, &s->b2); cgx_pop(v, m);
            }
            if (s->iv) { cgx_push(v, &m); cgx_blk(v, &s->b3); cgx_pop(v, m); }
            break;
        }
        default: cgx_verr(v, s->line, "internal: unexpected statement kind %d", s->k);
    }
}
static void cgx_blk(CgxV *v, CgxBlk *b) { for (int i = 0; i < b->n; i++) cgx_stmt_check(v, b->p[i]); }

/* defaults de parametro e de campo: so constantes (literais, listas e mapas de literais) */
static int cgx_is_const_expr(const CgxNode *n) {
    switch (n->k) {
        case CGN_INT: case CGN_FLT: case CGN_STR: case CGN_BOOL: case CGN_NULL: return 1;
        case CGN_LIST: case CGN_MAP: for (int i = 0; i < n->b1.n; i++) if (!cgx_is_const_expr(n->b1.p[i])) return 0; return 1;
        default: return 0;
    }
}

static void cgx_fn_check(CgxV *v, CgxNode *fn, CgxNode *cls) {
    char t1[60], t2[60];
    int m; cgx_push(v, &m);
    v->fn = fn; v->curcls = cls; v->loops = 0; v->ret_val = v->ret_void = 0;
    v->ret = fn->has_ty ? fn->ty : (fn->flag ? fn->ty : cgx_t(CGT_VOID));
    if (!fn->has_ty && fn->flag == 0) v->ret = cgx_t(CGT_VOID);
    if (cls) { CgxTy st = cgx_t(CGT_CLASS); snprintf(st.cls, sizeof st.cls, "%s", cls->s); cgx_declare(v, fn->line, "self", st, fn, 0); }
    for (int i = 0; i < fn->npar; i++) {
        CgxParam *p = &fn->par[i];
        for (int j = 0; j < i; j++) if (!strcmp(fn->par[j].name, p->name)) cgx_verr(v, p->line, "duplicate parameter `%s` in `%s`", p->name, fn->s);
        CgxTy pt = p->has_ty ? p->ty : cgx_t(CGT_ANY);
        if (p->def) {
            if (!cgx_is_const_expr(p->def)) cgx_verr(v, p->line, "the default of `%s` must be a constant (a number, str, bool, null, or a list or map of constants)", p->name);
            CgxTy dt = cgx_ex(v, p->def);
            if (p->has_ty && !cgx_assignable(&p->ty, &dt)) { cgx_tyname(&p->ty, t1, sizeof t1); cgx_tyname(&dt, t2, sizeof t2); cgx_verr(v, p->line, "the default of `%s` is %s but the parameter is %s", p->name, t2, t1); }
            if (!p->has_ty && dt.k != CGT_NULL) { pt = dt; }
        }
        { int dup = 0; for (int j = 0; j < i; j++) if (!strcmp(fn->par[j].name, p->name)) dup = 1; if (!dup) cgx_declare(v, p->line, p->name, pt, fn, 0); }
        if (!p->has_ty) p->ty = pt;
    }
    cgx_blk(v, &fn->b1);
    if (!fn->has_ty) {
        if (v->ret_val && v->ret_void) cgx_verr(v, fn->line, "`%s` returns a value on some paths and nothing on others (declare `-> type`, and return a value everywhere)", fn->s);
        fn->ty = v->ret.k == CGT_VOID && v->ret_val ? cgx_t(CGT_ANY) : v->ret; fn->flag = 1;
    }
    if (fn->ty.k != CGT_VOID && !(cls && !strcmp(fn->s, "init")) && !cgx_ends(v, &fn->b1)) {
        cgx_tyname(&fn->ty, t1, sizeof t1);
        cgx_verr(v, fn->line, "`%s` returns %s but some path reaches the end without `return` (expected a `return` or `throw` on every path)", fn->s, fn->has_ty ? t1 : t1);
    }
    cgx_pop(v, m);
    v->fn = NULL; v->curcls = NULL;
}

/* valida o programa inteiro (duas passadas: a primeira so infere os tipos de retorno que nao foram escritos) */
static int cgx_check(Cgx *g) {
    cgx_reg_init();
    CgxV *v = (CgxV *)calloc(1, sizeof *v);
    v->g = g;
    CgxNode *pr = g->prog;
    for (int i = 0; i < pr->b1.n; i++) {
        CgxNode *d = pr->b1.p[i];
        if (d->k == CGN_FN) {
            if (cgx_op_find(d->s)) cgx_verr(v, d->line, "function `%s` has the name of a built-in (rename it; built-ins: %s)", d->s, cgx_reg_names_all());
            if (cgx_find_fn(v, d->s) || cgx_find_cls(v, d->s)) cgx_verr(v, d->line, "`%s` is defined twice", d->s);
            else if (v->nfn < 4096) v->fns[v->nfn++] = d;
            v->nfunc++;
        } else if (d->k == CGN_CLASS) {
            if (cgx_find_fn(v, d->s) || cgx_find_cls(v, d->s)) cgx_verr(v, d->line, "`%s` is defined twice", d->s);
            else if (v->ncls < 40) v->cls[v->ncls++] = d;
            v->nclass++;
            for (int j = 0; j < d->b1.n; j++) {
                CgxNode *m = d->b1.p[j];
                for (int k = 0; k < j; k++) if (d->b1.p[k]->s && m->s && d->b1.p[k]->k == m->k && !strcmp(d->b1.p[k]->s, m->s)) cgx_verr(v, m->line, "`%s` is declared twice in class `%s`", m->s, d->s);
                if (m->k == CGN_FN && (!strcmp(m->s, "constructor") || !strcmp(m->s, "__init__"))) m->s = cgx_dup(g, "init");
                if (m->k == CGN_FN && !strcmp(m->s, "init")) { m->ty = cgx_t(CGT_VOID); m->has_ty = 1; }
                if (m->k == CGN_FIELDDECL && m->a && !m->has_ty) { /* tipo do campo vem do padrao, na passada */ }
            }
        } else if (d->k == CGN_TEST) v->ntest++;
    }
    for (v->pass = 0; v->pass < 2; v->pass++) {
        const int nerr0 = g->nerr, nall0 = g->nerr_all;
        v->quiet = v->pass == 0; v->nsym = 0; v->depth = 0;
        memset(v->nops, 0, sizeof v->nops); memset(v->nbi, 0, sizeof v->nbi);
        v->nloop = v->ntry = v->neach = v->nmap = v->nlist = v->nstr = v->nraw = v->ndiv = v->nstrnum = v->raw_ret = 0;
        for (int i = 0; i < pr->b1.n; i++) {                       /* constantes de modulo, campos e metodos, na ordem */
            CgxNode *d = pr->b1.p[i];
            if (d->k == CGN_LET) { d->flag &= 1; cgx_stmt_check(v, d); }
            else if (d->k == CGN_RAW) cgx_stmt_check(v, d);
            else if (d->k == CGN_CLASS) {
                for (int j = 0; j < d->b1.n; j++) {
                    CgxNode *f = d->b1.p[j];
                    if (f->k != CGN_FIELDDECL) continue;
                    if (f->a) {
                        if (!cgx_is_const_expr(f->a)) cgx_verr(v, f->line, "the default of field `%s` must be a constant", f->s);
                        CgxTy t = cgx_ex(v, f->a);
                        if (f->has_ty && !cgx_assignable(&f->ty, &t)) { char a1[60], a2[60]; cgx_tyname(&f->ty, a1, sizeof a1); cgx_tyname(&t, a2, sizeof a2); cgx_verr(v, f->line, "field `%s` is %s but its default is %s", f->s, a1, a2); }
                        if (!f->has_ty) { f->ty = t.k == CGT_NULL ? cgx_t(CGT_ANY) : t; f->has_ty = 1; f->flag |= 2; }
                    } else if (!f->has_ty) { f->ty = cgx_t(CGT_ANY); f->has_ty = 1; f->flag |= 2; }
                    else if (f->ty.k == CGT_INT || f->ty.k == CGT_FLOAT || f->ty.k == CGT_BOOL || f->ty.k == CGT_STR) { /* sem padrao: o motor inicia com o zero do tipo */ }
                }
            }
        }
        for (int i = 0; i < pr->b1.n; i++) {
            CgxNode *d = pr->b1.p[i];
            if (d->k == CGN_FN) cgx_fn_check(v, d, NULL);
            else if (d->k == CGN_CLASS) for (int j = 0; j < d->b1.n; j++) if (d->b1.p[j]->k == CGN_FN) cgx_fn_check(v, d->b1.p[j], d);
        }
        for (int i = 0; i < pr->b1.n; i++) {
            CgxNode *d = pr->b1.p[i];
            if (d->k != CGN_TEST) continue;
            CgxTy a = cgx_ex(v, d->a);
            if (d->iv == 2 && d->a->k != CGN_CALL && d->a->k != CGN_MCALL) cgx_verr(v, d->line, "`test ... raises` needs a call (expected `test f(x) raises`)");
            if (d->b) {
                CgxTy b = cgx_ex(v, d->b);
                if (a.k != CGT_ANY && b.k != CGT_ANY && !(cgx_isnum(&a) && cgx_isnum(&b)) && !cgx_assignable(&a, &b) && !cgx_assignable(&b, &a)) { char t1[60], t2[60]; cgx_tyname(&a, t1, sizeof t1); cgx_tyname(&b, t2, sizeof t2); cgx_verr(v, d->line, "the test compares %s with %s", t1, t2); }
                if (d->iv == 1 && !(cgx_isnum(&a) || a.k == CGT_ANY)) cgx_verr(v, d->line, "`~=` compares numbers");
            } else if (d->iv == 3 && a.k != CGT_BOOL && a.k != CGT_ANY) cgx_verr(v, d->line, "a `test` without `==` must be a bool expression");
        }
        if (v->pass == 0) { g->nerr = nerr0; g->nerr_all = nall0; }
    }
    free(v);
    return g->nerr == 0;
}

/* resumo do que a especificacao usa, para o relatorio (nivel L1/L2/L3 da UIR) */
typedef struct { int nfunc, nclass, ntest, nraw, level, ndiv, nstrnum; char ops[400]; } CgxStats;
static void cgx_stats(Cgx *g, CgxStats *st) {
    /* conta sobre a AST (independe do CgxV, que ja foi liberado) */
    memset(st, 0, sizeof *st);
    int nops[CGO_N] = {0}, loops = 0, tries = 0, eachs = 0, strs = 0, maps = 0, lists = 0, calls = 0, ifs = 0, rets = 0;
    CgxNode *stack[4096]; int sp = 0;
    stack[sp++] = g->prog;
    while (sp) {
        CgxNode *n = stack[--sp];
        if (!n) continue;
        switch (n->k) {
            case CGN_FN: st->nfunc++; break; case CGN_CLASS: st->nclass++; break; case CGN_TEST: st->ntest++; break; case CGN_RAW: st->nraw++; break;
            case CGN_BIN: case CGN_UN: nops[n->op]++; if (n->k == CGN_BIN && (n->op == CGO_DIV || n->op == CGO_IDIV || n->op == CGO_MOD) && !(n->b->k == CGN_INT || n->b->k == CGN_FLT)) st->ndiv++; break;
            case CGN_WHILE: case CGN_FOR: loops++; break; case CGN_TRY: tries++; break; case CGN_EACH: eachs++; break;
            case CGN_STR: strs++; break; case CGN_MAP: maps++; break; case CGN_LIST: lists++; break; case CGN_CALL: case CGN_MCALL: calls++; break;
            case CGN_IF: ifs++; break; case CGN_RET: rets++; break;
        }
        if (n->k == CGN_CALL && n->bi && (n->bi == CGB_STR) && (n->b1.n && (n->b1.p[0]->ty.k == CGT_FLOAT || n->b1.p[0]->ty.k == CGT_BOOL))) st->nstrnum++;
        if (n->k == CGN_CALL && n->bi) { const CgxOp *bo = cgx_op_by_id(n->bi); if (bo && bo->level >= 2) strs++; }
        CgxNode *kids[8] = {n->a, n->b, n->c};
        for (int i = 0; i < 3 && sp < 4090; i++) if (kids[i]) stack[sp++] = kids[i];
        CgxBlk *bl[3] = {&n->b1, &n->b2, &n->b3};
        for (int i = 0; i < 3; i++) for (int j = 0; j < bl[i]->n && sp < 4090; j++) stack[sp++] = bl[i]->p[j];
        for (int i = 0; i < n->npar && sp < 4090; i++) if (n->par[i].def) stack[sp++] = n->par[i].def;
    }
    size_t w = 0;
    for (int i = 0; i < CGO_N; i++) if (nops[i] && w + 30 < sizeof st->ops) w += (size_t)snprintf(st->ops + w, sizeof st->ops - w, "%s%s", w ? " " : "", i < CGO_EQ || i > CGO_GE ? CGO_NAME[i] : "COMPARE");
    { /* COMPARE repetido: deixa so uma vez */
        char *p = st->ops, o[400]; size_t k = 0; int seen = 0; char *tok, *save; strncpy(o, p, sizeof o - 1); o[sizeof o - 1] = 0; p[0] = 0;
        for (tok = strtok_r(o, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) { if (!strcmp(tok, "COMPARE")) { if (seen) continue; seen = 1; } k += (size_t)snprintf(p + k, sizeof st->ops - k, "%s%s", k ? " " : "", tok); }
    }
    if (ifs && w + 8 < sizeof st->ops) snprintf(st->ops + strlen(st->ops), sizeof st->ops - strlen(st->ops), "%sIF", st->ops[0] ? " " : "");
    if (loops) snprintf(st->ops + strlen(st->ops), sizeof st->ops - strlen(st->ops), "%sLOOP", st->ops[0] ? " " : "");
    if (eachs) snprintf(st->ops + strlen(st->ops), sizeof st->ops - strlen(st->ops), "%sITERATE", st->ops[0] ? " " : "");
    if (tries) snprintf(st->ops + strlen(st->ops), sizeof st->ops - strlen(st->ops), "%sTRY_CATCH", st->ops[0] ? " " : "");
    if (st->nraw) snprintf(st->ops + strlen(st->ops), sizeof st->ops - strlen(st->ops), "%sRAW", st->ops[0] ? " " : "");
    st->level = st->nclass || st->nraw ? 3 : (tries || eachs || strs || maps || lists) ? 2 : 1;
    (void)calls; (void)rets;
}
