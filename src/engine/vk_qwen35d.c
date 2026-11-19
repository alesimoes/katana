/* ---------- qwen35 denso no modo hibrido: prompt em lote ----------
 * As camadas ficam na CPU (estado do DeltaNet e cache KV na RAM) e os produtos das matrizes que subiram para a
 * VRAM saem em lote na GPU (vk_mm_p), B tokens por passada: cada matriz e percorrida uma vez para o lote todo, em
 * vez de uma vez por token. Convolucao e recorrencia do DeltaNet e a atencao seguem na CPU, como no qn_layers_n.
 * Matriz que nao subiu (VRAM curta) ou de formato fora do kernel (linhas ou colunas nao multiplas de 64) vai pelo
 * matmat da CPU. KATANA_VK_QD=0 desliga (prompt token a token, como antes); KATANA_VK_PASS muda o lote. */
static struct {
    int ok, P;
    KvkBuf xin, y;
    float *T, *X, *Xb, *X2, *P1, *Z, *O, *KK, *VV, *AL, *BE, *HB, *HB2;
    const float *in; int in_cols, in_up;       /* entrada corrente dos produtos e se ja subiu */
    double t_gpu, t_cpu; long passes;
} vkqd;

static int qd_gpu_ok(const Mat *W, int rows, int cols) {
    return W->gbuf && gpu_kind(W) != T_Q2_K && gpu_has(W) && W->gs == 1 && rows % 64 == 0 && cols % 64 == 0;
}

static int vk_qd_build(Model *m) {
    const Config *c = &m->cfg;
    memset(&vkqd, 0, sizeof vkqd);
    if (!c->ssm_inner || c->n_expert || !gpu.vk || !vkg.coop) return 0;
    if (getenv("KATANA_VK_QD") && atoi(getenv("KATANA_VK_QD")) == 0) return 0;
    const size_t d = c->dim, qd = (size_t)c->n_heads * c->hd, kvd = (size_t)c->n_kv * c->hd, H = c->hidden;
    const size_t C = dn_channels(c), inner = c->ssm_inner, nv = c->ssm_rank;
    size_t P = getenv("KATANA_VK_PASS") ? (size_t)atoi(getenv("KATANA_VK_PASS")) : 64;
    if (P < 2) return 0;
    if (P > 256) P = 256;
    size_t win = d, wout = d;
    if (inner > win) win = inner;
    if (qd > win) win = qd;
    if (H > win) win = H;
    if (C > wout) wout = C;
    if (inner > wout) wout = inner;
    if (2 * qd > wout) wout = 2 * qd;
    if (H > wout) wout = H;
    int fail = 0;
    const size_t used0 = gpu.used;
    vkqd.xin = vk_wk(win * P * 4, &fail); vkqd.y = vk_wk(wout * P * 4, &fail);
    if (fail || !kvk_wait()) {
        fprintf(stderr, "Vulkan: sem memoria para o prompt em lote do qwen35 denso (%s): prompt token a token\n", kvk_error());
        gpu.used = used0;
        return 0;
    }
    const size_t wide = win > wout ? win : wout, o = inner > qd ? inner : qd;
    vkqd.T = xalloc(wide * P, 4);
    vkqd.X = xalloc(d * P, 4); vkqd.Xb = xalloc(d * P, 4); vkqd.X2 = xalloc(d * P, 4);
    vkqd.P1 = xalloc((C > 2 * qd ? C : 2 * qd) * P, 4); vkqd.Z = xalloc(inner * P, 4); vkqd.O = xalloc(o * P, 4);
    vkqd.KK = xalloc(kvd * P, 4); vkqd.VV = xalloc(kvd * P, 4);
    vkqd.AL = xalloc(nv * P, 4); vkqd.BE = xalloc(nv * P, 4);
    vkqd.HB = xalloc(H * P, 4); vkqd.HB2 = xalloc(H * P, 4);
    vkqd.P = (int)P; vkqd.ok = 1;
    fprintf(stderr, "Vulkan: prompt do qwen35 denso em lote (ate %zu tokens por passada, %.0f MB)\n", P, (gpu.used - used0) / 1048576.0);
    return 1;
}

/* define a entrada [B][cols] dos proximos produtos */
static void qd_in(const float *A, int cols) { vkqd.in = A; vkqd.in_cols = cols; vkqd.in_up = 0; }

/* out [B][rows] = W x entrada corrente, para os B tokens */
static void qd_mul(float *out, const Mat *W, int B, int rows) {
    const int cols = vkqd.in_cols;
    if (qd_gpu_ok(W, rows, cols)) {
        const double t0 = now_sec();
        float *T = vkqd.T;
        int ok = 1;
        if (!vkqd.in_up) {         /* sobe a entrada transposta [cols][B] uma vez para todos os produtos dela */
            for (int t = 0; t < B; t++) for (int i = 0; i < cols; i++) T[(size_t)i * B + t] = vkqd.in[(size_t)t * cols + i];
            ok &= kvk_buf_write_q(vkqd.xin, 0, T, (size_t)cols * B * 4);
            vkqd.in_up = 1;
        }
        ok &= vk_mm_p(W, vkqd.xin, vkqd.y, rows, cols, B, 0, 1);
        if (!ok || !kvk_buf_read(vkqd.y, 0, T, (size_t)rows * B * 4)) die("falha na execucao em Vulkan (lote, qwen35 denso)");
        for (int t = 0; t < B; t++) for (int i = 0; i < rows; i++) out[(size_t)t * rows + i] = T[(size_t)i * B + t];
        vkqd.t_gpu += now_sec() - t0;
        return;
    }
    static XQ xqs[GPU_BMAX];
    const double t0 = now_sec();
    for (int b = 0; b < B; b += GPU_BMAX) {
        int prep = 0;
        matmat(out + (size_t)b * rows, W, vkqd.in + (size_t)b * cols, xqs, &prep, B - b < GPU_BMAX ? B - b : GPU_BMAX, rows, cols);
    }
    vkqd.t_cpu += now_sec() - t0;
}

static float *vk_qd_n(Model *m, const int *toks, int B, int pos0, int want_logits) {
    const Config *c = &m->cfg;
    const int d = c->dim, nh = c->n_heads, hd = c->hd, qd = nh * hd, kvd = c->n_kv * hd, H = c->hidden;
    const int C = (int)dn_channels(c), inner = c->ssm_inner, nv = c->ssm_rank;
    if (B < 1 || B > vkqd.P || pos0 < 0 || pos0 + B > c->ctx) die("lote fora do limite (Vulkan, qwen35 denso)");
    float *X = vkqd.X, *Xb = vkqd.Xb, *X2 = vkqd.X2, *P1 = vkqd.P1, *Z = vkqd.Z, *O = vkqd.O, *KK = vkqd.KK, *VV = vkqd.VV;
    float *AL = vkqd.AL, *BE = vkqd.BE, *HB = vkqd.HB, *HB2 = vkqd.HB2;
    for (int t = 0; t < B; t++) embed_row(m, X + (size_t)t * d, toks[t]);
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
        for (int t = 0; t < B; t++) rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->attn_norm, d, c->eps);
        qd_in(Xb, d);
        if (L->lin) {
            if (pos0 == 0) dn_reset(m, L);
            qd_mul(P1, &L->wqkv, B, C);
            qd_mul(Z, &L->wz, B, inner);
            qd_mul(AL, &L->walpha, B, nv);
            qd_mul(BE, &L->wbeta, B, nv);
            for (int t = 0; t < B; t++) {
                float *qkv = P1 + (size_t)t * C, *o = O + (size_t)t * inner;
                dn_conv(c, L, dn_CV(m, L), qkv);
                dn_recur(m, L, qkv, AL + (size_t)t * nv, BE + (size_t)t * nv, o);
                dn_gate(c, L, o, Z + (size_t)t * inner);
            }
            qd_in(O, inner);
            qd_mul(X2, &L->wssm_out, B, d);
        } else {
            qd_mul(P1, &L->wq, B, 2 * qd);
            qd_mul(KK, &L->wk, B, kvd);
            qd_mul(VV, &L->wv, B, kvd);
            for (int b = 0; b < B; b += GPU_BMAX) {         /* a atencao em lote da CPU trabalha em blocos de GPU_BMAX */
                const int nb = B - b < GPU_BMAX ? B - b : GPU_BMAX;
                if (nb > 1) qn_core_n(m, L, l, pos0 + b, nb, P1 + (size_t)b * 2 * qd, KK + (size_t)b * kvd, VV + (size_t)b * kvd, O + (size_t)b * qd);
                else qn_core(m, L, l, pos0 + b, P1 + (size_t)b * 2 * qd, KK + (size_t)b * kvd, VV + (size_t)b * kvd, O + (size_t)b * qd);
            }
            qd_in(O, qd);
            qd_mul(X2, &L->wo, B, d);
        }
        for (size_t i = 0; i < (size_t)B * d; i++) X[i] += X2[i];
        for (int t = 0; t < B; t++) rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->ffn_norm, d, c->eps);
        qd_in(Xb, d);
        qd_mul(HB, &L->w_gate, B, H);
        qd_mul(HB2, &L->w_up, B, H);
        for (size_t i = 0; i < (size_t)B * H; i++) { const float g = HB[i]; HB[i] = g / (1.0f + expf(-g)) * HB2[i]; }
        qd_in(HB, H);
        qd_mul(X2, &L->w_down, B, d);
        for (size_t i = 0; i < (size_t)B * d; i++) X[i] += X2[i];
    }
    vkqd.passes++;
    memcpy(m->x, X + (size_t)(B - 1) * d, d * sizeof(float));
    return want_logits ? cpu_head(m) : NULL;
}
