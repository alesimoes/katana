/* ---------- ferramenta Find (ver docs/tools.md) ----------
 * Navegacao por intencao: o modelo diz o que procura (frase e/ou identificadores) e recebe poucos candidatos
 * (arquivo:linhaA-linhaB, simbolos definidos ali, 2 a 4 linhas, outros arquivos que usam esses simbolos), sem ler
 * arquivos inteiros. mode=refs: onde um simbolo e definido e as linhas que o usam, com a funcao em volta de cada uso.
 *
 * Indice: arquivos de texto da pasta (git ls-files quando ha repositorio, senao caminhada com .gitignore), trechos de
 * 40 linhas com passo 25, tres campos por trecho (caminho, simbolos definidos, corpo). Guardado em
 * <pasta>/.katana/cache/find-<hash>.idx (com .gitignore que ignora tudo); a cada chamada so os arquivos com data ou
 * tamanho diferentes sao lidos de novo. O grafo de referencias e o proprio indice invertido: o termo inteiro de um
 * identificador (listSessions -> list_session) leva aos trechos que o citam.
 * Ranking: BM25 por campo (pesos 3/1/1), radical simples, bonus de 10% por termo distinto da pergunta no trecho,
 * arquivo = melhor trecho + 0.3 do segundo (medido com tools/findbench/bench.py). KATANA_FIND_TUNE muda os parametros (pesquisa). */
static const char CT_FIND_DESC[] =
    "Find where something is in the code of the working folder without reading whole files. `query`: what you are "
    "looking for, in plain words and/or identifiers (e.g. \"where the session list is sent to the phone\" or "
    "\"listSessions\"). Returns a few candidates, best first: file:firstLine-lastLine, the symbols defined there, the "
    "best matching lines and other files that reference those symbols. mode=\"refs\" with a symbol name in `query`: "
    "where it is defined and the lines that use it, grouped by file, each with the function it is in; use it to follow "
    "a call chain. `path` limits the search to a subfolder; `limit` is the number of candidates (default 6, max 12). "
    "A name or \"quoted text\" is checked exactly in the files (NOT FOUND when it does not exist). "
    "Then Read only the part you need (offset and limit).";
static const char CT_FIND_PROPS[] =
    "\"query\": {\"type\": \"string\", \"description\": \"What you are looking for: a phrase and/or identifiers; with mode=refs, one symbol name\"}, "
    "\"path\": {\"type\": \"string\", \"description\": \"Subfolder to search in (default: the working folder)\"}, "
    "\"limit\": {\"type\": \"integer\", \"description\": \"Number of candidates (default 6, max 12)\"}, "
    "\"mode\": {\"type\": \"string\", \"enum\": [\"search\", \"refs\"], \"description\": \"search (default) or refs: definition and uses of a symbol\"}";
static const char CT_FIND_REQ[] = "\"query\"";

/* resultado estruturado da busca do coder (preenchido por coder_lookup em src/coder/search.c; a camada de decisao do Find esta mais abaixo) */
#define CODER_HITS_MAX 12
#define CODER_EV_MAX 3
typedef struct { char path[512]; int exact, tier, literal, anchor, nev, line[CODER_EV_MAX]; char text[CODER_EV_MAX][150]; char channels[200]; } CoderHit;
typedef struct { int n, total, concept, created; uint32_t files; double ms, index_ms; char notfound[600]; CoderHit r[CODER_HITS_MAX]; } CoderHits;

#ifndef _WIN32
static int coder_lookup(const char *root, const char *query, int concept, int limit, CoderHits *ho);   /* search.c */
#define FD_MAGIC 0x444e464bu       /* "KFND" */
#define FD_VERSION 4u       /* 4: grafo de arquivos depois dos arquivos (findgraph.c) */
#define FD_CHUNK 40
#define FD_STRIDE 25
#define FD_MAXBYTES 300000
#define FD_TERM_MAX (1 << 22)       /* ids de termo cabem em 22 bits (termo<<10 | campo<<8 | tf) */

/* findgraph.c (incluido depois deste arquivo) */
struct FdIndexS;
typedef struct { int file, chunk; double score; } FdHit;
static void fg_free(void);
static void fg_neighbor_note(const char *name, const char *js, const char *end, char *result, size_t cap);
static int fg_save(FILE *f, struct FdIndexS *X);
static void fg_load(const char *p, size_t len, struct FdIndexS *X);
static int fg_act_on(void);
static int fg_ensure(struct FdIndexS *X);
static int fg_drop_request(struct FdIndexS *X, uint32_t *qid, double *qw, int n);
static int fg_activate(struct FdIndexS *X, const FdHit *all, int na, const char *pre, int k, FdHit *hits, int *esrc);
static void fg_evidence(struct FdIndexS *X, int src, FdHit *h, char *why, size_t cap);

static double fd_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* ---- vetor de inteiros ---- */
typedef struct { uint32_t *v; int n, cap; } FdIv;
static void fd_iv_push(FdIv *a, uint32_t x) {
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 256; a->v = (uint32_t *)realloc(a->v, sizeof(uint32_t) * (size_t)a->cap); }
    a->v[a->n++] = x;
}

/* ---- tabela de termos (texto -> id) ---- */
typedef struct { char *pool; size_t pn, pcap; uint32_t *off; uint16_t *len; int n, cap; int *ht; int hcap; } FdTerms;
static uint32_t fd_hash(const char *s, int l) { uint32_t h = 2166136261u; for (int i = 0; i < l; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; } return h; }
static void fd_rehash(FdTerms *t) {
    const int nc = t->hcap ? t->hcap * 2 : 1 << 16;
    free(t->ht); t->ht = (int *)calloc((size_t)nc, sizeof(int)); t->hcap = nc;
    for (int i = 0; i < t->n; i++) {
        uint32_t h = fd_hash(t->pool + t->off[i], t->len[i]) & (uint32_t)(nc - 1);
        while (t->ht[h]) h = (h + 1) & (uint32_t)(nc - 1);
        t->ht[h] = i + 1;
    }
}
/* id do termo; add=0 devolve -1 se nao existe */
static int fd_term(FdTerms *t, const char *s, int l, int add) {
    if (l <= 0 || l > 200) return -1;
    if (add && (t->n + 1) * 2 > t->hcap) fd_rehash(t);
    if (!t->hcap) return -1;
    uint32_t h = fd_hash(s, l) & (uint32_t)(t->hcap - 1);
    while (t->ht[h]) {
        const int id = t->ht[h] - 1;
        if (t->len[id] == l && memcmp(t->pool + t->off[id], s, (size_t)l) == 0) return id;
        h = (h + 1) & (uint32_t)(t->hcap - 1);
    }
    if (!add || t->n >= FD_TERM_MAX) return -1;
    if (t->pn + (size_t)l + 1 > t->pcap) { t->pcap = t->pcap ? t->pcap * 2 : 1 << 20; while (t->pn + (size_t)l + 1 > t->pcap) t->pcap *= 2; t->pool = (char *)realloc(t->pool, t->pcap); }
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 1 << 15; t->off = (uint32_t *)realloc(t->off, sizeof(uint32_t) * (size_t)t->cap); t->len = (uint16_t *)realloc(t->len, sizeof(uint16_t) * (size_t)t->cap); }
    memcpy(t->pool + t->pn, s, (size_t)l); t->pool[t->pn + (size_t)l] = 0;
    t->off[t->n] = (uint32_t)t->pn; t->len[t->n] = (uint16_t)l; t->pn += (size_t)l + 1;
    t->ht[h] = t->n + 1;
    return t->n++;
}
static const char *fd_tstr(const FdTerms *t, int id) { return t->pool + t->off[id]; }

/* ---- termos de um texto ----
 * Acentos latinos viram a letra sem acento; cada identificador vira ele inteiro (partes unidas por _) e suas partes
 * (camelCase, snake_case); minusculas; descarta termos de 1 letra e palavras vazias; radical simples. */
static const char fd_c3[64] = "AAAAAA CEEEEIIII NOOOOO  UUUUY  aaaaaa ceeeeiiii nooooo  uuuuy y";
static size_t fd_fold(const char *s, size_t n, char *o) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) o[k++] = (char)c;
        else if (c == 0xC3 && i + 1 < n && ((unsigned char)s[i + 1] & 0xC0) == 0x80) { o[k++] = fd_c3[(unsigned char)s[i + 1] - 0x80]; i++; }
        else o[k++] = ' ';
    }
    return k;
}
static const char *const fd_stopw[] = {"as", "os", "um", "uma", "de", "do", "da", "dos", "das", "em", "no", "na", "nos", "nas", "para",
    "por", "com", "sem", "ou", "que", "se", "ao", "aos", "como", "mais", "ja", "so", "the", "of", "to", "in", "on", "for", "and", "or",
    "with", "is", "are", "be", "from", "by", "it", "its", "this", "that", "an", "at", "not", "into", "per", "via", NULL};
static int fd_is_stop(const char *w, int l) {
    if (l > 5) return 0;
    for (int i = 0; fd_stopw[i]; i++) if ((int)strlen(fd_stopw[i]) == l && memcmp(fd_stopw[i], w, (size_t)l) == 0) return 1;
    return 0;
}
static int fd_ends(const char *w, int l, const char *suf) { const int s = (int)strlen(suf); return l >= s && memcmp(w + l - s, suf, (size_t)s) == 0; }
static int fd_stem(char *w, int l) {
    if (l < 5) return l;
    int dig = 1; for (int i = 0; i < l; i++) if (!isdigit((unsigned char)w[i])) { dig = 0; break; }
    if (dig) return l;
    if (fd_ends(w, l, "ies")) { w[l - 3] = 'y'; return l - 2; }
    if (fd_ends(w, l, "coes")) { memcpy(w + l - 4, "cao", 3); return l - 1; }
    if (fd_ends(w, l, "ing") && l > 6) return l - 3;
    if (fd_ends(w, l, "ed") && l > 5) return l - 2;
    if (w[l - 1] == 's' && !fd_ends(w, l, "ss") && !fd_ends(w, l, "us")) return l - 1;
    return l;
}
typedef struct { FdTerms *T; int add; FdIv *out; } FdTok;
static void fd_emit(FdTok *k, const char *s, int l) {
    if (l < 2 || l > 120) return;
    char b[128];
    for (int i = 0; i < l; i++) b[i] = (char)tolower((unsigned char)s[i]);
    if (fd_is_stop(b, l)) return;
    l = fd_stem(b, l);
    const int id = fd_term(k->T, b, l, k->add);
    if (id >= 0) fd_iv_push(k->out, (uint32_t)id);
}
static void fd_word(FdTok *k, const char *w, int l) {
    if (isdigit((unsigned char)w[0])) { fd_emit(k, w, l); return; }
    int ps[48], pe[48], np = 0, st = -1;
    for (int i = 0; i <= l; i++) {
        const int c = i < l ? (unsigned char)w[i] : '_';
        int cut = c == '_';
        if (!cut && st >= 0 && i > st) {
            const int a = (unsigned char)w[i - 1];
            if ((islower(a) || isdigit(a)) && isupper(c)) cut = 2;
            else if (isupper(a) && isupper(c) && i + 1 < l && islower((unsigned char)w[i + 1])) cut = 2;
        }
        if (cut && st >= 0) { if (np < 48) { ps[np] = st; pe[np] = i; np++; } st = -1; }
        if (c != '_' && st < 0) st = i;
    }
    if (np > 1) {
        char j[128]; int jl = 0;
        for (int p = 0; p < np && jl < 120; p++) {
            if (p) j[jl++] = '_';
            for (int i = ps[p]; i < pe[p] && jl < 120; i++) j[jl++] = w[i];
        }
        fd_emit(k, j, jl);
    }
    for (int p = 0; p < np; p++) fd_emit(k, w + ps[p], pe[p] - ps[p]);
}
/* texto ja dobrado (so ASCII) */
static void fd_tokens(FdTok *k, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        const int c = (unsigned char)s[i];
        if (isalpha(c) || c == '_') {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            fd_word(k, s + i, (int)(j - i)); i = j;
        } else if (isdigit(c)) {
            size_t j = i + 1;
            while (j < n && isdigit((unsigned char)s[j])) j++;
            fd_word(k, s + i, (int)(j - i)); i = j;
        } else i++;
    }
}
static void fd_tokens_raw(FdTok *k, const char *s, size_t n) {
    char *f = (char *)malloc(n + 1);
    const size_t m = fd_fold(s, n, f);
    fd_tokens(k, f, m);
    free(f);
}

/* ---- tipos de arquivo e simbolos definidos ---- */
enum { FK_OTHER, FK_C, FK_JS, FK_PY, FK_JV, FK_GO, FK_RS, FK_SH, FK_MD, FK_QML, FK_PS };
static int fd_kind(const char *rel) {
    const char *b = strrchr(rel, '/'); b = b ? b + 1 : rel;
    if (!strcmp(b, "Makefile") || !strcmp(b, "Dockerfile") || !strcmp(b, "CMakeLists.txt")) return FK_OTHER;
    const char *d = strrchr(b, '.');
    if (!d) return -1;
    char e[12]; size_t l = strlen(d + 1);
    if (l == 0 || l >= sizeof e) return -1;
    for (size_t i = 0; i <= l; i++) e[i] = (char)tolower((unsigned char)d[1 + i]);
    static const struct { const char *e; int k; } tb[] = {
        {"c", FK_C}, {"h", FK_C}, {"cpp", FK_C}, {"cc", FK_C}, {"cxx", FK_C}, {"hpp", FK_C}, {"hxx", FK_C}, {"glsl", FK_C}, {"comp", FK_C},
        {"vert", FK_C}, {"frag", FK_C}, {"m", FK_C}, {"js", FK_JS}, {"mjs", FK_JS}, {"cjs", FK_JS}, {"ts", FK_JS}, {"tsx", FK_JS},
        {"jsx", FK_JS}, {"py", FK_PY}, {"kt", FK_JV}, {"kts", FK_JV}, {"java", FK_JV}, {"cs", FK_JV}, {"swift", FK_JV}, {"gradle", FK_JV},
        {"go", FK_GO}, {"rs", FK_RS}, {"sh", FK_SH}, {"bash", FK_SH}, {"md", FK_MD}, {"qml", FK_QML}, {"ps1", FK_PS}, {"psm1", FK_PS},
        {"xml", FK_OTHER}, {"toml", FK_OTHER}, {"yml", FK_OTHER}, {"yaml", FK_OTHER}, {"cfg", FK_OTHER}, {"txt", FK_OTHER},
        {"cmake", FK_OTHER}, {"lua", FK_OTHER}, {"rb", FK_OTHER}, {"php", FK_OTHER}, {NULL, 0}};
    for (int i = 0; tb[i].e; i++) if (!strcmp(tb[i].e, e)) return tb[i].k;
    return -1;
}
static int fd_isid(int c) { return isalnum(c) || c == '_' || c == '$'; }
static int fd_idlen(const char *p, const char *e, int kind) {
    const char *q = p;
    if (q >= e || !(isalpha((unsigned char)*q) || *q == '_' || *q == '$')) return 0;
    while (q < e && (fd_isid((unsigned char)*q) || (kind == FK_PS && *q == '-'))) q++;
    return (int)(q - p);
}
static const char *fd_ws(const char *p, const char *e) { while (p < e && (*p == ' ' || *p == '\t')) p++; return p; }
/* palavra-chave seguida de espaco; avanca p */
static int fd_kw(const char **pp, const char *e, const char *kw) {
    const size_t l = strlen(kw);
    const char *p = *pp;
    if ((size_t)(e - p) <= l || memcmp(p, kw, l) != 0 || !(p[l] == ' ' || p[l] == '\t')) return 0;
    *pp = fd_ws(p + l, e);
    return 1;
}
static int fd_arrow(const char *p, const char *e) { for (; p + 1 < e; p++) if (p[0] == '=' && p[1] == '>') return 1; return 0; }
static int fd_ctrl(const char *w, int l) {
    static const char *const k[] = {"if", "for", "while", "switch", "catch", "return", "else", "new", "throw", "case", "do", "try",
        "sizeof", "typeof", "await", "yield", "delete", "super", "this", "synchronized", "using", "foreach", "lock", "elif", "with",
        "assert", "print", "in", "function", "defined", "static_assert", "goto", "not", "and", "or", "when", "repeat", NULL};
    for (int i = 0; k[i]; i++) if ((int)strlen(k[i]) == l && !memcmp(k[i], w, (size_t)l)) return 1;
    return 0;
}
/* nome definido numa linha; devolve o comprimento (0 = nenhum) e *ns o inicio */
static int fd_sym(const char *s, int n, int kind, const char **ns) {
    const char *e = s + n;
    while (e > s && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
    const char *p = fd_ws(s, e);
    const int indent = (int)(p - s);
    if (p >= e) return 0;
    if (kind == FK_MD) {
        const char *q = p; int h = 0;
        while (q < e && *q == '#') { q++; h++; }
        if (indent || h < 1 || h > 6 || q >= e || *q != ' ') return 0;
        q = fd_ws(q, e);
        const char *z = e; while (z > q && (z[-1] == '#' || z[-1] == ' ')) z--;
        if (z <= q) return 0;
        *ns = q; return (int)(z - q > 80 ? 80 : z - q);
    }
    if (kind == FK_OTHER || kind < 0) return 0;
    if (kind == FK_C && *p == '#') {
        const char *q = fd_ws(p + 1, e);
        if (fd_kw(&q, e, "define")) { const int l = fd_idlen(q, e, kind); if (l) { *ns = q; return l; } }
        return 0;
    }
    if (*p == '/' || *p == '*' || (*p == '#' && kind != FK_C) || *p == '@') return 0;
    if (kind == FK_C && *p == '}' && !indent) {         /* } Nome; (fim de typedef) */
        const char *q = fd_ws(p + 1, e);
        const int l = fd_idlen(q, e, kind);
        if (l && fd_ws(q + l, e) < e && *fd_ws(q + l, e) == ';') { *ns = q; return l; }
        return 0;
    }
    /* modificadores */
    static const char *const mods[] = {"export", "default", "async", "static", "public", "private", "protected", "internal", "override",
        "open", "abstract", "final", "inline", "extern", "pub", "pub(crate)", "unsafe", "suspend", "data", "sealed", "virtual", "local",
        "readonly", "declare", "companion", "lateinit", "operator", "infix", "tailrec", "external", "enum", "annotation", "inner", "value", NULL};
    for (int again = 1; again; ) {
        again = 0;
        for (int i = 0; mods[i]; i++) {
            if (!strcmp(mods[i], "enum") && kind != FK_JV) continue;
            const char *q = p;
            if (fd_kw(&q, e, mods[i])) {
                /* "enum class X" no kotlin; "enum X" sozinho e definicao: so trata como modificador se vier outra palavra-chave */
                if (!strcmp(mods[i], "enum") && !(fd_kw(&(const char *){q}, e, "class"))) continue;
                p = q; again = 1; break;
            }
        }
    }
    static const char *const defs[] = {"function", "def", "class", "struct", "union", "interface", "trait", "fun", "func", "fn",
        "type", "object", "mod", "signal", "property", "namespace", "record", "enum", "macro_rules!", "typedef", NULL};
    for (int i = 0; defs[i]; i++) {
        const char *q = p;
        if (!fd_kw(&q, e, defs[i])) continue;
        const char *d = defs[i];
        if (!strcmp(d, "typedef")) {            /* typedef ... Nome; */
            if (kind != FK_C) return 0;
            const char *z = e; if (z > q && z[-1] == ';') z--; else return 0;
            if (memchr(q, '(', (size_t)(z - q))) return 0;
            const char *a = z; while (a > q && fd_isid((unsigned char)a[-1])) a--;
            const int l = fd_idlen(a, z, kind); if (l) { *ns = a; return l; }
            return 0;
        }
        if (!strcmp(d, "type") && kind != FK_GO && kind != FK_RS && kind != FK_JS) return 0;
        if (!strcmp(d, "object") && kind != FK_JV) return 0;
        if (!strcmp(d, "property")) {           /* qml: property tipo nome */
            if (kind != FK_QML) return 0;
            const int t = fd_idlen(q, e, kind); if (!t) return 0;
            q = q + t;
            while (q < e && (*q == '<' || *q == '>' || fd_isid((unsigned char)*q) || *q == '.')) q++;
            q = fd_ws(q, e);
        }
        if (kind == FK_GO && !strcmp(d, "func") && q < e && *q == '(') {      /* receptor */
            const char *r = memchr(q, ')', (size_t)(e - q)); if (!r) return 0;
            q = fd_ws(r + 1, e);
        }
        if (*q == '*') q = fd_ws(q + 1, e);     /* function* gerador */
        if (kind == FK_JV && !strcmp(d, "fun") && *q == '<') { const char *r = memchr(q, '>', (size_t)(e - q)); if (r) q = fd_ws(r + 1, e); }
        int l = fd_idlen(q, e, kind);
        if (!l) return 0;
        if (kind == FK_JV && !strcmp(d, "fun")) {      /* fun Tipo.nome( */
            const char *r = q + l;
            while (r < e && *r == '.') { const int l2 = fd_idlen(r + 1, e, kind); if (!l2) break; q = r + 1; l = l2; r = q + l; }
        }
        if ((!strcmp(d, "struct") || !strcmp(d, "union") || !strcmp(d, "enum")) && kind == FK_C) {
            const char *r = fd_ws(q + l, e);
            if (r < e && *r != '{' && *r != ':') return 0;      /* struct x *p = ... e uso, nao definicao */
        }
        *ns = q; return l;
    }
    /* var/const/let/val */
    if ((kind == FK_JS && indent == 0) || (kind == FK_JV && indent <= 4) || (kind == FK_GO && indent == 0) || (kind == FK_RS && indent <= 4)) {
        static const char *const vk[] = {"const", "let", "var", "val", "static", NULL};
        for (int i = 0; vk[i]; i++) {
            const char *q = p;
            if (!fd_kw(&q, e, vk[i])) continue;
            if (kind == FK_RS && fd_kw(&q, e, "mut")) {}
            const int l = fd_idlen(q, e, kind);
            if (l) { *ns = q; return l; }
            return 0;
        }
    }
    if (kind == FK_PY) {             /* NOME = no nivel do modulo */
        if (indent) return 0;
        const int l = fd_idlen(p, e, kind);
        const char *r = fd_ws(p + l, e);
        if (l && r < e && (*r == '=' || *r == ':') && (r + 1 >= e || r[1] != '=')) { *ns = p; return l; }
        return 0;
    }
    if (kind == FK_QML) {             /* id: nome */
        const char *q = p;
        if (e - q > 3 && !memcmp(q, "id:", 3)) { q = fd_ws(q + 3, e); const int l = fd_idlen(q, e, kind); if (l) { *ns = q; return l; } }
        return 0;
    }
    if (kind == FK_SH) {             /* nome() { */
        const int l = fd_idlen(p, e, kind);
        const char *r = fd_ws(p + l, e);
        if (l && e - r >= 2 && r[0] == '(' && r[1] == ')') { *ns = p; return l; }
        return 0;
    }
    if (kind == FK_JS) {
        const int l = fd_idlen(p, e, kind);
        if (!l || fd_ctrl(p, l)) return 0;
        const char *r = fd_ws(p + l, e);
        if (r < e && *r == '(' && e[-1] == '{' && indent > 0) {            /* metodo: nome(args) { */
            if (!memchr(r, ';', (size_t)(e - r))) { *ns = p; return l; }
            return 0;
        }
        if ((kind == FK_JS && (l == 3 && (!memcmp(p, "get", 3) || !memcmp(p, "set", 3)))) && r < e) {  /* get nome() */
            const int l2 = fd_idlen(r, e, kind);
            const char *r2 = fd_ws(r + l2, e);
            if (l2 && r2 < e && *r2 == '(') { *ns = r; return l2; }
        }
        if (r < e && (*r == '=' || *r == ':') && (r + 1 >= e || (r[1] != '=' && r[1] != '>'))) {         /* nome = function / nome: (...) => */
            const char *v = fd_ws(r + 1, e);
            if ((e - v >= 8 && !memcmp(v, "function", 8)) || (e - v >= 5 && !memcmp(v, "async", 5)) ||
                (memchr(v, '(', (size_t)(e - v)) && fd_arrow(v, e))) { *ns = p; return l; }
        }
        return 0;
    }
    if (kind == FK_PS || kind == FK_RS || kind == FK_GO) return 0;
    /* C e parecidos: tipo ... nome( sem ; no fim; no C so no inicio da linha */
    if (kind == FK_C && indent) return 0;
    {
        const char *q = p, *last = NULL; int ll = 0, words = 0;
        while (q < e) {
            if (*q == '(') break;
            if (*q == '=' || *q == ';' || *q == '.' || *q == ',' || *q == '"' || *q == '\'' || *q == '{' || *q == '}' || *q == '[' || *q == '!' || *q == '?' || *q == '|' || *q == '+') { last = NULL; break; }
            const int l = fd_idlen(q, e, kind);
            if (l) {
                if (fd_ctrl(q, l)) return 0;
                last = q; ll = l; words++; q += l;
            } else q++;
        }
        if (q < e && *q == '(' && last && ll && words >= 2 && e[-1] != ';' && e[-1] != ',') {
            if (kind == FK_C || e[-1] == '{' || e[-1] == ')') { *ns = last; return ll; }
        }
        /* variavel global do C: tipo nome[] = ... (sem parenteses antes do =) */
        if (kind == FK_C) {
            const char *eq = memchr(p, '=', (size_t)(e - p));
            if (eq && eq > p && eq[-1] != '=' && eq[-1] != '!' && eq[-1] != '<' && eq[-1] != '>' && (eq + 1 >= e || eq[1] != '=') && !memchr(p, '(', (size_t)(eq - p))) {
                const char *z = eq; while (z > p && (z[-1] == ' ' || z[-1] == ']' || fd_isid((unsigned char)z[-1]) || z[-1] == '[')) { if (z[-1] == ' ' && fd_isid((unsigned char)z[-2])) break; z--; }
                const char *a = eq; while (a > p && (a[-1] == ' ' || a[-1] == '\t')) a--;
                while (a > p && a[-1] == ']') { const char *o = a - 1; while (o > p && *o != '[') o--; a = o; while (a > p && a[-1] == ' ') a--; }
                const char *b0 = a; while (b0 > p && fd_isid((unsigned char)b0[-1])) b0--;
                int w2 = 0; for (const char *t = p; t < b0; ) { const int l = fd_idlen(t, b0, kind); if (l) { w2++; t += l; } else t++; }
                const int l = fd_idlen(b0, a, kind);
                if (l && w2 >= 1) { *ns = b0; return l; }
            }
        }
    }
    return 0;
}

/* ---- indice ---- */
typedef struct { char *rel; long long mt, size; uint32_t *blob; uint32_t nb; int kind; } FdFile;
typedef struct FdIndexS {
    char root[4096], cache[4400];
    FdTerms T;
    FdFile *f; int nf, capf;
    int listed, truncated;
    /* invertido (refeito a cada mudanca) */
    int nc; int *cf, *ca, *cb; float *cl[3]; double avg[3];
    int *poff[3]; int *pch[3]; uint8_t *ptf[3];
    int ns; int *sf, *sl, *sn; int *fs0, *fsn, *fc0, *fcn;
    int *shead, *snext;     /* simbolos pelo id do nome (lista encadeada) */
    int ready;
} FdIndex;
static FdIndex g_fd;

/* blob de um arquivo: [nchunk] {a b lp ls lb np (termo<<10|campo<<8|tf)*np} [nsym] {linha nome}* */
static uint32_t *g_fd_cnt; static int g_fd_cnt_cap;
static void fd_cnt_room(int n) {
    if (n <= g_fd_cnt_cap) return;
    int c = g_fd_cnt_cap ? g_fd_cnt_cap : 1 << 16; while (c < n) c *= 2;
    g_fd_cnt = (uint32_t *)realloc(g_fd_cnt, sizeof(uint32_t) * (size_t)c);
    memset(g_fd_cnt + g_fd_cnt_cap, 0, sizeof(uint32_t) * (size_t)(c - g_fd_cnt_cap));
    g_fd_cnt_cap = c;
}
/* conta os termos de v[a..b) no campo f e grava no blob */
static int fd_put_field(FdIv *blob, const uint32_t *v, int n, int field, FdIv *touched) {
    touched->n = 0;
    for (int i = 0; i < n; i++) { if (!g_fd_cnt[v[i]]++) fd_iv_push(touched, v[i]); }
    for (int i = 0; i < touched->n; i++) {
        const uint32_t t = touched->v[i], c = g_fd_cnt[t] > 255 ? 255 : g_fd_cnt[t];
        fd_iv_push(blob, t << 10 | (uint32_t)field << 8 | c);
        g_fd_cnt[t] = 0;
    }
    return touched->n;
}
static int fd_is_binary(const char *s, size_t n) { const size_t m = n < 8000 ? n : 8000; return memchr(s, 0, m) != NULL; }
static char *fd_slurp(const char *path, size_t *n, size_t maxb) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    char *b = (char *)malloc(maxb + 1);
    *n = fread(b, 1, maxb, fp);
    fclose(fp);
    b[*n] = 0;
    return b;
}
static void fd_build_file(FdIndex *X, FdFile *F) {
    char abs[8400];
    snprintf(abs, sizeof abs, "%s/%s", X->root, F->rel);
    free(F->blob); F->blob = NULL; F->nb = 0;
    FdIv blob = {0};
    size_t n = 0;
    char *s = F->size <= FD_MAXBYTES ? fd_slurp(abs, &n, FD_MAXBYTES) : NULL;
    if (!s || fd_is_binary(s, n) || n == 0) { fd_iv_push(&blob, 0); fd_iv_push(&blob, 0); free(s); F->blob = blob.v; F->nb = (uint32_t)blob.n; return; }
    /* linhas */
    FdIv ls = {0};
    for (size_t i = 0; i < n; ) { fd_iv_push(&ls, (uint32_t)i); const char *nl = memchr(s + i, '\n', n - i); i = nl ? (size_t)(nl - s) + 1 : n; }
    const int L = ls.n;
    FdIv tok = {0}, lt = {0}, ptok = {0}, stok = {0}, st0 = {0}, syl = {0}, synm = {0}, touched = {0};
    FdTok k = {&X->T, 1, &tok};
    char *fold = (char *)malloc(n + 1);
    for (int i = 0; i < L; i++) {
        const size_t a = ls.v[i], b = i + 1 < L ? ls.v[i + 1] : n;
        fd_iv_push(&lt, (uint32_t)tok.n);
        const size_t m = fd_fold(s + a, b - a, fold);
        fd_tokens(&k, fold, m);
        const char *nm = NULL;
        const int nl = fd_sym(s + a, (int)(b - a - (b > a && s[b - 1] == '\n')), F->kind, &nm);
        if (nl > 0) {
            const int id = fd_term(&X->T, nm, nl, 1);
            if (id >= 0) {
                fd_iv_push(&syl, (uint32_t)(i + 1)); fd_iv_push(&synm, (uint32_t)id);
                fd_iv_push(&st0, (uint32_t)stok.n);
                FdTok k2 = {&X->T, 1, &stok};
                const size_t m2 = fd_fold(nm, (size_t)nl, fold);
                fd_tokens(&k2, fold, m2);
            }
        }
    }
    fd_iv_push(&lt, (uint32_t)tok.n);
    fd_iv_push(&st0, (uint32_t)stok.n);
    { FdTok k3 = {&X->T, 1, &ptok}; fd_tokens_raw(&k3, F->rel, strlen(F->rel)); }
    fd_cnt_room(X->T.n + 1);
    int nch = 0;
    fd_iv_push(&blob, 0);
    for (int i = 0; ; i += FD_STRIDE) {
        const int a = i + 1, b = L <= FD_CHUNK ? L : (i + FD_CHUNK < L ? i + FD_CHUNK : L);
        const int at = blob.n;
        fd_iv_push(&blob, (uint32_t)a); fd_iv_push(&blob, (uint32_t)b);
        fd_iv_push(&blob, (uint32_t)ptok.n);
        /* simbolos do trecho */
        int s0 = -1, s1 = -1;
        for (int q = 0; q < syl.n; q++) if ((int)syl.v[q] >= a && (int)syl.v[q] <= b) { if (s0 < 0) s0 = q; s1 = q; }
        const int sa = s0 < 0 ? 0 : (int)st0.v[s0], sb = s0 < 0 ? 0 : (int)st0.v[s1 + 1];
        fd_iv_push(&blob, (uint32_t)(sb - sa));
        fd_iv_push(&blob, (uint32_t)(lt.v[b] - lt.v[a - 1]));
        fd_iv_push(&blob, 0);
        int np = 0;
        np += fd_put_field(&blob, ptok.v, ptok.n, 0, &touched);
        np += fd_put_field(&blob, stok.v + sa, sb - sa, 1, &touched);
        np += fd_put_field(&blob, tok.v + lt.v[a - 1], (int)(lt.v[b] - lt.v[a - 1]), 2, &touched);
        blob.v[at + 5] = (uint32_t)np;
        nch++;
        if (b >= L) break;
    }
    blob.v[0] = (uint32_t)nch;
    fd_iv_push(&blob, (uint32_t)syl.n);
    for (int q = 0; q < syl.n; q++) { fd_iv_push(&blob, syl.v[q]); fd_iv_push(&blob, synm.v[q]); }
    free(fold); free(s); free(ls.v); free(tok.v); free(lt.v); free(ptok.v); free(stok.v); free(st0.v); free(syl.v); free(synm.v); free(touched.v);
    F->blob = blob.v; F->nb = (uint32_t)blob.n;
}

/* inverte os blobs: trechos, listas por termo e campo, simbolos */
static void fd_invert(FdIndex *X) {
    for (int f = 0; f < 3; f++) { free(X->poff[f]); free(X->pch[f]); free(X->ptf[f]); free(X->cl[f]); }
    free(X->cf); free(X->ca); free(X->cb); free(X->sf); free(X->sl); free(X->sn); free(X->fs0); free(X->fsn); free(X->fc0); free(X->fcn);
    free(X->shead); free(X->snext);
    int nc = 0, ns = 0;
    const int T = X->T.n;
    int *cnt[3];
    for (int f = 0; f < 3; f++) cnt[f] = (int *)calloc((size_t)T + 1, sizeof(int));
    for (int i = 0; i < X->nf; i++) {
        const uint32_t *b = X->f[i].blob; uint32_t p = 1;
        const int k = (int)b[0];
        for (int c = 0; c < k; c++) {
            const int np = (int)b[p + 5]; p += 6;
            for (int j = 0; j < np; j++) { const uint32_t v = b[p + j]; cnt[(v >> 8) & 3][v >> 10]++; }
            p += (uint32_t)np;
        }
        nc += k; ns += (int)b[p];
    }
    X->nc = nc; X->ns = ns;
    X->cf = (int *)malloc(sizeof(int) * (size_t)(nc + 1)); X->ca = (int *)malloc(sizeof(int) * (size_t)(nc + 1)); X->cb = (int *)malloc(sizeof(int) * (size_t)(nc + 1));
    for (int f = 0; f < 3; f++) {
        X->cl[f] = (float *)malloc(sizeof(float) * (size_t)(nc + 1));
        X->poff[f] = (int *)malloc(sizeof(int) * ((size_t)T + 1));
        int acc = 0;
        for (int t = 0; t < T; t++) { X->poff[f][t] = acc; acc += cnt[f][t]; }
        X->poff[f][T] = acc;
        X->pch[f] = (int *)malloc(sizeof(int) * (size_t)(acc + 1)); X->ptf[f] = (uint8_t *)malloc((size_t)acc + 1);
        memcpy(cnt[f], X->poff[f], sizeof(int) * (size_t)T);
    }
    X->sf = (int *)malloc(sizeof(int) * (size_t)(ns + 1)); X->sl = (int *)malloc(sizeof(int) * (size_t)(ns + 1)); X->sn = (int *)malloc(sizeof(int) * (size_t)(ns + 1));
    X->fs0 = (int *)malloc(sizeof(int) * (size_t)(X->nf + 1)); X->fsn = (int *)malloc(sizeof(int) * (size_t)(X->nf + 1));
    X->fc0 = (int *)malloc(sizeof(int) * (size_t)(X->nf + 1)); X->fcn = (int *)malloc(sizeof(int) * (size_t)(X->nf + 1));
    double sum[3] = {0, 0, 0};
    int c = 0, s = 0;
    for (int i = 0; i < X->nf; i++) {
        const uint32_t *b = X->f[i].blob; uint32_t p = 1;
        const int k = (int)b[0];
        X->fc0[i] = c; X->fcn[i] = k;
        for (int q = 0; q < k; q++, c++) {
            X->cf[c] = i; X->ca[c] = (int)b[p]; X->cb[c] = (int)b[p + 1];
            for (int f = 0; f < 3; f++) { X->cl[f][c] = (float)b[p + 2 + f]; sum[f] += b[p + 2 + f]; }
            const int np = (int)b[p + 5]; p += 6;
            for (int j = 0; j < np; j++) {
                const uint32_t v = b[p + j]; const int f = (int)((v >> 8) & 3), t = (int)(v >> 10);
                const int at = cnt[f][t]++;
                X->pch[f][at] = c; X->ptf[f][at] = (uint8_t)(v & 255);
            }
            p += (uint32_t)np;
        }
        const int m = (int)b[p++];
        X->fs0[i] = s; X->fsn[i] = m;
        for (int q = 0; q < m; q++, s++) { X->sf[s] = i; X->sl[s] = (int)b[p]; X->sn[s] = (int)b[p + 1]; p += 2; }
    }
    X->shead = (int *)malloc(sizeof(int) * ((size_t)T + 1)); X->snext = (int *)malloc(sizeof(int) * (size_t)(ns + 1));
    for (int t = 0; t <= T; t++) X->shead[t] = -1;
    for (int q = ns - 1; q >= 0; q--) { X->snext[q] = X->shead[X->sn[q]]; X->shead[X->sn[q]] = q; }
    for (int f = 0; f < 3; f++) { X->avg[f] = nc ? sum[f] / nc : 1; if (X->avg[f] <= 0) X->avg[f] = 1; free(cnt[f]); }
    X->ready = 1;
}

/* ---- disco ---- */
static int fd_cache_path(FdIndex *X) {
    char home[700], d1[4400], d2[4400], fn[64];
    uint32_t h1 = fd_hash(X->root, (int)strlen(X->root)), h2 = fd_hash(X->root, (int)strlen(X->root) / 2 + 1);
    snprintf(fn, sizeof fn, "find-%08x%08x.idx", h1, h2);
    const char *over = getenv("KATANA_CACHE_DIR");
    if (over && *over) { MKDIR(over); snprintf(X->cache, sizeof X->cache, "%s/%s", over, fn); return 1; }
    agent_home(home, sizeof home);
    if (strcmp(X->root, home) != 0 && strcmp(X->root, "/") != 0) {
        snprintf(d1, sizeof d1, "%s/.katana", X->root);
        snprintf(d2, sizeof d2, "%s/.katana/cache", X->root);
        MKDIR(d1); MKDIR(d2);
        char gi[4500]; struct stat st;
        snprintf(gi, sizeof gi, "%s/.gitignore", d2);
        if (stat(d2, &st) == 0 && stat(gi, &st) != 0) { FILE *f = fopen(gi, "w"); if (f) { fputs("*\n", f); fclose(f); } }      /* o cache nao entra no git do projeto */
        if (access(d2, W_OK) == 0) { snprintf(X->cache, sizeof X->cache, "%s/%s", d2, fn); return 1; }
    }
    snprintf(d1, sizeof d1, "%s/.katana", home); MKDIR(d1);
    snprintf(d2, sizeof d2, "%s/.katana/cache", home); MKDIR(d2);
    snprintf(X->cache, sizeof X->cache, "%s/%s", d2, fn);
    return 1;
}
static int fd_save(FdIndex *X) {
    char tmp[4500];
    snprintf(tmp, sizeof tmp, "%s.%ld.tmp", X->cache, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    uint32_t h[4] = {FD_MAGIC, FD_VERSION, (uint32_t)strlen(X->root), (uint32_t)X->T.n};
    int ok = fwrite(h, sizeof h, 1, f) == 1 && fwrite(X->root, 1, h[2], f) == h[2];
    ok = ok && fwrite(X->T.len, sizeof(uint16_t), (size_t)X->T.n, f) == (size_t)X->T.n;
    ok = ok && fwrite(&X->T.pn, sizeof(size_t), 1, f) == 1 && fwrite(X->T.pool, 1, X->T.pn, f) == X->T.pn;
    uint32_t nf = (uint32_t)X->nf;
    ok = ok && fwrite(&nf, 4, 1, f) == 1;
    for (int i = 0; i < X->nf && ok; i++) {
        const FdFile *F = &X->f[i];
        const uint32_t rl = (uint32_t)strlen(F->rel);
        long long m[2] = {F->mt, F->size};
        ok = fwrite(&rl, 4, 1, f) == 1 && fwrite(F->rel, 1, rl, f) == rl && fwrite(m, sizeof m, 1, f) == 1 &&
             fwrite(&F->nb, 4, 1, f) == 1 && fwrite(F->blob, 4, F->nb, f) == F->nb;
    }
    ok = ok && fg_save(f, X);
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, X->cache) != 0) { remove(tmp); return 0; }
    return 1;
}
static void fd_free_files(FdIndex *X) {
    fg_free();
    for (int i = 0; i < X->nf; i++) { free(X->f[i].rel); free(X->f[i].blob); }
    free(X->f); X->f = NULL; X->nf = X->capf = 0;
    free(X->T.pool); free(X->T.off); free(X->T.len); free(X->T.ht); memset(&X->T, 0, sizeof X->T);
    X->ready = 0;
}
static int fd_load(FdIndex *X) {
    size_t n = 0;
    struct stat st;
    if (stat(X->cache, &st) != 0 || st.st_size < 32) return 0;
    char *b = fd_slurp(X->cache, &n, (size_t)st.st_size);
    if (!b) return 0;
    const char *p = b, *e = b + n;
#define FD_GET(dst, sz) do { if ((size_t)(e - p) < (size_t)(sz)) goto bad; memcpy((dst), p, (sz)); p += (sz); } while (0)
    uint32_t h[4];
    FD_GET(h, sizeof h);
    if (h[0] != FD_MAGIC || h[1] != FD_VERSION || h[2] != strlen(X->root) || (size_t)(e - p) < h[2] || memcmp(p, X->root, h[2]) != 0) goto bad;
    p += h[2];
    {
        FdTerms *T = &X->T;
        T->n = T->cap = (int)h[3];
        T->len = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)(T->cap + 1)); T->off = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(T->cap + 1));
        FD_GET(T->len, sizeof(uint16_t) * (size_t)T->n);
        FD_GET(&T->pn, sizeof(size_t));
        T->pcap = T->pn + 1; T->pool = (char *)malloc(T->pcap);
        FD_GET(T->pool, T->pn);
        size_t o = 0;
        for (int i = 0; i < T->n; i++) { T->off[i] = (uint32_t)o; o += T->len[i] + 1u; if (o > T->pn) goto bad; }
        T->hcap = 0; T->ht = NULL;
        { int hc = 1 << 16; while (hc < (T->n + 1) * 2) hc *= 2; T->hcap = hc / 2; fd_rehash(T); }
        uint32_t nf;
        FD_GET(&nf, 4);
        X->capf = (int)nf + 16; X->f = (FdFile *)calloc((size_t)X->capf, sizeof(FdFile));
        for (uint32_t i = 0; i < nf; i++) {
            FdFile *F = &X->f[X->nf];
            uint32_t rl; long long m[2];
            FD_GET(&rl, 4);
            if (rl > 4000 || (size_t)(e - p) < rl) goto bad;
            F->rel = (char *)malloc(rl + 1); memcpy(F->rel, p, rl); F->rel[rl] = 0; p += rl;
            X->nf++;
            FD_GET(m, sizeof m); F->mt = m[0]; F->size = m[1];
            FD_GET(&F->nb, 4);
            if ((size_t)(e - p) / 4 < F->nb) goto bad;
            F->blob = (uint32_t *)malloc(4 * (size_t)F->nb + 4); memcpy(F->blob, p, 4 * (size_t)F->nb); p += 4 * (size_t)F->nb;
            F->kind = fd_kind(F->rel);
        }
    }
    fg_load(p, (size_t)(e - p), X);
#undef FD_GET
    free(b);
    return 1;
bad:
    free(b);
    fd_free_files(X);
    return 0;
}

/* ---- lista de arquivos ---- */
static int fd_skip_rel(const char *rel) {
    static const char *const dirs[] = {"node_modules", "build", "dist", "vendor", ".katana", ".git", ".svn", ".hg", "__pycache__",
        ".venv", "venv", ".cache", "target", ".gradle", ".idea", NULL};
    const char *p = rel;
    for (;;) {
        const char *sl = strchr(p, '/');
        if (!sl) break;
        const size_t l = (size_t)(sl - p);
        for (int i = 0; dirs[i]; i++) if (strlen(dirs[i]) == l && !memcmp(dirs[i], p, l)) return 1;
        p = sl + 1;
    }
    return 0;
}
typedef struct { char *rel; long long mt, size; int kind; } FdLs;
static int fd_cmp_ls(const void *a, const void *b) { return strcmp(((const FdLs *)a)->rel, ((const FdLs *)b)->rel); }
static int fd_cmp_rel(const void *a, const void *b) { return strcmp(((const FdFile *)a)->rel, ((const FdFile *)b)->rel); }
static int fd_max_files(void) { const char *e = getenv("KATANA_FIND_MAX_FILES"); const int v = e ? atoi(e) : 0; return v > 0 ? v : 20000; }

/* atualiza o indice da pasta root: le so o que mudou; devolve quantos arquivos foram (re)lidos */
static int g_fd_verbose;
static int fd_update(FdIndex *X, const char *root, char *err, size_t ecap) {
    const double t0 = fd_ms();
    if (strcmp(X->root, root) != 0) {
        fd_free_files(X);
        snprintf(X->root, sizeof X->root, "%s", root);
        fd_cache_path(X);
        fd_load(X);
    } else if (!X->cache[0]) fd_cache_path(X);
    const double t1 = fd_ms();
    CtFiles fl; memset(&fl, 0, sizeof fl);
    CtGrepFilter flt = {NULL, NULL};
    ct_grep_collect(&fl, root, root, &flt);
    FdLs *ls = (FdLs *)malloc(sizeof(FdLs) * (size_t)(fl.n + 1));
    int nl = 0;
    const size_t rl = strlen(root);
    const int maxf = fd_max_files();
    X->truncated = 0;
    for (int i = 0; i < fl.n; i++) {
        const char *abs = fl.v[i].path;
        if (strncmp(abs, root, rl) != 0 || abs[rl] != '/') continue;
        const char *rel = abs + rl + 1;
        if (fd_skip_rel(rel)) continue;
        const int k = fd_kind(rel);
        if (k < 0) continue;
        struct stat st;
        if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > FD_MAXBYTES) continue;
        if (nl >= maxf) { X->truncated++; continue; }
        ls[nl].rel = strdup(rel); ls[nl].mt = (long long)st.st_mtime; ls[nl].size = (long long)st.st_size; ls[nl].kind = k; nl++;
    }
    ct_files_free(&fl);
    X->listed = nl;
    qsort(ls, (size_t)nl, sizeof(FdLs), fd_cmp_ls);
    qsort(X->f, (size_t)X->nf, sizeof(FdFile), fd_cmp_rel);
    /* junta: arquivo igual fica; mudado ou novo e relido; sumido sai */
    FdFile *nf = (FdFile *)calloc((size_t)nl + 1, sizeof(FdFile));
    int i = 0, j = 0, changed = 0, removed = 0;
    for (; j < nl; j++) {
        while (i < X->nf && strcmp(X->f[i].rel, ls[j].rel) < 0) { free(X->f[i].rel); free(X->f[i].blob); i++; removed++; }
        FdFile *F = &nf[j];
        if (i < X->nf && strcmp(X->f[i].rel, ls[j].rel) == 0) {
            *F = X->f[i]; i++;
            free(ls[j].rel);
            if (F->mt == ls[j].mt && F->size == ls[j].size && F->blob) continue;
        } else { F->rel = ls[j].rel; F->blob = NULL; }
        F->mt = ls[j].mt; F->size = ls[j].size; F->kind = ls[j].kind;
        fd_build_file(X, F);
        changed++;
    }
    for (; i < X->nf; i++) { free(X->f[i].rel); free(X->f[i].blob); removed++; }
    free(X->f); free(ls);
    X->f = nf; X->nf = nl; X->capf = nl + 1;
    const double t2 = fd_ms();
    if (changed || removed) fg_free();
    if (changed || removed || !X->ready) fd_invert(X);
    const double t3 = fd_ms();
    if (changed || removed) fd_save(X);
    if (g_fd_verbose) fprintf(stderr, "find: load %.0f ms, list %.0f ms (%d files, %d read again, %d gone), invert %.0f ms, save %.0f ms\n",
                              t1 - t0, t2 - t1, nl, changed, removed, t3 - t2, fd_ms() - t3);
    (void)err; (void)ecap;
    return changed;
}

/* ---- parametros do ranking (KATANA_FIND_TUNE="wp=3,ws=2,..."; so para pesquisa) ---- */
typedef struct { double wp, ws, wb, k1, b, cover, agg, idw, symb, docpen; } FdTune;
static FdTune fd_tune(void) {
    FdTune t = {3, 1, 1, 1.2, 0.75, 0.1, 0.3, 1.0, 0.0, 1.0};      /* medido: ws=1 e cover=0.1 >= o bench em Python nos 3 repositorios */
    const char *e = getenv("KATANA_FIND_TUNE");
    while (e && *e) {
        char k[16]; double v; int used = 0;
        if (sscanf(e, "%15[a-z0-9]=%lf%n", k, &v, &used) == 2) {
            if (!strcmp(k, "wp")) t.wp = v; else if (!strcmp(k, "ws")) t.ws = v; else if (!strcmp(k, "wb")) t.wb = v;
            else if (!strcmp(k, "k1")) t.k1 = v; else if (!strcmp(k, "b")) t.b = v; else if (!strcmp(k, "cover")) t.cover = v;
            else if (!strcmp(k, "agg")) t.agg = v; else if (!strcmp(k, "idw")) t.idw = v; else if (!strcmp(k, "symb")) t.symb = v;
            else if (!strcmp(k, "docpen")) t.docpen = v;
            e += used;
        }
        const char *c = strchr(e, ',');
        if (!c) break;
        e = c + 1;
    }
    return t;
}

/* termo inteiro de um identificador (o primeiro que a tokenizacao produz), -1 se nao esta no indice */
static int fd_key(FdIndex *X, const char *name, int l) {
    FdIv v = {0}; FdTok k = {&X->T, 0, &v};
    char tmp[256]; if (l > 250) l = 250;
    memcpy(tmp, name, (size_t)l); tmp[l] = 0;
    /* so o primeiro token: o identificador inteiro (ou a unica parte) */
    const int r = fd_idlen(tmp, tmp + l, FK_OTHER) == l ? (fd_tokens(&k, tmp, (size_t)l), v.n ? (int)v.v[0] : -1) : -1;
    free(v.v);
    return r;
}
/* linhas de um arquivo (texto inteiro, para trechos e usos) */
typedef struct { char *s; size_t n; FdIv ls; } FdText;
static int fd_text(FdIndex *X, int fi, FdText *t) {
    char abs[8400]; snprintf(abs, sizeof abs, "%s/%s", X->root, X->f[fi].rel);
    memset(t, 0, sizeof *t);
    t->s = fd_slurp(abs, &t->n, FD_MAXBYTES);
    if (!t->s) return 0;
    for (size_t i = 0; i < t->n; ) { fd_iv_push(&t->ls, (uint32_t)i); const char *nl = memchr(t->s + i, '\n', t->n - i); i = nl ? (size_t)(nl - t->s) + 1 : t->n; }
    return 1;
}
static void fd_text_free(FdText *t) { free(t->s); free(t->ls.v); }
static const char *fd_line(const FdText *t, int ln, int *len) {    /* ln a partir de 1 */
    if (ln < 1 || ln > t->ls.n) { *len = 0; return ""; }
    const size_t a = t->ls.v[ln - 1], b = ln < t->ls.n ? t->ls.v[ln] : t->n;
    size_t e = b; while (e > a && (t->s[e - 1] == '\n' || t->s[e - 1] == '\r')) e--;
    *len = (int)(e - a);
    return t->s + a;
}
/* texto da linha, sem o recuo e cortado em max (cuida de nao partir um caractere UTF-8) */
static void fd_put_line(CtSb *sb, const char *s, int l, int max) {
    while (l > 0 && (*s == ' ' || *s == '\t')) { s++; l--; }
    if (l > max) { int c = max; while (c > 0 && ((unsigned char)s[c] & 0xC0) == 0x80) c--; ct_sb_add(sb, s, (size_t)c); ct_sb_puts(sb, "..."); }
    else ct_sb_add(sb, s, (size_t)l);
}
/* identificador s aparece como palavra inteira na linha? */
static int fd_has_word(const char *line, int l, const char *w, int wl, int icase) {
    for (int i = 0; i + wl <= l; i++) {
        if ((icase ? strncasecmp(line + i, w, (size_t)wl) : memcmp(line + i, w, (size_t)wl)) != 0) continue;
        if (i > 0 && fd_isid((unsigned char)line[i - 1])) continue;
        if (i + wl < l && fd_isid((unsigned char)line[i + wl])) continue;
        return 1;
    }
    return 0;
}
/* simbolo definido em volta da linha ln do arquivo fi (o ultimo definido antes dela), -1 se nenhum */
static int fd_enclosing(FdIndex *X, int fi, int ln) {
    int best = -1;
    for (int s = X->fs0[fi]; s < X->fs0[fi] + X->fsn[fi]; s++) if (X->sl[s] <= ln) best = s; else break;
    return best;
}
static int fd_in_path(const char *rel, const char *pre) {
    if (!pre || !*pre) return 1;
    const size_t l = strlen(pre);
    return strncmp(rel, pre, l) == 0 && (rel[l] == '/' || rel[l] == 0);
}
static int fd_is_doc(int kind) { return kind == FK_MD || kind == FK_OTHER; }

/* ---- busca ---- */
static int fd_cmp_hit(const void *a, const void *b) {
    const double x = ((const FdHit *)a)->score, y = ((const FdHit *)b)->score;
    return x < y ? 1 : x > y ? -1 : ((const FdHit *)a)->file - ((const FdHit *)b)->file;
}
/* ranking: devolve ate k arquivos em hits (ordem decrescente) */
static int fd_rank(FdIndex *X, const char *query, const char *pre, FdHit *hits, int k, uint32_t *qid, int *nq, int *esrc) {
    const FdTune tu = fd_tune();
    FdIv v = {0}; FdTok tk = {&X->T, 0, &v};
    /* termos da pergunta, sem repetir; os de identificadores (snake_case, camelCase, com ponto ou barra) valem idw */
    double qw[64]; int n = 0;
    {
        const size_t ql = strlen(query);
        char *f = (char *)malloc(ql + 1);
        const size_t m = fd_fold(query, ql, f);
        for (size_t i = 0; i < m; ) {
            if (!(isalpha((unsigned char)f[i]) || f[i] == '_' || isdigit((unsigned char)f[i]))) { i++; continue; }
            size_t j = i; int idl = 0;
            while (j < m && (isalnum((unsigned char)f[j]) || f[j] == '_' || f[j] == '.' || f[j] == '/' || f[j] == '-')) {
                if (f[j] == '_' || ((f[j] == '.' || f[j] == '/') && j + 1 < m && isalnum((unsigned char)f[j + 1])) ||
                    (j > i && islower((unsigned char)f[j - 1]) && isupper((unsigned char)f[j]))) idl = 1;
                j++;
            }
            v.n = 0;
            fd_tokens(&tk, f + i, j - i);
            for (int q = 0; q < v.n; q++) {
                int dup = -1; for (int r = 0; r < n; r++) if (qid[r] == v.v[q]) dup = r;
                const double w = idl ? tu.idw : 1.0;
                if (dup >= 0) { if (w > qw[dup]) qw[dup] = w; continue; }
                if (n < 64) { qid[n] = v.v[q]; qw[n] = w; n++; }
            }
            i = j;
        }
        free(f);
    }
    free(v.v);
    n = fg_drop_request(X, qid, qw, n);        /* palavras de pedido fora (so com a ativacao ligada) */
    *nq = n;
    if (!n || !X->nc) return 0;
    float *sc = (float *)calloc((size_t)X->nc, sizeof(float));
    uint8_t *cov = (uint8_t *)calloc((size_t)X->nc, 1), *seen = (uint8_t *)calloc((size_t)X->nc, 1);
    int *mark = (int *)malloc(sizeof(int) * (size_t)X->nc);
    for (int c = 0; c < X->nc; c++) mark[c] = -1;
    int *touched = (int *)malloc(sizeof(int) * (size_t)X->nc); int nt = 0;
    const double wf[3] = {tu.wp, tu.ws, tu.wb};
    for (int q = 0; q < n; q++) {
        const int t = (int)qid[q];
        for (int f = 0; f < 3; f++) {
            const int a = X->poff[f][t], b = X->poff[f][t + 1], df = b - a;
            if (!df) continue;
            const double idf = log(1 + (X->nc - df + 0.5) / (df + 0.5)), w = wf[f] * qw[q] * idf;
            const double k1 = tu.k1, kb = tu.b, avg = X->avg[f];
            for (int i = a; i < b; i++) {
                const int c = X->pch[f][i]; const double tf = X->ptf[f][i];
                if (!seen[c]) { seen[c] = 1; touched[nt++] = c; }
                sc[c] += (float)(w * tf * (k1 + 1) / (tf + k1 * (1 - kb + kb * X->cl[f][c] / avg)));
                if (f >= 1 && mark[c] != q) { mark[c] = q; cov[c]++; }
            }
        }
    }
    /* bonus: trecho define um simbolo cujo termo inteiro e termo da pergunta */
    if (tu.symb > 0) {
        for (int s = 0; s < X->ns; s++) {
            const char *nm = fd_tstr(&X->T, X->sn[s]);
            const int key = fd_key(X, nm, (int)strlen(nm));
            if (key < 0) continue;
            int hit = 0; for (int q = 0; q < n; q++) if ((int)qid[q] == key) hit = 1;
            if (!hit) continue;
            const int fi = X->sf[s];
            for (int c = X->fc0[fi]; c < X->fc0[fi] + X->fcn[fi]; c++) if (X->ca[c] <= X->sl[s] && X->sl[s] <= X->cb[c] && sc[c] > 0) sc[c] *= (float)(1 + tu.symb);
        }
    }
    /* arquivo = melhor trecho + agg * segundo */
    double *best = (double *)calloc((size_t)X->nf, sizeof(double)), *second = (double *)calloc((size_t)X->nf, sizeof(double));
    int *bc = (int *)malloc(sizeof(int) * (size_t)X->nf);
    for (int i = 0; i < nt; i++) {
        const int c = touched[i], fi = X->cf[c];
        double s = sc[c] * (1 + tu.cover * cov[c]);
        if (s <= 0) continue;
        if (s > best[fi]) { second[fi] = best[fi]; best[fi] = s; bc[fi] = c; }
        else if (s > second[fi]) second[fi] = s;
    }
    int idlike = 0; for (int q = 0; q < n; q++) if (qw[q] > 1.0) idlike = 1;
    FdHit *all = (FdHit *)malloc(sizeof(FdHit) * (size_t)(X->nf + 1)); int na = 0;
    for (int fi = 0; fi < X->nf; fi++) {
        if (best[fi] <= 0 || !fd_in_path(X->f[fi].rel, pre)) continue;
        double s = best[fi] + tu.agg * second[fi];
        if (idlike && tu.docpen != 1.0 && fd_is_doc(X->f[fi].kind)) s *= tu.docpen;
        all[na].file = fi; all[na].chunk = bc[fi]; all[na].score = s; na++;
    }
    qsort(all, (size_t)na, sizeof(FdHit), fd_cmp_hit);
    int r;
    for (int i = 0; i < k; i++) esrc[i] = -1;
    if (na && fg_act_on() && fg_ensure(X)) r = fg_activate(X, all, na, pre, k, hits, esrc);      /* ativacao esparsa sobre o grafo de arquivos */
    else { r = na < k ? na : k; memcpy(hits, all, sizeof(FdHit) * (size_t)r); }
    free(all); free(best); free(second); free(bc); free(sc); free(cov); free(seen); free(mark); free(touched);
    return r;
}

/* outros arquivos que citam os simbolos definidos em [a,b] do arquivo fi; escreve "also referenced in: ...".
   Se alguma linha mostrada (pref) define um simbolo, so esses contam; senao todos os do trecho. */
static void fd_also(FdIndex *X, int fi, int a, int b, const int *pref, int npref, CtSb *sb) {
    int files[64], cnt[64], nfl = 0;
    if (X->f[fi].kind == FK_MD) return;
    int onpref = 0;
    for (int s = X->fs0[fi]; s < X->fs0[fi] + X->fsn[fi]; s++) for (int i = 0; i < npref; i++) if (pref[i] && pref[i] == X->sl[s]) onpref = 1;
    for (int s = X->fs0[fi]; s < X->fs0[fi] + X->fsn[fi]; s++) {
        if (X->sl[s] < a || X->sl[s] > b) continue;
        int isp = 0; for (int i = 0; i < npref; i++) if (pref[i] == X->sl[s]) isp = 1;
        if (onpref && !isp) continue;
        const char *nm = fd_tstr(&X->T, X->sn[s]);
        const int l = (int)strlen(nm);
        if (l < 4) continue;
        const int key = fd_key(X, nm, l);
        if (key < 0) continue;
        const int multi = strchr(fd_tstr(&X->T, key), '_') != NULL;
        const int pa = X->poff[2][key], pb = X->poff[2][key + 1];
        if (pb - pa > 2000) continue;                  /* nome comum demais para dizer algo */
        int last = -1, checked = 0;
        for (int i = pa; i < pb; i++) {
            const int of = X->cf[X->pch[2][i]];
            if (of == fi || of == last) continue;
            last = of;
            int at = -1; for (int q = 0; q < nfl; q++) if (files[q] == of) at = q;
            if (at >= 0) { cnt[at]++; continue; }
            if (nfl >= 64) break;
            if (!multi) {                              /* nome de uma parte so: confere a palavra exata no texto */
                if (++checked > 12) break;
                FdText t; if (!fd_text(X, of, &t)) continue;
                const int ok = fd_has_word(t.s, (int)t.n, nm, l, 0);
                fd_text_free(&t);
                if (!ok) continue;
            }
            files[nfl] = of; cnt[nfl] = 1 + 4 * !fd_is_doc(X->f[of].kind); nfl++;
        }
    }
    if (!nfl) return;
    /* mais simbolos citados primeiro; codigo antes de documentacao */
    for (int i = 0; i < nfl; i++) for (int j = i + 1; j < nfl; j++) if (cnt[j] > cnt[i]) { int t = cnt[i]; cnt[i] = cnt[j]; cnt[j] = t; t = files[i]; files[i] = files[j]; files[j] = t; }
    ct_sb_puts(sb, "   also referenced in: ");
    const int show = nfl < 3 ? nfl : 3;
    for (int i = 0; i < show; i++) { if (i) ct_sb_puts(sb, ", "); ct_sb_puts(sb, X->f[files[i]].rel); }
    if (nfl > show) { char m[32]; snprintf(m, sizeof m, " (+%d more)", nfl - show); ct_sb_puts(sb, m); }
    ct_sb_puts(sb, "\n");
}

/* nomes usados nas linhas do trecho que estao definidos em outro lugar do indice (a aresta de saida do grafo):
   "calls: nome (arquivo:linha)". Nota de cada nome: estar numa linha mostrada, perto delas, definido em outro
   arquivo, ter varias partes. Nomes definidos em mais de 3 lugares, curtos ou so em comentario ficam de fora. */
static void fd_calls(FdIndex *X, int fi, int a, int b, const FdText *t, const int *pref, int npref, CtSb *sb) {
    int got[32], gs[32], ng = 0;
    for (int ln = a; ln <= b; ln++) {
        int dist = 1000; for (int i = 0; i < npref; i++) if (pref[i] && abs(pref[i] - ln) < dist) dist = abs(pref[i] - ln);
        int ll = 0; const char *s = fd_line(t, ln, &ll);
        { const char *z = fd_ws(s, s + ll); if (z < s + ll && (*z == '#' || *z == '*' || (z[0] == '/' && z + 1 < s + ll && (z[1] == '/' || z[1] == '*')) || (z[0] == '-' && z + 1 < s + ll && z[1] == '-'))) continue; }
        for (int i = 0; i < ll; ) {
            if (!(isalpha((unsigned char)s[i]) || s[i] == '_') || (i > 0 && fd_isid((unsigned char)s[i - 1]))) { i++; continue; }
            const int l = fd_idlen(s + i, s + ll, FK_OTHER);
            if (l < 5) { i += l ? l : 1; continue; }
            const int id = fd_term(&X->T, s + i, l, 0);
            int multi = 0; for (int z = 1; z < l; z++) if (s[i + z] == '_' || (islower((unsigned char)s[i + z - 1]) && isupper((unsigned char)s[i + z]))) multi = 1;
            const char nx = i + l < ll ? s[i + l] : 0;
            i += l;
            if ((!multi && nx != '(') || id < 0 || X->shead[id] < 0) continue;
            int nd = 0, out = -1, self = 0;
            for (int q = X->shead[id]; q >= 0; q = X->snext[q]) {
                if (X->f[X->sf[q]].kind == FK_MD) continue;
                nd++;
                if (X->sf[q] == fi && X->sl[q] >= a && X->sl[q] <= b) self = 1;
                if (out < 0 || (X->sf[out] == fi && X->sf[q] != fi)) out = q;
            }
            if (!nd || nd > 3 || self || out < 0) continue;
            const int sc = (dist == 0 ? 100 : dist <= 3 ? 50 - dist : 20 - (dist > 20 ? 20 : dist)) + (X->sf[out] != fi ? 15 : 0) + (multi ? 10 : 0) + (nx == '(' ? 5 : 0);
            int at = -1; for (int r = 0; r < ng; r++) if (X->sn[got[r]] == id) at = r;
            if (at >= 0) { if (sc > gs[at]) gs[at] = sc; continue; }
            if (ng < 32) { got[ng] = out; gs[ng] = sc; ng++; }
        }
    }
    if (!ng) return;
    for (int i = 0; i < ng; i++) for (int j = i + 1; j < ng; j++) if (gs[j] > gs[i]) { int x = gs[i]; gs[i] = gs[j]; gs[j] = x; x = got[i]; got[i] = got[j]; got[j] = x; }
    ct_sb_puts(sb, "   calls: ");
    for (int i = 0; i < ng && i < 4; i++) {
        char m[600];
        const int q = got[i];
        if (X->sf[q] == fi) snprintf(m, sizeof m, "%s%s (line %d)", i ? ", " : "", fd_tstr(&X->T, X->sn[q]), X->sl[q]);
        else snprintf(m, sizeof m, "%s%s (%s:%d)", i ? ", " : "", fd_tstr(&X->T, X->sn[q]), X->f[X->sf[q]].rel, X->sl[q]);
        ct_sb_puts(sb, m);
    }
    ct_sb_puts(sb, "\n");
}

/* ---- modo refs ---- */
static void fd_refs(FdIndex *X, const char *query, const char *pre, int limit, CtSb *sb) {
    const char *p = query; while (*p && !(isalpha((unsigned char)*p) || *p == '_' || *p == '$')) p++;
    int l = fd_idlen(p, p + strlen(p), FK_PS);
    while (l > 0 && p[l - 1] == '-') l--;
    if (!l) { ct_sb_puts(sb, "error: mode=refs needs a symbol name in `query`, e.g. {\"query\": \"listSessions\", \"mode\": \"refs\"}"); return; }
    char name[256]; if (l > 250) l = 250; memcpy(name, p, (size_t)l); name[l] = 0;
    /* definicoes: nome exato; senao sem distinguir maiusculas */
    int defs[16], nd = 0, icase = 0;
    for (int pass = 0; pass < 2 && !nd; pass++)
        for (int s = 0; s < X->ns && nd < 16; s++) {
            const char *nm = fd_tstr(&X->T, X->sn[s]);
            if (X->f[X->sf[s]].kind == FK_MD || !fd_in_path(X->f[X->sf[s]].rel, pre)) continue;
            if (pass ? strcasecmp(nm, name) == 0 : strcmp(nm, name) == 0) { defs[nd++] = s; icase = pass; }
        }
    char hd[400];
    if (nd) {
        snprintf(hd, sizeof hd, "Defined (%d):\n", nd); ct_sb_puts(sb, hd);
        for (int i = 0; i < nd && i < 6; i++) {
            const int s = defs[i], fi = X->sf[s];
            FdText t; int ll = 0;
            snprintf(hd, sizeof hd, "  %s:%d  ", X->f[fi].rel, X->sl[s]); ct_sb_puts(sb, hd);
            if (fd_text(X, fi, &t)) { const char *ln = fd_line(&t, X->sl[s], &ll); fd_put_line(sb, ln, ll, 110); fd_text_free(&t); }
            ct_sb_puts(sb, "\n");
        }
    } else { snprintf(hd, sizeof hd, "No definition of %s found in the index (it may come from a library, or be spelled differently).\n", name); ct_sb_puts(sb, hd); }
    /* usos: arquivos com o termo inteiro no corpo, conferidos palavra a palavra */
    const int key = fd_key(X, name, l);
    if (key < 0) { ct_sb_puts(sb, "No uses found.\n"); return; }
    typedef struct { int fi, n, first; int lines[4]; } Use;
    Use *u = (Use *)calloc((size_t)X->nf + 1, sizeof(Use)); int nu = 0, total = 0;
    int last = -1;
    for (int i = X->poff[2][key]; i < X->poff[2][key + 1]; i++) {
        const int fi = X->cf[X->pch[2][i]];
        if (fi == last) continue;
        last = fi;
        if (!fd_in_path(X->f[fi].rel, pre)) continue;
        FdText t; if (!fd_text(X, fi, &t)) continue;
        Use *w = &u[nu]; w->fi = fi; w->n = 0;
        for (int ln = 1; ln <= t.ls.n; ln++) {
            int ll = 0; const char *s = fd_line(&t, ln, &ll);
            if (!fd_has_word(s, ll, name, l, icase)) continue;
            int isdef = 0; for (int d = 0; d < nd; d++) if (X->sf[defs[d]] == fi && X->sl[defs[d]] == ln) isdef = 1;
            if (isdef) continue;
            if (w->n < 4) w->lines[w->n] = ln;
            w->n++;
        }
        fd_text_free(&t);
        if (w->n) { total += w->n; nu++; }
    }
    /* arquivo da definicao primeiro, depois mais usos; documentacao por ultimo */
    for (int i = 0; i < nu; i++) for (int j = i + 1; j < nu; j++) {
        int ki = 0, kj = 0;
        for (int d = 0; d < nd; d++) { if (X->sf[defs[d]] == u[i].fi) ki = 1; if (X->sf[defs[d]] == u[j].fi) kj = 1; }
        const int di = fd_is_doc(X->f[u[i].fi].kind), dj = fd_is_doc(X->f[u[j].fi].kind);
        const int sw = di != dj ? di > dj : ki != kj ? kj > ki : u[j].n > u[i].n;
        if (sw) { Use t = u[i]; u[i] = u[j]; u[j] = t; }
    }
    if (!nu) { ct_sb_puts(sb, "No uses found outside the definition.\n"); free(u); return; }
    snprintf(hd, sizeof hd, "Used %d times in %d files%s:\n", total, nu, nu > limit ? " (top files shown)" : ""); ct_sb_puts(sb, hd);
    const size_t budget = 1500;
    for (int i = 0; i < nu && i < limit; i++) {
        FdText t; if (!fd_text(X, u[i].fi, &t)) continue;
        snprintf(hd, sizeof hd, "%s (%d):\n", X->f[u[i].fi].rel, u[i].n); ct_sb_puts(sb, hd);
        const int show = sb->n > budget ? 1 : u[i].n < 3 ? u[i].n : 3;
        for (int q = 0; q < show && q < 4; q++) {
            const int ln = u[i].lines[q], en = fd_enclosing(X, u[i].fi, ln);
            int ll = 0; const char *s = fd_line(&t, ln, &ll);
            if (en >= 0 && X->f[u[i].fi].kind != FK_MD) snprintf(hd, sizeof hd, "  %d (in %s): ", ln, fd_tstr(&X->T, X->sn[en]));
            else snprintf(hd, sizeof hd, "  %d: ", ln);
            ct_sb_puts(sb, hd); fd_put_line(sb, s, ll, 100); ct_sb_puts(sb, "\n");
        }
        if (u[i].n > show) { snprintf(hd, sizeof hd, "  (+%d more lines)\n", u[i].n - show); ct_sb_puts(sb, hd); }
        fd_text_free(&t);
    }
    if (nu > limit) {
        ct_sb_puts(sb, "Also in: ");
        for (int i = limit; i < nu && i < limit + 6; i++) { if (i > limit) ct_sb_puts(sb, ", "); ct_sb_puts(sb, X->f[u[i].fi].rel); }
        if (nu > limit + 6) { snprintf(hd, sizeof hd, " (+%d more)", nu - limit - 6); ct_sb_puts(sb, hd); }
        ct_sb_puts(sb, "\n");
    }
    free(u);
}

/* ---- modo search: saida ---- */
static void fd_show(FdIndex *X, const FdHit *h, int rank, const uint32_t *qid, int nq, size_t budget, const char *why, CtSb *sb) {
    const int fi = h->file, a = X->ca[h->chunk], b = X->cb[h->chunk];
    char hd[600];
    snprintf(hd, sizeof hd, "%d. %s:%d-%d", rank, X->f[fi].rel, a, b);
    ct_sb_puts(sb, hd);
    /* simbolos definidos no trecho: os que casam com a pergunta primeiro */
    int syms[32], ns = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int s = X->fs0[fi]; s < X->fs0[fi] + X->fsn[fi] && ns < 32; s++) {
            if (X->sl[s] < a || X->sl[s] > b) continue;
            const char *nm = fd_tstr(&X->T, X->sn[s]);
            const int key = fd_key(X, nm, (int)strlen(nm));
            int m = 0; for (int q = 0; q < nq; q++) if ((int)qid[q] == key) m = 1;
            if (m == !pass) { int dup = 0; for (int r = 0; r < ns; r++) if (X->sn[syms[r]] == X->sn[s]) dup = 1; if (!dup) syms[ns++] = s; }
        }
    if (ns) {
        ct_sb_puts(sb, X->f[fi].kind == FK_MD ? "  sections: " : "  defines: ");
        for (int i = 0; i < ns && i < 4; i++) { if (i) ct_sb_puts(sb, ", "); fd_put_line(sb, fd_tstr(&X->T, X->sn[syms[i]]), (int)strlen(fd_tstr(&X->T, X->sn[syms[i]])), 50); }
        if (ns > 4) { snprintf(hd, sizeof hd, " (+%d)", ns - 4); ct_sb_puts(sb, hd); }
    }
    ct_sb_puts(sb, "\n");
    if (why && *why) { ct_sb_puts(sb, "   why: "); ct_sb_puts(sb, why); ct_sb_puts(sb, "\n"); }
    /* linhas com mais termos distintos da pergunta */
    FdText t;
    if (fd_text(X, fi, &t)) {
        int best[4] = {0, 0, 0, 0}, bs[4] = {0, 0, 0, 0};
        const int want = budget < 160 ? 1 : budget < 400 ? 2 : 3;
        FdIv v = {0}; FdTok tk = {&X->T, 0, &v};
        char *fold = (char *)malloc(4096);
        for (int ln = a; ln <= b && ln <= t.ls.n; ln++) {
            int ll = 0; const char *s = fd_line(&t, ln, &ll);
            if (ll > 4000) ll = 4000;
            v.n = 0;
            fd_tokens(&tk, fold, fd_fold(s, (size_t)ll, fold));
            int m = 0;
            for (int q = 0; q < nq; q++) for (int r = 0; r < v.n; r++) if (v.v[r] == qid[q]) { m += 2; break; }
            for (int s2 = X->fs0[fi]; s2 < X->fs0[fi] + X->fsn[fi]; s2++) if (X->sl[s2] == ln && m) m++;
            if (!m) continue;
            for (int k = 0; k < want; k++) if (m > bs[k]) { for (int z = want - 1; z > k; z--) { bs[z] = bs[z - 1]; best[z] = best[z - 1]; } bs[k] = m; best[k] = ln; break; }
        }
        free(fold); free(v.v);
        const int top = best[0];       /* a linha que mais casa: nos dois primeiros resultados vira um trecho de codigo, para o modelo nao precisar de um Read */
        /* em ordem de linha */
        for (int i = 0; i < want; i++) for (int j = i + 1; j < want; j++) if (best[j] && (!best[i] || best[j] < best[i])) { int x = best[i]; best[i] = best[j]; best[j] = x; }
        int w0 = 0, w1 = 0;       /* trecho em torno da melhor linha (so os 2 primeiros resultados, de codigo) */
        if (top && rank <= 2 && budget >= 200 && X->f[fi].kind != FK_MD && X->f[fi].kind != FK_OTHER) {
            w0 = top - 3 < a ? a : top - 3; w1 = top + 8 > b ? b : top + 8;
            if (w1 > t.ls.n) w1 = t.ls.n;
            if (w1 - w0 < 2) w0 = w1 = 0;
        }
        for (int i = 0; i < want; i++) {
            if (!best[i] || (w0 && best[i] >= w0 && best[i] <= w1)) continue;      /* ja esta no trecho */
            int ll = 0; const char *s = fd_line(&t, best[i], &ll);
            snprintf(hd, sizeof hd, "   %d: ", best[i]); ct_sb_puts(sb, hd);
            fd_put_line(sb, s, ll, 100); ct_sb_puts(sb, "\n");
        }
        if (w0) for (int ln = w0; ln <= w1; ln++) {
            int ll = 0; const char *s = fd_line(&t, ln, &ll);
            snprintf(hd, sizeof hd, "   %d%s ", ln, ln == top ? ">" : ":"); ct_sb_puts(sb, hd);
            fd_put_line(sb, s, ll, 110); ct_sb_puts(sb, "\n");
        }
        if (budget >= 200 && X->f[fi].kind != FK_MD && X->f[fi].kind != FK_OTHER) fd_calls(X, fi, a, b, &t, best, want, sb);
        if (budget >= 200) fd_also(X, fi, a, b, best, want, sb);
        fd_text_free(&t);
    }
}

/* ---- camada de decisao do Find (KATANA_CODER=off|exact|full) ----
 * O katana decide o canal pela pergunta; o modelo nao escolhe:
 *   texto entre aspas            -> verificacao literal pelo motor do coder (acha ou diz NOT FOUND)
 *   identificador / nome de arquivo (uma palavra com _ . / ou camelCase) -> busca exata pelo motor do coder
 *   descricao em linguagem natural -> o Find de sempre (BM25 + grafo); com full, o motor do coder entra como segunda camada
 * O grafo do Find nunca e substituido: a saida dele vem inteira, o coder acrescenta linhas curtas (evidencia, NOT FOUND). */
enum { FQ_NATURAL, FQ_IDENT, FQ_QUOTED };
static int fd_coder_level(void) {          /* 0 off, 1 names (so nome e texto literal), 2 full (padrao: tambem a descricao, com os conceitos) */
    const char *e = getenv("KATANA_CODER");
    if (!e || !*e) return 2;
    if (!strcmp(e, "off") || !strcmp(e, "0")) return 0;
    if (!strcmp(e, "exact") || !strcmp(e, "names")) return 1;
    return 2;
}
static int fd_query_kind(const char *q) {
    const char *a = strchr(q, '"');
    if (a) { const char *b = strchr(a + 1, '"'); if (b && b - a - 1 >= 3) return FQ_QUOTED; }
    while (*q == ' ' || *q == '\t') q++;
    size_t n = strlen(q); while (n && (q[n - 1] == ' ' || q[n - 1] == '\t' || q[n - 1] == '\n')) n--;
    if (n < 3 || n > 120) return FQ_NATURAL;
    int codey = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)q[i];
        if (!(isalnum(c) || c == '_' || c == '.' || c == '/' || c == '-' || c == '$' || c == ':')) return FQ_NATURAL;
        if (c == '_' || c == '.' || c == '/') codey = 1;
        if (i && islower((unsigned char)q[i - 1]) && isupper(c)) codey = 1;
    }
    return codey ? FQ_IDENT : FQ_NATURAL;
}
/* o indice do coder nao le .gitignore: so entram os arquivos que o indice do Find tambem lista (mesmo conjunto de arquivos) */
static int fd_listed(FdIndex *X, const char *rel) {
    FdFile key; memset(&key, 0, sizeof key); key.rel = (char *)rel;
    return bsearch(&key, X->f, (size_t)X->nf, sizeof(FdFile), fd_cmp_rel) != NULL;
}
static int fd_coder_ok(FdIndex *X, const CoderHit *h, const char *pre) {
    const size_t pl = strlen(pre);
    return (!pl || strncmp(h->path, pre, pl) == 0) && fd_listed(X, h->path);
}
static void fd_hit_line(CtSb *sb, const CoderHit *h, const char *tag) {
    char m[760];
    if (h->nev) {
        const char *t = h->text[0]; while (*t == ' ' || *t == '\t') t++;
        snprintf(m, sizeof m, "  [%s] %s:%d: %.110s\n", tag, h->path, h->line[0], t);
    } else snprintf(m, sizeof m, "  [%s] %s\n", tag, h->path);
    ct_sb_puts(sb, m);
}
/* primeira linha do arquivo que contem o texto (sem diferenciar maiusculas): a evidencia que o motor devolve pode ser uma linha que casa so uma parte */
static int fd_line_of(FdIndex *X, const char *rel, const char *needle, int *ln, char *out, size_t cap) {
    char abs[8400]; snprintf(abs, sizeof abs, "%s/%s", X->root, rel);
    size_t n = 0; char *b = fd_slurp(abs, &n, FD_MAXBYTES); if (!b) return 0;
    const size_t nl = strlen(needle); int found = 0, line = 1;
    for (size_t i = 0; i < n && nl; ) {
        size_t e = i; while (e < n && b[e] != '\n') e++;
        if (e - i >= nl) for (size_t k = i; k + nl <= e; k++) if (strncasecmp(b + k, needle, nl) == 0) { found = 1; break; }
        if (found) {
            const char *t = b + i; size_t l = e - i; while (l && (*t == ' ' || *t == '\t')) { t++; l--; }
            snprintf(out, cap, "%.*s", (int)(l < 110 ? l : 110), t); *ln = line; break;
        }
        i = e + 1; line++;
    }
    free(b); return found;
}
/* camada antes dos candidatos: aspas -> literal; identificador -> exato. O arquivo e relido para mostrar a linha que tem o texto de fato.
 * Devolve quantas linhas uteis escreveu (0: nada achado) */
static int fd_coder_pre(FdIndex *X, const CoderHits *C, int qk, const char *q, const char *pre, CtSb *sb) {
    char m[760], needle[200], txt[160]; int shown = 0, ln = 0;
    if (qk == FQ_QUOTED) { const char *a = strchr(q, '"') + 1, *b = strchr(a, '"'); snprintf(needle, sizeof needle, "%.*s", (int)(b - a < 199 ? b - a : 199), a); }
    else { while (*q == ' ' || *q == '\t') q++; snprintf(needle, sizeof needle, "%s", q); size_t l = strlen(needle); while (l && (needle[l - 1] == ' ' || needle[l - 1] == '\t' || needle[l - 1] == '\n')) needle[--l] = 0; }
    if (qk == FQ_QUOTED) {
        for (int i = 0; i < C->n && shown < 3; i++) {
            if (!C->r[i].literal || !fd_coder_ok(X, &C->r[i], pre)) continue;
            if (!shown) ct_sb_puts(sb, "Literal text found (checked in the files, not guessed):\n");
            if (fd_line_of(X, C->r[i].path, needle, &ln, txt, sizeof txt)) snprintf(m, sizeof m, "  [confidence high] %s:%d: %s\n", C->r[i].path, ln, txt);
            else snprintf(m, sizeof m, "  [confidence high] %s\n", C->r[i].path);
            ct_sb_puts(sb, m); shown++;
        }
        if (!shown) { snprintf(m, sizeof m, "NOT FOUND (literal): the quoted text does not occur in any of the indexed files%s%s.\n", *pre ? " under " : "", pre); ct_sb_puts(sb, m); }
        return shown;
    }
    /* identificador: exato = o arquivo se chama assim ou o texto existe nele (linha mostrada); o resto e parecido */
    int ex = 0, pr = 0;
    for (int i = 0; i < C->n && ex < 3; i++) {
        if (!fd_coder_ok(X, &C->r[i], pre)) continue;
        const int has = fd_line_of(X, C->r[i].path, needle, &ln, txt, sizeof txt);
        if (!has && !C->r[i].anchor) continue;
        if (!ex) ct_sb_puts(sb, "Found by name or text (checked in the files):\n");
        if (has) snprintf(m, sizeof m, "  [confidence high] %s:%d: %s\n", C->r[i].path, ln, txt); else snprintf(m, sizeof m, "  [confidence high] %s (the file is named like this)\n", C->r[i].path);
        ct_sb_puts(sb, m); ex++;
    }
    shown = ex;
    if (!ex) {
        for (int i = 0; i < C->n && pr < 2; i++) {
            if (!fd_coder_ok(X, &C->r[i], pre)) continue;
            if (!pr) { snprintf(m, sizeof m, "NOT FOUND: nothing is named \"%.80s\". Similar names or spellings (confidence low):\n", needle); ct_sb_puts(sb, m); }
            fd_hit_line(sb, &C->r[i], "confidence low"); pr++;
        }
        shown = pr;
        if (!pr) { snprintf(m, sizeof m, "NOT FOUND: no file name or identifier like \"%.80s\" in the indexed files.\n", needle); ct_sb_puts(sb, m); }
    }
    return shown;
}
/* camada depois dos candidatos (full, pergunta em linguagem natural): ate 3 arquivos que o Find nao mostrou, com grau de certeza e evidencia, e o que nao foi achado */
static int fd_coder_post(const CoderHits *C, const char *pre, FdIndex *X, const FdHit *hits, int nh, CtSb *sb) {
    int shown = 0;
    for (int i = 0; i < C->n && shown < 3; i++) {
        if (!fd_coder_ok(X, &C->r[i], pre) || (!C->r[i].exact && C->r[i].tier > 2)) continue;       /* so o que tem nome/literal ou par conceito+operacao no caminho; o resto e ruido */
        int dup = 0; for (int k = 0; k < nh; k++) if (!strcmp(X->f[hits[k].file].rel, C->r[i].path)) dup = 1;
        if (dup) continue;
        if (!shown) ct_sb_puts(sb, "Also from the text search (other channels):\n");
        fd_hit_line(sb, &C->r[i], C->r[i].exact ? "confidence high" : "confidence low"); shown++;
    }
    if (C->notfound[0]) { char m[300]; snprintf(m, sizeof m, "NOT FOUND in the index: %.200s\n", C->notfound); ct_sb_puts(sb, m); shown++; }
    return shown;
}

static int sym_find_refs(const char *root, const char *pre, const char *name, int limit, CtSb *sb);      /* coder/symtool.c */
/* mode=refs pelo indice de simbolos, se a pasta ja o tem e ele conhece o nome exato; 0 = segue pelo caminho antigo (KATANA_FIND_REFS=text desliga) */
static int fd_refs_sym(const char *query, const char *root, const char *pre, int limit, CtSb *sb) {
    {   /* nome de arquivo (script, modulo): quem o importa/le por source, require, include, import */
        const char *q = query; while (*q == ' ' || *q == '"' || *q == '\'') q++;
        size_t ql = strlen(q); while (ql && (q[ql - 1] == ' ' || q[ql - 1] == '"' || q[ql - 1] == '\'')) ql--;
        const char *base = q; for (size_t i = 0; i < ql; i++) if (q[i] == '/') base = q + i + 1;
        size_t bl = (size_t)(q + ql - base); const char *dot = (const char *)memchr(base, '.', bl);
        if (dot && dot != base && bl < 250 && !memchr(q, ' ', ql) && !memchr(q, '(', ql)) {
            char fname[256]; memcpy(fname, base, bl); fname[bl] = 0; CtSb t = {0};
            if (sym_find_refs(root, pre, fname, limit, &t)) { if (t.p) ct_sb_puts(sb, t.p); free(t.p); return 1; }
            free(t.p);
        }
    }
    const char *e = getenv("KATANA_FIND_REFS"); if (e && !strcmp(e, "text")) return 0;
    const char *p = query; while (*p && !(isalpha((unsigned char)*p) || *p == '_' || *p == '$')) p++;
    int l = fd_idlen(p, p + strlen(p), FK_PS); while (l > 0 && p[l - 1] == '-') l--;
    if (l <= 0 || l > 250) return 0;
    char name[256]; memcpy(name, p, (size_t)l); name[l] = 0;
    CtSb t = {0};
    if (!sym_find_refs(root, pre, name, limit, &t)) { free(t.p); return 0; }
    if (t.p) ct_sb_puts(sb, t.p);
    free(t.p); return 1;
}
static void ct_findtool(const char *js, const char *end, char *out, size_t cap) {
    if (cap < 300) { if (cap) out[0] = 0; return; }
    g_fd_verbose = getenv("KATANA_FIND_TIME") != NULL;
    char *q = ct_str(js, end, "query"), *path = ct_str(js, end, "path"), *mode = ct_str(js, end, "mode");
    if (!q || !*q) { free(q); q = ct_str(js, end, "pattern"); }        /* nome parecido */
    long long lim = ct_int(js, end, "limit", 6);
    if (lim < 1) lim = 1;
    if (lim > 12) lim = 12;
    const int refs = mode && !strcmp(mode, "refs");
    const int nbr = mode && !strcmp(mode, "neighbors");      /* pesquisa/teste, nao anunciado: a nota de vizinhos que o laco poria depois de um Read de `query` */
    if (mode && *mode && !refs && !nbr && strcmp(mode, "search") != 0) { snprintf(out, cap, "error: unknown mode \"%s\". Use \"search\" (default) or \"refs\".", mode); goto done; }
    if (q && *q && !refs && !nbr) snprintf(g_intent_find, sizeof g_intent_find, "%s", q);      /* o Read seguinte mostra o trecho que casa com isto */
    if (!q || !*q) { snprintf(out, cap, "error: missing `query`. Example: {\"query\": \"where the config file is parsed\"} or {\"query\": \"parseConfig\", \"mode\": \"refs\"}"); goto done; }
    if ((!path || !*path) && ct_cwd_is_home()) {
        snprintf(out, cap, "error: the working folder is your home folder, not a project, so Find has nothing to search here but files of other programs. "
                 "Set `path` to the project folder the user means (if the user named it, find where it is with Bash: find / -maxdepth 4 -type d -name NAME 2>/dev/null), then call Find again with that path.");
        goto done;
    }
    {
        char cwd[4096], res[4200], root[4200], pre[4200];
        struct stat st;
        if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
        if (!ct_dir_arg(path, 0, res, sizeof res, &st, out, cap)) goto done;
        /* raiz do indice: a pasta de trabalho; path dentro dela so filtra */
        const size_t cl = strlen(cwd);
        if (strcmp(res, cwd) == 0) { snprintf(root, sizeof root, "%s", cwd); pre[0] = 0; }
        else if (strncmp(res, cwd, cl) == 0 && res[cl] == '/' && strcmp(cwd, "/") != 0) { snprintf(root, sizeof root, "%s", cwd); snprintf(pre, sizeof pre, "%s", res + cl + 1); }
        else if (S_ISDIR(st.st_mode)) { snprintf(root, sizeof root, "%s", res); pre[0] = 0; }
        else {                                  /* um arquivo fora da pasta de trabalho: indice da pasta dele */
            snprintf(root, sizeof root, "%s", res);
            char *sl = strrchr(root, '/');
            if (sl && sl != root) { *sl = 0; snprintf(pre, sizeof pre, "%s", sl + 1); } else { snprintf(root, sizeof root, "/"); snprintf(pre, sizeof pre, "%s", res + 1); }
        }
        char err[300] = "";
        fd_update(&g_fd, root, err, sizeof err);
        FdIndex *X = &g_fd;
        CtSb sb = {0};
        const double t0 = fd_ms();
        if (!X->nf || !X->nc) {
            snprintf(out, cap, "No indexable text files under %s (source and text files up to 300 KB; .git, node_modules, build, dist, vendor and gitignored files are skipped).", path && *path ? path : root);
            goto done;
        }
        if (nbr) {
            char js[4400], res[2000] = "";
            snprintf(js, sizeof js, "{\"file_path\": \"%.4000s\"}", q);
            if (fg_ensure(X)) fg_neighbor_note("Read", js, js + strlen(js), res, sizeof res);
            snprintf(out, cap, "%s", res[0] ? res + 1 : "(no note)");
            goto done;
        }
        if (refs) { if (!fd_refs_sym(q, root, pre, (int)lim, &sb)) fd_refs(X, q, pre, (int)lim, &sb); }
        else {
            FdHit hits[12]; uint32_t qid[64]; int nq = 0, esrc[12];
            const int nh = fd_rank(X, q, pre, hits, (int)lim, qid, &nq, esrc);
            const int lvl = fd_coder_level(), qk = lvl ? fd_query_kind(q) : FQ_NATURAL;
            CoderHits *CH = NULL; int cpre = 0;
            if (lvl && (qk != FQ_NATURAL || lvl >= 2)) {      /* o motor do coder: indice atualizado a cada chamada, criado na primeira */
                CH = (CoderHits *)malloc(sizeof *CH);
                if (CH && coder_lookup(root, q, lvl >= 2, 12, CH) != 0) { free(CH); CH = NULL; }
                if (CH && g_fd_verbose) fprintf(stderr, "find: coder %s%.1f ms (index update %.1f ms, search %.1f ms)\n", CH->created ? "index created, " : "", CH->index_ms + CH->ms, CH->index_ms, CH->ms);
            }
            if (CH && qk != FQ_NATURAL) cpre = fd_coder_pre(X, CH, qk, q, pre, &sb);
            const size_t base = sb.n;
            if (!nh) {
                char m[700];
                if (!cpre || qk == FQ_QUOTED) {
                    snprintf(m, sizeof m, "No match for \"%.200s\"%s%s. Try other words: names of functions, files or messages you expect in the code. "
                             "If it is something you saved earlier (a fact, a preference), try recall(query).", q, *pre ? " in " : "", pre);
                    ct_sb_puts(&sb, m);
                }
            } else {
                const char *e = getenv("KATANA_FIND_CHARS");
                const size_t total = base + (e && atoi(e) > 200 ? (size_t)atoi(e) : 1500);
                for (int i = 0; i < nh; i++) {
                    const size_t left = sb.n < total ? total - sb.n : 0;
                    const size_t share = left / (size_t)(nh - i);
                    if (left < 60) { char m[64]; snprintf(m, sizeof m, "(+%d more candidates; raise limit or narrow path)\n", nh - i); ct_sb_puts(&sb, m); break; }
                    char why[400]; why[0] = 0;
                    if (esrc[i] >= 0) {        /* entrou por uma aresta do grafo: a evidencia diz de qual arquivo e por que */
                        fg_evidence(X, esrc[i], &hits[i], why, sizeof why);
                    }
                    fd_show(X, &hits[i], i + 1, qid, nq, share, why, &sb);
                }
            }
            if (CH && qk == FQ_NATURAL) fd_coder_post(CH, pre, X, hits, nh, &sb);
            free(CH);
        }
        if (X->truncated) { char m[200]; snprintf(m, sizeof m, "(index covers %d files; %d more skipped: KATANA_FIND_MAX_FILES)\n", X->listed, X->truncated); ct_sb_puts(&sb, m); }
        ct_sb_puts(&sb, refs ? "Next: Find mode=refs on a caller, or Read a part with offset and limit." : "Follow a symbol with Find mode=refs, or Read a part with offset and limit.");
        if (g_fd_verbose) fprintf(stderr, "find: query %.1f ms\n", fd_ms() - t0);
        snprintf(out, cap, "%s", sb.p ? sb.p : "");
        free(sb.p);
    }
done:
    free(q); free(path); free(mode);
}
#else
static void ct_findtool(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: not available on Windows yet"); }
#endif
