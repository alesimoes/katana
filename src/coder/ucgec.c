/* ---------- coder/ucgec.c: UCGE, backend C (docs/ucge.md, "Backend C") ----------
 * Percorre a AST ja validada e tipada e monta um documento (ucgedoc.c), como o backend Python. O que muda: C nao tem listas, mapas,
 * strings com tamanho nem excecoes; tudo isso vem de um runtime pequeno (tools/ucge/rt/cgrt.h, embutido em ucgec_rt.inc) que o gerador
 * copia para o inicio do arquivo. O codigo gerado e C99, sem avisos em `gcc -std=c99 -Wall -Wextra -Wpedantic` e limpo sob ASan e UBSan.
 * Decisoes (detalhes em docs/ucge.md):
 *   int = long long com estouro verificado; float = double; bool = int; str = {p, n} (bytes; nula tem p == NULL);
 *   list = CgList * (vetor + ek, o tipo do elemento: int, float, str, bool ou any); map = CgMap * (hash na ordem de insercao);
 *   classe = struct * (referencia, como no Python); any = CgV (valor etiquetado); Error (o `e` do catch) = a mensagem (CgStr);
 *   try/catch/finally = setjmp/longjmp sobre uma pilha de manipuladores; return/break/continue dentro de try soltam o manipulador e
 *   rodam o finally; tudo sai de uma arena liberada de uma vez (cg_arena_free); a ordem de avaliacao e a da esquerda para a direita
 *   (operandos com efeito vao para temporarios sequenciados por virgula); funcoes com try declaram tudo `volatile`. */
#include <limits.h>
#include "ucgec_rt.inc"

enum { CK_VOID, CK_INT, CK_FLT, CK_BOOL, CK_STR, CK_LIST, CK_MAP, CK_OBJ, CK_ANY, CK_NULL };
typedef struct { int active; const CgxBlk *fin; } CgxCReg;
typedef struct {
    Cgx *g; const CgxGenOpts *o; CgxDocs D;
    int need_re;
    const CgxNode *prog, *fn, *cls;
    char **tmpty; int ntmp, captmp;                         /* temporarios do corpo corrente (cgt1, cgt2, ...): tipo C de cada um */
    int uniq, vol;
    CgxCReg reg[24]; int nreg, lbase[24], nloop;            /* regioes try ativas e onde cada laco comecou */
    struct { const void *ref; const char *name; } *used; int nused, capused;
    const CgxNode *cls_list[40]; int ncls; int line;
    struct { char name[48]; int kind, n; } dyn[96]; int ndyn;      /* ajudantes de despacho dinamico (receptor any): kind 0 metodo, 1 ler campo, 2 gravar campo */
} CgxC;
#define CD (&c->D)
static CgxDoc *cc_t(CgxC *c, const char *s) { return cgx_dtext(CD, s); }

/* ---- nomes: tudo que e do usuario ganha prefixo (u_ funcao e constante, C_ classe, N_ construtor, M_ metodo); locais so mudam se colidem ---- */
static const char *const CC_RESERVED[] = {"auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int",
    "long", "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "NULL", "setjmp", "longjmp",
    "EOF", "errno", "stdin", "stdout", "stderr", "assert", "main", "bool", "true", "false", "complex", "imaginary", "FILE", "size_t", NULL};
static void cc_name(const char *n, CgxB *o) {
    int bad = n[0] == '_' || !strncmp(n, "cg", 2) || !strncmp(n, "CG", 2) || !strncmp(n, "Cg", 2) || !strncmp(n, "u_", 2) || !strncmp(n, "C_", 2) || !strncmp(n, "M_", 2) || !strncmp(n, "N_", 2);
    for (int i = 0; !bad && CC_RESERVED[i]; i++) if (!strcmp(n, CC_RESERVED[i])) bad = 1;
    cgx_bs(o, n); if (bad) cgx_bs(o, "_");
}

/* ---- tipos ---- */
static int cc_kind(const CgxTy *t) {
    switch (t->k) {
        case CGT_INT: return CK_INT; case CGT_FLOAT: return CK_FLT; case CGT_BOOL: return CK_BOOL; case CGT_STR: return CK_STR; case CGT_VOID: return CK_VOID;
        case CGT_LIST: return CK_LIST; case CGT_MAP: return CK_MAP; case CGT_NULL: return CK_NULL;
        case CGT_CLASS: return !strcmp(t->cls, "Error") ? CK_STR : CK_OBJ;
    }
    return CK_ANY;
}
static int cc_clsid(const CgxC *c, const char *name) { for (int i = 0; i < c->ncls; i++) if (!strcmp(c->cls_list[i]->s, name)) return i + 1; return 0; }
static int cc_ek(int e) { return e == CGT_INT ? 0 : e == CGT_FLOAT ? 1 : e == CGT_STR ? 2 : e == CGT_BOOL ? 3 : 4; }
static const char *cc_ekname(int e) { static const char *const n[] = {"CG_EK_INT", "CG_EK_FLT", "CG_EK_STR", "CG_EK_BOOL", "CG_EK_ANY"}; return n[cc_ek(e)]; }
static const char *cc_pfx(int e) { static const char *const n[] = {"i_", "f_", "s_", "b_", "v_"}; return n[cc_ek(e)]; }
static CgxTy cc_elemty(const CgxTy *t) { return cgx_t(t->e); }
static CgxTy cc_listty(int e) { CgxTy t = cgx_t(CGT_LIST); t.e = e; return t; }
static void cc_type(const CgxTy *t, CgxB *o) {
    switch (cc_kind(t)) {
        case CK_INT: cgx_bs(o, "long long"); break; case CK_FLT: cgx_bs(o, "double"); break; case CK_BOOL: cgx_bs(o, "int"); break; case CK_STR: cgx_bs(o, "CgStr"); break;
        case CK_VOID: cgx_bs(o, "void"); break; case CK_LIST: cgx_bs(o, "CgList *"); break; case CK_MAP: cgx_bs(o, "CgMap *"); break;
        case CK_OBJ: cgx_bf(o, "C_%s *", t->cls); break; default: cgx_bs(o, "CgV");
    }
}
/* `T nome`, com volatile quando a funcao tem try (o qualificador vai depois do `*` nos ponteiros) */
static char *cc_declt(const char *ctype, const char *name, int vol) {
    CgxB b = {0}; const size_t l = strlen(ctype); const int ptr = ctype[l - 1] == '*';
    if (vol && !ptr) cgx_bs(&b, "volatile ");
    cgx_bs(&b, ctype); if (!ptr) cgx_bs(&b, " "); if (vol && ptr) cgx_bs(&b, "volatile "); cgx_bs(&b, name); return b.p;
}
static char *cc_decl(const CgxTy *t, const char *name, int vol) { CgxB ty = {0}; cc_type(t, &ty); char *r = cc_declt(ty.p, name, vol); free(ty.p); return r; }
/* tipo de uma variavel: o da declaracao (o let pode ter sido refinado depois do no) */
static CgxTy cc_vty(const CgxNode *n) { if (n->k == CGN_VAR && n->ref && n->ref->k == CGN_LET) return n->ref->ty; return n->ty; }

/* ---- literais ---- */
static void cc_cstr(const char *s, CgxB *o) {
    cgx_bs(o, "\"");
    for (; *s; s++) {
        const unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { cgx_bn(o, "\\", 1); cgx_bn(o, s, 1); }
        else if (ch == '?') cgx_bs(o, "\\?");                  /* nada de trigrafos */
        else if (ch == '\n') cgx_bs(o, "\\n"); else if (ch == '\t') cgx_bs(o, "\\t"); else if (ch == '\r') cgx_bs(o, "\\r");
        else if (ch < 32 || ch >= 127) cgx_bf(o, "\\%03o", ch);
        else cgx_bn(o, s, 1);
    }
    cgx_bs(o, "\"");
}
static CgxDoc *cc_lit_str(CgxC *c, const char *s) { CgxB b = {0}; cgx_bs(&b, "CG_S("); cc_cstr(s, &b); cgx_bs(&b, ")"); CgxDoc *d = cgx_dtext(CD, b.p); free(b.p); return d; }

/* ---- conversoes entre tipos estaticos ---- */
static CgxDoc *cc_pre(CgxC *c, const char *pre, CgxDoc *d, const char *post) { return cgx_dcatn(CD, cgx_dtext(CD, pre), d, cgx_dtext(CD, post), NULL); }
static CgxDoc *cc_box(CgxC *c, CgxDoc *d, const CgxTy *t) {
    switch (cc_kind(t)) {
        case CK_INT: return cc_pre(c, "cg_vint(", d, ")"); case CK_FLT: return cc_pre(c, "cg_vflt(", d, ")"); case CK_BOOL: return cc_pre(c, "cg_vbool(", d, ")");
        case CK_STR: return cc_pre(c, "cg_vstrv(", d, ")"); case CK_LIST: return cc_pre(c, "cg_vlist(", d, ")"); case CK_MAP: return cc_pre(c, "cg_vmap(", d, ")");
        case CK_OBJ: return cgx_dcatn(CD, cgx_dtext(CD, "cg_vobj((void *)"), d, cgx_dtextf(CD, ", %d)", cc_clsid(c, t->cls)), NULL);
        case CK_NULL: return cgx_dtext(CD, "cg_nil()");
    }
    return d;
}
static CgxDoc *cc_unbox(CgxC *c, CgxDoc *d, const CgxTy *t) {
    switch (cc_kind(t)) {
        case CK_INT: return cc_pre(c, "cg_ui(", d, ")"); case CK_FLT: return cc_pre(c, "cg_uf(", d, ")"); case CK_BOOL: return cc_pre(c, "cg_ub(", d, ")");
        case CK_STR: return cc_pre(c, "cg_us(", d, ")");
        case CK_LIST: return cgx_dcatn(CD, cgx_dtext(CD, "cg_ul("), d, cgx_dtextf(CD, ", %s)", cc_ekname(t->e)), NULL);
        case CK_MAP: return cgx_dcatn(CD, cgx_dtext(CD, "cg_um("), d, cgx_dtextf(CD, ", %s)", cc_ekname(t->e)), NULL);
        case CK_OBJ: return cgx_dcatn(CD, cgx_dtextf(CD, "(C_%s *)cg_uo(", t->cls), d, cgx_dtextf(CD, ", %d)", cc_clsid(c, t->cls)), NULL);
    }
    return d;
}
/* d tem o tipo `from`; devolve o mesmo valor com o tipo `to` */
static CgxDoc *cc_conv(CgxC *c, CgxDoc *d, const CgxTy *from, const CgxTy *to) {
    const int kf = cc_kind(from), kt = cc_kind(to);
    if (kt == CK_VOID || kf == CK_VOID) return d;
    if (kf == CK_NULL && (kt == CK_INT || kt == CK_FLT || kt == CK_BOOL)) { cgx_err(c->g, c->line, "c: null where a number or bool is expected (declare the type as `any`, or annotate `-> any`)"); return d; }
    if (kf == CK_NULL) return kt == CK_STR ? cgx_dtext(CD, "CG_NULLSTR") : kt == CK_ANY ? cgx_dtext(CD, "cg_nil()") : cgx_dtext(CD, "NULL");
    if (kt == CK_ANY) return kf == CK_ANY ? d : cc_box(c, d, from);
    if (kf == CK_ANY) return cc_unbox(c, d, to);
    if (kf == CK_INT && kt == CK_FLT) return cc_pre(c, "(double)", d, "");
    if (kf == CK_LIST && kt == CK_LIST && from->e != to->e && to->e != CGT_ANY)
        return cgx_dcatn(CD, cgx_dtext(CD, from->e == CGT_ANY ? "cg_las(" : "cg_lcvt("), d, cgx_dtextf(CD, ", %s)", cc_ekname(to->e)), NULL);
    if (kf == CK_MAP && kt == CK_MAP && from->e != to->e && to->e != CGT_ANY)
        return cgx_dcatn(CD, cgx_dtext(CD, from->e == CGT_ANY ? "cg_mas(" : "cg_mcvt("), d, cgx_dtextf(CD, ", %s)", cc_ekname(to->e)), NULL);
    return d;
}

/* ---- ordem de avaliacao: o que uma expressao pode fazer (efeito, leitura do monte, erro) ---- */
enum { CB_EFF = 1, CB_RD = 2, CB_THR = 4 };
static int cc_bits(const CgxNode *n) {
    int b = 0;
    if (!n) return 0;
    switch (n->k) {
        case CGN_INT: case CGN_FLT: case CGN_STR: case CGN_BOOL: case CGN_NULL: case CGN_VAR: return 0;
        case CGN_INDEX: case CGN_FIELD: b = CB_RD | CB_THR; break;
        case CGN_CALL:
            if (!n->bi || n->bi == CGB_PUSH || n->bi == CGB_POP || n->bi == CGB_PRINT) b = CB_EFF | CB_RD | CB_THR;
            else b = CB_THR | (n->b1.n && n->b1.p[0]->ty.k != CGT_STR && n->b1.p[0]->ty.k != CGT_INT && n->b1.p[0]->ty.k != CGT_FLOAT ? CB_RD : 0);
            break;
        case CGN_MCALL: b = CB_EFF | CB_RD | CB_THR; break;
        case CGN_BIN: b = CB_THR; break;
        default: break;
    }
    b |= cc_bits(n->a) | cc_bits(n->b) | cc_bits(n->c);
    for (int i = 0; i < n->b1.n; i++) b |= cc_bits(n->b1.p[i]);
    return b;
}

static CgxDoc *cc_ex0(CgxC *c, const CgxNode *n, int *prec, CgxTy *nat);
static CgxDoc *cc_exw(CgxC *c, const CgxNode *n, const CgxTy *want, int minprec);
static const int CC_PCMP = 5;
static CgxDoc *cc_wrap(CgxC *c, CgxDoc *d, int prec, int minprec) { return prec < minprec ? cgx_dcatn(CD, cc_t(c, "("), d, cc_t(c, ")"), NULL) : d; }
static CgxDoc *cc_seq(CgxC *c, const char *open, CgxDoc *const *items, int n, const char *close) {
    if (!n) return cgx_dtextf(CD, "%s%s", open, close);
    CgxDoc *in = NULL;
    for (int i = 0; i < n; i++) { CgxDoc *x = i ? cgx_dcatn(CD, cc_t(c, ","), cgx_dline(CD), items[i], NULL) : items[i]; in = in ? cgx_dcat(CD, in, x) : x; }
    return cgx_dgroup(CD, cgx_dcatn(CD, cc_t(c, open), cgx_dnest(CD, c->o->indent, cgx_dcat(CD, cgx_dsoft(CD), in)), cgx_dsoft(CD), cc_t(c, close), NULL));
}
/* um temporario do corpo corrente, com o tipo C do valor dado */
static int cc_newtmp(CgxC *c, const CgxTy *t) {
    CgxB b = {0}; cc_type(t, &b);
    if (c->ntmp == c->captmp) { c->captmp = c->captmp ? c->captmp * 2 : 8; c->tmpty = (char **)realloc(c->tmpty, sizeof(char *) * (size_t)c->captmp); }
    c->tmpty[c->ntmp++] = b.p; return c->ntmp;
}
/* gera n operandos ja convertidos para `want`; se a ordem importa (um deles tem efeito e outro mexe no estado), todos menos o ultimo
 * que importa vao para temporarios: *pre recebe `cgt1 = a, cgt2 = b` e out[i] passa a ser o nome do temporario */
static void cc_ops(CgxC *c, const CgxNode *const *ns, const CgxTy *want, int n, int minp, CgxDoc **out, CgxDoc **pre) {
    int bits[64], eff = 0, nz = 0, last = -1; *pre = NULL;
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) { bits[i] = cc_bits(ns[i]); if (bits[i] & CB_EFF) eff = 1; if (bits[i]) { nz++; last = i; } }
    for (int i = 0; i < n; i++) out[i] = cc_exw(c, ns[i], &want[i], minp);
    if (eff && nz >= 2) for (int i = 0; i < last; i++) if (bits[i] && cc_kind(&want[i]) != CK_VOID) {
        const int t = cc_newtmp(c, &want[i]);
        CgxDoc *as = cgx_dcatn(CD, cgx_dtextf(CD, "cgt%d = ", t), out[i], NULL);
        *pre = *pre ? cgx_dcatn(CD, *pre, cc_t(c, ", "), as, NULL) : as;
        out[i] = cgx_dtextf(CD, "cgt%d", t);
    }
}
static CgxDoc *cc_withpre(CgxC *c, CgxDoc *pre, CgxDoc *e, int *prec) {
    if (!pre) return e;
    *prec = 11; return cgx_dcatn(CD, cc_t(c, "("), pre, cc_t(c, ", "), e, cc_t(c, ")"), NULL);
}
/* f(a, b, ...) com os operandos ja sequenciados */
static CgxDoc *cc_callN(CgxC *c, const char *fname, const CgxNode *const *ns, const CgxTy *want, int n, int *prec) {
    CgxDoc *o[64], *pre; cc_ops(c, ns, want, n, 0, o, &pre);
    *prec = 10; CgxDoc *call = cgx_dcat(CD, cgx_dtext(CD, fname), cc_seq(c, "(", o, n, ")"));
    return cc_withpre(c, pre, call, prec);
}
/* a OP b com operador nativo, operandos sequenciados */
static CgxDoc *cc_infix(CgxC *c, const char *sym, const CgxNode *a, const CgxNode *b, const CgxTy *wa, const CgxTy *wb, int pr, int *prec) {
    const CgxNode *ns[2] = {a, b}; CgxTy w[2] = {*wa, *wb}; CgxDoc *o[2], *pre; cc_ops(c, ns, w, 2, pr <= 3 ? 4 : pr + 1, o, &pre);          /* && dentro de || sempre com parenteses (-Wparentheses) */
    *prec = pr;
    CgxDoc *e = cgx_dcatn(CD, o[0], cgx_dtextf(CD, " %s ", sym), o[1], NULL);
    return cc_withpre(c, pre, e, prec);
}

/* ---- listas e mapas literais: o ek vem do tipo esperado, se ele diz, senao do tipo do proprio literal ---- */
static CgxDoc *cc_list(CgxC *c, const CgxNode *n, const CgxTy *want) {
    CgxTy lt = cc_kind(want) == CK_LIST ? *want : n->ty; int prec = 0; CgxDoc *r;
    if (lt.k != CGT_LIST) lt = cc_listty(CGT_ANY);
    const CgxTy el = cc_elemty(&lt); const int k = n->b1.n;
    if (!k) r = cgx_dtextf(CD, "cg_lnew(%s)", cc_ekname(lt.e));
    else {
        CgxTy w[64] = {{0}}; const CgxNode *ns[64] = {0}; CgxDoc *o[64], *pre; int m = k > 64 ? 64 : k;
        for (int i = 0; i < m; i++) { w[i] = el; ns[i] = n->b1.p[i]; }
        cc_ops(c, ns, w, m, 0, o, &pre);
        const int e = cc_ek(lt.e);
        static const char *const ct[] = {"long long", "double", "CgStr", "int", "CgV"};
        CgxDoc *arr = cc_seq(c, "{", o, m, "}");
        r = cgx_dcatn(CD, cgx_dtextf(CD, "cg_l%sfrom(%d, (%s[])", cc_pfx(lt.e), m, ct[e]), arr, cc_t(c, ")"), NULL);
        r = cc_withpre(c, pre, r, &prec);
    }
    if (cc_kind(want) == CK_ANY) r = cc_box(c, r, &lt);
    return r;
}
static CgxDoc *cc_map(CgxC *c, const CgxNode *n, const CgxTy *want) {
    CgxTy mt = cc_kind(want) == CK_MAP ? *want : n->ty; int prec = 0; CgxDoc *r;
    if (mt.k != CGT_MAP) mt = cgx_t(CGT_MAP);
    const int k = n->b1.n / 2;
    if (!k) r = cgx_dtextf(CD, "cg_mnew(%s)", cc_ekname(mt.e));
    else {
        CgxTy w[128] = {{0}}; const CgxNode *ns[128] = {0}; CgxDoc *o[128], *pre, *ks[64], *vs[64]; int m = k > 64 ? 64 : k;
        for (int i = 0; i < m; i++) { ns[2 * i] = n->b1.p[2 * i]; w[2 * i] = cgx_t(CGT_STR); ns[2 * i + 1] = n->b1.p[2 * i + 1]; w[2 * i + 1] = cgx_t(CGT_ANY); }
        cc_ops(c, ns, w, 2 * m, 0, o, &pre);
        for (int i = 0; i < m; i++) { ks[i] = o[2 * i]; vs[i] = o[2 * i + 1]; }
        r = cgx_dcatn(CD, cgx_dtextf(CD, "cg_mfromv(%s, %d, (CgStr[])", cc_ekname(mt.e), m), cc_seq(c, "{", ks, m, "}"), cc_t(c, ", (CgV[])"), cc_seq(c, "{", vs, m, "}"), cc_t(c, ")"), NULL);
        r = cc_withpre(c, pre, r, &prec);
    }
    if (cc_kind(want) == CK_ANY) r = cc_box(c, r, &mt);
    return r;
}

/* ---- funcoes embutidas ---- */
static CgxTy cc_ty(int k) { return cgx_t(k); }
static CgxDoc *cc_cmp0(CgxC *c, CgxDoc *call, int *prec, const char *tail) {       /* call OP 0, com a precedencia de comparacao */
    CgxDoc *d = *prec < 10 ? cc_wrap(c, call, *prec, 11) : call; *prec = CC_PCMP; return cgx_dcatn(CD, d, cc_t(c, tail), NULL);
}
static CgxDoc *cc_builtin(CgxC *c, const CgxNode *n, int *prec, CgxTy *nat) {
    const CgxBlk *a = &n->b1; const int na = a->n; CgxTy t0 = na ? cc_vty(a->p[0]) : cc_ty(CGT_ANY), t1 = na > 1 ? cc_vty(a->p[1]) : cc_ty(CGT_ANY);
    const int k0 = cc_kind(&t0), k1 = cc_kind(&t1); const CgxNode *const *ns = (const CgxNode *const *)a->p;
    CgxTy S = cc_ty(CGT_STR), I = cc_ty(CGT_INT), F = cc_ty(CGT_FLOAT), B = cc_ty(CGT_BOOL), A = cc_ty(CGT_ANY), LA = cc_listty(CGT_ANY), MA = cgx_t(CGT_MAP), V = cc_ty(CGT_VOID);
    *prec = 10; *nat = n->ty;
    switch (n->bi) {
        case CGB_LEN: *nat = I;
            if (k0 == CK_STR) return cc_callN(c, "cg_slen", ns, &S, 1, prec);
            if (k0 == CK_LIST) return cc_callN(c, "cg_llen", ns, &LA, 1, prec);
            if (k0 == CK_MAP) return cc_callN(c, "cg_mlen", ns, &MA, 1, prec);
            return cc_callN(c, "cg_vlen", ns, &A, 1, prec);
        case CGB_PUSH: {
            CgxTy lt = k0 == CK_LIST ? t0 : LA; CgxTy w[2] = {lt, cc_elemty(&lt)}; char fn[40]; *nat = V;
            if (cc_ek(lt.e) == 4) snprintf(fn, sizeof fn, "cg_lpushv"); else snprintf(fn, sizeof fn, "cg_l%spush", cc_pfx(lt.e));
            return cc_callN(c, fn, ns, w, 2, prec);
        }
        case CGB_POP: {
            CgxTy lt = k0 == CK_LIST ? t0 : LA; char fn[40]; *nat = cc_elemty(&lt);
            if (cc_ek(lt.e) == 4) snprintf(fn, sizeof fn, "cg_lpopv"); else snprintf(fn, sizeof fn, "cg_l%spop", cc_pfx(lt.e));
            return cc_callN(c, fn, ns, &lt, 1, prec);
        }
        case CGB_STR: *nat = S;
            if (k0 == CK_STR) return cc_exw(c, ns[0], &S, 0);
            if (k0 == CK_INT) return cc_callN(c, "cg_istr", ns, &I, 1, prec);
            if (k0 == CK_FLT) return cc_callN(c, "cg_fstr", ns, &F, 1, prec);
            if (k0 == CK_BOOL) return cc_callN(c, "cg_bstr", ns, &B, 1, prec);
            return cc_callN(c, "cg_vstr", ns, &A, 1, prec);
        case CGB_INT: *nat = I;
            if (k0 == CK_INT || k0 == CK_BOOL) return cc_exw(c, ns[0], &t0, 0);
            if (k0 == CK_FLT) return cc_callN(c, "cg_f2i", ns, &F, 1, prec);
            if (k0 == CK_STR) return cc_callN(c, "cg_s2i", ns, &S, 1, prec);
            return cc_callN(c, "cg_v2i", ns, &A, 1, prec);
        case CGB_FLOAT: *nat = F;
            if (k0 == CK_INT || k0 == CK_BOOL || k0 == CK_FLT) { CgxDoc *d = cc_exw(c, ns[0], &t0, 8); *prec = 8; return k0 == CK_FLT ? d : cc_pre(c, "(double)", d, ""); }
            if (k0 == CK_STR) return cc_callN(c, "cg_s2f", ns, &S, 1, prec);
            return cc_callN(c, "cg_v2f", ns, &A, 1, prec);
        case CGB_UPPER: *nat = S; return cc_callN(c, "cg_supper", ns, &S, 1, prec);
        case CGB_LOWER: *nat = S; return cc_callN(c, "cg_slower", ns, &S, 1, prec);
        case CGB_TRIM: *nat = S; return cc_callN(c, "cg_strim", ns, &S, 1, prec);
        case CGB_SPLIT: { CgxTy w[2] = {S, S}; *nat = cc_listty(CGT_STR); return cc_callN(c, "cg_ssplit", ns, w, 2, prec); }
        case CGB_WORDS: *nat = cc_listty(CGT_STR); return cc_callN(c, "cg_swords", ns, &S, 1, prec);
        case CGB_JOIN: { CgxTy w[2] = {LA, S}; *nat = S; return cc_callN(c, "cg_ljoin", ns, w, 2, prec); }
        case CGB_CONTAINS: *nat = B;
            if (k0 == CK_STR) { CgxTy w[2] = {S, S}; CgxDoc *d = cc_callN(c, "cg_sfind", ns, w, 2, prec); return cc_cmp0(c, d, prec, " >= 0"); }
            if (k0 == CK_LIST) { CgxTy w[2] = {LA, A}; return cc_callN(c, "cg_lhas", ns, w, 2, prec); }
            if (k0 == CK_MAP) { CgxTy w[2] = {MA, S}; return cc_callN(c, "cg_mhas", ns, w, 2, prec); }
            { CgxTy w[2] = {A, A}; return cc_callN(c, "cg_vhas", ns, w, 2, prec); }
        case CGB_INDEXOF: { CgxTy w[2] = {S, S}; *nat = I; return cc_callN(c, "cg_sfind", ns, w, 2, prec); }
        case CGB_REPLACE: { CgxTy w[3] = {S, S, S}; *nat = S; return cc_callN(c, "cg_sreplace", ns, w, 3, prec); }
        case CGB_STARTS: { CgxTy w[2] = {S, S}; *nat = B; return cc_callN(c, "cg_sstarts", ns, w, 2, prec); }
        case CGB_ENDS: { CgxTy w[2] = {S, S}; *nat = B; return cc_callN(c, "cg_sends", ns, w, 2, prec); }
        case CGB_SLICE: {
            CgxTy w[3]; const char *fn; CgxDoc *o[3], *pre; int m = na;
            w[0] = k0 == CK_STR ? S : k0 == CK_LIST ? (t0.e == CGT_ANY ? LA : t0) : A; w[1] = I; w[2] = I;
            fn = k0 == CK_STR ? "cg_sslice" : k0 == CK_LIST ? "cg_lslice" : "cg_vslice"; *nat = k0 == CK_ANY ? A : w[0];
            cc_ops(c, ns, w, m, 0, o, &pre);
            if (m == 2) o[2] = cc_t(c, "LLONG_MAX");
            *prec = 10; return cc_withpre(c, pre, cgx_dcat(CD, cc_t(c, fn), cc_seq(c, "(", o, 3, ")")), prec);
        }
        case CGB_KEYS: *nat = cc_listty(CGT_STR); return cc_callN(c, "cg_mkeys", ns, &MA, 1, prec);
        case CGB_PRINT: *nat = V; return cc_callN(c, "cg_print", ns, &A, 1, prec);
        case CGB_ABS: *nat = t0;
            if (k0 == CK_INT) return cc_callN(c, "cg_iabs", ns, &I, 1, prec);
            if (k0 == CK_FLT) return cc_callN(c, "fabs", ns, &F, 1, prec);
            *nat = A; return cc_callN(c, "cg_vabs", ns, &A, 1, prec);
        case CGB_MIN: case CGB_MAX: {
            const int mn = n->bi == CGB_MIN; CgxTy w[2];
            if (k0 == CK_ANY || k1 == CK_ANY) { w[0] = w[1] = A; *nat = A; return cc_callN(c, mn ? "cg_vmin" : "cg_vmax", ns, w, 2, prec); }
            if (k0 == CK_FLT || k1 == CK_FLT) { w[0] = w[1] = F; *nat = F; return cc_callN(c, mn ? "cg_fmin" : "cg_fmax", ns, w, 2, prec); }
            w[0] = w[1] = I; *nat = I; return cc_callN(c, mn ? "cg_imin" : "cg_imax", ns, w, 2, prec);
        }
        case CGB_SQRT: *nat = F; return cc_callN(c, "cg_sqrt", ns, &F, 1, prec);
        case CGB_FLOOR: *nat = I;
            if (k0 == CK_INT) return cc_exw(c, ns[0], &I, 0);
            return cc_callN(c, "cg_floor", ns, &F, 1, prec);
        case CGB_POW: { CgxTy w[2] = {F, F}; *nat = F; return cc_callN(c, "cg_pow", ns, w, 2, prec); }
        case CGB_SUM: *nat = A; return cc_callN(c, "cg_lsum", ns, &LA, 1, prec);
        case CGB_SORT: *nat = k0 == CK_LIST ? t0 : LA; { CgxTy w = *nat; return cc_callN(c, "cg_lsort", ns, &w, 1, prec); }
        case CGB_REVERSE: *nat = t0;
            if (k0 == CK_STR) return cc_callN(c, "cg_srev", ns, &S, 1, prec);
            if (k0 == CK_LIST) return cc_callN(c, "cg_lrev", ns, &t0, 1, prec);
            *nat = A; return cc_callN(c, "cg_vrev", ns, &A, 1, prec);
        case CGB_MATCHES: { CgxTy w[2] = {S, S}; c->need_re = 1; *nat = B; return cc_callN(c, "cg_matches", ns, w, 2, prec); }
        case CGB_ERRMSG: *nat = S;
            if (k0 == CK_STR) return cc_exw(c, ns[0], &S, 0);
            return cc_callN(c, "cg_vstr", ns, &A, 1, prec);
    }
    cgx_err(c->g, n->line, "c: built-in `%s` has no C form yet", n->s);
    return cgx_dnil(CD);
}

/* ---- operadores ---- */
static int cc_isnum(int k) { return k == CK_INT || k == CK_FLT || k == CK_BOOL; }
static CgxDoc *cc_notnull(CgxC *c, const CgxNode *x, int eq, int *prec) {            /* x == null / x != null */
    CgxTy tx = cc_vty(x); const int k = cc_kind(&tx); CgxDoc *d = cc_exw(c, x, &tx, 10); *prec = 4;
    if (k == CK_STR) return cgx_dcatn(CD, d, cc_t(c, eq ? ".p == NULL" : ".p != NULL"), NULL);
    if (k == CK_ANY) return cgx_dcatn(CD, d, cc_t(c, eq ? ".t == CG_NIL" : ".t != CG_NIL"), NULL);
    return cgx_dcatn(CD, d, cc_t(c, eq ? " == NULL" : " != NULL"), NULL);
}
static CgxDoc *cc_bin(CgxC *c, const CgxNode *n, int *prec, CgxTy *nat) {
    const CgxTy ta = cc_vty(n->a), tb = cc_vty(n->b); const int ka = cc_kind(&ta), kb = cc_kind(&tb), op = n->op;
    CgxTy I = cc_ty(CGT_INT), F = cc_ty(CGT_FLOAT), S = cc_ty(CGT_STR), B = cc_ty(CGT_BOOL), A = cc_ty(CGT_ANY);
    const CgxNode *ns[2] = {n->a, n->b}; CgxTy w[2];
    static const char *const cmpsym[] = {"==", "!=", "<", "<=", ">", ">="};
    *prec = 10; *nat = n->ty;
    switch (op) {
        case CGO_AND: case CGO_OR: *nat = B; return cc_infix(c, op == CGO_AND ? "&&" : "||", n->a, n->b, &B, &B, op == CGO_AND ? 3 : 2, prec);
        case CGO_EQ: case CGO_NE: case CGO_LT: case CGO_LE: case CGO_GT: case CGO_GE: {
            const int ne = op == CGO_NE, ord = op >= CGO_LT; *nat = B;
            if (!ord && (ka == CK_NULL || kb == CK_NULL)) {
                if (ka == CK_NULL && kb == CK_NULL) return cc_t(c, ne ? "0" : "1");
                return cc_notnull(c, ka == CK_NULL ? n->b : n->a, !ne, prec);
            }
            if (cc_isnum(ka) && cc_isnum(kb) && ka != CK_FLT && n->a->k == CGN_VAR && n->b->k == CGN_VAR && !strcmp(n->a->s, n->b->s) && n->a->ref == n->b->ref)
                return cc_t(c, (op == CGO_EQ || op == CGO_LE || op == CGO_GE) ? "1" : "0");                  /* x == x: sem comparar a variavel com ela mesma (-Wtautological-compare) */
            if (cc_isnum(ka) && cc_isnum(kb)) { const CgxTy w1 = (ka == CK_FLT || kb == CK_FLT) ? F : I; return cc_infix(c, cmpsym[op - CGO_EQ], n->a, n->b, &w1, &w1, ord ? CC_PCMP : 4, prec); }
            if (ka == CK_STR && kb == CK_STR) {
                w[0] = w[1] = S; CgxDoc *d = cc_callN(c, ord ? "cg_scmp" : "cg_seq", ns, w, 2, prec);
                if (ord) { char t[8]; snprintf(t, sizeof t, " %s 0", cmpsym[op - CGO_EQ]); return cc_cmp0(c, d, prec, t); }
                if (ne) { *prec = 8; return cgx_dcat(CD, cc_t(c, "!"), cc_wrap(c, d, 10, 11)); }
                return d;
            }
            if (!ord && ka == CK_OBJ && kb == CK_OBJ) return cc_infix(c, cmpsym[op - CGO_EQ], n->a, n->b, &ta, &tb, 4, prec);
            w[0] = w[1] = A;
            if (!ord) { CgxDoc *d = cc_callN(c, "cg_veq", ns, w, 2, prec); if (ne) { *prec = 8; return cgx_dcat(CD, cc_t(c, "!"), cc_wrap(c, d, 10, 11)); } return d; }
            { CgxDoc *d = cc_callN(c, "cg_vcmp", ns, w, 2, prec); char t[8]; snprintf(t, sizeof t, " %s 0", cmpsym[op - CGO_EQ]); return cc_cmp0(c, d, prec, t); }
        }
        case CGO_IN: {                                   /* x in b: a avalia primeiro, a funcao recebe (b, x) */
            CgxDoc *o[2], *pre; const char *fn; CgxTy wb;
            *nat = B;
            if (kb == CK_STR) { fn = "cg_sfind"; w[0] = S; wb = S; }
            else if (kb == CK_LIST) { fn = "cg_lhas"; w[0] = A; wb = cc_listty(CGT_ANY); }
            else if (kb == CK_MAP) { fn = "cg_mhas"; w[0] = S; wb = cgx_t(CGT_MAP); }
            else { fn = "cg_vhas"; w[0] = A; wb = A; }
            w[1] = wb; cc_ops(c, ns, w, 2, 0, o, &pre);
            CgxDoc *call = cgx_dcat(CD, cc_t(c, fn), cc_seq(c, "(", (CgxDoc *[]){o[1], o[0]}, 2, ")"));
            if (kb == CK_STR) { *prec = 10; call = cc_withpre(c, pre, call, prec); return cc_cmp0(c, call, prec, " >= 0"); }
            *prec = 10; return cc_withpre(c, pre, call, prec);
        }
        case CGO_ADD: case CGO_SUB: case CGO_MUL: case CGO_DIV: case CGO_IDIV: case CGO_MOD: {
            if (ka == CK_ANY || kb == CK_ANY || (ka == CK_STR && kb != CK_STR) || (kb == CK_STR && ka != CK_STR)) {
                static const int code[] = {0, 1, 2, 3, 4, 5}; CgxDoc *o[2], *pre; w[0] = w[1] = A; *nat = A;
                cc_ops(c, ns, w, 2, 0, o, &pre);
                CgxDoc *call = cgx_dcatn(CD, cgx_dtextf(CD, "cg_varith(%d, ", code[op]), o[0], cc_t(c, ", "), o[1], cc_t(c, ")"), NULL);
                *prec = 10; return cc_withpre(c, pre, call, prec);
            }
            if (ka == CK_STR) { w[0] = w[1] = S; *nat = S; return cc_callN(c, "cg_sadd", ns, w, 2, prec); }
            if (!cc_isnum(ka) || !cc_isnum(kb)) { cgx_err(c->g, n->line, "c: operands of %s must be numbers or str", CGO_NAME[op]); return cgx_dnil(CD); }
            const int fl = ka == CK_FLT || kb == CK_FLT;
            if (op == CGO_DIV) { w[0] = w[1] = F; *nat = F; return cc_callN(c, "cg_fdiv", ns, w, 2, prec); }
            if (op == CGO_IDIV || op == CGO_MOD) {
                if (fl) { w[0] = w[1] = F; *nat = F; return cc_callN(c, "cg_fmod", ns, w, 2, prec); }
                w[0] = w[1] = I; *nat = I; return cc_callN(c, op == CGO_IDIV ? "cg_iidiv" : "cg_imod", ns, w, 2, prec);
            }
            if (fl) { *nat = F; return cc_infix(c, op == CGO_ADD ? "+" : op == CGO_SUB ? "-" : "*", n->a, n->b, &F, &F, op == CGO_MUL ? 7 : 6, prec); }
            w[0] = w[1] = I; *nat = I; return cc_callN(c, op == CGO_ADD ? "cg_iadd" : op == CGO_SUB ? "cg_isub" : "cg_imul", ns, w, 2, prec);
        }
    }
    cgx_err(c->g, n->line, "c: operator %s is not supported here", CGO_NAME[op]);
    return cgx_dnil(CD);
}

/* receptor de campo ou metodo: um ponteiro de classe nao nulo */
static CgxDoc *cc_recv(CgxC *c, const CgxNode *r, CgxDoc *d, const CgxTy *t) {
    if (r->k == CGN_VAR && !strcmp(r->s, "self")) return d;
    return cgx_dcatn(CD, cgx_dtextf(CD, "((C_%s *)cg_nn(", t->cls), d, cc_t(c, "))"), NULL);
}
static void cc_fname(const CgxNode *fn, const CgxNode *cls, CgxB *o) {
    if (cls) cgx_bf(o, "M_%s__%s", cls->s, fn->s); else cgx_bf(o, "u_%s", fn->s);
}
static CgxDoc *cc_userargs(CgxC *c, const CgxNode *callee, const CgxNode *recv, const CgxBlk *args, const char *fname, int *prec) {
    const CgxNode *ns[64] = {0}; CgxTy w[64] = {{0}}; CgxDoc *o[64], *pre; int m = 0;
    CgxTy rt = recv ? cc_vty(recv) : cc_ty(CGT_VOID);
    if (recv) { ns[m] = recv; w[m] = rt; m++; }
    const int np = callee ? callee->npar : 0;
    for (int i = 0; i < np && m < 64; i++) { const CgxParam *p = &callee->par[i]; ns[m] = i < args->n ? args->p[i] : p->def; w[m] = p->ty; if (!ns[m]) ns[m] = args->p[0]; m++; }
    cc_ops(c, ns, w, m, 0, o, &pre);
    if (recv) o[0] = cc_recv(c, recv, o[0], &rt);
    if (!m) { *prec = 10; return cgx_dtextf(CD, "%s()", fname); }
    *prec = 10; CgxDoc *call = cgx_dcat(CD, cc_t(c, fname), cc_seq(c, "(", o, m, ")"));
    return cc_withpre(c, pre, call, prec);
}

/* receptor sem classe conhecida (any): o gerador cria um ajudante que escolhe o metodo ou o campo pelo id da classe do objeto */
static CgxDoc *cc_dyncall(CgxC *c, int kind, const char *name, const CgxNode *recv, const CgxBlk *args, const CgxNode *val, int line, int *prec) {
    int idx = -1, k = args ? args->n : 0; char fn[96]; CgxTy A = cgx_t(CGT_ANY);
    for (int i = 0; i < c->ndyn; i++) if (c->dyn[i].kind == kind && c->dyn[i].n == k && !strcmp(c->dyn[i].name, name)) idx = i;
    if (idx < 0) {
        if (c->ndyn >= 96) { cgx_err(c->g, line, "c: too many dynamic member accesses"); return cgx_dnil(CD); }
        idx = c->ndyn++; snprintf(c->dyn[idx].name, sizeof c->dyn[idx].name, "%s", name); c->dyn[idx].kind = kind; c->dyn[idx].n = k;
    }
    snprintf(fn, sizeof fn, "cg_d%c_%s_%d", kind == 0 ? 'm' : kind == 1 ? 'f' : 's', name, k);
    const CgxNode *ns[40] = {0}; CgxTy w[40]; int m = 0;
    ns[m] = recv; w[m++] = A;
    for (int i = 0; i < k && m < 39; i++) { ns[m] = args->p[i]; w[m++] = A; }
    if (val) { ns[m] = val; w[m++] = A; }
    return cc_callN(c, fn, ns, w, m, prec);
}
static void cc_fn_begin(CgxC *c, const CgxNode *fn, const CgxNode *cls, int vol);
static void cc_dynboxres(CgxC *c, CgxB *o, const CgxTy *t, const char *e) {
    switch (cc_kind(t)) {
        case CK_INT: cgx_bf(o, "cg_vint(%s)", e); break; case CK_FLT: cgx_bf(o, "cg_vflt(%s)", e); break; case CK_BOOL: cgx_bf(o, "cg_vbool(%s)", e); break;
        case CK_STR: cgx_bf(o, "cg_vstrv(%s)", e); break; case CK_LIST: cgx_bf(o, "cg_vlist(%s)", e); break; case CK_MAP: cgx_bf(o, "cg_vmap(%s)", e); break;
        case CK_OBJ: cgx_bf(o, "cg_vobj((void *)%s, %d)", e, cc_clsid(c, t->cls)); break; default: cgx_bs(o, e);
    }
}
static char *cc_render(CgxC *c, CgxDoc *d) { CgxB b = {0}; cgx_render(d, 100000, &b); return b.p ? b.p : strdup(""); }
/* texto dos ajudantes, depois que todas as funcoes foram geradas (prototipos em *pr, definicoes em *df) */
static void cc_dynemit(CgxC *c, CgxB *pr, CgxB *df) {
    for (int i = 0; i < c->ndyn; i++) {
        const char *nm = c->dyn[i].name; const int kind = c->dyn[i].kind, k = c->dyn[i].n; CgxB sig = {0}; char fn[96];
        snprintf(fn, sizeof fn, "cg_d%c_%s_%d", kind == 0 ? 'm' : kind == 1 ? 'f' : 's', nm, k);
        cgx_bf(&sig, "static %s %s(CgV r", kind == 2 ? "void" : "CgV", fn);
        if (kind == 0) for (int j = 0; j < k; j++) cgx_bf(&sig, ", CgV a%d", j);
        if (kind == 2) cgx_bs(&sig, ", CgV v");
        cgx_bs(&sig, ")");
        cgx_bf(pr, "%s;\n", sig.p);
        cgx_bf(df, "%s {\n    if (r.t == CG_OBJ) switch (r.u.o.cls) {\n", sig.p);
        int found = 0;
        for (int ci = 0; ci < c->ncls; ci++) {
            const CgxNode *cl = c->cls_list[ci];
            if (kind == 0) {
                const CgxNode *m = NULL; for (int j = 0; j < cl->b1.n; j++) if (cl->b1.p[j]->k == CGN_FN && !strcmp(cl->b1.p[j]->s, nm) && strcmp(nm, "init")) m = cl->b1.p[j];
                if (!m || k > m->npar) continue;
                int req = 0; for (int j = 0; j < m->npar; j++) if (!m->par[j].def) req = j + 1;
                if (k < req) continue;
                CgxB call = {0}; found = 1;
                cgx_bf(&call, "M_%s__%s((C_%s *)r.u.o.p", cl->s, nm, cl->s);
                cc_fn_begin(c, NULL, NULL, 0);
                for (int j = 0; j < m->npar; j++) {
                    char an[16]; snprintf(an, sizeof an, "a%d", j); CgxTy pt = m->par[j].ty;
                    char *t = j < k ? cc_render(c, cc_unbox(c, cgx_dtext(CD, an), &pt)) : cc_render(c, cc_exw(c, m->par[j].def, &pt, 0));
                    if (cc_kind(&pt) == CK_ANY && j < k) { free(t); t = strdup(an); }
                    cgx_bf(&call, ", %s", t); free(t);
                }
                cgx_bs(&call, ")");
                CgxTy rt = m->ty;
                if (cc_kind(&rt) == CK_VOID) cgx_bf(df, "        case %d: %s; return cg_nil();\n", ci + 1, call.p);
                else { CgxB bx = {0}; cc_dynboxres(c, &bx, &rt, call.p); cgx_bf(df, "        case %d: return %s;\n", ci + 1, bx.p); free(bx.p); }
                free(call.p);
            } else {
                const CgxNode *f = NULL; for (int j = 0; j < cl->b1.n; j++) if (cl->b1.p[j]->k == CGN_FIELDDECL && !strcmp(cl->b1.p[j]->s, nm)) f = cl->b1.p[j];
                if (!f) continue;
                found = 1; CgxB fnm = {0}; cc_name(nm, &fnm); CgxTy ft = f->ty;
                if (kind == 1) { CgxB acc = {0}, bx = {0}; cgx_bf(&acc, "((C_%s *)r.u.o.p)->%s", cl->s, fnm.p); cc_dynboxres(c, &bx, &ft, acc.p); cgx_bf(df, "        case %d: return %s;\n", ci + 1, bx.p); free(acc.p); free(bx.p); }
                else { cc_fn_begin(c, NULL, NULL, 0); char *t = cc_render(c, cc_unbox(c, cgx_dtext(CD, "v"), &ft)); cgx_bf(df, "        case %d: ((C_%s *)r.u.o.p)->%s = %s; return;\n", ci + 1, cl->s, fnm.p, t); free(t); }
                free(fnm.p);
            }
        }
        cgx_bf(df, "        default: break;\n    }\n    cg_throw(CG_S(\"no member `%s` on this value\"));\n}\n\n", nm);
        if (!found) cgx_err(c->g, 0, "c: no class has a %s named `%s`", kind == 0 ? "method" : "field", nm);
        free(sig.p);
    }
}

static CgxDoc *cc_ex0(CgxC *c, const CgxNode *n, int *prec, CgxTy *nat) {
    CgxB b = {0}; CgxDoc *d; *prec = 11; *nat = n->ty;
    switch (n->k) {
        case CGN_INT:
            if (n->iv == LLONG_MIN) cgx_bs(&b, "(-9223372036854775807LL - 1)");
            else { cgx_bf(&b, n->iv >= -2147483647 && n->iv <= 2147483647 ? "%lld" : "%lldLL", n->iv); if (n->iv < 0) *prec = 8; }
            break;
        case CGN_FLT: { char t[64]; cgx_fmt_float(n->dv, t, sizeof t); cgx_bs(&b, t); if (n->dv < 0) *prec = 8; break; }
        case CGN_STR: free(b.p); return cc_lit_str(c, n->s);
        case CGN_BOOL: cgx_bs(&b, n->iv ? "1" : "0"); break;
        case CGN_NULL: cgx_bs(&b, "NULL"); *nat = cc_ty(CGT_NULL); break;
        case CGN_VAR:
            if (n->flag == 77) cgx_bs(&b, n->s);
            else if (n->ref && n->ref->k == CGN_LET && (n->ref->flag & 1)) cgx_bf(&b, "u_%s", n->s);
            else cc_name(n->s, &b);
            *nat = cc_vty(n); break;
        case CGN_LIST: free(b.p); { CgxTy w = n->ty; *nat = n->ty; return cc_list(c, n, &w); }
        case CGN_MAP: free(b.p); { CgxTy w = n->ty; *nat = n->ty; return cc_map(c, n, &w); }
        case CGN_INDEX: {
            free(b.p); const CgxTy tc = cc_vty(n->a); const int kc = cc_kind(&tc); char fn[40]; const CgxNode *ns[2] = {n->a, n->b}; CgxTy w[2];
            if (kc == CK_LIST) { w[0] = tc; w[1] = cc_ty(CGT_INT); if (cc_ek(tc.e) == 4) snprintf(fn, sizeof fn, "cg_lgetv"); else snprintf(fn, sizeof fn, "cg_l%sget", cc_pfx(tc.e)); *nat = cc_elemty(&tc); }
            else if (kc == CK_MAP) { w[0] = tc; w[1] = cc_ty(CGT_STR); if (cc_ek(tc.e) == 4) snprintf(fn, sizeof fn, "cg_mgetv"); else snprintf(fn, sizeof fn, "cg_m%sget", cc_pfx(tc.e)); *nat = cc_elemty(&tc); }
            else if (kc == CK_STR) { w[0] = cc_ty(CGT_STR); w[1] = cc_ty(CGT_INT); snprintf(fn, sizeof fn, "cg_sidx"); *nat = cc_ty(CGT_STR); }
            else { w[0] = cc_ty(CGT_ANY); w[1] = cc_ty(CGT_ANY); snprintf(fn, sizeof fn, "cg_vget"); *nat = cc_ty(CGT_ANY); }
            if (kc == CK_LIST && tc.e == CGT_ANY) w[0] = cc_listty(CGT_ANY);
            return cc_callN(c, fn, ns, w, 2, prec);
        }
        case CGN_FIELD: {
            free(b.p); const CgxTy tc = cc_vty(n->a);
            if (cc_kind(&tc) == CK_ANY) { *nat = cc_ty(CGT_ANY); return cc_dyncall(c, 1, n->s, n->a, NULL, NULL, n->line, prec); }
            if (cc_kind(&tc) != CK_OBJ) { cgx_err(c->g, n->line, "c: the field `%s` is read from a value that is not an object", n->s); return cgx_dnil(CD); }
            CgxDoc *o = cc_exw(c, n->a, &tc, 10); *prec = 10; *nat = n->ref ? n->ref->ty : n->ty;
            CgxB f = {0}; cgx_bs(&f, "->"); cc_name(n->s, &f); d = cgx_dcatn(CD, cc_recv(c, n->a, o, &tc), cgx_dtext(CD, f.p), NULL); free(f.p); return d;
        }
        case CGN_UN: free(b.p);
            if (n->op == CGO_NOT) { CgxTy B = cc_ty(CGT_BOOL); *prec = 8; *nat = B; return cgx_dcat(CD, cc_t(c, "!"), cc_exw(c, n->a, &B, 8)); }
            { const CgxTy ta = cc_vty(n->a); const int k = cc_kind(&ta); const CgxNode *ns[1] = {n->a};
              if (k == CK_FLT) { *prec = 8; *nat = ta; return cgx_dcat(CD, cc_t(c, "-"), cc_exw(c, n->a, &ta, 8)); }
              if (k == CK_INT) { CgxTy I = cc_ty(CGT_INT); *nat = I; return cc_callN(c, "cg_ineg", ns, &I, 1, prec); }
              { CgxTy A = cc_ty(CGT_ANY); *nat = A; return cc_callN(c, "cg_vneg", ns, &A, 1, prec); } }
        case CGN_BIN: free(b.p); return cc_bin(c, n, prec, nat);
        case CGN_TERN: {
            free(b.p); CgxTy B = cc_ty(CGT_BOOL); const CgxTy r = n->ty; *prec = 1; *nat = r;
            return cgx_dcatn(CD, cc_exw(c, n->a, &B, 2), cc_t(c, " ? "), cc_exw(c, n->b, &r, 2), cc_t(c, " : "), cc_exw(c, n->c, &r, 1), NULL);
        }
        case CGN_CALL: {
            free(b.p);
            if (n->bi) return cc_builtin(c, n, prec, nat);
            if (n->ref && n->ref->k == CGN_CLASS) {
                const CgxNode *cls = n->ref, *init = NULL; CgxB f = {0};
                for (int i = 0; i < cls->b1.n; i++) if (cls->b1.p[i]->k == CGN_FN && !strcmp(cls->b1.p[i]->s, "init")) init = cls->b1.p[i];
                cgx_bf(&f, "N_%s", cls->s); *nat = n->ty; d = cc_userargs(c, init, NULL, &n->b1, f.p, prec); free(f.p); return d;
            }
            if (n->ref && n->ref->k == CGN_FN) { CgxB f = {0}; cc_fname(n->ref, NULL, &f); *nat = n->ref->ty; d = cc_userargs(c, n->ref, NULL, &n->b1, f.p, prec); free(f.p); return d; }
            cgx_err(c->g, n->line, "c: call to `%s` could not be resolved", n->s); return cgx_dnil(CD);
        }
        case CGN_MCALL: {
            free(b.p); const CgxTy tr = cc_vty(n->a);
            if (cc_kind(&tr) == CK_ANY) { *nat = cc_ty(CGT_ANY); return cc_dyncall(c, 0, n->s, n->a, &n->b1, NULL, n->line, prec); }
            if (cc_kind(&tr) != CK_OBJ || !n->ref) { cgx_err(c->g, n->line, "c: the method `%s` is called on a value that is not an object", n->s); return cgx_dnil(CD); }
            const CgxNode *cls = NULL; for (int i = 0; i < c->ncls; i++) if (!strcmp(c->cls_list[i]->s, tr.cls)) cls = c->cls_list[i];
            CgxB f = {0}; cc_fname(n->ref, cls, &f); *nat = n->ref->ty; d = cc_userargs(c, n->ref, n->a, &n->b1, f.p, prec); free(f.p); return d;
        }
        default: cgx_err(c->g, n->line, "c: unexpected expression node %d", n->k); free(b.p); return cgx_dnil(CD);
    }
    d = cgx_dtext(CD, b.p ? b.p : ""); free(b.p); return d;
}
/* a expressao, ja com o tipo `want` (NULL: o tipo que o validador deu ao no) e parenteses se precisar */
static CgxDoc *cc_exw(CgxC *c, const CgxNode *n, const CgxTy *want, int minprec) {
    const CgxTy w = want ? *want : cc_vty(n);
    if (n->k == CGN_LIST) return cc_wrap(c, cc_list(c, n, &w), 10, minprec);
    if (n->k == CGN_MAP) return cc_wrap(c, cc_map(c, n, &w), 10, minprec);
    if (n->k == CGN_INT && cc_kind(&w) == CK_FLT) { CgxB b = {0}; cgx_bf(&b, "%lld.0", n->iv); CgxDoc *d = cgx_dtext(CD, b.p); free(b.p); return cc_wrap(c, d, n->iv < 0 ? 8 : 11, minprec); }
    int prec; CgxTy nat; CgxDoc *d = cc_ex0(c, n, &prec, &nat);
    const int kn = cc_kind(&nat), kw = cc_kind(&w);
    if (kn != kw || (kn == CK_LIST && nat.e != w.e) || (kn == CK_MAP && nat.e != w.e) || kn == CK_NULL) {
        if (kn == CK_INT && kw == CK_FLT) { d = cc_pre(c, "(double)", cc_wrap(c, d, prec, 8), ""); prec = 8; }
        else { d = cc_conv(c, cc_wrap(c, d, prec, 0), &nat, &w); prec = 10; }
    }
    return cc_wrap(c, d, prec, minprec);
}

/* ---- instrucoes ---- */
static void cc_scan(CgxC *c, const CgxNode *n) {                   /* quem e lido: o que nunca e lido ganha (void) para nao avisar */
    if (!n) return;
    if (n->k == CGN_BIN && n->op >= CGO_EQ && n->op <= CGO_GE && n->a->k == CGN_VAR && n->b->k == CGN_VAR && !strcmp(n->a->s, n->b->s) && n->a->ref == n->b->ref && cc_kind(&n->a->ty) != CK_FLT) return;   /* x == x vira constante */
    if (n->k == CGN_VAR && n->ref) {
        if (c->nused == c->capused) { c->capused = c->capused ? c->capused * 2 : 64; c->used = realloc(c->used, sizeof c->used[0] * (size_t)c->capused); }
        c->used[c->nused].ref = n->ref; c->used[c->nused].name = n->s; c->nused++;
    }
    if (n->k == CGN_ASSIGN && n->op < 0 && n->a->k == CGN_VAR) cc_scan(c, n->b);
    else { cc_scan(c, n->a); cc_scan(c, n->b); }
    cc_scan(c, n->c);
    const CgxBlk *bl[3] = {&n->b1, &n->b2, &n->b3};
    for (int k = 0; k < 3; k++) for (int i = 0; i < bl[k]->n; i++) cc_scan(c, bl[k]->p[i]);
    for (int i = 0; i < n->npar; i++) cc_scan(c, n->par[i].def);
}
static int cc_isused(const CgxC *c, const void *ref, const char *name) { for (int i = 0; i < c->nused; i++) if (c->used[i].ref == ref && !strcmp(c->used[i].name, name)) return 1; return 0; }
static int cc_has_try(const CgxBlk *b) {
    for (int i = 0; i < b->n; i++) {
        const CgxNode *s = b->p[i];
        if (s->k == CGN_TRY) return 1;
        if (cc_has_try(&s->b1) || cc_has_try(&s->b2) || cc_has_try(&s->b3)) return 1;
    }
    return 0;
}

static CgxDoc *cc_stmt(CgxC *c, const CgxNode *s);
static CgxDoc *cc_stmts(CgxC *c, const CgxBlk *b, CgxDoc *prologue) {
    CgxDoc *in = prologue ? cgx_dcat(CD, cgx_dhard(CD), prologue) : NULL;
    for (int i = 0; i < b->n; i++) {
        const CgxNode *s = b->p[i];
        if ((s->k == CGN_COMMENT || s->k == CGN_DOC) && !c->o->comments) continue;
        CgxDoc *d = cc_stmt(c, s); if (!d) continue;
        CgxDoc *x = cgx_dcat(CD, cgx_dhard(CD), d); in = in ? cgx_dcat(CD, in, x) : x;
    }
    return in;
}
static CgxDoc *cc_block(CgxC *c, const CgxBlk *b, CgxDoc *prologue, CgxDoc *epilogue) {
    CgxDoc *in = cc_stmts(c, b, prologue);
    if (epilogue) { CgxDoc *x = cgx_dcat(CD, cgx_dhard(CD), epilogue); in = in ? cgx_dcat(CD, in, x) : x; }
    if (!in) return cc_t(c, "{ }");
    return cgx_dcatn(CD, cc_t(c, "{"), cgx_dnest(CD, c->o->indent, in), cgx_dhard(CD), cc_t(c, "}"), NULL);
}
/* sai de `to` regioes: solta cada manipulador ativo e roda o finally, de dentro para fora */
static CgxDoc *cc_unwind(CgxC *c, int to) {
    CgxDoc *r = NULL; const int top = c->nreg;
    for (int i = top - 1; i >= to; i--) {
        const CgxCReg rg = c->reg[i]; CgxDoc *x = NULL;
        if (rg.active) x = cgx_dcat(CD, cgx_dhard(CD), cc_t(c, "cg_leave();"));
        if (rg.fin) { c->nreg = i; CgxDoc *f = cc_block(c, rg.fin, NULL, NULL); c->nreg = top; x = x ? cgx_dcatn(CD, x, cgx_dhard(CD), f, NULL) : cgx_dcat(CD, cgx_dhard(CD), f); }
        if (x) r = r ? cgx_dcat(CD, r, x) : x;
    }
    return r;
}
static CgxDoc *cc_wrapblock(CgxC *c, CgxDoc *inner) {          /* { inner } com inner = lista de linhas que comecam por hard */
    return cgx_dcatn(CD, cc_t(c, "{"), cgx_dnest(CD, c->o->indent, inner), cgx_dhard(CD), cc_t(c, "}"), NULL);
}
static CgxDoc *cc_voidname(CgxC *c, CgxDoc *d, const char *name, int used) { return used ? d : cgx_dcatn(CD, d, cgx_dtextf(CD, " (void)%s;", name), NULL); }
static const char *cc_zero(const CgxTy *t) {
    switch (cc_kind(t)) { case CK_INT: return "0"; case CK_FLT: return "0.0"; case CK_BOOL: return "0"; case CK_STR: return "CG_NULLSTR"; case CK_LIST: case CK_MAP: case CK_OBJ: return "NULL"; }
    return "cg_nil()";
}
static CgxDoc *cc_load_target(CgxC *c, const CgxNode *t, CgxDoc *v, const CgxTy *vt);
/* alvo = valor; alvo e VAR, INDEX ou FIELD; v ja tem o tipo do alvo */
static CgxDoc *cc_store(CgxC *c, const CgxNode *t, const CgxNode *rhs, CgxDoc *rhsdoc, const CgxTy *tt) {
    (void)rhsdoc;
    if (t->k == CGN_VAR) {
        CgxB nm = {0}; if (t->flag == 77) cgx_bs(&nm, t->s); else if (t->ref && t->ref->k == CGN_LET && (t->ref->flag & 1)) cgx_bf(&nm, "u_%s", t->s); else cc_name(t->s, &nm);
        CgxDoc *d = cgx_dcatn(CD, cgx_dtextf(CD, "%s = ", nm.p), cc_exw(c, rhs, tt, 0), cc_t(c, ";"), NULL); free(nm.p); return d;
    }
    if (t->k == CGN_FIELD) {
        const CgxTy to = cc_vty(t->a);
        if (cc_kind(&to) == CK_ANY) { int pr; CgxDoc *d = cc_dyncall(c, 2, t->s, t->a, NULL, rhs, t->line, &pr); return cgx_dcat(CD, d, cc_t(c, ";")); }
        if (cc_kind(&to) != CK_OBJ) { cgx_err(c->g, t->line, "c: the field `%s` is assigned on a value that is not an object", t->s); return NULL; }
        const CgxNode *ns[2] = {rhs, t->a}; CgxTy w[2] = {*tt, to}; CgxDoc *o[2], *pre; cc_ops(c, ns, w, 2, 0, o, &pre);
        CgxB f = {0}; cgx_bs(&f, "->"); cc_name(t->s, &f);
        CgxDoc *lhs = cgx_dcatn(CD, cc_recv(c, t->a, o[1], &to), cgx_dtext(CD, f.p), NULL); free(f.p);
        CgxDoc *as = cgx_dcatn(CD, lhs, cc_t(c, " = "), o[0], NULL);
        return cgx_dcatn(CD, pre ? cgx_dcatn(CD, pre, cc_t(c, ", "), as, NULL) : as, cc_t(c, ";"), NULL);
    }
    /* INDEX */
    const CgxTy tc = cc_vty(t->a); const int kc = cc_kind(&tc); char fn[40]; CgxTy w[3]; const CgxNode *ns[3] = {rhs, t->a, t->b}; CgxDoc *o[3], *pre;
    if (kc == CK_LIST) {
        w[1] = tc.e == CGT_ANY ? cc_listty(CGT_ANY) : tc; w[2] = cc_ty(CGT_INT); w[0] = cc_ek(tc.e) == 4 ? cc_ty(CGT_ANY) : cc_elemty(&tc);
        if (cc_ek(tc.e) == 4) snprintf(fn, sizeof fn, "cg_lsetv"); else snprintf(fn, sizeof fn, "cg_l%sset", cc_pfx(tc.e));
    } else if (kc == CK_MAP) {
        w[1] = tc; w[2] = cc_ty(CGT_STR); w[0] = cc_ek(tc.e) == 4 ? cc_ty(CGT_ANY) : cc_elemty(&tc);
        if (cc_ek(tc.e) == 4) snprintf(fn, sizeof fn, "cg_msetv"); else snprintf(fn, sizeof fn, "cg_m%sset", cc_pfx(tc.e));
    } else if (kc == CK_ANY) { w[0] = w[1] = w[2] = cc_ty(CGT_ANY); snprintf(fn, sizeof fn, "cg_vset"); }
    else { cgx_err(c->g, t->line, "c: cannot assign to an element of this value"); return NULL; }
    cc_ops(c, ns, w, 3, 0, o, &pre);
    CgxDoc *args[3] = {o[1], o[2], o[0]};
    CgxDoc *call = cgx_dcat(CD, cc_t(c, fn), cc_seq(c, "(", args, 3, ")"));
    return cgx_dcatn(CD, pre ? cgx_dcatn(CD, pre, cc_t(c, ", "), call, NULL) : call, cc_t(c, ";"), NULL);
}
static CgxNode *cc_synth_var(CgxC *c, const char *name, CgxTy ty) { CgxNode *v = (CgxNode *)cgx_alloc(c->g, sizeof *v); v->k = CGN_VAR; v->s = cgx_dup(c->g, name); v->flag = 77; v->ty = ty; return v; }
static CgxDoc *cc_assign(CgxC *c, const CgxNode *s) {
    const CgxNode *t = s->a; CgxTy tt = t->k == CGN_VAR ? cc_vty(t) : t->k == CGN_FIELD && t->ref ? t->ref->ty : t->ty;
    if (cc_kind(&tt) == CK_VOID) tt = cc_ty(CGT_ANY);
    if (s->op < 0) return cc_store(c, t, s->b, NULL, &tt);
    if (s->op == CGO_DIV && cc_kind(&tt) == CK_INT) { cgx_err(c->g, s->line, "c: `/=` on an int variable would store a float; use `//=`-style `x = x // y`, or declare the variable float"); return NULL; }
    CgxNode bin; memset(&bin, 0, sizeof bin); bin.k = CGN_BIN; bin.op = s->op; bin.line = s->line; bin.ty = tt;
    if ((t->k == CGN_INDEX || t->k == CGN_FIELD) && (cc_bits(t->a) || (t->k == CGN_INDEX && cc_bits(t->b)))) {
        /* alvo com efeito: avalia o recipiente e o indice uma vez, em variaveis do bloco */
        const int id = ++c->uniq; CgxDoc *in = NULL; char n1[24], n2[24];
        const CgxTy tc = cc_vty(t->a); CgxNode t2 = *t; CgxNode *v1, *v2 = NULL;
        CgxTy tcn = tc.k == CGT_LIST && tc.e == CGT_ANY ? cc_listty(CGT_ANY) : tc;
        snprintf(n1, sizeof n1, "cgc%d", id); snprintf(n2, sizeof n2, "cgd%d", id);
        v1 = cc_synth_var(c, n1, tc); t2.a = v1;
        { CgxB ty = {0}; cc_type(&tcn, &ty); CgxDoc *d = cgx_dcatn(CD, cgx_dtextf(CD, "%s %s = ", ty.p, n1), cc_exw(c, t->a, &tcn, 0), cc_t(c, ";"), NULL); free(ty.p); in = cgx_dcat(CD, cgx_dhard(CD), d); }
        if (t->k == CGN_INDEX) {
            CgxTy it = cc_kind(&tc) == CK_MAP ? cc_ty(CGT_STR) : cc_kind(&tc) == CK_LIST ? cc_ty(CGT_INT) : cc_ty(CGT_ANY); CgxB ty = {0}; cc_type(&it, &ty);
            v2 = cc_synth_var(c, n2, it); t2.b = v2;
            CgxDoc *d = cgx_dcatn(CD, cgx_dtextf(CD, "%s %s = ", ty.p, n2), cc_exw(c, t->b, &it, 0), cc_t(c, ";"), NULL); free(ty.p); in = cgx_dcatn(CD, in, cgx_dhard(CD), d, NULL);
        }
        bin.a = &t2; bin.b = s->b; CgxDoc *st = cc_store(c, &t2, &bin, NULL, &tt);
        if (!st) return NULL;
        in = cgx_dcatn(CD, in, cgx_dhard(CD), st, NULL);
        return cc_wrapblock(c, in);
    }
    bin.a = (CgxNode *)t; bin.b = s->b;
    return cc_store(c, t, &bin, NULL, &tt);
}

static CgxDoc *cc_try(CgxC *c, const CgxNode *s) {
    const int id = ++c->uniq, hc = s->flag, hf = (int)s->iv; const int base = c->nreg;
    if (base + 2 > 24) { cgx_err(c->g, s->line, "c: try blocks nested too deep (12 at most)"); return NULL; }
    CgxDoc *body, *els = NULL, *fin_inline;
    c->reg[c->nreg].active = 1; c->reg[c->nreg].fin = hf ? &s->b3 : NULL; c->nreg++;
    body = cc_block(c, &s->b1, NULL, cc_t(c, "cg_leave();"));
    c->nreg = base;
    fin_inline = hf ? cc_block(c, &s->b3, NULL, NULL) : NULL;
    if (hc) {
        CgxDoc *decl = NULL;
        if (s->s2) { CgxB nm = {0}; cc_name(s->s2, &nm); decl = cc_isused(c, s, s->s2) ? cgx_dtextf(CD, "CgStr %s = cg_err;", nm.p) : cgx_dtextf(CD, "CgStr %s = cg_err; (void)%s;", nm.p, nm.p); free(nm.p); }
        if (hf) {
            c->reg[c->nreg].active = 1; c->reg[c->nreg].fin = &s->b3; c->nreg++;
            CgxDoc *cb = cc_block(c, &s->b2, decl, cc_t(c, "cg_leave();"));
            c->nreg = base;
            CgxDoc *fin2 = cc_block(c, &s->b3, NULL, cc_t(c, "cg_throw(cg_err);"));
            CgxDoc *in = cgx_dcatn(CD, cgx_dhard(CD), cgx_dtextf(CD, "CgTry cgk%d;", id), cgx_dhard(CD), cgx_dtextf(CD, "cg_enter(&cgk%d);", id), cgx_dhard(CD),
                cgx_dtextf(CD, "if (setjmp(cgk%d.jb) == 0) ", id), cb, cc_t(c, " else "), fin2, NULL);
            els = cc_wrapblock(c, in);
        } else els = cc_block(c, &s->b2, decl, NULL);
    } else els = cc_block(c, &s->b3, NULL, cc_t(c, "cg_throw(cg_err);"));
    CgxDoc *in = cgx_dcatn(CD, cgx_dhard(CD), cgx_dtextf(CD, "CgTry cgj%d;", id), cgx_dhard(CD), cgx_dtextf(CD, "cg_enter(&cgj%d);", id), cgx_dhard(CD),
        cgx_dtextf(CD, "if (setjmp(cgj%d.jb) == 0) ", id), body, cc_t(c, " else "), els, NULL);
    if (hf) in = cgx_dcatn(CD, in, cgx_dhard(CD), fin_inline, NULL);
    return cc_wrapblock(c, in);
}

/* uma funcao ou metodo ou teste usa estas funcoes de estado */
static void cc_fn_begin(CgxC *c, const CgxNode *fn, const CgxNode *cls, int vol) {
    for (int i = 0; i < c->ntmp; i++) free(c->tmpty[i]);
    c->ntmp = 0; c->nreg = 0; c->nloop = 0; c->nused = 0; c->fn = fn; c->cls = cls; c->vol = vol;
}
static CgxDoc *cc_tmpdecls(CgxC *c) {
    CgxDoc *r = NULL;
    for (int i = 0; i < c->ntmp; i++) {
        char nm[24]; snprintf(nm, sizeof nm, "cgt%d", i + 1); char *d = cc_declt(c->tmpty[i], nm, c->vol);
        CgxDoc *x = cgx_dcatn(CD, cgx_dhard(CD), cgx_dtextf(CD, "%s;", d), NULL); free(d); r = r ? cgx_dcat(CD, r, x) : x;
    }
    return r;
}

static CgxDoc *cc_stmt(CgxC *c, const CgxNode *s) {
    CgxTy B = cc_ty(CGT_BOOL), I = cc_ty(CGT_INT); c->line = s->line;
    switch (s->k) {
        case CGN_COMMENT: case CGN_DOC: { if (!s->defs) return NULL; CgxB b = {0}; cgx_bs(&b, "/* "); for (const char *q = s->defs; *q; q++) { if (q[0] == '*' && q[1] == '/') { cgx_bs(&b, "* /"); q++; } else cgx_bn(&b, q, 1); } cgx_bs(&b, " */"); CgxDoc *d = cgx_dtext(CD, b.p); free(b.p); return d; }
        case CGN_RAW: {
            if (!s->raw[3]) { cgx_err(c->g, s->line, "c: this `raw` block has no `raw c` version (it has: %s%s%s)", s->raw[0] ? "python " : "", s->raw[1] ? "js " : "", s->raw[2] ? "ts" : ""); return NULL; }
            CgxDoc *r = NULL; const char *q = s->raw[3];
            while (*q) { const char *e = strchr(q, '\n'); size_t l = e ? (size_t)(e - q) : strlen(q); char *ln = (char *)malloc(l + 1); memcpy(ln, q, l); ln[l] = 0;
                CgxDoc *t = cgx_dtext(CD, ln); free(ln); r = r ? cgx_dcatn(CD, r, cgx_dhard(CD), t, NULL) : t; q += l + (e ? 1 : 0); }
            return r ? r : cgx_dnil(CD);
        }
        case CGN_LET: {
            CgxTy t = s->ty; if (cc_kind(&t) == CK_VOID || cc_kind(&t) == CK_NULL) t = cc_ty(CGT_ANY);
            CgxB nm = {0}; cc_name(s->s, &nm); char *dc = cc_decl(&t, nm.p, c->vol);
            CgxDoc *d = cgx_dcatn(CD, cgx_dtextf(CD, "%s = ", dc), cc_exw(c, s->a, &t, 0), cc_t(c, ";"), NULL);
            d = cc_voidname(c, d, nm.p, cc_isused(c, s, s->s)); free(dc); free(nm.p); return d;
        }
        case CGN_ASSIGN: return cc_assign(c, s);
        case CGN_EXPR: { CgxTy nat; int prec; CgxDoc *d = cc_ex0(c, s->a, &prec, &nat); return cgx_dcat(CD, d, cc_t(c, ";")); }
        case CGN_THROW: {
            const CgxTy t = cc_vty(s->a); CgxTy S = cc_ty(CGT_STR), A = cc_ty(CGT_ANY);
            CgxDoc *d = cc_exw(c, s->a, cc_kind(&t) == CK_STR ? &S : &A, 0);
            if (cc_kind(&t) != CK_STR) d = cc_pre(c, "cg_vstr(", d, ")");
            return cgx_dcatn(CD, cc_t(c, "cg_throw("), d, cc_t(c, ");"), NULL);
        }
        case CGN_RET: {
            if (!c->nreg) return s->a ? cgx_dcatn(CD, cc_t(c, "return "), cc_exw(c, s->a, &c->fn->ty, 0), cc_t(c, ";"), NULL) : cc_t(c, "return;");
            CgxDoc *un = cc_unwind(c, 0); const int id = ++c->uniq;
            if (!s->a) return cc_wrapblock(c, cgx_dcatn(CD, un ? un : cgx_dnil(CD), cgx_dhard(CD), cc_t(c, "return;"), NULL));
            CgxTy rt = c->fn->ty; CgxB ty = {0}; cc_type(&rt, &ty);
            CgxDoc *in = cgx_dcatn(CD, cgx_dhard(CD), cgx_dtextf(CD, "%s cgr%d = ", ty.p, id), cc_exw(c, s->a, &rt, 0), cc_t(c, ";"), un ? un : cgx_dnil(CD), cgx_dhard(CD), cgx_dtextf(CD, "return cgr%d;", id), NULL);
            free(ty.p); return cc_wrapblock(c, in);
        }
        case CGN_BREAK: case CGN_CONT: {
            const char *w = s->k == CGN_BREAK ? "break;" : "continue;";
            CgxDoc *un = c->nloop ? cc_unwind(c, c->lbase[c->nloop - 1]) : NULL;
            if (!un) return cc_t(c, w);
            return cc_wrapblock(c, cgx_dcatn(CD, un, cgx_dhard(CD), cc_t(c, w), NULL));
        }
        case CGN_IF: {
            CgxDoc *d = cgx_dcatn(CD, cc_t(c, "if ("), cc_exw(c, s->a, &B, 0), cc_t(c, ") "), cc_block(c, &s->b1, NULL, NULL), NULL);
            if (s->b2.n == 1 && s->b2.p[0]->k == CGN_IF && s->b2.p[0]->flag) d = cgx_dcatn(CD, d, cc_t(c, " else "), cc_stmt(c, s->b2.p[0]), NULL);
            else if (s->b2.n) d = cgx_dcatn(CD, d, cc_t(c, " else "), cc_block(c, &s->b2, NULL, NULL), NULL);
            return d;
        }
        case CGN_WHILE: {
            CgxDoc *cond = cc_exw(c, s->a, &B, 0); c->lbase[c->nloop++] = c->nreg;
            CgxDoc *d = cgx_dcatn(CD, cc_t(c, "while ("), cond, cc_t(c, ") "), cc_block(c, &s->b1, NULL, NULL), NULL); c->nloop--; return d;
        }
        case CGN_FOR: {
            const int id = ++c->uniq; const long long step = s->c ? s->c->iv : 1; CgxB nm = {0}; cc_name(s->s, &nm); const char *V_ = c->vol ? "volatile " : "";
            CgxDoc *a = cc_exw(c, s->a, &I, 0), *b = cc_exw(c, s->b, &I, 0);
            c->lbase[c->nloop++] = c->nreg;
            CgxDoc *pro = cgx_dtextf(CD, "long long %s = cgi%d;", nm.p, id);
            if (!cc_isused(c, s, s->s)) pro = cgx_dcatn(CD, pro, cgx_dtextf(CD, " (void)%s;", nm.p), NULL);
            CgxDoc *body = cc_block(c, &s->b1, pro, NULL); c->nloop--;
            CgxDoc *d = cgx_dcatn(CD, cgx_dtextf(CD, "for (%slong long cgi%d = ", V_, id), a, cgx_dtextf(CD, ", cge%d = ", id), b, cgx_dtextf(CD, "; cgi%d %s cge%d; cgi%d += %lld) ", id, step > 0 ? "<" : ">", id, id, step), body, NULL);
            free(nm.p); return d;
        }
        case CGN_EACH: {
            const int id = ++c->uniq; const CgxTy tc = cc_vty(s->a); const int kc = cc_kind(&tc); const char *V_ = c->vol ? "volatile " : ""; CgxB nm = {0}, nm2 = {0}; cc_name(s->s, &nm); if (s->s2) cc_name(s->s2, &nm2);
            CgxTy vt = cc_ty(CGT_ANY), kt = cc_ty(CGT_STR), cty; CgxDoc *head, *p1 = NULL, *p2 = NULL; char *dc;
            c->lbase[c->nloop++] = c->nreg;
            const char *lim;
            if (kc == CK_LIST) {
                cty = tc.e == CGT_ANY ? cc_listty(CGT_ANY) : tc; vt = cc_elemty(&tc);
                head = cgx_dcatn(CD, cgx_dtextf(CD, "CgList *%scgl%d = cg_lnn(", V_, id), cc_exw(c, s->a, &cty, 0), cc_t(c, ");"), NULL); lim = "cgl%d->n";
                dc = cc_decl(&vt, nm.p, c->vol);
                char acc[80]; if (cc_ek(tc.e) == 4) snprintf(acc, sizeof acc, "cg_lgetv(cgl%d, cgi%d)", id, id); else snprintf(acc, sizeof acc, "cg_l%sget(cgl%d, cgi%d)", cc_pfx(tc.e), id, id);
                p1 = cgx_dtextf(CD, "%s = %s;", dc, acc);
            } else if (kc == CK_STR) {
                vt = cc_ty(CGT_STR); CgxTy S = vt;
                head = cgx_dcatn(CD, cgx_dtextf(CD, "%sCgStr cgs%d = cg_snn(", V_, id), cc_exw(c, s->a, &S, 0), cc_t(c, ");"), NULL); lim = "cgs%d.n";
                dc = cc_decl(&vt, nm.p, c->vol); p1 = cgx_dtextf(CD, "%s = cg_sidx(cgs%d, cgi%d);", dc, id, id);
            } else if (kc == CK_MAP) {
                CgxTy mt = tc; lim = "cgm%d->n";
                head = cgx_dcatn(CD, cgx_dtextf(CD, "CgMap *%scgm%d = cg_mnn(", V_, id), cc_exw(c, s->a, &mt, 0), cc_t(c, ");"), NULL);
                dc = cc_decl(&kt, nm.p, c->vol); p1 = cgx_dtextf(CD, "%s = cgm%d->keys[cgi%d];", dc, id, id);
                if (s->s2) { vt = cc_elemty(&tc); char *dc2 = cc_decl(&vt, nm2.p, c->vol); CgxTy A = cc_ty(CGT_ANY); CgxDoc *raw = cgx_dtextf(CD, "cg_mvalat(cgm%d, cgi%d)", id, id);
                    p2 = cgx_dcatn(CD, cgx_dtextf(CD, "%s = ", dc2), cc_conv(c, raw, &A, &vt), cc_t(c, ";"), NULL); free(dc2); }
            } else {
                CgxTy A = cc_ty(CGT_ANY);
                head = cgx_dcatn(CD, cgx_dtextf(CD, "%sCgV cgv%d = ", V_, id), cc_exw(c, s->a, &A, 0), cc_t(c, ";"), NULL); lim = "cg_vlen(cgv%d)";
                dc = cc_decl(&A, nm.p, c->vol);
                if (s->s2) { char *dc2 = cc_decl(&A, nm2.p, c->vol); p1 = cgx_dtextf(CD, "%s = cg_vitem(cgv%d, cgi%d);", dc, id, id); p2 = cgx_dtextf(CD, "%s = cg_vitemv(cgv%d, cgi%d);", dc2, id, id); free(dc2); }
                else p1 = cgx_dtextf(CD, "%s = cg_vitem(cgv%d, cgi%d);", dc, id, id);
            }
            CgxDoc *pro = p1; if (p2) pro = cgx_dcatn(CD, pro, cgx_dhard(CD), p2, NULL);
            if (!cc_isused(c, s, s->s)) pro = cgx_dcatn(CD, pro, cgx_dtextf(CD, " (void)%s;", nm.p), NULL);
            if (s->s2 && !cc_isused(c, s, s->s2)) pro = cgx_dcatn(CD, pro, cgx_dtextf(CD, " (void)%s;", nm2.p), NULL);
            CgxDoc *body = cc_block(c, &s->b1, pro, NULL); c->nloop--;
            char lm[60]; snprintf(lm, sizeof lm, lim, id);
            CgxDoc *loop = cgx_dcatn(CD, cgx_dtextf(CD, "for (%slong long cgi%d = 0; cgi%d < %s; cgi%d++) ", V_, id, id, lm, id), body, NULL);
            free(dc); free(nm.p); free(nm2.p);
            return cc_wrapblock(c, cgx_dcatn(CD, cgx_dhard(CD), head, cgx_dhard(CD), loop, NULL));
        }
        case CGN_TRY: return cc_try(c, s);
    }
    cgx_err(c->g, s->line, "c: unexpected statement node %d", s->k);
    return NULL;
}

/* ---- funcoes, classes, testes ---- */
static CgxDoc *cc_head(CgxC *c, const char *ret, const char *name, char **pd, int n, CgxB *proto) {
    CgxDoc *it[64]; int k = 0;
    for (int i = 0; i < n && k < 64; i++) it[k++] = cgx_dtext(CD, pd[i]);
    if (proto) { cgx_bf(proto, "%s %s(", ret, name); if (!n) cgx_bs(proto, "void"); for (int i = 0; i < n; i++) { char *q = pd[i]; while (!strncmp(q, "volatile ", 9)) q += 9; cgx_bf(proto, "%s%s", i ? ", " : "", q); } cgx_bs(proto, ");\n"); }
    if (!n) it[k++] = cc_t(c, "void");
    return cgx_dcatn(CD, cgx_dtextf(CD, "%s %s", ret, name), cc_seq(c, "(", it, k, ")"), NULL);
}
static CgxDoc *cc_tail(CgxC *c, CgxDoc *head, CgxDoc *tmps, CgxDoc *pro, CgxDoc *body, CgxDoc *fallback) {
    CgxDoc *in = NULL;
    CgxDoc *parts[4] = {tmps, pro, body, fallback};
    for (int i = 0; i < 4; i++) if (parts[i]) in = in ? cgx_dcat(CD, in, parts[i]) : parts[i];
    if (!in) return cgx_dcatn(CD, head, cc_t(c, " { }"), NULL);
    return cgx_dcatn(CD, head, cc_t(c, " {"), cgx_dnest(CD, c->o->indent, in), cgx_dhard(CD), cc_t(c, "}"), NULL);
}
static CgxDoc *cc_func(CgxC *c, const CgxNode *fn, const CgxNode *cls, CgxB *proto) {
    const int vol = cc_has_try(&fn->b1);
    cc_fn_begin(c, fn, cls, vol);
    for (int i = 0; i < fn->b1.n; i++) cc_scan(c, fn->b1.p[i]);
    char *pd[44]; int np = 0; CgxDoc *pro = NULL;
    if (cls) { CgxTy st = cgx_t(CGT_CLASS); snprintf(st.cls, sizeof st.cls, "%s", cls->s); pd[np++] = cc_decl(&st, "self", 0); if (!cc_isused(c, fn, "self")) pro = cgx_dtext(CD, "(void)self;"); }
    for (int i = 0; i < fn->npar && np < 43; i++) {
        const CgxParam *p = &fn->par[i]; CgxB nm = {0}; cc_name(p->name, &nm); CgxTy pt = p->ty;
        if (cc_kind(&pt) == CK_VOID || cc_kind(&pt) == CK_NULL) pt = cc_ty(CGT_ANY);
        pd[np++] = cc_decl(&pt, nm.p, vol);
        if (!cc_isused(c, fn, p->name)) { CgxDoc *x = cgx_dtextf(CD, "(void)%s;", nm.p); pro = pro ? cgx_dcatn(CD, pro, cgx_dhard(CD), x, NULL) : x; }
        free(nm.p);
    }
    CgxB rt = {0}, fname = {0}; CgxTy rty = fn->ty; cc_type(&rty, &rt); cc_fname(fn, cls, &fname);
    CgxDoc *head = cc_head(c, rt.p, fname.p, pd, np, proto);
    CgxDoc *body = cc_stmts(c, &fn->b1, NULL);
    CgxDoc *tmps = cc_tmpdecls(c);
    if (pro) pro = cgx_dcat(CD, cgx_dhard(CD), pro);
    CgxDoc *fb = NULL;
    if (cc_kind(&rty) != CK_VOID && (vol || (fn->b1.n && fn->b1.p[fn->b1.n - 1]->k == CGN_RAW))) fb = cgx_dcat(CD, cgx_dhard(CD), cgx_dtextf(CD, "return %s;", cc_zero(&rty)));
    CgxDoc *out = cc_tail(c, head, tmps, pro, body, fb);
    for (int i = 0; i < np; i++) free(pd[i]);
    free(rt.p); free(fname.p); return out;
}
static CgxDoc *cc_ctor(CgxC *c, const CgxNode *cls, CgxB *proto) {
    const CgxNode *init = NULL; for (int i = 0; i < cls->b1.n; i++) if (cls->b1.p[i]->k == CGN_FN && !strcmp(cls->b1.p[i]->s, "init")) init = cls->b1.p[i];
    cc_fn_begin(c, NULL, cls, 0);
    char *pd[44]; int np = 0; CgxDoc *body; CgxB nmb = {0}; CgxB args = {0};
    for (int i = 0; init && i < init->npar && np < 43; i++) {
        const CgxParam *p = &init->par[i]; CgxB nm = {0}; cc_name(p->name, &nm); CgxTy pt = p->ty;
        if (cc_kind(&pt) == CK_VOID || cc_kind(&pt) == CK_NULL) pt = cc_ty(CGT_ANY);
        pd[np++] = cc_decl(&pt, nm.p, 0); cgx_bf(&args, ", %s", nm.p); free(nm.p);
    }
    cgx_bf(&nmb, "N_%s", cls->s);
    CgxB rt = {0}; cgx_bf(&rt, "C_%s *", cls->s);
    CgxDoc *head = cc_head(c, rt.p, nmb.p, pd, np, proto);
    body = cgx_dcatn(CD, cgx_dhard(CD), cgx_dtextf(CD, "C_%s *self = (C_%s *)cg_alloc(sizeof *self);", cls->s, cls->s), NULL);
    for (int i = 0; i < cls->b1.n; i++) {
        const CgxNode *f = cls->b1.p[i]; if (f->k != CGN_FIELDDECL) continue;
        CgxTy ft = f->ty; if (cc_kind(&ft) == CK_VOID) ft = cc_ty(CGT_ANY);
        CgxB fnm = {0}; cc_name(f->s, &fnm);
        CgxDoc *v = f->a ? cc_exw(c, f->a, &ft, 0) : cgx_dtext(CD, cc_kind(&ft) == CK_STR ? "CG_S(\"\")" : cc_kind(&ft) == CK_LIST ? "cg_lnew(CG_EK_ANY)" : cc_kind(&ft) == CK_MAP ? "cg_mnew(CG_EK_ANY)" : cc_zero(&ft));
        if (!f->a && cc_kind(&ft) == CK_LIST) v = cgx_dtextf(CD, "cg_lnew(%s)", cc_ekname(ft.e));
        if (!f->a && cc_kind(&ft) == CK_MAP) v = cgx_dtextf(CD, "cg_mnew(%s)", cc_ekname(ft.e));
        body = cgx_dcatn(CD, body, cgx_dhard(CD), cgx_dtextf(CD, "self->%s = ", fnm.p), v, cc_t(c, ";"), NULL); free(fnm.p);
    }
    if (init) body = cgx_dcatn(CD, body, cgx_dhard(CD), cgx_dtextf(CD, "M_%s__init(self%s);", cls->s, args.p ? args.p : ""), NULL);
    body = cgx_dcatn(CD, body, cgx_dhard(CD), cc_t(c, "return self;"), NULL);
    CgxDoc *out = cc_tail(c, head, cc_tmpdecls(c), NULL, body, NULL);
    for (int i = 0; i < np; i++) free(pd[i]);
    free(rt.p); free(nmb.p); free(args.p); return out;
}
static CgxDoc *cc_test(CgxC *c, const CgxNode *t, int idx, CgxB *proto) {
    const CgxTy A = cc_ty(CGT_ANY); const int mode = (int)t->iv;
    cc_fn_begin(c, NULL, NULL, 1);
    CgxDoc *try_part;
    if (mode == 2) {
        int prec; CgxTy nat; CgxDoc *e = cc_ex0(c, t->a, &prec, &nat);
        try_part = cgx_dcatn(CD, cgx_dhard(CD), cgx_dcat(CD, e, cc_t(c, ";")), cgx_dhard(CD), cc_t(c, "cg_leave();"), cgx_dhard(CD), cgx_dtextf(CD, "printf(\"test %d: FAIL did not raise\\n\");", idx), NULL);
    } else {
        CgxDoc *g = cc_exw(c, t->a, &A, 0), *x = t->b ? cc_exw(c, t->b, &A, 0) : cc_t(c, "cg_nil()");
        const char *chk = mode == 1 ? "fabs(cg_uf(cgg) - cg_uf(cge)) < 1e-9" : mode == 3 ? "cgg.t == CG_BOOL && cgg.u.i" : "cg_veq(cgg, cge)";
        try_part = cgx_dcatn(CD, cgx_dhard(CD), cc_t(c, "CgV cgg = "), g, cc_t(c, ";"), cgx_dhard(CD), cc_t(c, "CgV cge = "), x, cc_t(c, ";"), cgx_dhard(CD), cc_t(c, "cg_leave();"),
            cgx_dhard(CD), cgx_dtextf(CD, "if (%s) printf(\"test %d: ok\\n\");", chk, idx), cgx_dhard(CD), cgx_dtextf(CD, "else cg_tfail(%d, cgg, cge);", idx), NULL);
    }
    CgxDoc *els = mode == 2 ? cgx_dcatn(CD, cgx_dhard(CD), cgx_dtextf(CD, "printf(\"test %d: ok\\n\");", idx), NULL)
                            : cgx_dcatn(CD, cgx_dhard(CD), cgx_dtextf(CD, "printf(\"test %d: FAIL raised %%.*s\\n\", (int)cg_err.n, cg_err.p);", idx), NULL);
    CgxDoc *body = cgx_dcatn(CD, cgx_dhard(CD), cc_t(c, "CgTry cgj;"), cgx_dhard(CD), cc_t(c, "cg_enter(&cgj);"), cgx_dhard(CD), cc_t(c, "if (setjmp(cgj.jb) == 0) {"),
        cgx_dnest(CD, c->o->indent, try_part), cgx_dhard(CD), cc_t(c, "} else {"), cgx_dnest(CD, c->o->indent, els), cgx_dhard(CD), cc_t(c, "}"), NULL);
    char nm[32]; snprintf(nm, sizeof nm, "cg_test_%d", idx);
    CgxDoc *head = cc_head(c, "static void", nm, NULL, 0, proto);
    return cc_tail(c, head, cc_tmpdecls(c), NULL, body, NULL);
}

/* ---- o programa ---- */
static void cc_rt(CgxB *o, const char *const *sec) { for (int i = 0; sec[i]; i++) { cgx_bs(o, sec[i]); cgx_bs(o, "\n"); } }
static int cgx_gen_c(Cgx *g, const CgxGenOpts *o, CgxB *code, CgxB *tests) {
    CgxC C; memset(&C, 0, sizeof C); C.g = g; C.o = o; CgxC *c = &C;
    const CgxNode *prog = g->prog; const int err0 = g->nerr_all;
    CgxB protos = {0}, structs = {0}, globals = {0}, defs = {0}, initb = {0}, tb = {0};
    c->prog = prog;
    for (int i = 0; i < prog->b1.n; i++) if (prog->b1.p[i]->k == CGN_CLASS) { if (c->ncls >= 40) { cgx_err(g, prog->b1.p[i]->line, "c: too many classes (40 at most)"); break; } c->cls_list[c->ncls++] = prog->b1.p[i]; }
    /* classes: struct e construtor */
    for (int k = 0; k < c->ncls; k++) {
        const CgxNode *cl = c->cls_list[k];
        cgx_bf(&structs, "typedef struct C_%s C_%s;\n", cl->s, cl->s);
    }
    for (int k = 0; k < c->ncls; k++) {
        const CgxNode *cl = c->cls_list[k]; int nf = 0;
        cgx_bf(&structs, "struct C_%s {\n", cl->s);
        for (int i = 0; i < cl->b1.n; i++) if (cl->b1.p[i]->k == CGN_FIELDDECL) {
            CgxTy ft = cl->b1.p[i]->ty; if (cc_kind(&ft) == CK_VOID) ft = cc_ty(CGT_ANY);
            CgxB nm = {0}; cc_name(cl->b1.p[i]->s, &nm); char *d = cc_decl(&ft, nm.p, 0); cgx_bf(&structs, "    %s;\n", d); free(d); free(nm.p); nf++;
        }
        if (!nf) cgx_bs(&structs, "    int cg_unused;\n");
        cgx_bs(&structs, "};\n");
    }
    for (int i = 0; i < prog->b1.n; i++) {
        const CgxNode *d = prog->b1.p[i]; CgxDoc *x = NULL;
        if (d->k == CGN_FN) x = cc_func(c, d, NULL, &protos);
        else if (d->k == CGN_CLASS) {
            CgxDoc *all = cc_ctor(c, d, &protos);
            for (int j = 0; j < d->b1.n; j++) if (d->b1.p[j]->k == CGN_FN) all = cgx_dcatn(CD, all, cgx_dhard(CD), cgx_dhard(CD), cc_func(c, d->b1.p[j], d, &protos), NULL);
            x = all;
        } else if (d->k == CGN_LET) {
            CgxTy t = d->ty; if (cc_kind(&t) == CK_VOID || cc_kind(&t) == CK_NULL) t = cc_ty(CGT_ANY);
            CgxB nm = {0}; cgx_bf(&nm, "u_%s", d->s); char *dc = cc_decl(&t, nm.p, 0); cgx_bf(&globals, "static %s;\n", dc); free(dc);
            cc_fn_begin(c, NULL, NULL, 0);
            CgxDoc *v = cgx_dcatn(CD, cgx_dhard(CD), cgx_dtextf(CD, "%s = ", nm.p), cc_exw(c, d->a, &t, 0), cc_t(c, ";"), NULL);
            CgxDoc *tm = cc_tmpdecls(c);
            CgxDoc *blk = cgx_dcatn(CD, cgx_dhard(CD), cc_t(c, "{"), cgx_dnest(CD, o->indent, tm ? cgx_dcat(CD, tm, v) : v), cgx_dhard(CD), cc_t(c, "}"), NULL);
            cgx_render(blk, o->width, &initb); cgx_bs(&initb, "\n"); free(nm.p);
        } else if (d->k == CGN_RAW) { cc_fn_begin(c, NULL, NULL, 0); x = cc_stmt(c, d); }
        else if (d->k == CGN_COMMENT && o->comments && d->defs) x = cc_stmt(c, d);
        if (x) { cgx_render(x, o->width, &defs); cgx_bs(&defs, "\n\n"); }
    }
    /* testes */
    int nt = 0;
    if (tests) {
        for (int i = 0; i < prog->b1.n; i++) if (prog->b1.p[i]->k == CGN_TEST) {
            CgxDoc *x = cc_test(c, prog->b1.p[i], ++nt, NULL);
            cgx_render(x, o->width, &tb); cgx_bs(&tb, "\n\n");
        }
        cgx_bs(&tb, "int main(void) {\n    CgTry cgm;\n    cg_enter(&cgm);\n    if (setjmp(cgm.jb) == 0) { cg_init(); cg_leave(); }\n    else { printf(\"init FAIL %.*s\\n\", (int)cg_err.n, cg_err.p); cg_arena_free(); return 1; }\n");
        for (int i = 1; i <= nt; i++) cgx_bf(&tb, "    cg_test_%d();\n", i);
        cgx_bs(&tb, "    cg_arena_free();\n    return 0;\n}\n");
    }
    cc_dynemit(c, &protos, &defs);
    if (g->nerr_all == err0) {
        cgx_bs(code, "/* Generated by UCGE (C backend). Build: gcc -std=c99 -Wall -Wextra -Wpedantic file.c -lm. Memory: one arena, freed by cg_arena_free(). */\n");
        cc_rt(code, CGX_C_RT_CORE); cc_rt(code, CGX_C_RT_COLL); if (c->need_re) cc_rt(code, CGX_C_RT_RE);
        cgx_bs(code, "\n");
        if (structs.p) { cgx_bs(code, structs.p); cgx_bs(code, "\n"); }
        if (protos.p) { cgx_bs(code, protos.p); cgx_bs(code, "\n"); }
        if (globals.p) { cgx_bs(code, globals.p); cgx_bs(code, "\n"); }
        if (defs.p) cgx_bs(code, defs.p);
        cgx_bs(code, "void cg_init(void) {\n"); if (initb.p) cgx_bs(code, initb.p); cgx_bs(code, "}\n");
        if (tests && tb.p) cgx_bs(tests, tb.p);
    }
    free(protos.p); free(structs.p); free(globals.p); free(defs.p); free(initb.p); free(tb.p);
    for (int i = 0; i < c->ntmp; i++) free(c->tmpty[i]);
    free(c->tmpty); free(c->used); cgx_docs_free(&c->D);
    return g->nerr_all == err0;
}
