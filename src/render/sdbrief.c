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

static int fxt_count(const char *prompt);                  /* render/fxtext.c: tokens do prompt como o FLUX os conta (-1 = sem o tokenizador) */

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
    if (!c) { int w = 0, in = 0, pw = 0; for (const char *p = text; *p; p++) { if (*p == '(') pw++; if (isalnum((unsigned char)*p)) { if (!in) { w++; in = 1; } } else in = 0; } return w + w / 2 + 4 * pw; }
    static int tmp[4096]; static float wt[4096];
    return sdc_ids_w(c, text, tmp, wt, 4096);
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

/* ---------- as unidades do briefing (sdsel.c): uma leitura serve aos dois motores ---------- */

static int sdb_pattern(const char *s, int *a, int *b);
static int sdb_fx_material(const char *name, const char *desc, char *out, size_t cap, double *area);

typedef struct { const char *pt, *flux, *sd; double prior; } SdbStyle;
static const SdbStyle SDB_STYLES[] = {
    { "nunca supersaturadas", "faithful colours, never oversaturated", "faithful natural colors, never oversaturated", 1.2 },
    { "texturas", "visible wood grain and fabric weave", "visible material textures, wood grain, fabric weave", 1.0 },
    { "balan", "neutral to slightly warm white balance", "neutral to slightly warm white balance", 0.8 },
    { "contato", "deep contact shadows", "deep contact shadows under the furniture", 0.7 },
    { "natural e realista", "natural realistic interior photography", "natural realistic interior photography", 0.6 },
    { "sem apar", "no computer graphics look", "not a computer graphics look", 0.5 },
    { "atenua", "soft light with real falloff", "physically plausible soft light with real falloff", 0.5 },
    { "full-frame", "full-frame camera", "shot with a full-frame camera", 0.4 },
    { "gradientes", "soft light gradients on the walls", "soft light gradients on the walls", 0.4 },
    { "quentes", "", "warm artificial lights", 0.3 },
    { "elegante", "clean, elegant, authentic", "clean elegant authentic", 0.2 } };

typedef struct { SdsUnit u[SDS_MAXU]; int n; double elev; int temp, lights_on, kinds, nobj, nmat; char room[16], objs[8][40]; int ban, plants_listed; const SdClip *clip; } SdbRead;

static void sdb_app(char *out, size_t cap, const char *fmt, ...) {
    const size_t l = strlen(out); if (l + 1 >= cap) return;
    va_list ap; va_start(ap, fmt); vsnprintf(out + l, cap - l, fmt, ap); va_end(ap);
}

static SdsUnit *sdb_new(SdbRead *r, int cat, int sub) {
    if (r->n >= SDS_MAXU) return NULL;
    SdsUnit *u = &r->u[r->n++]; memset(u, 0, sizeof *u); u->cat = cat; u->sub = sub; u->weight = 1.0; u->elev = -1;
    return u;
}
static void sdb_rule(SdbRead *r, const char *src, const char *flux) { SdsUnit *u = sdb_new(r, SDS_RULE, 0); if (!u) return; u->must = 1; snprintf(u->src, sizeof u->src, "%s", src); snprintf(u->flux, sizeof u->flux, "%s", flux); }

/* o briefing -> unidades (regras, materiais por area, luz, objetos, estilo, pedido do usuario); r e preenchido */
static void sdb_read(const char *text, int has_lines, const char *extra, SdbRead *r) {
    memset(r, 0, sizeof *r); r->elev = -1;
    int sec = 0, ntemps = 0, temps[256], kcount[10] = {0}; char ksrc[10][400]; memset(ksrc, 0, sizeof ksrc);
    char sunsrc[300] = "";
    SdbRead *tmp = r;
    r->ban = sdb_has(text, "sem decora") || sdb_has(text, "n\xc3\xa3o adicione") || sdb_has(text, "nao adicione");
    snprintf(r->room, sizeof r->room, "%s", sdb_has(text, "recep") ? "a reception" : "an interior");
    /* regras absolutas: sempre entram */
    sdb_rule(r, "mantenha posicao camera lente enquadramento perspectiva proporcao", "Keep the exact camera position, lens, framing, perspective and proportions.");
    { char g[400]; snprintf(g, sizeof g, "Every wall, opening, ceiling, floor, piece of furniture and object keeps its position, size and shape: nothing is added, removed, moved or resized%s.", r->ban ? ", no extra decoration, plants, people, lamps, artwork or written text" : "");
      sdb_rule(r, "mantenha paredes aberturas tetos pisos moveis objetos mesma posicao nao adicione remova mova", g); }
    if (has_lines) sdb_rule(r, "desenho linear cada aresta deve permanecer", "Every edge of the line drawing stays in place.");
    for (const char *p = text; *p; ) {
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
            if (sdb_has(name, "folha") || sdb_has(desc, "planta")) r->plants_listed = 1;
            char fx[240], sd[96] = ""; double area = 0, a2 = 0; int rr, gg, bb, pa, pb;
            if (!sdb_fx_material(name, desc, fx, sizeof fx, &area)) continue;
            if (!sdb_material(name, desc, sd, sizeof sd, &a2)) sd[0] = 0;
            int found = -1; for (int i = 0; i < r->n; i++) if (r->u[i].cat == SDS_MAT && !strcmp(r->u[i].flux, fx)) found = i;
            if (found >= 0) { r->u[found].area += area; r->u[found].count++; if (!r->u[found].sd[0]) snprintf(r->u[found].sd, sizeof r->u[found].sd, "%s", sd); continue; }
            SdsUnit *u = sdb_new(r, SDS_MAT, 0); if (!u) continue;
            u->area = area; u->count = 1; snprintf(u->flux, sizeof u->flux, "%s", fx); snprintf(u->sd, sizeof u->sd, "%s", sd);
            snprintf(u->src, sizeof u->src, "%s%s", name, desc);
            if (sdb_rgb(desc, &rr, &gg, &bb)) u->sig |= SDS_SIG_RGB;
            if (sdb_pattern(desc, &pa, &pb)) u->sig |= SDS_SIG_PAT;
            if (strchr(desc, '%')) u->sig |= SDS_SIG_PCT;                                  /* o briefing disse a parte do quadro */
            if (sdb_has(name, "led") || sdb_has(name, "lamp")) u->skip = "material de lampada (luz, nao superficie)";
        } else if (sec == 2 && *t == '-') {
            static const char *const key[][2] = { { "bal", "reception counter" }, { "cadeira", "chair" }, { "poltrona", "armchair" }, { "sof", "sofa" }, { "mesa", "table" }, { "espelho", "mirror" }, { "cama", "bed" }, { "maca", "treatment bed" } };
            for (int k = 0; k < 8; k++) {
                if (!sdb_has(t, key[k][0])) continue;
                if (k == 0 && !sdb_has(t, "atendimento") && !sdb_has(t, "balc")) continue;
                int dup = 0; for (int i = 0; i < r->n; i++) if (r->u[i].cat == SDS_OBJ && !strcmp(r->u[i].flux, key[k][1])) dup = 1;
                SdsUnit *u = dup ? NULL : sdb_new(r, SDS_OBJ, 0); if (!u) continue;
                snprintf(u->flux, sizeof u->flux, "%s", key[k][1]); snprintf(u->sd, sizeof u->sd, "%s", key[k][1]); snprintf(u->src, sizeof u->src, "%s", t);
                if (r->nobj < 8) snprintf(r->objs[r->nobj++], sizeof r->objs[0], "%s", key[k][1]);
            }
        } else if (sec == 3 || sdb_has(t, "luz do dia")) {
            if (sdb_has(t, "eleva")) { const char *ev = strstr(t, "eleva"); const char *d = ev; while (*d && !isdigit((unsigned char)*d)) d++; if (*d) { char num[16]; size_t k = 0; while ((isdigit((unsigned char)*d) || *d == ',' || *d == '.') && k < 15) { num[k++] = *d == ',' ? '.' : *d; d++; } num[k] = 0; r->elev = atof(num); snprintf(sunsrc, sizeof sunsrc, "luz do dia elevacao solar sol"); } }
            if ((strstr(t, "LIGAD") || strstr(t, "ON")) && sdb_has(t, " k")) {
                const char *colon = strstr(t, "\": "); if (!colon) colon = strchr(t, ':');
                if (colon) { const char *kk = strstr(colon, " K"); if (kk) { const char *d = kk; while (d > colon && isdigit((unsigned char)d[-1])) d--; const int tv = atoi(d); if (tv >= 1500 && tv <= 8000 && ntemps < 256) temps[ntemps++] = tv; } }
                r->lights_on++;
                const int k = sdb_light_kind(t); r->kinds |= k;
                for (int i = 0; i < 10; i++) if (k & SDB_LIGHTS[i].bit) { kcount[i]++; if (strlen(ksrc[i]) + strlen(t) + 2 < sizeof ksrc[i]) { strcat(ksrc[i], t); strcat(ksrc[i], " "); } }
            }
        }
    }
    (void)tmp;
    /* luz: sol, temperatura (a mais comum das ligadas) e um tipo por unidade */
    if (ntemps) { int best = 0, bc = 0; for (int i = 0; i < ntemps; i++) { int c = 0; for (int j = 0; j < ntemps; j++) if (temps[j] == temps[i]) c++; if (c > bc) { bc = c; best = temps[i]; } } r->temp = best; }
    if (r->elev >= 0) {
        const char *day = r->elev >= 60 ? "bright daylight from a high sun" : r->elev >= 30 ? "soft daylight" : "low warm sunlight";
        SdsUnit *u = sdb_new(r, SDS_LIGHT, SDS_SUN);
        if (u) { u->must = 1; u->elev = r->elev; snprintf(u->src, sizeof u->src, "%s", sunsrc); snprintf(u->sd, sizeof u->sd, "%s", day);
                 snprintf(u->flux, sizeof u->flux, "%s (solar elevation %.1f degrees) matching the shadows of the view", day, r->elev); }
    }
    if (r->temp) {
        const char *w = r->temp <= 3200 ? "warm" : r->temp >= 4500 ? "cool white" : "neutral";
        SdsUnit *u = sdb_new(r, SDS_LIGHT, SDS_TEMP);
        if (u) { u->must = 1; u->temp = r->temp; u->sig |= SDS_SIG_K; u->weight = 1.1; snprintf(u->src, sizeof u->src, "luzes ligadas %d K cor temperatura", r->temp);
                 snprintf(u->flux, sizeof u->flux, "%s artificial light at %d K", w, r->temp); snprintf(u->sd, sizeof u->sd, "%s artificial lighting %dK", w, r->temp); }
    }
    for (int i = 0; i < 10; i++) if (kcount[i]) {
        SdsUnit *u = sdb_new(r, SDS_LIGHT, SDS_KIND); if (!u) break;
        u->kind = SDB_LIGHTS[i].bit; u->count = kcount[i]; snprintf(u->src, sizeof u->src, "%s", ksrc[i]);
        snprintf(u->flux, sizeof u->flux, "%s", SDB_LIGHTS[i].name); snprintf(u->sd, sizeof u->sd, "%s", SDB_LIGHTS[i].name);
    }
    if (r->lights_on) sdb_rule(r, "luminarias que nao estejam listadas como ligadas permanecem desligadas", "Luminaires that are not listed as on stay off.");
    /* estilo */
    for (int i = 0; i < (int)(sizeof SDB_STYLES / sizeof SDB_STYLES[0]); i++) if (sdb_has(text, SDB_STYLES[i].pt)) {
        SdsUnit *u = sdb_new(r, SDS_STYLE, 0); if (!u) break;
        u->prior = SDB_STYLES[i].prior; snprintf(u->src, sizeof u->src, "%s", SDB_STYLES[i].pt); snprintf(u->flux, sizeof u->flux, "%s", SDB_STYLES[i].flux); snprintf(u->sd, sizeof u->sd, "%s", SDB_STYLES[i].sd);
    }
    { SdsUnit *u = sdb_new(r, SDS_STYLE, 1); if (u) { u->prior = 0.1; snprintf(u->src, sizeof u->src, "high quality"); snprintf(u->sd, sizeof u->sd, "high quality"); } }
    if (extra && extra[0]) { SdsUnit *u = sdb_new(r, SDS_USER, 0); if (u) { u->must = 1; snprintf(u->src, sizeof u->src, "%.900s", extra); snprintf(u->flux, sizeof u->flux, "%.900s", extra); } }
    /* materiais: abaixo de 1% do quadro e ruido (os "~0%"); peso do Stable pela area; ordem estavel: categoria, depois maior area */
    for (int i = 0; i < r->n; i++) if (r->u[i].cat == SDS_MAT) {
        SdsUnit *u = &r->u[i]; r->nmat++;
        if (u->skip) {}
        else if ((u->sig & SDS_SIG_PCT) && u->area < 1.0) u->skip = "abaixo de 1% do quadro";
        else if (!strncmp(u->flux, "painted wall surface, colour not stated", 39)) u->skip = "tinta sem cor: nao diz nada ao modelo";
        if (!strncmp(u->flux, "surface material", 16) && !(u->sig & SDS_SIG_RGB) && !strstr(u->flux, "colour ")) u->sig |= SDS_SIG_VAGUE;
        u->weight = u->area >= 20 ? 1.2 : u->area >= 8 ? 1.1 : 1.0;
    }
    for (int i = 1; i < r->n; i++) {
        SdsUnit v = r->u[i]; int j = i - 1;
        while (j >= 0 && (r->u[j].cat > v.cat || (r->u[j].cat == v.cat && v.cat == SDS_MAT && r->u[j].area < v.area) || (r->u[j].cat == v.cat && v.cat == SDS_LIGHT && r->u[j].sub > v.sub))) { r->u[j + 1] = r->u[j]; j--; }
        r->u[j + 1] = v;
    }
}

/* ---------- FLUX: prosa ---------- */
static void sdb_render_flux(const SdsUnit *u, int n, char *out, size_t cap, void *ctx) {
    const SdbRead *r = ctx; out[0] = 0;
    sdb_app(out, cap, "Photorealistic architectural interior photograph of %s, made from the reference view (a 3D model export). ", r->room);
    for (int i = 0; i < n; i++) if (u[i].chosen && u[i].cat == SDS_RULE) sdb_app(out, cap, "%s ", u[i].flux);
    int first = 1;
    for (int i = 0; i < n; i++) if (u[i].chosen && u[i].cat == SDS_MAT) {
        if (first) sdb_app(out, cap, "Each material sits exactly on its surfaces in the view, with its real colour and pattern scale (share of the frame in %%): ");
        if (u[i].area > 0) sdb_app(out, cap, "%s%s, %.0f%%", first ? "" : "; ", u[i].flux, u[i].area); else sdb_app(out, cap, "%s%s", first ? "" : "; ", u[i].flux);
        first = 0;
    }
    if (!first) sdb_app(out, cap, ". ");
    const SdsUnit *sun = NULL, *tp = NULL; int nk = 0;
    for (int i = 0; i < n; i++) if (u[i].chosen && u[i].cat == SDS_LIGHT) { if (u[i].sub == SDS_SUN) sun = &u[i]; else if (u[i].sub == SDS_TEMP) tp = &u[i]; else nk++; }
    if (sun || tp || nk) {
        sdb_app(out, cap, "Lighting: ");
        if (sun) sdb_app(out, cap, "%s%s", sun->flux, tp || nk ? "; " : "");
        if (tp) sdb_app(out, cap, "%s", tp->flux);
        if (nk) {
            sdb_app(out, cap, "%s", tp ? " from" : "artificial light from");
            int f = 1; for (int i = 0; i < n; i++) if (u[i].chosen && u[i].cat == SDS_LIGHT && u[i].sub == SDS_KIND) { sdb_app(out, cap, "%s %s", f ? "" : ",", u[i].flux); f = 0; }
        }
        sdb_app(out, cap, ". ");
    }
    first = 1;
    for (int i = 0; i < n; i++) if (u[i].chosen && u[i].cat == SDS_OBJ) { sdb_app(out, cap, "%s%s", first ? "Main objects: " : ", ", u[i].flux); first = 0; }
    if (!first) sdb_app(out, cap, ". ");
    first = 1;
    for (int i = 0; i < n; i++) if (u[i].chosen && u[i].cat == SDS_STYLE && u[i].flux[0]) { sdb_app(out, cap, "%s%s", first ? "Style: " : ", ", u[i].flux); first = 0; }
    if (!first) sdb_app(out, cap, ". ");
    for (int i = 0; i < n; i++) if (u[i].chosen && u[i].cat == SDS_USER) sdb_app(out, cap, "Additional request from the user: %s. ", u[i].flux);
    sdb_app(out, cap, "16:9, no borders, no watermark.");
}

/* ---------- Stable: tags em ingles, (termo:peso) nas prioridades ---------- */
static void sdb_tag(char *out, size_t cap, const SdsUnit *u) { if (u->weight > 1.0) sdb_app(out, cap, ", (%s:%.1f)", u->sd, u->weight); else sdb_app(out, cap, ", %s", u->sd); }

static void sdb_render_sd(const SdsUnit *u, int n, char *out, size_t cap, void *ctx) {
    const SdbRead *r = ctx; out[0] = 0;
    sdb_app(out, cap, "photorealistic architectural interior photograph of %s", r->room);
    static const int order[] = { SDS_MAT, SDS_LIGHT, SDS_OBJ, SDS_STYLE };
    for (int c = 0; c < 4; c++) {
        int nk = 0;
        for (int i = 0; i < n; i++) {
            if (!u[i].chosen || u[i].cat != order[c] || !u[i].sd[0]) continue;
            int dup = 0; for (int j = 0; j < i; j++) if (u[j].chosen && u[j].cat == u[i].cat && !strcmp(u[j].sd, u[i].sd)) dup = 1;
            if (dup) continue;
            if (u[i].cat == SDS_LIGHT && u[i].sub == SDS_KIND) { nk++; continue; }
            sdb_tag(out, cap, &u[i]);
        }
        if (nk) { int f = 1; for (int i = 0; i < n; i++) if (u[i].chosen && u[i].cat == SDS_LIGHT && u[i].sub == SDS_KIND) { sdb_app(out, cap, "%s%s", f ? ", lit by " : ", ", u[i].sd); f = 0; } }
    }
}

/* contadores de tokens */
static int sdb_count_sd(const char *text, void *ctx) { return sdb_tokens(((const SdbRead *)ctx)->clip, text); }
static int sdb_count_flux(const char *text, void *ctx) {
    (void)ctx;
    const int t = fxt_count(text);
    if (t >= 0) return t;
    static int warned; if (!warned) { warned = 1; fprintf(stderr, "[brief] sem o tokenizador do FLUX (qwen3-4b-text.gguf): contagem estimada por caracteres, com folga\n"); }
    return (int)(strlen(text) * 0.30) + 16;
}
#define SDB_FLUX_BUDGET 504                    /* 512 posicoes de texto menos 8 de margem (o modelo de conversa gasta cerca de 12, ja contados) */

static void sdb_neg_add(const SdClip *clip, char *neg, size_t cap, const char *item, int budget, int *cut) {
    char trial[600]; snprintf(trial, sizeof trial, "%s%s%s", neg, neg[0] ? ", " : "", item);
    if (sdb_tokens(clip, trial) > budget) { (*cut)++; if (sdb_explain_on) fprintf(stderr, "[explain-brief] negativo CORTE %s\n", item); return; }
    snprintf(neg, cap, "%s", trial);
}

/* o briefing -> prompt do Stable (tags com pesos) e negativo; devolve quantos materiais entraram */
static int sdb_build(const char *text, const SdClip *clip, int chunks, SdbOut *o) {
    SdbRead *r = xalloc(sizeof *r, 1); sdb_read(text, 0, NULL, r); r->clip = clip;
    const int budget = chunks * 75 - 4;
    for (int i = 0; i < r->n; i++) if (!r->u[i].must && !r->u[i].sd[0] && !r->u[i].skip) r->u[i].skip = "sem texto para o Stable (as regras vao no negativo)";
    const int used = sds_select(r->u, r->n, budget, sdb_render_sd, sdb_count_sd, r);
    sdb_render_sd(r->u, r->n, o->prompt, sizeof o->prompt, r);
    if (sdb_explain_on) sds_explain(r->u, r->n, "stable", used, budget, stderr);
    o->elev = r->elev; o->temp = r->temp; o->lights_on = r->lights_on; o->kinds = r->kinds; o->nobj = r->nobj; memcpy(o->objs, r->objs, sizeof o->objs);
    o->chunks_used = (sdb_tokens(clip, o->prompt) + 74) / 75;
    /* negativo (separado, mesmo limite de blocos): o que o briefing proibe primeiro */
    static const char *const base[] = { "text", "logo", "watermark", "people" }, *const rest[] = { "cartoon", "sketch", "drawing", "outline", "lines", "oversaturated", "lowres", "blurry", "deformed" };
    char neg[600] = ""; int cut = 0;
    for (int i = 0; i < 4; i++) sdb_neg_add(clip, neg, sizeof neg, base[i], budget, &cut);
    if (r->ban) { sdb_neg_add(clip, neg, sizeof neg, "extra decoration", budget, &cut); sdb_neg_add(clip, neg, sizeof neg, "extra objects", budget, &cut); }
    if (!r->plants_listed) sdb_neg_add(clip, neg, sizeof neg, "plants", budget, &cut);
    for (int i = 0; i < 9; i++) sdb_neg_add(clip, neg, sizeof neg, rest[i], budget, &cut);
    snprintf(o->negative, sizeof o->negative, "%s", neg);
    int nm = 0; for (int i = 0; i < r->n; i++) if (r->u[i].cat == SDS_MAT && r->u[i].chosen) nm++;
    free(r);
    return nm;
}

/* ---------- prompt em prosa para o FLUX (render generate --engine flux --brief; render spec --brief) ----------
 * O FLUX le prosa e nao tem limite de 75 tokens nem prompt negativo. Aqui o briefing vira frases por regra, sem resumo a mao:
 *   - cada material com a cor (RGB -> nome), a escala do padrao em cm e a parte do quadro; iguais somam a area; da maior para a menor;
 *   - o TIPO do material vem so do que o briefing escreve depois do nome. "Arenito" do tipo "material" fica "surface material \"Arenito\"" (nunca
 *     pedra); "AK_PISO CONCRETO" e piso de concreto, com o tamanho de padrao dito; nome desconhecido fica como superficie neutra;
 *   - materiais abaixo de 1% do quadro (os "~0%") nao entram; luzes, objetos, sol e estilo vem das mesmas leituras do prompt do Stable;
 *   - as regras de fidelidade (camera, geometria, nada acrescentado) entram sempre. */

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
    const int has_rgb = sdb_rgb(desc, &r, &g, &b), has_pat = sdb_pattern(desc, &pa, &pb);
    *area = sdb_pct(desc);
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

/* o briefing inteiro -> prosa em ingles para o FLUX; extra = pedido adicional do usuario (--nl), opcional; has_lines = ha desenho linear (--init2).
 * all = 1 escolhe tudo o que nao e ruido (sem orcamento; so para o teste); senao o selecionador (sdsel.c) enche as 512 posicoes de texto do FLUX. */
static void sdb_flux_prose_x(const char *text, int has_lines, const char *extra, char *out, size_t cap, int all) {
    SdbRead *r = xalloc(sizeof *r, 1); sdb_read(text, has_lines, extra, r);
    int used;
    if (all) { for (int i = 0; i < r->n; i++) r->u[i].chosen = !r->u[i].skip && (r->u[i].flux[0] != 0); sdb_render_flux(r->u, r->n, out, cap, r); used = sdb_count_flux(out, r); }
    else {
        for (int i = 0; i < r->n; i++) if (!r->u[i].flux[0] && !r->u[i].skip) r->u[i].skip = "sem texto para o FLUX";
        used = sds_select(r->u, r->n, SDB_FLUX_BUDGET, sdb_render_flux, sdb_count_flux, r);
        sdb_render_flux(r->u, r->n, out, cap, r);
        int cutm = 0, allm = 0; for (int i = 0; i < r->n; i++) if (r->u[i].cat == SDS_MAT && !r->u[i].skip && r->u[i].flux[0]) { allm++; if (!r->u[i].chosen) cutm++; }
        if (cutm) fprintf(stderr, "[brief] %d de %d material(is) de menor sinal ficaram de fora do prompt do FLUX (limite de 512 posicoes de texto)\n", cutm, allm);
        if (sdb_explain_on) sds_explain(r->u, r->n, "flux", used, SDB_FLUX_BUDGET, stderr);
    }
    free(r);
}
static void sdb_flux_prose(const char *text, int has_lines, const char *extra, char *out, size_t cap) { sdb_flux_prose_x(text, has_lines, extra, out, cap, 0); }

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

/* briefing sintetico grande: o do teste mais materiais e luzes de pouco sinal; o que tem muito sinal tem de ficar e o resto, de fora, sem passar do orcamento */
static int sdb_selftest_select(void) {
    int bad = 0;
    static char big[16000]; snprintf(big, sizeof big, "%s", SDB_TEST_BRIEF);
    char *mp = strstr(big, "- \"Iron_Blurry_5cm\"");
    static char tail[8000]; snprintf(tail, sizeof tail, "%s", mp ? mp : "");
    if (mp) *mp = 0;
    for (int i = 0; i < 24; i++) { char l[200]; snprintf(l, sizeof l, "- \"MAT_BAIXO_%02d\": material, repeti\xc3\xa7\xc3\xa3o do padr\xc3\xa3o %dx%d cm (aproximadamente 1%% da moldura)\n", i, 10 + i, 20 + i); strncat(big, l, sizeof big - strlen(big) - 1); }
    strncat(big, "- \"Metal Dourado\": metal \xe2\x80\x94 RGB 190,150,60 (~6% do quadro)\n", sizeof big - strlen(big) - 1);
    strncat(big, tail, sizeof big - strlen(big) - 1);
    printf("briefing sintetico grande (%zu caracteres, 24 materiais de ~1%% e um metal dourado de 6%%)\n", strlen(big));
    static char fit[6000]; sdb_flux_prose(big, 1, "add a vase of tulips", fit, sizeof fit);
    const int tk = sdb_count_flux(fit, NULL);
    bad += sdb_tck("FLUX: cabe no orcamento mesmo com 40 materiais", tk <= SDB_FLUX_BUDGET);
    bad += sdb_tck("FLUX: tintas, concreto e o metal dourado (RGB) ficam; os de 1% sem sinal saem", strstr(fit, "burgundy painted wall (RGB 119,79,81)") && strstr(fit, "concrete floor tiles") && strstr(fit, "metal") && !strstr(fit, "MAT_BAIXO_2"));
    bad += sdb_tck("FLUX: regras, sol, 3000 K e o pedido do usuario (--nl) entram sempre", strstr(fit, "Keep the exact camera") && strstr(fit, "solar elevation 66.5") && strstr(fit, "3000 K") && strstr(fit, "Additional request from the user: add a vase of tulips"));
    bad += sdb_tck("FLUX: ordem regras > materiais > luz > objetos > estilo", strstr(fit, "Style:") && strstr(fit, "Keep the exact") < strstr(fit, "Each material") && strstr(fit, "Each material") < strstr(fit, "Lighting:") && strstr(fit, "Lighting:") < strstr(fit, "Main objects:") && strstr(fit, "Main objects:") < strstr(fit, "Style:"));
    static SdbOut o; sdb_build(big, NULL, 2, &o);
    const int ts = sdb_tokens(NULL, o.prompt);
    printf("  Stable com 2 blocos: %d tokens estimados: %s\n", ts, o.prompt);
    bad += sdb_tck("Stable: cabe em 2 blocos de 75 (estimativa)", ts <= 146);
    bad += sdb_tck("Stable: tintas com peso (x:1.2) e cor nomeada; 3000K; sol", strstr(o.prompt, "(slate blue-grey painted wall:1.2)") && strstr(o.prompt, "3000K") && strstr(o.prompt, "bright daylight"));
    bad += sdb_tck("Stable: negativo separado, sem plantas listadas no briefing (so FOLHA, 0%) nao some", strstr(o.negative, "text") && strstr(o.negative, "extra decoration"));
    return bad;
}

static int sdb_selftest(const char *file) {
    int bad = 0; char *real = file ? sdb_read_text(file) : NULL;
    printf("katana render brief selftest (prompt do FLUX a partir do briefing)\n");
    for (int pass = 0; pass < (real ? 2 : 1); pass++) {
        const char *text = pass ? real : SDB_TEST_BRIEF;
        static char pr[6000], fit[6000]; sdb_flux_prose_x(text, 1, NULL, pr, sizeof pr, 1); sdb_flux_prose(text, 1, NULL, fit, sizeof fit);
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
        bad += sdb_tck("prosa final cabe nas 512 posicoes do texto do FLUX (orcamento do selecionador)", sdb_count_flux(fit, NULL) <= SDB_FLUX_BUDGET);
        bad += sdb_tck("prosa final tem as regras, o sol e os 3000 K", strstr(fit, "Keep the exact camera") && strstr(fit, "nothing is added") && strstr(fit, "solar elevation 66.5") && strstr(fit, "3000 K"));
        bad += sdb_tck("as tres tintas com RGB sobrevivem ao corte", strstr(fit, "slate blue-grey painted wall (RGB 99,112,123)") && strstr(fit, "burgundy painted wall (RGB 119,79,81)") && strstr(fit, "off-white painted wall (RGB 228,222,204)"));
    }
    bad += sdb_selftest_select();
    printf("RESULTADO: %s\n", bad ? "FALHOU" : "todos passaram");
    return bad ? 1 : 0;
}

static int sdb_main(int argc, char **argv) {
    if (argc >= 1 && !strcmp(argv[0], "selftest")) return sdb_selftest(argc > 1 ? argv[1] : NULL);
    if (argc < 1) { fprintf(stderr, "uso: katana render brief <briefing.md> --image1 vista.png --out saida.png [--image2 linhas.png] [--image3 amostras.png] [--show] [--explain-brief]\n"
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
        else if (!strcmp(a, "--explain-brief")) sdb_explain_on = 1;
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
