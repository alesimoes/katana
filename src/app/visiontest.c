/* ---------- katana vision: conferencia do codificador de imagem, sem modelo de linguagem ----------
 *   vision decode IMG SAIDA.ppm            decodifica (PPM binario)
 *   vision resize IMG L A SAIDA.ppm        decodifica e redimensiona para L x A
 *   vision size A L TOKENS                 tamanho final pela regra de referencia
 *   vision stages PROJETOR IMG SAIDA [TOKENS]   grava cada etapa (recortes, embutido, blocos, fusao) em float32 para o oraculo
 *   vision info PROJETOR                   medidas do projetor
 *   vision bench PROJETOR IMG [TOKENS]     tempo e memoria de uma imagem
 *   vision positions GW GH ANTES DEPOIS    posicoes de tres eixos de um texto com uma imagem no meio
 *   vision fuzz IMG [N] [SEMENTE]          decodifica N versoes estragadas do arquivo (o erro esperado e uma mensagem, nunca queda) */
static FILE *vt_out;
static void vt_dump(const char *stage, int layer, const float *x, size_t n) {
    char nm[32]; memset(nm, 0, sizeof nm);
    snprintf(nm, sizeof nm, "%s%d", stage, layer);
    const uint64_t k = n;
    fwrite(nm, 1, 32, vt_out); fwrite(&k, 8, 1, vt_out); fwrite(x, 4, n, vt_out);
}
static int vt_ppm(const Img *im, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "nao abriu %s\n", path); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
    fwrite(im->rgb, 3, (size_t)im->w * im->h, f);
    return fclose(f) != 0;
}
static int vt_load_img(const char *path, Img *im) {
    char err[300]; size_t n;
    uint8_t *b = vis_slurp(path, &n, err, sizeof err);
    const int ok = b && img_decode(b, n, im, 0, err, sizeof err);
    free(b);
    if (!ok) fprintf(stderr, "error: %s: %s\n", path, err);
    return ok;
}
static int vision_main(int argc, char **argv) {
    char err[300] = "";
    Img im; memset(&im, 0, sizeof im);
    const char *cmd = argc ? argv[0] : "";
    if (!strcmp(cmd, "decode") && argc == 3) return !vt_load_img(argv[1], &im) || vt_ppm(&im, argv[2]);
    if (!strcmp(cmd, "resize") && argc == 5) return !vt_load_img(argv[1], &im) || !vr_resize(&im, atoi(argv[2]), atoi(argv[3])) || vt_ppm(&im, argv[4]);
    if (!strcmp(cmd, "size") && argc == 4) {
        int h, w;
        if (!vr_size(atoi(argv[1]), atoi(argv[2]), 32, 65536, atof(argv[3]) * 1024, &h, &w)) { printf("invalid\n"); return 0; }
        printf("%d %d\n", h, w);
        return 0;
    }
    if (!strcmp(cmd, "info") && argc == 2) {
        Vit *v = vit_load(argv[1], 0, err, sizeof err);
        if (!v) { fprintf(stderr, "error: %s\n", err); return 1; }
        printf("%d layers, width %d, heads %d, feed-forward %d, patch %d, merge %d, positions %dx%d, output %d, file %.0f MB\n",
               v->depth, v->d, v->heads, v->ff, v->ps, v->mg, v->side, v->side, v->outd, v->size / 1048576.0);
        vit_free(v);
        return 0;
    }
    if ((!strcmp(cmd, "stages") && argc >= 4) || (!strcmp(cmd, "bench") && argc >= 3)) {
        const int st = *cmd == 's', tk = argc > 3 + st ? atoi(argv[3 + st]) : 1000;
        Vit *v = vit_load(argv[1], 0, err, sizeof err);
        if (!v) { fprintf(stderr, "error: %s\n", err); return 1; }
        if (!vt_load_img(argv[2], &im)) return 1;
        const int s = v->ps * v->mg; int h, w, n;
        const double t0 = now_sec();
        if (!vr_size(im.h, im.w, s, 64.0 * s * s, (double)tk * s * s, &h, &w) || !vr_resize(&im, w, h)) { fprintf(stderr, "error: size\n"); return 1; }
        const double t1 = now_sec();
        if (st) { vt_out = fopen(argv[3], "wb"); if (!vt_out) return 1; vit_tap = vt_dump; }
        float *e = vit_encode(v, &im, &n);
        double rt, ra; vis_rss(&rt, &ra);
        fprintf(stderr, "%dx%d, %d tokens: resize %.3f s, encoder %.2f s (memory now: %.0f MB, anonymous %.0f MB)\n", w, h, n, t1 - t0, now_sec() - t1, rt, ra);
        if (st) fclose(vt_out);
        vt_free(e); vit_free(v); free(im.rgb);
        return 0;
    }
    if (!strcmp(cmd, "positions") && argc == 5) {
        static VisImg one; Model m; memset(&m, 0, sizeof m);
        one.gw = atoi(argv[1]); one.gh = atoi(argv[2]); one.nt = one.gw * one.gh;
        g_vis.v = &one; g_vis.n = 1;
        const int a = atoi(argv[3]), b = atoi(argv[4]), N = a + one.nt + b;
        m.cfg.ctx = N + 4;
        int *t = xalloc((size_t)N + 1, sizeof(int));
        for (int i = 0; i < N; i++) t[i] = i >= a && i < a + one.nt ? VIS_BASE | (i - a) : 5;
        for (int i = 0; i < N; ) { const int nb = i % 7 + 1 < N - i ? i % 7 + 1 : N - i; vis_seq(&m, t + i, nb, i); i += nb; }      /* em pedacos: o resultado nao depende do corte */
        for (int i = 0; i < N; i++) printf("%d %d %d\n", vis_rp[3 * i], vis_rp[3 * i + 1], vis_rp[3 * i + 2]);
        return 0;
    }
    if (!strcmp(cmd, "fuzz") && argc >= 2) {
        size_t n; uint8_t *b = vis_slurp(argv[1], &n, err, sizeof err);
        if (!b || n < 8) { fprintf(stderr, "error: %s\n", err); return 1; }
        const long N = argc > 2 ? atol(argv[2]) : 1000; uint64_t r = argc > 3 ? strtoull(argv[3], NULL, 10) : 1;
        long good = 0; double worst = 0;
#define RND() (r ^= r << 13, r ^= r >> 7, r ^= r << 17, r)
        r |= 1;
        for (long i = 0; i < N; i++) {
            uint8_t *t = malloc(n); size_t k = n;      /* tamanho exato: o verificador de memoria pega leitura alem do fim */
            memcpy(t, b, n);
            const int mode = (int)(RND() % 4), ch = 1 + (int)(RND() % 8);
            if (mode == 0) { k = 1 + RND() % n; t = realloc(t, k); }
            else for (int c = 0; c < ch; c++) {
                const size_t at = mode == 1 ? RND() % n : mode == 2 ? RND() % (n < 700 ? n : 700) : RND() % n;
                if (mode == 3 && at + 4 < n) { for (int q = 0; q < 4; q++) t[at + q] = RND() & 1 ? 0xFF : (uint8_t)RND(); }
                else t[at] = (uint8_t)RND();
            }
            if (img_kind(t, k) && *img_kind(t, k) == 'p' && (RND() & 1))      /* PNG: refaz as somas, senao tudo para no CRC */
                for (size_t p = 8; p + 12 <= k; ) {
                    const uint32_t l = im_be32(t + p);
                    if (l > k - p - 12) break;
                    const uint32_t c = im_crc32(t + p + 4, (size_t)l + 4);
                    for (int q = 0; q < 4; q++) t[p + 8 + l + q] = (uint8_t)(c >> (24 - 8 * q));
                    p += (size_t)l + 12;
                }
            im_lax = (int)(RND() & 1);
            const double t0 = now_sec();
            Img g; char e2[200] = "";
            if (img_decode(t, k, &g, 0, e2, sizeof e2)) { good++; volatile uint8_t x = g.rgb[(size_t)g.w * g.h * 3 - 1]; (void)x; free(g.rgb); }
            else if (!e2[0]) { printf("error without a message\n"); return 1; }
            const double dt = now_sec() - t0;
            if (dt > worst) worst = dt;
            free(t);
        }
        printf("%ld damaged files: %ld decoded, %ld refused, slowest %.3f s\n", N, good, N - good, worst);
        free(b);
        return 0;
    }
    fprintf(stderr, "use: katana vision info|decode|resize|size|stages|bench|positions|fuzz ... (src/app/visiontest.c)\n");
    return 1;
}
