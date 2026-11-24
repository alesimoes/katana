/* ============================ render: codificador de texto CLIP ===============================
 * O CLIP ViT-L/14 (so a torre de texto) do Stable Diffusion 1.5: tokenizador BPE de bytes em minusculas e 12 camadas de transformador
 * com atencao causal; a saida usada pela UNet e o ultimo estado ja normalizado, [77][768] (docs/render.md, etapa 4).
 *
 * Tokenizador (o do CLIP, igual ao CLIPTokenizer do transformers): texto em minusculas, espacos juntados e aparados; partido em
 *   <|startoftext|> | <|endoftext|> | 's 't 're 've 'm 'll 'd | letras+ | UM digito | outros+ (sem espaco, letra ou digito)
 * cada pedaco vira bytes -> caracteres (a tabela de bytes do GPT-2), o ultimo simbolo ganha "</w>", e as fusoes (merges) sao aplicadas
 * pela menor ordem ate nao sobrar nenhuma. Sequencia: <|startoftext|> ids <|endoftext|>, completada com <|endoftext|> ate 77 (o
 * pad do CLIP). Passou de 75 ids: corta e fecha com <|endoftext|>. Minusculas so para o latim (ASCII, Latin-1, Latin Estendido-A),
 * o grego e o cirilico; outras escritas ficam como estao.
 *
 * Camada: x += atencao(LN1(x)); x += MLP(LN2(x)); atencao com 12 cabecas de 64, q multiplicado por 1/8, mascara causal, projecao de
 * saida; MLP = fc1 (768 -> 3072), quick_gelu (x * sigmoid(1,702 x)), fc2. LN com eps 1e-5. */

typedef struct {
    const SdgFile *g;
    Map vocab, merges;
    int bos, eos, ntok, nmerge;
    int layers, hidden, heads, ffn, maxpos;
    float eps;
} SdClip;

/* chave do GGUF: devolve o ponteiro para o valor (logo depois do tipo) ou NULL */
static const uint8_t *sdg_kv_find(const SdgFile *g, const char *key, uint32_t *type) {
    const uint8_t *p = g->kv;
    char k[160];
    for (uint64_t i = 0; i < g->nkv; i++) {
        sdg_str(&p, k, sizeof k);
        const uint32_t t = sdg_u32(&p);
        if (!strcmp(k, key)) { if (type) *type = t; return p; }
        sdg_skip(&p, t);
    }
    return NULL;
}
static uint32_t sdg_kv_u32(const SdgFile *g, const char *key, uint32_t def) {
    uint32_t t; const uint8_t *p = sdg_kv_find(g, key, &t);
    return p && t == 4 ? sdg_u32(&p) : def;
}
static float sdg_kv_f32(const SdgFile *g, const char *key, float def) {
    uint32_t t; const uint8_t *p = sdg_kv_find(g, key, &t);
    float v;
    if (!p || t != 6) return def;
    memcpy(&v, p, 4);
    return v;
}

/* vetor de textos do GGUF para um mapa texto -> indice (os textos ficam no arquivo mapeado; o mapa guarda ponteiros) */
static int sdc_map_array(const SdgFile *g, const char *key, Map *m, int *count) {
    uint32_t t; const uint8_t *p = sdg_kv_find(g, key, &t);
    if (!p || t != 9) { fprintf(stderr, "render: chave %s ausente no GGUF\n", key); return 0; }
    const uint32_t et = sdg_u32(&p); const uint64_t n = sdg_u64(&p);
    if (et != 8) { fprintf(stderr, "render: %s nao e um vetor de textos\n", key); return 0; }
    map_init(m, (uint32_t)n + 16);
    for (uint64_t i = 0; i < n; i++) { const uint64_t l = sdg_u64(&p); map_put(m, (const char *)p, (int)l, (int)i); p += l; }
    *count = (int)n;
    return 1;
}

static int sdc_load(SdClip *c, const SdgFile *g) {
    memset(c, 0, sizeof *c);
    c->g = g;
    if (!sdc_map_array(g, "tokenizer.clip.tokens", &c->vocab, &c->ntok) || !sdc_map_array(g, "tokenizer.clip.merges", &c->merges, &c->nmerge)) return 0;
    c->bos = (int)sdg_kv_u32(g, "tokenizer.clip.bos_token_id", 49406); c->eos = (int)sdg_kv_u32(g, "tokenizer.clip.eos_token_id", 49407);
    c->layers = (int)sdg_kv_u32(g, "sd15.clip.num_hidden_layers", 12); c->hidden = (int)sdg_kv_u32(g, "sd15.clip.hidden_size", 768);
    c->heads = (int)sdg_kv_u32(g, "sd15.clip.num_attention_heads", 12); c->ffn = (int)sdg_kv_u32(g, "sd15.clip.intermediate_size", 3072);
    c->maxpos = (int)sdg_kv_u32(g, "sd15.clip.max_position_embeddings", 77); c->eps = sdg_kv_f32(g, "sd15.clip.layer_norm_eps", 1e-5f);
    return 1;
}

/* ---------- tokenizador ---------- */
static int sdc_dec(const unsigned char *s, int n, uint32_t *cp) {       /* um caractere UTF-8; devolve os bytes usados (1 se invalido) */
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    int len = s[0] >= 0xF0 ? 4 : s[0] >= 0xE0 ? 3 : s[0] >= 0xC0 ? 2 : 1;
    if (len == 1 || len > n) { *cp = s[0]; return 1; }
    uint32_t v = s[0] & (0xFFu >> (len + 1));
    for (int i = 1; i < len; i++) { if ((s[i] & 0xC0) != 0x80) { *cp = s[0]; return 1; } v = (v << 6) | (s[i] & 0x3F); }
    *cp = v;
    return len;
}
static int sdc_enc(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}
static uint32_t sdc_lower(uint32_t c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x100 && c <= 0x137 && !(c & 1)) return c + 1;                 /* Latin Estendido-A: pares maiuscula/minuscula */
    if (c >= 0x139 && c <= 0x148 && (c & 1)) return c + 1;
    if (c >= 0x14A && c <= 0x177 && !(c & 1)) return c + 1;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;               /* grego */
    if (c >= 0x410 && c <= 0x42F) return c + 32;                             /* cirilico */
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}
static int sdc_space(uint32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == 0xA0 || c == 0x2028 || c == 0x2029 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A) || c == 0x85; }

/* tabela de bytes do GPT-2: byte -> caractere imprimivel */
static uint32_t sdc_b2u(int b) {
    if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255)) return (uint32_t)b;
    int n = 0;
    for (int i = 0; i < b; i++) if (!((i >= 33 && i <= 126) || (i >= 161 && i <= 172) || (i >= 174 && i <= 255))) n++;
    return 256 + (uint32_t)n;
}

/* BPE de um pedaco (bytes s[0..n)); escreve os ids em ids (ate cap) e devolve quantos */
static int sdc_bpe(const SdClip *c, const unsigned char *s, int n, int *ids, int cap) {
    enum { MAXS = 512 };
    char buf[MAXS * 4 + 8]; int off[MAXS + 1], ns = 0, o = 0;
    if (n <= 0 || n > MAXS - 1) return 0;
    for (int i = 0; i < n; i++) { off[ns++] = o; o += sdc_enc(sdc_b2u(s[i]), buf + o); }
    memcpy(buf + o, "</w>", 4); o += 4;                                        /* o ultimo simbolo carrega o </w> */
    off[ns] = o;
    for (;;) {
        int best = -1, brank = 0x7fffffff;
        for (int i = 0; i + 1 < ns; i++) {
            const int la = off[i + 1] - off[i], lb = off[i + 2] - off[i + 1];
            char key[80];
            if (la + 1 + lb >= (int)sizeof key) continue;
            memcpy(key, buf + off[i], (size_t)la); key[la] = ' '; memcpy(key + la + 1, buf + off[i + 1], (size_t)lb);
            const int r = map_get(&c->merges, key, la + 1 + lb);
            if (r >= 0 && r < brank) { brank = r; best = i; }
        }
        if (best < 0) break;
        for (int i = best + 1; i < ns; i++) off[i] = off[i + 1];               /* funde best e best+1: some a fronteira entre eles */
        ns--;
    }
    int k = 0;
    for (int i = 0; i < ns && k < cap; i++) {
        int id = map_get(&c->vocab, buf + off[i], off[i + 1] - off[i]);
        if (id < 0) id = c->eos;
        ids[k++] = id;
    }
    return k;
}

/* ids do texto (sem o de inicio e o de fim), no maximo cap; devolve quantos */
static int sdc_ids(const SdClip *c, const char *text, int *ids, int cap) {
    const size_t tl = strlen(text);
    unsigned char *t = xalloc(tl + 2, 1); size_t n = 0;
    int pend = 0;                                                            /* normalizacao: minusculas, espacos juntados e aparados */
    for (size_t i = 0; i < tl; ) {
        uint32_t cp; const int l = sdc_dec((const unsigned char *)text + i, (int)(tl - i), &cp);
        i += (size_t)l;
        if (sdc_space(cp)) { pend = n > 0; continue; }
        if (pend) { t[n++] = ' '; pend = 0; }
        n += (size_t)sdc_enc(sdc_lower(cp), (char *)t + n);
    }
    int ni = 0;
    for (size_t i = 0; i < n && ni < cap; ) {
        if (t[i] == ' ') { i++; continue; }
        const size_t rest = n - i;
        if (rest >= 15 && !memcmp(t + i, "<|startoftext|>", 15)) { ids[ni++] = c->bos; i += 15; continue; }
        if (rest >= 13 && !memcmp(t + i, "<|endoftext|>", 13)) { ids[ni++] = c->eos; i += 13; continue; }
        size_t e = i;
        static const char *const con[] = { "'s", "'t", "'re", "'ve", "'m", "'ll", "'d" };
        int hit = 0;
        for (int k = 0; k < 7 && !hit; k++) { const size_t cl = strlen(con[k]); if (rest >= cl && !memcmp(t + i, con[k], cl)) { e = i + cl; hit = 1; } }
        if (!hit) {
            uint32_t cp; int l = sdc_dec(t + i, (int)rest, &cp);
            if (u_L(cp)) { e = i; while (e < n) { l = sdc_dec(t + e, (int)(n - e), &cp); if (!u_L(cp)) break; e += (size_t)l; } }
            else if (u_N(cp)) e = i + (size_t)l;                             /* um digito por vez */
            else { e = i; while (e < n) { l = sdc_dec(t + e, (int)(n - e), &cp); if (sdc_space(cp) || u_L(cp) || u_N(cp)) break; e += (size_t)l; } }
        }
        if (e == i) e = i + 1;
        const int got = sdc_bpe(c, t + i, (int)(e - i), ids + ni, cap - ni);
        ni += got; i = e;
    }
    free(t);
    return ni;
}

/* Pesos de prompt no estilo A1111: (texto) x1.1, [texto] /1.1, (texto:1.3) peso explicito, aninhados multiplicam, \( \) \[ \] \\ sao literais.
 * O texto sem marcas e cortado em trechos de peso constante; cada trecho e tokenizado a parte (como no A1111). ids[i] recebe o id e
 * wts[i] o peso do token. Devolve quantos ids (no maximo cap). */
static int sdc_ids_w(const SdClip *c, const char *text, int *ids, float *wts, int cap) {
    const size_t tl = strlen(text);
    char *seg = xalloc(tl + 2, 1);
    float saved[64]; int sp = 0;
    float cur = 1.0f; size_t sn = 0; int ni = 0;
#define FLUSH() do { if (sn) { seg[sn] = 0; const int g = sdc_ids(c, seg, ids + ni, cap - ni); for (int q = 0; q < g; q++) wts[ni + q] = cur; ni += g; sn = 0; } } while (0)
    for (size_t i = 0; i < tl && ni < cap; i++) {
        const char ch = text[i];
        if (ch == '\\' && i + 1 < tl && strchr("()[]\\", text[i + 1])) { seg[sn++] = text[++i]; continue; }
        if ((ch == '(' || ch == '[') && sp < 64) { FLUSH(); saved[sp++] = cur; cur *= ch == '(' ? 1.1f : 1.0f / 1.1f; continue; }
        if (ch == ':' && sp > 0) {                                           /* (texto:peso) */
            char *end; const double w = strtod(text + i + 1, &end);
            while (*end == ' ') end++;
            if (end != text + i + 1 && *end == ')') { cur = saved[--sp] * (float)w; FLUSH(); cur = saved[sp]; i = (size_t)(end - text); continue; }
        }
        if ((ch == ')' || ch == ']') && sp > 0) { FLUSH(); cur = saved[--sp]; continue; }
        seg[sn++] = ch;
    }
    FLUSH();
#undef FLUSH
    free(seg);
    return ni;
}

/* texto -> 77 ids (preenchidos com <|endoftext|>); devolve quantos ids sao do texto, contando o de inicio e o de fim */
static int sdc_tokenize(const SdClip *c, const char *text, int32_t *out) {
    const int L = c->maxpos;
    int *ids = xalloc((size_t)L + 8, sizeof(int));
    const int n = sdc_ids(c, text, ids, L - 2);
    out[0] = c->bos;
    for (int i = 0; i < n; i++) out[1 + i] = ids[i];
    out[1 + n] = c->eos;
    for (int i = n + 2; i < L; i++) out[i] = c->eos;
    free(ids);
    return n + 2;
}

/* ---------- transformador ---------- */
static VtMat sdc_mat(const SdgFile *g, const char *name, int *ok) {
    VtMat m; memset(&m, 0, sizeof m);
    const SdgInfo *t = sdg_find(g, name);
    if (!t || t->nd != 2 || t->type > 1) { fprintf(stderr, "render: peso %s ausente ou com tipo inesperado\n", name); *ok = 0; return m; }
    m.p = sdg_data(g, t); m.type = t->type == 0 ? VT_F32 : VT_F16; m.cols = (int)t->dim[0]; m.rows = (int)t->dim[1];
    return m;
}
static const float *sdc_vec(const SdgFile *g, const char *name, int n, int *ok) {
    const SdgInfo *t = sdg_find(g, name);
    if (!t || t->nd != 1 || t->type != 0 || (int)t->dim[0] != n) { fprintf(stderr, "render: vetor %s ausente ou com forma inesperada\n", name); *ok = 0; return NULL; }
    return (const float *)sdg_data(g, t);
}

/* ids[77] -> out[77*768] (ultimo estado normalizado); h1 (opcional) recebe o estado depois da primeira camada. 1 = ok */
static int sdc_encode(const SdClip *c, const int32_t *ids, float *out, float *h1) {
    const SdgFile *g = c->g;
    const int n = c->maxpos, D = c->hidden, H = c->heads, hd = D / H, F = c->ffn;
    int ok = 1; char nm[160];
    float *x = xalloc((size_t)n * D, 4), *y = xalloc((size_t)n * D, 4), *q = xalloc((size_t)n * D, 4), *k = xalloc((size_t)n * D, 4), *v = xalloc((size_t)n * D, 4);
    float *a = xalloc((size_t)n * D, 4), *f = xalloc((size_t)n * F, 4), *sc = xalloc((size_t)n, 4);
    const SdgInfo *te = sdg_find(g, "clip.embeddings.token_embedding.weight"), *pe = sdg_find(g, "clip.embeddings.position_embedding.weight");
    if (!te || !pe || te->type != 1 || pe->type != 1) { fprintf(stderr, "render: embeddings do CLIP ausentes ou nao F16\n"); return 0; }
    const uint16_t *tw = (const uint16_t *)sdg_data(g, te), *pw = (const uint16_t *)sdg_data(g, pe);
    for (int i = 0; i < n; i++) {
        if (ids[i] < 0 || ids[i] >= (int)te->dim[1]) { fprintf(stderr, "render: id %d fora do vocabulario\n", ids[i]); return 0; }
        for (int d = 0; d < D; d++) x[(size_t)i * D + d] = f16_to_f32(tw[(size_t)ids[i] * D + d]) + f16_to_f32(pw[(size_t)i * D + d]);
    }
    for (int l = 0; l < c->layers && ok; l++) {
        char p[96]; snprintf(p, sizeof p, "clip.encoder.layers.%d", l);
#define W(s) (snprintf(nm, sizeof nm, "%s." s ".weight", p), sdc_mat(g, nm, &ok))
#define B(s, len) (snprintf(nm, sizeof nm, "%s." s ".bias", p), sdc_vec(g, nm, len, &ok))
        const VtMat wq = W("self_attn.q_proj"), wk = W("self_attn.k_proj"), wv = W("self_attn.v_proj"), wo = W("self_attn.out_proj"), w1 = W("mlp.fc1"), w2 = W("mlp.fc2");
        const float *bq = B("self_attn.q_proj", D), *bk = B("self_attn.k_proj", D), *bv = B("self_attn.v_proj", D), *bo = B("self_attn.out_proj", D), *b1 = B("mlp.fc1", F), *b2 = B("mlp.fc2", D);
        const float *g1 = (snprintf(nm, sizeof nm, "%s.layer_norm1.weight", p), sdc_vec(g, nm, D, &ok)), *e1 = (snprintf(nm, sizeof nm, "%s.layer_norm1.bias", p), sdc_vec(g, nm, D, &ok));
        const float *g2 = (snprintf(nm, sizeof nm, "%s.layer_norm2.weight", p), sdc_vec(g, nm, D, &ok)), *e2 = (snprintf(nm, sizeof nm, "%s.layer_norm2.bias", p), sdc_vec(g, nm, D, &ok));
#undef W
#undef B
        if (!ok) break;
        vt_layernorm(y, x, n, D, g1, e1, c->eps);
        vt_linear(q, y, n, &wq, bq); vt_linear(k, y, n, &wk, bk); vt_linear(v, y, n, &wv, bv);
        const float scale = 1.0f / sqrtf((float)hd);
        for (int h = 0; h < H; h++)                                          /* atencao causal: a posicao i ve de 0 a i */
            for (int i = 0; i < n; i++) {
                const float *qi = q + (size_t)i * D + (size_t)h * hd;
                float mx = -1e30f;
                for (int j = 0; j <= i; j++) {
                    const float *kj = k + (size_t)j * D + (size_t)h * hd; float s = 0;
                    for (int d = 0; d < hd; d++) s += qi[d] * kj[d];
                    sc[j] = s * scale; if (sc[j] > mx) mx = sc[j];
                }
                float sum = 0;
                for (int j = 0; j <= i; j++) { sc[j] = expf(sc[j] - mx); sum += sc[j]; }
                float *ai = a + (size_t)i * D + (size_t)h * hd;
                for (int d = 0; d < hd; d++) ai[d] = 0;
                for (int j = 0; j <= i; j++) { const float w = sc[j] / sum; const float *vj = v + (size_t)j * D + (size_t)h * hd; for (int d = 0; d < hd; d++) ai[d] += w * vj[d]; }
            }
        vt_linear(y, a, n, &wo, bo);
        for (size_t i = 0; i < (size_t)n * D; i++) x[i] += y[i];
        vt_layernorm(y, x, n, D, g2, e2, c->eps);
        vt_linear(f, y, n, &w1, b1);
        for (size_t i = 0; i < (size_t)n * F; i++) f[i] = f[i] / (1.0f + expf(-1.702f * f[i]));       /* quick_gelu */
        vt_linear(y, f, n, &w2, b2);
        for (size_t i = 0; i < (size_t)n * D; i++) x[i] += y[i];
        if (l == 0 && h1) memcpy(h1, x, (size_t)n * D * 4);
    }
    if (ok) {
        const float *gf = sdc_vec(g, "clip.final_layer_norm.weight", D, &ok), *bf = sdc_vec(g, "clip.final_layer_norm.bias", D, &ok);
        if (ok) vt_layernorm(out, x, n, D, gf, bf, c->eps);
    }
    free(x); free(y); free(q); free(k); free(v); free(a); free(f); free(sc);
    return ok;
}

/* Prompt longo, em blocos (a tecnica das outras ferramentas): o texto vira ids, que sao cortados em blocos de ate 75 (um bloco cheio com
 * mais texto depois e cortado na ultima virgula dos seus ultimos 12 ids, se houver); cada bloco e codificado como um prompt de 77 (inicio,
 * ids, fim, preenchimento) e a saida e a concatenacao [blocos*77][768]; a atencao cruzada da UNet aceita qualquer tamanho de contexto.
 * maxchunks (1 a 64) limita o tamanho: o que passar disso e cortado. *nchunks recebe quantos blocos saiu. NULL se falhou. */
static float *sdc_encode_long(const SdClip *c, const char *text, int maxchunks, int *nchunks) {
    const int L = c->maxpos, per = L - 2, D = c->hidden;
    if (maxchunks < 1) maxchunks = 1;
    if (maxchunks > 64) maxchunks = 64;
    int *ids = xalloc((size_t)per * maxchunks + 8, sizeof(int));
    float *wt = xalloc((size_t)per * maxchunks + 8, sizeof(float));
    const int n = sdc_ids_w(c, text, ids, wt, per * maxchunks);
    const int comma = map_get(&c->vocab, ",</w>", 5);
    int start[64], len[64], nc = 0, pos = 0;
    do {
        int l = n - pos < per ? n - pos : per;
        if (l == per && pos + l < n && nc + 1 < maxchunks) for (int k = l - 1; k >= l - 12 && k > 0; k--) if (ids[pos + k] == comma) { l = k + 1; break; }
        start[nc] = pos; len[nc] = l; nc++; pos += l;
    } while (pos < n && nc < maxchunks);
    float *out = xalloc((size_t)nc * L * D, sizeof(float));
    int ok = 1;
    for (int i = 0; i < nc && ok; i++) {
        int32_t buf[128];
        buf[0] = c->bos;
        for (int k = 0; k < len[i]; k++) buf[1 + k] = ids[start[i] + k];
        buf[1 + len[i]] = c->eos;
        for (int k = len[i] + 2; k < L; k++) buf[k] = c->eos;
        float *o = out + (size_t)i * L * D;
        ok = sdc_encode(c, buf, o, NULL);
        int weighted = 0;
        for (int k = 0; k < len[i]; k++) if (wt[start[i] + k] != 1.0f) weighted = 1;
        if (ok && weighted) {                                                /* A1111: multiplica cada token pelo peso e devolve a media original */
            double m0 = 0, m1 = 0;
            for (size_t k = 0; k < (size_t)L * D; k++) m0 += o[k];
            for (int k = 0; k < len[i]; k++) { const float w = wt[start[i] + k]; float *r = o + (size_t)(1 + k) * D; for (int d = 0; d < D; d++) r[d] *= w; }
            for (size_t k = 0; k < (size_t)L * D; k++) m1 += o[k];
            if (m1 != 0) { const float f = (float)(m0 / m1); for (size_t k = 0; k < (size_t)L * D; k++) o[k] *= f; }
        }
    }
    free(ids); free(wt);
    if (!ok) { free(out); return NULL; }
    *nchunks = nc;
    return out;
}
