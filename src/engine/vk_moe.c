/* ---------- MoE em Vulkan (um token por vez) ----------
   Atencao de cada camada na GPU (vk_emit_attn); a entrada normalizada do feed-forward volta para a CPU, que roteia
   (moe_route) e decide, pelo calor, quem roda na GPU (especialista numa vaga do buffer da camada) e quem roda na CPU
   (frio, direto do arquivo mapeado). A thread carregadora traz os que esquentam. Os despachos de uma camada saem ao vivo:
   gate/up e down de cada quente (um despacho cada, a vaga entra por deslocamento em palavras), as saidas da CPU sobem
   para as linhas livres de vkm.bt e moe_sum soma tudo, pesado, no residual. Uma leitura bloqueante por camada. */
static struct { long lay, allhit, tok; double t_rd, t_route, t_cold; } gprof;    /* KATANA_VK_GENPROF: perfil da geracao MoE */
static int m_nused;
static struct { KvkBuf hb, bt, meta, hs, *rw; KvkPipe sum, sumrms, rpipe; int sh, rt; } vkm;   /* rt: roteador na GPU (rw: pesos float32 em linhas) */   /* sh: compartilhado na GPU (linha K de bt) */

static int vk_moe_init(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden;
    const int NL = c->n_layers, E = c->n_expert, K = c->n_used;
    if (K > 16) { fprintf(stderr, "Vulkan: MoE com %d especialistas por token nao suportado\n", K); return 0; }
    vkm.sh = 0;
    if (c->n_shared) {      /* compartilhado: so se ja subiu inteiro (par gate/up + down) */
        for (int l = 0; l < NL; l++) {
            const Layer *L = &m->layers[l];
            if (!L->sg.gbuf || !L->sg.pair || !L->sd.gbuf) { fprintf(stderr, "Vulkan: compartilhado fora da VRAM\n"); return 0; }
        }
        vkm.sh = 1;
    }
    if (c->n_dense || c->gate_sig) { fprintf(stderr, "Vulkan: MoE com camadas densas ou roteador sigmoide ainda nao suportado\n"); return 0; }
    vkm.rt = 0;
    if (!c->ssm_inner && E % 32 == 0 && vkg.xn.size >= (d + E) * 4 && !(getenv("KATANA_VK_ROUTER") && atoi(getenv("KATANA_VK_ROUTER")) == 0)) {
        /* roteador na GPU: os logits saem junto com a entrada do feed-forward, sem o produto na CPU. Pesos float32 no
           layout da GPU (grupos de 32 linhas, coluna a coluna), o mesmo que o mm.comp le no lote */
        int f = 0;
        float *wr = xalloc((size_t)E * d, sizeof(float)), *row = xalloc(d, sizeof(float));
        vkm.rw = xalloc(NL, sizeof(KvkBuf));
        for (int l = 0; l < NL && !f; l++) {
            for (int e = 0; e < E; e++) {
                mat_row(row, &m->layers[l].router, e, (int)d);
                for (size_t i = 0; i < d; i++) wr[(size_t)(e / 32) * d * 32 + i * 32 + e % 32] = row[i];
            }
            vkm.rw[l] = vk_vec(wr, (size_t)E * d, &f);
        }
        free(row);
        free(wr);
        vkm.rpipe = kvk_pipe_create(moe_router_spv, sizeof moe_router_spv, 3, 8, NULL, NULL, 0);
        if (!f && vkm.rpipe.p) { vkm.rt = 1; gpu.used += (size_t)NL * E * d * 4; }
        else for (int l = 0; l < NL; l++) if (vkm.rw[l].b) kvk_buf_destroy(&vkm.rw[l]);
    }
    gx.bg = xalloc(NL, sizeof(size_t)); gx.bd = xalloc(NL, sizeof(size_t));
    size_t per = 0;
    for (int l = 0; l < NL; l++) {
        const Layer *L = &m->layers[l];
        if (!glu_pair(&L->w_gate, &L->w_up)) { fprintf(stderr, "Vulkan: gate e up em formatos diferentes\n"); return 0; }
        if (!gpu_has(&L->w_gate) || !gpu_has(&L->w_down)) { fprintf(stderr, "Vulkan: especialistas em formato sem kernel de GPU\n"); return 0; }
        gx.bg[l] = 2 * gpu_mat_bytes(&L->w_gate, H, d); gx.bd[l] = gpu_mat_bytes(&L->w_down, d, H);
        per += gx.bg[l] + gx.bd[l];
    }
    uint64_t budget = gpu_budget();
    size_t avail = budget > gpu.used + ((size_t)16 << 20) ? (size_t)(budget - gpu.used - ((size_t)16 << 20)) : 0;
    if (getenv("KATANA_EXPERT_MB")) { const size_t lim = (size_t)atoll(getenv("KATANA_EXPERT_MB")) << 20; if (lim < avail) avail = lim; }
    size_t S = avail / per;
    if (S > (size_t)E) S = E;
    if (S < (size_t)K) { fprintf(stderr, "Vulkan: sobra VRAM para %zu vagas por camada, precisa de %d\n", S, K); return 0; }
    int fail = 0;
    vkm.hb = vk_wk((K * H + 128) * 4, &fail); vkm.bt = vk_wk((size_t)(K + 1) * d * 4, &fail); vkm.meta = vk_wk((size_t)8 * (K + 1) * 4, &fail);
    if (vkm.sh) vkm.hs = vk_wk(((size_t)c->n_shared * H + 128) * 4, &fail);
    vk_sg = xalloc(NL, sizeof(KvkBuf)); vk_sd = xalloc(NL, sizeof(KvkBuf));
    for (int l = 0; l < NL && !fail; l++) {
        vk_sg[l] = kvk_buf_create(S * gx.bg[l], KVK_USAGE_RW);
        vk_sd[l] = kvk_buf_create(S * gx.bd[l], KVK_USAGE_RW);
        if (!vk_sg[l].b || !vk_sd[l].b) fail = 1;
    }
    vkm.sum = kvk_pipe_create(moe_sum_spv, sizeof moe_sum_spv, 3, 16, NULL, NULL, 0);
    vkm.sumrms = kvk_pipe_create(moe_sum_rms_spv, sizeof moe_sum_rms_spv, 5, 16, NULL, NULL, 0);
    if (fail || !vkm.sum.p || !vkm.sumrms.p) {
        fprintf(stderr, "Vulkan: sem memoria para o cache de especialistas (%s)\n", kvk_error());
        for (int l = 0; l < NL; l++) { if (vk_sg[l].b) kvk_buf_destroy(&vk_sg[l]); if (vk_sd[l].b) kvk_buf_destroy(&vk_sd[l]); }
        return 0;
    }
    gpu.used += S * per;
    gx.S = (int)S;
    gx.xn = xalloc(d + E, sizeof(float));
    moe_tables(m, S, per);
    return 1;
}

/* feed-forward MoE da camada l para o token atual; a entrada normalizada ja esta em gx.xn.
   nnorm.b != 0: a soma tambem calcula a norma de entrada da camada seguinte (peso nnorm) em vkg.xn */
static void vk_moe_ffn_x(Model *m, const Layer *L, int l, KvkBuf nnorm);
static void vk_moe_ffn(Model *m, const Layer *L, int l) { const KvkBuf z = { 0 }; vk_moe_ffn_x(m, L, l, z); }
static void vk_moe_ffn_x(Model *m, const Layer *L, int l, KvkBuf nnorm) {
    const Config *c = &m->cfg;
    const int d = c->dim, H = c->hidden, K = c->n_used, E = c->n_expert;
    static const int zero[16];
    int cpu_e[16], cpu_j[16], hit_j[16], hit_v[16], n_cpu = 0, nh = 0;
    uint32_t meta[8 * (16 + 1)];
    float *heat = gx.heat + (size_t)l * E;
    int *own = gx.owner + (size_t)l * gx.S;
    uint64_t *st = gx.stamp + (size_t)l * gx.S;
    if (gx.loader) loader_install(m);
    const uint64_t tick = ++gx.clock;
    memcpy(m->xb, gx.xn, (size_t)d * sizeof(float));
    const double trt0 = now_sec();
    moe_route(m, L, l);
    gprof.t_route += now_sec() - trt0;
    for (int e = 0; e < E; e++) heat[e] *= gx.decay;
    for (int j = 0; j < K; j++) heat[moe.sel[j]] += 1.0f;
    for (int j = 0; j < K; j++) {
        const int e = moe.sel[j];
        int v = gx.slot[(size_t)l * E + e].slot;
        if (v >= 0) gx.hits++;
        else {
            gx.miss++;
            int cand = -1;      /* vaga livre, ou o residente mais frio que nao e deste token */
            for (int i = 0; i < gx.S; i++) {
                if (own[i] < 0) { cand = i; break; }
                if (st[i] == tick) continue;
                if (cand < 0 || heat[own[i]] < heat[own[cand]]) cand = i;
            }
            if (gx.loader) {    /* roda na CPU agora; se merecer, sobe num proximo token */
                if (cand >= 0 && (own[cand] < 0 || heat[e] > heat[own[cand]] + gx.swap_margin)) loader_request(l, e, E);
            } else if (cand >= 0 && (own[cand] < 0 || !gx.cpu_cold || heat[e] > heat[own[cand]] + 0.5f)) {
                expert_load(m, l, e, cand); v = cand;
            }
        }
        if (v >= 0) { st[v] = tick; hit_j[nh] = j; hit_v[nh++] = v; }
        else { cpu_e[n_cpu] = e; cpu_j[n_cpu++] = j; }
    }
    const uint32_t kg = gpu_kind(&L->eg[0]), kd = gpu_kind(&L->ed[0]);
    int ok = 1;
    gprof.lay++; gprof.allhit += n_cpu == 0; m_nused = K;
    const int sh = vkm.sh && L->sg.gbuf, Kt = K + sh, mo = 6 * nh;     /* tabela das vagas, depois pesos e linhas da soma */
    for (int i = 0; i < nh; i++) {   /* gate/up: (vaga, entrada 0, saida i em hb); down: (vaga, entrada i, linha j em bt) */
        uint32_t *t = meta + 6 * i;
        t[0] = (uint32_t)((size_t)hit_v[i] * (gx.bg[l] / 4)); t[1] = 0; t[2] = (uint32_t)i;
        t[3] = (uint32_t)((size_t)hit_v[i] * (gx.bd[l] / 4)); t[4] = (uint32_t)i; t[5] = (uint32_t)hit_j[i];
    }
    if (sh) {               /* compartilhado: linha K de bt, peso = portao sigmoide escalar (ou 1) */
        const int Hs = c->n_shared * H;
        const float sw = shexp_gate_w(m, L);
        ok &= vk_gmv_ex(&L->sg, vkg.xn, vkm.hs, vkg.dummy, Hs, d, 0, 1);
        ok &= vk_gmv_raw(gpu_kind(&L->sd), *(const KvkBuf *)L->sd.gbuf, vkm.hs, vkm.bt, vkg.dummy, d, Hs, 0, 0, L->sd.gs, L->sd.go, 0, K * d);
        memcpy(&meta[mo + K], &sw, 4);
    }
    for (int j = 0; j < K; j++) memcpy(&meta[mo + j], &moe.w[j], 4);
    for (int j = 0; j < Kt; j++) meta[mo + Kt + j] = (uint32_t)j;
    ok &= kvk_buf_write_q(vkm.meta, 0, meta, (size_t)(mo + 2 * Kt) * 4);
    if (nh > 0) {           /* quentes: gate/up+SiLU num lancamento, down noutro */
        ok &= vk_gmv_multi(kg, vk_sg[l], vkg.xn, vkm.hb, vkm.meta, H, d, 1, nh, 0, H, 0);
        ok &= vk_gmv_multi(kd, vk_sd[l], vkm.hb, vkm.bt, vkm.meta, d, H, 0, nh, H, d, 3);
    }
    if (!ok) die("falha na execucao em Vulkan (MoE)");
    if (n_cpu > 0) {    /* os frios, na CPU, enquanto a GPU trabalha */
        const double t0 = now_sec();
        kvk_flush();
        moe_cpu_multi(m, L, cpu_e, zero, n_cpu, 1, gx.xn, gx.cpu_out);
        for (int i = 0; i < n_cpu; i++)
            if (!kvk_buf_write_q(vkm.bt, (size_t)cpu_j[i] * d * 4, gx.cpu_out + (size_t)i * d, (size_t)d * 4)) die("falha na execucao em Vulkan (MoE, CPU)");
        gx.cpu_runs += n_cpu;
        gx.t_cpu += now_sec() - t0; gprof.t_cold += now_sec() - t0;
    }
    if (nnorm.b) {
        const KvkBuf sb[5] = { vkg.x, vkm.bt, vkm.meta, nnorm, vkg.xn };
        const struct { int woff, K, n; float eps; } pc = { mo, Kt, d, c->eps };
        if (!kvk_dispatch(vkm.sumrms, sb, 5, &pc, sizeof pc, 1, 1, 1)) die("falha na execucao em Vulkan (MoE, soma)");
        return;
    }
    const KvkBuf sb[3] = { vkg.x, vkm.bt, vkm.meta };
    const int pc[4] = { mo, Kt, d, 1 };
    if (!kvk_dispatch(vkm.sum, sb, 3, pc, sizeof pc, (uint32_t)((d + 127) / 128), 1, 1)) die("falha na execucao em Vulkan (MoE, soma)");
}

/* um token pelas camadas todas: atencao e logits do roteador na GPU, uma leitura por camada (entrada do
   feed-forward + logits), escolha e especialistas camada a camada */
static float *vk_forward_moe(Model *m, int want_logits) {
    const Config *c = &m->cfg;
    const int d = c->dim, E = c->n_expert;
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
        int ok = vk_emit_attn_x(m, l, l > 0);     /* a norma de entrada (l > 0) veio somada com o MoE da anterior */
        if (vkm.rt) {
            const KvkBuf rb[3] = { vkm.rw[l], vkg.xn, vkg.xn };
            const int pc[2] = { d, d };
            ok &= kvk_dispatch(vkm.rpipe, rb, 3, pc, sizeof pc, (uint32_t)(E / 32), 1, 1);
        }
        if (!ok) die("falha na execucao em Vulkan (atencao)");
        if (!kvk_buf_read(vkg.xn, 0, gx.xn, (size_t)(d + (vkm.rt ? E : 0)) * sizeof(float))) die("falha na execucao em Vulkan (leitura)");
        moe_lg_ext = vkm.rt ? gx.xn + d : NULL;
        const KvkBuf z = { 0 };
        vk_moe_ffn_x(m, L, l, l + 1 < c->n_layers ? vkg.an[l + 1] : z);
        moe_lg_ext = NULL;
    }
    if (!want_logits) { if (!kvk_flush()) die("falha na execucao em Vulkan"); return NULL; }
    if (!vk_emit_head(m) || !kvk_buf_read(vkg.lg, 0, m->logits, (size_t)c->vocab * sizeof(float))) die("falha na execucao em Vulkan (cabeca)");
    return m->logits;
}

/* MoE do lote na camada l (residual em vkb.x, B tokens). Norma em fp32 e logits do roteador na GPU (mm preciso),
   uma leitura traz os dois; a CPU escolhe os especialistas de cada token, atualiza calor e vagas como no caminho de um
   token e agrupa os pares (token, especialista) por vaga. Na GPU cada vaga roda sobre as colunas dos seus tokens
   (moe_gath, mm glu e down com a vaga por deslocamento, moe_scat soma pesada no residual); os frios rodam na CPU
   ao mesmo tempo e sobem somados. KATANA_VK_MOEP=1: produtos dos especialistas em fp32 (alta + resto). */
/* gemma4: a soma dos especialistas vai para um buffer proprio (normas de saida antes do residual) e os logits do
   roteador vem prontos da CPU (a entrada do roteador nao e a dos especialistas): [B][E] */
static KvkBuf vk_moe_out;
static const float *vk_moe_lgh;
static int vk_moe_scat(KvkBuf y, int d, int B, int n, int moff) {
    const KvkBuf b[3] = { vk_moe_out.b ? vk_moe_out : vkb.x, y, vkbm.meta };
    const int pc[4] = { d, B, n, moff };
    return kvk_dispatch(vkbm.scat, b, 3, pc, sizeof pc, (uint32_t)(((size_t)d * n + 255) / 256), 1, 1);
}
/* sobe o especialista e (frio) da camada l para a vaga temporaria j do lote: do shard empacotado (arquivo proprio,
   a thread carregadora usa o dela), do blob morno se ja existir, ou convertendo na hora (sem guardar) */
static void vk_moe_tmp_load(Model *m, const Layer *L, int l, int e, int j) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, bg = gx.bg[l], bd = gx.bd[l];
    const ExpSlot *sl = &gx.slot[(size_t)l * c->n_expert + e];
    const void *g = NULL, *dn = NULL;
    uint32_t *cg = NULL, *cd = NULL;
    if (gx.pf && gx.pf[l]) {
        if (!vkbm.pf[l]) {
            char fn[1100];
            snprintf(fn, sizeof fn, "%s/L%03d.exps.bin", shards.dir, l);
            vkbm.pf[l] = fopen(fn, "rb");
            if (vkbm.pf[l]) setvbuf(vkbm.pf[l], NULL, _IONBF, 0);
        }
        const double t0 = now_sec();
        if (vkbm.pf[l] && FSEEK64(vkbm.pf[l], (int64_t)((size_t)e * (bg + bd)), SEEK_SET) == 0
            && fread(vkbm.stage, 1, bg + bd, vkbm.pf[l]) == bg + bd) { g = vkbm.stage; dn = vkbm.stage + bg; }
        vkbm.t_frd += now_sec() - t0;
    }
    if (!g && sl->down) { g = sl->glu; dn = sl->down; }
    if (!g) {
        Mat *two[2] = { &L->eg[e], &L->eu[e] }, *dm = &L->ed[e];
        size_t b0 = 0, b1 = 0;
        cg = gpu_build_buf(two, 2, H, d, &b0); cd = gpu_build_buf(&dm, 1, d, H, &b1);
        if (!cg || !cd || b0 != bg || b1 != bd) die("MoE: falha ao converter especialista");
        g = cg; dn = cd;
    }
    if (!kvk_buf_write_q(vkbm.tg, (size_t)j * vkbm.tgb, g, bg) || !kvk_buf_write_q(vkbm.td, (size_t)j * vkbm.tdb, dn, bd))
        die("falha ao subir especialista (Vulkan, lote)");
    free(cg); free(cd);
}
/* Vida util: vkbm.tstage e os arquivos vkbm.pf duram o processo todo. A regiao j so e regravada pela thread da camada
   seguinte, que comeca depois da leitura bloqueante do vk_moe_n (GPU ociosa: as copias anteriores ja terminaram);
   a thread sempre e juntada antes do vk_moe_n voltar, e o fallback so usa o arquivo depois do join.
   A thread le do shard para vkbm.tstage; a principal sobe com kvk_buf_write_q (copia os bytes na hora para o anel de
   staging, sem depender de coerencia de memoria mapeada). done[j]: 1 pronto, -1 falhou. */
static struct { int l, n; size_t bg, bd; volatile long done[64]; } vk_trd;
static void vk_trd_run(void) {
    FILE *f = vkbm.pf[vk_trd.l];
    for (int j = 0; j < vk_trd.n; j++) {
        uint8_t *dst = vkbm.tstage + (size_t)j * (vkbm.tgb + vkbm.tdb);
        const int e = vkbm.tmpe[j];
        const int ok = f && FSEEK64(f, (int64_t)((size_t)e * (vk_trd.bg + vk_trd.bd)), SEEK_SET) == 0
                       && fread(dst, 1, vk_trd.bg, f) == vk_trd.bg && fread(dst + vkbm.tgb, 1, vk_trd.bd, f) == vk_trd.bd;
        ATOM_SET(&vk_trd.done[j], ok ? 1 : -1);
    }
}
#ifdef _WIN32
static DWORD WINAPI vk_trd_thread(LPVOID a) { (void)a; vk_trd_run(); return 0; }
#else
static void *vk_trd_thread(void *a) { (void)a; vk_trd_run(); return NULL; }
#endif
/* frios do lote na CPU (blocos de ate GPU_BMAX tokens), somados em vkbm.xc; *nc vira 0 */
static void vk_moe_cold_cpu(Model *m, const Layer *L, int l, int B, int *pnc) {
    const int d = m->cfg.dim, nc = *pnc;
    if (nc <= 0) return;
    const double t0 = now_sec();
    memset(vkbm.xc, 0, (size_t)d * B * sizeof(float));
    int p = 0;
    while (p < nc) {
        const int b0 = vkbm.cb[p];
        int q = p;
        while (q < nc && vkbm.cb[q] < b0 + GPU_BMAX) q++;
        static int xi[GPU_BMAX * 64];
        for (int i = p; i < q; i++) xi[i - p] = vkbm.cb[i] - b0;
        const int nb = vkbm.cb[q - 1] - b0 + 1;
        moe_cpu_multi(m, L, vkbm.ce + p, xi, q - p, nb, vkbm.xn + (size_t)b0 * d, vkbm.outs);
        for (int i = p; i < q; i++) {
            const float w = vkbm.cw[i], *o = vkbm.outs + (size_t)(i - p) * d;
            const int b = vkbm.cb[i];
            for (int r = 0; r < d; r++) vkbm.xc[(size_t)r * B + b] += w * o[r];
        }
        p = q;
    }
    gx.cpu_runs += nc;
    gx.t_cpu += now_sec() - t0; vkbm.t_cpu += now_sec() - t0;
    vkbm.cdone = 1; *pnc = 0;
    (void)l;
}
static void vk_moe_n(Model *m, const Layer *L, int l, int B) {
    const Config *c = &m->cfg;
    const int d = c->dim, K = c->n_used, E = c->n_expert, H = c->hidden, Hs = c->n_shared * H, prec = vkbm.prec;
    float *heat = gx.heat + (size_t)l * E;
    int *own = gx.owner + (size_t)l * gx.S;
    uint64_t *st = gx.stamp + (size_t)l * gx.S;
    const struct { int n; float eps; int B; } rp = { d, c->eps, B };
    int ok = 1;
    const KvkBuf rb[3] = { vkb.x, vkg.fn[l], vkbm.xf };
    ok &= kvk_dispatch(vkbm.rms32, rb, 3, &rp, sizeof rp, (uint32_t)((B + 7) / 8), 1, 1);
    if (!prec) { const KvkBuf rh[3] = { vkb.x, vkg.fn[l], vkb.xh }; ok &= kvk_dispatch(vkb.rms16, rh, 3, &rp, sizeof rp, (uint32_t)((B + 7) / 8), 1, 1); }
    if (vkm.rt) ok &= vk_mm_pw(T_F32, vkm.rw[l], 0, (size_t)d, vkbm.xf, vkbm.lg, E, d, B, 0, 1);
    if (!ok) die("falha na execucao em Vulkan (MoE do lote)");
    const double ta = now_sec();
    if (!kvk_buf_read(vkbm.xf, 0, vkb.xT, (size_t)d * B * sizeof(float))) die("falha na execucao em Vulkan (leitura do lote)");
    if (vkm.rt && !kvk_buf_read(vkbm.lg, 0, vkbm.lgT, (size_t)E * B * sizeof(float))) die("falha na execucao em Vulkan (leitura do lote)");
    const double tb = now_sec();
    vkbm.t_rd += tb - ta;
    if (gx.loader) loader_install(m);       /* fila vazia: as vagas podem ser reescritas */
    const uint64_t tick = ++gx.clock;
    int np = 0, nc = 0;
    static float *lcol;
    if (!lcol) lcol = xalloc(E, sizeof(float));
    for (int v = 0; v <= gx.S + vkbm.T; v++) vkbm.cnt[v] = 0;
    for (int b = 0; b < B; b++) {
        float *xn = vkbm.xn + (size_t)b * d;
        for (int i = 0; i < d; i++) xn[i] = vkb.xT[(size_t)i * B + b];
        memcpy(m->xb, xn, (size_t)d * sizeof(float));
        if (layer_tap && c->ssm_inner) layer_tap(l, vkq_posn + b, xn);
        if (vk_moe_lgh) moe_lg_ext = vk_moe_lgh + (size_t)b * E;
        else if (vkm.rt) { for (int e = 0; e < E; e++) lcol[e] = vkbm.lgT[(size_t)e * B + b]; moe_lg_ext = lcol; }
        moe_route(m, L, l);
        moe_lg_ext = NULL;
        vkbm.sw[b] = vkm.sh ? shexp_gate_w(m, L) : 0.0f;
        for (int e = 0; e < E; e++) heat[e] *= gx.decay;
        for (int j = 0; j < K; j++) heat[moe.sel[j]] += 1.0f;
        for (int j = 0; j < K; j++) {
            const int e = moe.sel[j];
            int v = gx.slot[(size_t)l * E + e].slot;
            if (v >= 0) gx.hits++;
            else {
                gx.miss++;
                int cand = -1;
                for (int i = 0; i < gx.S; i++) {
                    if (own[i] < 0) { cand = i; break; }
                    if (st[i] == tick) continue;
                    if (cand < 0 || heat[own[i]] < heat[own[cand]]) cand = i;
                }
                if (gx.loader) { if (cand >= 0 && own[cand] < 0) loader_request(l, e, E); }   /* lote: so vaga livre */
                else if (cand >= 0 && (own[cand] < 0 || !gx.cpu_cold || heat[e] > heat[own[cand]] + 0.5f)) { expert_load(m, l, e, cand); v = cand; }
            }
            if (v >= 0) { st[v] = tick; vkbm.pv[np] = v; vkbm.pb[np] = b; vkbm.pw[np++] = moe.w[j]; vkbm.cnt[v]++; }
            else { vkbm.ce[nc] = e; vkbm.cb[nc] = b; vkbm.cw[nc++] = moe.w[j]; }
        }
    }
    /* frios com cmin ou mais tokens no lote: vagas temporarias na GPU (os mais usados primeiro) */
    int ntmp = 0;
    if (vkbm.T && nc > 0) {
        for (int i = 0; i < nc; i++) vkbm.ccnt[vkbm.ce[i]]++;
        while (ntmp < vkbm.T) {
            int be = -1;
            for (int e = 0; e < E; e++) if (vkbm.ccnt[e] >= vkbm.cmin && (be < 0 || vkbm.ccnt[e] > vkbm.ccnt[be])) be = e;
            if (be < 0) break;
            vkbm.tmpe[ntmp] = be; vkbm.ccnt[be] = -1 - ntmp; ntmp++;
        }
        int q = 0;
        for (int i = 0; i < nc; i++) {
            const int e = vkbm.ce[i], c0 = vkbm.ccnt[e];
            if (c0 < 0) { const int v = gx.S - 1 - c0; vkbm.pv[np] = v; vkbm.pb[np] = vkbm.cb[i]; vkbm.pw[np++] = vkbm.cw[i]; vkbm.cnt[v]++; }
            else { vkbm.ce[q] = e; vkbm.cb[q] = vkbm.cb[i]; vkbm.cw[q++] = vkbm.cw[i]; }
        }
        memset(vkbm.ccnt, 0, (size_t)E * sizeof(int));
        vkbm.n_tmp += nc - q; vkbm.n_tmpe += ntmp;
        nc = q;
    }
    const int nv = gx.S + ntmp;
    const double tc = now_sec();
    vkbm.t_route += tc - tb; vkbm.n_hot += np; vkbm.n_cold += nc;
    /* meta: [compartilhado: (b, sw)] [por vaga: (b, w)...] [frios somados: (b, 1)] */
    uint32_t *mt = vkbm.mt;
    const int off_sh = 0, off_v = 2 * B;
    for (int b = 0; b < B; b++) { mt[2 * b] = (uint32_t)b; memcpy(&mt[2 * b + 1], &vkbm.sw[b], 4); }
    int acc = 0;
    for (int v = 0; v < nv; v++) { vkbm.ofs[v] = acc; acc += vkbm.cnt[v]; }
    for (int v = 0; v < nv; v++) vkbm.cnt[v] = 0;
    for (int p = 0; p < np; p++) {
        const int v = vkbm.pv[p], k = vkbm.ofs[v] + vkbm.cnt[v]++;
        mt[off_v + 2 * k] = (uint32_t)vkbm.pb[p]; memcpy(&mt[off_v + 2 * k + 1], &vkbm.pw[p], 4);
    }
    const int off_c = off_v + 2 * np;
    vkbm.off_c = off_c; vkbm.cdone = 0;
    const float one = 1.0f;
    for (int b = 0; b < B; b++) { mt[off_c + 2 * b] = (uint32_t)b; memcpy(&mt[off_c + 2 * b + 1], &one, 4); }
    ok &= kvk_buf_write_q(vkbm.meta, 0, mt, (size_t)(off_c + 2 * B) * 4);
    if (vkm.sh && L->sg.gbuf) {
        if (prec) {
            ok &= vk_mm_p(&L->sg, vkbm.xf, vkb.hb, Hs, d, B, 0, 2);
            ok &= vk_mm_p(&L->sd, vkb.hb, vkbm.ys, d, Hs, B, 0, 1);
        } else {
            ok &= vk_mm(&L->sg, vkb.xh, vkb.hb, Hs, d, B, 0, 1);
            ok &= vk_mm(&L->sd, vkb.hb, vkbm.ys, d, Hs, B, 0, 0);
        }
        ok &= vk_moe_scat(vkbm.ys, d, B, B, off_sh);
    }
    const uint32_t kg = gpu_kind(&L->eg[0]), kd = gpu_kind(&L->ed[0]);
    const size_t wg = gpu_wpr(&L->eg[0], d), wd = gpu_wpr(&L->ed[0], H);
    /* frios temporarios: leitura do shard numa thread, ao mesmo tempo que os quentes e os frios da CPU */
    int thr = 0;
#ifdef _WIN32
    HANDLE th = NULL;
#else
    pthread_t th;
#endif
    if (ntmp > 0 && vkbm.tstage && gx.pf && gx.pf[l]) {
        if (!vkbm.pf[l]) {
            char fn[1100];
            snprintf(fn, sizeof fn, "%s/L%03d.exps.bin", shards.dir, l);
            vkbm.pf[l] = fopen(fn, "rb");
            if (vkbm.pf[l]) setvbuf(vkbm.pf[l], NULL, _IONBF, 0);
        }
        if (vkbm.pf[l]) {
            vk_trd.l = l; vk_trd.n = ntmp; vk_trd.bg = gx.bg[l]; vk_trd.bd = gx.bd[l];
            for (int t = 0; t < ntmp; t++) vk_trd.done[t] = 0;
#ifdef _WIN32
            th = CreateThread(NULL, 0, vk_trd_thread, NULL, 0, NULL);
            thr = th != NULL;
#else
            thr = pthread_create(&th, NULL, vk_trd_thread, NULL) == 0;
#endif
        }
    }
    for (int v = 0; v < nv && ok; v++) {
        const int n = vkbm.cnt[v], mo = off_v + 2 * vkbm.ofs[v];
        if (!n) continue;
        KvkBuf sg = vk_sg[l], sd = vk_sd[l];
        uint32_t og = (uint32_t)((size_t)v * (gx.bg[l] / 4)), od = (uint32_t)((size_t)v * (gx.bd[l] / 4));
        if (v >= gx.S) {        /* vaga temporaria */
            const int j = v - gx.S;
            if (j == 0) {
                if (!kvk_flush()) die("falha na execucao em Vulkan (MoE do lote)");
                if (thr) vk_moe_cold_cpu(m, L, l, B, &nc);     /* frios da CPU enquanto a thread le e a GPU roda os quentes */
            }
            const double t0 = now_sec();
            if (thr) {
                while (!vk_trd.done[j]) CPU_RELAX();
                if (vk_trd.done[j] > 0) {
                    const uint8_t *src = vkbm.tstage + (size_t)j * (vkbm.tgb + vkbm.tdb);
                    if (!kvk_buf_write_q(vkbm.tg, (size_t)j * vkbm.tgb, src, gx.bg[l])
                        || !kvk_buf_write_q(vkbm.td, (size_t)j * vkbm.tdb, src + vkbm.tgb, gx.bd[l])) die("falha ao subir especialista (Vulkan, lote)");
                } else {            /* falhou a leitura: espera a thread acabar e sobe pelo caminho comum */
#ifdef _WIN32
                    WaitForSingleObject(th, INFINITE); CloseHandle(th);
#else
                    pthread_join(th, NULL);
#endif
                    thr = 0;
                    vk_moe_tmp_load(m, L, l, vkbm.tmpe[j], j);
                }
            } else vk_moe_tmp_load(m, L, l, vkbm.tmpe[j], j);
            vkbm.t_tmp += now_sec() - t0;
            sg = vkbm.tg; sd = vkbm.td;
            og = (uint32_t)((size_t)j * (vkbm.tgb / 4)); od = (uint32_t)((size_t)j * (vkbm.tdb / 4));
        }
        const int gp[4] = { d, B, n, mo };
        if (prec) {
            const KvkBuf gb[3] = { vkbm.xf, vkbm.xg, vkbm.meta };
            ok &= kvk_dispatch(vkbm.gath32, gb, 3, gp, sizeof gp, (uint32_t)(((size_t)d * n + 255) / 256), 1, 1);
            ok &= vk_mm_pw(kg, sg, og, wg, vkbm.xg, vkb.hb, H, d, n, 0, 2);
            ok &= vk_mm_pw(kd, sd, od, wd, vkb.hb, vkbm.ys, d, H, n, 0, 1);
        } else {
            const KvkBuf gb[3] = { vkb.xh, vkbm.xg, vkbm.meta };
            ok &= kvk_dispatch(vkbm.gath, gb, 3, gp, sizeof gp, (uint32_t)(((size_t)d * n + 255) / 256), 1, 1);
            ok &= vk_mm_raw(kg, sg, og, wg, vkbm.xg, vkb.hb, H, d, n, 0, 1);
            ok &= vk_mm_raw(kd, sd, od, wd, vkb.hb, vkbm.ys, d, H, n, 0, 0);
        }
        ok &= vk_moe_scat(vkbm.ys, d, B, n, mo);
    }
    if (thr) {
#ifdef _WIN32
        WaitForSingleObject(th, INFINITE); CloseHandle(th);
#else
        pthread_join(th, NULL);
#endif
    }
    if (!ok) die("falha na execucao em Vulkan (MoE do lote)");
    if (nc > 0) {
        kvk_flush();
        vk_moe_cold_cpu(m, L, l, B, &nc);
    }
    if (vkbm.cdone) {
        vkbm.cdone = 0;
        if (!kvk_buf_write_q(vkbm.yc, 0, vkbm.xc, (size_t)d * B * sizeof(float)) || !vk_moe_scat(vkbm.yc, d, B, B, vkbm.off_c))
            die("falha na execucao em Vulkan (MoE do lote, CPU)");
    }
}

static int gx_host_layer(int l) { return gpu.vk ? (vk_sg && vk_sg[l].b) : (gx.sg && gx.sg[l]); }

/* ---------- cache de especialistas em Vulkan no modo hibrido (camadas na CPU: MLA) ----------
   Vagas so nas camadas MoE (as densas iniciais e os compartilhados ja subiram como matrizes normais).
   Por camada: a entrada sobe, os quentes rodam em dois lancamentos (gate/up+SiLU e down, tabela em vkm.meta),
   o compartilhado na GPU, os frios na CPU ao mesmo tempo; uma leitura traz as saidas e a CPU soma pesado. */
static int vk_host_moe_setup(Model *m, size_t S, size_t per, size_t work) {
    const Config *c = &m->cfg;
    const int NL = c->n_layers, K = c->n_used;
    const size_t d = c->dim, H = c->hidden, Hs = (size_t)c->n_shared * H;
    int fail = 0;
    vk_sg = xalloc(NL, sizeof(KvkBuf)); vk_sd = xalloc(NL, sizeof(KvkBuf));
    for (int l = 0; l < NL && !fail; l++) {
        if (!m->layers[l].eg) continue;
        vk_sg[l] = kvk_buf_create(S * gx.bg[l], KVK_USAGE_RW);
        vk_sd[l] = kvk_buf_create(S * gx.bd[l], KVK_USAGE_RW);
        if (!vk_sg[l].b || !vk_sd[l].b) fail = 1;
    }
    vkm.hb = vk_wk(((size_t)K * H + 128) * 4, &fail); vkm.bt = vk_wk((size_t)(K + 1) * d * 4, &fail);
    vkm.meta = vk_wk((size_t)8 * (K + 1) * 4, &fail); vkm.hs = vk_wk((Hs + 128) * 4, &fail);
    if (!fail) {    /* reserva a memoria agora, fora do primeiro token */
        for (int l = 0; l < NL; l++) if (vk_sg[l].b) {
            void *z = calloc(1, gx.bg[l] > gx.bd[l] ? gx.bg[l] : gx.bd[l]);
            for (size_t v = 0; v < S && z && !fail; v++)
                if (!kvk_buf_write_q(vk_sg[l], v * gx.bg[l], z, gx.bg[l]) || !kvk_buf_write_q(vk_sd[l], v * gx.bd[l], z, gx.bd[l])) fail = 1;
            free(z);
        }
        if (!kvk_wait()) fail = 1;
    }
    if (fail) {
        fprintf(stderr, "Vulkan: sem memoria para o cache de especialistas (%s)\n", kvk_error());
        for (int l = 0; l < NL; l++) { if (vk_sg[l].b) kvk_buf_destroy(&vk_sg[l]); if (vk_sd[l].b) kvk_buf_destroy(&vk_sd[l]); }
        KvkBuf *all[4] = { &vkm.hb, &vkm.bt, &vkm.meta, &vkm.hs };
        for (int i = 0; i < 4; i++) if (all[i]->b) kvk_buf_destroy(all[i]);
        free(vk_sg); free(vk_sd); vk_sg = vk_sd = NULL;
        return 0;
    }
    gpu.used += S * per + work;
    gx.S = (int)S;
    gx.xn = xalloc(d, sizeof(float)); gx.hout = xalloc((size_t)(K + 1) * d, sizeof(float));
    moe_tables(m, S, per);
    return 1;
}

static void vk_host_moe_ffn(Model *m, const Layer *L, int l, float *out) {
    const Config *c = &m->cfg;
    const int d = c->dim, H = c->hidden, K = c->n_used, E = c->n_expert, Hs = c->n_shared * H;
    static const int zero[MOE_KMAX];
    int cpu_e[MOE_KMAX], cpu_j[MOE_KMAX], hit_j[MOE_KMAX], n_cpu = 0, R = 0;
    uint32_t meta[6 * MOE_KMAX];
    float *heat = gx.heat + (size_t)l * E;
    int *own = gx.owner + (size_t)l * gx.S;
    uint64_t *st = gx.stamp + (size_t)l * gx.S;
    const int has_sh = L->sg.q || L->sg.f;
    const int sh_gpu = has_sh && L->sg.gbuf && L->sd.gbuf && (L->sg.pair || L->su.gbuf);
    if (gx.loader) loader_install(m);
    int ok = kvk_buf_write_q(gpu.vxb, 0, m->xb, (size_t)d * sizeof(float));
    const uint64_t tick = ++gx.clock;
    moe_route(m, L, l);
    for (int e = 0; e < E; e++) heat[e] *= gx.decay;
    for (int j = 0; j < K; j++) heat[moe.sel[j]] += 1.0f;
    for (int j = 0; j < K; j++) {
        const int e = moe.sel[j];
        int v = gx.slot[(size_t)l * E + e].slot;
        if (v >= 0) gx.hits++;
        else {
            gx.miss++;
            int cand = -1;      /* vaga livre, ou o residente mais frio que nao e deste token */
            for (int i = 0; i < gx.S; i++) {
                if (own[i] < 0) { cand = i; break; }
                if (st[i] == tick) continue;
                if (cand < 0 || heat[own[i]] < heat[own[cand]]) cand = i;
            }
            if (gx.loader) {
                if (cand >= 0 && (own[cand] < 0 || heat[e] > heat[own[cand]] + gx.swap_margin)) loader_request(l, e, E);
            } else if (cand >= 0 && (own[cand] < 0 || !gx.cpu_cold || heat[e] > heat[own[cand]] + 0.5f)) {
                expert_load(m, l, e, cand); v = cand;
            }
        }
        if (v >= 0) {
            uint32_t *t = meta + 6 * R;
            st[v] = tick;
            t[0] = (uint32_t)((size_t)v * (gx.bg[l] / 4)); t[1] = 0; t[2] = (uint32_t)R;
            t[3] = (uint32_t)((size_t)v * (gx.bd[l] / 4)); t[4] = (uint32_t)R; t[5] = (uint32_t)R;
            hit_j[R++] = j;
        } else { cpu_e[n_cpu] = e; cpu_j[n_cpu++] = j; }
    }
    gprof.lay++; gprof.allhit += n_cpu == 0; m_nused = K;
    if (R > 0) {            /* quentes nas linhas 0..R-1 de bt */
        const uint32_t kg = gpu_kind(&L->eg[0]), kd = gpu_kind(&L->ed[0]);
        ok &= kvk_buf_write_q(vkm.meta, 0, meta, (size_t)6 * R * 4);
        ok &= vk_gmv_multi(kg, vk_sg[l], gpu.vxb, vkm.hb, vkm.meta, H, d, 1, R, 0, H, 0);
        ok &= vk_gmv_multi(kd, vk_sd[l], vkm.hb, vkm.bt, vkm.meta, d, H, 0, R, H, d, 3);
    }
    if (sh_gpu) {           /* compartilhado na linha R de bt */
        if (L->sg.pair) ok &= vk_gmv_ex(&L->sg, gpu.vxb, vkm.hs, gpu.vxb, Hs, d, 0, 1);
        else {
            ok &= vk_gmv_ex(&L->sg, gpu.vxb, vkm.hs, gpu.vxb, Hs, d, 0, 0);
            ok &= vk_gmv_ex(&L->su, gpu.vxb, vkm.hs, gpu.vxb, Hs, d, 2, 0);
        }
        ok &= vk_gmv_raw(gpu_kind(&L->sd), *(const KvkBuf *)L->sd.gbuf, vkm.hs, vkm.bt, gpu.vxb, d, Hs, 0, 0, L->sd.gs, L->sd.go, 0, R * d);
    }
    if (!ok || !kvk_flush()) die("falha na execucao em Vulkan (MoE)");
    if (n_cpu > 0) {    /* os frios, na CPU, enquanto a GPU trabalha */
        const double t0 = now_sec();
        moe_cpu_multi(m, L, cpu_e, zero, n_cpu, 1, m->xb, gx.cpu_out);
        gx.cpu_runs += n_cpu;
        gx.t_cpu += now_sec() - t0; gprof.t_cold += now_sec() - t0;
    }
    const int nr = R + sh_gpu;
    if (nr > 0 && !kvk_buf_read(vkm.bt, 0, gx.hout, (size_t)nr * d * sizeof(float))) die("falha na execucao em Vulkan (leitura MoE)");
    for (int i = 0; i < R; i++) {
        const float w = moe.w[hit_j[i]], *o = gx.hout + (size_t)i * d;
        for (int k = 0; k < d; k++) out[k] += w * o[k];
    }
    for (int i = 0; i < n_cpu; i++) {
        const float w = moe.w[cpu_j[i]], *o = gx.cpu_out + (size_t)i * d;
        for (int k = 0; k < d; k++) out[k] += w * o[k];
    }
    if (has_sh) {
        const float gw = shexp_gate_w(m, L);
        const float *so = sh_gpu ? gx.hout + (size_t)R * d : m->xb2;
        if (!sh_gpu) shexp_cpu(m, L);
        for (int k = 0; k < d; k++) out[k] += gw * so[k];
    }
}

/* gate/up e down das R faixas (vaga, token) descritas em gx.bmeta, sobre a entrada xin; saidas nas linhas 0..R-1 de gx.bt */
static void gx_run(const Layer *L, int l, int R, clh xin) {
    const Config *c = &gx.m->cfg;
    const KSet *k1 = &gpu.set[gpu.n_sets - 1];
    const int d = c->dim, H = c->hidden;
    const size_t rpH = ((size_t)H + 127) & ~(size_t)127, rpd = ((size_t)d + 127) & ~(size_t)127;
    {   /* gate e up das R faixas: bh[r*H + i] = silu(gate) * up */
        const uint32_t kind = gpu_kind(&L->eg[0]);
        clh k = kind == T_F32 ? k1->kp_f32 : kind == T_Q4_K ? k1->kp_q4k : kind == T_Q5_K ? k1->kp_q5k : kind == T_Q6_K ? k1->kp_q6k : k1->kp_q8;
        const size_t tp = rpH / k1->nr, threads = tp * R;
        const int nb = kind == T_F32 ? 1 : kind == T_Q8_0 ? d / 32 : d / 256;
        const int nseg = gpu_nseg(threads, nb, k1->nr * 2);
        ka_m(k, 0, gx.sg[l]); ka_m(k, 1, xin); ka_m(k, 2, gx.bh);
        ka_m(k, 3, xin); ka_m(k, 4, gpu.bsx ? gpu.bsx : xin);
        ka_i(k, 5, d); ka_i(k, 6, 8); ka_i(k, 7, H); ka_i(k, 8, nseg); ka_i(k, 9, 1); ka_i(k, 10, 0);
        ka_m(k, 11, gx.bmeta); ka_i(k, 12, (int)tp); ka_i(k, 13, d); ka_i(k, 14, H); ka_i(k, 15, 0);
        prof_elems = 2.0 * H * d * R;
        krun2(k, threads, (size_t)nseg);
        ka_i(k, 12, 0);
    }
    {   /* down das R faixas: bt[(b*K + j)*d + i] */
        const uint32_t kind = gpu_kind(&L->ed[0]);
        clh k = kind == T_F32 ? k1->k_f32 : kind == T_Q4_K ? k1->k_q4k : kind == T_Q5_K ? k1->k_q5k : kind == T_Q6_K ? k1->k_q6k : k1->k_q8;
        const size_t tp = rpd / k1->nr, threads = tp * R;
        const int nb = kind == T_F32 ? 1 : kind == T_Q8_0 ? H / 32 : H / 256;
        const int nseg = gpu_nseg(threads, nb, k1->nr);
        ka_m(k, 0, gx.sd[l]); ka_m(k, 1, gx.bh); ka_m(k, 2, gx.bt);
        ka_m(k, 3, gx.bh); ka_m(k, 4, gpu.bsx ? gpu.bsx : gx.bh);
        ka_i(k, 5, H); ka_i(k, 6, 0); ka_i(k, 7, d); ka_i(k, 8, nseg); ka_i(k, 9, 1); ka_i(k, 10, 0);
        ka_m(k, 11, gx.bmeta); ka_i(k, 12, (int)tp); ka_i(k, 13, H); ka_i(k, 14, d); ka_i(k, 15, 3);
        prof_elems = (double)H * d * R;
        krun2(k, threads, (size_t)nseg);
        ka_i(k, 12, 0);
    }
}

/* Feed-forward MoE da camada l com a atencao na GPU, para os B tokens do lote:
     x_b += soma_k w_k * down_k(silu(gate_k xb_b) * up_k xb_b)
   Cada especialista tem um "calor" (usos recentes, com decaimento). Os que estao na VRAM rodam
   na GPU: cada par (token, especialista) e uma faixa, e todas as faixas do lote saem em um
   lancamento. Um que esta fora roda na CPU, direto do arquivo mapeado, enquanto a GPU faz a
   parte dela; se estiver mais quente que o mais frio dos residentes, a thread carregadora o
   traz. Assim a VRAM converge para os especialistas do assunto sem ficar trocando.
   Os vetores do host usados em copias sem bloqueio (tabelas, pesos, entradas, saidas da CPU)
   so sao reescritos depois da proxima leitura bloqueante, quando a fila ja esvaziou. */
static void gpu_moe_ffn(Model *m, const KSet *ks, const Layer *L, int l) {
    const Config *c = &m->cfg;
    const int d = c->dim, H = c->hidden, K = c->n_used, E = c->n_expert, B = ks->nb;
    static int cpu_e[MOE_NMAX], cpu_x[MOE_NMAX], cpu_row[MOE_NMAX];
    float *heat = gx.heat + (size_t)l * E;
    int *own = gx.owner + (size_t)l * gx.S;
    uint64_t *st = gx.stamp + (size_t)l * gx.S;
    int R = 0, n_cpu = 0;
    /* a entrada desce para a CPU escolher os especialistas */
    if (B == 1) gerr |= gread(gpu.bxb, (size_t)d * sizeof(float), gx.xn);
    else {
        gerr |= gread(gpu.bxb, (size_t)d * B * sizeof(float), gpu.xT);
        for (int b = 0; b < B; b++) for (int i = 0; i < d; i++) gx.xn[(size_t)b * d + i] = gpu.xT[(size_t)i * B + b];
    }
    if (gerr) die("falha na execucao em GPU");
    if (gx.loader) loader_install(m);
    if (L->sg.gbuf) {   /* compartilhado (denso, sempre ativo): x += down(silu(gate) * up), enquanto a CPU roteia */
        const int Hs = c->n_shared * H;
        g_bsum(ks, gpu.bxb, d, &L->sg, &L->su, NULL);
        if (L->sg.pair) gmv_glu(ks, &L->sg, gpu.bxb, gpu.bhb, Hs, d);
        else { gmv(ks, &L->sg, gpu.bxb, gpu.bhb, NULL, Hs, d, 0); gmv(ks, &L->su, gpu.bxb, gpu.bhb, NULL, Hs, d, 2); }
        g_bsum(ks, gpu.bhb, Hs, &L->sd, NULL, NULL);
        gmv(ks, &L->sd, gpu.bhb, gpu.bx, NULL, d, Hs, 1);
        cl.Flush(gpu.queue);
    }
    const uint64_t tick = ++gx.clock;
    for (int b = 0; b < B; b++) {
        memcpy(m->xb, gx.xn + (size_t)b * d, (size_t)d * sizeof(float));
        moe_route(m, L, l);
        for (int e = 0; e < E; e++) heat[e] *= gx.decay;
        for (int j = 0; j < K; j++) heat[moe.sel[j]] += 1.0f;
        for (int j = 0; j < K; j++) {
            const int e = moe.sel[j], orow = b * K + j;      /* posicao (token, escolha) */
            int v = gx.slot[(size_t)l * E + e].slot;
            gx.w[orow] = moe.w[j];
            if (v >= 0) gx.hits++;
            else {
                gx.miss++;
                int cand = -1;      /* vaga livre, ou o residente mais frio que nao e deste lote */
                for (int i = 0; i < gx.S; i++) {
                    if (own[i] < 0) { cand = i; break; }
                    if (st[i] == tick) continue;
                    if (cand < 0 || heat[own[i]] < heat[own[cand]]) cand = i;
                }
                if (gx.loader) {    /* roda na CPU agora; se merecer, sobe num proximo token */
                    /* em lote (prompt) so ocupa vaga livre: nao expulsa os especialistas da geracao */
                    if (cand >= 0 && (own[cand] < 0 || (B == 1 && heat[e] > heat[own[cand]] + gx.swap_margin))) loader_request(l, e, E);
                } else if (cand >= 0 && (own[cand] < 0 || !gx.cpu_cold || heat[e] > heat[own[cand]] + 0.5f)) {
                    expert_load(m, l, e, cand); v = cand;
                }
            }
            if (v >= 0) {
                st[v] = tick;
                uint32_t *t = gx.meta + 6 * R;      /* gate/up: (vaga, token, faixa); down: (vaga, faixa, faixa) */
                t[0] = (uint32_t)((size_t)v * (gx.bg[l] / 4 / 32)); t[1] = (uint32_t)b; t[2] = (uint32_t)R;
                t[3] = (uint32_t)((size_t)v * (gx.bd[l] / 4 / 32)); t[4] = (uint32_t)R; t[5] = (uint32_t)R;
                gx.rowi[orow] = (uint32_t)R;        /* a saida fica na linha R de bt */
                R++;
            } else { cpu_e[n_cpu] = e; cpu_x[n_cpu] = b; cpu_row[n_cpu++] = orow; }
        }
    }
    /* saidas da CPU ocupam as linhas de bt logo depois das R da GPU; faixas, pesos e linhas
       sobem numa copia so */
    for (int j = 0; j < n_cpu; j++) gx.rowi[cpu_row[j]] = (uint32_t)(R + j);
    memcpy(gx.meta + 6 * R, gx.w, (size_t)B * K * sizeof(float));
    memcpy(gx.meta + 6 * R + B * K, gx.rowi, (size_t)B * K * sizeof(uint32_t));
    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.bmeta, 0, 0, ((size_t)6 * R + 2 * (size_t)B * K) * 4, gx.meta, 0, NULL, NULL);
    if (R > 0) {
        if (B > 1) gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.bxn, 0, 0, (size_t)B * d * sizeof(float), gx.xn, 0, NULL, NULL);
        gx_run(L, l, R, B > 1 ? gx.bxn : gpu.bxb);
    }
    if (n_cpu > 0) {    /* os frios, na CPU, enquanto a GPU trabalha */
        const double t0 = now_sec();
        cl.Flush(gpu.queue);
        moe_cpu_multi(m, L, cpu_e, cpu_x, n_cpu, B, gx.xn, gx.cpu_out);
        gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.bt, 0, (size_t)R * d * sizeof(float), (size_t)n_cpu * d * sizeof(float), gx.cpu_out, 0, NULL, NULL);
        gx.cpu_runs += n_cpu;
        gx.t_cpu += now_sec() - t0;
    }
    clh k = ks->k_moe_sum;
    ka_m(k, 0, gpu.bx); ka_m(k, 1, gx.bt); ka_m(k, 2, gx.bmeta); ka_i(k, 3, 6 * R); ka_i(k, 4, K); ka_i(k, 5, d); ka_i(k, 6, B);
    krun(k, (size_t)d * B);
}

/* Feed-forward MoE da camada l com as camadas na CPU (modo hibrido: MLA e DeltaNet), um token, entrada em m->xb.
   A entrada sobe uma vez; o compartilhado (inteiro na VRAM) e os especialistas quentes rodam na GPU enquanto
   a CPU roteia e roda os frios; as saidas descem numa leitura e a soma pesada (com o portao do compartilhado,
   se houver) e acumulada em out.
   Mesmo calor, vagas e thread carregadora do grafo completo. */
static void vk_host_moe_ffn(Model *m, const Layer *L, int l, float *out);
static void host_moe_ffn(Model *m, const Layer *L, int l, float *out) {
    const Config *c = &m->cfg;
    const KSet *k1 = &gpu.set[gpu.n_sets - 1];
    const int d = c->dim, K = c->n_used, E = c->n_expert, Hs = c->n_shared * c->hidden;
    static const int zero[MOE_KMAX];
    int cpu_e[MOE_KMAX], cpu_j[MOE_KMAX], hit_j[MOE_KMAX], n_cpu = 0, R = 0;
    float *heat = gx.heat + (size_t)l * E;
    int *own = gx.owner + (size_t)l * gx.S;
    uint64_t *st = gx.stamp + (size_t)l * gx.S;
    const int has_sh = L->sg.q || L->sg.f;
    if (gpu.vk) { vk_host_moe_ffn(m, L, l, out); return; }
    const int sh_gpu = has_sh && L->sg.gbuf && L->su.gbuf && L->sd.gbuf && gx.bsh;
    gerr |= cl.Finish(gpu.queue);       /* fila vazia: as vagas podem ser reescritas */
    if (gx.loader) loader_install(m);
    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gpu.xbuf, 0, 0, (size_t)d * sizeof(float), m->xb, 0, NULL, NULL);
    if (sh_gpu) {
        if (L->sg.pair) gmv_glu(k1, &L->sg, gpu.xbuf, gx.bsh, Hs, d);
        else { gmv(k1, &L->sg, gpu.xbuf, gx.bsh, NULL, Hs, d, 0); gmv(k1, &L->su, gpu.xbuf, gx.bsh, NULL, Hs, d, 2); }
        gmv(k1, &L->sd, gx.bsh, gx.bso, NULL, d, Hs, 0);
        cl.Flush(gpu.queue);
    }
    const uint64_t tick = ++gx.clock;
    moe_route(m, L, l);
    for (int e = 0; e < E; e++) heat[e] *= gx.decay;
    for (int j = 0; j < K; j++) heat[moe.sel[j]] += 1.0f;
    for (int j = 0; j < K; j++) {
        const int e = moe.sel[j];
        int v = gx.slot[(size_t)l * E + e].slot;
        if (v >= 0) gx.hits++;
        else {
            gx.miss++;
            int cand = -1;      /* vaga livre, ou o residente mais frio que nao e deste token */
            for (int i = 0; i < gx.S; i++) {
                if (own[i] < 0) { cand = i; break; }
                if (st[i] == tick) continue;
                if (cand < 0 || heat[own[i]] < heat[own[cand]]) cand = i;
            }
            if (gx.loader) {
                if (cand >= 0 && (own[cand] < 0 || heat[e] > heat[own[cand]] + gx.swap_margin)) loader_request(l, e, E);
            } else if (cand >= 0 && (own[cand] < 0 || !gx.cpu_cold || heat[e] > heat[own[cand]] + 0.5f)) {
                expert_load(m, l, e, cand); v = cand;
            }
        }
        if (v >= 0) {
            uint32_t *t = gx.meta + 6 * R;
            st[v] = tick;
            t[0] = (uint32_t)((size_t)v * (gx.bg[l] / 4 / 32)); t[1] = 0; t[2] = (uint32_t)R;
            t[3] = (uint32_t)((size_t)v * (gx.bd[l] / 4 / 32)); t[4] = (uint32_t)R; t[5] = (uint32_t)R;
            hit_j[R++] = j;
        } else { cpu_e[n_cpu] = e; cpu_j[n_cpu++] = j; }
    }
    if (R > 0) {
        gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.bmeta, 0, 0, (size_t)6 * R * 4, gx.meta, 0, NULL, NULL);
        gx_run(L, l, R, gpu.xbuf);
        cl.Flush(gpu.queue);
    }
    if (n_cpu > 0) {    /* os frios, na CPU, enquanto a GPU trabalha */
        const double t0 = now_sec();
        moe_cpu_multi(m, L, cpu_e, zero, n_cpu, 1, m->xb, gx.cpu_out);
        gx.cpu_runs += n_cpu;
        gx.t_cpu += now_sec() - t0;
    }
    if (sh_gpu) gerr |= cl.EnqueueReadBuffer(gpu.queue, gx.bso, R == 0, 0, (size_t)d * sizeof(float), m->xb2, 0, NULL, NULL);
    if (R > 0) gerr |= gread(gx.bt, (size_t)R * d * sizeof(float), gx.hout);
    if (gerr) die("falha na execucao em GPU");
    for (int i = 0; i < R; i++) {
        const float w = moe.w[hit_j[i]], *o = gx.hout + (size_t)i * d;
        for (int k = 0; k < d; k++) out[k] += w * o[k];
    }
    for (int i = 0; i < n_cpu; i++) {
        const float w = moe.w[cpu_j[i]], *o = gx.cpu_out + (size_t)i * d;
        for (int k = 0; k < d; k++) out[k] += w * o[k];
    }
    if (has_sh) {
        const float gw = shexp_gate_w(m, L);
        if (!sh_gpu) shexp_cpu(m, L);
        for (int k = 0; k < d; k++) out[k] += gw * m->xb2[k];
    }
}

/* processa ks->nb tokens de uma vez (posicoes pos0, pos0+1, ...); devolve os logits do ultimo */
static float *gpu_forward_n(Model *m, const KSet *ks, const int *toks, int pos0, int want_logits) {
    const Config *c = &m->cfg;
    const int B = ks->nb, d = c->dim, hd = c->hd, H = c->hidden, ctx = c->ctx, nh = c->n_heads;
    const int kvd = c->n_kv * hd, group = nh / c->n_kv, tmax = pos0 + B, qd = nh * hd;
    const float scale = 1.0f / sqrtf((float)hd);
    const KSet *k1 = &gpu.set[gpu.n_sets - 1];

    for (int b = 0; b < B; b++) {       /* embeddings transpostos: xT[coluna*B + token] */
        embed_row(m, m->x, toks[b]);
        for (int i = 0; i < d; i++) gpu.xT[(size_t)i * B + b] = m->x[i];
    }
    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gpu.bx, 1, 0, (size_t)d * B * sizeof(float), gpu.xT, 0, NULL, NULL);

    for (int l = 0; l < gpu.n_gl; l++) {
        const Layer *L = &m->layers[l];
        const int base = l * ctx * kvd;
        clh k;

        /* --- atencao --- */
        g_rms(ks, gpu.bx, gpu.an[l], gpu.bxb, d, c->eps);
        g_bsum(ks, gpu.bxb, d, &L->wq, &L->wk, &L->wv);
        const uint32_t kq = gpu_kind(&L->wq), kk = gpu_kind(&L->wk), kv = gpu_kind(&L->wv);
        if (ks->k_mv3 && kq >= T_Q4_K && kk >= T_Q4_K && kv >= T_Q4_K && d % 256 == 0) {
            /* q, k e v num lancamento so */
            const size_t rpq = ((size_t)qd + 127) & ~(size_t)127, rpk = ((size_t)kvd + 127) & ~(size_t)127;
            const int nseg = gpu_nseg(rpq + 2 * rpk, d / 256, 1);
            k = ks->k_mv3;
            ka_m(k, 0, L->wq.gbuf); ka_m(k, 1, L->wk.gbuf); ka_m(k, 2, L->wv.gbuf); ka_m(k, 3, gpu.bxb);
            ka_m(k, 4, c->qk_norm ? gx.bqn : gpu.bq); ka_m(k, 5, c->qk_norm ? gx.bkn : gpu.bk); ka_m(k, 6, gpu.bv);
            ka_i(k, 7, (int)kq); ka_i(k, 8, (int)kk); ka_i(k, 9, (int)kv);
            ka_i(k, 10, (int)rpq); ka_i(k, 11, (int)rpk);
            ka_i(k, 12, qd); ka_i(k, 13, kvd); ka_i(k, 14, kvd); ka_i(k, 15, d); ka_i(k, 16, nseg);
            prof_elems = (double)(qd + 2 * kvd) * d;
            krun2(k, rpq + 2 * rpk, (size_t)nseg);
        } else {
            gmv(ks, &L->wq, gpu.bxb, c->qk_norm ? gx.bqn : gpu.bq, NULL, qd, d, 0);
            gmv(ks, &L->wk, gpu.bxb, c->qk_norm ? gx.bkn : gpu.bk, NULL, kvd, d, 0);
            gmv(ks, &L->wv, gpu.bxb, gpu.bv, NULL, kvd, d, 0);
        }
        /* a norma RMS de q e k (vetor inteiro ou por cabeca) e feita dentro de qkv_post, que le
           q e k crus de bqn/bkn; com bias e norma juntos a soma dos quadrados ignoraria o bias,
           mas nenhuma arquitetura suportada tem os dois */
        if (ks->k_att1f && hd <= 256 && hd % 2 == 0 && !getenv("KATANA_NOATTF")) {
            /* um token: bias/norma, RoPE, gravacao no cache KV e a atencao num lancamento so */
            k = ks->k_att1f;
            ka_m(k, 0, c->qk_norm ? gx.bqn : gpu.bq); ka_m(k, 1, c->qk_norm ? gx.bkn : gpu.bk); ka_m(k, 2, gpu.bv);
            ka_m(k, 3, gpu.lbq[l] ? gpu.lbq[l] : gpu.bq); ka_m(k, 4, gpu.lbk[l] ? gpu.lbk[l] : gpu.bk);
            ka_m(k, 5, gpu.lbv[l] ? gpu.lbv[l] : gpu.bv);
            ka_m(k, 6, gpu.bkc); ka_m(k, 7, gpu.bvc); ka_m(k, 8, gpu.batt); ka_m(k, 9, gpu.bxb);
            ka_m(k, 10, gpu.bfreq); ka_m(k, 11, c->qk_norm ? gx.qn[l] : gpu.bq); ka_m(k, 12, c->qk_norm ? gx.kn[l] : gpu.bq);
            ka_i(k, 13, base); ka_i(k, 14, pos0); ka_i(k, 15, ctx); ka_i(k, 16, hd); ka_i(k, 17, kvd); ka_i(k, 18, group);
            ka_i(k, 19, c->rope_neox); ka_i(k, 20, (gpu.lbq[l] ? 1 : 0) | (gpu.lbk[l] ? 2 : 0) | (gpu.lbv[l] ? 4 : 0));
            ka_i(k, 21, c->qk_norm); ka_i(k, 22, qd); ka_f(k, 23, scale); ka_f(k, 24, c->eps);
            krun_local(k, (size_t)nh * hd, (size_t)hd);
        } else {
        k = ks->k_qkv;      /* bias + RoPE em q (no lugar) e k (no cache), v copiado para o cache */
        ka_m(k, 0, gpu.bq); ka_m(k, 1, c->qk_norm ? gx.bkn : gpu.bk); ka_m(k, 2, gpu.bv);
        ka_m(k, 16, c->qk_norm ? gx.bqn : gpu.bq); ka_m(k, 17, c->qk_norm ? gx.qn[l] : gpu.bq);
        ka_m(k, 18, c->qk_norm ? gx.kn[l] : gpu.bq); ka_i(k, 19, c->qk_norm); ka_f(k, 20, c->eps);
        ka_m(k, 3, gpu.lbq[l] ? gpu.lbq[l] : gpu.bq); ka_m(k, 4, gpu.lbk[l] ? gpu.lbk[l] : gpu.bk);
        ka_m(k, 5, gpu.lbv[l] ? gpu.lbv[l] : gpu.bv);
        ka_m(k, 6, gpu.bkc); ka_m(k, 7, gpu.bvc); ka_m(k, 8, gpu.bfreq);
        ka_i(k, 9, base); ka_i(k, 10, pos0); ka_i(k, 11, hd); ka_i(k, 12, c->rope_neox);
        ka_i(k, 13, (gpu.lbq[l] ? 1 : 0) | (gpu.lbk[l] ? 2 : 0) | (gpu.lbv[l] ? 4 : 0));
        ka_i(k, 14, qd); ka_i(k, 15, kvd);
        krun(k, (size_t)B * (qd / 2 + kvd / 2 + kvd));

        if (ks->k_att1 && hd <= 256) {      /* atencao inteira num lancamento */
            k = ks->k_att1;
            ka_m(k, 0, gpu.bq); ka_m(k, 1, gpu.bkc); ka_m(k, 2, gpu.bvc); ka_m(k, 3, gpu.batt); ka_m(k, 4, gpu.bxb);
            ka_i(k, 5, base); ka_i(k, 6, pos0); ka_i(k, 7, ctx); ka_i(k, 8, hd); ka_i(k, 9, kvd); ka_i(k, 10, group);
            ka_f(k, 11, scale);
            krun_local(k, (size_t)nh * hd, (size_t)hd);
        } else {
            k = ks->k_att_scores;
            ka_m(k, 0, gpu.bq); ka_m(k, 1, gpu.bkc); ka_m(k, 2, gpu.batt);
            ka_i(k, 3, base); ka_i(k, 4, pos0); ka_i(k, 5, tmax); ka_i(k, 6, ctx); ka_i(k, 7, hd);
            ka_i(k, 8, kvd); ka_i(k, 9, group); ka_i(k, 10, nh); ka_f(k, 11, scale);
            krun(k, (size_t)B * nh * tmax);
            k = ks->k_att_softmax;
            ka_m(k, 0, gpu.batt); ka_i(k, 1, pos0); ka_i(k, 2, ctx); ka_i(k, 3, nh);
            krun(k, (size_t)B * nh);
            k = ks->k_att_mix;
            ka_m(k, 0, gpu.batt); ka_m(k, 1, gpu.bvc); ka_m(k, 2, gpu.bxb);
            ka_i(k, 3, base); ka_i(k, 4, pos0); ka_i(k, 5, ctx); ka_i(k, 6, hd); ka_i(k, 7, kvd);
            ka_i(k, 8, group); ka_i(k, 9, nh);
            krun(k, (size_t)B * qd);
        }
        }
        g_bsum(ks, gpu.bxb, qd, &L->wo, NULL, NULL);
        gmv(ks, &L->wo, gpu.bxb, gpu.bx, gpu.lbo[l], d, qd, 1);           /* x += Wo*att (+bias) */

        /* --- feed-forward SwiGLU --- */
        g_rms(ks, gpu.bx, gpu.fn[l], gpu.bxb, d, c->eps);
        if (L->eg) {
            gpu_moe_ffn(m, ks, L, l);
            continue;
        }
        const int Hl = c->n_expert ? c->hidden_dense : H;      /* camada densa inicial de um MoE: largura propria */
        g_bsum(ks, gpu.bxb, d, &L->w_gate, &L->w_up, NULL);
        if (L->w_gate.pair) gmv_glu(ks, &L->w_gate, gpu.bxb, gpu.bhb, Hl, d);    /* hb = silu(gate) * up */
        else {
            gmv(ks, &L->w_gate, gpu.bxb, gpu.bhb, NULL, Hl, d, 0);        /* hb = gate */
            gmv(ks, &L->w_up, gpu.bxb, gpu.bhb, NULL, Hl, d, 2);          /* hb = silu(hb) * up */
        }
        g_bsum(ks, gpu.bhb, Hl, &L->w_down, NULL, NULL);
        gmv(ks, &L->w_down, gpu.bhb, gpu.bx, NULL, d, Hl, 1);             /* x += down */
        { static int fl = -1; if (fl < 0) fl = getenv("KATANA_FLUSH") ? atoi(getenv("KATANA_FLUSH")) : 0; if (fl > 0 && l % fl == fl - 1) cl.Flush(gpu.queue); }
    }

    /* o estado desce uma vez e a CPU faz o que nao esta na VRAM: as camadas restantes e,
       se for o caso, a projecao final */
    if (gpu.n_gl < c->n_layers || (want_logits && !m->lm_head.gbuf)) {
        float *lg = NULL;
        gerr |= gread(gpu.bx, (size_t)d * B * sizeof(float), gpu.xT);
        if (gerr) die("falha na execucao em GPU");
        for (int b = 0; b < B; b++) {
            for (int i = 0; i < d; i++) m->x[i] = gpu.xT[(size_t)i * B + b];
            cpu_layers(m, gpu.n_gl, c->n_layers, pos0 + b);
            if (want_logits && b == B - 1) {
                if (m->lm_head.gbuf) {      /* o estado sobe de novo so para a norma final e a projecao */
                    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gpu.bx, 1, 0, (size_t)d * sizeof(float), m->x, 0, NULL, NULL);
                    g_rms(k1, gpu.bx, gpu.bon, gpu.bxb, d, c->eps);
                    gmv(k1, &m->lm_head, gpu.bxb, gpu.blog, NULL, c->vocab, d, 0);
                    gerr |= gread(gpu.blog, (size_t)c->vocab * sizeof(float), m->logits);
                    if (gerr) die("falha na execucao em GPU");
                    lg = m->logits;
                } else lg = cpu_head(m);
            }
        }
        return lg;
    }
    if (!want_logits) {     /* nada a ler: o trabalho fica na fila e a GPU segue em paralelo */
        if (gerr) die("falha na execucao em GPU");
        return NULL;
    }
    g_rms(ks, gpu.bx, gpu.bon, gpu.bxb, d, c->eps);
    if (gpu.want_all && B == 4 && gpu.blog4) {      /* logits das 4 posicoes, numa passada so */
        g_bsum(ks, gpu.bxb, d, &m->lm_head, NULL, NULL);
        gmv(ks, &m->lm_head, gpu.bxb, gpu.blog4, NULL, c->vocab, d, 0);
        gerr |= gread(gpu.blog4, (size_t)c->vocab * 4 * sizeof(float), gpu.logits_all);
        if (gerr) die("falha na execucao em GPU");
        return gpu.logits_all;
    }
    if (B > 1) {            /* so o ultimo token do lote precisa de logits */
        clh k = ks->k_gather;
        ka_m(k, 0, gpu.bxb); ka_m(k, 1, gpu.bq); ka_i(k, 2, B); ka_i(k, 3, B - 1);
        krun(k, (size_t)d);
    }
    gmv(k1, &m->lm_head, B > 1 ? gpu.bq : gpu.bxb, gpu.blog, NULL, c->vocab, d, 0);
    gerr |= gread(gpu.blog, (size_t)c->vocab * sizeof(float), m->logits);
    if (gerr) die("falha na execucao em GPU");
    return m->logits;
}

/* ================= forward (um token por vez, com KV cache) ================= */

/* Leitura antecipada: enquanto a CPU calcula a camada l, pede ao sistema as paginas da
   camada l+1 do arquivo mapeado (em outra thread, para nao esperar o disco). So liga quando
   o arquivo e grande em relacao a RAM (ou com KATANA_PREFETCH=1): ai as paginas de fato
   saem da memoria entre um token e outro. */
static int prefetch_on;

static void prefetch_setup(int64_t fsz) {
    uint64_t ram = 0;
#ifdef _WIN32
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) ram = ms.ullTotalPhys;
#else
    ram = (uint64_t)sysconf(_SC_PHYS_PAGES) * (uint64_t)sysconf(_SC_PAGE_SIZE);
#endif
    prefetch_on = getenv("KATANA_PREFETCH") ? atoi(getenv("KATANA_PREFETCH")) : (ram && (uint64_t)fsz > ram / 10 * 6);
}

#ifdef _WIN32
typedef struct { WIN32_MEMORY_RANGE_ENTRY r[7]; int n; } Prefetch;
static DWORD WINAPI prefetch_thread(LPVOID a) {
    Prefetch *pf = a;
    PrefetchVirtualMemory(GetCurrentProcess(), (ULONG_PTR)pf->n, pf->r, 0);
    free(pf);
    return 0;
}
#endif

static void layer_prefetch(const Model *m, int l) {
    const Config *c = &m->cfg;
    if (!prefetch_on || l >= c->n_layers) return;
    const Layer *L = &m->layers[l];
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd;
    const Mat *ms[7] = { &L->wq, &L->wk, &L->wv, &L->wo, &L->w_gate, &L->w_up, &L->w_down };
    const size_t rows[7] = { d, kvd, kvd, d, H, H, d };
#ifdef _WIN32
    Prefetch *pf = malloc(sizeof *pf);
    if (!pf) return;
    pf->n = 0;
    for (int i = 0; i < 7; i++) if (ms[i]->q) {
        pf->r[pf->n].VirtualAddress = (PVOID)ms[i]->q;
        pf->r[pf->n].NumberOfBytes = rows[i] * ms[i]->stride;
        pf->n++;
    }
    HANDLE h = pf->n ? CreateThread(NULL, 0, prefetch_thread, pf, 0, NULL) : NULL;
    if (h) CloseHandle(h); else free(pf);
#else
    const size_t pg = (size_t)sysconf(_SC_PAGE_SIZE);
    for (int i = 0; i < 7; i++) if (ms[i]->q) {
        const uintptr_t a = (uintptr_t)ms[i]->q & ~(uintptr_t)(pg - 1);
        madvise((void *)a, (uintptr_t)ms[i]->q + rows[i] * ms[i]->stride - a, MADV_WILLNEED);
    }
#endif
}

/* experimento de esparsidade por assunto (comando `sparsity`): soma |ativacao| de cada
   neuronio do feed-forward e, com mascara, zera os que ficaram fora do conjunto quente */
static struct { int collect; float *sum; uint8_t *mask; double mass_all, mass_kept; int oracle_keep; float *tmp; } sp;
static int cmp_float_desc(const void *a, const void *b);

/* MLA por cabeca, na forma absorvida: q_nope passa por k_b (vira vetor no espaco latente), a
   pontuacao e q_lat . c_kv + q_rope . k_rope, a media ponderada fica no latente e so no fim passa
   por v_b. O cache nunca e descomprimido. */
typedef struct { Model *m; const Layer *L; int l, pos; float scale; } MlaJob;
static void mla_job(void *cx, int h0, int h1) {
    const MlaJob *j = cx;
    const Config *c = &j->m->cfg;
    const int hd = c->hd, nr = c->rope_dim, nope = hd - nr, hv = c->hd_v, R = c->mla, W = R + nr;
    static __thread float *att, *qa, *lat; static __thread int cap;
    if (cap < c->ctx) {
        free(att); free(qa); free(lat);
        att = xalloc(c->ctx, sizeof(float)); qa = xalloc(R, sizeof(float)); lat = xalloc(R, sizeof(float));
        cap = c->ctx;
    }
    const float *kb = j->m->ckv + (size_t)j->l * c->ctx * W;
    for (int h = h0; h < h1; h++) {
        const float *qn = j->m->q + (size_t)h * hd, *qr = qn + nope;
        const float *wk = j->L->wkb + (size_t)h * R * nope;
        for (int r = 0; r < R; r++) qa[r] = dot(wk + (size_t)r * nope, qn, nope);
        for (int t = 0; t <= j->pos; t++) {
            const float *row = kb + (size_t)t * W;
            att[t] = (dot(qa, row, R) + dot(qr, row + R, nr)) * j->scale;
        }
        softmax(att, j->pos + 1);
        memset(lat, 0, R * sizeof(float));
        for (int t = 0; t <= j->pos; t++) {
            const float *row = kb + (size_t)t * W, a = att[t];
            for (int r = 0; r < R; r++) lat[r] += a * row[r];
        }
        float *out = j->m->xb + (size_t)h * hv;
        const float *wv = j->L->wvb + (size_t)h * hv * R;
        for (int i = 0; i < hv; i++) out[i] = dot(wv + (size_t)i * R, lat, R);
    }
}
static void mla_attn(Model *m, const Layer *L, int l, int pos) {
    const Config *c = &m->cfg;
    const int d = c->dim, nh = c->n_heads, hd = c->hd, nr = c->rope_dim, nope = hd - nr, R = c->mla, W = R + nr;
    const int step = c->rope_neox ? 1 : 2, gap = c->rope_neox ? nr / 2 : 1;
    rmsnorm(m->xb, m->x, L->attn_norm, d, c->eps);
    if (c->q_lora) {    /* nao testado: nenhum modelo pequeno com q comprimida foi baixado */
        matvec(m->hb2, &L->wq_a, m->xb, c->q_lora, d);
        rmsnorm(m->hb2, m->hb2, L->q_a_norm, c->q_lora, c->eps);
        matvec(m->q, &L->wq_b, m->hb2, nh * hd, c->q_lora);
    } else matvec(m->q, &L->wq, m->xb, nh * hd, d);
    float *row = m->ckv + ((size_t)l * c->ctx + pos) * W;
    matvec(row, &L->wkv_a, m->xb, W, d);
    rmsnorm(row, row, L->kv_a_norm, R, c->eps);
    for (int i = 0; i < nr / 2; i++) {
        const float ang = pos * m->rope_freq[i], cs = cosf(ang), sn = sinf(ang);
        for (int h = 0; h < nh; h++) {
            float *p = m->q + h * hd + nope + step * i, a = p[0], b = p[gap];
            p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs;
        }
        float *p = row + R + step * i, a = p[0], b = p[gap];
        p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs;
    }
    MlaJob job = { m, L, l, pos, c->kq_mul / sqrtf((float)hd) };
    pool_for(mla_job, &job, nh, 1);
    matvec(m->xb2, &L->wo, m->xb, d, nh * c->hd_v);
    for (int i = 0; i < d; i++) m->x[i] += m->xb2[i];
}

/* ---------- MLA (deepseek2) em Vulkan: prompt em lote ----------
   Residual dos B tokens na VRAM (vkb.x, layout transposto como no lote dos densos). Por camada: norma fp32 e
   produtos q e kv_a no mm preciso (alta + resto); uma leitura traz q e o latente; a CPU faz norma do latente,
   RoPE, grava o cache (m->ckv, o mesmo do token a token) e a atencao absorvida em lote (tokens x cabecas no
   pool); a saida sobe e o produto da saida soma no residual. Feed-forward: denso (par gate/up proprio do lote)
   ou vk_moe_n (roteador na GPU, quentes nas vagas, frios em vagas temporarias ou na CPU). Tudo em fp32 (o
   DeepSeek-V2-Lite e sensivel). KATANA_VK_MLA=0 desliga (prompt token a token). */
static int vk_mla_build(Model *m) {
    const Config *c = &m->cfg;
    const int NL = c->n_layers, E = c->n_expert, K = c->n_used;
    const size_t d = c->dim, H = c->hidden, Hd = c->hidden_dense, nh = c->n_heads, hd = c->hd, hv = c->hd_v;
    const size_t W = (size_t)c->mla + c->rope_dim, qd = nh * hd, od = nh * hv;
    memset(&vkml, 0, sizeof vkml);
    if (getenv("KATANA_VK_MLA") && atoi(getenv("KATANA_VK_MLA")) == 0) return 0;
    if (!vkg.coop || c->q_lora || !E || K > 16 || E % 32 || d % 64 || qd % 64 || od % 64 || W % 64 || H % 64) return 0;
    /* formatos: tudo o que o lote multiplica precisa estar na VRAM, sem divisao, num tipo do mm.comp */
#define MOK(M) ((M).gbuf && gpu_kind(&(M)) != T_Q2_K && gpu_has(&(M)))
    for (int l = 0; l < NL; l++) {
        const Layer *L = &m->layers[l];
        if (!MOK(L->wq) || L->wq.gs != 1 || !MOK(L->wkv_a) || L->wkv_a.gs != 1 || !MOK(L->wo) || L->wo.gs != 1) return 0;
        if (L->eg) {
            if (!glu_pair(&L->eg[0], &L->eu[0]) || !gpu_has(&L->eg[0]) || !gpu_has(&L->ed[0])) return 0;
            if (gpu_kind(&L->eg[0]) == T_Q2_K || gpu_kind(&L->ed[0]) == T_Q2_K) return 0;
            if ((L->sg.q || L->sg.f) && (!MOK(L->sg) || !L->sg.pair || !MOK(L->sd) || L->sd.gs != 1)) return 0;
        } else {
            if (!glu_pair(&L->w_gate, &L->w_up) || !gpu_has(&L->w_gate) || gpu_kind(&L->w_gate) == T_Q2_K) return 0;
            if (!MOK(L->w_down) || L->w_down.gs != 1 || Hd % 64) return 0;
        }
    }
#undef MOK
    size_t P = getenv("KATANA_VK_PASS") ? (size_t)atoi(getenv("KATANA_VK_PASS")) : VK_PASS_DEF;
    if (P < 2) return 0;
    if (P > 1024) P = 1024;
    int fail = 0;
    const size_t used0 = gpu.used;
    memset(&vkb, 0, sizeof vkb);
    memset(&vkbm, 0, sizeof vkbm);
    const size_t Hs = (size_t)c->n_shared * H;
    size_t hbw = H > Hd ? H : Hd;
    if (Hs > hbw) hbw = Hs;
    vkb.x = vk_wk(d * P * 4, &fail); vkb.q = vk_wk(qd * P * 4, &fail); vkb.k = vk_wk(W * P * 4, &fail);
    vkb.q2 = vk_wk(od * P * 4, &fail); vkb.hb = vk_wk(hbw * P * 4, &fail);
    vkbm.xf = vk_wk(d * P * 4, &fail); vkbm.xg = vk_wk(d * P * 4, &fail);
    vkbm.ys = vk_wk(d * P * 4, &fail); vkbm.yc = vk_wk(d * P * 4, &fail);
    vkbm.lg = vk_wk(((size_t)E * P + 128) * 4, &fail); vkbm.meta = vk_wk(2 * P * (K + 2) * 4, &fail);
    vkbm.rms32 = kvk_pipe_create(rms_wg32_spv, sizeof rms_wg32_spv, 3, 12, NULL, NULL, 0);
    vkbm.gath32 = kvk_pipe_create(moe_gath32_spv, sizeof moe_gath32_spv, 3, 16, NULL, NULL, 0);
    vkbm.scat = kvk_pipe_create(moe_scat_spv, sizeof moe_scat_spv, 3, 16, NULL, NULL, 0);
    if (!vkbm.rms32.p || !vkbm.gath32.p || !vkbm.scat.p) fail = 1;
    vkml.an = xalloc(NL, sizeof(KvkBuf)); vkml.fn = xalloc(NL, sizeof(KvkBuf)); vkml.dg = xalloc(NL, sizeof(KvkBuf));
    for (int l = 0; l < NL && !fail; l++) {
        Layer *L = &m->layers[l];
        vkml.an[l] = vk_vec(L->attn_norm, d, &fail); vkml.fn[l] = vk_vec(L->ffn_norm, d, &fail);
        if (!L->eg && !fail) {      /* camada densa: gate e up intercalados num buffer so do lote */
            Mat *two[2] = { &L->w_gate, &L->w_up };
            size_t bytes = 0;
            uint32_t *buf = gpu_build_buf(two, 2, Hd, d, &bytes);
            if (!buf) { fail = 1; break; }
            vkml.dg[l] = kvk_buf_create(bytes, KVK_USAGE_RW);
            if (!vkml.dg[l].b || !kvk_buf_write(vkml.dg[l], 0, buf, bytes)) fail = 1;
            else gpu.used += bytes;
            free(buf);
        }
    }
    /* roteador na GPU (float32, grupos de 32 linhas: o layout que o mm.comp le) */
    if (!fail && !(getenv("KATANA_VK_ROUTER") && atoi(getenv("KATANA_VK_ROUTER")) == 0)) {
        float *wr = xalloc((size_t)E * d, sizeof(float)), *row = xalloc(d, sizeof(float));
        vkm.rw = xalloc(NL, sizeof(KvkBuf));
        for (int l = 0; l < NL && !fail; l++) {
            if (!m->layers[l].eg) continue;
            for (int e = 0; e < E; e++) {
                mat_row(row, &m->layers[l].router, e, (int)d);
                for (size_t i = 0; i < d; i++) wr[(size_t)(e / 32) * d * 32 + i * 32 + e % 32] = row[i];
            }
            vkm.rw[l] = vk_vec(wr, (size_t)E * d, &fail);
        }
        free(row); free(wr);
        if (!fail) vkm.rt = 1;
    }
    vkbm.T = getenv("KATANA_VK_COLDT") ? atoi(getenv("KATANA_VK_COLDT")) : 16;
    vkbm.cmin = getenv("KATANA_VK_COLDMIN") ? atoi(getenv("KATANA_VK_COLDMIN")) : 2;
    if (vkbm.T < 0) vkbm.T = 0;
    if (vkbm.T > 64) vkbm.T = 64;
    if (vkbm.T && !fail) {
        for (int l = 0; l < NL; l++) {
            const Layer *L = &m->layers[l];
            if (!L->eg) continue;
            const size_t bg = 2 * gpu_mat_bytes(&L->eg[0], H, d), bd = gpu_mat_bytes(&L->ed[0], d, H);
            if (bg > vkbm.tgb) vkbm.tgb = bg;
            if (bd > vkbm.tdb) vkbm.tdb = bd;
        }
        vkbm.tg = vk_wk(vkbm.T * vkbm.tgb, &fail); vkbm.td = vk_wk(vkbm.T * vkbm.tdb, &fail);
        vkbm.stage = xalloc(vkbm.tgb + vkbm.tdb, 1);
        vkbm.pf = xalloc(NL, sizeof(FILE *));
        if (!getenv("KATANA_VK_TMPTHR") || atoi(getenv("KATANA_VK_TMPTHR"))) vkbm.tstage = xalloc(vkbm.T * (vkbm.tgb + vkbm.tdb), 1);
    }
    if (fail || !kvk_wait()) {
        fprintf(stderr, "Vulkan: sem memoria para o lote MLA (%s): prompt token a token\n", kvk_error());
        vkm.rt = 0;
        gpu.used = used0;       /* os buffers ficam (pequenos); so o orcamento das vagas volta */
        return 0;
    }
    vkbm.xn = xalloc(d * P, sizeof(float)); vkbm.lgT = xalloc((size_t)E * P, sizeof(float)); vkbm.xc = xalloc(d * P, sizeof(float));
    vkbm.outs = xalloc((size_t)GPU_BMAX * K * d, sizeof(float)); vkbm.sw = xalloc(P, sizeof(float));
    vkbm.mt = xalloc(2 * P * (K + 2), sizeof(uint32_t));
    vkbm.ccnt = xalloc(E, sizeof(int)); vkbm.tmpe = xalloc(64, sizeof(int));
    vkbm.cnt = xalloc(E + 66, sizeof(int)); vkbm.ofs = xalloc(E + 66, sizeof(int));
    vkbm.pv = xalloc(P * K, sizeof(int)); vkbm.pb = xalloc(P * K, sizeof(int)); vkbm.pw = xalloc(P * K, sizeof(float));
    vkbm.ce = xalloc(P * K, sizeof(int)); vkbm.cb = xalloc(P * K, sizeof(int)); vkbm.cw = xalloc(P * K, sizeof(float));
    vkbm.prec = 1;
    vkm.sh = c->n_shared > 0;
    vkg.fn = vkml.fn;           /* vk_moe_n le a norma do feed-forward daqui (vkg.on continua 0) */
    vkb.xT = xalloc(d * P, sizeof(float));
    vkml.Qh = xalloc(qd * P, sizeof(float)); vkml.Kh = xalloc(W * P, sizeof(float));
    vkml.Q = xalloc(qd * P, sizeof(float)); vkml.OT = xalloc(od * P, sizeof(float));
    vkml.P = (int)P; vkml.ok = 1;
    /* atencao na GPU: q por token, cache latente da camada (sobe a cada passada a partir de m->ckv), k_b e v_b da
       camada (float32, sobem por camada: nao ficam residentes para nao tirar vagas do cache de especialistas) */
    if (!(getenv("KATANA_VK_MLAATT") && atoi(getenv("KATANA_VK_MLAATT")) == 0) && vkg.sg32 && hd == 192 && c->rope_dim == 64
        && c->mla == 512 && hv == 128) {
        int f2 = 0;
        const size_t u1 = gpu.used;
        vkml.qb = vk_wk(qd * P * 4, &f2); vkml.cb = vk_wk((size_t)c->ctx * W * 4, &f2);
        vkml.wk = vk_wk(nh * 512 * 128 * 4, &f2); vkml.wv = vk_wk(nh * 128 * 512 * 4, &f2);
        vkml.att = kvk_pipe_create(mla_att_spv, sizeof mla_att_spv, 5, 16, NULL, NULL, 0);
        if (!f2 && vkml.att.p) vkml.gatt = 1;
        else gpu.used = u1;
    }
    fprintf(stderr, "Vulkan: prompt MLA em lote (ate %zu tokens por passada, %.0f MB)\n", P, (gpu.used - used0) / 1048576.0);
    return 1;
}

/* atencao MLA absorvida de B tokens (cache ja gravado ate pos0+B-1): um item por (bloco de MLA_TB tokens, cabeca),
   para cada linha de k_b e v_b ser lida uma vez por bloco; saida transposta OT[(h*hv + i)*B + t] para o produto
   da saida na GPU */
#define MLA_TB 8
typedef struct { Model *m; const Layer *L; int l, pos0, B, nb; float scale; } MlaNJob;
static void mla_n_job(void *cx, int u0, int u1) {
    const MlaNJob *j = cx;
    const Config *c = &j->m->cfg;
    const int nh = c->n_heads, hd = c->hd, nr = c->rope_dim, nope = hd - nr, hv = c->hd_v, R = c->mla, W = R + nr;
    static __thread float *att, *qa, *lat; static __thread int cap;
    if (cap < c->ctx) {
        free(att); free(qa); free(lat);
        att = xalloc(c->ctx, sizeof(float)); qa = xalloc((size_t)MLA_TB * R, sizeof(float)); lat = xalloc((size_t)MLA_TB * R, sizeof(float));
        cap = c->ctx;
    }
    const float *kb = j->m->ckv + (size_t)j->l * c->ctx * W;
    for (int u = u0; u < u1; u++) {
        const int h = u % nh, t0 = (u / nh) * MLA_TB, n = j->B - t0 < MLA_TB ? j->B - t0 : MLA_TB;
        const float *wk = j->L->wkb + (size_t)h * R * nope, *wv = j->L->wvb + (size_t)h * hv * R;
        for (int r = 0; r < R; r++)
            for (int t = 0; t < n; t++) qa[(size_t)t * R + r] = dot(wk + (size_t)r * nope, vkml.Q + (size_t)(t0 + t) * nh * hd + (size_t)h * hd, nope);
        for (int t = 0; t < n; t++) {
            const int pos = j->pos0 + t0 + t;
            const float *qr = vkml.Q + (size_t)(t0 + t) * nh * hd + (size_t)h * hd + nope, *q = qa + (size_t)t * R;
            float *lt = lat + (size_t)t * R;
            for (int s = 0; s <= pos; s++) {
                const float *row = kb + (size_t)s * W;
                att[s] = (dot(q, row, R) + dot(qr, row + R, nr)) * j->scale;
            }
            softmax(att, pos + 1);
            memset(lt, 0, R * sizeof(float));
            for (int s = 0; s <= pos; s++) {
                const float *row = kb + (size_t)s * W, a = att[s];
                for (int r = 0; r < R; r++) lt[r] += a * row[r];
            }
        }
        for (int i = 0; i < hv; i++) {
            float *o = vkml.OT + (size_t)(h * hv + i) * j->B + t0;
            for (int t = 0; t < n; t++) o[t] = dot(wv + (size_t)i * R, lat + (size_t)t * R, R);
        }
    }
}

static void vk_moe_n(Model *m, const Layer *L, int l, int B);
static float *vk_mla_n(Model *m, const int *toks, int B, int pos0, int want_logits) {
    const Config *c = &m->cfg;
    const int d = c->dim, nh = c->n_heads, hd = c->hd, nr = c->rope_dim, nope = hd - nr, R = c->mla, W = R + nr;
    const int qd = nh * hd, od = nh * c->hd_v, Hd = c->hidden_dense;
    const int step = c->rope_neox ? 1 : 2, gap = c->rope_neox ? nr / 2 : 1;
    if (B < 1 || B > vkml.P || pos0 < 0 || pos0 + B > c->ctx) die("lote fora do limite (Vulkan, MLA)");
    for (int t = 0; t < B; t++) {
        mat_row(m->x, &m->tok_emb, toks[t], d);
        for (int j = 0; j < d; j++) vkb.xT[(size_t)j * B + t] = m->x[j];
    }
    if (!kvk_buf_write_q(vkb.x, 0, vkb.xT, (size_t)d * B * sizeof(float))) die("falha na execucao em Vulkan (escrita do lote)");
    const struct { int n; float eps; int B; } rp = { d, c->eps, B };
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
        int ok = 1;
        {   const KvkBuf rb[3] = { vkb.x, vkml.an[l], vkbm.xf };
            ok &= kvk_dispatch(vkbm.rms32, rb, 3, &rp, sizeof rp, (uint32_t)((B + 7) / 8), 1, 1); }
        ok &= vk_mm_p(&L->wq, vkbm.xf, vkb.q, qd, d, B, 0, 1);
        ok &= vk_mm_p(&L->wkv_a, vkbm.xf, vkb.k, W, d, B, 0, 1);
        if (!ok) die("falha na execucao em Vulkan (MLA do lote)");
        const double t0 = now_sec();
        if (!kvk_buf_read(vkb.q, 0, vkml.Qh, (size_t)qd * B * 4) || !kvk_buf_read(vkb.k, 0, vkml.Kh, (size_t)W * B * 4))
            die("falha na execucao em Vulkan (leitura do lote, MLA)");
        const double t1 = now_sec();
        for (int t = 0; t < B; t++) {
            const int pos = pos0 + t;
            float *q = vkml.Q + (size_t)t * qd, *row = m->ckv + ((size_t)l * c->ctx + pos) * W;
            for (int i = 0; i < qd; i++) q[i] = vkml.Qh[(size_t)i * B + t];
            for (int i = 0; i < W; i++) row[i] = vkml.Kh[(size_t)i * B + t];
            rmsnorm(row, row, L->kv_a_norm, R, c->eps);
            for (int i = 0; i < nr / 2; i++) {
                const float ang = pos * m->rope_freq[i], cs = cosf(ang), sn = sinf(ang);
                for (int h = 0; h < nh; h++) {
                    float *p = q + h * hd + nope + step * i, a = p[0], b = p[gap];
                    p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs;
                }
                float *p = row + R + step * i, a = p[0], b = p[gap];
                p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs;
            }
        }
        const int nb = (B + MLA_TB - 1) / MLA_TB;
        const float scale = c->kq_mul / sqrtf((float)hd);
        if (vkml.gatt) {
            ok &= kvk_buf_write_q(vkml.qb, 0, vkml.Q, (size_t)qd * B * 4);
            ok &= kvk_buf_write_q(vkml.cb, 0, m->ckv + (size_t)l * c->ctx * W, (size_t)(pos0 + B) * W * 4);
            ok &= kvk_buf_write_q(vkml.wk, 0, L->wkb, (size_t)nh * R * nope * 4);
            ok &= kvk_buf_write_q(vkml.wv, 0, L->wvb, (size_t)nh * c->hd_v * R * 4);
            const KvkBuf ab[5] = { vkml.qb, vkml.cb, vkml.wk, vkml.wv, vkb.q2 };
            const struct { int B, pos0, nh; float scale; } ap = { B, pos0, nh, scale };
            ok &= kvk_dispatch(vkml.att, ab, 5, &ap, sizeof ap, (uint32_t)B, (uint32_t)nh, 1);
            vkml.t_rd += t1 - t0; vkml.t_attn += now_sec() - t1;
        } else {
            MlaNJob job = { m, L, l, pos0, B, nb, scale };
            pool_for(mla_n_job, &job, nb * nh, 1);
            vkml.t_rd += t1 - t0; vkml.t_attn += now_sec() - t1;
            ok &= kvk_buf_write_q(vkb.q2, 0, vkml.OT, (size_t)od * B * 4);
        }
        ok &= vk_mm_p(&L->wo, vkb.q2, vkb.x, d, od, B, 1, 1);
        if (L->eg) {
            if (!ok) die("falha na execucao em Vulkan (MLA do lote)");
            vk_moe_n(m, L, l, B);
            continue;
        }
        const KvkBuf rb[3] = { vkb.x, vkml.fn[l], vkbm.xf };
        ok &= kvk_dispatch(vkbm.rms32, rb, 3, &rp, sizeof rp, (uint32_t)((B + 7) / 8), 1, 1);
        ok &= vk_mm_pw(gpu_kind(&L->w_gate), vkml.dg[l], 0, gpu_wpr(&L->w_gate, d), vkbm.xf, vkb.hb, Hd, d, B, 0, 2);
        ok &= vk_mm_p(&L->w_down, vkb.hb, vkb.x, d, Hd, B, 1, 1);
        if (!ok) die("falha na execucao em Vulkan (MLA do lote)");
    }
    vkml.passes++;
    if (getenv("KATANA_VK_MOEPROF")) {
        fprintf(stderr, "[lote MLA] %d tokens: leitura q/kv %.3f, atencao na CPU %.3f | MoE: leitura %.3f, roteamento %.3f, frios na CPU %.3f, "
                "subida temporaria %.3f (arquivo %.3f) (pares GPU %ld, CPU %ld, temporarios %ld em %ld subidas)\n", B, vkml.t_rd, vkml.t_attn,
                vkbm.t_rd, vkbm.t_route, vkbm.t_cpu, vkbm.t_tmp, vkbm.t_frd, vkbm.n_hot, vkbm.n_cold, vkbm.n_tmp, vkbm.n_tmpe);
        vkml.t_rd = vkml.t_attn = 0;
        vkbm.t_rd = vkbm.t_route = vkbm.t_cpu = vkbm.t_tmp = vkbm.t_frd = 0; vkbm.n_hot = vkbm.n_cold = vkbm.n_tmp = vkbm.n_tmpe = 0;
    }
    if (!want_logits) { if (!kvk_flush()) die("falha na execucao em Vulkan (lote, MLA)"); return NULL; }
    if (!kvk_buf_read(vkb.x, 0, vkb.xT, (size_t)d * B * sizeof(float))) die("falha na execucao em Vulkan (leitura do lote, MLA)");
    for (int j = 0; j < d; j++) m->x[j] = vkb.xT[(size_t)j * B + B - 1];
    return cpu_head(m);
}

