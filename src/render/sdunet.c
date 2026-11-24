/* ============================ render: UNet e ControlNet na CPU ===============================
 * A UNet do Stable Diffusion 1.5 (860 M de parametros) e o ControlNet (a metade de descida da UNet mais uma entrada de imagem e
 * convolucoes 1x1 de saida), float32, NHWC, lote 1 (docs/render.md, etapa 5). Os pesos vem do GGUF (render/sdgguf.c); cada camada
 * busca os seus pelo nome, no formato do diffusers (prefixo unet. ou ctrl.).
 *
 * Estrutura (config.json: block_out_channels 320/640/1280/1280, 2 resnets por bloco, 8 cabecas de atencao, contexto de 768):
 *   emb do passo: seno/cosseno de 320 numeros (cos primeiro) -> linear 320->1280 -> SiLU -> linear 1280->1280
 *   conv_in 4->320
 *   descida: 4 blocos; nos 3 primeiros, [resnet, atencao] x 2 e uma convolucao 3x3 de passo 2; o 4o so tem 2 resnets
 *            cada saida (conv_in, cada resnet/atencao, cada descida) e guardada como "atalho": 12 no total
 *   meio:    resnet, atencao, resnet
 *   subida:  4 blocos x 3 camadas; cada camada recebe o atalho (concatenado nos canais), resnet e, do 2o bloco em diante, atencao;
 *            ampliacao 2x + convolucao 3x3 no fim dos 3 primeiros blocos
 *   saida:   GroupNorm + SiLU + conv 320->4
 * Resnet: GN(32, eps 1e-5)+SiLU, conv 3x3, soma de linear(SiLU(emb)) em cada pixel, GN+SiLU, conv 3x3, mais o atalho (conv 1x1 se mudar de canais).
 * Atencao (Transformer2D): GN(32, eps 1e-6), conv 1x1 (proj_in), LayerNorm+autoatencao, LayerNorm+atencao ao texto, LayerNorm+GEGLU
 *   (proj 2*F, metade x GELU da outra metade, depois 1x1 F->C), conv 1x1 (proj_out), mais a entrada. q, k, v sem vies; saida com vies.
 * ControlNet: x0 = conv_in(latente) + cond_embedding(imagem); mesma descida e meio; cada atalho (12) e o meio passam por uma conv 1x1
 *   ("zero conv") e por um fator de escala; a UNet soma esses 12 + 1 aos seus atalhos e ao meio (diffusers: down_block_additional_residuals). */

static float *sdn_alloc(size_t n) { return xalloc(n ? n : 1, sizeof(float)); }

/* Cache de pesos preparados: as camadas lineares e as convolucoes sao "abertas" em paineis (vt_pack) uma vez e reusadas em todos os
 * passos e chamadas; sem isso cada passo refazia esse trabalho (e a reordenacao dos pesos de convolucao) em todas as camadas. Custa
 * memoria (os pesos em float32: ate uns 5 GB para UNet + ControlNet) e vale enquanto o processo viver. KATANA_RENDER_NOCACHE=1 desliga. */
typedef struct { const void *p; int rows, cols; float *B; } SdnLinC;
static SdnLinC g_sdn_lc[4096]; static int g_sdn_nlc;
static void sdn_vtlin(float *Y, const float *X, int n, const VtMat *W, const float *bias) {
    float *B = NULL; int own = 1;
    if (!getenv("KATANA_RENDER_NOCACHE")) {
        for (int i = 0; i < g_sdn_nlc; i++) if (g_sdn_lc[i].p == W->p && g_sdn_lc[i].rows == W->rows && g_sdn_lc[i].cols == W->cols) { B = g_sdn_lc[i].B; own = 0; break; }
        if (!B && g_sdn_nlc < 4096) { B = vt_pack(W); g_sdn_lc[g_sdn_nlc++] = (SdnLinC){ W->p, W->rows, W->cols, B }; own = 0; }
    }
    if (!B) B = vt_pack(W);
    VtLinJob j = { Y, X, B, bias, n, W->cols, W->rows, (size_t)W->rows, (size_t)W->cols };
    pool_for(vt_lin_job, &j, (n + VT_ROWS - 1) / VT_ROWS, 1);
    if (own) vt_free(B);
}
typedef struct { const SdgFile *g; char name[128]; SdrConv c; } SdnConvC;
static SdnConvC g_sdn_cc[1024]; static int g_sdn_ncc;

/* camada linear do GGUF: Y[n][rows] = X[n][cols] W^T (+ vies). base = nome sem .weight/.bias; 1 = ok */
static int sdn_lin(float *Y, const float *X, int n, const SdgFile *g, const char *base, int bias, int *rows_out) {
    char nm[220]; int ok = 1;
    snprintf(nm, sizeof nm, "%s.weight", base);
    const VtMat W = sdc_mat(g, nm, &ok);
    const float *b = NULL;
    if (ok && bias) { snprintf(nm, sizeof nm, "%s.bias", base); b = sdc_vec(g, nm, W.rows, &ok); }
    if (!ok) return 0;
    sdn_vtlin(Y, X, n, &W, b);
    if (rows_out) *rows_out = W.rows;
    return 1;
}
/* tamanho de saida de uma camada linear sem calcular nada */
static int sdn_rows(const SdgFile *g, const char *base) {
    char nm[220]; snprintf(nm, sizeof nm, "%s.weight", base);
    const SdgInfo *t = sdg_find(g, nm);
    return t && t->nd == 2 ? (int)t->dim[1] : -1;
}
static int sdn_conv(float *Y, const float *X, int H, int W, const SdgFile *g, const char *base, int stride, int pad_all, int *Ho, int *Wo) {
    if (getenv("KATANA_RENDER_NOCACHE") || strlen(base) >= sizeof g_sdn_cc[0].name) {
        SdrConv c;
        if (!sdr_conv_load(g, base, &c)) return 0;
        sdr_conv2d(Y, X, H, W, &c, stride, pad_all, pad_all, pad_all, pad_all, Ho, Wo);
        sdr_conv_free(&c);
        return 1;
    }
    SdnConvC *e = NULL;
    for (int i = 0; i < g_sdn_ncc; i++) if (g_sdn_cc[i].g == g && !strcmp(g_sdn_cc[i].name, base)) { e = &g_sdn_cc[i]; break; }
    if (!e) {
        if (g_sdn_ncc >= 1024) { fprintf(stderr, "render: cache de convolucoes cheio\n"); return 0; }
        e = &g_sdn_cc[g_sdn_ncc];
        if (!sdr_conv_load(g, base, &e->c)) return 0;
        e->c.packed = vt_pack(&e->c.W);
        e->g = g; snprintf(e->name, sizeof e->name, "%s", base);
        g_sdn_ncc++;
    }
    sdr_conv2d(Y, X, H, W, &e->c, stride, pad_all, pad_all, pad_all, pad_all, Ho, Wo);
    return 1;
}
static int sdn_conv_cin_cout(const SdgFile *g, const char *base, int *cin, int *cout) {
    char nm[220]; snprintf(nm, sizeof nm, "%s.weight", base);
    const SdgInfo *t = sdg_find(g, nm);
    if (!t || t->nd != 4) return 0;
    *cin = (int)t->dim[2]; *cout = (int)t->dim[3];
    return 1;
}
static int sdn_gn(float *Y, const float *X, int n, int C, const SdgFile *g, const char *base, float eps, int silu) {
    char nm[220]; int ok = 1;
    snprintf(nm, sizeof nm, "%s.weight", base); const float *gw = sdc_vec(g, nm, C, &ok);
    snprintf(nm, sizeof nm, "%s.bias", base); const float *gb = sdc_vec(g, nm, C, &ok);
    if (!ok) return 0;
    sdr_groupnorm(Y, X, n, C, 32, gw, gb, eps, silu);
    return 1;
}
static int sdn_ln(float *Y, const float *X, int n, int C, const SdgFile *g, const char *base) {
    char nm[220]; int ok = 1;
    snprintf(nm, sizeof nm, "%s.weight", base); const float *gw = sdc_vec(g, nm, C, &ok);
    snprintf(nm, sizeof nm, "%s.bias", base); const float *gb = sdc_vec(g, nm, C, &ok);
    if (!ok) return 0;
    vt_layernorm(Y, X, n, C, gw, gb, 1e-5f);
    return 1;
}

/* softmax por linha, em paralelo */
typedef struct { float *S; int nk; } SdnSm;
static void sdn_softmax_job(void *cx, int r0, int r1) { const SdnSm *j = cx; for (int r = r0; r < r1; r++) vt_softmax(j->S + (size_t)r * j->nk, j->nk); }

/* atencao de varias cabecas: O[nq][D] = softmax(Q K^T / sqrt(hd)) V; Q[nq][D], K e V[nk][D]. Cabeca por cabeca, em blocos de linhas de
 * Q (a matriz de notas nunca passa de QB x nk): K da cabeca vira o "peso" de um produto linear (linhas = chaves), V^T o do segundo. */
static void sdn_attention(float *O, const float *Q, const float *K, const float *V, int nq, int nk, int D, int heads) {
    const int hd = D / heads, QB = 512;
    const float scale = 1.0f / sqrtf((float)hd);
    float *Kh = sdn_alloc((size_t)nk * hd), *Vt = sdn_alloc((size_t)hd * nk), *Qh = sdn_alloc((size_t)QB * hd), *S = sdn_alloc((size_t)QB * nk), *Oh = sdn_alloc((size_t)QB * hd);
    for (int h = 0; h < heads; h++) {
        for (int j = 0; j < nk; j++) for (int d = 0; d < hd; d++) { Kh[(size_t)j * hd + d] = K[(size_t)j * D + (size_t)h * hd + d]; Vt[(size_t)d * nk + j] = V[(size_t)j * D + (size_t)h * hd + d]; }
        const VtMat Km = { Kh, VT_F32, nk, hd }, Vm = { Vt, VT_F32, hd, nk };
        for (int q0 = 0; q0 < nq; q0 += QB) {
            const int qb = q0 + QB > nq ? nq - q0 : QB;
            for (int i = 0; i < qb; i++) for (int d = 0; d < hd; d++) Qh[(size_t)i * hd + d] = Q[(size_t)(q0 + i) * D + (size_t)h * hd + d] * scale;
            vt_linear(S, Qh, qb, &Km, NULL);
            SdnSm sm = { S, nk };
            pool_for(sdn_softmax_job, &sm, qb, 8);
            vt_linear(Oh, S, qb, &Vm, NULL);
            for (int i = 0; i < qb; i++) memcpy(O + (size_t)(q0 + i) * D + (size_t)h * hd, Oh + (size_t)i * hd, (size_t)hd * 4);
        }
    }
    free(Kh); free(Vt); free(Qh); free(S); free(Oh);
}

static float sdn_gelu(float x) { return 0.5f * x * (1.0f + erff(x * 0.70710678118654752f)); }

/* um bloco de atencao (Transformer2D) no lugar: x[H*W][C]; ctx[77][768] */
static int sdn_transformer(const SdgFile *g, const char *pfx, float *x, int H, int W, int C, const float *ctx, int nctx, int ctxd) {
    const int n = H * W, heads = 8;
    char b[220]; int ok = 1;
    float *res = sdn_alloc((size_t)n * C), *h = sdn_alloc((size_t)n * C), *t = sdn_alloc((size_t)n * C), *q = sdn_alloc((size_t)n * C);
    float *k = sdn_alloc((size_t)(n > nctx ? n : nctx) * C), *v = sdn_alloc((size_t)(n > nctx ? n : nctx) * C), *a = sdn_alloc((size_t)n * C);
    memcpy(res, x, (size_t)n * C * 4);
    snprintf(b, sizeof b, "%s.norm", pfx); ok &= sdn_gn(t, x, n, C, g, b, 1e-6f, 0);
    snprintf(b, sizeof b, "%s.proj_in", pfx); ok &= sdn_conv(h, t, H, W, g, b, 1, 0, NULL, NULL);
    const int F = ok ? sdn_rows(g, (snprintf(b, sizeof b, "%s.transformer_blocks.0.ff.net.2", pfx), b)) : -1;      /* F x ? : so para a conferencia */
    (void)F;
    if (ok) {
        char tb[200]; snprintf(tb, sizeof tb, "%s.transformer_blocks.0", pfx);
        snprintf(b, sizeof b, "%s.norm1", tb); ok &= sdn_ln(t, h, n, C, g, b);
        snprintf(b, sizeof b, "%s.attn1.to_q", tb); ok &= sdn_lin(q, t, n, g, b, 0, NULL);
        snprintf(b, sizeof b, "%s.attn1.to_k", tb); ok &= sdn_lin(k, t, n, g, b, 0, NULL);
        snprintf(b, sizeof b, "%s.attn1.to_v", tb); ok &= sdn_lin(v, t, n, g, b, 0, NULL);
        if (ok) sdn_attention(a, q, k, v, n, n, C, heads);
        snprintf(b, sizeof b, "%s.attn1.to_out.0", tb); ok &= sdn_lin(t, a, n, g, b, 1, NULL);
        if (ok) for (size_t i = 0; i < (size_t)n * C; i++) h[i] += t[i];
        snprintf(b, sizeof b, "%s.norm2", tb); ok &= sdn_ln(t, h, n, C, g, b);
        snprintf(b, sizeof b, "%s.attn2.to_q", tb); ok &= sdn_lin(q, t, n, g, b, 0, NULL);
        snprintf(b, sizeof b, "%s.attn2.to_k", tb); ok &= sdn_lin(k, ctx, nctx, g, b, 0, NULL);
        snprintf(b, sizeof b, "%s.attn2.to_v", tb); ok &= sdn_lin(v, ctx, nctx, g, b, 0, NULL);
        (void)ctxd;
        if (ok) sdn_attention(a, q, k, v, n, nctx, C, heads);
        snprintf(b, sizeof b, "%s.attn2.to_out.0", tb); ok &= sdn_lin(t, a, n, g, b, 1, NULL);
        if (ok) for (size_t i = 0; i < (size_t)n * C; i++) h[i] += t[i];
        snprintf(b, sizeof b, "%s.norm3", tb); ok &= sdn_ln(t, h, n, C, g, b);
        snprintf(b, sizeof b, "%s.ff.net.0.proj", tb);
        const int P = sdn_rows(g, b), FF = P / 2;
        if (P < 2) ok = 0;
        if (ok) {
            float *pj = sdn_alloc((size_t)n * P), *gg = sdn_alloc((size_t)n * FF);
            ok &= sdn_lin(pj, t, n, g, b, 1, NULL);
            if (ok) {
                for (int i = 0; i < n; i++) for (int j = 0; j < FF; j++) gg[(size_t)i * FF + j] = pj[(size_t)i * P + j] * sdn_gelu(pj[(size_t)i * P + FF + j]);      /* GEGLU */
                snprintf(b, sizeof b, "%s.ff.net.2", tb); ok &= sdn_lin(t, gg, n, g, b, 1, NULL);
                if (ok) for (size_t i = 0; i < (size_t)n * C; i++) h[i] += t[i];
            }
            free(pj); free(gg);
        }
        snprintf(b, sizeof b, "%s.proj_out", pfx); ok &= sdn_conv(t, h, H, W, g, b, 1, 0, NULL, NULL);
        if (ok) for (size_t i = 0; i < (size_t)n * C; i++) x[i] = t[i] + res[i];
    }
    free(res); free(h); free(t); free(q); free(k); free(v); free(a);
    return ok;
}

/* resnet: x[H*W][Cin] -> out[H*W][Cout]; temb_act = SiLU(emb do passo) [1280]. temb_act NULL = resnet do VAE: sem emb do passo e com eps 1e-6 */
static int sdn_resnet(const SdgFile *g, const char *pfx, const float *x, float *out, int H, int W, int *Cout_out, const float *temb_act) {
    const int n = H * W; char b[220]; int Cin, Cout, ok = 1;
    snprintf(b, sizeof b, "%s.conv1", pfx);
    if (!sdn_conv_cin_cout(g, b, &Cin, &Cout)) return 0;
    float *t = sdn_alloc((size_t)n * (Cin > Cout ? Cin : Cout)), *u = sdn_alloc((size_t)n * Cout), *tp = sdn_alloc((size_t)Cout);
    const float eps = temb_act ? 1e-5f : 1e-6f;
    snprintf(b, sizeof b, "%s.norm1", pfx); ok &= sdn_gn(t, x, n, Cin, g, b, eps, 1);
    snprintf(b, sizeof b, "%s.conv1", pfx); ok &= sdn_conv(u, t, H, W, g, b, 1, 1, NULL, NULL);
    if (temb_act) { snprintf(b, sizeof b, "%s.time_emb_proj", pfx); ok &= sdn_lin(tp, temb_act, 1, g, b, 1, NULL); }
    if (ok && temb_act) for (int i = 0; i < n; i++) for (int c = 0; c < Cout; c++) u[(size_t)i * Cout + c] += tp[c];
    snprintf(b, sizeof b, "%s.norm2", pfx); ok &= sdn_gn(t, u, n, Cout, g, b, eps, 1);
    snprintf(b, sizeof b, "%s.conv2", pfx); ok &= sdn_conv(out, t, H, W, g, b, 1, 1, NULL, NULL);
    if (ok) {
        snprintf(b, sizeof b, "%s.conv_shortcut.weight", pfx);
        if (sdg_find(g, b)) { snprintf(b, sizeof b, "%s.conv_shortcut", pfx); ok &= sdn_conv(u, x, H, W, g, b, 1, 0, NULL, NULL); for (size_t i = 0; i < (size_t)n * Cout; i++) out[i] += u[i]; }
        else if (Cin == Cout) for (size_t i = 0; i < (size_t)n * Cout; i++) out[i] += x[i];
        else ok = 0;
    }
    *Cout_out = Cout;
    free(t); free(u); free(tp);
    return ok;
}

/* emb do passo (1280) e SiLU dele: temb[1280], act[1280] */
static int sdn_temb(const SdgFile *g, const char *pfx, float t, float *temb, float *act) {
    float sin_cos[320], h[1280]; char b[220];
    for (int i = 0; i < 160; i++) {
        const float a = t * expf(-logf(10000.0f) * (float)i / 160.0f);
        sin_cos[i] = cosf(a); sin_cos[160 + i] = sinf(a);            /* flip_sin_to_cos: cos primeiro */
    }
    snprintf(b, sizeof b, "%s.time_embedding.linear_1", pfx); if (!sdn_lin(h, sin_cos, 1, g, b, 1, NULL)) return 0;
    for (int i = 0; i < 1280; i++) h[i] = h[i] / (1.0f + expf(-h[i]));
    snprintf(b, sizeof b, "%s.time_embedding.linear_2", pfx); if (!sdn_lin(temb, h, 1, g, b, 1, NULL)) return 0;
    for (int i = 0; i < 1280; i++) act[i] = temb[i] / (1.0f + expf(-temb[i]));
    return 1;
}

/* atalhos da descida */
typedef struct { float *a[12]; int H[12], W[12], C[12]; int n; } SdSkips;
static void sdn_skip_push(SdSkips *s, const float *x, int H, int W, int C) {
    s->a[s->n] = sdn_alloc((size_t)H * W * C); memcpy(s->a[s->n], x, (size_t)H * W * C * 4);
    s->H[s->n] = H; s->W[s->n] = W; s->C[s->n] = C; s->n++;
}
static void sdn_skips_free(SdSkips *s) { for (int i = 0; i < s->n; i++) free(s->a[i]); s->n = 0; }

/* descida e meio, para a UNet e para o ControlNet: x0 = saida do conv_in (com a imagem de controle, no ControlNet), 64x64x320.
 * Devolve os 12 atalhos e o meio (memoria nova, 8x8x1280). */
static int sdn_encoder(const SdgFile *g, const char *pfx, const float *x0, int H0, int W0, const float *temb_act, const float *ctx, int nctx, SdSkips *sk, float **mid_out, int *Hm, int *Wm) {
    int H = H0, W = W0, C = 320, ok = 1; char b[220];
    float *x = sdn_alloc((size_t)H * W * C); memcpy(x, x0, (size_t)H * W * C * 4);
    memset(sk, 0, sizeof *sk);
    sdn_skip_push(sk, x, H, W, C);
    for (int blk = 0; blk < 4 && ok; blk++) {
        for (int l = 0; l < 2 && ok; l++) {
            int Co; float *y = sdn_alloc((size_t)H * W * 1280);
            snprintf(b, sizeof b, "%s.down_blocks.%d.resnets.%d", pfx, blk, l);
            ok &= sdn_resnet(g, b, x, y, H, W, &Co, temb_act);
            free(x); x = y; C = Co;
            if (ok && blk < 3) { snprintf(b, sizeof b, "%s.down_blocks.%d.attentions.%d", pfx, blk, l); ok &= sdn_transformer(g, b, x, H, W, C, ctx, nctx, 768); }
            if (ok) sdn_skip_push(sk, x, H, W, C);
        }
        if (ok && blk < 3) {
            const int Ho = (H + 2 - 3) / 2 + 1, Wo = (W + 2 - 3) / 2 + 1;
            float *y = sdn_alloc((size_t)Ho * Wo * C);
            snprintf(b, sizeof b, "%s.down_blocks.%d.downsamplers.0.conv", pfx, blk);
            ok &= sdn_conv(y, x, H, W, g, b, 2, 1, NULL, NULL);
            free(x); x = y; H = Ho; W = Wo;
            if (ok) sdn_skip_push(sk, x, H, W, C);
        }
    }
    if (ok) {
        float *y = sdn_alloc((size_t)H * W * C); int Co;
        snprintf(b, sizeof b, "%s.mid_block.resnets.0", pfx); ok &= sdn_resnet(g, b, x, y, H, W, &Co, temb_act);
        free(x); x = y;
        if (ok) { snprintf(b, sizeof b, "%s.mid_block.attentions.0", pfx); ok &= sdn_transformer(g, b, x, H, W, C, ctx, nctx, 768); }
        y = sdn_alloc((size_t)H * W * C);
        if (ok) { snprintf(b, sizeof b, "%s.mid_block.resnets.1", pfx); ok &= sdn_resnet(g, b, x, y, H, W, &Co, temb_act); }
        free(x); x = y;
    }
    *mid_out = x; *Hm = H; *Wm = W;
    return ok;
}

/* cadeia de convolucoes da imagem de controle: img[H*W][3] (valores em [0,1]) -> [H/8*W/8][320] (memoria nova) */
static float *sdn_cond_embed(const SdgFile *ctrl, const float *img, int H, int W, int *Ho_out, int *Wo_out) {
    static const char *const nm[8] = { "ctrl.controlnet_cond_embedding.conv_in", "ctrl.controlnet_cond_embedding.blocks.0", "ctrl.controlnet_cond_embedding.blocks.1",
        "ctrl.controlnet_cond_embedding.blocks.2", "ctrl.controlnet_cond_embedding.blocks.3", "ctrl.controlnet_cond_embedding.blocks.4",
        "ctrl.controlnet_cond_embedding.blocks.5", "ctrl.controlnet_cond_embedding.conv_out" };
    static const int stride[8] = { 1, 1, 2, 1, 2, 1, 2, 1 };
    float *cur = sdn_alloc((size_t)H * W * 3); memcpy(cur, img, (size_t)H * W * 3 * 4);
    int h = H, w = W, c = 3;
    for (int i = 0; i < 8; i++) {
        SdrConv cv;
        if (!sdr_conv_load(ctrl, nm[i], &cv)) { free(cur); return NULL; }
        const int ho = (h + 2 - 3) / stride[i] + 1, wo = (w + 2 - 3) / stride[i] + 1;
        float *nx = sdn_alloc((size_t)ho * wo * cv.cout);
        sdr_conv2d(nx, cur, h, w, &cv, stride[i], 1, 1, 1, 1, NULL, NULL);
        if (i < 7) sdr_silu(nx, (size_t)ho * wo * cv.cout);
        c = cv.cout; sdr_conv_free(&cv);
        free(cur); cur = nx; h = ho; w = wo;
    }
    (void)c;
    *Ho_out = h; *Wo_out = w;
    return cur;
}

/* ControlNet: lat[H*W][4], passo t, ctx[77][768], cond[H*W][320] (sdn_cond_embed), escala -> 12 residuos + meio (memoria nova nos SdSkips e em *mid) */
static int sdn_control(const SdgFile *ctrl, const float *lat, int H, int W, float t, const float *ctx, int nctx, const float *cond, float scale, SdSkips *res, float **mid_res, int *Hm, int *Wm) {
    float temb[1280], act[1280]; char b[220]; int ok = 1;
    if (!sdn_temb(ctrl, "ctrl", t, temb, act)) return 0;
    float *x0 = sdn_alloc((size_t)H * W * 320);
    ok &= sdn_conv(x0, lat, H, W, ctrl, "ctrl.conv_in", 1, 1, NULL, NULL);
    if (ok) for (size_t i = 0; i < (size_t)H * W * 320; i++) x0[i] += cond[i];
    SdSkips sk; float *mid = NULL;
    if (ok) ok &= sdn_encoder(ctrl, "ctrl", x0, H, W, act, ctx, nctx, &sk, &mid, Hm, Wm);
    free(x0);
    memset(res, 0, sizeof *res);
    for (int i = 0; ok && i < sk.n; i++) {
        float *y = sdn_alloc((size_t)sk.H[i] * sk.W[i] * sk.C[i]);
        snprintf(b, sizeof b, "ctrl.controlnet_down_blocks.%d", i);
        ok &= sdn_conv(y, sk.a[i], sk.H[i], sk.W[i], ctrl, b, 1, 0, NULL, NULL);
        for (size_t k = 0; k < (size_t)sk.H[i] * sk.W[i] * sk.C[i]; k++) y[k] *= scale;
        res->a[res->n] = y; res->H[res->n] = sk.H[i]; res->W[res->n] = sk.W[i]; res->C[res->n] = sk.C[i]; res->n++;
    }
    if (ok) {
        *mid_res = sdn_alloc((size_t)*Hm * *Wm * 1280);
        ok &= sdn_conv(*mid_res, mid, *Hm, *Wm, ctrl, "ctrl.controlnet_mid_block", 1, 0, NULL, NULL);
        for (size_t k = 0; k < (size_t)*Hm * *Wm * 1280; k++) (*mid_res)[k] *= scale;
    }
    sdn_skips_free(&sk); free(mid);
    return ok;
}

/* UNet: lat[H*W][4], passo t, ctx[77][768]; res (12 atalhos de ControlNet) e mid_res podem ser NULL; saida eps[H*W][4] */
static int sdn_unet(const SdgFile *g, const float *lat, int H0, int W0, float t, const float *ctx, int nctx, const SdSkips *res, const float *mid_res, float *eps) {
    float temb[1280], act[1280]; char b[220]; int ok = 1;
    if (!sdn_temb(g, "unet", t, temb, act)) return 0;
    float *x0 = sdn_alloc((size_t)H0 * W0 * 320);
    ok &= sdn_conv(x0, lat, H0, W0, g, "unet.conv_in", 1, 1, NULL, NULL);
    SdSkips sk; float *x = NULL; int H, W;
    if (ok) ok &= sdn_encoder(g, "unet", x0, H0, W0, act, ctx, nctx, &sk, &x, &H, &W);
    free(x0);
    if (ok && res) { for (int i = 0; i < sk.n; i++) for (size_t k = 0; k < (size_t)sk.H[i] * sk.W[i] * sk.C[i]; k++) sk.a[i][k] += res->a[i][k]; }
    if (ok && mid_res) for (size_t k = 0; k < (size_t)H * W * 1280; k++) x[k] += mid_res[k];
    int C = 1280;
    for (int blk = 0; blk < 4 && ok; blk++) {
        for (int l = 0; l < 3 && ok; l++) {
            sk.n--;                                                       /* o ultimo atalho que sobrou */
            const int sc = sk.C[sk.n];
            float *cat = sdn_alloc((size_t)H * W * (C + sc));
            for (int i = 0; i < H * W; i++) { memcpy(cat + (size_t)i * (C + sc), x + (size_t)i * C, (size_t)C * 4); memcpy(cat + (size_t)i * (C + sc) + C, sk.a[sk.n] + (size_t)i * sc, (size_t)sc * 4); }
            free(sk.a[sk.n]); sk.a[sk.n] = NULL;
            int Co; float *y = sdn_alloc((size_t)H * W * 1280);
            snprintf(b, sizeof b, "unet.up_blocks.%d.resnets.%d", blk, l);
            ok &= sdn_resnet(g, b, cat, y, H, W, &Co, act);
            free(cat); free(x); x = y; C = Co;
            if (ok && blk > 0) { snprintf(b, sizeof b, "unet.up_blocks.%d.attentions.%d", blk, l); ok &= sdn_transformer(g, b, x, H, W, C, ctx, nctx, 768); }
        }
        if (ok && blk < 3) {
            float *u = sdn_alloc((size_t)4 * H * W * C), *y = sdn_alloc((size_t)4 * H * W * C);
            sdr_upsample2x(u, x, H, W, C);
            snprintf(b, sizeof b, "unet.up_blocks.%d.upsamplers.0.conv", blk);
            ok &= sdn_conv(y, u, 2 * H, 2 * W, g, b, 1, 1, NULL, NULL);
            free(u); free(x); x = y; H *= 2; W *= 2;
        }
    }
    if (ok) {
        float *t1 = sdn_alloc((size_t)H * W * C);
        ok &= sdn_gn(t1, x, H * W, C, g, "unet.conv_norm_out", 1e-5f, 1);
        if (ok) ok &= sdn_conv(eps, t1, H, W, g, "unet.conv_out", 1, 1, NULL, NULL);
        free(t1);
    }
    free(x); sdn_skips_free(&sk);
    return ok;
}
