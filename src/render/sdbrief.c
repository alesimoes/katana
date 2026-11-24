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
    const int paint = sdb_has(desc, "parede pintada") || sdb_has(name, "tinta");
    if (sdb_has(name, "arenito") || sdb_has(desc, "arenito")) noun = "sandstone";
    else if (sdb_has(name, "travertino")) noun = "travertine";
    else if (sdb_has(desc, "rmore") || sdb_has(name, "marmore")) noun = "marble";
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
    else if (sdb_has(desc, "planta") || sdb_has(name, "folha")) noun = "foliage";
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

typedef struct { char prompt[2400], negative[600]; double elev; int temp, lights_on, chunks_used; } SdbOut;

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
    o->elev = -1; o->temp = 0; o->lights_on = 0;
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
    for (int i = 1; i < np; i++) { SdbPhrase v = ph[i]; int j = i - 1; while (j >= 0 && ph[j].area < v.area) { ph[j + 1] = ph[j]; j--; } ph[j + 1] = v; }      /* maior area primeiro */
    char pr[2400]; snprintf(pr, sizeof pr, "photorealistic architectural interior photograph of a reception");
    for (int i = 0; i < nobj; i++) sdb_add(clip, pr, sizeof pr, objs[i], budget);
    for (int i = 0; i < np; i++) if (!sdb_add(clip, pr, sizeof pr, ph[i].phrase, budget)) break;
    const char *day = o->elev >= 60 ? "bright daylight from a high sun" : o->elev >= 30 ? "soft daylight" : o->elev >= 0 ? "low warm sunlight" : NULL;
    if (day) sdb_add(clip, pr, sizeof pr, day, budget);
    if (o->temp) { char w[80]; snprintf(w, sizeof w, "%s artificial lighting %dK", o->temp <= 3200 ? "warm" : o->temp >= 4500 ? "cool white" : "neutral", o->temp); sdb_add(clip, pr, sizeof pr, w, budget); }
    if (kinds) {
        static const struct { int bit; const char *name; } L[] = { { SL_SPOT, "recessed spotlights" }, { SL_LINEAR, "linear LED strip" }, { SL_ROUND, "round ceiling lamp" }, { SL_PENDANT, "organic pendant lamp" },
            { SL_MIRROR, "mirror light" }, { SL_COUNTER, "counter spotlights" }, { SL_WALL, "wall washer lights" }, { SL_ARCH, "arch accent lighting" }, { SL_SPHERE, "sphere lamp" }, { SL_SUPPORT, "support light" } };
        char lp[400] = "lit by"; int first = 1;
        for (int i = 0; i < 10; i++) if (kinds & L[i].bit) { strncat(lp, first ? " " : ", ", sizeof lp - strlen(lp) - 1); strncat(lp, L[i].name, sizeof lp - strlen(lp) - 1); first = 0; }
        sdb_add(clip, pr, sizeof pr, lp, budget);
    }
    /* estilo: cada pedido do texto vira uma frase fixa */
    static const struct { const char *pt; const char *en; } S[] = {
        { "natural e realista", "natural realistic interior photography" }, { "full-frame", "shot with a full-frame camera" }, { "sem apar", "not a computer graphics look" },
        { "atenua", "physically plausible soft light with real falloff" }, { "contato", "deep contact shadows under the furniture" }, { "gradientes", "soft light gradients on the walls" },
        { "texturas", "visible material textures, wood grain, fabric weave, stone veining" }, { "quentes", "warm artificial lights" }, { "nunca supersaturadas", "faithful natural colors, never oversaturated" },
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

static int sdb_main(int argc, char **argv) {
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
