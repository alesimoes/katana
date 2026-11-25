/* ---------- coder/codegengo.c: Codegen, backend Go (docs/codegen.md, "Backend Go") ----------
 * Mesma estrutura do backend C (codegenc.c), mas Go ja tem coleta de lixo, mapas, strings e panic/recover; o que o gerador acrescenta e
 * um runtime (tools/codegen/rt/cgrt.go, embutido em codegengo_rt.inc): inteiros de 64 bits com estouro verificado, // e % por piso, listas
 * e mapas genericos (List[T], Map[T], na ordem de insercao), a vista dinamica Lister/Mapper para list[any] e map[str,any], e o valor
 * etiquetado (any do Go). Decisoes:
 *   int = int64; float = float64; bool = bool; str = string (bytes; nulo e ""); list[int] = *List[int64]; list[any] = Lister; classe = *struct;
 *   throw = panic(CgErr); try/catch/finally = cgTry(corpo, catch, finally) com closures; return, break e continue dentro de um try viram
 *   codigos de controle (1 break, 2 continue, 3 return) que o codigo depois do cgTry trata; o resultado vai em cgrv;
 *   variavel nunca lida ganha `_ = x` (Go recusa); ternario vira closure (so um ramo roda). A saida passa por gofmt sem mudar. */
#include "codegengo_rt.inc"

typedef struct { const CgxBlk *fin; } CgxGReg;
typedef struct {
    Cgx *g; const CgxGenOpts *o; CgxDocs D;
    const CgxNode *fn; const CgxNode *cls_list[40]; int ncls;
    int uniq, tdepth, nloop, loop_td[24], line, need_rv, top, curtop, infin;     /* top: a expressao e a instrucao inteira (profundidade 1 do gofmt) */
    struct { const void *ref; const char *name; } *used; int nused, capused;
    struct { char name[48]; int kind, n; } dyn[96]; int ndyn;
    CgxTy rett;
} CgxG;
#define GD (&c->D)
static CgxDoc *cgxg_t(CgxG *c, const char *s) { return cgx_dtext(GD, s); }
static CgxDoc *cgxg_pre(CgxG *c, const char *pre, CgxDoc *d, const char *post) { return cgx_dcatn(GD, cgx_dtext(GD, pre), d, cgx_dtext(GD, post), NULL); }

/* ---- nomes ---- */
static const char *const CGXG_RESERVED[] = {"break", "default", "func", "interface", "select", "case", "defer", "go", "map", "struct", "chan", "else", "goto", "package", "switch", "const",
    "fallthrough", "if", "range", "type", "continue", "for", "import", "return", "var", "any", "bool", "byte", "comparable", "complex64", "complex128", "error", "float32", "float64", "int", "int8",
    "int16", "int32", "int64", "rune", "string", "uint", "uint8", "uint16", "uint32", "uint64", "uintptr", "true", "false", "iota", "nil", "append", "cap", "clear", "close", "complex", "copy",
    "delete", "imag", "len", "make", "max", "min", "new", "panic", "print", "println", "real", "recover", "fmt", "math", "os", "regexp", "sort", "strconv", "strings", "main", "init",
    "List", "Map", "Lister", "Mapper", "CgErr", NULL};
static void cgxg_name(const char *n, CgxB *o) {
    int bad = !strncmp(n, "cg", 2) || !strncmp(n, "Cg", 2) || !strncmp(n, "CG", 2) || n[0] == '_' || (!strncmp(n, "new", 3) && n[3] >= 'A' && n[3] <= 'Z');
    for (int i = 0; !bad && CGXG_RESERVED[i]; i++) if (!strcmp(n, CGXG_RESERVED[i])) bad = 1;
    cgx_bs(o, n); if (bad) cgx_bs(o, "_");
}
static void cgxg_cstr(const char *s, CgxB *o) {
    cgx_bs(o, "\"");
    for (; *s; s++) {
        const unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { cgx_bn(o, "\\", 1); cgx_bn(o, s, 1); }
        else if (ch == '\n') cgx_bs(o, "\\n"); else if (ch == '\t') cgx_bs(o, "\\t"); else if (ch == '\r') cgx_bs(o, "\\r");
        else if (ch < 32 || ch == 127) cgx_bf(o, "\\x%02x", ch);
        else cgx_bn(o, s, 1);                       /* bytes UTF-8 passam como estao: Go le a fonte como UTF-8 */
    }
    cgx_bs(o, "\"");
}

/* ---- tipos ---- */
static int cgxg_clsid(const CgxG *c, const char *name) { for (int i = 0; i < c->ncls; i++) if (!strcmp(c->cls_list[i]->s, name)) return i + 1; return 0; }
static const char *cgxg_ety(int e) { return e == CGT_INT ? "int64" : e == CGT_FLOAT ? "float64" : e == CGT_STR ? "string" : e == CGT_BOOL ? "bool" : "any"; }
static void cgxg_type(const CgxTy *t, CgxB *o) {
    switch (cgxc_kind(t)) {
        case CGXC_K_INT: cgx_bs(o, "int64"); break; case CGXC_K_FLT: cgx_bs(o, "float64"); break; case CGXC_K_BOOL: cgx_bs(o, "bool"); break; case CGXC_K_STR: cgx_bs(o, "string"); break;
        case CGXC_K_VOID: break;
        case CGXC_K_LIST: if (cgxc_ek(t->e) == 4) cgx_bs(o, "Lister"); else cgx_bf(o, "*List[%s]", cgxg_ety(t->e)); break;
        case CGXC_K_MAP: if (cgxc_ek(t->e) == 4) cgx_bs(o, "Mapper"); else cgx_bf(o, "*Map[%s]", cgxg_ety(t->e)); break;
        case CGXC_K_OBJ: { CgxB n = {0}; cgxg_name(t->cls, &n); cgx_bf(o, "*%s", n.p); free(n.p); break; }
        default: cgx_bs(o, "any");
    }
}
static char *cgxg_ts(const CgxTy *t) { CgxB b = {0}; cgxg_type(t, &b); return b.p ? b.p : strdup(""); }
static CgxTy cgxg_vty(const CgxNode *n) { return cgxc_vty(n); }
static CgxTy cgxg_ty(int k) { return cgx_t(k); }
static CgxTy cgxg_listty(int e) { CgxTy t = cgx_t(CGT_LIST); t.e = e; return t; }

/* ---- conversoes ---- */
static CgxDoc *cgxg_box(CgxG *c, CgxDoc *d, const CgxTy *t) {
    switch (cgxc_kind(t)) {
        case CGXC_K_INT: return cgxg_pre(c, "any(int64(", d, "))"); case CGXC_K_FLT: return cgxg_pre(c, "any(float64(", d, "))");
        case CGXC_K_BOOL: case CGXC_K_STR: return cgxg_pre(c, "any(", d, ")");
        case CGXC_K_LIST: return cgxc_ek(t->e) == 4 ? cgxg_pre(c, "any(", d, ")") : cgxg_pre(c, "cgBox(", d, ")");
        case CGXC_K_MAP: return cgxc_ek(t->e) == 4 ? cgxg_pre(c, "any(", d, ")") : cgxg_pre(c, "cgBox(", d, ")");
        case CGXC_K_OBJ: return cgxg_pre(c, "cgBox(", d, ")");
        case CGXC_K_NULL: return cgxg_t(c, "nil");
    }
    return d;
}
static CgxDoc *cgxg_unbox(CgxG *c, CgxDoc *d, const CgxTy *t) {
    CgxB b = {0}; CgxDoc *r;
    switch (cgxc_kind(t)) {
        case CGXC_K_INT: case CGXC_K_FLT: case CGXC_K_BOOL: case CGXC_K_STR: case CGXC_K_OBJ: { char *ts = cgxg_ts(t); cgx_bf(&b, "cgAs[%s](", ts); free(ts); r = cgxg_pre(c, b.p, d, ")"); free(b.p); return r; }
        case CGXC_K_LIST: if (cgxc_ek(t->e) == 4) return cgxg_pre(c, "cgUL(", d, ")"); cgx_bf(&b, "cgList[%s](", cgxg_ety(t->e)); r = cgxg_pre(c, b.p, d, ")"); free(b.p); return r;
        case CGXC_K_MAP: if (cgxc_ek(t->e) == 4) return cgxg_pre(c, "cgUM(", d, ")"); cgx_bf(&b, "cgMap[%s](", cgxg_ety(t->e)); r = cgxg_pre(c, b.p, d, ")"); free(b.p); return r;
    }
    return d;
}
static CgxDoc *cgxg_conv(CgxG *c, CgxDoc *d, const CgxTy *from, const CgxTy *to) {
    const int kf = cgxc_kind(from), kt = cgxc_kind(to);
    if (kt == CGXC_K_VOID || kf == CGXC_K_VOID) return d;
    if (kf == CGXC_K_NULL) {
        if (kt == CGXC_K_INT || kt == CGXC_K_FLT || kt == CGXC_K_BOOL) { cgx_err(c->g, c->line, "go: null where a number or bool is expected (declare the type as `any`, or annotate `-> any`)"); return d; }
        return kt == CGXC_K_STR ? cgxg_t(c, "\"\"") : cgxg_t(c, "nil");
    }
    if (kt == CGXC_K_ANY) return kf == CGXC_K_ANY ? d : cgxg_box(c, d, from);
    if (kf == CGXC_K_ANY) return cgxg_unbox(c, d, to);
    if (kf == CGXC_K_INT && kt == CGXC_K_FLT) return cgxg_pre(c, "float64(", d, ")");
    if (kf == CGXC_K_LIST && kt == CGXC_K_LIST && cgxc_ek(to->e) != 4 && (from->e != to->e)) {
        CgxB b = {0}; cgx_bf(&b, "cgList[%s](", cgxg_ety(to->e)); CgxDoc *r = cgxg_pre(c, b.p, d, ")"); free(b.p); return r;
    }
    if (kf == CGXC_K_MAP && kt == CGXC_K_MAP && cgxc_ek(to->e) != 4 && (from->e != to->e)) {
        CgxB b = {0}; cgx_bf(&b, "cgMap[%s](", cgxg_ety(to->e)); CgxDoc *r = cgxg_pre(c, b.p, d, ")"); free(b.p); return r;
    }
    return d;
}

/* ---- expressoes ---- */
static CgxDoc *cgxg_ex0(CgxG *c, const CgxNode *n, int *prec, CgxTy *nat);
static CgxDoc *cgxg_exw(CgxG *c, const CgxNode *n, const CgxTy *want, int minprec);
static CgxDoc *cgxg_wrap(CgxG *c, CgxDoc *d, int prec, int minprec) { return prec < minprec ? cgx_dcatn(GD, cgxg_t(c, "("), d, cgxg_t(c, ")"), NULL) : d; }
static CgxDoc *cgxg_seq(CgxG *c, const char *open, CgxDoc *const *items, int n, const char *close) {
    if (!n) return cgx_dtextf(GD, "%s%s", open, close);
    CgxDoc *in = NULL;
    for (int i = 0; i < n; i++) { CgxDoc *x = i ? cgx_dcatn(GD, cgxg_t(c, ","), cgx_dline(GD), items[i], NULL) : items[i]; in = in ? cgx_dcat(GD, in, x) : x; }
    return cgx_dgroup(GD, cgx_dcatn(GD, cgxg_t(c, open), cgx_dnest(GD, 1, cgx_dcatn(GD, cgx_dsoft(GD), in, cgx_dbrk(GD, ","), NULL)), cgx_dsoft(GD), cgxg_t(c, close), NULL));
}
/* f(a, b, ...): cada operando ja convertido; Go avalia as chamadas da esquerda para a direita, entao nao ha temporarios */
static CgxDoc *cgxg_callN(CgxG *c, const char *fname, const CgxNode *const *ns, const CgxTy *want, int n, int *prec) {
    CgxDoc *o[64]; if (n > 64) n = 64;
    for (int i = 0; i < n; i++) o[i] = cgxg_exw(c, ns[i], &want[i], 0);
    *prec = 8; return cgx_dcat(GD, cgxg_t(c, fname), cgxg_seq(c, "(", o, n, ")"));
}
static CgxDoc *cgxg_infix(CgxG *c, const char *sym, const CgxNode *a, const CgxNode *b, const CgxTy *wa, const CgxTy *wb, int pr, int *prec) {
    *prec = pr; return cgx_dcatn(GD, cgxg_exw(c, a, wa, pr + 1), cgx_dtextf(GD, " %s ", sym), cgxg_exw(c, b, wb, pr + 1), NULL);
}
static CgxDoc *cgxg_list(CgxG *c, const CgxNode *n, const CgxTy *want) {
    CgxTy lt = cgxc_kind(want) == CGXC_K_LIST ? *want : n->ty; CgxDoc *r;
    if (lt.k != CGT_LIST) lt = cgxg_listty(CGT_ANY);
    const CgxTy el = cgx_t(lt.e); const int k = n->b1.n; CgxDoc *o[128]; int m = k > 128 ? 128 : k;
    for (int i = 0; i < m; i++) o[i] = cgxg_exw(c, n->b1.p[i], &el, 0);
    CgxB h = {0}; cgx_bf(&h, "cgNewList[%s]", cgxg_ety(lt.e));
    r = cgx_dcat(GD, cgxg_t(c, h.p), cgxg_seq(c, "(", o, m, ")")); free(h.p);
    if (cgxc_ek(lt.e) == 4 && cgxc_kind(want) == CGXC_K_LIST) r = cgxg_pre(c, "Lister(", r, ")");
    if (cgxc_kind(want) == CGXC_K_ANY) r = cgxg_box(c, r, &lt);
    return r;
}
static CgxDoc *cgxg_map(CgxG *c, const CgxNode *n, const CgxTy *want) {
    CgxTy mt = cgxc_kind(want) == CGXC_K_MAP ? *want : n->ty; CgxDoc *r;
    if (mt.k != CGT_MAP) mt = cgxg_ty(CGT_MAP);
    const int k = n->b1.n / 2; CgxDoc *o[128]; int m = k > 60 ? 60 : k; CgxTy S = cgxg_ty(CGT_STR), A = cgxg_ty(CGT_ANY);
    for (int i = 0; i < m; i++) { o[2 * i] = cgxg_box(c, cgxg_exw(c, n->b1.p[2 * i], &S, 0), &S); o[2 * i + 1] = cgxg_exw(c, n->b1.p[2 * i + 1], &A, 0); }
    CgxB h = {0}; cgx_bf(&h, "cgMapLit[%s]", cgxg_ety(mt.e));
    r = cgx_dcat(GD, cgxg_t(c, h.p), cgxg_seq(c, "(", o, 2 * m, ")")); free(h.p);
    if (cgxc_ek(mt.e) == 4 && cgxc_kind(want) == CGXC_K_MAP) r = cgxg_pre(c, "Mapper(", r, ")");
    if (cgxc_kind(want) == CGXC_K_ANY) r = cgxg_box(c, r, &mt);
    return r;
}

/* ---- funcoes embutidas ---- */
static CgxDoc *cgxg_cmp0(CgxG *c, CgxDoc *call, int *prec, const char *tail) { CgxDoc *d = cgxg_wrap(c, call, *prec, 8); *prec = 3; return cgx_dcatn(GD, d, cgxg_t(c, tail), NULL); }
static const char *cgxg_lname(const CgxTy *t) { return cgxg_ety(t->e); }
static CgxDoc *cgxg_builtin(CgxG *c, const CgxNode *n, int *prec, CgxTy *nat) {
    const CgxBlk *a = &n->b1; const int na = a->n; CgxTy t0 = na ? cgxg_vty(a->p[0]) : cgxg_ty(CGT_ANY), t1 = na > 1 ? cgxg_vty(a->p[1]) : cgxg_ty(CGT_ANY);
    const int k0 = cgxc_kind(&t0), k1 = cgxc_kind(&t1); const CgxNode *const *ns = (const CgxNode *const *)a->p;
    CgxTy S = cgxg_ty(CGT_STR), I = cgxg_ty(CGT_INT), F = cgxg_ty(CGT_FLOAT), B = cgxg_ty(CGT_BOOL), A = cgxg_ty(CGT_ANY), LA = cgxg_listty(CGT_ANY), MA = cgxg_ty(CGT_MAP), V = cgxg_ty(CGT_VOID);
    CgxB b = {0}; CgxDoc *r;
    *prec = 8; *nat = n->ty;
    switch (n->bi) {
        case CGB_LEN: *nat = I;
            if (k0 == CGXC_K_STR) return cgxg_pre(c, "int64(len(", cgxg_exw(c, ns[0], &S, 0), "))");
            if (k0 == CGXC_K_LIST) return cgxg_pre(c, "int64(", cgx_dcat(GD, cgxg_exw(c, ns[0], &t0, 8), cgxg_t(c, ".Len()")), ")");
            if (k0 == CGXC_K_MAP) return cgxg_pre(c, "int64(", cgx_dcat(GD, cgxg_exw(c, ns[0], &t0, 8), cgxg_t(c, ".Len()")), ")");
            return cgxg_callN(c, "cgVLen", ns, &A, 1, prec);
        case CGB_PUSH: {
            CgxTy lt = k0 == CGXC_K_LIST ? t0 : LA; CgxTy el = cgx_t(lt.e); *nat = V;
            CgxDoc *l = k0 == CGXC_K_LIST ? cgxg_exw(c, ns[0], &t0, 8) : cgxg_pre(c, "cgLister(", cgxg_exw(c, ns[0], &A, 0), ")");
            const char *m = cgxc_ek(lt.e) == 4 ? "PushAny" : "Push";
            return cgx_dcatn(GD, l, cgx_dtextf(GD, ".%s(", m), cgxg_exw(c, ns[1], &el, 0), cgxg_t(c, ")"), NULL);
        }
        case CGB_POP: {
            CgxTy lt = k0 == CGXC_K_LIST ? t0 : LA; *nat = cgx_t(lt.e);
            CgxDoc *l = k0 == CGXC_K_LIST ? cgxg_exw(c, ns[0], &t0, 8) : cgxg_pre(c, "cgLister(", cgxg_exw(c, ns[0], &A, 0), ")");
            return cgx_dcat(GD, l, cgxg_t(c, cgxc_ek(lt.e) == 4 ? ".PopAny()" : ".Pop()"));
        }
        case CGB_STR: *nat = S;
            if (k0 == CGXC_K_STR) return cgxg_exw(c, ns[0], &S, 0);
            if (k0 == CGXC_K_INT) return cgxg_pre(c, "strconv.FormatInt(", cgxg_exw(c, ns[0], &I, 0), ", 10)");
            if (k0 == CGXC_K_FLT) return cgxg_callN(c, "cgFStr", ns, &F, 1, prec);
            if (k0 == CGXC_K_BOOL) return cgxg_callN(c, "cgBStr", ns, &B, 1, prec);
            return cgxg_callN(c, "cgStr", ns, &A, 1, prec);
        case CGB_INT: *nat = I;
            if (k0 == CGXC_K_INT) return cgxg_exw(c, ns[0], &I, 0);
            if (k0 == CGXC_K_FLT) return cgxg_callN(c, "cgF2I", ns, &F, 1, prec);
            if (k0 == CGXC_K_STR) return cgxg_callN(c, "cgS2I", ns, &S, 1, prec);
            return cgxg_callN(c, "cgV2I", ns, &A, 1, prec);
        case CGB_FLOAT: *nat = F;
            if (k0 == CGXC_K_INT || k0 == CGXC_K_FLT) return cgxg_exw(c, ns[0], &F, 0);
            if (k0 == CGXC_K_STR) return cgxg_callN(c, "cgS2F", ns, &S, 1, prec);
            return cgxg_callN(c, "cgV2F", ns, &A, 1, prec);
        case CGB_UPPER: *nat = S; return cgxg_callN(c, "cgSUpper", ns, &S, 1, prec);
        case CGB_LOWER: *nat = S; return cgxg_callN(c, "cgSLower", ns, &S, 1, prec);
        case CGB_TRIM: *nat = S; return cgxg_callN(c, "cgSTrim", ns, &S, 1, prec);
        case CGB_SPLIT: { CgxTy w[2] = {S, S}; *nat = cgxg_listty(CGT_STR); return cgxg_callN(c, "cgSSplit", ns, w, 2, prec); }
        case CGB_WORDS: *nat = cgxg_listty(CGT_STR); return cgxg_callN(c, "cgSWords", ns, &S, 1, prec);
        case CGB_JOIN: { CgxTy w[2] = {k0 == CGXC_K_LIST ? t0 : LA, S}; *nat = S; if (k0 != CGXC_K_LIST) { CgxDoc *l = cgxg_pre(c, "cgLister(", cgxg_exw(c, ns[0], &A, 0), ")"); return cgx_dcatn(GD, cgxg_t(c, "cgJoin("), l, cgxg_t(c, ", "), cgxg_exw(c, ns[1], &S, 0), cgxg_t(c, ")"), NULL); } return cgxg_callN(c, "cgJoin", ns, w, 2, prec); }
        case CGB_CONTAINS: *nat = B;
            if (k0 == CGXC_K_STR) { CgxTy w[2] = {S, S}; return cgxg_callN(c, "strings.Contains", ns, w, 2, prec); }
            if (k0 == CGXC_K_MAP) { CgxTy w[2] = {t0, S}; if (cgxc_ek(t0.e) == 4 || 1) return cgx_dcatn(GD, cgxg_exw(c, ns[0], &t0, 8), cgxg_t(c, ".HasKey("), cgxg_exw(c, ns[1], &S, 0), cgxg_t(c, ")"), NULL); (void)w; }
            { CgxTy w[2] = {A, A}; return cgxg_callN(c, "cgVHas", ns, w, 2, prec); }
        case CGB_INDEXOF: { CgxTy w[2] = {S, S}; *nat = I; return cgxg_callN(c, "cgSFind", ns, w, 2, prec); }
        case CGB_REPLACE: { CgxTy w[3] = {S, S, S}; *nat = S; return cgxg_callN(c, "cgSReplace", ns, w, 3, prec); }
        case CGB_STARTS: { CgxTy w[2] = {S, S}; *nat = B; return cgxg_callN(c, "strings.HasPrefix", ns, w, 2, prec); }
        case CGB_ENDS: { CgxTy w[2] = {S, S}; *nat = B; return cgxg_callN(c, "strings.HasSuffix", ns, w, 2, prec); }
        case CGB_SLICE: {
            CgxDoc *o[3]; const char *fn; CgxTy w[3]; w[1] = I; w[2] = I;
            if (k0 == CGXC_K_STR) { fn = "cgSSlice"; w[0] = S; *nat = S; }
            else if (k0 == CGXC_K_LIST) { fn = "cgVSlice"; w[0] = t0; *nat = t0; }
            else { fn = "cgVSlice"; w[0] = A; *nat = A; }
            o[0] = cgxg_exw(c, ns[0], &w[0], 0); o[1] = cgxg_exw(c, ns[1], &I, 0); o[2] = na > 2 ? cgxg_exw(c, ns[2], &I, 0) : cgxg_t(c, "math.MaxInt64");
            r = cgx_dcat(GD, cgxg_t(c, fn), cgxg_seq(c, "(", o, 3, ")"));
            if (k0 == CGXC_K_LIST) { char *ts = cgxg_ts(&t0); r = cgxc_ek(t0.e) == 4 ? cgxg_pre(c, "cgLister(", r, ")") : cgxg_conv(c, r, &A, &t0); free(ts); }
            return r;
        }
        case CGB_KEYS: *nat = cgxg_listty(CGT_STR); return cgxg_callN(c, "cgKeys", ns, &MA, 1, prec);
        case CGB_PRINT: *nat = V; return cgxg_callN(c, "cgPrint", ns, &A, 1, prec);
        case CGB_ABS: *nat = t0;
            if (k0 == CGXC_K_INT) return cgxg_callN(c, "cgAbs", ns, &I, 1, prec);
            if (k0 == CGXC_K_FLT) return cgxg_callN(c, "math.Abs", ns, &F, 1, prec);
            *nat = A; return cgxg_callN(c, "cgVAbs", ns, &A, 1, prec);
        case CGB_MIN: case CGB_MAX: {
            const int mn = n->bi == CGB_MIN; CgxTy w[2];
            if (k0 == CGXC_K_ANY || k1 == CGXC_K_ANY) { w[0] = w[1] = A; *nat = A; return cgxg_callN(c, mn ? "cgVMin" : "cgVMax", ns, w, 2, prec); }
            if (k0 == CGXC_K_FLT || k1 == CGXC_K_FLT) { w[0] = w[1] = F; *nat = F; return cgxg_callN(c, mn ? "cgFMin" : "cgFMax", ns, w, 2, prec); }
            w[0] = w[1] = I; *nat = I; return cgxg_callN(c, mn ? "cgIMin" : "cgIMax", ns, w, 2, prec);
        }
        case CGB_SQRT: *nat = F; return cgxg_callN(c, "cgSqrt", ns, &F, 1, prec);
        case CGB_FLOOR: *nat = I; if (k0 == CGXC_K_INT) return cgxg_exw(c, ns[0], &I, 0); return cgxg_callN(c, "cgFloor", ns, &F, 1, prec);
        case CGB_POW: { CgxTy w[2] = {F, F}; *nat = F; return cgxg_callN(c, "cgPow", ns, w, 2, prec); }
        case CGB_SUM: { *nat = A; if (k0 != CGXC_K_LIST) { return cgxg_pre(c, "cgSum(cgLister(", cgxg_exw(c, ns[0], &A, 0), "))"); } return cgxg_callN(c, "cgSum", ns, &t0, 1, prec); }
        case CGB_SORT: {
            CgxDoc *l = k0 == CGXC_K_LIST ? cgxg_exw(c, ns[0], &t0, 0) : cgxg_pre(c, "cgLister(", cgxg_exw(c, ns[0], &A, 0), ")");
            *nat = k0 == CGXC_K_LIST ? t0 : LA; r = cgxg_pre(c, "cgSort(", l, ")");
            if (k0 == CGXC_K_LIST && cgxc_ek(t0.e) != 4) r = cgxg_conv(c, r, &A, &t0);
            return r;
        }
        case CGB_REVERSE: *nat = t0;
            if (k0 == CGXC_K_STR) return cgxg_callN(c, "cgSRev", ns, &S, 1, prec);
            if (k0 == CGXC_K_LIST) { r = cgx_dcat(GD, cgxg_exw(c, ns[0], &t0, 8), cgxg_t(c, ".RevAny()")); return cgxc_ek(t0.e) == 4 ? r : cgxg_conv(c, r, &A, &t0); }
            *nat = A; return cgxg_callN(c, "cgVRev", ns, &A, 1, prec);
        case CGB_MATCHES: { CgxTy w[2] = {S, S}; *nat = B; return cgxg_callN(c, "cgMatches", ns, w, 2, prec); }
        case CGB_ERRMSG: *nat = S; if (k0 == CGXC_K_STR) return cgxg_exw(c, ns[0], &S, 0); return cgxg_callN(c, "cgStr", ns, &A, 1, prec);
    }
    (void)b; (void)t1;
    cgx_err(c->g, n->line, "go: built-in `%s` has no Go form yet", n->s);
    return cgx_dnil(GD);
}
static int cgxg_isnum(int k) { return k == CGXC_K_INT || k == CGXC_K_FLT; }
static CgxDoc *cgxg_nullcmp(CgxG *c, const CgxNode *x, int eq, int *prec) {
    CgxTy tx = cgxg_vty(x); const int k = cgxc_kind(&tx); *prec = 3;
    return cgx_dcatn(GD, cgxg_exw(c, x, &tx, 4), cgxg_t(c, k == CGXC_K_STR ? (eq ? " == \"\"" : " != \"\"") : (eq ? " == nil" : " != nil")), NULL);
}
/* duas expressoes iguais (so literais, variaveis e operadores): `a && a` o go vet recusa e o valor e o mesmo */
static int cgxg_same(const CgxNode *a, const CgxNode *b) {
    if (!a || !b) return a == b;
    if (a->k != b->k || a->op != b->op) return 0;
    switch (a->k) {
        case CGN_BOOL: case CGN_INT: return a->iv == b->iv;
        case CGN_FLT: return a->dv == b->dv;
        case CGN_STR: return !strcmp(a->s, b->s);
        case CGN_VAR: return !strcmp(a->s, b->s) && a->ref == b->ref;
        case CGN_UN: return cgxg_same(a->a, b->a);
        case CGN_BIN: return cgxg_same(a->a, b->a) && cgxg_same(a->b, b->b);
    }
    return 0;
}
static int cgxg_constbool(const CgxNode *n, int *v) {
    int x, y;
    if (n->k == CGN_BOOL) { *v = (int)n->iv; return 1; }
    if (n->k == CGN_UN && n->op == CGO_NOT && cgxg_constbool(n->a, &x)) { *v = !x; return 1; }
    if (n->k == CGN_BIN && (n->op == CGO_AND || n->op == CGO_OR) && cgxg_constbool(n->a, &x) && cgxg_constbool(n->b, &y)) { *v = n->op == CGO_AND ? x && y : x || y; return 1; }
    return 0;
}
static CgxDoc *cgxg_bin(CgxG *c, const CgxNode *n, int *prec, CgxTy *nat) {
    const int top = c->curtop;
    const CgxTy ta = cgxg_vty(n->a), tb = cgxg_vty(n->b); const int ka = cgxc_kind(&ta), kb = cgxc_kind(&tb), op = n->op;
    CgxTy I = cgxg_ty(CGT_INT), F = cgxg_ty(CGT_FLOAT), S = cgxg_ty(CGT_STR), B = cgxg_ty(CGT_BOOL), A = cgxg_ty(CGT_ANY);
    const CgxNode *ns[2] = {n->a, n->b}; CgxTy w[2];
    static const char *const cmpsym[] = {"==", "!=", "<", "<=", ">", ">="};
    *prec = 8; *nat = n->ty;
    switch (op) {
        case CGO_AND: case CGO_OR:
            *nat = B;
            { int cv; if (cgxg_constbool(n, &cv)) return cgxg_t(c, cv ? "true" : "false"); }
            {                                                           /* a && b && a: o go vet recusa a repeticao; o valor e o mesmo, entao a cadeia perde os iguais */
                const CgxNode *ops[32]; int no = 0;
                const CgxNode *stack[32]; int sp = 0; stack[sp++] = n;
                while (sp) {
                    const CgxNode *x = stack[--sp];
                    if (x->k == CGN_BIN && x->op == op && sp < 30) { stack[sp++] = x->b; stack[sp++] = x->a; continue; }
                    int dup = 0, cv; for (int i = 0; i < no; i++) if (cgxg_same(ops[i], x)) dup = 1;
                    if (cgxg_constbool(x, &cv)) { for (int i = 0; i < no; i++) { int cw; if (cgxg_constbool(ops[i], &cw) && cw == cv) dup = 1; } }
                    if (!dup && no < 32) ops[no++] = x;
                }
                if (no == 1) return cgxg_exw(c, ops[0], &B, 0);
                CgxDoc *r = NULL; *prec = op == CGO_AND ? 2 : 1;
                for (int i = 0; i < no; i++) { CgxDoc *x = cgxg_exw(c, ops[i], &B, 4); r = r ? cgx_dcatn(GD, r, cgxg_t(c, op == CGO_AND ? " && " : " || "), x, NULL) : x; }
                return r;
            }
        case CGO_EQ: case CGO_NE: case CGO_LT: case CGO_LE: case CGO_GT: case CGO_GE: {
            const int ne = op == CGO_NE, ord = op >= CGO_LT; *nat = B;
            if (!ord && (ka == CGXC_K_NULL || kb == CGXC_K_NULL)) {
                if (ka == CGXC_K_NULL && kb == CGXC_K_NULL) return cgxg_t(c, ne ? "false" : "true");
                return cgxg_nullcmp(c, ka == CGXC_K_NULL ? n->b : n->a, !ne, prec);
            }
            if (cgxg_isnum(ka) && cgxg_isnum(kb)) { const CgxTy w1 = (ka == CGXC_K_FLT || kb == CGXC_K_FLT) ? F : I; return cgxg_infix(c, cmpsym[op - CGO_EQ], n->a, n->b, &w1, &w1, 3, prec); }
            if (ka == CGXC_K_BOOL && kb == CGXC_K_BOOL) return cgxg_infix(c, cmpsym[op - CGO_EQ], n->a, n->b, &B, &B, 3, prec);
            if (ka == CGXC_K_STR && kb == CGXC_K_STR) return cgxg_infix(c, cmpsym[op - CGO_EQ], n->a, n->b, &S, &S, 3, prec);
            if (!ord && ka == CGXC_K_OBJ && kb == CGXC_K_OBJ) return cgxg_infix(c, cmpsym[op - CGO_EQ], n->a, n->b, &ta, &tb, 3, prec);
            w[0] = w[1] = A;
            if (!ord) { CgxDoc *d = cgxg_callN(c, "cgEq", ns, w, 2, prec); if (ne) { *prec = 6; return cgx_dcat(GD, cgxg_t(c, "!"), d); } return d; }
            { CgxDoc *d = cgxg_callN(c, "cgCmp", ns, w, 2, prec); char t[8]; snprintf(t, sizeof t, " %s 0", cmpsym[op - CGO_EQ]); return cgxg_cmp0(c, d, prec, t); }
        }
        case CGO_IN: {
            *nat = B;
            if (kb == CGXC_K_STR) { CgxDoc *d = cgx_dcatn(GD, cgxg_t(c, "strings.Contains("), cgxg_exw(c, n->b, &S, 0), cgxg_t(c, ", "), cgxg_exw(c, n->a, &S, 0), cgxg_t(c, ")"), NULL); return d; }
            if (kb == CGXC_K_MAP) return cgx_dcatn(GD, cgxg_exw(c, n->b, &tb, 8), cgxg_t(c, ".HasKey("), cgxg_exw(c, n->a, &S, 0), cgxg_t(c, ")"), NULL);
            return cgx_dcatn(GD, cgxg_t(c, "cgVHas("), cgxg_exw(c, n->b, &A, 0), cgxg_t(c, ", "), cgxg_exw(c, n->a, &A, 0), cgxg_t(c, ")"), NULL);
        }
        case CGO_ADD: case CGO_SUB: case CGO_MUL: case CGO_DIV: case CGO_IDIV: case CGO_MOD: {
            if (ka == CGXC_K_ANY || kb == CGXC_K_ANY || (ka == CGXC_K_STR && kb != CGXC_K_STR) || (kb == CGXC_K_STR && ka != CGXC_K_STR)) {
                CgxDoc *o0 = cgxg_exw(c, n->a, &A, 0), *o1 = cgxg_exw(c, n->b, &A, 0); *nat = A;
                return cgx_dcatn(GD, cgx_dtextf(GD, "cgArith(%d, ", op), o0, cgxg_t(c, ", "), o1, cgxg_t(c, ")"), NULL);
            }
            if (ka == CGXC_K_STR) { *nat = S; if (!top) { w[0] = w[1] = S; return cgxg_callN(c, "cgCat", ns, w, 2, prec); } return cgxg_infix(c, "+", n->a, n->b, &S, &S, 4, prec); }
            if (!cgxg_isnum(ka) || !cgxg_isnum(kb)) { cgx_err(c->g, n->line, "go: operands of %s must be numbers or str", CGO_NAME[op]); return cgx_dnil(GD); }
            const int fl = ka == CGXC_K_FLT || kb == CGXC_K_FLT;
            if (op == CGO_DIV) { w[0] = w[1] = F; *nat = F; return cgxg_callN(c, "cgFDiv", ns, w, 2, prec); }
            if (op == CGO_IDIV || op == CGO_MOD) {
                if (fl) { w[0] = w[1] = F; *nat = F; return cgxg_callN(c, "cgFMod", ns, w, 2, prec); }
                w[0] = w[1] = I; *nat = I; return cgxg_callN(c, op == CGO_IDIV ? "cgIDiv" : "cgMod", ns, w, 2, prec);
            }
            if (fl) {
                *nat = F;
                if (!top) { w[0] = w[1] = F; return cgxg_callN(c, op == CGO_ADD ? "cgFAdd" : op == CGO_SUB ? "cgFSub" : "cgFMul", ns, w, 2, prec); }
                return cgxg_infix(c, op == CGO_ADD ? "+" : op == CGO_SUB ? "-" : "*", n->a, n->b, &F, &F, op == CGO_MUL ? 5 : 4, prec);
            }
            w[0] = w[1] = I; *nat = I; return cgxg_callN(c, op == CGO_ADD ? "cgAdd" : op == CGO_SUB ? "cgSub" : "cgMul", ns, w, 2, prec);
        }
    }
    cgx_err(c->g, n->line, "go: operator %s is not supported here", CGO_NAME[op]);
    return cgx_dnil(GD);
}
static const CgxNode *cgxg_class(const CgxG *c, const char *name) { for (int i = 0; i < c->ncls; i++) if (!strcmp(c->cls_list[i]->s, name)) return c->cls_list[i]; return NULL; }
static int cgxg_has_field(const CgxNode *cls, const char *name) { for (int i = 0; i < cls->b1.n; i++) if (cls->b1.p[i]->k == CGN_FIELDDECL && !strcmp(cls->b1.p[i]->s, name)) return 1; return 0; }
static void cgxg_mname(const CgxNode *cls, const char *name, CgxB *o) { cgxg_name(name, o); if (cls && cgxg_has_field(cls, name)) cgx_bs(o, "_"); }
static CgxDoc *cgxg_dyncall(CgxG *c, int kind, const char *name, const CgxNode *recv, const CgxBlk *args, const CgxNode *val, int line, int *prec) {
    int idx = -1, k = args ? args->n : 0; char fn[96]; CgxTy A = cgxg_ty(CGT_ANY); CgxDoc *o[40]; int m = 0;
    for (int i = 0; i < c->ndyn; i++) if (c->dyn[i].kind == kind && c->dyn[i].n == k && !strcmp(c->dyn[i].name, name)) idx = i;
    if (idx < 0) {
        if (c->ndyn >= 96) { cgx_err(c->g, line, "go: too many dynamic member accesses"); return cgx_dnil(GD); }
        idx = c->ndyn++; snprintf(c->dyn[idx].name, sizeof c->dyn[idx].name, "%s", name); c->dyn[idx].kind = kind; c->dyn[idx].n = k;
    }
    snprintf(fn, sizeof fn, "cgd%c_%s_%d", kind == 0 ? 'm' : kind == 1 ? 'f' : 's', name, k);
    o[m++] = cgxg_exw(c, recv, &A, 0);
    for (int i = 0; i < k && m < 39; i++) o[m++] = cgxg_exw(c, args->p[i], &A, 0);
    if (val) o[m++] = cgxg_exw(c, val, &A, 0);
    *prec = 8; return cgx_dcat(GD, cgxg_t(c, fn), cgxg_seq(c, "(", o, m, ")"));
}
static CgxDoc *cgxg_userargs(CgxG *c, const CgxNode *callee, const CgxBlk *args, CgxDoc *head, int *prec) {
    CgxDoc *o[64]; int m = 0; const int np = callee ? callee->npar : 0;
    for (int i = 0; i < np && m < 64; i++) { const CgxParam *p = &callee->par[i]; const CgxNode *x = i < args->n ? args->p[i] : p->def; CgxTy pt = p->ty; if (!x) x = args->p[0]; o[m++] = cgxg_exw(c, x, &pt, 0); }
    *prec = 8; return cgx_dcat(GD, head, cgxg_seq(c, "(", o, m, ")"));
}
static CgxDoc *cgxg_ex0(CgxG *c, const CgxNode *n, int *prec, CgxTy *nat) {
    CgxB b = {0}; CgxDoc *d; *prec = 9; *nat = n->ty;
    switch (n->k) {
        case CGN_INT: cgx_bf(&b, "%lld", n->iv); if (n->iv < 0) *prec = 6; break;
        case CGN_FLT: { char t[64]; cgx_fmt_float(n->dv, t, sizeof t); cgx_bs(&b, t); if (n->dv < 0) *prec = 6; break; }
        case CGN_STR: cgxg_cstr(n->s, &b); break;
        case CGN_BOOL: cgx_bs(&b, n->iv ? "true" : "false"); break;
        case CGN_NULL: cgx_bs(&b, "nil"); *nat = cgxg_ty(CGT_NULL); break;
        case CGN_VAR:
            if (n->flag == 77) cgx_bs(&b, n->s); else cgxg_name(n->s, &b);
            *nat = cgxg_vty(n); break;
        case CGN_LIST: free(b.p); { CgxTy w = n->ty; *nat = n->ty; return cgxg_list(c, n, &w); }
        case CGN_MAP: free(b.p); { CgxTy w = n->ty; *nat = n->ty; return cgxg_map(c, n, &w); }
        case CGN_INDEX: {
            free(b.p); const CgxTy tc = cgxg_vty(n->a); const int kc = cgxc_kind(&tc); CgxTy I = cgxg_ty(CGT_INT), S = cgxg_ty(CGT_STR), A = cgxg_ty(CGT_ANY); *prec = 8;
            if (kc == CGXC_K_LIST) {
                *nat = cgx_t(tc.e);
                if (cgxc_ek(tc.e) == 4) return cgx_dcatn(GD, cgxg_t(c, "cgVGet("), cgxg_exw(c, n->a, &A, 0), cgxg_t(c, ", "), cgxg_exw(c, n->b, &A, 0), cgxg_t(c, ")"), NULL);
                return cgx_dcatn(GD, cgxg_exw(c, n->a, &tc, 8), cgxg_t(c, ".Get("), cgxg_exw(c, n->b, &I, 0), cgxg_t(c, ")"), NULL);
            }
            if (kc == CGXC_K_MAP) {
                *nat = cgx_t(tc.e);
                if (cgxc_ek(tc.e) == 4) return cgx_dcatn(GD, cgxg_t(c, "cgVGet("), cgxg_exw(c, n->a, &A, 0), cgxg_t(c, ", "), cgxg_exw(c, n->b, &A, 0), cgxg_t(c, ")"), NULL);
                return cgx_dcatn(GD, cgxg_exw(c, n->a, &tc, 8), cgxg_t(c, ".Get("), cgxg_exw(c, n->b, &S, 0), cgxg_t(c, ")"), NULL);
            }
            if (kc == CGXC_K_STR) { *nat = S; return cgx_dcatn(GD, cgxg_t(c, "cgSIdx("), cgxg_exw(c, n->a, &S, 0), cgxg_t(c, ", "), cgxg_exw(c, n->b, &I, 0), cgxg_t(c, ")"), NULL); }
            *nat = A; return cgx_dcatn(GD, cgxg_t(c, "cgVGet("), cgxg_exw(c, n->a, &A, 0), cgxg_t(c, ", "), cgxg_exw(c, n->b, &A, 0), cgxg_t(c, ")"), NULL);
        }
        case CGN_FIELD: {
            free(b.p); const CgxTy tc = cgxg_vty(n->a);
            if (cgxc_kind(&tc) == CGXC_K_ANY) { *nat = cgxg_ty(CGT_ANY); return cgxg_dyncall(c, 1, n->s, n->a, NULL, NULL, n->line, prec); }
            if (cgxc_kind(&tc) != CGXC_K_OBJ) { cgx_err(c->g, n->line, "go: the field `%s` is read from a value that is not an object", n->s); return cgx_dnil(GD); }
            CgxB f = {0}; cgx_bs(&f, "."); cgxg_name(n->s, &f); *prec = 8; *nat = n->ref ? n->ref->ty : n->ty;
            d = cgx_dcatn(GD, cgxg_exw(c, n->a, &tc, 8), cgx_dtext(GD, f.p), NULL); free(f.p); return d;
        }
        case CGN_UN: free(b.p);
            if (n->op == CGO_NOT) { CgxTy B = cgxg_ty(CGT_BOOL); *prec = 6; *nat = B; return cgx_dcat(GD, cgxg_t(c, "!"), cgxg_exw(c, n->a, &B, 6)); }
            { const CgxTy ta = cgxg_vty(n->a); const int k = cgxc_kind(&ta); const CgxNode *ns[1] = {n->a};
              if (k == CGXC_K_FLT) { *prec = 6; *nat = ta; return cgx_dcat(GD, cgxg_t(c, "-"), cgxg_exw(c, n->a, &ta, 6)); }
              if (k == CGXC_K_INT) { CgxTy I = cgxg_ty(CGT_INT); *nat = I; return cgxg_callN(c, "cgNeg", ns, &I, 1, prec); }
              { CgxTy A = cgxg_ty(CGT_ANY); *nat = A; return cgxg_callN(c, "cgVNeg", ns, &A, 1, prec); } }
        case CGN_BIN: free(b.p); return cgxg_bin(c, n, prec, nat);
        case CGN_TERN: {
            free(b.p); CgxTy B = cgxg_ty(CGT_BOOL); const CgxTy r = n->ty; *prec = 8; *nat = r; char *ts = cgxg_ts(&r);
            CgxDoc *in = cgx_dcatn(GD, cgx_dhard(GD), cgxg_t(c, "if "), cgxg_exw(c, n->a, &B, 0), cgxg_t(c, " {"), cgx_dnest(GD, 1, cgx_dcatn(GD, cgx_dhard(GD), cgxg_t(c, "return "), cgxg_exw(c, n->b, &r, 0), NULL)),
                cgx_dhard(GD), cgxg_t(c, "}"), cgx_dhard(GD), cgxg_t(c, "return "), cgxg_exw(c, n->c, &r, 0), NULL);
            CgxDoc *res = cgx_dcatn(GD, cgx_dtextf(GD, "func() %s {", ts), cgx_dnest(GD, 1, in), cgx_dhard(GD), cgxg_t(c, "}()"), NULL);
            free(ts); return res;
        }
        case CGN_CALL: {
            free(b.p);
            if (n->bi) return cgxg_builtin(c, n, prec, nat);
            if (n->ref && n->ref->k == CGN_CLASS) {
                const CgxNode *cls = n->ref, *init = NULL; CgxB f = {0};
                for (int i = 0; i < cls->b1.n; i++) if (cls->b1.p[i]->k == CGN_FN && !strcmp(cls->b1.p[i]->s, "init")) init = cls->b1.p[i];
                cgx_bs(&f, "new"); { CgxB cn = {0}; cgxg_name(cls->s, &cn); cgx_bs(&f, cn.p); free(cn.p); }
                *nat = n->ty; d = cgxg_userargs(c, init, &n->b1, cgx_dtext(GD, f.p), prec); free(f.p); return d;
            }
            if (n->ref && n->ref->k == CGN_FN) { CgxB f = {0}; cgxg_name(n->ref->s, &f); *nat = n->ref->ty; d = cgxg_userargs(c, n->ref, &n->b1, cgx_dtext(GD, f.p), prec); free(f.p); return d; }
            cgx_err(c->g, n->line, "go: call to `%s` could not be resolved", n->s); return cgx_dnil(GD);
        }
        case CGN_MCALL: {
            free(b.p); const CgxTy tr = cgxg_vty(n->a);
            if (cgxc_kind(&tr) == CGXC_K_ANY) { *nat = cgxg_ty(CGT_ANY); return cgxg_dyncall(c, 0, n->s, n->a, &n->b1, NULL, n->line, prec); }
            if (cgxc_kind(&tr) != CGXC_K_OBJ || !n->ref) { cgx_err(c->g, n->line, "go: the method `%s` is called on a value that is not an object", n->s); return cgx_dnil(GD); }
            CgxB f = {0}; cgx_bs(&f, "."); cgxg_mname(cgxg_class(c, tr.cls), n->s, &f); *nat = n->ref->ty;
            d = cgxg_userargs(c, n->ref, &n->b1, cgx_dcatn(GD, cgxg_exw(c, n->a, &tr, 8), cgx_dtext(GD, f.p), NULL), prec); free(f.p); return d;
        }
        default: cgx_err(c->g, n->line, "go: unexpected expression node %d", n->k); free(b.p); return cgx_dnil(GD);
    }
    d = cgx_dtext(GD, b.p ? b.p : ""); free(b.p); return d;
}
static CgxDoc *cgxg_exw(CgxG *c, const CgxNode *n, const CgxTy *want, int minprec) {
    const CgxTy w = want ? *want : cgxg_vty(n); c->curtop = c->top; c->top = 0;
    if (n->k == CGN_LIST) return cgxg_wrap(c, cgxg_list(c, n, &w), 8, minprec);
    if (n->k == CGN_MAP) return cgxg_wrap(c, cgxg_map(c, n, &w), 8, minprec);
    if (n->k == CGN_INT && cgxc_kind(&w) == CGXC_K_FLT) { CgxB b = {0}; cgx_bf(&b, "%lld.0", n->iv); CgxDoc *d = cgx_dtext(GD, b.p); free(b.p); return cgxg_wrap(c, d, n->iv < 0 ? 6 : 9, minprec); }
    int prec; CgxTy nat; CgxDoc *d = cgxg_ex0(c, n, &prec, &nat);
    const int kn = cgxc_kind(&nat), kw = cgxc_kind(&w);
    if (kn != kw || (kn == CGXC_K_LIST && nat.e != w.e) || (kn == CGXC_K_MAP && nat.e != w.e) || kn == CGXC_K_NULL) {
        d = cgxg_conv(c, cgxg_wrap(c, d, prec, 0), &nat, &w);
        prec = 8;
    }
    return cgxg_wrap(c, d, prec, minprec);
}

/* ---- instrucoes ---- */
/* ate a primeira instrucao que encerra o bloco (return, throw, break, continue): o resto e codigo morto e o go vet o recusa */
static int cgxg_nlive(const CgxBlk *b) {
    for (int i = 0; i < b->n; i++) { const int k = b->p[i]->k; if (k == CGN_RET || k == CGN_THROW || k == CGN_BREAK || k == CGN_CONT) return i + 1; }
    return b->n;
}
static void cgxg_scan(CgxG *c, const CgxNode *n) {
    if (!n) return;
    if (n->k == CGN_BIN && n->op >= CGO_EQ && n->op <= CGO_GE && n->a->k == CGN_VAR && n->b->k == CGN_VAR && !strcmp(n->a->s, n->b->s) && n->a->ref == n->b->ref && 0) return;
    if (n->k == CGN_VAR && n->ref) {
        if (c->nused == c->capused) { c->capused = c->capused ? c->capused * 2 : 64; c->used = realloc(c->used, sizeof c->used[0] * (size_t)c->capused); }
        c->used[c->nused].ref = n->ref; c->used[c->nused].name = n->s; c->nused++;
    }
    if (n->k == CGN_ASSIGN && n->op < 0 && n->a->k == CGN_VAR) cgxg_scan(c, n->b);
    else { cgxg_scan(c, n->a); cgxg_scan(c, n->b); }
    cgxg_scan(c, n->c);
    const CgxBlk *bl[3] = {&n->b1, &n->b2, &n->b3};
    for (int k = 0; k < 3; k++) for (int i = 0, m = cgxg_nlive(bl[k]); i < m; i++) cgxg_scan(c, bl[k]->p[i]);
    for (int i = 0; i < n->npar; i++) cgxg_scan(c, n->par[i].def);
}
static int cgxg_isused(const CgxG *c, const void *ref, const char *name) { for (int i = 0; i < c->nused; i++) if (c->used[i].ref == ref && !strcmp(c->used[i].name, name)) return 1; return 0; }
static CgxDoc *cgxg_stmt(CgxG *c, const CgxNode *s);
static CgxDoc *cgxg_stmts(CgxG *c, const CgxBlk *b, CgxDoc *prologue) {
    CgxDoc *in = prologue ? cgx_dcat(GD, cgx_dhard(GD), prologue) : NULL;
    for (int i = 0, m = cgxg_nlive(b); i < m; i++) {
        const CgxNode *s = b->p[i];
        if ((s->k == CGN_COMMENT || s->k == CGN_DOC) && !c->o->comments) continue;
        CgxDoc *d = cgxg_stmt(c, s); if (!d) continue;
        CgxDoc *x = cgx_dcat(GD, cgx_dhard(GD), d); in = in ? cgx_dcat(GD, in, x) : x;
    }
    return in;
}
static CgxDoc *cgxg_block(CgxG *c, const CgxBlk *b, CgxDoc *prologue, CgxDoc *epilogue) {
    CgxDoc *in = cgxg_stmts(c, b, prologue);
    if (epilogue) { CgxDoc *x = cgx_dcat(GD, cgx_dhard(GD), epilogue); in = in ? cgx_dcat(GD, in, x) : x; }
    if (!in) return cgx_dcatn(GD, cgxg_t(c, "{"), cgx_dhard(GD), cgxg_t(c, "}"), NULL);
    return cgx_dcatn(GD, cgxg_t(c, "{"), cgx_dnest(GD, 1, in), cgx_dhard(GD), cgxg_t(c, "}"), NULL);
}
static CgxDoc *cgxg_wrapblock(CgxG *c, CgxDoc *inner) { return cgx_dcatn(GD, cgxg_t(c, "{"), cgx_dnest(GD, 1, inner), cgx_dhard(GD), cgxg_t(c, "}"), NULL); }
static CgxDoc *cgxg_unused(CgxG *c, CgxDoc *d, const char *name, int used) { return used ? d : cgx_dcatn(GD, d, cgx_dhard(GD), cgx_dtextf(GD, "_ = %s", name), NULL); }
static const char *cgxg_zero(const CgxTy *t) {
    switch (cgxc_kind(t)) { case CGXC_K_INT: return "0"; case CGXC_K_FLT: return "0.0"; case CGXC_K_BOOL: return "false"; case CGXC_K_STR: return "\"\""; }
    return "nil";
}
static int cgxg_terminates(const CgxBlk *b) {
    for (int i = b->n - 1; i >= 0; i--) {
        const CgxNode *s = b->p[i];
        if (s->k == CGN_COMMENT || s->k == CGN_DOC) continue;
        if (s->k == CGN_RET || s->k == CGN_THROW || s->k == CGN_BREAK || s->k == CGN_CONT) return 1;
        if (s->k == CGN_IF) { if (!s->b2.n || !cgxg_terminates(&s->b1)) return 0; if (s->b2.n == 1 && s->b2.p[0]->k == CGN_IF && s->b2.p[0]->flag) return cgxg_terminates(&s->b2); return cgxg_terminates(&s->b2); }
        return 0;
    }
    return 0;
}
/* break, continue e return: dentro de um closure de try viram codigos de controle (1, 2, 3) para o codigo depois do cgTry */
static int cgxg_flags;
static CgxDoc *cgxg_ctl(CgxG *c, int what, CgxDoc *value) {       /* what: 1 break, 2 continue, 3 return */
    if (c->infin && (what == 3 || !c->nloop || c->tdepth != c->loop_td[c->nloop - 1])) { cgx_err(c->g, c->line, "go: `return`, `break` and `continue` that leave a `finally` block are not supported"); return cgxg_t(c, "return"); }
    if (what == 3) {
        if (!c->tdepth) return value ? cgx_dcatn(GD, cgxg_t(c, "return "), value, NULL) : cgxg_t(c, "return");
        cgxg_flags |= 8;
        if (value) { c->need_rv = 1; return cgx_dcatn(GD, cgxg_t(c, "cgrv = "), value, cgx_dhard(GD), cgxg_t(c, "return 3"), NULL); }
        return cgxg_t(c, "return 3");
    }
    if (c->nloop && c->tdepth == c->loop_td[c->nloop - 1]) return cgxg_t(c, what == 1 ? "break" : "continue");
    cgxg_flags |= what; return cgxg_t(c, what == 1 ? "return 1" : "return 2");
}
static CgxDoc *cgxg_store(CgxG *c, const CgxNode *t, const CgxNode *rhs, const CgxTy *tt) {
    CgxTy A = cgxg_ty(CGT_ANY), I = cgxg_ty(CGT_INT), S = cgxg_ty(CGT_STR);
    if (t->k == CGN_VAR) { CgxB nm = {0}; if (t->flag == 77) cgx_bs(&nm, t->s); else cgxg_name(t->s, &nm); c->top = 1; CgxDoc *d = cgx_dcatn(GD, cgx_dtextf(GD, "%s = ", nm.p), cgxg_exw(c, rhs, tt, 0), NULL); free(nm.p); return d; }
    if (t->k == CGN_FIELD) {
        const CgxTy to = cgxg_vty(t->a);
        if (cgxc_kind(&to) == CGXC_K_ANY) { int pr; return cgxg_dyncall(c, 2, t->s, t->a, NULL, rhs, t->line, &pr); }
        if (cgxc_kind(&to) != CGXC_K_OBJ) { cgx_err(c->g, t->line, "go: the field `%s` is assigned on a value that is not an object", t->s); return NULL; }
        CgxB f = {0}; cgx_bs(&f, "."); cgxg_name(t->s, &f);
        CgxDoc *d = cgx_dcatn(GD, cgxg_exw(c, t->a, &to, 8), cgx_dtext(GD, f.p), cgxg_t(c, " = "), cgxg_exw(c, rhs, tt, 0), NULL); free(f.p); return d;
    }
    const CgxTy tc = cgxg_vty(t->a); const int kc = cgxc_kind(&tc);
    if (kc == CGXC_K_LIST && cgxc_ek(tc.e) != 4) return cgx_dcatn(GD, cgxg_exw(c, t->a, &tc, 8), cgxg_t(c, ".Set("), cgxg_exw(c, t->b, &I, 0), cgxg_t(c, ", "), cgxg_exw(c, rhs, tt, 0), cgxg_t(c, ")"), NULL);
    if (kc == CGXC_K_MAP && cgxc_ek(tc.e) != 4) return cgx_dcatn(GD, cgxg_exw(c, t->a, &tc, 8), cgxg_t(c, ".Set("), cgxg_exw(c, t->b, &S, 0), cgxg_t(c, ", "), cgxg_exw(c, rhs, tt, 0), cgxg_t(c, ")"), NULL);
    if (kc == CGXC_K_LIST || kc == CGXC_K_MAP || kc == CGXC_K_ANY) { CgxTy ix = A; return cgx_dcatn(GD, cgxg_t(c, "cgVSet("), cgxg_exw(c, t->a, &A, 0), cgxg_t(c, ", "), cgxg_exw(c, t->b, &ix, 0), cgxg_t(c, ", "), cgxg_exw(c, rhs, &A, 0), cgxg_t(c, ")"), NULL); }
    cgx_err(c->g, t->line, "go: cannot assign to an element of this value"); return NULL;
}
static CgxNode *cgxg_synth_var(CgxG *c, const char *name, CgxTy ty) { CgxNode *v = (CgxNode *)cgx_alloc(c->g, sizeof *v); v->k = CGN_VAR; v->s = cgx_dup(c->g, name); v->flag = 77; v->ty = ty; return v; }
static CgxDoc *cgxg_assign(CgxG *c, const CgxNode *s) {
    const CgxNode *t = s->a; CgxTy tt = t->k == CGN_VAR ? cgxg_vty(t) : t->k == CGN_FIELD && t->ref ? t->ref->ty : t->ty;
    if (cgxc_kind(&tt) == CGXC_K_VOID) tt = cgxg_ty(CGT_ANY);
    if (s->op < 0 && t->k == CGN_VAR && s->b->k == CGN_VAR && !strcmp(t->s, s->b->s) && t->ref == s->b->ref) { CgxB nm = {0}; cgxg_name(t->s, &nm); CgxDoc *d = cgx_dtextf(GD, "_ = %s", nm.p); free(nm.p); return d; }
    if (s->op < 0) return cgxg_store(c, t, s->b, &tt);
    if (s->op == CGO_DIV && cgxc_kind(&tt) == CGXC_K_INT) { cgx_err(c->g, s->line, "go: `/=` on an int variable would store a float; use `x = x // y`, or declare the variable float"); return NULL; }
    CgxNode bin; memset(&bin, 0, sizeof bin); bin.k = CGN_BIN; bin.op = s->op; bin.line = s->line; bin.ty = tt;
    if ((t->k == CGN_INDEX || t->k == CGN_FIELD) && !(t->a->k == CGN_VAR && (t->k == CGN_FIELD || t->b->k == CGN_VAR || t->b->k == CGN_INT || t->b->k == CGN_STR))) {
        const int id = ++c->uniq; char n1[24], n2[24]; const CgxTy tc = cgxg_vty(t->a); CgxNode t2 = *t; CgxDoc *in; CgxTy I = cgxg_ty(CGT_INT);
        snprintf(n1, sizeof n1, "cgc%d", id); snprintf(n2, sizeof n2, "cgd%d", id);
        t2.a = cgxg_synth_var(c, n1, tc);
        in = cgx_dcatn(GD, cgx_dhard(GD), cgx_dtextf(GD, "%s := ", n1), cgxg_exw(c, t->a, &tc, 0), NULL);
        if (t->k == CGN_INDEX) {
            CgxTy it = cgxc_kind(&tc) == CGXC_K_MAP ? cgxg_ty(CGT_STR) : cgxc_kind(&tc) == CGXC_K_LIST ? I : cgxg_ty(CGT_ANY);
            t2.b = cgxg_synth_var(c, n2, it); in = cgx_dcatn(GD, in, cgx_dhard(GD), cgx_dtextf(GD, "%s := ", n2), cgxg_exw(c, t->b, &it, 0), NULL);
        }
        bin.a = &t2; bin.b = s->b; CgxDoc *st = cgxg_store(c, &t2, &bin, &tt); if (!st) return NULL;
        return cgxg_wrapblock(c, cgx_dcatn(GD, in, cgx_dhard(GD), st, NULL));
    }
    bin.a = (CgxNode *)t; bin.b = s->b;
    return cgxg_store(c, t, &bin, &tt);
}
static CgxDoc *cgxg_try(CgxG *c, const CgxNode *s) {
    const int id = ++c->uniq, hc = s->flag, hf = (int)s->iv; const int saved = cgxg_flags; cgxg_flags = 0;
    CgxDoc *body, *catchd = NULL, *find = NULL; CgxTy rt = c->rett;
    c->tdepth++;
    CgxDoc *ret0 = cgxg_t(c, "return 0");
    body = cgxg_block(c, &s->b1, NULL, cgxg_terminates(&s->b1) ? NULL : ret0);
    if (hc) {
        CgxDoc *decl = NULL;
        if (s->s2) { CgxB nm = {0}; cgxg_name(s->s2, &nm); decl = cgx_dtextf(GD, "%s := msg", nm.p); decl = cgxg_unused(c, decl, nm.p, cgxg_isused(c, s, s->s2)); free(nm.p); }
        else decl = cgx_dtext(GD, "_ = msg");
        catchd = cgxg_block(c, &s->b2, decl, cgxg_terminates(&s->b2) ? NULL : cgxg_t(c, "return 0"));
    }
    if (hf) { c->tdepth++; c->infin++; find = cgxg_block(c, &s->b3, NULL, NULL); c->infin--; c->tdepth--; }
    c->tdepth--;
    const int fl = cgxg_flags; cgxg_flags = saved; (void)rt;
    CgxDoc *call = cgx_dcatn(GD, cgxg_t(c, "cgTry(func() int "), body, cgxg_t(c, ", "), hc ? cgx_dcatn(GD, cgxg_t(c, "func(msg string) int "), catchd, NULL) : cgxg_t(c, "nil"), cgxg_t(c, ", "),
        hf ? cgx_dcatn(GD, cgxg_t(c, "func() "), find, NULL) : cgxg_t(c, "nil"), cgxg_t(c, ")"), NULL);
    if (!fl) return call;
    CgxDoc *r = cgx_dcatn(GD, cgx_dtextf(GD, "cgc%d := ", id), call, NULL);
    for (int code = 1; code <= 3; code++) {
        const int bit = code == 3 ? 8 : code; if (!(fl & bit)) continue;
        CgxDoc *act = cgxg_ctl(c, code, code == 3 && c->rett.k != CGT_VOID && !c->tdepth ? cgxg_t(c, "cgrv") : NULL);
        if (code == 3 && c->tdepth && c->rett.k != CGT_VOID) c->need_rv = 1;
        r = cgx_dcatn(GD, r, cgx_dhard(GD), cgx_dtextf(GD, "if cgc%d == %d ", id, code), cgx_dcatn(GD, cgxg_t(c, "{"), cgx_dnest(GD, 1, cgx_dcat(GD, cgx_dhard(GD), act)), cgx_dhard(GD), cgxg_t(c, "}"), NULL), NULL);
    }
    return r;
}
static CgxDoc *cgxg_stmt(CgxG *c, const CgxNode *s) {
    CgxTy B = cgxg_ty(CGT_BOOL), I = cgxg_ty(CGT_INT); c->line = s->line;
    switch (s->k) {
        case CGN_COMMENT: case CGN_DOC: { if (!s->defs) return NULL; CgxB b = {0}; cgx_bs(&b, "// "); cgx_bs(&b, s->defs); CgxDoc *d = cgx_dtext(GD, b.p); free(b.p); return d; }
        case CGN_RAW: cgx_err(c->g, s->line, "go: `raw` blocks have no Go version (the raw languages are python, js, ts and c)"); return NULL;
        case CGN_LET: {
            CgxTy t = s->ty; if (cgxc_kind(&t) == CGXC_K_VOID || cgxc_kind(&t) == CGXC_K_NULL) t = cgxg_ty(CGT_ANY);
            CgxB nm = {0}; cgxg_name(s->s, &nm); char *ts = cgxg_ts(&t);
            c->top = 1; CgxDoc *d = cgx_dcatn(GD, cgx_dtextf(GD, "var %s %s = ", nm.p, ts), cgxg_exw(c, s->a, &t, 0), NULL);
            d = cgxg_unused(c, d, nm.p, cgxg_isused(c, s, s->s)); free(ts); free(nm.p); return d;
        }
        case CGN_ASSIGN: return cgxg_assign(c, s);
        case CGN_EXPR: { CgxTy nat; int prec; return cgxg_ex0(c, s->a, &prec, &nat); }
        case CGN_THROW: {
            const CgxTy t = cgxg_vty(s->a); CgxTy S = cgxg_ty(CGT_STR), A = cgxg_ty(CGT_ANY);
            CgxDoc *d = cgxg_exw(c, s->a, cgxc_kind(&t) == CGXC_K_STR ? &S : &A, 0);
            if (cgxc_kind(&t) != CGXC_K_STR) d = cgxg_pre(c, "cgStr(", d, ")");
            return cgx_dcatn(GD, cgxg_t(c, "panic(CgErr{"), d, cgxg_t(c, "})"), NULL);
        }
        case CGN_RET: if (s->a && !c->tdepth) c->top = 1; return cgxg_ctl(c, 3, s->a ? cgxg_exw(c, s->a, &c->rett, 0) : NULL);
        case CGN_BREAK: return cgxg_ctl(c, 1, NULL);
        case CGN_CONT: return cgxg_ctl(c, 2, NULL);
        case CGN_IF: {
            CgxDoc *d = cgx_dcatn(GD, cgxg_t(c, "if "), cgxg_exw(c, s->a, &B, 0), cgxg_t(c, " "), cgxg_block(c, &s->b1, NULL, NULL), NULL);
            if (s->b2.n == 1 && s->b2.p[0]->k == CGN_IF && s->b2.p[0]->flag) d = cgx_dcatn(GD, d, cgxg_t(c, " else "), cgxg_stmt(c, s->b2.p[0]), NULL);
            else if (s->b2.n) d = cgx_dcatn(GD, d, cgxg_t(c, " else "), cgxg_block(c, &s->b2, NULL, NULL), NULL);
            return d;
        }
        case CGN_WHILE: {
            CgxDoc *cond = cgxg_exw(c, s->a, &B, 0); c->loop_td[c->nloop++] = c->tdepth;
            CgxDoc *d = cgx_dcatn(GD, cgxg_t(c, "for "), cond, cgxg_t(c, " "), cgxg_block(c, &s->b1, NULL, NULL), NULL); c->nloop--; return d;
        }
        case CGN_FOR: {
            const int id = ++c->uniq; const long long step = s->c ? s->c->iv : 1; CgxB nm = {0}; cgxg_name(s->s, &nm);
            CgxDoc *a = cgxg_exw(c, s->a, &I, 0), *b = cgxg_exw(c, s->b, &I, 0);
            c->loop_td[c->nloop++] = c->tdepth;
            CgxDoc *pro = cgxg_unused(c, cgx_dtextf(GD, "%s := cgi%d", nm.p, id), nm.p, cgxg_isused(c, s, s->s));
            CgxDoc *body = cgxg_block(c, &s->b1, pro, NULL); c->nloop--;
            CgxDoc *d = cgx_dcatn(GD, cgx_dtextf(GD, "for cgi%d, cge%d := int64(", id, id), a, cgxg_t(c, "), int64("), b, cgx_dtextf(GD, "); cgi%d %s cge%d; cgi%d += %lld ", id, step > 0 ? "<" : ">", id, id, step), body, NULL);
            free(nm.p); return d;
        }
        case CGN_EACH: {
            const int id = ++c->uniq; const CgxTy tc = cgxg_vty(s->a); const int kc = cgxc_kind(&tc); CgxB nm = {0}, nm2 = {0}; cgxg_name(s->s, &nm); if (s->s2) cgxg_name(s->s2, &nm2);
            CgxDoc *head, *p1, *p2 = NULL; CgxTy A = cgxg_ty(CGT_ANY), S = cgxg_ty(CGT_STR); char lim[80];
            c->loop_td[c->nloop++] = c->tdepth;
            if (kc == CGXC_K_LIST) {
                head = cgx_dcatn(GD, cgx_dtextf(GD, "cgl%d := ", id), cgxg_exw(c, s->a, &tc, 0), NULL); snprintf(lim, sizeof lim, "cgi%d < cgl%d.Len()", id, id);
                p1 = cgxc_ek(tc.e) == 4 ? cgx_dtextf(GD, "%s := cgl%d.AnyAt(cgi%d)", nm.p, id, id) : cgx_dtextf(GD, "%s := cgl%d.a[cgi%d]", nm.p, id, id);
            } else if (kc == CGXC_K_STR) {
                head = cgx_dcatn(GD, cgx_dtextf(GD, "cgs%d := ", id), cgxg_exw(c, s->a, &S, 0), NULL); snprintf(lim, sizeof lim, "cgi%d < len(cgs%d)", id, id);
                p1 = cgx_dtextf(GD, "%s := cgs%d[cgi%d : cgi%d+1]", nm.p, id, id, id);
            } else if (kc == CGXC_K_MAP) {
                head = cgx_dcatn(GD, cgx_dtextf(GD, "cgm%d := ", id), cgxg_exw(c, s->a, &tc, 0), NULL); snprintf(lim, sizeof lim, "cgi%d < cgm%d.Len()", id, id);
                p1 = cgxc_ek(tc.e) == 4 ? cgx_dtextf(GD, "%s := cgm%d.KeyAt(cgi%d)", nm.p, id, id) : cgx_dtextf(GD, "%s := cgm%d.keys[cgi%d]", nm.p, id, id);
                if (s->s2) p2 = cgxc_ek(tc.e) == 4 ? cgx_dtextf(GD, "%s := cgm%d.AnyValAt(cgi%d)", nm2.p, id, id) : cgx_dtextf(GD, "%s := cgm%d.vals[cgi%d]", nm2.p, id, id);
            } else {
                head = cgx_dcatn(GD, cgx_dtextf(GD, "cgv%d := ", id), cgxg_exw(c, s->a, &A, 0), NULL); snprintf(lim, sizeof lim, "int64(cgi%d) < cgVLen(cgv%d)", id, id);
                p1 = cgx_dtextf(GD, "%s := cgItem(cgv%d, cgi%d)", nm.p, id, id);
                if (s->s2) p2 = cgx_dtextf(GD, "%s := cgItemV(cgv%d, cgi%d)", nm2.p, id, id);
            }
            CgxDoc *pro = cgxg_unused(c, p1, nm.p, cgxg_isused(c, s, s->s));
            if (p2) pro = cgx_dcatn(GD, pro, cgx_dhard(GD), cgxg_unused(c, p2, nm2.p, cgxg_isused(c, s, s->s2)), NULL);
            CgxDoc *body = cgxg_block(c, &s->b1, pro, NULL); c->nloop--;
            CgxDoc *loop = cgx_dcatn(GD, cgx_dtextf(GD, "for cgi%d := 0; %s; cgi%d++ ", id, lim, id), body, NULL);
            free(nm.p); free(nm2.p);
            return cgxg_wrapblock(c, cgx_dcatn(GD, cgx_dhard(GD), head, cgx_dhard(GD), loop, NULL));
        }
        case CGN_TRY: return cgxg_try(c, s);
    }
    cgx_err(c->g, s->line, "go: unexpected statement node %d", s->k);
    return NULL;
}

/* ---- funcoes, classes, testes, programa ---- */
static CgxDoc *cgxg_params(CgxG *c, const CgxNode *fn) {
    CgxDoc *it[44]; int k = 0;
    for (int i = 0; i < fn->npar && k < 43; i++) {
        const CgxParam *p = &fn->par[i]; CgxB nm = {0}; cgxg_name(p->name, &nm); CgxTy pt = p->ty;
        if (cgxc_kind(&pt) == CGXC_K_VOID || cgxc_kind(&pt) == CGXC_K_NULL) pt = cgxg_ty(CGT_ANY);
        char *ts = cgxg_ts(&pt); it[k++] = cgx_dtextf(GD, "%s %s", nm.p, ts); free(ts); free(nm.p);
    }
    return cgxg_seq(c, "(", it, k, ")");
}
static void cgxg_fn_begin(CgxG *c, const CgxNode *fn, CgxTy rt) {
    c->tdepth = 0; c->nloop = 0; c->need_rv = 0; c->nused = 0; c->fn = fn; c->rett = rt; cgxg_flags = 0;
    if (fn) for (int i = 0, m = cgxg_nlive(&fn->b1); i < m; i++) cgxg_scan(c, fn->b1.p[i]);
}
static CgxDoc *cgxg_func(CgxG *c, const CgxNode *fn, const CgxNode *cls) {
    CgxTy rt = fn->ty; cgxg_fn_begin(c, fn, rt);
    CgxB nm = {0}; if (cls) cgxg_mname(cls, fn->s, &nm); else cgxg_name(fn->s, &nm);
    char *rs = cgxg_ts(&rt); CgxB cn = {0}; if (cls) cgxg_name(cls->s, &cn);
    CgxDoc *head = cgx_dcatn(GD, cls ? cgx_dtextf(GD, "func (self *%s) %s", cn.p, nm.p) : cgx_dtextf(GD, "func %s", nm.p), cgxg_params(c, fn), cgx_dtextf(GD, "%s%s {", *rs ? " " : "", rs), NULL);
    CgxDoc *body = cgxg_stmts(c, &fn->b1, NULL), *pre = NULL;
    if (c->need_rv) { char *ts = cgxg_ts(&rt); pre = cgx_dcatn(GD, cgx_dhard(GD), cgx_dtextf(GD, "var cgrv %s", ts), NULL); free(ts); }
    CgxDoc *tail = cgxc_kind(&rt) != CGXC_K_VOID && !cgxg_terminates(&fn->b1) ? cgx_dcatn(GD, cgx_dhard(GD), cgxg_t(c, "panic(\"unreachable\")"), NULL) : NULL;
    CgxDoc *in = pre; if (body) in = in ? cgx_dcat(GD, in, body) : body; if (tail) in = in ? cgx_dcat(GD, in, tail) : tail;
    CgxDoc *out = in ? cgx_dcatn(GD, head, cgx_dnest(GD, 1, in), cgx_dhard(GD), cgxg_t(c, "}"), NULL) : cgx_dcatn(GD, head, cgx_dhard(GD), cgxg_t(c, "}"), NULL);
    free(nm.p); free(rs); free(cn.p); return out;
}
static CgxDoc *cgxg_default(CgxG *c, const CgxNode *f) {
    CgxTy ft = f->ty; if (cgxc_kind(&ft) == CGXC_K_VOID) ft = cgxg_ty(CGT_ANY);
    if (f->a) return cgxg_exw(c, f->a, &ft, 0);
    if (cgxc_kind(&ft) == CGXC_K_LIST) { CgxTy ft2 = ft; return cgxg_list(c, &(CgxNode){.k = CGN_LIST, .ty = ft2}, &ft2); }
    if (cgxc_kind(&ft) == CGXC_K_MAP) { CgxTy ft2 = ft; return cgxg_map(c, &(CgxNode){.k = CGN_MAP, .ty = ft2}, &ft2); }
    return cgxg_t(c, cgxg_zero(&ft));
}
static CgxDoc *cgxg_class_def(CgxG *c, const CgxNode *cls) {
    CgxB cn = {0}; cgxg_name(cls->s, &cn); CgxDoc *fields = NULL; const CgxNode *init = NULL; size_t wmax = 0;
    for (int i = 0; i < cls->b1.n; i++) if (cls->b1.p[i]->k == CGN_FIELDDECL) { CgxB fnm = {0}; cgxg_name(cls->b1.p[i]->s, &fnm); if (fnm.n > wmax) wmax = fnm.n; free(fnm.p); }
    for (int i = 0; i < cls->b1.n; i++) {                       /* gofmt alinha os tipos dos campos */
        const CgxNode *f = cls->b1.p[i];
        if (f->k == CGN_FN && !strcmp(f->s, "init")) init = f;
        if (f->k != CGN_FIELDDECL) continue;
        CgxTy ft = f->ty; if (cgxc_kind(&ft) == CGXC_K_VOID) ft = cgxg_ty(CGT_ANY);
        CgxB fnm = {0}; cgxg_name(f->s, &fnm); char *ts = cgxg_ts(&ft);
        CgxDoc *x = cgx_dcatn(GD, cgx_dhard(GD), cgx_dtextf(GD, "%-*s %s", (int)wmax, fnm.p, ts), NULL); fields = fields ? cgx_dcat(GD, fields, x) : x; free(ts); free(fnm.p);
    }
    CgxDoc *d = cgx_dcatn(GD, cgx_dtextf(GD, "type %s struct {", cn.p), fields ? cgx_dnest(GD, 1, fields) : cgx_dnil(GD), cgx_dhard(GD), cgxg_t(c, "}"), NULL);
    /* construtor: campos com o valor padrao, depois init */
    cgxg_fn_begin(c, NULL, cgxg_ty(CGT_VOID));
    CgxDoc *params = init ? cgxg_params(c, init) : cgxg_t(c, "()");
    CgxDoc *body = cgx_dcatn(GD, cgx_dhard(GD), cgx_dtextf(GD, "self := &%s{}", cn.p), NULL);
    for (int i = 0; i < cls->b1.n; i++) {
        const CgxNode *f = cls->b1.p[i]; if (f->k != CGN_FIELDDECL) continue;
        CgxB fnm = {0}; cgxg_name(f->s, &fnm);
        body = cgx_dcatn(GD, body, cgx_dhard(GD), cgx_dtextf(GD, "self.%s = ", fnm.p), cgxg_default(c, f), NULL); free(fnm.p);
    }
    if (init) {
        CgxB args = {0}; for (int i = 0; i < init->npar; i++) { cgx_bs(&args, i ? ", " : ""); cgxg_name(init->par[i].name, &args); }
        CgxB im = {0}; cgxg_mname(cls, "init", &im);
        body = cgx_dcatn(GD, body, cgx_dhard(GD), cgx_dtextf(GD, "self.%s(%s)", im.p, args.p ? args.p : ""), NULL); free(args.p); free(im.p);
    }
    body = cgx_dcatn(GD, body, cgx_dhard(GD), cgxg_t(c, "return self"), NULL);
    d = cgx_dcatn(GD, d, cgx_dhard(GD), cgx_dhard(GD), cgx_dtextf(GD, "func new%s", cn.p), params, cgx_dtextf(GD, " *%s {", cn.p), cgx_dnest(GD, 1, body), cgx_dhard(GD), cgxg_t(c, "}"), NULL);
    for (int j = 0; j < cls->b1.n; j++) if (cls->b1.p[j]->k == CGN_FN) d = cgx_dcatn(GD, d, cgx_dhard(GD), cgx_dhard(GD), cgxg_func(c, cls->b1.p[j], cls), NULL);
    free(cn.p); return d;
}
static CgxDoc *cgxg_test(CgxG *c, const CgxNode *t, int idx) {
    const CgxTy A = cgxg_ty(CGT_ANY); const int mode = (int)t->iv; cgxg_fn_begin(c, NULL, cgxg_ty(CGT_VOID));
    CgxDoc *inner;
    if (mode == 2) {
        int prec; CgxTy nat; CgxDoc *e = cgxg_ex0(c, t->a, &prec, &nat);
        inner = cgx_dcatn(GD, cgx_dhard(GD), cgxc_kind(&nat) == CGXC_K_VOID ? e : cgx_dcat(GD, cgxg_t(c, "_ = "), e), NULL);
    } else {
        CgxDoc *g = cgxg_exw(c, t->a, &A, 0), *x = t->b ? cgxg_exw(c, t->b, &A, 0) : cgxg_t(c, "nil");
        const char *chk = mode == 1 ? "math.Abs(cgAs[float64](cgg)-cgAs[float64](cge)) < 1e-9" : mode == 3 ? "cgg == true" : "cgEq(cgg, cge)";
        inner = cgx_dcatn(GD, cgx_dhard(GD), cgxg_t(c, "var cgg any = "), g, cgx_dhard(GD), cgxg_t(c, "var cge any = "), x, cgx_dhard(GD), cgx_dtextf(GD, "if %s {", chk),
            cgx_dnest(GD, 1, cgx_dcatn(GD, cgx_dhard(GD), cgx_dtextf(GD, "fmt.Println(\"test %d: ok\")", idx), NULL)), cgx_dhard(GD), cgxg_t(c, "} else {"),
            cgx_dnest(GD, 1, cgx_dcatn(GD, cgx_dhard(GD), cgx_dtextf(GD, "cgTestFail(%d, cgg, cge)", idx), NULL)), cgx_dhard(GD), cgxg_t(c, "}"), NULL);
    }
    return cgx_dcatn(GD, cgx_dtextf(GD, "func cgTest%d() {", idx), cgx_dnest(GD, 1, cgx_dcatn(GD, cgx_dhard(GD), cgx_dtextf(GD, "cgTestRun(%d, func() {", idx), cgx_dnest(GD, 1, inner), cgx_dhard(GD), cgx_dtextf(GD, "}, %s)", mode == 2 ? "true" : "false"), NULL)), cgx_dhard(GD), cgxg_t(c, "}"), NULL);
}
static char *cgxg_render_str(CgxG *c, CgxDoc *d, int width) { CgxB b = {0}; cgx_render(d, width, &b); return b.p ? b.p : strdup(""); }
/* texto dos ajudantes de despacho dinamico (receptor any): um type switch sobre as classes */
static void cgxg_dynemit(CgxG *c, CgxB *out) {
    for (int i = 0; i < c->ndyn; i++) {
        const char *nm = c->dyn[i].name; const int kind = c->dyn[i].kind, k = c->dyn[i].n; char fn[96]; int found = 0;
        snprintf(fn, sizeof fn, "cgd%c_%s_%d", kind == 0 ? 'm' : kind == 1 ? 'f' : 's', nm, k);
        cgx_bf(out, "func %s(r any", fn);
        if (kind == 0) for (int j = 0; j < k; j++) cgx_bf(out, ", a%d any", j);
        if (kind == 2) cgx_bs(out, ", v any");
        cgx_bf(out, ")%s {\n\tswitch o := r.(type) {\n", kind == 2 ? "" : " any");
        for (int ci = 0; ci < c->ncls; ci++) {
            const CgxNode *cl = c->cls_list[ci]; CgxB cn = {0}; cgxg_name(cl->s, &cn);
            if (kind == 0) {
                const CgxNode *m = NULL; for (int j = 0; j < cl->b1.n; j++) if (cl->b1.p[j]->k == CGN_FN && !strcmp(cl->b1.p[j]->s, nm) && strcmp(nm, "init")) m = cl->b1.p[j];
                if (!m || k > m->npar) { free(cn.p); continue; }
                int req = 0; for (int j = 0; j < m->npar; j++) if (!m->par[j].def) req = j + 1;
                if (k < req) { free(cn.p); continue; }
                found = 1; CgxB call = {0}, mn = {0}; cgxg_mname(cl, nm, &mn); cgx_bf(&call, "o.%s(", mn.p);
                cgxg_fn_begin(c, NULL, cgxg_ty(CGT_VOID));
                for (int j = 0; j < m->npar; j++) {
                    char an[16]; snprintf(an, sizeof an, "a%d", j); CgxTy pt = m->par[j].ty; char *t;
                    if (j < k) t = cgxc_kind(&pt) == CGXC_K_ANY ? strdup(an) : cgxg_render_str(c, cgxg_unbox(c, cgx_dtext(GD, an), &pt), 100000);
                    else t = cgxg_render_str(c, cgxg_exw(c, m->par[j].def, &pt, 0), 100000);
                    cgx_bf(&call, "%s%s", j ? ", " : "", t); free(t);
                }
                cgx_bs(&call, ")"); CgxTy rt = m->ty;
                cgx_bf(out, "\tcase *%s:\n", cn.p);
                if (cgxc_kind(&rt) == CGXC_K_VOID) cgx_bf(out, "\t\t%s\n\t\treturn nil\n", call.p);
                else { char *bx = cgxg_render_str(c, cgxg_box(c, cgx_dtext(GD, call.p), &rt), 100000); cgx_bf(out, "\t\treturn %s\n", bx); free(bx); }
                free(call.p); free(mn.p);
            } else {
                const CgxNode *f = NULL; for (int j = 0; j < cl->b1.n; j++) if (cl->b1.p[j]->k == CGN_FIELDDECL && !strcmp(cl->b1.p[j]->s, nm)) f = cl->b1.p[j];
                if (!f) { free(cn.p); continue; }
                found = 1; CgxB fnm = {0}; cgxg_name(nm, &fnm); CgxTy ft = f->ty; cgxg_fn_begin(c, NULL, cgxg_ty(CGT_VOID));
                cgx_bf(out, "\tcase *%s:\n", cn.p);
                if (kind == 1) { CgxB acc = {0}; cgx_bf(&acc, "o.%s", fnm.p); char *bx = cgxg_render_str(c, cgxg_box(c, cgx_dtext(GD, acc.p), &ft), 100000); cgx_bf(out, "\t\treturn %s\n", bx); free(bx); free(acc.p); }
                else { char *t = cgxg_render_str(c, cgxg_unbox(c, cgx_dtext(GD, "v"), &ft), 100000); cgx_bf(out, "\t\to.%s = %s\n\t\treturn\n", fnm.p, t); free(t); }
                free(fnm.p);
            }
            free(cn.p);
        }
        cgx_bf(out, "\t}\n\tpanic(CgErr{\"no member `%s` on this value\"})\n}\n\n", nm);
        if (!found) cgx_err(c->g, 0, "go: no class has a %s named `%s`", kind == 0 ? "method" : "field", nm);
    }
}
/* indentacao por tabulacao (gofmt): os niveis foram escritos como um espaco */
static void cgxg_tabs(const char *in, CgxB *out) {
    const char *p = in;
    while (*p) {
        const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p), sp = 0;
        while (sp < l && p[sp] == ' ') sp++;
        for (size_t i = 0; i < sp; i++) cgx_bs(out, "\t");
        cgx_bn(out, p + sp, l - sp);
        if (e) cgx_bs(out, "\n");
        p += l + (e ? 1 : 0);
    }
}
static int cgx_gen_go(Cgx *g, const CgxGenOpts *o, CgxB *code, CgxB *tests) {
    CgxG C; memset(&C, 0, sizeof C); C.g = g; C.o = o; CgxG *c = &C;
    const CgxNode *prog = g->prog; const int err0 = g->nerr_all;
    CgxB globals = {0}, defs = {0}, tb = {0}; int nt = 0; CgxDoc *initdoc = NULL;
    for (int i = 0; i < prog->b1.n; i++) if (prog->b1.p[i]->k == CGN_CLASS) { if (c->ncls >= 40) { cgx_err(g, prog->b1.p[i]->line, "go: too many classes (40 at most)"); break; } c->cls_list[c->ncls++] = prog->b1.p[i]; }
    int width = o->width;
    for (int i = 0; i < prog->b1.n; i++) {
        const CgxNode *d = prog->b1.p[i]; CgxDoc *x = NULL;
        if (d->k == CGN_FN) x = cgxg_func(c, d, NULL);
        else if (d->k == CGN_CLASS) x = cgxg_class_def(c, d);
        else if (d->k == CGN_LET) {
            CgxTy t = d->ty; if (cgxc_kind(&t) == CGXC_K_VOID || cgxc_kind(&t) == CGXC_K_NULL) t = cgxg_ty(CGT_ANY);
            CgxB nm = {0}; cgxg_name(d->s, &nm); char *ts = cgxg_ts(&t); cgx_bf(&globals, "var %s %s\n", nm.p, ts); free(ts);
            cgxg_fn_begin(c, NULL, cgxg_ty(CGT_VOID));
            CgxDoc *v = cgx_dcatn(GD, cgx_dhard(GD), cgx_dtextf(GD, "%s = ", nm.p), cgxg_exw(c, d->a, &t, 0), NULL);
            initdoc = initdoc ? cgx_dcat(GD, initdoc, v) : v; free(nm.p);
        } else if (d->k == CGN_RAW) { cgx_err(g, d->line, "go: `raw` blocks have no Go version (the raw languages are python, js, ts and c)"); }
        else if (d->k == CGN_COMMENT && o->comments && d->defs) x = cgx_dtextf(GD, "// %s", d->defs);
        if (x) { char *s = cgxg_render_str(c, x, width); cgx_bs(&defs, s); cgx_bs(&defs, "\n\n"); free(s); }
    }
    cgxg_dynemit(c, &defs);
    if (tests) {
        for (int i = 0; i < prog->b1.n; i++) if (prog->b1.p[i]->k == CGN_TEST) { char *s = cgxg_render_str(c, cgxg_test(c, prog->b1.p[i], ++nt), width); cgx_bs(&tb, s); cgx_bs(&tb, "\n\n"); free(s); }
        cgx_bs(&tb, "func main() {\n\tcgInit()\n");
        for (int i = 1; i <= nt; i++) cgx_bf(&tb, "\tcgTest%d()\n", i);
        cgx_bs(&tb, "}\n");
    } else cgx_bs(&defs, "func main() {\n\tcgInit()\n}\n");
    if (g->nerr_all == err0) {
        CgxB all = {0};
        cgx_bs(&all, "// Generated by codegen (Go backend). Build: go run file.go\npackage main\n\nimport (\n\t\"fmt\"\n\t\"math\"\n\t\"os\"\n\t\"regexp\"\n\t\"sort\"\n\t\"strconv\"\n\t\"strings\"\n)\n\n");
        for (int i = 0; CGX_GO_RT[i]; i++) { cgx_bs(&all, CGX_GO_RT[i]); cgx_bs(&all, "\n"); }
        cgx_bs(&all, "\n");
        if (globals.p) { cgx_bs(&all, globals.p); cgx_bs(&all, "\n"); }
        if (defs.p) cgx_bs(&all, defs.p);
        { CgxDoc *fi = initdoc ? cgx_dcatn(GD, cgxg_t(c, "func cgInit() {"), cgx_dnest(GD, 1, initdoc), cgx_dhard(GD), cgxg_t(c, "}"), NULL) : cgxg_t(c, "func cgInit() {\n}"); char *z = cgxg_render_str(c, fi, width); cgx_bs(&all, z); cgx_bs(&all, "\n"); free(z); }
        { CgxB t = {0}; cgxg_tabs(all.p, &t); cgx_bs(code, t.p); free(t.p); }
        if (tests && tb.p) { CgxB t = {0}; cgxg_tabs(tb.p, &t); cgx_bs(tests, t.p); free(t.p); }
        free(all.p);
    }
    free(globals.p); free(defs.p); free(tb.p); free(c->used); cgx_docs_free(&c->D);
    return g->nerr_all == err0;
}
