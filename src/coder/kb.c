/* ---------- coder/kb.c: base de conhecimento de padroes (formato orientado a linha) e carregador ----------
 * Le data/coder/kb (.kb e .rules, recursivo) e .katana/coder/ do projeto (prioridade maior) para tabelas compactas.
 * Formato documentado em data/coder/kb/README.md. Sem biblioteca de terceiros: busca binaria em indices ordenados.
 * Texto sempre normalizado: minusculas ASCII, acentos latinos dobrados (a com til vira a), '-' e ' ' mantidos. */
#include <dirent.h>
#include <sys/stat.h>

#define KB_PT 0
#define KB_EN 1
#define KB_CODE 2
#define KB_NLANG 3
static const char *const g_kb_langname[KB_NLANG] = {"pt", "en", "code"};

typedef struct { char *term; int lang, con; float w; } KbTerm;
typedef struct { char *id; float w; } KbRel;
typedef struct { char *id, *category, *canonical, *source, *note; int t0, nt, a0, na, r0, nr, prio, dead, file, line; } KbConcept;
typedef struct { char *w; int lang, op; } KbVerb;
typedef struct { char *name, *category; int v0, nv, p0, np, n0, nn, prio, dead, file, line; } KbOp;
typedef struct { char *abbr; int e0, ne, prio, dead, file, line; } KbAbbr;
typedef struct { char *glob, *op, *role; int prio, file, line; } KbName;
typedef struct { int lang, minstem, order; char *suf, *rep; } KbSuffix;
typedef struct { int lang; char *form, *canon; } KbIrr;
typedef struct { char *name, *note; float w; int prio, file, line; } KbEdge;
typedef struct { const char *key; int ref; } KbIx;
typedef struct {
    char *path; int prio, concepts, terms, ops, verbs, abbrs, names, suffixes, irregulars, edges, warns, errs;
} KbFile;

typedef struct {
    KbConcept *con; int ncon, ccon;   KbTerm *term; int nterm, cterm;   char **act; int nact, cact;   KbRel *rel; int nrel, crel;
    KbOp *op; int nop, cop;           KbVerb *verb; int nverb, cverb;   char **pat; int npat, cpat;   char **ant; int nant, cant;
    KbAbbr *abbr; int nabbr, cabbr;   char **exp; int nexp, cexp;
    KbName *name; int nname, cname;   KbSuffix *suf; int nsuf, csuf;    KbIrr *irr; int nirr, cirr;   KbEdge *edge; int nedge, cedge;
    KbFile *file; int nfile, cfile;
    KbIx *ix_term, *ix_verb, *ix_abbr, *ix_irr, *ix_con, *ix_op;   int nix_term, nix_verb, nix_abbr, nix_irr, nix_con, nix_op;
    int nerr, nwarn, quiet;
    const char *project;                 /* pasta do projeto (.katana/coder); NULL = nenhuma. Definir antes de coder_kb_load. */
} CoderKb;

#define KB_PUSH(a, n, c, v) do { if ((n) >= (c)) { (c) = (c) ? (c) * 2 : 64; (a) = realloc((a), (size_t)(c) * sizeof *(a)); if (!(a)) { fprintf(stderr, "kb: sem memoria\n"); exit(1); } } (a)[(n)++] = (v); } while (0)

static char *kb_dup(const char *s) { size_t n = strlen(s); char *p = malloc(n + 1); if (!p) { fprintf(stderr, "kb: sem memoria\n"); exit(1); } memcpy(p, s, n + 1); return p; }

/* normaliza para out: minusculas, dobra acentos de Latin-1 (UTF-8 C3 xx) para ASCII; devolve tamanho */
static size_t coder_kb_norm(const char *s, char *out, size_t cap) {
    static const char *f = "aaaaaaaceeeeiiiidnooooo/ouuuuyts" /* C0..DF */ "aaaaaaaceeeeiiiidnooooo/ouuuuyty" /* E0..FF */;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 1 < cap; p++) {
        unsigned c = *p;
        if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) { c = (unsigned)f[p[1] - 0x80]; p++; if (c == '/') c = 'x'; }
        else if (c >= 'A' && c <= 'Z') c += 32;
        out[n++] = (char)c;
    }
    out[n] = 0; return n;
}
static char *kb_ndup(const char *s) { char b[512]; coder_kb_norm(s, b, sizeof b); return kb_dup(b); }

/* nome de codigo para snake_case minusculo: getUserName -> get_user_name, HTTPServer -> http_server */
static void coder_kb_snake(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; s[i] && n + 3 < cap; i++) {
        unsigned char c = (unsigned char)s[i], p = i ? (unsigned char)s[i - 1] : 0, q = (unsigned char)s[i + 1];
        if (c == '-' || c == ' ' || c == '.') c = '_';
        if (c >= 'A' && c <= 'Z') {
            if (i && n && out[n - 1] != '_' && ((p >= 'a' && p <= 'z') || (p >= '0' && p <= '9') || (p >= 'A' && p <= 'Z' && q >= 'a' && q <= 'z'))) out[n++] = '_';
            c += 32;
        }
        out[n++] = (char)c;
    }
    out[n] = 0;
}

/* glob simples: '*' qualquer sequencia, '?' um caractere */
static int kb_glob(const char *g, const char *s) {
    while (*g) {
        if (*g == '*') { while (*g == '*') g++; if (!*g) return 1; for (; *s; s++) if (kb_glob(g, s)) return 1; return 0; }
        if (!*s || (*g != '?' && *g != *s)) return 0;
        g++; s++;
    }
    return !*s;
}

/* ---------- analise ---------- */
typedef struct { CoderKb *kb; int file, prio; const char *path; int line; KbFile *f;
    int in; /* 0 fora, 1 concept, 2 op */ int cur, t0, a0, r0, v0, p0, n0, opline; } KbParse;

static void kb_msg(KbParse *p, int err, const char *fmt, ...) {
    va_list ap; char b[400]; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (err) { p->kb->nerr++; p->f->errs++; } else { p->kb->nwarn++; p->f->warns++; }
    if (!err && p->kb->quiet) return;
    fprintf(stderr, "%s:%d: %s: %s\n", p->path, p->line, err ? "erro" : "aviso", b);
}

static int kb_isid(const char *s) { if (!*s) return 0; for (; *s; s++) if (!((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0; return 1; }
static int kb_lang(const char *s) { for (int i = 0; i < KB_NLANG; i++) if (!strcmp(s, g_kb_langname[i])) return i; return -1; }
static int kb_weight(const char *s, float *w) { char *e; double d = strtod(s, &e); if (e == s || *e || d < 0 || d > 1) return 0; *w = (float)d; return 1; }

/* quebra a linha em palavras (modifica buf); devolve n; w[i] ponteiros */
static int kb_words(char *buf, char **w, int max) {
    int n = 0;
    for (char *p = buf; *p && n < max;) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;
        w[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
        if (*p) *p++ = 0;
    }
    return n;
}
/* junta words [a,b) em um termo (com espacos), normalizado */
static char *kb_join(char **w, int a, int b) {
    char buf[512]; size_t n = 0; buf[0] = 0;
    for (int i = a; i < b && n < sizeof buf - 2; i++) { if (i > a) buf[n++] = ' '; n += snprintf(buf + n, sizeof buf - n, "%s", w[i]); }
    return kb_ndup(buf);
}
/* divide por '|' ou ',' e normaliza; empurra cada item para o callback */
static int kb_split(const char *s, int sep, char **out, int max) {
    int n = 0; char b[1024]; snprintf(b, sizeof b, "%s", s);
    for (char *p = b; p && *p && n < max;) { char *q = strchr(p, sep); if (q) *q = 0; while (*p == ' ') p++; if (*p) out[n++] = kb_ndup(p); p = q ? q + 1 : NULL; }
    return n;
}

static void kb_close_block(KbParse *p) {
    CoderKb *kb = p->kb;
    if (p->in == 1) {
        KbConcept *c = &kb->con[p->cur];
        c->t0 = p->t0; c->nt = kb->nterm - p->t0; c->a0 = p->a0; c->na = kb->nact - p->a0; c->r0 = p->r0; c->nr = kb->nrel - p->r0;
        if (!c->category) kb_msg(p, 0, "conceito %s sem category", c->id);
        if (!c->canonical) kb_msg(p, 0, "conceito %s sem canonical", c->id);
        if (!c->nt) kb_msg(p, 0, "conceito %s sem nenhum term", c->id);
        p->f->concepts++;
    } else if (p->in == 2) {
        KbOp *o = &kb->op[p->cur];
        o->v0 = p->v0; o->nv = kb->nverb - p->v0; o->p0 = p->p0; o->np = kb->npat - p->p0; o->n0 = p->n0; o->nn = kb->nant - p->n0;
        if (!o->nv) kb_msg(p, 0, "op %s sem verbos", o->name);
        p->f->ops++;
    }
    p->in = 0;
}

static void kb_line(KbParse *p, char *buf) {
    CoderKb *kb = p->kb; char *w[48]; float wt;
    char *h = strchr(buf, '#'); if (h) *h = 0;
    int n = kb_words(buf, w, 48);
    if (!n) return;
    const char *k = w[0];
    if (!strcmp(k, "concept")) {
        if (p->in) { kb_msg(p, 0, "bloco anterior sem 'end'"); kb_close_block(p); }
        if (n != 2 || !kb_isid(w[1])) { kb_msg(p, 1, "concept espera um ID em maiusculas e _"); p->in = -1; return; }
        KbConcept c = {0}; c.id = kb_dup(w[1]); c.prio = p->prio; c.file = p->file; c.line = p->line;
        c.source = kb_dup(p->prio ? "domain" : "seed");
        p->cur = kb->ncon; p->t0 = kb->nterm; p->a0 = kb->nact; p->r0 = kb->nrel; KB_PUSH(kb->con, kb->ncon, kb->ccon, c); p->in = 1; return;
    }
    if (!strcmp(k, "op")) {
        if (p->in) { kb_msg(p, 0, "bloco anterior sem 'end'"); kb_close_block(p); }
        if (n != 2 || !kb_isid(w[1])) { kb_msg(p, 1, "op espera um ID em maiusculas e _"); p->in = -1; return; }
        KbOp o = {0}; o.name = kb_dup(w[1]); o.prio = p->prio; o.file = p->file; o.line = p->line;
        p->cur = kb->nop; p->v0 = kb->nverb; p->p0 = kb->npat; p->n0 = kb->nant; KB_PUSH(kb->op, kb->nop, kb->cop, o); p->in = 2; return;
    }
    if (!strcmp(k, "end")) { if (p->in > 0) kb_close_block(p); else if (p->in == 0) kb_msg(p, 1, "'end' sem bloco aberto"); p->in = 0; return; }
    if (p->in == -1) return;   /* dentro de um bloco invalido: ignora ate o end */

    if (p->in == 1) {
        KbConcept *c = &kb->con[p->cur];
        if (!strcmp(k, "category")) { if (n != 2) kb_msg(p, 1, "category espera 1 valor"); else c->category = kb_dup(w[1]); }
        else if (!strcmp(k, "canonical")) { if (n != 2) kb_msg(p, 1, "canonical espera 1 valor"); else c->canonical = kb_dup(w[1]); }
        else if (!strcmp(k, "term")) {
            int lg = n >= 4 ? kb_lang(w[1]) : -1;
            if (n < 4 || lg < 0 || !kb_weight(w[n - 1], &wt)) { kb_msg(p, 1, "term espera: term <pt|en|code> <termo> <peso 0..1>"); return; }
            KbTerm t = {kb_join(w, 2, n - 1), lg, p->cur, wt}; KB_PUSH(kb->term, kb->nterm, kb->cterm, t); p->f->terms++;
        }
        else if (!strcmp(k, "action")) { if (n != 2) kb_msg(p, 1, "action espera 1 verbo"); else { char *a = kb_ndup(w[1]); KB_PUSH(kb->act, kb->nact, kb->cact, a); } }
        else if (!strcmp(k, "related")) {
            if (n != 3 || !kb_isid(w[1]) || !kb_weight(w[2], &wt)) { kb_msg(p, 1, "related espera: related <ID> <peso 0..1>"); return; }
            KbRel r = {kb_dup(w[1]), wt}; KB_PUSH(kb->rel, kb->nrel, kb->crel, r);
        }
        else if (!strcmp(k, "source")) {
            if (n != 2 || (strcmp(w[1], "seed") && strcmp(w[1], "mined") && strcmp(w[1], "domain") && strcmp(w[1], "user"))) kb_msg(p, 1, "source espera seed|mined|domain|user");
            else { free(c->source); c->source = kb_dup(w[1]); }
        }
        else if (!strcmp(k, "note")) { char *t = kb_join(w, 1, n); free(c->note); c->note = t; }
        else kb_msg(p, 0, "palavra-chave desconhecida em concept: '%s'", k);
        return;
    }
    if (p->in == 2) {
        KbOp *o = &kb->op[p->cur];
        if (!strcmp(k, "verb")) {
            int lg = n >= 3 ? kb_lang(w[1]) : -1;
            if (lg < 0 || lg == KB_CODE + 5) { kb_msg(p, 1, "verb espera: verb <pt|en|code> a|b|c"); return; }
            char rest[1024]; rest[0] = 0; for (int i = 2; i < n; i++) { if (i > 2) strcat(rest, " "); strncat(rest, w[i], sizeof rest - strlen(rest) - 2); }
            char *it[64]; int m = kb_split(rest, '|', it, 64);
            for (int i = 0; i < m; i++) { KbVerb v = {it[i], lg, p->cur}; KB_PUSH(kb->verb, kb->nverb, kb->cverb, v); p->f->verbs++; }
        }
        else if (!strcmp(k, "pattern")) { if (n != 3 || kb_lang(w[1]) < 0) kb_msg(p, 1, "pattern espera: pattern <lang> <glob>"); else { char *g = kb_dup(w[2]); KB_PUSH(kb->pat, kb->npat, kb->cpat, g); } }
        else if (!strcmp(k, "antonym")) { if (n != 2 || !kb_isid(w[1])) kb_msg(p, 1, "antonym espera um ID de operacao"); else { char *g = kb_dup(w[1]); KB_PUSH(kb->ant, kb->nant, kb->cant, g); } }
        else if (!strcmp(k, "category")) { if (n != 2) kb_msg(p, 1, "category espera 1 valor"); else o->category = kb_dup(w[1]); }
        else if (!strcmp(k, "note") || !strcmp(k, "source")) { }
        else kb_msg(p, 0, "palavra-chave desconhecida em op: '%s'", k);
        return;
    }
    /* nivel de arquivo */
    if (!strcmp(k, "abbr")) {
        if (n < 3) { kb_msg(p, 1, "abbr espera: abbr <abreviacao> <exp>[,<exp2>]"); return; }
        char rest[512]; rest[0] = 0; for (int i = 2; i < n; i++) { if (i > 2) strcat(rest, " "); strncat(rest, w[i], sizeof rest - strlen(rest) - 2); }
        char *it[16]; int m = kb_split(rest, ',', it, 16);
        if (!m) { kb_msg(p, 1, "abbr sem expansao"); return; }
        KbAbbr a = {kb_ndup(w[1]), kb->nexp, m, p->prio, 0, p->file, p->line}; for (int i = 0; i < m; i++) KB_PUSH(kb->exp, kb->nexp, kb->cexp, it[i]);
        KB_PUSH(kb->abbr, kb->nabbr, kb->cabbr, a); p->f->abbrs++;
    }
    else if (!strcmp(k, "pattern")) {
        if (n != 4 || !kb_isid(w[2])) { kb_msg(p, 1, "pattern espera: pattern <glob> <OPERACAO> <papel>"); return; }
        KbName a = {kb_ndup(w[1]), kb_dup(w[2]), kb_ndup(w[3]), p->prio, p->file, p->line}; KB_PUSH(kb->name, kb->nname, kb->cname, a); p->f->names++;
    }
    else if (!strcmp(k, "edge")) {
        if (n < 3 || !kb_weight(w[2], &wt)) { kb_msg(p, 1, "edge espera: edge <tipo> <peso 0..1> [nota]"); return; }
        KbEdge e = {kb_dup(w[1]), n > 3 ? kb_join(w, 3, n) : kb_dup(""), wt, p->prio, p->file, p->line}; KB_PUSH(kb->edge, kb->nedge, kb->cedge, e); p->f->edges++;
    }
    else if (!strcmp(k, "suffix") || !strcmp(k, "irregular")) {
        const char *b = strrchr(p->path, '/'); b = b ? b + 1 : p->path;
        int lg = -1; { const char *d = strchr(b, '.'); if (d && !strncmp(b, "morphology.", 11)) { char t[8]; const char *e = strchr(d + 1, '.'); size_t l = e ? (size_t)(e - d - 1) : 0; if (l && l < sizeof t) { memcpy(t, d + 1, l); t[l] = 0; lg = kb_lang(t); } } }
        if (lg < 0) { kb_msg(p, 1, "regra morfologica fora de morphology.<lang>.rules"); return; }
        if (k[0] == 's') {
            if (n != 5) { kb_msg(p, 1, "suffix espera: suffix <sufixo> <subst|-> <raiz minima> <ordem>"); return; }
            char *e1, *e2; long ms = strtol(w[3], &e1, 10), od = strtol(w[4], &e2, 10);
            if (*e1 || *e2 || ms < 1 || ms > 20) { kb_msg(p, 1, "suffix: raiz minima (1..20) ou ordem invalida"); return; }
            KbSuffix s = {lg, (int)ms, (int)od, kb_ndup(w[1]), kb_ndup(strcmp(w[2], "-") ? w[2] : "")}; KB_PUSH(kb->suf, kb->nsuf, kb->csuf, s); p->f->suffixes++;
        } else {
            if (n != 3) { kb_msg(p, 1, "irregular espera: irregular <forma> <canonica>"); return; }
            KbIrr r = {lg, kb_ndup(w[1]), kb_ndup(w[2])}; KB_PUSH(kb->irr, kb->nirr, kb->cirr, r); p->f->irregulars++;
        }
    }
    else kb_msg(p, 0, "palavra-chave desconhecida: '%s'", k);
}

static int kb_load_file(CoderKb *kb, const char *path, int prio) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    KbFile kf = {0}; kf.path = kb_dup(path); kf.prio = prio; KB_PUSH(kb->file, kb->nfile, kb->cfile, kf);
    KbParse p = {0}; p.kb = kb; p.file = kb->nfile - 1; p.prio = prio; p.path = kb->file[p.file].path; p.f = &kb->file[p.file];
    char *buf = NULL; size_t cap = 0; int c; size_t n = 0;
    for (;;) {
        c = fgetc(f);
        if (c == EOF && n == 0) break;
        if (c == '\n' || c == EOF) {
            if (n + 1 > cap) { cap = cap ? cap * 2 : 256; buf = realloc(buf, cap); }
            buf[n] = 0; p.line++; kb_line(&p, buf); n = 0; if (c == EOF) break; continue;
        }
        if (n + 2 > cap) { cap = cap ? cap * 2 : 256; buf = realloc(buf, cap); }
        buf[n++] = (char)c;
    }
    if (p.in) { kb_msg(&p, 1, "bloco sem 'end' no fim do arquivo"); kb_close_block(&p); }
    free(buf); fclose(f); return 0;
}

static int kb_cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void kb_collect(const char *dir, char ***v, int *n, int *c, int depth) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e; char **loc = NULL; int ln = 0, lc = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char *full = malloc(strlen(dir) + strlen(e->d_name) + 2); sprintf(full, "%s/%s", dir, e->d_name);
        KB_PUSH(loc, ln, lc, full);
    }
    closedir(d);
    if (ln > 1) qsort(loc, (size_t)ln, sizeof *loc, kb_cmp_str);
    for (int i = 0; i < ln; i++) {
        struct stat st; size_t l = strlen(loc[i]);
        if (stat(loc[i], &st) == 0 && S_ISDIR(st.st_mode)) { if (depth < 3) kb_collect(loc[i], v, n, c, depth + 1); free(loc[i]); }
        else if ((l > 3 && !strcmp(loc[i] + l - 3, ".kb")) || (l > 6 && !strcmp(loc[i] + l - 6, ".rules"))) KB_PUSH(*v, *n, *c, loc[i]);
        else free(loc[i]);
    }
    free(loc);
}

static int kb_cmp_ix(const void *a, const void *b) {
    const KbIx *x = a, *y = b; int r = strcmp(x->key, y->key); return r ? r : (x->ref - y->ref);
}
static int kb_cmp_suf(const void *a, const void *b) {
    const KbSuffix *x = a, *y = b; if (x->lang != y->lang) return x->lang - y->lang;
    if (x->order != y->order) return x->order - y->order;
    return (int)strlen(y->suf) - (int)strlen(x->suf);
}
static void kb_ixadd(KbIx **v, int *n, const char *key, int ref) { KbIx x = {key, ref}; int c = *n; (void)c; *v = realloc(*v, (size_t)(*n + 1) * sizeof **v); (*v)[(*n)++] = x; }

/* concepts/ops/abbr de prioridade menor com o mesmo nome sao substituidos pelos de prioridade maior (o ultimo de mesma prioridade vence com aviso) */
static void kb_resolve(CoderKb *kb) {
    for (int i = 0; i < kb->ncon; i++) kb_ixadd(&kb->ix_con, &kb->nix_con, kb->con[i].id, i);
    qsort(kb->ix_con, (size_t)kb->nix_con, sizeof(KbIx), kb_cmp_ix);
    for (int i = 0; i + 1 < kb->nix_con; i++) if (!strcmp(kb->ix_con[i].key, kb->ix_con[i + 1].key)) {
        KbConcept *a = &kb->con[kb->ix_con[i].ref], *b = &kb->con[kb->ix_con[i + 1].ref];
        KbConcept *lo = a->prio <= b->prio ? a : b; lo->dead = 1;
        if (a->prio == b->prio) { kb->nwarn++; fprintf(stderr, "%s:%d: aviso: conceito %s repetido (tambem em %s:%d; vale o ultimo)\n", kb->file[a->file].path, a->line, a->id, kb->file[b->file].path, b->line); }
    }
    for (int i = 0; i < kb->nop; i++) kb_ixadd(&kb->ix_op, &kb->nix_op, kb->op[i].name, i);
    qsort(kb->ix_op, (size_t)kb->nix_op, sizeof(KbIx), kb_cmp_ix);
    for (int i = 0; i + 1 < kb->nix_op; i++) if (!strcmp(kb->ix_op[i].key, kb->ix_op[i + 1].key)) {
        KbOp *a = &kb->op[kb->ix_op[i].ref], *b = &kb->op[kb->ix_op[i + 1].ref]; (a->prio <= b->prio ? a : b)->dead = 1;
        if (a->prio == b->prio) { kb->nwarn++; fprintf(stderr, "%s:%d: aviso: op %s repetida (tambem em %s:%d; vale a ultima)\n", kb->file[a->file].path, a->line, a->name, kb->file[b->file].path, b->line); }
    }
    KbIx *tmp = NULL; int nt = 0;
    for (int i = 0; i < kb->nabbr; i++) kb_ixadd(&tmp, &nt, kb->abbr[i].abbr, i);
    qsort(tmp, (size_t)nt, sizeof(KbIx), kb_cmp_ix);
    for (int i = 0; i + 1 < nt; i++) if (!strcmp(tmp[i].key, tmp[i + 1].key)) {
        KbAbbr *a = &kb->abbr[tmp[i].ref], *b = &kb->abbr[tmp[i + 1].ref];
        if (a->prio != b->prio) (a->prio < b->prio ? a : b)->dead = 1;   /* mesma prioridade: as duas valem (expansoes somadas) */
    }
    free(tmp);
    /* indices de consulta */
    for (int i = 0; i < kb->nterm; i++) if (!kb->con[kb->term[i].con].dead) kb_ixadd(&kb->ix_term, &kb->nix_term, kb->term[i].term, i);
    for (int i = 0; i < kb->nverb; i++) if (!kb->op[kb->verb[i].op].dead) kb_ixadd(&kb->ix_verb, &kb->nix_verb, kb->verb[i].w, i);
    for (int i = 0; i < kb->nabbr; i++) if (!kb->abbr[i].dead) kb_ixadd(&kb->ix_abbr, &kb->nix_abbr, kb->abbr[i].abbr, i);
    for (int i = 0; i < kb->nirr; i++) kb_ixadd(&kb->ix_irr, &kb->nix_irr, kb->irr[i].form, i);
    qsort(kb->ix_term, (size_t)kb->nix_term, sizeof(KbIx), kb_cmp_ix); qsort(kb->ix_verb, (size_t)kb->nix_verb, sizeof(KbIx), kb_cmp_ix);
    qsort(kb->ix_abbr, (size_t)kb->nix_abbr, sizeof(KbIx), kb_cmp_ix); qsort(kb->ix_irr, (size_t)kb->nix_irr, sizeof(KbIx), kb_cmp_ix);
    if (kb->nsuf > 1) qsort(kb->suf, (size_t)kb->nsuf, sizeof *kb->suf, kb_cmp_suf);
}

/* le todos os .kb e .rules de dir (e de kb->project, com prioridade maior); 0 = ok (pode haver avisos), -1 = erros de formato ou nada lido */
static int coder_kb_load(const char *dir, CoderKb *kb) {
    const char *proj = kb->project; int quiet = kb->quiet; memset(kb, 0, sizeof *kb); kb->project = proj; kb->quiet = quiet;
    for (int pass = 0; pass < 2; pass++) {
        const char *d = pass ? proj : dir; if (!d || !*d) continue;
        char **v = NULL; int n = 0, c = 0; kb_collect(d, &v, &n, &c, 0);
        for (int i = 0; i < n; i++) { kb_load_file(kb, v[i], pass); free(v[i]); }
        free(v);
    }
    if (!kb->nfile) { if (!kb->quiet) fprintf(stderr, "kb: nenhum arquivo .kb/.rules em %s\n", dir ? dir : "(nulo)"); return -1; }
    kb_resolve(kb);
    return kb->nerr ? -1 : 0;
}

/* libera tudo o que coder_kb_load alocou (a pasta `project` e do chamador); deixa kb zerado, com project e quiet como estavam */
static void coder_kb_free(CoderKb *kb) {
    for (int i = 0; i < kb->ncon; i++) { KbConcept *c = &kb->con[i]; free(c->id); free(c->category); free(c->canonical); free(c->source); free(c->note); }
    for (int i = 0; i < kb->nterm; i++) free(kb->term[i].term);
    for (int i = 0; i < kb->nact; i++) free(kb->act[i]);
    for (int i = 0; i < kb->nrel; i++) free(kb->rel[i].id);
    for (int i = 0; i < kb->nop; i++) { free(kb->op[i].name); free(kb->op[i].category); }
    for (int i = 0; i < kb->nverb; i++) free(kb->verb[i].w);
    for (int i = 0; i < kb->npat; i++) free(kb->pat[i]);
    for (int i = 0; i < kb->nant; i++) free(kb->ant[i]);
    for (int i = 0; i < kb->nabbr; i++) free(kb->abbr[i].abbr);
    for (int i = 0; i < kb->nexp; i++) free(kb->exp[i]);
    for (int i = 0; i < kb->nname; i++) { free(kb->name[i].glob); free(kb->name[i].op); free(kb->name[i].role); }
    for (int i = 0; i < kb->nsuf; i++) { free(kb->suf[i].suf); free(kb->suf[i].rep); }
    for (int i = 0; i < kb->nirr; i++) { free(kb->irr[i].form); free(kb->irr[i].canon); }
    for (int i = 0; i < kb->nedge; i++) { free(kb->edge[i].name); free(kb->edge[i].note); }
    for (int i = 0; i < kb->nfile; i++) free(kb->file[i].path);
    free(kb->con); free(kb->term); free(kb->act); free(kb->rel); free(kb->op); free(kb->verb); free(kb->pat); free(kb->ant); free(kb->abbr); free(kb->exp);
    free(kb->name); free(kb->suf); free(kb->irr); free(kb->edge); free(kb->file);
    free(kb->ix_term); free(kb->ix_verb); free(kb->ix_abbr); free(kb->ix_irr); free(kb->ix_con); free(kb->ix_op);
    const char *proj = kb->project; const int quiet = kb->quiet; memset(kb, 0, sizeof *kb); kb->project = proj; kb->quiet = quiet;
}

/* ---------- consultas ---------- */
static int kb_lower(const KbIx *v, int n, const char *key) { int lo = 0, hi = n; while (lo < hi) { int m = (lo + hi) / 2; if (strcmp(v[m].key, key) < 0) lo = m + 1; else hi = m; } return lo; }

/* termo -> conceitos: preenche out com indices de KbTerm; devolve quantos (ordenados por peso desc) */
static int coder_kb_term_concepts(const CoderKb *kb, const char *term, int *out, int max) {
    char k[512]; coder_kb_norm(term, k, sizeof k); int n = 0;
    for (int i = kb_lower(kb->ix_term, kb->nix_term, k); i < kb->nix_term && !strcmp(kb->ix_term[i].key, k); i++) if (n < max) out[n++] = kb->ix_term[i].ref;
    for (int i = 1; i < n; i++) { int x = out[i], j = i; while (j > 0 && kb->term[out[j - 1]].w < kb->term[x].w) { out[j] = out[j - 1]; j--; } out[j] = x; }
    return n;
}
/* verbo -> operacoes (indices de KbVerb) */
static int coder_kb_verb_ops(const CoderKb *kb, const char *verb, int *out, int max) {
    char k[512]; coder_kb_norm(verb, k, sizeof k); int n = 0;
    for (int i = kb_lower(kb->ix_verb, kb->nix_verb, k); i < kb->nix_verb && !strcmp(kb->ix_verb[i].key, k); i++) if (n < max) out[n++] = kb->ix_verb[i].ref;
    return n;
}
/* abreviacao -> entradas (indices de KbAbbr) */
static int coder_kb_abbr(const CoderKb *kb, const char *abbr, int *out, int max) {
    char k[512]; coder_kb_norm(abbr, k, sizeof k); int n = 0;
    for (int i = kb_lower(kb->ix_abbr, kb->nix_abbr, k); i < kb->nix_abbr && !strcmp(kb->ix_abbr[i].key, k); i++) if (n < max) out[n++] = kb->ix_abbr[i].ref;
    return n;
}
/* expansao -> abreviacoes que a produzem (busca linear; poucos milhares de entradas) */
static int coder_kb_abbr_rev(const CoderKb *kb, const char *word, int *out, int max) {
    char k[512]; coder_kb_norm(word, k, sizeof k); int n = 0;
    for (int i = 0; i < kb->nabbr && n < max; i++) { if (kb->abbr[i].dead) continue;
        for (int j = 0; j < kb->abbr[i].ne; j++) if (!strcmp(kb->exp[kb->abbr[i].e0 + j], k)) { out[n++] = i; break; } }
    return n;
}
/* nome de codigo -> padroes de nome (naming.kb e pattern de operacoes) que casam; out[i] = indice em kb->name, ou -(1+indice de op) para pattern de op */
static int coder_kb_name_match(const CoderKb *kb, const char *ident, int *out, int max) {
    char s[256]; coder_kb_snake(ident, s, sizeof s); int n = 0;
    for (int i = 0; i < kb->nname && n < max; i++) if (kb_glob(kb->name[i].glob, s)) out[n++] = i;
    for (int o = 0; o < kb->nop && n < max; o++) { if (kb->op[o].dead) continue;
        for (int j = 0; j < kb->op[o].np; j++) if (kb_glob(kb->pat[kb->op[o].p0 + j], s)) { out[n++] = -(1 + o); break; } }
    return n;
}
static int coder_kb_op_find(const CoderKb *kb, const char *name) {
    int i = kb_lower(kb->ix_op, kb->nix_op, name);
    for (; i < kb->nix_op && !strcmp(kb->ix_op[i].key, name); i++) if (!kb->op[kb->ix_op[i].ref].dead) return kb->ix_op[i].ref;
    return -1;
}
static int coder_kb_concept_find(const CoderKb *kb, const char *id) {
    int i = kb_lower(kb->ix_con, kb->nix_con, id);
    for (; i < kb->nix_con && !strcmp(kb->ix_con[i].key, id); i++) if (!kb->con[kb->ix_con[i].ref].dead) return kb->ix_con[i].ref;
    return -1;
}
/* candidatos de raiz/canonica para a palavra (irregular, senao a primeira regra de sufixo que casa); devolve quantos (0 = sem mudanca) */
static int coder_kb_stems(const CoderKb *kb, int lang, const char *word, char cand[][64], int max) {
    char w[128]; size_t wl = coder_kb_norm(word, w, sizeof w); int n = 0;
    for (int i = kb_lower(kb->ix_irr, kb->nix_irr, w); i < kb->nix_irr && !strcmp(kb->ix_irr[i].key, w); i++) {
        const KbIrr *r = &kb->irr[kb->ix_irr[i].ref]; if (r->lang == lang && n < max) snprintf(cand[n++], 64, "%s", r->canon);
    }
    if (n) return n;
    for (int i = 0; i < kb->nsuf; i++) {
        const KbSuffix *s = &kb->suf[i]; size_t sl = strlen(s->suf);
        if (s->lang != lang || sl >= wl || (int)(wl - sl) < s->minstem || strcmp(w + wl - sl, s->suf)) continue;
        char stem[128]; memcpy(stem, w, wl - sl); stem[wl - sl] = 0;
        char rep[128]; snprintf(rep, sizeof rep, "%s", s->rep); char *p = rep;
        while (p && n < max) { char *q = strchr(p, ','); if (q) *q = 0; snprintf(cand[n++], 64, "%.40s%.20s", stem, p); p = q ? q + 1 : NULL; }
        return n;
    }
    return 0;
}

/* ---------- caminho padrao e subcomando `katana coder kb` ---------- */
static int kb_isdir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
/* KATANA_KB_DIR, senao ../data/coder/kb ou ./data/coder/kb relativo ao executavel, senao o relativo a pasta de trabalho */
static const char *coder_kb_default_dir(void) {
    static char buf[4200]; const char *e = getenv("KATANA_KB_DIR");
    if (e && *e) { snprintf(buf, sizeof buf, "%s", e); return buf; }
    char exe[4096]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0; char *sl = strrchr(exe, '/');
        if (sl) { *sl = 0;
            static const char *const rel[] = {"../data/coder/kb", "data/coder/kb", "../../data/coder/kb"};
            for (int i = 0; i < 3; i++) { snprintf(buf, sizeof buf, "%s/%s", exe, rel[i]); if (kb_isdir(buf)) return buf; } }
    }
    snprintf(buf, sizeof buf, "data/coder/kb"); return buf;
}

static void kb_showterms(const CoderKb *kb, const KbConcept *c, int lang_not, int lang_only, int max) {
    int k = 0;
    for (int i = 0; i < c->nt && k < max; i++) { const KbTerm *t = &kb->term[c->t0 + i];
        if ((lang_not >= 0 && t->lang == lang_not) || (lang_only >= 0 && t->lang != lang_only)) continue;
        printf("%s%s:%s(%.2f)", k ? " " : "", g_kb_langname[t->lang], t->term, t->w); k++; }
}

/* mostra o que se sabe da palavra/frase curta key; mode 0 = lookup, 1 = expand (ponte entre idiomas); devolve 1 se achou algo */
static int kb_show(const CoderKb *kb, const char *key, int mode) {
    int hit = 0, ids[64]; char norm[256]; coder_kb_norm(key, norm, sizeof norm);
    int n = coder_kb_term_concepts(kb, norm, ids, 64);
    for (int i = 0; i < n; i++) { const KbTerm *t = &kb->term[ids[i]]; const KbConcept *c = &kb->con[t->con]; hit = 1;
        printf("  concept %-28s %s  (%s %.2f) [%s]\n", c->id, c->category ? c->category : "-", g_kb_langname[t->lang], t->w, c->source);
        if (mode) {
            printf("      other languages: "); kb_showterms(kb, c, t->lang, -1, 8); printf("\n");
            if (c->na) { printf("      actions: "); for (int j = 0; j < c->na; j++) printf("%s%s", j ? ", " : "", kb->act[c->a0 + j]); printf("\n"); }
            if (c->nr) { printf("      related: "); for (int j = 0; j < c->nr && j < 6; j++) printf("%s%s %.2f", j ? ", " : "", kb->rel[c->r0 + j].id, kb->rel[c->r0 + j].w); printf("\n"); }
        }
    }
    n = coder_kb_verb_ops(kb, norm, ids, 64);
    for (int i = 0; i < n; i++) { const KbVerb *v = &kb->verb[ids[i]]; const KbOp *o = &kb->op[v->op]; hit = 1;
        printf("  op      %-28s %s (verb %s)", o->name, o->category ? o->category : "-", g_kb_langname[v->lang]);
        if (o->nn) { printf("  antonym:"); for (int j = 0; j < o->nn; j++) printf(" %s", kb->ant[o->n0 + j]); }
        printf("\n");
        if (mode) {
            int other = v->lang == KB_PT ? KB_EN : KB_PT, k = 0;
            printf("      %s verbs: ", g_kb_langname[other]);
            for (int j = 0; j < o->nv && k < 10; j++) if (kb->verb[o->v0 + j].lang == other) { printf("%s%s", k ? ", " : "", kb->verb[o->v0 + j].w); k++; }
            printf("\n      code names: ");
            const char *seen[8]; k = 0;
            for (int j = 0; j < kb->nname + o->np && k < 8; j++) {
                const char *g = j < kb->nname ? (strcmp(kb->name[j].op, o->name) ? NULL : kb->name[j].glob) : kb->pat[o->p0 + j - kb->nname];
                int dup = 0; for (int s = 0; g && s < k; s++) if (!strcmp(seen[s], g)) dup = 1;
                if (g && !dup) { seen[k] = g; printf("%s%s", k ? ", " : "", g); k++; }
            }
            printf("\n");
        }
    }
    n = coder_kb_abbr(kb, norm, ids, 64);
    for (int i = 0; i < n; i++) { const KbAbbr *a = &kb->abbr[ids[i]]; hit = 1; printf("  abbr    %s ->", a->abbr); for (int j = 0; j < a->ne; j++) printf("%s%s", j ? "," : " ", kb->exp[a->e0 + j]); printf("\n"); }
    n = coder_kb_abbr_rev(kb, norm, ids, 12);
    if (n) { hit = 1; printf("  abbr    '%s' is abbreviated in code as:", norm); for (int i = 0; i < n; i++) printf(" %s", kb->abbr[ids[i]].abbr); printf("\n"); }
    int nm[32]; n = coder_kb_name_match(kb, key, nm, 32);
    for (int i = 0; i < n; i++) { hit = 1;
        if (nm[i] >= 0) printf("  name    %s -> %s as %s\n", kb->name[nm[i]].glob, kb->name[nm[i]].op, kb->name[nm[i]].role);
        else printf("  name    pattern of op %s\n", kb->op[-(nm[i] + 1)].name); }
    return hit;
}

/* tenta key; se nada, tenta as raizes candidatas (pt, depois en) */
static int kb_show_word(const CoderKb *kb, const char *w, int mode) {
    if (kb_show(kb, w, mode)) return 1;
    for (int lg = KB_PT; lg <= KB_EN; lg++) {
        char cand[6][64]; int n = coder_kb_stems(kb, lg, w, cand, 6);
        for (int i = 0; i < n; i++) { int ids[4];
            if (coder_kb_term_concepts(kb, cand[i], ids, 4) || coder_kb_verb_ops(kb, cand[i], ids, 4) || coder_kb_abbr(kb, cand[i], ids, 4)) {
                printf("  (via %s stem '%s')\n", g_kb_langname[lg], cand[i]); return kb_show(kb, cand[i], mode); } }
    }
    return 0;
}

static int kb_cmp_cat(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void kb_print_counts(const CoderKb *kb, int detail) {
    printf("files: %d\n", kb->nfile);
    if (detail) for (int i = 0; i < kb->nfile; i++) { const KbFile *f = &kb->file[i];
        printf("  %-52s concepts %3d terms %5d ops %3d verbs %5d abbr %4d names %3d suffix %3d irregular %4d edges %2d  warn %d err %d%s\n",
               f->path, f->concepts, f->terms, f->ops, f->verbs, f->abbrs, f->names, f->suffixes, f->irregulars, f->edges, f->warns, f->errs, f->prio ? "  [project]" : ""); }
    int live = 0; for (int i = 0; i < kb->ncon; i++) if (!kb->con[i].dead) live++;
    int lo = 0; for (int i = 0; i < kb->nop; i++) if (!kb->op[i].dead) lo++;
    int la = 0; for (int i = 0; i < kb->nabbr; i++) if (!kb->abbr[i].dead) la++;
    int tl[KB_NLANG] = {0}; for (int i = 0; i < kb->nterm; i++) if (!kb->con[kb->term[i].con].dead) tl[kb->term[i].lang]++;
    int vl[KB_NLANG] = {0}; for (int i = 0; i < kb->nverb; i++) if (!kb->op[kb->verb[i].op].dead) vl[kb->verb[i].lang]++;
    int sl[KB_NLANG] = {0}, il[KB_NLANG] = {0}; for (int i = 0; i < kb->nsuf; i++) sl[kb->suf[i].lang]++; for (int i = 0; i < kb->nirr; i++) il[kb->irr[i].lang]++;
    printf("total: concepts %d, terms %d (pt %d en %d code %d), operations %d, verbs %d (pt %d en %d), abbreviations %d (%d expansions), name patterns %d, edge types %d\n",
           live, tl[0] + tl[1] + tl[2], tl[0], tl[1], tl[2], lo, vl[0] + vl[1] + vl[2], vl[0], vl[1], la, kb->nexp, kb->nname, kb->nedge);
    printf("morphology: pt %d suffix rules + %d irregulars; en %d suffix rules + %d irregulars\n", sl[KB_PT], il[KB_PT], sl[KB_EN], il[KB_EN]);
    char **cats = NULL; int nc = 0, cc = 0;
    for (int i = 0; i < kb->ncon; i++) if (!kb->con[i].dead && kb->con[i].category) KB_PUSH(cats, nc, cc, kb->con[i].category);
    for (int i = 0; i < kb->nop; i++) if (!kb->op[i].dead && kb->op[i].category) KB_PUSH(cats, nc, cc, kb->op[i].category);
    if (nc) { qsort(cats, (size_t)nc, sizeof *cats, kb_cmp_cat); printf("by category (concepts + operations):\n");
        for (int i = 0; i < nc;) { int j = i; while (j < nc && !strcmp(cats[j], cats[i])) j++; printf("  %-28s %d\n", cats[i], j - i); i = j; } }
    free(cats);
}

static int kb_cmd_check(CoderKb *kb) {
    int problems = 0;
    kb_print_counts(kb, 1);
    for (int i = 0; i < kb->ncon; i++) { const KbConcept *c = &kb->con[i]; if (c->dead) continue;
        for (int a = 0; a < c->nt; a++) for (int b = a + 1; b < c->nt; b++)
            if (kb->term[c->t0 + a].lang == kb->term[c->t0 + b].lang && !strcmp(kb->term[c->t0 + a].term, kb->term[c->t0 + b].term)) {
                printf("duplicate term in %s: %s %s (%s:%d)\n", c->id, g_kb_langname[kb->term[c->t0 + a].lang], kb->term[c->t0 + a].term, kb->file[c->file].path, c->line); problems++; }
        for (int r = 0; r < c->nr; r++) if (coder_kb_concept_find(kb, kb->rel[c->r0 + r].id) < 0) {
            printf("missing related concept: %s -> %s (%s:%d)\n", c->id, kb->rel[c->r0 + r].id, kb->file[c->file].path, c->line); problems++; } }
    for (int i = 0; i < kb->nop; i++) { const KbOp *o = &kb->op[i]; if (o->dead) continue;
        for (int j = 0; j < o->nn; j++) if (coder_kb_op_find(kb, kb->ant[o->n0 + j]) < 0) { printf("missing antonym op: %s -> %s (%s:%d)\n", o->name, kb->ant[o->n0 + j], kb->file[o->file].path, o->line); problems++; }
        for (int a = 0; a < o->nv; a++) for (int b = a + 1; b < o->nv; b++) if (kb->verb[o->v0 + a].lang == kb->verb[o->v0 + b].lang && !strcmp(kb->verb[o->v0 + a].w, kb->verb[o->v0 + b].w)) {
            printf("duplicate verb in op %s: %s\n", o->name, kb->verb[o->v0 + a].w); problems++; } }
    for (int i = 0; i < kb->nname; i++) if (coder_kb_op_find(kb, kb->name[i].op) < 0) { printf("%s:%d: naming pattern %s refers to unknown op %s\n", kb->file[kb->name[i].file].path, kb->name[i].line, kb->name[i].glob, kb->name[i].op); problems++; }
    /* verbo em operacoes diferentes (ambiguidade entre operacoes) e termo em muitos conceitos */
    int amb = 0;
    for (int i = 0; i < kb->nix_term;) { int j = i; while (j < kb->nix_term && !strcmp(kb->ix_term[j].key, kb->ix_term[i].key)) j++;
        int distinct = 0; for (int a = i; a < j; a++) { int dup = 0; for (int b = i; b < a; b++) if (kb->term[kb->ix_term[a].ref].con == kb->term[kb->ix_term[b].ref].con) dup = 1; if (!dup) distinct++; }
        if (distinct > 4) { if (amb++ < 20) printf("ambiguous term '%s': %d concepts\n", kb->ix_term[i].key, distinct); }
        i = j; }
    if (amb) printf("ambiguous terms (more than 4 concepts): %d\n", amb);
    int vamb = 0;
    for (int i = 0; i < kb->nix_verb;) { int j = i; while (j < kb->nix_verb && !strcmp(kb->ix_verb[j].key, kb->ix_verb[i].key)) j++;
        int distinct = 0; for (int a = i; a < j; a++) { int dup = 0; for (int b = i; b < a; b++) if (kb->verb[kb->ix_verb[a].ref].op == kb->verb[kb->ix_verb[b].ref].op) dup = 1; if (!dup) distinct++; }
        if (distinct > 2) { vamb++; if (vamb <= 15) printf("verb '%s' belongs to %d operations (ambiguous)\n", kb->ix_verb[i].key, distinct); }
        i = j; }
    if (vamb) printf("verbs in more than 2 operations: %d\n", vamb);
    printf("result: %d error(s), %d warning(s), %d consistency problem(s)\n", kb->nerr, kb->nwarn, problems);
    return (kb->nerr || problems) ? 1 : 0;
}

static int coder_kb_main(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "uso: katana coder kb check | lookup <termo> | expand \"frase\" | stats | edges\n"); return 2; }
    CoderKb kb; int quiet = strcmp(argv[0], "check") != 0;
    memset(&kb, 0, sizeof kb); kb.quiet = quiet; kb.project = kb_isdir(".katana/coder") ? ".katana/coder" : NULL;
    int lr = coder_kb_load(coder_kb_default_dir(), &kb);
    if (lr && strcmp(argv[0], "check")) { fprintf(stderr, "kb: base com erros (rode `katana coder kb check`)\n"); if (!kb.nfile) return 1; }
    if (!strcmp(argv[0], "check")) { printf("kb dir: %s%s%s\n", coder_kb_default_dir(), kb.project ? " + " : "", kb.project ? kb.project : ""); return kb_cmd_check(&kb); }
    if (!strcmp(argv[0], "stats")) { kb_print_counts(&kb, 1); return 0; }
    if (!strcmp(argv[0], "edges")) { for (int i = 0; i < kb.nedge; i++) printf("%-14s %.2f  %s\n", kb.edge[i].name, kb.edge[i].w, kb.edge[i].note); return 0; }
    if (!strcmp(argv[0], "lookup")) {
        if (argc < 2) { fprintf(stderr, "uso: katana coder kb lookup <termo>\n"); return 2; }
        char q[512]; q[0] = 0; for (int i = 1; i < argc; i++) { if (i > 1) strcat(q, " "); strncat(q, argv[i], sizeof q - strlen(q) - 2); }
        printf("lookup '%s'\n", q);
        if (!kb_show_word(&kb, q, 0)) printf("  (no match)\n");
        return 0;
    }
    if (!strcmp(argv[0], "expand")) {
        if (argc < 2) { fprintf(stderr, "uso: katana coder kb expand \"frase\"\n"); return 2; }
        char q[1024]; q[0] = 0; for (int i = 1; i < argc; i++) { if (i > 1) strcat(q, " "); strncat(q, argv[i], sizeof q - strlen(q) - 2); }
        char buf[1024]; coder_kb_norm(q, buf, sizeof buf);
        for (char *c = buf; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) *c = ' ';
        char *w[64]; int n = kb_words(buf, w, 64);
        for (int i = 0; i < n;) {
            int used = 1, found = 0;
            static const char *const stop[] = {"de", "do", "da", "dos", "das", "o", "a", "os", "as", "e", "em", "no", "na", "para", "por", "com", "um", "uma", "the", "of", "to", "and", "in", "for", "an", NULL};
            int isstop = 0; for (int s = 0; stop[s]; s++) if (!strcmp(w[i], stop[s])) isstop = 1;
            if (isstop) { i++; continue; }
            for (int len = (n - i < 3 ? n - i : 3); len >= 2 && !found; len--) {   /* frases de ate 3 palavras que sejam termo */
                char ph[300]; ph[0] = 0; for (int k = 0; k < len; k++) { if (k) strcat(ph, " "); strncat(ph, w[i + k], 90); }
                int ids[2]; if (coder_kb_term_concepts(&kb, ph, ids, 2) || coder_kb_verb_ops(&kb, ph, ids, 2)) { printf("'%s'\n", ph); kb_show(&kb, ph, 1); used = len; found = 1; } }
            if (!found) { printf("'%s'\n", w[i]); if (!kb_show_word(&kb, w[i], 1)) printf("  (no match)\n"); }
            i += used;
        }
        return 0;
    }
    fprintf(stderr, "kb: subcomando desconhecido '%s'\n", argv[0]); return 2;
}
