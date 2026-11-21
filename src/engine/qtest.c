/* ---------- katana qtest: teste dos tipos de quantizacao sem modelo ----------
   1) valores conhecidos: blocos montados a mao decodificam para os valores esperados;
   2) para cada tipo, uma matriz de blocos aleatorios (escalas em faixa sa) e um x aleatorio:
      produto exato (decodifica + double) contra o produto da CPU em float, o rapido (inteiro,
      quando o tipo tem) e o da GPU (KATANA_BACKEND=vulkan ou OpenCL; KATANA_CPU=1 pula). */
static int qtest_gpu_init(int rows, int cols) {
    if (getenv("KATANA_CPU") && atoi(getenv("KATANA_CPU"))) return 0;
    const char *be = getenv("KATANA_BACKEND");
    gpu.seg_target = 16384;
    if (!be || strcmp(be, "opencl") != 0) {
        KvkInfo I;
        if (!kvk_init_wait(&I)) { fprintf(stderr, "Vulkan: indisponivel (%s)\n", kvk_error()); return 0; }
        gpu.vk = 1; snprintf(gpu.name, sizeof gpu.name, "%s", I.name); gpu.vram = I.vram_bytes;
        gpu.vxb = kvk_buf_create((size_t)cols * sizeof(float), KVK_USAGE_RW);
        gpu.vob = kvk_buf_create(((size_t)rows + 128) * sizeof(float), KVK_USAGE_RW);
        return gpu.vxb.b && gpu.vob.b;
    }
    if (!gpu_init()) return 0;
    gpu.cfg_rows = 1; gpu.cfg_rowsb = 2; gpu.cfg_seg = 16384;
    if (!gpu_build_all()) return 0;
    int32_t e1 = 0, e2 = 0;
    gpu.xbuf = cl.CreateBuffer(gpu.ctx, 1, (size_t)cols * sizeof(float), NULL, &e1);
    gpu.obuf = cl.CreateBuffer(gpu.ctx, 1, ((size_t)rows + 128) * sizeof(float), NULL, &e2);
    return !e1 && !e2;
}

static int qtest_known(void) {
    static const float e2m1[8] = { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f };
    uint8_t b[256]; float o[256];
    int bad = 0;
    /* MXFP4: expoente 127 (escala 1); nibble j -> E2M1 j (bit 3 = sinal); alto = 15 - j */
    b[0] = 127;
    for (int j = 0; j < 16; j++) b[1 + j] = (uint8_t)(j | ((15 - j) << 4));
    dequant(T_MXFP4, b, o, 32);
    for (int j = 0; j < 16; j++) {
        const float e0 = (j & 8 ? -1 : 1) * e2m1[j & 7], e1 = ((15 - j) & 8 ? -1 : 1) * e2m1[(15 - j) & 7];
        if (o[j] != e0 || o[j + 16] != e1) bad |= 1;
    }
    b[0] = 130;     /* escala 8 */
    dequant(T_MXFP4, b, o, 32);
    if (o[7] != 48.0f || o[16] != -48.0f) bad |= 1;     /* 0x7 = +6 e 0xF = -6, vezes 2^3 */
    /* IQ4_NL: d = 1 -> a tabela */
    b[0] = 0x00; b[1] = 0x3C;
    for (int j = 0; j < 16; j++) b[2 + j] = (uint8_t)(j | (j << 4));
    dequant(T_IQ4_NL, b, o, 32);
    for (int j = 0; j < 16; j++) if (o[j] != q_iq4nl_vals[j] || o[j + 16] != q_iq4nl_vals[j]) bad |= 2;
    /* IQ4_XS: d = 1, todas as escalas 33 (dl = 1) */
    memset(b, 0, sizeof b); b[1] = 0x3C; b[2] = 0xAA; b[3] = 0xAA; b[4] = b[5] = b[6] = b[7] = 0x11;
    for (int j = 0; j < 128; j++) b[8 + j] = (uint8_t)((j & 15) | ((15 - (j & 15)) << 4));
    dequant(T_IQ4_XS, b, o, 256);
    for (int ib = 0; ib < 8; ib++) for (int j = 0; j < 16; j++)
        if (o[ib * 32 + j] != q_iq4nl_vals[j] || o[ib * 32 + 16 + j] != q_iq4nl_vals[15 - j]) bad |= 4;
    /* Q2_K: escala 1, minimo 2, d = dmin = 1, q = 3 -> 1 */
    memset(b, 0x21, 16); memset(b + 16, 0xFF, 64); b[80] = 0; b[81] = 0x3C; b[82] = 0; b[83] = 0x3C;
    dequant(T_Q2_K, b, o, 256);
    for (int j = 0; j < 256; j++) if (o[j] != 1.0f) bad |= 8;
    /* IQ2_XXS: tudo zero com d = 1 -> grade 0 (8 x 8), escala 1/8 -> 1 */
    memset(b, 0, 66); b[1] = 0x3C;
    dequant(T_IQ2_XXS, b, o, 256);
    for (int j = 0; j < 256; j++) if (o[j] != 1.0f) bad |= 16;
    /* BF16 */
    b[0] = 0x80; b[1] = 0x3F; b[2] = 0x40; b[3] = 0xC0;
    dequant(T_BF16, b, o, 2);
    if (o[0] != 1.0f || o[1] != -3.0f) bad |= 32;
    /* TQ2_0: 2 bits 0,1,2 -> -1,0,1 (d = 1) */
    memset(b, 0x24, 64); b[64] = 0; b[65] = 0x3C;     /* 0x24 = 00 10 01 00 */
    dequant(T_TQ2_0, b, o, 256);
    if (o[0] != -1.0f || o[32] != 0.0f || o[64] != 1.0f || o[96] != -1.0f) bad |= 64;
    printf("valores conhecidos (MXFP4, IQ4_NL, IQ4_XS, Q2_K, IQ2_XXS, BF16, TQ2_0): %s", bad ? "FALHOU" : "OK");
    if (bad) printf(" (mascara %d)", bad);
    printf("\n");
    return bad;
}

static int qtest(void) {
    static const struct { uint32_t t; const char *nome; int f16[2]; } TT[] = {
        { T_Q4_0, "Q4_0", { 0, -1 } }, { T_Q4_1, "Q4_1", { 0, 2 } }, { T_Q5_0, "Q5_0", { 0, -1 } }, { T_Q5_1, "Q5_1", { 0, 2 } },
        { T_Q8_0, "Q8_0", { 0, -1 } }, { T_Q2_K, "Q2_K", { 80, 82 } }, { T_Q3_K, "Q3_K", { 108, -1 } }, { T_Q4_K, "Q4_K", { 0, 2 } },
        { T_Q5_K, "Q5_K", { 0, 2 } }, { T_Q6_K, "Q6_K", { 208, -1 } }, { T_IQ4_NL, "IQ4_NL", { 0, -1 } }, { T_IQ4_XS, "IQ4_XS", { 0, -1 } },
        { T_MXFP4, "MXFP4", { -1, -1 } }, { T_IQ2_XXS, "IQ2_XXS", { 0, -1 } }, { T_IQ2_XS, "IQ2_XS", { 0, -1 } }, { T_IQ2_S, "IQ2_S", { 0, -1 } },
        { T_IQ3_XXS, "IQ3_XXS", { 0, -1 } }, { T_IQ3_S, "IQ3_S", { 0, -1 } }, { T_IQ1_S, "IQ1_S", { 0, -1 } }, { T_IQ1_M, "IQ1_M", { -1, -1 } },
        { T_TQ1_0, "TQ1_0", { 52, -1 } }, { T_TQ2_0, "TQ2_0", { 64, -1 } } };
    const int rows = 200, cols = 2048;      /* 200 linhas: testa a sobra do grupo de 32 na GPU */
    int fails = qtest_known();
    const int has_gpu = qtest_gpu_init(rows, cols);
    printf("GPU: %s\n", has_gpu ? (gpu.vk ? "Vulkan" : "OpenCL") : "nenhuma (so CPU)");
    float *x = xalloc(cols, sizeof(float)), *row = xalloc(cols, sizeof(float)), *og = xalloc(rows, sizeof(float));
    float sx[MAX_SX];
    srand(4321);
    for (int i = 0; i < cols; i++) x[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    block_sums(sx, x, cols);
    printf("%-8s %-10s %-10s %-10s %-10s\n", "tipo", "CPU float", "CPU rapida", "GPU", "");
    for (size_t k = 0; k < sizeof TT / sizeof TT[0]; k++) {
        const uint32_t t = TT[k].t;
        const int unit = qunit(t);
        const size_t ub = qbytes(t, unit), stride = qbytes(t, cols);
        uint8_t *buf = xalloc((size_t)rows * stride, 1);
        for (size_t i = 0; i < (size_t)rows * stride; i++) buf[i] = (uint8_t)(rand() >> 7);
        for (size_t blk = 0; blk < (size_t)rows * stride / ub; blk++) {
            uint8_t *p = buf + blk * ub;
            for (int f = 0; f < 2; f++) if (TT[k].f16[f] >= 0) {     /* f16 positivo entre 2^-8 e 2^-7 */
                const uint16_t h = (uint16_t)(0x1C00 + rand() % 0x400);
                memcpy(p + TT[k].f16[f], &h, 2);
            }
            if (t == T_MXFP4) p[0] = (uint8_t)(118 + rand() % 16);
            if (t == T_IQ1_M) {     /* escala f16 espalhada nos 4 bits altos das 4 palavras de escala */
                const uint16_t h = (uint16_t)(0x1C00 + rand() % 0x400);
                for (int i = 0; i < 4; i++) {
                    uint16_t w; memcpy(&w, p + 48 + 2 * i, 2);
                    w = (uint16_t)((w & 0x0fff) | (((h >> (4 * i)) & 0xf) << 12));
                    memcpy(p + 48 + 2 * i, &w, 2);
                }
            }
        }
        Mat W; memset(&W, 0, sizeof W);
        W.q = buf; W.type = t; W.stride = stride;
        double e_f = 0, e_r = 0, e_g = -1;
        for (int r = 0; r < rows; r++) {
            mat_row(row, &W, r, cols);
            double ref = 0, mag = 0;
            for (int j = 0; j < cols; j++) { ref += (double)row[j] * x[j]; mag += fabs((double)row[j] * x[j]); }
            cpu_exact = 1;
            double e = fabs(dot_row(&W, r, x, sx, cols) - ref) / (mag + 1e-30);
            cpu_exact = 0;
            if (e > e_f) e_f = e;
            if (mat_int(&W)) xq_prepare(x, cols);
            e = fabs(dot_row(&W, r, x, sx, cols) - ref) / (mag + 1e-30);
            if (e > e_r) e_r = e;
        }
        if (has_gpu) {
            gpu_upload(&W, rows, cols, TT[k].nome);
            if (W.gbuf) {
                gpu.test_mv = 1; matvec(og, &W, x, rows, cols); gpu.test_mv = 0;
                e_g = 0;
                for (int r = 0; r < rows; r++) {
                    mat_row(row, &W, r, cols);
                    double ref = 0, mag = 0;
                    for (int j = 0; j < cols; j++) { ref += (double)row[j] * x[j]; mag += fabs((double)row[j] * x[j]); }
                    const double e = fabs(og[r] - ref) / (mag + 1e-30);
                    if (e > e_g) e_g = e;
                }
            }
        }
        const int ok = e_f < 1e-4 && e_r < 1e-4 && e_g < 1e-4;
        char gs[32];
        if (e_g < 0) snprintf(gs, sizeof gs, "%s", has_gpu ? "CPU (sem kernel)" : "-");
        else snprintf(gs, sizeof gs, "%.2e", e_g);
        printf("%-8s %-10.2e %-10.2e %-16s %s%s\n", TT[k].nome, e_f, e_r, gs, ok ? "OK" : "FALHOU",
               mat_int(&W) ? " (rapida = inteiro)" : "");
        if (!ok) fails++;
        free(buf);      /* o buffer da GPU fica ate o fim do processo */
    }
    printf("qtest: %s\n", fails ? "FALHOU" : "OK");
    free(x); free(row); free(og);
    return fails ? 1 : 0;
}

static int selftest(Model *m) {
    const Config *c = &m->cfg;
    const int d = c->dim, kvd = c->n_kv * c->hd, H = c->hidden;
    double worst = 0.0;
    srand(1234);
    selftest_mat("tok_emb", &m->tok_emb, c->vocab, d, &worst);
    for (int l = 0; l < c->n_layers; l += c->n_layers - 1 > 0 ? c->n_layers - 1 : 1) {
        const Layer *L = &m->layers[l];
        printf(" camada %d\n", l);
        if (c->g4) {    /* largura de cabeca e cabecas kv por camada */
            const int qd = c->n_heads * L->hd_l, hk = L->nkv_l * L->hd_l;
            selftest_mat("attn_q", &L->wq, qd, d, &worst);
            selftest_mat("attn_k", &L->wk, hk, d, &worst);
            if (L->wv.q) selftest_mat("attn_v", &L->wv, hk, d, &worst);
            selftest_mat("attn_out", &L->wo, d, qd, &worst);
            const int Hg = L->eg ? c->hidden_dense : H;
            selftest_mat("ffn_gate", &L->w_gate, Hg, d, &worst);
            selftest_mat("ffn_up", &L->w_up, Hg, d, &worst);
            selftest_mat("ffn_down", &L->w_down, d, Hg, &worst);
            if (L->eg) {
                selftest_mat("exp0_gate", &L->eg[0], H, d, &worst);
                selftest_mat("exp0_down", &L->ed[0], d, H, &worst);
            }
            continue;
        }
        selftest_mat("attn_q", &L->wq, (c->attn_gated ? 2 : 1) * m->cfg.n_heads * m->cfg.hd, d, &worst);
        if (L->lin) {
            const int C = 2 * c->ssm_groups * c->ssm_state + c->ssm_inner;
            selftest_mat("ssm_qkv", &L->wqkv, C, d, &worst);
            selftest_mat("ssm_z", &L->wz, c->ssm_inner, d, &worst);
            selftest_mat("ssm_out", &L->wssm_out, d, c->ssm_inner, &worst);
        }
        selftest_mat("attn_k", &L->wk, kvd, d, &worst);
        selftest_mat("attn_v", &L->wv, kvd, d, &worst);
        const int Hl = c->n_expert && !L->eg ? c->hidden_dense : H;
        selftest_mat("attn_out", &L->wo, d, m->cfg.n_heads * (c->mla ? c->hd_v : m->cfg.hd), &worst);
        if (c->mla) selftest_mat("attn_kv_a", &L->wkv_a, c->mla + c->rope_dim, d, &worst);
        selftest_mat("ffn_gate", &L->w_gate, Hl, d, &worst);
        selftest_mat("ffn_up", &L->w_up, Hl, d, &worst);
        selftest_mat("ffn_down", &L->w_down, d, Hl, &worst);
        if (L->sg.q) {
            selftest_mat("shexp_gate", &L->sg, c->n_shared * H, d, &worst);
            selftest_mat("shexp_down", &L->sd, d, c->n_shared * H, &worst);
        }
    }
    int ok = worst < 1e-4 && worst_int < 1e-4;
    printf("produtos: %s (pior erro relativo: exato %.2e, CPU inteiro %.2e)\n", ok ? "OK" : "FALHOU", worst, worst_int);
    if (gpu.vk && !vkg.on) printf("Vulkan: grafo na CPU (verificacoes de grafo completo puladas)\n");

    if (gpu.n_gl > 0 || vkg.on || gpu.mv_host) {   /* mesma sequencia pela GPU (em lote e token a token) e pela CPU pura */
        const int V = c->vocab, N = 23;
        int toks[23];
        float *ref = xalloc(V, sizeof(float)), *o = NULL;
        for (int i = 0; i < N; i++) toks[i] = (int)((1000003u * (i + 1)) % (unsigned)V);
        gpu.force_cpu = 1; cpu_exact = 1;
        for (int i = 0; i < N; i++) o = forward(m, toks[i], i, i == N - 1);
        gpu.force_cpu = 0; cpu_exact = 0;
        memcpy(ref, o, V * sizeof(float));
        if (getenv("KATANA_QN_DBG") && c->ssm_inner) {   /* medida: residual por camada e escolhidos, lote x CPU exata */
            dbg_x = xalloc((size_t)N * c->n_layers * c->dim, 4); dbg_sel = xalloc((size_t)N * c->n_layers * c->n_used, 4);
            dbg_on = 1; gpu.force_cpu = 1; cpu_exact = 1;
            for (int i = 0; i < N; i++) o = forward(m, toks[i], i, i == N - 1);
            gpu.force_cpu = 0; cpu_exact = 0;
            o = forward_seq(m, toks, N, 0, 1);
            dbg_on = 0;
        }
        for (int mode = 0; mode < 4; mode++) {
            if (mode == 3 && !(vkg.on && vkb.ok)) continue;
            const long p0 = vkb.passes;
            if (mode == 0) o = forward_seq(m, toks, N, 0, 1);
            else if (mode == 3) { setenv_("KATANA_VK_CAP", "16"); o = forward_seq(m, toks, N, 0, 1); setenv_("KATANA_VK_CAP", NULL); }
            else if (mode == 1) for (int i = 0; i < N; i++) o = forward(m, toks[i], i, i == N - 1);
            else { gpu.force_cpu = 1; for (int i = 0; i < N; i++) o = forward(m, toks[i], i, i == N - 1); gpu.force_cpu = 0; }
            int ag = 0, ac = 0; double md = 0.0, mr = 0.0;
            for (int i = 0; i < V; i++) {
                if (o[i] > o[ag]) ag = i;
                if (ref[i] > ref[ac]) ac = i;
                if (fabs(o[i] - ref[i]) > md) md = fabs(o[i] - ref[i]);
                if (fabs(ref[i]) > mr) mr = fabs(ref[i]);
            }
            int e2e = ag == ac && md < (gpu.kv16 ? 1.0 : 0.05);
            if (vkb.ok && (mode == 0 || mode == 3)) printf("  (%ld passadas em lote)\n", vkb.passes - p0);
            printf("%s x CPU exata (%d tokens): maior diferenca de logit=%.6f, argmax %d x %d: %s\n",
                   mode == 0 ? (vkg.on ? (vkb.ok ? "GPU (Vk, lote)   " : "GPU (Vk, 1 a 1)  ") : "GPU em lote      ") : mode == 1 ? "GPU token a token" : mode == 3 ? "GPU (Vk, lote 16)" : "CPU inteira      ", N, md, ag, ac, e2e ? "OK" : "FALHOU");
            ok = ok && e2e;
        }
        free(ref);
    }
    if (!c->n_expert && !c->ssm_inner) {     /* camadas em lote na CPU x as mesmas camadas token a token (produto inteiro nos dois); o qwen35 denso nao usa cpu_layers_n */
        const int N = 23, V = c->vocab;
        float *X = xalloc((size_t)N * d, sizeof(float)), *Y = xalloc((size_t)N * d, sizeof(float));
        gpu.force_cpu = 1; cpu_exact = 0;
        for (int t = 0; t < N; t++) mat_row(X + (size_t)t * d, &m->tok_emb, (int)((1000003u * (t + 1)) % (unsigned)V), d);
        memcpy(Y, X, (size_t)N * d * sizeof(float));
        cpu_layers_n(m, 0, c->n_layers, 0, N, X);
        for (int t = 0; t < N; t++) {
            memcpy(m->x, Y + (size_t)t * d, d * sizeof(float));
            cpu_layers(m, 0, c->n_layers, t);
            memcpy(Y + (size_t)t * d, m->x, d * sizeof(float));
        }
        gpu.force_cpu = 0;
        double md = 0.0, mx = 0.0;
        for (size_t i = 0; i < (size_t)N * d; i++) {
            if (fabs(X[i] - Y[i]) > md) md = fabs(X[i] - Y[i]);
            if (fabs(Y[i]) > mx) mx = fabs(Y[i]);
        }
        const int okb = md <= 1e-3 * mx;
        printf("CPU em lote       x CPU token a token (%d tokens): maior diferenca de estado=%.3e (maximo %.3e): %s\n", N, md, mx, okb ? "OK" : "FALHOU");
        ok = ok && okb;
        free(X); free(Y);
    }
    return ok ? 0 : 1;
}

/* comando oculto `vkcheck modelo texto.txt [ngen]`: processa o texto (ate ~600 tokens) em lote e depois token a token em Vulkan
   (a mesma sequencia, caches KV refeitos) e compara os logits finais e os proximos ngen tokens gulosos */
static char *read_text(const char *path);
static int vkcheck(Model *m, const char *path, int ngen, int ntok) {
    const Config *c = &m->cfg;
    const int V = c->vocab;
    if (!vkg.on || !vkb.ok) { printf("vkcheck: lote Vulkan inativo\n"); return 1; }
    if (getenv("KATANA_VKC_EXACT")) cpu_exact = 1;      /* medida: especialistas frios da CPU em float exato */
    Ids ids = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    encode(&m->tk, read_text(path), &ids);
    int n = ids.n < ntok ? ids.n : ntok;
    if (n + ngen >= c->ctx) n = c->ctx - ngen - 1;
    float *lgs[2] = { xalloc(V, sizeof(float)), xalloc(V, sizeof(float)) };
    int *gen[2] = { xalloc(ngen, sizeof(int)), xalloc(ngen, sizeof(int)) };
    double tp[2];
    for (int run = 0; run < 2; run++) {
        vkb.ok = run == 0;              /* 0: lote; 1: um token por vez */
        double t0 = now_sec();
        float *lg = forward_seq(m, ids.ids, n, 0, 1);
        tp[run] = now_sec() - t0;
        memcpy(lgs[run], lg, V * sizeof(float));
        int pos = n;
        for (int g = 0; g < ngen; g++) {
            int best = 0;
            for (int i = 1; i < V; i++) if (lg[i] > lg[best]) best = i;
            gen[run][g] = best;
            lg = forward(m, best, pos++, 1);
        }
    }
    vkb.ok = 1;
    double md = 0, mx = 0;
    int a0 = 0, a1 = 0;
    for (int i = 0; i < V; i++) {
        if (lgs[0][i] > lgs[0][a0]) a0 = i;
        if (lgs[1][i] > lgs[1][a1]) a1 = i;
        if (fabs(lgs[0][i] - lgs[1][i]) > md) md = fabs(lgs[0][i] - lgs[1][i]);
        if (fabs(lgs[1][i]) > mx) mx = fabs(lgs[1][i]);
    }
    int first = -1;
    for (int g = 0; g < ngen; g++) if (gen[0][g] != gen[1][g]) { first = g; break; }
    printf("vkcheck: prompt de %d tokens: lote %.1f tok/s, token a token %.1f tok/s\n", n, n / tp[0], n / tp[1]);
    printf("  logits finais: maior diferenca %.6f (maior |logit| %.2f), argmax lote %d x 1 a 1 %d\n", md, mx, a0, a1);
    printf("  proximos %d tokens gulosos: %s", ngen, first < 0 ? "todos iguais" : "primeira diferenca no token");
    if (first >= 0) printf(" %d (lote %d x 1 a 1 %d)", first, gen[0][first], gen[1][first]);
    printf("\n");
    return (a0 == a1 && first < 0) ? 0 : 2;
}

/* mede prompt e geracao com tokens sinteticos, depois de aquecer (pesos ja na VRAM) */
static int bench(Model *m, int n_prompt, int n_gen) {
    const Config *c = &m->cfg;
    if (n_prompt < 1) n_prompt = 1;
    if (n_prompt + n_gen >= c->ctx) { n_prompt = c->ctx / 2; n_gen = c->ctx / 4; }
    int *toks = xalloc(n_prompt, sizeof(int));
    for (int i = 0; i < n_prompt; i++) toks[i] = (int)((1000003u * (i + 1)) % (unsigned)c->vocab);

    forward_seq(m, toks, n_prompt < 32 ? n_prompt : 32, 0, 1);      /* aquecimento */
    for (int r = 0; r < 3; r++) {
        double t0 = now_sec();
        float *lg = forward_seq(m, toks, n_prompt, 0, 1);
        double t1 = now_sec();
        int pos = n_prompt;
        for (int g = 0; g < n_gen; g++) lg = forward(m, sample(lg, c->vocab, 0.0f), pos++, 1);
        double t2 = now_sec();
        printf("rodada %d: prompt %d tokens %.1f tok/s | geracao %d tokens %.1f tok/s (%.2f ms/token)\n",
               r + 1, n_prompt, n_prompt / (t1 - t0), n_gen, n_gen / (t2 - t1), (t2 - t1) * 1e3 / n_gen);
    }
    if (getenv("KATANA_TIME")) fprintf(stderr, "[espera pela GPU nas leituras: %.2f s no total]\n", t_gread);
    if (getenv("KATANA_TIME") && vkb.passes)
        fprintf(stderr, "[lote Vulkan: %ld passadas, %ld tokens; CPU embedding+envio %.3f s, enfileirar %.3f s, espera %.3f s]\n",
                vkb.passes, vkb.toks, vkb.t_emb, vkb.t_emit, vkb.t_wait);
    prof_report();
    cprof_report();
    moe_report();
    free(toks);
    return 0;
}

/* microbenchmark do produto em Vulkan: muitos despachos num fluxo so e uma unica espera no fim */
static double vk_time_run(const Mat *const *ws, const int *rr, const int *cc, int nw, int reps, KvkBuf x, KvkBuf out, double *weights) {
    double w = 0.0;
    kvk_wait();
    const double t0 = now_sec();
    for (int i = 0; i < reps; i++) {
        const int k = i % nw;
        if (!vk_gmv(ws[k], x, out, rr[k], cc[k], 0)) die("vkmicro: despacho falhou");
        w += (double)rr[k] * cc[k];
    }
    kvk_wait();
    *weights = w;
    return now_sec() - t0;
}

static int vkmicro(Model *m) {
    const Config *c = &m->cfg;
    const int d = c->dim, kvd = c->n_kv * c->hd, H = c->hidden, qd = c->n_heads * c->hd;
    if (!gpu.vk) { printf("vkmicro: Vulkan indisponivel\n"); return 1; }
    float *xh = xalloc(H > d ? (H > qd ? H : qd) : (d > qd ? d : qd), sizeof(float));
    for (int i = 0; i < d; i++) xh[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    kvk_buf_write(gpu.vxb, 0, xh, (size_t)d * sizeof(float));
    printf("Vulkan: produto matriz x vetor, 400 despachos por espera, G pesos/s\n");
    const Layer *L = &m->layers[0];
    const struct { const char *n; const Mat *w; int r, c; } t[] = {
        { "attn_q", &L->wq, qd, d }, { "attn_k", &L->wk, kvd, d }, { "attn_v", &L->wv, kvd, d }, { "attn_out", &L->wo, d, qd },
        { "ffn_gate", &L->w_gate, H, d }, { "ffn_up", &L->w_up, H, d }, { "ffn_down", &L->w_down, d, H } };
    for (int i = 0; i < 7; i++) {
        if (!t[i].w->gbuf) continue;
        double w;
        vk_time_run(&t[i].w, &t[i].r, &t[i].c, 1, 20, gpu.vxb, gpu.vob, &w);     /* aquece */
        const double s = vk_time_run(&t[i].w, &t[i].r, &t[i].c, 1, 400, gpu.vxb, gpu.vob, &w);
        printf("  %-9s tipo=%-2u %6dx%-5d (mesma matriz, pode estar na L2): %7.1f G pesos/s  %6.1f us/despacho\n",
               t[i].n, t[i].w->type, t[i].r, t[i].c, w / s / 1e9, s / 400 * 1e6);
    }
    /* todas as matrizes ja na VRAM em sequencia (como uma passada de geracao): fora da L2 */
    int n = 0, cap = c->n_layers * 7 + 1;
    const Mat **ws = xalloc(cap, sizeof(Mat *)); int *rr = xalloc(cap, sizeof(int)), *cc = xalloc(cap, sizeof(int));
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *Ll = &m->layers[l];
        const struct { const Mat *w; int r, c; } u[] = { { &Ll->wq, qd, d }, { &Ll->wk, kvd, d }, { &Ll->wv, kvd, d }, { &Ll->wo, d, qd },
            { &Ll->w_gate, H, d }, { &Ll->w_up, H, d }, { &Ll->w_down, d, H } };
        for (int i = 0; i < 7; i++) if (u[i].w->gbuf) { ws[n] = u[i].w; rr[n] = u[i].r; cc[n] = u[i].c; n++; }
    }
    if (n) {
        const int reps = n * (n > 100 ? 2 : 6);
        double w;
        vk_time_run(ws, rr, cc, n, n, gpu.vxb, gpu.vob, &w);
        const double s = vk_time_run(ws, rr, cc, n, reps, gpu.vxb, gpu.vob, &w);
        printf("  todas as %d matrizes na VRAM em sequencia (%d despachos): %7.1f G pesos/s  %6.1f us/despacho\n", n, reps, w / s / 1e9, s / reps * 1e6);
    }
    KvkStats st; kvk_stats(&st);
    printf("  despachos=%llu submissoes=%llu conjuntos de descritores=%llu\n", (unsigned long long)st.dispatches, (unsigned long long)st.submits, (unsigned long long)st.ds_allocs);
    free(xh); free(ws); free(rr); free(cc);
    return 0;
}

/* microbenchmark da CPU numa matriz: a mesma linha repetida (so calculo, dados no cache L1)
   contra todas as linhas em sequencia (calculo + leitura da memoria), em uma thread */
static void micro_mat(const char *name, const Mat *W, int rows, int cols) {
    if (!W->q) return;
    float *x = xalloc(cols, sizeof(float)), *sx = xalloc(cols / 32 + 1, sizeof(float));
    for (int i = 0; i < cols; i++) x[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    block_sums(sx, x, cols);
    for (int exact = 1; exact >= 0; exact--) {
        cpu_exact = exact;
        if (!exact && !mat_int(W)) break;
        if (mat_int(W)) xq_prepare(x, cols);
        volatile float sink = 0.0f;
        const int reps = (int)(400000000.0 / cols) + 1;
        double t0 = now_sec();
        for (int r = 0; r < reps; r++) sink += dot_row(W, 0, x, sx, cols);
        double t1 = now_sec();
        int passes = (int)(400000000.0 / ((double)rows * cols)) + 1;
        for (int ps = 0; ps < passes; ps++)
            for (int i = 0; i < rows; i++) sink += dot_row(W, (size_t)i, x, sx, cols);
        double t2 = now_sec();
        printf("  %-9s tipo=%-2u %-8s  mesma linha: %6.2f G pesos/s | matriz inteira: %6.2f G pesos/s (%.2f GB/s lidos)\n",
               name, W->type, exact ? "float" : "inteiro", (double)reps * cols / (t1 - t0) / 1e9,
               (double)passes * rows * cols / (t2 - t1) / 1e9, (double)passes * rows * W->stride / (t2 - t1) / 1e9);
    }
    cpu_exact = 0;
    free(x); free(sx);
}

/* o mesmo teste com todas as threads do pool: se "mesma linha" escala e "matriz inteira"
   nao, o limite e a memoria; se nem "mesma linha" escala, sao os nucleos (ocupados ou
   com a frequencia reduzida) */
typedef struct { const Mat *W; const float *x, *sx; int cols; volatile float sink; } MicroJob;
static void micro_same_row(void *c, int i0, int i1) {
    MicroJob *j = c;
    float a = 0.0f;
    for (int i = i0; i < i1; i++) a += dot_row(j->W, 0, j->x, j->sx, j->cols);
    j->sink += a;
}
static void micro_calib(void *c, int i0, int i1) {
    uint64_t acc = (uint64_t)i0 + 1;
    (void)c;
    for (int r = i0; r < i1; r++)
        for (int i = 0; i < 1000000; i++) acc = acc * 6364136223846793005ull + 1442695040888963407ull;
    if (acc == 42) printf(" ");
}

static void micro_par(const char *name, const Mat *W, int rows, int cols) {
    if (!mat_int(W)) return;
    float *x = xalloc(cols, sizeof(float)), *out = xalloc(rows, sizeof(float));
    for (int i = 0; i < cols; i++) x[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    xq_prepare(x, cols);
    MicroJob job = { W, x, NULL, cols, 0.0f };
    const int reps = (int)(1600000000.0 / cols) + 1;
    double t0 = now_sec();
    pool_for(micro_same_row, &job, reps, reps / (pool.n * 16) + 1);
    double t1 = now_sec();
    pool.st_jobs = pool.st_main = pool.st_work = pool.st_sleep = 0;
    const int passes = (int)(1600000000.0 / ((double)rows * cols)) + 1;
    for (int ps = 0; ps < passes; ps++) matvec(out, W, x, rows, cols);
    double t2 = now_sec();
    printf("  %-9s tipo=%-2u %d threads  mesma linha: %6.2f G pesos/s | matriz inteira: %6.2f G pesos/s (%.2f GB/s lidos)\n",
           name, W->type, pool.n, (double)reps * cols / (t1 - t0) / 1e9,
           (double)passes * rows * cols / (t2 - t1) / 1e9, (double)passes * rows * W->stride / (t2 - t1) / 1e9);
    printf("            pool: %ld trabalhos, blocos feitos pela principal %ld e pelas outras %ld, sonos %ld, %.0f us por trabalho\n",
           (long)pool.st_jobs, (long)pool.st_main, (long)pool.st_work, (long)pool.st_sleep, (t2 - t1) * 1e6 / passes);
    pool.st_jobs = pool.st_main = pool.st_work = pool.st_sleep = 0;
    free(x); free(out);
}

static int micro(const Model *m) {
    const Config *c = &m->cfg;
    const Layer *L = &m->layers[0];
    const int d = c->dim, H = c->hidden;
    printf("microbenchmark da CPU (1 thread):\n");
    {   /* calibracao: o que esta maquina entrega agora em contas simples, com dados no cache L1 */
        float *v = xalloc(1024, sizeof(float));
        volatile float sink = 0.0f;
        for (int i = 0; i < 1024; i++) v[i] = (float)i * 0.001f;
        double t0 = now_sec();
        for (int r = 0; r < 2000000; r++) sink += dot(v, v, 1024);
        double t1 = now_sec();
        uint64_t acc = 1;
        for (uint64_t i = 0; i < 400000000ull; i++) acc = acc * 6364136223846793005ull + 1442695040888963407ull;
        double t2 = now_sec();
        printf("  calibracao: dot float L1 = %.2f G elementos/s | cadeia de multiplicacoes = %.2f GHz equivalentes (%u)\n",
               2000000.0 * 1024 / (t1 - t0) / 1e9, 400000000.0 * 4 / (t2 - t1) / 1e9, (unsigned)(acc & 7));
        free(v);
    }
    micro_mat("attn_q", &L->wq, m->cfg.n_heads * m->cfg.hd, d);
    micro_mat("ffn_gate", &L->w_gate, H, d);
    micro_mat("ffn_down", &L->w_down, d, H);
    micro_mat("lm_head", &m->lm_head, c->vocab, d);
    {   /* calibracao paralela: contas escalares puras em todas as threads */
        double t0 = now_sec();
        pool_for(micro_calib, NULL, 64 * pool.n, 1);
        double t1 = now_sec();
        printf("em paralelo (%d threads): contas escalares = %.2f GHz equivalentes por thread\n",
               pool.n, 64.0 * 1000000 * 4 / (t1 - t0) / 1e9);
    }
    micro_par("attn_q", &L->wq, m->cfg.n_heads * m->cfg.hd, d);
    micro_par("ffn_gate", &L->w_gate, H, d);
    micro_par("ffn_down", &L->w_down, d, H);
    return 0;
}

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

/* grava um shard e acrescenta a linha dele ao manifesto */
static int pack_one(FILE *mf, Mat **ms, int n, size_t rows, size_t cols, const char *name, double *total) {
    char fn[1100], hex[65];
    size_t bytes = 0;
    uint32_t *buf = gpu_build_buf(ms, n, rows, cols, &bytes);
    if (!buf) return 0;
    sha_hex(buf, bytes, hex);
    snprintf(fn, sizeof fn, "%s/%s.bin", shards.dir, name);
    FILE *f = fopen(fn, "wb");
    int ok = f && fwrite(buf, 1, bytes, f) == bytes;
    if (f) fclose(f);
    free(buf);
    if (ok) { fprintf(mf, "%s %zu %s\n", hex, bytes, name); *total += (double)bytes; }
    return ok;
}

/* converte o modelo inteiro para shards no layout da GPU */
/* MoE: todos os especialistas da camada num arquivo so, cada um como par gate/up + down no layout da GPU */
static int pack_experts(FILE *mf, Model *m, int l, double *total) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden;
    Layer *L = &m->layers[l];
    char fn[1100], hex[65], nm[32];
    uint8_t *all = NULL;
    size_t bytes = 0, per = 0;
    int ok = 1;
    for (int e = 0; e < c->n_expert && ok; e++) {
        Mat *two[2] = { &L->eg[e], &L->eu[e] }, *dn = &L->ed[e];
        size_t b0 = 0, b1 = 0;
        uint32_t *g = gpu_build_buf(two, 2, H, d, &b0), *w = gpu_build_buf(&dn, 1, d, H, &b1);
        if (!all && g && w) { per = b0 + b1; all = malloc(per * c->n_expert); }
        ok = g && w && all && b0 + b1 == per;
        if (ok) { memcpy(all + bytes, g, b0); memcpy(all + bytes + b0, w, b1); bytes += per; }
        free(g); free(w);
    }
    if (ok) {
        sha_hex(all, bytes, hex);
        snprintf(nm, sizeof nm, "L%03d.exps", l);
        snprintf(fn, sizeof fn, "%s/%s.bin", shards.dir, nm);
        FILE *f = fopen(fn, "wb");
        ok = f && fwrite(all, 1, bytes, f) == bytes;
        if (f) fclose(f);
    }
    free(all);
    if (!ok) return 0;
    fprintf(mf, "%s %zu %s\n", hex, bytes, nm);
    *total += (double)bytes;
    return 1;
}

static int pack(Model *m, const char *path, int64_t fsz) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd;
    char mfn[1100], nm[32];
    double total = 0.0, t0 = now_sec();
    int ok = 1, count = 0;
    snprintf(shards.dir, sizeof shards.dir, "%s.ktn", path);
    MKDIR(shards.dir);
    snprintf(mfn, sizeof mfn, "%s/manifest.txt", shards.dir);
    FILE *mf = fopen(mfn, "w");
    if (!mf) { fprintf(stderr, "Nao consegui criar %s\n", mfn); return 1; }
    fprintf(mf, "katana-shards 1\norigem %lld\n", (long long)fsz);
    for (int l = 0; l < c->n_layers && ok; l++) {
        Layer *L = &m->layers[l];
        Mat *one;
        const size_t Hl = c->n_expert && !L->eg ? (size_t)c->hidden_dense : H;
        if (c->mla) {       /* MLA: as demais matrizes sobem convertidas na carga; so os especialistas viram shard */
            if (L->eg) { ok = ok && pack_experts(mf, m, l, &total); count++; }
            printf("\rcamada %d de %d", l + 1, c->n_layers); fflush(stdout);
            continue;
        }
#define P1(mat, rows, cols, suf) one = &L->mat; snprintf(nm, sizeof nm, "L%03d." suf, l); ok = ok && pack_one(mf, &one, 1, rows, cols, nm, &total); count++
        P1(wq, m->cfg.n_heads * m->cfg.hd, d, "wq"); P1(wk, kvd, d, "wk"); P1(wv, kvd, d, "wv"); P1(wo, d, m->cfg.n_heads * m->cfg.hd, "wo");
        if (L->eg) {
            if (!glu_pair(&L->w_gate, &L->w_up)) { fprintf(stderr, "MoE com gate e up em formatos diferentes: nao empacotado\n"); ok = 0; }
            ok = ok && pack_experts(mf, m, l, &total); count++;
            printf("\rcamada %d de %d", l + 1, c->n_layers); fflush(stdout);
        } else if (glu_pair(&L->w_gate, &L->w_up)) {
            Mat *two[2] = { &L->w_gate, &L->w_up };
            snprintf(nm, sizeof nm, "L%03d.glu", l);
            ok = ok && pack_one(mf, two, 2, Hl, d, nm, &total); count++;
        } else { P1(w_gate, Hl, d, "gate"); P1(w_up, Hl, d, "up"); }
        if (!L->eg) { P1(w_down, d, Hl, "down"); }
#undef P1
    }
    if (ok) { Mat *one = &m->lm_head; ok = pack_one(mf, &one, 1, c->vocab, d, "head", &total); count++; }
    fclose(mf);
    if (!ok) { fprintf(stderr, "Falha ao gravar os shards (disco cheio?)\n"); return 1; }
    printf("%d shards, %.0f MB em %s (%.1f s)\n", count, total / 1048576.0, shards.dir, now_sec() - t0);
    return 0;
}

/* confere o hash de todos os shards do manifesto */
static int verify(const char *path) {
    char mfn[1100], line[256], fn[1100], hex[65];
    int n = 0, bad = 0;
    snprintf(shards.dir, sizeof shards.dir, "%s.ktn", path);
    snprintf(mfn, sizeof mfn, "%s/manifest.txt", shards.dir);
    FILE *mf = fopen(mfn, "r");
    if (!mf) { fprintf(stderr, "Sem manifesto em %s (rode: katana pack)\n", shards.dir); return 1; }
    while (fgets(line, sizeof line, mf)) {
        char want[65], name[32]; unsigned long long bytes;
        if (sscanf(line, "%64s %llu %31s", want, &bytes, name) != 3 || strlen(want) != 64) continue;
        snprintf(fn, sizeof fn, "%s/%s.bin", shards.dir, name);
        FILE *f = fopen(fn, "rb");
        uint8_t *buf = f ? malloc((size_t)bytes) : NULL;
        int ok = buf && fread(buf, 1, (size_t)bytes, f) == (size_t)bytes && fgetc(f) == EOF;
        if (ok) { sha_hex(buf, (size_t)bytes, hex); ok = strcmp(hex, want) == 0; }
        if (f) fclose(f);
        free(buf);
        n++;
        if (!ok) { bad++; printf("CORROMPIDO ou ausente: %s\n", name); }
    }
    fclose(mf);
    printf("%d shards conferidos, %d com problema: %s\n", n, bad, bad || !n ? "FALHOU" : "OK");
    return bad || !n ? 1 : 0;
}

/* tempo de uma passada: prompt em lote e geracao token a token (melhor de 2) */
static void tune_measure(Model *m, const int *toks, int n_prompt, int n_gen, double *pt, double *gt) {
    *pt = *gt = 1e30;
    forward_seq(m, toks, 16, 0, 1);
    for (int r = 0; r < 2; r++) {
        double t0 = now_sec();
        float *lg = forward_seq(m, toks, n_prompt, 0, 1);
        double t1 = now_sec();
        int pos = n_prompt;
        for (int g = 0; g < n_gen; g++) lg = forward(m, sample(lg, m->cfg.vocab, 0.0f), pos++, 1);
        double t2 = now_sec();
        if (t1 - t0 < *pt) *pt = t1 - t0;
        if (t2 - t1 < *gt) *gt = t2 - t1;
    }
}

/* mede as opcoes de ajuste nesta placa e grava a melhor combinacao em katana.cfg */
static int tune(Model *m) {
    static const size_t segs[] = { 0, 4096, 16384, 65536 };
    const int NP = 128, NG = 32;
    int toks[128];
    double pt, gt, best;
    if (!gpu.full) { fprintf(stderr, "tune: use um modelo que caiba inteiro na VRAM\n"); return 1; }
    for (int i = 0; i < NP; i++) toks[i] = (int)((1000003u * (i + 1)) % (unsigned)m->cfg.vocab);

    size_t best_seg = gpu.seg_target; best = 1e30;
    for (int i = 0; i < 4; i++) {
        gpu.seg_target = segs[i];
        tune_measure(m, toks, NP, NG, &pt, &gt);
        printf("segmentos alvo=%-6zu  geracao %.1f tok/s  prompt %.1f tok/s\n", segs[i], NG / gt, NP / pt);
        if (gt < best) { best = gt; best_seg = segs[i]; }
    }
    gpu.seg_target = best_seg;

    int best_rows = 1, best_rowsb = 1; double bg = 1e30, bp = 1e30;
    for (int nr = 1; nr <= 2; nr++) {       /* linhas por thread: mede geracao (rows) e lote (rowsb) juntos */
        gpu.cfg_rows = nr; gpu.cfg_rowsb = nr;
        if (!gpu_build_all()) { fprintf(stderr, "tune: falha ao recompilar os kernels\n"); return 1; }
        tune_measure(m, toks, NP, NG, &pt, &gt);
        printf("linhas por thread=%d      geracao %.1f tok/s  prompt %.1f tok/s\n", nr, NG / gt, NP / pt);
        if (gt < bg) { bg = gt; best_rows = nr; }
        if (pt < bp) { bp = pt; best_rowsb = nr; }
    }
    gpu.cfg_rows = best_rows; gpu.cfg_rowsb = best_rowsb;
    if (!gpu_build_all()) return 1;

    char fn[1100];
    cfg_path(fn, sizeof fn);
    char keep[8192]; size_t kn = 0;      /* as outras chaves do arquivo (model=, models=) ficam */
    keep[0] = 0;
    { FILE *o = fopen(fn, "r"); char l[1024];
      while (o && fgets(l, sizeof l, o)) if (strncmp(l, "gpu=", 4) && strncmp(l, "seg=", 4) && strncmp(l, "rows=", 5) && strncmp(l, "rowsb=", 6) && kn + strlen(l) < sizeof keep) { strcpy(keep + kn, l); kn += strlen(l); }
      if (o) fclose(o); }
    FILE *f = fopen(fn, "w");
    if (!f) { fprintf(stderr, "tune: nao consegui gravar %s\n", fn); return 1; }
    fprintf(f, "gpu=%s\nseg=%zu\nrows=%d\nrowsb=%d\n%s", gpu.name, best_seg, best_rows, best_rowsb, keep);
    fclose(f);
    printf("melhor: segmentos alvo=%zu, linhas por thread=%d (geracao) e %d (lote); gravado em %s\n",
           best_seg, best_rows, best_rowsb, fn);
    return 0;
}

/* nome legivel do tipo de quantizacao de um tensor */
static const char *qtype_name(uint32_t t) {
    switch (t) {
        case T_F32: return "F32"; case T_F16: return "F16"; case T_BF16: return "BF16";
        case T_Q4_0: return "Q4_0"; case T_Q4_1: return "Q4_1"; case T_Q5_0: return "Q5_0"; case T_Q5_1: return "Q5_1";
        case T_Q8_0: return "Q8_0"; case T_Q2_K: return "Q2_K"; case T_Q3_K: return "Q3_K"; case T_Q4_K: return "Q4_K";
        case T_Q5_K: return "Q5_K"; case T_Q6_K: return "Q6_K"; case T_IQ4_NL: return "IQ4_NL"; case T_IQ4_XS: return "IQ4_XS";
        case T_MXFP4: return "MXFP4"; case T_IQ2_XXS: return "IQ2_XXS"; case T_IQ2_XS: return "IQ2_XS"; case T_IQ2_S: return "IQ2_S";
        case T_IQ3_XXS: return "IQ3_XXS"; case T_IQ3_S: return "IQ3_S"; case T_IQ1_S: return "IQ1_S"; case T_IQ1_M: return "IQ1_M";
        case T_TQ1_0: return "TQ1_0"; case T_TQ2_0: return "TQ2_0";
        default: return "?";
    }
}

