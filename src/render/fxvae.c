/* ============================ render: VAE do FLUX.2 klein na CPU ===============================
 * katana render flux-vae convert <pasta-do-vae> <saida.gguf>        (render/fxconvert_vae.c)
 * katana render flux-vae selftest <pasta-de-referencia> <vae.gguf>  contra os tensores float32 do diffusers (docs/render.md, etapa F4)
 *
 * O mesmo desenho do VAE do Stable Diffusion (render/sdvae.c: resnets, atencao de uma cabeca no meio, descida 0,1,0,1), com:
 *   - 32 canais de latente (codificador: conv_out 512 -> 64, quant_conv 64 -> 64, os 32 primeiros sao a media; decodificador:
 *     post_quant_conv 32 -> 32, conv_in 32 -> 512), blocos de canais 128, 256, 512, 512, fator 8;
 *   - patchify 2x2: o latente [H/8][W/8][32] vira [H/16 * W/16][128] fichas, canal = c*4 + a*2 + b (a = linha, b = coluna dentro do
 *     bloco 2x2), ficha = linha * (W/16) + coluna;
 *   - normalizacao por estatisticas de lote (BatchNorm sem ganho nem vies, modo de inferencia): o codificador devolve
 *     (x - media) / sqrt(variancia + eps) e o decodificador desfaz (x * sqrt(variancia + eps) + media), eps = 1e-4, 128 estatisticas.
 * Nao ha fator de escala escalar (o do Stable Diffusion): a normalizacao por canal faz esse papel. Imagem em [-1, 1], NHWC.
 * Funcoes publicas: fxv_load, fxv_encode (imagem -> fichas normalizadas), fxv_decode (fichas normalizadas -> imagem). Altura e largura da
 * imagem multiplos de 16. */

typedef struct { SdgFile g; int latc; float bn_mean[128], bn_std[128]; } FxVae;

static int fxv_load(FxVae *v, const char *path) {
    if (!sdg_open(&v->g, path)) return 0;
    v->latc = (int)sdg_kv_u32(&v->g, "flux2vae.latent_channels", 32);
    if (v->latc != 32) { fprintf(stderr, "render: VAE com %d canais de latente (esperado 32)\n", v->latc); return 0; }
    const float eps = sdg_kv_f32(&v->g, "flux2vae.batch_norm_eps", 1e-4f);
    uint64_t n = 0;
    float *m = sdg_floats(&v->g, "vae.bn.running_mean", &n), *var = sdg_floats(&v->g, "vae.bn.running_var", &n);
    if (!m || !var || n != 128) { fprintf(stderr, "render: estatisticas de lote do VAE ausentes ou com tamanho errado\n"); return 0; }
    for (int i = 0; i < 128; i++) { v->bn_mean[i] = m[i]; v->bn_std[i] = (float)sqrt((double)var[i] + (double)eps); }
    free(m); free(var);
    return 1;
}

/* um resnet no lugar de *x ([H*W][canais de entrada]); *C recebe os canais de saida */
static int fxv_res(const SdgFile *g, const char *pfx, float **x, int H, int W, int *C) {
    char b[220]; int cin, cout;
    snprintf(b, sizeof b, "%s.conv2", pfx);
    if (!sdn_conv_cin_cout(g, b, &cin, &cout)) return 0;
    float *y = sdn_alloc((size_t)H * W * cout); int Co = 0;
    const int ok = sdn_resnet(g, pfx, *x, y, H, W, &Co, NULL);
    free(*x); *x = y; *C = Co;
    return ok;
}

/* fichas normalizadas [th*tw][128] -> latente [2th*2tw][32] (memoria nova), desfazendo a normalizacao e o patchify */
static float *fxv_unpatch(const FxVae *v, const float *tok, int th, int tw) {
    const int W8 = 2 * tw;
    float *z = sdn_alloc((size_t)4 * th * tw * 32);
    for (int i = 0; i < th; i++)
        for (int j = 0; j < tw; j++) {
            const float *t = tok + ((size_t)i * tw + j) * 128;
            for (int c = 0; c < 32; c++)
                for (int a = 0; a < 2; a++)
                    for (int b = 0; b < 2; b++) {
                        const int ch = c * 4 + a * 2 + b;
                        z[((size_t)(2 * i + a) * W8 + (2 * j + b)) * 32 + c] = t[ch] * v->bn_std[ch] + v->bn_mean[ch];
                    }
        }
    return z;
}

/* fichas normalizadas [th*tw][128] -> imagem [8*2th * 8*2tw][3] em [-1, 1] (memoria nova) ou NULL */
static float *fxv_decode(const FxVae *v, const float *tok, int th, int tw, int *Ho, int *Wo) {
    const SdgFile *g = &v->g;
    int H = 2 * th, W = 2 * tw, C = 512, ok = 1; char b[220];
    float *z = fxv_unpatch(v, tok, th, tw);
    { float *r = fxq_decode(g, z, H, W, Ho, Wo); if (r) { free(z); return r; } }     /* GPU (fxvaegpu.c); NULL = CPU */
    float *x = sdn_alloc((size_t)H * W * 32);
    ok &= sdn_conv(x, z, H, W, g, "vae.post_quant_conv", 1, 0, NULL, NULL);
    free(z);
    float *y = sdn_alloc((size_t)H * W * 512);
    if (ok) ok &= sdn_conv(y, x, H, W, g, "vae.decoder.conv_in", 1, 1, NULL, NULL);
    free(x); x = y;
    if (ok) ok &= sdv_mid(g, "vae.decoder.mid_block", &x, H, W, C);
    for (int blk = 0; blk < 4 && ok; blk++) {
        for (int l = 0; l < 3 && ok; l++) {
            snprintf(b, sizeof b, "vae.decoder.up_blocks.%d.resnets.%d", blk, l);
            ok &= fxv_res(g, b, &x, H, W, &C);
            if (getenv("FXQ_DBG")) { double sm = 0, a = 0; for (size_t i = 0; i < (size_t)H * W * C; i++) { sm += x[i]; a += fabs(x[i]); } fprintf(stderr, "  dbgcpu resnet n=%zu mean %.5f meanabs %.5f\n", (size_t)H * W * C, sm / ((size_t)H * W * C), a / ((size_t)H * W * C)); }
        }
        if (ok && blk < 3) {
            float *u = sdn_alloc((size_t)4 * H * W * C);
            sdr_upsample2x(u, x, H, W, C);
            y = sdn_alloc((size_t)4 * H * W * C);
            snprintf(b, sizeof b, "vae.decoder.up_blocks.%d.upsamplers.0.conv", blk);
            ok &= sdn_conv(y, u, 2 * H, 2 * W, g, b, 1, 1, NULL, NULL);
            free(u); free(x); x = y; H *= 2; W *= 2;
        }
    }
    float *img = NULL;
    if (ok) {
        y = sdn_alloc((size_t)H * W * C);
        ok &= sdn_gn(y, x, H * W, C, g, "vae.decoder.conv_norm_out", 1e-6f, 1);
        img = sdn_alloc((size_t)H * W * 3);
        if (ok) ok &= sdn_conv(img, y, H, W, g, "vae.decoder.conv_out", 1, 1, NULL, NULL);
        free(y);
    }
    free(x);
    if (!ok) { free(img); return NULL; }
    *Ho = H; *Wo = W;
    return img;
}

/* imagem [H0*W0][3] em [-1, 1] -> media do latente [H0/8 * W0/8][32] (memoria nova) ou NULL; antes do patchify e da normalizacao */
static float *fxv_encode_mean(const FxVae *v, const float *img, int H0, int W0, int *ho, int *wo) {
    const SdgFile *g = &v->g;
    { float *r = fxq_encode(g, img, H0, W0, ho, wo); if (r) return r; }              /* GPU (fxvaegpu.c); NULL = CPU */
    int H = H0, W = W0, C = 128, ok = 1; char b[220];
    float *x = sdn_alloc((size_t)H * W * C);
    ok &= sdn_conv(x, img, H, W, g, "vae.encoder.conv_in", 1, 1, NULL, NULL);
    for (int blk = 0; blk < 4 && ok; blk++) {
        for (int l = 0; l < 2 && ok; l++) {
            snprintf(b, sizeof b, "vae.encoder.down_blocks.%d.resnets.%d", blk, l);
            ok &= fxv_res(g, b, &x, H, W, &C);
        }
        if (ok && blk < 3) {                                              /* descida: preenchimento 0,1,0,1 (em cima, esquerda, embaixo, direita) */
            SdrConv c; int Hn, Wn;
            snprintf(b, sizeof b, "vae.encoder.down_blocks.%d.downsamplers.0.conv", blk);
            if (!sdr_conv_load(g, b, &c)) { ok = 0; break; }
            float *y = sdn_alloc((size_t)((H + 1 - 3) / 2 + 1) * ((W + 1 - 3) / 2 + 1) * c.cout);
            sdr_conv2d(y, x, H, W, &c, 2, 0, 0, 1, 1, &Hn, &Wn);
            sdr_conv_free(&c);
            free(x); x = y; H = Hn; W = Wn;
        }
    }
    if (ok) ok &= sdv_mid(g, "vae.encoder.mid_block", &x, H, W, C);
    float *mean = NULL;
    if (ok) {
        float *t = sdn_alloc((size_t)H * W * C), *m64 = sdn_alloc((size_t)H * W * 64), *q64 = sdn_alloc((size_t)H * W * 64);
        ok &= sdn_gn(t, x, H * W, C, g, "vae.encoder.conv_norm_out", 1e-6f, 1);
        if (ok) ok &= sdn_conv(m64, t, H, W, g, "vae.encoder.conv_out", 1, 1, NULL, NULL);
        if (ok) ok &= sdn_conv(q64, m64, H, W, g, "vae.quant_conv", 1, 0, NULL, NULL);
        if (ok) { mean = sdn_alloc((size_t)H * W * 32); for (int i = 0; i < H * W; i++) memcpy(mean + (size_t)i * 32, q64 + (size_t)i * 64, 32 * 4); }
        free(t); free(m64); free(q64);
    }
    free(x);
    if (!ok) { free(mean); return NULL; }
    *ho = H; *wo = W;
    return mean;
}

/* imagem [H0*W0][3] em [-1, 1] (multiplos de 16) -> fichas normalizadas [th*tw][128] (memoria nova) ou NULL; *th = H0/16, *tw = W0/16 */
static float *fxv_encode(const FxVae *v, const float *img, int H0, int W0, int *th, int *tw) {
    if (H0 % 16 || W0 % 16 || H0 < 16 || W0 < 16) { fprintf(stderr, "render: imagem %dx%d: os lados devem ser multiplos de 16\n", W0, H0); return NULL; }
    int h8, w8;
    float *mean = fxv_encode_mean(v, img, H0, W0, &h8, &w8);
    if (!mean) return NULL;
    const int h = h8 / 2, w = w8 / 2;
    float *tok = sdn_alloc((size_t)h * w * 128);
    for (int i = 0; i < h; i++)
        for (int j = 0; j < w; j++)
            for (int c = 0; c < 32; c++)
                for (int a = 0; a < 2; a++)
                    for (int b = 0; b < 2; b++) {
                        const int ch = c * 4 + a * 2 + b;
                        tok[((size_t)i * w + j) * 128 + ch] = (mean[((size_t)(2 * i + a) * w8 + (2 * j + b)) * 32 + c] - v->bn_mean[ch]) / v->bn_std[ch];
                    }
    free(mean);
    *th = h; *tw = w;
    return tok;
}

/* ---------- selftest ---------- */
/* a: meu, b: referencia (mesma ordem); passa se max|a-b| / max|b| < 1e-3 e media|a-b| / media|b| < 1e-4 */
static int fxv_cmp(const char *what, const float *a, const float *b, size_t n) {
    double mx = 0, mr = 0, sd = 0, sr = 0;
    for (size_t i = 0; i < n; i++) {
        const double d = fabs((double)a[i] - b[i]);
        if (!(d <= mx)) mx = d;                                         /* NaN conta como erro */
        if (fabs(b[i]) > mr) mr = fabs(b[i]);
        sd += d; sr += fabs(b[i]);
    }
    const double r1 = mr > 0 ? mx / mr : mx, r2 = sr > 0 ? sd / sr : sd;
    const int ok = r1 < 1e-3 && r2 < 1e-4;
    printf("  %-38s %s  erro max %.3e  |ref| max %.3f  razao max %.2e  razao media %.2e\n", what, ok ? "OK    " : "FALHOU", mx, mr, r1, r2);
    return ok ? 0 : 1;
}

static int fxv_selftest(const char *refdir, const char *gguf) {
    FxVae v; int bad = 0;
    if (!fxv_load(&v, gguf)) return 1;
    printf("katana render flux-vae selftest (CPU, float32, %d threads)\n", pool.n);
    const int H = 192, W = 320, th = H / 16, tw = W / 16;
    float *img = sdt_ref(refdir, "vae_img", NULL), *rmean = sdt_ref(refdir, "vae_enc_mean", NULL), *rpack = sdt_ref(refdir, "vae_enc_packed", NULL);
    float *rdec = sdt_ref(refdir, "vae_dec_from_enc", NULL), *zin = sdt_ref(refdir, "vae_dec_in_packed", NULL), *rrand = sdt_ref(refdir, "vae_dec_rand", NULL);
    if (!img || !rmean || !rpack || !rdec || !zin || !rrand) return 1;
    float *x = sdt_to_nhwc(img, 3, H * W);
    int h8, w8, t2, t3;
    float *mean = fxv_encode_mean(&v, x, H, W, &h8, &w8);
    if (!mean || h8 != H / 8 || w8 != W / 8) { printf("  codificador falhou\n"); return 1; }
    float *mean_nchw = xalloc((size_t)32 * h8 * w8, sizeof(float));                 /* para comparar na ordem da referencia [C][H][W] */
    for (int c = 0; c < 32; c++) for (int p = 0; p < h8 * w8; p++) mean_nchw[(size_t)c * h8 * w8 + p] = mean[(size_t)p * 32 + c];
    bad += fxv_cmp("codificador: media do latente [32,24,40]", mean_nchw, rmean, (size_t)32 * h8 * w8);
    float *tok = fxv_encode(&v, x, H, W, &t2, &t3);
    if (!tok || t2 != th || t3 != tw) { printf("  fxv_encode falhou\n"); return 1; }
    bad += fxv_cmp("patchify + normalizacao [240,128]", tok, rpack, (size_t)th * tw * 128);
    int Ho, Wo;
    float *d1 = fxv_decode(&v, rpack, th, tw, &Ho, &Wo);                             /* decodifica o latente DA REFERENCIA: isola o decodificador */
    if (!d1 || Ho != H || Wo != W) { printf("  decodificador falhou\n"); return 1; }
    float *d1n = xalloc((size_t)3 * H * W, sizeof(float));
    for (int c = 0; c < 3; c++) for (int p = 0; p < H * W; p++) d1n[(size_t)c * H * W + p] = d1[(size_t)p * 3 + c];
    bad += fxv_cmp("decodificador: do latente da referencia", d1n, rdec, (size_t)3 * H * W);
    float *d2 = fxv_decode(&v, zin, th, tw, &Ho, &Wo);
    float *d2n = xalloc((size_t)3 * H * W, sizeof(float));
    for (int c = 0; c < 3; c++) for (int p = 0; p < H * W; p++) d2n[(size_t)c * H * W + p] = d2[(size_t)p * 3 + c];
    bad += fxv_cmp("decodificador: latente aleatorio", d2n, rrand, (size_t)3 * H * W);
    float *d3 = fxv_decode(&v, tok, th, tw, &Ho, &Wo);                               /* ida e volta com o meu codificador */
    float *d3n = xalloc((size_t)3 * H * W, sizeof(float));
    for (int c = 0; c < 3; c++) for (int p = 0; p < H * W; p++) d3n[(size_t)c * H * W + p] = d3[(size_t)p * 3 + c];
    bad += fxv_cmp("ida e volta (meu codificador e decodificador)", d3n, rdec, (size_t)3 * H * W);
    printf("%s\n", bad ? "RESULTADO: FALHOU" : "RESULTADO: todos passaram");
    return bad ? 1 : 0;
}

/* gpu-test <vae.gguf>: convolucao e GroupNorm na GPU contra a CPU, entradas aleatorias */
static int fxv_gpu_test(const char *gguf) {
    FxVae v; if (!fxv_load(&v, gguf)) return 1;
    if (!fxq_open()) { fprintf(stderr, "sem GPU\n"); return 1; }
    int bad = 0; const SdgFile *g = &v.g;
    struct { const char *name; int H, W, cin, up; } T[] = { { "vae.decoder.conv_in", 24, 40, 32, 0 }, { "vae.decoder.mid_block.resnets.0.conv1", 24, 40, 512, 0 },
        { "vae.decoder.up_blocks.0.upsamplers.0.conv", 48, 80, 512, 1 }, { "vae.decoder.up_blocks.2.resnets.0.conv_shortcut", 96, 160, 512, 0 }, { "vae.decoder.conv_out", 96, 160, 128, 0 }, { "vae.decoder.up_blocks.2.upsamplers.0.conv", 192, 320, 256, 1 } };
    for (int ti = 0; ti < 6; ti++) {
        int cin, cout; if (!sdn_conv_cin_cout(g, T[ti].name, &cin, &cout)) { printf("sem %s\n", T[ti].name); continue; }
        const int H = T[ti].H, W = T[ti].W, hin = T[ti].up ? H / 2 : H, win = T[ti].up ? W / 2 : W, pad = cin == 512 && cout == 256 ? 0 : 1;
        const int kk = (int)sdg_find(g, (char[]){0}) ? 0 : 0; (void)kk;
        float *X = xalloc((size_t)hin * win * cin, 4), *Xu = X, *Yc = xalloc((size_t)H * W * cout, 4), *Yg = xalloc((size_t)H * W * cout, 4);
        uint64_t r = 5; for (size_t i = 0; i < (size_t)hin * win * cin; i++) { r = r * 6364136223846793005ULL + 1442695040888963407ULL; X[i] = ((int)(r >> 40) % 2000 - 1000) / 1000.0f; }
        if (T[ti].up) { Xu = xalloc((size_t)H * W * cin, 4); sdr_upsample2x(Xu, X, hin, win, cin); }
        SdrConv c; if (!sdr_conv_load(g, T[ti].name, &c)) return 1;
        const int pd = c.k == 1 ? 0 : 1; (void)pad;
        sdr_conv2d(Yc, Xu, H, W, &c, 1, pd, pd, pd, pd, NULL, NULL); sdr_conv_free(&c);
        KvkBuf bx = kvk_buf_create((size_t)hin * win * cin * 4, KVK_USAGE_RW), by = kvk_buf_create((size_t)H * W * cout * 4, KVK_USAGE_RW);
        kvk_buf_write(bx, 0, X, (size_t)hin * win * cin * 4);
        const FxqConv *fc = fxq_conv(g, T[ti].name);
        const int ok = fc && fxq_run_conv(fc, bx, H, W, 1, pd, pd, pd, pd, T[ti].up, by, (KvkBuf){0}, NULL, NULL);
        kvk_wait(); kvk_buf_read(by, 0, Yg, (size_t)H * W * cout * 4);
        printf("conv %s (%dx%d, %d->%d, ok=%d)\n", T[ti].name, H, W, cin, cout, ok);
        bad += fxv_cmp("  GPU x CPU", Yg, Yc, (size_t)H * W * cout);
    }
    const char *rn[3] = { "vae.decoder.mid_block.resnets.0", "vae.decoder.up_blocks.2.resnets.0", "vae.decoder.up_blocks.3.resnets.0" };
    const int rh[3] = { 24, 96, 192 }, rw[3] = { 40, 160, 320 };
    for (int ti = 0; ti < 3; ti++) {
        char nb[220]; int cin, cout; snprintf(nb, sizeof nb, "%s.conv1", rn[ti]); sdn_conv_cin_cout(g, nb, &cin, &cout);
        const int H = rh[ti], W = rw[ti], n = H * W;
        float *X = xalloc((size_t)n * cin, 4), *Yc = xalloc((size_t)n * cout, 4), *Yg = xalloc((size_t)n * cout, 4);
        uint64_t r = 3; for (size_t i = 0; i < (size_t)n * cin; i++) { r = r * 6364136223846793005ULL + 1442695040888963407ULL; X[i] = ((int)(r >> 40) % 2000 - 1000) / 700.0f; }
        int Co; sdn_resnet(g, rn[ti], X, Yc, H, W, &Co, NULL);
        KvkBuf bb[4]; size_t mx = (size_t)n * (cin > cout ? cin : cout) * 4;
        for (int i = 0; i < 4; i++) bb[i] = kvk_buf_create(mx, KVK_USAGE_RW);
        kvk_buf_write(bb[0], 0, X, (size_t)n * cin * 4);
        const int ok = fxq_resnet(g, rn[ti], bb[0], bb[1], bb[2], bb[3], H, W, &Co);
        kvk_wait(); kvk_buf_read(bb[1], 0, Yg, (size_t)n * cout * 4);
        printf("resnet %s (%dx%d %d->%d ok=%d)\n", rn[ti], H, W, cin, cout, ok);
        bad += fxv_cmp("  GPU x CPU", Yg, Yc, (size_t)n * cout);
        for (int i = 0; i < 4; i++) kvk_buf_destroy(&bb[i]);
    }
    { /* GroupNorm + SiLU */
        const int n = 96 * 160, C = 128; float *X = xalloc((size_t)n * C, 4), *Yc = xalloc((size_t)n * C, 4), *Yg = xalloc((size_t)n * C, 4);
        uint64_t r = 9; for (size_t i = 0; i < (size_t)n * C; i++) { r = r * 6364136223846793005ULL + 1442695040888963407ULL; X[i] = 3.0f + ((int)(r >> 40) % 2000 - 1000) / 300.0f; }
        sdn_gn(Yc, X, n, C, g, "vae.decoder.up_blocks.3.resnets.1.norm1", 1e-6f, 1);
        KvkBuf bx = kvk_buf_create((size_t)n * C * 4, KVK_USAGE_RW), by = kvk_buf_create((size_t)n * C * 4, KVK_USAGE_RW);
        kvk_buf_write(bx, 0, X, (size_t)n * C * 4);
        fxq_gn(g, "vae.decoder.up_blocks.3.resnets.1.norm1", bx, by, n, C, 1);
        kvk_wait(); kvk_buf_read(by, 0, Yg, (size_t)n * C * 4);
        bad += fxv_cmp("groupnorm+silu GPU x CPU", Yg, Yc, (size_t)n * C);
    }
    return bad;
}

static int fxv_main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[0], "gpu-test")) return fxv_gpu_test(argv[1]);
    if (argc >= 3 && !strcmp(argv[0], "convert")) return fxv_convert(argv[1], argv[2]);
    if (argc >= 3 && !strcmp(argv[0], "selftest")) return fxv_selftest(argv[1], argv[2]);
    fprintf(stderr, "uso: katana render flux-vae convert <pasta-do-vae> <saida.gguf>\n     katana render flux-vae selftest <pasta-de-referencia> <vae.gguf>\n");
    return 1;
}
