/* ---------- coder/ucgepy.c: UCGE, backend de referencia: Python (docs/ucge.md, "Backends") ----------
 * Percorre a AST ja validada e monta um documento (ucgedoc.c); nunca concatena codigo solto. Este arquivo e o MODELO dos demais backends:
 * mesma estrutura (precedencia por no, tabela das funcoes embutidas, classe, tentativa/captura, raw, harness de testes), so muda a tabela.
 * Semantica fixada pela UIR e como o Python a cumpre: `//` e `%` ja sao piso (floored); `/` e divisao real; strings e listas indexadas por
 * inteiro; mapas viram dict; `catch e` captura Exception; defaults mutaveis (lista, mapa) viram None + criacao no corpo. */
typedef struct { Cgx *g; const CgxGenOpts *o; CgxDocs D; int need_math, need_re; const CgxNode *fn; int in_class; } CgxPy;
#define PD (&p->D)
static const char *const CGX_PY_RESERVED[] = {"False", "None", "True", "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif", "else", "except", "finally", "for", "from",
    "global", "if", "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try", "while", "with", "yield", "match", "case", NULL};
static const char *const CGX_PY_SHADOW[] = {"len", "str", "int", "float", "list", "dict", "print", "sum", "min", "max", "abs", "sorted", "math", "re", "range", "set", "bool", "type", "Exception", NULL};
static void cgx_py_name(CgxPy *p, int line, const char *n, int attr) {
    for (int i = 0; CGX_PY_RESERVED[i]; i++) if (!strcmp(n, CGX_PY_RESERVED[i])) { cgx_err(p->g, line, "python: `%s` is a Python keyword; rename it", n); return; }
    if (!attr) for (int i = 0; CGX_PY_SHADOW[i]; i++) if (!strcmp(n, CGX_PY_SHADOW[i])) { cgx_err(p->g, line, "python: `%s` would shadow a Python built-in that the generated code uses; rename it", n); return; }
}
static void cgx_fmt_float(double d, char *o, size_t cap) {
    if (isinf(d) || isnan(d)) { snprintf(o, cap, "0.0"); return; }
    snprintf(o, cap, "%.15g", d);
    if (strtod(o, NULL) != d) snprintf(o, cap, "%.17g", d);
    if (!strpbrk(o, ".eE")) strncat(o, ".0", cap - strlen(o) - 1);
}
static void cgx_py_quote(const char *s, CgxB *o) {
    cgx_bs(o, "\"");
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { cgx_bn(o, "\\", 1); cgx_bn(o, s, 1); }
        else if (c == '\n') cgx_bs(o, "\\n"); else if (c == '\t') cgx_bs(o, "\\t"); else if (c == '\r') cgx_bs(o, "\\r");
        else if (c < 32 || c == 127) cgx_bf(o, "\\x%02x", c);
        else cgx_bn(o, s, 1);
    }
    cgx_bs(o, "\"");
}
static CgxDoc *py_t(CgxPy *p, const char *s) { return cgx_dtext(PD, s); }

static void py_type(CgxPy *p, const CgxTy *t, CgxB *o) {
    static const char *const nm[] = {"", "int", "float", "str", "bool", "None", "list", "dict", "", ""};
    switch (t->k) {
        case CGT_INT: case CGT_FLOAT: case CGT_STR: case CGT_BOOL: cgx_bs(o, nm[t->k]); break;
        case CGT_LIST: cgx_bs(o, "list"); if (t->e == CGT_INT || t->e == CGT_FLOAT || t->e == CGT_STR || t->e == CGT_BOOL) cgx_bf(o, "[%s]", nm[t->e]); break;
        case CGT_MAP: cgx_bs(o, "dict"); if (t->e == CGT_INT || t->e == CGT_FLOAT || t->e == CGT_STR || t->e == CGT_BOOL) cgx_bf(o, "[str, %s]", nm[t->e]); break;
        case CGT_CLASS: cgx_bs(o, t->cls); break;
        default: break;
    }
    (void)p;
}
static int py_hintable(const CgxTy *t) { return t->k != CGT_ANY && t->k != CGT_VOID && t->k != CGT_NULL && !(t->k == CGT_CLASS && !strcmp(t->cls, "Error")); }

static CgxDoc *py_ex(CgxPy *p, const CgxNode *n, int minprec);
/* lista de argumentos entre delimitadores, quebrando um por linha quando nao cabe */
static CgxDoc *py_seq(CgxPy *p, const char *open, CgxDoc *const *items, int n, const char *close) {
    if (!n) return cgx_dtextf(PD, "%s%s", open, close);
    CgxDoc *in = NULL;
    for (int i = 0; i < n; i++) { CgxDoc *x = i ? cgx_dcatn(PD, py_t(p, ","), cgx_dline(PD), items[i], NULL) : items[i]; in = in ? cgx_dcat(PD, in, x) : x; }
    return cgx_dgroup(PD, cgx_dcatn(PD, py_t(p, open), cgx_dnest(PD, p->o->indent, cgx_dcat(PD, cgx_dsoft(PD), in)), cgx_dsoft(PD), py_t(p, close), NULL));
}
static CgxDoc *py_args(CgxPy *p, const char *head, const CgxBlk *a, int skip) {
    CgxDoc *it[64]; int n = 0;
    for (int i = skip; i < a->n && n < 64; i++) it[n++] = py_ex(p, a->p[i], 0);
    return cgx_dcat(PD, py_t(p, head), py_seq(p, "(", it, n, ")"));
}
static const int CGX_PREC_CMP = 5;
static CgxDoc *py_wrap(CgxPy *p, CgxDoc *d, int prec, int minprec) { return prec < minprec ? cgx_dcatn(PD, py_t(p, "("), d, py_t(p, ")"), NULL) : d; }

static CgxDoc *py_call(CgxPy *p, const CgxNode *n, int *prec) {
    const CgxBlk *a = &n->b1; *prec = 10;
    if (n->bi == 0) {                                   /* funcao ou construtor da especificacao */
        CgxDoc *it[64]; int k = 0;
        for (int i = 0; i < a->n && k < 64; i++) it[k++] = py_ex(p, a->p[i], 0);
        return cgx_dcat(PD, cgx_dtext(PD, n->s), py_seq(p, "(", it, k, ")"));
    }
    CgxDoc *A0 = a->n > 0 ? py_ex(p, a->p[0], 0) : NULL;
    switch (n->bi) {
        case CGB_LEN: return py_args(p, "len", a, 0);
        case CGB_PUSH: return cgx_dcatn(PD, py_ex(p, a->p[0], 10), py_args(p, ".append", a, 1), NULL);
        case CGB_POP: return cgx_dcat(PD, py_ex(p, a->p[0], 10), py_t(p, ".pop()"));
        case CGB_STR: return py_args(p, "str", a, 0);
        case CGB_INT: return py_args(p, "int", a, 0);
        case CGB_FLOAT: return py_args(p, "float", a, 0);
        case CGB_UPPER: return cgx_dcat(PD, py_ex(p, a->p[0], 10), py_t(p, ".upper()"));
        case CGB_LOWER: return cgx_dcat(PD, py_ex(p, a->p[0], 10), py_t(p, ".lower()"));
        case CGB_TRIM: return cgx_dcat(PD, py_ex(p, a->p[0], 10), py_t(p, ".strip()"));
        case CGB_SPLIT: return cgx_dcatn(PD, py_ex(p, a->p[0], 10), py_args(p, ".split", a, 1), NULL);
        case CGB_WORDS: return cgx_dcat(PD, py_ex(p, a->p[0], 10), py_t(p, ".split()"));
        case CGB_JOIN: return cgx_dcatn(PD, py_ex(p, a->p[1], 10), py_t(p, ".join"), py_seq(p, "(", (CgxDoc *[]){A0}, 1, ")"), NULL);
        case CGB_CONTAINS: *prec = CGX_PREC_CMP; return cgx_dcatn(PD, py_ex(p, a->p[1], CGX_PREC_CMP + 1), py_t(p, " in "), py_ex(p, a->p[0], CGX_PREC_CMP + 1), NULL);
        case CGB_INDEXOF: return cgx_dcatn(PD, py_ex(p, a->p[0], 10), py_args(p, ".find", a, 1), NULL);
        case CGB_REPLACE: return cgx_dcatn(PD, py_ex(p, a->p[0], 10), py_args(p, ".replace", a, 1), NULL);
        case CGB_STARTS: return cgx_dcatn(PD, py_ex(p, a->p[0], 10), py_args(p, ".startswith", a, 1), NULL);
        case CGB_ENDS: return cgx_dcatn(PD, py_ex(p, a->p[0], 10), py_args(p, ".endswith", a, 1), NULL);
        case CGB_SLICE: return cgx_dcatn(PD, py_ex(p, a->p[0], 10), py_t(p, "["), py_ex(p, a->p[1], 0), py_t(p, ":"), a->n > 2 ? py_ex(p, a->p[2], 0) : cgx_dnil(PD), py_t(p, "]"), NULL);
        case CGB_KEYS: return cgx_dcatn(PD, py_t(p, "list("), py_ex(p, a->p[0], 10), py_t(p, ".keys())"), NULL);
        case CGB_PRINT: return py_args(p, "print", a, 0);
        case CGB_ABS: return py_args(p, "abs", a, 0);
        case CGB_MIN: return py_args(p, "min", a, 0);
        case CGB_MAX: return py_args(p, "max", a, 0);
        case CGB_SQRT: p->need_math = 1; return py_args(p, "math.sqrt", a, 0);
        case CGB_FLOOR: p->need_math = 1; return py_args(p, "math.floor", a, 0);
        case CGB_POW: p->need_math = 1; return py_args(p, "math.pow", a, 0);
        case CGB_SUM: return py_args(p, "sum", a, 0);
        case CGB_SORT: return py_args(p, "sorted", a, 0);
        case CGB_REVERSE: return cgx_dcat(PD, py_ex(p, a->p[0], 10), py_t(p, "[::-1]"));
        case CGB_MATCHES: p->need_re = 1; *prec = CGX_PREC_CMP; return cgx_dcatn(PD, py_t(p, "re.search("), py_ex(p, a->p[1], 0), py_t(p, ", "), A0, py_t(p, ") is not None"), NULL);
        case CGB_ERRMSG: return py_args(p, "str", a, 0);
    }
    cgx_err(p->g, n->line, "python: built-in `%s` has no Python form yet", n->s);
    return cgx_dnil(PD);
}
static int py_binprec(int op) { switch (op) { case CGO_OR: return 2; case CGO_AND: return 3; case CGO_EQ: case CGO_NE: case CGO_LT: case CGO_LE: case CGO_GT: case CGO_GE: case CGO_IN: return 5; case CGO_ADD: case CGO_SUB: return 6; default: return 7; } }
static CgxDoc *py_ex0(CgxPy *p, const CgxNode *n, int *prec) {
    CgxB b = {0}; CgxDoc *d; *prec = 11;
    switch (n->k) {
        case CGN_INT: cgx_bf(&b, "%lld", n->iv); if (n->iv < 0) *prec = 8; break;
        case CGN_FLT: { char t[64]; cgx_fmt_float(n->dv, t, sizeof t); cgx_bs(&b, t); if (n->dv < 0) *prec = 8; break; }
        case CGN_STR: cgx_py_quote(n->s, &b); break;
        case CGN_BOOL: cgx_bs(&b, n->iv ? "True" : "False"); break;
        case CGN_NULL: cgx_bs(&b, "None"); break;
        case CGN_VAR: cgx_bs(&b, n->s); break;
        case CGN_LIST: { free(b.p); CgxDoc *it[256]; int k = 0; for (int i = 0; i < n->b1.n && k < 256; i++) it[k++] = py_ex(p, n->b1.p[i], 0); return py_seq(p, "[", it, k, "]"); }
        case CGN_MAP: { free(b.p); CgxDoc *it[256]; int k = 0; for (int i = 0; i + 1 < n->b1.n && k < 256; i += 2) it[k++] = cgx_dcatn(PD, py_ex(p, n->b1.p[i], 0), py_t(p, ": "), py_ex(p, n->b1.p[i + 1], 0), NULL); return py_seq(p, "{", it, k, "}"); }
        case CGN_INDEX: free(b.p); return cgx_dcatn(PD, py_ex(p, n->a, 10), py_t(p, "["), py_ex(p, n->b, 0), py_t(p, "]"), NULL);
        case CGN_FIELD: free(b.p); return cgx_dcatn(PD, py_ex(p, n->a, 10), cgx_dtextf(PD, ".%s", n->s), NULL);
        case CGN_MCALL: { free(b.p); CgxDoc *it[64]; int k = 0; for (int i = 0; i < n->b1.n && k < 64; i++) it[k++] = py_ex(p, n->b1.p[i], 0); return cgx_dcatn(PD, py_ex(p, n->a, 10), cgx_dtextf(PD, ".%s", n->s), py_seq(p, "(", it, k, ")"), NULL); }
        case CGN_CALL: free(b.p); return py_call(p, n, prec);
        case CGN_UN: free(b.p);
            if (n->op == CGO_NOT) { *prec = 4; return cgx_dcat(PD, py_t(p, "not "), py_ex(p, n->a, 4)); }
            *prec = 8; return cgx_dcat(PD, py_t(p, "-"), py_ex(p, n->a, 8));
        case CGN_BIN: {
            free(b.p);
            static const char *const sym[] = {"+", "-", "*", "/", "//", "%", "==", "!=", "<", "<=", ">", ">=", "and", "or", "", "", "in", "=="};
            const int pr = py_binprec(n->op); *prec = pr;
            const int cmp = pr == 5;
            d = cgx_dcatn(PD, py_ex(p, n->a, cmp ? pr + 1 : pr), cgx_dtextf(PD, " %s ", sym[n->op]), py_ex(p, n->b, pr + 1), NULL);
            return d;
        }
        case CGN_TERN: free(b.p); *prec = 1; return cgx_dcatn(PD, py_ex(p, n->b, 2), py_t(p, " if "), py_ex(p, n->a, 2), py_t(p, " else "), py_ex(p, n->c, 1), NULL);
        default: cgx_err(p->g, n->line, "python: unexpected expression node %d", n->k); free(b.p); return cgx_dnil(PD);
    }
    d = cgx_dtext(PD, b.p ? b.p : ""); free(b.p); return d;
}
static CgxDoc *py_ex(CgxPy *p, const CgxNode *n, int minprec) { int prec; CgxDoc *d = py_ex0(p, n, &prec); return py_wrap(p, d, prec, minprec); }

static CgxDoc *py_stmt(CgxPy *p, const CgxNode *s);
static CgxDoc *py_body(CgxPy *p, const CgxBlk *b, const CgxNode *prologue_fn) {
    CgxDoc *in = NULL; int any = 0;
    if (prologue_fn) for (int i = 0; i < prologue_fn->npar; i++) {          /* default mutavel: None + criacao no corpo */
        const CgxParam *pr = &prologue_fn->par[i];
        if (pr->def && (pr->def->k == CGN_LIST || pr->def->k == CGN_MAP)) {
            CgxDoc *x = cgx_dcatn(PD, cgx_dhard(PD), cgx_dtextf(PD, "if %s is None:", pr->name), cgx_dnest(PD, p->o->indent, cgx_dcatn(PD, cgx_dhard(PD), cgx_dtextf(PD, "%s = ", pr->name), py_ex(p, pr->def, 0), NULL)), NULL);
            in = in ? cgx_dcat(PD, in, x) : x; any = 1;
        }
    }
    for (int i = 0; i < b->n; i++) {
        const CgxNode *s = b->p[i];
        if ((s->k == CGN_COMMENT || s->k == CGN_DOC) && !p->o->comments) continue;
        CgxDoc *d = py_stmt(p, s);
        if (!d) continue;
        CgxDoc *x = cgx_dcat(PD, cgx_dhard(PD), d);
        in = in ? cgx_dcat(PD, in, x) : x; any = 1;
    }
    if (!any) in = cgx_dcat(PD, cgx_dhard(PD), py_t(p, "pass"));
    return cgx_dnest(PD, p->o->indent, in);
}
static CgxDoc *py_raw(CgxPy *p, const CgxNode *s) {
    if (!s->raw[0]) { cgx_err(p->g, s->line, "python: this `raw` block has no `raw python` version (it has: %s%s%s%s)", s->raw[1] ? "js " : "", s->raw[2] ? "ts " : "", s->raw[3] ? "c" : "", "" ); return NULL; }
    CgxDoc *r = NULL; const char *q = s->raw[0];
    while (*q) {
        const char *e = strchr(q, '\n'); size_t l = e ? (size_t)(e - q) : strlen(q);
        char *ln = (char *)malloc(l + 1); memcpy(ln, q, l); ln[l] = 0;
        CgxDoc *t = cgx_dtext(PD, ln); free(ln);
        r = r ? cgx_dcatn(PD, r, cgx_dhard(PD), t, NULL) : t;
        q += l + (e ? 1 : 0);
    }
    return r ? r : cgx_dnil(PD);
}
static CgxDoc *py_stmt(CgxPy *p, const CgxNode *s) {
    switch (s->k) {
        case CGN_COMMENT: if (!s->defs) return NULL; return cgx_dtextf(PD, "# %s", s->defs);
        case CGN_DOC: return cgx_dtextf(PD, "# %s", s->defs);
        case CGN_RAW: return py_raw(p, s);
        case CGN_LET: {
            cgx_py_name(p, s->line, s->s, 0);
            CgxB h = {0}; if (!(s->flag & 2) && py_hintable(&s->ty)) { cgx_bs(&h, ": "); py_type(p, &s->ty, &h); }
            CgxDoc *d = cgx_dcatn(PD, cgx_dtextf(PD, "%s%s = ", s->s, h.p ? h.p : ""), py_ex(p, s->a, 0), NULL); free(h.p); return d;
        }
        case CGN_ASSIGN: {
            static const char *const as[] = {"+=", "-=", "*=", "/=", "", "%="};
            const char *op = s->op < 0 ? "=" : as[s->op];
            return cgx_dcatn(PD, py_ex(p, s->a, 0), cgx_dtextf(PD, " %s ", op), py_ex(p, s->b, 0), NULL);
        }
        case CGN_RET: return s->a ? cgx_dcat(PD, py_t(p, "return "), py_ex(p, s->a, 0)) : py_t(p, "return");
        case CGN_BREAK: return py_t(p, "break");
        case CGN_CONT: return py_t(p, "continue");
        case CGN_EXPR: return py_ex(p, s->a, 0);
        case CGN_THROW: {
            if (s->a->k == CGN_VAR && s->a->ty.k == CGT_CLASS && !strcmp(s->a->ty.cls, "Error")) return cgx_dcat(PD, py_t(p, "raise "), py_ex(p, s->a, 0));
            return cgx_dcatn(PD, py_t(p, "raise Exception("), py_ex(p, s->a, 0), py_t(p, ")"), NULL);
        }
        case CGN_IF: {
            CgxDoc *d = cgx_dcatn(PD, py_t(p, s->flag ? "elif " : "if "), py_ex(p, s->a, 0), py_t(p, ":"), py_body(p, &s->b1, NULL), NULL);
            if (s->b2.n == 1 && s->b2.p[0]->k == CGN_IF && s->b2.p[0]->flag) d = cgx_dcatn(PD, d, cgx_dhard(PD), py_stmt(p, s->b2.p[0]), NULL);
            else if (s->b2.n) d = cgx_dcatn(PD, d, cgx_dhard(PD), py_t(p, "else:"), py_body(p, &s->b2, NULL), NULL);
            return d;
        }
        case CGN_WHILE: return cgx_dcatn(PD, py_t(p, "while "), py_ex(p, s->a, 0), py_t(p, ":"), py_body(p, &s->b1, NULL), NULL);
        case CGN_FOR: {
            cgx_py_name(p, s->line, s->s, 0);
            CgxDoc *it[3]; int k = 0;
            if (!(s->a->k == CGN_INT && s->a->iv == 0) || s->c) it[k++] = py_ex(p, s->a, 0);
            it[k++] = py_ex(p, s->b, 0);
            if (s->c) it[k++] = py_ex(p, s->c, 0);
            return cgx_dcatn(PD, cgx_dtextf(PD, "for %s in range", s->s), py_seq(p, "(", it, k, ")"), py_t(p, ":"), py_body(p, &s->b1, NULL), NULL);
        }
        case CGN_EACH: {
            cgx_py_name(p, s->line, s->s, 0); if (s->s2) cgx_py_name(p, s->line, s->s2, 0);
            const int mp = s->a->ty.k == CGT_MAP;
            CgxDoc *coll = py_ex(p, s->a, 10);
            if (mp && s->s2) coll = cgx_dcat(PD, coll, py_t(p, ".items()"));
            CgxDoc *vars = s->s2 ? cgx_dtextf(PD, "%s, %s", s->s, s->s2) : cgx_dtext(PD, s->s);
            return cgx_dcatn(PD, py_t(p, "for "), vars, py_t(p, " in "), coll, py_t(p, ":"), py_body(p, &s->b1, NULL), NULL);
        }
        case CGN_TRY: {
            CgxDoc *d = cgx_dcatn(PD, py_t(p, "try:"), py_body(p, &s->b1, NULL), NULL);
            if (s->flag) { if (s->s2) cgx_py_name(p, s->line, s->s2, 0); d = cgx_dcatn(PD, d, cgx_dhard(PD), s->s2 ? cgx_dtextf(PD, "except Exception as %s:", s->s2) : py_t(p, "except Exception:"), py_body(p, &s->b2, NULL), NULL); }
            if (s->iv) d = cgx_dcatn(PD, d, cgx_dhard(PD), py_t(p, "finally:"), py_body(p, &s->b3, NULL), NULL);
            return d;
        }
    }
    cgx_err(p->g, s->line, "python: unexpected statement node %d", s->k);
    return NULL;
}
static CgxDoc *py_fn(CgxPy *p, const CgxNode *fn, const CgxNode *cls) {
    const int ctor = cls && !strcmp(fn->s, "init");
    cgx_py_name(p, fn->line, ctor ? "__init__" : fn->s, cls != NULL);
    CgxDoc *it[41]; int k = 0;
    if (cls) it[k++] = py_t(p, "self");
    for (int i = 0; i < fn->npar && k < 41; i++) {
        const CgxParam *pr = &fn->par[i]; CgxB b = {0};
        cgx_py_name(p, pr->line, pr->name, 0);
        const int mut = pr->def && (pr->def->k == CGN_LIST || pr->def->k == CGN_MAP);
        cgx_bs(&b, pr->name);
        if (pr->has_ty && py_hintable(&pr->ty) && !mut) { cgx_bs(&b, ": "); py_type(p, &pr->ty, &b); }
        CgxDoc *d = cgx_dtext(PD, b.p); free(b.p);
        if (pr->def) d = cgx_dcatn(PD, d, py_t(p, pr->has_ty && py_hintable(&pr->ty) && !mut ? " = " : "="), mut ? py_t(p, "None") : py_ex(p, pr->def, 0), NULL);
        it[k++] = d;
    }
    CgxB rb = {0};
    if (!ctor && py_hintable(&fn->ty) && fn->ty.k != CGT_VOID) { cgx_bs(&rb, " -> "); py_type(p, &fn->ty, &rb); }
    CgxDoc *head = cgx_dcatn(PD, cgx_dtextf(PD, "def %s", ctor ? "__init__" : fn->s), py_seq(p, "(", it, k, ")"), cgx_dtextf(PD, "%s:", rb.p ? rb.p : ""), NULL);
    free(rb.p);
    /* docstring: o primeiro `doc` do corpo */
    CgxDoc *doc = NULL; CgxBlk rest = {0}; int skip = -1;
    if (p->o->comments) for (int i = 0; i < fn->b1.n; i++) { if (fn->b1.p[i]->k == CGN_DOC) { skip = i; break; } if (fn->b1.p[i]->k != CGN_COMMENT) break; }
    CgxBlk nb = fn->b1;
    if (skip >= 0) { doc = cgx_dcatn(PD, cgx_dhard(PD), cgx_dtextf(PD, "\"\"\"%s\"\"\"", fn->b1.p[skip]->defs), NULL); CgxNode **tmp = (CgxNode **)malloc(sizeof(CgxNode *) * (size_t)(fn->b1.n)); int w = 0; for (int i = 0; i < fn->b1.n; i++) if (i != skip) tmp[w++] = fn->b1.p[i]; rest.p = tmp; rest.n = w; nb = rest; }
    CgxDoc *body;
    if (ctor) {
        /* campos com valor inicial primeiro, depois o corpo escrito */
        CgxDoc *in = NULL;
        for (int i = 0; i < cls->b1.n; i++) {
            const CgxNode *f = cls->b1.p[i]; if (f->k != CGN_FIELDDECL) continue;
            cgx_py_name(p, f->line, f->s, 1);
            CgxDoc *v = f->a ? py_ex(p, f->a, 0) : py_t(p, f->ty.k == CGT_INT ? "0" : f->ty.k == CGT_FLOAT ? "0.0" : f->ty.k == CGT_STR ? "\"\"" : f->ty.k == CGT_BOOL ? "False" : f->ty.k == CGT_LIST ? "[]" : f->ty.k == CGT_MAP ? "{}" : "None");
            CgxDoc *x = cgx_dcatn(PD, cgx_dhard(PD), cgx_dtextf(PD, "self.%s = ", f->s), v, NULL); in = in ? cgx_dcat(PD, in, x) : x;
        }
        CgxDoc *rest_d = py_body(p, &nb, fn);
        body = in ? cgx_dnest(PD, p->o->indent, cgx_dcatn(PD, in, cgx_dnest(PD, -p->o->indent, nb.n ? rest_d : cgx_dnil(PD)), NULL)) : rest_d;
    } else body = py_body(p, &nb, fn);
    free(rest.p);
    return cgx_dcatn(PD, head, doc ? cgx_dnest(PD, p->o->indent, doc) : cgx_dnil(PD), body, NULL);
}
static CgxDoc *py_class(CgxPy *p, const CgxNode *c) {
    cgx_py_name(p, c->line, c->s, 0);
    CgxDoc *in = NULL; int first = 1, hasinit = 0;
    for (int i = 0; i < c->b1.n; i++) if (c->b1.p[i]->k == CGN_FN && !strcmp(c->b1.p[i]->s, "init")) hasinit = 1;
    int nfield = 0; for (int i = 0; i < c->b1.n; i++) if (c->b1.p[i]->k == CGN_FIELDDECL) nfield++;
    if (!hasinit && nfield) {                                           /* sem init escrito: o motor cria um que so inicia os campos */
        CgxNode *fake = (CgxNode *)calloc(1, sizeof *fake); fake->k = CGN_FN; fake->s = (char *)"init"; fake->line = c->line; fake->ty = cgx_t(CGT_VOID); fake->has_ty = 1;
        in = cgx_dcat(PD, cgx_dhard(PD), py_fn(p, fake, c)); first = 0; free(fake);
    }
    for (int i = 0; i < c->b1.n; i++) {
        const CgxNode *m = c->b1.p[i];
        if (m->k != CGN_FN) continue;
        CgxDoc *d = py_fn(p, m, c);
        CgxDoc *x = first ? cgx_dcat(PD, cgx_dhard(PD), d) : cgx_dcatn(PD, cgx_dhard(PD), cgx_dhard(PD), d, NULL);
        in = in ? cgx_dcat(PD, in, x) : x; first = 0;
    }
    if (!in) in = cgx_dcat(PD, cgx_dhard(PD), py_t(p, "pass"));
    return cgx_dcatn(PD, cgx_dtextf(PD, "class %s:", c->s), cgx_dnest(PD, p->o->indent, in), NULL);
}

/* harness: uma linha por `test`, executavel depois do codigo; imprime `test N: ok` ou `test N: FAIL ...` */
static void py_tests(CgxPy *p, const CgxNode *prog, CgxB *out) {
    int n = 0;
    for (int i = 0; i < prog->b1.n; i++) if (prog->b1.p[i]->k == CGN_TEST) n++;
    if (!n) return;
    cgx_bs(out,
        "def _ucge_t(n, f, exp, mode):\n"
        "    try:\n"
        "        got = f()\n"
        "    except Exception as e:\n"
        "        print(\"test %d: %s\" % (n, \"ok\" if mode == 2 else \"FAIL raised %r\" % (e,)))\n"
        "        return\n"
        "    if mode == 2:\n"
        "        print(\"test %d: FAIL did not raise\" % n)\n"
        "        return\n"
        "    good = (got is True) if mode == 3 else (abs(got - exp) < 1e-9 if mode == 1 else got == exp)\n"
        "    print(\"test %d: %s\" % (n, \"ok\" if good else \"FAIL got %r expected %r\" % (got, exp)))\n");
    int k = 0;
    for (int i = 0; i < prog->b1.n; i++) {
        const CgxNode *t = prog->b1.p[i]; if (t->k != CGN_TEST) continue;
        CgxDoc *d = cgx_dcatn(PD, cgx_dtextf(PD, "_ucge_t(%d, lambda: ", ++k), py_ex(p, t->a, 0), py_t(p, ", "), t->b ? py_ex(p, t->b, 0) : py_t(p, "None"), cgx_dtextf(PD, ", %d)", (int)t->iv), NULL);
        cgx_render(d, 100000, out); cgx_bs(out, "\n");
    }
}

static int cgx_gen_python(Cgx *g, const CgxGenOpts *o, CgxB *code, CgxB *tests) {
    CgxPy P; memset(&P, 0, sizeof P); P.g = g; P.o = o; CgxPy *p = &P;
    const CgxNode *prog = g->prog; CgxDoc *all = NULL; int first = 1, prev_fn = 0;
    const int err0 = g->nerr_all;
    for (int i = 0; i < prog->b1.n; i++) {
        const CgxNode *d = prog->b1.p[i]; CgxDoc *x = NULL; int isfn = 0;
        if (d->k == CGN_FN) { x = py_fn(p, d, NULL); isfn = 1; }
        else if (d->k == CGN_CLASS) { x = py_class(p, d); isfn = 1; }
        else if (d->k == CGN_LET) { cgx_py_name(p, d->line, d->s, 0); x = cgx_dcatn(PD, cgx_dtextf(PD, "%s = ", d->s), py_ex(p, d->a, 0), NULL); }
        else if (d->k == CGN_RAW) x = py_raw(p, d);
        else if (d->k == CGN_COMMENT && o->comments && d->defs) x = cgx_dtextf(PD, "# %s", d->defs);
        if (!x) continue;
        if (!first) { const int blank = isfn || prev_fn ? 2 : 0; for (int b = 0; b <= blank; b++) x = cgx_dcat(PD, cgx_dhard(PD), x); }
        all = all ? cgx_dcat(PD, all, x) : x; first = 0; prev_fn = isfn;
    }
    CgxB body = {0};
    if (all) { cgx_render(all, o->width, &body); cgx_bs(&body, "\n"); }
    if (P.need_math) cgx_bs(code, "import math\n");
    if (P.need_re) cgx_bs(code, "import re\n");
    if (P.need_math || P.need_re) cgx_bs(code, "\n\n");
    if (body.p) cgx_bs(code, body.p);
    free(body.p);
    if (tests) py_tests(p, prog, tests);
    cgx_docs_free(&P.D);
    return g->nerr_all == err0;
}
static int cgx_gen_c(Cgx *g, const CgxGenOpts *o, CgxB *code, CgxB *tests);
static const CgxBackend CGX_BACKENDS[] = {
    {"python", "py", ".py", cgx_gen_python},
    {"c", "c99", ".c", cgx_gen_c},
    {NULL, NULL, NULL, NULL}};
static const CgxBackend *cgx_backend(const char *name) {
    for (int i = 0; CGX_BACKENDS[i].name; i++) if (!strcmp(CGX_BACKENDS[i].name, name) || !strcmp(CGX_BACKENDS[i].alias, name)) return &CGX_BACKENDS[i];
    return NULL;
}
