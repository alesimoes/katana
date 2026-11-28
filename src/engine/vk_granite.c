/* ---------- granitehybrid no modo hibrido: prompt em lote (e, por opcao, token a token) ----------
   Mesmo padrao do qwen35 denso (vk_qwen35d.c) e do gemma4 (vk_gemma4.c): residual dos B tokens na CPU, os
   produtos densos sobem a GPU no produto preciso (vk_mm_p) - ssm_in/ssm_out das camadas Mamba-2, wq/wk/wv/wo
   das camadas de atencao. A convolucao causal e a recorrencia SSD (sequencial por natureza: o estado de um
   passo alimenta o proximo, pouco paralelismo por token) ficam na CPU, reusando as mesmas funcoes do caminho
   generico (grh_conv/grh_recur/grh_gate, engine/granite.c) - exatamente a mesma decisao do PLE no gemma4
   (matrizes pequenas ou contas sequenciais, o ida-e-volta nao paga). A atencao (RoPE, cache, softmax) tambem
   fica na CPU (att_job), como no hibrido do gemma4 e do qwen35 denso. O feed-forward MoE (toda camada, com
   especialista compartilhado) reusa o cache de especialistas e o roteamento em lote ja existentes
   (vk_moe_init/vk_moe_n), os mesmos do gemma4 e do qwen35moe: nenhum kernel novo, so orquestracao.
   KATANA_VK_GR=0 desliga (tudo na CPU, como antes do hibrido). KATANA_VK_GR_DEC=1 tambem usa este caminho na
   decodificacao (token a token); o padrao decodifica pela CPU pura (grh_layers_n), que nao paga ida-e-volta
   por camada - mesma escolha medida no gemma4 (prompt em lote na GPU, decodificacao na CPU). */
static struct {
    int ok, moe, P;
    KvkBuf xin, y, moe_out;
    float *X, *Xb, *X2, *ZX, *O, *QQ, *KK, *VV, *YB, *T;
    double t_gpu, t_cpu, t_moe; long passes;
} vkgr;

static int gr_gpu_ok(const Mat *W, int rows, int cols) {
    return W->gbuf && gpu_kind(W) != T_Q2_K && gpu_has(W) && W->gs == 1 && rows % 64 == 0 && cols % 64 == 0;
}

/* entrada corrente dos produtos (B tokens, 'cols' colunas); gr_mul sobe, multiplica e desce (ou cai na CPU
   quando a matriz nao esta na VRAM ou o formato nao e multiplo de 64) */
static const float *gr_in; static int gr_in_cols;
static void gr_set_in(const float *A, int cols) { gr_in = A; gr_in_cols = cols; }
static void gr_mul(float *out, const Mat *W, int B, int rows) {
    const int cols = gr_in_cols;
    if (gr_gpu_ok(W, rows, cols)) {
        const double t0 = now_sec();
        float *T = vkgr.T;
        for (int t = 0; t < B; t++) for (int i = 0; i < cols; i++) T[(size_t)i * B + t] = gr_in[(size_t)t * cols + i];
        int ok = kvk_buf_write_q(vkgr.xin, 0, T, (size_t)cols * B * 4);
        ok &= vk_mm_p(W, vkgr.xin, vkgr.y, rows, cols, B, 0, 1);
        if (!ok || !kvk_buf_read(vkgr.y, 0, T, (size_t)rows * B * 4)) die("falha na execucao em Vulkan (lote, granitehybrid)");
        for (int t = 0; t < B; t++) for (int i = 0; i < rows; i++) out[(size_t)t * rows + i] = T[(size_t)i * B + t];
        vkgr.t_gpu += now_sec() - t0;
        return;
    }
    static XQ xqs[GPU_BMAX];
    const double t0 = now_sec();
    for (int b = 0; b < B; b += GPU_BMAX) {
        int prep = 0;
        const int nb = B - b < GPU_BMAX ? B - b : GPU_BMAX;
        matmat(out + (size_t)b * rows, W, gr_in + (size_t)b * cols, xqs, &prep, nb, rows, cols);
    }
    vkgr.t_cpu += now_sec() - t0;
}

static int vk_gr_build(Model *m) {
    const Config *c = &m->cfg;
    memset(&vkgr, 0, sizeof vkgr);
    if (!c->gr || !gpu.vk || !vkg.coop) return 0;
    if (getenv("KATANA_VK_GR") && atoi(getenv("KATANA_VK_GR")) == 0) return 0;
    if (!c->n_expert) return 0;      /* granitehybrid sempre tem MoE em toda camada; sem isso, algo esta errado */
    const size_t d = c->dim, qd = (size_t)c->n_heads * c->hd, kvd = (size_t)c->n_kv * c->hd;
    const size_t di = c->ssm_inner, ng = c->ssm_groups, ds = c->ssm_state, nsh = c->ssm_rank;
    const size_t d_in_proj = 2 * di + 2 * ng * ds + nsh;
    size_t P = getenv("KATANA_VK_PASS") ? (size_t)atoi(getenv("KATANA_VK_PASS")) : 64;
    if (P < 2) return 0;
    if (P > 256) P = 256;
    size_t win = d > qd ? d : qd;
    if (d_in_proj > win) win = d_in_proj;
    size_t wout = di > qd ? di : qd;
    if (d_in_proj > wout) wout = d_in_proj;
    int fail = 0;
    const size_t used0 = gpu.used;
    vkgr.xin = vk_wk(win * P * 4, &fail); vkgr.y = vk_wk(wout * P * 4, &fail);
    if (fail || !kvk_wait()) {
        fprintf(stderr, "Vulkan: sem memoria para o lote do granitehybrid (%s): prompt token a token\n", kvk_error());
        gpu.used = used0;
        return 0;
    }
    const size_t wide = win > wout ? win : wout;
    vkgr.T = xalloc(wide * P, 4);
    vkgr.X = xalloc(d * P, 4); vkgr.Xb = xalloc(d * P, 4); vkgr.X2 = xalloc(d * P, 4);
    vkgr.ZX = xalloc(d_in_proj * P, 4); vkgr.O = xalloc((di > qd ? di : qd) * P, 4);
    vkgr.QQ = xalloc(qd * P, 4); vkgr.KK = xalloc(kvd * P, 4); vkgr.VV = xalloc(kvd * P, 4);
    vkgr.YB = xalloc(d * P, 4);

    /* MoE em toda camada (com especialista compartilhado): cache de especialistas (vagas) e lote, no mesmo
       padrao do gemma4 e do qwen35moe (vk_moe_init/vk_moe_n) - nenhum kernel novo, so orquestracao */
    if (!vk_moe_init(m)) { fprintf(stderr, "Vulkan: cache de especialistas indisponivel (granitehybrid): prompt token a token\n"); gpu.used = used0; return 0; }
    vkg.fn = xalloc(c->n_layers, sizeof(KvkBuf));      /* vk_moe_n normaliza a entrada do FFN com ffn_norm */
    int f2 = 0;
    for (int l = 0; l < c->n_layers; l++) vkg.fn[l] = vk_vec(m->layers[l].ffn_norm, d, &f2);
    const size_t Pp = P, E = c->n_expert, K = c->n_used, H = c->hidden;
    const size_t Hs = (size_t)(c->n_shared ? c->n_shared : 1) * H;
    vkb.x = vk_wk(d * Pp * 4, &f2); vkgr.moe_out = vk_wk(d * Pp * 4, &f2); vkb.hb = vk_wk((Hs > H ? Hs : H) * Pp * 4, &f2);
    vkbm.xf = vk_wk(d * Pp * 4, &f2); vkbm.xg = vk_wk(d * Pp * 4, &f2);
    vkbm.ys = vk_wk(d * Pp * 4, &f2); vkbm.yc = vk_wk(d * Pp * 4, &f2);
    vkbm.lg = vk_wk(128 * 4, &f2); vkbm.meta = vk_wk(2 * Pp * (K + 2) * 4, &f2);
    vkbm.rms32 = kvk_pipe_create(rms_wg32_spv, sizeof rms_wg32_spv, 3, 12, NULL, NULL, 0);
    vkbm.gath32 = kvk_pipe_create(moe_gath32_spv, sizeof moe_gath32_spv, 3, 16, NULL, NULL, 0);
    vkbm.scat = kvk_pipe_create(moe_scat_spv, sizeof moe_scat_spv, 3, 16, NULL, NULL, 0);
    if (!vkbm.rms32.p || !vkbm.gath32.p || !vkbm.scat.p) f2 = 1;
    if (f2 || !kvk_wait()) {
        fprintf(stderr, "Vulkan: sem memoria para o MoE do lote (granitehybrid, %s): prompt token a token\n", kvk_error());
        gpu.used = used0;
        return 0;
    }
    vkbm.xn = xalloc(d * Pp, sizeof(float)); vkbm.lgT = xalloc(E, sizeof(float)); vkbm.xc = xalloc(d * Pp, sizeof(float));
    vkbm.outs = xalloc((size_t)GPU_BMAX * K * d, sizeof(float)); vkbm.sw = xalloc(Pp, sizeof(float));
    vkbm.mt = xalloc(2 * Pp * (K + 2), sizeof(uint32_t));
    vkbm.ccnt = xalloc(E, sizeof(int)); vkbm.tmpe = xalloc(64, sizeof(int));
    vkbm.cnt = xalloc(E + 66, sizeof(int)); vkbm.ofs = xalloc(E + 66, sizeof(int));
    vkbm.pv = xalloc(Pp * K, sizeof(int)); vkbm.pb = xalloc(Pp * K, sizeof(int)); vkbm.pw = xalloc(Pp * K, sizeof(float));
    vkbm.ce = xalloc(Pp * K, sizeof(int)); vkbm.cb = xalloc(Pp * K, sizeof(int)); vkbm.cw = xalloc(Pp * K, sizeof(float));
    vkbm.prec = 1;
    vkb.xT = xalloc(d * Pp, sizeof(float));
    vkgr.moe = 1;
    vkgr.P = (int)P; vkgr.ok = 1;
    fprintf(stderr, "Vulkan: prompt granitehybrid em lote (ate %zu tokens por passada, %.0f MB)\n", P, (gpu.used - used0) / 1048576.0);
    return 1;
}

/* B tokens a partir de pos0 (toks != NULL: comeca do embedding; toks == NULL nao usado aqui, ao contrario do
   gemma4 - o granitehybrid sempre entra pelo token, nunca por um residuo ja pronto) */
static float *vk_gr_n(Model *m, const int *toks, int B, int pos0, int want_logits) {
    const Config *c = &m->cfg;
    const int d = c->dim, nh = c->n_heads, hd = c->hd, qd = nh * hd, kvd = c->n_kv * hd;
    const int di = c->ssm_inner, ng = c->ssm_groups, ds = c->ssm_state;
    const int d_in_proj = (int)(2 * (size_t)di + 2 * (size_t)ng * ds + c->ssm_rank);
    const int conv_dim = (int)((size_t)di + 2 * (size_t)ng * ds);
    const float rs = c->residual_scale != 0.0f ? c->residual_scale : 1.0f;
    const float ascale = c->attn_scale != 0.0f ? c->attn_scale : 1.0f / sqrtf((float)hd);
    const int rhalf = hd / 2, step = c->rope_neox ? 1 : 2, gap = c->rope_neox ? rhalf : 1;
    if (B < 1 || B > vkgr.P || pos0 < 0 || pos0 + B > c->ctx) die("lote fora do limite (Vulkan, granitehybrid)");
    float *X = vkgr.X, *Xb = vkgr.Xb, *X2 = vkgr.X2, *ZX = vkgr.ZX, *O = vkgr.O;
    float *QQ = vkgr.QQ, *KK = vkgr.KK, *VV = vkgr.VV, *YB = vkgr.YB;
    for (int t = 0; t < B; t++) embed_row(m, X + (size_t)t * d, toks[t]);
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
        for (int t = 0; t < B; t++) rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->attn_norm, d, c->eps);
        if (L->lin) {
            if (pos0 == 0) grh_reset(m, L);
            gr_set_in(Xb, d);
            gr_mul(ZX, &L->wqkv, B, d_in_proj);
            for (int t = 0; t < B; t++) {
                float *zx = ZX + (size_t)t * d_in_proj;
                float *z = zx, *xBC = zx + di, *dt = zx + di + conv_dim;
                grh_conv(c, L, grh_CV(m, L), xBC);
                grh_recur(m, L, xBC, dt, O + (size_t)t * di);
                grh_gate(c, L, O + (size_t)t * di, z, l);
            }
            gr_set_in(O, di);
            gr_mul(X2, &L->wssm_out, B, d);
        } else {
            gr_set_in(Xb, d);
            gr_mul(QQ, &L->wq, B, qd);
            gr_mul(KK, &L->wk, B, kvd);
            gr_mul(VV, &L->wv, B, kvd);
            float *kbase = m->kcache + L->kv_off, *vbase = m->vcache + L->kv_off;
            for (int t = 0; t < B; t++) {
                float *q = QQ + (size_t)t * qd, *k = KK + (size_t)t * kvd;
                const int pos = pos0 + t;
                for (int i = 0; i < rhalf; i++) {
                    const float ang = pos * m->rope_freq[i], cs = cosf(ang), sn = sinf(ang);
                    for (int h = 0; h < nh; h++) { float *p = q + h * hd + step * i, a = p[0], bb = p[gap]; p[0] = a * cs - bb * sn; p[gap] = a * sn + bb * cs; }
                    for (int h = 0; h < c->n_kv; h++) { float *p = k + h * hd + step * i, a = p[0], bb = p[gap]; p[0] = a * cs - bb * sn; p[gap] = a * sn + bb * cs; }
                }
                memcpy(kbase + (size_t)pos * kvd, k, kvd * sizeof(float));
                memcpy(vbase + (size_t)pos * kvd, VV + (size_t)t * kvd, kvd * sizeof(float));
            }
            AttJob aj = { QQ, O, kbase, vbase, pos0, B, nh, hd, kvd, nh / c->n_kv, qd, ascale, c->ctx };
            pool_for(att_job, &aj, B * nh, 1);
            gr_set_in(O, qd);
            gr_mul(X2, &L->wo, B, d);
        }
        for (size_t i = 0; i < (size_t)B * d; i++) X[i] += rs * X2[i];

        /* feed-forward MoE (toda camada): residuo sobe (transposto) para vkb.x, vk_moe_n roteia, roda os
           especialistas quentes nas vagas e os frios na CPU, soma pesado em vkgr.moe_out (zerado antes: o
           residual_scale do granitehybrid precisa multiplicar so a saida do FFN, por isso nao escreve direto
           em vkb.x como o qwen35moe faz - mesma solucao do gemma4 com o post_ffn2) */
        const double tm0 = now_sec();
        for (int t = 0; t < B; t++) for (int i = 0; i < d; i++) vkgr.T[(size_t)i * B + t] = X[(size_t)t * d + i];
        int ok = kvk_buf_write_q(vkb.x, 0, vkgr.T, (size_t)d * B * 4);
        memset(vkgr.T, 0, (size_t)d * B * sizeof(float));
        ok &= kvk_buf_write_q(vkgr.moe_out, 0, vkgr.T, (size_t)d * B * 4);
        if (!ok) die("falha na execucao em Vulkan (granitehybrid, residuo do MoE)");
        vk_moe_out = vkgr.moe_out;
        vk_moe_n(m, L, l, B);
        memset(&vk_moe_out, 0, sizeof vk_moe_out);
        if (!kvk_buf_read(vkgr.moe_out, 0, vkgr.T, (size_t)d * B * sizeof(float))) die("falha na execucao em Vulkan (granitehybrid, leitura do MoE)");
        for (int t = 0; t < B; t++) for (int i = 0; i < d; i++) YB[(size_t)t * d + i] = vkgr.T[(size_t)i * B + t];
        for (size_t i = 0; i < (size_t)B * d; i++) X[i] += rs * YB[i];
        vkgr.t_moe += now_sec() - tm0;
    }
    vkgr.passes++;
    memcpy(m->x, X + (size_t)(B - 1) * d, d * sizeof(float));
    return want_logits ? cpu_head(m) : NULL;
}

static int vk_gr_ready(void) { return vkgr.ok; }
static int vk_gr_pass(void) { return vkgr.P; }
