/* ---------- coder/ucgetype.c: UCGE, tabela de tipos e unificacao (docs/ucge.md, "Sistema de tipos") ----------
 * Base da inferencia da fase 2 (Hindley-Milner com let-polimorfismo por niveis e extensao gradual). O validador da fase 1 (ucgecheck.c) ainda usa o
 * CgxTy simples; a fase 2 troca-o por estes tipos. Aqui ja ficam prontos e testados (`ucgecli selftest`):
 *   - tabela de tipos com hash-consing: cada tipo estrutural tem UM id (CgxTid), comparar tipos iguais e comparar inteiros;
 *   - variaveis de tipo com union-find (caminho comprimido), nivel para generalizar (Remy) e verificacao de ocorrencia;
 *   - o tipo gradual `any`: unifica com tudo sem ligar nada (compativel nos dois sentidos, como em linguagens dinamicas);
 *   - esquemas (forall) e instanciacao; o subtipo unico int <: float em cgx_ty_assignable;
 *   - tuplas, funcoes, opcionais (T | null), unioes (conjunto ordenado e sem repeticao) e classes nomeadas. */
enum { TYK_VAR, TYK_ANY, TYK_INT, TYK_FLOAT, TYK_STR, TYK_BOOL, TYK_VOID, TYK_NEVER, TYK_LIST, TYK_MAP, TYK_TUPLE, TYK_FN, TYK_OPT, TYK_UNION, TYK_CLASS, TYK_GEN };
typedef int CgxTid;
typedef struct { int k; int n; CgxTid *a; int sym; CgxTid link; int level; } CgxTyNode;       /* a: argumentos (FN: parametros e depois o retorno); sym: nome da classe ou indice do TYK_GEN */
typedef struct { CgxTyNode *t; int n, cap; int *tab, tabn; int level; } CgxTypes;

static unsigned cgx_ty_hash(int k, const CgxTid *a, int n, int sym) { unsigned h = 2166136261u ^ (unsigned)k; h = (h ^ (unsigned)sym) * 16777619u; for (int i = 0; i < n; i++) h = (h ^ (unsigned)a[i]) * 16777619u; return h; }
static void cgx_ty_init(CgxTypes *T) { memset(T, 0, sizeof *T); T->cap = 64; T->t = (CgxTyNode *)calloc((size_t)T->cap, sizeof *T->t); T->n = 1; T->tabn = 256; T->tab = (int *)calloc((size_t)T->tabn, sizeof(int)); }
static void cgx_ty_free(CgxTypes *T) { for (int i = 1; i < T->n; i++) free(T->t[i].a); free(T->t); free(T->tab); memset(T, 0, sizeof *T); }
static CgxTid cgx_ty_new(CgxTypes *T, int k, const CgxTid *a, int n, int sym) {
    if (T->n == T->cap) { T->cap *= 2; T->t = (CgxTyNode *)realloc(T->t, sizeof *T->t * (size_t)T->cap); }
    CgxTyNode *x = &T->t[T->n]; memset(x, 0, sizeof *x); x->k = k; x->n = n; x->sym = sym;
    if (n) { x->a = (CgxTid *)malloc(sizeof(CgxTid) * (size_t)n); memcpy(x->a, a, sizeof(CgxTid) * (size_t)n); }
    return T->n++;
}
/* um tipo estrutural sem variavel livre mutavel: devolve o mesmo id para a mesma estrutura */
static CgxTid cgx_ty_cons(CgxTypes *T, int k, const CgxTid *a, int n, int sym) {
    if (T->n * 2 >= T->tabn) {
        const int nn = T->tabn * 2; int *t = (int *)calloc((size_t)nn, sizeof(int));
        for (int i = 1; i < T->n; i++) if (T->t[i].k != TYK_VAR) { unsigned h = cgx_ty_hash(T->t[i].k, T->t[i].a, T->t[i].n, T->t[i].sym) % (unsigned)nn; while (t[h]) h = (h + 1) % (unsigned)nn; t[h] = i; }
        free(T->tab); T->tab = t; T->tabn = nn;
    }
    unsigned h = cgx_ty_hash(k, a, n, sym) % (unsigned)T->tabn;
    while (T->tab[h]) {
        const CgxTyNode *x = &T->t[T->tab[h]];
        if (x->k == k && x->n == n && x->sym == sym && (!n || !memcmp(x->a, a, sizeof(CgxTid) * (size_t)n))) return T->tab[h];
        h = (h + 1) % (unsigned)T->tabn;
    }
    const CgxTid id = cgx_ty_new(T, k, a, n, sym);
    T->tab[h] = id;
    return id;
}
static CgxTid cgx_ty_prim(CgxTypes *T, int k) { return cgx_ty_cons(T, k, NULL, 0, 0); }
static CgxTid cgx_ty_list(CgxTypes *T, CgxTid e) { return cgx_ty_cons(T, TYK_LIST, &e, 1, 0); }
static CgxTid cgx_ty_map(CgxTypes *T, CgxTid v) { return cgx_ty_cons(T, TYK_MAP, &v, 1, 0); }             /* as chaves sao sempre str */
static CgxTid cgx_ty_opt(CgxTypes *T, CgxTid e) { return cgx_ty_cons(T, TYK_OPT, &e, 1, 0); }
static CgxTid cgx_ty_class(CgxTypes *T, int sym) { return cgx_ty_cons(T, TYK_CLASS, NULL, 0, sym); }
static CgxTid cgx_ty_tuple(CgxTypes *T, const CgxTid *a, int n) { return cgx_ty_cons(T, TYK_TUPLE, a, n, 0); }
static CgxTid cgx_ty_fn(CgxTypes *T, const CgxTid *params, int np, CgxTid ret) {
    CgxTid a[40]; if (np > 39) np = 39;
    memcpy(a, params, sizeof(CgxTid) * (size_t)np); a[np] = ret;
    return cgx_ty_cons(T, TYK_FN, a, np + 1, 0);
}
static CgxTid cgx_ty_var(CgxTypes *T) { const CgxTid id = cgx_ty_new(T, TYK_VAR, NULL, 0, 0); T->t[id].level = T->level; T->t[id].link = 0; return id; }
static CgxTid cgx_ty_gen(CgxTypes *T, int idx) { return cgx_ty_cons(T, TYK_GEN, NULL, 0, idx); }
static int cgx_ty_cmp(const void *x, const void *y) { return *(const CgxTid *)x - *(const CgxTid *)y; }
/* uniao: achata unioes, tira repetidos, ordena por id; um membro so vira ele mesmo; any absorve */
static CgxTid cgx_ty_union(CgxTypes *T, const CgxTid *a, int n) {
    CgxTid m[64]; int k = 0;
    for (int i = 0; i < n && k < 60; i++) {
        const CgxTyNode *x = &T->t[a[i]];
        if (x->k == TYK_ANY) return a[i];
        if (x->k == TYK_UNION) { for (int j = 0; j < x->n && k < 60; j++) m[k++] = x->a[j]; } else m[k++] = a[i];
    }
    qsort(m, (size_t)k, sizeof(CgxTid), cgx_ty_cmp);
    int w = 0; for (int i = 0; i < k; i++) if (!w || m[w - 1] != m[i]) m[w++] = m[i];
    if (w == 1) return m[0];
    return cgx_ty_cons(T, TYK_UNION, m, w, 0);
}
/* representante de uma variavel (union-find com compressao de caminho) */
static CgxTid cgx_ty_find(CgxTypes *T, CgxTid t) {
    CgxTid r = t;
    while (T->t[r].k == TYK_VAR && T->t[r].link) r = T->t[r].link;
    while (T->t[t].k == TYK_VAR && T->t[t].link && T->t[t].link != r) { const CgxTid nx = T->t[t].link; T->t[t].link = r; t = nx; }
    return r;
}
/* a variavel v ocorre em t? ao mesmo tempo baixa o nivel das variaveis de t ao de v (necessario para generalizar certo) */
static int cgx_ty_occurs(CgxTypes *T, CgxTid v, CgxTid t) {
    t = cgx_ty_find(T, t);
    if (t == v) return 1;
    if (T->t[t].k == TYK_VAR) { if (T->t[t].level > T->t[v].level) T->t[t].level = T->t[v].level; return 0; }
    for (int i = 0; i < T->t[t].n; i++) if (cgx_ty_occurs(T, v, T->t[t].a[i])) return 1;
    return 0;
}
/* unifica a e b; 0 se incompativeis (nada fica ligado pela metade: a unificacao so liga variaveis). any unifica com tudo sem ligar. */
static int cgx_ty_unify(CgxTypes *T, CgxTid a, CgxTid b) {
    a = cgx_ty_find(T, a); b = cgx_ty_find(T, b);
    if (a == b) return 1;
    const CgxTyNode *x = &T->t[a], *y = &T->t[b];
    if (x->k == TYK_ANY || y->k == TYK_ANY) return 1;
    if (x->k == TYK_VAR) { if (cgx_ty_occurs(T, a, b)) return 0; T->t[a].link = b; return 1; }
    if (y->k == TYK_VAR) { if (cgx_ty_occurs(T, b, a)) return 0; T->t[b].link = a; return 1; }
    if (x->k == TYK_OPT && y->k != TYK_OPT) return cgx_ty_unify(T, x->a[0], b);       /* T | null aceita T */
    if (y->k == TYK_OPT && x->k != TYK_OPT) return cgx_ty_unify(T, a, y->a[0]);
    if (x->k != y->k || x->n != y->n || x->sym != y->sym) return 0;
    for (int i = 0; i < x->n; i++) if (!cgx_ty_unify(T, T->t[a].a[i], T->t[b].a[i])) return 0;
    return 1;
}
/* o valor de tipo s pode ir num lugar de tipo d? (unificacao mais int <: float e T <: T|null; nao liga variaveis do lado de s que ja sao fixas) */
static int cgx_ty_assignable(CgxTypes *T, CgxTid d, CgxTid s) {
    d = cgx_ty_find(T, d); s = cgx_ty_find(T, s);
    if (T->t[d].k == TYK_FLOAT && T->t[s].k == TYK_INT) return 1;
    if (T->t[d].k == TYK_OPT && T->t[s].k == TYK_VOID) return 1;
    return cgx_ty_unify(T, d, s);
}
/* generalizacao: variaveis com nivel maior que o atual viram TYK_GEN(i); *nq recebe quantas */
static CgxTid cgx_ty_gen_walk(CgxTypes *T, CgxTid t, CgxTid *seen, int *nq) {
    t = cgx_ty_find(T, t);
    const CgxTyNode *x = &T->t[t];
    if (x->k == TYK_VAR) {
        if (x->level <= T->level) return t;
        for (int i = 0; i < *nq; i++) if (seen[i] == t) return cgx_ty_gen(T, i);
        seen[*nq] = t; return cgx_ty_gen(T, (*nq)++);
    }
    if (!x->n) return t;
    CgxTid a[64]; const int n = x->n > 64 ? 64 : x->n;
    for (int i = 0; i < n; i++) a[i] = cgx_ty_gen_walk(T, T->t[t].a[i], seen, nq);
    return cgx_ty_cons(T, T->t[t].k, a, n, T->t[t].sym);
}
static CgxTid cgx_ty_generalize(CgxTypes *T, CgxTid t, int *nq) { CgxTid seen[32]; *nq = 0; return cgx_ty_gen_walk(T, t, seen, nq); }
static CgxTid cgx_ty_inst_walk(CgxTypes *T, CgxTid t, CgxTid *fresh, int nq) {
    t = cgx_ty_find(T, t);
    const CgxTyNode *x = &T->t[t];
    if (x->k == TYK_GEN) return x->sym < nq ? fresh[x->sym] : t;
    if (!x->n) return t;
    CgxTid a[64]; const int n = x->n > 64 ? 64 : x->n;
    for (int i = 0; i < n; i++) a[i] = cgx_ty_inst_walk(T, T->t[t].a[i], fresh, nq);
    return cgx_ty_cons(T, T->t[t].k, a, n, T->t[t].sym);
}
static CgxTid cgx_ty_instantiate(CgxTypes *T, CgxTid scheme, int nq) { CgxTid fresh[32]; if (nq > 32) nq = 32; for (int i = 0; i < nq; i++) fresh[i] = cgx_ty_var(T); return cgx_ty_inst_walk(T, scheme, fresh, nq); }

static void cgx_ty_str(const CgxTypes *T, CgxTid t, const Cgx *g, CgxB *o) {
    static const char *const nm[] = {"?", "any", "int", "float", "str", "bool", "void", "never", "", "", "", "", "", "", "", ""};
    while (T->t[t].k == TYK_VAR && T->t[t].link) t = T->t[t].link;
    const CgxTyNode *x = &T->t[t];
    switch (x->k) {
        case TYK_VAR: cgx_bf(o, "'t%d", t); break;
        case TYK_GEN: cgx_bf(o, "'%c", 'a' + x->sym % 26); break;
        case TYK_LIST: cgx_bs(o, "list["); cgx_ty_str(T, x->a[0], g, o); cgx_bs(o, "]"); break;
        case TYK_MAP: cgx_bs(o, "map[str,"); cgx_ty_str(T, x->a[0], g, o); cgx_bs(o, "]"); break;
        case TYK_OPT: cgx_ty_str(T, x->a[0], g, o); cgx_bs(o, "?"); break;
        case TYK_CLASS: cgx_bs(o, g ? cgx_symname(g, x->sym) : "class"); break;
        case TYK_TUPLE: cgx_bs(o, "("); for (int i = 0; i < x->n; i++) { if (i) cgx_bs(o, ", "); cgx_ty_str(T, x->a[i], g, o); } cgx_bs(o, ")"); break;
        case TYK_UNION: for (int i = 0; i < x->n; i++) { if (i) cgx_bs(o, " | "); cgx_ty_str(T, x->a[i], g, o); } break;
        case TYK_FN: cgx_bs(o, "("); for (int i = 0; i + 1 < x->n; i++) { if (i) cgx_bs(o, ", "); cgx_ty_str(T, x->a[i], g, o); } cgx_bs(o, ") -> "); cgx_ty_str(T, x->a[x->n - 1], g, o); break;
        default: cgx_bs(o, nm[x->k]);
    }
}
