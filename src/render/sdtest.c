/* ============================ katana render selftest ===============================
 * katana render selftest <pasta-de-referencia> <pasta-models/render>
 * Compara as operacoes de render/sdops.c com os tensores de referencia (float32 do diffusers, em /mnt/dados/render-ref/ref, ver
 * docs/render.md) usando os pesos dos GGUF. Cada teste imprime o maior erro absoluto, o maior |valor| da referencia e a razao;
 * passa se a razao for menor que 1e-4 (a ordem das somas muda em float32; fora isso as contas sao as mesmas). */

static float *sdt_ref(const char *dir, const char *name, size_t *n) {
    char p[1100]; snprintf(p, sizeof p, "%s/%s.bin", dir, name);
    int64_t sz = 0; const uint8_t *b = map_file(p, &sz);
    if (!b) { fprintf(stderr, "selftest: nao achei %s\n", p); return NULL; }
    float *f = xalloc((size_t)sz / 4, sizeof(float));
    memcpy(f, b, (size_t)sz);
    if (n) *n = (size_t)sz / 4;
    return f;
}
/* [C][H][W] -> [H*W][C] */
static float *sdt_to_nhwc(const float *a, int C, int HW) {
    float *o = xalloc((size_t)C * HW, sizeof(float));
    for (int c = 0; c < C; c++) for (int p = 0; p < HW; p++) o[(size_t)p * C + c] = a[(size_t)c * HW + p];
    return o;
}
static int sdt_compare(const char *what, const float *mine_nhwc, int C, int HW, const float *ref_nchw, double secs) {
    double mx = 0, mr = 0; size_t arg = 0;
    for (int c = 0; c < C; c++)
        for (int p = 0; p < HW; p++) {
            const double a = mine_nhwc[(size_t)p * C + c], r = ref_nchw[(size_t)c * HW + p], d = fabs(a - r);
            if (!(d <= mx)) { mx = d; arg = (size_t)c * HW + p; }       /* NaN conta como erro */
            if (fabs(r) > mr) mr = fabs(r);
        }
    const double ratio = mr > 0 ? mx / mr : mx;
    const int ok = ratio < 1e-4;
    printf("  %-34s %s  erro max %.3e  |ref| max %.3f  razao %.2e  (%.2f s)%s\n", what, ok ? "OK    " : "FALHOU", mx, mr, ratio, secs, ok ? "" : "  <- pior no indice");
    if (!ok) printf("      indice %zu: ref %.6f\n", arg, ref_nchw[arg]);
    return ok ? 0 : 1;
}
static int sdt_cmp_flat(const char *what, const float *a, const float *b, size_t n, double secs) {
    double mx = 0, mr = 0;
    for (size_t i = 0; i < n; i++) { const double d = fabs((double)a[i] - b[i]); if (!(d <= mx)) mx = d; if (fabs(b[i]) > mr) mr = fabs(b[i]); }
    const double ratio = mr > 0 ? mx / mr : mx; const int ok = ratio < 1e-4;
    printf("  %-34s %s  erro max %.3e  |ref| max %.3f  razao %.2e  (%.2f s)\n", what, ok ? "OK    " : "FALHOU", mx, mr, ratio, secs);
    return ok ? 0 : 1;
}
static double sdt_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

/* um resnet do VAE (sem mudanca de canais): norma+SiLU, conv, norma+SiLU, conv, soma com a entrada */
static void sdt_vae_resnet(const SdgFile *g, const char *pfx, float *out, const float *in, int H, int W, int C) {
    char nm[200]; SdrConv c1, c2;
    snprintf(nm, sizeof nm, "%s.conv1", pfx); sdr_conv_load(g, nm, &c1);
    snprintf(nm, sizeof nm, "%s.conv2", pfx); sdr_conv_load(g, nm, &c2);
    float *t = xalloc((size_t)H * W * C, sizeof(float)), *u = xalloc((size_t)H * W * C, sizeof(float));
    uint64_t n;
    snprintf(nm, sizeof nm, "%s.norm1.weight", pfx); float *g1 = sdg_floats(g, nm, &n);
    snprintf(nm, sizeof nm, "%s.norm1.bias", pfx); float *b1 = sdg_floats(g, nm, &n);
    snprintf(nm, sizeof nm, "%s.norm2.weight", pfx); float *g2 = sdg_floats(g, nm, &n);
    snprintf(nm, sizeof nm, "%s.norm2.bias", pfx); float *b2 = sdg_floats(g, nm, &n);
    sdr_groupnorm(t, in, H * W, C, 32, g1, b1, 1e-6f, 1);
    sdr_conv2d(u, t, H, W, &c1, 1, 1, 1, 1, 1, NULL, NULL);
    sdr_groupnorm(t, u, H * W, C, 32, g2, b2, 1e-6f, 1);
    sdr_conv2d(out, t, H, W, &c2, 1, 1, 1, 1, 1, NULL, NULL);
    for (size_t i = 0; i < (size_t)H * W * C; i++) out[i] += in[i];
    free(t); free(u); free(g1); free(b1); free(g2); free(b2); sdr_conv_free(&c1); sdr_conv_free(&c2);
}

static int sdt_main(const char *refdir, const char *mdir) {
    SdgFile base, ctrl; char p[1100]; int bad = 0; double t0;
    snprintf(p, sizeof p, "%s/sd15-realistic-vision.gguf", mdir); if (!sdg_open(&base, p)) return 1;
    snprintf(p, sizeof p, "%s/sd15-controlnet-canny.gguf", mdir); if (!sdg_open(&ctrl, p)) return 1;
    printf("katana render selftest (CPU, float32, %d threads)\n", pool.n);

    /* 1. convolucao 3x3 com 4 canais de entrada: a primeira camada da UNet */
    { float *lat = sdt_ref(refdir, "unet_latent_in", NULL), *ref = sdt_ref(refdir, "unet_conv_in", NULL);
      if (!lat || !ref) return 1;
      float *x = sdt_to_nhwc(lat, 4, 64 * 64), *y = xalloc((size_t)64 * 64 * 320, sizeof(float)); SdrConv c;
      if (!sdr_conv_load(&base, "unet.conv_in", &c)) return 1;
      t0 = sdt_now(); sdr_conv2d(y, x, 64, 64, &c, 1, 1, 1, 1, 1, NULL, NULL);
      bad += sdt_compare("UNet conv_in (conv 3x3, 4 -> 320)", y, 320, 64 * 64, ref, sdt_now() - t0);
      sdr_conv_free(&c); free(lat); free(ref); free(x); free(y); }

    /* 2. a cadeia de convolucoes do ControlNet: stride 2, SiLU entre elas, de 512x512x3 a 64x64x320 */
    { float *img = sdt_ref(refdir, "control_image", NULL), *ref = sdt_ref(refdir, "control_cond_embedding", NULL);
      if (!img || !ref) return 1;
      static const char *const nm[8] = { "ctrl.controlnet_cond_embedding.conv_in", "ctrl.controlnet_cond_embedding.blocks.0", "ctrl.controlnet_cond_embedding.blocks.1",
          "ctrl.controlnet_cond_embedding.blocks.2", "ctrl.controlnet_cond_embedding.blocks.3", "ctrl.controlnet_cond_embedding.blocks.4",
          "ctrl.controlnet_cond_embedding.blocks.5", "ctrl.controlnet_cond_embedding.conv_out" };
      static const int stride[8] = { 1, 1, 2, 1, 2, 1, 2, 1 };
      float *cur = sdt_to_nhwc(img, 3, 512 * 512); int H = 512, W = 512; SdrConv c; t0 = sdt_now(); int ok = 1;
      for (int i = 0; i < 8; i++) {
          if (!sdr_conv_load(&ctrl, nm[i], &c)) return 1;
          int Ho, Wo; float *nx = xalloc((size_t)((H + 2 - 3) / stride[i] + 1) * ((W + 2 - 3) / stride[i] + 1) * c.cout, sizeof(float));
          sdr_conv2d(nx, cur, H, W, &c, stride[i], 1, 1, 1, 1, &Ho, &Wo);
          if (i < 7) sdr_silu(nx, (size_t)Ho * Wo * c.cout);
          free(cur); cur = nx; H = Ho; W = Wo; sdr_conv_free(&c);
          if (i == 7 && (H != 64 || W != 64)) ok = 0;
      }
      if (!ok) { printf("  cadeia do ControlNet: tamanho final %dx%d (esperado 64x64)\n", H, W); bad++; }
      else bad += sdt_compare("ControlNet cond_embedding (8 convs)", cur, 320, 64 * 64, ref, sdt_now() - t0);
      free(img); free(ref); free(cur); }

    /* 3. conv do VAE com 512 canais, depois um resnet inteiro: norma por grupos + SiLU + conv, duas vezes, mais a soma */
    { float *lat = sdt_ref(refdir, "vae_decode_in", NULL), *r1 = sdt_ref(refdir, "vae_dec_conv_in", NULL), *r2 = sdt_ref(refdir, "vae_dec_mid_res0", NULL);
      if (!lat || !r1 || !r2) return 1;
      float *x0 = sdt_to_nhwc(lat, 4, 64 * 64), *x = xalloc((size_t)64 * 64 * 4, sizeof(float)), *y = xalloc((size_t)64 * 64 * 512, sizeof(float)), *z = xalloc((size_t)64 * 64 * 512, sizeof(float)); SdrConv c, pq;
      if (!sdr_conv_load(&base, "vae.post_quant_conv", &pq) || !sdr_conv_load(&base, "vae.decoder.conv_in", &c)) return 1;
      t0 = sdt_now(); sdr_conv2d(x, x0, 64, 64, &pq, 1, 0, 0, 0, 0, NULL, NULL);      /* vae.decode: primeiro a convolucao 1x1 post_quant_conv */
      sdr_conv2d(y, x, 64, 64, &c, 1, 1, 1, 1, 1, NULL, NULL);
      bad += sdt_compare("VAE post_quant 1x1 + conv_in (4 -> 512)", y, 512, 64 * 64, r1, sdt_now() - t0);
      sdr_conv_free(&c);
      t0 = sdt_now(); sdt_vae_resnet(&base, "vae.decoder.mid_block.resnets.0", z, y, 64, 64, 512);
      bad += sdt_compare("VAE mid resnet (GroupNorm+SiLU+conv)", z, 512, 64 * 64, r2, sdt_now() - t0);
      sdr_conv_free(&pq); free(lat); free(r1); free(r2); free(x0); free(x); free(y); free(z); }

    /* 4. ampliacao 2x seguida de convolucao: o fim de um bloco do decodificador */
    { float *in = sdt_ref(refdir, "vae_dec_up0_res2", NULL), *ref = sdt_ref(refdir, "vae_dec_up0", NULL);
      if (!in || !ref) return 1;
      float *x = sdt_to_nhwc(in, 512, 64 * 64), *u = xalloc((size_t)128 * 128 * 512, sizeof(float)), *y = xalloc((size_t)128 * 128 * 512, sizeof(float)); SdrConv c;
      if (!sdr_conv_load(&base, "vae.decoder.up_blocks.0.upsamplers.0.conv", &c)) return 1;
      t0 = sdt_now(); sdr_upsample2x(u, x, 64, 64, 512); sdr_conv2d(y, u, 128, 128, &c, 1, 1, 1, 1, 1, NULL, NULL);
      bad += sdt_compare("VAE upsample 2x + conv (512, 128x128)", y, 512, 128 * 128, ref, sdt_now() - t0);
      sdr_conv_free(&c); free(in); free(ref); free(x); free(u); free(y); }

    /* 5. CLIP: o tokenizador contra o CLIPTokenizer em 13 prompts, e o codificador contra o CLIPTextModel */
    { SdClip cl; if (!sdc_load(&cl, &base)) return 1;
      JV cj; snprintf(p, sizeof p, "%s/tok_cases.json", refdir);
      if (!cv_json_file(p, &cj) || cj.t != J_ARR) { fprintf(stderr, "selftest: nao achei %s\n", p); return 1; }
      int same = 0;
      for (size_t i = 0; i < cj.n; i++) {
          const JV *e = &cj.v[i], *ids = jget(e, "ids"); const char *txt = jstr(e, "p");
          int32_t mine[77]; sdc_tokenize(&cl, txt ? txt : "", mine);
          int diff = -1;
          for (int k = 0; k < 77; k++) if (!ids || (size_t)k >= ids->n || mine[k] != (int32_t)ids->v[k].i) { diff = k; break; }
          if (diff < 0) same++;
          else { printf("  tokenizador, caso %zu diferente na posicao %d: mine %d, ref %d  (%.50s)\n", i, diff, mine[diff], ids && (size_t)diff < ids->n ? (int)ids->v[diff].i : -1, txt ? txt : ""); }
      }
      printf("  %-34s %s  %d de %zu casos identicos ao CLIPTokenizer\n", "tokenizador CLIP", same == (int)cj.n ? "OK    " : "FALHOU", same, cj.n);
      bad += same == (int)cj.n ? 0 : 1;
      { int wi[64]; float ww[64];                                            /* pesos A1111: (a:1.3), [b], (c), aninhado, escape */
        const int n = sdc_ids_w(&cl, "red, (gold:1.3) logo, [dim] wall, ((deep)) floor, \\(raw\\)", wi, ww, 64);
        int ra[64], rg[64]; const int ng = sdc_ids(&cl, "gold", rg, 64), nd = sdc_ids(&cl, "dim", ra, 64), nn = sdc_ids(&cl, "red, gold logo, dim wall, deep floor, (raw)", wi + 32, 32);
        int okw = n == nn, g13 = 0, d09 = 0, d121 = 0;
        for (int k = 0; k < n; k++) { if (ww[k] > 1.29f && ww[k] < 1.31f) g13++; if (ww[k] > 0.90f && ww[k] < 0.92f) d09++; if (ww[k] > 1.20f && ww[k] < 1.22f) d121++; }
        okw = okw && g13 == ng && d09 == nd && d121 >= 1;
        printf("  %-34s %s  %d tokens, 1.3 x%d, 0.91 x%d, 1.21 x%d\n", "pesos de prompt (A1111)", okw ? "OK    " : "FALHOU", n, g13, d09, d121);
        bad += okw ? 0 : 1; }
      size_t nt = 0; float *rids = sdt_ref(refdir, "text_ids", &nt);       /* os ids do prompt da referencia, gravados como int32 */
      int32_t ids77[77]; memcpy(ids77, rids, 77 * 4);
      float *emb = xalloc((size_t)77 * 768, 4), *h1 = xalloc((size_t)77 * 768, 4), *remb = sdt_ref(refdir, "text_emb", NULL), *rh1 = sdt_ref(refdir, "text_hidden_layer1", NULL);
      t0 = sdt_now();
      if (!sdc_encode(&cl, ids77, emb, h1)) return 1;
      const double dt = sdt_now() - t0;
      bad += sdt_cmp_flat("CLIP depois da camada 1", h1, rh1, (size_t)77 * 768, dt);
      bad += sdt_cmp_flat("CLIP saida final [77x768]", emb, remb, (size_t)77 * 768, dt);
      free(rids); free(emb); free(h1); free(remb); free(rh1); }

    /* 6. UNet e ControlNet: passo de tempo, os 12+1 residuos do ControlNet, um passo da UNet sem e com eles (t = 500, semente 0 da referencia) */
    { float *lat0 = sdt_ref(refdir, "unet_latent_in", NULL), *ctx = sdt_ref(refdir, "text_emb", NULL), *img0 = sdt_ref(refdir, "control_image", NULL);
      if (!lat0 || !ctx || !img0) return 1;
      float *lat = sdt_to_nhwc(lat0, 4, 64 * 64), *img = sdt_to_nhwc(img0, 3, 512 * 512);
      float temb[1280], act[1280], *rt = sdt_ref(refdir, "unet_temb", NULL);
      t0 = sdt_now(); if (!sdn_temb(&base, "unet", 500.0f, temb, act)) return 1;
      bad += sdt_cmp_flat("UNet: embedding do passo (t = 500)", temb, rt, 1280, sdt_now() - t0);
      int hc, wc; float *cond = sdn_cond_embed(&ctrl, img, 512, 512, &hc, &wc);
      SdSkips res; float *mres = NULL; int hm, wm;
      t0 = sdt_now();
      if (!cond || !sdn_control(&ctrl, lat, 64, 64, 500.0f, ctx, 77, cond, 1.0f, &res, &mres, &hm, &wm)) { printf("  ControlNet: falhou\n"); return 1; }
      const double dc = sdt_now() - t0;
      for (int i = 0; i < res.n; i++) {
          char nm[64], what[80]; snprintf(nm, sizeof nm, "control_down_%02d", i); snprintf(what, sizeof what, "ControlNet residuo %d (%dx%dx%d)", i, res.H[i], res.W[i], res.C[i]);
          float *r = sdt_ref(refdir, nm, NULL); bad += sdt_compare(what, res.a[i], res.C[i], res.H[i] * res.W[i], r, i == res.n - 1 ? dc : 0.0); free(r);
      }
      { float *r = sdt_ref(refdir, "control_mid", NULL); bad += sdt_compare("ControlNet residuo do meio (8x8x1280)", mres, 1280, hm * wm, r, 0.0); free(r); }
      float *eps = sdn_alloc((size_t)64 * 64 * 4);
      t0 = sdt_now(); if (!sdn_unet(&base, lat, 64, 64, 500.0f, ctx, 77, NULL, NULL, eps)) { printf("  UNet: falhou\n"); return 1; }
      { float *r = sdt_ref(refdir, "unet_noise_pred", NULL); bad += sdt_compare("UNet, um passo (ruido previsto)", eps, 4, 64 * 64, r, sdt_now() - t0); free(r); }
      t0 = sdt_now(); if (!sdn_unet(&base, lat, 64, 64, 500.0f, ctx, 77, &res, mres, eps)) { printf("  UNet com ControlNet: falhou\n"); return 1; }
      { float *r = sdt_ref(refdir, "unet_noise_pred_control", NULL); bad += sdt_compare("UNet + residuos do ControlNet", eps, 4, 64 * 64, r, sdt_now() - t0); free(r); }
      free(lat0); free(ctx); free(img0); free(lat); free(img); free(rt); free(cond); free(eps); free(mres); sdn_skips_free(&res); }

    /* 7. VAE inteiro: o decodificador (latente 64x64 -> imagem 512x512) e o codificador (imagem 512x512 -> media do latente) */
    { float *l0 = sdt_ref(refdir, "vae_decode_in", NULL), *rimg = sdt_ref(refdir, "vae_decode_out", NULL), *i0 = sdt_ref(refdir, "vae_encode_in", NULL), *rm = sdt_ref(refdir, "vae_encode_mean", NULL);
      if (!l0 || !rimg || !i0 || !rm) return 1;
      float *lat = sdt_to_nhwc(l0, 4, 64 * 64), *im = sdt_to_nhwc(i0, 3, 512 * 512); int Ho, Wo;
      t0 = sdt_now(); float *img = sdv_decode(&base, lat, 64, 64, &Ho, &Wo);
      if (!img || Ho != 512 || Wo != 512) { printf("  VAE decodificador: falhou (%dx%d)\n", Ho, Wo); return 1; }
      bad += sdt_compare("VAE decodificador (64x64 -> 512x512)", img, 3, 512 * 512, rimg, sdt_now() - t0);
      t0 = sdt_now(); int h8, w8; float *mean = sdv_encode(&base, im, 512, 512, &h8, &w8);
      if (!mean || h8 != 64 || w8 != 64) { printf("  VAE codificador: falhou (%dx%d)\n", h8, w8); return 1; }
      bad += sdt_compare("VAE codificador (512x512 -> 64x64)", mean, 4, 64 * 64, rm, sdt_now() - t0);
      free(l0); free(rimg); free(i0); free(rm); free(lat); free(im); free(img); free(mean); }

    /* 8. amostrador Euler: sigmas, passos e 12 passos com um "modelo" falso e determinstico, contra o EulerDiscreteScheduler */
    { SdpEuler e; sdp_euler_setup(&e, 12, 1000, 0.00085, 0.012);
      float *rs = sdt_ref(refdir, "euler_sigmas", NULL), *rt = sdt_ref(refdir, "euler_timesteps", NULL), *noise = sdt_ref(refdir, "euler_noise", NULL), *rf = sdt_ref(refdir, "euler_x_final", NULL), *r5 = sdt_ref(refdir, "euler_x_step5", NULL);
      if (!rs || !rt || !noise || !rf || !r5) return 1;
      float s[13], t[12], x[4 * 16 * 16], x5[4 * 16 * 16];
      for (int i = 0; i < 13; i++) s[i] = (float)e.sigma[i];
      for (int i = 0; i < 12; i++) t[i] = (float)e.t[i];
      bad += sdt_cmp_flat("amostrador: sigmas (13)", s, rs, 13, 0.0);
      bad += sdt_cmp_flat("amostrador: passos de tempo (12)", t, rt, 12, 0.0);
      for (int i = 0; i < 4 * 16 * 16; i++) x[i] = noise[i] * (float)e.init_sigma;
      for (int k = 0; k < 12; k++) {
          const float sg = s[k], inv = 1.0f / sqrtf(sg * sg + 1.0f), dt = s[k + 1] - s[k];
          for (int i = 0; i < 4 * 16 * 16; i++) { const float xin = x[i] * inv, eps = 0.5f * xin * cosf(t[k] / 200.0f) + 0.1f; x[i] += eps * dt; }
          if (k == 4) memcpy(x5, x, sizeof x5);
      }
      bad += sdt_cmp_flat("amostrador: latente no passo 5", x5, r5, 4 * 16 * 16, 0.0);
      bad += sdt_cmp_flat("amostrador: latente depois de 12 passos", x, rf, 4 * 16 * 16, 0.0);
      free(rs); free(rt); free(noise); free(rf); free(r5); }

    /* 9. amostrador LCM: instantes de tempo e 4 passos com o "modelo" falso e os MESMOS ruidos do LCMScheduler do diffusers */
    { SdpLcm l; sdp_lcm_setup(&l, 4, 0.00085, 0.012);
      float *rt = sdt_ref(refdir, "lcm_timesteps", NULL), *x0 = sdt_ref(refdir, "lcm_x0", NULL), *nz = sdt_ref(refdir, "lcm_noises", NULL), *r1 = sdt_ref(refdir, "lcm_x_step1", NULL), *rf = sdt_ref(refdir, "lcm_x_final", NULL);
      if (!rt || !x0 || !nz || !r1 || !rf) return 1;
      float tsf[4], x[1024], x1[1024];
      for (int i = 0; i < 4; i++) tsf[i] = (float)l.t[i];
      bad += sdt_cmp_flat("LCM: instantes de tempo (4)", tsf, rt, 4, 0.0);
      memcpy(x, x0, sizeof x);
      for (int s = 0; s < 4; s++) {
          const int t = l.t[s], last = s == 3; double cs, co; sdp_lcm_scalings(t, &cs, &co);
          const double at = l.ac[t], sat = sqrt(at), sbt = sqrt(1.0 - at), ap = l.ac[last ? t : l.t[s + 1]];
          for (int i = 0; i < 1024; i++) {
              const float eps = 0.5f * x[i] * cosf((float)t / 200.0f) + 0.1f;
              const double xp = (x[i] - sbt * eps) / sat, d = co * xp + cs * x[i];
              x[i] = last ? (float)d : (float)(sqrt(ap) * d + sqrt(1.0 - ap) * nz[s * 1024 + i]);
          }
          if (s == 0) memcpy(x1, x, sizeof x1);
      }
      bad += sdt_cmp_flat("LCM: latente depois do passo 1", x1, r1, 1024, 0.0);
      bad += sdt_cmp_flat("LCM: latente depois de 4 passos", x, rf, 1024, 0.0);
      free(rt); free(x0); free(nz); free(r1); free(rf); }

    printf(bad ? "RESULTADO: %d teste(s) FALHARAM\n" : "RESULTADO: todos passaram\n", bad);
    return bad ? 1 : 0;
}
