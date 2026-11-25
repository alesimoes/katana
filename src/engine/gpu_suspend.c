/* ---------- gpu_suspend / gpu_resume: o agente solta a VRAM e a recompoe ----------
 * Quando o proprio agente chama a ferramenta Render, o subprocesso de imagem precisa da placa inteira (4 GB): o processo
 * do agente segurava ~3,6 GB e a imagem caia na CPU. gpu_suspend destroi o dispositivo Vulkan (kvk_shutdown: tudo o que o
 * motor tinha na VRAM some, inclusive o contexto do driver) e devolve os estaticos de GPU ao estado de antes da carga
 * (copiado em gpu_pristine_save, logo antes de gpu_setup); os pesos continuam no mmap do GGUF. gpu_resume roda gpu_setup de
 * novo (o mesmo caminho da carga) e devolve o calor dos especialistas (heat_apply), que repovoa as vagas. O contexto
 * (cache KV e estado do DeltaNet) quem guarda e quem chama: pfx_save/pfx_load (tools/render.c), como o delegate.
 * Cobre so o qwen35moe no grafo Vulkan (o modelo de trabalho); o resto devolve 0 e o chamador segue como antes.
 * Sem Windows (a thread carregadora nao tem juncao la). KATANA_GPU_SUSPEND=0 desliga. */
#define GS_LIST(X) X(gpu) X(vkg) X(vkb) X(vkbm) X(vkq) X(vkm) X(vkml) X(gx) X(vkqd) X(vkg4) X(vk_pipe) X(vk_pipe2) X(vk_sg) X(vk_sd) \
    X(vk_moe_out) X(vkq_posn) X(vkq_pos1) X(vk_pos_live) X(m_nused) X(gprof)
#define GS_DECL(v) static unsigned char gs_pr_##v[sizeof(v)];
GS_LIST(GS_DECL)
static int gs_pr_ok, gs_down;
static float *gs_heat; static int gs_heat_nl, gs_heat_e;

static void gpu_pristine_save(void) {
    if (gpu.vk || gs_down) return;      /* ja ha GPU viva (outro modelo carregado depois): o instantaneo valido e o de antes dela */
#define GS_SAVE(v) memcpy(gs_pr_##v, &(v), sizeof(v));
    GS_LIST(GS_SAVE)
    gs_pr_ok = 1;
}

static int gpu_suspendable(const Model *m) {
#ifdef _WIN32
    (void)m; return 0;
#else
    const char *e = getenv("KATANA_GPU_SUSPEND");
    if (e && atoi(e) == 0) return 0;
    return gs_pr_ok && !gs_down && gpu.vk && vkq.on && vkg.on && m->cfg.ssm_inner && m->cfg.n_expert && !m->cfg.q4e;
#endif
}

static void gs_clear_mat(Mat *w) { w->gbuf = NULL; w->gs = 0; w->go = 0; w->pair = 0; }

/* 1 = a VRAM foi toda devolvida ao sistema (o modelo fica sem GPU ate gpu_resume); 0 = nada mudou */
static int gpu_suspend(Model *m) {
#ifdef _WIN32
    (void)m; return 0;
#else
    if (!gpu_suspendable(m)) return 0;
    const Config *c = &m->cfg;
    if (!kvk_wait()) return 0;
    if (gx.loader) {        /* a carregadora termina o que tem na fila e sai */
        const double t0 = now_sec();
        while (gx.head > 0 && !__atomic_load_n(&gx.rq[(gx.head - 1) % LQ].done, __ATOMIC_ACQUIRE)) { if (now_sec() - t0 > 20) return 0; usleep(500); }
        loader_quit = 1; pthread_join(loader_tid, NULL); loader_quit = 0;
    }
    free(gs_heat); gs_heat = NULL;
    if (gx.on && gx.heat) {
        const size_t n = (size_t)c->n_layers * c->n_expert;
        gs_heat = xalloc(n, sizeof(float)); memcpy(gs_heat, gx.heat, n * sizeof(float));
        gs_heat_nl = c->n_layers; gs_heat_e = c->n_expert;
    }
    kvk_shutdown();
#define GS_REST(v) memcpy(&(v), gs_pr_##v, sizeof(v));
    GS_LIST(GS_REST)
    for (int l = 0; l < c->n_layers; l++) {
        Layer *L = &m->layers[l];
        Mat *ms[] = { &L->wq, &L->wk, &L->wv, &L->wo, &L->w_gate, &L->w_up, &L->w_down, &L->router, &L->wkv_a, &L->wq_a, &L->wq_b, &L->sg, &L->su, &L->sd,
                      &L->wqkv, &L->wz, &L->walpha, &L->wbeta, &L->wssm_out };
        for (size_t i = 0; i < sizeof ms / sizeof *ms; i++) gs_clear_mat(ms[i]);
        for (int e = 0; e < c->n_expert; e++) { if (L->eg) gs_clear_mat(&L->eg[e]); if (L->eu) gs_clear_mat(&L->eu[e]); if (L->ed) gs_clear_mat(&L->ed[e]); }
    }
    gs_clear_mat(&m->lm_head); gs_clear_mat(&m->tok_emb);
    gs_down = 1;
    return 1;
#endif
}

/* recompoe a GPU (o mesmo gpu_setup da carga). Devolve 1 se o grafo Vulkan voltou; 0 = o motor ficou em modo hibrido/CPU
   (a placa nao tinha memoria): funciona, so mais lento, e o contexto precisa ser recalculado */
static int gpu_resume(Model *m) {
    if (!gs_down) return 1;
    gs_down = 0;
    gpu_setup(m);
    if (gs_heat) { if (gx.on) heat_apply(m, gs_heat, gs_heat_nl, gs_heat_e); free(gs_heat); gs_heat = NULL; }
    return vkq.on && vkg.on;
}
