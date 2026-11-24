/* ============================ render: o briefing e o prompt (sem reescrita) ===============================
 * O motor NAO escreve o briefing: quem manda (o modelo de conversa pela ferramenta Render, ou o usuario) escreve o prompt final no idioma e no
 * estilo do motor, e ele passa com as palavras do autor, sem prefixo, gabarito, traducao nem acrescimo. Cabendo no orcamento (FLUX: 504 posicoes
 * de texto, tokenizador real; Stable: --chunks blocos de 75 tokens do CLIP) passa inteiro e intacto; excedendo, sdsel.c escolhe por relevancia
 * as unidades (linhas, frases, trechos) que ficam, na ordem original. O resultado diz "tokens N/orcamento" e o que foi cortado, para o autor reenviar mais curto.
 *
 *   katana render brief <briefing.md> --image1 vista.png --out saida.png [--image2 linhas.png] [--show] [opcoes]    (Stable, imagem a imagem)
 *   katana render brief selftest [briefing.md] */

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
#define SDB_FLUX_BUDGET 504                    /* 512 posicoes de texto menos 8 de margem (o modelo de conversa gasta cerca de 12, ja contados) */
#define SDB_NEG_DEFAULT "blurry, low quality, deformed"   /* negativo do Stable quando o chamador nao passa --negative: generico, sem dominio */


/* o briefing (+ pedido extra) -> o prompt do motor; rep diz tokens/orcamento e o que saiu */
static char *sdb_flux_prompt(const char *text, const char *extra, SdsRep *rep) { return sds_prompt(text, extra, SDB_FLUX_BUDGET, sdb_count_flux, NULL, rep, sdb_explain_on); }
static char *sdb_stable_prompt(const char *text, const char *extra, const SdClip *clip, int chunks, SdsRep *rep) {
    return sds_prompt(text, extra, chunks * 75 - 4, sdb_count_sd, (void *)clip, rep, sdb_explain_on);
}

/* arquivo -> texto (malloc) ou NULL */
static char *sdb_read_text(const char *path) {
    int64_t sz = 0; const uint8_t *b = map_file(path, &sz);
    if (!b) { fprintf(stderr, "render: nao consegui abrir %s\n", path); return NULL; }
    char *t = xalloc((size_t)sz + 1, 1); memcpy(t, b, (size_t)sz);
    return t;
}

/* generate --engine stable --brief: o texto vira o prompt (P->prompt); o negativo e o do chamador ou o generico curto */
static int sdb_fill(SdpParams *P, const char *path, const char *extra) {
    char *text = sdb_read_text(path); if (!text) return 0;
    SdgFile base; SdClip clip; const SdClip *cp = NULL; char mp[1100];
    snprintf(mp, sizeof mp, "%s/sd15-realistic-vision.gguf", P->models);
    if (sdg_open(&base, mp) && sdc_load(&clip, &base)) cp = &clip;
    SdsRep rep; P->prompt = sdb_stable_prompt(text, extra, cp, P->max_chunks, &rep);
    if (!P->negative || !P->negative[0]) P->negative = SDB_NEG_DEFAULT;
    fprintf(stderr, "[brief] prompt do Stable: %s\n[brief] negativo: %s\n", P->prompt, P->negative);
    sds_report(&rep, "stable", stderr);
    return 1;
}

/* ---------- teste ---------- */
static const char SDB_T_MAGA[] =
    "Pixel art portrait of a young sorceress in the style of a classic 16-bit adventure game, hero shot, clean readable sprite.\n"
    "She wears a deep green hooded robe (RGB 30,110,60) with gold trim, holds a wooden staff with a glowing blue crystal, long silver braid.\n"
    "Warm torch light from the left, soft rim light, simple dark stone dungeon background, limited palette of 16 colors, crisp pixels, no anti-aliasing.\n";

static int sdb_tck(const char *what, int ok) { printf("  %-76s %s\n", what, ok ? "OK" : "FALHOU"); return ok ? 0 : 1; }
static int sdb_count_est(const char *t, void *ctx) { (void)ctx; return sdb_tokens(NULL, t); }

/* as palavras de out aparecem em in, na mesma ordem (subsequencia): nada acrescentado, ordem original mantida */
static int sdb_subseq(const char *in, const char *out) {
    static char A[6000][32], B[6000][32];
    const int na = sds_words(in, A, 6000), nb = sds_words(out, B, 6000);
    int i = 0;
    for (int j = 0; j < nb; j++) { while (i < na && strcmp(A[i], B[j])) i++; if (i >= na) return 0; i++; }
    return 1;
}
/* termos do gabarito antigo: nenhum aparece na saida se nao estavam na entrada */
static int sdb_no_old_template(const char *in, const char *out) {
    static const char *const T[] = { "architectural", "interior photograph", "3D model export", "no watermark", "reference view", "Each material", "Main objects", "16:9", NULL };
    for (int i = 0; T[i]; i++) if (strstr(out, T[i]) && !strstr(in, T[i])) return 0;
    return 1;
}
static int sdb_has_in_order(const char *s, const char *a, const char *b) { const char *p = strstr(s, a), *q = strstr(s, b); return p && q && p < q; }

static int sdb_selftest_unit(const char *real_path) {
    int bad = 0; SdsRep rep;
    printf("selecao generica (contador estimado, deterministico)\n");
    /* (i) texto curto: intacto, byte a byte; com o pedido extra, so o texto + nova linha + o pedido */
    char *o = sds_prompt(SDB_T_MAGA, NULL, 296, sdb_count_est, NULL, &rep, 0);
    bad += sdb_tck("curto: passa inteiro e intacto (byte a byte)", rep.whole && !strcmp(o, SDB_T_MAGA) && rep.ncut == 0);
    free(o);
    o = sds_prompt("a red apple", "on a table", 296, sdb_count_est, NULL, &rep, 0);
    bad += sdb_tck("curto + pedido extra: texto, nova linha, pedido; nada mais", !strcmp(o, "a red apple\non a table"));
    free(o);
    /* (ii) longo: maga + 40 itens */
    static char big[16000]; snprintf(big, sizeof big, "%s\nREGRAS ABSOLUTAS:\n- never add text or logos\n- keep the face unchanged\n\nDETALHES:\n", SDB_T_MAGA);
    for (int i = 0; i < 40; i++) {
        char l[220];
        if (i == 7) snprintf(l, sizeof l, "- item %02d: belt buckle color RGB 190,150,60, 3 cm wide\n", i);
        else if (i == 21) snprintf(l, sizeof l, "- item %02d: rim light at 3000 K from the upper left\n", i);
        else if (i == 33) snprintf(l, sizeof l, "- item %02d: staff of dark wood, 120 cm tall\n", i);
        else snprintf(l, sizeof l, "- item %02d: small plain ornament number %d somewhere near the border of the picture\n", i, i * 7);
        strncat(big, l, sizeof big - strlen(big) - 1);
    }
    const int tk0 = sdb_count_est(big, NULL);
    o = sds_prompt(big, NULL, 200, sdb_count_est, NULL, &rep, 0);
    const int tk = sdb_count_est(o, NULL);
    printf("  longo: %d -> %d tokens (orcamento 200), %d unidades, %d cortadas\n", tk0, tk, rep.nunits, rep.ncut);
    bad += sdb_tck("longo: cabe no orcamento e houve corte", tk0 > 200 && tk <= 200 && rep.ncut > 0 && !rep.whole);
    bad += sdb_tck("longo: primeira linha (assunto) e regras obrigatorias ficam", strstr(o, "Pixel art portrait of a young sorceress") && strstr(o, "REGRAS ABSOLUTAS:") && strstr(o, "never add text or logos") && strstr(o, "keep the face unchanged"));
    bad += sdb_tck("longo: alto sinal (RGB, K, cm) fica; ornamentos sem sinal saem", strstr(o, "RGB 190,150,60") && strstr(o, "3000 K") && strstr(o, "120 cm") && !strstr(o, "number 14 "));
    bad += sdb_tck("longo: ordem original (assunto < regras < itens, item 07 < 21 < 33)", sdb_has_in_order(o, "Pixel art", "REGRAS ABSOLUTAS") && sdb_has_in_order(o, "REGRAS ABSOLUTAS", "item 07") && sdb_has_in_order(o, "item 07", "item 21") && sdb_has_in_order(o, "item 21", "item 33"));
    bad += sdb_tck("longo: nenhuma palavra acrescentada (subsequencia do original)", sdb_subseq(big, o));
    bad += sdb_tck("longo: cabecalho DETALHES fica porque algum filho entrou", strstr(o, "DETALHES:") != NULL);
    bad += sdb_tck("longo: o relatorio lista o corte", rep.cut[0] && rep.used == tk);
    bad += sdb_tck("(iii) gabarito antigo ausente (architectural, interior photograph, 3D model export, no watermark)", sdb_no_old_template(big, o) && sdb_no_old_template(SDB_T_MAGA, SDB_T_MAGA));
    free(o);
    /* cabecalho sem filho escolhido nao fica; pedido extra obrigatorio */
    o = sds_prompt(big, "make it night", 120, sdb_count_est, NULL, &rep, 0);
    bad += sdb_tck("pedido extra (--nl) fica sempre, como ultima linha", strstr(o, "make it night") && strlen(o) >= 13 && !strcmp(o + strlen(o) - 13, "make it night") && sdb_count_est(o, NULL) <= 120);
    free(o);
    /* lista de tags numa linha so: cada tag e uma unidade, corte na tag inteira */
    static char tags[4000]; snprintf(tags, sizeof tags, "pixel art sorceress, 16-bit, green robe RGB 30,110,60");
    for (int i = 0; i < 90; i++) { char t[40]; snprintf(t, sizeof t, ", filler tag%d", i); strncat(tags, t, sizeof tags - strlen(tags) - 1); }
    o = sds_prompt(tags, NULL, 100, sdb_count_est, NULL, &rep, 0);
    bad += sdb_tck("tags: cabe, comeca no assunto, mantem a cor RGB, sem virgula pendurada, sem acrescimo", sdb_count_est(o, NULL) <= 100 && !strncmp(o, "pixel art sorceress,", 20) && strstr(o, "RGB 30,110,60") && o[strlen(o) - 1] != ',' && sdb_subseq(tags, o));
    free(o);
    if (real_path) {                                                          /* briefing real (dado local, nunca no repositorio): so confere, o texto nao e impresso */
        char *real = sdb_read_text(real_path);
        if (real) {
            for (int e = 0; e < 2; e++) {
                SdsRep r; char *out = e ? sdb_stable_prompt(real, NULL, NULL, 4, &r) : sdb_flux_prompt(real, NULL, &r);
                printf("  arquivo (%s): %d/%d tokens, %d cortadas\n", e ? "stable" : "flux", r.used, r.budget, r.ncut);
                bad += sdb_tck(e ? "arquivo/stable: cabe, ordem, nada acrescentado, sem gabarito antigo" : "arquivo/flux: cabe, ordem, nada acrescentado, sem gabarito antigo", r.used <= r.budget && sdb_subseq(real, out) && sdb_no_old_template(real, out));
                free(out);
            }
        }
    }
    return bad;
}

static int sdb_selftest(const char *file) {
    int bad = sdb_selftest_unit(file);
    /* contador real do FLUX (se o tokenizador existir): o curto passa intacto e o longo cabe nas 504 posicoes */
    SdsRep rep; char *o = sdb_flux_prompt(SDB_T_MAGA, NULL, &rep);
    bad += sdb_tck("FLUX: briefing curto da maga passa intacto", rep.whole && !strcmp(o, SDB_T_MAGA)); free(o);
    static char longp[20000]; longp[0] = 0;
    snprintf(longp, sizeof longp, "%s", SDB_T_MAGA);
    for (int i = 0; i < 60; i++) { char l[200]; snprintf(l, sizeof l, "Detail %d: a small ornament, number %d, somewhere near the border of the whole picture, dull grey.\n", i, i * 13); strncat(longp, l, sizeof longp - strlen(longp) - 1); }
    o = sdb_flux_prompt(longp, NULL, &rep);
    bad += sdb_tck("FLUX: briefing longo cabe nas 504 posicoes, com corte, sem acrescimo", rep.used <= SDB_FLUX_BUDGET && rep.ncut > 0 && sdb_subseq(longp, o) && !strncmp(o, "Pixel art portrait", 18)); free(o);
    o = sdb_stable_prompt(longp, NULL, NULL, 4, &rep);
    bad += sdb_tck("Stable: briefing longo cabe em 4 blocos (estimativa), sem acrescimo", rep.used <= 296 && rep.ncut > 0 && sdb_subseq(longp, o)); free(o);
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
    SdsRep rep; char *prompt = sdb_stable_prompt(text, NULL, cp, P.max_chunks, &rep); const char *negative = SDB_NEG_DEFAULT;
    fprintf(stderr, "[brief] prompt (o briefing, sem reescrita): %s\n[brief] negativo: %s\n", prompt, negative);
    sds_report(&rep, "stable", stderr);
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
