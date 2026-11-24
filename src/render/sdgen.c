/* ============================ render: pipeline de geracao ===============================
 * katana render generate --prompt "..." [--negative "..."] [--steps 20] [--guidance 7.5] [--seed 0] [--size WxH]
 *                        [--control img] [--control-scale 1.0] [--canny BAIXO ALTO] [--init img] [--strength 0.75]
 *                        [--models pasta] --out saida.png
 * Junta as pecas das etapas 3 a 6 (docs/render.md): CLIP -> (ControlNet + UNet, duas vezes por passo, com e sem o prompt) ->
 * amostrador Euler -> VAE -> PNG. Sem --control e sem --init e texto para imagem; --init parte de uma imagem (SketchUp) com ruido parcial
 * (--strength: 1 = comeca do zero, perto de 0 = quase a imagem); --control liga o ControlNet com as bordas (filtro canny) dessa imagem.
 *
 * Amostrador (EulerDiscreteScheduler, previsao de epsilon, espacamento "linspace"): betas scaled_linear (de sqrt(beta_start) a
 * sqrt(beta_end), ao quadrado), alphas acumulados, sigma(t) = sqrt((1 - a) / a); os passos sao t = 999 ... 0 em N pontos, com sigma
 * interpolado, e sigma = 0 no fim. Entrada do modelo x / sqrt(sigma^2 + 1); x += eps * (sigma_seguinte - sigma). Guia do texto:
 * eps = eps_sem + g (eps_com - eps_sem). O ControlNet roda nos dois ramos, como no diffusers.
 * Tamanho: lados multiplos de 64 (a UNet reduz o latente tres vezes). Ruido: gerador proprio (splitmix64 + Box-Muller), nao e o do torch:
 * a mesma semente da outra imagem diferente, mas e repetivel aqui. */

/* GPU (render/sdcu.c, incluido depois): cada uma devolve 0/NULL se nao deu e a CPU faz o mesmo trabalho */
static int g_sd_be_unet, g_sd_be_vae;
static int scu_plan(const SdgFile *base, const SdgFile *ctrl, int h8, int w8, int nctx);
static int scu_eps(const SdgFile *base, const SdgFile *ctrl, const float *lat, int h8, int w8, float t, const float *ctx, int nctx, float scale, float *eps);
static int scu_cond(const SdgFile *ctrl, const float *img, int H, int W, int nctx);
static float *scu_vae_decode(const SdgFile *g, const float *lat, int h, int w, int *Ho, int *Wo);
static float *scu_vae_encode(const SdgFile *g, const float *img, int H0, int W0, int *ho, int *wo);

static double sdp_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

typedef struct { int n; double sigma[1025]; double t[1024]; double init_sigma; } SdpEuler;

static void sdp_euler_setup(SdpEuler *e, int N, int T, double beta_start, double beta_end) {
    static double tab[1024];
    double ac = 1.0;
    for (int i = 0; i < T; i++) {
        const double s = sqrt(beta_start) + (sqrt(beta_end) - sqrt(beta_start)) * (T > 1 ? (double)i / (T - 1) : 0.0), beta = s * s;
        ac *= 1.0 - beta;
        tab[i] = sqrt((1.0 - ac) / ac);
    }
    e->n = N;
    for (int i = 0; i < N; i++) {
        const double t = N > 1 ? (double)(T - 1) * (double)(N - 1 - i) / (double)(N - 1) : (double)(T - 1);
        const int lo = (int)floor(t), hi = lo + 1 < T ? lo + 1 : lo;
        e->t[i] = t;
        e->sigma[i] = tab[lo] + (tab[hi] - tab[lo]) * (t - lo);
    }
    e->sigma[N] = 0.0;
    e->init_sigma = e->sigma[0];
}

/* Amostrador LCM (LCMScheduler do diffusers com o LoRA do LCM): 4 passos, um passe do modelo por passo (sem o ramo sem prompt), epsilon.
 * alphas acumulados iguais aos do Euler; instantes: os 50 instantes de treino 19, 39, ... 999, de tras para frente, escolhidos em
 * i * 50 / N (piso): para 4 passos, 999, 759, 499 e 259. Passo: x0 = (x - sqrt(1-a_t) eps) / sqrt(a_t); den = c_out x0 + c_skip x, com
 * s = 10 t, c_skip = 0,25 / (s^2 + 0,25), c_out = s / sqrt(s^2 + 0,25); fora o ultimo passo, x = sqrt(a_prev) den + sqrt(1 - a_prev) ruido
 * novo (a_prev do proximo instante); no ultimo, x = den. De uma imagem: x = sqrt(a_t) x0 + sqrt(1 - a_t) ruido no primeiro instante usado. */
typedef struct { int n; int t[64]; double ac[1000]; } SdpLcm;
static void sdp_lcm_setup(SdpLcm *l, int N, double beta_start, double beta_end) {
    double ac = 1.0;
    for (int i = 0; i < 1000; i++) { const double s = sqrt(beta_start) + (sqrt(beta_end) - sqrt(beta_start)) * (double)i / 999.0; ac *= 1.0 - s * s; l->ac[i] = ac; }
    l->n = N;
    for (int i = 0; i < N && i < 64; i++) { const int idx = (int)floor((double)i * 50.0 / (double)N); l->t[i] = (50 - idx) * 20 - 1; }
}
static void sdp_lcm_scalings(int t, double *cskip, double *cout) { const double s = (double)t * 10.0; *cskip = 0.25 / (s * s + 0.25); *cout = s / sqrt(s * s + 0.25); }

/* ruido: splitmix64 e Box-Muller */
typedef struct { uint64_t s; int have; double spare; } SdpRng;
static uint64_t sdp_rng_next(SdpRng *r) { uint64_t z = (r->s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
static double sdp_rng_u01(SdpRng *r) { return ((double)(sdp_rng_next(r) >> 11) + 0.5) / 9007199254740992.0; }
static float sdp_rng_gauss(SdpRng *r) {
    if (r->have) { r->have = 0; return (float)r->spare; }
    const double u1 = sdp_rng_u01(r), u2 = sdp_rng_u01(r), m = sqrt(-2.0 * log(u1));
    r->spare = m * sin(6.283185307179586 * u2); r->have = 1;
    return (float)(m * cos(6.283185307179586 * u2));
}

/* ---------- PNG sem compressao (blocos "stored" do deflate): simples e correto; o arquivo fica do tamanho da imagem ---------- */
static uint32_t sdp_crc(uint32_t c, const uint8_t *p, size_t n) {
    static uint32_t tab[256]; static int init;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; tab[i] = v; } init = 1; }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void sdp_be32(uint8_t *o, uint32_t v) { o[0] = (uint8_t)(v >> 24); o[1] = (uint8_t)(v >> 16); o[2] = (uint8_t)(v >> 8); o[3] = (uint8_t)v; }
static void sdp_chunk(FILE *f, const char *type, const uint8_t *d, size_t n) {
    uint8_t h[8]; sdp_be32(h, (uint32_t)n); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = sdp_crc(0, (const uint8_t *)type, 4); if (n) c = sdp_crc(c, d, n);      /* a CRC e do tipo mais os dados */
    uint8_t cb[4]; sdp_be32(cb, c); fwrite(cb, 1, 4, f);
}
static int sdp_png_write(const char *path, const uint8_t *rgb, int w, int h) {
    const size_t rowb = (size_t)w * 3 + 1, rawn = rowb * (size_t)h;
    uint8_t *raw = xalloc(rawn, 1);
    for (int y = 0; y < h; y++) { raw[(size_t)y * rowb] = 0; memcpy(raw + (size_t)y * rowb + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    const size_t nblk = (rawn + 65534) / 65535, zn = 2 + rawn + nblk * 5 + 4;
    uint8_t *z = xalloc(zn, 1); size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawn; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    for (size_t p = 0; p < rawn; p += 65535) {
        const size_t l = rawn - p < 65535 ? rawn - p : 65535;
        z[o++] = p + l >= rawn ? 1 : 0; z[o++] = (uint8_t)l; z[o++] = (uint8_t)(l >> 8); z[o++] = (uint8_t)~l; z[o++] = (uint8_t)(~l >> 8);
        memcpy(z + o, raw + p, l); o += l;
    }
    sdp_be32(z + o, (b << 16) | a); o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return 0; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13]; sdp_be32(ihdr, (uint32_t)w); sdp_be32(ihdr + 4, (uint32_t)h); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    sdp_chunk(f, "IHDR", ihdr, 13); sdp_chunk(f, "IDAT", z, o); sdp_chunk(f, "IEND", NULL, 0);
    const int ok = fclose(f) == 0;
    free(raw); free(z);
    return ok;
}

/* ---------- bordas (Canny): cinza, Sobel 3x3, supressao de nao maximos, histerese; saida 0 ou 1 por pixel ---------- */
static uint8_t *sdp_canny(const uint8_t *rgb, int w, int h, int lo, int hi) {
    float *gr = xalloc((size_t)w * h, 4); int *mag = xalloc((size_t)w * h, 4); uint8_t *dir = xalloc((size_t)w * h, 1), *out = xalloc((size_t)w * h, 1);
    for (int i = 0; i < w * h; i++) gr[i] = 0.299f * rgb[3 * i] + 0.587f * rgb[3 * i + 1] + 0.114f * rgb[3 * i + 2];
    int *gx = xalloc((size_t)w * h, 4), *gy = xalloc((size_t)w * h, 4);
#define G(x, y) gr[(size_t)((y) < 0 ? 0 : (y) >= h ? h - 1 : (y)) * w + ((x) < 0 ? 0 : (x) >= w ? w - 1 : (x))]
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float sx = (G(x + 1, y - 1) + 2 * G(x + 1, y) + G(x + 1, y + 1)) - (G(x - 1, y - 1) + 2 * G(x - 1, y) + G(x - 1, y + 1));
            const float sy = (G(x - 1, y + 1) + 2 * G(x, y + 1) + G(x + 1, y + 1)) - (G(x - 1, y - 1) + 2 * G(x, y - 1) + G(x + 1, y - 1));
            const size_t i = (size_t)y * w + x;
            gx[i] = (int)sx; gy[i] = (int)sy; mag[i] = abs((int)sx) + abs((int)sy);        /* norma L1, como o cv2.Canny padrao */
            const float ax = fabsf(sx), ay = fabsf(sy);
            dir[i] = ay <= ax * 0.41421356f ? 0 : ay >= ax * 2.41421356f ? 2 : ((sx > 0) == (sy > 0) ? 1 : 3);   /* 0 horizontal, 1 diagonal \, 2 vertical, 3 diagonal / */
        }
#undef G
    uint8_t *cls = xalloc((size_t)w * h, 1);      /* 0 nada, 1 candidato fraco, 2 forte */
    for (int y = 1; y < h - 1; y++)
        for (int x = 1; x < w - 1; x++) {
            const size_t i = (size_t)y * w + x; const int m = mag[i];
            if (m <= lo) continue;
            int a, b;
            switch (dir[i]) { case 0: a = mag[i - 1]; b = mag[i + 1]; break; case 2: a = mag[i - w]; b = mag[i + w]; break; case 1: a = mag[i - w - 1]; b = mag[i + w + 1]; break; default: a = mag[i - w + 1]; b = mag[i + w - 1]; }
            if (m > a && m >= b) cls[i] = m > hi ? 2 : 1;
        }
    size_t *stack = xalloc((size_t)w * h, sizeof(size_t)); size_t sp = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) if (cls[i] == 2) { out[i] = 1; stack[sp++] = i; }
    while (sp) {                                    /* histerese: o fraco vizinho (8) de um forte tambem e borda */
        const size_t i = stack[--sp]; const int x = (int)(i % w), y = (int)(i / w);
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            const int nx = x + dx, ny = y + dy; if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const size_t j = (size_t)ny * w + nx;
            if (cls[j] == 1 && !out[j]) { out[j] = 1; stack[sp++] = j; }
        }
    }
    free(gr); free(mag); free(dir); free(gx); free(gy); free(cls); free(stack);
    return out;
}

/* ---------- pipeline ---------- */
typedef struct {
    const char *prompt, *negative, *models, *out, *control, *init, *sdmodel;
    int steps, w, h, canny_lo, canny_hi, control_lines, max_chunks, lcm; double guidance, strength, control_scale; uint64_t seed;
} SdpParams;

static uint8_t *sdp_load_image(const char *path, int w, int h) {       /* arquivo -> RGB8 w x h (redimensionado) ou NULL */
    int64_t sz = 0; const uint8_t *b = map_file(path, &sz);
    if (!b) { fprintf(stderr, "render: nao consegui abrir %s\n", path); return NULL; }
    Img im; char err[200] = "";
    if (!img_decode(b, (size_t)sz, &im, 0, err, sizeof err)) { fprintf(stderr, "render: %s: %s\n", path, err); return NULL; }
    if ((im.w != w || im.h != h) && !vr_resize(&im, w, h)) { fprintf(stderr, "render: nao consegui redimensionar %s\n", path); return NULL; }
    return im.rgb;
}
static int sdp_peek_size(const char *path, int *w, int *h) {
    int64_t sz = 0; const uint8_t *b = map_file(path, &sz);
    Img im; char err[200] = "";
    if (!b || !img_decode(b, (size_t)sz, &im, 1, err, sizeof err)) return 0;
    *w = im.w; *h = im.h;
    return 1;
}
static int sdp_round64(double v) { int r = (int)(v / 64.0 + 0.5) * 64; return r < 64 ? 64 : r > 1024 ? 1024 : r; }

/* uma passada do modelo (ControlNet + UNet) para um ramo: GPU se ha, senao (ou se a GPU falha) CPU */
typedef struct { const SdgFile *base, *ctrl; float *ctrl_img, *cond; int gpu, W, H, h8, w8, nctx; double scale; } SdpPass;
static int sdp_eps(SdpPass *Q, const float *lat, float t, const float *ctx, float *eps) {
    if (Q->gpu) {
        if (scu_eps(Q->base, Q->ctrl, lat, Q->h8, Q->w8, t, ctx, Q->nctx, (float)Q->scale, eps)) return 1;
        Q->gpu = 0;
    }
    if (Q->ctrl && !Q->cond) {
        int hc, wc; Q->cond = sdn_cond_embed(Q->ctrl, Q->ctrl_img, Q->H, Q->W, &hc, &wc);
        if (!Q->cond || hc != Q->h8 || wc != Q->w8) { fprintf(stderr, "render: falhou o embedding da imagem de controle\n"); return 0; }
    }
    SdSkips res; float *mres = NULL; int hm, wm;
    if (Q->ctrl && !sdn_control(Q->ctrl, lat, Q->h8, Q->w8, t, ctx, Q->nctx, Q->cond, (float)Q->scale, &res, &mres, &hm, &wm)) { fprintf(stderr, "render: ControlNet falhou\n"); return 0; }
    if (!sdn_unet(Q->base, lat, Q->h8, Q->w8, t, ctx, Q->nctx, Q->ctrl ? &res : NULL, mres, eps)) { fprintf(stderr, "render: UNet falhou\n"); return 0; }
    if (Q->ctrl) { sdn_skips_free(&res); free(mres); }
    return 1;
}

/* Checkpoint do Stable: --sd-model <nome> (ou KATANA_SD_MODEL; a opcao vale mais) escolhe sd15-<nome>.gguf, ou sd15-<nome>-lcm.gguf com --lcm.
 * Padrao: realistic-vision. O nome so tem letras, digitos, '-' e '_' (e um nome de arquivo, nunca um caminho). Os modelos que existem
 * (sd15-*.gguf em models/render, sem os -lcm e o ControlNet) sao listados no erro. 0 = ok */
static int sdp_model_path(const SdpParams *P, char *out, size_t cap) {
    const char *nm = P->sdmodel && *P->sdmodel ? P->sdmodel : getenv("KATANA_SD_MODEL");
    if (!nm || !*nm) nm = "realistic-vision";
    for (const char *c = nm; *c; c++) if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') { fprintf(stderr, "render: nome de modelo invalido: %s (so letras, digitos, - e _)\n", nm); return 1; }
    snprintf(out, cap, "%s/sd15-%s%s.gguf", P->models, nm, P->lcm ? "-lcm" : "");
    if (access(out, R_OK) == 0) return 0;
    fprintf(stderr, "render: o modelo \"%s\"%s nao existe (%s).", nm, P->lcm ? " na variante LCM" : "", out);
    DIR *d = opendir(P->models);
    if (d) {
        fprintf(stderr, " Disponiveis:");
        for (struct dirent *e; (e = readdir(d)) != NULL; ) {
            const size_t l = strlen(e->d_name);
            if (l > 9 && !strncmp(e->d_name, "sd15-", 5) && !strcmp(e->d_name + l - 5, ".gguf") && !strstr(e->d_name, "-lcm.") && !strstr(e->d_name, "controlnet")) fprintf(stderr, " %.*s", (int)(l - 10), e->d_name + 5);
        }
        closedir(d);
    }
    fprintf(stderr, "\n");
    return 1;
}

static int sdp_generate(const SdpParams *P) {
    int W = P->w, H = P->h; char path[1100];
    if ((!W || !H) && (P->control || P->init)) {                         /* tamanho pela proporcao da imagem, lado maior 512 */
        int iw, ih; const char *src = P->init ? P->init : P->control;
        if (!sdp_peek_size(src, &iw, &ih)) { fprintf(stderr, "render: nao consegui ler o tamanho de %s\n", src); return 1; }
        const double s = 512.0 / (iw > ih ? iw : ih);
        W = sdp_round64(iw * s); H = sdp_round64(ih * s);
    }
    if (!W || !H) { W = 512; H = 512; }
    if (W % 64 || H % 64) { fprintf(stderr, "render: o tamanho tem de ter lados multiplos de 64 (veio %dx%d)\n", W, H); return 1; }
    SdgFile base, ctrl; SdClip clip; int use_ctrl = P->control != NULL;
    if (sdp_model_path(P, path, sizeof path)) return 1;
    if (!sdg_open(&base, path)) return 1;
    if (use_ctrl) { snprintf(path, sizeof path, "%s/sd15-controlnet-canny.gguf", P->models); if (!sdg_open(&ctrl, path)) return 1; }
    if (!sdc_load(&clip, &base)) return 1;
    const double t0 = sdp_now();
    /* texto: prompt e negativo em blocos de 75 ids (sdc_encode_long); os dois com o mesmo numero de blocos (o mais curto ganha blocos vazios) */
    int ncp = 0, ncn = 0;
    float *cc = sdc_encode_long(&clip, P->prompt, P->max_chunks, &ncp), *cu = sdc_encode_long(&clip, P->negative ? P->negative : "", P->max_chunks, &ncn);
    if (!cc || !cu) { fprintf(stderr, "render: o codificador de texto falhou\n"); return 1; }
    const int nch = P->lcm ? ncp : (ncp > ncn ? ncp : ncn), nctx = nch * 77;      /* LCM: um passe so, sem o negativo */
    if (!P->lcm && (ncp < nch || ncn < nch)) {
        int one = 0; float *empty = sdc_encode_long(&clip, "", 1, &one);
        float **sh = ncp < nch ? &cc : &cu; const int have = ncp < nch ? ncp : ncn;
        float *grown = xalloc((size_t)nch * 77 * 768, sizeof(float));
        memcpy(grown, *sh, (size_t)have * 77 * 768 * 4);
        for (int i = have; i < nch; i++) memcpy(grown + (size_t)i * 77 * 768, empty, (size_t)77 * 768 * 4);
        *sh = grown; free(empty);
    }
    fprintf(stderr, "[render] prompt: %d bloco(s) de 75 tokens, negativo: %d; contexto de %d posicoes\n", ncp, ncn, nctx);
    const int h8 = H / 8, w8 = W / 8;
    SdpEuler eu; sdp_euler_setup(&eu, P->steps, 1000, sdg_kv_f32(&base, "sd15.sched.beta_start", 0.00085f), sdg_kv_f32(&base, "sd15.sched.beta_end", 0.012f));
    SdpLcm lc; if (P->lcm) sdp_lcm_setup(&lc, P->steps, sdg_kv_f32(&base, "sd15.sched.beta_start", 0.00085f), sdg_kv_f32(&base, "sd15.sched.beta_end", 0.012f));
    const float vscale = sdg_kv_f32(&base, "sd15.vae.scaling_factor", 0.18215f);
    /* imagem de controle (bordas) */
    float *cond = NULL, *ctrl_img = NULL;
    int gpu = scu_plan(&base, use_ctrl ? &ctrl : NULL, h8, w8, nctx), cond_gpu = 0;
    if (use_ctrl) {
        uint8_t *rgb = sdp_load_image(P->control, W, H);
        if (!rgb) return 1;
        uint8_t *ed;
        if (P->control_lines) {      /* a imagem de controle ja e um desenho linear (linhas escuras em fundo claro, ou o contrario): vira o mapa de bordas direto */
            double mean = 0; ed = xalloc((size_t)W * H, 1);
            for (size_t i = 0; i < (size_t)W * H; i++) mean += 0.299 * rgb[3 * i] + 0.587 * rgb[3 * i + 1] + 0.114 * rgb[3 * i + 2];
            mean /= (double)W * H;
            for (size_t i = 0; i < (size_t)W * H; i++) { const double gv = 0.299 * rgb[3 * i] + 0.587 * rgb[3 * i + 1] + 0.114 * rgb[3 * i + 2]; ed[i] = (mean > 127.0 ? 255.0 - gv : gv) > 96.0; }
        } else ed = sdp_canny(rgb, W, H, P->canny_lo, P->canny_hi);
        float *img = sdn_alloc((size_t)W * H * 3); size_t on = 0;
        for (size_t i = 0; i < (size_t)W * H; i++) { const float v = ed[i] ? 1.0f : 0.0f; img[3 * i] = img[3 * i + 1] = img[3 * i + 2] = v; on += ed[i]; }
        fprintf(stderr, "[render] bordas: %.1f%% dos pixels\n", 100.0 * (double)on / ((double)W * H));
        ctrl_img = img;
        free(ed); free(rgb);
    }
    /* latente inicial */
    SdpRng rng = { P->seed * 0x9E3779B97F4A7C15ull + 12345, 0, 0 };
    float *x = sdn_alloc((size_t)h8 * w8 * 4); int start = 0;
    for (size_t i = 0; i < (size_t)h8 * w8 * 4; i++) x[i] = sdp_rng_gauss(&rng);
    if (P->init) {
        uint8_t *rgb = sdp_load_image(P->init, W, H);
        if (!rgb) return 1;
        float *im = sdn_alloc((size_t)W * H * 3);
        for (size_t i = 0; i < (size_t)W * H * 3; i++) im[i] = rgb[i] / 127.5f - 1.0f;
        int hh, ww; float *m = gpu ? scu_vae_encode(&base, im, H, W, &hh, &ww) : NULL;
        if (!m) m = sdv_encode(&base, im, H, W, &hh, &ww);
        if (!m) return 1;
        start = P->steps - (int)(P->steps * P->strength); if (start < 0) start = 0; if (start > P->steps - 1) start = P->steps - 1;
        if (P->lcm) { const double a = lc.ac[lc.t[start]]; for (size_t i = 0; i < (size_t)h8 * w8 * 4; i++) x[i] = (float)(sqrt(a) * m[i] * vscale + sqrt(1.0 - a) * x[i]); }
        else for (size_t i = 0; i < (size_t)h8 * w8 * 4; i++) x[i] = m[i] * vscale + x[i] * (float)eu.sigma[start];
        free(m); free(im); free(rgb);
    } else if (!P->lcm) for (size_t i = 0; i < (size_t)h8 * w8 * 4; i++) x[i] *= (float)eu.init_sigma;
    SdpPass Q = { &base, use_ctrl ? &ctrl : NULL, ctrl_img, cond, gpu, W, H, h8, w8, nctx, P->control_scale };
    if (use_ctrl && gpu && scu_cond(&ctrl, ctrl_img, H, W, nctx)) cond_gpu = 1;
    if (use_ctrl && !cond_gpu) {
        int hc, wc; Q.cond = sdn_cond_embed(&ctrl, ctrl_img, H, W, &hc, &wc);
        if (!Q.cond || hc != h8 || wc != w8) { fprintf(stderr, "render: falhou o embedding da imagem de controle\n"); return 1; }
    }
    fprintf(stderr, "[render] %dx%d, %d passos (comecando no %d), guia %.1f, semente %llu%s%s\n", W, H, P->steps, start, P->guidance, (unsigned long long)P->seed, use_ctrl ? ", ControlNet" : "", P->init ? ", imagem a imagem" : "");
    float *xin = sdn_alloc((size_t)h8 * w8 * 4), *e_c = sdn_alloc((size_t)h8 * w8 * 4), *e_u = sdn_alloc((size_t)h8 * w8 * 4);
    if (P->lcm) {
        float *den = sdn_alloc((size_t)h8 * w8 * 4);
        for (int s = start; s < P->steps; s++) {
            const double ts = sdp_now(); const int t = lc.t[s], last = s == P->steps - 1;
            if (!sdp_eps(&Q, x, (float)t, cc, e_c)) return 1;
            double cs, co; sdp_lcm_scalings(t, &cs, &co);
            const double at = lc.ac[t], sat = sqrt(at), sbt = sqrt(1.0 - at), ap = lc.ac[last ? t : lc.t[s + 1]];
            for (size_t i = 0; i < (size_t)h8 * w8 * 4; i++) {
                const double x0 = (x[i] - sbt * e_c[i]) / sat, d = co * x0 + cs * x[i];
                den[i] = (float)d;
                x[i] = last ? (float)d : (float)(sqrt(ap) * d + sqrt(1.0 - ap) * sdp_rng_gauss(&rng));
            }
            fprintf(stderr, "[render] passo LCM %d/%d  (t %d)  %.1f s\n", s + 1, P->steps, t, sdp_now() - ts);
        }
    } else for (int s = start; s < P->steps; s++) {
        const double ts = sdp_now();
        const float sg = (float)eu.sigma[s], inv = 1.0f / sqrtf(sg * sg + 1.0f), t = (float)eu.t[s];
        for (size_t i = 0; i < (size_t)h8 * w8 * 4; i++) xin[i] = x[i] * inv;
        for (int branch = 0; branch < 2; branch++) {                    /* 0 = com o prompt, 1 = sem (negativo) */
            const float *ctx = branch ? cu : cc; float *e = branch ? e_u : e_c;
            if (!sdp_eps(&Q, xin, t, ctx, e)) return 1;
        }
        const float dt = (float)(eu.sigma[s + 1] - eu.sigma[s]);
        for (size_t i = 0; i < (size_t)h8 * w8 * 4; i++) { const float eps = e_u[i] + (float)P->guidance * (e_c[i] - e_u[i]); x[i] += eps * dt; }
        fprintf(stderr, "[render] passo %d/%d  (sigma %.3f)  %.1f s\n", s + 1, P->steps, sg, sdp_now() - ts);
    }
    for (size_t i = 0; i < (size_t)h8 * w8 * 4; i++) x[i] /= vscale;
    int Ho, Wo; float *img = gpu ? scu_vae_decode(&base, x, h8, w8, &Ho, &Wo) : NULL;
    if (!img) img = sdv_decode(&base, x, h8, w8, &Ho, &Wo);
    if (!img) { fprintf(stderr, "render: VAE falhou\n"); return 1; }
    uint8_t *px = xalloc((size_t)Wo * Ho * 3, 1);
    for (size_t i = 0; i < (size_t)Wo * Ho * 3; i++) { float v = (img[i] * 0.5f + 0.5f) * 255.0f; v = v < 0 ? 0 : v > 255 ? 255 : v; px[i] = (uint8_t)(v + 0.5f); }
    if (!sdp_png_write(P->out, px, Wo, Ho)) { fprintf(stderr, "render: nao consegui gravar %s\n", P->out); return 1; }
    { static const char *nmb[3] = { "cpu", "cuda", "vulkan" }; fprintf(stderr, "[stable] usado: unet=%s vae=%s\n", nmb[g_sd_be_unet], nmb[g_sd_be_vae]); }
    fprintf(stderr, "[render] pronto: %s (%dx%d) em %.0f s\n", P->out, Wo, Ho, sdp_now() - t0);
    return 0;
}

static int sds_fill(SdpParams *P, const char *nl);
static void sds_nl_prose(const char *nl, int has_image, char *out, size_t cap);   /* render/sdspec.c */
static int fxg_generate(const char *prompt, const char *ref1, const char *ref2, int w, int h, int steps, uint64_t seed, const char *out, const char *lora, float lora_scale);   /* render/fxgen.c */
struct SdsRep;                                                                              /* render/sdsel.c */
static char *sdb_flux_prompt(const char *text, const char *extra, struct SdsRep *rep);          /* render/sdbrief.c */
static void sds_report(const struct SdsRep *r, const char *engine, FILE *f);
static char *sdb_read_text(const char *path);
static int sdb_fill(SdpParams *P, const char *path, const char *extra);
static int sdb_explain_on;                                                    /* --explain-brief: o que entrou e o que foi cortado, por unidade (render/sdbrief.c) */
static int sdl_run(const char *image, const char *logo_path, const char *box, const char *persp, int blend, const char *out);   /* render/sdlogo.c */

/* depois de gerar: o logo de verdade por cima. O render sem logo fica ao lado, com "-nologo" antes da extensao (a caixa errada se refaz sem gerar de novo) */
static int fxr_exists(const char *p);                                                  /* render/fxtrain.c */

static int sdp_post_logo(const char *out, const char *logo, const char *box, const char *persp, int blend) {
    char raw[1200]; snprintf(raw, sizeof raw, "%s", out);
    char *dot = strrchr(raw, '.'); if (dot && !strchr(dot, '/')) *dot = 0;
    strncat(raw, "-nologo.png", sizeof raw - strlen(raw) - 1);
    if (rename(out, raw) != 0) { fprintf(stderr, "render: nao consegui guardar o render sem logo em %s\n", raw); return 1; }
    return sdl_run(raw, logo, box, persp, blend, out);
}

static int sdp_main(int argc, char **argv) {
    SdpParams P; memset(&P, 0, sizeof P); int steps_set = 0; const char *engine = NULL, *nl = NULL, *init2 = NULL, *brief = NULL, *logo = NULL, *logo_box = NULL, *logo_persp = NULL; int logo_blend = 0; const char *lora = NULL; double lora_scale = 1.0;
    P.steps = 20; P.guidance = 7.5; P.strength = 0.75; P.control_scale = 1.0; P.canny_lo = 100; P.canny_hi = 200; P.negative = ""; P.max_chunks = 4;
    static char models[1100]; snprintf(models, sizeof models, "%s/render", models_dir()); P.models = models;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i]; const int more = i + 1 < argc;
        if (!strcmp(a, "--prompt") && more) P.prompt = argv[++i];
        else if (!strcmp(a, "--negative") && more) P.negative = argv[++i];
        else if ((!strcmp(a, "--prompt-file") || !strcmp(a, "--negative-file")) && more) {
            int64_t sz = 0; const uint8_t *fb = map_file(argv[i + 1], &sz);
            if (!fb) { fprintf(stderr, "render: nao consegui abrir %s\n", argv[i + 1]); return 1; }
            char *txt = xalloc((size_t)sz + 1, 1); memcpy(txt, fb, (size_t)sz);
            if (!strcmp(a, "--prompt-file")) P.prompt = txt; else P.negative = txt;
            i++;
        }
        else if (!strcmp(a, "--chunks") && more) P.max_chunks = atoi(argv[++i]);
        else if (!strcmp(a, "--lcm")) P.lcm = 1;
        else if (!strcmp(a, "--sd-model") && more) P.sdmodel = argv[++i];
        else if (!strcmp(a, "--steps") && more) { P.steps = atoi(argv[++i]); steps_set = 1; }
        else if (!strcmp(a, "--guidance") && more) P.guidance = atof(argv[++i]);
        else if (!strcmp(a, "--seed") && more) P.seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--size") && more) { if (sscanf(argv[++i], "%dx%d", &P.w, &P.h) != 2) { fprintf(stderr, "render: --size espera LARGURAxALTURA\n"); return 1; } }
        else if (!strcmp(a, "--control") && more) P.control = argv[++i];
        else if (!strcmp(a, "--control-scale") && more) P.control_scale = atof(argv[++i]);
        else if (!strcmp(a, "--canny") && i + 2 < argc) { P.canny_lo = atoi(argv[++i]); P.canny_hi = atoi(argv[++i]); }
        else if (!strcmp(a, "--control-lines")) P.control_lines = 1;
        else if (!strcmp(a, "--engine") && more) engine = argv[++i];
        else if (!strcmp(a, "--nl") && more) nl = argv[++i];
        else if (!strcmp(a, "--init") && more) P.init = argv[++i];
        else if (!strcmp(a, "--init2") && more) init2 = argv[++i];
        else if (!strcmp(a, "--brief") && more) brief = argv[++i];
        else if (!strcmp(a, "--explain-brief")) sdb_explain_on = 1;
        else if (!strcmp(a, "--logo") && more) logo = argv[++i];
        else if (!strcmp(a, "--logo-box") && more) logo_box = argv[++i];
        else if (!strcmp(a, "--logo-perspective") && more) logo_persp = argv[++i];
        else if (!strcmp(a, "--logo-blend")) logo_blend = 1;
        else if (!strcmp(a, "--lora") && more) lora = argv[++i];
        else if (!strcmp(a, "--lora-scale") && more) lora_scale = atof(argv[++i]);
        else if (!strcmp(a, "--strength") && more) P.strength = atof(argv[++i]);
        else if (!strcmp(a, "--models") && more) P.models = argv[++i];
        else if (!strcmp(a, "--out") && more) P.out = argv[++i];
        else { fprintf(stderr, "render: opcao desconhecida ou sem valor: %s\n", a); return 1; }
    }
    if (logo && !logo_box && !logo_persp) { fprintf(stderr, "render: --logo exige --logo-box x,y,w,h (ou --logo-perspective)\n"); return 1; }
    if ((logo_box || logo_persp) && !logo) { fprintf(stderr, "render: --logo-box/--logo-perspective exigem --logo arquivo.png\n"); return 1; }
    if (engine && !strcmp(engine, "flux")) {                                 /* FLUX.2 klein: prosa, sem negativo, referencias como fichas (fxgen.c) */
        char prose[6000]; const char *pr = P.prompt;
        if (brief) {                                                         /* o briefing E o prompt, com as palavras do autor; so se passa do orcamento o excesso e cortado (render/sdbrief.c) */
            char *bt = sdb_read_text(brief); if (!bt) return 1;
            struct SdsRep rep; pr = sdb_flux_prompt(bt, nl, &rep);
            fprintf(stderr, "[brief] prompt do FLUX: %s\n", pr);
            sds_report(&rep, "flux", stderr);
        } else if (nl) { sds_nl_prose(nl, P.init != NULL, prose, sizeof prose); pr = prose; }
        if (!pr || !P.out) { fprintf(stderr, "uso: katana render generate --engine flux (--brief briefing.md [--nl \"pedido adicional\"] | --nl \"pedido\" | --prompt \"...\" | --prompt-file arq) --out saida.png [--init ref.png [--init2 ref2.png]] [--size WxH (multiplos de 16)] [--steps 4] [--seed 0] [--lora arq.fxlora [--lora-scale 1.0]]\n       [--logo logo.png --logo-box x,y,w,h [--logo-perspective x1,y1,..,x4,y4] [--logo-blend]]\n"); return 1; }
        if (init2 && !P.init) { fprintf(stderr, "render: --init2 exige --init\n"); return 1; }
        if (lora && !fxr_exists(lora)) { fprintf(stderr, "render: --lora: nao achei %s\n", lora); return 1; }
        if (!fxg_generate(pr, P.init, init2, P.w, P.h, steps_set ? P.steps : 4, P.seed, P.out, lora, (float)lora_scale)) return 2;
        return logo ? sdp_post_logo(P.out, logo, logo_box, logo_persp, logo_blend) : 0;
    }
    if (brief && P.out) { if (!sdb_fill(&P, brief, nl)) return 1; }
    else if (nl && P.out && !sds_fill(&P, nl)) return 1;
    if (!P.prompt || !P.out) { fprintf(stderr, "uso: katana render generate --prompt \"...\" (ou --prompt-file arq) --out saida.png [--chunks 4] [--lcm: LCM, 4 passos, 1 passe] [--sd-model nome (ou KATANA_SD_MODEL): realistic-vision (padrao), interior] [--negative \"...\"] [--steps 20] [--guidance 7.5] [--seed 0]\n"
                                              "       [--size WxH (multiplos de 64)] [--control img [--control-lines] [--control-scale 1.0] [--canny BAIXO ALTO]] [--init img [--strength 0.75]] [--models pasta]\n"); return 1; }
    if (engine && strcmp(engine, "stable")) {                                /* motores em preparacao (WIP): contrato ja fixado para o desktop */
        if (!strcmp(engine, "flux") || !strcmp(engine, "krea")) { fprintf(stderr, "render: o motor %s esta em desenvolvimento (WIP), ainda nao disponivel\n", engine); return 3; }
        fprintf(stderr, "render: motor desconhecido: %s (stable, flux ou krea)\n", engine); return 1;
    }
    if (P.lcm && !steps_set) P.steps = 4;
    if (P.steps < 1 || P.steps > 200) { fprintf(stderr, "render: --steps entre 1 e 200\n"); return 1; }
    if (P.strength <= 0 || P.strength > 1) { fprintf(stderr, "render: --strength entre 0 (exclusive) e 1\n"); return 1; }
    { const int rc = sdp_generate(&P); if (rc || !logo) return rc; }
    return sdp_post_logo(P.out, logo, logo_box, logo_persp, logo_blend);
}
