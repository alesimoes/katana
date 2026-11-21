/* ---------- motor de expressoes regulares (usado pelo Grep; ver docs/tools.md) ----------
 * Escrito do zero, so com a libc. O padrao vira uma arvore (RxNode) e depois um programa pequeno (RxInst) que uma
 * maquina com retrocesso executa sobre bytes UTF-8, um ponto de codigo por vez. Byte invalido (no padrao ou no texto)
 * vale como uma unidade sozinha, com codigo RX_BAD + byte: so casa com o mesmo byte invalido, com . e com classes negadas.
 *
 * Sintaxe: literais, . ^ $ \A \z \Z \b \B \< \>; classes [...] com negacao, intervalos, [[:alpha:]] etc.; \d \D \w \W
 * \s \S \h \H \v \V; \t \n \r \f \e \a \0 \xHH \x{H..} \uHHHH; \p{..} e \P{..} (L Lu Ll N Nd P Z, Latin Greek Cyrillic);
 * grupos (...) (?:...) (?<n>...) (?P<n>...) (?'n'...) (?>...); alternativa |; quantificadores * + ? {n} {n,} {n,m} {,m}
 * gulosos, preguicosos (?) e possessivos (+); (?=) (?!) (?<=) (?<!); \1..\99, \k<n>, (?P=n); flags (?imsx-imsx) e
 * (?i:...); \Q...\E. Uma { que nao forma repeticao e literal.
 *
 * Limites: lookbehind de tamanho limitado (no maximo RX_MAXLB pontos de codigo; sem * + {n,} nem referencia dentro);
 * repeticao ate RX_MAXREP; programa ate RX_MAXPROG instrucoes; grupos aninhados ate RX_MAXDEPTH (isso limita tambem a
 * recursao da maquina, que so se chama de novo em lookaround e grupo atomico); passos por busca RX_STEPS mais 100 por
 * byte do texto (rx_search devolve -1 quando estoura). Tabelas Unicode aproximadas: latim (com estendidos A, B e
 * adicional), grego, cirilico, armenio, hebraico, arabe, devanagari, tailandes, georgiano, CJK, kana, hangul.
 * Maiuscula/minuscula: ASCII, latim-1, latim estendido A/B/adicional, grego, cirilico, armenio, largura cheia. */

#define RX_I 1          /* ignora caixa */
#define RX_M 2          /* ^ e $ em cada linha */
#define RX_S 4          /* . casa \n */
#define RX_X 8          /* espacos e # comentarios ignorados no padrao */
#define RX_LITERAL 16   /* padrao inteiro e texto simples */
#define RX_STEPS 10000000L
#define RX_MAXPROG 200000
#define RX_MAXDEPTH 200
#define RX_MAXLB 255
#define RX_MAXREP 10000
#define RX_BAD 0x110000u

enum { RX_CHAR, RX_ANY, RX_ANYNL, RX_CLASS, RX_BOL, RX_EOL, RX_MBOL, RX_MEOL, RX_BEGIN, RX_END, RX_ENDZ, RX_WORDB,
       RX_NWORDB, RX_WSTART, RX_WEND, RX_SPLIT, RX_JMP, RX_SAVE, RX_MARK, RX_LOOP, RX_BREF, RX_ATOMIC, RX_LOOK,
       RX_NLOOK, RX_LOOKB, RX_NLOOKB, RX_SUCCEED, RX_MATCH };
enum { N_CHAR, N_ANY, N_CLASS, N_ASSERT, N_GROUP, N_ATOMIC, N_LOOK, N_CAT, N_ALT, N_REP, N_BREF, N_EMPTY };
enum { SP_DIGIT = 1, SP_WORD, SP_SPACE, SP_H, SP_V, SP_L, SP_LU, SP_LL, SP_N, SP_ND, SP_P, SP_Z, SP_ALNUM, SP_BLANK,
       SP_CNTRL, SP_GRAPH, SP_PRINT, SP_PUNCT, SP_XDIGIT, SP_LATIN, SP_GREEK, SP_CYRILLIC };

typedef struct { int t, a, b, ic, kid, next; unsigned c; } RxNode;
typedef struct { unsigned char op, ic; int x, y, mn, mx; unsigned c; } RxInst;
typedef struct { unsigned lo, hi; } RxRange;
typedef struct { int neg, n, cap, ns; RxRange *r; int sp[16]; } RxClass;
typedef struct { int pc; long pos; } RxBt;      /* pc >= 0: alternativa; pc < 0: restaura o slot -1-pc */

typedef struct {
    RxNode *nd; int nn, ndcap;
    RxInst *pg; int np, pgcap, toobig;
    RxClass *cl; int ncl, clcap;
    int ngroups, nregs, nslots, maxref;
    char nm[64][33]; int nmi[64], nnames;
    int anchor;                 /* 0 nenhum, 1 so no inicio do texto, 2 inicio de linha */
    unsigned char fb[256]; int usefb;
    char pre[64]; int prelen;   /* prefixo literal obrigatorio */
    char req[64]; int reqlen;   /* trecho literal obrigatorio em qualquer lugar */
    /* execucao */
    long *slots; RxBt *stk; size_t sp, scap; long steps, budget; int err;
    const unsigned char *s; long n;
} Rx;

/* ---- UTF-8 ---- */
static int rx_dec(const unsigned char *s, const unsigned char *e, unsigned *cp) {
    const unsigned c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int n; unsigned v;
    if (c >= 0xC2 && c <= 0xDF) { n = 2; v = c & 31; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; v = c & 15; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; v = c & 7; }
    else goto bad;
    if (e - s < n) goto bad;
    for (int i = 1; i < n; i++) { if ((s[i] & 0xC0) != 0x80) goto bad; v = (v << 6) | (s[i] & 63); }
    if ((n == 3 && v < 0x800) || (n == 4 && (v < 0x10000 || v > 0x10FFFF)) || (v >= 0xD800 && v <= 0xDFFF)) goto bad;
    *cp = v; return n;
bad:
    *cp = RX_BAD + c; return 1;
}
/* tamanho da unidade que termina em pos (pos > 0) */
static int rx_backlen(const unsigned char *s, long pos) {
    if (s[pos - 1] < 0x80) return 1;
    for (int k = 2; k <= 4 && pos - k >= 0; k++) {
        unsigned cp;
        if ((s[pos - k] & 0xC0) == 0x80) continue;
        return rx_dec(s + pos - k, s + pos, &cp) == k ? k : 1;
    }
    return 1;
}
static int rx_enc(unsigned c, char *o) {
    if (c >= RX_BAD) { o[0] = (char)(c - RX_BAD); return 1; }
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}
static const char *rx_memmem(const char *h, size_t hl, const char *nd, size_t nl) {
    if (nl == 0) return h;
    if (hl < nl) return NULL;
    const char *end = h + hl - nl + 1;
    for (const char *p = h; p < end; ) {
        p = (const char *)memchr(p, nd[0], (size_t)(end - p));
        if (!p) return NULL;
        if (memcmp(p, nd, nl) == 0) return p;
        p++;
    }
    return NULL;
}

/* ---- caixa ---- */
static unsigned rx_lower(unsigned c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c < 0x100) return (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? c + 32 : c;
    if (c < 0x180) {
        if (c == 0x130) return 'i';
        if (c == 0x178) return 0xFF;
        if ((c <= 0x12F || (c >= 0x132 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && !(c & 1)) return c + 1;
        if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && (c & 1)) return c + 1;
        return c;
    }
    if (c < 0x250) {
        if (c >= 0x1CD && c <= 0x1DC && (c & 1)) return c + 1;
        if (((c >= 0x1DE && c <= 0x1EF) || (c >= 0x1F8 && c <= 0x21F) || (c >= 0x222 && c <= 0x233) || (c >= 0x246 && c <= 0x24F)) && !(c & 1)) return c + 1;
        return c;
    }
    if (c >= 0x370 && c < 0x400) {
        if (c == 0x386) return 0x3AC;
        if (c >= 0x388 && c <= 0x38A) return c + 37;
        if (c == 0x38C) return 0x3CC;
        if (c == 0x38E || c == 0x38F) return c + 63;
        if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 32;
        if (c >= 0x3D8 && c <= 0x3EF && !(c & 1)) return c + 1;
        return c;
    }
    if (c >= 0x400 && c < 0x530) {
        if (c <= 0x40F) return c + 80;
        if (c <= 0x42F) return c + 32;
        if (c == 0x4C0) return 0x4CF;
        if (((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) && !(c & 1)) return c + 1;
        if (c >= 0x4C1 && c <= 0x4CE && (c & 1)) return c + 1;
        return c;
    }
    if (c >= 0x531 && c <= 0x556) return c + 48;
    if (c >= 0x1E00 && c <= 0x1EFF) return ((c <= 0x1E95 || c >= 0x1EA0) && !(c & 1)) ? c + 1 : c;
    if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
    return c;
}
static unsigned rx_upper(unsigned c) {
    static const unsigned d[] = { 1, 32, 37, 48, 63, 80 };
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c == 0xFF) return 0x178;
    if (c == 0x3AC) return 0x386;
    if (c == 0x3CC) return 0x38C;
    if (c == 0x3C2) return 0x3A3;
    if (c == 0x4CF) return 0x4C0;
    if (c == 0x17F) return 'S';
    for (int k = 0; k < 6; k++) if (c >= d[k] + 0x80 && rx_lower(c - d[k]) == c) return c - d[k];
    return c;
}
/* forma usada para comparar sem caixa */
static unsigned rx_fold(unsigned c) {
    c = rx_lower(c);
    if (c == 0x3C2) return 0x3C3;
    if (c == 0x17F) return 's';
    return c;
}

/* ---- categorias ---- */
static int rx_inr(unsigned c, const RxRange *r, int n) {
    for (int i = 0; i < n; i++) { if (c < r[i].lo) return 0; if (c <= r[i].hi) return 1; }
    return 0;
}
static int rx_isletter(unsigned c) {
    static const RxRange r[] = { {0x531,0x556},{0x561,0x587},{0x5D0,0x5EA},{0x620,0x64A},{0x671,0x6D3},{0x904,0x939},
        {0xE01,0xE30},{0x10A0,0x10FF},{0x1E00,0x1FBC},{0x1FC2,0x1FCC},{0x1FD0,0x1FDB},{0x1FE0,0x1FEC},{0x1FF2,0x1FFC},
        {0x3041,0x3096},{0x30A1,0x30FA},{0x3400,0x4DBF},{0x4E00,0x9FFF},{0xAC00,0xD7A3},{0xF900,0xFAFF},{0xFF21,0xFF3A},
        {0xFF41,0xFF5A} };
    if (c < 0x80) return (c | 32) >= 'a' && (c | 32) <= 'z';
    if (c < 0x100) return c == 0xAA || c == 0xB5 || c == 0xBA || (c >= 0xC0 && c != 0xD7 && c != 0xF7);
    if (c < 0x2C0) return 1;
    if (c < 0x370) return 0;
    if (c < 0x400) return !(c == 0x375 || c == 0x37E || c == 0x384 || c == 0x385 || c == 0x387 || c == 0x3F6 ||
                            c == 0x378 || c == 0x379 || (c >= 0x380 && c <= 0x383) || c == 0x38B || c == 0x38D || c == 0x3A2);
    if (c < 0x530) return !(c >= 0x482 && c <= 0x489);
    return rx_inr(c, r, (int)(sizeof r / sizeof r[0]));
}
static int rx_isnd(unsigned c) {
    static const RxRange r[] = { {0x30,0x39},{0x660,0x669},{0x6F0,0x6F9},{0x966,0x96F},{0xE50,0xE59},{0xFF10,0xFF19} };
    return rx_inr(c, r, 6);
}
static int rx_isnum(unsigned c) {
    static const RxRange r[] = { {0x30,0x39},{0xB2,0xB3},{0xB9,0xB9},{0xBC,0xBE},{0x660,0x669},{0x6F0,0x6F9},{0x966,0x96F},
        {0xE50,0xE59},{0x2070,0x2070},{0x2074,0x2079},{0x2080,0x2089},{0x2150,0x2189},{0x2460,0x249B},{0x3007,0x3007},
        {0xFF10,0xFF19} };
    return rx_inr(c, r, (int)(sizeof r / sizeof r[0]));
}
static int rx_ispunct(unsigned c) {
    static const RxRange r[] = { {0xA1,0xA1},{0xA7,0xA7},{0xAB,0xAB},{0xB6,0xB7},{0xBB,0xBB},{0xBF,0xBF},{0x37E,0x37E},
        {0x387,0x387},{0x55A,0x55F},{0x589,0x58A},{0x5BE,0x5BE},{0x5C0,0x5C0},{0x5C3,0x5C3},{0x5F3,0x5F4},{0x60C,0x60D},
        {0x61B,0x61B},{0x61F,0x61F},{0x66A,0x66D},{0x964,0x965},{0x2010,0x2027},{0x2030,0x2043},{0x2045,0x2051},
        {0x2053,0x205E},{0x3001,0x3003},{0x3008,0x3011},{0x3014,0x301F},{0xFF01,0xFF03},{0xFF05,0xFF0A},{0xFF0C,0xFF0F},
        {0xFF1A,0xFF1B},{0xFF1F,0xFF20} };
    if (c < 0x80) return c < 0x7F && strchr("!\"#%&'()*,-./:;?@[\\]_{}", (int)c) != NULL && c != 0;
    return rx_inr(c, r, (int)(sizeof r / sizeof r[0]));
}
static int rx_isz(unsigned c) {
    return c == 0x20 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000;
}
static int rx_isspace(unsigned c) { return (c >= 9 && c <= 13) || c == 0x85 || rx_isz(c); }
static int rx_ish(unsigned c) { return c == 9 || (rx_isz(c) && c != 0x2028 && c != 0x2029); }
static int rx_isv(unsigned c) { return (c >= 10 && c <= 13) || c == 0x85 || c == 0x2028 || c == 0x2029; }
static int rx_isword(unsigned c) {
    if (c < 0x80) return isalnum((int)c) || c == '_';
    return rx_isletter(c) || rx_isnd(c) || (c >= 0x300 && c <= 0x36F) || (c >= 0x483 && c <= 0x487) ||
           (c >= 0x591 && c <= 0x5BD) || (c >= 0x64B && c <= 0x65F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x20D0 && c <= 0x20FF);
}
static int rx_special(int code, unsigned c) {
    switch (code) {
        case SP_DIGIT: return c >= '0' && c <= '9';
        case SP_WORD: return rx_isword(c);
        case SP_SPACE: return rx_isspace(c);
        case SP_H: return rx_ish(c);
        case SP_V: return rx_isv(c);
        case SP_L: return rx_isletter(c);
        case SP_LU: return rx_isletter(c) && rx_lower(c) != c;
        case SP_LL: return rx_isletter(c) && (rx_upper(c) != c || c == 0xDF || c == 0x138 || c == 0x149 || (c >= 0x250 && c <= 0x2AF));
        case SP_N: return rx_isnum(c);
        case SP_ND: return rx_isnd(c);
        case SP_P: return rx_ispunct(c);
        case SP_Z: return rx_isz(c);
        case SP_ALNUM: return rx_isletter(c) || rx_isnd(c);
        case SP_BLANK: return c == ' ' || c == '\t';
        case SP_CNTRL: return c < 0x20 || c == 0x7F;
        case SP_GRAPH: return c > 0x20 && c != 0x7F && c < RX_BAD && !rx_isspace(c) && !(c >= 0x80 && c < 0xA0);
        case SP_PRINT: return c == ' ' || (c > 0x20 && c != 0x7F && c < RX_BAD && !rx_isspace(c) && !(c >= 0x80 && c < 0xA0));
        case SP_PUNCT: return c < 0x80 ? (c > 0x20 && c < 0x7F && !isalnum((int)c)) : rx_ispunct(c);
        case SP_XDIGIT: return c < 0x80 && isxdigit((int)c);
        case SP_LATIN: return rx_isletter(c) && (c < 0x250 || (c >= 0x1E00 && c <= 0x1EFF) || (c >= 0xFF21 && c <= 0xFF5A));
        case SP_GREEK: return (c >= 0x370 && c <= 0x3FF && rx_isletter(c)) || (c >= 0x1F00 && c <= 0x1FFF);
        case SP_CYRILLIC: return c >= 0x400 && c <= 0x52F;
    }
    return 0;
}
static int rx_cin(const RxClass *k, unsigned c) {
    for (int i = 0; i < k->n; i++) if (c >= k->r[i].lo && c <= k->r[i].hi) return 1;
    for (int i = 0; i < k->ns; i++) { const int s = k->sp[i]; if (rx_special(s < 0 ? -s : s, c) != (s < 0)) return 1; }
    return 0;
}
static int rx_cmatch(const Rx *re, int idx, unsigned c, int ic) {
    const RxClass *k = &re->cl[idx];
    int in = rx_cin(k, c);
    if (!in && ic) {
        const unsigned l = rx_lower(c), u = rx_upper(c), f = rx_fold(c);
        in = (l != c && rx_cin(k, l)) || (u != c && rx_cin(k, u)) || (f != l && rx_cin(k, f)) || rx_cin(k, rx_upper(f));
    }
    return in != k->neg;
}

/* ---- analise do padrao ---- */
typedef struct { Rx *re; const unsigned char *s, *p, *e; int fl, depth, inq; char *err; size_t ecap; } RxP;

static int rx_node(Rx *re, int t) {
    if (re->nn == re->ndcap) { re->ndcap = re->ndcap ? re->ndcap * 2 : 64; re->nd = (RxNode *)realloc(re->nd, sizeof(RxNode) * (size_t)re->ndcap); }
    RxNode *n = &re->nd[re->nn];
    memset(n, 0, sizeof *n); n->t = t; n->kid = -1; n->next = -1;
    return re->nn++;
}
static int rx_fail(RxP *P, const unsigned char *at, const char *msg) {
    if (!P->err[0]) snprintf(P->err, P->ecap, "%s at position %d", msg, (int)(at - P->s) + 1);
    return -1;
}
static int rx_newclass(Rx *re) {
    if (re->ncl == re->clcap) { re->clcap = re->clcap ? re->clcap * 2 : 8; re->cl = (RxClass *)realloc(re->cl, sizeof(RxClass) * (size_t)re->clcap); }
    memset(&re->cl[re->ncl], 0, sizeof(RxClass));
    return re->ncl++;
}
static void rx_crange(Rx *re, int ci, unsigned lo, unsigned hi) {
    RxClass *k = &re->cl[ci];
    if (k->n == k->cap) { k->cap = k->cap ? k->cap * 2 : 8; k->r = (RxRange *)realloc(k->r, sizeof(RxRange) * (size_t)k->cap); }
    k->r[k->n].lo = lo; k->r[k->n].hi = hi; k->n++;
}
static int rx_cspec(RxP *P, int ci, int code, const unsigned char *at) {
    RxClass *k = &P->re->cl[ci];
    if (k->ns >= 16) return rx_fail(P, at, "too many \\d, \\w, \\p{..} or [:name:] items in one class");
    k->sp[k->ns++] = code;
    return 0;
}
static int rx_hex(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }
static void rx_skipx(RxP *P) {
    if (!(P->fl & RX_X) || P->inq) return;
    for (;;) {
        while (P->p < P->e && isspace(*P->p)) P->p++;
        if (P->p < P->e && *P->p == '#') { while (P->p < P->e && *P->p != '\n') P->p++; continue; }
        break;
    }
}
/* escape que vira um caractere; P->p aponta logo depois da barra. 0 ok, -1 erro */
static int rx_escchar(RxP *P, unsigned *cp, int inclass) {
    const unsigned char *at = P->p - 1;
    const unsigned c = *P->p++;
    switch (c) {
        case 't': *cp = 9; return 0;
        case 'n': *cp = 10; return 0;
        case 'r': *cp = 13; return 0;
        case 'f': *cp = 12; return 0;
        case 'e': *cp = 27; return 0;
        case 'a': *cp = 7; return 0;
        case 'b': if (inclass) { *cp = 8; return 0; } break;
        case '0': {
            unsigned v = 0;
            for (int i = 0; i < 2 && P->p < P->e && *P->p >= '0' && *P->p <= '7'; i++) v = v * 8 + (unsigned)(*P->p++ - '0');
            *cp = v; return 0;
        }
        case 'x': case 'u': {
            unsigned v = 0; int nd = 0;
            if (P->p < P->e && *P->p == '{') {
                P->p++;
                while (P->p < P->e && rx_hex(*P->p) >= 0 && nd < 7) { v = v * 16 + (unsigned)rx_hex(*P->p++); nd++; }
                if (P->p >= P->e || *P->p != '}' || nd == 0 || v > 0x10FFFF) return rx_fail(P, at, "bad \\x{...} escape: use 1 to 6 hex digits up to 10FFFF");
                P->p++;
            } else {
                const int want = c == 'x' ? 2 : 4;
                while (nd < want && P->p < P->e && rx_hex(*P->p) >= 0) { v = v * 16 + (unsigned)rx_hex(*P->p++); nd++; }
                if (nd == 0 || (c == 'u' && nd != 4)) return rx_fail(P, at, c == 'x' ? "bad \\x escape: use \\xHH or \\x{HHHH}" : "bad \\u escape: use \\uHHHH");
            }
            *cp = v; return 0;
        }
        default: break;
    }
    if (c >= 0x80) { P->p--; P->p += rx_dec(P->p, P->e, cp); return 0; }
    if (!isalnum((int)c)) { *cp = c; return 0; }
    char m[80];
    snprintf(m, sizeof m, "unknown escape \\%c (write \\\\%c for a backslash followed by %c)", (char)c, (char)c, (char)c);
    return rx_fail(P, at, m);
}
/* nome de \p{..}; P->p aponta depois do p/P. Devolve codigo (negativo se negado) ou 0 com erro */
static int rx_prop(RxP *P, int neg) {
    static const struct { const char *n; int c; } t[] = {
        {"L", SP_L}, {"Letter", SP_L}, {"Lu", SP_LU}, {"Uppercase_Letter", SP_LU}, {"Ll", SP_LL}, {"Lowercase_Letter", SP_LL},
        {"N", SP_N}, {"Number", SP_N}, {"Nd", SP_ND}, {"Decimal_Number", SP_ND}, {"P", SP_P}, {"Punctuation", SP_P},
        {"Z", SP_Z}, {"Zs", SP_Z}, {"Separator", SP_Z}, {"Latin", SP_LATIN}, {"Greek", SP_GREEK}, {"Cyrillic", SP_CYRILLIC},
        {"Alpha", SP_L}, {"Alphabetic", SP_L}, {"White_Space", SP_SPACE}, {"Space", SP_SPACE} };
    const unsigned char *at = P->p - 2;
    char nm[40]; int l = 0;
    if (P->p < P->e && *P->p == '{') {
        P->p++;
        if (P->p < P->e && *P->p == '^') { neg = !neg; P->p++; }
        while (P->p < P->e && *P->p != '}' && l < 39) nm[l++] = (char)*P->p++;
        if (P->p >= P->e || *P->p != '}') { rx_fail(P, at, "missing } in \\p{...}"); return 0; }
        P->p++;
    } else if (P->p < P->e && isalpha(*P->p)) nm[l++] = (char)*P->p++;
    nm[l] = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (strcmp(t[i].n, nm) == 0) return neg ? -t[i].c : t[i].c;
    rx_fail(P, at, "unknown Unicode property (known: L Lu Ll N Nd P Z Latin Greek Cyrillic)");
    return 0;
}
static int rx_letterclass(int c) {
    switch (c) {
        case 'd': return SP_DIGIT; case 'D': return -SP_DIGIT; case 'w': return SP_WORD; case 'W': return -SP_WORD;
        case 's': return SP_SPACE; case 'S': return -SP_SPACE; case 'h': return SP_H; case 'H': return -SP_H;
        case 'v': return SP_V; case 'V': return -SP_V;
    }
    return 0;
}
static int rx_posix(const char *nm) {
    static const struct { const char *n; int c; } t[] = { {"alpha", SP_L}, {"digit", SP_DIGIT}, {"alnum", SP_ALNUM},
        {"upper", SP_LU}, {"lower", SP_LL}, {"space", SP_SPACE}, {"blank", SP_BLANK}, {"punct", SP_PUNCT}, {"print", SP_PRINT},
        {"graph", SP_GRAPH}, {"cntrl", SP_CNTRL}, {"xdigit", SP_XDIGIT}, {"word", SP_WORD} };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (strcmp(t[i].n, nm) == 0) return t[i].c;
    return 0;
}
static int rx_mkclass(RxP *P, int code) {
    const int ci = rx_newclass(P->re);
    P->re->cl[ci].sp[P->re->cl[ci].ns++] = code;
    const int n = rx_node(P->re, N_CLASS);
    P->re->nd[n].a = ci; P->re->nd[n].ic = (P->fl & RX_I) != 0;
    return n;
}
static int rx_mkchar(RxP *P, unsigned cp) {
    const int n = rx_node(P->re, N_CHAR);
    const int ic = (P->fl & RX_I) != 0;
    P->re->nd[n].c = ic ? rx_fold(cp) : cp; P->re->nd[n].ic = ic;
    return n;
}
static int rx_class(RxP *P) {
    const unsigned char *open = P->p++;
    const int ci = rx_newclass(P->re);
    if (P->p < P->e && *P->p == '^') { P->re->cl[ci].neg = 1; P->p++; }
    int first = 1;
    for (;;) {
        if (P->p >= P->e) return rx_fail(P, open, "missing ] to close the class [ opened");
        if (*P->p == ']' && !first) { P->p++; break; }
        first = 0;
        const unsigned char *at = P->p;
        unsigned lo = 0; int sp = 0;
        if (*P->p == '[' && P->p + 1 < P->e && P->p[1] == ':') {
            const unsigned char *q = P->p + 2; int neg = 0; char nm[16]; int l = 0;
            if (q < P->e && *q == '^') { neg = 1; q++; }
            while (q < P->e && isalpha(*q) && l < 15) nm[l++] = (char)*q++;
            nm[l] = 0;
            if (q + 1 < P->e && q[0] == ':' && q[1] == ']') {
                const int code = rx_posix(nm);
                if (!code) return rx_fail(P, at, "unknown class name in [:name:] (known: alpha digit alnum upper lower space blank punct print graph cntrl xdigit word)");
                P->p = q + 2;
                if (rx_cspec(P, ci, neg ? -code : code, at) < 0) return -1;
                continue;
            }
            lo = '['; P->p++;
        } else if (*P->p == '\\') {
            P->p++;
            if (P->p >= P->e) return rx_fail(P, at, "trailing backslash");
            const int c = *P->p;
            if ((sp = rx_letterclass(c)) != 0) P->p++;
            else if (c == 'p' || c == 'P') { P->p++; sp = rx_prop(P, c == 'P'); if (!sp) return -1; }
            else if (rx_escchar(P, &lo, 1) < 0) return -1;
            if (sp) { if (rx_cspec(P, ci, sp, at) < 0) return -1; continue; }
        } else P->p += rx_dec(P->p, P->e, &lo);
        unsigned hi = lo;
        if (P->p + 1 < P->e && *P->p == '-' && P->p[1] != ']') {
            const unsigned char *save = P->p;
            P->p++;
            int ok = 1;
            if (*P->p == '\\') {
                P->p++;
                if (P->p >= P->e) return rx_fail(P, at, "trailing backslash");
                if (rx_letterclass(*P->p) || *P->p == 'p' || *P->p == 'P') ok = 0;
                else if (rx_escchar(P, &hi, 1) < 0) return -1;
            } else if (*P->p == '[' && P->p + 1 < P->e && P->p[1] == ':') ok = 0;
            else P->p += rx_dec(P->p, P->e, &hi);
            if (!ok) { P->p = save; hi = lo; }
            else if (hi < lo) return rx_fail(P, at, "invalid range in class (the first character comes after the second)");
        }
        rx_crange(P->re, ci, lo, hi);
    }
    const int n = rx_node(P->re, N_CLASS);
    P->re->nd[n].a = ci; P->re->nd[n].ic = (P->fl & RX_I) != 0;
    return n;
}
static int rx_alt(RxP *P);
static int rx_findname(Rx *re, const char *nm) {
    for (int i = 0; i < re->nnames; i++) if (strcmp(re->nm[i], nm) == 0) return re->nmi[i];
    return -1;
}
/* le um nome de grupo ate o caractere close */
static int rx_name(RxP *P, char close, char *nm, const unsigned char *at) {
    int l = 0;
    while (P->p < P->e && *P->p != close) {
        const int c = *P->p;
        if (!(isalnum(c) || c == '_') || l >= 32 || (l == 0 && isdigit(c))) return rx_fail(P, at, "bad group name: use letters, digits and _, starting with a letter");
        nm[l++] = (char)c; P->p++;
    }
    nm[l] = 0;
    if (P->p >= P->e || l == 0) return rx_fail(P, at, "bad group name");
    P->p++;
    return 0;
}
static int rx_bref(RxP *P, int g, const unsigned char *at) {
    (void)at;
    const int n = rx_node(P->re, N_BREF);
    P->re->nd[n].a = g; P->re->nd[n].ic = (P->fl & RX_I) != 0;
    if (g > P->re->maxref) P->re->maxref = g;
    return n;
}
static void rx_len(Rx *re, int ni, long *mn, long *mx);
static int rx_group(RxP *P) {
    const unsigned char *open = P->p++;
    if (P->depth >= RX_MAXDEPTH) return rx_fail(P, open, "groups nested too deeply");
    const int savefl = P->fl;
    int kind = N_GROUP, cap = 1, look = 0;
    char nm[40] = "";
    if (P->p < P->e && *P->p == '?') {
        P->p++;
        const int c = P->p < P->e ? *P->p : 0;
        if (c == ':') { cap = 0; P->p++; }
        else if (c == '>') { kind = N_ATOMIC; cap = 0; P->p++; }
        else if (c == '=' || c == '!') { kind = N_LOOK; look = c == '=' ? RX_LOOK : RX_NLOOK; cap = 0; P->p++; }
        else if (c == '<' && P->p + 1 < P->e && (P->p[1] == '=' || P->p[1] == '!')) { kind = N_LOOK; look = P->p[1] == '=' ? RX_LOOKB : RX_NLOOKB; cap = 0; P->p += 2; }
        else if (c == '<' || c == '\'') { P->p++; if (rx_name(P, c == '<' ? '>' : '\'', nm, open) < 0) return -1; }
        else if (c == 'P' && P->p + 1 < P->e && P->p[1] == '<') { P->p += 2; if (rx_name(P, '>', nm, open) < 0) return -1; }
        else if (c == 'P' && P->p + 1 < P->e && P->p[1] == '=') {
            P->p += 2;
            if (rx_name(P, ')', nm, open) < 0) return -1;
            const int g = rx_findname(P->re, nm);
            if (g < 0) return rx_fail(P, open, "reference to an unknown group name");
            return rx_bref(P, g, open);
        } else if (c == '#') {
            while (P->p < P->e && *P->p != ')') P->p++;
            if (P->p >= P->e) return rx_fail(P, open, "missing ) to close the comment (?#");
            P->p++;
            return -2;
        } else if (c && strchr("imsx-", c)) {
            int on = 1, fl = P->fl;
            while (P->p < P->e && strchr("imsx-", *P->p) && *P->p) {
                const int f = *P->p == 'i' ? RX_I : *P->p == 'm' ? RX_M : *P->p == 's' ? RX_S : *P->p == 'x' ? RX_X : 0;
                if (*P->p == '-') on = 0; else if (on) fl |= f; else fl &= ~f;
                P->p++;
            }
            if (P->p < P->e && *P->p == ')') { P->p++; P->fl = fl; return -2; }
            if (P->p < P->e && *P->p == ':') { P->p++; P->fl = fl; cap = 0; }
            else return rx_fail(P, open, "bad inline flags: use (?i) (?m) (?s) (?x), (?-i) or (?i:...)");
        } else return rx_fail(P, open, "unknown group syntax after (? (known: (?: (?> (?= (?! (?<= (?<! (?<name> (?i)");
    }
    int gi = -1;
    if (cap) {
        gi = P->re->ngroups++;
        if (nm[0]) {
            if (rx_findname(P->re, nm) >= 0) return rx_fail(P, open, "duplicate group name");
            if (P->re->nnames >= 64) return rx_fail(P, open, "too many named groups");
            snprintf(P->re->nm[P->re->nnames], 33, "%s", nm); P->re->nmi[P->re->nnames++] = gi;
        }
    }
    P->depth++;
    const int kid = rx_alt(P);
    P->depth--;
    if (kid < 0) return -1;
    if (P->p >= P->e || *P->p != ')') return rx_fail(P, open, "missing ) to close the group opened");
    P->p++;
    P->fl = savefl;
    const int n = rx_node(P->re, kind);
    P->re->nd[n].kid = kid; P->re->nd[n].a = kind == N_GROUP ? gi : look;
    if (look == RX_LOOKB || look == RX_NLOOKB) {
        long mn, mx;
        rx_len(P->re, kid, &mn, &mx);
        if (mx < 0 || mx > RX_MAXLB) return rx_fail(P, open, "lookbehind must have a bounded length (at most 255 characters; no *, +, {n,} or backreference inside)");
        P->re->nd[n].b = (int)mn; P->re->nd[n].c = (unsigned)mx;
    }
    return n;
}
/* {n}, {n,}, {n,m}, {,m}: 1 e preenche se for repeticao valida em q */
static int rx_brace(const unsigned char *q, const unsigned char *e, long *mn, long *mx, const unsigned char **after) {
    q++;
    long a = -1, b = -1; int comma = 0;
    if (q < e && isdigit(*q)) { a = 0; while (q < e && isdigit(*q)) { if (a < 1000000) a = a * 10 + (*q - '0'); q++; } }
    if (q < e && *q == ',') { comma = 1; q++; if (q < e && isdigit(*q)) { b = 0; while (q < e && isdigit(*q)) { if (b < 1000000) b = b * 10 + (*q - '0'); q++; } } }
    if (q >= e || *q != '}' || (a < 0 && b < 0)) return 0;
    *mn = a < 0 ? 0 : a; *mx = comma ? b : a; *after = q + 1;
    return 1;
}
static int rx_escape(RxP *P) {
    const unsigned char *at = P->p++;
    if (P->p >= P->e) return rx_fail(P, at, "trailing backslash (write \\\\ for a literal backslash)");
    const int c = *P->p;
    int sp = rx_letterclass(c);
    if (sp) { P->p++; return rx_mkclass(P, sp); }
    if (c == 'p' || c == 'P') { P->p++; sp = rx_prop(P, c == 'P'); return sp ? rx_mkclass(P, sp) : -1; }
    int as = -1;
    switch (c) {
        case 'b': as = RX_WORDB; break; case 'B': as = RX_NWORDB; break; case 'A': as = RX_BEGIN; break;
        case 'z': as = RX_END; break; case 'Z': as = RX_ENDZ; break; case '<': as = RX_WSTART; break; case '>': as = RX_WEND; break;
        case 'Q': P->p++; P->inq = 1; return -2;
        case 'E': P->p++; return -2;
        case 'k': {
            P->p++;
            const char close = P->p < P->e ? (*P->p == '<' ? '>' : *P->p == '\'' ? '\'' : *P->p == '{' ? '}' : 0) : 0;
            if (!close) return rx_fail(P, at, "bad \\k: use \\k<name>");
            P->p++;
            char nm[40];
            if (rx_name(P, close, nm, at) < 0) return -1;
            const int g = rx_findname(P->re, nm);
            if (g < 0) return rx_fail(P, at, "reference to an unknown group name");
            return rx_bref(P, g, at);
        }
        default: break;
    }
    if (as >= 0) { P->p++; const int n = rx_node(P->re, N_ASSERT); P->re->nd[n].a = as; return n; }
    if (c >= '1' && c <= '9') {
        int g = c - '0'; P->p++;
        if (P->p < P->e && isdigit(*P->p) && g * 10 + (*P->p - '0') < P->re->ngroups) g = g * 10 + (*P->p++ - '0');
        return rx_bref(P, g, at);
    }
    unsigned cp;
    if (rx_escchar(P, &cp, 0) < 0) return -1;
    return rx_mkchar(P, cp);
}
static int rx_atom(RxP *P) {
    const unsigned char *at = P->p;
    if (P->inq) {
        if (*P->p == '\\' && P->p + 1 < P->e && P->p[1] == 'E') { P->p += 2; P->inq = 0; return -2; }
        unsigned cp; P->p += rx_dec(P->p, P->e, &cp);
        return rx_mkchar(P, cp);
    }
    const int c = *P->p;
    int n;
    switch (c) {
        case '(': return rx_group(P);
        case '[': return rx_class(P);
        case '\\': return rx_escape(P);
        case '.': P->p++; n = rx_node(P->re, N_ANY); P->re->nd[n].a = (P->fl & RX_S) != 0; return n;
        case '^': P->p++; n = rx_node(P->re, N_ASSERT); P->re->nd[n].a = (P->fl & RX_M) ? RX_MBOL : RX_BOL; return n;
        case '$': P->p++; n = rx_node(P->re, N_ASSERT); P->re->nd[n].a = (P->fl & RX_M) ? RX_MEOL : RX_EOL; return n;
        case '*': case '+': case '?':
            return rx_fail(P, at, "quantifier with nothing to repeat (escape it with a backslash to match it literally)");
        case '{': {
            long a, b; const unsigned char *q;
            if (rx_brace(P->p, P->e, &a, &b, &q)) return rx_fail(P, at, "quantifier with nothing to repeat (escape it with a backslash to match it literally)");
            P->p++; return rx_mkchar(P, '{');
        }
        default: break;
    }
    unsigned cp; P->p += rx_dec(P->p, P->e, &cp);
    return rx_mkchar(P, cp);
}
static int rx_quant(RxP *P, int a) {
    if (P->inq) {       /* quantificador logo depois de \E vale para o ultimo caractere citado */
        if (P->p + 1 < P->e && P->p[0] == '\\' && P->p[1] == 'E') { P->p += 2; P->inq = 0; }
        else return a;
    }
    rx_skipx(P);
    if (P->p >= P->e) return a;
    const unsigned char *at = P->p;
    long mn, mx; const unsigned char *after = P->p + 1;
    switch (*P->p) {
        case '*': mn = 0; mx = -1; break;
        case '+': mn = 1; mx = -1; break;
        case '?': mn = 0; mx = 1; break;
        case '{': if (!rx_brace(P->p, P->e, &mn, &mx, &after)) return a; break;
        default: return a;
    }
    if (mn > RX_MAXREP || mx > RX_MAXREP) return rx_fail(P, at, "repetition count above 10000");
    if (mx >= 0 && mx < mn) return rx_fail(P, at, "repetition {n,m} with m smaller than n");
    P->p = after;
    int greed = 0;
    if (P->p < P->e && *P->p == '?') { greed = 1; P->p++; }
    else if (P->p < P->e && *P->p == '+') { greed = 2; P->p++; }
    const int n = rx_node(P->re, N_REP);
    P->re->nd[n].kid = a; P->re->nd[n].a = (int)mn; P->re->nd[n].b = (int)mx; P->re->nd[n].c = (unsigned)greed;
    return n;
}
static int rx_seq(RxP *P) {
    const int cat = rx_node(P->re, N_CAT);
    int last = -1;
    for (;;) {
        rx_skipx(P);
        if (P->p >= P->e) break;
        if (!P->inq && (*P->p == '|' || *P->p == ')')) break;
        int a = rx_atom(P);
        if (a == -1) return -1;
        if (a == -2) continue;
        a = rx_quant(P, a);
        if (a < 0) return -1;
        if (last < 0) P->re->nd[cat].kid = a; else P->re->nd[last].next = a;
        last = a;
    }
    return cat;
}
static int rx_alt(RxP *P) {
    const int first = rx_seq(P);
    if (first < 0) return -1;
    if (P->p >= P->e || *P->p != '|') return first;
    const int alt = rx_node(P->re, N_ALT);
    P->re->nd[alt].kid = first;
    int last = first;
    while (P->p < P->e && *P->p == '|') {
        P->p++;
        const int b = rx_seq(P);
        if (b < 0) return -1;
        P->re->nd[last].next = b; last = b;
    }
    return alt;
}

/* tamanho minimo e maximo em pontos de codigo (-1 = sem limite) */
static void rx_len(Rx *re, int ni, long *mn, long *mx) {
    const RxNode n = re->nd[ni];
    long a, b;
    *mn = 0; *mx = 0;
    switch (n.t) {
        case N_CHAR: case N_ANY: case N_CLASS: *mn = *mx = 1; break;
        case N_BREF: *mx = -1; break;
        case N_GROUP: case N_ATOMIC: rx_len(re, n.kid, mn, mx); break;
        case N_CAT:
            for (int k = n.kid; k >= 0; k = re->nd[k].next) {
                rx_len(re, k, &a, &b);
                *mn += a;
                if (*mx >= 0) *mx = b < 0 ? -1 : *mx + b;
            }
            break;
        case N_ALT: {
            int first = 1;
            for (int k = n.kid; k >= 0; k = re->nd[k].next) {
                rx_len(re, k, &a, &b);
                if (first || a < *mn) *mn = a;
                if (first) *mx = b; else if (*mx >= 0) *mx = b < 0 ? -1 : (b > *mx ? b : *mx);
                first = 0;
            }
            break;
        }
        case N_REP:
            rx_len(re, n.kid, &a, &b);
            *mn = a * n.a;
            *mx = n.b < 0 ? (b == 0 ? 0 : -1) : (b < 0 ? -1 : b * n.b);
            break;
        default: break;
    }
    if (*mn > (1L << 30)) *mn = 1L << 30;
    if (*mx > (1L << 30)) *mx = 1L << 30;
}

/* ---- geracao do programa ---- */
static int rx_inst(Rx *re, int op) {
    if (re->np >= RX_MAXPROG) { re->toobig = 1; return 0; }
    if (re->np == re->pgcap) { re->pgcap = re->pgcap ? re->pgcap * 2 : 64; re->pg = (RxInst *)realloc(re->pg, sizeof(RxInst) * (size_t)re->pgcap); }
    memset(&re->pg[re->np], 0, sizeof(RxInst));
    re->pg[re->np].op = (unsigned char)op;
    return re->np++;
}
static void rx_emit(Rx *re, int ni);
static void rx_emit_rep(Rx *re, const RxNode n) {
    const int mn = n.a, mx = n.b, greed = (int)n.c;
    int at = -1, i;
    if (greed == 2) at = rx_inst(re, RX_ATOMIC);
    if (mx < 0) {
        for (i = 0; i < (mn > 0 ? mn - 1 : 0) && !re->toobig; i++) rx_emit(re, n.kid);
        long kmn, kmx;
        rx_len(re, n.kid, &kmn, &kmx);
        const int reg = kmn == 0 ? 2 * re->ngroups + re->nregs++ : -1;
        const int s0 = mn == 0 ? rx_inst(re, RX_SPLIT) : -1;
        const int L = re->np;
        if (reg >= 0) { const int m = rx_inst(re, RX_MARK); re->pg[m].x = reg; }
        rx_emit(re, n.kid);
        const int lp = reg >= 0 ? rx_inst(re, RX_LOOP) : -1;
        if (lp >= 0) re->pg[lp].x = reg;
        const int s1 = rx_inst(re, RX_SPLIT);
        const int ex = re->np;
        if (re->toobig) return;
        re->pg[s1].x = greed == 1 ? ex : L; re->pg[s1].y = greed == 1 ? L : ex;
        if (s0 >= 0) { re->pg[s0].x = greed == 1 ? ex : L; re->pg[s0].y = greed == 1 ? L : ex; }
        if (lp >= 0) re->pg[lp].y = ex;
    } else {
        for (i = 0; i < mn && !re->toobig; i++) rx_emit(re, n.kid);
        const int cnt = mx - mn;
        int *sp = cnt ? (int *)malloc(sizeof(int) * (size_t)cnt) : NULL;
        for (i = 0; i < cnt && !re->toobig; i++) { sp[i] = rx_inst(re, RX_SPLIT); rx_emit(re, n.kid); }
        const int ex = re->np;
        if (!re->toobig) for (i = 0; i < cnt; i++) {
            re->pg[sp[i]].x = greed == 1 ? ex : sp[i] + 1; re->pg[sp[i]].y = greed == 1 ? sp[i] + 1 : ex;
        }
        free(sp);
    }
    if (at >= 0 && !re->toobig) { rx_inst(re, RX_SUCCEED); re->pg[at].x = re->np; }
}
static void rx_emit(Rx *re, int ni) {
    if (re->toobig) return;
    const RxNode n = re->nd[ni];
    int i;
    switch (n.t) {
        case N_CHAR: i = rx_inst(re, RX_CHAR); re->pg[i].c = n.c; re->pg[i].ic = (unsigned char)n.ic; break;
        case N_ANY: rx_inst(re, n.a ? RX_ANYNL : RX_ANY); break;
        case N_CLASS: i = rx_inst(re, RX_CLASS); re->pg[i].x = n.a; re->pg[i].ic = (unsigned char)n.ic; break;
        case N_ASSERT: rx_inst(re, n.a); break;
        case N_BREF: i = rx_inst(re, RX_BREF); re->pg[i].x = n.a; re->pg[i].ic = (unsigned char)n.ic; break;
        case N_CAT: for (int k = n.kid; k >= 0; k = re->nd[k].next) rx_emit(re, k); break;
        case N_GROUP:
            if (n.a >= 0) { i = rx_inst(re, RX_SAVE); re->pg[i].x = 2 * n.a; }
            rx_emit(re, n.kid);
            if (n.a >= 0) { i = rx_inst(re, RX_SAVE); re->pg[i].x = 2 * n.a + 1; }
            break;
        case N_ATOMIC: case N_LOOK:
            i = rx_inst(re, n.t == N_ATOMIC ? RX_ATOMIC : n.a);
            re->pg[i].mn = n.b; re->pg[i].mx = (int)n.c;
            rx_emit(re, n.kid);
            rx_inst(re, RX_SUCCEED);
            if (!re->toobig) re->pg[i].x = re->np;
            break;
        case N_ALT: {
            int nb = 0, k;
            for (k = n.kid; k >= 0; k = re->nd[k].next) nb++;
            int *jm = (int *)malloc(sizeof(int) * (size_t)nb); int nj = 0;
            for (k = n.kid; k >= 0 && !re->toobig; k = re->nd[k].next) {
                if (re->nd[k].next >= 0) {
                    const int s = rx_inst(re, RX_SPLIT);
                    rx_emit(re, k);
                    jm[nj++] = rx_inst(re, RX_JMP);
                    if (!re->toobig) { re->pg[s].x = s + 1; re->pg[s].y = re->np; }
                } else rx_emit(re, k);
            }
            if (!re->toobig) for (k = 0; k < nj; k++) re->pg[jm[k]].x = re->np;
            free(jm);
            break;
        }
        case N_REP: rx_emit_rep(re, n); break;
        default: break;
    }
}

/* ---- otimizacoes: ancora, prefixo e trecho literal, primeiros bytes ---- */
static int rx_anchor_of(Rx *re, int ni) {
    const RxNode n = re->nd[ni];
    switch (n.t) {
        case N_ASSERT: return (n.a == RX_BOL || n.a == RX_BEGIN) ? 1 : n.a == RX_MBOL ? 2 : 0;
        case N_CAT: return n.kid >= 0 ? rx_anchor_of(re, n.kid) : 0;
        case N_GROUP: case N_ATOMIC: return rx_anchor_of(re, n.kid);
        case N_REP: return n.a >= 1 ? rx_anchor_of(re, n.kid) : 0;
        case N_ALT: {
            int a = -1;
            for (int k = n.kid; k >= 0; k = re->nd[k].next) { const int b = rx_anchor_of(re, k); if (a < 0) a = b; else if (a != b) return 0; }
            return a < 0 ? 0 : a;
        }
    }
    return 0;
}
static void rx_lits(Rx *re, int root) {
    if (re->nd[root].t != N_CAT) return;
    char run[64]; int rl = 0, start = 1;
    for (int k = re->nd[root].kid; ; k = re->nd[k].next) {
        int ok = 0, brk = 1; unsigned c = 0;
        if (k >= 0) {
            const RxNode *n = &re->nd[k];
            if (n->t == N_CHAR && !n->ic) { ok = 1; brk = 0; c = n->c; }
            else if (n->t == N_REP && n->a >= 1 && re->nd[n->kid].t == N_CHAR && !re->nd[n->kid].ic) { ok = 1; c = re->nd[n->kid].c; }
        }
        if (ok) { char b[4]; const int l = rx_enc(c, b); if (rl + l < 64) { memcpy(run + rl, b, (size_t)l); rl += l; } else brk = 1; }
        if (brk) {
            if (rl > re->reqlen) { memcpy(re->req, run, (size_t)rl); re->reqlen = rl; }
            if (start && rl) { memcpy(re->pre, run, (size_t)rl); re->prelen = rl; }
            start = 0; rl = 0;
        }
        if (k < 0) break;
    }
}
static void rx_fbc(Rx *re, unsigned c, int ic) {
    char b[4];
    rx_enc(c, b); re->fb[(unsigned char)b[0]] = 1;
    if (ic) {
        rx_enc(rx_upper(c), b); re->fb[(unsigned char)b[0]] = 1;
        for (int i = 0xC0; i < 0x100; i++) re->fb[i] = 1;
    }
}
static void rx_firstbytes(Rx *re) {
    unsigned char *vis = (unsigned char *)calloc((size_t)re->np, 1);
    int *st = (int *)malloc(sizeof(int) * (size_t)(re->np + 1)), sp = 0, nullable = 0;
    st[sp++] = 0;
    while (sp && !nullable) {
        int pc = st[--sp];
        for (;;) {
            if (vis[pc]) break;
            vis[pc] = 1;
            const RxInst *I = &re->pg[pc];
            int stop = 0;
            switch (I->op) {
                case RX_CHAR: rx_fbc(re, I->c, I->ic); stop = 1; break;
                case RX_ANY: for (int b = 0; b < 256; b++) if (b != '\n') re->fb[b] = 1; stop = 1; break;
                case RX_ANYNL: memset(re->fb, 1, 256); stop = 1; break;
                case RX_CLASS:
                    for (unsigned b = 0; b < 0x80; b++) if (rx_cmatch(re, I->x, b, I->ic)) re->fb[b] = 1;
                    for (int b = 0x80; b < 0x100; b++) re->fb[b] = 1;
                    stop = 1; break;
                case RX_SPLIT: st[sp++] = I->y; pc = I->x; continue;
                case RX_JMP: pc = I->x; continue;
                case RX_ATOMIC: st[sp++] = I->x; pc++; continue;
                case RX_LOOK: case RX_NLOOK: case RX_LOOKB: case RX_NLOOKB: pc = I->x; continue;
                case RX_LOOP: st[sp++] = I->y; pc++; continue;
                case RX_SUCCEED: stop = 1; break;
                case RX_MATCH: case RX_BREF: nullable = 1; stop = 1; break;
                default: pc++; continue;
            }
            if (stop) break;
        }
    }
    re->usefb = !nullable;
    free(vis); free(st);
}

static void rx_free(Rx *re) {
    if (!re) return;
    for (int i = 0; i < re->ncl; i++) free(re->cl[i].r);
    free(re->cl); free(re->nd); free(re->pg); free(re->slots); free(re->stk);
    free(re);
}
/* compila; NULL com a mensagem em err se o padrao for invalido */
static Rx *rx_compile(const char *pat, size_t len, int flags, char *err, size_t ecap) {
    Rx *re = (Rx *)calloc(1, sizeof(Rx));
    RxP P; memset(&P, 0, sizeof P);
    char eb[300] = "";
    P.re = re; P.s = P.p = (const unsigned char *)pat; P.e = P.s + len; P.fl = flags & (RX_I | RX_M | RX_S | RX_X);
    P.err = eb; P.ecap = sizeof eb;
    re->ngroups = 1;
    int root;
    if (flags & RX_LITERAL) {
        root = rx_node(re, N_CAT);
        int last = -1;
        while (P.p < P.e) {
            unsigned cp; P.p += rx_dec(P.p, P.e, &cp);
            const int c = rx_mkchar(&P, cp);
            if (last < 0) re->nd[root].kid = c; else re->nd[last].next = c;
            last = c;
        }
    } else {
        root = rx_alt(&P);
        if (root >= 0 && P.p < P.e) root = rx_fail(&P, P.p, "unmatched ) (escape it as \\) to match a parenthesis)");
        if (root >= 0 && re->maxref >= re->ngroups) {
            snprintf(eb, sizeof eb, "backreference \\%d to a group that does not exist (the pattern has %d group%s)", re->maxref, re->ngroups - 1, re->ngroups == 2 ? "" : "s");
            root = -1;
        }
    }
    if (root < 0) { snprintf(err, ecap, "%s", eb[0] ? eb : "invalid pattern"); rx_free(re); return NULL; }
    int i = rx_inst(re, RX_SAVE); re->pg[i].x = 0;
    rx_emit(re, root);
    i = rx_inst(re, RX_SAVE); re->pg[i].x = 1;
    rx_inst(re, RX_MATCH);
    if (re->toobig) { snprintf(err, ecap, "pattern too large (repetition counts too high; at most %d instructions)", RX_MAXPROG); rx_free(re); return NULL; }
    re->nslots = 2 * re->ngroups + re->nregs;
    re->slots = (long *)malloc(sizeof(long) * (size_t)re->nslots);
    re->anchor = rx_anchor_of(re, root);
    rx_lits(re, root);
    rx_firstbytes(re);
    return re;
}

/* ---- execucao ---- */
static int rx_push(Rx *m, int pc, long pos) {
    if (m->sp == m->scap) {
        if (m->scap >= ((size_t)1 << 25)) { m->err = 1; return 0; }
        m->scap = m->scap ? m->scap * 2 : 1024;
        m->stk = (RxBt *)realloc(m->stk, sizeof(RxBt) * m->scap);
    }
    m->stk[m->sp].pc = pc; m->stk[m->sp].pos = pos; m->sp++;
    return 1;
}
static void rx_setslot(Rx *m, int k, long v) { rx_push(m, -1 - k, m->slots[k]); m->slots[k] = v; }
/* depois de um sub-casamento aceito: descarta as alternativas acima de b0, guarda as restauracoes */
static void rx_commit(Rx *m, size_t b0) {
    size_t w = b0;
    for (size_t r = b0; r < m->sp; r++) if (m->stk[r].pc < 0) m->stk[w++] = m->stk[r];
    m->sp = w;
}
static void rx_unwind(Rx *m, size_t b0) {
    while (m->sp > b0) { const RxBt b = m->stk[--m->sp]; if (b.pc < 0) m->slots[-1 - b.pc] = b.pos; }
}
static unsigned rx_prevcp(const Rx *m, long pos) {
    if (pos <= 0) return 0;
    unsigned c; const int l = rx_backlen(m->s, pos);
    rx_dec(m->s + pos - l, m->s + m->n, &c);
    return c;
}
static int rx_wordat(const Rx *m, long pos) {
    if (pos >= m->n) return 0;
    unsigned c; rx_dec(m->s + pos, m->s + m->n, &c);
    return rx_isword(c);
}
static int rx_wordbefore(const Rx *m, long pos) { return pos > 0 && rx_isword(rx_prevcp(m, pos)); }

static long rx_run(Rx *m, int pc, long pos, long target) {
    const size_t base = m->sp;
    const unsigned char *s = m->s;
    const long n = m->n;
    for (;;) {
        if (++m->steps > m->budget) { m->err = 1; return -1; }
        const RxInst *I = &m->pg[pc];
        switch (I->op) {
            case RX_CHAR: {
                if (pos >= n) goto fail;
                if (!I->ic && I->c < 0x80) { if (s[pos] != I->c) goto fail; pos++; pc++; continue; }
                unsigned c; const int l = rx_dec(s + pos, s + n, &c);
                if (I->ic) c = rx_fold(c);
                if (c != I->c) goto fail;
                pos += l; pc++; continue;
            }
            case RX_ANY: case RX_ANYNL: {
                if (pos >= n || (I->op == RX_ANY && s[pos] == '\n')) goto fail;
                unsigned c; pos += rx_dec(s + pos, s + n, &c); pc++; continue;
            }
            case RX_CLASS: {
                if (pos >= n) goto fail;
                unsigned c; const int l = rx_dec(s + pos, s + n, &c);
                if (!rx_cmatch(m, I->x, c, I->ic)) goto fail;
                pos += l; pc++; continue;
            }
            case RX_BOL: case RX_BEGIN: if (pos != 0) goto fail; pc++; continue;
            case RX_MBOL: if (pos != 0 && s[pos - 1] != '\n') goto fail; pc++; continue;
            case RX_END: if (pos != n) goto fail; pc++; continue;
            case RX_EOL: case RX_ENDZ: if (!(pos == n || (pos == n - 1 && s[pos] == '\n'))) goto fail; pc++; continue;
            case RX_MEOL: if (!(pos == n || s[pos] == '\n' || (s[pos] == '\r' && pos + 1 < n && s[pos + 1] == '\n'))) goto fail; pc++; continue;
            case RX_WORDB: case RX_NWORDB: {
                const int b = rx_wordbefore(m, pos) != rx_wordat(m, pos);
                if (b != (I->op == RX_WORDB)) goto fail;
                pc++; continue;
            }
            case RX_WSTART: if (rx_wordbefore(m, pos) || !rx_wordat(m, pos)) goto fail; pc++; continue;
            case RX_WEND: if (!rx_wordbefore(m, pos) || rx_wordat(m, pos)) goto fail; pc++; continue;
            case RX_SPLIT: if (!rx_push(m, I->y, pos)) return -1; pc = I->x; continue;
            case RX_JMP: pc = I->x; continue;
            case RX_SAVE: case RX_MARK: rx_setslot(m, I->x, pos); if (m->err) return -1; pc++; continue;
            case RX_LOOP: pc = pos == m->slots[I->x] ? I->y : pc + 1; continue;
            case RX_BREF: {
                const long a = m->slots[2 * I->x], b = m->slots[2 * I->x + 1];
                if (a < 0 || b < 0) goto fail;
                if (!I->ic) {
                    if (pos + (b - a) > n || memcmp(s + a, s + pos, (size_t)(b - a)) != 0) goto fail;
                    pos += b - a; pc++; continue;
                }
                long p = pos, q = a;
                while (q < b) {
                    if (p >= n) goto fail;
                    unsigned c1, c2; const int l1 = rx_dec(s + q, s + b, &c1), l2 = rx_dec(s + p, s + n, &c2);
                    if (rx_fold(c1) != rx_fold(c2)) goto fail;
                    q += l1; p += l2;
                }
                pos = p; pc++; continue;
            }
            case RX_ATOMIC: case RX_LOOK: case RX_NLOOK: {
                const size_t b0 = m->sp;
                const long r = rx_run(m, pc + 1, pos, -1);
                if (m->err) return -1;
                if (I->op == RX_NLOOK) { if (r >= 0) { rx_unwind(m, b0); goto fail; } pc = I->x; continue; }
                if (r < 0) goto fail;
                rx_commit(m, b0);
                if (I->op == RX_ATOMIC) pos = r;
                pc = I->x; continue;
            }
            case RX_LOOKB: case RX_NLOOKB: {
                const size_t b0 = m->sp;
                long st = pos; int found = 0;
                for (int k = 0; ; k++) {
                    if (k >= I->mn) {
                        const long r = rx_run(m, pc + 1, st, pos);
                        if (m->err) return -1;
                        if (r >= 0) { found = 1; break; }
                    }
                    if (k >= I->mx || st == 0) break;
                    st -= rx_backlen(s, st);
                }
                if (I->op == RX_NLOOKB) { if (found) { rx_unwind(m, b0); goto fail; } pc = I->x; continue; }
                if (!found) goto fail;
                rx_commit(m, b0);
                pc = I->x; continue;
            }
            case RX_SUCCEED: if (target >= 0 && pos != target) goto fail; return pos;
            case RX_MATCH: return pos;
            default: goto fail;
        }
    fail:
        for (;;) {
            if (m->sp <= base) return -1;
            const RxBt b = m->stk[--m->sp];
            if (b.pc < 0) { m->slots[-1 - b.pc] = b.pos; continue; }
            pc = b.pc; pos = b.pos;
            break;
        }
    }
}

/* procura a partir de start em s[0..n). spans (2 por grupo, -1 se vazio) pode ser NULL.
   1 achou, 0 nao achou, -1 passou do limite de passos ou de memoria */
static int rx_search(Rx *re, const char *s, long n, long start, long *spans) {
    re->s = (const unsigned char *)s; re->n = n; re->steps = 0; re->err = 0;
    re->budget = RX_STEPS + 100 * (n - start);
    if (start > n) return 0;
    if (re->reqlen && !rx_memmem(s + start, (size_t)(n - start), re->req, (size_t)re->reqlen)) return 0;
    long st = start;
    while (st <= n) {
        if (re->anchor == 1 && st != 0) return 0;
        if (re->anchor == 2 && st != 0 && s[st - 1] != '\n') {
            const char *nl = (const char *)memchr(s + st, '\n', (size_t)(n - st));
            if (!nl) return 0;
            st = (long)(nl - s) + 1;
            continue;
        }
        if (re->prelen) {
            const char *p = rx_memmem(s + st, (size_t)(n - st), re->pre, (size_t)re->prelen);
            if (!p) return 0;
            st = (long)(p - s);
        } else if (re->usefb && re->anchor == 0) {
            while (st < n && !re->fb[(unsigned char)s[st]]) st++;
            if (st >= n) return 0;
        }
        for (int i = 0; i < re->nslots; i++) re->slots[i] = -1;
        re->sp = 0;
        const long r = rx_run(re, 0, st, -1);
        if (re->err) return -1;
        if (r >= 0) {
            if (spans) for (int i = 0; i < 2 * re->ngroups; i++) spans[i] = re->slots[i];
            return 1;
        }
        if (st >= n) break;
        unsigned c; st += rx_dec((const unsigned char *)s + st, (const unsigned char *)s + n, &c);
    }
    return 0;
}
