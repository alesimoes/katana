/* ============================ render: treino de LoRA do FLUX.2 klein (katana flux-train) ===============================
 *   katana flux-train --pairs <pasta> --out arq.fxlora [opcoes]
 *   katana render flux-train selftest-grad | selftest-overfit [passos]
 *
 * Pares na pasta: <nome>_cond.png (vista do SketchUp), <nome>_target.png (render aprovado), <nome>.txt (legenda) e, opcional,
 * <nome>_lines.png (desenho de linhas, segunda referencia). Texto (fxt_embed) e imagens (fxv_encode) sao codificados uma vez e ficam em
 * <saida>.cache/. Perda de fluxo: x_t = (1 - s) x0 + s e, velocidade alvo v = e - x0 (a mesma convencao do amostrador de fxgen.c:
 * x += (s_prox - s) v); a imagem de condicao entra como fichas de referencia (ids de tempo 10, 20) exatamente como na geracao; a perda e o
 * erro quadratico medio so nas fichas do alvo. s: --t-mode shifted (padrao): u uniforme em (0,1) e s = e^mu / (e^mu + 1/u - 1) com o mu
 * dinamico de fxg_mu (4 passos); --t-mode steps: um dos sigmas de fxg_sigmas(4) por sorteio (so os pontos usados na geracao).
 * Otimizador: AdamW (b1 .9, b2 .999, eps 1e-8) com corte da norma global e correcao de vies, como o `katana train`; estado em <saida>.kopt. */

typedef struct {
    const char *pairs, *out, *cache, *tmode;
    int rank, steps, accum, W, H, txtlen, save_every, eval_every, seed, rebuild, resume, warmup, nsig;
    float alpha, lr, wd, clip;
} FxrOpt;

typedef struct { char stem[256]; float *txt; float *tok[1 + FXG_MAXREF]; int nref; } FxrPair;     /* tok[0] = alvo, tok[1..] = referencias */

static long fxr_rss_kb(const char *key) {
    FILE *f = fopen("/proc/self/status", "r"); char l[256]; long v = -1; const size_t kl = strlen(key);
    if (!f) return -1;
    while (fgets(l, sizeof l, f)) if (!strncmp(l, key, kl)) { v = atol(l + kl); break; }
    fclose(f); return v;
}

static char *fxr_read_text(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = xalloc((size_t)n + 1, 1); if (fread(s, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s); return NULL; }
    fclose(f); while (strlen(s) > 0 && (s[strlen(s) - 1] == '\n' || s[strlen(s) - 1] == '\r')) s[strlen(s) - 1] = 0;
    return s;
}
static int fxr_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
/* acha <pasta>/<stem><suf>.{png,jpg,jpeg} */
static int fxr_find_img(const char *dir, const char *stem, const char *suf, char *out, size_t cap) {
    static const char *ext[3] = { "png", "jpg", "jpeg" };
    for (int i = 0; i < 3; i++) { snprintf(out, cap, "%s/%s%s.%s", dir, stem, suf, ext[i]); if (fxr_exists(out)) return 1; }
    return 0;
}
static int fxr_cmp_str(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* lista os nomes (stems) com _cond, _target e legenda */
static int fxr_scan(const char *dir, char (*stems)[256], int cap) {
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "flux-train: nao abriu %s\n", dir); return -1; }
    int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < cap) {
        const char *nm = e->d_name; const char *c = strstr(nm, "_cond.");
        if (!c || c == nm) continue;
        char stem[256]; snprintf(stem, sizeof stem, "%.*s", (int)(c - nm), nm);
        char p[1400];
        if (!fxr_find_img(dir, stem, "_target", p, sizeof p)) { fprintf(stderr, "flux-train: %s sem _target, ignorado\n", nm); continue; }
        snprintf(p, sizeof p, "%s/%s.txt", dir, stem); if (!fxr_exists(p)) { fprintf(stderr, "flux-train: %s sem %s.txt, ignorado\n", nm, stem); continue; }
        snprintf(stems[n++], 256, "%s", stem);
    }
    closedir(d);
    qsort(stems, (size_t)n, 256, fxr_cmp_str);
    return n;
}

/* cache de um par: "FXCACHE1", W, H, txtlen, nref, impressao da legenda, depois texto [txtlen][7680] e fichas [1 + nref][Ntg][128] */
static uint64_t fxr_hash_str(const char *s) { uint64_t h = 1469598103934665603ULL; for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ULL; return h; }
static int fxr_cache_read(const char *path, const FxrOpt *o, int nref, uint64_t ph, FxrPair *P) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    char mg[8]; int32_t h[4]; uint64_t hh = 0; const size_t Ntg = (size_t)(o->W / 16) * (o->H / 16);
    int ok = fread(mg, 1, 8, f) == 8 && !memcmp(mg, "FXCACHE1", 8) && fread(h, 4, 4, f) == 4 && fread(&hh, 8, 1, f) == 1
          && h[0] == o->W && h[1] == o->H && h[2] == o->txtlen && h[3] == nref && hh == ph;
    if (ok) {
        P->txt = xalloc((size_t)o->txtlen * FXD_TXTCH, 4); ok = fread(P->txt, 4, (size_t)o->txtlen * FXD_TXTCH, f) == (size_t)o->txtlen * FXD_TXTCH;
        for (int k = 0; k <= nref && ok; k++) { P->tok[k] = xalloc(Ntg * 128, 4); ok = fread(P->tok[k], 4, Ntg * 128, f) == Ntg * 128; }
    }
    fclose(f);
    if (!ok) { free(P->txt); P->txt = NULL; for (int k = 0; k <= nref; k++) { free(P->tok[k]); P->tok[k] = NULL; } }
    return ok;
}
static int fxr_cache_write(const char *path, const FxrOpt *o, int nref, uint64_t ph, const FxrPair *P) {
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    const int32_t h[4] = { o->W, o->H, o->txtlen, nref }; const size_t Ntg = (size_t)(o->W / 16) * (o->H / 16);
    fwrite("FXCACHE1", 1, 8, f); fwrite(h, 4, 4, f); fwrite(&ph, 8, 1, f);
    fwrite(P->txt, 4, (size_t)o->txtlen * FXD_TXTCH, f);
    for (int k = 0; k <= nref; k++) fwrite(P->tok[k], 4, Ntg * 128, f);
    const int ok = !ferror(f); return fclose(f) == 0 && ok;
}

static float *fxr_encode_image(const FxVae *V, const char *path, int W, int H) {
    uint8_t *rgb = sdp_load_image(path, W, H); if (!rgb) return NULL;
    float *img = xalloc((size_t)W * H * 3, 4);
    for (size_t i = 0; i < (size_t)W * H * 3; i++) img[i] = (float)rgb[i] / 127.5f - 1.0f;
    int a, b; float *tok = fxv_encode(V, img, H, W, &a, &b);
    free(img);
    return tok;
}

/* carrega os pares (do cache ou codificando agora). Devolve quantos */
static int fxr_load_pairs(const FxrOpt *o, FxrPair **out) {
    static char stems[512][256];
    const int n = fxr_scan(o->pairs, stems, 512);
    if (n <= 0) { if (n == 0) fprintf(stderr, "flux-train: nenhum par (<nome>_cond.png + <nome>_target.png + <nome>.txt) em %s\n", o->pairs); return 0; }
    char cdir[1200]; snprintf(cdir, sizeof cdir, "%s.cache", o->cache ? o->cache : o->out); mkdir(cdir, 0777);
    FxrPair *P = xalloc((size_t)n, sizeof(FxrPair));
    static FxText T; static FxVae V; int have_t = 0, have_v = 0; char dir[1100], p[1500];
    snprintf(dir, sizeof dir, "%s/flux2klein/gguf", models_dir());
    for (int i = 0; i < n; i++) {
        FxrPair *pr = &P[i]; snprintf(pr->stem, sizeof pr->stem, "%s", stems[i]);
        char ip[1500], tp[1500], lp[1500], cp[1500]; int nref = 1;
        snprintf(tp, sizeof tp, "%s/%s.txt", o->pairs, stems[i]);
        char *prompt = fxr_read_text(tp); if (!prompt) { fprintf(stderr, "flux-train: nao leu %s\n", tp); return 0; }
        if (fxr_find_img(o->pairs, stems[i], "_lines", lp, sizeof lp)) nref = 2;
        pr->nref = nref;
        snprintf(cp, sizeof cp, "%s/%s.fxc", cdir, stems[i]);
        const uint64_t ph = fxr_hash_str(prompt);
        if (!o->rebuild && fxr_cache_read(cp, o, nref, ph, pr)) { free(prompt); continue; }
        if (!have_t) { snprintf(p, sizeof p, "%s/qwen3-4b-text.gguf", dir); if (!fxt_load(&T, p)) { fprintf(stderr, "flux-train: falta %s\n", p); return 0; } have_t = 1; }
        if (!have_v) { snprintf(p, sizeof p, "%s/flux2-vae.gguf", dir); if (!fxv_load(&V, p)) { fprintf(stderr, "flux-train: falta %s\n", p); return 0; } have_v = 1; }
        int nreal = 0; pr->txt = fxt_embed(&T, prompt, o->txtlen, &nreal);
        if (!pr->txt) return 0;
        fxr_find_img(o->pairs, stems[i], "_target", ip, sizeof ip); pr->tok[0] = fxr_encode_image(&V, ip, o->W, o->H);
        fxr_find_img(o->pairs, stems[i], "_cond", ip, sizeof ip); pr->tok[1] = fxr_encode_image(&V, ip, o->W, o->H);
        if (nref == 2) pr->tok[2] = fxr_encode_image(&V, lp, o->W, o->H);
        for (int k = 0; k <= nref; k++) if (!pr->tok[k]) return 0;
        fprintf(stderr, "[flux-train] %s: legenda de %d tokens (de %d posicoes), %d referencia(s) codificadas\n", stems[i], nreal, o->txtlen, nref);
        if (!fxr_cache_write(cp, o, nref, ph, pr)) fprintf(stderr, "flux-train: aviso: nao gravou %s\n", cp);
        free(prompt);
    }
    *out = P;
    return n;
}

/* ---- passo de tempo e ruido ---- */
static double fxr_sigma(const FxrOpt *o, SdpRng *r, int Ntg) {
    if (!strcmp(o->tmode, "steps")) { double sg[16]; fxg_sigmas(sg, o->nsig, Ntg); return sg[(int)(sdp_rng_u01(r) * o->nsig) % o->nsig]; }
    const double u = sdp_rng_u01(r), mu = fxg_mu(Ntg, 4);
    return exp(mu) / (exp(mu) + (1.0 / u - 1.0));
}

/* entradas de um passo: x_t = (1 - s) x0 + s e, ids, alvo v = e - x0 */
static void fxr_build(const FxrPair *P, const float *noise, double s, int Th, int Tw, float *img, float *ids, float *tgt) {
    const size_t Ntg = (size_t)Th * Tw;
    for (size_t i = 0; i < Ntg * 128; i++) { const float x0 = P->tok[0][i], e = noise[i]; img[i] = (float)((1.0 - s) * x0 + s * e); tgt[i] = e - x0; }
    for (int k = 1; k <= P->nref; k++) memcpy(img + (size_t)k * Ntg * 128, P->tok[k], Ntg * 128 * 4);
    fxg_ids_grid(ids, Th, Tw, 0.0f);
    for (int k = 1; k <= P->nref; k++) fxg_ids_grid(ids + (size_t)k * Ntg * 4, Th, Tw, (float)(10 * k));
}

/* AdamW com corte da norma global; devolve a norma antes do corte */
static double fxr_adamw(int step, float lr, float wd, float clip, float gscale) {
    FxLora *L = &g_fxl_store; double ss = 0;
    for (int i = 0; i < L->n; i++) { const FxlMod *m = &L->m[i]; for (size_t j = 0; j < (size_t)m->r * m->cols; j++) ss += (double)m->gA[j] * m->gA[j]; for (size_t j = 0; j < (size_t)m->rows * m->r; j++) ss += (double)m->gB[j] * m->gB[j]; }
    const double norm = sqrt(ss) * gscale; const float sc = gscale * (clip > 0 && norm > clip ? (float)(clip / norm) : 1.0f);
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f, c1 = 1.0f - powf(b1, (float)step), c2 = 1.0f - powf(b2, (float)step);
    for (int i = 0; i < L->n; i++) {
        FxlMod *m = &L->m[i];
        for (int w = 0; w < 2; w++) {
            float *p = w ? m->B : m->A, *g = w ? m->gB : m->gA, *m1 = w ? m->mB1 : m->mA1, *m2 = w ? m->mB2 : m->mA2; const size_t n = w ? (size_t)m->rows * m->r : (size_t)m->r * m->cols;
            for (size_t j = 0; j < n; j++) {
                const float gr = g[j] * sc;
                m1[j] = b1 * m1[j] + (1 - b1) * gr; m2[j] = b2 * m2[j] + (1 - b2) * gr * gr;
                p[j] -= lr * ((m1[j] / c1) / (sqrtf(m2[j] / c2) + eps) + wd * p[j]);
            }
        }
    }
    return norm;
}

/* perda de avaliacao: ruido e s fixos (semente 777), media de dois s; so a ida */
static double fxr_eval(const FxDit *F, const FxrPair *P, const FxrOpt *o, const float *ids_txt, int Th, int Tw) {
    const size_t Ntg = (size_t)Th * Tw; const int Ni = (int)Ntg * (1 + P->nref);
    float *noise = xalloc(Ntg * 128, 4), *img = xalloc((size_t)Ni * 128, 4), *ids = xalloc((size_t)Ni * 4, 4), *tgt = xalloc(Ntg * 128, 4);
    SdpRng r = { 777, 0, 0 }; for (size_t i = 0; i < Ntg * 128; i++) noise[i] = sdp_rng_gauss(&r);
    const double sg[2] = { 0.55, 0.85 }; double tot = 0;
    for (int k = 0; k < 2; k++) {
        fxr_build(P, noise, sg[k], Th, Tw, img, ids, tgt);
        double l; if (!fxr_step(F, img, ids, Ni, (int)Ntg, P->txt, ids_txt, o->txtlen, (float)sg[k], tgt, 0, &l, NULL)) return -1;
        tot += l;
    }
    free(noise); free(img); free(ids); free(tgt);
    return tot / 2;
}

static int fxr_train(const FxrOpt *o) {
    char gp[1100]; snprintf(gp, sizeof gp, "%s/flux2klein/gguf/flux-2-klein-4b-Q4_K_M.gguf", models_dir());
    FxrPair *P = NULL; const int np = fxr_load_pairs(o, &P);
    if (np <= 0) return 1;
    static FxDit F; if (!fxd_open(&F, gp)) return 1;
    const int Th = o->H / 16, Tw = o->W / 16; const size_t Ntg = (size_t)Th * Tw;
    int64_t step0 = 0;
    if (o->resume && fxr_exists(o->out)) {
        if (!fxl_load(&F, o->out, 1.0f)) return 1;
        fxr_alloc_grads(); step0 = fxl_opt_load(o->out);
        fprintf(stderr, "[flux-train] retomado de %s: %d modulos, passo %lld%s\n", o->out, g_fxl_store.n, (long long)step0, step0 < 0 ? " (sem .kopt: otimizador recomeca do zero)" : "");
        if (step0 < 0) step0 = 0;
    } else {
        if (!fxl_create(&F, o->rank, o->alpha, (uint64_t)o->seed)) return 1;
        g_fxl_on = 1; fxr_alloc_grads();
    }
    g_fxl_store.scale = 1.0f; g_fxl_on = 1; g_fxr_train = 1;
    size_t ntr = 0; for (int i = 0; i < g_fxl_store.n; i++) ntr += (size_t)g_fxl_store.m[i].r * (g_fxl_store.m[i].cols + g_fxl_store.m[i].rows);
    fprintf(stderr, "[flux-train] %d par(es), %dx%d (%zu fichas de alvo), texto %d, posto %d alpha %.1f, %zu pesos treinaveis (%.1f M), lr %g, %d passos x %d, t=%s\n",
            np, o->W, o->H, Ntg, o->txtlen, g_fxl_store.m[0].r, o->alpha, ntr, (double)ntr / 1e6, o->lr, o->steps, o->accum, o->tmode);
    float *ids_txt = xalloc((size_t)o->txtlen * 4, 4); for (int l = 0; l < o->txtlen; l++) ids_txt[l * 4 + 3] = (float)l;
    const int maxref = 2, NiMax = (int)Ntg * (1 + maxref);
    float *noise = xalloc(Ntg * 128, 4), *img = xalloc((size_t)NiMax * 128, 4), *ids = xalloc((size_t)NiMax * 4, 4), *tgt = xalloc(Ntg * 128, 4);
    SdpRng rng = { (uint64_t)o->seed * 7919 + 13 + (uint64_t)step0 * 104729, 0, 0 };
    char lp[1300]; snprintf(lp, sizeof lp, "%s.loss.csv", o->out);
    FILE *lf = fopen(lp, step0 ? "ab" : "wb"); if (lf && !step0) fprintf(lf, "step,loss,eval,sigma,grad_norm,seconds\n");
    if (o->eval_every) { const double e0 = fxr_eval(&F, &P[0], o, ids_txt, Th, Tw); fprintf(stderr, "[flux-train] perda de avaliacao inicial (ruido e s fixos): %.5f\n", e0); if (lf) fprintf(lf, "%lld,,%.6f,,,\n", (long long)step0, e0); }
    const double tstart = sdt_now(); double sum_t = 0; int nt = 0;
    for (int st = 1; st <= o->steps; st++) {
        const int gs = (int)step0 + st; const double t0 = sdt_now();
        fxr_zero_grads();
        double lsum = 0, sg_last = 0;
        for (int a = 0; a < o->accum; a++) {
            const FxrPair *pr = &P[(int)(sdp_rng_u01(&rng) * np) % np];
            for (size_t i = 0; i < Ntg * 128; i++) noise[i] = sdp_rng_gauss(&rng);
            const double s = fxr_sigma(o, &rng, (int)Ntg); sg_last = s;
            fxr_build(pr, noise, s, Th, Tw, img, ids, tgt);
            double l; const int Ni = (int)Ntg * (1 + pr->nref);
            if (!fxr_step(&F, img, ids, Ni, (int)Ntg, pr->txt, ids_txt, o->txtlen, (float)s, tgt, 1, &l, NULL)) return 1;
            lsum += l;
        }
        const float lr = o->warmup > 0 && gs <= o->warmup ? o->lr * (float)gs / (float)o->warmup : o->lr;
        const double gn = fxr_adamw(gs, lr, o->wd, o->clip, 1.0f / (float)o->accum);
        double ev = -1; if (o->eval_every && st % o->eval_every == 0) ev = fxr_eval(&F, &P[0], o, ids_txt, Th, Tw);
        const double dt = sdt_now() - t0; sum_t += dt; nt++;
        fprintf(stderr, "[flux-train] passo %d/%d  perda %.5f  s %.3f  |g| %.3e  %.1f s  RSS %.1f GB (pico %.1f)", gs, (int)step0 + o->steps, lsum / o->accum, sg_last, gn, dt, (double)fxr_rss_kb("VmRSS:") / 1048576.0, (double)fxr_rss_kb("VmHWM:") / 1048576.0);
        if (ev >= 0) fprintf(stderr, "  avaliacao %.5f", ev);
        fprintf(stderr, "\n");
        if (lf) { fprintf(lf, "%d,%.6f,", gs, lsum / o->accum); if (ev >= 0) fprintf(lf, "%.6f", ev); fprintf(lf, ",%.4f,%.4e,%.2f\n", sg_last, gn, dt); fflush(lf); }
        if ((o->save_every && st % o->save_every == 0) || st == o->steps) {
            if (!fxl_save(o->out) || !fxl_opt_save(o->out, gs)) { fprintf(stderr, "flux-train: nao gravou %s\n", o->out); return 1; }
        }
    }
    if (lf) fclose(lf);
    struct stat sb; const double mb = stat(o->out, &sb) == 0 ? (double)sb.st_size / 1048576.0 : 0.0;
    fprintf(stderr, "[flux-train] %d passos em %.0f s (%.1f s por passo, micro-lote %d), %s gravado (%.0f MB)\n", nt, sdt_now() - tstart, sum_t / (nt ? nt : 1), o->accum, o->out, mb);
    return 0;
}

/* ---- selftest de sobreajuste: um par sintetico, poucos passos ---- */
static void fxr_synth_png(const char *path, int W, int H, int kind) {          /* kind 0 = condicao (linhas), 1 = alvo (cores) */
    uint8_t *px = xalloc((size_t)W * H * 3, 1);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = px + ((size_t)y * W + x) * 3; int r, g, b;
        const int inA = x > W / 8 && x < W / 2 && y > H / 4 && y < 3 * H / 4, inB = x > W / 2 + W / 8 && x < 7 * W / 8 && y > H / 2 && y < 7 * H / 8;
        const int edgeA = inA && (x < W / 8 + 3 || x > W / 2 - 4 || y < H / 4 + 3 || y > 3 * H / 4 - 4), edgeB = inB && (x < W / 2 + W / 8 + 3 || x > 7 * W / 8 - 4 || y < H / 2 + 3 || y > 7 * H / 8 - 4);
        if (kind == 0) { r = g = b = (edgeA || edgeB || y == H * 3 / 4 || (x == y)) ? 20 : 235; }
        else {
            r = 90 + 100 * y / H; g = 140 + 60 * x / W; b = 200 - 80 * y / H;                      /* parede em degrade */
            if (y > 3 * H / 4) { r = 150; g = 105; b = 60; }                                        /* piso */
            if (inA) { r = 200; g = 40; b = 40; }
            if (inB) { r = 40; g = 70; b = 190; }
        }
        p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
    }
    sdp_png_write(path, px, W, H); free(px);
}

static int fxr_parse(int argc, char **argv, FxrOpt *o) {
    *o = (FxrOpt){ .rank = 16, .alpha = 16.0f, .steps = 100, .accum = 1, .txtlen = 512, .save_every = 20, .lr = 1e-4f, .wd = 0.0f, .clip = 1.0f, .tmode = "shifted", .nsig = 4, .seed = 1 };
    int long_side = 512;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i]; const int more = i + 1 < argc;
        if (!strcmp(a, "--pairs") && more) o->pairs = argv[++i];
        else if (!strcmp(a, "--out") && more) o->out = argv[++i];
        else if (!strcmp(a, "--cache") && more) o->cache = argv[++i];
        else if (!strcmp(a, "--rank") && more) o->rank = atoi(argv[++i]);
        else if (!strcmp(a, "--alpha") && more) o->alpha = (float)atof(argv[++i]);
        else if (!strcmp(a, "--steps") && more) o->steps = atoi(argv[++i]);
        else if (!strcmp(a, "--accum") && more) o->accum = atoi(argv[++i]);
        else if (!strcmp(a, "--lr") && more) o->lr = (float)atof(argv[++i]);
        else if (!strcmp(a, "--wd") && more) o->wd = (float)atof(argv[++i]);
        else if (!strcmp(a, "--clip") && more) o->clip = (float)atof(argv[++i]);
        else if (!strcmp(a, "--warmup") && more) o->warmup = atoi(argv[++i]);
        else if (!strcmp(a, "--size") && more) { if (sscanf(argv[++i], "%dx%d", &o->W, &o->H) != 2) { fprintf(stderr, "flux-train: --size espera LARGURAxALTURA\n"); return 0; } }
        else if (!strcmp(a, "--long") && more) long_side = atoi(argv[++i]);
        else if (!strcmp(a, "--txt-len") && more) o->txtlen = atoi(argv[++i]);
        else if (!strcmp(a, "--save-every") && more) o->save_every = atoi(argv[++i]);
        else if (!strcmp(a, "--eval-every") && more) o->eval_every = atoi(argv[++i]);
        else if (!strcmp(a, "--t-mode") && more) o->tmode = argv[++i];
        else if (!strcmp(a, "--seed") && more) o->seed = atoi(argv[++i]);
        else if (!strcmp(a, "--rebuild-cache")) o->rebuild = 1;
        else if (!strcmp(a, "--resume")) o->resume = 1;
        else { fprintf(stderr, "flux-train: opcao desconhecida ou sem valor: %s\n", a); return 0; }
    }
    if (!o->pairs || !o->out) { fprintf(stderr, "uso: katana flux-train --pairs <pasta> --out arq.fxlora [--rank 16] [--alpha 16] [--steps 100] [--lr 1e-4] [--accum 1] [--size WxH | --long 512]\n"
                                      "       [--txt-len 512] [--t-mode shifted|steps] [--wd 0] [--clip 1] [--warmup 0] [--save-every 20] [--eval-every 0] [--seed 1] [--resume] [--rebuild-cache] [--cache pasta]\n"
                                      "  pares: <nome>_cond.png + <nome>_target.png + <nome>.txt (+ <nome>_lines.png); docs/render.md\n"); return 0; }
    if (strcmp(o->tmode, "shifted") && strcmp(o->tmode, "steps")) { fprintf(stderr, "flux-train: --t-mode shifted ou steps\n"); return 0; }
    if (o->rank < 1 || o->rank > 256 || o->steps < 1 || o->accum < 1 || o->txtlen < 8 || o->txtlen > 512) { fprintf(stderr, "flux-train: valores fora da faixa (--rank 1..256, --steps >= 1, --accum >= 1, --txt-len 8..512)\n"); return 0; }
    if (!o->W || !o->H) {                                    /* proporcao da primeira condicao, lado maior = --long */
        static char stems[512][256]; const int n = fxr_scan(o->pairs, stems, 512); char p[1500]; int iw = 0, ih = 0;
        if (n <= 0 || !fxr_find_img(o->pairs, stems[0], "_cond", p, sizeof p) || !sdp_peek_size(p, &iw, &ih)) { fprintf(stderr, "flux-train: nao consegui deduzir o tamanho (use --size)\n"); return 0; }
        const double s = (double)long_side / (iw > ih ? iw : ih);
        o->W = (int)(iw * s / 16.0 + 0.5) * 16; o->H = (int)(ih * s / 16.0 + 0.5) * 16;
        if (o->W < 16) o->W = 16;
        if (o->H < 16) o->H = 16;
    }
    if (o->W % 16 || o->H % 16) { fprintf(stderr, "flux-train: os lados devem ser multiplos de 16\n"); return 0; }
    return 1;
}

static int fxr_selftest_overfit(int argc, char **argv) {
    char dir[] = "/tmp/fxr-overfit-XXXXXX"; if (!mkdtemp(dir)) return 1;
    const int steps = argc > 0 ? atoi(argv[0]) : 30; const int W = 128, H = 128;
    char p[1300];
    snprintf(p, sizeof p, "%s/room_cond.png", dir); fxr_synth_png(p, W, H, 0);
    snprintf(p, sizeof p, "%s/room_target.png", dir); fxr_synth_png(p, W, H, 1);
    snprintf(p, sizeof p, "%s/room.txt", dir); { FILE *f = fopen(p, "w"); fputs("a red box and a blue box on a wooden floor, soft daylight", f); fclose(f); }
    char out[1300]; snprintf(out, sizeof out, "%s/overfit.fxlora", dir);
    char sbuf[32]; snprintf(sbuf, sizeof sbuf, "%d", steps);
    char *av[] = { "--pairs", dir, "--out", out, "--size", "128x128", "--txt-len", "32", "--steps", sbuf, "--lr", "1e-3", "--eval-every", "5", "--save-every", "10", "--warmup", "3" };
    FxrOpt o; if (!fxr_parse((int)(sizeof av / sizeof *av), av, &o)) return 1;
    printf("sobreajuste: um par sintetico %dx%d (alvo = caixas coloridas, condicao = desenho de linhas), %d passos, lr %g\n", W, H, steps, o.lr);
    const int rc = fxr_train(&o);
    printf("arquivos em %s (curva em overfit.fxlora.loss.csv)\n", dir);
    return rc;
}

static int fxr_main(int argc, char **argv) {
    if (argc >= 1 && !strcmp(argv[0], "selftest-grad")) return fxr_selftest_grad();
    if (argc >= 2 && !strcmp(argv[0], "selftest-dump")) return fxr_selftest_dump(argv[1]);
    if (argc >= 1 && !strcmp(argv[0], "selftest-overfit")) return fxr_selftest_overfit(argc - 1, argv + 1);
    if (argc >= 1 && !strcmp(argv[0], "flux-train")) { argc--; argv++; }
    FxrOpt o; if (!fxr_parse(argc, argv, &o)) return 1;
    return fxr_train(&o);
}
