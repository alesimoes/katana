/* ---------- coder/codegeninc.c: Codegen, AST incremental (docs/codegen.md, "AST incremental") ----------
 * Muda codigo EXISTENTE sem reescreve-lo: le o arquivo (Python primeiro) num scanner proprio de subconjunto que guarda, para cada declaracao e
 * cada instrucao, o intervalo exato de bytes e de linhas; traduz o que entende para a UIR-H (code spec -> cgx_parse) com o mapa no -> intervalo, e o que
 * nao entende vira `raw python` (RAW: opaco, nunca regenerado); aplica uma ESPECIFICACAO DE MUDANCA so nos nos afetados e produz uma lista de
 * edicoes [a,b) -> texto: todo byte fora das edicoes (comentarios, espacos, aspas, ordem) sai igual. O texto novo dos trechos afetados vem do
 * gerador (code spec -> Python). A gravacao e um gancho (CgxIncWriter): o padrao grava o arquivo de forma atomica; no katana (CGX_INC_KATANA) cada
 * simbolo alterado passa por ct_edit_symbol (validacao estrutural, desfazer, LSP). Nada aqui depende do katana alem desse gancho.
 *
 * Especificacao de mudanca (uma operacao por bloco; a carga util vem recuada de 2 espacos e e code spec, salvo `insert-py`):
 *   add-function [after NOME | before NOME | end]      carga: uma ou mais funcoes/classes code spec
 *   add-method CLASSE [after NOME]                      carga: `fn` code spec (o `self` e acrescentado)
 *   replace-function NOME                               carga: a funcao/classe code spec inteira (Classe.metodo para metodos)
 *   replace-body NOME                                   carga: instrucoes code spec; o docstring antigo fica se a carga nao traz `doc`
 *   rename ANTIGO NOVO                                  nome de funcao/classe/metodo (Classe.metodo): definicao e usos, so os tokens
 *   add-param FUNCAO TEXTO-DO-PARAMETRO [call=EXPR]     `limite: int = 10`; com call=, cada chamada ganha `limite=EXPR`
 *   insert FUNCAO start|end|before N|after N [vars a b] carga: instrucoes code spec (N conta as instrucoes do corpo, de 1)
 *   insert-py FUNCAO start|end|before N|after N         carga: Python como esta
 *   delete-function NOME */

/* ---- tokens e linhas logicas ---- */
enum { PT_END, PT_NAME, PT_NUM, PT_STR, PT_OP };
typedef struct { int k; size_t b0, b1; } CgxPyTok;
/* proximo token a partir de *pos (pula espacos, quebras, comentarios e continuacoes) */
static int cgx_pt_next(const char *t, size_t n, size_t *pos, CgxPyTok *o) {
    size_t i = *pos;
    for (;;) {
        while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r' || t[i] == '\f')) i++;
        if (i < n && t[i] == '\\' && i + 1 < n && (t[i + 1] == '\n' || t[i + 1] == '\r')) { i += 2; continue; }
        if (i < n && t[i] == '#') { while (i < n && t[i] != '\n') i++; continue; }
        break;
    }
    if (i >= n) { *pos = n; o->k = PT_END; o->b0 = o->b1 = n; return 0; }
    o->b0 = i;
    const unsigned char c = (unsigned char)t[i];
    if (isalpha(c) || c == '_' || c >= 0x80) {
        size_t j = i; while (j < n && (isalnum((unsigned char)t[j]) || t[j] == '_' || (unsigned char)t[j] >= 0x80)) j++;
        if (j < n && (t[j] == '"' || t[j] == '\'') && j - i <= 3) {          /* prefixo de string: r, b, f, u, rb... */
            int ok = 1; for (size_t k = i; k < j; k++) if (!strchr("rRbBfFuU", t[k])) ok = 0;
            if (ok) { i = j; goto str; }
        }
        o->k = PT_NAME; o->b1 = j; *pos = j; return 1;
    }
    if (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)t[i + 1]))) {
        size_t j = i + 1; while (j < n && (isalnum((unsigned char)t[j]) || t[j] == '.' || t[j] == '_' || ((t[j] == '+' || t[j] == '-') && (t[j - 1] == 'e' || t[j - 1] == 'E')))) j++;
        o->k = PT_NUM; o->b1 = j; *pos = j; return 1;
    }
    if (c == '"' || c == '\'') {
    str:;
        const char q = t[i]; size_t j = i + 1; int triple = 0;
        if (j + 1 < n && t[j] == q && t[j + 1] == q) { triple = 1; j += 2; }
        while (j < n) {
            if (t[j] == '\\') { j += 2; continue; }
            if (triple) { if (t[j] == q && j + 2 < n + 0 && t[j + 1] == q && t[j + 2] == q) { j += 3; break; } if (t[j] == q && j + 2 == n) { j = n; break; } }
            else { if (t[j] == q) { j++; break; } if (t[j] == '\n') break; }
            j++;
        }
        if (j > n) j = n;
        o->k = PT_STR; o->b1 = j; *pos = j; return 1;
    }
    /* operadores de dois caracteres que importam (=, ==, +=, ->, **, //) ficam separados do `=` simples */
    size_t j = i + 1;
    if (j < n && ((c == '=' && t[j] == '=') || (c == '-' && t[j] == '>') || (c == '*' && t[j] == '*') || (c == '/' && t[j] == '/') || (strchr("+-*/%&|^<>!:@", (int)c) && t[j] == '=') || (c == '<' && t[j] == '<') || (c == '>' && t[j] == '>'))) j++;
    if (j < n && j == i + 2 && (c == '*' || c == '/' || c == '<' || c == '>') && t[j] == '=' && t[i + 1] == c) j++;
    o->k = PT_OP; o->b1 = j; *pos = j; return 1;
}
static int cgx_pt_is(const char *t, const CgxPyTok *k, const char *s) { const size_t l = strlen(s); return k->b1 - k->b0 == l && !memcmp(t + k->b0, s, l); }

/* ---- arvore de instrucoes ---- */
enum { PK_STMT, PK_DEF, PK_CLASS };
typedef struct {
    int kind, indent, parent, clause, decorated, oneliner, hdr;
    char kw[32]; char *name;
    size_t b0, c0, hb1, b1;               /* b0: inicio da linha (ou do 1o decorador); c0: 1o caractere do codigo; hb1: fim do cabecalho logico; b1: fim da instrucao com filhos */
    int line0, line1;                     /* 1-based */
    int *ch; int nch, cch;
} CgxPyNode;
typedef struct {
    const char *t; size_t n; int crlf, ok; char err[200]; int errline;
    CgxPyNode *nd; int nn, cn;                                    /* nd[0] = modulo (indent -1) */
    size_t *lst; int nl;                                           /* inicio de cada linha (busca binaria) */
} CgxInc;
static int cgx_inc_line_of(const char *t, size_t off) { int l = 1; for (size_t i = 0; i < off; i++) if (t[i] == '\n') l++; return l; }
static size_t cgx_inc_ls(const char *t, size_t off) { while (off > 0 && t[off - 1] != '\n') off--; return off; }
static size_t cgx_inc_le(const char *t, size_t n, size_t off) { while (off < n && t[off] != '\n') off++; return off; }
static int cgx_inc_indent_of(const char *t, size_t ls, size_t n) { int c = 0; while (ls < n && (t[ls] == ' ' || t[ls] == '\t')) { c = t[ls] == '\t' ? (c / 8 + 1) * 8 : c + 1; ls++; } return c; }

static int cgx_inc_add(CgxInc *c, int parent) {
    if (c->nn == c->cn) { c->cn = c->cn ? c->cn * 2 : 64; c->nd = (CgxPyNode *)realloc(c->nd, sizeof(CgxPyNode) * (size_t)c->cn); }
    CgxPyNode *x = &c->nd[c->nn]; memset(x, 0, sizeof *x); x->parent = parent;
    if (parent >= 0) { CgxPyNode *p = &c->nd[parent]; if (p->nch == p->cch) { p->cch = p->cch ? p->cch * 2 : 4; p->ch = (int *)realloc(p->ch, sizeof(int) * (size_t)p->cch); } p->ch[p->nch++] = c->nn; }
    return c->nn++;
}
static int cgx_inc_lineno(const CgxInc *c, size_t off) {
    int lo = 0, hi = c->nl - 1;
    while (lo < hi) { const int mid = (lo + hi + 1) / 2; if (c->lst[mid] <= off) lo = mid; else hi = mid - 1; }
    return lo + 1;
}
static void cgx_inc_free(CgxInc *c) { free(c->lst); for (int i = 0; i < c->nn; i++) { free(c->nd[i].ch); free(c->nd[i].name); } free(c->nd); memset(c, 0, sizeof *c); }
/* a linha logica que comeca em ls (primeira linha fisica com codigo); devolve o fim (posicao da quebra ou n) */
static size_t cgx_inc_logical_end(const char *t, size_t n, size_t from, int *hdr, size_t *lastsig, int *balanced) {
    size_t i = from; int depth = 0; char last = 0; size_t lastpos = from;
    while (i < n) {
        const char c = t[i];
        if (c == '#') { while (i < n && t[i] != '\n') i++; continue; }
        if (c == '\n') { if (depth == 0) break; i++; continue; }
        if (c == '\\' && i + 1 < n && (t[i + 1] == '\n' || (t[i + 1] == '\r' && i + 2 < n && t[i + 2] == '\n'))) { i += t[i + 1] == '\r' ? 3 : 2; continue; }
        if (c == '"' || c == '\'') {
            size_t j = i + 1; int triple = 0;
            if (j + 1 < n && t[j] == c && t[j + 1] == c) { triple = 1; j += 2; }
            while (j < n) {
                if (t[j] == '\\') { j += 2; continue; }
                if (triple) { if (t[j] == c && j + 2 < n && t[j + 1] == c && t[j + 2] == c) { j += 3; break; } if (t[j] == c && j + 2 >= n) { j = n; break; } }
                else { if (t[j] == c) { j++; break; } if (t[j] == '\n') break; }
                j++;
            }
            if (j > n) j = n;
            last = '"'; lastpos = j; i = j; continue;
        }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; }
        if (c != ' ' && c != '\t' && c != '\r') { last = c; lastpos = i; }
        i++;
    }
    *hdr = last == ':' && depth == 0; *lastsig = lastpos; *balanced = depth == 0;
    return i;
}
static int cgx_inc_word(const char *t, size_t pos, size_t n, char *w, size_t cap) { size_t i = pos, k = 0; while (i < n && (isalnum((unsigned char)t[i]) || t[i] == '_') && k + 1 < cap) w[k++] = t[i++]; w[k] = 0; return (int)k; }
static void cgx_inc_fail(CgxInc *c, int line, const char *msg) { if (c->ok) { c->ok = 0; c->errline = line; snprintf(c->err, sizeof c->err, "%s", msg); } }
/* le o arquivo inteiro; c->ok = 0 se a estrutura nao e confiavel (o chamador recusa mexer) */
static int cgx_inc_parse(CgxInc *c, const char *t, size_t n) {
    memset(c, 0, sizeof *c); c->t = t; c->n = n; c->ok = 1;
    { const char *nl = memchr(t, '\n', n); c->crlf = nl && nl > t && nl[-1] == '\r'; }
    { int cap = 256; c->lst = (size_t *)malloc(sizeof(size_t) * (size_t)cap); c->lst[c->nl++] = 0; for (size_t i = 0; i < n; i++) if (t[i] == '\n') { if (c->nl == cap) { cap *= 2; c->lst = (size_t *)realloc(c->lst, sizeof(size_t) * (size_t)cap); } c->lst[c->nl++] = i + 1; } }
    const int root = cgx_inc_add(c, -1); c->nd[root].indent = -1; c->nd[root].b0 = 0; c->nd[root].b1 = n; c->nd[root].line0 = 1;
    int stack[256], sp = 0; stack[sp++] = root;
    size_t pos = 0; int pend_deco = -1; size_t deco_b0 = 0;
    while (pos < n && c->ok) {
        size_t ls = pos, le = cgx_inc_le(t, n, ls), k = ls;
        while (k < le && (t[k] == ' ' || t[k] == '\t' || t[k] == '\r' || t[k] == '\f')) k++;
        if (k >= le || t[k] == '#') { pos = le < n ? le + 1 : n; continue; }                        /* em branco ou comentario */
        int hdr, bal; size_t lastsig; const size_t e = cgx_inc_logical_end(t, n, ls, &hdr, &lastsig, &bal);
        if (!bal) { cgx_inc_fail(c, cgx_inc_lineno(c, ls), "unbalanced brackets or an unterminated string"); break; }
        const int ind = cgx_inc_indent_of(t, ls, n);
        char w[24]; cgx_inc_word(t, k, n, w, sizeof w);
        /* pilha: sai dos blocos mais fundos ou iguais */
        while (sp > 1 && c->nd[stack[sp - 1]].indent >= ind) sp--;
        const int par = stack[sp - 1];
        if (c->nd[par].nch > 0 && c->nd[c->nd[par].ch[0]].indent != ind) { cgx_inc_fail(c, cgx_inc_lineno(c, ls), "inconsistent indentation"); break; }
        size_t ee = e; if (ee > ls && t[ee - 1] == '\r') ee--;
        if (t[k] == '@') {                                                                         /* decorador: gruda no proximo def/class */
            if (pend_deco < 0) { pend_deco = 1; deco_b0 = ls; }
            pos = e < n ? e + 1 : n; continue;
        }
        const int x = cgx_inc_add(c, par); CgxPyNode *nd = &c->nd[x];
        nd->indent = ind; nd->b0 = pend_deco >= 0 ? deco_b0 : ls; nd->decorated = pend_deco >= 0; nd->c0 = k; nd->hb1 = ee; nd->b1 = ee; nd->hdr = hdr;
        nd->line0 = cgx_inc_lineno(c, nd->b0); nd->line1 = cgx_inc_lineno(c, ee > 0 ? ee - 1 : 0);
        snprintf(nd->kw, sizeof nd->kw, "%s", w);
        if (!strcmp(w, "async")) { size_t q = k + 5; while (q < n && (t[q] == ' ' || t[q] == '\t')) q++; cgx_inc_word(t, q, n, w, sizeof w); snprintf(nd->kw, sizeof nd->kw, "async %s", w); }
        const char *kw = nd->kw[0] == 'a' && !strncmp(nd->kw, "async ", 6) ? nd->kw + 6 : nd->kw;
        if (!strcmp(kw, "def") || !strcmp(kw, "class")) {
            nd->kind = !strcmp(kw, "def") ? PK_DEF : PK_CLASS;
            size_t q = k; CgxPyTok tk; cgx_pt_next(t, n, &q, &tk); if (nd->kw[0] == 'a') cgx_pt_next(t, n, &q, &tk);
            if (cgx_pt_next(t, n, &q, &tk) && tk.k == PT_NAME) { nd->name = (char *)malloc(tk.b1 - tk.b0 + 1); memcpy(nd->name, t + tk.b0, tk.b1 - tk.b0); nd->name[tk.b1 - tk.b0] = 0; }
            if (!nd->name) { cgx_inc_fail(c, nd->line0, "def or class without a name"); break; }
            if (!hdr) nd->oneliner = 1;
        }
        nd->clause = !strcmp(w, "elif") || !strcmp(w, "else") || !strcmp(w, "except") || !strcmp(w, "finally");
        pend_deco = -1;
        if (nd->hdr && sp < 250) stack[sp++] = x;
        /* ate aqui o fim e so o do cabecalho; os filhos estendem depois */
        pos = e < n ? e + 1 : n;
    }
    if (c->ok && pend_deco >= 0) cgx_inc_fail(c, cgx_inc_lineno(c, deco_b0), "decorator without a definition");
    if (!c->ok) return 0;
    /* fim de cada no = fim do ultimo filho (de baixo para cima: filhos tem indice maior) */
    for (int i = c->nn - 1; i >= 1; i--) {
        CgxPyNode *x = &c->nd[i];
        if (x->hdr && x->nch == 0) { cgx_inc_fail(c, x->line0, "a block header (`:`) with no indented body"); return 0; }
        if (x->nch) { const CgxPyNode *lc = &c->nd[x->ch[x->nch - 1]]; if (lc->b1 > x->b1) x->b1 = lc->b1; x->line1 = cgx_inc_lineno(c, x->b1 > 0 ? x->b1 - 1 : 0); }
    }
    return 1;
}
/* fim de uma instrucao incluindo as clausulas seguintes (elif/else/except/finally) */
static size_t cgx_inc_chain_end(const CgxInc *c, int idx) {
    const CgxPyNode *x = &c->nd[idx]; const CgxPyNode *p = &c->nd[x->parent]; size_t e = x->b1; int found = 0;
    for (int i = 0; i < p->nch; i++) {
        if (p->ch[i] == idx) { found = 1; continue; }
        if (found) { if (c->nd[p->ch[i]].clause) e = c->nd[p->ch[i]].b1; else break; }
    }
    return e;
}
/* inicio da regiao de uma declaracao: decoradores e os comentarios coladinhos logo acima (mesma indentacao, sem linha em branco) */
static size_t cgx_inc_attach_start(const CgxInc *c, int idx) {
    const char *t = c->t; size_t s = c->nd[idx].b0;
    while (s > 0) {
        const size_t ps = cgx_inc_ls(t, s - 1); size_t k = ps; while (k < s && (t[k] == ' ' || t[k] == '\t')) k++;
        if (k < s && t[k] == '#' && cgx_inc_indent_of(t, ps, c->n) == c->nd[idx].indent) s = ps; else break;
    }
    return s;
}
/* instrucoes (sem clausulas) do corpo de um def/class */
static int cgx_inc_body_stmts(const CgxInc *c, int idx, int *out, int cap) {
    int m = 0; const CgxPyNode *x = &c->nd[idx];
    for (int i = 0; i < x->nch && m < cap; i++) if (!c->nd[x->ch[i]].clause) out[m++] = x->ch[i];
    return m;
}
/* acha def/class por `Nome` (modulo) ou `Classe.metodo` (ou aninhados); -1 e mensagem se nao ha ou e ambiguo */
static int cgx_inc_find(const CgxInc *c, const char *spec, char *err, size_t ecap) {
    char s[200]; snprintf(s, sizeof s, "%s", spec); int cur = 0;
    for (char *tok = strtok(s, "."); tok; tok = strtok(NULL, ".")) {
        int found = -1, cnt = 0; const CgxPyNode *p = &c->nd[cur];
        for (int i = 0; i < p->nch; i++) { const CgxPyNode *x = &c->nd[p->ch[i]]; if ((x->kind == PK_DEF || x->kind == PK_CLASS) && !strcmp(x->name, tok)) { if (found < 0) found = p->ch[i]; cnt++; } }
        if (found < 0) {
            size_t w = (size_t)snprintf(err, ecap, "no definition `%s` in %s. Definitions there: ", tok, cur ? c->nd[cur].name : "the file"); int sh = 0;
            for (int i = 0; i < p->nch && sh < 12 && w + 40 < ecap; i++) { const CgxPyNode *x = &c->nd[p->ch[i]]; if (x->kind != PK_STMT) w += (size_t)snprintf(err + w, ecap - w, "%s%s", sh++ ? ", " : "", x->name); }
            if (!sh) snprintf(err + w, ecap - w, "(none)");
            return -1;
        }
        if (cnt > 1) { snprintf(err, ecap, "`%s` is defined %d times in %s (lines %d..): say which by editing it with replace-function after renaming", tok, cnt, cur ? c->nd[cur].name : "the file", c->nd[found].line0); return -1; }
        cur = found;
    }
    return cur ? cur : -1;
}

/* ---- Python -> UIR-H: gera o code spec equivalente (uma linha de code spec por instrucao) e o mapa linha-do-code spec -> no; o que nao entende vira `raw python` ---- */
typedef struct { CgxB out; int *lmap; int nl, cl; unsigned char *force_raw; const CgxInc *inc; int nforce; size_t covered; } CgxIncKcode;
static void cgx_ic_line(CgxIncKcode *x, int level, int node, const char *txt) {
    if (x->nl == x->cl) { x->cl = x->cl ? x->cl * 2 : 64; x->lmap = (int *)realloc(x->lmap, sizeof(int) * (size_t)x->cl); }
    x->lmap[x->nl++] = node;
    for (int i = 0; i < level; i++) cgx_bs(&x->out, "  ");
    cgx_bs(&x->out, txt); cgx_bs(&x->out, "\n");
}
/* texto de uma expressao em uma linha: sem comentarios, quebras e continuacoes; as aspas ficam como estao */
static void cgx_inc_flat(const char *t, size_t b0, size_t b1, CgxB *o) {
    size_t i = b0; int sp = 0;
    while (i < b1) {
        const char c = t[i];
        if (c == '#') { while (i < b1 && t[i] != '\n') i++; continue; }
        if (c == '\\' && i + 1 < b1 && (t[i + 1] == '\n' || t[i + 1] == '\r')) { i += t[i + 1] == '\r' ? 3 : 2; sp = 1; continue; }
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') { sp = 1; i++; continue; }
        if (sp && o->n) cgx_bs(o, " ");
        sp = 0;
        if (c == '"' || c == '\'') {
            size_t j = i + 1; int triple = 0;
            if (j + 1 < b1 && t[j] == c && t[j + 1] == c) { triple = 1; j += 2; }
            while (j < b1) { if (t[j] == '\\') { j += 2; continue; } if (triple ? (t[j] == c && j + 2 < b1 + 0 + 1 && t[j + 1] == c && t[j + 2] == c) : t[j] == c) { j += triple ? 3 : 1; break; } j++; }
            if (j > b1) j = b1;
            cgx_bn(o, t + i, j - i); i = j; continue;
        }
        cgx_bn(o, &t[i], 1); i++;
    }
}
/* dentro de [b0,b1): posicao do primeiro `ch` fora de parenteses, aspas e comentarios (nao seguido de `=` quando eq_only); -1 se nao ha */
static long cgx_inc_top_find(const char *t, size_t b0, size_t b1, const char *what) {
    size_t pos = b0; CgxPyTok k; int depth = 0; const size_t wl = strlen(what);
    while (cgx_pt_next(t, b1, &pos, &k)) {
        if (k.k == PT_OP && k.b1 - k.b0 == 1 && strchr("([{", t[k.b0])) depth++;
        else if (k.k == PT_OP && k.b1 - k.b0 == 1 && strchr(")]}", t[k.b0])) depth--;
        else if (depth == 0 && k.b1 - k.b0 == wl && !memcmp(t + k.b0, what, wl)) return (long)k.b0;
    }
    return -1;
}
/* fim do que vem depois do cabecalho: a posicao do `:` que fecha a linha logica de um cabecalho (ultimo `:` fora de parenteses) */
static size_t cgx_inc_hdr_colon(const char *t, size_t b0, size_t b1) {
    size_t pos = b0, last = b1; CgxPyTok k; int depth = 0;
    while (cgx_pt_next(t, b1, &pos, &k)) {
        if (k.k == PT_OP && k.b1 - k.b0 == 1) { const char c = t[k.b0]; if (strchr("([{", c)) depth++; else if (strchr(")]}", c)) depth--; else if (c == ':' && depth == 0) last = k.b0; }
    }
    return last;
}
static const char *cgx_inc_ty(const char *a, char *o, size_t cap) {         /* anotacao Python -> tipo code spec, ou NULL */
    char s[120]; size_t k = 0; for (const char *p = a; *p && k + 1 < sizeof s; p++) if (*p != ' ') s[k++] = *p; s[k] = 0;
    if (!strcmp(s, "int") || !strcmp(s, "float") || !strcmp(s, "str") || !strcmp(s, "bool")) { snprintf(o, cap, "%s", s); return o; }
    if (!strncmp(s, "list[", 5) || !strncmp(s, "List[", 5)) { char in[120]; snprintf(in, sizeof in, "%s", s + 5); char *e = strrchr(in, ']'); if (e) *e = 0; char b[100]; if (cgx_inc_ty(in, b, sizeof b)) { snprintf(o, cap, "list[%s]", b); return o; } return "list"; }
    if (!strncmp(s, "dict[str,", 9) || !strncmp(s, "Dict[str,", 9)) { char in[120]; snprintf(in, sizeof in, "%s", s + 9); char *e = strrchr(in, ']'); if (e) *e = 0; char b[100]; if (cgx_inc_ty(in, b, sizeof b)) { snprintf(o, cap, "map[str,%s]", b); return o; } return "map"; }
    return NULL;
}
typedef struct { char name[64]; char ann[80]; char *def; int star; } CgxIncParam;
/* parametros do cabecalho de um def; 0 se algo fora do subconjunto (*args, **kw, `/`, `*`) */
static int cgx_inc_params(const CgxInc *c, int idx, CgxIncParam *p, int cap, int *np, size_t *open_b, size_t *close_b, char **ret) {
    const char *t = c->t; const CgxPyNode *x = &c->nd[idx]; size_t pos = x->c0; CgxPyTok k; *np = 0; *ret = NULL;
    while (cgx_pt_next(t, c->n, &pos, &k) && !(k.k == PT_OP && cgx_pt_is(t, &k, "("))) { if (k.b0 >= x->hb1) return 0; }
    if (k.k != PT_OP) return 0;
    *open_b = k.b0; int depth = 1; size_t ps = k.b1; int cur = 0; int first = 1; (void)first;
    size_t seg0 = ps; int ok = 1;
    for (;;) {
        if (!cgx_pt_next(t, c->n, &pos, &k)) return 0;
        const int isop = k.k == PT_OP && k.b1 - k.b0 == 1; const char ch = isop ? t[k.b0] : 0;
        if (isop && strchr("([{", ch)) depth++;
        else if (isop && strchr(")]}", ch)) depth--;
        if ((depth == 0 && ch == ')') || (depth == 1 && ch == ',')) {
            CgxB seg = {0}; cgx_inc_flat(t, seg0, k.b0, &seg);
            if (seg.n) {
                if (cur >= cap) { free(seg.p); return 0; }
                CgxIncParam *q = &p[cur]; memset(q, 0, sizeof *q); const char *s = seg.p;
                if (*s == '*' || *s == '/') { ok = 0; }
                size_t nl = 0; while (isalnum((unsigned char)s[nl]) || s[nl] == '_') nl++;
                if (!nl || nl >= sizeof q->name) ok = 0; else { memcpy(q->name, s, nl); q->name[nl] = 0; }
                s += nl; while (*s == ' ') s++;
                if (*s == ':') { s++; while (*s == ' ') s++; const char *e = s; int d2 = 0; while (*e && !(d2 == 0 && *e == '=')) { if (strchr("([{", *e)) d2++; if (strchr(")]}", *e)) d2--; e++; } size_t al = (size_t)(e - s); while (al && s[al - 1] == ' ') al--; snprintf(q->ann, sizeof q->ann, "%.*s", (int)(al < sizeof q->ann ? al : sizeof q->ann - 1), s); s = e; }
                if (*s == '=') { s++; while (*s == ' ') s++; q->def = strdup(s); }
                else if (*s) ok = 0;
                cur++;
            }
            free(seg.p); seg0 = k.b1;
            if (depth == 0) { *close_b = k.b0; *np = cur; break; }
        }
    }
    /* anotacao de retorno */
    { size_t q2 = *close_b + 1; CgxPyTok r; if (cgx_pt_next(t, x->hb1, &q2, &r) && r.k == PT_OP && cgx_pt_is(t, &r, "->")) { const size_t colon = cgx_inc_hdr_colon(t, r.b1, x->hb1); CgxB b = {0}; cgx_inc_flat(t, r.b1, colon, &b); *ret = b.p; } }
    if (!ok) { for (int i = 0; i < cur; i++) free(p[i].def); *np = 0; return 0; }
    return 1;
}
static void cgx_inc_params_free(CgxIncParam *p, int n) { for (int i = 0; i < n; i++) free(p[i].def); }
/* o corpo traduzido */
typedef struct { char names[64][48]; int n; } CgxIncDecl;
static int cgx_inc_declared(CgxIncDecl *d, const char *nm) { for (int i = 0; i < d->n; i++) if (!strcmp(d->names[i], nm)) return 1; return 0; }
static void cgx_inc_declare(CgxIncDecl *d, const char *nm) { if (!cgx_inc_declared(d, nm) && d->n < 64) snprintf(d->names[d->n++], 48, "%s", nm); }
static void cgx_ic_raw(CgxIncKcode *x, const CgxInc *c, int idx, int level) {
    const CgxPyNode *n = &c->nd[idx]; const char *t = c->t; size_t a = n->b0, e = cgx_inc_chain_end(c, idx);
    if (e > x->covered) x->covered = e;
    /* nomes que o bloco define (so o primeiro nivel: atribuicao, def, class, import) */
    char defs[200] = ""; size_t dw = 0;
    if (n->kind != PK_STMT) dw += (size_t)snprintf(defs + dw, sizeof defs - dw, "%s", n->name);
    else if (!strcmp(n->kw, "import") || !strcmp(n->kw, "from")) {
        size_t pos = n->c0; CgxPyTok k; int after_import = !strcmp(n->kw, "import"); char prev[64] = "";
        while (cgx_pt_next(t, n->hb1, &pos, &k)) {
            if (k.k == PT_NAME && cgx_pt_is(t, &k, "import")) { after_import = 1; prev[0] = 0; continue; }
            if (!after_import || k.k != PT_NAME) continue;
            char w[64]; snprintf(w, sizeof w, "%.*s", (int)(k.b1 - k.b0 < 63 ? k.b1 - k.b0 : 63), t + k.b0);
            size_t save = pos; CgxPyTok nx; const int more = cgx_pt_next(t, n->hb1, &save, &nx);
            if (!strcmp(w, "as")) { if (more) { snprintf(prev, sizeof prev, "%.*s", (int)(nx.b1 - nx.b0 < 63 ? nx.b1 - nx.b0 : 63), t + nx.b0); pos = save; } continue; }
            if (more && nx.k == PT_OP && cgx_pt_is(t, &nx, ".")) continue;                 /* a.b.c: so o primeiro nome conta (import a.b) */
            if (more && nx.k == PT_NAME && cgx_pt_is(t, &nx, "as")) continue;
            snprintf(prev, sizeof prev, "%s", w);
            if (dw + strlen(prev) + 2 < sizeof defs && !strstr(defs, prev)) dw += (size_t)snprintf(defs + dw, sizeof defs - dw, "%s%s", dw ? ", " : "", prev);
        }
    } else {
        const long eq = cgx_inc_top_find(t, n->c0, n->hb1, "=");
        if (eq > 0) { size_t pos = n->c0; CgxPyTok k; while (cgx_pt_next(t, (size_t)eq, &pos, &k)) if (k.k == PT_NAME && dw + k.b1 - k.b0 + 3 < sizeof defs) dw += (size_t)snprintf(defs + dw, sizeof defs - dw, "%s%.*s", dw ? ", " : "", (int)(k.b1 - k.b0), t + k.b0); }
    }
    char hd[300]; cgx_ic_line(x, level, idx, "#");                       /* separa blocos raw vizinhos (o parser junta raw seguidos) */
    snprintf(hd, sizeof hd, "raw python%s%s", dw ? " defines " : "", defs); cgx_ic_line(x, level, idx, hd);
    const size_t drop = (size_t)(n->c0 - cgx_inc_ls(t, n->c0));
    size_t p = cgx_inc_ls(t, n->c0);
    if (n->decorated) p = a;
    int firstl = 1;
    while (p < e) {
        size_t le = cgx_inc_le(t, c->n, p); if (le > e) le = e;
        size_t q = p; size_t skip = 0; while (q < le && skip < drop && (t[q] == ' ' || t[q] == '\t')) { q++; skip++; }
        size_t qe = le; if (qe > q && t[qe - 1] == '\r') qe--;
        CgxB ln = {0}; cgx_bn(&ln, t + q, qe - q);
        char *lt = ln.p ? ln.p : (char *)"";
        if (!*lt && !firstl) { cgx_ic_line(x, 0, idx, ""); }
        else { CgxB w = {0}; cgx_bs(&w, "  "); cgx_bs(&w, lt); cgx_ic_line(x, level, idx, w.p); free(w.p); }
        free(ln.p); firstl = 0; p = le + 1;
    }
}
static void cgx_ic_block(CgxIncKcode *x, const CgxInc *c, int parent, int level, CgxIncDecl *dc, int in_fn);
static void cgx_ic_stmt(CgxIncKcode *x, const CgxInc *c, int idx, int level, CgxIncDecl *dc, int in_fn) {
    const CgxPyNode *n = &c->nd[idx]; const char *t = c->t; char hd[900]; CgxB e = {0}; hd[0] = 0;
    if (n->b0 < x->covered) return;
    if (x->force_raw[idx] || n->kind != PK_STMT || n->decorated || n->oneliner) { cgx_ic_raw(x, c, idx, level); free(e.p); return; }
    const char *kw = n->kw; int comp = 0;
    if (!strcmp(kw, "return")) { cgx_inc_flat(t, n->c0 + 6, n->hb1, &e); snprintf(hd, sizeof hd, e.n ? "return %s" : "return", e.p); }
    else if (!strcmp(kw, "raise")) { cgx_inc_flat(t, n->c0 + 5, n->hb1, &e); if (!e.n) { cgx_ic_raw(x, c, idx, level); free(e.p); return; } snprintf(hd, sizeof hd, "throw %s", e.p); }
    else if (!strcmp(kw, "break") || !strcmp(kw, "continue")) snprintf(hd, sizeof hd, "%s", kw);
    else if (!strcmp(kw, "if") || !strcmp(kw, "elif") || !strcmp(kw, "while")) { comp = 1; cgx_inc_flat(t, n->c0 + strlen(kw), cgx_inc_hdr_colon(t, n->c0, n->hb1), &e); snprintf(hd, sizeof hd, "%s %s", kw, e.p ? e.p : ""); }
    else if (!strcmp(kw, "else") && n->hdr) { comp = 1; snprintf(hd, sizeof hd, "else"); }
    else if (!strcmp(kw, "try") && n->hdr) { comp = 1; snprintf(hd, sizeof hd, "try"); }
    else if (!strcmp(kw, "finally") && n->hdr) { comp = 1; snprintf(hd, sizeof hd, "finally"); }
    else if (!strcmp(kw, "except") && n->hdr) {
        comp = 1; const size_t colon = cgx_inc_hdr_colon(t, n->c0, n->hb1); cgx_inc_flat(t, n->c0 + 6, colon, &e);
        const char *as = e.p ? strstr(e.p, " as ") : NULL; snprintf(hd, sizeof hd, as ? "catch %s" : "catch", as ? as + 4 : "");
    }
    else if (!strcmp(kw, "for") && n->hdr) {
        const size_t colon = cgx_inc_hdr_colon(t, n->c0, n->hb1); const long inp = cgx_inc_top_find(t, n->c0 + 3, colon, "in");
        if (inp < 0) { cgx_ic_raw(x, c, idx, level); free(e.p); return; }
        CgxB tg = {0}, it = {0}; cgx_inc_flat(t, n->c0 + 3, (size_t)inp, &tg); cgx_inc_flat(t, (size_t)inp + 2, colon, &it); comp = 1;
        if (it.p && !strncmp(it.p, "range(", 6) && it.p[it.n - 1] == ')') {
            char a3[3][200]; int na = 0, d2 = 0; size_t s0 = 6;
            for (size_t i = 6; i < it.n; i++) { const char ch = it.p[i]; if (strchr("([{", ch)) d2++; else if (strchr(")]}", ch)) { if (d2 == 0) { snprintf(a3[na < 3 ? na : 2], 200, "%.*s", (int)(i - s0 < 199 ? i - s0 : 199), it.p + s0); na++; break; } d2--; } else if (ch == ',' && d2 == 0) { if (na < 3) snprintf(a3[na], 200, "%.*s", (int)(i - s0 < 199 ? i - s0 : 199), it.p + s0); na++; s0 = i + 1; while (it.p[s0] == ' ') s0++; } }
            if (na == 1) snprintf(hd, sizeof hd, "for %s in 0..%s", tg.p, a3[0]);
            else if (na == 2) snprintf(hd, sizeof hd, "for %s in %s..%s", tg.p, a3[0], a3[1]);
            else if (na == 3) snprintf(hd, sizeof hd, "for %s in %s..%s step %s", tg.p, a3[0], a3[1], a3[2]);
            else comp = 0;
        } else snprintf(hd, sizeof hd, "for %s in %s", tg.p, it.p);
        free(tg.p); free(it.p);
        if (!comp) { cgx_ic_raw(x, c, idx, level); free(e.p); return; }
    }
    else if (!strcmp(kw, "pass") || !strcmp(kw, "import") || !strcmp(kw, "from") || !strcmp(kw, "global") || !strcmp(kw, "nonlocal") || !strcmp(kw, "assert") || !strcmp(kw, "del") || !strcmp(kw, "with") || !strcmp(kw, "match") || !strcmp(kw, "lambda") || !strcmp(kw, "yield") || !strcmp(kw, "async") || !strcmp(kw, "await") || !strcmp(kw, "print") || !strcmp(kw, "def") || !strcmp(kw, "class")) { cgx_ic_raw(x, c, idx, level); free(e.p); return; }
    else if (!n->hdr) {
        /* atribuicao, atribuicao aumentada, anotada, chamada */
        const long eq = cgx_inc_top_find(t, n->c0, n->hb1, "=");
        long aug = -1; static const char *const augs[] = {"+=", "-=", "*=", "/=", "%="}; for (int i = 0; i < 5 && aug < 0; i++) aug = cgx_inc_top_find(t, n->c0, n->hb1, augs[i]);
        CgxB lhs = {0}, rhs = {0};
        if (eq > 0 && cgx_inc_top_find(t, (size_t)eq + 1, n->hb1, "=") < 0) {
            cgx_inc_flat(t, n->c0, (size_t)eq, &lhs); cgx_inc_flat(t, (size_t)eq + 1, n->hb1, &rhs);
            char *colon = lhs.p ? strchr(lhs.p, ':') : NULL; char ann[80] = "";
            if (colon) { *colon = 0; const char *a = colon + 1; while (*a == ' ') a++; snprintf(ann, sizeof ann, "%s", a); size_t l = strlen(lhs.p); while (l && lhs.p[l - 1] == ' ') lhs.p[--l] = 0; }
            int simple = lhs.p && lhs.n; for (const char *p = lhs.p; p && *p; p++) if (!isalnum((unsigned char)*p) && *p != '_') simple = 0;
            if (!lhs.n || (!simple && colon)) { cgx_ic_raw(x, c, idx, level); free(lhs.p); free(rhs.p); free(e.p); return; }
            if (simple) {
                char tyb[60]; const char *ty = ann[0] ? cgx_inc_ty(ann, tyb, sizeof tyb) : NULL;
                if (in_fn && !cgx_inc_declared(dc, lhs.p)) { cgx_inc_declare(dc, lhs.p); if (ty) snprintf(hd, sizeof hd, "let %s: %s = %s", lhs.p, ty, rhs.p); else snprintf(hd, sizeof hd, "let %s = %s", lhs.p, rhs.p); }
                else snprintf(hd, sizeof hd, "%s = %s", lhs.p, rhs.p);
            } else snprintf(hd, sizeof hd, "%s = %s", lhs.p, rhs.p);
        } else if (aug > 0 && eq < 0) {
            cgx_inc_flat(t, n->c0, (size_t)aug, &lhs); cgx_inc_flat(t, (size_t)aug + 2, n->hb1, &rhs);
            snprintf(hd, sizeof hd, "%s %.2s %s", lhs.p, t + aug, rhs.p);
        } else if (eq < 0 && aug < 0) { cgx_inc_flat(t, n->c0, n->hb1, &lhs); snprintf(hd, sizeof hd, "%s", lhs.p ? lhs.p : ""); }
        else { cgx_ic_raw(x, c, idx, level); free(lhs.p); free(rhs.p); free(e.p); return; }
        free(lhs.p); free(rhs.p);
    }
    else { cgx_ic_raw(x, c, idx, level); free(e.p); return; }
    free(e.p);
    cgx_ic_line(x, level, idx, hd);
    if (comp) cgx_ic_block(x, c, idx, level + 1, dc, in_fn);
}
/* texto de um docstring de uma linha sem escapes; 0 se nao serve */
static int cgx_ic_doctext(const CgxInc *c, int s, char *o, size_t cap) {
    CgxB d = {0}; cgx_inc_flat(c->t, c->nd[s].c0, c->nd[s].hb1, &d);
    const char *q = d.p; const size_t l = d.n; int ok = l >= 2 && (q[0] == '"' || q[0] == '\''); size_t qn = 1;
    if (ok && l >= 6 && q[1] == q[0] && q[2] == q[0]) qn = 3;
    if (ok && (l < 2 * qn || memchr(q, '\\', l) || (l - 2 * qn) >= cap)) ok = 0;
    if (ok) { memcpy(o, q + qn, l - 2 * qn); o[l - 2 * qn] = 0; }
    free(d.p); return ok;
}
static int cgx_ic_is_doc(const CgxInc *c, int idx) {
    const CgxPyNode *n = &c->nd[idx]; if (n->kind != PK_STMT || n->hdr) return 0;
    size_t pos = n->c0; CgxPyTok k; if (!cgx_pt_next(c->t, n->hb1, &pos, &k) || k.k != PT_STR) return 0;
    CgxPyTok k2; if (cgx_pt_next(c->t, n->hb1, &pos, &k2)) return 0;
    return 1;
}
static void cgx_ic_block(CgxIncKcode *x, const CgxInc *c, int parent, int level, CgxIncDecl *dc, int in_fn) {
    const CgxPyNode *p = &c->nd[parent];
    for (int i = 0; i < p->nch; i++) cgx_ic_stmt(x, c, p->ch[i], level, dc, in_fn);
}
/* def -> `fn`; devolve 0 se o cabecalho nao cabe no subconjunto (o chamador emite raw) */
static int cgx_ic_fn(CgxIncKcode *x, const CgxInc *c, int idx, int level, int is_method) {
    const CgxPyNode *n = &c->nd[idx]; CgxIncParam ps[24]; int np; size_t ob, cb; char *ret = NULL;
    if (x->force_raw[idx] || n->decorated || n->oneliner || n->kw[0] == 'a' || !cgx_inc_params(c, idx, ps, 24, &np, &ob, &cb, &ret)) { free(ret); return 0; }
    CgxB h = {0}; cgx_bf(&h, "fn %s(", n->name); int first = 1; CgxIncDecl dc; memset(&dc, 0, sizeof dc);
    for (int i = 0; i < np; i++) {
        if (i == 0 && is_method && (!strcmp(ps[i].name, "self") || !strcmp(ps[i].name, "cls"))) continue;
        char tyb[60]; const char *ty = ps[i].ann[0] ? cgx_inc_ty(ps[i].ann, tyb, sizeof tyb) : NULL;
        cgx_bf(&h, "%s%s", first ? "" : ", ", ps[i].name); if (ty) cgx_bf(&h, ": %s", ty); if (ps[i].def) cgx_bf(&h, " = %s", ps[i].def);
        cgx_inc_declare(&dc, ps[i].name); first = 0;
    }
    cgx_bs(&h, ")");
    if (ret) { char tyb[60]; const char *ty = cgx_inc_ty(ret, tyb, sizeof tyb); if (ty) cgx_bf(&h, " -> %s", ty); }
    cgx_inc_params_free(ps, np); free(ret);
    cgx_ic_line(x, level, idx, h.p); free(h.p);
    for (int i = 0; i < n->nch; i++) {
        const int s = n->ch[i];
        if (i == 0 && cgx_ic_is_doc(c, s) && !x->force_raw[s]) {
            char dt[380]; if (cgx_ic_doctext(c, s, dt, sizeof dt)) { char tx[400]; snprintf(tx, sizeof tx, "doc %s", dt); cgx_ic_line(x, level + 1, s, tx); continue; }
        }
        cgx_ic_stmt(x, c, s, level + 1, &dc, 1);
    }
    return 1;
}
static void cgx_ic_unit(CgxIncKcode *x, const CgxInc *c, int idx) {
    const CgxPyNode *n = &c->nd[idx]; const char *t = c->t;
    if (n->b0 < x->covered) return;
    if (n->kind == PK_DEF) { const int save = x->nl; const size_t sv = x->out.n; if (!cgx_ic_fn(x, c, idx, 0, 0)) { x->nl = save; x->out.n = sv; if (x->out.p) x->out.p[sv] = 0; cgx_ic_raw(x, c, idx, 0); } return; }
    if (n->kind == PK_CLASS) {
        /* classe simples: `class A:` ou `class A(object):`, com metodos e atribuicoes de classe */
        const size_t colon = cgx_inc_hdr_colon(t, n->c0, n->hb1); CgxB bases = {0}; const char *pa = memchr(t + n->c0, '(', colon - n->c0);
        int simple = !x->force_raw[idx] && !n->decorated && !n->oneliner;
        if (pa) { cgx_inc_flat(t, (size_t)(pa - t) + 1, colon, &bases); size_t l = bases.n; while (l && (bases.p[l - 1] == ' ' || bases.p[l - 1] == ')')) l--; if (l && !(l == 6 && !strncmp(bases.p, "object", 6))) simple = 0; }
        free(bases.p);
        for (int i = 0; simple && i < n->nch; i++) {
            const CgxPyNode *m = &c->nd[n->ch[i]];
            if (m->kind == PK_DEF) continue;
            if (cgx_ic_is_doc(c, n->ch[i])) continue;
            const long eq = m->kind == PK_STMT && !m->hdr ? cgx_inc_top_find(t, m->c0, m->hb1, "=") : -1;
            if (eq <= 0) simple = 0;
        }
        if (!simple) { cgx_ic_raw(x, c, idx, 0); return; }
        const int save = x->nl; const size_t sv = x->out.n; char hd[100]; snprintf(hd, sizeof hd, "class %s", n->name); cgx_ic_line(x, 0, idx, hd);
        for (int i = 0; i < n->nch; i++) {
            const int s = n->ch[i]; const CgxPyNode *m = &c->nd[s];
            if (m->kind == PK_DEF) { const int sl = x->nl; const size_t so = x->out.n; if (!cgx_ic_fn(x, c, s, 1, 1)) { /* metodo fora do subconjunto: a classe toda vira raw */ (void)sl; (void)so; x->nl = save; x->out.n = sv; if (x->out.p) x->out.p[sv] = 0; cgx_ic_raw(x, c, idx, 0); return; } }
            else if (cgx_ic_is_doc(c, s)) { char dt[280]; if (cgx_ic_doctext(c, s, dt, sizeof dt)) { char tx[300]; snprintf(tx, sizeof tx, "doc %s", dt); cgx_ic_line(x, 1, s, tx); } else { x->nl = save; x->out.n = sv; if (x->out.p) x->out.p[sv] = 0; cgx_ic_raw(x, c, idx, 0); return; } }
            else { CgxB l = {0}, r = {0}; const long eq = cgx_inc_top_find(t, m->c0, m->hb1, "="); cgx_inc_flat(t, m->c0, (size_t)eq, &l); cgx_inc_flat(t, (size_t)eq + 1, m->hb1, &r); char tx[500]; snprintf(tx, sizeof tx, "field %s = %s", l.p ? l.p : "", r.p ? r.p : ""); cgx_ic_line(x, 1, s, tx); free(l.p); free(r.p); }
        }
        return;
    }
    /* instrucao de modulo: NAME = expr vira const; o resto e raw */
    if (!n->hdr && !n->decorated && !x->force_raw[idx] && !n->oneliner && strcmp(n->kw, "import") && strcmp(n->kw, "from")) {
        const long eq = cgx_inc_top_find(t, n->c0, n->hb1, "=");
        if (eq > 0 && cgx_inc_top_find(t, (size_t)eq + 1, n->hb1, "=") < 0) {
            CgxB l = {0}, r = {0}; cgx_inc_flat(t, n->c0, (size_t)eq, &l); cgx_inc_flat(t, (size_t)eq + 1, n->hb1, &r);
            int simple = l.n > 0; for (size_t i = 0; i < l.n; i++) if (!isalnum((unsigned char)l.p[i]) && l.p[i] != '_') simple = 0;
            if (simple) { char tx[600]; snprintf(tx, sizeof tx, "const %s = %s", l.p, r.p ? r.p : ""); cgx_ic_line(x, 0, idx, tx); free(l.p); free(r.p); return; }
            free(l.p); free(r.p);
        }
    }
    cgx_ic_raw(x, c, idx, 0);
}
/* arquivo -> code spec (+ mapa). Itera: cada erro do parser marca a instrucao responsavel como raw e tenta de novo. */
typedef struct { Cgx g; CgxIncKcode kcode; int ok, nraw_forced; } CgxIncUir;
static void cgx_inc_uir_free(CgxIncUir *u) { cgx_free(&u->g); free(u->kcode.out.p); free(u->kcode.lmap); free(u->kcode.force_raw); memset(u, 0, sizeof *u); }
static int cgx_inc_uir(const CgxInc *c, CgxIncUir *u) {
    memset(u, 0, sizeof *u); u->kcode.inc = c; u->kcode.force_raw = (unsigned char *)calloc((size_t)c->nn + 1, 1);
    for (int round = 0; round < 12; round++) {
        free(u->kcode.out.p); free(u->kcode.lmap); memset(&u->kcode.out, 0, sizeof u->kcode.out); u->kcode.lmap = NULL; u->kcode.nl = u->kcode.cl = 0; u->kcode.covered = 0;
        cgx_ic_line(&u->kcode, 0, 0, "# codegen incremental reader"); /* linha 1: mantem o mapa 1-based */
        const CgxPyNode *root = &c->nd[0];
        for (int i = 0; i < root->nch; i++) cgx_ic_unit(&u->kcode, c, root->ch[i]);
        memset(&u->g, 0, sizeof u->g);
        if (cgx_parse(&u->g, u->kcode.out.p ? u->kcode.out.p : "")) { u->ok = 1; return 1; }
        int marked = 0;
        for (int e = 0; e < u->g.nerr; e++) {
            const int ln = u->g.err[e].line; if (ln < 1 || ln > u->kcode.nl) continue;
            int nd = u->kcode.lmap[ln - 1]; if (nd <= 0) continue;
            while (c->nd[nd].clause && c->nd[nd].parent >= 0) { const CgxPyNode *pp = &c->nd[c->nd[nd].parent]; int hd = nd; for (int q = 0; q < pp->nch; q++) { if (pp->ch[q] == nd) break; if (!c->nd[pp->ch[q]].clause) hd = pp->ch[q]; } if (hd == nd) break; nd = hd; }
            if (!u->kcode.force_raw[nd]) { u->kcode.force_raw[nd] = 1; marked++; }
            else if (c->nd[nd].parent > 0 && !u->kcode.force_raw[c->nd[nd].parent]) { u->kcode.force_raw[c->nd[nd].parent] = 1; marked++; }       /* sobe: o pai inteiro vira raw */
        }
        cgx_free(&u->g);
        if (!marked) break;
    }
    /* ultimo recurso: tudo raw (o mapa continua valendo) */
    memset(u->kcode.force_raw, 1, (size_t)c->nn + 1);
    free(u->kcode.out.p); free(u->kcode.lmap); memset(&u->kcode.out, 0, sizeof u->kcode.out); u->kcode.lmap = NULL; u->kcode.nl = u->kcode.cl = 0; u->kcode.covered = 0;
    cgx_ic_line(&u->kcode, 0, 0, "# codegen incremental reader");
    for (int i = 0; i < c->nd[0].nch; i++) cgx_ic_raw(&u->kcode, c, c->nd[0].ch[i], 0);
    memset(&u->g, 0, sizeof u->g);
    u->ok = cgx_parse(&u->g, u->kcode.out.p) != 0; return u->ok;
}
/* intervalo de um no da UIR-H no arquivo original: a instrucao ou declaracao que o originou */
static int cgx_inc_range(const CgxInc *c, const CgxIncUir *u, const CgxNode *n, size_t *b0, size_t *b1, int *l0, int *l1) {
    if (n->k == CGN_PROG) { *b0 = 0; *b1 = c->n; *l0 = 1; *l1 = cgx_inc_lineno(c, c->n); return 1; }
    if (n->line < 1 || n->line > u->kcode.nl) return 0;
    const int nd = u->kcode.lmap[n->line - 1]; if (nd <= 0) return 0;
    *b0 = c->nd[nd].b0; *b1 = cgx_inc_chain_end(c, nd); *l0 = cgx_inc_lineno(c, *b0); *l1 = cgx_inc_lineno(c, *b1 > 0 ? *b1 - 1 : 0);
    return 1;
}

/* ---- edicoes: [a,b) -> texto, aplicadas de uma vez; o que esta fora das edicoes sai byte a byte igual ---- */
typedef struct { size_t a, b; char *text; } CgxEdit;
typedef struct { CgxEdit *e; int n, cap; } CgxEdits;
static void cgx_ed_add(CgxEdits *E, size_t a, size_t b, const char *text) {
    if (E->n == E->cap) { E->cap = E->cap ? E->cap * 2 : 16; E->e = (CgxEdit *)realloc(E->e, sizeof(CgxEdit) * (size_t)E->cap); }
    E->e[E->n].a = a; E->e[E->n].b = b; E->e[E->n].text = strdup(text); E->n++;
}
static void cgx_ed_free(CgxEdits *E) { for (int i = 0; i < E->n; i++) free(E->e[i].text); free(E->e); memset(E, 0, sizeof *E); }
static int cgx_ed_cmp(const void *x, const void *y) { const CgxEdit *a = (const CgxEdit *)x, *b = (const CgxEdit *)y; if (a->a != b->a) return a->a < b->a ? -1 : 1; return a->b < b->b ? -1 : a->b > b->b; }
static int cgx_ed_apply(const char *t, size_t n, CgxEdits *E, CgxB *out, char *err, size_t ecap) {
    qsort(E->e, (size_t)E->n, sizeof(CgxEdit), cgx_ed_cmp);
    for (int i = 1; i < E->n; i++) if (E->e[i].a < E->e[i - 1].b || (E->e[i].a == E->e[i - 1].a && E->e[i].b == E->e[i - 1].b && E->e[i].a != E->e[i].b)) { snprintf(err, ecap, "two edits overlap at byte %zu", E->e[i].a); return 0; }
    size_t pos = 0;
    for (int i = 0; i < E->n; i++) { cgx_bn(out, t + pos, E->e[i].a - pos); cgx_bs(out, E->e[i].text); pos = E->e[i].b; }
    cgx_bn(out, t + pos, n - pos);
    if (!out->p) cgx_bs(out, "");
    return 1;
}

/* ---- especificacao de mudanca ---- */
typedef struct { char cmd[24]; char args[700]; char *payload; int line; } CgxIncOp;
typedef struct { CgxIncOp *op; int n, cap; } CgxIncSpec;
static void cgx_inc_spec_free(CgxIncSpec *s) { for (int i = 0; i < s->n; i++) free(s->op[i].payload); free(s->op); memset(s, 0, sizeof *s); }
static int cgx_inc_spec_parse(const char *src, CgxIncSpec *s, char *err, size_t ecap) {
    memset(s, 0, sizeof *s); int ln = 0; const char *p = src; CgxB cur = {0}; int have = 0;
    for (;;) {
        const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p); ln++;
        char *line = (char *)malloc(l + 1); memcpy(line, p, l); line[l] = 0; if (l && line[l - 1] == '\r') line[--l] = 0;
        const int indented = line[0] == ' ' || line[0] == '\t'; const char *q = line; while (*q == ' ' || *q == '\t') q++;
        if (!*q && !indented) { if (have && cur.n) cgx_bs(&cur, "\n"); }                              /* linha em branco: faz parte da carga se ja ha carga */
        else if (!indented) {
            if (have) { CgxIncOp *o = &s->op[s->n - 1]; while (cur.n && cur.p[cur.n - 1] == '\n') cur.p[--cur.n] = 0; o->payload = cur.p ? cur.p : strdup(""); memset(&cur, 0, sizeof cur); }
            if (*q == '#') { have = 0; free(line); goto next; }
            if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->op = (CgxIncOp *)realloc(s->op, sizeof(CgxIncOp) * (size_t)s->cap); }
            CgxIncOp *o = &s->op[s->n++]; memset(o, 0, sizeof *o); o->line = ln;
            size_t k = 0;
            while (q[k] && q[k] != ' ' && k + 1 < sizeof o->cmd) { o->cmd[k] = q[k]; k++; }
            o->cmd[k] = 0;
            while (q[k] == ' ') k++;
            snprintf(o->args, sizeof o->args, "%s", q + k); have = 1;
        } else {
            if (!have) { snprintf(err, ecap, "line %d: an indented line with no command above it", ln); free(line); free(cur.p); return 0; }
            if (l < 2 || (line[0] != ' ' && line[0] != '\t') || (line[1] != ' ' && line[1] != '\t')) { if (*q) { snprintf(err, ecap, "line %d: the payload of a command is indented by 2 spaces", ln); free(line); free(cur.p); return 0; } }
            cgx_bs(&cur, l >= 2 ? line + 2 : ""); cgx_bs(&cur, "\n");
        }
        free(line);
    next:
        if (!e) break;
        p = e + 1;
    }
    if (have) { CgxIncOp *o = &s->op[s->n - 1]; while (cur.n && cur.p[cur.n - 1] == '\n') cur.p[--cur.n] = 0; o->payload = cur.p ? cur.p : strdup(""); }
    else free(cur.p);
    return 1;
}

/* ---- geracao de trechos (code spec -> Python) ---- */
/* gera o Python de um texto code spec; hdr_lines: linhas de envoltorio antes da carga do autor (para o numero da linha nos erros) */
static int cgx_inc_gen(const char *kcode, int width, int hdr_lines, CgxB *code, int *imports, char *err, size_t ecap) {
    Cgx g; CgxGenOpts o = {width, 1, 4}; int ok = 0; *imports = 0; err[0] = 0;
    if (cgx_parse(&g, kcode) && cgx_check(&g)) {
        CgxB raw = {0}; ok = cgx_backend("python")->gen(&g, &o, &raw, NULL);
        if (ok) {
            const char *q = raw.p ? raw.p : "";
            while (!strncmp(q, "import ", 7)) { if (!strncmp(q, "import math\n", 12)) *imports |= 1; if (!strncmp(q, "import re\n", 10)) *imports |= 2; q = strchr(q, '\n') + 1; }
            while (*q == '\n') q++;
            cgx_bs(code, q);
        }
        for (int i = 0; i < g.nerr && !ok; i++) { const int l = g.err[i].line - hdr_lines; snprintf(err + strlen(err), ecap - strlen(err), "%sspec line %d: %s", i ? "; " : "", l > 0 ? l : g.err[i].line, g.err[i].msg); }
        if (!ok && !err[0]) snprintf(err, ecap, "the generator rejected the specification");
        free(raw.p);
    } else for (int i = 0; i < g.nerr; i++) { const int l = g.err[i].line - hdr_lines; snprintf(err + strlen(err), ecap - strlen(err), "%sspec line %d: %s", i ? "; " : "", l > 0 ? l : g.err[i].line, g.err[i].msg); if (strlen(err) > ecap - 300) break; }
    cgx_free(&g); return ok;
}
static void cgx_inc_indent_lines(const char *txt, const char *ind, int crlf, CgxB *o) {          /* recua cada linha nao vazia; fim de linha do arquivo */
    const char *p = txt; int first = 1;
    while (*p) {
        const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (!first) cgx_bs(o, crlf ? "\r\n" : "\n");
        if (l) { cgx_bs(o, ind); cgx_bn(o, p, l); }
        first = 0; p += l; if (*p == '\n') p++; else break;
        if (!*p) break;
    }
}
static void cgx_inc_trim_nl(CgxB *b) { while (b->n && (b->p[b->n - 1] == '\n' || b->p[b->n - 1] == '\r')) b->p[--b->n] = 0; }
static void cgx_inc_ws_of(const CgxInc *c, size_t b0, char *o, size_t cap) { size_t ls = cgx_inc_ls(c->t, b0), k = 0; while (ls + k < c->n && (c->t[ls + k] == ' ' || c->t[ls + k] == '\t') && k + 1 < cap) { o[k] = c->t[ls + k]; k++; } o[k] = 0; }
/* garante `import NOME` no topo do arquivo */
static void cgx_inc_ensure_import(const CgxInc *c, const char *name, CgxEdits *E) {
    const CgxPyNode *r = &c->nd[0]; int last_imp = -1, doc = -1;
    for (int i = 0; i < r->nch; i++) {
        const CgxPyNode *x = &c->nd[r->ch[i]];
        if (x->kind == PK_STMT && !strcmp(x->kw, "import")) {
            size_t pos = x->c0; CgxPyTok k; while (cgx_pt_next(c->t, x->hb1, &pos, &k)) if (k.k == PT_NAME && (int)(k.b1 - k.b0) == (int)strlen(name) && !strncmp(c->t + k.b0, name, strlen(name)) && (k.b0 == x->c0 + 7 || c->t[k.b0 - 1] == ' ' || c->t[k.b0 - 1] == ',')) return;
            last_imp = r->ch[i];
        } else if (x->kind == PK_STMT && !strcmp(x->kw, "from")) last_imp = r->ch[i];
        else if (i == 0 && cgx_ic_is_doc(c, r->ch[i])) doc = r->ch[i];
    }
    char t[80]; const char *nl = c->crlf ? "\r\n" : "\n";
    if (last_imp >= 0) { snprintf(t, sizeof t, "%simport %s", nl, name); cgx_ed_add(E, c->nd[last_imp].b1, c->nd[last_imp].b1, t); }
    else if (doc >= 0) { snprintf(t, sizeof t, "%s%simport %s", nl, nl, name); cgx_ed_add(E, c->nd[doc].b1, c->nd[doc].b1, t); }
    else { snprintf(t, sizeof t, "import %s%s%s%s", name, nl, nl, nl); cgx_ed_add(E, 0, 0, t); }
}
static void cgx_inc_need_imports(const CgxInc *c, int im, CgxEdits *E) { if (im & 1) cgx_inc_ensure_import(c, "math", E); if (im & 2) cgx_inc_ensure_import(c, "re", E); }

/* ---- operacoes ---- */
typedef struct { char msg[1200]; int nedits; } CgxIncRep;
static int cgx_inc_eq_slice(const char *t, size_t a, size_t b, const char *txt, int crlf) {        /* o texto novo ja e o que esta no arquivo? */
    CgxB o = {0}; for (size_t i = a; i < b; i++) { if (t[i] == '\r' && crlf) continue; cgx_bn(&o, &t[i], 1); }
    const int eq = strlen(txt) == o.n && !memcmp(txt, o.p ? o.p : "", o.n); free(o.p); return eq;
}
static void cgx_inc_crlf(CgxB *b) { /* \n -> \r\n */ CgxB o = {0}; for (size_t i = 0; i < b->n; i++) { if (b->p[i] == '\n') cgx_bs(&o, "\r\n"); else cgx_bn(&o, &b->p[i], 1); } free(b->p); *b = o; }

static int cgx_inc_op_add_function(const CgxInc *c, const CgxIncOp *op, CgxEdits *E, CgxIncRep *rp, char *err, size_t ecap) {
    CgxB code = {0}; int im; char ge[600];
    if (!cgx_inc_gen(op->payload, 88, 0, &code, &im, ge, sizeof ge)) { snprintf(err, ecap, "add-function: %s", ge); free(code.p); return 0; }
    cgx_inc_trim_nl(&code);
    const CgxPyNode *r = &c->nd[0]; int anchor = -1, mode = 0;                                          /* 0 end, 1 after, 2 before */
    char a1[40] = "", a2[300] = ""; sscanf(op->args, "%39s %299s", a1, a2);
    if (!strcmp(a1, "after") || !strcmp(a1, "before")) { anchor = cgx_inc_find(c, a2, ge, sizeof ge); if (anchor < 0) { snprintf(err, ecap, "add-function: %s", ge); free(code.p); return 0; } mode = a1[0] == 'a' ? 1 : 2; if (c->nd[anchor].parent != 0) { snprintf(err, ecap, "add-function: `%s` is not at the top level of the file (use add-method for class members)", a2); free(code.p); return 0; } }
    else if (a1[0] && strcmp(a1, "end")) { snprintf(err, ecap, "add-function: expected `after NAME`, `before NAME` or `end`, found `%s`", a1); free(code.p); return 0; }
    const char *nl = c->crlf ? "\r\n" : "\n"; CgxB ins = {0};
    if (c->crlf) cgx_inc_crlf(&code);
    size_t at;
    if (mode == 2) { at = cgx_inc_attach_start(c, anchor); cgx_bs(&ins, code.p); cgx_bs(&ins, nl); cgx_bs(&ins, nl); cgx_bs(&ins, nl); }
    else {
        if (mode == 0) {
            int last = r->nch ? r->ch[r->nch - 1] : -1;
            if (last >= 0 && !strcmp(c->nd[last].kw, "if") && strstr(c->t + c->nd[last].c0, "__name__") && strstr(c->t + c->nd[last].c0, "__name__") < c->t + c->nd[last].hb1) { at = cgx_inc_attach_start(c, last); cgx_bs(&ins, code.p); cgx_bs(&ins, nl); cgx_bs(&ins, nl); cgx_bs(&ins, nl); goto done; }
            anchor = last;
        }
        if (anchor < 0) { at = c->n; if (c->n && c->t[c->n - 1] != '\n') cgx_bs(&ins, nl); cgx_bs(&ins, code.p); cgx_bs(&ins, nl); }
        else { at = cgx_inc_chain_end(c, anchor); cgx_bs(&ins, nl); cgx_bs(&ins, nl); cgx_bs(&ins, nl); cgx_bs(&ins, code.p); }
    }
done:
    cgx_ed_add(E, at, at, ins.p); cgx_inc_need_imports(c, im, E); rp->nedits++;
    snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "added %d line(s)", 1 + (int)(code.n ? 0 : 0));
    free(code.p); free(ins.p); return 1;
}
/* campos da classe real (self.x usado em qualquer metodo, atribuicao de classe): o validador exige `field` para ler self.x */
static int cgx_inc_class_fields(const CgxInc *c, int cls, CgxB *o) {
    const CgxPyNode *k = &c->nd[cls]; const char *t = c->t; char seen[40][64]; int ns = 0; size_t pos = k->c0; CgxPyTok tk, d, nm; int nf = 0;
    while (cgx_pt_next(t, k->b1, &pos, &tk)) {
        if (tk.k == PT_NAME && cgx_pt_is(t, &tk, "self")) {
            size_t q = pos; if (cgx_pt_next(t, k->b1, &q, &d) && d.k == PT_OP && cgx_pt_is(t, &d, ".") && cgx_pt_next(t, k->b1, &q, &nm) && nm.k == PT_NAME) {
                char w[64]; snprintf(w, sizeof w, "%.*s", (int)(nm.b1 - nm.b0 < 63 ? nm.b1 - nm.b0 : 63), t + nm.b0);
                int dup = 0; for (int i = 0; i < ns; i++) if (!strcmp(seen[i], w)) dup = 1;
                for (int i = 0; i < k->nch; i++) if (c->nd[k->ch[i]].name && !strcmp(c->nd[k->ch[i]].name, w)) dup = 1;
                if (!dup && ns < 40) { snprintf(seen[ns++], 64, "%s", w); cgx_bf(o, "  field %s\n", w); nf++; }
            }
        }
    }
    for (int i = 0; i < k->nch; i++) {                                                       /* atribuicoes no corpo da classe */
        const CgxPyNode *m = &c->nd[k->ch[i]]; if (m->kind != PK_STMT || m->hdr) continue;
        size_t q = m->c0; CgxPyTok a1, a2; if (cgx_pt_next(t, m->hb1, &q, &a1) && a1.k == PT_NAME && cgx_pt_next(t, m->hb1, &q, &a2) && a2.k == PT_OP && cgx_pt_is(t, &a2, "=")) {
            char w[64]; snprintf(w, sizeof w, "%.*s", (int)(a1.b1 - a1.b0 < 63 ? a1.b1 - a1.b0 : 63), t + a1.b0);
            int dup = 0; for (int j = 0; j < ns; j++) if (!strcmp(seen[j], w)) dup = 1;
            if (!dup && ns < 40) { snprintf(seen[ns++], 64, "%s", w); cgx_bf(o, "  field %s\n", w); nf++; }
        }
    }
    return nf;
}
/* `class __W` + campos + a carga indentada; devolve quantas linhas vem antes da carga */
static int cgx_inc_wrap_class(const CgxInc *c, int cls, const char *payload, CgxB *o) {
    CgxB f = {0}; const int nf = cls >= 0 ? cgx_inc_class_fields(c, cls, &f) : 0;
    cgx_bs(o, "class __W\n"); if (f.p) cgx_bs(o, f.p); free(f.p);
    cgx_inc_indent_lines(payload, "  ", 0, o); cgx_bs(o, "\n"); return 1 + nf;
}
/* acha, no Python gerado, a linha `    def NOME(` e devolve o ponteiro para o inicio dela (NULL se nao ha) */
static const char *cgx_inc_find_def(const char *code, const char *name) {
    char key[200]; snprintf(key, sizeof key, "\n    def %s(", name); const char *p = strstr(code, key); return p ? p + 1 : NULL;
}
static void cgx_inc_first_fn_name(const char *payload, char *o, size_t cap) {
    const char *p = payload; o[0] = 0;
    while (*p) { while (*p == ' ') p++; if (!strncmp(p, "fn ", 3) || !strncmp(p, "def ", 4)) { p += p[0] == 'f' ? 3 : 4; while (*p == ' ') p++; size_t k = 0; while ((isalnum((unsigned char)*p) || *p == '_') && k + 1 < cap) o[k++] = *p++; o[k] = 0; return; } const char *e = strchr(p, '\n'); if (!e) break; p = e + 1; }
}
/* gera um metodo (code spec `fn`) como linhas de classe, sem o `class __W:`; ind: recuo do corpo da classe de destino */
static int cgx_inc_gen_method(const CgxInc *c, int cls, const char *payload, const char *ind, const char *unit, int crlf, CgxB *o, int *im, char *err, size_t ecap) {
    CgxB w = {0}, code = {0}; const int hl = cgx_inc_wrap_class(c, cls, payload, &w);
    if (!cgx_inc_gen(w.p, 88, hl, &code, im, err, ecap)) { free(w.p); free(code.p); return 0; }
    free(w.p);
    char nm[100]; cgx_inc_first_fn_name(payload, nm, sizeof nm); const char *p = cgx_inc_find_def(code.p ? code.p : "", nm);
    if (!p) { snprintf(err, ecap, "internal: the generated class has no method `%s`", nm); free(code.p); return 0; }
    CgxB body = {0};
    while (*p) {                                                                                           /* troca os 4 espacos do gerador pelo recuo do destino */
        const char *le = strchr(p, '\n'); size_t l = le ? (size_t)(le - p) : strlen(p);
        if (l) { size_t k = 0; while (k + 4 <= l && !strncmp(p + k, "    ", 4)) k += 4; const size_t lv = k >= 4 ? k / 4 - 1 : 0;
                 cgx_bs(&body, ind); for (size_t q = 0; q < lv; q++) cgx_bs(&body, unit); cgx_bn(&body, p + k, l - k); }
        cgx_bs(&body, "\n"); p += l; if (*p) p++;
    }
    cgx_inc_trim_nl(&body); if (crlf) cgx_inc_crlf(&body);
    cgx_bs(o, body.p ? body.p : ""); free(body.p); free(code.p); return 1;
}
static void cgx_inc_body_unit(const CgxInc *c, int clsidx, char *ind, size_t icap, char *unit, size_t ucap) {
    const CgxPyNode *k = &c->nd[clsidx]; ind[0] = 0; snprintf(unit, ucap, "    ");
    if (k->nch) { cgx_inc_ws_of(c, c->nd[k->ch[0]].c0, ind, icap); char di[64]; cgx_inc_ws_of(c, k->c0, di, sizeof di); const size_t dl = strlen(di), il = strlen(ind); if (il > dl && il - dl < ucap) { memcpy(unit, ind + dl, il - dl); unit[il - dl] = 0; } }
}
static int cgx_inc_op_add_method(const CgxInc *c, const CgxIncOp *op, CgxEdits *E, CgxIncRep *rp, char *err, size_t ecap) {
    char cls[200] = "", a1[40] = "", a2[200] = ""; sscanf(op->args, "%199s %39s %199s", cls, a1, a2); char ge[600];
    const int ci = cgx_inc_find(c, cls, ge, sizeof ge); if (ci < 0) { snprintf(err, ecap, "add-method: %s", ge); return 0; }
    if (c->nd[ci].kind != PK_CLASS) { snprintf(err, ecap, "add-method: `%s` is not a class", cls); return 0; }
    char ind[64], unit[32]; cgx_inc_body_unit(c, ci, ind, sizeof ind, unit, sizeof unit); if (!ind[0]) { snprintf(err, ecap, "add-method: the class `%s` has no body to follow", cls); return 0; }
    CgxB m = {0}; int im;
    if (!cgx_inc_gen_method(c, ci, op->payload, ind, unit, c->crlf, &m, &im, ge, sizeof ge)) { snprintf(err, ecap, "add-method: %s", ge); free(m.p); return 0; }
    int after = -1;
    if (!strcmp(a1, "after")) { char qn[420]; snprintf(qn, sizeof qn, "%s.%s", cls, a2); after = cgx_inc_find(c, qn, ge, sizeof ge); if (after < 0) { snprintf(err, ecap, "add-method: %s", ge); free(m.p); return 0; } }
    else if (a1[0]) { snprintf(err, ecap, "add-method: expected `after NAME`, found `%s`", a1); free(m.p); return 0; }
    else after = c->nd[ci].ch[c->nd[ci].nch - 1];
    const char *nl = c->crlf ? "\r\n" : "\n"; CgxB ins = {0}; cgx_bs(&ins, nl); cgx_bs(&ins, nl); cgx_bs(&ins, m.p);
    const size_t at = cgx_inc_chain_end(c, after); cgx_ed_add(E, at, at, ins.p); cgx_inc_need_imports(c, im, E); rp->nedits++;
    snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "method added to %s", cls);
    free(m.p); free(ins.p); return 1;
}
static int cgx_inc_op_replace_function(const CgxInc *c, const CgxIncOp *op, CgxEdits *E, CgxIncRep *rp, char *err, size_t ecap) {
    char ge[600]; const int x = cgx_inc_find(c, op->args, ge, sizeof ge); if (x < 0) { snprintf(err, ecap, "replace-function: %s", ge); return 0; }
    const CgxPyNode *n = &c->nd[x]; const size_t e = cgx_inc_chain_end(c, x); int im; CgxB nw = {0};
    char ind[64]; cgx_inc_ws_of(c, n->c0, ind, sizeof ind);
    if (c->nd[n->parent].kind == PK_CLASS) {
        char bi[64], unit[32]; cgx_inc_body_unit(c, n->parent, bi, sizeof bi, unit, sizeof unit);
        if (!cgx_inc_gen_method(c, n->parent, op->payload, ind, unit, 0, &nw, &im, ge, sizeof ge)) { snprintf(err, ecap, "replace-function: %s", ge); free(nw.p); return 0; }
    } else {
        CgxB code = {0}; if (!cgx_inc_gen(op->payload, 88, 0, &code, &im, ge, sizeof ge)) { snprintf(err, ecap, "replace-function: %s", ge); free(code.p); return 0; }
        cgx_inc_trim_nl(&code); cgx_inc_indent_lines(code.p ? code.p : "", ind, 0, &nw); free(code.p);
    }
    /* o texto atual, de b0 (inicio da linha, decoradores incluidos) ate o fim */
    if (cgx_inc_eq_slice(c->t, n->b0, e, nw.p ? nw.p : "", c->crlf)) { snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "%s unchanged (no write)", op->args); free(nw.p); return 1; }
    if (c->crlf) cgx_inc_crlf(&nw);
    cgx_ed_add(E, n->b0, e, nw.p ? nw.p : ""); cgx_inc_need_imports(c, im, E); rp->nedits++;
    snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "replaced %s (lines %d-%d)", op->args, n->line0, n->line1);
    free(nw.p); return 1;
}
/* cabecalho `fn nome(params)` a partir do def existente; mais envoltorio do corpo code spec */
static int cgx_inc_wrap_fn(const CgxInc *c, int fx, const char *extra_vars, const char *payload, CgxB *o, char *err, size_t ecap) {
    CgxIncParam ps[24]; int np; size_t ob, cb; char *ret = NULL; const CgxPyNode *n = &c->nd[fx];
    if (n->oneliner) { snprintf(err, ecap, "`%s` is a one-line def; use replace-function", n->name); return 0; }
    if (!cgx_inc_params(c, fx, ps, 24, &np, &ob, &cb, &ret)) { snprintf(err, ecap, "the signature of `%s` has *args, **kwargs or other forms the generator cannot restate; use insert-py or replace-function", n->name); free(ret); return 0; }
    cgx_bf(o, "fn %s(", n->name); int first = 1; const int ismeth = c->nd[n->parent].kind == PK_CLASS;
    for (int i = 0; i < np; i++) {
        if (i == 0 && ismeth && (!strcmp(ps[i].name, "self") || !strcmp(ps[i].name, "cls"))) continue;
        char tyb[100]; const char *ty = ps[i].ann[0] ? cgx_inc_ty(ps[i].ann, tyb, sizeof tyb) : NULL;
        cgx_bf(o, "%s%s", first ? "" : ", ", ps[i].name); if (ty) cgx_bf(o, ": %s", ty); if (ps[i].def) cgx_bf(o, " = %s", ps[i].def); first = 0;
    }
    if (extra_vars) { char v[300]; snprintf(v, sizeof v, "%s", extra_vars); for (char *t = strtok(v, " ,"); t; t = strtok(NULL, " ,")) { cgx_bf(o, "%s%s", first ? "" : ", ", t); first = 0; } }
    cgx_bs(o, ")");
    if (ret) { char tyb[100]; const char *ty = cgx_inc_ty(ret, tyb, sizeof tyb); if (ty) cgx_bf(o, " -> %s", ty); }
    cgx_bs(o, "\n"); cgx_inc_indent_lines(payload, "  ", 0, o); cgx_bs(o, "\n");
    cgx_inc_params_free(ps, np); free(ret); (void)err; (void)ecap; return 1;
}
/* corpo gerado (sem o cabecalho), com o recuo do corpo existente: depth 1 -> bi, depth k -> bi + (k-1) unidades */
static int cgx_inc_gen_body(const CgxInc *c, int fx, const char *extra_vars, const char *payload, const char *bi, const char *unit, CgxB *body, int *im, char *err, size_t ecap) {
    CgxB w = {0}, code = {0};
    if (!cgx_inc_wrap_fn(c, fx, extra_vars, payload, &w, err, ecap)) { free(w.p); return 0; }
    const int ismeth = c->nd[c->nd[fx].parent].kind == PK_CLASS; char ge[700];
    CgxB w2 = {0}; int hl = 1;
    if (ismeth) hl = cgx_inc_wrap_class(c, c->nd[fx].parent, w.p, &w2) + 1; else cgx_bs(&w2, w.p);
    free(w.p);
    if (!cgx_inc_gen(w2.p, 100000, hl, &code, im, ge, sizeof ge)) { snprintf(err, ecap, "%s", ge); free(w2.p); free(code.p); return 0; }
    free(w2.p);
    const char *p = code.p ? code.p : "";
    if (ismeth) { p = cgx_inc_find_def(p, c->nd[fx].name); if (!p) { free(code.p); snprintf(err, ecap, "internal: the generated class has no method `%s`", c->nd[fx].name); return 0; } }
    { const char *e = strchr(p, '\n'); if (!e) { free(code.p); return 0; } p = e + 1; }                  /* pula `def ...:` */
    while (*p) {
        const char *le = strchr(p, '\n'); size_t l = le ? (size_t)(le - p) : strlen(p);
        if (l) { size_t k = 0; while (k + 4 <= l && !strncmp(p + k, "    ", 4)) k += 4; const size_t lv = ismeth && k >= 4 ? k / 4 - 1 : k / 4; cgx_bs(body, bi); for (size_t q = 1; q < lv; q++) cgx_bs(body, unit); cgx_bn(body, p + k, l - k); }
        cgx_bs(body, "\n"); p += l; if (*p) p++;
    }
    cgx_inc_trim_nl(body); free(code.p); return 1;
}
static void cgx_inc_body_indent(const CgxInc *c, int fx, char *bi, size_t bcap, char *unit, size_t ucap) {
    const CgxPyNode *n = &c->nd[fx]; char di[64]; cgx_inc_ws_of(c, n->c0, di, sizeof di);
    cgx_inc_ws_of(c, c->nd[n->ch[0]].c0, bi, bcap); snprintf(unit, ucap, "    ");
    const size_t dl = strlen(di), il = strlen(bi); if (il > dl && il - dl < ucap) { memcpy(unit, bi + dl, il - dl); unit[il - dl] = 0; }
}
static int cgx_inc_op_replace_body(const CgxInc *c, const CgxIncOp *op, CgxEdits *E, CgxIncRep *rp, char *err, size_t ecap) {
    char ge[700]; const int x = cgx_inc_find(c, op->args, ge, sizeof ge); if (x < 0) { snprintf(err, ecap, "replace-body: %s", ge); return 0; }
    const CgxPyNode *n = &c->nd[x]; if (n->kind != PK_DEF || n->oneliner || !n->nch) { snprintf(err, ecap, "replace-body: `%s` has no multi-line body to replace", op->args); return 0; }
    char bi[64], unit[32]; cgx_inc_body_indent(c, x, bi, sizeof bi, unit, sizeof unit); CgxB body = {0}; int im;
    if (!cgx_inc_gen_body(c, x, NULL, op->payload, bi, unit, &body, &im, ge, sizeof ge)) { snprintf(err, ecap, "replace-body: %s", ge); free(body.p); return 0; }
    const int keepdoc = cgx_ic_is_doc(c, n->ch[0]) && strncmp(op->payload, "doc ", 4) != 0;
    size_t from = keepdoc ? c->nd[n->ch[0]].b1 : n->hb1; const size_t to = n->b1;
    if (keepdoc && n->nch == 1 && !body.n) { free(body.p); return 1; }
    if (c->crlf) cgx_inc_crlf(&body);
    CgxB ins = {0}; if (body.n) { cgx_bs(&ins, c->crlf ? "\r\n" : "\n"); cgx_bs(&ins, body.p); } else if (!keepdoc) { cgx_bs(&ins, c->crlf ? "\r\n" : "\n"); cgx_bs(&ins, bi); cgx_bs(&ins, "pass"); }
    if (cgx_inc_eq_slice(c->t, from, to, ins.p ? ins.p : "", c->crlf)) { snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "%s body unchanged (no write)", op->args); free(body.p); free(ins.p); return 1; }
    /* fim do cabecalho: se ha comentario na mesma linha do `:` ele fica (hb1 ja inclui) */
    cgx_ed_add(E, from, to, ins.p ? ins.p : ""); cgx_inc_need_imports(c, im, E); rp->nedits++;
    snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "body of %s replaced%s", op->args, keepdoc ? " (docstring kept)" : "");
    free(body.p); free(ins.p); return 1;
}
static int cgx_inc_op_insert(const CgxInc *c, const CgxIncOp *op, int py, CgxEdits *E, CgxIncRep *rp, char *err, size_t ecap) {
    char fn[200] = "", pos[24] = "", ge[700]; int num = 0; const char *rest = op->args;
    { int k = 0; while (*rest == ' ') rest++; while (*rest && *rest != ' ' && k < 199) fn[k++] = *rest++; fn[k] = 0; while (*rest == ' ') rest++; k = 0; while (*rest && *rest != ' ' && k < 23) pos[k++] = *rest++; pos[k] = 0; while (*rest == ' ') rest++; }
    if (!strcmp(pos, "before") || !strcmp(pos, "after")) { num = atoi(rest); while (isdigit((unsigned char)*rest)) rest++; while (*rest == ' ') rest++; if (num < 1) { snprintf(err, ecap, "%s: `%s` needs a statement number from 1", op->cmd, pos); return 0; } }
    else if (strcmp(pos, "start") && strcmp(pos, "end")) { snprintf(err, ecap, "%s: expected `start`, `end`, `before N` or `after N`, found `%s`", op->cmd, pos); return 0; }
    const char *vars = NULL; if (!strncmp(rest, "vars ", 5)) vars = rest + 5;
    const int x = cgx_inc_find(c, fn, ge, sizeof ge); if (x < 0) { snprintf(err, ecap, "%s: %s", op->cmd, ge); return 0; }
    const CgxPyNode *n = &c->nd[x]; if (n->kind != PK_DEF || n->oneliner || !n->nch) { snprintf(err, ecap, "%s: `%s` has no multi-line body", op->cmd, fn); return 0; }
    int st[512]; const int ns = cgx_inc_body_stmts(c, x, st, 512); char bi[64], unit[32]; cgx_inc_body_indent(c, x, bi, sizeof bi, unit, sizeof unit);
    CgxB body = {0}; int im = 0;
    if (py) {                                                                                   /* Python como esta: tira o recuo comum e poe o do corpo */
        const char *p = op->payload; size_t minind = 1000; CgxB lines = {0};
        for (const char *q = p; *q;) { const char *e = strchr(q, '\n'); size_t l = e ? (size_t)(e - q) : strlen(q); size_t k = 0; while (k < l && q[k] == ' ') k++; if (k < l && k < minind) minind = k; q += l; if (*q) q++; else break; }
        if (minind == 1000) minind = 0;
        for (const char *q = p; *q;) { const char *e = strchr(q, '\n'); size_t l = e ? (size_t)(e - q) : strlen(q); if (l > minind) { cgx_bs(&lines, bi); cgx_bn(&lines, q + minind, l - minind); } cgx_bs(&lines, "\n"); q += l; if (*q) q++; else break; }
        cgx_inc_trim_nl(&lines); body = lines;
    } else if (!cgx_inc_gen_body(c, x, vars, op->payload, bi, unit, &body, &im, ge, sizeof ge)) { snprintf(err, ecap, "%s: %s", op->cmd, ge); free(body.p); return 0; }
    if (c->crlf) cgx_inc_crlf(&body);
    const char *nl = c->crlf ? "\r\n" : "\n"; size_t at; CgxB ins = {0}; int before = 0, idx = -1;
    int first = 0; if (cgx_ic_is_doc(c, st[0])) first = 1;
    if (!strcmp(pos, "start")) { if (first && ns == 1) { idx = 0; } else { idx = first; before = 1; } if (first && ns == 1) { before = 0; } }
    else if (!strcmp(pos, "end")) { idx = ns - 1; }
    else { if (num > ns) { snprintf(err, ecap, "%s: `%s` has only %d statement(s) in its body", op->cmd, fn, ns); free(body.p); return 0; } idx = num - 1; before = !strcmp(pos, "before"); }
    if (before) { at = cgx_inc_attach_start(c, st[idx]); if (at < n->hb1 + 1) at = cgx_inc_ls(c->t, c->nd[st[idx]].c0); cgx_bs(&ins, body.p); cgx_bs(&ins, nl); }
    else { at = cgx_inc_chain_end(c, st[idx]); cgx_bs(&ins, nl); cgx_bs(&ins, body.p); }
    cgx_ed_add(E, at, at, ins.p); cgx_inc_need_imports(c, im, E); rp->nedits++;
    snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "inserted into %s", fn);
    free(body.p); free(ins.p); return 1;
}
static int cgx_inc_op_delete(const CgxInc *c, const CgxIncOp *op, CgxEdits *E, CgxIncRep *rp, char *err, size_t ecap) {
    char ge[600]; const int x = cgx_inc_find(c, op->args, ge, sizeof ge); if (x < 0) { snprintf(err, ecap, "delete-function: %s", ge); return 0; }
    const CgxPyNode *n = &c->nd[x], *p = &c->nd[n->parent]; const size_t s = cgx_inc_attach_start(c, x), e = cgx_inc_chain_end(c, x);
    int pos = 0; while (p->ch[pos] != x) pos++;
    size_t b = e < c->n ? e + 1 : e;                                                                  /* a quebra do fim da ultima linha */
    if (pos + 1 < p->nch) b = cgx_inc_ls(c->t, c->nd[p->ch[pos + 1]].c0), b = cgx_inc_attach_start(c, p->ch[pos + 1]);      /* some ate o proximo (as linhas em branco depois vao juntas) */
    else if (pos > 0) { cgx_ed_add(E, c->nd[p->ch[pos - 1]].b1 > 0 ? cgx_inc_chain_end(c, p->ch[pos - 1]) : s, e, ""); rp->nedits++; snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "deleted %s", op->args); return 1; }
    else b = c->n;
    cgx_ed_add(E, s, b, ""); rp->nedits++;
    snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "deleted %s (lines %d-%d)", op->args, n->line0, n->line1);
    return 1;
}

/* ---- renomear ---- */
/* o def/metodo mais interno que contem a posicao off (indice) ou 0 (modulo) */
static int cgx_inc_enclosing_def(const CgxInc *c, size_t off) {
    int best = 0;
    for (int i = 1; i < c->nn; i++) { const CgxPyNode *x = &c->nd[i]; if (x->kind == PK_DEF && off >= x->b0 && off < x->b1 && (best == 0 || x->indent > c->nd[best].indent)) best = i; }
    return best;
}
/* o def liga o nome (parametro, atribuicao, for, as)? entao o nome ali e outra coisa e nao se renomeia */
static int cgx_inc_binds(const CgxInc *c, int dx, const char *name) {
    const char *t = c->t; const CgxPyNode *n = &c->nd[dx]; size_t pos = n->c0; CgxPyTok k, pv; const size_t nl = strlen(name); int depth = 0, atline = 1; pv.k = PT_END; pv.b0 = pv.b1 = 0;
    CgxIncParam ps[24]; int np; size_t ob, cb; char *ret = NULL;
    if (cgx_inc_params(c, dx, ps, 24, &np, &ob, &cb, &ret)) { for (int i = 0; i < np; i++) if (!strcmp(ps[i].name, name)) { cgx_inc_params_free(ps, np); free(ret); return 1; } cgx_inc_params_free(ps, np); }
    free(ret);
    size_t last_line_start = n->hb1;
    while (cgx_pt_next(t, n->b1, &pos, &k)) {
        const int line_start = k.b0 > last_line_start && memchr(t + last_line_start, '\n', k.b0 - last_line_start) != NULL && depth == 0;
        if (line_start) atline = 1;
        if (k.k == PT_OP && k.b1 - k.b0 == 1) { if (strchr("([{", t[k.b0])) depth++; else if (strchr(")]}", t[k.b0])) depth--; }
        if (k.k == PT_NAME && k.b1 - k.b0 == nl && !memcmp(t + k.b0, name, nl) && k.b0 > n->hb1) {
            size_t q = k.b1; CgxPyTok nx; const int more = cgx_pt_next(t, n->b1, &q, &nx);
            if (more && nx.k == PT_OP && (cgx_pt_is(t, &nx, "=") || cgx_pt_is(t, &nx, "+=") || cgx_pt_is(t, &nx, "-=")) && atline && depth == 0) return 1;
            if (pv.k == PT_NAME && (cgx_pt_is(t, &pv, "for") || cgx_pt_is(t, &pv, "as"))) return 1;
        }
        atline = 0; pv = k; last_line_start = k.b1;
    }
    return 0;
}
static int cgx_inc_op_rename(const CgxInc *c, const CgxIncOp *op, CgxEdits *E, CgxIncRep *rp, char *err, size_t ecap) {
    char old[200] = "", nw[120] = "", ge[600]; sscanf(op->args, "%199s %119s", old, nw);
    if (!old[0] || !nw[0]) { snprintf(err, ecap, "rename: expected `rename OLD NEW`"); return 0; }
    for (const char *q = nw; *q; q++) if (!isalnum((unsigned char)*q) && *q != '_') { snprintf(err, ecap, "rename: `%s` is not a valid name", nw); return 0; }
    const int dx = cgx_inc_find(c, old, ge, sizeof ge); if (dx < 0) { snprintf(err, ecap, "rename: %s", ge); return 0; }
    const int method = c->nd[dx].parent != 0; const char *oname = c->nd[dx].name; const size_t ol = strlen(oname);
    { const CgxPyNode *p = &c->nd[c->nd[dx].parent]; for (int i = 0; i < p->nch; i++) if (c->nd[p->ch[i]].name && !strcmp(c->nd[p->ch[i]].name, nw)) { snprintf(err, ecap, "rename: `%s` is already defined in %s (line %d)", nw, c->nd[dx].parent ? c->nd[c->nd[dx].parent].name : "the file", c->nd[p->ch[i]].line0); return 0; } }
    const char *t = c->t; size_t pos = 0; CgxPyTok k, pv, nx; pv.k = PT_END; int depth = 0, nren = 0, nskip_shadow = 0, nskip_kw = 0, nskip_attr = 0, nskip_imp = 0;
    unsigned char *bind = (unsigned char *)calloc((size_t)c->nn + 1, 1), *bindk = (unsigned char *)calloc((size_t)c->nn + 1, 1);
    while (cgx_pt_next(t, c->n, &pos, &k)) {
        if (k.k == PT_OP && k.b1 - k.b0 == 1) { if (strchr("([{", t[k.b0])) depth++; else if (strchr(")]}", t[k.b0])) depth--; }
        if (k.k == PT_NAME && k.b1 - k.b0 == ol && !memcmp(t + k.b0, oname, ol)) {
            size_t q = pos; const int more = cgx_pt_next(t, c->n, &q, &nx);
            const int after_dot = pv.k == PT_OP && cgx_pt_is(t, &pv, ".");
            const int after_def = pv.k == PT_NAME && (cgx_pt_is(t, &pv, "def") || cgx_pt_is(t, &pv, "class"));
            int doit = 0;
            const size_t ls = cgx_inc_ls(t, k.b0); size_t fk = ls; while (fk < k.b0 && (t[fk] == ' ' || t[fk] == '\t')) fk++;
            const int on_import = !strncmp(t + fk, "import ", 7) || !strncmp(t + fk, "from ", 5);
            if (on_import) nskip_imp++;
            else if (after_def) { const int own = cgx_inc_enclosing_def(c, k.b0); (void)own; /* a definicao e a certa se o no dela e dx */
                for (int i = 1; i < c->nn; i++) if (c->nd[i].kind != PK_STMT && i == dx && k.b0 >= c->nd[i].c0 && k.b0 < c->nd[i].hb1) doit = 1; }
            else if (after_dot) { if (method) doit = 1; else nskip_attr++; }
            else if (!method && more && nx.k == PT_OP && cgx_pt_is(t, &nx, "=") && depth > 0) nskip_kw++;
            else if (!method) {
                const int ed = cgx_inc_enclosing_def(c, k.b0);
                if (ed && ed != dx) { if (!bindk[ed]) { bindk[ed] = 1; bind[ed] = (unsigned char)cgx_inc_binds(c, ed, oname); } }
                if (ed && ed != dx && bind[ed]) nskip_shadow++; else doit = 1;
            } else { /* metodo: um nome solto igual ao do metodo nao e o metodo */ }
            if (doit) { cgx_ed_add(E, k.b0, k.b1, nw); nren++; }
        }
        pv = k;
    }
    free(bind); free(bindk);
    if (!nren) { snprintf(err, ecap, "rename: nothing to rename"); return 0; }
    rp->nedits += nren;
    snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "renamed %s -> %s in %d place(s)", old, nw, nren);
    if (nskip_shadow) snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "; %d left alone (another variable of that name in the function)", nskip_shadow);
    if (nskip_kw) snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "; %d keyword argument(s) left alone", nskip_kw);
    if (nskip_attr) snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "; %d attribute use(s) `.%s` left alone", nskip_attr, oname);
    if (nskip_imp) snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "; %d import line(s) left alone", nskip_imp);
    if (method) snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "; every `.%s` was renamed (check other classes with that method)", oname);
    return 1;
}
/* ---- listas entre parenteses: acrescentar um item de forma que o diff seja so ele ---- */
static void cgx_inc_list_append(const CgxInc *c, size_t open, size_t close, const char *item, CgxEdits *E) {
    const char *t = c->t; size_t pos = open + 1; CgxPyTok k; int depth = 0; size_t last_end = 0, last_start = 0, trail = 0; int any = 0, multiline = 0, at_start = 1;
    if (memchr(t + open, '\n', close - open)) multiline = 1;
    while (cgx_pt_next(t, close, &pos, &k)) {
        const int isop = k.k == PT_OP && k.b1 - k.b0 == 1; const char ch = isop ? t[k.b0] : 0;
        if (isop && strchr("([{", ch)) depth++; else if (isop && strchr(")]}", ch)) depth--;
        if (depth == 0 && ch == ',') { trail = k.b0; at_start = 1; continue; }
        if (at_start && depth >= 0) { last_start = k.b0; at_start = 0; }
        last_end = k.b1; trail = 0; any = 1;
    }
    CgxB o = {0}; const char *nl = c->crlf ? "\r\n" : "\n";
    if (!any) { cgx_bs(&o, item); cgx_ed_add(E, open + 1, open + 1, o.p); free(o.p); return; }
    if (!multiline) { if (trail) { cgx_bf(&o, " %s,", item); cgx_ed_add(E, trail + 1, trail + 1, o.p); } else { cgx_bf(&o, ", %s", item); cgx_ed_add(E, last_end, last_end, o.p); } free(o.p); return; }
    char ind[64]; cgx_inc_ws_of(c, last_start, ind, sizeof ind);
    { const size_t ls = cgx_inc_ls(t, last_start); size_t k2 = ls; while (k2 < last_start && (t[k2] == ' ' || t[k2] == '\t')) k2++; if (k2 < last_start) { /* o ultimo item divide a linha com o abre-parenteses: recuo = o da linha */ } }
    if (trail) { cgx_bf(&o, "%s%s%s,", nl, ind, item); cgx_ed_add(E, trail + 1, trail + 1, o.p); }
    else { cgx_bf(&o, ",%s%s%s", nl, ind, item); cgx_ed_add(E, last_end, last_end, o.p); }
    free(o.p);
}
static size_t cgx_inc_match_paren(const CgxInc *c, size_t open) {
    size_t pos = open + 1; CgxPyTok k; int depth = 1;
    while (cgx_pt_next(c->t, c->n, &pos, &k)) { if (k.k == PT_OP && k.b1 - k.b0 == 1) { if (strchr("([{", c->t[k.b0])) depth++; else if (strchr(")]}", c->t[k.b0]) && --depth == 0) return k.b0; } }
    return 0;
}
static int cgx_inc_op_add_param(const CgxInc *c, const CgxIncOp *op, CgxEdits *E, CgxIncRep *rp, char *err, size_t ecap) {
    char fn[200] = "", ge[600]; const char *rest = op->args; { int k = 0; while (*rest == ' ') rest++; while (*rest && *rest != ' ' && k < 199) fn[k++] = *rest++; fn[k] = 0; while (*rest == ' ') rest++; }
    char ptxt[720]; snprintf(ptxt, sizeof ptxt, "%s", rest); char callx[720] = ""; char *cl = strstr(ptxt, " call=");
    if (cl) { snprintf(callx, sizeof callx, "%s", cl + 6); *cl = 0; }
    size_t pl = strlen(ptxt); while (pl && ptxt[pl - 1] == ' ') ptxt[--pl] = 0;
    if (!ptxt[0]) { snprintf(err, ecap, "add-param: expected `add-param FUNCTION name: type = default [call=EXPR]`"); return 0; }
    char pname[80]; { size_t k = 0; while (isalnum((unsigned char)ptxt[k]) || ptxt[k] == '_') { if (k < 79) pname[k] = ptxt[k]; k++; } pname[k < 79 ? k : 79] = 0; if (!k) { snprintf(err, ecap, "add-param: the parameter must start with its name"); return 0; } }
    const int hasdef = strchr(ptxt, '=') != NULL;
    const int fx = cgx_inc_find(c, fn, ge, sizeof ge); if (fx < 0) { snprintf(err, ecap, "add-param: %s", ge); return 0; }
    const CgxPyNode *n = &c->nd[fx]; if (n->kind != PK_DEF) { snprintf(err, ecap, "add-param: `%s` is a class, not a function", fn); return 0; }
    /* abre/fecha do cabecalho e cada parametro (inicio, tem default) */
    size_t pos = n->c0, open = 0, close = 0, star = 0, sstart = 0; CgxPyTok k; int depth = 0, anydef = 0, first = 1, started = 0; size_t seg = 0;
    while (cgx_pt_next(c->t, n->hb1, &pos, &k)) {
        if (!started) { if (k.k == PT_OP && cgx_pt_is(c->t, &k, "(")) { started = 1; open = k.b0; depth = 1; seg = k.b1; } continue; }
        const int isop = k.k == PT_OP && k.b1 - k.b0 == 1; const char ch = isop ? c->t[k.b0] : 0;
        if (isop && strchr("([{", ch)) depth++;
        else if (isop && strchr(")]}", ch)) { depth--; if (depth == 0) { close = k.b0; break; } }
        if (depth == 1 && first && k.k != PT_END) { first = 0; sstart = k.b0; (void)sstart; }
        if (depth == 1 && (k.k == PT_OP) && (cgx_pt_is(c->t, &k, "*") || cgx_pt_is(c->t, &k, "**") || cgx_pt_is(c->t, &k, "/")) && !star) { size_t before = k.b0; /* so conta se abre um parametro (depois de `(` ou `,`) */ size_t b = before; while (b > seg && (c->t[b - 1] == ' ' || c->t[b - 1] == '\n' || c->t[b - 1] == '\t' || c->t[b - 1] == '\r')) b--; if (b == seg || c->t[b - 1] == ',' || c->t[b - 1] == '(') star = k.b0; }
        if (depth == 1 && isop && ch == ',') { seg = k.b1; }
        if (depth == 1 && isop && ch == '=' && !star) anydef = 1;
        if (depth == 1 && k.k == PT_NAME && (int)(k.b1 - k.b0) == (int)strlen(pname) && !strncmp(c->t + k.b0, pname, strlen(pname))) {
            size_t b = k.b0; while (b > seg && (c->t[b - 1] == ' ' || c->t[b - 1] == '\n' || c->t[b - 1] == '\t' || c->t[b - 1] == '\r')) b--;
            if (b == seg) { snprintf(err, ecap, "add-param: `%s` already has a parameter `%s`", fn, pname); return 0; }
        }
    }
    if (!close) { snprintf(err, ecap, "add-param: cannot read the header of `%s`", fn); return 0; }
    if (!hasdef && anydef) { snprintf(err, ecap, "add-param: `%s` has parameters with defaults, so a new parameter needs a default too (write `%s = ...`) or Python rejects the order", fn, pname); return 0; }
    int n0 = E->n;
    if (star) {                                                                                   /* antes de *args / ** / `/`: o item e o que vem antes da estrela */
        CgxB o = {0}; const int ml = memchr(c->t + open, '\n', close - open) != NULL; char ind[64]; cgx_inc_ws_of(c, star, ind, sizeof ind);
        if (ml) cgx_bf(&o, "%s,%s%s", ptxt, c->crlf ? "\r\n" : "\n", ind); else cgx_bf(&o, "%s, ", ptxt);
        cgx_ed_add(E, star, star, o.p); free(o.p);
    } else cgx_inc_list_append(c, open, close, ptxt, E);
    int ncalls = 0;
    if (callx[0]) {                                                                                /* chamadas: `nome=EXPR` */
        const int ismeth = c->nd[n->parent].kind == PK_CLASS; const char *t = c->t; size_t p2 = 0; CgxPyTok pv, nx; pv.k = PT_END;
        char item[820]; snprintf(item, sizeof item, "%s=%s", pname, callx);
        while (cgx_pt_next(t, c->n, &p2, &k)) {
            if (k.k == PT_NAME && (int)(k.b1 - k.b0) == (int)strlen(n->name) && !strncmp(t + k.b0, n->name, strlen(n->name))) {
                size_t q = p2; const int more = cgx_pt_next(t, c->n, &q, &nx);
                const int after_dot = pv.k == PT_OP && cgx_pt_is(t, &pv, "."); const int after_def = pv.k == PT_NAME && (cgx_pt_is(t, &pv, "def") || cgx_pt_is(t, &pv, "class"));
                if (more && nx.k == PT_OP && cgx_pt_is(t, &nx, "(") && !after_def && (ismeth ? after_dot : !after_dot)) {
                    const size_t cb = cgx_inc_match_paren(c, nx.b0);
                    if (cb) {
                        int dup = 0; size_t p3 = nx.b1; CgxPyTok z, zn; int d3 = 0;
                        while (cgx_pt_next(t, cb, &p3, &z)) { if (z.k == PT_OP && z.b1 - z.b0 == 1) { if (strchr("([{", t[z.b0])) d3++; else if (strchr(")]}", t[z.b0])) d3--; } size_t p4 = p3; if (d3 == 0 && z.k == PT_NAME && cgx_pt_next(t, cb, &p4, &zn) && zn.k == PT_OP && cgx_pt_is(t, &zn, "=") && (int)(z.b1 - z.b0) == (int)strlen(pname) && !strncmp(t + z.b0, pname, strlen(pname))) dup = 1; }
                        if (!dup) { cgx_inc_list_append(c, nx.b0, cb, item, E); ncalls++; }
                    }
                }
            }
            pv = k;
        }
    }
    rp->nedits += E->n - n0;
    snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "added parameter `%s` to %s", pname, fn);
    if (callx[0]) snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), "; %d call(s) updated", ncalls);
    else if (hasdef) snprintf(rp->msg + strlen(rp->msg), sizeof rp->msg - strlen(rp->msg), " (calls keep working: it has a default)");
    return 1;
}

/* ---- aplicar a especificacao: cada operacao sobre o texto que a anterior deixou ---- */
typedef struct { char *text; size_t n; CgxIncRep rep[64]; int nrep; int changed; char err[1000]; int failed_op; } CgxIncResult;
static void cgx_inc_result_free(CgxIncResult *r) { free(r->text); memset(r, 0, sizeof *r); }
static int cgx_inc_apply_spec(const char *src, size_t n, const char *spec, CgxIncResult *R) {
    memset(R, 0, sizeof *R); CgxIncSpec S; char e[900] = "";
    if (!cgx_inc_spec_parse(spec, &S, e, sizeof e)) { snprintf(R->err, sizeof R->err, "%s", e); return 0; }
    char *cur = (char *)malloc(n + 1); memcpy(cur, src, n); cur[n] = 0; size_t cn = n;
    for (int i = 0; i < S.n && i < 64; i++) {
        CgxInc c; const CgxIncOp *op = &S.op[i]; CgxEdits E = {0}; CgxIncRep *rp = &R->rep[R->nrep++]; memset(rp, 0, sizeof *rp);
        if (!cgx_inc_parse(&c, cur, cn)) { snprintf(R->err, sizeof R->err, "op %d (%s): the file is outside the supported subset (line %d: %s); nothing was changed", i + 1, op->cmd, c.errline, c.err); cgx_inc_free(&c); cgx_inc_spec_free(&S); free(cur); R->failed_op = i + 1; return 0; }
        int ok;
        if (!strcmp(op->cmd, "add-function")) ok = cgx_inc_op_add_function(&c, op, &E, rp, e, sizeof e);
        else if (!strcmp(op->cmd, "add-method")) ok = cgx_inc_op_add_method(&c, op, &E, rp, e, sizeof e);
        else if (!strcmp(op->cmd, "replace-function")) ok = cgx_inc_op_replace_function(&c, op, &E, rp, e, sizeof e);
        else if (!strcmp(op->cmd, "replace-body")) ok = cgx_inc_op_replace_body(&c, op, &E, rp, e, sizeof e);
        else if (!strcmp(op->cmd, "rename")) ok = cgx_inc_op_rename(&c, op, &E, rp, e, sizeof e);
        else if (!strcmp(op->cmd, "add-param")) ok = cgx_inc_op_add_param(&c, op, &E, rp, e, sizeof e);
        else if (!strcmp(op->cmd, "insert")) ok = cgx_inc_op_insert(&c, op, 0, &E, rp, e, sizeof e);
        else if (!strcmp(op->cmd, "insert-py")) ok = cgx_inc_op_insert(&c, op, 1, &E, rp, e, sizeof e);
        else if (!strcmp(op->cmd, "delete-function")) ok = cgx_inc_op_delete(&c, op, &E, rp, e, sizeof e);
        else { snprintf(e, sizeof e, "unknown command `%s` (expected add-function, add-method, replace-function, replace-body, rename, add-param, insert, insert-py, delete-function)", op->cmd); ok = 0; }
        if (!ok) { snprintf(R->err, sizeof R->err, "spec line %d: %s; nothing was changed", op->line, e); cgx_ed_free(&E); cgx_inc_free(&c); cgx_inc_spec_free(&S); free(cur); R->failed_op = i + 1; return 0; }
        CgxB out = {0};
        if (E.n) {
            if (!cgx_ed_apply(cur, cn, &E, &out, e, sizeof e)) { snprintf(R->err, sizeof R->err, "spec line %d: %s; nothing was changed", op->line, e); free(out.p); cgx_ed_free(&E); cgx_inc_free(&c); cgx_inc_spec_free(&S); free(cur); return 0; }
            CgxInc c2; const int valid = cgx_inc_parse(&c2, out.p, out.n);
            if (!valid) { snprintf(R->err, sizeof R->err, "spec line %d: the result would not be valid Python (line %d: %s); nothing was changed", op->line, c2.errline, c2.err); cgx_inc_free(&c2); free(out.p); cgx_ed_free(&E); cgx_inc_free(&c); cgx_inc_spec_free(&S); free(cur); return 0; }
            cgx_inc_free(&c2); free(cur); cur = out.p; cn = out.n; R->changed = 1;
        }
        cgx_ed_free(&E); cgx_inc_free(&c);
    }
    cgx_inc_spec_free(&S); R->text = cur; R->n = cn; R->changed = !(cn == n && !memcmp(cur, src, n));
    return 1;
}

/* ---- da mudanca para acoes por simbolo (o que a gravacao pelo Edit por simbolo recebe) ---- */
enum { CIA_REPLACE, CIA_AFTER, CIA_BEFORE, CIA_DELETE };
typedef struct { int kind; char symbol[220]; char *text; } CgxIncAct;
typedef struct { CgxIncAct *a; int n, cap; } CgxIncActs;
static void cgx_inc_acts_free(CgxIncActs *A) { for (int i = 0; i < A->n; i++) free(A->a[i].text); free(A->a); memset(A, 0, sizeof *A); }
static void cgx_inc_act(CgxIncActs *A, int kind, const char *sym, const char *text, size_t tl) {
    if (A->n == A->cap) { A->cap = A->cap ? A->cap * 2 : 8; A->a = (CgxIncAct *)realloc(A->a, sizeof(CgxIncAct) * (size_t)A->cap); }
    CgxIncAct *x = &A->a[A->n++]; x->kind = kind; snprintf(x->symbol, sizeof x->symbol, "%s", sym); x->text = (char *)malloc(tl + 1); memcpy(x->text, text, tl); x->text[tl] = 0;
}
/* texto de um simbolo sem o recuo da primeira linha (o Edit por simbolo recua sozinho) */
static void cgx_inc_sym_text(const CgxInc *c, int idx, CgxB *o) {
    const size_t a = c->nd[idx].b0, e = cgx_inc_chain_end(c, idx); char ind[64]; cgx_inc_ws_of(c, c->nd[idx].c0, ind, sizeof ind); const size_t il = strlen(ind);
    const char *t = c->t; size_t p = a;
    while (p < e) { size_t le = cgx_inc_le(t, c->n, p); if (le > e) le = e; size_t q = p, k = 0; while (q < le && k < il && (t[q] == ' ' || t[q] == '\t')) { q++; k++; } size_t qe = le; if (qe > q && t[qe - 1] == '\r') qe--; cgx_bn(o, t + q, qe - q); if (le < e) cgx_bs(o, "\n"); p = le + 1; }
    if (!o->p) cgx_bs(o, "");
}
static int cgx_inc_node_text_eq(const CgxInc *a, int x, const CgxInc *b, int y) {
    CgxB p = {0}, q = {0}; cgx_inc_sym_text(a, x, &p); cgx_inc_sym_text(b, y, &q); const int eq = p.n == q.n && !memcmp(p.p, q.p, p.n); free(p.p); free(q.p); return eq;
}
static int cgx_inc_find_named(const CgxInc *c, int parent, const char *name) { const CgxPyNode *p = &c->nd[parent]; for (int i = 0; i < p->nch; i++) if (c->nd[p->ch[i]].name && !strcmp(c->nd[p->ch[i]].name, name)) return p->ch[i]; return -1; }
/* compara os dois arquivos simbolo a simbolo; devolve 0 se a mudanca nao se descreve assim (mudancas de estrutura fora dos simbolos) */
static int cgx_inc_diff(const char *old, size_t on, const char *nw, size_t nn, CgxIncActs *A) {
    CgxInc a, b; int ok = 1;
    if (!cgx_inc_parse(&a, old, on)) return 0;
    if (!cgx_inc_parse(&b, nw, nn)) { cgx_inc_free(&a); return 0; }
    const CgxPyNode *ra = &a.nd[0], *rb = &b.nd[0];
    for (int i = 0; i < ra->nch; i++) {                                                         /* removidos e alterados */
        const CgxPyNode *x = &a.nd[ra->ch[i]]; if (x->kind == PK_STMT) continue;
        const int y = cgx_inc_find_named(&b, 0, x->name);
        if (y < 0) { cgx_inc_act(A, CIA_DELETE, x->name, "", 0); continue; }
        if (cgx_inc_node_text_eq(&a, ra->ch[i], &b, y)) continue;
        if (x->kind == PK_CLASS && b.nd[y].kind == PK_CLASS) {                                  /* classe: desce aos metodos se o miolo nao mudou */
            int same = x->nch == b.nd[y].nch; CgxB h1 = {0}, h2 = {0};
            if (same) { cgx_bn(&h1, a.t + x->b0, a.nd[x->ch[0]].b0 - x->b0); cgx_bn(&h2, b.t + b.nd[y].b0, b.nd[b.nd[y].ch[0]].b0 - b.nd[y].b0); same = h1.n == h2.n && !memcmp(h1.p, h2.p, h1.n); }
            free(h1.p); free(h2.p);
            for (int m = 0; same && m < x->nch; m++) { const CgxPyNode *mm = &a.nd[x->ch[m]], *m2 = &b.nd[b.nd[y].ch[m]]; if (mm->kind != m2->kind || (mm->name ? !m2->name || strcmp(mm->name, m2->name) : m2->name != NULL)) same = 0; if (mm->kind == PK_STMT && !cgx_inc_node_text_eq(&a, x->ch[m], &b, b.nd[y].ch[m])) same = 0; }
            if (same) {
                for (int m = 0; m < x->nch; m++) { const int mi = x->ch[m], mj = b.nd[y].ch[m]; if (a.nd[mi].kind == PK_STMT || cgx_inc_node_text_eq(&a, mi, &b, mj)) continue; CgxB t = {0}; char sym[220]; snprintf(sym, sizeof sym, "%s.%s", x->name, a.nd[mi].name); cgx_inc_sym_text(&b, mj, &t); cgx_inc_act(A, CIA_REPLACE, sym, t.p, t.n); free(t.p); }
                continue;
            }
        }
        CgxB t = {0}; cgx_inc_sym_text(&b, y, &t); cgx_inc_act(A, CIA_REPLACE, x->name, t.p, t.n); free(t.p);
    }
    for (int j = 0; j < rb->nch; j++) {                                                         /* novos: depois do vizinho anterior que ja existia */
        const CgxPyNode *y = &b.nd[rb->ch[j]]; if (y->kind == PK_STMT) continue;
        if (cgx_inc_find_named(&a, 0, y->name) >= 0) continue;
        CgxB t = {0}; cgx_inc_sym_text(&b, rb->ch[j], &t); int placed = 0;
        for (int q = j - 1; q >= 0 && !placed; q--) { const CgxPyNode *pq = &b.nd[rb->ch[q]]; if (pq->kind != PK_STMT && cgx_inc_find_named(&a, 0, pq->name) >= 0) { cgx_inc_act(A, CIA_AFTER, pq->name, t.p, t.n); placed = 1; } }
        for (int q = j + 1; q < rb->nch && !placed; q++) { const CgxPyNode *pq = &b.nd[rb->ch[q]]; if (pq->kind != PK_STMT && cgx_inc_find_named(&a, 0, pq->name) >= 0) { cgx_inc_act(A, CIA_BEFORE, pq->name, t.p, t.n); placed = 1; } }
        free(t.p); if (!placed) ok = 0;
    }
    cgx_inc_free(&a); cgx_inc_free(&b); return ok;
}

/* ---- gravacao ---- */
typedef int (*CgxIncWriter)(void *ctx, const char *path, const CgxIncActs *acts, const char *final_text, size_t fn, char *msg, size_t cap);
/* padrao: grava o arquivo inteiro de forma atomica (arquivo temporario na mesma pasta + rename), mantendo as permissoes */
static int cgx_inc_write_atomic(void *ctx, const char *path, const CgxIncActs *acts, const char *final_text, size_t fn, char *msg, size_t cap) {
#ifdef _WIN32
    (void)ctx; (void)acts; char tmp[1500]; snprintf(tmp, sizeof tmp, "%s.codegen.tmp", path);
#else
    (void)ctx; (void)acts; char tmp[1500]; snprintf(tmp, sizeof tmp, "%s.codegen.%ld.tmp", path, (long)getpid());
#endif
    FILE *f = fopen(tmp, "wb"); if (!f) { snprintf(msg, cap, "cannot write %s", tmp); return 0; }
    const int ok = fwrite(final_text, 1, fn, f) == fn; if (fclose(f) != 0 || !ok) { remove(tmp); snprintf(msg, cap, "write failed for %s", tmp); return 0; }
#ifdef _WIN32
    remove(path);
#else
    { struct stat st; if (stat(path, &st) == 0) chmod(tmp, st.st_mode & 07777); }
#endif
    if (rename(tmp, path) != 0) { remove(tmp); snprintf(msg, cap, "cannot replace %s", path); return 0; }
    return 1;
}
#ifdef CGX_INC_KATANA
/* No katana: cada simbolo alterado passa por ct_edit_symbol (validacao estrutural, chamadores, LSP, desfazer). O que mudou fora dos simbolos
 * (imports, texto solto) e conferido depois: se o arquivo nao ficou igual ao esperado, grava o texto final pelo caminho atomico. */
static void cgx_inc_json_str(const char *s, CgxB *o) {
    cgx_bs(o, "\"");
    for (; *s; s++) { const unsigned char ch = (unsigned char)*s; if (ch == '"' || ch == '\\') { cgx_bn(o, "\\", 1); cgx_bn(o, s, 1); } else if (ch == '\n') cgx_bs(o, "\\n"); else if (ch == '\r') cgx_bs(o, "\\r"); else if (ch == '\t') cgx_bs(o, "\\t"); else if (ch < 32) cgx_bf(o, "\\u%04x", ch); else cgx_bn(o, s, 1); }
    cgx_bs(o, "\"");
}
static int cgx_inc_write_symedit(void *ctx, const char *path, const CgxIncActs *acts, const char *final_text, size_t fn, char *msg, size_t cap) {
    (void)ctx; static char out[8192]; static const char *const W[] = {"replace", "after", "before", "delete"};
    for (int i = 0; i < acts->n; i++) {
        CgxB js = {0}; cgx_bs(&js, "{\"file_path\":"); cgx_inc_json_str(path, &js); cgx_bs(&js, ",\"symbol\":"); cgx_inc_json_str(acts->a[i].symbol, &js);
        cgx_bf(&js, ",\"where\":\"%s\"", W[acts->a[i].kind]);
        if (acts->a[i].kind != CIA_DELETE) { cgx_bs(&js, ",\"new_string\":"); cgx_inc_json_str(acts->a[i].text, &js); }
        cgx_bs(&js, "}");
        out[0] = 0; ct_edit_symbol(js.p, js.p + js.n, out, sizeof out); free(js.p);
        if (!strncmp(out, "error:", 6)) { snprintf(msg, cap, "%s (symbol %s)", out, acts->a[i].symbol); return 0; }
    }
    size_t cn = 0; char *cur = NULL; FILE *f = fopen(path, "rb");
    if (f) { fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET); cur = (char *)malloc((size_t)l + 1); cn = fread(cur, 1, (size_t)l, f); fclose(f); }
    const int same = cur && cn == fn && !memcmp(cur, final_text, fn); free(cur);
    if (same) return 1;
    return cgx_inc_write_atomic(ctx, path, acts, final_text, fn, msg, cap);
}
#endif
/* o ciclo todo: le o arquivo, aplica a especificacao, grava pelo escritor. dry != 0: so calcula. Devolve 1 se ok (inclusive "nada mudou"). */
static int cgx_inc_apply_file(const char *path, const char *spec, CgxIncWriter w, void *wctx, int dry, CgxIncResult *R, CgxIncActs *A, char *msg, size_t cap) {
    FILE *f = fopen(path, "rb"); if (!f) { snprintf(msg, cap, "cannot open %s", path); return 0; }
    fseek(f, 0, SEEK_END); const long l = ftell(f); fseek(f, 0, SEEK_SET); char *src = (char *)malloc((size_t)l + 1); const size_t n = fread(src, 1, (size_t)l, f); fclose(f); src[n] = 0;
    if (memchr(src, 0, n)) { free(src); snprintf(msg, cap, "%s is not a text file", path); return 0; }
    const int ok = cgx_inc_apply_spec(src, n, spec, R);
    if (!ok) { snprintf(msg, cap, "%s", R->err); free(src); return 0; }
    if (R->changed) {
        if (A) cgx_inc_diff(src, n, R->text, R->n, A);
        if (!dry) { CgxIncActs none = {0}; if (!w(wctx, path, A ? A : &none, R->text, R->n, msg, cap)) { free(src); return 0; } }
    }
    msg[0] = 0; free(src); return 1;
}
