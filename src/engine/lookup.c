/* ---------- decodificacao especulativa por consulta ao contexto ----------
 * Ao escrever codigo o modelo repete muito texto que acabou de ler. Depois de cada token, procura no
 * contexto (prompt + gerado) a ocorrencia anterior mais recente dos ultimos n tokens e propoe como
 * rascunho os k tokens que vieram depois dela. Uma passada em lote processa o token atual e o rascunho
 * de uma vez e guarda o estado oculto de todas as posicoes; os logits de cada posicao saem sob demanda.
 *
 * Aceitacao: quem chama continua amostrando como sempre (temperatura, top-k, top-p, gramatica) dos
 * logits que recebe. Se o token amostrado for o do rascunho, os logits seguintes ja estao prontos; se
 * nao for, o rascunho para ali. Com rascunho deterministico isto e exatamente a amostragem
 * especulativa: o token proposto x passa com probabilidade p(x) e, quando nao passa, o token sai de p
 * sem x renormalizada (a amostra de p condicionada a ser diferente de x). Em temperatura 0 a saida e a
 * do guloso, a menos da diferenca numerica entre o caminho em lote e o de um token (ver docs).
 *
 * Estado recorrente (DeltaNet): o cache KV se corrige sozinho (a posicao e reescrita), o estado
 * recorrente nao. Na CPU a passada guarda o estado depois de cada posicao (pontos de controle); no
 * desacordo volta ao ponto da ultima posicao certa e a passada nova comeca dali, sem refazer nada. No
 * grafo Vulkan ainda ha uma copia antes da passada e o refazer dos aceitos. No fim da geracao
 * (lk_finish) o estado volta ao ultimo token consumido.
 *
 * Politica (CPU): so rascunha quando o ganho esperado paga. Escolhe a ocorrencia de casamento mais longo,
 * estima a taxa de acerto por token pela faixa do comprimento do casamento (media movel) e escolhe o k
 * que maximiza tokens servidos esperados / custo medido da passada; sem ganho de 10%, nao rascunha.
 * KATANA_LOOKUP_ADAPT=0 rascunha sempre (k fixo). O guarda de seguranca (recuo exponencial) continua.
 *
 * KATANA_LOOKUP=1 liga (desligado por padrao); KATANA_LOOKUP_N (3), KATANA_LOOKUP_K (8),
 * KATANA_LOOKUP_ADAPT=0 desliga a adaptacao. Caminhos: qwen35moe na CPU em lote e no grafo Vulkan
 * completo; os demais (densos, gemma4, MLA, hibrido com camadas na GPU, atencao na CPU com o grafo,
 * imagens) seguem token a token. */
#define LK_MAXB 32
#define LK_NBUCK 5

static struct {
    Model *m;
    int n, kmax, k, adapt, be;          /* be: 1 CPU em lote, 2 grafo Vulkan */
    int *hist, hcap;                    /* tokens por posicao (prompt + gerado) */
    int toks[LK_MAXB], B, pos0, used;   /* passada pendente: B tokens a partir de pos0; used ja consumidos */
    float *X;                           /* CPU: estado oculto das B posicoes */
    float *ck; size_t ck_n; int ck_cap; /* CPU: estado recorrente depois de cada posicao da passada (ck_n floats por posicao) */
    char *snap; size_t snap_n;          /* copia do estado antes da passada (so o grafo Vulkan; a CPU usa ck) */
    KvkBuf *sring, *sst; int vkfail;    /* Vulkan: copia do estado recorrente */
    int nd;                             /* rascunhos da passada atual */
    double acc_t; int acc_n;            /* custo da passada atual e tokens servidos por ela */
    double plain, spec; long np;        /* medias moveis: segundos por token sem e com rascunho */
    double tb[LK_MAXB + 1]; int nb[LK_MAXB + 1];    /* custo medido da passada (sem a cabeca) por numero de posicoes */
    double hrow; int nh;                /* custo medido de uma linha de logits (cabeca) */
    double pa[LK_NBUCK]; int bucket;    /* taxa de acerto por token do rascunho, por faixa de comprimento do casamento */
    int cool, backoff;
    long proposed, accepted, passes, rejects, plain_toks, skipped;
} lk;

/* faixas do comprimento do casamento (tokens iguais antes do rascunho) e a taxa de acerto inicial de cada uma */
static const int lk_bl[LK_NBUCK] = { 0, 6, 12, 20, 40 };
static const double lk_p0[LK_NBUCK] = { 0.5, 0.75, 0.9, 0.96, 0.98 };

static void lk_init(Model *m) {
    const char *e = getenv("KATANA_LOOKUP");
    lk_on_ = e && *e && strcmp(e, "0") != 0;
    if (!lk_on_) { lk.m = m; return; }
    if (lk.m && lk.m != m) {            /* outro modelo: buffers novos */
        free(lk.X); free(lk.snap); free(lk.ck); lk.X = NULL; lk.snap = NULL; lk.snap_n = 0; lk.ck = NULL; lk.ck_cap = 0;
        lk.sring = lk.sst = NULL; lk.vkfail = 0;
    }
    lk.m = m;
    lk.n = getenv("KATANA_LOOKUP_N") ? atoi(getenv("KATANA_LOOKUP_N")) : 3;
    lk.kmax = getenv("KATANA_LOOKUP_K") ? atoi(getenv("KATANA_LOOKUP_K")) : 8;
    lk.adapt = !(getenv("KATANA_LOOKUP_ADAPT") && atoi(getenv("KATANA_LOOKUP_ADAPT")) == 0);
    if (lk.n < 1) lk.n = 1;
    if (lk.n > 8) lk.n = 8;
    if (lk.kmax < 1) lk.kmax = 1;
    if (lk.kmax > LK_MAXB - 1) lk.kmax = LK_MAXB - 1;
    lk.k = lk.kmax; lk.backoff = 16; lk.cool = 0; lk.B = 0; lk_pend_ = 0;
    lk.plain = lk.spec = 0; lk.np = 0; lk.nh = 0; lk.hrow = 0;
    memset(lk.nb, 0, sizeof lk.nb);
    for (int b = 0; b < LK_NBUCK; b++) lk.pa[b] = lk_p0[b];
    lk.hcap = m->cfg.ctx;
    lk.hist = realloc(lk.hist, sizeof(int) * (size_t)lk.hcap);
    if (!lk.hist) die("sem memoria");
    memset(lk.hist, 0xff, sizeof(int) * (size_t)lk.hcap);
}

/* qual caminho verifica em lote; 0 = nenhum (segue token a token) */
static int lk_backend(Model *m) {
    const Config *c = &m->cfg;
    if (!c->ssm_inner || !c->n_expert || c->q4e || c->g4 || m->ckv || vis_rp) return 0;
    if (vkg.on && !gpu.force_cpu) return vkq.on && vkb.ok && !vkq.cpuatt && !lk.vkfail ? 2 : 0;
    if (gpu.n_gl > 0 && !gpu.force_cpu) return 0;
    return qn_batch(m) ? 1 : 0;
}
static int lk_maxb(void) {
    int b = lk.be == 1 ? GPU_BMAX : vk_pass_cap();
    return b < LK_MAXB ? b : LK_MAXB;
}

/* anotacao de todo token processado (gancho em forward e forward_seq) */
static void lk_hook(Model *m, const int *toks, int n, int pos0) {
    if (lk_on_ < 0 || lk.m != m) lk_init(m);
    if (!lk_on_) return;
    if (lk_pend_) lk_finish(m);         /* alguem segue de outro ponto: o estado volta ao consumido */
    for (int i = 0; i < n; i++) if (pos0 + i >= 0 && pos0 + i < lk.hcap) lk.hist[pos0 + i] = toks[i];
}

/* rascunho: o token em pos fecha uma sequencia de n tokens. Entre as ocorrencias anteriores escolhe a de
   casamento mais longo (tokens iguais para tras, ate 64), e em empate a mais recente; devolve o rascunho
   (ate k tokens) e o comprimento do casamento em *ml */
static int lk_draft(int pos, int room, int *out, int *ml) {
    const int n = lk.n;
    int k = lk.kmax < room ? lk.kmax : room;
    *ml = 0;
    if (k < 1 || pos + 1 < n + 1 || pos >= lk.hcap) return 0;
    if (pos + 1 + k >= lk.hcap) k = lk.hcap - pos - 2;
    if (k < 1) return 0;
    const int *h = lk.hist, *tail = h + pos - n + 1;
    for (int i = 0; i < n; i++) if (tail[i] < 0) return 0;
    int bj = -1, bl = 0;
    for (int j = pos - n; j >= 0; j--) {
        if (h[j] != tail[0] || memcmp(h + j, tail, sizeof(int) * (size_t)n)) continue;
        if (h[j + n] < 0) continue;
        int l = n;                                  /* j..j+n-1 casa com o final; estende para tras */
        while (l < 64 && j - (l - n + 1) >= 0 && h[j - (l - n + 1)] == h[pos - l] && h[pos - l] >= 0) l++;
        if (l > bl) { bl = l; bj = j; if (l >= 64) break; }
    }
    if (bj < 0) return 0;
    int nd = 0;
    while (nd < k && bj + n + nd <= pos && h[bj + n + nd] >= 0) { out[nd] = h[bj + n + nd]; nd++; }
    *ml = bl;
    return nd;
}

/* quanto vale a pena: tamanho do rascunho (<= nd) que maximiza tokens servidos por tempo, ou 0 (sem rascunho).
   Servidos esperados com taxa de acerto p por token: 1 + p + p^2 + ... + p^k; custo: passada de k+1 posicoes
   (medida, ou a razao medida de 0,65 token por posicao a mais enquanto nao ha medida) mais uma linha de logits
   por token servido e um pedaco da restauracao do estado nos desacordos */
static int lk_trace(void) { static int v = -1; if (v < 0) v = getenv("KATANA_LOOKUP_TRACE") != NULL; return v; }
static int lk_pick(int nd, int ml) {
    if (!lk.adapt) return nd;
    if (lk.np < 4 || nd < 1) return 0;
    int b = 0;
    while (b + 1 < LK_NBUCK && ml >= lk_bl[b + 1]) b++;
    lk.bucket = b;
    const double p = lk.pa[b], plain = lk.plain, hrow = lk.nh >= 2 ? lk.hrow : 0.15 * plain;
    double best = 1.10, E = 1.0, pw = 1.0;      /* ganho minimo de 10% para compensar o erro da estimativa */
    int bk = 0;
    for (int k = 1; k <= nd; k++) {
        pw *= p; E += pw;
        const double run = lk.nb[k + 1] >= 2 ? lk.tb[k + 1] : plain * (0.65 * (k + 1) + 0.2);
        const double T = run + hrow * E + (1.0 - pw) * 0.2 * plain;
        const double g = E * plain / T;
        if (g > best) { best = g; bk = k; }
    }
    if (lk_trace()) fprintf(stderr, "[lk: nd=%d ml=%d p=%.2f -> k=%d (plain %.0f ms, run(k+1) %.0f ms, row %.0f ms, cool %d)]\n", nd, ml, p, bk, 1e3 * plain, 1e3 * (lk.nb[bk + 1] >= 2 ? lk.tb[bk + 1] : 0), 1e3 * hrow, lk.cool);
    return bk;
}

/* copia do estado recorrente: dir 1 guarda, 0 volta (grafo Vulkan) */
static int lk_state(Model *m, int dir) {
    if (!lk.sring) {                    /* primeira vez: buffers da copia na VRAM, do tamanho dos do grafo */
        int fail = 0;
        lk.sring = xalloc(m->n_lin, sizeof(KvkBuf)); lk.sst = xalloc(m->n_lin, sizeof(KvkBuf));
        for (int li = 0; li < m->n_lin && !fail; li++) {
            lk.sring[li] = vk_wk(vkq.ring[li].size, &fail);
            if (!fail) lk.sst[li] = vk_wk(vkq.st[li].size, &fail);
        }
        if (fail) {
            fprintf(stderr, "[KATANA_LOOKUP: sem VRAM para a copia do estado recorrente; rascunho desligado]\n");
            lk.vkfail = 1;
            return 0;
        }
    }
    for (int li = 0; li < m->n_lin; li++) {
        const int ok = dir ? kvk_buf_copy(lk.sring[li], 0, vkq.ring[li], 0, vkq.ring[li].size) && kvk_buf_copy(lk.sst[li], 0, vkq.st[li], 0, vkq.st[li].size)
                           : kvk_buf_copy(vkq.ring[li], 0, lk.sring[li], 0, vkq.ring[li].size) && kvk_buf_copy(vkq.st[li], 0, lk.sst[li], 0, vkq.st[li].size);
        if (!ok) die("falha na execucao em Vulkan (copia do estado recorrente)");
    }
    return 1;
}

/* CPU: o estado recorrente volta ao que era depois da posicao r da passada (guardado durante a passada) */
static void lk_ck_back(Model *m, int r) {
    (void)m;
    memcpy(m->ssm, lk.ck + (size_t)r * lk.ck_n, lk.ck_n * sizeof(float));
}

/* passada em lote dos B tokens da passada pendente, sem logits */
static void lk_run(Model *m) {
    const int d = m->cfg.dim;
    const double t0 = now_sec();
    if (lk.be == 1) {
        if (!lk.X) lk.X = xalloc((size_t)GPU_BMAX * d, sizeof(float));
        lk.ck_n = (size_t)m->n_lin * dn_state_floats(&m->cfg);
        if (lk.ck_cap < lk.B - 1) {      /* de uma vez para o maior rascunho; as paginas so existem depois de usadas */
            const int cap = lk.kmax > lk.B - 1 ? lk.kmax : lk.B - 1;
            free(lk.ck); lk.ck = xalloc(lk.ck_n * (size_t)cap, sizeof(float)); lk.ck_cap = cap;
        }
        for (int t = 0; t < lk.B; t++) embed_row(m, lk.X + (size_t)t * d, lk.toks[t]);
        qn_ckpt = lk.ck; qn_ckpt_n = lk.ck_n;
        qn_layers_n(m, 0, m->cfg.n_layers, lk.pos0, lk.B, lk.X);
        qn_ckpt = NULL;
    } else vk_forward_n(m, lk.toks, lk.B, lk.pos0, 0);
    const double dt = now_sec() - t0;
    lk.tb[lk.B] = lk.nb[lk.B] ? 0.7 * lk.tb[lk.B] + 0.3 * dt : dt;
    lk.nb[lk.B]++;
    lk.passes++;
}

/* logits da posicao r da passada (preve o token seguinte a toks[r]) */
static float *lk_row(Model *m, int r) {
    const double t0 = now_sec();
    float *lg;
    if (lk.be == 1) {
        memcpy(m->x, lk.X + (size_t)r * m->cfg.dim, sizeof(float) * m->cfg.dim);
        lg = cpu_head(m);
    } else {
        const KvkBuf gb[2] = { vkb.x, vkg.x };
        const int gp[3] = { m->cfg.dim, lk.B, r };
        if (!kvk_dispatch(vkb.gat, gb, 2, gp, sizeof gp, (uint32_t)((m->cfg.dim + 127) / 128), 1, 1) || !vk_emit_head(m)
            || !kvk_buf_read(vkg.lg, 0, m->logits, (size_t)m->cfg.vocab * sizeof(float))) die("falha na execucao em Vulkan (cabeca do rascunho)");
        lg = m->logits;
    }
    const double dt = now_sec() - t0;
    lk.hrow = lk.nh ? 0.8 * lk.hrow + 0.2 * dt : dt; lk.nh++;
    return lg;
}

/* fecha a conta da passada: custo por token servido, taxa de acerto da faixa e recuo. rej: terminou num desacordo
   (um erro conta como observacao; um fim por mudanca de rumo nao) */
static void lk_account(int all_ok, int ok, int rej) {
    if (lk.acc_n > 0) {
        const double r = lk.acc_t / lk.acc_n;
        lk.spec = lk.spec > 0 ? 0.7 * lk.spec + 0.3 * r : r;
    }
    if (lk.adapt) {
        if (lk.nd > 0) {
            double *p = &lk.pa[lk.bucket];
            for (int i = 0; i < ok; i++) *p += 0.12 * (1.0 - *p);
            if (rej && ok < lk.nd) *p -= 0.12 * *p;
        }
        if (lk.np >= 4 && lk.spec > 0) {
            if (lk.spec > 1.1 * lk.plain) {         /* nao paga: um tempo sem rascunho, o dobro a cada vez */
                lk.cool = lk.backoff;
                lk.backoff = lk.backoff * 2 > 1024 ? 1024 : lk.backoff * 2;
                lk.spec = 0;                        /* a volta mede de novo */
            } else if (lk.spec < 0.9 * lk.plain) lk.backoff = 16;
        }
    }
    if (lk_trace() && lk.nd) fprintf(stderr, "[lk: passada: %d de %d aceitos%s | custo %.0f ms por token servido (plain %.0f) | p=%.2f]\n", ok, lk.nd, rej ? " (desacordo)" : "", lk.acc_n ? 1e3 * lk.acc_t / lk.acc_n : 0.0, 1e3 * lk.plain, lk.pa[lk.bucket]);
    lk.acc_t = 0; lk.acc_n = 0; lk.nd = 0; (void)all_ok;
}

/* comeca a passada com toks[0..pre] ja montados; devolve os logits da posicao pre */
static float *lk_start(Model *m, int pre, int nd, int snap) {
    const double t0 = now_sec();
    lk.B = pre + 1 + nd; lk.used = pre + 1; lk.nd = nd; lk.k = nd;
    lk.proposed += nd;
    if (snap && !lk_state(m, 1)) { lk.B = 0; lk_pend_ = 0; return NULL; }
    lk_run(m);
    float *lg = lk_row(m, pre);
    lk.acc_t += now_sec() - t0; lk.acc_n++;
    lk_pend_ = lk.used < lk.B;
    if (!lk_pend_) { lk_account(0, 0, 0); lk.B = 0; }
    return lg;
}

/* o estado volta ao ultimo token consumido (fim da geracao ou mudanca de rumo) */
static void lk_finish(Model *m) {
    if (!lk_pend_) return;
    lk_pend_ = 0;
    const int used = lk.used, pos0 = lk.pos0;
    lk.B = 0;
    const double t0 = now_sec();
    if (lk.be == 1) lk_ck_back(m, used - 1);        /* o estado depois da posicao used-1 ja foi guardado: sem refazer nada */
    else if (lk_state(m, 0)) forward_seq(m, lk.toks, used, pos0, 0);
    lk.acc_t += now_sec() - t0;
    lk_account(0, used - 1, 0);
}

static float *lk_plain(Model *m, int token, int pos) {
    const double t0 = now_sec();
    float *lg = forward(m, token, pos, 1);
    const double dt = now_sec() - t0;
    lk.plain = lk.np ? 0.8 * lk.plain + 0.2 * dt : dt;
    lk.np++; lk.plain_toks++;
    return lg;
}

/* troca de forward(m, token, pos, 1) no laco de geracao */
static float *lk_forward(Model *m, int token, int pos) {
    if (lk_on_ < 0 || lk.m != m) lk_init(m);
    if (!lk_on_) return forward(m, token, pos, 1);
    if (pos >= 0 && pos < lk.hcap) lk.hist[pos] = token;
    int dr[LK_MAXB], ml = 0;
    if (lk_pend_) {
        if (pos == lk.pos0 + lk.used && token == lk.toks[lk.used] && lk_backend(m) == lk.be) {
            const double t0 = now_sec();
            float *lg = lk_row(m, lk.used);
            lk.acc_t += now_sec() - t0; lk.acc_n++;
            lk.used++; lk.accepted++;
            if (lk.used == lk.B) { lk_pend_ = 0; lk.B = 0; lk_account(1, lk.nd, 0); }
            return lg;
        }
        if (pos != lk.pos0 + lk.used || lk_backend(m) != lk.be) { lk_finish(m); return lk_plain(m, token, pos); }
        /* desacordo */
        const int ok = lk.used - 1, pre = lk.be == 1 ? 0 : lk.used;
        lk.rejects++;
        const double t0 = now_sec();
        lk_pend_ = 0;
        if (lk.be == 1) { lk_ck_back(m, lk.used - 1); lk.pos0 = pos; }     /* CPU: volta ao estado depois da ultima posicao certa; nada a refazer */
        else lk_state(m, 0);                                               /* Vulkan: volta a copia; a passada nova refaz os aceitos */
        lk.acc_t += now_sec() - t0;
        lk_account(0, ok, 1);
        lk.toks[pre] = token;
        int nd = 0;
        if (!lk.cool) {
            nd = lk_draft(pos, lk_maxb() - pre - 1, dr, &ml);
            if (lk.be == 1) { nd = lk_pick(nd, ml); if (!nd) lk.skipped++; }
        }
        if (lk.be == 1 && !nd) return lk_plain(m, token, pos);
        memcpy(lk.toks + pre + 1, dr, sizeof(int) * (size_t)nd);
        float *lg = lk_start(m, pre, nd, 0);
        if (lg) return lg;
        return lk_plain(m, token, pos);      /* nao chega aqui: a copia ja existia */
    }
    lk.be = lk_backend(m);
    if (!lk.be || g_gr.on) return lk_plain(m, token, pos);
    if (lk.cool > 0) { lk.cool--; return lk_plain(m, token, pos); }
    if (lk.adapt && lk.np < 4) return lk_plain(m, token, pos);    /* primeiro mede o custo sem rascunho */
    int nd = lk_draft(pos, lk_maxb() - 1, dr, &ml);
    if (nd && lk.be == 1) { nd = lk_pick(nd, ml); if (!nd) lk.skipped++; }
    if (!nd) return lk_plain(m, token, pos);
    lk.toks[0] = token; lk.pos0 = pos;
    memcpy(lk.toks + 1, dr, sizeof(int) * (size_t)nd);
    float *lg = lk_start(m, 0, nd, lk.be == 2);
    return lg ? lg : lk_plain(m, token, pos);
}

static void lk_report(void) {
    if (lk_on_ <= 0) return;
    fprintf(stderr, "[lookup: %ld rascunhos propostos, %ld aceitos, %ld passadas de verificacao, %ld desacordos, %ld tokens sem rascunho, %ld rascunhos descartados pela politica | k=%d, %.2f ms/token sem rascunho, %.2f com]\n",
            lk.proposed, lk.accepted, lk.passes, lk.rejects, lk.plain_toks, lk.skipped, lk.k, 1e3 * lk.plain, 1e3 * lk.spec);
}

/* katana lookuptest <modelo> [n]: conferencia de correcao (nao e medida de velocidade). Gera n tokens em
 * guloso duas vezes a partir de um prompt repetitivo: token a token e com o rascunho ligado (sem adaptacao).
 * Compara os tokens, a maior diferenca dos logits e, depois de lk_finish, os logits de um token extra
 * (confere que o estado recorrente voltou ao ponto certo). KATANA_LOOKUP_FORCED=1: em vez do guloso, a
 * sequencia segue o proprio prompt (com desvios), para exercitar aceitacao e desacordo num modelo aleatorio. */
static int lookup_test(Model *m, int N) {
    const Config *c = &m->cfg;
    const int V = c->vocab;
    const char *txt = "int add(int a, int b) { return a + b; }\nint sub(int a, int b) { return a - b; }\n"
                      "int add(int a, int b) { return a + b; }\nint sub(int a, int b) { return a - b; }\nint add(";
    Ids ids = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    encode(&m->tk, txt, &ids);
    if (ids.n + N + 2 >= c->ctx) N = c->ctx - ids.n - 3;
    if (N < 1) { fprintf(stderr, "lookuptest: contexto pequeno\n"); return 1; }
    int *ta = xalloc(N, sizeof(int));
    float *la = xalloc((size_t)N * V, sizeof(float)), *ea = xalloc(V, sizeof(float));
    const int extra = ids.ids[1 % ids.n];
    const int forced = getenv("KATANA_LOOKUP_FORCED") && atoi(getenv("KATANA_LOOKUP_FORCED"));
    /* A: token a token */
    lk_on_ = 0;
    float *lg = forward_seq(m, ids.ids, ids.n, 0, 1);
    int pos = ids.n;
    for (int i = 0; i < N; i++) {
        memcpy(la + (size_t)i * V, lg, sizeof(float) * V);
        /* metade das vezes segue o texto repetido (rascunhos aceitos), a cada 11 tokens desvia (desacordos) */
        ta[i] = forced && i % 11 != 10 ? ids.ids[(i % (ids.n - 1)) + 1] : sample(lg, V, 0.0f);
        lg = forward(m, ta[i], pos++, 1);
    }
    memcpy(ea, forward(m, extra, pos, 1), sizeof(float) * V);
    /* B: com o rascunho; segue a sequencia de A para comparar posicao a posicao */
    setenv_("KATANA_LOOKUP", "1"); setenv_("KATANA_LOOKUP_ADAPT", "0");
    lk_on_ = -1; lk_init(m);
    lg = forward_seq(m, ids.ids, ids.n, 0, 1);
    pos = ids.n;
    int diff_tok = -1; double dmax = 0.0;
    for (int i = 0; i < N; i++) {
        for (int v = 0; v < V; v++) { const double e = fabs((double)lg[v] - la[(size_t)i * V + v]); if (e > dmax) dmax = e; }
        if (sample(lg, V, 0.0f) != sample(la + (size_t)i * V, V, 0.0f) && diff_tok < 0) diff_tok = i;
        lg = lk_forward(m, ta[i], pos++);
    }
    const int be = lk.be;
    lk_finish(m);
    lk_on_ = 0;
    double dext = 0.0;
    const float *eb = forward(m, extra, pos, 1);
    for (int v = 0; v < V; v++) { const double e = fabs((double)eb[v] - ea[v]); if (e > dext) dext = e; }
    lk_on_ = 1; lk_report(); lk_on_ = 0;
    printf("lookuptest: caminho %s, %d tokens, %ld rascunhos propostos, %ld aceitos, %ld desacordos\n",
           be == 1 ? "CPU em lote" : be == 2 ? "grafo Vulkan" : "nenhum (token a token)", N, lk.proposed, lk.accepted, lk.rejects);
    printf("lookuptest: primeira posicao com escolha gulosa diferente: %d (-1 = nenhum) | maior diferenca de logit %.3g | token extra depois do fim %.3g\n",
           diff_tok, dmax, dext);
    free(ta); free(la); free(ea); free(ids.ids);
    return diff_tok >= 0 || lk.proposed == 0;
}

/* katana spectime <modelo> <arquivo.txt> [rodadas]: custo de uma passada de B posicoes em relacao a de uma.
 * Processa o texto do arquivo (prefixo), depois, em rodadas alternadas, mede forward (B=1) e forward_seq de
 * B = 2, 3, 5, 9 tokens que continuam o texto (copia de trecho do proprio contexto, roteamento realista).
 * Mostra a mediana em ms e a razao ao custo de um token. E medida de velocidade: rodar com a maquina calma. */
static int cmp_dbl_(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static int spec_time(Model *m, const char *file, int R) {
    FILE *f = fopen(file, "rb");
    if (!f) { fprintf(stderr, "spectime: sem arquivo\n"); return 1; }
    char *txt = xalloc(1 << 20, 1);
    const size_t n = fread(txt, 1, (1 << 20) - 1, f);
    fclose(f); txt[n] = 0;
    Ids ids = {0};
    if (m->cfg.add_bos && m->cfg.bos >= 0) ids_push(&ids, m->cfg.bos);
    encode(&m->tk, txt, &ids);
    int Bs[] = { 1, 2, 3, 5, 9 }, NB = 5;
    if (getenv("KATANA_SPEC_B")) { Bs[0] = atoi(getenv("KATANA_SPEC_B")); NB = 1; }      /* so este B (para o perf) */
    const int base = ids.n / 2;                 /* prefixo; os tokens medidos vem de depois dele */
    const int ab = getenv("KATANA_SPEC_AB") != NULL;
    double *t = xalloc((size_t)NB * 2 * R, sizeof(double));
    forward_seq(m, ids.ids, base, 0, 0);
    qn_prof_on = getenv("KATANA_SPEC_PROF") != NULL;
    for (int r = -1; r < R; r++)
        for (int bi = 0; bi < NB; bi++) {
            const int B = Bs[(bi + (r < 0 ? 0 : r)) % NB], idx = (bi + (r < 0 ? 0 : r)) % NB;
            const int p0 = base + (r < 0 ? 0 : (r * 13) % 64);
            for (int g = 0; g < 2; g++) {       /* alternado: especialistas agrupados (g = 1) e por par (g = 0) na mesma carga */
                if (ab) moe_grouped = g ^ (r & 1);
                const double t0 = now_sec();
                if (B == 1) forward(m, ids.ids[p0], p0, 1);
                else forward_seq(m, ids.ids + p0, B, p0, 1);
                if (r >= 0) t[((size_t)idx * 2 + (ab ? moe_grouped : 0)) * R + r] = now_sec() - t0;
                if (!ab) break;
            }
        }
    double one = 0;
    if (qn_prof_on) printf("spectime: fases por passada (ms, soma das rodadas / (R+1) passadas): atencao+DeltaNet %.0f (entrada: projecoes+conv+recorrencia %.0f, dos quais conv+recorrencia+portao %.0f) | norma+roteador %.0f | especialistas %.0f | soma+compartilhado %.0f\n",
        1e3 * qn_prof[0] / (R + 1), 1e3 * qn_prof[4] / (R + 1), 1e3 * qn_prof[5] / (R + 1), 1e3 * qn_prof[1] / (R + 1), 1e3 * qn_prof[2] / (R + 1), 1e3 * qn_prof[3] / (R + 1));
    for (int g = (ab ? 0 : 0); g < (ab ? 2 : 1); g++) {
        for (int bi = 0; bi < NB; bi++) {
            double *v = t + ((size_t)bi * 2 + g) * R;
            qsort(v, R, sizeof(double), cmp_dbl_);
        }
        one = t[((size_t)0 * 2 + g) * R + R / 2];
        for (int bi = 0; bi < NB; bi++) {
            const double *v = t + ((size_t)bi * 2 + g) * R, med = v[R / 2];
            printf("spectime: %sB=%d  mediana %.1f ms  (%.2fx de um token, %.1f ms por posicao)  min %.1f\n", ab ? (g ? "agrupado    " : "por par     ") : "", Bs[bi], 1e3 * med, med / (one > 0 ? one : med), 1e3 * med / Bs[bi], 1e3 * v[0]);
        }
    }
    return 0;
}
