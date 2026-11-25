/* ============================ render: o briefing e o prompt (sem reescrita, sem corte) ===============================
 * O motor NAO escreve o briefing: quem manda (o modelo de conversa pela ferramenta Render, ou o usuario) escreve o prompt final no idioma e no
 * estilo do motor, e ele passa com as palavras do autor, sem prefixo, gabarito, traducao nem acrescimo. O motor so CONTA os tokens (FLUX: 504
 * posicoes de texto, tokenizador real; Stable: --chunks blocos de 75 tokens do CLIP, chunks*75-4). Cabendo, o texto passa inteiro e intacto;
 * passando do limite NADA e cortado: o motor recusa com um erro que diz quantos tokens tem, o limite e o que fazer, e quem mandou reescreve mais curto.
 *
 *   katana render brief <briefing.md> --image1 vista.png --out saida.png [--image2 linhas.png] [--show] [opcoes]    (Stable, imagem a imagem)
 *   katana render brief selftest */

static int fxt_count(const char *prompt);                  /* render/fxtext.c: tokens do prompt como o FLUX os conta (-1 = sem o tokenizador) */

/* quantos ids do CLIP tem o texto (sem inicio e fim; um bloco de 75 ids por 75 tokens); sem modelo, estima por palavras */
static int sdb_tokens(const SdClip *c, const char *text) {
    if (!c) { int w = 0, in = 0, pw = 0; for (const char *p = text; *p; p++) { if (*p == '(') pw++; if (isalnum((unsigned char)*p)) { if (!in) { w++; in = 1; } } else in = 0; } return w + w / 2 + 4 * pw; }
    static int tmp[4096]; static float wt[4096];
    return sdc_ids_w(c, text, tmp, wt, 4096);
}

static int sdb_count_sd(const char *text, void *ctx) { return sdb_tokens((const SdClip *)ctx, text); }
static int sdb_count_flux(const char *text, void *ctx) {
    (void)ctx;
    const int t = fxt_count(text);
    if (t >= 0) return t;
    static int warned; if (!warned) { warned = 1; fprintf(stderr, "[brief] sem o tokenizador do FLUX (qwen3-4b-text.gguf): contagem estimada por caracteres, com folga\n"); }
    return (int)(strlen(text) * 0.30) + 16;
}
static int sdb_count_est(const char *t, void *ctx) { (void)ctx; return sdb_tokens(NULL, t); }
#define SDB_FLUX_BUDGET 504                    /* 512 posicoes de texto menos 8 de margem (o modelo de conversa gasta cerca de 12, ja contados) */
#define SDB_MAX_CHUNKS 16                      /* o maximo de blocos de 75 tokens que o Stable aceita */
#define SDB_NEG_DEFAULT "blurry, low quality, deformed"   /* negativo do Stable quando o chamador nao passa --negative: generico, sem dominio */

/* a mensagem de recusa (em ingles, acionavel) quando used passa de budget; devolve 1 se passou. chunks < 0: motor sem --chunks (FLUX) */
static int sdb_limit_msg(const char *engine, int used, int budget, int chunks, char *o, size_t cap) {
    if (used <= budget) return 0;
    const int words = budget * 7 / 100 * 10;                                     /* ~0.7 palavra por token, em dezenas */
    if (chunks < 0) snprintf(o, cap, "error: the brief has %d tokens, the limit for %s is %d (about %d words): shorten it and send it again", used, engine, budget, words);
    else if (chunks < SDB_MAX_CHUNKS) snprintf(o, cap, "error: the brief has %d tokens, the limit for %s is %d with --chunks %d (about %d words): shorten it, or raise --chunks (up to %d) and send it again", used, engine, budget, chunks, words, SDB_MAX_CHUNKS);
    else snprintf(o, cap, "error: the brief has %d tokens, the limit for %s is %d, the most --chunks allows (about %d words): shorten it and send it again", used, engine, budget, words);
    return 1;
}

/* o briefing (+ pedido extra, ultima linha, como veio) -> o prompt do motor: o proprio texto, intacto, ou NULL (recusa, com o erro em err) se passa do limite.
 * rep (opcional) recebe "[brief] <motor> tokens N/limite" */
static char *sdb_make(const char *engine, const char *text, const char *extra, int budget, int chunks, int (*count)(const char *, void *), void *ctx, FILE *rep, FILE *err) {
    const size_t tl = strlen(text), el = extra && extra[0] ? strlen(extra) : 0;
    char *tx = xalloc(tl + el + 2, 1); memcpy(tx, text, tl);
    if (el) { tx[tl] = '\n'; memcpy(tx + tl + 1, extra, el); }
    const int used = count(tx, ctx); char msg[400];
    if (sdb_limit_msg(engine, used, budget, chunks, msg, sizeof msg)) { if (err) fprintf(err, "%s\n", msg); free(tx); return NULL; }
    if (rep) fprintf(rep, "[brief] %s tokens %d/%d\n", engine, used, budget);
    return tx;
}
static char *sdb_flux_prompt(const char *text, const char *extra, FILE *rep) { return sdb_make("flux", text, extra, SDB_FLUX_BUDGET, -1, sdb_count_flux, NULL, rep, stderr); }
static char *sdb_stable_prompt(const char *text, const char *extra, const SdClip *clip, int chunks, FILE *rep) {
    return sdb_make("stable", text, extra, chunks * 75 - 4, chunks, sdb_count_sd, (void *)clip, rep, stderr);
}

/* arquivo -> texto (malloc) ou NULL */
static char *sdb_read_text(const char *path) {
    int64_t sz = 0; const uint8_t *b = map_file(path, &sz);
    if (!b) { fprintf(stderr, "render: nao consegui abrir %s\n", path); return NULL; }
    char *t = xalloc((size_t)sz + 1, 1); memcpy(t, b, (size_t)sz);
    return t;
}

/* generate --engine stable --brief: o texto vira o prompt (P->prompt); o negativo e o do chamador ou o generico curto. 0 = recusado (passa do limite) ou erro */
static int sdb_fill(SdpParams *P, const char *path, const char *extra) {
    char *text = sdb_read_text(path); if (!text) return 0;
    SdgFile base; SdClip clip; const SdClip *cp = NULL; char mp[1100];
    snprintf(mp, sizeof mp, "%s/sd15-realistic-vision.gguf", P->models);
    if (sdg_open(&base, mp) && sdc_load(&clip, &base)) cp = &clip;
    if (P->max_chunks < 1) P->max_chunks = 1;
    if (P->max_chunks > SDB_MAX_CHUNKS) P->max_chunks = SDB_MAX_CHUNKS;
    P->prompt = sdb_stable_prompt(text, extra, cp, P->max_chunks, stderr);
    free(text);
    if (!P->prompt) return 0;
    if (!P->negative || !P->negative[0]) P->negative = SDB_NEG_DEFAULT;
    fprintf(stderr, "[brief] prompt do Stable: %s\n[brief] negativo: %s\n", P->prompt, P->negative);
    return 1;
}

/* ---------- teste ---------- */
static const char SDB_T_MAGA[] =
    "Pixel art portrait of a young sorceress in the style of a classic 16-bit adventure game, hero shot, clean readable sprite.\n"
    "She wears a deep green hooded robe with gold trim, holds a wooden staff with a glowing blue crystal, long silver braid.\n"
    "Warm torch light from the left, soft rim light, simple dark stone dungeon background, limited palette of 16 colors, crisp pixels, no anti-aliasing.\n";

static int sdb_tck(const char *what, int ok) { printf("  %-76s %s\n", what, ok ? "OK" : "FALHOU"); return ok ? 0 : 1; }

static int sdb_selftest(void) {
    int bad = 0; char msg[400];
    /* contagem e limite (contador estimado, deterministico) */
    const int tk = sdb_count_est(SDB_T_MAGA, NULL);
    printf("contagem e limite (contador estimado: %d tokens no briefing curto)\n", tk);
    bad += sdb_tck("contagem: texto vazio 0 e mais palavras, mais tokens", sdb_count_est("", NULL) == 0 && sdb_count_est("a b c d", NULL) > sdb_count_est("a b", NULL) && tk > 40);
    /* (i) curto: intacto, byte a byte; com o pedido extra, so o texto + nova linha + o pedido */
    char *o = sdb_make("stable", SDB_T_MAGA, NULL, 296, 4, sdb_count_est, NULL, NULL, NULL);
    bad += sdb_tck("curto: passa intacto (byte a byte), nada acrescentado", o && !strcmp(o, SDB_T_MAGA)); free(o);
    o = sdb_make("stable", "a red apple", "on a table", 296, 4, sdb_count_est, NULL, NULL, NULL);
    bad += sdb_tck("curto + pedido extra: texto, nova linha, pedido; nada mais", o && !strcmp(o, "a red apple\non a table")); free(o);
    /* (ii) o limite: no limite passa, um token acima e recusado, sem cortar nada */
    static char big[16000]; big[0] = 0;
    int nw = 0, last_ok = 0;
    for (; nw < 800; nw++) {                                                    /* palavras "w" ate a contagem passar de 296 */
        strncat(big, "w ", sizeof big - strlen(big) - 1);
        if (sdb_count_est(big, NULL) <= 296) last_ok = nw + 1; else break;
    }
    o = sdb_make("stable", big, NULL, 296, 4, sdb_count_est, NULL, NULL, NULL);
    bad += sdb_tck("acima do limite: recusado (NULL), nada cortado nem devolvido", nw < 800 && o == NULL);
    free(o);
    big[last_ok * 2] = 0;
    o = sdb_make("stable", big, NULL, 296, 4, sdb_count_est, NULL, NULL, NULL);
    bad += sdb_tck("no limite: passa intacto", o && sdb_count_est(big, NULL) <= 296 && !strcmp(o, big)); free(o);
    /* a recusa traz o numero, o limite e o que fazer (ingles) */
    bad += sdb_tck("mensagem FLUX: tokens, limite 504, ~350 palavras, encurtar", sdb_limit_msg("flux", 812, 504, -1, msg, sizeof msg) && !strcmp(msg, "error: the brief has 812 tokens, the limit for flux is 504 (about 350 words): shorten it and send it again"));
    bad += sdb_tck("mensagem Stable: limite do --chunks e pode subir ate 16", sdb_limit_msg("stable", 400, 296, 4, msg, sizeof msg) && strstr(msg, "limit for stable is 296 with --chunks 4") && strstr(msg, "raise --chunks (up to 16)"));
    bad += sdb_tck("mensagem Stable com --chunks 16: so encurtar", sdb_limit_msg("stable", 1300, 1196, 16, msg, sizeof msg) && !strstr(msg, "raise") && strstr(msg, "shorten it"));
    bad += sdb_tck("dentro do limite: sem mensagem", !sdb_limit_msg("flux", 504, 504, -1, msg, sizeof msg));
    /* contador real do FLUX (se o tokenizador existir): o curto passa intacto e o longo e recusado */
    if (fxt_count("a") >= 0) {
        o = sdb_flux_prompt(SDB_T_MAGA, NULL, NULL);
        bad += sdb_tck("FLUX (tokenizador real): briefing curto passa intacto", o && !strcmp(o, SDB_T_MAGA)); free(o);
        static char longp[20000]; longp[0] = 0;
        for (int i = 0; i < 60; i++) { char l[200]; snprintf(l, sizeof l, "Detail %d: a small ornament, number %d, somewhere near the border of the whole picture, dull grey.\n", i, i * 13); strncat(longp, l, sizeof longp - strlen(longp) - 1); }
        const int n = fxt_count(longp);
        char *r = sdb_make("flux", longp, NULL, SDB_FLUX_BUDGET, -1, sdb_count_flux, NULL, NULL, NULL);
        bad += sdb_tck("FLUX (tokenizador real): briefing longo e recusado", n > SDB_FLUX_BUDGET && r == NULL); free(r);
    } else printf("  (sem o tokenizador do FLUX nesta maquina: verificacoes do contador real puladas)\n");
    printf("RESULTADO: %s\n", bad ? "FALHOU" : "todos passaram");
    return bad ? 1 : 0;
}

static int sdb_main(int argc, char **argv) {
    if (argc >= 1 && !strcmp(argv[0], "selftest")) return sdb_selftest();
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
    if (P.max_chunks > SDB_MAX_CHUNKS) P.max_chunks = SDB_MAX_CHUNKS;
    int64_t sz = 0; const uint8_t *b = map_file(brief, &sz);
    if (!b) { fprintf(stderr, "render brief: nao consegui abrir %s\n", brief); return 1; }
    char *text = xalloc((size_t)sz + 1, 1); memcpy(text, b, (size_t)sz);
    SdgFile base; SdClip clip; const SdClip *cp = NULL; char path[1100];
    snprintf(path, sizeof path, "%s/sd15-realistic-vision.gguf", P.models);
    if (sdg_open(&base, path) && sdc_load(&clip, &base)) cp = &clip;
    char *prompt = sdb_stable_prompt(text, NULL, cp, P.max_chunks, stderr); const char *negative = SDB_NEG_DEFAULT;
    if (!prompt) return 1;                                           /* passou do limite: recusado, nada cortado */
    fprintf(stderr, "[brief] prompt (o briefing, sem reescrita): %s\n[brief] negativo: %s\n", prompt, negative);
    if (img3) fprintf(stderr, "[brief] --image3 (amostras) nao e usada: o Stable Diffusion 1.5 nao toma uma textura como referencia\n");
    if (show) return 0;
    P.prompt = prompt; P.negative = negative; P.out = out; P.init = img1; P.control = img2 ? img2 : img1; P.control_lines = img2 != NULL;
    if (!P.w || !P.h) {                                              /* proporcao da vista, lado maior 576 */
        int iw, ih;
        if (sdp_peek_size(img1, &iw, &ih)) { const double s = longside / (iw > ih ? iw : ih); P.w = sdp_round64(iw * s); P.h = sdp_round64(ih * s); }
    }
    char tp[1200]; snprintf(tp, sizeof tp, "%s.txt", out);
    FILE *tf = fopen(tp, "w");
    if (tf) { fprintf(tf, "prompt: %s\nnegative: %s\nsize: %dx%d  steps: %d  guidance: %.1f  strength: %.2f  control_scale: %.2f  seed: %llu  chunks: %d  control: %s\nimage1: %s\nimage2: %s\n", prompt, negative, P.w, P.h, P.steps, P.guidance, P.strength, P.control_scale, (unsigned long long)P.seed, P.max_chunks, img2 ? "line drawing (image2)" : "canny of image1", img1, img2 ? img2 : "(none)"); fclose(tf); }
    return sdp_generate(&P);
}
