/* ---------- perfil por modelo: as escolhas que `katana prepare` gravou ----------
 * <modelo>.gguf.ktn.prof, texto simples. chat/serve/agent/run leem e definem as variaveis
 * de ambiente correspondentes; uma variavel ja definida pelo usuario sempre vence. */
static void perfil_path(char *out, size_t n, const char *model_path) {
    snprintf(out, n, "%s.ktn.prof", model_path);
}

static long long file_size_ll(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long long n = ftell(f);
    fclose(f);
    return n;
}

static void putenv_dup(const char *s) { putenv(strdup(s)); }   /* a string precisa viver ate o fim do processo */

/* for_agent: nao mexe em KATANA_CTX (o agente usa um contexto maior para os resultados de ferramenta) */
static void perfil_apply(const char *model_path, int for_agent) {
    char fn[1200], line[256], backend[32] = "";
    long long origem = -1; int kv16 = -1, ctx = 0, noexp = -1; double expram = -1;
    perfil_path(fn, sizeof fn, model_path);
    FILE *f = fopen(fn, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "origem %lld", &origem) == 1) continue;
        if (sscanf(line, "backend %31s", backend) == 1) continue;
        if (sscanf(line, "kv16 %d", &kv16) == 1) continue;
        if (sscanf(line, "ctx %d", &ctx) == 1) continue;
        if (sscanf(line, "expram %lf", &expram) == 1) continue;
        sscanf(line, "noexpcache %d", &noexp);
    }
    fclose(f);
    if (origem >= 0 && origem != file_size_ll(model_path)) {
        fprintf(stderr, "Perfil: %s e de outra versao do modelo; ignorado (rode: katana prepare)\n", fn);
        return;
    }
    char buf[80];
    if (backend[0] && !getenv("KATANA_BACKEND")) { snprintf(buf, sizeof buf, "KATANA_BACKEND=%s", backend); putenv_dup(buf); }
    if (kv16 >= 0 && !getenv("KATANA_KV16")) { snprintf(buf, sizeof buf, "KATANA_KV16=%d", kv16); putenv_dup(buf); }
    if (!for_agent && ctx > 0 && !getenv("KATANA_CTX")) { snprintf(buf, sizeof buf, "KATANA_CTX=%d", ctx); putenv_dup(buf); }
    if (noexp > 0 && !getenv("KATANA_NOEXPCACHE")) putenv_dup("KATANA_NOEXPCACHE=1");
    /* arena de RAM para especialistas (katana prepare decidiu com margem de seguranca; ver prepare() acima);
       a variavel do usuario, se setada manualmente, sempre vence */
    if (expram > 0 && !getenv("KATANA_EXPERT_RAM")) { snprintf(buf, sizeof buf, "KATANA_EXPERT_RAM=%.3f", expram); putenv_dup(buf); }
    if (expram > 0) fprintf(stderr, "Perfil: %s (backend=%s kv16=%d ctx=%d expram=%.3f)\n", fn, backend[0] ? backend : "-", kv16, ctx, expram);
    else fprintf(stderr, "Perfil: %s (backend=%s kv16=%d ctx=%d)\n", fn, backend[0] ? backend : "-", kv16, ctx);
}

/* RAM util AGORA nesta maquina (bytes), com margem de seguranca tirada: ram_avail()/ram_total()
   (cpu_kernels.c), nao um valor fixo de outra maquina. Margem (pedido do dono, 2026-11-28: "nao
   faz sentido usar toda a memoria que a pessoa tem"): pelo menos 20% da RAM livre ou 2 GB, o que
   for maior, fica reservado para o sistema e outros programas. Usada por recomendar_quant() e
   pela decisao de arena/bundle em prepare(), e tambem por "katana quantize" (main.c) sem modelo
   carregado. ram_av_out e ram_tot_out, quando nao NULL, recebem as medidas brutas (para imprimir). */
static uint64_t ram_usable_now(uint64_t *ram_av_out, uint64_t *ram_tot_out) {
    const uint64_t ram_av = ram_avail(), ram_tot = ram_total();
    const uint64_t margin_min = (uint64_t)2 << 30;
    const uint64_t margin = ram_av ? ram_av / 5 : (ram_tot ? ram_tot / 5 : 0);
    const uint64_t margin_use = margin > margin_min ? margin : margin_min;
    if (ram_av_out) *ram_av_out = ram_av;
    if (ram_tot_out) *ram_tot_out = ram_tot;
    if (ram_av > margin_use) return ram_av - margin_use;
    if (!ram_av && ram_tot > margin_use) return ram_tot - margin_use;
    return 0;
}

/* ---------- prepare: inspeciona modelo e maquina, decide o plano, executa o seguro, grava o perfil ----------
 * Sozinho: le a arquitetura e o tamanho do modelo e os recursos da maquina (ja medidos na carga),
 * imprime um plano em portugues, roda o que e seguro (pack, tune, calibracao do calor dos especialistas)
 * respeitando a GPU ocupada, e grava um perfil por modelo que chat/serve/agent passam a usar. */
/* bits por peso do FORMATO de quantizacao (nao do modelo); para F32/F16 e os valores exatos */
static double bpw_fmt(uint32_t t) {
    if (t == T_F32) return 32.0;
    if (t == T_F16 || t == T_BF16) return 16.0;
    const int u = qunit(t);
    const size_t qb = qbytes(t, (size_t)u);
    return qb ? (double)qb * 8.0 / u : 16.0;
}

/* recomenda um tipo de quantizacao para esta maquina (ESTIMATIVA). Devolve o T_* alvo, ou 0
   quando nao vale requantizar (origem ja <= o alvo sugerido). Imprime o raciocinio.
   is_moe: modelo de mistura de especialistas; src_t: tipo representativo da origem.
   vram: orcamento real de VRAM desta maquina AGORA (bytes; 0 = sem GPU) -- gpu_budget(), nao um
   valor fixo de outra maquina. ram: RAM util real desta maquina AGORA (bytes), ja com a margem de
   seguranca tirada (ver ram_usable em prepare()). Antes (ate 2026-11-28) estas duas vinham de
   constantes fixas (3,4 GB / 43 GB) calibradas na maquina do desenvolvedor; bug de portabilidade
   corrigido medindo a maquina real. */
static uint32_t recomendar_quant(size_t n_params, int is_moe, uint32_t src_t, double vram, double ram) {
    static const struct { uint32_t t; const char *name; double bpp; } S[] = {
        { T_Q8_0, "Q8_0", 1.06 }, { T_Q6_K, "Q6_K", 0.82 }, { T_Q5_K, "Q5_K_M", 0.70 },
        { T_Q4_K, "Q4_K_M", 0.58 }, { T_Q2_K, "Q2_K", 0.36 } };
    uint32_t tgt;
    if (is_moe) {
        const double q4 = (double)n_params * 0.58, q2 = (double)n_params * 0.36;
        if (q4 <= ram) {
            tgt = T_Q4_K;
            printf("  - Tipo recomendado (estimativa): Q4_K_M  (~%.1f GB) -- MoE: equilibrio qualidade/tamanho, cabe na RAM util medida agora (~%.1f GB)\n", q4 / 1e9, ram / 1e9);
        } else {
            tgt = T_Q2_K;
            printf("  - Tipo recomendado (estimativa): Q2_K  (~%.1f GB) -- MoE: Q4_K_M (~%.1f GB) nao caberia na RAM util medida agora (~%.1f GB)\n", q2 / 1e9, q4 / 1e9, ram / 1e9);
        }
    } else if (vram > 0) {
        int pick = -1;
        for (int i = 0; i < 5; i++) {
            if (S[i].t == T_Q2_K) break;        /* denso nunca cai sozinho em Q2_K */
            if ((double)n_params * S[i].bpp <= vram) { pick = i; break; }
        }
        if (pick >= 0) {
            tgt = S[pick].t;
            printf("  - Tipo recomendado (estimativa): %s  (~%.1f GB) -- denso: maior K-quant que cabe no orcamento de VRAM medido agora (~%.1f GB) para rodar inteiro na GPU\n",
                   S[pick].name, (double)n_params * S[pick].bpp / 1e9, vram / 1e9);
        } else {
            tgt = T_Q4_K;
            printf("  - Tipo recomendado (estimativa): Q4_K_M  (~%.1f GB) -- denso: nem Q4_K cabe na VRAM medida agora (~%.1f GB); roda parcial/CPU\n",
                   (double)n_params * 0.58 / 1e9, vram / 1e9);
        }
    } else {    /* sem GPU: a mesma logica do MoE, mas para o modelo denso inteiro mapeado na RAM */
        const double q4 = (double)n_params * 0.58, q2 = (double)n_params * 0.36;
        tgt = q4 <= ram ? T_Q4_K : T_Q2_K;
        printf("  - Tipo recomendado (estimativa): %s  (~%.1f GB) -- sem GPU: %s na RAM util medida agora (~%.1f GB)\n",
               qtype_name(tgt), (tgt == T_Q4_K ? q4 : q2) / 1e9, tgt == T_Q4_K ? "cabe" : "Q4_K_M nao caberia", ram / 1e9);
    }
    if (bpw_fmt(tgt) >= bpw_fmt(src_t) - 1e-6) {
        printf("    (a origem ja esta em %s, ~%.2f bits/peso <= o alvo; nao vale requantizar)\n", qtype_name(src_t), bpw_fmt(src_t));
        return 0;
    }
    return tgt;
}

static int prepare(Model *m, const char *path) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab;
    const size_t qd = (size_t)c->n_heads * c->hd, ctx = c->ctx;
    const uint64_t budget = gpu_budget();
    const int gpu_ok = gpu.on;
    const int have_gpu = gpu.vram > 0;
    const int packable = !c->g4 && !c->ssm_inner;   /* pack ainda nao cobre gemma4 nem DeltaNet */

    /* RAM real desta maquina, medida agora -- nao um valor fixo de outra maquina (ram_usable_now() acima). */
    uint64_t ram_av, ram_tot;
    const uint64_t ram_usable = ram_usable_now(&ram_av, &ram_tot);

    /* tipo de quantizacao: a matriz representativa (gate do feed-forward; especialista num MoE) */
    uint32_t qt = T_F32, qte = 0;
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
        if (L->w_gate.q || L->w_gate.f) { qt = L->w_gate.type; break; }
        if (L->w_down.q || L->w_down.f) { qt = L->w_down.type; break; }
    }
    if (c->n_expert)
        for (int l = 0; l < c->n_layers; l++) if (m->layers[l].eg) { qte = m->layers[l].eg[0].type; break; }

    /* tamanho total como o motor estima (todos os especialistas residentes + cache KV float32) */
    const size_t NX = c->n_expert ? (size_t)c->n_expert : 1;
    const size_t kv_layer = 2 * ctx * kvd * sizeof(float);
    size_t total = gpu_mat_bytes(&m->lm_head, V, d) + ((size_t)256 << 20);
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
        total += gpu_mat_bytes(&L->wq, qd, d) + gpu_mat_bytes(&L->wk, kvd, d) + gpu_mat_bytes(&L->wv, kvd, d)
               + gpu_mat_bytes(&L->wo, d, qd) + NX * (gpu_mat_bytes(&L->w_gate, H, d) + gpu_mat_bytes(&L->w_up, H, d)
               + gpu_mat_bytes(&L->w_down, d, H)) + kv_layer;
    }
    const int kv16 = getenv("KATANA_KV16") ? atoi(getenv("KATANA_KV16")) != 0 : total > budget;
    const size_t total_ch = kv16 ? total - (size_t)c->n_layers * kv_layer / 2 : total;
    const int fits = have_gpu && total_ch <= budget;
    const char *backend = gpu.vk ? "vulkan" : have_gpu ? "opencl" : "cpu";

    /* contexto sugerido: o do modelo quando cabe; senao um que deixe o cache KV numa fatia da VRAM */
    int ctx_sug = (int)ctx;
    if (!fits && have_gpu) {
        const size_t kvpos = 2 * kvd * (size_t)c->n_layers * (kv16 ? 2 : 4);
        long cs = kvpos ? (long)(budget / 4 / kvpos) : (long)ctx;
        cs = cs / 512 * 512;
        if (cs < 512) cs = 512;
        if (cs > (long)ctx) cs = (long)ctx;
        if (cs > 8192) cs = 8192;
        ctx_sug = (int)cs;
    }

    printf("\n==== Plano de preparo: %s ====\n", path);
    printf("Modelo: arquitetura=%s  parametros=%.1fM  camadas=%d  quantizacao=%s", c->arch, m->n_params / 1e6, c->n_layers, qtype_name(qt));
    if (c->n_expert) printf("  (especialistas %s)", qtype_name(qte));
    printf("\n");
    if (c->n_expert) printf("MoE: %d especialistas por camada, %d ativos por token\n", c->n_expert, c->n_used);
    if (have_gpu) printf("Maquina: GPU %s %.1f GB (orcamento %.0f MB); tamanho estimado na VRAM %.0f MB\n",
                         gpu.name, gpu.vram / 1073741824.0, budget / 1048576.0, total_ch / 1048576.0);
    else printf("Maquina: sem GPU disponivel agora; plano para CPU\n");
    printf("Maquina: RAM total %.1f GB, livre agora %.1f GB; util com margem de seguranca %.1f GB (medido nesta maquina)\n",
           ram_tot / 1e9, ram_av / 1e9, ram_usable / 1e9);

    printf("\nDecisoes:\n");
    printf("  - Cabe inteiro na VRAM: %s\n", fits ? "sim" : "nao");
    printf("  - Backend: %s\n", backend);
    printf("  - Cache KV: %d bits%s\n", kv16 ? 16 : 32, kv16 ? " (o modelo nao cabe inteiro com 32)" : "");
    printf("  - Contexto sugerido: %d%s\n", ctx_sug, ctx_sug < (int)ctx ? " (reduzido para liberar VRAM)" : "");
    if (c->n_expert) {
        if (fits) printf("  - Especialistas: todos na VRAM (grafo completo)\n");
        else if (gpu_ok && gx.on) printf("  - Especialistas: cache na VRAM com %d vagas por camada; o resto na CPU/disco\n", gx.S);
        else printf("  - Especialistas: cache na VRAM com vagas por camada (quantas, a definir com a GPU livre); o resto na CPU/disco\n");
    }
    if (c->n_expert && !fits && (qte == T_Q6_K || qte == T_Q5_K || qte == T_Q8_0 || qte == T_F16 || qte == T_BF16))
        printf("  - Sugestao: os especialistas estao em %s; requantizados em Q4_K caberiam mais vagas na VRAM (nao requantizo sozinho)\n", qtype_name(qte));

    recomendar_quant(m->n_params, c->n_expert > 0, c->n_expert ? qte : qt,
                      (double)(have_gpu ? budget : 0), (double)ram_usable);

    printf("\nOtimizacoes para %s:\n", c->arch);
    printf("  aplicadas: backend %s, cache KV %d bits, ajuste de ladrilhos (tune), shards no layout da GPU (pack)%s\n",
           backend, kv16 ? 16 : 32, c->n_expert ? ", cache de especialistas por calor (heat)" : "");
    if (c->g4) printf("  faltam: prompt em lote na GPU (hoje token a token no Vulkan), grafo completo gravado, pack dos shards\n");
    else if (c->ssm_inner) printf("  faltam: prompt do DeltaNet em blocos na GPU, pack dos shards\n");
    else if (c->mla) printf("  faltam: prompt MoE em lote no modo hibrido (hoje token a token), atencao MLA na GPU\n");
    else if (c->n_expert) printf("  aplicadas tambem: prompt MoE em lote no Vulkan\n");
    else printf("  aplicadas tambem: prompt em lote com nucleos tensoriais, decodificacao especulativa\n");

    /* ---- executa o que e seguro e automatico ---- */
    int did_pack = 0, did_tune = 0, did_heat = 0, pend_gpu = 0;
    printf("\nExecutando:\n");

    /* 1) pack: shards no layout da GPU, se ainda nao houver atualizados */
    if (!packable) printf("  - pack: ainda nao cobre esta arquitetura; pulado\n");
    else if (shards.use) printf("  - pack: shards ja atualizados em %s\n", shards.dir);
    else if (getenv("KATANA_PREP_NOPACK")) printf("  - pack: pulado (KATANA_PREP_NOPACK)\n");
    else {
        printf("  - pack: gerando shards (pode demorar em modelos grandes)...\n");
        if (pack(m, path, m->fsize) == 0) { did_pack = 1; shards_open(path, m->fsize); }
        else printf("    pack falhou\n");
    }

    /* 2) tune: so com o modelo inteiro na VRAM e a placa livre */
    if (fits && gpu_ok && gpu.full) {
        printf("  - tune: medindo o melhor ajuste de ladrilhos...\n");
        if (tune(m) == 0) did_tune = 1;
    } else if (fits) { printf("  - tune: adiado (a GPU esta ocupada; rode de novo com a placa livre)\n"); pend_gpu = 1; }
    else printf("  - tune: nao se aplica (o modelo nao cabe inteiro na VRAM)\n");

    /* 3) calibracao do calor dos especialistas: so MoE com o cache ativo */
    if (c->n_expert) {
        if (gpu_ok && gx.on) {
            printf("  - calor: calibrando quais especialistas manter quentes...\n");
            const char *calib = "A capital da Franca e Paris. Explique brevemente a fotossintese. "
                "Escreva uma funcao em C que soma dois vetores. Qual e a raiz quadrada de 144? "
                "Traduza bom dia para o ingles. Liste tres planetas do sistema solar. "
                "Resuma a teoria da relatividade em uma frase. def soma(a, b): return a + b. "
                "Conte uma piada curta. Quais sao as cores primarias?";
            Ids ids = {0};
            if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
            encode(&m->tk, calib, &ids);
            if (ids.n > 0 && ids.n < c->ctx) {
                float *lg = forward_seq(m, ids.ids, ids.n, 0, 1);
                int pos = ids.n;
                for (int g = 0; g < 48 && pos < c->ctx; g++) lg = forward(m, sample(lg, V, 0.0f), pos++, 1);
                heat_save(m);
                did_heat = 1;
            }
            free(ids.ids);
        } else { printf("  - calor: adiado (a GPU esta ocupada; o cache de especialistas nao esta ativo)\n"); pend_gpu = 1; }
    }

    /* 4) bundle de quantizacao mista + arena de RAM para os especialistas: automatico quando a parte do
       modelo que fica fora da VRAM (ESTIMATIVA) nao cabe confortavel na RAM livre com a margem de seguranca
       (ram_usable, calculada acima). So se aplica a MoE. A env var do usuario, se setada, sempre vence -- aqui
       so gravamos o mesmo valor no perfil, nao decidimos por cima dela. Decisao do dono, 2026-11-28: nunca usar
       100% da RAM livre; a arena nunca passa do teto de 25 GB que et_init() ja impoe. */
    double expram_gb = 0.0;
    const char *arena_status = "n/a";
    if (c->n_expert) {
        const char *uev = getenv("KATANA_EXPERT_RAM");
        const int manual = uev && atof(uev) > 0;
        if (manual) {
            expram_gb = atof(uev);
            printf("  - arena de RAM: KATANA_EXPERT_RAM=%.2f definido pelo usuario; mantido (nao decido por cima)\n", expram_gb);
            arena_status = "manual";
        } else {
            /* parte do modelo que fica fora da VRAM e precisa ser lida de RAM/disco a cada passo (ESTIMATIVA) */
            const double off_gpu = have_gpu ? (total > budget ? (double)(total - budget) : 0.0) : (double)total;
            if (off_gpu > (double)ram_usable && ram_usable > 0) {
                expram_gb = ram_usable / 1e9;
                if (expram_gb > 25.0) expram_gb = 25.0;   /* mesmo teto de et_init() */
                printf("  - arena de RAM: parte fora da VRAM (~%.1f GB, estimativa) nao cabe confortavel na RAM livre com margem (~%.1f GB); ligando arena de %.1f GB\n",
                       off_gpu / 1e9, ram_usable / 1e9, expram_gb);
                arena_status = "ligada";
            } else {
                printf("  - arena de RAM: nao necessaria (parte fora da VRAM ~%.1f GB, estimativa, cabe confortavel na RAM livre com margem ~%.1f GB)\n",
                       off_gpu / 1e9, ram_usable / 1e9);
                arena_status = "nao";
            }
        }
        if (expram_gb > 0) {
            char exb[1100];
            et_bx_name(exb, sizeof exb, path);
            struct stat stb;
            if (stat(exb, &stb) == 0) printf("  - bundle (.exb): ja existe em %s\n", exb);
            else {
                printf("  - bundle (.exb): criando pacote de quantizacao mista para os especialistas (katana bundle)...\n");
                etb_write(path);
            }
        }
    }

    /* ---- grava o perfil ---- */
    char fn[1200];
    perfil_path(fn, sizeof fn, path);
    FILE *f = fopen(fn, "w");
    if (f) {
        fprintf(f, "katana-perfil 1\norigem %lld\ngpu %s\nbackend %s\nkv16 %d\nctx %d\nnoexpcache 0\nvagas %d\ntune %s\nexpram %.3f\n",
                (long long)m->fsize, have_gpu ? gpu.name : "-", backend, kv16, ctx_sug,
                (gpu_ok && gx.on) ? gx.S : 0, did_tune ? "sim" : "nao", expram_gb);
        fclose(f);
        printf("\nPerfil gravado em %s\n", fn);
    } else printf("\nNao consegui gravar o perfil em %s\n", fn);

    printf("Resumo: pack=%s  tune=%s  calor=%s  arena=%s%s\n",
           did_pack ? "feito" : !packable ? "n/a" : shards.use ? "ja estava" : "nao",
           did_tune ? "feito" : fits ? "pendente" : "n/a",
           c->n_expert ? (did_heat ? "feito" : "pendente") : "n/a",
           arena_status,
           pend_gpu ? "   (partes de GPU pendentes: a placa estava ocupada)" : "");
    printf("Agora chat/serve/agent usam este perfil automaticamente (uma variavel de ambiente do usuario vence).\n");
    return 0;
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Nao consegui abrir %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *t = xalloc((size_t)n + 1, 1);
    if (fread(t, 1, (size_t)n, f) != (size_t)n) die("falha ao ler o texto");
    fclose(f);
    return t;
}

static int cmp_float_desc(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* Mede se o feed-forward e esparso POR ASSUNTO.
   1) roda o texto de calibracao e soma a ativacao de cada neuronio (por camada);
   2) roda o texto de avaliacao com o modelo inteiro e guarda o token previsto em cada posicao;
   3) repete a avaliacao mantendo so os p% neuronios mais ativos na calibracao (e, para
      comparar, p% escolhidos ao acaso) e conta em quantas posicoes a previsao continua igual. */
static int sparsity(Model *m, const char *calib_path, const char *eval_path) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    const int H = c->hidden, L = c->n_layers, V = c->vocab;
    Ids ca = {0}, ev = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ca, c->bos);
    encode(tk, read_text(calib_path), &ca);
    encode(tk, read_text(eval_path), &ev);
    if (ca.n + ev.n >= c->ctx) die("textos maiores que o contexto");
    printf("calibracao: %d tokens | avaliacao: %d tokens | %d camadas x %d neuronios\n", ca.n, ev.n, L, H);

    sp.sum = xalloc((size_t)L * H, sizeof(float));
    sp.collect = 1;
    for (int i = 0; i < ca.n; i++) forward(m, ca.ids[i], i, 0);
    sp.collect = 0;

    int *full = xalloc(ev.n, sizeof(int));
    float *ref = xalloc((size_t)ev.n, sizeof(float));
    for (int i = 0; i < ev.n; i++) {
        const float *lg = forward(m, ev.ids[i], ca.n + i, 1);
        full[i] = sample(lg, V, 0.0f); ref[i] = lg[full[i]];
    }

    /* quanto da ativacao total da calibracao os p% mais ativos concentram */
    float *tmp = xalloc(H, sizeof(float));
    uint8_t *mask = xalloc((size_t)L * H, 1);
    static const int pcts[] = { 10, 20, 30, 50, 70 };
    printf("%-10s %-22s %-28s %-22s\n", "mantidos", "ativacao coberta", "mesma previsao (quentes)", "mesma previsao (acaso)");
    for (int pi = 0; pi < 5; pi++) {
        const int keep = H * pcts[pi] / 100;
        double agree[2] = { 0, 0 }, cover = 0.0;
        for (int mode = 0; mode < 2; mode++) {      /* 0 = quentes da calibracao, 1 = ao acaso */
            srand(4242);
            for (int l = 0; l < L; l++) {
                uint8_t *mk = mask + (size_t)l * H;
                memset(mk, 0, H);
                if (mode == 0) {
                    memcpy(tmp, sp.sum + (size_t)l * H, H * sizeof(float));
                    qsort(tmp, H, sizeof(float), cmp_float_desc);
                    const float thr = tmp[keep - 1];
                    int n = 0;
                    for (int i = 0; i < H && n < keep; i++) if (sp.sum[(size_t)l * H + i] >= thr) { mk[i] = 1; n++; }
                } else {
                    for (int n = 0; n < keep; ) { const int i = (int)(((unsigned)rand() * 32768u + (unsigned)rand()) % (unsigned)H); if (!mk[i]) { mk[i] = 1; n++; } }
                }
            }
            sp.mask = mask; sp.mass_all = sp.mass_kept = 0.0;
            int same = 0;
            for (int i = 0; i < ev.n; i++) {
                const float *lg = forward(m, ev.ids[i], ca.n + i, 1);
                if (sample(lg, V, 0.0f) == full[i]) same++;
            }
            sp.mask = NULL;
            agree[mode] = 100.0 * same / ev.n;
            if (mode == 0) cover = 100.0 * sp.mass_kept / (sp.mass_all + 1e-30);
        }
        printf("%3d%%       %5.1f%%                 %5.1f%%                       %5.1f%%\n", pcts[pi], cover, agree[0], agree[1]);
    }
    /* teto da escolha por token: mantem os p% mais ativos de cada token (oraculo) */
    printf("escolha por token (oraculo):\n%-10s %-22s %-28s\n", "mantidos", "ativacao coberta", "mesma previsao");
    sp.tmp = xalloc(H, sizeof(float));
    static const int op[] = { 5, 10, 20, 30, 50 };
    for (int pi = 0; pi < 5; pi++) {
        int same = 0;
        sp.oracle_keep = H * op[pi] / 100; sp.mass_all = sp.mass_kept = 0.0;
        for (int i = 0; i < ev.n; i++) {
            const float *lg = forward(m, ev.ids[i], ca.n + i, 1);
            if (sample(lg, V, 0.0f) == full[i]) same++;
        }
        sp.oracle_keep = 0;
        printf("%3d%%       %5.1f%%                 %5.1f%%\n", op[pi], 100.0 * sp.mass_kept / (sp.mass_all + 1e-30), 100.0 * same / ev.n);
    }
    (void)ref;
    return 0;
}

typedef struct { uint32_t cnt; int id; } RtRank;
static int cmp_rtrank(const void *a, const void *b) {
    const RtRank *x = a, *y = b;
    return x->cnt < y->cnt ? 1 : x->cnt > y->cnt ? -1 : x->id - y->id;
}

/* Mede quanto o roteamento de um modelo MoE se concentra POR ASSUNTO, e quanto isso vale
   para manter na VRAM so parte dos especialistas.
   1) roda o texto de calibracao contando o uso de cada especialista (camada, indice);
   2) roda o texto de avaliacao guardando os especialistas escolhidos em cada token;
   3) para uma VRAM que comporta p% dos especialistas, conta quantos usos da avaliacao
      cairiam na VRAM: com o conjunto fixo mais usado na calibracao, com um cache que
      troca o menos recente (LRU, partindo desse conjunto) e com um conjunto ao acaso. */
static int routing(Model *m, const char *calib_path, const char *eval_path) {
    const Config *c = &m->cfg;
    const int E = c->n_expert, K = c->n_used, L = c->n_layers, N = L * E;
    if (!E) die("este modelo nao e MoE");
    Ids ca = {0}, ev = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ca, c->bos);
    encode(&m->tk, read_text(calib_path), &ca);
    encode(&m->tk, read_text(eval_path), &ev);
    if (ca.n + ev.n >= c->ctx) die("textos maiores que o contexto");
    printf("calibracao: %d tokens | avaliacao: %d tokens | %d camadas x %d especialistas (%d por token)\n", ca.n, ev.n, L, E, K);

    moe.cnt = xalloc(N, sizeof(uint32_t));
    for (int i = 0; i < ca.n; i++) forward(m, ca.ids[i], i, 0);
    uint32_t *ccal = moe.cnt;
    moe.cnt = xalloc(N, sizeof(uint32_t));
    moe.trace = xalloc((size_t)ev.n * L * K, sizeof(uint16_t));
    for (int i = 0; i < ev.n; i++) { moe.ti = i; forward(m, ev.ids[i], ca.n + i, 0); }
    const uint16_t *tr = moe.trace; moe.trace = NULL;
    uint32_t *cev = moe.cnt; moe.cnt = NULL;

    int used_c = 0, used_e = 0, both = 0;
    for (int i = 0; i < N; i++) { used_c += ccal[i] > 0; used_e += cev[i] > 0; both += ccal[i] > 0 && cev[i] > 0; }
    printf("especialistas distintos usados: calibracao %d de %d (%.0f%%), avaliacao %d (%.0f%%), em comum %d\n",
           used_c, N, 100.0 * used_c / N, used_e, 100.0 * used_e / N, both);

    RtRank *rk = xalloc(N, sizeof(RtRank)), *rke = xalloc(N, sizeof(RtRank));
    for (int i = 0; i < N; i++) { rk[i].cnt = ccal[i]; rk[i].id = i; rke[i].cnt = cev[i]; rke[i].id = i; }
    qsort(rk, N, sizeof(RtRank), cmp_rtrank);
    qsort(rke, N, sizeof(RtRank), cmp_rtrank);
    const double total = (double)ev.n * L * K;
    int *stamp = xalloc(N, sizeof(int));        /* 0 = fora da VRAM; senao, ultimo uso */
    uint8_t *in = xalloc(N, 1);
    static const int pcts[] = { 5, 10, 15, 25, 35, 50 };
    printf("%-10s %-12s %-14s %-14s %-12s %-10s\n", "na VRAM", "teto", "conjunto fixo", "cache LRU", "trocas/token", "acaso");
    for (int pi = 0; pi < 6; pi++) {
        const int cap = N * pcts[pi] / 100;
        double hit_fix = 0, hit_lru = 0, hit_best = 0;
        long swaps = 0;
        memset(in, 0, N);
        for (int i = 0; i < cap; i++) in[rk[i].id] = 1;
        for (int i = 0; i < cap; i++) hit_best += rke[i].cnt;       /* teto: os mais usados da propria avaliacao */
        {   /* teto com o mesmo numero de vagas em toda camada */
            const int per = E * pcts[pi] / 100;
            double hu = 0;
            RtRank *tmp = xalloc(E, sizeof(RtRank));
            for (int l = 0; l < L; l++) {
                for (int e = 0; e < E; e++) { tmp[e].cnt = cev[l * E + e]; tmp[e].id = e; }
                qsort(tmp, E, sizeof(RtRank), cmp_rtrank);
                for (int i = 0; i < per; i++) hu += tmp[i].cnt;
            }
            free(tmp);
            printf("%3d%%: teto com vagas iguais por camada %5.1f%%\n", pcts[pi], 100.0 * hu / total);
        }
        memset(stamp, 0, N * sizeof(int));
        for (int i = 0; i < cap; i++) stamp[rk[i].id] = 1;
        int clock = 1;
        for (int t = 0; t < ev.n; t++)
            for (int l = 0; l < L; l++)
                for (int k = 0; k < K; k++) {
                    const int id = l * E + tr[((size_t)t * L + l) * K + k];
                    if (in[id]) hit_fix++;
                    clock++;
                    if (stamp[id]) { hit_lru++; stamp[id] = clock; continue; }
                    int old = -1;                               /* troca o usado ha mais tempo */
                    for (int j = 0; j < N; j++) if (stamp[j] && (old < 0 || stamp[j] < stamp[old])) old = j;
                    if (old >= 0) stamp[old] = 0;
                    stamp[id] = clock; swaps++;
                }
        printf("%3d%%       %5.1f%%       %5.1f%%         %5.1f%%         %6.2f       %3d%%\n", pcts[pi],
               100.0 * hit_best / total, 100.0 * hit_fix / total, 100.0 * hit_lru / total, (double)swaps / ev.n, pcts[pi]);
    }
    return 0;
}

