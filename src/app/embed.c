/* ============================ katana embed ===============================
 * Codificador de embeddings (arquitetura embeddinggemma2, GGUF de `katana convert`). So o texto, na CPU, em float32
 * (as ativacoes do modelo estouram o float16: nada aqui passa por caminhos fp16 nem por quantizacao de ativacoes).
 *
 *   katana embed <modelo.gguf> [--task code|search|document|raw] [--title T] [--dim 128|256|512|768] [--json]
 *                [--cosine] [--ids] "texto"...
 *
 * Funcao reutilizavel: emb_load(&m, caminho) uma vez; embed_text(&m, texto, tarefa, titulo, dim, saida) por texto.
 * O vetor sai normalizado (L2) no tamanho pedido (Matryoshka: trunca e normaliza de novo).
 *
 * Forward (docs/embed.md): ids = <bos> prefixo+texto <eos>; h = embed[id] * sqrt(dim); per-layer inputs vindos so de h
 * (projecao + RMSNorm); por camada: atencao BIDIRECIONAL (janela |i-j| <= W nas camadas locais, global nas demais; q/k com
 * RMSNorm por cabeca, v com RMSNorm sem peso, RoPE com base propria de cada tipo, escala 1), FFN com GELU(tanh), bloco
 * de per-layer input (portao GELU * entrada da camada), normas pos-sub-bloco, escala escalar da camada; norma final,
 * media dos tokens, projecao dim -> dim_saida, truncar, normalizar. */

enum { EMB_CODE = 0, EMB_SEARCH = 1, EMB_DOCUMENT = 2, EMB_RAW = 3 };
#define EMB_MAXL 64

typedef struct {
    float *attn_norm, *post_attn_norm, *ffn_norm, *post_ffn_norm, *q_norm, *k_norm, *ple_post_norm;
    float *wq, *wk, *wv, *wo, *w_gate, *w_up, *w_down, *ple_gate, *ple_out;
    float scale;
} EmbLayer;

typedef struct {
    int n_layers, dim, ffn, heads, ple, out_dim, window, ctx, bos, eos, add_bos, add_eos, pooling, causal;
    float eps, base_glob, base_loc;
    int nkv[EMB_MAXL], hd[EMB_MAXL], local[EMB_MAXL];
    int mdims[8], n_mdims;
    char p_code[96], p_search[96], p_dtitle[48], p_dtext[48];
    Tokenizer tk;
    const uint8_t *tok_q; uint32_t tok_type; size_t tok_stride;      /* embedding: linhas lidas do mapa sob demanda */
    float *ple_proj, *ple_norm, *out_norm, *proj;
    EmbLayer L[EMB_MAXL];
    int last_tokens;                                                  /* tokens do ultimo texto (observacao) */
} EmbModel;

static float emb_gelu(float x) { return 0.5f * x * (1.0f + tanhf(0.7978845608028654f * (x + 0.044715f * x * x * x))); }

static void emb_rms(float *out, const float *x, const float *w, int n, float eps) {
    float ss = 0.0f;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    const float s = powf(ss / (float)n + eps, -0.5f);
    if (w) for (int i = 0; i < n; i++) out[i] = x[i] * s * w[i];
    else for (int i = 0; i < n; i++) out[i] = x[i] * s;
}

/* Y[t][o] = W[o] . X[t] (W: out x in, linhas contiguas) */
typedef struct { const float *W, *X; float *Y; int T, in, out; } EmbMm;
static void emb_mm_job(void *ctx, int i0, int i1) {
    const EmbMm *j = ctx;
    for (int o = i0; o < i1; o++) {
        const float *w = j->W + (size_t)o * j->in;
        for (int t = 0; t < j->T; t++) j->Y[(size_t)t * j->out + o] = dot(w, j->X + (size_t)t * j->in, j->in);
    }
}
static void emb_mm(float *Y, const float *W, const float *X, int T, int in, int out) {
    EmbMm j = { W, X, Y, T, in, out };
    pool_for(emb_mm_job, &j, out, 16);
}

typedef struct { const EmbModel *m; int l; const float *Q, *K, *V; float *O; int T; } EmbAt;
static void emb_at_job(void *ctx, int i0, int i1) {
    const EmbAt *a = ctx;
    const EmbModel *m = a->m;
    const int l = a->l, hd = m->hd[l], nkv = m->nkv[l], T = a->T, H = m->heads, grp = H / nkv;
    float *sc = xalloc((size_t)T, sizeof(float));
    for (int idx = i0; idx < i1; idx++) {
        const int t = idx / H, h = idx % H, kh = h / grp;
        const float *q = a->Q + ((size_t)t * H + h) * hd;
        int lo = 0, hi = T - 1;
        if (m->causal) hi = t;
        if (m->local[l]) { if (t - m->window > lo) lo = t - m->window; if (t + m->window < hi) hi = t + m->window; }
        float mx = -1e30f;
        for (int j = lo; j <= hi; j++) {
            sc[j - lo] = dot(q, a->K + ((size_t)j * nkv + kh) * hd, hd);
            if (sc[j - lo] > mx) mx = sc[j - lo];
        }
        float sum = 0.0f;
        for (int j = lo; j <= hi; j++) { sc[j - lo] = expf(sc[j - lo] - mx); sum += sc[j - lo]; }
        float *o = a->O + ((size_t)t * H + h) * hd;
        for (int d = 0; d < hd; d++) o[d] = 0.0f;
        for (int j = lo; j <= hi; j++) {
            const float w = sc[j - lo] / sum, *v = a->V + ((size_t)j * nkv + kh) * hd;
            for (int d = 0; d < hd; d++) o[d] += w * v[d];
        }
    }
    free(sc);
}

/* ---------- carga ---------- */
static void emb_str(char *dst, size_t cap, Rd *r) { int sl; const char *s = rd_str(r, &sl); if ((size_t)sl >= cap) die("embed: prefixo longo demais no GGUF"); memcpy(dst, s, sl); dst[sl] = 0; }

static int emb_load(EmbModel *m, const char *path) {
    memset(m, 0, sizeof *m);
    int64_t fsz = 0;
    const uint8_t *buf = map_file(path, &fsz);
    if (!buf) { fprintf(stderr, "embed: nao consegui abrir %s\n", path); return -1; }
    Rd r = { buf, buf + fsz };
    if (rd_u32(&r) != 0x46554747u) { fprintf(stderr, "embed: %s nao e um GGUF\n", path); return -1; }
    if (rd_u32(&r) < 2) { fprintf(stderr, "embed: versao de GGUF nao suportada\n"); return -1; }
    const uint64_t n_tensors = rd_u64(&r), n_kv = rd_u64(&r);
    uint64_t align = 32;
    char arch[64] = "";
    const char **merges = NULL; int *mlen = NULL, n_merges = 0;
    Tokenizer *tk = &m->tk;
    m->bos = m->eos = -1; m->eps = 1e-6f;
    int have = 0;
    for (uint64_t i = 0; i < n_kv; i++) {
        int kl; const char *k = rd_str(&r, &kl);
        const uint32_t t = rd_u32(&r);
        if (t == 8 && key_is(k, kl, "general.architecture")) { emb_str(arch, sizeof arch, &r); }
        else if (t == 8 && key_ends(k, kl, ".prompt.code")) emb_str(m->p_code, sizeof m->p_code, &r);
        else if (t == 8 && key_ends(k, kl, ".prompt.search")) emb_str(m->p_search, sizeof m->p_search, &r);
        else if (t == 8 && key_ends(k, kl, ".prompt.document_title")) emb_str(m->p_dtitle, sizeof m->p_dtitle, &r);
        else if (t == 8 && key_ends(k, kl, ".prompt.document_text")) emb_str(m->p_dtext, sizeof m->p_dtext, &r);
        else if (t == 8 && key_is(k, kl, "tokenizer.ggml.model")) {
            int sl; const char *sv = rd_str(&r, &sl);
            if (!(sl == 6 && !memcmp(sv, "gemma4", 6))) { fprintf(stderr, "embed: tokenizador '%.*s' nao suportado (esperado gemma4)\n", sl, sv); return -1; }
            tk->spm = 2;
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.tokens")) {
            const uint32_t et = rd_u32(&r); const uint64_t n = rd_u64(&r);
            if (et != 8) die("tokens: tipo inesperado");
            tk->n = (int)n; tk->tok = xalloc(n, sizeof(char *)); tk->toklen = xalloc(n, sizeof(int));
            for (uint64_t j = 0; j < n; j++) { int sl; const char *s = rd_str(&r, &sl); tk->tok[j] = xalloc(sl + 1, 1); memcpy(tk->tok[j], s, sl); tk->toklen[j] = sl; }
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.merges")) {
            const uint32_t et = rd_u32(&r); const uint64_t n = rd_u64(&r);
            if (et != 8) die("merges: tipo inesperado");
            n_merges = (int)n; merges = xalloc(n, sizeof(char *)); mlen = xalloc(n, sizeof(int));
            for (uint64_t j = 0; j < n; j++) { int sl; const char *s = rd_str(&r, &sl); char *cp = xalloc(sl + 1, 1); memcpy(cp, s, sl); merges[j] = cp; mlen[j] = sl; }
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.token_type")) {
            const uint32_t et = rd_u32(&r); const uint64_t n = rd_u64(&r);
            tk->toktype = xalloc(n, sizeof(int));
            for (uint64_t j = 0; j < n; j++) tk->toktype[j] = (int)rd_num(&r, et);
        } else if (t == 9 && (key_ends(k, kl, ".attention.head_count_kv") || key_ends(k, kl, ".attention.head_dim_list") || key_ends(k, kl, ".attention.sliding_window_pattern")
                              || key_ends(k, kl, ".matryoshka_dims"))) {
            const uint32_t et = rd_u32(&r); const uint64_t n = rd_u64(&r);
            if (et == 8 || et == 9) die("embed: lista com tipo inesperado");
            for (uint64_t j = 0; j < n; j++) {
                const int v = (int)rd_num(&r, et);
                if (key_ends(k, kl, ".head_count_kv")) { if (j < EMB_MAXL) m->nkv[j] = v; }
                else if (key_ends(k, kl, ".head_dim_list")) { if (j < EMB_MAXL) m->hd[j] = v; }
                else if (key_ends(k, kl, ".sliding_window_pattern")) { if (j < EMB_MAXL) m->local[j] = v; }
                else if (j < 8) { m->mdims[j] = v; m->n_mdims = (int)j + 1; }
            }
            have++;
        } else if (t != 8 && t != 9) {
            const double v = rd_num(&r, t);
            if (key_ends(k, kl, ".block_count")) m->n_layers = (int)v;
            else if (key_ends(k, kl, ".context_length")) m->ctx = (int)v;
            else if (key_ends(k, kl, ".embedding_length")) m->dim = (int)v;
            else if (key_ends(k, kl, ".feed_forward_length")) m->ffn = (int)v;
            else if (key_ends(k, kl, ".attention.head_count")) m->heads = (int)v;
            else if (key_ends(k, kl, ".attention.sliding_window")) m->window = (int)v;
            else if (key_ends(k, kl, ".attention.causal")) m->causal = (int)v;
            else if (key_ends(k, kl, ".rope.freq_base_swa")) m->base_loc = (float)v;
            else if (key_ends(k, kl, ".rope.freq_base")) m->base_glob = (float)v;
            else if (key_ends(k, kl, ".layer_norm_rms_epsilon")) m->eps = (float)v;
            else if (key_ends(k, kl, ".ple_length")) m->ple = (int)v;
            else if (key_ends(k, kl, ".embedding_dim")) m->out_dim = (int)v;
            else if (key_ends(k, kl, ".pooling_type")) m->pooling = (int)v;
            else if (key_is(k, kl, "tokenizer.ggml.bos_token_id")) m->bos = (int)v;
            else if (key_is(k, kl, "tokenizer.ggml.eos_token_id")) m->eos = (int)v;
            else if (key_is(k, kl, "tokenizer.ggml.add_bos_token")) m->add_bos = (int)v;
            else if (key_is(k, kl, "tokenizer.ggml.add_eos_token")) m->add_eos = (int)v;
            else if (key_is(k, kl, "general.alignment")) align = (uint64_t)v;
        } else rd_skip(&r, t);
    }
    (void)have;
    if (strcmp(arch, "embeddinggemma2") != 0) { fprintf(stderr, "embed: arquitetura '%s' (esperado embeddinggemma2; converta com katana convert)\n", arch); return -1; }
    if (!m->n_layers || m->n_layers > EMB_MAXL || !m->dim || !m->ffn || !m->heads || !m->ple || !m->out_dim || !m->window || !tk->n || !merges || !m->n_mdims) { fprintf(stderr, "embed: metadados incompletos no GGUF\n"); return -1; }
    if (m->pooling != 1) { fprintf(stderr, "embed: pooling %d nao suportado (so 1 = media)\n", m->pooling); return -1; }
    if (m->bos < 0 || m->eos < 0 || !m->p_code[0] || !m->p_search[0] || !m->p_dtitle[0]) { fprintf(stderr, "embed: GGUF sem prefixos de tarefa ou ids bos/eos\n"); return -1; }
    if (m->ctx <= 0) m->ctx = 8192;
    if (!tk->score) tk->score = xalloc(tk->n, sizeof(float));
    tokenizer_finish(tk, merges, mlen, n_merges);

    TInfo *ti = xalloc(n_tensors, sizeof(TInfo));
    tensors_read(&r, buf, fsz, align, ti, n_tensors);
    Tensors ts = { ti, n_tensors };
    char nm[96];
    const size_t D = (size_t)m->dim, F = (size_t)m->ffn, P = (size_t)m->ple, NL = (size_t)m->n_layers;
    {   /* embedding: so as linhas usadas, lidas do mapa */
        int found = 0;
        for (uint64_t i = 0; i < n_tensors; i++) if (ti[i].namelen == 17 && !memcmp(ti[i].name, "token_embd.weight", 17)) {
            if (ti[i].dims[0] != D || ti[i].dims[1] != (uint64_t)tk->n) { fprintf(stderr, "embed: token_embd com forma inesperada\n"); return -1; }
            m->tok_q = ti[i].base + ti[i].offset; m->tok_type = ti[i].type; m->tok_stride = qbytes(ti[i].type, D); found = 1;
            if (!m->tok_stride) { fprintf(stderr, "embed: tipo %u de token_embd nao suportado\n", ti[i].type); return -1; }
        }
        if (!found) { fprintf(stderr, "embed: token_embd.weight ausente\n"); return -1; }
    }
    m->ple_proj = get_tensor(&ts, "ple_proj.weight", NL * P * D, 1, NULL);
    m->ple_norm = get_tensor(&ts, "ple_norm.weight", P, 1, NULL);
    m->out_norm = get_tensor(&ts, "output_norm.weight", D, 1, NULL);
    m->proj = get_tensor(&ts, "embd_proj.weight", (size_t)m->out_dim * D, 1, NULL);
    for (int l = 0; l < m->n_layers; l++) {
        EmbLayer *L = &m->L[l];
        const int hd = m->hd[l], nkv = m->nkv[l];
        if (hd <= 0 || hd % 2 || nkv <= 0 || m->heads % nkv) { fprintf(stderr, "embed: camada %d com cabecas invalidas\n", l); return -1; }
        for (int q = 0; q < l; q++) if (m->local[q] == m->local[l] && m->hd[q] != hd) { fprintf(stderr, "embed: largura de cabeca diferente entre camadas do mesmo tipo\n"); return -1; }
        const size_t QO = (size_t)m->heads * hd, KO = (size_t)nkv * hd;
#define EV(field, suffix, n) do { snprintf(nm, sizeof nm, "blk.%d." suffix, l); L->field = get_tensor(&ts, nm, (n), 1, NULL); } while (0)
        EV(attn_norm, "attn_norm.weight", D); EV(post_attn_norm, "post_attention_norm.weight", D);
        EV(ffn_norm, "ffn_norm.weight", D); EV(post_ffn_norm, "post_ffw_norm.weight", D);
        EV(q_norm, "attn_q_norm.weight", (size_t)hd); EV(k_norm, "attn_k_norm.weight", (size_t)hd);
        EV(wq, "attn_q.weight", QO * D); EV(wk, "attn_k.weight", KO * D); EV(wv, "attn_v.weight", KO * D); EV(wo, "attn_output.weight", D * QO);
        EV(w_gate, "ffn_gate.weight", F * D); EV(w_up, "ffn_up.weight", F * D); EV(w_down, "ffn_down.weight", D * F);
        EV(ple_gate, "ple_gate.weight", P * D); EV(ple_out, "ple_out.weight", D * P); EV(ple_post_norm, "ple_post_norm.weight", D);
#undef EV
        snprintf(nm, sizeof nm, "blk.%d.layer_scale.weight", l);
        float *sc = get_tensor(&ts, nm, 1, 1, NULL); L->scale = sc[0]; free(sc);
    }
    return 0;
}

/* ---------- forward ---------- */
static void emb_row(const EmbModel *m, int id, float *out) {
    dequant(m->tok_type, m->tok_q + (size_t)id * m->tok_stride, out, (size_t)m->dim);
}

/* tokens do texto ja com prefixo: <bos> ... <eos>, no maximo ctx */
static int emb_tokens(EmbModel *m, const char *text, Ids *ids) {
    if (m->add_bos) ids_push(ids, m->bos);
    Ids body = {0};
    encode(&m->tk, text, &body);
    int room = m->ctx - ids->n - (m->add_eos ? 1 : 0);
    for (int i = 0; i < body.n && i < room; i++) ids_push(ids, body.ids[i]);
    free(body.ids);
    if (m->add_eos) ids_push(ids, m->eos);
    return ids->n;
}

/* vetor do texto, ja com prefixo (sem prefixar), sem normalizar: media dos tokens projetada (dim_saida) */
static int emb_forward(EmbModel *m, const int *ids, int T, float *pooled) {
    const int D = m->dim, F = m->ffn, H = m->heads, P = m->ple, NL = m->n_layers;
    float *h = xalloc((size_t)T * D, sizeof(float)), *x = xalloc((size_t)T * D, sizeof(float)), *tmp = xalloc((size_t)T * D, sizeof(float));
    const float es = sqrtf((float)D);
    for (int t = 0; t < T; t++) {
        if (ids[t] < 0 || ids[t] >= m->tk.n) die("embed: id de token invalido");
        emb_row(m, ids[t], h + (size_t)t * D);
        for (int i = 0; i < D; i++) h[(size_t)t * D + i] *= es;
    }
    /* per-layer inputs: so da projecao do embedding (sem busca por token), escala dim^-0.5, norma por (camada, token) */
    float *pli = xalloc((size_t)T * NL * P, sizeof(float));
    emb_mm(pli, m->ple_proj, h, T, D, NL * P);
    const float ps = powf((float)D, -0.5f);
    for (size_t i = 0; i < (size_t)T * NL; i++) {
        float *v = pli + i * P;
        for (int j = 0; j < P; j++) v[j] *= ps;
        emb_rms(v, v, m->ple_norm, P, m->eps);
    }
    int maxhd = 0, maxkv = 0;
    for (int l = 0; l < NL; l++) { if (m->hd[l] > maxhd) maxhd = m->hd[l]; if (m->nkv[l] > maxkv) maxkv = m->nkv[l]; }
    float *Q = xalloc((size_t)T * H * maxhd, sizeof(float)), *K = xalloc((size_t)T * maxkv * maxhd, sizeof(float)), *V = xalloc((size_t)T * maxkv * maxhd, sizeof(float));
    float *O = xalloc((size_t)T * H * maxhd, sizeof(float));
    float *g = xalloc((size_t)T * F, sizeof(float)), *u = xalloc((size_t)T * F, sizeof(float));
    float *pg = xalloc((size_t)T * P, sizeof(float)), *pt = xalloc((size_t)T * D, sizeof(float));
    /* tabelas de cos/sin do RoPE por tipo de camada (0 = global, 1 = local): [posicao][hd/2] */
    float *rc[2] = { NULL, NULL }, *rs[2] = { NULL, NULL };
    for (int l = 0; l < NL; l++) {
        const int ty = m->local[l] ? 1 : 0, hd = m->hd[l];
        if (rc[ty]) continue;
        const float base = ty ? m->base_loc : m->base_glob;
        rc[ty] = xalloc((size_t)T * (hd / 2), sizeof(float)); rs[ty] = xalloc((size_t)T * (hd / 2), sizeof(float));
        for (int i = 0; i < hd / 2; i++) {
            const float fr = 1.0f / powf(base, (float)(2 * i) / (float)hd);
            for (int t = 0; t < T; t++) { const float an = (float)t * fr; rc[ty][(size_t)t * (hd / 2) + i] = cosf(an); rs[ty][(size_t)t * (hd / 2) + i] = sinf(an); }
        }
    }
    for (int l = 0; l < NL; l++) {
        const EmbLayer *L = &m->L[l];
        const int hd = m->hd[l], nkv = m->nkv[l];
        for (int t = 0; t < T; t++) emb_rms(x + (size_t)t * D, h + (size_t)t * D, L->attn_norm, D, m->eps);
        emb_mm(Q, L->wq, x, T, D, H * hd); emb_mm(K, L->wk, x, T, D, nkv * hd); emb_mm(V, L->wv, x, T, D, nkv * hd);
        const float *cs = rc[m->local[l] ? 1 : 0], *sn = rs[m->local[l] ? 1 : 0];
        for (int t = 0; t < T; t++) {
            for (int hh = 0; hh < H + 2 * nkv; hh++) {
                float *v = hh < H ? Q + ((size_t)t * H + hh) * hd : hh < H + nkv ? K + ((size_t)t * nkv + (hh - H)) * hd : V + ((size_t)t * nkv + (hh - H - nkv)) * hd;
                emb_rms(v, v, hh < H ? L->q_norm : hh < H + nkv ? L->k_norm : NULL, hd, m->eps);      /* v: norma sem peso */
                if (hh >= H + nkv) continue;
                for (int i = 0; i < hd / 2; i++) {      /* RoPE rotate_half sobre a cabeca inteira */
                    const float c = cs[(size_t)t * (hd / 2) + i], s = sn[(size_t)t * (hd / 2) + i], a = v[i], b = v[i + hd / 2];
                    v[i] = a * c - b * s; v[i + hd / 2] = b * c + a * s;
                }
            }
        }
        EmbAt at = { m, l, Q, K, V, O, T };
        pool_for(emb_at_job, &at, T * H, 4);
        emb_mm(tmp, L->wo, O, T, H * hd, D);
        for (int t = 0; t < T; t++) {
            emb_rms(tmp + (size_t)t * D, tmp + (size_t)t * D, L->post_attn_norm, D, m->eps);
            for (int i = 0; i < D; i++) h[(size_t)t * D + i] += tmp[(size_t)t * D + i];
        }
        for (int t = 0; t < T; t++) emb_rms(x + (size_t)t * D, h + (size_t)t * D, L->ffn_norm, D, m->eps);
        emb_mm(g, L->w_gate, x, T, D, F); emb_mm(u, L->w_up, x, T, D, F);
        for (size_t i = 0; i < (size_t)T * F; i++) g[i] = emb_gelu(g[i]) * u[i];
        emb_mm(tmp, L->w_down, g, T, F, D);
        for (int t = 0; t < T; t++) {
            emb_rms(tmp + (size_t)t * D, tmp + (size_t)t * D, L->post_ffn_norm, D, m->eps);
            for (int i = 0; i < D; i++) h[(size_t)t * D + i] += tmp[(size_t)t * D + i];
        }
        /* per-layer input: h += norm(out(gelu(gate(h)) * pli[t][l])) ; depois a escala da camada */
        emb_mm(pg, L->ple_gate, h, T, D, P);
        for (int t = 0; t < T; t++) { const float *pl = pli + ((size_t)t * NL + l) * P; for (int j = 0; j < P; j++) pg[(size_t)t * P + j] = emb_gelu(pg[(size_t)t * P + j]) * pl[j]; }
        emb_mm(pt, L->ple_out, pg, T, P, D);
        for (int t = 0; t < T; t++) {
            emb_rms(pt + (size_t)t * D, pt + (size_t)t * D, L->ple_post_norm, D, m->eps);
            for (int i = 0; i < D; i++) { float *hv = h + (size_t)t * D + i; *hv = (*hv + pt[(size_t)t * D + i]) * L->scale; }
        }
    }
    /* norma final, media dos tokens (a projecao e linear: projetar a media = media das projecoes) */
    float *mean = xalloc(D, sizeof(float));
    for (int t = 0; t < T; t++) {
        emb_rms(x, h + (size_t)t * D, m->out_norm, D, m->eps);
        for (int i = 0; i < D; i++) mean[i] += x[i];
    }
    for (int i = 0; i < D; i++) mean[i] /= (float)T;
    for (int o = 0; o < m->out_dim; o++) pooled[o] = dot(m->proj + (size_t)o * D, mean, D);
    for (int i = 0; i < 2; i++) { free(rc[i]); free(rs[i]); }
    free(mean); free(h); free(x); free(tmp); free(pli); free(Q); free(K); free(V); free(O); free(g); free(u); free(pg); free(pt);
    return 0;
}

/* Embedding de um texto. task: EMB_CODE (consulta de codigo), EMB_SEARCH (consulta geral), EMB_DOCUMENT (documento/trecho;
   title = titulo ou nome do arquivo, NULL = "none"), EMB_RAW (o texto ja traz o prefixo). dim: 0 = nativo, ou uma das
   dimensoes Matryoshka do modelo (128/256/512/768). out: dim floats, normalizado (L2). Devolve a dimensao, ou -1. */
static int embed_text(EmbModel *m, const char *text, int task, const char *title, int dim, float *out) {
    if (dim <= 0) dim = m->out_dim;
    int ok = dim == m->out_dim;
    for (int i = 0; i < m->n_mdims; i++) if (m->mdims[i] == dim) ok = 1;
    if (!ok || dim > m->out_dim) { fprintf(stderr, "embed: dimensao %d nao suportada (use 128, 256, 512 ou 768)\n", dim); return -1; }
    size_t cap = strlen(text) + (title ? strlen(title) : 4) + 256;
    char *full = xalloc(cap, 1);
    switch (task) {
        case EMB_CODE: snprintf(full, cap, "%s%s", m->p_code, text); break;
        case EMB_SEARCH: snprintf(full, cap, "%s%s", m->p_search, text); break;
        case EMB_DOCUMENT: snprintf(full, cap, "%s%s%s%s", m->p_dtitle, title && title[0] ? title : "none", m->p_dtext, text); break;
        case EMB_RAW: snprintf(full, cap, "%s", text); break;
        default: free(full); fprintf(stderr, "embed: tarefa invalida\n"); return -1;
    }
    Ids ids = {0};
    const int T = emb_tokens(m, full, &ids);
    free(full);
    if (T < 1) { free(ids.ids); return -1; }
    m->last_tokens = T;
    float *v = xalloc(m->out_dim, sizeof(float));
    emb_forward(m, ids.ids, T, v);
    free(ids.ids);
    double ss = 0.0;
    for (int i = 0; i < dim; i++) ss += (double)v[i] * v[i];
    const float inv = ss > 0.0 ? (float)(1.0 / sqrt(ss)) : 1.0f;
    for (int i = 0; i < dim; i++) out[i] = v[i] * inv;
    free(v);
    return dim;
}

static void emb_free(EmbModel *m) {
    for (int l = 0; l < m->n_layers; l++) {
        EmbLayer *L = &m->L[l];
        float *f[] = { L->attn_norm, L->post_attn_norm, L->ffn_norm, L->post_ffn_norm, L->q_norm, L->k_norm, L->ple_post_norm, L->wq, L->wk, L->wv, L->wo, L->w_gate, L->w_up, L->w_down, L->ple_gate, L->ple_out };
        for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) free(f[i]);
    }
    free(m->ple_proj); free(m->ple_norm); free(m->out_norm); free(m->proj);
}

static double emb_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }

static int embed_main(int argc, char **argv) {
    const char *usage = "uso: katana embed <modelo.gguf> [--task code|search|document|raw] [--title T] [--dim 128|256|512|768] [--json] [--cosine] [--ids] \"texto\"...\n"
                        "  code: consulta de codigo; search: consulta geral; document: documento ou trecho (use --title com o nome do arquivo); raw: o texto ja tem o prefixo\n";
    if (argc < 2) { fputs(usage, stderr); return 1; }
    const char *path = argv[0], *title = NULL;
    int task = EMB_CODE, dim = 0, json = 0, cosine = 0, show_ids = 0, nt = 0;
    const char **texts = xalloc((size_t)argc, sizeof(char *));
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--task") && i + 1 < argc) {
            const char *v = argv[++i];
            task = !strcmp(v, "code") ? EMB_CODE : !strcmp(v, "search") ? EMB_SEARCH : !strcmp(v, "document") ? EMB_DOCUMENT : !strcmp(v, "raw") ? EMB_RAW : -1;
            if (task < 0) { fprintf(stderr, "embed: tarefa '%s' invalida\n%s", v, usage); return 1; }
        } else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--dim") && i + 1 < argc) dim = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--cosine")) cosine = 1;
        else if (!strcmp(argv[i], "--ids")) show_ids = 1;
        else if (!strncmp(argv[i], "--", 2) && argv[i][2]) { fprintf(stderr, "embed: opcao desconhecida %s\n%s", argv[i], usage); return 1; }
        else texts[nt++] = argv[i];
    }
    if (!nt) { fputs(usage, stderr); return 1; }
    EmbModel *m = xalloc(1, sizeof(EmbModel));
    const double t0 = emb_now();
    if (emb_load(m, path) != 0) return 1;
    const double tl = emb_now() - t0;
    if (!dim) dim = m->out_dim;
    float *vec = xalloc((size_t)nt * dim, sizeof(float));
    double *ms = xalloc((size_t)nt, sizeof(double));
    int *tokens = xalloc((size_t)nt, sizeof(int));
    for (int i = 0; i < nt; i++) {
        const double a = emb_now();
        if (embed_text(m, texts[i], task, title, dim, vec + (size_t)i * dim) < 0) return 1;
        ms[i] = emb_now() - a; tokens[i] = m->last_tokens;
        if (show_ids) {
            Ids ids = {0}; char *full = xalloc(strlen(texts[i]) + 512, 1);
            snprintf(full, strlen(texts[i]) + 512, "%s%s", task == EMB_CODE ? m->p_code : task == EMB_SEARCH ? m->p_search : "", texts[i]);
            if (task == EMB_DOCUMENT) snprintf(full, strlen(texts[i]) + 512, "%s%s%s%s", m->p_dtitle, title ? title : "none", m->p_dtext, texts[i]);
            emb_tokens(m, full, &ids);
            fprintf(stderr, "ids[%d]:", i); for (int k = 0; k < ids.n; k++) fprintf(stderr, " %d", ids.ids[k]); fputc('\n', stderr);
            free(full); free(ids.ids);
        }
    }
    const char *tn = task == EMB_CODE ? "code" : task == EMB_SEARCH ? "search" : task == EMB_DOCUMENT ? "document" : "raw";
    if (json) {
        printf("{\"model\":\"%s\",\"task\":\"%s\",\"dim\":%d,\"load_ms\":%.1f,\"embeddings\":[", path, tn, dim, tl);
        for (int i = 0; i < nt; i++) {
            printf("%s{\"tokens\":%d,\"ms\":%.1f,\"vector\":[", i ? "," : "", tokens[i], ms[i]);
            for (int k = 0; k < dim; k++) printf("%s%.7g", k ? "," : "", vec[(size_t)i * dim + k]);
            printf("]}");
        }
        printf("]");
        if (cosine) {
            printf(",\"cosine\":[");
            for (int i = 0; i < nt; i++) { printf("%s[", i ? "," : ""); for (int j = 0; j < nt; j++) printf("%s%.5f", j ? "," : "", dot(vec + (size_t)i * dim, vec + (size_t)j * dim, dim)); printf("]"); }
            printf("]");
        }
        printf("}\n");
    } else if (cosine) {
        for (int i = 0; i < nt; i++) { for (int j = 0; j < nt; j++) printf("%7.4f ", dot(vec + (size_t)i * dim, vec + (size_t)j * dim, dim)); printf("\n"); }
    } else {
        for (int i = 0; i < nt; i++) { for (int k = 0; k < dim; k++) printf(k ? " %.7g" : "%.7g", vec[(size_t)i * dim + k]); printf("\n"); }
    }
    double tot = 0; int tt = 0;
    for (int i = 0; i < nt; i++) { tot += ms[i]; tt += tokens[i]; }
    fprintf(stderr, "embed: carga %.0f ms; %d texto(s), %d tokens, %.0f ms no total (%.0f ms por texto)\n", tl, nt, tt, tot, tot / nt);
    emb_free(m);
    return 0;
}
