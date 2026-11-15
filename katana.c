/*
 * katana.c - motor de inferencia em C puro para modelos abertos no formato GGUF.
 * Sem dependencias externas (so libc; AVX2 e OpenMP sao opcionais).
 *
 * Suporta: arquiteturas "llama" e "qwen2" (RMSNorm, RoPE, GQA, SwiGLU),
 *          tensores F32/F16/Q8_0/Q4_K/Q6_K (os quantizados sao usados direto do
 *          arquivo mapeado em memoria, sem conversao), tokenizador BPE lido do GGUF.
 *
 * Uso:
 *   katana info <modelo.gguf>
 *   katana selftest <modelo.gguf>                                    confere a matematica quantizada
 *   katana bench <modelo.gguf> [tokens_prompt] [tokens_geracao]      mede a velocidade (ja aquecido)
 *   katana run  <modelo.gguf> "texto" [max_tokens] [temperatura]     completa o texto
 *   katana chat <modelo.gguf> "pergunta" [max_tokens] [temperatura]  formato ChatML
 *
 * As multiplicacoes de matriz rodam na GPU (OpenCL) quando ha uma; KATANA_CPU=1 forca CPU.
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#ifdef __AVX2__
#include <immintrin.h>
#endif
#ifdef _OPENMP
#include <omp.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#endif

#define MAX_CTX 4096
#define TOP_K 40

static void die(const char *msg) { fprintf(stderr, "Erro: %s\n", msg); exit(1); }

static void *xalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz);
    if (!p) die("sem memoria");
    return p;
}

/* ================= mapa string -> int (hash aberto, FNV-1a) ================= */

typedef struct { const char **k; int *kl; int *v; uint32_t cap; } Map;

static uint32_t fnv(const char *s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}
static void map_init(Map *m, uint32_t n) {
    uint32_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    m->cap = cap;
    m->k = xalloc(cap, sizeof(char *));
    m->kl = xalloc(cap, sizeof(int));
    m->v = xalloc(cap, sizeof(int));
}
static void map_put(Map *m, const char *s, int n, int v) {
    uint32_t i = fnv(s, n) & (m->cap - 1);
    while (m->k[i]) {
        if (m->kl[i] == n && memcmp(m->k[i], s, n) == 0) return; /* mantem o primeiro */
        i = (i + 1) & (m->cap - 1);
    }
    m->k[i] = s; m->kl[i] = n; m->v[i] = v;
}
static int map_get(const Map *m, const char *s, int n) {
    uint32_t i = fnv(s, n) & (m->cap - 1);
    while (m->k[i]) {
        if (m->kl[i] == n && memcmp(m->k[i], s, n) == 0) return m->v[i];
        i = (i + 1) & (m->cap - 1);
    }
    return -1;
}

/* ================= estruturas ================= */

typedef struct {
    int dim, n_layers, n_heads, n_kv, hd, hidden, ctx, vocab;
    float rope_base, eps;
    int bos, eos, add_bos;
    int rope_neox;      /* RoPE em metades (qwen2) em vez de pares adjacentes (llama) */
    int chat_fmt;       /* 0 = ChatML, 1 = Llama 3 */
    char arch[64];
} Config;

/* matriz de pesos: float32 (f) ou quantizada (q aponta direto para o arquivo mapeado) */
typedef struct {
    float *f; const uint8_t *q; uint32_t type; size_t stride;
    void *gbuf;         /* buffer na VRAM (pode ser compartilhado com outra matriz) */
    int gs, go, pair;   /* matrizes intercaladas no buffer: quantas, qual; par gate/up fundido */
} Mat;

typedef struct {
    float *attn_norm, *ffn_norm;
    float *bq, *bk, *bv, *bo;           /* bias da atencao (opcional; qwen2 usa) */
    Mat wq, wk, wv, wo, w_gate, w_up, w_down;
} Layer;

typedef struct {
    char **tok; int *toklen; int *toktype; int n;
    Map vocab, merges;
    int *special; int n_special;
    int byte2cp[256]; int cp2byte[512];
} Tokenizer;

typedef struct {
    Config cfg;
    Tokenizer tk;
    Mat tok_emb, lm_head;
    float *out_norm;
    Layer *layers;
    float *kcache, *vcache;             /* [L][ctx][kv_dim] */
    float *x, *xb, *xb2, *q, *k, *v, *att, *hb, *hb2, *logits, *rope_freq;
    size_t n_params;
} Model;

/* ================= leitura do GGUF ================= */

typedef struct { const uint8_t *p, *end; } Rd;

static void rd_need(Rd *r, size_t n) { if ((size_t)(r->end - r->p) < n) die("GGUF truncado"); }
static uint32_t rd_u32(Rd *r) { uint32_t v; rd_need(r, 4); memcpy(&v, r->p, 4); r->p += 4; return v; }
static uint64_t rd_u64(Rd *r) { uint64_t v; rd_need(r, 8); memcpy(&v, r->p, 8); r->p += 8; return v; }
static const char *rd_str(Rd *r, int *len) {
    uint64_t n = rd_u64(r);
    rd_need(r, (size_t)n);
    const char *s = (const char *)r->p;
    r->p += n; *len = (int)n;
    return s;
}

static const int GG_SIZE[13] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};

static double rd_num(Rd *r, uint32_t t) {
    if (t > 12 || GG_SIZE[t] == 0) die("tipo numerico inesperado no GGUF");
    rd_need(r, GG_SIZE[t]);
    const uint8_t *p = r->p; r->p += GG_SIZE[t];
    switch (t) {
        case 0: case 7: return *p;
        case 1: return *(const int8_t *)p;
        case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
        case 3: { int16_t v; memcpy(&v, p, 2); return v; }
        case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
        case 5: { int32_t v; memcpy(&v, p, 4); return v; }
        case 6: { float v; memcpy(&v, p, 4); return v; }
        case 10: { uint64_t v; memcpy(&v, p, 8); return (double)v; }
        case 11: { int64_t v; memcpy(&v, p, 8); return (double)v; }
        default: { double v; memcpy(&v, p, 8); return v; }
    }
}

static void rd_skip(Rd *r, uint32_t t) {
    int len;
    if (t == 8) { rd_str(r, &len); return; }
    if (t == 9) {
        uint32_t et = rd_u32(r); uint64_t n = rd_u64(r);
        if (et != 8 && et != 9) {
            if (et > 12) die("tipo de array invalido");
            rd_need(r, (size_t)n * GG_SIZE[et]); r->p += n * GG_SIZE[et];
        } else for (uint64_t i = 0; i < n; i++) rd_skip(r, et);
        return;
    }
    rd_num(r, t);
}

static int key_ends(const char *k, int kl, const char *suf) {
    int sl = (int)strlen(suf);
    return kl >= sl && memcmp(k + kl - sl, suf, sl) == 0;
}
static int key_is(const char *k, int kl, const char *s) {
    return kl == (int)strlen(s) && memcmp(k, s, kl) == 0;
}

typedef struct {
    const char *name; int namelen;
    uint64_t dims[4]; uint32_t ndims, type; uint64_t offset;
} TInfo;

typedef struct { TInfo *t; uint64_t n; const uint8_t *data, *end; } Tensors;

static float f16_to_f32(uint16_t h) {
    uint32_t s = (h >> 15) & 1, m = h & 0x3ff, u;
    int e = (h >> 10) & 0x1f;
    if (e == 0) {
        if (m == 0) u = s << 31;
        else {
            e = 1;
            while (!(m & 0x400)) { m <<= 1; e--; }
            m &= 0x3ff;
            u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13);
        }
    } else if (e == 31) u = (s << 31) | 0x7f800000u | (m << 13);
    else u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &u, 4);
    return f;
}

/* tipos de tensor do GGUF */
enum { T_F32 = 0, T_F16 = 1, T_Q8_0 = 8, T_Q4_K = 12, T_Q6_K = 14 };

/* bytes ocupados por n elementos (0 = tipo nao suportado ou n desalinhado) */
static size_t qbytes(uint32_t type, size_t n) {
    switch (type) {
        case T_F32:  return n * 4;
        case T_F16:  return n * 2;
        case T_Q8_0: return n % 32 ? 0 : n / 32 * 34;     /* f16 d + 32 x int8 */
        case T_Q4_K: return n % 256 ? 0 : n / 256 * 144;  /* f16 d, f16 dmin, 12 escalas, 128 nibbles */
        case T_Q6_K: return n % 256 ? 0 : n / 256 * 210;  /* 128 ql, 64 qh, 16 escalas, f16 d */
        default:     return 0;
    }
}

/* escala e minimo (6 bits cada) do sub-bloco j de um bloco Q4_K */
static void k4_scale_min(int j, const uint8_t *q, int *d, int *m) {
    if (j < 4) { *d = q[j] & 63; *m = q[j + 4] & 63; }
    else {
        *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j] >> 6) << 4);
    }
}

static void dequant(uint32_t type, const uint8_t *src, float *out, size_t n) {
    if (type == T_F32) memcpy(out, src, n * 4);
    else if (type == T_F16) {
        for (size_t j = 0; j < n; j++) { uint16_t h; memcpy(&h, src + j * 2, 2); out[j] = f16_to_f32(h); }
    } else if (type == T_Q8_0) {
        for (size_t b = 0; b < n / 32; b++, src += 34, out += 32) {
            uint16_t h; memcpy(&h, src, 2);
            const float d = f16_to_f32(h);
            const int8_t *qs = (const int8_t *)(src + 2);
            for (int j = 0; j < 32; j++) out[j] = d * qs[j];
        }
    } else if (type == T_Q4_K) {
        for (size_t b = 0; b < n / 256; b++, src += 144) {
            uint16_t h0, h1; memcpy(&h0, src, 2); memcpy(&h1, src + 2, 2);
            const float d = f16_to_f32(h0), dmin = f16_to_f32(h1);
            const uint8_t *sc = src + 4, *q = src + 16;
            for (int j = 0; j < 8; j += 2, q += 32) {
                int s1, m1, s2, m2;
                k4_scale_min(j, sc, &s1, &m1); k4_scale_min(j + 1, sc, &s2, &m2);
                for (int l = 0; l < 32; l++) *out++ = d * s1 * (q[l] & 0xF) - dmin * m1;
                for (int l = 0; l < 32; l++) *out++ = d * s2 * (q[l] >> 4) - dmin * m2;
            }
        }
    } else if (type == T_Q6_K) {
        for (size_t b = 0; b < n / 256; b++, src += 210) {
            const uint8_t *ql = src, *qh = src + 128;
            const int8_t *sc = (const int8_t *)(src + 192);
            uint16_t h; memcpy(&h, src + 208, 2);
            const float d = f16_to_f32(h);
            for (int c = 0; c < 2; c++, out += 128, ql += 64, qh += 32, sc += 8) {
                for (int l = 0; l < 32; l++) {
                    const int is = l / 16;
                    const int q1 = (int)((ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                    const int q2 = (int)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                    const int q3 = (int)((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                    const int q4 = (int)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                    out[l]      = d * sc[is] * q1;
                    out[l + 32] = d * sc[is + 2] * q2;
                    out[l + 64] = d * sc[is + 4] * q3;
                    out[l + 96] = d * sc[is + 6] * q4;
                }
            }
        }
    }
}

/* devolve o tensor convertido para float32; NULL se nao existir e !required */
static float *get_tensor(const Tensors *ts, const char *name, size_t expect, int required, size_t *count) {
    int nl = (int)strlen(name);
    for (uint64_t i = 0; i < ts->n; i++) {
        const TInfo *t = &ts->t[i];
        if (t->namelen != nl || memcmp(t->name, name, nl) != 0) continue;
        size_t n = 1;
        for (uint32_t d = 0; d < t->ndims; d++) n *= (size_t)t->dims[d];
        if (n != expect) { fprintf(stderr, "Tensor %s: %zu elementos, esperava %zu\n", name, n, expect); exit(1); }
        const uint8_t *src = ts->data + t->offset;
        float *out = xalloc(n, sizeof(float));
        size_t bytes = qbytes(t->type, n);
        if (!bytes) {
            fprintf(stderr, "Tensor %s: quantizacao tipo %u nao suportada (use F32, F16, Q8_0, Q4_K ou Q6_K)\n", name, t->type);
            exit(1);
        }
        if (src + bytes > ts->end) die("tensor fora do arquivo");
        dequant(t->type, src, out, n);
        if (count) *count += n;
        return out;
    }
    if (required) { fprintf(stderr, "Tensor %s nao encontrado\n", name); exit(1); }
    return NULL;
}

/* matriz [rows][cols]: usa os dados quantizados direto do arquivo quando cada linha
   comeca num bloco inteiro; senao converte para float32 */
static Mat get_mat(const Tensors *ts, const char *name, size_t rows, size_t cols, int required, size_t *count) {
    Mat m = {0};
    int nl = (int)strlen(name);
    for (uint64_t i = 0; i < ts->n; i++) {
        const TInfo *t = &ts->t[i];
        if (t->namelen != nl || memcmp(t->name, name, nl) != 0) continue;
        if (t->type != T_Q8_0 && t->type != T_Q4_K && t->type != T_Q6_K) break;
        size_t n = 1, stride = qbytes(t->type, cols);
        if (!stride) break;
        for (uint32_t d = 0; d < t->ndims; d++) n *= (size_t)t->dims[d];
        if (n != rows * cols) { fprintf(stderr, "Tensor %s: tamanho inesperado\n", name); exit(1); }
        if (ts->data + t->offset + rows * stride > ts->end) die("tensor fora do arquivo");
        m.q = ts->data + t->offset;
        m.type = t->type; m.stride = stride;
        if (count) *count += n;
        return m;
    }
    m.f = get_tensor(ts, name, rows * cols, required, count);
    return m;
}

static void tokenizer_finish(Tokenizer *tk, const char **merges, int *mlen, int n_merges) {
    int n = 0;
    for (int b = 0; b < 256; b++) {
        if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || b >= 174) tk->byte2cp[b] = b;
        else tk->byte2cp[b] = 256 + n++;
    }
    for (int i = 0; i < 512; i++) tk->cp2byte[i] = -1;
    for (int b = 0; b < 256; b++) tk->cp2byte[tk->byte2cp[b]] = b;

    map_init(&tk->vocab, tk->n);
    tk->special = xalloc(tk->n, sizeof(int));
    for (int i = 0; i < tk->n; i++) {
        map_put(&tk->vocab, tk->tok[i], tk->toklen[i], i);
        if (tk->toktype && (tk->toktype[i] == 3 || tk->toktype[i] == 4) && tk->toklen[i] > 0)
            tk->special[tk->n_special++] = i;
    }
    map_init(&tk->merges, n_merges);
    for (int i = 0; i < n_merges; i++) map_put(&tk->merges, merges[i], mlen[i], i);
}

static void gpu_setup(Model *m);

/* mapeia o arquivo em memoria (somente leitura): o SO carrega as paginas sob demanda
   e o mapeamento fica vivo ate o processo terminar */
static const uint8_t *map_file(const char *path, int64_t *size) {
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *w = xalloc(wn, sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, wn);
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart == 0) return NULL;
    HANDLE mp = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mp) return NULL;
    *size = sz.QuadPart;
    return MapViewOfFile(mp, FILE_MAP_READ, 0, 0, 0);
#else
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size == 0) return NULL;
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    *size = st.st_size;
    return p == MAP_FAILED ? NULL : p;
#endif
}

static void load_model(Model *m, const char *path) {
    int64_t fsz = 0;
    const uint8_t *buf = map_file(path, &fsz);
    if (!buf) { fprintf(stderr, "Nao consegui abrir %s\n", path); exit(1); }

    Rd r = { buf, buf + fsz };
    if (rd_u32(&r) != 0x46554747u) die("nao e um arquivo GGUF");
    uint32_t ver = rd_u32(&r);
    if (ver < 2) die("versao de GGUF nao suportada");
    uint64_t n_tensors = rd_u64(&r), n_kv = rd_u64(&r);

    Config *c = &m->cfg;
    c->rope_base = 10000.0f; c->eps = 1e-5f; c->bos = -1; c->eos = -1;
    uint64_t align = 32;
    const char **merges = NULL; int *mlen = NULL; int n_merges = 0;
    Tokenizer *tk = &m->tk;

    for (uint64_t i = 0; i < n_kv; i++) {
        int kl; const char *k = rd_str(&r, &kl);
        uint32_t t = rd_u32(&r);
        if (t == 8 && key_is(k, kl, "general.architecture")) {
            int sl; const char *s = rd_str(&r, &sl);
            if (sl > 63) sl = 63;
            memcpy(c->arch, s, sl);
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.tokens")) {
            uint32_t et = rd_u32(&r); uint64_t n = rd_u64(&r);
            if (et != 8) die("tokens: tipo inesperado");
            tk->n = (int)n;
            tk->tok = xalloc(n, sizeof(char *)); tk->toklen = xalloc(n, sizeof(int));
            for (uint64_t j = 0; j < n; j++) {
                int sl; const char *s = rd_str(&r, &sl);
                tk->tok[j] = xalloc(sl + 1, 1); memcpy(tk->tok[j], s, sl); tk->toklen[j] = sl;
            }
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.merges")) {
            uint32_t et = rd_u32(&r); uint64_t n = rd_u64(&r);
            if (et != 8) die("merges: tipo inesperado");
            n_merges = (int)n;
            merges = xalloc(n, sizeof(char *)); mlen = xalloc(n, sizeof(int));
            for (uint64_t j = 0; j < n; j++) {
                int sl; const char *s = rd_str(&r, &sl);
                char *cp = xalloc(sl + 1, 1); memcpy(cp, s, sl);
                merges[j] = cp; mlen[j] = sl;
            }
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.token_type")) {
            uint32_t et = rd_u32(&r); uint64_t n = rd_u64(&r);
            tk->toktype = xalloc(n, sizeof(int));
            for (uint64_t j = 0; j < n; j++) tk->toktype[j] = (int)rd_num(&r, et);
        } else if (t != 8 && t != 9) {
            double v = rd_num(&r, t);
            if (key_ends(k, kl, ".block_count")) c->n_layers = (int)v;
            else if (key_ends(k, kl, ".context_length")) c->ctx = (int)v;
            else if (key_ends(k, kl, ".embedding_length")) c->dim = (int)v;
            else if (key_ends(k, kl, ".feed_forward_length")) c->hidden = (int)v;
            else if (key_ends(k, kl, ".attention.head_count")) c->n_heads = (int)v;
            else if (key_ends(k, kl, ".attention.head_count_kv")) c->n_kv = (int)v;
            else if (key_ends(k, kl, ".rope.freq_base")) c->rope_base = (float)v;
            else if (key_ends(k, kl, ".layer_norm_rms_epsilon")) c->eps = (float)v;
            else if (key_is(k, kl, "tokenizer.ggml.bos_token_id")) c->bos = (int)v;
            else if (key_is(k, kl, "tokenizer.ggml.eos_token_id")) c->eos = (int)v;
            else if (key_is(k, kl, "tokenizer.ggml.add_bos_token")) c->add_bos = (int)v;
            else if (key_is(k, kl, "general.alignment")) align = (uint64_t)v;
        } else rd_skip(&r, t);
    }

    if (strcmp(c->arch, "qwen2") == 0) c->rope_neox = 1;
    else if (strcmp(c->arch, "llama") != 0)
        fprintf(stderr, "Aviso: arquitetura '%s' nao testada (so 'llama' e 'qwen2' por enquanto)\n", c->arch);
    if (!c->dim || !c->n_layers || !c->n_heads || !c->hidden || !tk->n) die("metadados incompletos no GGUF");
    if (!c->n_kv) c->n_kv = c->n_heads;
    c->vocab = tk->n;
    c->hd = c->dim / c->n_heads;
    if (c->dim % c->n_heads || c->hd % 2 || c->n_heads % c->n_kv) die("dimensoes de atencao invalidas");
    if (!c->ctx || c->ctx > MAX_CTX) c->ctx = MAX_CTX;
    if (!merges) die("GGUF sem merges BPE (tokenizador nao suportado)");
    tokenizer_finish(tk, merges, mlen, n_merges);
    if (map_get(&tk->vocab, "<|im_start|>", 12) < 0 && map_get(&tk->vocab, "<|start_header_id|>", 19) >= 0)
        c->chat_fmt = 1;

    Tensors ts;
    ts.n = n_tensors; ts.t = xalloc(n_tensors, sizeof(TInfo));
    for (uint64_t i = 0; i < n_tensors; i++) {
        TInfo *t = &ts.t[i];
        t->name = rd_str(&r, &t->namelen);
        t->ndims = rd_u32(&r);
        if (t->ndims > 4) die("tensor com dimensoes demais");
        for (uint32_t d = 0; d < t->ndims; d++) t->dims[d] = rd_u64(&r);
        t->type = rd_u32(&r);
        t->offset = rd_u64(&r);
    }
    uint64_t pos = (uint64_t)(r.p - buf);
    pos = (pos + align - 1) / align * align;
    ts.data = buf + pos; ts.end = buf + fsz;

    size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab;
    size_t *np = &m->n_params;
    m->tok_emb = get_mat(&ts, "token_embd.weight", V, d, 1, np);
    m->out_norm = get_tensor(&ts, "output_norm.weight", d, 1, np);
    m->lm_head = get_mat(&ts, "output.weight", V, d, 0, np);
    if (!m->lm_head.f && !m->lm_head.q) m->lm_head = m->tok_emb;   /* pesos amarrados */
    m->layers = xalloc(c->n_layers, sizeof(Layer));
    for (int l = 0; l < c->n_layers; l++) {
        Layer *L = &m->layers[l];
        char nm[96];
#define V1(field, suffix) snprintf(nm, sizeof nm, "blk.%d." suffix, l); L->field = get_tensor(&ts, nm, d, 1, np)
#define M2(field, suffix, rows, cols) snprintf(nm, sizeof nm, "blk.%d." suffix, l); L->field = get_mat(&ts, nm, rows, cols, 1, np)
        V1(attn_norm, "attn_norm.weight");
        M2(wq, "attn_q.weight", d, d);
        M2(wk, "attn_k.weight", kvd, d);
        M2(wv, "attn_v.weight", kvd, d);
        M2(wo, "attn_output.weight", d, d);
        V1(ffn_norm, "ffn_norm.weight");
        M2(w_gate, "ffn_gate.weight", H, d);
        M2(w_up, "ffn_up.weight", H, d);
        M2(w_down, "ffn_down.weight", d, H);
        snprintf(nm, sizeof nm, "blk.%d.attn_q.bias", l);      L->bq = get_tensor(&ts, nm, d, 0, np);
        snprintf(nm, sizeof nm, "blk.%d.attn_k.bias", l);      L->bk = get_tensor(&ts, nm, kvd, 0, np);
        snprintf(nm, sizeof nm, "blk.%d.attn_v.bias", l);      L->bv = get_tensor(&ts, nm, kvd, 0, np);
        snprintf(nm, sizeof nm, "blk.%d.attn_output.bias", l); L->bo = get_tensor(&ts, nm, d, 0, np);
#undef V1
#undef M2
    }
    free(ts.t);   /* o mapeamento do arquivo continua vivo: os pesos apontam para ele */

#ifdef _OPENMP
    /* padrao medido: nucleos fisicos para modelos grandes, metade disso para os pequenos
       (o custo de acordar threads supera o ganho). OMP_NUM_THREADS sobrepoe. */
    if (!getenv("OMP_NUM_THREADS") && omp_get_num_procs() >= 4)
        omp_set_num_threads(omp_get_num_procs() / (m->n_params < 500000000 ? 4 : 2));
#endif

    size_t ctx = c->ctx, L = c->n_layers;
    m->kcache = xalloc(L * ctx * kvd, sizeof(float));
    m->vcache = xalloc(L * ctx * kvd, sizeof(float));
    m->x = xalloc(d, sizeof(float));   m->xb = xalloc(d, sizeof(float));
    m->xb2 = xalloc(d, sizeof(float)); m->q = xalloc(d, sizeof(float));
    m->k = xalloc(kvd, sizeof(float)); m->v = xalloc(kvd, sizeof(float));
    m->att = xalloc(ctx, sizeof(float));
    m->hb = xalloc(H, sizeof(float));  m->hb2 = xalloc(H, sizeof(float));
    m->logits = xalloc(V, sizeof(float));
    m->rope_freq = xalloc(c->hd / 2, sizeof(float));
    for (int i = 0; i < c->hd / 2; i++)
        m->rope_freq[i] = powf(c->rope_base, -2.0f * i / c->hd);

    /* por ultimo: a GPU precisa de tudo acima ja preenchido (ex.: rope_freq) */
    if (!getenv("KATANA_CPU")) gpu_setup(m);
}

/* ================= tokenizador BPE (estilo GPT-2) ================= */

typedef struct { int *ids; int n, cap; } Ids;

static void ids_push(Ids *o, int id) {
    if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 256; o->ids = realloc(o->ids, o->cap * sizeof(int)); if (!o->ids) die("sem memoria"); }
    o->ids[o->n++] = id;
}

/* aplica BPE em uma "palavra" (bytes crus) e empilha os ids */
static void bpe_word(const Tokenizer *tk, const uint8_t *s, int n, Ids *out) {
    char *buf = xalloc((size_t)n * 2 + 1, 1);
    int *st = xalloc(n, sizeof(int)), *ln = xalloc(n, sizeof(int));
    char *pair = xalloc((size_t)n * 2 + 2, 1);
    int bl = 0, ns = 0;

    for (int i = 0; i < n; i++) {   /* um simbolo por caractere unicode (bytes mapeados) */
        int start = bl, cp = tk->byte2cp[s[i]];
        if (cp < 0x80) buf[bl++] = (char)cp;
        else { buf[bl++] = (char)(0xC0 | (cp >> 6)); buf[bl++] = (char)(0x80 | (cp & 0x3F)); }
        /* bytes de continuacao UTF-8 viram simbolos proprios (BPE em nivel de byte) */
        st[ns] = start; ln[ns] = bl - start; ns++;
    }
    for (;;) {
        int best = -1, best_rank = 0x7fffffff;
        for (int i = 0; i + 1 < ns; i++) {
            int a = ln[i], b = ln[i + 1];
            memcpy(pair, buf + st[i], a); pair[a] = ' '; memcpy(pair + a + 1, buf + st[i + 1], b);
            int rk = map_get(&tk->merges, pair, a + 1 + b);
            if (rk >= 0 && rk < best_rank) { best_rank = rk; best = i; }
        }
        if (best < 0) break;
        ln[best] += ln[best + 1];
        for (int i = best + 1; i + 1 < ns; i++) { st[i] = st[i + 1]; ln[i] = ln[i + 1]; }
        ns--;
    }
    for (int i = 0; i < ns; i++) {
        int id = map_get(&tk->vocab, buf + st[i], ln[i]);
        ids_push(out, id >= 0 ? id : 0);
    }
    free(buf); free(st); free(ln); free(pair);
}

static int c_space(int c)  { return c == ' ' || (c >= 9 && c <= 13); }
static int c_digit(int c)  { return c >= '0' && c <= '9'; }
static int c_letter(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80; }
static int c_other(int c)  { return !c_space(c) && !c_digit(c) && !c_letter(c); }

/* pre-tokenizacao aproximada do regex do GPT-2 (digitos isolados, como no SmolLM) */
static void encode_plain(const Tokenizer *tk, const uint8_t *s, int n, Ids *out) {
    int i = 0;
    while (i < n) {
        int c = s[i], j, k;
        if (c_digit(c)) { bpe_word(tk, s + i, 1, out); i++; continue; }
        if (c == '\'' && i + 1 < n) {   /* contracoes do ingles */
            int len = 0, a = s[i + 1], b = i + 2 < n ? s[i + 2] : 0;
            if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') || (a == 'l' && b == 'l')) len = 3;
            else if (a == 's' || a == 't' || a == 'm' || a == 'd') len = 2;
            if (len) { bpe_word(tk, s + i, len, out); i += len; continue; }
        }
        j = (c == ' ' && i + 1 < n && (c_letter(s[i + 1]) || c_other(s[i + 1]))) ? i + 1 : i;
        if (c_letter(s[j])) {
            for (k = j; k < n && c_letter(s[k]); k++);
            bpe_word(tk, s + i, k - i, out); i = k; continue;
        }
        if (c_other(s[j])) {
            for (k = j; k < n && c_other(s[k]); k++);
            bpe_word(tk, s + i, k - i, out); i = k; continue;
        }
        for (k = i; k < n && c_space(s[k]); k++);
        if (k < n && k - i > 1) k--;    /* o ultimo espaco fica para a proxima palavra */
        bpe_word(tk, s + i, k - i, out); i = k;
    }
}

/* codifica texto, reconhecendo tokens especiais (ex.: <|im_start|>) */
static void encode(const Tokenizer *tk, const char *text, Ids *out) {
    const uint8_t *s = (const uint8_t *)text;
    int n = (int)strlen(text), start = 0, i = 0;
    while (i < n) {
        int hit = -1;
        if (s[i] == '<') {
            for (int j = 0; j < tk->n_special; j++) {
                int id = tk->special[j], l = tk->toklen[id];
                if (l <= n - i && memcmp(s + i, tk->tok[id], l) == 0 && (hit < 0 || l > tk->toklen[hit])) hit = id;
            }
        }
        if (hit >= 0) {
            if (i > start) encode_plain(tk, s + start, i - start, out);
            ids_push(out, hit);
            i += tk->toklen[hit]; start = i;
        } else i++;
    }
    if (n > start) encode_plain(tk, s + start, n - start, out);
}

/* escreve os bytes originais de um token */
static void print_token(const Tokenizer *tk, int id, FILE *f) {
    const uint8_t *s = (const uint8_t *)tk->tok[id];
    int n = tk->toklen[id], i = 0;
    while (i < n) {
        int cp = s[i], adv = 1;
        if (cp >= 0xC0 && cp < 0xE0 && i + 1 < n) { cp = ((cp & 0x1F) << 6) | (s[i + 1] & 0x3F); adv = 2; }
        if (adv <= 2 && cp < 512 && tk->cp2byte[cp] >= 0) fputc(tk->cp2byte[cp], f);
        else fwrite(s + i, 1, adv, f);
        i += adv;
    }
}

/* ================= matematica ================= */

static float dot(const float *a, const float *b, int n) {
    int j = 0;
    float s = 0.0f;
#ifdef __AVX2__
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    for (; j + 16 <= n; j += 16) {
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + j), _mm256_loadu_ps(b + j), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + j + 8), _mm256_loadu_ps(b + j + 8), acc1);
    }
    acc0 = _mm256_add_ps(acc0, acc1);
    __m128 lo = _mm_add_ps(_mm256_castps256_ps128(acc0), _mm256_extractf128_ps(acc0, 1));
    lo = _mm_hadd_ps(lo, lo); lo = _mm_hadd_ps(lo, lo);
    s = _mm_cvtss_f32(lo);
#endif
    for (; j < n; j++) s += a[j] * b[j];
    return s;
}

/* produto de uma linha Q8_0 (blocos: f16 escala + 32 x int8) por um vetor float */
static float dot_q8(const uint8_t *row, const float *x, int cols) {
    const int nb = cols / 32;
#ifdef __AVX2__
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < nb; b++, row += 34, x += 32) {
        uint16_t h; memcpy(&h, row, 2);
        const __m256 d = _mm256_set1_ps(_mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))));
        const uint8_t *qs = row + 2;
#define Q8(o) _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_loadl_epi64((const __m128i *)(qs + (o)))))
        __m256 s = _mm256_mul_ps(Q8(0), _mm256_loadu_ps(x));
        s = _mm256_fmadd_ps(Q8(8), _mm256_loadu_ps(x + 8), s);
        s = _mm256_fmadd_ps(Q8(16), _mm256_loadu_ps(x + 16), s);
        s = _mm256_fmadd_ps(Q8(24), _mm256_loadu_ps(x + 24), s);
#undef Q8
        acc = _mm256_fmadd_ps(s, d, acc);
    }
    __m128 lo = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
    lo = _mm_hadd_ps(lo, lo); lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
#else
    float total = 0.0f;
    for (int b = 0; b < nb; b++, row += 34, x += 32) {
        uint16_t h; memcpy(&h, row, 2);
        const int8_t *qs = (const int8_t *)(row + 2);
        float s = 0.0f;
        for (int j = 0; j < 32; j++) s += qs[j] * x[j];
        total += s * f16_to_f32(h);
    }
    return total;
#endif
}

#ifdef __AVX2__
static float hsum256(__m256 v) {
    __m128 lo = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    lo = _mm_hadd_ps(lo, lo); lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
}
/* soma de 16 bytes (sem sinal / com sinal) vezes 16 floats, em 8 acumuladores */
static __m256 mul16_u8(__m128i v, const float *x) {
    __m256 s = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(v)), _mm256_loadu_ps(x));
    return _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_srli_si128(v, 8))), _mm256_loadu_ps(x + 8), s);
}
static __m256 mul16_i8(__m128i v, const float *x) {
    __m256 s = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(v)), _mm256_loadu_ps(x));
    return _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_srli_si128(v, 8))), _mm256_loadu_ps(x + 8), s);
}
#endif

/* linha Q4_K vezes vetor float. Cada peso vale d*sc*q - dmin*m, entao por sub-bloco de 32:
   soma = d*sc * SUM(q_i*x_i) - dmin*m * SUM(x_i); sx traz SUM(x_i) ja calculado por sub-bloco */
static float dot_q4k(const uint8_t *row, const float *x, const float *sx, int cols) {
#ifdef __AVX2__
    const __m256i m4 = _mm256_set1_epi8(0x0F);
    __m256 acc = _mm256_setzero_ps();
    float corr = 0.0f;
    for (int b = 0; b < cols / 256; b++, row += 144) {
        uint16_t h0, h1; memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2);
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0)));
        const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1)));
        const uint8_t *sc = row + 4, *q = row + 16;
        for (int j = 0; j < 8; j += 2, q += 32, x += 64, sx += 2) {
            int s1, m1, s2, m2;
            k4_scale_min(j, sc, &s1, &m1); k4_scale_min(j + 1, sc, &s2, &m2);
            const __m256i v = _mm256_loadu_si256((const __m256i *)q);
            const __m256i lo = _mm256_and_si256(v, m4);
            const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), m4);
            __m256 a = _mm256_add_ps(mul16_u8(_mm256_castsi256_si128(lo), x),
                                     mul16_u8(_mm256_extracti128_si256(lo, 1), x + 16));
            __m256 c = _mm256_add_ps(mul16_u8(_mm256_castsi256_si128(hi), x + 32),
                                     mul16_u8(_mm256_extracti128_si256(hi, 1), x + 48));
            acc = _mm256_fmadd_ps(a, _mm256_set1_ps(d * s1), acc);
            acc = _mm256_fmadd_ps(c, _mm256_set1_ps(d * s2), acc);
            corr += dmin * (m1 * sx[0] + m2 * sx[1]);
        }
    }
    return hsum256(acc) - corr;
#else
    float tmp[256], total = 0.0f;
    (void)sx;
    for (int b = 0; b < cols / 256; b++, row += 144, x += 256) {
        dequant(T_Q4_K, row, tmp, 256);
        total += dot(tmp, x, 256);
    }
    return total;
#endif
}

/* linha Q6_K vezes vetor float: pesos de 6 bits (-32..31), escala int8 por sub-bloco de 16 */
static float dot_q6k(const uint8_t *row, const float *x, int cols) {
#ifdef __AVX2__
    const __m256i m4 = _mm256_set1_epi8(0x0F), m2 = _mm256_set1_epi8(3), off = _mm256_set1_epi8(32);
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 210) {
        const uint8_t *ql = row, *qh = row + 128;
        const int8_t *sc = (const int8_t *)(row + 192);
        uint16_t h; memcpy(&h, row + 208, 2);
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h)));
        for (int c = 0; c < 2; c++, ql += 64, qh += 32, sc += 8, x += 128) {
            const __m256i l0 = _mm256_loadu_si256((const __m256i *)ql);
            const __m256i l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
            const __m256i hv = _mm256_loadu_si256((const __m256i *)qh);
            __m256i q[4];
            q[0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(hv, m2), 4));
            q[1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2), m2), 4));
            q[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 4), m2), 4));
            q[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 6), m2), 4));
            for (int k = 0; k < 4; k++) {
                const __m256i v = _mm256_sub_epi8(q[k], off);
                acc = _mm256_fmadd_ps(mul16_i8(_mm256_castsi256_si128(v), x + 32 * k),
                                      _mm256_set1_ps(d * sc[2 * k]), acc);
                acc = _mm256_fmadd_ps(mul16_i8(_mm256_extracti128_si256(v, 1), x + 32 * k + 16),
                                      _mm256_set1_ps(d * sc[2 * k + 1]), acc);
            }
        }
    }
    return hsum256(acc);
#else
    float tmp[256], total = 0.0f;
    for (int b = 0; b < cols / 256; b++, row += 210, x += 256) {
        dequant(T_Q6_K, row, tmp, 256);
        total += dot(tmp, x, 256);
    }
    return total;
#endif
}

/* ---------- produtos inteiros na CPU ----------
 * As ativacoes sao quantizadas em 16 bits por bloco de 256 colunas (x ~ q * xs[bloco]).
 * Os pesos (4 ou 6 bits) sao alargados para 16 bits e um madd (s16 x s16 -> s32) faz 16
 * produtos de uma vez, sem converter peso nenhum para float; so a soma de cada sub-bloco
 * vira float para receber a escala. Com 16 bits o erro de quantizacao das ativacoes fica
 * na casa de 1e-5 (com 8 bits, medido, chegava a trocar o token escolhido).
 * cpu_exact=1 volta aos produtos em float (referencia exata, usada pelo selftest). */
typedef struct { int16_t *q; int32_t *bs32; float *xs; int cap; } XQ;     /* bs32: soma de cada 32 valores */
static XQ xq;
static int cpu_exact;

#ifdef __AVX2__
#define HAVE_INT_DOT 1
static void xq_prepare(const float *x, int cols) {
    if (cols > xq.cap) {
        free(xq.q); free(xq.bs32); free(xq.xs);
        xq.q = xalloc((size_t)cols + 64, sizeof(int16_t));
        xq.bs32 = xalloc((size_t)cols / 32 + 16, sizeof(int32_t));
        xq.xs = xalloc((size_t)cols / 256 + 1, sizeof(float));
        xq.cap = cols;
    }
    const __m256 absm = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    for (int b = 0; b < cols / 256; b++) {
        const float *xb = x + b * 256;
        int16_t *q = xq.q + b * 256;
        __m256 mx = _mm256_setzero_ps();
        for (int j = 0; j < 256; j += 8) mx = _mm256_max_ps(mx, _mm256_and_ps(_mm256_loadu_ps(xb + j), absm));
        float t[8], amax = 0.0f;
        _mm256_storeu_ps(t, mx);
        for (int j = 0; j < 8; j++) if (t[j] > amax) amax = t[j];
        if (amax == 0.0f) {
            memset(q, 0, 256 * sizeof(int16_t)); memset(xq.bs32 + b * 8, 0, 8 * sizeof(int32_t)); xq.xs[b] = 0.0f;
            continue;
        }
        const __m256 is = _mm256_set1_ps(32767.0f / amax);
        for (int sub = 0; sub < 8; sub++) {
            __m256i sum = _mm256_setzero_si256();
            for (int j = 0; j < 32; j += 16) {
                const __m256i a = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(xb + sub * 32 + j), is));
                const __m256i c = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(xb + sub * 32 + j + 8), is));
                sum = _mm256_add_epi32(sum, _mm256_add_epi32(a, c));
                _mm256_storeu_si256((__m256i *)(q + sub * 32 + j), _mm256_permute4x64_epi64(_mm256_packs_epi32(a, c), 0xD8));
            }
            __m128i s4 = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
            s4 = _mm_add_epi32(s4, _mm_srli_si128(s4, 8));
            s4 = _mm_add_epi32(s4, _mm_srli_si128(s4, 4));
            xq.bs32[b * 8 + sub] = _mm_cvtsi128_si32(s4);
        }
        xq.xs[b] = amax / 32767.0f;
    }
}

/* soma de 32 pesos u8 (em v) vezes 32 ativacoes s16: devolve 8 parciais s32 */
static __m256i mul32_u8_s16(__m256i v, const int16_t *x) {
    const __m256i a = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(v));
    const __m256i c = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(v, 1));
    return _mm256_add_epi32(_mm256_madd_epi16(a, _mm256_loadu_si256((const __m256i *)x)),
                            _mm256_madd_epi16(c, _mm256_loadu_si256((const __m256i *)(x + 16))));
}

static float hsum128(__m128 v) {
    v = _mm_hadd_ps(v, v); v = _mm_hadd_ps(v, v);
    return _mm_cvtss_f32(v);
}

/* Q4_K inteiro. Por bloco de 256 pesos: as 8 escalas e os 8 minimos de 6 bits saem de 12
   bytes com meia duzia de operacoes de 32 bits (sem desvios). Cada peso vale
   d*sc*q - dmin*m: o termo dos minimos usa as somas de x por sub-bloco (bs32). */
static float dot_q4k_i(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F);
    const uint32_t k1 = 0x3f3f3f3fu, k2 = 0x0f0f0f0fu, k3 = 0x03030303u;
    const int16_t *xp = xq.q;
    const int32_t *bs = xq.bs32;
    __m256 acc = _mm256_setzero_ps(), accm = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 144, xp += 256, bs += 8) {
        uint16_t h0, h1; uint32_t u[4];
        memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2); memcpy(u, row + 4, 12);
        u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);     /* minimos 4..7 */
        const uint32_t aux = u[1] & k1;                              /* minimos 0..3 */
        u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);             /* escalas 4..7 */
        u[2] = aux;
        u[0] &= k1;                                                  /* escalas 0..3 */
        const float xs = xq.xs[b];
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0))) * xs;
        const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1))) * xs;
        const __m256i sc32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)u));
        const __m256i mn32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(u + 2)));
        const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(sc32), _mm256_set1_ps(d));   /* d*xs*escala[j] */
        accm = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_mullo_epi32(mn32, _mm256_loadu_si256((const __m256i *)bs))),
                               _mm256_set1_ps(dmin), accm);
        const uint8_t *q = row + 16;
        for (int j = 0; j < 4; j++) {
            const __m256i v = _mm256_loadu_si256((const __m256i *)(q + 32 * j));
            const __m256i lo = _mm256_and_si256(v, m4), hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), m4);
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(mul32_u8_s16(lo, xp + 64 * j)),
                                  _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j)), acc);
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(mul32_u8_s16(hi, xp + 64 * j + 32)),
                                  _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j + 1)), acc);
        }
    }
    return hsum256(acc) - hsum256(accm);
}

/* Q6_K inteiro: pesos de 6 bits (-32..31), uma escala int8 para cada 16 pesos */
static float dot_q6k_i(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), m2 = _mm256_set1_epi8(3), off = _mm256_set1_epi16(32);
    const int16_t *xp = xq.q;
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 210, xp += 256) {
        const uint8_t *ql = row, *qh = row + 128;
        uint16_t h; memcpy(&h, row + 208, 2);
        const __m256 dv = _mm256_set1_ps(_mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))) * xq.xs[b]);
        const __m128i s8 = _mm_loadu_si128((const __m128i *)(row + 192));
        for (int c = 0; c < 2; c++, ql += 64, qh += 32) {
            /* 8 escalas deste meio bloco, ja multiplicadas por d*xs */
            const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(c ? _mm_srli_si128(s8, 8) : s8)), dv);
            const __m256i l0 = _mm256_loadu_si256((const __m256i *)ql);
            const __m256i l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
            const __m256i hv = _mm256_loadu_si256((const __m256i *)qh);
            __m256i q[4];
            q[0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(hv, m2), 4));
            q[1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2), m2), 4));
            q[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 4), m2), 4));
            q[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 6), m2), 4));
            for (int k = 0; k < 4; k++) {
                const int16_t *x = xp + c * 128 + k * 32;
                const __m256i a = _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm256_castsi256_si128(q[k])), off);
                const __m256i e = _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm256_extracti128_si256(q[k], 1)), off);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_madd_epi16(a, _mm256_loadu_si256((const __m256i *)x))),
                                      _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * k)), acc);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_madd_epi16(e, _mm256_loadu_si256((const __m256i *)(x + 16)))),
                                      _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * k + 1)), acc);
            }
        }
    }
    return hsum256(acc);
}
#else
#define HAVE_INT_DOT 0
static void xq_prepare(const float *x, int cols) { (void)x; (void)cols; }
static float dot_q4k_i(const uint8_t *row, int cols) { (void)row; (void)cols; return 0.0f; }
static float dot_q6k_i(const uint8_t *row, int cols) { (void)row; (void)cols; return 0.0f; }
#endif

/* a matriz usa o produto inteiro? (so tipos K, e se nao estiver no modo exato) */
static int mat_int(const Mat *W) {
    return HAVE_INT_DOT && !cpu_exact && W->q && (W->type == T_Q4_K || W->type == T_Q6_K);
}

static float dot_row(const Mat *W, size_t i, const float *x, const float *sx, int cols) {
    if (W->f) return dot(W->f + i * cols, x, cols);
    const uint8_t *row = W->q + i * W->stride;
    if (mat_int(W)) return W->type == T_Q4_K ? dot_q4k_i(row, cols) : dot_q6k_i(row, cols);
    if (W->type == T_Q4_K) return dot_q4k(row, x, sx, cols);
    if (W->type == T_Q6_K) return dot_q6k(row, x, cols);
    return dot_q8(row, x, cols);
}

/* somas de x por sub-bloco de 32 (usadas pelo Q4_K) */
static void block_sums(float *sx, const float *x, int cols) {
    for (int b = 0; b < cols / 32; b++) {
        float s = 0.0f;
        for (int j = 0; j < 32; j++) s += x[b * 32 + j];
        sx[b] = s;
    }
}

#define MAX_SX 2048   /* suporta ate 65536 colunas */

/* ================= GPU: OpenCL carregado em tempo de execucao =================
 * Sem SDK: as funcoes vem do OpenCL.dll / libOpenCL.so do driver e os kernels sao
 * compilados pelo proprio driver. Os pesos sobem quantizados, como estao no arquivo,
 * e sao decodificados dentro do kernel.
 *
 * Dois modos:
 *   grafo completo - tudo cabe na VRAM: o token inteiro roda na GPU (pesos, cache KV,
 *                    atencao, normas); por token ha 1 escrita (embedding) e 1 leitura (logits).
 *   hibrido        - so parte cabe: as matrizes que couberam fazem o produto na GPU.
 * KATANA_CPU=1 desliga a GPU.
 */

static double now_sec(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

typedef void *clh;

static struct {
    int32_t (*GetPlatformIDs)(uint32_t, clh *, uint32_t *);
    int32_t (*GetDeviceIDs)(clh, uint64_t, uint32_t, clh *, uint32_t *);
    int32_t (*GetDeviceInfo)(clh, uint32_t, size_t, void *, size_t *);
    clh     (*CreateContext)(const intptr_t *, uint32_t, const clh *, void *, void *, int32_t *);
    clh     (*CreateCommandQueue)(clh, clh, uint64_t, int32_t *);
    clh     (*CreateProgramWithSource)(clh, uint32_t, const char **, const size_t *, int32_t *);
    int32_t (*BuildProgram)(clh, uint32_t, const clh *, const char *, void *, void *);
    int32_t (*GetProgramBuildInfo)(clh, clh, uint32_t, size_t, void *, size_t *);
    clh     (*CreateKernel)(clh, const char *, int32_t *);
    clh     (*CreateBuffer)(clh, uint64_t, size_t, void *, int32_t *);
    int32_t (*SetKernelArg)(clh, uint32_t, size_t, const void *);
    int32_t (*EnqueueNDRangeKernel)(clh, clh, uint32_t, const size_t *, const size_t *, const size_t *, uint32_t, const clh *, clh *);
    int32_t (*EnqueueReadBuffer)(clh, clh, uint32_t, size_t, size_t, void *, uint32_t, const clh *, clh *);
    int32_t (*EnqueueWriteBuffer)(clh, clh, uint32_t, size_t, size_t, const void *, uint32_t, const clh *, clh *);
    int32_t (*Finish)(clh);
} cl;

#define GPU_BMAX 16     /* maior lote de tokens por passada */

/* um conjunto de kernels compilado para um tamanho de lote fixo (NB) */
typedef struct {
    int nb, nr;     /* tokens por passada; linhas da matriz por thread */
    clh k_f32, k_q8, k_q4k, k_q6k, k_bsum;
    clh kp_f32, kp_q8, kp_q4k, kp_q6k;      /* mesmos kernels com 2*nr linhas por thread (par gate/up) */
    clh k_rms, k_qkv, k_gather, k_att_scores, k_att_softmax, k_att_mix;
} KSet;

typedef struct {
    int on, full, force_cpu, debug, test_mv;
    int n_gl;                                           /* camadas inteiras na GPU (as primeiras) */
    clh ctx, queue;
    KSet set[4]; int n_sets;                            /* nb decrescente; o ultimo e nb=1 */
    clh bpart, bsx;                                     /* parciais dos segmentos; somas de x */
    size_t seg_target, local;                           /* ajuste: KATANA_SEG, KATANA_LOCAL */
    clh xbuf, obuf;                                     /* modo hibrido */
    clh bx, bxb, bq, bk, bv, bhb, blog, batt, bfreq, bkc, bvc, bon;   /* grafo completo */
    clh *an, *fn, *lbq, *lbk, *lbv, *lbo;               /* normas e bias por camada */
    float *xT;                                          /* embeddings do lote, transpostos */
    char name[128];
    uint64_t vram, used;
    int n_up, n_total;
} Gpu;

static Gpu gpu;

/* fracao da VRAM que o motor pode ocupar (o resto fica para o driver e o sistema) */
static uint64_t gpu_budget(void) {
    int pct = getenv("KATANA_VRAM_PCT") ? atoi(getenv("KATANA_VRAM_PCT")) : 92;
    if (pct < 10 || pct > 99) pct = 92;
    return gpu.vram / 100 * (uint64_t)pct;
}
static int32_t gerr;
static struct { clh k; const char *name; double t; long calls; double elems; } prof[64];
static int n_prof;
static double prof_elems;   /* elementos de peso do produto que esta sendo enfileirado */

static const char *GPU_SRC =
/* O mesmo fonte e compilado uma vez por tamanho de lote (-DNB=1, 4, 16).
   Ativacoes ficam transpostas: X[coluna*NB + token]. Assim os NB valores de uma coluna
   sao um vetor so (FB) e cada peso decodificado faz UMA multiplicacao vetorial pelo lote:
   o peso e lido e decodificado uma vez para NB tokens. */
"#if NB == 1\n"
"#define FB float\n#define LOADB(p, k) ((p)[k])\n#define STOREB(v, p, k) (p)[k] = (v)\n"
"#elif NB == 4\n"
"#define FB float4\n#define LOADB(p, k) vload4(k, p)\n#define STOREB(v, p, k) vstore4(v, k, p)\n"
"#elif NB == 8\n"
"#define FB float8\n#define LOADB(p, k) vload8(k, p)\n#define STOREB(v, p, k) vstore8(v, k, p)\n"
"#else\n"
"#define FB float16\n#define LOADB(p, k) vload16(k, p)\n#define STOREB(v, p, k) vstore16(v, k, p)\n"
"#endif\n"
/* Pesos (layout nosso, nao o do arquivo): cada linha vira `wpr` palavras de 32 bits e as
   linhas sao entrelacadas de 32 em 32: palavra w da linha i em W[(i/32)*wpr*32 + w*32 + i%32].
   As 32 threads de um warp leem 32 palavras consecutivas a cada passo (acesso coalescido).
   Cada thread calcula NR linhas de uma vez (de NR grupos de 32 vizinhos): os valores de x
   sao carregados uma vez e reusados nas NR linhas (ladrilhamento em registradores).
   Um buffer pode conter varias matrizes intercaladas por grupo (gs = quantas, go = qual). */
"#if NB * NR > 32\n#define MAXSEG 4\n#else\n#define MAXSEG 8\n#endif\n"
"#define GIDX(r) (((i >> 5) * NR + (r)) * gs + go)\n"
"#define RPTR(r, wpr) (W + (size_t)GIDX(r) * (size_t)(wpr) * 32 + (i & 31))\n"
"#define RROW(r) ((((i >> 5) * NR + (r)) << 5) + (i & 31))\n"
/* fecho: bias opcional (bit 4) e modo: 0 grava, 1 soma (residual), 2 SwiGLU com out */
"#define FIN(t, row) { FB t_ = (t); if (mode & 4) t_ += bias[row]; int mm = mode & 3;\\\n"
"  if (mm == 1) t_ += LOADB(out, row);\\\n"
"  else if (mm == 2) { FB g_ = LOADB(out, row); t_ = g_ / (1.0f + exp(-g_)) * t_; }\\\n"
"  STOREB(t_, out, row); }\n"
/* modo 8 (par): as linhas pares do buffer sao o gate e as impares o up da mesma linha;
   a thread tem os dois produtos e ja grava silu(gate)*up */
"#if NR >= 2\n"
"#define PAIRS if (mode & 8) { for (int r = 0; r < NR; r += 2) {\\\n"
"    int row = ((((i >> 5) * NR + r) >> 1) << 5) + (i & 31);\\\n"
"    if (row < rows) { FB g_ = total[r]; STOREB(g_ / (1.0f + exp(-g_)) * total[r + 1], out, row); } }\\\n"
"    return; }\n"
"#else\n"
"#define PAIRS\n"
"#endif\n"
/* com segmentos de colunas (dimensao 1 do NDRange) as threads do mesmo grupo de trabalho
   (32 linhas x nseg segmentos) somam os parciais pela memoria local: um kernel so */
"#define FINISH \\\n"
"  if (nseg > 1) { int sg = get_global_id(1), lane = i & 31;\\\n"
"    for (int r = 0; r < NR; r++) lp[(r * MAXSEG + sg) * 32 + lane] = total[r];\\\n"
"    barrier(CLK_LOCAL_MEM_FENCE);\\\n"
"    if (sg != 0) return;\\\n"
"    for (int r = 0; r < NR; r++) { FB t = (FB)(0.0f);\\\n"
"      for (int q_ = 0; q_ < nseg; q_++) t += lp[(r * MAXSEG + q_) * 32 + lane];\\\n"
"      total[r] = t; } }\\\n"
"  PAIRS\\\n"
"  for (int r = 0; r < NR; r++) { int row = RROW(r); if (row < rows) FIN(total[r], row) }\n"
"#define MVARGS __global const uint *W, __global const float *x, __global float *out,\\\n"
"  __global const float *bias, __global const float *sx,\\\n"
"  int cols, int mode, int rows, int nseg, int gs, int go\n"
"#define MVLOCAL __local FB lp[NR * MAXSEG * 32];\n"
"inline float h2f(uint h) {\n"
"  uint e = (h >> 10) & 31, m = h & 1023;\n"
"  float v = e == 0 ? ldexp((float)m, -24) : ldexp((float)(m | 1024), (int)e - 25);\n"
"  return (h & 0x8000) ? -v : v;\n"
"}\n"
"inline uint sb(const uint *hw, int k) { return (hw[k >> 2] >> ((k & 3) * 8)) & 255; }\n"
"inline void k4(int j, const uint *hw, int *d, int *m) {\n"
"  if (j < 4) { *d = sb(hw, j) & 63; *m = sb(hw, j + 4) & 63; }\n"
"  else { *d = (sb(hw, j + 4) & 0xF) | ((sb(hw, j - 4) >> 6) << 4); *m = (sb(hw, j + 4) >> 4) | ((sb(hw, j) >> 6) << 4); }\n"
"}\n"
"__kernel void mv_f32(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, cols); total[r] = (FB)(0.0f); }\n"
"  for (int j = 0; j < cols; j++) {\n"
"    FB xv = LOADB(x, j);\n"
"    for (int r = 0; r < NR; r++) total[r] += as_float(p[r][(size_t)j * 32]) * xv;\n"
"  }\n"
"  FINISH\n"
"}\n"
/* Q8: 9 palavras por bloco de 32 pesos: escala float + 32 int8 */
"__kernel void mv_q8(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  int nb = cols >> 5, bps = nb / nseg, b0 = get_global_id(1) * bps;\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, nb * 9) + (size_t)b0 * (9 * 32); total[r] = (FB)(0.0f); }\n"
"  __global const float *xb = x + (size_t)b0 * 32 * NB;\n"
"  for (int b = 0; b < bps; b++, xb += 32 * NB) {\n"
"    FB a[NR];\n"
"    for (int r = 0; r < NR; r++) a[r] = (FB)(0.0f);\n"
"    for (int l = 0; l < 8; l++) {\n"
"      FB x0 = LOADB(xb, 4 * l), x1 = LOADB(xb, 4 * l + 1), x2 = LOADB(xb, 4 * l + 2), x3 = LOADB(xb, 4 * l + 3);\n"
"      for (int r = 0; r < NR; r++) {\n"
"        int w = (int)p[r][(1 + l) * 32];\n"
"        a[r] += (float)((w << 24) >> 24) * x0 + (float)((w << 16) >> 24) * x1\n"
"              + (float)((w << 8) >> 24) * x2 + (float)(w >> 24) * x3;\n"
"      }\n"
"    }\n"
"    for (int r = 0; r < NR; r++) { total[r] += as_float(p[r][0]) * a[r]; p[r] += 9 * 32; }\n"
"  }\n"
"  FINISH\n"
"}\n"
/* Q4_K: 36 palavras por bloco de 256 pesos (mesmos bytes do arquivo). Cada peso vale
   d*sc*q - dmin*m; o termo do minimo usa a soma de x por sub-bloco, igual para todas as
   linhas: calculada uma vez por thread quando NB=1, ou vinda do kernel bsum (sx) em lote. */
"__kernel void mv_q4k(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  int nb = cols >> 8, bps = nb / nseg, b0 = get_global_id(1) * bps;\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, nb * 36) + (size_t)b0 * (36 * 32); total[r] = (FB)(0.0f); }\n"
"  __global const float *xb = x + (size_t)b0 * 256 * NB;\n"
"  for (int b = 0; b < bps; b++) {\n"
"    float d[NR], dmin[NR]; uint hw[NR][3];\n"
"    for (int r = 0; r < NR; r++) {\n"
"      uint h0 = p[r][0];\n"
"      hw[r][0] = p[r][32]; hw[r][1] = p[r][64]; hw[r][2] = p[r][96];\n"
"      d[r] = h2f(h0 & 0xFFFF); dmin[r] = h2f(h0 >> 16);\n"
"    }\n"
"    for (int j = 0; j < 8; j += 2, xb += 64 * NB) {\n"
"      FB a[NR], c[NR];\n"
"      for (int r = 0; r < NR; r++) { a[r] = (FB)(0.0f); c[r] = (FB)(0.0f); }\n"
"#if NB == 1\n"
"      FB xa = 0.0f, xc = 0.0f;\n"
"#else\n"
"      FB xa = LOADB(sx, (b0 + b) * 8 + j), xc = LOADB(sx, (b0 + b) * 8 + j + 1);\n"
"#endif\n"
"      for (int l = 0; l < 8; l++) {\n"
"        FB x0 = LOADB(xb, 4 * l), x1 = LOADB(xb, 4 * l + 1), x2 = LOADB(xb, 4 * l + 2), x3 = LOADB(xb, 4 * l + 3);\n"
"        FB y0 = LOADB(xb, 32 + 4 * l), y1 = LOADB(xb, 33 + 4 * l), y2 = LOADB(xb, 34 + 4 * l), y3 = LOADB(xb, 35 + 4 * l);\n"
"#if NB == 1\n"
"        xa += x0 + x1 + x2 + x3; xc += y0 + y1 + y2 + y3;\n"
"#endif\n"
"        for (int r = 0; r < NR; r++) {\n"
"          uint w = p[r][(4 + j * 4 + l) * 32];\n"
"          a[r] += (float)(w & 15) * x0 + (float)((w >> 8) & 15) * x1 + (float)((w >> 16) & 15) * x2 + (float)((w >> 24) & 15) * x3;\n"
"          c[r] += (float)((w >> 4) & 15) * y0 + (float)((w >> 12) & 15) * y1 + (float)((w >> 20) & 15) * y2 + (float)(w >> 28) * y3;\n"
"        }\n"
"      }\n"
"      for (int r = 0; r < NR; r++) {\n"
"        int s1, m1, s2, m2;\n"
"        k4(j, hw[r], &s1, &m1); k4(j + 1, hw[r], &s2, &m2);\n"
"        total[r] += d[r] * ((float)s1 * a[r] + (float)s2 * c[r]) - dmin[r] * ((float)m1 * xa + (float)m2 * xc);\n"
"      }\n"
"    }\n"
"    for (int r = 0; r < NR; r++) p[r] += 36 * 32;\n"
"  }\n"
"  FINISH\n"
"}\n"
/* Q6 reempacotado: 53 palavras por bloco de 256 pesos:
   48 palavras de pesos (16 pesos de 6 bits a cada 3 palavras), 4 de escalas int8, 1 float d */
"#define QT(v, k) ((float)((int)((v) & 63) - 32) * xv[k])\n"
"__kernel void mv_q6k(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  int nb = cols >> 8, bps = nb / nseg, b0 = get_global_id(1) * bps;\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, nb * 53) + (size_t)b0 * (53 * 32); total[r] = (FB)(0.0f); }\n"
"  __global const float *xb = x + (size_t)b0 * 256 * NB;\n"
"  for (int b = 0; b < bps; b++) {\n"
"    FB acc[NR];\n"
"    for (int r = 0; r < NR; r++) acc[r] = (FB)(0.0f);\n"
"    for (int g = 0; g < 4; g++) {\n"
"      for (int k = 0; k < 4; k++, xb += 16 * NB) {\n"
"        FB xv[16];\n"
"        for (int u = 0; u < 16; u++) xv[u] = LOADB(xb, u);\n"
"        for (int r = 0; r < NR; r++) {\n"
"          __global const uint *q = p[r] + (g * 4 + k) * 3 * 32;\n"
"          uint w0 = q[0], w1 = q[32], w2 = q[64];\n"
"          int scw = (int)p[r][(48 + g) * 32];\n"
"          FB s = QT(w0, 0) + QT(w0 >> 6, 1) + QT(w0 >> 12, 2) + QT(w0 >> 18, 3) + QT(w0 >> 24, 4)\n"
"               + QT((w0 >> 30) | (w1 << 2), 5) + QT(w1 >> 4, 6) + QT(w1 >> 10, 7) + QT(w1 >> 16, 8)\n"
"               + QT(w1 >> 22, 9) + QT((w1 >> 28) | (w2 << 4), 10) + QT(w2 >> 2, 11) + QT(w2 >> 8, 12)\n"
"               + QT(w2 >> 14, 13) + QT(w2 >> 20, 14) + QT(w2 >> 26, 15);\n"
"          acc[r] += (float)((scw << (24 - 8 * k)) >> 24) * s;\n"
"        }\n"
"      }\n"
"    }\n"
"    for (int r = 0; r < NR; r++) { total[r] += as_float(p[r][52 * 32]) * acc[r]; p[r] += 53 * 32; }\n"
"  }\n"
"  FINISH\n"
"}\n"
/* somas de x por sub-bloco de 32 colunas (termo do minimo do Q4_K em lote) */
"__kernel void bsum(__global const float *x, __global float *sx) {\n"
"  int i = get_global_id(0);\n"
"  FB s = (FB)(0.0f);\n"
"  for (int j = 0; j < 32; j++) s += LOADB(x, i * 32 + j);\n"
"  STOREB(s, sx, i);\n"
"}\n"
/* RMSNorm num kernel so: cada thread refaz a soma dos quadrados (n e pequeno e x esta no cache) */
"__kernel void rms(__global const float *x, __global const float *g, __global float *out, int n, float eps) {\n"
"  int i = get_global_id(0);\n"
"  FB ss = (FB)(0.0f);\n"
"  for (int j = 0; j < n; j++) { FB v = LOADB(x, j); ss += v * v; }\n"
"  STOREB(g[i] * (LOADB(x, i) / sqrt(ss / (float)n + eps)), out, i);\n"
"}\n"
/* copia a coluna `off` de um buffer transposto para um vetor simples */
"__kernel void gather(__global const float *src, __global float *dst, int stride, int off) {\n"
"  int i = get_global_id(0);\n"
"  dst[i] = src[(size_t)i * stride + off];\n"
"}\n"
/* pos-projecao de q, k e v num kernel so: bias opcional, RoPE em q (no lugar) e em k
   (direto no cache KV) e copia de v para o cache; o token b do lote esta na posicao pos0+b */
"__kernel void qkv_post(__global float *q, __global const float *k, __global const float *v,\n"
"    __global const float *bq, __global const float *bk, __global const float *bv,\n"
"    __global float *kc, __global float *vc, __global const float *freq,\n"
"    int base, int pos0, int hd, int neox, int has_bias, int d, int kvd) {\n"
"  int gid = get_global_id(0), b = gid % NB, p = gid / NB, half_ = hd / 2;\n"
"  int pos = pos0 + b, off = base + pos * kvd;\n"
"  if (p >= d / 2 + kvd / 2) {\n"
"    p -= d / 2 + kvd / 2;\n"
"    vc[off + p] = v[p * NB + b] + ((has_bias & 4) ? bv[p] : 0.0f);\n"
"    return;\n"
"  }\n"
"  int isk = p >= d / 2;\n"
"  if (isk) p -= d / 2;\n"
"  int h = p / half_, i = p % half_;\n"
"  int i0 = h * hd + (neox ? i : 2 * i), i1 = i0 + (neox ? half_ : 1);\n"
"  float ang = pos * freq[i], cs = cos(ang), sn = sin(ang);\n"
"  if (isk) {\n"
"    float a = k[i0 * NB + b], c = k[i1 * NB + b];\n"
"    if (has_bias & 2) { a += bk[i0]; c += bk[i1]; }\n"
"    kc[off + i0] = a * cs - c * sn; kc[off + i1] = a * sn + c * cs;\n"
"  } else {\n"
"    float a = q[i0 * NB + b], c = q[i1 * NB + b];\n"
"    if (has_bias & 1) { a += bq[i0]; c += bq[i1]; }\n"
"    q[i0 * NB + b] = a * cs - c * sn; q[i1 * NB + b] = a * sn + c * cs;\n"
"  }\n"
"}\n"
/* atencao: notas (token x cabeca x posicao; so t <= posicao do token), softmax, mistura */
"__kernel void att_scores(__global const float *q, __global const float *kc, __global float *att,\n"
"    int base, int pos0, int tmax, int ctx, int hd, int kvd, int group, int nh, float scale) {\n"
"  int gid = get_global_id(0), t = gid % tmax, bh = gid / tmax, h = bh % nh, b = bh / nh;\n"
"  if (t > pos0 + b) return;\n"
"  __global const float *kh = kc + base + t * kvd + (h / group) * hd;\n"
"  float s = 0.0f;\n"
"  for (int j = 0; j < hd; j++) s += q[(h * hd + j) * NB + b] * kh[j];\n"
"  att[(size_t)bh * ctx + t] = s * scale;\n"
"}\n"
"__kernel void att_softmax(__global float *att, int pos0, int ctx, int nh) {\n"
"  int gid = get_global_id(0), T = pos0 + gid / nh + 1;\n"
"  __global float *a = att + (size_t)gid * ctx;\n"
"  float mx = a[0], sum = 0.0f;\n"
"  for (int t = 1; t < T; t++) mx = max(mx, a[t]);\n"
"  for (int t = 0; t < T; t++) { float e = exp(a[t] - mx); a[t] = e; sum += e; }\n"
"  float inv = 1.0f / sum;\n"
"  for (int t = 0; t < T; t++) a[t] *= inv;\n"
"}\n"
"__kernel void att_mix(__global const float *att, __global const float *vc, __global float *out,\n"
"    int base, int pos0, int ctx, int hd, int kvd, int group, int nh) {\n"
"  int gid = get_global_id(0), b = gid % NB, i = gid / NB, h = i / hd, j = i % hd;\n"
"  __global const float *a = att + (size_t)(b * nh + h) * ctx;\n"
"  __global const float *v = vc + base + (h / group) * hd + j;\n"
"  float s = 0.0f;\n"
"  for (int t = 0; t <= pos0 + b; t++) s += a[t] * v[t * kvd];\n"
"  out[i * NB + b] = s;\n"
"}\n";

static int cl_load(void) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA("OpenCL.dll");
    if (!h) return 0;
#define SYM(f) if ((*(void **)&cl.f = (void *)GetProcAddress(h, "cl" #f)) == NULL) return 0
#else
    void *h = dlopen("libOpenCL.so.1", RTLD_NOW);
    if (!h) h = dlopen("libOpenCL.so", RTLD_NOW);
    if (!h) return 0;
#define SYM(f) if ((*(void **)&cl.f = dlsym(h, "cl" #f)) == NULL) return 0
#endif
    SYM(GetPlatformIDs); SYM(GetDeviceIDs); SYM(GetDeviceInfo); SYM(CreateContext);
    SYM(CreateCommandQueue); SYM(CreateProgramWithSource); SYM(BuildProgram);
    SYM(GetProgramBuildInfo); SYM(CreateKernel); SYM(CreateBuffer); SYM(SetKernelArg);
    SYM(EnqueueNDRangeKernel); SYM(EnqueueReadBuffer); SYM(EnqueueWriteBuffer); SYM(Finish);
#undef SYM
    return 1;
}

/* compila o fonte para um tamanho de lote e um numero de linhas por thread */
static clh gpu_compile(int nb, int nr, clh dev) {
    int32_t err = 0;
    char opts[64];
    snprintf(opts, sizeof opts, "-cl-mad-enable -DNB=%d -DNR=%d", nb, nr);
    clh prog = cl.CreateProgramWithSource(gpu.ctx, 1, &GPU_SRC, NULL, &err);
    if (err) return NULL;
    if (cl.BuildProgram(prog, 1, &dev, opts, NULL, NULL) != 0) {
        static char log[8192];
        cl.GetProgramBuildInfo(prog, dev, 0x1183, sizeof log - 1, log, NULL);
        fprintf(stderr, "GPU: falha ao compilar os kernels (lote %d, linhas %d):\n%s\n", nb, nr, log);
        return NULL;
    }
    return prog;
}

static int gpu_build_set(KSet *ks, int nb, int nr, clh dev) {
    int32_t err = 0;
    clh prog = gpu_compile(nb, nr, dev), prog2 = gpu_compile(nb, nr * 2, dev);
    if (!prog || !prog2) return 0;
    ks->nb = nb; ks->nr = nr;
#define KERN(pr, field, kname) ks->field = cl.CreateKernel(pr, kname, &err); if (err) return 0; \
    if (n_prof < 64) { prof[n_prof].k = ks->field; prof[n_prof++].name = kname; }
    KERN(prog, k_f32, "mv_f32"); KERN(prog, k_q8, "mv_q8"); KERN(prog, k_q4k, "mv_q4k"); KERN(prog, k_q6k, "mv_q6k");
    KERN(prog2, kp_f32, "mv_f32"); KERN(prog2, kp_q8, "mv_q8"); KERN(prog2, kp_q4k, "mv_q4k"); KERN(prog2, kp_q6k, "mv_q6k");
    KERN(prog, k_bsum, "bsum"); KERN(prog, k_rms, "rms"); KERN(prog, k_qkv, "qkv_post");
    KERN(prog, k_gather, "gather"); KERN(prog, k_att_scores, "att_scores");
    KERN(prog, k_att_softmax, "att_softmax"); KERN(prog, k_att_mix, "att_mix");
#undef KERN
    return 1;
}

/* escolhe a GPU (prefere placa dedicada), cria contexto, fila e kernels */
static int gpu_init(void) {
    if (!cl_load()) return 0;
    clh plats[8], devs[8], best = NULL;
    uint32_t np = 0;
    int best_score = -1;
    if (cl.GetPlatformIDs(8, plats, &np) != 0) return 0;
    for (uint32_t i = 0; i < np && i < 8; i++) {
        uint32_t nd = 0;
        if (cl.GetDeviceIDs(plats[i], 4 /* GPU */, 8, devs, &nd) != 0) continue;
        for (uint32_t j = 0; j < nd && j < 8; j++) {
            char vendor[128] = {0}, name[128] = {0};
            cl.GetDeviceInfo(devs[j], 0x102C, sizeof vendor - 1, vendor, NULL);
            cl.GetDeviceInfo(devs[j], 0x102B, sizeof name - 1, name, NULL);
            int score = (strstr(vendor, "NVIDIA") || strstr(vendor, "Advanced Micro") || strstr(vendor, "AMD")) ? 2 : 1;
            if (score > best_score) {
                best_score = score; best = devs[j];
                memcpy(gpu.name, name, sizeof gpu.name);
                cl.GetDeviceInfo(devs[j], 0x101F, sizeof gpu.vram, &gpu.vram, NULL);
            }
        }
    }
    if (!best) return 0;

    int32_t err = 0;
    gpu.ctx = cl.CreateContext(NULL, 1, &best, NULL, NULL, &err);
    if (err) return 0;
    gpu.queue = cl.CreateCommandQueue(gpu.ctx, best, 0, &err);
    if (err) return 0;
    gpu.debug = getenv("KATANA_DEBUG") != NULL;

    /* lotes: KATANA_BATCH limita o maior (1 = sem lote) */
    static const int sizes[] = { 16, 4, 1 };
    int maxb = getenv("KATANA_BATCH") ? atoi(getenv("KATANA_BATCH")) : GPU_BMAX;
    for (int i = 0; i < 3; i++) {
        if (sizes[i] > maxb && sizes[i] != 1) continue;
        const char *ev = getenv(sizes[i] == 1 ? "KATANA_ROWS" : "KATANA_ROWSB");
        int nr = ev ? atoi(ev) : (sizes[i] == 1 ? 1 : 2);   /* medido na RTX 3050 */
        if (nr != 1 && nr != 2) nr = 1;      /* o par gate/up usa 2*nr <= 4 */
        if (!gpu_build_set(&gpu.set[gpu.n_sets], sizes[i], nr, best)) return 0;
        gpu.n_sets++;
    }
    return 1;
}

/* palavras de 32 bits por linha no layout da GPU */
static size_t gpu_wpr(const Mat *W, size_t cols) {
    if (!W->q) return cols;
    if (W->type == T_Q4_K) return cols / 256 * 36;
    if (W->type == T_Q6_K) return cols / 256 * 53;
    return cols / 32 * 9;
}

/* converte a linha r para as palavras do layout da GPU */
static void gpu_pack_row(const Mat *W, size_t r, size_t cols, uint32_t *o) {
    if (!W->q) { memcpy(o, W->f + r * cols, cols * 4); return; }
    const uint8_t *src = W->q + r * W->stride;
    if (W->type == T_Q4_K) { memcpy(o, src, cols / 256 * 144); return; }
    if (W->type == T_Q8_0) {
        for (size_t b = 0; b < cols / 32; b++, src += 34, o += 9) {
            uint16_t h; memcpy(&h, src, 2);
            float d = f16_to_f32(h);
            memcpy(o, &d, 4); memcpy(o + 1, src + 2, 32);
        }
        return;
    }
    for (size_t b = 0; b < cols / 256; b++, src += 210, o += 53) {      /* Q6_K */
        uint8_t qv[256];
        const uint8_t *ql = src, *qh = src + 128;
        for (int c = 0; c < 2; c++, ql += 64, qh += 32) {
            uint8_t *y = qv + c * 128;
            for (int l = 0; l < 32; l++) {
                y[l]      = (uint8_t)((ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4));
                y[l + 32] = (uint8_t)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4));
                y[l + 64] = (uint8_t)((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4));
                y[l + 96] = (uint8_t)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4));
            }
        }
        for (int g = 0; g < 16; g++) {          /* 16 pesos de 6 bits em 96 bits */
            uint64_t lo = 0, hi = 0;
            for (int k = 0; k < 16; k++) {
                const int bit = 6 * k;
                const uint64_t v = qv[g * 16 + k];
                if (bit < 64) { lo |= v << bit; if (bit > 58) hi |= v >> (64 - bit); }
                else hi |= v << (bit - 64);
            }
            o[g * 3] = (uint32_t)lo; o[g * 3 + 1] = (uint32_t)(lo >> 32); o[g * 3 + 2] = (uint32_t)hi;
        }
        memcpy(o + 48, src + 192, 16);
        uint16_t h; memcpy(&h, src + 208, 2);
        float d = f16_to_f32(h);
        memcpy(o + 52, &d, 4);
    }
}

/* envia n matrizes de mesmo formato e tipo para um unico buffer na VRAM, no layout
   entrelacado e intercaladas por grupo de 32 linhas (nada acontece se nao couber) */
static void gpu_upload_n(Mat **ms, int n, size_t rows, size_t cols) {
    const size_t wpr = gpu_wpr(ms[0], cols), rowsP = (rows + 127) & ~(size_t)127;
    const size_t bytes = rowsP * wpr * 4 * n;
    gpu.n_total += n;
    if (gpu.used + bytes > gpu_budget()) return;
    uint32_t *buf = calloc(rowsP * wpr * n, 4);
    if (!buf) return;
    long long g;
    const long long ng = (long long)(rowsP / 32) * n;
#pragma omp parallel for schedule(static)
    for (g = 0; g < ng; g++) {
        const Mat *W = ms[g % n];
        const size_t r0 = (size_t)(g / n) * 32;
        uint32_t *tmp = malloc(wpr * 4);
        uint32_t *base = buf + (size_t)g * wpr * 32;
        for (size_t k = 0; k < 32 && r0 + k < rows; k++) {
            gpu_pack_row(W, r0 + k, cols, tmp);
            for (size_t w = 0; w < wpr; w++) base[w * 32 + k] = tmp[w];
        }
        free(tmp);
    }
    int32_t err = 0;
    clh b = cl.CreateBuffer(gpu.ctx, 4 | 32 /* READ_ONLY | COPY_HOST_PTR */, bytes, buf, &err);
    free(buf);
    if (err || !b) return;
    for (int i = 0; i < n; i++) { ms[i]->gbuf = b; ms[i]->gs = n; ms[i]->go = i; }
    gpu.used += bytes; gpu.n_up += n;
}

static void gpu_upload(Mat *W, size_t rows, size_t cols) { gpu_upload_n(&W, 1, rows, cols); }

/* gate e up do feed-forward: se tiverem o mesmo tipo vao intercalados num buffer so, e um
   unico kernel calcula os dois produtos e o SwiGLU */
static void gpu_upload_glu(Mat *gate, Mat *up, size_t rows, size_t cols) {
    if ((gate->q != NULL) == (up->q != NULL) && gate->type == up->type && !getenv("KATANA_NOPAIR")) {
        Mat *ms[2] = { gate, up };
        gpu_upload_n(ms, 2, rows, cols);
        gate->pair = gate->gbuf != NULL;
    } else { gpu_upload(gate, rows, cols); gpu_upload(up, rows, cols); }
}

/* buffer de leitura/escrita na VRAM; NULL se nao couber */
static clh gbuf_rw(size_t bytes) {
    int32_t err = 0;
    if (gpu.used + bytes > gpu_budget()) return NULL;
    clh b = cl.CreateBuffer(gpu.ctx, 1, bytes, NULL, &err);
    if (err) return NULL;
    gpu.used += bytes;
    return b;
}
/* vetor float32 constante (normas, bias); NULL se src for NULL */
static clh gbuf_f(const float *src, size_t n, int *fail) {
    int32_t err = 0;
    if (!src) return NULL;
    clh b = cl.CreateBuffer(gpu.ctx, 4 | 32, n * sizeof(float), (void *)src, &err);
    if (err || !b) { *fail = 1; return NULL; }
    gpu.used += n * sizeof(float);
    return b;
}

/* quantos segmentos de colunas (divisor de nb, ate 8 ou 4) usar para `threads` threads */
static int gpu_nseg(size_t threads, int nb, int lote_x_linhas) {
    const int maxseg = lote_x_linhas > 32 ? 4 : 8;      /* igual ao MAXSEG dos kernels */
    int best = 1;
    if (!gpu.seg_target || nb <= 1 || threads >= gpu.seg_target) return 1;
    for (int sg = 1; sg <= maxseg && sg <= nb; sg++) if (nb % sg == 0) {
        best = sg;
        if (threads * sg >= gpu.seg_target) break;
    }
    return best;
}

/* cria os buffers de trabalho do grafo (sem o cache KV); devolve 0 se algo nao couber */
static int gpu_setup_bufs(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab;
    const size_t ctx = c->ctx, L = c->n_layers, B = GPU_BMAX;
    int fail = 0;
    gpu.bx = gbuf_rw(d * B * 4);    gpu.bxb = gbuf_rw(d * B * 4);   gpu.bq = gbuf_rw(d * B * 4);
    gpu.bk = gbuf_rw(kvd * B * 4);  gpu.bv = gbuf_rw(kvd * B * 4);  gpu.bhb = gbuf_rw(H * B * 4);
    gpu.blog = gbuf_rw(V * 4);      gpu.batt = gbuf_rw(B * c->n_heads * ctx * 4);
    if (!gpu.bx || !gpu.bxb || !gpu.bq || !gpu.bk || !gpu.bv || !gpu.bhb || !gpu.blog || !gpu.batt) return 0;
    gpu.bfreq = gbuf_f(m->rope_freq, c->hd / 2, &fail);
    gpu.bon = gbuf_f(m->out_norm, d, &fail);
    gpu.an = xalloc(L, sizeof(clh));  gpu.fn = xalloc(L, sizeof(clh));
    gpu.lbq = xalloc(L, sizeof(clh)); gpu.lbk = xalloc(L, sizeof(clh));
    gpu.lbv = xalloc(L, sizeof(clh)); gpu.lbo = xalloc(L, sizeof(clh));
    gpu.xT = xalloc(d * B, sizeof(float));
    return !fail && gpu.bfreq && gpu.bon;
}

/* bytes que uma matriz ocupa na VRAM no nosso layout */
static size_t gpu_mat_bytes(const Mat *W, size_t rows, size_t cols) {
    return ((rows + 127) & ~(size_t)127) * gpu_wpr(W, cols) * 4;
}

/* Decide o que vai para a VRAM. As camadas sobem em ordem, inteiras, enquanto couberem
   (pesos + cache KV da camada). Se todas couberem, a saida (lm_head) tambem sobe e o token
   inteiro roda na GPU; senao as primeiras n_gl camadas rodam na GPU e o resto na CPU, com
   uma unica transferencia do estado por token. KATANA_GPU_LAYERS limita n_gl. */
static void gpu_setup(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab;
    const size_t ctx = c->ctx, kv_layer = 2 * ctx * kvd * sizeof(float);
    if (!gpu_init()) { fprintf(stderr, "GPU: nenhuma disponivel via OpenCL, usando CPU\n"); return; }
    const uint64_t vram_budget = gpu_budget();

    size_t maxc = d > H ? d : H, maxr = V > maxc ? V : maxc;
    int32_t e1 = 0, e2 = 0, e4 = 0;
    gpu.xbuf = cl.CreateBuffer(gpu.ctx, 1, maxc * sizeof(float), NULL, &e1);
    gpu.obuf = cl.CreateBuffer(gpu.ctx, 1, (maxr + 128) * sizeof(float), NULL, &e2);
    gpu.seg_target = getenv("KATANA_SEG") ? (size_t)atoll(getenv("KATANA_SEG")) : 16384;   /* medido na RTX 3050 */
    gpu.bsx = cl.CreateBuffer(gpu.ctx, 1, (maxc / 32 + 1) * GPU_BMAX * sizeof(float), NULL, &e4);
    if (e1 || e2 || e4 || !gpu_setup_bufs(m)) { fprintf(stderr, "GPU: sem memoria para os buffers, usando CPU\n"); return; }

    int max_layers = getenv("KATANA_GPU_LAYERS") ? atoi(getenv("KATANA_GPU_LAYERS")) : c->n_layers;
    if (max_layers > c->n_layers) max_layers = c->n_layers;
    int n_gl = 0, fail = 0;
    for (int l = 0; l < max_layers; l++) {
        Layer *L = &m->layers[l];
        const size_t need = gpu_mat_bytes(&L->wq, d, d) + gpu_mat_bytes(&L->wk, kvd, d) + gpu_mat_bytes(&L->wv, kvd, d)
                          + gpu_mat_bytes(&L->wo, d, d) + gpu_mat_bytes(&L->w_gate, H, d) + gpu_mat_bytes(&L->w_up, H, d)
                          + gpu_mat_bytes(&L->w_down, d, H) + kv_layer;
        if (gpu.used + need > vram_budget || (size_t)(l + 1) * ctx * kvd > 0x7fffffff) break;
        gpu_upload(&L->wq, d, d);      gpu_upload(&L->wk, kvd, d);
        gpu_upload(&L->wv, kvd, d);    gpu_upload(&L->wo, d, d);
        gpu_upload_glu(&L->w_gate, &L->w_up, H, d);
        gpu_upload(&L->w_down, d, H);
        if (!L->wq.gbuf || !L->wk.gbuf || !L->wv.gbuf || !L->wo.gbuf || !L->w_gate.gbuf || !L->w_up.gbuf || !L->w_down.gbuf) break;
        gpu.an[l] = gbuf_f(L->attn_norm, d, &fail);  gpu.fn[l] = gbuf_f(L->ffn_norm, d, &fail);
        gpu.lbq[l] = gbuf_f(L->bq, d, &fail);        gpu.lbk[l] = gbuf_f(L->bk, kvd, &fail);
        gpu.lbv[l] = gbuf_f(L->bv, kvd, &fail);      gpu.lbo[l] = gbuf_f(L->bo, d, &fail);
        if (fail) break;
        gpu.used += kv_layer;       /* reserva o cache KV desta camada */
        n_gl++;
    }
    if (n_gl == c->n_layers && gpu.used + gpu_mat_bytes(&m->lm_head, V, d) <= vram_budget) gpu_upload(&m->lm_head, V, d);
    if (n_gl > 0) {
        int32_t e5 = 0, e6 = 0;
        gpu.bkc = cl.CreateBuffer(gpu.ctx, 1, (size_t)n_gl * ctx * kvd * sizeof(float), NULL, &e5);
        gpu.bvc = cl.CreateBuffer(gpu.ctx, 1, (size_t)n_gl * ctx * kvd * sizeof(float), NULL, &e6);
        if (e5 || e6) n_gl = 0;
    }
    gpu.n_gl = n_gl;
    gpu.on = n_gl > 0;
    gpu.full = n_gl == c->n_layers && m->lm_head.gbuf != NULL;
    cl.Finish(gpu.queue);
    fprintf(stderr, "GPU: %s (%.1f GB) - %d de %d camadas na VRAM, %.0f MB usados - %s\n",
            gpu.name, gpu.vram / 1073741824.0, n_gl, c->n_layers, gpu.used / 1048576.0,
            gpu.full ? "grafo completo na GPU" : n_gl ? "o resto roda na CPU" : "usando CPU");
}

/* ---- pequenos atalhos para montar chamadas de kernel ---- */
static void ka(clh k, uint32_t i, size_t sz, const void *v) { int32_t e = cl.SetKernelArg(k, i, sz, v); if (e && !gerr) fprintf(stderr, "GPU: SetKernelArg(kernel=%p, arg=%u) erro %d\n", k, i, e); gerr |= e; }
static void ka_m(clh k, uint32_t i, clh b) { ka(k, i, sizeof(clh), &b); }
static void ka_i(clh k, uint32_t i, int32_t v) { ka(k, i, sizeof v, &v); }
static void ka_f(clh k, uint32_t i, float v) { ka(k, i, sizeof v, &v); }

static void krun(clh k, size_t n) {
    double t0 = gpu.debug ? now_sec() : 0.0;
    int32_t e = cl.EnqueueNDRangeKernel(gpu.queue, k, 1, NULL, &n, NULL, 0, NULL, NULL);
    if (e && !gerr) fprintf(stderr, "GPU: enqueue(kernel=%p, n=%zu) erro %d\n", k, n, e);
    gerr |= e;
    if (gpu.debug && !gerr) {
        e = cl.Finish(gpu.queue);
        if (e) fprintf(stderr, "GPU: kernel=%p n=%zu falhou na execucao, erro %d\n", k, n, e);
        gerr |= e;
        for (int i = 0; i < n_prof; i++) if (prof[i].k == k) {
            prof[i].t += now_sec() - t0; prof[i].calls++; prof[i].elems += prof_elems;
        }
    }
    prof_elems = 0.0;
}

/* NDRange 2D: n0 linhas (multiplo de 32) x n1 segmentos */
static void krun2(clh k, size_t n0, size_t n1) {
    double t0 = gpu.debug ? now_sec() : 0.0;
    size_t gs[2] = { n0, n1 }, ls[2] = { 32, n1 };     /* com segmentos: grupo = 32 linhas x nseg */
    int32_t e = cl.EnqueueNDRangeKernel(gpu.queue, k, 2, NULL, gs, n1 > 1 ? ls : NULL, 0, NULL, NULL);
    if (e && !gerr) fprintf(stderr, "GPU: enqueue2(kernel=%p, n=%zux%zu) erro %d\n", k, n0, n1, e);
    gerr |= e;
    if (gpu.debug && !gerr) {
        e = cl.Finish(gpu.queue);
        if (e) fprintf(stderr, "GPU: kernel=%p n=%zux%zu falhou na execucao, erro %d\n", k, n0, n1, e);
        gerr |= e;
        for (int i = 0; i < n_prof; i++) if (prof[i].k == k) {
            prof[i].t += now_sec() - t0; prof[i].calls++; prof[i].elems += prof_elems;
        }
    }
    prof_elems = 0.0;
}

static void prof_report(void) {
    double tot = 0.0;
    if (!gpu.debug) return;
    for (int i = 0; i < n_prof; i++) tot += prof[i].t;
    fprintf(stderr, "perfil GPU (com sincronizacao por kernel; total %.1f ms):\n", tot * 1e3);
    for (int i = 0; i < n_prof; i++) if (prof[i].calls)
        fprintf(stderr, "  %-12s %7ld chamadas %9.1f ms %5.1f%%  %7.1f us/chamada  %6.2f G pesos/s\n",
                prof[i].name, prof[i].calls, prof[i].t * 1e3, 100.0 * prof[i].t / (tot + 1e-12),
                prof[i].t * 1e6 / prof[i].calls, prof[i].elems / (prof[i].t + 1e-12) / 1e9);
}

/* enfileira out = W * x na GPU (modo: 0 grava, 1 soma em out, 2 SwiGLU com o que ja esta em out).
   Matrizes com poucas linhas e muitas colunas sao divididas em segmentos de colunas para
   ocupar a GPU inteira. Em lote (ks->nb > 1), matrizes Q4_K exigem g_bsum(x) antes. */
static void gmv(const KSet *ks, const Mat *W, clh x, clh out, clh bias, int rows, int cols, int mode) {
    clh k = !W->q ? ks->k_f32 : W->type == T_Q4_K ? ks->k_q4k : W->type == T_Q6_K ? ks->k_q6k : ks->k_q8;
    const size_t rowsP = ((size_t)rows + 127) & ~(size_t)127, threads = rowsP / ks->nr;
    const int nb = !W->q ? 1 : W->type == T_Q8_0 ? cols / 32 : cols / 256;
    const int nseg = gpu_nseg(threads, nb, ks->nb * ks->nr);
    if (bias) mode |= 4;
    ka_m(k, 0, W->gbuf); ka_m(k, 1, x); ka_m(k, 2, out);
    ka_m(k, 3, bias ? bias : x); ka_m(k, 4, gpu.bsx ? gpu.bsx : x);
    ka_i(k, 5, cols); ka_i(k, 6, mode); ka_i(k, 7, rows); ka_i(k, 8, nseg);
    ka_i(k, 9, W->gs); ka_i(k, 10, W->go);
    prof_elems = (double)rows * cols * ks->nb;
    krun2(k, threads, (size_t)nseg);
}

/* out = silu(gate * x) * (up * x) num kernel so, sobre o buffer com gate e up intercalados */
static void gmv_glu(const KSet *ks, const Mat *gate, clh x, clh out, int rows, int cols) {
    clh k = !gate->q ? ks->kp_f32 : gate->type == T_Q4_K ? ks->kp_q4k : gate->type == T_Q6_K ? ks->kp_q6k : ks->kp_q8;
    const size_t rowsP = ((size_t)rows + 127) & ~(size_t)127, threads = 2 * rowsP / (2 * ks->nr);
    const int nb = !gate->q ? 1 : gate->type == T_Q8_0 ? cols / 32 : cols / 256;
    const int nseg = gpu_nseg(threads, nb, ks->nb * ks->nr * 2);
    ka_m(k, 0, gate->gbuf); ka_m(k, 1, x); ka_m(k, 2, out);
    ka_m(k, 3, x); ka_m(k, 4, gpu.bsx ? gpu.bsx : x);
    ka_i(k, 5, cols); ka_i(k, 6, 8); ka_i(k, 7, rows); ka_i(k, 8, nseg);
    ka_i(k, 9, 1); ka_i(k, 10, 0);
    prof_elems = 2.0 * rows * cols * ks->nb;
    krun2(k, threads, (size_t)nseg);
}

/* modo hibrido: um produto na GPU, com ida e volta */
static void gpu_matvec(float *out, const Mat *W, const float *x, int rows, int cols) {
    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gpu.xbuf, 0, 0, cols * sizeof(float), x, 0, NULL, NULL);
    gmv(&gpu.set[gpu.n_sets - 1], W, gpu.xbuf, gpu.obuf, NULL, rows, cols, 0);
    gerr |= cl.EnqueueReadBuffer(gpu.queue, gpu.obuf, 1 /* bloqueante */, 0, (size_t)rows * sizeof(float), out, 0, NULL, NULL);
    if (gerr) die("falha na execucao em GPU");
}

/* perfil da CPU (KATANA_DEBUG=1): tempo e pesos por tipo de produto, e preparo das ativacoes */
static struct { double t, w; long n; } cprof[5];
static const char *cprof_name[5] = { "float32", "Q8 float", "Q4_K", "Q6_K", "quantizar x" };

static void cprof_report(void) {
    double tot = 0.0;
    if (!gpu.debug) return;
    for (int i = 0; i < 5; i++) tot += cprof[i].t;
    if (tot == 0.0) return;
    fprintf(stderr, "perfil CPU (produtos; total %.1f ms):\n", tot * 1e3);
    for (int i = 0; i < 5; i++) if (cprof[i].n)
        fprintf(stderr, "  %-12s %7ld chamadas %9.1f ms %5.1f%%  %6.2f G pesos/s\n", cprof_name[i], cprof[i].n,
                cprof[i].t * 1e3, 100.0 * cprof[i].t / tot, cprof[i].w / (cprof[i].t + 1e-12) / 1e9);
}

/* out[rows] = W[rows][cols] * x[cols] */
static void matvec(float *out, const Mat *W, const float *x, int rows, int cols) {
    float sx[MAX_SX];
    int i;
    if (W->gbuf && gpu.test_mv) { gpu_matvec(out, W, x, rows, cols); return; }
    const double t0 = gpu.debug ? now_sec() : 0.0;
    if (mat_int(W)) {
        xq_prepare(x, cols);
        if (gpu.debug) { cprof[4].t += now_sec() - t0; cprof[4].n++; }
    }
    const double t1 = gpu.debug ? now_sec() : 0.0;
    if (mat_int(W)) ;
    else if (W->q && W->type == T_Q4_K) {
        if (cols / 32 > MAX_SX) die("matriz larga demais");
        block_sums(sx, x, cols);
    }
#pragma omp parallel for schedule(static) if ((long long)rows * cols >= 200000)
    for (i = 0; i < rows; i++) out[i] = dot_row(W, (size_t)i, x, sx, cols);
    if (gpu.debug) {
        const int k = !W->q ? 0 : W->type == T_Q4_K ? 2 : W->type == T_Q6_K ? 3 : 1;
        cprof[k].t += now_sec() - t1; cprof[k].w += (double)rows * cols; cprof[k].n++;
    }
}

/* copia a linha `row` de uma matriz para out (usado no embedding) */
static void mat_row(float *out, const Mat *W, int row, int cols) {
    if (W->f) memcpy(out, W->f + (size_t)row * cols, cols * sizeof(float));
    else dequant(W->type, W->q + (size_t)row * W->stride, out, cols);
}

static void rmsnorm(float *out, const float *x, const float *g, int n, float eps) {
    float inv = 1.0f / sqrtf(dot(x, x, n) / n + eps);
    for (int i = 0; i < n; i++) out[i] = g[i] * (x[i] * inv);
}

static void softmax(float *x, int n) {
    float mx = x[0], sum = 0.0f;
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    for (int i = 0; i < n; i++) { x[i] = expf(x[i] - mx); sum += x[i]; }
    for (int i = 0; i < n; i++) x[i] /= sum;
}

/* ================= forward com o grafo inteiro na GPU ================= */

static void g_rms(const KSet *ks, clh x, clh g, clh out, int n, float eps) {
    clh k = ks->k_rms;
    ka_m(k, 0, x); ka_m(k, 1, g); ka_m(k, 2, out); ka_i(k, 3, n); ka_f(k, 4, eps);
    krun(k, (size_t)n);
}

/* somas de x por sub-bloco, so quando ha lote e alguma das matrizes consumidoras e Q4_K */
static void g_bsum(const KSet *ks, clh x, int cols, const Mat *a, const Mat *b, const Mat *c3) {
    const Mat *ms[3] = { a, b, c3 };
    int need = 0;
    if (ks->nb == 1) return;
    for (int i = 0; i < 3; i++) if (ms[i] && ms[i]->q && ms[i]->type == T_Q4_K) need = 1;
    if (!need) return;
    ka_m(ks->k_bsum, 0, x); ka_m(ks->k_bsum, 1, gpu.bsx);
    krun(ks->k_bsum, (size_t)cols / 32);
}

static void cpu_layers(Model *m, int l0, int l1, int pos);
static float *cpu_head(Model *m);

/* processa ks->nb tokens de uma vez (posicoes pos0, pos0+1, ...); devolve os logits do ultimo */
static float *gpu_forward_n(Model *m, const KSet *ks, const int *toks, int pos0, int want_logits) {
    const Config *c = &m->cfg;
    const int B = ks->nb, d = c->dim, hd = c->hd, H = c->hidden, ctx = c->ctx, nh = c->n_heads;
    const int kvd = c->n_kv * hd, group = nh / c->n_kv, tmax = pos0 + B;
    const float scale = 1.0f / sqrtf((float)hd);
    const KSet *k1 = &gpu.set[gpu.n_sets - 1];

    for (int b = 0; b < B; b++) {       /* embeddings transpostos: xT[coluna*B + token] */
        mat_row(m->x, &m->tok_emb, toks[b], d);
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
        gmv(ks, &L->wq, gpu.bxb, gpu.bq, NULL, d, d, 0);
        gmv(ks, &L->wk, gpu.bxb, gpu.bk, NULL, kvd, d, 0);
        gmv(ks, &L->wv, gpu.bxb, gpu.bv, NULL, kvd, d, 0);

        k = ks->k_qkv;      /* bias + RoPE em q (no lugar) e k (no cache), v copiado para o cache */
        ka_m(k, 0, gpu.bq); ka_m(k, 1, gpu.bk); ka_m(k, 2, gpu.bv);
        ka_m(k, 3, gpu.lbq[l] ? gpu.lbq[l] : gpu.bq); ka_m(k, 4, gpu.lbk[l] ? gpu.lbk[l] : gpu.bk);
        ka_m(k, 5, gpu.lbv[l] ? gpu.lbv[l] : gpu.bv);
        ka_m(k, 6, gpu.bkc); ka_m(k, 7, gpu.bvc); ka_m(k, 8, gpu.bfreq);
        ka_i(k, 9, base); ka_i(k, 10, pos0); ka_i(k, 11, hd); ka_i(k, 12, c->rope_neox);
        ka_i(k, 13, (gpu.lbq[l] ? 1 : 0) | (gpu.lbk[l] ? 2 : 0) | (gpu.lbv[l] ? 4 : 0));
        ka_i(k, 14, d); ka_i(k, 15, kvd);
        krun(k, (size_t)B * (d / 2 + kvd / 2 + kvd));

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
        krun(k, (size_t)B * d);
        g_bsum(ks, gpu.bxb, d, &L->wo, NULL, NULL);
        gmv(ks, &L->wo, gpu.bxb, gpu.bx, gpu.lbo[l], d, d, 1);           /* x += Wo*att (+bias) */

        /* --- feed-forward SwiGLU --- */
        g_rms(ks, gpu.bx, gpu.fn[l], gpu.bxb, d, c->eps);
        g_bsum(ks, gpu.bxb, d, &L->w_gate, &L->w_up, NULL);
        if (L->w_gate.pair) gmv_glu(ks, &L->w_gate, gpu.bxb, gpu.bhb, H, d);    /* hb = silu(gate) * up */
        else {
            gmv(ks, &L->w_gate, gpu.bxb, gpu.bhb, NULL, H, d, 0);        /* hb = gate */
            gmv(ks, &L->w_up, gpu.bxb, gpu.bhb, NULL, H, d, 2);          /* hb = silu(hb) * up */
        }
        g_bsum(ks, gpu.bhb, H, &L->w_down, NULL, NULL);
        gmv(ks, &L->w_down, gpu.bhb, gpu.bx, NULL, d, H, 1);             /* x += down */
    }

    if (gpu.n_gl < c->n_layers) {   /* o estado desce uma vez e a CPU faz as camadas restantes */
        float *lg = NULL;
        gerr |= cl.EnqueueReadBuffer(gpu.queue, gpu.bx, 1, 0, (size_t)d * B * sizeof(float), gpu.xT, 0, NULL, NULL);
        if (gerr) die("falha na execucao em GPU");
        for (int b = 0; b < B; b++) {
            for (int i = 0; i < d; i++) m->x[i] = gpu.xT[(size_t)i * B + b];
            cpu_layers(m, gpu.n_gl, c->n_layers, pos0 + b);
            if (want_logits && b == B - 1) lg = cpu_head(m);
        }
        return lg;
    }
    if (!want_logits) {     /* nada a ler: o trabalho fica na fila e a GPU segue em paralelo */
        if (gerr) die("falha na execucao em GPU");
        return NULL;
    }
    g_rms(ks, gpu.bx, gpu.bon, gpu.bxb, d, c->eps);
    if (B > 1) {            /* so o ultimo token do lote precisa de logits */
        clh k = ks->k_gather;
        ka_m(k, 0, gpu.bxb); ka_m(k, 1, gpu.bq); ka_i(k, 2, B); ka_i(k, 3, B - 1);
        krun(k, (size_t)d);
    }
    gmv(k1, &m->lm_head, B > 1 ? gpu.bq : gpu.bxb, gpu.blog, NULL, c->vocab, d, 0);
    gerr |= cl.EnqueueReadBuffer(gpu.queue, gpu.blog, 1, 0, (size_t)c->vocab * sizeof(float), m->logits, 0, NULL, NULL);
    if (gerr) die("falha na execucao em GPU");
    return m->logits;
}

/* ================= forward (um token por vez, com KV cache) ================= */

/* camadas [l0, l1) na CPU sobre o estado m->x, para o token na posicao pos */
static void cpu_layers(Model *m, int l0, int l1, int pos) {
    const Config *c = &m->cfg;
    const int d = c->dim, hd = c->hd, H = c->hidden, ctx = c->ctx;
    const int kvd = c->n_kv * hd, group = c->n_heads / c->n_kv;
    const float scale = 1.0f / sqrtf((float)hd);
    const int step = c->rope_neox ? 1 : 2, gap = c->rope_neox ? hd / 2 : 1;

    for (int l = l0; l < l1; l++) {
        const Layer *L = &m->layers[l];

        /* --- atencao (GQA + RoPE) --- */
        rmsnorm(m->xb, m->x, L->attn_norm, d, c->eps);
        matvec(m->q, &L->wq, m->xb, d, d);
        matvec(m->k, &L->wk, m->xb, kvd, d);
        matvec(m->v, &L->wv, m->xb, kvd, d);
        if (L->bq) for (int i = 0; i < d; i++) m->q[i] += L->bq[i];
        if (L->bk) for (int i = 0; i < kvd; i++) m->k[i] += L->bk[i];
        if (L->bv) for (int i = 0; i < kvd; i++) m->v[i] += L->bv[i];

        /* RoPE: gira pares (2i, 2i+1) no llama, ou (i, i+hd/2) no qwen2 */
        for (int i = 0; i < hd / 2; i++) {
            float ang = pos * m->rope_freq[i], cs = cosf(ang), sn = sinf(ang);
            for (int h = 0; h < c->n_heads; h++) {
                float *p = m->q + h * hd + step * i, a = p[0], b = p[gap];
                p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs;
            }
            for (int h = 0; h < c->n_kv; h++) {
                float *p = m->k + h * hd + step * i, a = p[0], b = p[gap];
                p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs;
            }
        }

        float *kbase = m->kcache + (size_t)l * ctx * kvd;
        float *vbase = m->vcache + (size_t)l * ctx * kvd;
        memcpy(kbase + (size_t)pos * kvd, m->k, kvd * sizeof(float));
        memcpy(vbase + (size_t)pos * kvd, m->v, kvd * sizeof(float));

        for (int h = 0; h < c->n_heads; h++) {
            const float *qh = m->q + h * hd;
            const int kvh = (h / group) * hd;
            for (int t = 0; t <= pos; t++)
                m->att[t] = dot(qh, kbase + (size_t)t * kvd + kvh, hd) * scale;
            softmax(m->att, pos + 1);

            float *out = m->xb + h * hd;
            memset(out, 0, hd * sizeof(float));
            for (int t = 0; t <= pos; t++) {
                const float *vh = vbase + (size_t)t * kvd + kvh;
                const float a = m->att[t];
                for (int i = 0; i < hd; i++) out[i] += a * vh[i];
            }
        }
        matvec(m->xb2, &L->wo, m->xb, d, d);
        if (L->bo) for (int i = 0; i < d; i++) m->xb2[i] += L->bo[i];
        for (int i = 0; i < d; i++) m->x[i] += m->xb2[i];

        /* --- feed-forward SwiGLU: down(silu(gate(x)) * up(x)) --- */
        rmsnorm(m->xb, m->x, L->ffn_norm, d, c->eps);
        matvec(m->hb, &L->w_gate, m->xb, H, d);
        matvec(m->hb2, &L->w_up, m->xb, H, d);
        for (int i = 0; i < H; i++) {
            float g = m->hb[i];
            m->hb[i] = g / (1.0f + expf(-g)) * m->hb2[i];
        }
        matvec(m->xb2, &L->w_down, m->hb, d, H);
        for (int i = 0; i < d; i++) m->x[i] += m->xb2[i];
    }
}

/* norma final + projecao para o vocabulario, na CPU */
static float *cpu_head(Model *m) {
    const Config *c = &m->cfg;
    rmsnorm(m->x, m->x, m->out_norm, c->dim, c->eps);
    matvec(m->logits, &m->lm_head, m->x, c->vocab, c->dim);
    return m->logits;
}

/* as camadas da GPU so sao usadas pelo grafo da GPU; na CPU os produtos sao sempre locais */
static float *forward(Model *m, int token, int pos, int want_logits) {
    const Config *c = &m->cfg;
    if (gpu.n_gl > 0 && !gpu.force_cpu) return gpu_forward_n(m, &gpu.set[gpu.n_sets - 1], &token, pos, want_logits);
    mat_row(m->x, &m->tok_emb, token, c->dim);
    cpu_layers(m, 0, c->n_layers, pos);
    return want_logits ? cpu_head(m) : NULL;
}

/* processa n tokens seguidos a partir de pos0 (em lotes, na GPU); logits do ultimo se pedido */
static float *forward_seq(Model *m, const int *toks, int n, int pos0, int want_logits) {
    float *lg = NULL;
    int i = 0;
    if (gpu.n_gl > 0 && !gpu.force_cpu) {
        while (i < n) {
            const KSet *ks = &gpu.set[gpu.n_sets - 1];
            for (int s = 0; s < gpu.n_sets; s++) if (gpu.set[s].nb <= n - i) { ks = &gpu.set[s]; break; }
            lg = gpu_forward_n(m, ks, toks + i, pos0 + i, want_logits && i + ks->nb == n);
            i += ks->nb;
        }
        return lg;
    }
    for (; i < n; i++) lg = forward(m, toks[i], pos0 + i, want_logits && i == n - 1);
    return lg;
}

/* ================= amostragem (temperatura + top-k) ================= */

static int sample(const float *logits, int n, float temp) {
    int idx[TOP_K]; float val[TOP_K]; int k = 0;
    if (temp <= 0.0f) {
        int best = 0;
        for (int i = 1; i < n; i++) if (logits[i] > logits[best]) best = i;
        return best;
    }
    for (int i = 0; i < n; i++) {   /* mantem os TOP_K maiores, em ordem decrescente */
        if (k == TOP_K && logits[i] <= val[k - 1]) continue;
        int j = k < TOP_K ? k++ : k - 1;
        while (j > 0 && val[j - 1] < logits[i]) { val[j] = val[j - 1]; idx[j] = idx[j - 1]; j--; }
        val[j] = logits[i]; idx[j] = i;
    }
    for (int i = 0; i < k; i++) val[i] /= temp;
    softmax(val, k);
    float r = (float)rand() / ((float)RAND_MAX + 1.0f), acc = 0.0f;
    for (int i = 0; i < k; i++) { acc += val[i]; if (r < acc) return idx[i]; }
    return idx[k - 1];
}

/* ================= comandos ================= */


static void print_info(const Model *m) {
    const Config *c = &m->cfg;
    fprintf(stderr, "arquitetura=%s  parametros=%.1fM  camadas=%d  dim=%d  cabecas=%d (kv=%d)  ffn=%d  vocab=%d  ctx=%d\n",
            c->arch, m->n_params / 1e6, c->n_layers, c->dim, c->n_heads, c->n_kv, c->hidden, c->vocab, c->ctx);
}

static int generate(Model *m, const char *text, int max_tokens, float temp, int echo) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    Ids ids = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    encode(tk, text, &ids);
    if (ids.n == 0) ids_push(&ids, c->bos >= 0 ? c->bos : 0);
    if (ids.n >= c->ctx) die("prompt maior que o contexto");

    srand((unsigned)time(NULL));
    float *lg = NULL;
    int pos = 0, gen = 0;
    double t0 = now_sec();
    lg = forward_seq(m, ids.ids, ids.n, 0, 1);
    pos = ids.n;
    double t1 = now_sec();
    if (echo) { fputs(text, stdout); fflush(stdout); }

    while (gen < max_tokens && pos < c->ctx) {
        int next = sample(lg, c->vocab, temp);
        if (next == c->eos || (tk->toktype && tk->toktype[next] == 3)) break;
        print_token(tk, next, stdout);
        fflush(stdout);
        gen++;
        lg = forward(m, next, pos++, 1);
    }
    double t2 = now_sec();
    printf("\n");
    fflush(stdout);
    prof_report();
    fprintf(stderr, "[prompt: %d tokens, %.1f tok/s | geracao: %d tokens, %.1f tok/s]\n",
            ids.n, ids.n / (t1 - t0 + 1e-9), gen, gen / (t2 - t1 + 1e-9));
    free(ids.ids);
    return 0;
}

/* confere o produto quantizado nativo contra dequantizar + produto em float */
static double worst_int;    /* pior erro do produto inteiro da CPU (quantizacao de 8 bits das ativacoes) */

static void selftest_mat(const char *name, const Mat *W, int rows, int cols, double *worst) {
    if (!W->q) { printf("  %-10s float32 (sem caminho quantizado)\n", name); return; }
    float *x = xalloc(cols, sizeof(float)), *row = xalloc(cols, sizeof(float)), *sx = xalloc(cols / 32 + 1, sizeof(float));
    double maxerr = 0.0, interr = 0.0;
    for (int i = 0; i < cols; i++) x[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    block_sums(sx, x, cols);
    int n = rows < 64 ? rows : 64;
    for (int r = 0; r < n; r++) {
        size_t i = (size_t)r * (rows / n);
        mat_row(row, W, (int)i, cols);
        double ref = 0.0, mag = 0.0;
        for (int j = 0; j < cols; j++) { ref += (double)row[j] * x[j]; mag += fabs((double)row[j] * x[j]); }
        cpu_exact = 1;
        double err = fabs(dot_row(W, i, x, sx, cols) - ref) / (mag + 1e-30);
        cpu_exact = 0;
        if (err > maxerr) maxerr = err;
        if (mat_int(W)) {
            xq_prepare(x, cols);
            double ei = fabs(dot_row(W, i, x, sx, cols) - ref) / (mag + 1e-30);
            if (ei > interr) interr = ei;
        }
    }
    printf("  %-10s tipo=%-2u linhas=%-6d colunas=%-5d erro relativo max=%.2e", name, W->type, rows, cols, maxerr);
    if (maxerr > *worst) *worst = maxerr;
    if (mat_int(W)) { printf("  | CPU inteiro=%.2e", interr); if (interr > worst_int) worst_int = interr; }
    if (W->gbuf) {   /* resultado inteiro da GPU contra a CPU, linha a linha */
        float *og = xalloc(rows, sizeof(float));
        double gerr = 0.0;
        gpu.test_mv = 1; matvec(og, W, x, rows, cols); gpu.test_mv = 0;
        for (int r = 0; r < rows; r++) {
            mat_row(row, W, r, cols);
            double ref = 0.0, mag = 0.0;
            for (int j = 0; j < cols; j++) { ref += (double)row[j] * x[j]; mag += fabs((double)row[j] * x[j]); }
            double err = fabs(og[r] - ref) / (mag + 1e-30);
            if (err > gerr) gerr = err;
        }
        printf("  | GPU=%.2e", gerr);
        if (gerr > *worst) *worst = gerr;
        free(og);
    }
    printf("\n");
    free(x); free(row); free(sx);
}

static int selftest(Model *m) {
    const Config *c = &m->cfg;
    const int d = c->dim, kvd = c->n_kv * c->hd, H = c->hidden;
    double worst = 0.0;
    srand(1234);
    selftest_mat("tok_emb", &m->tok_emb, c->vocab, d, &worst);
    for (int l = 0; l < c->n_layers; l += c->n_layers - 1 > 0 ? c->n_layers - 1 : 1) {
        const Layer *L = &m->layers[l];
        printf(" camada %d\n", l);
        selftest_mat("attn_q", &L->wq, d, d, &worst);
        selftest_mat("attn_k", &L->wk, kvd, d, &worst);
        selftest_mat("attn_v", &L->wv, kvd, d, &worst);
        selftest_mat("attn_out", &L->wo, d, d, &worst);
        selftest_mat("ffn_gate", &L->w_gate, H, d, &worst);
        selftest_mat("ffn_up", &L->w_up, H, d, &worst);
        selftest_mat("ffn_down", &L->w_down, d, H, &worst);
    }
    int ok = worst < 1e-4 && worst_int < 1e-4;
    printf("produtos: %s (pior erro relativo: exato %.2e, CPU inteiro %.2e)\n", ok ? "OK" : "FALHOU", worst, worst_int);

    if (gpu.n_gl > 0) {   /* mesma sequencia pela GPU (em lote e token a token) e pela CPU pura */
        const int V = c->vocab, N = 23;
        int toks[23];
        float *ref = xalloc(V, sizeof(float)), *o = NULL;
        for (int i = 0; i < N; i++) toks[i] = (int)((1000003u * (i + 1)) % (unsigned)V);
        gpu.force_cpu = 1; cpu_exact = 1;
        for (int i = 0; i < N; i++) o = forward(m, toks[i], i, i == N - 1);
        gpu.force_cpu = 0; cpu_exact = 0;
        memcpy(ref, o, V * sizeof(float));
        for (int mode = 0; mode < 3; mode++) {
            if (mode == 0) o = forward_seq(m, toks, N, 0, 1);
            else if (mode == 1) for (int i = 0; i < N; i++) o = forward(m, toks[i], i, i == N - 1);
            else { gpu.force_cpu = 1; for (int i = 0; i < N; i++) o = forward(m, toks[i], i, i == N - 1); gpu.force_cpu = 0; }
            int ag = 0, ac = 0; double md = 0.0;
            for (int i = 0; i < V; i++) {
                if (o[i] > o[ag]) ag = i;
                if (ref[i] > ref[ac]) ac = i;
                if (fabs(o[i] - ref[i]) > md) md = fabs(o[i] - ref[i]);
            }
            int e2e = ag == ac && md < (mode == 2 || gpu.n_gl < c->n_layers ? 1.0 : 0.05);
            printf("%s x CPU exata (%d tokens): maior diferenca de logit=%.6f, argmax %d x %d: %s\n",
                   mode == 0 ? "GPU em lote      " : mode == 1 ? "GPU token a token" : "CPU inteira      ", N, md, ag, ac, e2e ? "OK" : "FALHOU");
            ok = ok && e2e;
        }
        free(ref);
    }
    return ok ? 0 : 1;
}

/* mede prompt e geracao com tokens sinteticos, depois de aquecer (pesos ja na VRAM) */
static int bench(Model *m, int n_prompt, int n_gen) {
    const Config *c = &m->cfg;
    if (n_prompt < 1) n_prompt = 1;
    if (n_prompt + n_gen >= c->ctx) { n_prompt = c->ctx / 2; n_gen = c->ctx / 4; }
    int *toks = xalloc(n_prompt, sizeof(int));
    for (int i = 0; i < n_prompt; i++) toks[i] = (int)((1000003u * (i + 1)) % (unsigned)c->vocab);

    forward_seq(m, toks, n_prompt < 32 ? n_prompt : 32, 0, 1);      /* aquecimento */
    for (int r = 0; r < 3; r++) {
        double t0 = now_sec();
        float *lg = forward_seq(m, toks, n_prompt, 0, 1);
        double t1 = now_sec();
        int pos = n_prompt;
        for (int g = 0; g < n_gen; g++) lg = forward(m, sample(lg, c->vocab, 0.0f), pos++, 1);
        double t2 = now_sec();
        printf("rodada %d: prompt %d tokens %.1f tok/s | geracao %d tokens %.1f tok/s (%.2f ms/token)\n",
               r + 1, n_prompt, n_prompt / (t1 - t0), n_gen, n_gen / (t2 - t1), (t2 - t1) * 1e3 / n_gen);
    }
    prof_report();
    cprof_report();
    free(toks);
    return 0;
}

/* microbenchmark da CPU numa matriz: a mesma linha repetida (so calculo, dados no cache L1)
   contra todas as linhas em sequencia (calculo + leitura da memoria), em uma thread */
static void micro_mat(const char *name, const Mat *W, int rows, int cols) {
    if (!W->q) return;
    float *x = xalloc(cols, sizeof(float)), *sx = xalloc(cols / 32 + 1, sizeof(float));
    for (int i = 0; i < cols; i++) x[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    block_sums(sx, x, cols);
    for (int exact = 1; exact >= 0; exact--) {
        cpu_exact = exact;
        if (!exact && !mat_int(W)) break;
        if (mat_int(W)) xq_prepare(x, cols);
        volatile float sink = 0.0f;
        const int reps = (int)(400000000.0 / cols) + 1;
        double t0 = now_sec();
        for (int r = 0; r < reps; r++) sink += dot_row(W, 0, x, sx, cols);
        double t1 = now_sec();
        int passes = (int)(400000000.0 / ((double)rows * cols)) + 1;
        for (int ps = 0; ps < passes; ps++)
            for (int i = 0; i < rows; i++) sink += dot_row(W, (size_t)i, x, sx, cols);
        double t2 = now_sec();
        printf("  %-9s tipo=%-2u %-8s  mesma linha: %6.2f G pesos/s | matriz inteira: %6.2f G pesos/s (%.2f GB/s lidos)\n",
               name, W->type, exact ? "float" : "inteiro", (double)reps * cols / (t1 - t0) / 1e9,
               (double)passes * rows * cols / (t2 - t1) / 1e9, (double)passes * rows * W->stride / (t2 - t1) / 1e9);
    }
    cpu_exact = 0;
    free(x); free(sx);
}

static int micro(const Model *m) {
    const Config *c = &m->cfg;
    const Layer *L = &m->layers[0];
    const int d = c->dim, H = c->hidden;
    printf("microbenchmark da CPU (1 thread):\n");
    {   /* calibracao: o que esta maquina entrega agora em contas simples, com dados no cache L1 */
        float *v = xalloc(1024, sizeof(float));
        volatile float sink = 0.0f;
        for (int i = 0; i < 1024; i++) v[i] = (float)i * 0.001f;
        double t0 = now_sec();
        for (int r = 0; r < 2000000; r++) sink += dot(v, v, 1024);
        double t1 = now_sec();
        uint64_t acc = 1;
        for (uint64_t i = 0; i < 400000000ull; i++) acc = acc * 6364136223846793005ull + 1442695040888963407ull;
        double t2 = now_sec();
        printf("  calibracao: dot float L1 = %.2f G elementos/s | cadeia de multiplicacoes = %.2f GHz equivalentes (%u)\n",
               2000000.0 * 1024 / (t1 - t0) / 1e9, 400000000.0 * 4 / (t2 - t1) / 1e9, (unsigned)(acc & 7));
        free(v);
    }
    micro_mat("attn_q", &L->wq, d, d);
    micro_mat("ffn_gate", &L->w_gate, H, d);
    micro_mat("ffn_down", &L->w_down, d, H);
    micro_mat("lm_head", &m->lm_head, c->vocab, d);
    return 0;
}

static int run_main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "micro") == 0) {
        Model m; memset(&m, 0, sizeof m);
        _putenv("KATANA_CPU=1");
        load_model(&m, argv[2]);
        return micro(&m);
    }
    gpu.debug = getenv("KATANA_DEBUG") != NULL;
    if (argc >= 3 && strcmp(argv[1], "bench") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        return bench(&m, argc > 3 ? atoi(argv[3]) : 256, argc > 4 ? atoi(argv[4]) : 64);
    }
    if (argc >= 3 && strcmp(argv[1], "selftest") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        return selftest(&m);
    }
    if (argc >= 3 && strcmp(argv[1], "info") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        return 0;
    }
    if (argc >= 4 && (strcmp(argv[1], "run") == 0 || strcmp(argv[1], "chat") == 0)) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        int max_tokens = argc > 4 ? atoi(argv[4]) : 200;
        float temp = argc > 5 ? (float)atof(argv[5]) : 0.7f;
        if (strcmp(argv[1], "run") == 0) return generate(&m, argv[3], max_tokens, temp, 1);

        size_t n = strlen(argv[3]) + 128;
        char *p = xalloc(n, 1);
        if (m.cfg.chat_fmt == 1)
            snprintf(p, n, "<|start_header_id|>user<|end_header_id|>\n\n%s<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n", argv[3]);
        else
            snprintf(p, n, "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", argv[3]);
        return generate(&m, p, max_tokens, temp, 0);
    }
    fprintf(stderr,
        "Uso:\n"
        "  katana info <modelo.gguf>\n"
        "  katana selftest <modelo.gguf>\n"
        "  katana bench <modelo.gguf> [tokens_prompt] [tokens_geracao]\n"
        "  katana run  <modelo.gguf> \"texto\" [max_tokens] [temperatura]\n"
        "  katana chat <modelo.gguf> \"pergunta\" [max_tokens] [temperatura]\n");
    return 1;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* argumentos e saida em UTF-8 no Windows */
    int wargc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        char **u8 = xalloc(wargc + 1, sizeof(char *));
        for (int i = 0; i < wargc; i++) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
            u8[i] = xalloc(n, 1);
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, u8[i], n, NULL, NULL);
        }
        argc = wargc; argv = u8;
    }
    SetConsoleOutputCP(CP_UTF8);
#endif
    return run_main(argc, argv);
}
