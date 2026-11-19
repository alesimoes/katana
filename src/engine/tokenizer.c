/* ================= tokenizador BPE (estilo GPT-2) ================= */

typedef struct { int *ids; int n, cap; } Ids;

static void ids_push(Ids *o, int id) {
    if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 256; o->ids = realloc(o->ids, o->cap * sizeof(int)); if (!o->ids) die("sem memoria"); }
    o->ids[o->n++] = id;
}

/* par candidato do BPE: junta os simbolos i e j (vizinhos); a e b sao os tamanhos no momento em que o par foi avaliado,
   para descartar entradas velhas depois que algum dos lados mudou */
typedef struct { int rank, pos, i, j, a, b; } BpePar;

static int bpe_menor(const BpePar *x, const BpePar *y) { return x->rank < y->rank || (x->rank == y->rank && x->pos < y->pos); }

static void bpe_sobe(BpePar *h, int k) {
    while (k > 0) {
        const int p = (k - 1) / 2;
        if (!bpe_menor(&h[k], &h[p])) break;
        BpePar t = h[k]; h[k] = h[p]; h[p] = t; k = p;
    }
}

static void bpe_desce(BpePar *h, int n, int k) {
    for (;;) {
        int m = k, l = 2 * k + 1, r = l + 1;
        if (l < n && bpe_menor(&h[l], &h[m])) m = l;
        if (r < n && bpe_menor(&h[r], &h[m])) m = r;
        if (m == k) break;
        BpePar t = h[k]; h[k] = h[m]; h[m] = t; k = m;
    }
}

/* aplica BPE em uma "palavra" (bytes crus) e empilha os ids.
   Lista encadeada de simbolos + heap de pares (menor rank; empate = mais a esquerda): O(n log n) em vez de O(n^2),
   que travava em trechos longos sem fronteira (base64, JSON minificado, resultados de ferramenta). */
static void bpe_word(const Tokenizer *tk, const uint8_t *s, int n, Ids *out) {
    char *buf = xalloc((size_t)n * 2 + 1, 1);
    int *st = xalloc(n, sizeof(int)), *ln = xalloc(n, sizeof(int));
    int *nx = xalloc(n, sizeof(int)), *pv = xalloc(n, sizeof(int));
    char *vivo = xalloc(n, 1);
    int bl = 0, ns = 0;

    for (int i = 0; i < n; i++) {   /* um simbolo por caractere unicode (bytes mapeados) */
        int start = bl, cp = tk->byte2cp[s[i]];
        if (cp < 0x80) buf[bl++] = (char)cp;
        else { buf[bl++] = (char)(0xC0 | (cp >> 6)); buf[bl++] = (char)(0x80 | (cp & 0x3F)); }
        /* bytes de continuacao UTF-8 viram simbolos proprios (BPE em nivel de byte) */
        st[ns] = start; ln[ns] = bl - start; nx[ns] = ns + 1 < n ? ns + 1 : -1; pv[ns] = ns - 1; vivo[ns] = 1; ns++;
    }
    int hcap = 3 * n + 4, hn = 0;
    BpePar *h = xalloc((size_t)hcap, sizeof(BpePar));
    char *pair = NULL; size_t pcap = 0;
#define BPE_AVALIA(I, J) do { \
        const int a_ = ln[I], b_ = ln[J]; \
        if ((size_t)(a_ + b_ + 1) > pcap) { pcap = (size_t)(a_ + b_ + 1) * 2; pair = realloc(pair, pcap); if (!pair) die("sem memoria"); } \
        memcpy(pair, buf + st[I], a_); pair[a_] = ' '; memcpy(pair + a_ + 1, buf + st[J], b_); \
        const int rk_ = map_get(&tk->merges, pair, a_ + 1 + b_); \
        if (rk_ >= 0) { h[hn] = (BpePar){ rk_, st[I], I, J, a_, b_ }; bpe_sobe(h, hn); hn++; } \
    } while (0)
    for (int i = 0; i + 1 < n; i++) BPE_AVALIA(i, i + 1);
    while (hn > 0) {
        const BpePar e = h[0];
        h[0] = h[--hn]; bpe_desce(h, hn, 0);
        if (!vivo[e.i] || !vivo[e.j] || nx[e.i] != e.j || ln[e.i] != e.a || ln[e.j] != e.b) continue;   /* entrada velha */
        ln[e.i] += ln[e.j];                 /* junta j em i (os bytes sao contiguos em buf) */
        vivo[e.j] = 0;
        nx[e.i] = nx[e.j];
        if (nx[e.i] >= 0) pv[nx[e.i]] = e.i;
        if (pv[e.i] >= 0) BPE_AVALIA(pv[e.i], e.i);
        if (nx[e.i] >= 0) BPE_AVALIA(e.i, nx[e.i]);
    }
#undef BPE_AVALIA
    for (int i = 0; i < n; i++) {
        if (!vivo[i]) continue;
        int id = map_get(&tk->vocab, buf + st[i], ln[i]);
        ids_push(out, id >= 0 ? id : 0);
    }
    free(buf); free(st); free(ln); free(nx); free(pv); free(vivo); free(h); free(pair);
}

static int c_space(int c)  { return c == ' ' || (c >= 9 && c <= 13); }
static int c_digit(int c)  { return c >= '0' && c <= '9'; }
static int c_letter(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80; }
static int c_other(int c)  { return !c_space(c) && !c_digit(c) && !c_letter(c); }

/* pre-tokenizacao aproximada do regex do GPT-2 (digitos isolados, como no SmolLM); so para vocabularios sem pre-tokenizador conhecido */
static void encode_legacy(const Tokenizer *tk, const uint8_t *s, int n, Ids *out) {
    int i = 0;
    while (i < n) {
        int c = s[i], j, k;
        if (c_digit(c)) {
            const int mx = tk->digits > 1 ? tk->digits : 1;
            for (k = i; k < n && k - i < mx && c_digit(s[k]); k++);
            bpe_word(tk, s + i, k - i, out); i = k; continue;
        }
        if (c == '\'' && i + 1 < n) {   /* contracoes do ingles */
            int len = 0, a = s[i + 1], b = i + 2 < n ? s[i + 2] : 0;
            if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') || (a == 'l' && b == 'l')) len = 3;
            else if (a == 's' || a == 't' || a == 'm' || a == 'd') len = 2;
            if (len) { bpe_word(tk, s + i, len, out); i += len; continue; }
        }
        j = (c == ' ' && i + 1 < n && (c_letter(s[i + 1]) || c_other(s[i + 1]))) ? i + 1 : i;
        if (c_letter(s[j])) {
            for (k = j; k < n && c_letter(s[k]); k++);
            bpe_word(tk, s + i, k - i, out); i = k; continue;
        }
        if (c_other(s[j])) {
            for (k = j; k < n && c_other(s[k]); k++);
            bpe_word(tk, s + i, k - i, out); i = k; continue;
        }
        for (k = i; k < n && c_space(s[k]); k++);
        if (k < n && k - i > 1) k--;    /* o ultimo espaco fica para a proxima palavra */
        bpe_word(tk, s + i, k - i, out); i = k;
    }
}



/* ---- normalizacao Unicode NFC (os vocabularios Qwen e OLMo normalizam o texto antes de tokenizar) ---- */
#include "nfc.h"

static int nfc_ccc(uint32_t c) {
    int lo = 0, hi = NFC_CCC_N - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) >> 1;
        if (c < NFC_CCC[mid][0]) hi = mid - 1;
        else if (c > NFC_CCC[mid][1]) lo = mid + 1;
        else return (int)NFC_CCC[mid][2];
    }
    return 0;
}

static uint32_t nfc_compose(uint32_t a, uint32_t b) {
    if (a >= 0x1100 && a <= 0x1112 && b >= 0x1161 && b <= 0x1175) return 0xAC00 + ((a - 0x1100) * 21 + (b - 0x1161)) * 28;
    if (a >= 0xAC00 && a <= 0xD7A3 && (a - 0xAC00) % 28 == 0 && b >= 0x11A8 && b <= 0x11C2) return a + (b - 0x11A7);
    int lo = 0, hi = NFC_PAIR_N - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) >> 1;
        if (a < NFC_PAIR[mid][0] || (a == NFC_PAIR[mid][0] && b < NFC_PAIR[mid][1])) hi = mid - 1;
        else if (a > NFC_PAIR[mid][0] || b > NFC_PAIR[mid][1]) lo = mid + 1;
        else return NFC_PAIR[mid][2];
    }
    return 0;
}

/* NFC de um texto UTF-8; devolve NULL quando o texto ja esta normalizado de forma trivial (tudo abaixo de U+0300).
   Bytes invalidos passam intactos (viram pontos 0x110000+byte durante o processo). O resultado e alocado. */
static uint8_t *nfc_norm(const uint8_t *s, int n, int *outn) {
    int i, big = 0;
    for (i = 0; i < n; i++) if (s[i] >= 0xCC) { big = 1; break; }       /* U+0300 em diante comeca por 0xCC */
    if (!big) return NULL;
    uint32_t *a = xalloc((size_t)n + 1, sizeof(uint32_t)), *b;
    int na = 0;
    for (i = 0; i < n; ) {
        const uint8_t b0 = s[i];
        int l = b0 < 0x80 ? 1 : (b0 >> 5) == 6 ? 2 : (b0 >> 4) == 14 ? 3 : (b0 >> 3) == 30 ? 4 : 0;
        uint32_t u = l == 1 ? b0 : l == 2 ? b0 & 0x1F : l == 3 ? b0 & 0x0F : b0 & 0x07;
        if (i + l > n) l = 0;
        for (int k = 1; k < l; k++) {
            if ((s[i + k] & 0xC0) != 0x80) { l = 0; break; }
            u = (u << 6) | (s[i + k] & 0x3F);
        }
        if (l == 0) { u = 0x110000u + b0; l = 1; }
        a[na++] = u; i += l;
    }
    /* decomposicao canonica completa */
    uint32_t *d = xalloc((size_t)na * NFC_MAXD + 4, sizeof(uint32_t));
    int nd = 0;
    for (i = 0; i < na; i++) {
        const uint32_t c = a[i];
        int lo = 0, hi = NFC_DEC_N - 1, hit = -1;
        if (c >= 0xC0 && c < 0x110000)
            while (lo <= hi) {
                const int mid = (lo + hi) >> 1;
                if (c < NFC_DEC[mid][0]) hi = mid - 1;
                else if (c > NFC_DEC[mid][0]) lo = mid + 1;
                else { hit = mid; break; }
            }
        if (hit < 0) d[nd++] = c;
        else for (int k = 0; k < (int)NFC_DEC[hit][1]; k++) d[nd++] = NFC_DEC[hit][2 + k];
    }
    /* ordem canonica: dentro de cada corrida de classe > 0, ordena (estavel) pela classe */
    for (i = 1; i < nd; i++) {
        const int cc = nfc_ccc(d[i]);
        if (!cc) continue;
        int j = i;
        while (j > 0 && nfc_ccc(d[j - 1]) > cc) { const uint32_t t = d[j]; d[j] = d[j - 1]; d[j - 1] = t; j--; }
    }
    /* composicao */
    b = xalloc((size_t)nd + 1, sizeof(uint32_t));
    int nb = 0, starter = -1, lastcc = 0;
    for (i = 0; i < nd; i++) {
        const uint32_t c = d[i];
        const int cc = nfc_ccc(c);
        uint32_t p;
        if (starter >= 0 && (nb - 1 == starter || lastcc < cc) && (p = nfc_compose(b[starter], c)) != 0) { b[starter] = p; continue; }
        if (cc == 0) starter = nb;
        b[nb++] = c; lastcc = cc;
    }
    uint8_t *o = xalloc((size_t)nb * 4 + 1, 1);
    int no = 0;
    for (i = 0; i < nb; i++) {
        const uint32_t u = b[i];
        if (u >= 0x110000u) o[no++] = (uint8_t)(u - 0x110000u);
        else if (u < 0x80) o[no++] = (uint8_t)u;
        else if (u < 0x800) { o[no++] = (uint8_t)(0xC0 | (u >> 6)); o[no++] = (uint8_t)(0x80 | (u & 0x3F)); }
        else if (u < 0x10000) { o[no++] = (uint8_t)(0xE0 | (u >> 12)); o[no++] = (uint8_t)(0x80 | ((u >> 6) & 0x3F)); o[no++] = (uint8_t)(0x80 | (u & 0x3F)); }
        else { o[no++] = (uint8_t)(0xF0 | (u >> 18)); o[no++] = (uint8_t)(0x80 | ((u >> 12) & 0x3F)); o[no++] = (uint8_t)(0x80 | ((u >> 6) & 0x3F)); o[no++] = (uint8_t)(0x80 | (u & 0x3F)); }
    }
    free(a); free(d); free(b);
    *outn = no;
    return o;
}

/* ---- propriedades Unicode para os pre-tokenizadores (tabelas de faixas geradas em unitab.h) ---- */
#include "unitab.h"

static int ut_in(const uint32_t (*t)[2], int n, uint32_t c) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) >> 1;
        if (c < t[mid][0]) hi = mid - 1;
        else if (c > t[mid][1]) lo = mid + 1;
        else return 1;
    }
    return 0;
}
static int u_space(uint32_t c) {      /* \s: propriedade White_Space */
    return (c >= 9 && c <= 13) || c == 32 || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A)
        || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
static int u_nl(uint32_t c) { return c == '\r' || c == '\n'; }
static int u_L(uint32_t c) { return c < 0x80 ? (c | 32) >= 'a' && (c | 32) <= 'z' : ut_in(UT_L, UT_L_N, c); }
static int u_N(uint32_t c) { return c < 0x80 ? c >= '0' && c <= '9' : ut_in(UT_N, UT_N_N, c); }
static int u_M(uint32_t c) { return c >= 0x300 && ut_in(UT_M, UT_M_N, c); }
static int u_han(uint32_t c) {        /* Script=Han */
    static const uint32_t h[][2] = { {0x2E80,0x2E99},{0x2E9B,0x2EF3},{0x2F00,0x2FD5},{0x3005,0x3005},{0x3007,0x3007},{0x3021,0x3029},
        {0x3038,0x303B},{0x3400,0x4DBF},{0x4E00,0x9FFF},{0xF900,0xFA6D},{0xFA70,0xFAD9},{0x16FE2,0x16FE3},{0x16FF0,0x16FF1},
        {0x20000,0x2A6DF},{0x2A700,0x2EBE0},{0x2EBF0,0x2EE5D},{0x2F800,0x2FA1D},{0x30000,0x3134A},{0x31350,0x323AF} };
    return c >= 0x2E80 && ut_in(h, (int)(sizeof h / sizeof h[0]), c);
}
static int k_U(uint32_t c) { return ut_in(UT_U, UT_U_N, c) && !u_han(c); }      /* [\p{Lu}\p{Lt}\p{Lm}\p{Lo}\p{M}&&[^\p{Han}]] */
static int k_W(uint32_t c) { return ut_in(UT_W, UT_W_N, c) && !u_han(c); }      /* [\p{Ll}\p{Lm}\p{Lo}\p{M}&&[^\p{Han}]] */

enum { PRE_LEGACY = 0, PRE_QWEN2, PRE_QWEN35, PRE_LLAMA3, PRE_GPT2, PRE_KIMI, PRE_DEEPSEEK, PRE_OLMO, PRE_GPT4O };

static int pre_has_nfc(int kind) { return kind == PRE_QWEN2 || kind == PRE_QWEN35 || kind == PRE_OLMO; }

static int pre_kind(const char *sv, int sl) {
    static const struct { const char *nome; int kind; } t[] = {
        { "qwen2", PRE_QWEN2 }, { "qwen35", PRE_QWEN35 }, { "llama-bpe", PRE_LLAMA3 }, { "glm4", PRE_LLAMA3 }, { "olmo", PRE_OLMO }, { "gpt-2", PRE_GPT2 },
        { "kimi-k2", PRE_KIMI }, { "deepseek-llm", PRE_DEEPSEEK }, { "gpt-4o", PRE_GPT4O } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if ((int)strlen(t[i].nome) == sl && memcmp(sv, t[i].nome, sl) == 0) return t[i].kind;
    return PRE_LEGACY;
}

typedef struct { const uint32_t *c; int n; } CPS;       /* texto decodificado em pontos de codigo */

/* contracao 's 't 're 've 'm 'll 'd em i; devolve o tamanho (em pontos de codigo) ou 0 */
static int contr_len(const CPS *p, int i, int ci) {
    if (i >= p->n || p->c[i] != '\'') return 0;
    uint32_t a = i + 1 < p->n ? p->c[i + 1] : 0, b = i + 2 < p->n ? p->c[i + 2] : 0;
    if (ci) { if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; }
    if (a == 's' || a == 't' || a == 'm' || a == 'd') return 2;
    if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') || (a == 'l' && b == 'l')) return 3;
    return 0;
}

/* alternativas comuns do fim dos regex (digitos, pontuacao, espacos); mflag: \p{M} conta como letra */
static int match_tail(const CPS *p, int i, int mflag, int dig, int nl_alt) {
    const uint32_t *c = p->c; const int n = p->n;
    int k;
    if (u_N(c[i])) { for (k = i + 1; k < n && k - i < dig && u_N(c[k]); k++); return k; }
#define OP(x) (!u_space(x) && !u_L(x) && !u_N(x) && !(mflag && u_M(x)))
    int j = -1;
    if (c[i] == ' ' && i + 1 < n && OP(c[i + 1])) j = i + 1;
    else if (OP(c[i])) j = i;
    if (j >= 0) {
        for (k = j; k < n && OP(c[k]); k++);
        if (nl_alt) while (k < n && u_nl(c[k])) k++;
        return k;
    }
#undef OP
    if (u_space(c[i])) {
        for (j = i; j < n && u_space(c[j]); j++);
        if (nl_alt) for (k = j - 1; k >= i; k--) if (u_nl(c[k])) return k + 1;
        if (j == n) return j;
        if (j - 1 > i) return j - 1;
        return j;
    }
    return i + 1;
}

/* Qwen2/Qwen3.5/Llama 3: (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?[\p{L}(\p{M})]+|\p{N}{1,dig}|... */
static int match_qwen(const CPS *p, int i, int mflag, int dig) {
    const uint32_t *c = p->c; const int n = p->n;
    int k;
#define LM(x) (u_L(x) || (mflag && u_M(x)))
    if (c[i] == '\'' && (k = contr_len(p, i, 1))) return i + k;
    if (!u_nl(c[i]) && !u_L(c[i]) && !u_N(c[i]) && i + 1 < n && LM(c[i + 1])) { for (k = i + 2; k < n && LM(c[k]); k++); return k; }
    if (LM(c[i])) { for (k = i + 1; k < n && LM(c[k]); k++); return k; }
#undef LM
    return match_tail(p, i, mflag, dig, 1);
}

/* GPT-4o / o200k (gpt-oss): a mesma logica do regex collapsado do motor de referencia.
   "caixa alta" = letra/marca que nao e a-z; "caixa baixa" = letra/marca que nao e A-Z (letras sem
   caixa e marcas entram nas duas). alt1: pont? alta* baixa+; alt2: pont? alta+ baixa*; depois numeros
   em grupos de 1-3, pontuacao com \r\n/ ao fim, e espacos (mesmo tratamento do match_tail). */
static int g4o_up(uint32_t c) { return (u_L(c) || u_M(c)) && !(c >= 'a' && c <= 'z'); }
static int g4o_lo(uint32_t c) { return (u_L(c) || u_M(c)) && !(c >= 'A' && c <= 'Z'); }
static int match_gpt4o(const CPS *p, int i) {
    const uint32_t *c = p->c; const int n = p->n;
    int k;
    int s = i, lead = 0;
    if (!u_nl(c[i]) && !u_L(c[i]) && !u_N(c[i]) && !u_M(c[i]) && i + 1 < n && (u_L(c[i + 1]) || u_M(c[i + 1]))) { s = i + 1; lead = 1; }
    if ((lead || u_L(c[i]) || u_M(c[i])) && s < n && (u_L(c[s]) || u_M(c[s]))) {
        k = s;
        while (k < n && g4o_up(c[k])) k++;      /* alta* (sem-caixa/marcas entram) */
        while (k < n && g4o_lo(c[k])) k++;      /* baixa* */
        if (k > s) return k + contr_len(p, k, 1);
    }
    k = match_tail(p, i, 1, 3, 1);
    /* a pontuacao do o200k ainda engole '/' ao fim (alt4: [^\s\p{L}\p{N}]+[\r\n/]*) */
    if (k > i && !u_space(c[i]) && !u_L(c[i]) && !u_N(c[i]) && !(u_M(c[i])))
        while (k < n && c[k] == '/') k++;
    return k;
}

/* GPT-2 (OLMo): 's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+ */
static int match_gpt2(const CPS *p, int i) {
    const uint32_t *c = p->c; const int n = p->n;
    int k, j = (c[i] == ' ') ? i + 1 : i;
    if ((k = contr_len(p, i, 0))) return i + k;
    if (j < n && u_L(c[j])) { for (k = j + 1; k < n && u_L(c[k]); k++); return k; }
    if (j < n && u_N(c[j])) { for (k = j + 1; k < n && u_N(c[k]); k++); return k; }
    if (j < n && !u_space(c[j]) && !u_L(c[j]) && !u_N(c[j])) { for (k = j + 1; k < n && !u_space(c[k]) && !u_L(c[k]) && !u_N(c[k]); k++); return k; }
    if (u_space(c[i])) {
        for (j = i; j < n && u_space(c[j]); j++);
        if (j == n) return j;
        if (j - 1 > i) return j - 1;
        return j;
    }
    return i + 1;
}

/* Kimi: [\p{Han}]+ | prefixo? [U]*[W]+ contracao? | prefixo? [U]+[W]* contracao? | \p{N}{1,3} | ... */
static int match_kimi(const CPS *p, int i) {
    const uint32_t *c = p->c; const int n = p->n;
    int k;
    if (u_han(c[i])) { for (k = i + 1; k < n && u_han(c[k]); k++); return k; }
    for (int alt = 2; alt <= 3; alt++)
        for (int pref = 1; pref >= 0; pref--) {
            int s = i;
            if (pref) { if (u_nl(c[i]) || u_L(c[i]) || u_N(c[i])) continue; s = i + 1; }
            int r = s;
            while (r < n && k_U(c[r])) r++;
            if (alt == 2) {
                for (k = r; k >= s; k--) if (k < n && k_W(c[k])) {
                    int e = k + 1;
                    while (e < n && k_W(c[e])) e++;
                    return e + contr_len(p, e, 1);
                }
            } else if (r > s) {
                int e = r;
                while (e < n && k_W(c[e])) e++;
                return e + contr_len(p, e, 1);
            }
        }
    return match_tail(p, i, 0, 3, 1);
}

/* DeepSeek: sequencia de divisoes isoladas (\r\n, letras com \s? antes, pontuacao com \s? antes, \s+$, CJK, um digito por
   peca); cada passo refina as pecas do anterior. bd[k] = 1 quando uma peca comeca no ponto de codigo k. */
static int ds_punct(uint32_t c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '~') || (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF5E)
        || (c >= 0x2018 && c <= 0x201F) || (c >= 0x3000 && c <= 0x3002);
}
static int ds_letter(uint32_t c) { return ut_in(UT_DS, UT_DS_N, c); }
static int ds_cjk(uint32_t c) { return (c >= 0x4E00 && c <= 0x9FA5) || (c >= 0x0800 && c <= 0x4E00) || (c >= 0xAC00 && c <= 0xD7FF); }

/* casamento do passo st na posicao i dentro da peca [a,b); devolve o fim ou -1 */
static int ds_match(const CPS *p, int st, int i, int b) {
    const uint32_t *c = p->c;
    int k;
    switch (st) {
    case 0: return u_nl(c[i]) ? i + 1 : -1;
    case 1: case 2: {
        int (*cls)(uint32_t) = st == 1 ? ds_letter : ds_punct;
        int j = (u_space(c[i]) && i + 1 < b && cls(c[i + 1])) ? i + 1 : i;
        if (!cls(c[j])) return -1;
        for (k = j + 1; k < b && cls(c[k]); k++);
        return k;
    }
    case 3:
        if (!u_space(c[i])) return -1;
        for (k = i; k < b && u_space(c[k]); k++);
        return k == b ? b : -1;
    case 4:
        if (!ds_cjk(c[i])) return -1;
        for (k = i + 1; k < b && ds_cjk(c[k]); k++);
        return k;
    default: return u_N(c[i]) ? i + 1 : -1;
    }
}

/* pre-tokeniza e aplica BPE em cada peca */
static void encode_pre(const Tokenizer *tk, const uint8_t *s, int n, Ids *out) {
    uint32_t *cp = xalloc((size_t)n + 1, sizeof(uint32_t));
    int *bo = xalloc((size_t)n + 2, sizeof(int));
    int nc = 0;
    for (int i = 0; i < n; ) {      /* UTF-8 -> pontos de codigo; byte invalido vira U+FFFD de 1 byte */
        const uint8_t b0 = s[i];
        int l = b0 < 0x80 ? 1 : (b0 >> 5) == 6 ? 2 : (b0 >> 4) == 14 ? 3 : (b0 >> 3) == 30 ? 4 : 0;
        uint32_t u = l == 1 ? b0 : l == 2 ? b0 & 0x1F : l == 3 ? b0 & 0x0F : b0 & 0x07;
        if (l == 0 || i + l > n) l = 0;
        for (int k = 1; k < l; k++) {
            if ((s[i + k] & 0xC0) != 0x80) { l = 0; break; }
            u = (u << 6) | (s[i + k] & 0x3F);
        }
        if (l == 0) { u = 0xFFFD; l = 1; }
        bo[nc] = i; cp[nc++] = u; i += l;
    }
    bo[nc] = n;
    CPS p = { cp, nc };
    if (tk->pre == PRE_DEEPSEEK) {
        uint8_t *bd = xalloc((size_t)nc + 2, 1);
        bd[0] = 1; bd[nc] = 1;
        for (int st = 0; st < 6; st++) {
            for (int a = 0; a < nc; ) {
                int b = a + 1;
                while (b < nc && !bd[b]) b++;
                for (int i = a; i < b; ) {
                    int e = ds_match(&p, st, i, b);
                    if (e > i) { bd[i] = 1; bd[e] = 1; i = e; }
                    else if (st == 3 && u_space(cp[i])) { int k = i; while (k < b && u_space(cp[k])) k++; i = k; }
                    else i++;
                }
                a = b;
            }
        }
        for (int a = 0; a < nc; ) {
            int b = a + 1;
            while (b < nc && !bd[b]) b++;
            bpe_word(tk, s + bo[a], bo[b] - bo[a], out);
            a = b;
        }
        free(bd);
    } else {
        for (int i = 0; i < nc; ) {
            int e;
            switch (tk->pre) {
            case PRE_QWEN2:  e = match_qwen(&p, i, 0, 1); break;
            case PRE_QWEN35: e = match_qwen(&p, i, 1, 1); break;
            case PRE_LLAMA3: e = match_qwen(&p, i, 0, 3); break;
            case PRE_GPT4O:  e = match_gpt4o(&p, i); break;
            case PRE_KIMI:   e = match_kimi(&p, i); break;
            default:         e = match_gpt2(&p, i); break;
            }
            if (e <= i) e = i + 1;
            bpe_word(tk, s + bo[i], bo[e] - bo[i], out);
            i = e;
        }
    }
    free(cp); free(bo);
}

static void encode_plain(const Tokenizer *tk, const uint8_t *s, int n, Ids *out) {
    if (tk->nfc) {
        int m = 0;
        uint8_t *t = nfc_norm(s, n, &m);
        if (t) { encode_pre(tk, t, m, out); free(t); return; }
    }
    if (tk->pre == PRE_LEGACY) encode_legacy(tk, s, n, out);
    else encode_pre(tk, s, n, out);
}

/* SentencePiece: espacos viram U+2581; parte de um simbolo por caractere e funde, a cada
   passo, o par vizinho cujo token resultante existe e tem a maior pontuacao. O que sobrar
   fora do vocabulario sai como tokens de byte (<0xNN>). */
static void spm_encode(const Tokenizer *tk, const uint8_t *s, int n, Ids *out, int prefix) {
    char *buf = xalloc((size_t)n * 3 + 4, 1);
    int bl = 0, ns = 0;
    if (prefix) { buf[bl++] = (char)0xE2; buf[bl++] = (char)0x96; buf[bl++] = (char)0x81; }
    for (int i = 0; i < n; i++) {
        if (s[i] == ' ') { buf[bl++] = (char)0xE2; buf[bl++] = (char)0x96; buf[bl++] = (char)0x81; }
        else buf[bl++] = (char)s[i];
    }
    int *st = xalloc(bl + 1, sizeof(int)), *ln = xalloc(bl + 1, sizeof(int));
    for (int i = 0; i < bl; ) {         /* um simbolo por caractere UTF-8 */
        const uint8_t ch = (uint8_t)buf[i];
        int l = ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4;
        if (i + l > bl) l = bl - i;
        st[ns] = i; ln[ns] = l; ns++; i += l;
    }
    for (;;) {
        int best = -1; float bscore = -1e30f;
        for (int i = 0; i + 1 < ns; i++) {
            const int id = map_get(&tk->vocab, buf + st[i], ln[i] + ln[i + 1]);
            if (id >= 0 && tk->score[id] > bscore) { bscore = tk->score[id]; best = i; }
        }
        if (best < 0) break;
        ln[best] += ln[best + 1];
        for (int i = best + 1; i + 1 < ns; i++) { st[i] = st[i + 1]; ln[i] = ln[i + 1]; }
        ns--;
    }
    for (int i = 0; i < ns; i++) {
        const int id = map_get(&tk->vocab, buf + st[i], ln[i]);
        if (id >= 0) { ids_push(out, id); continue; }
        for (int j = 0; j < ln[i]; j++) {
            char bt[8];
            snprintf(bt, sizeof bt, "<0x%02X>", (uint8_t)buf[st[i] + j]);
            const int bid = map_get(&tk->vocab, bt, 6);
            ids_push(out, bid >= 0 ? bid : 0);
        }
    }
    free(buf); free(st); free(ln);
}

/* gemma4: espacos viram U+2581 (sem prefixo); o texto e cortado em sequencias de quebras de linha e de
   nao-quebras. Uma sequencia so de quebras que exista no vocabulario vira um token; o resto passa por
   BPE sobre os caracteres UTF-8 (sem o mapeamento de bytes do GPT-2), fundindo a cada passo o par
   vizinho de menor posicao na lista de fusoes. O que sobrar fora do vocabulario sai em bytes <0xNN>. */
static void g4_encode(const Tokenizer *tk, const uint8_t *s, int n, Ids *out) {
    char *buf = xalloc((size_t)n * 3 + 4, 1);
    int bl = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == ' ') { buf[bl++] = (char)0xE2; buf[bl++] = (char)0x96; buf[bl++] = (char)0x81; }
        else buf[bl++] = (char)s[i];
    }
    int *st = xalloc(bl + 1, sizeof(int)), *ln = xalloc(bl + 1, sizeof(int));
    char *key = NULL; size_t kcap = 0;
    for (int a = 0; a < bl; ) {
        const int nl = buf[a] == '\n';
        int b = a;
        while (b < bl && (buf[b] == '\n') == nl) b++;
        const int whole = nl ? map_get(&tk->vocab, buf + a, b - a) : -1;
        if (whole >= 0) { ids_push(out, whole); a = b; continue; }
        int ns = 0;
        for (int i = a; i < b; ) {
            const uint8_t ch = (uint8_t)buf[i];
            int l = ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4;
            if (i + l > b) l = b - i;
            st[ns] = i; ln[ns] = l; ns++; i += l;
        }
        for (;;) {
            int best = -1, brank = 0x7fffffff;
            for (int i = 0; i + 1 < ns; i++) {
                const size_t kl = (size_t)ln[i] + 1 + ln[i + 1];
                if (kl > kcap) { kcap = kl * 2; key = realloc(key, kcap); if (!key) die("sem memoria"); }
                memcpy(key, buf + st[i], ln[i]); key[ln[i]] = ' '; memcpy(key + ln[i] + 1, buf + st[i + 1], ln[i + 1]);
                const int r = map_get(&tk->merges, key, (int)kl);
                if (r >= 0 && r < brank) { brank = r; best = i; }
            }
            if (best < 0) break;
            ln[best] += ln[best + 1];
            for (int i = best + 1; i + 1 < ns; i++) { st[i] = st[i + 1]; ln[i] = ln[i + 1]; }
            ns--;
        }
        for (int i = 0; i < ns; i++) {
            const int id = map_get(&tk->vocab, buf + st[i], ln[i]);
            if (id >= 0) { ids_push(out, id); continue; }
            for (int j = 0; j < ln[i]; j++) {
                char bt[8];
                snprintf(bt, sizeof bt, "<0x%02X>", (uint8_t)buf[st[i] + j]);
                const int bid = map_get(&tk->vocab, bt, 6);
                ids_push(out, bid >= 0 ? bid : 0);
            }
        }
        a = b;
    }
    free(key); free(buf); free(st); free(ln);
}

/* codifica texto, reconhecendo tokens especiais (ex.: <|im_start|>, </s>) */
static void encode(const Tokenizer *tk, const char *text, Ids *out) {
    const uint8_t *s = (const uint8_t *)text;
    int n = (int)strlen(text), start = 0, i = 0, prev_special = 1;
    while (i <= n) {
        int hit = -1;
        if (i < n && tk->spec1[s[i]]) {
            for (int j = 0; j < tk->n_special; j++) {
                int id = tk->special[j], l = tk->toklen[id];
                if (l <= n - i && memcmp(s + i, tk->tok[id], l) == 0 && (hit < 0 || l > tk->toklen[hit])) hit = id;
            }
        }
        if (hit >= 0 || i == n) {
            if (i > start) {
                if (tk->spm == 2) g4_encode(tk, s + start, i - start, out);
                else if (tk->spm) spm_encode(tk, s + start, i - start, out, prev_special);
                else encode_plain(tk, s + start, i - start, out);
                prev_special = 0;
            }
            if (hit < 0) break;
            ids_push(out, hit);
            prev_special = 1;
            i += tk->toklen[hit]; start = i;
        } else i++;
    }
}

/* escreve os bytes originais de um token */
static void print_token(const Tokenizer *tk, int id, FILE *f) {
    const uint8_t *s = (const uint8_t *)tk->tok[id];
    int n = tk->toklen[id], i = 0;
    if (tk->spm) {
        if (tk->toktype && tk->toktype[id] == 6 && n == 6 && s[0] == '<') {     /* token de byte <0xNN> */
            fputc((int)strtol((const char *)s + 3, NULL, 16), f);
            return;
        }
        while (i < n) {
            if (i + 2 < n && s[i] == 0xE2 && s[i + 1] == 0x96 && s[i + 2] == 0x81) { fputc(' ', f); i += 3; }
            else fputc(s[i++], f);
        }
        return;
    }
    while (i < n) {
        int cp = s[i], adv = 1;
        if (cp >= 0xC0 && cp < 0xE0 && i + 1 < n) { cp = ((cp & 0x1F) << 6) | (s[i + 1] & 0x3F); adv = 2; }
        if (adv <= 2 && cp < 512 && tk->cp2byte[cp] >= 0) fputc(tk->cp2byte[cp], f);
        else fwrite(s + i, 1, adv, f);
        i += adv;
    }
}

