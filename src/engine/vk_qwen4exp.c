/* ---------- qwen4exp em Vulkan: geracao (um token) ----------
   Na placa ficam os mixers do residual, as projecoes e a recorrencia do DeltaNet, as projecoes da atencao e o roteador (pesos no
   formato do GGUF; a tabela de tamanhos esta em docs/new-models.md). Na CPU: especialistas, especialista compartilhado, nucleo da
   atencao esparsa, n-gramas e a projecao final. Por camada ha uma ida (saida dos especialistas) e uma volta (entrada do
   feed-forward e notas do roteador); a atencao acrescenta outra. O estado do DeltaNet mora na CPU (m->ssm, usado pelo prompt em
   lote e pelo prefixo em disco) ou na placa; q4g.loc diz onde, e q4g_home o traz de volta.
   KATANA_Q4_GPU=0 desliga. Sem memoria para tudo, o que nao couber de atencao e roteador fica na CPU; sem os mixers e o DeltaNet
   inteiros, nada vai para a placa. */
typedef struct { KvkBuf b; KvkPipe p; int rows, rw, cols, groups, ok; } Q4Gm;
typedef struct { Q4Gm dA, uA, dF, uF, qkv, z, ab, out, q, k, v, o, rt, sg, su, sd, sw; KvkBuf nA, nF, cw, pr, ring, st; int att; } Q4Gl;
static struct {
    int on, loc, R, rank, d, C, inner, NV;
    Q4Gl *l; Q4Gm dO, uO; KvkBuf nO;
    KvkBuf shb, shh, shs, X, N, DN, iA, iF, U, xn, y, qkvb, zb, abb, o, meta, grid, qg, kb, vb, one;
    KvkPipe hc[6], step;
    struct { uint32_t t; KvkPipe p; } pc[16]; int npc;
    size_t bytes, lim;
    double t_gpu, t_cpu; long n;
} q4g;

static KvkBuf q4g_buf(size_t bytes, int *fail) {
    KvkBuf b = { 0 };
    if (*fail || q4g.bytes + bytes > q4g.lim) { *fail = 1; return b; }
    b = kvk_buf_create(bytes < 16 ? 16 : bytes, KVK_USAGE_RW);
    if (!b.b) *fail = 1; else q4g.bytes += (bytes + 255) & ~(size_t)255;
    return b;
}
static KvkBuf q4g_vec(const float *v, size_t n, int *fail) {
    KvkBuf b = q4g_buf(n * 4, fail);
    if (!*fail && !kvk_buf_write(b, 0, v, n * 4)) *fail = 1;
    return b;
}
static int q4g_type(uint32_t t) { return t == T_Q4_K || t == T_Q5_K || t == T_Q6_K || t == T_IQ3_S || t == T_IQ4_XS || t == T_BF16 || t == T_IQ4_NL || t == T_Q2_0; }
/* sobe k matrizes de mesmo tipo e largura, uma embaixo da outra */
static Q4Gm q4g_mat(const Mat *const *Ws, const size_t *rows, int k, size_t cols, int *fail) {
    Q4Gm g = { 0 };
    const uint32_t t = Ws[0]->type;
    for (int i = 0; i < k; i++) if (!Ws[i]->q || Ws[i]->type != t || !q4g_type(t)) { *fail = 1; return g; }
    const int blk = t == T_IQ4_NL ? 32 : t == T_Q2_0 ? 64 : t != T_BF16 || cols % 256 == 0 ? 256 : cols % 64 == 0 ? 64 : (int)cols;
    if (cols % blk || (t == T_BF16 && blk % 2)) { *fail = 1; return g; }
    const int nb = (int)(cols / blk);
    const size_t src = t == T_BF16 ? (size_t)blk * 2 : qbytes(t, blk), dst = (src + 3) & ~(size_t)3;
    size_t tot = 0;
    for (int i = 0; i < k; i++) tot += rows[i];
    g.rows = (int)tot; g.rw = (int)(nb * dst / 4);
    g.b = q4g_buf(tot * nb * dst, fail);
    if (*fail) return g;
    uint8_t *tmp = xalloc((size_t)1 << 20, 1);
    size_t off = 0, n = 0;
    for (int i = 0; i < k; i++)
        for (size_t r = 0; r < rows[i]; r++)
            for (int b = 0; b < nb; b++) {
                const uint8_t *s = Ws[i]->q + r * Ws[i]->stride + (size_t)b * src;
                if (n + dst > ((size_t)1 << 20)) { if (!kvk_buf_write_q(g.b, off, tmp, n)) *fail = 1; off += n; n = 0; }
                if (t == T_IQ3_S || t == T_IQ4_NL || t == T_Q2_0) { memcpy(tmp + n, s, 2); tmp[n + 2] = tmp[n + 3] = 0; memcpy(tmp + n + 4, s + 2, src - 2); }     /* a escala ganha 2 bytes: o resto alinha em 32 bits */
                else { memset(tmp + n + dst - 4, 0, 4); memcpy(tmp + n, s, src); }
                n += dst;
            }
    if (n && !kvk_buf_write_q(g.b, off, tmp, n)) *fail = 1;
    if (!kvk_wait()) *fail = 1;
    free(tmp);
    g.cols = (int)cols;
    g.groups = (g.rows + 7) / 8;        /* 8 subgrupos de 32 por grupo */
    for (int i = 0; i < q4g.npc; i++) if (q4g.pc[i].t == t) { g.p = q4g.pc[i].p; g.ok = 1; return g; }
    if (q4g.npc == 16) { *fail = 1; return g; }
    const uint32_t id = 0;
    g.p = kvk_pipe_create(q4mv_spv, sizeof q4mv_spv, 4, 16, &id, &t, 1);
    if (!g.p.p) { *fail = 1; return g; }
    q4g.pc[q4g.npc].t = t; q4g.pc[q4g.npc++].p = g.p;
    g.ok = 1;
    return g;
}
static Q4Gm q4g_mat1(const Mat *W, size_t rows, size_t cols, int *fail) { const Mat *a[1] = { W }; return q4g_mat(a, &rows, 1, cols, fail); }
static int q4g_mv(const Q4Gm *g, KvkBuf x, KvkBuf o, int oofs) {
    const KvkBuf b[4] = { g->b, x, o, q4g.grid };
    const int pc[4] = { g->rows, g->rw, oofs, g->cols };
    return kvk_dispatch(g->p, b, 4, pc, sizeof pc, (uint32_t)g->groups, 1, 1);
}
static void q4g_setup(Model *m) {
    const Config *c = &m->cfg;
    Q4 *q = m->q4;
    const size_t d = c->dim, D = q->R * d, C = dn_channels(c), nv = c->ssm_rank, qd = (size_t)c->n_heads * c->hd, kvd = (size_t)c->n_kv * c->hd;
    int fail = 0;
    memset(&q4g, 0, sizeof q4g);
    if (getenv("KATANA_CPU") || (getenv("KATANA_Q4_GPU") && atoi(getenv("KATANA_Q4_GPU")) == 0)) return;
    if (c->ssm_state != 128 || c->ssm_inner != (int)nv * 128 || c->ssm_conv != 4) return;
    KvkInfo I;
    if (!kvk_init(&I)) { fprintf(stderr, "qwen4exp: Vulkan indisponivel (%s), geracao na CPU\n", kvk_error()); return; }
    if (I.subgroup_size != 32 || I.max_wg_invocations < 512) { kvk_shutdown(); return; }
    gpu.vram = I.vram_bytes; gpu.avail0 = kvk_vram_budget();
    q4g.lim = getenv("KATANA_Q4_VRAM_MB") ? (size_t)atol(getenv("KATANA_Q4_VRAM_MB")) << 20 : (size_t)gpu_budget();
    q4g.R = q->R; q4g.rank = q->rank; q4g.d = (int)d; q4g.C = (int)C; q4g.inner = c->ssm_inner; q4g.NV = (int)nv;
    q4g.l = xalloc(c->n_layers, sizeof(Q4Gl));
    const double t0 = now_sec();
    q4g.grid = q4g_buf(sizeof q_iq3s_grid, &fail);
    if (!fail && !kvk_buf_write(q4g.grid, 0, q_iq3s_grid, sizeof q_iq3s_grid)) fail = 1;
    q4g.X = q4g_buf(D * 4, &fail); q4g.N = q4g_buf(D * 4, &fail); q4g.U = q4g_buf(D * 4, &fail);
    q4g.DN = q4g_buf((q->rank + q->R) * 4, &fail); q4g.iA = q4g_buf((q->rank + q->R) * 4, &fail); q4g.iF = q4g_buf((q->rank + q->R) * 4, &fail);
    q4g.xn = q4g_buf((d + c->n_expert) * 4, &fail); q4g.y = q4g_buf(d * 4, &fail);
    q4g.qkvb = q4g_buf(C * 4, &fail); q4g.zb = q4g_buf((size_t)c->ssm_inner * 4, &fail); q4g.abb = q4g_buf(2 * nv * 4, &fail);
    q4g.o = q4g_buf((c->ssm_inner > (int)qd ? (size_t)c->ssm_inner : qd) * 4, &fail); q4g.meta = q4g_buf(16, &fail);
    q4g.qg = q4g_buf(2 * qd * 4, &fail); q4g.kb = q4g_buf(kvd * 4, &fail); q4g.vb = q4g_buf(kvd * 4, &fail);
    const uint32_t hid[4] = { 0, 1, 2, 3 };
    q4g.shb = q4g_buf(2 * (size_t)c->hidden * 4, &fail); q4g.shh = q4g_buf((size_t)c->hidden * 4, &fail); q4g.shs = q4g_buf((d + 1) * 4, &fail);
    for (int k = 0; k < 6; k++) {
        const uint32_t hv[4] = { (uint32_t)k, (uint32_t)q->R, k == 4 ? (uint32_t)c->hidden : (uint32_t)d, (uint32_t)q->rank };
        q4g.hc[k] = kvk_pipe_create(q4hc_spv, sizeof q4hc_spv, 3, 4, hid, hv, 4);
        if (!q4g.hc[k].p) fail = 1;
    }
    const uint32_t sid[2] = { 0, 1 }, sv[2] = { (uint32_t)c->ssm_groups, (uint32_t)nv };
    q4g.step = kvk_pipe_create(dn_step_sig_spv, sizeof dn_step_sig_spv, 9, 16, sid, sv, 2);
    if (!q4g.step.p) fail = 1;
    const size_t hr[2] = { (size_t)q->rank, (size_t)q->R };
    q4g.dO = q4g_mat1(&q->out.down, q->rank, D, &fail); q4g.uO = q4g_mat1(&q->out.up, D, q->rank, &fail); q4g.nO = q4g_vec(q->out.norm, D, &fail);
    float *prv = xalloc(2 * nv + 128, 4);
    for (int l = 0; l < c->n_layers && !fail; l++) {
        const Layer *L = &m->layers[l];
        Q4Gl *G = &q4g.l[l];
        const Mat *da[2] = { &q->l[l].att.down, &q->l[l].att.inj }, *df[2] = { &q->l[l].ffn.down, &q->l[l].ffn.inj };
        G->dA = q4g_mat(da, hr, 2, D, &fail); G->uA = q4g_mat1(&q->l[l].att.up, D, q->rank, &fail); G->nA = q4g_vec(q->l[l].att.norm, D, &fail);
        G->dF = q4g_mat(df, hr, 2, D, &fail); G->uF = q4g_mat1(&q->l[l].ffn.up, D, q->rank, &fail); G->nF = q4g_vec(q->l[l].ffn.norm, D, &fail);
        if (!L->lin) continue;
        const Mat *ab[2] = { &L->walpha, &L->wbeta };
        const size_t ar[2] = { nv, nv };
        G->qkv = q4g_mat1(&L->wqkv, C, d, &fail); G->z = q4g_mat1(&L->wz, c->ssm_inner, d, &fail);
        G->ab = q4g_mat(ab, ar, 2, d, &fail); G->out = q4g_mat1(&L->wssm_out, d, c->ssm_inner, &fail);
        memcpy(prv, L->ssm_a, nv * 4); memcpy(prv + nv, L->dt_bias, nv * 4); memcpy(prv + 2 * nv, L->ssm_norm, 128 * 4);
        G->cw = q4g_vec(L->conv_w, C * 4, &fail); G->pr = q4g_vec(prv, 2 * nv + 128, &fail);
        G->ring = q4g_buf(4 * C * 4, &fail); G->st = q4g_buf(nv * 128 * 128 * 4, &fail);
    }
    free(prv);
    if (fail) { fprintf(stderr, "qwen4exp: mixers e DeltaNet nao foram para a placa (%.0f de %.0f MB; %s): geracao na CPU\n", q4g.bytes / 1048576.0, q4g.lim / 1048576.0, kvk_error()); kvk_shutdown(); memset(&q4g, 0, sizeof q4g); return; }
    int natt = 0, nrt = 0;
    const size_t resv = (size_t)16 << 20;
    for (int l = 0; l < c->n_layers; l++) {        /* opcionais, enquanto couber */
        const Layer *L = &m->layers[l];
        Q4Gl *G = &q4g.l[l];
        if (L->lin) continue;
        int f2 = q4g.bytes + resv + qd * 2 * L->wq.stride + d * L->wo.stride > q4g.lim;
        if (!f2) { G->q = q4g_mat1(&L->wq, 2 * qd, d, &f2); G->k = q4g_mat1(&L->wk, kvd, d, &f2); G->v = q4g_mat1(&L->wv, kvd, d, &f2); G->o = q4g_mat1(&L->wo, d, qd, &f2); }
        G->att = !f2; natt += !f2;
    }
    for (int l = 0; l < c->n_layers; l++) {
        int f2 = q4g.bytes + resv + (size_t)c->n_expert * d * 2 > q4g.lim;
        if (!f2) q4g.l[l].rt = q4g_mat1(&q->l[l].router, c->n_expert, d, &f2);
        if (f2) q4g.l[l].rt.ok = 0;
        nrt += q4g.l[l].rt.ok;
    }
    int nsh = 0;
    for (int l = 0; l < c->n_layers; l++) {     /* especialista compartilhado: roda na placa enquanto a CPU faz os roteados */
        const Layer *L = &m->layers[l];
        Q4Gl *G = &q4g.l[l];
        const size_t H = c->hidden;
        int f2 = q4g.bytes + resv + H * (L->sg.stride + L->su.stride) + d * L->sd.stride / 9 * 10 > q4g.lim;
        uint16_t *gb = xalloc(d, 2);        /* o portao e uma linha BF16 (veio de BF16: o resto e zero) */
        for (size_t i = 0; i < d; i++) { uint32_t u; memcpy(&u, L->shexp_gate + i, 4); gb[i] = (uint16_t)(u >> 16); if (u & 0xffff) f2 = 1; }
        Mat gm = { 0 };
        gm.q = (const uint8_t *)gb; gm.type = T_BF16; gm.stride = d * 2;
        if (!f2) { G->sg = q4g_mat1(&L->sg, H, d, &f2); G->su = q4g_mat1(&L->su, H, d, &f2); G->sd = q4g_mat1(&L->sd, d, H, &f2); G->sw = q4g_mat1(&gm, 1, d, &f2); }
        if (f2) G->sw.ok = 0;
        free(gb);
        nsh += G->sw.ok;
    }
    q4g.on = 1;
    fprintf(stderr, "GPU: Vulkan %s - qwen4exp, geracao: mixers e DeltaNet, atencao de %d camadas, roteador de %d, compartilhado de %d, %.0f MB (%.1f s)\n", I.name, natt, nrt, nsh, q4g.bytes / 1048576.0, now_sec() - t0);
}

/* estado do DeltaNet: CPU -> placa (pos: posicao do proximo token; o anel da convolucao e indexado pela posicao) e volta */
static void q4g_sync(Model *m, int pos, int up) {
    const Config *c = &m->cfg;
    const size_t C = q4g.C, ns = (size_t)c->ssm_state * c->ssm_inner * 4;
    float *ring = xalloc(4 * C, 4);
    int ok = 1;
    for (int l = 0; l < c->n_layers && ok; l++) {
        const Layer *L = &m->layers[l];
        if (!L->lin) continue;
        Q4Gl *G = &q4g.l[l];
        float *cv = dn_CV(m, L);
        if (up) {
            memset(ring, 0, 4 * C * 4);
            for (int k = 0; k < 3; k++) if (pos - 3 + k >= 0) memcpy(ring + (size_t)((pos - 3 + k) & 3) * C, cv + (size_t)k * C, C * 4);
            ok = kvk_buf_write_q(G->ring, 0, ring, 4 * C * 4) && kvk_buf_write_q(G->st, 0, dn_S(m, L), ns);
        } else {
            ok = kvk_buf_read(G->ring, 0, ring, 4 * C * 4) && kvk_buf_read(G->st, 0, dn_S(m, L), ns);
            for (int k = 0; k < 3; k++) { if (pos - 3 + k >= 0) memcpy(cv + (size_t)k * C, ring + (size_t)((pos - 3 + k) & 3) * C, C * 4); else memset(cv + (size_t)k * C, 0, C * 4); }
        }
    }
    free(ring);
    if (!ok) die("qwen4exp: falha ao mover o estado da placa");
}
static int q4g_pos;         /* proxima posicao do estado que esta na placa */
static void q4g_home(Model *m) {        /* o estado volta para a CPU */
    if (!q4g.on || !q4g.loc) return;
    q4g_sync(m, q4g_pos, 0);
    q4g.loc = 0;
}
static int q4g_hc(int mode, KvkBuf a, KvkBuf b, KvkBuf o, float eps, uint32_t groups) {
    const KvkBuf bb[3] = { a, b, o };
    return kvk_dispatch(q4g.hc[mode], bb, 3, &eps, 4, groups, 1, 1);
}
/* le os ramos: xn = entrada do bloco; inj (rank + R): pesos da escrita nos R ultimos */
static int q4g_read(const Q4Gm *dn, const Q4Gm *up, KvkBuf norm, KvkBuf inj, float eps) {
    const uint32_t gd = (uint32_t)((q4g.d + 255) / 256);
    return q4g_hc(0, q4g.X, norm, q4g.N, eps, (uint32_t)q4g.R) && q4g_mv(dn, q4g.N, q4g.DN, 0)
        && q4g_hc(1, q4g.DN, q4g.DN, inj, eps, (uint32_t)((q4g.rank + q4g.R + 255) / 256)) && q4g_mv(up, inj, q4g.U, 0)
        && q4g_hc(2, q4g.N, q4g.U, q4g.xn, eps, gd);
}
static void q4_att(Model *m, int l, const float *Xb, float *out, int pos0, int B, XQ *xqs);
static int q4g_write(KvkBuf inj) { return q4g_hc(3, q4g.y, inj, q4g.X, 0, (uint32_t)(((size_t)q4g.R * q4g.d + 255) / 256)); }
/* um token; deixa em m->x a entrada da projecao final */
static void q4g_token(Model *m, int tok, int pos) {
    const Config *c = &m->cfg;
    Q4 *q = m->q4;
    const int d = c->dim, D = q->R * d, qd = c->n_heads * c->hd, kvd = c->n_kv * c->hd;
    static XQ xqs[1];
    float *X = q4_scr(20, D), *xb = q4_scr(21, d + c->n_expert), *Y = q4_scr(22, d), *QG = q4_scr(29, 2 * (size_t)qd), *K = q4_scr(30, kvd), *V = q4_scr(31, kvd);
    if (!q4g.loc) { q4g_sync(m, pos, 1); q4g.loc = 1; }
    q4_pw = 1; q4_ptok[1]++;
    double tk = q4_tick(-1, 0);
    embed_row(m, X, tok);
    for (int r = 1; r < q->R; r++) memcpy(X + (size_t)r * d, X, d * sizeof(float));
    const int mt[4] = { pos, 0, 0, 0 };
    int ok = kvk_buf_write_q(q4g.X, 0, X, (size_t)D * 4) && kvk_buf_write_q(q4g.meta, 0, mt, 16);
    q4_tick(QP_EMB, tk);
    for (int l = 0; l < c->n_layers && ok; l++) {
        const Layer *L = &m->layers[l];
        Q4Gl *G = &q4g.l[l];
        if (l == q->ple_l) {
            ok = kvk_buf_read(q4g.X, 0, X, (size_t)D * 4);
            tk = q4_tick(-1, 0);
            q4_ple(m, &tok, X, 1);
            q4_tick(QP_PLE, tk);
            ok = ok && kvk_buf_write_q(q4g.X, 0, X, (size_t)D * 4);
        }
        ok = ok && q4g_read(&G->dA, &G->uA, G->nA, q4g.iA, c->eps);
        if (L->lin) {
            const KvkBuf sb[9] = { q4g.qkvb, q4g.zb, q4g.abb, G->cw, G->pr, G->ring, G->st, q4g.o, q4g.meta };
            const struct { int pos, kmap; float e1, e2; } sp = { 0, 1, c->eps, c->eps };
            ok = ok && q4g_mv(&G->qkv, q4g.xn, q4g.qkvb, 0) && q4g_mv(&G->z, q4g.xn, q4g.zb, 0) && q4g_mv(&G->ab, q4g.xn, q4g.abb, 0)
                 && kvk_dispatch(q4g.step, sb, 9, &sp, sizeof sp, (uint32_t)q4g.NV, 1, 1) && q4g_mv(&G->out, q4g.o, q4g.y, 0);
        } else {
            tk = q4_tick(-1, 0);
            if (G->att) {
                ok = ok && q4g_mv(&G->q, q4g.xn, q4g.qg, 0) && q4g_mv(&G->k, q4g.xn, q4g.kb, 0) && q4g_mv(&G->v, q4g.xn, q4g.vb, 0);
                const KvkBuf rb[4] = { q4g.xn, q4g.qg, q4g.kb, q4g.vb };
                void *const rd[4] = { xb, QG, K, V };
                const size_t rn[4] = { (size_t)d * 4, (size_t)2 * qd * 4, (size_t)kvd * 4, (size_t)kvd * 4 };
                ok = ok && kvk_buf_readv(rb, rd, rn, 4);
                q4_tick(QP_ATP, tk);
                float *O = q4_scr(11, qd);
                if (!ok) break;
                q4_att_core(m, l, xb, QG, K, V, O, pos, 1, xqs, NULL);
                ok = kvk_buf_write_q(q4g.o, 0, O, (size_t)qd * 4) && q4g_mv(&G->o, q4g.o, q4g.y, 0);
            } else {
                ok = ok && kvk_buf_read(q4g.xn, 0, xb, (size_t)d * 4);
                q4_tick(QP_ATP, tk);
                if (!ok) break;
                q4_att(m, l, xb, Y, pos, 1, xqs);
                ok = kvk_buf_write_q(q4g.y, 0, Y, (size_t)d * 4);
            }
        }
        ok = ok && q4g_write(q4g.iA) && q4g_read(&G->dF, &G->uF, G->nF, q4g.iF, c->eps);
        if (G->rt.ok) ok = ok && q4g_mv(&G->rt, q4g.xn, q4g.xn, d);
        tk = q4_tick(-1, 0);
        ok = ok && kvk_buf_read(q4g.xn, 0, xb, (size_t)(d + (G->rt.ok ? c->n_expert : 0)) * 4);
        q4_tick(QP_HCR, tk);        /* tempo da placa (mixers, DeltaNet, roteador) visto da CPU */
        if (!ok) break;
        if (layer_tap) layer_tap(l, pos, xb);
        if (G->sw.ok) {     /* compartilhado: despachado agora, roda junto com os especialistas da CPU */
            ok = q4g_mv(&G->sg, q4g.xn, q4g.shb, 0) && q4g_mv(&G->su, q4g.xn, q4g.shb, c->hidden) && q4g_mv(&G->sw, q4g.xn, q4g.shs, d)
                 && q4g_hc(4, q4g.shb, q4g.shb, q4g.shh, 0, (uint32_t)((c->hidden + 255) / 256)) && q4g_mv(&G->sd, q4g.shh, q4g.shs, 0)
                 && q4g_hc(5, q4g.shs, q4g.iF, q4g.X, 0, (uint32_t)(((size_t)q4g.R * d + 255) / 256)) && kvk_flush();
            if (!ok) break;
        }
        q4_nosh = G->sw.ok;
        q4_lg = G->rt.ok ? xb + d : NULL;
        q4_moe(m, l, xb, Y, 1, xqs);
        q4_lg = NULL; q4_nosh = 0;
        ok = kvk_buf_write_q(q4g.y, 0, Y, (size_t)d * 4) && q4g_write(q4g.iF);
    }
    ok = ok && q4g_read(&q4g.dO, &q4g.uO, q4g.nO, q4g.iA, c->eps) && kvk_buf_read(q4g.xn, 0, m->x, (size_t)d * 4);
    if (!ok) die("qwen4exp: falha na execucao em Vulkan");
    q4g_pos = pos + 1;
    for (int cl = q->ng - 1; q->n_ple; ) { memmove(q->hist, q->hist + 1, (cl - 1) * sizeof(int)); q->hist[cl - 1] = tok; break; }
}

/* produtos da placa em pesos aleatorios contra a linha dequantizada (q4check) */
static int q4g_test(void) {
    if (!q4g.on) return 1;
    enum { ROWS = 40 };
    const uint32_t ty[8] = { T_Q4_K, T_Q5_K, T_Q6_K, T_IQ3_S, T_IQ4_XS, T_BF16, T_IQ4_NL, T_Q2_0 };
    const int cols[8] = { 2560, 6144, 6144, 2560, 2560, 10240, 640, 640 };
    int ok = 1;
    srand(11);
    for (int k = 0; k < 8; k++) {
        const int C = cols[k];
        const size_t sb = qbytes(ty[k], C), ub = ty[k] == T_BF16 ? 512 : qbytes(ty[k], qunit(ty[k]));
        uint8_t *q = xalloc(ROWS * sb, 1);
        float *x = xalloc(C, 4), *w = xalloc(C, 4), *o = xalloc(ROWS, 4);
        for (size_t i = 0; i < ROWS * sb; i++) q[i] = (uint8_t)(rand() >> 7);
        if (ty[k] == T_BF16) for (int i = 0; i < ROWS * C; i++) { const float v = rand() / (float)RAND_MAX - 0.5f; memcpy(q + 2 * (size_t)i, (const uint8_t *)&v + 2, 2); }
        else for (size_t b = 0; b < ROWS * sb / ub; b++) {
            const uint16_t h = (uint16_t)(0x1800 + (rand() & 0x3ff)), h2 = (uint16_t)(0x1400 + (rand() & 0x3ff));
            uint8_t *p = q + b * ub;
            if (ty[k] == T_Q6_K) memcpy(p + 208, &h, 2); else { memcpy(p, &h, 2); if (ty[k] == T_Q4_K || ty[k] == T_Q5_K) memcpy(p + 2, &h2, 2); }
        }
        for (int i = 0; i < C; i++) x[i] = rand() / (float)RAND_MAX - 0.5f;
        Mat W = { 0 };
        W.q = q; W.type = ty[k]; W.stride = sb;
        int fail = 0;
        Q4Gm g = q4g_mat1(&W, ROWS, C, &fail);
        KvkBuf bx = q4g_vec(x, C, &fail), bo = q4g_buf(ROWS * 4, &fail);
        double worst = 0.0;
        if (fail || !q4g_mv(&g, bx, bo, 0) || !kvk_buf_read(bo, 0, o, ROWS * 4)) { printf("placa, tipo %u: FALHOU (%s)\n", ty[k], kvk_error()); ok = 0; }
        else for (int r = 0; r < ROWS; r++) {
            double e = 0.0, mag = 0.0;
            dequant(ty[k], q + r * sb, w, C);
            for (int i = 0; i < C; i++) { e += (double)w[i] * x[i]; mag += fabs((double)w[i] * x[i]); }
            const double er = fabs(o[r] - e) / mag;
            if (!(er <= worst)) worst = er;
        }
        printf("produto na placa, tipo %u: erro relativo %.1e: %s\n", ty[k], worst, worst < 2e-6 ? "OK" : "FALHOU");
        if (!(worst < 2e-6)) ok = 0;
        kvk_buf_destroy(&g.b); kvk_buf_destroy(&bx); kvk_buf_destroy(&bo);
        free(q); free(x); free(w); free(o);
    }
    return ok;
}

/* katana q4gpu <gguf>: tempo das partes da placa por token (48 camadas), sem o trabalho da CPU */
static int q4g_bench(Model *m) {
    const Config *c = &m->cfg;
    if (!q4g.on) die("q4gpu: placa inativa");
    float *xb = xalloc(2 * (size_t)c->n_heads * c->hd + c->dim + c->n_expert, 4);
    static const char *const nm[] = { "tudo, um envio so", "tudo, leitura por camada", "mixers: rms", "mixers: down", "mixers: silu/sig", "mixers: up", "mixers: mix", "escrita", "deltanet qkv", "deltanet z",
        "deltanet ab", "deltanet passo", "deltanet out", "atencao q", "atencao k v", "atencao out", "roteador", "so leitura (60)" };
    for (int k = 0; k < 18; k++) {
        double best = 1e9;
        for (int rep = 0; rep < 8; rep++) {
            kvk_wait();
            const double t0 = now_sec();
            int ok = 1, nd = 0;
            for (int l = 0; l < c->n_layers; l++) {
                const Layer *L = &m->layers[l];
                Q4Gl *G = &q4g.l[l];
                const KvkBuf sb[9] = { q4g.qkvb, q4g.zb, q4g.abb, G->cw, G->pr, G->ring, G->st, q4g.o, q4g.meta };
                const struct { int pos, kmap; float e1, e2; } sp = { 0, 1, c->eps, c->eps };
                const int all = k < 2;
                for (int h = 0; h < 2; h++) {
                    if (all || k == 2) ok &= q4g_hc(0, q4g.X, h ? G->nF : G->nA, q4g.N, c->eps, (uint32_t)q4g.R), nd++;
                    if (all || k == 3) ok &= q4g_mv(h ? &G->dF : &G->dA, q4g.N, q4g.DN, 0), nd++;
                    if (all || k == 4) ok &= q4g_hc(1, q4g.DN, q4g.DN, h ? q4g.iF : q4g.iA, c->eps, 2), nd++;
                    if (all || k == 5) ok &= q4g_mv(h ? &G->uF : &G->uA, h ? q4g.iF : q4g.iA, q4g.U, 0), nd++;
                    if (all || k == 6) ok &= q4g_hc(2, q4g.N, q4g.U, q4g.xn, c->eps, 10), nd++;
                    if (!h && L->lin) {
                        if (all || k == 8) ok &= q4g_mv(&G->qkv, q4g.xn, q4g.qkvb, 0);
                        if (all || k == 9) ok &= q4g_mv(&G->z, q4g.xn, q4g.zb, 0);
                        if (all || k == 10) ok &= q4g_mv(&G->ab, q4g.xn, q4g.abb, 0);
                        if (all || k == 11) ok &= kvk_dispatch(q4g.step, sb, 9, &sp, sizeof sp, (uint32_t)q4g.NV, 1, 1);
                        if (all || k == 12) ok &= q4g_mv(&G->out, q4g.o, q4g.y, 0);
                    } else if (!h && G->att) {
                        if (all || k == 13) ok &= q4g_mv(&G->q, q4g.xn, q4g.qg, 0);
                        if (all || k == 14) ok &= q4g_mv(&G->k, q4g.xn, q4g.kb, 0) && q4g_mv(&G->v, q4g.xn, q4g.vb, 0);
                        if (k == 1) ok &= kvk_buf_read(q4g.qg, 0, xb, 4096);
                        if (all || k == 15) ok &= q4g_mv(&G->o, q4g.o, q4g.y, 0);
                    }
                    if (all || k == 7) ok &= q4g_write(h ? q4g.iF : q4g.iA), nd++;
                }
                if ((all || k == 16) && G->rt.ok) ok &= q4g_mv(&G->rt, q4g.xn, q4g.xn, c->dim);
                if (k == 1 || k == 17) ok &= kvk_buf_read(q4g.xn, 0, xb, (size_t)(c->dim + c->n_expert) * 4);
            }
            if (k == 17) for (int i = 0; i < 12; i++) ok &= kvk_buf_read(q4g.xn, 0, xb, 4096);
            if (!kvk_wait() || !ok) die("q4gpu: falha");
            const double t = now_sec() - t0;
            if (t < best) best = t;
        }
        printf("%-28s %7.2f ms/token\n", nm[k], best * 1e3);
    }
    return 0;
}
