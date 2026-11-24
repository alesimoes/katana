/* ============================ render: selecionador de relevancia do briefing ===============================
 * Um briefing real passa do que o motor de imagem le (FLUX: 512 posicoes de texto; Stable: blocos de 75 tokens do CLIP). Cortar no fim joga fora
 * o que vem por ultimo (os materiais menores, a luz, o estilo). Aqui o briefing vira UNIDADES (uma regra, um material, uma luz, um objeto, um
 * pedido de estilo), cada uma com uma pontuacao, e entra o que tem mais sinal ate o orcamento acabar. Sem LLM; so contas.
 *
 * Pontuacao (score) de cada unidade:
 *   score = peso_da_categoria * (0.6 + bm25_normalizado) + area + sinais
 *   bm25    BM25 (k1 1.2, b 0.75) do texto de origem da unidade contra um vocabulario de busca com pesos (cor, rgb, k, materiais, piso, parede,
 *           ligada, nunca/saturadas...); o idf vem das proprias unidades do briefing: uma palavra que aparece em quase todas (como "piso" em
 *           cinco pisos) vale menos, uma rara vale mais. O bm25 e dividido pelo maior do briefing (0 a 1).
 *   area    materiais: % do quadro / 10 (limitado a 3): 25% = +2.5, 1% = +0.1
 *   sinais  tem cor RGB +1.5, tem temperatura em K +0.5, tem escala de padrao +0.4; prior do estilo; material vago (sem cor e sem tipo) -1.2
 *   obrigatorias (must): regras absolutas, elevacao solar, temperatura de cor e o pedido do usuario (--nl) entram sempre; se so elas ja passam do
 *           orcamento isso e avisado e nada mais entra.
 * Escolha: as demais, da maior pontuacao para a menor; cada uma entra se o texto montado (medido com o contador de tokens do proprio motor)
 * continuar dentro do orcamento; se nao couber, e pulada e a proxima, menor, ainda pode entrar (nao se corta no primeiro que estoura).
 * Ordem final do texto, estavel: regras > materiais (maior area primeiro) > luz > objetos > estilo > pedido do usuario. */

enum { SDS_RULE = 0, SDS_MAT = 1, SDS_LIGHT = 2, SDS_OBJ = 3, SDS_STYLE = 4, SDS_USER = 5, SDS_NCAT = 6 };
enum { SDS_SUN = 0, SDS_TEMP = 1, SDS_KIND = 2 };                      /* subtipo das unidades de luz */
enum { SDS_SIG_RGB = 1, SDS_SIG_K = 2, SDS_SIG_PAT = 4, SDS_SIG_PCT = 8, SDS_SIG_VAGUE = 16 };   /* VAGUE: sem cor e sem tipo conhecido: nao diz nada ao motor */

typedef struct {
    int cat, sub, must, sig, count;       /* count: quantas linhas do briefing deram esta unidade (luzes do mesmo tipo) */
    double area, weight, prior;           /* prior: peso fixo do estilo; weight: peso do Stable (1.0 = sem parenteses) */
    char src[1000];                        /* texto de origem (portugues) para o BM25 */
    char flux[1000];                       /* fragmento da prosa do FLUX */
    char sd[160];                         /* fragmento do Stable ("" = nao vai para o prompt positivo) */
    double elev; int temp; int kind;      /* dados da luz: elevacao, K, bit do tipo */
    double bm, score;
    int chosen, cost; const char *why, *skip;   /* skip: motivo de nunca entrar (ruido) */
} SdsUnit;

#define SDS_MAXU 160
#define SDS_MAXT 600

static const double SDS_CATW[SDS_NCAT] = { 10.0, 3.0, 2.0, 1.5, 1.0, 10.0 };

/* vocabulario de busca: palavras (sem acento, minusculas) e o quanto cada uma indica informacao visual de alto valor */
static const struct { const char *w; double q; } SDS_QUERY[] = {
    { "rgb", 2.0 }, { "k", 1.5 }, { "cor", 1.5 }, { "cores", 1.5 }, { "fieis", 1.5 }, { "fiel", 1.5 }, { "quentes", 1.2 }, { "saturadas", 1.2 }, { "nunca", 1.0 },
    { "parede", 1.3 }, { "pintada", 1.3 }, { "tinta", 1.0 }, { "piso", 1.3 }, { "madeira", 1.3 }, { "tecido", 1.2 }, { "concreto", 1.2 }, { "porcelana", 1.2 },
    { "metal", 1.1 }, { "laca", 1.0 }, { "vidro", 1.1 }, { "espelho", 1.1 }, { "azulejo", 1.1 }, { "padrao", 0.8 }, { "texturas", 1.2 }, { "gradientes", 0.9 },
    { "ligada", 1.2 }, { "ligado", 1.2 }, { "on", 1.2 }, { "luz", 0.8 }, { "balanco", 1.0 }, { "branco", 1.0 }, { "contato", 1.0 }, { "sombras", 1.0 },
    { "balcao", 1.2 }, { "atendimento", 1.2 }, { "cadeira", 1.0 }, { "poltrona", 1.0 }, { "sofa", 1.0 }, { "mesa", 1.0 }, { "cama", 1.0 },
    { "realista", 1.0 }, { "natural", 0.8 }, { "elegante", 0.6 }, { "elevacao", 1.2 }, { "solar", 1.2 }, { "sol", 1.0 } };
#define SDS_NQ ((int)(sizeof SDS_QUERY / sizeof SDS_QUERY[0]))

/* texto -> palavras minusculas sem acento (letras e digitos); devolve quantas (ate max); cada palavra em w[i] (32 chars) */
static int sds_words(const char *s, char (*w)[32], int max) {
    int n = 0, len = 0; char cur[32];
    for (const unsigned char *p = (const unsigned char *)s; ; p++) {
        int c = *p;
        if (c == 0xC3 && p[1]) {                                           /* UTF-8 latino: tira o acento das vogais e do c */
            const int d = p[1]; p++;
            static const char fold[] = "aaaaaaaceeeeiiiidnooooo*ouuuuyts"; /* U+00C0..U+00DF */
            c = (d >= 0x80 && d < 0xA0) ? fold[d - 0x80] : (d >= 0xA0 && d < 0xC0) ? fold[d - 0xA0] : '?';
            if (c == '*') c = 'x';
        } else if (c >= 0x80) c = ' ';
        if (isalnum(c)) { if (len < 31) cur[len++] = (char)tolower(c); }
        else if (len) { cur[len] = 0; if (n < max) memcpy(w[n++], cur, (size_t)len + 1); len = 0; }
        if (!*p) break;
    }
    if (len && n < max) { cur[len] = 0; memcpy(w[n++], cur, (size_t)len + 1); }
    return n;
}

/* BM25 de cada unidade contra o vocabulario de busca; o idf vem das unidades. Preenche bm (normalizado em 0..1) */
static void sds_bm25(SdsUnit *u, int n) {
    static char W[SDS_MAXU][96][32]; static int nw[SDS_MAXU];
    double avg = 0;
    for (int i = 0; i < n; i++) { nw[i] = sds_words(u[i].src, W[i], 96); avg += nw[i]; }
    avg = n ? avg / n : 1; if (avg < 1) avg = 1;
    double best = 0;
    for (int i = 0; i < n; i++) {
        double s = 0;
        for (int q = 0; q < SDS_NQ; q++) {
            int tf = 0; for (int j = 0; j < nw[i]; j++) if (!strcmp(W[i][j], SDS_QUERY[q].w)) tf++;
            if (!tf) continue;
            int df = 0; for (int k = 0; k < n; k++) { for (int j = 0; j < nw[k]; j++) if (!strcmp(W[k][j], SDS_QUERY[q].w)) { df++; break; } }
            const double idf = log(1.0 + (n - df + 0.5) / (df + 0.5));
            s += SDS_QUERY[q].q * idf * (tf * 2.2) / (tf + 1.2 * (0.25 + 0.75 * nw[i] / avg));
        }
        u[i].bm = s; if (s > best) best = s;
    }
    for (int i = 0; i < n; i++) u[i].bm = best > 0 ? u[i].bm / best : 0;
}

/* pontuacao final de cada unidade */
static void sds_score(SdsUnit *u, int n) {
    sds_bm25(u, n);
    for (int i = 0; i < n; i++) {
        double a = u[i].area / 10.0; if (a > 3.0) a = 3.0;
        u[i].score = SDS_CATW[u[i].cat] * (0.6 + u[i].bm) + (u[i].cat == SDS_MAT ? a : 0.0) + ((u[i].sig & SDS_SIG_RGB) ? 1.5 : 0.0) + ((u[i].sig & SDS_SIG_K) ? 0.5 : 0.0) + ((u[i].sig & SDS_SIG_PAT) ? 0.4 : 0.0);
        if (u[i].cat == SDS_LIGHT && u[i].sub == SDS_KIND) u[i].score += 0.3 * (u[i].count > 3 ? 3 : u[i].count);     /* tipo de luz que aparece em varias linhas */
        u[i].score += u[i].prior - ((u[i].sig & SDS_SIG_VAGUE) ? 1.2 : 0.0);
    }
}

typedef void (*SdsRender)(const SdsUnit *u, int n, char *out, size_t cap, void *ctx);      /* monta o texto com as unidades escolhidas */
typedef int (*SdsCount)(const char *text, void *ctx);                                        /* tokens do texto no motor */

/* escolhe: obrigatorias primeiro, depois por pontuacao; cada uma so fica se o texto continuar dentro de budget. Devolve os tokens do resultado */
static int sds_select(SdsUnit *u, int n, int budget, SdsRender render, SdsCount count, void *ctx) {
    static char text[16000];
    sds_score(u, n);
    for (int i = 0; i < n; i++) { u[i].chosen = u[i].must; u[i].why = u[i].must ? "obrigatoria" : "ainda nao avaliada"; u[i].cost = 0; }
    render(u, n, text, sizeof text, ctx);
    int used = count(text, ctx);
    if (used > budget) fprintf(stderr, "[brief] aviso: so as unidades obrigatorias (regras, sol, temperatura) ja usam %d de %d tokens\n", used, budget);
    int order[SDS_MAXU], m = 0;
    for (int i = 0; i < n; i++) { if (u[i].skip && !u[i].must) u[i].why = u[i].skip; else if (!u[i].must) order[m++] = i; }
    for (int i = 1; i < m; i++) { const int v = order[i]; int j = i - 1; while (j >= 0 && u[order[j]].score < u[v].score) { order[j + 1] = order[j]; j--; } order[j + 1] = v; }   /* estavel */
    for (int k = 0; k < m; k++) {
        SdsUnit *c = &u[order[k]];
        c->chosen = 1; render(u, n, text, sizeof text, ctx);
        const int t = count(text, ctx);
        if (t > budget) { c->chosen = 0; c->why = "nao coube no orcamento"; c->cost = t - used; }
        else { c->why = "entrou"; c->cost = t - used; used = t; }
    }
    return used;
}

/* relatorio: o que entrou, o que foi cortado, a pontuacao e o custo em tokens */
static void sds_explain(const SdsUnit *u, int n, const char *engine, int used, int budget, FILE *f) {
    static const char *const cn[SDS_NCAT] = { "regra", "material", "luz", "objeto", "estilo", "usuario" };
    fprintf(f, "[explain-brief] motor %s: %d de %d tokens; %d unidades\n", engine, used, budget, n);
    for (int c = 0; c < SDS_NCAT; c++)
        for (int i = 0; i < n; i++) {
            if (u[i].cat != c) continue;
            const char *txt = !strcmp(engine, "flux") ? u[i].flux : u[i].sd;
            if (!txt[0]) txt = u[i].flux[0] ? u[i].flux : u[i].src;
            fprintf(f, "[explain-brief] %-5s %-8s score %5.2f bm25 %.2f area %4.1f%% %+4d tok  %s | %.100s\n", u[i].chosen ? "ENTROU" : "CORTE", cn[c], u[i].score, u[i].bm, u[i].area, u[i].cost, u[i].chosen ? "" : u[i].why, txt);
        }
}
