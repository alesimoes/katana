/* ---------- granitehybrid: Mamba-2 (SSD) intercalado com atencao completa, MoE em toda camada ----------
   Cada camada e OU atencao (GQA comum, RoPE em pares, escala pronta do GGUF) OU Mamba-2 (L->lin, decidido
   na carga pela lista head_count_kv). O feed-forward e sempre MoE (roteador softmax renormalizado,
   especialistas compartilhados somados sem portao): reusa moe_route/moe_cpu_multi, iguais aos outros MoE.
   Duas escalas extras do Granite (muP): residual_scale multiplica a saida de cada sub-camada antes do
   residuo; attn_scale substitui 1/sqrt(hd) quando o GGUF traz um valor pronto.

   Camada Mamba-2, um token (xb ja normalizado, tamanho d):
     zxBCdt = ssm_in(xb)                                  [2*di + 2*ng*ds + nh]
     z = zxBCdt[0:di]; xBC = zxBCdt[di:di+di+2*ng*ds]; dt = zxBCdt[...:+nh]
     xBC = silu(conv1d_causal(xBC) + conv_bias)           (estado da convolucao: janela ssm_conv-1)
     x = xBC[0:di] (cabecas [nh][hdim]); B = xBC[di:di+ng*ds]; C = xBC[di+ng*ds:di+2*ng*ds]
     por cabeca h (grupo g = h / (nh/ng)):
       dt_sp = softplus(dt[h] + dt_bias[h]); dA = exp(ssm_a[h] * dt_sp)
       S[h] = S[h] * dA + outer(B[g], x[h] * dt_sp)       (estado [ds][hdim], por cabeca)
       y[h] = S[h]^T . C[g]  +  x[h] * ssm_d[h]
     y = silu(z) * y;  y = rmsnorm_por_grupo(y) * ssm_norm
     saida = ssm_out(y)                                   [d]
   Formula conferida contra ggml_ssm_scan (ggml-cpu/ops.cpp) e o grafo de referencia (mamba-base.cpp,
   granite-hybrid.cpp) do motor de referencia: so para entender a formula publica do Mamba-2, sem copiar
   codigo (ver docs/new-models.md). */

static size_t grh_channels(const Config *c) { return 2 * (size_t)c->ssm_groups * c->ssm_state + c->ssm_inner; }
static size_t grh_state_floats(const Config *c) { return (size_t)c->ssm_state * c->ssm_inner + (size_t)(c->ssm_conv - 1) * grh_channels(c); }
static float *grh_S(Model *m, const Layer *L) { return m->ssm + (size_t)L->lin_i * grh_state_floats(&m->cfg); }
static float *grh_CV(Model *m, const Layer *L) { return grh_S(m, L) + (size_t)m->cfg.ssm_state * m->cfg.ssm_inner; }
static void grh_reset(Model *m, const Layer *L) { memset(grh_S(m, L), 0, grh_state_floats(&m->cfg) * sizeof(float)); }

/* convolucao causal por canal (janela K; as K-1 entradas anteriores ficam em cv), + vies, + SiLU.
   Mesma convencao do DeltaNet (dn_conv em qwen35.c): w[K-1] multiplica a amostra atual, w[0] a mais antiga. */
static void grh_conv(const Config *c, const Layer *L, float *cv, float *xBC) {
    const int K = c->ssm_conv, C = (int)grh_channels(c);
    for (int ch = 0; ch < C; ch++) {
        const float *w = L->conv_w + (size_t)ch * K, x = xBC[ch];
        float y = w[K - 1] * x;
        for (int t = 0; t < K - 1; t++) y += w[t] * cv[(size_t)t * C + ch];
        for (int t = 0; t < K - 2; t++) cv[(size_t)t * C + ch] = cv[(size_t)(t + 1) * C + ch];
        cv[(size_t)(K - 2) * C + ch] = x;
        if (L->conv_b) y += L->conv_b[ch];
        xBC[ch] = y / (1.0f + expf(-y));
    }
}

/* recorrencia do Mamba-2 (SSD), uma cabeca por trabalho: decaimento escalar dA = exp(softplus(dt+dt_bias)*A),
   estado [d_state][head_dim] por cabeca, y = S^T C + x*D. B/C de um grupo so (ng pode ser < nh: GVA). */
typedef struct { const Config *c; const Layer *L; float *S; const float *x, *Bv, *Cv, *dt; float *y; } GrhJob;
static void grh_job(void *cx, int h0, int h1) {
    const GrhJob *j = cx; const Config *c = j->c;
    const int ds = c->ssm_state, nh = c->ssm_rank, ng = c->ssm_groups, hdim = c->ssm_inner / nh;
    for (int h = h0; h < h1; h++) {
        const int g = h / (nh / ng);
        const float dtv = j->dt[h] + j->L->dt_bias[h];
        const float sp = dtv > 20.0f ? dtv : log1pf(expf(dtv));
        const float dA = expf(j->L->ssm_a[h] * sp);
        const float *Bg = j->Bv + (size_t)g * ds, *Cg = j->Cv + (size_t)g * ds;
        const float *xh = j->x + (size_t)h * hdim;
        float *S = j->S + (size_t)h * ds * hdim, *yh = j->y + (size_t)h * hdim;
        for (int i1 = 0; i1 < hdim; i1++) yh[i1] = 0.0f;
        for (int i0 = 0; i0 < ds; i0++) {
            float *Si = S + (size_t)i0 * hdim;
            const float b = Bg[i0], cc = Cg[i0];
            for (int i1 = 0; i1 < hdim; i1++) {
                const float st = Si[i1] * dA + b * xh[i1] * sp;
                Si[i1] = st;
                yh[i1] += st * cc;
            }
        }
        for (int i1 = 0; i1 < hdim; i1++) yh[i1] += xh[i1] * j->L->ssm_d[h];
    }
}
static void grh_recur(Model *m, const Layer *L, const float *xBC, const float *dt, float *y) {
    const Config *c = &m->cfg;
    const size_t di = c->ssm_inner, gds = (size_t)c->ssm_groups * c->ssm_state;
    GrhJob job = { c, L, grh_S(m, L), xBC, xBC + di, xBC + di + gds, dt, y };
    pool_for(grh_job, &job, c->ssm_rank, 1);
}

/* portao SiLU(z) seguido da norma RMS por grupo (ssm_norm): ng == 1 no Granite-4.0-H-Tiny vira uma
   unica norma sobre o vetor inteiro; a formula geral (grupos > 1) fica pronta para outros tamanhos. */
static void grh_dbg(const char *name, int l, const float *v, int n);
static void grh_gate(const Config *c, const Layer *L, float *y, const float *z, int l) {
    const int di = c->ssm_inner, ng = c->ssm_groups, cpg = di / ng;
    grh_dbg("dbg_z", l, z, di);
    for (int i = 0; i < di; i++) y[i] *= z[i] / (1.0f + expf(-z[i]));
    grh_dbg("dbg_y_gated", l, y, di);
    for (int g = 0; g < ng; g++) rmsnorm(y + (size_t)g * cpg, y + (size_t)g * cpg, L->ssm_norm + (size_t)g * cpg, cpg, c->eps);
    grh_dbg("dbg_y_normed", l, y, di);
}

/* depuracao temporaria (KATANA_GR_DEBUG=1): soma/somaquadrado/3 primeiros valores do token 0, para
   comparar camada a camada com o motor de referencia (cb_eval). Removido depois de confirmar os logits. */
static void grh_dbg(const char *name, int l, const float *v, int n) {
    if (!getenv("KATANA_GR_DEBUG")) return;
    char nm[32]; snprintf(nm, sizeof nm, "%s-%d", name, l);
    double sum = 0, sumsq = 0;
    for (int i = 0; i < n; i++) { sum += v[i]; sumsq += (double)v[i] * v[i]; }
    fprintf(stderr, "%-16s n=%-6d soma=%.6f somasq=%.6f  [%.6f %.6f %.6f]\n", nm, n, sum, sumsq, v[0], v[1], v[2]);
}

/* camadas [l0, l1) para B tokens a partir de pos0; X: B x dim, token a token no eixo do tempo (igual ao
   qn_layers_n do DeltaNet: produtos em lote, convolucao/recorrencia token a token). */
static void grh_layers_n(Model *m, int l0, int l1, int pos0, int B, float *X) {
    const Config *c = &m->cfg;
    const int d = c->dim, nh = c->n_heads, hd = c->hd, qd = nh * hd, kvd = c->n_kv * hd, K = c->n_used;
    const int di = c->ssm_inner, nsh = c->ssm_rank, ng = c->ssm_groups, ds = c->ssm_state;
    const size_t d_in_proj = 2 * (size_t)di + 2 * (size_t)ng * ds + nsh, conv_dim = (size_t)di + 2 * (size_t)ng * ds;
    const int Hs = c->n_shared * c->hidden;
    const float rs = c->residual_scale != 0.0f ? c->residual_scale : 1.0f;
    const float ascale = c->attn_scale != 0.0f ? c->attn_scale : 1.0f / sqrtf((float)hd);
    const int rhalf = hd / 2, step = c->rope_neox ? 1 : 2, gap = c->rope_neox ? rhalf : 1;
    static float *Xb, *X2, *ZX, *O, *QQ, *KK, *VV, *HB, *HB2, *HB3, *OUTS;
    static XQ xqs[GPU_BMAX];
    if (B > GPU_BMAX) die("granitehybrid: lote grande demais");
    if (!Xb) {
        const size_t o = (size_t)(di > qd ? di : qd);
        Xb = xalloc((size_t)GPU_BMAX * d, 4); X2 = xalloc((size_t)GPU_BMAX * d, 4);
        ZX = xalloc((size_t)GPU_BMAX * d_in_proj, 4); O = xalloc((size_t)GPU_BMAX * o, 4);
        QQ = xalloc((size_t)GPU_BMAX * qd, 4); KK = xalloc((size_t)GPU_BMAX * kvd, 4); VV = xalloc((size_t)GPU_BMAX * kvd, 4);
        HB = xalloc((size_t)GPU_BMAX * (Hs + 1), 4); HB2 = xalloc((size_t)GPU_BMAX * (Hs + 1), 4);
        HB3 = xalloc((size_t)GPU_BMAX * d, 4);
        OUTS = xalloc((size_t)GPU_BMAX * K * d, 4);
    }
    for (int l = l0; l < l1; l++) {
        const Layer *L = &m->layers[l];
        int prep = 0;
        if (l + 1 < l1) layer_prefetch(m, l + 1);
        for (int t = 0; t < B; t++) rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->attn_norm, d, c->eps);
        grh_dbg("attn_norm", l, Xb, d);
        if (L->lin) {
            if (pos0 == 0) grh_reset(m, L);
            matmat(ZX, &L->wqkv, Xb, xqs, &prep, B, (int)d_in_proj, d);
            for (int t = 0; t < B; t++) {
                float *zx = ZX + (size_t)t * d_in_proj;
                float *z = zx, *xBC = zx + di, *dt = zx + di + conv_dim;
                grh_conv(c, L, grh_CV(m, L), xBC);
                grh_recur(m, L, xBC, dt, O + (size_t)t * di);
                grh_dbg("mamba2_y_add_d", l, O + (size_t)t * di, di);
                grh_gate(c, L, O + (size_t)t * di, z, l);
            }
            prep = 0;
            matmat(X2, &L->wssm_out, O, xqs, &prep, B, d, di);
            grh_dbg("mamba_out", l, X2, d);
        } else {
            matmat(QQ, &L->wq, Xb, xqs, &prep, B, qd, d);
            matmat(KK, &L->wk, Xb, xqs, &prep, B, kvd, d);
            matmat(VV, &L->wv, Xb, xqs, &prep, B, kvd, d);
            float *kbase = m->kcache + L->kv_off, *vbase = m->vcache + L->kv_off;
            for (int t = 0; t < B; t++) {
                float *q = QQ + (size_t)t * qd, *k = KK + (size_t)t * kvd;
                const int pos = pos0 + t;
                for (int i = 0; i < rhalf; i++) {
                    const float ang = pos * m->rope_freq[i], cs = cosf(ang), sn = sinf(ang);
                    for (int h = 0; h < nh; h++) { float *p = q + h * hd + step * i, a = p[0], b = p[gap]; p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs; }
                    for (int h = 0; h < c->n_kv; h++) { float *p = k + h * hd + step * i, a = p[0], b = p[gap]; p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs; }
                }
                memcpy(kbase + (size_t)pos * kvd, k, kvd * sizeof(float));
                memcpy(vbase + (size_t)pos * kvd, VV + (size_t)t * kvd, kvd * sizeof(float));
            }
            AttJob aj = { QQ, O, kbase, vbase, pos0, B, nh, hd, kvd, nh / c->n_kv, qd, ascale, c->ctx };
            pool_for(att_job, &aj, B * nh, 1);
            prep = 0;
            matmat(X2, &L->wo, O, xqs, &prep, B, d, qd);
            grh_dbg("attn_out", l, X2, d);
        }
        for (size_t i = 0; i < (size_t)B * d; i++) X[i] += rs * X2[i];
        grh_dbg("ffn_inp", l, X, d);
        /* feed-forward: MoE em toda camada (roteador softmax renormalizado + especialistas compartilhados) */
        for (int t = 0; t < B; t++) rmsnorm(Xb + (size_t)t * d, X + (size_t)t * d, L->ffn_norm, d, c->eps);
        grh_dbg("ffn_norm", l, Xb, d);
        static int ex[GPU_BMAX * MOE_KMAX], xi[GPU_BMAX * MOE_KMAX]; static float ew[GPU_BMAX * MOE_KMAX];
        for (int t = 0; t < B; t++) {
            memcpy(m->xb, Xb + (size_t)t * d, d * sizeof(float));
            moe_route(m, L, l);
            for (int j = 0; j < K; j++) { ex[t * K + j] = moe.sel[j]; xi[t * K + j] = t; ew[t * K + j] = moe.w[j]; }
        }
        moe_cpu_multi(m, L, ex, xi, B * K, B, Xb, OUTS);
        memset(X2, 0, (size_t)B * d * sizeof(float));
        for (int t = 0; t < B; t++) {
            float *x2 = X2 + (size_t)t * d;
            for (int j = 0; j < K; j++) {
                const float w = ew[t * K + j], *o = OUTS + (size_t)(t * K + j) * d;
                for (int i = 0; i < d; i++) x2[i] += w * o[i];
            }
        }
        grh_dbg("ffn_moe_out", l, X2, d);
        if (Hs > 0) {
            prep = 0;
            matmat(HB, &L->sg, Xb, xqs, &prep, B, Hs, d);
            matmat(HB2, &L->su, Xb, xqs, &prep, B, Hs, d);
            for (size_t i = 0; i < (size_t)B * Hs; i++) { const float g = HB[i]; HB[i] = g / (1.0f + expf(-g)) * HB2[i]; }
            prep = 0;
            matmat(HB3, &L->sd, HB, xqs, &prep, B, d, Hs);
            grh_dbg("ffn_shexp", l, HB3, d);
            for (size_t i = 0; i < (size_t)B * d; i++) X2[i] += HB3[i];
        }
        grh_dbg("ffn_out", l, X2, d);
        for (size_t i = 0; i < (size_t)B * d; i++) X[i] += rs * X2[i];
        grh_dbg("l_out", l, X, d);
    }
}

/* conferencia da recorrencia do Mamba-2 (grh_job, usada em producao) contra uma segunda formula, escrita
   em paralelo e em double, sem reusar nenhuma linha do codigo de producao: mesmos moldes do
   dq_q4k_selftest (NEON x escalar) em model.c. Cabecas, estado e dados sinteticos pequenos, varios passos
   em sequencia (o estado carrega de um passo para o outro, como entre posicoes de um prompt real).
   Devolve a maior diferenca absoluta entre as duas (perto de 0 = bateu; nao e exatamente 0 porque uma
   conta em float32 e a outra em double, com a soma em ordem diferente). */
static double grh_recur_selftest(void) {
    enum { NH = 3, DS = 4, HDIM = 2, NSTEPS = 5 };
    Config c; memset(&c, 0, sizeof c);
    c.ssm_state = DS; c.ssm_rank = NH; c.ssm_groups = 1; c.ssm_inner = NH * HDIM;
    Layer L; memset(&L, 0, sizeof L);
    float ssm_a[NH], ssm_d[NH], dt_bias[NH];
    uint32_t r = 2463534242u;
    double worst = 0.0;
#define GRH_RNDF (r ^= r << 13, r ^= r >> 17, r ^= r << 5, (float)(r % 20000) / 10000.0f - 1.0f)
    for (int h = 0; h < NH; h++) { ssm_a[h] = -fabsf(GRH_RNDF) - 0.05f; ssm_d[h] = GRH_RNDF; dt_bias[h] = GRH_RNDF; }
    L.ssm_a = ssm_a; L.ssm_d = ssm_d; L.dt_bias = dt_bias;
    float Sprod[NH * DS * HDIM]; memset(Sprod, 0, sizeof Sprod);
    double Sref[NH * DS * HDIM]; memset(Sref, 0, sizeof Sref);
    for (int t = 0; t < NSTEPS; t++) {
        float x[NH * HDIM], Bv[DS], Cv[DS], dt[NH], yprod[NH * HDIM];
        for (int i = 0; i < NH * HDIM; i++) x[i] = GRH_RNDF;
        for (int i = 0; i < DS; i++) { Bv[i] = GRH_RNDF; Cv[i] = GRH_RNDF; }
        for (int h = 0; h < NH; h++) dt[h] = GRH_RNDF;
        GrhJob job = { &c, &L, Sprod, x, Bv, Cv, dt, yprod };
        grh_job(&job, 0, NH);
        for (int h = 0; h < NH; h++) {
            const double dtv = (double)dt[h] + (double)dt_bias[h];
            const double sp = dtv > 20.0 ? dtv : log1p(exp(dtv));
            const double dA = exp((double)ssm_a[h] * sp);
            for (int i1 = 0; i1 < HDIM; i1++) {
                double y = 0.0;
                for (int i0 = 0; i0 < DS; i0++) {
                    double *Si = &Sref[(h * DS + i0) * HDIM + i1];
                    *Si = *Si * dA + (double)Bv[i0] * (double)x[h * HDIM + i1] * sp;
                    y += *Si * (double)Cv[i0];
                }
                y += (double)x[h * HDIM + i1] * (double)ssm_d[h];
                const double e = fabs(y - (double)yprod[h * HDIM + i1]);
                if (e > worst) worst = e;
            }
        }
    }
#undef GRH_RNDF
    return worst;
}
