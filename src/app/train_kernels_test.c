/* katana kerneltest: compara os kernels em blocos (train_kernels.c) com os lacos simples, nos formatos reais dos modelos,
   com linhas e tokens em quantidades que nao sao multiplos dos blocos. Sai com 1 se algo diverge. */

static uint32_t tkt_rng = 12345u;
static float tkt_rand(void) { tkt_rng = tkt_rng * 1664525u + 1013904223u; return ((tkt_rng >> 8) & 0xFFFF) / 32768.0f - 1.0f; }

static float tkt_maxrel(const float *a, const float *b, size_t n) {
    double num = 0, den = 1e-30;
    for (size_t i = 0; i < n; i++) { const double d = fabs((double)a[i] - b[i]); if (d > num) num = d; if (fabs(b[i]) > den) den = fabs(b[i]); }
    return (float)(num / den);
}

static int tk_selftest(void) {
    pool_init();
    struct { const char *name; uint32_t type; size_t blk, per; } fm[] = {
        { "Q8_0", T_Q8_0, 34, 32 }, { "Q4_K", T_Q4_K, 144, 256 }, { "Q5_K", T_Q5_K, 176, 256 },
        { "Q6_K", T_Q6_K, 210, 256 }, { "Q5_1", T_Q5_1, 24, 32 }, { "F32", T_F32, 4, 1 }, { "Q4_0", T_Q4_0, 18, 32 } };
    const size_t C = 768, R = 61;
    int bad = 0;
    for (size_t f = 0; f < sizeof fm / sizeof fm[0]; f++) {
        Mat m; memset(&m, 0, sizeof m);
        m.type = fm[f].type; m.stride = C / fm[f].per * fm[f].blk;
        uint8_t *q = (uint8_t *)xalloc(R * m.stride, 1);
        for (size_t i = 0; i < R * m.stride; i++) { tkt_rng = tkt_rng * 1664525u + 1013904223u; q[i] = (uint8_t)(tkt_rng >> 16); }
        for (size_t r = 0; r < R; r++) for (size_t b = 0; b < C / fm[f].per; b++) {        /* escalas fp16 validas e pequenas */
            uint8_t *blk = q + r * m.stride + b * fm[f].blk;
            const uint16_t h = (uint16_t)(0x2800 + (tkt_rng >> 20 & 0x3FF)), h2 = (uint16_t)(0x2000 + (tkt_rng >> 14 & 0x3FF));
            switch (fm[f].type) {
            case T_Q8_0: case T_Q4_0: memcpy(blk, &h, 2); break;
            case T_Q4_K: case T_Q5_K: case T_Q5_1: memcpy(blk, &h, 2); memcpy(blk + 2, &h2, 2); break;
            case T_Q6_K: memcpy(blk + 208, &h, 2); break;
            default: break;
            }
            tkt_rng = tkt_rng * 1664525u + 1013904223u;
        }
        if (fm[f].type == T_F32) { m.f = (float *)q; for (size_t i = 0; i < R * C; i++) m.f[i] = tkt_rand(); }
        else m.q = q;
        TPar p; memset(&p, 0, sizeof p);
        p.mat = &m; p.rows = R; p.cols = C;
        /* desquantizacao: SIMD x original */
        if (!m.f) {
            float *a = (float *)xalloc(C, 4), *b = (float *)xalloc(C, 4);
            float e = 0;
            for (size_t r = 0; r < R; r++) { dequant(m.type, q + r * m.stride, a, C); tk_dequant(m.type, q + r * m.stride, b, C); const float x = tkt_maxrel(b, a, C); if (x > e) e = x; }
            printf("%-5s desquantizacao   erro relativo maximo %.2e\n", fm[f].name, e);
            if (e > 1e-5f) bad = 1;
            free(a); free(b);
        }
        for (int B = 1; B <= 9; B += (B < 5 ? 1 : 4)) {
            float *x = (float *)xalloc((size_t)B * C, 4), *dy = (float *)xalloc((size_t)B * R, 4);
            for (size_t i = 0; i < (size_t)B * C; i++) x[i] = tkt_rand();
            for (size_t i = 0; i < (size_t)B * R; i++) dy[i] = (i % 7 == 3) ? 0.0f : tkt_rand();
            float *o1 = (float *)xalloc((size_t)B * R, 4), *o2 = (float *)xalloc((size_t)B * R, 4);
            TMvJob jm = { &p, o1, x, NULL, B };
            pool_for(tmv_job, &jm, (int)R, 16);
            tk_mv(&p, o2, x, B);
            const float e1 = tkt_maxrel(o2, o1, (size_t)B * R);
            float *d1 = (float *)xalloc((size_t)B * C, 4), *d2 = (float *)xalloc((size_t)B * C, 4);
            for (size_t i = 0; i < (size_t)B * C; i++) d1[i] = d2[i] = 0.5f;
            TMtJob jt = { &p, d1, dy, B };
            pool_for(tmt_job, &jt, (int)C, (int)C / pool.n + 1);
            tk_mvT(&p, d2, dy, B);
            const float e2 = tkt_maxrel(d2, d1, (size_t)B * C);
            float e3 = 0;
            if (m.f) {                                   /* gradiente: so para parametros em float */
                float *g1 = (float *)xalloc(R * C, 4), *g2 = (float *)xalloc(R * C, 4);
                for (size_t i = 0; i < R * C; i++) g1[i] = g2[i] = 0.25f;
                p.g = g1; TGJob jg = { &p, dy, x, B }; pool_for(tg_job, &jg, (int)R, 16);
                p.g = g2; tk_grad(&p, dy, x, B); p.g = NULL;
                e3 = tkt_maxrel(g2, g1, R * C);
                free(g1); free(g2);
            }
            printf("%-5s B=%d  W x %.2e   W^T dy %.2e   grad %.2e\n", fm[f].name, B, e1, e2, e3);
            if (e1 > 1e-4f || e2 > 1e-4f || e3 > 1e-4f) bad = 1;
            free(x); free(dy); free(o1); free(o2); free(d1); free(d2);
        }
        free(q);
    }
    printf(bad ? "kerneltest: FALHOU\n" : "kerneltest: ok\n");
    return bad;
}

/* katana kerneltest bench: microtempo de um especialista (Q4_K 512 x 2048, B tokens) por chamada */
static int tk_bench(int B, int reps) {
    pool_init();
    const size_t C = 2048, R = 512;
    Mat m; memset(&m, 0, sizeof m); m.type = T_Q4_K; m.stride = C / 256 * 144;
    uint8_t *q = (uint8_t *)xalloc(R * m.stride * 64, 1);          /* 64 "especialistas" para nao caber no cache */
    for (size_t i = 0; i < R * m.stride * 64; i++) { tkt_rng = tkt_rng * 1664525u + 1013904223u; q[i] = (uint8_t)(tkt_rng >> 16); }
    for (size_t o = 0; o < R * 64; o++) { uint16_t h = 0x2800; memcpy(q + o * m.stride + 0, &h, 2); memcpy(q + o * m.stride + 2, &h, 2); }
    TPar p; memset(&p, 0, sizeof p); p.rows = R; p.cols = C;
    float *x = (float *)xalloc((size_t)B * C, 4), *dy = (float *)xalloc((size_t)B * R, 4), *o = (float *)xalloc((size_t)B * R, 4), *dx = (float *)xalloc((size_t)B * C, 4);
    for (size_t i = 0; i < (size_t)B * C; i++) x[i] = tkt_rand();
    for (size_t i = 0; i < (size_t)B * R; i++) dy[i] = tkt_rand();
    for (int mode = 0; mode < 6; mode++) {
        double best = 1e9;
        for (int rd = 0; rd < 9; rd++) {
        const double t0 = now_sec();
        for (int i = 0; i < reps; i++) {
            m.q = q + (size_t)(i % 64) * R * m.stride; p.mat = &m;
            if (mode == 0) { TMvJob jm = { &p, o, x, NULL, B }; pool_for(tmv_job, &jm, (int)R, 16); }
            else if (mode == 1) tk_mv(&p, o, x, B);
            else if (mode == 2) { TMtJob jt = { &p, dx, dy, B }; pool_for(tmt_job, &jt, (int)C, (int)C / pool.n + 1); }
            else if (mode == 3) tk_mvT(&p, dx, dy, B);
            else if (mode == 4) { float *b = tk_scratch(R * C); for (size_t r = 0; r < R; r++) tk_dequant(m.type, m.q + r * m.stride, b + r * C, C); }
            else { volatile int z = 0; (void)z; pool_for(tk_red_job, &(TkRedJob){ dx, dx, 8, 0 }, 8, 1); }
        }
        const double dt = (now_sec() - t0) / reps * 1e6; if (dt < best) best = dt;
        }
        const char *nm[] = { "old W x", "new W x", "old W^T dy", "new W^T dy", "dequant 1 thread (512 linhas)", "pool_for vazio" };
        printf("%-30s %8.1f us/chamada\n", nm[mode], best);
    }
    return 0;
}
