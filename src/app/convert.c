/* ============================ katana convert =============================
 * katana convert <pasta-hf> <saida.gguf> [tipo]
 * Le um checkpoint do Hugging Face (*.safetensors em um ou varios arquivos, config.json, tokenizer.json,
 * tokenizer_config.json, generation_config.json) e grava um GGUF que o motor carrega. Arquiteturas: Qwen3.5/3.6 MoE
 * (qwen35moe: atencao com portao + Gated DeltaNet + MoE com especialista compartilhado) e a variante densa (qwen35).
 *
 * Memoria limitada: nada do modelo e carregado de uma vez. Cada arquivo safetensors e mapeado (mmap), a tabela de
 * tensores do GGUF e planejada numa primeira passada (tamanhos finais conhecidos pelos tipos escolhidos) e os dados sao
 * gravados na segunda passada, um tensor por vez, em fatias de linhas: cada fatia e lida, reordenada, convertida para
 * float, codificada (reaproveita os codificadores do katana quantize) e gravada. O pico de memoria e uma fatia (dezenas
 * de MB), nao o modelo. Depois de cada tensor as paginas lidas sao devolvidas (madvise).
 *
 * Mapeamento (HF -> GGUF), por camada l; P = model.language_model. (ou model.):
 *   P embed_tokens.weight                     token_embd.weight
 *   P norm.weight                             output_norm.weight          (+1: a norma do HF e (1 + w))
 *   lm_head.weight                            output.weight               (ausente = pesos amarrados)
 *   P layers.l.input_layernorm.weight         blk.l.attn_norm.weight      (+1)
 *   P layers.l.post_attention_layernorm.weight blk.l.post_attention_norm.weight (+1)
 *   atencao completa:  self_attn.{q,k,v,o}_proj.weight -> attn_{q,k,v}.weight, attn_output.weight (q tem a metade do
 *                      portao junto: sem mexer); self_attn.{q,k}_norm.weight -> attn_{q,k}_norm.weight (+1)
 *   Gated DeltaNet:    linear_attn.in_proj_qkv.weight -> attn_qkv.weight (linhas de v reordenadas)
 *                      linear_attn.in_proj_z.weight   -> attn_gate.weight (linhas reordenadas)
 *                      linear_attn.in_proj_b/a.weight -> ssm_beta/ssm_alpha.weight (linhas reordenadas)
 *                      linear_attn.conv1d.weight [C,1,K] -> ssm_conv1d.weight (K,C) (canais de v reordenados)
 *                      linear_attn.A_log -> ssm_a  (-exp(A_log), reordenado); linear_attn.dt_bias -> ssm_dt.bias (reordenado)
 *                      linear_attn.norm.weight -> ssm_norm.weight (sem +1); linear_attn.out_proj.weight -> ssm_out.weight
 *                      (colunas reordenadas)
 *   MoE:               mlp.gate.weight -> ffn_gate_inp.weight; mlp.experts.gate_up_proj [E,2F,D] -> ffn_gate_exps e
 *                      ffn_up_exps (metades de F linhas); mlp.experts.down_proj -> ffn_down_exps; ou, no formato antigo,
 *                      mlp.experts.N.{gate,up,down}_proj.weight empilhados na ordem de N;
 *                      mlp.shared_expert.{gate,up,down}_proj.weight -> ffn_{gate,up,down}_shexp.weight;
 *                      mlp.shared_expert_gate.weight [1,D] -> ffn_gate_inp_shexp.weight (D)
 *   denso (qwen35):    mlp.{gate,up,down}_proj.weight -> ffn_{gate,up,down}.weight
 * Fora do mapa e ignorado: visual.* e mtp.* (contados no relatorio). Qualquer outro tensor sobrando gera aviso.
 * As dimensoes do GGUF sao as do HF na ordem inversa (ne0 = coluna, que e contigua); por isso nenhum peso e
 * transposto: so reordenado quando preciso.
 *
 * Reordenacao das cabecas v do DeltaNet: o HF guarda as cabecas v agrupadas por cabeca k ([k0:v0..v(r-1), k1:...], r =
 * v/k); o motor le na ordem "em telhas" ([k0:v0, k1:v0, ..., k0:v1, ...]), para o broadcast do grupo ser um simples
 * repeat. A cabeca de saida h = a*nk + b vem da cabeca de entrada b*r + a (a < r, b < nk). Vale em todos os tensores
 * indexados por cabeca v, cada um com a sua largura de cabeca (128 linhas em qkv/z, 1 em a/b/A_log/dt_bias, 128
 * colunas em out_proj). Com nk == nv a permutacao e a identidade. */

/* ---------- JSON minimo (arvore em memoria; so o necessario para configs, indices e tokenizer.json) ---------- */
enum { J_NULL = 0, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct JV JV;
struct JV { int t; double num; int64_t i; int isint; char *s; size_t slen; JV *v; char **k; size_t n, cap; };
typedef struct { const char *p, *end; int err; } JP;

static void jp_ws(JP *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++; }

static int jp_hex4(JP *j, uint32_t *out) {
    if (j->end - j->p < 4) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) { int h = hex_val(j->p[i]); if (h < 0) return 0; v = v * 16 + (uint32_t)h; }
    j->p += 4; *out = v; return 1;
}

/* le uma string JSON ja depois da aspa de abertura; devolve memoria nova com terminador */
static char *jp_string(JP *j, size_t *len) {
    size_t cap = 32, n = 0;
    char *b = xalloc(cap, 1);
    while (j->p < j->end && *j->p != '"') {
        uint32_t cp; unsigned char c = (unsigned char)*j->p++;
        if (n + 8 > cap) { cap *= 2; b = realloc(b, cap); if (!b) die("convert: sem memoria"); }
        if (c != '\\') { b[n++] = (char)c; continue; }
        if (j->p >= j->end) { j->err = 1; break; }
        c = (unsigned char)*j->p++;
        switch (c) {
            case 'n': b[n++] = '\n'; break; case 't': b[n++] = '\t'; break; case 'r': b[n++] = '\r'; break;
            case 'b': b[n++] = '\b'; break; case 'f': b[n++] = '\f'; break;
            case 'u':
                if (!jp_hex4(j, &cp)) { j->err = 1; break; }
                if (cp >= 0xD800 && cp < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                    uint32_t lo; j->p += 2;
                    if (!jp_hex4(j, &lo)) { j->err = 1; break; }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                if (cp < 0x80) b[n++] = (char)cp;
                else if (cp < 0x800) { b[n++] = (char)(0xC0 | (cp >> 6)); b[n++] = (char)(0x80 | (cp & 63)); }
                else if (cp < 0x10000) { b[n++] = (char)(0xE0 | (cp >> 12)); b[n++] = (char)(0x80 | ((cp >> 6) & 63)); b[n++] = (char)(0x80 | (cp & 63)); }
                else { b[n++] = (char)(0xF0 | (cp >> 18)); b[n++] = (char)(0x80 | ((cp >> 12) & 63)); b[n++] = (char)(0x80 | ((cp >> 6) & 63)); b[n++] = (char)(0x80 | (cp & 63)); }
                break;
            default: b[n++] = (char)c; break;      /* \" \\ \/ */
        }
        if (j->err) break;
    }
    if (j->p >= j->end) j->err = 1; else j->p++;   /* aspa de fechamento */
    b[n] = 0; *len = n;
    return b;
}

static void jv_push(JV *a, JV *item, char *key) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4;
        a->v = realloc(a->v, a->cap * sizeof(JV));
        if (a->t == J_OBJ) a->k = realloc(a->k, a->cap * sizeof(char *));
        if (!a->v || (a->t == J_OBJ && !a->k)) die("convert: sem memoria");
    }
    a->v[a->n] = *item;
    if (a->t == J_OBJ) a->k[a->n] = key;
    a->n++;
}

static void jp_value(JP *j, JV *out, int depth) {
    memset(out, 0, sizeof *out);
    jp_ws(j);
    if (j->p >= j->end || depth > 64) { j->err = 1; return; }
    const char c = *j->p;
    if (c == '"') { j->p++; out->t = J_STR; out->s = jp_string(j, &out->slen); return; }
    if (c == '{' || c == '[') {
        const char close = c == '{' ? '}' : ']';
        out->t = c == '{' ? J_OBJ : J_ARR; j->p++;
        jp_ws(j);
        if (j->p < j->end && *j->p == close) { j->p++; return; }
        for (;;) {
            char *key = NULL; JV item;
            jp_ws(j);
            if (c == '{') {
                if (j->p >= j->end || *j->p != '"') { j->err = 1; return; }
                j->p++; size_t kl; key = jp_string(j, &kl);
                jp_ws(j);
                if (j->p >= j->end || *j->p != ':') { j->err = 1; return; }
                j->p++;
            }
            jp_value(j, &item, depth + 1);
            if (j->err) return;
            jv_push(out, &item, key);
            jp_ws(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == close) { j->p++; return; }
            j->err = 1; return;
        }
    }
    if (j->end - j->p >= 4 && !memcmp(j->p, "true", 4)) { out->t = J_BOOL; out->i = 1; out->num = 1; j->p += 4; return; }
    if (j->end - j->p >= 5 && !memcmp(j->p, "false", 5)) { out->t = J_BOOL; j->p += 5; return; }
    if (j->end - j->p >= 4 && !memcmp(j->p, "null", 4)) { out->t = J_NULL; j->p += 4; return; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char tmp[64]; size_t n = 0; int isint = 1;
        while (j->p < j->end && n < 63 && strchr("+-0123456789.eE", *j->p)) { if (strchr(".eE", *j->p)) isint = 0; tmp[n++] = *j->p++; }
        tmp[n] = 0;
        out->t = J_NUM; out->num = strtod(tmp, NULL); out->isint = isint; out->i = isint ? strtoll(tmp, NULL, 10) : (int64_t)out->num;
        return;
    }
    j->err = 1;
}

static void jv_free(JV *a) {
    free(a->s);
    for (size_t i = 0; i < a->n; i++) { jv_free(&a->v[i]); if (a->k) free(a->k[i]); }
    free(a->v); free(a->k);
}

static JV *jget(const JV *o, const char *key) {
    if (!o || o->t != J_OBJ) return NULL;
    for (size_t i = 0; i < o->n; i++) if (!strcmp(o->k[i], key)) return &o->v[i];
    return NULL;
}
static double jnum(const JV *o, const char *key, double def) { const JV *v = jget(o, key); return v && v->t == J_NUM ? v->num : def; }
static const char *jstr(const JV *o, const char *key) { const JV *v = jget(o, key); return v && v->t == J_STR ? v->s : NULL; }

/* mapeia um arquivo e le o JSON dele; 1 = ok. Os dados do arquivo ficam mapeados ate o fim do programa */
static int cv_json_file(const char *path, JV *out) {
    int64_t sz = 0;
    const uint8_t *b = map_file(path, &sz);
    if (!b) return 0;
    JP j = { (const char *)b, (const char *)b + sz, 0 };
    jp_value(&j, out, 0);
    if (j.err) { fprintf(stderr, "convert: JSON invalido em %s (perto do byte %lld)\n", path, (long long)(j.p - (const char *)b)); exit(1); }
    return 1;
}

/* ---------- safetensors ---------- */
enum { ST_F32 = 0, ST_F16, ST_BF16 };
typedef struct { char *name; int shard; int dt; int nd; uint64_t shape[8]; uint64_t b, e; int used; } CvSt;
typedef struct { char *path; const uint8_t *base; int64_t size; uint64_t data0; } CvShard;

static size_t cv_esz(int dt) { return dt == ST_F32 ? 4 : 2; }

static int cv_st_cmp(const void *a, const void *b) { return strcmp(((const CvSt *)a)->name, ((const CvSt *)b)->name); }
static int cv_str_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

typedef struct { CvShard *sh; int nsh; CvSt *st; size_t nst, capst; } CvSrcSet;

static CvSt *cv_find(CvSrcSet *S, const char *name) {
    CvSt key; key.name = (char *)name;
    return bsearch(&key, S->st, S->nst, sizeof(CvSt), cv_st_cmp);
}

static void cv_open_shard(CvSrcSet *S, const char *path) {
    int64_t sz = 0;
    const uint8_t *b = map_file(path, &sz);
    if (!b || sz < 16) { fprintf(stderr, "convert: nao consegui abrir %s\n", path); exit(1); }
    uint64_t hl; memcpy(&hl, b, 8);
    if (hl > (uint64_t)sz - 8 || hl > (1ull << 30)) { fprintf(stderr, "convert: cabecalho safetensors invalido em %s\n", path); exit(1); }
    JP j = { (const char *)b + 8, (const char *)b + 8 + hl, 0 };
    JV h; jp_value(&j, &h, 0);
    if (j.err || h.t != J_OBJ) { fprintf(stderr, "convert: JSON do cabecalho invalido em %s\n", path); exit(1); }
    const int si = S->nsh++;
    S->sh = realloc(S->sh, (size_t)S->nsh * sizeof(CvShard));
    S->sh[si].path = strdup(path); S->sh[si].base = b; S->sh[si].size = sz; S->sh[si].data0 = 8 + hl;
    for (size_t i = 0; i < h.n; i++) {
        if (!strcmp(h.k[i], "__metadata__")) continue;
        const JV *e = &h.v[i], *dt = jget(e, "dtype"), *sh = jget(e, "shape"), *off = jget(e, "data_offsets");
        if (!dt || dt->t != J_STR || !sh || sh->t != J_ARR || !off || off->t != J_ARR || off->n != 2 || sh->n > 8) {
            fprintf(stderr, "convert: entrada invalida '%s' em %s\n", h.k[i], path); exit(1);
        }
        CvSt t; memset(&t, 0, sizeof t);
        t.name = strdup(h.k[i]); t.shard = si;
        if (!strcmp(dt->s, "F32")) t.dt = ST_F32; else if (!strcmp(dt->s, "F16")) t.dt = ST_F16; else if (!strcmp(dt->s, "BF16")) t.dt = ST_BF16;
        else { fprintf(stderr, "convert: tensor '%s' tem tipo %s, nao suportado (so F32, F16, BF16)\n", h.k[i], dt->s); exit(1); }
        t.nd = (int)sh->n;
        uint64_t cnt = 1;
        for (size_t d = 0; d < sh->n; d++) { t.shape[d] = (uint64_t)sh->v[d].i; cnt *= t.shape[d]; }
        t.b = (uint64_t)off->v[0].i; t.e = (uint64_t)off->v[1].i;
        if (t.e < t.b || 8 + hl + t.e > (uint64_t)sz || t.e - t.b != cnt * cv_esz(t.dt)) {
            fprintf(stderr, "convert: tensor '%s' em %s: deslocamentos ou tamanho inconsistentes (arquivo truncado?)\n", h.k[i], path); exit(1);
        }
        if (S->nst == S->capst) { S->capst = S->capst ? S->capst * 2 : 1024; S->st = realloc(S->st, S->capst * sizeof(CvSt)); if (!S->st) die("convert: sem memoria"); }
        S->st[S->nst++] = t;
    }
    jv_free(&h);
}

/* ---------- conversao de elementos ---------- */
static uint16_t cv_f32_to_bf16(float f) {
    uint32_t x; memcpy(&x, &f, 4);
    if ((x & 0x7fffffffu) > 0x7f800000u) return (uint16_t)((x >> 16) | 0x40);     /* NaN continua NaN */
    x += 0x7fffu + ((x >> 16) & 1u);                                                /* arredonda ao mais proximo, empate para par */
    return (uint16_t)(x >> 16);
}

static void cv_load(int dt, const uint8_t *p, float *dst, size_t n) {
    if (dt == ST_F32) { memcpy(dst, p, n * 4); return; }
    for (size_t i = 0; i < n; i++) {
        uint16_t h; memcpy(&h, p + 2 * i, 2);
        dst[i] = dt == ST_F16 ? f16_to_f32(h) : bf16_to_f32(h);
    }
}

/* ---------- plano de saida ---------- */
enum { EW_NONE = 0, EW_PLUS1, EW_NEGEXP };
typedef struct { const uint8_t *p; int dt; int st; } CvSrcRef;            /* st: indice em CvSrcSet.st (para devolver as paginas) */
typedef struct {
    char name[96];
    int nd; uint64_t dim[4];                 /* ordem do GGUF: dim[0] = coluna (contigua) */
    int layer;                               /* -1 = global */
    int keep32;                              /* tensor pequeno ou sensivel: sempre F32 nos modos de quantizacao */
    uint32_t otype; uint64_t nbytes, off;
    int nsrc; CvSrcRef *src; uint64_t rows_each;   /* varias fontes empilhadas: linhas por fonte */
    uint64_t A, B, C;                        /* linha de origem = (r / A) * B + r % A + C */
    uint64_t vro_off, vro_hd; int vro_nk, vro_r;   /* reordenacao de cabecas v nas linhas (a partir de vro_off) */
    uint64_t vco_hd;                         /* idem nas colunas */
    int ew;
} CvOut;

typedef struct {
    CvSrcSet S;
    CvOut *o; int no, cap;
    /* hiperparametros */
    char arch[16]; int moe;
    int layers, hidden, heads, kvh, hd, dk, dv, nk, nv, conv, interval, nexp, nused, ffe, ffs, ffd, vocab_rows;
    char prefix[40];
    int skipped, unused_warn;
} CvCtx;

static CvOut *cv_new(CvCtx *c, const char *name, int layer) {
    if (c->no == c->cap) { c->cap = c->cap ? c->cap * 2 : 256; c->o = realloc(c->o, (size_t)c->cap * sizeof(CvOut)); if (!c->o) die("convert: sem memoria"); }
    CvOut *o = &c->o[c->no++];
    memset(o, 0, sizeof *o);
    snprintf(o->name, sizeof o->name, "%s", name);
    o->layer = layer;
    return o;
}

static CvSt *cv_need(CvCtx *c, const char *name) {
    CvSt *t = cv_find(&c->S, name);
    if (!t) { fprintf(stderr, "convert: tensor '%s' nao encontrado no checkpoint\n", name); exit(1); }
    t->used = 1;
    return t;
}
static CvSt *cv_opt(CvCtx *c, const char *name) { CvSt *t = cv_find(&c->S, name); if (t) t->used = 1; return t; }

static void cv_addsrc(CvCtx *c, CvOut *o, CvSt *t) {
    o->src = realloc(o->src, (size_t)(o->nsrc + 1) * sizeof(CvSrcRef));
    CvShard *sh = &c->S.sh[t->shard];
    o->src[o->nsrc].p = sh->base + sh->data0 + t->b; o->src[o->nsrc].dt = t->dt; o->src[o->nsrc].st = (int)(t - c->S.st);
    o->nsrc++;
}

/* dimensoes do GGUF = forma do HF invertida; `drop` remove um eixo de tamanho 1 (indice no HF; -1 = nenhum) */
static void cv_dims(CvOut *o, const CvSt *t, int drop) {
    int n = 0;
    for (int d = t->nd - 1; d >= 0; d--) {
        if (d == drop) { if (t->shape[d] != 1) { fprintf(stderr, "convert: %s: eixo %d de '%s' deveria ter tamanho 1\n", o->name, d, t->name); exit(1); } continue; }
        if (n >= 4) { fprintf(stderr, "convert: tensor '%s' com dimensoes demais\n", t->name); exit(1); }
        o->dim[n++] = t->shape[d];
    }
    o->nd = n;
}
static uint64_t cv_rows(const CvOut *o) { uint64_t r = 1; for (int d = 1; d < o->nd; d++) r *= o->dim[d]; return r; }
static uint64_t cv_elems(const CvOut *o) { return o->dim[0] * cv_rows(o); }
static void cv_finish_map(CvOut *o) { if (!o->A) { o->A = o->B = cv_rows(o) ? cv_rows(o) : 1; o->C = 0; } }

/* tensor simples: uma fonte, mesmas dimensoes. ew: transformacao por elemento */
static CvOut *cv_simple(CvCtx *c, const char *gname, const char *hf, int layer, int ew, int keep32, int drop) {
    CvSt *t = cv_need(c, hf);
    CvOut *o = cv_new(c, gname, layer);
    cv_dims(o, t, drop); cv_addsrc(c, o, t);
    o->ew = ew; o->keep32 = keep32 || o->nd == 1;
    cv_finish_map(o);
    return o;
}

static void cv_vreorder_check(const CvCtx *c) {
    if (c->nk <= 0 || c->nv % c->nk) { fprintf(stderr, "convert: linear_num_value_heads (%d) nao e multiplo de linear_num_key_heads (%d)\n", c->nv, c->nk); exit(1); }
}

static void cv_layer_ffn_dense(CvCtx *c, int l, const char *lp) {
    static const char *const hf[3] = { "gate_proj", "up_proj", "down_proj" }, *const gg[3] = { "ffn_gate", "ffn_up", "ffn_down" };
    for (int i = 0; i < 3; i++) {
        char a[200], b[100]; snprintf(a, sizeof a, "%smlp.%s.weight", lp, hf[i]); snprintf(b, sizeof b, "blk.%d.%s.weight", l, gg[i]);
        cv_simple(c, b, a, l, EW_NONE, 0, -1);
    }
}

static void cv_layer_moe(CvCtx *c, int l, const char *lp) {
    char a[220], b[100];
    snprintf(a, sizeof a, "%smlp.gate.weight", lp); snprintf(b, sizeof b, "blk.%d.ffn_gate_inp.weight", l);
    cv_simple(c, b, a, l, EW_NONE, 1, -1);
    const int E = c->nexp;
    CvSt *gu = NULL, *dn = NULL;
    snprintf(a, sizeof a, "%smlp.experts.gate_up_proj", lp); gu = cv_opt(c, a);
    if (!gu) { snprintf(a, sizeof a, "%smlp.experts.gate_up_proj.weight", lp); gu = cv_opt(c, a); }
    if (gu) {
        snprintf(a, sizeof a, "%smlp.experts.down_proj", lp); dn = cv_opt(c, a);
        if (!dn) { snprintf(a, sizeof a, "%smlp.experts.down_proj.weight", lp); dn = cv_need(c, a); }
        if (gu->nd != 3 || (int)gu->shape[0] != E || gu->shape[1] % 2) { fprintf(stderr, "convert: forma inesperada de %s\n", gu->name); exit(1); }
        const uint64_t F = gu->shape[1] / 2;
        for (int u = 0; u < 2; u++) {
            snprintf(b, sizeof b, "blk.%d.ffn_%s_exps.weight", l, u ? "up" : "gate");
            CvOut *o = cv_new(c, b, l);
            o->nd = 3; o->dim[0] = gu->shape[2]; o->dim[1] = F; o->dim[2] = (uint64_t)E;
            cv_addsrc(c, o, gu);
            o->A = F; o->B = 2 * F; o->C = u ? F : 0;      /* gate = primeiras F linhas de cada especialista, up = as F seguintes */
        }
        snprintf(b, sizeof b, "blk.%d.ffn_down_exps.weight", l);
        CvOut *o = cv_new(c, b, l);
        cv_dims(o, dn, -1); cv_addsrc(c, o, dn); cv_finish_map(o);
    } else {
        static const char *const hf[3] = { "gate_proj", "up_proj", "down_proj" }, *const gg[3] = { "ffn_gate_exps", "ffn_up_exps", "ffn_down_exps" };
        for (int i = 0; i < 3; i++) {
            CvOut *o = NULL;
            for (int e = 0; e < E; e++) {
                snprintf(a, sizeof a, "%smlp.experts.%d.%s.weight", lp, e, hf[i]);
                CvSt *t = cv_need(c, a);
                if (t->nd != 2) { fprintf(stderr, "convert: forma inesperada de %s\n", t->name); exit(1); }
                if (!o) {
                    snprintf(b, sizeof b, "blk.%d.%s.weight", l, gg[i]);
                    o = cv_new(c, b, l);
                    o->nd = 3; o->dim[0] = t->shape[1]; o->dim[1] = t->shape[0]; o->dim[2] = (uint64_t)E;
                    o->rows_each = t->shape[0];
                } else if (t->shape[0] != o->dim[1] || t->shape[1] != o->dim[0]) { fprintf(stderr, "convert: especialista %d de %s com forma diferente\n", e, gg[i]); exit(1); }
                cv_addsrc(c, o, t);
            }
            cv_finish_map(o);
        }
    }
    /* especialista compartilhado e seu portao */
    static const char *const sh[3] = { "gate_proj", "up_proj", "down_proj" }, *const sg[3] = { "ffn_gate_shexp", "ffn_up_shexp", "ffn_down_shexp" };
    for (int i = 0; i < 3; i++) {
        snprintf(a, sizeof a, "%smlp.shared_expert.%s.weight", lp, sh[i]); snprintf(b, sizeof b, "blk.%d.%s.weight", l, sg[i]);
        cv_simple(c, b, a, l, EW_NONE, 0, -1);
    }
    snprintf(a, sizeof a, "%smlp.shared_expert_gate.weight", lp); snprintf(b, sizeof b, "blk.%d.ffn_gate_inp_shexp.weight", l);
    cv_simple(c, b, a, l, EW_NONE, 1, 0);        /* [1, D] -> (D) */
}

static void cv_layer(CvCtx *c, int l) {
    char lp[80], a[220], b[100];
    snprintf(lp, sizeof lp, "%slayers.%d.", c->prefix, l);
    snprintf(a, sizeof a, "%sinput_layernorm.weight", lp); snprintf(b, sizeof b, "blk.%d.attn_norm.weight", l);
    cv_simple(c, b, a, l, EW_PLUS1, 1, -1);
    snprintf(a, sizeof a, "%sself_attn.q_proj.weight", lp);
    if (cv_find(&c->S, a)) {
        static const char *const hf[4] = { "q_proj", "k_proj", "v_proj", "o_proj" }, *const gg[4] = { "attn_q", "attn_k", "attn_v", "attn_output" };
        for (int i = 0; i < 4; i++) {
            snprintf(a, sizeof a, "%sself_attn.%s.weight", lp, hf[i]); snprintf(b, sizeof b, "blk.%d.%s.weight", l, gg[i]);
            cv_simple(c, b, a, l, EW_NONE, 0, -1);
        }
        for (int i = 0; i < 2; i++) {
            snprintf(a, sizeof a, "%sself_attn.%c_norm.weight", lp, i ? 'k' : 'q'); snprintf(b, sizeof b, "blk.%d.attn_%c_norm.weight", l, i ? 'k' : 'q');
            cv_simple(c, b, a, l, EW_PLUS1, 1, -1);
        }
    } else {
        cv_vreorder_check(c);
        const int r = c->nv / c->nk;
        const uint64_t keydim = (uint64_t)c->nk * c->dk;
        CvOut *o;
        snprintf(a, sizeof a, "%slinear_attn.in_proj_qkv.weight", lp); snprintf(b, sizeof b, "blk.%d.attn_qkv.weight", l);
        o = cv_simple(c, b, a, l, EW_NONE, 0, -1); o->vro_off = 2 * keydim; o->vro_hd = (uint64_t)c->dv; o->vro_nk = c->nk; o->vro_r = r;
        snprintf(a, sizeof a, "%slinear_attn.in_proj_z.weight", lp); snprintf(b, sizeof b, "blk.%d.attn_gate.weight", l);
        o = cv_simple(c, b, a, l, EW_NONE, 0, -1); o->vro_off = 0; o->vro_hd = (uint64_t)c->dv; o->vro_nk = c->nk; o->vro_r = r;
        snprintf(a, sizeof a, "%slinear_attn.in_proj_b.weight", lp); snprintf(b, sizeof b, "blk.%d.ssm_beta.weight", l);
        o = cv_simple(c, b, a, l, EW_NONE, 1, -1); o->vro_hd = 1; o->vro_nk = c->nk; o->vro_r = r;
        snprintf(a, sizeof a, "%slinear_attn.in_proj_a.weight", lp); snprintf(b, sizeof b, "blk.%d.ssm_alpha.weight", l);
        o = cv_simple(c, b, a, l, EW_NONE, 1, -1); o->vro_hd = 1; o->vro_nk = c->nk; o->vro_r = r;
        snprintf(a, sizeof a, "%slinear_attn.conv1d.weight", lp); snprintf(b, sizeof b, "blk.%d.ssm_conv1d.weight", l);
        o = cv_simple(c, b, a, l, EW_NONE, 1, 1); o->vro_off = 2 * keydim; o->vro_hd = (uint64_t)c->dv; o->vro_nk = c->nk; o->vro_r = r;   /* [C,1,K] -> (K,C) */
        snprintf(a, sizeof a, "%slinear_attn.A_log", lp); snprintf(b, sizeof b, "blk.%d.ssm_a", l);
        o = cv_simple(c, b, a, l, EW_NEGEXP, 1, -1); o->vco_hd = 1; o->vro_nk = c->nk; o->vro_r = r;
        snprintf(a, sizeof a, "%slinear_attn.dt_bias", lp); snprintf(b, sizeof b, "blk.%d.ssm_dt.bias", l);
        o = cv_simple(c, b, a, l, EW_NONE, 1, -1); o->vco_hd = 1; o->vro_nk = c->nk; o->vro_r = r;
        snprintf(a, sizeof a, "%slinear_attn.norm.weight", lp); snprintf(b, sizeof b, "blk.%d.ssm_norm.weight", l);
        cv_simple(c, b, a, l, EW_NONE, 1, -1);
        snprintf(a, sizeof a, "%slinear_attn.out_proj.weight", lp); snprintf(b, sizeof b, "blk.%d.ssm_out.weight", l);
        o = cv_simple(c, b, a, l, EW_NONE, 0, -1); o->vco_hd = (uint64_t)c->dv; o->vro_nk = c->nk; o->vro_r = r;
    }
    snprintf(a, sizeof a, "%spost_attention_layernorm.weight", lp); snprintf(b, sizeof b, "blk.%d.post_attention_norm.weight", l);
    cv_simple(c, b, a, l, EW_PLUS1, 1, -1);
    if (c->moe) cv_layer_moe(c, l, lp); else cv_layer_ffn_dense(c, l, lp);
}

/* ---------- tipo de saida por tensor ---------- */
enum { CM_F32 = 0, CM_F16, CM_BF16, CM_Q8, CM_Q6, CM_Q5, CM_Q4, CM_Q4M };
static int cv_mode_parse(const char *s) {
    static const struct { const char *n; int m; } M[] = { { "f32", CM_F32 }, { "f16", CM_F16 }, { "bf16", CM_BF16 }, { "q8_0", CM_Q8 }, { "q6_k", CM_Q6 },
        { "q5_k", CM_Q5 }, { "q5_k_m", CM_Q5 }, { "q4_k", CM_Q4 }, { "q4_k_s", CM_Q4 }, { "q4_k_m", CM_Q4M } };
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) if (ci_eq(s, (int)strlen(s), M[i].n)) return M[i].m;
    return -1;
}
static const char *cv_mode_name(int m) { static const char *const N[] = { "F32", "F16", "BF16", "Q8_0", "Q6_K", "Q5_K_M", "Q4_K", "Q4_K_M" }; return N[m]; }
static int cv_mode_ft(int m) { static const int F[] = { 0, 1, 32, 7, 18, 17, 14, 15 }; return F[m]; }

/* camada de um bloco: o down dos especialistas ganha mais bits nas camadas das pontas e a cada terceira do meio (a mesma
   ideia do mix "M" dos formatos K: onde o erro mais pesa) */
static int cv_more_bits(int l, int n) {
    if (l < n / 8 || l >= n - n / 8) return 1;
    return (l - n / 8) % 3 == 2;
}

static uint32_t cv_choose(const CvOut *o, int mode, int nlayers) {
    const uint64_t cols = o->dim[0];
    if (mode == CM_F32 || o->keep32) return T_F32;
    if (mode == CM_F16) return T_F16;
    if (mode == CM_BF16) return T_BF16;
    const int is_embd = !strcmp(o->name, "token_embd.weight"), is_out = !strcmp(o->name, "output.weight");
    const int is_exp = strstr(o->name, "_exps") != NULL, is_down = strstr(o->name, "ffn_down") != NULL;
    uint32_t t;
    switch (mode) {
        case CM_Q8: t = T_Q8_0; break;
        case CM_Q6: t = T_Q6_K; break;
        case CM_Q5: t = (is_embd || is_out) ? T_Q6_K : T_Q5_K; break;
        case CM_Q4: t = (is_embd || is_out) ? T_Q6_K : T_Q4_K; break;
        default:   /* Q4_K_M dinamico */
            if (is_embd) t = T_Q8_0;
            else if (is_out) t = T_Q6_K;
            else if (!is_exp) t = T_Q8_0;                                        /* atencao, DeltaNet, especialista compartilhado, FFN denso */
            else if (is_down) t = cv_more_bits(o->layer, nlayers) ? T_Q6_K : T_Q4_K;
            else t = T_Q4_K;
            break;
    }
    if (cols % (uint64_t)qunit(t)) t = cols % 32 == 0 ? T_Q8_0 : T_F16;          /* colunas que nao fecham o bloco */
    return t;
}

/* ---------- gravacao ---------- */
typedef struct { const CvOut *o; uint64_t row0, nrows; uint8_t *out; size_t rowbytes; uint32_t otype; } CvJob;

static void cv_row(const CvOut *o, uint64_t r, float *dst, float *tmp) {
    const uint64_t cols = o->dim[0];
    size_t s = 0; uint64_t rr = r;
    if (o->nsrc > 1) { s = (size_t)(r / o->rows_each); rr = r % o->rows_each; }
    uint64_t sr = (rr / o->A) * o->B + rr % o->A + o->C;
    if (o->vro_hd && sr >= o->vro_off) {
        const uint64_t rel = sr - o->vro_off, c = rel % o->vro_hd, h = rel / o->vro_hd, a = h / (uint64_t)o->vro_nk, b = h % (uint64_t)o->vro_nk;
        sr = o->vro_off + (b * (uint64_t)o->vro_r + a) * o->vro_hd + c;
    }
    const CvSrcRef *S = &o->src[s];
    const uint8_t *rowp = S->p + sr * cols * cv_esz(S->dt);
    if (o->vco_hd) {
        cv_load(S->dt, rowp, tmp, cols);
        for (uint64_t j = 0; j < cols; j++) {
            const uint64_t c = j % o->vco_hd, h = j / o->vco_hd, a = h / (uint64_t)o->vro_nk, b = h % (uint64_t)o->vro_nk;
            dst[j] = tmp[(b * (uint64_t)o->vro_r + a) * o->vco_hd + c];
        }
    } else cv_load(S->dt, rowp, dst, cols);
    if (o->ew == EW_PLUS1) for (uint64_t j = 0; j < cols; j++) dst[j] += 1.0f;
    else if (o->ew == EW_NEGEXP) for (uint64_t j = 0; j < cols; j++) dst[j] = -expf(dst[j]);
}

static void cv_job(void *ctx, int i0, int i1) {
    const CvJob *J = ctx;
    const uint64_t cols = J->o->dim[0];
    float *f = xalloc((size_t)cols, sizeof(float)), *tmp = xalloc((size_t)cols, sizeof(float));
    for (int i = i0; i < i1; i++) {
        uint8_t *dst = J->out + (size_t)i * J->rowbytes;
        cv_row(J->o, J->row0 + (uint64_t)i, f, tmp);
        if (J->otype == T_F32) memcpy(dst, f, cols * 4);
        else if (J->otype == T_F16) for (uint64_t j = 0; j < cols; j++) { const uint16_t h = f32_to_f16(f[j]); memcpy(dst + 2 * j, &h, 2); }
        else if (J->otype == T_BF16) for (uint64_t j = 0; j < cols; j++) { const uint16_t h = cv_f32_to_bf16(f[j]); memcpy(dst + 2 * j, &h, 2); }
        else enc_block(J->otype, f, dst, (size_t)cols);
    }
    free(f); free(tmp);
}

/* ---------- GGUF: valores ---------- */
static void cv_kv_f32(GBuf *b, const char *key, float v) { gbuf_gstr(b, key, strlen(key)); gbuf_u32(b, 6); gbuf_bytes(b, &v, 4); }
static void cv_kv_bool(GBuf *b, const char *key, int v) { gbuf_gstr(b, key, strlen(key)); gbuf_u32(b, 7); uint8_t x = v ? 1 : 0; gbuf_bytes(b, &x, 1); }
static void cv_kv_arr_i32(GBuf *b, const char *key, const int32_t *v, int n) {
    gbuf_gstr(b, key, strlen(key)); gbuf_u32(b, 9); gbuf_u32(b, 5); gbuf_u64(b, (uint64_t)n);
    for (int i = 0; i < n; i++) gbuf_bytes(b, &v[i], 4);
}

/* le o arquivo de texto inteiro (mapeado); NULL se nao existe */
static char *cv_read_text(const char *path) {
    int64_t sz = 0;
    const uint8_t *b = map_file(path, &sz);
    if (!b) return NULL;
    char *s = xalloc((size_t)sz + 1, 1); memcpy(s, b, (size_t)sz);
    return s;
}

static int cv_tok_id(char **toks, int n, const char *s) {
    for (int i = 0; i < n; i++) if (toks[i] && !strcmp(toks[i], s)) return i;
    return -1;
}

/* campo de token especial do tokenizer_config.json: string ou {"content": ...} */
static const char *cv_special_str(const JV *cfg, const char *key) {
    const JV *v = jget(cfg, key);
    if (!v) return NULL;
    if (v->t == J_STR) return v->s;
    if (v->t == J_OBJ) return jstr(v, "content");
    return NULL;
}

/* tokenizador e modelo de conversa -> KV */
static void cv_tokenizer(const char *dir, CvCtx *c, const JV *cfgtop, const JV *txt, GBuf *kv, uint64_t *nkv) {
    char path[2048];
    JV tj; snprintf(path, sizeof path, "%s/tokenizer.json", dir);
    if (!cv_json_file(path, &tj)) { fprintf(stderr, "convert: %s nao existe (so tokenizadores BPE em tokenizer.json sao suportados)\n", path); exit(1); }
    JV tc; memset(&tc, 0, sizeof tc);
    snprintf(path, sizeof path, "%s/tokenizer_config.json", dir);
    const int has_tc = cv_json_file(path, &tc);
    const JV *model = jget(&tj, "model"), *vocab = jget(model, "vocab"), *merges = jget(model, "merges"), *added = jget(&tj, "added_tokens");
    if (!model || !vocab || vocab->t != J_OBJ || !merges || merges->t != J_ARR) { fprintf(stderr, "convert: tokenizer.json sem model.vocab/model.merges (so BPE)\n"); exit(1); }
    const char *mty = jstr(model, "type");
    if (mty && strcmp(mty, "BPE")) { fprintf(stderr, "convert: tokenizador do tipo %s nao suportado (so BPE)\n", mty); exit(1); }
    int64_t maxid = -1;
    for (size_t i = 0; i < vocab->n; i++) if (vocab->v[i].i > maxid) maxid = vocab->v[i].i;
    if (added) for (size_t i = 0; i < added->n; i++) { const int64_t id = (int64_t)jnum(&added->v[i], "id", -1); if (id > maxid) maxid = id; }
    int V = c->vocab_rows > 0 ? c->vocab_rows : (int)jnum(txt, "vocab_size", 0);
    if (V < maxid + 1) {
        if (c->vocab_rows > 0) { fprintf(stderr, "convert: o tokenizador tem ids ate %lld e o embedding so %d linhas\n", (long long)maxid, c->vocab_rows); exit(1); }
        V = (int)maxid + 1;
    }
    char **toks = xalloc((size_t)V, sizeof(char *));
    int32_t *types = xalloc((size_t)V, 4);
    for (int i = 0; i < V; i++) types[i] = 5;                      /* nao usado: preenchimento ate o tamanho do embedding */
    for (size_t i = 0; i < vocab->n; i++) { const int64_t id = vocab->v[i].i; toks[id] = vocab->k[i]; types[id] = 1; }
    if (added) for (size_t i = 0; i < added->n; i++) {
        const JV *e = &added->v[i]; const int64_t id = (int64_t)jnum(e, "id", -1); const char *ct = jstr(e, "content");
        if (id < 0 || !ct) continue;
        toks[id] = (char *)ct;
        const JV *sp = jget(e, "special");
        types[id] = sp && sp->t == J_BOOL && sp->i ? 3 : 4;        /* controle (3) ou definido pelo usuario (4) */
    }
    for (int i = 0; i < V; i++) if (!toks[i]) { char tmp[32]; snprintf(tmp, sizeof tmp, "[PAD%d]", i); toks[i] = strdup(tmp); }

    gkv_str(kv, "tokenizer.ggml.model", "gpt2"); (*nkv)++;
    gkv_str(kv, "tokenizer.ggml.pre", "qwen35"); (*nkv)++;     /* a pre-tokenizacao da familia Qwen3.5/3.6 (o motor conhece este nome) */
    gbuf_gstr(kv, "tokenizer.ggml.tokens", strlen("tokenizer.ggml.tokens")); gbuf_u32(kv, 9); gbuf_u32(kv, 8); gbuf_u64(kv, (uint64_t)V);
    for (int i = 0; i < V; i++) gbuf_gstr(kv, toks[i], strlen(toks[i]));
    (*nkv)++;
    gbuf_gstr(kv, "tokenizer.ggml.token_type", strlen("tokenizer.ggml.token_type")); gbuf_u32(kv, 9); gbuf_u32(kv, 5); gbuf_u64(kv, (uint64_t)V);
    gbuf_bytes(kv, types, (size_t)V * 4);
    (*nkv)++;
    gbuf_gstr(kv, "tokenizer.ggml.merges", strlen("tokenizer.ggml.merges")); gbuf_u32(kv, 9); gbuf_u32(kv, 8); gbuf_u64(kv, (uint64_t)merges->n);
    for (size_t i = 0; i < merges->n; i++) {
        const JV *m = &merges->v[i];
        if (m->t == J_STR) gbuf_gstr(kv, m->s, m->slen);
        else if (m->t == J_ARR && m->n == 2 && m->v[0].t == J_STR && m->v[1].t == J_STR) {   /* formato novo: ["a","b"] */
            gbuf_u64(kv, m->v[0].slen + 1 + m->v[1].slen);
            gbuf_bytes(kv, m->v[0].s, m->v[0].slen); gbuf_bytes(kv, " ", 1); gbuf_bytes(kv, m->v[1].s, m->v[1].slen);
        } else { fprintf(stderr, "convert: merge %zu invalido\n", i); exit(1); }
    }
    (*nkv)++;

    /* ids especiais: eos e pad pelo tokenizer_config.json (token), senao pelos ids do config.json */
    int eos = -1, bos = -1, pad = -1;
    const char *se = has_tc ? cv_special_str(&tc, "eos_token") : NULL, *sb = has_tc ? cv_special_str(&tc, "bos_token") : NULL, *sp = has_tc ? cv_special_str(&tc, "pad_token") : NULL;
    if (se) eos = cv_tok_id(toks, V, se);
    if (sb) bos = cv_tok_id(toks, V, sb);
    if (sp) pad = cv_tok_id(toks, V, sp);
    const JV *ei = jget(txt, "eos_token_id"); if (!ei) ei = jget(cfgtop, "eos_token_id");
    if (eos < 0 && ei) eos = (int)(ei->t == J_ARR && ei->n ? ei->v[0].i : ei->i);
    const JV *bi = jget(txt, "bos_token_id"); if (!bi) bi = jget(cfgtop, "bos_token_id");
    if (bos < 0 && bi && bi->t == J_NUM) bos = (int)bi->i;
    const JV *pi = jget(txt, "pad_token_id"); if (!pi) pi = jget(cfgtop, "pad_token_id");
    if (pad < 0 && pi && pi->t == J_NUM) pad = (int)pi->i;
    if (eos < 0) { fprintf(stderr, "convert: nao achei o token de fim de turno (eos_token no tokenizer_config.json ou eos_token_id no config.json)\n"); exit(1); }
    if (bos >= 0) { gkv_u32(kv, "tokenizer.ggml.bos_token_id", (uint32_t)bos); (*nkv)++; }
    gkv_u32(kv, "tokenizer.ggml.eos_token_id", (uint32_t)eos); (*nkv)++;
    if (pad >= 0) { gkv_u32(kv, "tokenizer.ggml.padding_token_id", (uint32_t)pad); (*nkv)++; }
    { const JV *ab = has_tc ? jget(&tc, "add_bos_token") : NULL; cv_kv_bool(kv, "tokenizer.ggml.add_bos_token", ab && ab->t == J_BOOL && ab->i); (*nkv)++; }

    /* modelo de conversa: chat_template.jinja, chat_template.json ou tokenizer_config.json */
    char *tmpl = NULL;
    snprintf(path, sizeof path, "%s/chat_template.jinja", dir); tmpl = cv_read_text(path);
    if (!tmpl) {
        JV ct; snprintf(path, sizeof path, "%s/chat_template.json", dir);
        if (cv_json_file(path, &ct)) { const char *s = jstr(&ct, "chat_template"); if (s) tmpl = strdup(s); }
    }
    if (!tmpl && has_tc) {
        const JV *v = jget(&tc, "chat_template");
        if (v && v->t == J_STR) tmpl = strdup(v->s);
        else if (v && v->t == J_ARR) {              /* lista de {name, template}: usa a "default" ou a primeira */
            const JV *pick = NULL;
            for (size_t i = 0; i < v->n; i++) { const char *nm = jstr(&v->v[i], "name"); if (nm && !strcmp(nm, "default")) pick = &v->v[i]; }
            if (!pick && v->n) pick = &v->v[0];
            if (pick && jstr(pick, "template")) tmpl = strdup(jstr(pick, "template"));
        }
    }
    if (tmpl) { gkv_str(kv, "tokenizer.chat_template", tmpl); (*nkv)++; }
    else fprintf(stderr, "convert: aviso: nenhum modelo de conversa encontrado (chat_template.jinja, chat_template.json ou tokenizer_config.json)\n");
    printf("  tokenizador: %d tokens (%d ids de preenchimento), %zu merges, eos=%d bos=%d pad=%d, modelo de conversa %s\n", V,
           (int)(V - vocab->n - (added ? added->n : 0)), merges->n, eos, bos, pad, tmpl ? "sim" : "nao");
    free(types);
}

/* ---------- configuracao -> hiperparametros e KV ---------- */
static int cv_i(const JV *txt, const JV *top, const char *key, int def) {
    const JV *v = jget(txt, key); if (!v) v = jget(top, key);
    return v && v->t == J_NUM ? (int)v->i : def;
}

/* arquivos de pesos: os do indice (model.safetensors.index.json), se existe, senao todos os *.safetensors da pasta.
   Abre cada um e junta os tensores em S (ordenados por nome). Devolve o numero de arquivos, ou -1 */
static int cv_open_all(const char *dir, CvSrcSet *S) {
    char path[2048];
    char **files = NULL; int nf = 0;
    JV idx; snprintf(path, sizeof path, "%s/model.safetensors.index.json", dir);
    if (cv_json_file(path, &idx)) {
        const JV *wm = jget(&idx, "weight_map");
        if (wm && wm->t == J_OBJ) for (size_t i = 0; i < wm->n; i++) {
            if (wm->v[i].t != J_STR) continue;
            int dup = 0; for (int k = 0; k < nf; k++) if (!strcmp(files[k], wm->v[i].s)) { dup = 1; break; }
            if (!dup) { files = realloc(files, (size_t)(nf + 1) * sizeof(char *)); files[nf++] = strdup(wm->v[i].s); }
        }
    }
    if (!nf) {
        DIR *d = opendir(dir);
        if (!d) { fprintf(stderr, "convert: nao consegui abrir a pasta %s\n", dir); return -1; }
        struct dirent *de;
        while ((de = readdir(d))) {
            const size_t l = strlen(de->d_name);
            if (l > 12 && !strcmp(de->d_name + l - 12, ".safetensors")) { files = realloc(files, (size_t)(nf + 1) * sizeof(char *)); files[nf++] = strdup(de->d_name); }
        }
        closedir(d);
        qsort(files, (size_t)nf, sizeof(char *), cv_str_cmp);
    }
    if (!nf) { fprintf(stderr, "convert: nenhum arquivo .safetensors em %s\n", dir); return -1; }
    for (int i = 0; i < nf; i++) { snprintf(path, sizeof path, "%s/%s", dir, files[i]); cv_open_shard(S, path); free(files[i]); }
    free(files);
    qsort(S->st, S->nst, sizeof(CvSt), cv_st_cmp);
    return nf;
}

/* katana convert <pasta> --list: tensores dos safetensors (nome, tipo, forma, bytes), para conferir o leitor e o checkpoint */
static int cv_list(const char *dir) {
    CvSrcSet S; memset(&S, 0, sizeof S);
    const int nf = cv_open_all(dir, &S);
    if (nf < 0) return 1;
    uint64_t total = 0;
    for (size_t i = 0; i < S.nst; i++) {
        const CvSt *t = &S.st[i];
        printf("%s  %s  [", t->name, t->dt == ST_F32 ? "F32" : t->dt == ST_F16 ? "F16" : "BF16");
        for (int d = 0; d < t->nd; d++) printf(d ? ",%llu" : "%llu", (unsigned long long)t->shape[d]);
        printf("]  %llu bytes  (arquivo %d)\n", (unsigned long long)(t->e - t->b), t->shard);
        total += t->e - t->b;
    }
    printf("%d arquivos, %zu tensores, %.3f GB\n", nf, S.nst, total / 1073741824.0);
    return 0;
}

static int cv_run(const char *dir, const char *out_path, int mode) {
    CvCtx *c = xalloc(1, sizeof(CvCtx));
    char path[2048];
    JV cfg; snprintf(path, sizeof path, "%s/config.json", dir);
    if (!cv_json_file(path, &cfg)) { fprintf(stderr, "convert: %s nao existe\n", path); return 1; }
    const JV *tx = jget(&cfg, "text_config"); const JV *txt = tx && tx->t == J_OBJ ? tx : &cfg;
    const JV *archs = jget(&cfg, "architectures");
    const char *an = archs && archs->t == J_ARR && archs->n && archs->v[0].t == J_STR ? archs->v[0].s : "";
    const char *mt = jstr(&cfg, "model_type"); if (!mt) mt = "";
    const char *tmt = jstr(txt, "model_type"); if (!tmt) tmt = "";
    if (strstr(an, "Qwen3_5Moe") || !strncmp(mt, "qwen3_5_moe", 11) || !strncmp(tmt, "qwen3_5_moe", 11)) { strcpy(c->arch, "qwen35moe"); c->moe = 1; }
    else if (strstr(an, "Qwen3_5") || !strncmp(mt, "qwen3_5", 7) || !strncmp(tmt, "qwen3_5", 7)) { strcpy(c->arch, "qwen35"); c->moe = 0; }
    else { fprintf(stderr, "convert: arquitetura '%s' (model_type '%s') nao suportada. Suportadas: Qwen3_5MoeForConditionalGeneration / Qwen3_5MoeForCausalLM (qwen35moe) e Qwen3_5ForConditionalGeneration / Qwen3_5ForCausalLM (qwen35).\n", an, mt); return 1; }

    c->layers = cv_i(txt, &cfg, "num_hidden_layers", 0); c->hidden = cv_i(txt, &cfg, "hidden_size", 0);
    c->heads = cv_i(txt, &cfg, "num_attention_heads", 0); c->kvh = cv_i(txt, &cfg, "num_key_value_heads", c->heads);
    c->hd = cv_i(txt, &cfg, "head_dim", c->heads ? c->hidden / c->heads : 0);
    c->dk = cv_i(txt, &cfg, "linear_key_head_dim", 0); c->dv = cv_i(txt, &cfg, "linear_value_head_dim", 0);
    c->nk = cv_i(txt, &cfg, "linear_num_key_heads", 0); c->nv = cv_i(txt, &cfg, "linear_num_value_heads", 0);
    c->conv = cv_i(txt, &cfg, "linear_conv_kernel_dim", 4); c->interval = cv_i(txt, &cfg, "full_attention_interval", 4);
    c->nexp = cv_i(txt, &cfg, "num_experts", 0); c->nused = cv_i(txt, &cfg, "num_experts_per_tok", 0);
    c->ffe = cv_i(txt, &cfg, "moe_intermediate_size", 0); c->ffs = cv_i(txt, &cfg, "shared_expert_intermediate_size", 0);
    c->ffd = cv_i(txt, &cfg, "intermediate_size", 0);
    if (c->layers <= 0 || c->hidden <= 0 || c->heads <= 0 || c->dk <= 0 || c->dv <= 0 || c->nk <= 0 || c->nv <= 0 || (c->moe && (c->nexp <= 0 || c->nused <= 0 || c->ffe <= 0)) || (!c->moe && c->ffd <= 0)) {
        fprintf(stderr, "convert: config.json incompleto (faltam hiperparametros da arquitetura %s)\n", c->arch); return 1;
    }
    cv_vreorder_check(c);

    const int nf = cv_open_all(dir, &c->S);
    if (nf < 0) return 1;
    qsort(c->S.st, c->S.nst, sizeof(CvSt), cv_st_cmp);
    printf("convert: %s -> %s  (%s, %d camadas, %d arquivos de pesos, %zu tensores no checkpoint, tipo %s)\n", dir, out_path, c->arch, c->layers, nf, c->S.nst, cv_mode_name(mode));

    /* prefixo dos tensores de texto */
    if (cv_find(&c->S, "model.language_model.embed_tokens.weight")) strcpy(c->prefix, "model.language_model.");
    else if (cv_find(&c->S, "model.embed_tokens.weight")) strcpy(c->prefix, "model.");
    else { fprintf(stderr, "convert: nao achei embed_tokens.weight (esperado model.language_model.embed_tokens.weight ou model.embed_tokens.weight)\n"); return 1; }

    /* plano: globais e camadas */
    char nm[220];
    { CvOut *o; snprintf(nm, sizeof nm, "%sembed_tokens.weight", c->prefix);
      o = cv_simple(c, "token_embd.weight", nm, -1, EW_NONE, 0, -1); c->vocab_rows = (int)o->dim[1];
      if ((int)o->dim[0] != c->hidden) { fprintf(stderr, "convert: embedding com %d colunas, hidden_size=%d\n", (int)o->dim[0], c->hidden); return 1; } }
    snprintf(nm, sizeof nm, "%snorm.weight", c->prefix); cv_simple(c, "output_norm.weight", nm, -1, EW_PLUS1, 1, -1);
    if (cv_opt(c, "lm_head.weight")) cv_simple(c, "output.weight", "lm_head.weight", -1, EW_NONE, 0, -1);
    else printf("  lm_head.weight ausente: pesos amarrados (output.weight omitido; o motor usa token_embd)\n");
    for (int l = 0; l < c->layers; l++) cv_layer(c, l);

    /* conferencia do que sobrou no checkpoint */
    for (size_t i = 0; i < c->S.nst; i++) {
        const CvSt *t = &c->S.st[i];
        if (t->used) continue;
        if (strstr(t->name, "visual.") || strstr(t->name, "vision") || !strncmp(t->name, "mtp.", 4) || strstr(t->name, ".mtp.")) { c->skipped++; continue; }
        if (c->unused_warn++ < 8) fprintf(stderr, "convert: aviso: tensor do checkpoint sem mapeamento, ignorado: %s\n", t->name);
    }
    if (c->unused_warn > 8) fprintf(stderr, "convert: aviso: mais %d tensores sem mapeamento\n", c->unused_warn - 8);
    if (c->skipped) printf("  %d tensores de visao/MTP ignorados (o motor carrega o projetor de visao de um GGUF a parte)\n", c->skipped);

    /* tipos, deslocamentos, KV (primeira passada) */
    const uint64_t align = 32;
    uint64_t off = 0, total_params = 0;
    for (int i = 0; i < c->no; i++) {
        CvOut *o = &c->o[i];
        o->otype = cv_choose(o, mode, c->layers);
        o->nbytes = qbytes(o->otype, (size_t)cv_elems(o));
        if (!o->nbytes) { fprintf(stderr, "convert: tamanho invalido para %s\n", o->name); return 1; }
        off = (off + align - 1) / align * align;
        o->off = off; off += o->nbytes; total_params += cv_elems(o);
    }
    GBuf kv = {0}; uint64_t nkv = 0;
    const char *base = strrchr(dir, '/'); base = base ? base + 1 : dir;
    gkv_str(&kv, "general.architecture", c->arch); nkv++;
    gkv_str(&kv, "general.type", "model"); nkv++;
    gkv_str(&kv, "general.name", base[0] ? base : "model"); nkv++;
    gkv_u32(&kv, "general.quantization_version", 2); nkv++;
    gkv_u32(&kv, "general.file_type", (uint32_t)cv_mode_ft(mode)); nkv++;
    gkv_str(&kv, "general.quantized_by", "katana"); nkv++;
    { JV gc; snprintf(path, sizeof path, "%s/generation_config.json", dir);
      if (cv_json_file(path, &gc)) {
          const JV *k = jget(&gc, "top_k"), *p = jget(&gc, "top_p"), *t = jget(&gc, "temperature");
          if (k && k->t == J_NUM) { gkv_u32(&kv, "general.sampling.top_k", (uint32_t)k->i); nkv++; }
          if (p && p->t == J_NUM) { cv_kv_f32(&kv, "general.sampling.top_p", (float)p->num); nkv++; }
          if (t && t->t == J_NUM) { cv_kv_f32(&kv, "general.sampling.temp", (float)t->num); nkv++; }
      } }
    char key[96];
#define KU(suffix, val) do { snprintf(key, sizeof key, "%s." suffix, c->arch); gkv_u32(&kv, key, (uint32_t)(val)); nkv++; } while (0)
#define KF(suffix, val) do { snprintf(key, sizeof key, "%s." suffix, c->arch); cv_kv_f32(&kv, key, (float)(val)); nkv++; } while (0)
    KU("block_count", c->layers);
    KU("context_length", cv_i(txt, &cfg, "max_position_embeddings", 4096));
    KU("embedding_length", c->hidden);
    if (!c->moe) KU("feed_forward_length", c->ffd);
    KU("attention.head_count", c->heads);
    KU("attention.head_count_kv", c->kvh);
    /* rope: base e fracao rotativa em rope_parameters (formato novo) ou rope_theta/partial_rotary_factor soltos */
    const JV *rp = jget(txt, "rope_parameters"); if (!rp) rp = jget(txt, "rope_scaling"); if (!rp) rp = jget(&cfg, "rope_parameters");
    const double theta = jget(rp, "rope_theta") ? jnum(rp, "rope_theta", 1e7) : jnum(txt, "rope_theta", jnum(&cfg, "rope_theta", 1e7));
    const double prf = jget(rp, "partial_rotary_factor") ? jnum(rp, "partial_rotary_factor", 0.25) : jnum(txt, "partial_rotary_factor", 0.25);
    int32_t sec[4] = { 11, 11, 10, 0 };
    { const JV *ms = jget(rp, "mrope_section"); if (ms && ms->t == J_ARR) { for (int i = 0; i < 4; i++) sec[i] = i < (int)ms->n ? (int32_t)ms->v[i].i : 0; } }
    snprintf(key, sizeof key, "%s.rope.dimension_sections", c->arch); cv_kv_arr_i32(&kv, key, sec, 4); nkv++;
    KF("rope.freq_base", theta);
    KF("attention.layer_norm_rms_epsilon", jget(txt, "rms_norm_eps") ? jnum(txt, "rms_norm_eps", 1e-6) : jnum(&cfg, "rms_norm_eps", 1e-6));
    if (c->moe) { KU("expert_count", c->nexp); KU("expert_used_count", c->nused); }
    KU("attention.key_length", c->hd); KU("attention.value_length", c->hd);
    if (c->moe) { KU("expert_feed_forward_length", c->ffe); if (c->ffs) KU("expert_shared_feed_forward_length", c->ffs); }
    KU("ssm.conv_kernel", c->conv); KU("ssm.state_size", c->dk); KU("ssm.group_count", c->nk); KU("ssm.time_step_rank", c->nv);
    KU("ssm.inner_size", c->nv * c->dv); KU("full_attention_interval", c->interval);
    KU("rope.dimension_count", (int)(c->hd * prf + 0.5));
#undef KU
#undef KF
    cv_tokenizer(dir, c, &cfg, txt, &kv, &nkv);

    /* tabela de tensores */
    GBuf tib = {0};
    for (int i = 0; i < c->no; i++) {
        const CvOut *o = &c->o[i];
        gbuf_gstr(&tib, o->name, strlen(o->name)); gbuf_u32(&tib, (uint32_t)o->nd);
        for (int d = 0; d < o->nd; d++) gbuf_u64(&tib, o->dim[d]);
        gbuf_u32(&tib, o->otype); gbuf_u64(&tib, o->off);
    }
    printf("  %d tensores, %.2fG parametros, %.2f GB no GGUF (%.2f bits/peso)\n", c->no, total_params / 1e9, (off + kv.n + tib.n) / 1073741824.0, off * 8.0 / (double)total_params);

    /* segunda passada: grava em um arquivo temporario e renomeia no fim */
    FILE *ex = fopen(out_path, "rb");
    if (ex) { fclose(ex); fprintf(stderr, "convert: %s ja existe; escolha outro nome.\n", out_path); return 1; }
    char tmpp[2100]; snprintf(tmpp, sizeof tmpp, "%s.part", out_path);
    FILE *f = fopen(tmpp, "wb");
    if (!f) { fprintf(stderr, "convert: nao consegui gravar %s\n", tmpp); return 1; }
    uint8_t hdr[24]; const uint32_t magic = 0x46554747u, v3 = 3; const uint64_t nt = (uint64_t)c->no;
    memcpy(hdr, &magic, 4); memcpy(hdr + 4, &v3, 4); memcpy(hdr + 8, &nt, 8); memcpy(hdr + 16, &nkv, 8);
    fwrite(hdr, 1, 24, f); fwrite(kv.p, 1, kv.n, f); fwrite(tib.p, 1, tib.n, f);
    const uint64_t hpos = 24 + kv.n + tib.n, dstart = (hpos + align - 1) / align * align;
    for (uint64_t p = hpos; p < dstart; p++) fputc(0, f);
    uint64_t cur = 0;
    const uint64_t SLAB = 16u << 20;               /* elementos por fatia */
    const double t0 = (double)clock() / CLOCKS_PER_SEC;
    for (int i = 0; i < c->no; i++) {
        CvOut *o = &c->o[i];
        while (cur < o->off) { fputc(0, f); cur++; }
        const uint64_t cols = o->dim[0], R = cv_rows(o);
        const size_t rowbytes = qbytes(o->otype, (size_t)cols);
        uint64_t per = SLAB / cols; if (per < 1) per = 1; if (per > R) per = R;
        uint8_t *buf = xalloc((size_t)(per * rowbytes), 1);
        for (uint64_t r0 = 0; r0 < R; r0 += per) {
            const uint64_t n = r0 + per > R ? R - r0 : per;
            CvJob J = { o, r0, n, buf, rowbytes, o->otype };
            int chunk = (int)(65536 / cols); if (chunk < 1) chunk = 1;
            pool_for(cv_job, &J, (int)n, chunk);
            if (fwrite(buf, 1, (size_t)(n * rowbytes), f) != (size_t)(n * rowbytes)) { fprintf(stderr, "\nconvert: erro de escrita (disco cheio?)\n"); fclose(f); remove(tmpp); return 1; }
        }
        free(buf);
        cur = o->off + o->nbytes;
#ifndef _WIN32
        for (int s = 0; s < o->nsrc; s++) {                    /* devolve as paginas lidas: o pico de memoria nao cresce com o modelo */
            const uintptr_t pg = (uintptr_t)sysconf(_SC_PAGESIZE);
            const CvSt *t = &c->S.st[o->src[s].st];
            const uintptr_t a = (uintptr_t)o->src[s].p, b = a + (t->e - t->b);
            const uintptr_t a0 = (a + pg - 1) & ~(pg - 1), b0 = b & ~(pg - 1);
            if (b0 > a0) madvise((void *)a0, b0 - a0, MADV_DONTNEED);
        }
#endif
        if ((i & 15) == 0 || i == c->no - 1) { printf("\r  gravando: %d/%d tensores (%.0f%%)", i + 1, c->no, 100.0 * (double)cur / (double)off); fflush(stdout); }
    }
    printf("\n");
    if (fclose(f) != 0) { fprintf(stderr, "convert: erro ao fechar %s\n", tmpp); remove(tmpp); return 1; }
    if (rename(tmpp, out_path) != 0) { fprintf(stderr, "convert: nao consegui renomear %s\n", tmpp); return 1; }
    printf("Pronto: %s (%d tensores, %.1f s de CPU). Confira com: katana info %s\n", out_path, c->no, (double)clock() / CLOCKS_PER_SEC - t0, out_path);
    return 0;
}

static int convert_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "uso: katana convert <pasta-hf> <saida.gguf> [tipo]   |   katana convert <pasta-hf> --list\n"
                        "  tipo: q4_k_m (padrao, mix dinamico), q4_k, q5_k_m, q6_k, q8_0, f16, bf16, f32\n"
                        "  a pasta tem *.safetensors (+ model.safetensors.index.json), config.json, tokenizer.json, tokenizer_config.json\n");
        return 1;
    }
    if (!strcmp(argv[1], "--list")) return cv_list(argv[0]);
    int mode = CM_Q4M;
    if (argc >= 3) { mode = cv_mode_parse(argv[2]); if (mode < 0) { fprintf(stderr, "convert: tipo '%s' nao suportado (q4_k_m, q4_k, q5_k_m, q6_k, q8_0, f16, bf16, f32)\n", argv[2]); return 1; } }
    return cv_run(argv[0], argv[1], mode);
}
