/* ============================ render: briefing -> prompt ===============================
 * katana render brief <briefing.md> --image1 vista.png --out saida.png [--image2 linhas.png] [--image3 amostras.png] [--show] [opcoes]
 *
 * Le um briefing de renderizacao no formato que o arquiteto manda ao servico de fora (secoes MATERIAIS, PRINCIPAIS OBJETOS, ILUMINACAO,
 * ESTILO FOTOGRAFICO, regras) e monta o que o Stable Diffusion 1.5 aceita: um prompt em ingles de ate --chunks blocos de 75 tokens do CLIP
 * (padrao 4 = 300 tokens; sdc_encode_long junta os blocos), um prompt negativo e os parametros. O briefing inteiro NAO entra: ele tem milhares
 * de tokens (listas de luzes repetidas) e o CLIP entende descricoes, nao instrucoes; entra o que descreve a imagem, em ordem de importancia.
 *
 *   Materiais   "- \"NOME\": tipo — descricao, RGB r,g,b (~N% do quadro)": com RGB vira "<cor> <superficie>" (cor por HSV: "burgundy",
 *               "slate blue-grey", "off-white"...); sem RGB vira o material ("sandstone", "concrete floor", "natural wood", "fabric"...).
 *               Frases iguais somam a area; entram da maior area para a menor; tipo "material" sem palavra conhecida, tinta sem cor e luz
 *               de LED sao pulados.
 *   Objetos     os nomes que se entendem ("balcao de atendimento" -> "reception counter", "cadeira" -> "chair"...); "Componente n 27" nao diz nada.
 *   Iluminacao  elevacao solar (>= 60 graus: "bright daylight"), temperatura de cor mais comum das luzes LIGADAS, e os TIPOS de luz que
 *               aparecem (spots embutidos, perfil de LED, plafon, pendente, luz de espelho, de balcao, de parede, de arco...).
 *   Estilo      cada pedido de estilo do texto vira uma frase fixa em ingles (sombras de contato, texturas visiveis, balanco de branco...).
 *   Negativo    sempre: texto, logo, marca d'agua, pessoas, desenho, linhas, saturacao, baixa qualidade; "extra decoration" se o briefing
 *               proibe acrescentar objetos; "plants" so se o briefing nao lista folhagem.
 *   Imagens     --image1 e a vista do SketchUp (ponto de partida, imagem a imagem); --image2, se dada, e o desenho linear da mesma vista
 *               e vira a entrada do ControlNet (aresta por aresta); sem ela, as bordas vem de um filtro Canny da imagem 1. --image3
 *               (amostras de material) nao e usada: o SD 1.5 nao toma uma textura como referencia; valem os nomes e as cores do texto.
 * --show so imprime o prompt e para (sem gerar). O prompt, o negativo e os parametros tambem vao para <saida>.txt. */

#define SDB_MAXM 64

typedef struct { char phrase[96]; double area; } SdbPhrase;

/* nome de cor em ingles a partir de RGB (HSV): neutros pelo brilho, o resto pelo matiz */
static const char *sdb_color(int r, int g, int b) {
    const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    const double v = mx / 255.0, s = mx ? (double)(mx - mn) / mx : 0.0;
    double h = 0;
    if (mx != mn) {
        if (mx == r) h = 60.0 * ((double)(g - b) / (mx - mn)); else if (mx == g) h = 60.0 * (2.0 + (double)(b - r) / (mx - mn)); else h = 60.0 * (4.0 + (double)(r - g) / (mx - mn));
        if (h < 0) h += 360.0;
    }
    if (v < 0.18) return "black";
    if (s < 0.10) return v > 0.88 ? "white" : v > 0.72 ? "light grey" : v > 0.45 ? "grey" : "dark grey";
    if (h < 15 || h >= 345) return v < 0.55 ? "burgundy" : s < 0.45 ? "dusty rose" : "red";
    if (h < 40) return v < 0.5 ? "brown" : s < 0.45 ? "tan" : "terracotta";
    if (h < 65) return s < 0.25 ? (v > 0.85 ? "off-white" : "warm beige") : v < 0.5 ? "olive brown" : s < 0.5 ? "sand" : "yellow";
    if (h < 165) return v < 0.45 ? "dark green" : s < 0.3 ? "sage green" : "green";
    if (h < 200) return "teal";
    if (h < 250) return s < 0.3 ? "slate blue-grey" : v < 0.4 ? "navy blue" : "blue";
    if (h < 290) return "purple";
    return "dusty pink";
}

static int sdb_has(const char *s, const char *lc) { return ci_find(s, strlen(s), lc, strlen(lc)); }

/* RGB de um texto como "RGB 99.112.123" ou "RGB 119,79,81"; 1 = achou */
static int sdb_rgb(const char *s, int *r, int *g, int *b) {
    const char *p = strstr(s, "RGB");
    if (!p) return 0;
    p += 3;
    int v[3], n = 0;
    while (*p && n < 3) {
        while (*p && !isdigit((unsigned char)*p)) p++;
        if (!*p) break;
        v[n++] = (int)strtol(p, (char **)&p, 10);
        if (n < 3 && (*p == ',' || *p == '.' || *p == ' ')) p++; else if (n < 3) break;
    }
    if (n != 3 || v[0] > 255 || v[1] > 255 || v[2] > 255) return 0;
    *r = v[0]; *g = v[1]; *b = v[2];
    return 1;
}
static double sdb_pct(const char *s) {      /* "(~25% do quadro)" ou "(aproximadamente 7% da moldura)" */
    const char *pc = strchr(s, '%');
    if (!pc) return 0.0;
    const char *q = pc;
    while (q > s && (isdigit((unsigned char)q[-1]) || q[-1] == ',' || q[-1] == '.')) q--;
    return atof(q);
}

/* uma linha de material -> frase em ingles (0 = pular) */
static int sdb_material(const char *name, const char *desc, char *out, size_t cap, double *area) {
    int r, g, b; const int has_rgb = sdb_rgb(desc, &r, &g, &b);
    *area = sdb_pct(desc);
    const int floor_ = sdb_has(name, "piso") || sdb_has(desc, "piso"), led = sdb_has(name, "led") || sdb_has(name, "lamp");
    if (led) return 0;
    const char *noun = NULL;
    /* o TIPO vem so do que o briefing diz depois do nome; um nome como "Arenito" num material do tipo "material" nao vira pedra (o nome e so o rotulo do SketchUp) */
    const int paint = sdb_has(desc, "parede pintada") || sdb_has(desc, "tinta");
    if (sdb_has(desc, "arenito")) noun = "sandstone";
    else if (sdb_has(desc, "travertino")) noun = "travertine";
    else if (sdb_has(desc, "rmore")) noun = "marble";
    else if (sdb_has(desc, "granito")) noun = "granite";
    else if (sdb_has(desc, "pedra")) noun = "stone";
    else if (sdb_has(desc, "porcelana") || sdb_has(desc, "azulejo") || sdb_has(desc, "cer")) noun = floor_ ? "porcelain tile floor" : "porcelain tile";
    else if (sdb_has(desc, "concreto") || sdb_has(desc, "cimento")) noun = floor_ ? "concrete floor" : "concrete";
    else if (sdb_has(desc, "madeira")) noun = "natural wood";
    else if (sdb_has(desc, "tecido")) noun = "fabric";
    else if (sdb_has(desc, "metal")) noun = "metal";
    else if (sdb_has(desc, "laca") || sdb_has(desc, "plast")) noun = "lacquer";
    else if (sdb_has(desc, "vidro")) noun = "glass";
    else if (sdb_has(desc, "espelho")) noun = "mirror";
    else if (sdb_has(desc, "planta")) noun = "foliage";
    if (paint && has_rgb) { snprintf(out, cap, "%s painted wall", sdb_color(r, g, b)); return 1; }
    if (paint) return 0;                                                      /* tinta sem cor conhecida: nao diz nada ao modelo */
    if (noun && has_rgb && strcmp(noun, "foliage") && strcmp(noun, "natural wood")) { snprintf(out, cap, "%s %s", sdb_color(r, g, b), noun); return 1; }
    if (noun) { snprintf(out, cap, "%s", noun); return 1; }
    return 0;
}

/* quantos ids do CLIP tem o texto (sem inicio e fim; um bloco de 75 ids por 75 tokens); sem modelo, estima por palavras */
static int sdb_tokens(const SdClip *c, const char *text) {
    if (!c) { int w = 0, in = 0; for (const char *p = text; *p; p++) { if (isalnum((unsigned char)*p)) { if (!in) { w++; in = 1; } } else in = 0; } return w + w / 2; }
    static int tmp[4096];
    return sdc_ids(c, text, tmp, 4096);
}

typedef struct { char prompt[2400], negative[600]; double elev; int temp, lights_on, chunks_used, kinds, nobj; char objs[8][40]; } SdbOut;

/* categorias de luz: bits */
enum { SL_SPOT = 1, SL_LINEAR = 2, SL_ROUND = 4, SL_PENDANT = 8, SL_MIRROR = 16, SL_COUNTER = 32, SL_WALL = 64, SL_ARCH = 128, SL_SPHERE = 256, SL_SUPPORT = 512 };

static int sdb_light_kind(const char *t) {
    int k = 0;
    if (sdb_has(t, "mr16") || sdb_has(t, "mr11")) k |= SL_SPOT;
    if (sdb_has(t, "linear") || sdb_has(t, "perfil led")) k |= SL_LINEAR;
    if (sdb_has(t, "plafon redondo")) k |= SL_ROUND;
    if (sdb_has(t, "pendente")) k |= SL_PENDANT;
    if (sdb_has(t, "espelho")) k |= SL_MIRROR;
    if (sdb_has(t, "balc")) k |= SL_COUNTER;
    if (sdb_has(t, " parede")) k |= SL_WALL;
    if (sdb_has(t, " arco")) k |= SL_ARCH;
    if (sdb_has(t, "esfera") || sdb_has(t, "bolla")) k |= SL_SPHERE;
    if (sdb_has(t, "apoio")) k |= SL_SUPPORT;
    return k;
}

static const struct { int bit; const char *name; } SDB_LIGHTS[] = { { SL_SPOT, "recessed spotlights" }, { SL_LINEAR, "linear LED strip" }, { SL_ROUND, "round ceiling lamp" }, { SL_PENDANT, "organic pendant lamp" },
    { SL_MIRROR, "mirror light" }, { SL_COUNTER, "counter spotlights" }, { SL_WALL, "wall washer lights" }, { SL_ARCH, "arch accent lighting" }, { SL_SPHERE, "sphere lamp" }, { SL_SUPPORT, "support light" } };

/* acrescenta ", frase" ao prompt se o total de ids continuar dentro de budget; 1 = coube */
static int sdb_add(const SdClip *clip, char *pr, size_t cap, const char *phrase, int budget) {
    char trial[2400]; snprintf(trial, sizeof trial, "%s, %s", pr, phrase);
    if (sdb_tokens(clip, trial) > budget) return 0;
    snprintf(pr, cap, "%s", trial);
    return 1;
}

static int sdb_build(const char *text, const SdClip *clip, int chunks, SdbOut *o) {
    SdbPhrase ph[SDB_MAXM]; int np = 0;
    int sec = 0, plants_listed = 0, kinds = 0; int temps[256]; int ntemps = 0;
    char objs[8][40]; int nobj = 0;
    o->elev = -1; o->temp = 0; o->lights_on = 0; o->kinds = 0; o->nobj = 0;
    const int budget = chunks * 75 - 4;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        char line[1200]; size_t l = (size_t)(e - p); if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l); line[l] = 0;
        p = *e ? e + 1 : e;
        const char *t = line; while (*t == ' ' || *t == '\t') t++;
        if (!*t) continue;
        if (!strncmp(t, "MATERIAIS", 9)) { sec = 1; continue; }
        if (!strncmp(t, "PRINCIPAIS OBJETOS", 18)) { sec = 2; continue; }
        if (!strncmp(t, "ILUMINA", 7)) { sec = 3; continue; }
        if (!strncmp(t, "ESTILO", 6)) { sec = 4; continue; }
        if (!strncmp(t, "REGRAS", 6)) { sec = 5; continue; }
        if (sec == 1 && *t == '-') {
            const char *q1 = strchr(t, '"'); const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2) continue;
            char name[160]; size_t nl = (size_t)(q2 - q1 - 1); if (nl >= sizeof name) nl = sizeof name - 1;
            memcpy(name, q1 + 1, nl); name[nl] = 0;
            const char *desc = q2 + 1;
            if (sdb_has(name, "folha") || sdb_has(desc, "planta")) plants_listed = 1;
            char phr[96]; double area;
            if (!sdb_material(name, desc, phr, sizeof phr, &area)) continue;
            int found = -1; for (int i = 0; i < np; i++) if (!strcmp(ph[i].phrase, phr)) found = i;
            if (found >= 0) ph[found].area += area;
            else if (np < SDB_MAXM) { snprintf(ph[np].phrase, sizeof ph[np].phrase, "%s", phr); ph[np].area = area; np++; }
        } else if (sec == 2 && *t == '-') {
            static const char *const key[][2] = { { "bal", "reception counter" }, { "cadeira", "chair" }, { "poltrona", "armchair" }, { "sof", "sofa" }, { "mesa", "table" }, { "espelho", "mirror" }, { "cama", "bed" }, { "maca", "treatment bed" } };
            for (int k = 0; k < 8; k++) {
                if (!sdb_has(t, key[k][0])) continue;
                if (k == 0 && !sdb_has(t, "atendimento") && !sdb_has(t, "balc")) continue;
                int dup = 0; for (int i = 0; i < nobj; i++) if (!strcmp(objs[i], key[k][1])) dup = 1;
                if (!dup && nobj < 8) snprintf(objs[nobj++], sizeof objs[0], "%s", key[k][1]);
            }
        } else if (sec == 3 || sdb_has(t, "luz do dia")) {
            if (sdb_has(t, "eleva")) { const char *ev = strstr(t, "eleva"); const char *d = ev; while (*d && !isdigit((unsigned char)*d)) d++; if (*d) { char num[16]; size_t k = 0; while ((isdigit((unsigned char)*d) || *d == ',' || *d == '.') && k < 15) { num[k++] = *d == ',' ? '.' : *d; d++; } num[k] = 0; o->elev = atof(num); } }
            if ((strstr(t, "LIGAD") || strstr(t, "ON")) && sdb_has(t, " k")) {
                const char *k = strstr(t, ", ");                          /* "...: LIGADA, 3000 K, ~500 lm": a temperatura e o numero antes de " K" depois dos dois pontos */
                const char *colon = strstr(t, "\": "); if (!colon) colon = strchr(t, ':');
                if (colon) { const char *kk = strstr(colon, " K"); if (kk) { const char *d = kk; while (d > colon && isdigit((unsigned char)d[-1])) d--; const int tv = atoi(d); if (tv >= 1500 && tv <= 8000 && ntemps < 256) temps[ntemps++] = tv; } }
                (void)k;
                o->lights_on++;
                kinds |= sdb_light_kind(t);
            }
        }
    }
    if (ntemps) { int best = 0, bc = 0; for (int i = 0; i < ntemps; i++) { int c = 0; for (int j = 0; j < ntemps; j++) if (temps[j] == temps[i]) c++; if (c > bc) { bc = c; best = temps[i]; } } o->temp = best; }
    o->kinds = kinds; o->nobj = nobj; memcpy(o->objs, objs, sizeof objs);
    for (int i = 1; i < np; i++) { SdbPhrase v = ph[i]; int j = i - 1; while (j >= 0 && ph[j].area < v.area) { ph[j + 1] = ph[j]; j--; } ph[j + 1] = v; }      /* maior area primeiro */
    char pr[2400]; snprintf(pr, sizeof pr, "photorealistic architectural interior photograph of a reception");
    for (int i = 0; i < nobj; i++) sdb_add(clip, pr, sizeof pr, objs[i], budget);
    for (int i = 0; i < np; i++) if (!sdb_add(clip, pr, sizeof pr, ph[i].phrase, budget)) break;
    const char *day = o->elev >= 60 ? "bright daylight from a high sun" : o->elev >= 30 ? "soft daylight" : o->elev >= 0 ? "low warm sunlight" : NULL;
    if (day) sdb_add(clip, pr, sizeof pr, day, budget);
    if (o->temp) { char w[80]; snprintf(w, sizeof w, "%s artificial lighting %dK", o->temp <= 3200 ? "warm" : o->temp >= 4500 ? "cool white" : "neutral", o->temp); sdb_add(clip, pr, sizeof pr, w, budget); }
    if (kinds) {
        char lp[400] = "lit by"; int first = 1;
        for (int i = 0; i < 10; i++) if (kinds & SDB_LIGHTS[i].bit) { strncat(lp, first ? " " : ", ", sizeof lp - strlen(lp) - 1); strncat(lp, SDB_LIGHTS[i].name, sizeof lp - strlen(lp) - 1); first = 0; }
        sdb_add(clip, pr, sizeof pr, lp, budget);
    }
    /* estilo: cada pedido do texto vira uma frase fixa */
    static const struct { const char *pt; const char *en; } S[] = {
        { "natural e realista", "natural realistic interior photography" }, { "full-frame", "shot with a full-frame camera" }, { "sem apar", "not a computer graphics look" },
        { "atenua", "physically plausible soft light with real falloff" }, { "contato", "deep contact shadows under the furniture" }, { "gradientes", "soft light gradients on the walls" },
        { "texturas", "visible material textures, wood grain, fabric weave" }, { "quentes", "warm artificial lights" }, { "nunca supersaturadas", "faithful natural colors, never oversaturated" },
        { "balan", "neutral to slightly warm white balance" }, { "elegante", "clean elegant authentic" } };
    for (int i = 0; i < (int)(sizeof S / sizeof S[0]); i++) if (sdb_has(text, S[i].pt)) sdb_add(clip, pr, sizeof pr, S[i].en, budget);
    sdb_add(clip, pr, sizeof pr, "high quality", budget);
    snprintf(o->prompt, sizeof o->prompt, "%s", pr);
    o->chunks_used = (sdb_tokens(clip, pr) + 74) / 75;
    char neg[600] = "text, logo, watermark, people, cartoon, sketch, drawing, outline, lines, oversaturated, lowres, blurry, deformed";
    if (sdb_has(text, "n\xc3\xa3o adicione") || sdb_has(text, "sem decora") || sdb_has(text, "nao adicione")) strncat(neg, ", extra decoration, extra objects", sizeof neg - strlen(neg) - 1);
    if (!plants_listed) strncat(neg, ", plants", sizeof neg - strlen(neg) - 1);
    snprintf(o->negative, sizeof o->negative, "%s", neg);
    return np;
}

/* ---------- prompt em prosa para o FLUX (render generate --engine flux --brief; render spec --brief) ----------
 * O FLUX le prosa e nao tem limite de 75 tokens nem prompt negativo. Aqui o briefing vira frases por regra, sem resumo a mao:
 *   - cada material com a cor (RGB -> nome), a escala do padrao em cm e a parte do quadro; iguais somam a area; da maior para a menor;
 *   - o TIPO do material vem so do que o briefing escreve depois do nome. "Arenito" do tipo "material" fica "surface material \"Arenito\"" (nunca
 *     pedra); "AK_PISO CONCRETO" e piso de concreto, com o tamanho de padrao dito; nome desconhecido fica como superficie neutra;
 *   - materiais abaixo de 1% do quadro (os "~0%") nao entram; luzes, objetos, sol e estilo vem das mesmas leituras do prompt do Stable;
 *   - as regras de fidelidade (camera, geometria, nada acrescentado) entram sempre. */

typedef struct { char phrase[240]; double area; } SdbFx;

/* "275x184" ou "275\xc3\x97""184" (cm) no texto; 1 = achou */
static int sdb_pattern(const char *s, int *a, int *b) {
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p) || (p > s && (isdigit((unsigned char)p[-1]) || p[-1] == '.' || p[-1] == ','))) continue;
        char *q; const long x = strtol(p, &q, 10);
        if (*q == 'x' || *q == 'X') q++; else if ((unsigned char)q[0] == 0xC3 && (unsigned char)q[1] == 0x97) q += 2; else { p = q - 1; continue; }
        if (!isdigit((unsigned char)*q)) { p = q - 1; continue; }
        *a = (int)x; *b = (int)strtol(q, NULL, 10);
        return 1;
    }
    return 0;
}

/* uma linha de material -> frase do FLUX (0 = pular: abaixo de 1% do quadro) */
static int sdb_fx_material(const char *name, const char *desc, char *out, size_t cap, double *area) {
    int r = 0, g = 0, b = 0, pa = 0, pb = 0;
    const int has_rgb = sdb_rgb(desc, &r, &g, &b), has_pat = sdb_pattern(desc, &pa, &pb), has_pct = strchr(desc, '%') != NULL;
    *area = sdb_pct(desc);
    if (has_pct && *area < 1.0) return 0;
    char type[120] = ""; {
        const char *t = desc; while (*t == ':' || *t == ' ') t++;
        size_t n = 0;
        while (t[n] && n < sizeof type - 1 && t[n] != ',' && t[n] != '(' && !((unsigned char)t[n] == 0xE2 && (unsigned char)t[n + 1] == 0x80 && (unsigned char)t[n + 2] == 0x94)) n++;
        memcpy(type, t, n); type[n] = 0;
    }
    const int floor_ = sdb_has(name, "piso"), conc_name = sdb_has(name, "concreto");
    char col[40] = ""; if (has_rgb) snprintf(col, sizeof col, "%s", sdb_color(r, g, b));
    char rgbs[40] = ""; if (has_rgb) snprintf(rgbs, sizeof rgbs, " (RGB %d,%d,%d)", r, g, b);      /* so a tinta leva os numeros: o FLUX nao precisa deles nos outros */
    char what[200];
    if (sdb_has(type, "parede pintada") || sdb_has(type, "tinta")) { if (has_rgb) snprintf(what, sizeof what, "%s painted wall%s", col, rgbs); else snprintf(what, sizeof what, "painted wall surface, colour not stated"); }
    else if (sdb_has(type, "concreto") || sdb_has(type, "cimento")) snprintf(what, sizeof what, "%s", floor_ ? "concrete floor tiles" : "concrete");
    else if (sdb_has(type, "porcel") || sdb_has(type, "azulejo") || sdb_has(type, "cer")) snprintf(what, sizeof what, "%s", floor_ ? (conc_name ? "concrete-look porcelain floor tiles" : "porcelain floor tiles") : "porcelain tile");
    else if (sdb_has(type, "madeira")) snprintf(what, sizeof what, "natural wood \"%s\"%s%s", name, has_rgb ? ", " : "", col);
    else if (sdb_has(type, "tecido")) snprintf(what, sizeof what, "fabric \"%s\"%s%s", name, has_rgb ? ", " : "", col);
    else if (sdb_has(type, "metal")) snprintf(what, sizeof what, "%s%smetal", col, has_rgb ? " " : "");
    else if (sdb_has(type, "laca") || sdb_has(type, "plast")) snprintf(what, sizeof what, "%s%slacquer", col, has_rgb ? " " : "");
    else if (sdb_has(type, "vidro")) snprintf(what, sizeof what, "glass");
    else if (sdb_has(type, "espelho")) snprintf(what, sizeof what, "mirror");
    else if (sdb_has(type, "planta")) snprintf(what, sizeof what, "foliage \"%s\"", name);
    else snprintf(what, sizeof what, "surface material \"%s\"%s%s", name, has_rgb ? ", colour " : "", col);      /* tipo "material" ou desconhecido: neutro, sem inferir pelo nome */
    int n = snprintf(out, cap, "%s", what);
    if (has_pat && n > 0 && (size_t)n < cap) snprintf(out + n, cap - (size_t)n, ", pattern %dx%d cm", pa, pb);
    return 1;
}

static int sdb_fx_materials(const char *text, char *out, size_t cap, int maxn) {
    SdbFx ph[SDB_MAXM]; int np = 0, sec = 0;
    out[0] = 0;
    for (const char *p = text; *p; ) {
        const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        char line[1200]; size_t l = (size_t)(e - p); if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l); line[l] = 0;
        p = *e ? e + 1 : e;
        const char *t = line; while (*t == ' ' || *t == '\t') t++;
        if (!strncmp(t, "MATERIAIS", 9)) { sec = 1; continue; }
        if (!strncmp(t, "PRINCIPAIS", 10) || !strncmp(t, "ILUMINA", 7) || !strncmp(t, "ESTILO", 6) || !strncmp(t, "REGRAS", 6)) { sec = 0; continue; }
        if (sec != 1 || *t != '-') continue;
        const char *q1 = strchr(t, '"'); const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!q1 || !q2) continue;
        char name[160]; size_t nl = (size_t)(q2 - q1 - 1); if (nl >= sizeof name) nl = sizeof name - 1;
        memcpy(name, q1 + 1, nl); name[nl] = 0;
        char phr[240]; double area;
        if (!sdb_fx_material(name, q2 + 1, phr, sizeof phr, &area)) continue;
        int found = -1; for (int i = 0; i < np; i++) if (!strcmp(ph[i].phrase, phr)) found = i;
        if (found >= 0) ph[found].area += area;
        else if (np < SDB_MAXM) { snprintf(ph[np].phrase, sizeof ph[np].phrase, "%s", phr); ph[np].area = area; np++; }
    }
    for (int i = 1; i < np; i++) { SdbFx v = ph[i]; int j = i - 1; while (j >= 0 && ph[j].area < v.area) { ph[j + 1] = ph[j]; j--; } ph[j + 1] = v; }
    if (np > maxn) np = maxn;
    for (int i = 0; i < np; i++) {
        char item[300];
        if (ph[i].area > 0) snprintf(item, sizeof item, "%s%s, %.0f%%", i ? "; " : "", ph[i].phrase, ph[i].area);
        else snprintf(item, sizeof item, "%s%s", i ? "; " : "", ph[i].phrase);
        if (strlen(out) + strlen(item) + 1 < cap) strcat(out, item);
    }
    return np;
}

/* o briefing inteiro -> prosa em ingles para o FLUX; extra = pedido adicional do usuario (--nl), opcional; has_lines = ha desenho linear (--init2) */
static void sdb_flux_prose_n(const char *text, int has_lines, const char *extra, char *out, size_t cap, int maxn, int *nmat) {
    SdbOut *o = xalloc(sizeof *o, 1);
    sdb_build(text, NULL, 16, o);
    char mats[2400]; *nmat = sdb_fx_materials(text, mats, sizeof mats, maxn);
    const int ban = sdb_has(text, "sem decora") || sdb_has(text, "n\xc3\xa3o adicione") || sdb_has(text, "nao adicione");
    const char *room = sdb_has(text, "recep") ? "a reception" : "an interior";
    int n = snprintf(out, cap, "Photorealistic architectural interior photograph of %s, made from the reference view (a 3D model export). "
        "Keep the exact camera position, lens, framing, perspective and proportions. "
        "Every wall, opening, ceiling, floor, piece of furniture and object keeps its position, size and shape: nothing is added, removed, moved or resized%s. ",
        room, ban ? ", no extra decoration, plants, people, lamps, artwork or written text" : "");
    if (has_lines) n += snprintf(out + n, cap - (size_t)n, "Every edge of the line drawing stays in place. ");
    if (mats[0]) n += snprintf(out + n, cap - (size_t)n, "Each material sits exactly on its surfaces in the view, with its real colour and pattern scale (share of the frame in %%): %s. ", mats);
    if (o->nobj) {
        n += snprintf(out + n, cap - (size_t)n, "Main objects:");
        for (int i = 0; i < o->nobj; i++) n += snprintf(out + n, cap - (size_t)n, "%s %s", i ? "," : "", o->objs[i]);
        n += snprintf(out + n, cap - (size_t)n, ". ");
    }
    const char *day = o->elev >= 60 ? "bright daylight from a high sun" : o->elev >= 30 ? "soft daylight" : o->elev >= 0 ? "low warm sunlight" : NULL;
    if (day || o->temp || o->kinds) {
        n += snprintf(out + n, cap - (size_t)n, "Lighting: ");
        if (day) n += snprintf(out + n, cap - (size_t)n, "%s (solar elevation %.1f degrees) matching the shadows of the view; ", day, o->elev);
        if (o->temp) n += snprintf(out + n, cap - (size_t)n, "%s artificial light at %d K", o->temp <= 3200 ? "warm" : o->temp >= 4500 ? "cool white" : "neutral", o->temp);
        if (o->kinds) {
            n += snprintf(out + n, cap - (size_t)n, " from");
            int first = 1;
            for (int i = 0; i < 10; i++) if (o->kinds & SDB_LIGHTS[i].bit) { n += snprintf(out + n, cap - (size_t)n, "%s %s", first ? "" : ",", SDB_LIGHTS[i].name); first = 0; }
        }
        n += snprintf(out + n, cap - (size_t)n, "; unlisted luminaires stay off. ");
    }
    static const struct { const char *pt, *en; } S[] = {
        { "natural e realista", "natural realistic interior photography" }, { "full-frame", "full-frame camera" }, { "sem apar", "no computer graphics look" },
        { "atenua", "soft light with real falloff" }, { "contato", "deep contact shadows" }, { "gradientes", "soft light gradients on the walls" },
        { "texturas", "visible wood grain and fabric weave" }, { "nunca supersaturadas", "faithful colours, never oversaturated" },
        { "balan", "neutral to slightly warm white balance" }, { "elegante", "clean, elegant, authentic" } };
    int first = 1;
    for (int i = 0; i < (int)(sizeof S / sizeof S[0]); i++) if (sdb_has(text, S[i].pt)) { n += snprintf(out + n, cap - (size_t)n, "%s%s", first ? "Style: " : ", ", S[i].en); first = 0; }
    if (!first) n += snprintf(out + n, cap - (size_t)n, ". ");
    if (extra && extra[0]) n += snprintf(out + n, cap - (size_t)n, "Additional request from the user: %s. ", extra);
    snprintf(out + n, cap - (size_t)n, "16:9, no borders, no watermark.");
    free(o);
}

/* o texto do FLUX tem 512 posicoes (cerca de 0.285 ficha por caractere deste texto; 1740 caracteres sao cerca de 500): se o briefing tem materiais demais, os de menor area ficam de fora */
static void sdb_flux_prose(const char *text, int has_lines, const char *extra, char *out, size_t cap) {
    int nmat = 0;
    sdb_flux_prose_n(text, has_lines, extra, out, cap, 99, &nmat);
    const int all = nmat;
    for (int k = nmat; k > 0 && strlen(out) > 1740; k--) sdb_flux_prose_n(text, has_lines, extra, out, cap, k - 1, &nmat);
    if (nmat < all) fprintf(stderr, "[brief] %d material(is) de menor area ficaram de fora do prompt do FLUX (limite de 512 posicoes de texto)\n", all - nmat);
}

/* arquivo -> texto (malloc) ou NULL */
static char *sdb_read_text(const char *path) {
    int64_t sz = 0; const uint8_t *b = map_file(path, &sz);
    if (!b) { fprintf(stderr, "render: nao consegui abrir %s\n", path); return NULL; }
    char *t = xalloc((size_t)sz + 1, 1); memcpy(t, b, (size_t)sz);
    return t;
}

/* generate --engine stable --brief: o mesmo briefing no prompt condensado do Stable (sdb_build) */
static int sdb_fill(SdpParams *P, const char *path) {
    char *text = sdb_read_text(path); if (!text) return 0;
    SdgFile base; SdClip clip; const SdClip *cp = NULL; char mp[1100];
    snprintf(mp, sizeof mp, "%s/sd15-realistic-vision.gguf", P->models);
    if (sdg_open(&base, mp) && sdc_load(&clip, &base)) cp = &clip;
    static SdbOut o; sdb_build(text, cp, P->max_chunks, &o);
    P->prompt = o.prompt; P->negative = o.negative;
    fprintf(stderr, "[brief] prompt do Stable: %s\n[brief] negativo: %s\n", o.prompt, o.negative);
    return 1;
}

/* ---------- teste: o briefing da recepcao (formato do servico de fora; texto sintetico com as mesmas linhas de material) ---------- */
static const char SDB_TEST_BRIEF[] =
    "# Briefing - RECEPCAO V1\n\nREGRAS ABSOLUTAS (fidelidade ao projeto do arquiteto):\n"
    "- Mantenha a posi\xc3\xa7\xc3\xa3o exata da c\xc3\xa2mera, lente, enquadramento, perspectiva e propor\xc3\xa7\xc3\xa3o da IMAGEM 1.\n"
    "Mantenha todas as paredes, aberturas, tetos, pisos, m\xc3\xb3veis e objetos na mesma posi\xc3\xa7\xc3\xa3o. N\xc3\x83O adicione, remova, mova, redimensione ou altere nada. Sem decora\xc3\xa7\xc3\xa3o extra, plantas, pessoas, lumin\xc3\xa1rias, obras de arte ou textos.\n\n"
    "MATERIAIS (nome: tipo \xe2\x80\x94 acabamento, marca, c\xc3\xb3digo, cor real, observa\xc3\xa7\xc3\xb5\x65s):\n"
    "- \"AK_TINTA CORAL CORRIDA DE RUA\": parede pintada \xe2\x80\x94 RGB 99.112.123 (~25% do quadro)\n"
    "- \"AK_TINTA SUVINIL LICOR DE CASSIS\": parede pintada \xe2\x80\x94 RGB 119,79,81 (~23% do quadro)\n"
    "- \"AK_TINTA SUVINIL PALAVRAS CRUZADAS\": parede pintada \xe2\x80\x94 RGB 228.222.204 (~10% do quadro)\n"
    "- \"Arenito\": material, repeti\xc3\xa7\xc3\xa3o do padr\xc3\xa3o 275\xc3\x97" "184 cm (aproximadamente 7% da moldura)\n"
    "- \"H10248\": tecido, repeti\xc3\xa7\xc3\xa3o do padr\xc3\xa3o 40\xc3\x97" "40 cm (aproximadamente 4% da moldura)\n"
    "- \"ANGRA_21_PRATA\": material, repeti\xc3\xa7\xc3\xa3o do padr\xc3\xa3o 187\xc3\x97" "281 cm (~4% da moldura)\n"
    "- \"AK_PISO CONCRETO 04#1\": concreto/cimento, repeti\xc3\xa7\xc3\xa3o do padr\xc3\xa3o 90\xc3\x97" "90 cm (aproximadamente 4% da moldura)\n"
    "- \"HIBISCO\": material, repeti\xc3\xa7\xc3\xa3o do padr\xc3\xa3o 10\xc3\x97" "10 cm (aproximadamente 4% da moldura)\n"
    "- \"AK_PISO CONCRETO 03#1\": concreto/cimento, repeti\xc3\xa7\xc3\xa3o do padr\xc3\xa3o 90\xc3\x97" "90 cm (aproximadamente 3% da moldura)\n"
    "- \"AK_ PISO CONCRETO 01\": azulejo de porcelana/cer\xc3\xa2mica, repeti\xc3\xa7\xc3\xa3o do padr\xc3\xa3o 100\xc3\x97" "100 cm (aproximadamente 3% da moldura)\n"
    "- \"demoli\xc3\xa7\xc3\xa3o\": madeira natural, padr\xc3\xa3o repetido 121\xc3\x97" "181 cm (~2% da moldura)\n"
    "- \"Iron_Blurry_5cm\": metal \xe2\x80\x94 RGB 60,60,60 (~1% do quadro)\n"
    "- \"FOLHA BEGE\": plantas, repeti\xc3\xa7\xc3\xa3o de padr\xc3\xa3o 14\xc3\x97" "158 cm (~0% do quadro)\n"
    "- \"Laca Preta\": pl\xc3\xa1stico/laca \xe2\x80\x94 RGB 25,25,25 (~0% da moldura)\n\n"
    "PRINCIPAIS OBJETOS \xc3\x80 VISTA:\n- BALC\xc3\x83O DE ATENDIMENTO#1\n- Cadeira Delgada\n- Componente n\xc2\xba 27\n\n"
    "ILUMINA\xc3\x87\xc3\x83O:\nLuz do dia: eleva\xc3\xa7\xc3\xa3o solar 66,5\xc2\xb0, azimute 282,0\xc2\xb0 (dia (sol alto)).\n"
    "- luz \"PLAFON LINEAR\" no canto superior direito: LIGADA, 3000 K, ~500 lm\n- Luz \"MR16 3000K\" no centro superior: LIGADA, 3000 K, ~500 lm\n- Luz \"PERFIL LED\" no centro: LIGADA, 3000 K, ~500 lm\n\n"
    "ESTILO FOTOGR\xc3\x81" "FICO:\nFotografia arquitet\xc3\xb4nica de interiores natural e realista, capturada com uma c\xc3\xa2mera full-frame, sem apar\xc3\xaancia de computa\xc3\xa7\xc3\xa3o gr\xc3\xa1" "fica.\n"
    "texturas vis\xc3\xadveis dos materiais (gr\xc3\xa3os da madeira, trama do tecido, veios da pedra, relevo das ripas). Cores fi\xc3\xa9is, nunca supersaturadas;\n";

static int sdb_tck(const char *what, int ok) { printf("  %-70s %s\n", what, ok ? "OK" : "FALHOU"); return ok ? 0 : 1; }

static int sdb_selftest(const char *file) {
    int bad = 0; char *real = file ? sdb_read_text(file) : NULL;
    printf("katana render brief selftest (prompt do FLUX a partir do briefing)\n");
    for (int pass = 0; pass < (real ? 2 : 1); pass++) {
        const char *text = pass ? real : SDB_TEST_BRIEF;
        static char pr[6000], fit[6000]; int nm = 0; sdb_flux_prose_n(text, 1, NULL, pr, sizeof pr, 99, &nm); sdb_flux_prose(text, 1, NULL, fit, sizeof fit);
        static SdbOut o; sdb_build(text, NULL, 4, &o);
        printf("%s\n  prompt do FLUX (%zu caracteres): %s\n", pass ? "briefing do arquivo:" : "texto embutido (mesmas linhas de material):", strlen(pr), pr);
        char low[6000]; size_t n = strlen(pr); for (size_t i = 0; i <= n; i++) low[i] = (char)tolower((unsigned char)pr[i]);
        bad += sdb_tck("nenhuma pedra: sem 'stone', 'sandstone', 'pedra' (flux)", !strstr(low, "stone") && !strstr(low, "pedra"));
        bad += sdb_tck("nenhuma pedra no prompt do Stable (sandstone/stone)", !strstr(o.prompt, "stone"));
        bad += sdb_tck("Arenito fica superficie neutra com o padrao 275x184 cm", strstr(pr, "surface material \"Arenito\", pattern 275x184 cm") != NULL);
        bad += sdb_tck("piso: concrete floor tiles, 90x90 cm, areas somadas", strstr(pr, "concrete floor tiles, pattern 90x90 cm, ") != NULL);
        bad += sdb_tck("piso de nome CONCRETO e tipo porcelana: concrete-look, 100x100 cm", strstr(pr, "concrete-look porcelain floor tiles, pattern 100x100 cm") != NULL);
        bad += sdb_tck("cores das paredes: burgundy, slate blue-grey, off-white com RGB", strstr(pr, "burgundy painted wall (RGB 119,79,81)") && strstr(pr, "slate blue-grey painted wall (RGB 99,112,123)") && strstr(pr, "off-white painted wall (RGB 228,222,204)"));
        bad += sdb_tck("parte do quadro: 25% e a maior area vem primeiro", strstr(pr, "(share of the frame in %): slate blue-grey painted wall (RGB 99,112,123), 25%;") != NULL);
        bad += sdb_tck("tecido e madeira so pelo tipo dito no briefing", strstr(pr, "fabric \"H10248\"") && strstr(pr, "natural wood \"demoli"));
        bad += sdb_tck("nome sem tipo (ANGRA_21_PRATA, HIBISCO) fica neutro, sem material inferido", strstr(pr, "surface material \"ANGRA_21_PRATA\"") && strstr(pr, "surface material \"HIBISCO\"") && !strstr(low, "silver"));
        bad += sdb_tck("itens de 0% do quadro nao entram (FOLHA BEGE, Laca Preta)", !strstr(pr, "FOLHA") && !strstr(low, "lacquer"));
        bad += sdb_tck("regras de fidelidade: camera, geometria, nada acrescentado", strstr(pr, "Keep the exact camera position, lens, framing") && strstr(pr, "nothing is added, removed, moved or resized") && strstr(pr, "written text"));
        bad += sdb_tck("desenho linear mencionado quando ha a imagem 2", strstr(pr, "line drawing") != NULL);
        bad += sdb_tck("luz: elevacao solar e 3000 K, spots/perfil de LED lidos", strstr(pr, "solar elevation 66.5") && strstr(pr, "3000 K"));
        bad += sdb_tck("objetos: reception counter, chair", strstr(pr, "Main objects: reception counter, chair") != NULL);
        bad += sdb_tck("prosa final cabe nas 512 posicoes do texto do FLUX (ate 1740 caracteres, cerca de 0.285 ficha cada)", strlen(fit) <= 1740 || strstr(fit, "Materials") == NULL);
    }
    printf("RESULTADO: %s\n", bad ? "FALHOU" : "todos passaram");
    return bad ? 1 : 0;
}

static int sdb_main(int argc, char **argv) {
    if (argc >= 1 && !strcmp(argv[0], "selftest")) return sdb_selftest(argc > 1 ? argv[1] : NULL);
    if (argc < 1) { fprintf(stderr, "uso: katana render brief <briefing.md> --image1 vista.png --out saida.png [--image2 linhas.png] [--image3 amostras.png] [--show]\n"
                                    "       [--size WxH] [--steps 20] [--seed 1] [--strength 0.55] [--guidance 7.0] [--control-scale 1.0] [--chunks 4] [--lcm] [--long 576] [--models pasta]\n"); return 1; }
    const char *brief = argv[0], *img1 = NULL, *img2 = NULL, *img3 = NULL, *out = NULL; int show = 0, steps_set = 0; double longside = 576.0;
    SdpParams P; memset(&P, 0, sizeof P);
    P.steps = 20; P.guidance = 7.0; P.strength = 0.55; P.control_scale = 1.0; P.canny_lo = 100; P.canny_hi = 200; P.seed = 1; P.max_chunks = 4;
    static char models[1100]; snprintf(models, sizeof models, "%s/render", models_dir()); P.models = models;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i]; const int more = i + 1 < argc;
        if (!strcmp(a, "--image1") && more) img1 = argv[++i];
        else if (!strcmp(a, "--image2") && more) img2 = argv[++i];
        else if (!strcmp(a, "--image3") && more) img3 = argv[++i];
        else if (!strcmp(a, "--out") && more) out = argv[++i];
        else if (!strcmp(a, "--show")) show = 1;
        else if (!strcmp(a, "--size") && more) { if (sscanf(argv[++i], "%dx%d", &P.w, &P.h) != 2) { fprintf(stderr, "render: --size espera LARGURAxALTURA\n"); return 1; } }
        else if (!strcmp(a, "--lcm")) P.lcm = 1;
        else if (!strcmp(a, "--long") && more) longside = atof(argv[++i]);
        else if (!strcmp(a, "--steps") && more) { P.steps = atoi(argv[++i]); steps_set = 1; }
        else if (!strcmp(a, "--seed") && more) P.seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--strength") && more) P.strength = atof(argv[++i]);
        else if (!strcmp(a, "--guidance") && more) P.guidance = atof(argv[++i]);
        else if (!strcmp(a, "--control-scale") && more) P.control_scale = atof(argv[++i]);
        else if (!strcmp(a, "--chunks") && more) P.max_chunks = atoi(argv[++i]);
        else if (!strcmp(a, "--models") && more) P.models = argv[++i];
        else { fprintf(stderr, "render brief: opcao desconhecida ou sem valor: %s\n", a); return 1; }
    }
    if (!img1 || (!out && !show)) { fprintf(stderr, "render brief: faltam --image1 e --out (ou --show)\n"); return 1; }
    if (P.lcm && !steps_set) P.steps = 4;
    if (P.max_chunks < 1) P.max_chunks = 1;
    if (P.max_chunks > 16) P.max_chunks = 16;
    int64_t sz = 0; const uint8_t *b = map_file(brief, &sz);
    if (!b) { fprintf(stderr, "render brief: nao consegui abrir %s\n", brief); return 1; }
    char *text = xalloc((size_t)sz + 1, 1); memcpy(text, b, (size_t)sz);
    SdgFile base; SdClip clip; const SdClip *cp = NULL; char path[1100];
    snprintf(path, sizeof path, "%s/sd15-realistic-vision.gguf", P.models);
    if (sdg_open(&base, path) && sdc_load(&clip, &base)) cp = &clip;
    SdbOut o; const int nm = sdb_build(text, cp, P.max_chunks, &o);
    fprintf(stderr, "[brief] %d tipos de material, %d luzes ligadas lidas, elevacao solar %.1f, temperatura %d K\n[brief] prompt (%d tokens, %d bloco(s) de 75): %s\n[brief] negativo: %s\n",
            nm, o.lights_on, o.elev, o.temp, sdb_tokens(cp, o.prompt), o.chunks_used, o.prompt, o.negative);
    if (img3) fprintf(stderr, "[brief] --image3 (amostras de material) nao e usada: o Stable Diffusion 1.5 nao toma uma textura como referencia; valem os nomes e as cores do briefing\n");
    if (show) return 0;
    P.prompt = o.prompt; P.negative = o.negative; P.out = out; P.init = img1; P.control = img2 ? img2 : img1; P.control_lines = img2 != NULL;
    if (!P.w || !P.h) {                                              /* proporcao da vista, lado maior 576 */
        int iw, ih;
        if (sdp_peek_size(img1, &iw, &ih)) { const double s = longside / (iw > ih ? iw : ih); P.w = sdp_round64(iw * s); P.h = sdp_round64(ih * s); }
    }
    char tp[1200]; snprintf(tp, sizeof tp, "%s.txt", out);
    FILE *tf = fopen(tp, "w");
    if (tf) { fprintf(tf, "prompt: %s\nnegative: %s\nsize: %dx%d  steps: %d  guidance: %.1f  strength: %.2f  control_scale: %.2f  seed: %llu  chunks: %d  control: %s\nimage1: %s\nimage2: %s\n", o.prompt, o.negative, P.w, P.h, P.steps, P.guidance, P.strength, P.control_scale, (unsigned long long)P.seed, P.max_chunks, img2 ? "line drawing (image2)" : "canny of image1", img1, img2 ? img2 : "(none)"); fclose(tf); }
    return sdp_generate(&P);
}
