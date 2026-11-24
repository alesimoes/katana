/* ============================ render: FLUX.2 klein, o motor completo (etapa F5) ===============================
 * texto (fxtext.c) + transformador (fxdit.c) + VAE (fxvae.c) + amostrador. Usado por `katana render generate --engine flux`.
 *
 *   fxg_generate(prompt, ref1, ref2, w, h, steps, seed, out)   gera e grava o PNG; 0 = falhou
 *   katana render flux-gen selftest [pasta-de-referencia]       compara o laco inteiro com a referencia em float32
 *
 * O modelo e destilado: sem prompt negativo e sem guia (guidance 1.0), 4 passos por padrao (1 a 8). O agendamento e o de fluxo
 * (Euler): sigmas lineares de 1 a 1/passos, deslocados por um mu que depende do numero de fichas da imagem (a formula empirica do
 * pipeline), mais um 0 no fim; cada passo faz x += (sigma_proximo - sigma) * v. Nenhum filtro de ruido extra.
 *
 * Imagens de referencia (ate 2): vao ao codificador do VAE e entram como fichas extras depois das fichas do alvo, com o eixo de tempo
 * dos ids deslocado (10, 20, ...); o transformador atende todas, mas so as fichas do alvo sao atualizadas. A referencia e redimensionada
 * para o tamanho do alvo (lados multiplos de 16). */

#define FXG_MAXREF 2

static double fxg_mu(int seq, int steps) {
    const double a1 = 8.73809524e-05, b1 = 1.89833333, a2 = 0.00016927, b2 = 0.45666666;
    if (seq > 4300) return a2 * seq + b2;
    const double m200 = a2 * seq + b2, m10 = a1 * seq + b1, a = (m200 - m10) / 190.0, b = m200 - 200.0 * a;
    return a * steps + b;
}

/* n+1 sigmas: linspace(1, 1/n, n) deslocado por exp(mu) / (exp(mu) + (1/s - 1)), e 0 no fim */
static void fxg_sigmas(double *sg, int n, int seq) {
    const double mu = fxg_mu(seq, n);
    for (int i = 0; i < n; i++) {
        const double s = n == 1 ? 1.0 : 1.0 - (double)i * (1.0 - 1.0 / n) / (n - 1);
        const double sf = (double)(float)s;
        sg[i] = exp(mu) / (exp(mu) + (1.0 / sf - 1.0));
    }
    sg[n] = 0.0;
}

/* ids [n][4] float: (t, h, w, l) */
static void fxg_ids_grid(float *ids, int th, int tw, float t) {
    for (int i = 0; i < th; i++) for (int j = 0; j < tw; j++) { float *p = ids + ((size_t)i * tw + j) * 4; p[0] = t; p[1] = (float)i; p[2] = (float)j; p[3] = 0; }
}

static void fxg_dump(const char *dir, const char *name, const float *x, size_t n) {
    if (!dir) return;
    char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(x, 4, n, f); fclose(f); }
}

/* o laco inteiro. maxlen: posicoes de texto (512 no uso normal). noise_in (opcional): ruido pronto [fichas][128] (teste).
 * dumpdir (opcional): grava o latente depois de cada passo. Devolve [H*W][3] em [-1,1] (malloc) ou NULL */
static float *fxg_run(const char *prompt, const char *const *ref, int nref, int W, int H, int steps, uint64_t seed, int maxlen, const float *noise_in, const char *dumpdir) {
    char dir[1100], p[1200];
    snprintf(dir, sizeof dir, "%s/flux2klein/gguf", models_dir());
    if (W % 16 || H % 16 || W < 16 || H < 16) { fprintf(stderr, "render: o tamanho do FLUX deve ter lados multiplos de 16 (%dx%d)\n", W, H); return NULL; }
    if (steps < 1 || steps > 8) { fprintf(stderr, "render: --steps do FLUX entre 1 e 8\n"); return NULL; }
    static FxText T; static FxVae V; static FxDit D;
    snprintf(p, sizeof p, "%s/qwen3-4b-text.gguf", dir); if (!fxt_load(&T, p)) { fprintf(stderr, "render: falta %s (katana render flux-convert-text)\n", p); return NULL; }
    snprintf(p, sizeof p, "%s/flux2-vae.gguf", dir); if (!fxv_load(&V, p)) { fprintf(stderr, "render: falta %s (katana render flux-vae convert)\n", p); return NULL; }
    snprintf(p, sizeof p, "%s/flux-2-klein-4b-Q4_K_M.gguf", dir); if (!fxd_open(&D, p)) { fprintf(stderr, "render: falta %s\n", p); return NULL; }
    const int th = H / 16, tw = W / 16, Nt = th * tw;
    /* 1. texto */
    int nreal = 0;
    float *emb = fxt_embed(&T, prompt, maxlen, &nreal);
    if (!emb) return NULL;
    fprintf(stderr, "[flux] texto: %d tokens de %d posicoes\n", nreal, maxlen);
    fxg_dump(dumpdir, "c_text.bin", emb, (size_t)maxlen * 7680);
    float *ids_txt = xalloc((size_t)maxlen * 4, 4);
    for (int l = 0; l < maxlen; l++) { ids_txt[l * 4] = 0; ids_txt[l * 4 + 1] = 0; ids_txt[l * 4 + 2] = 0; ids_txt[l * 4 + 3] = (float)l; }
    /* 2. latente do alvo (ruido) e fichas das referencias */
    const int Ni = Nt * (1 + nref);
    float *x = xalloc((size_t)Nt * 128, 4), *in = xalloc((size_t)Ni * 128, 4), *ids = xalloc((size_t)Ni * 4, 4), *vel = xalloc((size_t)Ni * 128, 4);
    if (noise_in) memcpy(x, noise_in, (size_t)Nt * 128 * 4);
    else { SdpRng r = { seed, 0, 0 }; for (size_t i = 0; i < (size_t)Nt * 128; i++) x[i] = sdp_rng_gauss(&r); }
    fxg_ids_grid(ids, th, tw, 0.0f);
    for (int k = 0; k < nref; k++) {
        uint8_t *rgb = sdp_load_image(ref[k], W, H);
        if (!rgb) return NULL;
        float *img = xalloc((size_t)W * H * 3, 4);
        for (size_t i = 0; i < (size_t)W * H * 3; i++) img[i] = (float)rgb[i] / 127.5f - 1.0f;
        int a, b; float *tok = fxv_encode(&V, img, H, W, &a, &b);
        if (!tok) return NULL;
        memcpy(in + (size_t)(1 + k) * Nt * 128, tok, (size_t)Nt * 128 * 4);
        fxg_ids_grid(ids + (size_t)(1 + k) * Nt * 4, th, tw, (float)(10 + 10 * k));
        fxg_dump(dumpdir, k ? "ref1_tokens.bin" : "ref0_tokens.bin", tok, (size_t)Nt * 128);
        free(img); free(tok);
    }
    /* 3. agendamento e laco */
    double sg[16]; fxg_sigmas(sg, steps, Nt);
    fprintf(stderr, "[flux] %dx%d, %d fichas de alvo + %d de referencia, %d passos, sigmas:", W, H, Nt, Nt * nref, steps);
    for (int i = 0; i <= steps; i++) fprintf(stderr, " %.4f", sg[i]);
    fprintf(stderr, "\n");
    for (int s = 0; s < steps; s++) {
        memcpy(in, x, (size_t)Nt * 128 * 4);
        if (!fxd_forward(&D, in, ids, Ni, emb, ids_txt, maxlen, (float)sg[s], vel, NULL)) return NULL;
        const float dt = (float)(sg[s + 1] - sg[s]);
        for (size_t i = 0; i < (size_t)Nt * 128; i++) x[i] += dt * vel[i];
        char nm[64]; snprintf(nm, sizeof nm, "lat_step%d.bin", s + 1); fxg_dump(dumpdir, nm, x, (size_t)Nt * 128);
        fprintf(stderr, "[flux] passo %d/%d\n", s + 1, steps);
    }
    /* 4. imagem */
    int Ho, Wo;
    float *rgb = fxv_decode(&V, x, th, tw, &Ho, &Wo);
    free(emb); free(ids_txt); free(x); free(in); free(ids); free(vel);
    if (!rgb || Ho != H || Wo != W) { fprintf(stderr, "render: o decodificador devolveu %dx%d, esperado %dx%d\n", Wo, Ho, W, H); return NULL; }
    return rgb;
}

/* gera e grava o PNG. w/h = 0: proporcao da primeira referencia com o lado maior em 576 (sem referencia: 512x512) */
static int fxg_generate(const char *prompt, const char *ref1, const char *ref2, int w, int h, int steps, uint64_t seed, const char *out) {
    const char *ref[FXG_MAXREF]; int nref = 0;
    if (ref1) ref[nref++] = ref1;
    if (ref2) ref[nref++] = ref2;
    if ((!w || !h) && nref) {
        int iw, ih;
        if (!sdp_peek_size(ref[0], &iw, &ih)) { fprintf(stderr, "render: nao consegui ler o tamanho de %s\n", ref[0]); return 0; }
        const double s = 576.0 / (iw > ih ? iw : ih);
        w = (int)(iw * s / 16.0 + 0.5) * 16; h = (int)(ih * s / 16.0 + 0.5) * 16;
        if (w < 16) w = 16; if (h < 16) h = 16;
    }
    if (!w || !h) { w = 512; h = 512; }
    if (steps <= 0) steps = 4;
    float *rgb = fxg_run(prompt, ref, nref, w, h, steps, seed, 512, NULL, NULL);
    if (!rgb) return 0;
    uint8_t *px = xalloc((size_t)w * h * 3, 1);
    for (size_t i = 0; i < (size_t)w * h * 3; i++) { float v = (rgb[i] * 0.5f + 0.5f) * 255.0f; px[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f)); }
    const int ok = sdp_png_write(out, px, w, h);
    free(px); free(rgb);
    if (ok) fprintf(stderr, "[flux] pronto: %s (%dx%d)\n", out, w, h);
    return ok;
}

/* ---------- teste: o laco inteiro contra a referencia em float32 ---------- */
static float *fxg_read(const char *dir, const char *name, size_t *n) {
    char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, name);
    int64_t sz = 0; const uint8_t *b = map_file(p, &sz);
    if (!b) { fprintf(stderr, "flux-gen: nao achei %s\n", p); return NULL; }
    float *o = xalloc((size_t)sz, 1); memcpy(o, b, (size_t)sz); *n = (size_t)sz / 4;
    return o;
}

static int fxg_cmp(const char *what, const float *a, const float *b, size_t n, double lim) {
    double mx = 0, rm = 0, sa = 0, sb = 0;
    for (size_t i = 0; i < n; i++) { const double d = fabs((double)a[i] - b[i]); if (d > mx) mx = d; if (fabs((double)b[i]) > rm) rm = fabs((double)b[i]); sa += d; sb += fabs((double)b[i]); }
    const double rel = rm > 0 ? mx / rm : mx, mean = sb > 0 ? sa / sb : sa;
    const int ok = rel < lim && mean < lim * 0.1;
    printf("  %-34s %s  erro max/|ref|max %.2e  razao media %.2e\n", what, ok ? "OK    " : "FALHOU", rel, mean);
    return ok ? 0 : 1;
}

static int fxg_selftest(const char *dir) {
    char cd[1200], p[1200]; snprintf(cd, sizeof cd, "%s/c_out", dir);
    mkdir(cd, 0777);
    size_t n; int bad = 0;
    float *meta = fxg_read(dir, "meta.bin", &n);                           /* W, H, passos, maxlen */
    float *noise = fxg_read(dir, "noise.bin", &n);
    if (!meta || !noise) return 1;
    const int W = (int)meta[0], H = (int)meta[1], steps = (int)meta[2], maxlen = (int)meta[3];
    snprintf(p, sizeof p, "%s/prompt.txt", dir);
    int64_t sz = 0; const uint8_t *pb = map_file(p, &sz);
    if (!pb) { fprintf(stderr, "flux-gen: nao achei %s\n", p); return 1; }
    char *prompt = xalloc((size_t)sz + 1, 1); memcpy(prompt, pb, (size_t)sz);
    char refp[1200]; snprintf(refp, sizeof refp, "%s/ref.png", dir);
    const char *ref[1] = { refp };
    float *rgb = fxg_run(prompt, ref, 1, W, H, steps, 0, maxlen, noise, cd);
    if (!rgb) return 1;
    float *r = fxg_read(dir, "c_text.bin", &n); float *m = fxg_read(cd, "c_text.bin", &n);
    if (r && m) bad += fxg_cmp("texto [maxlen][7680]", m, r, n, 1e-3); free(r); free(m);
    r = fxg_read(dir, "ref0_tokens.bin", &n); m = fxg_read(cd, "ref0_tokens.bin", &n);
    if (r && m) bad += fxg_cmp("referencia (fichas do VAE)", m, r, n, 1e-3); free(r); free(m);
    for (int s = 1; s <= steps; s++) {
        char nm[64], lb[96]; snprintf(nm, sizeof nm, "lat_step%d.bin", s); snprintf(lb, sizeof lb, "latente depois do passo %d", s);
        r = fxg_read(dir, nm, &n); m = fxg_read(cd, nm, &n);
        if (r && m) bad += fxg_cmp(lb, m, r, n, 1e-3); else bad++;
        free(r); free(m);
    }
    r = fxg_read(dir, "final_rgb.bin", &n);
    if (r) {
        bad += fxg_cmp("imagem final [H*W][3] em [-1,1]", rgb, r, n, 1e-2);
        double s = 0; for (size_t i = 0; i < n; i++) s += fabs((double)rgb[i] - r[i]) * 127.5;
        printf("  diferenca media de pixel: %.3f de 255\n", s / (double)n);
    } else bad++;
    printf("RESULTADO: %s\n", bad ? "FALHOU" : "todos passaram");
    return bad ? 1 : 0;
}

static int fxg_main(int argc, char **argv) {
    if (argc < 1 || strcmp(argv[0], "selftest")) { fprintf(stderr, "uso: katana render flux-gen selftest [pasta-referencia]\n"); return 1; }
    return fxg_selftest(argc > 1 ? argv[1] : "/mnt/dados/render-ref/fluxD");
}
