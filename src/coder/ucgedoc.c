/* ---------- coder/ucgedoc.c: UCGE, pretty-printer por algebra de documentos (docs/ucge.md, "Geracao") ----------
 * Wadler/Lindig ("Strictly Pretty"): o gerador monta um DOCUMENTO (texto, linha, aninhamento, grupo) em vez de concatenar strings; o renderizador
 * escolhe, grupo a grupo, entre a forma plana (cabe na largura) e a quebrada. Assim a quebra de chamadas, listas e cabecalhos longos e a
 * indentacao saem corretas em qualquer linguagem de destino, e o mesmo gerador serve para largura 80, 100 ou 120.
 *   dtext(s)        texto sem quebra (copiado)
 *   dline()         espaco quando o grupo cabe; senao quebra de linha na indentacao atual
 *   dsoft()         nada quando cabe; senao quebra de linha
 *   dhard()         quebra obrigatoria (forca o grupo que a contem a quebrar)
 *   dcat(a, b)      a seguido de b;  dnest(n, d): indentacao + n dentro de d;  dgroup(d): tenta caber numa linha
 * Os documentos vivem num CgxDocs (arena): solta tudo de uma vez. A indentacao so e escrita quando ha texto (sem espacos no fim das linhas). */
enum { DK_NIL, DK_TEXT, DK_LINE, DK_SOFT, DK_HARD, DK_CAT, DK_NEST, DK_GROUP };
typedef struct CgxDoc CgxDoc;
struct CgxDoc { int k, n; char *s; CgxDoc *a, *b; };
typedef struct { void **p; int n, cap; } CgxDocs;
static CgxDoc *cgx_dnew(CgxDocs *D, int k) {
    CgxDoc *d = (CgxDoc *)calloc(1, sizeof *d); d->k = k;
    if (D->n == D->cap) { D->cap = D->cap ? D->cap * 2 : 256; D->p = (void **)realloc(D->p, sizeof(void *) * (size_t)D->cap); }
    D->p[D->n++] = d; return d;
}
static void cgx_docs_free(CgxDocs *D) { for (int i = 0; i < D->n; i++) { free(((CgxDoc *)D->p[i])->s); free(D->p[i]); } free(D->p); memset(D, 0, sizeof *D); }
static CgxDoc *cgx_dtext(CgxDocs *D, const char *s) { CgxDoc *d = cgx_dnew(D, DK_TEXT); d->s = strdup(s); d->n = (int)strlen(s); return d; }
static CgxDoc *cgx_dtextf(CgxDocs *D, const char *fmt, ...) { char t[1024]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap); return cgx_dtext(D, t); }
static CgxDoc *cgx_dline(CgxDocs *D) { return cgx_dnew(D, DK_LINE); }
static CgxDoc *cgx_dsoft(CgxDocs *D) { return cgx_dnew(D, DK_SOFT); }
static CgxDoc *cgx_dhard(CgxDocs *D) { return cgx_dnew(D, DK_HARD); }
static CgxDoc *cgx_dnil(CgxDocs *D) { return cgx_dnew(D, DK_NIL); }
static CgxDoc *cgx_dcat(CgxDocs *D, CgxDoc *a, CgxDoc *b) { CgxDoc *d = cgx_dnew(D, DK_CAT); d->a = a; d->b = b; return d; }
static CgxDoc *cgx_dnest(CgxDocs *D, int n, CgxDoc *x) { CgxDoc *d = cgx_dnew(D, DK_NEST); d->n = n; d->a = x; return d; }
static CgxDoc *cgx_dgroup(CgxDocs *D, CgxDoc *x) { CgxDoc *d = cgx_dnew(D, DK_GROUP); d->a = x; return d; }
/* concatena varios (terminado em NULL) */
static CgxDoc *cgx_dcatn(CgxDocs *D, ...) {
    va_list ap; va_start(ap, D); CgxDoc *r = NULL, *x;
    while ((x = va_arg(ap, CgxDoc *)) != NULL) r = r ? cgx_dcat(D, r, x) : x;
    va_end(ap);
    return r ? r : cgx_dnil(D);
}

typedef struct { int i, flat; const CgxDoc *d; } CgxItem;
typedef struct { CgxItem *p; int n, cap; } CgxStk;
static void cgx_spush(CgxStk *s, int i, int flat, const CgxDoc *d) {
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 128; s->p = (CgxItem *)realloc(s->p, sizeof(CgxItem) * (size_t)s->cap); }
    s->p[s->n].i = i; s->p[s->n].flat = flat; s->p[s->n].d = d; s->n++;
}
/* cabe o resto na largura w (ate a primeira quebra permitida)? */
static int cgx_fits(int w, const CgxStk *main, const CgxItem *first) {
    CgxStk t = {0}; cgx_spush(&t, first->i, first->flat, first->d);
    int mi = main->n, ok = 1;
    while (w >= 0) {
        CgxItem it;
        if (t.n) it = t.p[--t.n];
        else if (mi > 0) it = main->p[--mi];
        else break;
        const CgxDoc *d = it.d;
        switch (d->k) {
            case DK_NIL: break;
            case DK_TEXT: w -= d->n; break;
            case DK_LINE: if (it.flat) w -= 1; else goto done; break;
            case DK_SOFT: if (!it.flat) goto done; break;
            case DK_HARD: if (it.flat) ok = 0; goto done;
            case DK_CAT: cgx_spush(&t, it.i, it.flat, d->b); cgx_spush(&t, it.i, it.flat, d->a); break;
            case DK_NEST: cgx_spush(&t, it.i + d->n, it.flat, d->a); break;
            case DK_GROUP: cgx_spush(&t, it.i, it.flat, d->a); break;
        }
    }
done:
    free(t.p);
    return ok && w >= 0;
}
/* escreve o documento em o, com largura width */
static void cgx_render(const CgxDoc *root, int width, CgxB *o) {
    CgxStk s = {0}; int k = 0, pend = 0;
    cgx_spush(&s, 0, 0, root);
    while (s.n) {
        CgxItem it = s.p[--s.n]; const CgxDoc *d = it.d;
        switch (d->k) {
            case DK_NIL: break;
            case DK_TEXT: if (d->n) { for (; pend > 0; pend--) cgx_bn(o, " ", 1); cgx_bn(o, d->s, (size_t)d->n); k += d->n; } break;
            case DK_LINE: if (it.flat) { for (; pend > 0; pend--) cgx_bn(o, " ", 1); cgx_bn(o, " ", 1); k++; } else { cgx_bn(o, "\n", 1); pend = it.i; k = it.i; } break;
            case DK_SOFT: if (!it.flat) { cgx_bn(o, "\n", 1); pend = it.i; k = it.i; } break;
            case DK_HARD: cgx_bn(o, "\n", 1); pend = it.i; k = it.i; break;
            case DK_CAT: cgx_spush(&s, it.i, it.flat, d->b); cgx_spush(&s, it.i, it.flat, d->a); break;
            case DK_NEST: cgx_spush(&s, it.i + d->n, it.flat, d->a); break;
            case DK_GROUP: {
                CgxItem f = {it.i, 1, d->a};
                const int fl = it.flat || cgx_fits(width - k, &s, &f);
                cgx_spush(&s, it.i, fl, d->a); break;
            }
        }
    }
    free(s.p);
}

/* ---- interface dos backends de linguagem (fase 2 acrescenta js, ts, c, go, java, rust: cada um e UM arquivo com esta forma) ---- */
typedef struct { int width; int comments; int indent; } CgxGenOpts;
typedef struct {
    const char *name, *alias, *ext;
    /* gera o codigo das declaracoes em code e, se tests != NULL, o corpo do harness de testes (executavel sozinho depois de code); erros em g->err; 1 se gerou */
    int (*gen)(Cgx *g, const CgxGenOpts *o, CgxB *code, CgxB *tests);
} CgxBackend;
