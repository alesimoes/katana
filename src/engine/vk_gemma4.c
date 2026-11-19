/* ---------- gemma4 em Vulkan (modo hibrido): prompt em lote ----------
   Residual dos B tokens na CPU (token a token: X[t*dim]). Por camada, os produtos densos do lote vao a GPU no mm
   preciso (alta + resto, ativacao fp32): q, k, v; saida da atencao; gate, up e down do feed-forward denso. Normas,
   RoPE, gravacao do cache (anel nas locais) e a atencao ficam na CPU, em blocos de G4_NB tokens (o anel tem folga
   de G4_NB posicoes: um bloco grava antes de atender). O MoE: vk_moe_n (vagas do cache, frios temporarios ou na
   CPU; roteador calculado na CPU) ou, sem cache de especialistas, na CPU como o g4_layers_n.
   KATANA_VK_G4=0 desliga (prompt como antes); o MoE do lote vai as vagas por padrao (KATANA_VK_G4MOE=0 o deixa na CPU). */
static struct {
    int ok, P, moe;
    float *X, *Xb, *X2, *Q, *KK, *VV, *O, *HB, *HB2, *YA, *YB, *LG, *T, *OUTS;
    double t_gpu, t_att, t_moe; long passes;
} vkg4;

static int vk_g4_build(Model *m) {
    const Config *c = &m->cfg;
    const int NL = c->n_layers;
    const size_t d = c->dim, nh = c->n_heads, Hd = c->hidden_dense, E = c->n_expert;
    memset(&vkg4, 0, sizeof vkg4);
    if (getenv("KATANA_VK_G4") && atoi(getenv("KATANA_VK_G4")) == 0) return 0;
    if (!vkg.coop || d % 64 || Hd % 64) return 0;
    size_t qdm = 0, kvm = 0;
#define MOK(M) ((M).gbuf && gpu_kind(&(M)) != T_Q2_K && gpu_has(&(M)) && (M).gs == 1)
    for (int l = 0; l < NL; l++) {
        const Layer *L = &m->layers[l];
        const size_t qd = nh * L->hd_l, kvd = (size_t)L->nkv_l * L->hd_l;
        if (qd % 64 || kvd % 64) return 0;
        if (qd > qdm) qdm = qd;
        if (kvd > kvm) kvm = kvd;
        int ok = MOK(L->wq) && MOK(L->wk) && MOK(L->wo) && MOK(L->w_gate) && MOK(L->w_up) && MOK(L->w_down);
        if (L->wv.q || L->wv.f) ok = ok && MOK(L->wv);
        if (!ok) { fprintf(stderr, "Vulkan: produto denso do gemma4 fora da VRAM (camada %d): prompt em lote na CPU\n", l); return 0; }
    }
#undef MOK
    size_t P = getenv("KATANA_VK_PASS") ? (size_t)atoi(getenv("KATANA_VK_PASS")) : VK_PASS_DEF;
    if (P < 2) return 0;
    if (P > 1024) P = 1024;
    size_t win = d > qdm ? d : qdm, wo1 = win, wo2 = kvm;
    if (Hd > win) win = Hd;
    if (Hd > wo1) wo1 = Hd;
    if (Hd > wo2) wo2 = Hd;
    int fail = 0;
    const size_t used0 = gpu.used;
    memset(&vkb, 0, sizeof vkb);
    vkbm.xf = vk_wk(win * P * 4, &fail); vkb.q = vk_wk(wo1 * P * 4, &fail);
    vkb.k = vk_wk(wo2 * P * 4, &fail); vkb.q2 = vk_wk(kvm * P * 4, &fail);
    if (fail || !kvk_wait()) {
        fprintf(stderr, "Vulkan: sem memoria para o lote do gemma4 (%s): prompt em lote na CPU\n", kvk_error());
        gpu.used = used0;
        return 0;
    }
    /* MoE do lote nas vagas (KATANA_VK_G4MOE=0 desliga): o down dos especialistas do gemma4 e Q5_1 em 29 de 30
       camadas e sobe no layout exato de Q5_1 (antes era requantizado em Q8_0 e o lote nao batia com a CPU) */
    if (E && vk_sg && !(getenv("KATANA_VK_G4MOE") && atoi(getenv("KATANA_VK_G4MOE")) == 0)) {
        const int K = c->n_used;
        const size_t H = c->hidden;
        int f2 = 0;
        const size_t u1 = gpu.used;
        const KvkBuf xf0 = vkbm.xf;     /* entrada dos produtos densos; o vk_moe_n grava nela a entrada normalizada */
        memset(&vkbm, 0, sizeof vkbm);
        vkbm.xf = xf0;
        vkb.x = vk_wk(d * P * 4, &f2); vkb.hb = vk_wk(H * P * 4, &f2); vk_moe_out = vk_wk(d * P * 4, &f2);
        vkbm.xg = vk_wk(d * P * 4, &f2); vkbm.ys = vk_wk(d * P * 4, &f2); vkbm.yc = vk_wk(d * P * 4, &f2);
        vkbm.lg = vk_wk(128 * 4, &f2); vkbm.meta = vk_wk(2 * P * (K + 2) * 4, &f2);
        vkbm.rms32 = kvk_pipe_create(rms_wg32_spv, sizeof rms_wg32_spv, 3, 12, NULL, NULL, 0);
        vkbm.gath32 = kvk_pipe_create(moe_gath32_spv, sizeof moe_gath32_spv, 3, 16, NULL, NULL, 0);
        vkbm.scat = kvk_pipe_create(moe_scat_spv, sizeof moe_scat_spv, 3, 16, NULL, NULL, 0);
        if (!vkbm.rms32.p || !vkbm.gath32.p || !vkbm.scat.p) f2 = 1;
        static KvkBuf *pre2;
        pre2 = xalloc(NL, sizeof(KvkBuf));
        for (int l = 0; l < NL && !f2; l++) if (m->layers[l].eg) pre2[l] = vk_vec(m->layers[l].pre_ffn2, d, &f2);
        /* frios temporarios: poucas vagas (as 8 do cache mal cabem na VRAM do gemma4) */
        vkbm.T = getenv("KATANA_VK_COLDT") ? atoi(getenv("KATANA_VK_COLDT")) : 4;
        vkbm.cmin = getenv("KATANA_VK_COLDMIN") ? atoi(getenv("KATANA_VK_COLDMIN")) : 2;
        if (vkbm.T < 0) vkbm.T = 0;
        if (vkbm.T > 64) vkbm.T = 64;
        if (vkbm.T && !f2) {
            for (int l = 0; l < NL; l++) {
                const Layer *L = &m->layers[l];
                if (!L->eg) continue;
                const size_t bg = 2 * gpu_mat_bytes(&L->eg[0], H, d), bd = gpu_mat_bytes(&L->ed[0], d, H);
                if (bg > vkbm.tgb) vkbm.tgb = bg;
                if (bd > vkbm.tdb) vkbm.tdb = bd;
            }
            vkbm.tg = vk_wk(vkbm.T * vkbm.tgb, &f2); vkbm.td = vk_wk(vkbm.T * vkbm.tdb, &f2);
            vkbm.stage = xalloc(vkbm.tgb + vkbm.tdb, 1);
            vkbm.pf = xalloc(NL, sizeof(FILE *));
            if (!getenv("KATANA_VK_TMPTHR") || atoi(getenv("KATANA_VK_TMPTHR"))) vkbm.tstage = xalloc(vkbm.T * (vkbm.tgb + vkbm.tdb), 1);
        }
        if (f2 || !kvk_wait()) {
            fprintf(stderr, "Vulkan: sem memoria para o MoE do lote do gemma4 (%s): MoE do lote na CPU\n", kvk_error());
            gpu.used = u1;
            memset(&vk_moe_out, 0, sizeof vk_moe_out);
        } else {
            vkbm.xn = xalloc(d * P, sizeof(float)); vkbm.lgT = xalloc(E, sizeof(float)); vkbm.xc = xalloc(d * P, sizeof(float));
            vkbm.outs = xalloc((size_t)GPU_BMAX * K * d, sizeof(float)); vkbm.sw = xalloc(P, sizeof(float));
            vkbm.mt = xalloc(2 * P * (K + 2), sizeof(uint32_t));
            vkbm.ccnt = xalloc(E, sizeof(int)); vkbm.tmpe = xalloc(64, sizeof(int));
            vkbm.cnt = xalloc(E + 66, sizeof(int)); vkbm.ofs = xalloc(E + 66, sizeof(int));
            vkbm.pv = xalloc(P * K, sizeof(int)); vkbm.pb = xalloc(P * K, sizeof(int)); vkbm.pw = xalloc(P * K, sizeof(float));
            vkbm.ce = xalloc(P * K, sizeof(int)); vkbm.cb = xalloc(P * K, sizeof(int)); vkbm.cw = xalloc(P * K, sizeof(float));
            vkbm.prec = 1;
            vkm.sh = 0; vkm.rt = 0;
            vkg.fn = pre2;      /* vk_moe_n normaliza a entrada dos especialistas com pre_ffw_norm_2 */
            vkb.xT = xalloc(d * P, sizeof(float));
            vkg4.moe = 1;
        }
    }
    const size_t mx = win > wo1 ? win : wo1;
    vkg4.T = xalloc(mx * P, 4);
    vkg4.X = xalloc(d * P, 4); vkg4.Xb = xalloc(d * P, 4); vkg4.X2 = xalloc(d * P, 4);
    vkg4.Q = xalloc(qdm * P, 4); vkg4.O = xalloc(qdm * P, 4); vkg4.KK = xalloc(kvm * P, 4); vkg4.VV = xalloc(kvm * P, 4);
    vkg4.HB = xalloc(Hd * P, 4); vkg4.HB2 = xalloc(Hd * P, 4); vkg4.YA = xalloc(d * P, 4); vkg4.YB = xalloc(d * P, 4);
    vkg4.LG = xalloc((E ? E : 1) * P, 4); vkg4.OUTS = xalloc((size_t)G4_NB * (c->n_used ? c->n_used : 1) * d, 4);
    vkg4.P = (int)P; vkg4.ok = 1;
    fprintf(stderr, "Vulkan: prompt gemma4 em lote (ate %zu tokens por passada, %.0f MB; MoE do lote %s)\n", P,
            (gpu.used - used0) / 1048576.0, vkg4.moe ? "nas vagas" : "na CPU");
    return 1;
}

/* sobe A [B][n] (token a token) transposta para buf [n][B] */
static int vk_g4_put(KvkBuf buf, const float *A, int B, int n) {
    float *T = vkg4.T;
    for (int t = 0; t < B; t++) for (int i = 0; i < n; i++) T[(size_t)i * B + t] = A[(size_t)t * n + i];
    return kvk_buf_write_q(buf, 0, T, (size_t)n * B * 4);
}
/* le buf [n][B] e devolve A [B][n] */
static void vk_g4_get(KvkBuf buf, float *A, int B, int n) {
    float *T = vkg4.T;
    if (!kvk_buf_read(buf, 0, T, (size_t)n * B * 4)) die("falha na execucao em Vulkan (leitura do lote, gemma4)");
    for (int t = 0; t < B; t++) for (int i = 0; i < n; i++) A[(size_t)t * n + i] = T[(size_t)i * B + t];
}

static float *vk_g4_n(Model *m, const int *toks, int B, int pos0, int want_logits) {
    const Config *c = &m->cfg;
    const int d = c->dim, nh = c->n_heads, K = c->n_used, E = c->n_expert, Hd = c->hidden_dense;
    static int ex[G4_NB * MOE_KMAX], xi[G4_NB * MOE_KMAX]; static float ew[G4_NB * MOE_KMAX];
    if (B < 1 || B > vkg4.P || pos0 < 0 || pos0 + B > c->ctx) die("lote fora do limite (Vulkan, gemma4)");
    float *X = vkg4.X, *Xb = vkg4.Xb, *X2 = vkg4.X2, *Q = vkg4.Q, *KK = vkg4.KK, *VV = vkg4.VV, *O = vkg4.O;
    float *HB = vkg4.HB, *HB2 = vkg4.HB2, *YA = vkg4.YA, *YB = vkg4.YB;
    for (int t = 0; t < B; t++) embed_row(m, X + (size_t)t * d, toks[t]);
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
        const int hd = L->hd_l, nkv = L->nkv_l, qd = nh * hd, kvd = nkv * hd, half = hd / 2, cap = L->kv_cap;
        const int hasv = L->wv.q || L->wv.f;
        const float *fr = L->swa ? m->rope_freq_swa : m->rope_freq;
        double t0 = now_sec();
        int ok = 1;
        for (int t = 0; t < B; t++) rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->attn_norm, d, c->eps);
        ok &= vk_g4_put(vkbm.xf, Xb, B, d);
        ok &= vk_mm_p(&L->wq, vkbm.xf, vkb.q, qd, d, B, 0, 1);
        ok &= vk_mm_p(&L->wk, vkbm.xf, vkb.k, kvd, d, B, 0, 1);
        if (hasv) ok &= vk_mm_p(&L->wv, vkbm.xf, vkb.q2, kvd, d, B, 0, 1);
        if (!ok) die("falha na execucao em Vulkan (lote, gemma4)");
        vk_g4_get(vkb.q, Q, B, qd);
        vk_g4_get(vkb.k, KK, B, kvd);
        if (hasv) vk_g4_get(vkb.q2, VV, B, kvd);
        else memcpy(VV, KK, (size_t)B * kvd * sizeof(float));
        double t1 = now_sec();
        vkg4.t_gpu += t1 - t0;
        float *kb = m->kcache ? m->kcache + L->kv_off : NULL, *vb = m->vcache ? m->vcache + L->kv_off : NULL;
        uint16_t *kh = m->kh ? m->kh + L->kv_off : NULL, *vh = m->vh ? m->vh + L->kv_off : NULL;
        for (int b0 = 0; b0 < B; b0 += G4_NB) {     /* blocos de G4_NB: o anel das locais so tem essa folga */
            const int nb = B - b0 < G4_NB ? B - b0 : G4_NB;
            for (int t = b0; t < b0 + nb; t++) {
                float *q = Q + (size_t)t * qd, *k = KK + (size_t)t * kvd, *v = VV + (size_t)t * kvd;
                const int pos = pos0 + t;
                for (int h = 0; h < nh; h++) rmsnorm(q + h * hd, q + h * hd, L->q_norm, hd, c->eps);
                for (int h = 0; h < nkv; h++) { rmsnorm(k + h * hd, k + h * hd, L->k_norm, hd, c->eps); rms_plain(v + h * hd, hd, c->eps); }
                for (int i = 0; i < half; i++) {
                    const float ang = pos * fr[i], cs = cosf(ang), sn = sinf(ang);
                    for (int h = 0; h < nh; h++) { float *p = q + h * hd + i, a = p[0], bb = p[half]; p[0] = a * cs - bb * sn; p[half] = a * sn + bb * cs; }
                    for (int h = 0; h < nkv; h++) { float *p = k + h * hd + i, a = p[0], bb = p[half]; p[0] = a * cs - bb * sn; p[half] = a * sn + bb * cs; }
                }
                g4_kv_put(kb, vb, kh, vh, (size_t)(pos % cap) * kvd, k, v, kvd);
            }
            G4AttN aj = { Q + (size_t)b0 * qd, O + (size_t)b0 * qd, kb, vb, kh, vh, pos0 + b0, nh, hd, kvd, nh / nkv,
                          L->swa && c->swa_win < cap ? c->swa_win : cap, cap };
            pool_for(g4_attn_job, &aj, nb * nh, 1);
        }
        t0 = now_sec();
        vkg4.t_att += t0 - t1;
        ok &= vk_g4_put(vkbm.xf, O, B, qd);
        ok &= vk_mm_p(&L->wo, vkbm.xf, vkb.q, d, qd, B, 0, 1);
        if (!ok) die("falha na execucao em Vulkan (lote, gemma4)");
        vk_g4_get(vkb.q, X2, B, d);
        for (int t = 0; t < B; t++) {
            float *x = X + (size_t)t * d, *y = X2 + (size_t)t * d;
            rmsnorm(y, y, L->post_attn_norm, d, c->eps);
            for (int i = 0; i < d; i++) x[i] += y[i];
        }
        /* feed-forward denso: gate e up na GPU, GELU na CPU, down na GPU */
        for (int t = 0; t < B; t++) rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->ffn_norm, d, c->eps);
        ok &= vk_g4_put(vkbm.xf, Xb, B, d);
        ok &= vk_mm_p(&L->w_gate, vkbm.xf, vkb.q, Hd, d, B, 0, 1);
        ok &= vk_mm_p(&L->w_up, vkbm.xf, vkb.k, Hd, d, B, 0, 1);
        if (!ok) die("falha na execucao em Vulkan (lote, gemma4)");
        vk_g4_get(vkb.q, HB, B, Hd);
        vk_g4_get(vkb.k, HB2, B, Hd);
        for (size_t i = 0; i < (size_t)B * Hd; i++) HB[i] = gelu_t(HB[i]) * HB2[i];
        ok &= vk_g4_put(vkbm.xf, HB, B, Hd);
        ok &= vk_mm_p(&L->w_down, vkbm.xf, vkb.q, d, Hd, B, 0, 1);
        if (!ok) die("falha na execucao em Vulkan (lote, gemma4)");
        vk_g4_get(vkb.q, YA, B, d);
        t1 = now_sec();
        vkg4.t_gpu += t1 - t0;
        if (L->eg) {
            const float rs = 1.0f / sqrtf((float)d);
            for (int t = 0; t < B; t++) {      /* roteador na CPU: le o residuo normalizado sem peso */
                rmsnorm(YA + (size_t)t * d, YA + (size_t)t * d, L->post_ffn1, d, c->eps);
                memcpy(m->xb, X + (size_t)t * d, d * sizeof(float));
                rms_plain(m->xb, d, c->eps);
                for (int i = 0; i < d; i++) m->xb[i] *= rs * L->router_scale[i];
                matvec(vkg4.LG + (size_t)t * E, &L->router, m->xb, E, d);
            }
            if (vkg4.moe) {     /* vagas do cache + frios temporarios/CPU; a soma volta para YB */
                memset(vkg4.T, 0, (size_t)d * B * 4);
                ok &= kvk_buf_write_q(vk_moe_out, 0, vkg4.T, (size_t)d * B * 4);
                ok &= vk_g4_put(vkb.x, X, B, d);
                if (!ok) die("falha na execucao em Vulkan (lote, gemma4)");
                vk_moe_lgh = vkg4.LG;
                vk_moe_n(m, L, l, B);
                vk_moe_lgh = NULL;
                vk_g4_get(vk_moe_out, YB, B, d);
                if (getenv("KATANA_VK_G4CHK")) {   /* depuracao: compara com o MoE na CPU */
                    double mx = 0, mr = 0;
                    for (int t = 0; t < B; t++) {
                        moe_lg_ext = vkg4.LG + (size_t)t * E; moe_route(m, L, l); moe_lg_ext = NULL;
                        int ee[MOE_KMAX], zz[MOE_KMAX] = {0};
                        for (int j = 0; j < K; j++) ee[j] = moe.sel[j];
                        rmsnorm(Xb, X + (size_t)t * d, L->pre_ffn2, d, c->eps);
                        moe_cpu_multi(m, L, ee, zz, K, 1, Xb, vkg4.OUTS);
                        for (int i = 0; i < d; i++) {
                            float s = 0; for (int j = 0; j < K; j++) s += moe.w[j] * vkg4.OUTS[(size_t)j * d + i];
                            const double df = fabs(s - YB[(size_t)t * d + i]);
                            if (df > mx) mx = df;
                            if (fabs(s) > mr) mr = fabs(s);
                        }
                    }
                    fprintf(stderr, "[chk g4 moe] camada %d: dif max %.3e (escala %.3e)\n", l, mx, mr);
                }
            }
            else for (int b0 = 0; b0 < B; b0 += G4_NB) {
                const int nb = B - b0 < G4_NB ? B - b0 : G4_NB;
                for (int t = 0; t < nb; t++) {
                    moe_lg_ext = vkg4.LG + (size_t)(b0 + t) * E;
                    moe_route(m, L, l);
                    moe_lg_ext = NULL;
                    for (int j = 0; j < K; j++) { ex[t * K + j] = moe.sel[j]; xi[t * K + j] = t; ew[t * K + j] = moe.w[j]; }
                    rmsnorm(Xb + (size_t)t * d, X + (size_t)(b0 + t) * d, L->pre_ffn2, d, c->eps);
                }
                moe_cpu_multi(m, L, ex, xi, nb * K, nb, Xb, vkg4.OUTS);
                for (int t = 0; t < nb; t++) {
                    float *yb = YB + (size_t)(b0 + t) * d;
                    memset(yb, 0, d * sizeof(float));
                    for (int j = 0; j < K; j++) { const float w = ew[t * K + j], *o = vkg4.OUTS + (size_t)(t * K + j) * d; for (int i = 0; i < d; i++) yb[i] += w * o[i]; }
                }
            }
            for (int t = 0; t < B; t++) {
                float *yb = YB + (size_t)t * d, *ya = YA + (size_t)t * d;
                rmsnorm(yb, yb, L->post_ffn2, d, c->eps);
                for (int i = 0; i < d; i++) ya[i] += yb[i];
            }
        }
        for (int t = 0; t < B; t++) {
            float *x = X + (size_t)t * d, *ya = YA + (size_t)t * d;
            rmsnorm(ya, ya, L->post_ffn_norm, d, c->eps);
            for (int i = 0; i < d; i++) x[i] = (x[i] + ya[i]) * L->out_scale;
        }
        vkg4.t_moe += now_sec() - t1;
    }
    vkg4.passes++;
    if (getenv("KATANA_VK_MOEPROF")) {
        fprintf(stderr, "[lote gemma4] %d tokens: produtos na GPU %.3f, atencao na CPU %.3f, MoE e normas %.3f\n", B, vkg4.t_gpu, vkg4.t_att, vkg4.t_moe);
        vkg4.t_gpu = vkg4.t_att = vkg4.t_moe = 0;
    }
    memcpy(m->x, X + (size_t)(B - 1) * d, d * sizeof(float));
    return want_logits ? cpu_head(m) : NULL;
}

static void cpu_layers(Model *m, int l0, int l1, int pos) {
    const Config *c = &m->cfg;
    const int d = c->dim, hd = c->hd, H = c->hidden, ctx = c->ctx;
    const int kvd = c->n_kv * hd, group = c->n_heads / c->n_kv;
    const float scale = 1.0f / sqrtf((float)hd);
    const int rhalf = (c->glm || c->glmoe) && c->rope_dim > 0 && c->rope_dim < hd ? c->rope_dim / 2 : hd / 2;
    const int step = c->rope_neox ? 1 : 2, gap = c->rope_neox ? rhalf : 1;
    const int qd = c->n_heads * hd;

    for (int l = l0; l < l1; l++) {
        const Layer *L = &m->layers[l];
        if (l + 1 < l1) layer_prefetch(m, l + 1);

        if (c->ssm_inner) { qn_layer(m, L, l, pos); continue; }
        if (c->g4) { g4_layer(m, L, l, pos); continue; }
        if (c->go) { go_layer(m, L, l, pos); continue; }
        if (c->mla) mla_attn(m, L, l, pos);
        else {
        /* --- atencao (GQA + RoPE) --- */
        rmsnorm(m->xb, m->x, L->attn_norm, d, c->eps);
        matvec(m->q, &L->wq, m->xb, qd, d);
        matvec(m->k, &L->wk, m->xb, kvd, d);
        matvec(m->v, &L->wv, m->xb, kvd, d);
        if (L->bq) for (int i = 0; i < qd; i++) m->q[i] += L->bq[i];
        if (L->bk) for (int i = 0; i < kvd; i++) m->k[i] += L->bk[i];
        if (L->bv) for (int i = 0; i < kvd; i++) m->v[i] += L->bv[i];

        if (c->qk_norm == 1) {
            rmsnorm(m->q, m->q, L->q_norm, qd, c->eps);
            rmsnorm(m->k, m->k, L->k_norm, kvd, c->eps);
        } else if (c->qk_norm == 2) {
            for (int h = 0; h < c->n_heads; h++) rmsnorm(m->q + h * hd, m->q + h * hd, L->q_norm, hd, c->eps);
            for (int h = 0; h < c->n_kv; h++) rmsnorm(m->k + h * hd, m->k + h * hd, L->k_norm, hd, c->eps);
        }

        /* RoPE: gira pares (2i, 2i+1) no llama, ou (i, i+hd/2) no qwen2; glm4: so rope_dim dimensoes */
        for (int i = 0; i < rhalf; i++) {
            float ang = pos * m->rope_freq[i], cs = cosf(ang), sn = sinf(ang);
            for (int h = 0; h < c->n_heads; h++) {
                float *p = m->q + h * hd + step * i, a = p[0], b = p[gap];
                p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs;
            }
            for (int h = 0; h < c->n_kv; h++) {
                float *p = m->k + h * hd + step * i, a = p[0], b = p[gap];
                p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs;
            }
        }

        float *kbase = m->kcache + (size_t)l * ctx * kvd;
        float *vbase = m->vcache + (size_t)l * ctx * kvd;
        memcpy(kbase + (size_t)pos * kvd, m->k, kvd * sizeof(float));
        memcpy(vbase + (size_t)pos * kvd, m->v, kvd * sizeof(float));

        for (int h = 0; h < c->n_heads; h++) {
            const float *qh = m->q + h * hd;
            const int kvh = (h / group) * hd;
            for (int t = 0; t <= pos; t++)
                m->att[t] = dot(qh, kbase + (size_t)t * kvd + kvh, hd) * scale;
            softmax(m->att, pos + 1);

            float *out = m->xb + h * hd;
            memset(out, 0, hd * sizeof(float));
            for (int t = 0; t <= pos; t++) {
                const float *vh = vbase + (size_t)t * kvd + kvh;
                const float a = m->att[t];
                for (int i = 0; i < hd; i++) out[i] += a * vh[i];
            }
        }
        matvec(m->xb2, &L->wo, m->xb, d, qd);
        if (L->bo) for (int i = 0; i < d; i++) m->xb2[i] += L->bo[i];
        if (c->glm) rmsnorm(m->xb2, m->xb2, L->post_attn_norm, d, c->eps);
        for (int i = 0; i < d; i++) m->x[i] += m->xb2[i];
        }

        /* --- feed-forward SwiGLU: down(silu(gate(x)) * up(x)) --- */
        rmsnorm(m->xb, m->x, L->ffn_norm, d, c->eps);
        if (c->n_expert && L->eg) {
            /* MoE: o roteador pontua todos os especialistas; so os n_used melhores rodam,
               cada um pesado pela sua probabilidade (softmax sobre todos) */
            const int K = c->n_used;
            if (gx.on && gpu.mv_host && !gpu.force_cpu && gx_host_layer(l)) { host_moe_ffn(m, L, l, m->x); continue; }
            moe_route(m, L, l);
            static float *outs;
            if (!outs) outs = xalloc((size_t)K * d, sizeof(float));
            moe_cpu_experts(m, L, moe.sel, K, outs);
            for (int k = 0; k < K; k++) {
                const float w = moe.w[k];
                const float *o = outs + (size_t)k * d;
                for (int i = 0; i < d; i++) m->x[i] += w * o[i];
            }
            if (L->sg.q || L->sg.f) {   /* compartilhados: sempre entram (peso 1 ou portao sigmoide) */
                const float gw = shexp_gate_w(m, L);
                shexp_cpu(m, L);
                for (int i = 0; i < d; i++) m->x[i] += gw * m->xb2[i];
            }
            continue;
        }
        if (c->n_expert) {      /* camada densa inicial de um MoE */
            const int Hd = c->hidden_dense;
            matvec(m->hb, &L->w_gate, m->xb, Hd, d);
            matvec(m->hb2, &L->w_up, m->xb, Hd, d);
            for (int i = 0; i < Hd; i++) { const float g = m->hb[i]; m->hb[i] = g / (1.0f + expf(-g)) * m->hb2[i]; }
            matvec(m->xb2, &L->w_down, m->hb, d, Hd);
            for (int i = 0; i < d; i++) m->x[i] += m->xb2[i];
            continue;
        }
        matvec(m->hb, &L->w_gate, m->xb, H, d);
        matvec(m->hb2, &L->w_up, m->xb, H, d);
        for (int i = 0; i < H; i++) {
            float g = m->hb[i];
            m->hb[i] = g / (1.0f + expf(-g)) * m->hb2[i];
        }
        if (sp.collect) for (int i = 0; i < H; i++) sp.sum[(size_t)l * H + i] += fabsf(m->hb[i]);
        if (sp.oracle_keep) {   /* experimento: so os N mais ativos DESTE token contribuem */
            for (int i = 0; i < H; i++) sp.tmp[i] = fabsf(m->hb[i]);
            qsort(sp.tmp, H, sizeof(float), cmp_float_desc);
            const float thr = sp.tmp[sp.oracle_keep - 1];
            for (int i = 0; i < H; i++) {
                const double a = fabs((double)m->hb[i]);
                sp.mass_all += a;
                if (a >= thr) sp.mass_kept += a; else m->hb[i] = 0.0f;
            }
        }
        if (sp.mask) {      /* experimento: so os neuronios "quentes" contribuem */
            const uint8_t *mk = sp.mask + (size_t)l * H;
            for (int i = 0; i < H; i++) {
                const double a = fabs((double)m->hb[i]);
                sp.mass_all += a;
                if (mk[i]) sp.mass_kept += a; else m->hb[i] = 0.0f;
            }
        }
        matvec(m->xb2, &L->w_down, m->hb, d, H);
        if (c->glm) rmsnorm(m->xb2, m->xb2, L->post_ffn_norm, d, c->eps);
        for (int i = 0; i < d; i++) m->x[i] += m->xb2[i];
    }
}

