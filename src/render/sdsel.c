/* ============================ render: selecao do briefing por relevancia (generica) ===============================
 * O motor NAO escreve nem traduz o briefing: o texto recebido (de quem manda: o modelo de conversa ou o usuario) E o prompt, com as palavras do autor.
 * Cabendo no orcamento do motor (FLUX: 504 posicoes de texto, contadas com o tokenizador real; Stable: --chunks blocos de 75 tokens do CLIP) ele
 * passa INTEIRO e intacto, byte a byte. So quando passa do orcamento o motor escolhe, sem LLM e sem dominio fixo, o que fica:
 *
 *   unidades  linhas (itens de lista, frases); uma linha longa se divide em frases e, se ainda longa, em trechos separados por ; ou ,
 *             (lista de tags separadas por virgula: cada tag e uma unidade). Cabecalhos (#, terminados em ":", linhas so de maiusculas) ficam
 *             so se algum filho entra.
 *   obrigatorias  a primeira linha (assunto) e a primeira unidade que nao e cabecalho; unidades com marcadores de regra (ABSOLUTAS, REGRAS,
 *             NUNCA, SEM, PROIBIDO, NEVER, MUST...). Se so elas passam de 60% do orcamento, as de menor pontuacao voltam a competir como as demais.
 *   pontuacao sinais de informacao visual: cor RGB/hex, numero com unidade (K, cm, %...), palavras de cor, material, luz e estilo (pt e en),
 *             palavra toda em maiusculas (enfase), raridade (idf das palavras dentro do proprio texto, BM25 de documento) e um leve favor a quem vem antes.
 *   escolha   da maior pontuacao para a menor; cada unidade entra se o texto montado (medido pelo contador do motor) continuar no orcamento; a que nao
 *             cabe e pulada e uma menor ainda pode entrar. Corte sempre na unidade inteira; ordem original mantida; nada e acrescentado. */

#define SDS_MAXU 800

typedef struct {
    int start, end;          /* bytes no texto (sem espacos nas pontas; inclui o , ou ; final de um trecho) */
    int line, header, parent, must;
    double score;
    int chosen, cost;
} SdsUnit;

typedef struct SdsRep { int used, budget, whole, nunits, ncut, mustcut; char cut[1400]; } SdsRep;     /* resultado: tokens usados, se passou inteiro, o que foi cortado */

typedef int (*SdsCount)(const char *text, void *ctx);                   /* tokens do texto no motor */

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

static int sds_in(const char *w, const char *const *list) { for (int i = 0; list[i]; i++) if (!strcmp(w, list[i])) return 1; return 0; }

/* marcadores de regra obrigatoria */
static const char *const SDS_MARK[] = { "absolute", "absoluta", "absolutas", "absoluto", "absolutos", "regra", "regras", "nunca", "sem", "proibido", "proibida", "proibidos", "jamais",
    "obrigatorio", "obrigatoria", "never", "forbidden", "must", "mandatory", NULL };
/* palavras de sinal visual (pt e en; so pontuam, nunca entram no texto) */
static const char *const SDS_COLOR[] = { "cor", "cores", "color", "colors", "colour", "rgb", "hex", "vermelho", "vermelha", "azul", "verde", "amarelo", "amarela", "rosa", "roxo", "lilas",
    "laranja", "preto", "preta", "branco", "branca", "cinza", "marrom", "bege", "dourado", "dourada", "prata", "red", "blue", "green", "yellow", "pink", "purple", "orange", "black",
    "white", "gray", "grey", "brown", "beige", "gold", "golden", "silver", "teal", "cyan", "magenta", "violet", "crimson", "burgundy", "turquesa", "escuro", "claro", "dark", "light", "pastel", NULL };
static const char *const SDS_MAT[] = { "madeira", "metal", "vidro", "tecido", "concreto", "pedra", "couro", "pintada", "tinta", "laca", "porcelana", "ceramica", "azulejo", "marmore",
    "wood", "wooden", "glass", "fabric", "stone", "leather", "paint", "painted", "marble", "ceramic", "steel", "iron", "cloth", "velvet", "silk", "fur", "cobre", "aco", NULL };
static const char *const SDS_LIGHT[] = { "luz", "luzes", "iluminacao", "sol", "solar", "sombra", "sombras", "ligada", "ligado", "lampada", "brilho", "lighting", "sun", "shadow",
    "shadows", "lamp", "glow", "glowing", "neon", "sunset", "backlight", "rim", "ambient", NULL };
static const char *const SDS_STYLE[] = { "estilo", "style", "pixel", "art", "arte", "realista", "realistic", "fotografia", "photo", "photograph", "photography", "photorealistic",
    "ilustracao", "illustration", "anime", "cartoon", "pintura", "painting", "aquarela", "watercolor", "render", "cinematic", "cinematografico", "sketch", "desenho", "manga",
    "retro", "bit", "8bit", "16bit", "lowpoly", "oil", "cyberpunk", "fantasy", "fantasia", "game", "jogo", "vintage", "isometric", "isometrico", NULL };
static const char *const SDS_STOP[] = { "the", "and", "for", "with", "that", "this", "from", "are", "was", "uma", "um", "uns", "umas", "com", "para", "por", "que", "dos", "das", "nos",
    "nas", "mais", "como", "ser", "seu", "sua", "seus", "suas", "ele", "ela", "tem", "ao", "aos", "em", "na", "no", "do", "da", "de", "os", "as", "of", "to", "in", "on", "at", "is", "it", NULL };

/* tem RGB (r,g,b com separador , ou . ; cada um 0..255) ou hex #rrggbb; devolve 1 */
static int sds_has_color(const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '#') { int k = 0; while (isxdigit((unsigned char)p[1 + k])) k++; if ((k == 6 || k == 3) && !isalnum((unsigned char)p[1 + k])) return 1; }
        if (isdigit((unsigned char)*p) && (p == s || !isalnum((unsigned char)p[-1]))) {
            int v[3], k = 0; const char *q = p;
            while (k < 3 && isdigit((unsigned char)*q)) {
                int x = 0, d = 0; while (isdigit((unsigned char)*q) && d < 4) { x = x * 10 + (*q - '0'); q++; d++; }
                v[k++] = x;
                if (k < 3) { if ((*q == ',' || *q == '.') && isdigit((unsigned char)q[1])) q++; else if (*q == ',' && q[1] == ' ' && isdigit((unsigned char)q[2])) q += 2; else break; }
            }
            if (k == 3 && v[0] <= 255 && v[1] <= 255 && v[2] <= 255 && !isalnum((unsigned char)*q) && *q != '%') return 1;
        }
    }
    return 0;
}

/* quantos tipos distintos de "numero com unidade" (K, cm, mm, m, %, px, lm, w, kg, graus, °) ha no texto */
static int sds_units(const char *s) {
    static const char *const U[] = { "k", "cm", "mm", "m", "px", "lm", "w", "kg", "graus", "grau", "deg", "mp", NULL };
    int seen = 0;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p) || (p > s && isalnum((unsigned char)p[-1]))) continue;
        const char *q = p; while (isdigit((unsigned char)*q) || *q == '.' || *q == ',') q++;
        while (*q == ' ') q++;
        if ((unsigned char)q[0] == 0xC2 && (unsigned char)q[1] == 0xB0) { seen |= 1 << 20; p = q; continue; }
        if (*q == '%') { seen |= 1 << 19; p = q; continue; }
        char u[8]; int n = 0; while (isalpha((unsigned char)*q) && n < 7) u[n++] = (char)tolower((unsigned char)*q++); u[n] = 0;
        for (int i = 0; U[i]; i++) if (n && !strcmp(u, U[i]) && !isalnum((unsigned char)*q)) seen |= 1 << i;
        p = q > p ? q - 1 : p;
    }
    int c = 0; while (seen) { c += seen & 1; seen >>= 1; }
    return c;
}

/* linha: cabecalho? (# no inicio, termina em ":" curta, ou so maiusculas) */
static int sds_is_header(const char *s, int len) {
    while (len && isspace((unsigned char)s[len - 1])) len--;
    if (!len) return 0;
    if (s[0] == '#') return 1;
    if (s[0] == '-' || s[0] == '*') return 0;
    if (s[len - 1] == ':' && len < 100) return 1;
    int up = 0, lo = 0; for (int i = 0; i < len; i++) { if (isupper((unsigned char)s[i])) up++; else if (islower((unsigned char)s[i])) lo++; }
    return len <= 80 && up >= 3 && lo == 0;
}

typedef struct { const char *text; SdsUnit *u; int n, el; } SdsDoc;

static int sds_on(const SdsDoc *d, int i) {
    const SdsUnit *c = &d->u[i];
    if (!c->header) return c->chosen;
    if (c->must) return 1;
    for (int j = i + 1; j < d->n && !d->u[j].header; j++) if (d->u[j].chosen) return 1;
    return 0;
}

/* texto montado com as unidades escolhidas (e o cabecalho de quem tem filho escolhido). Mesma linha: espaco; linha nova: \n */
static void sds_assemble(const SdsDoc *d, char *out) {
    char *o = out; int last = -1, lastline = -1;
    for (int i = 0; i < d->n; i++) {
        const SdsUnit *c = &d->u[i];
        if (!sds_on(d, i)) continue;
        if (last >= 0) *o++ = c->line == lastline ? ' ' : '\n';
        int e = c->end, nextsame = 0;
        for (int j = i + 1; j < d->n; j++) { if (!sds_on(d, j)) continue; nextsame = !d->u[j].header && d->u[j].line == c->line; break; }
        if (!nextsame && !c->header && e > c->start && (d->text[e - 1] == ',' || d->text[e - 1] == ';')) e--;     /* ultimo trecho da linha: sem a virgula pendurada */
        memcpy(o, d->text + c->start, (size_t)(e - c->start)); o += e - c->start;
        last = i; lastline = c->line;
    }
    *o = 0;
}

static void sds_cut_add(SdsRep *r, const char *s, int len) {
    char snip[64]; int k = 0; for (int i = 0; i < len && k < 40; i++) { const char ch = s[i]; snip[k++] = ch == '\n' || ch == '"' ? ' ' : ch; }
    while (k > 0 && ((unsigned char)snip[k - 1] & 0xC0) == 0x80) k--;                 /* nao corta no meio de um caractere UTF-8 */
    if (k > 0 && (unsigned char)snip[k - 1] >= 0xC0) k--;
    snip[k] = 0;
    if (r->ncut < 7) { const size_t l = strlen(r->cut); snprintf(r->cut + l, sizeof r->cut - l, "%s\"%s%s\"", l ? "; " : "", snip, len > 40 ? "..." : ""); }
    r->ncut++;
}

/* divide uma linha longa [a,b) em frases e trechos; pieces[][2] = inicio e fim; devolve quantos */
static int sds_split(const char *tx, int a, int b, int (*pieces)[2], int max) {
    int np = 0, s0 = a;
    int (*sent)[2] = (int (*)[2])xalloc((size_t)max * 2, sizeof(int)); int ns = 0;
    for (int k = a; k < b && ns < max - 1; k++) {                                       /* frases: . ! ? seguidos de espaco (nao 3.5) */
        if ((tx[k] == '.' || tx[k] == '!' || tx[k] == '?') && (k + 1 == b || tx[k + 1] == ' ')) {
            sent[ns][0] = s0; sent[ns][1] = k + 1; ns++;
            s0 = k + 1; while (s0 < b && tx[s0] == ' ') s0++; k = s0 - 1;
        }
    }
    if (s0 < b) { sent[ns][0] = s0; sent[ns][1] = b; ns++; }
    for (int k = 0; k < ns && np < max - 2; k++) {                                      /* trechos de frase longa: ; e depois , */
        const int pa = sent[k][0], pb = sent[k][1];
        if (pb - pa <= 140) { pieces[np][0] = pa; pieces[np][1] = pb; np++; continue; }
        int commas = 0, semis = 0; for (int q = pa; q < pb; q++) { if (tx[q] == ',') commas++; if (tx[q] == ';') semis++; }
        const int tags = commas >= 6 && (pb - pa) / (commas + 1) < 26;                  /* lista de tags: cada uma e uma unidade */
        int t0 = pa;
        for (int q = pa; q < pb && np < max - 2; q++) {
            const int sep = tx[q] == ';' || (tx[q] == ',' && !semis);
            if (sep && (q + 1 == pb || tx[q + 1] == ' ') && (tags || semis || q + 1 - t0 >= 60)) {
                pieces[np][0] = t0; pieces[np][1] = q + 1; np++;
                t0 = q + 1; while (t0 < pb && tx[t0] == ' ') t0++; q = t0 - 1;
            }
        }
        if (t0 < pb) { pieces[np][0] = t0; pieces[np][1] = pb; np++; }
    }
    free(sent);
    return np;
}

/* o texto (malloc, nunca NULL) que vai ao motor: o proprio texto se cabe; senao as unidades de maior sinal que cabem em budget. extra = pedido do
 * usuario (--nl), opcional: entra como ultima unidade obrigatoria, nas palavras dele. explain: relatorio por unidade em stderr */
static char *sds_prompt(const char *text, const char *extra, int budget, SdsCount count, void *ctx, SdsRep *rep, int explain) {
    memset(rep, 0, sizeof *rep); rep->budget = budget;
    const size_t tl = strlen(text), el = extra && extra[0] ? strlen(extra) : 0;
    char *tx = xalloc(tl + el + 4, 1); memcpy(tx, text, tl);
    if (el) { tx[tl] = '\n'; memcpy(tx + tl + 1, extra, el); }
    rep->used = count(tx, ctx);
    if (rep->used <= budget) { rep->whole = 1; return tx; }

    SdsUnit *u = xalloc(SDS_MAXU, sizeof(SdsUnit)); int n = 0, line = 0, parent = -1;
    for (int p = 0; tx[p] && n < SDS_MAXU - 70; line++) {
        int e = p; while (tx[e] && tx[e] != '\n') e++;
        int a = p, b = e; while (a < b && isspace((unsigned char)tx[a])) a++; while (b > a && isspace((unsigned char)tx[b - 1])) b--;
        p = tx[e] ? e + 1 : e;
        if (a >= b) continue;
        if (el && (size_t)a >= tl + 1) break;                                           /* o pedido extra vem depois, como unidade propria */
        if (sds_is_header(tx + a, b - a)) {
            char hb[300]; snprintf(hb, sizeof hb, "%.*s", b - a < 299 ? b - a : 299, tx + a);
            static char hw[40][32]; const int nh = sds_words(hb, hw, 40); int mark = 0;
            for (int k = 0; k < nh; k++) if (sds_in(hw[k], SDS_MARK)) mark = 1;
            u[n] = (SdsUnit){ .start = a, .end = b, .line = line, .header = 1, .parent = -1, .must = mark ? 2 : 0 };    /* must 2: cabecalho de regras (so ele; cada filho se julga pelas proprias palavras) */
            parent = n++;
            continue;
        }
        int pieces[64][2], np = 0;
        if (b - a <= 140) { pieces[0][0] = a; pieces[0][1] = b; np = 1; } else np = sds_split(tx, a, b, pieces, 64);
        for (int k = 0; k < np && n < SDS_MAXU - 1; k++) {
            u[n] = (SdsUnit){ .start = pieces[k][0], .end = pieces[k][1], .line = line, .parent = parent };
            n++;
        }
    }
    if (el) { const int s0 = (int)(tl + 1); u[n++] = (SdsUnit){ .start = s0, .end = s0 + (int)el, .line = 1000000, .parent = -1, .must = 1 }; }
    for (int i = 0; i < n; i++) if (u[i].header && u[i].must == 2) u[i].must = 0;
    { int k = 0; while (k < n && u[k].header) { u[k].must = 1; k++; } if (k < n) u[k].must = 1; }      /* assunto: primeira linha e a primeira que nao e cabecalho */
    static char W[SDS_MAXU][48][32]; static int nw[SDS_MAXU]; static double bm[SDS_MAXU];
    for (int i = 0; i < n; i++) {
        char buf[1200]; const int l = u[i].end - u[i].start; snprintf(buf, sizeof buf, "%.*s", l < 1199 ? l : 1199, tx + u[i].start);
        nw[i] = sds_words(buf, W[i], 48);
        if (!u[i].must && !u[i].header) for (int k = 0; k < nw[i]; k++) if (sds_in(W[i][k], SDS_MARK)) { u[i].must = 1; break; }
    }
    double avg = 0, best = 0; int nc = 0; for (int i = 0; i < n; i++) if (!u[i].header) { avg += nw[i]; nc++; }
    avg = nc ? avg / nc : 1; if (avg < 1) avg = 1;
    for (int i = 0; i < n; i++) {
        bm[i] = 0; if (u[i].header) continue;
        for (int k = 0; k < nw[i]; k++) {
            if (strlen(W[i][k]) < 3 || sds_in(W[i][k], SDS_STOP)) continue;
            int seen = 0; for (int j = 0; j < k; j++) if (!strcmp(W[i][j], W[i][k])) { seen = 1; break; }
            if (seen) continue;
            int tf = 0; for (int j = 0; j < nw[i]; j++) if (!strcmp(W[i][j], W[i][k])) tf++;
            int df = 0; for (int q = 0; q < n; q++) { if (u[q].header) continue; for (int j = 0; j < nw[q]; j++) if (!strcmp(W[q][j], W[i][k])) { df++; break; } }
            const double idf = log(1.0 + (nc - df + 0.5) / (df + 0.5));
            bm[i] += idf * (tf * 2.2) / (tf + 1.2 * (0.25 + 0.75 * nw[i] / avg));
        }
        if (bm[i] > best) best = bm[i];
    }
    for (int i = 0; i < n; i++) {
        if (u[i].header) continue;
        char buf[1200]; const int l = u[i].end - u[i].start; snprintf(buf, sizeof buf, "%.*s", l < 1199 ? l : 1199, tx + u[i].start);
        double s = 2.0 * (best > 0 ? bm[i] / best : 0) + 0.4 * (1.0 - (double)i / n);
        int color = 0, mat = 0, light = 0, style = 0, caps = 0;
        for (int k = 0; k < nw[i]; k++) { const char *w = W[i][k]; color |= sds_in(w, SDS_COLOR); mat |= sds_in(w, SDS_MAT); light |= sds_in(w, SDS_LIGHT); style |= sds_in(w, SDS_STYLE); }
        for (const char *q = buf; *q; ) { int k = 0; while (isupper((unsigned char)q[k])) k++; if (k >= 3 && !isalpha((unsigned char)q[k]) && (q == buf || !isalpha((unsigned char)q[-1]))) caps = 1; q += k ? k : 1; }
        if (sds_has_color(buf)) s += 2.5;
        const int nu = sds_units(buf); s += nu >= 2 ? 1.5 : nu ? 1.0 : 0;
        s += (color ? 1.2 : 0) + (mat ? 1.0 : 0) + (light ? 0.8 : 0) + (style ? 1.5 : 0) + (caps ? 1.0 : 0);
        if (nw[i] < 2) s -= 0.8;
        u[i].score = s;
    }
    SdsDoc d = { tx, u, n, (int)el };
    char *as = xalloc(strlen(tx) + 16, 1);
    for (int i = 0; i < n; i++) u[i].chosen = u[i].must && !u[i].header;
    sds_assemble(&d, as);
    int used = count(as, ctx);
    while (used > budget * 6 / 10) {                                                    /* as obrigatorias passam de 60% do orcamento: as de menor pontuacao voltam a competir (nunca a primeira nem o pedido extra) */
        int w = -1; for (int i = 1; i < n; i++) if (u[i].chosen && !u[i].header && !(el && i == n - 1) && (w < 0 || u[i].score < u[w].score)) w = i;
        if (w < 0) break;
        u[w].chosen = 0; u[w].must = 0; rep->mustcut++;
        sds_assemble(&d, as); used = count(as, ctx);
    }
    int *order = xalloc((size_t)n + 1, sizeof(int)), m = 0;
    for (int i = 0; i < n; i++) if (!u[i].header && !u[i].chosen) order[m++] = i;
    for (int i = 1; i < m; i++) { const int v = order[i]; int j = i - 1; while (j >= 0 && u[order[j]].score < u[v].score) { order[j + 1] = order[j]; j--; } order[j + 1] = v; }
    for (int k = 0; k < m; k++) {
        SdsUnit *c = &u[order[k]]; c->chosen = 1;
        sds_assemble(&d, as); const int t = count(as, ctx);
        c->cost = t - used;
        if (t > budget) c->chosen = 0; else used = t;
    }
    sds_assemble(&d, as); rep->used = count(as, ctx);
    for (int i = 0; i < n; i++) if (!u[i].header) { rep->nunits++; if (!u[i].chosen) sds_cut_add(rep, tx + u[i].start, u[i].end - u[i].start); }
    if (explain) {
        fprintf(stderr, "[explain-brief] %d de %d tokens; %d unidades, %d cortadas\n", rep->used, budget, rep->nunits, rep->ncut);
        for (int i = 0; i < n; i++) {
            if (u[i].header) continue;
            fprintf(stderr, "[explain-brief] %-6s linha %3d score %5.2f %+4d tok  %.*s\n", u[i].chosen ? (u[i].must ? "OBRIG" : "ENTROU") : "CORTE", u[i].line + 1, u[i].score, u[i].cost,
                    u[i].end - u[i].start < 90 ? u[i].end - u[i].start : 90, tx + u[i].start);
        }
    }
    free(order); free(u); free(tx);
    char *res = xalloc(strlen(as) + 1, 1); strcpy(res, as); free(as);
    return res;
}

/* relatorio de uma linha para quem chamou (o modelo de conversa le o resultado da ferramenta e pode reenviar mais curto) */
static void sds_report(const SdsRep *r, const char *engine, FILE *f) {
    fprintf(f, "[brief] %s tokens %d/%d%s\n", engine, r->used, r->budget, r->whole ? " (inteiro)" : "");
    if (r->ncut) fprintf(f, "[brief] cut %d: %s%s\n", r->ncut, r->cut, r->ncut > 7 ? "; ..." : "");
    if (r->mustcut) fprintf(f, "[brief] aviso: as unidades obrigatorias passavam de 60%% do orcamento; %d voltou(aram) a competir por pontuacao\n", r->mustcut);
}
