/* ---------- katana toktest <modelo.gguf> [--root pasta]: auditoria do tokenizador (so metadados e vocabulario, sem pesos) ----------
 * 1) casos dourados do pre-tokenizador (qwen35 e llama-bpe/glm4): pedacos esperados, derivados a mao das expressoes regulares
 *    publicadas no tokenizer.json oficial de cada modelo (a origem de cada caso esta na propria linha);
 * 2) casos dourados de tokens (vocabulario do proprio GGUF): especiais, byte fallback, prefixo de espaco do gemma4, NFC, BOS;
 * 3) ida e volta (encode -> decode == texto) num corpus: docs (.md), src (.c, recursivo), README e textos escritos aqui (portugues com
 *    acentos, emoji, CJK, espacos e tabs estranhos, linhas longas), com contagem de tokens e velocidade;
 * 4) conferencias cruzadas: ignore_merges ligado x desligado (glm4/llama-bpe), BPE de heap x versao lenta e x sem corte por
 *    quebra de linha (gemma4): o numero de casos que mudam em relacao ao comportamento anterior.
 * Sem o primeiro argumento de arquivo de casos; com `katana toktest <gguf> <casos.hex> [--chat]` continua o teste dourado
 * contra a referencia externa (tools/tokref). */
#include <time.h>
#include <dirent.h>

#define TT_S "\x01"                       /* separador de pedacos nos dourados */
#define TT_SP "\xc4\xa0"                  /* U+0120: o espaco do mapeamento de bytes do GPT-2 */
#define TT_NL "\xc4\x8a"                  /* U+010A: a quebra de linha */
#define TT_U2 "\xe2\x96\x81"              /* U+2581: o espaco do gemma4 */

static int g_tt_lens[1 << 16], g_tt_nl;
static void tt_trace(int len) { if (g_tt_nl < (int)(sizeof g_tt_lens / sizeof g_tt_lens[0])) g_tt_lens[g_tt_nl++] = len; }

typedef struct { int fam; const char *text, *pieces, *why; } TtPre;      /* fam: 1 = qwen35, 2 = llama-bpe/glm4 (soma = ambos) */
/* regex qwen35 (Qwen3.6, tokenizer.json, 2026-11-26): (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?[\p{L}\p{M}]+|\p{N}| ?[^\s\p{L}\p{M}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+
   regex glm4 (GLM-4.7-Flash, idem): (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}{1,3}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+
   Cada caso: primeira alternativa (da esquerda) que casa na posicao, com retrocesso do `\s*[\r\n]+` e do `\s+(?!\S)`. */
static const TtPre TT_PRE[] = {
    { 3, "Hello world", "Hello" TT_S " world", "alt2: letras; ' world' = prefixo espaco + letras" },
    { 3, "Hello, world!", "Hello" TT_S "," TT_S " world" TT_S "!", "alt2 sem prefixo antes de ','; alt4 pontuacao; '!' final" },
    { 3, "don't", "don" TT_S "'t", "alt1 contracao 't" },
    { 3, "DON'T", "DON" TT_S "'T", "alt1 e (?i:): contracao sem diferenca de caixa" },
    { 3, "I'm sure they'll go", "I" TT_S "'m" TT_S " sure" TT_S " they" TT_S "'ll" TT_S " go", "alt1 'm e 'll" },
    { 3, "rock'n'roll", "rock" TT_S "'n" TT_S "'roll", "'n e 'ro nao sao contracao: alt2 com o apostrofo de prefixo" },
    { 3, "it\xe2\x80\x99s", "it" TT_S "\xe2\x80\x99s", "alt1 so aceita o apostrofo ASCII; U+2019 vira prefixo da alt2" },
    { 1, "12345", "1" TT_S "2" TT_S "3" TT_S "4" TT_S "5", "qwen35: \\p{N} um digito por pedaco" },
    { 2, "12345", "123" TT_S "45", "glm4: \\p{N}{1,3}, grupos de ate 3" },
    { 1, "x=3.14", "x" TT_S "=" TT_S "3" TT_S "." TT_S "1" TT_S "4", "qwen35: '=' e '.' pela alt4; digitos soltos" },
    { 2, "x=3.14", "x" TT_S "=" TT_S "3" TT_S "." TT_S "14", "glm4: '.' alt4; '14' grupo" },
    { 1, "1,234.5", "1" TT_S "," TT_S "2" TT_S "3" TT_S "4" TT_S "." TT_S "5", "qwen35: um digito por pedaco" },
    { 2, "1,234.5", "1" TT_S "," TT_S "234" TT_S "." TT_S "5", "glm4: grupos de 3" },
    { 1, "abc123def", "abc" TT_S "1" TT_S "2" TT_S "3" TT_S "def", "digitos separam letras" },
    { 2, "abc123def", "abc" TT_S "123" TT_S "def", "digitos separam letras (grupo de 3)" },
    { 3, "3 4", "3" TT_S " " TT_S "4", "digito nao leva espaco: ' ' cai na alt7 (alt6 falha: seguido de \\S)" },
    { 3, "a  b", "a" TT_S " " TT_S " b", "alt6 \\s+(?!\\S) recua um: o ultimo espaco fica para ' b'" },
    { 3, "a  ", "a" TT_S "  ", "alt6 no fim do texto leva todos os espacos" },
    { 3, "  indent", " " TT_S " indent", "alt6 recua um espaco" },
    { 3, "a\n\nb", "a" TT_S "\n\n" TT_S "b", "alt5: quebras de linha juntas" },
    { 3, "a \nb", "a" TT_S " \n" TT_S "b", "alt5: \\s* recua ate a ultima quebra" },
    { 3, "end.\nNext", "end" TT_S ".\n" TT_S "Next", "alt4: [\\r\\n]* depois da pontuacao" },
    { 3, "a\r\nb", "a" TT_S "\r\n" TT_S "b", "alt5 com \\r\\n" },
    { 3, "foo\n  bar", "foo" TT_S "\n" TT_S " " TT_S " bar", "alt5 so a quebra; os 2 espacos: alt6 recua um" },
    { 3, "a  \n  b", "a" TT_S "  \n" TT_S " " TT_S " b", "alt5 leva os espacos antes da quebra" },
    { 3, "!!!\n\n", "!!!\n\n", "alt4 com [\\r\\n]* final" },
    { 3, "a\n \nb", "a" TT_S "\n \n" TT_S "b", "alt5: \\s* leva '\\n ' e a quebra final" },
    { 3, "x\n\n\ny", "x" TT_S "\n\n\n" TT_S "y", "alt5 com tres quebras" },
    { 3, "a\tb", "a" TT_S "\tb", "TAB e prefixo valido da alt2" },
    { 3, "a\xe3\x80\x80" "b", "a" TT_S "\xe3\x80\x80" "b", "U+3000 e \\s mas entra como prefixo da alt2" },
    { 3, "a\xc2\xa0 b", "a" TT_S "\xc2\xa0" TT_S " b", "NBSP e \\s: alt6 recua e deixa ' b'" },
    { 3, "(hello)", "(hello" TT_S ")", "alt2 com '(' de prefixo" },
    { 3, "_foo_bar", "_foo" TT_S "_bar", "'_' nao e letra nem numero: prefixo da alt2" },
    { 3, "#include <stdio.h>", "#include" TT_S " <" TT_S "stdio" TT_S ".h" TT_S ">", "prefixo '#', ' <' pela alt4, '.h' prefixo" },
    { 3, "A\xc3\xa7\xc3\xa3o \xc3\xa9 n\xc3\xa3o", "A\xc3\xa7\xc3\xa3o" TT_S " \xc3\xa9" TT_S " n\xc3\xa3o", "letras acentuadas sao \\p{L}" },
    { 3, "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c", "\xe4\xbd\xa0\xe5\xa5\xbd" TT_S "\xef\xbc\x8c" "\xe4\xb8\x96\xe7\x95\x8c", "Han e \\p{L}; U+FF0C (Po) e prefixo valido da alt2: ',世界' e um pedaco" },
    { 3, "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82, \xd0\xbc\xd0\xb8\xd1\x80", "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82" TT_S "," TT_S " \xd0\xbc\xd0\xb8\xd1\x80", "cirilico e \\p{L}" },
    { 3, "Hi \xf0\x9f\x98\x80!", "Hi" TT_S " \xf0\x9f\x98\x80!", "emoji (So) e '!' formam um so pedaco da alt4 com o espaco" },
    { 3, "!\xcc\x81", "!\xcc\x81", "'!' e prefixo da alt2 com a marca (qwen35: \\p{M} conta como letra); no glm4 a alt4 leva a marca" },
    { 1, "!!\xcc\x81", "!!" TT_S "\xcc\x81", "qwen35: \\p{M} fora da classe da alt4; a marca vira pedaco pela alt2" },
    { 2, "!!\xcc\x81", "!!\xcc\x81", "glm4: \\p{M} dentro da classe da alt4" },
    { 2, "a\xcc\x81", "a" TT_S "\xcc\x81", "glm4: \\p{L}+ nao leva a marca; alt4 pega a marca (sem NFC)" },
    { 1, "a\xcc\x81", "\xc3\xa1", "qwen35: normalizador NFC compoe 'a'+U+0301 em U+00E1" },
    { 1, "a\xe3\x80\x82", "a" TT_S "\xe3\x80\x82", "U+3002 e pontuacao" },
    { 1, "a\xc2\xbd" "b", "a" TT_S "\xc2\xbd" TT_S "b", "U+00BD e \\p{N} (No): um pedaco, sem unir as letras" },
    { 2, "a\xc2\xbd" "b", "a" TT_S "\xc2\xbd" TT_S "b", "U+00BD e \\p{N} (No)" },
    { 1, "\xd9\xa3\xd9\xa4", "\xd9\xa3" TT_S "\xd9\xa4", "qwen35: digitos arabes (Nd) um por pedaco" },
    { 2, "\xd9\xa3\xd9\xa4", "\xd9\xa3\xd9\xa4", "glm4: digitos arabes (Nd) em grupo" },
    { 3, "\n", "\n", "so quebra de linha" },
    { 3, "", "", "texto vazio" },
};

typedef struct { int fam; const char *text, *toks, *why; } TtTok;       /* fam: 1 qwen35, 2 glm4, 4 gemma4; toks = strings do vocabulario */
static const TtTok TT_TOK[] = {
    { 3, "Hello world", "Hello" TT_S TT_SP "world", "palavras comuns sao um token (vocabulario)" },
    { 3, "Hello, world!", "Hello" TT_S "," TT_S TT_SP "world" TT_S "!", "pontuacao separada" },
    { 3, "a\nb", "a" TT_S TT_NL TT_S "b", "quebra de linha = U+010A" },
    { 1, "<|im_start|>user\n", "<|im_start|>" TT_S "user" TT_S TT_NL, "especial reconhecido no texto (tokenizer.json, id 248045)" },
    { 1, "a<|im_end|>b", "a" TT_S "<|im_end|>" TT_S "b", "especial no meio do texto" },
    { 1, "<think>\n", "<think>" TT_S TT_NL, "<think> e token adicionado (nao-especial) mas casa no texto" },
    { 1, "<|im_start", "<" TT_S "|" TT_S "im" TT_S "_start", "especial incompleto e texto comum" },
    { 2, "<|user|>hi", "<|user|>" TT_S "hi", "especial do GLM (id 154827)" },
    { 2, "[gMASK]<sop>", "[gMASK]" TT_S "<sop>", "especiais iniciais do template do GLM (154822 e 154824)" },
    { 2, "a</think>b", "a" TT_S "</think>" TT_S "b", "</think> do GLM (154842)" },
    { 4, "Hello world", "Hello" TT_S TT_U2 "world", "gemma4: sem prefixo de espaco (add_space_prefix=false); espaco = U+2581" },
    { 4, " Hello", TT_U2 "Hello", "espaco inicial e preservado como U+2581 e funde com a palavra" },
    { 4, "x\ny", "x" TT_S "\n" TT_S "y", "quebra de linha e um token proprio" },
    { 4, "\n\n\n", "\n\n\n", "sequencia de quebras que existe no vocabulario = um token" },
    { 4, "\x01", "\x01", "o controle U+0001 existe no vocabulario como token proprio (nao precisa de byte fallback)" },
    { 4, "\xf4\x8f\xbf\xbd", "<0xF4>" TT_S "<0x8F>" TT_S "<0xBF>" TT_S "<0xBD>", "byte fallback (byte_fallback=true no tokenizer.json): U+10FFFD fora do vocabulario vira 4 tokens <0xNN>" },
    { 4, "<|turn>user\n", "<|turn>" TT_S "user" TT_S "\n", "especial do gemma4 (id 105)" },
    { 4, "<eos>", "<eos>", "tokenizer.json marca <eos> como adicionado; o GGUF a marca como comum (corrigido)" },
    { 4, "<bos>hi", "<bos>" TT_S "hi", "<bos> no texto e o token 2" },
};

static void tt_show(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = (uint8_t)s[i];
        if (c == 1) printf("|");
        else if (c == '\n') printf("\\n");
        else if (c == '\r') printf("\\r");
        else if (c == '\t') printf("\\t");
        else if (c < 32 || c == 127) printf("\\x%02x", c);
        else putchar(c);
    }
}

static int tt_ids_eq(const Ids *a, const Ids *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) if (a->ids[i] != b->ids[i]) return 0;
    return 1;
}

static void tt_decode(const Tokenizer *tk, const Ids *ids, Buf *o) {
    char tmp[2048];
    for (int i = 0; i < ids->n; i++) buf_add(o, tmp, tok_bytes(tk, ids->ids[i], tmp, sizeof tmp));
}

/* BPE do gemma4 sem o corte por quebras de linha (so a normalizacao de espacos): o tokenizer.json oficial nao corta */
static void tt_g4_uncut(const Tokenizer *tk, const uint8_t *s, int n, Ids *out) {
    char *buf = xalloc((size_t)n * 3 + 4, 1);
    int bl = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == ' ') { buf[bl++] = (char)0xE2; buf[bl++] = (char)0x96; buf[bl++] = (char)0x81; }
        else buf[bl++] = (char)s[i];
    }
    if (bl) bpe_sym(tk, (const uint8_t *)buf, bl, out, 1);
    free(buf);
}

typedef struct {
    long cases, bytes, tokens, rt_fail, rt_nfc, rt_skip, im_diff, g4_ref_diff, g4_cut_diff, g4_ref_skip;
    double secs;
} TtStats;

static void tt_case(const Model *m, const char *name, const char *text, size_t n, TtStats *st, int verbose) {
    const Tokenizer *tk = &m->tk;
    Ids ids = {0};
    const clock_t t0 = clock();
    encode(tk, text, &ids);
    st->secs += (double)(clock() - t0) / CLOCKS_PER_SEC;
    st->cases++; st->bytes += (long)n; st->tokens += ids.n;
    /* ida e volta */
    Buf d = {0};
    tt_decode(tk, &ids, &d);
    int same = d.n == n && (n == 0 || memcmp(d.p, text, n) == 0);
    if (!same) {
        int skip = 0, nfc = 0;
        if (tk->nfc) { int nn = 0; uint8_t *t = nfc_norm((const uint8_t *)text, (int)n, &nn); nfc = t && (size_t)nn == d.n && memcmp(t, d.p, d.n) == 0; free(t); }
        if (tk->spm == 2 && strstr(text, TT_U2)) skip = 1;          /* o texto ja traz U+2581: decodifica como espaco */
        if (nfc) st->rt_nfc++;
        else if (skip) st->rt_skip++;
        else { st->rt_fail++; printf("  FALHA ida e volta: %s (%zu bytes)\n", name, n); }
    }
    free(d.p);
    /* ignore_merges ligado x desligado */
    if (tk->pre == PRE_LLAMA3) {
        Tokenizer t2 = *tk; t2.ignore_merges = !tk->ignore_merges;
        Ids o = {0};
        encode(&t2, text, &o);
        if (!tt_ids_eq(&ids, &o)) { st->im_diff++; if (verbose) printf("  ignore_merges muda %s (%d x %d tokens)\n", name, ids.n, o.n); }
        free(o.ids);
    }
    if (tk->spm == 2) {
        int run = 0, longest = 0;
        for (size_t i = 0; i < n; i++) { run = text[i] == '\n' ? 0 : run + 1; if (run > longest) longest = run; }
        if (longest <= 3000) {
            Ids a = {0}, b = {0}, h = {0};
            g4_encode_ref(tk, (const uint8_t *)text, (int)n, &a);
            g4_encode(tk, (const uint8_t *)text, (int)n, &h);       /* sem tokens especiais: o mesmo caminho das duas outras */
            if (!tt_ids_eq(&h, &a)) {
                st->g4_ref_diff++;
                if (verbose) {
                    int k = 0;
                    while (k < h.n && k < a.n && h.ids[k] == a.ids[k]) k++;
                    printf("  g4: heap difere da versao lenta em %s: token %d, heap '", name, k);
                    if (k < h.n) tt_show(tk->tok[h.ids[k]], (size_t)tk->toklen[h.ids[k]]);
                    printf("' lenta '");
                    if (k < a.n) tt_show(tk->tok[a.ids[k]], (size_t)tk->toklen[a.ids[k]]);
                    printf("'\n");
                }
            }
            tt_g4_uncut(tk, (const uint8_t *)text, (int)n, &b);
            if (!tt_ids_eq(&h, &b)) {
                st->g4_cut_diff++;
                if (verbose) printf("  g4: corte por quebra de linha difere do BPE sem corte em %s\n", name);
            }
            free(a.ids); free(b.ids); free(h.ids);
        } else st->g4_ref_skip++;
    }
    free(ids.ids);
}

static int tt_read(const char *path, char **out, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (8 << 20)) { fclose(f); return 0; }
    *out = xalloc((size_t)sz + 1, 1);
    *n = fread(*out, 1, (size_t)sz, f); (*out)[*n] = 0;
    fclose(f);
    return 1;
}

static void tt_walk(const Model *m, const char *dir, const char *ext, int recurse, TtStats *st, int verbose) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        DIR *sub = recurse ? opendir(p) : NULL;
        if (sub) { closedir(sub); tt_walk(m, p, ext, recurse, st, verbose); continue; }
        const size_t l = strlen(p), el = strlen(ext);
        if (l < el || strcmp(p + l - el, ext) != 0) continue;
        char *t; size_t n;
        if (tt_read(p, &t, &n)) { tt_case(m, p, t, n, st, verbose); free(t); }
    }
    closedir(d);
}

static int tt_audit(Model *m, const char *root) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    int fam = tk->spm == 2 ? 4 : tk->pre == PRE_QWEN35 ? 1 : tk->pre == PRE_LLAMA3 ? 2 : 0;
    int fail = 0;
    printf("modelo: arquitetura %s, vocabulario %d, tokenizer.ggml.model=%s, pre-tokenizador=%s, NFC=%d, ignore_merges=%d, add_bos=%d (bos=%d)\n",
           c->arch, tk->n, tk->spm == 2 ? "gemma4" : tk->spm ? "llama" : "gpt2",
           tk->spm ? "(sem; corte por quebra de linha + BPE de caracteres)" : tk->pre == PRE_QWEN35 ? "qwen35" : tk->pre == PRE_LLAMA3 ? "llama-bpe/glm4" : "outro",
           tk->nfc, tk->ignore_merges, c->add_bos, c->bos);
    printf("especiais reconhecidos no texto: %d\n", tk->n_special);
    if (!fam) printf("(sem casos dourados para esta familia; so ida e volta e contagem)\n");

    /* 1) pre-tokenizador */
    int np = 0, npf = 0;
    if (fam == 1 || fam == 2) {
        for (size_t i = 0; i < sizeof TT_PRE / sizeof TT_PRE[0]; i++) {
            const TtPre *g = &TT_PRE[i];
            if (!(g->fam & fam)) continue;
            np++;
            g_tok_trace = tt_trace; g_tt_nl = 0;
            Ids ids = {0};
            encode(tk, g->text, &ids);
            g_tok_trace = NULL;
            Buf got = {0};
            if (fam == 1 && strcmp(g->text, "a\xcc\x81") == 0) {      /* NFC muda o texto: compara o pedaco normalizado */
                int nn = 0; uint8_t *t = nfc_norm((const uint8_t *)g->text, (int)strlen(g->text), &nn);
                buf_add(&got, (const char *)t, (size_t)nn); free(t);
            } else {
                size_t pos = 0;
                for (int k = 0; k < g_tt_nl; k++) {
                    if (k) buf_add(&got, TT_S, 1);
                    buf_add(&got, g->text + pos, (size_t)g_tt_lens[k]); pos += (size_t)g_tt_lens[k];
                }
            }
            const size_t el = strlen(g->pieces);
            if (got.n != el || (el && memcmp(got.p, g->pieces, el) != 0)) {
                npf++;
                printf("  FALHA pre-tokenizador [%s] ", fam == 1 ? "qwen35" : "glm4"); tt_show(g->text, strlen(g->text));
                printf("\n    esperado: "); tt_show(g->pieces, el);
                printf("\n    obtido:   "); tt_show(got.p ? got.p : "", got.n);
                printf("\n    regra:    %s\n", g->why);
            }
            free(got.p); free(ids.ids);
        }
        printf("pre-tokenizador (casos dourados): %d casos, %d falhas\n", np, npf);
        fail += npf;
    }

    /* 2) tokens */
    int nt = 0, ntf = 0;
    for (size_t i = 0; i < sizeof TT_TOK / sizeof TT_TOK[0]; i++) {
        const TtTok *g = &TT_TOK[i];
        if (!(g->fam & fam)) continue;
        nt++;
        Ids ids = {0};
        encode(tk, g->text, &ids);
        Buf got = {0};
        for (int k = 0; k < ids.n; k++) { if (k) buf_add(&got, TT_S, 1); buf_add(&got, tk->tok[ids.ids[k]], (size_t)tk->toklen[ids.ids[k]]); }
        const size_t el = strlen(g->toks);
        if (got.n != el || memcmp(got.p ? got.p : "", g->toks, el) != 0) {
            ntf++;
            printf("  FALHA tokens "); tt_show(g->text, strlen(g->text));
            printf("\n    esperado: "); tt_show(g->toks, el);
            printf("\n    obtido:   "); tt_show(got.p ? got.p : "", got.n);
            printf("\n    origem:   %s\n", g->why);
        }
        free(got.p); free(ids.ids);
    }
    /* normalizacao: Qwen compoe (NFC); GLM e Gemma nao normalizam (normalizer nulo / so espacos) */
    if (fam == 1 || fam == 2) {
        Ids a = {0}, b = {0};
        encode(tk, "e\xcc\x81", &a); encode(tk, "\xc3\xa9", &b);
        const int eq = tt_ids_eq(&a, &b);
        nt++;
        if (eq != (fam == 1)) { ntf++; printf("  FALHA normalizacao: e+U+0301 %s o mesmo que U+00E9 (esperado %s)\n", eq ? "e" : "nao e", fam == 1 ? "igual (NFC)" : "diferente (sem normalizador)"); }
        free(a.ids); free(b.ids);
    }
    /* BOS: Qwen e GLM nao acrescentam (o template do GLM traz [gMASK]<sop>); gemma4 acrescenta <bos> (id 2) */
    nt++;
    if (fam && (c->add_bos > 0) != (fam == 4)) { ntf++; printf("  FALHA BOS: add_bos=%d\n", c->add_bos); }
    if (fam == 4 && c->bos != 2) { ntf++; printf("  FALHA BOS: bos=%d (esperado 2)\n", c->bos); }
    printf("tokens, especiais, byte fallback, normalizacao e BOS (casos dourados): %d casos, %d falhas\n", nt, ntf);
    fail += ntf;

    /* 3) corpus */
    TtStats st; memset(&st, 0, sizeof st);
    { char p[1024]; snprintf(p, sizeof p, "%s/docs", root); tt_walk(m, p, ".md", 0, &st, 1); }
    { char p[1024]; snprintf(p, sizeof p, "%s/src", root); tt_walk(m, p, ".c", 1, &st, 1); }
    { const char *top[] = { "README.md", "CLAUDE.md" };
      for (int i = 0; i < 2; i++) { char p[1024], *t; size_t n; snprintf(p, sizeof p, "%s/%s", root, top[i]); if (tt_read(p, &t, &n)) { tt_case(m, p, t, n, &st, 1); free(t); } } }
    long nfiles = st.cases;
    static const char *BUILTIN[] = {
        "A a\xc3\xa7\xc3\xa3o r\xc3\xa1pida: n\xc3\xa3o h\xc3\xa1 cora\xc3\xa7\xc3\xa3o que resista \xc3\xa0s p\xc3\xa1ginas do \xc3\xadndice. \xc3\x94nibus, a\xc3\xa7\xc3\xba" "car, ping\xc3\xbc" "im, caf\xc3\xa9 com leite \xc3\xa0s 15h.",
        "Emoji: \xf0\x9f\x98\x80 \xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd \xf0\x9f\x87\xa7\xf0\x9f\x87\xb7 \xe2\x9d\xa4\xef\xb8\x8f \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7 \xf0\x9f\x9a\x80\xf0\x9f\x9a\x80\xf0\x9f\x9a\x80!",
        "CJK: \xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c\xe3\x80\x82 \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88 \xed\x95\x9c\xea\xb5\xad\xec\x96\xb4 \xed\x85\x8c\xec\x8a\xa4\xed\x8a\xb8",
        "Outros: \xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 \xd0\xbc\xd0\xb8\xd1\x80 \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5 \xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87 \xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac",
        "Espacos: a  b   c\t\td \t e\xc2\xa0\xc2\xa0" "f\xe3\x80\x80g\xe2\x80\x83h\xe2\x80\xa8i  \n  j\r\n\r\nk\n\n\n\nl \n m\n\t\n",
        "   \n\n   inicio com espacos e fim com espacos   \n\n\n   ",
        "\t\tdois tabs\n\t\t\ttres tabs\n    quatro espacos\n        oito espacos\n",
        "int main(void) {\r\n\tprintf(\"%d\\n\", 0x7fffffff);\r\n\treturn 0;\r\n}\r\n",
        "1234567890 3.14159265358979323846 1,234,567.89 R$ 1.234,56 2026-11-26T20:40:00Z 0xDEADBEEF 1e-9 -42",
        "Contracoes: don't can't won't it's they're we've I'll I'm he'd rock'n'roll O'Brien d'agua 'tis 'twas DON'T IT'S",
        "Pontuacao: ...!!!??? --- === *** ### ~~~ ``` <<>> {{}} [[]] ((())) ;;; ::: ,,, ||| &&& @@@ $$$ %%% ^^^ ___ \\\\\\ ///",
        "Mistura: caf\xc3\xa9\xc3\x9f\xc3\xb1\xc3\xb8\xc5\x82\xc5\x93 \xe2\x82\xac\xc2\xa3\xc2\xa5\xe2\x82\xbf \xc2\xb2\xc2\xb3\xc2\xbd \xe2\x85\xa7 \xe2\x80\x9c" "aspas\xe2\x80\x9d \xe2\x80\x98simples\xe2\x80\x99 \xe2\x80\x94 \xe2\x80\xa6",
        "{\"a\":[1,2,3],\"b\":{\"c\":null,\"d\":\"\\u00e9\\n\"},\"e\":true}",
        "SGVsbG8gd29ybGQhIFRoaXMgaXMgYSBiYXNlNjQgc3RyaW5nIHdpdGggbm8gc3BhY2VzIGF0IGFsbC4uLg==",
        "",
        " ",
        "\n",
        "a",
    };
    for (size_t i = 0; i < sizeof BUILTIN / sizeof BUILTIN[0]; i++) {
        char nm[64]; snprintf(nm, sizeof nm, "texto %zu", i);
        tt_case(m, nm, BUILTIN[i], strlen(BUILTIN[i]), &st, 1);
    }
    /* linhas longas e repeticoes (tambem medem o custo: o BPE precisa ser O(n log n)) */
    {
        const size_t L = 200000;
        char *t = xalloc(L + 1, 1);
        memset(t, 'a', L); t[L] = 0; tt_case(m, "200000 x 'a'", t, L, &st, 1);
        for (size_t i = 0; i < L; i++) t[i] = i % 7 == 6 ? ' ' : (char)('a' + (i * 7919 + i / 13) % 26);
        tt_case(m, "200000 letras e espacos pseudo-aleatorios", t, L, &st, 1);
        for (size_t i = 0; i < L; i++) t[i] = (char)('A' + (i * 31 + i / 5) % 58);     /* parecido com base64, sem espacos */
        for (size_t i = 0; i < L; i++) if (t[i] == '[' || t[i] == '\\' || t[i] == ']' || t[i] == '^' || t[i] == '_' || t[i] == '`') t[i] = '0';
        tt_case(m, "200000 caracteres base64 sem espacos", t, L, &st, 1);
        memset(t, '\n', 5000); t[5000] = 0; tt_case(m, "5000 quebras de linha", t, 5000, &st, 1);
        memset(t, ' ', 5000); t[5000] = 0; tt_case(m, "5000 espacos", t, 5000, &st, 1);
        for (size_t i = 0; i < L; i++) t[i] = i % 11 == 10 ? '\n' : (char)('0' + i % 10);
        tt_case(m, "200000 digitos e quebras", t, L, &st, 1);
        free(t);
    }
    printf("corpus: %ld casos (%ld arquivos de docs/ src/ README), %ld bytes, %ld tokens (%.2f bytes/token), %.1f MB/s (so a codificacao)\n",
           st.cases, nfiles, st.bytes, st.tokens, st.tokens ? (double)st.bytes / (double)st.tokens : 0.0, st.secs > 0 ? (double)st.bytes / 1e6 / st.secs : 0.0);
    printf("ida e volta: %ld falhas; %ld so diferem pela normalizacao NFC (esperado); %ld com U+2581 no texto (nao reversivel no gemma4)\n", st.rt_fail, st.rt_nfc, st.rt_skip);
    fail += (int)st.rt_fail;
    if (tk->pre == PRE_LLAMA3)
        printf("ignore_merges ligado x desligado: %ld de %ld casos mudam de tokens (a chave KATANA_TOK_IGNMERGES=0 volta ao comportamento anterior)\n", st.im_diff, st.cases);
    if (tk->spm == 2) {
        printf("gemma4, BPE de heap x versao lenta anterior: %ld casos diferem (%ld casos com linhas > 3000 bytes ficaram de fora da conferencia)\n", st.g4_ref_diff, st.g4_ref_skip);
        printf("gemma4, corte por quebras de linha x BPE sem corte (o do tokenizer.json): %ld casos diferem\n", st.g4_cut_diff);
        fail += (int)st.g4_ref_diff;
    }
    printf("%s: %d falhas\n", fail ? "REPROVADO" : "APROVADO", fail);
    return fail ? 1 : 0;
}
