/* ---------- qwen4exp (Qwen3.8-Flash-Next) na CPU ----------
   Camadas do qwen35moe (Gated DeltaNet, atencao com portao, MoE com compartilhado) com quatro diferencas:
   - residual em R ramos (hc): o estado de um token tem R x dim. Cada bloco le uma mistura dos ramos, com um portao por
     elemento de posto baixo (sobre o estado normalizado por ramo), e soma a saida em cada ramo com um peso por ramo:
       N = rms por ramo(X);  M = sigmoide(up(silu(down(N)/R)));  entrada = media dos ramos de M*N
       X_r += saida * 2 sigmoide(inject(N)/R)_r
     Nao ha outras normas de camada; a saida do modelo e mais uma mistura, sem norma final.
   - atencao esparsa: as posicoes 0..p sao cortadas em micro-blocos de `ratio` tokens; um indexador (q de varias
     cabecas, k unica, a media das k cruas do bloco) da uma nota a cada bloco completo e a consulta so ve os
     budget/ratio melhores mais a sobra incompleta. Ate `budget` posicoes completas isso e a atencao inteira.
   - n-gramas (ple): numa camada, o bigrama e o trigrama que terminam no token viram linhas de uma tabela enorme
     (varios hashes por ordem); as linhas concatenadas dao uma chave por ramo e um valor, o portao sai de chave x ramo
     e uma convolucao dilatada por canal acrescenta o contexto. A tabela fica no disco (arquivo mapeado com aviso de
     acesso aleatorio): so as paginas usadas entram na RAM, pedidas juntas antes de um lote.
   - o portao da saida do DeltaNet e sigmoide(z), nao SiLU(z).
   Matrizes BF16 (hc, roteador, projecoes do ple) ficam no arquivo mapeado, sem copia em float32. */
#define Q4_BMAX 1024             /* maior lote por passada */
/* opcoes de velocidade (KATANA_Q4_OPT; mascaras alternadas pelo q4bench). Todas dao o mesmo resultado dentro do arredondamento:
   1 lotes grandes no prompt (KATANA_Q4_BATCH, padrao 256), 2 produtos em lote com a linha aberta uma vez tambem em Q4_K/Q5_K/Q6_K
   e BF16 em AVX-512, 4 DeltaNet por cabeca e por canal (todos os tokens do lote de uma vez), 8 trabalho serial por token repartido
   entre as threads, 16 especialistas em blocos com saida local, 32 leitura antecipada dos especialistas ainda nao tocados,
   64 Q2_0 e IQ4_NL em 8 bits (VBMI), 128 geracao com a placa (vk_qwen4exp.c) */
enum { QO_BATCH = 1, QO_KERN = 2, QO_DN = 4, QO_PAR = 8, QO_EXP = 16, QO_AHEAD = 32, QO_K8 = 64, QO_GPU = 128, QO_ONE = 1 << 20 /* q4bench: prompt token a token */ };
static int q4_opt = -1, q4_bsize;
#define Q4_RMAX 16              /* ramos */
typedef struct { Mat down, up, inj; float *norm; } Q4Hc;
typedef struct { Q4Hc att, ffn; Mat router, iq, ik; float *iqn, *ikn; int ia; } Q4L;
typedef struct Q4 {
    int R, rank;                                /* ramos e posto da mistura */
    int in, ihd, ibudget, ratio, n_ratio;       /* indexador: cabecas de q, largura, orcamento em tokens, tokens por bloco */
    int ratios[1024];
    int ple_l, n_ple, ng, hpn, pk, pd;          /* camada com ple, ordem maxima, hashes por ordem, janela da convolucao, largura por hash */
    uint64_t eos, mult[8], off[64], voc[64];
    int n_mult, n_off, n_voc;
    Q4L *l;
    Q4Hc out;
    Mat tab, pkey, pval; size_t tab_rows;       /* tabela de n-gramas (no arquivo) e projecoes */
    float *pconv, *pn_key, *pn_q, *pn_conv;
    /* estado */
    int hist[8];                                /* ultimos ng-1 tokens */
    float *conv;                                /* [(pk-1)*ng][R*dim] entradas da convolucao do ple */
    uint8_t *lseen;                             /* [camada] pedida inteira (prompt em lote) */
    uint8_t *seen;                              /* [camada][especialista] ja pedido ao sistema nesta execucao */
    float *iacc, *ikey;                         /* por camada esparsa: soma das k cruas do bloco aberto [ihd]; chaves prontas [ctx/ratio][ihd] */
} Q4;
static struct { float *x; int n0, n; } q4_dbg;  /* q4check: estado depois de cada camada [pos][camada][R*dim] */

/* medida por parte (KATANA_Q4_PROF=1 ou q4bench): [0] lote (prompt), [1] um token (geracao) */
enum { QP_EMB, QP_PLE, QP_HCR, QP_HCW, QP_DNP, QP_DNR, QP_DNO, QP_ATP, QP_ATI, QP_ATC, QP_ATO, QP_RT, QP_EG, QP_EA, QP_ED, QP_ES, QP_SH, QP_HD, QP_N };
static const char *const q4_pname[QP_N] = { "embedding", "n-grams", "mixer read", "mixer write", "deltanet proj", "deltanet recurrence", "deltanet out", "attention proj",
    "attention keys", "attention core", "attention out", "router", "experts gate/up", "experts act", "experts down", "experts sum", "shared expert", "output head" };
static double q4_pt[2][QP_N]; static long q4_ptok[2]; static int q4_prof = -1, q4_pw;
static double q4_tick(int part, double t0) {
    if (q4_prof <= 0) return 0.0;
    const double t = now_sec();
    if (part >= 0) q4_pt[q4_pw][part] += t - t0;
    return t;
}
static void q4_prof_print(void) {
    for (int w = 0; w < 2; w++) {
        double tot = 0.0;
        if (!q4_ptok[w]) continue;
        for (int i = 0; i < QP_N; i++) tot += q4_pt[w][i];
        printf("%s: %ld tokens, %.2f s, %.1f ms/token\n", w ? "generation" : "prompt", q4_ptok[w], tot, 1e3 * tot / q4_ptok[w]);
        for (int i = 0; i < QP_N; i++) printf("  %-20s %8.2f ms/token %5.1f%%\n", q4_pname[i], 1e3 * q4_pt[w][i] / q4_ptok[w], 100 * q4_pt[w][i] / tot);
    }
}

static int q4_kv(Model *m, const char *k, int kl, uint32_t t, Rd *r) {
    if (!m->q4) { m->q4 = xalloc(1, sizeof(Q4)); m->q4->ple_l = -1; }
    Q4 *q = m->q4;
    if (t == 8) return 0;
    if (t == 9) {
        const int which = key_ends(k, kl, ".attention.compress_ratios") ? 0 : key_ends(k, kl, ".ple.layers") ? 1 : key_ends(k, kl, ".ple.layer_multipliers") ? 2
                        : key_ends(k, kl, ".ple.head_offsets") ? 3 : key_ends(k, kl, ".ple.head_vocab_sizes") ? 4 : -1;
        if (which < 0) return 0;
        const uint32_t et = rd_u32(r);
        const uint64_t n = rd_u64(r);
        if (et == 8 || et == 9) die("qwen4exp: lista de tipo inesperado");
        for (uint64_t j = 0; j < n; j++) {
            const double v = rd_num(r, et);     /* inteiros abaixo de 2^53: o double guarda exato */
            if (v < 0 || v >= 9007199254740992.0) die("qwen4exp: valor de lista invalido");
            if (which == 0 && j < 1024) q->ratios[q->n_ratio++] = (int)v;
            else if (which == 1 && q->n_ple++ == 0) q->ple_l = (int)v;
            else if (which == 2 && j < 8) q->mult[q->n_mult++] = (uint64_t)v;
            else if (which == 3 && j < 64) q->off[q->n_off++] = (uint64_t)v;
            else if (which == 4 && j < 64) q->voc[q->n_voc++] = (uint64_t)v;
        }
        return 1;
    }
    int *dst = key_ends(k, kl, ".hyper_connection.count") ? &q->R : key_ends(k, kl, ".hyper_connection.low_rank") ? &q->rank
             : key_ends(k, kl, ".attention.indexer.head_count") ? &q->in : key_ends(k, kl, ".attention.indexer.key_length") ? &q->ihd
             : key_ends(k, kl, ".attention.indexer.top_k") ? &q->ibudget : key_ends(k, kl, ".ple.ngram_size") ? &q->ng
             : key_ends(k, kl, ".ple.heads_per_ngram") ? &q->hpn : key_ends(k, kl, ".ple.conv_kernel") ? &q->pk
             : key_ends(k, kl, ".embedding_length_per_layer_input") ? &q->pd : NULL;
    if (dst) { *dst = (int)rd_num(r, t); return 1; }
    if (key_ends(k, kl, ".ple.eos_token_id")) { q->eos = (uint64_t)rd_num(r, t); return 1; }
    return 0;
}

static void q4_config(Model *m) {
    Config *c = &m->cfg;
    Q4 *q = m->q4;
    if (!q || q->R < 2 || q->R > Q4_RMAX || q->rank < 1 || !c->ssm_inner || !c->n_expert || c->n_shared != 1) die("qwen4exp: metadados incompletos");
    if (q->n_ratio != c->n_layers) die("qwen4exp: compress_ratios nao cobre as camadas");
    for (int l = 0; l < c->n_layers; l++) {
        if ((q->ratios[l] == 0) != ((l + 1) % c->attn_every != 0)) die("qwen4exp: camadas de atencao fora do intervalo");
        if (q->ratios[l]) { if (q->ratio && q->ratio != q->ratios[l]) die("qwen4exp: razoes de compressao diferentes"); q->ratio = q->ratios[l]; }
    }
    if (q->ratio && (q->in < 1 || q->ihd < 2 || q->ibudget < q->ratio || q->ibudget % q->ratio || c->rope_dim < 2 || c->rope_dim > q->ihd || c->rope_dim > c->hd))
        die("qwen4exp: indexador invalido");
    if (q->n_ple > 1) die("qwen4exp: so uma camada com n-gramas e suportada");
    if (q->n_ple) {
        const int nh = (q->ng - 1) * q->hpn;
        if (q->ple_l >= c->n_layers || q->ratios[q->ple_l] || q->ng < 2 || q->ng > 8 || nh < 1 || nh > 64 || q->pk < 1 || q->pd < 1
            || q->n_mult != q->ng || q->n_off != nh || q->n_voc != nh) die("qwen4exp: n-gramas invalidos");
        for (int h = 0; h < nh; h++) if (!q->voc[h]) die("qwen4exp: n-gramas invalidos");
    }
    for (int i = 0; i < 8; i++) q->hist[i] = (int)q->eos;
}

/* ---- carga ---- */
static const TInfo *q4_find(const Tensors *ts, const char *name) {
    const int nl = (int)strlen(name);
    for (uint64_t i = 0; i < ts->n; i++) if (ts->t[i].namelen == nl && memcmp(ts->t[i].name, name, nl) == 0) { ts->t[i].used = 1; return &ts->t[i]; }
    return NULL;
}
/* como get_mat, mas BF16 fica no arquivo (os produtos daqui leem BF16 direto) */
static Mat q4_mat(Model *m, const Tensors *ts, const char *name, size_t rows, size_t cols) {
    const TInfo *t = q4_find(ts, name);
    if (t && t->type == T_BF16) {
        size_t n = 1;
        for (uint32_t i = 0; i < t->ndims; i++) n *= (size_t)t->dims[i];
        if (n != rows * cols) { fprintf(stderr, "Tensor %s: tamanho inesperado\n", name); exit(1); }
        if (t->base + t->offset + n * 2 > t->lim) die("tensor fora do arquivo");
        Mat W = {0};
        W.q = t->base + t->offset; W.type = T_BF16; W.stride = cols * 2;
        m->n_params += n;
        return W;
    }
    return get_mat(ts, name, rows, cols, 1, &m->n_params);
}
static void q4_hc_load(Model *m, const Tensors *ts, Q4Hc *h, const char *pre, int inject) {
    const Q4 *q = m->q4;
    const size_t D = (size_t)q->R * m->cfg.dim;
    char nm[96];
    snprintf(nm, sizeof nm, "%s_norm.weight", pre); h->norm = get_tensor(ts, nm, D, 1, &m->n_params);
    snprintf(nm, sizeof nm, "%s_down.weight", pre); h->down = q4_mat(m, ts, nm, q->rank, D);
    snprintf(nm, sizeof nm, "%s_up.weight", pre);   h->up = q4_mat(m, ts, nm, D, q->rank);
    if (inject) { snprintf(nm, sizeof nm, "%s_inject.weight", pre); h->inj = q4_mat(m, ts, nm, q->R, D); }
}
static void q4_load(Model *m, const Tensors *ts) {
    Config *c = &m->cfg;
    Q4 *q = m->q4;
    const size_t d = c->dim, H = c->hidden, E = c->n_expert, D = q->R * d, kvd = (size_t)c->n_kv * c->hd, qd = (size_t)c->n_heads * c->hd;
    size_t *np = &m->n_params;
    char nm[96];
    int na = 0;
    q->l = xalloc(c->n_layers, sizeof(Q4L));
    q->seen = xalloc((size_t)c->n_layers * E, 1); q->lseen = xalloc(c->n_layers, 1);
    q4_hc_load(m, ts, &q->out, "output_hc", 0);
    m->out_norm = q->out.norm;
    for (int l = 0; l < c->n_layers; l++) {
        Layer *L = &m->layers[l];
        Q4L *Q = &q->l[l];
#define QN(x) (snprintf(nm, sizeof nm, "blk.%d." x, l), nm)
        q4_hc_load(m, ts, &Q->att, QN("hc_attn"), 1);       /* nm e recopiado dentro: o prefixo so e lido la */
        q4_hc_load(m, ts, &Q->ffn, QN("hc_ffn"), 1);
        L->lin = q->ratios[l] == 0;
        if (L->lin) {
            const size_t nv = c->ssm_rank, C = dn_channels(c);
            L->lin_i = m->n_lin++;
            L->wqkv = q4_mat(m, ts, QN("attn_qkv.weight"), C, d);
            L->wz = q4_mat(m, ts, QN("attn_gate.weight"), c->ssm_inner, d);
            L->walpha = q4_mat(m, ts, QN("ssm_alpha.weight"), nv, d);
            L->wbeta = q4_mat(m, ts, QN("ssm_beta.weight"), nv, d);
            L->wssm_out = q4_mat(m, ts, QN("ssm_out.weight"), d, c->ssm_inner);
            L->conv_w = get_tensor(ts, QN("ssm_conv1d.weight"), C * c->ssm_conv, 1, np);
            L->ssm_a = get_tensor(ts, QN("ssm_a"), nv, 1, np);
            L->dt_bias = get_tensor(ts, QN("ssm_dt.bias"), nv, 1, np);
            L->ssm_norm = get_tensor(ts, QN("ssm_norm.weight"), c->ssm_inner / nv, 1, np);
        } else {
            L->wq = q4_mat(m, ts, QN("attn_q.weight"), 2 * qd, d);
            L->wk = q4_mat(m, ts, QN("attn_k.weight"), kvd, d);
            L->wv = q4_mat(m, ts, QN("attn_v.weight"), kvd, d);
            L->wo = q4_mat(m, ts, QN("attn_output.weight"), d, qd);
            L->q_norm = get_tensor(ts, QN("attn_q_norm.weight"), c->hd, 1, np);
            L->k_norm = get_tensor(ts, QN("attn_k_norm.weight"), c->hd, 1, np);
            Q->ia = na++;
            Q->iq = q4_mat(m, ts, QN("indexer.q_proj.weight"), (size_t)q->in * q->ihd, d);
            Q->ik = q4_mat(m, ts, QN("indexer.k_proj.weight"), q->ihd, d);
            Q->iqn = get_tensor(ts, QN("indexer.q_norm.weight"), q->ihd, 1, np);
            Q->ikn = get_tensor(ts, QN("indexer.k_norm.weight"), q->ihd, 1, np);
        }
        Q->router = q4_mat(m, ts, QN("ffn_gate_inp.weight"), E, d);
        const Mat g = get_mat(ts, QN("ffn_gate_exps.weight"), E * H, d, 1, np);
        const Mat u = get_mat(ts, QN("ffn_up_exps.weight"), E * H, d, 1, np);
        const Mat w = get_mat(ts, QN("ffn_down_exps.weight"), E * d, H, 1, np);
        L->eg = xalloc(E, sizeof(Mat)); L->eu = xalloc(E, sizeof(Mat)); L->ed = xalloc(E, sizeof(Mat));
        for (size_t e = 0; e < E; e++) {
            L->eg[e] = g; L->eu[e] = u; L->ed[e] = w;
            if (g.q) L->eg[e].q += e * H * g.stride; else L->eg[e].f += e * H * d;
            if (u.q) L->eu[e].q += e * H * u.stride; else L->eu[e].f += e * H * d;
            if (w.q) L->ed[e].q += e * d * w.stride; else L->ed[e].f += e * d * H;
        }
        L->w_gate = L->eg[0]; L->w_up = L->eu[0]; L->w_down = L->ed[0];
        L->sg = q4_mat(m, ts, QN("ffn_gate_shexp.weight"), H, d);
        L->su = q4_mat(m, ts, QN("ffn_up_shexp.weight"), H, d);
        L->sd = q4_mat(m, ts, QN("ffn_down_shexp.weight"), d, H);
        L->shexp_gate = get_tensor(ts, QN("ffn_gate_inp_shexp.weight"), d, 1, np);
        if (l == q->ple_l) {
            const size_t pe = (size_t)(q->ng - 1) * q->hpn * q->pd;
            q->pkey = q4_mat(m, ts, QN("ple_key.weight"), D, pe);
            q->pval = q4_mat(m, ts, QN("ple_value.weight"), d, pe);
            q->pconv = get_tensor(ts, QN("ple_conv1d.weight"), D * q->pk, 1, np);
            q->pn_key = get_tensor(ts, QN("ple_norm_key.weight"), D, 1, np);
            q->pn_q = get_tensor(ts, QN("ple_norm_query.weight"), D, 1, np);
            q->pn_conv = get_tensor(ts, QN("ple_norm_conv.weight"), D, 1, np);
        }
#undef QN
    }
    if (q->n_ple) {     /* a tabela nunca e copiada nem convertida: so o endereco no arquivo mapeado */
        const TInfo *t = q4_find(ts, "per_layer_token_embd.weight");
        if (!t) die("qwen4exp: tabela de n-gramas (per_layer_token_embd) nao encontrada: falta a outra parte do modelo?");
        if (t->ndims != 2 || t->dims[0] != (uint64_t)q->pd) die("qwen4exp: tabela de n-gramas com dimensoes inesperadas");
        q->tab.type = t->type;
        q->tab.stride = qbytes(t->type, q->pd);
        q->tab.q = t->base + t->offset;
        q->tab_rows = t->dims[1];
        if (!q->tab.stride || q->tab_rows > (size_t)(t->lim - q->tab.q) / q->tab.stride) die("qwen4exp: tabela de n-gramas fora do arquivo");
        for (int h = 0; h < q->n_voc; h++)
            if (q->off[h] > q->tab_rows || q->voc[h] > q->tab_rows - q->off[h]) die("qwen4exp: n-gramas fora da tabela");
        *np += (size_t)q->tab_rows * q->pd;
#ifndef _WIN32
        const uintptr_t a = (uintptr_t)q->tab.q & ~(uintptr_t)4095;
        madvise((void *)a, (uintptr_t)q->tab.q + q->tab_rows * q->tab.stride - a, MADV_RANDOM);
#endif
        q->conv = xalloc((size_t)(q->pk - 1) * q->ng * D + 1, sizeof(float));
    }
    if (na) {
        q->iacc = xalloc((size_t)na * q->ihd, sizeof(float));
        q->ikey = xalloc((size_t)na * (c->ctx / q->ratio + 1) * q->ihd, sizeof(float));
    }
}

/* sigmoide com a entrada limitada: com -ffast-math o laco vetorizado divide pela reciproca aproximada com um passo de
   Newton, que da NaN quando expf estoura (x < -88); a saida nao muda, |x| > 60 ja e 0 ou 1 em float */
static inline float q4_sig(float x) {
    x = x < -60.0f ? -60.0f : x;
    x = x > 60.0f ? 60.0f : x;
    return 1.0f / (1.0f + expf(-x));
}
static inline float q4_silu(float x) { return x * q4_sig(x); }

/* ---- produtos em lote: cada linha e aberta uma vez e atende os B tokens ---- */
static float q4_dot_bf16(const uint8_t *row, const float *x, int n) {
    __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
    int j = 0;
    for (; j + 16 <= n; j += 16) {
        const __m256i w = _mm256_loadu_si256((const __m256i *)(row + 2 * j));
        a0 = _mm256_fmadd_ps(_mm256_castsi256_ps(_mm256_slli_epi32(_mm256_cvtepu16_epi32(_mm256_castsi256_si128(w)), 16)), _mm256_loadu_ps(x + j), a0);
        a1 = _mm256_fmadd_ps(_mm256_castsi256_ps(_mm256_slli_epi32(_mm256_cvtepu16_epi32(_mm256_extracti128_si256(w, 1)), 16)), _mm256_loadu_ps(x + j + 8), a1);
    }
    a0 = _mm256_add_ps(a0, a1);
    __m128 lo = _mm_add_ps(_mm256_castps256_ps128(a0), _mm256_extractf128_ps(a0, 1));
    lo = _mm_hadd_ps(lo, lo); lo = _mm_hadd_ps(lo, lo);
    float s = _mm_cvtss_f32(lo);
    for (; j < n; j++) { uint16_t h; memcpy(&h, row + 2 * j, 2); s += bf16_to_f32(h) * x[j]; }
    return s;
}
static void mm_prepare(XQ *xqs, const float *X, int B, int cols);
#if HAVE_LOWBIT
/* Q4_K, Q5_K e Q6_K abertos como nos tipos de poucos bits: 256 inteiros de 16 bits com a escala inteira do sub-bloco ja
   multiplicada e um fator so por bloco; o minimo (Q4_K, Q5_K) entra pela soma das ativacoes de cada 32 (xq.bs32).
   Limites: Q6_K 128 x 32 por produto, 16 produtos por faixa: abaixo de 2^31 com ativacao de 32512. */
static int q4_ktype(uint32_t t) { return t == T_Q4_K || t == T_Q5_K || t == T_Q6_K; }
static void q4_kdec(uint32_t type, const uint8_t *src, int16_t *w, float *f, float *fm, int *mn) {
    const __m256i m4 = _mm256_set1_epi8(15);
    *fm = 0.0f;
    if (type == T_Q6_K) {
        const __m256i m2 = _mm256_set1_epi8(3), o32 = _mm256_set1_epi8(32);
        const int8_t *sc = (const int8_t *)(src + 192);
        *f = h2f(src + 208);
        for (int c = 0; c < 2; c++, w += 128, sc += 8) {
            const __m256i a = _mm256_loadu_si256((const __m256i *)(src + 64 * c)), b = _mm256_loadu_si256((const __m256i *)(src + 64 * c + 32));
            const __m256i h = _mm256_loadu_si256((const __m256i *)(src + 128 + 32 * c));
#define Q4_HI(sh) _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, sh), m2), 4)
            lb_store32(w, _mm256_sub_epi8(_mm256_or_si256(_mm256_and_si256(a, m4), Q4_HI(0)), o32), sc[0], sc[1]);
            lb_store32(w + 32, _mm256_sub_epi8(_mm256_or_si256(_mm256_and_si256(b, m4), Q4_HI(2)), o32), sc[2], sc[3]);
            lb_store32(w + 64, _mm256_sub_epi8(_mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(a, 4), m4), Q4_HI(4)), o32), sc[4], sc[5]);
            lb_store32(w + 96, _mm256_sub_epi8(_mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(b, 4), m4), Q4_HI(6)), o32), sc[6], sc[7]);
#undef Q4_HI
        }
        return;
    }
    const int five = type == T_Q5_K;
    *f = h2f(src); *fm = h2f(src + 2);
    for (int j = 0; j < 8; j += 2) {
        int s1, s2;
        k4_scale_min(j, src + 4, &s1, &mn[j]); k4_scale_min(j + 1, src + 4, &s2, &mn[j + 1]);
        const __m256i v = _mm256_loadu_si256((const __m256i *)(src + (five ? 48 : 16) + 16 * j));
        __m256i lo = _mm256_and_si256(v, m4), hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), m4);
        if (five) {
            const __m256i h = _mm256_loadu_si256((const __m256i *)(src + 16)), one = _mm256_set1_epi8(1);
            lo = _mm256_or_si256(lo, _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, j), one), 4));
            hi = _mm256_or_si256(hi, _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, j + 1), one), 4));
        }
        lb_store32(w + 32 * j, lo, s1, s1);
        lb_store32(w + 32 * j + 32, hi, s2, s2);
    }
}
/* uma linha (tipos K ou de poucos bits) contra ate Q4_TB tokens: cada bloco de 256 pesos e aberto uma vez e passa por todos os
   tokens (a ativacao do bloco, 512 bytes por token, fica na L2); 4 tokens por vez. Mesmos inteiros do lb_row/dot_row */
#define Q4_TB 256
/* Q2_0 e IQ4_NL em 8 bits: o peso vira byte sem sinal (codigo 0..3; valor da tabela + 128) e a ativacao de 16 bits entra como
   alto*256 + baixo (xq.qh/ql): dois dpbusd por 64 pesos, sem abrir em 16 bits. O deslocamento sai pela soma das ativacoes (xq.bs16).
   O inteiro de cada grupo e o mesmo do caminho de 16 bits. So para ate 3 tokens: com mais, o de 16 bits (um produto so por token) ganha. */
#define Q4_VB __attribute__((target("avx2,fma,f16c,avx512f,avx512bw,avx512vl,avx512vnni,avx512vbmi")))
static int q4_vbmi(void) {
    static int v = -1;
    if (v < 0) { __builtin_cpu_init(); v = cpu_vnni && lb_mode() == 3 && __builtin_cpu_supports("avx512vbmi"); }
    return v;
}
Q4_VB static void q4_row8(uint32_t type, const uint8_t *row, const XQ *xqs, int n, int cols, float *out) {
    __m512 acc[Q4_TB];
    for (int t = 0; t < n; t++) acc[t] = _mm512_setzero_ps();
    const int nl = type == T_IQ4_NL;
    const __m512i sh2 = _mm512_set1_epi64(0x0E0C0A0806040200LL), m3 = _mm512_set1_epi8(3), m15 = _mm512_set1_epi8(15);
    const __m512i rep = _mm512_set_epi16(7, 7, 7, 7, 6, 6, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0);
    const __m512i tbl = _mm512_add_epi8(_mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i *)q_iq4nl_vals)), _mm512_set1_epi8((char)128));
    const __m512i z = _mm512_setzero_si512();
    for (int c = 0; c < cols; c += 64) {
        __m512i w; __m512 f;
        if (nl) {
            const __m128i v0 = _mm_loadu_si128((const __m128i *)(row + 2)), v1 = _mm_loadu_si128((const __m128i *)(row + 20));
            const __m512i x = _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_set_m128i(_mm_srli_epi16(v0, 4), v0)), _mm256_set_m128i(_mm_srli_epi16(v1, 4), v1), 1);
            w = _mm512_shuffle_epi8(tbl, _mm512_and_si512(x, m15));
            f = _mm512_mask_mov_ps(_mm512_set1_ps(h2f(row)), 0xFF00, _mm512_set1_ps(h2f(row + 18)));
            row += 36;
        } else {
            w = _mm512_and_si512(_mm512_multishift_epi64_epi8(sh2, _mm512_permutexvar_epi16(rep, _mm512_castsi128_si512(_mm_loadu_si128((const __m128i *)(row + 2))))), m3);
            f = _mm512_set1_ps(h2f(row));
            row += 18;
        }
        const int b = c >> 8, s = c >> 4;
        for (int t = 0; t < n; t++) {
            const XQ *x = xqs + t;
            __m512i p = _mm512_dpbusd_epi32(z, w, _mm512_loadu_si512((const void *)(x->qh + c)));
            p = _mm512_dpbusd_epi32(_mm512_slli_epi32(p, 8), w, _mm512_loadu_si512((const void *)(x->ql + c)));
            const int32_t *bs = x->bs16 + s;
            p = _mm512_sub_epi32(p, nl ? _mm512_mask_set1_epi32(_mm512_maskz_set1_epi32(1, 128 * (bs[0] + bs[1])), 0x100, 128 * (bs[2] + bs[3]))
                                       : _mm512_maskz_set1_epi32(1, bs[0] + bs[1] + bs[2] + bs[3]));
            acc[t] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(p), _mm512_mul_ps(f, _mm512_set1_ps(x->xs[b])), acc[t]);
        }
    }
    for (int t = 0; t < n; t++) out[t] = _mm512_reduce_add_ps(acc[t]);
}
LB_512 static void q4_krow(const Mat *W, const uint8_t *row, const XQ *xqs, int n, int cols, float *out) {
    if (n <= 3 && (W->type == T_Q2_0 || (W->type == T_IQ4_NL && cols % 64 == 0)) && (q4_opt & QO_K8) && q4_vbmi()) { q4_row8(W->type, row, xqs, n, cols, out); return; }
    int16_t w[256] __attribute__((aligned(64)));
    __m512 acc[Q4_TB];
    float corr[Q4_TB], fs[8];
    const int kt = q4_ktype(W->type);
    const LbT *T = kt ? NULL : lb_by_type(W->type);
    const int unit = kt ? 256 : T->unit, fold = kt || T->fold;
    const size_t ub = kt ? (W->type == T_Q6_K ? 210 : W->type == T_Q5_K ? 176 : 144) : T->ub;
    for (int t = 0; t < n; t++) { acc[t] = _mm512_setzero_ps(); corr[t] = 0.0f; }
    for (int c = 0; c < cols; c += 256) {
        const int len = cols - c < 256 ? cols - c : 256, ng = len / 32, b = c >> 8;
        float fm = 0.0f; int mn[8];
        const uint8_t *src = row + (size_t)(c / unit) * ub;
        if (kt) q4_kdec(W->type, src, w, fs, &fm, mn); else T->dec5(src, w, fs, len);
        __m512i wv[8];
        for (int g = 0; g < ng; g++) wv[g] = _mm512_load_si512((const void *)(w + 32 * g));
        int t = 0;
        for (; t + 4 <= n; t += 4) {
            const XQ *x = xqs + t;
            const int16_t *x0 = x[0].q + c, *x1 = x[1].q + c, *x2 = x[2].q + c, *x3 = x[3].q + c;
#define Q4_X(p, g) _mm512_loadu_si512((const void *)((p) + 32 * (g)))
            if (fold) {
                __m512i p0 = _mm512_setzero_si512(), p1 = p0, p2 = p0, p3 = p0;
                for (int g = 0; g < ng; g++) {
                    p0 = _mm512_dpwssd_epi32(p0, wv[g], Q4_X(x0, g)); p1 = _mm512_dpwssd_epi32(p1, wv[g], Q4_X(x1, g));
                    p2 = _mm512_dpwssd_epi32(p2, wv[g], Q4_X(x2, g)); p3 = _mm512_dpwssd_epi32(p3, wv[g], Q4_X(x3, g));
                }
                acc[t] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(p0), _mm512_set1_ps(fs[0] * x[0].xs[b]), acc[t]);
                acc[t + 1] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(p1), _mm512_set1_ps(fs[0] * x[1].xs[b]), acc[t + 1]);
                acc[t + 2] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(p2), _mm512_set1_ps(fs[0] * x[2].xs[b]), acc[t + 2]);
                acc[t + 3] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(p3), _mm512_set1_ps(fs[0] * x[3].xs[b]), acc[t + 3]);
            } else {
                const __m512i z = _mm512_setzero_si512();
                __m512 b0 = _mm512_setzero_ps(), b1 = b0, b2 = b0, b3 = b0;
                for (int g = 0; g < ng; g++) {
                    const __m512 fg = _mm512_set1_ps(fs[g]);
                    b0 = _mm512_fmadd_ps(_mm512_cvtepi32_ps(_mm512_dpwssd_epi32(z, wv[g], Q4_X(x0, g))), fg, b0);
                    b1 = _mm512_fmadd_ps(_mm512_cvtepi32_ps(_mm512_dpwssd_epi32(z, wv[g], Q4_X(x1, g))), fg, b1);
                    b2 = _mm512_fmadd_ps(_mm512_cvtepi32_ps(_mm512_dpwssd_epi32(z, wv[g], Q4_X(x2, g))), fg, b2);
                    b3 = _mm512_fmadd_ps(_mm512_cvtepi32_ps(_mm512_dpwssd_epi32(z, wv[g], Q4_X(x3, g))), fg, b3);
                }
                acc[t] = _mm512_fmadd_ps(b0, _mm512_set1_ps(x[0].xs[b]), acc[t]);
                acc[t + 1] = _mm512_fmadd_ps(b1, _mm512_set1_ps(x[1].xs[b]), acc[t + 1]);
                acc[t + 2] = _mm512_fmadd_ps(b2, _mm512_set1_ps(x[2].xs[b]), acc[t + 2]);
                acc[t + 3] = _mm512_fmadd_ps(b3, _mm512_set1_ps(x[3].xs[b]), acc[t + 3]);
            }
        }
        for (; t < n; t++) {
            const XQ *x = xqs + t;
            if (fold) {
                __m512i p0 = _mm512_setzero_si512();
                for (int g = 0; g < ng; g++) p0 = _mm512_dpwssd_epi32(p0, wv[g], Q4_X(x->q + c, g));
                acc[t] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(p0), _mm512_set1_ps(fs[0] * x->xs[b]), acc[t]);
            } else {
                __m512 b0 = _mm512_setzero_ps();
                for (int g = 0; g < ng; g++) b0 = _mm512_fmadd_ps(_mm512_cvtepi32_ps(_mm512_dpwssd_epi32(_mm512_setzero_si512(), wv[g], Q4_X(x->q + c, g))), _mm512_set1_ps(fs[g]), b0);
                acc[t] = _mm512_fmadd_ps(b0, _mm512_set1_ps(x->xs[b]), acc[t]);
            }
        }
#undef Q4_X
        if (fm != 0.0f)
            for (t = 0; t < n; t++) {
                int64_t sm = 0;
                for (int j = 0; j < 8; j++) sm += (int64_t)mn[j] * xqs[t].bs32[8 * b + j];
                corr[t] += fm * xqs[t].xs[b] * (float)sm;
            }
    }
    for (int t = 0; t < n; t++) out[t] = _mm512_reduce_add_ps(acc[t]) - corr[t];
}
static int q4_tile = 32;     /* tokens por ladrilho nos produtos densos (KATANA_Q4_TILE, no maximo Q4_TB) */
typedef struct { float *out; const Mat *W; XQ *xqs; int B, rows, cols; } Q4Kj;
static void q4_k_job(void *cx, int i0, int i1) {
    const Q4Kj *j = cx;
    float tmp[Q4_TB], loc[Q4_TB * 16];      /* 16 linhas por vez: uma linha de cache por token na gravacao */
    const int tb = q4_tile;
    for (int t0 = 0; t0 < j->B; t0 += tb) {
        const int n = j->B - t0 < tb ? j->B - t0 : tb;
        for (int i = i0; i < i1; i += 16) {
            const int nr = i1 - i < 16 ? i1 - i : 16;
            for (int k = 0; k < nr; k++) {
                q4_krow(j->W, j->W->q + (size_t)(i + k) * j->W->stride, j->xqs + t0, n, j->cols, tmp);
                for (int t = 0; t < n; t++) loc[16 * t + k] = tmp[t];
            }
            for (int t = 0; t < n; t++) memcpy(j->out + (size_t)(t0 + t) * j->rows + i, loc + 16 * t, nr * sizeof(float));
        }
    }
}
static int q4_kern_ok(const Mat *W) { return (q4_opt & QO_KERN) && lb_mode() == 3 && mat_int(W) && (q4_ktype(W->type) || lowbit_ok(W->type)); }
/* BF16: nt tokens (1 a 4) contra 2 linhas; a conversao da linha serve todos */
LB_512 static void q4_bf2x4(const uint8_t *r0, const uint8_t *r1, const float *x, size_t xs, int nt, int n, float *o0, float *o1) {
    __m512 a[4], b[4];
    for (int t = 0; t < nt; t++) a[t] = b[t] = _mm512_setzero_ps();
    int j = 0;
#define Q4_CV(p) _mm512_castsi512_ps(_mm512_slli_epi32(_mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(p))), 16))
    if (nt == 4)
        for (; j + 16 <= n; j += 16) {
            const __m512 w0 = Q4_CV(r0 + 2 * j), w1 = Q4_CV(r1 + 2 * j);
            const __m512 x0 = _mm512_loadu_ps(x + j), x1 = _mm512_loadu_ps(x + xs + j), x2 = _mm512_loadu_ps(x + 2 * xs + j), x3 = _mm512_loadu_ps(x + 3 * xs + j);
            a[0] = _mm512_fmadd_ps(w0, x0, a[0]); a[1] = _mm512_fmadd_ps(w0, x1, a[1]); a[2] = _mm512_fmadd_ps(w0, x2, a[2]); a[3] = _mm512_fmadd_ps(w0, x3, a[3]);
            b[0] = _mm512_fmadd_ps(w1, x0, b[0]); b[1] = _mm512_fmadd_ps(w1, x1, b[1]); b[2] = _mm512_fmadd_ps(w1, x2, b[2]); b[3] = _mm512_fmadd_ps(w1, x3, b[3]);
        }
    else
        for (; j + 16 <= n; j += 16) {
            const __m512 w0 = Q4_CV(r0 + 2 * j), w1 = Q4_CV(r1 + 2 * j);
            for (int t = 0; t < nt; t++) {
                const __m512 xv = _mm512_loadu_ps(x + t * xs + j);
                a[t] = _mm512_fmadd_ps(w0, xv, a[t]); b[t] = _mm512_fmadd_ps(w1, xv, b[t]);
            }
        }
    for (int t = 0; t < nt; t++) {
        o0[t] = _mm512_reduce_add_ps(a[t]); o1[t] = _mm512_reduce_add_ps(b[t]);
        for (int k = j; k < n; k++) {
            uint16_t h; memcpy(&h, r0 + 2 * k, 2); o0[t] += bf16_to_f32(h) * x[t * xs + k];
            memcpy(&h, r1 + 2 * k, 2); o1[t] += bf16_to_f32(h) * x[t * xs + k];
        }
    }
}
/* uma linha e um token: 4 acumuladores (a latencia do fma nao limita) */
LB_512 static float q4_bf1(const uint8_t *r, const float *x, int n) {
    __m512 a0 = _mm512_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
    int j = 0;
    for (; j + 64 <= n; j += 64) {
        a0 = _mm512_fmadd_ps(Q4_CV(r + 2 * j), _mm512_loadu_ps(x + j), a0); a1 = _mm512_fmadd_ps(Q4_CV(r + 2 * j + 32), _mm512_loadu_ps(x + j + 16), a1);
        a2 = _mm512_fmadd_ps(Q4_CV(r + 2 * j + 64), _mm512_loadu_ps(x + j + 32), a2); a3 = _mm512_fmadd_ps(Q4_CV(r + 2 * j + 96), _mm512_loadu_ps(x + j + 48), a3);
    }
    for (; j + 16 <= n; j += 16) a0 = _mm512_fmadd_ps(Q4_CV(r + 2 * j), _mm512_loadu_ps(x + j), a0);
    float s = _mm512_reduce_add_ps(_mm512_add_ps(_mm512_add_ps(a0, a1), _mm512_add_ps(a2, a3)));
    for (; j < n; j++) { uint16_t h; memcpy(&h, r + 2 * j, 2); s += bf16_to_f32(h) * x[j]; }
    return s;
}
#undef Q4_CV
typedef struct { float *out; const Mat *W; const float *X; int B, rows, cols; } Q4Bj;
static void q4_bf_job(void *cx, int i0, int i1) {
    const Q4Bj *j = cx;
    float o0[4], o1[4];
    if (j->B == 1) { for (int i = i0; i < i1; i++) j->out[i] = q4_bf1(j->W->q + (size_t)i * j->W->stride, j->X, j->cols); return; }
    for (int t = 0; t < j->B; t += 4) {
        const int nt = j->B - t < 4 ? j->B - t : 4;
        for (int i = i0; i < i1; i += 2) {
            const int i2 = i + 1 < i1 ? i + 1 : i;
            q4_bf2x4(j->W->q + (size_t)i * j->W->stride, j->W->q + (size_t)i2 * j->W->stride, j->X + (size_t)t * j->cols, j->cols, nt, j->cols, o0, o1);
            for (int k = 0; k < nt; k++) { j->out[(size_t)(t + k) * j->rows + i] = o0[k]; j->out[(size_t)(t + k) * j->rows + i2] = o1[k]; }
        }
    }
}
#else
static int q4_kern_ok(const Mat *W) { (void)W; return 0; }
#define Q4_TB LB_MAXB
#endif
static __thread float *q4_row; static __thread int q4_rcap;
/* linha r de W em float: a propria (float32) ou aberta na memoria da thread */
static const float *q4_open(const Mat *W, size_t r, int cols) {
    if (W->f) return W->f + r * cols;
    if (q4_rcap < cols) { free(q4_row); q4_row = xalloc(cols, sizeof(float)); q4_rcap = cols; }
    dequant(W->type, W->q + r * W->stride, q4_row, cols);
    return q4_row;
}
typedef struct { float *out; const Mat *W; const float *X; int B, rows, cols; } Q4Mm;
static void q4_mm_job(void *cx, int i0, int i1) {
    const Q4Mm *j = cx;
    for (int i = i0; i < i1; i++) {
        if (j->W->q && j->W->type == T_BF16) {
            for (int t = 0; t < j->B; t++) j->out[(size_t)t * j->rows + i] = q4_dot_bf16(j->W->q + (size_t)i * j->W->stride, j->X + (size_t)t * j->cols, j->cols);
            continue;
        }
        const float *w = q4_open(j->W, i, j->cols);
        for (int t = 0; t < j->B; t++) j->out[(size_t)t * j->rows + i] = dot(w, j->X + (size_t)t * j->cols, j->cols);
    }
}
/* out [B][rows] = W X [B][cols]; tipos com produto inteiro usam o caminho comum (xqs e prepared como no matmat) */
static void q4_mm(float *out, const Mat *W, const float *X, XQ *xqs, int *prep, int B, int rows, int cols) {
    int chunk = rows / (pool.n * 8 + 1), minc = 32768 / ((long)cols * B) + 1;
#if HAVE_LOWBIT
    if (q4_kern_ok(W) && (B > 1 || !q4_ktype(W->type))) {      /* um token: o produto VNNI dos tipos K e mais rapido */
        if (!*prep) { mm_prepare(xqs, X, B, cols); *prep = 1; }
        Q4Kj k = { out, W, xqs, B, rows, cols };
        if (chunk > 64) chunk = 64;
        pool_for(q4_k_job, &k, rows, chunk > minc ? chunk : minc);
        return;
    }
    if ((q4_opt & QO_KERN) && W->q && W->type == T_BF16 && lb_mode() == 3 && !cpu_exact) {
        Q4Bj k = { out, W, X, B, rows, cols };
        const int lim = 524288 / (cols * 2) + 1;        /* linhas por bloco: cabem na L2 enquanto os tokens passam */
        if (chunk > lim) chunk = lim;
        if (chunk < minc) chunk = minc;
        pool_for(q4_bf_job, &k, rows, (chunk + 1) & ~1);
        return;
    }
#endif
    if (mat_int(W)) { matmat(out, W, X, xqs, prep, B, rows, cols); return; }
    Q4Mm j = { out, W, X, B, rows, cols };
    pool_for(q4_mm_job, &j, rows, chunk > minc ? chunk : minc);
}

/* ---- residual em ramos ---- */
static void q4_rms_groups(float *out, const float *x, const float *g, int R, int d, float eps) {
    for (int r = 0; r < R; r++) rmsnorm(out + (size_t)r * d, x + (size_t)r * d, g + (size_t)r * d, d, eps);
}
typedef struct { const Q4 *q; const Q4Hc *h; const float *X, *DN; float *N, *U, *out, *IN; int d; float eps; int fase; } Q4Hj;
static void q4_hc_job(void *cx, int t0, int t1) {
    const Q4Hj *j = cx;
    const int R = j->q->R, d = j->d;
    const size_t D = (size_t)R * d;
    for (int t = t0; t < t1; t++) {
        float *N = j->N + t * D;
        if (j->fase == 0) { q4_rms_groups(N, j->X + t * D, j->h->norm, R, d, j->eps); continue; }
        if (j->fase == 2) { float *dn = (float *)j->DN + (size_t)t * j->q->rank; for (int i = 0; i < j->q->rank; i++) dn[i] = q4_silu(dn[i] / R); continue; }
        const float *U = j->U + t * D;
        float *o = j->out + (size_t)t * d;
        memset(o, 0, d * sizeof(float));
        for (int r = 0; r < R; r++)
            for (int i = 0; i < d; i++) o[i] += N[(size_t)r * d + i] * q4_sig(U[(size_t)r * d + i]);
        for (int i = 0; i < d; i++) o[i] /= R;
    }
}
static void *q4_scr(int id, size_t n) {       /* rascunhos que so crescem */
    static float *p[32]; static size_t cap[32];
    if (cap[id] < n) { free(p[id]); p[id] = xalloc(n, sizeof(float)); cap[id] = n; }
    return p[id];
}
/* out [B][d]: entrada do bloco; inj [B][R]: peso da saida em cada ramo (NULL no mixer final) */
static void q4_hc_read(Model *m, const Q4Hc *h, const float *X, int B, float *out, float *inj) {
    const Q4 *q = m->q4;
    const int d = m->cfg.dim, R = q->R, D = R * d;
    static XQ xqs[Q4_BMAX];
    float *N = q4_scr(0, (size_t)B * D), *DN = q4_scr(1, (size_t)B * q->rank), *U = q4_scr(2, (size_t)B * D);
    double tk = q4_tick(-1, 0);
    Q4Hj j = { q, h, X, DN, N, U, out, inj, d, m->cfg.eps, 0 };
    pool_for(q4_hc_job, &j, B, 1);
    int prep = 0;
    q4_mm(DN, &h->down, N, xqs, &prep, B, q->rank, D);
    if (inj) {
        q4_mm(inj, &h->inj, N, xqs, &prep, B, R, D);
        for (int i = 0; i < B * R; i++) inj[i] = 2.0f * q4_sig(inj[i] / R);
    }
    if ((q4_opt & QO_PAR) && B > 1) { j.fase = 2; pool_for(q4_hc_job, &j, B, 8); }
    else for (int i = 0; i < B * q->rank; i++) DN[i] = q4_silu(DN[i] / R);
    prep = 0;
    q4_mm(U, &h->up, DN, xqs, &prep, B, D, q->rank);
    j.fase = 1;
    pool_for(q4_hc_job, &j, B, 1);
    q4_tick(QP_HCR, tk);
}
typedef struct { const Q4 *q; float *X; const float *y, *inj; int d; } Q4Wj;
static void q4_hcw_job(void *cx, int t0, int t1) {
    const Q4Wj *j = cx;
    const int d = j->d, R = j->q->R;
    for (int t = t0; t < t1; t++)
        for (int r = 0; r < R; r++) {
            float *x = j->X + ((size_t)t * R + r) * d;
            const float *o = j->y + (size_t)t * d, w = j->inj[t * R + r];
            for (int i = 0; i < d; i++) x[i] += w * o[i];
        }
}
static void q4_hc_write(const Q4 *q, float *X, const float *y, const float *inj, int B, int d) {
    Q4Wj j = { q, X, y, inj, d };
    if (q4_opt & QO_PAR) pool_for(q4_hcw_job, &j, B, 2); else q4_hcw_job(&j, 0, B);
}

/* ---- n-gramas ----
   h guarda os ng-1 tokens anteriores seguidos dos n do lote; ids [n][(ng-1)*hpn] = linhas da tabela. O token a `s` posicoes
   so vale se nenhum fim de texto (eos) fica entre ele e o atual; senao entra o proprio eos. */
static void q4_ngram_ids(const Q4 *q, const int *toks, int n, uint64_t *ids) {
    const int cl = q->ng - 1, nh = cl * q->hpn;
    for (int t = 0; t < n; t++) {
        uint64_t sh[8];
        int cut = 0;
        for (int s = 0; s < q->ng; s++) {
            const int p = t - s;
            const uint64_t tok = (uint64_t)(p >= 0 ? toks[p] : q->hist[cl + p]);
            sh[s] = cut ? q->eos : tok;
            if (s > 0 && tok == q->eos) cut = 1;
        }
        uint64_t mix = sh[0] * q->mult[0];
        for (int g = 2; g <= q->ng; g++) {
            mix ^= sh[g - 1] * q->mult[g - 1];
            for (int i = 0; i < q->hpn; i++) {
                const int h = (g - 2) * q->hpn + i;
                ids[(size_t)t * nh + h] = mix % q->voc[h] + q->off[h];
            }
        }
    }
}
static int q4_cmp64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
/* pede ao sistema, de uma vez e em ordem, as paginas das linhas de n tokens (o disco atende em paralelo) */
static void q4_ngram_prefetch(const Q4 *q, const int *toks, int n) {
#ifndef _WIN32
    if (!q->n_ple) return;
    const int nh = (q->ng - 1) * q->hpn;
    uint64_t *ids = xalloc((size_t)n * nh, 8), *pg = xalloc((size_t)n * nh * 2, 8);
    size_t np = 0;
    q4_ngram_ids(q, toks, n, ids);
    for (size_t i = 0; i < (size_t)n * nh; i++) {
        const uint64_t a = (uint64_t)(uintptr_t)q->tab.q + ids[i] * q->tab.stride, p0 = a >> 12, p1 = (a + q->tab.stride - 1) >> 12;
        pg[np++] = p0;
        if (p1 != p0) pg[np++] = p1;
    }
    qsort(pg, np, 8, q4_cmp64);
    for (size_t i = 0; i < np;) {
        size_t j = i + 1;
        while (j < np && pg[j] <= pg[j - 1] + 1) j++;
        madvise((void *)(uintptr_t)(pg[i] << 12), (size_t)(pg[j - 1] - pg[i] + 1) << 12, MADV_WILLNEED);
        i = j;
    }
    free(ids); free(pg);
#else
    (void)q; (void)toks; (void)n;
#endif
}
typedef struct { const Q4 *q; const uint64_t *ids; float *e; } Q4Ej;
static void q4_emb_job(void *cx, int i0, int i1) {
    const Q4Ej *j = cx;
    for (int i = i0; i < i1; i++) dequant(j->q->tab.type, j->q->tab.q + j->ids[i] * j->q->tab.stride, j->e + (size_t)i * j->q->pd, j->q->pd);
}
typedef struct { const Q4 *q; const float *K, *V, *X; float *G, *N; int d; float eps; } Q4Pj;
static void q4_ple_job(void *cx, int t0, int t1) {
    const Q4Pj *j = cx;
    const int R = j->q->R, d = j->d;
    const size_t D = (size_t)R * d;
    float *kn = xalloc(D, 4), *qn = xalloc(D, 4);
    for (int t = t0; t < t1; t++) {
        q4_rms_groups(kn, j->K + t * D, j->q->pn_key, R, d, j->eps);
        q4_rms_groups(qn, j->X + t * D, j->q->pn_q, R, d, j->eps);
        for (int r = 0; r < R; r++) {
            float g = dot(kn + (size_t)r * d, qn + (size_t)r * d, d) / sqrtf((float)d), a = fabsf(g);
            g = g == 0.0f ? 0.0f : sqrtf(a < 1e-6f ? 1e-6f : a) * (g < 0.0f ? -1.0f : 1.0f);
            const float s = q4_sig(g);
            for (int i = 0; i < d; i++) j->G[t * D + (size_t)r * d + i] = s * j->V[(size_t)t * d + i];
        }
        q4_rms_groups(j->N + t * D, j->G + t * D, j->q->pn_conv, R, d, j->eps);
    }
    free(kn); free(qn);
}
typedef struct { const Q4 *q; float *X; const float *G, *W; int D; } Q4Cj;
static void q4_plec_job(void *cx, int t0, int t1) {
    const Q4Cj *j = cx;
    const Q4 *q = j->q;
    const int D = j->D;
    for (int t = t0; t < t1; t++) {
        float *x = j->X + (size_t)t * D;
        const float *g = j->G + (size_t)t * D;
        for (int i = 0; i < D; i++) {
            const float *w = q->pconv + (size_t)i * q->pk;
            float y = 0.0f;
            for (int k = 0; k < q->pk; k++) y += w[k] * j->W[(size_t)(t + k * q->ng) * D + i];
            x[i] += g[i] + q4_silu(y);
        }
    }
}
static void q4_ple(Model *m, const int *toks, float *X, int B) {
    const Config *c = &m->cfg;
    Q4 *q = m->q4;
    const int d = c->dim, D = q->R * d, nh = (q->ng - 1) * q->hpn, pe = nh * q->pd, S = (q->pk - 1) * q->ng;
    static XQ xqs[Q4_BMAX];
    static uint64_t ids[Q4_BMAX * 64];
    float *e = q4_scr(3, (size_t)B * pe), *K = q4_scr(4, (size_t)B * D), *V = q4_scr(5, (size_t)B * d), *G = q4_scr(6, (size_t)B * D);
    float *W = q4_scr(7, (size_t)(S + B) * D);        /* janela da convolucao: as S entradas anteriores e as B novas */
    q4_ngram_ids(q, toks, B, ids);
    Q4Ej ej = { q, ids, e };
    pool_for(q4_emb_job, &ej, B * nh, 4);
    int prep = 0;
    q4_mm(K, &q->pkey, e, xqs, &prep, B, D, pe);
    q4_mm(V, &q->pval, e, xqs, &prep, B, d, pe);
    memcpy(W, q->conv, (size_t)S * D * sizeof(float));
    Q4Pj pj = { q, K, V, X, G, W + (size_t)S * D, d, c->eps };
    pool_for(q4_ple_job, &pj, B, 1);
    Q4Cj cj = { q, X, G, W, D };
    if (q4_opt & QO_PAR) pool_for(q4_plec_job, &cj, B, 1); else q4_plec_job(&cj, 0, B);
    memcpy(q->conv, W + (size_t)B * D, (size_t)S * D * sizeof(float));
}

/* ---- DeltaNet: como no qwen35, com portao sigmoide na saida ---- */
static void q4_dn_gate(const Config *c, const Layer *L, float *o, const float *z) {
    const int dv = c->ssm_inner / c->ssm_rank;
    for (int h = 0; h < c->ssm_rank; h++) {
        float *p = o + (size_t)h * dv;
        const float *zh = z + (size_t)h * dv;
        rmsnorm(p, p, L->ssm_norm, dv, c->eps);
        for (int i = 0; i < dv; i++) p[i] *= q4_sig(zh[i]);
    }
}

/* lote: convolucao por faixa de canais, normas e portao por token, recorrencia por cabeca com todos os tokens seguidos (o estado
   da cabeca, 64 KB, fica na cache do nucleo). Mesmas contas do dn_conv, dn_recur e q4_dn_gate. */
typedef struct { const Config *c; const Layer *L; float *P, *P2, *cv, *S; const float *AL, *BE, *Z; float *O; int B, C, fase; } Q4Dj;
#if HAVE_LOWBIT
LB_512 static void q4_dn_step(float *S, const float *q, const float *k, const float *v, float decay, float beta, float *o, float *kv, int dk, int dv) {
    const __m512 vd = _mm512_set1_ps(decay), vb = _mm512_set1_ps(beta);
    for (int j = 0; j < dv; j += 16) { _mm512_storeu_ps(kv + j, _mm512_setzero_ps()); _mm512_storeu_ps(o + j, _mm512_setzero_ps()); }
    for (int i = 0; i < dk; i++) {
        float *Si = S + (size_t)i * dv;
        const __m512 ki = _mm512_set1_ps(k[i]);
        for (int j = 0; j < dv; j += 16) {
            const __m512 x = _mm512_mul_ps(_mm512_loadu_ps(Si + j), vd);
            _mm512_storeu_ps(Si + j, x);
            _mm512_storeu_ps(kv + j, _mm512_fmadd_ps(x, ki, _mm512_loadu_ps(kv + j)));
        }
    }
    for (int j = 0; j < dv; j += 16) _mm512_storeu_ps(kv + j, _mm512_mul_ps(_mm512_sub_ps(_mm512_loadu_ps(v + j), _mm512_loadu_ps(kv + j)), vb));
    for (int i = 0; i < dk; i++) {
        float *Si = S + (size_t)i * dv;
        const __m512 ki = _mm512_set1_ps(k[i]), qi = _mm512_set1_ps(q[i]);
        for (int j = 0; j < dv; j += 16) {
            const __m512 x = _mm512_fmadd_ps(ki, _mm512_loadu_ps(kv + j), _mm512_loadu_ps(Si + j));
            _mm512_storeu_ps(Si + j, x);
            _mm512_storeu_ps(o + j, _mm512_fmadd_ps(x, qi, _mm512_loadu_ps(o + j)));
        }
    }
}
#endif
static void q4_dn_job(void *cx, int i0, int i1) {
    const Q4Dj *j = cx;
    const Config *c = j->c;
    const int K = c->ssm_conv, C = j->C, dk = c->ssm_state, ng = c->ssm_groups, nv = c->ssm_rank, dv = c->ssm_inner / nv, B = j->B;
    if (j->fase == 0) {         /* canais i0..i1 em blocos de 64: P (cru) -> P2 (convolvido, SiLU) */
        for (int t = 0; t < B; t++) {
            float *y = j->P2 + (size_t)t * C;
            for (int k = 0; k < K; k++) {
                const int u = t - (K - 1 - k);
                const float *x = u >= 0 ? j->P + (size_t)u * C : j->cv + (size_t)(K - 1 + u) * C;
                if (k == 0) for (int ch = i0 * 64; ch < i1 * 64 && ch < C; ch++) y[ch] = j->L->conv_w[(size_t)ch * K] * x[ch];
                else for (int ch = i0 * 64; ch < i1 * 64 && ch < C; ch++) y[ch] += j->L->conv_w[(size_t)ch * K + k] * x[ch];
            }
            for (int ch = i0 * 64; ch < i1 * 64 && ch < C; ch++) y[ch] = q4_silu(y[ch]);
        }
        return;
    }
    if (j->fase == 1) {         /* normas de q e k, por token */
        for (int t = i0; t < i1; t++)
            for (int g = 0; g < 2 * ng; g++) {
                float *p = j->P2 + (size_t)t * C + (size_t)g * dk;
                const float inv = 1.0f / sqrtf(dot(p, p, dk) + c->eps) * (g < ng ? 1.0f / sqrtf((float)dk) : 1.0f);
                for (int i = 0; i < dk; i++) p[i] *= inv;
            }
        return;
    }
    if (j->fase == 2) {         /* recorrencia, por cabeca */
        const int kd = ng * dk;
        for (int h = i0; h < i1; h++)
            for (int t = 0; t < B; t++) {
                const float *qk = j->P2 + (size_t)t * C;
#if HAVE_LOWBIT
                if (lb_mode() == 3 && dv % 16 == 0 && dn_tile == 1) {
                    float kv[512] __attribute__((aligned(64)));
                    const int kg = h % ng;
                    const float a = j->AL[(size_t)t * nv + h] + j->L->dt_bias[h], sp = a > 20.0f ? a : log1pf(expf(a));
                    q4_dn_step(j->S + (size_t)h * dk * dv, qk + (size_t)kg * dk, qk + kd + (size_t)kg * dk, qk + 2 * kd + (size_t)h * dv,
                               expf(j->L->ssm_a[h] * sp), q4_sig(j->BE[(size_t)t * nv + h]), j->O + (size_t)t * c->ssm_inner + (size_t)h * dv, kv, dk, dv);
                    continue;
                }
#endif
                DnJob jj = { c, j->L, j->S, qk, qk + kd, qk + 2 * kd, j->AL + (size_t)t * nv, j->BE + (size_t)t * nv, j->O + (size_t)t * c->ssm_inner };
                dn_job(&jj, h, h + 1);
            }
        return;
    }
    for (int t = i0; t < i1; t++) q4_dn_gate(c, j->L, j->O + (size_t)t * c->ssm_inner, j->Z + (size_t)t * c->ssm_inner);
}
static void q4_dn(Model *m, const Layer *L, float *P, const float *Z, const float *AL, const float *BE, float *O, int B) {
    const Config *c = &m->cfg;
    const int C = (int)dn_channels(c), K = c->ssm_conv;
    if (dn_tile < 0) dn_tile = getenv("KATANA_DN_TILE") ? atoi(getenv("KATANA_DN_TILE")) : 1;
    if (c->ssm_inner / c->ssm_rank > 512) die("DeltaNet: cabeca v larga demais");
    float *cv = dn_CV(m, L), *P2 = q4_scr(28, (size_t)B * C);
    Q4Dj j = { c, L, P, P2, cv, dn_S(m, L), AL, BE, Z, O, B, C, 0 };
    pool_for(q4_dn_job, &j, (C + 63) / 64, 4);
    for (int k = 0; k < K - 1; k++) {   /* as ultimas K-1 entradas cruas; com B < K-1 as antigas andam (em ordem) */
        const int u = B - (K - 1 - k);
        memmove(cv + (size_t)k * C, u >= 0 ? P + (size_t)u * C : cv + (size_t)(K - 1 + u) * C, C * sizeof(float));
    }
    j.fase = 1; pool_for(q4_dn_job, &j, B, 4);
    j.fase = 2; pool_for(q4_dn_job, &j, c->ssm_rank, 1);
    j.fase = 3; pool_for(q4_dn_job, &j, B, 1);
}

/* ---- atencao esparsa ---- */
static void q4_rope(const Model *m, float *p, int pos) {
    const int half = m->cfg.rope_dim / 2;
    for (int i = 0; i < half; i++) {
        const float ang = pos * m->rope_freq[i], cs = cosf(ang), sn = sinf(ang), a = p[i], b = p[half + i];
        p[i] = a * cs - b * sn; p[half + i] = a * sn + b * cs;
    }
}
/* escolhe os top blocos: notas em s[0..n), marca em mk; empate fica com o bloco mais antigo */
static void q4_top(const float *s, int n, int top, uint8_t *mk) {
    int *hp = xalloc(top, sizeof(int)), hn = 0;        /* monte com o pior na raiz */
#define Q4_WORSE(a, b) (s[a] < s[b] || (s[a] == s[b] && (a) > (b)))
    memset(mk, 0, n);
    for (int i = 0; i < n; i++) {
        int k;
        if (hn < top) {
            k = hn++;
            hp[k] = i;
            while (k > 0 && Q4_WORSE(hp[k], hp[(k - 1) / 2])) { const int t = hp[k]; hp[k] = hp[(k - 1) / 2]; hp[(k - 1) / 2] = t; k = (k - 1) / 2; }
            continue;
        }
        if (!Q4_WORSE(hp[0], i)) continue;
        hp[0] = i; k = 0;
        for (;;) {
            int w = k;
            for (int ch = 2 * k + 1; ch <= 2 * k + 2 && ch < hn; ch++) if (Q4_WORSE(hp[ch], hp[w])) w = ch;
            if (w == k) break;
            const int t = hp[k]; hp[k] = hp[w]; hp[w] = t; k = w;
        }
    }
#undef Q4_WORSE
    for (int i = 0; i < hn; i++) mk[hp[i]] = 1;
    free(hp);
}
typedef struct {
    Model *m; const Layer *L; const Q4L *Q;
    const float *IQ, *q, *qg; float *O; int *sel, *nsel; int pos0, scap;
} Q4Aj;
/* um item por token: posicoes que a consulta ve, em ordem */
static void q4_sel_job(void *cx, int t0, int t1) {
    const Q4Aj *j = cx;
    Model *m = j->m;
    const Q4 *q = m->q4;
    const int top = q->ibudget / q->ratio, hd = q->ihd;
    const float *keys = q->ikey + (size_t)j->Q->ia * (m->cfg.ctx / q->ratio + 1) * hd;
    float *qq = xalloc((size_t)q->in * hd, 4);
    for (int t = t0; t < t1; t++) {
        const int n = j->pos0 + t + 1, nb = n / q->ratio;
        int *sel = j->sel + (size_t)t * j->scap, ns = 0;
        if (nb <= top) { for (int p = 0; p < n; p++) sel[ns++] = p; }
        else {
            float *s = xalloc(nb, 4);
            uint8_t *mk = xalloc(nb, 1);
            for (int h = 0; h < q->in; h++) {
                rmsnorm(qq + h * hd, j->IQ + ((size_t)t * q->in + h) * hd, j->Q->iqn, hd, m->cfg.eps);
                q4_rope(m, qq + h * hd, n - 1);
            }
            for (int b = 0; b < nb; b++)
                for (int h = 0; h < q->in; h++) { const float v = dot(qq + h * hd, keys + (size_t)b * hd, hd); if (v > 0.0f) s[b] += v; }
            q4_top(s, nb, top, mk);
            for (int b = 0; b < nb; b++) if (mk[b]) for (int p = 0; p < q->ratio; p++) sel[ns++] = b * q->ratio + p;
            for (int p = nb * q->ratio; p < n; p++) sel[ns++] = p;
            free(s); free(mk);
        }
        j->nsel[t] = ns;
    }
    free(qq);
}
/* um item por (token, cabeca) */
static void q4_att_job(void *cx, int i0, int i1) {
    const Q4Aj *j = cx;
    const Model *m = j->m;
    const Config *c = &m->cfg;
    const int nh = c->n_heads, hd = c->hd, kvd = c->n_kv * hd, group = nh / c->n_kv;
    const float scale = c->kq_mul / sqrtf((float)hd);
    float *sc = xalloc(j->scap, 4), *row = xalloc(hd, 4);
    for (int it = i0; it < i1; it++) {
        const int t = it / nh, h = it % nh, ns = j->nsel[t];
        const int *sel = j->sel + (size_t)t * j->scap;
        const size_t base = j->L->kv_off + (size_t)(h / group) * hd;
        const float *qh = j->q + (size_t)it * hd, *gt = j->qg + (size_t)it * 2 * hd + hd;
        float *o = j->O + (size_t)it * hd;
        for (int i = 0; i < ns; i++) {
            const size_t r = base + (size_t)sel[i] * kvd;
            if (m->kq8) { q8_get(row, m->kq8 + r / 32 * 34, hd); sc[i] = dot(qh, row, hd) * scale; }
            else sc[i] = (m->kh ? dot_h(qh, m->kh + r, hd) : dot(qh, m->kcache + r, hd)) * scale;
        }
        softmax(sc, ns);
        memset(o, 0, hd * sizeof(float));
        for (int i = 0; i < ns; i++) {
            const size_t r = base + (size_t)sel[i] * kvd;
            const float w = sc[i];
            const float *v = row;
            if (m->vq8) q8_get(row, m->vq8 + r / 32 * 34, hd);
            else if (m->vh) { for (int k = 0; k < hd; k += 8) _mm256_storeu_ps(row + k, _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(m->vh + r + k)))); }
            else v = m->vcache + r;
            for (int k = 0; k < hd; k++) o[k] += w * v[k];
        }
        for (int k = 0; k < hd; k++) o[k] *= q4_sig(gt[k]);
    }
    free(sc); free(row);
}
typedef struct { const Model *m; const Layer *L; const float *QG; float *qq, *K; int pos0; } Q4Nj;
static void q4_attn_job(void *cx, int t0, int t1) {
    const Q4Nj *j = cx;
    const Config *c = &j->m->cfg;
    const int nh = c->n_heads, hd = c->hd, kvd = c->n_kv * hd;
    for (int t = t0; t < t1; t++) {
        const int pos = j->pos0 + t;
        for (int h = 0; h < nh; h++) {
            rmsnorm(j->qq + ((size_t)t * nh + h) * hd, j->QG + ((size_t)t * nh + h) * 2 * hd, j->L->q_norm, hd, c->eps);
            q4_rope(j->m, j->qq + ((size_t)t * nh + h) * hd, pos);
        }
        float *k = j->K + (size_t)t * kvd;
        for (int h = 0; h < c->n_kv; h++) { rmsnorm(k + h * hd, k + h * hd, j->L->k_norm, hd, c->eps); q4_rope(j->m, k + h * hd, pos); }
    }
}
/* nucleo: QG/K/V ja projetados -> O [B][qd]; prep: se Xb ja esta quantizado em xqs (NULL = nao) */
static void q4_att_core(Model *m, int l, const float *Xb, const float *QG, float *K, const float *V, float *O, int pos0, int B, XQ *xqs, int *prep_) {
    const Config *c = &m->cfg;
    Q4 *q = m->q4;
    const Layer *L = &m->layers[l];
    const Q4L *Q = &q->l[l];
    const int d = c->dim, nh = c->n_heads, hd = c->hd, kvd = c->n_kv * hd, qd = nh * hd, ih = q->ihd;
    if (hd % 8) die("qwen4exp: largura de cabeca invalida");
    float *qq = q4_scr(12, (size_t)B * qd), *IQ = q4_scr(13, (size_t)B * q->in * ih), *IK = q4_scr(14, (size_t)B * ih);
    int prep0 = 0, *prep = prep_ ? prep_ : &prep0;
    double tk = q4_tick(-1, 0);
    q4_mm(IQ, &Q->iq, Xb, xqs, prep, B, q->in * ih, d);
    q4_mm(IK, &Q->ik, Xb, xqs, prep, B, ih, d);
    Q4Nj nj = { m, L, QG, qq, K, pos0 };
    if (q4_opt & QO_PAR) pool_for(q4_attn_job, &nj, B, 1); else q4_attn_job(&nj, 0, B);
    for (int t = 0; t < B; t++) {
        const int pos = pos0 + t;
        float *k = K + (size_t)t * kvd;
        const float *v = V + (size_t)t * kvd;
        const size_t r = L->kv_off + (size_t)pos * kvd;
        if (m->kq8) { q8_put(m->kq8 + r / 32 * 34, k, kvd); q8_put(m->vq8 + r / 32 * 34, v, kvd); }
        else if (m->kh) for (int i = 0; i < kvd; i += 8) {
            _mm_storeu_si128((__m128i *)(m->kh + r + i), _mm256_cvtps_ph(_mm256_loadu_ps(k + i), 0));
            _mm_storeu_si128((__m128i *)(m->vh + r + i), _mm256_cvtps_ph(_mm256_loadu_ps(v + i), 0));
        } else { memcpy(m->kcache + r, k, kvd * sizeof(float)); memcpy(m->vcache + r, v, kvd * sizeof(float)); }
        /* indice: soma as k cruas do bloco; bloco completo vira chave (media, norma, RoPE da primeira posicao) */
        float *acc = q->iacc + (size_t)Q->ia * ih;
        if (pos % q->ratio == 0) memset(acc, 0, ih * sizeof(float));
        for (int i = 0; i < ih; i++) acc[i] += IK[(size_t)t * ih + i];
        if (pos % q->ratio == q->ratio - 1) {
            float *key = q->ikey + ((size_t)Q->ia * (c->ctx / q->ratio + 1) + pos / q->ratio) * ih;
            for (int i = 0; i < ih; i++) key[i] = acc[i] / q->ratio;
            rmsnorm(key, key, Q->ikn, ih, c->eps);
            q4_rope(m, key, pos - (q->ratio - 1));
        }
    }
    tk = q4_tick(QP_ATI, tk);
    int scap = q->ibudget + q->ratio;
    if (pos0 + B < scap) scap = pos0 + B;
    static int *sel; static size_t sel_n; static int nsel[Q4_BMAX];
    if (sel_n < (size_t)B * scap) { free(sel); sel_n = (size_t)B * scap; sel = xalloc(sel_n, sizeof(int)); }
    Q4Aj j = { m, L, Q, IQ, qq, QG, O, sel, nsel, pos0, scap };
    pool_for(q4_sel_job, &j, B, 1);
    pool_for(q4_att_job, &j, B * nh, 1);
    q4_tick(QP_ATC, tk);
}
static void q4_att(Model *m, int l, const float *Xb, float *out, int pos0, int B, XQ *xqs) {
    const Config *c = &m->cfg;
    const Layer *L = &m->layers[l];
    const int d = c->dim, hd = c->hd, kvd = c->n_kv * hd, qd = c->n_heads * hd;
    float *QG = q4_scr(8, (size_t)B * 2 * qd), *K = q4_scr(9, (size_t)B * kvd), *V = q4_scr(10, (size_t)B * kvd), *O = q4_scr(11, (size_t)B * qd);
    int prep = 0;
    double tk = q4_tick(-1, 0);
    q4_mm(QG, &L->wq, Xb, xqs, &prep, B, 2 * qd, d);
    q4_mm(K, &L->wk, Xb, xqs, &prep, B, kvd, d);
    q4_mm(V, &L->wv, Xb, xqs, &prep, B, kvd, d);
    q4_tick(QP_ATP, tk);
    q4_att_core(m, l, Xb, QG, K, V, O, pos0, B, xqs, &prep);
    tk = q4_tick(-1, 0);
    prep = 0;
    q4_mm(out, &L->wo, O, xqs, &prep, B, d, qd);
    q4_tick(QP_ATO, tk);
}

/* ---- MoE ----
   Os pares (token, especialista) sao agrupados por especialista e cada linha de peso e aberta uma vez para todos os tokens
   dele: nos tipos de poucos bits com o produto inteiro em lote (xp: entrada quantizada de cada par; xh: a do down), nos
   outros em float. */
static int q4_low(const Mat *W) { return mat_int(W) && lowbit_ok(W->type); }
typedef struct { XQ *xp, *xh; const Layer *L; const float *X; float *hg, *hu, *o; const int *ee, *beg, *pt; int H, d, ng; } Q4Xj;
#define Q4_RB 64
/* uma linha de especialista contra as entradas dos pares p0..p1-1 (no maximo Q4_TB): pt != NULL = entrada do token pt[p] em X */
static void q4_exp_row(const Mat *M, int r, XQ *xqs, const float *X, const int *pt, int p0, int p1, int cols, float *out) {
#if HAVE_LOWBIT
    if (q4_low(M) && lb_mode() == 3) { q4_krow(M, M->q + (size_t)r * M->stride, xqs + p0, p1 - p0, cols, out); return; }
#endif
    if (q4_low(M)) { for (int p = p0; p < p1; p += LB_MAXB) lowbit_dot_batch(M, M->q + (size_t)r * M->stride, xqs, p, p1 - p < LB_MAXB ? p1 : p + LB_MAXB, cols, out + (p - p0)); return; }
    const float *w = q4_open(M, r, cols);
    for (int p = p0; p < p1; p++) out[p - p0] = dot(w, X + (size_t)(pt ? pt[p] : p) * cols, cols);
}
static void q4_exp_glu(void *cx, int i0, int i1) {
    const Q4Xj *j = cx;
    if (q4_opt & QO_EXP) {      /* blocos de ate Q4_RB linhas do mesmo grupo e da mesma matriz; saida local e depois copia por token */
        float loc[Q4_TB * Q4_RB], tmp[Q4_TB];
        for (int i = i0; i < i1;) {
            const int g = i / (2 * j->H), r = i % (2 * j->H), up = r >= j->H, r0 = up ? r - j->H : r, e = j->ee[g];
            int nr = j->H - r0;
            if (nr > i1 - i) nr = i1 - i;
            if (nr > Q4_RB) nr = Q4_RB;
            const Mat *M = up ? &j->L->eu[e] : &j->L->eg[e];
            float *dst = up ? j->hu : j->hg;
            for (int p0 = j->beg[g]; p0 < j->beg[g + 1]; p0 += Q4_TB) {
                const int p1 = p0 + Q4_TB < j->beg[g + 1] ? p0 + Q4_TB : j->beg[g + 1];
                for (int k = 0; k < nr; k++) { q4_exp_row(M, r0 + k, j->xp, j->X, j->pt, p0, p1, j->d, tmp); for (int p = p0; p < p1; p++) loc[(p - p0) * Q4_RB + k] = tmp[p - p0]; }
                for (int p = p0; p < p1; p++) memcpy(dst + (size_t)p * j->H + r0, loc + (p - p0) * Q4_RB, nr * sizeof(float));
            }
            i += nr;
        }
        return;
    }
    for (int i = i0; i < i1; i++) {
        const int g = i / (2 * j->H), r = i % (2 * j->H), up = r >= j->H, e = j->ee[g];
        const Mat *M = up ? &j->L->eu[e] : &j->L->eg[e];
        float *dst = up ? j->hu : j->hg, tmp[LB_MAXB];
        if (q4_low(M)) {
            for (int p0 = j->beg[g]; p0 < j->beg[g + 1]; p0 += LB_MAXB) {
                const int p1 = p0 + LB_MAXB < j->beg[g + 1] ? p0 + LB_MAXB : j->beg[g + 1];
                lowbit_dot_batch(M, M->q + (size_t)(up ? r - j->H : r) * M->stride, j->xp, p0, p1, j->d, tmp);
                for (int p = p0; p < p1; p++) dst[(size_t)p * j->H + (up ? r - j->H : r)] = tmp[p - p0];
            }
            continue;
        }
        const float *w = q4_open(M, up ? r - j->H : r, j->d);
        for (int p = j->beg[g]; p < j->beg[g + 1]; p++) dst[(size_t)p * j->H + (up ? r - j->H : r)] = dot(w, j->X + (size_t)j->pt[p] * j->d, j->d);
    }
}
static void q4_exp_act(void *cx, int p0, int p1) {
    const Q4Xj *j = cx;
    for (size_t i = (size_t)p0 * j->H; i < (size_t)p1 * j->H; i++) j->hg[i] = q4_silu(j->hg[i]) * j->hu[i];
}
static void q4_exp_down(void *cx, int i0, int i1) {
    const Q4Xj *j = cx;
    if (q4_opt & QO_EXP) {
        float loc[Q4_TB * Q4_RB], tmp[Q4_TB];
        for (int i = i0; i < i1;) {
            const int g = i / j->d, r0 = i % j->d;
            int nr = j->d - r0;
            if (nr > i1 - i) nr = i1 - i;
            if (nr > Q4_RB) nr = Q4_RB;
            const Mat *M = &j->L->ed[j->ee[g]];
            for (int p0 = j->beg[g]; p0 < j->beg[g + 1]; p0 += Q4_TB) {
                const int p1 = p0 + Q4_TB < j->beg[g + 1] ? p0 + Q4_TB : j->beg[g + 1];
                for (int k = 0; k < nr; k++) { q4_exp_row(M, r0 + k, j->xh, j->hg, NULL, p0, p1, j->H, tmp); for (int p = p0; p < p1; p++) loc[(p - p0) * Q4_RB + k] = tmp[p - p0]; }
                for (int p = p0; p < p1; p++) memcpy(j->o + (size_t)p * j->d + r0, loc + (p - p0) * Q4_RB, nr * sizeof(float));
            }
            i += nr;
        }
        return;
    }
    for (int i = i0; i < i1; i++) {
        const int g = i / j->d, r = i % j->d;
        const Mat *M = &j->L->ed[j->ee[g]];
        if (q4_low(M)) {
            float tmp[LB_MAXB];
            for (int p0 = j->beg[g]; p0 < j->beg[g + 1]; p0 += LB_MAXB) {
                const int p1 = p0 + LB_MAXB < j->beg[g + 1] ? p0 + LB_MAXB : j->beg[g + 1];
                lowbit_dot_batch(M, M->q + (size_t)r * M->stride, j->xh, p0, p1, j->H, tmp);
                for (int p = p0; p < p1; p++) j->o[(size_t)p * j->d + r] = tmp[p - p0];
            }
            continue;
        }
        const float *w = q4_open(M, r, j->H);
        for (int p = j->beg[g]; p < j->beg[g + 1]; p++) j->o[(size_t)p * j->d + r] = dot(w, j->hg + (size_t)p * j->H, j->H);
    }
}
/* leitura antecipada: threads proprias trazem faixas do arquivo mapeado (MADV_POPULATE_READ: a chamada volta com as paginas
   na memoria; varias threads = varias leituras no disco ao mesmo tempo). Medido: MADV_WILLNEED nao adianta nada neste sistema
   de arquivos (mesmo numero de faltas de pagina e mesmo tempo com o arquivo frio). So faixas ainda nao pedidas nesta execucao. */
#if defined(__linux__)
#ifndef MADV_POPULATE_READ
#define MADV_POPULATE_READ 22
#endif
#define Q4_AHT 8
#define Q4_AHQ 65536
static struct { pthread_mutex_t mu; pthread_cond_t cv; struct { uintptr_t a; size_t n; } q[Q4_AHQ]; unsigned head, tail; int on; } q4_ah = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };
static void *q4_ah_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&q4_ah.mu);
        while (q4_ah.head == q4_ah.tail) pthread_cond_wait(&q4_ah.cv, &q4_ah.mu);
        const uintptr_t a = q4_ah.q[q4_ah.head % Q4_AHQ].a;
        const size_t n = q4_ah.q[q4_ah.head++ % Q4_AHQ].n;
        pthread_mutex_unlock(&q4_ah.mu);
        madvise((void *)a, n, MADV_POPULATE_READ);
    }
    return NULL;
}
static void q4_will(const Mat *W, size_t rows) {
    if (!W->q) return;
    if (!q4_ah.on) {
        q4_ah.on = 1;
        for (int i = 0; i < Q4_AHT; i++) { pthread_t t; if (pthread_create(&t, NULL, q4_ah_thread, NULL) == 0) pthread_detach(t); }
    }
    uintptr_t a = (uintptr_t)W->q & ~(uintptr_t)4095;
    const uintptr_t e = ((uintptr_t)W->q + rows * W->stride + 4095) & ~(uintptr_t)4095;
    pthread_mutex_lock(&q4_ah.mu);
    for (; a < e; a += (uintptr_t)1 << 20) {
        if (q4_ah.tail - q4_ah.head >= Q4_AHQ) break;       /* fila cheia: o resto vem por falta de pagina */
        q4_ah.q[q4_ah.tail % Q4_AHQ].a = a;
        q4_ah.q[q4_ah.tail++ % Q4_AHQ].n = e - a < ((uintptr_t)1 << 20) ? e - a : (size_t)1 << 20;
    }
    pthread_cond_broadcast(&q4_ah.cv);
    pthread_mutex_unlock(&q4_ah.mu);
}
#else
static void q4_will(const Mat *W, size_t rows) { (void)W; (void)rows; }
#endif
static void q4_ahead(const Layer *L, uint8_t *seen, const int *ee, int ng, int H, int d) {
    for (int g = 0; g < ng; g++) {
        if (seen[ee[g]]) continue;
        seen[ee[g]] = 1;
        q4_will(&L->eg[ee[g]], H); q4_will(&L->eu[ee[g]], H); q4_will(&L->ed[ee[g]], d);
    }
}
/* prompt em lote grande com o arquivo frio: pede as paginas da camada inteira (um lote de 256 usa quase todos os especialistas
   dela) duas camadas antes de precisar, uma vez por execucao */
static void q4_layer_ahead(Model *m, int l) {
    const Config *c = &m->cfg;
    Q4 *q = m->q4;
    if (l >= c->n_layers || q->lseen[l]) return;
    q->lseen[l] = 1;
    const Layer *L = &m->layers[l];
    const size_t d = c->dim, D = q->R * d, E = c->n_expert, H = c->hidden, qd = (size_t)c->n_heads * c->hd;
    const Q4Hc *h[2] = { &q->l[l].att, &q->l[l].ffn };
    for (int k = 0; k < 2; k++) { q4_will(&h[k]->down, q->rank); q4_will(&h[k]->up, D); }
    if (L->lin) { q4_will(&L->wqkv, dn_channels(c)); q4_will(&L->wz, c->ssm_inner); q4_will(&L->wssm_out, d); }
    else { q4_will(&L->wq, 2 * qd); q4_will(&L->wo, d); }
    q4_will(&L->eg[0], E * H); q4_will(&L->eu[0], E * H); q4_will(&L->ed[0], E * d);
    memset(q->seen + (size_t)l * E, 1, E);
}
/* roteador do lote: a mesma conta do moe_route, por token em paralelo */
typedef struct { const Config *c; float *LG; int *ex; float *ew; } Q4Rj;
static void q4_rt_job(void *cx, int t0, int t1) {
    const Q4Rj *j = cx;
    const int E = j->c->n_expert, K = j->c->n_used;
    for (int t = t0; t < t1; t++) {
        float *pr = j->LG + (size_t)t * E, wsum = 0.0f;
        softmax(pr, E);
        for (int k = 0; k < K; k++) {
            int best = 0;
            for (int e = 1; e < E; e++) if (pr[e] > pr[best]) best = e;
            j->ex[t * K + k] = best; j->ew[t * K + k] = pr[best]; wsum += pr[best];
            pr[best] = -1.0f;
        }
        if (j->c->moe_norm) for (int k = 0; k < K; k++) j->ew[t * K + k] /= wsum > 6.103515625e-5f ? wsum : 6.103515625e-5f;
    }
}
typedef struct { const float *OU, *ew; const int *inv; float *out; int K, d; } Q4Sj;
static void q4_sum_job(void *cx, int t0, int t1) {
    const Q4Sj *j = cx;
    for (int t = t0; t < t1; t++) {
        float *o = j->out + (size_t)t * j->d;
        for (int k = 0; k < j->K; k++) {
            const float *y = j->OU + (size_t)j->inv[t * j->K + k] * j->d, w = j->ew[t * j->K + k];
            for (int i = 0; i < j->d; i++) o[i] += w * y[i];
        }
    }
}
typedef struct { float *G; const float *U, *X, *S; const Layer *L; float *out; int H, d, fase; } Q4Shj;
static void q4_sh_job(void *cx, int t0, int t1) {
    const Q4Shj *j = cx;
    for (int t = t0; t < t1; t++) {
        if (j->fase == 0) { for (size_t i = (size_t)t * j->H; i < (size_t)(t + 1) * j->H; i++) j->G[i] = q4_silu(j->G[i]) * j->U[i]; continue; }
        const float w = q4_sig(dot(j->L->shexp_gate, j->X + (size_t)t * j->d, j->d));
        for (int i = 0; i < j->d; i++) j->out[(size_t)t * j->d + i] += w * j->S[(size_t)t * j->d + i];
    }
}
static const float *q4_lg;
static int q4_nosh;         /* o compartilhado desta camada roda na placa */
static void q4_moe(Model *m, int l, const float *Xb, float *out, int B, XQ *xqs) {
    const Config *c = &m->cfg;
    const Layer *L = &m->layers[l];
    const int d = c->dim, K = c->n_used, E = c->n_expert, H = c->hidden;
    static int ex[Q4_BMAX * 64], xi[Q4_BMAX * 64]; static float ew[Q4_BMAX * 64];
    float *LG = q4_scr(15, (size_t)B * E), *OU = q4_scr(16, (size_t)B * K * d);
    int prep = 0;
    double tk = q4_tick(-1, 0);
    if (q4_lg && B == 1) memcpy(LG, q4_lg, E * sizeof(float));      /* notas prontas (placa) */
    else q4_mm(LG, &m->q4->l[l].router, Xb, xqs, &prep, B, E, d);
    if ((q4_opt & QO_PAR) && B > 1 && !moe.cnt && !moe.trace && !dbg_on && !L->exp_w && c->exp_scale == 1.0f && !getenv("KATANA_EXPLOG")) {
        Q4Rj rj = { c, LG, ex, ew };
        pool_for(q4_rt_job, &rj, B, 2);
        for (int s = 0; s < B * K; s++) xi[s] = s / K;
        if (route_tap) for (int t = 0; t < B; t++) route_tap(l, ex + t * K, ew + t * K, K, c->moe_norm);
    } else for (int t = 0; t < B; t++) {
        moe_lg_ext = LG + (size_t)t * E;
        moe_route(m, L, l);
        for (int k = 0; k < K; k++) { ex[t * K + k] = moe.sel[k]; xi[t * K + k] = t; ew[t * K + k] = moe.w[k]; }
    }
    moe_lg_ext = NULL;
    tk = q4_tick(QP_RT, tk);
    memset(out, 0, (size_t)B * d * sizeof(float));
    if (B <= GPU_BMAX && moe_int(&L->eg[0]) && moe_int(&L->eu[0]) && moe_int(&L->ed[0]) && !q4_low(&L->eg[0]) && pool.n > 1) {
        moe_cpu_multi(m, L, ex, xi, B * K, B, Xb, OU);
        for (int p = 0; p < B * K; p++) {
            float *o = out + (size_t)xi[p] * d;
            for (int i = 0; i < d; i++) o[i] += ew[p] * OU[(size_t)p * d + i];
        }
    } else {
        static int ee[Q4_BMAX * 64], beg[Q4_BMAX * 64 + 1], pt[Q4_BMAX * 64], src[Q4_BMAX * 64], of[1025];
        int ng = 0, np = 0;
        /* grupos por especialista (contagem), os de mais tokens primeiro: os grandes nao ficam para o fim do trabalho */
        memset(of, 0, sizeof of);
        for (int s = 0; s < B * K; s++) of[ex[s]]++;
        for (int e = 0; e < E; e++) if (of[e]) ee[ng++] = e;
        for (int a = 1; a < ng; a++) { const int e = ee[a]; int b = a - 1; for (; b >= 0 && of[ee[b]] < of[e]; b--) ee[b + 1] = ee[b]; ee[b + 1] = e; }
        for (int g = 0; g < ng; g++) { beg[g] = np; np += of[ee[g]]; of[ee[g]] = beg[g]; }
        beg[ng] = np;
        for (int s = 0; s < B * K; s++) { const int p = of[ex[s]]++; pt[p] = xi[s]; src[p] = s; }
        if (q4_opt & QO_AHEAD) q4_ahead(L, m->q4->seen + (size_t)l * E, ee, ng, H, d);
        static XQ xh[Q4_BMAX * 64];
        static XQ xp[Q4_BMAX * 64];
        if (q4_low(&L->eg[0]) || q4_low(&L->eu[0])) {
            if (!prep) { mm_prepare(xqs, Xb, B, d); prep = 1; }
            for (int p = 0; p < np; p++) xp[p] = xqs[pt[p]];
        }
        Q4Xj j = { xp, xh, L, Xb, q4_scr(17, (size_t)np * H), q4_scr(18, (size_t)np * H), OU, ee, beg, pt, H, d, ng };
        pool_for(q4_exp_glu, &j, ng * 2 * H, q4_opt & QO_EXP ? 32 : 16);
        tk = q4_tick(QP_EG, tk);
        pool_for(q4_exp_act, &j, np, np / (pool.n * 8 + 1) + 1);
        if (q4_low(&L->ed[0])) mm_prepare(xh, j.hg, np, H);
        tk = q4_tick(QP_EA, tk);
        pool_for(q4_exp_down, &j, ng * d, 64);
        tk = q4_tick(QP_ED, tk);
        static int inv[Q4_BMAX * 64];
        for (int p = 0; p < np; p++) inv[src[p]] = p;
        Q4Sj sj = { OU, ew, inv, out, K, d };
        if (q4_opt & QO_PAR) pool_for(q4_sum_job, &sj, B, 1);
        else for (int s = 0; s < B * K; s++) {       /* soma na ordem do roteador: igual para qualquer lote */
            float *o = out + (size_t)xi[s] * d;
            const float *y = OU + (size_t)inv[s] * d;
            for (int i = 0; i < d; i++) o[i] += ew[s] * y[i];
        }
    }
    tk = q4_tick(QP_ES, tk);
    if (q4_nosh) return;
    float *G = q4_scr(17, (size_t)B * H), *U = q4_scr(18, (size_t)B * H), *S = q4_scr(19, (size_t)B * d);
    q4_mm(G, &L->sg, Xb, xqs, &prep, B, H, d);
    q4_mm(U, &L->su, Xb, xqs, &prep, B, H, d);
    Q4Shj hj = { G, U, Xb, S, L, out, H, d, 0 };
    if (q4_opt & QO_PAR) pool_for(q4_sh_job, &hj, B, 4); else q4_sh_job(&hj, 0, B);
    prep = 0;
    q4_mm(S, &L->sd, G, xqs, &prep, B, d, H);
    hj.fase = 1;
    if (q4_opt & QO_PAR) pool_for(q4_sh_job, &hj, B, 4); else q4_sh_job(&hj, 0, B);
    q4_tick(QP_SH, tk);
}

#include "vk_qwen4exp.c"        /* geracao com os mixers e o DeltaNet na placa */

/* ---- B tokens (B <= Q4_BMAX) a partir de pos0: deixa em m->x a entrada da projecao final do ultimo ---- */
static void q4_batch(Model *m, const int *toks, int B, int pos0) {
    const Config *c = &m->cfg;
    Q4 *q = m->q4;
    const int d = c->dim, D = q->R * d, C = (int)dn_channels(c), inner = c->ssm_inner, nv = c->ssm_rank;
    static XQ xqs[Q4_BMAX];
    float *X = q4_scr(20, (size_t)B * D), *Xb = q4_scr(21, (size_t)B * d), *Y = q4_scr(22, (size_t)B * d);
    static float inj[Q4_BMAX * Q4_RMAX];
    q4_pw = B == 1; q4_ptok[q4_pw] += B;
    double tk = q4_tick(-1, 0);
    for (int t = 0; t < B; t++) {
        embed_row(m, X + (size_t)t * D, toks[t]);
        for (int r = 1; r < q->R; r++) memcpy(X + (size_t)t * D + (size_t)r * d, X + (size_t)t * D, d * sizeof(float));
    }
    tk = q4_tick(QP_EMB, tk);
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
        if (B >= 64 && (q4_opt & QO_AHEAD)) { q4_layer_ahead(m, l); q4_layer_ahead(m, l + 1); q4_layer_ahead(m, l + 2); }
        tk = q4_tick(-1, 0);
        if (l == q->ple_l) { q4_ple(m, toks, X, B); q4_tick(QP_PLE, tk); }
        q4_hc_read(m, &q->l[l].att, X, B, Xb, inj);
        if (L->lin) {
            float *P = q4_scr(23, (size_t)B * C), *Z = q4_scr(24, (size_t)B * inner), *O = q4_scr(25, (size_t)B * inner);
            float *AL = q4_scr(26, (size_t)B * nv), *BE = q4_scr(27, (size_t)B * nv);
            int prep = 0;
            tk = q4_tick(-1, 0);
            q4_mm(P, &L->wqkv, Xb, xqs, &prep, B, C, d);
            q4_mm(Z, &L->wz, Xb, xqs, &prep, B, inner, d);
            q4_mm(AL, &L->walpha, Xb, xqs, &prep, B, nv, d);
            q4_mm(BE, &L->wbeta, Xb, xqs, &prep, B, nv, d);
            tk = q4_tick(QP_DNP, tk);
            if (q4_opt & QO_DN) q4_dn(m, L, P, Z, AL, BE, O, B);
            else for (int t = 0; t < B; t++) {
                dn_conv(c, L, dn_CV(m, L), P + (size_t)t * C);
                dn_recur(m, L, P + (size_t)t * C, AL + (size_t)t * nv, BE + (size_t)t * nv, O + (size_t)t * inner);
                q4_dn_gate(c, L, O + (size_t)t * inner, Z + (size_t)t * inner);
            }
            tk = q4_tick(QP_DNR, tk);
            prep = 0;
            q4_mm(Y, &L->wssm_out, O, xqs, &prep, B, d, inner);
            q4_tick(QP_DNO, tk);
        } else q4_att(m, l, Xb, Y, pos0, B, xqs);
        tk = q4_tick(-1, 0);
        q4_hc_write(q, X, Y, inj, B, d);
        q4_tick(QP_HCW, tk);
        q4_hc_read(m, &q->l[l].ffn, X, B, Xb, inj);
        if (layer_tap) for (int t = 0; t < B; t++) layer_tap(l, pos0 + t, Xb + (size_t)t * d);      /* entrada do feed-forward */
        q4_moe(m, l, Xb, Y, B, xqs);
        tk = q4_tick(-1, 0);
        q4_hc_write(q, X, Y, inj, B, d);
        q4_tick(QP_HCW, tk);
        if (q4_dbg.x)
            for (int t = 0; t < B; t++)
                if (pos0 + t < q4_dbg.n) memcpy(q4_dbg.x + ((size_t)(pos0 + t) * c->n_layers + l) * D, X + (size_t)t * D, D * sizeof(float));
    }
    q4_hc_read(m, &q->out, X + (size_t)(B - 1) * D, 1, m->x, NULL);
    for (int i = 0, cl = q->ng - 1; i < B && q->n_ple; i++) {
        memmove(q->hist, q->hist + 1, (cl - 1) * sizeof(int));
        q->hist[cl - 1] = toks[i];
    }
}

static float *cpu_head(Model *m);
static float *q4_forward(Model *m, const int *toks, int n, int pos0, int want_logits) {
    const Config *c = &m->cfg;
    Q4 *q = m->q4;
    if (n < 1) return NULL;
    if (q4_prof < 0) { q4_prof = getenv("KATANA_Q4_PROF") ? atoi(getenv("KATANA_Q4_PROF")) : 0; if (q4_prof > 0) atexit(q4_prof_print); }
    if (pos0 + n > c->ctx) die("qwen4exp: contexto cheio");
    if (pos0 == 0) {        /* conversa nova */
        memset(m->ssm, 0, (size_t)m->n_lin * dn_state_floats(c) * sizeof(float));
        q4g.loc = 0;
        for (int i = 0; i < 8; i++) q->hist[i] = (int)q->eos;
        if (q->n_ple) memset(q->conv, 0, (size_t)(q->pk - 1) * q->ng * q->R * c->dim * sizeof(float));
    }
    if (!q4_bsize) {
        const char *e = getenv("KATANA_Q4_OPT");
        if (e) q4_opt = atoi(e);
#if HAVE_LOWBIT
        e = getenv("KATANA_Q4_TILE");
        if (e && atoi(e) >= 4 && atoi(e) <= Q4_TB) q4_tile = atoi(e);
#endif
        e = getenv("KATANA_Q4_BATCH");
        q4_bsize = e && atoi(e) > 0 ? atoi(e) : 256;
        if (q4_bsize > Q4_BMAX) q4_bsize = Q4_BMAX;
    }
    const int bm = q4_opt != -1 && (q4_opt & QO_ONE) ? 1 : q4_opt & QO_BATCH ? q4_bsize : 16;
    for (int i = 0, pf = 0; i < n;) {
        if (i >= pf) { q4_ngram_prefetch(q, toks + i, n - i < 4096 ? n - i : 4096); pf = i + 4096; }
        const int B = n - i < bm ? n - i : bm;
        if (B == 1 && q4g.on && (q4_opt & QO_GPU) && !gpu.force_cpu && !cpu_exact && !q4_dbg.x && (n == 1 || (q4_opt & QO_ONE))) q4g_token(m, toks[i], pos0 + i);
        else { q4g_home(m); q4_batch(m, toks + i, B, pos0 + i); }
        i += B;
    }
    if (!want_logits) return NULL;
    const double tk = q4_tick(-1, 0);
    float *lg = cpu_head(m);
    q4_tick(QP_HD, tk);
    return lg;
}

/* estado fora do cache KV e do DeltaNet depois de n tokens, para o prefixo em disco: ultimos tokens, entradas da convolucao
   dos n-gramas, bloco aberto do indice e chaves dos blocos completos */
static void q4_state_home(Model *m) { q4g_home(m); }
static size_t q4_state_bytes(const Model *m, int n) {
    const Q4 *q = m->q4;
    int na = 0;
    for (int l = 0; l < m->cfg.n_layers; l++) na += q->ratios[l] != 0;
    return sizeof q->hist + (q->n_ple ? (size_t)(q->pk - 1) * q->ng * q->R * m->cfg.dim * 4 : 0)
         + (na ? (size_t)na * q->ihd * 4 + (size_t)na * (n / q->ratio) * q->ihd * 4 : 0);
}
static int q4_state_io(Model *m, FILE *f, int n, int dir) {
    Q4 *q = m->q4;
    if (!dir) q4g.loc = 0;       /* o estado lido do arquivo esta na CPU */
    int na = 0;
    for (int l = 0; l < m->cfg.n_layers; l++) na += q->ratios[l] != 0;
#define Q4_IO(p, b) ((dir ? fwrite(p, 1, b, f) : fread(p, 1, b, f)) == (size_t)(b))
    int ok = Q4_IO(q->hist, sizeof q->hist);
    if (ok && q->n_ple) ok = Q4_IO(q->conv, (size_t)(q->pk - 1) * q->ng * q->R * m->cfg.dim * 4);
    if (ok && na) ok = Q4_IO(q->iacc, (size_t)na * q->ihd * 4);
    for (int a = 0; a < na && ok; a++) ok = Q4_IO(q->ikey + (size_t)a * (m->cfg.ctx / q->ratio + 1) * q->ihd, (size_t)(n / q->ratio) * q->ihd * 4);
#undef Q4_IO
    return ok;
}

/* katana q4bench <gguf> <texto> [tokens=256] [gerados=24] [A=0] [B=todas] [pares=3; 0 = A e depois cada mascara seguinte uma vez] [mascaras...] : mede dentro do mesmo processo, paginas ja
   quentes (uma passada de aquecimento), alternando duas mascaras de opcoes (q4_opt): tok/s do prompt e da geracao de cada passada,
   diferenca de logits de B contra A (fim do prompt e cada gerado; os tokens gerados sao os da passada de A) e os tempos por parte */
static char *read_text(const char *path);
static int pfx_geom(const Model *m, int *ngl, int *esz, int *backend);
static int pfx_save(Model *m, uint64_t key, int n, int ngl, int esz, int backend);
static int pfx_load(Model *m, uint64_t key, int n, int ngl, int esz, int backend);
static int pfx_path(char *out, size_t cap, uint64_t key, int mk);
static int q4_bench(Model *m, int argc, char **argv) {
    const Config *c = &m->cfg;
    const int N0 = argc > 2 ? atoi(argv[2]) : 256, G = argc > 3 ? atoi(argv[3]) : 24, pairs = argc > 6 ? atoi(argv[6]) : 3, V = c->vocab, nm = pairs ? 2 * pairs : argc - 6;
    const int ma = argc > 4 ? atoi(argv[4]) : 0, mb = argc > 5 ? atoi(argv[5]) : -1;
    if (!c->q4e) die("q4bench: nao e um modelo qwen4exp");
    Ids ids = {0};
    encode(&m->tk, read_text(argv[1]), &ids);
    const int n = ids.n < N0 ? ids.n : N0;
    if (n + G > c->ctx || n < 1 || G < 1) die("q4bench: contexto pequeno");
    float *ref = xalloc((size_t)(G + 1) * V, 4);
    int *gen = xalloc(G + 1, sizeof(int));
    for (int p = 0; p <= nm; p++) {
        const int b = pairs ? p > 0 && p % 2 == 0 : p > 1;      /* 0 aquecimento (A), depois A B A B ... */
        q4_opt = !b ? ma : pairs ? mb : atoi(argv[5 + p]);
        q4_prof = p > 0;
        memset(q4_pt, 0, sizeof q4_pt); memset(q4_ptok, 0, sizeof q4_ptok);
        double t0 = now_sec(), md = 0.0, mx = 0.0;
        int same = 0;
        float *lg = q4_forward(m, ids.ids, n, 0, 1);
        const double tp = now_sec() - t0;
        t0 = now_sec();
        for (int g = 0; g <= G; g++) {
            float *w = ref + (size_t)g * V;
            int a = 0;
            for (int i = 1; i < V; i++) if (lg[i] > lg[a]) a = i;
            if (!b) { memcpy(w, lg, (size_t)V * 4); gen[g] = a; }
            else for (int i = 0; i < V; i++) { const double e = fabs(lg[i] - w[i]); if (e > md) md = e; if (fabs(w[i]) > mx) mx = fabs(w[i]); }
            same += a == gen[g];
            if (g < G) lg = q4_forward(m, &gen[g], 1, n + g, 1);
        }
        const double tg = now_sec() - t0;
        printf("%s opt=%d: prompt %d tokens %.2f tok/s, geracao %d tokens %.2f tok/s", p ? b ? "B" : "A" : "aquecimento", q4_opt, n, n / tp, G, G / tg);
        if (b) printf(", logits: maior diferenca %.3e (maior %.1f), argmax igual %d/%d", md, mx, same, G + 1);
        printf("\n");
        if (pairs && p >= 2 * pairs - 1) q4_prof_print();
        fflush(stdout);
    }
    q4_prof = 0;
    if (getenv("KATANA_CACHE_DIR")) {       /* prefixo em disco: gravar, outra conversa, restaurar e gerar de novo os mesmos tokens */
        int ngl, esz, backend, same = 0;
        const uint64_t key = 0x5134425800000000ULL ^ (uint64_t)getpid();
        char path[1000];
        double md = 0.0;
        q4_opt = pairs ? mb : ma;
        float *lg = q4_forward(m, ids.ids, n, 0, 1);
        double t0 = now_sec();
        const int sv = pfx_geom(m, &ngl, &esz, &backend) && pfx_save(m, key, n, ngl, esz, backend);
        const double ts = now_sec() - t0;
        for (int g = 0; g < G; g++) { lg = q4_forward(m, &gen[g], 1, n + g, 1); memcpy(ref + (size_t)g * V, lg, (size_t)V * 4); }
        q4_forward(m, ids.ids + n / 2, n / 4 + 1, 0, 0);
        t0 = now_sec();
        const int ld = sv && pfx_load(m, key, n, ngl, esz, backend);
        const double tl = now_sec() - t0;
        for (int g = 0; g < G && ld; g++) {
            const float *w = ref + (size_t)g * V;
            int eq = 1;
            lg = q4_forward(m, &gen[g], 1, n + g, 1);
            for (int i = 0; i < V; i++) { const double e = fabs(lg[i] - w[i]); if (e > md) md = e; if (memcmp(lg + i, w + i, 4)) eq = 0; }
            same += eq;
        }
        pfx_path(path, sizeof path, key, 0);
        struct stat st;
        printf("prefixo de %d tokens: gravado %s em %.2f s (%.0f MB), restaurado %s em %.2f s; %d tokens gerados depois, %d com logits iguais bit a bit, maior diferenca %.1e\n",
               n, sv ? "sim" : "NAO", ts, stat(path, &st) == 0 ? st.st_size / 1e6 : 0.0, ld ? "sim" : "NAO", tl, G, same, md);
        remove(path);
    }
    return 0;
}

/* produtos em lote em pesos aleatorios, erro relativo ao |w|.|x|: Q4_K, Q5_K e Q6_K contra o produto inteiro de sempre (a mesma
   ativacao de 16 bits), BF16 contra a linha em float */
static int q4_kern_test(void) {
#if HAVE_LOWBIT
    enum { COLS = 2560, NT = 23 };
    if (lb_mode() != 3) { printf("produtos em lote: sem AVX-512 VNNI nesta maquina\n"); return 1; }
    const uint32_t ty[7] = { T_Q4_K, T_Q5_K, T_Q6_K, T_IQ3_S, T_Q2_0, T_IQ4_NL, T_BF16 };
    float *x = xalloc((size_t)NT * COLS, 4), *w = xalloc(COLS, 4), o[32], o2[32];
    static XQ xqs[NT];
    int ok = 1;
    srand(7);
    for (int i = 0; i < NT * COLS; i++) x[i] = (rand() / (float)RAND_MAX - 0.5f) * (i % 97 == 0 ? 60.0f : 1.0f);
    mm_prepare(xqs, x, NT, COLS);
    for (int k = 0; k < 7; k++) {
        const size_t sb = qbytes(ty[k], COLS);
        uint8_t *q = xalloc(2 * sb, 1);
        double worst = 0.0;
        for (int rep = 0; rep < 20; rep++) {
            for (size_t i = 0; i < 2 * sb; i++) q[i] = (uint8_t)(rand() >> 7);
            if (ty[k] == T_BF16) for (int i = 0; i < 2 * COLS; i++) { const float v = rand() / (float)RAND_MAX - 0.5f; memcpy(q + 2 * i, (const uint8_t *)&v + 2, 2); }
            else if (ty[k] == T_Q2_0 || ty[k] == T_IQ3_S || ty[k] == T_IQ4_NL) {
                const size_t u = ty[k] == T_IQ3_S ? 110 : 18;
                for (size_t o = 0; o < 2 * sb; o += u) { const uint16_t h = (uint16_t)(0x1800 + (rand() & 0x3ff)); memcpy(q + o, &h, 2); }
            } else for (int b = 0; b < 2 * COLS / 256; b++) {      /* escalas f16 finitas */
                uint8_t *p = q + (size_t)b * sb / (COLS / 256);
                const uint16_t h = (uint16_t)(0x1800 + (rand() & 0x3ff)), h2 = (uint16_t)(0x1400 + (rand() & 0x3ff));
                if (ty[k] == T_Q6_K) memcpy(p + 208, &h, 2); else { memcpy(p, &h, 2); memcpy(p + 2, &h2, 2); }
            }
            Mat W = {0}; W.q = q; W.type = ty[k]; W.stride = sb;
            if (ty[k] == T_BF16) { q4_bf2x4(q, q + sb, x, COLS, 3, COLS, o, o2); for (int t = 3; t < NT; t += 4) q4_bf2x4(q, q + sb, x + (size_t)t * COLS, COLS, NT - t < 4 ? NT - t : 4, COLS, o + t, o2 + t); }
            else q4_krow(&W, q, xqs, NT, COLS, o);
            dequant(ty[k], q, w, COLS);
            for (int t = 0; t < NT; t++) {
                double e = 0.0, mag = 0.0;
                for (int i = 0; i < COLS; i++) { e += (double)w[i] * x[(size_t)t * COLS + i]; mag += fabs((double)w[i] * x[(size_t)t * COLS + i]); }
                if (ty[k] != T_BF16) { xq_tls = &xqs[t]; e = dot_row(&W, 0, NULL, NULL, COLS); xq_tls = NULL; }
                double er = fabs(o[t] - e) / mag;
                if (ty[k] == T_BF16 && fabs(q4_bf1(q, x + (size_t)t * COLS, COLS) - e) / mag > er) er = fabs(q4_bf1(q, x + (size_t)t * COLS, COLS) - e) / mag;
                if (!(er <= worst)) worst = er;
            }
        }
        const int good = worst < 1e-6;
        printf("produto em lote tipo %u: erro relativo %.1e: %s\n", ty[k], worst, good ? "OK" : "FALHOU");
        ok = ok && good;
        free(q);
    }
    free(x); free(w);
    return ok;
#else
    return 1;
#endif
}

/* katana q4check <gguf> <referencia>: estados por camada e logits contra a definicao de referencia.
   Arquivo: "Q4RF", int32 n, camadas, R*dim, vocab; int32 tokens[n]; float32 estados[n][camadas][R*dim]; float32 logits[n][vocab].
   Com produtos exatos e depois inteiros, tres formas: token a token, prompt inteiro em lotes e pedacos de tamanho variado seguidos de token a token. */
static int q4_check(Model *m, const char *path) {
    const Config *c = &m->cfg;
    int h[5], ok = 1;
    if (!c->q4e) die("q4check: nao e um modelo qwen4exp");
    FILE *f = fopen(path, "rb");
    if (!f || fread(h, 4, 5, f) != 5 || h[0] != 0x46523451) die("q4check: referencia invalida");
    const int n = h[1], nl = h[2], D = h[3], V = h[4];
    if (nl != c->n_layers || D != m->q4->R * c->dim || V != c->vocab || n < 1 || n > c->ctx) die("q4check: referencia de outro modelo");
    long nan = 0;
    int *tk = xalloc(n, 4);
    float *ref = xalloc((size_t)n * nl * D, 4), *lg = xalloc((size_t)n * V, 4);
    if (fread(tk, 4, n, f) != (size_t)n || fread(ref, 4, (size_t)n * nl * D, f) != (size_t)n * nl * D || fread(lg, 4, (size_t)n * V, f) != (size_t)n * V) die("q4check: referencia truncada");
    fclose(f);
        if (!q4_kern_test()) ok = 0;
    if (!q4g_test()) ok = 0;
    q4_dbg.n = n;
    for (int it = 0; it < 6; it++) {
        const int mode = it % 3;
        cpu_exact = it < 3;     /* produtos em float; depois os inteiros (ativacao em 16 bits) */
        const double tol = !cpu_exact || m->kh || m->kq8 ? 5e-3 : 2e-4;    /* arredondamentos (ativacao, K e V) se propagam pelas camadas */
        static const int pieces[] = { 1, 7, 2, 16, 3, 19, 5, 33 };
        q4_dbg.x = q4g.on && it >= 3 ? NULL : xalloc((size_t)n * nl * D, 4);     /* com a placa: so os logits */
        double mlg = 0.0, mref = 0.0;
        int nlg = 0, agree = 0;
        for (int i = 0, p = 0; i < n;) {
            int B = mode == 0 ? 1 : mode == 1 ? n : i < n * 2 / 3 ? pieces[p++ % 8] : 1;
            if (B > n - i) B = n - i;
            const float *o = q4_forward(m, tk + i, B, i, 1), *w = lg + (size_t)(i + B - 1) * V;
            int a = 0, b = 0;
            for (int k = 0; k < V; k++) {
                uint32_t u; memcpy(&u, o + k, 4); nan += (u & 0x7f800000u) == 0x7f800000u;     /* pelos bits: -ffast-math apaga o isnan */
                if (!(fabs(o[k] - w[k]) <= mlg)) mlg = fabs(o[k] - w[k]);
                if (fabs(w[k]) > mref) mref = fabs(w[k]);
                if (o[k] > o[a]) a = k;
                if (w[k] > w[b]) b = k;
            }
            nlg++; agree += a == b;
            i += B;
        }
        printf("%s %s: %d tokens, logits em %d posicoes: maior diferenca %.3e (maior logit %.3f), argmax igual %d/%d\n",
               cpu_exact ? "exato  " : "inteiro", mode == 0 ? "token a token" : mode == 1 ? "lotes        " : "misto        ", n, nlg, mlg, mref, agree, nlg);
        if (((cpu_exact || q4g.on) && !(mlg <= tol * (mref > 1 ? mref : 1))) || agree != nlg) ok = 0;
        for (int l = 0; l < nl && q4_dbg.x; l++) {
            double md = 0.0, mx = 0.0;
            int bad = 0;        /* tokens acima da tolerancia */
            for (size_t k = 0; k < (size_t)n * D; k++) { const size_t i = (k / D * nl + l) * D + k % D; if (fabs(ref[i]) > mx) mx = fabs(ref[i]); }
            for (int t = 0; t < n; t++) {
                double mt = 0.0;
                for (int i = 0; i < D; i++) {
                    const size_t k = ((size_t)t * nl + l) * D + i;
                    uint32_t u; memcpy(&u, q4_dbg.x + k, 4); nan += (u & 0x7f800000u) == 0x7f800000u;
                    const double e = fabs(q4_dbg.x[k] - ref[k]);
                    if (!(e <= mt)) mt = e;
                }
                if (mt > md) md = mt;
                bad += !(mt <= tol * mx);
            }
            printf("  camada %d: maior diferenca %.3e de %.3f (%.1e), %d tokens acima de %.0e\n", l, md, mx, md / mx, bad, tol);
            /* inteiro: um empate quase exato do roteador ou do top-k pode virar num token isolado do modelo aleatorio */
            if (bad > (cpu_exact ? 0 : n / 50)) ok = 0;
        }
        free(q4_dbg.x); q4_dbg.x = NULL;
    }
    if (nan) { printf("%ld valores nao finitos\n", nan); ok = 0; }
    printf("q4check: %s\n", ok ? "OK" : "FALHOU");
    return ok ? 0 : 1;
}
