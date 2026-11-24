/* ---------- coder/ucge.c: Unified Code Generation Engine, parte 1: CSP -> UIR -> AST e validador (docs/ucge.md) ----------
 * O modelo NAO escreve o codigo final: descreve o que construir numa especificacao compacta (CSP, pseudocodigo com indentacao, ou o JSON
 * equivalente); o motor le a especificacao, monta a AST da UIR (independente de linguagem), valida (nomes, aridade, retorno, tipos
 * obvios, operacao desconhecida; erro com linha e o que esperava) e os geradores de ucgegen.c escrevem o codigo por AST (nunca por
 * concatenacao solta). O que a UIR nao expressa vai num bloco `raw`: codigo escrito pelo modelo, NAO confiavel, opaco para a AST.
 * Este arquivo e C puro (so a biblioteca padrao): compila sozinho (tools/ucge/ucgecli.c) e dentro do katana.c (coder/ucgetool.c liga
 * ao scanner, ao Edit por simbolo e a execucao dos testes). */

/* ---- tipos ---- */
enum { CGT_ANY, CGT_INT, CGT_FLOAT, CGT_STR, CGT_BOOL, CGT_VOID, CGT_LIST, CGT_MAP, CGT_CLASS, CGT_NULL };
typedef struct { int k, e; char cls[40]; } CgxTy;       /* e: tipo do elemento (lista) ou do valor (mapa), so o tipo base */

/* ---- nos da AST ---- */
enum { CGN_INT, CGN_FLT, CGN_STR, CGN_BOOL, CGN_NULL, CGN_VAR, CGN_BIN, CGN_UN, CGN_CALL, CGN_MCALL, CGN_FIELD, CGN_INDEX, CGN_LIST, CGN_MAP, CGN_TERN,
       CGN_LET, CGN_ASSIGN, CGN_RET, CGN_IF, CGN_WHILE, CGN_FOR, CGN_EACH, CGN_TRY, CGN_THROW, CGN_BREAK, CGN_CONT, CGN_EXPR, CGN_RAW, CGN_DOC, CGN_COMMENT,
       CGN_FN, CGN_CLASS, CGN_FIELDDECL, CGN_TEST, CGN_PROG };
enum { CGO_ADD, CGO_SUB, CGO_MUL, CGO_DIV, CGO_IDIV, CGO_MOD, CGO_EQ, CGO_NE, CGO_LT, CGO_LE, CGO_GT, CGO_GE, CGO_AND, CGO_OR, CGO_NOT, CGO_NEG, CGO_IN, CGO_APPROX, CGO_N };
static const char *const CGO_NAME[CGO_N] = {"ADD", "SUBTRACT", "MULTIPLY", "DIVIDE", "IDIVIDE", "MOD", "COMPARE ==", "COMPARE !=", "COMPARE <", "COMPARE <=", "COMPARE >", "COMPARE >=", "AND", "OR", "NOT", "NEGATE", "IN", "APPROX"};
static const char *const CGO_SYM[CGO_N] = {"+", "-", "*", "/", "//", "%", "==", "!=", "<", "<=", ">", ">=", "and", "or", "not", "-", "in", "~="};

typedef struct CgxNode CgxNode;
typedef struct { CgxNode **p; int n, cap; } CgxBlk;
typedef struct { char *name; CgxTy ty; int has_ty; CgxNode *def; int line; } CgxParam;
struct CgxNode {
    int k, op, line, flag;
    int id, sid;                  /* id estavel (ordem de pre-ordem na AST, 1..n); sid = simbolo internado de s (0 = sem nome) */
    char *s, *s2;                 /* nome (variavel, funcao, metodo, campo, variavel do laco); s2: segunda variavel (chave do mapa), variavel do catch */
    long long iv; double dv;
    CgxTy ty;                     /* tipo da expressao, ou o declarado (let, campo), ou o de retorno (funcao) */
    int has_ty;                   /* tipo escrito pelo autor (ou inferido e gravado) */
    CgxNode *a, *b, *c;
    CgxBlk b1, b2, b3;
    CgxParam *par; int npar;
    char *raw[4]; char *defs;     /* RAW: texto por linguagem (0 python, 1 js, 2 ts, 3 c); nomes que o bloco define; DOC: texto */
    int bi;                       /* chamada: id da funcao embutida (0 = da especificacao) */
    CgxNode *ref;                 /* chamada: funcao ou classe da especificacao; variavel: no do let que a declara */
};

#define CGX_MAXERR 24
typedef struct { int line; char msg[300]; } CgxErr;
typedef struct {
    char *text; char **ln; int *indent; int *no; unsigned char *cmt; int nl;
    CgxNode *prog;
    CgxErr err[CGX_MAXERR]; int nerr, nerr_all;
    void **mem; int nmem, capmem;
    int from_json; char *norm;    /* texto normalizado quando a especificacao veio em JSON */
    char **syms; int nsyms, capsyms; int *symtab, symtab_n;      /* simbolos internados: id 1..nsyms, tabela hash de enderecamento aberto */
    int nnodes;
} Cgx;

/* ---- memoria: tudo que o motor aloca num Cgx e solto de uma vez ---- */
static void *cgx_alloc(Cgx *g, size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) { fprintf(stderr, "ucge: sem memoria\n"); exit(1); }
    if (g->nmem == g->capmem) { g->capmem = g->capmem ? g->capmem * 2 : 256; g->mem = (void **)realloc(g->mem, sizeof(void *) * (size_t)g->capmem); }
    g->mem[g->nmem++] = p;
    return p;
}
static char *cgx_dupn(Cgx *g, const char *s, size_t n) { char *r = (char *)cgx_alloc(g, n + 1); memcpy(r, s, n); return r; }
static char *cgx_dup(Cgx *g, const char *s) { return cgx_dupn(g, s, strlen(s)); }
/* ---- simbolos internados: um nome, um id (comparar nomes vira comparar inteiros; base das tabelas de simbolos e do grafo de chamadas) ---- */
static unsigned cgx_hash(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
static int cgx_intern(Cgx *g, const char *s) {
    if (!s || !*s) return 0;
    if (g->nsyms * 2 >= g->symtab_n) {                                  /* cresce a tabela */
        const int nn = g->symtab_n ? g->symtab_n * 2 : 256; int *t = (int *)calloc((size_t)nn, sizeof(int));
        for (int i = 1; i <= g->nsyms; i++) { unsigned h = cgx_hash(g->syms[i]) % (unsigned)nn; while (t[h]) h = (h + 1) % (unsigned)nn; t[h] = i; }
        free(g->symtab); g->symtab = t; g->symtab_n = nn;
    }
    unsigned h = cgx_hash(s) % (unsigned)g->symtab_n;
    while (g->symtab[h]) { if (!strcmp(g->syms[g->symtab[h]], s)) return g->symtab[h]; h = (h + 1) % (unsigned)g->symtab_n; }
    if (g->nsyms + 2 > g->capsyms) { g->capsyms = g->capsyms ? g->capsyms * 2 : 128; g->syms = (char **)realloc(g->syms, sizeof(char *) * (size_t)g->capsyms); }
    g->syms[g->nsyms + 1] = strdup(s);
    g->symtab[h] = ++g->nsyms;
    return g->nsyms;
}
static const char *cgx_symname(const Cgx *g, int id) { return id > 0 && id <= g->nsyms ? g->syms[id] : ""; }
static void cgx_free(Cgx *g) {
    for (int i = 1; i <= g->nsyms; i++) free(g->syms[i]);
    free(g->syms); free(g->symtab);
    for (int i = 0; i < g->nmem; i++) free(g->mem[i]);
    free(g->mem); free(g->text); free(g->ln); free(g->indent); free(g->no); free(g->cmt); free(g->norm);
    memset(g, 0, sizeof *g);
}
static void cgx_err(Cgx *g, int line, const char *fmt, ...) {
    va_list ap;
    g->nerr_all++;
    if (g->nerr >= CGX_MAXERR) return;
    g->err[g->nerr].line = line;
    va_start(ap, fmt); vsnprintf(g->err[g->nerr].msg, sizeof g->err[0].msg, fmt, ap); va_end(ap);
    g->nerr++;
}
static CgxNode *cgx_node(Cgx *g, int k, int line) { CgxNode *n = (CgxNode *)cgx_alloc(g, sizeof *n); n->k = k; n->line = line; return n; }
static void cgx_blk_add(Cgx *g, CgxBlk *b, CgxNode *n) {
    if (b->n == b->cap) {
        const int nc = b->cap ? b->cap * 2 : 4;
        CgxNode **np = (CgxNode **)cgx_alloc(g, sizeof(CgxNode *) * (size_t)nc);
        if (b->n) memcpy(np, b->p, sizeof(CgxNode *) * (size_t)b->n);
        b->p = np; b->cap = nc;      /* o vetor velho fica na arena: solto no fim */
    }
    b->p[b->n++] = n;
}

/* ---- nomes de tipos ---- */
static void cgx_tyname(const CgxTy *t, char *o, size_t cap) {
    static const char *const nm[] = {"any", "int", "float", "str", "bool", "void", "list", "map", "class", "null"};
    if (t->k == CGT_CLASS) { snprintf(o, cap, "%s", t->cls); return; }
    if (t->k == CGT_LIST && t->e != CGT_ANY) { snprintf(o, cap, "list[%s]", nm[t->e]); return; }
    if (t->k == CGT_MAP && t->e != CGT_ANY) { snprintf(o, cap, "map[str,%s]", nm[t->e]); return; }
    snprintf(o, cap, "%s", nm[t->k]);
}
static CgxTy cgx_t(int k) { CgxTy t; memset(&t, 0, sizeof t); t.k = k; return t; }
static int cgx_isnum(const CgxTy *t) { return t->k == CGT_INT || t->k == CGT_FLOAT; }
/* src pode ser guardado em dst? (any aceita tudo; int vira float; null so em tipos que aceitam nulo) */
static int cgx_assignable(const CgxTy *d, const CgxTy *s) {
    if (d->k == CGT_ANY || s->k == CGT_ANY) return 1;
    if (s->k == CGT_NULL) return d->k != CGT_INT && d->k != CGT_FLOAT && d->k != CGT_BOOL;
    if (d->k == CGT_FLOAT && s->k == CGT_INT) return 1;
    if (d->k != s->k) return 0;
    if (d->k == CGT_CLASS) return !strcmp(d->cls, s->cls);
    if ((d->k == CGT_LIST || d->k == CGT_MAP) && d->e != CGT_ANY && s->e != CGT_ANY) return d->e == s->e || (d->e == CGT_FLOAT && s->e == CGT_INT);
    return 1;
}

/* ---- linhas ---- */
static int cgx_is_comment_line(const char *p) { return *p == '#' || (p[0] == '/' && p[1] == '/') ; }
static int cgx_load(Cgx *g, const char *src) {
    g->text = strdup(src);
    int cap = 64; g->ln = (char **)malloc(sizeof(char *) * (size_t)cap); g->indent = (int *)malloc(sizeof(int) * (size_t)cap); g->no = (int *)malloc(sizeof(int) * (size_t)cap); g->cmt = (unsigned char *)malloc((size_t)cap);
    char *p = g->text; int no = 0;
    while (*p || no == 0) {
        char *e = strchr(p, '\n'); char *next = e ? e + 1 : p + strlen(p);
        if (e) *e = 0;
        size_t l = strlen(p); if (l && p[l - 1] == '\r') p[l - 1] = 0;
        no++;
        if (g->nl == cap) { cap *= 2; g->ln = (char **)realloc(g->ln, sizeof(char *) * (size_t)cap); g->indent = (int *)realloc(g->indent, sizeof(int) * (size_t)cap); g->no = (int *)realloc(g->no, sizeof(int) * (size_t)cap); g->cmt = (unsigned char *)realloc(g->cmt, (size_t)cap); }
        int ind = 0; const char *q = p;
        while (*q == ' ' || *q == '\t') { ind += *q == '\t' ? 4 : 1; q++; }
        g->ln[g->nl] = p; g->no[g->nl] = no;
        g->indent[g->nl] = *q ? ind : -1;                        /* -1: linha em branco */
        g->cmt[g->nl] = *q && cgx_is_comment_line(q);
        g->nl++;
        if (!e) break;
        p = next;
    }
    return 1;
}

/* ---- lexico de uma linha ---- */
enum { TK_END, TK_NUM, TK_FLT, TK_STR, TK_ID, TK_OP };
typedef struct { int t, n; const char *p; char *str; long long iv; double dv; } CgxTok;
#define CGX_MAXTOK 600
static int cgx_op_at(const char *p) {
    static const char *const ops3[] = {"===", "!==", NULL};
    static const char *const ops2[] = {"==", "!=", "<=", ">=", "//", "..", "+=", "-=", "*=", "/=", "%=", "~=", "&&", "||", "->", NULL};
    for (int i = 0; ops3[i]; i++) if (!strncmp(p, ops3[i], 3)) return 3;
    for (int i = 0; ops2[i]; i++) if (!strncmp(p, ops2[i], 2)) return 2;
    return strchr("+-*/%<>=()[]{},:.?!", *p) ? 1 : 0;
}
static int cgx_lex(Cgx *g, const char *s, int line, CgxTok *t) {
    int n = 0; const char *p = s;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;
        if (n >= CGX_MAXTOK - 1) { cgx_err(g, line, "line too long (more than %d tokens); split it with `let`", CGX_MAXTOK); return -1; }
        CgxTok *k = &t[n]; memset(k, 0, sizeof *k); k->p = p;
        if (isdigit((unsigned char)*p)) {
            const char *q = p; int fl = 0;
            while (isdigit((unsigned char)*q)) q++;
            if (*q == '.' && isdigit((unsigned char)q[1])) { fl = 1; q++; while (isdigit((unsigned char)*q)) q++; }
            if ((*q == 'e' || *q == 'E') && (isdigit((unsigned char)q[1]) || ((q[1] == '-' || q[1] == '+') && isdigit((unsigned char)q[2])))) { fl = 1; q += 2; while (isdigit((unsigned char)*q)) q++; }
            char tmp[64]; size_t l = (size_t)(q - p); if (l >= sizeof tmp) l = sizeof tmp - 1; memcpy(tmp, p, l); tmp[l] = 0;
            if (fl) { k->t = TK_FLT; k->dv = atof(tmp); } else { k->t = TK_NUM; k->iv = atoll(tmp); }
            k->n = (int)(q - p); p = q;
        } else if (isalpha((unsigned char)*p) || *p == '_') {
            const char *q = p; while (isalnum((unsigned char)*q) || *q == '_') q++;
            k->t = TK_ID; k->n = (int)(q - p); p = q;
        } else if (*p == '"' || *p == '\'') {
            const char qc = *p; const char *q = p + 1;
            char *o = (char *)cgx_alloc(g, strlen(q) + 2); size_t w = 0;
            while (*q && *q != qc) {
                if (*q == '\\' && q[1]) {
                    q++;
                    switch (*q) { case 'n': o[w++] = '\n'; break; case 't': o[w++] = '\t'; break; case 'r': o[w++] = '\r'; break; case '0': o[w++] = 0; break; default: o[w++] = *q; }
                    q++;
                } else o[w++] = *q++;
            }
            if (*q != qc) { cgx_err(g, line, "unterminated string (expected a closing %c)", qc); return -1; }
            k->t = TK_STR; k->str = o; k->n = (int)(q + 1 - p); p = q + 1;
        } else {
            const int l = cgx_op_at(p);
            if (!l) { cgx_err(g, line, "unexpected character `%c` (expected a name, number, string or operator)", *p); return -1; }
            k->t = TK_OP; k->n = l; p += l;
        }
        n++;
    }
    memset(&t[n], 0, sizeof t[n]); t[n].t = TK_END; t[n].p = p;
    return n;
}

/* ---- parser de expressoes ---- */
typedef struct { Cgx *g; CgxTok *t; int n, i, line, bad; } CgxP;
static int cgx_tis(const CgxTok *k, const char *s) { const size_t l = strlen(s); return (k->t == TK_ID || k->t == TK_OP) && (size_t)k->n == l && !strncmp(k->p, s, l); }
static int cgx_is(CgxP *p, const char *s) { return cgx_tis(&p->t[p->i], s); }
static int cgx_acc(CgxP *p, const char *s) { if (cgx_is(p, s)) { p->i++; return 1; } return 0; }
static void cgx_tokdesc(const CgxTok *k, char *o, size_t cap) {
    if (k->t == TK_END) snprintf(o, cap, "the end of the line");
    else if (k->t == TK_STR) snprintf(o, cap, "a string");
    else snprintf(o, cap, "`%.*s`", k->n > 30 ? 30 : k->n, k->p);
}
static void cgx_perr(CgxP *p, const char *want) {
    if (p->bad) return;
    p->bad = 1;
    char d[80]; cgx_tokdesc(&p->t[p->i], d, sizeof d);
    cgx_err(p->g, p->line, "expected %s, found %s", want, d);
}
static int cgx_expect(CgxP *p, const char *s) { if (cgx_acc(p, s)) return 1; char w[40]; snprintf(w, sizeof w, "`%s`", s); cgx_perr(p, w); return 0; }
static CgxNode *cgx_pe(CgxP *p);

static CgxNode *cgx_mk(CgxP *p, int k) { return cgx_node(p->g, k, p->line); }
static CgxNode *cgx_bin(CgxP *p, int op, CgxNode *a, CgxNode *b) { CgxNode *n = cgx_mk(p, CGN_BIN); n->op = op; n->a = a; n->b = b; return n; }
static int cgx_ident_ok(const CgxTok *k) { return k->t == TK_ID; }
static char *cgx_tokname(CgxP *p) { const CgxTok *k = &p->t[p->i]; return cgx_dupn(p->g, k->p, (size_t)k->n); }

/* tipo escrito: int float str bool any void, list[T], list<T>, T[], map[str,T], nome de classe; sinonimos comuns (string, number, boolean, dict...) */
static int cgx_base_ty(const char *s, size_t n) {
    static const struct { const char *a; int k; } tb[] = {{"int", CGT_INT}, {"integer", CGT_INT}, {"long", CGT_INT}, {"float", CGT_FLOAT}, {"double", CGT_FLOAT}, {"number", CGT_FLOAT}, {"str", CGT_STR}, {"string", CGT_STR},
        {"bool", CGT_BOOL}, {"boolean", CGT_BOOL}, {"any", CGT_ANY}, {"void", CGT_VOID}, {"none", CGT_VOID}, {"list", CGT_LIST}, {"array", CGT_LIST}, {"map", CGT_MAP}, {"dict", CGT_MAP}, {NULL, 0}};
    for (int i = 0; tb[i].a; i++) if (strlen(tb[i].a) == n && !strncmp(tb[i].a, s, n)) return tb[i].k;
    return -1;
}
static int cgx_ptype(CgxP *p, CgxTy *t) {
    *t = cgx_t(CGT_ANY);
    const CgxTok *k = &p->t[p->i];
    if (!cgx_ident_ok(k)) { cgx_perr(p, "a type (int, float, str, bool, list[T], map[str,T], any, void or a class name)"); return 0; }
    const int b = cgx_base_ty(k->p, (size_t)k->n);
    p->i++;
    if (b < 0) { t->k = CGT_CLASS; snprintf(t->cls, sizeof t->cls, "%.*s", k->n, k->p); }
    else t->k = b;
    if (t->k == CGT_LIST || t->k == CGT_MAP) {
        const int br = cgx_is(p, "[") ? 1 : cgx_is(p, "<") ? 2 : 0;
        if (br) {
            p->i++;
            CgxTy a; if (!cgx_ptype(p, &a)) return 0;
            if (t->k == CGT_MAP && cgx_acc(p, ",")) { if (a.k != CGT_STR) { cgx_err(p->g, p->line, "map keys must be str (the key type is `str`; found %d)", a.k); p->bad = 1; return 0; } if (!cgx_ptype(p, &a)) return 0; }
            t->e = a.k == CGT_CLASS || a.k == CGT_LIST || a.k == CGT_MAP ? CGT_ANY : a.k;
            if (!cgx_expect(p, br == 1 ? "]" : ">")) return 0;
        }
    }
    while (cgx_is(p, "[") && p->t[p->i + 1].t == TK_OP && cgx_tis(&p->t[p->i + 1], "]")) { p->i += 2; CgxTy in = *t; *t = cgx_t(CGT_LIST); t->e = in.k == CGT_CLASS || in.k == CGT_LIST || in.k == CGT_MAP ? CGT_ANY : in.k; }
    return 1;
}

static CgxNode *cgx_args(CgxP *p, CgxNode *call) {            /* depois do "(" ; argumentos em b1 */
    if (!cgx_acc(p, ")")) {
        for (;;) {
            CgxNode *a = cgx_pe(p); if (!a) return NULL;
            cgx_blk_add(p->g, &call->b1, a);
            if (cgx_acc(p, ",")) continue;
            if (!cgx_expect(p, ")")) return NULL;
            break;
        }
    }
    return call;
}
static CgxNode *cgx_primary(CgxP *p) {
    const CgxTok *k = &p->t[p->i];
    if (p->bad) return NULL;
    if (k->t == TK_NUM) { p->i++; CgxNode *n = cgx_mk(p, CGN_INT); n->iv = k->iv; n->ty = cgx_t(CGT_INT); return n; }
    if (k->t == TK_FLT) { p->i++; CgxNode *n = cgx_mk(p, CGN_FLT); n->dv = k->dv; n->ty = cgx_t(CGT_FLOAT); return n; }
    if (k->t == TK_STR) { p->i++; CgxNode *n = cgx_mk(p, CGN_STR); n->s = k->str; n->ty = cgx_t(CGT_STR); return n; }
    if (cgx_tis(k, "(")) { p->i++; CgxNode *e = cgx_pe(p); if (!e) return NULL; if (!cgx_expect(p, ")")) return NULL; return e; }
    if (cgx_tis(k, "[")) {
        p->i++; CgxNode *n = cgx_mk(p, CGN_LIST);
        if (!cgx_acc(p, "]")) for (;;) {
            CgxNode *a = cgx_pe(p); if (!a) return NULL;
            cgx_blk_add(p->g, &n->b1, a);
            if (cgx_acc(p, ",")) { if (cgx_acc(p, "]")) break; continue; }
            if (!cgx_expect(p, "]")) return NULL;
            break;
        }
        return n;
    }
    if (cgx_tis(k, "{")) {
        p->i++; CgxNode *n = cgx_mk(p, CGN_MAP);
        if (!cgx_acc(p, "}")) for (;;) {
            CgxNode *a = cgx_pe(p); if (!a) return NULL;
            if (!cgx_expect(p, ":")) return NULL;
            CgxNode *b = cgx_pe(p); if (!b) return NULL;
            cgx_blk_add(p->g, &n->b1, a); cgx_blk_add(p->g, &n->b1, b);
            if (cgx_acc(p, ",")) { if (cgx_acc(p, "}")) break; continue; }
            if (!cgx_expect(p, "}")) return NULL;
            break;
        }
        return n;
    }
    if (k->t == TK_ID) {
        if (cgx_tis(k, "true") || cgx_tis(k, "True")) { p->i++; CgxNode *n = cgx_mk(p, CGN_BOOL); n->iv = 1; n->ty = cgx_t(CGT_BOOL); return n; }
        if (cgx_tis(k, "false") || cgx_tis(k, "False")) { p->i++; CgxNode *n = cgx_mk(p, CGN_BOOL); n->iv = 0; n->ty = cgx_t(CGT_BOOL); return n; }
        if (cgx_tis(k, "null") || cgx_tis(k, "None") || cgx_tis(k, "nil")) { p->i++; CgxNode *n = cgx_mk(p, CGN_NULL); n->ty = cgx_t(CGT_NULL); return n; }
        if (cgx_tis(k, "new") && p->t[p->i + 1].t == TK_ID) p->i++;
        k = &p->t[p->i];
        char *name = cgx_tokname(p); p->i++;
        if (cgx_is(p, "(")) { p->i++; CgxNode *c = cgx_mk(p, CGN_CALL); c->s = name; return cgx_args(p, c); }
        CgxNode *n = cgx_mk(p, CGN_VAR); n->s = name; return n;
    }
    cgx_perr(p, "an expression (a number, string, name, `(`, `[` or `{`)");
    return NULL;
}
static CgxNode *cgx_postfix(CgxP *p) {
    CgxNode *e = cgx_primary(p);
    while (e && !p->bad) {
        if (cgx_is(p, "[")) {
            p->i++; CgxNode *i = cgx_pe(p); if (!i) return NULL;
            if (!cgx_expect(p, "]")) return NULL;
            CgxNode *n = cgx_mk(p, CGN_INDEX); n->a = e; n->b = i; e = n;
        } else if (cgx_is(p, ".") ) {
            p->i++;
            if (!cgx_ident_ok(&p->t[p->i])) { cgx_perr(p, "a field or method name after `.`"); return NULL; }
            char *name = cgx_tokname(p); p->i++;
            if (cgx_is(p, "(")) { p->i++; CgxNode *c = cgx_mk(p, CGN_MCALL); c->a = e; c->s = name; if (!cgx_args(p, c)) return NULL; e = c; }
            else { CgxNode *f = cgx_mk(p, CGN_FIELD); f->a = e; f->s = name; e = f; }
        } else break;
    }
    return e;
}
static CgxNode *cgx_unary(CgxP *p) {
    if (cgx_is(p, "-")) {
        p->i++; CgxNode *a = cgx_unary(p); if (!a) return NULL;
        if (a->k == CGN_INT) { a->iv = -a->iv; return a; }
        if (a->k == CGN_FLT) { a->dv = -a->dv; return a; }
        CgxNode *n = cgx_mk(p, CGN_UN); n->op = CGO_NEG; n->a = a; return n;
    }
    return cgx_postfix(p);
}
static CgxNode *cgx_mul(CgxP *p) {
    CgxNode *l = cgx_unary(p);
    while (l && (cgx_is(p, "*") || cgx_is(p, "/") || cgx_is(p, "//") || cgx_is(p, "%"))) {
        const int op = cgx_is(p, "*") ? CGO_MUL : cgx_is(p, "/") ? CGO_DIV : cgx_is(p, "//") ? CGO_IDIV : CGO_MOD;
        p->i++; CgxNode *r = cgx_unary(p); if (!r) return NULL;
        l = cgx_bin(p, op, l, r);
    }
    return l;
}
static CgxNode *cgx_add(CgxP *p) {
    CgxNode *l = cgx_mul(p);
    while (l && (cgx_is(p, "+") || cgx_is(p, "-"))) {
        const int op = cgx_is(p, "+") ? CGO_ADD : CGO_SUB;
        p->i++; CgxNode *r = cgx_mul(p); if (!r) return NULL;
        l = cgx_bin(p, op, l, r);
    }
    return l;
}
static CgxNode *cgx_cmp(CgxP *p) {
    CgxNode *l = cgx_add(p);
    if (!l) return NULL;
    static const struct { const char *s; int op; } cm[] = {{"===", CGO_EQ}, {"!==", CGO_NE}, {"==", CGO_EQ}, {"!=", CGO_NE}, {"<=", CGO_LE}, {">=", CGO_GE}, {"<", CGO_LT}, {">", CGO_GT}, {"~=", CGO_APPROX}, {"in", CGO_IN}, {NULL, 0}};
    int neg = 0;
    if (cgx_is(p, "not") && cgx_tis(&p->t[p->i + 1], "in")) { p->i++; neg = 1; }
    for (int i = 0; cm[i].s; i++) if (cgx_is(p, cm[i].s)) {
        p->i++; CgxNode *r = cgx_add(p); if (!r) return NULL;
        CgxNode *n = cgx_bin(p, cm[i].op, l, r);
        if (neg) { CgxNode *u = cgx_mk(p, CGN_UN); u->op = CGO_NOT; u->a = n; return u; }
        return n;
    }
    if (neg) { cgx_perr(p, "`in` after `not`"); return NULL; }
    return l;
}
static CgxNode *cgx_not(CgxP *p) {
    if (cgx_is(p, "not") || (cgx_is(p, "!") )) { p->i++; CgxNode *a = cgx_not(p); if (!a) return NULL; CgxNode *n = cgx_mk(p, CGN_UN); n->op = CGO_NOT; n->a = a; return n; }
    return cgx_cmp(p);
}
static CgxNode *cgx_and(CgxP *p) {
    CgxNode *l = cgx_not(p);
    while (l && (cgx_is(p, "and") || cgx_is(p, "&&"))) { p->i++; CgxNode *r = cgx_not(p); if (!r) return NULL; l = cgx_bin(p, CGO_AND, l, r); }
    return l;
}
static CgxNode *cgx_or(CgxP *p) {
    CgxNode *l = cgx_and(p);
    while (l && (cgx_is(p, "or") || cgx_is(p, "||"))) { p->i++; CgxNode *r = cgx_and(p); if (!r) return NULL; l = cgx_bin(p, CGO_OR, l, r); }
    return l;
}
static CgxNode *cgx_pe(CgxP *p) {
    CgxNode *c = cgx_or(p);
    if (c && cgx_is(p, "?")) {
        p->i++; CgxNode *a = cgx_pe(p); if (!a) return NULL;
        if (!cgx_expect(p, ":")) return NULL;
        CgxNode *b = cgx_pe(p); if (!b) return NULL;
        CgxNode *n = cgx_mk(p, CGN_TERN); n->a = c; n->b = a; n->c = b; return n;
    }
    return c;
}

/* ---- parser de instrucoes ---- */
typedef struct { Cgx *g; int li; } CgxL;           /* posicao nas linhas */
static void cgx_block(Cgx *g, int *li, int ind, CgxBlk *out, int ctx);

static int cgx_word_is(const char *p, const char *w) { const size_t l = strlen(w); return !strncmp(p, w, l) && !(isalnum((unsigned char)p[l]) || p[l] == '_'); }
static int cgx_next_real(Cgx *g, int li) { while (li < g->nl && (g->indent[li] < 0 || g->cmt[li])) li++; return li; }

/* corpo indentado de um cabecalho (indent ind): devolve 0 e erra se nao houver */
static int cgx_body(Cgx *g, int *li, int ind, CgxBlk *out, int ctx, int hdrline, const char *what) {
    const int nx = cgx_next_real(g, *li);
    if (nx >= g->nl || g->indent[nx] <= ind) { cgx_err(g, hdrline, "expected an indented block after %s", what); return 0; }
    cgx_block(g, li, g->indent[nx], out, ctx);
    return 1;
}
static void cgx_text_trim(char *s) { size_t l = strlen(s); while (l && isspace((unsigned char)s[l - 1])) s[--l] = 0; }

/* bloco raw: linhas com mais indentacao que o cabecalho, reindentadas */
static char *cgx_raw_text(Cgx *g, int *li, int ind) {
    int a = *li, b = *li;
    while (b < g->nl && (g->indent[b] < 0 || g->indent[b] > ind)) b++;
    while (b > a && g->indent[b - 1] < 0) b--;
    int mi = 1 << 20;
    for (int i = a; i < b; i++) if (g->indent[i] >= 0 && g->indent[i] < mi) mi = g->indent[i];
    size_t cap = 16; for (int i = a; i < b; i++) cap += strlen(g->ln[i]) + 2;
    char *o = (char *)cgx_alloc(g, cap); size_t w = 0;
    for (int i = a; i < b; i++) {
        if (g->indent[i] >= 0) {
            const char *q = g->ln[i]; int skip = mi;
            while (skip > 0 && (*q == ' ' || *q == '\t')) { skip -= *q == '\t' ? 4 : 1; q++; }
            w += (size_t)snprintf(o + w, cap - w, "%s", q);
        }
        o[w++] = '\n';
    }
    o[w] = 0;
    *li = b;
    return b > a ? o : NULL;
}
static int cgx_raw_lang(const char *s) {
    if (!strcmp(s, "python") || !strcmp(s, "py")) return 0;
    if (!strcmp(s, "js") || !strcmp(s, "javascript") || !strcmp(s, "node")) return 1;
    if (!strcmp(s, "ts") || !strcmp(s, "typescript")) return 2;
    if (!strcmp(s, "c")) return 3;
    return -1;
}
/* `raw python [defines a, b]` + bloco. Blocos raw seguidos de linguagens diferentes viram um no so */
static CgxNode *cgx_raw_stmt(Cgx *g, int *li, int ind) {
    CgxNode *n = cgx_node(g, CGN_RAW, g->no[*li]);
    for (;;) {
        const int hl = *li; const char *h = g->ln[hl];
        while (*h == ' ' || *h == '\t') h++;
        h += 3;
        while (*h == ' ') h++;
        char lang[24]; int w = 0;
        while (isalpha((unsigned char)*h) && w < 20) lang[w++] = *h++;
        lang[w] = 0;
        const int L = cgx_raw_lang(lang);
        if (L < 0) { cgx_err(g, g->no[hl], "unknown raw language `%s` (expected python, js, ts or c)", lang); (*li)++; cgx_raw_text(g, li, ind); return NULL; }
        while (*h == ' ') h++;
        if (cgx_word_is(h, "defines")) {
            h += 7; char *d = cgx_dup(g, h); char *hash = strchr(d, '#'); if (hash) *hash = 0; cgx_text_trim(d);
            for (char *q = d; *q; q++) if (*q == ',') *q = ' ';
            n->defs = n->defs ? n->defs : d;
            if (n->defs != d) { char *m = (char *)cgx_alloc(g, strlen(n->defs) + strlen(d) + 2); sprintf(m, "%s %s", n->defs, d); n->defs = m; }
        }
        (*li)++;
        char *txt = cgx_raw_text(g, li, ind);
        if (!txt) { cgx_err(g, g->no[hl], "expected an indented block of code after `raw %s`", lang); return NULL; }
        if (n->raw[L]) cgx_err(g, g->no[hl], "two `raw %s` blocks in a row", lang);
        n->raw[L] = txt;
        const int nx = *li;
        if (nx < g->nl && g->indent[nx] == ind && !g->cmt[nx]) { const char *q = g->ln[nx]; while (*q == ' ' || *q == '\t') q++; if (cgx_word_is(q, "raw")) continue; }
        break;
    }
    return n;
}

static int cgx_parse_params(CgxP *p, CgxNode *fn, int in_class) {
    if (!cgx_expect(p, "(")) return 0;
    CgxParam tmp[40]; int np = 0;
    if (!cgx_acc(p, ")")) for (;;) {
        if (!cgx_ident_ok(&p->t[p->i])) { cgx_perr(p, "a parameter name"); return 0; }
        if (np >= 40) { cgx_err(p->g, p->line, "too many parameters (40 at most)"); return 0; }
        CgxParam *pr = &tmp[np]; memset(pr, 0, sizeof *pr); pr->name = cgx_tokname(p); pr->line = p->line; p->i++;
        if (cgx_acc(p, ":")) { if (!cgx_ptype(p, &pr->ty)) return 0; pr->has_ty = 1; }
        if (cgx_acc(p, "=")) { pr->def = cgx_pe(p); if (!pr->def) return 0; }
        if (in_class && np == 0 && (!strcmp(pr->name, "self") || !strcmp(pr->name, "this"))) { /* self explicito: descartado */ }
        else np++;
        if (cgx_acc(p, ",")) continue;
        if (!cgx_expect(p, ")")) return 0;
        break;
    }
    fn->par = (CgxParam *)cgx_alloc(p->g, sizeof(CgxParam) * (size_t)(np ? np : 1)); memcpy(fn->par, tmp, sizeof(CgxParam) * (size_t)np); fn->npar = np;
    return 1;
}

static CgxNode *cgx_fn_stmt(Cgx *g, CgxTok *tk, int nt, int *li, int ind, int in_class) {
    const int line = g->no[*li];
    CgxP p = {g, tk, nt, 1, line, 0};
    CgxNode *fn = cgx_node(g, CGN_FN, line);
    if (!cgx_ident_ok(&p.t[p.i])) { cgx_perr(&p, "a function name after `fn`"); (*li)++; return NULL; }
    fn->s = cgx_tokname(&p); p.i++;
    if (!cgx_parse_params(&p, fn, in_class)) { (*li)++; return NULL; }
    if (cgx_acc(&p, "->")) { if (!cgx_ptype(&p, &fn->ty)) { (*li)++; return NULL; } fn->has_ty = 1; }
    if (p.t[p.i].t != TK_END) { cgx_perr(&p, "the end of the header (use `-> type` for the return type)"); (*li)++; return NULL; }
    (*li)++;
    cgx_body(g, li, ind, &fn->b1, 1, line, "the function header");
    return fn;
}

/* uma instrucao ou declaracao que comeca em *li (indent ind) */
static CgxNode *cgx_stmt(Cgx *g, int *li, int ind, int ctx) {
    const int line = g->no[*li];
    const char *src = g->ln[*li]; while (*src == ' ' || *src == '\t') src++;
    if (cgx_word_is(src, "raw")) return cgx_raw_stmt(g, li, ind);
    CgxTok tk[CGX_MAXTOK]; const int nt = cgx_lex(g, src, line, tk);
    if (nt < 0) { (*li)++; return NULL; }
    CgxP p = {g, tk, nt, 0, line, 0};
    /* `:` no fim do cabecalho (estilo Python) e aceito e ignorado */
    if (nt > 1 && cgx_tis(&tk[nt - 1], ":") && tk[0].t == TK_ID && !cgx_tis(&tk[0], "doc")) { memset(&tk[nt - 1], 0, sizeof tk[0]); tk[nt - 1].t = TK_END; tk[nt - 1].p = src + strlen(src); p.n = nt - 1; }
    const CgxTok *f = &tk[0];
    CgxNode *n = NULL;
    if (cgx_tis(f, "doc")) {
        n = cgx_node(g, CGN_DOC, line);
        const char *q = src + 3; while (*q == ' ' || *q == ':') q++;
        char *t = cgx_dup(g, q); cgx_text_trim(t);
        size_t l = strlen(t); if (l >= 2 && (t[0] == '"' || t[0] == '\'') && t[l - 1] == t[0]) { t[l - 1] = 0; t++; }
        n->defs = t; (*li)++;
        return n;
    }
    if (cgx_tis(f, "fn") || cgx_tis(f, "def") || cgx_tis(f, "function") || cgx_tis(f, "func")) { p.i = 1; return cgx_fn_stmt(g, tk, p.n, li, ind, ctx == 2); }
    if (cgx_tis(f, "class")) {
        p.i = 1;
        if (ctx != 0) { cgx_err(g, line, "a class can only be declared at the top level"); (*li)++; return NULL; }
        n = cgx_node(g, CGN_CLASS, line);
        if (!cgx_ident_ok(&p.t[p.i])) { cgx_perr(&p, "a class name"); (*li)++; return NULL; }
        n->s = cgx_tokname(&p); p.i++;
        if (cgx_is(&p, "extends") || cgx_is(&p, "(")) { cgx_err(g, line, "inheritance is not part of the UIR (L3): declare the class without a base, or write it in a raw block"); (*li)++; return NULL; }
        if (p.t[p.i].t != TK_END) { cgx_perr(&p, "the end of the class header"); (*li)++; return NULL; }
        (*li)++;
        cgx_body(g, li, ind, &n->b1, 2, line, "the class header");
        return n;
    }
    if (cgx_tis(f, "field") && ctx == 2) {
        p.i = 1; n = cgx_node(g, CGN_FIELDDECL, line);
        if (!cgx_ident_ok(&p.t[p.i])) { cgx_perr(&p, "a field name"); (*li)++; return NULL; }
        n->s = cgx_tokname(&p); p.i++;
        if (cgx_acc(&p, ":")) { if (!cgx_ptype(&p, &n->ty)) { (*li)++; return NULL; } n->has_ty = 1; }
        if (cgx_acc(&p, "=")) { n->a = cgx_pe(&p); if (!n->a) { (*li)++; return NULL; } }
        if (p.t[p.i].t != TK_END) { cgx_perr(&p, "the end of the field line"); (*li)++; return NULL; }
        (*li)++; return n;
    }
    if (cgx_tis(f, "test")) {
        if (ctx != 0) { cgx_err(g, line, "`test` lines go at the top level, after the functions"); (*li)++; return NULL; }
        p.i = 1; n = cgx_node(g, CGN_TEST, line);
        int raises = 0;
        if (p.n > 2 && cgx_tis(&tk[p.n - 1], "raises")) { raises = 1; memset(&tk[p.n - 1], 0, sizeof tk[0]); tk[p.n - 1].t = TK_END; tk[p.n - 1].p = src + strlen(src); p.n--; }
        CgxNode *e = cgx_pe(&p);
        if (!e || p.bad) { (*li)++; return NULL; }
        if (p.t[p.i].t != TK_END) { cgx_perr(&p, "the end of the test (`test f(1, 2) == 3`)"); (*li)++; return NULL; }
        if (raises) { n->a = e; n->iv = 2; }
        else if (e->k == CGN_BIN && (e->op == CGO_EQ || e->op == CGO_APPROX)) { n->a = e->a; n->b = e->b; n->iv = e->op == CGO_APPROX ? 1 : 0; }
        else { n->a = e; n->iv = 3; }
        (*li)++; return n;
    }
    if (ctx == 2) { cgx_err(g, line, "inside a class only `field` and `fn` lines are allowed (found `%.*s`)", f->n, f->p); (*li)++; return NULL; }
    if (cgx_tis(f, "import") || cgx_tis(f, "from") || cgx_tis(f, "include") || cgx_tis(f, "require")) { cgx_err(g, line, "imports are not part of the UIR (L3 modules and APIs): put them in a `raw` block"); (*li)++; return NULL; }
    if (ctx == 0 && !(cgx_tis(f, "let") || cgx_tis(f, "const") || cgx_tis(f, "var"))) { cgx_err(g, line, "at the top level expected `fn`, `class`, `const`, `test` or `raw` (found `%.*s`)", f->n, f->p); (*li)++; return NULL; }
    if (cgx_tis(f, "let") || cgx_tis(f, "const") || cgx_tis(f, "var")) {
        p.i = 1; n = cgx_node(g, CGN_LET, line);
        if (!cgx_ident_ok(&p.t[p.i])) { cgx_perr(&p, "a variable name after `let`"); (*li)++; return NULL; }
        n->s = cgx_tokname(&p); p.i++;
        if (cgx_acc(&p, ":")) { if (!cgx_ptype(&p, &n->ty)) { (*li)++; return NULL; } n->has_ty = 1; }
        if (!cgx_expect(&p, "=")) { (*li)++; return NULL; }
        n->a = cgx_pe(&p);
        if (!n->a) { (*li)++; return NULL; }
        if (p.t[p.i].t != TK_END) { cgx_perr(&p, "the end of the line (one statement per line)"); (*li)++; return NULL; }
        if (ctx == 0) n->flag = 1;      /* constante de modulo */
        (*li)++; return n;
    }
    if (cgx_tis(f, "return")) {
        p.i = 1; n = cgx_node(g, CGN_RET, line);
        if (p.t[p.i].t != TK_END) { n->a = cgx_pe(&p); if (!n->a) { (*li)++; return NULL; } }
        if (p.t[p.i].t != TK_END) { cgx_perr(&p, "the end of the line after `return`"); (*li)++; return NULL; }
        (*li)++; return n;
    }
    if (cgx_tis(f, "throw") || cgx_tis(f, "raise")) {
        p.i = 1; n = cgx_node(g, CGN_THROW, line);
        n->a = cgx_pe(&p);
        if (!n->a) { (*li)++; return NULL; }
        if (p.t[p.i].t != TK_END) { cgx_perr(&p, "the end of the line after `throw`"); (*li)++; return NULL; }
        (*li)++; return n;
    }
    if (cgx_tis(f, "break") || cgx_tis(f, "continue") || cgx_tis(f, "pass")) {
        n = cgx_node(g, cgx_tis(f, "break") ? CGN_BREAK : cgx_tis(f, "continue") ? CGN_CONT : CGN_COMMENT, line);
        if (n->k == CGN_COMMENT) { n->defs = NULL; }
        if (tk[1].t != TK_END) { p.i = 1; cgx_perr(&p, "the end of the line"); (*li)++; return NULL; }
        (*li)++; return n->k == CGN_COMMENT ? NULL : n;
    }
    if (cgx_tis(f, "if")) {
        p.i = 1; n = cgx_node(g, CGN_IF, line);
        n->a = cgx_pe(&p);
        if (!n->a || p.t[p.i].t != TK_END) { if (n->a) cgx_perr(&p, "the end of the condition"); (*li)++; /* pula o corpo */ { const int nx = cgx_next_real(g, *li); if (nx < g->nl && g->indent[nx] > ind) { CgxBlk tmp = {0}; cgx_block(g, li, g->indent[nx], &tmp, 1); } } return NULL; }
        (*li)++;
        cgx_body(g, li, ind, &n->b1, 1, line, "`if`");
        /* elif / else if / else no mesmo nivel */
        CgxNode *cur = n;
        for (;;) {
            const int nx = cgx_next_real(g, *li);
            if (nx >= g->nl || g->indent[nx] != ind) break;
            const char *q = g->ln[nx]; while (*q == ' ' || *q == '\t') q++;
            const int isel = cgx_word_is(q, "elif") || cgx_word_is(q, "elsif");
            const int iselif = cgx_word_is(q, "else") && ({ const char *r = q + 4; while (*r == ' ') r++; cgx_word_is(r, "if"); });
            if (isel || iselif) {
                *li = nx;
                CgxTok t2[CGX_MAXTOK]; const int n2 = cgx_lex(g, q, g->no[nx], t2);
                if (n2 < 0) { (*li)++; break; }
                int start = isel ? 1 : 2, endn = n2;
                if (n2 > 1 && cgx_tis(&t2[n2 - 1], ":")) { memset(&t2[n2 - 1], 0, sizeof t2[0]); t2[n2 - 1].t = TK_END; endn = n2 - 1; }
                CgxP p2 = {g, t2, endn, start, g->no[nx], 0};
                CgxNode *e = cgx_node(g, CGN_IF, g->no[nx]); e->flag = 1;
                e->a = cgx_pe(&p2);
                if (!e->a || p2.t[p2.i].t != TK_END) { if (e->a) cgx_perr(&p2, "the end of the condition"); (*li)++; break; }
                (*li)++;
                cgx_body(g, li, ind, &e->b1, 1, g->no[nx], "`elif`");
                cgx_blk_add(g, &cur->b2, e); cur = e;
                continue;
            }
            if (cgx_word_is(q, "else")) {
                *li = nx; (*li)++;
                cgx_body(g, li, ind, &cur->b2, 1, g->no[nx], "`else`");
            }
            break;
        }
        return n;
    }
    if (cgx_tis(f, "elif") || cgx_tis(f, "elsif") || cgx_tis(f, "else") || cgx_tis(f, "catch") || cgx_tis(f, "except") || cgx_tis(f, "finally")) {
        cgx_err(g, line, "`%.*s` without a matching `%s` at the same indentation", f->n, f->p, cgx_tis(f, "elif") || cgx_tis(f, "elsif") || cgx_tis(f, "else") ? "if" : "try");
        (*li)++; { const int nx = cgx_next_real(g, *li); if (nx < g->nl && g->indent[nx] > ind) { CgxBlk tmp = {0}; cgx_block(g, li, g->indent[nx], &tmp, 1); } }
        return NULL;
    }
    if (cgx_tis(f, "while")) {
        p.i = 1; n = cgx_node(g, CGN_WHILE, line);
        n->a = cgx_pe(&p);
        if (!n->a || p.t[p.i].t != TK_END) { if (n->a) cgx_perr(&p, "the end of the condition"); (*li)++; return NULL; }
        (*li)++;
        cgx_body(g, li, ind, &n->b1, 3, line, "`while`");
        return n;
    }
    if (cgx_tis(f, "for") || cgx_tis(f, "foreach")) {
        p.i = 1;
        if (cgx_is(&p, "each")) p.i++;
        if (cgx_acc(&p, "(")) { /* for (k, v) in */ }
        if (!cgx_ident_ok(&p.t[p.i])) { cgx_perr(&p, "a loop variable after `for`"); (*li)++; return NULL; }
        char *v1 = cgx_tokname(&p); p.i++; char *v2 = NULL;
        if (cgx_acc(&p, ",")) { if (!cgx_ident_ok(&p.t[p.i])) { cgx_perr(&p, "a second loop variable"); (*li)++; return NULL; } v2 = cgx_tokname(&p); p.i++; }
        cgx_acc(&p, ")");
        if (!cgx_expect(&p, "in")) { (*li)++; return NULL; }
        CgxNode *e = cgx_pe(&p);
        if (!e) { (*li)++; return NULL; }
        if (cgx_acc(&p, "..")) {
            n = cgx_node(g, CGN_FOR, line); n->s = v1; n->a = e;
            if (v2) { cgx_err(g, line, "a range loop has one variable (`for i in 0..n`)"); (*li)++; return NULL; }
            n->b = cgx_pe(&p); if (!n->b) { (*li)++; return NULL; }
            if (cgx_acc(&p, "step")) { n->c = cgx_pe(&p); if (!n->c) { (*li)++; return NULL; } }
        } else if (e->k == CGN_CALL && !strcmp(e->s, "range") && e->b1.n >= 1 && e->b1.n <= 3) {
            n = cgx_node(g, CGN_FOR, line); n->s = v1;
            if (e->b1.n == 1) { n->a = cgx_node(g, CGN_INT, line); n->a->ty = cgx_t(CGT_INT); n->b = e->b1.p[0]; }
            else { n->a = e->b1.p[0]; n->b = e->b1.p[1]; n->c = e->b1.n == 3 ? e->b1.p[2] : NULL; }
        } else { n = cgx_node(g, CGN_EACH, line); n->s = v1; n->s2 = v2; n->a = e; }
        if (p.t[p.i].t != TK_END) { cgx_perr(&p, "the end of the `for` header"); (*li)++; return NULL; }
        (*li)++;
        cgx_body(g, li, ind, &n->b1, 3, line, "`for`");
        return n;
    }
    if (cgx_tis(f, "try")) {
        if (tk[1].t != TK_END) { p.i = 1; cgx_perr(&p, "the end of the line after `try`"); (*li)++; return NULL; }
        n = cgx_node(g, CGN_TRY, line); (*li)++;
        cgx_body(g, li, ind, &n->b1, ctx, line, "`try`");
        int nx = cgx_next_real(g, *li);
        if (nx < g->nl && g->indent[nx] == ind) {
            const char *q = g->ln[nx]; while (*q == ' ' || *q == '\t') q++;
            if (cgx_word_is(q, "catch") || cgx_word_is(q, "except")) {
                *li = nx;
                CgxTok t2[CGX_MAXTOK]; const int n2 = cgx_lex(g, q, g->no[nx], t2);
                if (n2 > 1 && cgx_tis(&t2[n2 - 1], ":")) t2[n2 - 1].t = TK_END;
                n->flag = 1;
                for (int i = 1; i < n2 && t2[i].t != TK_END; i++) if (t2[i].t == TK_ID && !cgx_tis(&t2[i], "as") && !cgx_tis(&t2[i], "Exception") && !cgx_tis(&t2[i], "Error")) n->s2 = cgx_dupn(g, t2[i].p, (size_t)t2[i].n);      /* o ultimo nome: `catch e`, `except ValueError as e` */
                (*li)++;
                cgx_body(g, li, ind, &n->b2, ctx, g->no[nx], "`catch`");
                nx = cgx_next_real(g, *li);
            }
        }
        if (nx < g->nl && g->indent[nx] == ind) {
            const char *q = g->ln[nx]; while (*q == ' ' || *q == '\t') q++;
            if (cgx_word_is(q, "finally")) { *li = nx; (*li)++; n->iv = 1; cgx_body(g, li, ind, &n->b3, ctx, g->no[nx], "`finally`"); }
        }
        if (!n->flag && !n->iv) cgx_err(g, line, "`try` needs a `catch` or a `finally` block at the same indentation (expected `catch e`)");
        return n;
    }
    /* atribuicao ou expressao (chamada) */
    CgxNode *lhs = cgx_pe(&p);
    if (!lhs || p.bad) { (*li)++; return NULL; }
    static const struct { const char *s; int op; } as[] = {{"=", -1}, {"+=", CGO_ADD}, {"-=", CGO_SUB}, {"*=", CGO_MUL}, {"/=", CGO_DIV}, {"%=", CGO_MOD}, {NULL, 0}};
    for (int i = 0; as[i].s; i++) if (cgx_is(&p, as[i].s)) {
        p.i++;
        if (lhs->k != CGN_VAR && lhs->k != CGN_INDEX && lhs->k != CGN_FIELD) { cgx_err(g, line, "cannot assign to this expression (expected a variable, `xs[i]` or `obj.field` before `%s`)", as[i].s); (*li)++; return NULL; }
        n = cgx_node(g, CGN_ASSIGN, line); n->a = lhs; n->op = as[i].op; n->b = cgx_pe(&p);
        if (!n->b) { (*li)++; return NULL; }
        if (p.t[p.i].t != TK_END) { cgx_perr(&p, "the end of the line (one statement per line)"); (*li)++; return NULL; }
        (*li)++; return n;
    }
    if (p.t[p.i].t != TK_END) { cgx_perr(&p, "`=` or the end of the line"); (*li)++; return NULL; }
    if (lhs->k != CGN_CALL && lhs->k != CGN_MCALL) { cgx_err(g, line, "a line that is only an expression has no effect (expected a call such as `print(x)`, or an assignment `x = ...`)"); (*li)++; return NULL; }
    n = cgx_node(g, CGN_EXPR, line); n->a = lhs;
    (*li)++; return n;
}

static void cgx_block(Cgx *g, int *li, int ind, CgxBlk *out, int ctx) {
    while (*li < g->nl) {
        if (g->indent[*li] < 0) { (*li)++; continue; }
        if (g->cmt[*li]) {
            const int nx = cgx_next_real(g, *li);
            if (ctx != 0 && nx < g->nl && g->indent[nx] < ind && g->indent[*li] < ind) break;      /* comentario do bloco de fora */
            const char *q = g->ln[*li]; while (*q == ' ' || *q == '\t') q++;
            q += (*q == '#') ? 1 : 2;
            while (*q == ' ') q++;
            CgxNode *c = cgx_node(g, CGN_COMMENT, g->no[*li]); c->defs = cgx_dup(g, q); cgx_text_trim(c->defs);
            cgx_blk_add(g, out, c);
            (*li)++; continue;
        }
        if (g->indent[*li] < ind) break;
        if (g->indent[*li] > ind) { cgx_err(g, g->no[*li], "unexpected indentation (expected %d spaces here, found %d)", ind, g->indent[*li]); (*li)++; continue; }
        CgxNode *s = cgx_stmt(g, li, ind, ctx);
        if (s) cgx_blk_add(g, out, s);
    }
}

/* ---- JSON equivalente: vira o mesmo texto CSP e passa pelo mesmo parser ---- */
typedef struct CgxJ CgxJ;
struct CgxJ { int t; char *s; double d; CgxJ **kid; char **key; int n; };      /* t: 0 null, 1 bool, 2 num, 3 str, 4 arr, 5 obj */
typedef struct { const char *p; Cgx *g; int bad; } CgxJP;
static void cgx_jws(CgxJP *j) { while (*j->p == ' ' || *j->p == '\n' || *j->p == '\t' || *j->p == '\r') j->p++; }
static CgxJ *cgx_jval(CgxJP *j, int depth) {
    cgx_jws(j);
    CgxJ *v = (CgxJ *)cgx_alloc(j->g, sizeof *v);
    if (depth > 60) { j->bad = 1; return v; }
    if (*j->p == '"') {
        j->p++; char *o = (char *)cgx_alloc(j->g, strlen(j->p) + 1); size_t w = 0;
        while (*j->p && *j->p != '"') {
            if (*j->p == '\\' && j->p[1]) {
                j->p++;
                switch (*j->p) {
                    case 'n': o[w++] = '\n'; break; case 't': o[w++] = '\t'; break; case 'r': o[w++] = '\r'; break;
                    case 'u': { unsigned cp = 0; if (isxdigit((unsigned char)j->p[1]) && isxdigit((unsigned char)j->p[2]) && isxdigit((unsigned char)j->p[3]) && isxdigit((unsigned char)j->p[4])) { char h[5] = {j->p[1], j->p[2], j->p[3], j->p[4], 0}; cp = (unsigned)strtoul(h, NULL, 16); j->p += 4; }
                        if (cp < 0x80) o[w++] = (char)cp; else if (cp < 0x800) { o[w++] = (char)(0xC0 | (cp >> 6)); o[w++] = (char)(0x80 | (cp & 63)); } else { o[w++] = (char)(0xE0 | (cp >> 12)); o[w++] = (char)(0x80 | ((cp >> 6) & 63)); o[w++] = (char)(0x80 | (cp & 63)); } break; }
                    default: o[w++] = *j->p;
                }
                j->p++;
            } else o[w++] = *j->p++;
        }
        if (*j->p != '"') j->bad = 1; else j->p++;
        v->t = 3; v->s = o; return v;
    }
    if (*j->p == '[' || *j->p == '{') {
        const int obj = *j->p == '{'; j->p++; v->t = obj ? 5 : 4; int cap = 0;
        for (;;) {
            cgx_jws(j);
            if (*j->p == (obj ? '}' : ']')) { j->p++; break; }
            if (!*j->p) { j->bad = 1; break; }
            char *key = NULL;
            if (obj) { CgxJ *k = cgx_jval(j, depth + 1); if (k->t != 3) { j->bad = 1; break; } key = k->s; cgx_jws(j); if (*j->p != ':') { j->bad = 1; break; } j->p++; }
            CgxJ *c = cgx_jval(j, depth + 1);
            if (j->bad) break;
            if (v->n == cap) { const int nc = cap ? cap * 2 : 8; CgxJ **nk = (CgxJ **)cgx_alloc(j->g, sizeof(CgxJ *) * (size_t)nc); char **nks = (char **)cgx_alloc(j->g, sizeof(char *) * (size_t)nc); if (v->n) { memcpy(nk, v->kid, sizeof(CgxJ *) * (size_t)v->n); memcpy(nks, v->key, sizeof(char *) * (size_t)v->n); } v->kid = nk; v->key = nks; cap = nc; }
            v->kid[v->n] = c; v->key[v->n] = key; v->n++;
            cgx_jws(j);
            if (*j->p == ',') j->p++;
        }
        return v;
    }
    if (!strncmp(j->p, "true", 4)) { v->t = 1; v->d = 1; j->p += 4; return v; }
    if (!strncmp(j->p, "false", 5)) { v->t = 1; v->d = 0; j->p += 5; return v; }
    if (!strncmp(j->p, "null", 4)) { v->t = 0; j->p += 4; return v; }
    if (isdigit((unsigned char)*j->p) || *j->p == '-') { char *e; v->t = 2; v->d = strtod(j->p, &e); if (e == j->p) j->bad = 1; j->p = e; return v; }
    j->bad = 1; return v;
}
static CgxJ *cgx_jget(const CgxJ *o, const char *k) { if (o && o->t == 5) for (int i = 0; i < o->n; i++) if (!strcmp(o->key[i], k)) return o->kid[i]; return NULL; }
static const char *cgx_jstr(const CgxJ *o, const char *k) { CgxJ *v = cgx_jget(o, k); return v && v->t == 3 ? v->s : NULL; }

typedef struct { char *p; size_t n, cap; } CgxB;
static void cgx_bn(CgxB *b, const char *s, size_t n) { if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 64; b->p = (char *)realloc(b->p, b->cap); } memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0; }
static void cgx_bs(CgxB *b, const char *s) { cgx_bn(b, s, strlen(s)); }
static void cgx_bf(CgxB *b, const char *fmt, ...) { char t[2048]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap); cgx_bs(b, t); }

static void cgx_jquote(CgxB *b, const char *s) {
    cgx_bs(b, "\"");
    for (; *s; s++) { if (*s == '"' || *s == '\\') { cgx_bn(b, "\\", 1); cgx_bn(b, s, 1); } else if (*s == '\n') cgx_bs(b, "\\n"); else if (*s == '\t') cgx_bs(b, "\\t"); else cgx_bn(b, s, 1); }
    cgx_bs(b, "\"");
}
static const char *cgx_jop(const char *op) {
    static const struct { const char *n, *s; } m[] = {{"ADD", "+"}, {"SUBTRACT", "-"}, {"MULTIPLY", "*"}, {"DIVIDE", "/"}, {"IDIVIDE", "//"}, {"MOD", "%"}, {"AND", "and"}, {"OR", "or"}, {"IN", "in"}, {NULL, NULL}};
    for (int i = 0; m[i].n; i++) if (!strcmp(m[i].n, op)) return m[i].s;
    return NULL;
}
static int cgx_jexpr(Cgx *g, const CgxJ *v, CgxB *b);
static int cgx_jlist(Cgx *g, const CgxJ *arr, CgxB *b, const char *sep) {
    for (int i = 0; arr && i < arr->n; i++) { if (i) cgx_bs(b, sep); if (!cgx_jexpr(g, arr->kid[i], b)) return 0; }
    return 1;
}
/* expressao em JSON -> texto. Uma string e uma expressao escrita no proprio CSP */
static int cgx_jexpr(Cgx *g, const CgxJ *v, CgxB *b) {
    if (!v || v->t == 0) { cgx_bs(b, "null"); return 1; }
    if (v->t == 1) { cgx_bs(b, v->d ? "true" : "false"); return 1; }
    if (v->t == 2) { if (v->d == (long long)v->d) cgx_bf(b, "%lld", (long long)v->d); else cgx_bf(b, "%.17g", v->d); return 1; }
    if (v->t == 3) { cgx_bs(b, v->s); return 1; }
    if (v->t == 4) { cgx_bs(b, "["); if (!cgx_jlist(g, v, b, ", ")) return 0; cgx_bs(b, "]"); return 1; }
    const char *op = cgx_jstr(v, "op");
    if (!op) { cgx_err(g, 0, "JSON expression object without \"op\" (expected one of ADD, SUBTRACT, MULTIPLY, DIVIDE, IDIVIDE, MOD, COMPARE, AND, OR, NOT, CALL, VAR, LIT, LIST, MAP, INDEX, FIELD, METHOD)"); return 0; }
    CgxJ *args = cgx_jget(v, "args");
    if (!args && cgx_jget(v, "left") && cgx_jget(v, "right")) {          /* forma {"op", "left", "right"}: vira args [left, right] */
        args = (CgxJ *)cgx_alloc(g, sizeof *args); args->t = 4; args->n = 2; args->kid = (CgxJ **)cgx_alloc(g, sizeof(CgxJ *) * 2);
        args->kid[0] = cgx_jget(v, "left"); args->kid[1] = cgx_jget(v, "right");
    }
    if (cgx_jop(op)) {
        if (!args || args->t != 4 || args->n < 2) { cgx_err(g, 0, "JSON %s needs \"args\": [a, b]", op); return 0; }
        cgx_bs(b, "(");
        for (int i = 0; i < args->n; i++) { if (i) cgx_bf(b, " %s ", cgx_jop(op)); if (!cgx_jexpr(g, args->kid[i], b)) return 0; }
        cgx_bs(b, ")"); return 1;
    }
    if (!strcmp(op, "COMPARE")) {
        const char *c = cgx_jstr(v, "cmp"); if (!c) c = cgx_jstr(v, "op2");
        if (!c || !args || args->n != 2) { cgx_err(g, 0, "JSON COMPARE needs \"cmp\": one of == != < <= > >= and \"args\": [a, b]"); return 0; }
        cgx_bs(b, "("); if (!cgx_jexpr(g, args->kid[0], b)) return 0; cgx_bf(b, " %s ", c); if (!cgx_jexpr(g, args->kid[1], b)) return 0; cgx_bs(b, ")"); return 1;
    }
    if (!strcmp(op, "NOT")) { cgx_bs(b, "(not "); if (!args || !cgx_jexpr(g, args->kid[0], b)) return 0; cgx_bs(b, ")"); return 1; }
    if (!strcmp(op, "VAR")) { const char *n = cgx_jstr(v, "name"); if (!n) { cgx_err(g, 0, "JSON VAR needs \"name\""); return 0; } cgx_bs(b, n); return 1; }
    if (!strcmp(op, "LIT")) {
        CgxJ *x = cgx_jget(v, "value");
        if (x && x->t == 3) cgx_jquote(b, x->s); else return cgx_jexpr(g, x, b);
        return 1;
    }
    if (!strcmp(op, "LIST")) { cgx_bs(b, "["); if (!cgx_jlist(g, args, b, ", ")) return 0; cgx_bs(b, "]"); return 1; }
    if (!strcmp(op, "MAP")) {
        CgxJ *items = cgx_jget(v, "items"); cgx_bs(b, "{");
        for (int i = 0; items && i < items->n; i++) { if (i) cgx_bs(b, ", "); cgx_jquote(b, items->key[i] ? items->key[i] : ""); cgx_bs(b, ": "); if (!cgx_jexpr(g, items->kid[i], b)) return 0; }
        cgx_bs(b, "}"); return 1;
    }
    if (!strcmp(op, "CALL")) { const char *n = cgx_jstr(v, "name"); if (!n) { cgx_err(g, 0, "JSON CALL needs \"name\""); return 0; } cgx_bf(b, "%s(", n); if (!cgx_jlist(g, args, b, ", ")) return 0; cgx_bs(b, ")"); return 1; }
    if (!strcmp(op, "METHOD")) { const char *n = cgx_jstr(v, "name"); if (!n || !args || !args->n) { cgx_err(g, 0, "JSON METHOD needs \"name\" and \"args\": [object, ...]"); return 0; } if (!cgx_jexpr(g, args->kid[0], b)) return 0; cgx_bf(b, ".%s(", n); for (int i = 1; i < args->n; i++) { if (i > 1) cgx_bs(b, ", "); if (!cgx_jexpr(g, args->kid[i], b)) return 0; } cgx_bs(b, ")"); return 1; }
    if (!strcmp(op, "INDEX")) { if (!args || args->n != 2) { cgx_err(g, 0, "JSON INDEX needs \"args\": [a, i]"); return 0; } if (!cgx_jexpr(g, args->kid[0], b)) return 0; cgx_bs(b, "["); if (!cgx_jexpr(g, args->kid[1], b)) return 0; cgx_bs(b, "]"); return 1; }
    if (!strcmp(op, "FIELD")) { const char *n = cgx_jstr(v, "name"); if (!n || !args || !args->n) { cgx_err(g, 0, "JSON FIELD needs \"name\" and \"args\": [object]"); return 0; } if (!cgx_jexpr(g, args->kid[0], b)) return 0; cgx_bf(b, ".%s", n); return 1; }
    cgx_err(g, 0, "unknown operation \"%s\" in an expression (expected ADD, SUBTRACT, MULTIPLY, DIVIDE, IDIVIDE, MOD, COMPARE, AND, OR, NOT, CALL, METHOD, VAR, LIT, LIST, MAP, INDEX, FIELD)", op);
    return 0;
}
static void cgx_ind(CgxB *b, int n) { for (int i = 0; i < n; i++) cgx_bs(b, "  "); }
static const char *cgx_jtype(const CgxJ *v, const char *k) { const char *s = cgx_jstr(v, k); return s; }
static int cgx_jstmts(Cgx *g, const CgxJ *arr, CgxB *b, int d);
static int cgx_jraw(Cgx *g, const CgxJ *st, CgxB *b, int d) {
    const char *lang = cgx_jstr(st, "lang"), *code = cgx_jstr(st, "code"), *defs = cgx_jstr(st, "defines");
    if (!lang || !code) { cgx_err(g, 0, "JSON RAW needs \"lang\" (python, js, ts, c) and \"code\""); return 0; }
    cgx_ind(b, d); cgx_bf(b, "raw %s%s%s\n", lang, defs ? " defines " : "", defs ? defs : "");
    const char *p = code;
    while (*p) { const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p); cgx_ind(b, d + 1); cgx_bn(b, p, l); cgx_bs(b, "\n"); p += l + (e ? 1 : 0); }
    return 1;
}
static int cgx_jstmt(Cgx *g, const CgxJ *st, CgxB *b, int d) {
    const char *op = cgx_jstr(st, "op");
    if (!op) { cgx_err(g, 0, "JSON statement without \"op\" (expected FUNCTION, DECLARE, ASSIGN, RETURN, IF, LOOP, ITERATE, TRY_CATCH, THROW, CALL, RAW, BREAK, CONTINUE, CLASS, TEST)"); return 0; }
    CgxJ *body = cgx_jget(st, "body");
    if (cgx_jstr(st, "result") && (cgx_jop(op) || !strcmp(op, "COMPARE") || !strcmp(op, "NOT"))) {      /* {"op": "ADD", "left": a, "right": b, "result": c} = let c = a + b */
        cgx_ind(b, d); cgx_bf(b, "let %s = ", cgx_jstr(st, "result"));
        if (!cgx_jexpr(g, st, b)) return 0;
        cgx_bs(b, "\n"); return 1;
    }
    if (!strcmp(op, "FUNCTION")) {
        const char *name = cgx_jstr(st, "name"); CgxJ *pars = cgx_jget(st, "params");
        if (!name) { cgx_err(g, 0, "JSON FUNCTION needs \"name\""); return 0; }
        cgx_ind(b, d); cgx_bf(b, "fn %s(", name);
        for (int i = 0; pars && i < pars->n; i++) {
            CgxJ *pr = pars->kid[i]; if (i) cgx_bs(b, ", ");
            if (pr->t == 3) { cgx_bs(b, pr->s); continue; }
            const char *pn = cgx_jstr(pr, "name"), *pt = cgx_jtype(pr, "type"); CgxJ *df = cgx_jget(pr, "default");
            if (!pn) { cgx_err(g, 0, "JSON parameter without \"name\""); return 0; }
            cgx_bs(b, pn); if (pt) cgx_bf(b, ": %s", pt);
            if (df) { cgx_bs(b, " = "); if (df->t == 3) cgx_jquote(b, df->s); else if (!cgx_jexpr(g, df, b)) return 0; }
        }
        cgx_bs(b, ")"); const char *rt = cgx_jtype(st, "returns"); if (!rt) rt = cgx_jtype(st, "returnType"); if (rt) cgx_bf(b, " -> %s", rt);
        cgx_bs(b, "\n"); return cgx_jstmts(g, body, b, d + 1);
    }
    if (!strcmp(op, "CLASS")) {
        const char *name = cgx_jstr(st, "name"); CgxJ *fields = cgx_jget(st, "fields"), *meths = cgx_jget(st, "methods");
        if (!name) { cgx_err(g, 0, "JSON CLASS needs \"name\""); return 0; }
        cgx_ind(b, d); cgx_bf(b, "class %s\n", name);
        for (int i = 0; fields && i < fields->n; i++) {
            CgxJ *f = fields->kid[i]; const char *fn = cgx_jstr(f, "name"), *ft = cgx_jstr(f, "type"); CgxJ *df = cgx_jget(f, "default");
            if (!fn) { cgx_err(g, 0, "JSON field without \"name\""); return 0; }
            cgx_ind(b, d + 1); cgx_bf(b, "field %s", fn); if (ft) cgx_bf(b, ": %s", ft);
            if (df) { cgx_bs(b, " = "); if (df->t == 3) cgx_jquote(b, df->s); else if (!cgx_jexpr(g, df, b)) return 0; }
            cgx_bs(b, "\n");
        }
        for (int i = 0; meths && i < meths->n; i++) if (!cgx_jstmt(g, meths->kid[i], b, d + 1)) return 0;
        return 1;
    }
    if (!strcmp(op, "DECLARE")) { const char *n = cgx_jstr(st, "name"), *t = cgx_jtype(st, "type"); if (!n) { cgx_err(g, 0, "JSON DECLARE needs \"name\" and \"value\""); return 0; } cgx_ind(b, d); cgx_bf(b, "let %s", n); if (t) cgx_bf(b, ": %s", t); cgx_bs(b, " = "); if (!cgx_jexpr(g, cgx_jget(st, "value"), b)) return 0; cgx_bs(b, "\n"); return 1; }
    if (!strcmp(op, "ASSIGN")) { cgx_ind(b, d); if (!cgx_jexpr(g, cgx_jget(st, "target"), b)) return 0; const char *o = cgx_jstr(st, "assign"); cgx_bf(b, " %s ", o ? o : "="); if (!cgx_jexpr(g, cgx_jget(st, "value"), b)) return 0; cgx_bs(b, "\n"); return 1; }
    if (!strcmp(op, "RETURN")) { cgx_ind(b, d); cgx_bs(b, "return"); CgxJ *v = cgx_jget(st, "value"); if (v) { cgx_bs(b, " "); if (!cgx_jexpr(g, v, b)) return 0; } cgx_bs(b, "\n"); return 1; }
    if (!strcmp(op, "THROW")) { cgx_ind(b, d); cgx_bs(b, "throw "); if (!cgx_jexpr(g, cgx_jget(st, "value"), b)) return 0; cgx_bs(b, "\n"); return 1; }
    if (!strcmp(op, "BREAK")) { cgx_ind(b, d); cgx_bs(b, "break\n"); return 1; }
    if (!strcmp(op, "CONTINUE")) { cgx_ind(b, d); cgx_bs(b, "continue\n"); return 1; }
    if (!strcmp(op, "CALL") || !strcmp(op, "METHOD")) { cgx_ind(b, d); if (!cgx_jexpr(g, st, b)) return 0; cgx_bs(b, "\n"); return 1; }
    if (!strcmp(op, "RAW")) return cgx_jraw(g, st, b, d);
    if (!strcmp(op, "IF")) {
        CgxJ *th = cgx_jget(st, "then"), *el = cgx_jget(st, "else");
        cgx_ind(b, d); cgx_bs(b, "if "); if (!cgx_jexpr(g, cgx_jget(st, "cond"), b)) return 0; cgx_bs(b, "\n");
        if (!cgx_jstmts(g, th, b, d + 1)) return 0;
        if (el && el->n) { cgx_ind(b, d); cgx_bs(b, "else\n"); if (!cgx_jstmts(g, el, b, d + 1)) return 0; }
        return 1;
    }
    if (!strcmp(op, "LOOP")) {
        const char *var = cgx_jstr(st, "var"); CgxJ *from = cgx_jget(st, "from"), *to = cgx_jget(st, "to"), *step = cgx_jget(st, "step");
        cgx_ind(b, d);
        if (var && to) { cgx_bf(b, "for %s in ", var); if (!cgx_jexpr(g, from ? from : NULL, b)) return 0; cgx_bs(b, ".."); if (!cgx_jexpr(g, to, b)) return 0; if (step) { cgx_bs(b, " step "); if (!cgx_jexpr(g, step, b)) return 0; } }
        else { cgx_bs(b, "while "); if (!cgx_jexpr(g, cgx_jget(st, "cond"), b)) return 0; }
        cgx_bs(b, "\n"); return cgx_jstmts(g, body, b, d + 1);
    }
    if (!strcmp(op, "ITERATE")) {
        const char *var = cgx_jstr(st, "var"), *key = cgx_jstr(st, "key");
        if (!var) { cgx_err(g, 0, "JSON ITERATE needs \"var\" and \"over\""); return 0; }
        cgx_ind(b, d); cgx_bf(b, "for %s%s%s in ", key ? key : "", key ? ", " : "", var); if (!cgx_jexpr(g, cgx_jget(st, "over"), b)) return 0; cgx_bs(b, "\n");
        return cgx_jstmts(g, body, b, d + 1);
    }
    if (!strcmp(op, "TRY_CATCH")) {
        const char *cv = cgx_jstr(st, "catch_var"); CgxJ *ct = cgx_jget(st, "catch"), *fi = cgx_jget(st, "finally");
        cgx_ind(b, d); cgx_bs(b, "try\n"); if (!cgx_jstmts(g, body, b, d + 1)) return 0;
        if (ct) { cgx_ind(b, d); cgx_bf(b, "catch %s\n", cv ? cv : "e"); if (!cgx_jstmts(g, ct, b, d + 1)) return 0; }
        if (fi) { cgx_ind(b, d); cgx_bs(b, "finally\n"); if (!cgx_jstmts(g, fi, b, d + 1)) return 0; }
        return 1;
    }
    if (!strcmp(op, "TEST")) { cgx_ind(b, d); cgx_bs(b, "test "); if (!cgx_jexpr(g, cgx_jget(st, "expr"), b)) return 0; const char *c = cgx_jstr(st, "cmp"); CgxJ *ex = cgx_jget(st, "expect"); if (ex) { cgx_bf(b, " %s ", c ? c : "=="); if (!cgx_jexpr(g, ex, b)) return 0; } cgx_bs(b, "\n"); return 1; }
    cgx_err(g, 0, "unknown operation \"%s\" (expected FUNCTION, CLASS, DECLARE, ASSIGN, RETURN, IF, LOOP, ITERATE, TRY_CATCH, THROW, CALL, METHOD, RAW, BREAK, CONTINUE, TEST)", op);
    return 0;
}
static int cgx_jstmts(Cgx *g, const CgxJ *arr, CgxB *b, int d) {
    if (!arr || arr->t != 4) { cgx_err(g, 0, "JSON: expected a list of statements (\"body\": [ ... ])"); return 0; }
    for (int i = 0; i < arr->n; i++) if (!cgx_jstmt(g, arr->kid[i], b, d)) return 0;
    return 1;
}
/* texto JSON da especificacao -> texto CSP (memoria nova em *out); 0 se nao e JSON valido da UIR */
static int cgx_json_to_text(Cgx *g, const char *src, char **out) {
    CgxJP j = {src, g, 0};
    CgxJ *root = cgx_jval(&j, 0);
    cgx_jws(&j);
    if (j.bad) { cgx_err(g, 0, "invalid JSON (expected a list of declarations such as {\"op\": \"FUNCTION\", ...})"); return 0; }
    CgxB b = {0};
    CgxJ *items = root->t == 4 ? root : cgx_jget(root, "items");
    if (root->t == 5 && cgx_jstr(root, "op")) { CgxJ *one = root; items = (CgxJ *)cgx_alloc(g, sizeof *items); items->t = 4; items->n = 1; items->kid = (CgxJ **)cgx_alloc(g, sizeof(CgxJ *)); items->kid[0] = one; }
    if (!items) { cgx_err(g, 0, "JSON: expected a list of declarations, or {\"items\": [...]}"); free(b.p); return 0; }
    for (int i = 0; i < items->n; i++) { if (!cgx_jstmt(g, items->kid[i], &b, 0)) { free(b.p); return 0; } }
    *out = b.p ? b.p : strdup("");
    return 1;
}

/* numera os nos em pre-ordem (id estavel para a mesma especificacao) e interna os nomes */
static void cgx_number_node(Cgx *g, CgxNode *n) {
    if (!n) return;
    n->id = ++g->nnodes; n->sid = cgx_intern(g, n->s);
    cgx_number_node(g, n->a); cgx_number_node(g, n->b); cgx_number_node(g, n->c);
    for (int i = 0; i < n->npar; i++) { cgx_intern(g, n->par[i].name); cgx_number_node(g, n->par[i].def); }
    CgxBlk *bl[3] = {&n->b1, &n->b2, &n->b3};
    for (int k = 0; k < 3; k++) for (int i = 0; i < bl[k]->n; i++) cgx_number_node(g, bl[k]->p[i]);
}
static void cgx_number(Cgx *g) { g->nnodes = 0; cgx_number_node(g, g->prog); }

/* ---- entrada: texto CSP ou JSON ---- */
static int cgx_parse(Cgx *g, const char *src) {
    memset(g, 0, sizeof *g);
    const char *q = src; while (isspace((unsigned char)*q)) q++;
    char *norm = NULL;
    if (*q == '{' || *q == '[') {
        g->from_json = 1;
        if (!cgx_json_to_text(g, q, &norm)) return 0;
        g->norm = norm; src = norm;
    }
    if (!cgx_load(g, src)) return 0;
    g->prog = cgx_node(g, CGN_PROG, 1);
    int li = cgx_next_real(g, 0);
    cgx_block(g, &li, li < g->nl ? g->indent[li] : 0, &g->prog->b1, 0);      /* a primeira linha define a indentacao do nivel de cima */
    cgx_number(g);
    return g->nerr == 0;
}
