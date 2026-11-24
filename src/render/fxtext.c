/* ============================ render: texto do FLUX.2 klein (Qwen3-4B, camadas 9/18/27) ===============================
 * katana render flux-text "<prompt>" [--gguf arq] [--maxlen 512] [--out emb.bin]      gera os embeddings de um prompt
 * katana render flux-text --selftest [pasta-ref] [--gguf arq]                          compara com a referencia em float32
 *
 * O prompt entra no modelo de conversa do Qwen3 (usuario, turno do assistente aberto, pensamento desligado), vai a maxlen posicoes
 * (preenchimento com <|endoftext|> e mascara) e passa pelas 27 primeiras camadas; a saida e [maxlen][3 * 2560]: para cada posicao os
 * estados ocultos depois de 9, de 18 e de 27 camadas, um atras do outro. Todas as posicoes saem, as de preenchimento tambem (o
 * transformador do FLUX as recebe): uma posicao de preenchimento atende as posicoes reais anteriores e nunca as de preenchimento.
 * Atencao causal com mascara de chaves, GQA (32 cabecas de consulta, 8 de chave/valor), norma RMS por cabeca em q e k, RoPE de
 * metades (rotate_half) com theta 1e6, MLP SwiGLU. Pesos F16 no GGUF (fxconvert_text.c), contas em float32. */

#define FXT_MAXL 27

typedef struct { VtMat q, k, v, o, gate, up, down; const float *qn, *kn, *ln1, *ln2; } FxtLayer;
typedef struct {
    SdgFile g; Tokenizer tk;
    int pad, eos, D, H, KV, hd, F, NL, vocab;
    float eps, theta;
    const uint16_t *emb;
    FxtLayer L[FXT_MAXL];
} FxText;

#define FXT_OUT 3
static const int FXT_TAP[FXT_OUT] = { 9, 18, 27 };         /* hidden_states depois de tantas camadas */

static const uint8_t *fxt_kv_arr(const SdgFile *g, const char *key, uint32_t want_et, uint64_t *n) {
    uint32_t t; const uint8_t *p = sdg_kv_find(g, key, &t);
    if (!p || t != 9) { fprintf(stderr, "render: chave %s ausente no GGUF\n", key); return NULL; }
    const uint32_t et = sdg_u32(&p); *n = sdg_u64(&p);
    if (et != want_et) { fprintf(stderr, "render: %s com tipo inesperado\n", key); return NULL; }
    return p;
}

static int fxt_load_tokenizer(FxText *t) {
    Tokenizer *tk = &t->tk;
    memset(tk, 0, sizeof *tk);
    uint64_t n = 0;
    const uint8_t *p = fxt_kv_arr(&t->g, "tokenizer.ggml.tokens", 8, &n);
    if (!p) return 0;
    tk->n = (int)n; tk->tok = xalloc(n, sizeof(char *)); tk->toklen = xalloc(n, sizeof(int));
    for (uint64_t j = 0; j < n; j++) { const uint64_t sl = sdg_u64(&p); tk->tok[j] = xalloc(sl + 1, 1); memcpy(tk->tok[j], p, sl); tk->toklen[j] = (int)sl; p += sl; }
    p = fxt_kv_arr(&t->g, "tokenizer.ggml.token_type", 5, &n);
    if (!p) return 0;
    tk->toktype = xalloc(n, sizeof(int));
    for (uint64_t j = 0; j < n; j++) { int32_t v; memcpy(&v, p + 4 * j, 4); tk->toktype[j] = v; }
    p = fxt_kv_arr(&t->g, "tokenizer.ggml.merges", 8, &n);
    if (!p) return 0;
    const char **merges = xalloc(n, sizeof(char *)); int *mlen = xalloc(n, sizeof(int));
    for (uint64_t j = 0; j < n; j++) { const uint64_t sl = sdg_u64(&p); char *cp = xalloc(sl + 1, 1); memcpy(cp, p, sl); merges[j] = cp; mlen[j] = (int)sl; p += sl; }
    uint32_t ty; const uint8_t *pv = sdg_kv_find(&t->g, "tokenizer.ggml.pre", &ty);
    if (!pv || ty != 8) { fprintf(stderr, "render: GGUF sem tokenizer.ggml.pre\n"); return 0; }
    { const uint64_t sl = sdg_u64(&pv); tk->pre = pre_kind((const char *)pv, (int)sl); tk->nfc = pre_has_nfc(tk->pre); }
    tk->score = xalloc(tk->n, sizeof(float));
    tokenizer_finish(tk, merges, mlen, (int)n);
    return 1;
}

static int fxt_mat(const FxText *t, VtMat *m, int l, const char *name, int rows, int cols) {
    char nm[200];
    snprintf(nm, sizeof nm, "txt.model.layers.%d.%s.weight", l, name);
    const SdgInfo *i = sdg_find(&t->g, nm);
    if (!i || i->type != 1 || i->nd != 2 || (int)i->dim[0] != cols || (int)i->dim[1] != rows) { fprintf(stderr, "render: tensor %s ausente ou com forma/tipo inesperado\n", nm); return 0; }
    m->p = sdg_data(&t->g, i); m->type = VT_F16; m->rows = rows; m->cols = cols;
    return 1;
}
static const float *fxt_vec(const FxText *t, int l, const char *name, int n) {
    char nm[200];
    snprintf(nm, sizeof nm, "txt.model.layers.%d.%s.weight", l, name);
    const SdgInfo *i = sdg_find(&t->g, nm);
    if (!i || i->type != 0 || i->nd != 1 || (int)i->dim[0] != n) { fprintf(stderr, "render: tensor %s ausente ou com forma/tipo inesperado\n", nm); return NULL; }
    return (const float *)sdg_data(&t->g, i);
}

static int fxt_load(FxText *t, const char *path) {
    memset(t, 0, sizeof *t);
    if (!sdg_open(&t->g, path)) return 0;
    uint32_t ty; const uint8_t *p = sdg_kv_find(&t->g, "general.architecture", &ty);
    if (!p || ty != 8) { fprintf(stderr, "render: %s sem general.architecture\n", path); return 0; }
    { const uint64_t sl = sdg_u64(&p); if (sl != 8 || memcmp(p, "fluxtext", 8)) { fprintf(stderr, "render: %s nao e o codificador de texto do FLUX (flux-convert-text)\n", path); return 0; } }
    t->D = (int)sdg_kv_u32(&t->g, "fluxtext.hidden_size", 0); t->F = (int)sdg_kv_u32(&t->g, "fluxtext.intermediate_size", 0);
    t->H = (int)sdg_kv_u32(&t->g, "fluxtext.num_attention_heads", 0); t->KV = (int)sdg_kv_u32(&t->g, "fluxtext.num_key_value_heads", 0);
    t->hd = (int)sdg_kv_u32(&t->g, "fluxtext.head_dim", 0); t->NL = (int)sdg_kv_u32(&t->g, "fluxtext.num_layers", 0);
    t->vocab = (int)sdg_kv_u32(&t->g, "fluxtext.vocab_size", 0);
    t->eps = sdg_kv_f32(&t->g, "fluxtext.rms_eps", 1e-6f); t->theta = sdg_kv_f32(&t->g, "fluxtext.rope_theta", 1e6f);
    t->pad = (int)sdg_kv_u32(&t->g, "tokenizer.ggml.padding_token_id", 0); t->eos = (int)sdg_kv_u32(&t->g, "tokenizer.ggml.eos_token_id", 0);
    if (!t->D || !t->F || !t->H || !t->KV || !t->hd || t->NL < FXT_TAP[FXT_OUT - 1] || t->NL > FXT_MAXL || t->H % t->KV || t->hd % 2) { fprintf(stderr, "render: metadados do codificador de texto incompletos\n"); return 0; }
    const SdgInfo *e = sdg_find(&t->g, "txt.model.embed_tokens.weight");
    if (!e || e->type != 1 || e->nd != 2 || (int)e->dim[0] != t->D) { fprintf(stderr, "render: embedding ausente ou com forma/tipo inesperado\n"); return 0; }
    t->emb = (const uint16_t *)sdg_data(&t->g, e);
    if ((int)e->dim[1] != t->vocab) { fprintf(stderr, "render: embedding com %d linhas, esperado %d\n", (int)e->dim[1], t->vocab); return 0; }
    for (int l = 0; l < t->NL; l++) {
        FxtLayer *y = &t->L[l];
        if (!fxt_mat(t, &y->q, l, "self_attn.q_proj", t->H * t->hd, t->D) || !fxt_mat(t, &y->k, l, "self_attn.k_proj", t->KV * t->hd, t->D) ||
            !fxt_mat(t, &y->v, l, "self_attn.v_proj", t->KV * t->hd, t->D) || !fxt_mat(t, &y->o, l, "self_attn.o_proj", t->D, t->H * t->hd) ||
            !fxt_mat(t, &y->gate, l, "mlp.gate_proj", t->F, t->D) || !fxt_mat(t, &y->up, l, "mlp.up_proj", t->F, t->D) || !fxt_mat(t, &y->down, l, "mlp.down_proj", t->D, t->F)) return 0;
        if (!(y->qn = fxt_vec(t, l, "self_attn.q_norm", t->hd)) || !(y->kn = fxt_vec(t, l, "self_attn.k_norm", t->hd)) ||
            !(y->ln1 = fxt_vec(t, l, "input_layernorm", t->D)) || !(y->ln2 = fxt_vec(t, l, "post_attention_layernorm", t->D))) return 0;
    }
    return fxt_load_tokenizer(t);
}

/* ---------- tokens ---------- */

/* prompt -> ids (maxlen), mascara (1 = real) pelo modelo de conversa do Qwen3 sem pensamento; preenchimento a direita com o token de
 * preenchimento; texto maior que maxlen e cortado no fim. Devolve quantos ids sao reais. */
static int fxt_tokenize(const FxText *t, const char *prompt, int maxlen, int32_t *ids, int32_t *mask) {
    const char *pre = "<|im_start|>user\n", *post = "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
    char *text = xalloc(strlen(pre) + strlen(prompt) + strlen(post) + 1, 1);
    strcpy(text, pre); strcat(text, prompt); strcat(text, post);
    Ids o = {0};
    encode(&t->tk, text, &o);
    free(text);
    const int n = o.n < maxlen ? o.n : maxlen;
    for (int i = 0; i < maxlen; i++) { ids[i] = i < n ? o.ids[i] : t->pad; mask[i] = i < n; }
    free(o.ids);
    return n;
}

/* ---------- forward ---------- */

static void fxt_rms(float *y, const float *x, const float *w, int n, float eps) {
    double s = 0.0;
    for (int i = 0; i < n; i++) s += (double)x[i] * x[i];
    const float r = (float)(1.0 / sqrt(s / n + (double)eps));
    for (int i = 0; i < n; i++) y[i] = x[i] * r * w[i];
}

typedef struct { float *X, *Y; const float *w; int n, d; float eps; } FxtRmsJob;
static void fxt_rms_job(void *cx, int i0, int i1) { const FxtRmsJob *j = cx; for (int i = i0; i < i1; i++) fxt_rms(j->Y + (size_t)i * j->d, j->X + (size_t)i * j->d, j->w, j->d, j->eps); }

/* norma por cabeca (peso [hd]) e RoPE em q [n][heads*hd] */
typedef struct { float *X; const float *w, *cs, *sn; int n, heads, hd; float eps; } FxtQkJob;
static void fxt_qk_job(void *cx, int i0, int i1) {
    const FxtQkJob *j = cx; const int h2 = j->hd / 2;
    float tmp[256];
    for (int r = i0; r < i1; r++) {
        const int pos = r / j->heads;
        float *v = j->X + (size_t)r * j->hd;
        fxt_rms(tmp, v, j->w, j->hd, j->eps);
        const float *c = j->cs + (size_t)pos * h2, *s = j->sn + (size_t)pos * h2;
        for (int i = 0; i < h2; i++) { v[i] = tmp[i] * c[i] - tmp[i + h2] * s[i]; v[i + h2] = tmp[i + h2] * c[i] + tmp[i] * s[i]; }
    }
}

typedef struct { const float *Q, *K, *V; float *O; const int32_t *mask; int n, H, KV, hd; } FxtAttJob;
static void fxt_att_job(void *cx, int i0, int i1) {
    const FxtAttJob *j = cx; const int g = j->H / j->KV, hd = j->hd, kvd = j->KV * hd;
    float *sc = xalloc((size_t)j->n, sizeof(float));
    const float scale = 1.0f / sqrtf((float)hd);
    for (int r = i0; r < i1; r++) {
        const int i = r / j->H, h = r % j->H, kv = h / g;
        const float *q = j->Q + (size_t)i * j->H * hd + (size_t)h * hd;
        float mx = -1e30f; int any = 0;
        for (int k = 0; k <= i; k++) {
            if (!j->mask[k]) { sc[k] = -1e30f; continue; }
            const float *kk = j->K + (size_t)k * kvd + (size_t)kv * hd;
            float s = 0.0f;
            for (int d = 0; d < hd; d++) s += q[d] * kk[d];
            sc[k] = s * scale; if (sc[k] > mx) mx = sc[k]; any = 1;
        }
        float *o = j->O + (size_t)i * j->H * hd + (size_t)h * hd;
        for (int d = 0; d < hd; d++) o[d] = 0.0f;
        if (!any) continue;
        double z = 0.0;
        for (int k = 0; k <= i; k++) { if (!j->mask[k]) { sc[k] = 0.0f; continue; } sc[k] = expf(sc[k] - mx); z += sc[k]; }
        const float iz = (float)(1.0 / z);
        for (int k = 0; k <= i; k++) {
            if (!sc[k]) continue;
            const float w = sc[k] * iz, *vv = j->V + (size_t)k * kvd + (size_t)kv * hd;
            for (int d = 0; d < hd; d++) o[d] += w * vv[d];
        }
    }
    free(sc);
}

typedef struct { float *G; const float *U; } FxtSiluJob;
static void fxt_silu_job(void *cx, int i0, int i1) { const FxtSiluJob *j = cx; for (int i = i0; i < i1; i++) { const float g = j->G[i]; j->G[i] = g / (1.0f + expf(-g)) * j->U[i]; } }

/* ids[n], mask[n] -> [n][3*D] (estados depois de 9, 18 e 27 camadas); NULL se falhou */
static float *fxt_encode_ids(const FxText *t, const int32_t *ids, const int32_t *mask, int n) {
    const int D = t->D, H = t->H, KV = t->KV, hd = t->hd, F = t->F, h2 = hd / 2;
    float *x = xalloc((size_t)n * D, 4), *h = xalloc((size_t)n * D, 4), *q = xalloc((size_t)n * H * hd, 4), *k = xalloc((size_t)n * KV * hd, 4), *v = xalloc((size_t)n * KV * hd, 4);
    float *a = xalloc((size_t)n * H * hd, 4), *g = xalloc((size_t)n * F, 4), *u = xalloc((size_t)n * F, 4);
    float *out = xalloc((size_t)n * FXT_OUT * D, 4);
    float *cs = xalloc((size_t)n * h2, 4), *sn = xalloc((size_t)n * h2, 4);
    for (int pos = 0; pos < n; pos++)
        for (int i = 0; i < h2; i++) { const double ang = (double)pos * pow((double)t->theta, -2.0 * i / hd); cs[(size_t)pos * h2 + i] = (float)cos(ang); sn[(size_t)pos * h2 + i] = (float)sin(ang); }
    for (int i = 0; i < n; i++) {
        if (ids[i] < 0 || ids[i] >= t->vocab) { fprintf(stderr, "render: id %d fora do vocabulario\n", ids[i]); return NULL; }
        const uint16_t *row = t->emb + (size_t)ids[i] * D;
        for (int d = 0; d < D; d++) x[(size_t)i * D + d] = f16_to_f32(row[d]);
    }
    int tap = 0;
    for (int l = 0; l < t->NL; l++) {
        const FxtLayer *y = &t->L[l];
        FxtRmsJob rj = { x, h, y->ln1, n, D, t->eps };
        pool_for(fxt_rms_job, &rj, n, 1);
        vt_linear(q, h, n, &y->q, NULL); vt_linear(k, h, n, &y->k, NULL); vt_linear(v, h, n, &y->v, NULL);
        FxtQkJob qj = { q, y->qn, cs, sn, n, H, hd, t->eps }, kj = { k, y->kn, cs, sn, n, KV, hd, t->eps };
        pool_for(fxt_qk_job, &qj, n * H, 16); pool_for(fxt_qk_job, &kj, n * KV, 16);
        FxtAttJob aj = { q, k, v, a, mask, n, H, KV, hd };
        pool_for(fxt_att_job, &aj, n * H, 4);
        vt_linear(h, a, n, &y->o, NULL);
        for (size_t i = 0; i < (size_t)n * D; i++) x[i] += h[i];
        FxtRmsJob r2 = { x, h, y->ln2, n, D, t->eps };
        pool_for(fxt_rms_job, &r2, n, 1);
        vt_linear(g, h, n, &y->gate, NULL); vt_linear(u, h, n, &y->up, NULL);
        FxtSiluJob sj = { g, u };
        pool_for(fxt_silu_job, &sj, (int)((size_t)n * F), 65536);
        vt_linear(h, g, n, &y->down, NULL);
        for (size_t i = 0; i < (size_t)n * D; i++) x[i] += h[i];
        if (tap < FXT_OUT && l + 1 == FXT_TAP[tap]) {                       /* hidden_states[l+1] */
            for (int i = 0; i < n; i++) memcpy(out + (size_t)i * FXT_OUT * D + (size_t)tap * D, x + (size_t)i * D, (size_t)D * 4);
            tap++;
        }
    }
    free(x); free(h); free(q); free(k); free(v); free(a); free(g); free(u); free(cs); free(sn);
    return out;
}

/* prompt -> [maxlen][3*D]; *nreal recebe quantas posicoes sao do texto (o resto e preenchimento). Memoria nova. */
static float *fxt_embed(const FxText *t, const char *prompt, int maxlen, int *nreal) {
    int32_t *ids = xalloc((size_t)maxlen, 4), *mask = xalloc((size_t)maxlen, 4);
    const int n = fxt_tokenize(t, prompt, maxlen, ids, mask);
    if (nreal) *nreal = n;
    float *e = fxt_encode_ids(t, ids, mask, maxlen);
    free(ids); free(mask);
    return e;
}

/* ---------- teste e comando ---------- */

static float *fxt_read(const char *path, size_t bytes) {
    int64_t sz = 0; const uint8_t *b = map_file(path, &sz);
    if (!b || (size_t)sz != bytes) { fprintf(stderr, "flux-text: nao achei %s (%zu bytes)\n", path, bytes); return NULL; }
    float *o = xalloc(bytes, 1); memcpy(o, b, bytes);
    return o;
}

static int fxt_selftest(const char *refdir, const char *gguf) {
    FxText t;
    if (!fxt_load(&t, gguf)) return 1;
    static const char *const name[] = { "en", "pt", "acc" };
    const int L = 512, D3 = FXT_OUT * t.D;
    int bad = 0;
    for (int c = 0; c < 3; c++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/%s_prompt.txt", refdir, name[c]);
        int64_t sz = 0; const uint8_t *pb = map_file(p, &sz);
        if (!pb) { fprintf(stderr, "flux-text: nao achei %s\n", p); return 1; }
        char *prompt = xalloc((size_t)sz + 1, 1); memcpy(prompt, pb, (size_t)sz);
        snprintf(p, sizeof p, "%s/%s_ids.bin", refdir, name[c]); int32_t *rid = (int32_t *)fxt_read(p, (size_t)L * 4);
        snprintf(p, sizeof p, "%s/%s_mask.bin", refdir, name[c]); int32_t *rmk = (int32_t *)fxt_read(p, (size_t)L * 4);
        snprintf(p, sizeof p, "%s/%s_emb.bin", refdir, name[c]); float *ref = fxt_read(p, (size_t)L * D3 * 4);
        if (!rid || !rmk || !ref) return 1;
        int32_t ids[512], mask[512];
        const int n = fxt_tokenize(&t, prompt, L, ids, mask);
        int diff = 0; for (int i = 0; i < L; i++) if (ids[i] != rid[i] || mask[i] != rmk[i]) diff++;
        printf("  %-4s tokens: %d reais, %d posicoes diferentes da referencia  %s\n", name[c], n, diff, diff ? "FALHOU" : "OK");
        if (diff) { bad++; continue; }
        float *e = fxt_encode_ids(&t, ids, mask, L);
        if (!e) return 1;
        double mx = 0, mref = 0, ssd = 0, ssr = 0;
        double slot_mx[FXT_OUT] = { 0 };
        for (int i = 0; i < L; i++)
            for (int k = 0; k < D3; k++) {
                const double r = ref[(size_t)i * D3 + k], d = fabs(e[(size_t)i * D3 + k] - r);
                if (d > mx) mx = d; if (fabs(r) > mref) mref = fabs(r);
                const int s = k / t.D; if (d > slot_mx[s]) slot_mx[s] = d;
                ssd += d * d; ssr += r * r;
            }
        const int ok = mx / mref < 1e-3;
        printf("  %-4s embeddings [512x%d]: erro max %.3e  |ref| max %.1f  razao %.2e  rms relativo %.2e  (camadas 9/18/27: %.2e %.2e %.2e)  %s\n", name[c], D3, mx, mref, mx / mref, sqrt(ssd / ssr),
               slot_mx[0] / mref, slot_mx[1] / mref, slot_mx[2] / mref, ok ? "OK" : "FALHOU");
        if (!ok) bad++;
        free(e); free(rid); free(rmk); free(ref); free(prompt);
    }
    printf(bad ? "RESULTADO: %d teste(s) FALHARAM\n" : "RESULTADO: todos passaram\n", bad);
    return bad ? 1 : 0;
}

static int fxt_main(int argc, char **argv) {
    const char *prompt = NULL, *out = NULL, *refdir = "/mnt/dados/render-ref/fluxA"; char gg[1100]; int maxlen = 512, selftest = 0;
    snprintf(gg, sizeof gg, "%s/flux2klein/gguf/qwen3-4b-text.gguf", models_dir());
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i]; const int more = i + 1 < argc;
        if (!strcmp(a, "--selftest")) { selftest = 1; if (more && argv[i + 1][0] != '-') refdir = argv[++i]; }
        else if (!strcmp(a, "--gguf") && more) snprintf(gg, sizeof gg, "%s", argv[++i]);
        else if (!strcmp(a, "--maxlen") && more) maxlen = atoi(argv[++i]);
        else if (!strcmp(a, "--out") && more) out = argv[++i];
        else if (a[0] != '-' && !prompt) prompt = a;
        else { fprintf(stderr, "flux-text: opcao desconhecida: %s\n", a); return 1; }
    }
    if (selftest) return fxt_selftest(refdir, gg);
    if (!prompt || maxlen < 16 || maxlen > 1024) { fprintf(stderr, "uso: katana render flux-text \"<prompt>\" [--gguf arq] [--maxlen 512] [--out emb.bin]\n       katana render flux-text --selftest [pasta-ref] [--gguf arq]\n"); return 1; }
    FxText t;
    if (!fxt_load(&t, gg)) return 1;
    int nreal = 0;
    float *e = fxt_embed(&t, prompt, maxlen, &nreal);
    if (!e) return 1;
    printf("flux-text: %d posicoes reais de %d, saida [%d][%d]\n", nreal, maxlen, maxlen, FXT_OUT * t.D);
    if (out) { FILE *f = fopen(out, "wb"); if (!f || fwrite(e, 4, (size_t)maxlen * FXT_OUT * t.D, f) != (size_t)maxlen * FXT_OUT * t.D) { fprintf(stderr, "flux-text: nao consegui gravar %s\n", out); return 1; } fclose(f); }
    free(e);
    return 0;
}
