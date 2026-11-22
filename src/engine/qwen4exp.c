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
#define Q4_BMAX GPU_BMAX
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
            L->wqkv = get_mat(ts, QN("attn_qkv.weight"), C, d, 1, np);
            L->wz = get_mat(ts, QN("attn_gate.weight"), c->ssm_inner, d, 1, np);
            L->walpha = q4_mat(m, ts, QN("ssm_alpha.weight"), nv, d);
            L->wbeta = q4_mat(m, ts, QN("ssm_beta.weight"), nv, d);
            L->wssm_out = get_mat(ts, QN("ssm_out.weight"), d, c->ssm_inner, 1, np);
            L->conv_w = get_tensor(ts, QN("ssm_conv1d.weight"), C * c->ssm_conv, 1, np);
            L->ssm_a = get_tensor(ts, QN("ssm_a"), nv, 1, np);
            L->dt_bias = get_tensor(ts, QN("ssm_dt.bias"), nv, 1, np);
            L->ssm_norm = get_tensor(ts, QN("ssm_norm.weight"), c->ssm_inner / nv, 1, np);
        } else {
            L->wq = get_mat(ts, QN("attn_q.weight"), 2 * qd, d, 1, np);
            L->wk = get_mat(ts, QN("attn_k.weight"), kvd, d, 1, np);
            L->wv = get_mat(ts, QN("attn_v.weight"), kvd, d, 1, np);
            L->wo = get_mat(ts, QN("attn_output.weight"), d, qd, 1, np);
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
        L->sg = get_mat(ts, QN("ffn_gate_shexp.weight"), H, d, 1, np);
        L->su = get_mat(ts, QN("ffn_up_shexp.weight"), H, d, 1, np);
        L->sd = get_mat(ts, QN("ffn_down_shexp.weight"), d, H, 1, np);
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
    if (mat_int(W)) { matmat(out, W, X, xqs, prep, B, rows, cols); return; }
    Q4Mm j = { out, W, X, B, rows, cols };
    int chunk = rows / (pool.n * 8 + 1), minc = 32768 / ((long)cols * B) + 1;
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
    for (int i = 0; i < B * q->rank; i++) DN[i] = q4_silu(DN[i] / R);
    prep = 0;
    q4_mm(U, &h->up, DN, xqs, &prep, B, D, q->rank);
    j.fase = 1;
    pool_for(q4_hc_job, &j, B, 1);
    q4_tick(QP_HCR, tk);
}
static void q4_hc_write(const Q4 *q, float *X, const float *y, const float *inj, int B, int d) {
    for (int t = 0; t < B; t++)
        for (int r = 0; r < q->R; r++) {
            float *x = X + ((size_t)t * q->R + r) * d;
            const float *o = y + (size_t)t * d, w = inj[t * q->R + r];
            for (int i = 0; i < d; i++) x[i] += w * o[i];
        }
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
static void q4_ple(Model *m, const int *toks, float *X, int B) {
    const Config *c = &m->cfg;
    Q4 *q = m->q4;
    const int d = c->dim, D = q->R * d, nh = (q->ng - 1) * q->hpn, pe = nh * q->pd, S = (q->pk - 1) * q->ng;
    static XQ xqs[Q4_BMAX];
    uint64_t ids[Q4_BMAX * 64];
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
    for (int t = 0; t < B; t++) {
        float *x = X + (size_t)t * D;
        const float *g = G + (size_t)t * D;
        for (int i = 0; i < D; i++) {
            const float *w = q->pconv + (size_t)i * q->pk;
            float y = 0.0f;
            for (int k = 0; k < q->pk; k++) y += w[k] * W[(size_t)(t + k * q->ng) * D + i];
            x[i] += g[i] + q4_silu(y);
        }
    }
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
static void q4_att(Model *m, int l, const float *Xb, float *out, int pos0, int B, XQ *xqs) {
    const Config *c = &m->cfg;
    Q4 *q = m->q4;
    const Layer *L = &m->layers[l];
    const Q4L *Q = &q->l[l];
    const int d = c->dim, nh = c->n_heads, hd = c->hd, kvd = c->n_kv * hd, qd = nh * hd, ih = q->ihd;
    if (hd % 8) die("qwen4exp: largura de cabeca invalida");
    float *QG = q4_scr(8, (size_t)B * 2 * qd), *K = q4_scr(9, (size_t)B * kvd), *V = q4_scr(10, (size_t)B * kvd), *O = q4_scr(11, (size_t)B * qd);
    float *qq = q4_scr(12, (size_t)B * qd), *IQ = q4_scr(13, (size_t)B * q->in * ih), *IK = q4_scr(14, (size_t)B * ih);
    int prep = 0;
    double tk = q4_tick(-1, 0);
    q4_mm(QG, &L->wq, Xb, xqs, &prep, B, 2 * qd, d);
    q4_mm(K, &L->wk, Xb, xqs, &prep, B, kvd, d);
    q4_mm(V, &L->wv, Xb, xqs, &prep, B, kvd, d);
    q4_mm(IQ, &Q->iq, Xb, xqs, &prep, B, q->in * ih, d);
    q4_mm(IK, &Q->ik, Xb, xqs, &prep, B, ih, d);
    tk = q4_tick(QP_ATP, tk);
    for (int t = 0; t < B; t++) {
        const int pos = pos0 + t;
        for (int h = 0; h < nh; h++) {
            rmsnorm(qq + ((size_t)t * nh + h) * hd, QG + ((size_t)t * nh + h) * 2 * hd, L->q_norm, hd, c->eps);
            q4_rope(m, qq + ((size_t)t * nh + h) * hd, pos);
        }
        float *k = K + (size_t)t * kvd;
        const float *v = V + (size_t)t * kvd;
        for (int h = 0; h < c->n_kv; h++) { rmsnorm(k + h * hd, k + h * hd, L->k_norm, hd, c->eps); q4_rope(m, k + h * hd, pos); }
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
    tk = q4_tick(QP_ATC, tk);
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
static void q4_exp_glu(void *cx, int i0, int i1) {
    const Q4Xj *j = cx;
    for (int i = i0; i < i1; i++) {
        const int g = i / (2 * j->H), r = i % (2 * j->H), up = r >= j->H, e = j->ee[g];
        const Mat *M = up ? &j->L->eu[e] : &j->L->eg[e];
        float *dst = up ? j->hu : j->hg, tmp[Q4_BMAX];
        if (q4_low(M)) {
            lowbit_dot_batch(M, M->q + (size_t)(up ? r - j->H : r) * M->stride, j->xp, j->beg[g], j->beg[g + 1], j->d, tmp);
            for (int p = j->beg[g]; p < j->beg[g + 1]; p++) dst[(size_t)p * j->H + (up ? r - j->H : r)] = tmp[p - j->beg[g]];
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
    for (int i = i0; i < i1; i++) {
        const int g = i / j->d, r = i % j->d;
        const Mat *M = &j->L->ed[j->ee[g]];
        if (q4_low(M)) {
            float tmp[Q4_BMAX];
            lowbit_dot_batch(M, M->q + (size_t)r * M->stride, j->xh, j->beg[g], j->beg[g + 1], j->H, tmp);
            for (int p = j->beg[g]; p < j->beg[g + 1]; p++) j->o[(size_t)p * j->d + r] = tmp[p - j->beg[g]];
            continue;
        }
        const float *w = q4_open(M, r, j->H);
        for (int p = j->beg[g]; p < j->beg[g + 1]; p++) j->o[(size_t)p * j->d + r] = dot(w, j->hg + (size_t)p * j->H, j->H);
    }
}
static void q4_moe(Model *m, int l, const float *Xb, float *out, int B, XQ *xqs) {
    const Config *c = &m->cfg;
    const Layer *L = &m->layers[l];
    const int d = c->dim, K = c->n_used, E = c->n_expert, H = c->hidden;
    static int ex[Q4_BMAX * 64], xi[Q4_BMAX * 64]; static float ew[Q4_BMAX * 64];
    float *LG = q4_scr(15, (size_t)B * E), *OU = q4_scr(16, (size_t)B * K * d);
    int prep = 0;
    double tk = q4_tick(-1, 0);
    q4_mm(LG, &m->q4->l[l].router, Xb, xqs, &prep, B, E, d);
    for (int t = 0; t < B; t++) {
        moe_lg_ext = LG + (size_t)t * E;
        moe_route(m, L, l);
        for (int k = 0; k < K; k++) { ex[t * K + k] = moe.sel[k]; xi[t * K + k] = t; ew[t * K + k] = moe.w[k]; }
    }
    moe_lg_ext = NULL;
    tk = q4_tick(QP_RT, tk);
    memset(out, 0, (size_t)B * d * sizeof(float));
    if (moe_int(&L->eg[0]) && moe_int(&L->eu[0]) && moe_int(&L->ed[0]) && !q4_low(&L->eg[0]) && pool.n > 1) {
        moe_cpu_multi(m, L, ex, xi, B * K, B, Xb, OU);
        for (int p = 0; p < B * K; p++) {
            float *o = out + (size_t)xi[p] * d;
            for (int i = 0; i < d; i++) o[i] += ew[p] * OU[(size_t)p * d + i];
        }
    } else {
        int ee[Q4_BMAX * 64], beg[Q4_BMAX * 64 + 1], pt[Q4_BMAX * 64], src[Q4_BMAX * 64], ng = 0, np = 0;
        for (int p = 0; p < B * K; p++) {       /* grupos na ordem em que o especialista aparece */
            int seen = 0;
            for (int g = 0; g < ng && !seen; g++) seen = ee[g] == ex[p];
            if (seen) continue;
            ee[ng] = ex[p]; beg[ng++] = np;
            for (int s = p; s < B * K; s++) if (ex[s] == ex[p]) { pt[np] = xi[s]; src[np++] = s; }
        }
        beg[ng] = np;
        static XQ xh[Q4_BMAX * 64];
        XQ xp[Q4_BMAX * 64];
        if (q4_low(&L->eg[0]) || q4_low(&L->eu[0])) {
            if (!prep) { mm_prepare(xqs, Xb, B, d); prep = 1; }
            for (int p = 0; p < np; p++) xp[p] = xqs[pt[p]];
        }
        Q4Xj j = { xp, xh, L, Xb, q4_scr(17, (size_t)np * H), q4_scr(18, (size_t)np * H), OU, ee, beg, pt, H, d, ng };
        pool_for(q4_exp_glu, &j, ng * 2 * H, 16);
        tk = q4_tick(QP_EG, tk);
        pool_for(q4_exp_act, &j, np, 1);
        if (q4_low(&L->ed[0])) mm_prepare(xh, j.hg, np, H);
        tk = q4_tick(QP_EA, tk);
        pool_for(q4_exp_down, &j, ng * d, 64);
        tk = q4_tick(QP_ED, tk);
        int inv[Q4_BMAX * 64];
        for (int p = 0; p < np; p++) inv[src[p]] = p;
        for (int s = 0; s < B * K; s++) {       /* soma na ordem do roteador: igual para qualquer lote */
            float *o = out + (size_t)xi[s] * d;
            const float *y = OU + (size_t)inv[s] * d;
            for (int i = 0; i < d; i++) o[i] += ew[s] * y[i];
        }
    }
    tk = q4_tick(QP_ES, tk);
    float *G = q4_scr(17, (size_t)B * H), *U = q4_scr(18, (size_t)B * H), *S = q4_scr(19, (size_t)B * d);
    q4_mm(G, &L->sg, Xb, xqs, &prep, B, H, d);
    q4_mm(U, &L->su, Xb, xqs, &prep, B, H, d);
    for (size_t i = 0; i < (size_t)B * H; i++) G[i] = q4_silu(G[i]) * U[i];
    prep = 0;
    q4_mm(S, &L->sd, G, xqs, &prep, B, d, H);
    for (int t = 0; t < B; t++) {
        const float w = q4_sig(dot(L->shexp_gate, Xb + (size_t)t * d, d));
        for (int i = 0; i < d; i++) out[(size_t)t * d + i] += w * S[(size_t)t * d + i];
    }
    q4_tick(QP_SH, tk);
}

/* ---- B tokens (B <= Q4_BMAX) a partir de pos0: deixa em m->x a entrada da projecao final do ultimo ---- */
static void q4_batch(Model *m, const int *toks, int B, int pos0) {
    const Config *c = &m->cfg;
    Q4 *q = m->q4;
    const int d = c->dim, D = q->R * d, C = (int)dn_channels(c), inner = c->ssm_inner, nv = c->ssm_rank;
    static XQ xqs[Q4_BMAX];
    float *X = q4_scr(20, (size_t)B * D), *Xb = q4_scr(21, (size_t)B * d), *Y = q4_scr(22, (size_t)B * d);
    float inj[Q4_BMAX * Q4_RMAX];
    q4_pw = B == 1; q4_ptok[q4_pw] += B;
    double tk = q4_tick(-1, 0);
    for (int t = 0; t < B; t++) {
        embed_row(m, X + (size_t)t * D, toks[t]);
        for (int r = 1; r < q->R; r++) memcpy(X + (size_t)t * D + (size_t)r * d, X + (size_t)t * D, d * sizeof(float));
    }
    tk = q4_tick(QP_EMB, tk);
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
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
            for (int t = 0; t < B; t++) {
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
        for (int i = 0; i < 8; i++) q->hist[i] = (int)q->eos;
        if (q->n_ple) memset(q->conv, 0, (size_t)(q->pk - 1) * q->ng * q->R * c->dim * sizeof(float));
    }
    for (int i = 0; i < n;) {
        if (i % 4096 == 0) q4_ngram_prefetch(q, toks + i, n - i < 4096 ? n - i : 4096);
        const int B = n - i < Q4_BMAX ? n - i : Q4_BMAX;
        q4_batch(m, toks + i, B, pos0 + i);
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
static size_t q4_state_bytes(const Model *m, int n) {
    const Q4 *q = m->q4;
    int na = 0;
    for (int l = 0; l < m->cfg.n_layers; l++) na += q->ratios[l] != 0;
    return sizeof q->hist + (q->n_ple ? (size_t)(q->pk - 1) * q->ng * q->R * m->cfg.dim * 4 : 0)
         + (na ? (size_t)na * q->ihd * 4 + (size_t)na * (n / q->ratio) * q->ihd * 4 : 0);
}
static int q4_state_io(Model *m, FILE *f, int n, int dir) {
    Q4 *q = m->q4;
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

/* katana q4bench <gguf> <texto> [tokens=256] [gerados=24] [A=0] [B=todas] [pares=3] : mede dentro do mesmo processo, paginas ja
   quentes (uma passada de aquecimento), alternando duas mascaras de opcoes (q4_opt): tok/s do prompt e da geracao de cada passada,
   diferenca de logits de B contra A (fim do prompt e cada gerado; os tokens gerados sao os da passada de A) e os tempos por parte */
static int q4_opt = -1;
static char *read_text(const char *path);
static int q4_bench(Model *m, int argc, char **argv) {
    const Config *c = &m->cfg;
    const int N0 = argc > 2 ? atoi(argv[2]) : 256, G = argc > 3 ? atoi(argv[3]) : 24, pairs = argc > 6 ? atoi(argv[6]) : 3, V = c->vocab;
    const int ma = argc > 4 ? atoi(argv[4]) : 0, mb = argc > 5 ? atoi(argv[5]) : -1;
    if (!c->q4e) die("q4bench: nao e um modelo qwen4exp");
    Ids ids = {0};
    encode(&m->tk, read_text(argv[1]), &ids);
    const int n = ids.n < N0 ? ids.n : N0;
    if (n + G > c->ctx || n < 1 || G < 1) die("q4bench: contexto pequeno");
    float *ref = xalloc((size_t)(G + 1) * V, 4);
    int *gen = xalloc(G + 1, sizeof(int));
    for (int p = 0; p <= 2 * pairs; p++) {
        const int b = p > 0 && p % 2 == 0;      /* 0 aquecimento (A), depois A B A B ... */
        q4_opt = b ? mb : ma;
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
        if (p >= 2 * pairs - 1) q4_prof_print();
        fflush(stdout);
    }
    q4_prof = 0;
    return 0;
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
        q4_dbg.n = n;
    for (int it = 0; it < 6; it++) {
        const int mode = it % 3;
        cpu_exact = it < 3;     /* produtos em float; depois os inteiros (ativacao em 16 bits) */
        const double tol = !cpu_exact || m->kh || m->kq8 ? 5e-3 : 2e-4;    /* arredondamentos (ativacao, K e V) se propagam pelas camadas */
        static const int pieces[] = { 1, 7, 2, 16, 3, 19, 5, 33 };
        q4_dbg.x = xalloc((size_t)n * nl * D, 4);
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
        if ((cpu_exact && !(mlg <= tol * (mref > 1 ? mref : 1))) || agree != nlg) ok = 0;
        for (int l = 0; l < nl; l++) {
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
