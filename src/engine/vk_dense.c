/* ---------- grafo denso completo em Vulkan (um token por vez) ----------
   Mesma sequencia do gpu_forward_n do OpenCL: norma, q/k/v, atencao fundida (att1f), saida com residual,
   norma, gate/up com SwiGLU, down com residual; no fim norma final e vocabulario. A posicao do token vem
   de um buffer pequeno (meta), entao o mesmo grafo gravado serve para todos os tokens. */
static void mat_row(float *out, const Mat *W, int row, int cols);
static void cpu_layers(Model *m, int l0, int l1, int pos);
static void cpu_layers_n(Model *m, int l0, int l1, int pos0, int B, float *X);
static float *cpu_head(Model *m);

typedef struct {
    int on, rec, kv16;
    int nl;                         /* camadas no grafo Vulkan; < n_layers: o resto e a cabeca na CPU */
    KvkBuf x, xn, q, k, v, o, hb, lg, att, meta, freq, onorm, dummy;
    KvkBuf *an, *fn, *bq, *bk, *bv, *bo, *qn, *kn, *kc, *vc;    /* b==0: ausente */
    KvkPipe rms, att1f, att1r;
    KvkBuf part;                    /* pedacos do contexto da atencao de um token (split-K) */
    int chunk, nchunk;              /* posicoes por pedaco e numero de pedacos (1 = sem divisao, sem att1r) */
    KvkRec g[2];                    /* grafo gravado: [0] sem a projecao final, [1] com ela */
    int coop, sg32;                 /* produto cooperativo fp16 disponivel; subgrupo de 32 */
    int P;                          /* maior lote por passada em Vulkan (0 = so token a token) */
    int g_ok[2];
    double t_emb, t_sub, t_wait;    /* embedding+escrita, enfileirar/submeter, espera+leitura */
} VkG;
static VkG vkg;
#define VK_PASS_DEF 256
static int vk_batch_build(Model *m);
static int vk_emit_head(const Model *m);

static KvkBuf vk_wk(size_t bytes, int *fail) {
    KvkBuf b = kvk_buf_create(bytes < 16 ? 16 : bytes, KVK_USAGE_RW);
    if (!b.b) { *fail = 1; return b; }
    gpu.used += bytes;
    return b;
}

/* vetor float32 constante na VRAM; handle zerado se src for NULL */
static KvkBuf vk_vec(const float *src, size_t n, int *fail) {
    KvkBuf z; memset(&z, 0, sizeof z);
    if (!src) return z;
    KvkBuf b = kvk_buf_create(n * sizeof(float), KVK_USAGE_RW);
    if (!b.b || !kvk_buf_write_q(b, 0, src, n * sizeof(float))) { *fail = 1; return z; }
    gpu.used += n * sizeof(float);
    return b;
}

/* pesos de uma camada densa na VRAM (mesma conta do upload) */
static size_t vk_layer_bytes(const Model *m, int l) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, qd = (size_t)c->n_heads * c->hd;
    const Layer *L = &m->layers[l];
    return gpu_mat_bytes(&L->wq, qd, d) + gpu_mat_bytes(&L->wk, kvd, d) + gpu_mat_bytes(&L->wv, kvd, d)
         + gpu_mat_bytes(&L->wo, d, qd)
         + (c->n_expert ? 0 : gpu_mat_bytes(&L->w_gate, H, d) + gpu_mat_bytes(&L->w_up, H, d) + gpu_mat_bytes(&L->w_down, d, H));
}

/* decide quantas camadas o grafo leva (pesos + vetores + cache KV + buffers de trabalho) e o tipo do cache.
   Devolve o numero de camadas (0 = sem grafo). Se nem tudo cabe: as primeiras camadas na GPU, o resto e a
   cabeca na CPU, cache KV em 16 bits e espaco reservado para o lote do prompt. */
static int vk_plan(const Model *m, uint64_t budget) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab, qd = (size_t)c->n_heads * c->hd;
    const int NL = c->n_layers;
    if (getenv("KATANA_VK_GRAPH") && atoi(getenv("KATANA_VK_GRAPH")) == 0) return 0;
    if (c->hd > 256 || c->hd % 2) { fprintf(stderr, "Vulkan: cabeca de %d nao suportada pelo grafo, grafo na CPU\n", c->hd); return 0; }
    const size_t head = gpu_mat_bytes(&m->lm_head, V, d);
    const size_t peq_l = (2 * d + qd + 2 * kvd + d + (c->qk_norm ? d + kvd : 0)) * 4, kv32_l = 2 * (size_t)c->ctx * kvd * 4;
    size_t w = 0;
    for (int l = 0; l < NL; l++) w += vk_layer_bytes(m, l);
    const size_t pequenos = d * 4 + NL * peq_l, kv32 = NL * kv32_l;
    const size_t work = (d + 2 * (qd > d ? qd : d) + qd + 2 * kvd + H + V + (size_t)c->n_heads * c->ctx + 256) * 4;
    const size_t mx = d > qd ? (d > H ? d : H) : (qd > H ? qd : H);
    /* bytes do lote por token; MoE: entrada fp32, colunas juntadas, saidas (GPU e CPU), logits do roteador e tabela */
    const size_t per = d * 4 + mx * 2 + 2 * qd * 4 + 2 * kvd * 4 + H * 2
                     + (c->n_expert ? d * 4 * 4 + H * 2 + (size_t)c->n_expert * 4 + (size_t)8 * (c->n_used + 2) : 0);
    const int batch = vkg.coop && !(c->n_expert && (c->mla || c->ssm_inner)) && !(getenv("KATANA_VK_BATCH") && atoi(getenv("KATANA_VK_BATCH")) == 0);
    size_t P = getenv("KATANA_VK_PASS") ? (size_t)atoi(getenv("KATANA_VK_PASS")) : VK_PASS_DEF;
    /* MoE: os especialistas ocupam o que sobra; cada camada precisa de pelo menos n_used vagas */
    size_t wexp_min = 0, wexp_all = 0;
    if (c->n_expert) for (int l = 0; l < NL; l++) {
        const Layer *L = &m->layers[l];
        const size_t eb = 2 * gpu_mat_bytes(&L->w_gate, H, d) + gpu_mat_bytes(&L->w_down, d, H);
        wexp_min += (size_t)c->n_used * eb; wexp_all += (size_t)c->n_expert * eb;
    }
    int kv16 = getenv("KATANA_KV16") ? atoi(getenv("KATANA_KV16")) != 0
             : gpu.used + head + w + wexp_all + pequenos + work + kv32 + ((size_t)256 << 20) > budget;
    size_t need = head + w + wexp_min + pequenos + work + (kv16 ? kv32 / 2 : kv32);
    int nl = NL;
    if (gpu.used + need > budget && c->n_expert) {
        fprintf(stderr, "Vulkan: MoE nao cabe no grafo (atencao + %d vagas por camada: %.0f MB, orcamento %.0f MB): grafo na CPU\n",
                c->n_used, need / 1048576.0, budget / 1048576.0);
        return 0;
    }
    if (gpu.used + need > budget) {
        if (!getenv("KATANA_KV16")) kv16 = 1;
        const size_t reserve = batch ? P * per : 0;
        size_t acc = gpu.used + d * 4 + work + reserve;
        nl = 0;
        for (int l = 0; l < NL; l++) {
            const size_t lb = vk_layer_bytes(m, l) + peq_l + (kv16 ? kv32_l / 2 : kv32_l);
            if (acc + lb > budget) break;
            acc += lb; nl++;
        }
        if (nl < 1) { fprintf(stderr, "Vulkan: nenhuma camada cabe no grafo (orcamento %.0f MB): grafo na CPU\n", budget / 1048576.0); return 0; }
        need = acc - gpu.used - reserve;
        fprintf(stderr, "Vulkan: grafo parcial: %d de %d camadas na GPU (%.0f MB), o resto e a cabeca na CPU\n", nl, NL, need / 1048576.0);
    }
    vkg.kv16 = kv16;
    vkg.nl = nl;
    /* lote do prompt: buffers de trabalho por token (ativacoes transpostas); o que sobrar do orcamento limita o lote */
    vkg.P = 0;
    if (batch) {
        const size_t left = budget > gpu.used + need ? budget - gpu.used - need : 0;
        if (P * per > left) P = left / per;
        if (P > 1024) P = 1024;
        vkg.P = P >= 2 ? (int)P : 0;
    }
    return nl;
}

/* cria vetores, cache KV, buffers de trabalho e pipelines do grafo; 0 se algo falhar */
static int vk_graph_build(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab, qd = (size_t)c->n_heads * c->hd;
    const size_t NL = c->n_layers, dq = qd > d ? qd : d;
    int fail = 0;
    for (size_t l = 0; l < (size_t)vkg.nl; l++) {
        const Layer *L = &m->layers[l];
        if (!L->wq.gbuf || !L->wk.gbuf || !L->wv.gbuf || !L->wo.gbuf) return 0;
        if (!c->n_expert && (!L->w_gate.gbuf || !L->w_up.gbuf || !L->w_down.gbuf)) return 0;
    }
    if (vkg.nl == c->n_layers && !m->lm_head.gbuf) return 0;
    /* MoE: os logits do roteador ficam logo depois da entrada normalizada (uma leitura traz os dois) */
    const size_t nxn = c->n_expert && d + c->n_expert > dq ? d + c->n_expert : dq;
    vkg.x = vk_wk(d * 4, &fail); vkg.xn = vk_wk(nxn * 4, &fail); vkg.q = vk_wk(dq * 4, &fail);
    vkg.k = vk_wk(kvd * 4, &fail); vkg.v = vk_wk(kvd * 4, &fail); vkg.o = vk_wk(qd * 4, &fail);
    vkg.hb = vk_wk(H * 4, &fail); vkg.lg = vk_wk((V + 128) * 4, &fail);
    vkg.att = vk_wk((size_t)c->n_heads * c->ctx * 4, &fail); vkg.meta = vk_wk(16, &fail); vkg.dummy = vk_wk(256, &fail);
    /* atencao de um token: pedacos de 128 posicoes (no maximo 32 pedacos); contexto <= 128 nao divide */
    vkg.chunk = 128 * ((c->ctx + 128 * 32 - 1) / (128 * 32));
    vkg.nchunk = (c->ctx + vkg.chunk - 1) / vkg.chunk;
    if (getenv("KATANA_VK_NOSPLIT")) { vkg.chunk = c->ctx; vkg.nchunk = 1; }
    vkg.part = vk_wk((size_t)c->n_heads * vkg.nchunk * (c->hd + 2) * 4, &fail);
    vkg.freq = vk_vec(m->rope_freq, c->hd / 2, &fail); vkg.onorm = vk_vec(m->out_norm, d, &fail);
    vkg.an = xalloc(NL, sizeof(KvkBuf)); vkg.fn = xalloc(NL, sizeof(KvkBuf)); vkg.bq = xalloc(NL, sizeof(KvkBuf));
    vkg.bk = xalloc(NL, sizeof(KvkBuf)); vkg.bv = xalloc(NL, sizeof(KvkBuf)); vkg.bo = xalloc(NL, sizeof(KvkBuf));
    vkg.qn = xalloc(NL, sizeof(KvkBuf)); vkg.kn = xalloc(NL, sizeof(KvkBuf));
    vkg.kc = xalloc(NL, sizeof(KvkBuf)); vkg.vc = xalloc(NL, sizeof(KvkBuf));
    const size_t kvb = (size_t)c->ctx * kvd * (vkg.kv16 ? 2 : 4);
    for (size_t l = 0; l < (size_t)vkg.nl && !fail; l++) {
        const Layer *L = &m->layers[l];
        vkg.an[l] = vk_vec(L->attn_norm, d, &fail); vkg.fn[l] = vk_vec(L->ffn_norm, d, &fail);
        vkg.bq[l] = vk_vec(L->bq, qd, &fail); vkg.bk[l] = vk_vec(L->bk, kvd, &fail);
        vkg.bv[l] = vk_vec(L->bv, kvd, &fail); vkg.bo[l] = vk_vec(L->bo, d, &fail);
        if (c->qk_norm) {
            vkg.qn[l] = vk_vec(L->q_norm, c->qk_norm == 1 ? d : (size_t)c->hd, &fail);
            vkg.kn[l] = vk_vec(L->k_norm, c->qk_norm == 1 ? kvd : (size_t)c->hd, &fail);
        }
        vkg.kc[l] = vk_wk(kvb, &fail); vkg.vc[l] = vk_wk(kvb, &fail);
    }
    if (fail) { fprintf(stderr, "Vulkan: sem memoria para o grafo completo (%s)\n", kvk_error()); return 0; }
    kvk_wait();
    vkg.rms = kvk_pipe_create(rms_wg_spv, sizeof rms_wg_spv, 3, 12, NULL, NULL, 0);
    const uint32_t id0 = 0, hdv = (uint32_t)c->hd;
    vkg.att1f = vkg.kv16 ? kvk_pipe_create(att1f_kv16_spv, sizeof att1f_kv16_spv, 15, 64, &id0, &hdv, 1)
                         : kvk_pipe_create(att1f_kv32_spv, sizeof att1f_kv32_spv, 15, 64, &id0, &hdv, 1);
    vkg.att1r = kvk_pipe_create(att1r_spv, sizeof att1r_spv, 2, 8, &id0, &hdv, 1);
    if (!vkg.rms.p || !vkg.att1f.p || !vkg.att1r.p) { fprintf(stderr, "Vulkan: pipeline do grafo falhou: %s\n", kvk_error()); return 0; }
    vkg.on = 1;
    vk_batch_build(m);
    return 1;
}

/* norma final + projecao para o vocabulario do vetor vkg.x (um token) */
static int vk_emit_head(const Model *m) {
    const Config *c = &m->cfg;
    const struct { int n; float eps; int B; } rp = { c->dim, c->eps, 1 };
    const KvkBuf rb[3] = { vkg.x, vkg.onorm, vkg.xn };
    int ok = kvk_dispatch(vkg.rms, rb, 3, &rp, sizeof rp, 1, 1, 1);
    ok &= vk_gmv_ex(&m->lm_head, vkg.xn, vkg.lg, vkg.dummy, c->vocab, c->dim, 0, 0);
    return ok;
}

/* atencao da camada l (norma, q/k/v, atencao fundida, saida com residual) e a norma do feed-forward em vkg.xn */
#define OR_DUMMY(v) ((v).b ? (v) : vkg.dummy)
static int vk_emit_attn_x(const Model *m, int l, int pre_norm);
static int vk_pos_live = -1;    /* grafo ao vivo (MoE): posicao do token, so os pedacos de contexto ja ocupados */
static int vk_emit_attn(const Model *m, int l) { return vk_emit_attn_x(m, l, 0); }
/* pre_norm = 1: vkg.xn ja tem a norma de entrada desta camada (somada junto com o MoE da anterior) */
static int vk_emit_attn_x(const Model *m, int l, int pre_norm) {
    const Config *c = &m->cfg;
    const int d = c->dim, hd = c->hd, kvd = c->n_kv * hd, qd = c->n_heads * hd, nh = c->n_heads;
    const struct { int n; float eps; int B; } rp = { d, c->eps, 1 };
    const Layer *L = &m->layers[l];
    int ok = 1;
    const KvkBuf rb[3] = { vkg.x, vkg.an[l], vkg.xn };
    if (!pre_norm) ok &= kvk_dispatch(vkg.rms, rb, 3, &rp, sizeof rp, 1, 1, 1);
    ok &= vk_gmv_ex(&L->wq, vkg.xn, vkg.q, vkg.dummy, qd, d, 0, 0);
    kvk_set_auto_barrier(0);        /* q, k e v sao independentes: uma barreira antes do primeiro basta */
    ok &= vk_gmv_ex(&L->wk, vkg.xn, vkg.k, vkg.dummy, kvd, d, 0, 0);
    ok &= vk_gmv_ex(&L->wv, vkg.xn, vkg.v, vkg.dummy, kvd, d, 0, 0);
    kvk_set_auto_barrier(1);
    const KvkBuf ab[15] = { vkg.q, vkg.k, vkg.v, OR_DUMMY(vkg.bq[l]), OR_DUMMY(vkg.bk[l]), OR_DUMMY(vkg.bv[l]),
                            vkg.kc[l], vkg.vc[l], vkg.att, vkg.o, vkg.freq, OR_DUMMY(vkg.qn[l]), OR_DUMMY(vkg.kn[l]), vkg.meta, vkg.part };
    int nch = vkg.nchunk;
    if (vk_pos_live >= 0 && vk_pos_live / vkg.chunk + 1 < nch) nch = vk_pos_live / vkg.chunk + 1;
    const struct { int base, ctx, hd, kvd, group, neox, has_bias, norm, d; float scale, eps; int chunk, nchunk, nrot, qst, gate; } ap =
        { 0, c->ctx, hd, kvd, nh / c->n_kv, c->rope_neox, (vkg.bq[l].b ? 1 : 0) | (vkg.bk[l].b ? 2 : 0) | (vkg.bv[l].b ? 4 : 0),
          c->qk_norm, qd, 1.0f / sqrtf((float)hd), c->eps, vkg.chunk, nch, hd, hd, 0 };
    ok &= kvk_dispatch(vkg.att1f, ab, 15, &ap, sizeof ap, (uint32_t)nh, (uint32_t)nch, 1);
    if (nch > 1) {
        const KvkBuf pb[2] = { vkg.part, vkg.o };
        const struct { int hd, nchunk; } rp2 = { hd, nch };
        ok &= kvk_dispatch(vkg.att1r, pb, 2, &rp2, sizeof rp2, (uint32_t)nh, 1, 1);
    }
    ok &= vk_gmv_ex(&L->wo, vkg.o, vkg.x, OR_DUMMY(vkg.bo[l]), d, qd, 1 | (vkg.bo[l].b ? 4 : 0), 0);
    const KvkBuf rb2[3] = { vkg.x, vkg.fn[l], vkg.xn };
    ok &= kvk_dispatch(vkg.rms, rb2, 3, &rp, sizeof rp, 1, 1, 1);
    return ok;
}

/* enfileira (ao vivo ou na gravacao) todos os despachos de um token denso; head = norma final + vocabulario */
static int vk_emit(const Model *m, int head) {
    const Config *c = &m->cfg;
    const int d = c->dim, H = c->hidden;
    int ok = 1;
    for (int l = 0; l < vkg.nl && ok; l++) {
        const Layer *L = &m->layers[l];
        ok &= vk_emit_attn(m, l);
        if (L->w_gate.pair) ok &= vk_gmv_ex(&L->w_gate, vkg.xn, vkg.hb, vkg.dummy, H, d, 0, 1);
        else {
            ok &= vk_gmv_ex(&L->w_gate, vkg.xn, vkg.hb, vkg.dummy, H, d, 0, 0);
            ok &= vk_gmv_ex(&L->w_up, vkg.xn, vkg.hb, vkg.dummy, H, d, 2, 0);
        }
        ok &= vk_gmv_ex(&L->w_down, vkg.hb, vkg.x, vkg.dummy, d, H, 1, 0);
    }
    if (ok && head) ok &= vk_emit_head(m);
    return ok;
}
#undef OR_DUMMY

static float *vk_forward_moe(Model *m, int want_logits);
static float *vk_qn_forward_1(Model *m, int want_logits);
static float *vk_qn_forward_n(Model *m, int B, int pos0, int want_logits);
static void vkq_setup(Model *m);
static int vkq_posn;      /* posicao do primeiro token do lote em curso do qwen35moe (layer_tap) */
static int vkq_angles(const Model *m, int pos0, int B);
static int vkq_pos1;      /* posicao do token no caminho de um token do qwen35moe */

/* um token pelas camadas todas na GPU; devolve os logits (m->logits) ou NULL se !want_logits */
static float *vk_forward_1(Model *m, int token, int pos, int want_logits) {
    const Config *c = &m->cfg;
    double t0 = now_sec();
    if (pos < 0 || pos >= c->ctx) die("posicao fora do contexto (Vulkan)");
    embed_row(m, m->x, token);
    const int meta[4] = { pos, vis_rp && c->ssm_inner ? vkq_angles(m, pos, 1) : 0, 0, 0 };      /* houve imagem: angulos prontos no lugar de pos * freq */
    if (!kvk_buf_write_q(vkg.x, 0, m->x, (size_t)c->dim * sizeof(float)) || !kvk_buf_write_q(vkg.meta, 0, meta, sizeof meta))
        die("falha na execucao em Vulkan (escrita)");
    if (c->ssm_inner) { vkq_pos1 = pos; return vk_qn_forward_1(m, want_logits); }
    if (c->n_expert) { vk_pos_live = pos; float *lg = vk_forward_moe(m, want_logits); vk_pos_live = -1; return lg; }
    double t1 = now_sec();
    const int h = want_logits && vkg.nl == c->n_layers ? 1 : 0;
    if (vkg.rec) {
        if (!vkg.g_ok[h]) {
            if (!kvk_rec_begin()) die("falha ao gravar o grafo Vulkan");
            if (!vk_emit(m, h)) { kvk_rec_cancel(); die("falha ao gravar o grafo Vulkan"); }
            vkg.g[h] = kvk_rec_end();
            if (!vkg.g[h].cb) die("falha ao gravar o grafo Vulkan");
            vkg.g_ok[h] = 1;
            t1 = now_sec();
        }
        if (!kvk_rec_submit(vkg.g[h])) die("falha na execucao em Vulkan (submit)");
    } else {
        if (!vk_emit(m, h) || !kvk_flush()) die("falha na execucao em Vulkan");
    }
    double t2 = now_sec();
    if (vkg.nl < c->n_layers) {     /* grafo parcial: o vetor volta e a CPU faz o resto */
        if (!kvk_buf_read(vkg.x, 0, m->x, (size_t)c->dim * sizeof(float))) die("falha na execucao em Vulkan (leitura)");
        vkg.t_emb += t1 - t0; vkg.t_sub += t2 - t1; vkg.t_wait += now_sec() - t2;
        cpu_layers(m, vkg.nl, c->n_layers, pos);
        return want_logits ? cpu_head(m) : NULL;
    }
    if (!want_logits) { vkg.t_emb += t1 - t0; vkg.t_sub += t2 - t1; return NULL; }
    if (!kvk_buf_read(vkg.lg, 0, m->logits, (size_t)c->vocab * sizeof(float))) die("falha na execucao em Vulkan (leitura)");
    double t3 = now_sec();
    vkg.t_emb += t1 - t0; vkg.t_sub += t2 - t1; vkg.t_wait += t3 - t2;
    return m->logits;
}

/* ---------- prompt em lote em Vulkan (produtos em matrizes cooperativas fp16) ----------
   Camadas na mesma ordem do grafo de um token, mas para B tokens por passada. Layout das ativacoes: TRANSPOSTO,
   X[coluna*B + token] (o mm.comp le x[col*tokens + token] e grava y[linha*tokens + token]; qkv_post, rms e a atencao
   usam o mesmo layout), entao a saida de um produto e a entrada do seguinte sem copiar. Entradas dos produtos em fp16:
   a norma ja grava fp16 (rms_wg16) e a atencao tambem; so a saida do gate/up (fp32) passa por uma conversao.
   Residual em fp32 (bx), produto com modo "soma". So o ULTIMO token vai a norma final e ao vocabulario (gather da
   ultima coluna para o vetor de um token e o mesmo caminho do vk_forward_1). Os produtos arredondam pesos e ativacoes
   a fp16 dentro dos ladrilhos: o resultado difere do caminho float (comparar com `selftest`). */
typedef struct {
    int ok, P, flash;
    KvkBuf x, xh, q, q2, k, v, hb, o3, att3;
    KvkPipe rms16, qkv, flash_p, sc, sm, mix, cvt, gat;
    KvkPipe mm[6][6];               /* [tipo do mm.comp][indice do ladrilho, plano ou glu] */
    KvkPipe mmx[3][6][3];           /* variantes precisas: [0] pesos em alta + resto (ws), [1] tambem ativacao fp32 (xs), [2] glu xs com saida fp32 */
    float *xT;
    unsigned skip;                  /* KATANA_VK_SKIP: bits de fases a omitir (so para medir o custo de cada uma) */
    double t_emb, t_emit, t_wait; long passes, toks;
} VkB;
static VkB vkb;
/* MoE do lote (grafo completo): entrada normalizada em fp32 (roteador e frios) e fp16 (especialistas), colunas de
   cada vaga juntadas (moe_gath), saida de cada vaga (ys) somada pesada no residual (moe_scat), frios somados (yc) */
static struct {
    int on, prec;
    KvkBuf xf, xg, ys, yc, lg, meta;
    KvkPipe rms32, gath, gath32, scat, flash32;
    float *xn, *lgT, *xc, *outs, *sw, *pw, *cw; uint32_t *mt;
    int *cnt, *ofs, *pv, *pb, *ce, *cb;
    /* frios do lote levados a GPU: T vagas temporarias (tg, td), para os frios com pelo menos cmin tokens no lote */
    KvkBuf tg, td; int T, cmin, *ccnt, *tmpe; size_t tgb, tdb; uint8_t *stage; FILE **pf;
    uint8_t *tstage;                /* T regioes (tgb + tdb): a thread de leitura grava do arquivo, a principal sobe com write_q */
    int cdone, off_c;               /* frios da CPU ja somados em xc; deslocamento da tabela deles */
    double t_rd, t_route, t_cpu, t_tmp, t_frd; long n_hot, n_cold, n_tmp, n_tmpe;
} vkbm;

/* tipo do mm.comp: 0 Q4_K, 1 Q6_K, 2 Q5_K, 3 Q8_0, 4 F32, 5 Q5_1; vk_kind_idx: 0 F32, 1 Q8_0, 2 Q4_K, 3 Q5_K, 4 Q6_K,
   5 Q2_K (sem produto em lote: os chamadores o excluem), 6 Q5_1 */
static const int vk_mmt[7] = { 4, 3, 0, 2, 1, 4, 5 };
/* variantes: 0 64x64, 1 64x32w4 (so plano), 2 64x16w4 (so plano), 3 64x32 (glu), 4 64x16 (glu) */
static const int vk_mm_bn[5] = { 64, 32, 16, 32, 16 };
static const uint32_t *vk_mm_spv(int t, int v, int glu, size_t *n) {
#define MMS(T) \
    { { mm_t##T##_64x64_g0_spv, sizeof mm_t##T##_64x64_g0_spv }, { mm_t##T##_64x32w4_g0_spv, sizeof mm_t##T##_64x32w4_g0_spv }, \
      { mm_t##T##_64x16w4_g0_spv, sizeof mm_t##T##_64x16w4_g0_spv }, { mm_t##T##_64x64_g1_spv, sizeof mm_t##T##_64x64_g1_spv }, \
      { mm_t##T##_64x32_g1_spv, sizeof mm_t##T##_64x32_g1_spv }, { mm_t##T##_64x16_g1_spv, sizeof mm_t##T##_64x16_g1_spv } }
    static const struct { const uint32_t *p; size_t n; } tab[6][6] = { MMS(0), MMS(1), MMS(2), MMS(3), MMS(4), MMS(5) };
#undef MMS
    /* indice: plano 0,1,2 = 64x64, 64x32w4, 64x16w4; glu 3,4,5 = 64x64, 64x32, 64x16 */
    const int ix = glu ? (v == 0 ? 3 : v == 3 ? 4 : 5) : v;
    *n = tab[t][ix].n;
    return tab[t][ix].p;
}

/* regra de escolha do ladrilho (tools/vkq4k/NOTAS.md): ate 16 tokens 64x16; glu acima disso 64x32 (2x2);
   plano ate 64 tokens 64x32w4, acima 64x64 (4x2). Ate 2 acumuladores por subgrupo em todos. */
static int vk_tile(int ki, int B, int glu) {
    (void)ki;
    if (B <= 16) return glu ? 4 : 2;
    if (glu) return 3;              /* 64x32 em 2x2 subgrupos: melhor que o 64x64 em todos os lotes medidos */
    return B <= 64 ? 1 : 0;         /* 64x32w4; acima de 64 tokens 64x64 em 4x2 subgrupos */
}

/* y = W * x (B tokens), x fp16 [cols][B], y fp32 [rows][B]; add: y += W*x; glu: W e o par gate/up intercalado (y = silu(gate)*up) */
static int vk_mm_raw(uint32_t kind, KvkBuf wb, uint32_t woff, size_t wpr, KvkBuf xh, KvkBuf y, int rows, int cols, int B, int add, int glu) {
    const int ki = vk_kind_idx(kind), t = vk_mmt[ki], v = vk_tile(ki, B, glu);
    KvkPipe *pp = &vkb.mm[t][glu ? (v == 0 ? 3 : v == 3 ? 4 : 5) : v];
    if (!pp->p) {
        size_t n; const uint32_t *spv = vk_mm_spv(t, v, glu, &n);
        *pp = kvk_pipe_create(spv, n, 3, 32, NULL, NULL, 0);
        if (!pp->p) { fprintf(stderr, "Vulkan: pipeline do produto em lote falhou: %s\n", kvk_error()); return 0; }
    }
    const uint32_t pc[8] = { (uint32_t)rows, (uint32_t)cols, (uint32_t)B, (uint32_t)wpr, (uint32_t)cols, 0, (uint32_t)add, woff };
    const KvkBuf bufs[3] = { wb, xh, y };
    const int bm = 64, bn = vk_mm_bn[v], gm = glu ? bm / 2 : bm;
    return kvk_dispatch(*pp, bufs, 3, pc, sizeof pc, (uint32_t)((rows + gm - 1) / gm), (uint32_t)((B + bn - 1) / bn), 1);
}
/* produto preciso: peso decodificado em duas partes fp16 (alta + resto*2048) e, com xs, a ativacao em fp32 [cols][B]
   tambem em alta + resto; tres produtos sobre o mesmo ladrilho (o resto x resto fica de fora), erro ~1e-6 relativo
   contra ~1e-4 do produto fp16. O qwen35moe em lote usa so este: o roteamento top-8 tem quase empates (margem de
   1e-5) que o erro do fp16 vira, e um especialista trocado leva o token a ~5% de erro (docs/new-models.md) */
static int vk_mm_pw(uint32_t kind, KvkBuf wb, uint32_t woff, size_t wpr, KvkBuf x, KvkBuf y, int rows, int cols, int B, int add, int xs);
static int vk_mm_p(const Mat *W, KvkBuf x, KvkBuf y, int rows, int cols, int B, int add, int xs) {
    return vk_mm_pw(gpu_kind(W), *(const KvkBuf *)W->gbuf, 0, gpu_wpr(W, cols), x, y, rows, cols, B, add, xs);
}
/* idem com o buffer de pesos explicito (vaga de especialista: woff); xs = 2: glu (gate/up intercalados), y fp32 */
static int vk_mm_pw(uint32_t kind, KvkBuf wb, uint32_t woff, size_t wpr, KvkBuf x, KvkBuf y, int rows, int cols, int B, int add, int xs) {
#define MMX(T, S) { { mm_t##T##_64x64_##S##_spv, sizeof mm_t##T##_64x64_##S##_spv }, { mm_t##T##_64x32w4_##S##_spv, sizeof mm_t##T##_64x32w4_##S##_spv }, \
                    { mm_t##T##_64x16w4_##S##_spv, sizeof mm_t##T##_64x16w4_##S##_spv } }
#define MMG(T) { { mm_t##T##_64x64_gx_spv, sizeof mm_t##T##_64x64_gx_spv }, { mm_t##T##_64x32_gx_spv, sizeof mm_t##T##_64x32_gx_spv }, \
                 { mm_t##T##_64x16_gx_spv, sizeof mm_t##T##_64x16_gx_spv } }
    static const struct { const uint32_t *p; size_t n; } tab[3][6][3] = { { MMX(0, ws), MMX(1, ws), MMX(2, ws), MMX(3, ws), MMX(4, ws), MMX(5, ws) },
                                                                          { MMX(0, xs), MMX(1, xs), MMX(2, xs), MMX(3, xs), MMX(4, xs), MMX(5, xs) },
                                                                          { MMG(0), MMG(1), MMG(2), MMG(3), MMG(4), MMG(5) } };
#undef MMX
#undef MMG
    const int glu = xs == 2, ki = vk_kind_idx(kind), t = vk_mmt[ki], v0 = vk_tile(ki, B, glu);
    const int v = glu ? (v0 == 0 ? 0 : v0 == 3 ? 1 : 2) : v0;      /* glu: 64x64, 64x32, 64x16 */
    KvkPipe *pp = &vkb.mmx[xs][t][v];
    if (!pp->p) {
        *pp = kvk_pipe_create(tab[xs][t][v].p, tab[xs][t][v].n, 3, 32, NULL, NULL, 0);
        if (!pp->p) { fprintf(stderr, "Vulkan: pipeline do produto em lote (XS) falhou: %s\n", kvk_error()); return 0; }
    }
    const uint32_t ad = (uint32_t)add | (glu && g_gelu ? 2u : 0u);     /* bit 1: GELU no glu (gemma4) */
    const uint32_t pc[8] = { (uint32_t)rows, (uint32_t)cols, (uint32_t)B, (uint32_t)wpr, (uint32_t)cols, 0, ad, woff };
    const KvkBuf bufs[3] = { wb, x, y };
    const int gm = glu ? 32 : 64;
    return kvk_dispatch(*pp, bufs, 3, pc, sizeof pc, (uint32_t)((rows + gm - 1) / gm), (uint32_t)((B + vk_mm_bn[v0] - 1) / vk_mm_bn[v0]), 1);
}
static int vk_mm(const Mat *W, KvkBuf xh, KvkBuf y, int rows, int cols, int B, int add, int glu) {
    return vk_mm_raw(gpu_kind(W), *(const KvkBuf *)W->gbuf, 0, gpu_wpr(W, cols), xh, y, rows, cols, B, add, glu);
}

/* o lote Vulkan so vale para camadas que o caminho acima sabe fazer */
static int vk_batch_supported(const Model *m) {
    const Config *c = &m->cfg;
    const int d = c->dim, qd = c->n_heads * c->hd, H = c->hidden;
    if (d % 64 || qd % 64 || H % 64) return 0;
    if (c->n_expert) {      /* MoE: atencao no mm.comp, especialistas pelas vagas (formato conferido por camada) */
        if (c->n_used > 16 || vkg.nl != c->n_layers) return 0;
        for (int l = 0; l < vkg.nl; l++) {
            const Layer *L = &m->layers[l];
            if (L->bo || !L->eg) return 0;
            const Mat *ms[4] = { &L->wq, &L->wk, &L->wv, &L->wo };
            for (int i = 0; i < 4; i++) if (ms[i]->gs != 1 || !ms[i]->gbuf || gpu_kind(ms[i]) == T_Q2_K) return 0;
            if (gpu_kind(&L->eg[0]) == T_Q2_K || gpu_kind(&L->ed[0]) == T_Q2_K) return 0;
            if (c->n_shared && (!L->sg.gbuf || !L->sg.pair || !L->sd.gbuf || gpu_kind(&L->sg) == T_Q2_K || gpu_kind(&L->sd) == T_Q2_K
                                || L->sd.gs != 1)) return 0;
        }
        return 1;
    }
    for (int l = 0; l < vkg.nl; l++) {
        const Layer *L = &m->layers[l];
        if (L->bo || !L->w_gate.pair) return 0;
        const Mat *ms[5] = { &L->wq, &L->wk, &L->wv, &L->wo, &L->w_down };
        for (int i = 0; i < 5; i++) if (ms[i]->gs != 1 || !ms[i]->gbuf) return 0;
        const Mat *all[6] = { &L->wq, &L->wk, &L->wv, &L->wo, &L->w_gate, &L->w_down };    /* o mm.comp nao tem Q2_K */
        for (int i = 0; i < 6; i++) if (gpu_kind(all[i]) == T_Q2_K) return 0;
    }
    return 1;
}

static int vk_batch_build(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, qd = (size_t)c->n_heads * c->hd;
    const size_t mx = d > qd ? (d > H ? d : H) : (qd > H ? qd : H);
    int fail = 0;
    memset(&vkb, 0, sizeof vkb);
    if (!vkg.P || !vkg.coop || !vk_batch_supported(m)) { vkg.P = 0; return 0; }
    const char *av = getenv("KATANA_VK_ATT");           /* 1 = um lancamento (padrao), 3 = tres kernels */
    vkb.flash = !(av && atoi(av) == 3) && vkg.sg32 && c->hd % 32 == 0 && c->hd <= 256 && c->n_heads % c->n_kv == 0 && c->n_heads / c->n_kv <= 8;
    int P = vkg.P;
    if (!vkb.flash && P > 64) P = 64;                   /* o buffer de notas cresce com o lote */
    vkb.P = P;
    vkb.x = vk_wk(d * P * 4, &fail); vkb.xh = vk_wk(mx * P * 2, &fail);
    vkb.q = vk_wk(qd * P * 4, &fail); vkb.q2 = vk_wk(qd * P * 4, &fail);
    vkb.k = vk_wk(kvd * P * 4, &fail); vkb.v = vk_wk(kvd * P * 4, &fail); vkb.hb = vk_wk(H * P * 2, &fail);
    if (!vkb.flash) { vkb.o3 = vk_wk(qd * P * 4, &fail); vkb.att3 = vk_wk((size_t)P * c->n_heads * c->ctx * 4, &fail); }
    vkb.xT = xalloc(d * P, sizeof(float));
    memset(&vkbm, 0, sizeof vkbm);
    if (c->n_expert && !fail) {
        const size_t Pp = (size_t)P, E = c->n_expert, K = c->n_used;
        vkbm.xf = vk_wk(d * Pp * 4, &fail); vkbm.xg = vk_wk(d * Pp * 4, &fail);     /* xg em fp32 para o modo preciso */
        vkbm.ys = vk_wk(d * Pp * 4, &fail); vkbm.yc = vk_wk(d * Pp * 4, &fail);
        vkbm.lg = vk_wk((E * Pp + 128) * 4, &fail); vkbm.meta = vk_wk(2 * Pp * (K + 2) * 4, &fail);
        const size_t Hs = (size_t)(c->n_shared ? c->n_shared : 1) * H;
        kvk_buf_destroy(&vkb.hb); vkb.hb = vk_wk((Hs > H ? Hs : H) * Pp * 4, &fail);    /* fp32 no modo preciso */
        vkbm.rms32 = kvk_pipe_create(rms_wg32_spv, sizeof rms_wg32_spv, 3, 12, NULL, NULL, 0);
        vkbm.gath = kvk_pipe_create(moe_gath_spv, sizeof moe_gath_spv, 3, 16, NULL, NULL, 0);
        vkbm.gath32 = kvk_pipe_create(moe_gath32_spv, sizeof moe_gath32_spv, 3, 16, NULL, NULL, 0);
        vkbm.scat = kvk_pipe_create(moe_scat_spv, sizeof moe_scat_spv, 3, 16, NULL, NULL, 0);
        {   const uint32_t fi[2] = { 0, 1 }, fw[2] = { (uint32_t)c->hd, (uint32_t)(c->n_heads / c->n_kv) };
            if (vkb.flash) vkbm.flash32 = vkg.kv16 ? kvk_pipe_create(att_flash32_kv16_spv, sizeof att_flash32_kv16_spv, 4, 24, fi, fw, 2)
                                                   : kvk_pipe_create(att_flash32_kv32_spv, sizeof att_flash32_kv32_spv, 4, 24, fi, fw, 2); }
        if (!vkbm.rms32.p || !vkbm.gath.p || !vkbm.gath32.p || !vkbm.scat.p || (vkb.flash && !vkbm.flash32.p)) fail = 1;
        vkbm.xn = xalloc(d * Pp, sizeof(float)); vkbm.lgT = xalloc(E * Pp, sizeof(float)); vkbm.xc = xalloc(d * Pp, sizeof(float));
        vkbm.outs = xalloc((size_t)GPU_BMAX * K * d, sizeof(float)); vkbm.sw = xalloc(Pp, sizeof(float));
        vkbm.mt = xalloc(2 * Pp * (K + 2), sizeof(uint32_t));
        vkbm.T = getenv("KATANA_VK_COLDT") ? atoi(getenv("KATANA_VK_COLDT")) : 16;
        vkbm.cmin = getenv("KATANA_VK_COLDMIN") ? atoi(getenv("KATANA_VK_COLDMIN")) : 2;
        if (vkbm.T < 0) vkbm.T = 0;
        if (vkbm.T > 64) vkbm.T = 64;
        if (vkbm.T) {
            for (int l = 0; l < c->n_layers; l++) {
                const Layer *L = &m->layers[l];
                const size_t bg = 2 * gpu_mat_bytes(&L->w_gate, H, d), bd = gpu_mat_bytes(&L->w_down, d, H);
                if (bg > vkbm.tgb) vkbm.tgb = bg;
                if (bd > vkbm.tdb) vkbm.tdb = bd;
            }
            vkbm.tg = vk_wk(vkbm.T * vkbm.tgb, &fail); vkbm.td = vk_wk(vkbm.T * vkbm.tdb, &fail);
            vkbm.stage = xalloc(vkbm.tgb + vkbm.tdb, 1);
            vkbm.pf = xalloc(c->n_layers, sizeof(FILE *));
            if (!getenv("KATANA_VK_TMPTHR") || atoi(getenv("KATANA_VK_TMPTHR"))) vkbm.tstage = xalloc(vkbm.T * (vkbm.tgb + vkbm.tdb), 1);
        }
        vkbm.ccnt = xalloc(E, sizeof(int)); vkbm.tmpe = xalloc(64, sizeof(int));
        vkbm.cnt = xalloc(E + 66, sizeof(int)); vkbm.ofs = xalloc(E + 66, sizeof(int));
        vkbm.pv = xalloc(Pp * K, sizeof(int)); vkbm.pb = xalloc(Pp * K, sizeof(int)); vkbm.pw = xalloc(Pp * K, sizeof(float));
        vkbm.ce = xalloc(Pp * K, sizeof(int)); vkbm.cb = xalloc(Pp * K, sizeof(int)); vkbm.cw = xalloc(Pp * K, sizeof(float));
        vkbm.prec = getenv("KATANA_VK_MOEP") ? atoi(getenv("KATANA_VK_MOEP")) : 1;     /* fp16 troca roteamentos quase empatados (OLMoE no selftest) */
        vkbm.on = 1;
    }
    if (fail) { fprintf(stderr, "Vulkan: sem memoria para o lote (%s): prompt token a token\n", kvk_error()); vkg.P = 0; return 0; }
    const uint32_t fid[2] = { 0, 1 }, fv[2] = { (uint32_t)c->hd, (uint32_t)(c->n_heads / c->n_kv) };
    vkb.rms16 = kvk_pipe_create(rms_wg16_spv, sizeof rms_wg16_spv, 3, 12, NULL, NULL, 0);
    vkb.qkv = vkg.kv16 ? kvk_pipe_create(qkv_post_kv16_spv, sizeof qkv_post_kv16_spv, 12, 48, NULL, NULL, 0)
                       : kvk_pipe_create(qkv_post_kv32_spv, sizeof qkv_post_kv32_spv, 12, 48, NULL, NULL, 0);
    vkb.cvt = kvk_pipe_create(cvt_spv, sizeof cvt_spv, 3, 4, NULL, NULL, 0);
    vkb.gat = kvk_pipe_create(gather_spv, sizeof gather_spv, 2, 12, NULL, NULL, 0);
    if (vkb.flash)
        vkb.flash_p = vkg.kv16 ? kvk_pipe_create(att_flash_kv16_spv, sizeof att_flash_kv16_spv, 4, 24, fid, fv, 2)
                               : kvk_pipe_create(att_flash_kv32_spv, sizeof att_flash_kv32_spv, 4, 24, fid, fv, 2);
    else {
        vkb.sc = vkg.kv16 ? kvk_pipe_create(att_scores_kv16_spv, sizeof att_scores_kv16_spv, 3, 40, NULL, NULL, 0)
                          : kvk_pipe_create(att_scores_kv32_spv, sizeof att_scores_kv32_spv, 3, 40, NULL, NULL, 0);
        vkb.sm = vkg.kv16 ? kvk_pipe_create(att_softmax_kv16_spv, sizeof att_softmax_kv16_spv, 1, 16, NULL, NULL, 0)
                          : kvk_pipe_create(att_softmax_kv32_spv, sizeof att_softmax_kv32_spv, 1, 16, NULL, NULL, 0);
        vkb.mix = vkg.kv16 ? kvk_pipe_create(att_mix_kv16_spv, sizeof att_mix_kv16_spv, 3, 32, NULL, NULL, 0)
                           : kvk_pipe_create(att_mix_kv32_spv, sizeof att_mix_kv32_spv, 3, 32, NULL, NULL, 0);
    }
    if (!vkb.rms16.p || !vkb.qkv.p || !vkb.cvt.p || !vkb.gat.p || (vkb.flash ? !vkb.flash_p.p : (!vkb.sc.p || !vkb.sm.p || !vkb.mix.p))) {
        fprintf(stderr, "Vulkan: pipeline do lote falhou (%s): prompt token a token\n", kvk_error()); vkg.P = 0; return 0;
    }
    vkb.skip = getenv("KATANA_VK_SKIP") ? (unsigned)strtoul(getenv("KATANA_VK_SKIP"), NULL, 0) : 0;
    vkb.ok = 1;
    return 1;
}

static void vk_moe_n(Model *m, const Layer *L, int l, int B);
/* fases (bits de KATANA_VK_SKIP, so para medir): 1 norma, 2 q/k/v, 4 qkv_post, 8 atencao, 16 saida, 32 gate/up, 128 down, 256 cabeca (e poda da ultima camada) */
#define VKS(bit) (!(vkb.skip & (bit)))
static int vk_emit_n(const Model *m, int B, int pos0, int head) {
    const Config *c = &m->cfg;
    const int d = c->dim, hd = c->hd, H = c->hidden, kvd = c->n_kv * hd, qd = c->n_heads * hd, nh = c->n_heads;
    const struct { int n; float eps; int B; } rp = { d, c->eps, B };
    int ok = 1, pruned = 0;
    for (int l = 0; l < vkg.nl && ok; l++) {
        const Layer *L = &m->layers[l];
#define OR_DUMMY(v) ((v).b ? (v) : vkg.dummy)
        /* MoE no modo preciso: a atencao tambem em fp32 (norma fp32, produtos alta + resto, atencao com saida fp32).
           Com fp16 o residual muda o bastante para virar escolhas quase empatadas do roteador nas camadas seguintes */
        const int p32 = vkbm.on && vkbm.prec && vkb.flash;
        if (p32) { const KvkBuf rb[3] = { vkb.x, vkg.an[l], vkbm.xf }; ok &= kvk_dispatch(vkbm.rms32, rb, 3, &rp, sizeof rp, (uint32_t)((B + 7) / 8), 1, 1); }
        else if (VKS(1)) { const KvkBuf rb[3] = { vkb.x, vkg.an[l], vkb.xh }; ok &= kvk_dispatch(vkb.rms16, rb, 3, &rp, sizeof rp, (uint32_t)((B + 7) / 8), 1, 1); }
        if (p32) {
            ok &= vk_mm_p(&L->wq, vkbm.xf, vkb.q, qd, d, B, 0, 1);
            ok &= vk_mm_p(&L->wk, vkbm.xf, vkb.k, kvd, d, B, 0, 1);
            ok &= vk_mm_p(&L->wv, vkbm.xf, vkb.v, kvd, d, B, 0, 1);
        } else if (VKS(2)) {
            ok &= vk_mm(&L->wq, vkb.xh, vkb.q, qd, d, B, 0, 0);
            ok &= vk_mm(&L->wk, vkb.xh, vkb.k, kvd, d, B, 0, 0);
            ok &= vk_mm(&L->wv, vkb.xh, vkb.v, kvd, d, B, 0, 0);
        }
        if (VKS(4)) {
            const KvkBuf qb[12] = { vkb.q2, vkb.k, vkb.v, OR_DUMMY(vkg.bq[l]), OR_DUMMY(vkg.bk[l]), OR_DUMMY(vkg.bv[l]), vkg.kc[l], vkg.vc[l],
                                    vkg.freq, vkb.q, OR_DUMMY(vkg.qn[l]), OR_DUMMY(vkg.kn[l]) };
            const struct { int base, pos0, hd, neox, has_bias, d, kvd, norm, B; float eps; int nrot, qst; } qp =
                { 0, pos0, hd, c->rope_neox, (vkg.bq[l].b ? 1 : 0) | (vkg.bk[l].b ? 2 : 0) | (vkg.bv[l].b ? 4 : 0), qd, kvd, c->qk_norm, B, c->eps, hd, hd };
            ok &= kvk_dispatch(vkb.qkv, qb, 12, &qp, sizeof qp, (uint32_t)((B * (qd / 2 + kvd / 2 + kvd) + 127) / 128), 1, 1);
        }
        if (VKS(8)) {
            const float scale = 1.0f / sqrtf((float)hd);
            if (p32) {     /* saida fp32 em vkb.q (livre depois do qkv_post) */
                const KvkBuf ab[4] = { vkb.q2, vkg.kc[l], vkg.vc[l], vkb.q };
                const struct { int base, pos0, kvd, nkv, B; float scale; } ap = { 0, pos0, kvd, c->n_kv, B, scale };
                ok &= kvk_dispatch(vkbm.flash32, ab, 4, &ap, sizeof ap, (uint32_t)(B * c->n_kv), 1, 1);
            } else if (vkb.flash) {
                const KvkBuf ab[4] = { vkb.q2, vkg.kc[l], vkg.vc[l], vkb.xh };
                const struct { int base, pos0, kvd, nkv, B; float scale; } ap = { 0, pos0, kvd, c->n_kv, B, scale };
                ok &= kvk_dispatch(vkb.flash_p, ab, 4, &ap, sizeof ap, (uint32_t)(B * c->n_kv), 1, 1);
            } else {
                const int tmax = pos0 + B;
                const KvkBuf sb[3] = { vkb.q2, vkg.kc[l], vkb.att3 };
                const struct { int base, pos0, tmax, ctx, hd, kvd, group, nh, B; float scale; } sp = { 0, pos0, tmax, c->ctx, hd, kvd, nh / c->n_kv, nh, B, scale };
                ok &= kvk_dispatch(vkb.sc, sb, 3, &sp, sizeof sp, (uint32_t)(((size_t)B * nh * tmax + 127) / 128), 1, 1);
                const struct { int pos0, ctx, nh, B; } mp = { pos0, c->ctx, nh, B };
                ok &= kvk_dispatch(vkb.sm, &vkb.att3, 1, &mp, sizeof mp, (uint32_t)((B * nh + 127) / 128), 1, 1);
                const KvkBuf xb[3] = { vkb.att3, vkg.vc[l], vkb.o3 };
                const struct { int base, pos0, ctx, hd, kvd, group, nh, B; } xp = { 0, pos0, c->ctx, hd, kvd, nh / c->n_kv, nh, B };
                ok &= kvk_dispatch(vkb.mix, xb, 3, &xp, sizeof xp, (uint32_t)(((size_t)B * qd + 127) / 128), 1, 1);
                const uint32_t n4 = (uint32_t)((size_t)B * qd / 4);
                const KvkBuf cb[3] = { vkb.o3, vkb.o3, vkb.xh };
                ok &= kvk_dispatch(vkb.cvt, cb, 3, &n4, 4, (n4 + 255) / 256, 1, 1);
            }
        }
        if (p32) ok &= vk_mm_p(&L->wo, vkb.q, vkb.x, d, qd, B, 1, 1);
        else if (VKS(16)) ok &= vk_mm(&L->wo, vkb.xh, vkb.x, d, qd, B, 1, 0);
        if (c->n_expert) {      /* MoE: o roteamento depende dos tokens; a camada espera a GPU */
            if (!ok) return 0;
            vk_moe_n((Model *)m, L, l, B);
            continue;
        }
        /* ultima camada com logits: so o ultimo token precisa do feed-forward; a coluna B-1 vai para vkg.x (vetor de um token) */
        int Bf = B;
        KvkBuf xf = vkb.x;
        if (head && l == c->n_layers - 1 && VKS(256)) {
            const KvkBuf gb[2] = { vkb.x, vkg.x };
            const int gp[3] = { d, B, B - 1 };
            ok &= kvk_dispatch(vkb.gat, gb, 2, gp, sizeof gp, (uint32_t)((d + 127) / 128), 1, 1);
            Bf = 1; xf = vkg.x; pruned = 1;
        }
        const struct { int n; float eps; int B; } rpf = { d, c->eps, Bf };
        if (VKS(1)) { const KvkBuf rb2[3] = { xf, vkg.fn[l], vkb.xh }; ok &= kvk_dispatch(vkb.rms16, rb2, 3, &rpf, sizeof rpf, (uint32_t)((Bf + 7) / 8), 1, 1); }
        if (VKS(32)) ok &= vk_mm(&L->w_gate, vkb.xh, vkb.hb, H, d, Bf, 0, 1);     /* saida em fp16 (silu(gate)*up) */
        if (VKS(128)) ok &= vk_mm(&L->w_down, vkb.hb, xf, d, H, Bf, 1, 0);
#undef OR_DUMMY
    }
    if (ok && head && VKS(256)) {
        if (!pruned) {      /* so com a poda desligada (KATANA_VK_SKIP): copia a ultima coluna */
            const KvkBuf gb[2] = { vkb.x, vkg.x };
            const int gp[3] = { d, B, B - 1 };
            ok &= kvk_dispatch(vkb.gat, gb, 2, gp, sizeof gp, (uint32_t)((d + 127) / 128), 1, 1);
        }
        ok &= vk_emit_head(m);
    }
    return ok;
}

/* B tokens (B <= vkb.P) a partir de pos0 pelas camadas na GPU; logits do ultimo token se pedido */
static float *vk_forward_n(Model *m, const int *toks, int B, int pos0, int want_logits) {
    const Config *c = &m->cfg;
    const int d = c->dim;
    if (B < 1 || B > vkb.P || pos0 < 0 || pos0 + B > c->ctx) die("lote fora do limite (Vulkan)");
    double t0 = now_sec();
    for (int t = 0; t < B; t++) {
        embed_row(m, m->x, toks[t]);
        for (int j = 0; j < d; j++) vkb.xT[(size_t)j * B + t] = m->x[j];
    }
    if (!kvk_buf_write_q(vkb.x, 0, vkb.xT, (size_t)d * B * sizeof(float))) die("falha na execucao em Vulkan (escrita do lote)");
    if (c->ssm_inner) { vkb.t_emb += now_sec() - t0; vkb.passes++; vkb.toks += B; return vk_qn_forward_n(m, B, pos0, want_logits); }
    double t1 = now_sec();
    const int full = vkg.nl == c->n_layers;
    if (!vk_emit_n(m, B, pos0, want_logits && full) || !kvk_flush()) die("falha na execucao em Vulkan (lote)");
    double t2 = now_sec();
    if (!full) {                    /* grafo parcial: as B colunas voltam e a CPU faz o resto token a token */
        if (!kvk_buf_read(vkb.x, 0, vkb.xT, (size_t)d * B * sizeof(float))) die("falha na execucao em Vulkan (leitura do lote)");
        vkb.t_emb += t1 - t0; vkb.t_emit += t2 - t1; vkb.passes++; vkb.toks += B; vkb.t_wait += now_sec() - t2;
        static float *xrow; static size_t xrow_cap;
        if ((size_t)B * d > xrow_cap) { free(xrow); xrow_cap = (size_t)B * d; xrow = xalloc(xrow_cap, sizeof(float)); }
        for (int t = 0; t < B; t++)
            for (int j = 0; j < d; j++) xrow[(size_t)t * d + j] = vkb.xT[(size_t)j * B + t];
        cpu_layers_n(m, vkg.nl, c->n_layers, pos0, B, xrow);        /* resto das camadas em lote na CPU */
        if (!want_logits) return NULL;
        memcpy(m->x, xrow + (size_t)(B - 1) * d, d * sizeof(float));
        return cpu_head(m);
    }
    vkb.t_emb += t1 - t0; vkb.t_emit += t2 - t1; vkb.passes++; vkb.toks += B;
    if (vkbm.on && getenv("KATANA_VK_MOEPROF")) {
        fprintf(stderr, "[lote MoE] %d tokens em %.3f s: leitura/espera %.3f, roteamento %.3f, frios na CPU %.3f, subida temporaria %.3f "
                "(pares GPU %ld, CPU %ld, temporarios %ld em %ld subidas)\n", B, t2 - t1, vkbm.t_rd, vkbm.t_route, vkbm.t_cpu, vkbm.t_tmp,
                vkbm.n_hot, vkbm.n_cold, vkbm.n_tmp, vkbm.n_tmpe);
        vkbm.t_rd = vkbm.t_route = vkbm.t_cpu = vkbm.t_tmp = 0; vkbm.n_hot = vkbm.n_cold = vkbm.n_tmp = vkbm.n_tmpe = 0;
    }
    if (!want_logits) return NULL;
    if (!kvk_buf_read(vkg.lg, 0, m->logits, (size_t)c->vocab * sizeof(float))) die("falha na execucao em Vulkan (leitura)");
    vkb.t_wait += now_sec() - t2;
    return m->logits;
}

/* inicia o Vulkan e sobe os pesos (mesmo empacotador e mesmo orcamento do OpenCL); 0 = falhou, usar OpenCL */
static int vk_moe_init(Model *m);

/* GPU sem memoria (outro programa ocupando a VRAM): espera ate KATANA_VK_WAIT segundos (padrao 60)
 * tentando de novo, em vez de cair direto para a CPU */
static int kvk_init_wait(KvkInfo *I) {
    if (kvk_init(I)) return 1;
    const char *w = getenv("KATANA_VK_WAIT");
    const int lim = w ? atoi(w) : 60;
    for (int t = 0; t < lim; t += 3) {
        const char *e = kvk_error();
        if (!strstr(e, ": -3") && !strstr(e, ": -2")) return 0;     /* so espera por falta de memoria */
        if (t == 0) fprintf(stderr, "Vulkan: GPU ocupada (%s); esperando liberar ate %d s (KATANA_VK_WAIT)...\n", e, lim);
#ifdef _WIN32
        Sleep(3000);
#else
        sleep(3);
#endif
        if (kvk_init(I)) { fprintf(stderr, "Vulkan: GPU liberada\n"); return 1; }
    }
    return 0;
}

static int vk_setup(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab, qd = (size_t)c->n_heads * c->hd;
    KvkInfo I;
    /* MoE em Vulkan: padrao no qwen3moe (geracao igual ou melhor que no OpenCL). No olmoe ainda gera 2-4% mais
       devagar (medido alternado, bench 64+64): la so com KATANA_VK_MOE=1. KATANA_VK_MOE=0 manda tudo para o OpenCL */
    const char *vm = getenv("KATANA_VK_MOE");
    const int vk_moe = vm ? atoi(vm) != 0 : strcmp(c->arch, "olmoe") != 0;
    if (c->n_expert && !c->mla && !c->ssm_inner && !vk_moe) {
        fprintf(stderr, "Vulkan: MoE em Vulkan desligado para %s (KATANA_VK_MOE=1 liga), usando OpenCL/CPU\n", c->arch);
        return 0;
    }
    if (!kvk_init_wait(&I)) { fprintf(stderr, "Vulkan: indisponivel (%s)\n", kvk_error()); return 0; }
    gpu.vk = 1;
    vkg.sg32 = I.subgroup_size == 32;
    vkg.coop = 0;      /* produto 16x16x16 fp16 x fp16 -> fp32 no escopo de subgrupo (VK_COMPONENT_TYPE_FLOAT16 = 0, FLOAT32 = 1, SCOPE_SUBGROUP = 3) */
    if (I.coopmat && I.fp16 && I.storage16 && I.max_shared >= 32768)
        for (int i = 0; i < I.n_coop; i++)
            if (I.coop[i].m == 16 && I.coop[i].n == 16 && I.coop[i].k == 16 && I.coop[i].a_type == 0 && I.coop[i].b_type == 0
                && I.coop[i].c_type == 1 && I.coop[i].r_type == 1 && I.coop[i].scope == 3) vkg.coop = 1;
    if (!vkg.coop) fprintf(stderr, "Vulkan: sem matriz cooperativa fp16 16x16x16: prompt token a token\n");
    snprintf(gpu.name, sizeof gpu.name, "%s", I.name);
    gpu.vram = I.vram_bytes;
    gpu.avail0 = kvk_vram_budget();
    if (getenv("KATANA_VRAM_AVAIL_MB")) gpu.avail0 = (uint64_t)atoll(getenv("KATANA_VRAM_AVAIL_MB")) << 20;    /* so para teste */
    if (gpu.avail0 && gpu.avail0 < gpu.vram / 100 * 80)
        fprintf(stderr, "Vulkan: outro programa ocupa %.0f MB da placa; usando ate %.0f MB (o agente se reajusta quando ela liberar)\n",
                (gpu.vram - gpu.avail0) / 1048576.0, gpu_budget() / 1048576.0);
    gpu.seg_target = getenv("KATANA_SEG") ? (size_t)atoll(getenv("KATANA_SEG")) : 16384;
    size_t maxc = d > H ? d : H;
    if (qd > maxc) maxc = qd;
    if ((size_t)c->hidden_dense > maxc) maxc = c->hidden_dense;
    if ((size_t)c->n_shared * H > maxc) maxc = (size_t)c->n_shared * H;
    const size_t maxr = V > maxc ? V : maxc;
    gpu.vxb = kvk_buf_create(maxc * sizeof(float), KVK_USAGE_RW);
    gpu.vob = kvk_buf_create((maxr + 128) * sizeof(float), KVK_USAGE_RW);
    if (!gpu.vxb.b || !gpu.vob.b) { fprintf(stderr, "Vulkan: sem memoria para os buffers (%s)\n", kvk_error()); kvk_shutdown(); gpu.vk = 0; return 0; }
    gpu.used = (maxc + maxr + 128) * sizeof(float);
    if (c->mla || c->ssm_inner || c->g4) {
        mla_gpu_upload(m); kvk_wait();
        if (c->ssm_inner) vkq_setup(m);      /* grafo completo do qwen35moe; se nao der, fica o modo hibrido */
        return 1;
    }
    const uint64_t budget = gpu_budget();
    const int graph = vk_plan(m, budget);       /* 1 = o grafo completo cabe */
    int nl = 0;
    for (int l = 0; l < c->n_layers; l++) {
        Layer *L = &m->layers[l];
        const size_t ffn = gpu_mat_bytes(&L->w_gate, H, d) + gpu_mat_bytes(&L->w_up, H, d) + gpu_mat_bytes(&L->w_down, d, H);
        const size_t need = gpu_mat_bytes(&L->wq, qd, d) + gpu_mat_bytes(&L->wk, kvd, d) + gpu_mat_bytes(&L->wv, kvd, d)
                          + gpu_mat_bytes(&L->wo, d, qd) + (c->n_expert ? 0 : ffn);
        if (gpu.used + need > budget || (graph && l >= graph)) break;
        char nm[32];
        snprintf(nm, sizeof nm, "L%03d.wq", l);   gpu_upload(&L->wq, qd, d, nm);
        snprintf(nm, sizeof nm, "L%03d.wk", l);   gpu_upload(&L->wk, kvd, d, nm);
        snprintf(nm, sizeof nm, "L%03d.wv", l);   gpu_upload(&L->wv, kvd, d, nm);
        snprintf(nm, sizeof nm, "L%03d.wo", l);   gpu_upload(&L->wo, d, qd, nm);
        if (!c->n_expert) {
            gpu_upload_glu(&L->w_gate, &L->w_up, H, d, l);
            snprintf(nm, sizeof nm, "L%03d.down", l); gpu_upload(&L->w_down, d, H, nm);
            if (!L->w_gate.gbuf || !L->w_up.gbuf || !L->w_down.gbuf) break;
        }
        if (!L->wq.gbuf || !L->wk.gbuf || !L->wv.gbuf || !L->wo.gbuf) break;     /* tipo sem kernel de GPU */
        nl++;
    }
    if (!m->lm_head.gbuf && nl == c->n_layers && gpu.used + gpu_mat_bytes(&m->lm_head, V, d) <= budget) gpu_upload(&m->lm_head, V, d, "head");
    kvk_wait();
    gpu.n_gl = 0; gpu.on = 0; gpu.full = 0;      /* o grafo OpenCL nao e usado; vkg.on liga o grafo Vulkan */
    if (graph) {
        gpu.kv16 = vkg.kv16;
        if (nl == graph && vk_graph_build(m)) {
            const char *r = getenv("KATANA_VK_REC");
            vkg.rec = r ? atoi(r) != 0 : 1;
            if (c->n_expert) {      /* o roteamento depende do token: grafo ao vivo, uma camada por vez */
                vkg.rec = 0;
                if (!vk_moe_init(m)) { vkg.on = 0; fprintf(stderr, "Vulkan: cache de especialistas indisponivel: grafo na CPU\n"); }
            }
        } else fprintf(stderr, "Vulkan: nao cabe (pesos ou buffers do grafo falharam): grafo na CPU\n");
    }
    fprintf(stderr, "GPU: Vulkan %s (%.1f GB) - %d de %d camadas na VRAM, %.0f MB usados em %d buffers%s - %s\n",
            gpu.name, gpu.vram / 1073741824.0, nl, c->n_layers, gpu.used / 1048576.0, gpu.vk_nbuf,
            vkg.on && vkg.kv16 ? ", cache KV em 16 bits" : "",
            !vkg.on ? "Vulkan: grafo na CPU" : vkg.nl < c->n_layers ? (vkg.rec ? "grafo parcial em Vulkan (gravado), resto na CPU" : "grafo parcial em Vulkan (ao vivo), resto na CPU")
            : vkg.rec ? "grafo completo em Vulkan (gravado)" : "grafo completo em Vulkan (ao vivo)");
    if (gpu.n_shard) fprintf(stderr, "GPU: %d buffers lidos dos shards em %s\n", gpu.n_shard, shards.dir);
    return 1;
}

/* cria os buffers de trabalho do grafo (sem o cache KV); devolve 0 se algo nao couber */
static int gpu_setup_bufs(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab;
    const size_t ctx = c->ctx, L = c->n_layers, B = GPU_BMAX;
    int fail = 0;
    const size_t qd = (size_t)c->n_heads * c->hd, dq = qd > d ? qd : d;
    gpu.bx = gbuf_rw(d * B * 4);    gpu.bxb = gbuf_rw(dq * B * 4);  gpu.bq = gbuf_rw(dq * B * 4);
    size_t Hm = (size_t)c->hidden_dense > H ? (size_t)c->hidden_dense : H;
    if ((size_t)c->n_shared * H > Hm) Hm = (size_t)c->n_shared * H;
    gpu.bk = gbuf_rw(kvd * B * 4);  gpu.bv = gbuf_rw(kvd * B * 4);  gpu.bhb = gbuf_rw(Hm * B * 4);
    gpu.blog = gbuf_rw(V * 4);      gpu.batt = gbuf_rw(B * c->n_heads * ctx * 4);
    if (!gpu.bx || !gpu.bxb || !gpu.bq || !gpu.bk || !gpu.bv || !gpu.bhb || !gpu.blog || !gpu.batt) return 0;
    gpu.bfreq = gbuf_f(m->rope_freq, c->hd / 2, &fail);
    gpu.bon = gbuf_f(m->out_norm, d, &fail);
    gpu.an = xalloc(L, sizeof(clh));  gpu.fn = xalloc(L, sizeof(clh));
    gpu.lbq = xalloc(L, sizeof(clh)); gpu.lbk = xalloc(L, sizeof(clh));
    gpu.lbv = xalloc(L, sizeof(clh)); gpu.lbo = xalloc(L, sizeof(clh));
    gpu.xT = xalloc(d * B, sizeof(float));
    gpu.blog4 = gbuf_rw(V * 4 * 4);     /* opcional: so a decodificacao especulativa usa */
    gpu.logits_all = xalloc(V * 4, sizeof(float));
    return !fail && gpu.bfreq && gpu.bon;
}

/* bytes que uma matriz ocupa na VRAM no nosso layout */
static size_t gpu_mat_bytes(const Mat *W, size_t rows, size_t cols) {
    return ((rows + 127) & ~(size_t)127) * gpu_wpr(W, cols) * 4;
}

/* katana.cfg fica ao lado do executavel e guarda o ajuste medido por `katana tune` para a
   placa desta maquina; so vale se o nome da GPU for o mesmo. Variaveis de ambiente ganham. */
static void cfg_path(char *out, size_t n) {
#ifdef _WIN32
    char exe[1024];
    DWORD l = GetModuleFileNameA(NULL, exe, sizeof exe);
    while (l > 0 && exe[l - 1] != '\\' && exe[l - 1] != '/') l--;
    exe[l] = 0;
    snprintf(out, n, "%skatana.cfg", exe);
#else
    /* na pasta atual se ja existir la (uso no repositorio); senao em ~/.katana (aplicativo instalado: a pasta do
       executavel pode ser so de leitura) */
    struct stat st;
    const char *home = getenv("HOME");
    {   /* ao lado do executavel (o mesmo arquivo de qualquer pasta de trabalho) */
        char ed[1024], f[1100];
        if (exe_dir(ed, sizeof ed)) { snprintf(f, sizeof f, "%s/katana.cfg", ed); if (stat(f, &st) == 0) { snprintf(out, n, "%s", f); return; } }
    }
    if (stat("katana.cfg", &st) == 0 || !home) { snprintf(out, n, "katana.cfg"); return; }
    snprintf(out, n, "%s/.katana", home);
    mkdir(out, 0755);
    snprintf(out, n, "%s/.katana/katana.cfg", home);
#endif
}

static void gpu_cfg_load(void) {
    char fn[1100], line[256], name[128] = {0};
    long long seg = -1; int rows = 0, rowsb = 0;
    cfg_path(fn, sizeof fn);
    FILE *f = fopen(fn, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "gpu=", 4) == 0) { strncpy(name, line + 4, sizeof name - 1); name[strcspn(name, "\r\n")] = 0; }
        else if (sscanf(line, "seg=%lld", &seg) == 1) continue;
        else if (sscanf(line, "rows=%d", &rows) == 1) continue;
        else sscanf(line, "rowsb=%d", &rowsb);
    }
    fclose(f);
    if (strcmp(name, gpu.name) != 0) return;
    if (seg >= 0) gpu.cfg_seg = (size_t)seg;
    if (rows == 1 || rows == 2) gpu.cfg_rows = rows;
    if (rowsb == 1 || rowsb == 2) gpu.cfg_rowsb = rowsb;
}

/* Decide o que vai para a VRAM. As camadas sobem em ordem, inteiras, enquanto couberem
   (pesos + cache KV da camada). Se todas couberem, a saida (lm_head) tambem sobe e o token
   inteiro roda na GPU; senao as primeiras n_gl camadas rodam na GPU e o resto na CPU, com
   uma unica transferencia do estado por token. KATANA_GPU_LAYERS limita n_gl. */
/* cache de especialistas na VRAM (OpenCL): o que sobrou do orcamento vira vagas por camada.
   Serve ao grafo completo (qwen3moe) e ao modo com as camadas na CPU (qwen35moe). 0 = nao coube. */
static int gx_setup(Model *m, uint64_t vram_budget, int all_layers) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden;
    {
        /* o que sobrou da VRAM vira o cache de especialistas, dividido em vagas iguais por camada
           (KATANA_EXPERT_MB limita o total, para testes) */
        const Layer *L0 = &m->layers[0];
        const int E = c->n_expert, NL = c->n_layers;
        size_t budget = vram_budget > gpu.used ? (size_t)(vram_budget - gpu.used) : 0;
        if (getenv("KATANA_EXPERT_MB")) { const size_t lim = (size_t)atoll(getenv("KATANA_EXPERT_MB")) << 20; if (lim < budget) budget = lim; }
        /* cada camada tem vagas do tamanho dos seus especialistas (a quantizacao varia por camada) */
        gx.bg = xalloc(NL, sizeof(size_t)); gx.bd = xalloc(NL, sizeof(size_t));
        int ok = all_layers && c->n_used <= MOE_KMAX;
        size_t per = 0;
        for (int l = 0; l < NL; l++) {
            const Layer *L = &m->layers[l];
            if (!L->eg) continue;       /* camada densa inicial: feed-forward normal, ja na VRAM */
            if (!glu_pair(&L->w_gate, &L->w_up) || !gpu_has(&L->w_gate) || !gpu_has(&L->w_down)) ok = 0;
            if (L->shexp_gate) { ok = 0; fprintf(stderr, "GPU: portao do compartilhado ainda nao suportado no grafo completo\n"); }
            gx.bg[l] = 2 * gpu_mat_bytes(&L->w_gate, H, d); gx.bd[l] = gpu_mat_bytes(&L->w_down, d, H);
            per += gx.bg[l] + gx.bd[l];
        }
        (void)L0;
        size_t S = budget / per;
        if (S > (size_t)E) S = E;
        if (!ok || S < (size_t)c->n_used) {
            fprintf(stderr, "GPU: MoE nao coube na VRAM (ou gate e up em formatos diferentes), usando CPU\n"); return 0;
        } else {
            int32_t e1 = 0, e2 = 0, e3 = 0, e4 = 0;
            gx.S = (int)S;
            gx.sg = xalloc(NL, sizeof(clh)); gx.sd = xalloc(NL, sizeof(clh));
            for (int l = 0; l < NL; l++) {
                if (!m->layers[l].eg) continue;
                gx.sg[l] = cl.CreateBuffer(gpu.ctx, 4, S * gx.bg[l], NULL, &e1);
                gx.sd[l] = cl.CreateBuffer(gpu.ctx, 4, S * gx.bd[l], NULL, &e2);
                if (e1 || e2) break;
            }
            if (!e1 && !e2 && !getenv("KATANA_NOTOUCH")) {
                /* a placa so reserva a memoria de um buffer no primeiro uso; preencher agora tira
                   esse custo do caminho do primeiro token */
                const double tt = now_sec();
                const uint32_t zero = 0;
                for (int l = 0; l < NL; l++) if (gx.sg[l]) {
                    cl.EnqueueFillBuffer(gpu.queue, gx.sg[l], &zero, 4, 0, S * gx.bg[l], 0, NULL, NULL);
                    cl.EnqueueFillBuffer(gpu.queue, gx.sd[l], &zero, 4, 0, S * gx.bd[l], 0, NULL, NULL);
                }
                cl.Finish(gpu.queue);
                if (gpu.debug) fprintf(stderr, "GPU: vagas dos especialistas reservadas em %.2f s\n", now_sec() - tt);
            }
            const size_t RM = (size_t)GPU_BMAX * c->n_used;     /* faixas (token, especialista) por passada */
            gx.bh = cl.CreateBuffer(gpu.ctx, 1, RM * (H + 128) * 4, NULL, &e3);
            gx.bt = cl.CreateBuffer(gpu.ctx, 1, RM * (d + 128) * 4, NULL, &e4);
            if (!e3 && !e4) gx.bmeta = cl.CreateBuffer(gpu.ctx, 1, RM * 8 * 4, NULL, &e3);
            if (!e3 && !e4) gx.bxn = cl.CreateBuffer(gpu.ctx, 1, (size_t)GPU_BMAX * d * 4, NULL, &e4);
            gx.meta = xalloc(RM * 8, sizeof(uint32_t)); gx.w = xalloc(RM, sizeof(float)); gx.rowi = xalloc(RM, sizeof(uint32_t));
            gx.xn = xalloc((size_t)GPU_BMAX * d, sizeof(float));
            if (e1 || e2 || e3 || e4) { fprintf(stderr, "GPU: sem memoria para o cache de especialistas, usando CPU\n"); return 0; }
            else {
                moe_tables(m, S, per);
            }
        }
    }
    return 1;
}

static void gpu_setup(Model *m) {
    const Config *c = &m->cfg;
    if (c->go) { fprintf(stderr, "GPU: gpt-oss ainda so na CPU (hibrido Vulkan pendente)\n"); return; }
    if ((c->glm || c->glmoe) && !getenv("KATANA_GLM_GPU")) { fprintf(stderr, "GPU: glm4 ainda so na CPU\n"); return; }
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab;
    const size_t ctx = c->ctx, qd = (size_t)c->n_heads * c->hd;
    size_t kv_layer = 2 * ctx * kvd * sizeof(float);
    gpu.cfg_rows = 1; gpu.cfg_rowsb = 2; gpu.cfg_seg = 16384;       /* padroes medidos numa RTX 3050 */
    {   /* Vulkan e o padrao (matrizes cooperativas, grafo gravado); o que ele ainda nao cobre cai para o
           OpenCL. KATANA_BACKEND=opencl forca o OpenCL. */
        const char *be = getenv("KATANA_BACKEND");
        if (be && strcmp(be, "opencl") != 0 && strcmp(be, "vulkan") != 0)
            fprintf(stderr, "KATANA_BACKEND=%s desconhecido (use vulkan ou opencl), usando o padrao\n", be);
        const int vk_ok = !be || strcmp(be, "opencl") != 0;
        if (vk_ok) {
            if (vk_setup(m)) return;
            if (be && strcmp(be, "vulkan") == 0) fprintf(stderr, "Vulkan: falhou, tentando OpenCL\n");
        }
    }
    if (c->g4) { fprintf(stderr, "GPU: gemma4 so tem caminho em Vulkan; usando CPU\n"); return; }
    if (!gpu_init()) { fprintf(stderr, "GPU: nenhuma disponivel via OpenCL, usando CPU\n"); return; }
    gpu_cfg_load();     /* ajuste desta placa, se `katana tune` ja rodou */
    const uint64_t vram_budget = gpu_budget();
    const size_t NX = c->n_expert ? (size_t)c->n_expert : 1;
    {   /* se o modelo nao cabe inteiro com o cache KV em float32, o cache vai em float16 */
        size_t total = gpu_mat_bytes(&m->lm_head, V, d) + ((size_t)256 << 20);
        for (int l = 0; l < c->n_layers; l++) {
            const Layer *L = &m->layers[l];
            total += gpu_mat_bytes(&L->wq, qd, d) + gpu_mat_bytes(&L->wk, kvd, d) + gpu_mat_bytes(&L->wv, kvd, d)
                   + gpu_mat_bytes(&L->wo, d, qd) + NX * (gpu_mat_bytes(&L->w_gate, H, d) + gpu_mat_bytes(&L->w_up, H, d)
                   + gpu_mat_bytes(&L->w_down, d, H)) + kv_layer;
        }
        gpu.kv16 = getenv("KATANA_KV16") ? atoi(getenv("KATANA_KV16")) != 0 : total > vram_budget;
        if (gpu.kv16) kv_layer /= 2;
    }
    if (!gpu_build_all()) { fprintf(stderr, "GPU: falha ao preparar os kernels, usando CPU\n"); return; }

    size_t maxc = d > H ? d : H;
    if (qd > maxc) maxc = qd;
    if ((size_t)c->hidden_dense > maxc) maxc = c->hidden_dense;
    if ((size_t)c->n_shared * H > maxc) maxc = (size_t)c->n_shared * H;
    size_t maxr = V > maxc ? V : maxc;
    int32_t e1 = 0, e2 = 0, e4 = 0;
    gpu.xbuf = cl.CreateBuffer(gpu.ctx, 1, maxc * sizeof(float), NULL, &e1);
    gpu.obuf = cl.CreateBuffer(gpu.ctx, 1, (maxr + 128) * sizeof(float), NULL, &e2);
    gpu.seg_target = getenv("KATANA_SEG") ? (size_t)atoll(getenv("KATANA_SEG")) : gpu.cfg_seg;
    gpu.bsx = cl.CreateBuffer(gpu.ctx, 1, (maxc / 32 + 1) * GPU_BMAX * sizeof(float), NULL, &e4);
    if (!e1 && !e2 && !e4 && (c->mla || c->ssm_inner)) {
        mla_gpu_upload(m);     /* inclui o cache de especialistas do modo hibrido (host_moe_setup) */
        cl.Finish(gpu.queue);
        return;
    }
    if (e1 || e2 || e4 || !gpu_setup_bufs(m)) { fprintf(stderr, "GPU: sem memoria para os buffers, usando CPU\n"); return; }

    if (c->qk_norm) {
        gx.bqn = gbuf_rw(qd * GPU_BMAX * 4); gx.bkn = gbuf_rw(kvd * GPU_BMAX * 4);
        gx.qn = xalloc(c->n_layers, sizeof(clh)); gx.kn = xalloc(c->n_layers, sizeof(clh));
        if (!gx.bqn || !gx.bkn) { fprintf(stderr, "GPU: sem memoria para os buffers, usando CPU\n"); return; }
    }
    int max_layers = getenv("KATANA_GPU_LAYERS") ? atoi(getenv("KATANA_GPU_LAYERS")) : c->n_layers;
    if (max_layers > c->n_layers) max_layers = c->n_layers;
    int n_gl = 0, fail = 0;
    /* KATANA_HEAD_GPU=1: a projecao final sobe antes das camadas. Medido no 7B em 4 GB: ela
       desloca 3 camadas e o conjunto fica mais lento, por isso nao e o padrao. */
    if (gpu.kv16 && getenv("KATANA_HEAD_GPU")) gpu_upload(&m->lm_head, V, d, "head");
    for (int l = 0; l < max_layers; l++) {
        Layer *L = &m->layers[l];
        const size_t Hl = c->n_expert && !L->eg ? (size_t)c->hidden_dense : H;
        const size_t ffn = gpu_mat_bytes(&L->w_gate, Hl, d) + gpu_mat_bytes(&L->w_up, Hl, d) + gpu_mat_bytes(&L->w_down, d, Hl);
        /* MoE: a camada so reserva os especialistas de um token; o resto e cache */
        const size_t need = gpu_mat_bytes(&L->wq, qd, d) + gpu_mat_bytes(&L->wk, kvd, d) + gpu_mat_bytes(&L->wv, kvd, d)
                          + gpu_mat_bytes(&L->wo, d, qd) + (L->eg ? 0 : ffn) + kv_layer;
        if (gpu.used + need > vram_budget || (size_t)(l + 1) * ctx * kvd > 0x7fffffff) break;
        char nm[32];
        snprintf(nm, sizeof nm, "L%03d.wq", l);   gpu_upload(&L->wq, qd, d, nm);
        snprintf(nm, sizeof nm, "L%03d.wk", l);   gpu_upload(&L->wk, kvd, d, nm);
        snprintf(nm, sizeof nm, "L%03d.wv", l);   gpu_upload(&L->wv, kvd, d, nm);
        snprintf(nm, sizeof nm, "L%03d.wo", l);   gpu_upload(&L->wo, d, qd, nm);
        if (!L->eg) {
            gpu_upload_glu(&L->w_gate, &L->w_up, Hl, d, l);
            snprintf(nm, sizeof nm, "L%03d.down", l); gpu_upload(&L->w_down, d, Hl, nm);
            if (!L->w_gate.gbuf || !L->w_up.gbuf || !L->w_down.gbuf) break;
        } else if (L->sg.q || L->sg.f) {     /* especialista compartilhado: denso, inteiro na VRAM, fora das vagas */
            const size_t Hs = (size_t)c->n_shared * H;
            if (glu_pair(&L->sg, &L->su)) {
                Mat *ms[2] = { &L->sg, &L->su };
                snprintf(nm, sizeof nm, "L%03d.sglu", l); gpu_upload_n(ms, 2, Hs, d, nm);
                L->sg.pair = L->sg.gbuf != NULL;
            } else {
                snprintf(nm, sizeof nm, "L%03d.sgate", l); gpu_upload(&L->sg, Hs, d, nm);
                snprintf(nm, sizeof nm, "L%03d.sup", l);   gpu_upload(&L->su, Hs, d, nm);
            }
            snprintf(nm, sizeof nm, "L%03d.sdown", l); gpu_upload(&L->sd, d, Hs, nm);
            if (!L->sg.gbuf || !L->su.gbuf || !L->sd.gbuf) break;
        }
        if (!L->wq.gbuf || !L->wk.gbuf || !L->wv.gbuf || !L->wo.gbuf) break;
        if (c->qk_norm) { gx.qn[l] = gbuf_f(L->q_norm, c->qk_norm == 1 ? d : (size_t)c->hd, &fail); gx.kn[l] = gbuf_f(L->k_norm, c->qk_norm == 1 ? kvd : (size_t)c->hd, &fail); }
        gpu.an[l] = gbuf_f(L->attn_norm, d, &fail);  gpu.fn[l] = gbuf_f(L->ffn_norm, d, &fail);
        gpu.lbq[l] = gbuf_f(L->bq, qd, &fail);        gpu.lbk[l] = gbuf_f(L->bk, kvd, &fail);
        gpu.lbv[l] = gbuf_f(L->bv, kvd, &fail);      gpu.lbo[l] = gbuf_f(L->bo, d, &fail);
        if (fail) break;
        gpu.used += kv_layer;       /* reserva o cache KV desta camada */
        n_gl++;
    }
    if (!m->lm_head.gbuf && n_gl == c->n_layers && gpu.used + gpu_mat_bytes(&m->lm_head, V, d) <= vram_budget) gpu_upload(&m->lm_head, V, d, "head");
    if (n_gl > 0) {
        int32_t e5 = 0, e6 = 0;
        gpu.bkc = cl.CreateBuffer(gpu.ctx, 1, (size_t)n_gl * ctx * kvd * (gpu.kv16 ? 2 : 4), NULL, &e5);
        gpu.bvc = cl.CreateBuffer(gpu.ctx, 1, (size_t)n_gl * ctx * kvd * (gpu.kv16 ? 2 : 4), NULL, &e6);
        if (e5 || e6) n_gl = 0;
    }
    if (c->n_expert && n_gl > 0 && !gx_setup(m, vram_budget, n_gl == c->n_layers)) n_gl = 0;
    gpu.n_gl = n_gl;
    gpu.on = n_gl > 0;
    gpu.full = n_gl == c->n_layers && m->lm_head.gbuf != NULL;
    cl.Finish(gpu.queue);
    fprintf(stderr, "GPU: %s (%.1f GB) - %d de %d camadas na VRAM, %.0f MB usados%s - %s\n",
            gpu.name, gpu.vram / 1073741824.0, n_gl, c->n_layers, gpu.used / 1048576.0, gpu.kv16 ? ", cache KV em 16 bits" : "",
            gpu.full ? "grafo completo na GPU" : n_gl ? "o resto roda na CPU" : "usando CPU");
    if (gpu.n_shard) fprintf(stderr, "GPU: %d buffers lidos dos shards em %s\n", gpu.n_shard, shards.dir);
}

/* Leitura que espera a fila terminar. O padrao e a leitura bloqueante do driver; com
   KATANA_SPIN=1 a leitura entra na fila, a fila e despachada e a thread fica sondando o evento
   (sem dormir), para medir quanto do tempo por token e o despertar da thread. */
static double t_gread;      /* tempo esperando a GPU nas leituras bloqueantes */
static int32_t gread(clh buf, size_t bytes, void *dst) {
    static int spin = -1;
    if (spin < 0) spin = getenv("KATANA_SPIN") ? atoi(getenv("KATANA_SPIN")) : 0;
    if (!spin) {
        const double t0 = now_sec();
        const int32_t e0 = cl.EnqueueReadBuffer(gpu.queue, buf, 1, 0, bytes, dst, 0, NULL, NULL);
        t_gread += now_sec() - t0;
        return e0;
    }
    clh ev = NULL;
    int32_t e = cl.EnqueueReadBuffer(gpu.queue, buf, 0, 0, bytes, dst, 0, NULL, &ev);
    if (e || !ev) return e ? e : -1;
    cl.Flush(gpu.queue);
    int32_t st = 1;
    while (st > 0) {
        if (cl.GetEventInfo(ev, 0x11D3 /* COMMAND_EXECUTION_STATUS */, sizeof st, &st, NULL) != 0) { st = -1; break; }
        if (st > 0) _mm_pause();
    }
    cl.ReleaseEvent(ev);
    return st < 0 ? st : 0;
}

/* ---- pequenos atalhos para montar chamadas de kernel ---- */
static void ka(clh k, uint32_t i, size_t sz, const void *v) { int32_t e = cl.SetKernelArg(k, i, sz, v); if (e && !gerr) fprintf(stderr, "GPU: SetKernelArg(kernel=%p, arg=%u) erro %d\n", k, i, e); gerr |= e; }
static void ka_m(clh k, uint32_t i, clh b) { ka(k, i, sizeof(clh), &b); }
static void ka_i(clh k, uint32_t i, int32_t v) { ka(k, i, sizeof v, &v); }
static void ka_f(clh k, uint32_t i, float v) { ka(k, i, sizeof v, &v); }

static void krun(clh k, size_t n) {
    double t0 = gpu.debug ? now_sec() : 0.0;
    int32_t e = cl.EnqueueNDRangeKernel(gpu.queue, k, 1, NULL, &n, NULL, 0, NULL, NULL);
    if (e && !gerr) fprintf(stderr, "GPU: enqueue(kernel=%p, n=%zu) erro %d\n", k, n, e);
    gerr |= e;
    if (gpu.debug && !gerr) {
        e = cl.Finish(gpu.queue);
        if (e) fprintf(stderr, "GPU: kernel=%p n=%zu falhou na execucao, erro %d\n", k, n, e);
        gerr |= e;
        for (int i = 0; i < n_prof; i++) if (prof[i].k == k) {
            prof[i].t += now_sec() - t0; prof[i].calls++; prof[i].elems += prof_elems;
        }
    }
    prof_elems = 0.0;
}

/* NDRange 2D: n0 linhas (multiplo de 32) x n1 segmentos */
static void krun2(clh k, size_t n0, size_t n1) {
    double t0 = gpu.debug ? now_sec() : 0.0;
    size_t gs[2] = { n0, n1 }, ls[2] = { 32, n1 };     /* com segmentos: grupo = 32 linhas x nseg */
    int32_t e = cl.EnqueueNDRangeKernel(gpu.queue, k, 2, NULL, gs, n1 > 1 ? ls : NULL, 0, NULL, NULL);
    if (e && !gerr) fprintf(stderr, "GPU: enqueue2(kernel=%p, n=%zux%zu) erro %d\n", k, n0, n1, e);
    gerr |= e;
    if (gpu.debug && !gerr) {
        e = cl.Finish(gpu.queue);
        if (e) fprintf(stderr, "GPU: kernel=%p n=%zux%zu falhou na execucao, erro %d\n", k, n0, n1, e);
        gerr |= e;
        for (int i = 0; i < n_prof; i++) if (prof[i].k == k) {
            prof[i].t += now_sec() - t0; prof[i].calls++; prof[i].elems += prof_elems;
        }
    }
    prof_elems = 0.0;
}

/* NDRange 1D com tamanho de grupo explicito */
static void krun_local(clh k, size_t n, size_t local) {
    double t0 = gpu.debug ? now_sec() : 0.0;
    int32_t e = cl.EnqueueNDRangeKernel(gpu.queue, k, 1, NULL, &n, &local, 0, NULL, NULL);
    if (e && !gerr) fprintf(stderr, "GPU: enqueue(kernel=%p, n=%zu, grupo=%zu) erro %d\n", k, n, local, e);
    gerr |= e;
    if (gpu.debug && !gerr) {
        e = cl.Finish(gpu.queue);
        if (e) fprintf(stderr, "GPU: kernel=%p n=%zu falhou na execucao, erro %d\n", k, n, e);
        gerr |= e;
        for (int i = 0; i < n_prof; i++) if (prof[i].k == k) {
            prof[i].t += now_sec() - t0; prof[i].calls++; prof[i].elems += prof_elems;
        }
    }
    prof_elems = 0.0;
}

static void prof_report(void) {
    double tot = 0.0;
    if (!gpu.debug) return;
    for (int i = 0; i < n_prof; i++) tot += prof[i].t;
    fprintf(stderr, "perfil GPU (com sincronizacao por kernel; total %.1f ms):\n", tot * 1e3);
    for (int i = 0; i < n_prof; i++) if (prof[i].calls)
        fprintf(stderr, "  %-12s %7ld chamadas %9.1f ms %5.1f%%  %7.1f us/chamada  %6.2f G pesos/s\n",
                prof[i].name, prof[i].calls, prof[i].t * 1e3, 100.0 * prof[i].t / (tot + 1e-12),
                prof[i].t * 1e6 / prof[i].calls, prof[i].elems / (prof[i].t + 1e-12) / 1e9);
}

/* enfileira out = W * x na GPU (modo: 0 grava, 1 soma em out, 2 SwiGLU com o que ja esta em out).
   Matrizes com poucas linhas e muitas colunas sao divididas em segmentos de colunas para
   ocupar a GPU inteira. Em lote (ks->nb > 1), matrizes Q4_K exigem g_bsum(x) antes. */
static void gmv(const KSet *ks, const Mat *W, clh x, clh out, clh bias, int rows, int cols, int mode) {
    const uint32_t kind = gpu_kind(W);
    clh k = kind == T_F32 ? ks->k_f32 : kind == T_Q4_K ? ks->k_q4k : kind == T_Q5_K ? ks->k_q5k : kind == T_Q6_K ? ks->k_q6k : ks->k_q8;
    const size_t rowsP = ((size_t)rows + 127) & ~(size_t)127, threads = rowsP / ks->nr;
    const int nb = kind == T_F32 ? 1 : kind == T_Q8_0 ? cols / 32 : cols / 256;
    const int nseg = gpu_nseg(threads, nb, ks->nb * ks->nr);
    if (bias) mode |= 4;
    ka_m(k, 0, W->gbuf); ka_m(k, 1, x); ka_m(k, 2, out);
    ka_m(k, 3, bias ? bias : x); ka_m(k, 4, gpu.bsx ? gpu.bsx : x);
    ka_i(k, 5, cols); ka_i(k, 6, mode); ka_i(k, 7, rows); ka_i(k, 8, nseg);
    ka_i(k, 9, W->gs); ka_i(k, 10, W->go);
    prof_elems = (double)rows * cols * ks->nb;
    krun2(k, threads, (size_t)nseg);
}

/* out = silu(gate * x) * (up * x) num kernel so, sobre o buffer com gate e up intercalados */
static void gmv_glu(const KSet *ks, const Mat *gate, clh x, clh out, int rows, int cols) {
    const uint32_t kind = gpu_kind(gate);
    clh k = kind == T_F32 ? ks->kp_f32 : kind == T_Q4_K ? ks->kp_q4k : kind == T_Q5_K ? ks->kp_q5k : kind == T_Q6_K ? ks->kp_q6k : ks->kp_q8;
    const size_t rowsP = ((size_t)rows + 127) & ~(size_t)127, threads = 2 * rowsP / (2 * ks->nr);
    const int nb = kind == T_F32 ? 1 : kind == T_Q8_0 ? cols / 32 : cols / 256;
    const int nseg = gpu_nseg(threads, nb, ks->nb * ks->nr * 2);
    ka_m(k, 0, gate->gbuf); ka_m(k, 1, x); ka_m(k, 2, out);
    ka_m(k, 3, x); ka_m(k, 4, gpu.bsx ? gpu.bsx : x);
    ka_i(k, 5, cols); ka_i(k, 6, 8); ka_i(k, 7, rows); ka_i(k, 8, nseg);
    ka_i(k, 9, 1); ka_i(k, 10, 0);
    prof_elems = 2.0 * rows * cols * ks->nb;
    krun2(k, threads, (size_t)nseg);
}

/* modo hibrido: um produto na GPU, com ida e volta */
static void gpu_matvec(float *out, const Mat *W, const float *x, int rows, int cols) {
    if (gpu.vk) { vk_matvec(out, W, x, rows, cols); return; }
    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gpu.xbuf, 0, 0, cols * sizeof(float), x, 0, NULL, NULL);
    gmv(&gpu.set[gpu.n_sets - 1], W, gpu.xbuf, gpu.obuf, NULL, rows, cols, 0);
    gerr |= cl.EnqueueReadBuffer(gpu.queue, gpu.obuf, 1 /* bloqueante */, 0, (size_t)rows * sizeof(float), out, 0, NULL, NULL);
    if (gerr) die("falha na execucao em GPU");
}

/* perfil da CPU (KATANA_DEBUG=1): tempo e pesos por tipo de produto, e preparo das ativacoes */
static struct { double t, w; long n; } cprof[5];
static const char *cprof_name[5] = { "float32", "outros", "Q4_K", "Q6_K", "quantizar x" };

static void cprof_report(void) {
    double tot = 0.0;
    if (!gpu.debug) return;
    for (int i = 0; i < 5; i++) tot += cprof[i].t;
    if (tot == 0.0) return;
    fprintf(stderr, "perfil CPU (produtos; total %.1f ms):\n", tot * 1e3);
    for (int i = 0; i < 5; i++) if (cprof[i].n)
        fprintf(stderr, "  %-12s %7ld chamadas %9.1f ms %5.1f%%  %6.2f G pesos/s\n", cprof_name[i], cprof[i].n,
                cprof[i].t * 1e3, 100.0 * cprof[i].t / tot, cprof[i].w / (cprof[i].t + 1e-12) / 1e9);
}

typedef struct { float *out; const Mat *W; const float *x, *sx; int cols; } MvJob;
static void mv_job(void *c, int i0, int i1) {
    const MvJob *j = c;
    for (int i = i0; i < i1; i++) j->out[i] = dot_row(j->W, (size_t)i, j->x, j->sx, j->cols);
}

/* out[rows] = W[rows][cols] * x[cols] */
static void matvec(float *out, const Mat *W, const float *x, int rows, int cols) {
    float sx[MAX_SX];
    int i;
    if (W->gbuf && (gpu.test_mv || (gpu.mv_host && !gpu.force_cpu))) { gpu_matvec(out, W, x, rows, cols); return; }
    const double t0 = gpu.debug ? now_sec() : 0.0;
    if (mat_int(W)) {
        xq_prepare(x, cols);
        if (gpu.debug) { cprof[4].t += now_sec() - t0; cprof[4].n++; }
    }
    const double t1 = gpu.debug ? now_sec() : 0.0;
    if (mat_int(W)) ;
    else if (W->q && W->type == T_Q4_K) {
        if (cols / 32 > MAX_SX) die("matriz larga demais");
        block_sums(sx, x, cols);
    }
    if ((long long)rows * cols >= 200000 && pool.n > 1) {
        /* blocos de ~32 mil pesos, e pelo menos 8 por thread para absorver threads atrasadas */
        MvJob job = { out, W, x, sx, cols };
        int chunk = rows / (pool.n * 8), minc = 32768 / cols + 1;
        if (chunk < minc) chunk = minc;
        pool_for(mv_job, &job, rows, chunk);
    } else for (i = 0; i < rows; i++) out[i] = dot_row(W, (size_t)i, x, sx, cols);
    if (gpu.debug) {
        const int k = !W->q ? 0 : W->type == T_Q4_K ? 2 : W->type == T_Q6_K ? 3 : 1;
        cprof[k].t += now_sec() - t1; cprof[k].w += (double)rows * cols; cprof[k].n++;
    }
}

/* embedding de um token (com a escala do modelo, quando tem) */
static void embed_row(const Model *m, float *out, int tok) {
    const int d = m->cfg.dim;
    if (tok >= VIS_BASE) { vis_row(m, out, tok); return; }      /* vetor de imagem */
    mat_row(out, &m->tok_emb, tok, d);
    const float s = m->cfg.emb_scale;
    if (s != 0.0f && s != 1.0f) for (int i = 0; i < d; i++) out[i] *= s;
}

/* copia a linha `row` de uma matriz para out (usado no embedding) */
static void mat_row(float *out, const Mat *W, int row, int cols) {
    if (W->f) memcpy(out, W->f + (size_t)row * cols, cols * sizeof(float));
    else dequant(W->type, W->q + (size_t)row * W->stride, out, cols);
}

static void rmsnorm(float *out, const float *x, const float *g, int n, float eps) {
    float inv = 1.0f / sqrtf(dot(x, x, n) / n + eps);
    for (int i = 0; i < n; i++) out[i] = g[i] * (x[i] * inv);
}

static void softmax(float *x, int n) {
    float mx = x[0], sum = 0.0f;
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    for (int i = 0; i < n; i++) { x[i] = expf(x[i] - mx); sum += x[i]; }
    for (int i = 0; i < n; i++) x[i] /= sum;
}

/* ================= forward com o grafo inteiro na GPU ================= */

static void g_rms(const KSet *ks, clh x, clh g, clh out, int n, float eps) {
    clh k = ks->k_rms;
    ka_m(k, 0, x); ka_m(k, 1, g); ka_m(k, 2, out); ka_i(k, 3, n); ka_f(k, 4, eps);
    krun(k, (size_t)n);
}

/* somas de x por sub-bloco, so quando ha lote e alguma das matrizes consumidoras e Q4_K */
static void g_bsum(const KSet *ks, clh x, int cols, const Mat *a, const Mat *b, const Mat *c3) {
    const Mat *ms[3] = { a, b, c3 };
    int need = 0;
    if (ks->nb == 1) return;
    for (int i = 0; i < 3; i++) if (ms[i] && ms[i]->q && (ms[i]->type == T_Q4_K || ms[i]->type == T_Q5_K)) need = 1;
    if (!need) return;
    ka_m(ks->k_bsum, 0, x); ka_m(ks->k_bsum, 1, gpu.bsx);
    krun(ks->k_bsum, (size_t)cols / 32);
}

static void cpu_layers(Model *m, int l0, int l1, int pos);
static float *cpu_head(Model *m);

/* MoE: contagem de uso de cada especialista [camada][especialista] e os escolhidos do ultimo token */
static struct { uint32_t *cnt; uint64_t tokens; float *lg, *ch; int sel[MOE_KMAX]; float w[MOE_KMAX]; uint16_t *trace; int ti; } moe;

/* MoE: o roteador pontua todos os especialistas sobre xb (entrada normalizada do feed-forward);
   so os n_used melhores rodam. Softmax sobre todos, ou sigmoide com vies so na escolha
   (exp_probs_b); pesos opcionalmente normalizados e escalados por exp_scale.
   Preenche moe.sel e moe.w. */
static const float *moe_lg_ext;     /* logits do roteador ja calculados (na GPU); NULL = calcula aqui */
static void moe_route(Model *m, const Layer *L, int l) {
    const Config *c = &m->cfg;
    const int E = c->n_expert, K = c->n_used;
    if (!moe.lg) {
        if (K > MOE_KMAX) die("MoE: especialistas ativos demais por token");
        moe.lg = xalloc(E, sizeof(float)); moe.ch = xalloc(E, sizeof(float));
    }
    float *pr = moe.lg, *ch = moe.ch;
    if (moe_lg_ext) memcpy(pr, moe_lg_ext, (size_t)E * sizeof(float));
    else matvec(pr, &L->router, m->xb, E, c->dim);
    if (c->gate_sig) for (int e = 0; e < E; e++) { pr[e] = 1.0f / (1.0f + expf(-pr[e])); ch[e] = pr[e] + (L->exp_bias ? L->exp_bias[e] : 0.0f); }
    else { if (L->router_b) for (int e = 0; e < E; e++) pr[e] += L->router_b[e]; softmax(pr, E); memcpy(ch, pr, (size_t)E * sizeof(float)); }
    float wsum = 0.0f;
    for (int k = 0; k < K; k++) {
        int best = 0;
        for (int e = 1; e < E; e++) if (ch[e] > ch[best]) best = e;
        moe.sel[k] = best; moe.w[k] = pr[best]; wsum += pr[best];
        ch[best] = -INFINITY;
    }
    const float eps = c->mla ? 1e-20f : 6.103515625e-5f;
    if (c->moe_norm) for (int k = 0; k < K; k++) moe.w[k] /= (wsum > eps ? wsum : eps);
    if (c->exp_scale != 1.0f) for (int k = 0; k < K; k++) moe.w[k] *= c->exp_scale;
    if (route_tap) route_tap(l, moe.sel, moe.w, K, c->moe_norm && c->exp_scale == 1.0f);
    if (L->exp_w) for (int k = 0; k < K; k++) moe.w[k] *= L->exp_w[moe.sel[k]];     /* gemma4: escala de cada especialista */
    if (dbg_on && cpu_exact) for (int k = 0; k < K; k++) dbg_sel[((size_t)dbg_pos * c->n_layers + l) * K + k] = moe.sel[k];
    if (moe.cnt) { for (int k = 0; k < K; k++) moe.cnt[(size_t)l * E + moe.sel[k]]++; if (l == c->n_dense) moe.tokens++; }
    if (moe.trace) for (int k = 0; k < K; k++) moe.trace[((size_t)moe.ti * c->n_layers + l) * K + k] = (uint16_t)moe.sel[k];
    if (et_hook_route) et_hook_route(m, L, l);
    /* KATANA_EXPLOG=<arquivo>: registra (camada, K escolhidos) em uint16 para simular politicas de vagas offline */
    static FILE *xlog; static int xlog_init;
    if (!xlog_init) {
        xlog_init = 1;
        const char *fn = getenv("KATANA_EXPLOG");
        if (fn && (xlog = fopen(fn, "ab")) != NULL) setvbuf(xlog, NULL, _IOFBF, 1 << 16);
    }
    if (xlog && !(dbg_on && !cpu_exact)) {
        uint16_t r[1 + MOE_KMAX];
        r[0] = (uint16_t)l;
        for (int k = 0; k < K; k++) r[1 + k] = (uint16_t)moe.sel[k];
        fwrite(r, sizeof(uint16_t), (size_t)K + 1, xlog);
    }
}

/* especialista compartilhado na CPU: m->xb2 = down(silu(gate xb) * up xb); devolve o peso com que entra
   (1, ou o portao sigmoide escalar quando o modelo tem ffn_gate_inp_shexp) */
static float shexp_gate_w(const Model *m, const Layer *L) {
    if (!L->shexp_gate) return 1.0f;
    return 1.0f / (1.0f + expf(-dot(L->shexp_gate, m->xb, m->cfg.dim)));
}
static void shexp_cpu(Model *m, const Layer *L) {
    const Config *c = &m->cfg;
    const int d = c->dim, Hs = c->n_shared * c->hidden;
    matvec(m->hb, &L->sg, m->xb, Hs, d);
    matvec(m->hb2, &L->su, m->xb, Hs, d);
    for (int i = 0; i < Hs; i++) { const float g = m->hb[i]; m->hb[i] = g / (1.0f + expf(-g)) * m->hb2[i]; }
    matvec(m->xb2, &L->sd, m->hb, d, Hs);
}

/* n pares (entrada, especialista) da camada na CPU: o j-esimo usa a entrada xs[xi[j]*d ..] e o
   especialista ex[j]; outs[j*d ..] recebe a saida. Cada entrada e quantizada uma vez e os produtos
   de todos os pares saem em dois despachos do pool (gate/up, depois down), cada thread apontando
   para a entrada quantizada do par que esta calculando. */
#define MOE_NMAX (GPU_BMAX * 64)
static inline float gelu_t(float x) { return 0.5f * x * (1.0f + tanhf(0.7978845608f * (x + 0.044715f * x * x * x))); }
static inline float act_glu(float g) { return g_gelu ? gelu_t(g) : g / (1.0f + expf(-g)); }
typedef struct { const Mat *eg, *eu, *ed; const int *ex, *xi; int H, d; float *hg, *hu, *outs; XQ *xin, *xh; } MoeJob;
/* produto de uma linha de especialista com a entrada quantizada em xq: tipos K pelo dot_row, e
   Q8_0/Q5_1 (down do gemma4) pelos produtos inteiros de blocos de 32 */
static int moe_int(const Mat *W) {
    return mat_int(W) || (HAVE_INT_DOT && !cpu_exact && !moe_q5k_old && W->q && (W->type == T_Q8_0 || W->type == T_Q5_1));
}
static inline float moe_dot(const Mat *W, size_t r, int cols) {
    if (W->type == T_Q8_0) return dot_q8_i(W->q + r * W->stride, cols);
    if (W->type == T_Q5_1) return dot_q51_i(W->q + r * W->stride, cols);
    return dot_row(W, r, NULL, NULL, cols);
}
static void moe_job(void *c, int i0, int i1) {
    const MoeJob *q = c;
    for (int i = i0; i < i1; i++) {
        const int j = i / (2 * q->H), r = i % (2 * q->H);
        xq_tls = &q->xin[q->xi[j]];
        if (r < q->H) q->hg[(size_t)j * q->H + r] = moe_dot(&q->eg[q->ex[j]], (size_t)r, q->d);
        else q->hu[(size_t)j * q->H + r - q->H] = moe_dot(&q->eu[q->ex[j]], (size_t)(r - q->H), q->d);
    }
    xq_tls = NULL;
}
/* ativacao e quantizacao da entrada do down, um especialista por item */
static void moe_act_job(void *c, int i0, int i1) {
    const MoeJob *q = c;
    for (int j = i0; j < i1; j++) {
        float *g = q->hg + (size_t)j * q->H; const float *u = q->hu + (size_t)j * q->H;
        for (int i = 0; i < q->H; i++) g[i] = act_glu(g[i]) * u[i];
        xq_tls = &q->xh[j]; xq_prepare(g, q->H);
    }
    xq_tls = NULL;
}
static void moe_down_job(void *c, int i0, int i1) {
    const MoeJob *q = c;
    for (int i = i0; i < i1; i++) {
        const int j = i / q->d, r = i % q->d;
        xq_tls = &q->xh[j];
        q->outs[i] = moe_dot(&q->ed[q->ex[j]], (size_t)r, q->H);
    }
    xq_tls = NULL;
}
/* medida das fases (microbancada): quantizar entrada, gate/up, ativacao+quantizar, down */
static int moe_prof_on; static double moe_prof[4];
static void moe_cpu_multi(Model *m, const Layer *L, const int *ex, const int *xi, int n, int n_x, const float *xs, float *outs) {
    static float *hg, *hu;
    static XQ xin[GPU_BMAX], *xh;
    const Config *c = &m->cfg;
    const int d = c->dim, H = c->hidden;
    if (!hg) {
        hg = xalloc((size_t)MOE_NMAX * H, sizeof(float)); hu = xalloc((size_t)MOE_NMAX * H, sizeof(float));
        xh = xalloc(MOE_NMAX, sizeof(XQ));
    }
    if (n <= 0) return;
    if (n > MOE_NMAX || n_x > GPU_BMAX) die("MoE: pares demais num lote");
    if (et_on && et_wrap(m, L, ex, xi, n, n_x, xs, outs)) return;     /* especialistas da arena de RAM (expert_tier.c) */
    if (!moe_int(&L->eg[ex[0]]) || !moe_int(&L->eu[ex[0]]) || !moe_int(&L->ed[ex[0]]) || pool.n <= 1) {
        for (int j = 0; j < n; j++) {       /* caminho exato/float: um a um */
            const float *x = xs + (size_t)xi[j] * d;
            matvec(hg, &L->eg[ex[j]], x, H, d);
            matvec(hu, &L->eu[ex[j]], x, H, d);
            for (int i = 0; i < H; i++) hg[i] = act_glu(hg[i]) * hu[i];
            matvec(outs + (size_t)j * d, &L->ed[ex[j]], hg, d, H);
        }
        return;
    }
    double t0 = moe_prof_on ? now_sec() : 0.0, t1 = 0.0;
    for (int b = 0; b < n_x; b++) { xq_tls = &xin[b]; xq_prepare(xs + (size_t)b * d, d); }
    xq_tls = NULL;
    MoeJob job = { L->eg, L->eu, L->ed, ex, xi, H, d, hg, hu, outs, xin, xh };
    int total = n * 2 * H, chunk = total / (pool.n * 8), minc = 32768 / d + 1;
    if (chunk < minc) chunk = minc;
    if (moe_prof_on) { t1 = now_sec(); moe_prof[0] += t1 - t0; t0 = t1; }
    pool_for(moe_job, &job, total, chunk);
    if (moe_prof_on) { t1 = now_sec(); moe_prof[1] += t1 - t0; t0 = t1; }
    if (moe_q5k_old) {
        for (int i = 0; i < n * H; i++) hg[i] = act_glu(hg[i]) * hu[i];
        for (int j = 0; j < n; j++) { xq_tls = &xh[j]; xq_prepare(hg + (size_t)j * H, H); }
        xq_tls = NULL;
    } else pool_for(moe_act_job, &job, n, 1);
    total = n * d; chunk = total / (pool.n * 8); minc = 32768 / H + 1;
    if (chunk < minc) chunk = minc;
    if (moe_prof_on) { t1 = now_sec(); moe_prof[2] += t1 - t0; t0 = t1; }
    pool_for(moe_down_job, &job, total, chunk);
    if (moe_prof_on) moe_prof[3] += now_sec() - t0;
}
/* n especialistas sobre a mesma entrada m->xb */
static void moe_cpu_experts(Model *m, const Layer *L, const int *ex, int n, float *outs) {
    static const int zero[64];
    moe_cpu_multi(m, L, ex, zero, n, 1, m->xb, outs);
}

/* ---- cache de especialistas na VRAM ----
 * Num modelo MoE a atencao de todas as camadas fica fixa na VRAM e o que sobra do orcamento
 * vira um cache de especialistas: cada camada tem S vagas; um especialista ocupa uma vaga na
 * primeira vez que o roteador o escolhe e, quando a camada enche, sai o usado ha mais tempo.
 *   quente = numa vaga da VRAM;  morno = ja convertido para o layout da GPU, na RAM (subir e
 *   so uma copia);  frio = so no arquivo mapeado (precisa converter antes de subir).
 * Com as vagas num buffer so por camada, um unico lancamento roda os especialistas do token. */
/* copia um especialista (par gate/up e down, ja no layout da GPU) para a vaga v da camada l.
   OpenCL: copia sem bloqueio (o chamador garante que o buffer de origem vive ate a fila esvaziar).
   Vulkan: os dados sao copiados na hora para a area de upload, entao a origem pode ser reusada. */
static void exp_write(int l, int v, const void *g, const void *dn) {
    if (gpu.vk) {
        if (!kvk_buf_write_q(vk_sg[l], (size_t)v * gx.bg[l], g, gx.bg[l]) || !kvk_buf_write_q(vk_sd[l], (size_t)v * gx.bd[l], dn, gx.bd[l]))
            die("falha ao subir especialista (Vulkan)");
        return;
    }
    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.sg[l], 0, (size_t)v * gx.bg[l], gx.bg[l], g, 0, NULL, NULL);
    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.sd[l], 0, (size_t)v * gx.bd[l], gx.bd[l], dn, 0, NULL, NULL);
}

/* poe o especialista e da camada l na vaga v (tirando quem estiver la) */
static void expert_load(Model *m, int l, int e, int v) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden;
    Layer *L = &m->layers[l];
    ExpSlot *sl = &gx.slot[(size_t)l * c->n_expert + e];
    int *own = gx.owner + (size_t)l * gx.S;
    const double t0 = now_sec();
    if (gx.pf && gx.pf[l]) {
        /* empacotado: o arquivo ja esta no layout da GPU. Le o especialista para um buffer fixo
           (o cache de arquivos do sistema faz o papel da RAM morna) e copia para a vaga. A fila
           esta vazia neste ponto (a leitura da entrada acabou de esvaziar), entao a copia
           bloqueante nao espera por nada. */
        const size_t per = gx.bg[l] + gx.bd[l];
        if (FSEEK64(gx.pf[l], (int64_t)((size_t)e * per), SEEK_SET) != 0 || fread(gx.stage, 1, per, gx.pf[l]) != per)
            die("MoE: falha ao ler especialista do shard");
        gx.t_read += now_sec() - t0;
        if (own[v] >= 0) { gx.slot[(size_t)l * c->n_expert + own[v]].slot = -1; gx.evict++; }
        own[v] = e; sl->slot = v;
        exp_write(l, v, gx.stage, gx.stage + gx.bg[l]);
        if (!gpu.vk) gerr |= cl.Finish(gpu.queue);     /* gx.stage e reusado na proxima carga */
        gx.t_up += now_sec() - t0; gx.loads++;
        return;
    }
    if (!sl->down) {            /* frio -> morno: converte para o layout da GPU, uma vez */
        Mat *two[2] = { &L->eg[e], &L->eu[e] }, *dn = &L->ed[e];
        size_t b0 = 0, b1 = 0;
        sl->glu = gpu_build_buf(two, 2, H, d, &b0);
        sl->down = gpu_build_buf(&dn, 1, d, H, &b1);
        if (!sl->glu || !sl->down || b0 != gx.bg[l] || b1 != gx.bd[l]) die("MoE: falha ao converter especialista");
        gx.conv++;
    }
    gx.loads++;
    if (own[v] >= 0) { gx.slot[(size_t)l * c->n_expert + own[v]].slot = -1; gx.evict++; }
    own[v] = e; sl->slot = v;
    /* os blobs mornos nunca sao liberados, entao a copia pode ficar na fila */
    exp_write(l, v, sl->glu, sl->down);
    gx.t_up += now_sec() - t0;
}

static void pack_job(void *c, int g0, int g1);

/* thread carregadora: atende os pedidos em ordem; nunca toca na GPU nem no pool de threads */
static void loader_do(LoadReq *r) {
    Model *m = gx.m;
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden;
    const int l = r->l, e = r->e;
    if (gx.pf && gx.pf[l]) {
        const size_t per = gx.bg[l] + gx.bd[l];
        if (FSEEK64(gx.pf[l], (int64_t)((size_t)e * per), SEEK_SET) != 0 || fread(r->buf, 1, per, gx.pf[l]) != per)
            die("MoE: falha ao ler especialista do shard");
    } else {    /* sem shards: converte do GGUF (bem mais lento; `katana pack` evita) */
        Layer *L = &m->layers[l];
        Mat *two[2] = { &L->eg[e], &L->eu[e] }, *dn = &L->ed[e];
        const size_t rpH = (H + 127) & ~(size_t)127, rpd = (d + 127) & ~(size_t)127;
        memset(r->buf, 0, gx.bg[l] + gx.bd[l]);
        PackJob j0 = { two, 2, H, d, gpu_wpr(two[0], d), (uint32_t *)r->buf };
        pack_job(&j0, 0, (int)(rpH / 32) * 2);
        PackJob j1 = { &dn, 1, d, H, gpu_wpr(dn, H), (uint32_t *)(r->buf + gx.bg[l]) };
        pack_job(&j1, 0, (int)(rpd / 32));
    }
}
#ifdef _WIN32
static DWORD WINAPI loader_thread(LPVOID a) {
    long tail = 0;
    (void)a;
    for (;;) {
        long h = gx.head;
        while (tail == h) { WaitOnAddress((volatile VOID *)&gx.head, &h, sizeof h, INFINITE); h = gx.head; }
        LoadReq *r = &gx.rq[tail % LQ];
        loader_do(r);
        r->done = 1;
        tail++;
    }
}
#else
static void *loader_thread(void *a) {
    long tail = 0;
    (void)a;
    for (;;) {
        while (tail == gx.head) usleep(200);
        LoadReq *r = &gx.rq[tail % LQ];
        loader_do(r);
        __atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
        tail++;
    }
    return NULL;
}
#endif
static void loader_start(Model *m, size_t maxper) {
    gx.m = m;
    for (int i = 0; i < LQ; i++) { gx.rq[i].buf = xalloc(maxper, 1); memset(gx.rq[i].buf, 1, maxper); }
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, loader_thread, NULL, 0, NULL);
    if (h) { CloseHandle(h); gx.loader = 1; }
#else
    pthread_t t;
    if (pthread_create(&t, NULL, loader_thread, NULL) == 0) gx.loader = 1;
#endif
}

/* pede a carga do especialista (se couber na fila) */
static void loader_request(int l, int e, int E) {
    ExpSlot *sl = &gx.slot[(size_t)l * E + e];
    if (sl->pending || gx.head - gx.fin_safe >= LQ) return;
    LoadReq *r = &gx.rq[gx.head % LQ];
    r->l = l; r->e = e; r->done = 0;
    sl->pending = 1;
#ifdef _WIN32
    InterlockedIncrement(&gx.head);
    WakeByAddressSingle((PVOID)&gx.head);
#else
    __atomic_add_fetch(&gx.head, 1, __ATOMIC_RELEASE);
#endif
}

/* Memoria de assunto entre sessoes: o calor de cada especialista e gravado ao sair e, na
   partida seguinte, os mais quentes de cada camada sao pedidos a thread carregadora antes
   mesmo do primeiro token. */
static void heat_path(char *out, size_t n) { snprintf(out, n, "%s.heat", shards.dir); }
static void heat_save(const Model *m) {
    char fn[1100];
    const int32_t hdr[2] = { m->cfg.n_layers, m->cfg.n_expert };
    if (!gx.on || !gx.heat || getenv("KATANA_NOHEAT")) return;
    heat_path(fn, sizeof fn);
    FILE *f = fopen(fn, "wb");
    if (!f) return;
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(gx.heat, sizeof(float), (size_t)hdr[0] * hdr[1], f);
    fclose(f);
}
static int cmp_pre(const void *a, const void *b) {
    const float x = gx.heat[*(const uint32_t *)a], y = gx.heat[*(const uint32_t *)b];
    return x < y ? 1 : x > y ? -1 : 0;
}
static void heat_load(const Model *m) {
    char fn[1100];
    int32_t hdr[2] = { 0, 0 };
    const int NL = m->cfg.n_layers, E = m->cfg.n_expert;
    if (getenv("KATANA_NOHEAT")) return;
    heat_path(fn, sizeof fn);
    FILE *f = fopen(fn, "rb");
    if (!f) return;
    if (fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == NL && hdr[1] == E
        && fread(gx.heat, sizeof(float), (size_t)NL * E, f) == (size_t)NL * E) {
        /* por camada, os S mais quentes; a lista final vai do mais quente para o mais frio */
        gx.pre = xalloc((size_t)NL * gx.S, sizeof(uint32_t));
        uint32_t *idx = xalloc(E, sizeof(uint32_t));
        for (int l = 0; l < NL; l++) {
            for (int e = 0; e < E; e++) idx[e] = (uint32_t)(l * E + e);
            qsort(idx, E, sizeof(uint32_t), cmp_pre);
            for (int i = 0; i < gx.S; i++) if (gx.heat[idx[i]] > 0.05f) gx.pre[gx.n_pre++] = idx[i];
        }
        free(idx);
        qsort(gx.pre, gx.n_pre, sizeof(uint32_t), cmp_pre);
        for (size_t i = 0; i < (size_t)NL * E; i++) gx.heat[i] *= 0.5f;     /* a sessao nova pode discordar */
    } else memset(gx.heat, 0, (size_t)NL * E * sizeof(float));
    fclose(f);
}
static void loader_request(int l, int e, int E);
static void preload_feed(const Model *m);
/* sessao retomada: troca o calor pelo da sessao e pede so os especialistas que ela usou (os S mais quentes de
   cada camada). Com a thread carregadora a carga corre em paralelo com o prompt; sem ela, carrega ja.
   Devolve quantos especialistas foram pedidos (0 se o cache de especialistas nao esta ligado). */
static int heat_apply(Model *m, const float *h, int NL_f, int E_f) {
    const int NL = m->cfg.n_layers, E = m->cfg.n_expert;
    if (!gx.on || !gx.heat || NL_f != NL || E_f != E) return 0;
    memcpy(gx.heat, h, (size_t)NL * E * sizeof(float));
    free(gx.pre); gx.pre = xalloc((size_t)NL * gx.S, sizeof(uint32_t));
    gx.n_pre = 0; gx.i_pre = 0;
    uint32_t *idx = xalloc(E, sizeof(uint32_t));
    for (int l = 0; l < NL; l++) {
        if (!m->layers[l].eg) continue;
        for (int e = 0; e < E; e++) idx[e] = (uint32_t)(l * E + e);
        qsort(idx, E, sizeof(uint32_t), cmp_pre);
        for (int i = 0; i < gx.S; i++) if (gx.heat[idx[i]] > 0.05f) gx.pre[gx.n_pre++] = idx[i];
    }
    free(idx);
    qsort(gx.pre, gx.n_pre, sizeof(uint32_t), cmp_pre);
    int asked = 0;
    if (gx.loader) {
        for (size_t i = 0; i < gx.n_pre; i++) asked += gx.slot[gx.pre[i]].slot < 0;
        preload_feed(m);
    } else {            /* sem carregadora: poe ja nas vagas, do mais quente ao mais frio de cada camada */
        int *fill = xalloc(NL, sizeof(int));
        for (size_t i = 0; i < gx.n_pre; i++) {
            const int l = (int)(gx.pre[i] / E), e = (int)(gx.pre[i] % E);
            if (gx.slot[gx.pre[i]].slot >= 0) { fill[l]++; continue; }
            int *own = gx.owner + (size_t)l * gx.S, v = -1;
            for (int k = 0; k < gx.S && v < 0; k++) if (own[k] < 0) v = k;
            for (int k = 0; k < gx.S && v < 0; k++) if (gx.heat[(size_t)l * E + own[k]] < gx.heat[gx.pre[i]] && fill[l] < gx.S) v = k;
            if (v < 0) continue;
            expert_load(m, l, e, v); fill[l]++; asked++;
        }
        free(fill);
        gx.i_pre = gx.n_pre;
    }
    return asked;
}
static void preload_feed(const Model *m) {
    const int E = m->cfg.n_expert;
    while (gx.i_pre < gx.n_pre && gx.head - gx.fin_safe < LQ) {
        const uint32_t id = gx.pre[gx.i_pre++];
        if (gx.slot[id].slot < 0) loader_request((int)(id / E), (int)(id % E), E);
    }
}

/* instala na VRAM as cargas que ficaram prontas. So e chamada logo depois de uma leitura
   bloqueante: a fila da GPU esta vazia, entao nenhuma vaga esta em uso e as copias anteriores
   (dos buffers dos pedidos) ja terminaram. */
static void loader_install(Model *m) {
    const int E = m->cfg.n_expert;
    gx.fin_safe = gx.fin;
    while (gx.fin < gx.head && gx.rq[gx.fin % LQ].done) {
        LoadReq *r = &gx.rq[gx.fin % LQ];
        const int l = r->l, e = r->e;
        ExpSlot *sl = &gx.slot[(size_t)l * E + e];
        int *own = gx.owner + (size_t)l * gx.S;
        const float *heat = gx.heat + (size_t)l * E;
        int v = -1;
        for (int i = 0; i < gx.S; i++) {
            if (own[i] < 0) { v = i; break; }
            if (v < 0 || heat[own[i]] < heat[own[v]]) v = i;
        }
        sl->pending = 0;
        gx.fin++;
        if (own[v] >= 0 && heat[e] <= heat[own[v]]) continue;      /* esfriou enquanto carregava */
        const double t0 = now_sec();
        if (own[v] >= 0) { gx.slot[(size_t)l * E + own[v]].slot = -1; gx.evict++; }
        own[v] = e; sl->slot = v;
        exp_write(l, v, r->buf, r->buf + gx.bg[l]);
        gx.loads++;
        gx.t_up += now_sec() - t0;
    }
    preload_feed(m);
}

/* tabelas do cache de especialistas (vagas, calor, shards, thread carregadora); gx.S e gx.bg/gx.bd ja preenchidos */
static void moe_tables(Model *m, size_t S, size_t per) {
    const Config *c = &m->cfg;
    const int E = c->n_expert, NL = c->n_layers;
    const size_t d = c->dim;
    int n_moe = 0;
    for (int l = 0; l < NL; l++) n_moe += m->layers[l].eg != NULL;
    gx.m = m;
    gx.slot = xalloc((size_t)NL * E, sizeof(ExpSlot));
    for (size_t i = 0; i < (size_t)NL * E; i++) gx.slot[i].slot = -1;
    gx.owner = xalloc((size_t)NL * S, sizeof(int)); gx.stamp = xalloc((size_t)NL * S, sizeof(uint64_t));
    for (size_t i = 0; i < (size_t)NL * S; i++) gx.owner[i] = -1;
    gx.heat = xalloc((size_t)NL * E, sizeof(float));
    gx.cpu_out = xalloc((size_t)GPU_BMAX * c->n_used * d, sizeof(float));
    gx.cpu_cold = !getenv("KATANA_MOE_CPU") || atoi(getenv("KATANA_MOE_CPU")) != 0;
    gx.decay = getenv("KATANA_HEAT_DECAY") ? (float)atof(getenv("KATANA_HEAT_DECAY")) : 0.98f;
    if (!(gx.decay > 0.0f && gx.decay < 1.0f)) gx.decay = 0.98f;
    if (shards.use) {       /* especialistas empacotados por `katana pack`: um arquivo por camada */
        int n_pk = 0;
        size_t maxper = 0;
        gx.pf = xalloc(NL, sizeof(FILE *));
        for (int l = 0; l < NL; l++) {
            char nm[32], fn[1100];
            const size_t want = (size_t)E * (gx.bg[l] + gx.bd[l]);
            int found = 0;
            if (!m->layers[l].eg) continue;
            snprintf(nm, sizeof nm, "L%03d.exps", l);
            for (int i = 0; i < shards.n; i++) if (strcmp(shards.e[i].name, nm) == 0 && shards.e[i].bytes == want) found = 1;
            if (!found) continue;
            snprintf(fn, sizeof fn, "%s/%s.bin", shards.dir, nm);
            gx.pf[l] = fopen(fn, "rb");
            if (!gx.pf[l]) continue;
            setvbuf(gx.pf[l], NULL, _IONBF, 0);
            if (gx.bg[l] + gx.bd[l] > maxper) maxper = gx.bg[l] + gx.bd[l];
            n_pk++;
        }
        if (n_pk) { gx.stage = xalloc(maxper, 1); memset(gx.stage, 1, maxper); }
        if (n_pk != n_moe) { for (int l = 0; l < NL; l++) if (gx.pf[l]) { fclose(gx.pf[l]); gx.pf[l] = NULL; } }
        if (n_pk) fprintf(stderr, "GPU: especialistas de %d camadas direto dos shards em %s\n", n_pk, shards.dir);
    }
    if (gx.cpu_cold && !getenv("KATANA_NOLOADER")) {
        size_t maxper = 0;
        for (int l = 0; l < NL; l++) if (gx.bg[l] + gx.bd[l] > maxper) maxper = gx.bg[l] + gx.bd[l];
        loader_start(m, maxper);
        /* margem de calor para trocar uma vaga na geracao. qwen35moe (grafo Vulkan, 17 de 256 vagas): 8 mediu 4-8%
           mais rapido que 4 (metade das subidas, subida bloqueia a fila); gemma4 (9 de 128) ficou igual: segue 4 */
        gx.swap_margin = getenv("KATANA_SWAP") ? (float)atof(getenv("KATANA_SWAP")) : c->ssm_inner ? 8.0f : 4.0f;
        if (gx.loader) { heat_load(m); preload_feed(m); }
    }
    gx.on = 1;
    fprintf(stderr, "GPU: cache de especialistas: %d de %d por camada na VRAM (%.0f MB)\n", gx.S, E, (double)S * per / 1048576.0);
}

