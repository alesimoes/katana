/* ============================ render: VAE (codificador e decodificador) na CPU ===============================
 * O autoencoder do Stable Diffusion (sd-vae-ft-mse, 83,7 M de parametros), float32, NHWC (docs/render.md, etapa 6). O fator de escala do
 * latente (0,18215) NAO e aplicado aqui: quem chama divide o latente por ele antes do decodificador e multiplica a media do codificador.
 *
 *   decodificador: post_quant_conv 1x1 (4->4), conv_in 4->512, meio (resnet, atencao, resnet), 4 blocos de 3 resnets com ampliacao 2x
 *                  + conv 3x3 nos 3 primeiros (canais 512, 512, 256, 128), GroupNorm + SiLU, conv_out 128->3. Imagem em [-1, 1].
 *   codificador:   conv_in 3->128, 4 blocos de 2 resnets com descida (conv 3x3, passo 2, preenchimento so embaixo e a direita: 0,1,0,1)
 *                  nos 3 primeiros (canais 128, 256, 512, 512), meio, GroupNorm + SiLU, conv_out 512->8, quant_conv 1x1 (8->8);
 *                  os 4 primeiros canais sao a media do latente, os 4 seguintes o log da variancia (nao usado aqui).
 * Resnet: o mesmo da UNet sem o passo de tempo (sdn_resnet com temb NULL), GroupNorm com eps 1e-6.
 * Atencao do meio: GN(32, eps 1e-6), q, k, v lineares com vies, UMA cabeca de 512 sobre todos os pixels, saida linear, mais a entrada. */

static int sdv_attention(const SdgFile *g, const char *pfx, float *x, int H, int W, int C) {
    const int n = H * W; char b[220]; int ok = 1;
    float *t = sdn_alloc((size_t)n * C), *q = sdn_alloc((size_t)n * C), *k = sdn_alloc((size_t)n * C), *v = sdn_alloc((size_t)n * C), *a = sdn_alloc((size_t)n * C);
    snprintf(b, sizeof b, "%s.group_norm", pfx); ok &= sdn_gn(t, x, n, C, g, b, 1e-6f, 0);
    snprintf(b, sizeof b, "%s.to_q", pfx); ok &= sdn_lin(q, t, n, g, b, 1, NULL);
    snprintf(b, sizeof b, "%s.to_k", pfx); ok &= sdn_lin(k, t, n, g, b, 1, NULL);
    snprintf(b, sizeof b, "%s.to_v", pfx); ok &= sdn_lin(v, t, n, g, b, 1, NULL);
    if (ok) sdn_attention(a, q, k, v, n, n, C, 1);
    snprintf(b, sizeof b, "%s.to_out.0", pfx); ok &= sdn_lin(t, a, n, g, b, 1, NULL);
    if (ok) for (size_t i = 0; i < (size_t)n * C; i++) x[i] += t[i];
    free(t); free(q); free(k); free(v); free(a);
    return ok;
}

/* resnet, atencao, resnet do meio; x[H*W][C] no lugar */
static int sdv_mid(const SdgFile *g, const char *pfx, float **x, int H, int W, int C) {
    char b[220]; int Co, ok = 1;
    float *y = sdn_alloc((size_t)H * W * C);
    snprintf(b, sizeof b, "%s.resnets.0", pfx); ok &= sdn_resnet(g, b, *x, y, H, W, &Co, NULL);
    free(*x); *x = y;
    if (ok) { snprintf(b, sizeof b, "%s.attentions.0", pfx); ok &= sdv_attention(g, b, *x, H, W, C); }
    y = sdn_alloc((size_t)H * W * C);
    if (ok) { snprintf(b, sizeof b, "%s.resnets.1", pfx); ok &= sdn_resnet(g, b, *x, y, H, W, &Co, NULL); }
    free(*x); *x = y;
    return ok;
}

/* lat[h*w][4] (ja dividido pelo fator de escala) -> imagem [8h*8w][3] (memoria nova) ou NULL */
static float *sdv_decode(const SdgFile *g, const float *lat, int h, int w, int *Ho, int *Wo) {
    int H = h, W = w, C = 4, ok = 1; char b[220];
    float *x = sdn_alloc((size_t)H * W * 512);
    ok &= sdn_conv(x, lat, H, W, g, "vae.post_quant_conv", 1, 0, NULL, NULL);
    float *y = sdn_alloc((size_t)H * W * 512);
    if (ok) ok &= sdn_conv(y, x, H, W, g, "vae.decoder.conv_in", 1, 1, NULL, NULL);
    free(x); x = y; C = 512;
    if (ok) ok &= sdv_mid(g, "vae.decoder.mid_block", &x, H, W, C);
    for (int blk = 0; blk < 4 && ok; blk++) {
        for (int l = 0; l < 3 && ok; l++) {
            int Co; y = sdn_alloc((size_t)H * W * 512);
            snprintf(b, sizeof b, "vae.decoder.up_blocks.%d.resnets.%d", blk, l);
            ok &= sdn_resnet(g, b, x, y, H, W, &Co, NULL);
            free(x); x = y; C = Co;
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

/* img[H*W][3] em [-1, 1] -> media do latente [H/8*W/8][4] (memoria nova) ou NULL */
static float *sdv_encode(const SdgFile *g, const float *img, int H0, int W0, int *ho, int *wo) {
    int H = H0, W = W0, C = 128, ok = 1; char b[220];
    float *x = sdn_alloc((size_t)H * W * C);
    ok &= sdn_conv(x, img, H, W, g, "vae.encoder.conv_in", 1, 1, NULL, NULL);
    for (int blk = 0; blk < 4 && ok; blk++) {
        for (int l = 0; l < 2 && ok; l++) {
            int Co; float *y = sdn_alloc((size_t)H * W * 512);
            snprintf(b, sizeof b, "vae.encoder.down_blocks.%d.resnets.%d", blk, l);
            ok &= sdn_resnet(g, b, x, y, H, W, &Co, NULL);
            free(x); x = y; C = Co;
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
        float *t = sdn_alloc((size_t)H * W * C), *m8 = sdn_alloc((size_t)H * W * 8), *q8 = sdn_alloc((size_t)H * W * 8);
        ok &= sdn_gn(t, x, H * W, C, g, "vae.encoder.conv_norm_out", 1e-6f, 1);
        if (ok) ok &= sdn_conv(m8, t, H, W, g, "vae.encoder.conv_out", 1, 1, NULL, NULL);
        if (ok) ok &= sdn_conv(q8, m8, H, W, g, "vae.quant_conv", 1, 0, NULL, NULL);
        if (ok) { mean = sdn_alloc((size_t)H * W * 4); for (int i = 0; i < H * W; i++) memcpy(mean + (size_t)i * 4, q8 + (size_t)i * 8, 16); }
        free(t); free(m8); free(q8);
    }
    free(x);
    if (!ok) { free(mean); return NULL; }
    *ho = H; *wo = W;
    return mean;
}
